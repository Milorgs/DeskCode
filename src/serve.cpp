// Daemon server (`desk serve`), terminal attach client (`desk attach`), and
// native cross-platform OS notifications.
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "desk.hpp"
#include "tasks.hpp"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#ifdef _WIN32
#include "windows/win.hpp"
using SockFd=SOCKET;
constexpr SockFd BAD_SOCK=INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
using SockFd=int;
constexpr SockFd BAD_SOCK=-1;
#endif

namespace {
void close_socket(SockFd s){
    if(s==BAD_SOCK)return;
#ifdef _WIN32
    shutdown(s,SD_BOTH);
    closesocket(s);
#else
    shutdown(s,SHUT_RDWR);
    close(s);
#endif
}
bool send_all(SockFd s,std::string_view data){
    size_t off=0;
    while(off<data.size()){
#ifdef _WIN32
        int n=send(s,data.data()+off,static_cast<int>((std::min)(data.size()-off,static_cast<size_t>(65536))),0);
        if(n<=0)return false;
#else
        ssize_t n=send(s,data.data()+off,data.size()-off,
#ifdef MSG_NOSIGNAL
            MSG_NOSIGNAL
#else
            0
#endif
        );
        if(n<0&&errno==EINTR)continue;
        if(n<=0)return false;
#endif
        off+=static_cast<size_t>(n);
    }
    return true;
}
std::string clean_notify_text(const char *raw,size_t limit){
    if(!raw)return {};
    std::string out;
    for(size_t i=0;raw[i]&&out.size()<limit;i++){
        unsigned char c=static_cast<unsigned char>(raw[i]);
        if(c=='\n'||c=='\r'||c=='\t'){if(!out.empty()&&out.back()!=' ')out+=' ';}
        else if(c>=32&&c!=127)out+=static_cast<char>(c);
    }
    while(!out.empty()&&(static_cast<unsigned char>(out.back())&0xc0)==0x80)out.pop_back();
    return valid_utf8(out)?out:std::string("DeskCode");
}
bool env_on(const char *name){
    const char *v=env_value(name);
    return v&&*v&&strcmp(v,"0")&&
#ifdef _WIN32
        _stricmp(v,"false")&&_stricmp(v,"off");
#else
        strcasecmp(v,"false")&&strcasecmp(v,"off");
#endif
}
std::string json_str(J *v){
    return json_object_to_json_string_ext(v,JSON_C_TO_STRING_PLAIN);
}
} // namespace

