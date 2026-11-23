#include "desk.hpp"
#include "tasks.hpp"
#include <openssl/evp.h>
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <set>
#ifndef _WIN32
#include <fcntl.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
constexpr size_t MAX_IMAGE_BYTES=5*1024*1024;
constexpr size_t MAX_ATTACHED_TEXT=65536;
constexpr size_t MAX_PROJECT_GUIDE=16384;

bool skip_entry(std::string_view name){
    if(name.empty()||name=="."||name=="..")return true;
    if(name==".git"||name==".desk-agent"||name==".ssh"||name==".gnupg"||name=="node_modules"
       ||name=="build"||name=="dist"||name=="out"||name=="target"||name=="__pycache__"
       ||name==".venv"||name=="venv"||name==".next"||name==".cache"||name==".idea"
       ||name==".vscode"||name=="vendor"||name=="CMakeFiles")return true;
    if(name.rfind(".env",0)==0&&(name.size()==4||name[4]=='.')&&name!=".env.example")return true;
    return false;
}

std::string lower_ascii(std::string_view text){
    std::string out(text);
    for(char &c:out)c=static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

bool is_space_byte(char c){
    return c==' '||c=='\t'||c=='\n'||c=='\r';
}

struct ImageKind {
    const char *mime=nullptr;
    const char *ext=nullptr;
};

ImageKind detect_image(const unsigned char *bytes,size_t len){
    if(len>=8&&!memcmp(bytes,"\x89PNG\r\n\x1a\n",8))return {"image/png","png"};
    if(len>=3&&bytes[0]==0xff&&bytes[1]==0xd8&&bytes[2]==0xff)return {"image/jpeg","jpg"};
    if(len>=6&&(!memcmp(bytes,"GIF87a",6)||!memcmp(bytes,"GIF89a",6)))return {"image/gif","gif"};
    if(len>=12&&!memcmp(bytes,"RIFF",4)&&!memcmp(bytes+8,"WEBP",4))return {"image/webp","webp"};
    return {};
}

std::string base64_encode(const void *data,size_t len){
    if(!len)return {};
    std::string out(((len+2)/3)*4+1,'\0');
    int written=EVP_EncodeBlock(reinterpret_cast<unsigned char *>(out.data()),
                                reinterpret_cast<const unsigned char *>(data),
                                static_cast<int>(len));
    if(written<=0)return {};
    out.resize(static_cast<size_t>(written));
    return out;
}

#if defined(_WIN32) || defined(__APPLE__)
std::string base64_decode(std::string_view encoded){
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
#ifndef _WIN32
std::string run_clipboard_tool(const char *const argv[],bool &tool_present){
    int pipefd[2];
    if(pipe(pipefd)!=0)return {};
    pid_t pid=fork();
    if(pid<0){close(pipefd[0]);close(pipefd[1]);return {};}
    if(pid==0){
        close(pipefd[0]);
        int devnull=open("/dev/null",O_RDWR);
        if(devnull>=0){
            dup2(devnull,STDIN_FILENO);
            dup2(devnull,STDERR_FILENO);
            if(devnull>2)close(devnull);
        }
        dup2(pipefd[1],STDOUT_FILENO);
        if(pipefd[1]>2)close(pipefd[1]);
        execvp(argv[0],const_cast<char *const *>(argv));
        _exit(errno==ENOENT?127:126);
    }
    close(pipefd[1]);
    fcntl(pipefd[0],F_SETFL,fcntl(pipefd[0],F_GETFL)|O_NONBLOCK);
    std::string data;
    bool oversized=false;
    const double deadline=monotime()+3.0;
    for(;;){
        double left=deadline-monotime();
        if(left<=0||interrupted)break;
        pollfd pfd{pipefd[0],POLLIN,0};
        int rc=poll(&pfd,1,static_cast<int>(std::min(left*1000.0,100.0)));
        if(rc<0&&errno!=EINTR)break;
        if(rc>0&&(pfd.revents&(POLLIN|POLLHUP))){
            char buf[8192];
            ssize_t n=read(pipefd[0],buf,sizeof buf);
            if(n==0)break;
            if(n>0){
                if(data.size()+static_cast<size_t>(n)>MAX_IMAGE_BYTES){oversized=true;break;}
                data.append(buf,static_cast<size_t>(n));
            }else if(errno!=EAGAIN&&errno!=EINTR)break;
        }
    }
    close(pipefd[0]);
    int status=0;
    if(waitpid(pid,&status,WNOHANG)==0){
        kill(pid,SIGTERM);
        double wait_until=monotime()+0.5;
        while(waitpid(pid,&status,WNOHANG)==0&&monotime()<wait_until)usleep(10000);
        if(waitpid(pid,&status,WNOHANG)==0){kill(pid,SIGKILL);while(waitpid(pid,&status,0)<0&&errno==EINTR){}}
    }
    if(WIFEXITED(status)&&WEXITSTATUS(status)!=127)tool_present=true;
    if(oversized||!WIFEXITED(status)||WEXITSTATUS(status)!=0)return {};
    return data;
}
#endif

J *make_image_url_block(const char *mime,const void *bytes,size_t len){
    std::string b64=base64_encode(bytes,len);
    if(b64.empty())return nullptr;
    std::string url=std::string("data:")+mime+";base64,"+b64;
    J *block=obj(),*inner=obj();
    put(block,"type",str("image_url"));
    put(inner,"url",str(url.c_str()));
    put(block,"image_url",inner);
    return block;
}

bool valid_hex32(std::string_view s){
    return s.size()==32&&std::all_of(s.begin(),s.end(),[](char c){
        return (c>='0'&&c<='9')||(c>='a'&&c<='f');
    });
}

// Trim trailing sentence punctuation on unquoted @path tokens.
std::string_view trim_mention_punct(std::string_view token){
    static constexpr std::string_view utf8_punct[]={"，","。","；","！","？","）","】","、","："};
    while(!token.empty()){
        bool removed=false;
        for(auto p:utf8_punct){
            if(token.size()>p.size()&&token.ends_with(p)){
                token.remove_suffix(p.size());
                removed=true;
                break;
            }
        }
        if(removed)continue;
        char back=token.back();
        if(back==','||back=='.'||back==';'||back=='!'||back=='?'||back==')'||back==']'||back=='}'||back=='\''||back=='"'){
            token.remove_suffix(1);
            continue;
        }
        break;
    }
    return token;
}

bool parse_line_suffix(std::string_view token,std::string &path_part,int64_t &start_line,int64_t &end_line){
    start_line=0;
    end_line=0;
    auto colon=token.rfind(':');
    if(colon==std::string_view::npos||colon==0||colon+1>=token.size()){
        path_part=std::string(token);
        return false;
    }
    std::string_view range=token.substr(colon+1);
    if(!range.empty()&&(range[0]=='L'||range[0]=='l'))range.remove_prefix(1);
    auto dash=range.find('-');
    auto parse_pos=[](std::string_view part,int64_t &out)->bool{
        if(!part.empty()&&(part[0]=='L'||part[0]=='l'))part.remove_prefix(1);
        if(part.empty()||part.size()>9)return false;
        int64_t val=0;
        for(char c:part){
            if(c<'0'||c>'9')return false;
            val=val*10+(c-'0');
        }
        if(val<1)return false;
        out=val;
        return true;
    };
    if(dash==std::string_view::npos){
        if(!parse_pos(range,start_line))return path_part=std::string(token),false;
        end_line=start_line;
    }else{
        if(!parse_pos(range.substr(0,dash),start_line)||!parse_pos(range.substr(dash+1),end_line)||end_line<start_line)
            return path_part=std::string(token),false;
    }
    path_part=std::string(token.substr(0,colon));
    return true;
}

int resolve_quiet(App *app,const char *candidate,char full[PATH_MAX]){
    char saved_error[sizeof app->error];
    memcpy(saved_error,app->error,sizeof saved_error);
    int rc=resolve_file(app,candidate,full);
    memcpy(app->error,saved_error,sizeof saved_error);
    return rc;
}

enum class Lang { Unknown, CLike, Python, JsTs, Go, Rust };

Lang detect_language(std::string_view path){
    auto dot=path.rfind('.');
    if(dot==std::string_view::npos)return Lang::Unknown;
    std::string ext=lower_ascii(path.substr(dot));
    if(ext==".c"||ext==".h"||ext==".cc"||ext==".cpp"||ext==".cxx"||ext==".hh"||ext==".hpp"||ext==".hxx"
       ||ext==".cs"||ext==".java"||ext==".kt"||ext==".swift")return Lang::CLike;
    if(ext==".py"||ext==".pyi")return Lang::Python;
    if(ext==".js"||ext==".jsx"||ext==".ts"||ext==".tsx"||ext==".mjs"||ext==".cjs")return Lang::JsTs;
    if(ext==".go")return Lang::Go;
    if(ext==".rs")return Lang::Rust;
    return Lang::Unknown;
}

bool is_ident_start(char c){
    auto u=static_cast<unsigned char>(c);
    return std::isalpha(u)||c=='_'||c=='$';
}
bool is_ident_char(char c){
    auto u=static_cast<unsigned char>(c);
    return std::isalnum(u)||c=='_'||c=='$'||c==':';
}

bool starts_word(std::string_view s,std::string_view word){
    return s.starts_with(word)&&(s.size()==word.size()||!is_ident_char(s[word.size()]));
}

std::string compact_signature(std::string_view raw){
    std::string out;
    out.reserve(std::min<size_t>(raw.size(),160));
    bool space=false;
    for(char c:raw){
        if(c=='\r'||c=='\n'||c=='\t'||c==' '){
            if(!out.empty())space=true;
            continue;
        }
        if(space){out+=' ';space=false;}
        out+=c;
    }
    while(!out.empty()&&(out.back()=='{'||out.back()==' '||out.back()==';')){
        if(out.back()==';')break;
        out.pop_back();
    }
    while(!out.empty()&&out.back()==' ')out.pop_back();
    if(out.size()>140){
        size_t cut=140;
        while(cut>0&&(static_cast<unsigned char>(out[cut])&0xc0)==0x80)--cut;
        out.resize(cut);
        out+="…";
    }
    return out;
}

bool clike_control_word(std::string_view name){
    static constexpr std::string_view words[]={
        "if","else","for","while","switch","catch","return","sizeof","alignof",
        "decltype","static_assert","new","delete","throw","case","do"
    };
    for(auto w:words)if(name==w)return true;
    return false;
}

struct SymbolEntry {
    std::string path;
    size_t line=0;
    const char *kind="";
    std::string signature;
};

void scan_file_symbols(std::string_view rel_path,std::string_view content,Lang lang,
                       size_t &total_lines,std::vector<SymbolEntry> &out,size_t limit,bool &truncated){
    bool block_comment=false;
    char py_triple=0;
    bool raw_tick=false;
    int brace_depth=0;
    size_t pos=0,line_no=0;
    std::string pending_header;
    size_t pending_line=0;

    while(pos<content.size()){
        line_no++;
        auto nl=content.find('\n',pos);
        std::string_view raw_line=nl==std::string_view::npos?content.substr(pos):content.substr(pos,nl-pos);
        if(!raw_line.empty()&&raw_line.back()=='\r')raw_line.remove_suffix(1);
        pos=nl==std::string_view::npos?content.size():nl+1;

        if(out.size()>=limit){truncated=true;continue;}

        std::string clean;
        clean.reserve(raw_line.size());
        for(size_t i=0;i<raw_line.size();){
            char c=raw_line[i];
            if(block_comment){
                if(c=='*'&&i+1<raw_line.size()&&raw_line[i+1]=='/'){block_comment=false;i+=2;clean+="  ";}
                else{clean+=' ';i++;}
                continue;
            }
            if(py_triple){
                if(c==py_triple&&i+2<raw_line.size()&&raw_line[i+1]==py_triple&&raw_line[i+2]==py_triple){
                    py_triple=0;i+=3;clean+="   ";
                }else{clean+=' ';i++;}
                continue;
            }
            if(raw_tick){
                if(c=='`'){raw_tick=false;clean+='`';i++;}
                else{if(c=='\\'&&i+1<raw_line.size())i+=2;else i++;clean+=' ';}
                continue;
            }
            if(lang==Lang::Python&&c=='#')break;
            if(lang!=Lang::Python&&c=='/'&&i+1<raw_line.size()){
                if(raw_line[i+1]=='/')break;
                if(raw_line[i+1]=='*'){block_comment=true;i+=2;clean+="  ";continue;}
            }
            if(lang==Lang::Python&&(c=='"'||c=='\'')&&i+2<raw_line.size()&&raw_line[i+1]==c&&raw_line[i+2]==c){
                i+=3;
                bool closed=false;
                while(i+2<raw_line.size()){
                    if(raw_line[i]==c&&raw_line[i+1]==c&&raw_line[i+2]==c){i+=3;closed=true;break;}
                    i++;
                }
                if(!closed){py_triple=c;break;}
                clean+="\"\"";
                continue;
            }
            if((lang==Lang::JsTs||lang==Lang::Go)&&c=='`'){
                i++;
                while(i<raw_line.size()&&raw_line[i]!='`'){
                    if(lang==Lang::JsTs&&raw_line[i]=='\\'&&i+1<raw_line.size())i+=2;
                    else i++;
                }
                if(i<raw_line.size())i++;
                else raw_tick=true;
                clean+="``";
                continue;
            }
            if(c=='"'||c=='\''){
                char q=c;
                clean+=q;i++;
                while(i<raw_line.size()&&raw_line[i]!=q){
                    if(raw_line[i]=='\\'&&i+1<raw_line.size())i+=2;
                    else i++;
                }
                if(i<raw_line.size())i++;
                clean+=q;
                continue;
            }
            clean+=c;
            i++;
        }

        size_t indent=0;
        while(indent<raw_line.size()&&(raw_line[indent]==' '||raw_line[indent]=='\t'))
            indent+=raw_line[indent]=='\t'?4:1;
        std::string_view trimmed(clean);
        while(!trimmed.empty()&&is_space_byte(trimmed.front()))trimmed.remove_prefix(1);
        while(!trimmed.empty()&&is_space_byte(trimmed.back()))trimmed.remove_suffix(1);

        const int line_depth=brace_depth;
        for(char c:clean){
            if(c=='{')brace_depth++;
            else if(c=='}'&&brace_depth>0)brace_depth--;
        }

        if(trimmed.empty())continue;

        auto emit=[&](size_t at_line,const char *kind,std::string_view sig_src){
            if(out.size()>=limit){truncated=true;return;}
            std::string sig=compact_signature(sig_src);
            if(!sig.empty())out.push_back({std::string(rel_path),at_line,kind,std::move(sig)});
        };

        if(lang==Lang::Python){
            if(indent>8)continue;
            std::string_view rest=trimmed;
            if(starts_word(rest,"async")){
                rest.remove_prefix(5);
                while(!rest.empty()&&is_space_byte(rest.front()))rest.remove_prefix(1);
            }
            if(starts_word(rest,"def"))emit(line_no,indent?"method":"function",raw_line);
            else if(starts_word(rest,"class"))emit(line_no,"class",raw_line);
            continue;
        }

        if(lang==Lang::Go){
            if(line_depth>0)continue;
            if(starts_word(trimmed,"func"))emit(line_no,"function",raw_line);
            else if(starts_word(trimmed,"type")){
                const char *kind=trimmed.find("struct")!=std::string_view::npos?"struct":
                                 trimmed.find("interface")!=std::string_view::npos?"interface":"type";
                emit(line_no,kind,raw_line);
            }
            continue;
        }

        if(lang==Lang::Rust){
            if(line_depth>1)continue;
            std::string_view rest=trimmed;
            if(rest.starts_with("pub")){
                rest.remove_prefix(3);
                if(!rest.empty()&&rest.front()=='('){
                    auto close=rest.find(')');
                    if(close!=std::string_view::npos)rest.remove_prefix(close+1);
                }
                while(!rest.empty()&&is_space_byte(rest.front()))rest.remove_prefix(1);
            }
            for(bool stripped=true;stripped;){
                stripped=false;
                for(std::string_view kw:{"async","unsafe","const","extern"}){
                    if(starts_word(rest,kw)){
                        rest.remove_prefix(kw.size());
                        while(!rest.empty()&&is_space_byte(rest.front()))rest.remove_prefix(1);
                        if(kw=="extern"&&!rest.empty()&&rest.front()=='"'){
                            auto q=rest.find('"',1);
                            if(q!=std::string_view::npos){
                                rest.remove_prefix(q+1);
                                while(!rest.empty()&&is_space_byte(rest.front()))rest.remove_prefix(1);
                            }
                        }
                        stripped=true;
                    }
                }
            }
            if(starts_word(rest,"fn"))emit(line_no,line_depth?"method":"function",raw_line);
            else if(starts_word(rest,"struct"))emit(line_no,"struct",raw_line);
            else if(starts_word(rest,"enum"))emit(line_no,"enum",raw_line);
            else if(starts_word(rest,"trait"))emit(line_no,"trait",raw_line);
            else if(starts_word(rest,"impl"))emit(line_no,"impl",raw_line);
            else if(starts_word(rest,"mod"))emit(line_no,"module",raw_line);
            else if(starts_word(rest,"type"))emit(line_no,"type",raw_line);
            continue;
        }

        if(lang==Lang::JsTs){
            if(line_depth>1)continue;
            std::string_view rest=trimmed;
            for(bool stripped=true;stripped;){
                stripped=false;
                for(std::string_view kw:{"export","default","declare","abstract","async","public","private","protected","static"}){
                    if(starts_word(rest,kw)){
                        rest.remove_prefix(kw.size());
                        while(!rest.empty()&&is_space_byte(rest.front()))rest.remove_prefix(1);
                        stripped=true;
                    }
                }
            }
            if(starts_word(rest,"function"))emit(line_no,"function",raw_line);
            else if(starts_word(rest,"class"))emit(line_no,"class",raw_line);
            else if(starts_word(rest,"interface"))emit(line_no,"interface",raw_line);
            else if(starts_word(rest,"enum"))emit(line_no,"enum",raw_line);
            else if(starts_word(rest,"type"))emit(line_no,"type",raw_line);
            else if((starts_word(rest,"const")||starts_word(rest,"let"))&&rest.find("=>")!=std::string_view::npos)
                emit(line_no,"function",raw_line);
            continue;
        }

        // C / C++ / Java / C# / Kotlin / Swift
        if(trimmed.front()=='#'){pending_header.clear();continue;}
        if(line_depth>2){pending_header.clear();continue;}

        std::string combined;
        std::string_view candidate=trimmed;
        size_t candidate_line=line_no;
        if(!pending_header.empty()){
            combined=pending_header+" "+std::string(trimmed);
            candidate=combined;
            candidate_line=pending_line;
            pending_header.clear();
        }

        // Type / namespace declarations.
        std::string_view scan=candidate;
        if(scan.starts_with("template")){
            auto gt=scan.find('>');
            if(gt!=std::string_view::npos){
                scan.remove_prefix(gt+1);
                while(!scan.empty()&&is_space_byte(scan.front()))scan.remove_prefix(1);
            }
        }
        bool matched_type=false;
        for(auto [kw,kind]:std::initializer_list<std::pair<std::string_view,const char *>>{
            {"namespace","namespace"},{"class","class"},{"struct","struct"},{"enum","enum"},{"interface","interface"}
        }){
            if(starts_word(scan,kw)){
                auto after=scan.substr(kw.size());
                while(!after.empty()&&is_space_byte(after.front()))after.remove_prefix(1);
                if(starts_word(after,"class")||starts_word(after,"struct")){
                    after.remove_prefix(after.starts_with("class")?5:6);
                    while(!after.empty()&&is_space_byte(after.front()))after.remove_prefix(1);
                }
                if(!after.empty()&&is_ident_start(after.front())){
                    // Skip pure forward declarations like "struct Foo;".
                    if(after.find('{')!=std::string_view::npos||!after.ends_with(';'))
                        emit(candidate_line,kind,candidate);
                    matched_type=true;
                }
                break;
            }
        }
        if(matched_type)continue;

        // Multi-line C/C++ function signature whose opening '{' sits on the next line.
        if(candidate.find('(')!=std::string_view::npos&&candidate.find('{')==std::string_view::npos
           &&candidate.find(';')==std::string_view::npos&&candidate.size()<200){
            pending_header=std::string(candidate);
            pending_line=candidate_line;
            continue;
        }

        auto brace=candidate.find('{');
        if(brace==std::string_view::npos)continue;
        std::string_view head=candidate.substr(0,brace);
        if(head.find(';')!=std::string_view::npos)continue;
        auto paren=head.find('(');
        auto close_paren=head.rfind(')');
        if(paren==std::string_view::npos||close_paren==std::string_view::npos||close_paren<paren)continue;
        std::string_view before_paren=head.substr(0,paren);
        while(!before_paren.empty()&&is_space_byte(before_paren.back()))before_paren.remove_suffix(1);
        if(before_paren.empty())continue;
        // Reject assignments like "auto f = []() {" unless "operator=".
        if(before_paren.find('=')!=std::string_view::npos&&before_paren.find("operator")==std::string_view::npos)continue;
        size_t name_end=before_paren.size();
        size_t name_start=name_end;
        while(name_start>0&&(is_ident_char(before_paren[name_start-1])||before_paren[name_start-1]=='~'))name_start--;
        std::string_view fn_name=before_paren.substr(name_start,name_end-name_start);
        if(fn_name.empty()||clike_control_word(fn_name))continue;
        emit(candidate_line,line_depth>0?"method":"function",head);
    }
    total_lines=line_no;
}
} // namespace

void expand_draft_input(std::string &text,ChatDraft &draft){
    if(draft.newline_inserted){
        const std::string_view glyph="\xE2\x86\xB5";
        for(size_t pos=0;(pos=text.find(glyph,pos))!=std::string::npos;)
            text.replace(pos,glyph.size(),"\n");
    }
    for(const auto &chunk:draft.pastes){
        if(chunk.marker.empty())continue;
        for(size_t pos=0;(pos=text.find(chunk.marker,pos))!=std::string::npos;){
            text.replace(pos,chunk.marker.size(),chunk.content);
            pos+=chunk.content.size();
        }
    }
    draft.pastes.clear();
    draft.next_paste=1;
    draft.newline_inserted=false;
}

bool find_at_mention(std::string_view text,size_t cursor,size_t &at_pos,std::string &query){
    cursor=std::min(cursor,text.size());
    std::string_view prefix=text.substr(0,cursor);
    auto line_start=prefix.find_last_of("\r\n");
    size_t bound=line_start==std::string_view::npos?0:line_start+1;

    // Quoted form: @"path with spaces
    for(size_t i=cursor;i>bound+1;--i){
        if(prefix[i-1]=='"'&&prefix[i-2]=='@'){
            size_t at=i-2;
            if(at==0||is_space_byte(prefix[at-1])){
                std::string_view inner=prefix.substr(i,cursor-i);
                if(inner.find('"')==std::string_view::npos){
                    at_pos=at;
                    query=std::string(inner);
                    return true;
                }
            }
            break;
        }
    }

    size_t start=cursor;
    while(start>bound&&!is_space_byte(prefix[start-1]))--start;
    if(start<cursor&&prefix[start]=='@'&&(start==0||is_space_byte(prefix[start-1]))){
        at_pos=start;
        query=std::string(prefix.substr(start+1,cursor-start-1));
        return true;
    }
    if(cursor>bound&&prefix[cursor-1]=='@'&&(cursor-1==0||is_space_byte(prefix[cursor-2]))){
        at_pos=cursor-1;
        query.clear();
        return true;
    }
    return false;
}

std::vector<std::string> workspace_complete_files(const App &app,std::string_view query,size_t limit){
    struct Candidate {
        int score;
        std::string rel;
    };
    std::vector<Candidate> matches;
    const std::string q=lower_ascii(query);
    const fs::path root=fs_path(app.root);
    GitIgnore gitignore;
    gitignore.load(app.root);
    std::error_code cfg_ec;
    const fs::path cfg=fs::weakly_canonical(fs_path(app.config_dir),cfg_ec);
    const double deadline=monotime()+1.5;
    size_t visited=0;

    auto score_path=[&](std::string_view rel,int depth)->int{
        if(q.empty())return 1000-depth*20;
        std::string lower=lower_ascii(rel);
        auto slash=lower.rfind('/');
        std::string_view base=slash==std::string::npos?std::string_view(lower):std::string_view(lower).substr(slash+1);
        if(base==q)return 1000;
        if(lower==q)return 950;
        if(base.starts_with(q))return 800-static_cast<int>(std::min<size_t>(base.size(),100));
        if(lower.starts_with(q))return 700-static_cast<int>(std::min<size_t>(lower.size(),100));
        if(auto p=base.find(q);p!=std::string_view::npos)return 520-static_cast<int>(p)*2;
        if(auto p=lower.find(q);p!=std::string_view::npos)return 360-static_cast<int>(std::min<size_t>(p,150));
        size_t qi=0,first_hit=0,last_hit=0;
        for(size_t i=0;i<lower.size()&&qi<q.size();i++){
            if(lower[i]==q[qi]){
                if(qi==0)first_hit=i;
                last_hit=i;
                qi++;
            }
        }
        if(qi==q.size())return 180-static_cast<int>(std::min<size_t>(last_hit-first_hit,150));
        return -1;
    };

    auto walk=[&](auto &self,const fs::path &dir,int depth)->void{
        if(depth>16||visited>=5000||monotime()>deadline||interrupted)return;
        std::error_code ec;
        fs::directory_iterator it(dir,ec);
        if(ec)return;
        for(fs::directory_iterator end;it!=end;it.increment(ec)){
            if(ec||visited>=5000||monotime()>deadline||interrupted)break;
            const fs::path entry=it->path();
            if(path_link(entry))continue;
            if(!cfg_ec&&path_inside(cfg,entry))continue;
            std::string name=path_text(entry.filename());
            if(skip_entry(name))continue;
            std::error_code st_ec;
            auto st=fs::symlink_status(entry,st_ec);
            if(st_ec)continue;
            std::string rel=path_text(entry.lexically_relative(root));
            if(!rel.empty()&&gitignore.ignored(rel,fs::is_directory(st)))continue;
            if(fs::is_directory(st)){
                self(self,entry,depth+1);
            }else if(fs::is_regular_file(st)){
                visited++;
                if(rel.empty())continue;
                int s=score_path(rel,depth);
                if(s>=0)matches.push_back({s,std::move(rel)});
            }
        }
    };
    walk(walk,root,0);
    std::sort(matches.begin(),matches.end(),[](const Candidate &a,const Candidate &b){
        if(a.score!=b.score)return a.score>b.score;
        if(a.rel.size()!=b.rel.size())return a.rel.size()<b.rel.size();
        return a.rel<b.rel;
    });
    std::vector<std::string> out;
    const size_t count=std::min(limit,matches.size());
    out.reserve(count);
    for(size_t i=0;i<count;i++)out.push_back(std::move(matches[i].rel));
    return out;
}

std::string capture_clipboard_image(App &app){
    std::string raw;
    bool tool_present=false;
    if(const char *mock=env_value("DESK_CLIPBOARD_IMAGE_FILE");mock&&*mock){
        tool_present=true;
        size_t len=0;
        char *bytes=read_bytes(mock,MAX_IMAGE_BYTES,&len);
        if(bytes){raw.assign(bytes,len);free(bytes);}
    }else{
#ifdef _WIN32
        tool_present=true;
        App probe{};
        snprintf(probe.root,sizeof probe.root,"%s",app.root);
        JsonPtr res(run_command(&probe,
            "powershell.exe -Sta -NoProfile -NonInteractive -Command "
            "\"Add-Type -AssemblyName System.Windows.Forms,System.Drawing; "
            "$img=[Windows.Forms.Clipboard]::GetImage(); "
            "if($img){$ms=New-Object IO.MemoryStream; $img.Save($ms,[Drawing.Imaging.ImageFormat]::Png); "
            "[Convert]::ToBase64String($ms.ToArray())}\"",5000));
        if(res&&iget(res.get(),"exit_code",-1)==0&&*sget(res.get(),"output"))
            raw=base64_decode(sget(res.get(),"output"));
#elif defined(__APPLE__)
        const char *argv[]={"osascript","-e",
            "use framework \"AppKit\"\n"
            "set pb to current application's NSPasteboard's generalPasteboard()\n"
            "set d to pb's dataForType:(current application's NSPasteboardTypePNG)\n"
            "if d is missing value then return \"\"\n"
            "return (d's base64EncodedStringWithOptions:0) as text",nullptr};
        std::string b64=run_clipboard_tool(argv,tool_present);
        if(!b64.empty())raw=base64_decode(b64);
#else
        if(const char *wayland=env_value("WAYLAND_DISPLAY");wayland&&*wayland){
            for(const char *mime:{"image/png","image/jpeg","image/webp"}){
                const char *argv[]={"wl-paste","--no-newline","--type",mime,nullptr};
                raw=run_clipboard_tool(argv,tool_present);
                if(!raw.empty())break;
            }
        }
        if(raw.empty()){
            if(const char *x11=env_value("DISPLAY");x11&&*x11){
                for(const char *mime:{"image/png","image/jpeg"}){
                    const char *argv[]={"xclip","-selection","clipboard","-t",mime,"-o",nullptr};
                    raw=run_clipboard_tool(argv,tool_present);
                    if(!raw.empty())break;
                }
            }
        }
#endif
    }

    auto kind=detect_image(reinterpret_cast<const unsigned char *>(raw.data()),raw.size());
    if(!kind.mime||raw.size()>MAX_IMAGE_BYTES){
        seterr(&app,tool_present?tr("剪贴板中没有可用的图片（支持 PNG/JPEG/WebP/GIF，最大 5 MB）。")
                                :tr("剪贴板中没有可用的图片，或未安装 wl-paste / xclip。"));
        return {};
    }
    std::string dir=std::string(app.root)+"/.desk-agent/clipboard";
    if(mkdirs(dir.c_str(),0700)){
        seterr(&app,tr("无法保存剪贴板图片缓存。"));
        return {};
    }
    char id[33];
    id_string(id);
    std::string filename=std::string(id)+"."+kind.ext;
    std::string full=dir+"/"+filename;
    if(atomic_file(full.c_str(),raw.data(),raw.size(),0600)){
        seterr(&app,tr("无法保存剪贴板图片缓存。"));
        return {};
    }
    return "@clipboard:"+filename;
}

J *prepare_user_message(App *app,const char *task){
    std::string_view raw(task);
    std::string attached_text;
    std::vector<JsonPtr> images;
    std::set<std::string> seen;

    for(size_t i=0;i<raw.size();){
        if(raw[i]!='@'||(i>0&&!is_space_byte(raw[i-1]))){i++;continue;}
        size_t token_end=i+1;
        std::string_view token;
        if(token_end<raw.size()&&raw[token_end]=='"'){
            auto close=raw.find('"',token_end+1);
            auto nl=raw.find_first_of("\r\n",token_end+1);
            if(close!=std::string_view::npos&&(nl==std::string_view::npos||close<nl)){
                token=raw.substr(token_end+1,close-token_end-1);
                i=close+1;
            }else{i++;continue;}
        }else{
            while(token_end<raw.size()&&!is_space_byte(raw[token_end]))token_end++;
            token=trim_mention_punct(raw.substr(i+1,token_end-i-1));
            i=token_end;
        }
        if(token.empty())continue;

        // 1. Staged clipboard image: @clipboard:<32-hex>.<ext>
        if(token.starts_with("clipboard:")){
            if(images.size()>=4)continue;
            std::string_view name=token.substr(10);
            auto dot=name.find('.');
            if(dot!=32||!valid_hex32(name.substr(0,32)))continue;
            std::string_view ext=name.substr(33);
            if(ext!="png"&&ext!="jpg"&&ext!="webp"&&ext!="gif")continue;
            std::string key="clipboard:"+std::string(name);
            if(!seen.insert(key).second)continue;
            std::string clip_path=std::string(app->root)+"/.desk-agent/clipboard/"+std::string(name);
            if(path_link(fs_path(clip_path)))continue;
            size_t len=0;
            char *bytes=read_bytes(clip_path.c_str(),MAX_IMAGE_BYTES,&len);
            if(!bytes)continue;
            auto kind=detect_image(reinterpret_cast<const unsigned char *>(bytes),len);
            if(kind.mime){
                if(J *img=make_image_url_block(kind.mime,bytes,len))images.emplace_back(img);
            }
            free(bytes);
            continue;
        }

        // 2. Workspace file or sliced file: @path or @path:start-end
        std::string path_part;
        int64_t start_line=0,end_line=0;
        bool has_range=parse_line_suffix(token,path_part,start_line,end_line);
        char full[PATH_MAX];
        if(path_part.empty()||resolve_quiet(app,path_part.c_str(),full)!=0){
            // Maybe the filename literally contains a colon; try the whole token.
            if(!has_range||resolve_quiet(app,std::string(token).c_str(),full)!=0)continue;
            path_part=std::string(token);
            has_range=false;
        }
        if(path_link(fs_path(full)))continue;
        std::error_code st_ec;
        auto st=fs::symlink_status(fs_path(full),st_ec);
        if(st_ec||!fs::is_regular_file(st))continue;
        std::string rel=path_text(fs_path(full).lexically_relative(fs_path(app->root)));
        if(rel.empty())rel=path_part;
        std::string key=rel+(has_range?":"+std::to_string(start_line)+"-"+std::to_string(end_line):"");
        if(!seen.insert(key).second)continue;

        if(!has_range){
            size_t len=0;
            char *bytes=read_bytes(full,MAX_IMAGE_BYTES,&len);
            if(!bytes)continue;
            auto kind=detect_image(reinterpret_cast<const unsigned char *>(bytes),len);
            if(kind.mime){
                if(images.size()<4){
                    if(J *img=make_image_url_block(kind.mime,bytes,len))images.emplace_back(img);
                }
                free(bytes);
                continue;
            }
            if(len<=MAX_ATTACHED_TEXT&&!memchr(bytes,0,len)&&valid_utf8(std::string_view(bytes,len))
               &&attached_text.size()+len<=MAX_ATTACHED_TEXT){
                char *digest=sha256(bytes,len);
                attached_text+="<referenced_file path=\""+rel+"\"";
                if(digest){attached_text+=" sha256=\"";attached_text+=digest;attached_text+="\"";free(digest);}
                attached_text+=">\n";
                attached_text.append(bytes,len);
                if(len&&bytes[len-1]!='\n')attached_text+='\n';
                attached_text+="</referenced_file>\n";
            }
            free(bytes);
            continue;
        }

        if(attached_text.size()>=MAX_ATTACHED_TEXT)continue;
        size_t len=0;
        char *bytes=read_bytes(full,SLICED_FILE_LIMIT,&len);
        if(!bytes)continue;
        if(!memchr(bytes,0,len)&&valid_utf8(std::string_view(bytes,len))){
            size_t slice_begin=len,slice_end=len,line_no=0;
            for(size_t pos=0;pos<len;){
                line_no++;
                const char *nl=static_cast<const char *>(memchr(bytes+pos,'\n',len-pos));
                size_t next=nl?static_cast<size_t>(nl-bytes)+1:len;
                if(static_cast<int64_t>(line_no)==start_line)slice_begin=pos;
                if(static_cast<int64_t>(line_no)>=start_line&&static_cast<int64_t>(line_no)<=end_line)slice_end=next;
                if(static_cast<int64_t>(line_no)>=end_line)break;
                pos=next;
            }
            if(slice_begin<slice_end){
                size_t take=std::min(slice_end-slice_begin,MAX_ATTACHED_TEXT-attached_text.size());
                while(take>0&&(static_cast<unsigned char>(bytes[slice_begin+take])&0xc0)==0x80)--take;
                attached_text+="<referenced_file path=\""+rel+"\" lines=\""
                    +std::to_string(start_line)+"-"+std::to_string(end_line)+"\">\n";
                attached_text.append(bytes+slice_begin,take);
                if(take&&bytes[slice_begin+take-1]!='\n')attached_text+='\n';
                attached_text+="</referenced_file>\n";
            }
        }
        free(bytes);
    }

    J *user=obj();
    put(user,"role",str("user"));
    if(attached_text.empty()&&images.empty()){
        put(user,"content",str(task));
        return user;
    }
    put(user,"_display",str(task));
    std::string combined=attached_text.empty()?std::string(task):std::string(task)+"\n\n"+attached_text;
    if(images.empty()){
        put(user,"content",str(combined.c_str()));
        return user;
    }
    J *blocks=arr();
    J *text_block=obj();
    put(text_block,"type",str("text"));
    put(text_block,"text",str(combined.c_str()));
    add(blocks,text_block);
    for(auto &img:images)add(blocks,img.release());
    put(user,"content",blocks);
    return user;
}

std::string user_message_text(J *message){
    const char *display=sget(message,"_display");
    if(*display)return display;
    J *content=get(message,"content");
    if(json_object_is_type(content,json_type_string))return json_object_get_string(content);
    std::string out;
    for(size_t i=0;i<alen(content);i++){
        J *block=at(content,i);
        if(!strcmp(sget(block,"type"),"text")&&*sget(block,"text")){
            if(!out.empty())out+='\n';
            out+=sget(block,"text");
        }
    }
    return out;
}

std::string load_project_guide(const App &app){
    const char *disabled=env_value("DESK_NO_PROJECT_GUIDE");
    if(disabled&&*disabled&&strcmp(disabled,"0")!=0)return {};
    if(!*app.root)return {};
    std::string combined;
    std::vector<fs::path> loaded_paths;
    auto try_load=[&](const char *name){
        fs::path candidate=fs_path(app.root)/name;
        if(path_link(candidate))return;
        std::error_code ec;
        auto st=fs::symlink_status(candidate,ec);
        if(ec||!fs::is_regular_file(st))return;
        for(const auto &prev:loaded_paths){
            std::error_code eq_ec;
            if(fs::equivalent(prev,candidate,eq_ec)&&!eq_ec)return;
        }
        size_t len=0;
        std::string full=path_text(candidate);
        char *bytes=read_bytes(full.c_str(),FILE_LIMIT,&len);
        if(!bytes)return;
        if(len&&!memchr(bytes,0,len)&&valid_utf8(std::string_view(bytes,len))){
            size_t room=combined.size()<MAX_PROJECT_GUIDE?MAX_PROJECT_GUIDE-combined.size():0;
            if(room>64){
                size_t take=std::min(len,room);
                while(take>0&&(static_cast<unsigned char>(bytes[take])&0xc0)==0x80)--take;
                if(!combined.empty())combined+="\n\n";
                combined+="Workspace project instructions (";
                combined+=name;
                combined+="):\n";
                combined.append(bytes,take);
                if(take<len)combined+="\n…";
                loaded_paths.push_back(candidate);
            }
        }
        free(bytes);
    };
    for(const char *name:{"DESKCODE.md","deskcode.md","AGENTS.md","agents.md"})try_load(name);
    if(combined.empty())try_load("CLAUDE.md");
    return combined;
}

J *tool_code_outline(App *app,J *args){
    char full[PATH_MAX];
    const char *target=sget(args,"path");
    if(resolve_file(app,target,full))return err(app->error);
    int max_results=120;
    if(J *limit_obj=get(args,"max_results")){
        int64_t requested=json_object_get_int64(limit_obj);
        if(requested<1||requested>400)return err(tr("max_results 须在 1 到 400 之间。"));
        max_results=static_cast<int>(requested);
    }
    std::error_code ec;
    auto st=fs::symlink_status(fs_path(full),ec);
    if(ec||!fs::exists(st))return err(tr("文件不存在，请先用 list_files 或 search_text 确认路径。"));
    const fs::path root=fs_path(app->root);

    if(fs::is_regular_file(st)){
        if(path_link(fs_path(full)))return err(tr("文件工具只能访问工作目录。"));
        size_t len=0;
        char *bytes=read_bytes(full,FILE_LIMIT,&len);
        if(!bytes)return err(tr("无法读取文件，文件可能不是普通文件或超过 256 KB。"));
        if(memchr(bytes,0,len)||!valid_utf8(std::string_view(bytes,len))){
            free(bytes);
            return err(tr("只支持 UTF-8 文本文件。"));
        }
        std::string rel=path_text(fs_path(full).lexically_relative(root));
        if(rel.empty())rel=target;
        Lang lang=detect_language(rel);
        std::vector<SymbolEntry> found;
        size_t total_lines=0;
        bool truncated=false;
        if(lang!=Lang::Unknown)
            scan_file_symbols(rel,std::string_view(bytes,len),lang,total_lines,found,static_cast<size_t>(max_results),truncated);
        else{
            for(size_t i=0;i<len;i++)if(bytes[i]=='\n')total_lines++;
            if(len&&bytes[len-1]!='\n')total_lines++;
        }
        free(bytes);
        J *result=obj(),*symbols=arr();
        put(result,"path",str(rel.c_str()));
        put(result,"total_lines",json_object_new_int64(static_cast<int64_t>(total_lines)));
        for(const auto &sym:found){
            J *item=obj();
            put(item,"line",json_object_new_int64(static_cast<int64_t>(sym.line)));
            put(item,"kind",str(sym.kind));
            put(item,"signature",str(sym.signature.c_str()));
            add(symbols,item);
        }
        put(result,"symbols",symbols);
        put(result,"truncated",json_object_new_boolean(truncated));
        return result;
    }

    if(!fs::is_directory(st))return err(tr("无法读取目标路径。"));
    GitIgnore gitignore;
    gitignore.load(app->root);
    std::error_code cfg_ec;
    const fs::path cfg=fs::weakly_canonical(fs_path(app->config_dir),cfg_ec);
    std::vector<fs::path> source_files;
    const double deadline=monotime()+5.0;

    auto collect=[&](auto &self,const fs::path &dir,int depth)->void{
        if(depth>20||source_files.size()>=2000||monotime()>deadline||interrupted)return;
        std::error_code dir_ec;
        fs::directory_iterator it(dir,dir_ec);
        if(dir_ec)return;
        for(fs::directory_iterator end;it!=end;it.increment(dir_ec)){
            if(dir_ec||source_files.size()>=2000||monotime()>deadline||interrupted)break;
            const fs::path entry=it->path();
            if(path_link(entry))continue;
            if(!cfg_ec&&path_inside(cfg,entry))continue;
            std::string name=path_text(entry.filename());
            if(skip_entry(name))continue;
            std::error_code item_ec;
            auto item_st=fs::symlink_status(entry,item_ec);
            if(item_ec)continue;
            std::string rel=path_text(entry.lexically_relative(root));
            if(!rel.empty()&&gitignore.ignored(rel,fs::is_directory(item_st)))continue;
            if(fs::is_directory(item_st))self(self,entry,depth+1);
            else if(fs::is_regular_file(item_st)&&detect_language(name)!=Lang::Unknown)
                source_files.push_back(entry);
        }
    };
    collect(collect,fs_path(full),0);
    std::sort(source_files.begin(),source_files.end());

    std::vector<SymbolEntry> found;
    bool truncated=false;
    size_t files_scanned=0;
    for(const auto &file:source_files){
        if(monotime()>deadline||interrupted){truncated=true;break;}
        if(found.size()>=static_cast<size_t>(max_results)){truncated=true;break;}
        size_t len=0;
        std::string file_str=path_text(file);
        char *bytes=read_bytes(file_str.c_str(),FILE_LIMIT,&len);
        if(!bytes)continue;
        if(!memchr(bytes,0,len)&&valid_utf8(std::string_view(bytes,len))){
            files_scanned++;
            std::string rel=path_text(file.lexically_relative(root));
            size_t lines=0;
            scan_file_symbols(rel,std::string_view(bytes,len),detect_language(rel),lines,
                              found,static_cast<size_t>(max_results),truncated);
        }
        free(bytes);
    }

    std::string root_rel=path_text(fs_path(full).lexically_relative(root));
    if(root_rel.empty())root_rel=".";
    J *result=obj(),*symbols=arr();
    put(result,"path",str(root_rel.c_str()));
    put(result,"files_scanned",json_object_new_int64(static_cast<int64_t>(files_scanned)));
    for(const auto &sym:found){
        J *item=obj();
        put(item,"path",str(sym.path.c_str()));
        put(item,"line",json_object_new_int64(static_cast<int64_t>(sym.line)));
        put(item,"kind",str(sym.kind));
        put(item,"signature",str(sym.signature.c_str()));
        add(symbols,item);
    }
    put(result,"symbols",symbols);
    put(result,"truncated",json_object_new_boolean(truncated));
    return result;
}

J *tool_delegate_research(App *app,J *args){
    const char *question=sget(args,"question");
    const size_t qlen=strlen(question);
    if(!qlen||qlen>4000)return err(tr("调研问题不能为空且不能超过 4000 字节。"));
    const char *focus=sget(args,"path");
    if(*focus){
        char full[PATH_MAX];
        if(resolve_file(app,focus,full))return err(app->error);
    }

    App sub{};
    sub.config=copy(app->config);
    sub.messages=arr();
    snprintf(sub.root,sizeof sub.root,"%s",app->root);
    snprintf(sub.config_dir,sizeof sub.config_dir,"%s",app->config_dir);
    sub.yes=true;
    sub.shell=false;
    sub.desktop=false;
    sub.subagent=true;
    sub.max_steps=8;
    sub.task_events=-1;
    auto cleanup=[](App *value){app_cleanup(value);};
    std::unique_ptr<App,decltype(cleanup)> guard(&sub,cleanup);

    std::string prompt="You are a read-only codebase research subagent. "
        "Inspect the workspace with code_outline, list_files, search_text, and read_file, "
        "then return a concise, factual answer citing exact file paths and 1-based line numbers.\n\n";
    if(*focus&&strcmp(focus,".")!=0){
        prompt+="Focus path: ";
        prompt+=focus;
        prompt+="\n\n";
    }
    prompt+="Question:\n";
    prompt+=question;

    J *user=obj();
    put(user,"role",str("user"));
    put(user,"content",str(prompt.c_str()));
    add(sub.messages,user);

    JsonPtr sub_tools(schemas(&sub));
    std::vector<std::string> inspected;
    auto record_path=[&](const char *p){
        if(!p||!*p||inspected.size()>=32)return;
        if(std::find(inspected.begin(),inspected.end(),p)==inspected.end())
            inspected.emplace_back(p);
    };

    auto sync_tokens=[&]{
        if(sub.task_tokens){
            app->task_tokens=add_tokens(app->task_tokens,sub.task_tokens);
            sub.task_tokens=0;
            task_tokens(*app);
        }
    };

    std::string findings;
    int steps_used=0;
    for(int step=1;step<=8&&!interrupted;++step){
        steps_used=step;
        JsonPtr history(context(&sub,sub_tools.get()));
        if(!history)return err(sub.error[0]?sub.error:tr("子任务上下文超出预算。"));
        JsonPtr reply(api_complete(&sub,sub.config,history.get(),sub_tools.get()));
        sync_tokens();
        if(!reply)return err(sub.error[0]?sub.error:tr("子任务请求失败。"));
        J *calls=get(reply.get(),"tool_calls");
        if(!alen(calls)){
            findings=sget(reply.get(),"content");
            break;
        }
        if(alen(calls)>12)return err(tr("单轮工具数量过多。"));
        // Validate every call before executing.
        std::vector<JsonPtr> parsed_args;
        parsed_args.reserve(alen(calls));
        for(size_t i=0;i<alen(calls);i++){
            J *call=at(calls,i),*fn=get(call,"function");
            if(!*sget(call,"id")||!*sget(fn,"name"))return err(tr("工具调用格式无效。"));
            JsonPtr ca(parse_tool_arguments(sget(fn,"arguments")));
            if(!ca)return err(tr("工具参数不是完整的 JSON 对象，本轮操作未执行。"));
            parsed_args.push_back(std::move(ca));
        }
        add(sub.messages,reply.release());
        for(size_t i=0;i<alen(calls)&&!interrupted;i++){
            J *call=at(calls,i),*fn=get(call,"function");
            const char *cname=sget(fn,"name");
            record_path(sget(parsed_args[i].get(),"path"));
            JsonPtr res(tool_call(&sub,cname,parsed_args[i].get()));
            J *tm=obj();
            put(tm,"role",str("tool"));
            put(tm,"tool_call_id",str(sget(call,"id")));
            put(tm,"content",str(json_object_to_json_string_ext(res.get(),JSON_C_TO_STRING_PLAIN)));
            add(sub.messages,tm);
        }
    }

    if(interrupted)return err(tr("任务已停止。"));
    if(findings.empty()){
        J *ask=obj();
        put(ask,"role",str("user"));
        put(ask,"content",str("Summarize your findings so far concisely with exact file paths and line numbers."));
        add(sub.messages,ask);
        JsonPtr history(context(&sub,nullptr));
        if(history){
            JsonPtr final_reply(api_complete(&sub,sub.config,history.get(),nullptr));
            sync_tokens();
            if(final_reply)findings=sget(final_reply.get(),"content");
        }
    }
    if(findings.empty())return err(tr("子任务未返回有效结论。"));

    J *result=obj(),*files=arr();
    put(result,"findings",str(findings.c_str()));
    put(result,"steps",json_object_new_int(steps_used));
    for(const auto &p:inspected)add(files,str(p.c_str()));
    put(result,"inspected",files);
    return result;
}
