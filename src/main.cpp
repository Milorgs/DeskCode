#include "desk.hpp"
#include "tasks.hpp"
#include "mcp.hpp"
#include "ui.hpp"
#include <curl/curl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifndef _WIN32
#include <unistd.h>
#include <sys/ioctl.h>
#include <poll.h>
#include <wchar.h>
#endif
#include <locale.h>
#include <algorithm>
#include <optional>
#include <cstdarg>
#include <cerrno>
#include <cctype>

#ifndef _WIN32
static void on_signal(int number){
    interrupted=number;
}
#endif
// One alternate screen owns every interactive page, including the main menu.
static bool interface_active=false;
static int content_top=1;
static std::string page_notice;
static void set_notice(const std::string &text){
    if(interface_active){page_notice=text;while(!page_notice.empty()&&page_notice.back()=='\n')page_notice.pop_back();}
    else puts(text.c_str());
}
static void show_notice(){
    if(!page_notice.empty()){terminal_message(page_notice+"\n\n");page_notice.clear();}
}
static bool roomy_interface(){
    // Room for the banner plus a full menu; a default 90x26 Konsole window must
    // still get rules and spacing, so keep the bar low.
    return interface_active&&terminal_columns()>=60&&terminal_rows()>=20;
}
static bool spaced_interface(){
    // Blank lines between options cost a row each. A default 90x26 Konsole
    // window fits the banner, model list and a six-item menu, so allow them.
    return roomy_interface()&&terminal_rows()>=24&&terminal_columns()>=70;
}
static void menu_text(const char *text){
    const bool roomy=spaced_interface(),color=interface_active&&terminal_supports_color();
    bool heading=*text=='\n';
    std::string_view rest(text);
    do{
        auto end=rest.find('\n');auto line=rest.substr(0,end);
        bool option=line.starts_with("  [");
        bool title=heading&&!line.empty()&&line.front()!=' ';
        if(interface_active&&(option||title))fputs(title?"    ":"  ",stdout);
        if(color&&option&&line.find("] ")!=line.npos){
            auto split=line.find("] ")+1;
            fputs(ui_number(line.substr(0,split)).c_str(),stdout);
            fputs(ui_paint(Tone::Accent,line.substr(split)).c_str(),stdout);
        }
        else if(color&&title)fputs(ui_paint(Tone::Plain,line,true).c_str(),stdout);
        else fwrite(line.data(),1,line.size(),stdout);
        putchar('\n');
        if(roomy&&(option||title))putchar('\n');
        if(!line.empty())heading=false;
        if(end==std::string_view::npos)break;
        rest.remove_prefix(end+1);
    }while(true);
}
static void menu_printf(const char *format,...){
    va_list args;va_start(args,format);va_list sizing;va_copy(sizing,args);
    int count=vsnprintf(nullptr,0,format,sizing);va_end(sizing);
    if(count>=0){std::vector<char> text(static_cast<size_t>(count)+1);vsnprintf(text.data(),text.size(),format,args);if(count&&text[static_cast<size_t>(count)-1]=='\n')text[static_cast<size_t>(count)-1]=0;menu_text(text.data());}
    va_end(args);
}
static void page_rule(){
    if(!interface_active||terminal_columns()<40||terminal_rows()<16)return;
    const int width=std::min(terminal_columns()-4,100);
    if(width<4)return;
    std::string line;
    for(int i=0;i<width;i++)line+="─";
    fputs("  ",stdout);
    fputs(ui_paint(Tone::Rule,line).c_str(),stdout);
    fputc('\n',stdout);
}
static void clear_page(){
    if(interface_active){printf("\033[%d;1H\033[J",content_top);fflush(stdout);}
}
class InterfaceScreen {
public:
    InterfaceScreen(){
        interface_active=terminal_supports_ansi();
        if(interface_active){fputs("\033[?1049h\033[r\033[2J\033[H",stdout);fflush(stdout);}
#ifndef _WIN32
        // Ctrl+Z would stop the process with the alternate screen, scroll
        // region and mouse reporting still active in the parent shell.
        previous_stop=signal(SIGTSTP,SIG_IGN);
#endif
    }
    ~InterfaceScreen(){
#ifndef _WIN32
        signal(SIGTSTP,previous_stop==SIG_ERR?SIG_DFL:previous_stop);
#endif
        if(interface_active){fputs("\033[r\033[0m\033[?1049l",stdout);fflush(stdout);}
        interface_active=false;content_top=1;
    }
private:
#ifndef _WIN32
    void (*previous_stop)(int)=SIG_DFL;
#endif
};
static void status(App *a){
    const int width=terminal_columns()>0?std::max(24,std::min(terminal_columns()-4,88)):0;
    printf("\n  %s\n\n",ui_paint(Tone::Bold,tr("DeskCode 状态")).c_str());
    auto row=[&](const char *label,const char *value){
        std::string lead=tr(label);
        while(ui_width(lead)<8)lead+=' ';
        fputs("  ",stdout);
        fputs(ui_paint(Tone::Dim,lead).c_str(),stdout);
        fputs(ui_paint(Tone::Plain,ui_clip(value,width>0?std::max(12,width-10):0)).c_str(),stdout);
        putchar('\n');
    };
    row("模型",sget(a->config,"model"));
    row("接口",sget(a->config,"protocol"));
    row("目录",a->root);
    row("批准",permission_name(*a));
    row("推理",reasoning_name(sget(a->config,"reasoning_effort")));
}
// In interactive arrow-menu mode, Ctrl+D at a text field cancels just that
// field instead of tearing down the whole menu loop; a hung-up pty stays closed.
static void recover_interactive_eof(){
    if(!input_closed||interrupted==SIGTERM||env_value("DESK_NO_ARROW")||!isatty(STDIN_FILENO))return;
#ifndef _WIN32
    pollfd pfd{STDIN_FILENO,POLLIN,0};
    if(poll(&pfd,1,0)>0&&(pfd.revents&(POLLHUP|POLLERR|POLLNVAL)))return;
#endif
    input_closed=false;
}
static char *field(const char *name,const char *fallback,bool secret){
    char prompt[8192];
    if(secret)snprintf(prompt,sizeof prompt,tr("%s%s："),name,fallback&&*fallback?tr(" [回车保留]"):"");
    else snprintf(prompt,sizeof prompt,tr("%s [%s]："),name,fallback?fallback:"");
    char *value=input(prompt,fallback,secret);
    if(!value)recover_interactive_eof();
    return value;
}
static int menu_choices(Tasks &jobs,const char *options,const char *details=nullptr,int initial=-1);
static const char *choose_protocol(Tasks &jobs,const char *current){
    clear_page();
    int choice=menu_choices(jobs,tr("\n  [1] OpenAI接口\n  [2] Anthropic接口\n  [0] 返回"),nullptr,!strcmp(current,"anthropic")?2:1);
    return choice==1?"openai":choice==2?"anthropic":nullptr;
}
static void use_reasoning(J *config,J *value){
    if(value&&valid_reasoning_effort(config,value)&&*json_object_get_string(value))
        put(config,"reasoning_effort",copy(value));
    else json_object_object_del(config,"reasoning_effort");
}
static J *profile_reasoning(const App &app){
    J *profile=get(get(app.store,"profiles"),sget(app.config,"profile"));
    return get(profile,"reasoning_effort");
}
static J *editable_model_config(const App &app){
    J *config=copy(app.config);
    if(!app.conversation_id.empty())use_reasoning(config,profile_reasoning(app));
    return config;
}
// Ready-made provider entries so a first run needs only an API key. Every
// preset is a plain OpenAI-compatible or Anthropic endpoint; nothing is added
// to requests beyond what the user configures.
struct ProviderPreset {const char *label,*slug,*protocol,*base_url,*model,*key;};
static const ProviderPreset provider_presets[]={
    {"OpenAI接口","","openai","https://api.openai.com/v1","",""},
    {"Anthropic接口","","anthropic","https://api.anthropic.com/v1","",""},
    {"DeepSeek","deepseek","openai","https://api.deepseek.com/v1","deepseek-chat",""},
    {"Kimi（月之暗面）","kimi","openai","https://api.moonshot.cn/v1","",""},
    {"通义千问（阿里云百炼）","qwen","openai","https://dashscope.aliyuncs.com/compatible-mode/v1","qwen-plus",""},
    {"智谱 GLM","glm","openai","https://open.bigmodel.cn/api/paas/v4","glm-4-plus",""},
    {"硅基流动 SiliconFlow","siliconflow","openai","https://api.siliconflow.cn/v1","",""},
    {"OpenRouter","openrouter","openai","https://openrouter.ai/api/v1","",""},
    {"Ollama 本地模型","ollama","openai","http://127.0.0.1:11434/v1","","ollama"},
};
static int number(Tasks &jobs,int maximum,int initial=-1);
static int arrow_menu(Tasks &jobs,const std::vector<std::string> &items,int initial=-1,bool allow_escape=true,const std::function<void()> &on_resize={});
// Offer the provider's live model list when it answers, otherwise fall back to
// typing an ID. Listing models is a free GET and sends no conversation data.
static bool choose_model_id(App *a,Tasks &jobs,J *candidate,const char *suggestion){
    std::string current=sget(candidate,"model");
    if(current.empty()&&suggestion)current=suggestion;
    menu_text(tr("正在获取在线模型列表…"));
    fflush(stdout);
    JsonPtr probe(copy(candidate));
    if(!*sget(probe.get(),"model"))put(probe.get(),"model",str("model-list"));
    a->error[0]=0;
    JsonPtr models(api_models(a,probe.get()));
    a->error[0]=0;
    const size_t count=alen(models.get());
    if(count){
        std::vector<std::string> options;
        int initial=1;
        for(size_t i=0;i<count;i++){
            options.emplace_back(json_object_get_string(at(models.get(),i)));
            if(options.back()==current)initial=static_cast<int>(i)+1;
        }
        options.emplace_back(tr("手动输入模型 ID"));
        options.emplace_back(tr("返回"));
        char heading[128];
        snprintf(heading,sizeof heading,tr("\n选择模型（共 %zu 个）"),count);
        menu_text(heading);
        int choice=arrow_menu(jobs,options,initial);
        if(choice<=0)return false;
        if(static_cast<size_t>(choice)<=count){
            put(candidate,"model",str(options[static_cast<size_t>(choice-1)].c_str()));
            return true;
        }
    }else menu_text(tr("未获取到在线模型列表，请手动输入模型 ID。"));
    char *model=field(tr("模型名称"),current.c_str(),false);
    if(!model)return false;
    put(candidate,"model",str(model));
    free(model);
    return true;
}
static int configure(App *a,Tasks &jobs,const char *name,const ProviderPreset *preset,bool fresh){
    if(!isatty(STDIN_FILENO)){
        seterr(a,tr("配置向导需要交互终端；也可通过 DESK_* 环境变量配置。"));
        return -1;
    }
    if(!config_name_valid(name)){
        seterr(a,tr("模型配置名无效，不含空格或路径符号。"));
        return -1;
    }
    if(fresh&&get(get(a->store,"profiles"),name)){
        seterr(a,tr("配置名已存在，请选择另一个名称。"));
        return -1;
    }
    JsonPtr candidate_owner(fresh?config_default(name):editable_model_config(*a));
    J *candidate=candidate_owner.get();
    if(!candidate)return -1;
    const char *protocol=preset?preset->protocol:choose_protocol(jobs,sget(candidate,"protocol"));
    if(!protocol){
        return 0;
    }
    bool changed=strcmp(sget(candidate,"protocol"),protocol)!=0;
    put(candidate,"protocol",str(protocol));
    if(changed&&!valid_reasoning_effort(candidate,get(candidate,"reasoning_effort")))
        use_reasoning(candidate,nullptr);
    if(preset&&fresh)put(candidate,"base_url",str(preset->base_url));
    else if(changed)put(candidate,"base_url",str(!strcmp(protocol,"anthropic")?"https://api.anthropic.com/v1":"https://api.openai.com/v1"));
    menu_text(tr("\n密钥输入不显示，仅保存在本机私有配置文件。"));
    char *base=field(tr("API 地址"),sget(candidate,"base_url"),false);
    if(!base)return 0;
    bool host_changed=changed;
    CURLU *old=curl_url(),*next=curl_url();
    char *h1=NULL,*h2=NULL,*s1=NULL,*s2=NULL,*p1=NULL,*p2=NULL;
    curl_url_set(old,CURLUPART_URL,sget(a->config,"base_url"),0);
    curl_url_set(next,CURLUPART_URL,base,0);
    curl_url_get(old,CURLUPART_HOST,&h1,0);
    curl_url_get(next,CURLUPART_HOST,&h2,0);
    curl_url_get(old,CURLUPART_SCHEME,&s1,0);
    curl_url_get(next,CURLUPART_SCHEME,&s2,0);
    curl_url_get(old,CURLUPART_PORT,&p1,CURLU_DEFAULT_PORT);
    curl_url_get(next,CURLUPART_PORT,&p2,CURLU_DEFAULT_PORT);
    host_changed=host_changed||!h1||!h2||strcmp(h1,h2)||!s1||!s2||strcmp(s1,s2)||!p1||!p2||strcmp(p1,p2);
    curl_free(h1);
    curl_free(h2);
    curl_free(s1);
    curl_free(s2);
    curl_free(p1);
    curl_free(p2);
    curl_url_cleanup(old);
    curl_url_cleanup(next);
    put(candidate,"base_url",str(base));
    free(base);
    // A key never follows the configuration to a different host.
    if(host_changed)put(candidate,"api_key",str(""));
    if(fresh&&preset&&*preset->key)put(candidate,"api_key",str(preset->key));
    char *key=field("API Key",sget(candidate,"api_key"),true);
    if(!key)return 0;
    put(candidate,"api_key",str(key));
    free(key);
    if(!choose_model_id(a,jobs,candidate,preset?preset->model:nullptr))return 0;
    if(config_valid(a,candidate)){
        return -1;
    }
    int action=menu_choices(jobs,tr("\n  [1] 测试连接并保存（少量 API 用量）\n  [2] 直接保存，稍后测试\n  [0] 取消"),nullptr,1);
    if(action<=0)return 0;
    if(action==1&&api_check(a,candidate))return -1;
    if(action==2)menu_text(tr("配置尚未联网验证。"));
    int rc=save_config(a,candidate);
    if(!rc)set_notice(tr("已保存并选用该模型。"));
    return rc;
}
static int add_model(App *a,Tasks &jobs,const char *preset_name){
    clear_page();
    menu_text(tr("\n添加模型 · 选择服务商"));
    std::vector<std::string> options;
    for(const auto &preset:provider_presets)options.emplace_back(tr(preset.label));
    options.emplace_back(tr("返回"));
    int choice=arrow_menu(jobs,options,1);
    if(choice<=0)return 0;
    const ProviderPreset &preset=provider_presets[static_cast<size_t>(choice-1)];
    char fallback[64];
    int number=1;
    if(*preset.slug){
        snprintf(fallback,sizeof fallback,"%s",preset.slug);
        while(get(get(a->store,"profiles"),fallback))snprintf(fallback,sizeof fallback,"%s-%d",preset.slug,++number);
    }else{
        do{
            snprintf(fallback,sizeof fallback,"model-%d",number++);
        }
        while(get(get(a->store,"profiles"),fallback));
    }
    char *name=preset_name?strdup(preset_name):field(tr("模型配置名称（输入 0 返回）"),fallback,false);
    if(!name)return 0;
    int rc=strcmp(name,"0")?configure(a,jobs,name,&preset,true):0;
    free(name);
    return rc;
}
static int select_model(App *a,Tasks &jobs){
    clear_page();
    J *profiles=get(a->store,"profiles");
    if(!json_object_object_length(profiles)){
        set_notice(tr("尚未保存模型，请选择 [3] 添加模型。"));
        return 0;
    }
    std::vector<std::string> order,labels;
    menu_text(tr("\n选择模型："));
    json_object_object_foreach(profiles,name,entry){
        order.emplace_back(name);
        labels.push_back(std::string(sget(entry,"model"))+" ("+sget(entry,"protocol")+")"+(same_model(entry,a->config)?tr(" · 当前"):""));
    }
    int count=static_cast<int>(order.size());
    auto options=labels;
    if(count>1)options.emplace_back(tr("调整模型编号"));
    options.emplace_back(tr("返回"));
    int selected=arrow_menu(jobs,options);
    if(selected<=0)return 0;
    if(selected<=count){
        int result=activate(a,order[static_cast<size_t>(selected-1)].c_str());
        if(!result){
            char message[512];snprintf(message,sizeof message,tr("已选择模型：%s（下次启动默认使用）。"),sget(a->config,"model"));
            std::string notice=message;
            JsonPtr states(jobs.snapshot());
            for(size_t i=0;i<alen(states.get());i++)if(bget(at(states.get(),i),"active")){
                notice+="\n"+std::string(tr("后续及排队需求使用新模型；正在执行的任务继续使用原模型。"));break;
            }
            set_notice(notice);
        }
        return result;
    }

    menu_text(tr("输入要调整的模型编号（0 返回）。"));
    labels.emplace_back(tr("返回"));
    int from=arrow_menu(jobs,labels);
    if(from<=0)return 0;
    menu_text(tr("输入该模型的新编号（0 返回）。"));
    int to=arrow_menu(jobs,labels);
    if(to<=0||from==to)return 0;
    auto source=order.begin()+(from-1),target=order.begin()+(to-1);
    if(source<target)std::rotate(source,source+1,target+1);
    else std::rotate(target,source,source+1);
    return reorder_models(a,order);
}
static int number(Tasks &jobs,int maximum,int initial){
    for(;;){
        if(input_closed||interrupted==SIGTERM)return -1;
        interrupted=0;
        std::string prompt=std::string(roomy_interface()?"    ":"")+tr("请选择编号：");
        std::unique_ptr<char,decltype(&free)> value(jobs.readline(prompt.c_str()),free);
        if(!value){if(interrupted==SIGINT)continue;return -1;}
        char *start=value.get();while(isspace(static_cast<unsigned char>(*start)))start++;
        if(!*start&&initial>=0)return initial;
        char *end=nullptr;errno=0;long selected=strtol(start,&end,10);
        bool digits=end!=start;while(isspace(static_cast<unsigned char>(*end)))end++;
        if(digits&&errno!=ERANGE&&!*end&&selected>=0&&selected<=maximum)return static_cast<int>(selected);
        menu_text(tr("请输入列表中的编号。"));
    }
}
static std::string request_text(Tasks &jobs,const char *prompt){
    std::unique_ptr<char,decltype(&free)> text(jobs.readline(prompt),free);
    if(!text)recover_interactive_eof();
    return text?text.get():"";
}
static std::optional<int> arrow_choice(Tasks &jobs,const std::vector<std::string> &items,int initial=1,bool allow_escape=true,const std::function<void()> &on_resize={}){
    return terminal_choice(items,initial,content_top,[&]{
        jobs.refresh();
        if(jobs.on_idle)jobs.on_idle();
    },false,false,allow_escape,on_resize);
}
static int arrow_menu(Tasks &jobs,const std::vector<std::string> &items,int initial,bool allow_escape,const std::function<void()> &on_resize){
    if(auto choice=arrow_choice(jobs,items,initial<0?1:initial,allow_escape,on_resize))return *choice;
    for(size_t i=0;i<items.size();i++)menu_printf("  [%zu] %s\n",(i+1)%items.size(),items[i].c_str());
    return number(jobs,static_cast<int>(items.size()-1),initial);
}
// Static menus keep their existing localized labels and numbered fallback.
static int menu_choices(Tasks &jobs,const char *options,const char *details,int initial){
    std::vector<std::string> items;
    std::string heading;
    for(std::string_view lines(options);!lines.empty();){
        auto end=lines.find('\n');
        auto line=lines.substr(0,end);
        auto label=line.find("] ");
        if(line.starts_with("  [")&&label!=line.npos)items.emplace_back(line.substr(label+2));
        else {heading.append(line);heading+='\n';}
        if(end==lines.npos)break;
        lines.remove_prefix(end+1);
    }
    if(!heading.empty())menu_text(heading.c_str());
    if(details)menu_text(details);
    return arrow_menu(jobs,items,initial);
}
static void language_menu(App *a,Tasks &jobs){
    clear_page();
    int choice=menu_choices(jobs,tr("\n界面语言\n  [1] 简体中文\n  [2] English\n  [0] 返回"));
    if(choice==1&&save_language(*a,"zh"))set_notice(tr("语言已切换为简体中文。"));
    else if(choice==2&&save_language(*a,"en"))set_notice(tr("语言已切换为 English。"));
}
static void model_id_menu(App *a,Tasks &jobs){
    clear_page();
    int choice=menu_choices(jobs,tr("\n切换模型 ID\n  [1] 从在线列表选择\n  [2] 手动输入\n  [0] 返回"));std::string model;
    if(choice==1){
        JsonPtr models(api_models(a,a->config));
        if(!models){fprintf(stderr,"%s\n",a->error);return;}
        std::vector<std::string> options;
        for(size_t i=0;i<alen(models.get());i++)options.emplace_back(json_object_get_string(at(models.get(),i)));
        options.emplace_back(tr("返回"));
        int index=arrow_menu(jobs,options);
        if(index<=0)return;
        model=json_object_get_string(at(models.get(),static_cast<size_t>(index-1)));
    }else if(choice==2)model=request_text(jobs,tr("模型 ID（回车返回）："));
    if(model.empty())return;
    JsonPtr candidate(editable_model_config(*a));put(candidate.get(),"model",str(model.c_str()));
    if(save_config(a,candidate.get()))fprintf(stderr,"%s\n",a->error);
    else set_notice(tr("已保存模型设置。"));
}
static void approval_menu(App *a,Tasks &jobs){
    clear_page();
    const char *options=tr("\n批准模式\n  [1] 请求批准\n  [2] 帮我批准\n  [3] 完全访问权限\n  [0] 返回");
    const char *details=tr("请求批准：修改、命令和桌面操作前确认。帮我批准：自动批准工作目录内文件编辑，命令和桌面操作仍确认。完全访问权限：自动批准已启用工具，文件工具仍限工作目录，命令以当前用户权限运行。");
    int selected=menu_choices(jobs,options,details);if(selected<=0)return;
    a->yes=selected==3;a->auto_edit=selected==2;
    char message[256];
    const char *format=a->conversation_id.empty()?tr("批准模式：%s（本次运行）\n"):tr("本对话的批准模式：%s\n");
    snprintf(message,sizeof message,format,permission_name(*a));
    set_notice(message);
}
static void reasoning_menu(App *a,Tasks &jobs){
    clear_page();
    bool native=!strcmp(sget(a->config,"protocol"),"anthropic");
    const char *openai[]={"none","minimal","low","medium","high","xhigh"};
    const char *anthropic[]={"none","low","medium","high","xhigh","max"};
    const char **levels=native?anthropic:openai;
    menu_text(tr("\n推理模式"));
    std::vector<std::string> options;
    for(int i=0;i<6;i++)options.push_back(std::string(reasoning_name(levels[i]))+" ("+levels[i]+")");
    options.emplace_back(tr("恢复服务商默认（不传推理参数）"));
    options.emplace_back(tr("返回"));
    menu_text(tr("仅发送接口原生参数；支持档位由模型决定，不自动更换模型、提高输出上限或降档。"));
    int choice=arrow_menu(jobs,options);if(choice<=0)return;
    JsonPtr candidate(copy(a->config));
    if(choice==7)json_object_object_del(candidate.get(),"reasoning_effort");
    else put(candidate.get(),"reasoning_effort",str(levels[choice-1]));
    if(!a->conversation_id.empty()){
        use_reasoning(a->config,get(candidate.get(),"reasoning_effort"));
    }else if(save_config(a,candidate.get(),false)){
        fprintf(stderr,"%s\n",a->error);
        return;
    }
    char message[256];
    snprintf(message,sizeof message,tr("已保存推理模式：%s；保留当前对话。\n"),reasoning_name(sget(a->config,"reasoning_effort")));
    set_notice(message);
}
static void mcp_menu(App *a,Tasks &jobs);
static void settings_menu(App *a,Tasks &jobs,bool from_chat=false){
    for(;;){
        clear_page();show_notice();
        const char *options=from_chat
            ?tr("\n更多设置\n  [1] 界面语言\n  [2] 桌面工具\n  [3] MCP 服务\n  [4] 编辑当前模型配置\n  [5] 连接测试（少量 API 用量）\n  [6] 切换模型 ID\n  [7] 查看状态\n  [0] 返回")
            :tr("\n设置\n  [1] 界面语言\n  [2] 桌面工具\n  [3] 批准模式\n  [4] 编辑当前模型配置\n  [5] 连接测试（少量 API 用量）\n  [0] 返回");
        int choice=menu_choices(jobs,options);
        if(choice<=0)return;
        a->error[0]=0;
        if(choice==1)language_menu(a,jobs);
        else if(choice==2){
            clear_page();
            int selected=menu_choices(jobs,tr("\n桌面工具\n  [1] 开启\n  [2] 关闭\n  [0] 返回"));
            if(selected==1){a->desktop=true;set_notice(tr("桌面工具已开启，目标应用文字会发送至 API。"));}
            else if(selected==2){a->desktop=false;desktop_clear(a);set_notice(tr("桌面工具已关闭。"));}
        }else if(choice==3){
            if(from_chat)mcp_menu(a,jobs);
            else approval_menu(a,jobs);
        }else if(choice==4){
            std::string name=sget(a->config,"profile");
            if(configure(a,jobs,name.c_str(),nullptr,false))fprintf(stderr,"%s\n",a->error);
        }else if(choice==5){
            menu_text(tr("发送最小工具调用测试（少量 API 用量），不发送当前会话。"));
            if(api_check(a,a->config))fprintf(stderr,"%s\n",a->error);
            else set_notice(tr("连接成功，工具调用已验证。测试未操作电脑。"));
        }else if(choice==6){
            model_id_menu(a,jobs);
        }else if(choice==7){
            clear_page();
            status(a);
            arrow_menu(jobs,{tr("返回")});
        }
        if(a->error[0]){set_notice(a->error);a->error[0]=0;}
        if(interrupted==SIGTERM||input_closed)return;
    }
}
struct Conversation {
    std::string title;
    JsonPtr messages,config;
    int job=0;
    bool reset_context=false;
    bool sync_messages=false;
    std::string draft{};
    int draft_cursor=0;
    // Absolute byte position in the worker output already shown, and where
    // the latest turn began; both survive truncation of the output buffer.
    std::uint64_t displayed=0,turn_offset=0;
    struct Request {std::string text;JsonPtr settings;};
    std::vector<Request> queue{};
    std::string id{};
    bool yes=false,auto_edit=false,plan=false;
    // Requests sent in this conversation, oldest first, for recall in the
    // composer. Loaded from the saved transcript the first time it opens.
    std::vector<std::string> prompts{};
    bool prompts_loaded=false;
};
static bool persist_conversation(App &app,Conversation &item){
    if(item.id.empty()){char id[33];id_string(id);item.id=id;}
    const char *approval=item.yes?"full":item.auto_edit?"edit":"ask";
    if(history_save(app,item.id,item.title,item.config.get(),false,approval))return true;
    set_notice(tr("无法保存对话记录，请检查本机配置目录。"));
    return false;
}
// User messages carry the accent marker.
static void show_user_message(std::string_view body){
    // Build one string and hand it to terminal_message: that routes it into the
    // transcript region instead of drawing over the pinned input row.
    std::string text=ui_paint(Tone::Accent,"\n● ");
    size_t start=0;
    for(int line=0;;line++){
        auto end=body.find('\n',start);
        auto part=body.substr(start,end==std::string_view::npos?std::string_view::npos:end-start);
        if(line)text+="  ";
        text+=ui_paint(Tone::Plain,part);
        text+='\n';
        if(end==std::string_view::npos)break;
        start=end+1;
    }
    terminal_message(text);
}
static size_t show_conversation_history(App &app,const Conversation &item){
    JsonPtr record(history_read(app,item.id));
    J *messages=get(record.get(),"transcript");
    size_t shown=0;
    for(size_t i=0;i<alen(messages);i++){
        J *message=at(messages,i);
        const char *role=sget(message,"role");
        if(!strcmp(role,"user")){
            auto user_text=user_message_text(message);
            if(!user_text.empty()){show_user_message(user_text);shown++;}
        }
        else if(!strcmp(role,"assistant")){
            J *content=get(message,"content");
            std::string text;
            if(json_object_is_type(content,json_type_string))text=json_object_get_string(content);
            else for(size_t j=0;j<alen(content);j++){
                J *block=at(content,j);
                if(strcmp(sget(block,"type"),"text")||!*sget(block,"text"))continue;
                if(!text.empty())text+='\n';
                text+=sget(block,"text");
            }
            if(!text.empty()){
                terminal_message(std::string("\n")+ui_assistant_marker(ui_color())+ui_markdown(text,2)+"\n");
                shown++;
            }
        }
    }
    return shown;
}
static std::string conversation_title(std::string_view source,bool automatic){
    if(!valid_utf8(source))return {};
    auto first=source.find_first_not_of(" \t\r\n");
    if(first==std::string_view::npos)return {};
    source.remove_prefix(first);
    if(automatic){
        for(std::string_view prefix:{"请帮我","能帮我","帮我","请你","Please ","please ","Can you ","Could you "})
            if(source.starts_with(prefix)&&source.size()>prefix.size()){source.remove_prefix(prefix.size());break;}
    }
    std::string title;bool gap=false;size_t count=0;
    while(!source.empty()){
        auto byte=static_cast<unsigned char>(source.front());
        if(automatic&&!title.empty()&&(byte=='\n'||byte=='\r'||byte=='?'||byte=='!'||source.starts_with("。")||source.starts_with("？")||source.starts_with("！")))break;
        if(byte<=32||byte==127){gap=!title.empty();source.remove_prefix(1);continue;}
        size_t width=byte<0x80?1:byte<0xe0?2:byte<0xf0?3:4;
        if(count+(gap?1:0)>=48){title+="…";break;}
        if(gap){title+=' ';count++;gap=false;}
        title.append(source.substr(0,width));source.remove_prefix(width);count++;
    }
    return title;
}
static const char *title_of(const Conversation &item){return item.title.empty()?tr("新对话"):item.title.c_str();}

