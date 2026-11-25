#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#endif
#include "mcp.hpp"
#include "tasks.hpp"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <ctime>
#include <map>
#include <stdexcept>
#ifdef _WIN32
#include "windows/win.hpp"
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <sys/wait.h>
#include <unistd.h>
extern char **environ;
#endif

namespace {
constexpr size_t config_limit=256000;
constexpr const char *protocol="2025-06-18";
using Failure=std::runtime_error;
std::string encoded(J *value){return json_object_to_json_string_ext(value,JSON_C_TO_STRING_PLAIN);}
JsonPtr parsed(const std::string &text){
    if(text.size()>RESPONSE_LIMIT||!valid_utf8(text))throw Failure("MCP response is too large or is not UTF-8.");
    json_tokener *parser=json_tokener_new();
    json_tokener_set_flags(parser,JSON_TOKENER_STRICT);
    J *value=json_tokener_parse_ex(parser,text.c_str(),static_cast<int>(text.size()+1));
    auto parse_error=json_tokener_get_error(parser);
    bool ok=parse_error==json_tokener_success;
    size_t end=json_tokener_get_parse_end(parser);json_tokener_free(parser);
    for(size_t i=end;i<text.size();i++)if(!isspace(static_cast<unsigned char>(text[i])))ok=false;
    JsonPtr result(value);
    if(!ok){
        size_t row=1,column=1;
        for(size_t i=0;i<std::min(end,text.size());i++){
            if(text[i]=='\n'){row++;column=1;}else column++;
        }
        char message[256];
        snprintf(message,sizeof message,tr("JSON 格式错误（第 %zu 行，第 %zu 字节）：%s"),row,column,parse_error==json_tokener_success?tr("JSON 后有多余内容"):json_tokener_error_desc(parse_error));
        throw Failure(message);
    }
    if(!json_object_is_type(value,json_type_object))throw Failure(tr("MCP JSON 顶层必须是对象。"));
    return result;
}
bool string_value(J *value,bool empty=false){
    if(!json_object_is_type(value,json_type_string))return false;
    const char *text=json_object_get_string(value);
    return (empty||*text)&&strlen(text)==static_cast<size_t>(json_object_get_string_len(value))&&valid_utf8(text);
}
bool visible(std::string_view value){
    return std::none_of(value.begin(),value.end(),[](unsigned char c){return c<32||c==127;});
}
bool valid_http_endpoint(const char *raw_url){
    CURLU *url=curl_url();
    if(!url)return false;
    char *scheme=nullptr,*host=nullptr,*part=nullptr;
    bool ok=curl_url_set(url,CURLUPART_URL,raw_url,0)==CURLUE_OK;
    curl_url_get(url,CURLUPART_SCHEME,&scheme,0);curl_url_get(url,CURLUPART_HOST,&host,0);
    ok=ok&&scheme&&host&&(!strcmp(scheme,"https")||(!strcmp(scheme,"http")&&(!strcmp(host,"localhost")||!strcmp(host,"127.0.0.1")||!strcmp(host,"[::1]"))));
    for(auto key:{CURLUPART_USER,CURLUPART_PASSWORD,CURLUPART_FRAGMENT}){if(curl_url_get(url,key,&part,0)==CURLUE_OK)ok=false;curl_free(part);part=nullptr;}
    curl_free(scheme);curl_free(host);curl_url_cleanup(url);
    return ok;
}
void validate(J *servers){
    if(!json_object_is_type(servers,json_type_object)||json_object_object_length(servers)>16)throw Failure("MCP configuration must contain at most 16 servers.");
    json_object_object_foreach(servers,name,entry){
        if(!*name||strlen(name)>100||!valid_utf8(name)||!visible(name)||!json_object_is_type(entry,json_type_object))throw Failure("Invalid MCP server name or configuration.");
        const bool remote=get(entry,"url")!=nullptr;
        if(get(entry,"disabled")&&!json_object_is_type(get(entry,"disabled"),json_type_boolean))throw Failure("MCP disabled must be true or false.");
        const char *type=sget(entry,"type");
        if(*type&&strcmp(type,remote?"http":"stdio")&&!(remote&&!strcmp(type,"streamable-http")))throw Failure("Supported MCP transports are stdio and Streamable HTTP.");
        if(get(entry,"timeoutMs")&&(!json_object_is_type(get(entry,"timeoutMs"),json_type_int)||iget(entry,"timeoutMs",0)<1000||iget(entry,"timeoutMs",0)>300000))throw Failure("MCP timeoutMs must be between 1000 and 300000.");
        if(remote){
            if(!string_value(get(entry,"url"))||get(entry,"command"))throw Failure("Specify either an MCP URL or a command.");
            if(!valid_http_endpoint(sget(entry,"url")))throw Failure("MCP URL must use HTTPS, or HTTP on localhost, without embedded credentials or a fragment.");
            if(J *oauth=get(entry,"oauth")){
                if(!json_object_is_type(oauth,json_type_object))throw Failure("MCP oauth configuration must be an object.");
                for(const char *f:{"clientId","clientSecret","authorizationUrl","tokenUrl","scopes","scope"}){
                    if(J *v=get(oauth,f);v&&(!string_value(v,true)||!visible(json_object_get_string(v))))throw Failure("Invalid MCP oauth field.");
                }
                for(const char *u:{"authorizationUrl","tokenUrl"}){
                    const char *uv=sget(oauth,u);
                    if(*uv&&!valid_http_endpoint(uv))throw Failure("MCP oauth endpoint must use HTTPS, or HTTP on localhost.");
                }
            }
        }else{
            if(get(entry,"oauth"))throw Failure("MCP oauth is only supported for remote HTTP servers.");
            if(!string_value(get(entry,"command")))throw Failure("MCP stdio requires a command.");
            J *args=get(entry,"args");
            if(args&&!json_object_is_type(args,json_type_array))throw Failure("MCP args must be an array of strings.");
            for(size_t i=0;i<alen(args);i++)if(!string_value(at(args,i),true))throw Failure("Invalid MCP command argument.");
            if(get(entry,"cwd")&&!string_value(get(entry,"cwd")))throw Failure("Invalid MCP working directory.");
        }
        for(const char *key:{"env","headers"}){
            J *map=get(entry,key);if(!map)continue;
            if(!json_object_is_type(map,json_type_object))throw Failure("MCP env and headers must be string maps.");
            json_object_object_foreach(map,field,value){
                if(!*field||!visible(field)||!string_value(value,true))throw Failure("Invalid MCP environment or header value.");
                if(!strcmp(key,"env")&&strchr(field,'='))throw Failure("Invalid MCP environment variable name.");
                if(!strcmp(key,"headers")){
                    if(!visible(json_object_get_string(value))||std::any_of(field,field+strlen(field),[](unsigned char c){return !isalnum(c)&&c!='-'&&c!='_';}))throw Failure("Invalid MCP HTTP header.");
                    std::string lower=field;std::transform(lower.begin(),lower.end(),lower.begin(),[](unsigned char c){return std::tolower(c);});
                    for(const char *reserved:{"host","content-length","transfer-encoding","connection","accept","content-type","mcp-session-id","mcp-protocol-version"})if(lower==reserved)throw Failure("MCP configuration contains a reserved HTTP header.");
                }
            }
        }
        if(encoded(entry).size()>64000)throw Failure("MCP server configuration is too large.");
    }
}
std::string config_path(App &app){return std::string(app.config_dir)+"/mcp.json";}
std::string tokens_path(const App &app){return std::string(app.config_dir)+"/mcp_tokens.json";}

std::string url_encode(std::string_view text){
    static constexpr char hex[]="0123456789ABCDEF";
    std::string out;
    out.reserve(text.size()*2);
    for(unsigned char c:text){
        if(isalnum(c)||c=='-'||c=='_'||c=='.'||c=='~')out+=static_cast<char>(c);
        else{out+='%';out+=hex[c>>4];out+=hex[c&0xf];}
    }
    return out;
}
std::string url_decode(std::string_view text){
    std::string out;
    out.reserve(text.size());
    for(size_t i=0;i<text.size();i++){
        if(text[i]=='+')out+=' ';
        else if(text[i]=='%'&&i+2<text.size()&&isxdigit(static_cast<unsigned char>(text[i+1]))&&isxdigit(static_cast<unsigned char>(text[i+2]))){
            char buf[3]{text[i+1],text[i+2],0};
            out+=static_cast<char>(strtol(buf,nullptr,16));
            i+=2;
        }else out+=text[i];
    }
    return out;
}
std::string base64url_encode(const unsigned char *bytes,size_t len){
    std::string b64(((len+2)/3)*4,'\0');
    EVP_EncodeBlock(reinterpret_cast<unsigned char *>(b64.data()),bytes,static_cast<int>(len));
    while(!b64.empty()&&(b64.back()=='='||b64.back()=='\0'))b64.pop_back();
    for(char &c:b64){if(c=='+')c='-';else if(c=='/')c='_';}
    return b64;
}
JsonPtr load_oauth_tokens(const App &app){
    size_t len=0;
    std::unique_ptr<char,decltype(&free)> raw(read_bytes(tokens_path(app).c_str(),config_limit,&len),free);
    if(!raw)return JsonPtr(obj());
    J *doc=json_tokener_parse(raw.get());
    if(!json_object_is_type(doc,json_type_object)){json_object_put(doc);return JsonPtr(obj());}
    return JsonPtr(doc);
}
bool save_oauth_tokens(const App &app,J *tokens){
    std::string bytes=encoded(tokens);
    return !mkdirs(app.config_dir,0700)&&!atomic_file(tokens_path(app).c_str(),bytes.data(),bytes.size(),0600);
}
std::string oauth_http_call(const char *method,const std::string &url,const std::string &body,const char *content_type,long &status_out){
    status_out=0;
    CurlHandle handle(curl_easy_init());
    if(!handle)return {};
    std::string resp;
    HttpHeaders headers;
    headers.append("Accept: application/json");
    if(content_type&&*content_type)headers.append(std::string("Content-Type: ")+content_type);
    auto writer=+[](char *ptr,size_t sz,size_t cnt,void *ud)->size_t{
        auto *out=static_cast<std::string *>(ud);
        if(sz&&cnt>SIZE_MAX/sz)return 0;
        size_t n=sz*cnt;
        if(n>1048576-out->size())return 0;
        try{out->append(ptr,n);}catch(...){return 0;}
        return n;
    };
    CurlRequest req(handle.get());
    curl_easy_setopt(handle.get(),CURLOPT_URL,url.c_str());
    curl_easy_setopt(handle.get(),CURLOPT_HTTPHEADER,headers.get());
    curl_easy_setopt(handle.get(),CURLOPT_CONNECTTIMEOUT_MS,5000L);
    curl_easy_setopt(handle.get(),CURLOPT_TIMEOUT_MS,15000L);
    curl_easy_setopt(handle.get(),CURLOPT_NOSIGNAL,1L);
    curl_easy_setopt(handle.get(),CURLOPT_FOLLOWLOCATION,0L);
    curl_easy_setopt(handle.get(),CURLOPT_WRITEFUNCTION,writer);
    curl_easy_setopt(handle.get(),CURLOPT_WRITEDATA,&resp);
    if(strcmp(method,"POST")==0){
        curl_easy_setopt(handle.get(),CURLOPT_POST,1L);
        curl_easy_setopt(handle.get(),CURLOPT_POSTFIELDS,body.c_str());
        curl_easy_setopt(handle.get(),CURLOPT_POSTFIELDSIZE,static_cast<long>(body.size()));
    }
    if(curl_easy_perform(handle.get())!=CURLE_OK)return {};
    curl_easy_getinfo(handle.get(),CURLINFO_RESPONSE_CODE,&status_out);
    return resp;
}
bool headers_have_authorization(J *headers){
    if(!headers)return false;
    json_object_object_foreach(headers,k,v){
        (void)v;
        std::string low=k;
        for(char &c:low)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if(low=="authorization")return true;
    }
    return false;
}
std::string resolve_oauth_bearer(App &app,const std::string &server_name,J *server_cfg){
    if(headers_have_authorization(get(server_cfg,"headers")))return {};
    JsonPtr all=load_oauth_tokens(app);
    J *entry=get(all.get(),server_name.c_str());
    if(!json_object_is_type(entry,json_type_object))return {};
    std::string access=sget(entry,"access_token");
    const std::string refresh=sget(entry,"refresh_token");
    const int64_t expires_at=json_object_get_int64(get(entry,"expires_at"));
    const int64_t now=static_cast<int64_t>(std::time(nullptr));
    if(expires_at>0&&now+30>=expires_at&&!refresh.empty()){
        J *oauth=get(server_cfg,"oauth");
        std::string token_url=*sget(entry,"token_url")?sget(entry,"token_url"):sget(oauth,"tokenUrl");
        std::string client_id=*sget(entry,"client_id")?sget(entry,"client_id"):sget(oauth,"clientId");
        std::string client_secret=sget(oauth,"clientSecret");
        if(!token_url.empty()&&valid_http_endpoint(token_url.c_str())){
            std::string form="grant_type=refresh_token&refresh_token="+url_encode(refresh);
            if(!client_id.empty())form+="&client_id="+url_encode(client_id);
            if(!client_secret.empty())form+="&client_secret="+url_encode(client_secret);
            long status=0;
            std::string body=oauth_http_call("POST",token_url,form,"application/x-www-form-urlencoded",status);
            if(status>=200&&status<300){
                JsonPtr tok(json_tokener_parse(body.c_str()));
                if(tok&&*sget(tok.get(),"access_token")){
                    access=sget(tok.get(),"access_token");
                    put(entry,"access_token",str(access.c_str()));
                    if(*sget(tok.get(),"refresh_token"))put(entry,"refresh_token",str(sget(tok.get(),"refresh_token")));
                    int exp_in=iget(tok.get(),"expires_in",0);
                    put(entry,"expires_at",json_object_new_int64(exp_in>0?now+exp_in:0));
                    save_oauth_tokens(app,all.get());
                }
            }
        }
    }
    return access;
}

struct Client {
    App &app;
    std::string name,session,version;
    JsonPtr config;
    CurlHandle http_handle{};
    long next_id=0;
    std::string pending;
    bool has_tools=false,has_resources=false,has_prompts=false;
    size_t tool_count=0,resource_count=0,prompt_count=0;
#ifdef _WIN32
    WinHandle job{},child_proc{},input_pipe{},output_pipe{};
#else
    pid_t child=-1;
    int input=-1,output=-1;
#endif
    Client(App &a,const char *n,J *c):app(a),name(n),config(copy(c)){}
    bool healthy(){
        if(*sget(config.get(),"url"))return !session.empty()||version.empty();
#ifdef _WIN32
        if(!child_proc||!input_pipe||!output_pipe)return false;
        return WaitForSingleObject(child_proc.get(),0)==WAIT_TIMEOUT;
#else
        if(child<=0||input<0||output<0)return false;
        int status=0;
        pid_t ended=waitpid(child,&status,WNOHANG);
        if(ended==0)return true;
        if(ended==child)child=-1;
        return false;
#endif
    }
    ~Client(){
        close_remote();
#ifdef _WIN32
        input_pipe.reset();
        output_pipe.reset();
        if(child_proc&&WaitForSingleObject(child_proc.get(),0)==WAIT_TIMEOUT){
            if(job)TerminateJobObject(job.get(),1);
            else TerminateProcess(child_proc.get(),1);
            WaitForSingleObject(child_proc.get(),500);
        }
#else
        if(input>=0)close(input);
        if(output>=0)close(output);
        if(child>0){
            kill(-child,SIGTERM);
            int status=0;double until=monotime()+0.3;
            pid_t result=0;
            while((result=waitpid(child,&status,WNOHANG))==0&&monotime()<until)usleep(10000);
            if(result==0){
                kill(-child,SIGKILL);
                while(waitpid(child,&status,0)<0&&errno==EINTR){}
            }
        }
#endif
    }
    void close_remote() noexcept {
        if(session.empty()||!*sget(config.get(),"url"))return;
        try{
            CurlHandle handle=std::move(http_handle);
            if(!handle)handle.reset(curl_easy_init());
            if(!handle)return;
            HttpHeaders headers;
            headers.append("Mcp-Session-Id: "+session);
            if(!version.empty())headers.append("MCP-Protocol-Version: "+version);
            J *configured=get(config.get(),"headers");
            if(configured){json_object_object_foreach(configured,key,value){headers.append(std::string(key)+": "+json_object_get_string(value));}}
            std::string bearer=resolve_oauth_bearer(app,name,config.get());
            if(!bearer.empty())headers.append("Authorization: Bearer "+bearer);
            CurlRequest request(handle.get());
            auto discard=+[](char *,size_t size,size_t count,void *)->size_t{return size*count;};
            curl_easy_setopt(handle.get(),CURLOPT_URL,sget(config.get(),"url"));
            curl_easy_setopt(handle.get(),CURLOPT_CUSTOMREQUEST,"DELETE");
            curl_easy_setopt(handle.get(),CURLOPT_HTTPHEADER,headers.get());
            curl_easy_setopt(handle.get(),CURLOPT_CONNECTTIMEOUT_MS,500L);
            curl_easy_setopt(handle.get(),CURLOPT_TIMEOUT_MS,1000L);
            curl_easy_setopt(handle.get(),CURLOPT_NOSIGNAL,1L);
            curl_easy_setopt(handle.get(),CURLOPT_FOLLOWLOCATION,0L);
            curl_easy_setopt(handle.get(),CURLOPT_WRITEFUNCTION,discard);
            curl_easy_perform(handle.get());
        }catch(...){} // Best-effort shutdown must not interrupt local cleanup.
    }
    void check(double until){
        check_exit(app);
        progress_tick(&app);
        if(app_cancelled(app))throw Failure("MCP operation cancelled.");
        if(monotime()>until)throw Failure("MCP operation timed out. The operation was not automatically repeated.");
    }
#ifdef _WIN32
    void start(){
        WinHandle child_in,child_out;
        if(!win_pipe(child_in,input_pipe))throw Failure("Could not create MCP input pipe.");
        if(!win_pipe(output_pipe,child_out)){input_pipe.reset();throw Failure("Could not create MCP output pipe.");}
        SetHandleInformation(input_pipe.get(),HANDLE_FLAG_INHERIT,0);
        SetHandleInformation(output_pipe.get(),HANDLE_FLAG_INHERIT,0);
        SECURITY_ATTRIBUTES attr{sizeof attr,nullptr,TRUE};
        WinHandle sink(CreateFileW(L"NUL",GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE,&attr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr));
        if(!sink){input_pipe.reset();output_pipe.reset();throw Failure("Could not open NUL device.");}

        std::wstring raw_cmd=wide(sget(config.get(),"command"));
        std::wstring resolved_exe;
        bool is_batch=false;
        auto probe_ext=[&](const wchar_t *ext)->bool{
            wchar_t buf[32768];
            DWORD n=SearchPathW(nullptr,raw_cmd.c_str(),ext,32768,buf,nullptr);
            if(n>0&&n<32768){resolved_exe.assign(buf,n);return true;}
            return false;
        };
        if(probe_ext(nullptr)||probe_ext(L".exe")||probe_ext(L".cmd")||probe_ext(L".bat")){
            std::wstring low=resolved_exe;
            for(wchar_t &c:low)c=static_cast<wchar_t>(towlower(c));
            is_batch=low.ends_with(L".cmd")||low.ends_with(L".bat");
        }else{
            input_pipe.reset();output_pipe.reset();
            throw Failure("Could not find MCP server command.");
        }

        std::wstring cmdline=quote_arg(resolved_exe);
        J *args=get(config.get(),"args");
        for(size_t i=0;i<alen(args);i++)cmdline+=L" "+quote_arg(wide(json_object_get_string(at(args,i))));
        std::wstring app_exe=resolved_exe;
        if(is_batch){
            const char *comspec=env_value("ComSpec");
            app_exe=comspec&&*comspec?wide(comspec):L"C:\\Windows\\System32\\cmd.exe";
            cmdline=quote_arg(app_exe)+L" /d /s /c \""+cmdline+L"\"";
        }

        std::map<std::wstring,std::wstring> env_map;
        for(const char *key:{"PATH","PATHEXT","SystemRoot","SystemDrive","ComSpec","USERPROFILE","APPDATA","LOCALAPPDATA","TEMP","TMP","HOMEDRIVE","HOMEPATH"})
            if(const char *val=env_value(key))env_map[wide(key)]=wide(val);
        J *env=get(config.get(),"env");
        if(env){json_object_object_foreach(env,key,val){env_map[wide(key)]=wide(json_object_get_string(val));}}
        std::vector<wchar_t> env_block;
        for(const auto &[k,v]:env_map){
            std::wstring entry=k+L"="+v;
            env_block.insert(env_block.end(),entry.begin(),entry.end());
            env_block.push_back(0);
        }
        env_block.push_back(0);
        if(env_block.size()==1)env_block.push_back(0);

        std::string cwd=sget(config.get(),"cwd");
        if(cwd.empty())cwd=app.root;
        std::wstring cwd_w=wide(cwd);

        job=kill_job();
        if(!job){input_pipe.reset();output_pipe.reset();throw Failure("Could not create MCP job object.");}
        std::vector<HANDLE> inherit{child_in.get(),child_out.get(),sink.get()};
        SIZE_T attr_bytes=0;
        InitializeProcThreadAttributeList(nullptr,1,0,&attr_bytes);
        std::vector<unsigned char> storage(attr_bytes);
        auto *attr_list=reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
        const bool attr_init=InitializeProcThreadAttributeList(attr_list,1,0,&attr_bytes)!=FALSE;
        if(!attr_init||!UpdateProcThreadAttribute(attr_list,0,PROC_THREAD_ATTRIBUTE_HANDLE_LIST,inherit.data(),inherit.size()*sizeof(HANDLE),nullptr,nullptr)){
            if(attr_init)DeleteProcThreadAttributeList(attr_list);
            input_pipe.reset();output_pipe.reset();
            throw Failure("Could not configure MCP stdio handles.");
        }
        STARTUPINFOEXW si{};
        si.StartupInfo.cb=sizeof si;
        si.StartupInfo.dwFlags=STARTF_USESTDHANDLES;
        si.StartupInfo.hStdInput=child_in.get();
        si.StartupInfo.hStdOutput=child_out.get();
        si.StartupInfo.hStdError=sink.get();
        si.lpAttributeList=attr_list;
        PROCESS_INFORMATION pi{};
        BOOL created=CreateProcessW(app_exe.c_str(),cmdline.data(),nullptr,nullptr,TRUE,
            EXTENDED_STARTUPINFO_PRESENT|CREATE_UNICODE_ENVIRONMENT|CREATE_NO_WINDOW|CREATE_SUSPENDED,
            env_block.data(),cwd_w.empty()?nullptr:cwd_w.c_str(),&si.StartupInfo,&pi);
        DeleteProcThreadAttributeList(attr_list);
        if(!created){input_pipe.reset();output_pipe.reset();throw Failure("Could not launch MCP server.");}
        WinHandle proc(pi.hProcess),thr(pi.hThread);
        if(!AssignProcessToJobObject(job.get(),proc.get())||ResumeThread(thr.get())==static_cast<DWORD>(-1)){
            TerminateProcess(proc.get(),1);WaitForSingleObject(proc.get(),1000);
            input_pipe.reset();output_pipe.reset();
            throw Failure("Could not start MCP server process.");
        }
        child_proc=std::move(proc);
    }
    void write_message(J *message,double until){
        check(until);
        std::string bytes=encoded(message)+"\n";
        if(!win_write(input_pipe.get(),bytes))throw Failure("MCP server closed its input.");
    }
    JsonPtr read_message(double until){
        for(;;){
            check(until);
            auto end=pending.find('\n');
            if(end!=std::string::npos){
                auto line=pending.substr(0,end);
                if(!line.empty()&&line.back()=='\r')line.pop_back();
                pending.erase(0,end+1);
                return parsed(line);
            }
            DWORD available=0;
            if(!PeekNamedPipe(output_pipe.get(),nullptr,0,nullptr,&available,nullptr))
                throw Failure("MCP server closed its output. Check the command and its dependencies.");
            if(!available){
                if(WaitForSingleObject(child_proc.get(),20)!=WAIT_TIMEOUT){
                    DWORD rem=0;
                    if(!PeekNamedPipe(output_pipe.get(),nullptr,0,nullptr,&rem,nullptr)||!rem)
                        throw Failure("MCP server closed its output. Check the command and its dependencies.");
                }
                continue;
            }
            char bytes[8192];
            DWORD count=0;
            if(!ReadFile(output_pipe.get(),bytes,(std::min)(available,static_cast<DWORD>(sizeof bytes)),&count,nullptr)||!count)
                throw Failure("MCP server closed its output. Check the command and its dependencies.");
            pending.append(bytes,count);
            if(pending.size()>RESPONSE_LIMIT)throw Failure("MCP response exceeds the size limit.");
        }
    }
#else
    void start(){
        auto cloexec_pipe=[](int fds[2])->int{
#ifdef __linux__
            return pipe2(fds,O_CLOEXEC);
#else
            if(pipe(fds)!=0)return -1;
            fcntl(fds[0],F_SETFD,FD_CLOEXEC);
            fcntl(fds[1],F_SETFD,FD_CLOEXEC);
            return 0;
#endif
        };
        int to_child[2],from_child[2];
        if(cloexec_pipe(to_child))throw Failure("Could not create MCP input pipe.");
        if(cloexec_pipe(from_child)){close(to_child[0]);close(to_child[1]);throw Failure("Could not create MCP output pipe.");}
        std::vector<std::string> arguments{sget(config.get(),"command")};
        J *args=get(config.get(),"args");for(size_t i=0;i<alen(args);i++)arguments.emplace_back(json_object_get_string(at(args,i)));
        std::vector<char *> argv;for(auto &arg:arguments)argv.push_back(arg.data());argv.push_back(nullptr);
        std::map<std::string,std::string> environment;
        // Keep display authentication paired with DISPLAY for headed browsers on Xwayland.
        // Other environment variables must be explicitly configured.
        for(const char *key:{"PATH","HOME","LANG","LC_ALL","TMPDIR","XDG_RUNTIME_DIR","DISPLAY","XAUTHORITY","WAYLAND_DISPLAY"})if(const char *value=env_value(key))environment[key]=value;
        J *env=get(config.get(),"env");if(env){json_object_object_foreach(env,key,value){environment[key]=json_object_get_string(value);}}
        std::vector<std::string> env_entries;
        env_entries.reserve(environment.size());
        for(const auto &[key,value]:environment)env_entries.push_back(key+"="+value);
        std::vector<char *> envp;
        envp.reserve(env_entries.size()+1);
        for(auto &entry:env_entries)envp.push_back(entry.data());
        envp.push_back(nullptr);
        std::string cwd=sget(config.get(),"cwd");if(cwd.empty())cwd=app.root;
#ifdef __linux__
        pid_t parent=getpid();
#endif
        child=fork();
        if(child==0){
            signal(SIGINT,SIG_DFL);signal(SIGTERM,SIG_DFL);signal(SIGPIPE,SIG_DFL);
            if(setpgid(0,0)
#ifdef __linux__
               ||prctl(PR_SET_PDEATHSIG,SIGTERM)||getppid()!=parent
#endif
               ||chdir(cwd.c_str()))_exit(126);
            int sink=open("/dev/null",O_WRONLY);
            if(sink<0||dup2(to_child[0],0)<0||dup2(from_child[1],1)<0||dup2(sink,2)<0)_exit(126);
            close(to_child[0]);close(to_child[1]);close(from_child[0]);close(from_child[1]);if(sink>2)close(sink);
            environ=envp.data();
            execvp(argv[0],argv.data());_exit(127);
        }
        close(to_child[0]);close(from_child[1]);
        if(child<0){close(to_child[1]);close(from_child[0]);throw Failure("Could not launch MCP server.");}
        setpgid(child,child);input=to_child[1];output=from_child[0];
        fcntl(input,F_SETFL,O_NONBLOCK);fcntl(output,F_SETFL,O_NONBLOCK);
    }
    void write_message(J *message,double until){
        std::string bytes=encoded(message)+"\n";size_t used=0;
        while(used<bytes.size()){
            check(until);pollfd ready{input,POLLOUT,0};if(poll(&ready,1,50)<=0)continue;
            ssize_t count=write(input,bytes.data()+used,bytes.size()-used);
            if(count<0&&(errno==EAGAIN||errno==EINTR))continue;
            if(count<=0)throw Failure("MCP server closed its input.");
            used+=static_cast<size_t>(count);
        }
    }
    JsonPtr read_message(double until){
        for(;;){
            check(until);auto end=pending.find('\n');
            if(end!=std::string::npos){auto line=pending.substr(0,end);pending.erase(0,end+1);return parsed(line);}
            pollfd ready{output,POLLIN,0};if(poll(&ready,1,50)<=0)continue;
            char bytes[8192];ssize_t count=read(output,bytes,sizeof bytes);
            if(count<0&&(errno==EAGAIN||errno==EINTR))continue;
            if(count<=0)throw Failure("MCP server closed its output. Check the command and its dependencies.");
            pending.append(bytes,static_cast<size_t>(count));if(pending.size()>RESPONSE_LIMIT)throw Failure("MCP response exceeds the size limit.");
        }
    }
#endif
    JsonPtr server_reply(J *message){
        JsonPtr reply(obj());put(reply.get(),"jsonrpc",str("2.0"));put(reply.get(),"id",copy(get(message,"id")));
        if(!strcmp(sget(message,"method"),"ping"))put(reply.get(),"result",obj());
        else{J *error=obj();put(error,"code",json_object_new_int(-32601));put(error,"message",str("Client capability not supported"));put(reply.get(),"error",error);}
        return reply;
    }
    struct Transfer {
        Client &client;long id;bool notification;
        std::string bytes,event,session,content_type,error;
        JsonPtr response;
        size_t received=0;
        bool sse=false;
        void accept(const std::string &data){
            auto value=parsed(data);
            if(strcmp(sget(value.get(),"jsonrpc"),"2.0"))throw Failure("Invalid MCP JSON-RPC version.");
            if(*sget(value.get(),"method")){
                if(get(value.get(),"id")){auto reply=client.server_reply(value.get());client.http(reply.get(),0,true);}
            }else if(get(value.get(),"id")&&json_object_is_type(get(value.get(),"id"),json_type_int)&&json_object_get_int64(get(value.get(),"id"))==id)response=std::move(value);
        }
    };
    static size_t http_header(char *data,size_t size,size_t count,void *pointer){
        auto &transfer=*static_cast<Transfer *>(pointer);
        try{
            size_t length=size*count;std::string line(data,length);auto colon=line.find(':');
            if(colon!=std::string::npos){
                auto key=line.substr(0,colon);std::transform(key.begin(),key.end(),key.begin(),[](unsigned char c){return std::tolower(c);});
                auto value=line.substr(colon+1);while(!value.empty()&&isspace(static_cast<unsigned char>(value.back())))value.pop_back();size_t start=value.find_first_not_of(" \t");value=start==std::string::npos?"":value.substr(start);
                if(key=="mcp-session-id"){
                    if(value.empty()||value.size()>256||std::any_of(value.begin(),value.end(),[](unsigned char c){return c<33||c>126;}))throw Failure("Invalid MCP session ID.");
                    transfer.session=value;
                }
                if(key=="content-type"){std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return std::tolower(c);});transfer.content_type=value;transfer.sse=value.starts_with("text/event-stream");}
            }
            return length;
        }catch(...){transfer.error="Invalid MCP response headers.";return 0;}
    }
    static size_t http_body(char *data,size_t size,size_t count,void *pointer){
        auto &transfer=*static_cast<Transfer *>(pointer);
        try{
            if(size&&count>SIZE_MAX/size)throw Failure("MCP response exceeds the size limit.");
            size_t length=size*count;
            if(length>RESPONSE_LIMIT-transfer.received)throw Failure("MCP response exceeds the size limit.");
            transfer.received+=length;transfer.bytes.append(data,length);
            if(transfer.sse){
                std::string_view rem(transfer.bytes);
                for(;;){
                    auto end=rem.find('\n');if(end==std::string_view::npos)break;
                    std::string_view line=rem.substr(0,end);
                    rem.remove_prefix(end+1);
                    if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
                    if(line.empty()){
                        if(!transfer.event.empty()){transfer.accept(transfer.event);transfer.event.clear();}
                        if(transfer.response){
                            const size_t done_bytes=transfer.bytes.size()-rem.size();
                            if(done_bytes)transfer.bytes.erase(0,done_bytes);
                            return 0;
                        }
                    }else if(line.starts_with("data:")){
                        std::string_view value=line.substr(5);
                        if(!value.empty()&&value.front()==' ')value.remove_prefix(1);
                        if(!transfer.event.empty())transfer.event+='\n';
                        transfer.event.append(value);
                    }
                }
                const size_t consumed=transfer.bytes.size()-rem.size();
                if(consumed)transfer.bytes.erase(0,consumed);
            }
            return length;
        }catch(const std::exception &error){transfer.error=error.what();return 0;}catch(...){transfer.error="Could not read MCP response.";return 0;}
    }
    static int http_progress(void *pointer,curl_off_t,curl_off_t,curl_off_t,curl_off_t){
        auto &app=*static_cast<App *>(pointer);check_exit(app);progress_tick(&app);return app_cancelled(app)?1:0;
    }
    JsonPtr http(J *message,long id,bool notification){
        // Borrow the client's persistent handle for connection reuse; if an SSE
        // callback re-enters http() to answer a server ping, fall back to a
        // fresh handle so the active stream is never reset mid-transfer.
        CurlHandle handle=std::move(http_handle);
        if(!handle)handle.reset(curl_easy_init());
        if(!handle)throw Failure("Could not initialize MCP HTTP client.");
        struct ReturnHandle {
            CurlHandle &slot,&borrowed;
            ~ReturnHandle(){if(!slot)slot=std::move(borrowed);}
        } restore_handle{http_handle,handle};
        std::string body=encoded(message);Transfer transfer{*this,id,notification,{},{},{},{},{},JsonPtr{},0,false};
        HttpHeaders headers;
        headers.append("Content-Type: application/json");headers.append("Accept: application/json, text/event-stream");
        if(!session.empty())headers.append("Mcp-Session-Id: "+session);
        if(!version.empty())headers.append("MCP-Protocol-Version: "+version);
        J *configured=get(config.get(),"headers");if(configured){json_object_object_foreach(configured,key,value){headers.append(std::string(key)+": "+json_object_get_string(value));}}
        std::string bearer=resolve_oauth_bearer(app,name,config.get());
        if(!bearer.empty())headers.append("Authorization: Bearer "+bearer);
        CURL *curl=handle.get();CurlRequest request(curl);
        curl_easy_setopt(curl,CURLOPT_URL,sget(config.get(),"url"));curl_easy_setopt(curl,CURLOPT_HTTPHEADER,headers.get());
        curl_easy_setopt(curl,CURLOPT_POSTFIELDS,body.c_str());curl_easy_setopt(curl,CURLOPT_POSTFIELDSIZE,static_cast<long>(body.size()));
        curl_easy_setopt(curl,CURLOPT_CONNECTTIMEOUT,10L);curl_easy_setopt(curl,CURLOPT_TIMEOUT_MS,static_cast<long>(iget(config.get(),"timeoutMs",60000)));
        curl_easy_setopt(curl,CURLOPT_NOSIGNAL,1L);curl_easy_setopt(curl,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(curl,CURLOPT_HEADERFUNCTION,http_header);curl_easy_setopt(curl,CURLOPT_HEADERDATA,&transfer);
        curl_easy_setopt(curl,CURLOPT_WRITEFUNCTION,http_body);curl_easy_setopt(curl,CURLOPT_WRITEDATA,&transfer);
        curl_easy_setopt(curl,CURLOPT_XFERINFOFUNCTION,http_progress);curl_easy_setopt(curl,CURLOPT_XFERINFODATA,&app);curl_easy_setopt(curl,CURLOPT_NOPROGRESS,0L);
        CURLcode code=curl_easy_perform(curl);long status=0;curl_easy_getinfo(curl,CURLINFO_RESPONSE_CODE,&status);
        if(app_cancelled(app))throw Failure("MCP request cancelled. A remote operation may still be running.");
        if(!transfer.error.empty())throw Failure(transfer.error);
        if(code!=CURLE_OK&&!(code==CURLE_WRITE_ERROR&&transfer.response))throw Failure("MCP connection failed or timed out. The request was not automatically repeated.");
        if(status<200||status>=300){if(status==404)session.clear();throw Failure("MCP HTTP "+std::to_string(status)+". Check the address, authentication and server availability.");}
        if(!transfer.session.empty())session=transfer.session;
        if(notification)return JsonPtr(obj());
        if(!transfer.sse){if(!transfer.content_type.starts_with("application/json"))throw Failure("MCP server did not return JSON or an SSE stream.");transfer.accept(transfer.bytes);}
        if(!transfer.response)throw Failure("MCP response did not match the request ID.");
        return std::move(transfer.response);
    }
    JsonPtr request(const char *method,J *params=nullptr,bool notification=false){
        JsonPtr message(obj());put(message.get(),"jsonrpc",str("2.0"));put(message.get(),"method",str(method));
        long id=++next_id;if(!notification)put(message.get(),"id",json_object_new_int64(id));if(params)put(message.get(),"params",copy(params));
        JsonPtr response;
        if(*sget(config.get(),"url"))response=http(message.get(),id,notification);
        else{
            double until=monotime()+iget(config.get(),"timeoutMs",60000)/1000.0;
            write_message(message.get(),until);if(notification)return JsonPtr(obj());
            for(int count=0;count<256;count++){
                response=read_message(until);
                if(strcmp(sget(response.get(),"jsonrpc"),"2.0"))throw Failure("Invalid MCP JSON-RPC version.");
                if(*sget(response.get(),"method")){
                    if(get(response.get(),"id")){auto reply=server_reply(response.get());write_message(reply.get(),until);}
                    continue;
                }
                if(json_object_is_type(get(response.get(),"id"),json_type_int)&&json_object_get_int64(get(response.get(),"id"))==id)break;
                response.reset();
            }
        }
        if(notification)return JsonPtr(obj());
        if(!response||!json_object_is_type(get(response.get(),"id"),json_type_int)||json_object_get_int64(get(response.get(),"id"))!=id)throw Failure("MCP response did not match the request ID.");
        if(get(response.get(),"error"))throw Failure("MCP server rejected "+std::string(method)+" (JSON-RPC "+std::to_string(iget(get(response.get(),"error"),"code",0))+").");
        J *result=get(response.get(),"result");if(!json_object_is_type(result,json_type_object))throw Failure("MCP result is not an object.");
        return JsonPtr(copy(result));
    }
    void initialize(bool explicit_test){
        if(!*sget(config.get(),"url")){
            // Launching a server the user already configured is part of starting
            // the tool, not a new operation to confirm on every request. Tool
            // calls themselves still go through approve() in mcp_call().
            start();
        }
        // Only the manual "test connection" dialog needs a progress line; a
        // normal message reconnects each time and the notice is just noise.
        if(explicit_test)progress(&app,tr("正在连接 MCP…"));
        JsonPtr params(obj());put(params.get(),"protocolVersion",str(protocol));put(params.get(),"capabilities",obj());
        J *info=obj();put(info,"name",str("DeskCode"));put(info,"version",str(VERSION));put(params.get(),"clientInfo",info);
        auto result=request("initialize",params.get());version=sget(result.get(),"protocolVersion");
        if(version!="2025-06-18"&&version!="2025-03-26"&&version!="2024-11-05")throw Failure("MCP server selected an unsupported protocol version.");
        J *caps=get(result.get(),"capabilities");
        has_tools=get(caps,"tools")!=nullptr;
        has_resources=get(caps,"resources")!=nullptr;
        has_prompts=get(caps,"prompts")!=nullptr;
        if(!has_tools&&!has_resources&&!has_prompts)throw Failure("MCP server does not advertise tools, resources or prompts.");
        request("notifications/initialized",nullptr,true);
    }
};
struct Tool {Client *client;std::string original;JsonPtr definition;};
struct ResourceItem {Client *client;std::string uri,name,description,mime_type;};
struct ResourceTemplateItem {Client *client;std::string uri_template,name,description;};
struct PromptItem {Client *client;std::string name,description;JsonPtr arguments;};
struct Registry {
    std::string configuration;
    std::vector<std::unique_ptr<Client>> clients;
    std::map<std::string,Tool> tools;
    std::vector<ResourceItem> resources;
    std::vector<ResourceTemplateItem> templates;
    std::vector<PromptItem> prompts;
};
}

