#include "win.hpp"
#include "tasks.hpp"
#include <cwctype>
bool sandbox_supported(void){return true;}
bool sandbox_net_supported(void){return true;}
const char *sandbox_backend(void){return "job-restricted-token";}
#include <cstring>
#include <openssl/evp.h>
namespace {
bool win_env_truthy(const char *name){
    const char *v=env_value(name);
    return v&&*v&&strcmp(v,"0")&&_stricmp(v,"false")&&_stricmp(v,"off");
}
}
std::vector<wchar_t> command_environment(bool no_net){
    std::vector<wchar_t> result;LPWCH block=GetEnvironmentStringsW();if(!block)throw std::runtime_error("Environment unavailable");
    for(const wchar_t *entry=block;*entry;entry+=wcslen(entry)+1){
        const wchar_t *end=wcschr(entry+(*entry==L'='?1:0),L'=');if(!end)continue;
        std::wstring key(entry,end);for(auto &c:key)c=static_cast<wchar_t>(towupper(c));
        if(key.find(L"KEY")!=std::wstring::npos||key.find(L"TOKEN")!=std::wstring::npos||key.find(L"SECRET")!=std::wstring::npos||key.find(L"PASSWORD")!=std::wstring::npos)continue;
        if(no_net&&(key==L"HTTP_PROXY"||key==L"HTTPS_PROXY"||key==L"ALL_PROXY"||key==L"NO_PROXY"))continue;
        result.insert(result.end(),entry,entry+wcslen(entry)+1);
    }
    FreeEnvironmentStringsW(block);
    if(no_net){
        for(const wchar_t *kv:{L"HTTP_PROXY=http://127.0.0.1:1",L"HTTPS_PROXY=http://127.0.0.1:1",L"ALL_PROXY=http://127.0.0.1:1",L"NO_PROXY="}){
            result.insert(result.end(),kv,kv+wcslen(kv)+1);
        }
    }
    result.push_back(0);if(result.size()==1)result.push_back(0);return result;
}
bool win_spawn(const std::wstring &executable,std::wstring command,const std::wstring &directory,HANDLE input,HANDLE output,const std::vector<HANDLE> &inherit,HANDLE job,WinHandle &process,bool sandboxed,bool no_net){
    SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);std::vector<unsigned char> storage(bytes);
    auto *attributes=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if(!InitializeProcThreadAttributeList(attributes,1,0,&bytes))return false;
    if(!UpdateProcThreadAttribute(attributes,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,const_cast<HANDLE *>(inherit.data()),inherit.size()*sizeof(HANDLE),nullptr,nullptr)){DeleteProcThreadAttributeList(attributes);return false;}
    STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof start;start.StartupInfo.dwFlags=STARTF_USESTDHANDLES;start.StartupInfo.hStdInput=input;start.StartupInfo.hStdOutput=output;start.StartupInfo.hStdError=output;start.lpAttributeList=attributes;
    PROCESS_INFORMATION info{};auto environment=command_environment(no_net);
    const DWORD flags=EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|CREATE_SUSPENDED;
    BOOL created=FALSE;
    if(sandboxed){
        HANDLE raw_tok=nullptr,raw_restricted=nullptr;
        if(OpenProcessToken(GetCurrentProcess(),TOKEN_DUPLICATE|TOKEN_ASSIGN_PRIMARY|TOKEN_QUERY|TOKEN_ADJUST_DEFAULT,&raw_tok)){
            WinHandle proc_tok(raw_tok);
            if(CreateRestrictedToken(proc_tok.get(),DISABLE_MAX_PRIVILEGE,0,nullptr,0,nullptr,0,nullptr,&raw_restricted)){
                WinHandle restricted(raw_restricted);
                std::wstring cmd_copy=command;
                created=CreateProcessAsUserW(restricted.get(),executable.c_str(),cmd_copy.data(),nullptr,nullptr,TRUE,flags,environment.data(),directory.empty()?nullptr:directory.c_str(),&start.StartupInfo,&info);
            }
        }
    }
    if(!created)created=CreateProcessW(executable.c_str(),command.data(),nullptr,nullptr,TRUE,flags,environment.data(),directory.empty()?nullptr:directory.c_str(),&start.StartupInfo,&info);
    DeleteProcThreadAttributeList(attributes);if(!created)return false;
    WinHandle child(info.hProcess),thread(info.hThread);
    if(!AssignProcessToJobObject(job,child.get())||ResumeThread(thread.get())==static_cast<DWORD>(-1)){TerminateProcess(child.get(),1);WaitForSingleObject(child.get(),2000);return false;}
    process=std::move(child);return true;
}
std::string safe_output(std::string_view bytes){
    if(valid_utf8(bytes))return std::string(bytes);
    std::string result;
    result.reserve(bytes.size());
    for(size_t i=0;i<bytes.size();){
        auto c=static_cast<unsigned char>(bytes[i]);size_t size=c<0x80?1:c>=0xc2&&c<=0xdf?2:c>=0xe0&&c<=0xef?3:c>=0xf0&&c<=0xf4?4:0;
        if(size&&size<=bytes.size()-i&&valid_utf8(bytes.substr(i,size))){result.append(bytes.substr(i,size));i+=size;}
        else{result+="\xef\xbf\xbd";i++;}
    }
    return result;
}
static std::wstring powershell_executable(){
    const char *root=env_value("SystemRoot");
    if(!root)return {};
    return wide(root)+L"\\System32\\WindowsPowerShell\\v1.0\\powershell.exe";
}
static std::wstring powershell_arguments(const std::wstring &executable,const char *command,bool no_net=false){
    std::string prefix=no_net?"[System.Net.WebRequest]::DefaultWebProxy = [System.Net.WebProxy]::new('http://127.0.0.1:1'); ":"";
    std::string script="[Console]::OutputEncoding = [System.Text.UTF8Encoding]::new($false); $OutputEncoding = [Console]::OutputEncoding; "+prefix+"& {\n"+std::string(command)+"\n}; $desk_ok=$?; $desk_exit=$LASTEXITCODE; if($null -ne $desk_exit){exit $desk_exit}; if(!$desk_ok){exit 1}";
    std::wstring text=wide(script);std::string encoded(((text.size()*sizeof(wchar_t)+2)/3)*4,'\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(encoded.data()),reinterpret_cast<const unsigned char *>(text.data()),static_cast<int>(text.size()*sizeof(wchar_t)));
    return quote_arg(executable)+L" -NoLogo -NoProfile -NonInteractive -OutputFormat Text -EncodedCommand "+wide(encoded);
}
namespace {
struct BackgroundShell {
    std::string id,command;
    WinHandle process,job,reader;
    std::uint64_t shown=0;
    double started=0;
    bool finished=false,killed=false;
    long long exit_code=0;
};
struct Shells {std::vector<std::unique_ptr<BackgroundShell>> list;int next=1;};
Shells &shells(App *a){
    if(!a->shell_data)a->shell_data=new Shells;
    return *static_cast<Shells *>(a->shell_data);
}
void refresh_shell(BackgroundShell &shell){
    if(shell.finished)return;
    if(WaitForSingleObject(shell.process.get(),0)==WAIT_OBJECT_0){
        DWORD code=1;GetExitCodeProcess(shell.process.get(),&code);
        shell.finished=true;shell.exit_code=code;
    }
}
J *shell_state(BackgroundShell &shell){
    refresh_shell(shell);
    J *r=obj();
    put(r,"shell_id",str(shell.id.c_str()));
    put(r,"command",str(shell.command.c_str()));
    put(r,"running",json_object_new_boolean(!shell.finished));
    if(shell.finished)put(r,"exit_code",json_object_new_int64(shell.exit_code));
    if(shell.killed)put(r,"killed",json_object_new_boolean(1));
    LARGE_INTEGER size{};
    std::uint64_t total=GetFileSizeEx(shell.reader.get(),&size)?static_cast<std::uint64_t>(size.QuadPart):shell.shown;
    std::uint64_t from=shell.shown;
    if(total-from>32768){
        put(r,"skipped_bytes",json_object_new_int64(static_cast<int64_t>(total-32768-from)));
        from=total-32768;
    }
    std::string data(static_cast<size_t>(total>from?total-from:0),'\0');
    DWORD got=0;
    if(!data.empty()){
        OVERLAPPED offset{};offset.Offset=static_cast<DWORD>(from&0xffffffffu);offset.OffsetHigh=static_cast<DWORD>(from>>32);
        if(!ReadFile(shell.reader.get(),data.data(),static_cast<DWORD>(data.size()),&got,&offset))got=0;
    }
    data.resize(got);
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
    shell.shown=from+keep;
    auto text=safe_output(data);
    put(r,"output",json_object_new_string_len(text.data(),static_cast<int>(text.size())));
    return r;
}
}
J *start_background_command(App *a,const char *command){
    auto &all=shells(a);
    size_t running=0;
    for(auto &shell:all.list){refresh_shell(*shell);running+=!shell->finished;}
    if(running>=8)return err(tr("后台命令已达 8 个上限，请先用 shell_kill 停止不需要的命令。"));
    const std::wstring executable=powershell_executable();
    if(executable.empty())return err("SystemRoot is unavailable.");
    wchar_t folder[MAX_PATH+1]{},name[MAX_PATH+1]{};
    if(!GetTempPathW(MAX_PATH,folder)||!GetTempFileNameW(folder,L"dsk",0,name))return err(tr("无法创建命令输出文件。"));
    SECURITY_ATTRIBUTES attr{sizeof attr,nullptr,TRUE};
    WinHandle writer(CreateFileW(name,GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,&attr,CREATE_ALWAYS,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr));
    if(!writer)return err(tr("无法创建命令输出文件。"));
    WinHandle reader(CreateFileW(name,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    WinHandle null(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&attr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    const bool use_sb=a->sandbox&&!win_env_truthy("DESK_NO_SANDBOX");
    const bool use_no_net=a->sandbox_no_net||win_env_truthy("DESK_SANDBOX_NO_NET");
    auto shell=std::make_unique<BackgroundShell>();
    shell->job=kill_job(use_sb);
    if(!reader||!null||!shell->job||!win_spawn(executable,powershell_arguments(executable,command,use_no_net),wide(a->root),null.get(),writer.get(),{null.get(),writer.get()},shell->job.get(),shell->process,use_sb,use_no_net))
        return err(tr("无法启动命令。"));
    writer.reset();
    shell->reader=std::move(reader);
    shell->command=command;
    shell->started=monotime();
    if(WaitForSingleObject(shell->process.get(),1000)==WAIT_OBJECT_0){
        J *r=shell_state(*shell);
        json_object_object_del(r,"shell_id");
        return r;
    }
    shell->id="ps-"+std::to_string(all.next++);
    all.list.push_back(std::move(shell));
    J *r=shell_state(*all.list.back());
    put(r,"note",str("Running in the background. Use shell_output to read new output and shell_kill to stop it; it stops when this conversation ends."));
    return r;
}
J *tool_shell_output(App *a,J *args){
    const char *id=sget(args,"shell_id");
    for(auto &shell:shells(a).list)if(shell->id==id)return shell_state(*shell);
    return err(tr("没有这个后台命令；shell_id 来自 run_shell 的返回结果。"));
}
J *tool_shell_kill(App *a,J *args){
    const char *id=sget(args,"shell_id");
    for(auto &shell:shells(a).list){
        if(shell->id!=id)continue;
        refresh_shell(*shell);
        if(!shell->finished){
            TerminateJobObject(shell->job.get(),1);
            WaitForSingleObject(shell->process.get(),2000);
            shell->killed=true;
        }
        return shell_state(*shell);
    }
    return err(tr("没有这个后台命令；shell_id 来自 run_shell 的返回结果。"));
}
size_t background_shell_count(App *a){
    if(!a->shell_data)return 0;
    size_t running=0;
    for(auto &shell:shells(a).list){refresh_shell(*shell);running+=!shell->finished;}
    return running;
}
void shells_destroy(App *a){
    if(!a->shell_data)return;
    auto *all=static_cast<Shells *>(a->shell_data);
    for(auto &shell:all->list)TerminateJobObject(shell->job.get(),1);
    delete all;
    a->shell_data=nullptr;
}
J *run_command(App *app,const char *command,int timeout_ms){
    if(timeout_ms<1||timeout_ms>300000)return err(tr("命令超时设置无效。"));
    const bool use_sb=app->sandbox&&!win_env_truthy("DESK_NO_SANDBOX");
    const bool use_no_net=app->sandbox_no_net||win_env_truthy("DESK_SANDBOX_NO_NET");
    std::wstring executable=powershell_executable();if(executable.empty())return err("SystemRoot is unavailable.");
    std::wstring arguments=powershell_arguments(executable,command,use_no_net);
    WinHandle read,write;if(!win_pipe(read,write))return err("Could not create command output pipe.");SetHandleInformation(read.get(),HANDLE_FLAG_INHERIT,0);
    SECURITY_ATTRIBUTES attr{sizeof attr,nullptr,TRUE};WinHandle null(CreateFileW(L"NUL",GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE,&attr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
    WinHandle job=kill_job(use_sb),process;if(!null||!job||!win_spawn(executable,std::move(arguments),wide(app->root),null.get(),write.get(),{null.get(),write.get()},job.get(),process,use_sb,use_no_net))return err(tr("无法启动命令。"));
    write.reset();std::string output,last_tail;bool truncated=false,timed=false;
    const double started=monotime(),until=started+timeout_ms/1000.0;double last_tail_at=started;DWORD code=1;
    auto latest_line=[](std::string_view chunk)->std::string{
        while(!chunk.empty()&&(chunk.back()=='\n'||chunk.back()=='\r'||chunk.back()==' '))chunk.remove_suffix(1);
        if(chunk.empty())return {};
        auto pos=chunk.find_last_of("\r\n");
        std::string_view raw=pos==std::string_view::npos?chunk:chunk.substr(pos+1);
        std::string clean;clean.reserve(raw.size());bool sp=false;
        for(size_t i=0;i<raw.size();){
            auto c=static_cast<unsigned char>(raw[i]);
            if(c==0x1b&&i+1<raw.size()&&raw[i+1]=='['){i+=2;while(i<raw.size()&&!(raw[i]>='@'&&raw[i]<='~'))i++;if(i<raw.size())i++;continue;}
            if(c<32||c==127){if(c=='\t'&&!clean.empty())sp=true;i++;continue;}
            if(c==' '){if(!clean.empty())sp=true;i++;continue;}
            if(sp){clean+=' ';sp=false;}
            clean+=static_cast<char>(c);i++;
        }
        size_t used=0;clean=display_utf8(clean,used,true);
        if(clean.size()>72){size_t cut=72;while(cut>0&&(static_cast<unsigned char>(clean[cut])&0xc0)==0x80)--cut;clean.resize(cut);clean+="…";}
        return clean;
    };
    auto drain=[&]{for(int i=0;i<16;i++){
        DWORD available=0;if(!PeekNamedPipe(read.get(),nullptr,0,nullptr,&available,nullptr)||!available)break;
        char bytes[4096];DWORD count=0;if(!ReadFile(read.get(),bytes,(std::min)(available,static_cast<DWORD>(sizeof bytes)),&count,nullptr)||!count)break;
        size_t keep=(std::min)(static_cast<size_t>(count),static_cast<size_t>(COMMAND_OUTPUT_LIMIT)-output.size());
        if(keep)output.append(bytes,keep);
        if(keep<count)truncated=true;
    }};
    for(;;){
        drain();
        check_exit(*app);
        progress_tick(app);
        if(WaitForSingleObject(process.get(),10)==WAIT_OBJECT_0){GetExitCodeProcess(process.get(),&code);break;}
        if(interrupted)break;
        const double now=monotime();
        if(now>=until){timed=true;break;}
        if(app->config&&!app->verbose&&!output.empty()&&now-started>=0.35&&now-last_tail_at>=0.25){
            last_tail_at=now;
            std::string_view tail_view=output.size()>512?std::string_view(output).substr(output.size()-512):std::string_view(output);
            std::string line=latest_line(tail_view);
            if(!line.empty()&&line!=last_tail){
                last_tail=line;
                if(app->task_events>=0)task_detail(*app,line);
                else if(isatty(1)){
                    char msg[256];snprintf(msg,sizeof msg,tr("正在执行命令：%s"),line.c_str());
                    progress(app,msg);
                }
            }
        }
    }
    if(!last_tail.empty()&&app->task_events>=0)task_detail(*app,"");
    TerminateJobObject(job.get(),1);WaitForSingleObject(process.get(),2000);drain();
    if(interrupted)return err(tr("命令已取消。"));
    if(truncated){
        size_t tail=0;
        while(tail<output.size()&&tail<3&&(static_cast<unsigned char>(output[output.size()-1-tail])&0xc0)==0x80)tail++;
        if(tail<output.size()){
            auto lead=static_cast<unsigned char>(output[output.size()-1-tail]);
            size_t expected=lead<0x80?1:lead<0xe0?2:lead<0xf0?3:lead<0xf8?4:0;
            if(expected>tail+1)output.resize(output.size()-1-tail);
        }
    }
    auto safe=safe_output(output);
    if(safe.size()>INT_MAX)return err(tr("命令输出无法转换为 JSON。"));
    J *result=obj();put(result,"exit_code",json_object_new_int64(code));put(result,"timed_out",json_object_new_boolean(timed));put(result,"truncated",json_object_new_boolean(truncated));
    put(result,"output",json_object_new_string_len(safe.data(),static_cast<int>(safe.size())));return result;
}
