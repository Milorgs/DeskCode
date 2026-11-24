#include "desk.hpp"
#include "mcp.hpp"
#include "tasks.hpp"
#include "ui.hpp"
#include <openssl/evp.h>
#include <sys/stat.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <cctype>
#include <errno.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmaybe-uninitialized"
#endif
#include <regex>
static bool compile_search_regex(const char *pattern,bool ci,std::regex &out){
    try{
        auto flags=std::regex::ECMAScript|std::regex::optimize;
        if(ci)flags|=std::regex::icase;
        out=std::regex(pattern,flags);
        return true;
    }catch(const std::regex_error &){
        return false;
    }
}
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif
char *sha256(const void *data,size_t len){
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int n=0;
    if(!EVP_Digest(data,len,hash,&n,EVP_sha256(),NULL))return NULL;
    char *out=static_cast<char *>(malloc(n*2+1));
    if(!out)return NULL;
    for(unsigned int i=0;i<n;i++)snprintf(out+i*2,3,"%02x",hash[i]);
    return out;
}
int resolve_file(App *a,const char *path,char out[PATH_MAX]){
    fs::path input=fs_path(path),root=fs_path(a->root);
    for(const auto &part:input)if(part==".."){seterr(a,tr("不接受含 .. 的文件路径。"));return -1;}
#ifdef _WIN32
    // Reject alternate streams, device namespaces, aliases and drive-relative paths.
    std::string raw=path;
    if(raw.rfind("\\\\",0)==0||raw.rfind("//",0)==0||(input.has_root_name()&&!input.is_absolute())){seterr(a,tr("文件路径无效。"));return -1;}
    for(const auto &part:input.relative_path()){
        std::string name=path_text(part);if(name.empty()||name==".")continue;
        if(name.find(':')!=std::string::npos||name.back()=='.'||name.back()==' '){seterr(a,tr("文件路径无效。"));return -1;}
        std::string base=name.substr(0,name.find('.'));for(char &c:base)c=static_cast<char>(toupper(static_cast<unsigned char>(c)));
        if(base=="CON"||base=="PRN"||base=="AUX"||base=="NUL"||(base.size()==4&&(base.rfind("COM",0)==0||base.rfind("LPT",0)==0)&&base[3]>='0'&&base[3]<='9')){seterr(a,tr("文件路径无效。"));return -1;}
    }
#endif
    fs::path candidate=input.is_absolute()?input:root/input;
    std::error_code ec;
#ifdef _WIN32
    fs::path current;
    for(const auto &part:candidate){current/=part;if(path_link(current)){seterr(a,tr("文件工具只能访问工作目录。"));return -1;}}
#endif
    fs::path resolved=fs::weakly_canonical(candidate,ec);
    if(ec){seterr(a,tr("无法解析文件路径。"));return -1;}
    if(!path_inside(root,resolved)){seterr(a,tr("文件工具只能访问工作目录。"));return -1;}
    auto config=fs::weakly_canonical(fs_path(a->config_dir),ec);
    if(!ec&&path_inside(config,resolved)){seterr(a,tr("文件工具不访问模型配置与密钥。"));return -1;}
    for(const auto &part:resolved.lexically_relative(root)){
        std::string name=path_text(part);
#ifdef _WIN32
        for(char &c:name)c=static_cast<char>(tolower(static_cast<unsigned char>(c)));
#endif
        if(name==".git"||name==".desk-agent"||name==".ssh"||name==".gnupg"||(name.rfind(".env",0)==0&&(name.size()==4||name[4]=='.')&&name!=".env.example")){
            seterr(a,tr("文件工具不访问凭据、Git 内部文件或备份。"));return -1;
        }
    }
    std::string text=path_text(resolved);
    if(text.size()>=PATH_MAX){seterr(a,tr("文件路径过长。"));return -1;}
    memcpy(out,text.c_str(),text.size()+1);return 0;
}
static bool utf8_text(const char *bytes,size_t n){
    return !memchr(bytes,0,n)&&valid_utf8(std::string_view(bytes,n));
}
static bool search_skip(const char *name);
static J *file_read(App *a,const char *path,J *args=nullptr){
    char full[PATH_MAX];
    if(resolve_file(a,path,full))return err(a->error);
    J *start_obj=args?get(args,"start_line"):nullptr;
    J *end_obj=args?get(args,"end_line"):nullptr;
    std::error_code status_ec;
    auto st=fs::symlink_status(fs_path(full),status_ec);
    if(!status_ec){
        if(!fs::exists(st))return err(tr("文件不存在，请先用 list_files 或 search_text 确认路径。"));
        if(fs::is_directory(st))return err(tr("目标是文件夹，请改用 list_files 查看目录内容。"));
        if(!start_obj&&!end_obj&&fs::is_regular_file(st)){
            auto fsize=fs::file_size(fs_path(full),status_ec);
            if(!status_ec&&fsize>FILE_LIMIT){
                char msg[256];
                snprintf(msg,sizeof msg,tr("文件大小为 %llu 字节，超过 256 KB 限制；请传入 start_line 和 end_line 分段读取（最大 16 MB）。"),static_cast<unsigned long long>(fsize));
                J *oversized=err(msg);
                put(oversized,"size",json_object_new_int64(static_cast<int64_t>(fsize)));
                return oversized;
            }
        }
    }
    if(!start_obj&&!end_obj){
        size_t n=0;
        char *bytes=read_bytes(full,FILE_LIMIT,&n);
        if(!bytes)return err(tr("无法读取文件，文件可能不是普通文件或超过 256 KB。"));
        if(!utf8_text(bytes,n)){
            free(bytes);
            return err(tr("只支持 UTF-8 文本文件。"));
        }
        char *hash=sha256(bytes,n);
        J *result=obj();
        put(result,"content",json_object_new_string_len(bytes,(int)n));
        put(result,"sha256",str(hash));
        free(hash);
        free(bytes);
        return result;
    }
    int64_t start_line=start_obj?json_object_get_int64(start_obj):1;
    int64_t end_line=end_obj?json_object_get_int64(end_obj):INT64_MAX;
    if(start_line<1||end_line<start_line){
        return err(tr("行号范围无效：start_line 和 end_line 须为正整数，且 start_line 不大于 end_line。"));
    }
    size_t n=0;
    char *bytes=read_bytes(full,SLICED_FILE_LIMIT,&n);
    if(!bytes)return err(tr("无法读取文件，文件可能不是普通文件或超过 16 MB。"));
    if(!utf8_text(bytes,n)){
        free(bytes);
        return err(tr("只支持 UTF-8 文本文件。"));
    }
    size_t total_lines=0,slice_begin=n,slice_end=n;
    int64_t actual_end=0;
    for(size_t pos=0;pos<n;){
        total_lines++;
        const int64_t current=static_cast<int64_t>(total_lines);
        const char *nl=static_cast<const char *>(memchr(bytes+pos,'\n',n-pos));
        const size_t next=nl?static_cast<size_t>(nl-bytes)+1:n;
        if(current>=start_line&&current<=end_line){
            if(slice_begin==n)slice_begin=pos;
            slice_end=next;
            actual_end=current;
        }
        pos=next;
    }
    const size_t slice_len=slice_begin<n?slice_end-slice_begin:0;
    if(slice_len>FILE_LIMIT){
        free(bytes);
        return err(tr("所选行范围的内容超过 256 KB，请缩小 start_line..end_line 范围。"));
    }
    J *result=obj();
    put(result,"content",json_object_new_string_len(slice_len?bytes+slice_begin:"",static_cast<int>(slice_len)));
    put(result,"sliced",json_object_new_boolean(1));
    put(result,"start_line",json_object_new_int64(start_line));
    put(result,"end_line",json_object_new_int64(actual_end));
    put(result,"total_lines",json_object_new_int64(static_cast<int64_t>(total_lines)));
    if(n<=EDIT_FILE_LIMIT){
        char *hash=sha256(bytes,n);
        put(result,"sha256",str(hash));
        free(hash);
    }
    free(bytes);
    return result;
}
static J *file_read_editable(App *a,const char *path){
    char full[PATH_MAX];
    if(resolve_file(a,path,full))return err(a->error);
    std::error_code status_ec;
    auto st=fs::symlink_status(fs_path(full),status_ec);
    if(!status_ec){
        if(!fs::exists(st))return err(tr("文件不存在，请先用 list_files 或 search_text 确认路径。"));
        if(fs::is_directory(st))return err(tr("目标是文件夹，请改用 list_files 查看目录内容。"));
        if(fs::is_regular_file(st)){
            auto fsize=fs::file_size(fs_path(full),status_ec);
            if(!status_ec&&fsize>EDIT_FILE_LIMIT)return err(tr("无法读取原文件或原文件过大。"));
        }
    }
    size_t n=0;
    char *bytes=read_bytes(full,EDIT_FILE_LIMIT,&n);
    if(!bytes)return err(tr("无法读取原文件或原文件过大。"));
    if(!utf8_text(bytes,n)){
        free(bytes);
        return err(tr("只支持 UTF-8 文本文件。"));
    }
    char *hash=sha256(bytes,n);
    J *result=obj();
    put(result,"content",json_object_new_string_len(bytes,static_cast<int>(n)));
    put(result,"sha256",str(hash));
    free(hash);
    free(bytes);
    return result;
}
static J *file_list(App *a,const char *path,J *args=nullptr){
    char full[PATH_MAX];
    if(resolve_file(a,path,full))return err(a->error);
    int max_depth=1;
    if(args&&get(args,"max_depth")){
        int64_t d=json_object_get_int64(get(args,"max_depth"));
        if(d<1||d>4)return err(tr("max_depth 须为 1..4 的整数。"));
        max_depth=static_cast<int>(d);
    }
    std::error_code ec;
    fs::path base=fs_path(full);
    if(!fs::is_directory(base,ec)||ec)return err(tr("无法读取文件夹。"));
    struct ListedEntry {
        std::string name;
        bool directory=false,symlink=false;
        bool has_size=false;
        std::uint64_t size=0;
    };
    std::vector<ListedEntry> collected;
    bool truncated=false;
    auto walk=[&](auto &self,const fs::path &dir,int depth)->void{
        std::error_code iter_ec;
        fs::directory_iterator directory(dir,iter_ec);
        if(iter_ec)return;
        std::vector<fs::directory_entry> items;
        for(auto it=directory;it!=fs::directory_iterator{};it.increment(iter_ec)){
            if(iter_ec)break;
            items.push_back(*it);
        }
        std::sort(items.begin(),items.end(),[](const fs::directory_entry &lhs,const fs::directory_entry &rhs){
            std::error_code lec,rec;
            const bool ldir=lhs.is_directory(lec)&&!path_link(lhs.path());
            const bool rdir=rhs.is_directory(rec)&&!path_link(rhs.path());
            if(ldir!=rdir)return ldir>rdir;
            return path_text(lhs.path().filename())<path_text(rhs.path().filename());
        });
        for(const auto &item:items){
            if(collected.size()>=200){truncated=true;return;}
            std::string leaf=path_text(item.path().filename());
            if(depth>1&&search_skip(leaf.c_str()))continue;
            std::string child=path_text(item.path());
            char resolved[PATH_MAX];
            if(resolve_file(a,child.c_str(),resolved))continue;
            std::error_code item_ec;
            const bool link=path_link(item.path());
            const bool is_dir=!link&&item.is_directory(item_ec);
            std::string rel=max_depth==1?leaf:path_text(item.path().lexically_relative(base));
            ListedEntry entry{std::move(rel),is_dir,link,false,0};
            if(!link&&!is_dir&&item.is_regular_file(item_ec)){
                auto bytes=item.file_size(item_ec);
                if(!item_ec){entry.has_size=true;entry.size=bytes;}
            }
            collected.push_back(std::move(entry));
            if(is_dir&&depth<max_depth){
                self(self,item.path(),depth+1);
                if(truncated)return;
            }
        }
    };
    walk(walk,base,1);
    J *result=obj(),*entries=arr();
    for(const auto &item:collected){
        J *e=obj();
        put(e,"name",str(item.name.c_str()));
        put(e,"directory",json_object_new_boolean(item.directory));
        put(e,"symlink",json_object_new_boolean(item.symlink));
        if(item.has_size)put(e,"size",json_object_new_int64(static_cast<int64_t>(std::min<std::uint64_t>(item.size,INT64_MAX))));
        add(entries,e);
    }
    put(result,"possibly_truncated",json_object_new_boolean(truncated));
    put(result,"entries",entries);
    return result;
}
struct Search {
    App *app;
    const char *query;
    bool case_insensitive=false;
    bool use_regex=false;
    const std::regex *re=nullptr;
    const char *file_pattern=nullptr;
    GitIgnore gitignore{};
    J *matches;
    size_t entries=0,files=0,bytes=0,skipped=0;
    double until;
    bool truncated=false;
};
static bool search_limit(Search *s){
    if(interrupted||s->entries>=40000||s->files>=4000||s->bytes>=32*1024*1024||alen(s->matches)>=120||monotime()>s->until)s->truncated=true;
    return s->truncated;
}
static bool search_skip(const char *name){
    return !strcmp(name,"node_modules")||!strcmp(name,"build")||!strcmp(name,"dist")||!strcmp(name,"target")||!strcmp(name,".venv")||!strcmp(name,"venv")||!strcmp(name,"__pycache__")||!strcmp(name,".cache")||!strcmp(name,"CMakeFiles")||!strcmp(name,".next")||!strcmp(name,"coverage");
}
static void emit_search_match(Search *s,const char *relative,size_t line,char *line_start,char *found,char *end){
    J *m=obj();
    put(m,"path",str(relative));
    put(m,"line",json_object_new_int64(static_cast<int64_t>(line)));
    char *start=line_start;
    if(found-line_start>120){
        start=found-120;
        while(((unsigned char)*start&0xc0)==0x80)start++;
    }
    size_t length=static_cast<size_t>(end-start);
    if(length>360){
        length=360;
        while(length&&((unsigned char)start[length]&0xc0)==0x80)length--;
    }
    put(m,"text",json_object_new_string_len(start,static_cast<int>(length)));
    put(m,"line_truncated",json_object_new_boolean(start!=line_start||start+length<end));
    add(s->matches,m);
}
static void search_walk(Search *s,const char *path,int depth){
    if(search_limit(s))return;
    s->entries++;
    std::error_code ec;fs::path source=fs_path(path);
    if(path_link(source)){s->skipped++;return;}
    auto status=fs::symlink_status(source,ec);if(ec){s->skipped++;return;}
    char full[PATH_MAX];if(resolve_file(s->app,path,full)){s->skipped++;return;}
    const fs::path root_path=fs_path(s->app->root);
    if(fs::is_directory(status)){
        if(depth>=24){s->truncated=true;return;}
        fs::directory_iterator directory(fs_path(full),ec);if(ec){s->skipped++;return;}
        for(auto it=directory;it!=fs::directory_iterator{}&&!search_limit(s);it.increment(ec)){
            if(ec){s->skipped++;break;}
            std::string name=path_text(it->path().filename());if(search_skip(name.c_str())){s->skipped++;continue;}
            std::error_code child_ec;
            const bool child_dir=!path_link(it->path())&&it->is_directory(child_ec);
            std::string child_rel=path_text(it->path().lexically_relative(root_path));
            if(!child_rel.empty()&&s->gitignore.ignored(child_rel,child_dir)){s->skipped++;continue;}
            std::string child=path_text(it->path());search_walk(s,child.c_str(),depth+1);
        }
        return;
    }
    if(!fs::is_regular_file(status)){s->skipped++;return;}
    std::string rel=path_text(fs_path(full).lexically_relative(root_path));
    if(s->file_pattern&&*s->file_pattern&&!glob_match(s->file_pattern,rel,s->case_insensitive)){s->skipped++;return;}
    s->files++;
    size_t n=0;
    char *data=read_bytes(full,FILE_LIMIT,&n);
    if(!data){s->skipped++;return;}
    s->bytes+=n;
    if(!utf8_text(data,n)){s->skipped++;free(data);return;}
    char *const limit=data+n;
    if(s->use_regex&&s->re){
        size_t line=1;
        for(char *p=data;p<limit&&!search_limit(s);line++){
            char *end=static_cast<char *>(memchr(p,'\n',static_cast<size_t>(limit-p)));
            if(!end)end=limit;
            char *clean_end=(end>p&&end[-1]=='\r')?end-1:end;
            const char *scan_end=(clean_end-p>4096)?p+4096:clean_end;
            std::cmatch cm;
            if(std::regex_search(static_cast<const char *>(p),scan_end,cm,*s->re)){
                char *found=const_cast<char *>(cm[0].first);
                emit_search_match(s,rel.c_str(),line,p,found,clean_end);
            }
            if(end==limit)break;
            p=end+1;
        }
        free(data);
        return;
    }
    const std::string_view query(s->query);
    size_t line=1;
    for(char *p=data;p<limit&&!search_limit(s);line++){
        size_t at=std::string_view::npos;
        const std::string_view window(p,(size_t)(limit-p));
        if(!s->case_insensitive){
            at=window.find(query);
        }else{
            auto it=std::search(window.begin(),window.end(),query.begin(),query.end(),[](unsigned char lhs,unsigned char rhs){
                return std::tolower(lhs)==std::tolower(rhs);
            });
            if(it!=window.end())at=static_cast<size_t>(it-window.begin());
        }
        if(at==std::string_view::npos)break;
        char *found=p+at;
        for(char *nl;(nl=static_cast<char *>(memchr(p,'\n',(size_t)(found-p))));p=nl+1)line++;
        char *end=static_cast<char *>(memchr(found,'\n',(size_t)(limit-found)));
        if(!end)end=limit;
        emit_search_match(s,rel.c_str(),line,p,found,end);
        if(end==limit)break;
        p=end+1;
    }
    free(data);
}
static J *file_search(App *a,J *args){
    const char *query=sget(args,"query");
    if(!*query||strlen(query)>256||strchr(query,'\n')||strchr(query,'\r'))return err(tr("搜索内容须为 1..256 字节的单行文本，按字面匹配。"));
    const char *file_pattern=sget(args,"file_pattern");
    if(*file_pattern&&strlen(file_pattern)>128)return err(tr("file_pattern 长度不能超过 128 字节。"));
    const bool use_regex=bget(args,"regex");
    const bool ci=bget(args,"case_insensitive");
    std::regex compiled;
    if(use_regex&&!compile_search_regex(query,ci,compiled)){
        return err(tr("正则表达式语法无效。"));
    }
    char full[PATH_MAX];
    if(resolve_file(a,sget(args,"path"),full))return err(a->error);
    std::error_code ec;auto status=fs::status(fs_path(full),ec);
    if(ec||(!fs::is_directory(status)&&!fs::is_regular_file(status)))return err(tr("搜索路径须为存在的文件或目录。"));
    Search s={.app=a,.query=query,.case_insensitive=ci,.use_regex=use_regex,.re=use_regex?&compiled:nullptr,.file_pattern=*file_pattern?file_pattern:nullptr,.matches=arr(),.until=monotime()+5};
    s.gitignore.load(a->root);
    search_walk(&s,full,0);
    if(interrupted){json_object_put(s.matches);return err(tr("搜索已取消。"));}
    J *r=obj();
    put(r,"matches",s.matches);
    put(r,"files_scanned",json_object_new_int64((int64_t)s.files));
    put(r,"skipped",json_object_new_int64((int64_t)s.skipped));
    put(r,"truncated",json_object_new_boolean(s.truncated));
    if(s.truncated)put(r,"hint",str(tr("达到搜索预算；缩小 path 后继续，结果并非完整列表。")));
    return r;
}
// A compact line diff for approval panels: common leading and trailing lines
// are trimmed to two lines of context, the changed middle is shown as -/+.
// Good enough to review an edit at a glance without a full LCS diff.
namespace {
std::vector<std::string_view> split_lines(std::string_view text){
    std::vector<std::string_view> lines;
    if(text.empty())return lines;
    size_t start=0;
    while(start<=text.size()){
        auto end=text.find('\n',start);
        auto line=text.substr(start,end==std::string_view::npos?std::string_view::npos:end-start);
        if(!line.empty()&&line.back()=='\r')line.remove_suffix(1);
        if(end==std::string_view::npos){if(!line.empty())lines.push_back(line);break;}
        lines.push_back(line);
        start=end+1;
        if(start==text.size())break;
    }
    return lines;
}
std::string diff_preview(std::string_view before,std::string_view after,size_t first_line,size_t &added,size_t &removed){
    const auto old_lines=split_lines(before),new_lines=split_lines(after);
    size_t prefix=0;
    while(prefix<old_lines.size()&&prefix<new_lines.size()&&old_lines[prefix]==new_lines[prefix])prefix++;
    size_t suffix=0;
    while(suffix<old_lines.size()-prefix&&suffix<new_lines.size()-prefix&&
          old_lines[old_lines.size()-1-suffix]==new_lines[new_lines.size()-1-suffix])suffix++;
    removed=old_lines.size()-prefix-suffix;
    added=new_lines.size()-prefix-suffix;
    constexpr size_t context=2,shown_limit=400;
    const size_t context_start=prefix>context?prefix-context:0;
    std::string out;
    char header[96];
    snprintf(header,sizeof header,tr("@@ 第 %zu 行 @@"),first_line+context_start);
    out+=ui_paint(Tone::Accent,header);
    size_t shown=0,hidden=0;
    auto emit=[&](Tone tone,const char *mark,std::string_view text){
        if(shown>=shown_limit){hidden++;return;}
        out+='\n';
        out+=ui_paint(tone,std::string(mark)+std::string(text));
        shown++;
    };
    for(size_t i=context_start;i<prefix;i++)emit(Tone::Dim,"  ",old_lines[i]);
    for(size_t i=prefix;i<old_lines.size()-suffix;i++)emit(Tone::Error,"- ",old_lines[i]);
    for(size_t i=prefix;i<new_lines.size()-suffix;i++)emit(Tone::Success,"+ ",new_lines[i]);
    const size_t tail_start=new_lines.size()-suffix;
    for(size_t i=tail_start;i<std::min(new_lines.size(),tail_start+context);i++)emit(Tone::Dim,"  ",new_lines[i]);
    if(hidden){
        char note[96];
        snprintf(note,sizeof note,tr("… 其余 %zu 行未显示"),hidden);
        out+='\n';
        out+=ui_paint(Tone::Dim,note);
    }
    return out;
}
}
static J *file_write(App *a,J *args,const std::string *edit_diff=nullptr,size_t edit_added=0,size_t edit_removed=0){
    const char *path=sget(args,"path"),*content=sget(args,"content");
    const size_t byte_limit=edit_diff?EDIT_FILE_LIMIT:FILE_LIMIT;
    size_t n=strlen(content);
    if(n>byte_limit)return err(edit_diff?tr("修改后的文件超过 4 MB 限制。"):tr("文件超过 256 KB 限制。"));
    char full[PATH_MAX];
    if(resolve_file(a,path,full))return err(a->error);
    std::error_code ec;auto status=fs::symlink_status(fs_path(full),ec);
    bool exists=fs::exists(status);
    if(ec&&ec!=std::errc::no_such_file_or_directory)return err(tr("无法访问目标文件。"));
    if(exists&&!fs::is_regular_file(status))return err(tr("目标不是普通文件。"));
    unsigned int mode=exists?static_cast<unsigned int>(status.permissions())&0777:0644;
    size_t old_len=0;
    char *old=exists?read_bytes(full,byte_limit,&old_len):NULL;
    if(exists&&!old)return err(tr("无法读取原文件或原文件过大。"));
    char *hash=exists?sha256(old,old_len):NULL;
    J *expected=get(args,"expected_sha256");
    bool matches=exists?(expected&&json_object_is_type(expected,json_type_string)&&hash&&!strcmp(hash,json_object_get_string(expected))):!expected;
    free(hash);
    if(!matches){
        free(old);
        return err(tr("文件已变化，或覆盖前未读取，请重新读取文件。"));
    }
    size_t added=edit_added,removed=edit_removed;
    std::string diff;
    if(edit_diff)diff=*edit_diff;
    else diff=diff_preview(exists?std::string_view(old,old_len):std::string_view(),std::string_view(content,n),1,added,removed);
    char *description=NULL;
    if(asprintf(&description,tr("%s %s（+%zu −%zu 行）\n%s"),edit_diff?tr("修改"):exists?tr("覆盖"):tr("新建"),path,added,removed,diff.c_str())<0){
        free(old);
        return err(tr("无法分配写入预览内存，原文件未修改。"));
    }
    int allowed=approve(a,description,Operation::FileEdit,edit_diff?tr("编辑文件"):tr("写入文件"));
    free(description);
    if(!allowed){
        free(old);
        J *r=err(tr("用户拒绝写入，不要绕过。"));
        put(r,"denied",json_object_new_boolean(1));
        return r;
    }
    size_t latest_len=0;
    char *latest=read_bytes(full,byte_limit,&latest_len);
    bool unchanged=exists?(latest&&latest_len==old_len&&!memcmp(old,latest,old_len)):(!latest&&errno==ENOENT);
    free(latest);
    if(!unchanged){
        free(old);
        return err(tr("确认期间文件已变化，未写入。"));
    }
    char backup[PATH_MAX]="";
    if(exists){
        char directory[PATH_MAX],id[33];
        id_string(id);
        if(path_suffix(directory,a->root,"/.desk-agent/backups")||mkdirs(directory,0700)||snprintf(backup,sizeof backup,"%s/%s.bak",directory,id)>=(int)sizeof backup||atomic_file(backup,old,old_len,0600)){
            free(old);
            return err(tr("创建备份失败，未修改原文件。"));
        }
    }
    free(old);
    char parent[PATH_MAX];
    strcpy(parent,full);
    char *slash=strrchr(parent,'/');
    if(slash&&slash!=parent)*slash=0;
    if(mkdirs(parent,0755)||atomic_file(full,content,n,mode))return err(tr("写入文件失败。"));
    latest=read_bytes(full,byte_limit,&latest_len);
    bool verified=latest&&latest_len==n&&!memcmp(latest,content,n);
    free(latest);
    if(!verified)return err(tr("已写入，但回读核对不一致；请重新读取当前文件，不要直接重试覆盖。"));
    std::string rel_written=path_text(fs_path(full).lexically_relative(fs_path(a->root)));
    if(!rel_written.empty())record_file_change(a,rel_written.c_str(),backup,exists,mode);
    J *result=obj();
    put(result,"written",str(path));
    hash=sha256(content,n);
    put(result,"sha256",str(hash));
    free(hash);
    put(result,"backup",*backup?str(backup):NULL);
    put(result,"verified",json_object_new_boolean(1));
    if(!rel_written.empty())run_post_edit_hook(*a,rel_written.c_str(),result);
    return result;
}
static std::string to_crlf(std::string_view input){
    std::string out;
    out.reserve(input.size()+16);
    for(size_t i=0;i<input.size();++i){
        if(input[i]=='\n'&&(i==0||input[i-1]!='\r'))out+="\r\n";
        else out+=input[i];
    }
    return out;
}
static std::vector<size_t> find_occurrences(std::string_view text,std::string_view needle){
    std::vector<size_t> offsets;
    if(needle.empty()||needle.size()>text.size())return offsets;
    for(size_t pos=0;(pos=text.find(needle,pos))!=std::string_view::npos;pos+=1){
        offsets.push_back(pos);
        if(offsets.size()>=64)break;
    }
    return offsets;
}
static J *file_edit(App *a,J *args){
    const char *before_raw=sget(args,"old_text"),*after_raw=sget(args,"new_text");
    if(!*before_raw||!strcmp(before_raw,after_raw))return err(tr("old_text 不能为空，且新旧文本须不同。"));
    JsonPtr original(file_read_editable(a,sget(args,"path")));
    J *read=original.get();
    if(get(read,"error"))return original.release();
    if(strcmp(sget(read,"sha256"),sget(args,"expected_sha256"))){
        J *mismatch=err(tr("文件已变化，或修改前未读取，请重新读取文件。"));
        put(mismatch,"current_sha256",str(sget(read,"sha256")));
        return mismatch;
    }
    const std::string_view content(sget(read,"content"));
    std::string before(before_raw),after(after_raw);
    auto matches=find_occurrences(content,before);
    // Normalize LF -> CRLF automatically when editing a CRLF file with LF text.
    if(matches.empty()&&content.find("\r\n")!=content.npos&&before.find('\n')!=before.npos&&before.find("\r\n")==before.npos){
        std::string candidate_before=to_crlf(before);
        auto crlf_matches=find_occurrences(content,candidate_before);
        if(crlf_matches.size()==1){
            before=std::move(candidate_before);
            after=to_crlf(after);
            matches=std::move(crlf_matches);
        }
    }
    if(matches.size()!=1){
        J *failure=err(tr("old_text 必须在文件中精确匹配一次；请加入相邻上下文后重试。"));
        put(failure,"match_count",json_object_new_int64(static_cast<int64_t>(matches.size())));
        if(matches.size()>1){
            J *lines=arr();
            size_t line=1,cursor=0;
            for(size_t i=0;i<std::min<size_t>(matches.size(),10);++i){
                for(;cursor<matches[i];++cursor)if(content[cursor]=='\n')++line;
                add(lines,json_object_new_int64(static_cast<int64_t>(line)));
            }
            put(failure,"match_lines",lines);
            put(failure,"hint",str("Multiple matches found at match_lines; include 2-3 surrounding lines in old_text and new_text to disambiguate."));
        }else{
            put(failure,"hint",str("No exact match found. Call read_file with start_line..end_line around the target to check exact indentation and characters."));
        }
        return failure;
    }
    size_t offset=matches[0],size=content.size()-before.size()+after.size();
    if(size>EDIT_FILE_LIMIT)return err(tr("修改后的文件超过 4 MB 限制。"));
    std::string updated(content);
    updated.replace(offset,before.size(),after);
    size_t first_line=1;
    for(size_t i=0;i<offset;i++)if(content[i]=='\n')first_line++;
    size_t added=0,removed=0;
    const std::string preview=diff_preview(before,after,first_line,added,removed);
    JsonPtr write(copy(args));
    json_object_object_del(write.get(),"old_text");
    json_object_object_del(write.get(),"new_text");
    put(write.get(),"content",str(updated.c_str()));
    J *result=file_write(a,write.get(),&preview,added,removed);
    if(result&&bget(result,"verified"))put(result,"line",json_object_new_int64(static_cast<int64_t>(first_line)));
    return result;
}
static J *file_multi_edit(App *a,J *args){
    J *edits=get(args,"edits");
    const size_t count=alen(edits);
    if(count<1||count>20)return err(tr("multi_edit 的 edits 须包含 1 到 20 处修改。"));
    JsonPtr original(file_read_editable(a,sget(args,"path")));
    J *read=original.get();
    if(get(read,"error"))return original.release();
    if(strcmp(sget(read,"sha256"),sget(args,"expected_sha256"))){
        J *mismatch=err(tr("文件已变化，或修改前未读取，请重新读取文件。"));
        put(mismatch,"current_sha256",str(sget(read,"sha256")));
        return mismatch;
    }
    std::string working(sget(read,"content"));
    std::string combined_preview;
    size_t total_added=0,total_removed=0,first_changed_line=1;
    for(size_t idx=0;idx<count;idx++){
        J *hunk=at(edits,idx);
        const char *before_raw=sget(hunk,"old_text"),*after_raw=sget(hunk,"new_text");
        if(!*before_raw||!strcmp(before_raw,after_raw)){
            J *bad=err(tr("old_text 不能为空，且新旧文本须不同。"));
            put(bad,"failed_edit_index",json_object_new_int64(static_cast<int64_t>(idx)));
            return bad;
        }
        std::string before(before_raw),after(after_raw);
        auto matches=find_occurrences(working,before);
        if(matches.empty()&&working.find("\r\n")!=working.npos&&before.find('\n')!=before.npos&&before.find("\r\n")==before.npos){
            std::string candidate=to_crlf(before);
            auto crlf_matches=find_occurrences(working,candidate);
            if(crlf_matches.size()==1){
                before=std::move(candidate);
                after=to_crlf(after);
                matches=std::move(crlf_matches);
            }
        }
        if(matches.size()!=1){
            J *failure=err(tr("old_text 必须在文件中精确匹配一次；请加入相邻上下文后重试。"));
            put(failure,"failed_edit_index",json_object_new_int64(static_cast<int64_t>(idx)));
            put(failure,"match_count",json_object_new_int64(static_cast<int64_t>(matches.size())));
            if(matches.size()>1){
                J *lines=arr();
                size_t line=1,cursor=0;
                for(size_t i=0;i<std::min<size_t>(matches.size(),10);++i){
                    for(;cursor<matches[i];++cursor)if(working[cursor]=='\n')++line;
                    add(lines,json_object_new_int64(static_cast<int64_t>(line)));
                }
                put(failure,"match_lines",lines);
            }
            return failure;
        }
        const size_t offset=matches[0];
        const size_t next_size=working.size()-before.size()+after.size();
        if(next_size>EDIT_FILE_LIMIT)return err(tr("修改后的文件超过 4 MB 限制。"));
        size_t line_no=1;
        for(size_t i=0;i<offset;i++)if(working[i]=='\n')line_no++;
        if(idx==0)first_changed_line=line_no;
        size_t h_add=0,h_rem=0;
        if(!combined_preview.empty())combined_preview+='\n';
        combined_preview+=diff_preview(before,after,line_no,h_add,h_rem);
        total_added+=h_add;
        total_removed+=h_rem;
        working.replace(offset,before.size(),after);
    }
    JsonPtr write(obj());
    put(write.get(),"path",str(sget(args,"path")));
    put(write.get(),"expected_sha256",str(sget(args,"expected_sha256")));
    put(write.get(),"content",str(working.c_str()));
    J *result=file_write(a,write.get(),&combined_preview,total_added,total_removed);
    if(result&&bget(result,"verified")){
        put(result,"line",json_object_new_int64(static_cast<int64_t>(first_changed_line)));
        put(result,"edits_applied",json_object_new_int64(static_cast<int64_t>(count)));
    }
    return result;
}
static void definition(J *tools,const char *name,const char *description,const char **fields,int count){
    J *t=obj(),*f=obj(),*p=obj(),*properties=obj(),*required=arr();
    put(t,"type",str("function"));
    put(f,"name",str(name));
    put(f,"description",str(description));
    put(p,"type",str("object"));
    put(p,"properties",properties);
    put(p,"required",required);
    put(p,"additionalProperties",json_object_new_boolean(0));
    for(int i=0;i<count;i++){
        J *value=obj();
        if(!strcmp(fields[i],"expected_sha256")&&!strcmp(name,"write_file")){
            J *types=arr();
            add(types,str("string"));
            add(types,str("null"));
            put(value,"type",types);
        }
        else put(value,"type",str("string"));
        const char *description=!strcmp(fields[i],"path")?"Workspace-relative file or directory path; use . for the workspace root.":
            !strcmp(fields[i],"expected_sha256")?"SHA256 from the latest read_file result; null only when creating a new file.":
            !strcmp(fields[i],"old_text")?"Exact original text, including whitespace. Must occur exactly once; include surrounding lines to disambiguate.":
            !strcmp(fields[i],"new_text")?"Replacement text; empty string deletes the matched text.":
            !strcmp(fields[i],"query")?"Single-line search text, or ECMAScript regular expression when regex=true (1..256 bytes).":
            !strcmp(fields[i],"url")?"HTTPS URL to fetch (or localhost HTTP for local debugging).":
            !strcmp(fields[i],"content")?"Complete UTF-8 file contents (up to 256 KB).":
            !strcmp(fields[i],"command")?
#ifdef _WIN32
            "One noninteractive Windows PowerShell command. Runs independently in the workspace; variables and directory changes do not persist.":
#else
            "One noninteractive bash command. Runs independently in the workspace; cd and exported variables do not persist.":
#endif
            !strcmp(fields[i],"app")?"Empty string lists applications; otherwise use an exact application name from the latest snapshot.":
            !strcmp(fields[i],"question")?"Specific codebase question to investigate with read-only tools (1..4000 bytes).":
            !strcmp(fields[i],"ref")?"Fresh control reference from the latest snapshot; invalid after any desktop action.":
            !strcmp(fields[i],"action")?"Exact action name advertised by the referenced control.":"Complete replacement text for the editable control.";
        put(value,"description",str(description));
        put(properties,fields[i],value);
        add(required,str(fields[i]));
    }
    put(f,"parameters",p);
    put(t,"function",f);
    add(tools,t);
}
static J *builtin_tools(){
    // Immutable catalog: construct once; callers receive their own JSON copies.
    static const JsonPtr catalog([]{
        J *tools=arr();
        const char *path[]={
            "path"
        }
        ,*write[]={
            "path","content","expected_sha256"
        }
        ,*cmd[]={
            "command"
        }
        ,*search[]={
            "path","query"
        }
        ,*edit[]={
            "path","old_text","new_text","expected_sha256"
        }
        ,*app[]={
            "app"
        }
        ,*act[]={
            "ref","action"
        }
        ,*text[]={
            "ref","text"
        }
        ,*research[]={
            "question"
        }
        ,*fetch_url[]={
            "url"
        }
        ,*multi_edit_req[]={
            "path","expected_sha256"
        }
        ,*web_q[]={
            "query"
        };
        definition(tools,"list_files","List up to 200 workspace entries sorted with directories first and byte sizes on files. Set optional max_depth (1..4) to inspect directory trees in one call.",path,1);
        {
            J *list_props=get(get(get(at(tools,0),"function"),"parameters"),"properties");
            J *depth=obj();
            put(depth,"type",str("integer"));
            put(depth,"description",str("Optional traversal depth (1..4, default 1)."));
            put(list_props,"max_depth",depth);
        }
        definition(tools,"search_text","Recursively locate text or regex matches with file paths and line numbers. Respects .gitignore and skips symlinks, protected files, dependency/build directories and non-UTF-8 or >256 KB files. Up to 120 matching lines, 4000 files, depth 24, 32 MB, 5 seconds; inspect truncated and skipped before claiming absence.",search,2);
        {
            J *search_props=get(get(get(at(tools,1),"function"),"parameters"),"properties");
            J *ci=obj(),*re=obj(),*fp=obj();
            put(ci,"type",str("boolean"));
            put(ci,"description",str("Optional case-insensitive matching (default false)."));
            put(search_props,"case_insensitive",ci);
            put(re,"type",str("boolean"));
            put(re,"description",str("Optional ECMAScript regular expression mode (default false)."));
            put(search_props,"regex",re);
            put(fp,"type",str("string"));
            put(fp,"description",str("Optional filename or path glob filter such as *.cpp, *.{ts,tsx}, or src/*.h."));
            put(search_props,"file_pattern",fp);
        }
        definition(tools,"read_file","Read a UTF-8 file (full file up to 256 KB with SHA256, or optional 1-based start_line..end_line slice from files up to 16 MB; full-file SHA256 is included up to 4 MB so edit_file works on sliced reads). Read before editing an existing file.",path,1);
        {
            J *read_props=get(get(get(at(tools,2),"function"),"parameters"),"properties");
            J *sline=obj(),*eline=obj();
            put(sline,"type",str("integer"));
            put(sline,"description",str("Optional 1-based first line to read (inclusive). Omit to read the full file."));
            put(eline,"type",str("integer"));
            put(eline,"description",str("Optional 1-based last line to read (inclusive). Omit to read through the end."));
            put(read_props,"start_line",sline);
            put(read_props,"end_line",eline);
        }
        definition(tools,"edit_file","Replace one unique exact text occurrence in an existing UTF-8 file (up to 4 MB). Prefer this for targeted edits. Supply SHA256 from read_file; if ambiguous, match_lines are returned so you can add surrounding context. Preserves other bytes and creates a backup.",edit,4);
        definition(tools,"write_file","Create a UTF-8 file or replace its entire contents. Prefer edit_file for small changes. Read existing file first and supply SHA256; null only for a new file. Original bytes are backed up and new bytes read back. Verify the task's actual behavior separately.",write,3);
        definition(tools,"run_shell",
#ifdef _WIN32
            "Execute noninteractive Windows PowerShell in the workspace (default 30s timeout, configurable up to 300s via timeout_ms). Use PowerShell syntax, not bash. There is no sandbox. Calls do not share variables or working-directory changes. Do not start persistent/background services. Inspect exit_code, timed_out and output to verify the result.",
#elif defined(__linux__)
            "Execute noninteractive bash in workspace (default 30s timeout, configurable up to 300s via timeout_ms; Landlock restricts writes to workspace and /tmp when enabled). Do not start persistent/background services. Separate calls do not preserve cd or variables. Inspect exit_code, timed_out and output; on failure diagnose and change approach rather than repeating. A successful command alone does not prove task completion.",
#else
            "Execute noninteractive bash in workspace (default 30s timeout, configurable up to 300s via timeout_ms), no sandbox. Do not start persistent/background services. Separate calls do not preserve cd or variables. Inspect exit_code, timed_out and output; on failure diagnose and change approach rather than repeating. A successful command alone does not prove task completion.",
#endif
            cmd,1);
        {
            J *shell_props=get(get(get(at(tools,5),"function"),"parameters"),"properties");
            J *timeout=obj();
            put(timeout,"type",str("integer"));
            put(timeout,"description",str("Optional command timeout in milliseconds (1000..300000, default 30000)."));
            put(shell_props,"timeout_ms",timeout);
            J *background=obj();
            put(background,"type",str("boolean"));
            put(background,"description",str("Optional: start a long-running command (dev server, watcher, long build or test run) in the background and return a shell_id at once; read its output with shell_output and stop it with shell_kill."));
            put(shell_props,"background",background);
        }
        definition(tools,"desktop_snapshot","Read apps with empty app, or up to 500 controls for an exact app name (with optional filter on control name/role). Text is sent to model API.",app,1);
        {
            J *snap_props=get(get(get(at(tools,6),"function"),"parameters"),"properties");
            J *flt=obj();
            put(flt,"type",str("string"));
            put(flt,"description",str("Optional case-insensitive substring filter matching control name or role."));
            put(snap_props,"filter",flt);
        }
        definition(tools,"desktop_action","Invoke an action advertised by current snapshot. Re-snapshot after action.",act,2);
        definition(tools,"desktop_set_text","Replace entire editable control text. Re-snapshot to verify.",text,2);
        definition(tools,"code_outline","Extract structural symbols (classes, structs, interfaces, enums, functions, methods) and 1-based line numbers from a file or directory tree. Prefer this to orient in unfamiliar or large files before reading slices.",path,1);
        {
            J *outline_props=get(get(get(at(tools,9),"function"),"parameters"),"properties");
            J *limit=obj();
            put(limit,"type",str("integer"));
            put(limit,"description",str("Optional maximum symbol entries to return (1..400, default 120)."));
            put(outline_props,"max_results",limit);
        }
        definition(tools,"delegate_research","Run an isolated read-only research subagent (using list_files, code_outline, search_text, read_file) to answer a multi-file codebase question and return concise findings without filling the main context.",research,1);
        {
            J *research_props=get(get(get(at(tools,10),"function"),"parameters"),"properties");
            J *focus=obj();
            put(focus,"type",str("string"));
            put(focus,"description",str("Optional workspace directory or file path to focus on (default .)."));
            put(research_props,"path",focus);
        }
        definition(tools,"web_fetch","Fetch a public HTTPS web page (or localhost HTTP) and extract clean readable text. Private/LAN addresses are blocked.",fetch_url,1);
        {
            J *fetch_props=get(get(get(at(tools,11),"function"),"parameters"),"properties");
            J *maxb=obj();
            put(maxb,"type",str("integer"));
            put(maxb,"description",str("Optional maximum UTF-8 bytes to return (1000..120000, default 40000)."));
            put(fetch_props,"max_bytes",maxb);
        }
        definition(tools,"desktop_screenshot","Capture a screenshot image of the active window or desktop for visual UI inspection.",nullptr,0);
        {
            J *shot_props=get(get(get(at(tools,12),"function"),"parameters"),"properties");
            J *target=obj();
            put(target,"type",str("string"));
            put(target,"description",str("Optional application name; omit or empty for full screen."));
            put(shot_props,"app",target);
        }
        definition(tools,"multi_edit","Apply multiple exact text replacements atomically in one UTF-8 file (up to 4 MB) under a single expected_sha256 check, one diff preview, and one backup.",multi_edit_req,2);
        {
            J *me_param=get(get(at(tools,13),"function"),"parameters");
            J *me_props=get(me_param,"properties"),*me_req=get(me_param,"required");
            J *edits_arr=obj(),*item_obj=obj(),*item_props=obj(),*item_req=arr(),*ot=obj(),*nt=obj();
            put(ot,"type",str("string"));
            put(ot,"description",str("Exact original text for this hunk (must occur once)."));
            put(nt,"type",str("string"));
            put(nt,"description",str("Replacement text for this hunk."));
            put(item_props,"old_text",ot);
            put(item_props,"new_text",nt);
            add(item_req,str("old_text"));
            add(item_req,str("new_text"));
            put(item_obj,"type",str("object"));
            put(item_obj,"properties",item_props);
            put(item_obj,"required",item_req);
            put(item_obj,"additionalProperties",json_object_new_boolean(0));
            put(edits_arr,"type",str("array"));
            put(edits_arr,"description",str("Ordered list of 1..20 {old_text, new_text} hunks applied atomically."));
            put(edits_arr,"items",item_obj);
            put(me_props,"edits",edits_arr);
            add(me_req,str("edits"));
        }
        definition(tools,"web_search","Search the web by keywords and return titles, URLs, and snippets. Follow up with web_fetch to read a relevant URL in full.",web_q,1);
        {
            J *ws_props=get(get(get(at(tools,14),"function"),"parameters"),"properties");
            J *maxr=obj();
            put(maxr,"type",str("integer"));
            put(maxr,"description",str("Optional maximum search results to return (1..10, default 5)."));
            put(ws_props,"max_results",maxr);
        }
        // Tools whose schema does not follow the shared field descriptions.
        auto literal=[tools](const char *json){
            J *tool=json_tokener_parse(json);
            if(tool)add(tools,tool);
        };
        literal(R"({"type":"function","function":{"name":"todo_write","description":"Create or update your checklist for multi-step work (3 or more distinct steps). Send the complete list each time; keep one item in_progress and mark items completed as soon as they are verified. The user sees the list.","parameters":{"type":"object","properties":{"todos":{"type":"array","description":"The complete checklist, 1..50 items in order.","items":{"type":"object","properties":{"content":{"type":"string","description":"Short imperative description of the step."},"status":{"type":"string","description":"pending, in_progress or completed."}},"required":["content","status"],"additionalProperties":false}}},"required":["todos"],"additionalProperties":false}}})");
        literal(R"({"type":"function","function":{"name":"submit_plan","description":"Plan mode is on: files cannot be changed, MCP tools are unavailable and only read-only shell commands run. Investigate with the read-only tools, then call submit_plan once with the complete implementation plan in Markdown (goal, files to change, steps, how it will be verified). The user reviews it; implementation starts only after approval.","parameters":{"type":"object","properties":{"plan":{"type":"string","description":"The complete plan in Markdown."}},"required":["plan"],"additionalProperties":false}}})");
        literal(R"({"type":"function","function":{"name":"shell_output","description":"Read new output and the status (running, or exit_code once finished) of a background command started by run_shell with background=true.","parameters":{"type":"object","properties":{"shell_id":{"type":"string","description":"The shell_id returned when the command was started."}},"required":["shell_id"],"additionalProperties":false}}})");
        literal(R"({"type":"function","function":{"name":"shell_kill","description":"Stop a background command and its child processes.","parameters":{"type":"object","properties":{"shell_id":{"type":"string","description":"The shell_id returned when the command was started."}},"required":["shell_id"],"additionalProperties":false}}})");
        return tools;
    }());
    return catalog.get();
}
bool plan_safe_tool(const char *name){
    for(const char *safe:{"list_files","search_text","read_file","code_outline","delegate_research","web_fetch",
                          "web_search","desktop_snapshot","desktop_screenshot","todo_write","submit_plan","shell_output"})
        if(!strcmp(name,safe))return true;
    return false;
}
static bool tool_enabled(const App &app,const char *name){
    if(app.subagent){
        return !strcmp(name,"list_files")||!strcmp(name,"search_text")
            ||!strcmp(name,"read_file")||!strcmp(name,"code_outline");
    }
    if(!strcmp(name,"submit_plan"))return app.plan_mode;
    // Plan mode keeps run_shell for read-only commands; tool_call refuses the rest.
    if(app.plan_mode&&!plan_safe_tool(name)&&strcmp(name,"run_shell"))return false;
    if(!strcmp(name,"run_shell"))return app.shell;
    if(!strcmp(name,"shell_output")||!strcmp(name,"shell_kill"))return app.shell;
    if(!strncmp(name,"desktop_",8))return app.desktop;
    return true;
}
J *schemas(App *a){
    JsonPtr tools(arr());
    J *catalog=builtin_tools();
    for(size_t i=0;i<alen(catalog);i++){
        J *item=at(catalog,i);
        if(tool_enabled(*a,sget(get(item,"function"),"name")))add(tools.get(),copy(item));
    }
    // An MCP tool can change anything, so plan mode leaves them out.
    if(!a->plan_mode)mcp_append_tools(*a,tools.get());
    return tools.release();
}
J *tool_call(App *a,const char *name,J *args){
    if(!strncmp(name,"mcp_",4)){
        if(a->plan_mode)return err(tr("计划模式下不能调用 MCP 工具；请写入计划，批准后再执行。"));
        return mcp_call(*a,name,args);
    }
    J *defs=builtin_tools(),*spec=NULL;
    for(size_t i=0;i<alen(defs);i++){
        J *f=get(at(defs,i),"function");
        if(!strcmp(sget(f,"name"),name)){
            spec=get(f,"parameters");
            break;
        }
    }
    if(!spec||!tool_enabled(*a,name)||!json_object_is_type(args,json_type_object)){
        return err(tr("未知或未启用的工具，或参数不是对象。"));
    }
    J *properties=get(spec,"properties"),*required=get(spec,"required");
    bool valid=true;
    for(size_t i=0;i<alen(required);i++){
        if(!json_object_object_get_ex(args,json_object_get_string(at(required,i)),NULL))valid=false;
    }
    json_object_object_foreach(args,key,value){
        J *description=NULL;
        if(!json_object_object_get_ex(properties,key,&description)){valid=false;continue;}
        J *type=get(description,"type");
        bool nullable=json_object_is_type(type,json_type_array);
        const char *kind=json_object_is_type(type,json_type_string)?json_object_get_string(type):"string";
        bool req=false;
        for(size_t i=0;i<alen(required);i++)if(!strcmp(json_object_get_string(at(required,i)),key))req=true;
        if(!strcmp(kind,"integer")){
            if(value==NULL){if(req)valid=false;}
            else if(!json_object_is_type(value,json_type_int))valid=false;
        }else if(!strcmp(kind,"boolean")){
            if(value==NULL){if(req)valid=false;}
            else if(!json_object_is_type(value,json_type_boolean))valid=false;
        }else if(!strcmp(kind,"array")){
            if(value==NULL){if(req)valid=false;}
            else if(!json_object_is_type(value,json_type_array))valid=false;
            else{
                J *item_spec=get(description,"items"),*item_props=get(item_spec,"properties"),*item_req=get(item_spec,"required");
                if(item_props){
                    for(size_t ai=0;ai<alen(value)&&valid;ai++){
                        J *elem=at(value,ai);
                        if(!json_object_is_type(elem,json_type_object)){valid=false;break;}
                        for(size_t ri=0;ri<alen(item_req);ri++)
                            if(!json_object_object_get_ex(elem,json_object_get_string(at(item_req,ri)),NULL))valid=false;
                        json_object_object_foreach(elem,ek,ev){
                            if(!json_object_object_get_ex(item_props,ek,NULL)||!json_object_is_type(ev,json_type_string)
                               ||(size_t)json_object_get_string_len(ev)!=strlen(json_object_get_string(ev)))valid=false;
                        }
                    }
                }
            }
        }else{
            if(!json_object_is_type(value,json_type_string)&&!(value==NULL&&nullable))valid=false;
            if(json_object_is_type(value,json_type_string)&&(size_t)json_object_get_string_len(value)!=strlen(json_object_get_string(value)))valid=false;
        }
    }
    if(!valid)return err(tr("工具参数缺失、多余或类型不正确。"));
    if(!strcmp(name,"todo_write"))return tool_todo_write(a,args);
    if(!strcmp(name,"submit_plan")){
        if(!a->plan_mode)return err(tr("当前不在计划模式。"));
        const char *plan=sget(args,"plan");
        size_t length=strlen(plan),first=strspn(plan," \t\r\n");
        if(first==length)return err(tr("计划内容为空。"));
        if(length>65536)return err(tr("计划超过 64 KB，请精简后重新提交。"));
        J *r=obj();
        put(r,"submitted",json_object_new_boolean(1));
        put(r,"status",str("The plan is shown to the user for review. Stop here and wait."));
        return r;
    }
    if(!strcmp(name,"shell_output"))return tool_shell_output(a,args);
    if(!strcmp(name,"shell_kill"))return tool_shell_kill(a,args);
    std::unique_ptr<ResourceLock> lock;
    if(!strcmp(name,"write_file")||!strcmp(name,"edit_file")||!strcmp(name,"multi_edit")||!strcmp(name,"run_shell")){
        lock=std::make_unique<ResourceLock>(*a,false);
        if(!lock->acquired()||interrupted)return err(interrupted?tr("资源等待已取消。"):tr("工作目录被占用或无法加锁，未执行操作；请返回对应对话处理待批准操作。"));
    }
    if(!strcmp(name,"list_files"))return file_list(a,sget(args,"path"),args);
    if(!strcmp(name,"read_file"))return file_read(a,sget(args,"path"),args);
    if(!strcmp(name,"search_text"))return file_search(a,args);
    if(!strcmp(name,"code_outline"))return tool_code_outline(a,args);
    if(!strcmp(name,"delegate_research"))return tool_delegate_research(a,args);
    if(!strcmp(name,"web_fetch"))return tool_web_fetch(a,args);
    if(!strcmp(name,"web_search"))return tool_web_search(a,args);
    if(!strcmp(name,"edit_file"))return file_edit(a,args);
    if(!strcmp(name,"multi_edit"))return file_multi_edit(a,args);
    if(!strcmp(name,"write_file"))return file_write(a,args);
    if(!strcmp(name,"run_shell")){
        int timeout_ms=30000;
        if(J *timeout_obj=get(args,"timeout_ms")){
            int64_t requested=json_object_get_int64(timeout_obj);
            if(requested<1000||requested>300000)return err(tr("命令超时设置无效。"));
            timeout_ms=static_cast<int>(requested);
        }
        const char *cmd=sget(args,"command");
        const bool background=bget(args,"background");
        if(a->plan_mode&&!safe_readonly_command(cmd))
            return err(tr("计划模式只允许只读命令；请把需要执行的命令写进计划，批准后再运行。"));
        std::string hook_err;
        if(!run_pre_shell_hook(*a,cmd,hook_err))return err(hook_err.c_str());
        const bool auto_safe=(a->auto_edit&&safe_readonly_command(cmd))||command_allowed_in_session(*a,cmd)||(a->plan_mode&&safe_readonly_command(cmd));
        if(!auto_safe&&!approve(a,cmd,Operation::Shell,tr("执行命令"))){
            J *r=err(tr("用户拒绝执行，不要绕过。"));
            put(r,"denied",json_object_new_boolean(1));
            return r;
        }
        if(background)return start_background_command(a,cmd);
        return run_command(a,cmd,timeout_ms);
    }
    if(!strcmp(name,"desktop_snapshot"))return desktop_snapshot(a,sget(args,"app"),sget(args,"filter"));
    if(!strcmp(name,"desktop_screenshot"))return tool_desktop_screenshot(a,args);
    if(!approve(a,json_object_to_json_string_ext(args,JSON_C_TO_STRING_PLAIN),Operation::Other,tr("操作桌面"))){
        J *r=err(tr("用户拒绝桌面操作，不要绕过。"));
        put(r,"denied",json_object_new_boolean(1));
        return r;
    }
    return desktop_act(a,sget(args,"ref"),!strcmp(name,"desktop_action")?sget(args,"action"):NULL,!strcmp(name,"desktop_set_text")?sget(args,"text"):NULL);
}
