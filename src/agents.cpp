// Subagents (.deskcode/agents/*.md, .claude/agents/*.md, plugins) with optional
// background parallel execution and isolated git worktrees.
#include "desk.hpp"
#include "tasks.hpp"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>
#ifdef _WIN32
#include "windows/win.hpp"
#include <cwctype>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
constexpr size_t AGENT_FILE_LIMIT=262144;
constexpr size_t MAX_BG_SUBAGENTS=4;

std::string trim_str(std::string_view text){
    size_t a=0,b=text.size();
    while(a<b&&isspace(static_cast<unsigned char>(text[a])))a++;
    while(b>a&&isspace(static_cast<unsigned char>(text[b-1])))b--;
    return std::string(text.substr(a,b-a));
}
std::string lower_str(std::string text){
    for(char &c:text)c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}
bool truthy_str(std::string_view v){
    std::string s=lower_str(trim_str(v));
    return s=="1"||s=="true"||s=="yes"||s=="on";
}
bool valid_agent_name(std::string_view name){
    if(name.empty()||name.size()>64)return false;
    return std::all_of(name.begin(),name.end(),[](char c){
        return isalnum(static_cast<unsigned char>(c))||c=='-'||c=='_'||c=='.'||c==':';
    });
}
std::string slug_of(std::string_view name){
    std::string s;
    for(char c:name){
        if(isalnum(static_cast<unsigned char>(c))||c=='-'||c=='_')s+=static_cast<char>(tolower(static_cast<unsigned char>(c)));
        else if(c==':'||c=='.')s+='-';
    }
    if(s.empty())s="agent";
    if(s.size()>24)s.resize(24);
    return s;
}
std::string read_utf8_file(const std::string &path,size_t limit){
    if(path_link(fs_path(path)))return {};
    size_t length=0;
    std::unique_ptr<char,decltype(&free)> bytes(read_bytes(path.c_str(),limit,&length),free);
    if(!bytes||memchr(bytes.get(),0,length))return {};
    std::string text(bytes.get(),length);
    if(text.starts_with("\xEF\xBB\xBF"))text.erase(0,3);
    return valid_utf8(text)?text:std::string();
}
std::string first_summary_line(std::string_view body){
    for(size_t start=0;start<body.size();){
        auto end=body.find('\n',start);
        std::string line=trim_str(body.substr(start,end==std::string_view::npos?std::string_view::npos:end-start));
        start=end==std::string_view::npos?body.size():end+1;
        while(!line.empty()&&line.front()=='#')line.erase(0,1);
        line=trim_str(line);
        if(!line.empty()){
            if(line.size()>180)line.resize(180);
            return line;
        }
    }
    return {};
}
bool user_agents_enabled(){
    const char *off=env_value("DESK_NO_USER_SKILLS");
    return !(off&&*off&&strcmp(off,"0"));
}
std::string user_home(){
#ifdef _WIN32
    const char *h=env_value("USERPROFILE");
#else
    const char *h=env_value("HOME");
#endif
    return h?h:"";
}

