#include "desk.hpp"
#include "tasks.hpp"
#include <curl/curl.h>
#include <openssl/evp.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <sys/stat.h>
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
constexpr size_t MAX_WEB_RAW=2*1024*1024;
constexpr size_t MAX_SCREENSHOT_BYTES=5*1024*1024;

std::string lower_str(std::string_view s){
    std::string out(s);
    for(char &c:out)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool valid_hex32_str(std::string_view s){
    return s.size()==32&&std::all_of(s.begin(),s.end(),[](char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');
    });
}

std::string undo_journal_path(const char *root){
    return std::string(root)+"/.desk-agent/undo.json";
}

JsonPtr read_undo_journal(const char *root){
    size_t len=0;
    std::string path=undo_journal_path(root);
    char *bytes=read_bytes(path.c_str(),524288,&len);
    if(!bytes)return {};
    JsonPtr doc(json_tokener_parse(bytes));
    free(bytes);
    if(!doc||!json_object_is_type(doc.get(),json_type_object))return {};
    return doc;
}

bool write_undo_journal(const char *root,J *doc){
    std::string dir=std::string(root)+"/.desk-agent";
    if(mkdirs(dir.c_str(),0700))return false;
    const char *text=json_object_to_json_string_ext(doc,JSON_C_TO_STRING_PLAIN);
    std::string path=dir+"/undo.json";
    return atomic_file(path.c_str(),text,strlen(text),0600)==0;
}

bool glob_simple(std::string_view pat,std::string_view str,bool ci){
    size_t pi=0,si=0,star=std::string_view::npos,match=0;
    auto eq=[ci](char a,char b){
        if(ci)return std::tolower(static_cast<unsigned char>(a))==std::tolower(static_cast<unsigned char>(b));
        return a==b;
    };
    while(si<str.size()){
        if(pi<pat.size()&&(pat[pi]=='?'||eq(pat[pi],str[si]))){pi++;si++;}
        else if(pi<pat.size()&&pat[pi]=='*'){star=pi++;match=si;}
        else if(star!=std::string_view::npos){pi=star+1;si=++match;}
        else return false;
    }
    while(pi<pat.size()&&pat[pi]=='*')pi++;
    return pi==pat.size();
}

std::string b64_encode(const void *data,size_t len){
    if(!len)return {};
    std::string out(((len+2)/3)*4+1,'\0');
    int n=EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                          reinterpret_cast<const unsigned char *>(data),
                          static_cast<int>(len));
    if(n<=0)return {};
    out.resize(static_cast<size_t>(n));
    return out;
}

#ifdef _WIN32
std::string b64_decode(std::string_view encoded){
    std::string clean;
    clean.reserve(encoded.size());
    for(char c:encoded)if(!std::isspace(static_cast<unsigned char>(c)))clean+=c;
    if(clean.empty()||clean.size()%4!=0)return {};
    std::string out((clean.size()/4)*3,'\0');
    int decoded=EVP_DecodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                                reinterpret_cast<const unsigned char *>(clean.data()),
                                static_cast<int>(clean.size()));
    if(decoded<0)return {};
    size_t pad=(clean.ends_with("==")?2:clean.ends_with("=")?1:0);
    if(static_cast<size_t>(decoded)<pad)return {};
    out.resize(static_cast<size_t>(decoded)-pad);
    return out;
}
#endif

bool private_ip_address(const sockaddr *addr){
    if(addr->sa_family==AF_INET){
        uint32_t ip=ntohl(reinterpret_cast<const sockaddr_in *>(addr)->sin_addr.s_addr);
        if((ip>>24)==0||(ip>>24)==10||(ip>>24)==127)return true;
        if((ip>>20)==((172u<<4)|1u))return true; // 172.16.0.0/12
        if((ip>>16)==((192u<<8)|168u))return true;
        if((ip>>16)==((169u<<8)|254u))return true;
        if((ip>>22)==((100u<<2)|1u))return true; // 100.64.0.0/10 CGNAT
        if((ip>>28)>=14)return true; // multicast/reserved
        return false;
    }
    if(addr->sa_family==AF_INET6){
        const auto *b=reinterpret_cast<const sockaddr_in6 *>(addr)->sin6_addr.s6_addr;
        bool all_zero=true;
        for(int i=0;i<15;i++)if(b[i]!=0)all_zero=false;
        if(all_zero&&(b[15]==0||b[15]==1))return true; // :: and ::1
        if((b[0]&0xfe)==0xfc)return true; // fc00::/7 ULA
        if(b[0]==0xfe&&(b[1]&0xc0)==0x80)return true; // fe80::/10 link-local
        if(b[0]==0xff)return true; // multicast
        // IPv4-mapped ::ffff:a.b.c.d
        bool v4_mapped=true;
        for(int i=0;i<10;i++)if(b[i]!=0)v4_mapped=false;
        if(v4_mapped&&b[10]==0xff&&b[11]==0xff){
            sockaddr_in v4{};
            v4.sin_family=AF_INET;
            memcpy(&v4.sin_addr,b+12,4);
            return private_ip_address(reinterpret_cast<const sockaddr *>(&v4));
        }
        return false;
    }
    return true;
}

bool validate_fetch_url(const char *raw_url,std::string &clean_url,std::string &resolve_entry,std::string &err_out){
    resolve_entry.clear();
    CURLU *u=curl_url();
    if(!u){err_out=tr("无法解析网页地址。");return false;}
    struct Guard {CURLU *h;~Guard(){curl_url_cleanup(h);}} guard{u};
    if(curl_url_set(u,CURLUPART_URL,raw_url,0)!=CURLUE_OK){err_out=tr("网页地址格式无效。");return false;}
    char *scheme=nullptr,*host=nullptr,*port=nullptr,*user=nullptr,*pass=nullptr,*normalized=nullptr;
    curl_url_get(u,CURLUPART_SCHEME,&scheme,0);
    curl_url_get(u,CURLUPART_HOST,&host,0);
    curl_url_get(u,CURLUPART_PORT,&port,0);
    bool has_cred=curl_url_get(u,CURLUPART_USER,&user,0)==CURLUE_OK
                ||curl_url_get(u,CURLUPART_PASSWORD,&pass,0)==CURLUE_OK;
    curl_url_set(u,CURLUPART_FRAGMENT,nullptr,0);
    curl_url_get(u,CURLUPART_URL,&normalized,0);
    std::string sc=scheme?lower_str(scheme):"";
    std::string ho=host?lower_str(host):"";
    std::string po=port?port:(sc=="https"?"443":"80");
    if(normalized)clean_url=normalized;
    curl_free(scheme);curl_free(host);curl_free(port);curl_free(user);curl_free(pass);curl_free(normalized);

    if(has_cred||ho.empty()||(sc!="https"&&sc!="http")){
        err_out=tr("仅支持不带凭据的 HTTPS 网页地址（本机调试可用 localhost HTTP）。");
        return false;
    }
    const bool explicit_local=(ho=="localhost"||ho=="127.0.0.1"||ho=="[::1]");
    if(sc=="http"&&!explicit_local){
        err_out=tr("外部网页须使用 HTTPS。");
        return false;
    }
    if(explicit_local)return true;

    std::string lookup_host=ho;
    if(lookup_host.size()>2&&lookup_host.front()=='['&&lookup_host.back()==']')
        lookup_host=lookup_host.substr(1,lookup_host.size()-2);
    addrinfo hints{};
    hints.ai_socktype=SOCK_STREAM;
    hints.ai_family=AF_UNSPEC;
    addrinfo *res=nullptr;
    if(getaddrinfo(lookup_host.c_str(),po.c_str(),&hints,&res)!=0||!res){
        err_out=tr("无法解析网页域名。");
        return false;
    }
    std::string first_ip;
    bool blocked=false;
    for(addrinfo *p=res;p;p=p->ai_next){
        if(private_ip_address(p->ai_addr)){blocked=true;break;}
        if(first_ip.empty()){
            char buf[INET6_ADDRSTRLEN]{};
            if(p->ai_family==AF_INET)
                inet_ntop(AF_INET,&reinterpret_cast<sockaddr_in *>(p->ai_addr)->sin_addr,buf,sizeof buf);
            else if(p->ai_family==AF_INET6)
                inet_ntop(AF_INET6,&reinterpret_cast<sockaddr_in6 *>(p->ai_addr)->sin6_addr,buf,sizeof buf);
            if(*buf)first_ip=p->ai_family==AF_INET6?(std::string("[")+buf+"]"):std::string(buf);
        }
    }
    freeaddrinfo(res);
    if(blocked||first_ip.empty()){
        err_out=tr("已拦截指向本机或内网私有地址的请求。");
        return false;
    }
    resolve_entry=lookup_host+":"+po+":"+first_ip;
    return true;
}

