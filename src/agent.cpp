#include "desk.hpp"
#include "tasks.hpp"
#include "mcp.hpp"
#include "ui.hpp"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <unistd.h>
#include <termios.h>
#endif
#include <ctype.h>
#include <algorithm>
void reset_session(App *a){
    a->session_revision++;
    json_object_put(a->messages);
    a->messages=arr();
    a->state="等待任务";
}

static size_t json_bytes(J *value){
    if(!value)return 4;
    size_t length=0;
    json_object_to_json_string_length(value,JSON_C_TO_STRING_PLAIN,&length);
    return length;
}

static bool image_block(J *block){
    return !strncmp(sget(block,"type"),"image",5);
}

// A provider bills an image at roughly a thousand tokens whatever its base64
// length, so charging the payload would compact on the first screenshot and
// price a dozen of them like a library of books.
static constexpr size_t IMAGE_BYTES=8192;
// Newest tool results keep their detail: summarising them would leave the model
// blind to the state it just inspected.
static constexpr size_t FRESH_RESULTS=3;
// Newest screenshots stay attached, older ones become a note. The budget does
// the real trimming; this only keeps a request from carrying megabytes.
static constexpr size_t FRESH_IMAGES=3;
static constexpr size_t MIN_CONTEXT_LIMIT=16384;

// DESK_CONTEXT_BYTES raises or lowers the budget for models with a wider or
// narrower window than the built-in default assumes.
static size_t context_limit(void){
    static const size_t limit=[]{
        const char *value=env_value("DESK_CONTEXT_BYTES");
        if(!value||!*value)return static_cast<size_t>(CONTEXT_LIMIT);
        char *end=nullptr;
        long long parsed=strtoll(value,&end,10);
        if(end==value||parsed<static_cast<long long>(MIN_CONTEXT_LIMIT))return static_cast<size_t>(CONTEXT_LIMIT);
        return static_cast<size_t>(std::min<long long>(parsed,RESPONSE_LIMIT));
    }();
    return limit;
}

// Tool results carry their blocks inside a JSON string, so the cost estimate
// looks at the parsed form to find the payload worth billing flat.
static size_t message_bytes(J *message){
    size_t bytes=json_bytes(message);
    J *blocks=get(message,"content");
    JsonPtr parsed;
    if(!json_object_is_type(blocks,json_type_array)){
        if(strcmp(sget(message,"role"),"tool"))return bytes;
        parsed.reset(json_tokener_parse(sget(message,"content")));
        blocks=get(parsed.get(),"content");
        if(!json_object_is_type(blocks,json_type_array))return bytes;
    }
    for(size_t i=0;i<alen(blocks);++i){
        J *block=at(blocks,i);
        if(!image_block(block))continue;
        size_t length=json_bytes(block);
        if(length>IMAGE_BYTES)bytes-=length-IMAGE_BYTES;
    }
    return bytes;
}

static size_t context_bytes(J *messages,J *tools){
    size_t total=json_bytes(tools);
    for(size_t i=0;i<alen(messages);++i)total+=message_bytes(at(messages,i));
    return total;
}

static std::string excerpt(std::string_view text,size_t limit){
    if(text.size()<=limit)return std::string(text);
    while(limit>0&&(static_cast<unsigned char>(text[limit])&0xc0)==0x80)--limit;
    return std::string(text.substr(0,limit))+"…";
}

// Text from tools or the model printed to the terminal: cut on a character
// boundary and without control bytes, so it cannot split UTF-8 or drive the
// terminal (colour codes in command output are dropped as well).
static std::string printable(std::string_view text,size_t limit){
    std::string cut=excerpt(text,limit),shown;
    shown.reserve(cut.size());
    for(size_t i=0;i<cut.size();i++){
        const auto c=static_cast<unsigned char>(cut[i]);
        if(c==0x1b&&i+1<cut.size()&&cut[i+1]=='['){
            i+=2;
            while(i<cut.size()&&!(cut[i]>='@'&&cut[i]<='~'))i++;
            continue;
        }
        if((c<32&&c!='\n'&&c!='\t')||c==127)continue;
        shown+=static_cast<char>(c);
    }
    return valid_utf8(shown)?shown:std::string();
}
static std::string tool_excerpt_parsed(J *result,const char *fallback,size_t limit){
    J *blocks=get(result,"content");
    if(!json_object_is_type(blocks,json_type_array))return excerpt(fallback,limit);
    std::string summary;
    bool omitted_image=false;
    for(size_t i=0;i<alen(blocks);++i){
        J *block=at(blocks,i);
        omitted_image|=image_block(block);
        if(!strcmp(sget(block,"type"),"text")&&summary.size()<limit){
            summary+=excerpt(sget(block,"text"),limit-summary.size());
            summary+='\n';
        }
    }
    if(omitted_image){
        summary+="[Image omitted to fit context. If visual detail is still needed, request a smaller screenshot with a reduced resolution or byte limit.]";
    }
    return summary;
}

