#include "desk.hpp"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
static bool desktop_match_ci(const char *haystack,const char *needle){
    if(!needle||!*needle)return true;
    if(!haystack||!*haystack)return false;
    size_t nlen=strlen(needle);
    for(const char *p=haystack;*p;p++){
        size_t i=0;
        while(i<nlen&&p[i]&&tolower((unsigned char)p[i])==tolower((unsigned char)needle[i]))i++;
        if(i==nlen)return true;
    }
    return false;
}
#ifdef WITH_ATSPI
#include <atspi/atspi.h>
typedef struct{
    AtspiAccessible *nodes[500];
    char *names[500];
    AtspiRole roles[500];
    int count;
    char stamp[33];
    double created;
    bool ready;
}
Desktop;
static Desktop *desktop(App *a){
    if(!a->desktop_data)a->desktop_data=calloc(1,sizeof(Desktop));
    return static_cast<Desktop *>(a->desktop_data);
}
void desktop_clear(App *a){
    Desktop *d=static_cast<Desktop *>(a->desktop_data);
    if(!d)return;
    for(int i=0;i<d->count;i++){
        g_object_unref(d->nodes[i]);
        g_free(d->names[i]);
    }
    d->count=0;
    d->created=0;
}
static void visit(Desktop *d,AtspiAccessible *node,int depth,J *nodes,const char *filter){
    if(!node||depth>16||d->count>=500||monotime()-d->created>8||interrupted)return;
    AtspiRole role=atspi_accessible_get_role(node,NULL);
    if(role==ATSPI_ROLE_PASSWORD_TEXT)return;
    char *name=atspi_accessible_get_name(node,NULL),*role_name=atspi_accessible_get_role_name(node,NULL);
    bool matched=!filter||!*filter||desktop_match_ci(name,filter)||desktop_match_ci(role_name,filter);
    if(matched){
        int index=d->count++;
        d->nodes[index]=g_object_ref(node);
        d->names[index]=g_strdup(name?name:"");
        d->roles[index]=role;
        char ref[64];
        snprintf(ref,sizeof ref,"%s:%d",d->stamp,index+1);
        J *item=obj(),*actions=arr();
        put(item,"ref",str(ref));
        put(item,"name",str(name));
        put(item,"role",str(role_name));
        put(item,"depth",json_object_new_int(depth));
        put(item,"actions",actions);
        AtspiStateSet *states=atspi_accessible_get_state_set(node);
        put(item,"enabled",json_object_new_boolean(states&&atspi_state_set_contains(states,ATSPI_STATE_ENABLED)));
        put(item,"editable",json_object_new_boolean(states&&atspi_state_set_contains(states,ATSPI_STATE_EDITABLE)));
        if(states)g_object_unref(states);
        AtspiAction *action=atspi_accessible_get_action_iface(node);
        if(action){
            int n=atspi_action_get_n_actions(action,NULL);
            for(int i=0;i<n&&i<16;i++){
                char *value=atspi_action_get_action_name(action,i,NULL);
                add(actions,str(value));
                g_free(value);
            }
            g_object_unref(action);
        }
        AtspiText *text=atspi_accessible_get_text_iface(node);
        if(text){
            int n=atspi_text_get_character_count(text,NULL);
            if(n>1200)n=1200;
            if(n>0){
                char *value=atspi_text_get_text(text,0,n,NULL);
                put(item,"text",str(value));
                g_free(value);
            }
            g_object_unref(text);
        }
        add(nodes,item);
    }
    g_free(name);
    g_free(role_name);
    int count=atspi_accessible_get_child_count(node,NULL);
    for(int i=0;i<count&&i<500&&d->count<500;i++){
        AtspiAccessible *child=atspi_accessible_get_child_at_index(node,i,NULL);
        if(child){
            visit(d,child,depth+1,nodes,filter);
            g_object_unref(child);
        }
    }
}
J *desktop_snapshot(App *a,const char *app,const char *filter){
    Desktop *d=desktop(a);
    if(!d)return err(tr("内存不足。"));
    desktop_clear(a);
    if(!d->ready){
        if(atspi_init()!=0)return err(tr("AT-SPI 无法初始化，请确认正在图形会话中运行。"));
        atspi_set_timeout(1500,3000);
        d->ready=true;
    }
    id_string(d->stamp);
    d->created=monotime();
    AtspiAccessible *root=atspi_get_desktop(0);
    if(!root)return err(tr("无法读取桌面无障碍服务。"));
    J *r=obj(),*apps=arr(),*nodes=arr();
    put(r,"apps",apps);
    put(r,"nodes",nodes);
    put(r,"app",str(app));
    int n=atspi_accessible_get_child_count(root,NULL);
    for(int i=0;i<n&&i<80&&!interrupted;i++){
        AtspiAccessible *application=atspi_accessible_get_child_at_index(root,i,NULL);
        if(!application)continue;
        char *name=atspi_accessible_get_name(application,NULL);
        add(apps,str(name));
        if(*app&&name&&!g_utf8_collate(app,name))visit(d,application,0,nodes,filter);
        g_free(name);
        g_object_unref(application);
    }
    g_object_unref(root);
    put(r,"partial",json_object_new_boolean(d->count>=500||monotime()-d->created>=8));
    put(r,"hint",str(tr("按完整应用名读取控件。每次操作后重新 snapshot 验证。")));
    return r;
}
J *desktop_act(App *a,const char *ref,const char *action,const char *text){
    Desktop *d=desktop(a);
    if(!d||monotime()-d->created>30||strncmp(ref,d->stamp,32)||ref[32]!=':')return err(tr("控件引用无效或过期，请重新读取桌面。"));
    char *end=NULL;
    long number=strtol(ref+33,&end,10);
    if(!end||*end||number<1||number>d->count)return err(tr("控件引用无效。"));
    int i=(int)number-1;
    AtspiAccessible *node=g_object_ref(d->nodes[i]);
    char *name=atspi_accessible_get_name(node,NULL);
    bool same=name&&!strcmp(name,d->names[i])&&atspi_accessible_get_role(node,NULL)==d->roles[i];
    g_free(name);
    desktop_clear(a);
    if(!same){
        g_object_unref(node);
        return err(tr("控件已变化。"));
    }
    AtspiStateSet *states=atspi_accessible_get_state_set(node);
    bool enabled=states&&atspi_state_set_contains(states,ATSPI_STATE_ENABLED);
    bool editable=states&&atspi_state_set_contains(states,ATSPI_STATE_EDITABLE);
    if(states)g_object_unref(states);
    bool ok=false;
    if(enabled&&text&&editable&&atspi_accessible_get_role(node,NULL)!=ATSPI_ROLE_PASSWORD_TEXT){
        AtspiEditableText *t=atspi_accessible_get_editable_text_iface(node);
        if(t){
            ok=atspi_editable_text_set_text_contents(t,text,NULL);
            g_object_unref(t);
        }
    }
    else if(enabled&&action){
        AtspiAction *act=atspi_accessible_get_action_iface(node);
        if(act){
            int n=atspi_action_get_n_actions(act,NULL);
            for(int k=0;k<n;k++){
                char *v=atspi_action_get_action_name(act,k,NULL);
                if(v&&!strcmp(v,action))ok=atspi_action_do_action(act,k,NULL);
                g_free(v);
                if(ok)break;
            }
            g_object_unref(act);
        }
    }
    g_object_unref(node);
    J *r=obj();
    put(r,"accepted",json_object_new_boolean(ok));
    put(r,"verified",json_object_new_boolean(0));
    put(r,"next",str(tr("重新读取桌面验证结果。")));
    return r;
}
bool desktop_compiled(void){
    return true;
}
#elif defined(__APPLE__)
#include <vector>
#include <string>
namespace {
struct MacRef {
    std::string app,role,name;
    int index=1;
};
struct MacDesktop {
    std::vector<MacRef> refs;
    std::string stamp;
    double created=0;
};
MacDesktop *mac_state(App *a){
    if(!a->desktop_data)a->desktop_data=new MacDesktop();
    return static_cast<MacDesktop *>(a->desktop_data);
}
std::string applescript_escape(const char *s){
    std::string out;
    for(;s&&*s;s++){
        if(*s=='"'||*s=='\\')out+='\\';
        out+=*s;
    }
    return out;
}
}
void desktop_clear(App *a){
    if(auto *d=static_cast<MacDesktop *>(a->desktop_data)){d->refs.clear();d->created=0;}
}
bool desktop_compiled(void){return true;}
J *desktop_snapshot(App *a,const char *app_name,const char *filter){
    MacDesktop *d=mac_state(a);
    desktop_clear(a);
    char stamp[33];id_string(stamp);d->stamp=stamp;d->created=monotime();
    App probe{};snprintf(probe.root,sizeof probe.root,"%s",a->root);probe.sandbox=false;
    J *r=obj(),*apps=arr(),*nodes=arr();
    put(r,"apps",apps);put(r,"nodes",nodes);put(r,"app",str(app_name));
    JsonPtr list_res(run_command(&probe,"osascript -e 'tell application \"System Events\" to get name of every application process whose background only is false'",6000));
    if(list_res&&iget(list_res.get(),"exit_code",-1)==0){
        std::string_view out=sget(list_res.get(),"output");
        for(size_t pos=0;pos<out.size();){
            auto comma=out.find(',',pos);
            std::string_view item=comma==std::string_view::npos?out.substr(pos):out.substr(pos,comma-pos);
            pos=comma==std::string_view::npos?out.size():comma+1;
            while(!item.empty()&&(item.front()==' '||item.front()=='\n'||item.front()=='\r'))item.remove_prefix(1);
            while(!item.empty()&&(item.back()==' '||item.back()=='\n'||item.back()=='\r'))item.remove_suffix(1);
            if(!item.empty())add(apps,json_object_new_string_len(item.data(),static_cast<int>(item.size())));
        }
    }
    if(app_name&&*app_name){
        std::string esc=applescript_escape(app_name);
        std::string cmd="osascript -e 'tell application \"System Events\" to tell process \""+esc+"\" to get {role, name} of entire contents of front window' 2>/dev/null";
        JsonPtr ui_res(run_command(&probe,cmd.c_str(),8000));
        if(ui_res&&iget(ui_res.get(),"exit_code",-1)==0){
            std::string_view text=sget(ui_res.get(),"output");
            int idx=0;
            for(size_t pos=0;pos<text.size()&&d->refs.size()<500;){
                auto nl=text.find(',',pos);
                std::string part(nl==std::string_view::npos?text.substr(pos):text.substr(pos,nl-pos));
                pos=nl==std::string_view::npos?text.size():nl+1;
                while(!part.empty()&&(part.front()==' '||part.front()=='\n'))part.erase(part.begin());
                while(!part.empty()&&(part.back()==' '||part.back()=='\n'||part.back()=='\r'))part.pop_back();
                if(part.empty())continue;
                if(!filter||!*filter||desktop_match_ci(part.c_str(),filter)){
                    d->refs.push_back({app_name,"control",part,++idx});
                    std::string ref=d->stamp+":"+std::to_string(d->refs.size());
                    J *node=obj(),*acts=arr();
                    add(acts,str("click"));
                    put(node,"ref",str(ref.c_str()));
                    put(node,"name",str(part.c_str()));
                    put(node,"role",str("control"));
                    put(node,"enabled",json_object_new_boolean(1));
                    put(node,"actions",acts);
                    add(nodes,node);
                }
            }
        }
    }
    put(r,"partial",json_object_new_boolean(d->refs.size()>=500));
    put(r,"hint",str(tr("按完整应用名读取控件。每次操作后重新 snapshot 验证。")));
    return r;
}
J *desktop_act(App *a,const char *ref,const char *action,const char *text){
    auto *d=static_cast<MacDesktop *>(a->desktop_data);
    if(!d||monotime()-d->created>30)return err(tr("控件引用无效或过期，请重新读取桌面。"));
    std::string prefix=d->stamp+":";
    if(strncmp(ref,prefix.c_str(),prefix.size()))return err(tr("控件引用无效。"));
    char *end=nullptr;long number=strtol(ref+prefix.size(),&end,10);
    if(!end||*end||number<1||static_cast<size_t>(number)>d->refs.size())return err(tr("控件引用无效。"));
    MacRef target=d->refs[static_cast<size_t>(number-1)];
    desktop_clear(a);
    App probe{};snprintf(probe.root,sizeof probe.root,"%s",a->root);probe.sandbox=false;
    std::string app_esc=applescript_escape(target.app.c_str()),name_esc=applescript_escape(target.name.c_str());
    std::string cmd;
    if(text){
        std::string text_esc=applescript_escape(text);
        cmd="osascript -e 'tell application \"System Events\" to tell process \""+app_esc+"\" to set value of first UI element of front window whose name is \""+name_esc+"\" to \""+text_esc+"\"'";
    }else if(action&&!strcmp(action,"click")){
        cmd="osascript -e 'tell application \"System Events\" to tell process \""+app_esc+"\" to click first UI element of front window whose name is \""+name_esc+"\"'";
    }else return err(tr("控件引用无效。"));
    JsonPtr res(run_command(&probe,cmd.c_str(),6000));
    bool ok=res&&iget(res.get(),"exit_code",-1)==0;
    J *r=obj();
    put(r,"accepted",json_object_new_boolean(ok));
    put(r,"verified",json_object_new_boolean(0));
    put(r,"next",str(tr("重新读取桌面验证结果。")));
    return r;
}
#else
void desktop_clear(App *a){
    (void)a;
}
bool desktop_compiled(void){
    return false;
}
J *desktop_snapshot(App *a,const char *name,const char *filter){
    (void)a;
    (void)name;
    (void)filter;
    return err(tr("本次构建未启用 AT-SPI，请安装开发库后重新编译。"));
}
J *desktop_act(App *a,const char *r,const char *action,const char *text){
    (void)a;
    (void)r;
    (void)action;
    (void)text;
    return err(tr("本次构建未启用 AT-SPI。"));
}
#endif

void desktop_destroy(App *a){
    desktop_clear(a);
#if defined(__APPLE__) && !defined(WITH_ATSPI)
    delete static_cast<MacDesktop *>(a->desktop_data);
#else
    free(a->desktop_data);
#endif
    a->desktop_data=nullptr;
}
