#include "desk.hpp"
#include <mutex>
#include <set>
#include "tasks.hpp"
#include <curl/curl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <thread>
#include <chrono>
struct Buffer {
    std::string data;
    bool oversized=false;
};
// Incremental reader for server-sent events. Both protocols are reduced to the
// same three outputs (text, tool calls, stop reason), so the existing response
// validation can run unchanged on the assembled result.
struct StreamState {
    std::string raw;
    std::string pending;
    std::string event_data;
    std::string text;
    std::vector<std::string> ids,names,args;
    std::string stop_reason;
    std::string usage;
    CURL *http=nullptr;
    bool native=false, events=false, complete=false, invalid=false, oversized=false;
    std::function<void(const std::string &)> delta;
};
// Anthropic sends input tokens in message_start and output tokens in
// message_delta; keep both instead of letting the later event replace the first.
static void merge_usage(StreamState *st,J *usage){
    if(!json_object_is_type(usage,json_type_object))return;
    JsonPtr merged(st->usage.empty()?nullptr:json_tokener_parse(st->usage.c_str()));
    if(!json_object_is_type(merged.get(),json_type_object))merged.reset(obj());
    json_object_object_foreach(usage,key,value)put(merged.get(),key,copy(value));
    st->usage=json_object_to_json_string_ext(merged.get(),JSON_C_TO_STRING_PLAIN);
}
static void stream_event(StreamState *st,const char *json){
    st->events=true;
    if(st->complete){st->invalid=true;return;}
    if(!st->native&&!strcmp(json,"[DONE]")){st->complete=true;return;}
    JsonPtr event(json_tokener_parse(json));
    J *o=event.get();
    if(!json_object_is_type(o,json_type_object)||get(o,"error")){st->invalid=true;return;}
    const char *type=sget(o,"type");
    if(st->native&&!strcmp(type,"message_stop")){st->complete=true;return;}
    if(!strcmp(type,"error")){st->invalid=true;return;}
    if(!strcmp(type,"content_block_start")){
        J *block=get(o,"content_block");
        if(!strcmp(sget(block,"type"),"tool_use")){
            st->ids.push_back(sget(block,"id"));
            st->names.push_back(sget(block,"name"));
            st->args.emplace_back();
        }
    }else if(!strcmp(type,"content_block_delta")){
        J *d=get(o,"delta");
        const char *part=sget(d,"text");
        if(*part){
            st->text+=part;
            if(st->delta)st->delta(part);
        }
        if(*sget(d,"partial_json")&&!st->args.empty())st->args.back()+=sget(d,"partial_json");
    }else if(!strcmp(type,"message_delta")){
        const char *stop=sget(get(o,"delta"),"stop_reason");
        if(*stop)st->stop_reason=stop;
        merge_usage(st,get(o,"usage"));
    }else if(!strcmp(type,"message_start")){
        merge_usage(st,get(get(o,"message"),"usage"));
    }else{
        J *choice=at(get(o,"choices"),0);
        J *d=get(choice,"delta");
        const char *part=sget(d,"content");
        if(*part){
            st->text+=part;
            if(st->delta)st->delta(part);
        }
        J *calls=get(d,"tool_calls");
        for(size_t i=0;i<alen(calls);i++){
            J *call=at(calls,i),*fn=get(call,"function");
            int index=iget(call,"index",static_cast<int>(i));
            if(index<0||index>=12){st->invalid=true;return;}
            while(st->ids.size()<=static_cast<size_t>(index)){st->ids.emplace_back();st->names.emplace_back();st->args.emplace_back();}
            if(*sget(call,"id"))st->ids[index]=sget(call,"id");
            if(*sget(fn,"name"))st->names[index]+=sget(fn,"name");
            if(*sget(fn,"arguments"))st->args[index]+=sget(fn,"arguments");
        }
        const char *finish=sget(choice,"finish_reason");
        if(*finish)st->stop_reason=finish;
        J *usage=get(o,"usage");
        if(json_object_is_type(usage,json_type_object))st->usage=json_object_to_json_string_ext(usage,JSON_C_TO_STRING_PLAIN);
    }
}
static void stream_feed(StreamState *st,const char *bytes,size_t n){
    st->pending.append(bytes,n);
    std::string_view rem(st->pending);
    for(;;){
        auto end=rem.find('\n');
        if(end==std::string_view::npos)break;
        std::string_view line=rem.substr(0,end);
        rem.remove_prefix(end+1);
        if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
        if(line.empty()&&!st->event_data.empty()){
            stream_event(st,st->event_data.c_str());
            st->event_data.clear();
        }
        if(!line.starts_with("data:"))continue;
        std::string_view payload=line.substr(5);
        if(!payload.empty()&&payload.front()==' ')payload.remove_prefix(1);
        if(!st->event_data.empty())st->event_data+='\n';
        st->event_data.append(payload);
    }
    const size_t consumed=st->pending.size()-rem.size();
    if(consumed)st->pending.erase(0,consumed);
}
static size_t receive(char *p,size_t size,size_t count,void *user){
    auto *b=static_cast<Buffer *>(user);
    if(size&&count>SIZE_MAX/size)return 0;
    size_t n=size*count;
    if(n>RESPONSE_LIMIT-b->data.size()){
        b->oversized=true;
        return 0;
    }
    try { b->data.append(p,n); }
    catch(...) { return 0; } // Exceptions must not cross libcurl's C callback.
    return n;
}
static size_t stream_chunk(char *p,size_t size,size_t count,void *user){
    auto *st=static_cast<StreamState *>(user);
    if(size&&count>SIZE_MAX/size)return 0;
    size_t n=size*count;
    long status=0;
    curl_easy_getinfo(st->http,CURLINFO_RESPONSE_CODE,&status);
    if(status<200||status>=300)return n;
    if(n>RESPONSE_LIMIT-st->raw.size()){
        st->oversized=true;
        return 0;
    }
    try {
        st->raw.append(p,n);
        stream_feed(st,p,n);
    }
    catch(...) { return 0; }
    return n;
}
static int transfer_progress(void *u,curl_off_t d,curl_off_t n,curl_off_t t,curl_off_t v){
    auto *app=static_cast<App *>(u);
    if(app){
        check_exit(*app);
        progress_tick(app);
    }
    (void)d;
    (void)n;
    (void)t;
    (void)v;
    return app_cancelled(app)?1:0;
}
J *api_request(App *a,J *c,const char *endpoint,J *payload,void *stream_void) try {
    auto *stream=static_cast<StreamState *>(stream_void);
    if(config_valid(a,c))return NULL;
    char url[8192];
    if(snprintf(url,sizeof url,"%s%s",sget(c,"base_url"),endpoint)>=(int)sizeof url){
        seterr(a,tr("API 地址过长。"));
        return NULL;
    }
    bool native=!strcmp(sget(c,"protocol"),"anthropic");
    const char *body=payload?json_object_to_json_string_ext(payload,JSON_C_TO_STRING_PLAIN):NULL;
    if(!a->api_http)a->api_http.reset(curl_easy_init());
    if(!a->api_http){seterr(a,tr("无法初始化 HTTP。"));return nullptr;}
    for(int attempt=0;attempt<3;attempt++){
        CURL *h=a->api_http.get();
        Buffer b;
        HttpHeaders headers;
        headers.append(stream?"Accept: text/event-stream":"Accept: application/json");
        headers.append("Content-Type: application/json");
        headers.append(std::string(native?"x-api-key: ":"Authorization: Bearer ")+sget(c,"api_key"));
        if(native)headers.append(std::string("anthropic-version: ")+sget(c,"anthropic_version"));
        CurlRequest request(h);
        curl_easy_setopt(h,CURLOPT_URL,url);
        curl_easy_setopt(h,CURLOPT_HTTPHEADER,headers.get());
        curl_easy_setopt(h,CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(h,CURLOPT_CONNECTTIMEOUT,10L);
        // Long generations stream for minutes; the connect timeout still
        // bounds a dead endpoint, and a stalled transfer aborts on low speed.
        curl_easy_setopt(h,CURLOPT_TIMEOUT,600L);
        curl_easy_setopt(h,CURLOPT_LOW_SPEED_LIMIT,1L);
        curl_easy_setopt(h,CURLOPT_LOW_SPEED_TIME,120L);
        curl_easy_setopt(h,CURLOPT_NOSIGNAL,1L);
        if(stream){
            stream->http=h;
            curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,stream_chunk);
            curl_easy_setopt(h,CURLOPT_WRITEDATA,stream);
        }else{
            curl_easy_setopt(h,CURLOPT_WRITEFUNCTION,receive);
            curl_easy_setopt(h,CURLOPT_WRITEDATA,&b);
        }
        curl_easy_setopt(h,CURLOPT_XFERINFOFUNCTION,transfer_progress);
        curl_easy_setopt(h,CURLOPT_XFERINFODATA,a);
        curl_easy_setopt(h,CURLOPT_NOPROGRESS,0L);
        if(body){
            curl_easy_setopt(h,CURLOPT_POSTFIELDS,body);
            curl_easy_setopt(h,CURLOPT_POSTFIELDSIZE,(long)strlen(body));
        }
        CURLcode rc=curl_easy_perform(h);
        long status=0;
        curl_easy_getinfo(h,CURLINFO_RESPONSE_CODE,&status);
        if(rc!=CURLE_OK||app_cancelled(a)){
            seterr(a,app_cancelled(a)?tr("请求已取消。"):(stream?stream->oversized:b.oversized)?tr("API 响应超过大小限制。"):tr("API 连接失败或超时；未自动重发请求。"));
            return NULL;
        }
        if(status>=200&&status<300){
            const std::string &payload=stream?stream->raw:b.data;
            if(!valid_utf8(payload)){
                seterr(a,tr("API 返回的文本不是有效 UTF-8。"));
                return nullptr;
            }
            if(stream&&stream->events){
                if(stream->invalid||!stream->complete||stream->stop_reason.empty()||!stream->event_data.empty()){
                    seterr(a,tr("模型响应被截断，本轮工具未执行。"));
                    return nullptr;
                }
                return obj();
            }
            J *data=payload.empty()?nullptr:json_tokener_parse(payload.c_str());
            if(!data||!json_object_is_type(data,json_type_object)){
                json_object_put(data);
                seterr(a,tr("API 未返回有效 JSON 对象，请检查接口地址。"));
                return NULL;
            }
            return data;
        }
        bool retry=status==429||status==500||status==502||status==503||status==504||status==529;
        if(retry&&attempt<2&&!app_cancelled(a)){
            char message[160];
            snprintf(message,sizeof message,tr("API HTTP %ld，正在重试（%d/2）…"),status,attempt+1);
            if(a->progress_visible)progress(a,message);
            else if(!a->quiet_human)fprintf(stderr,"%s\n",message);
            auto until=monotime()+(1<<attempt);
            while(monotime()<until&&!app_cancelled(a)){check_exit(*a);progress_tick(a);std::this_thread::sleep_for(std::chrono::milliseconds(50));}
            if(app_cancelled(a)){
                seterr(a,tr("请求已取消。"));
                return NULL;
            }
            continue;
        }
        const char *help=status==401?tr("密钥无效或已过期。"):status==403?tr("没有模型或接口权限。"):status==404?tr("地址或模型不存在。"):status==429?tr("限流或额度不足。"):status==400?tr("接口类型或参数不兼容。"):tr("服务商请求失败。");
        if(status==400&&payload&&*sget(c,"reasoning_effort"))
            snprintf(a->error,sizeof a->error,"API HTTP 400：%s",tr("接口或模型可能不支持所选推理档位或工具组合；请调整档位或恢复服务商默认。未自动降档或更换模型。"));
        else snprintf(a->error,sizeof a->error,"API HTTP %ld：%s",status,help);
        return NULL;
    }
    return NULL;
}catch(const std::bad_alloc &){
    seterr(a,tr("无法分配请求头内存。"));
    return nullptr;
}
static void block_message(J *history,const char *role,J *blocks){
    size_t n=alen(history);
    J *last=n?at(history,n-1):NULL;
    if(last&&!strcmp(sget(last,"role"),role)){
        J *content=get(last,"content");
        for(size_t i=0;i<alen(blocks);i++)add(content,copy(at(blocks,i)));
        json_object_put(blocks);
    }
    else{
        J *m=obj();
        put(m,"role",str(role));
        put(m,"content",blocks);
        add(history,m);
    }
}
static J *textblock(const char *text){
    J *b=obj();
    put(b,"type",str("text"));
    put(b,"text",str(text));
    return b;
}
static J *anthropic_image_from_part(J *part){
    const char *ptype=sget(part,"type");
    if(!strcmp(ptype,"image_url")){
        std::string_view url=sget(get(part,"image_url"),"url");
        auto semi=url.find(";base64,");
        if(url.starts_with("data:")&&semi!=std::string_view::npos){
            std::string mime(url.substr(5,semi-5));
            std::string data(url.substr(semi+8));
            J *img=obj(),*src=obj();
            put(img,"type",str("image"));
            put(src,"type",str("base64"));
            put(src,"media_type",str(mime.c_str()));
            put(src,"data",str(data.c_str()));
            put(img,"source",src);
            return img;
        }
    }else if(!strcmp(ptype,"image")){
        if(get(part,"source"))return copy(part);
        if(*sget(part,"data")&&*sget(part,"mimeType")){
            J *img=obj(),*src=obj();
            put(img,"type",str("image"));
            put(src,"type",str("base64"));
            put(src,"media_type",str(sget(part,"mimeType")));
            put(src,"data",str(sget(part,"data")));
            put(img,"source",src);
            return img;
        }
    }
    return nullptr;
}
static J *openai_image_from_part(J *part){
    const char *ptype=sget(part,"type");
    if(!strcmp(ptype,"image_url")&&*sget(get(part,"image_url"),"url"))return copy(part);
    if(!strcmp(ptype,"image")&&*sget(part,"data")&&*sget(part,"mimeType")){
        std::string url=std::string("data:")+sget(part,"mimeType")+";base64,"+sget(part,"data");
        J *ib=obj(),*iu=obj();
        put(ib,"type",str("image_url"));
        put(iu,"url",str(url.c_str()));
        put(ib,"image_url",iu);
        return ib;
    }
    return nullptr;
}
static J *ephemeral_cache_marker(){
    J *cc=obj();
    put(cc,"type",str("ephemeral"));
    return cc;
}
J *native_payload(J *c,J *messages,J *tools,bool prompt_cache){
    J *body=obj(),*history=arr();
    put(body,"model",str(sget(c,"model")));
    put(body,"max_tokens",json_object_new_int(iget(c,"max_tokens",4096)));
    const char *effort=sget(c,"reasoning_effort");
    if(*effort){
        J *thinking=obj();put(thinking,"type",str(!strcmp(effort,"none")?"disabled":"adaptive"));put(body,"thinking",thinking);
        if(strcmp(effort,"none")){J *output=obj();put(output,"effort",str(effort));put(body,"output_config",output);}
    }
    put(body,"messages",history);
    for(size_t i=0;i<alen(messages);i++){
        J *m=at(messages,i);
        const char *role=sget(m,"role");
        if(!strcmp(role,"system")){
            if(prompt_cache&&*sget(m,"content")){
                J *sys_arr=arr(),*sb=textblock(sget(m,"content"));
                put(sb,"cache_control",ephemeral_cache_marker());
                add(sys_arr,sb);
                put(body,"system",sys_arr);
            }else{
                put(body,"system",copy(get(m,"content")));
            }
            continue;
        }
        J *blocks=arr();
        if(!strcmp(role,"user")){
            J *content=get(m,"content");
            if(json_object_is_type(content,json_type_array)){
                for(size_t k=0;k<alen(content);k++){
                    J *part=at(content,k);
                    const char *ptype=sget(part,"type");
                    if(!strcmp(ptype,"text"))add(blocks,textblock(sget(part,"text")));
                    else if(J *img=anthropic_image_from_part(part))add(blocks,img);
                }
            }else add(blocks,textblock(sget(m,"content")));
        }
        else if(!strcmp(role,"tool")){
            J *b=obj();
            put(b,"type",str("tool_result"));
            put(b,"tool_use_id",str(sget(m,"tool_call_id")));
            J *result=json_tokener_parse(sget(m,"content"));
            J *rblocks=get(result,"content");
            bool has_img=false;
            if(json_object_is_type(rblocks,json_type_array)){
                for(size_t k=0;k<alen(rblocks);k++)
                    if(!strncmp(sget(at(rblocks,k),"type"),"image",5))has_img=true;
            }
            if(has_img){
                J *tr_content=arr();
                for(size_t k=0;k<alen(rblocks);k++){
                    J *part=at(rblocks,k);
                    if(!strcmp(sget(part,"type"),"text"))add(tr_content,textblock(sget(part,"text")));
                    else if(J *img=anthropic_image_from_part(part))add(tr_content,img);
                }
                put(b,"content",tr_content);
            }else{
                put(b,"content",str(sget(m,"content")));
            }
            put(b,"is_error",json_object_new_boolean(tool_failed(result)));
            json_object_put(result);
            add(blocks,b);
            role="user";
        }
        else if(!strcmp(role,"assistant")){
            J *raw=get(m,"_anthropic_content");
            if(raw){
                json_object_put(blocks);
                blocks=copy(raw);
            }
            else{
                if(*sget(m,"content"))add(blocks,textblock(sget(m,"content")));
                J *calls=get(m,"tool_calls");
                for(size_t k=0;k<alen(calls);k++){
                    J *call=at(calls,k),*fn=get(call,"function"),*b=obj();
                    put(b,"type",str("tool_use"));
                    put(b,"id",str(sget(call,"id")));
                    put(b,"name",str(sget(fn,"name")));
                    J *args=json_tokener_parse(sget(fn,"arguments"));
                    put(b,"input",args?args:obj());
                    add(blocks,b);
                }
            }
        }
        if(alen(blocks))block_message(history,role,blocks);
        else json_object_put(blocks);
    }
    if(alen(tools)){
        J *defs=arr();
        for(size_t i=0;i<alen(tools);i++){
            J *f=get(at(tools,i),"function"),*d=obj();
            put(d,"name",str(sget(f,"name")));
            put(d,"description",str(sget(f,"description")));
            put(d,"input_schema",copy(get(f,"parameters")));
            if(prompt_cache&&i+1==alen(tools))put(d,"cache_control",ephemeral_cache_marker());
            add(defs,d);
        }
        put(body,"tools",defs);
        J *choice=obj();
        put(choice,"type",str("auto"));
        put(body,"tool_choice",choice);
    }
    if(prompt_cache){
        for(size_t i=alen(history);i>0;--i){
            J *hm=at(history,i-1);
            if(strcmp(sget(hm,"role"),"user"))continue;
            J *hc=get(hm,"content");
            if(alen(hc)>0)put(at(hc,alen(hc)-1),"cache_control",ephemeral_cache_marker());
            break;
        }
    }
    return body;
}
J *native_response(App *a,J *data){
    const char *stop=sget(data,"stop_reason");
    if(!strcmp(stop,"max_tokens")||!strcmp(stop,"model_context_window_exceeded")){
        seterr(a,tr("模型响应被截断，本轮工具未执行。"));
        return NULL;
    }
    if(strcmp(stop,"end_turn")&&strcmp(stop,"tool_use")&&strcmp(stop,"stop_sequence")){
        seterr(a,tr("模型拒绝或尚未结束响应，本轮工具未执行。"));
        return NULL;
    }
    J *blocks=get(data,"content");
    if(!json_object_is_type(blocks,json_type_array)){
        seterr(a,tr("Anthropic 响应缺少 content 数组。"));
        return NULL;
    }
    JsonPtr result(obj()),calls(arr());
    std::string text;
    for(size_t i=0;i<alen(blocks);i++){
        J *b=at(blocks,i);
        const char *type=sget(b,"type");
        if(!strcmp(type,"text")){
            const char *part=sget(b,"text");
            if(text.size()+strlen(part)+2>RESPONSE_LIMIT)goto bad;
            if(!text.empty())text+='\n';
            text+=part;
        }
        else if(!strcmp(type,"tool_use")){
            const char *id=sget(b,"id"),*name=sget(b,"name");
            if(!*id||!*name||!json_object_is_type(get(b,"input"),json_type_object))goto bad;
            for(size_t k=0;k<alen(calls.get());k++)if(!strcmp(sget(at(calls.get(),k),"id"),id))goto bad;
            J *call=obj(),*f=obj();
            put(call,"id",str(id));
            put(call,"type",str("function"));
            put(f,"name",str(name));
            put(f,"arguments",str(json_object_to_json_string_ext(get(b,"input"),JSON_C_TO_STRING_PLAIN)));
            put(call,"function",f);
            add(calls.get(),call);
        }
        else if(strcmp(type,"thinking")&&strcmp(type,"redacted_thinking"))goto bad;
    }
    if((!strcmp(stop,"tool_use"))!=(alen(calls.get())>0))goto bad;
    put(result.get(),"role",str("assistant"));
    put(result.get(),"content",text.empty()?nullptr:str(text.c_str()));
    put(result.get(),"_anthropic_content",copy(blocks));
    if(alen(calls.get()))put(result.get(),"tool_calls",calls.release());
    return result.release();
    bad:
    seterr(a,tr("Anthropic 工具或内容块格式无效，本轮工具未执行。"));
    return NULL;
}
// Prices are per million tokens in the profile's currency. A profile without
// prices is never charged a guessed amount; an OpenRouter-style usage.cost is
// used as reported when present.
static double price_field(J *config,const char *key,bool &present){
    J *value=get(config,key);
    if(!json_object_is_type(value,json_type_double)&&!json_object_is_type(value,json_type_int))return 0;
    const double number=json_object_get_double(value);
    if(!(number>=0)||number>1e9)return 0;
    present=true;
    return number;
}
bool usage_priced(J *config){
    bool input=false,output=false;
    price_field(config,"price_input",input);
    price_field(config,"price_output",output);
    return input||output;
}
TokenUsage usage_from_response(J *usage,bool native,J *config){
    auto count=[](J *object,const char *name){
        J *value=get(object,name);
        if(!json_object_is_type(value,json_type_int)||json_object_get_int64(value)<0)return std::uint64_t{0};
        return json_object_get_uint64(value);
    };
    TokenUsage used;
    if(native){
        used.input=add_tokens(count(usage,"input_tokens"),count(usage,"cache_creation_input_tokens"));
        used.cache_read=count(usage,"cache_read_input_tokens");
        used.output=count(usage,"output_tokens");
    }else{
        const std::uint64_t prompt=count(usage,"prompt_tokens"),completion=count(usage,"completion_tokens");
        std::uint64_t cached=count(get(usage,"prompt_tokens_details"),"cached_tokens");
        if(!cached)cached=count(usage,"prompt_cache_hit_tokens");
        cached=std::min(cached,prompt);
        used.input=prompt-cached;
        used.cache_read=cached;
        used.output=completion;
        // Some providers only send a total, others count extra reasoning
        // tokens there; keep the total as reported either way.
        const std::uint64_t total=count(usage,"total_tokens"),sum=add_tokens(prompt,completion);
        if(total>sum){
            if(sum)used.output=add_tokens(used.output,total-sum);
            else used.input=total;
        }
    }
    bool has_input=false,has_output=false,has_cache=false;
    const double input_price=price_field(config,"price_input",has_input);
    const double output_price=price_field(config,"price_output",has_output);
    const double cache_price=price_field(config,"price_cache_read",has_cache);
    if(has_input||has_output){
        used.cost=(static_cast<double>(used.input)*input_price+static_cast<double>(used.output)*output_price
                   +static_cast<double>(used.cache_read)*(has_cache?cache_price:input_price))/1e6;
        used.priced=true;
    }else if(J *reported=get(usage,"cost");json_object_is_type(reported,json_type_double)||json_object_is_type(reported,json_type_int)){
        const double cost=json_object_get_double(reported);
        if(cost>=0&&cost<1e6){used.cost=cost;used.priced=true;}
    }
    return used;
}
std::string usage_cost_label(J *config,double cost){
    std::string currency=usage_priced(config)?sget(config,"price_currency"):"";
    for(char &c:currency)c=static_cast<char>(toupper(static_cast<unsigned char>(c)));
    const std::string symbol=currency.empty()||currency=="USD"||currency=="$"?"$"
        :currency=="CNY"||currency=="RMB"||currency=="¥"?"¥":currency=="EUR"?"€":currency+" ";
    char number[64];
    snprintf(number,sizeof number,cost<0.01?"%.4f":cost<1?"%.3f":"%.2f",cost);
    return "≈"+symbol+number;
}
J *api_complete(App *a,J *c,J *messages,J *tools){
    bool native=!strcmp(sget(c,"protocol"),"anthropic");
    static std::mutex rejected_mutex;
    static std::set<std::string> usage_option_rejected;
    static std::set<std::string> cache_control_rejected;
    const std::string usage_key=std::string(sget(c,"profile"))+'\n'+sget(c,"base_url")+'\n'+sget(c,"model");
    bool ask_cache=false,ask_usage=false;
    {
        std::lock_guard lock(rejected_mutex);
        ask_cache=native&&!cache_control_rejected.contains(usage_key);
        ask_usage=!native&&!usage_option_rejected.contains(usage_key);
    }
    J *body;
    if(native)body=native_payload(c,messages,tools,ask_cache);
    else{
        body=obj();
        put(body,"model",str(sget(c,"model")));
        J *wire=arr();
        std::vector<JsonPtr> pending_tool_images;
        for(size_t i=0;i<alen(messages);i++){
            J *clean=copy(at(messages,i));
            std::vector<std::string> internal_keys;
            json_object_object_foreach(clean,key,val){
                (void)val;
                if(key[0]=='_')internal_keys.emplace_back(key);
            }
            for(const auto &k:internal_keys)json_object_object_del(clean,k.c_str());
            if(!strcmp(sget(clean,"role"),"tool")){
                JsonPtr parsed(json_tokener_parse(sget(clean,"content")));
                J *rblocks=get(parsed.get(),"content");
                bool stripped=false;
                if(json_object_is_type(rblocks,json_type_array)){
                    for(size_t k=0;k<alen(rblocks);k++){
                        J *part=at(rblocks,k);
                        if(J *img=openai_image_from_part(part)){
                            pending_tool_images.emplace_back(img);
                            json_object_object_del(part,"image_url");
                            json_object_object_del(part,"data");
                            json_object_object_del(part,"mimeType");
                            put(part,"type",str("text"));
                            put(part,"text",str("[Image attached below]"));
                            stripped=true;
                        }
                    }
                }
                if(stripped)
                    put(clean,"content",str(json_object_to_json_string_ext(parsed.get(),JSON_C_TO_STRING_PLAIN)));
            }
            add(wire,clean);
            const bool next_is_tool=(i+1<alen(messages))&&!strcmp(sget(at(messages,i+1),"role"),"tool");
            if(!next_is_tool&&!pending_tool_images.empty()){
                J *vision_msg=obj(),*vblocks=arr();
                put(vision_msg,"role",str("user"));
                add(vblocks,textblock("Attached image from tool result:"));
                for(auto &img:pending_tool_images)add(vblocks,img.release());
                pending_tool_images.clear();
                put(vision_msg,"content",vblocks);
                add(wire,vision_msg);
            }
        }
        put(body,"messages",wire);
        if(*sget(c,"reasoning_effort"))put(body,"reasoning_effort",str(sget(c,"reasoning_effort")));
        if(alen(tools)){
            put(body,"tools",copy(tools));
            put(body,"tool_choice",str("auto"));
        }
    }
    StreamState stream;
    stream.native=native;
    // Streamed text is handed to the caller as it arrives; tool arguments and
    // the stop reason are assembled into a normal response below so the same
    // validation runs for both paths.
    stream.delta=[a](const std::string &part){
        if(a->stream_text)a->stream_text(part);
        if(a->stream_observer){a->streamed_chunks++;a->stream_observer(part);}
    };
    put(body,"stream",json_object_new_boolean(1));
    // OpenAI only reports usage on a stream when asked. An endpoint and model
    // that reject the option get one plain retry and are not asked again in
    // this process; other providers and models keep requesting usage.
    if(ask_usage){
        J *options=obj();
        put(options,"include_usage",json_object_new_boolean(1));
        put(body,"stream_options",options);
    }
    J *data=api_request(a,c,native?"/messages":"/chat/completions",body,&stream);
    if(!data&&!app_cancelled(a)&&!strncmp(a->error,"API HTTP 400",12)&&!stream.events&&(ask_usage||ask_cache)){
        // Retry once without the optional extension; only a success proves the
        // option was the problem. Otherwise the first, more specific error stands.
        const std::string first_error=a->error;
        if(ask_usage)json_object_object_del(body,"stream_options");
        if(ask_cache){
            json_object_put(body);
            body=native_payload(c,messages,tools,false);
            put(body,"stream",json_object_new_boolean(1));
        }
        stream=StreamState{};
        stream.native=native;
        stream.delta=[a](const std::string &part){
            if(a->stream_text)a->stream_text(part);
            if(a->stream_observer){a->streamed_chunks++;a->stream_observer(part);}
        };
        a->error[0]=0;
        data=api_request(a,c,native?"/messages":"/chat/completions",body,&stream);
        if(data){
            std::lock_guard lock(rejected_mutex);
            if(ask_usage)usage_option_rejected.insert(usage_key);
            if(ask_cache)cache_control_rejected.insert(usage_key);
        }
        else if(!app_cancelled(a))snprintf(a->error,sizeof a->error,"%s",first_error.c_str());
    }
    json_object_put(body);
    if(!data)return NULL;
    if(stream.events){
        if(!native&&stream.stop_reason!="length"&&stream.stop_reason!=(stream.ids.empty()?"stop":"tool_calls")){
            json_object_put(data);
            seterr(a,tr("模型拒绝或尚未结束响应，本轮工具未执行。"));
            return nullptr;
        }
        // Rebuild the response the non-streaming path expects.
        J *usage=json_tokener_parse(stream.usage.c_str());
        put(data,"usage",usage?usage:obj());
        if(native){
            J *blocks=arr();
            if(!stream.text.empty()){J *b=obj();put(b,"type",str("text"));put(b,"text",str(stream.text.c_str()));add(blocks,b);}
            for(size_t i=0;i<stream.ids.size();i++){
                J *b=obj();put(b,"type",str("tool_use"));put(b,"id",str(stream.ids[i].c_str()));put(b,"name",str(stream.names[i].c_str()));
                J *input=stream.args[i].empty()?obj():parse_tool_arguments(stream.args[i].c_str());
                put(b,"input",input);
                add(blocks,b);
            }
            put(data,"content",blocks);
            put(data,"stop_reason",str(stream.stop_reason.c_str()));
        }else{
            J *choices=arr(),*choice=obj(),*message=obj();
            put(message,"role",str("assistant"));
            if(!stream.text.empty())put(message,"content",str(stream.text.c_str()));
            J *calls=arr();
            for(size_t i=0;i<stream.ids.size();i++){
                J *call=obj(),*f=obj();
                put(call,"id",str(stream.ids[i].c_str()));
                put(call,"type",str("function"));
                put(f,"name",str(stream.names[i].c_str()));
                put(f,"arguments",json_object_new_string_len(stream.args[i].data(),static_cast<int>(stream.args[i].size())));
                put(call,"function",f);
                add(calls,call);
            }
            if(alen(calls))put(message,"tool_calls",calls);
            else json_object_put(calls);
            put(choice,"message",message);
            put(choice,"finish_reason",str(stream.stop_reason.c_str()));
            add(choices,choice);
            put(data,"choices",choices);
        }
    }
    const TokenUsage used=usage_from_response(get(data,"usage"),native,c);
    a->turn_usage.add(used);
    a->session_usage.add(used);
    a->task_tokens=add_tokens(a->task_tokens,used.total());
    task_tokens(*a);
    task_usage(*a);
    if(!record_tokens(*a,c,used.total()))
        fputs(tr("无法保存词元用量。\n"),stderr);
    J *result=NULL;
    if(native)result=native_response(a,data);
    else{
        J *choice=at(get(data,"choices"),0);
        if(!strcmp(sget(choice,"finish_reason"),"length"))seterr(a,tr("模型响应被截断，本轮工具未执行。"));
        else if(get(get(choice,"message"),"refusal"))seterr(a,tr("模型拒绝处理本次请求。"));
        else if(json_object_is_type(get(choice,"message"),json_type_object)){
            result=copy(get(choice,"message"));
            if(get(result,"role")&&strcmp(sget(result,"role"),"assistant")){
                json_object_put(result);
                result=NULL;
                seterr(a,tr("模型响应的消息角色无效。"));
            }else if(result)put(result,"role",str("assistant"));
        }
        else seterr(a,tr("Chat Completions 响应格式无效。"));
    }
    json_object_put(data);
    return result;
}
J *api_models(App *a,J *c){
    J *data=api_request(a,c,!strcmp(sget(c,"protocol"),"anthropic")?"/models?limit=1000":"/models",NULL);
    if(!data)return NULL;
    J *entries=get(data,"data"),*out=arr();
    if(!json_object_is_type(entries,json_type_array)){
        seterr(a,tr("服务商不支持模型列表；可手动填写模型名称。"));
        json_object_put(out);
        out=NULL;
    }
    else for(size_t i=0;i<alen(entries)&&i<500;i++){
        const char *id=sget(at(entries,i),"id");
        if(*id)add(out,str(id));
    }
    json_object_put(data);
    return out;
}
int api_check(App *a,J *c){
    J *messages=arr(),*m=obj();
    put(m,"role",str("user"));
    put(m,"content",str("Call connection_check with empty arguments."));
    add(messages,m);
    J *tools=arr(),*t=obj(),*f=obj(),*parameters=obj();
    put(t,"type",str("function"));
    put(f,"name",str("connection_check"));
    put(f,"description",str("Check tool support without operating the computer."));
    put(parameters,"type",str("object"));
    put(parameters,"properties",obj());
    put(parameters,"required",arr());
    put(parameters,"additionalProperties",json_object_new_boolean(0));
    put(f,"parameters",parameters);
    put(t,"function",f);
    add(tools,t);
    J *reply=api_complete(a,c,messages,tools);
    json_object_put(messages);
    json_object_put(tools);
    if(!reply)return -1;
    int ok=-1;
    J *calls=get(reply,"tool_calls");
    for(size_t i=0;i<alen(calls);i++){
        J *call=at(calls,i),*fn=get(call,"function"),*args=json_tokener_parse(sget(fn,"arguments"));
        if(*sget(call,"id")&&!strcmp(sget(fn,"name"),"connection_check")&&json_object_is_type(args,json_type_object)&&json_object_object_length(args)==0)ok=0;
        json_object_put(args);
    }
    json_object_put(reply);
    if(ok)seterr(a,tr("接口响应成功，但未验证工具调用能力；原配置未改动。"));
    return ok;
}
