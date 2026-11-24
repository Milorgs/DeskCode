// Skills (SKILL.md, compatible with Claude Code and the Agent Skills layout)
// and plugins (Claude Code plugin layout: commands, skills, hooks, .mcp.json).
#include "desk.hpp"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#ifdef _WIN32
#include <process.h>
#else
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
constexpr size_t SKILL_FILE_LIMIT=262144;
constexpr size_t CATALOG_LIMIT=12000;

std::string trim(std::string_view text){
    size_t a=0,b=text.size();
    while(a<b&&isspace(static_cast<unsigned char>(text[a])))a++;
    while(b>a&&isspace(static_cast<unsigned char>(text[b-1])))b--;
    return std::string(text.substr(a,b-a));
}
std::string unquote(std::string value){
    if(value.size()>=2&&((value.front()=='"'&&value.back()=='"')||(value.front()=='\''&&value.back()=='\'')))
        value=value.substr(1,value.size()-2);
    return value;
}
std::string read_text(const std::string &path,size_t limit){
    if(path_link(fs_path(path)))return {};
    size_t length=0;
    std::unique_ptr<char,decltype(&free)> bytes(read_bytes(path.c_str(),limit,&length),free);
    if(!bytes||memchr(bytes.get(),0,length))return {};
    std::string text(bytes.get(),length);
    if(text.starts_with("\xEF\xBB\xBF"))text.erase(0,3);
    return valid_utf8(text)?text:std::string();
}
bool home_skills_enabled(){
    const char *off=env_value("DESK_NO_USER_SKILLS");
    return !(off&&*off&&strcmp(off,"0"));
}
std::string home_dir(){
#ifdef _WIN32
    const char *home=env_value("USERPROFILE");
#else
    const char *home=env_value("HOME");
#endif
    return home?home:"";
}
bool valid_skill_name(std::string_view name){
    if(name.empty()||name.size()>64)return false;
    return std::all_of(name.begin(),name.end(),[](char c){
        auto u=static_cast<unsigned char>(c);
        return isalnum(u)||c=='-'||c=='_'||c=='.'||c==':';
    });
}
bool valid_plugin_name(std::string_view name){
    if(name.empty()||name.size()>48||name[0]=='.')return false;
    return std::all_of(name.begin(),name.end(),[](char c){
        auto u=static_cast<unsigned char>(c);
        return isalnum(u)||c=='-'||c=='_'||c=='.';
    });
}
}

// Front matter between --- lines: "key: value", quoted values, and folded
// (>) or literal (|) blocks. body receives the text after it.
bool parse_frontmatter(std::string_view text,std::map<std::string,std::string> &fields,std::string_view &body){
    body=text;
    if(!text.starts_with("---"))return false;
    size_t pos=text.find('\n');
    if(pos==std::string_view::npos||trim(text.substr(0,pos))!="---")return false;
    pos++;
    std::string key;
    char block=0;
    while(pos<text.size()){
        size_t end=text.find('\n',pos);
        std::string_view raw=text.substr(pos,end==std::string_view::npos?std::string_view::npos:end-pos);
        pos=end==std::string_view::npos?text.size():end+1;
        if(!raw.empty()&&raw.back()=='\r')raw.remove_suffix(1);
        if(trim(raw)=="---"){body=text.substr(pos);return true;}
        const bool indented=!raw.empty()&&(raw[0]==' '||raw[0]=='\t');
        if(indented&&!key.empty()){
            std::string &value=fields[key];
            const std::string line=trim(raw);
            if(!value.empty())value+=block=='|'?'\n':' ';
            value+=line;
            continue;
        }
        auto colon=raw.find(':');
        if(colon==std::string_view::npos){key.clear();continue;}
        key=trim(raw.substr(0,colon));
        std::string value=trim(raw.substr(colon+1));
        block=0;
        if(value=="|"||value=="|-"||value==">"||value==">-"){block=value[0];value.clear();}
        fields[key]=unquote(value);
    }
    body=text;
    fields.clear();
    return false;
}

std::vector<PluginInfo> list_plugins(const App &app);
std::vector<PluginInfo> enabled_plugins(const App &app){
    auto all=list_plugins(app);
    std::erase_if(all,[](const PluginInfo &plugin){return !plugin.enabled;});
    return all;
}