std::string html_to_text(std::string_view html){
    std::string out;
    out.reserve(std::min<size_t>(html.size(),65536));
    bool in_skip=false;
    std::string skip_tag;
    auto push_nl=[&](int count){
        int have=0;
        for(size_t k=out.size();k>0&&out[k-1]=='\n';--k)have++;
        while(have<count&&!out.empty()){out+='\n';have++;}
    };
    for(size_t i=0;i<html.size();){
        if(html[i]=='<'){
            if(html.substr(i,4)=="<!--"){
                auto end=html.find("-->",i+4);
                i=end==std::string_view::npos?html.size():end+3;
                continue;
            }
            auto close=html.find('>',i+1);
            if(close==std::string_view::npos)break;
            std::string_view inside=html.substr(i+1,close-i-1);
            i=close+1;
            while(!inside.empty()&&std::isspace(static_cast<unsigned char>(inside.front())))inside.remove_prefix(1);
            bool closing=!inside.empty()&&inside.front()=='/';
            if(closing)inside.remove_prefix(1);
            size_t tlen=0;
            while(tlen<inside.size()&&(std::isalnum(static_cast<unsigned char>(inside[tlen]))||inside[tlen]=='-'))tlen++;
            std::string tag=lower_str(inside.substr(0,tlen));
            bool self_close=!inside.empty()&&inside.back()=='/';
            if(in_skip){
                if(closing&&tag==skip_tag)in_skip=false;
                continue;
            }
            if(!closing&&!self_close&&(tag=="script"||tag=="style"||tag=="noscript"||tag=="svg"||tag=="head")){
                in_skip=true;
                skip_tag=tag;
                continue;
            }
            if(tag=="br"||tag=="tr"||tag=="li"){
                push_nl(1);
                if(!closing&&tag=="li")out+="- ";
            }else if(tag=="p"||tag=="div"||tag=="section"||tag=="article"||tag=="pre"||tag=="blockquote"
                     ||(tag.size()==2&&tag[0]=='h'&&tag[1]>='1'&&tag[1]<='6')){
                push_nl(2);
                if(!closing&&tag.size()==2&&tag[0]=='h'){
                    out.append(static_cast<size_t>(tag[1]-'0'),'#');
                    out+=' ';
                }
            }
            continue;
        }
        if(in_skip){i++;continue;}
        if(html[i]=='&'){
            auto semi=html.find(';',i+1);
            if(semi!=std::string_view::npos&&semi-i<=10){
                std::string ent=lower_str(html.substr(i+1,semi-i-1));
                i=semi+1;
                if(ent=="amp")out+='&';
                else if(ent=="lt")out+='<';
                else if(ent=="gt")out+='>';
                else if(ent=="quot")out+='"';
                else if(ent=="#39"||ent=="apos")out+='\'';
                else if(ent=="nbsp")out+=' ';
                else if(ent.size()>1&&ent[0]=='#'){
                    unsigned long code=ent[1]=='x'?strtoul(ent.c_str()+2,nullptr,16):strtoul(ent.c_str()+1,nullptr,10);
                    if(code>=32&&code<127)out+=static_cast<char>(code);
                    else out+=' ';
                }
                continue;
            }
        }
        char c=html[i++];
        if(c=='\r'||c=='\n'||c=='\t'||c==' '){
            if(!out.empty()&&out.back()!=' '&&out.back()!='\n')out+=' ';
        }else if(static_cast<unsigned char>(c)>=32){
            out+=c;
        }
    }
    while(!out.empty()&&(out.back()==' '||out.back()=='\n'))out.pop_back();
    size_t used=0;
    return display_utf8(out,used,true);
}

std::vector<std::string> shell_tokens(std::string_view cmd,bool &clean_simple){
    clean_simple=true;
    std::vector<std::string> tokens;
    std::string cur;
    bool in_tok=false;
    char quote=0;
    for(size_t i=0;i<cmd.size();i++){
        char c=cmd[i];
        if(quote){
            if(c==quote){quote=0;continue;}
            if(quote=='"'&&(c=='$'||c=='`'||c=='\\')){clean_simple=false;return {};}
            cur+=c;
            continue;
        }
        if(c=='\''||c=='"'){quote=c;in_tok=true;continue;}
        if(c=='\n'||c=='\r'||c==';'||c=='&'||c=='|'||c=='>'||c=='<'||c=='`'||c=='$'||c=='('||c==')'||c=='\\'){
            clean_simple=false;
            return {};
        }
        if(std::isspace(static_cast<unsigned char>(c))){
            if(in_tok){tokens.push_back(std::move(cur));cur.clear();in_tok=false;}
            continue;
        }
        in_tok=true;
        cur+=c;
    }
    if(quote){clean_simple=false;return {};}
    if(in_tok)tokens.push_back(std::move(cur));
    return tokens;
}
} // namespace