J *mcp_load(App &app){
    const auto path=config_path(app);errno=0;std::unique_ptr<char,decltype(&free)> bytes(read_bytes(path.c_str(),config_limit,nullptr),free);
    if(!bytes){if(errno==ENOENT)return obj();seterr(&app,"Could not read the local MCP configuration.");return nullptr;}
    try{auto document=parsed(bytes.get());J *servers=get(document.get(),"mcpServers");validate(servers);return copy(servers);}
    catch(const std::exception &error){seterr(&app,error.what());return nullptr;}
}
bool mcp_save(App &app,J *servers){
    try{
        validate(servers);JsonPtr document(obj());put(document.get(),"mcpServers",copy(servers));auto bytes=encoded(document.get());
        if(bytes.size()>config_limit)throw Failure("MCP configuration is too large.");
        if(mkdirs(app.config_dir,0700)||atomic_file(config_path(app).c_str(),bytes.data(),bytes.size(),0600))throw Failure("Could not save the local MCP configuration.");
        mcp_close(app);return true;
    }catch(const std::exception &error){seterr(&app,error.what());return false;}
}
bool mcp_import_json(App &app,const std::string &text){
    if(text.size()>config_limit){seterr(&app,tr("MCP 配置不能超过 256 KB。"));return false;}
    try{
        auto document=parsed(text);
        J *servers=get(document.get(),"mcpServers");
        if(!servers)throw Failure(tr("缺少 mcpServers 对象，请参考 MCP 配置示例。"));
        validate(servers);
        if(!json_object_object_length(servers))throw Failure(tr("mcpServers 为空，没有可导入的服务。"));
        JsonPtr existing(mcp_load(app));
        if(!existing)return false;
        json_object_object_foreach(servers,name,entry){
            if(get(existing.get(),name))throw Failure(tr("服务名称重复，请修改 JSON 中的服务名称后重试。"));
            put(existing.get(),name,copy(entry));
        }
        return mcp_save(app,existing.get());
    }catch(const std::exception &error){seterr(&app,error.what());return false;}
}
bool mcp_import(App &app,const char *path){
    size_t length=0;
    std::unique_ptr<char,decltype(&free)> bytes(read_bytes(path,config_limit,&length),free);
    if(!bytes){seterr(&app,tr("无法读取 MCP JSON 文件，请检查文件路径和权限（最大 256 KB）。"));return false;}
    return mcp_import_json(app,std::string(bytes.get(),length));
}
void mcp_close(App &app){delete static_cast<Registry *>(app.mcp_data);app.mcp_data=nullptr;}
// Saved servers, then those of enabled plugins and of the front end (ACP).
// Each extra source is validated on its own; names never replace saved ones.
static J *effective_servers(App &app,bool only_saved){
    JsonPtr servers(mcp_load(app));
    if(!servers||only_saved)return servers.release();
    auto merge=[&](J *extra){
        if(!json_object_is_type(extra,json_type_object)||!json_object_object_length(extra))return;
        try{validate(extra);}
        catch(const std::exception &error){fprintf(stderr,"%s\n",error.what());return;}
        json_object_object_foreach(extra,name,entry)if(!get(servers.get(),name))put(servers.get(),name,copy(entry));
    };
    JsonPtr plugins(plugin_mcp_servers(app));
    merge(plugins.get());
    merge(app.extra_mcp);
    return servers.release();
}
bool mcp_prepare(App &app,bool explicit_test,const char *only){
    JsonPtr servers(effective_servers(app,false));if(!servers)return false;
    JsonPtr oauth_state=load_oauth_tokens(app);
    std::string configuration=encoded(servers.get())+"\n"+encoded(oauth_state.get());
    auto *previous=static_cast<Registry *>(app.mcp_data);
    if(!explicit_test&&previous&&previous->configuration==configuration
       &&std::all_of(previous->clients.begin(),previous->clients.end(),[](const auto &c){return c->healthy();}))
        return true;
    mcp_close(app);auto registry=std::make_unique<Registry>();registry->configuration=configuration;
    try{
        json_object_object_foreach(servers.get(),name,entry){
            if(only?strcmp(only,name)!=0:bget(entry,"disabled"))continue;
            auto client=std::make_unique<Client>(app,name,entry);client->initialize(explicit_test);
            if(client->has_tools){
                std::string cursor;std::vector<std::string> cursors;
                do{
                    JsonPtr params(obj());if(!cursor.empty())put(params.get(),"cursor",str(cursor.c_str()));
                    auto page=client->request("tools/list",params.get());J *list=get(page.get(),"tools");
                    if(!json_object_is_type(list,json_type_array))throw Failure("MCP tools/list did not return an array.");
                    for(size_t i=0;i<alen(list);i++){
                        J *tool=at(list,i),*schema=get(tool,"inputSchema");
                        if(!string_value(get(tool,"name"))||!visible(sget(tool,"name"))||!json_object_is_type(schema,json_type_object)||strcmp(sget(schema,"type"),"object"))throw Failure("Invalid MCP tool name or input schema.");
                        std::string original=sget(tool,"name"),identity=std::string(name)+"\n"+original;
                        std::unique_ptr<char,decltype(&free)> hash(sha256(identity.data(),identity.size()),free);if(!hash)throw Failure("Could not name MCP tool.");
                        std::string alias="mcp_"+std::string(hash.get(),48);
                        if(registry->tools.count(alias)||registry->tools.size()>=128)throw Failure("Duplicate MCP tool or more than 128 imported tools.");
                        JsonPtr definition(obj());put(definition.get(),"type",str("function"));J *function=obj();put(function,"name",str(alias.c_str()));
                        std::string description="MCP "+std::string(name)+" / "+original+". "+sget(tool,"description");
                        put(function,"description",str(description.c_str()));put(function,"parameters",copy(schema));put(definition.get(),"function",function);
                        if(encoded(definition.get()).size()>64000)throw Failure("MCP tool schema is too large.");
                        registry->tools.emplace(alias,Tool{client.get(),original,std::move(definition)});
                        client->tool_count++;
                    }
                    cursor=sget(page.get(),"nextCursor");
                    if(!cursor.empty()){if(cursors.size()>=32||std::find(cursors.begin(),cursors.end(),cursor)!=cursors.end())throw Failure("MCP tools/list pagination did not finish.");cursors.push_back(cursor);}
                }while(!cursor.empty());
            }
            if(client->has_resources){
                std::string cursor;std::vector<std::string> cursors;
                do{
                    JsonPtr params(obj());if(!cursor.empty())put(params.get(),"cursor",str(cursor.c_str()));
                    auto page=client->request("resources/list",params.get());J *list=get(page.get(),"resources");
                    if(!json_object_is_type(list,json_type_array))throw Failure("MCP resources/list did not return an array.");
                    for(size_t i=0;i<alen(list)&&registry->resources.size()<256;i++){
                        J *res=at(list,i);
                        if(!string_value(get(res,"uri"))||!visible(sget(res,"uri")))continue;
                        registry->resources.push_back(ResourceItem{client.get(),sget(res,"uri"),sget(res,"name"),sget(res,"description"),sget(res,"mimeType")});
                        client->resource_count++;
                    }
                    cursor=sget(page.get(),"nextCursor");
                    if(!cursor.empty()){if(cursors.size()>=32||std::find(cursors.begin(),cursors.end(),cursor)!=cursors.end())throw Failure("MCP resources/list pagination did not finish.");cursors.push_back(cursor);}
                }while(!cursor.empty());
                try{
                    JsonPtr tp(obj());
                    auto tpage=client->request("resources/templates/list",tp.get());
                    J *tlist=get(tpage.get(),"resourceTemplates");
                    for(size_t i=0;i<alen(tlist)&&registry->templates.size()<128;i++){
                        J *tm=at(tlist,i);
                        if(!string_value(get(tm,"uriTemplate")))continue;
                        registry->templates.push_back(ResourceTemplateItem{client.get(),sget(tm,"uriTemplate"),sget(tm,"name"),sget(tm,"description")});
                    }
                }catch(...){}
            }
            if(client->has_prompts){
                std::string cursor;std::vector<std::string> cursors;
                do{
                    JsonPtr params(obj());if(!cursor.empty())put(params.get(),"cursor",str(cursor.c_str()));
                    auto page=client->request("prompts/list",params.get());J *list=get(page.get(),"prompts");
                    if(!json_object_is_type(list,json_type_array))throw Failure("MCP prompts/list did not return an array.");
                    for(size_t i=0;i<alen(list)&&registry->prompts.size()<128;i++){
                        J *pr=at(list,i);
                        if(!string_value(get(pr,"name"))||!visible(sget(pr,"name")))continue;
                        J *pargs=get(pr,"arguments");
                        registry->prompts.push_back(PromptItem{client.get(),sget(pr,"name"),sget(pr,"description"),JsonPtr(json_object_is_type(pargs,json_type_array)?copy(pargs):arr())});
                        client->prompt_count++;
                    }
                    cursor=sget(page.get(),"nextCursor");
                    if(!cursor.empty()){if(cursors.size()>=32||std::find(cursors.begin(),cursors.end(),cursor)!=cursors.end())throw Failure("MCP prompts/list pagination did not finish.");cursors.push_back(cursor);}
                }while(!cursor.empty());
            }
            registry->clients.push_back(std::move(client));
        }
        app.mcp_data=registry.release();progress_clear(&app);return true;
    }catch(const std::exception &error){progress_clear(&app);seterr(&app,error.what());return false;}
}
bool mcp_readonly_tool(const char *name){
    return !strcmp(name,"mcp_list_resources")||!strcmp(name,"mcp_read_resource")||!strcmp(name,"mcp_get_prompt");
}
void mcp_append_tools(App &app,J *tools,bool readonly_only){
    auto *registry=static_cast<Registry *>(app.mcp_data);
    if(!registry)return;
    if(!readonly_only){
        for(const auto &[name,tool]:registry->tools){(void)name;add(tools,copy(tool.definition.get()));}
    }
    if(!registry->resources.empty()||!registry->templates.empty()){
        J *t1=json_tokener_parse(R"({"type":"function","function":{"name":"mcp_list_resources","description":"List read-only MCP resources and URI templates exposed by connected MCP servers.","parameters":{"type":"object","properties":{"server":{"type":"string","description":"Optional MCP server name filter."}},"required":[],"additionalProperties":false}}})");
        if(t1)add(tools,t1);
        std::string rdesc="Read a specific resource URI from a connected MCP server. Available:";
        for(size_t i=0;i<std::min<size_t>(registry->resources.size(),12);i++){
            rdesc+=" "+registry->resources[i].client->name+":"+registry->resources[i].uri+";";
        }
        J *t2=json_tokener_parse(R"({"type":"function","function":{"name":"mcp_read_resource","description":"","parameters":{"type":"object","properties":{"server":{"type":"string","description":"MCP server name."},"uri":{"type":"string","description":"Resource URI from mcp_list_resources."}},"required":["server","uri"],"additionalProperties":false}}})");
        if(t2){put(get(t2,"function"),"description",str(rdesc.c_str()));add(tools,t2);}
    }
    if(!registry->prompts.empty()){
        std::string pdesc="Render a prompt template from a connected MCP server. Available:";
        for(size_t i=0;i<std::min<size_t>(registry->prompts.size(),16);i++){
            pdesc+=" "+registry->prompts[i].client->name+"/"+registry->prompts[i].name;
            if(!registry->prompts[i].description.empty())pdesc+=" ("+registry->prompts[i].description+")";
            pdesc+=";";
        }
        J *t3=json_tokener_parse(R"({"type":"function","function":{"name":"mcp_get_prompt","description":"","parameters":{"type":"object","properties":{"server":{"type":"string","description":"MCP server name."},"name":{"type":"string","description":"Prompt name."},"arguments":{"type":"object","description":"Optional string key-value arguments for the prompt."}},"required":["server","name"],"additionalProperties":false}}})");
        if(t3){put(get(t3,"function"),"description",str(pdesc.c_str()));add(tools,t3);}
    }
}
J *mcp_tools(App &app){
    JsonPtr tools(arr());mcp_append_tools(app,tools.get(),false);return tools.release();
}
namespace {
std::string format_prompt_messages(J *result){
    std::string out;
    if(*sget(result,"description")){out+=sget(result,"description");out+="\n\n";}
    J *msgs=get(result,"messages");
    for(size_t i=0;i<alen(msgs);i++){
        J *m=at(msgs,i),*content=get(m,"content");
        const char *role=*sget(m,"role")?sget(m,"role"):"user";
        std::string text;
        if(json_object_is_type(content,json_type_string))text=json_object_get_string(content);
        else if(json_object_is_type(content,json_type_object)){
            if(!strcmp(sget(content,"type"),"text"))text=sget(content,"text");
            else if(!strcmp(sget(content,"type"),"resource"))text=sget(get(content,"resource"),"text");
        }
        if(!text.empty()){
            if(!out.empty()&&out.back()!='\n')out+='\n';
            if(strcmp(role,"user")!=0){out+="[";out+=role;out+="]\n";}
            out+=text;
            out+='\n';
        }
    }
    while(!out.empty()&&(out.back()=='\n'||out.back()=='\r'))out.pop_back();
    return out;
}
} // namespace
J *mcp_call(App &app,const char *name,J *arguments){
    auto *registry=static_cast<Registry *>(app.mcp_data);
    if(!registry||!json_object_is_type(arguments,json_type_object))return err("Unknown MCP tool or invalid arguments.");
    if(!strcmp(name,"mcp_list_resources")){
        const std::string filter=sget(arguments,"server");
        J *out=obj(),*res_arr=arr(),*tpl_arr=arr();
        for(const auto &r:registry->resources){
            if(!filter.empty()&&r.client->name!=filter)continue;
            J *item=obj();
            put(item,"server",str(r.client->name.c_str()));
            put(item,"uri",str(r.uri.c_str()));
            if(!r.name.empty())put(item,"name",str(r.name.c_str()));
            if(!r.description.empty())put(item,"description",str(r.description.c_str()));
            if(!r.mime_type.empty())put(item,"mimeType",str(r.mime_type.c_str()));
            add(res_arr,item);
        }
        for(const auto &t:registry->templates){
            if(!filter.empty()&&t.client->name!=filter)continue;
            J *item=obj();
            put(item,"server",str(t.client->name.c_str()));
            put(item,"uriTemplate",str(t.uri_template.c_str()));
            if(!t.name.empty())put(item,"name",str(t.name.c_str()));
            if(!t.description.empty())put(item,"description",str(t.description.c_str()));
            add(tpl_arr,item);
        }
        put(out,"resources",res_arr);
        if(alen(tpl_arr))put(out,"resourceTemplates",tpl_arr);
        else json_object_put(tpl_arr);
        return out;
    }
    if(!strcmp(name,"mcp_read_resource")){
        const std::string srv=sget(arguments,"server"),uri=sget(arguments,"uri");
        if(srv.empty()||uri.empty())return err("mcp_read_resource requires server and uri.");
        Client *target=nullptr;
        for(const auto &c:registry->clients)if(c->name==srv&&c->has_resources){target=c.get();break;}
        if(!target)return err("Unknown MCP resource server.");
        const bool auto_ok=app.yes||app.auto_edit||app.plan_mode;
        std::string preview="MCP "+srv+" / resources/read "+uri;
        if(!auto_ok&&!approve(&app,preview.c_str(),Operation::Other,tr("读取 MCP 资源"))){
            J *r=err("MCP resource read was not approved.");put(r,"denied",json_object_new_boolean(1));return r;
        }
        try{
            JsonPtr params(obj());put(params.get(),"uri",str(uri.c_str()));
            auto result=target->request("resources/read",params.get());
            return result.release();
        }catch(const std::exception &error){J *result=err(error.what());mcp_close(app);return result;}
    }
    if(!strcmp(name,"mcp_get_prompt")){
        const std::string srv=sget(arguments,"server"),pname=sget(arguments,"name");
        if(srv.empty()||pname.empty())return err("mcp_get_prompt requires server and name.");
        Client *target=nullptr;
        for(const auto &c:registry->clients)if(c->name==srv&&c->has_prompts){target=c.get();break;}
        if(!target)return err("Unknown MCP prompt server.");
        const bool auto_ok=app.yes||app.auto_edit||app.plan_mode;
        std::string preview="MCP "+srv+" / prompts/get "+pname;
        if(!auto_ok&&!approve(&app,preview.c_str(),Operation::Other,tr("获取 MCP 提示模板"))){
            J *r=err("MCP prompt fetch was not approved.");put(r,"denied",json_object_new_boolean(1));return r;
        }
        try{
            JsonPtr params(obj());put(params.get(),"name",str(pname.c_str()));
            if(J *pargs=get(arguments,"arguments");json_object_is_type(pargs,json_type_object))put(params.get(),"arguments",copy(pargs));
            auto result=target->request("prompts/get",params.get());
            put(result.get(),"rendered_text",str(format_prompt_messages(result.get()).c_str()));
            return result.release();
        }catch(const std::exception &error){J *result=err(error.what());mcp_close(app);return result;}
    }
    auto found=registry->tools.find(name);
    if(found==registry->tools.end())return err("Unknown MCP tool or invalid arguments.");
    auto &tool=found->second;
    std::string preview="MCP "+tool.client->name+" / "+tool.original+"\n"+encoded(arguments);
    if(!approve(&app,preview.c_str(),Operation::Other,tr("调用 MCP 工具"))){J *result=err("MCP operation was not approved.");put(result,"denied",json_object_new_boolean(true));return result;}
    try{
        JsonPtr params(obj());put(params.get(),"name",str(tool.original.c_str()));put(params.get(),"arguments",copy(arguments));
        auto result=tool.client->request("tools/call",params.get());
        if(bget(result.get(),"isError"))put(result.get(),"error",str("MCP tool reported a failure."));
        return result.release();
    }catch(const std::exception &error){J *result=err(error.what());mcp_close(app);return result;}
}