std::vector<SkillInfo> list_skills(const App &app){
    std::vector<SkillInfo> out;
    std::set<std::string> seen;
    auto scan=[&](const std::string &dir,const std::string &scope,const std::string &prefix){
        std::error_code ec;
        if(dir.empty()||!fs::is_directory(fs_path(dir),ec)||path_link(fs_path(dir)))return;
        std::vector<fs::path> entries;
        for(fs::directory_iterator it(fs_path(dir),ec),end;it!=end&&!ec;it.increment(ec))
            if(!path_link(it->path())&&fs::is_directory(it->symlink_status(ec)))entries.push_back(it->path());
        std::sort(entries.begin(),entries.end());
        for(const auto &entry:entries){
            if(out.size()>=100)return;
            const std::string skill_file=path_text(entry/"SKILL.md");
            const std::string text=read_text(skill_file,SKILL_FILE_LIMIT);
            if(text.empty())continue;
            std::map<std::string,std::string> fields;
            std::string_view body;
            parse_frontmatter(text,fields,body);
            std::string name=fields.count("name")&&!fields["name"].empty()?fields["name"]:path_text(entry.filename());
            if(!prefix.empty())name=prefix+":"+name;
            if(!valid_skill_name(name)||!seen.insert(name).second)continue;
            std::string description=fields["description"];
            if(description.size()>1024)description.resize(1024);
            size_t used=0;
            description=display_utf8(description,used,true);
            for(char &c:description)if(c=='\n'||c=='\r'||c=='\t')c=' ';
            out.push_back({name,description,path_text(entry),scope});
        }
    };
    if(*app.root){
        for(const char *sub:{"/.deskcode/skills","/.claude/skills","/.agents/skills"})scan(std::string(app.root)+sub,"workspace","");
    }
    if(*app.config_dir)scan(std::string(app.config_dir)+"/skills","user","");
    if(home_skills_enabled()){
        const std::string home=home_dir();
        if(!home.empty()){
            scan(home+"/.claude/skills","user","");
            scan(home+"/.agents/skills","user","");
        }
    }
    for(const auto &plugin:enabled_plugins(app))scan(plugin.directory+"/skills","plugin",plugin.name);
    return out;
}

J *skill_tool_definition(const App &app){
    auto skills=list_skills(app);
    if(skills.empty())return nullptr;
    std::string description="Load a skill: packaged instructions (and bundled files) for a specific kind of task. "
        "When the request matches a skill below, call this first and follow the returned instructions. "
        "Use the optional file parameter to read a file bundled in the skill directory.\nAvailable skills:";
    for(const auto &skill:skills){
        std::string line="\n- "+skill.name+(skill.description.empty()?"":": "+skill.description);
        if(description.size()+line.size()>CATALOG_LIMIT){description+="\n- …";break;}
        description+=line;
    }
    JsonPtr tool(json_tokener_parse(R"({"type":"function","function":{"name":"skill","parameters":{"type":"object","properties":{"name":{"type":"string","description":"Exact skill name from the list."},"file":{"type":"string","description":"Optional path of a bundled file, relative to the skill directory."}},"required":["name"],"additionalProperties":false}}})"));
    put(get(tool.get(),"function"),"description",str(description.c_str()));
    return tool.release();
}