void record_file_change(App *app,const char *rel_path,const char *backup_path,bool existed_before,unsigned int mode_before){
    if(!app||!*app->root||app->turn_id.empty())return;
    std::string bak_leaf;
    if(backup_path&&*backup_path){
        bak_leaf=path_text(fs_path(backup_path).filename());
        if(bak_leaf.size()!=36||!bak_leaf.ends_with(".bak")||!valid_hex32_str(std::string_view(bak_leaf).substr(0,32)))
            bak_leaf.clear();
    }
    if(existed_before&&bak_leaf.empty())return;

    JsonPtr doc=read_undo_journal(app->root);
    if(!doc)doc.reset(obj());
    J *turns=get(doc.get(),"turns");
    if(!json_object_is_type(turns,json_type_array)){turns=arr();put(doc.get(),"turns",turns);}

    J *turn=nullptr;
    if(alen(turns)>0){
        J *last=at(turns,alen(turns)-1);
        if(!strcmp(sget(last,"id"),app->turn_id.c_str()))turn=last;
    }
    if(!turn){
        turn=obj();
        put(turn,"id",str(app->turn_id.c_str()));
        put(turn,"files",arr());
        add(turns,turn);
    }
    J *files=get(turn,"files");
    // Keep only the earliest snapshot of a file within the same turn so /undo
    // restores the exact pre-turn state even after multiple edits.
    for(size_t i=0;i<alen(files);i++){
        if(!strcmp(sget(at(files,i),"path"),rel_path)){
            if(!bak_leaf.empty()){
                std::error_code ec;
                fs::remove(fs_path(std::string(app->root)+"/.desk-agent/backups/"+bak_leaf),ec);
            }
            return;
        }
    }
    J *entry=obj();
    put(entry,"path",str(rel_path));
    put(entry,"existed",json_object_new_boolean(existed_before));
    put(entry,"mode",json_object_new_int(static_cast<int>(mode_before&0777)));
    if(!bak_leaf.empty())put(entry,"backup",str(bak_leaf.c_str()));
    add(files,entry);

    while(alen(turns)>20){
        J *oldest=at(turns,0);
        J *old_files=get(oldest,"files");
        for(size_t i=0;i<alen(old_files);i++){
            const char *b=sget(at(old_files,i),"backup");
            if(*b){
                std::error_code ec;
                fs::remove(fs_path(std::string(app->root)+"/.desk-agent/backups/"+b),ec);
            }
        }
        json_object_array_del_idx(turns,0,1);
    }
    write_undo_journal(app->root,doc.get());
}

bool undo_last_turn(App &app,std::string &summary){
    summary.clear();
    ResourceLock lock(app,false);
    if(!lock.acquired()){
        seterr(&app,tr("工作目录正在执行操作，请等待结束或停止后再撤回。"));
        return false;
    }
    JsonPtr doc=read_undo_journal(app.root);
    J *turns=get(doc.get(),"turns");
    if(!turns||alen(turns)==0){
        seterr(&app,tr("没有可撤回的文件修改记录。"));
        return false;
    }
    J *last=at(turns,alen(turns)-1);
    J *files=get(last,"files");
    size_t restored=0,removed=0;
    std::vector<std::string> touched,backups,failed;

    auto restore=[&](J *entry,const char *rel){
        char full[PATH_MAX];
        if(!*rel||resolve_file(&app,rel,full)!=0||path_link(fs_path(full)))return false;
        if(bget(entry,"existed")){
            const std::string_view bak=sget(entry,"backup");
            if(bak.size()!=36||!bak.ends_with(".bak")||!valid_hex32_str(bak.substr(0,32)))return false;
            std::string bak_full=std::string(app.root)+"/.desk-agent/backups/"+std::string(bak);
            if(path_link(fs_path(bak_full)))return false;
            size_t len=0;
            char *bytes=read_bytes(bak_full.c_str(),EDIT_FILE_LIMIT,&len);
            if(!bytes)return false;
            unsigned int mode=static_cast<unsigned int>(iget(entry,"mode",0644))&0777;
            std::string parent=path_text(fs_path(full).parent_path());
            const bool ok=mkdirs(parent.c_str(),0755)==0&&atomic_file(full,bytes,len,mode?mode:0644)==0;
            free(bytes);
            if(!ok)return false;
            backups.push_back(std::move(bak_full));
            restored++;
        }else{
            std::error_code ec;
            const auto target=fs_path(full);
            const auto status=fs::symlink_status(target,ec);
            if(status.type()==fs::file_type::not_found)return true;
            if(ec||!fs::is_regular_file(status)||!fs::remove(target,ec)||ec)return false;
            removed++;
        }
        touched.emplace_back(rel);
        return true;
    };

    for(size_t k=alen(files);k>0;--k){
        J *entry=at(files,k-1);
        const char *rel=sget(entry,"path");
        if(restore(entry,rel))json_object_array_del_idx(files,k-1,1);
        else failed.emplace_back(rel);
    }
    if(failed.empty())json_object_array_del_idx(turns,alen(turns)-1,1);
    if(!write_undo_journal(app.root,doc.get())){
        seterr(&app,tr("无法保存撤回进度，请先检查工作区文件和权限。"));
        return false;
    }
    for(const auto &backup:backups){
        std::error_code ec;
        fs::remove(fs_path(backup),ec);
    }
    if(!failed.empty()){
        summary=tr("撤回未完成，以下文件保留在记录中，可重试：");
        for(const auto &path:failed)summary+="\n  "+path;
        seterr(&app,tr("撤回未完成，失败项已保留。"));
        return false;
    }
    app.error[0]=0;
    char head[256];
    snprintf(head,sizeof head,tr("已撤回上一轮改动（恢复 %zu 个文件，移除 %zu 个新建文件）："),restored,removed);
    summary=head;
    for(size_t i=0;i<std::min<size_t>(touched.size(),6);i++){
        summary+=(i?", ":" ")+touched[i];
    }
    if(touched.size()>6)summary+=" …";
    return true;
}

void prune_workspace_cache(const App &app){
    if(!*app.root)return;
    std::set<std::string> referenced;
    if(JsonPtr doc=read_undo_journal(app.root)){
        J *turns=get(doc.get(),"turns");
        for(size_t i=0;i<alen(turns);i++){
            J *files=get(at(turns,i),"files");
            for(size_t j=0;j<alen(files);j++){
                const char *b=sget(at(files,j),"backup");
                if(*b)referenced.insert(b);
            }
        }
    }
    auto prune_dir=[&](const std::string &dir,size_t keep_count,bool honor_refs){
        std::error_code ec;
        if(!fs::exists(fs_path(dir),ec)||ec)return;
        struct Item {fs::file_time_type mtime;fs::path path;std::string name;};
        std::vector<Item> items;
        for(fs::directory_iterator it(fs_path(dir),ec),end;it!=end&&!ec;it.increment(ec)){
            if(path_link(it->path()))continue;
            auto st=fs::symlink_status(it->path(),ec);
            if(ec||!fs::is_regular_file(st))continue;
            std::string name=path_text(it->path().filename());
            if(honor_refs&&referenced.contains(name))continue;
            auto mt=fs::last_write_time(it->path(),ec);
            if(!ec)items.push_back({mt,it->path(),std::move(name)});
        }
        std::sort(items.begin(),items.end(),[](const Item &a,const Item &b){return a.mtime>b.mtime;});
        for(size_t i=keep_count;i<items.size();i++)fs::remove(items[i].path,ec);
    };
    prune_dir(std::string(app.root)+"/.desk-agent/backups",32,true);
    prune_dir(std::string(app.root)+"/.desk-agent/clipboard",24,false);
}

bool glob_match(std::string_view pattern,std::string_view text,bool case_insensitive){
    auto lbrace=pattern.find('{');
    auto rbrace=lbrace==std::string_view::npos?std::string_view::npos:pattern.find('}',lbrace+1);
    if(lbrace!=std::string_view::npos&&rbrace!=std::string_view::npos){
        std::string_view prefix=pattern.substr(0,lbrace);
        std::string_view suffix=pattern.substr(rbrace+1);
        std::string_view opts=pattern.substr(lbrace+1,rbrace-lbrace-1);
        for(size_t s=0;;){
            auto comma=opts.find(',',s);
            std::string_view part=opts.substr(s,comma==std::string_view::npos?std::string_view::npos:comma-s);
            std::string expanded=std::string(prefix)+std::string(part)+std::string(suffix);
            if(glob_match(expanded,text,case_insensitive))return true;
            if(comma==std::string_view::npos)break;
            s=comma+1;
        }
        return false;
    }
    if(pattern.find('/')==std::string_view::npos){
        auto slash=text.rfind('/');
        std::string_view base=slash==std::string_view::npos?text:text.substr(slash+1);
        return glob_simple(pattern,base,case_insensitive);
    }
    return glob_simple(pattern,text,case_insensitive);
}