std::vector<McpPromptInfo> mcp_list_prompts(App &app){
    std::vector<McpPromptInfo> out;
    auto *registry=static_cast<Registry *>(app.mcp_data);
    if(!registry)return out;
    for(const auto &p:registry->prompts)out.push_back({p.client->name,p.name,p.description});
    return out;
}

bool mcp_expand_prompt(App &app,std::string_view input,std::string &expanded,std::string &label){
    while(!input.empty()&&isspace(static_cast<unsigned char>(input.front())))input.remove_prefix(1);
    while(!input.empty()&&isspace(static_cast<unsigned char>(input.back())))input.remove_suffix(1);
    if(input.size()<4||input.front()!='/')return false;
    std::string_view body=input.substr(1);
    if(body.starts_with("mcp:"))body.remove_prefix(4);
    auto sp=body.find_first_of(" \t\r\n");
    std::string_view head=sp==std::string_view::npos?body:body.substr(0,sp);
    std::string_view rest=sp==std::string_view::npos?std::string_view{}:body.substr(sp+1);
    while(!rest.empty()&&isspace(static_cast<unsigned char>(rest.front())))rest.remove_prefix(1);
    auto colon=head.find(':');
    if(colon==std::string_view::npos||colon==0||colon+1>=head.size())return false;
    const std::string srv(head.substr(0,colon)),pname(head.substr(colon+1));
    if(!app.mcp_data){
        JsonPtr servers(effective_servers(app,false));
        if(!servers||!get(servers.get(),srv.c_str()))return false;
        if(!mcp_prepare(app,false,nullptr))return false;
    }
    auto *registry=static_cast<Registry *>(app.mcp_data);
    if(!registry)return false;
    const PromptItem *matched=nullptr;
    for(const auto &p:registry->prompts){
        if(p.client->name==srv&&p.name==pname){matched=&p;break;}
    }
    if(!matched)return false;
    JsonPtr arg_obj(obj());
    if(!rest.empty()){
        bool any_kv=false;
        for(size_t pos=0;pos<rest.size();){
            while(pos<rest.size()&&isspace(static_cast<unsigned char>(rest[pos])))pos++;
            if(pos>=rest.size())break;
            size_t end=pos;
            while(end<rest.size()&&!isspace(static_cast<unsigned char>(rest[end])))end++;
            std::string_view tok=rest.substr(pos,end-pos);
            auto eq=tok.find('=');
            if(eq!=std::string_view::npos&&eq>0){
                std::string k(tok.substr(0,eq)),v(tok.substr(eq+1));
                if(v.size()>=2&&((v.front()=='"'&&v.back()=='"')||(v.front()=='\''&&v.back()=='\'')))v=v.substr(1,v.size()-2);
                put(arg_obj.get(),k.c_str(),str(v.c_str()));
                any_kv=true;
            }
            pos=end;
        }
        if(!any_kv&&alen(matched->arguments.get())>=1){
            const char *first_arg=sget(at(matched->arguments.get(),0),"name");
            if(*first_arg)put(arg_obj.get(),first_arg,str(std::string(rest).c_str()));
        }
    }
    try{
        JsonPtr params(obj());
        put(params.get(),"name",str(pname.c_str()));
        if(json_object_object_length(arg_obj.get())>0)put(params.get(),"arguments",copy(arg_obj.get()));
        auto result=matched->client->request("prompts/get",params.get());
        expanded=format_prompt_messages(result.get());
        if(expanded.empty())return false;
        label="/"+srv+":"+pname+(rest.empty()?"":" "+std::string(rest));
        return true;
    }catch(...){return false;}
}

