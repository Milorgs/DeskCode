#include "win.hpp"
#include "tasks.hpp"
#include <cstdio>
#include <cstring>
namespace {
struct Job {
    int id=0;bool active=true,alive=true,cancelled=false,truncated=false;std::uint64_t dropped=0;
    bool retired=false;double retired_at=0;
    bool thinking=false;double thinking_started=0,changed_at=0,turn_started=0;
    std::uint64_t tokens=0;int context=-1;bool shell_approval=false;std::string detail,pending_events;
    TokenUsage turn_usage{},session_usage{};
    std::string title,workspace,state="执行中",text;
    WinHandle process,group,feed,approval,logs,events,cancellation;
    std::mutex mutex;std::jthread reader;
};
void bounded_output(Job &job,const char *bytes,size_t count){
    job.text.append(bytes,count);if(job.text.size()>65536){size_t cut=job.text.size()-65536;while(cut<job.text.size()&&(static_cast<unsigned char>(job.text[cut])&0xc0)==0x80)cut++;job.text.erase(0,cut);job.truncated=true;job.dropped+=cut;}
}
void pump(Job &job){
    auto drain=[&](HANDLE handle,bool events){for(int i=0;i<16;i++){
        DWORD available=0;if(!PeekNamedPipe(handle,nullptr,0,nullptr,&available,nullptr)||!available)break;
        char bytes[4096];DWORD count=0;if(!ReadFile(handle,bytes,(std::min)(available,static_cast<DWORD>(sizeof bytes)),&count,nullptr)||!count)break;
        std::lock_guard lock(job.mutex);
        if(!events){bounded_output(job,bytes,count);continue;}
        job.pending_events.append(bytes,count);
        size_t used=0;
        for(size_t k=0;k<job.pending_events.size();k++){
            char event=job.pending_events[k];
            if(event=='k'){
                if(k+9>job.pending_events.size())break;
                std::uint64_t value=0;memcpy(&value,job.pending_events.data()+k+1,sizeof value);
                job.tokens=value;k+=8;used=k+1;continue;
            }
            if(event=='x'){
                if(k+2>job.pending_events.size())break;
                job.context=static_cast<unsigned char>(job.pending_events[k+1]);
                k+=1;used=k+1;continue;
            }
            if(event=='u'){
                if(k+USAGE_EVENT_SIZE>job.pending_events.size())break;
                parse_usage_event(job.pending_events.data()+k,job.turn_usage,job.session_usage);
                k+=USAGE_EVENT_SIZE-1;used=k+1;continue;
            }
            if(event=='p'){
                if(k+2>job.pending_events.size())break;
                size_t plen=static_cast<unsigned char>(job.pending_events[k+1]);
                if(k+2+plen>job.pending_events.size())break;
                job.detail.assign(job.pending_events.data()+k+2,plen);
                k+=1+plen;used=k+1;continue;
            }
            used=k+1;
            job.changed_at=monotime();
            if(event=='t'){job.thinking=true;job.thinking_started=monotime();job.detail.clear();continue;}
            if(event=='e'){job.thinking=false;continue;}
            job.thinking=false;
            job.detail.clear();
            if(event=='d'){job.state="已完成";job.active=false;job.shell_approval=false;}
            else if(event=='f'){job.state="失败";job.active=false;job.shell_approval=false;}
            else if(event=='c'){job.state="已取消";job.active=false;job.shell_approval=false;}
            else if(event=='a'){job.state="等待确认";job.shell_approval=false;}
            else if(event=='A'){job.state="等待确认";job.shell_approval=true;}
            else if(event=='q')job.state="等待资源";
            else if(event=='r'){job.state="执行中";job.shell_approval=false;}
        }
        job.pending_events.erase(0,used);
    }};
    drain(job.logs.get(),false);drain(job.events.get(),true);
}
struct Lines {
    HANDLE handle;std::string pending;
    bool next(std::string &line){
        for(;;){
            auto end=pending.find('\n');if(end!=std::string::npos){line=pending.substr(0,end);pending.erase(0,end+1);return true;}
            if(pending.size()>256000||interrupted==SIGTERM)return false;
            DWORD available=0;if(!PeekNamedPipe(handle,nullptr,0,nullptr,&available,nullptr))return false;
            if(!available){Sleep(20);continue;}char bytes[4096];DWORD count=0;
            if(!ReadFile(handle,bytes,(std::min)(available,static_cast<DWORD>(sizeof bytes)),&count,nullptr)||!count)return false;pending.append(bytes,count);
        }
    }
};
}
void task_event(App &app,char event){if(app.task_events>=0){DWORD count=0;WriteFile(reinterpret_cast<HANDLE>(app.task_events),&event,1,&count,nullptr);}}
void task_tokens(App &app){
    if(app.task_events<0)return;
    char message[9];message[0]='k';
    std::uint64_t value=app.task_tokens;memcpy(message+1,&value,sizeof value);
    DWORD count=0;WriteFile(reinterpret_cast<HANDLE>(app.task_events),message,sizeof message,&count,nullptr);
}
void task_context(App &app,int percent){
    if(app.task_events<0)return;
    const char message[2]={'x',static_cast<char>(std::clamp(percent,0,100))};
    DWORD count=0;WriteFile(reinterpret_cast<HANDLE>(app.task_events),message,sizeof message,&count,nullptr);
}
void task_usage(App &app){
    if(app.task_events<0)return;
    const auto message=usage_event(app);
    DWORD count=0;WriteFile(reinterpret_cast<HANDLE>(app.task_events),message.data(),static_cast<DWORD>(message.size()),&count,nullptr);
}
void task_detail(App &app,std::string_view detail){
    if(app.task_events<0)return;
    size_t len=(std::min)(detail.size(),static_cast<size_t>(96));
    while(len>0&&len<detail.size()&&(static_cast<unsigned char>(detail[len])&0xc0)==0x80)--len;
    char message[98];
    message[0]='p';
    message[1]=static_cast<char>(len);
    if(len)memcpy(message+2,detail.data(),len);
    DWORD count=0;WriteFile(reinterpret_cast<HANDLE>(app.task_events),message,static_cast<DWORD>(2+len),&count,nullptr);
}
ResourceLock::ResourceLock(App &app,bool desktop){
    std::string directory=desktop?app.config_dir:std::string(app.root)+"/.desk-agent";
    if(mkdirs(directory.c_str(),0700))return;
    auto path=fs_path(directory+(desktop?"/desktop.lock":"/operations.lock"));
    WinHandle handle(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL,nullptr));if(!handle)return;
    bool waiting=false;
    while(!interrupted){
        OVERLAPPED offset{};
        if(LockFileEx(handle.get(),LOCKFILE_EXCLUSIVE_LOCK|LOCKFILE_FAIL_IMMEDIATELY,0,1,0,&offset)){fd=reinterpret_cast<std::intptr_t>(handle.release());if(waiting)task_event(app,'r');return;}
        if(GetLastError()!=ERROR_LOCK_VIOLATION)break;
        if(app.task_events<0){puts(tr("资源正被任务占用，请返回对应对话处理待批准操作。"));break;}
        if(!waiting){puts(tr(desktop?"等待其他任务释放桌面…":"等待同一工作目录的其他操作结束…"));fflush(stdout);task_event(app,'q');waiting=true;}
        Sleep(50);
    }
}
ResourceLock::ResourceLock(const char *config_dir){
    auto path=fs_path(std::string(config_dir)+"/token-usage.lock");
    WinHandle handle(CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,
        FILE_SHARE_READ|FILE_SHARE_WRITE,nullptr,OPEN_ALWAYS,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!handle||path_link(path))return;
    OVERLAPPED offset{};
    if(LockFileEx(handle.get(),LOCKFILE_EXCLUSIVE_LOCK,0,1,0,&offset))fd=reinterpret_cast<std::intptr_t>(handle.release());
}
ResourceLock::~ResourceLock(){if(fd>=0){HANDLE handle=reinterpret_cast<HANDLE>(fd);OVERLAPPED offset{};UnlockFileEx(handle,0,1,0,&offset);CloseHandle(handle);}}
struct Tasks::Impl {
    std::vector<std::unique_ptr<Job>> jobs;int next=1;std::wstring executable;
    Job *find(int id){for(auto &job:jobs)if(job->id==id&&!job->retired)return job.get();return nullptr;}
};
Tasks::Tasks(std::string executable):impl(std::make_unique<Impl>()){
    if(executable=="/proc/self/exe"){
        std::vector<wchar_t> path(32768);DWORD size=GetModuleFileNameW(nullptr,path.data(),static_cast<DWORD>(path.size()));if(!size||size>=path.size())throw std::runtime_error("Executable path unavailable");impl->executable.assign(path.data(),size);
    }else impl->executable=wide(path_text(fs::absolute(fs_path(executable))));
    impl->jobs.reserve(32);
}
Tasks::~Tasks(){
    for(auto &job:impl->jobs){SetEvent(job->cancellation.get());job->feed.reset();}
    double until=monotime()+10;
    while(monotime()<until){bool alive=false;for(auto &job:impl->jobs)if(WaitForSingleObject(job->process.get(),0)!=WAIT_OBJECT_0)alive=true;if(!alive)break;Sleep(20);}
    for(auto &job:impl->jobs){if(WaitForSingleObject(job->process.get(),0)!=WAIT_OBJECT_0)TerminateJobObject(job->group.get(),1);job->reader.request_stop();}
    for(auto &job:impl->jobs)if(job->reader.joinable())job->reader.join();
}
void Tasks::refresh(){
    for(auto &job:impl->jobs){
        if(job->retired&&monotime()-job->retired_at>10&&WaitForSingleObject(job->process.get(),0)!=WAIT_OBJECT_0)TerminateJobObject(job->group.get(),1);
        if(WaitForSingleObject(job->process.get(),0)==WAIT_OBJECT_0){
            std::lock_guard lock(job->mutex);if(!job->alive)continue;job->alive=false;job->changed_at=monotime();job->thinking=false;
            DWORD code=1;GetExitCodeProcess(job->process.get(),&code);
            if(job->active)job->state=job->cancelled?"已取消":code==0?"已完成":"失败";job->active=false;
        }
    }
    std::erase_if(impl->jobs,[](auto &job){
        if(!job->retired||WaitForSingleObject(job->process.get(),0)!=WAIT_OBJECT_0)return false;
        job->reader.request_stop();if(job->reader.joinable())job->reader.join();return true;
    });
}
int Tasks::start(App &app,const std::string &task,const std::string &workspace){
    refresh();size_t active=0;for(auto &job:impl->jobs){std::lock_guard lock(job->mutex);if(job->active)active++;}
    if(task.empty()||task.size()>65536||active>=4||std::count_if(impl->jobs.begin(),impl->jobs.end(),[](const auto &job){return !job->retired;})>=32){seterr(&app,tr("任务为空、过长或达到上限（同时 4 个，最多保留 32 个）。"));return -1;}
    if(config_valid(&app,app.config))return -1;
    char root[PATH_MAX];if(!canonical_directory(workspace.empty()?app.root:workspace.c_str(),root)){seterr(&app,tr("工作目录不存在。"));return -1;}
    auto job=std::make_unique<Job>();job->id=impl->next;job->title=task;job->workspace=root;job->group=kill_job();job->changed_at=monotime();job->turn_started=monotime();
    WinHandle payload_read,approval_read,logs_write,events_write;
    if(!job->group||!win_pipe(payload_read,job->feed)||!win_pipe(approval_read,job->approval)||!win_pipe(job->logs,logs_write)||!win_pipe(job->events,events_write)){seterr(&app,tr("无法创建任务管道。"));return -1;}
    SECURITY_ATTRIBUTES attr{sizeof attr,nullptr,TRUE};job->cancellation.reset(CreateEventW(&attr,TRUE,FALSE,nullptr));if(!job->cancellation){seterr(&app,tr("无法创建状态管道。"));return -1;}
    for(HANDLE handle:{job->feed.get(),job->approval.get(),job->logs.get(),job->events.get()})SetHandleInformation(handle,HANDLE_FLAG_INHERIT,0);
    JsonPtr data(obj());
    put(data.get(),"conversation_id",str(app.conversation_id.c_str()));put(data.get(),"config",copy(app.config));put(data.get(),"language",str(language_code()));put(data.get(),"root",str(root));put(data.get(),"config_dir",str(app.config_dir));put(data.get(),"conversation",json_object_new_boolean(app.conversation_worker));put(data.get(),"task",str(task.c_str()));
    put(data.get(),"yes",json_object_new_boolean(app.yes));put(data.get(),"auto_edit",json_object_new_boolean(app.auto_edit));put(data.get(),"guided_approval",json_object_new_boolean(app.guided_approval));
    put(data.get(),"columns",json_object_new_int(terminal_columns()));put(data.get(),"ansi",json_object_new_boolean(terminal_supports_ansi()));
    put(data.get(),"shell",json_object_new_boolean(app.shell));put(data.get(),"desktop",json_object_new_boolean(app.desktop));put(data.get(),"record",json_object_new_boolean(app.record));put(data.get(),"verbose",json_object_new_boolean(app.verbose));put(data.get(),"max_steps",json_object_new_int(app.max_steps));
    put(data.get(),"plan_mode",json_object_new_boolean(app.plan_mode));
    put(data.get(),"events",json_object_new_int64(reinterpret_cast<std::intptr_t>(events_write.get())));put(data.get(),"cancel",json_object_new_int64(reinterpret_cast<std::intptr_t>(job->cancellation.get())));
    std::string body=json_object_to_json_string_ext(data.get(),JSON_C_TO_STRING_PLAIN);body+='\n';if(body.size()>250000){seterr(&app,tr("任务配置过大。"));return -1;}
    auto args=quote_arg(impl->executable)+L" --task-worker "+std::to_wstring(reinterpret_cast<std::intptr_t>(payload_read.get()));
    if(!win_spawn(impl->executable,std::move(args),wide(app.root),approval_read.get(),logs_write.get(),{payload_read.get(),approval_read.get(),logs_write.get(),events_write.get(),job->cancellation.get()},job->group.get(),job->process)){seterr(&app,tr("无法启动任务。"));return -1;}
    payload_read.reset();approval_read.reset();logs_write.reset();events_write.reset();
    Job *pointer=job.get();job->reader=std::jthread([pointer](std::stop_token stop){while(!stop.stop_requested()){pump(*pointer);Sleep(10);}pump(*pointer);});
    if(!win_write(job->feed.get(),body)){TerminateJobObject(job->group.get(),1);job->reader.request_stop();job->reader.join();seterr(&app,tr("无法传递任务配置。"));return -1;}
    int id=impl->next++;impl->jobs.push_back(std::move(job));return id;
}
J *Tasks::snapshot(){
    refresh();J *items=arr();
    for(auto &job:impl->jobs){
        if(job->retired)continue;
        std::lock_guard lock(job->mutex);
        J *entry=obj();
        put(entry,"id",json_object_new_int(job->id));
        put(entry,"active",json_object_new_boolean(job->active));
        put(entry,"task",str(job->title.c_str()));
        put(entry,"state",str(tr(job->active&&job->cancelled?"正在停止":job->state.c_str())));
        put(entry,"workspace",str(job->workspace.c_str()));
        put(entry,"awaiting_approval",json_object_new_boolean(job->active&&!job->cancelled&&job->state=="等待确认"));
        put(entry,"shell_approval",json_object_new_boolean(job->active&&!job->cancelled&&job->state=="等待确认"&&job->shell_approval));
        put(entry,"detail",str(job->detail.c_str()));
        put(entry,"changed_at",json_object_new_double(job->changed_at));
        bool thinking=job->thinking&&job->active&&!job->cancelled;
        put(entry,"thinking",json_object_new_boolean(thinking));
        put(entry,"thinking_seconds",json_object_new_double(thinking?monotime()-job->thinking_started:0));
        put(entry,"turn_seconds",json_object_new_double(job->active&&job->turn_started>0?monotime()-job->turn_started:0));
        put(entry,"tokens",json_object_new_int64(static_cast<int64_t>(job->tokens)));
        put(entry,"context_percent",json_object_new_int(job->context));
        put_usage(entry,job->turn_usage,job->session_usage);
        add(items,entry);
    }
    return items;
}
bool Tasks::cancel(int id){refresh();Job *job=impl->find(id);if(!job)return false;std::lock_guard lock(job->mutex);if(!job->active)return false;job->cancelled=true;job->state="正在停止";job->changed_at=monotime();return SetEvent(job->cancellation.get())!=FALSE;}
bool Tasks::remove(int id){
    refresh();Job *job=impl->find(id);if(!job)return false;
    {std::lock_guard lock(job->mutex);job->retired=true;job->retired_at=monotime();job->cancelled=true;}
    SetEvent(job->cancellation.get());job->feed.reset();refresh();return true;
}
bool Tasks::resume(int id,const std::string &task,J *settings){
    refresh();if(task.empty()||task.size()>65536)return false;size_t active=0;for(auto &job:impl->jobs){std::lock_guard lock(job->mutex);if(job->active)active++;}if(active>=4)return false;
    Job *job=impl->find(id);if(!job)return false;std::lock_guard lock(job->mutex);if(job->active||!job->alive)return false;
    JsonPtr message(obj());if(settings)put(message.get(),"settings",copy(settings));put(message.get(),"task",str(task.c_str()));put(message.get(),"language",str(language_code()));put(message.get(),"columns",json_object_new_int(terminal_columns()));
    std::string bytes=json_object_to_json_string_ext(message.get(),JSON_C_TO_STRING_PLAIN);bytes+='\n';
    if(bytes.size()>250000)return false;ResetEvent(job->cancellation.get());
    if(!win_write(job->feed.get(),bytes))return false;
    job->active=true;job->cancelled=false;job->state="执行中";job->title=task;job->changed_at=monotime();job->turn_started=monotime();job->tokens=0;job->turn_usage=TokenUsage{};
    return true;
}
// Detaching a running command needs a signal the Windows worker does not have;
// run_shell's background option covers long-running commands there.
bool Tasks::background(int){return false;}
std::string Tasks::output(int id,bool *truncated,std::uint64_t *dropped){if(dropped)*dropped=0;Job *job=impl->find(id);if(!job)return {};std::lock_guard lock(job->mutex);if(truncated)*truncated=job->truncated;if(dropped)*dropped=job->dropped;return job->text;}
bool Tasks::decide(int id,bool allow){Job *job=impl->find(id);if(!job)return false;std::lock_guard lock(job->mutex);return job->active&&job->state=="等待确认"&&win_write(job->approval.get(),allow?"1\n":"2\n");}
char *Tasks::readline(const char *prompt){refresh();if(on_idle)on_idle();return input(prompt,nullptr,false);}
bool Tasks::respond(int id,int answer){
    Job *job=impl->find(id);if(!job||answer<0)return false;
    std::lock_guard lock(job->mutex);
    if(!job->active||job->state!="等待确认"||answer>(job->shell_approval?4:3))return false;
    if(!win_write(job->approval.get(),std::to_string(answer)+"\n"))return false;
    job->state="执行中";job->shell_approval=false;return true;
}
int task_worker(std::intptr_t raw){
    WinHandle control(reinterpret_cast<HANDLE>(raw));Lines source{control.get()};std::string line;if(!source.next(line))return 1;
    JsonPtr data(json_tokener_parse(line.c_str()));if(!data)return 1;set_language(sget(data.get(),"language"));
    App app;app.conversation_worker=bget(data.get(),"conversation");app.guided_approval=bget(data.get(),"guided_approval");
    set_worker_terminal(iget(data.get(),"columns",0),bget(data.get(),"ansi"));
    app.config=copy(get(data.get(),"config"));snprintf(app.root,sizeof app.root,"%s",sget(data.get(),"root"));snprintf(app.config_dir,sizeof app.config_dir,"%s",sget(data.get(),"config_dir"));
    app.yes=bget(data.get(),"yes");app.auto_edit=bget(data.get(),"auto_edit");app.shell=bget(data.get(),"shell");app.desktop=bget(data.get(),"desktop");app.record=bget(data.get(),"record");app.verbose=bget(data.get(),"verbose");app.max_steps=iget(data.get(),"max_steps",0);app.task_events=static_cast<std::intptr_t>(json_object_get_int64(get(data.get(),"events")));
    app.conversation_id=sget(data.get(),"conversation_id");
    app.plan_mode=bget(data.get(),"plan_mode");
    if(!app.conversation_id.empty())app.messages=history_context(app,app.conversation_id);
    WinHandle events(reinterpret_cast<HANDLE>(app.task_events)),cancel(reinterpret_cast<HANDLE>(static_cast<std::intptr_t>(json_object_get_int64(get(data.get(),"cancel")))));
    SetHandleInformation(control.get(),HANDLE_FLAG_INHERIT,0);SetHandleInformation(events.get(),HANDLE_FLAG_INHERIT,0);SetHandleInformation(cancel.get(),HANDLE_FLAG_INHERIT,0);
    auto cleanup=[](App *value){app_cleanup(value);};std::unique_ptr<App,decltype(cleanup)> guard(&app,cleanup);
    std::atomic<bool> running{false};std::mutex cancellation_guard;std::jthread watcher([&](std::stop_token stop){while(!stop.stop_requested()){{std::lock_guard lock(cancellation_guard);if(running&&WaitForSingleObject(cancel.get(),0)==WAIT_OBJECT_0)interrupted=SIGINT;}Sleep(20);}});
    std::string task=sget(data.get(),"task");
    for(;;){
        {std::lock_guard lock(cancellation_guard);interrupted=0;running=true;if(WaitForSingleObject(cancel.get(),0)==WAIT_OBJECT_0)interrupted=SIGINT;}
        int rc=run_task(&app,task.c_str());{std::lock_guard lock(cancellation_guard);running=false;ResetEvent(cancel.get());}
        if(rc)fprintf(stderr,"%s\n",app.error);fflush(nullptr);desktop_clear(&app);
        task_event(app,interrupted||!strcmp(app.state,"已取消")?'c':rc?'f':'d');
        if(!source.next(line))break;JsonPtr next(json_tokener_parse(line.c_str()));if(!next)break;
        if(get(next.get(),"columns"))set_worker_terminal(iget(next.get(),"columns",0),bget(data.get(),"ansi"));
        apply_conversation_settings(app,get(next.get(),"settings"));set_language(sget(next.get(),"language"));task=sget(next.get(),"task");if(task.empty())break;
    }
    watcher.request_stop();return 0;
}