// Map both DeskCode tool names and Claude Code tool names to canonical DeskCode names.
void add_mapped_tools(std::string_view token,std::vector<std::string> &out){
    const std::string low=lower_str(trim_str(token));
    if(low.empty())return;
    auto push=[&](const char *name){
        if(std::find(out.begin(),out.end(),name)==out.end())out.emplace_back(name);
    };
    if(low=="read"||low=="read_file")push("read_file");
    else if(low=="ls"||low=="list"||low=="list_files")push("list_files");
    else if(low=="glob"){push("list_files");push("search_text");}
    else if(low=="grep"||low=="search"||low=="search_text")push("search_text");
    else if(low=="outline"||low=="code_outline")push("code_outline");
    else if(low=="lsp")push("lsp");
    else if(low=="edit"||low=="edit_file")push("edit_file");
    else if(low=="multiedit"||low=="multi_edit")push("multi_edit");
    else if(low=="write"||low=="write_file")push("write_file");
    else if(low=="bash"||low=="shell"||low=="run_shell"){push("run_shell");push("shell_output");push("shell_kill");}
    else if(low=="shell_output")push("shell_output");
    else if(low=="shell_kill")push("shell_kill");
    else if(low=="webfetch"||low=="web_fetch")push("web_fetch");
    else if(low=="websearch"||low=="web_search")push("web_search");
    else if(low=="todowrite"||low=="todo_write")push("todo_write");
    else if(low=="skill")push("skill");
}
std::vector<std::string> parse_tools_spec(std::string_view raw){
    std::string cleaned(raw);
    if(!cleaned.empty()&&cleaned.front()=='['&&cleaned.back()==']')cleaned=cleaned.substr(1,cleaned.size()-2);
    std::vector<std::string> tools;
    for(size_t start=0;start<=cleaned.size();){
        auto comma=cleaned.find(',',start);
        std::string part=trim_str(std::string_view(cleaned).substr(start,comma==std::string::npos?std::string::npos:comma-start));
        if(part.size()>=2&&((part.front()=='"'&&part.back()=='"')||(part.front()=='\''&&part.back()=='\'')))
            part=part.substr(1,part.size()-2);
        add_mapped_tools(part,tools);
        if(comma==std::string::npos)break;
        start=comma+1;
    }
    return tools;
}
bool any_writable_tool(const std::vector<std::string> &tools){
    for(const auto &t:tools){
        if(t=="edit_file"||t=="multi_edit"||t=="write_file"||t=="run_shell"||t=="shell_kill")return true;
    }
    return false;
}

bool parse_agent_file(const fs::path &file,const std::string &scope,const std::string &prefix,SubagentInfo &out){
    const std::string text=read_utf8_file(path_text(file),AGENT_FILE_LIMIT);
    if(text.empty())return false;
    std::map<std::string,std::string> fields;
    std::string_view body=text;
    parse_frontmatter(text,fields,body);
    std::string stem=path_text(file.stem());
    std::string base=fields.count("name")&&!fields["name"].empty()?fields["name"]:stem;
    std::string full_name=prefix.empty()?base:(prefix+":"+base);
    if(!valid_agent_name(full_name))return false;
    out=SubagentInfo{};
    out.name=std::move(full_name);
    out.description=fields.count("description")?fields["description"]:first_summary_line(body);
    out.instructions=trim_str(body);
    out.scope=scope;
    out.file=path_text(file);
    if(fields.count("model"))out.model=fields["model"];
    if(fields.count("tools"))out.tools=parse_tools_spec(fields["tools"]);
    if(fields.count("readonly"))out.readonly=truthy_str(fields["readonly"]);
    else if(fields.count("tools")&&!out.tools.empty())out.readonly=!any_writable_tool(out.tools);
    else out.readonly=false;
    if(fields.count("isolation"))out.worktree=(lower_str(trim_str(fields["isolation"]))=="worktree");
    else if(fields.count("worktree"))out.worktree=truthy_str(fields["worktree"]);
    int default_steps=out.readonly?10:16;
    if(fields.count("max_steps")&&!fields["max_steps"].empty()){
        long v=strtol(fields["max_steps"].c_str(),nullptr,10);
        if(v>=1&&v<=40)default_steps=static_cast<int>(v);
    }
    out.max_steps=default_steps;
    return !out.instructions.empty();
}