void notify_user_event(const App &app,const char *title,const char *body){
    const std::string safe_title=clean_notify_text(title,120);
    const std::string safe_body=clean_notify_text(body,240);
    if(const char *cap=env_value("DESK_NOTIFY_CAPTURE");cap&&*cap){
        JsonPtr entry(obj());
        put(entry.get(),"title",str(safe_title.c_str()));
        put(entry.get(),"body",str(safe_body.c_str()));
        std::string line=json_str(entry.get())+"\n";
#ifdef _WIN32
        FILE *fp=_wfopen(wide(cap).c_str(),L"ab");
#else
        FILE *fp=fopen(cap,"ab");
#endif
        if(fp){fwrite(line.data(),1,line.size(),fp);fclose(fp);}
        return;
    }
    if(!app.notify&&!env_on("DESK_NOTIFY"))return;
#ifdef _WIN32
    const char *sysroot=env_value("SystemRoot");
    if(!sysroot)return;
    std::wstring exe=wide(sysroot)+L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
    auto ps_esc=[](const std::string &s){
        std::string r;for(char c:s){if(c=='\'')r+="''";else r+=c;}return r;
    };
    std::string script="Add-Type -AssemblyName System.Windows.Forms; "
        "$n=New-Object System.Windows.Forms.NotifyIcon; "
        "$n.Icon=[System.Drawing.SystemIcons]::Information; $n.Visible=$true; "
        "$n.ShowBalloonTip(4000,'"+ps_esc(safe_title)+"','"+ps_esc(safe_body)+"',[System.Windows.Forms.ToolTipIcon]::Info); "
        "Start-Sleep -Milliseconds 1500; $n.Dispose()";
    std::wstring wscript=wide(script);
    std::string b64(((wscript.size()*sizeof(wchar_t)+2)/3)*4,'\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(b64.data()),reinterpret_cast<const unsigned char *>(wscript.data()),static_cast<int>(wscript.size()*sizeof(wchar_t)));
    std::wstring cmd=quote_arg(exe)+L" -NoLogo -NoProfile -NonInteractive -WindowStyle Hidden -EncodedCommand "+wide(b64);
    STARTUPINFOW si{};si.cb=sizeof si;PROCESS_INFORMATION pi{};
    if(CreateProcessW(exe.c_str(),cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);
    }
#else
    pid_t pid=fork();
    if(pid==0){
        pid_t grand=fork();
        if(grand==0){
            int nullfd=open("/dev/null",O_RDWR|O_CLOEXEC);
            if(nullfd>=0){dup2(nullfd,0);dup2(nullfd,1);dup2(nullfd,2);if(nullfd>2)close(nullfd);}
#ifdef __APPLE__
            execlp("osascript","osascript",
                   "-e","on run argv",
                   "-e","display notification (item 2 of argv) with title (item 1 of argv)",
                   "-e","end run",
                   "--",safe_title.c_str(),safe_body.c_str(),static_cast<char *>(nullptr));
#else
            execlp("notify-send","notify-send","--app-name=DeskCode",safe_title.c_str(),safe_body.c_str(),static_cast<char *>(nullptr));
#endif
            _exit(0);
        }
        _exit(0);
    }
    if(pid>0){int st=0;while(waitpid(pid,&st,0)<0&&errno==EINTR){}}
#endif
}

namespace {
constexpr const char *WEB_CONSOLE_HTML=R"HTML(<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width,initial-scale=1">
<title>DeskCode Serve</title>
<style>
:root{--bg:#0d1117;--panel:#161b22;--border:#30363d;--fg:#e6edf3;--dim:#8b949e;--acc:#58a6ff;--ok:#3fb950;--warn:#d29922;--err:#f85149}
*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--fg);font:14px/1.55 ui-sans-serif,system-ui,-apple-system,sans-serif;display:flex;height:100vh;overflow:hidden}
aside{width:260px;background:var(--panel);border-right:1px solid var(--border);display:flex;flex-direction:column;padding:14px;gap:10px}
.brand{font-weight:700;font-size:16px;display:flex;justify-content:space-between;align-items:center}
button,select,input,textarea{background:#0d1117;color:var(--fg);border:1px solid var(--border);border-radius:8px;padding:7px 11px;font:inherit}
button{cursor:pointer;background:#21262d}button.primary{background:#1f6feb;border-color:#1f6feb;color:#fff}
.sess-list{flex:1;overflow:auto;display:flex;flex-direction:column;gap:6px}
.sess-item{padding:8px 10px;border-radius:8px;border:1px solid transparent;cursor:pointer;font-size:13px}
.sess-item.active{background:#1f2937;border-color:var(--acc)}
main{flex:1;display:flex;flex-direction:column;min-width:0}
header{padding:10px 16px;border-bottom:1px solid var(--border);background:var(--panel);display:flex;gap:10px;align-items:center;flex-wrap:wrap}
#log{flex:1;overflow:auto;padding:18px;display:flex;flex-direction:column;gap:12px}
.msg{padding:11px 14px;border-radius:10px;border:1px solid var(--border);background:var(--panel);white-space:pre-wrap;word-break:break-word}
.msg.user{border-color:#1f6feb;background:#121d2f}.msg.tool{font-family:ui-monospace,monospace;font-size:12.5px;color:var(--dim)}
#approval{display:none;margin:0 18px;padding:12px 14px;border-radius:10px;border:1px solid var(--warn);background:#2a2111;align-items:center;justify-content:space-between;gap:12px}
footer{padding:12px 18px;border-top:1px solid var(--border);background:var(--panel);display:flex;gap:10px}
textarea{flex:1;resize:none;height:52px}
.badge{font-size:12px;padding:2px 8px;border-radius:999px;border:1px solid var(--border);color:var(--dim)}
</style>
</head>
<body>
<aside>
  <div class="brand"><span>DeskCode Serve</span><span class="badge" id="ver"></span></div>
  <button class="primary" onclick="createSession()">+ New Session</button>
  <div class="sess-list" id="sessions"></div>
</aside>
<main>
  <header>
    <strong id="curTitle">No session</strong>
    <select id="modeSel" onchange="changeMode()">
      <option value="ask">Mode: Ask</option>
      <option value="edit">Mode: Auto-Edit</option>
      <option value="plan">Mode: Plan</option>
      <option value="full">Mode: Full</option>
    </select>
    <label class="badge"><input type="checkbox" id="noNet" onchange="changeMode()"> No-Net Sandbox</label>
    <span class="badge" id="statusBadge">Idle</span>
    <span class="badge" id="tokBadge">0 tokens</span>
    <button onclick="cancelTurn()">Stop</button>
  </header>
  <div id="log"></div>
  <div id="approval">
    <div><strong id="apTitle">Approval required</strong><div id="apDesc" style="font-family:monospace;font-size:12px;margin-top:4px"></div></div>
    <div style="display:flex;gap:8px"><button class="primary" onclick="decide(true)">Allow</button><button onclick="decide(false)">Deny</button></div>
  </div>
  <footer>
    <textarea id="promptBox" placeholder="Ask DeskCode across any connected terminal or browser... (Enter to send, Shift+Enter for newline)"></textarea>
    <button class="primary" onclick="sendPrompt()">Send</button>
  </footer>
</main>
<script>
const qs=new URLSearchParams(location.search);
const tok=qs.get('token')||localStorage.getItem('desk_serve_token')||'';
if(qs.get('token'))localStorage.setItem('desk_serve_token',tok);
let cur='',es=null,streamDiv=null,pendingReq='';
async function api(path,opts={}){
  const headers=Object.assign({'Content-Type':'application/json'},opts.headers||{});
  if(tok)headers['Authorization']='Bearer '+tok;
  const r=await fetch(path,Object.assign({},opts,{headers}));
  return r.json();
}
function addMsg(cls,text){const d=document.createElement('div');d.className='msg '+cls;d.textContent=text;const l=document.getElementById('log');l.appendChild(d);l.scrollTop=l.scrollHeight;return d;}
function showApproval(ap){
  const box=document.getElementById('approval');
  if(!ap){box.style.display='none';pendingReq='';return;}
  pendingReq=ap.request_id||'';
  document.getElementById('apTitle').textContent=ap.title||'Approval required';
  document.getElementById('apDesc').textContent=ap.description||'';
  box.style.display='flex';
}
async function refreshSessions(){
  const h=await api('/api/health');document.getElementById('ver').textContent='v'+(h.version||'');
  const data=await api('/api/sessions');
  const box=document.getElementById('sessions');box.innerHTML='';
  (data.sessions||[]).forEach(s=>{
    const el=document.createElement('div');el.className='sess-item'+(s.id===cur?' active':'');
    el.textContent=s.id+' · '+s.mode+(s.running?' (running)':'');
    el.onclick=()=>selectSession(s.id);box.appendChild(el);
  });
  if(!cur&&data.sessions&&data.sessions.length)selectSession(data.sessions[0].id);
}
async function createSession(){const r=await api('/api/sessions',{method:'POST',body:'{"mode":"edit"}'});if(r.session)await selectSession(r.session.id);refreshSessions();}
async function selectSession(id){
  cur=id;if(es){es.close();es=null;}streamDiv=null;
  document.getElementById('curTitle').textContent=id;
  document.getElementById('log').innerHTML='';
  const u='/api/sessions/'+encodeURIComponent(id)+'/events'+(tok?'?token='+encodeURIComponent(tok):'');
  es=new EventSource(u);
  es.addEventListener('snapshot',e=>{
    const s=JSON.parse(e.data);
    document.getElementById('modeSel').value=s.mode||'ask';
    document.getElementById('noNet').checked=!!s.no_net;
    document.getElementById('statusBadge').textContent=s.running?'Running':(s.state||'Idle');
    document.getElementById('tokBadge').textContent=(s.tokens||0)+' tokens';
    document.getElementById('log').innerHTML='';
    (s.transcript||[]).forEach(m=>addMsg(m.role==='user'?'user':(m.role==='tool'?'tool':'assistant'),m.text||''));
    showApproval(s.pending_approval||null);
  });
  es.addEventListener('user_message',e=>{streamDiv=null;addMsg('user',JSON.parse(e.data).text||'');document.getElementById('statusBadge').textContent='Running';});
  es.addEventListener('chunk',e=>{const t=JSON.parse(e.data).text||'';if(!streamDiv)streamDiv=addMsg('assistant','');streamDiv.textContent+=t;});
  es.addEventListener('assistant_text',e=>{const d=JSON.parse(e.data);if(!d.streamed)addMsg('assistant',d.text||'');streamDiv=null;});
  es.addEventListener('tool_start',e=>{const d=JSON.parse(e.data);addMsg('tool','▸ '+d.name+' '+(d.args||''));});
  es.addEventListener('permission_request',e=>showApproval(JSON.parse(e.data)));
  es.addEventListener('permission_resolved',()=>showApproval(null));
  es.addEventListener('turn_done',e=>{const d=JSON.parse(e.data);streamDiv=null;showApproval(null);document.getElementById('statusBadge').textContent=d.status||'Done';document.getElementById('tokBadge').textContent=(d.tokens||0)+' tokens';refreshSessions();});
  refreshSessions();
}
async function changeMode(){if(!cur)return;await api('/api/sessions/'+cur+'/mode',{method:'POST',body:JSON.stringify({mode:document.getElementById('modeSel').value,no_net:document.getElementById('noNet').checked})});}
async function sendPrompt(){
  const box=document.getElementById('promptBox');const text=box.value.trim();if(!text)return;
  if(!cur)await createSession();
  box.value='';
  await api('/api/sessions/'+cur+'/prompt',{method:'POST',body:JSON.stringify({prompt:text})});
}
async function decide(allow){if(!cur)return;await api('/api/sessions/'+cur+'/approve',{method:'POST',body:JSON.stringify({request_id:pendingReq,allow})});}
async function cancelTurn(){if(!cur)return;await api('/api/sessions/'+cur+'/cancel',{method:'POST',body:'{}'});}
document.getElementById('promptBox').addEventListener('keydown',e=>{if(e.key==='Enter'&&!e.shiftKey){e.preventDefault();sendPrompt();}});
refreshSessions();
</script>
</body>
</html>)HTML";

struct SseSubscriber {
    std::mutex mutex;
    std::condition_variable cv;
    std::deque<std::string> events;
    bool closed=false;
};

struct TranscriptEntry {
    std::string role,text;
};

struct SharedSession {
    std::string id;
    std::unique_ptr<App> app;
    std::string mode="ask";
    bool running=false;
    bool pending_flag=false;
    std::string pending_req_id,pending_title,pending_desc;
    int pending_decision=-1; // -1 waiting, 0 deny, 1 allow
    std::vector<TranscriptEntry> transcript;
    std::vector<std::shared_ptr<SseSubscriber>> subscribers;
    std::deque<std::string> prompt_queue;
    std::mutex mutex;
    std::condition_variable cv;
    std::atomic<bool> turn_cancelled{false};
    bool stop_worker=false;
    std::thread worker;
};

void apply_mode_str(App &a,const std::string &mode){
    a.yes=(mode=="full");
    a.auto_edit=(mode=="edit");
    a.plan_mode=(mode=="plan");
}
bool valid_mode_str(std::string_view m){
    return m=="ask"||m=="edit"||m=="plan"||m=="full";
}

void broadcast_sse(SharedSession &sess,const char *event_name,J *payload){
    std::string frame=std::string("event: ")+event_name+"\ndata: "+json_str(payload)+"\n\n";
    for(auto it=sess.subscribers.begin();it!=sess.subscribers.end();){
        auto &sub=*it;
        std::lock_guard slock(sub->mutex);
        if(sub->closed){it=sess.subscribers.erase(it);continue;}
        if(sub->events.size()<512)sub->events.push_back(frame);
        sub->cv.notify_all();
        ++it;
    }
}

J *session_summary_json(SharedSession &s){
    J *o=obj();
    put(o,"id",str(s.id.c_str()));
    put(o,"cwd",str(s.app->root));
    put(o,"mode",str(s.mode.c_str()));
    put(o,"no_net",json_object_new_boolean(s.app->sandbox_no_net));
    put(o,"running",json_object_new_boolean(s.running));
    put(o,"awaiting_approval",json_object_new_boolean(s.pending_flag));
    put(o,"state",str(s.app->state));
    put(o,"tokens",json_object_new_int64(static_cast<int64_t>(s.app->task_tokens)));
    return o;
}

J *session_snapshot_json(SharedSession &s){
    J *o=session_summary_json(s);
    J *tr_arr=arr();
    for(const auto &e:s.transcript){
        J *m=obj();
        put(m,"role",str(e.role.c_str()));
        put(m,"text",str(e.text.c_str()));
        add(tr_arr,m);
    }
    put(o,"transcript",tr_arr);
    if(s.app->todos)put(o,"todos",copy(s.app->todos));
    if(s.pending_flag){
        J *ap=obj();
        put(ap,"request_id",str(s.pending_req_id.c_str()));
        put(ap,"title",str(s.pending_title.c_str()));
        put(ap,"description",str(s.pending_desc.c_str()));
        put(o,"pending_approval",ap);
    }
    return o;
}

struct ServeState {
    App &base;
    std::string token;
    int bound_port=0;
    std::mutex mutex;
    int next_id=0;
    long req_seq=0;
    std::map<std::string,std::shared_ptr<SharedSession>> sessions;

    std::shared_ptr<SharedSession> create_session(const char *cwd,const std::string &mode,bool no_net){
        auto s=std::make_shared<SharedSession>();
        s->id="sess-"+std::to_string(++next_id);
        s->mode=valid_mode_str(mode)?mode:(base.plan_mode?"plan":base.yes?"full":base.auto_edit?"edit":"ask");
        s->app=std::make_unique<App>();
        const char *target_cwd=(cwd&&*cwd)?cwd:base.root;
        if(!canonical_directory(target_cwd,s->app->root))snprintf(s->app->root,sizeof s->app->root,"%s",base.root);
        snprintf(s->app->config_dir,sizeof s->app->config_dir,"%s",base.config_dir);
        s->app->store=copy(base.store);
        s->app->config=copy(base.config);
        s->app->messages=arr();
        s->app->shell=base.shell;
        s->app->desktop=base.desktop;
        s->app->sandbox=base.sandbox;
        s->app->sandbox_no_net=no_net||base.sandbox_no_net;
        s->app->notify=base.notify;
        s->app->max_steps=base.max_steps;
        s->app->json_output=true;
        s->app->quiet_human=true;
        s->app->cancel_requested=&s->turn_cancelled;
        s->app->state="等待任务";
        apply_mode_str(*s->app,s->mode);

        std::weak_ptr<SharedSession> weak=s;
        s->app->stream_observer=[weak](const std::string &chunk){
            auto sp=weak.lock();if(!sp||chunk.empty())return;
            std::lock_guard lock(sp->mutex);
            JsonPtr ev(obj());
            put(ev.get(),"session_id",str(sp->id.c_str()));
            put(ev.get(),"text",json_object_new_string_len(chunk.data(),static_cast<int>(chunk.size())));
            broadcast_sse(*sp,"chunk",ev.get());
        };
        s->app->on_assistant_text=[weak](const char *text,bool streamed){
            auto sp=weak.lock();if(!sp||!text||!*text)return;
            std::lock_guard lock(sp->mutex);
            if(sp->transcript.size()<400)sp->transcript.push_back({"assistant",text});
            JsonPtr ev(obj());
            put(ev.get(),"session_id",str(sp->id.c_str()));
            put(ev.get(),"text",str(text));
            put(ev.get(),"streamed",json_object_new_boolean(streamed));
            broadcast_sse(*sp,"assistant_text",ev.get());
        };
        s->app->on_tool_start=[weak](const char *id,const char *name,J *args){
            auto sp=weak.lock();if(!sp)return;
            std::string arg_txt=args?json_str(args):"{}";
            if(arg_txt.size()>240)arg_txt=arg_txt.substr(0,240)+"...";
            std::lock_guard lock(sp->mutex);
            if(sp->transcript.size()<400)sp->transcript.push_back({"tool",std::string("▸ ")+name+" "+arg_txt});
            JsonPtr ev(obj());
            put(ev.get(),"id",str(id?id:""));
            put(ev.get(),"name",str(name?name:""));
            put(ev.get(),"args",str(arg_txt.c_str()));
            broadcast_sse(*sp,"tool_start",ev.get());
        };
        s->app->on_tool_end=[weak](const char *id,const char *name,J *,J *result){
            auto sp=weak.lock();if(!sp)return;
            std::lock_guard lock(sp->mutex);
            JsonPtr ev(obj());
            put(ev.get(),"id",str(id?id:""));
            put(ev.get(),"name",str(name?name:""));
            put(ev.get(),"ok",json_object_new_boolean(!tool_failed(result)));
            if(sp->app->todos)put(ev.get(),"todos",copy(sp->app->todos));
            broadcast_sse(*sp,"tool_end",ev.get());
        };
        s->app->approval_hook=[this,weak](const char *title,const char *description,int)->int{
            auto sp=weak.lock();if(!sp)return 0;
            std::string rid;
            {
                std::lock_guard glock(mutex);
                rid="perm-"+std::to_string(++req_seq);
            }
            std::unique_lock lock(sp->mutex);
            sp->pending_flag=true;
            sp->pending_req_id=rid;
            sp->pending_title=title?title:"Approval required";
            sp->pending_desc=description?description:"";
            sp->pending_decision=-1;
            JsonPtr ev(obj());
            put(ev.get(),"session_id",str(sp->id.c_str()));
            put(ev.get(),"request_id",str(rid.c_str()));
            put(ev.get(),"title",str(sp->pending_title.c_str()));
            put(ev.get(),"description",str(sp->pending_desc.c_str()));
            broadcast_sse(*sp,"permission_request",ev.get());
            notify_user_event(*sp->app,tr("DeskCode 等待批准"),sp->pending_title.c_str());
            while(!sp->stop_worker&&!app_cancelled(sp->app.get())&&sp->pending_decision<0){
                sp->cv.wait_for(lock,std::chrono::milliseconds(100));
            }
            int decision=sp->pending_decision==1?1:0;
            sp->pending_flag=false;
            sp->pending_req_id.clear();
            JsonPtr res_ev(obj());
            put(res_ev.get(),"request_id",str(rid.c_str()));
            put(res_ev.get(),"allow",json_object_new_boolean(decision==1));
            broadcast_sse(*sp,"permission_resolved",res_ev.get());
            return decision;
        };

        s->worker=std::thread([s]{
            for(;;){
                std::string prompt;
                {
                    std::unique_lock lock(s->mutex);
                    s->cv.wait(lock,[&]{return s->stop_worker||!s->prompt_queue.empty();});
                    if(s->stop_worker)break;
                    prompt=std::move(s->prompt_queue.front());
                    s->prompt_queue.pop_front();
                    s->running=true;
                    s->turn_cancelled.store(false,std::memory_order_relaxed);
                    apply_mode_str(*s->app,s->mode);
                }
                int rc=run_task(s->app.get(),prompt.c_str());
                {
                    std::lock_guard lock(s->mutex);
                    s->running=false;
                    const bool was_cancelled=s->turn_cancelled.load(std::memory_order_relaxed)||!strcmp(s->app->state,"已取消");
                    const char *status=s->app->plan_submitted?"plan_ready":(rc==0?"completed":(was_cancelled?"cancelled":"failed"));
                    JsonPtr done_ev(obj());
                    put(done_ev.get(),"session_id",str(s->id.c_str()));
                    put(done_ev.get(),"status",str(status));
                    put(done_ev.get(),"state",str(s->app->state));
                    put(done_ev.get(),"tokens",json_object_new_int64(static_cast<int64_t>(s->app->task_tokens)));
                    if(rc!=0&&*s->app->error)put(done_ev.get(),"error",str(s->app->error));
                    broadcast_sse(*s,"turn_done",done_ev.get());
                    notify_user_event(*s->app,rc==0?tr("DeskCode 任务已完成"):tr("DeskCode 任务未完成"),prompt.c_str());
                }
            }
        });

        sessions[s->id]=s;
        return s;
    }

    void shutdown_all(){
        std::vector<std::shared_ptr<SharedSession>> all;
        {
            std::lock_guard lock(mutex);
            for(auto &[id,s]:sessions)all.push_back(s);
            sessions.clear();
        }
        for(auto &s:all){
            s->turn_cancelled.store(true,std::memory_order_relaxed);
            {
                std::lock_guard lock(s->mutex);
                s->stop_worker=true;
                s->pending_decision=0;
                for(auto &sub:s->subscribers){
                    std::lock_guard slock(sub->mutex);
                    sub->closed=true;
                    sub->cv.notify_all();
                }
                s->cv.notify_all();
            }
            if(s->worker.joinable())s->worker.join();
            app_cleanup(s->app.get());
        }
    }
};

struct HttpReq {
    std::string method,path,query,body;
    std::map<std::string,std::string> headers;
};

bool read_http_request(SockFd fd,HttpReq &req){
    std::string buf;
    size_t hdr_end=std::string::npos;
    const double deadline=monotime()+10.0;
    while(monotime()<deadline&&(hdr_end=buf.find("\r\n\r\n"))==std::string::npos){
#ifdef _WIN32
        fd_set rfds;FD_ZERO(&rfds);FD_SET(fd,&rfds);
        timeval tv{0,200000};
        if(select(0,&rfds,nullptr,nullptr,&tv)<=0)continue;
        char chunk[4096];
        int n=recv(fd,chunk,sizeof chunk,0);
#else
        pollfd pfd{fd,POLLIN,0};
        if(poll(&pfd,1,200)<=0)continue;
        char chunk[4096];
        ssize_t n=recv(fd,chunk,sizeof chunk,0);
#endif
        if(n<=0)return false;
        buf.append(chunk,static_cast<size_t>(n));
        if(buf.size()>1048576)return false;
    }
    if(hdr_end==std::string::npos)return false;
    std::string_view head(buf.data(),hdr_end);
    auto line_end=head.find("\r\n");
    std::string_view req_line=line_end==std::string_view::npos?head:head.substr(0,line_end);
    auto sp1=req_line.find(' '),sp2=sp1==std::string_view::npos?std::string_view::npos:req_line.find(' ',sp1+1);
    if(sp1==std::string_view::npos||sp2==std::string_view::npos)return false;
    req.method.assign(req_line.substr(0,sp1));
    std::string_view target=req_line.substr(sp1+1,sp2-sp1-1);
    auto qpos=target.find('?');
    req.path.assign(qpos==std::string_view::npos?target:target.substr(0,qpos));
    if(qpos!=std::string_view::npos)req.query.assign(target.substr(qpos+1));

    size_t pos=line_end==std::string_view::npos?head.size():line_end+2;
    while(pos<head.size()){
        auto nl=head.find("\r\n",pos);
        std::string_view ln=nl==std::string_view::npos?head.substr(pos):head.substr(pos,nl-pos);
        auto col=ln.find(':');
        if(col!=std::string_view::npos){
            std::string k(ln.substr(0,col));
            for(char &c:k)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            std::string_view v=ln.substr(col+1);
            while(!v.empty()&&(v.front()==' '||v.front()=='\t'))v.remove_prefix(1);
            req.headers[k]=std::string(v);
        }
        if(nl==std::string_view::npos)break;
        pos=nl+2;
    }
    size_t clen=0;
    if(auto it=req.headers.find("content-length");it!=req.headers.end()){
        long parsed=strtol(it->second.c_str(),nullptr,10);
        if(parsed<0||parsed>1048576)return false;
        clen=static_cast<size_t>(parsed);
    }
    req.body.assign(buf.data()+hdr_end+4,buf.size()-(hdr_end+4));
    while(req.body.size()<clen&&monotime()<deadline){
#ifdef _WIN32
        fd_set rfds;FD_ZERO(&rfds);FD_SET(fd,&rfds);
        timeval tv{0,200000};
        if(select(0,&rfds,nullptr,nullptr,&tv)<=0)continue;
        char chunk[4096];
        int n=recv(fd,chunk,static_cast<int>((std::min)(sizeof chunk,clen-req.body.size())),0);
#else
        pollfd pfd{fd,POLLIN,0};
        if(poll(&pfd,1,200)<=0)continue;
        char chunk[4096];
        ssize_t n=recv(fd,chunk,std::min(sizeof chunk,clen-req.body.size()),0);
#endif
        if(n<=0)return false;
        req.body.append(chunk,static_cast<size_t>(n));
    }
    if(req.body.size()>clen)req.body.resize(clen);
    return req.body.size()==clen;
}

void send_json(SockFd fd,int status,const char *reason,J *body){
    std::string payload=json_str(body);
    char hdr[256];
    snprintf(hdr,sizeof hdr,"HTTP/1.1 %d %s\r\nContent-Type: application/json; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n",status,reason,payload.size());
    send_all(fd,hdr);
    send_all(fd,payload);
}

std::string query_value(std::string_view qs,std::string_view key){
    while(!qs.empty()){
        auto amp=qs.find('&');
        std::string_view kv=amp==std::string_view::npos?qs:qs.substr(0,amp);
        auto eq=kv.find('=');
        if(eq!=std::string_view::npos&&kv.substr(0,eq)==key)return std::string(kv.substr(eq+1));
        if(amp==std::string_view::npos)break;
        qs.remove_prefix(amp+1);
    }
    return {};
}

bool check_auth_and_origin(ServeState &st,const HttpReq &req){
    if(!st.token.empty()){
        std::string provided;
        if(auto it=req.headers.find("authorization");it!=req.headers.end()&&it->second.starts_with("Bearer "))
            provided=it->second.substr(7);
        else provided=query_value(req.query,"token");
        if(provided.size()!=st.token.size())return false;
        unsigned char diff=0;
        for(size_t i=0;i<st.token.size();i++)diff|=static_cast<unsigned char>(provided[i]^st.token[i]);
        if(diff!=0)return false;
    }else if(req.method!="GET"){
        if(auto it=req.headers.find("origin");it!=req.headers.end()){
            const std::string &orig=it->second;
            const std::string p1="http://127.0.0.1:"+std::to_string(st.bound_port);
            const std::string p2="http://localhost:"+std::to_string(st.bound_port);
            if(orig!=p1&&orig!=p2)return false;
        }
    }
    return true;
}

void handle_serve_client(ServeState &st,SockFd cfd){
    HttpReq req;
    if(!read_http_request(cfd,req)){close_socket(cfd);return;}
    if(req.method=="GET"&&(req.path=="/"||req.path=="/index.html")){
        std::string_view html(WEB_CONSOLE_HTML);
        char hdr[256];
        snprintf(hdr,sizeof hdr,"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\nConnection: close\r\nContent-Length: %zu\r\n\r\n",html.size());
        send_all(cfd,hdr);send_all(cfd,html);close_socket(cfd);return;
    }
    if(!check_auth_and_origin(st,req)){
        JsonPtr e(err("Unauthorized"));
        send_json(cfd,401,"Unauthorized",e.get());
        close_socket(cfd);
        return;
    }
    if(req.method=="GET"&&req.path=="/api/health"){
        JsonPtr r(obj());
        put(r.get(),"ok",json_object_new_boolean(1));
        put(r.get(),"version",str(VERSION));
        put(r.get(),"workspace",str(st.base.root));
        put(r.get(),"sandbox_backend",str(sandbox_backend()));
        put(r.get(),"sandbox_net_support",json_object_new_boolean(sandbox_net_supported()));
        send_json(cfd,200,"OK",r.get());
        close_socket(cfd);
        return;
    }
    if(req.path=="/api/sessions"){
        if(req.method=="GET"){
            JsonPtr r(obj());J *list=arr();
            {
                std::lock_guard glock(st.mutex);
                for(auto &[id,s]:st.sessions){
                    std::lock_guard slock(s->mutex);
                    add(list,session_summary_json(*s));
                }
            }
            put(r.get(),"sessions",list);
            send_json(cfd,200,"OK",r.get());
            close_socket(cfd);
            return;
        }
        if(req.method=="POST"){
            JsonPtr body(req.body.empty()?obj():json_tokener_parse(req.body.c_str()));
            if(!json_object_is_type(body.get(),json_type_object)){JsonPtr e(err("Invalid JSON body"));send_json(cfd,400,"Bad Request",e.get());close_socket(cfd);return;}
            std::shared_ptr<SharedSession> created;
            {
                std::lock_guard glock(st.mutex);
                if(st.sessions.size()>=16){JsonPtr e(err("Maximum of 16 active sessions reached."));send_json(cfd,429,"Too Many Requests",e.get());close_socket(cfd);return;}
                created=st.create_session(sget(body.get(),"cwd"),sget(body.get(),"mode"),bget(body.get(),"no_net"));
            }
            JsonPtr r(obj());
            {
                std::lock_guard slock(created->mutex);
                put(r.get(),"session",session_snapshot_json(*created));
            }
            send_json(cfd,201,"Created",r.get());
            close_socket(cfd);
            return;
        }
    }
    if(req.path.starts_with("/api/sessions/")){
        std::string rest=req.path.substr(strlen("/api/sessions/"));
        auto slash=rest.find('/');
        std::string sid=slash==std::string::npos?rest:rest.substr(0,slash);
        std::string action=slash==std::string::npos?"":rest.substr(slash+1);
        std::shared_ptr<SharedSession> sess;
        {
            std::lock_guard glock(st.mutex);
            auto it=st.sessions.find(sid);
            if(it!=st.sessions.end())sess=it->second;
        }
        if(!sess){JsonPtr e(err("Session not found"));send_json(cfd,404,"Not Found",e.get());close_socket(cfd);return;}

        if(action.empty()&&req.method=="GET"){
            JsonPtr snap;
            {std::lock_guard slock(sess->mutex);snap.reset(session_snapshot_json(*sess));}
            send_json(cfd,200,"OK",snap.get());
            close_socket(cfd);
            return;
        }
        if(action=="mode"&&req.method=="POST"){
            JsonPtr body(json_tokener_parse(req.body.c_str()));
            const std::string m=sget(body.get(),"mode");
            {
                std::lock_guard slock(sess->mutex);
                if(valid_mode_str(m)){sess->mode=m;apply_mode_str(*sess->app,m);}
                if(json_object_object_get_ex(body.get(),"no_net",nullptr))sess->app->sandbox_no_net=bget(body.get(),"no_net");
            }
            JsonPtr r;
            {std::lock_guard slock(sess->mutex);r.reset(session_summary_json(*sess));}
            send_json(cfd,200,"OK",r.get());
            close_socket(cfd);
            return;
        }
        if(action=="prompt"&&req.method=="POST"){
            JsonPtr body(json_tokener_parse(req.body.c_str()));
            const std::string prompt=sget(body.get(),"prompt");
            if(prompt.empty()||prompt.size()>262144){JsonPtr e(err("Prompt must be 1..262144 bytes."));send_json(cfd,400,"Bad Request",e.get());close_socket(cfd);return;}
            const std::string m=sget(body.get(),"mode");
            {
                std::lock_guard slock(sess->mutex);
                if(valid_mode_str(m)){sess->mode=m;apply_mode_str(*sess->app,m);}
                if(sess->transcript.size()<400)sess->transcript.push_back({"user",prompt});
                sess->prompt_queue.push_back(prompt);
                JsonPtr uev(obj());
                put(uev.get(),"session_id",str(sess->id.c_str()));
                put(uev.get(),"text",str(prompt.c_str()));
                broadcast_sse(*sess,"user_message",uev.get());
                sess->cv.notify_all();
            }
            JsonPtr r(obj());
            put(r.get(),"accepted",json_object_new_boolean(1));
            put(r.get(),"session_id",str(sess->id.c_str()));
            send_json(cfd,202,"Accepted",r.get());
            close_socket(cfd);
            return;
        }
        if(action=="approve"&&req.method=="POST"){
            JsonPtr body(json_tokener_parse(req.body.c_str()));
            const bool allow=bget(body.get(),"allow");
            const std::string req_id=sget(body.get(),"request_id");
            bool matched=false;
            {
                std::lock_guard slock(sess->mutex);
                if(sess->pending_flag&&(req_id.empty()||req_id==sess->pending_req_id)){
                    sess->pending_decision=allow?1:0;
                    sess->cv.notify_all();
                    matched=true;
                }
            }
            JsonPtr r(obj());
            put(r.get(),"resolved",json_object_new_boolean(matched));
            send_json(cfd,matched?200:409,matched?"OK":"Conflict",r.get());
            close_socket(cfd);
            return;
        }
        if(action=="cancel"&&req.method=="POST"){
            sess->turn_cancelled.store(true,std::memory_order_relaxed);
            {
                std::lock_guard slock(sess->mutex);
                if(sess->pending_flag){sess->pending_decision=0;sess->cv.notify_all();}
            }
            JsonPtr r(obj());put(r.get(),"cancelled",json_object_new_boolean(1));
            send_json(cfd,200,"OK",r.get());
            close_socket(cfd);
            return;
        }
        if(action=="events"&&req.method=="GET"){
            const char *sse_hdr="HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\nCache-Control: no-cache\r\nConnection: keep-alive\r\n\r\n";
            if(!send_all(cfd,sse_hdr)){close_socket(cfd);return;}
            auto sub=std::make_shared<SseSubscriber>();
            std::string initial_frame;
            {
                std::lock_guard slock(sess->mutex);
                JsonPtr snap(session_snapshot_json(*sess));
                initial_frame="event: snapshot\ndata: "+json_str(snap.get())+"\n\n";
                sess->subscribers.push_back(sub);
            }
            if(!send_all(cfd,initial_frame)){
                std::lock_guard slock(sub->mutex);sub->closed=true;
                close_socket(cfd);return;
            }
            for(;;){
                std::string frame;
                {
                    std::unique_lock slock(sub->mutex);
                    sub->cv.wait_for(slock,std::chrono::seconds(15),[&]{return sub->closed||!sub->events.empty()||interrupted!=0;});
                    if(sub->closed||interrupted!=0)break;
                    if(!sub->events.empty()){
                        frame=std::move(sub->events.front());
                        sub->events.pop_front();
                    }else frame=": ping\n\n";
                }
                if(!send_all(cfd,frame))break;
            }
            {std::lock_guard slock(sub->mutex);sub->closed=true;}
            close_socket(cfd);
            return;
        }
    }
    JsonPtr nf(err("Not found"));
    send_json(cfd,404,"Not Found",nf.get());
    close_socket(cfd);
}
} // namespace

int serve_main(App &app,int argc,char **argv,int first){
    std::string host="127.0.0.1";
    int port=7890;
    std::string token=env_value("DESK_SERVE_TOKEN")?env_value("DESK_SERVE_TOKEN"):"";
    std::string port_file;
    for(int i=first;i<argc;i++){
        if(!strcmp(argv[i],"--host")&&i+1<argc)host=argv[++i];
        else if(!strcmp(argv[i],"--port")&&i+1<argc)port=atoi(argv[++i]);
        else if(!strcmp(argv[i],"--token")&&i+1<argc)token=argv[++i];
        else if(!strcmp(argv[i],"--port-file")&&i+1<argc)port_file=argv[++i];
        else if(!strcmp(argv[i],"--no-net"))app.sandbox_no_net=true;
        else if(!strcmp(argv[i],"--notify"))app.notify=true;
        else{fprintf(stderr,"%s\n",tr("用法：desk serve [--host 127.0.0.1] [--port 7890] [--token 密钥] [--no-net] [--notify]"));return 2;}
    }
    if(port<0||port>65535){fprintf(stderr,"%s\n",tr("端口号无效（0..65535）。"));return 2;}
    const bool is_loopback=(host=="127.0.0.1"||host=="localhost");
    if(!is_loopback&&token.empty()){
        char rnd[33]{};id_string(rnd);
        token=rnd;
    }

    SockFd lfd=socket(AF_INET,SOCK_STREAM,0);
    if(lfd==BAD_SOCK){fprintf(stderr,"%s\n",tr("无法创建监听套接字。"));return 1;}
    int reuse=1;
#ifdef _WIN32
    SetHandleInformation(reinterpret_cast<HANDLE>(lfd),HANDLE_FLAG_INHERIT,0);
    setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,reinterpret_cast<const char *>(&reuse),sizeof reuse);
#else
    fcntl(lfd,F_SETFD,FD_CLOEXEC);
    setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof reuse);
#endif
    sockaddr_in addr{};
    addr.sin_family=AF_INET;
    if(host=="localhost"||host=="127.0.0.1")addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    else if(host=="0.0.0.0")addr.sin_addr.s_addr=htonl(INADDR_ANY);
    else if(inet_pton(AF_INET,host.c_str(),&addr.sin_addr)!=1){
        close_socket(lfd);
        fprintf(stderr,"%s\n",tr("监听地址无效，请使用 127.0.0.1 或有效的 IPv4 地址。"));
        return 2;
    }
    addr.sin_port=htons(static_cast<uint16_t>(port));
#ifdef _WIN32
    int alen_sz=sizeof addr;
    if(bind(lfd,reinterpret_cast<sockaddr *>(&addr),sizeof addr)==SOCKET_ERROR||getsockname(lfd,reinterpret_cast<sockaddr *>(&addr),&alen_sz)==SOCKET_ERROR||listen(lfd,32)==SOCKET_ERROR){
        close_socket(lfd);fprintf(stderr,"%s\n",tr("无法绑定 desk serve 监听端口。"));return 1;
    }
#else
    socklen_t alen_sz=sizeof addr;
    if(bind(lfd,reinterpret_cast<sockaddr *>(&addr),sizeof addr)!=0||getsockname(lfd,reinterpret_cast<sockaddr *>(&addr),&alen_sz)!=0||listen(lfd,32)!=0){
        close_socket(lfd);fprintf(stderr,"%s\n",tr("无法绑定 desk serve 监听端口。"));return 1;
    }
#endif
    const int actual_port=ntohs(addr.sin_port);
    ServeState state{app,token,actual_port,{},0,0,{}};
    {
        std::lock_guard glock(state.mutex);
        state.create_session(app.root,"",app.sandbox_no_net);
    }
    if(!port_file.empty()){
        std::string ptxt=std::to_string(actual_port)+"\n";
        atomic_file(port_file.c_str(),ptxt.data(),ptxt.size(),0600);
    }
    const std::string display_host=(host=="0.0.0.0")?"127.0.0.1":host;
    fprintf(stderr,"DeskCode Serve %s listening on http://%s:%d%s\n",VERSION,display_host.c_str(),actual_port,
            token.empty()?"":("?token="+token).c_str());
    fflush(stderr);

    constexpr size_t MAX_SERVE_CLIENTS=64;
    std::mutex client_mutex;
    std::condition_variable client_cv;
    size_t active_clients=0;
    while(!interrupted){
#ifdef _WIN32
        fd_set rfds;FD_ZERO(&rfds);FD_SET(lfd,&rfds);
        timeval tv{0,150000};
        if(select(0,&rfds,nullptr,nullptr,&tv)<=0)continue;
        SockFd cfd=accept(lfd,nullptr,nullptr);
        if(cfd==BAD_SOCK)continue;
        SetHandleInformation(reinterpret_cast<HANDLE>(cfd),HANDLE_FLAG_INHERIT,0);
        DWORD snd_tmo=15000;
        setsockopt(cfd,SOL_SOCKET,SO_SNDTIMEO,reinterpret_cast<const char *>(&snd_tmo),sizeof snd_tmo);
#else
        pollfd pfd{lfd,POLLIN,0};
        if(poll(&pfd,1,150)<=0)continue;
        SockFd cfd=accept(lfd,nullptr,nullptr);
        if(cfd<0){if(errno==EINTR)continue;break;}
        fcntl(cfd,F_SETFD,FD_CLOEXEC);
        timeval snd_tmo{15,0};
        setsockopt(cfd,SOL_SOCKET,SO_SNDTIMEO,&snd_tmo,sizeof snd_tmo);
#endif
        {
            std::lock_guard lock(client_mutex);
            if(active_clients>=MAX_SERVE_CLIENTS){close_socket(cfd);continue;}
            ++active_clients;
        }
        std::thread([&state,&client_mutex,&client_cv,&active_clients,cfd]{
            struct ClientSlotGuard {
                std::mutex &m;
                std::condition_variable &cv;
                size_t &cnt;
                ~ClientSlotGuard(){
                    std::lock_guard lock(m);
                    --cnt;
                    cv.notify_all();
                }
            } slot{client_mutex,client_cv,active_clients};
            handle_serve_client(state,cfd);
        }).detach();
    }
    close_socket(lfd);
    state.shutdown_all();
    {
        std::unique_lock lock(client_mutex);
        client_cv.wait_for(lock,std::chrono::seconds(5),[&]{return active_clients==0;});
    }
    return 0;
}

namespace {
std::string attach_http(const char *method,const std::string &url,const std::string &token,const std::string &body,long &status_out){
    status_out=0;
    CurlHandle h(curl_easy_init());
    if(!h)return {};
    std::string resp;
    HttpHeaders hdrs;
    hdrs.append("Content-Type: application/json");
    if(!token.empty())hdrs.append("Authorization: Bearer "+token);
    auto writer=+[](char *ptr,size_t sz,size_t cnt,void *ud)->size_t{
        auto *out=static_cast<std::string *>(ud);
        if(sz&&cnt>SIZE_MAX/sz)return 0;
        size_t n=sz*cnt;
        if(n>4194304-out->size())return 0;
        try{out->append(ptr,n);}catch(...){return 0;}
        return n;
    };
    CurlRequest req(h.get());
    curl_easy_setopt(h.get(),CURLOPT_URL,url.c_str());
    curl_easy_setopt(h.get(),CURLOPT_HTTPHEADER,hdrs.get());
    curl_easy_setopt(h.get(),CURLOPT_CONNECTTIMEOUT_MS,5000L);
    curl_easy_setopt(h.get(),CURLOPT_TIMEOUT_MS,15000L);
    curl_easy_setopt(h.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(h.get(),CURLOPT_WRITEFUNCTION,writer);
    curl_easy_setopt(h.get(),CURLOPT_WRITEDATA,&resp);
    if(!strcmp(method,"POST")){
        curl_easy_setopt(h.get(),CURLOPT_POST,1L);
        curl_easy_setopt(h.get(),CURLOPT_POSTFIELDS,body.c_str());
        curl_easy_setopt(h.get(),CURLOPT_POSTFIELDSIZE,static_cast<long>(body.size()));
    }
    if(curl_easy_perform(h.get())!=CURLE_OK)return {};
    curl_easy_getinfo(h.get(),CURLINFO_RESPONSE_CODE,&status_out);
    return resp;
}
} // namespace

int attach_main(App &,int argc,char **argv,int first){
    std::string base_url="http://127.0.0.1:7890";
    std::string session_id,prompt,approve_decision;
    std::string token=env_value("DESK_SERVE_TOKEN")?env_value("DESK_SERVE_TOKEN"):"";
    bool json_out=false;
    for(int i=first;i<argc;i++){
        const char *a=argv[i];
        if(!strcmp(a,"--session")&&i+1<argc)session_id=argv[++i];
        else if((!strcmp(a,"-p")||!strcmp(a,"--print"))&&i+1<argc)prompt=argv[++i];
        else if(!strcmp(a,"--token")&&i+1<argc)token=argv[++i];
        else if(!strcmp(a,"--approve")&&i+1<argc)approve_decision=argv[++i];
        else if(!strcmp(a,"--json"))json_out=true;
        else if(a[0]!='-'&&!strncmp(a,"http",4))base_url=a;
        else{fprintf(stderr,"%s\n",tr("用法：desk attach [http://127.0.0.1:7890] [--session ID] [-p 需求] [--approve allow|deny] [--token 密钥]"));return 2;}
    }
    while(!base_url.empty()&&base_url.back()=='/')base_url.pop_back();

    long status=0;
    if(session_id.empty()){
        std::string sraw=attach_http("GET",base_url+"/api/sessions",token,{},status);
        if(status<200||status>=300){fprintf(stderr,"%s\n",tr("无法连接 desk serve 守护服务。"));return 1;}
        JsonPtr sdoc(json_tokener_parse(sraw.c_str()));
        J *slist=get(sdoc.get(),"sessions");
        if(alen(slist)>0)session_id=sget(at(slist,alen(slist)-1),"id");
        else{
            std::string craw=attach_http("POST",base_url+"/api/sessions",token,"{}",status);
            JsonPtr cdoc(json_tokener_parse(craw.c_str()));
            session_id=sget(get(cdoc.get(),"session"),"id");
        }
    }
    if(session_id.empty()){fprintf(stderr,"%s\n",tr("无法获取共享会话 ID。"));return 1;}

    if(!approve_decision.empty()){
        const bool allow=(approve_decision=="allow"||approve_decision=="yes"||approve_decision=="1");
        JsonPtr body(obj());put(body.get(),"allow",json_object_new_boolean(allow));
        std::string r=attach_http("POST",base_url+"/api/sessions/"+session_id+"/approve",token,json_str(body.get()),status);
        if(!json_out)puts(r.c_str());
        else puts(r.c_str());
        return (status>=200&&status<300)?0:1;
    }

    if(prompt.empty()){
        std::string snap=attach_http("GET",base_url+"/api/sessions/"+session_id,token,{},status);
        if(status<200||status>=300){fprintf(stderr,"%s\n",tr("无法读取共享会话状态。"));return 1;}
        puts(snap.c_str());
        return 0;
    }

    // Open SSE stream BEFORE posting prompt so zero chunks are missed!
    struct StreamCtx {
        std::string buffer,event_name,data_lines,reply,final_status;
        int64_t tokens=0;
        std::atomic<bool> done{false};
        bool printed_chunks=false,json_mode=false;
    } ctx{};
    ctx.json_mode=json_out;

    CurlHandle sse(curl_easy_init());
    if(!sse)return 1;
    HttpHeaders hdrs;
    hdrs.append("Accept: text/event-stream");
    if(!token.empty())hdrs.append("Authorization: Bearer "+token);
    std::string events_url=base_url+"/api/sessions/"+session_id+"/events";
    std::atomic<bool> sse_connected{false};

    auto sse_writer=+[](char *ptr,size_t sz,size_t cnt,void *ud)->size_t{
        auto *c=static_cast<StreamCtx *>(ud);
        try{
            if(sz&&cnt>SIZE_MAX/sz)return 0;
            size_t n=sz*cnt;
            if(n>RESPONSE_LIMIT-c->buffer.size())return 0;
            c->buffer.append(ptr,n);
            std::string_view rem(c->buffer);
            for(;;){
                auto nl=rem.find('\n');
                if(nl==std::string_view::npos)break;
                std::string_view line=rem.substr(0,nl);
                rem.remove_prefix(nl+1);
                if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
                if(line.empty()){
                    if(!c->event_name.empty()&&!c->data_lines.empty()){
                        JsonPtr payload(json_tokener_parse(c->data_lines.c_str()));
                        if(c->event_name=="chunk"){
                            const char *t=sget(payload.get(),"text");
                            c->reply+=t;
                            c->printed_chunks=true;
                            if(!c->json_mode){fputs(t,stdout);fflush(stdout);}
                        }else if(c->event_name=="assistant_text"){
                            const char *t=sget(payload.get(),"text");
                            if(!bget(payload.get(),"streamed")){
                                c->reply+=t;
                                if(!c->json_mode){fputs(t,stdout);fputc('\n',stdout);fflush(stdout);}
                            }else if(!c->json_mode){
                                fputc('\n',stdout);fflush(stdout);
                            }
                        }else if(c->event_name=="turn_done"){
                            c->final_status=sget(payload.get(),"status");
                            c->tokens=json_object_get_int64(get(payload.get(),"tokens"));
                            c->done.store(true,std::memory_order_relaxed);
                            c->event_name.clear();c->data_lines.clear();
                            return 0; // End SSE transfer cleanly once turn completes.
                        }
                    }
                    c->event_name.clear();
                    c->data_lines.clear();
                }else if(line.starts_with("event:")){
                    std::string_view ev=line.substr(6);
                    while(!ev.empty()&&ev.front()==' ')ev.remove_prefix(1);
                    c->event_name.assign(ev);
                }else if(line.starts_with("data:")){
                    std::string_view d=line.substr(5);
                    if(!d.empty()&&d.front()==' ')d.remove_prefix(1);
                    if(!c->data_lines.empty())c->data_lines+='\n';
                    c->data_lines.append(d);
                }
            }
            const size_t consumed=c->buffer.size()-rem.size();
            if(consumed)c->buffer.erase(0,consumed);
            return n;
        }catch(...){return 0;}
    };
    auto sse_hdr_cb=+[](char *ptr,size_t sz,size_t cnt,void *ud)->size_t{
        if(sz&&cnt>SIZE_MAX/sz)return 0;
        size_t n=sz*cnt;
        std::string_view sv(ptr,n);
        if(sv.find("200")!=std::string_view::npos)static_cast<std::atomic<bool> *>(ud)->store(true,std::memory_order_relaxed);
        return n;
    };
    auto sse_prog=+[](void *ud,curl_off_t,curl_off_t,curl_off_t,curl_off_t)->int{
        auto *c=static_cast<StreamCtx *>(ud);
        return (interrupted||c->done.load(std::memory_order_relaxed))?1:0;
    };

    std::thread poster([&]{
        for(int i=0;i<100&&!sse_connected.load(std::memory_order_relaxed)&&!interrupted;i++){
#ifdef _WIN32
            Sleep(15);
#else
            usleep(15000);
#endif
        }
        JsonPtr pbody(obj());
        put(pbody.get(),"prompt",str(prompt.c_str()));
        long pst=0;
        attach_http("POST",base_url+"/api/sessions/"+session_id+"/prompt",token,json_str(pbody.get()),pst);
        if(pst<200||pst>=300)ctx.done.store(true,std::memory_order_relaxed);
    });

    CurlRequest sreq(sse.get());
    curl_easy_setopt(sse.get(),CURLOPT_URL,events_url.c_str());
    curl_easy_setopt(sse.get(),CURLOPT_HTTPHEADER,hdrs.get());
    curl_easy_setopt(sse.get(),CURLOPT_CONNECTTIMEOUT_MS,5000L);
    curl_easy_setopt(sse.get(),CURLOPT_TIMEOUT_MS,300000L);
    curl_easy_setopt(sse.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(sse.get(),CURLOPT_HEADERFUNCTION,sse_hdr_cb);
    curl_easy_setopt(sse.get(),CURLOPT_HEADERDATA,&sse_connected);
    curl_easy_setopt(sse.get(),CURLOPT_WRITEFUNCTION,sse_writer);
    curl_easy_setopt(sse.get(),CURLOPT_WRITEDATA,&ctx);
    curl_easy_setopt(sse.get(),CURLOPT_XFERINFOFUNCTION,sse_prog);
    curl_easy_setopt(sse.get(),CURLOPT_XFERINFODATA,&ctx);
    curl_easy_setopt(sse.get(),CURLOPT_NOPROGRESS,0L);
    curl_easy_perform(sse.get());
    sse_connected=true;
    if(poster.joinable())poster.join();

    if(json_out){
        JsonPtr out(obj());
        put(out.get(),"session_id",str(session_id.c_str()));
        put(out.get(),"status",str(ctx.final_status.empty()?"failed":ctx.final_status.c_str()));
        put(out.get(),"reply",str(ctx.reply.c_str()));
        put(out.get(),"tokens",json_object_new_int64(ctx.tokens));
        puts(json_str(out.get()).c_str());
    }
    return (ctx.final_status=="completed"||ctx.final_status=="plan_ready")?0:1;
}