static std::string tool_excerpt(const char *content,size_t limit){
    JsonPtr result(json_tokener_parse(content));
    return tool_excerpt_parsed(result.get(),content,limit);
}

static J *compact_message(J *message,bool fresh=false){
    const char *content=sget(message,"content");
    if(fresh||strcmp(sget(message,"role"),"tool")||strlen(content)<=8192)return copy(message);

    JsonPtr original(json_tokener_parse(content)),result(obj());
    put(result.get(),"context_compacted",json_object_new_boolean(true));
    put(result.get(),"summary",str(tool_excerpt_parsed(original.get(),content,8192).c_str()));
    if(tool_failed(original.get())){
        const char *error=sget(original.get(),"error");
        put(result.get(),"error",str(*error?excerpt(error,512).c_str():"The tool reported a failure."));
    }
    J *shortened=copy(message);
    put(shortened,"content",str(json_object_to_json_string_ext(result.get(),JSON_C_TO_STRING_PLAIN)));
    return shortened;
}

static std::string recap(J *messages,size_t begin,size_t end){
    std::string notes;
    const size_t recent=std::max(begin,end>8?end-8:0);
    for(size_t i=recent;i>begin;--i){
        J *message=at(messages,i-1);
        if(!strcmp(sget(message,"role"),"user")){
            notes+="user: "+excerpt(user_message_text(message),720)+"\n";
            break;
        }
    }
    for(size_t i=recent;i<end;++i){
        J *message=at(messages,i);
        std::string_view role=sget(message,"role");
        notes+=std::string(role)+": ";
        if(role=="tool"){
            notes+=tool_excerpt(sget(message,"content"),360);
        }else if(role=="user"){
            notes+=excerpt(user_message_text(message),360);
        }else{
            notes+=excerpt(sget(message,"content"),360);
            J *calls=get(message,"tool_calls");
            for(size_t j=0;j<std::min<size_t>(alen(calls),2);++j){
                J *function=get(at(calls,j),"function");
                notes+=" "+excerpt(sget(function,"name"),80)+" "+excerpt(sget(function,"arguments"),160);
            }
        }
        notes+='\n';
    }
    return notes;
}

static void append_recap(J *messages,const std::string &notes){
    if(notes.empty())return;
    J *request=obj(),*summary=obj();
    put(request,"role",str("user"));
    put(request,"content",str("Carry forward a summary of the preceding context."));
    put(summary,"role",str("assistant"));
    const std::string content="Automatic context summary. The following are excerpts of prior requests and tool observations. Verify state before repeating any operation.\n"+notes;
    put(summary,"content",str(content.c_str()));
    add(messages,request);
    add(messages,summary);
}

static void append_messages(J *dest,J *source,size_t start=0){
    for(size_t i=start;i<alen(source);++i)add(dest,copy(at(source,i)));
}

// A trimmed request keeps a recap in front of it, unless that recap itself
// pushes the request back over the budget.
static J *with_recap(App *a,J *body,size_t first,J *tools){
    if(!first)return body;
    JsonPtr original(body);
    JsonPtr wrapped(arr());
    append_recap(wrapped.get(),recap(a->messages,0,first));
    append_messages(wrapped.get(),body);
    if(context_bytes(wrapped.get(),tools)>context_limit())return original.release();
    return wrapped.release();
}

// An older screenshot costs tokens on every later request while the newest views
// answer the question being asked. Older ones become a note the model can act
// on instead of megabytes of base64 it cannot read.
static void strip_stale_images(J *messages){
    size_t kept=0;
    for(size_t i=alen(messages);i>0;--i){
        J *message=at(messages,i-1);
        const char *role=sget(message,"role");
        if(!strcmp(role,"user")){
            J *blocks=get(message,"content");
            if(!json_object_is_type(blocks,json_type_array))continue;
            bool images=false;
            for(size_t j=0;j<alen(blocks);++j)images|=image_block(at(blocks,j));
            if(!images||kept++<FRESH_IMAGES)continue;
            for(size_t j=0;j<alen(blocks);++j){
                J *block=at(blocks,j);
                if(!image_block(block))continue;
                json_object_object_del(block,"data");
                json_object_object_del(block,"source");
                json_object_object_del(block,"image_url");
                put(block,"type",str("text"));
                put(block,"text",str("[Image omitted from an earlier step.]"));
            }
            continue;
        }
        if(strcmp(role,"tool"))continue;
        JsonPtr parsed(json_tokener_parse(sget(message,"content")));
        J *blocks=get(parsed.get(),"content");
        if(!json_object_is_type(blocks,json_type_array))continue;
        bool images=false;
        for(size_t j=0;j<alen(blocks);++j)images|=image_block(at(blocks,j));
        if(!images||kept++<FRESH_IMAGES)continue;
        for(size_t j=0;j<alen(blocks);++j){
            J *block=at(blocks,j);
            if(!image_block(block))continue;
            json_object_object_del(block,"data");
            json_object_object_del(block,"source");
            json_object_object_del(block,"image_url");
            put(block,"type",str("text"));
            put(block,"text",str("[Image omitted from an earlier step. Take a fresh screenshot if that view is still needed.]"));
        }
        put(message,"content",str(json_object_to_json_string_ext(parsed.get(),JSON_C_TO_STRING_PLAIN)));
    }
}