std::vector<McpServerSummary> mcp_summaries(App &app){
    std::vector<McpServerSummary> out;
    JsonPtr servers(effective_servers(app,false));
    JsonPtr tokens=load_oauth_tokens(app);
    auto *registry=static_cast<Registry *>(app.mcp_data);
    if(!servers)return out;
    json_object_object_foreach(servers.get(),name,entry){
        McpServerSummary s;
        s.name=name;
        s.remote=*sget(entry,"url")!=0;
        s.oauth=get(entry,"oauth")!=nullptr;
        s.has_token=*sget(get(tokens.get(),name),"access_token")!=0;
        if(registry){
            for(const auto &c:registry->clients){
                if(c->name==name){s.tools=c->tool_count;s.resources=c->resource_count;s.prompts=c->prompt_count;break;}
            }
        }
        out.push_back(std::move(s));
    }
    return out;
}

namespace {
std::string url_origin(const char *raw_url){
    CURLU *u=curl_url();
    if(!u)return {};
    if(curl_url_set(u,CURLUPART_URL,raw_url,0)!=CURLUE_OK){curl_url_cleanup(u);return {};}
    char *scheme=nullptr,*host=nullptr,*port=nullptr;
    curl_url_get(u,CURLUPART_SCHEME,&scheme,0);
    curl_url_get(u,CURLUPART_HOST,&host,0);
    curl_url_get(u,CURLUPART_PORT,&port,0);
    std::string origin;
    if(scheme&&host){
        origin=std::string(scheme)+"://"+host;
        if(port&&*port)origin+=std::string(":")+port;
    }
    curl_free(scheme);curl_free(host);curl_free(port);curl_url_cleanup(u);
    return origin;
}
void open_browser_best_effort(const std::string &url){
    const char *no_br=env_value("DESK_OAUTH_NO_BROWSER");
    if(no_br&&*no_br&&strcmp(no_br,"0"))return;
#ifdef _WIN32
    std::wstring wurl=wide(url);
    std::wstring cmd=L"cmd.exe /c start \"\" "+quote_arg(wurl);
    STARTUPINFOW si{};si.cb=sizeof si;PROCESS_INFORMATION pi{};
    if(CreateProcessW(nullptr,cmd.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,nullptr,&si,&pi)){
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
            execlp("open","open",url.c_str(),static_cast<char *>(nullptr));
#else
            execlp("xdg-open","xdg-open",url.c_str(),static_cast<char *>(nullptr));
#endif
            _exit(0);
        }
        _exit(0);
    }
    if(pid>0){int st=0;while(waitpid(pid,&st,0)<0&&errno==EINTR){}}
#endif
}
std::string query_param(std::string_view query,std::string_view key){
    while(!query.empty()){
        auto amp=query.find('&');
        std::string_view pair=amp==std::string_view::npos?query:query.substr(0,amp);
        auto eq=pair.find('=');
        if(eq!=std::string_view::npos&&pair.substr(0,eq)==key)return url_decode(pair.substr(eq+1));
        if(amp==std::string_view::npos)break;
        query.remove_prefix(amp+1);
    }
    return {};
}
} // namespace

