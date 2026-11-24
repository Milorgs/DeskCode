// Language Server Protocol client: diagnostics after edits and read-only code
// navigation (definition, references, hover, document symbols).
//
// Servers are found on PATH from a built-in table (clangd, pyright, gopls,
// typescript-language-server, rust-analyzer) or configured in the user's
// <config>/lsp.json; a workspace cannot add commands. DESK_NO_LSP=1 disables
// everything, DESK_LSP_TIMEOUT_MS bounds the wait for diagnostics.
#include "desk.hpp"
#include "tasks.hpp"
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <stdexcept>
#include <vector>
#ifdef _WIN32
#include "windows/win.hpp"
#include <cwctype>
#else
#include <fcntl.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
extern char **environ;
#endif

namespace {
using Failure=std::runtime_error;
constexpr size_t MAX_MESSAGE=16*1024*1024;
constexpr size_t MAX_DIAGNOSTICS=20;

struct ServerSpec {
    std::string name;
    std::vector<std::vector<std::string>> commands; // alternatives, first found wins
    std::vector<std::string> extensions;
};

bool disabled(){
    const char *off=env_value("DESK_NO_LSP");
    return off&&*off&&strcmp(off,"0");
}
int timeout_ms(){
    const char *value=env_value("DESK_LSP_TIMEOUT_MS");
    if(value&&*value){long n=strtol(value,nullptr,10);if(n>=200&&n<=60000)return static_cast<int>(n);}
    return 4000;
}
std::string lower(std::string text){
    for(char &c:text)c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return text;
}
std::string extension_of(const std::string &path){
    auto slash=path.find_last_of("/\\"),dot=path.rfind('.');
    if(dot==std::string::npos||(slash!=std::string::npos&&dot<slash))return {};
    return lower(path.substr(dot));
}
std::string language_id(const std::string &ext){
    static const std::map<std::string,std::string> ids={
        {".c","c"},{".h","c"},{".cc","cpp"},{".cpp","cpp"},{".cxx","cpp"},{".hpp","cpp"},{".hh","cpp"},{".hxx","cpp"},
        {".m","objective-c"},{".mm","objective-cpp"},{".py","python"},{".pyi","python"},{".go","go"},{".rs","rust"},
        {".ts","typescript"},{".mts","typescript"},{".cts","typescript"},{".tsx","typescriptreact"},
        {".js","javascript"},{".mjs","javascript"},{".cjs","javascript"},{".jsx","javascriptreact"},
        {".java","java"},{".lua","lua"},{".rb","ruby"},{".php","php"},{".cs","csharp"},{".swift","swift"},{".zig","zig"}};
    auto found=ids.find(ext);
    return found==ids.end()?(ext.size()>1?ext.substr(1):"plaintext"):found->second;
}
std::vector<ServerSpec> builtin_specs(){
    return {
        {"clangd",{{"clangd","--log=error"}},{".c",".h",".cc",".cpp",".cxx",".hpp",".hh",".hxx",".m",".mm"}},
        {"python",{{"pyright-langserver","--stdio"},{"basedpyright-langserver","--stdio"},{"pylsp"}},{".py",".pyi"}},
        {"typescript",{{"typescript-language-server","--stdio"}},{".ts",".tsx",".js",".jsx",".mjs",".cjs",".mts",".cts"}},
        {"go",{{"gopls"}},{".go"}},
        {"rust",{{"rust-analyzer"}},{".rs"}},
    };
}
// User configuration: {"servers":{"name":{"command":"x","args":[],"extensions":[".x"],"disabled":false}}}.
// An entry with a built-in name replaces that built-in.
std::vector<ServerSpec> load_specs(const App &app){
    auto specs=builtin_specs();
    std::string path=std::string(app.config_dir)+"/lsp.json";
    std::unique_ptr<char,decltype(&free)> bytes(read_bytes(path.c_str(),262144,nullptr),free);
    if(!bytes)return specs;
    JsonPtr document(json_tokener_parse(bytes.get()));
    J *servers=get(document.get(),"servers");
    if(!json_object_is_type(servers,json_type_object))return specs;
    json_object_object_foreach(servers,name,entry){
        std::erase_if(specs,[&](const ServerSpec &spec){return spec.name==name;});
        if(bget(entry,"disabled")||!*sget(entry,"command"))continue;
        ServerSpec spec;spec.name=name;
        std::vector<std::string> command{sget(entry,"command")};
        J *args=get(entry,"args");
        for(size_t i=0;i<alen(args);i++)if(json_object_is_type(at(args,i),json_type_string))command.emplace_back(json_object_get_string(at(args,i)));
        spec.commands.push_back(std::move(command));
        J *extensions=get(entry,"extensions");
        for(size_t i=0;i<alen(extensions);i++){
            std::string ext=lower(json_object_get_string(at(extensions,i)));
            if(!ext.empty()&&ext[0]!='.')ext.insert(ext.begin(),'.');
            if(ext.size()>1)spec.extensions.push_back(ext);
        }
        if(!spec.extensions.empty())specs.insert(specs.begin(),std::move(spec));
    }
    return specs;
}
// Absolute path of an executable on PATH, or "" when it is not installed.
std::string find_program(const std::string &name){
    if(name.empty())return {};
#ifdef _WIN32
    for(const wchar_t *ext:{static_cast<const wchar_t *>(nullptr),L".exe",L".cmd",L".bat"}){
        wchar_t buffer[32768];
        DWORD n=SearchPathW(nullptr,wide(name).c_str(),ext,32768,buffer,nullptr);
        if(n>0&&n<32768)return utf8(std::wstring(buffer,n));
    }
    return {};
#else
    auto runnable=[](const std::string &candidate){
        struct stat info{};
        return stat(candidate.c_str(),&info)==0&&S_ISREG(info.st_mode)&&access(candidate.c_str(),X_OK)==0;
    };
    if(name.find('/')!=std::string::npos)return runnable(name)?name:std::string();
    const char *path=env_value("PATH");
    if(!path)return {};
    std::string entries=path;
    for(size_t start=0;start<=entries.size();){
        size_t end=entries.find(':',start);
        if(end==std::string::npos)end=entries.size();
        std::string dir=entries.substr(start,end-start);
        if(!dir.empty()){
            std::string candidate=dir+"/"+name;
            if(runnable(candidate))return candidate;
        }
        start=end+1;
    }
    return {};
#endif
}

std::string percent_encode(const std::string &path){
    static const char hex[]="0123456789ABCDEF";
    std::string out;
    for(unsigned char c:path){
        if(isalnum(c)||c=='/'||c=='-'||c=='_'||c=='.'||c=='~'||c==':')out+=static_cast<char>(c);
        else{out+='%';out+=hex[c>>4];out+=hex[c&15];}
    }
    return out;
}
std::string file_uri(std::string path){
#ifdef _WIN32
    std::replace(path.begin(),path.end(),'\\','/');
    if(!path.empty()&&path[0]!='/')path.insert(path.begin(),'/');
#endif
    return "file://"+percent_encode(path);
}
std::string uri_path(const std::string &uri){
    if(!uri.starts_with("file://"))return uri;
    std::string raw=uri.substr(7),out;
    for(size_t i=0;i<raw.size();i++){
        if(raw[i]=='%'&&i+2<raw.size()&&isxdigit(static_cast<unsigned char>(raw[i+1]))&&isxdigit(static_cast<unsigned char>(raw[i+2]))){
            out+=static_cast<char>(strtol(raw.substr(i+1,2).c_str(),nullptr,16));
            i+=2;
        }else out+=raw[i];
    }
#ifdef _WIN32
    if(out.size()>2&&out[0]=='/'&&out[2]==':')out.erase(0,1);
    std::replace(out.begin(),out.end(),'/','\\');
#endif
    return out;
}
// LSP positions count UTF-16 code units; tools count characters from 1.
int utf16_column(std::string_view line,int characters){
    int units=0,seen=0;
    for(size_t i=0;i<line.size()&&seen<characters;){
        auto c=static_cast<unsigned char>(line[i]);
        size_t width=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
        units+=width==4?2:1;
        i+=width;seen++;
    }
    return units;
}
int character_column(std::string_view line,int units){
    int counted=0,characters=0;
    for(size_t i=0;i<line.size()&&counted<units;){
        auto c=static_cast<unsigned char>(line[i]);
        size_t width=c<0x80?1:c<0xe0?2:c<0xf0?3:4;
        counted+=width==4?2:1;
        i+=width;characters++;
    }
    return characters;
}
std::string line_of(const std::string &text,int zero_based){
    size_t start=0;
    for(int i=0;i<zero_based;i++){
        start=text.find('\n',start);
        if(start==std::string::npos)return {};
        start++;
    }
    size_t end=text.find('\n',start);
    std::string line=text.substr(start,end==std::string::npos?std::string::npos:end-start);
    if(!line.empty()&&line.back()=='\r')line.pop_back();
    return line;
}

struct Document {int version=0;std::string text;};

class Server {
public:
    std::string name,root;
    bool broken=false;
    std::map<std::string,Document> documents;
    std::map<std::string,JsonPtr> diagnostics;
    std::map<std::string,int> diagnostics_version;
    Server(std::string n,std::string r):name(std::move(n)),root(std::move(r)){}
    ~Server(){
        if(started){
            try{request("shutdown",nullptr,1500);notify("exit",nullptr);}catch(...){}
        }
        stop();
    }
    void start(const std::vector<std::string> &command){
        launch(command);
        started=true;
        JsonPtr params(obj());
#ifdef _WIN32
        put(params.get(),"processId",json_object_new_int64(static_cast<int64_t>(GetCurrentProcessId())));
#else
        put(params.get(),"processId",json_object_new_int(getpid()));
#endif
        put(params.get(),"rootUri",str(file_uri(root).c_str()));
        put(params.get(),"rootPath",str(root.c_str()));
        J *folders=arr(),*folder=obj();
        put(folder,"uri",str(file_uri(root).c_str()));
        put(folder,"name",str(path_text(fs_path(root).filename()).c_str()));
        add(folders,folder);
        put(params.get(),"workspaceFolders",folders);
        J *info=obj();put(info,"name",str("DeskCode"));put(info,"version",str(VERSION));put(params.get(),"clientInfo",info);
        put(params.get(),"capabilities",json_tokener_parse(R"({
            "general":{"positionEncodings":["utf-16"]},
            "workspace":{"configuration":true,"workspaceFolders":true},
            "textDocument":{
                "synchronization":{"didSave":true,"dynamicRegistration":false},
                "publishDiagnostics":{"relatedInformation":false,"versionSupport":true},
                "definition":{"linkSupport":true},"references":{},
                "hover":{"contentFormat":["markdown","plaintext"]},
                "documentSymbol":{"hierarchicalDocumentSymbolSupport":true}}})"));
        request("initialize",params.get(),15000);
        notify("initialized",JsonPtr(obj()).get());
    }
    // Open or update a document and return true when the text changed.
    bool sync(const std::string &path,const std::string &text){
        const std::string uri=file_uri(path);
        auto found=documents.find(uri);
        if(found==documents.end()){
            JsonPtr params(obj());
            J *doc=obj();
            put(doc,"uri",str(uri.c_str()));
            put(doc,"languageId",str(language_id(extension_of(path)).c_str()));
            put(doc,"version",json_object_new_int(1));
            put(doc,"text",json_object_new_string_len(text.data(),static_cast<int>(text.size())));
            put(params.get(),"textDocument",doc);
            notify("textDocument/didOpen",params.get());
            documents[uri]={1,text};
            return true;
        }
        if(found->second.text==text)return false;
        found->second.version++;
        found->second.text=text;
        JsonPtr params(obj());
        J *doc=obj();
        put(doc,"uri",str(uri.c_str()));
        put(doc,"version",json_object_new_int(found->second.version));
        put(params.get(),"textDocument",doc);
        J *changes=arr(),*change=obj();
        put(change,"text",json_object_new_string_len(text.data(),static_cast<int>(text.size())));
        add(changes,change);
        put(params.get(),"contentChanges",changes);
        notify("textDocument/didChange",params.get());
        JsonPtr saved(obj());
        J *id=obj();put(id,"uri",str(uri.c_str()));put(saved.get(),"textDocument",id);
        notify("textDocument/didSave",saved.get());
        return true;
    }
    // Wait for diagnostics of the current version, then a short settle time
    // for servers that publish in stages.
    J *wait_diagnostics(const std::string &path,bool changed){
        const std::string uri=file_uri(path);
        const int version=documents[uri].version;
        if(changed)diagnostics.erase(uri);
        const double until=monotime()+timeout_ms()/1000.0;
        double settle=0;
        while(!interrupted){
            const double now=monotime();
            auto found=diagnostics.find(uri);
            const bool current=found!=diagnostics.end()&&diagnostics_version[uri]>=0&&
                (diagnostics_version[uri]==0||diagnostics_version[uri]>=version);
            if(current&&settle==0)settle=now+0.3;
            if(settle&&now>=settle)break;
            if(now>=until)break;
            pump(std::min(until,settle?settle:until)-now);
        }
        auto found=diagnostics.find(uri);
        return found==diagnostics.end()?nullptr:copy(found->second.get());
    }
    JsonPtr request(const char *method,J *params,int wait_ms){
        const int64_t id=++next_id;
        JsonPtr message(obj());
        put(message.get(),"jsonrpc",str("2.0"));
        put(message.get(),"id",json_object_new_int64(id));
        put(message.get(),"method",str(method));
        put(message.get(),"params",params?copy(params):obj());
        send(message.get());
        const double until=monotime()+wait_ms/1000.0;
        while(!interrupted&&monotime()<until){
            auto found=responses.find(id);
            if(found!=responses.end()){
                JsonPtr response=std::move(found->second);
                responses.erase(found);
                if(J *error=get(response.get(),"error"))throw Failure(std::string(method)+": "+sget(error,"message"));
                return JsonPtr(copy(get(response.get(),"result")));
            }
            pump(until-monotime());
        }
        throw Failure(std::string(method)+" timed out");
    }
    void notify(const char *method,J *params){
        JsonPtr message(obj());
        put(message.get(),"jsonrpc",str("2.0"));
        put(message.get(),"method",str(method));
        if(params)put(message.get(),"params",copy(params));
        send(message.get());
    }
private:
    bool started=false;
    int64_t next_id=0;
    std::string pending;
    std::map<int64_t,JsonPtr> responses;
#ifdef _WIN32
    WinHandle job{},process{},input{},output{};
#else
    pid_t child=-1;
    int input=-1,output=-1;
#endif
    void send(J *message){
        const char *body=json_object_to_json_string_ext(message,JSON_C_TO_STRING_PLAIN);
        std::string frame="Content-Length: "+std::to_string(strlen(body))+"\r\n\r\n"+body;
        write_all(frame);
    }
    // Read whatever arrives within `seconds` and dispatch complete messages.
    void pump(double seconds){
        std::string chunk=read_some(std::max(0.0,seconds));
        pending+=chunk;
        if(pending.size()>MAX_MESSAGE*2)throw Failure("language server output too large");
        for(;;){
            auto header_end=pending.find("\r\n\r\n");
            if(header_end==std::string::npos)return;
            size_t length=std::string::npos;
            std::string headers=lower(pending.substr(0,header_end));
            auto key=headers.find("content-length:");
            if(key!=std::string::npos)length=strtoul(headers.c_str()+key+15,nullptr,10);
            if(length==std::string::npos||length>MAX_MESSAGE)throw Failure("invalid language server message");
            if(pending.size()<header_end+4+length)return;
            JsonPtr message(json_tokener_parse(pending.substr(header_end+4,length).c_str()));
            pending.erase(0,header_end+4+length);
            if(message)dispatch(message.get());
        }
    }
    void dispatch(J *message){
        const char *method=sget(message,"method");
        J *id=get(message,"id");
        if(*method&&id){
            // Requests from the server: answer the ones servers need to proceed.
            JsonPtr reply(obj());
            put(reply.get(),"jsonrpc",str("2.0"));
            put(reply.get(),"id",copy(id));
            if(!strcmp(method,"workspace/configuration")){
                J *items=get(get(message,"params"),"items"),*result=arr();
                for(size_t i=0;i<alen(items);i++)add(result,nullptr);
                put(reply.get(),"result",result);
            }else if(!strcmp(method,"workspace/workspaceFolders")){
                J *folders=arr(),*folder=obj();
                put(folder,"uri",str(file_uri(root).c_str()));
                put(folder,"name",str(path_text(fs_path(root).filename()).c_str()));
                add(folders,folder);
                put(reply.get(),"result",folders);
            }else if(!strcmp(method,"client/registerCapability")||!strcmp(method,"client/unregisterCapability")
                     ||!strcmp(method,"window/workDoneProgress/create")||!strcmp(method,"window/showMessageRequest")){
                put(reply.get(),"result",nullptr);
            }else{
                J *error=obj();
                put(error,"code",json_object_new_int(-32601));
                put(error,"message",str("Method not supported"));
                put(reply.get(),"error",error);
            }
            send(reply.get());
            return;
        }
        if(!strcmp(method,"textDocument/publishDiagnostics")){
            J *params=get(message,"params");
            const std::string uri=sget(params,"uri");
            J *list=get(params,"diagnostics");
            if(uri.empty()||!json_object_is_type(list,json_type_array))return;
            diagnostics[uri].reset(copy(list));
            diagnostics_version[uri]=json_object_is_type(get(params,"version"),json_type_int)?iget(params,"version",0):0;
            return;
        }
        if(!*method&&json_object_is_type(id,json_type_int))responses[json_object_get_int64(id)].reset(copy(message));
    }
#ifdef _WIN32
    void launch(const std::vector<std::string> &command){
        std::string program=find_program(command[0]);
        if(program.empty())throw Failure("language server not found");
        WinHandle child_in,child_out;
        if(!win_pipe(child_in,input)||!win_pipe(output,child_out))throw Failure("could not create language server pipes");
        SetHandleInformation(input.get(),HANDLE_FLAG_INHERIT,0);
        SetHandleInformation(output.get(),HANDLE_FLAG_INHERIT,0);
        SECURITY_ATTRIBUTES attr{sizeof attr,nullptr,TRUE};
        WinHandle sink(CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&attr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!sink)throw Failure("could not open NUL");
        std::wstring exe=wide(program),line=quote_arg(exe);
        for(size_t i=1;i<command.size();i++)line+=L" "+quote_arg(wide(command[i]));
        std::wstring low=exe;for(auto &c:low)c=static_cast<wchar_t>(towlower(c));
        if(low.ends_with(L".cmd")||low.ends_with(L".bat")){
            const char *comspec=env_value("ComSpec");
            exe=comspec&&*comspec?wide(comspec):L"C:\\Windows\\System32\\cmd.exe";
            line=quote_arg(exe)+L" /d /s /c \""+line+L"\"";
        }
        job=kill_job();
        if(!job)throw Failure("could not create language server job");
        SIZE_T bytes=0;InitializeProcThreadAttributeList(nullptr,1,0,&bytes);
        std::vector<unsigned char> storage(bytes);
        auto *list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        std::vector<HANDLE> inherit{child_in.get(),child_out.get(),sink.get()};
        if(!InitializeProcThreadAttributeList(list,1,0,&bytes)||
           !UpdateProcThreadAttribute(list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherit.data(),inherit.size()*sizeof(HANDLE),nullptr,nullptr))
            throw Failure("could not configure language server handles");
        STARTUPINFOEXW start{};start.StartupInfo.cb=sizeof start;start.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        start.StartupInfo.hStdInput=child_in.get();start.StartupInfo.hStdOutput=child_out.get();start.StartupInfo.hStdError=sink.get();
        start.lpAttributeList=list;
        PROCESS_INFORMATION info{};
        std::wstring cwd=wide(root);
        auto environment=command_environment();
        BOOL created=CreateProcessW(exe.c_str(),line.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,environment.data(),cwd.c_str(),&start.StartupInfo,&info);
        DeleteProcThreadAttributeList(list);
        if(!created)throw Failure("could not start language server");
        WinHandle proc(info.hProcess),thread(info.hThread);
        if(!AssignProcessToJobObject(job.get(),proc.get())||ResumeThread(thread.get())==static_cast<DWORD>(-1)){
            TerminateProcess(proc.get(),1);
            throw Failure("could not start language server");
        }
        process=std::move(proc);
    }
    void stop(){
        input.reset();output.reset();
        if(job)TerminateJobObject(job.get(),1);
        if(process)WaitForSingleObject(process.get(),500);
    }
    void write_all(const std::string &bytes){
        if(!input||!win_write(input.get(),bytes))throw Failure("language server closed its input");
    }
    std::string read_some(double seconds){
        const double until=monotime()+seconds;
        for(;;){
            DWORD available=0;
            if(!PeekNamedPipe(output.get(),nullptr,0,nullptr,&available,nullptr))throw Failure("language server closed its output");
            if(available){
                std::string data(std::min<DWORD>(available,65536),'\0');
                DWORD count=0;
                if(!ReadFile(output.get(),data.data(),static_cast<DWORD>(data.size()),&count,nullptr)||!count)throw Failure("language server closed its output");
                data.resize(count);
                return data;
            }
            if(WaitForSingleObject(process.get(),0)==WAIT_OBJECT_0)throw Failure("language server exited");
            if(monotime()>=until)return {};
            Sleep(10);
        }
    }
#else
    void launch(const std::vector<std::string> &command){
        std::string program=find_program(command[0]);
        if(program.empty())throw Failure("language server not found");
        int to_child[2],from_child[2];
        if(pipe(to_child))throw Failure("could not create language server pipes");
        if(pipe(from_child)){close(to_child[0]);close(to_child[1]);throw Failure("could not create language server pipes");}
        for(int fd:{to_child[0],to_child[1],from_child[0],from_child[1]})fcntl(fd,F_SETFD,FD_CLOEXEC);
        std::vector<std::string> arguments=command;
        arguments[0]=program;
        std::vector<char *> argv;
        for(auto &argument:arguments)argv.push_back(argument.data());
        argv.push_back(nullptr);
        // Secrets stay out of the server's environment, like shell commands.
        std::vector<std::string> keep;
        for(char **entry=environ;*entry;entry++){
            std::string text=*entry,key=text.substr(0,text.find('='));
            std::string upper=key;for(char &c:upper)c=static_cast<char>(toupper(static_cast<unsigned char>(c)));
            if(upper.find("KEY")!=std::string::npos||upper.find("TOKEN")!=std::string::npos||upper.find("SECRET")!=std::string::npos||upper.find("PASSWORD")!=std::string::npos)continue;
            keep.push_back(std::move(text));
        }
        std::vector<char *> envp;
        for(auto &entry:keep)envp.push_back(entry.data());
        envp.push_back(nullptr);
#ifdef __linux__
        pid_t parent=getpid();
#endif
        child=fork();
        if(child==0){
            signal(SIGINT,SIG_IGN);signal(SIGTERM,SIG_DFL);signal(SIGPIPE,SIG_DFL);signal(SIGUSR1,SIG_DFL);
            if(setpgid(0,0)
#ifdef __linux__
               ||prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent
#endif
               ||chdir(root.c_str()))_exit(126);
            int sink=open("/dev/null",O_WRONLY);
            if(sink<0||dup2(to_child[0],0)<0||dup2(from_child[1],1)<0||dup2(sink,2)<0)_exit(126);
            execve(argv[0],argv.data(),envp.data());
            _exit(127);
        }
        close(to_child[0]);close(from_child[1]);
        if(child<0){close(to_child[1]);close(from_child[0]);throw Failure("could not start language server");}
        setpgid(child,child);
        input=to_child[1];output=from_child[0];
        fcntl(input,F_SETFL,O_NONBLOCK);fcntl(output,F_SETFL,O_NONBLOCK);
    }
    void stop(){
        if(input>=0){close(input);input=-1;}
        if(output>=0){close(output);output=-1;}
        if(child>0){
            int status=0;
            const double until=monotime()+0.5;
            pid_t ended=0;
            while((ended=waitpid(child,&status,WNOHANG))==0&&monotime()<until)usleep(10000);
            if(ended==0){
                kill(-child,SIGKILL);
                while(waitpid(child,&status,0)<0&&errno==EINTR){}
            }
            child=-1;
        }
    }
    void write_all(const std::string &bytes){
        size_t used=0;
        const double until=monotime()+10;
        while(used<bytes.size()){
            if(input<0||monotime()>until)throw Failure("language server closed its input");
            pollfd ready{input,POLLOUT,0};
            if(poll(&ready,1,50)<=0)continue;
            ssize_t n=write(input,bytes.data()+used,bytes.size()-used);
            if(n<0&&(errno==EAGAIN||errno==EINTR))continue;
            if(n<=0)throw Failure("language server closed its input");
            used+=static_cast<size_t>(n);
        }
    }
    std::string read_some(double seconds){
        pollfd ready{output,POLLIN,0};
        int wait=static_cast<int>(std::min(seconds,0.2)*1000);
        if(poll(&ready,1,std::max(wait,1))<=0)return {};
        char buffer[65536];
        ssize_t n=read(output,buffer,sizeof buffer);
        if(n<0&&(errno==EAGAIN||errno==EINTR))return {};
        if(n<=0)throw Failure("language server exited");
        return std::string(buffer,static_cast<size_t>(n));
    }
#endif
};

std::string lsp_config_stamp(const App &app){
    std::string stamp=app.config_dir;
    stamp+='\n';
    if(const char *path=env_value("PATH"))stamp+=path;
    std::error_code ec;
    const fs::path cfg=fs_path(app.config_dir)/"lsp.json";
    if(fs::exists(cfg,ec)){
        stamp+='\n';
        stamp+=std::to_string(static_cast<unsigned long long>(fs::file_size(cfg,ec)));
        stamp+=':';
        stamp+=std::to_string(static_cast<long long>(fs::last_write_time(cfg,ec).time_since_epoch().count()));
    }
    return stamp;
}

struct Registry {
    std::string stamp;
    std::vector<ServerSpec> specs;
    std::map<std::string,std::unique_ptr<Server>> servers; // by spec name
    std::map<std::string,std::vector<std::string>> resolved; // spec name -> command with absolute program
};
Registry &registry(App &app){
    const std::string stamp=lsp_config_stamp(app);
    if(!app.lsp_data){
        auto *created=new Registry;
        created->stamp=stamp;
        created->specs=load_specs(app);
        app.lsp_data=created;
    }else{
        auto *existing=static_cast<Registry *>(app.lsp_data);
        if(existing->stamp!=stamp){
            existing->stamp=stamp;
            existing->specs=load_specs(app);
            existing->resolved.clear();
        }
    }
    return *static_cast<Registry *>(app.lsp_data);
}
// The installed server for a file, or nullptr with a reason.
const ServerSpec *spec_for(Registry &reg,const std::string &path,std::vector<std::string> &command){
    const std::string ext=extension_of(path);
    for(const auto &spec:reg.specs){
        if(std::find(spec.extensions.begin(),spec.extensions.end(),ext)==spec.extensions.end())continue;
        auto cached=reg.resolved.find(spec.name);
        if(cached!=reg.resolved.end()){
            if(cached->second.empty())continue;
            command=cached->second;
            return &spec;
        }
        for(const auto &candidate:spec.commands){
            if(!find_program(candidate[0]).empty()){
                reg.resolved[spec.name]=candidate;
                command=candidate;
                return &spec;
            }
        }
        reg.resolved[spec.name]={};
    }
    return nullptr;
}
Server *server_for(App &app,const std::string &path,std::string &reason){
    if(disabled()){reason="language servers are disabled (DESK_NO_LSP)";return nullptr;}
    auto &reg=registry(app);
    std::vector<std::string> command;
    const ServerSpec *spec=spec_for(reg,path,command);
    if(!spec){reason="no language server is installed for "+(extension_of(path).empty()?std::string("this file"):extension_of(path))+" files";return nullptr;}
    auto &slot=reg.servers[spec->name];
    if(slot&&slot->broken){reason="the "+spec->name+" language server failed earlier in this session";return nullptr;}
    if(!slot){
        slot=std::make_unique<Server>(spec->name,app.root);
        try{slot->start(command);}
        catch(const std::exception &error){
            slot->broken=true;
            reason="could not start "+spec->name+": "+error.what();
            return nullptr;
        }
    }
    return slot.get();
}
std::string read_text(const std::string &path){
    size_t length=0;
    std::unique_ptr<char,decltype(&free)> bytes(read_bytes(path.c_str(),EDIT_FILE_LIMIT,&length),free);
    if(!bytes)throw Failure("could not read the file (missing, not a regular file, or larger than 4 MB)");
    std::string text(bytes.get(),length);
    if(!valid_utf8(text))throw Failure("the file is not UTF-8 text");
    return text;
}
std::string display_path(const App &app,const std::string &path){
    std::string root=app.root;
    if(path.size()>root.size()&&path.starts_with(root)&&(path[root.size()]=='/'||path[root.size()]=='\\'))return path.substr(root.size()+1);
    return path;
}
// Errors and warnings in the shape the model reads, newest text positions.
J *summarize(const App &app,const std::string &path,J *list,const std::string &text,std::string &summary){
    J *out=arr();
    size_t errors=0,warnings=0;
    for(size_t i=0;i<alen(list);i++){
        J *item=at(list,i);
        int severity=iget(item,"severity",1);
        if(severity>2)continue;
        severity==1?errors++:warnings++;
        if(alen(out)>=MAX_DIAGNOSTICS)continue;
        J *start=get(get(item,"range"),"start");
        const int line=iget(start,"line",0);
        J *entry=obj();
        put(entry,"line",json_object_new_int(line+1));
        put(entry,"column",json_object_new_int(character_column(line_of(text,line),iget(start,"character",0))+1));
        put(entry,"severity",str(severity==1?"error":"warning"));
        std::string message=sget(item,"message");
        if(message.size()>500)message.resize(500);
        size_t used=0;
        message=display_utf8(message,used,true);
        put(entry,"message",str(message.c_str()));
        if(*sget(item,"source"))put(entry,"source",str(sget(item,"source")));
        J *code=get(item,"code");
        if(json_object_is_type(code,json_type_string)||json_object_is_type(code,json_type_int))put(entry,"code",str(json_object_get_string(code)));
        add(out,entry);
    }
    (void)app;(void)path;
    char text_summary[96];
    if(!errors&&!warnings)snprintf(text_summary,sizeof text_summary,"no errors or warnings");
    else snprintf(text_summary,sizeof text_summary,"%zu error%s, %zu warning%s",errors,errors==1?"":"s",warnings,warnings==1?"":"s");
    summary=text_summary;
    return out;
}
J *locations(const App &app,J *result,size_t limit){
    J *out=arr();
    auto one=[&](J *item){
        if(alen(out)>=limit)return;
        const char *uri=*sget(item,"targetUri")?sget(item,"targetUri"):sget(item,"uri");
        J *range=get(item,"targetSelectionRange")?get(item,"targetSelectionRange"):get(item,"range");
        if(!*uri)return;
        const std::string path=uri_path(uri);
        const int line=iget(get(range,"start"),"line",0);
        J *entry=obj();
        put(entry,"path",str(display_path(app,path).c_str()));
        put(entry,"line",json_object_new_int(line+1));
        std::string preview;
        try{
            const std::string text=read_text(path);
            preview=line_of(text,line);
            put(entry,"column",json_object_new_int(character_column(preview,iget(get(range,"start"),"character",0))+1));
        }catch(...){}
        while(!preview.empty()&&(preview.front()==' '||preview.front()=='\t'))preview.erase(0,1);
        if(preview.size()>200)preview.resize(200);
        size_t used=0;
        preview=display_utf8(preview,used,true);
        if(!preview.empty())put(entry,"preview",str(preview.c_str()));
        add(out,entry);
    };
    if(json_object_is_type(result,json_type_array))for(size_t i=0;i<alen(result);i++)one(at(result,i));
    else if(json_object_is_type(result,json_type_object))one(result);
    return out;
}
std::string hover_text(J *contents){
    if(json_object_is_type(contents,json_type_string))return json_object_get_string(contents);
    if(json_object_is_type(contents,json_type_object))return sget(contents,"value");
    std::string text;
    for(size_t i=0;i<alen(contents);i++){
        if(!text.empty())text+="\n\n";
        text+=hover_text(at(contents,i));
    }
    return text;
}
void flatten_symbols(J *list,J *out,int depth,const std::string &container){
    static const char *kinds[]={"","file","module","namespace","package","class","method","property","field","constructor",
        "enum","interface","function","variable","constant","string","number","boolean","array","object","key","null",
        "enum member","struct","event","operator","type parameter"};
    for(size_t i=0;i<alen(list)&&alen(out)<300;i++){
        J *item=at(list,i);
        const int kind=iget(item,"kind",0);
        J *range=get(item,"selectionRange")?get(item,"selectionRange"):get(item,"range");
        if(!range)range=get(get(item,"location"),"range");
        J *entry=obj();
        put(entry,"name",str(sget(item,"name")));
        put(entry,"kind",str(kind>0&&kind<27?kinds[kind]:"symbol"));
        put(entry,"line",json_object_new_int(iget(get(range,"start"),"line",0)+1));
        const std::string parent=*sget(item,"containerName")?sget(item,"containerName"):container;
        if(!parent.empty())put(entry,"container",str(parent.c_str()));
        add(out,entry);
        if(depth<4)flatten_symbols(get(item,"children"),out,depth+1,sget(item,"name"));
    }
}
}