void GitIgnore::load(const char *root){
    rules.clear();
    if(!root||!*root)return;
    fs::path p=fs_path(root)/".gitignore";
    if(path_link(p))return;
    size_t len=0;
    std::string full=path_text(p);
    char *bytes=read_bytes(full.c_str(),65536,&len);
    if(!bytes)return;
    std::string_view text(bytes,len);
    if(!memchr(bytes,0,len)&&valid_utf8(text)){
        for(size_t pos=0;pos<text.size()&&rules.size()<256;){
            auto nl=text.find('\n',pos);
            std::string_view line=nl==std::string_view::npos?text.substr(pos):text.substr(pos,nl-pos);
            pos=nl==std::string_view::npos?text.size():nl+1;
            while(!line.empty()&&(line.back()=='\r'||line.back()==' '||line.back()=='\t'))line.remove_suffix(1);
            while(!line.empty()&&(line.front()==' '||line.front()=='\t'))line.remove_prefix(1);
            if(line.empty()||line.front()=='#'||line.front()=='!')continue;
            rules.emplace_back(line);
        }
    }
    free(bytes);
}

bool GitIgnore::ignored(std::string_view rel_path,bool is_dir) const {
    if(rules.empty()||rel_path.empty())return false;
    auto slash=rel_path.rfind('/');
    std::string_view base=slash==std::string_view::npos?rel_path:rel_path.substr(slash+1);
    for(std::string_view rule:rules){
        bool dir_only=rule.ends_with('/');
        if(dir_only){
            if(!is_dir)continue;
            rule.remove_suffix(1);
        }
        bool rooted=rule.starts_with('/');
        if(rooted)rule.remove_prefix(1);
        if(rule.empty())continue;
        if(rule.ends_with("/**")){
            std::string_view prefix=rule.substr(0,rule.size()-3);
            if(rel_path==prefix||rel_path.starts_with(std::string(prefix)+"/"))return true;
            continue;
        }
        if(rooted||rule.find('/')!=std::string_view::npos){
            if(glob_simple(rule,rel_path,false))return true;
        }else{
            if(glob_simple(rule,base,false))return true;
        }
    }
    return false;
}

bool safe_readonly_command(std::string_view command){
    bool simple=false;
    auto tokens=shell_tokens(command,simple);
    if(!simple||tokens.empty())return false;
    const std::string &prog=tokens[0];
    if(prog=="pwd"||prog=="whoami"||prog=="uname"||prog=="date"||prog=="ls"
       ||prog=="wc"||prog=="head"||prog=="tail"||prog=="cat"||prog=="stat"||prog=="file"||prog=="which"){
        return true;
    }
    if(prog=="grep"||prog=="rg"){
        for(size_t i=1;i<tokens.size();i++)
            if(tokens[i]=="--pre"||tokens[i].starts_with("--pre="))return false;
        return true;
    }
    if(prog=="git"&&tokens.size()>=2){
        const std::string &sub=tokens[1];
        if(sub=="status"||sub=="diff"||sub=="log"||sub=="show"||sub=="rev-parse"||sub=="ls-files")return true;
        if(sub=="branch"){
            for(size_t i=2;i<tokens.size();i++){
                const auto &a=tokens[i];
                if(a!="--show-current"&&a!="-l"&&a!="--list"&&a!="-a"&&a!="--all"&&a!="-r"&&a!="-v"&&a!="-vv")
                    return false;
            }
            return true;
        }
    }
    return false;
}

static std::string command_rule_prefix(std::string_view command){
    bool simple=false;
    auto tokens=shell_tokens(command,simple);
    if(!simple||tokens.empty())return {};
    const std::string &p=tokens[0];
    if((p=="git"||p=="make"||p=="cargo"||p=="go"||p=="npm"||p=="pnpm"||p=="yarn"||p=="cmake"||p=="ctest")&&tokens.size()>=2){
        if(tokens[1].front()!='-')return p+" "+tokens[1];
    }
    if((p=="pytest"||p=="ctest")&&tokens.size()>=1)return p;
    // Exact simple command match for everything else.
    std::string joined;
    for(size_t i=0;i<tokens.size();i++){if(i)joined+=' ';joined+=tokens[i];}
    return joined;
}

void remember_allowed_command(App &app,std::string_view command){
    std::string key=command_rule_prefix(command);
    if(key.empty())return;
    if(std::find(app.allowed_commands.begin(),app.allowed_commands.end(),key)==app.allowed_commands.end())
        app.allowed_commands.push_back(std::move(key));
}

bool command_allowed_in_session(const App &app,std::string_view command){
    if(app.allowed_commands.empty())return false;
    bool simple=false;
    auto tokens=shell_tokens(command,simple);
    if(!simple||tokens.empty())return false;
    std::string key=command_rule_prefix(command);
    if(key.empty())return false;
    return std::find(app.allowed_commands.begin(),app.allowed_commands.end(),key)!=app.allowed_commands.end();
}