J *tool_skill(App *a,J *args){
    json_object_object_foreach(args,key,value){
        if((strcmp(key,"name")&&strcmp(key,"file"))||!json_object_is_type(value,json_type_string))
            return err(tr("工具参数缺失、多余或类型不正确。"));
    }
    const std::string name=sget(args,"name");
    auto skills=list_skills(*a);
    auto found=std::find_if(skills.begin(),skills.end(),[&](const SkillInfo &skill){return skill.name==name;});
    if(found==skills.end())return err(tr("没有这个技能；请使用工具说明中列出的名称。"));
    const fs::path dir=fs_path(found->directory);
    const std::string file=sget(args,"file");
    if(!file.empty()){
        const fs::path relative=fs_path(file);
        bool safe=!relative.is_absolute()&&file.find('\\')==std::string::npos;
        for(const auto &part:relative)if(part==".."||part.empty())safe=false;
        std::error_code ec;
        const fs::path target=dir/relative;
        if(safe){
            const auto base=fs::weakly_canonical(dir,ec),resolved=fs::weakly_canonical(target,ec);
            const std::string b=path_text(base),r=path_text(resolved);
            safe=!ec&&r.size()>b.size()&&r.starts_with(b)&&!path_link(target);
        }
        if(!safe)return err(tr("技能文件路径须为技能目录内的相对路径。"));
        const std::string text=read_text(path_text(target),SKILL_FILE_LIMIT);
        if(text.empty())return err(tr("无法读取技能文件（不存在、不是 UTF-8 文本或超过 256 KB）。"));
        J *r=obj();
        put(r,"name",str(name.c_str()));
        put(r,"file",str(file.c_str()));
        put(r,"content",str(text.c_str()));
        return r;
    }
    const std::string text=read_text(path_text(dir/"SKILL.md"),SKILL_FILE_LIMIT);
    std::map<std::string,std::string> fields;
    std::string_view body;
    parse_frontmatter(text,fields,body);
    J *r=obj(),*files=arr();
    put(r,"name",str(name.c_str()));
    put(r,"directory",str(found->directory.c_str()));
    put(r,"instructions",str(trim(body).c_str()));
    std::error_code ec;
    for(fs::recursive_directory_iterator it(dir,fs::directory_options::none,ec),end;it!=end&&!ec&&alen(files)<50;it.increment(ec)){
        if(it.depth()>3||path_link(it->path())){it.disable_recursion_pending();continue;}
        const std::string leaf=path_text(it->path().filename());
        if(leaf.starts_with(".")){it.disable_recursion_pending();continue;}
        if(!fs::is_regular_file(it->symlink_status(ec))||leaf=="SKILL.md")continue;
        std::string rel=path_text(it->path().lexically_relative(dir));
        std::replace(rel.begin(),rel.end(),'\\','/');
        add(files,str(rel.c_str()));
    }
    put(r,"files",files);
    return r;
}

bool expand_skill_command(const App &app,std::string_view input,std::string &expanded,std::string &label){
    if(input.size()<2||input.front()!='/')return false;
    size_t space=1;
    while(space<input.size()&&!isspace(static_cast<unsigned char>(input[space])))space++;
    const std::string name(input.substr(1,space-1));
    if(!valid_skill_name(name))return false;
    for(const auto &skill:list_skills(app)){
        if(skill.name!=name)continue;
        const std::string text=read_text(skill.directory+"/SKILL.md",SKILL_FILE_LIMIT);
        std::map<std::string,std::string> fields;
        std::string_view body;
        parse_frontmatter(text,fields,body);
        const std::string request=trim(input.substr(space));
        expanded="Use the \""+name+"\" skill. Its instructions follow; bundled files are in "+skill.directory+
            " and can be read with the skill tool's file parameter.\n\n"+trim(body);
        if(!request.empty())expanded+="\n\nRequest: "+request;
        label=std::string(input);
        return true;
    }
    return false;
}