bool lsp_available(const App &app){
    if(disabled())return false;
    const std::string key=lsp_config_stamp(app);
    static std::map<std::string,bool> cache;
    auto found=cache.find(key);
    if(found!=cache.end())return found->second;
    bool any=false;
    for(const auto &spec:load_specs(app)){
        for(const auto &candidate:spec.commands)if(!find_program(candidate[0]).empty()){any=true;break;}
        if(any)break;
    }
    cache[key]=any;
    return any;
}

void lsp_attach_diagnostics(App &app,const char *full_path,J *result){
    if(disabled()||!full_path||!*full_path||tool_failed(result))return;
    std::string reason;
    try{
        Server *server=server_for(app,full_path,reason);
        if(!server)return;
        const std::string text=read_text(full_path);
        const bool changed=server->sync(full_path,text);
        JsonPtr list(server->wait_diagnostics(full_path,changed));
        if(!list)return;
        std::string summary;
        put(result,"diagnostics",summarize(app,full_path,list.get(),text,summary));
        put(result,"diagnostics_summary",str((server->name+": "+summary).c_str()));
    }catch(const std::exception &){
        // A failing server never fails the edit; it is skipped from now on.
        auto &reg=registry(app);
        for(auto &[name,server]:reg.servers)if(server)server->broken=true;
    }
}