// Run git with an explicit argv in `cwd`, capturing combined stdout/stderr.
int run_git(const std::string &cwd,const std::vector<std::string> &args,std::string &output,const std::string &stdin_data={}){
    output.clear();
#ifdef _WIN32
    wchar_t git_buf[32768];
    DWORD found=SearchPathW(nullptr,L"git",L".exe",32768,git_buf,nullptr);
    if(!found||found>=32768)return -1;
    std::wstring exe(git_buf,found),cmdline=quote_arg(exe);
    for(const auto &arg:args)cmdline+=L" "+quote_arg(wide(arg));
    WinHandle child_in,parent_in,parent_out,child_out;
    if(!win_pipe(child_in,parent_in)||!win_pipe(parent_out,child_out))return -1;
    SetHandleInformation(parent_in.get(),HANDLE_FLAG_INHERIT,0);
    SetHandleInformation(parent_out.get(),HANDLE_FLAG_INHERIT,0);
    WinHandle job=kill_job();
    if(!job)return -1;
    WinHandle process;
    std::vector<HANDLE> inherit{child_in.get(),child_out.get()};
    if(!win_spawn(exe,cmdline,wide(cwd),child_in.get(),child_out.get(),inherit,job.get(),process))return -1;
    child_in.reset();child_out.reset();
    if(!stdin_data.empty())win_write(parent_in.get(),stdin_data);
    parent_in.reset();
    char buf[4096];
    DWORD count=0;
    while(ReadFile(parent_out.get(),buf,sizeof buf,&count,nullptr)&&count>0){
        if(output.size()<262144)output.append(buf,count);
    }
    WaitForSingleObject(process.get(),15000);
    DWORD exit_code=1;
    GetExitCodeProcess(process.get(),&exit_code);
    return static_cast<int>(exit_code);
#else
    int in_pipe[2],out_pipe[2];
    if(pipe(in_pipe))return -1;
    if(pipe(out_pipe)){close(in_pipe[0]);close(in_pipe[1]);return -1;}
    pid_t child=fork();
    if(child<0){
        close(in_pipe[0]);close(in_pipe[1]);close(out_pipe[0]);close(out_pipe[1]);
        return -1;
    }
    if(child==0){
        dup2(in_pipe[0],STDIN_FILENO);
        dup2(out_pipe[1],STDOUT_FILENO);
        dup2(out_pipe[1],STDERR_FILENO);
        close(in_pipe[0]);close(in_pipe[1]);close(out_pipe[0]);close(out_pipe[1]);
        if(!cwd.empty()&&chdir(cwd.c_str())!=0)_exit(126);
        setenv("GIT_TERMINAL_PROMPT","0",1);
        std::vector<char *> argv;
        argv.push_back(const_cast<char *>("git"));
        for(const auto &arg:args)argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        execvp("git",argv.data());
        _exit(127);
    }
    close(in_pipe[0]);close(out_pipe[1]);
    if(!stdin_data.empty()){
        size_t off=0;
        while(off<stdin_data.size()){
            ssize_t w=write(in_pipe[1],stdin_data.data()+off,stdin_data.size()-off);
            if(w<=0){if(w<0&&errno==EINTR)continue;break;}
            off+=static_cast<size_t>(w);
        }
    }
    close(in_pipe[1]);
    char buf[4096];
    for(;;){
        ssize_t n=read(out_pipe[0],buf,sizeof buf);
        if(n<0&&errno==EINTR)continue;
        if(n<=0)break;
        if(output.size()<262144)output.append(buf,static_cast<size_t>(n));
    }
    close(out_pipe[0]);
    int status=0;
    while(waitpid(child,&status,0)<0)if(errno!=EINTR)return -1;
    return WIFEXITED(status)?WEXITSTATUS(status):-1;
#endif
}

void ensure_git_excludes_desk_state(const std::string &repo_root){
    std::string rel;
    if(run_git(repo_root,{"rev-parse","--git-path","info/exclude"},rel)!=0)return;
    rel=trim_str(rel);
    if(rel.empty())return;
    fs::path p=fs_path(rel);
    if(!p.is_absolute())p=fs_path(repo_root)/p;
    std::error_code ec;
    fs::create_directories(p.parent_path(),ec);
    std::string cur=read_utf8_file(path_text(p),65536);
    if(cur.find(".desk-agent/")!=std::string::npos)return;
    if(!cur.empty()&&cur.back()!='\n')cur+='\n';
    cur+=".desk-agent/\n";
    atomic_file(path_text(p).c_str(),cur.data(),cur.size(),0600);
}

struct WorktreeHandle {
    std::string repo_root;
    std::string wt_dir;
    std::string branch;
    std::string base_sha;
    bool active=false;
};