namespace {
std::string plugins_root(const App &app){return std::string(app.config_dir)+"/plugins";}
std::string registry_path(const App &app){return std::string(app.config_dir)+"/plugins.json";}
JsonPtr read_registry(const App &app){
    const std::string text=read_text(registry_path(app),262144);
    JsonPtr document(text.empty()?nullptr:json_tokener_parse(text.c_str()));
    if(!json_object_is_type(get(document.get(),"plugins"),json_type_object)){
        document.reset(obj());
        put(document.get(),"plugins",obj());
    }
    return document;
}
bool write_registry(App &app,J *document){
    if(mkdirs(app.config_dir,0700))return false;
    const char *text=json_object_to_json_string_ext(document,JSON_C_TO_STRING_PRETTY);
    return !atomic_file(registry_path(app).c_str(),text,strlen(text),0600);
}
// The manifest of a plugin directory, or "" when it has none.
std::string manifest_path(const std::string &dir){
    for(const char *leaf:{"/.deskcode-plugin/plugin.json","/.claude-plugin/plugin.json","/plugin.json"}){
        const std::string path=dir+leaf;
        std::error_code ec;
        if(fs::is_regular_file(fs_path(path),ec))return path;
    }
    return {};
}
JsonPtr read_manifest(const std::string &dir){
    const std::string path=manifest_path(dir);
    const std::string text=path.empty()?std::string():read_text(path,262144);
    JsonPtr manifest(text.empty()?nullptr:json_tokener_parse(text.c_str()));
    if(!json_object_is_type(manifest.get(),json_type_object))manifest.reset(obj());
    return manifest;
}
bool looks_like_plugin(const std::string &dir){
    if(!manifest_path(dir).empty())return true;
    std::error_code ec;
    for(const char *sub:{"/commands","/skills","/hooks"})if(fs::is_directory(fs_path(dir+sub),ec))return true;
    return fs::is_regular_file(fs_path(dir+"/.mcp.json"),ec);
}
// Replace ${CLAUDE_PLUGIN_ROOT} / ${DESK_PLUGIN_ROOT} in every string.
J *with_root(J *value,const std::string &root){
    if(json_object_is_type(value,json_type_string)){
        std::string text=json_object_get_string(value);
        for(const char *token:{"${CLAUDE_PLUGIN_ROOT}","${DESK_PLUGIN_ROOT}"}){
            const std::string needle=token;
            for(size_t pos=0;(pos=text.find(needle,pos))!=std::string::npos;pos+=root.size())text.replace(pos,needle.size(),root);
        }
        return str(text.c_str());
    }
    if(json_object_is_type(value,json_type_array)){
        J *out=arr();
        for(size_t i=0;i<alen(value);i++)add(out,with_root(at(value,i),root));
        return out;
    }
    if(json_object_is_type(value,json_type_object)){
        J *out=obj();
        json_object_object_foreach(value,key,item)put(out,key,with_root(item,root));
        return out;
    }
    return copy(value);
}
int run_program(const std::vector<std::string> &arguments){
#ifdef _WIN32
    std::vector<std::wstring> wide_args;
    for(const auto &argument:arguments)wide_args.push_back(wide(argument));
    std::vector<const wchar_t *> argv;
    for(auto &argument:wide_args)argv.push_back(argument.c_str());
    argv.push_back(nullptr);
    return static_cast<int>(_wspawnvp(_P_WAIT,argv[0],argv.data()));
#else
    pid_t child=fork();
    if(child<0)return -1;
    if(child==0){
        std::vector<char *> argv;
        for(const auto &argument:arguments)argv.push_back(const_cast<char *>(argument.c_str()));
        argv.push_back(nullptr);
        dup2(STDERR_FILENO,STDOUT_FILENO);
        execvp(argv[0],argv.data());
        _exit(127);
    }
    int status=0;
    while(waitpid(child,&status,0)<0)if(errno!=EINTR)return -1;
    return WIFEXITED(status)?WEXITSTATUS(status):-1;
#endif
}
bool copy_tree(const fs::path &from,const fs::path &to){
    std::error_code ec;
    fs::create_directories(to,ec);
    if(ec)return false;
    for(fs::recursive_directory_iterator it(from,fs::directory_options::none,ec),end;it!=end;it.increment(ec)){
        if(ec)return false;
        const std::string leaf=path_text(it->path().filename());
        if(leaf==".git"){it.disable_recursion_pending();continue;}
        // Links could point anywhere on the machine; plugins are copied without them.
        if(path_link(it->path())){it.disable_recursion_pending();continue;}
        const fs::path target=to/it->path().lexically_relative(from);
        auto status=it->symlink_status(ec);
        if(fs::is_directory(status))fs::create_directories(target,ec);
        else if(fs::is_regular_file(status))fs::copy_file(it->path(),target,fs::copy_options::overwrite_existing,ec);
        if(ec)return false;
    }
    return true;
}
struct Counts {size_t commands=0,skills=0,mcp=0,agents=0;bool hooks=false;};
Counts count_components(const std::string &dir){
    Counts counts;
    std::error_code ec;
    auto count_in=[&](const std::string &sub,bool dirs,const char *ext){
        size_t n=0;
        if(!fs::is_directory(fs_path(dir+sub),ec))return n;
        for(fs::directory_iterator it(fs_path(dir+sub),ec),end;it!=end&&!ec;it.increment(ec)){
            if(dirs?fs::is_directory(it->symlink_status(ec)):path_text(it->path().extension())==ext)n++;
        }
        return n;
    };
    counts.commands=count_in("/commands",false,".md");
    counts.skills=count_in("/skills",true,nullptr);
    counts.agents=count_in("/agents",false,".md");
    counts.hooks=fs::is_regular_file(fs_path(dir+"/hooks/hooks.json"),ec);
    const std::string mcp_text=read_text(dir+"/.mcp.json",262144);
    JsonPtr mcp(mcp_text.empty()?nullptr:json_tokener_parse(mcp_text.c_str()));
    J *servers=get(mcp.get(),"mcpServers")?get(mcp.get(),"mcpServers"):mcp.get();
    if(json_object_is_type(servers,json_type_object))counts.mcp+=static_cast<size_t>(json_object_object_length(servers));
    JsonPtr manifest=read_manifest(dir);
    if(json_object_is_type(get(manifest.get(),"mcpServers"),json_type_object))
        counts.mcp+=static_cast<size_t>(json_object_object_length(get(manifest.get(),"mcpServers")));
    return counts;
}
}

