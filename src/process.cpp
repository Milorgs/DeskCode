#include <algorithm>
#include "desk.hpp"
#include "tasks.hpp"
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/socket.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>

namespace {
// When running inside a git worktree (`<wt>/.git` is a regular file pointing to
// `<repo>/.git/worktrees/<id>`), resolve `<repo>/.git` so git index/commit operations
// inside the worktree work while `<repo>`'s main checkout stays read-only.
std::string resolve_worktree_git_dir(const char *workspace_root){
    if(!workspace_root||!*workspace_root)return {};
    std::string dotgit=std::string(workspace_root)+"/.git";
    struct stat st{};
    if(lstat(dotgit.c_str(),&st)!=0||!S_ISREG(st.st_mode))return {};
    size_t len=0;
    std::unique_ptr<char,decltype(&free)> raw(read_bytes(dotgit.c_str(),4096,&len),free);
    if(!raw)return {};
    std::string_view text(raw.get(),len);
    if(!text.starts_with("gitdir:"))return {};
    text.remove_prefix(7);
    while(!text.empty()&&(text.front()==' '||text.front()=='\t'))text.remove_prefix(1);
    while(!text.empty()&&(text.back()=='\n'||text.back()=='\r'||text.back()==' '||text.back()=='\t'))text.remove_suffix(1);
    if(text.empty())return {};
    fs::path p=fs_path(text);
    if(!p.is_absolute())p=fs_path(workspace_root)/p;
    char resolved[PATH_MAX];
    if(!realpath(path_text(p).c_str(),resolved))return {};
    fs::path cur=fs_path(resolved);
    for(int depth=0;depth<3&&cur.has_parent_path();++depth){
        if(cur.filename()==".git")return path_text(cur);
        cur=cur.parent_path();
    }
    return {};
}
bool env_truthy(const char *name){
    const char *v=env_value(name);
    return v&&*v&&strcmp(v,"0")&&strcasecmp(v,"false")&&strcasecmp(v,"off");
}
} // namespace

#ifdef __linux__
#include <linux/filter.h>
#include <linux/seccomp.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#ifndef __NR_landlock_create_ruleset
#define __NR_landlock_create_ruleset 444
#define __NR_landlock_add_rule 445
#define __NR_landlock_restrict_self 446
#endif
namespace {
struct DeskLandlockRuleset {
    uint64_t handled_access_fs;
};
struct DeskLandlockPathBeneath {
    uint64_t allowed_access;
    int32_t parent_fd;
} __attribute__((packed));

int landlock_abi_version(){
    long v=syscall(__NR_landlock_create_ruleset,nullptr,0,1U);
    return v>0?static_cast<int>(v):0;
}

bool linux_seccomp_supported(){
    return prctl(PR_GET_SECCOMP,0,0,0,0)>=0||errno!=EINVAL;
}

bool apply_landlock_sandbox(const char *workspace_root){
    const int abi=landlock_abi_version();
    if(abi<1)return false;
    constexpr uint64_t fs_exec=1ULL<<0;
    constexpr uint64_t fs_write_file=1ULL<<1;
    constexpr uint64_t fs_read_file=1ULL<<2;
    constexpr uint64_t fs_read_dir=1ULL<<3;
    uint64_t handled=(1ULL<<13)-1ULL; // ABI v1 bits 0..12
    if(abi>=2)handled|=(1ULL<<13);    // REFER
    if(abi>=3)handled|=(1ULL<<14);    // TRUNCATE
    const uint64_t file_only_mask=(fs_exec|fs_write_file|fs_read_file|(1ULL<<14))&handled;
    const uint64_t ro_tree=(fs_exec|fs_read_file|fs_read_dir)&handled;

    DeskLandlockRuleset attr{handled};
    long ruleset=syscall(__NR_landlock_create_ruleset,&attr,sizeof attr,0U);
    if(ruleset<0)return false;
    const int rfd=static_cast<int>(ruleset);

    auto allow_path=[&](const char *p,uint64_t access,bool optional=false){
        int pfd=open(p,O_PATH|O_CLOEXEC);
        if(pfd<0)return optional&&(errno==ENOENT||errno==ENOTDIR);
        struct stat st{};
        bool added=false;
        if(fstat(pfd,&st)==0){
            uint64_t mask=S_ISDIR(st.st_mode)?access:(access&file_only_mask);
            DeskLandlockPathBeneath rule{mask,pfd};
            added=mask&&syscall(__NR_landlock_add_rule,rfd,1,&rule,0U)==0;
        }
        close(pfd);
        return added;
    };

    bool ready=allow_path("/",ro_tree)&&allow_path(workspace_root,handled)&&allow_path("/tmp",handled)&&allow_path("/var/tmp",handled,true);
    const std::string git_common=resolve_worktree_git_dir(workspace_root);
    if(!git_common.empty())ready=ready&&allow_path(git_common.c_str(),handled,true);
    for(const char *dev:{"/dev/null","/dev/zero","/dev/full","/dev/random","/dev/urandom","/dev/tty","/dev/pts"})
        ready=ready&&allow_path(dev,handled,true);
    const bool applied=ready&&prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0
        &&syscall(__NR_landlock_restrict_self,rfd,0U)==0;
    close(rfd);
    return applied;
}

bool apply_linux_no_net(){
#if defined(__NR_socket)
    if(prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)!=0)return false;
    struct sock_filter insns[]={
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,static_cast<uint32_t>(offsetof(struct seccomp_data,nr))),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,static_cast<uint32_t>(__NR_socket),0,5),
        BPF_STMT(BPF_LD|BPF_W|BPF_ABS,static_cast<uint32_t>(offsetof(struct seccomp_data,args[0]))),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,static_cast<uint32_t>(AF_INET),2,0),
        BPF_JUMP(BPF_JMP|BPF_JEQ|BPF_K,static_cast<uint32_t>(AF_INET6),1,0),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ERRNO|static_cast<uint32_t>(EACCES)),
        BPF_STMT(BPF_RET|BPF_K,SECCOMP_RET_ALLOW),
    };
    struct sock_fprog prog{
        static_cast<unsigned short>(sizeof(insns)/sizeof(insns[0])),
        insns
    };
    return prctl(PR_SET_SECCOMP,SECCOMP_MODE_FILTER,&prog)==0;
