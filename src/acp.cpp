// Agent Client Protocol (https://agentclientprotocol.com) on stdin/stdout:
// newline-delimited JSON-RPC 2.0. Editors such as Zed and JetBrains IDEs start
// `desk acp` and drive conversations through it. stdout carries only protocol
// messages; anything else the agent prints goes to stderr.
#include "desk.hpp"
#include "tasks.hpp"
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <iostream>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#define acp_dup _dup
#define acp_dup2 _dup2
#define acp_fdopen _fdopen
#else
#include <unistd.h>
#define acp_dup dup
#define acp_dup2 dup2
#define acp_fdopen fdopen
#endif

namespace {
FILE *protocol_out=nullptr;
std::mutex output_mutex;

void send(J *message){
    std::lock_guard lock(output_mutex);
    fputs(json_object_to_json_string_ext(message,JSON_C_TO_STRING_PLAIN),protocol_out);
    fputc('\n',protocol_out);
    fflush(protocol_out);
}
void respond(J *id,J *result){
    JsonPtr message(obj());
    put(message.get(),"jsonrpc",str("2.0"));
    put(message.get(),"id",copy(id));
    put(message.get(),"result",result?result:obj());
    send(message.get());
}
void respond_error(J *id,int code,const char *text){
    JsonPtr message(obj());
    put(message.get(),"jsonrpc",str("2.0"));
    put(message.get(),"id",id?copy(id):nullptr);
    J *error=obj();
    put(error,"code",json_object_new_int(code));
    put(error,"message",str(text));
    put(message.get(),"error",error);
    send(message.get());
}
void session_update(const std::string &session,J *update){
    JsonPtr message(obj());
    put(message.get(),"jsonrpc",str("2.0"));
    put(message.get(),"method",str("session/update"));
    J *params=obj();
    put(params,"sessionId",str(session.c_str()));
    put(params,"update",update);
    put(message.get(),"params",params);
    send(message.get());
}
void text_chunk(const std::string &session,const std::string &text){
    if(text.empty())return;
    J *update=obj(),*content=obj();
    put(update,"sessionUpdate",str("agent_message_chunk"));
    put(content,"type",str("text"));
    put(content,"text",json_object_new_string_len(text.data(),static_cast<int>(text.size())));
    put(update,"content",content);
    session_update(session,update);
}

enum Mode {ASK=0,EDIT=1,FULL=2,PLAN=3};
const char *mode_id(int mode){return mode==EDIT?"edit":mode==FULL?"full":mode==PLAN?"plan":"ask";}
int mode_from(std::string_view id){return id=="edit"?EDIT:id=="full"?FULL:id=="plan"?PLAN:id=="ask"?ASK:-1;}
J *modes_state(int current){
    J *state=obj(),*available=arr();
    struct Item {const char *id,*name,*description;};
    const Item items[]={
        {"ask","Ask for approval","Confirm every edit, command and other change."},
        {"edit","Approve file edits","Workspace file edits and read-only commands run without asking; other commands still ask."},
        {"full","Full access","Every enabled tool runs without asking. File tools stay inside the workspace."},
        {"plan","Plan","Investigate with read-only tools and propose a plan; nothing changes until you switch modes."}};
    for(const auto &item:items){
        J *entry=obj();
        put(entry,"id",str(item.id));
        put(entry,"name",str(item.name));
        put(entry,"description",str(item.description));
        add(available,entry);
    }
    put(state,"currentModeId",str(mode_id(current)));
    put(state,"availableModes",available);
    return state;
}
void apply_mode(App &app,int mode){
    app.yes=mode==FULL;
    app.auto_edit=mode==EDIT;
    app.plan_mode=mode==PLAN;
}

struct Session {
    std::string id;
    std::unique_ptr<App> app;
    std::atomic<int> mode{ASK};
    std::atomic<bool> cancelled{false};
};
struct PromptJob {JsonPtr id;std::string session;JsonPtr prompt;};

std::mutex state_mutex;
std::condition_variable state_changed;
std::map<std::string,std::unique_ptr<Session>> sessions;
std::deque<PromptJob> queue;
std::string running_session;
std::map<std::string,JsonPtr> permission_replies;
std::atomic<bool> shutting_down{false};
long permission_counter=0;

std::string uri_to_path(const std::string &uri){
    if(!uri.starts_with("file://"))return {};
    std::string raw=uri.substr(7),path;
    for(size_t i=0;i<raw.size();i++){
        if(raw[i]=='%'&&i+2<raw.size()&&isxdigit(static_cast<unsigned char>(raw[i+1]))&&isxdigit(static_cast<unsigned char>(raw[i+2]))){
            path+=static_cast<char>(strtol(raw.substr(i+1,2).c_str(),nullptr,16));
            i+=2;
        }else path+=raw[i];
    }
#ifdef _WIN32
    if(path.size()>2&&path[0]=='/'&&path[2]==':')path.erase(0,1);
    for(char &c:path)if(c=='/')c='\\';
#endif
    return path;
}
// Prompt content blocks become one request: text as written, workspace files
// as @mentions (expanded the usual way), embedded resources as context.
std::string compose_prompt(const App &app,J *blocks){
    std::string text,context;
    const std::string root=app.root;
    for(size_t i=0;i<alen(blocks);i++){
        J *block=at(blocks,i);
        const std::string_view type=sget(block,"type");
        if(type=="text"){
            if(!text.empty()&&text.back()!='\n'&&text.back()!=' ')text+=' ';
            text+=sget(block,"text");
        }else if(type=="resource_link"){
            const std::string path=uri_to_path(sget(block,"uri"));
            if(path.size()>root.size()&&path.starts_with(root)&&(path[root.size()]=='/'||path[root.size()]=='\\')){
                std::string rel=path.substr(root.size()+1);
                for(char &c:rel)if(c=='\\')c='/';
                text+=rel.find(' ')!=std::string::npos?" @\""+rel+"\"":" @"+rel;
            }else text+=std::string(" ")+sget(block,"uri");
        }else if(type=="resource"){
            J *resource=get(block,"resource");
            if(*sget(resource,"text")){
                context+="\n\n<context uri=\"";
                context+=sget(resource,"uri");
                context+="\">\n";
                context+=sget(resource,"text");
                context+="\n</context>";
            }
        }
    }
    return text+context;
}
J *mcp_from_acp(J *servers){
    J *out=obj();
    for(size_t i=0;i<alen(servers)&&json_object_object_length(out)<16;i++){
        J *server=at(servers,i);
        const char *name=sget(server,"name");
        if(!*name||get(out,name))continue;
        const std::string_view type=sget(server,"type");
        J *entry=obj();
        auto pairs=[](J *list){
            J *map=obj();
            for(size_t k=0;k<alen(list);k++)if(*sget(at(list,k),"name"))put(map,sget(at(list,k),"name"),str(sget(at(list,k),"value")));
            return map;
        };
        if(type=="http"){
            put(entry,"url",str(sget(server,"url")));
            if(alen(get(server,"headers")))put(entry,"headers",pairs(get(server,"headers")));
        }else if(type.empty()||type=="stdio"){
            put(entry,"command",str(sget(server,"command")));
            J *args=arr();
            for(size_t k=0;k<alen(get(server,"args"));k++)add(args,copy(at(get(server,"args"),k)));
            put(entry,"args",args);
            if(alen(get(server,"env")))put(entry,"env",pairs(get(server,"env")));
        }else{json_object_put(entry);continue;}
        put(out,name,entry);
    }
    return out;
}
}