std::vector<PluginInfo> list_plugins(const App &app){
    std::vector<PluginInfo> out;
    if(!*app.config_dir)return out;
    JsonPtr registry=read_registry(app);
    json_object_object_foreach(get(registry.get(),"plugins"),name,entry){
        const std::string dir=plugins_root(app)+"/"+name;
        std::error_code ec;
        if(!valid_plugin_name(name)||!fs::is_directory(fs_path(dir),ec)||path_link(fs_path(dir)))continue;
        JsonPtr manifest=read_manifest(dir);
        PluginInfo info;
        info.name=name;
        info.version=sget(manifest.get(),"version");
        info.description=sget(manifest.get(),"description");
        info.directory=dir;
        info.source=sget(entry,"source");
        info.enabled=!get(entry,"enabled")||bget(entry,"enabled");
        out.push_back(std::move(info));
    }
    std::sort(out.begin(),out.end(),[](const PluginInfo &a,const PluginInfo &b){return a.name<b.name;});
    return out;
}

J *plugin_mcp_servers(const App &app){
    J *servers=obj();
    for(const auto &plugin:enabled_plugins(app)){
        auto merge=[&](J *map){
            if(!json_object_is_type(map,json_type_object))return;
            json_object_object_foreach(map,name,entry){
                if(!json_object_is_type(entry,json_type_object))continue;
                const std::string qualified=plugin.name+":"+name;
                if(!get(servers,qualified.c_str()))put(servers,qualified.c_str(),with_root(entry,plugin.directory));
            }
        };
        const std::string text=read_text(plugin.directory+"/.mcp.json",262144);
        JsonPtr document(text.empty()?nullptr:json_tokener_parse(text.c_str()));
        merge(get(document.get(),"mcpServers")?get(document.get(),"mcpServers"):document.get());
        JsonPtr manifest=read_manifest(plugin.directory);
        merge(get(manifest.get(),"mcpServers"));
    }
    return servers;
}