#else
    return false;
#endif
}
} // namespace
#elif defined(__APPLE__)
namespace {
std::string sbpl_escape(std::string_view s){
    std::string out;
    out.reserve(s.size());
    for(char c:s){if(c=='\\'||c=='"')out+='\\';out+=c;}
    return out;
}
std::string build_seatbelt_profile(const char *workspace_root,bool fs_restrict,bool net_restrict){
    std::string prof="(version 1)\n(allow default)\n";
    if(fs_restrict){
        prof+="(deny file-write* (subpath \"/\"))\n(allow file-write*\n";
        auto allow_sub=[&](const char *raw){
            if(!raw||!*raw)return;
            std::string orig(raw);
            while(orig.size()>1&&orig.back()=='/')orig.pop_back();
            prof+="  (subpath \""+sbpl_escape(orig)+"\")\n";
            char real_buf[PATH_MAX];
            if(realpath(orig.c_str(),real_buf)){
                std::string rp(real_buf);
                if(rp!=orig)prof+="  (subpath \""+sbpl_escape(rp)+"\")\n";
            }
        };
        allow_sub(workspace_root);
        const std::string git_common=resolve_worktree_git_dir(workspace_root);
        if(!git_common.empty())allow_sub(git_common.c_str());
        allow_sub("/tmp");
        allow_sub("/private/tmp");
        allow_sub("/var/tmp");
        allow_sub("/private/var/tmp");
        if(const char *td=env_value("TMPDIR");td&&*td)allow_sub(td);
        prof+="  (subpath \"/dev\"))\n";
    }
    if(net_restrict){
        prof+="(deny network* (remote ip))\n";
    }
    return prof;
}
} // namespace
#endif

