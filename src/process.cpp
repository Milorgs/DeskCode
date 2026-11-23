#include <algorithm>
#include "desk.hpp"
#include "tasks.hpp"
#include <sys/stat.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <ctype.h>
#ifdef __linux__
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

    bool ready=allow_path("/",ro_tree)&&allow_path(workspace_root,handled)&&allow_path("/tmp",handled);
    for(const char *dev:{"/dev/null","/dev/zero","/dev/full","/dev/random","/dev/urandom","/dev/tty","/dev/pts"})
        ready=ready&&allow_path(dev,handled,true);
    const bool applied=ready&&prctl(PR_SET_NO_NEW_PRIVS,1,0,0,0)==0
        &&syscall(__NR_landlock_restrict_self,rfd,0U)==0;
    close(rfd);
    return applied;
}
} // namespace
#endif

bool sandbox_supported(void){
#ifdef __linux__
    static const bool ok=landlock_abi_version()>=1;
    return ok;
#else
    return false;
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
J *run_command(App *a,const char *command,int timeout_ms){
    if(timeout_ms<1||timeout_ms>300000)return err(tr("命令超时设置无效。"));
    int count=0;
    while(environ[count])count++;
    std::vector<char *> env;
    env.reserve(static_cast<size_t>(count)+1);
    for(int i=0;i<count;i++)if(!private_variable(environ[i]))env.push_back(environ[i]);
    env.push_back(nullptr);
    FILE *output=tmpfile();
    int nullfd=open("/dev/null",O_RDONLY);
    if(!output||nullfd<0){
        if(output)fclose(output);
        if(nullfd>=0)close(nullfd);
        return err(tr("无法创建命令输出文件。"));
    }
    double start=monotime();
#ifdef __linux__
    const bool use_sandbox=a->sandbox&&sandbox_supported();
    const std::string sandbox_error=std::string(tr("无法启用 Landlock 沙箱，命令未执行。"))+'\n';
#endif
    pid_t child=fork();
    if(child<0){
        fclose(output);
        close(nullfd);
        return err(tr("无法启动命令。"));
    }
    if(child==0){
        signal(SIGINT,SIG_DFL);
        signal(SIGTERM,SIG_DFL);
        if(setpgid(0,0)||chdir(a->root)||dup2(nullfd,0)<0||dup2(fileno(output),1)<0||dup2(fileno(output),2)<0)_exit(126);
        close(nullfd);
        if(fileno(output)>2)close(fileno(output));
#ifdef __linux__
        const char *no_sb=env_value("DESK_NO_SANDBOX");
        if(use_sandbox&&(!no_sb||!*no_sb||!strcmp(no_sb,"0"))&&!apply_landlock_sandbox(a->root)){
            const ssize_t written=write(STDERR_FILENO,sandbox_error.data(),sandbox_error.size());
            (void)written;
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
        fclose(output);
        return err(tr("无法建立命令进程组。"));
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