namespace {
bool local_directory(const std::string &source){
    std::error_code ec;
    return fs::is_directory(fs_path(source),ec);
}
std::string git_url(const std::string &source){
    if(source.find("://")!=std::string::npos||source.starts_with("git@"))return source;
    // owner/repo shorthand for GitHub.
    auto slash=source.find('/');
    if(slash!=std::string::npos&&slash>0&&source.find('/',slash+1)==std::string::npos&&
       std::all_of(source.begin(),source.end(),[](char c){return isalnum(static_cast<unsigned char>(c))||c=='/'||c=='-'||c=='_'||c=='.';}))
        return "https://github.com/"+source+".git";
    return {};
}
struct Staging {
    std::string path;
    ~Staging(){if(!path.empty()){std::error_code ec;fs::remove_all(fs_path(path),ec);}}
};
int install_from(App &app,const std::string &source,const std::string &wanted,int depth);
// A directory holding one plugin, or a marketplace listing several.
int install_directory(App &app,const std::string &dir,const std::string &source,const std::string &wanted,int depth){
    if(!looks_like_plugin(dir)){
        const std::string text=read_text(dir+"/.claude-plugin/marketplace.json",524288);
        JsonPtr market(text.empty()?nullptr:json_tokener_parse(text.c_str()));
        J *plugins=get(market.get(),"plugins");
        if(!alen(plugins)){fprintf(stderr,"%s\n",tr("没有找到插件清单（.claude-plugin/plugin.json）或插件市场清单。"));return 1;}
        J *chosen=nullptr;
        for(size_t i=0;i<alen(plugins);i++)if(wanted==sget(at(plugins,i),"name"))chosen=at(plugins,i);
        if(!chosen&&wanted.empty()&&alen(plugins)==1)chosen=at(plugins,0);
        if(!chosen){
            fprintf(stderr,"%s\n",tr("这是插件市场，请指定要安装的插件名称：desk plugin install <来源> <插件名>"));
            for(size_t i=0;i<alen(plugins);i++)fprintf(stderr,"  %s  %s\n",sget(at(plugins,i),"name"),sget(at(plugins,i),"description"));
            return 1;
        }
        J *from=get(chosen,"source");
        const std::string name=sget(chosen,"name");
        if(json_object_is_type(from,json_type_string)){
            std::string rel=json_object_get_string(from);
            if(rel.find("..")!=std::string::npos||fs_path(rel).is_absolute()){fprintf(stderr,"%s\n",tr("插件市场中的路径无效。"));return 1;}
            const std::string root=sget(get(market.get(),"metadata"),"pluginRoot");
            if(!rel.starts_with("./")&&!root.empty()&&root.find("..")==std::string::npos)rel=root+"/"+rel;
            return install_directory(app,dir+"/"+rel,source,name,depth+1);
        }
        if(depth<2&&json_object_is_type(from,json_type_object)){
            std::string next=*sget(from,"repo")?sget(from,"repo"):sget(from,"url");
            if(!next.empty())return install_from(app,next,name,depth+1);
        }
        fprintf(stderr,"%s\n",tr("不支持此插件来源，请直接安装该插件所在的仓库或目录。"));
        return 1;
    }
    JsonPtr manifest=read_manifest(dir);
    std::string name=*sget(manifest.get(),"name")?sget(manifest.get(),"name"):wanted;
    if(name.empty())name=path_text(fs_path(dir).filename());
    if(!valid_plugin_name(name)){fprintf(stderr,"%s\n",tr("插件名称无效：只能包含字母、数字、-、_ 与 .，最多 48 个字符。"));return 1;}
    const fs::path target=fs_path(plugins_root(app)+"/"+name);
    const fs::path staged=fs_path(plugins_root(app)+"/."+name+".new");
    std::error_code ec;
    fs::remove_all(staged,ec);
    if(mkdirs(plugins_root(app).c_str(),0700)||!copy_tree(fs_path(dir),staged)){
        fs::remove_all(staged,ec);
        fprintf(stderr,"%s\n",tr("无法复制插件文件。"));
        return 1;
    }
    fs::remove_all(target,ec);
    fs::rename(staged,target,ec);
    if(ec){fs::remove_all(staged,ec);fprintf(stderr,"%s\n",tr("无法安装插件。"));return 1;}
    JsonPtr registry=read_registry(app);
    J *entry=obj();
    put(entry,"enabled",json_object_new_boolean(1));
    put(entry,"source",str(source.c_str()));
    put(get(registry.get(),"plugins"),name.c_str(),entry);
    if(!write_registry(app,registry.get())){fprintf(stderr,"%s\n",tr("无法保存插件列表。"));return 1;}
    const Counts counts=count_components(path_text(target));
    printf(tr("已安装插件 %s%s%s：%zu 个指令，%zu 个技能，%zu 个 MCP 服务%s。\n"),name.c_str(),
           *sget(manifest.get(),"version")?" ":"",sget(manifest.get(),"version"),counts.commands,counts.skills,counts.mcp,
           counts.hooks?tr("，含钩子"):"");
    if(counts.agents)printf(tr("提示：插件中的 %zu 个子代理定义（agents/）暂不支持，已忽略。\n"),counts.agents);
    return 0;
}
int install_from(App &app,const std::string &source,const std::string &wanted,int depth){
    if(local_directory(source)){
        std::error_code ec;
        const std::string absolute=path_text(fs::absolute(fs_path(source),ec));
        return install_directory(app,absolute,absolute,wanted,depth);
    }
    const std::string url=git_url(source);
    if(url.empty()){fprintf(stderr,"%s\n",tr("插件来源须为本地目录、Git 地址或 GitHub 的 owner/repo。"));return 1;}
    char id[33];id_string(id);
    Staging staging{plugins_root(app)+"/.clone-"+std::string(id,12)};
    if(mkdirs(plugins_root(app).c_str(),0700)){fprintf(stderr,"%s\n",tr("无法创建插件目录。"));return 1;}
    if(run_program({"git","clone","--depth","1","--quiet","--",url,staging.path})!=0){
        fprintf(stderr,"%s\n",tr("git clone 失败，请检查地址、网络与 git 是否已安装。"));
        return 1;
    }
    return install_directory(app,staging.path,url,wanted,depth);
}
}

