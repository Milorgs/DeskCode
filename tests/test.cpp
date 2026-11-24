#include "desk.hpp"
#include "tasks.hpp"
#include "mcp.hpp"
#include "ui.hpp"
#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <arpa/inet.h>
#ifdef __APPLE__
#include <util.h>
#include <mach-o/dyld.h>
#include <sys/sysctl.h>
#else
#include <pty.h>
#endif
#include <termios.h>
#include <algorithm>
#include <functional>
#include <filesystem>
#include <thread>
#include <poll.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
static int checks=0;
static void cancel_command(int number){(void)number;interrupted=SIGINT;}
#define CHECK(x) do{checks++;if(!(x)){fprintf(stderr,"FAIL %s:%d: %s\n",__FILE__,__LINE__,#x);exit(1);}}while(0)
static char sandbox[PATH_MAX];
static int sequence=0;
static App fresh(void){
    App a={
        .yes=true,.shell=true,.max_steps=8,.state="等待任务",.stream_text={}
    };
    char suffix[64];
    snprintf(suffix,sizeof suffix,"/case-%d",sequence++);
    CHECK(path_suffix(a.root,sandbox,suffix)==0);
    CHECK(mkdirs(a.root,0700)==0);
    char cfg[PATH_MAX];
    CHECK(path_suffix(cfg,a.root,"/config")==0);
    CHECK(setenv("XDG_CONFIG_HOME",cfg,1)==0);
    unsetenv("DESK_API_KEY");
    unsetenv("DESK_BASE_URL");
    unsetenv("DESK_MODEL");
    unsetenv("DESK_API_TYPE");
    CHECK(store_load(&a)==0);
    reset_session(&a);
    return a;
}
static J *config(const char *name,const char *protocol){
    J *c=config_default(name);
    put(c,"model",str("model-test"));
    put(c,"api_key",str("fake-test-key"));
    put(c,"protocol",str(protocol));
    put(c,"base_url",str(!strcmp(protocol,"anthropic")?"https://api.anthropic.com/v1":"https://example.com/v1"));
    return c;
}
static J *args(const char *path){
    J *v=obj();
    put(v,"path",str(path));
    return v;
}
static void test_utf8(void){
    CHECK(valid_utf8("中文 English 😀"));
    CHECK(valid_utf8(""));
    CHECK(!valid_utf8("\xc0\xaf"));
    CHECK(!valid_utf8("\xed\xa0\x80"));
    CHECK(!valid_utf8("\xf4\x90\x80\x80"));
    CHECK(!valid_utf8("\xe4\xb8"));
    CHECK(!valid_utf8("\x80"));
}
static void test_files(void){
    App a=fresh();
    J *v=args("你好.txt");
    put(v,"content",str("第一版"));
    put(v,"expected_sha256",NULL);
    J *r=tool_call(&a,"write_file",v);
    CHECK(!get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    v=args("你好.txt");
    r=tool_call(&a,"read_file",v);
    CHECK(!strcmp(sget(r,"content"),"第一版"));
    char *digest=strdup(sget(r,"sha256"));
    json_object_put(r);
    json_object_put(v);
    v=args("你好.txt");
    put(v,"content",str("第二版"));
    put(v,"expected_sha256",str(digest));
    r=tool_call(&a,"write_file",v);
    CHECK(!get(r,"error"));
    char *backup=read_bytes(sget(r,"backup"),FILE_LIMIT,NULL);
    CHECK(backup&&!strcmp(backup,"第一版"));
    free(backup);
    json_object_put(r);
    r=tool_call(&a,"write_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    free(digest);
    v=args("../outside");
    r=tool_call(&a,"read_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    v=args(".env");
    r=tool_call(&a,"read_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    char link[PATH_MAX];
    CHECK(!path_suffix(link,a.root,"/escape"));
    CHECK(!symlink("/etc",link));
    v=args("escape/passwd");
    r=tool_call(&a,"read_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    char *hash=sha256("abc",3);
    CHECK(!strcmp(hash,"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"));
    free(hash);
    // Sliced read_file: exact 1-based line range on normal and >256 KB files.
    {
        std::string multi="第一行\n第二行\n第三行\n第四行\n";
        char multi_path[PATH_MAX];
        CHECK(!path_suffix(multi_path,a.root,"/lines.txt"));
        CHECK(!atomic_file(multi_path,multi.data(),multi.size(),0600));
        char *multi_hash=sha256(multi.data(),multi.size());
        JsonPtr slice_args(args("lines.txt"));
        put(slice_args.get(),"start_line",json_object_new_int(2));
        put(slice_args.get(),"end_line",json_object_new_int(3));
        JsonPtr slice_res(tool_call(&a,"read_file",slice_args.get()));
        CHECK(!get(slice_res.get(),"error"));
        CHECK(bget(slice_res.get(),"sliced"));
        CHECK(iget(slice_res.get(),"start_line",0)==2);
        CHECK(iget(slice_res.get(),"end_line",0)==3);
        CHECK(iget(slice_res.get(),"total_lines",0)==4);
        CHECK(!strcmp(sget(slice_res.get(),"content"),"第二行\n第三行\n"));
        CHECK(!strcmp(sget(slice_res.get(),"sha256"),multi_hash));
        free(multi_hash);

        // Invalid range and wrong parameter types must be rejected.
        put(slice_args.get(),"start_line",json_object_new_int(0));
        slice_res.reset(tool_call(&a,"read_file",slice_args.get()));
        CHECK(get(slice_res.get(),"error"));
        put(slice_args.get(),"start_line",str("2"));
        slice_res.reset(tool_call(&a,"read_file",slice_args.get()));
        CHECK(get(slice_res.get(),"error"));

        // Files >256 KB fail full read_file, but succeed with a line slice.
        std::string huge;
        huge.reserve(320000);
        for(int i=1;i<=5000;i++)huge+="Line "+std::to_string(i)+": 0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ\n";
        CHECK(huge.size()>FILE_LIMIT);
        char huge_path[PATH_MAX];
        CHECK(!path_suffix(huge_path,a.root,"/huge.log"));
        CHECK(!atomic_file(huge_path,huge.data(),huge.size(),0600));
        JsonPtr full_req(args("huge.log")),full_res(tool_call(&a,"read_file",full_req.get()));
        CHECK(get(full_res.get(),"error"));
        JsonPtr huge_slice(args("huge.log"));
        put(huge_slice.get(),"start_line",json_object_new_int(4999));
        put(huge_slice.get(),"end_line",json_object_new_int(5000));
        JsonPtr huge_res(tool_call(&a,"read_file",huge_slice.get()));
        CHECK(!get(huge_res.get(),"error"));
        CHECK(iget(huge_res.get(),"total_lines",0)==5000);
        CHECK(strstr(sget(huge_res.get(),"content"),"Line 4999:")&&strstr(sget(huge_res.get(),"content"),"Line 5000:"));
        CHECK(strlen(sget(huge_res.get(),"sha256"))==64);
        JsonPtr huge_edit(args("huge.log"));
        put(huge_edit.get(),"old_text",str("Line 4999: 0123456789"));
        put(huge_edit.get(),"new_text",str("Line 4999 [EDITED]: 0123456789"));
        put(huge_edit.get(),"expected_sha256",str(sget(huge_res.get(),"sha256")));
        JsonPtr huge_edit_res(tool_call(&a,"edit_file",huge_edit.get()));
        CHECK(!get(huge_edit_res.get(),"error")&&bget(huge_edit_res.get(),"verified"));

        // Sorted list_files with file sizes and recursive max_depth.
        char sub_dir[PATH_MAX],sub_file[PATH_MAX];
        CHECK(!path_suffix(sub_dir,a.root,"/subdir"));
        CHECK(!mkdirs(sub_dir,0700));
        CHECK(!path_suffix(sub_file,a.root,"/subdir/nested.txt"));
        CHECK(!atomic_file(sub_file,"nested",6,0600));
        JsonPtr list_req(args("."));
        put(list_req.get(),"max_depth",json_object_new_int(2));
        JsonPtr list_res(tool_call(&a,"list_files",list_req.get()));
        CHECK(!get(list_res.get(),"error"));
        J *entries=get(list_res.get(),"entries");
        CHECK(alen(entries)>=3);
        CHECK(bget(at(entries,0),"directory")); // directories sorted first
        bool found_nested=false;
        for(size_t idx=0;idx<alen(entries);++idx){
            J *item=at(entries,idx);
            if(!strcmp(sget(item,"name"),"subdir/nested.txt")){
                found_nested=true;
                CHECK(iget(item,"size",0)==6);
            }
        }
        CHECK(found_nested);

        // Case-insensitive search_text and CRLF-tolerant edit_file + match_lines diagnostics.
        const char *crlf_doc="Header=Alpha\r\nSection=DeskCode\r\nFooter=Alpha\r\n";
        char crlf_path[PATH_MAX];
        CHECK(!path_suffix(crlf_path,a.root,"/crlf.txt"));
        CHECK(!atomic_file(crlf_path,crlf_doc,strlen(crlf_doc),0600));
        JsonPtr ci_search(args("crlf.txt"));
        put(ci_search.get(),"query",str("deskcode"));
        put(ci_search.get(),"case_insensitive",json_object_new_boolean(1));
        JsonPtr ci_res(tool_call(&a,"search_text",ci_search.get()));
        CHECK(alen(get(ci_res.get(),"matches"))==1&&iget(at(get(ci_res.get(),"matches"),0),"line",0)==2);

        char *crlf_hash=sha256(crlf_doc,strlen(crlf_doc));
        JsonPtr dup_edit(args("crlf.txt"));
        put(dup_edit.get(),"old_text",str("Alpha"));
        put(dup_edit.get(),"new_text",str("Beta"));
        put(dup_edit.get(),"expected_sha256",str(crlf_hash));
        JsonPtr dup_res(tool_call(&a,"edit_file",dup_edit.get()));
        CHECK(get(dup_res.get(),"error"));
        CHECK(iget(dup_res.get(),"match_count",0)==2);
        CHECK(alen(get(dup_res.get(),"match_lines"))==2);
        CHECK(json_object_get_int(at(get(dup_res.get(),"match_lines"),0))==1);
        CHECK(json_object_get_int(at(get(dup_res.get(),"match_lines"),1))==3);

        // LF old_text/new_text against CRLF file succeeds and preserves CRLF.
        JsonPtr lf_edit(args("crlf.txt"));
        put(lf_edit.get(),"old_text",str("Header=Alpha\nSection=DeskCode"));
        put(lf_edit.get(),"new_text",str("Header=Updated\nSection=DeskCode"));
        put(lf_edit.get(),"expected_sha256",str(crlf_hash));
        free(crlf_hash);
        JsonPtr lf_res(tool_call(&a,"edit_file",lf_edit.get()));
        CHECK(!get(lf_res.get(),"error")&&bget(lf_res.get(),"verified"));
        char *after_crlf=read_bytes(crlf_path,FILE_LIMIT,NULL);
        CHECK(after_crlf&&!strcmp(after_crlf,"Header=Updated\r\nSection=DeskCode\r\nFooter=Alpha\r\n"));
        free(after_crlf);
    }
    app_cleanup(&a);
}
static void fixture(App *a,const char *name,const void *data,size_t length){
    char path[PATH_MAX];
    CHECK(!path_suffix(path,a->root,name));
    CHECK(!atomic_file(path,data,length,0600));
}
static void test_search_and_edit(void){
    App a=fresh();
    const char *original="标题=旧值\r\n保留这一行\r\n";
    fixture(&a,"/文档.txt",original,strlen(original));
    fixture(&a,"/.env","旧值",strlen("旧值"));
    const char binary[]={ 'x', (char)0xff, '\n' };
    fixture(&a,"/binary",binary,sizeof binary);
    char ignored[PATH_MAX],link[PATH_MAX];
    CHECK(!path_suffix(ignored,a.root,"/node_modules"));
    CHECK(!mkdirs(ignored,0700));
    fixture(&a,"/node_modules/dependency","旧值",strlen("旧值"));
    CHECK(!path_suffix(link,a.root,"/alias.txt"));
    CHECK(!symlink("文档.txt",link));
    J *v=args(".");
    put(v,"query",str("旧值"));
    J *r=tool_call(&a,"search_text",v);
    CHECK(!get(r,"error"));
    CHECK(alen(get(r,"matches"))==1);
    CHECK(!strcmp(sget(at(get(r,"matches"),0),"path"),"文档.txt"));
    CHECK(iget(at(get(r,"matches"),0),"line",0)==1);
    CHECK(iget(r,"skipped",0)>=4);
    CHECK(!bget(r,"truncated"));
    json_object_put(r);
    put(v,"query",str(""));
    r=tool_call(&a,"search_text",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    v=args("文档.txt");
    r=tool_call(&a,"read_file",v);
    char *hash=strdup(sget(r,"sha256"));
    json_object_put(r);
    put(v,"old_text",str("旧值"));
    put(v,"new_text",str("新值"));
    put(v,"expected_sha256",str(hash));
    r=tool_call(&a,"edit_file",v);
    CHECK(!get(r,"error")&&bget(r,"verified"));
    char *backup=read_bytes(sget(r,"backup"),FILE_LIMIT,NULL);
    CHECK(backup&&!strcmp(backup,original));
    free(backup);
    char path[PATH_MAX];
    CHECK(!path_suffix(path,a.root,"/文档.txt"));
    char *updated=read_bytes(path,FILE_LIMIT,NULL);
    CHECK(updated&&!strcmp(updated,"标题=新值\r\n保留这一行\r\n"));
    char *actual_hash=sha256(updated,strlen(updated));
    CHECK(!strcmp(sget(r,"sha256"),actual_hash));
    free(actual_hash);
    free(updated);
    json_object_put(r);
    struct stat st;
    CHECK(!stat(path,&st)&&(st.st_mode&0777)==0600);
    r=tool_call(&a,"edit_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    put(v,"expected_sha256",NULL);
    r=tool_call(&a,"edit_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    free(hash);
    fixture(&a,"/overlap.txt","aaa",3);
    v=args("overlap.txt");
    put(v,"old_text",str("aa"));
    put(v,"new_text",str("z"));
    hash=sha256("aaa",3);
    put(v,"expected_sha256",str(hash));
    r=tool_call(&a,"edit_file",v);
    CHECK(get(r,"error"));
    json_object_put(r);
    put(v,"old_text",str("aaa"));
    put(v,"new_text",str(""));
    r=tool_call(&a,"edit_file",v);
    CHECK(!get(r,"error"));
    json_object_put(r);
    json_object_put(v);
    free(hash);
    CHECK(!path_suffix(path,a.root,"/overlap.txt"));
    size_t n=99;
    updated=read_bytes(path,FILE_LIMIT,&n);
    CHECK(updated&&n==0);
    free(updated);
    char many[4096]="";
    for(int i=0;i<200;i++)strcat(many,"匹配\n");
    fixture(&a,"/many.txt",many,strlen(many));
    v=args("many.txt");
    put(v,"query",str("匹配"));
    r=tool_call(&a,"search_text",v);
    CHECK(alen(get(r,"matches"))==120&&bget(r,"truncated"));
    CHECK(iget(at(get(r,"matches"),119),"line",0)==120);
    json_object_put(r);
    json_object_put(v);
    // Line numbers survive skipped lines, blank lines, a repeat on one line and
    // a final line without a newline.
    const char sparse[]="无关\n\n命中 一\n无关\n\n\n命中 命中 二\n无关\n末行命中";
    fixture(&a,"/sparse.txt",sparse,strlen(sparse));
    v=args("sparse.txt");
    put(v,"query",str("命中"));
    r=tool_call(&a,"search_text",v);
    CHECK(alen(get(r,"matches"))==3&&!bget(r,"truncated"));
    CHECK(iget(at(get(r,"matches"),0),"line",0)==3);
    CHECK(!strcmp(sget(at(get(r,"matches"),0),"text"),"命中 一"));
    CHECK(iget(at(get(r,"matches"),1),"line",0)==7);
    CHECK(!strcmp(sget(at(get(r,"matches"),1),"text"),"命中 命中 二"));
    CHECK(iget(at(get(r,"matches"),2),"line",0)==9);
    CHECK(!strcmp(sget(at(get(r,"matches"),2),"text"),"末行命中"));
    json_object_put(r);
    json_object_put(v);
    app_cleanup(&a);
}
static void test_config(void){
    App a=fresh();
    J *one=config("日常助手","openai"),*two=config("备用","anthropic");
    CHECK(!save_config(&a,one));
    put(two,"api_key",str("different-native-key"));
    put(two,"max_tokens",json_object_new_int(8192));
    CHECK(!save_config(&a,two));
    CHECK(!strcmp(sget(a.config,"profile"),"备用"));
    CHECK(!activate(&a,"日常助手"));
    CHECK(!strcmp(sget(a.config,"protocol"),"openai"));
    CHECK(!strcmp(sget(a.config,"api_key"),"fake-test-key"));
    J *kept=obj();put(kept,"role",str("user"));put(kept,"content",str("keep this context"));add(a.messages,kept);
    const auto revision=a.session_revision;
    CHECK(!activate(&a,"日常助手"));CHECK(a.session_revision==revision);CHECK(alen(a.messages)==1);
    char path[PATH_MAX];
    CHECK(!path_suffix(path,a.config_dir,"/settings.json"));
    struct stat st;
    CHECK(!stat(path,&st));
    CHECK((st.st_mode&0777)==0600);
    J *arguments=args(path),*result=tool_call(&a,"read_file",arguments);
    CHECK(get(result,"error"));
    json_object_put(result);
    json_object_put(arguments);
    J *bad=copy(two);
    put(bad,"base_url",str("http://remote.example/v1"));
    CHECK(config_valid(&a,bad)!=0);
    put(bad,"base_url",str("https://user:password@example.com/v1"));
    CHECK(config_valid(&a,bad)!=0);
    json_object_put(bad);
    App b={};
    CHECK(!store_load(&b));
    CHECK(!strcmp(sget(b.config,"profile"),"日常助手"));
    CHECK(iget(get(get(b.store,"profiles"),"备用"),"max_tokens",0)==8192);
    app_cleanup(&b);
    json_object_put(one);
    json_object_put(two);
    app_cleanup(&a);
}
static void test_migration(void){
    App a=fresh();
    char toml[PATH_MAX],auth[PATH_MAX];
    CHECK(!mkdirs(a.config_dir,0700));
    CHECK(!path_suffix(toml,a.config_dir,"/config.toml"));
    CHECK(!path_suffix(auth,a.config_dir,"/auth.json"));
    const char *text="active_profile = \"claude\"\n\n[profiles.\"claude\"]\nbase_url = \"https://api.anthropic.com/v1\"\nmodel = \"claude-test\"\nprotocol = \"anthropic\"\nmax_tokens = 8192\nanthropic_version = \"2023-06-01\"\n";
    CHECK(!atomic_file(toml,text,strlen(text),0600));
    const char *keys="{\"profiles\":{\"claude\":\"legacy-secret\"}}";
    CHECK(!atomic_file(auth,keys,strlen(keys),0600));
    App b={};
    CHECK(!store_load(&b));
    CHECK(!strcmp(sget(b.config,"api_key"),"legacy-secret"));
    CHECK(iget(b.config,"max_tokens",0)==8192);
    CHECK(!strcmp(sget(b.config,"protocol"),"anthropic"));
    CHECK(!save_config(&b,b.config));
    char *unchanged=read_bytes(toml,262144,NULL);
    CHECK(unchanged&&!strcmp(unchanged,text));
    free(unchanged);
    app_cleanup(&a);
    app_cleanup(&b);
}
static bool alive(pid_t pid){
#ifdef __APPLE__
    int mib[4]={CTL_KERN,KERN_PROC,KERN_PROC_PID,static_cast<int>(pid)};
    struct kinfo_proc info{};
    size_t len=sizeof info;
    if(sysctl(mib,4,&info,&len,nullptr,0)<0||len==0)return false;
    return info.kp_proc.p_stat!=SZOMB;
#else
    char path[64];
    snprintf(path,sizeof path,"/proc/%ld/stat",(long)pid);
    FILE *f=fopen(path,"r");
    if(!f)return false;
    char line[2048];
    char *result=fgets(line,sizeof line,f);
    fclose(f);
    if(!result)return false;
    char *p=strrchr(line,')');
    return p&&p[2]!='Z'&&p[2]!='X';
#endif
}
static void test_process(void){
    App a=fresh();
    J *r=run_command(&a,"printf '你好'; printf ' error' >&2; exit 7",2000);
    CHECK(iget(r,"exit_code",0)==7);
    CHECK(!strcmp(sget(r,"output"),"你好 error"));
    json_object_put(r);
    setenv("DESK_API_KEY","dont-echo-this",1);
    r=run_command(&a,"printf '%s' \"${DESK_API_KEY-unset}\"",2000);
    CHECK(!strcmp(sget(r,"output"),"unset"));
    json_object_put(r);
    unsetenv("DESK_API_KEY");
    r=run_command(&a,"head -c 3000000 /dev/zero > large.bin && head -c 3000000 large.bin",5000);
    CHECK(iget(r,"exit_code",-1)==0);
    CHECK(!bget(r,"timed_out"));
    CHECK(!bget(r,"truncated"));
    CHECK(json_object_get_string_len(get(r,"output"))==3000000);
    json_object_put(r);
    r=run_command(&a,"head -c 9000000 /dev/zero",5000);
    CHECK(iget(r,"exit_code",-1)==0);
    CHECK(bget(r,"truncated"));
    CHECK(json_object_get_string_len(get(r,"output"))==COMMAND_OUTPUT_LIMIT);
    json_object_put(r);
    r=run_command(&a,"sleep 10 & echo $! > child.pid; wait",50);
    CHECK(bget(r,"timed_out"));
    CHECK(iget(r,"exit_code",0)<0);
    json_object_put(r);
    char path[PATH_MAX];
    CHECK(!path_suffix(path,a.root,"/child.pid"));
    char *pid_text=read_bytes(path,100,NULL);
    CHECK(pid_text);
    pid_t pid=(pid_t)strtol(pid_text,NULL,10);
    free(pid_text);
    double until=monotime()+2;
    while(alive(pid)&&monotime()<until)usleep(1000);
    CHECK(!alive(pid));
    struct sigaction action{},previous;
    action.sa_handler=cancel_command;
    sigemptyset(&action.sa_mask);
    CHECK(!sigaction(SIGALRM,&action,&previous));
    alarm(1);
    r=run_command(&a,"sleep 30",30000);
    alarm(0);
    CHECK(interrupted==SIGINT);
    CHECK(get(r,"error"));
    json_object_put(r);
    interrupted=0;
    CHECK(!sigaction(SIGALRM,&previous,NULL));
    app_cleanup(&a);
}
static void test_context_and_validation(void){
    App a=fresh();
    for(int i=0;i<10;i++){
        J *m=obj();
        put(m,"role",str("user"));
        put(m,"content",str("old"));
        add(a.messages,m);
        m=obj();
        put(m,"role",str("assistant"));
        put(m,"content",str("done"));
        add(a.messages,m);
    }
    J *m=obj();
    put(m,"role",str("user"));
    put(m,"content",str("current"));
    add(a.messages,m);
    J *defs=schemas(&a),*ctx=context(&a,defs);
    CHECK(ctx);
    CHECK(alen(ctx)==21);
    CHECK(alen(a.messages)==21);
    CHECK(!strcmp(sget(at(ctx,alen(ctx)-1),"content"),"current"));
    json_object_put(ctx);
    J *call_reply=json_tokener_parse(R"({"role":"assistant","tool_calls":[{"id":"shot","type":"function","function":{"name":"mcp_screenshot","arguments":"{}"}}]})");
    add(a.messages,call_reply);
    J *shot=obj();
    put(shot,"role",str("tool"));
    put(shot,"tool_call_id",str("shot"));
    const std::string oversized=R"({"content":[{"type":"image","data":")"+std::string(CONTEXT_LIMIT*2,'A')+
        R"("},{"type":"text","text":"Desktop tree still available"},{"type":"text","text":")"+
        std::string(CONTEXT_LIMIT*2,'x')+R"("}]})";
    put(shot,"content",str(oversized.c_str()));
    add(a.messages,shot);
    bool compacted=false;
    JsonPtr compact(context(&a,defs,&compacted));
    CHECK(compact&&compacted);
    CHECK(alen(compact.get())==5);
    CHECK(!strcmp(sget(at(compact.get(),2),"content"),"current"));
    CHECK(!strcmp(sget(at(compact.get(),4),"tool_call_id"),"shot"));
    CHECK(strstr(sget(at(compact.get(),4),"content"),"Desktop tree still available"));
    CHECK(strstr(sget(at(compact.get(),4),"content"),"smaller screenshot"));
    CHECK(!strstr(sget(at(compact.get(),4),"content"),"AAAAAAAA"));
    CHECK(strlen(sget(shot,"content"))==oversized.size());
    J *followup=obj();
    put(followup,"role",str("user"));
    put(followup,"content",str("continue"));
    add(a.messages,followup);
    compact.reset(context(&a,defs,&compacted));
    CHECK(compact&&!compacted);
    const size_t followup_size=alen(compact.get());
    CHECK(followup_size==24);
    CHECK(!strcmp(sget(at(compact.get(),followup_size-2),"tool_call_id"),"shot"));
    CHECK(strstr(sget(at(compact.get(),followup_size-2),"content"),"Desktop tree still available"));
    CHECK(!json_object_array_del_idx(a.messages,alen(a.messages)-1,1));
    for(int i=0;i<20;++i){
        const std::string id="extra-"+std::to_string(i);
        J *reply=copy(call_reply),*tool=copy(shot);
        put(at(get(reply,"tool_calls"),0),"id",str(id.c_str()));
        put(tool,"tool_call_id",str(id.c_str()));
        const std::string result=R"({"output":")"+std::string(8000,'x')+R"("})";
        put(tool,"content",str(result.c_str()));
        add(a.messages,reply);
        add(a.messages,tool);
    }
    compact.reset(context(&a,defs,&compacted));
    CHECK(compact&&compacted);
    CHECK(strstr(sget(at(compact.get(),1),"content"),"Automatic context summary"));
    CHECK(!strcmp(sget(at(compact.get(),2),"content"),"current"));
    CHECK(alen(compact.get())<43);
    CHECK(!strcmp(sget(at(compact.get(),alen(compact.get())-1),"tool_call_id"),"extra-19"));
    for(size_t i=3;i<alen(compact.get());i+=2){
        CHECK(!strcmp(sget(at(get(at(compact.get(),i),"tool_calls"),0),"id"),
            sget(at(compact.get(),i+1),"tool_call_id")));
    }
    json_object_put(defs);
    J *data=json_tokener_parse("{\"stop_reason\":\"max_tokens\",\"content\":[{\"type\":\"tool_use\",\"id\":\"x\",\"name\":\"write_file\",\"input\":{}}]}");
    CHECK(!native_response(&a,data));
    json_object_put(data);
    data=json_tokener_parse("{\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"tool_use\",\"id\":\"x\",\"name\":\"read_file\",\"input\":\"bad\"}]}");
    CHECK(!native_response(&a,data));
    json_object_put(data);
    app_cleanup(&a);
}
static void test_tool_configuration(){
    App a=fresh();a.shell=true;a.desktop=true;
    JsonPtr enabled(schemas(&a));CHECK(alen(enabled.get())==18);
    // Mutating one caller's schema must not change validation or other callers.
    json_object_object_del(get(get(get(at(enabled.get(),0),"function"),"parameters"),"properties"),"path");
    JsonPtr untouched(schemas(&a));
    CHECK(get(get(get(get(at(untouched.get(),0),"function"),"parameters"),"properties"),"path"));
    JsonPtr empty(obj()),result(tool_call(&a,"list_files",empty.get()));CHECK(tool_failed(result.get()));
    a.shell=false;a.desktop=false;
    JsonPtr disabled(schemas(&a));CHECK(alen(disabled.get())==11);
    JsonPtr shell_args(obj());put(shell_args.get(),"command",str("exit 0"));
    result.reset(tool_call(&a,"run_shell",shell_args.get()));CHECK(tool_failed(result.get()));
    JsonPtr desktop_args(obj());put(desktop_args.get(),"app",str(""));
    result.reset(tool_call(&a,"desktop_snapshot",desktop_args.get()));CHECK(tool_failed(result.get()));
    a.shell=true;enabled.reset(schemas(&a));CHECK(alen(enabled.get())==14);
    a.shell=false;enabled.reset(schemas(&a));CHECK(alen(enabled.get())==11);
    a.subagent=true;enabled.reset(schemas(&a));CHECK(alen(enabled.get())==4);
    JsonPtr research_args(obj());put(research_args.get(),"question",str("nested?"));
    result.reset(tool_call(&a,"delegate_research",research_args.get()));CHECK(tool_failed(result.get()));
    a.subagent=false;
    app_cleanup(&a);
}
/* A local HTTP server; no external model or real credential. */ static pid_t fake_server(App *a,const char **responses,int count,int http_status=200,int delay_ms=0,bool keep_alive=false,bool truncate_response=false){
    int fd=socket(AF_INET,SOCK_STREAM,0);
    CHECK(fd>=0);
    struct sockaddr_in address{};
    address.sin_family=AF_INET;
    address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(!bind(fd,(struct sockaddr *)&address,sizeof address));
    socklen_t length=sizeof address;
    CHECK(!getsockname(fd,(struct sockaddr *)&address,&length));
    CHECK(!listen(fd,4));
    char base[128];
    snprintf(base,sizeof base,"http://127.0.0.1:%d/v1",ntohs(address.sin_port));
    put(a->config,"base_url",str(base));
    pid_t child=fork();
    CHECK(child>=0);
    if(!child){
        alarm(12);
        FILE *stream=nullptr;
        for(int i=0;i<count;i++){
            if(!stream){
                int client=accept(fd,NULL,NULL);
                if(client<0)_exit(2);
                stream=fdopen(client,"r+");
                if(!stream)_exit(2);
            }
            char *line=NULL;
            size_t cap=0;
            long content_length=0;
            char request_path[1024]="",method[16]="";
            J *capture=obj();
            if(getline(&line,&cap,stream)<0)_exit(2);
            sscanf(line,"%15s %1023s",method,request_path);
            put(capture,"path",str(request_path));put(capture,"method",str(method));
            while(getline(&line,&cap,stream)>0&&strcmp(line,"\r\n")){
                if(!strncasecmp(line,"Content-Length:",15))content_length=strtol(line+15,NULL,10);
                if(!strncasecmp(line,"Authorization:",14))put(capture,"authorization",str(line));
                if(!strncasecmp(line,"x-api-key:",10))put(capture,"key",str(line));
                if(!strncasecmp(line,"anthropic-version:",18))put(capture,"version",str(line));
            }
            free(line);
            if(content_length<0||content_length>500000)_exit(2);
            std::string body(static_cast<size_t>(content_length),'\0');
            if(content_length&&fread(body.data(),1,body.size(),stream)!=body.size())_exit(2);
            J *parsed=json_tokener_parse(body.c_str());
            put(capture,"body",parsed);
            char path[PATH_MAX],suffix[64];
            snprintf(suffix,sizeof suffix,"/request-%d.json",i);
            if(path_suffix(path,a->root,suffix))_exit(2);
            const char *data=json_object_to_json_string(capture);
            if(atomic_file(path,data,strlen(data),0600))_exit(2);
            json_object_put(capture);
            if(delay_ms)usleep(static_cast<useconds_t>(delay_ms)*1000);
            bool reuse=keep_alive&&i+1<count;
            const char *content_type=std::string_view(responses[i]).starts_with("data:")?"text/event-stream":"application/json";
            fprintf(stream,"HTTP/1.1 %d Test\r\nContent-Type: %s\r\nContent-Length: %zu\r\nConnection: %s\r\n\r\n%s",
                    http_status,content_type,strlen(responses[i])+(truncate_response?100:0),reuse?"keep-alive":"close",responses[i]);
            fflush(stream);
            if(!reuse){fclose(stream);stream=nullptr;}
        }
        close(fd);
        _exit(0);
    }
    close(fd);
    return child;
}
static J *capture(App *a,int i){
    char path[PATH_MAX],suffix[64];
    snprintf(suffix,sizeof suffix,"/request-%d.json",i);
    CHECK(!path_suffix(path,a->root,suffix));
    char *text=read_bytes(path,500000,NULL);
    CHECK(text);
    J *j=json_tokener_parse(text);
    free(text);
    CHECK(j);
    return j;
}
static void server_done(pid_t child){
    int status=0;
    pid_t ended=0;
    double until=monotime()+15;
    while(monotime()<until&&(ended=waitpid(child,&status,WNOHANG))==0)usleep(5000);
    if(ended!=child){
        fprintf(stderr,"server_done timeout waiting for fake_server pid %ld\n",static_cast<long>(child));
        kill(child,SIGKILL);
        waitpid(child,&status,0);
    }
    CHECK(ended==child&&WIFEXITED(status)&&WEXITSTATUS(status)==0);
}
static void test_http_reuse(){
    App a=fresh();json_object_put(a.config);a.config=config("reuse","openai");
    const char *responses[]={"{\"ok\":true}","{\"ok\":true}","{\"ok\":true}","{\"ok\":true}"};
    // The fixture accepts one TCP connection for all four requests.
    pid_t server=fake_server(&a,responses,4,200,0,true);
    JsonPtr payload(obj());put(payload.get(),"request",str("first"));
    JsonPtr result(api_request(&a,a.config,"/chat/completions",payload.get()));CHECK(bget(result.get(),"ok"));
    put(a.config,"protocol",str("anthropic"));put(a.config,"api_key",str("native-test-key"));
    put(a.config,"anthropic_version",str("2023-06-01"));put(a.config,"max_tokens",json_object_new_int(4096));
    put(payload.get(),"request",str("second"));
    result.reset(api_request(&a,a.config,"/messages",payload.get()));CHECK(bget(result.get(),"ok"));
    result.reset(api_request(&a,a.config,"/models",nullptr));CHECK(bget(result.get(),"ok"));
    put(a.config,"protocol",str("openai"));put(a.config,"api_key",str("changed-test-key"));
    result.reset(api_request(&a,a.config,"/models",nullptr));CHECK(bget(result.get(),"ok"));
    server_done(server);
    for(int i=0;i<4;i++){
        JsonPtr request(capture(&a,i));bool native=i==1||i==2;
        CHECK(!strcmp(sget(request.get(),"method"),i<2?"POST":"GET"));
        CHECK((get(request.get(),"body")!=nullptr)==(i<2));
        CHECK((get(request.get(),"authorization")!=nullptr)==!native);
        CHECK((get(request.get(),"key")!=nullptr)==native);
        CHECK((get(request.get(),"version")!=nullptr)==native);
        if(i==0)CHECK(strstr(sget(request.get(),"authorization"),"fake-test-key"));
        if(native)CHECK(strstr(sget(request.get(),"key"),"native-test-key"));
        if(i==3)CHECK(strstr(sget(request.get(),"authorization"),"changed-test-key"));
        if(i<2)CHECK(!strcmp(sget(get(request.get(),"body"),"request"),i?"second":"first"));
    }
    app_cleanup(&a);
}
static J *model_call(const char *id,const char *name,J *arguments){
    JsonPtr input(arguments);
    J *call=obj(),*fn=obj();
    put(call,"id",str(id));
    put(call,"type",str("function"));
    put(fn,"name",str(name));
    put(fn,"arguments",str(json_object_to_json_string_ext(input.get(),JSON_C_TO_STRING_PLAIN)));
    put(call,"function",fn);
    return call;
}
static std::string model_reply(J *calls,const char *text=""){
    JsonPtr response(obj());
    J *choices=arr(),*choice=obj(),*message=obj();
    put(message,"role",str("assistant"));
    put(message,"content",str(text));
    if(calls)put(message,"tool_calls",calls);
    put(choice,"message",message);
    add(choices,choice);
    put(response.get(),"choices",choices);
    return json_object_to_json_string_ext(response.get(),JSON_C_TO_STRING_PLAIN);
}
static std::string model_tool(const char *id,const char *name,J *arguments){
    J *calls=arr();
    add(calls,model_call(id,name,arguments));
    return model_reply(calls);
}
static J *command(const char *text){
    J *v=obj();
    put(v,"command",str(text));
    return v;
}
static int workflow(App *a,const std::vector<std::string> &replies){
    std::vector<const char *> responses;
    for(const auto &reply:replies)responses.push_back(reply.c_str());
    json_object_put(a->config);
    a->config=config("test","openai");
    pid_t child=fake_server(a,responses.data(),static_cast<int>(responses.size()));
    int result=run_task(a,"完成任务并检查实际结果");
    if(result)fprintf(stderr,"Workflow stopped: %s\n",a->error);
    server_done(child);
    return result;
}
static void test_edit_workflow(void){
    App a=fresh();
    const char *original="欢迎：旧内容\n保留这行\n";
    fixture(&a,"/welcome.txt",original,strlen(original));
    char *hash=sha256(original,strlen(original));
    J *search=args(".");
    put(search,"query",str("旧内容"));
    J *edit=args("welcome.txt");
    put(edit,"old_text",str("旧内容"));
    put(edit,"new_text",str("新的欢迎语 😀"));
    put(edit,"expected_sha256",str(hash));
    free(hash);
    CHECK(!workflow(&a,{
        model_tool("find","search_text",search),
        model_tool("read","read_file",args("welcome.txt")),
        model_tool("edit","edit_file",edit),
        model_tool("check","read_file",args("welcome.txt")),
        model_reply(nullptr,"已修改并读取核对。")
    }));
    JsonPtr request(capture(&a,4));
    J *messages=get(get(request.get(),"body"),"messages");
    const char *result=sget(at(messages,alen(messages)-1),"content");
    CHECK(strstr(result,"新的欢迎语 😀")&&strstr(result,"保留这行"));
    request.reset(capture(&a,3));
    messages=get(get(request.get(),"body"),"messages");
    JsonPtr written(json_tokener_parse(sget(at(messages,alen(messages)-1),"content")));
    CHECK(bget(written.get(),"verified"));
    CHECK(*sget(written.get(),"backup"));
    app_cleanup(&a);
}
static void test_recovery_and_loops(void){
    App a=fresh();
    CHECK(workflow(&a,{
        model_tool("bad","run_shell",command("exit 7")),
        model_tool("unrelated","list_files",args(".")),
        model_reply(nullptr,"False success")
    })!=0);
    CHECK(!strcmp(a.state,"失败"));
    CHECK(!strstr(json_object_to_json_string(a.messages),"False success"));
    app_cleanup(&a);
    a=fresh();
    J *write=args("ready.txt");
    put(write,"content",str("ready"));
    put(write,"expected_sha256",nullptr);
    CHECK(!workflow(&a,{
        model_tool("missing","run_shell",command("test -f ready.txt")),
        model_tool("missing2","run_shell",command("test -r ready.txt")),
        model_tool("missing3","run_shell",command("test -s ready.txt")),
        model_tool("create","write_file",write),
        model_tool("retry","run_shell",command("test -f ready.txt")),
        model_reply(nullptr,"已创建并核对。")
    }));
    CHECK(!strcmp(a.state,"本轮结束"));
    app_cleanup(&a);
    a=fresh();
    fixture(&a,"/stable.txt","stable",6);
    write=args("after-repeat.txt");
    put(write,"content",str("executed"));
    put(write,"expected_sha256",nullptr);
    J *last=arr();
    add(last,model_call("read3","read_file",args("stable.txt")));
    add(last,model_call("read4","read_file",args("stable.txt")));
    add(last,model_call("write-after-repeat","write_file",write));
    CHECK(!workflow(&a,{
        model_tool("read1","read_file",args("stable.txt")),
        model_tool("read2","read_file",args("stable.txt")),
        model_reply(last),
        model_reply(nullptr,"已改用其他操作。")
    }));
    CHECK(strstr(sget(at(a.messages,alen(a.messages)-3),"content"),"cached_result"));
    char path[PATH_MAX];
    CHECK(!path_suffix(path,a.root,"/after-repeat.txt"));
    CHECK(access(path,F_OK)==0);
    app_cleanup(&a);
    a=fresh();
    write=args("must-not-exist.txt");
    put(write,"content",str("not executed"));
    put(write,"expected_sha256",nullptr);
    last=arr();
    add(last,model_call("write","write_file",write));
    J *bad=model_call("malformed","read_file",args("stable.txt"));
    put(get(bad,"function"),"arguments",str("{} trailing garbage"));
    add(last,bad);
    CHECK(workflow(&a,{model_reply(last)})!=0);
    CHECK(strstr(a.error,"JSON 对象"));
    CHECK(!path_suffix(path,a.root,"/must-not-exist.txt"));
    CHECK(access(path,F_OK)!=0&&errno==ENOENT);
    app_cleanup(&a);
    a=fresh();
    fixture(&a,"/existing.txt","keep",4);
    write=args("existing.txt");
    put(write,"content",str("changed"));
    put(write,"expected_sha256",str("stale-hash"));
    CHECK(workflow(&a,{
        model_tool("conflict","write_file",write),
        model_tool("read","read_file",args("existing.txt")),
        model_reply(nullptr,"False success")
    })!=0);
    CHECK(strstr(a.error,"未恢复"));
    CHECK(!path_suffix(path,a.root,"/existing.txt"));
    char *kept=read_bytes(path,FILE_LIMIT,nullptr);
    CHECK(kept&&!strcmp(kept,"keep"));
    free(kept);
    app_cleanup(&a);
    // Exploratory read_file miss on an optional file resolves once a subsequent read succeeds.
    a=fresh();
    fixture(&a,"/present.txt","present",7);
    CHECK(!workflow(&a,{
        model_tool("probe","read_file",args("optional-missing.toml")),
        model_tool("actual","read_file",args("present.txt")),
        model_reply(nullptr,"Found present.txt and answered.")
    }));
    CHECK(!strcmp(a.state,"本轮结束"));
    app_cleanup(&a);
}
static void test_native_error_flags(void){
    App a=fresh();
    J *m=obj();
    put(m,"role",str("tool"));
    put(m,"tool_call_id",str("nonzero"));
    put(m,"content",str("{\"exit_code\":7,\"output\":\"failed\"}"));
    add(a.messages,m);
    m=obj();
    put(m,"role",str("tool"));
    put(m,"tool_call_id",str("timeout"));
    put(m,"content",str("{\"exit_code\":0,\"timed_out\":true}"));
    add(a.messages,m);
    JsonPtr body(native_payload(a.config,a.messages,nullptr));
    J *messages=get(body.get(),"messages"),*content=get(at(messages,alen(messages)-1),"content");
    CHECK(alen(content)==2);
    CHECK(bget(at(content,0),"is_error"));
    CHECK(bget(at(content,1),"is_error"));
    app_cleanup(&a);
}
static void test_openai_loop(void){
    App a=fresh();
    json_object_put(a.config);
    a.config=config("test","openai");
    const char *responses[]={
        "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"write\",\"type\":\"function\",\"function\":{\"name\":\"write_file\",\"arguments\":\"{\\\"path\\\":\\\"hello.txt\\\",\\\"content\\\":\\\"hello C\\\",\\\"expected_sha256\\\":null}\"}}]}}]}", "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"tool_calls\":[{\"id\":\"read\",\"type\":\"function\",\"function\":{\"name\":\"read_file\",\"arguments\":\"{\\\"path\\\":\\\"hello.txt\\\"}\"}}]}}]}", "{\"choices\":[{\"message\":{\"role\":\"assistant\",\"content\":\"Created and checked.\"}}]}"
    };
    pid_t child=fake_server(&a,responses,3);
    int result=run_task(&a,"create and verify");
    server_done(child);
    CHECK(result==0);
    CHECK(!strcmp(a.state,"本轮结束"));
    J *request=capture(&a,2);
    CHECK(!strcmp(sget(request,"path"),"/v1/chat/completions"));
    CHECK(get(request,"authorization"));
    J *messages=get(get(request,"body"),"messages"),*last=at(messages,alen(messages)-1);
    CHECK(!strcmp(sget(last,"role"),"tool"));
    CHECK(strstr(sget(last,"content"),"hello C"));
    json_object_put(request);
    app_cleanup(&a);
}
static void test_anthropic_loop(void){
    App a=fresh();
    json_object_put(a.config);
    a.config=config("claude","anthropic");
    const char *responses[]={
        "{\"usage\":{\"input_tokens\":5,\"cache_read_input_tokens\":7,\"cache_creation_input_tokens\":11,\"output_tokens\":3},\"role\":\"assistant\",\"stop_reason\":\"tool_use\",\"content\":[{\"type\":\"thinking\",\"thinking\":\"private\",\"signature\":\"signed-value\"},{\"type\":\"tool_use\",\"id\":\"write\",\"name\":\"write_file\",\"input\":{\"path\":\"hello.txt\",\"content\":\"native C\",\"expected_sha256\":null}},{\"type\":\"tool_use\",\"id\":\"read\",\"name\":\"read_file\",\"input\":{\"path\":\"hello.txt\"}}]}", "{\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"Native done.\"}]}"
    };
    pid_t child=fake_server(&a,responses,2);
    int result=run_task(&a,"native create and read");
    server_done(child);
    CHECK(result==0);
    JsonPtr usage(load_tokens(a));CHECK(model_tokens(usage.get(),a.config)==26);
    J *r=capture(&a,1);
    CHECK(!get(r,"authorization"));
    CHECK(get(r,"key"));
    CHECK(get(r,"version"));
    CHECK(!strcmp(sget(r,"path"),"/v1/messages"));
    J *body=get(r,"body"),*messages=get(body,"messages");
    CHECK(!get(body,"system"));
    J *previous=at(messages,alen(messages)-2);
    CHECK(!strcmp(sget(at(get(previous,"content"),0),"signature"),"signed-value"));
    J *last=at(messages,alen(messages)-1);
    CHECK(!strcmp(sget(last,"role"),"user"));
    CHECK(alen(get(last,"content"))==2);
    CHECK(!strcmp(sget(at(get(last,"content"),0),"type"),"tool_result"));
    json_object_put(r);
    app_cleanup(&a);
}
static void test_failed_task(void){
    App a=fresh();
    json_object_put(a.config);
    a.config=config("test","openai");
    const char *responses[]={
        "{\"choices\":[{\"message\":{\"tool_calls\":[{\"id\":\"bad\",\"function\":{\"name\":\"run_shell\",\"arguments\":\"{\\\"command\\\":\\\"exit 7\\\"}\"}}]}}]}", "{\"choices\":[{\"message\":{\"content\":\"False success\"}}]}"
    };
    pid_t child=fake_server(&a,responses,2);
    int result=run_task(&a,"bad command");
    server_done(child);
    CHECK(result!=0);
    CHECK(!strcmp(a.state,"失败"));
    CHECK(!strstr(json_object_to_json_string(a.messages),"False success"));
    app_cleanup(&a);
}
static void test_menu(void){
    App a=fresh();
    int master;
    pid_t child=forkpty(&master,NULL,NULL,NULL);
    CHECK(child>=0);
    if(!child){
        execl("./build/desk","desk","--workspace",a.root,(char *)NULL);
        _exit(127);
    }
    const char *keys="3\n0\n";
    CHECK(write(master,keys,strlen(keys))==(ssize_t)strlen(keys));
    char output[16384];
    size_t used=0;bool eof_sent=false;
    double until=monotime()+5;
    while(used+1<sizeof output&&monotime()<until){
        struct pollfd p={
            master,POLLIN,0
        };
        int ready=poll(&p,1,100);
        if(ready<0)break;
        if(ready>0){
            ssize_t n=read(master,output+used,sizeof output-used-1);
            if(n<=0)break;
            used+=(size_t)n;
            std::string text(output,used);auto first=text.find("请选择编号：");
            if(!eof_sent&&first!=std::string::npos&&text.find("请选择编号：",first+1)!=std::string::npos){CHECK(write(master,"\x04",1)==1);eof_sent=true;}
        }
    }
    output[used]=0;
    close(master);
    int status;
    CHECK(waitpid(child,&status,0)==child);
    CHECK(WIFEXITED(status)&&!WEXITSTATUS(status));
    CHECK(strstr(output,"[1] 开始对话"));
    CHECK(strstr(output,"[1] OpenAI接口"));
    CHECK(strstr(output,"[2] Anthropic接口"));
    CHECK(!strstr(output,"API Key："));
    app_cleanup(&a);
}
static std::string task_state(Tasks &jobs,int id){
    JsonPtr list(jobs.snapshot());
    for(size_t i=0;i<alen(list.get());i++)if(iget(at(list.get(),i),"id",0)==id)return sget(at(list.get(),i),"state");
    return "missing";
}
static bool await_state(Tasks &jobs,int id,const std::string &state,double seconds=6){
    double until=monotime()+seconds;
    while(monotime()<until){if(task_state(jobs,id)==state)return true;usleep(10000);}
    return false;
}
/* The server replies only AFTER both requests arrive: serial execution deadlocks. */
static pid_t parallel_server(App &a){
    int fd=socket(AF_INET,SOCK_STREAM,0);CHECK(fd>=0);
    sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
    CHECK(!bind(fd,reinterpret_cast<sockaddr *>(&address),sizeof address));
    socklen_t length=sizeof address;CHECK(!getsockname(fd,reinterpret_cast<sockaddr *>(&address),&length));CHECK(!listen(fd,4));
    put(a.config,"base_url",str(("http://127.0.0.1:"+std::to_string(ntohs(address.sin_port))+"/v1").c_str()));
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        alarm(10);FILE *streams[2]{};std::string bodies[2];
        for(int i=0;i<2;i++){
            int client=accept(fd,nullptr,nullptr);if(client<0)_exit(2);
            streams[i]=fdopen(client,"r+");if(!streams[i])_exit(2);
            char *line=nullptr;size_t cap=0;long size=0;
            while(getline(&line,&cap,streams[i])>0&&strcmp(line,"\r\n"))if(!strncasecmp(line,"Content-Length:",15))size=strtol(line+15,nullptr,10);
            free(line);if(size<0||size>500000)_exit(2);
            bodies[i].resize(static_cast<size_t>(size));if(fread(bodies[i].data(),1,bodies[i].size(),streams[i])!=bodies[i].size())_exit(2);
        }
        if(bodies[0].find("alpha")!=std::string::npos&&bodies[0].find("beta")!=std::string::npos)_exit(3);
        if(bodies[1].find("alpha")!=std::string::npos&&bodies[1].find("beta")!=std::string::npos)_exit(3);
        std::string reply=model_reply(nullptr,"独立任务完成");
        for(FILE *stream:streams){fprintf(stream,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",reply.size(),reply.c_str());fclose(stream);}
        int client=accept(fd,nullptr,nullptr);if(client<0)_exit(2);
        FILE *stream=fdopen(client,"r+");if(!stream)_exit(2);
        char *line=nullptr;size_t cap=0;long size=0;
        while(getline(&line,&cap,stream)>0&&strcmp(line,"\r\n"))if(!strncasecmp(line,"Content-Length:",15))size=strtol(line+15,nullptr,10);
        free(line);if(size<0||size>500000)_exit(2);
        std::string continued(static_cast<size_t>(size),'\0');if(fread(continued.data(),1,continued.size(),stream)!=continued.size())_exit(2);
        if(continued.find("alpha")==std::string::npos||continued.find("followup")==std::string::npos||continued.find("beta")!=std::string::npos)_exit(3);
        fprintf(stream,"HTTP/1.1 200 OK\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",reply.size(),reply.c_str());fclose(stream);
        close(fd);_exit(0);
    }
    close(fd);return child;
}
static void test_parallel_tasks(void){
    App a=fresh();json_object_put(a.config);a.config=config("parallel","openai");
    pid_t server=parallel_server(a);
    {
        Tasks jobs("./build/desk");
        int first=jobs.start(a,"alpha 独立任务"),second=jobs.start(a,"beta 独立任务");
        CHECK(first==1&&second==2);
        CHECK(await_state(jobs,first,"已完成"));CHECK(await_state(jobs,second,"已完成"));
        CHECK(!jobs.cancel(first));CHECK(alen(a.messages)==0);
        CHECK(jobs.resume(first,"followup 继续原任务"));CHECK(!jobs.resume(first,"任务仍在执行"));
        CHECK(await_state(jobs,first,"已完成"));
    }
    server_done(server);app_cleanup(&a);
    a=fresh();json_object_put(a.config);a.config=config("approval","openai");a.yes=false;
    J *write=args("pending.txt");put(write,"content",str("approved only"));put(write,"expected_sha256",nullptr);
    std::string response=model_tool("write","write_file",write);const char *responses[]={response.c_str()};
    server=fake_server(&a,responses,1);
    {
        Tasks jobs("./build/desk");int id=jobs.start(a,"等待确认，禁止自动写入");CHECK(id==1);
        CHECK(await_state(jobs,id,"等待确认"));
        char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/pending.txt"));CHECK(access(path,F_OK)!=0);
        CHECK(jobs.cancel(id));CHECK(await_state(jobs,id,"已取消"));CHECK(access(path,F_OK)!=0);
    }
    server_done(server);app_cleanup(&a);
    a=fresh();json_object_put(a.config);a.config=config("cancel","openai");
    response=model_tool("run","run_shell",command("echo $$ > child.pid; sleep 30"));responses[0]=response.c_str();
    server=fake_server(&a,responses,1);
    {
        Tasks jobs("./build/desk");int id=jobs.start(a,"长命令取消测试");CHECK(id==1);
        char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/child.pid"));
        double until=monotime()+5;while(access(path,F_OK)&&monotime()<until){jobs.refresh();usleep(10000);}
        char *pid_text=read_bytes(path,100,nullptr);CHECK(pid_text);pid_t pid=static_cast<pid_t>(strtol(pid_text,nullptr,10));free(pid_text);
        CHECK(jobs.cancel(id));CHECK(await_state(jobs,id,"已取消"));
        until=monotime()+2;while(alive(pid)&&monotime()<until)usleep(10000);CHECK(!alive(pid));
    }
    server_done(server);app_cleanup(&a);
}
static void test_removed_tasks(){
    App a=fresh();json_object_put(a.config);a.config=config("removed-tasks","openai");
    const auto done=model_reply(nullptr,"done");
    const auto running=model_tool("run","run_shell",command("echo $$ > removed.pid; sleep 30"));
    std::vector<const char *> responses(34,done.c_str());responses.push_back(running.c_str());
    pid_t server=fake_server(&a,responses.data(),static_cast<int>(responses.size()));
#ifdef __APPLE__
    auto descriptors=[](){return static_cast<size_t>(std::distance(fs::directory_iterator("/dev/fd"),fs::directory_iterator{}));};
#else
    auto descriptors=[](){return static_cast<size_t>(std::distance(fs::directory_iterator("/proc/self/fd"),fs::directory_iterator{}));};
#endif
    {
        Tasks jobs("./build/desk");const auto before=descriptors();
        for(int i=0;i<34;i++){
            int id=jobs.start(a,"temporary conversation");CHECK(id>0);CHECK(await_state(jobs,id,"已完成"));
            CHECK(jobs.remove(id));CHECK(!jobs.remove(id));CHECK(!jobs.resume(id,"deleted context"));
            JsonPtr state(jobs.snapshot());CHECK(alen(state.get())==0);
            double until=monotime()+2;while(descriptors()>before&&monotime()<until){jobs.refresh();usleep(10000);}
            CHECK(descriptors()==before);
        }
        int id=jobs.start(a,"remove a running conversation");CHECK(id>0);
        auto path=std::string(a.root)+"/removed.pid";double until=monotime()+5;
        while(access(path.c_str(),F_OK)&&monotime()<until){jobs.refresh();usleep(10000);}
        char *pid_text=read_bytes(path.c_str(),100,nullptr);CHECK(pid_text);
        pid_t child=static_cast<pid_t>(strtol(pid_text,nullptr,10));free(pid_text);
        CHECK(child>0);CHECK(jobs.remove(id));
        JsonPtr state(jobs.snapshot());CHECK(alen(state.get())==0);
        until=monotime()+3;while((alive(child)||descriptors()>before)&&monotime()<until){jobs.refresh();usleep(10000);}
        CHECK(!alive(child));CHECK(descriptors()==before);
    }
    server_done(server);app_cleanup(&a);
}
static void test_parallel_resources(void){
    App a=fresh();json_object_put(a.config);a.config=config("locks","openai");
    char other[PATH_MAX];CHECK(!path_suffix(other,a.root,"/other"));CHECK(!mkdirs(other,0700));
    J *write=args("result.txt");put(write,"content",str("independent"));put(write,"expected_sha256",nullptr);
    std::string action=model_tool("write","write_file",write),done=model_reply(nullptr,"已核对");
    const char *responses[]={action.c_str(),action.c_str(),done.c_str()};pid_t server=fake_server(&a,responses,3);
    {
        ResourceLock held(a,false);CHECK(held.acquired());
        double start=monotime();{ResourceLock blocked(a,false);CHECK(!blocked.acquired());}CHECK(monotime()-start<1);
        Tasks jobs("./build/desk");int first=jobs.start(a,"等待同目录锁");CHECK(first==1);
        CHECK(await_state(jobs,first,"等待资源"));
        int second=jobs.start(a,"在另一个目录工作",other);CHECK(second==2);
        CHECK(await_state(jobs,second,"已完成"));CHECK(task_state(jobs,first)=="等待资源");
        CHECK(jobs.cancel(first));CHECK(await_state(jobs,first,"已取消"));
        char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/result.txt"));CHECK(access(path,F_OK)!=0);
        CHECK(!path_suffix(path,other,"/result.txt"));char *text=read_bytes(path,100,nullptr);CHECK(text&&!strcmp(text,"independent"));free(text);
    }
    server_done(server);app_cleanup(&a);
}
static std::string terminal_text(const std::string &bytes){
    std::string text;
    for(size_t i=0;i<bytes.size();){
        if(bytes[i]=='\x1b'&&i+1<bytes.size()&&bytes[i+1]=='['){i+=2;while(i<bytes.size()&&(static_cast<unsigned char>(bytes[i])<0x40||static_cast<unsigned char>(bytes[i])>0x7e))i++;if(i<bytes.size())i++;}
        else text+=bytes[i++];
    }
    return text;
}
static bool terminal_until(int fd,std::string &output,const char *needle){
    double until=monotime()+8;
    while(monotime()<until){
        if(output.find(needle)!=std::string::npos||terminal_text(output).find(needle)!=std::string::npos)return true;
        pollfd p{fd,POLLIN,0};if(poll(&p,1,100)<=0)continue;
        char buffer[4096];ssize_t n=read(fd,buffer,sizeof buffer);if(n<=0)break;
        output.append(buffer,static_cast<size_t>(n));
    }
    fprintf(stderr,"Terminal transcript:\n%s\n",output.c_str());return false;
}
static int reap_pty(int &master,pid_t child,std::string *tail=nullptr){
    if(master>=0)fcntl(master,F_SETFL,fcntl(master,F_GETFL)|O_NONBLOCK);
    int status=0;
    pid_t ended=0;
    double until=monotime()+12;
    while(monotime()<until){
        if(master>=0){
            char buf[4096];
            for(;;){
                ssize_t n=read(master,buf,sizeof buf);
                if(n>0){if(tail)tail->append(buf,static_cast<size_t>(n));}
                else{
                    if(n==0||(errno!=EAGAIN&&errno!=EWOULDBLOCK&&errno!=EINTR)){close(master);master=-1;}
                    break;
                }
            }
        }
        ended=waitpid(child,&status,WNOHANG);
        if(ended==child)break;
        usleep(10000);
    }
    if(ended!=child){
        fprintf(stderr,"reap_pty timeout waiting for pid %ld\n",static_cast<long>(child));
        kill(child,SIGKILL);
        waitpid(child,&status,0);
        if(master>=0){close(master);master=-1;}
        return -1;
    }
    if(master>=0){
        char buf[4096];
        while(ssize_t n=read(master,buf,sizeof buf)){
            if(n>0){if(tail)tail->append(buf,static_cast<size_t>(n));}
            else break;
        }
        close(master);
        master=-1;
    }
    CHECK(WIFEXITED(status));
    return WEXITSTATUS(status);
}
static void wait_for_chat_prompt(int master,const std::string &output,const char *after){
    auto offset=output.rfind(after);CHECK(offset!=std::string::npos);
    auto tail=output.substr(offset+strlen(after));
    CHECK(terminal_until(master,tail,"\r\n❯ "));
}
static void leave_chat_and_exit(int master,bool english=true){
    CHECK(write(master,"\x04",1)==1);
    const char *picker=english?"Choose a conversation":"选择对话";
    std::string output;CHECK(terminal_until(master,output,picker));
    output.erase(0,output.find(picker));
    CHECK(terminal_until(master,output,english?"Select a number:":"请选择编号："));
    output.clear();
    CHECK(write(master,"0\n",2)==2);
    CHECK(terminal_until(master,output,english?"[1] Start chat":"[1] 开始对话"));
    output.erase(0,output.find(english?"[1] Start chat":"[1] 开始对话"));
    CHECK(terminal_until(master,output,english?"Select a number:":"请选择编号："));
    CHECK(write(master,"0\n",2)==2);
}
static void test_task_switch_approval(bool allow=true){
    App a=fresh();json_object_put(a.config);a.config=config("switch","openai");
    J *write=args("confirmed.txt");put(write,"content",str("user approved"));put(write,"expected_sha256",nullptr);
    std::string action=model_tool("write","write_file",write),done=model_reply(nullptr,"已核对"),continued=model_reply(nullptr,"继续检查完成");
    const char *responses[]={action.c_str(),done.c_str(),continued.c_str()};pid_t server=fake_server(&a,responses,allow?3:1);
    int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){
        setenv("DESK_BASE_URL",sget(a.config,"base_url"),1);setenv("DESK_MODEL","model-test",1);setenv("DESK_API_KEY","fake-test-key",1);setenv("DESK_API_TYPE","openai",1);
        execl("./build/desk","desk","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
    std::string output;
    const char *keys="1\n1\n创建文件\n";CHECK(::write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));
    CHECK(terminal_until(master,output,"[3] 查看完整操作 [0] 取消："));
    char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/confirmed.txt"));CHECK(access(path,F_OK)!=0);
    output.clear();CHECK(::write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"选择对话"));
    output.clear();CHECK(::write(master,"1\n",2)==2);
    CHECK(terminal_until(master,output,"[3] 查看完整操作 [0] 取消："));
    output.clear();CHECK(::write(master,allow?"1\n":"2\n",2)==2);
    CHECK(terminal_until(master,output,allow?"已核对":"用户拒绝写入"));
    CHECK(terminal_until(master,output,allow?"已完成 · 创建文件":"已取消 · 创建文件"));
    if(allow){
        const char *next="再检查文件\n";CHECK(::write(master,next,strlen(next))==static_cast<ssize_t>(strlen(next)));
        CHECK(terminal_until(master,output,"继续检查完成"));
    }
    CHECK(output.find("任务管理")==std::string::npos);
    CHECK(output.find("fake-test-key")==std::string::npos);
    output.clear();leave_chat_and_exit(master,false);
    CHECK(reap_pty(master,child,&output)==0);
    char *text=read_bytes(path,100,nullptr);CHECK(allow?(text&&!strcmp(text,"user approved")):!text);free(text);
    CHECK(output.find("fake-test-key")==std::string::npos);
    server_done(server);app_cleanup(&a);
}
static void test_language_preferences(void){
    App a=fresh();json_object_put(a.config);a.config=config("unchanged-model","openai");
    std::string before=json_object_to_json_string_ext(a.config,JSON_C_TO_STRING_PLAIN);
    std::string store=json_object_to_json_string_ext(a.store,JSON_C_TO_STRING_PLAIN);
    J *user=obj();put(user,"role",str("user"));put(user,"content",str("Keep my request exactly."));add(a.messages,user);
    CHECK(save_language(a,"en"));CHECK(!strcmp(language_code(),"en"));
    CHECK(!strcmp(tr("等待任务"),"Waiting for a task"));
    CHECK(!strcmp(tr("文件超过 256 KB 限制。"),"The file exceeds the 256 KB limit."));
    CHECK(!strcmp(tr("修改后的文件超过 256 KB 限制。"),"The edited file exceeds the 256 KB limit."));
    CHECK(!strcmp(tr("无法读取文件，文件可能不是普通文件或超过 256 KB。"),"Could not read the file. It may not be a regular file or may exceed 256 KB."));
    CHECK(!strcmp(tr("-p 需要一个需求文本。\n"),"-p requires a prompt.\n"));
    CHECK(before==json_object_to_json_string_ext(a.config,JSON_C_TO_STRING_PLAIN));
    CHECK(store==json_object_to_json_string_ext(a.store,JSON_C_TO_STRING_PLAIN));
    CHECK(alen(a.messages)==1&&!strcmp(sget(at(a.messages,0),"content"),"Keep my request exactly."));
    char path[PATH_MAX];CHECK(!path_suffix(path,a.config_dir,"/ui.json"));
    char *text=read_bytes(path,4096,nullptr);CHECK(text);JsonPtr pref(json_tokener_parse(text));free(text);
    CHECK(json_object_object_length(pref.get())==1);CHECK(!strcmp(sget(pref.get(),"language"),"en"));
    CHECK(!path_suffix(path,a.config_dir,"/settings.json"));CHECK(access(path,F_OK)!=0);
    unsetenv("DESK_LANG");char program[]="desk";char *args[]={program,nullptr};
    CHECK(set_language("zh"));CHECK(language_init(1,args));CHECK(!strcmp(language_code(),"en"));
    CHECK(!setenv("DESK_LANG","zh",1));CHECK(language_init(1,args));CHECK(!strcmp(language_code(),"zh"));
    char flag[]="--lang",english[]="en",help[]="--help";char *override_args[]={program,help,flag,english,nullptr};
    CHECK(language_init(4,override_args));CHECK(!strcmp(language_code(),"en"));
    unsetenv("DESK_LANG");CHECK(save_language(a,"zh"));CHECK(!strcmp(language_code(),"zh"));
    app_cleanup(&a);
}
static void test_english_menu(void){
    App a=fresh();int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    std::string output;CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(terminal_until(master,output,"[3] Add model"));CHECK(output.find("[2] Select model")!=std::string::npos);
    CHECK(write(master,"3\n",2)==2);CHECK(terminal_until(master,output,"[1] OpenAI API"));
    CHECK(terminal_until(master,output,"[2] Anthropic API"));
    const char *switch_zh="0\n5\n1\n1\n0\n";CHECK(write(master,switch_zh,strlen(switch_zh))==static_cast<ssize_t>(strlen(switch_zh)));
    CHECK(terminal_until(master,output,"[1] 开始对话"));
    auto zh_tail=output.substr(output.rfind("[1] 开始对话"));
    CHECK(terminal_until(master,zh_tail,"请选择编号："));
    CHECK(write(master,"\x04",1)==1);
    CHECK(reap_pty(master,child)==0);
    app_cleanup(&a);
}
static void test_no_extra_model_prompts(void){
    App a=fresh();CHECK(set_language("en"));
    CHECK(!workflow(&a,{model_reply(nullptr,"Provider reply, unchanged.")}));
    JsonPtr request(capture(&a,0));J *body=get(request.get(),"body"),*messages=get(body,"messages");
    CHECK(!strcmp(sget(body,"model"),"model-test"));CHECK(alen(messages)==1);
    CHECK(!strcmp(sget(at(messages,0),"role"),"user"));CHECK(!strcmp(sget(at(messages,0),"content"),"完成任务并检查实际结果"));
    CHECK(!get(body,"system"));CHECK(!strcmp(sget(at(a.messages,alen(a.messages)-1),"content"),"Provider reply, unchanged."));
    CHECK(set_language("zh"));app_cleanup(&a);
}
static void test_numeric_request(void){
    App a=fresh();json_object_put(a.config);a.config=config("numeric","openai");
    std::string reply=model_reply(nullptr,"数字需求已收到");std::string zero=model_reply(nullptr,"零也是需求");const char *responses[]={reply.c_str(),zero.c_str()};pid_t server=fake_server(&a,responses,2);
    int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){
        setenv("DESK_BASE_URL",sget(a.config,"base_url"),1);setenv("DESK_MODEL","model-test",1);setenv("DESK_API_KEY","fake-test-key",1);setenv("DESK_API_TYPE","openai",1);
        execl("./build/desk","desk","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
    const char *keys="1\n1\n1\n";CHECK(write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));
    std::string output;CHECK(terminal_until(master,output,"数字需求已收到"));
    CHECK(write(master,"0\n",2)==2);CHECK(terminal_until(master,output,"零也是需求"));
    wait_for_chat_prompt(master,output,"零也是需求");leave_chat_and_exit(master,false);CHECK(reap_pty(master,child)==0);
    server_done(server);JsonPtr request(capture(&a,0));J *messages=get(get(request.get(),"body"),"messages");
    CHECK(alen(messages)==1&&!strcmp(sget(at(messages,0),"role"),"user"));CHECK(!strcmp(sget(at(messages,0),"content"),"1"));
    CHECK(output.find("[y/N]")==std::string::npos&&output.find("/help")==std::string::npos);
    JsonPtr next(capture(&a,1));J *history=get(get(next.get(),"body"),"messages");CHECK(!strcmp(sget(at(history,alen(history)-1),"content"),"0"));
    app_cleanup(&a);
}
static void test_numeric_settings(void){
    App a=fresh();int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    const char *keys="5\n2\n1\n3\n2\n0\n0\n";CHECK(write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));
    std::string output;CHECK(terminal_until(master,output,"Approve file edits (this run)"));
    CHECK(reap_pty(master,child,&output)==0);
    CHECK(output.find("[6] View status")==std::string::npos);CHECK(output.find("[5] Settings")!=std::string::npos);CHECK(output.find("[4] Tasks")==std::string::npos);
    CHECK(output.find("/desktop")==std::string::npos&&output.find("/permissions")==std::string::npos);
    CHECK(output.find("\n  Task:")==std::string::npos&&output.find("\n  Desktop:")==std::string::npos);
    char path[PATH_MAX];CHECK(!path_suffix(path,a.config_dir,"/settings.json"));CHECK(access(path,F_OK)!=0);
    app_cleanup(&a);
}
static void test_settings_redraw(){
    App a=fresh();int master=-1;struct winsize window{40,110,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){setenv("TERM","xterm-256color",1);setenv("NO_COLOR","1",1);execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    std::string output;CHECK(terminal_until(master,output,"Select a number:"));
    const char *keys=" 5 \n2\n1\n";CHECK(write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));
    output.clear();CHECK(terminal_until(master,output,"Desktop tools enabled"));
    CHECK(output.rfind("\033[1;1H\033[J")!=std::string::npos);output.erase(0,output.rfind("\033[1;1H\033[J"));
    CHECK(terminal_until(master,output,"Select a number:"));auto screen=output;
    CHECK(screen.find("[1] Interface language")!=std::string::npos);
    CHECK(screen.find("Reasoning mode")==std::string::npos);
    CHECK(screen.find("Change model ID")==std::string::npos);
    CHECK(screen.find("Desktop tools enabled")!=std::string::npos);
    CHECK(screen.find("[1] Enable")==std::string::npos);
    // A failed connection test remains visible on the settings page.
    output.clear();CHECK(write(master,"5\n",2)==2);
    CHECK(terminal_until(master,output,"Add a model and enter an API key first."));CHECK(terminal_until(master,output,"Select a number:"));
    CHECK(output.rfind("\033[1;1H\033[J")!=std::string::npos);
    screen=output.substr(output.rfind("\033[1;1H\033[J"));
    CHECK(screen.find("Add a model and enter an API key first.")!=std::string::npos);CHECK(screen.find("[1] Interface language")!=std::string::npos);
    // Saving a language preference should produce one visible notice, not a duplicate.
    output.clear();CHECK(write(master,"1\n2\n",4)==4);
    CHECK(terminal_until(master,output,"Language changed to English."));CHECK(terminal_until(master,output,"Select a number:"));
    size_t first_notice=output.find("Language changed to English."),last_notice=output.rfind("Language changed to English.");
    CHECK(first_notice==last_notice);
    // EOF from a settings submenu must terminate without redrawing forever.
    output.clear();CHECK(write(master,"2\n",2)==2);CHECK(terminal_until(master,output,"[1] Enable"));
    // Wait for the input prompt before sending the EOF key.
    CHECK(terminal_until(master,output,"Select a number:"));
    CHECK(write(master,"\x04",1)==1);
    CHECK(reap_pty(master,child)==0);app_cleanup(&a);
}
static void test_approval_modes(void){
    App a=fresh();a.yes=false;
    CHECK(approval_required(a,Operation::FileEdit));CHECK(approval_required(a));
    a.auto_edit=true;CHECK(!approval_required(a,Operation::FileEdit));CHECK(approval_required(a));
    JsonPtr write(args("automatic.txt"));put(write.get(),"content",str("first"));put(write.get(),"expected_sha256",nullptr);
    JsonPtr result(tool_call(&a,"write_file",write.get()));CHECK(bget(result.get(),"verified"));
    JsonPtr path(args("automatic.txt"));result.reset(tool_call(&a,"read_file",path.get()));std::string hash=sget(result.get(),"sha256");
    put(path.get(),"old_text",str("first"));put(path.get(),"new_text",str("second"));put(path.get(),"expected_sha256",str(hash.c_str()));
    result.reset(tool_call(&a,"edit_file",path.get()));CHECK(bget(result.get(),"verified"));
    put(write.get(),"path",str("../outside.txt"));result.reset(tool_call(&a,"write_file",write.get()));CHECK(get(result.get(),"error"));
    // A redirected foreground cannot approve commands or desktop actions in assisted mode.
    int saved=dup(STDIN_FILENO),null=open("/dev/null",O_RDONLY);CHECK(saved>=0&&null>=0);CHECK(dup2(null,STDIN_FILENO)>=0);close(null);
    JsonPtr cmd(command("touch forbidden.txt"));result.reset(tool_call(&a,"run_shell",cmd.get()));CHECK(bget(result.get(),"denied"));
    a.desktop=true;JsonPtr desktop(obj());put(desktop.get(),"ref",str("invalid"));put(desktop.get(),"action",str("invoke"));result.reset(tool_call(&a,"desktop_action",desktop.get()));CHECK(bget(result.get(),"denied"));
    CHECK(dup2(saved,STDIN_FILENO)>=0);close(saved);
    a.yes=true;CHECK(!approval_required(a));CHECK(!approval_required(a,Operation::FileEdit));
    CHECK(set_language("en"));CHECK(!strcmp(permission_name(a),"Full access"));a.yes=false;CHECK(!strcmp(permission_name(a),"Approve file edits"));a.auto_edit=false;CHECK(!strcmp(permission_name(a),"Ask for approval"));CHECK(set_language("zh"));
    app_cleanup(&a);
}
// The API layer must stream text as it arrives and still rebuild a normal
// response, for both protocols.
static void test_streaming(void){
    for(int native=0;native<2;native++){
        App a=fresh();json_object_put(a.config);a.config=config(native?"stream-native":"stream-openai",native?"anthropic":"openai");
        std::string reply;
        if(native){
            reply="data: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":3,\"output_tokens\":1}}}\n\n"
                  "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
                  "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"流式\"}}\n\n"
                  "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"回复\"}}\n\n"
                  "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"end_turn\"},\"usage\":{\"output_tokens\":4}}\n\n"
                  "data: {\"type\":\"message_stop\"}\n\n";
        }else{
            reply="data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"流式\"}}]}\n\n"
                  "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"回复\"}}]}\n\n"
                  "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"total_tokens\":7}}\n\n"
                  "data: [DONE]\n\n";
        }
        const char *responses[]={reply.c_str()};
        pid_t server=fake_server(&a,responses,1);
        std::string seen;
        a.stream_text=[&seen](const std::string &part){seen+=part;};
        JsonPtr result(api_complete(&a,a.config,a.messages,nullptr));
        a.stream_text=nullptr;
        CHECK(result);
        CHECK(!strcmp(sget(result.get(),"content"),"流式回复"));
        CHECK(seen=="流式回复"); // delivered incrementally, not after the fact
        JsonPtr request(capture(&a,0));
        CHECK(json_object_get_boolean(get(get(request.get(),"body"),"stream")));
        server_done(server);
        app_cleanup(&a);
    }
}
static void test_streamed_tools(void){
    for(bool native:{false,true}){
        for(int scenario=0;scenario<3;scenario++){
            bool disconnect=scenario==1,complete=scenario==2;
            App a=fresh();
            json_object_put(a.config);
            a.config=config("incomplete-stream",native?"anthropic":"openai");
            std::string reply,terminal;
            if(native){
                reply=R"(data: {"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"write","name":"write_file","input":{}}})" "\n\n"
                      R"(data: {"type":"content_block_delta","index":0,"delta":{"type":"input_json_delta","partial_json":"{\"path\":\"stream-side-effect.txt\",\"content\":\"created\",\"expected_sha256\":null}"}})" "\n\n"
                      R"(data: {"type":"content_block_stop","index":0})" "\n\n"
                      R"(data: {"type":"message_delta","delta":{"stop_reason":"tool_use"}})" "\n\n";
                terminal=R"(data: {"type":"message_stop"})" "\n\n";
            }else{
                reply=R"(data: {"choices":[{"delta":{"tool_calls":[{"index":0,"id":"write","function":{"name":"write_file","arguments":"{\"path\":\"stream-side-effect.txt\",\"content\":\"created\",\"expected_sha256\":null}"}}]}}]})" "\n\n"
                      R"(data: {"choices":[{"delta":{},"finish_reason":"tool_calls"}]})" "\n\n";
                terminal="data: [DONE]\n\n";
            }
            // A clean EOF without a terminal event is invalid, as is an HTTP
            // transfer failure even if the terminal event already arrived.
            if(disconnect||complete)reply+=terminal;
            const char *done=native?R"({"stop_reason":"end_turn","content":[{"type":"text","text":"Created."}]})"
                                   :R"({"choices":[{"message":{"role":"assistant","content":"Created."}}]})";
            const char *responses[]={reply.c_str(),done};
            pid_t server=fake_server(&a,responses,complete?2:1,200,0,false,disconnect);
            CHECK((run_task(&a,"create a file")==0)==complete);
            char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/stream-side-effect.txt"));
            CHECK((access(path,F_OK)==0)==complete);
            server_done(server);
            app_cleanup(&a);
        }
    }
}
static void test_reasoning_modes(void){
    for(bool native:{false,true}){
        App a=fresh();json_object_put(a.config);a.config=config("reasoning",native?"anthropic":"openai");
        J *user=obj();put(user,"role",str("user"));put(user,"content",str("Keep this exact request."));add(a.messages,user);
        const char *openai[]={"none","minimal","low","medium","high","xhigh"};
        const char *anthropic[]={"none","low","medium","high","xhigh","max"};const char **levels=native?anthropic:openai;
        std::string reply=native?"{\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"unchanged\"}]}":model_reply(nullptr,"unchanged");
        std::vector<const char *> responses(8,reply.c_str());pid_t server=fake_server(&a,responses.data(),static_cast<int>(responses.size()));
        std::string before=json_object_to_json_string_ext(a.messages,JSON_C_TO_STRING_PLAIN),model=sget(a.config,"model");int tokens=iget(a.config,"max_tokens",4096);
        for(int i=0;i<8;i++){
            JsonPtr candidate(copy(a.config));if(i==0||i==7)json_object_object_del(candidate.get(),"reasoning_effort");else put(candidate.get(),"reasoning_effort",str(levels[i-1]));
            CHECK(!save_config(&a,candidate.get(),false));CHECK(before==json_object_to_json_string_ext(a.messages,JSON_C_TO_STRING_PLAIN));CHECK(model==sget(a.config,"model"));CHECK(tokens==iget(a.config,"max_tokens",4096));
            App reload;CHECK(!store_load(&reload));CHECK(!strcmp(sget(reload.config,"reasoning_effort"),sget(a.config,"reasoning_effort")));app_cleanup(&reload);
            JsonPtr result(api_complete(&a,a.config,a.messages,nullptr));CHECK(result);CHECK(!strcmp(sget(result.get(),"content"),"unchanged"));
            JsonPtr request(capture(&a,i));J *body=get(request.get(),"body");CHECK(!get(body,"system"));CHECK(model==sget(body,"model"));CHECK(alen(get(body,"messages"))==1);
            if(i==0||i==7){CHECK(!get(body,"reasoning_effort"));CHECK(!get(body,"thinking"));CHECK(!get(body,"output_config"));}
            else if(!native)CHECK(!strcmp(sget(body,"reasoning_effort"),levels[i-1]));
            else if(i==1){CHECK(!strcmp(sget(get(body,"thinking"),"type"),"disabled"));CHECK(!get(body,"output_config"));}
            else{CHECK(!strcmp(sget(get(body,"thinking"),"type"),"adaptive"));CHECK(!strcmp(sget(get(body,"output_config"),"effort"),levels[i-1]));CHECK(iget(body,"max_tokens",0)==tokens);}
            // OpenAI streams report usage only when asked for it.
            if(!native)CHECK(bget(get(body,"stream_options"),"include_usage"));
            else CHECK(!get(body,"stream_options"));
        }
        server_done(server);
        put(a.config,"reasoning_effort",str("xhigh"));const char *rejected[]={"{\"error\":{\"message\":\"unsupported effort\"}}"};server=fake_server(&a,rejected,1,400);
        JsonPtr failure(api_complete(&a,a.config,a.messages,nullptr));CHECK(!failure);CHECK(strstr(a.error,"400"));CHECK(strstr(a.error,"推理"));CHECK(!strcmp(sget(a.config,"reasoning_effort"),"xhigh"));CHECK(model==sget(a.config,"model"));CHECK(before==json_object_to_json_string_ext(a.messages,JSON_C_TO_STRING_PLAIN));server_done(server);
        put(a.config,"reasoning_effort",str("invalid"));CHECK(config_valid(&a,a.config));put(a.config,"reasoning_effort",json_object_new_int(5));CHECK(config_valid(&a,a.config));
        put(a.config,"reasoning_effort",str("minimal"));CHECK(native?config_valid(&a,a.config)!=0:config_valid(&a,a.config)==0);
        app_cleanup(&a);
    }
}
static void test_assisted_worker(void){
    App a=fresh();json_object_put(a.config);a.config=config("assisted","openai");a.yes=false;a.auto_edit=true;put(a.config,"reasoning_effort",str("high"));
    J *write=args("assisted.txt");put(write,"content",str("auto-approved"));put(write,"expected_sha256",nullptr);
    std::string first=model_tool("write","write_file",write),second=model_tool("run","run_shell",command("touch command-must-wait.txt"));first.insert(first.size()-1,",\"usage\":{\"total_tokens\":17}");
    second.insert(second.size()-1,",\"usage\":{\"prompt_tokens\":19,\"completion_tokens\":4}");
    const char *responses[]={first.c_str(),second.c_str()};pid_t server=fake_server(&a,responses,2);
    {
        Tasks jobs("./build/desk");int id=jobs.start(a,"edit then ask for command approval");CHECK(id==1);
        a.auto_edit=false;a.yes=true;put(a.config,"reasoning_effort",str("low"));
        CHECK(await_state(jobs,id,"等待确认"));JsonPtr usage(load_tokens(a));CHECK(model_tokens(usage.get(),a.config)==40);char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/assisted.txt"));char *content=read_bytes(path,100,nullptr);CHECK(content&&!strcmp(content,"auto-approved"));free(content);
        CHECK(!path_suffix(path,a.root,"/command-must-wait.txt"));CHECK(access(path,F_OK)!=0);
        JsonPtr request(capture(&a,1));CHECK(!strcmp(sget(get(request.get(),"body"),"reasoning_effort"),"high"));
        CHECK(jobs.cancel(id));CHECK(await_state(jobs,id,"已取消"));CHECK(access(path,F_OK)!=0);
    }
    server_done(server);app_cleanup(&a);
}
static void type_terminal(int fd,std::string &output,const std::string &text);
static void test_chat_mode_menu(void){
    App a=fresh();
    json_object_put(a.config);
    a.config=config("modes","openai");
    put(a.config,"reasoning_effort",str("low"));
    CHECK(!save_config(&a,a.config));
    int master=-1;
    auto launch=[&]{
        pid_t child=forkpty(&master,nullptr,nullptr,nullptr);
        CHECK(child>=0);
        if(!child){
            execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));
            _exit(127);
        }
        return child;
    };
    auto finish=[&](pid_t child){
        leave_chat_and_exit(master);
        CHECK(reap_pty(master,child)==0);
    };
    std::string output;
    auto picker=[&]{
        output.clear();
        CHECK(write(master,"\x04",1)==1);
        CHECK(terminal_until(master,output,"Choose a conversation"));
        CHECK(terminal_until(master,output,"Select a number:"));
    };

    pid_t child=launch();
    type_terminal(master,output,"1\n1\n/\n2\n5\n/\n3\n2\n");
    CHECK(terminal_until(master,output,"Reasoning: High · Approval: Approve file edits"));
    CHECK(output.find("[6] Extra high (xhigh)")!=std::string::npos);
    CHECK(output.find("[3] Full access")!=std::string::npos);
    CHECK(output.find("[1] Enter a request")==std::string::npos);
    wait_for_chat_prompt(master,output,"Reasoning: High · Approval: Approve file edits");
    picker();
    output.clear();
    type_terminal(master,output,"2\n");
    CHECK(terminal_until(master,output,"Reasoning: Low · Approval: Ask for approval"));
    type_terminal(master,output,"/\n2\n7\n/\n3\n3\n");
    const char *second="Reasoning: Provider default (unspecified) · Approval: Full access";
    CHECK(terminal_until(master,output,second));
    wait_for_chat_prompt(master,output,second);
    finish(child);

    JsonPtr entries(history_list(a));
    CHECK(alen(entries.get())==2);
    CHECK(!strcmp(sget(at(entries.get(),0),"approval_mode"),"full"));
    CHECK(!strcmp(sget(at(entries.get(),1),"reasoning_effort"),"high"));
    CHECK(!get(at(entries.get(),0),"api_key"));
    App reloaded;
    CHECK(!store_load(&reloaded));
    CHECK(!strcmp(sget(reloaded.config,"reasoning_effort"),"low"));
    app_cleanup(&reloaded);

    child=launch();
    output.clear();
    type_terminal(master,output,"1\n1\n");
    CHECK(terminal_until(master,output,second));
    wait_for_chat_prompt(master,output,second);
    picker();
    output.clear();
    type_terminal(master,output,"2\n");
    const char *first="Reasoning: High · Approval: Approve file edits";
    CHECK(terminal_until(master,output,first));
    wait_for_chat_prompt(master,output,first);
    finish(child);
    app_cleanup(&a);
}
static void test_chat_exit_paths(void){
    for(int route=0;route<5;route++){
        App a=fresh();json_object_put(a.config);a.config=config("exit","openai");put(a.config,"base_url",str("http://127.0.0.1:1/v1"));CHECK(!save_config(&a,a.config));
        int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
        if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
        std::string output;
        if(route==4){CHECK(terminal_until(master,output,"[0] Exit"));}
        else{
            CHECK(write(master,"1\n1\n",4)==4);CHECK(terminal_until(master,output,"Approval: Ask for approval\r\n❯ "));output.clear();
            if(route==1){CHECK(write(master,"\n",1)==1);CHECK(terminal_until(master,output,"❯ "));CHECK(output.find("Approval:")==std::string::npos);}
            else if(route==2){CHECK(kill(child,SIGINT)==0);CHECK(terminal_until(master,output,"❯ "));CHECK(output.find("Approval:")==std::string::npos);}
            else if(route==3){CHECK(write(master,"/\n2\n",4)==4);CHECK(terminal_until(master,output,"[7] Restore provider default"));}
        }
        CHECK(kill(child,0)==0);
        if(route==4){CHECK(terminal_until(master,output,"Select a number:"));CHECK(write(master,"0\n",2)==2);}
        else{if(route==3){output.erase(0,output.find("[7] Restore provider default"));CHECK(terminal_until(master,output,"Select a number:"));}leave_chat_and_exit(master);}
        CHECK(reap_pty(master,child)==0);CHECK(output.find("Thinking...")==std::string::npos);app_cleanup(&a);
    }
}
static void test_compact_output(void){
    CHECK(set_language("zh"));
    for(bool verbose:{false,true}){
        App a=fresh();a.verbose=verbose;
        J *write=args("private-content.txt");put(write,"content",str("hidden-file-contents"));put(write,"expected_sha256",nullptr);
        J *calls=arr();add(calls,model_call("write","write_file",write));
        std::vector<std::string> replies={model_reply(calls,"hidden-intermediate-comment"),model_tool("run","run_shell",command("printf 'hidden-command-output'")),model_reply(nullptr,"Final reply, unchanged.")};
        fflush(stdout);FILE *log=tmpfile();CHECK(log);int saved=dup(STDOUT_FILENO);CHECK(saved>=0);CHECK(dup2(fileno(log),STDOUT_FILENO)>=0);
        int rc=workflow(&a,replies);fflush(stdout);CHECK(dup2(saved,STDOUT_FILENO)>=0);close(saved);
        long size=ftell(log);CHECK(size>=0);rewind(log);std::string output(static_cast<size_t>(size),'\0');CHECK(fread(output.data(),1,output.size(),log)==output.size());fclose(log);CHECK(!rc);
        CHECK(output.find("Final reply, unchanged.")!=std::string::npos);
        CHECK((output.find("hidden-file-contents")!=std::string::npos)==verbose);
        CHECK((output.find("hidden-intermediate-comment")!=std::string::npos)==verbose);
        CHECK((output.find("hidden-command-output")!=std::string::npos)==verbose);
        CHECK((output.find("正在思考…")!=std::string::npos)==!verbose);
        std::string history=json_object_to_json_string_ext(a.messages,JSON_C_TO_STRING_PLAIN);CHECK(history.find("hidden-command-output")!=std::string::npos);CHECK(history.find("hidden-intermediate-comment")!=std::string::npos);
        app_cleanup(&a);
    }

    App a=fresh();
    a.max_steps=0;
    a.conversation_worker=true;
    a.task_events=open("/dev/null",O_WRONLY);
    CHECK(a.task_events>=0);
    std::vector<std::string> replies;
    for(int i=0;i<22;++i){
        const std::string id="command-"+std::to_string(i);
        replies.push_back(model_tool(id.c_str(),"run_shell",command("true")));
    }
    fixture(&a,"/first.txt","first",5);
    fixture(&a,"/second.txt","second",6);
    replies.push_back(model_tool("first","read_file",args("first.txt")));
    replies.push_back(model_tool("second","read_file",args("second.txt")));
    replies.push_back(model_reply(nullptr,"Finished commands and reads."));

    fflush(stdout);
    FILE *log=tmpfile();
    CHECK(log);
    int saved=dup(STDOUT_FILENO);
    CHECK(saved>=0&&dup2(fileno(log),STDOUT_FILENO)>=0);
    const int rc=workflow(&a,replies);
    fflush(stdout);
    CHECK(dup2(saved,STDOUT_FILENO)>=0);
    close(saved);
    const long size=ftell(log);
    CHECK(size>=0);
    rewind(log);
    std::string output(static_cast<size_t>(size),'\0');
    CHECK(fread(output.data(),1,output.size(),log)==output.size());
    fclose(log);
    CHECK(!rc&&output.find("Finished commands and reads.")!=output.npos);
    for(const auto *label:{"正在执行命令…","正在读取文件…"}){
        const auto first=output.find(label);
        CHECK(first!=output.npos&&output.find(label,first+1)==output.npos);
    }
    close(a.task_events);
    a.task_events=-1;
    app_cleanup(&a);
}
static void test_exit_during_work(void){
    for(bool command_running:{false,true}){
        App a=fresh();json_object_put(a.config);a.config=config("exit-active","openai");
        std::string reply=command_running?model_tool("run","run_shell",command("echo $$ > running.pid; sleep 30")):model_reply(nullptr,"must not wait for this reply");const char *responses[]={reply.c_str()};pid_t server=fake_server(&a,responses,1,200,command_running?0:8000);CHECK(!save_config(&a,a.config));
        int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
        if(!child){execl("./build/desk","desk","--lang","en","--yes","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
        const char *keys="1\n1\nwork\n";CHECK(write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));
        std::string output;CHECK(terminal_until(master,output,command_running?"Running a command...":"Thinking..."));
        char path[PATH_MAX];CHECK(!path_suffix(path,a.root,command_running?"/running.pid":"/request-0.json"));double until=monotime()+5;while(access(path,F_OK)&&monotime()<until)usleep(10000);CHECK(access(path,F_OK)==0);
        pid_t process=-1;if(command_running){char *id=read_bytes(path,100,nullptr);CHECK(id);process=static_cast<pid_t>(strtol(id,nullptr,10));free(id);}
        double exit_started=monotime();leave_chat_and_exit(master);CHECK(monotime()-exit_started<4);
        const int exit_code=reap_pty(master,child);
        if(command_running){server_done(server);until=monotime()+2;while(alive(process)&&monotime()<until)usleep(10000);CHECK(!alive(process));}
        else{kill(server,SIGKILL);waitpid(server,nullptr,0);}
        CHECK(exit_code==0);app_cleanup(&a);
    }
}
static void test_compact_approval(void){
    for(bool expand:{false,true}){
        App a=fresh();json_object_put(a.config);a.config=config("approval-preview","openai");
        J *write=args("review.txt");put(write,"content",str("hidden-preview-body"));put(write,"expected_sha256",nullptr);
        std::string reply=model_tool("write","write_file",write);const char *responses[]={reply.c_str()};pid_t server=fake_server(&a,responses,1);CHECK(!save_config(&a,a.config));
        int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
        if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
        const char *keys="1\n1\ncreate a file\n";CHECK(::write(master,keys,strlen(keys))==static_cast<ssize_t>(strlen(keys)));std::string output;
        CHECK(terminal_until(master,output,"[3] Show full operation [0] Cancel: "));CHECK(output.find("hidden-preview-body")==std::string::npos);CHECK(output.find("review.txt")!=std::string::npos);
        if(expand){CHECK(::write(master,"3\n",2)==2);CHECK(terminal_until(master,output,"hidden-preview-body"));CHECK(::write(master,"2\n",2)==2);CHECK(terminal_until(master,output,"Operation cancelled."));wait_for_chat_prompt(master,output,"Operation cancelled.");}
        leave_chat_and_exit(master);
        CHECK(reap_pty(master,child)==0);char path[PATH_MAX];CHECK(!path_suffix(path,a.root,"/review.txt"));CHECK(access(path,F_OK)!=0);server_done(server);app_cleanup(&a);
    }
}
static void type_terminal(int fd,std::string &output,const std::string &text){
    size_t used=0;double until=monotime()+12;
    while(used<text.size()&&monotime()<until){
        struct pollfd ready{fd,POLLIN|POLLOUT,0};CHECK(poll(&ready,1,100)>=0);
        if(ready.revents&POLLIN){char bytes[8192];ssize_t n=read(fd,bytes,sizeof bytes);CHECK(n>0);output.append(bytes,static_cast<size_t>(n));}
        if(ready.revents&POLLOUT){size_t count=std::min(static_cast<size_t>(128),text.size()-used);ssize_t n=write(fd,text.data()+used,count);CHECK(n>0);used+=static_cast<size_t>(n);}
    }
    CHECK(used==text.size());
}
static void test_chat_history_scroll(){
    App a=fresh();
    const char *id="11111111111111111111111111111111";
    CHECK(history_save(a,id,"Scrollable chat",a.config));
    a.conversation_id=id;
    std::string body="OLDEST_SCROLL_LINE\n";
    for(int i=0;i<55;i++)body+="History line "+std::to_string(i)+"\n";
    body+="NEWEST_SCROLL_LINE";
    J *message=obj();
    put(message,"role",str("assistant"));
    put(message,"content",str(body.c_str()));
    add(a.messages,message);
    history_checkpoint(a,0);

    int master=-1;
    winsize window{32,90,0,0};
    pid_t child=forkpty(&master,nullptr,nullptr,&window);
    CHECK(child>=0);
    if(!child){
        setenv("TERM","xterm-256color",1);
        setenv("NO_COLOR","1",1);
        setenv("DESK_NO_ARROW","1",1);
        setenv("INPUTRC","/dev/null",1);
        unsetenv("DESK_NO_COMPOSER");
        execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));
        _exit(127);
    }
    std::string output;
    CHECK(terminal_until(master,output,"Select a number:"));
    type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"Choose a conversation"));
    type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"\033[?1006h"));
    CHECK(output.find("NEWEST_SCROLL_LINE")!=std::string::npos);
    CHECK(output.find("OLDEST_SCROLL_LINE")==std::string::npos);
    CHECK(output.find("\033[1;89H│")!=std::string::npos);
    CHECK(output.find("\033[28;89H┃")!=std::string::npos);
    output.clear();
    for(int i=0;i<60;i++)type_terminal(master,output,"\033[<64;4;4M");
    CHECK(terminal_until(master,output,"OLDEST_SCROLL_LINE"));
    output.clear();
    for(int i=0;i<60;i++)type_terminal(master,output,"\033[<65;4;4M");
    CHECK(terminal_until(master,output,"NEWEST_SCROLL_LINE"));
    // Clicking and dragging the scrollbar jumps directly to top and bottom.
    output.clear();
    type_terminal(master,output,"\033[<0;89;1M");
    CHECK(terminal_until(master,output,"OLDEST_SCROLL_LINE"));
    CHECK(terminal_until(master,output,"\033[1;89H┃"));
    CHECK(terminal_until(master,output,"\033[28;89H│"));
    output.clear();
    type_terminal(master,output,"\033[<32;40;32M");
    CHECK(terminal_until(master,output,"NEWEST_SCROLL_LINE"));
    CHECK(terminal_until(master,output,"\033[28;89H┃"));
    output.clear();
    // Release outside the track ends capture; later motion cannot scroll.
    type_terminal(master,output,"\033[<0;40;32m");
    type_terminal(master,output,"\033[<32;89;1M");
    CHECK(output.find("OLDEST_SCROLL_LINE")==std::string::npos);
    // Grabbing inside the thumb must not jump before movement begins.
    type_terminal(master,output,"\033[<0;89;28M\033[<32;89;28M");
    CHECK(output.find("History line 26")==std::string::npos);
    type_terminal(master,output,"\033[<0;89;28m");
    type_terminal(master,output,"\033[5~\033[5~");
    CHECK(terminal_until(master,output,"OLDEST_SCROLL_LINE"));
    CHECK(terminal_until(master,output,"more · PgDn"));
    output.clear();
    type_terminal(master,output,"\033[6~\033[6~");
    CHECK(terminal_until(master,output,"NEWEST_SCROLL_LINE"));
    std::string full_rule="\033[29;1H\033[2K  ";
    for(int i=0;i<80;i++)full_rule+="─";
    CHECK(terminal_until(master,output,full_rule.c_str()));
    CHECK(output.substr(output.rfind("\033[29;1H\033[2K")).find("more · PgDn")==std::string::npos);
    leave_chat_and_exit(master);
    CHECK(reap_pty(master,child)==0);
    // Legacy native-selection settings must not disable scrollbar dragging.
    // Up/Down remain available for keyboard scrolling.
    child=forkpty(&master,nullptr,nullptr,&window);
    CHECK(child>=0);
    if(!child){
        setenv("TERM","xterm-256color",1);
        setenv("NO_COLOR","1",1);
        setenv("DESK_NO_ARROW","1",1);
        setenv("INPUTRC","/dev/null",1);
        setenv("DESK_NATIVE_MOUSE_SELECTION","1",1);
        unsetenv("DESK_NO_COMPOSER");
        execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));
        _exit(127);
    }
    output.clear();
    CHECK(terminal_until(master,output,"Select a number:"));
    type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"Choose a conversation"));
    type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"\033[?1007h"));
    CHECK(terminal_until(master,output,"\033[?1000h"));
    CHECK(output.find("OLDEST_SCROLL_LINE")==std::string::npos);
    output.clear();
    for(int i=0;i<60;i++)type_terminal(master,output,"\033[A");
    CHECK(terminal_until(master,output,"OLDEST_SCROLL_LINE"));
    output.clear();
    for(int i=0;i<60;i++)type_terminal(master,output,"\033[B");
    CHECK(terminal_until(master,output,"NEWEST_SCROLL_LINE"));
    leave_chat_and_exit(master);
    CHECK(reap_pty(master,child)==0);
    // Inline approval row: wheel scrolling reviews the diff/transcript above
    // the choices without shifting focus away from Cancel (choice 0).
    for(bool native_select:{false,true}){
        child=forkpty(&master,nullptr,nullptr,&window);
        CHECK(child>=0);
        if(!child){
            unsetenv("DESK_NO_ARROW");
            unsetenv("DESK_NO_COMPOSER");
            setenv("TERM","xterm-256color",1);
            setenv("NO_COLOR","1",1);
            if(native_select)setenv("DESK_NATIVE_MOUSE_SELECTION","1",1);
            else unsetenv("DESK_NATIVE_MOUSE_SELECTION");
            set_composer_layout(30,28,1);
            terminal_message(body+"\n");
            printf("\033[30;1H\033[2K");
            fflush(stdout);
            auto picked=terminal_choice({"Allow","Deny","Cancel"},0,30,{},true,true);
            reset_transcript();
            printf("\nINLINE_PICK=%d\n",picked.value_or(-9));
            fflush(stdout);
            _exit(0);
        }
        output.clear();
        CHECK(terminal_until(master,output,"❯ 0. Cancel"));
        CHECK(output.find("OLDEST_SCROLL_LINE")==std::string::npos);
        output.clear();
        if(native_select){for(int i=0;i<60;i++)type_terminal(master,output,"\033[A");}
        else{for(int i=0;i<60;i++)type_terminal(master,output,"\033[<64;4;4M");}
        CHECK(terminal_until(master,output,"OLDEST_SCROLL_LINE"));
        output.clear();
        type_terminal(master,output,"\n");
        CHECK(terminal_until(master,output,"INLINE_PICK=0"));
        CHECK(reap_pty(master,child)==0);
    }
    app_cleanup(&a);
}
// Earlier requests come back with Ctrl+P/Ctrl+N (and Alt+Up/Down); the draft
// typed before recalling is restored, and the footer shows context fill.
static void test_prompt_recall_and_context(){
    App a=fresh();
    json_object_put(a.config);a.config=config("recall","openai");
    const std::string reply=model_reply(nullptr,"RECALL_DONE");
    const char *responses[]={reply.c_str()};
    pid_t server=fake_server(&a,responses,1);
    CHECK(!save_config(&a,a.config));
    const char *id="22222222222222222222222222222222";
    CHECK(history_save(a,id,"Recall chat",a.config));
    a.conversation_id=id;
    for(const char *text:{"ALPHA_ONE_X","BRAVO_TWO_Y"}){
        J *user=obj();put(user,"role",str("user"));put(user,"content",str(text));add(a.messages,user);
        J *answer=obj();put(answer,"role",str("assistant"));put(answer,"content",str("ok"));add(a.messages,answer);
    }
    history_checkpoint(a,0);
    int master=-1;
    winsize window{30,100,0,0};
    pid_t child=forkpty(&master,nullptr,nullptr,&window);
    CHECK(child>=0);
    if(!child){
        setenv("TERM","xterm-256color",1);
        setenv("NO_COLOR","1",1);
        setenv("DESK_NO_ARROW","1",1);
        setenv("INPUTRC","/dev/null",1);
        unsetenv("DESK_NO_COMPOSER");
        execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));
        _exit(127);
    }
    std::string output;
    CHECK(terminal_until(master,output,"Select a number:"));
    type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"Choose a conversation"));
    type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"\033[?1006h"));
    type_terminal(master,output,"draft");
    output.clear();
    type_terminal(master,output,"\x10");
    CHECK(terminal_until(master,output,"BRAVO_TWO_Y"));
    output.clear();
    type_terminal(master,output,"\033[1;3A");
    CHECK(terminal_until(master,output,"ALPHA_ONE_X"));
    output.clear();
    type_terminal(master,output,"\x0e\x0e");
    CHECK(terminal_until(master,output,"draft"));
    // Clear the restored draft, recall the newest request and send it.
    type_terminal(master,output,std::string(5,'\x7f'));
    output.clear();
    type_terminal(master,output,"\x10\n");
    CHECK(terminal_until(master,output,"RECALL_DONE"));
    CHECK(terminal_until(master,output," · Context ~"));
    CHECK(terminal_until(master,output,"(est.)"));
    // Reopening a finished chat shows the saved reply once, not again from
    // the worker's raw output.
    output.clear();
    type_terminal(master,output,"\x04");
    CHECK(terminal_until(master,output,"Choose a conversation"));
    output.clear();
    type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"RECALL_DONE"));
    {
        double until=monotime()+0.6;
        while(monotime()<until){
            pollfd ready{master,POLLIN,0};
            if(poll(&ready,1,50)>0){char bytes[4096];ssize_t n=read(master,bytes,sizeof bytes);if(n>0)output.append(bytes,static_cast<size_t>(n));}
        }
    }
    size_t replies=0;
    for(size_t at_=output.find("RECALL_DONE");at_!=std::string::npos;at_=output.find("RECALL_DONE",at_+1))replies++;
    CHECK(replies==1);
    CHECK(output.find("Worked for")==std::string::npos);
    leave_chat_and_exit(master);
    CHECK(reap_pty(master,child)==0);
    server_done(server);
    JsonPtr request(capture(&a,0));
    J *messages=get(get(request.get(),"body"),"messages");
    CHECK(!strcmp(sget(at(messages,alen(messages)-1),"content"),"BRAVO_TWO_Y"));
    app_cleanup(&a);
}
static void test_stream_usage_and_tail_utf8(){
    // 1. display_utf8 holds an incomplete multi-byte suffix while streaming,
    //    then flushes it as U+FFFD once the worker settles (final=true).
    {
        size_t used=0;
        const std::string part1="前缀:\xE4\xB8";
        CHECK(display_utf8(part1,used,false)=="前缀:");
        CHECK(used==7);
        const std::string rest=part1.substr(used)+"\xAD!";
        size_t used2=0;
        CHECK(display_utf8(rest,used2,false)=="中!");
        CHECK(used2==rest.size());
        size_t used_final=0;
        CHECK(display_utf8(part1,used_final,true)=="前缀:\xEF\xBF\xBD");
        CHECK(used_final==part1.size());
        // Overlong / surrogate prefixes are rejected immediately even before final.
        size_t used_bad=0;
        CHECK(display_utf8(std::string_view("\xED\xA0",2),used_bad,false)=="\xEF\xBF\xBD\xEF\xBF\xBD");
        CHECK(used_bad==2);
    }
    // 2. Rejecting stream_options on one provider/model must not disable token
    //    accounting for other providers or other models in the same process.
    auto start_usage_server=[](bool reject_usage,int expected_requests,std::string &url){
        int fd=socket(AF_INET,SOCK_STREAM,0);
        CHECK(fd>=0);
        sockaddr_in addr{};
        addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        CHECK(!bind(fd,reinterpret_cast<sockaddr *>(&addr),sizeof addr));
        socklen_t len=sizeof addr;
        CHECK(!getsockname(fd,reinterpret_cast<sockaddr *>(&addr),&len));
        CHECK(!listen(fd,4));
        char buf[128];
        snprintf(buf,sizeof buf,"http://127.0.0.1:%d/v1",ntohs(addr.sin_port));
        url=buf;
        pid_t pid=fork();
        CHECK(pid>=0);
        if(!pid){
            alarm(12);
            for(int i=0;i<expected_requests;i++){
                int client=accept(fd,nullptr,nullptr);
                if(client<0)_exit(2);
                FILE *s=fdopen(client,"r+");
                if(!s)_exit(2);
                char *line=nullptr;size_t cap=0;long clen=0;
                while(getline(&line,&cap,s)>0){
                    if(!strncasecmp(line,"Content-Length:",15))clen=strtol(line+15,nullptr,10);
                    if(!strcmp(line,"\r\n")||!strcmp(line,"\n"))break;
                }
                free(line);
                std::string body(static_cast<size_t>(std::max(0L,clen)),'\0');
                if(clen>0&&fread(body.data(),1,body.size(),s)!=body.size())_exit(2);
                const bool has_opt=body.find("\"stream_options\"")!=std::string::npos;
                if(reject_usage&&has_opt){
                    const char *err_body="{\"error\":{\"message\":\"unknown field stream_options\"}}";
                    fprintf(s,"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                            strlen(err_body),err_body);
                }else if(!reject_usage&&!has_opt){
                    const char *err_body="{\"error\":{\"message\":\"expected stream_options\"}}";
                    fprintf(s,"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                            strlen(err_body),err_body);
                }else{
                    const char *sse=
                        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"OK_USAGE\"}}]}\n\n"
                        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"prompt_tokens\":25,\"completion_tokens\":17,\"total_tokens\":42}}\n\n"
                        "data: [DONE]\n\n";
                    fprintf(s,"HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                            strlen(sse),sse);
                }
                fflush(s);
                fclose(s);
            }
            close(fd);
            _exit(0);
        }
        close(fd);
        return pid;
    };
    App a=fresh();
    J *user=obj();put(user,"role",str("user"));put(user,"content",str("hi"));add(a.messages,user);
    std::string url_strict,url_full;
    // Provider A: 1st call sends stream_options (400) + retries without (200);
    // 2nd call remembers and skips stream_options directly (1 request = 3 total).
    pid_t srv_strict=start_usage_server(true,3,url_strict);
    // Provider B (and a second model): must still send stream_options (2 requests).
    pid_t srv_full=start_usage_server(false,2,url_full);

    JsonPtr prof_a(config("prov-a","openai"));
    put(prof_a.get(),"base_url",str(url_strict.c_str()));
    put(prof_a.get(),"model",str("strict-model"));
    a.task_tokens=0;
    JsonPtr r1(api_complete(&a,prof_a.get(),a.messages,nullptr));
    CHECK(r1&&!strcmp(sget(r1.get(),"content"),"OK_USAGE"));
    JsonPtr r2(api_complete(&a,prof_a.get(),a.messages,nullptr));
    CHECK(r2&&!strcmp(sget(r2.get(),"content"),"OK_USAGE"));

    JsonPtr prof_b(config("prov-b","openai"));
    put(prof_b.get(),"base_url",str(url_full.c_str()));
    put(prof_b.get(),"model",str("strict-model"));
    a.task_tokens=0;
    JsonPtr r3(api_complete(&a,prof_b.get(),a.messages,nullptr));
    CHECK(r3&&!strcmp(sget(r3.get(),"content"),"OK_USAGE"));
    CHECK(a.task_tokens==42);

    JsonPtr prof_b2(config("prov-a","openai"));
    put(prof_b2.get(),"base_url",str(url_full.c_str()));
    put(prof_b2.get(),"model",str("other-model"));
    a.task_tokens=0;
    JsonPtr r4(api_complete(&a,prof_b2.get(),a.messages,nullptr));
    CHECK(r4&&!strcmp(sget(r4.get(),"content"),"OK_USAGE"));
    CHECK(a.task_tokens==42);

    JsonPtr usage(load_tokens(a));
    CHECK(model_tokens(usage.get(),prof_b.get())==42);
    server_done(srv_strict);
    server_done(srv_full);
    app_cleanup(&a);
}
static void test_chat_actions_layout(){
    App a=fresh();
    json_object_put(a.config);a.config=config("chat-layout","openai");
    CHECK(!save_config(&a,a.config));
    int master=-1;
    winsize window{12,50,0,0};
    pid_t child=forkpty(&master,nullptr,nullptr,&window);
    CHECK(child>=0);
    if(!child){
        setenv("TERM","xterm-256color",1);
        setenv("NO_COLOR","1",1);
        unsetenv("DESK_NO_ARROW");
        unsetenv("DESK_NO_COMPOSER");
        execl("./build/desk","desk","--lang","en","--workspace",a.root,"chat",static_cast<char *>(nullptr));
        _exit(127);
    }
    std::string output;
    CHECK(terminal_until(master,output,"❯ "));
    output.clear();
    type_terminal(master,output,"/");
    CHECK(terminal_until(master,output,"0. Back to chat"));
    CHECK(output.find("Chat actions")<output.find("❯ 1. Select model"));
    CHECK(output.find("2. Reasoning mode")<output.find("3. Approval mode"));
    CHECK(output.find("6. More settings")!=std::string::npos);
    CHECK(output.find("MCP servers")==std::string::npos);
    CHECK(output.find("Select a number:")==std::string::npos);

    output.clear();
    window.ws_row=8;window.ws_col=38;
    CHECK(!ioctl(master,TIOCSWINSZ,&window));
    CHECK(terminal_until(master,output,"❯ 1. Select model"));
    CHECK(output.find("\033[2J")==std::string::npos);
    output.clear();
    type_terminal(master,output,"\033[F");
    CHECK(terminal_until(master,output,"❯ 0. Back to chat"));
    output.clear();
    type_terminal(master,output,"\n");
    CHECK(terminal_until(master,output,"\033[?1006h"));

    output.clear();
    type_terminal(master,output,"\x04");
    CHECK(terminal_until(master,output,"0. Main menu"));
    // Ctrl+D in the conversation selector steps back to the main menu instead
    // of closing DeskCode, and Ctrl+D on the main menu stays put.
    output.clear();
    type_terminal(master,output,"\x04");
    CHECK(terminal_until(master,output,"0. Exit"));
    type_terminal(master,output,"\x04");
    usleep(80000);
    CHECK(kill(child,0)==0);
    output.clear();
    type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"0. Main menu"));
    output.clear();
    type_terminal(master,output,"0\n");
    CHECK(terminal_until(master,output,"0. Exit"));
    type_terminal(master,output,"0\n");
    CHECK(reap_pty(master,child)==0);
    app_cleanup(&a);
}
static void test_menu_navigation_and_resize(){
    int master=-1;
    winsize window{12,80,0,0};
    pid_t child=forkpty(&master,nullptr,nullptr,&window);
    CHECK(child>=0);
    if(!child){
        unsetenv("DESK_NO_ARROW");
        setenv("TERM","xterm-256color",1);
        setenv("NO_COLOR","1",1);
        termios before{},after{};
        if(tcgetattr(0,&before))_exit(2);
        std::vector<std::string> items;
        for(int i=1;i<=24;i++)items.push_back("Item "+std::to_string(i));
        items.back()+="\nunsafe\x1b[31m";
        items.emplace_back("Back");
        auto choice=terminal_choice(items,1,1);
        if(tcgetattr(0,&after))_exit(3);
        const tcflag_t user_lflags=ICANON|ECHO|ISIG|IEXTEN;
        printf("RESULT=%d RESTORED=%d\n",choice.value_or(-2),(before.c_lflag&user_lflags)==(after.c_lflag&user_lflags));
        fflush(stdout);
        _exit(0);
    }
    std::string output;
    CHECK(terminal_until(master,output,"❯ 1. Item 1"));
    output.clear();
    CHECK(write(master,"\x1b",1)==1);
    usleep(80000);
    CHECK(write(master,"[B",2)==2);
    CHECK(terminal_until(master,output,"❯ 2. Item 2"));
    window.ws_row=6;window.ws_col=35;
    output.clear();
    CHECK(!ioctl(master,TIOCSWINSZ,&window));
    CHECK(terminal_until(master,output,"❯ 2. Item 2"));
    CHECK(output.find("\033[5A")!=std::string::npos);
    output.clear();type_terminal(master,output,"\x1b[6~");
    CHECK(terminal_until(master,output,"❯ 7. Item 7"));
    output.clear();type_terminal(master,output,"\x1b[H");
    CHECK(terminal_until(master,output,"❯ 1. Item 1"));
    output.clear();type_terminal(master,output,"\x1b[F");
    CHECK(terminal_until(master,output,"❯ 0. Back"));
    CHECK(output.find("Item 24 unsafe [31m")!=std::string::npos);
    CHECK(output.find("unsafe\x1b[31m")==std::string::npos);
    output.clear();type_terminal(master,output,"12\n");
    CHECK(terminal_until(master,output,"RESULT=12 RESTORED=1"));
    CHECK(reap_pty(master,child)==0);
}
static void test_persistent_conversations(){
    App a=fresh();
    json_object_put(a.config);a.config=config("saved-chat","openai");
    std::string first=model_reply(nullptr,"PERSISTED_REPLY");
    std::string second=model_reply(nullptr,"RESUMED_REPLY");
    const char *responses[]={first.c_str(),second.c_str()};
    pid_t server=fake_server(&a,responses,2);
    CHECK(!save_config(&a,a.config));
    auto launch=[&](int &master){
        pid_t child=forkpty(&master,nullptr,nullptr,nullptr);
        CHECK(child>=0);
        if(!child){
            setenv("DESK_NO_ARROW","1",1);
            setenv("TERM","xterm-256color",1);
            unsetenv("NO_COLOR");
            execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));
            _exit(127);
        }
        return child;
    };
    auto close_app=[&](int &master,pid_t child,std::string &output){
        output.clear();type_terminal(master,output,"0\n");
        CHECK(terminal_until(master,output,"\033[38;5;247m  [1]\033[0m\033[38;5;111m Start chat"));
        type_terminal(master,output,"0\n");
        CHECK(reap_pty(master,child)==0);
    };
    int master=-1;pid_t child=launch(master);std::string output;
    type_terminal(master,output,"1\n1\npersist my work\n");
    CHECK(terminal_until(master,output,"\033[97m●\033[0m PERSISTED_REPLY"));
    wait_for_chat_prompt(master,output,"PERSISTED_REPLY");
    output.clear();type_terminal(master,output,"\x04");
    CHECK(terminal_until(master,output,"Choose a conversation"));
    type_terminal(master,output,"3\n1\nSaved title\n");
    CHECK(terminal_until(master,output,"\033[38;5;247m1.\033[0m Saved title"));
    close_app(master,child,output);
    JsonPtr list(history_list(a));CHECK(alen(list.get())==1);
    std::string id=sget(at(list.get(),0),"id");
    JsonPtr saved(history_read(a,id));
    CHECK(alen(get(saved.get(),"context"))==2);
    CHECK(std::string(json_object_to_json_string(saved.get())).find("fake-test-key")==std::string::npos);
    child=launch(master);output.clear();
    type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"\033[38;5;247m1.\033[0m Saved title"));
    output.clear();type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"\033[97m●\033[0m PERSISTED_REPLY"));
    type_terminal(master,output,"continue that work\n");
    CHECK(terminal_until(master,output,"RESUMED_REPLY"));
    wait_for_chat_prompt(master,output,"RESUMED_REPLY");
    output.clear();type_terminal(master,output,"\x04");
    CHECK(terminal_until(master,output,"Choose a conversation"));
    type_terminal(master,output,"4\n1\n1\n");
    CHECK(terminal_until(master,output,"No conversations yet."));
    close_app(master,child,output);
    list.reset(history_list(a));CHECK(alen(list.get())==0);
    saved.reset(history_read(a,id));CHECK(!saved);
    child=launch(master);output.clear();type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"No conversations yet."));
    CHECK(output.find("Saved title")==std::string::npos);
    close_app(master,child,output);
    server_done(server);
    JsonPtr request(capture(&a,1));J *messages=get(get(request.get(),"body"),"messages");
    CHECK(alen(messages)==3);
    CHECK(!strcmp(sget(at(messages,0),"content"),"persist my work"));
    app_cleanup(&a);
}
static void test_guided_approval(){
    App a=fresh();
    json_object_put(a.config);
    a.config=config("guided-approval","openai");
    J *write=args("guided.txt");
    put(write,"content",str("review-before-allow"));
    put(write,"expected_sha256",nullptr);
    std::string edit=model_tool("write","write_file",write);
    std::string done=model_reply(nullptr,"GUIDED_EDIT_DONE");
    std::string follow=model_reply(nullptr,"DRAFT_RECEIVED");
    const char *responses[]={edit.c_str(),done.c_str(),follow.c_str()};
    pid_t server=fake_server(&a,responses,3,200,700);
    CHECK(!save_config(&a,a.config));
    int master=-1;
    winsize window{30,100,0,0};
    pid_t child=forkpty(&master,nullptr,nullptr,&window);
    CHECK(child>=0);
    if(!child){
        unsetenv("DESK_NO_ARROW");
        setenv("TERM","xterm-256color",1);
        setenv("NO_COLOR","1",1);
        execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));
        _exit(127);
    }
    std::string output;
    CHECK(terminal_until(master,output,"❯ 1. Start chat"));
    type_terminal(master,output,"1\n1\nwrite the file\n");
    CHECK(terminal_until(master,output,"Thinking..."));
    output.clear();
    type_terminal(master,output,"next draft");
    CHECK(terminal_until(master,output,"❯ 0. Cancel"));
    std::string path=std::string(a.root)+"/guided.txt";
    CHECK(access(path.c_str(),F_OK)!=0);
    output.clear();
    type_terminal(master,output,"\x1b[A\n");
    CHECK(terminal_until(master,output,"review-before-allow"));
    CHECK(terminal_until(master,output,"❯ 0. Cancel"));
    CHECK(access(path.c_str(),F_OK)!=0);
    output.clear();
    type_terminal(master,output,"\x1b[A\x1b[A\x1b[A\n");
    CHECK(terminal_until(master,output,"GUIDED_EDIT_DONE"));
    CHECK(terminal_until(master,output,"next draft"));
    output.clear();
    type_terminal(master,output,"\n");
    CHECK(terminal_until(master,output,"DRAFT_RECEIVED"));
    wait_for_chat_prompt(master,output,"DRAFT_RECEIVED");
    output.clear();
    type_terminal(master,output,"draft kept");
    type_terminal(master,output,"\x1bOR");
    CHECK(terminal_until(master,output,"❯ 1. Off"));
    output.clear();type_terminal(master,output,"5\n");
    CHECK(terminal_until(master,output,"Reasoning: High"));
    CHECK(terminal_until(master,output,"draft kept"));
    type_terminal(master,output,std::string(10,'\x7f'));
    type_terminal(master,output,"\x1bOQ");
    CHECK(terminal_until(master,output,"model-test (openai)"));
    output.clear();type_terminal(master,output,"0\n");
    CHECK(terminal_until(master,output,"❯ "));
    type_terminal(master,output,"/");
    CHECK(terminal_until(master,output,"❯ 1. Select model"));
    output.clear();
    type_terminal(master,output,"\x1b");
    CHECK(terminal_until(master,output,"❯ "));
    CHECK(output.find("Select a number:")==std::string::npos);
    output.clear();
    type_terminal(master,output,"\x04");
    CHECK(terminal_until(master,output,"0. Main menu"));
    output.clear();
    type_terminal(master,output,"0\n");
    CHECK(terminal_until(master,output,"❯ 1. Start chat"));
    type_terminal(master,output,"0\n");
    CHECK(reap_pty(master,child)==0);
    server_done(server);
    JsonPtr request(capture(&a,2));
    J *messages=get(get(request.get(),"body"),"messages");
    CHECK(!strcmp(sget(at(messages,alen(messages)-1),"content"),"next draft"));
    app_cleanup(&a);
}
static void test_line_editing(void){
    App a=fresh();json_object_put(a.config);a.config=config("line-edit","openai");
    std::vector<std::pair<std::string,std::string>> cases={
        {"你好😀"+std::string(3,'\x7f')+"hi","hi"},
        {"ab中cd\x1b[D\x1b[D\x7f\x1b[3~文\x05\x7f\x01首","首ab文"},
        {std::string(400,'x')+"测试😀"+std::string(403,'\x7f')+"erase-ok","erase-ok"},
        {std::string(5000,'z'),std::string(5000,'z')},
        {std::string(5000,'z')+std::string(5000,'\x7f')+"clean","clean"},
        {"\x1b[200~多行\n内容\x1b[201~","多行\n内容"}
    };
    std::vector<std::string> replies;std::vector<const char *> responses;
    for(size_t i=0;i<cases.size();i++)replies.push_back(model_reply(nullptr,("EDIT_DONE_"+std::to_string(i)).c_str()));
    for(auto &reply:replies)responses.push_back(reply.c_str());
    pid_t server=fake_server(&a,responses.data(),static_cast<int>(responses.size()));CHECK(!save_config(&a,a.config));
    int master=-1;struct winsize window{12,24,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){
        setenv("TERM","xterm-256color",1);
        setenv("NO_COLOR","1",1);setenv("INPUTRC","/dev/null",1);struct termios mode{};if(!tcgetattr(STDIN_FILENO,&mode)){mode.c_iflag&=~IUTF8;tcsetattr(STDIN_FILENO,TCSANOW,&mode);}
        execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
        std::string output;CHECK(write(master,"1\n1\n",4)==4);CHECK(terminal_until(master,output,"Approval: Ask for approval\r\n❯ "));
    for(size_t i=0;i<cases.size();i++){
        output.clear();type_terminal(master,output,cases[i].first+"\n");std::string done="EDIT_DONE_"+std::to_string(i);CHECK(terminal_until(master,output,done.c_str()));wait_for_chat_prompt(master,output,done.c_str());
        JsonPtr request(capture(&a,static_cast<int>(i)));J *messages=get(get(request.get(),"body"),"messages");CHECK(cases[i].second==sget(at(messages,alen(messages)-1),"content"));CHECK(valid_utf8(sget(at(messages,alen(messages)-1),"content")));
    }
    leave_chat_and_exit(master);CHECK(reap_pty(master,child)==0);server_done(server);app_cleanup(&a);
}
static void test_queued_editing(void){
    App a=fresh();json_object_put(a.config);a.config=config("queued-edit","openai");
    std::string first=model_tool("run","run_shell",command("sleep .4")),second=model_reply(nullptr,"PRETYPE_DONE"),third=model_reply(nullptr,"QUEUED_DONE");const char *responses[]={first.c_str(),second.c_str(),third.c_str()};pid_t server=fake_server(&a,responses,3);CHECK(!save_config(&a,a.config));
    int master=-1;struct winsize window{24,80,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){setenv("TERM","xterm-256color",1);
        setenv("NO_COLOR","1",1);setenv("INPUTRC","/dev/null",1);execl("./build/desk","desk","--lang","en","--yes","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    CHECK(write(master,"1\n1\nfirst\n",10)==10);std::string output;CHECK(terminal_until(master,output,"Running a command..."));
    std::string queued="排队😀\x7f下一句\n";type_terminal(master,output,queued);CHECK(terminal_until(master,output,"Queued. This will run after the current operation."));CHECK(terminal_until(master,output,"QUEUED_DONE"));wait_for_chat_prompt(master,output,"QUEUED_DONE");
    JsonPtr request(capture(&a,2));J *messages=get(get(request.get(),"body"),"messages");CHECK(!strcmp(sget(at(messages,alen(messages)-1),"content"),"排队下一句"));
    leave_chat_and_exit(master);CHECK(reap_pty(master,child)==0);server_done(server);app_cleanup(&a);
}
static void test_conversation_picker(void){
    App a=fresh();json_object_put(a.config);a.config=config("conversation-picker","openai");
    const std::string first="请帮我整理比赛资料。后续说明",second="Draft a demo video",follow="Continue the notes",third="Write a release checklist";
    std::vector<std::string> replies={model_reply(nullptr,"REPLY_ONE"),model_reply(nullptr,"REPLY_TWO"),model_reply(nullptr,"REPLY_RESUMED"),model_reply(nullptr,"REPLY_FRESH")};
    replies[0].insert(replies[0].size()-1,",\"usage\":{\"total_tokens\":12}");
    replies[1].insert(replies[1].size()-1,",\"usage\":{\"total_tokens\":18}");
    const char *responses[]={replies[0].c_str(),replies[1].c_str(),replies[2].c_str(),replies[3].c_str()};
    pid_t server=fake_server(&a,responses,4);CHECK(!save_config(&a,a.config));
    JsonPtr alternate(copy(a.config));put(alternate.get(),"profile",str("alternate"));put(alternate.get(),"model",str("alternate-model"));
    CHECK(!save_config(&a,alternate.get()));CHECK(!activate(&a,"conversation-picker"));
    char state_path[PATH_MAX];CHECK(!path_suffix(state_path,a.root,"/private-state"));
    int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){
        setenv("XDG_STATE_HOME",state_path,1);
        execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
    std::string output;
    CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(output.find("Imported models")!=std::string::npos);
    CHECK(output.find("2. alternate-model · 0 Token")!=std::string::npos);
    CHECK(output.find("1. model-test · 0 Token · Current")!=std::string::npos);
    type_terminal(master,output,"1\n1\n"+first+"\n");CHECK(terminal_until(master,output,"REPLY_ONE"));wait_for_chat_prompt(master,output,"REPLY_ONE");
    CHECK(write(master,"\x04",1)==1);output.clear();
    CHECK(terminal_until(master,output,"1. 整理比赛资料"));CHECK(terminal_until(master,output,"Select a number:"));
    output.clear();type_terminal(master,output,"3\n2\n1\n我的比赛笔记\n");
    CHECK(terminal_until(master,output,"1. 我的比赛笔记"));
    output.clear();type_terminal(master,output,"0\n2\n3\n2\n1\n2\n1\n1\n2\n"+second+"\n");CHECK(terminal_until(master,output,"REPLY_TWO"));wait_for_chat_prompt(master,output,"REPLY_TWO");
    CHECK(write(master,"\x04",1)==1);output.clear();
    CHECK(terminal_until(master,output,"1. Draft a demo video"));
    CHECK(terminal_until(master,output,"[4] Rename conversation"));
    CHECK(output.find("1. Draft a demo video")<output.find("2. 我的比赛笔记"));
    CHECK(output.find("2. 我的比赛笔记")<output.find("[4] Rename conversation"));
    output.clear();type_terminal(master,output,"5\n1\n0\n");
    CHECK(terminal_until(master,output,"[1] Confirm deletion"));
    CHECK(terminal_until(master,output,"1. Draft a demo video"));
    output.clear();type_terminal(master,output,"5\n1\n1\n");
    CHECK(terminal_until(master,output,"Choose a conversation"));
    auto after_delete=output.substr(output.find("Choose a conversation"));
    CHECK(terminal_until(master,after_delete,"Select a number:"));
    CHECK(after_delete.find("Draft a demo video")==std::string::npos);
    CHECK(after_delete.find("1. 我的比赛笔记")!=std::string::npos);
    output.clear();type_terminal(master,output,"1\n"+follow+"\n");
    CHECK(terminal_until(master,output,"Conversation: 我的比赛笔记"));CHECK(terminal_until(master,output,"REPLY_RESUMED"));
    output.clear();type_terminal(master,output,"/\n4\n"+third+"\n");CHECK(terminal_until(master,output,"REPLY_FRESH"));wait_for_chat_prompt(master,output,"REPLY_FRESH");
    leave_chat_and_exit(master);
    CHECK(reap_pty(master,child)==0);
    server_done(server);CHECK(access(state_path,F_OK)!=0);
    for(int i=0;i<4;i++){
        JsonPtr request(capture(&a,i));J *body=get(request.get(),"body"),*messages=get(body,"messages");
        CHECK(!get(body,"system"));CHECK(alen(messages)==1);
        CHECK(!strcmp(sget(body,"model"),i>0?"alternate-model":"model-test"));
        const std::string expected[]={first,second,follow,third};
        CHECK(expected[i]==sget(at(messages,alen(messages)-1),"content"));
        if(i==2)CHECK(follow==sget(at(messages,0),"content"));
    }
    child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    output.clear();
    CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(output.find("1. alternate-model · 18 Token")!=std::string::npos);
    CHECK(output.find("2. model-test · 12 Token")!=std::string::npos);
    CHECK(write(master,"1\n",2)==2);
    CHECK(terminal_until(master,output,"Choose a conversation"));
    auto tail=output.substr(output.find("Choose a conversation"));
    CHECK(terminal_until(master,tail,"Select a number:"));CHECK(tail.find("我的比赛笔记")!=std::string::npos);
    CHECK(write(master,"\x04",1)==1);CHECK(reap_pty(master,child)==0);
    app_cleanup(&a);
}

static void test_thinking_timer(){
    // Exercise the foreground renderer and the activity row in a pinned chat.
    for(bool interactive:{false,true}){
        App a=fresh();json_object_put(a.config);a.config=config("thinking-timer","openai");
        std::string path=std::string(a.root)+"/note.txt";CHECK(!atomic_file(path.c_str(),"hello",5,0600));
        const auto first=interactive?model_reply(nullptr,"TIMER_FIRST_DONE"):model_tool("read","read_file",args("note.txt"));
        const auto second=model_reply(nullptr,"TIMER_SECOND_DONE");
        const char *responses[]={first.c_str(),second.c_str()};
        pid_t server=fake_server(&a,responses,2,200,2800);CHECK(!save_config(&a,a.config));
        int master=-1;struct winsize window{40,110,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
        if(!child){
            setenv("TERM","xterm-256color",1);setenv("NO_COLOR","1",1);
            if(interactive)unsetenv("DESK_NO_COMPOSER");
            if(interactive)execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));
            else execl("./build/desk","desk","--lang","en","--yes","--workspace",a.root,"First timed request",static_cast<char *>(nullptr));
            _exit(127);
        }
        std::string output;
        if(interactive){
            CHECK(terminal_until(master,output,"Select a number:"));
            CHECK(output.find("    [1] Start chat\r\n\r\n    [2] Select model")!=std::string::npos);
            CHECK(output.find("Conversation activity")==std::string::npos);
            type_terminal(master,output,"1\n1\n");
            CHECK(terminal_until(master,output,"\033[40;1H\033[2K  Reasoning: Default · Approval: Ask for approval"));
            type_terminal(master,output,"First timed request\n");
        }
        CHECK(terminal_until(master,output,"Thinking... 0s"));
        CHECK(terminal_until(master,output,"Thinking... 1s"));
        if(interactive){
            const auto thinking=output.find("Thinking... 1s");
            const auto cursor=output.rfind(";1H\033[2K",thinking);
            CHECK(cursor!=std::string::npos);
            const auto escape=output.rfind("\033[",cursor);
            CHECK(escape!=std::string::npos);
            const int row=std::stoi(output.substr(escape+2,cursor-escape-2));
            CHECK(row>=1&&row<=36);
            CHECK(output.find("● First timed request")<thinking);
            CHECK(output.find("Thinking... 1s · Reasoning:")==std::string::npos);
            type_terminal(master,output,"secondx\x7f");
            CHECK(terminal_until(master,output,"Thinking... 2s"));
            CHECK(terminal_until(master,output,"● TIMER_FIRST_DONE"));
            output.clear();type_terminal(master,output,"\n");
        }else{
            CHECK(terminal_until(master,output,"Reading files..."));
            output.erase(0,output.find("Reading files..."));
        }
        CHECK(terminal_until(master,output,"Thinking... 0s"));
        CHECK(terminal_until(master,output,"Thinking... 1s"));
        CHECK(terminal_until(master,output,"TIMER_SECOND_DONE"));
        if(interactive)leave_chat_and_exit(master);
        CHECK(reap_pty(master,child)==0);server_done(server);
        JsonPtr request(capture(&a,1));J *messages=get(get(request.get(),"body"),"messages");
        CHECK(!strcmp(sget(at(messages,0),"content"),"First timed request"));
        if(interactive)CHECK(!strcmp(sget(at(messages,alen(messages)-1),"content"),"second"));
        CHECK(std::string(json_object_to_json_string(messages)).find("Thinking...")==std::string::npos);
        app_cleanup(&a);
    }
}
static void test_background_conversations(){
    App a=fresh();json_object_put(a.config);a.config=config("background-conversations","openai");
    const std::string first=model_reply(nullptr,"FIRST_DONE"),second=model_reply(nullptr,"SECOND_DONE"),follow=model_reply(nullptr,"FOLLOW_DONE");
    const char *responses[]={first.c_str(),second.c_str(),follow.c_str()};
    pid_t server=fake_server(&a,responses,3,200,1000);CHECK(!save_config(&a,a.config));
    int master=-1;struct winsize window{24,80,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){
        setenv("TERM","xterm-256color",1);
        setenv("NO_COLOR","1",1);
        execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);
    }
    std::string output;
    CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(output.find("Conversation activity")==std::string::npos);
    CHECK(output.find("\033[?1049h")!=std::string::npos);
    type_terminal(master,output,"1\n1\nFirst conversation\n");
    CHECK(terminal_until(master,output,"Thinking..."));
    CHECK(output.find("Running · First conversation")==std::string::npos);
    output.clear();CHECK(write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"[2] New conversation"));
    CHECK(terminal_until(master,output,"Running · First conversation"));
    output.clear();type_terminal(master,output,"0\n");
    CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(kill(child,0)==0);
    CHECK(output.find("Conversation activity")==std::string::npos);
    output.clear();type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"[2] New conversation"));
    output.clear();type_terminal(master,output,"2\nSecond conversation\n");
    CHECK(terminal_until(master,output,"Thinking..."));
    CHECK(output.find("Conversation activity")==std::string::npos);
    CHECK(terminal_until(master,output,"SECOND_DONE"));
    CHECK(output.find("FIRST_DONE")==std::string::npos);
    output.clear();CHECK(write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"2. First conversation"));
    type_terminal(master,output,"2\n");
    CHECK(terminal_until(master,output,"FIRST_DONE"));
    type_terminal(master,output,"Continue first\n");
    CHECK(terminal_until(master,output,"FOLLOW_DONE"));
    output.clear();CHECK(write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"[0] Main menu"));
    output.clear();type_terminal(master,output,"0\n");
    CHECK(terminal_until(master,output,"[1] Start chat"));
    CHECK(output.find("\033[?1049l")==std::string::npos);
    CHECK(output.substr(output.rfind("[1] Start chat")).find("Conversation activity")==std::string::npos);
    CHECK(write(master,"0\n",2)==2);
    CHECK(reap_pty(master,child)==0);server_done(server);
    for(int i=0;i<3;i++){
        JsonPtr request(capture(&a,i));J *messages=get(get(request.get(),"body"),"messages");
        CHECK(alen(messages)==(i==2?3u:1u));
        CHECK(!strcmp(sget(at(messages,0),"content"),i==1?"Second conversation":"First conversation"));
    }
    app_cleanup(&a);
}
static void test_picker_approval_status(){
    App a=fresh();json_object_put(a.config);a.config=config("picker-approval","openai");
    J *write=args("reviewed.txt");put(write,"content",str("approved"));put(write,"expected_sha256",nullptr);
    const auto action=model_tool("write","write_file",write),other=model_reply(nullptr,"OTHER_FINISHED"),done=model_reply(nullptr,"CHECKED_FILE");
    const char *responses[]={action.c_str(),other.c_str(),done.c_str()};
    pid_t server=fake_server(&a,responses,3,200,1200);CHECK(!save_config(&a,a.config));
    int master=-1;struct winsize window{30,110,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){setenv("TERM","xterm-256color",1);unsetenv("NO_COLOR");execl("./build/desk","desk","--lang","zh","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    std::string output;type_terminal(master,output,"1\n1\n创建文件\n");
    CHECK(terminal_until(master,output,"正在思考…"));
    output.clear();CHECK(::write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"选择对话"));
    CHECK(terminal_until(master,output,"待审批：1"));
    CHECK(terminal_until(master,output,"[1] 等待审批 · 创建文件"));
    CHECK(output.find("\033[38;5;179m")!=std::string::npos); // warning tone
    const auto path=std::string(a.root)+"/reviewed.txt";CHECK(access(path.c_str(),F_OK)!=0);
    // A second conversation changes selector numbers, but approvals stay first.
    output.clear();type_terminal(master,output,"2\n另一任务\n");
    CHECK(terminal_until(master,output,"正在思考…"));
    CHECK(output.find("对话运行状态")==std::string::npos);
    output.clear();CHECK(::write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"\033[38;5;247m1.\033[0m 另一任务"));
    CHECK(terminal_until(master,output,"\033[38;5;247m2.\033[0m 创建文件"));
    CHECK(terminal_until(master,output,"\033[2;1H\033[2K\033[1m\033[38;5;179m[2] 等待审批 · 创建文件"));
    output.clear();type_terminal(master,output,"2\n");
    CHECK(terminal_until(master,output,"[3] 查看完整操作 [0] 取消："));
    CHECK(access(path.c_str(),F_OK)!=0); // Choosing a chat never approves its operation.
    output.clear();type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"正在思考…"));
    CHECK(output.find("对话运行状态")==std::string::npos);
    output.clear();CHECK(::write(master,"\x04",1)==1);
    CHECK(terminal_until(master,output,"选择对话"));
    CHECK(terminal_until(master,output,"[1] 已完成 · 创建文件"));
    CHECK(terminal_until(master,output,"[2] 已完成 · 另一任务"));
    CHECK(output.find("CHECKED_FILE")==std::string::npos);
    CHECK(output.find("fake-test-key")==std::string::npos);
    output.clear();type_terminal(master,output,"0\n");CHECK(terminal_until(master,output,"\033[38;5;247m  [1]\033[0m\033[38;5;111m 开始对话"));
    CHECK(output.find("对话运行状态")==std::string::npos);CHECK(::write(master,"0\n",2)==2);
    CHECK(reap_pty(master,child)==0);
    server_done(server);char *content=read_bytes(path.c_str(),100,nullptr);CHECK(content&&!strcmp(content,"approved"));free(content);app_cleanup(&a);
}
static void test_picker_failed_status(){
    App a=fresh();json_object_put(a.config);a.config=config("picker-failure","openai");
    const char *responses[]={"{\"error\":{\"message\":\"fixture rejection\"}}"};
    pid_t server=fake_server(&a,responses,1,400,1200);CHECK(!save_config(&a,a.config));
    int master=-1;struct winsize window{24,80,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){setenv("TERM","xterm-256color",1);setenv("NO_COLOR","1",1);execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    std::string output;type_terminal(master,output,"1\n1\nRejected task\n");CHECK(terminal_until(master,output,"Thinking..."));
    output.clear();CHECK(write(master,"\x04",1)==1);CHECK(terminal_until(master,output,"Choose a conversation"));
    CHECK(terminal_until(master,output,"[1] Failed · Rejected task"));
    output.clear();type_terminal(master,output,"0\n");CHECK(terminal_until(master,output,"[1] Start chat"));CHECK(write(master,"0\n",2)==2);
    CHECK(reap_pty(master,child)==0);server_done(server);app_cleanup(&a);
}
static void test_model_switch_requests(){
    App a=fresh();json_object_put(a.config);a.config=config("original","openai");
    put(a.config,"model",str("original-model"));put(a.config,"api_key",str("original-test-key"));
    const auto original=model_reply(nullptr,"ORIGINAL_FINISHED");const char *old_replies[]={original.c_str()};
    pid_t old_server=fake_server(&a,old_replies,1,200,1800);CHECK(!save_config(&a,a.config));
    App target;CHECK(!path_suffix(target.root,a.root,"/target-api"));CHECK(!mkdirs(target.root,0700));
    target.config=config("selected","anthropic");put(target.config,"model",str("selected-model"));put(target.config,"api_key",str("selected-test-key"));
    const char *new_replies[]={
        "{\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"QUEUED_ON_SELECTED\"}]}",
        "{\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"SECOND_QUEUED_ON_SELECTED\"}]}",
        "{\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"FOLLOW_ON_SELECTED\"}]}",
        "{\"role\":\"assistant\",\"stop_reason\":\"end_turn\",\"content\":[{\"type\":\"text\",\"text\":\"RESTART_ON_SELECTED\"}]}"
    };
    pid_t new_server=fake_server(&target,new_replies,4);CHECK(!save_config(&a,target.config));CHECK(!activate(&a,"original"));
    int master=-1;struct winsize window{40,110,0,0};pid_t child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){setenv("TERM","xterm-256color",1);setenv("NO_COLOR","1",1);execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    std::string output;type_terminal(master,output,"1\n1\noriginal work\n");CHECK(terminal_until(master,output,"Thinking..."));
    output.clear();type_terminal(master,output,"pending work\npending follow\n/\n1\n2\n");
    CHECK(terminal_until(master,output,"Selected model: selected-model"));
    CHECK(terminal_until(master,output,"SECOND_QUEUED_ON_SELECTED"));wait_for_chat_prompt(master,output,"SECOND_QUEUED_ON_SELECTED");
    CHECK(output.find("ORIGINAL_FINISHED")!=std::string::npos);
    // Re-entering and reselecting the current model must retain its context.
    output.clear();CHECK(write(master,"\x04",1)==1);CHECK(terminal_until(master,output,"Choose a conversation"));
    output.clear();type_terminal(master,output,"1\n/\n1\n2\nfollow work\n");
    CHECK(terminal_until(master,output,"FOLLOW_ON_SELECTED"));wait_for_chat_prompt(master,output,"FOLLOW_ON_SELECTED");
    leave_chat_and_exit(master);CHECK(reap_pty(master,child)==0);
    child=forkpty(&master,nullptr,nullptr,&window);CHECK(child>=0);
    if(!child){setenv("TERM","xterm-256color",1);setenv("NO_COLOR","1",1);execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    output.clear();CHECK(terminal_until(master,output,"selected-model · "));
    CHECK(output.find("selected-model · ")!=std::string::npos&&output.find(" Token · Current")!=std::string::npos);
    type_terminal(master,output,"1\n1\nrestart work\n");CHECK(terminal_until(master,output,"RESTART_ON_SELECTED"));wait_for_chat_prompt(master,output,"RESTART_ON_SELECTED");
    leave_chat_and_exit(master);CHECK(reap_pty(master,child)==0);
    server_done(old_server);server_done(new_server);
    JsonPtr old_request(capture(&a,0));CHECK(!strcmp(sget(old_request.get(),"path"),"/v1/chat/completions"));CHECK(strstr(sget(old_request.get(),"authorization"),"original-test-key"));
    const char *expected[]={"pending work","pending follow","follow work","restart work"};
    for(int i=0;i<4;i++){
        JsonPtr request(capture(&target,i));J *body=get(request.get(),"body"),*messages=get(body,"messages");
        CHECK(!strcmp(sget(request.get(),"path"),"/v1/messages"));CHECK(!get(request.get(),"authorization"));CHECK(strstr(sget(request.get(),"key"),"selected-test-key"));
        CHECK(!strcmp(sget(body,"model"),"selected-model"));CHECK(!get(body,"system"));CHECK(alen(messages)==static_cast<size_t>(i*2+1));
        CHECK(!strcmp(sget(at(get(at(messages,alen(messages)-1),"content"),0),"text"),expected[i]));
        CHECK(std::string(json_object_to_json_string(messages)).find("original work")==std::string::npos);
    }
    app_cleanup(&target);app_cleanup(&a);
}
static int mcp_fixture(){
    char *line=nullptr;size_t capacity=0;
    while(getline(&line,&capacity,stdin)>0){
        JsonPtr message(json_tokener_parse(line));const char *method=sget(message.get(),"method");
        if(!*method||!get(message.get(),"id"))continue;
        JsonPtr response(obj());put(response.get(),"jsonrpc",str("2.0"));put(response.get(),"id",copy(get(message.get(),"id")));
        J *result=obj();
        if(!strcmp(method,"initialize")){
            puts("{\"jsonrpc\":\"2.0\",\"id\":\"server-ping\",\"method\":\"ping\"}");
            put(result,"protocolVersion",str("2025-06-18"));J *capabilities=obj();put(capabilities,"tools",obj());put(result,"capabilities",capabilities);
        }else if(!strcmp(method,"tools/list")){
            bool second=*sget(get(message.get(),"params"),"cursor");
            J *tools=arr(),*tool=obj();put(tool,"name",str(second?"fail":"echo"));
            put(tool,"description",str("MCP fixture"));
            J *schema=json_tokener_parse("{\"type\":\"object\",\"properties\":{\"value\":{\"type\":\"object\"}},\"required\":[\"value\"]}");
            put(tool,"inputSchema",schema);add(tools,tool);put(result,"tools",tools);
            if(!second)put(result,"nextCursor",str("next"));
        }else if(!strcmp(method,"tools/call")){
            J *params=get(message.get(),"params");
            put(result,"isError",json_object_new_boolean(!strcmp(sget(params,"name"),"fail")));
            put(result,"structuredContent",copy(get(params,"arguments")));
            put(result,"pid",json_object_new_int(getpid()));
            put(result,"inheritedSecret",json_object_new_boolean(getenv("MCP_TEST_SECRET")!=nullptr));
            put(result,"explicitEnv",str(getenv("EXPLICIT_VALUE")));
            put(result,"xauthority",str(getenv("XAUTHORITY")));
            J *content=arr(),*item=obj();put(item,"type",str("text"));put(item,"text",str("MCP_ECHO_OK"));add(content,item);put(result,"content",content);
        }else{put(result,"unexpected",str(method));}
        put(response.get(),"result",result);puts(json_object_to_json_string_ext(response.get(),JSON_C_TO_STRING_PLAIN));fflush(stdout);
    }
    free(line);return 0;
}
static std::string self_executable(){
#ifdef __APPLE__
    char buf[PATH_MAX];
    uint32_t sz=sizeof buf;
    CHECK(_NSGetExecutablePath(buf,&sz)==0);
    char resolved[PATH_MAX];
    return realpath(buf,resolved)?std::string(resolved):std::string(buf);
#else
    return path_text(fs::read_symlink("/proc/self/exe"));
#endif
}
static void test_mcp(){
    App a=fresh();JsonPtr servers(obj()),entry(obj());
    put(entry.get(),"command",str(self_executable().c_str()));
    J *arguments=arr();add(arguments,str("--mcp-fixture"));put(entry.get(),"args",arguments);
    J *environment=obj();put(environment,"EXPLICIT_VALUE",str("explicit-test-value"));put(entry.get(),"env",environment);
    put(servers.get(),"fixture",entry.release());CHECK(mcp_save(a,servers.get()));
    char path[PATH_MAX];CHECK(!path_suffix(path,a.config_dir,"/mcp.json"));struct stat mode{};CHECK(!stat(path,&mode));CHECK((mode.st_mode&0777)==0600);
    JsonPtr invalid(copy(servers.get()));put(get(invalid.get(),"fixture"),"disabled",str("false"));CHECK(!mcp_save(a,invalid.get()));
    JsonPtr loaded(mcp_load(a));CHECK(json_object_equal(loaded.get(),servers.get()));
    CHECK(!path_suffix(path,a.root,"/import.json"));const char *duplicate="{\"mcpServers\":{\"fixture\":{\"command\":\"bad\"}}}";CHECK(!atomic_file(path,duplicate,strlen(duplicate),0600));CHECK(!mcp_import(a,path));
    JsonPtr before_import(mcp_load(a));
    CHECK(!mcp_import_json(a,"{broken"));
    CHECK(strstr(a.error,"JSON"));
    CHECK(!mcp_import_json(a,"{}"));
    CHECK(!mcp_import_json(a,"{\"mcpServers\":{}}"));
    CHECK(!mcp_import_json(a,std::string(256001,'x')));
    JsonPtr after_failed_import(mcp_load(a));
    CHECK(json_object_equal(before_import.get(),after_failed_import.get()));
    CHECK(mcp_import_json(a,"{\"mcpServers\":{\"pasted\":{\"url\":\"https://example.invalid/mcp\",\"disabled\":true}}}"));
    CHECK(!mcp_import_json(a,"{\"mcpServers\":{\"pasted\":{\"url\":\"https://example.invalid/other\"}}}"));
    JsonPtr after_duplicate(mcp_load(a));
    CHECK(!strcmp(sget(get(after_duplicate.get(),"pasted"),"url"),"https://example.invalid/mcp"));
    const char *additional="{\"mcpServers\":{\"remote\":{\"url\":\"https://example.invalid/mcp\",\"disabled\":true}}}";
    CHECK(!atomic_file(path,additional,strlen(additional),0600));CHECK(mcp_import(a,path));
    loaded.reset(mcp_load(a));CHECK(json_object_object_length(loaded.get())==3);
    const char *inherited_xauthority=getenv("XAUTHORITY");
    std::string original_xauthority=inherited_xauthority?inherited_xauthority:"";
    const bool had_xauthority=inherited_xauthority!=nullptr;
    CHECK(!setenv("XAUTHORITY","/test/session.xauth",1));
    setenv("MCP_TEST_SECRET","must-not-leak",1);CHECK(mcp_prepare(a));unsetenv("MCP_TEST_SECRET");
    CHECK(!(had_xauthority?setenv("XAUTHORITY",original_xauthority.c_str(),1):unsetenv("XAUTHORITY")));
    JsonPtr tools(mcp_tools(a));CHECK(alen(tools.get())==2);std::string echo,fail;
    for(size_t i=0;i<alen(tools.get());i++){
        J *function=get(at(tools.get(),i),"function");
        if(strstr(sget(function,"description")," / echo."))echo=sget(function,"name");else fail=sget(function,"name");
    }
    CHECK(!echo.empty()&&!fail.empty());CHECK(echo.size()<=64);
    JsonPtr input(json_tokener_parse("{\"value\":{\"count\":3,\"ok\":true,\"list\":[1,2]}}"));
    {
        int saved_stdin=dup(STDIN_FILENO),null_in=open("/dev/null",O_RDONLY);
        CHECK(saved_stdin>=0&&null_in>=0&&dup2(null_in,STDIN_FILENO)>=0);
        close(null_in);
        a.yes=false;JsonPtr denied(mcp_call(a,echo.c_str(),input.get()));CHECK(bget(denied.get(),"denied"));
        CHECK(dup2(saved_stdin,STDIN_FILENO)>=0);
        close(saved_stdin);
    }
    a.yes=true;JsonPtr result(mcp_call(a,echo.c_str(),input.get()));CHECK(!tool_failed(result.get()));
    CHECK(json_object_equal(get(result.get(),"structuredContent"),input.get()));CHECK(!bget(result.get(),"inheritedSecret"));
    CHECK(!strcmp(sget(result.get(),"xauthority"),"/test/session.xauth"));
    CHECK(!strcmp(sget(result.get(),"explicitEnv"),"explicit-test-value"));pid_t child=iget(result.get(),"pid",0);CHECK(child>0);
    // If a stdio MCP server crashes between turns, mcp_prepare restarts it automatically.
    CHECK(kill(child,SIGKILL)==0);
    {double until=monotime()+2;while(alive(child)&&monotime()<until)usleep(10000);}
    CHECK(mcp_prepare(a));
    result.reset(mcp_call(a,echo.c_str(),input.get()));CHECK(!tool_failed(result.get()));
    pid_t restarted=iget(result.get(),"pid",0);CHECK(restarted>0&&restarted!=child);
    child=restarted;
    JsonPtr failed(mcp_call(a,fail.c_str(),input.get()));CHECK(tool_failed(failed.get()));
    CHECK(!workflow(&a,{model_tool("external",echo.c_str(),copy(input.get())),model_reply(nullptr,"MCP model integration complete")}));
    JsonPtr request(capture(&a,0));J *defs=get(get(request.get(),"body"),"tools");CHECK(alen(defs)==16);
    JsonPtr follow(capture(&a,1));J *messages=get(get(follow.get(),"body"),"messages");CHECK(strstr(sget(at(messages,alen(messages)-1),"content"),"MCP_ECHO_OK"));
    mcp_close(a);CHECK(kill(child,0)<0&&errno==ESRCH);
    put(get(loaded.get(),"fixture"),"disabled",json_object_new_boolean(true));CHECK(mcp_save(a,loaded.get()));CHECK(mcp_prepare(a));tools.reset(mcp_tools(a));CHECK(alen(tools.get())==0);
    app_cleanup(&a);
}
// ---------------------------------------------------------------------------
// Platform features: layout, Markdown, diff preview, headless mode, presets.
// ---------------------------------------------------------------------------
static void test_layout_and_markdown(){
    CHECK(ui_width("\033[38;5;111m中文ab\033[0m")==6);
    const std::string clipped=ui_clip("\033[1mabcdefghij\033[0m",5);
    CHECK(ui_width(clipped)==5);
    CHECK(clipped.find("\033[0m")!=std::string::npos);
    const auto wrapped=ui_wrap("\033[38;5;114m+ abcdefghij\033[0m",6);
    CHECK(wrapped.size()==2);
    for(const auto &line:wrapped)CHECK(ui_width(line)<=6);
    CHECK(wrapped[1].rfind("\033[38;5;114m",0)==0);
    // Without colour the reply is untouched apart from the hanging indent, and
    // terminal control bytes from the model never reach the terminal.
    {
        MarkdownStream plain(false,2);
        std::string out=plain.feed("# Title\nline **two**\x1b[31m");
        out+=plain.finish();
        CHECK(out=="# Title\n  line **two**[31m");
    }
    // With colour the structure is styled identically however it is chunked.
    const std::string source="# 标题\n- 第一项 **重点**\n普通 `code` 文本\n```cpp\nint x=1;\n```\n> 引用\n---\n";
    std::string whole,chunked;
    {MarkdownStream md(true,0);whole=md.feed(source);whole+=md.finish();}
    {MarkdownStream md(true,0);for(char c:source)chunked+=md.feed(std::string(1,c));chunked+=md.finish();}
    CHECK(whole==chunked);
    CHECK(whole.find("# ")==std::string::npos);
    CHECK(whole.find("标题")!=std::string::npos);
    CHECK(whole.find("•")!=std::string::npos);
    CHECK(whole.find("**")==std::string::npos);
    CHECK(whole.find("\033[1m")!=std::string::npos);
    CHECK(whole.find('`')==std::string::npos);
    CHECK(whole.find("┌─")!=std::string::npos&&whole.find("└─")!=std::string::npos);
    CHECK(whole.find("│ ")!=std::string::npos&&whole.find("int x=1;")!=std::string::npos);
    CHECK(whole.find("▎")!=std::string::npos);
    CHECK(whole.find("────")!=std::string::npos);
    // Inside `code` stars are literal; bold must not swallow them.
    {
        MarkdownStream md(true,0);
        std::string out=md.feed("use `**kwargs` and `2**10` here\n");out+=md.finish();
        CHECK(out.find("**kwargs")!=std::string::npos&&out.find("2**10")!=std::string::npos);
        CHECK(out.find("\033[1m")==std::string::npos);
    }
    // A ```python line inside a ````markdown block is content, not a close.
    {
        MarkdownStream md(true,0);
        std::string raw=md.feed("````markdown\n```python\n# not a heading\n```\n````\nafter\n");raw+=md.finish();
        std::string out;
        for(size_t i=0;i<raw.size();i++){
            if(raw[i]=='\033'){while(i<raw.size()&&raw[i]!='m')i++;continue;}
            out+=raw[i];
        }
        CHECK(out.find("┌─ markdown")!=std::string::npos);
        CHECK(out.find("│ ```python")!=std::string::npos);
        CHECK(out.find("│ # not a heading")!=std::string::npos);
        CHECK(out.find("│ ```\n")!=std::string::npos);
        CHECK(out.find("└─\nafter")!=std::string::npos);
    }
    // Emoji drawn two cells wide count as two; interface glyphs stay one.
    CHECK(ui_width("✅")==2&&ui_width("❌")==2&&ui_width("🚀")==2&&ui_width("⭐")==2);
    CHECK(ui_width("⏺")==1&&ui_width("⎿")==1&&ui_width("❯")==1&&ui_width("✓")==1);
    // Plain lines stay byte-identical, so ordinary replies look as before.
    {MarkdownStream md(true,2);std::string out=md.feed("PLAIN_REPLY\nnext");out+=md.finish();CHECK(out=="PLAIN_REPLY\n  next");}
}
static std::string capture_stdout(const std::function<void()> &body){
    fflush(stdout);
    FILE *log=tmpfile();CHECK(log);
    int saved=dup(STDOUT_FILENO);CHECK(saved>=0);
    CHECK(dup2(fileno(log),STDOUT_FILENO)>=0);
    body();
    fflush(stdout);
    CHECK(dup2(saved,STDOUT_FILENO)>=0);close(saved);
    long size=ftell(log);CHECK(size>=0);rewind(log);
    std::string text(static_cast<size_t>(size),'\0');
    CHECK(fread(text.data(),1,text.size(),log)==text.size());
    fclose(log);
    return text;
}
static void test_diff_preview(){
    CHECK(set_language("zh"));
    App a=fresh();a.yes=true;a.verbose=true;
    const char *original="alpha\nbeta\ngamma\ndelta\n";
    fixture(&a,"/diff.txt",original,strlen(original));
    char *hash=sha256(original,strlen(original));
    JsonPtr edit(args("diff.txt"));
    put(edit.get(),"old_text",str("beta"));
    put(edit.get(),"new_text",str("BETA"));
    put(edit.get(),"expected_sha256",str(hash));
    free(hash);
    JsonPtr result;
    std::string shown=capture_stdout([&]{result.reset(tool_call(&a,"edit_file",edit.get()));});
    CHECK(bget(result.get(),"verified"));
    CHECK(iget(result.get(),"line",0)==2);
    CHECK(shown.find("修改 diff.txt（+1 −1 行）")!=std::string::npos);
    CHECK(shown.find("@@ 第 2 行 @@")!=std::string::npos);
    CHECK(shown.find("- beta")!=std::string::npos&&shown.find("+ BETA")!=std::string::npos);
    // A whole-file overwrite shows only the changed middle with context.
    JsonPtr read(tool_call(&a,"read_file",JsonPtr(args("diff.txt")).get()));
    JsonPtr write(args("diff.txt"));
    put(write.get(),"content",str("alpha\nBETA\nGAMMA\ndelta\n"));
    put(write.get(),"expected_sha256",str(sget(read.get(),"sha256")));
    shown=capture_stdout([&]{result.reset(tool_call(&a,"write_file",write.get()));});
    CHECK(bget(result.get(),"verified"));
    CHECK(shown.find("覆盖 diff.txt（+1 −1 行）")!=std::string::npos);
    CHECK(shown.find("  alpha")!=std::string::npos&&shown.find("- gamma")!=std::string::npos);
    CHECK(shown.find("+ GAMMA")!=std::string::npos&&shown.find("  delta")!=std::string::npos);
    JsonPtr created(args("new.txt"));
    put(created.get(),"content",str("one\ntwo\n"));
    put(created.get(),"expected_sha256",nullptr);
    shown=capture_stdout([&]{result.reset(tool_call(&a,"write_file",created.get()));});
    CHECK(shown.find("新建 new.txt（+2 −0 行）")!=std::string::npos);
    app_cleanup(&a);
}
struct Captured {int status=-1;std::string out,err;};
static Captured run_desk(const std::vector<std::string> &arguments,const std::vector<std::pair<std::string,std::string>> &environment,const std::string &input){
    int in[2],out[2],errs[2];
    CHECK(!pipe(in)&&!pipe(out)&&!pipe(errs));
    pid_t child=fork();CHECK(child>=0);
    if(!child){
        dup2(in[0],0);dup2(out[1],1);dup2(errs[1],2);
        for(int fd:{in[0],in[1],out[0],out[1],errs[0],errs[1]})close(fd);
        for(const auto &[key,value]:environment)setenv(key.c_str(),value.c_str(),1);
        std::vector<char *> argv{const_cast<char *>("desk")};
        for(const auto &argument:arguments)argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        execv("./build/desk",argv.data());
        _exit(127);
    }
    close(in[0]);close(out[1]);close(errs[1]);
    if(!input.empty())CHECK(write(in[1],input.data(),input.size())==static_cast<ssize_t>(input.size()));
    close(in[1]);
    Captured result;
    bool out_open=true,err_open=true;
    double until=monotime()+20;
    while((out_open||err_open)&&monotime()<until){
        pollfd fds[2]={{out[0],POLLIN,0},{errs[0],POLLIN,0}};
        if(poll(fds,2,100)<=0)continue;
        char buffer[4096];
        if(out_open&&(fds[0].revents&(POLLIN|POLLHUP))){ssize_t n=read(out[0],buffer,sizeof buffer);if(n>0)result.out.append(buffer,static_cast<size_t>(n));else out_open=false;}
        if(err_open&&(fds[1].revents&(POLLIN|POLLHUP))){ssize_t n=read(errs[0],buffer,sizeof buffer);if(n>0)result.err.append(buffer,static_cast<size_t>(n));else err_open=false;}
    }
    close(out[0]);close(errs[0]);
    int status=0;
    pid_t ended=0;
    double wuntil=monotime()+10;
    while(monotime()<wuntil&&(ended=waitpid(child,&status,WNOHANG))==0)usleep(5000);
    if(ended!=child){kill(child,SIGKILL);waitpid(child,&status,0);}
    CHECK(ended==child);
    result.status=WIFEXITED(status)?WEXITSTATUS(status):-1;
    return result;
}
static std::vector<std::pair<std::string,std::string>> model_environment(App &a){
    return {{"DESK_BASE_URL",sget(a.config,"base_url")},{"DESK_MODEL","model-test"},{"DESK_API_KEY","fake-test-key"},{"DESK_API_TYPE","openai"},{"NO_COLOR","1"}};
}
static void test_headless(){
    // --json: stdout carries exactly one result object; progress goes to stderr.
    {
        App a=fresh();json_object_put(a.config);a.config=config("headless-json","openai");
        fixture(&a,"/note.txt","note body",9);
        const auto first=model_tool("read","read_file",args("note.txt")),second=model_reply(nullptr,"JSON_DONE");
        const char *responses[]={first.c_str(),second.c_str()};
        pid_t server=fake_server(&a,responses,2);
        auto run=run_desk({"--json","--lang","en","--workspace",a.root,"-p","read the note"},model_environment(a),"");
        server_done(server);
        CHECK(run.status==0);
        CHECK(std::count(run.out.begin(),run.out.end(),'\n')==1);
        JsonPtr result(json_tokener_parse(run.out.c_str()));
        CHECK(result);
        CHECK(!strcmp(sget(result.get(),"status"),"completed"));
        CHECK(!strcmp(sget(result.get(),"reply"),"JSON_DONE"));
        CHECK(!strcmp(sget(result.get(),"model"),"model-test"));
        CHECK(alen(get(result.get(),"tools"))==1);
        J *tool=at(get(result.get(),"tools"),0);
        CHECK(!strcmp(sget(tool,"name"),"read_file")&&!strcmp(sget(tool,"target"),"note.txt")&&bget(tool,"ok"));
        CHECK(run.out.find("fake-test-key")==std::string::npos&&run.err.find("fake-test-key")==std::string::npos);
        app_cleanup(&a);
    }
    // Piped stdout and "-p -": only the raw final reply is printed on stdout.
    {
        App a=fresh();json_object_put(a.config);a.config=config("headless-text","openai");
        const auto reply=model_reply(nullptr,"# Heading\nTEXT_DONE");
        const char *responses[]={reply.c_str()};
        pid_t server=fake_server(&a,responses,1);
        auto run=run_desk({"--lang","en","--workspace",a.root,"-p","-"},model_environment(a),"summarize from stdin\n");
        server_done(server);
        CHECK(run.status==0);
        CHECK(run.out=="# Heading\nTEXT_DONE\n");
        JsonPtr request(capture(&a,0));J *messages=get(get(request.get(),"body"),"messages");
        CHECK(!strcmp(sget(at(messages,alen(messages)-1),"content"),"summarize from stdin"));
        app_cleanup(&a);
    }
    // Without a terminal an approval cannot be answered: the result says so.
    for(bool allowed:{false,true}){
        App a=fresh();json_object_put(a.config);a.config=config("headless-approval","openai");
        J *write=args("made.txt");put(write,"content",str("made"));put(write,"expected_sha256",nullptr);
        const auto action=model_tool("write","write_file",write),done=model_reply(nullptr,"WROTE");
        const char *responses[]={action.c_str(),done.c_str()};
        pid_t server=fake_server(&a,responses,allowed?2:1);
        std::vector<std::string> arguments{"--json","--lang","en","--workspace",a.root};
        if(allowed){arguments.push_back("--approval");arguments.push_back("edit");}
        arguments.push_back("-p");arguments.push_back("create made.txt");
        auto run=run_desk(arguments,model_environment(a),"");
        server_done(server);
        JsonPtr result(json_tokener_parse(run.out.c_str()));
        CHECK(result);
        const std::string path=std::string(a.root)+"/made.txt";
        if(allowed){
            CHECK(run.status==0&&!strcmp(sget(result.get(),"status"),"completed"));
            CHECK(access(path.c_str(),F_OK)==0);
        }else{
            CHECK(run.status==1&&!strcmp(sget(result.get(),"status"),"approval_required"));
            CHECK(strstr(sget(result.get(),"error"),"--approval"));
            CHECK(access(path.c_str(),F_OK)!=0);
        }
        app_cleanup(&a);
    }
    // Bad approval values are a usage error, and doctor reports in JSON.
    {
        App a=fresh();
        auto bad=run_desk({"--approval","maybe","-p","x"},{},"");
        CHECK(bad.status==2);
        auto doctor=run_desk({"doctor","--json","--workspace",a.root},{},"");
        JsonPtr report(json_tokener_parse(doctor.out.c_str()));
        CHECK(report);
        CHECK(!bget(report.get(),"model_configured")&&bget(report.get(),"workspace_writable"));
        CHECK(!strcmp(sget(report.get(),"version"),VERSION));
        app_cleanup(&a);
    }
}
static void test_provider_presets(){
    App a=fresh();
    const char *responses[]={"{\"data\":[{\"id\":\"alpha-model\"},{\"id\":\"beta-model\"}]}"};
    J *probe=config("probe","openai");json_object_put(a.config);a.config=probe;
    pid_t server=fake_server(&a,responses,1);
    const std::string base=sget(a.config,"base_url");
    int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    std::string output;
    CHECK(terminal_until(master,output,"First run: choose Add model"));
    CHECK(terminal_until(master,output,"Select a number:"));
    output.clear();type_terminal(master,output,"3\n");
    CHECK(terminal_until(master,output,"[9] Ollama (local)"));
    CHECK(output.find("[3] DeepSeek")!=std::string::npos&&output.find("[1] OpenAI API")!=std::string::npos);
    CHECK(terminal_until(master,output,"Select a number:"));
    output.clear();type_terminal(master,output,"1\n");
    CHECK(terminal_until(master,output,"[model-1]"));
    output.clear();type_terminal(master,output,"\n");
    CHECK(terminal_until(master,output,"API URL ["));
    output.clear();type_terminal(master,output,base+"\n");
    CHECK(terminal_until(master,output,"API Key"));
    output.clear();type_terminal(master,output,"fake-preset-key\n");
    CHECK(terminal_until(master,output,"[2] beta-model"));
    CHECK(terminal_until(master,output,"Select a number:"));
    output.clear();type_terminal(master,output,"2\n");
    CHECK(terminal_until(master,output,"Save without testing"));
    CHECK(terminal_until(master,output,"Select a number:"));
    output.clear();type_terminal(master,output,"2\n");
    CHECK(terminal_until(master,output,"Saved and selected this model."));
    CHECK(terminal_until(master,output,"Select a number:"));
    type_terminal(master,output,"0\n");
    CHECK(reap_pty(master,child,&output)==0);
    server_done(server);
    App saved{};CHECK(!store_load(&saved));
    J *profile=get(get(saved.store,"profiles"),"model-1");
    CHECK(profile);
    CHECK(!strcmp(sget(profile,"model"),"beta-model"));
    CHECK(!strcmp(sget(profile,"api_key"),"fake-preset-key"));
    CHECK(output.find("fake-preset-key")==std::string::npos);
    app_cleanup(&saved);
    app_cleanup(&a);
}
static void test_code_outline_and_project_guide(){
    App a=fresh();
    json_object_put(a.config);a.config=config("outline-test","openai");

    auto write_rel=[&](const char *rel,const char *content){
        std::string s=std::string(a.root)+"/"+rel;
        auto slash=s.rfind('/');
        if(slash!=std::string::npos)CHECK(!mkdirs(s.substr(0,slash).c_str(),0700));
        CHECK(!atomic_file(s.c_str(),content,strlen(content),0600));
    };
    write_rel("src/engine.cpp",
        "// void commented_out() {}\n"
        "/*\nclass FakeBlockClass {\nvoid fake_method() {}\n};\n*/\n"
        "namespace core {\n"
        "struct Config {\n"
        "    int run_step(const char *literal = \"void not_a_fn() {}\") {\n"
        "        if (literal) { return 1; }\n"
        "        return 0;\n"
        "    }\n"
        "};\n"
        "int compute_total(int x,\n"
        "                  int y)\n"
        "{\n"
        "    return x + y;\n"
        "}\n"
        "}\n");
    write_rel("src/worker.py",
        "# def ignored_comment(): pass\n"
        "\"\"\"\n"
        "def ignored_docstring():\n"
        "    pass\n"
        "\"\"\"\n"
        "class TaskRunner:\n"
        "    async def execute(self, item):\n"
        "        return item\n"
        "\n"
        "def top_helper(flag=True):\n"
        "    return flag\n");
    write_rel("src/ui.ts",
        "export interface ViewProps { width: number; }\n"
        "export class PanelView {\n"
        "    render(): string { return `function fake() {}`; }\n"
        "}\n"
        "export const formatBadge = (n: number) => `${n}`;\n");

    JsonPtr one_args(args("src/engine.cpp"));
    JsonPtr one(tool_call(&a,"code_outline",one_args.get()));
    CHECK(!tool_failed(one.get()));
    std::string one_json=json_object_to_json_string_ext(one.get(),JSON_C_TO_STRING_PLAIN);
    CHECK(one_json.find("namespace core")!=std::string::npos);
    CHECK(one_json.find("struct Config")!=std::string::npos);
    CHECK(one_json.find("run_step")!=std::string::npos);
    CHECK(one_json.find("compute_total")!=std::string::npos);
    CHECK(one_json.find("commented_out")==std::string::npos);
    CHECK(one_json.find("FakeBlockClass")==std::string::npos);
    CHECK(one_json.find("not_a_fn")==std::string::npos);

    JsonPtr all_args(args("."));
    JsonPtr all(tool_call(&a,"code_outline",all_args.get()));
    CHECK(!tool_failed(all.get()));
    CHECK(iget(all.get(),"files_scanned",0)==3);
    std::string all_json=json_object_to_json_string_ext(all.get(),JSON_C_TO_STRING_PLAIN);
    CHECK(all_json.find("TaskRunner")!=std::string::npos);
    CHECK(all_json.find("top_helper")!=std::string::npos);
    CHECK(all_json.find("ignored_docstring")==std::string::npos);
    CHECK(all_json.find("ViewProps")!=std::string::npos);
    CHECK(all_json.find("formatBadge")!=std::string::npos);

    // Project guide: absent -> no system message; present -> injected on OpenAI and Anthropic.
    {
        J *u=obj();put(u,"role",str("user"));put(u,"content",str("hello"));add(a.messages,u);
        JsonPtr defs(schemas(&a));
        JsonPtr ctx_empty(context(&a,defs.get()));
        CHECK(alen(ctx_empty.get())==1);
        CHECK(!strcmp(sget(at(ctx_empty.get(),0),"role"),"user"));

        write_rel("AGENTS.md","Always run `make check-fast` before finishing.\n");
        JsonPtr ctx_guided(context(&a,defs.get()));
        CHECK(alen(ctx_guided.get())==2);
        CHECK(!strcmp(sget(at(ctx_guided.get(),0),"role"),"system"));
        CHECK(strstr(sget(at(ctx_guided.get(),0),"content"),"make check-fast")!=nullptr);

        JsonPtr anth_cfg(config("anth","anthropic"));
        JsonPtr native(native_payload(anth_cfg.get(),ctx_guided.get(),defs.get()));
        CHECK(strstr(sget(native.get(),"system"),"make check-fast")!=nullptr);
        CHECK(alen(get(native.get(),"messages"))==1);

        setenv("DESK_NO_PROJECT_GUIDE","1",1);
        JsonPtr ctx_off(context(&a,defs.get()));
        unsetenv("DESK_NO_PROJECT_GUIDE");
        CHECK(alen(ctx_off.get())==1);
        reset_session(&a);
    }

    // delegate_research runs an isolated read-only subagent without polluting parent messages.
    {
        const char *sub_replies[]={
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"sub-1\",\"type\":\"function\",\"function\":{\"name\":\"code_outline\",\"arguments\":\"{\\\"path\\\":\\\"src/engine.cpp\\\"}\"}}]}}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"tool_calls\"}],\"usage\":{\"total_tokens\":19}}\n\n"
            "data: [DONE]\n\n",
            "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"compute_total is at src/engine.cpp:14.\"}}]}\n\n"
            "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"total_tokens\":23}}\n\n"
            "data: [DONE]\n\n"
        };
        pid_t srv=fake_server(&a,sub_replies,2);
        JsonPtr r_args(obj());
        put(r_args.get(),"question",str("Where is compute_total defined?"));
        put(r_args.get(),"path",str("src"));
        a.task_tokens=0;
        JsonPtr r_res(tool_call(&a,"delegate_research",r_args.get()));
        server_done(srv);
        CHECK(!tool_failed(r_res.get()));
        CHECK(strstr(sget(r_res.get(),"findings"),"src/engine.cpp:14")!=nullptr);
        CHECK(iget(r_res.get(),"steps",0)==2);
        CHECK(alen(get(r_res.get(),"inspected"))==1);
        CHECK(alen(a.messages)==0);
        CHECK(a.task_tokens==42);
    }
    app_cleanup(&a);
}

static void test_mentions_paste_and_multimodal(){
    App a=fresh();
    json_object_put(a.config);a.config=config("mention-test","openai");

    {
        size_t at_pos=99;std::string q;
        CHECK(!find_at_mention("contact dev@example.com",23,at_pos,q));
        CHECK(find_at_mention("check @src/ap",13,at_pos,q)&&at_pos==6&&q=="src/ap");
        CHECK(find_at_mention("@",1,at_pos,q)&&at_pos==0&&q.empty());
        CHECK(find_at_mention("open @\"my folder/fi",19,at_pos,q)&&at_pos==5&&q=="my folder/fi");
    }

    auto write_bin=[&](const char *rel,const void *data,size_t len){
        std::string s=std::string(a.root)+"/"+rel;
        auto slash=s.rfind('/');
        if(slash!=std::string::npos)CHECK(!mkdirs(s.substr(0,slash).c_str(),0700));
        CHECK(!atomic_file(s.c_str(),data,len,0600));
    };
    const char *notes="alpha_line_1\nbeta_line_2\ngamma_line_3\ndelta_line_4\n";
    write_bin("docs/uniq_notes.txt",notes,strlen(notes));
    write_bin(".env","SECRET=1\n",9);
    static const unsigned char tiny_png[]={
        0x89,'P','N','G',0x0d,0x0a,0x1a,0x0a,
        0x00,0x00,0x00,0x0d,'I','H','D','R',0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,
        0x08,0x02,0x00,0x00,0x00,0x90,0x77,0x53,0xde,0x00,0x00,0x00,0x00,'I','E','N','D',0xae,0x42,0x60,0x82
    };
    write_bin("assets/pixel.png",tiny_png,sizeof tiny_png);

    auto completed=workspace_complete_files(a,"uniq",8);
    CHECK(completed.size()==1&&completed[0]=="docs/uniq_notes.txt");
    auto env_try=workspace_complete_files(a,"env",8);
    CHECK(env_try.empty());

    {
        JsonPtr msg(prepare_user_message(&a,"Compare @docs/uniq_notes.txt:2-3 with @assets/pixel.png, ignore @.env."));
        CHECK(user_message_text(msg.get())=="Compare @docs/uniq_notes.txt:2-3 with @assets/pixel.png, ignore @.env.");
        J *blocks=get(msg.get(),"content");
        CHECK(json_object_is_type(blocks,json_type_array)&&alen(blocks)==2);
        const char *text_part=sget(at(blocks,0),"text");
        CHECK(strstr(text_part,"beta_line_2\ngamma_line_3")!=nullptr);
        CHECK(strstr(text_part,"alpha_line_1")==nullptr);
        CHECK(strstr(text_part,"SECRET")==nullptr);
        CHECK(!strcmp(sget(at(blocks,1),"type"),"image_url"));

        JsonPtr anth_cfg(config("anth-mm","anthropic"));
        JsonPtr msgs(arr());add(msgs.get(),copy(msg.get()));
        JsonPtr native(native_payload(anth_cfg.get(),msgs.get(),nullptr));
        J *nblocks=get(at(get(native.get(),"messages"),0),"content");
        CHECK(alen(nblocks)==2);
        CHECK(!strcmp(sget(at(nblocks,1),"type"),"image"));
        CHECK(!strcmp(sget(get(at(nblocks,1),"source"),"media_type"),"image/png"));
    }

    const char *pty_replies[]={
        "data: {\"choices\":[{\"index\":0,\"delta\":{\"content\":\"ALL_MULTIMODAL_OK\"}}]}\n\n"
        "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}],\"usage\":{\"total_tokens\":30}}\n\n"
        "data: [DONE]\n\n"
    };
    pid_t srv=fake_server(&a,pty_replies,1);
    CHECK(!save_config(&a,a.config));
    std::string clip_src=std::string(a.root)+"/assets/pixel.png";

    int master=-1;
    winsize window{24,90,0,0};
    pid_t child=forkpty(&master,nullptr,nullptr,&window);
    CHECK(child>=0);
    if(!child){
        setenv("TERM","xterm-256color",1);
        setenv("NO_COLOR","1",1);
        setenv("DESK_CLIPBOARD_IMAGE_FILE",clip_src.c_str(),1);
        unsetenv("DESK_NO_ARROW");
        unsetenv("DESK_NO_COMPOSER");
        execl("./build/desk","desk","--lang","en","--workspace",a.root,"chat",static_cast<char *>(nullptr));
        _exit(127);
    }
    std::string output;
    CHECK(terminal_until(master,output,"\033[?2004h"));
    output.clear();
    // Stage clipboard image via Ctrl+V hotkey.
    type_terminal(master,output,"\x16");
    CHECK(terminal_until(master,output,"Attached clipboard image"));
    output.clear();
    type_terminal(master,output,"@uniq\t");
    CHECK(terminal_until(master,output,"@docs/uniq_notes.txt "));
    output.clear();
    type_terminal(master,output,"\033[200~PASTE_ROW_A\nPASTE_ROW_B\033[201~");
    CHECK(terminal_until(master,output,"[Paste #1 · 2 lines]"));
    output.clear();
    type_terminal(master,output,"\r");
    CHECK(terminal_until(master,output,"ALL_MULTIMODAL_OK"));

    type_terminal(master,output,"\x04");
    CHECK(terminal_until(master,output,"0. Main menu"));
    type_terminal(master,output,"0\r");
    CHECK(terminal_until(master,output,"0. Exit"));
    type_terminal(master,output,"0\r");
    CHECK(reap_pty(master,child)==0);
    server_done(srv);

    JsonPtr cap(capture(&a,0));
    J *sent_msg=at(get(get(cap.get(),"body"),"messages"),0);
    CHECK(!get(sent_msg,"_display"));
    J *sent_blocks=get(sent_msg,"content");
    CHECK(json_object_is_type(sent_blocks,json_type_array)&&alen(sent_blocks)==2);
    const char *sent_text=sget(at(sent_blocks,0),"text");
    CHECK(strstr(sent_text,"PASTE_ROW_A\nPASTE_ROW_B")!=nullptr);
    CHECK(strstr(sent_text,"alpha_line_1")!=nullptr);
    CHECK(!strcmp(sget(at(sent_blocks,1),"type"),"image_url"));
    app_cleanup(&a);
}

static void test_v1_13_features(){
    App a=fresh();
    a.yes=true;
    a.desktop=true;

    // 1. Multi-edit turn undo (/undo) + newly created file removal + cache GC.
    std::string tracked_path=std::string(a.root)+"/tracked.txt";
    CHECK(!atomic_file(tracked_path.c_str(),"original-v1\n",12,0644));
    a.turn_id="0123456789abcdef0123456789abcdef";
    {
        JsonPtr r1(tool_call(&a,"read_file",JsonPtr(args("tracked.txt")).get()));
        JsonPtr e1(args("tracked.txt"));
        put(e1.get(),"old_text",str("original-v1"));
        put(e1.get(),"new_text",str("mid-v2"));
        put(e1.get(),"expected_sha256",str(sget(r1.get(),"sha256")));
        JsonPtr w1(tool_call(&a,"edit_file",e1.get()));
        CHECK(!get(w1.get(),"error"));

        JsonPtr e2(args("tracked.txt"));
        put(e2.get(),"old_text",str("mid-v2"));
        put(e2.get(),"new_text",str("final-v3"));
        put(e2.get(),"expected_sha256",str(sget(w1.get(),"sha256")));
        JsonPtr w2(tool_call(&a,"edit_file",e2.get()));
        CHECK(!get(w2.get(),"error"));

        JsonPtr cr(args("brand_new.txt"));
        put(cr.get(),"content",str("created in turn\n"));
        put(cr.get(),"expected_sha256",nullptr);
        JsonPtr wc(tool_call(&a,"write_file",cr.get()));
        CHECK(!get(wc.get(),"error"));
    }
    std::string bdir=std::string(a.root)+"/.desk-agent/backups";
    CHECK(!mkdirs(bdir.c_str(),0700));
    for(int i=0;i<38;i++){
        char orphan[PATH_MAX];
        snprintf(orphan,sizeof orphan,"%s/orphan-%02d.bak",bdir.c_str(),i);
        CHECK(!atomic_file(orphan,"x",1,0600));
    }
    prune_workspace_cache(a);
    std::string undo_msg;
    // A directory in the restore target blocks one entry. Undo must remove
    // the new file now and retain the blocked entry for a later retry.
    CHECK(!unlink(tracked_path.c_str()));
    CHECK(!mkdir(tracked_path.c_str(),0700));
    CHECK(!undo_last_turn(a,undo_msg));
    CHECK(undo_msg.find("tracked.txt")!=std::string::npos);
    std::string created_path=std::string(a.root)+"/brand_new.txt";
    CHECK(!fs::exists(fs_path(created_path)));
    CHECK(!rmdir(tracked_path.c_str()));
    CHECK(!atomic_file(created_path.c_str(),"keep",4,0600));
    CHECK(undo_last_turn(a,undo_msg));
    char *restored=read_bytes(tracked_path.c_str(),1024,nullptr);
    CHECK(restored&&!strcmp(restored,"original-v1\n"));
    free(restored);
    CHECK(fs::exists(fs_path(created_path)));
    CHECK(!undo_last_turn(a,undo_msg));

    // 2. Regex search, file_pattern glob, and .gitignore filtering.
    CHECK(!atomic_file((std::string(a.root)+"/.gitignore").c_str(),"ignored_out/\n*.gen.cpp\n",23,0600));
    CHECK(!mkdirs((std::string(a.root)+"/ignored_out").c_str(),0700));
    CHECK(!mkdirs((std::string(a.root)+"/pkg").c_str(),0700));
    CHECK(!atomic_file((std::string(a.root)+"/ignored_out/skip.cpp").c_str(),"int target_fn_42() { return 0; }\n",33,0600));
    CHECK(!atomic_file((std::string(a.root)+"/pkg/auto.gen.cpp").c_str(),"int target_fn_99() { return 0; }\n",33,0600));
    CHECK(!atomic_file((std::string(a.root)+"/pkg/keep.cpp").c_str(),"int target_fn_77() { return 77; }\n",34,0600));
    CHECK(!atomic_file((std::string(a.root)+"/pkg/readme.md").c_str(),"target_fn_88 in markdown\n",25,0600));
    {
        JsonPtr sq(args("."));
        put(sq.get(),"query",str("target_fn_[0-9]+\\("));
        put(sq.get(),"regex",json_object_new_boolean(true));
        put(sq.get(),"file_pattern",str("*.{cpp,hpp}"));
        JsonPtr sr(tool_call(&a,"search_text",sq.get()));
        CHECK(!get(sr.get(),"error"));
        J *matches=get(sr.get(),"matches");
        CHECK(alen(matches)==1);
        CHECK(!strcmp(sget(at(matches,0),"path"),"pkg/keep.cpp"));

        JsonPtr bad_re(args("."));
        put(bad_re.get(),"query",str("[unclosed"));
        put(bad_re.get(),"regex",json_object_new_boolean(true));
        JsonPtr bad_res(tool_call(&a,"search_text",bad_re.get()));
        CHECK(get(bad_res.get(),"error"));

        auto completed=workspace_complete_files(a,"target",20);
        for(const auto &c:completed)CHECK(c.find("ignored_out")==std::string::npos&&c.find(".gen.cpp")==std::string::npos);
        auto keep_comp=workspace_complete_files(a,"keep",10);
        CHECK(!keep_comp.empty()&&keep_comp[0]=="pkg/keep.cpp");
    }

    // 3. Smarter shell approval granularity & session command rules.
    CHECK(safe_readonly_command("git status --short"));
    CHECK(safe_readonly_command("ls -la pkg"));
    CHECK(!safe_readonly_command("rg --pre=sh target ."));
    CHECK(!safe_readonly_command("git status && rm -rf /"));
    CHECK(!safe_readonly_command("cat pkg/keep.cpp | sh"));
    CHECK(!safe_readonly_command("git checkout -f"));
    {
        a.yes=false;
        a.auto_edit=true;
        JsonPtr ro_cmd(obj());
        put(ro_cmd.get(),"command",str("pwd"));
        JsonPtr ro_res(tool_call(&a,"run_shell",ro_cmd.get()));
        CHECK(!tool_failed(ro_res.get())&&iget(ro_res.get(),"exit_code",-1)==0);

        JsonPtr rw_cmd(obj());
        put(rw_cmd.get(),"command",str("echo nope > blocked.txt"));
        JsonPtr rw_res(tool_call(&a,"run_shell",rw_cmd.get()));
        CHECK(bget(rw_res.get(),"denied"));

        remember_allowed_command(a,"git commit");
        CHECK(command_allowed_in_session(a,"git commit -m 'msg'"));
        CHECK(!command_allowed_in_session(a,"git push --force"));
        a.auto_edit=false;
        a.yes=true;
    }

    // 4. web_fetch SSRF blocks + local HTTP HTML readable text extraction.
    {
        for(const char *blocked_url:{"http://example.com/doc","https://10.0.0.1/secret","https://192.168.1.1/","https://169.254.169.254/latest/meta-data"}){
            JsonPtr fq(obj());
            put(fq.get(),"url",str(blocked_url));
            JsonPtr fr(tool_call(&a,"web_fetch",fq.get()));
            CHECK(get(fr.get(),"error"));
        }
        int sfd=socket(AF_INET,SOCK_STREAM,0);
        CHECK(sfd>=0);
        sockaddr_in addr{};
        addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        CHECK(bind(sfd,reinterpret_cast<sockaddr *>(&addr),sizeof addr)==0);
        socklen_t slen=sizeof addr;
        CHECK(getsockname(sfd,reinterpret_cast<sockaddr *>(&addr),&slen)==0);
        CHECK(listen(sfd,2)==0);
        pid_t web_pid=fork();
        if(web_pid==0){
            int cfd=accept(sfd,nullptr,nullptr);
            if(cfd>=0){
                char req[2048];
                ssize_t r=read(cfd,req,sizeof req);(void)r;
                const char *html="<html><head><title>Ignored</title><script>evil_js()</script></head>"
                                 "<body><h1>API Guide</h1><p>Hello &amp; Welcome to DeskCode.</p></body></html>";
                char resp[1024];
                int n=snprintf(resp,sizeof resp,
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                    strlen(html),html);
                ssize_t w=write(cfd,resp,static_cast<size_t>(n));(void)w;
                close(cfd);
            }
            close(sfd);
            _exit(0);
        }
        close(sfd);
        std::string local_url="http://127.0.0.1:"+std::to_string(ntohs(addr.sin_port))+"/guide";
        JsonPtr fq(obj());
        put(fq.get(),"url",str(local_url.c_str()));
        JsonPtr fr(tool_call(&a,"web_fetch",fq.get()));
        int wst=0;waitpid(web_pid,&wst,0);
        CHECK(!get(fr.get(),"error"));
        CHECK(strstr(sget(fr.get(),"content"),"# API Guide"));
        CHECK(strstr(sget(fr.get(),"content"),"Hello & Welcome to DeskCode."));
        CHECK(!strstr(sget(fr.get(),"content"),"evil_js"));
    }

    // 5. desktop_screenshot multimodal wiring (OpenAI follow-up vision + Anthropic native image tool_result).
    {
        static const unsigned char png_1x1[]={
            0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0x00,0x00,0x00,0x0d,0x49,0x48,0x44,0x52,
            0x00,0x00,0x00,0x01,0x00,0x00,0x00,0x01,0x08,0x06,0x00,0x00,0x00,0x1f,0x15,0xc4,
            0x89,0x00,0x00,0x00,0x0a,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0x00,0x01,0x00,0x00,
            0x05,0x00,0x01,0x0d,0x0a,0x2d,0xb4,0x00,0x00,0x00,0x00,0x49,0x45,0x4e,0x44,0xae,
            0x42,0x60,0x82
        };
        std::string shot_file=std::string(a.root)+"/mock-screen.png";
        CHECK(!atomic_file(shot_file.c_str(),png_1x1,sizeof png_1x1,0600));
        setenv("DESK_DESKTOP_SCREENSHOT_FILE",shot_file.c_str(),1);
        reset_session(&a);
        CHECK(!workflow(&a,{model_tool("shot1","desktop_screenshot",obj()),model_reply(nullptr,"Screenshot inspected.")}));
        JsonPtr req2(capture(&a,1));
        J *wire_msgs=get(get(req2.get(),"body"),"messages");
        CHECK(alen(wire_msgs)==4);
        CHECK(!strcmp(sget(at(wire_msgs,2),"role"),"tool"));
        CHECK(strstr(sget(at(wire_msgs,2),"content"),"[Image attached below]"));
        CHECK(!strcmp(sget(at(wire_msgs,3),"role"),"user"));
        J *vcontent=get(at(wire_msgs,3),"content");
        CHECK(json_object_is_type(vcontent,json_type_array)&&alen(vcontent)==2);
        CHECK(!strcmp(sget(at(vcontent,1),"type"),"image_url"));

        JsonPtr anth(native_payload(a.config,a.messages,nullptr));
        J *anth_msgs=get(anth.get(),"messages");
        // user(0), assistant(1), user(tool_result)(2), assistant(3)
        J *tr_block=at(get(at(anth_msgs,2),"content"),0);
        CHECK(!strcmp(sget(tr_block,"type"),"tool_result"));
        J *tr_inner=get(tr_block,"content");
        CHECK(json_object_is_type(tr_inner,json_type_array)&&alen(tr_inner)==2);
        CHECK(!strcmp(sget(at(tr_inner,1),"type"),"image"));
        CHECK(!strcmp(sget(get(at(tr_inner,1),"source"),"media_type"),"image/png"));
        unsetenv("DESK_DESKTOP_SCREENSHOT_FILE");
    }

    // 6. Active /compact model-driven context summarization.
    {
        const char *compact_resp=
            "{\"choices\":[{\"finish_reason\":\"stop\",\"message\":{\"role\":\"assistant\","
            "\"content\":\"Goals: implement 1.13.0\\nFiles: pkg/keep.cpp\"}}]}";
        const char *resps[]={compact_resp};
        pid_t srv=fake_server(&a,resps,1);
        std::string summary;
        CHECK(compact_conversation(a,summary));
        server_done(srv);
        CHECK(summary.find("pkg/keep.cpp")!=std::string::npos);
        CHECK(alen(a.messages)==2);
    }

    app_cleanup(&a);
}

static void test_v1_14_features(){
    App a=fresh();
    a.yes=true;

    // 1. Linux Landlock kernel filesystem sandbox.
    if(sandbox_supported()&&access("/dev/shm",W_OK)==0){
        char shm_probe[128];
        snprintf(shm_probe,sizeof shm_probe,"/dev/shm/desk-landlock-probe-%d",static_cast<int>(getpid()));
        unlink(shm_probe);
        a.sandbox=true;
        std::string inside_cmd="echo ok > landlock_inside.txt";
        JsonPtr r_in(run_command(&a,inside_cmd.c_str(),5000));
        CHECK(iget(r_in.get(),"exit_code",-1)==0);
        CHECK(fs::exists(fs_path(std::string(a.root)+"/landlock_inside.txt")));

        std::string outside_cmd=std::string("echo blocked > ")+shm_probe;
        JsonPtr r_out(run_command(&a,outside_cmd.c_str(),5000));
        CHECK(iget(r_out.get(),"exit_code",0)!=0);
        CHECK(access(shm_probe,F_OK)!=0);

        // Disabling sandbox allows the exact same write, proving Landlock was the blocker.
        a.sandbox=false;
        JsonPtr r_nosb(run_command(&a,outside_cmd.c_str(),5000));
        CHECK(iget(r_nosb.get(),"exit_code",-1)==0);
        CHECK(access(shm_probe,F_OK)==0);
        unlink(shm_probe);
        a.sandbox=true;

        setenv("DESK_NO_SANDBOX","1",1);
        JsonPtr r_env_nosb(run_command(&a,outside_cmd.c_str(),5000));
        unsetenv("DESK_NO_SANDBOX");
        CHECK(iget(r_env_nosb.get(),"exit_code",-1)==0);
        CHECK(access(shm_probe,F_OK)==0);
        unlink(shm_probe);
    }

    // 2. Atomic multi-hunk multi_edit + rollback on failed hunk + /undo.
    std::string me_path=std::string(a.root)+"/module.cpp";
    const char *initial_src="int alpha() { return 1; }\nint beta() { return 2; }\nint gamma() { return 3; }\n";
    CHECK(!atomic_file(me_path.c_str(),initial_src,strlen(initial_src),0644));
    a.turn_id="fedcba9876543210fedcba9876543210";
    {
        JsonPtr rf(tool_call(&a,"read_file",JsonPtr(args("module.cpp")).get()));
        const char *sha=sget(rf.get(),"sha256");

        // Second hunk missing -> entire multi_edit fails atomically without touching disk.
        JsonPtr bad_me(args("module.cpp"));
        put(bad_me.get(),"expected_sha256",str(sha));
        J *bad_edits=arr(),*bh1=obj(),*bh2=obj();
        put(bh1,"old_text",str("return 1;"));put(bh1,"new_text",str("return 10;"));add(bad_edits,bh1);
        put(bh2,"old_text",str("return 999;"));put(bh2,"new_text",str("return 20;"));add(bad_edits,bh2);
        put(bad_me.get(),"edits",bad_edits);
        JsonPtr bad_res(tool_call(&a,"multi_edit",bad_me.get()));
        CHECK(get(bad_res.get(),"error")&&iget(bad_res.get(),"failed_edit_index",-1)==1);
        char *unchanged=read_bytes(me_path.c_str(),1024,nullptr);
        CHECK(unchanged&&!strcmp(unchanged,initial_src));
        free(unchanged);

        // Valid 3-hunk multi_edit succeeds in one transaction.
        JsonPtr ok_me(args("module.cpp"));
        put(ok_me.get(),"expected_sha256",str(sha));
        J *edits=arr();
        for(auto [o,n]:{std::pair{"return 1;","return 10;"},std::pair{"return 2;","return 20;"},std::pair{"return 3;","return 30;"}}){
            J *h=obj();put(h,"old_text",str(o));put(h,"new_text",str(n));add(edits,h);
        }
        put(ok_me.get(),"edits",edits);
        JsonPtr ok_res(tool_call(&a,"multi_edit",ok_me.get()));
        CHECK(!get(ok_res.get(),"error")&&iget(ok_res.get(),"edits_applied",0)==3);
        char *after_me=read_bytes(me_path.c_str(),1024,nullptr);
        CHECK(after_me&&strstr(after_me,"return 10;")&&strstr(after_me,"return 20;")&&strstr(after_me,"return 30;"));
        free(after_me);

        std::string undo_summary;
        CHECK(undo_last_turn(a,undo_summary));
        char *reverted=read_bytes(me_path.c_str(),1024,nullptr);
        CHECK(reverted&&!strcmp(reverted,initial_src));
        free(reverted);
    }

    // 3. Lifecycle hooks (post_edit & pre_shell) + custom slash commands (.desk-agent & .claude).
    {
        CHECK(!mkdirs(a.config_dir,0700));
        const char *hooks_json=
            "{"
            "\"post_edit\":[{\"pattern\":\"*.cpp\",\"command\":\"echo HOOK_LINT:$DESK_HOOK_FILE\"}],"
            "\"pre_shell\":\"if echo \\\"$DESK_HOOK_CMD\\\" | grep -q BLOCK_ME_NOW; then echo 'Blocked by pre_shell policy'; exit 3; fi\""
            "}";
        CHECK(!atomic_file((std::string(a.config_dir)+"/hooks.json").c_str(),hooks_json,strlen(hooks_json),0600));

        JsonPtr rf(tool_call(&a,"read_file",JsonPtr(args("module.cpp")).get()));
        JsonPtr ed(args("module.cpp"));
        put(ed.get(),"expected_sha256",str(sget(rf.get(),"sha256")));
        put(ed.get(),"old_text",str("return 1;"));
        put(ed.get(),"new_text",str("return 11;"));
        JsonPtr ed_res(tool_call(&a,"edit_file",ed.get()));
        CHECK(!get(ed_res.get(),"error"));
        CHECK(iget(ed_res.get(),"hook_exit_code",-1)==0);
        CHECK(strstr(sget(ed_res.get(),"hook_output"),"HOOK_LINT:module.cpp")!=nullptr);

        JsonPtr sh_bad(obj());
        put(sh_bad.get(),"command",str("echo BLOCK_ME_NOW"));
        JsonPtr sh_bad_res(tool_call(&a,"run_shell",sh_bad.get()));
        CHECK(get(sh_bad_res.get(),"error")&&strstr(sget(sh_bad_res.get(),"error"),"Blocked by pre_shell policy"));
        std::error_code ec;
        fs::remove(fs_path(std::string(a.config_dir)+"/hooks.json"),ec);

        CHECK(!mkdirs((std::string(a.root)+"/.deskcode/commands").c_str(),0700));
        CHECK(!mkdirs((std::string(a.root)+"/.desk-agent/commands").c_str(),0700));
        const char *rev_tpl="# Audit target\nAudit $ARGUMENTS for thread safety and leaks.";
        const char *rel_tpl="Generate bilingual release summary.";
        CHECK(!atomic_file((std::string(a.root)+"/.deskcode/commands/audit.md").c_str(),rev_tpl,strlen(rev_tpl),0600));
        CHECK(!atomic_file((std::string(a.root)+"/.desk-agent/commands/ship.md").c_str(),rel_tpl,strlen(rel_tpl),0600));
        CHECK(!atomic_file((std::string(a.root)+"/.deskcode/commands/sandbox.md").c_str(),rel_tpl,strlen(rel_tpl),0600));
        auto cmds=list_custom_commands(a);
        CHECK(cmds.size()==2);
        std::string expanded,label;
        CHECK(!expand_custom_command(a,"/sandbox",expanded,label));
        CHECK(!expand_custom_command(a,"/sandbox off",expanded,label));
        CHECK(expand_custom_command(a,"/audit src/mcp.cpp",expanded,label));
        CHECK(label=="/audit src/mcp.cpp");
        CHECK(expanded.find("Audit src/mcp.cpp for thread safety and leaks.")!=std::string::npos);

        auto help_zh=run_desk({"--help"},{},"");
        auto help_en=run_desk({"--lang","en","--help"},{},"");
        CHECK(help_zh.status==0&&help_zh.out.find("--no-sandbox")!=std::string::npos);
        CHECK(help_en.status==0&&help_en.out.find("--no-sandbox")!=std::string::npos);
#ifdef __linux__
        JsonPtr tool_list(schemas(&a));
        bool shell_mentions_landlock=false;
        for(size_t i=0;i<alen(tool_list.get());i++){
            J *fn=get(at(tool_list.get(),i),"function");
            if(!strcmp(sget(fn,"name"),"run_shell")&&strstr(sget(fn,"description"),"Landlock"))
                shell_mentions_landlock=true;
        }
        CHECK(shell_mentions_landlock);
#endif

        CHECK(!atomic_file((std::string(a.root)+"/deskcode.md").c_str(),"Follow DESKCODE native rules.\n",30,0600));
        std::string guide=load_project_guide(a);
        CHECK(guide.find("Follow DESKCODE native rules.")!=std::string::npos);
    }

    // 4. Built-in web_search (DuckDuckGo HTML uddg unwrapping via local test server).
    {
        int sfd=socket(AF_INET,SOCK_STREAM,0);
        CHECK(sfd>=0);
        sockaddr_in addr{};
        addr.sin_family=AF_INET;
        addr.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
        CHECK(bind(sfd,reinterpret_cast<sockaddr *>(&addr),sizeof addr)==0);
        socklen_t slen=sizeof addr;
        CHECK(getsockname(sfd,reinterpret_cast<sockaddr *>(&addr),&slen)==0);
        CHECK(listen(sfd,2)==0);
        pid_t spid=fork();
        if(spid==0){
            int cfd=accept(sfd,nullptr,nullptr);
            if(cfd>=0){
                char req[2048];ssize_t r=read(cfd,req,sizeof req);(void)r;
                const char *ddg_html=
                    "<html><body>"
                    "<div class=\"result\"><a class=\"result__a\" href=\"//duckduckgo.com/l/?uddg=https%3A%2F%2Fman7.org%2Flinux%2Fman-pages%2Fman7%2Flandlock.7.html&amp;rut=123\">landlock(7) - Linux manual page</a>"
                    "<a class=\"result__snippet\">Unprivileged sandboxing &amp; access control.</a></div>"
                    "</body></html>";
                char resp[1024];
                int n=snprintf(resp,sizeof resp,
                    "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %zu\r\nConnection: close\r\n\r\n%s",
                    strlen(ddg_html),ddg_html);
                ssize_t w=write(cfd,resp,static_cast<size_t>(n));(void)w;
                close(cfd);
            }
            close(sfd);
            _exit(0);
        }
        close(sfd);
        std::string search_ep="http://127.0.0.1:"+std::to_string(ntohs(addr.sin_port))+"/search";
        setenv("DESK_SEARCH_URL",search_ep.c_str(),1);
        JsonPtr ws_args(obj());
        put(ws_args.get(),"query",str("linux landlock syscall"));
        JsonPtr ws_res(tool_call(&a,"web_search",ws_args.get()));
        unsetenv("DESK_SEARCH_URL");
        int wst=0;waitpid(spid,&wst,0);
        CHECK(!get(ws_res.get(),"error"));
        J *items=get(ws_res.get(),"results");
        CHECK(alen(items)==1);
        CHECK(!strcmp(sget(at(items,0),"url"),"https://man7.org/linux/man-pages/man7/landlock.7.html"));
        CHECK(strstr(sget(at(items,0),"snippet"),"Unprivileged sandboxing & access control."));
    }

    // 5. Anthropic native Prompt Caching (cache_control breakpoints + HTTP 400 fallback).
    {
        JsonPtr anth_cfg(config("anth-cache-test","anthropic"));
        JsonPtr defs(schemas(&a));
        JsonPtr msgs(arr());
        J *sys=obj();put(sys,"role",str("system"));put(sys,"content",str("Project guide"));add(msgs.get(),sys);
        J *usr=obj();put(usr,"role",str("user"));put(usr,"content",str("Hello Claude"));add(msgs.get(),usr);
        JsonPtr cached(native_payload(anth_cfg.get(),msgs.get(),defs.get(),true));
        J *sys_blocks=get(cached.get(),"system");
        CHECK(json_object_is_type(sys_blocks,json_type_array)&&alen(sys_blocks)==1);
        CHECK(!strcmp(sget(get(at(sys_blocks,0),"cache_control"),"type"),"ephemeral"));
        J *tool_defs=get(cached.get(),"tools");
        CHECK(!strcmp(sget(get(at(tool_defs,alen(tool_defs)-1),"cache_control"),"type"),"ephemeral"));
        J *hist_msgs=get(cached.get(),"messages");
        J *u_blocks=get(at(hist_msgs,0),"content");
        CHECK(!strcmp(sget(get(at(u_blocks,alen(u_blocks)-1),"cache_control"),"type"),"ephemeral"));
    }

    app_cleanup(&a);
}

static void test_v1_15_features(){
    // 1. Usage is split the way providers bill it, and priced only when asked.
    {
        JsonPtr priced(config("priced","openai"));
        put(priced.get(),"price_input",json_object_new_double(2));
        put(priced.get(),"price_output",json_object_new_double(8));
        put(priced.get(),"price_cache_read",json_object_new_double(0.5));
        put(priced.get(),"price_currency",str("CNY"));
        JsonPtr openai(json_tokener_parse(R"({"prompt_tokens":100,"completion_tokens":20,"total_tokens":120,"prompt_tokens_details":{"cached_tokens":60}})"));
        TokenUsage used=usage_from_response(openai.get(),false,priced.get());
        CHECK(used.input==40&&used.cache_read==60&&used.output==20&&used.total()==120);
        CHECK(used.priced&&used.cost>0.000269&&used.cost<0.000271);
        CHECK(usage_cost_label(priced.get(),used.cost)=="≈¥0.0003");
        JsonPtr plain(config("plain","openai"));
        used=usage_from_response(openai.get(),false,plain.get());
        CHECK(!used.priced&&used.cost==0);
        JsonPtr deepseek(json_tokener_parse(R"({"prompt_tokens":50,"completion_tokens":5,"prompt_cache_hit_tokens":30})"));
        used=usage_from_response(deepseek.get(),false,plain.get());
        CHECK(used.input==20&&used.cache_read==30&&used.output==5);
        JsonPtr total_only(json_tokener_parse(R"({"total_tokens":17})"));
        used=usage_from_response(total_only.get(),false,plain.get());
        CHECK(used.total()==17);
        JsonPtr native(json_tokener_parse(R"({"input_tokens":10,"cache_creation_input_tokens":5,"cache_read_input_tokens":100,"output_tokens":7})"));
        used=usage_from_response(native.get(),true,plain.get());
        CHECK(used.input==15&&used.cache_read==100&&used.output==7&&used.total()==122);
        JsonPtr router(json_tokener_parse(R"({"prompt_tokens":10,"completion_tokens":2,"cost":0.0125})"));
        used=usage_from_response(router.get(),false,plain.get());
        CHECK(used.priced&&used.cost==0.0125&&usage_cost_label(plain.get(),used.cost)=="≈$0.013");
        // The worker reports both counters to the controller in one event.
        App wire;wire.turn_usage={1,2,3,0.5,true};wire.session_usage={10,20,30,1.25,true};
        const auto event=usage_event(wire);
        CHECK(event.size()==USAGE_EVENT_SIZE&&event[0]=='u');
        TokenUsage turn,session;parse_usage_event(event.data(),turn,session);
        CHECK(turn.input==1&&turn.output==2&&turn.cache_read==3&&turn.cost==0.5&&turn.priced);
        CHECK(session.input==10&&session.output==20&&session.cache_read==30&&session.cost==1.25&&session.priced);
    }
    // 2. todo_write keeps one checklist and renders it.
    {
        App a=fresh();
        JsonPtr list(json_tokener_parse(R"({"todos":[{"content":"Read the code","status":"completed"},{"content":"Fix the bug","status":"in_progress"},{"content":"Run tests","status":"pending"}]})"));
        JsonPtr result(tool_call(&a,"todo_write",list.get()));
        CHECK(!tool_failed(result.get())&&iget(result.get(),"total",0)==3&&iget(result.get(),"completed",0)==1);
        CHECK(alen(a.todos)==3&&todo_progress(a.todos)=="1/3");
        const std::string shown=todo_render(a.todos,false);
        CHECK(shown.find("1/3")!=std::string::npos&&shown.find("[x] Read the code")!=std::string::npos);
        CHECK(shown.find("[~] Fix the bug")!=std::string::npos&&shown.find("[ ] Run tests")!=std::string::npos);
        JsonPtr bad(json_tokener_parse(R"({"todos":[{"content":"x","status":"done"}]})"));
        result.reset(tool_call(&a,"todo_write",bad.get()));CHECK(tool_failed(result.get()));
        JsonPtr empty(json_tokener_parse(R"({"todos":[]})"));
        result.reset(tool_call(&a,"todo_write",empty.get()));CHECK(tool_failed(result.get()));
        CHECK(alen(a.todos)==3);
        app_cleanup(&a);
    }
    // 3. Plan mode offers only read-only tools and ends the turn with a plan.
    {
        App a=fresh();a.shell=true;a.desktop=true;a.plan_mode=true;
        JsonPtr tools(schemas(&a));CHECK(alen(tools.get())==13);
        bool has_submit=false,has_write=false;
        for(size_t i=0;i<alen(tools.get());i++){
            const char *name=sget(get(at(tools.get(),i),"function"),"name");
            has_submit|=!strcmp(name,"submit_plan");
            has_write|=!strcmp(name,"write_file")||!strcmp(name,"edit_file")||!strcmp(name,"desktop_action");
        }
        CHECK(has_submit&&!has_write);
        a.desktop=false;
        J *write=args("planned.txt");put(write,"content",str("x"));put(write,"expected_sha256",nullptr);
        JsonPtr write_args(write),result(tool_call(&a,"write_file",write_args.get()));CHECK(tool_failed(result.get()));
        JsonPtr touch(command("touch planned.txt"));result.reset(tool_call(&a,"run_shell",touch.get()));
        CHECK(tool_failed(result.get())&&strstr(sget(result.get(),"error"),"计划模式"));
        JsonPtr list(command("ls"));result.reset(tool_call(&a,"run_shell",list.get()));
        CHECK(!tool_failed(result.get())&&iget(result.get(),"exit_code",-1)==0);
        CHECK(access((std::string(a.root)+"/planned.txt").c_str(),F_OK)!=0);
        JsonPtr plan(obj());put(plan.get(),"plan",str("# Plan\n1. Edit main.cpp"));
        CHECK(!workflow(&a,{model_tool("plan","submit_plan",copy(plan.get()))}));
        CHECK(a.plan_submitted&&!strcmp(a.state,"本轮结束"));
        J *last=at(a.messages,alen(a.messages)-1);
        CHECK(!strcmp(sget(last,"role"),"assistant")&&!strcmp(sget(last,"content"),"# Plan\n1. Edit main.cpp"));
        JsonPtr request(capture(&a,0));J *defs=get(get(request.get(),"body"),"tools");
        bool offered=false;
        for(size_t i=0;i<alen(defs);i++)offered|=!strcmp(sget(get(at(defs,i),"function"),"name"),"submit_plan");
        CHECK(offered);
        CHECK(!get(get(request.get(),"body"),"system"));
        // Outside plan mode the tool is not offered and refuses to run.
        a.plan_mode=false;
        result.reset(tool_call(&a,"submit_plan",plan.get()));CHECK(tool_failed(result.get()));
        app_cleanup(&a);
    }
    // 4. Headless --plan returns the plan as the reply, with split usage.
    {
        App a=fresh();json_object_put(a.config);a.config=config("headless-plan","openai");
        J *plan=obj();put(plan,"plan",str("PLAN_BODY"));
        std::string reply=model_tool("p","submit_plan",plan);
        reply.insert(reply.size()-1,R"(,"usage":{"prompt_tokens":30,"completion_tokens":4,"prompt_tokens_details":{"cached_tokens":10}})");
        const char *responses[]={reply.c_str()};
        pid_t server=fake_server(&a,responses,1);
        auto run=run_desk({"--json","--plan","--lang","en","--workspace",a.root,"-p","plan it"},model_environment(a),"");
        server_done(server);
        JsonPtr result(json_tokener_parse(run.out.c_str()));
        CHECK(result&&run.status==0);
        CHECK(!strcmp(sget(result.get(),"reply"),"PLAN_BODY")&&bget(result.get(),"plan_submitted"));
        J *usage=get(result.get(),"usage");
        CHECK(iget(usage,"input",0)==20&&iget(usage,"cache_read",0)==10&&iget(usage,"output",0)==4&&!get(usage,"cost"));
        CHECK(iget(result.get(),"tokens",0)==34);
        app_cleanup(&a);
    }
    // 5. Background commands: started by the model or detached with Ctrl+B.
    {
        App a=fresh();a.shell=true;a.yes=true;
        JsonPtr start(command("echo started; sleep 30"));put(start.get(),"background",json_object_new_boolean(1));
        JsonPtr result(tool_call(&a,"run_shell",start.get()));
        CHECK(!tool_failed(result.get())&&!strcmp(sget(result.get(),"shell_id"),"bash-1")&&bget(result.get(),"running"));
        CHECK(background_shell_count(&a)==1);
        JsonPtr id(obj());put(id.get(),"shell_id",str("bash-1"));
        std::string seen=sget(result.get(),"output");
        for(double until=monotime()+5;seen.find("started")==std::string::npos&&monotime()<until;usleep(50000)){
            JsonPtr state(tool_call(&a,"shell_output",id.get()));seen+=sget(state.get(),"output");
        }
        CHECK(seen.find("started")!=std::string::npos);
        JsonPtr stopped(tool_call(&a,"shell_kill",id.get()));
        CHECK(!bget(stopped.get(),"running")&&bget(stopped.get(),"killed"));
        CHECK(background_shell_count(&a)==0);
        JsonPtr quick(command("echo early; exit 3"));put(quick.get(),"background",json_object_new_boolean(1));
        result.reset(tool_call(&a,"run_shell",quick.get()));
        CHECK(!get(result.get(),"shell_id")&&iget(result.get(),"exit_code",0)==3&&strstr(sget(result.get(),"output"),"early"));
        JsonPtr unknown(obj());put(unknown.get(),"shell_id",str("bash-99"));
        result.reset(tool_call(&a,"shell_output",unknown.get()));CHECK(tool_failed(result.get()));
        // Ctrl+B reaches the worker as a flag; the running command is detached.
        std::thread detach([]{usleep(400000);background_requested=true;});
        const double began=monotime();
        result.reset(run_command(&a,"echo detached; sleep 20",30000));
        detach.join();
        CHECK(monotime()-began<5);
        CHECK(bget(result.get(),"moved_to_background")&&!strcmp(sget(result.get(),"shell_id"),"bash-2")&&bget(result.get(),"running"));
        CHECK(background_shell_count(&a)==1);
        app_cleanup(&a);
        CHECK(a.shell_data==nullptr);
    }
    // 6. Checkpoints: file changes of several turns come back newest first,
    // and another conversation's later edits are never overwritten.
    {
        App a=fresh();a.yes=true;
        const std::string path=std::string(a.root)+"/story.txt";
        CHECK(!atomic_file(path.c_str(),"v1\n",3,0644));
        auto edit=[&](const char *turn,const char *from,const char *to){
            a.turn_id=turn;
            JsonPtr read(tool_call(&a,"read_file",JsonPtr(args("story.txt")).get()));
            JsonPtr change(args("story.txt"));
            put(change.get(),"old_text",str(from));put(change.get(),"new_text",str(to));
            put(change.get(),"expected_sha256",str(sget(read.get(),"sha256")));
            JsonPtr done(tool_call(&a,"edit_file",change.get()));CHECK(!tool_failed(done.get()));
        };
        const char *first="11111111111111111111111111111111",*second="22222222222222222222222222222222",*other="33333333333333333333333333333333";
        edit(first,"v1","v2");
        edit(second,"v2","v3");
        a.turn_id=second;
        JsonPtr created(args("created.txt"));put(created.get(),"content",str("new"));put(created.get(),"expected_sha256",nullptr);
        JsonPtr wrote(tool_call(&a,"write_file",created.get()));CHECK(!tool_failed(wrote.get()));
        std::string summary;
        CHECK(undo_turns(a,{second,first},summary));
        char *restored=read_bytes(path.c_str(),64,nullptr);CHECK(restored&&!strcmp(restored,"v1\n"));free(restored);
        CHECK(access((std::string(a.root)+"/created.txt").c_str(),F_OK)!=0);
        CHECK(summary.find("2")!=std::string::npos);
        CHECK(undo_turns(a,{first},summary));
        edit(first,"v1","v2");
        edit(other,"v2","v3");
        CHECK(!undo_turns(a,{first},summary));
        restored=read_bytes(path.c_str(),64,nullptr);CHECK(restored&&!strcmp(restored,"v3\n"));free(restored);
        app_cleanup(&a);
    }
    // 7. history_rewind cuts the saved context and transcript at a request.
    {
        App a=fresh();
        const char *id="44444444444444444444444444444444";
        CHECK(history_save(a,id,"Rewind me",a.config));
        a.conversation_id=id;
        auto user=[&](const char *text,const char *turn){
            J *m=obj();put(m,"role",str("user"));put(m,"content",str(text));put(m,"_turn",str(turn));add(a.messages,m);
            J *r=obj();put(r,"role",str("assistant"));put(r,"content",str((std::string("reply to ")+text).c_str()));add(a.messages,r);
        };
        user("first request","aaaa");
        user("second request","bbbb");
        user("third request","cccc");
        history_checkpoint(a,0);
        CHECK(!history_rewind(a,"missing"));
        CHECK(history_rewind(a,"bbbb"));
        CHECK(alen(a.messages)==2&&!strcmp(sget(at(a.messages,0),"content"),"first request"));
        JsonPtr record(history_read(a,id));
        CHECK(alen(get(record.get(),"transcript"))==2&&alen(get(record.get(),"context"))==2);
        app_cleanup(&a);
    }
}
static void test_plan_rewind_session(){
    // End to end through the conversation worker: /plan, submit_plan, /go,
    // /price and /cost, then /rewind replaces the worker's context.
    App a=fresh();json_object_put(a.config);a.config=config("plan-session","openai");
    J *plan=obj();put(plan,"plan",str("PLAN_STEPS: edit and verify"));
    std::string planned=model_tool("plan","submit_plan",plan);
    planned.insert(planned.size()-1,R"(,"usage":{"prompt_tokens":1000000,"completion_tokens":0})");
    const std::string implemented=model_reply(nullptr,"IMPLEMENTED"),again=model_reply(nullptr,"AGAIN_DONE");
    const char *responses[]={planned.c_str(),implemented.c_str(),again.c_str()};
    pid_t server=fake_server(&a,responses,3);
    CHECK(!save_config(&a,a.config));
    int master=-1;pid_t child=forkpty(&master,nullptr,nullptr,nullptr);CHECK(child>=0);
    if(!child){execl("./build/desk","desk","--lang","en","--workspace",a.root,static_cast<char *>(nullptr));_exit(127);}
    std::string output;
    CHECK(write(master,"1\n1\n",4)==4);CHECK(terminal_until(master,output,"Approval: Ask for approval\r\n❯ "));
    output.clear();type_terminal(master,output,"/price 2 8 USD\n");
    CHECK(terminal_until(master,output,"Prices saved for this model"));
    output.clear();type_terminal(master,output,"/plan\n");
    CHECK(terminal_until(master,output,"Plan mode on"));
    CHECK(terminal_until(master,output,"\r\n❯ "));
    output.clear();type_terminal(master,output,"design it\n");
    CHECK(terminal_until(master,output,"Plan (awaiting approval)"));
    CHECK(terminal_until(master,output,"PLAN_STEPS"));
    CHECK(terminal_until(master,output,"Plan awaiting approval"));
    CHECK(terminal_until(master,output,"$2.00"));
    wait_for_chat_prompt(master,output,"Plan awaiting approval");
    output.clear();type_terminal(master,output,"/go\n");
    CHECK(terminal_until(master,output,"IMPLEMENTED"));
    wait_for_chat_prompt(master,output,"IMPLEMENTED");
    output.clear();type_terminal(master,output,"/cost\n");
    CHECK(terminal_until(master,output,"Usage and cost"));
    CHECK(terminal_until(master,output,"Estimated cost: ≈$2.00"));
    CHECK(terminal_until(master,output,"\r\n❯ "));
    output.clear();type_terminal(master,output,"/rewind\n");
    CHECK(terminal_until(master,output,"Rewind to before which request?"));
    CHECK(terminal_until(master,output,"Select a number:"));
    CHECK(output.find("The plan is approved")<output.find("design it"));
    output.clear();type_terminal(master,output,"2\n");
    CHECK(terminal_until(master,output,"Conversation only"));
    CHECK(terminal_until(master,output,"Select a number:"));
    output.clear();type_terminal(master,output,"2\n");
    CHECK(terminal_until(master,output,"Rewound to before the selected request"));
    CHECK(terminal_until(master,output,"\r\n❯ "));
    output.clear();type_terminal(master,output,"again\n");
    CHECK(terminal_until(master,output,"AGAIN_DONE"));
    wait_for_chat_prompt(master,output,"AGAIN_DONE");
    leave_chat_and_exit(master);
    CHECK(reap_pty(master,child)==0);
    server_done(server);
    auto names=[](J *request){
        std::string joined;
        J *defs=get(get(request,"body"),"tools");
        for(size_t i=0;i<alen(defs);i++){joined+=sget(get(at(defs,i),"function"),"name");joined+=' ';}
        return joined;
    };
    JsonPtr first(capture(&a,0)),second(capture(&a,1)),third(capture(&a,2));
    CHECK(names(first.get()).find("submit_plan")!=std::string::npos&&names(first.get()).find("write_file")==std::string::npos);
    CHECK(names(second.get()).find("submit_plan")==std::string::npos&&names(second.get()).find("write_file")!=std::string::npos);
    J *messages=get(get(second.get(),"body"),"messages");
    CHECK(!strcmp(sget(at(messages,alen(messages)-1),"content"),"The plan is approved. Implement it now and verify the result when done."));
    CHECK(!strcmp(sget(at(messages,alen(messages)-2),"content"),"PLAN_STEPS: edit and verify"));
    // After the rewind the worker starts from an empty context.
    messages=get(get(third.get(),"body"),"messages");
    CHECK(alen(messages)==1&&!strcmp(sget(at(messages,0),"content"),"again"));
    App reloaded;CHECK(!store_load(&reloaded));
    CHECK(json_object_get_double(get(get(get(reloaded.store,"profiles"),"plan-session"),"price_input"))==2);
    app_cleanup(&reloaded);
    app_cleanup(&a);
}
int main(int argc,char **argv){
    if(argc==2&&!strcmp(argv[1],"--mcp-fixture"))return mcp_fixture();
    curl_global_init(CURL_GLOBAL_DEFAULT);
    // The pty harness cannot emulate a pinned composer region at the bottom of
    // the screen, so keep the inline status line for the assertions.
    setenv("DESK_NO_COMPOSER","1",1);
    char directory[]="/tmp/desk-c-test-XXXXXX";
    CHECK(mkdtemp(directory));
    CHECK(realpath(directory,sandbox));
    const bool verbose_test=getenv("DESK_VERBOSE_TEST")!=nullptr;
    auto run_step=[&](const char *label,const std::function<void()> &fn){
        if(verbose_test){fprintf(stderr,"[test] %s\n",label);fflush(stderr);}
        fn();
    };
#define RUN_STEP(call) run_step(#call,[&]{call;})
    RUN_STEP(test_layout_and_markdown());
    RUN_STEP(test_diff_preview());
    RUN_STEP(test_headless());
    RUN_STEP(test_provider_presets());
    RUN_STEP(test_mcp());
    RUN_STEP(test_http_reuse());
    RUN_STEP(test_tool_configuration());
    RUN_STEP(test_approval_modes());
    RUN_STEP(test_streaming());
    RUN_STEP(test_streamed_tools());
    RUN_STEP(test_reasoning_modes());
    RUN_STEP(test_assisted_worker());
    RUN_STEP(test_chat_mode_menu());
    RUN_STEP(test_chat_exit_paths());
    RUN_STEP(test_compact_output());
    RUN_STEP(test_exit_during_work());
    RUN_STEP(test_compact_approval());
    RUN_STEP(test_chat_history_scroll());
    RUN_STEP(test_prompt_recall_and_context());
    RUN_STEP(test_stream_usage_and_tail_utf8());
    RUN_STEP(test_code_outline_and_project_guide());
    RUN_STEP(test_mentions_paste_and_multimodal());
    RUN_STEP(test_v1_13_features());
    RUN_STEP(test_v1_14_features());
    RUN_STEP(test_v1_15_features());
    RUN_STEP(test_plan_rewind_session());
    RUN_STEP(test_chat_actions_layout());
    RUN_STEP(test_menu_navigation_and_resize());
    RUN_STEP(test_persistent_conversations());
    RUN_STEP(test_guided_approval());
    RUN_STEP(test_line_editing());
    RUN_STEP(test_queued_editing());
    RUN_STEP(test_conversation_picker());
    RUN_STEP(test_thinking_timer());
    RUN_STEP(test_background_conversations());
    RUN_STEP(test_picker_approval_status());
    RUN_STEP(test_picker_failed_status());
    RUN_STEP(test_model_switch_requests());
    RUN_STEP(test_utf8());
    RUN_STEP(test_files());
    RUN_STEP(test_search_and_edit());
    RUN_STEP(test_config());
    RUN_STEP(test_migration());
    RUN_STEP(test_process());
    RUN_STEP(test_context_and_validation());
    RUN_STEP(test_openai_loop());
    RUN_STEP(test_anthropic_loop());
    RUN_STEP(test_failed_task());
    RUN_STEP(test_edit_workflow());
    RUN_STEP(test_recovery_and_loops());
    RUN_STEP(test_native_error_flags());
    RUN_STEP(test_menu());
    RUN_STEP(test_parallel_tasks());
    RUN_STEP(test_removed_tasks());
    RUN_STEP(test_parallel_resources());
    RUN_STEP(test_task_switch_approval());
    RUN_STEP(test_task_switch_approval(false));
    RUN_STEP(test_language_preferences());
    RUN_STEP(test_english_menu());
    RUN_STEP(test_no_extra_model_prompts());
    RUN_STEP(test_numeric_request());
    RUN_STEP(test_numeric_settings());
    RUN_STEP(test_settings_redraw());
#undef RUN_STEP
    curl_global_cleanup();
    // Keep the sandbox after a failure for inspection; a passing run cleans up
    // so repeated runs do not pile up /tmp/desk-c-test-* directories.
    std::error_code removed;
    std::filesystem::remove_all(sandbox,removed);
    if(removed)fprintf(stderr,"warning: cannot remove %s: %s\n",sandbox,removed.message().c_str());
    printf("PASS: %d assertions; file/search/edit/config/process/context/recovery/OpenAI/Anthropic/menu/parallel/resume/approval/locks integration.\n",checks);
    return 0;
}