static bool safe_http_get(App *app,std::string &current_url,std::string &resolve_entry,
                          std::string &body_out,std::string &ct_out,long &status_out,std::string &err_out){
    struct FetchBuf {
        std::string body,location,content_type;
        bool oversized=false;
    };
    auto write_cb=+[](char *p,size_t sz,size_t n,void *ud)->size_t{
        auto *fb=static_cast<FetchBuf *>(ud);
        if(sz&&n>SIZE_MAX/sz)return 0;
        size_t total=sz*n;
        if(total>MAX_WEB_RAW-fb->body.size()){fb->oversized=true;return 0;}
        try{fb->body.append(p,total);}catch(...){return 0;}
        return total;
    };
    auto header_cb=+[](char *p,size_t sz,size_t n,void *ud)->size_t{
        auto *fb=static_cast<FetchBuf *>(ud);
        size_t total=sz*n;
        std::string_view line(p,total);
        auto colon=line.find(':');
        if(colon!=std::string_view::npos){
            std::string k=lower_str(line.substr(0,colon));
            std::string_view v=line.substr(colon+1);
            while(!v.empty()&&(v.front()==' '||v.front()=='\t'))v.remove_prefix(1);
            while(!v.empty()&&(v.back()=='\r'||v.back()=='\n'||v.back()==' '))v.remove_suffix(1);
            if(k=="location")fb->location=std::string(v);
            else if(k=="content-type")fb->content_type=lower_str(v);
        }
        return total;
    };
    auto xfer_cb=+[](void *ud,curl_off_t,curl_off_t,curl_off_t,curl_off_t)->int{
        auto *a=static_cast<App *>(ud);
        check_exit(*a);
        progress_tick(a);
        return interrupted?1:0;
    };

    FetchBuf fetched;
    status_out=0;
    for(int hop=0;hop<=3;hop++){
        fetched=FetchBuf{};
        CurlHandle curl(curl_easy_init());
        if(!curl){err_out=tr("无法初始化网络请求。");return false;}
        curl_slist *resolve_list=nullptr;
        if(!resolve_entry.empty())resolve_list=curl_slist_append(nullptr,resolve_entry.c_str());
        struct FreeResolve {curl_slist *l;~FreeResolve(){if(l)curl_slist_free_all(l);}} guard_res{resolve_list};

        curl_easy_setopt(curl.get(),CURLOPT_URL,current_url.c_str());
        if(resolve_list)curl_easy_setopt(curl.get(),CURLOPT_RESOLVE,resolve_list);
        curl_easy_setopt(curl.get(),CURLOPT_USERAGENT,"DeskCode/" VERSION);
        curl_easy_setopt(curl.get(),CURLOPT_CONNECTTIMEOUT_MS,8000L);
        curl_easy_setopt(curl.get(),CURLOPT_TIMEOUT_MS,20000L);
        curl_easy_setopt(curl.get(),CURLOPT_FOLLOWLOCATION,0L);
        curl_easy_setopt(curl.get(),CURLOPT_NOSIGNAL,1L);
        curl_easy_setopt(curl.get(),CURLOPT_WRITEFUNCTION,write_cb);
        curl_easy_setopt(curl.get(),CURLOPT_WRITEDATA,&fetched);
        curl_easy_setopt(curl.get(),CURLOPT_HEADERFUNCTION,header_cb);
        curl_easy_setopt(curl.get(),CURLOPT_HEADERDATA,&fetched);
        curl_easy_setopt(curl.get(),CURLOPT_XFERINFOFUNCTION,xfer_cb);
        curl_easy_setopt(curl.get(),CURLOPT_XFERINFODATA,app);
        curl_easy_setopt(curl.get(),CURLOPT_NOPROGRESS,0L);

        CURLcode code=curl_easy_perform(curl.get());
        if(interrupted){err_out=tr("请求已取消。");return false;}
        if(fetched.oversized){err_out=tr("网页响应超过 2 MB 上限。");return false;}
        if(code!=CURLE_OK){err_out=tr("网页请求失败或连接超时。");return false;}
        curl_easy_getinfo(curl.get(),CURLINFO_RESPONSE_CODE,&status_out);
        if((status_out==301||status_out==302||status_out==303||status_out==307||status_out==308)&&!fetched.location.empty()){
            if(hop==3){err_out=tr("网页重定向次数过多。");return false;}
            CURLU *base_u=curl_url();
            std::string next_raw=fetched.location;
            if(base_u){
                if(curl_url_set(base_u,CURLUPART_URL,current_url.c_str(),0)==CURLUE_OK&&
                   curl_url_set(base_u,CURLUPART_URL,fetched.location.c_str(),0)==CURLUE_OK){
                    char *resolved_u=nullptr;
                    if(curl_url_get(base_u,CURLUPART_URL,&resolved_u,0)==CURLUE_OK&&resolved_u){
                        next_raw=resolved_u;
                        curl_free(resolved_u);
                    }
                }
                curl_url_cleanup(base_u);
            }
            if(!validate_fetch_url(next_raw.c_str(),current_url,resolve_entry,err_out))return false;
            continue;
        }
        break;
    }
    body_out=std::move(fetched.body);
    ct_out=std::move(fetched.content_type);
    return true;
}

static std::string percent_decode(std::string_view in){
    std::string out;
    out.reserve(in.size());
    auto hex=[](char c)->int{
        if(c>='0'&&c<='9')return c-'0';
        if(c>='a'&&c<='f')return c-'a'+10;
        if(c>='A'&&c<='F')return c-'A'+10;
        return -1;
    };
    for(size_t i=0;i<in.size();i++){
        if(in[i]=='%'&&i+2<in.size()){
            int hi=hex(in[i+1]),lo=hex(in[i+2]);
            if(hi>=0&&lo>=0){out+=static_cast<char>((hi<<4)|lo);i+=2;continue;}
        }
        if(in[i]=='+')out+=' ';
        else out+=in[i];
    }
    return out;
}

static std::string unwrap_ddg_href(std::string_view href){
    std::string clean=html_to_text(href);
    if(auto u=clean.find("uddg=");u!=std::string::npos){
        std::string_view part(clean.data()+u+5,clean.size()-(u+5));
        if(auto amp=part.find('&');amp!=std::string_view::npos)part=part.substr(0,amp);
        return percent_decode(part);
    }
    if(clean.starts_with("//"))return "https:"+clean;
    return clean;
}

J *tool_web_fetch(App *app,J *args){
    const char *raw_url=sget(args,"url");
    if(!*raw_url||strlen(raw_url)>2048)return err(tr("网页地址长度无效。"));
    size_t max_bytes=40000;
    if(J *mb=get(args,"max_bytes")){
        int64_t v=json_object_get_int64(mb);
        if(v<1000||v>120000)return err(tr("max_bytes 须在 1000 到 120000 之间。"));
        max_bytes=static_cast<size_t>(v);
    }

    std::string current_url,resolve_entry,url_err;
    if(!validate_fetch_url(raw_url,current_url,resolve_entry,url_err))return err(url_err.c_str());

    if(!approve(app,current_url.c_str(),Operation::Other,tr("访问网页"))){
        J *r=err(tr("用户拒绝访问网页，不要绕过。"));
        put(r,"denied",json_object_new_boolean(1));
        return r;
    }

    std::string body,content_type;
    long status=0;
    if(!safe_http_get(app,current_url,resolve_entry,body,content_type,status,url_err))
        return err(url_err.c_str());

    if(status<200||status>=300){
        char msg[128];
        snprintf(msg,sizeof msg,tr("网页返回 HTTP %ld。"),status);
        return err(msg);
    }

    bool is_html=content_type.find("html")!=std::string::npos
                 ||body.find("<html")!=std::string::npos
                 ||body.find("<!DOCTYPE html")!=std::string::npos;
    std::string text;
    if(is_html){
        text=html_to_text(body);
    }else{
        size_t used=0;
        text=display_utf8(body,used,true);
    }
    bool truncated=false;
    if(text.size()>max_bytes){
        size_t cut=max_bytes;
        while(cut>0&&(static_cast<unsigned char>(text[cut])&0xc0)==0x80)--cut;
        text.resize(cut);
        truncated=true;
    }
    J *res=obj();
    put(res,"url",str(current_url.c_str()));
    put(res,"content_type",str(content_type.empty()?"text/plain":content_type.c_str()));
    put(res,"content",str(text.c_str()));
    put(res,"truncated",json_object_new_boolean(truncated));
    return res;
}