int plugin_cli(App &app,int argc,char **argv,int first){
    const std::string action=first<argc?argv[first]:"list";
    const std::string target=first+1<argc?argv[first+1]:"";
    if(action=="install"||action=="add"||action=="update"){
        if(target.empty()){fputs(tr("用法：desk plugin install <目录 | Git 地址 | owner/repo> [插件名]\n"),stderr);return 2;}
        std::string source=target;
        if(action=="update"){
            for(const auto &plugin:list_plugins(app))if(plugin.name==target)source=plugin.source;
        }
        return install_from(app,source,first+2<argc?argv[first+2]:"",0);
    }
    if(action=="list"||action=="ls"){
        auto plugins=list_plugins(app);
        if(plugins.empty()){puts(tr("尚未安装插件。使用 desk plugin install <目录 | Git 地址 | owner/repo> 安装。"));return 0;}
        for(const auto &plugin:plugins){
            const Counts counts=count_components(plugin.directory);
            printf("%s %s%s%s · %s · %zu cmd · %zu skill · %zu mcp%s\n",plugin.enabled?"●":"○",plugin.name.c_str(),
                   plugin.version.empty()?"":" ",plugin.version.c_str(),tr(plugin.enabled?"已启用":"已停用"),
                   counts.commands,counts.skills,counts.mcp,counts.hooks?" · hooks":"");
            if(!plugin.description.empty())printf("    %s\n",plugin.description.c_str());
        }
        return 0;
    }
    if(action=="remove"||action=="uninstall"||action=="enable"||action=="disable"){
        JsonPtr registry=read_registry(app);
        J *plugins=get(registry.get(),"plugins");
        if(target.empty()||!get(plugins,target.c_str())){fprintf(stderr,"%s\n",tr("没有安装这个插件。"));return 1;}
        if(action=="remove"||action=="uninstall"){
            json_object_object_del(plugins,target.c_str());
            std::error_code ec;
            if(valid_plugin_name(target))fs::remove_all(fs_path(plugins_root(app)+"/"+target),ec);
        }else put(get(plugins,target.c_str()),"enabled",json_object_new_boolean(action=="enable"));
        if(!write_registry(app,registry.get())){fprintf(stderr,"%s\n",tr("无法保存插件列表。"));return 1;}
        printf("%s %s\n",tr(action=="enable"?"已启用插件":action=="disable"?"已停用插件":"已移除插件"),target.c_str());
        return 0;
    }
    if(action=="validate"){
        const std::string dir=target.empty()?".":target;
        if(!local_directory(dir)||!looks_like_plugin(dir)){fprintf(stderr,"%s\n",tr("不是有效的插件目录。"));return 1;}
        JsonPtr manifest=read_manifest(dir);
        const std::string name=*sget(manifest.get(),"name")?sget(manifest.get(),"name"):path_text(fs_path(dir).filename());
        const Counts counts=count_components(dir);
        printf("%s %s: %zu cmd · %zu skill · %zu mcp%s%s\n",valid_plugin_name(name)?"✓":"✗",name.c_str(),counts.commands,counts.skills,
               counts.mcp,counts.hooks?" · hooks":"",counts.agents?" · agents (ignored)":"");
        return valid_plugin_name(name)?0:1;
    }
    fputs(tr("用法：desk plugin [list | install <来源> [插件名] | update <名称> | enable <名称> | disable <名称> | remove <名称> | validate [目录]]\n"),stderr);
    return 2;
}