J *tool_lsp(App *a,J *args){
    const std::string action=sget(args,"action");
    char full[PATH_MAX];
    if(resolve_file(a,sget(args,"path"),full))return err(a->error);
    std::string reason;
    try{
        Server *server=server_for(*a,full,reason);
        if(!server)return err(("No language server: "+reason+".").c_str());
        const std::string text=read_text(full);
        const bool changed=server->sync(full,text);
        if(action=="diagnostics"){
            JsonPtr list(server->wait_diagnostics(full,changed||!server->diagnostics.count(file_uri(full))));
            J *r=obj();
            put(r,"path",str(display_path(*a,full).c_str()));
            put(r,"server",str(server->name.c_str()));
            std::string summary="diagnostics not reported within the time limit";
            put(r,"diagnostics",list?summarize(*a,full,list.get(),text,summary):arr());
            put(r,"summary",str(summary.c_str()));
            return r;
        }
        JsonPtr params(obj());
        J *doc=obj();put(doc,"uri",str(file_uri(full).c_str()));put(params.get(),"textDocument",doc);
        if(action=="symbols"){
            JsonPtr result=server->request("textDocument/documentSymbol",params.get(),timeout_ms()+4000);
            J *r=obj(),*symbols=arr();
            flatten_symbols(result.get(),symbols,0,"");
            put(r,"path",str(display_path(*a,full).c_str()));
            put(r,"symbols",symbols);
            return r;
        }
        const int line=iget(args,"line",0),column=iget(args,"column",1);
        if(line<1)return err("line (1-based) is required for this action.");
        J *position=obj();
        put(position,"line",json_object_new_int(line-1));
        put(position,"character",json_object_new_int(utf16_column(line_of(text,line-1),std::max(column,1)-1)));
        put(params.get(),"position",position);
        J *r=obj();
        put(r,"path",str(display_path(*a,full).c_str()));
        put(r,"line",json_object_new_int(line));
        if(action=="definition"){
            JsonPtr result=server->request("textDocument/definition",params.get(),timeout_ms()+4000);
            put(r,"definitions",locations(*a,result.get(),20));
        }else if(action=="references"){
            J *context=obj();put(context,"includeDeclaration",json_object_new_boolean(1));put(params.get(),"context",context);
            JsonPtr result=server->request("textDocument/references",params.get(),timeout_ms()+6000);
            J *found=locations(*a,result.get(),100);
            put(r,"total",json_object_new_int(static_cast<int>(alen(result.get()))));
            put(r,"references",found);
        }else if(action=="hover"){
            JsonPtr result=server->request("textDocument/hover",params.get(),timeout_ms()+4000);
            std::string hover=hover_text(get(result.get(),"contents"));
            if(hover.size()>4000)hover.resize(4000);
            size_t used=0;
            hover=display_utf8(hover,used,true);
            put(r,"hover",str(hover.c_str()));
        }else{
            json_object_put(r);
            return err("action must be diagnostics, definition, references, hover or symbols.");
        }
        return r;
    }catch(const std::exception &error){
        return err((std::string("Language server request failed: ")+error.what()).c_str());
    }
}

void lsp_destroy(App *a){
    delete static_cast<Registry *>(a->lsp_data);
    a->lsp_data=nullptr;
}