bool create_worktree(const std::string &repo_root,const std::string &agent_name,const std::string &agent_id,
                     WorktreeHandle &wt,std::string &error_out){
    std::string head;
    if(run_git(repo_root,{"rev-parse","--verify","HEAD"},head)!=0){
        error_out=tr("当前目录不是含提交记录的 Git 仓库，无法创建独立 worktree。");
        return false;
    }
    wt.repo_root=repo_root;
    wt.base_sha=trim_str(head);
    ensure_git_excludes_desk_state(repo_root);
    char rnd[33];id_string(rnd);
    const std::string short_id(rnd,8);
    wt.branch="desk/"+slug_of(agent_name)+"-"+short_id;
    const std::string parent_wt=repo_root+"/.desk-agent/worktrees";
    if(mkdirs(parent_wt.c_str(),0700)){
        error_out=tr("无法创建 worktree 临时目录。");
        return false;
    }
    const std::string raw_wt=parent_wt+"/"+agent_id+"-"+short_id;
    std::string git_out;
    if(run_git(repo_root,{"worktree","add","-q","-b",wt.branch,raw_wt,"HEAD"},git_out)!=0){
        error_out=std::string(tr("git worktree 创建失败："))+trim_str(git_out);
        return false;
    }
    char canon[PATH_MAX];
    wt.wt_dir=canonical_directory(raw_wt.c_str(),canon)?std::string(canon):raw_wt;
    wt.active=true;
    return true;
}

struct WorktreeOutcome {
    std::string branch;
    std::string commit;
    std::string diff_stat;
    std::vector<std::string> changed_files;
};

WorktreeOutcome finalize_worktree(WorktreeHandle &wt,const std::string &task_summary,bool aborted){
    WorktreeOutcome out;
    if(!wt.active)return out;
    wt.active=false;
    std::error_code ec;
    fs::remove_all(fs_path(wt.wt_dir)/".desk-agent",ec);
    std::string tmp;
    if(!aborted){
        std::string porcelain;
        if(run_git(wt.wt_dir,{"status","--porcelain","--untracked-files=all"},porcelain)==0&&!trim_str(porcelain).empty()){
            run_git(wt.wt_dir,{"add","-A"},tmp);
            std::string first_line=trim_str(task_summary);
            auto nl=first_line.find('\n');
            if(nl!=std::string::npos)first_line.resize(nl);
            if(first_line.empty())first_line="DeskCode subagent changes";
            if(first_line.size()>72)first_line=first_line.substr(0,69)+"...";
            run_git(wt.wt_dir,{"-c","user.name=DeskCode Subagent","-c","user.email=deskcode@users.noreply.github.com",
                               "-c","commit.gpgsign=false","commit","-q","--no-verify","-m",first_line},tmp);
        }
        std::string head_now;
        if(run_git(wt.wt_dir,{"rev-parse","--verify","HEAD"},head_now)==0){
            head_now=trim_str(head_now);
            if(!head_now.empty()&&head_now!=wt.base_sha){
                out.branch=wt.branch;
                out.commit=head_now;
                std::string names;
                if(run_git(wt.wt_dir,{"diff","--name-only",wt.base_sha+".."+head_now},names)==0){
                    for(size_t s=0;s<names.size();){
                        auto e=names.find('\n',s);
                        std::string f=trim_str(std::string_view(names).substr(s,e==std::string::npos?std::string::npos:e-s));
                        if(!f.empty()&&out.changed_files.size()<100)out.changed_files.push_back(std::move(f));
                        if(e==std::string::npos)break;
                        s=e+1;
                    }
                }
                std::string stat_txt;
                if(run_git(wt.wt_dir,{"diff","--stat",wt.base_sha+".."+head_now},stat_txt)==0)
                    out.diff_stat=trim_str(stat_txt);
            }
        }
    }
    run_git(wt.repo_root,{"worktree","remove","--force",wt.wt_dir},tmp);
    fs::remove_all(fs_path(wt.wt_dir),ec);
    if(out.branch.empty())run_git(wt.repo_root,{"branch","-D",wt.branch},tmp);
    return out;
}

struct SubagentRunSpec {
    std::string id;
    SubagentInfo agent;
    std::string prompt;
    std::string parent_root;
    std::string config_dir;
    JsonPtr store;
    JsonPtr config;
    bool shell=false;
    bool sandbox=true;
    bool use_worktree=false;
    WorktreeHandle wt;
};

struct SubagentJob {
    std::string id;
    std::string agent_name;
    std::string initial_branch;
    std::string initial_worktree;
    std::atomic<bool> cancel{false};
    std::mutex mutex;
    std::condition_variable cv;
    bool finished=false;
    bool tokens_harvested=false;
    JsonPtr result;
    std::uint64_t total_tokens=0;
    TokenUsage usage{};
    std::thread worker;
};

struct SubagentManager {
    int counter=0;
    std::map<std::string,std::shared_ptr<SubagentJob>> jobs;
};