namespace {
std::string clip(std::string text,size_t limit){
    for(char &c:text)if(c=='\n'||c=='\r'||c=='\t')c=' ';
    if(text.size()>limit){
        size_t cut=limit;
        while(cut>0&&(static_cast<unsigned char>(text[cut])&0xc0)==0x80)cut--;
        text.resize(cut);
        text+="…";
    }
    return text;
}
std::string absolute(const App &app,const char *path){
    if(!*path)return {};
    return path_text(fs_path(app.root)/fs_path(path));
}
const char *tool_kind(std::string_view name){
    if(name=="read_file"||name=="list_files"||name=="code_outline"||name=="lsp"||name=="desktop_snapshot"||name=="desktop_screenshot"||name=="shell_output")return "read";
    if(name=="search_text"||name=="web_search")return "search";
    if(name=="edit_file"||name=="multi_edit"||name=="write_file")return "edit";
    if(name=="run_shell"||name=="shell_kill")return "execute";
    if(name=="web_fetch")return "fetch";
    if(name=="delegate_research"||name=="delegate_task"||name=="subagent_output"||name=="todo_write"||name=="submit_plan"||name=="skill")return "think";
    return "other";
}
std::string tool_title(std::string_view name,J *args){
    const std::string path=sget(args,"path");
    if(name=="read_file")return "Read "+path;
    if(name=="list_files")return "List "+path;
    if(name=="code_outline")return "Outline "+path;
    if(name=="search_text")return "Search \""+clip(sget(args,"query"),60)+"\" in "+path;
    if(name=="edit_file"||name=="multi_edit")return "Edit "+path;
    if(name=="write_file")return "Write "+path;
    if(name=="run_shell")return clip(sget(args,"command"),120);
    if(name=="web_fetch")return "Fetch "+clip(sget(args,"url"),100);
    if(name=="web_search")return "Search the web: "+clip(sget(args,"query"),80);
    if(name=="delegate_research")return "Research: "+clip(sget(args,"question"),80);
    if(name=="delegate_task")return "Subagent ("+std::string(*sget(args,"agent")?sget(args,"agent"):"coder")+"): "+clip(sget(args,"prompt"),72);
    if(name=="subagent_output")return std::string("Await ")+sget(args,"agent_id");
    if(name=="todo_write")return "Update plan";
    if(name=="submit_plan")return "Submit plan";
    if(name=="lsp")return std::string("LSP ")+sget(args,"action")+" "+path;
    if(name=="skill")return std::string("Use skill ")+sget(args,"name");
    if(name=="shell_output")return std::string("Check ")+sget(args,"shell_id");
    if(name=="shell_kill")return std::string("Stop ")+sget(args,"shell_id");
    if(name.starts_with("desktop_"))return "Desktop: "+std::string(name.substr(8));
    if(name.starts_with("mcp_"))return "MCP tool";
    return std::string(name);
}
J *plan_entries(J *todos){
    J *entries=arr();
    for(size_t i=0;i<alen(todos);i++){
        J *entry=obj();
        put(entry,"content",str(sget(at(todos,i),"content")));
        put(entry,"priority",str("medium"));
        put(entry,"status",str(sget(at(todos,i),"status")));
        add(entries,entry);
    }
    return entries;
}
J *text_content(const std::string &text){
    J *item=obj(),*inner=obj();
    put(item,"type",str("content"));
    put(inner,"type",str("text"));
    put(inner,"text",json_object_new_string_len(text.data(),static_cast<int>(text.size())));
    put(item,"content",inner);
    return item;
}
std::string file_text(const std::string &path){
    size_t length=0;
    std::unique_ptr<char,decltype(&free)> bytes(read_bytes(path.c_str(),FILE_LIMIT,&length),free);
    if(!bytes)return {};
    std::string text(bytes.get(),length);
    return valid_utf8(text)?text:std::string();
}

// Ask the client; blocks the worker until the reply or a cancellation.
int request_permission(Session &session,const std::string &tool_id,const std::string &kind_hint,J *tool_args,const char *title,const char *description,int operation){
    std::string id;
    {
        std::lock_guard lock(state_mutex);
        id="permission-"+std::to_string(++permission_counter);
    }
    JsonPtr message(obj());
    put(message.get(),"jsonrpc",str("2.0"));
    put(message.get(),"id",str(id.c_str()));
    put(message.get(),"method",str("session/request_permission"));
    J *params=obj(),*tool=obj(),*options=arr();
    put(params,"sessionId",str(session.id.c_str()));
    put(tool,"toolCallId",str(tool_id.empty()?"approval":tool_id.c_str()));
    put(tool,"title",str(clip(std::string(*title?title:"Approval")+": "+description,200).c_str()));
    const char *fallback_kind=operation==static_cast<int>(Operation::FileEdit)?"edit"
        :operation==static_cast<int>(Operation::Shell)?"execute":"other";
    put(tool,"kind",str(kind_hint.empty()?fallback_kind:kind_hint.c_str()));
    put(tool,"status",str("pending"));
    J *raw=tool_args?copy(tool_args):obj();
    if(!get(raw,"description"))put(raw,"description",str(description));
    put(tool,"rawInput",raw);
    put(params,"toolCall",tool);
    auto option=[&](const char *option_id,const char *name,const char *kind){
        J *entry=obj();
        put(entry,"optionId",str(option_id));
        put(entry,"name",str(name));
        put(entry,"kind",str(kind));
        add(options,entry);
    };
    option("allow","Allow","allow_once");
    if(operation==static_cast<int>(Operation::Shell))option("allow_always","Always allow commands like this in this session","allow_always");
    else if(operation==static_cast<int>(Operation::FileEdit))option("allow_always","Always allow file edits in this session","allow_always");
    option("reject","Reject","reject_once");
    put(params,"options",options);
    put(message.get(),"params",params);
    send(message.get());
    JsonPtr reply;
    {
        std::unique_lock lock(state_mutex);
        while(!permission_replies.count(id)&&!session.cancelled&&!shutting_down)
            state_changed.wait_for(lock,std::chrono::milliseconds(100));
        auto found=permission_replies.find(id);
        if(found!=permission_replies.end()){reply=std::move(found->second);permission_replies.erase(found);}
    }
    J *outcome=get(get(reply.get(),"result"),"outcome");
    if(strcmp(sget(outcome,"outcome"),"selected"))return 0;
    const std::string choice=sget(outcome,"optionId");
    if(choice=="allow_always"){
        if(operation==static_cast<int>(Operation::Shell))remember_allowed_command(*session.app,description);
        else if(operation==static_cast<int>(Operation::FileEdit)&&session.mode==ASK){
            session.mode=EDIT;
            session.app->auto_edit=true;
            J *update=obj();
            put(update,"sessionUpdate",str("current_mode_update"));
            put(update,"currentModeId",str("edit"));
            session_update(session.id,update);
        }
        return 1;
    }
    return choice=="allow"?1:0;
}

void run_prompt(Session &session,J *request_id,J *prompt){
    App &app=*session.app;
    const std::string task=compose_prompt(app,prompt);
    if(task.empty()){respond_error(request_id,-32602,"The prompt has no text.");return;}
    apply_mode(app,session.mode);
    std::string current_tool,current_tool_kind;
    JsonPtr current_tool_args;
    std::map<std::string,std::string> before;
    const std::string sid=session.id;
    app.stream_observer=[&](const std::string &part){text_chunk(sid,part);};
    app.on_assistant_text=[&](const char *text,bool streamed){if(!streamed)text_chunk(sid,text);};
    app.on_tool_start=[&](const char *id,const char *name,J *args){
        current_tool=id;
        current_tool_kind=tool_kind(name);
        current_tool_args.reset(args?copy(args):nullptr);
        J *update=obj();
        put(update,"sessionUpdate",str("tool_call"));
        put(update,"toolCallId",str(id));
        put(update,"title",str(tool_title(name,args).c_str()));
        put(update,"kind",str(tool_kind(name)));
        put(update,"status",str("in_progress"));
        put(update,"rawInput",args?copy(args):obj());
        if(*sget(args,"path")){
            J *locations=arr(),*location=obj();
            put(location,"path",str(absolute(app,sget(args,"path")).c_str()));
            if(iget(args,"line",0)>0)put(location,"line",json_object_new_int(iget(args,"line",0)));
            add(locations,location);
            put(update,"locations",locations);
            if(!strcmp(tool_kind(name),"edit"))before[id]=file_text(absolute(app,sget(args,"path")));
        }
        session_update(sid,update);
    };
    app.on_tool_end=[&](const char *id,const char *name,J *args,J *result){
        const bool failed=tool_failed(result);
        J *update=obj(),*content=arr();
        put(update,"sessionUpdate",str("tool_call_update"));
        put(update,"toolCallId",str(id));
        put(update,"status",str(failed?"failed":"completed"));
        const std::string_view tool=name;
        if(failed)add(content,text_content(*sget(result,"error")?sget(result,"error"):"The operation failed."));
        else if(!strcmp(tool_kind(name),"edit")){
            const std::string path=absolute(app,sget(args,"path"));
            J *diff=obj();
            put(diff,"type",str("diff"));
            put(diff,"path",str(path.c_str()));
            auto old=before.find(id);
            put(diff,"oldText",old==before.end()||old->second.empty()?nullptr:str(old->second.c_str()));
            put(diff,"newText",str(file_text(path).c_str()));
            add(content,diff);
            if(*sget(result,"diagnostics_summary"))add(content,text_content(sget(result,"diagnostics_summary")));
        }else if(tool=="run_shell"||tool=="shell_output"||tool=="shell_kill"){
            std::string output=sget(result,"output");
            if(output.size()>8000)output=output.substr(output.size()-8000);
            std::string head=bget(result,"running")?"running in background "+std::string(sget(result,"shell_id"))
                :"exit code "+std::to_string(iget(result,"exit_code",0));
            add(content,text_content(head+(output.empty()?"":"\n"+output)));
        }else if(tool=="todo_write"){
            J *plan=obj();
            put(plan,"sessionUpdate",str("plan"));
            put(plan,"entries",plan_entries(app.todos));
            session_update(sid,plan);
        }else if(tool=="submit_plan")text_chunk(sid,sget(args,"plan"));
        before.erase(id);
        if(alen(content))put(update,"content",content);
        else json_object_put(content);
        session_update(sid,update);
        current_tool.clear();
        current_tool_kind.clear();
        current_tool_args.reset();
    };
    app.approval_hook=[&](const char *title,const char *description,int operation){
        const int mode=session.mode;
        if(mode==FULL||(mode==EDIT&&operation==static_cast<int>(Operation::FileEdit)))return 1;
        return request_permission(session,current_tool,current_tool_kind,current_tool_args.get(),title,description,operation);
    };
    interrupted=0;
    background_requested=false;
    const int rc=run_task(&app,task.c_str());
    const char *stop="end_turn";
    if(session.cancelled||interrupted)stop="cancelled";
    else if(rc&&!strcmp(app.error,tr("达到步骤上限，任务未确认完成。")))stop="max_turn_requests";
    else if(rc&&app.error[0])text_chunk(sid,std::string("\n\n⚠ ")+app.error);
    app.stream_observer=nullptr;
    app.on_assistant_text=nullptr;
    app.on_tool_start=nullptr;
    app.on_tool_end=nullptr;
    app.approval_hook=nullptr;
    interrupted=0;
    J *result=obj();
    put(result,"stopReason",str(stop));
    respond(request_id,result);
}

void worker(){
    for(;;){
        PromptJob job;
        Session *session=nullptr;
        {
            std::unique_lock lock(state_mutex);
            state_changed.wait(lock,[]{return shutting_down||!queue.empty();});
            if(queue.empty())return;
            job=std::move(queue.front());
            queue.pop_front();
            auto found=sessions.find(job.session);
            if(found!=sessions.end()){session=found->second.get();running_session=job.session;}
        }
        if(!session)respond_error(job.id.get(),-32602,"Unknown session.");
        else if(session->cancelled){
            J *result=obj();put(result,"stopReason",str("cancelled"));
            respond(job.id.get(),result);
        }else run_prompt(*session,job.id.get(),job.prompt.get());
        std::lock_guard lock(state_mutex);
        running_session.clear();
        if(session)session->cancelled=false;
        if(shutting_down&&queue.empty())return;
    }
}
}