bool sandbox_supported(void){
#ifdef __linux__
    static const bool ok=landlock_abi_version()>=1;
    return ok;
#elif defined(__APPLE__)
    static const bool ok=access("/usr/bin/sandbox-exec",X_OK)==0;
    return ok;
#else
    return false;
#endif
}
bool sandbox_net_supported(void){
#ifdef __linux__
    static const bool ok=linux_seccomp_supported();
    return ok;
#elif defined(__APPLE__)
    return sandbox_supported();
#else
    return false;
#endif
}
const char *sandbox_backend(void){
#ifdef __linux__
    return sandbox_supported()?"landlock":(sandbox_net_supported()?"seccomp":"none");
#elif defined(__APPLE__)
    return sandbox_supported()?"seatbelt":"none";
#else
    return "none";
#endif
}
extern char **environ;
static bool private_variable(const char *entry){
    const char *equals=strchr(entry,'=');
    if(!equals)return true;
    size_t n=(size_t)(equals-entry);
    char *name=strndup(entry,n);
    if(!name)return true;
    for(char *p=name;*p;p++)*p=(char)toupper((unsigned char)*p);
    bool secret=strstr(name,"KEY")||strstr(name,"TOKEN")||strstr(name,"SECRET")||strstr(name,"PASSWORD");
    free(name);
    return secret;
}
static std::string latest_output_line(std::string_view chunk){
    while(!chunk.empty()&&(chunk.back()=='\n'||chunk.back()=='\r'||chunk.back()==' '))chunk.remove_suffix(1);
    if(chunk.empty())return {};
    auto pos=chunk.find_last_of("\r\n");
    std::string_view raw=pos==std::string_view::npos?chunk:chunk.substr(pos+1);
    std::string clean;
    clean.reserve(raw.size());
    bool sp=false;
    for(size_t i=0;i<raw.size();){
        auto c=static_cast<unsigned char>(raw[i]);
        if(c==0x1b&&i+1<raw.size()&&raw[i+1]=='['){
            i+=2;
            while(i<raw.size()&&!(raw[i]>='@'&&raw[i]<='~'))i++;
            if(i<raw.size())i++;
            continue;
        }
        if(c<32||c==127){
            if(c=='\t'&&!clean.empty())sp=true;
            i++;
            continue;
        }
        if(c==' '){if(!clean.empty())sp=true;i++;continue;}
        if(sp){clean+=' ';sp=false;}
        clean+=static_cast<char>(c);
        i++;
    }
    size_t used=0;
    clean=display_utf8(clean,used,true);
    if(clean.size()>72){
        size_t cut=72;
        while(cut>0&&(static_cast<unsigned char>(clean[cut])&0xc0)==0x80)--cut;
        clean.resize(cut);
        clean+="…";
    }
    return clean;
}
namespace {
// Start bash for `command` in its own process group with stdout and stderr in
// `output`. Returns the child pid, or -1 with a translated message in error.
pid_t spawn_command(App *a,const char *command,FILE *output,const char *&error){
    int count=0;
    while(environ[count])count++;
    std::vector<char *> env;
    env.reserve(static_cast<size_t>(count)+1);
    for(int i=0;i<count;i++)if(!private_variable(environ[i]))env.push_back(environ[i]);
    env.push_back(nullptr);
    int nullfd=open("/dev/null",O_RDONLY);
    if(nullfd<0){error=tr("无法创建命令输出文件。");return -1;}
    const bool force_off=env_truthy("DESK_NO_SANDBOX");
    const bool use_sandbox=!force_off&&a->sandbox&&sandbox_supported();
    const bool use_no_net=(a->sandbox_no_net||env_truthy("DESK_SANDBOX_NO_NET"))&&sandbox_net_supported();
#ifdef __linux__
    const std::string sandbox_error=std::string(tr("无法启用 Landlock 沙箱，命令未执行。"))+'\n';
    const std::string net_error=std::string(tr("无法启用网络隔离沙箱，命令未执行。"))+'\n';
#elif defined(__APPLE__)
    std::string seatbelt_profile=(use_sandbox||use_no_net)?build_seatbelt_profile(a->root,use_sandbox,use_no_net):std::string();
#endif
    pid_t child=fork();
    if(child<0){
        close(nullfd);
        error=tr("无法启动命令。");
        return -1;
    }
    if(child==0){
        signal(SIGINT,SIG_DFL);
        signal(SIGTERM,SIG_DFL);
        signal(SIGUSR1,SIG_DFL);
        if(setpgid(0,0)||chdir(a->root)||dup2(nullfd,0)<0||dup2(fileno(output),1)<0||dup2(fileno(output),2)<0)_exit(126);
        close(nullfd);
        if(fileno(output)>2)close(fileno(output));
#ifdef __linux__
        if(use_sandbox&&!apply_landlock_sandbox(a->root)){
            const ssize_t written=write(STDERR_FILENO,sandbox_error.data(),sandbox_error.size());
            (void)written;
            _exit(126);
        }
        if(use_no_net&&!apply_linux_no_net()){
            const ssize_t written=write(STDERR_FILENO,net_error.data(),net_error.size());
            (void)written;
            _exit(126);
        }
#elif defined(__APPLE__)
        if(!seatbelt_profile.empty()){
            char sb_exe[]="sandbox-exec",sb_flag[]="-p",bash_path[]="/bin/bash",profile[]="--noprofile",rc[]="--norc",option[]="-c";
            char *sb_argv[]={sb_exe,sb_flag,seatbelt_profile.data(),bash_path,profile,rc,option,const_cast<char *>(command),nullptr};
            execve("/usr/bin/sandbox-exec",sb_argv,env.data());
            _exit(126);
        }
#endif
        char bash[]="bash",profile[]="--noprofile",rc[]="--norc",option[]="-c";
        char *argv[]={bash,profile,rc,option,const_cast<char *>(command),nullptr};
        execve("/bin/bash",argv,env.data());
        _exit(127);
    }
    close(nullfd);
    if(setpgid(child,child)<0&&errno!=EACCES&&errno!=ESRCH){
        kill(child,SIGKILL);
        while(waitpid(child,NULL,0)<0&&errno==EINTR){
        }
        error=tr("无法建立命令进程组。");
        return -1;
    }
    return child;
}
// Invalid UTF-8 becomes U+FFFD so the text always fits in a JSON string.
std::string utf8_text(std::string data){
    if(valid_utf8(data))return data;
    std::string safe;
    const size_t len=data.size();
    safe.reserve(len);
    for(size_t i=0;i<len;){
        unsigned char c=(unsigned char)data[i];
        size_t width=c<0x80?1:(c>=0xc2&&c<=0xdf)?2:(c>=0xe0&&c<=0xef)?3:(c>=0xf0&&c<=0xf4)?4:0;
        bool valid=width&&i+width<=len;
        for(size_t k=1;valid&&k<width;k++)if(((unsigned char)data[i+k]&0xc0)!=0x80)valid=false;
        if(valid&&width==3&&((c==0xe0&&(unsigned char)data[i+1]<0xa0)||(c==0xed&&(unsigned char)data[i+1]>=0xa0)))valid=false;
        if(valid&&width==4&&((c==0xf0&&(unsigned char)data[i+1]<0x90)||(c==0xf4&&(unsigned char)data[i+1]>=0x90)))valid=false;
        if(valid){safe.append(data.data()+i,width);i+=width;}
        else{safe+="\xef\xbf\xbd";i++;}
    }
    return safe;
}
struct BackgroundShell {
    std::string id,command;
    pid_t pid=-1;
    FILE *output=nullptr;
    off_t shown=0;
    double started=0;
    bool finished=false,killed=false;
    int exit_code=0;
};
struct Shells {
    std::vector<BackgroundShell> list;
    int next=1;
};
constexpr size_t MAX_BACKGROUND_SHELLS=8;
constexpr off_t SHELL_OUTPUT_CHUNK=32768;
Shells &shells(App *a){
    if(!a->shell_data)a->shell_data=new Shells;
    return *static_cast<Shells *>(a->shell_data);
}
void refresh_shell(BackgroundShell &shell){
    if(shell.finished)return;
    int status=0;
    if(waitpid(shell.pid,&status,WNOHANG)==shell.pid){
        shell.finished=true;
        shell.exit_code=WIFEXITED(status)?WEXITSTATUS(status):-WTERMSIG(status);
    }
}
size_t running_shells(Shells &all){
    size_t running=0;
    for(auto &shell:all.list){refresh_shell(shell);running+=!shell.finished;}
    return running;
}
// New output since the last read, at most one chunk; older bytes beyond a
// chunk are skipped and counted so the model knows output was dropped.
J *shell_state(BackgroundShell &shell){
    refresh_shell(shell);
    J *r=obj();
    put(r,"shell_id",str(shell.id.c_str()));
    put(r,"command",str(shell.command.c_str()));
    put(r,"running",json_object_new_boolean(!shell.finished));
    if(shell.finished)put(r,"exit_code",json_object_new_int(shell.exit_code));
    if(shell.killed)put(r,"killed",json_object_new_boolean(1));
    put(r,"elapsed_seconds",json_object_new_double(static_cast<double>(static_cast<long long>((monotime()-shell.started)*10))/10));
    struct stat info{};
    off_t size=fstat(fileno(shell.output),&info)==0?info.st_size:shell.shown;
    off_t from=shell.shown;
    if(size-from>SHELL_OUTPUT_CHUNK){
        put(r,"skipped_bytes",json_object_new_int64(static_cast<int64_t>(size-SHELL_OUTPUT_CHUNK-from)));
        from=size-SHELL_OUTPUT_CHUNK;
    }
    std::string data(static_cast<size_t>(std::max<off_t>(0,size-from)),'\0');
    ssize_t got=data.empty()?0:pread(fileno(shell.output),data.data(),data.size(),from);
    data.resize(got>0?static_cast<size_t>(got):0);
    // Hold back a partial UTF-8 character until the rest arrives.
    size_t keep=data.size();
    if(!shell.finished){
        size_t tail=0;
        while(tail<keep&&tail<3&&(static_cast<unsigned char>(data[keep-1-tail])&0xc0)==0x80)tail++;
        if(tail<keep){
            auto lead=static_cast<unsigned char>(data[keep-1-tail]);
            size_t expected=lead<0x80?1:lead<0xe0?2:lead<0xf0?3:lead<0xf8?4:0;
            if(expected>tail+1)keep-=tail+1;
        }
    }
    data.resize(keep);
    shell.shown=from+static_cast<off_t>(keep);
    const std::string text=utf8_text(std::move(data));
    put(r,"output",json_object_new_string_len(text.data(),static_cast<int>(text.size())));
    return r;
}
J *register_shell(App *a,const char *command,pid_t child,FILE *output,double started){
    auto &all=shells(a);
    BackgroundShell shell;
    shell.id="bash-"+std::to_string(all.next++);
    shell.command=command;
    shell.pid=child;
    shell.output=output;
    shell.started=started;
    all.list.push_back(std::move(shell));
    // Finished entries beyond the cap are dropped oldest first.
    while(all.list.size()>MAX_BACKGROUND_SHELLS*2){
        auto victim=std::find_if(all.list.begin(),all.list.end(),[](BackgroundShell &s){refresh_shell(s);return s.finished;});
        if(victim==all.list.end())break;
        fclose(victim->output);
        all.list.erase(victim);
    }
    J *r=shell_state(all.list.back());
    put(r,"note",str("Running in the background. Use shell_output to read new output and shell_kill to stop it; it stops when this conversation ends."));
    return r;
}
}
J *start_background_command(App *a,const char *command){
    auto &all=shells(a);
    if(running_shells(all)>=MAX_BACKGROUND_SHELLS)return err(tr("后台命令已达 8 个上限，请先用 shell_kill 停止不需要的命令。"));
    FILE *output=tmpfile();
    if(!output)return err(tr("无法创建命令输出文件。"));
    const char *error=nullptr;
    const double started=monotime();
    pid_t child=spawn_command(a,command,output,error);
    if(child<0){fclose(output);return err(error);}
    // A command that fails at once (typo, missing program) reports like a
    // normal one instead of leaving a dead background entry.
    int status=0;
    for(int i=0;i<40&&!interrupted;i++){
        if(waitpid(child,&status,WNOHANG)==child){
            BackgroundShell done;
            done.id="";done.command=command;done.pid=child;done.output=output;done.started=started;
            done.finished=true;done.exit_code=WIFEXITED(status)?WEXITSTATUS(status):-WTERMSIG(status);
            J *r=shell_state(done);
            json_object_object_del(r,"shell_id");
            fclose(output);
            return r;
        }
        usleep(25000);
    }
    return register_shell(a,command,child,output,started);
}
J *tool_shell_output(App *a,J *args){
    auto &all=shells(a);
    const char *id=sget(args,"shell_id");
    for(auto &shell:all.list)if(shell.id==id)return shell_state(shell);
    return err(tr("没有这个后台命令；shell_id 来自 run_shell 的返回结果。"));
}
J *tool_shell_kill(App *a,J *args){
    auto &all=shells(a);
    const char *id=sget(args,"shell_id");
    for(auto &shell:all.list){
        if(shell.id!=id)continue;
        refresh_shell(shell);
        if(!shell.finished){
            kill(-shell.pid,SIGTERM);
            for(int i=0;i<40;i++){refresh_shell(shell);if(shell.finished)break;usleep(25000);}
            if(!shell.finished){
                kill(-shell.pid,SIGKILL);
                int status=0;
                while(waitpid(shell.pid,&status,0)<0&&errno==EINTR){}
                shell.finished=true;
                shell.exit_code=WIFEXITED(status)?WEXITSTATUS(status):-WTERMSIG(status);
            }
            shell.killed=true;
        }
        // The group may still hold children after bash itself exits.
        kill(-shell.pid,SIGKILL);
        return shell_state(shell);
    }
    return err(tr("没有这个后台命令；shell_id 来自 run_shell 的返回结果。"));
}
size_t background_shell_count(App *a){
    return a->shell_data?running_shells(shells(a)):0;
}
void shells_destroy(App *a){
    if(!a->shell_data)return;
    auto *all=static_cast<Shells *>(a->shell_data);
    for(auto &shell:all->list){
        refresh_shell(shell);
        kill(-shell.pid,SIGKILL);
        if(!shell.finished)while(waitpid(shell.pid,nullptr,0)<0&&errno==EINTR){}
        fclose(shell.output);
    }
    delete all;
    a->shell_data=nullptr;
}
J *run_command(App *a,const char *command,int timeout_ms){
    if(timeout_ms<1||timeout_ms>300000)return err(tr("命令超时设置无效。"));
    FILE *output=tmpfile();
    if(!output)return err(tr("无法创建命令输出文件。"));
    double start=monotime();
    const char *spawn_error=nullptr;
    background_requested=false;
    pid_t child=spawn_command(a,command,output,spawn_error);
    if(child<0){
        fclose(output);
        return err(spawn_error);
    }
    int status=0;
    bool reaped=false,timed=false,failed=false;
    // Start polling at 1 ms so quick commands return at once, then back off to
    // 25 ms: long commands no longer wake the process a thousand times a second.
    long pause_ns=1000000;
    double last_tail_at=start;
    std::string last_tail;
    while(!interrupted){
        check_exit(*a);
        progress_tick(a);
        if(interrupted)break;
        if(background_requested.exchange(false)){
            // Ctrl+B: the command keeps running and the turn continues.
            if(!last_tail.empty()&&a->task_events>=0)task_detail(*a,"");
            J *r=register_shell(a,command,child,output,start);
            put(r,"moved_to_background",json_object_new_boolean(1));
            put(r,"note",str("The user moved this command to the background; it is still running. Continue with other work, check it with shell_output, and stop it with shell_kill when it is no longer needed."));
            progress_clear(a);
            printf("%s\n",tr("命令已转入后台继续运行，任务继续。"));
            fflush(stdout);
            return r;
        }
        pid_t result=waitpid(child,&status,WNOHANG);
        if(result==child){
            reaped=true;
            break;
        }
        if(result<0&&errno!=EINTR){
            failed=true;
            break;
        }
        const double now=monotime();
        if((now-start)*1000>=timeout_ms){
            timed=true;
            break;
        }
        if(a->config&&!a->verbose&&now-start>=0.35&&now-last_tail_at>=0.25){
            last_tail_at=now;
            struct stat live{};
            if(fstat(fileno(output),&live)==0&&live.st_size>0){
                const off_t off=std::max<off_t>(0,live.st_size-512);
                char tail_buf[512];
                const ssize_t got=pread(fileno(output),tail_buf,sizeof tail_buf,off);
                if(got>0){
                    std::string line=latest_output_line(std::string_view(tail_buf,static_cast<size_t>(got)));
                    if(!line.empty()&&line!=last_tail){
                        last_tail=line;
                        if(a->task_events>=0)task_detail(*a,line);
                        else if(isatty(1)){
                            char msg[256];
                            snprintf(msg,sizeof msg,tr("正在执行命令：%s"),line.c_str());
                            progress(a,msg);
                        }
                    }
                }
            }
        }
        struct timespec pause={
            0,pause_ns
        };
        nanosleep(&pause,NULL);
        pause_ns=std::min(pause_ns*2,25000000L);
    }
    if(!last_tail.empty()&&a->task_events>=0)task_detail(*a,"");
    kill(-child,SIGKILL);
    if(!reaped){
        pid_t result;
        do{
            result=waitpid(child,&status,0);
        }
        while(result<0&&errno==EINTR);
        if(result!=child)failed=true;
    }
    if(interrupted){
        fclose(output);
        return err(tr("命令已取消。"));
    }
    if(failed){
        fclose(output);
        return err(tr("命令等待失败，未自动重新执行。"));
    }
    // Read the output that exists after the command group has stopped.
    struct stat captured{};
    if(fstat(fileno(output),&captured)<0){
        fclose(output);
        return err(tr("读取命令输出失败。"));
    }
    rewind(output);
    const bool truncated=captured.st_size>static_cast<off_t>(COMMAND_OUTPUT_LIMIT);
    std::string data;
    const off_t target=std::min<off_t>(captured.st_size,COMMAND_OUTPUT_LIMIT);
    if(target>0)data.reserve(static_cast<size_t>(target));
    char buffer[16384];
    for(off_t remaining=target;remaining>0;){
        const size_t count=fread(buffer,1,static_cast<size_t>(std::min<off_t>(remaining,sizeof buffer)),output);
        if(!count)break;
        data.append(buffer,count);
        remaining-=static_cast<off_t>(count);
    }
    const bool io_error=ferror(output);
    fclose(output);
    if(io_error)return err(tr("读取命令输出失败。"));
    if(truncated){
        size_t tail=0;
        while(tail<data.size()&&tail<3&&(static_cast<unsigned char>(data[data.size()-1-tail])&0xc0)==0x80)tail++;
        if(tail<data.size()){
            auto lead=static_cast<unsigned char>(data[data.size()-1-tail]);
            size_t expected=lead<0x80?1:lead<0xe0?2:lead<0xf0?3:lead<0xf8?4:0;
            if(expected>tail+1)data.resize(data.size()-1-tail);
        }
    }
    std::string safe;
    const std::string *encoded_out=&data;
    if(!valid_utf8(data)){
        const size_t len=data.size();
        safe.reserve(len);
        for(size_t i=0;i<len;){
            unsigned char c=(unsigned char)data[i];
            size_t width=c<0x80?1:(c>=0xc2&&c<=0xdf)?2:(c>=0xe0&&c<=0xef)?3:(c>=0xf0&&c<=0xf4)?4:0;
            bool valid=width&&i+width<=len;
            for(size_t k=1;valid&&k<width;k++)if(((unsigned char)data[i+k]&0xc0)!=0x80)valid=false;
            if(valid&&width==3&&((c==0xe0&&(unsigned char)data[i+1]<0xa0)||(c==0xed&&(unsigned char)data[i+1]>=0xa0)))valid=false;
            if(valid&&width==4&&((c==0xf0&&(unsigned char)data[i+1]<0x90)||(c==0xf4&&(unsigned char)data[i+1]>=0x90)))valid=false;
            if(valid){
                safe.append(data.data()+i,width);
                i+=width;
            }
            else{
                safe+="\xef\xbf\xbd";
                i++;
            }
        }
        encoded_out=&safe;
    }
    if(encoded_out->size()>INT_MAX)return err(tr("命令输出无法转换为 JSON。"));
    J *r=obj();
    put(r,"exit_code",json_object_new_int(WIFEXITED(status)?WEXITSTATUS(status):-WTERMSIG(status)));
    put(r,"timed_out",json_object_new_boolean(timed));
    put(r,"truncated",json_object_new_boolean(truncated));
    put(r,"output",json_object_new_string_len(encoded_out->data(),static_cast<int>(encoded_out->size())));
    return r;
}