SubagentManager &manager_of(App &app){
    if(!app.subagents_data)app.subagents_data=new SubagentManager;
    return *static_cast<SubagentManager *>(app.subagents_data);
}

void harvest_usage(App &parent,SubagentJob &job){
    if(job.tokens_harvested)return;
    job.tokens_harvested=true;
    if(job.total_tokens){
        parent.task_tokens=add_tokens(parent.task_tokens,job.total_tokens);
        task_tokens(parent);
    }
    parent.turn_usage.add(job.usage);
    parent.session_usage.add(job.usage);
    if(parent.turn_usage.total()||parent.session_usage.total())task_usage(parent);
}

JsonPtr run_subagent_loop(SubagentRunSpec &spec,std::atomic<bool> &cancel_flag,std::uint64_t &tokens_out,TokenUsage &usage_out){
    App sub{};
    sub.store=spec.store?copy(spec.store.get()):obj();
    sub.config=copy(spec.config.get());
    sub.messages=arr();
    const std::string active_root=spec.use_worktree?spec.wt.wt_dir:spec.parent_root;
    snprintf(sub.root,sizeof sub.root,"%s",active_root.c_str());
    snprintf(sub.config_dir,sizeof sub.config_dir,"%s",spec.config_dir.c_str());
    sub.yes=true;
    sub.shell=spec.shell;
    sub.sandbox=spec.sandbox;
    sub.desktop=false;
    sub.subagent=true;
    sub.subagent_writable=!spec.agent.readonly;
    sub.subagent_tools=spec.agent.tools;
    sub.max_steps=spec.agent.max_steps;
    sub.task_events=-1;
    sub.json_output=true;
    auto cleanup=[](App *v){app_cleanup(v);};
    std::unique_ptr<App,decltype(cleanup)> guard(&sub,cleanup);

    std::string full_prompt;
    if(!spec.agent.instructions.empty()){
        full_prompt=spec.agent.instructions+"\n\nTask:\n"+spec.prompt;
    }else if(spec.agent.readonly){
        full_prompt="You are a read-only codebase research subagent. Inspect files and return concise, factual findings with exact file paths and 1-based line numbers.\n\nTask:\n"+spec.prompt;
    }else{
        full_prompt="You are an autonomous coding subagent working inside an isolated workspace. Complete the task, verify your changes when possible, and finish with a concise summary of changed files and verification results.\n\nTask:\n"+spec.prompt;
    }
    J *u=obj();
    put(u,"role",str("user"));
    put(u,"content",str(full_prompt.c_str()));
    add(sub.messages,u);

    JsonPtr sub_tools(schemas(&sub));
    std::vector<std::string> touched;
    auto note_path=[&](const char *p){
        if(!p||!*p||touched.size()>=40)return;
        if(std::find(touched.begin(),touched.end(),p)==touched.end())touched.emplace_back(p);
    };

    std::string reply_text,fail_error;
    int steps_used=0;
    const int max_steps=std::max(1,spec.agent.max_steps);
    for(int step=1;step<=max_steps&&!interrupted&&!cancel_flag;++step){
        steps_used=step;
        JsonPtr history(context(&sub,sub_tools.get()));
        if(!history){fail_error=sub.error[0]?sub.error:tr("子任务上下文超出预算。");break;}
        JsonPtr reply(api_complete(&sub,sub.config,history.get(),sub_tools.get()));
        if(!reply){fail_error=sub.error[0]?sub.error:tr("子任务请求失败。");break;}
        J *calls=get(reply.get(),"tool_calls");
        if(!alen(calls)){
            reply_text=sget(reply.get(),"content");
            break;
        }
        if(alen(calls)>12){fail_error=tr("单轮工具数量过多。");break;}
        std::vector<JsonPtr> parsed_args;
        parsed_args.reserve(alen(calls));
        for(size_t i=0;i<alen(calls);i++){
            J *call=at(calls,i),*fn=get(call,"function");
            if(!*sget(call,"id")||!*sget(fn,"name")){fail_error=tr("工具调用格式无效。");break;}
            JsonPtr ca(parse_tool_arguments(sget(fn,"arguments")));
            if(!ca){fail_error=tr("工具参数不是完整的 JSON 对象，本轮操作未执行。");break;}
            parsed_args.push_back(std::move(ca));
        }
        if(!fail_error.empty())break;
        add(sub.messages,reply.release());
        for(size_t i=0;i<alen(calls)&&!interrupted&&!cancel_flag;i++){
            J *call=at(calls,i),*fn=get(call,"function");
            const char *cname=sget(fn,"name");
            note_path(sget(parsed_args[i].get(),"path"));
            JsonPtr res(tool_call(&sub,cname,parsed_args[i].get()));
            J *tm=obj();
            put(tm,"role",str("tool"));
            put(tm,"tool_call_id",str(sget(call,"id")));
            put(tm,"content",str(json_object_to_json_string_ext(res.get(),JSON_C_TO_STRING_PLAIN)));
            add(sub.messages,tm);
        }
    }

    const bool cancelled=interrupted||cancel_flag;
    if(!cancelled&&fail_error.empty()&&reply_text.empty()){
        J *ask=obj();
        put(ask,"role",str("user"));
        put(ask,"content",str("Summarize what you completed and verified concisely."));
        add(sub.messages,ask);
        JsonPtr history(context(&sub,nullptr));
        if(history){
            JsonPtr final_reply(api_complete(&sub,sub.config,history.get(),nullptr));
            if(final_reply)reply_text=sget(final_reply.get(),"content");
        }
    }

    tokens_out=sub.task_tokens;
    usage_out=sub.session_usage;

    // Close LSP / background shells inside the subagent BEFORE removing its worktree.
    shells_destroy(&sub);
    lsp_destroy(&sub);

    WorktreeOutcome wt_out;
    if(spec.use_worktree)wt_out=finalize_worktree(spec.wt,spec.prompt,cancelled||!fail_error.empty());

    if(cancelled)return JsonPtr(err(tr("任务已停止。")));
    if(!fail_error.empty())return JsonPtr(err(fail_error.c_str()));
    if(reply_text.empty())reply_text=wt_out.branch.empty()?"Completed.":"Completed changes on branch "+wt_out.branch+".";

    JsonPtr out(obj());
    put(out.get(),"agent_id",str(spec.id.c_str()));
    put(out.get(),"agent",str(spec.agent.name.c_str()));
    put(out.get(),"running",json_object_new_boolean(0));
    put(out.get(),"status",str("completed"));
    put(out.get(),"reply",str(reply_text.c_str()));
    put(out.get(),"steps",json_object_new_int(steps_used));
    put(out.get(),"tokens",json_object_new_int64(static_cast<int64_t>(tokens_out)));
    if(!wt_out.branch.empty()){
        put(out.get(),"branch",str(wt_out.branch.c_str()));
        put(out.get(),"commit",str(wt_out.commit.c_str()));
        if(!wt_out.diff_stat.empty())put(out.get(),"diff_stat",str(wt_out.diff_stat.c_str()));
        J *changed=arr();
        for(const auto &f:wt_out.changed_files)add(changed,str(f.c_str()));
        put(out.get(),"changed_files",changed);
    }else if(!touched.empty()){
        J *files=arr();
        for(const auto &f:touched)add(files,str(f.c_str()));
        put(out.get(),"touched_files",files);
    }
    return out;
}
} // namespace