namespace {
void handle_request(App &base,J *message){
    J *id=get(message,"id");
    const std::string method=sget(message,"method");
    J *params=get(message,"params");
    if(method=="initialize"){
        J *result=obj(),*capabilities=obj(),*prompt=obj(),*mcp=obj(),*info=obj();
        put(result,"protocolVersion",json_object_new_int(1));
        put(capabilities,"loadSession",json_object_new_boolean(0));
        put(prompt,"image",json_object_new_boolean(0));
        put(prompt,"audio",json_object_new_boolean(0));
        put(prompt,"embeddedContext",json_object_new_boolean(1));
        put(capabilities,"promptCapabilities",prompt);
        put(mcp,"http",json_object_new_boolean(1));
        put(mcp,"sse",json_object_new_boolean(0));
        put(capabilities,"mcpCapabilities",mcp);
        put(result,"agentCapabilities",capabilities);
        put(result,"authMethods",arr());
        put(info,"name",str("deskcode"));
        put(info,"title",str("DeskCode"));
        put(info,"version",str(VERSION));
        put(result,"agentInfo",info);
        respond(id,result);
        return;
    }
    if(method=="authenticate"){respond(id,obj());return;}
    if(method=="session/new"){
        auto session=std::make_unique<Session>();
        auto app=std::make_unique<App>();
        const char *cwd=*sget(params,"cwd")?sget(params,"cwd"):base.root;
        if(!canonical_directory(cwd,app->root)){respond_error(id,-32602,"cwd must be an existing directory.");return;}
        snprintf(app->config_dir,sizeof app->config_dir,"%s",base.config_dir);
        app->store=copy(base.store);
        app->config=copy(base.config);
        app->messages=arr();
        app->shell=base.shell;
        app->sandbox=base.sandbox;
        app->max_steps=base.max_steps;
        app->json_output=true;
        app->state="等待任务";
        J *servers=mcp_from_acp(get(params,"mcpServers"));
        if(json_object_object_length(servers))app->extra_mcp=servers;
        else json_object_put(servers);
        char sid[33];id_string(sid);
        session->id=sid;
        session->mode=base.plan_mode?PLAN:base.yes?FULL:base.auto_edit?EDIT:ASK;
        session->app=std::move(app);
        J *result=obj();
        put(result,"sessionId",str(sid));
        put(result,"modes",modes_state(session->mode));
        {
            std::lock_guard lock(state_mutex);
            sessions[sid]=std::move(session);
        }
        respond(id,result);
        return;
    }
    if(method=="session/set_mode"){
        const int mode=mode_from(sget(params,"modeId"));
        std::lock_guard lock(state_mutex);
        auto found=sessions.find(sget(params,"sessionId"));
        if(found==sessions.end()||mode<0){respond_error(id,-32602,"Unknown session or mode.");return;}
        found->second->mode=mode;
        // A running turn picks the mode up at its next approval; the tool set
        // changes from the next prompt.
        if(running_session!=found->first)apply_mode(*found->second->app,mode);
        respond(id,obj());
        return;
    }
    if(method=="session/prompt"){
        std::lock_guard lock(state_mutex);
        if(!sessions.count(sget(params,"sessionId"))){respond_error(id,-32602,"Unknown session.");return;}
        queue.push_back({JsonPtr(copy(id)),sget(params,"sessionId"),JsonPtr(copy(get(params,"prompt")))});
        state_changed.notify_all();
        return;
    }
    respond_error(id,-32601,"Method not found.");
}
void handle_notification(J *message){
    if(strcmp(sget(message,"method"),"session/cancel"))return;
    const std::string sid=sget(get(message,"params"),"sessionId");
    std::lock_guard lock(state_mutex);
    auto found=sessions.find(sid);
    if(found==sessions.end())return;
    found->second->cancelled=true;
    if(running_session==sid)interrupted=SIGINT;
    state_changed.notify_all();
}
}