static J *compact_context(App *a,J *tools,size_t first){
    const size_t end=alen(a->messages);
    const size_t limit=context_limit();
    // The newest results keep their detail for as long as the budget allows.
    std::vector<char> fresh(end-first,0);
    for(size_t i=end,kept=0;i>first&&kept<FRESH_RESULTS;--i){
        if(!strcmp(sget(at(a->messages,i-1),"role"),"tool")){
            fresh[i-1-first]=1;
            ++kept;
        }
    }
    auto build=[&](bool detailed){
        JsonPtr body(arr());
        for(size_t i=first;i<end;++i)add(body.get(),compact_message(at(a->messages,i),detailed&&fresh[i-first]));
        return body;
    };

    JsonPtr kept=build(true);
    if(context_bytes(kept.get(),tools)<=limit)return with_recap(a,kept.release(),first,tools);
    // Still too big: summarise every result, newest ones included. Their images
    // become notes, so the model keeps the shape of what it saw.
    JsonPtr terse=build(false);
    if(context_bytes(terse.get(),tools)<=limit)return with_recap(a,terse.release(),first,tools);

    // Drop complete groups of calls and results from the front of this turn.
    // The latest user request stays intact even if all tool groups must be dropped.
    const size_t tools_bytes=json_bytes(tools),terse_count=alen(terse.get());
    std::vector<size_t> terse_used(terse_count+1,0);
    for(size_t i=0;i<terse_count;++i)terse_used[i+1]=terse_used[i]+message_bytes(at(terse.get(),i));
    for(size_t start=1;start<terse_count;){
        ++start;
        while(start<terse_count&&!strcmp(sget(at(terse.get(),start),"role"),"tool"))++start;
        JsonPtr candidate(arr());
        append_recap(candidate.get(),recap(terse.get(),1,start));
        size_t recap_bytes=0;
        for(size_t i=0;i<alen(candidate.get());++i)recap_bytes+=message_bytes(at(candidate.get(),i));
        const size_t remaining_bytes=terse_used[1]+(terse_used[terse_count]-terse_used[start]);
        if(tools_bytes+recap_bytes+remaining_bytes<=limit){
            add(candidate.get(),copy(at(terse.get(),0)));
            append_messages(candidate.get(),terse.get(),start);
            return candidate.release();
        }
    }
    return nullptr;
}