std::vector<SubagentInfo> list_subagents(const App &app){
    std::vector<SubagentInfo> result;
    std::set<std::string> seen{"coder","research"};
    auto scan_dir=[&](const std::string &dir,const std::string &scope,const std::string &prefix){
        if(dir.empty())return;
        std::error_code ec;
        if(!fs::is_directory(fs_path(dir),ec)||path_link(fs_path(dir)))return;
        std::vector<fs::path> entries;
        for(fs::directory_iterator it(fs_path(dir),ec),end;it!=end&&!ec;it.increment(ec)){
            if(path_link(it->path()))continue;
            if(fs::is_regular_file(it->symlink_status(ec))&&lower_str(path_text(it->path().extension()))==".md")
                entries.push_back(it->path());
        }
        std::sort(entries.begin(),entries.end());
        for(const auto &file:entries){
            SubagentInfo info;
            if(parse_agent_file(file,scope,prefix,info)&&seen.insert(info.name).second){
                result.push_back(std::move(info));
                if(result.size()>=64)return;
            }
        }
    };
    if(*app.root){
        scan_dir(std::string(app.root)+"/.deskcode/agents","workspace","");
        scan_dir(std::string(app.root)+"/.claude/agents","workspace","");
        scan_dir(std::string(app.root)+"/.desk-agent/agents","workspace","");
    }
    if(*app.config_dir)scan_dir(std::string(app.config_dir)+"/agents","user","");
    if(user_agents_enabled()){
        const std::string home=user_home();
        if(!home.empty()){
            scan_dir(home+"/.claude/agents","user","");
            scan_dir(home+"/.agents/agents","user","");
        }
    }
    for(const auto &plugin:enabled_plugins(app))scan_dir(plugin.directory+"/agents","plugin",plugin.name);
    return result;
}