J *tool_web_search(App *app,J *args){
    const char *query=sget(args,"query");
    if(!*query||strlen(query)>256||strchr(query,'\n')||strchr(query,'\r'))
        return err(tr("搜索关键词须为 1..256 字节的单行文本。"));
    size_t max_results=5;
    if(J *mr=get(args,"max_results")){
        int64_t v=json_object_get_int64(mr);
        if(v<1||v>10)return err(tr("max_results 须在 1 到 10 之间。"));
        max_results=static_cast<size_t>(v);
    }
    if(!approve(app,query,Operation::Other,tr("联网搜索"))){
        J *r=err(tr("用户拒绝联网搜索，不要绕过。"));
        put(r,"denied",json_object_new_boolean(1));
        return r;
    }

    CurlHandle esc_curl(curl_easy_init());
    if(!esc_curl)return err(tr("无法初始化网络请求。"));
    char *escaped=curl_easy_escape(esc_curl.get(),query,static_cast<int>(strlen(query)));
    if(!escaped)return err(tr("无法编码搜索关键词。"));
    std::string encoded_q(escaped);
    curl_free(escaped);

    const char *custom=env_value("DESK_SEARCH_URL");
    std::string raw_url;
    if(custom&&*custom){
        raw_url=std::string(custom)+(strchr(custom,'?')?"&q=":"?q=")+encoded_q+"&format=json";
    }else{
        raw_url="https://html.duckduckgo.com/html/?q="+encoded_q;
    }

    std::string current_url,resolve_entry,url_err,body,content_type;
    if(!validate_fetch_url(raw_url.c_str(),current_url,resolve_entry,url_err))return err(url_err.c_str());
    long status=0;
    if(!safe_http_get(app,current_url,resolve_entry,body,content_type,status,url_err))return err(url_err.c_str());
    if(status<200||status>=300){
        char msg[128];
        snprintf(msg,sizeof msg,tr("搜索服务返回 HTTP %ld。"),status);
        return err(msg);
    }

    J *results=arr();
    std::string_view trimmed_body(body);
    while(!trimmed_body.empty()&&std::isspace(static_cast<unsigned char>(trimmed_body.front())))trimmed_body.remove_prefix(1);
    if(content_type.find("json")!=std::string::npos||(!trimmed_body.empty()&&trimmed_body.front()=='{')){
        JsonPtr parsed(json_tokener_parse(body.c_str()));
        J *items=get(parsed.get(),"results");
        if(!items)items=get(parsed.get(),"items");
        for(size_t i=0;i<alen(items)&&alen(results)<max_results;i++){
            J *it=at(items,i);
            const char *u=*sget(it,"url")?sget(it,"url"):sget(it,"link");
            const char *t=sget(it,"title");
            const char *sn=*sget(it,"content")?sget(it,"content"):sget(it,"snippet");
            if(!*u||!*t)continue;
            J *entry=obj();
            put(entry,"title",str(t));
            put(entry,"url",str(u));
            put(entry,"snippet",str(sn));
            add(results,entry);
        }
    }else{
        std::string_view view(body);
        size_t pos=0;
        while(alen(results)<max_results&&pos<view.size()){
            auto hit=view.find("result__a",pos);
            if(hit==std::string_view::npos)break;
            auto a_open=view.rfind("<a",hit);
            auto tag_end=view.find('>',hit);
            auto a_close=tag_end==std::string_view::npos?std::string_view::npos:view.find("</a>",tag_end+1);
            if(a_open==std::string_view::npos||tag_end==std::string_view::npos||a_close==std::string_view::npos){
                pos=hit+9;
                continue;
            }
            std::string_view attrs=view.substr(a_open,tag_end-a_open);
            std::string href;
            if(auto hp=attrs.find("href=\"");hp!=std::string_view::npos){
                auto hend=attrs.find('"',hp+6);
                if(hend!=std::string_view::npos)href=unwrap_ddg_href(attrs.substr(hp+6,hend-(hp+6)));
            }
            std::string title=html_to_text(view.substr(tag_end+1,a_close-(tag_end+1)));
            std::string snippet;
            auto next_hit=view.find("result__a",a_close+4);
            std::string_view region=view.substr(a_close+4,next_hit==std::string_view::npos?std::min<size_t>(2000,view.size()-(a_close+4)):next_hit-(a_close+4));
            if(auto sp=region.find("result__snippet");sp!=std::string_view::npos){
                auto st_end=region.find('>',sp);
                auto sc_end=st_end==std::string_view::npos?std::string_view::npos:region.find("</",st_end+1);
                if(st_end!=std::string_view::npos&&sc_end!=std::string_view::npos)
                    snippet=html_to_text(region.substr(st_end+1,sc_end-(st_end+1)));
            }
            if(!href.empty()&&!title.empty()&&(href.starts_with("https://")||href.starts_with("http://"))){
                J *entry=obj();
                put(entry,"title",str(title.c_str()));
                put(entry,"url",str(href.c_str()));
                put(entry,"snippet",str(snippet.c_str()));
                add(results,entry);
            }
            pos=a_close+4;
        }
    }

    J *out=obj();
    put(out,"query",str(query));
    put(out,"results",results);
    return out;
}

J *tool_desktop_screenshot(App *app,J *args){
    const char *target=sget(args,"app");
    std::string desc=*target?(std::string(tr("目标窗口："))+target):std::string(tr("当前活动窗口或屏幕"));
    if(!approve(app,desc.c_str(),Operation::Other,tr("截取桌面"))){
        J *r=err(tr("用户拒绝截取桌面，不要绕过。"));
        put(r,"denied",json_object_new_boolean(1));
        return r;
    }

    std::string bytes;
    if(const char *mock=env_value("DESK_DESKTOP_SCREENSHOT_FILE");mock&&*mock){
        size_t len=0;
        char *raw=read_bytes(mock,MAX_SCREENSHOT_BYTES,&len);
        if(raw){bytes.assign(raw,len);free(raw);}
    }else{
#ifdef _WIN32
        App probe{};
        snprintf(probe.root,sizeof probe.root,"%s",app->root);
        JsonPtr res(run_command(&probe,
            "powershell.exe -Sta -NoProfile -NonInteractive -Command "
            "\"Add-Type -AssemblyName System.Windows.Forms,System.Drawing; "
            "$b=[Windows.Forms.Screen]::PrimaryScreen.Bounds; "
            "$bmp=New-Object Drawing.Bitmap $b.Width,$b.Height; "
            "$g=[Drawing.Graphics]::FromImage($bmp); "
            "$g.CopyFromScreen($b.Location,[Drawing.Point]::Empty,$b.Size); "
            "$ms=New-Object IO.MemoryStream; $bmp.Save($ms,[Drawing.Imaging.ImageFormat]::Png); "
            "$g.Dispose(); $bmp.Dispose(); [Convert]::ToBase64String($ms.ToArray())\"",8000));
        if(res&&iget(res.get(),"exit_code",-1)==0&&*sget(res.get(),"output"))
            bytes=b64_decode(sget(res.get(),"output"));
#else
        std::string dir=std::string(app->root)+"/.desk-agent/clipboard";
        if(mkdirs(dir.c_str(),0700)==0){
            char id[33];id_string(id);
            std::string tmp=dir+"/shot-"+id+".png";
            auto run_shot=[&](const char *const argv[]){
                pid_t pid=fork();
                if(pid==0){
                    int dn=open("/dev/null",O_RDWR);
                    if(dn>=0){dup2(dn,0);dup2(dn,1);dup2(dn,2);if(dn>2)close(dn);}
                    execvp(argv[0],const_cast<char *const *>(argv));
                    _exit(127);
                }
                if(pid<0)return false;
                int st=0;
                double until=monotime()+5.0;
                while(waitpid(pid,&st,WNOHANG)==0&&monotime()<until)usleep(20000);
                if(waitpid(pid,&st,WNOHANG)==0){kill(pid,SIGKILL);while(waitpid(pid,&st,0)<0&&errno==EINTR){}}
                return WIFEXITED(st)&&WEXITSTATUS(st)==0;
            };
#ifdef __APPLE__
            const char *mac_shot[]={"/usr/sbin/screencapture","-x",tmp.c_str(),nullptr};
            bool ok=run_shot(mac_shot);
#else
            const char *spec_active[]={"spectacle","-b","-n","-a","-o",tmp.c_str(),nullptr};
            const char *spec_full[]={"spectacle","-b","-n","-f","-o",tmp.c_str(),nullptr};
            const char *grim_cmd[]={"grim",tmp.c_str(),nullptr};
            bool ok=(*target?run_shot(spec_active):run_shot(spec_full))||run_shot(grim_cmd);
#endif
            if(ok&&!path_link(fs_path(tmp))){
                size_t len=0;
                char *raw=read_bytes(tmp.c_str(),MAX_SCREENSHOT_BYTES,&len);
                if(raw){bytes.assign(raw,len);free(raw);}
            }
            std::error_code ec;
            fs::remove(fs_path(tmp),ec);
        }
#endif
    }

    const char *mime=nullptr;
    if(bytes.size()>=8&&!memcmp(bytes.data(),"\x89PNG\r\n\x1a\n",8))mime="image/png";
    else if(bytes.size()>=3&&static_cast<unsigned char>(bytes[0])==0xff&&static_cast<unsigned char>(bytes[1])==0xd8)mime="image/jpeg";
    if(!mime)return err(tr("无法截取桌面图像（Linux 需 spectacle 或 grim）。"));

    std::string encoded=b64_encode(bytes.data(),bytes.size());
    if(encoded.empty())return err(tr("桌面图像编码失败。"));
    std::string data_url=std::string("data:")+mime+";base64,"+encoded;

    J *res=obj(),*blocks=arr(),*tb=obj(),*ib=obj(),*iu=obj();
    put(tb,"type",str("text"));
    put(tb,"text",str("Desktop screenshot captured."));
    add(blocks,tb);
    put(ib,"type",str("image_url"));
    put(iu,"url",str(data_url.c_str()));
    put(ib,"image_url",iu);
    add(blocks,ib);
    put(res,"content",blocks);
    return res;
}