static void sync_conversation_models(App &app,std::vector<Conversation> &items){
    for(auto &item:items){
        const bool changed=!same_model(item.config.get(),app.config);
        JsonPtr updated(copy(app.config));
        use_reasoning(updated.get(),get(item.config.get(),"reasoning_effort"));
        item.config=std::move(updated);
        if(!item.id.empty())persist_conversation(app,item);
        if(changed){
            if(item.messages)item.messages.reset(arr());
            item.reset_context=true;
            item.sync_messages=false;
        }
        for(auto &request:item.queue){
            J *config=copy(item.config.get());
            use_reasoning(config,get(get(request.settings.get(),"config"),"reasoning_effort"));
            put(request.settings.get(),"config",config);
        }
        if(!item.queue.empty()&&item.reset_context){
            // Only the first queued request starts a fresh model context.
            for(size_t i=0;i<item.queue.size();i++)put(item.queue[i].settings.get(),"reset",json_object_new_boolean(i==0));
            item.reset_context=false;
        }
    }
}

// Absolute end of a worker's output so far; a turn starting now begins here.
static std::uint64_t output_end(Tasks &jobs,int job){
    std::uint64_t dropped=0;
    const auto text=jobs.output(job,nullptr,&dropped);
    return dropped+text.size();
}
static void advance_conversations(Tasks &jobs,std::vector<Conversation> &items){
    if(std::none_of(items.begin(),items.end(),[](const auto &item){return !item.queue.empty();}))return;
    JsonPtr states(jobs.snapshot());
    size_t active=0;
    for(size_t i=0;i<alen(states.get());i++)if(bget(at(states.get(),i),"active"))active++;
    for(auto &item:items){
        if(item.queue.empty())continue;
        J *state=nullptr;
        for(size_t i=0;i<alen(states.get());i++)if(iget(at(states.get(),i),"id",0)==item.job)state=at(states.get(),i);
        if(state&&(!strcmp(sget(state,"state"),tr("已取消"))||!strcmp(sget(state,"state"),tr("正在停止")))){
            item.queue.clear();continue;
        }
        if(!state||bget(state,"active")||active>=4)continue;
        auto &next=item.queue.front();
        const auto offset=output_end(jobs,item.job);
        if(jobs.resume(item.job,next.text,next.settings.get())){
            item.turn_offset=offset;
            item.queue.erase(item.queue.begin());active++;
        }else{
            item.queue.clear();
            terminal_message(std::string(title_of(item))+" · "+tr("无法继续此对话，请新建对话。")+"\n");
        }
    }
}
class ConversationScreen {
    App &app;
    Tasks &jobs;
    std::vector<Conversation> &items;
    std::function<void()> previous;
    bool pinned=false;
    int rows=0,columns=0;
    std::string last_header,last_composer,top_frame,frame;
    bool composer=false,overlay=false;
    int context_percent=-1;
    bool session_priced=false;
    double session_cost=0;
    double checked=0;
    std::map<int,std::string> last_notified;
    static std::string clipped(std::string_view text,int width){
        return ui_clip(text,width);
    }
public:
    Conversation *current=nullptr;
    bool controls_open=false;
    // Two bottom rows belong to the composer status line, so the scroll region
    // stops above them and the conversation never overwrites it.
    void layout(){
        if(!pinned)return;
        content_top=!current&&rows>=10&&columns>=40?6:1;
        // Keep the inline status line when there is no room for a composer.
        composer=current&&rows>=8&&columns>=20&&!env_value("DESK_NO_COMPOSER");
        // Four fixed rows at the bottom: the box's top rule, the input line, the
        // bottom rule, then the status. The transcript scrolls above them.
        const int bottom=composer?rows-4:rows;
        printf("\033[r\033[2J\033[%d;%dr\033[%d;1H",content_top,bottom,content_top);
        set_composer_layout(composer?rows-2:0,composer?rows-4:0,content_top,current!=nullptr);
        top_frame.clear();
        frame.clear();
        last_composer.clear();
    }
    void select(Conversation *item){
        terminal_activity("");
        reset_transcript();
        current=item;
        last_header.clear();
        checked=0;
        layout();
        fflush(stdout);
    }
    ConversationScreen(App &a,Tasks &tasks,std::vector<Conversation> &list):app(a),jobs(tasks),items(list),previous(std::move(tasks.on_idle)){
        const int height=terminal_rows(),width=terminal_columns();
        pinned=interface_active&&terminal_supports_ansi()&&height>=10&&width>=40;
        if(pinned){rows=height;columns=width;content_top=6;printf("\033[2J\033[6;%dr\033[6;1H",rows);}
        composer=false;
        jobs.on_idle=[this]{if(previous)previous();tick();};
        tick();
    }
    ~ConversationScreen(){
        jobs.on_idle=std::move(previous);
        reset_transcript();
        if(pinned)fputs("\033[r",stdout);
        content_top=1;clear_page();
        fflush(stdout);
    }
    void tick(){
        const int cur_rows=terminal_rows(),cur_cols=terminal_columns();
        const bool resized=pinned&&cur_rows>0&&cur_cols>0&&(rows!=cur_rows||columns!=cur_cols);
        if(!resized&&monotime()-checked<0.05){
            if(has_composer()&&!controls_open)draw_composer();
            return;
        }
        checked=monotime();
        if(resized){
            rows=cur_rows;columns=cur_cols;
            if(!overlay)layout();
            last_header.clear();
        }
        const bool live_header=!current&&pinned&&rows>=10&&columns>=40;
        J *current_state=nullptr;
        JsonPtr states(jobs.snapshot());
        struct Activity {const Conversation *item;J *state;size_t number;int priority;};
        std::vector<Activity> activity;
        size_t queued=0,approvals=0;
        for(size_t index=0;index<items.size();index++){
            auto &item=items[index];queued+=item.queue.size();
            J *state=nullptr;
            for(size_t i=0;i<alen(states.get());i++)if(iget(at(states.get(),i),"id",0)==item.job)state=at(states.get(),i);
            if(state){
                bool approval=bget(state,"awaiting_approval");
                if(approval)approvals++;
                int priority=approval?0:bget(state,"active")?1:!item.queue.empty()?2:3;
                activity.push_back({&item,state,index+1,priority});
                if(item.job>0){
                    std::string tag=approval?"approval":(bget(state,"active")?"active":sget(state,"state"));
                    auto prev=last_notified.find(item.job);
                    if(prev==last_notified.end())last_notified[item.job]=tag;
                    else if(prev->second!=tag){
                        std::string old_tag=prev->second;
                        prev->second=tag;
                        if(&item!=current&&!old_tag.empty()){
                            if(tag=="approval")notify_user_event(app,tr("DeskCode 等待批准"),title_of(item));
                            else if(tag!="active")notify_user_event(app,!strcmp(sget(state,"state"),tr("已完成"))?tr("DeskCode 任务已完成"):tr("DeskCode 任务未完成"),title_of(item));
                        }
                    }
                }
            }
            if(&item==current)current_state=state;
            if(&item==current&&item.job&&!controls_open){
                std::uint64_t dropped=0;
                auto output=jobs.output(item.job,nullptr,&dropped);
                const std::uint64_t produced=dropped+output.size();
                if(item.displayed>produced)item.displayed=produced;
                if(item.displayed<dropped){
                    terminal_message(ui_paint(Tone::Dim,std::string("  ")+tr("…较早的输出已省略"))+"\n");
                    item.displayed=dropped;
                }
                const size_t start=static_cast<size_t>(item.displayed-dropped);
                auto end=output.rfind('\n');
                size_t length=end==std::string::npos?0:end+1;
                const bool settled=!state||!bget(state,"active")||bget(state,"awaiting_approval");
                if(settled||!strcmp(sget(state,"state"),tr("等待确认")))length=output.size();
                if(length>start){
                    size_t used=0;
                    auto text=display_utf8(std::string_view(output).substr(start,length-start),used,settled);
                    if(!text.empty())terminal_message(text);
                    item.displayed+=used;
                }
                if(!has_composer()&&state&&bget(state,"thinking")){
                    terminal_activity(std::string(tr("正在思考…"))+" "+elapsed_label(json_object_get_double(get(state,"thinking_seconds"))));
                }else if(!has_composer()&&state&&!bget(state,"active")){
                    terminal_activity(std::string(sget(state,"state"))+" · "+title_of(item));
                }else if(!has_composer())terminal_activity("");
            }
        }
        if(has_composer()&&!controls_open)draw_chat_activity(current_state);
        context_percent=current_state?iget(current_state,"context_percent",-1):-1;
        session_priced=current_state&&bget(current_state,"session_priced");
        session_cost=current_state?json_object_get_double(get(current_state,"session_cost")):0;
        draw_composer();
        if(current)return;
        // Approval requests remain visible even when several other chats run.
        std::stable_sort(activity.begin(),activity.end(),[](const auto &left,const auto &right){
            if(left.priority!=right.priority)return left.priority<right.priority;
            if(left.priority==3)return json_object_get_double(get(left.state,"changed_at"))>json_object_get_double(get(right.state,"changed_at"));
            return false;
        });
        struct Row {std::string text;bool approval=false;};
        std::vector<Row> rows_text{{tr("对话运行状态"),approvals>0}};
        if(approvals){char label[128];snprintf(label,sizeof label,tr(" · 待审批：%zu（进入对应对话处理）"),approvals);rows_text[0].text+=label;}
        if(queued){char label[80];snprintf(label,sizeof label,tr(" · 排队：%zu"),queued);rows_text[0].text+=label;}
        for(const auto &entry:activity){
            if(rows_text.size()==5)break;
            const auto &item=*entry.item;J *state=entry.state;
            bool approval=bget(state,"awaiting_approval"),thinking=bget(state,"thinking");
            const char *status=approval?tr("等待审批"):entry.priority==2?tr("排队中"):sget(state,"state");
            std::string prefix="["+std::to_string(entry.number)+"] "+status+" · ";
            auto title=live_header&&thinking?clipped(title_of(item),std::max(4,columns-48)):std::string(title_of(item));
            std::string text=prefix+title;
            if(thinking){
                text+=" · "+std::string(tr("正在思考…"));
                if(live_header)text+=" "+elapsed_label(json_object_get_double(get(state,"thinking_seconds")));
            }
            rows_text.push_back({std::move(text),approval});
        }
        std::string header;
        for(const auto &row:rows_text){if(!header.empty())header+='\n';header+=row.text;}
        if(header==last_header)return;
        last_header=header;
        if(live_header){
            fputs("\0337",stdout);
            for(int row=1;row<=5;row++){
                size_t index=static_cast<size_t>(row-1);
                std::string_view text=index<rows_text.size()?rows_text[index].text:std::string_view{};
                bool approval=index<rows_text.size()&&rows_text[index].approval;
                bool color=terminal_supports_color()&&(row==1||approval);
                auto body=clipped(text,columns);
                auto painted=color?ui_paint(approval?Tone::Warning:Tone::Accent,body,true):body;
                printf("\033[%d;1H\033[2K%s",row,painted.c_str());
            }
            fputs("\0338",stdout);fflush(stdout);
        }else if(rows_text.size()>1)terminal_message("\n"+header+"\n");
    }
    void draw_chat_activity(J *state){
        if(!bget(state,"active")||bget(state,"awaiting_approval")){
            terminal_activity("");
            return;
        }
        const bool thinking=bget(state,"thinking");
        std::string text=std::string(ui_spinner(static_cast<size_t>(monotime()*9)))+" ";
        text+=tr(thinking?"正在思考…":"正在执行…");
        text+=" "+elapsed_label(json_object_get_double(get(state,thinking?"thinking_seconds":"turn_seconds")));
        auto tokens=json_object_get_uint64(get(state,"tokens"));
        if(tokens){
            char used[64];
            snprintf(used,sizeof used,tr(" · %s 词元"),token_label(tokens).c_str());
            text+=used;
        }
        const char *detail=sget(state,"detail");
        if(*detail&&!thinking){
            const int room=columns-ui_width(text)-6;
            if(room>=12){
                text+=" · ";
                text+=ui_clip(detail,room);
            }
        }
        terminal_activity(text);
    }
    // Bottom frame: a rule above the input line, then the input row, then the
    // modes line. The input row itself is redrawn by readline, so this only
    // paints the rule and status when they change.
    void draw_composer(){
        if(!pinned||!composer||overlay)return;
        char status[1024];
        const char *effort=sget(app.config,"reasoning_effort");
        std::string_view permission=permission_name(app);
        snprintf(status,sizeof status,tr("思考强度：%s · 批准：%s · %s"),
                 *effort?reasoning_name(effort):tr("默认"),permission.data(),sget(app.config,"model"));
        // Context fill sits at the end, kept whole: a long model name is
        // clipped first so the number never disappears.
        std::string context;
        if(context_percent>=0&&current&&!current->reset_context){
            char label[64];
            snprintf(label,sizeof label,tr(" · 上下文约 %d%%（估算）"),context_percent);
            context=label;
        }
        if(session_priced&&current)context+=" · "+usage_cost_label(app.config,session_cost);
        const int context_cells=ui_width(context);
        if(context_cells>columns-28)context.clear();
        // Plan mode leads the line so it is never clipped away.
        const std::string plan=app.plan_mode?std::string(tr("计划模式"))+" · ":"";
        const int plan_cells=ui_width(plan);
        auto text=ui_clip(status,columns-4-plan_cells-(context.empty()?0:context_cells));
        const std::string composer_key=plan+text+context;
        const int rule_width=std::min(columns-4,100);
        std::string rule="  ";
        for(int i=0;i<rule_width;i++)rule+="─";
        std::string top_rule;
        const size_t hidden=transcript_hidden_lines();
        if(hidden&&rule_width>=24){
            char badge_buf[96];
            snprintf(badge_buf,sizeof badge_buf,tr(" ↑ 还有 %zu 行 · PgDn "),hidden);
            std::string badge=ui_clip(badge_buf,rule_width-4);
            const int badge_cells=ui_width(badge);
            const int left_dashes=std::max(2,rule_width-badge_cells-2);
            const int right_dashes=std::max(0,rule_width-left_dashes-badge_cells);
            std::string left="  ",right;
            for(int i=0;i<left_dashes;i++)left+="─";
            for(int i=0;i<right_dashes;i++)right+="─";
            top_rule=ui_paint(Tone::Rule,left)+ui_paint(Tone::Accent,badge)+ui_paint(Tone::Rule,right);
        }else{
            top_rule=ui_paint(Tone::Rule,rule);
        }
        if(composer_key==last_composer&&top_rule==top_frame&&rule==frame)return;
        last_composer=composer_key;top_frame=top_rule;frame=rule;
        fputs("\0337",stdout);
        printf("\033[%d;1H\033[2K",rows-3);
        fputs(top_rule.c_str(),stdout);
        printf("\033[%d;1H\033[2K",rows-1);
        fputs(ui_paint(Tone::Rule,rule).c_str(),stdout);
        printf("\033[%d;1H\033[2K",rows);
        std::string_view footer(text);
        const bool contrast=env_value("DESK_CONTRAST_NUMBERS");
        const Tone footer_tone=contrast?Tone::Plain:Tone::Dim;
        auto highlight=app.yes?footer.find(permission):footer.npos;
        fputs("  ",stdout);
        if(!plan.empty())fputs(ui_paint(Tone::Warning,plan,true).c_str(),stdout);
        if(highlight!=footer.npos){
            fputs(ui_paint(footer_tone,footer.substr(0,highlight),contrast).c_str(),stdout);
            fputs(ui_paint(Tone::Orange,permission).c_str(),stdout);
            footer.remove_prefix(highlight+permission.size());
        }
        fputs(ui_paint(footer_tone,footer,contrast).c_str(),stdout);
        if(!context.empty())
            fputs(ui_paint(context_percent>=80?Tone::Warning:footer_tone,context,contrast).c_str(),stdout);
        fputs("\0338",stdout);
        fflush(stdout);
    }
    bool has_composer() const {return pinned&&composer&&!overlay;}
    // Menus opened from a chat need the whole screen: drop the pinned frame and
    // its scroll region while they are up, then rebuild both afterwards.
    void begin_overlay(){
        if(!pinned||overlay)return;
        overlay=true;
        set_composer_layout(0,0);
        printf("\033[r\033[2J\033[1;1H");
        content_top=1;
        fflush(stdout);
    }
    void end_overlay(){
        if(!overlay)return;
        overlay=false;
        layout();
        refresh_composer();
        fflush(stdout);
    }
    // Approval prompts and menu redraws can overwrite the frame rows; drop the
    // cached copy so the next tick paints them again.
    void refresh_composer(){last_composer.clear();top_frame.clear();frame.clear();checked=0;tick();}
    // Show everything the worker has printed so far, ignoring the tick throttle.
    void flush(){jobs.refresh();checked=0;tick();}
    bool awaiting_approval(const Conversation &item){
        if(!item.job)return false;
        JsonPtr states(jobs.snapshot());
        for(size_t i=0;i<alen(states.get());i++){
            J *state=at(states.get(),i);
            if(iget(state,"id",0)==item.job)return bget(state,"awaiting_approval");
        }
        return false;
    }
    bool shell_approval(const Conversation &item){
        if(!item.job)return false;
        JsonPtr states(jobs.snapshot());
        for(size_t i=0;i<alen(states.get());i++){
            J *state=at(states.get(),i);
            if(iget(state,"id",0)==item.job)return bget(state,"shell_approval");
        }
        return false;
    }
    void set_context_percent(int p){
        context_percent=p;
        refresh_composer();
    }
    bool active(const Conversation &item){
        JsonPtr states(jobs.snapshot());
        for(size_t i=0;i<alen(states.get());i++)if(iget(at(states.get(),i),"id",0)==item.job)return bget(at(states.get(),i),"active");
        return false;
    }
    // With the pinned composer a plain print would land on the frame; route
    // short notices into the transcript instead.
    void note(const char *text){
        if(has_composer())terminal_message(ui_paint(Tone::Dim,std::string("  ")+text)+"\n");
        else menu_text(text);
    }
    void report_error(){
        if(!app.error[0])return;
        if(has_composer())terminal_message(ui_paint(Tone::Error,std::string("  ")+app.error)+"\n");
        else fprintf(stderr,"%s\n",app.error);
    }
    // Where to resume showing worker output when a chat is reopened: history
    // already holds finished turns; a running or failed turn replays its own
    // output so progress, approvals and errors stay visible.
    void reopen(Conversation &item){
        if(!item.job){item.displayed=0;return;}
        JsonPtr states(jobs.snapshot());
        J *state=nullptr;
        for(size_t i=0;i<alen(states.get());i++)if(iget(at(states.get(),i),"id",0)==item.job)state=at(states.get(),i);
        const bool finished=state&&!bget(state,"active")&&!strcmp(sget(state,"state"),tr("已完成"));
        item.displayed=finished||!state?output_end(jobs,item.job):item.turn_offset;
    }
    bool submit(Conversation &item,const std::string &task,std::string_view display_override={}){
        app.error[0]=0;
        scroll_transcript_bottom();
        // Echo the request into the transcript before starting the job; the
        // assistant reply streams in underneath it.
        show_user_message(display_override.empty()?std::string_view(task):display_override);
        JsonPtr settings(conversation_settings(app,item.reset_context,item.sync_messages?app.messages:nullptr));
        if(!item.job){
            persist_conversation(app,item);
            app.conversation_worker=true;
            app.guided_approval=terminal_supports_ansi()&&!env_value("DESK_NO_ARROW");
            item.job=jobs.start(app,task);
            app.guided_approval=false;
            app.conversation_worker=false;
            if(item.job<0){item.job=0;return false;}
            item.displayed=item.turn_offset=0;
        }else if(active(item)||!item.queue.empty()){
            if(item.queue.size()>=4){note(tr("当前对话的待执行需求已满。"));return false;}
            item.queue.push_back({task,std::move(settings)});
            note(tr("已排队，当前操作结束后继续。"));
        }else{
            const auto offset=output_end(jobs,item.job);
            if(!jobs.resume(item.job,task,settings.get())){note(tr("无法继续此对话，请新建对话。"));return false;}
            item.turn_offset=offset;
        }
        item.reset_context=false;
        item.sync_messages=false;
        checked=0;tick();return true;
    }
};
static std::string mcp_paste(Tasks &jobs){
    menu_text(tr("粘贴完整 JSON 后按回车，支持多行。输入空行取消。"));
    std::string text;
    for(;;){
        auto line=request_text(jobs,text.empty()?"JSON › ":"… ");
        if(line.empty()||input_closed||interrupted)return {};
        if(text.size()+line.size()+1>256000){
            menu_text(tr("MCP 配置不能超过 256 KB。"));
            return {};
        }
        text+=line;
        text+='\n';
        json_tokener *parser=json_tokener_new();
        json_tokener_set_flags(parser,JSON_TOKENER_STRICT);
        JsonPtr parsed(json_tokener_parse_ex(parser,text.data(),static_cast<int>(text.size())));
        auto error=json_tokener_get_error(parser);
        json_tokener_free(parser);
        if(error!=json_tokener_continue)return text;
    }
}
static void mcp_template(App *app,Tasks &jobs){
    const char *example="{\n  \"mcpServers\": {\n    \"my-server\": {\n      \"url\": \"https://example.com/mcp\"\n    }\n  }\n}\n";
    puts(example);
    menu_text(tr("替换服务名称和地址即可使用。留空返回，输入路径可保存示例文件。"));
    auto path=request_text(jobs,tr("保存路径："));
    if(path.empty())return;
    auto target=fs_path(path);
    if(target.is_relative())target=fs_path(app->root)/target;
    std::error_code error;
    bool exists=fs::exists(target,error);
    if(error){set_notice(tr("无法检查保存路径。"));return;}
    if(exists&&menu_choices(jobs,tr("文件已存在，是否替换？\n  [1] 替换\n  [0] 取消"),nullptr,0)!=1)return;
    if(atomic_file(path_text(target).c_str(),example,strlen(example),0600)){
        set_notice(tr("示例保存失败，请检查目录是否存在及是否可写。"));
    }else set_notice(tr("示例已保存，修改后可从 MCP 菜单导入。"));
}
static void mcp_menu(App *a,Tasks &jobs){
    std::string notice;
    for(;;){
        clear_page();menu_text(tr("\nMCP 服务"));show_notice();
        if(!notice.empty()){puts(notice.c_str());notice.clear();}
        JsonPtr servers(mcp_load(*a));
        if(!servers){fprintf(stderr,"%s\n",a->error);return;}
        std::vector<std::string> names,options;
        json_object_object_foreach(servers.get(),name,entry){
            names.emplace_back(name);
            options.push_back(std::string(name)+" · "+(*sget(entry,"url")?"HTTP":"stdio")+" · "+tr(bget(entry,"disabled")?"已停用":"已启用"));
        }
        int count=static_cast<int>(names.size());
        options.emplace_back(tr("导入 JSON 配置文件"));
        options.emplace_back(tr("添加远程 MCP 地址"));
        options.emplace_back(tr("粘贴 JSON 配置"));
        options.emplace_back(tr("查看或保存 JSON 示例"));
        options.emplace_back(tr("返回"));
        int choice=arrow_menu(jobs,options);if(choice<=0)return;
        a->error[0]=0;
        if(choice==count+1){
            std::string path=request_text(jobs,tr("配置文件路径（空行取消）："));if(path.empty())continue;
            if(path.size()>=2&&((path.front()=='"'&&path.back()=='"')||(path.front()=='\''&&path.back()=='\'')))path=path.substr(1,path.size()-2);
            if(path.starts_with("~/")){const char *home=env_value("HOME");if(home)path=std::string(home)+path.substr(1);}
            auto file=fs_path(path);if(file.is_relative())file=fs_path(a->root)/file;
            notice=mcp_import(*a,path_text(file).c_str())?tr("MCP 配置已导入，下一条需求生效。"):a->error;
        }else if(choice==count+2){
            std::string name=request_text(jobs,tr("服务名称（空行取消）："));if(name.empty())continue;
            if(get(servers.get(),name.c_str())){notice=tr("服务名称已存在。");continue;}
            std::string url=request_text(jobs,tr("MCP 地址（空行取消）："));if(url.empty())continue;
            std::unique_ptr<char,decltype(&free)> token(input(tr("Bearer Token（可留空，输入隐藏）："),nullptr,true),free);
            if(!token)continue;
            JsonPtr entry(obj());put(entry.get(),"url",str(url.c_str()));
            if(*token){J *headers=obj();put(headers,"Authorization",str((std::string("Bearer ")+token.get()).c_str()));put(entry.get(),"headers",headers);}
            put(servers.get(),name.c_str(),entry.release());
            notice=mcp_save(*a,servers.get())?tr("MCP 配置已导入，下一条需求生效。"):a->error;
        }else if(choice==count+3){
            auto text=mcp_paste(jobs);
            if(!text.empty())notice=mcp_import_json(*a,text)?tr("MCP 配置已导入，下一条需求生效。"):a->error;
        }else if(choice==count+4){
            mcp_template(a,jobs);
        }else{
            const std::string name=names[static_cast<size_t>(choice-1)];J *entry=get(servers.get(),name.c_str());
            clear_page();printf("\n  MCP · %s\n",name.c_str());
            int action=menu_choices(jobs,tr("  [1] 测试连接并列出工具\n  [2] 切换启用/停用\n  [3] 删除服务\n  [0] 返回"),
                                    tr("连接测试会启动配置中的本地程序，或连接远程服务。"));
            if(action==1){
                if(mcp_prepare(*a,true,name.c_str())){
                    JsonPtr tools(mcp_tools(*a));printf(tr("连接成功，共 %zu 个工具。\n"),alen(tools.get()));
                    for(size_t i=0;i<alen(tools.get());i++)printf("  %.*s\n",180,sget(get(at(tools.get(),i),"function"),"description"));
                }else fprintf(stderr,"%s\n",a->error);
                mcp_close(*a);arrow_menu(jobs,{tr("返回")});
            }else if(action==2){
                put(entry,"disabled",json_object_new_boolean(!bget(entry,"disabled")));
                notice=mcp_save(*a,servers.get())?tr("MCP 设置已保存，下一条需求生效。"):a->error;
            }else if(action==3){
                if(menu_choices(jobs,tr("删除此 MCP 配置？\n  [1] 确认删除\n  [0] 取消"),nullptr,0)==1){json_object_object_del(servers.get(),name.c_str());notice=mcp_save(*a,servers.get())?tr("MCP 配置已删除。"):a->error;}
            }
        }
        if(interrupted==SIGTERM||input_closed)return;
    }
}
// The "/" command palette. The first six entries keep their numbers so muscle
// memory (and scripts) survive; local actions follow. Returns 0 back, 1 done,
// 2 conversation list, 3 new chat, 4 clear context, 5 status, 6 help.
static int chat_menu(App *a,Tasks &jobs){
    clear_page();
    const auto heading=ui_clip(tr("对话操作"),std::max(1,terminal_columns()-4));
    printf("  %s\n",ui_paint(Tone::Plain,heading,true).c_str());
    std::vector<std::string> items;
    for(const auto *label:{"选择模型","推理模式","批准模式","新建对话","返回选择对话","更多设置","清空上下文","查看状态","快捷键帮助","返回输入"})
        items.emplace_back(tr(label));
    auto selected=terminal_choice(items,1,content_top+1,[&]{
        jobs.refresh();
        if(jobs.on_idle)jobs.on_idle();
    },true,false,true);
    if(!selected){
        for(size_t i=0;i<items.size();i++){
            auto label=ui_number("["+std::to_string((i+1)%items.size())+"]");
            printf("  %s %s\n",label.c_str(),items[i].c_str());
        }
        selected=number(jobs,static_cast<int>(items.size()-1),0);
    }
    int choice=*selected;
    if(choice<0)return -1;
    if(choice==0)return 0;
    if(choice==1){if(select_model(a,jobs))fprintf(stderr,"%s\n",a->error);}
    else if(choice==2)reasoning_menu(a,jobs);
    else if(choice==3)approval_menu(a,jobs);
    else if(choice==4)return 3;
    else if(choice==5)return 2;
    else if(choice==6)settings_menu(a,jobs,true);
    else if(choice>=7&&choice<=9)return choice-3;
    return interrupted==SIGTERM?-1:1;
}
// The chat title goes through the transcript writer so that, with the pinned
// input box, it takes the first transcript row instead of being overwritten.
static void chat_title(const App &app,const Conversation &item){
    char line[1024];
    snprintf(line,sizeof line,tr("对话：%s"),title_of(item));
    const char *home=env_value("HOME");
    std::string folder;
    if(home&&!strcmp(app.root,home))folder="~";
    else folder=path_text(fs_path(app.root).filename());
    if(folder.empty())folder=app.root;
    std::string text="\n";
    if(interface_active){
        text+="  "+ui_paint(Tone::Accent,line,true);
        text+=ui_paint(Tone::Dim," · "+folder);
    }else{
        text+=line;
    }
    text+="\n";
    terminal_message(text);
}
static void show_inline_status(const App &app){
    JsonPtr usage(load_tokens(app));
    std::string body=std::string(tr("模型"))+": "+sget(app.config,"model")+" ("+sget(app.config,"protocol")+")\n"
        +tr("目录")+": "+app.root+"\n"
        +tr("批准")+": "+permission_name(app)+" · "+tr("推理")+": "+reasoning_name(sget(app.config,"reasoning_effort"));
    if(sandbox_supported()){
        body+="\n"+std::string(tr("沙箱"))+": "+(app.sandbox?(std::string(sandbox_backend())+" "+tr("已开启（限制工作目录外写入）")):tr("已关闭"));
        if(app.sandbox_no_net&&sandbox_net_supported())body+=" · "+std::string(tr("禁网隔离已开启"));
    }
    if(usage)body+="\n"+std::string(tr("词元"))+": "+token_label(model_tokens(usage.get(),app.config));
    const int width=terminal_columns()>0?std::min(std::max(terminal_columns()-6,36),88):72;
    std::string box=ui_box(tr("DeskCode 状态"),body,width);
    std::string out="\n";
    for(size_t start=0;start<box.size();){
        auto end=box.find('\n',start);
        out+="  "+box.substr(start,end==std::string::npos?std::string::npos:end-start)+"\n";
        if(end==std::string::npos)break;
        start=end+1;
    }
    terminal_message(out);
}
static void show_inline_help(){
    const int width=terminal_columns()>0?std::min(std::max(terminal_columns()-6,36),88):76;
    std::string box=ui_box(tr("对话快捷键与指令"),tr("  / 命令面板 · F2 模型 · F3 思考强度 · /undo 撤回 · /compact 压缩 · /sandbox [net] 沙箱/禁网 · /notify 桌面通知 · /commands 指令 · /skills 技能 · /plugins 插件 · /agents 子代理\n  Shift+Tab 或 /plan 计划模式 · /go 批准计划 · /rewind 回退 · /cost 用量 · /price 价格 · Ctrl+B 命令转后台\n  @词+Tab 补全工作区文件 · @文件[:行段] 附带内容 · /paste 贴图 · Alt+Enter 换行\n  鼠标滚轮 / ↑↓ 滚动对话 · PgUp/PgDn 翻页 · Ctrl+Home/End 首尾\n  Ctrl+P/Ctrl+N 或 Alt+↑/↓ 调出之前发送的需求 · Shift+拖动 选择文字\n  Ctrl+C 停止当前任务 · Ctrl+D 返回对话列表（任务在后台继续）"),width);
    std::string out="\n";
    for(size_t start=0;start<box.size();){
        auto end=box.find('\n',start);
        out+="  "+box.substr(start,end==std::string::npos?std::string::npos:end-start)+"\n";
        if(end==std::string::npos)break;
        start=end+1;
    }
    terminal_message(out);
}
static void show_custom_commands(const App &app){
    auto cmds=list_custom_commands(app);
    std::string body;
    if(cmds.empty()){
        body=tr("暂无自定义指令。\n可在 .deskcode/commands/<名称>.md 或 .desk-agent/commands/<名称>.md 中编写提示词模板（支持 $ARGUMENTS），在对话中输入 /<名称> 调用。");
    }else{
        for(size_t i=0;i<cmds.size();i++){
            if(i)body+='\n';
            body+="/"+cmds[i].name+" ("+cmds[i].scope+")";
            if(!cmds[i].summary.empty())body+=" — "+cmds[i].summary;
        }
    }
    const int width=terminal_columns()>0?std::min(std::max(terminal_columns()-6,36),88):76;
    std::string box=ui_box(tr("自定义斜杠指令"),body,width);
    std::string out="\n";
    for(size_t start=0;start<box.size();){
        auto end=box.find('\n',start);
        out+="  "+box.substr(start,end==std::string::npos?std::string::npos:end-start)+"\n";
        if(end==std::string::npos)break;
        start=end+1;
    }
    terminal_message(out);
}
static void inline_box(const char *title,const std::string &body){
    const int width=terminal_columns()>0?std::min(std::max(terminal_columns()-6,36),88):76;
    std::string box=ui_box(title,body,width);
    std::string out="\n";
    for(size_t start=0;start<box.size();){
        auto end=box.find('\n',start);
        out+="  "+box.substr(start,end==std::string::npos?std::string::npos:end-start)+"\n";
        if(end==std::string::npos)break;
        start=end+1;
    }
    terminal_message(out);
}
static std::string price_number(J *config,const char *key){
    char text[64];
    snprintf(text,sizeof text,"%g",json_object_get_double(get(config,key)));
    return text;
}
// /cost: what this conversation used, split the way providers bill it.
static void show_cost(const App &app,J *state){
    TokenUsage session=app.session_usage;
    if(state){
        session.input=json_object_get_uint64(get(state,"session_input"));
        session.output=json_object_get_uint64(get(state,"session_output"));
        session.cache_read=json_object_get_uint64(get(state,"session_cache_read"));
        session.cost=json_object_get_double(get(state,"session_cost"));
        session.priced=bget(state,"session_priced");
    }
    char line[512];
    snprintf(line,sizeof line,tr("本对话：输入 %s · 缓存命中 %s · 输出 %s 词元"),
             token_label(session.input).c_str(),token_label(session.cache_read).c_str(),token_label(session.output).c_str());
    std::string body=line;
    if(session.priced)body+="\n"+std::string(tr("估算费用："))+usage_cost_label(app.config,session.cost);
    if(usage_priced(app.config)){
        std::string currency=sget(app.config,"price_currency");
        snprintf(line,sizeof line,tr("当前价格（每百万词元）：输入 %s · 输出 %s · 缓存命中 %s %s"),
                 price_number(app.config,"price_input").c_str(),price_number(app.config,"price_output").c_str(),
                 get(app.config,"price_cache_read")?price_number(app.config,"price_cache_read").c_str():price_number(app.config,"price_input").c_str(),
                 currency.c_str());
        body+="\n";body+=line;
    }else body+="\n"+std::string(tr("未设置价格：/price 输入价 输出价 [缓存命中价] [币种]，单位为每百万词元。"));
    JsonPtr usage(load_tokens(app));
    if(usage)body+="\n"+std::string(tr("此模型累计："))+token_label(model_tokens(usage.get(),app.config))+" "+tr("词元");
    inline_box(tr("用量与费用"),body);
}
// /price IN OUT [CACHE] [CURRENCY], /price off, or /price to show it.
static std::string set_price(App *a,std::string_view args){
    std::vector<std::string> words;
    for(size_t i=0;i<args.size();){
        while(i<args.size()&&isspace(static_cast<unsigned char>(args[i])))i++;
        size_t start=i;
        while(i<args.size()&&!isspace(static_cast<unsigned char>(args[i])))i++;
        if(i>start)words.emplace_back(args.substr(start,i-start));
    }
    if(words.empty()){
        if(!usage_priced(a->config))return tr("未设置价格：/price 输入价 输出价 [缓存命中价] [币种]，单位为每百万词元。");
        char line[256];
        snprintf(line,sizeof line,tr("当前价格（每百万词元）：输入 %s · 输出 %s · 缓存命中 %s %s"),
                 price_number(a->config,"price_input").c_str(),price_number(a->config,"price_output").c_str(),
                 get(a->config,"price_cache_read")?price_number(a->config,"price_cache_read").c_str():price_number(a->config,"price_input").c_str(),
                 sget(a->config,"price_currency"));
        return line;
    }
    JsonPtr candidate(copy(a->config));
    for(const char *key:{"price_input","price_output","price_cache_read","price_currency"})json_object_object_del(candidate.get(),key);
    if(words.size()!=1||(words[0]!="off"&&words[0]!="clear")){
        std::vector<double> numbers;
        std::string currency;
        for(const auto &word:words){
            char *end=nullptr;
            double value=strtod(word.c_str(),&end);
            if(end!=word.c_str()&&!*end&&numbers.size()<3&&currency.empty()){
                if(!(value>=0)||value>1e6)return tr("价格须为 0 到 1000000 之间的数字。");
                numbers.push_back(value);
            }else if(currency.empty()&&word.size()<=8&&std::all_of(word.begin(),word.end(),[](char c){return isalpha(static_cast<unsigned char>(c))||c=='$'||static_cast<unsigned char>(c)>=0x80;}))currency=word;
            else return tr("用法：/price 输入价 输出价 [缓存命中价] [币种]，例如 /price 2 8 0.5 CNY；/price off 清除。");
        }
        if(numbers.size()<2)return tr("用法：/price 输入价 输出价 [缓存命中价] [币种]，例如 /price 2 8 0.5 CNY；/price off 清除。");
        if(currency.empty())currency=strcmp(language_code(),"zh")?"USD":"CNY";
        put(candidate.get(),"price_input",json_object_new_double(numbers[0]));
        put(candidate.get(),"price_output",json_object_new_double(numbers[1]));
        if(numbers.size()==3)put(candidate.get(),"price_cache_read",json_object_new_double(numbers[2]));
        put(candidate.get(),"price_currency",str(currency.c_str()));
    }
    // Prices belong to the saved profile; this conversation keeps its own
    // reasoning setting.
    JsonPtr profile(copy(get(get(a->store,"profiles"),sget(a->config,"profile"))));
    if(!profile)profile.reset(copy(candidate.get()));
    for(const char *key:{"price_input","price_output","price_cache_read","price_currency"}){
        json_object_object_del(profile.get(),key);
        if(J *value=get(candidate.get(),key))put(profile.get(),key,copy(value));
    }
    JsonPtr reasoning(copy(get(a->config,"reasoning_effort")));
    if(save_config(a,profile.get(),false))return a->error;
    use_reasoning(a->config,reasoning.get());
    return usage_priced(a->config)?tr("已保存此模型的价格，之后的用量会显示估算费用。"):tr("已清除此模型的价格。");
}
// First screen of an empty conversation: what to type and where the controls
// are. Shown again whenever an empty chat is opened, never once history exists.
static void show_welcome(){
    std::string text="\n";
    text+="  "+ui_paint(Tone::Plain,tr("直接描述你要完成的事，例如："),true)+"\n";
    for(const char *example:{"梳理这个项目的结构、入口和构建方式","运行测试，找出失败原因并修复","给 README 补一段安装与使用说明"}){
        text+="    "+ui_paint(Tone::Accent,"›")+" "+ui_paint(Tone::Dim,tr(example))+"\n";
    }
    text+="\n  "+ui_paint(Tone::Dim,tr("/ 命令面板 · Shift+Tab 计划模式 · @+Tab 选文件 · /rewind 回退 · /undo 撤回 · Ctrl+D 返回"))+"\n";
    terminal_message(text);
}
static bool chat(App *a,Tasks &jobs,Conversation *conversation=nullptr,ConversationScreen *screen=nullptr){
    if(screen)screen->select(conversation);
    if(!screen||!screen->has_composer())clear_page();
    if(conversation)chat_title(*a,*conversation);
    show_notice();
    // Orientation is one dim line, once per run, above any restored history.
    if(!conversation||conversation->messages==nullptr){
        static bool context_shown=false;
        if(!context_shown){
            context_shown=true;
            terminal_message(ui_paint(Tone::Dim,tr("  工作目录及任务会发送到配置的 API；输入需求开始，或 / 打开操作菜单"))+"\n");
        }
    }
    if(conversation&&!show_conversation_history(*a,*conversation))show_welcome();
    const bool guided=terminal_supports_ansi()&&!env_value("DESK_NO_ARROW");
    ChatDraft draft;
    std::vector<std::string> session_prompts;
    std::vector<std::string> *prompts=&session_prompts;
    if(conversation){
        if(!conversation->prompts_loaded){
            conversation->prompts_loaded=true;
            JsonPtr record(history_read(*a,conversation->id));
            J *messages=get(record.get(),"transcript");
            for(size_t i=0;i<alen(messages);i++){
                J *message=at(messages,i);
                if(strcmp(sget(message,"role"),"user"))continue;
                std::string text=user_message_text(message);
                if(!text.empty()&&(conversation->prompts.empty()||conversation->prompts.back()!=text))
                    conversation->prompts.push_back(std::move(text));
            }
        }
        prompts=&conversation->prompts;
    }
    draft.history=prompts;
    auto remember=[&](const std::string &text){
        if(!prompts->empty()&&prompts->back()==text)return;
        prompts->push_back(text);
        if(prompts->size()>200)prompts->erase(prompts->begin());
    };
    if(conversation){
        draft.text=std::move(conversation->draft);
        draft.cursor=conversation->draft_cursor;
    }
    if(guided&&screen&&conversation)draft.approval_pending=[&]{return screen->awaiting_approval(*conversation);};
    std::string last_model,last_modes;
    for(;;){
        if(input_closed||interrupted==SIGTERM)return false;
        if(interrupted==SIGINT)interrupted=0;
        // Without the pinned composer (pipes, narrow windows) the modes have
        // no home, so print them inline as before.
        if(!screen||!screen->has_composer())
        {
            std::string model=sget(a->config,"model");
            std::string modes=std::string(language_code())+" "+reasoning_name(sget(a->config,"reasoning_effort"))+" "+permission_name(*a)+(a->plan_mode?" plan":"");
            if(model!=last_model||modes!=last_modes){
                char hint[768],summary[512];
                snprintf(hint,sizeof hint,tr("  当前模型：%s · F2 模型 · F3 推理\n"),model.c_str());
                fputs(ui_paint(Tone::Dim,hint).c_str(),stdout);
                if(a->plan_mode)fputs(ui_paint(Tone::Warning,std::string("  ")+tr("计划模式：只调研并提交计划 · /go 批准执行 · Shift+Tab 或 /plan 退出")+"\n").c_str(),stdout);
                snprintf(summary,sizeof summary,tr("推理模式：%s · 批准模式：%s\n"),reasoning_name(sget(a->config,"reasoning_effort")),permission_name(*a));
                const bool contrast=env_value("DESK_CONTRAST_NUMBERS");
                fputs(ui_paint(contrast?Tone::Plain:Tone::Dim,summary,contrast).c_str(),stdout);
                last_model=std::move(model);
                last_modes=std::move(modes);
            }
        }
        const bool composer=screen&&screen->has_composer();
        if(composer){
            // Park the cursor on the frame's input row; draw_composer paints the
            // rule above it and the status below it.
            printf("\033[%d;1H\033[2K",terminal_rows()-2);
            fflush(stdout);
        }
        const char *prompt=tr("❯ ");
        std::unique_ptr<char,decltype(&free)> line((guided||composer)?editable_input(prompt,nullptr,false,[&]{
            jobs.refresh();
            if(jobs.on_idle)jobs.on_idle();
        },&draft):jobs.readline(prompt),free);
        if(guided&&draft.paused){
            // The approval panel must be on screen before the choices wait for
            // a key; otherwise it arrives later and lands on the input row.
            screen->flush();
            // The worker has just printed the request panel at the bottom of
            // the transcript; draw the choices on the input row right under it
            // so the panel and its answers read as one block.
            if(screen->has_composer()){
                printf("\033[%d;1H\033[2K",terminal_rows()-2);
                fflush(stdout);
            }
            const bool inline_choices=screen->has_composer();
            const int anchor=inline_choices?terminal_rows()-2:content_top;
            // Focus starts on Cancel so a stray Enter never approves anything.
            const bool allow_shell_rule=screen->shell_approval(*conversation);
            const auto choice=terminal_choice(approval_choices(!a->verbose,allow_shell_rule),0,anchor,[&]{
                jobs.refresh();
                if(jobs.on_idle)jobs.on_idle();
            },true,inline_choices);
            if(interrupted==SIGINT){
                conversation->queue.clear();
                jobs.cancel(conversation->job);
            }else if(input_closed){
                conversation->draft=std::move(draft.text);
                conversation->draft_cursor=draft.cursor;
                return false;
            }else if(choice&&*choice>=0){
                jobs.respond(conversation->job,*choice);
            }
            screen->refresh_composer();
            continue;
        }
        if(draft.complete_file){
            draft.complete_file=false;
            const size_t cur=std::min(draft.text.size(),static_cast<size_t>(std::max(0,draft.cursor)));
            size_t at_pos=0;
            std::string query;
            if(find_at_mention(draft.text,cur,at_pos,query)){
                auto matches=workspace_complete_files(*a,query,9);
                auto apply_choice=[&](const std::string &rel){
                    std::string token=rel.find(' ')!=std::string::npos?"@\""+rel+"\" ":"@"+rel+" ";
                    draft.text.replace(at_pos,cur-at_pos,token);
                    draft.cursor=static_cast<int>(at_pos+token.size());
                };
                if(matches.empty()){
                    terminal_message(ui_paint(Tone::Dim,std::string("  ")+tr("没有匹配的工作区文件。"))+"\n");
                }else if(matches.size()==1){
                    apply_choice(matches[0]);
                }else{
                    if(screen){screen->controls_open=true;screen->begin_overlay();}
                    clear_page();
                    const auto heading=ui_clip(tr("选择要引用的文件"),std::max(1,terminal_columns()-4));
                    printf("  %s\n",ui_paint(Tone::Plain,heading,true).c_str());
                    std::vector<std::string> options=matches;
                    options.emplace_back(tr("返回输入"));
                    auto picked=terminal_choice(options,1,content_top+1,[&]{
                        jobs.refresh();
                        if(jobs.on_idle)jobs.on_idle();
                    },true,false,true);
                    if(!picked){
                        for(size_t i=0;i<options.size();i++){
                            auto num=ui_number("["+std::to_string((i+1)%options.size())+"]");
                            printf("  %s %s\n",num.c_str(),options[i].c_str());
                        }
                        picked=number(jobs,static_cast<int>(options.size()-1),0);
                    }
                    if(screen){screen->end_overlay();screen->controls_open=false;}
                    else{
                        clear_page();
                        if(conversation)chat_title(*a,*conversation);
                        if(conversation)show_conversation_history(*a,*conversation);
                        last_modes.clear();
                    }
                    if(picked&&*picked>=1&&static_cast<size_t>(*picked)<=matches.size())
                        apply_choice(matches[static_cast<size_t>(*picked-1)]);
                }
            }
            continue;
        }
        if(draft.paste_image){
            draft.paste_image=false;
            a->error[0]=0;
            std::string clip_token=capture_clipboard_image(*a);
            if(clip_token.empty()){
                terminal_message(ui_paint(Tone::Warning,std::string("  ")+a->error)+"\n");
                a->error[0]=0;
            }else{
                const size_t cur=std::min(draft.text.size(),static_cast<size_t>(std::max(0,draft.cursor)));
                std::string ins=(cur>0&&draft.text[cur-1]!=' ')?(" "+clip_token+" "):(clip_token+" ");
                draft.text.insert(cur,ins);
                draft.cursor=static_cast<int>(cur+ins.size());
                terminal_message(ui_paint(Tone::Accent,std::string("  ")+tr("已从剪贴板附带图片，继续输入说明后按回车发送。"))+"\n");
            }
            continue;
        }
        if(!line){
            if(interrupted==SIGINT&&!input_closed){
                if(screen&&conversation){conversation->queue.clear();jobs.cancel(conversation->job);}
                continue;
            }
            return false;
        }
        std::string task=line.get();
        if(task.empty())continue;
        std::string trimmed=task;
        while(!trimmed.empty()&&isspace(static_cast<unsigned char>(trimmed.back())))trimmed.pop_back();
        // Local actions never reach the model: status card, key help and a
        // context reset that also clears the saved context of this chat.
        auto clear_context=[&]{
            reset_session(a);
            if(conversation){
                conversation->reset_context=true;
                conversation->sync_messages=false;
                persist_conversation(*a,*conversation);
                history_checkpoint(*a,0);
            }
            if(screen)screen->set_context_percent(-1);
            terminal_message(ui_paint(Tone::Accent,std::string("  ")+tr("已清空当前对话上下文，模型不会再看到之前的内容。"))+"\n");
        };
        auto local_action=[&](int action){
            if(action==4)clear_context();
            else if(action==5)show_inline_status(*a);
            else if(action==6)show_inline_help();
        };
        if(trimmed=="/status"){local_action(5);continue;}
        if(trimmed=="/help"){local_action(6);continue;}
        if(trimmed=="/clear"){local_action(4);continue;}
        if(trimmed=="/commands"){show_custom_commands(*a);continue;}
        if(trimmed=="/skills"){
            auto skills=list_skills(*a);
            std::string body;
            for(const auto &skill:skills){
                if(!body.empty())body+='\n';
                body+="/"+skill.name+" ("+skill.scope+")";
                if(!skill.description.empty())body+=" — "+ui_clip(skill.description,90);
            }
            if(body.empty())body=tr("暂无技能。把 <名称>/SKILL.md 放进 .deskcode/skills/、.claude/skills/ 或用户配置目录的 skills/，或安装插件。");
            inline_box(tr("技能"),body);
            continue;
        }
        if(trimmed=="/plugins"){
            std::string body;
            for(const auto &plugin:list_plugins(*a)){
                if(!body.empty())body+='\n';
                body+=std::string(plugin.enabled?"● ":"○ ")+plugin.name+(plugin.version.empty()?"":" "+plugin.version);
                if(!plugin.description.empty())body+=" — "+ui_clip(plugin.description,80);
            }
            if(body.empty())body=tr("尚未安装插件。使用 desk plugin install <目录 | Git 地址 | owner/repo> 安装。");
            inline_box(tr("插件"),body);
            continue;
        }
        if(trimmed=="/agents"){
            std::string body=std::string("coder (built-in) — ")+tr("可写编码子代理（支持独立 git worktree 与后台并行）")+"\n"
                +"research (built-in) — "+tr("只读代码库调研子代理");
            for(const auto &ag:list_subagents(*a)){
                body+="\n"+ag.name+" ("+ag.scope+(ag.readonly?", read-only":"")+(ag.worktree?", worktree":"")+")";
                if(!ag.description.empty())body+=" — "+ui_clip(ag.description,80);
            }
            inline_box(tr("子代理"),body);
            continue;
        }
        if(trimmed=="/sandbox"||trimmed.starts_with("/sandbox ")){
            std::string sub=trimmed.size()>9?trimmed.substr(9):"";
            while(!sub.empty()&&(sub.front()==' '||sub.front()=='\t'))sub.erase(0,1);
            if(sub=="net"||sub=="no-net"||sub=="net on"||sub=="net off"){
                if(!sandbox_net_supported()){
                    a->sandbox_no_net=false;
                    terminal_message(ui_paint(Tone::Dim,std::string("  ")+tr("当前平台或内核不支持命令网络隔离沙箱。"))+"\n");
                }else{
                    if(sub=="net on")a->sandbox_no_net=true;
                    else if(sub=="net off")a->sandbox_no_net=false;
                    else a->sandbox_no_net=!a->sandbox_no_net;
                    terminal_message(ui_paint(Tone::Accent,std::string("  ")+tr(a->sandbox_no_net?"已开启命令网络隔离沙箱（子进程禁止发起网络连接）。":"已关闭命令网络隔离沙箱。"))+"\n");
                }
            }else if(!sandbox_supported()){
                a->sandbox=false;
                terminal_message(ui_paint(Tone::Dim,std::string("  ")+tr("当前平台或内核未启用文件系统内核沙箱。"))+"\n");
            }else{
                if(sub=="on")a->sandbox=true;
                else if(sub=="off")a->sandbox=false;
                else a->sandbox=!a->sandbox;
                terminal_message(ui_paint(Tone::Accent,std::string("  ")+tr(a->sandbox?"已开启内核沙箱（禁止写入工作目录外文件）。":"已关闭当前会话的内核沙箱。"))+"\n");
            }
            continue;
        }
        if(trimmed=="/notify"){
            a->notify=!a->notify;
            terminal_message(ui_paint(Tone::Accent,std::string("  ")+tr(a->notify?"已开启后台任务与待批准系统桌面通知。":"已关闭系统桌面通知。"))+"\n");
            continue;
        }
        auto warn=[&](const char *text){terminal_message(ui_paint(Tone::Warning,std::string("  ")+text)+"\n");};
        auto accent=[&](const std::string &text){terminal_message(ui_paint(Tone::Accent,"  "+text)+"\n");};
        auto busy=[&]{return screen&&conversation&&screen->active(*conversation);};
        if(trimmed=="/plan"||trimmed.starts_with("/plan ")){
            std::string request=trimmed.size()>6?trimmed.substr(6):"";
            request.erase(0,std::min(request.size(),request.find_first_not_of(" \t")));
            if(request.empty()){
                a->plan_mode=!a->plan_mode;
                if(conversation)conversation->plan=a->plan_mode;
                accent(tr(a->plan_mode?"已进入计划模式：只调研并提交计划，不修改文件；批准后再执行。":"已退出计划模式，恢复正常执行。"));
                if(screen)screen->refresh_composer();
                last_modes.clear();
                continue;
            }
            a->plan_mode=true;
            if(conversation)conversation->plan=true;
            if(screen)screen->refresh_composer();
            last_modes.clear();
            task=request;
            trimmed=request;
        }
        if(trimmed=="/go"||trimmed.starts_with("/go ")){
            if(!a->plan_mode){warn(tr("当前不在计划模式；输入 /plan 或按 Shift+Tab 进入。"));continue;}
            a->plan_mode=false;
            if(conversation)conversation->plan=false;
            if(screen)screen->refresh_composer();
            last_modes.clear();
            std::string extra=trimmed.size()>4?trimmed.substr(4):"";
            task=tr("计划已批准，请按计划开始实施，完成后验证结果。");
            if(!extra.empty())task+="\n"+extra;
            trimmed=task;
        }
        if(trimmed=="/bg"){
            if(screen&&conversation&&conversation->job&&jobs.background(conversation->job))
                accent(tr("已请求将正在执行的命令转入后台。"));
            else warn(tr("当前没有可转入后台的命令；也可以让模型用 run_shell 的 background 参数启动长时间运行的命令。"));
            continue;
        }
        if(trimmed=="/cost"){
            JsonPtr states;
            J *state=nullptr;
            if(screen&&conversation&&conversation->job){
                states.reset(jobs.snapshot());
                for(size_t i=0;i<alen(states.get());i++)if(iget(at(states.get(),i),"id",0)==conversation->job)state=at(states.get(),i);
            }
            show_cost(*a,state);
            continue;
        }
        if(trimmed=="/price"||trimmed.starts_with("/price ")){
            const std::string message=set_price(a,std::string_view(trimmed).substr(6));
            if(conversation){
                conversation->config.reset(copy(a->config));
                persist_conversation(*a,*conversation);
            }
            if(screen)screen->refresh_composer();
            terminal_message(ui_paint(Tone::Dim,"  "+message)+"\n");
            continue;
        }
        if(trimmed=="/rewind"){
            if(busy()){warn(tr("当前对话正在执行任务，请等待完成或按 Ctrl+C 停止后再回退。"));continue;}
            if(!conversation||conversation->id.empty()){warn(tr("只有已保存的对话可以回退。"));continue;}
            JsonPtr context(history_context(*a,conversation->id));
            struct Turn {std::string id,text;};
            std::vector<Turn> turns;
            for(size_t i=0;i<alen(context.get());i++){
                J *message=at(context.get(),i);
                if(strcmp(sget(message,"role"),"user")||!*sget(message,"_turn"))continue;
                turns.push_back({sget(message,"_turn"),user_message_text(message)});
            }
            if(turns.empty()){warn(tr("没有可回退的轮次；更早的内容可能已压缩进摘要。"));continue;}
            std::reverse(turns.begin(),turns.end());
            if(turns.size()>15)turns.resize(15);
            std::vector<std::string> options;
            const int option_width=terminal_columns()>0?std::max(20,terminal_columns()-16):72;
            for(const auto &turn:turns){
                std::string text=turn.text;
                for(char &c:text)if(c=='\n'||c=='\r'||c=='\t')c=' ';
                options.push_back(ui_clip(text,option_width));
            }
            options.emplace_back(tr("返回"));
            if(screen){screen->controls_open=true;screen->begin_overlay();}
            clear_page();
            menu_text(tr("\n回退到哪条需求之前？（该需求及之后的对话会被移除）"));
            const int choice=arrow_menu(jobs,options);
            int mode=0;
            if(choice>0&&static_cast<size_t>(choice)<=turns.size()){
                clear_page();
                mode=menu_choices(jobs,tr("\n回退内容\n  [1] 对话和文件改动\n  [2] 仅对话（保留文件）\n  [0] 取消"),nullptr,1);
            }
            if(screen){screen->end_overlay();screen->controls_open=false;}
            if(choice<=0||static_cast<size_t>(choice)>turns.size()||mode<=0){
                if(!screen||!screen->has_composer()){clear_page();chat_title(*a,*conversation);show_conversation_history(*a,*conversation);}
                continue;
            }
            const size_t index=static_cast<size_t>(choice-1);
            std::vector<std::string> ids;
            for(size_t i=0;i<=index;i++)ids.push_back(turns[i].id);
            std::string summary;
            a->error[0]=0;
            if(mode==1&&!undo_turns(*a,ids,summary)){
                warn(summary.empty()?a->error:summary.c_str());
                a->error[0]=0;
                continue;
            }
            if(!history_rewind(*a,turns[index].id)){
                warn(mode==1?tr("文件已回退，但无法改写对话记录。"):tr("无法改写对话记录，对话未回退。"));
                continue;
            }
            conversation->sync_messages=true;
            conversation->reset_context=false;
            conversation->prompts.clear();
            {
                JsonPtr record(history_read(*a,conversation->id));
                J *messages=get(record.get(),"transcript");
                for(size_t i=0;i<alen(messages);i++){
                    J *message=at(messages,i);
                    if(strcmp(sget(message,"role"),"user"))continue;
                    std::string text=user_message_text(message);
                    if(!text.empty()&&(conversation->prompts.empty()||conversation->prompts.back()!=text))conversation->prompts.push_back(std::move(text));
                }
            }
            if(conversation->job)conversation->displayed=output_end(jobs,conversation->job);
            // The request comes back into the input box to edit and resend.
            draft.text.clear();
            for(char c:turns[index].text){
                if(c=='\n'){draft.text+="\xE2\x86\xB5";draft.newline_inserted=true;}
                else if(c!='\r')draft.text+=c;
            }
            draft.cursor=static_cast<int>(draft.text.size());
            if(screen){screen->select(conversation);screen->set_context_percent(-1);}
            else clear_page();
            chat_title(*a,*conversation);
            show_conversation_history(*a,*conversation);
            std::string done=tr("已回退到所选需求之前，可修改后重新发送。");
            if(mode==1&&!summary.empty())done+="\n  "+summary;
            accent(done);
            last_modes.clear();
            continue;
        }
        if(trimmed=="/undo"){
            if(screen&&conversation&&screen->active(*conversation)){
                terminal_message(ui_paint(Tone::Warning,std::string("  ")+tr("当前对话正在执行任务，请等待完成或按 Ctrl+C 停止后再撤回。"))+"\n");
                continue;
            }
            a->error[0]=0;
            std::string summary;
            if(undo_last_turn(*a,summary))
                terminal_message(ui_paint(Tone::Accent,"  "+summary)+"\n");
            else
                terminal_message(ui_paint(Tone::Warning,std::string("  ")+(summary.empty()?a->error:summary))+"\n");
            a->error[0]=0;
            continue;
        }
        if(trimmed=="/compact"){
            if(screen&&conversation&&screen->active(*conversation)){
                terminal_message(ui_paint(Tone::Warning,std::string("  ")+tr("当前对话正在执行任务，请等待完成后再压缩上下文。"))+"\n");
                continue;
            }
            if(conversation&&!conversation->id.empty()&&!conversation->reset_context){
                J *latest=history_context(*a,conversation->id);
                if(alen(latest)>alen(a->messages)){
                    json_object_put(a->messages);
                    a->messages=latest;
                }else json_object_put(latest);
            }
            a->error[0]=0;
            if(screen&&screen->has_composer())
                terminal_activity(std::string("⠋ ")+tr("正在生成上下文摘要…"));
            else
                terminal_message(ui_paint(Tone::Dim,std::string("  ")+tr("正在生成上下文摘要…"))+"\n");
            std::string summary;
            const bool ok=compact_conversation(*a,summary);
            terminal_activity("");
            if(!ok){
                terminal_message(ui_paint(Tone::Warning,std::string("  ")+a->error)+"\n");
                a->error[0]=0;
                continue;
            }
            if(conversation){
                conversation->reset_context=false;
                conversation->sync_messages=true;
                persist_conversation(*a,*conversation);
                history_checkpoint(*a,alen(a->messages));
            }
            if(screen)screen->set_context_percent(1);
            const int width=terminal_columns()>0?std::min(std::max(terminal_columns()-6,36),88):76;
            std::string box=ui_box(tr("上下文已压缩为续接摘要"),summary,width);
            std::string out="\n";
            for(size_t start=0;start<box.size();){
                auto end=box.find('\n',start);
                out+="  "+box.substr(start,end==std::string::npos?std::string::npos:end-start)+"\n";
                if(end==std::string::npos)break;
                start=end+1;
            }
            terminal_message(out);
            continue;
        }
        if(trimmed=="/paste"||trimmed.starts_with("/paste ")){
            a->error[0]=0;
            std::string clip_token=capture_clipboard_image(*a);
            if(clip_token.empty()){
                terminal_message(ui_paint(Tone::Warning,std::string("  ")+a->error)+"\n");
                a->error[0]=0;
                continue;
            }
            if(trimmed=="/paste"){
                draft.text=clip_token+" ";
                draft.cursor=static_cast<int>(draft.text.size());
                terminal_message(ui_paint(Tone::Accent,std::string("  ")+tr("已从剪贴板附带图片，继续输入说明后按回车发送。"))+"\n");
                continue;
            }
            task=clip_token+" "+trimmed.substr(7);
            trimmed=task;
        }
        if(trimmed=="/"||trimmed=="/model"||trimmed=="/reasoning"||trimmed=="/approval"||trimmed=="/perm"||trimmed=="/mcp"||trimmed=="/new"){
            size_t revision=a->session_revision,config_revision=a->config_revision;
            a->error[0]=0;
            if(screen&&trimmed!="/new"){screen->controls_open=true;screen->begin_overlay();}
            int selected=1,action=0;
            if(trimmed=="/model")select_model(a,jobs);
            else if(trimmed=="/reasoning")reasoning_menu(a,jobs);
            else if(trimmed=="/approval"||trimmed=="/perm")approval_menu(a,jobs);
            else if(trimmed=="/mcp")mcp_menu(a,jobs);
            else if(trimmed=="/new")selected=3;
            else{
                selected=chat_menu(a,jobs);
                if(selected>=4){action=selected;selected=1;}
            }
            if(screen&&trimmed!="/new"){screen->end_overlay();screen->controls_open=false;}
            if(conversation){
                if(revision!=a->session_revision)conversation->reset_context=true;
                if(config_revision!=a->config_revision)
                    use_reasoning(a->config,get(conversation->config.get(),"reasoning_effort"));
                conversation->config.reset(copy(a->config));
                conversation->yes=a->yes;
                conversation->auto_edit=a->auto_edit;
                persist_conversation(*a,*conversation);
            }
            if(selected<0||selected==2)return false;
            if(selected==3){if(conversation)return true;reset_session(a);}
            if(screen&&screen->has_composer()){
                if(a->error[0])terminal_message(std::string(a->error)+"\n");
                show_notice();
                local_action(action);
                continue;
            }
            clear_page();
            if(a->error[0])fprintf(stderr,"%s\n",a->error);
            if(conversation)chat_title(*a,*conversation);
            show_notice();
            // Put the transcript back the way opening the chat does; anything the
            // worker printed while the menu was up follows from the job output.
            if(conversation)show_conversation_history(*a,*conversation);
            local_action(action);
            if(screen)screen->refresh_composer();
            last_modes.clear();
            continue;
        }
        std::string custom_label;
        if(trimmed.size()>1&&trimmed.front()=='/'){
            std::string expanded_cmd;
            if(expand_custom_command(*a,trimmed,expanded_cmd,custom_label)
               ||expand_skill_command(*a,trimmed,expanded_cmd,custom_label)
               ||mcp_expand_prompt(*a,trimmed,expanded_cmd,custom_label)){
                task=std::move(expanded_cmd);
            }
        }
        if(conversation){
            if(conversation->title.empty())conversation->title=conversation_title(custom_label.empty()?task:custom_label,true);
            persist_conversation(*a,*conversation);
        }
        if(screen&&conversation){
            // Guided approvals have their own choice row; there a lone digit
            // is an ordinary request and never answers a pending approval.
            if(!guided&&task.size()==1&&task[0]>='0'&&task[0]<='4'&&jobs.respond(conversation->job,task[0]-'0'))continue;
            remember(custom_label.empty()?task:custom_label);
            if(!screen->submit(*conversation,task,custom_label))screen->report_error();
            continue;
        }
        remember(custom_label.empty()?task:custom_label);
        if(run_task(a,task.c_str())&&!input_closed)fprintf(stderr,"%s\n",a->error);
    }
}
static void show_token_usage(J *usage,J *config){
    if(usage)printf(tr("  累计词元（接口报告）：%s\n"),token_label(model_tokens(usage,config)).c_str());
    else menu_text(tr("  无法读取累计词元用量。"));
}
// "~/Documents/x" reads better than a full home path in headers.
static std::string display_path(const char *path){
    const char *home=env_value("HOME");
    if(home&&*home){
        const size_t length=strlen(home);
        if(!strncmp(path,home,length)&&(path[length]=='/'||!path[length]))return "~"+std::string(path+length);
    }
    return path;
}
static void show_banner(const App &app){
    printf("\n  %s %s\n",
           ui_paint(Tone::Accent,"DeskCode",true).c_str(),
           ui_number(VERSION).c_str());
    fputs("  ",stdout);
    const bool contrast=env_value("DESK_CONTRAST_NUMBERS");
    std::string subtitle=std::string(tr("终端编程助手"))+" · "+display_path(app.root);
    const int columns=terminal_columns();
    if(columns>8)subtitle=ui_clip(subtitle,columns-4);
    fputs(ui_paint(contrast?Tone::Plain:Tone::Dim,subtitle,contrast).c_str(),stdout);
    putchar('\n');
    page_rule();
}
static void conversation_menu(App *a,Tasks &jobs,std::vector<Conversation> &items,int start_mode=0){
    ConversationScreen screen(*a,jobs,items);
    // start_mode: 0 show the list, 1 open a new chat, 2 resume the latest one.
    bool new_chat=start_mode==1,resume=start_mode==2;
    while(interrupted!=SIGTERM&&!input_closed){
        const int count=static_cast<int>(items.size());
        const int create=count+1,rename=count+2,remove=count+3;
        int choice=create;
        if(resume){
            resume=false;
            choice=items.empty()?create:1;
        }else if(!new_chat){
            clear_page();
            menu_text(tr("\n选择对话"));
            if(items.empty())menu_text(tr("还没有对话。"));
            std::vector<std::string> options;
            for(auto &item:items){
                options.emplace_back(title_of(item));
                if(!item.queue.empty()){
                    char queued[80];
                    snprintf(queued,sizeof queued,tr(" · 排队：%zu"),item.queue.size());
                    options.back()+=queued;
                }
            }
            for(auto label:{"新建对话","重命名对话","删除对话","返回主菜单"})options.emplace_back(tr(label));
            if(auto selected=arrow_choice(jobs,options)){
                choice=*selected;
            }else{
                for(size_t i=0;i<items.size();i++){
                    auto label=ui_number(std::to_string(i+1)+".");
                    printf("  %s %s\n",label.c_str(),options[i].c_str());
                }
                menu_printf(tr("\n  [%d] 新建对话\n  [%d] 重命名对话\n  [%d] 删除对话\n  [0] 返回主菜单\n"),create,rename,remove);
                choice=number(jobs,remove);
            }
            if(choice<=0)return;
        }
        new_chat=false;
        if(choice==rename||choice==remove){
            if(items.empty()){
                menu_text(tr("还没有对话。"));
                continue;
            }
            bool deleting=choice==remove;
            menu_text(tr(deleting?"输入上方要删除的对话编号（0 返回）。":"输入上方要重命名的对话编号（0 返回）。"));
            std::vector<std::string> options;
            for(auto &item:items)options.emplace_back(title_of(item));
            options.emplace_back(tr("返回"));
            int which=arrow_menu(jobs,options);
            if(which<=0)continue;
            auto item=items.begin()+(which-1);
            if(deleting){
                menu_printf(tr("删除「%s」？"),title_of(*item));
                if(arrow_menu(jobs,{tr("确认删除"),tr("取消")},0)==1){
                    if(!history_save(*a,item->id,item->title,item->config.get(),true)){
                        set_notice(tr("无法删除本机对话记录。"));continue;
                    }
                    if(item->job)jobs.remove(item->job);
                    items.erase(item);
                }
            }else{
                auto title=conversation_title(request_text(jobs,tr("新标题（空行取消）：")),false);
                if(!title.empty()){item->title=std::move(title);persist_conversation(*a,*item);}
            }
            continue;
        }
        if(choice==create){
            if(config_valid(a,a->config)){set_notice(tr("请先选择 [3] 添加模型，或选择 [2] 选择模型。"));return;}
            items.push_back({{},JsonPtr(arr()),JsonPtr(copy(a->config))});
            items.back().yes=a->yes;
            items.back().auto_edit=a->auto_edit;
            items.back().plan=a->plan_mode;
            persist_conversation(*a,items.back());
        }
        auto selected=items.begin()+(choice-1);
        auto &item=*selected;
        JsonPtr previous(a->messages);
        a->messages=item.messages.release();
        a->conversation_id=item.id;
        const bool previous_yes=a->yes,previous_auto_edit=a->auto_edit,previous_plan=a->plan_mode;
        a->yes=item.yes;
        a->auto_edit=item.auto_edit;
        a->plan_mode=item.plan;
        json_object_put(a->config);a->config=copy(item.config.get());
        a->state="等待任务";
        screen.reopen(item);
        new_chat=chat(a,jobs,&item,&screen);
        screen.select(nullptr);
        item.messages.reset(a->messages);a->messages=previous.release();
        item.config.reset(copy(a->config));
        item.yes=a->yes;
        item.auto_edit=a->auto_edit;
        item.plan=a->plan_mode;
        persist_conversation(*a,item);
        a->conversation_id.clear();
        a->yes=previous_yes;
        a->auto_edit=previous_auto_edit;
        a->plan_mode=previous_plan;
        use_reasoning(a->config,profile_reasoning(*a));
        std::rotate(items.begin(),selected,selected+1);
        if(interrupted==SIGTERM)return;
        // Terminal EOF closes the current chat. The selector gets a fresh input.
        input_closed=false;
        if(interrupted==SIGINT)interrupted=0;
    }
}
static int menu(App *a,int start_mode=0){
    InterfaceScreen interface;
    std::string notice;
    Tasks jobs;
    std::vector<Conversation> conversations;
    JsonPtr saved(history_list(*a));
    for(size_t i=0;i<alen(saved.get());i++){
        J *entry=at(saved.get(),i);
        J *profile=get(get(a->store,"profiles"),sget(entry,"profile"));
        JsonPtr selected(copy(profile?profile:a->config));
        J *reasoning=get(entry,"reasoning_effort");
        if(json_object_is_type(reasoning,json_type_string))use_reasoning(selected.get(),reasoning);
        App view;
        view.config=selected.get();
        snprintf(view.config_dir,sizeof view.config_dir,"%s",a->config_dir);
        std::string id=sget(entry,"id");
        conversations.push_back({sget(entry,"title"),JsonPtr(history_context(view,id)),std::move(selected)});
        auto &conversation=conversations.back();
        conversation.id=std::move(id);
        std::string_view approval=sget(entry,"approval_mode");
        bool saved_approval=approval=="full"||approval=="edit"||approval=="ask";
        conversation.yes=saved_approval?approval=="full":a->yes;
        conversation.auto_edit=saved_approval?approval=="edit":a->auto_edit;
    }
    size_t synced_config=a->config_revision;
    jobs.on_idle=[&]{
        if(synced_config!=a->config_revision){
            sync_conversation_models(*a,conversations);
            synced_config=a->config_revision;
        }
        advance_conversations(jobs,conversations);
    };
    if(start_mode)conversation_menu(a,jobs,conversations,start_mode);
    for(;;){
        if(input_closed)return 0;
        JsonPtr usage(load_tokens(*a));
        J *profiles=get(a->store,"profiles");
        const char *active_model=sget(a->config,"model");
        const char *active_profile=sget(a->config,"profile");
        const bool first_run=!json_object_object_length(profiles)&&!*active_model;
        std::string current_notice=notice;
        notice.clear();
        auto draw_home=[&]{
            clear_page();
            show_banner(*a);
            show_notice();
            if(!current_notice.empty())puts(current_notice.c_str());
            menu_text(tr("  已导入的模型"));
            bool active_listed=false;
            if(!json_object_object_length(profiles))menu_text(tr("    暂无已导入的模型"));
            else{
                size_t index=1;
                json_object_object_foreach(profiles,name,profile){
                    const char *model=sget(profile,"model");
                    bool current=*active_model&&!strcmp(name,active_profile)&&!strcmp(model,active_model);
                    if(env_value("DESK_CONTRAST_NUMBERS")){
                        const Tone tone=current?Tone::Accent:Tone::Plain;
                        std::string row="    "+ui_number(std::to_string(index++)+".",tone)
                            +ui_paint(tone," "+std::string(model),!current);
                        if(usage){
                            row+=ui_paint(tone," · ");
                            row+=ui_number(token_label(model_tokens(usage.get(),profile)),tone);
                            row+=ui_paint(tone,tr(" 词元"),!current);
                        }
                        if(current)row+=ui_paint(tone,tr(" · 当前"));
                        puts(row.c_str());
                        if(current)active_listed=true;
                        continue;
                    }
                    char row[640];
                    int used=snprintf(row,sizeof row,"    %zu. %s",index++,model);
                    if(usage&&used>0&&used<(int)sizeof row)used+=snprintf(row+used,sizeof row-(size_t)used,tr(" · %s 词元"),token_label(model_tokens(usage.get(),profile)).c_str());
                    if(current&&used>0&&used<(int)sizeof row)snprintf(row+used,sizeof row-(size_t)used,"%s",tr(" · 当前"));
                    // The active profile reads as accent; the rest stay quiet.
                    puts(current?ui_paint(Tone::Accent,row).c_str():ui_paint(Tone::Dim,row).c_str());
                    if(current)active_listed=true;
                }
            }
            if(!active_listed){
                if(*active_model){
                    printf(tr("  当前模型：%s\n"),active_model);
                    show_token_usage(usage.get(),a->config);
                }else menu_text(tr("  当前模型：未配置"));
            }
            if(!usage&&(active_listed||!*active_model))menu_text(tr("  无法读取累计词元用量。"));
            if(first_run){
                putchar('\n');
                const int columns=terminal_columns();
                const int width=columns>12?std::min(columns-4,96):96;
                for(const auto &line:ui_wrap(tr("首次使用：选择「添加模型」，内置 DeepSeek、Kimi、通义千问、智谱、OpenRouter、Ollama 等预设，填入 API Key 即可开始。"),width))
                    puts(("  "+ui_paint(Tone::Accent,line)).c_str());
            }
            page_rule();
        };
        draw_home();
        int choice=arrow_menu(jobs,{tr("开始对话"),tr("选择模型"),tr("添加模型"),tr("MCP 服务"),tr("设置"),tr("退出")},first_run?3:-1,false,draw_home);
        if(choice<0){if(interrupted==SIGINT&&!input_closed)continue;return 0;}
        if(!choice)return 0;
        int rc=0;
        if(choice==1)conversation_menu(a,jobs,conversations);
        else if(choice==2)rc=select_model(a,jobs);
        else if(choice==3)rc=add_model(a,jobs,nullptr);
        else if(choice==4)mcp_menu(a,jobs);
        else if(choice==5)settings_menu(a,jobs);
        if(rc)notice=a->error;
        if(interrupted==SIGTERM)return 143;
    }
}
static void help(void){
    menu_text(tr("用法：desk [选项] [命令 | 需求]\n\n常用\n  desk                    打开主菜单（首次使用会引导添加模型）\n  desk chat               新建一个对话\n  desk -c, --continue     继续本目录最近的对话\n  desk \"需求\"             执行一次请求后退出（同 -p \"需求\"）\n  echo 需求 | desk -p -   从标准输入读取需求\n\n自动化与脚本\n  --json                  只在标准输出打印一个 JSON 结果（status/reply/tokens/tools）\n  --approval 模式         ask（默认）| edit 自动批准文件编辑 | full 自动批准全部\n  --yes                   等同 --approval full\n  --max-steps N           0..100，0 为不限（默认）\n  --plan                  计划模式：只调研并给出计划，不修改文件\n  标准输出不是终端时只输出最终回复，进度与提示写到标准错误。\n  退出码：0 完成 · 1 未完成 · 2 参数错误 · 130 已中断\n\n其他命令\n  desk serve [--port 7890] 启动多端共享会话守护进程与内置 Web 控制台\n  desk attach [URL]       连接正在运行的 desk serve 共享会话\n  desk acp                启动 Agent Client Protocol（Zed / JetBrains 等 IDE 接入）\n  desk mcp <子命令>       管理 MCP 服务与 OAuth 授权（list / auth / token / logout）\n  desk plugin <子命令>    管理插件包（list / install / update / enable / disable / remove / validate）\n  desk doctor [--json]    环境检查\n  desk config             编辑当前模型配置\n  desk inspect --app 名称 读取桌面控件（需桌面支持）\n\n选项\n  --workspace 目录  --provider 配置名  --lang zh|en\n  --no-shell  --no-sandbox  --no-net  --notify  --allow-desktop  --record  --verbose  --version  --help"));
}
#ifdef _WIN32
#define desk_dup _dup
#define desk_dup2 _dup2
#define desk_fdopen _fdopen
#else
#define desk_dup dup
#define desk_dup2 dup2
#define desk_fdopen fdopen
#endif
static int doctor(App &a,bool json){
#ifdef _WIN32
    const bool writable=true;
#else
    const bool writable=access(a.root,W_OK)==0;
#endif
    JsonPtr probe(copy(a.config));
    a.error[0]=0;
    const bool model_ok=!config_valid(&a,probe.get());
    const std::string model_error=model_ok?std::string():std::string(a.error);
    a.error[0]=0;
    JsonPtr servers(mcp_load(a));
    size_t mcp_total=0,mcp_enabled=0;
    if(servers){
        json_object_object_foreach(servers.get(),server_name,entry){
            (void)server_name;
            mcp_total++;
            if(!bget(entry,"disabled"))mcp_enabled++;
        }
    }
    const std::string mcp_error=servers?std::string():std::string(a.error);
    JsonPtr conversations(history_list(a));
    const bool ok=writable&&model_ok&&servers;
    if(json){
        JsonPtr result(obj());
        put(result.get(),"version",str(VERSION));
        put(result.get(),"ok",json_object_new_boolean(ok));
        put(result.get(),"workspace",str(a.root));
        put(result.get(),"workspace_writable",json_object_new_boolean(writable));
        put(result.get(),"config_dir",str(a.config_dir));
        put(result.get(),"model_configured",json_object_new_boolean(model_ok));
        put(result.get(),"model",str(sget(probe.get(),"model")));
        put(result.get(),"protocol",str(sget(probe.get(),"protocol")));
        put(result.get(),"base_url",str(sget(probe.get(),"base_url")));
        if(!model_ok)put(result.get(),"model_error",str(model_error.c_str()));
        put(result.get(),"shell",json_object_new_boolean(a.shell));
        put(result.get(),"sandbox_support",json_object_new_boolean(sandbox_supported()));
        put(result.get(),"sandbox",json_object_new_boolean(a.sandbox&&sandbox_supported()));
        put(result.get(),"sandbox_backend",str(sandbox_backend()));
        put(result.get(),"sandbox_net_support",json_object_new_boolean(sandbox_net_supported()));
        put(result.get(),"sandbox_no_net",json_object_new_boolean(a.sandbox_no_net&&sandbox_net_supported()));
        put(result.get(),"desktop_support",json_object_new_boolean(desktop_compiled()));
        put(result.get(),"mcp_configured",json_object_new_int64(static_cast<int64_t>(mcp_total)));
        put(result.get(),"mcp_enabled",json_object_new_int64(static_cast<int64_t>(mcp_enabled)));
        if(!servers)put(result.get(),"mcp_error",str(mcp_error.c_str()));
        put(result.get(),"conversations",json_object_new_int64(static_cast<int64_t>(alen(conversations.get()))));
        puts(json_object_to_json_string_ext(result.get(),JSON_C_TO_STRING_PRETTY));
        return ok?0:1;
    }
    printf("\n  %s %s · %s\n\n",ui_paint(Tone::Accent,"DeskCode",true).c_str(),VERSION,tr("环境检查"));
    auto row=[](int state,const char *label,const std::string &value){
        const char *mark=state>0?"✓":state==0?"·":"✗";
        const Tone tone=state>0?Tone::Success:state==0?Tone::Dim:Tone::Error;
        std::string lead=tr(label);
        while(ui_width(lead)<10)lead+=' ';
        printf("  %s %s%s\n",ui_paint(tone,mark).c_str(),ui_paint(Tone::Dim,lead).c_str(),value.c_str());
    };
    row(writable?1:-1,"工作目录",display_path(a.root)+(writable?"":tr("（不可写）")));
    row(model_ok?1:-1,"模型配置",model_ok?std::string(sget(probe.get(),"model"))+" ("+sget(probe.get(),"protocol")+") · "+sget(probe.get(),"base_url"):model_error);
    row(a.shell?1:0,"命令工具",a.shell
        ?(sandbox_supported()&&a.sandbox
            ?(std::string(tr("已开启（沙箱："))+sandbox_backend()+(a.sandbox_no_net&&sandbox_net_supported()?tr(" · 禁网"):"")+")")
            :tr("已开启"))
        :tr("已关闭"));
#ifdef __APPLE__
    row(desktop_compiled()?1:0,"桌面工具",desktop_compiled()?tr("System Events 可用，可在设置中开启"):tr("当前构建未启用"));
#elif defined(_WIN32)
    row(desktop_compiled()?1:0,"桌面工具",desktop_compiled()?tr("UI Automation 可用，可在设置中开启"):tr("当前构建未启用"));
#else
    row(desktop_compiled()?1:0,"桌面工具",desktop_compiled()?tr("AT-SPI 可用，可在设置中开启"):tr("当前构建未启用"));
#endif
    char mcp_line[160];
    snprintf(mcp_line,sizeof mcp_line,tr("%zu 个已启用 / 共 %zu 个"),mcp_enabled,mcp_total);
    row(servers?1:-1,"MCP 服务",servers?std::string(mcp_line):mcp_error);
    char history_line[96];
    snprintf(history_line,sizeof history_line,tr("本目录 %zu 个"),alen(conversations.get()));
    row(1,"对话记录",history_line);
    row(1,"配置目录",display_path(a.config_dir));
    const std::string verdict=ok?tr("全部检查通过。"):tr("有需要处理的项目；首次使用请运行 desk 并选择「添加模型」。");
    printf("\n  %s\n\n",ui_paint(ok?Tone::Success:Tone::Warning,verdict).c_str());
    return ok?0:1;
}
// One request without a menu. When stdout is not a terminal (a pipe, a file,
// another agent) only the final reply goes there; progress and prompts go to
// stderr. --json replaces the reply with one machine-readable result object.
static int run_once(App &a,const char *task,bool json_output){
    const bool headless=json_output||!isatty(1);
    int saved_stdout=-1;
    FILE *reply_stream=nullptr;
    if(headless){
        fflush(stdout);
        saved_stdout=desk_dup(1);
        if(saved_stdout>=0&&desk_dup2(2,1)>=0&&!json_output){
            int reply_fd=desk_dup(saved_stdout);
            if(reply_fd>=0)reply_stream=desk_fdopen(reply_fd,"w");
            a.reply_out=reply_stream;
        }
    }
    a.json_output=json_output;
    if(json_output)a.trace=arr();
    const double started=monotime();
    int rc=1;
    const char *status_name="failed";
    if(config_valid(&a,a.config)){
        fprintf(stderr,tr("%s 请运行 desk，在菜单中添加模型。\n"),a.error);
        status_name="not_configured";
    }else{
        if(!headless)status(&a);
        rc=run_task(&a,task)?1:0;
        if(rc)fprintf(stderr,"%s\n",a.error);
        notify_user_event(a,rc==0?tr("DeskCode 任务已完成"):tr("DeskCode 任务未完成"),task);
        if(!rc)status_name="completed";
        else if(interrupted)status_name="interrupted";
        else if(a.approval_unavailable)status_name="approval_required";
        else if(!strcmp(a.state,"已取消"))status_name="denied";
        else if(!strcmp(a.error,tr("达到步骤上限，任务未确认完成。")))status_name="step_limit";
    }
    if(json_output){
        std::string reply;
        J *last=at(a.messages,alen(a.messages)?alen(a.messages)-1:0);
        if(!rc&&!strcmp(sget(last,"role"),"assistant"))reply=sget(last,"content");
        JsonPtr result(obj());
        put(result.get(),"status",str(status_name));
        put(result.get(),"reply",str(reply.c_str()));
        if(rc)put(result.get(),"error",str(a.error));
        put(result.get(),"model",str(sget(a.config,"model")));
        put(result.get(),"protocol",str(sget(a.config,"protocol")));
        put(result.get(),"workspace",str(a.root));
        put(result.get(),"tokens",json_object_new_int64(static_cast<int64_t>(std::min<std::uint64_t>(a.task_tokens,INT64_MAX))));
        {
            J *usage=obj();
            put(usage,"input",json_object_new_int64(static_cast<int64_t>(std::min<std::uint64_t>(a.turn_usage.input,INT64_MAX))));
            put(usage,"output",json_object_new_int64(static_cast<int64_t>(std::min<std::uint64_t>(a.turn_usage.output,INT64_MAX))));
            put(usage,"cache_read",json_object_new_int64(static_cast<int64_t>(std::min<std::uint64_t>(a.turn_usage.cache_read,INT64_MAX))));
            if(a.turn_usage.priced){
                put(usage,"cost",json_object_new_double(a.turn_usage.cost));
                if(*sget(a.config,"price_currency"))put(usage,"currency",str(sget(a.config,"price_currency")));
            }
            put(result.get(),"usage",usage);
        }
        if(a.plan_mode)put(result.get(),"plan_submitted",json_object_new_boolean(a.plan_submitted));
        put(result.get(),"elapsed_seconds",json_object_new_double(static_cast<double>(static_cast<long long>((monotime()-started)*10))/10));
        put(result.get(),"tools",a.trace?copy(a.trace):arr());
        put(result.get(),"version",str(VERSION));
        const char *text=json_object_to_json_string_ext(result.get(),JSON_C_TO_STRING_PLAIN);
        FILE *out=saved_stdout>=0?desk_fdopen(saved_stdout,"w"):stdout;
        if(out){
            fputs(text,out);
            fputc('\n',out);
            fflush(out);
            if(out!=stdout){fclose(out);saved_stdout=-1;}
        }
    }
    if(reply_stream){fflush(reply_stream);fclose(reply_stream);a.reply_out=nullptr;}
    if(saved_stdout>=0){
        fflush(stdout);
        desk_dup2(saved_stdout,1);
#ifdef _WIN32
        _close(saved_stdout);
#else
        close(saved_stdout);
#endif
    }
    return rc;
}
static int run_cli(int argc,char **argv){
    setlocale(LC_ALL,"");
    platform_initialize();
#ifndef _WIN32
    struct sigaction action{};
    action.sa_handler=on_signal;
    sigemptyset(&action.sa_mask);
    sigaction(SIGINT,&action,NULL);
    sigaction(SIGTERM,&action,NULL);
    signal(SIGPIPE,SIG_IGN);
#endif
    if(argc==3&&!strcmp(argv[1],"--task-worker")){
        char *end=nullptr;long long fd=strtoll(argv[2],&end,10);
        return *end||fd<3?2:task_worker(static_cast<std::intptr_t>(fd));
    }
    {
        int cmd_idx=-1;
        const char *ws=".";
        for(int i=1;i<argc;i++){
            if(!strcmp(argv[i],"--lang")&&i+1<argc){i++;continue;}
            if(!strcmp(argv[i],"--workspace")&&i+1<argc){ws=argv[++i];continue;}
            if(argv[i][0]!='-'){cmd_idx=i;break;}
        }
        if(cmd_idx>=1&&(!strcmp(argv[cmd_idx],"plugin")||!strcmp(argv[cmd_idx],"mcp"))){
            App store{};
            auto cleanup_store=[](App *app){app_cleanup(app);};
            std::unique_ptr<App,decltype(cleanup_store)> owned(&store,cleanup_store);
            if(!canonical_directory(ws,store.root)||store_load(&store)){fprintf(stderr,"%s\n",store.error);return 1;}
            return !strcmp(argv[cmd_idx],"plugin")?plugin_cli(store,argc,argv,cmd_idx+1):mcp_cli(store,argc,argv,cmd_idx+1);
        }
    }
    App a={
        .yes=false,.shell=true,.desktop=false,.record=false,.max_steps=0,.state="等待任务",.stream_text={}
    };
    auto cleanup_app=[](App *app){app_cleanup(app);};
    std::unique_ptr<App,decltype(cleanup_app)> resources(&a,cleanup_app);
    const char *workspace=".",*profile=NULL,*task=NULL,*app="";
    bool json_output=false,resume=false;
    std::vector<char *> sub_argv;
    std::string stdin_task;
    int rc=0;
    for(int i=1;i<argc;i++){
        const char *arg=argv[i];
        const bool in_serve_or_attach=task&&(!strcmp(task,"serve")||!strcmp(task,"attach"));
        if(!strcmp(arg,"--version")){
            puts(VERSION);
            goto cleanup;
        }
        if(!strcmp(arg,"--help")||!strcmp(arg,"-h")){
            help();
            goto cleanup;
        }
        if(in_serve_or_attach&&(
            !strcmp(arg,"--host")||!strcmp(arg,"--port")||!strcmp(arg,"--token")||!strcmp(arg,"--port-file")||
            !strcmp(arg,"--session")||!strcmp(arg,"--approve")||!strcmp(arg,"-p")||!strcmp(arg,"--print"))){
            if(i+1>=argc){rc=2;fputs(tr("选项缺少参数。\n"),stderr);goto cleanup;}
            sub_argv.push_back(argv[i]);
            sub_argv.push_back(argv[++i]);
            continue;
        }
        if(in_serve_or_attach&&(!strcmp(arg,"--json")||(!strncmp(arg,"http",4)&&arg[0]!='-'))){
            sub_argv.push_back(argv[i]);
            continue;
        }
        if(!strcmp(arg,"-p")||!strcmp(arg,"--print")){
            if(i+1>=argc){
                rc=2;
                fputs(tr("-p 需要一个需求文本。\n"),stderr);
                goto cleanup;
            }
            task=argv[++i];
            continue;
        }
        if(!strcmp(arg,"--lang")){i++;continue;}
        if(!strcmp(arg,"--yes")){a.yes=true;a.auto_edit=false;}
        else if(!strcmp(arg,"--json"))json_output=true;
        else if(!strcmp(arg,"--plan"))a.plan_mode=true;
        else if(!strcmp(arg,"-c")||!strcmp(arg,"--continue"))resume=true;
        else if(!strcmp(arg,"--no-shell"))a.shell=false;
        else if(!strcmp(arg,"--no-sandbox"))a.sandbox=false;
        else if(!strcmp(arg,"--no-net")||!strcmp(arg,"--sandbox-no-net"))a.sandbox_no_net=true;
        else if(!strcmp(arg,"--notify"))a.notify=true;
        else if(!strcmp(arg,"--allow-shell"))a.shell=true;
        else if(!strcmp(arg,"--allow-desktop"))a.desktop=true;
        else if(!strcmp(arg,"--record"))a.record=true;
        else if(!strcmp(arg,"--verbose"))a.verbose=true;
        else if(!strcmp(arg,"--workspace")||!strcmp(arg,"--provider")||!strcmp(arg,"--app")||!strcmp(arg,"--max-steps")||!strcmp(arg,"--approval")){
            if(i+1>=argc){
                rc=2;
                fputs(tr("选项缺少参数。\n"),stderr);
                goto cleanup;
            }
            const char *v=argv[++i];
            if(!strcmp(arg,"--workspace"))workspace=v;
            else if(!strcmp(arg,"--provider"))profile=v;
            else if(!strcmp(arg,"--app"))app=v;
            else if(!strcmp(arg,"--approval")){
                if(!strcmp(v,"full")){a.yes=true;a.auto_edit=false;}
                else if(!strcmp(v,"edit")){a.yes=false;a.auto_edit=true;}
                else if(!strcmp(v,"ask")){a.yes=false;a.auto_edit=false;}
                else{
                    fputs(tr("--approval 只接受 ask、edit 或 full。\n"),stderr);
                    rc=2;
                    goto cleanup;
                }
            }
            else{
                char *end;
                long n=strtol(v,&end,10);
                if(end==v||*end||n<0||n>100){
                    rc=2;
                    goto cleanup;
                }
                a.max_steps=(int)n;
            }
        }
        else if((arg[0]=='-'&&strcmp(arg,"-"))||task){
            fputs(tr("未知参数，请运行 desk --help。\n"),stderr);
            rc=2;
            goto cleanup;
        }
        else task=arg;
    }
    if(task&&!strcmp(task,"code"))task=nullptr;
    if(task&&!strcmp(task,"-")){
        char chunk[8192];
        size_t count;
        while((count=fread(chunk,1,sizeof chunk,stdin))>0){
            stdin_task.append(chunk,count);
            if(stdin_task.size()>1048576)break;
        }
        while(!stdin_task.empty()&&isspace(static_cast<unsigned char>(stdin_task.back())))stdin_task.pop_back();
        if(stdin_task.empty()||stdin_task.size()>1048576||!valid_utf8(stdin_task)){
            fputs(tr("标准输入没有可用的需求文本（须为 1 MB 以内的 UTF-8）。\n"),stderr);
            rc=2;
            goto cleanup;
        }
        task=stdin_task.c_str();
    }
    if(!canonical_directory(workspace,a.root)){
        fputs(tr("工作目录不存在。\n"),stderr);
        rc=1;
        goto cleanup;
    }
    if(store_load(&a)){
        fprintf(stderr,"%s\n",a.error);
        rc=1;
        goto cleanup;
    }
    reset_session(&a);
    if(profile){
        J *c=get(get(a.store,"profiles"),profile);
        if(!c){
            fputs(tr("模型配置不存在。\n"),stderr);
            rc=1;
            goto cleanup;
        }
        json_object_put(a.config);
        a.config=copy(c);
    }
    if(task&&!strcmp(task,"doctor")){
        rc=doctor(a,json_output);
        goto cleanup;
    }
    if(task&&!strcmp(task,"acp")){
        rc=acp_main(a);
        goto cleanup;
    }
    if(task&&!strcmp(task,"serve")){
        rc=serve_main(a,static_cast<int>(sub_argv.size()),sub_argv.data(),0);
        goto cleanup;
    }
    if(task&&!strcmp(task,"attach")){
        if(json_output)sub_argv.push_back(const_cast<char *>("--json"));
        rc=attach_main(a,static_cast<int>(sub_argv.size()),sub_argv.data(),0);
        goto cleanup;
    }
    if(task&&!strcmp(task,"config")){
        Tasks jobs;
        rc=configure(&a,jobs,sget(a.config,"profile"),NULL,false)?1:0;
        if(rc)fprintf(stderr,"%s\n",a.error);
        goto cleanup;
    }
    if(task&&!strcmp(task,"inspect")){
        J *snapshot=desktop_snapshot(&a,app);
        puts(json_object_to_json_string_ext(snapshot,JSON_C_TO_STRING_PRETTY));
        rc=get(snapshot,"error")?1:0;
        json_object_put(snapshot);
        goto cleanup;
    }
    if(resume&&!task){
        if(!isatty(STDIN_FILENO)){
            fputs(tr("--continue 需要交互终端。\n"),stderr);
            rc=2;
            goto cleanup;
        }
        rc=menu(&a,2);
        goto cleanup;
    }
    if(!task&&isatty(STDIN_FILENO)){
        rc=menu(&a);
        goto cleanup;
    }
    if(task&&strcmp(task,"chat")){
        rc=run_once(a,task,json_output);
        goto cleanup;
    }
    if(config_valid(&a,a.config)){
        fprintf(stderr,tr("%s 请运行 desk，在菜单中添加模型。\n"),a.error);
        rc=1;
        goto cleanup;
    }
    if(task)rc=menu(&a,1);
    else {Tasks jobs;chat(&a,jobs);}
    cleanup:
    return input_closed?0:interrupted==SIGTERM?143:interrupted==SIGINT?130:rc;
}
int desk_main(int argc,char **argv){
    platform_initialize();
    if(!language_init(argc,argv)){fputs(tr("语言必须为 zh 或 en。\n"),stderr);return 2;}
    if(curl_global_init(CURL_GLOBAL_DEFAULT)!=CURLE_OK){
        fputs(tr("无法初始化网络组件。\n"),stderr);
        return 1;
    }
    int result=1;
    try { result=run_cli(argc,argv); }
    catch(const std::bad_alloc &) { fputs(tr("内存不足，本次任务已停止。\n"),stderr); }
    catch(const std::exception &) { fputs(tr("程序发生内部错误，本次任务已停止。\n"),stderr); }
    curl_global_cleanup();
    return result;
}

#ifdef _WIN32
int wmain(int argc,wchar_t **argv){
    std::vector<std::string> arguments;arguments.reserve(static_cast<size_t>(argc));
    for(int i=0;i<argc;i++)arguments.push_back(utf8(argv[i]));
    std::vector<char *> pointers;for(auto &arg:arguments)pointers.push_back(arg.data());pointers.push_back(nullptr);
    return desk_main(argc,pointers.data());
}
#else
int main(int argc,char **argv){return desk_main(argc,argv);}
#endif