std::string delegate_task_description(const App &app){
    std::string desc="Delegate a self-contained coding or research subtask to an isolated subagent. "
        "Built-in agents: 'coder' (default, can edit files and run commands) and 'research' (read-only). "
        "Set worktree=true to run inside a temporary git worktree branch (desk/<agent>-<id>) so parallel tasks can edit files simultaneously without conflicts; if changes are made, the branch name and commit are returned so you can git merge it. "
        "Set background=true to start up to 4 subagents in parallel and collect each result with subagent_output.";
    auto custom=list_subagents(app);
    if(!custom.empty()){
        desc+=" Custom agents:";
        for(const auto &ag:custom){
            if(desc.size()>8000)break;
            desc+=" "+ag.name+(ag.readonly?" [read-only]":"")+(ag.worktree?" [worktree]":"");
            if(!ag.description.empty())desc+=" ("+ag.description+")";
            desc+=";";
        }
    }
    return desc;
}

J *tool_delegate_task(App *app,J *args){
    if(app->subagent)return err(tr("子代理内部不能嵌套启动子代理。"));
    if(app->plan_mode)return err(tr("计划模式下不能启动可写子代理；请使用 delegate_research 做只读调研。"));
    const char *prompt=sget(args,"prompt");
    const size_t plen=strlen(prompt);
    if(!plen||plen>8000)return err(tr("子代理任务说明须为 1 到 8000 字节。"));

    const std::string requested=*sget(args,"agent")?sget(args,"agent"):"coder";
    SubagentInfo chosen;
    if(requested=="coder"){
        chosen.name="coder";
        chosen.readonly=false;
        chosen.max_steps=16;
    }else if(requested=="research"){
        chosen.name="research";
        chosen.readonly=true;
        chosen.max_steps=10;
    }else{
        bool found=false;
        for(const auto &ag:list_subagents(*app)){
            if(ag.name==requested){chosen=ag;found=true;break;}
        }
        if(!found)return err(tr("没有找到该子代理；请使用 coder、research 或已定义的自定义子代理名称。"));
    }

    const bool background=bget(args,"background");
    const bool use_worktree=json_object_object_get_ex(args,"worktree",nullptr)?bget(args,"worktree"):chosen.worktree;

    auto &mgr=manager_of(*app);
    size_t active_bg=0;
    for(const auto &[jid,job]:mgr.jobs){
        std::lock_guard lock(job->mutex);
        if(!job->finished)active_bg++;
    }
    if(background&&active_bg>=MAX_BG_SUBAGENTS)
        return err(tr("后台并行子代理已达 4 个上限，请先通过 subagent_output 等待已有子代理完成。"));

    // Resolve optional model profile override.
    JsonPtr sub_cfg(copy(app->config));
    if(!chosen.model.empty()){
        if(J *prof=get(get(app->store,"profiles"),chosen.model.c_str())){
            App check_app{};
            if(!config_valid(&check_app,prof))sub_cfg.reset(copy(prof));
        }
    }

    const bool allows_shell=!chosen.readonly&&app->shell&&(chosen.tools.empty()||
        std::find(chosen.tools.begin(),chosen.tools.end(),"run_shell")!=chosen.tools.end());
    if(!chosen.readonly){
        std::string preview="["+chosen.name+(use_worktree?" · worktree":"")+(background?" · background":"")+"] "+prompt;
        const Operation op=allows_shell?Operation::Shell:Operation::FileEdit;
        const bool pre_approved=allows_shell?app->yes:(app->yes||app->auto_edit);
        if(!pre_approved&&!approve(app,preview.c_str(),op,tr("启动可写子代理"))){
            J *r=err(tr("用户拒绝启动可写子代理，不要绕过。"));
            put(r,"denied",json_object_new_boolean(1));
            return r;
        }
    }

    auto spec=std::make_shared<SubagentRunSpec>();
    spec->id="agent-"+std::to_string(++mgr.counter);
    spec->agent=chosen;
    spec->prompt=prompt;
    spec->parent_root=app->root;
    spec->config_dir=app->config_dir;
    spec->store.reset(app->store?copy(app->store):obj());
    spec->config=std::move(sub_cfg);
    spec->shell=allows_shell;
    spec->sandbox=app->sandbox;
    spec->use_worktree=use_worktree;

    if(use_worktree){
        std::string wt_err;
        if(!create_worktree(app->root,chosen.name,spec->id,spec->wt,wt_err))
            return err(wt_err.c_str());
    }

    if(!background){
        std::atomic<bool> cancel_flag{false};
        std::uint64_t tokens=0;
        TokenUsage usage{};
        JsonPtr result=run_subagent_loop(*spec,cancel_flag,tokens,usage);
        if(tokens){
            app->task_tokens=add_tokens(app->task_tokens,tokens);
            task_tokens(*app);
        }
        app->turn_usage.add(usage);
        app->session_usage.add(usage);
        if(app->turn_usage.total()||app->session_usage.total())task_usage(*app);
        return result.release();
    }

    auto job=std::make_shared<SubagentJob>();
    job->id=spec->id;
    job->agent_name=chosen.name;
    if(use_worktree){
        job->initial_branch=spec->wt.branch;
        job->initial_worktree=spec->wt.wt_dir;
    }
    mgr.jobs[job->id]=job;
    job->worker=std::thread([spec,job]{
        std::uint64_t tokens=0;
        TokenUsage usage{};
        JsonPtr out=run_subagent_loop(*spec,job->cancel,tokens,usage);
        std::lock_guard lock(job->mutex);
        job->total_tokens=tokens;
        job->usage=usage;
        job->result=std::move(out);
        job->finished=true;
        job->cv.notify_all();
    });

    J *started=obj();
    put(started,"agent_id",str(job->id.c_str()));
    put(started,"agent",str(chosen.name.c_str()));
    put(started,"running",json_object_new_boolean(1));
    if(!job->initial_branch.empty())put(started,"branch",str(job->initial_branch.c_str()));
    return started;
}