int mcp_cli(App &app,int argc,char **argv,int first){
    const char *sub=first<argc?argv[first]:"list";
    JsonPtr servers(effective_servers(app,false));
    if(!servers){fprintf(stderr,"%s\n",app.error);return 1;}
    if(!strcmp(sub,"list")){
        JsonPtr tokens=load_oauth_tokens(app);
        if(!json_object_object_length(servers.get())){puts(tr("尚未配置 MCP 服务。"));return 0;}
        json_object_object_foreach(servers.get(),name,entry){
            const bool remote=*sget(entry,"url")!=0;
            const bool disabled=bget(entry,"disabled");
            const bool has_tok=*sget(get(tokens.get(),name),"access_token")!=0;
            printf("  %s %s (%s%s%s)\n",disabled?"·":"✓",name,remote?sget(entry,"url"):sget(entry,"command"),
                   disabled?", disabled":"",has_tok?", oauth-authenticated":(get(entry,"oauth")?", oauth-configured":""));
        }
        return 0;
    }
    if(!strcmp(sub,"token")){
        if(first+2>=argc){fprintf(stderr,"%s\n",tr("用法：desk mcp token <服务名> <访问令牌> [--refresh-token RT] [--expires-in 秒]"));return 2;}
        const char *srv=argv[first+1],*access=argv[first+2];
        if(!get(servers.get(),srv)){fprintf(stderr,"%s\n",tr("未找到指定的 MCP 服务。"));return 1;}
        std::string rt;int expires_in=0;
        for(int i=first+3;i<argc;i++){
            if(!strcmp(argv[i],"--refresh-token")&&i+1<argc)rt=argv[++i];
            else if(!strcmp(argv[i],"--expires-in")&&i+1<argc)expires_in=atoi(argv[++i]);
            else{fprintf(stderr,"%s\n",tr("未知参数，请运行 desk --help。"));return 2;}
        }
        JsonPtr tokens=load_oauth_tokens(app);
        J *item=obj();
        put(item,"access_token",str(access));
        if(!rt.empty())put(item,"refresh_token",str(rt.c_str()));
        if(expires_in>0)put(item,"expires_at",json_object_new_int64(static_cast<int64_t>(std::time(nullptr))+expires_in));
        put(tokens.get(),srv,item);
        if(!save_oauth_tokens(app,tokens.get())){fprintf(stderr,"%s\n",tr("无法保存 MCP 令牌。"));return 1;}
        printf(tr("已保存 MCP 服务 %s 的访问令牌。\n"),srv);
        return 0;
    }
    if(!strcmp(sub,"logout")){
        if(first+1>=argc){fprintf(stderr,"%s\n",tr("用法：desk mcp logout <服务名>"));return 2;}
        const char *srv=argv[first+1];
        JsonPtr tokens=load_oauth_tokens(app);
        json_object_object_del(tokens.get(),srv);
        if(!save_oauth_tokens(app,tokens.get())){fprintf(stderr,"%s\n",tr("无法保存 MCP 令牌。"));return 1;}
        printf(tr("已清除 MCP 服务 %s 的 OAuth 凭据。\n"),srv);
        return 0;
    }
    if(!strcmp(sub,"auth")){
        if(first+1>=argc){fprintf(stderr,"%s\n",tr("用法：desk mcp auth <服务名> [--port 回调端口]"));return 2;}
        const char *srv=argv[first+1];
        int req_port=0;
        for(int i=first+2;i<argc;i++){
            if(!strcmp(argv[i],"--port")&&i+1<argc)req_port=atoi(argv[++i]);
            else{fprintf(stderr,"%s\n",tr("未知参数，请运行 desk --help。"));return 2;}
        }
        J *entry=get(servers.get(),srv);
        if(!entry||!*sget(entry,"url")){fprintf(stderr,"%s\n",tr("仅远程 HTTP MCP 服务支持 OAuth 授权。"));return 1;}
        J *oauth=get(entry,"oauth");
        std::string auth_url=sget(oauth,"authorizationUrl"),token_url=sget(oauth,"tokenUrl"),reg_url;
        std::string client_id=sget(oauth,"clientId"),client_secret=sget(oauth,"clientSecret");
        std::string scopes=*sget(oauth,"scopes")?sget(oauth,"scopes"):sget(oauth,"scope");
        if(auth_url.empty()||token_url.empty()){
            std::string origin=url_origin(sget(entry,"url"));
            if(!origin.empty()){
                long st=0;
                std::string meta_raw=oauth_http_call("GET",origin+"/.well-known/oauth-authorization-server",{},"",st);
                if(st>=200&&st<300){
                    JsonPtr meta(json_tokener_parse(meta_raw.c_str()));
                    if(auth_url.empty())auth_url=sget(meta.get(),"authorization_endpoint");
                    if(token_url.empty())token_url=sget(meta.get(),"token_endpoint");
                    reg_url=sget(meta.get(),"registration_endpoint");
                }
            }
        }
        if(auth_url.empty()||token_url.empty()||!valid_http_endpoint(auth_url.c_str())||!valid_http_endpoint(token_url.c_str())){
            fprintf(stderr,"%s\n",tr("无法发现 OAuth 授权地址；请在 mcp.json 的 oauth 字段配置 authorizationUrl 与 tokenUrl。"));
            return 1;
        }
#ifdef _WIN32
        SOCKET lfd=socket(AF_INET,SOCK_STREAM,IPPROTO_TCP);
        if(lfd==INVALID_SOCKET){fprintf(stderr,"%s\n",tr("无法绑定本地 OAuth 回调端口。"));return 1;}
        SetHandleInformation(reinterpret_cast<HANDLE>(lfd),HANDLE_FLAG_INHERIT,0);
#else
        int lfd=socket(AF_INET,SOCK_STREAM,0);
        if(lfd<0){fprintf(stderr,"%s\n",tr("无法绑定本地 OAuth 回调端口。"));return 1;}
        fcntl(lfd,F_SETFD,FD_CLOEXEC);
        int yes_opt=1;setsockopt(lfd,SOL_SOCKET,SO_REUSEADDR,&yes_opt,sizeof yes_opt);
#endif
        sockaddr_in addr{};
        addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        addr.sin_port=htons(static_cast<uint16_t>(req_port>0&&req_port<65536?req_port:0));
#ifdef _WIN32
        auto close_sock=[](SOCKET s){closesocket(s);};
        int alen_sz=sizeof addr;
        if(bind(lfd,reinterpret_cast<sockaddr *>(&addr),sizeof addr)==SOCKET_ERROR||getsockname(lfd,reinterpret_cast<sockaddr *>(&addr),&alen_sz)==SOCKET_ERROR||listen(lfd,4)==SOCKET_ERROR){
            close_sock(lfd);fprintf(stderr,"%s\n",tr("无法绑定本地 OAuth 回调端口。"));return 1;
        }
#else
        auto close_sock=[](int s){close(s);};
        socklen_t alen_sz=sizeof addr;
        if(bind(lfd,reinterpret_cast<sockaddr *>(&addr),sizeof addr)!=0||getsockname(lfd,reinterpret_cast<sockaddr *>(&addr),&alen_sz)!=0||listen(lfd,4)!=0){
            close_sock(lfd);fprintf(stderr,"%s\n",tr("无法绑定本地 OAuth 回调端口。"));return 1;
        }
#endif
        const int bound_port=ntohs(addr.sin_port);
        const std::string redirect_uri="http://127.0.0.1:"+std::to_string(bound_port)+"/callback";
        if(client_id.empty()&&!reg_url.empty()&&valid_http_endpoint(reg_url.c_str())){
            JsonPtr reg_req(obj());
            put(reg_req.get(),"client_name",str("DeskCode"));
            J *ruris=arr();add(ruris,str(redirect_uri.c_str()));put(reg_req.get(),"redirect_uris",ruris);
            J *gts=arr();add(gts,str("authorization_code"));add(gts,str("refresh_token"));put(reg_req.get(),"grant_types",gts);
            J *rts=arr();add(rts,str("code"));put(reg_req.get(),"response_types",rts);
            put(reg_req.get(),"token_endpoint_auth_method",str("none"));
            long rst=0;
            std::string rbody=oauth_http_call("POST",reg_url,encoded(reg_req.get()),"application/json",rst);
            if(rst>=200&&rst<300){
                JsonPtr rdoc(json_tokener_parse(rbody.c_str()));
                client_id=sget(rdoc.get(),"client_id");
            }
        }
        if(client_id.empty())client_id="deskcode-cli";

        unsigned char verifier_bytes[32]{};
        if(RAND_bytes(verifier_bytes,sizeof verifier_bytes)!=1){close_sock(lfd);return 1;}
        const std::string code_verifier=base64url_encode(verifier_bytes,sizeof verifier_bytes);
        unsigned char digest[32]{};
        unsigned int digest_len=0;
        if(EVP_Digest(code_verifier.data(),code_verifier.size(),digest,&digest_len,EVP_sha256(),nullptr)!=1){close_sock(lfd);return 1;}
        const std::string code_challenge=base64url_encode(digest,digest_len);
        char state_hex[33]{};id_string(state_hex);
        const std::string expected_state(state_hex,16);

        std::string full_auth=auth_url+(auth_url.find('?')==std::string::npos?"?":"&")+
            "response_type=code&client_id="+url_encode(client_id)+
            "&redirect_uri="+url_encode(redirect_uri)+
            "&code_challenge="+url_encode(code_challenge)+
            "&code_challenge_method=S256&state="+url_encode(expected_state);
        if(!scopes.empty())full_auth+="&scope="+url_encode(scopes);

        printf("Authorization URL: %s\n",full_auth.c_str());
        fflush(stdout);
        open_browser_best_effort(full_auth);

        std::string auth_code;
        double deadline=monotime()+180.0;
        while(monotime()<deadline&&!interrupted&&auth_code.empty()){
#ifdef _WIN32
            fd_set rfds;FD_ZERO(&rfds);FD_SET(lfd,&rfds);
            timeval tv{0,100000};
            if(select(0,&rfds,nullptr,nullptr,&tv)<=0)continue;
            SOCKET cfd=accept(lfd,nullptr,nullptr);
            if(cfd==INVALID_SOCKET)continue;
            fd_set crfds;FD_ZERO(&crfds);FD_SET(cfd,&crfds);
            timeval ctv{2,0};
            char req_buf[4096]{};
            int n=select(0,&crfds,nullptr,nullptr,&ctv)>0?recv(cfd,req_buf,sizeof(req_buf)-1,0):0;
#else
            pollfd pfd{lfd,POLLIN,0};
            if(poll(&pfd,1,100)<=0)continue;
            int cfd=accept(lfd,nullptr,nullptr);
            if(cfd<0)continue;
            fcntl(cfd,F_SETFD,FD_CLOEXEC);
            pollfd cpfd{cfd,POLLIN,0};
            char req_buf[4096]{};
            ssize_t n=poll(&cpfd,1,2000)>0?read(cfd,req_buf,sizeof(req_buf)-1):0;
#endif
            if(n>0){
                std::string_view req_sv(req_buf,static_cast<size_t>(n));
                auto sp1=req_sv.find(' '),sp2=sp1==std::string_view::npos?std::string_view::npos:req_sv.find(' ',sp1+1);
                std::string_view target=sp2==std::string_view::npos?std::string_view{}:req_sv.substr(sp1+1,sp2-sp1-1);
                auto qpos=target.find('?');
                std::string_view qs=qpos==std::string_view::npos?std::string_view{}:target.substr(qpos+1);
                std::string got_state=query_param(qs,"state"),got_code=query_param(qs,"code");
                const bool ok_cb=(got_state==expected_state&&!got_code.empty());
                const char *html=ok_cb
                    ?"HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n<!doctype html><meta charset=utf-8><title>DeskCode OAuth</title><body style='font-family:system-ui;background:#0d1117;color:#e6edf3;padding:40px'><h2>DeskCode OAuth Completed</h2><p>You can close this window and return to DeskCode.</p></body>"
                    :"HTTP/1.1 400 Bad Request\r\nContent-Type: text/plain; charset=utf-8\r\nConnection: close\r\n\r\nInvalid OAuth callback state or missing code.";
#ifdef _WIN32
                send(cfd,html,static_cast<int>(strlen(html)),0);
#else
                const ssize_t wr=write(cfd,html,strlen(html));(void)wr;
#endif
                if(ok_cb)auth_code=std::move(got_code);
            }
            close_sock(cfd);
        }
        close_sock(lfd);
        if(auth_code.empty()){fprintf(stderr,"%s\n",tr("OAuth 授权未完成或已超时。"));return 1;}

        std::string form="grant_type=authorization_code&code="+url_encode(auth_code)+
            "&redirect_uri="+url_encode(redirect_uri)+
            "&client_id="+url_encode(client_id)+
            "&code_verifier="+url_encode(code_verifier);
        if(!client_secret.empty())form+="&client_secret="+url_encode(client_secret);
        long tst=0;
        std::string tbody=oauth_http_call("POST",token_url,form,"application/x-www-form-urlencoded",tst);
        JsonPtr tok(json_tokener_parse(tbody.c_str()));
        if(tst<200||tst>=300||!tok||!*sget(tok.get(),"access_token")){
            fprintf(stderr,"%s\n",tr("OAuth 令牌交换失败。"));
            return 1;
        }
        JsonPtr tokens=load_oauth_tokens(app);
        J *saved=obj();
        put(saved,"access_token",str(sget(tok.get(),"access_token")));
        if(*sget(tok.get(),"refresh_token"))put(saved,"refresh_token",str(sget(tok.get(),"refresh_token")));
        put(saved,"token_url",str(token_url.c_str()));
        put(saved,"client_id",str(client_id.c_str()));
        int exp_in=iget(tok.get(),"expires_in",0);
        if(exp_in>0)put(saved,"expires_at",json_object_new_int64(static_cast<int64_t>(std::time(nullptr))+exp_in));
        put(tokens.get(),srv,saved);
        if(!save_oauth_tokens(app,tokens.get())){fprintf(stderr,"%s\n",tr("无法保存 MCP 令牌。"));return 1;}
        printf(tr("已完成 MCP 服务 %s 的 OAuth 授权。\n"),srv);
        return 0;
    }
    fprintf(stderr,"%s\n",tr("用法：desk mcp <list|auth|token|logout> ..."));
    return 2;
}