int acp_main(App &app){
    if(config_valid(&app,app.config)){
        fprintf(stderr,tr("%s 请运行 desk，在菜单中添加模型。\n"),app.error);
        return 1;
    }
    fflush(stdout);
    const int protocol_fd=acp_dup(1);
    if(protocol_fd<0||acp_dup2(2,1)<0)return 1;
#ifdef _WIN32
    _setmode(protocol_fd,_O_BINARY);
#endif
    protocol_out=acp_fdopen(protocol_fd,"w");
    if(!protocol_out)return 1;
    std::thread prompts(worker);
    std::string line;
    while(std::getline(std::cin,line)){
        if(!line.empty()&&line.back()=='\r')line.pop_back();
        if(line.empty())continue;
        JsonPtr message(json_tokener_parse(line.c_str()));
        if(!json_object_is_type(message.get(),json_type_object)){respond_error(nullptr,-32700,"Parse error.");continue;}
        const bool has_method=*sget(message.get(),"method")!=0;
        J *id=get(message.get(),"id");
        if(has_method&&id)handle_request(app,message.get());
        else if(has_method)handle_notification(message.get());
        else if(id){
            // A reply to one of our requests (permission).
            std::lock_guard lock(state_mutex);
            permission_replies[json_object_get_string(id)].reset(copy(message.get()));
            state_changed.notify_all();
        }
    }
    {
        std::lock_guard lock(state_mutex);
        shutting_down=true;
        if(!running_session.empty())interrupted=SIGTERM;
        for(auto &[sid,session]:sessions)session->cancelled=true;
        state_changed.notify_all();
    }
    prompts.join();
    {
        std::lock_guard lock(state_mutex);
        for(auto &[sid,session]:sessions)app_cleanup(session->app.get());
        sessions.clear();
    }
    fclose(protocol_out);
    protocol_out=nullptr;
    return 0;
}