J *tool_subagent_output(App *app,J *args){
    const std::string id=sget(args,"agent_id");
    const bool wait_done=!json_object_object_get_ex(args,"wait",nullptr)||bget(args,"wait");
    if(!app->subagents_data)return err(tr("没有找到该后台子代理。"));
    auto &mgr=manager_of(*app);
    auto found=mgr.jobs.find(id);
    if(found==mgr.jobs.end())return err(tr("没有找到该后台子代理。"));
    auto &job=*found->second;
    {
        std::unique_lock lock(job.mutex);
        if(wait_done&&!job.finished){
            const auto deadline=std::chrono::steady_clock::now()+std::chrono::seconds(120);
            while(!job.finished&&!interrupted&&std::chrono::steady_clock::now()<deadline){
                job.cv.wait_for(lock,std::chrono::milliseconds(100));
            }
        }
        if(interrupted&&!job.finished){
            job.cancel=true;
            return err(tr("任务已停止。"));
        }
        if(!job.finished){
            J *r=obj();
            put(r,"agent_id",str(job.id.c_str()));
            put(r,"agent",str(job.agent_name.c_str()));
            put(r,"running",json_object_new_boolean(1));
            return r;
        }
        harvest_usage(*app,job);
    }
    if(job.worker.joinable())job.worker.join();
    JsonPtr res(copy(job.result.get()));
    mgr.jobs.erase(found);
    return res.release();
}

void subagents_destroy(App *app){
    if(!app||!app->subagents_data)return;
    auto *mgr=static_cast<SubagentManager *>(app->subagents_data);
    for(auto &[id,job]:mgr->jobs)job->cancel=true;
    for(auto &[id,job]:mgr->jobs)if(job->worker.joinable())job->worker.join();
    delete mgr;
    app->subagents_data=nullptr;
}