J *context(App *a,J *tools,bool *compacted){
    auto attach_guide=[&](J *selected)->J*{
        if(!selected||a->subagent)return selected;
        std::string guide=load_project_guide(*a);
        if(guide.empty())return selected;
        JsonPtr sys(obj());
        put(sys.get(),"role",str("system"));
        put(sys.get(),"content",str(guide.c_str()));
        if(message_bytes(sys.get())+context_bytes(selected,tools)>context_limit())return selected;
        JsonPtr with_guide(arr());
        add(with_guide.get(),sys.release());
        append_messages(with_guide.get(),selected);
        json_object_put(selected);
        return with_guide.release();
    };
    if(compacted)*compacted=false;
    const size_t n=alen(a->messages);
    size_t first=n;
    for(size_t i=0;i<n;i++)if(!strcmp(sget(at(a->messages,i),"role"),"user"))first=i;
    if(first==n)return attach_guide(copy(a->messages));

    // Bill every message once, then walk back through whole turns while the
    // request still fits: the budget decides what is sent, not a turn count.
    const size_t tools_bytes=json_bytes(tools),limit=context_limit();
    std::vector<size_t> used(n+1,0);
    for(size_t i=0;i<n;++i)used[i+1]=used[i]+message_bytes(at(a->messages,i));
    auto fits=[&](size_t start){return used[n]-used[start]+tools_bytes<=limit;};

    if(!fits(first)){
        J *selected=compact_context(a,tools,first);
        if(selected){
            if(compacted)*compacted=true;
        }else seterr(a,tr("本轮超过上下文预算，请拆分任务或选择新建对话。"));
        return attach_guide(selected);
    }

    // Try shortening bulky tool output before dropping an earlier turn.
    size_t start=first,total=used[n]-used[first]+tools_bytes;
    std::vector<JsonPtr> shortened(first);
    while(start>0){
        size_t previous=start;
        for(size_t i=start;i>0;i--)if(!strcmp(sget(at(a->messages,i-1),"role"),"user")){previous=i-1;break;}
        if(previous==start)break;
        size_t turn_bytes=used[start]-used[previous];
        if(turn_bytes>limit-total){
            turn_bytes=0;
            for(size_t i=previous;i<start;++i){
                shortened[i].reset(compact_message(at(a->messages,i)));
                turn_bytes+=message_bytes(shortened[i].get());
            }
            if(turn_bytes>limit-total)break;
        }
        total+=turn_bytes;
        start=previous;
    }

    JsonPtr history(arr());
    for(size_t i=start;i<n;++i){
        if(i<first&&shortened[i])add(history.get(),shortened[i].release());
        else add(history.get(),copy(at(a->messages,i)));
    }
    strip_stale_images(history.get());
    return attach_guide(with_recap(a,history.release(),start,tools));
}
J *parse_tool_arguments(const char *text){
    size_t length=strlen(text);
    if(length>CONTEXT_LIMIT||!valid_utf8(std::string_view(text,length)))return nullptr;
    // json-c 0.18's STRICT string check treats non-ASCII signed chars as
    // controls. Feed it equivalent Unicode escapes while keeping strict JSON.
    std::string portable;
    portable.reserve(length);
    auto unit=[&](uint32_t code){
        constexpr char hex[]="0123456789abcdef";
        portable+="\\u";
        for(int shift=12;shift>=0;shift-=4)portable+=hex[(code>>shift)&0xf];
    };
    for(size_t i=0;i<length;){
        auto c=static_cast<unsigned char>(text[i]);
        if(c<0x80){portable+=text[i++];continue;}
        size_t width=c<0xe0?2:c<0xf0?3:4;
        uint32_t code=c&((1u<<(7-width))-1);
        for(size_t k=1;k<width;k++)code=(code<<6)|(static_cast<unsigned char>(text[i+k])&0x3f);
        if(code<=0xffff)unit(code);
        else {code-=0x10000;unit(0xd800+(code>>10));unit(0xdc00+(code&0x3ff));}
        i+=width;
    }
    struct json_tokener *tok=json_tokener_new();
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT);
    J *args=json_tokener_parse_ex(tok,portable.c_str(),static_cast<int>(portable.size())+1);
    bool valid=json_tokener_get_error(tok)==json_tokener_success&&json_object_is_type(args,json_type_object);
    size_t end=json_tokener_get_parse_end(tok);
    for(size_t i=end;i<portable.size();i++)if(!isspace(static_cast<unsigned char>(portable[i])))valid=false;
    json_tokener_free(tok);
    if(!valid){json_object_put(args);return NULL;}
    return args;
}
static int validate_calls(App *a,J *calls){
    if(calls&&!json_object_is_type(calls,json_type_array)){
        seterr(a,tr("工具调用不是数组。"));
        return -1;
    }
    if(alen(calls)>12){
        seterr(a,tr("单轮工具数量过多。"));
        return -1;
    }
    for(size_t i=0;i<alen(calls);i++){
        J *call=at(calls,i),*f=get(call,"function");
        J *raw=get(f,"arguments");
        if(!*sget(call,"id")||!*sget(f,"name")||!json_object_is_type(raw,json_type_string)||static_cast<size_t>(json_object_get_string_len(raw))!=strlen(sget(f,"arguments"))){
            seterr(a,tr("工具调用格式无效。"));
            return -1;
        }
        J *args=parse_tool_arguments(sget(f,"arguments"));
        if(!args){
            seterr(a,tr("工具参数不是完整的 JSON 对象，本轮操作未执行。"));
            return -1;
        }
        json_object_put(args);
        for(size_t k=0;k<i;k++)if(!strcmp(sget(at(calls,k),"id"),sget(call,"id"))){
            seterr(a,tr("工具编号重复，未执行本轮操作。"));
            return -1;
        }
    }
    return 0;
}
static void tool_result(App *a,const char *id,J *r){
    J *m=obj();
    put(m,"role",str("tool"));
    put(m,"tool_call_id",str(id));
    put(m,"content",str(json_object_to_json_string_ext(r,JSON_C_TO_STRING_PLAIN)));
    add(a->messages,m);
}
static void pending(App *a,J *calls,size_t start){
    for(size_t k=start;k<alen(calls);k++){
        J *r=err(tr("任务已中止，此操作未确认执行；继续前重新检查状态。"));
        tool_result(a,sget(at(calls,k),"id"),r);
        json_object_put(r);
    }
}
static bool mutation(const char *name){
    return !strcmp(name,"write_file")||!strcmp(name,"edit_file")||!strcmp(name,"multi_edit")||!strcmp(name,"desktop_action")||!strcmp(name,"desktop_set_text");
}
static const char *domain(const char *name){
    if(!strcmp(name,"read_file")||!strcmp(name,"write_file")||!strcmp(name,"edit_file")||!strcmp(name,"multi_edit"))return "file";
    if(!strncmp(name,"desktop_",8))return "desktop";
    return name;
}
/* A successful unrelated read must not erase a failed command or write. */
struct FailedOperation {
    std::string kind,target;
    bool mutation;
};
struct Observation {
    std::string name;
    JsonPtr args,result;
    int count=1;
};
static bool readonly_probe(const char *name){
    return !strcmp(name,"read_file")||!strcmp(name,"list_files")||!strcmp(name,"search_text")
        ||!strcmp(name,"code_outline")||!strcmp(name,"delegate_research")||!strcmp(name,"web_fetch")||!strcmp(name,"web_search");
}
static void recovery(App *a,std::vector<FailedOperation> &unresolved,const char *name,J *args,bool failed){
    const char *kind=domain(name),*target=sget(args,"path");
    char full[PATH_MAX];
    if(*target&&!resolve_file(a,target,full))target=full;
    auto matches=[&](const FailedOperation &entry){return entry.kind==kind&&entry.target==target;};
    if(!failed){
        std::erase_if(unresolved,[&](const FailedOperation &entry){
            if(matches(entry)&&(!entry.mutation||mutation(name)))return true;
            // Exploratory read/list/search misses are resolved once workspace
            // exploration or editing moves forward; failed mutations and shell
            // commands still require explicit recovery.
            if(!entry.mutation&&(entry.kind=="file"||entry.kind=="list_files"||entry.kind=="search_text")
               &&(readonly_probe(name)||mutation(name)))return true;
            return false;
        });
        return;
    }
    for(auto &entry:unresolved){
        if(matches(entry)){
            entry.mutation=entry.mutation||mutation(name);
            return;
        }
    }
    unresolved.push_back({kind,target,mutation(name)});
}
static bool repeated(std::vector<Observation> &seen,const char *name,J *args,J *result){
    if((mutation(name)||!strcmp(name,"run_shell"))&&!tool_failed(result)){
        /* Changes invalidate old observations, so a post-edit read is fresh. */
        seen.clear();
        return false;
    }
    for(auto &entry:seen){
        if(entry.name!=name||!json_object_equal(entry.args.get(),args))continue;
        entry.count=json_object_equal(entry.result.get(),result)?entry.count+1:1;
        entry.result.reset(copy(result));
        return entry.count>=3;
    }
    seen.push_back({name,JsonPtr(copy(args)),JsonPtr(copy(result))});
    return false;
}
static J *repeated_result(const std::vector<Observation> &seen,const char *name,J *args){
    for(const auto &entry:seen){
        if(entry.count>=3&&entry.name==name&&json_object_equal(entry.args.get(),args)){
            J *result=copy(entry.result.get());
            put(result,"cached_result",json_object_new_boolean(true));
            return result;
        }
    }
    return nullptr;
}
static void show_call(const char *name,J *args){
    const char *label=!strcmp(name,"read_file")?tr("读取文件"):!strcmp(name,"list_files")?tr("查看目录"):
        !strcmp(name,"search_text")?tr("搜索文本"):!strcmp(name,"code_outline")?tr("代码大纲"):
        !strcmp(name,"delegate_research")?tr("调研代码库"):!strcmp(name,"web_fetch")?tr("读取网页"):
        !strcmp(name,"web_search")?tr("联网搜索"):!strcmp(name,"multi_edit")?tr("批量修改文件"):
        !strcmp(name,"edit_file")?tr("修改文件"):!strcmp(name,"write_file")?tr("写入文件"):
        !strcmp(name,"run_shell")?tr("执行命令"):!strcmp(name,"desktop_snapshot")?tr("读取桌面"):
        !strcmp(name,"desktop_screenshot")?tr("截取桌面"):tr("操作桌面");
    const char *target=*sget(args,"path")?sget(args,"path"):!strcmp(name,"run_shell")?sget(args,"command"):
        !strcmp(name,"delegate_research")?sget(args,"question"):!strcmp(name,"web_fetch")?sget(args,"url"):
        !strcmp(name,"web_search")?sget(args,"query"):sget(args,"app");
    const int width=std::max(24,terminal_columns()-6);
    std::string head=std::string(ui_bullet())+" "+label;
    if(*target){
        std::string inner=ui_clip(target,std::max(8,width-ui_width(head)-2));
        head+="("+inner+")";
    }
    printf("%s\n",ui_paint(Tone::Accent,ui_clip(head,width)).c_str());
    if(!strcmp(name,"search_text"))printf(tr("  查找：%s\n"),printable(sget(args,"query"),256).c_str());
    fflush(stdout);
}
static void show_result(const char *name,J *r,double elapsed){
    bool failed=tool_failed(r);
    std::string detail;
    char text[512];
    if(*sget(r,"error")){detail=" · ";detail.append(sget(r,"error"));}
    else if(!strcmp(name,"run_shell")){
        snprintf(text,sizeof text,tr(" · 退出码 %d%s"),iget(r,"exit_code",0),bget(r,"timed_out")?tr(" · 超时"):"");
        detail=text;
    }
    else if(get(r,"written"))detail=tr(" · 已回读核对");
    else if(!strcmp(name,"read_file")||!strcmp(name,"web_fetch")){
        snprintf(text,sizeof text,tr(" · %d 字节"),json_object_get_string_len(get(r,"content")));
        detail=text;
    }
    else if(!strcmp(name,"search_text")){
        snprintf(text,sizeof text,tr(" · %zu 条匹配%s"),alen(get(r,"matches")),bget(r,"truncated")?tr(" · 结果有截断"):"");
        detail=text;
    }
    else if(!strcmp(name,"list_files")){
        snprintf(text,sizeof text,tr(" · %zu 项"),alen(get(r,"entries")));
        detail=text;
    }
    else if(!strcmp(name,"code_outline")){
        snprintf(text,sizeof text,tr(" · %zu 个符号"),alen(get(r,"symbols")));
        detail=text;
    }
    else if(!strcmp(name,"delegate_research")){
        snprintf(text,sizeof text,tr(" · %d 步"),iget(r,"steps",0));
        detail=text;
    }
    else if(!strcmp(name,"web_search")){
        snprintf(text,sizeof text,tr(" · %zu 条结果"),alen(get(r,"results")));
        detail=text;
    }
    else if(!strcmp(name,"desktop_screenshot")){
        detail=tr(" · 已附带截图");
    }
    else if(get(r,"accepted"))detail=tr(" · 等待重新读取核对");
    char timing[48];
    snprintf(timing,sizeof timing," · %.1fs",elapsed);
    std::string line=std::string("  ")+ui_result()+"  ";
    line+=failed?tr("失败"):tr("完成");
    line+=detail;
    line+=timing;
    printf("%s\n",ui_paint(failed?Tone::Error:Tone::Dim,ui_clip(line,std::max(24,terminal_columns()-6))).c_str());
    if(!strcmp(name,"run_shell")&&*sget(r,"output")){
        const char *output=sget(r,"output");
        printf("%s\n",printable(output,1600).c_str());
        if(strlen(output)>1600||bget(r,"truncated"))puts(tr("  …输出已截断，模型可见工具返回的输出片段。"));
    }
}
static const char *activity(const char *name){
    if(!strncmp(name,"mcp_",4))return tr("正在调用 MCP 工具…");
    if(!strcmp(name,"read_file"))return tr("正在读取文件…");
    if(!strcmp(name,"list_files"))return tr("正在查看目录…");
    if(!strcmp(name,"search_text"))return tr("正在搜索…");
    if(!strcmp(name,"code_outline"))return tr("正在分析代码结构…");
    if(!strcmp(name,"delegate_research"))return tr("正在调研代码库…");
    if(!strcmp(name,"web_fetch"))return tr("正在读取网页…");
    if(!strcmp(name,"web_search"))return tr("正在联网搜索…");
    if(!strcmp(name,"desktop_screenshot"))return tr("正在截取桌面…");
    if(!strcmp(name,"write_file")||!strcmp(name,"edit_file")||!strcmp(name,"multi_edit"))return tr("正在编辑文件…");
    if(!strcmp(name,"run_shell"))return tr("正在执行命令…");
    return tr("正在操作桌面…");
}
#ifndef _WIN32
// Queued typing stays in the terminal input buffer while progress is drawn.
// An approval prompt temporarily lets Readline render its own input line.
struct OperationEcho {
    struct termios saved{};bool changed=false;
    explicit OperationEcho(const App &app){
        if(app.task_events<0&&isatty(STDIN_FILENO)&&isatty(STDOUT_FILENO)&&!tcgetattr(STDIN_FILENO,&saved)){
            struct termios mode=saved;mode.c_lflag&=~ECHO;
#ifdef IUTF8
            mode.c_iflag|=IUTF8;
#endif
            changed=!tcsetattr(STDIN_FILENO,TCSANOW,&mode);
        }
    }
    ~OperationEcho(){if(changed)tcsetattr(STDIN_FILENO,TCSANOW,&saved);}
};
#endif
class ThinkingProgress {
    App &app;
public:
    explicit ThinkingProgress(App &value):app(value){
        app.thinking=true;app.thinking_started=monotime();task_event(app,'t');
    }
    ~ThinkingProgress(){app.thinking=false;task_event(app,'e');progress_clear(&app);}
};
int run_task(App *a,const char *task){
#ifndef _WIN32
    OperationEcho input_echo(*a);
#endif
    std::unique_ptr<ResourceLock> desktop_lock;
    if(a->desktop){
        desktop_lock=std::make_unique<ResourceLock>(*a,true);
        if(!desktop_lock->acquired()){seterr(a,tr("桌面资源等待已取消、被占用或无法加锁；请返回对应对话处理已有操作。"));a->state=interrupted?"已取消":"失败";return -1;}
    }
    if(!a->messages)reset_session(a);
    if(!a->subagent){
        char tid[33];
        id_string(tid);
        a->turn_id=tid;
        prune_workspace_cache(*a);
    }
    size_t history_start=alen(a->messages);
    add(a->messages,prepare_user_message(a,task));
    history_checkpoint(*a,history_start);
    a->state="执行中";
    a->error[0]=0;
    a->task_tokens=0;
    a->approval_unavailable=false;
    const double task_started=monotime();
    if(!mcp_prepare(*a)){a->state="失败";return -1;}
    JsonPtr tool_owner(schemas(a));
    J *tools=tool_owner.get();
    std::vector<FailedOperation> unresolved;
    std::vector<Observation> seen;
    int rc=-1;
    bool announced_compaction=false,desktop_unverified=false;
    std::string_view last_activity;
    const char *reply_marker=a->conversation_id.empty()?"":ui_assistant_marker(ui_color());
    // Chat replies hang under the "● " marker; one-shot output stays flush.
    const int reply_indent=*reply_marker?2:0;
    // Headless runs keep the reply off the human stream entirely.
    const bool quiet_reply=a->reply_out||a->json_output;
    for(std::uint64_t step=1;!interrupted;++step){
        if(a->max_steps>0&&step>static_cast<std::uint64_t>(a->max_steps))break;
        bool compacted=false;
        J *history=context(a,tools,&compacted);
        if(!history)break;
        {
            // Rounded up so any content shows at least 1%.
            const size_t limit=context_limit(),used=context_bytes(history,tools);
            task_context(*a,static_cast<int>(std::min<size_t>(100,(used*100+limit-1)/limit)));
        }
        if(compacted&&!announced_compaction){
            progress_clear(a);
            puts(tr("已压缩上下文，继续当前任务。"));
            announced_compaction=true;
        }
        J *reply=nullptr;
        bool streamed=false;
        std::unique_ptr<MarkdownStream> markdown;
        {
            ThinkingProgress thinking(*a);
            if(a->verbose){
                if(a->max_steps>0)printf(tr("[%d/%d] 正在请求模型…\n"),static_cast<int>(step),a->max_steps);
                else puts(tr("正在请求模型…"));
            }
            else progress(a,tr("正在思考…"));
            fflush(stdout);
            // Print text as it arrives; the first chunk clears the "thinking"
            // row and opens the reply so the transcript fills in live.
            if(!quiet_reply)a->stream_text=[a,&streamed,&last_activity,&markdown,reply_marker,reply_indent](const std::string &part){
                if(!streamed){
                    streamed=true;
                    last_activity={};
                    progress_clear(a);
                    putchar('\n');
                    fputs(reply_marker,stdout);
                    markdown=std::make_unique<MarkdownStream>(ui_color(),reply_indent);
                }
                fputs(markdown->feed(part).c_str(),stdout);
                fflush(stdout);
            };
            reply=api_complete(a,a->config,history,tools);
            a->stream_text=nullptr;
            if(markdown){fputs(markdown->finish().c_str(),stdout);fflush(stdout);}
        }
        json_object_put(history);
        if(!reply)break;
        J *calls=get(reply,"tool_calls");
        if(validate_calls(a,calls)){
            json_object_put(reply);
            break;
        }
        if(!alen(calls)){
            if(!unresolved.empty()||desktop_unverified){
                seterr(a,desktop_unverified?tr("桌面操作尚未重新读取核对，任务未确认完成。"):tr("仍有未恢复的操作失败，任务未确认完成；可继续描述需求或要求诊断。"));
                json_object_put(reply);
                break;
            }
            if(!*sget(reply,"content")){
                seterr(a,tr("模型未返回操作或回答。"));
                json_object_put(reply);
                break;
            }
            progress_clear(a);
            if(quiet_reply){
                if(a->reply_out){
                    fputs(sget(reply,"content"),a->reply_out);
                    fputc('\n',a->reply_out);
                    fflush(a->reply_out);
                }
            }
            else if(streamed)putchar('\n');
            else printf("%s%s\n",reply_marker,ui_markdown(sget(reply,"content"),reply_indent).c_str());
            add(a->messages,reply);
            a->state="本轮结束";
            rc=0;
            break;
        }
        add(a->messages,reply);
        if(streamed)putchar('\n');
        else if(a->verbose&&*sget(reply,"content"))printf("%s%s\n",reply_marker,ui_markdown(sget(reply,"content"),reply_indent).c_str());
        for(size_t i=0;i<alen(calls);i++){
            J *call=at(calls,i),*fn=get(call,"function");
            if(interrupted){
                pending(a,calls,i);
                goto done;
            }
            const char *name=sget(fn,"name");
            J *args=parse_tool_arguments(sget(fn,"arguments"));
            if(a->verbose)show_call(name,args);
            else {
                const char *label=activity(name);
                if(!a->conversation_worker||last_activity!=label)progress(a,label);
                last_activity=label;
            }
            double tool_started=monotime();
            J *result=repeated_result(seen,name,args);
            const bool cached=result!=nullptr;
            if(!cached)result=tool_call(a,name,args);
            a->progress_line=false;
            bool denied=bget(result,"denied");
            bool failed=tool_failed(result);
            if(cached||repeated(seen,name,args,result)){
                put(result,"recovery_hint",str(tr("相同调用已三次返回相同结果。改用其他工具或参数，或先修复导致失败的条件。")));
            }
            if(!cached)recovery(a,unresolved,name,args,failed);
            a->error[0]=0;
            if(!cached&&!failed&&!strncmp(name,"desktop_",8)){
                if(mutation(name))desktop_unverified=true;
                else if(!strcmp(name,"desktop_screenshot")||*sget(args,"app"))desktop_unverified=false;
            }
            if(a->verbose)show_result(name,result,monotime()-tool_started);
            else if(failed){
                progress_clear(a);
                if(*sget(result,"error"))fprintf(stderr,"%s\n",sget(result,"error"));
                else fprintf(stderr,tr("操作失败（退出码 %d）。\n"),iget(result,"exit_code",-1));
            }
            if(a->trace){
                J *entry=obj();
                put(entry,"name",str(name));
                const char *target=*sget(args,"path")?sget(args,"path"):!strcmp(name,"run_shell")?sget(args,"command"):
                    !strcmp(name,"delegate_research")?sget(args,"question"):!strcmp(name,"web_fetch")?sget(args,"url"):
                    !strcmp(name,"web_search")?sget(args,"query"):sget(args,"app");
                if(*target)put(entry,"target",str(excerpt(target,240).c_str()));
                put(entry,"ok",json_object_new_boolean(!failed));
                if(failed&&*sget(result,"error"))put(entry,"error",str(excerpt(sget(result,"error"),240).c_str()));
                add(a->trace,entry);
            }
            tool_result(a,sget(call,"id"),result);
            json_object_put(args);
            json_object_put(result);
            if(denied||interrupted){
                pending(a,calls,i+1);
                if(denied)a->state="已取消";
                seterr(a,denied?tr("操作已取消。"):tr("任务已停止。"));
                goto done;
            }
        }
    }
    done:
    progress_clear(a);
    if(!a->error[0]&&rc)seterr(a,interrupted?tr("任务已停止。"):tr("达到步骤上限，任务未确认完成。"));
    if(interrupted)a->state="已取消";
    else if(rc&&strcmp(a->state,"已取消"))a->state="失败";
    if(rc&&!interrupted&&a->approval_unavailable)
        seterr(a,tr("需要批准，但当前没有可交互的终端；请使用 --approval edit 或 --approval full 重新运行。"));
    {
        // One closing line per turn: how long it worked, tokens used, outcome.
        const char *outcome=rc==0?tr("完成"):interrupted||!strcmp(a->state,"已取消")?tr("已停止"):tr("未完成");
        std::string line="  "+std::string(tr("用时"))+" "+elapsed_label(monotime()-task_started);
        if(a->task_tokens){
            char tokens[64];
            snprintf(tokens,sizeof tokens,tr(" · %s 词元"),token_label(a->task_tokens).c_str());
            line+=tokens;
        }
        line+=" · ";line+=outcome;
        const bool emphasize_summary=rc==0&&env_value("DESK_CONTRAST_NUMBERS");
        const Tone tone=rc!=0?Tone::Warning:emphasize_summary?Tone::Accent:Tone::Dim;
        printf("\n%s\n",ui_paint(tone,line,emphasize_summary).c_str());
        fflush(stdout);
    }
    history_checkpoint(*a,history_start+1);
    if(a->record)save_record(a);
    return rc;
}
void save_record(App *a){
    char base[PATH_MAX],directory[PATH_MAX],path[PATH_MAX],id[33],leaf[48];
    std::string state_path=state_base();
    if(state_path.size()>=sizeof base)return;
    strcpy(base,state_path.c_str());
    if(path_suffix(directory,base,"/desk-agent/sessions"))return;
    id_string(id);
    snprintf(leaf,sizeof leaf,"/%s.json",id);
    if(path_suffix(path,directory,leaf))return;
    J *record=obj();
    put(record,"version",str(VERSION));
    put(record,"state",str(a->state));
    put(record,"messages",copy(a->messages));
    const char *data=json_object_to_json_string_ext(record,JSON_C_TO_STRING_PRETTY);
    if(!mkdirs(directory,0700)&&!atomic_file(path,data,strlen(data),0600))printf(tr("会话记录：%s（分享前检查隐私内容）\n"),path);
    else fputs(tr("无法保存会话记录。\n"),stderr);
    json_object_put(record);
}
