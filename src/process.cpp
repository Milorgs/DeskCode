#include <algorithm>
#include "desk.hpp"
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
        if((monotime()-start)*1000>=timeout_ms){
            timed=true;
            break;
        }
        struct timespec pause={
            0,pause_ns
        };
        nanosleep(&pause,NULL);
        pause_ns=std::min(pause_ns*2,25000000L);
    }
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