bool compact_conversation(App &app,std::string &summary){
    if(alen(app.messages)<2){
        seterr(&app,tr("当前对话内容较少，无需压缩上下文。"));
        return false;
    }
    std::string digest;
    digest.reserve(32768);
    for(size_t i=0;i<alen(app.messages)&&digest.size()<90000;i++){
        J *m=at(app.messages,i);
        const char *role=sget(m,"role");
        digest+="["+std::string(role)+"] ";
        if(!strcmp(role,"user")){
            std::string u=user_message_text(m);
            if(u.size()>4000)u.resize(4000);
            digest+=u;
        }else if(!strcmp(role,"tool")){
            std::string_view c=sget(m,"content");
            digest.append(c.substr(0,std::min<size_t>(c.size(),1200)));
        }else{
            std::string_view c=sget(m,"content");
            digest.append(c.substr(0,std::min<size_t>(c.size(),4000)));
            J *calls=get(m,"tool_calls");
            for(size_t k=0;k<alen(calls);k++){
                J *fn=get(at(calls,k),"function");
                digest+=" [call: ";
                digest+=sget(fn,"name");
                digest+="]";
            }
        }
        digest+="\n\n";
    }
    size_t used=0;
    digest=display_utf8(digest,used,true);

    App sub{};
    sub.config=copy(app.config);
    sub.messages=arr();
    snprintf(sub.root,sizeof sub.root,"%s",app.root);
    snprintf(sub.config_dir,sizeof sub.config_dir,"%s",app.config_dir);
    sub.task_events=-1;
    auto cleanup=[](App *v){app_cleanup(v);};
    std::unique_ptr<App,decltype(cleanup)> guard(&sub,cleanup);

    std::string prompt=!strcmp(language_code(),"en")
        ?"Summarize the following coding conversation into a dense, accurate continuation brief "
         "(Goals, Files Modified/Inspected, Key Decisions & Findings, Pending Work). Keep exact file paths and commands:\n\n"
        :"请将以下编程对话提炼为高密度、准确的续接摘要（包含：目标、已检查或修改的文件路径、关键结论与约束、待办事项），保留精确文件名与命令：\n\n";
    prompt+=digest;

    J *u=obj();
    put(u,"role",str("user"));
    put(u,"content",str(prompt.c_str()));
    add(sub.messages,u);

    JsonPtr reply(api_complete(&sub,sub.config,sub.messages,nullptr));
    if(!reply||!*sget(reply.get(),"content")){
        seterr(&app,sub.error[0]?sub.error:tr("上下文摘要生成失败。"));
        return false;
    }
    summary=sget(reply.get(),"content");

    J *fresh_msgs=arr();
    J *anchor=obj(),*ans=obj();
    put(anchor,"role",str("user"));
    put(anchor,"content",str("Carry forward a summary of the preceding context."));
    put(anchor,"_display",str("/compact"));
    put(ans,"role",str("assistant"));
    std::string wrapped="Context summary:\n"+summary;
    put(ans,"content",str(wrapped.c_str()));
    add(fresh_msgs,anchor);
    add(fresh_msgs,ans);

    json_object_put(app.messages);
    app.messages=fresh_msgs;
    return true;
}

namespace {
void set_temp_env(const char *key,const char *val){
#ifdef _WIN32
    SetEnvironmentVariableW(wide(key).c_str(),val?wide(val).c_str():nullptr);
#else
    if(val)setenv(key,val,1);
    else unsetenv(key);
#endif
}

std::vector<std::string> collect_hook_commands(App &app,const char *event_name,std::string_view target){
    std::vector<std::string> cmds;
    auto parse_file=[&](const std::string &path,bool workspace_scoped){
        if(path_link(fs_path(path)))return;
        size_t len=0;
        char *bytes=read_bytes(path.c_str(),65536,&len);
        if(!bytes)return;
        JsonPtr doc(json_tokener_parse(bytes));
        free(bytes);
        if(!doc||!json_object_is_type(doc.get(),json_type_object))return;
        J *spec=get(doc.get(),event_name);
        if(!spec)return;
        std::vector<std::string> matched;
        if(json_object_is_type(spec,json_type_string)){
            const char *s=json_object_get_string(spec);
            if(*s)matched.emplace_back(s);
        }else if(json_object_is_type(spec,json_type_array)){
            for(size_t i=0;i<alen(spec)&&matched.size()<8;i++){
                J *item=at(spec,i);
                if(json_object_is_type(item,json_type_string)){
                    if(*json_object_get_string(item))matched.emplace_back(json_object_get_string(item));
                }else if(json_object_is_type(item,json_type_object)){
                    const char *pat=sget(item,"pattern");
                    const char *cmd=sget(item,"command");
                    if(*cmd&&(!*pat||glob_match(pat,target,false)))matched.emplace_back(cmd);
                }
            }
        }
        if(matched.empty())return;
        if(workspace_scoped&&!app.yes&&!app.workspace_hooks_approved){
            std::string preview=path+"\n"+matched.front();
            if(!approve(&app,preview.c_str(),Operation::Shell,tr("运行项目钩子")))return;
            app.workspace_hooks_approved=true;
        }
        for(auto &m:matched)cmds.push_back(std::move(m));
    };
    if(*app.config_dir)parse_file(std::string(app.config_dir)+"/hooks.json",false);
    if(*app.root)parse_file(std::string(app.root)+"/.desk-agent/hooks.json",true);
    return cmds;
}

bool builtin_slash_name(std::string_view name){
    static constexpr std::string_view reserved[]={
        "help","status","clear","undo","compact","paste","model",
        "reasoning","approval","perm","mcp","new","commands"
    };
    for(auto r:reserved)if(name==r)return true;
    return false;
}

bool valid_command_slug(std::string_view name){
    if(name.empty()||name.size()>48||builtin_slash_name(name))return false;
    return std::all_of(name.begin(),name.end(),[](char c){
        auto u=static_cast<unsigned char>(c);
        return std::isalnum(u)||c=='-'||c=='_';
    });
}
} // namespace

bool run_pre_shell_hook(App &app,const char *command,std::string &blocked_reason){
    auto hooks=collect_hook_commands(app,"pre_shell",command);
    if(hooks.empty())return true;
    App probe{};
    snprintf(probe.root,sizeof probe.root,"%s",app.root);
    probe.sandbox=app.sandbox;
    set_temp_env("DESK_HOOK_CMD",command);
    for(const auto &hcmd:hooks){
        JsonPtr res(run_command(&probe,hcmd.c_str(),15000));
        if(!res||iget(res.get(),"exit_code",-1)!=0){
            std::string out=res?sget(res.get(),"output"):"";
            while(!out.empty()&&(out.back()=='\n'||out.back()=='\r'))out.pop_back();
            blocked_reason=out.empty()?std::string(tr("pre_shell 钩子拦截了此次命令。")):out;
            set_temp_env("DESK_HOOK_CMD",nullptr);
            return false;
        }
    }
    set_temp_env("DESK_HOOK_CMD",nullptr);
    return true;
}

void run_post_edit_hook(App &app,const char *rel_path,J *result){
    if(!result||!rel_path||!*rel_path)return;
    auto hooks=collect_hook_commands(app,"post_edit",rel_path);
    if(hooks.empty())return;
    App probe{};
    snprintf(probe.root,sizeof probe.root,"%s",app.root);
    probe.sandbox=app.sandbox;
    set_temp_env("DESK_HOOK_FILE",rel_path);
    int final_code=0;
    std::string combined;
    for(const auto &hcmd:hooks){
        JsonPtr res(run_command(&probe,hcmd.c_str(),15000));
        int code=res?iget(res.get(),"exit_code",-1):-1;
        if(code!=0&&final_code==0)final_code=code;
        std::string_view out=res?sget(res.get(),"output"):"";
        if(!out.empty()&&combined.size()<4096){
            if(!combined.empty())combined+='\n';
            size_t take=std::min<size_t>(out.size(),4096-combined.size());
            while(take>0&&(static_cast<unsigned char>(out[take])&0xc0)==0x80)--take;
            combined.append(out.substr(0,take));
        }
    }
    set_temp_env("DESK_HOOK_FILE",nullptr);
    put(result,"hook_exit_code",json_object_new_int(final_code));
    if(!combined.empty())put(result,"hook_output",str(combined.c_str()));
}

std::vector<CustomCommand> list_custom_commands(const App &app){
    std::vector<CustomCommand> out;
    std::set<std::string> seen;
    auto scan=[&](const std::string &dir,const char *scope){
        std::error_code ec;
        if(dir.empty()||!fs::is_directory(fs_path(dir),ec)||ec)return;
        std::vector<fs::path> files;
        for(fs::directory_iterator it(fs_path(dir),ec),end;it!=end&&!ec;it.increment(ec)){
            if(path_link(it->path()))continue;
            auto st=fs::symlink_status(it->path(),ec);
            if(!ec&&fs::is_regular_file(st)&&path_text(it->path().extension())==".md")
                files.push_back(it->path());
        }
        std::sort(files.begin(),files.end());
        for(const auto &fp:files){
            std::string stem=path_text(fp.stem());
            if(!valid_command_slug(stem)||!seen.insert(stem).second)continue;
            size_t len=0;
            std::string full=path_text(fp);
            char *bytes=read_bytes(full.c_str(),32768,&len);
            if(!bytes)continue;
            std::string summary;
            if(!memchr(bytes,0,len)&&valid_utf8(std::string_view(bytes,len))){
                std::string_view text(bytes,len);
                for(size_t pos=0;pos<text.size();){
                    auto nl=text.find('\n',pos);
                    std::string_view line=nl==std::string_view::npos?text.substr(pos):text.substr(pos,nl-pos);
                    pos=nl==std::string_view::npos?text.size():nl+1;
                    while(!line.empty()&&(line.front()==' '||line.front()=='#'||line.front()=='\t'))line.remove_prefix(1);
                    while(!line.empty()&&(line.back()=='\r'||line.back()==' '))line.remove_suffix(1);
                    if(!line.empty()){
                        size_t take=std::min<size_t>(line.size(),72);
                        while(take>0&&(static_cast<unsigned char>(line[take])&0xc0)==0x80)--take;
                        summary.assign(line.substr(0,take));
                        if(take<line.size())summary+="…";
                        break;
                    }
                }
            }
            free(bytes);
            out.push_back({std::move(stem),scope,std::move(summary)});
        }
    };
    if(*app.root){
        scan(std::string(app.root)+"/.deskcode/commands","workspace");
        scan(std::string(app.root)+"/.desk-agent/commands","workspace");
        scan(std::string(app.root)+"/.claude/commands","compat");
    }
    if(*app.config_dir)scan(std::string(app.config_dir)+"/commands","user");
    return out;
}

bool expand_custom_command(const App &app,std::string_view input,std::string &expanded,std::string &label){
    if(input.size()<2||input.front()!='/'||input[1]=='/')return false;
    size_t sp=1;
    while(sp<input.size()&&!std::isspace(static_cast<unsigned char>(input[sp])))sp++;
    std::string_view name=input.substr(1,sp-1);
    if(!valid_command_slug(name))return false;
    std::string_view rest=sp<input.size()?input.substr(sp):std::string_view{};
    while(!rest.empty()&&std::isspace(static_cast<unsigned char>(rest.front())))rest.remove_prefix(1);

    std::vector<std::string> dirs;
    if(*app.root){
        dirs.push_back(std::string(app.root)+"/.deskcode/commands");
        dirs.push_back(std::string(app.root)+"/.desk-agent/commands");
        dirs.push_back(std::string(app.root)+"/.claude/commands");
    }
    if(*app.config_dir)dirs.push_back(std::string(app.config_dir)+"/commands");

    for(const auto &dir:dirs){
        fs::path fp=fs_path(dir)/(std::string(name)+".md");
        if(path_link(fp))continue;
        size_t len=0;
        std::string full=path_text(fp);
        char *bytes=read_bytes(full.c_str(),32768,&len);
        if(!bytes)continue;
        bool ok=!memchr(bytes,0,len)&&valid_utf8(std::string_view(bytes,len));
        std::string tpl=ok?std::string(bytes,len):std::string();
        free(bytes);
        if(!ok||tpl.empty())continue;

        bool replaced=false;
        const std::string_view placeholder="$ARGUMENTS";
        for(size_t pos=0;(pos=tpl.find(placeholder,pos))!=std::string::npos;){
            tpl.replace(pos,placeholder.size(),rest);
            pos+=rest.size();
            replaced=true;
        }
        if(!replaced&&!rest.empty()){
            tpl+="\n\nArguments: ";
            tpl.append(rest);
        }
        expanded=std::move(tpl);
        label=std::string(input);
        return true;
    }
    return false;
}
