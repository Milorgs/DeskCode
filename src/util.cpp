#include "desk.hpp"
#include "ui.hpp"
#include "tasks.hpp"
#include <sys/stat.h>
#include <sys/random.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#include <fcntl.h>
#include <termios.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <time.h>
#include <algorithm>
#include <readline/readline.h>
char *read_bytes(const char *path,size_t limit,size_t *length){
    int fd=open(path,O_RDONLY|O_NOFOLLOW);
    if(fd<0)return NULL;
    struct stat st;
    if(fstat(fd,&st)||!S_ISREG(st.st_mode)||st.st_size<0||(size_t)st.st_size>limit){
        close(fd);
        errno=EFBIG;
        return NULL;
    }
    char *buffer=static_cast<char *>(malloc(limit+1));
    if(!buffer){
        close(fd);
        return NULL;
    }
    size_t n=0;
    while(n<limit){
        ssize_t r=read(fd,buffer+n,limit-n);
        if(r<0&&errno==EINTR)continue;
        if(r<0){
            free(buffer);
            close(fd);
            return NULL;
        }
        if(!r)break;
        n+=(size_t)r;
    }
    if(n==limit){
        char extra;
        ssize_t extra_count;
        do{extra_count=read(fd,&extra,1);}while(extra_count<0&&errno==EINTR);
        if(extra_count!=0){free(buffer);close(fd);errno=EFBIG;return NULL;}
    }
    close(fd);
    buffer[n]=0;
    if(length)*length=n;
    return buffer;
}
int mkdirs(const char *path,unsigned int mode){
    char temp[PATH_MAX];
    if(strlen(path)>=sizeof temp)return -1;
    strcpy(temp,path);
    for(char *p=temp+1;;p++){
        if(*p=='/'||!*p){
            char c=*p;
            *p=0;
            struct stat st;
            if(lstat(temp,&st)){
                if(errno!=ENOENT||mkdir(temp,mode))return -1;
            }
            else if(!S_ISDIR(st.st_mode)||S_ISLNK(st.st_mode))return -1;
            *p=c;
            if(!c)break;
        }
    }
    return 0;
}
int atomic_file(const char *path,const void *data,size_t len,unsigned int mode){
    char tmp[PATH_MAX];
    if(snprintf(tmp,sizeof tmp,"%s.tmp-XXXXXX",path)>=(int)sizeof tmp)return -1;
    int fd=mkstemp(tmp);
    if(fd<0)return -1;
    int rc=fchmod(fd,mode);
    size_t n=0;
    while(!rc&&n<len){
        ssize_t w=write(fd,(const char *)data+n,len-n);
        if(w<0&&errno==EINTR)continue;
        if(w<=0){
            rc=-1;
            break;
        }
        n+=(size_t)w;
    }
    if(!rc)rc=fsync(fd);
    if(close(fd)&&!rc)rc=-1;
    if(!rc)rc=rename(tmp,path);
    if(rc)unlink(tmp);
    return rc;
}
namespace {
// Consume the canonical EOF marker before switching the terminal to raw input.
// Queued request text stays untouched.
bool terminal_eof(){
    if(!isatty(STDIN_FILENO))return false;
    struct termios mode{};if(tcgetattr(STDIN_FILENO,&mode)||!(mode.c_lflag&ICANON))return false;
    struct pollfd ready{STDIN_FILENO,POLLIN,0};int bytes=0;
    if(poll(&ready,1,0)<=0||!(ready.revents&(POLLIN|POLLHUP))||ioctl(STDIN_FILENO,FIONREAD,&bytes)||bytes)return false;
    char marker;
    return read(STDIN_FILENO,&marker,1)==0;
}
struct EditorState {char *line=nullptr;bool done=false,installed=false;std::string action;};
EditorState *editor=nullptr;
ChatDraft *active_draft=nullptr;
bool activity_visible=false;
std::string last_activity;
void accepted_line(char *line){
    activity_visible=false;
    editor->line=line;editor->done=true;
    rl_callback_handler_remove();editor->installed=false;
}
int choose_chat_model(int,int){
    if(editor)editor->action="/model";
    return 0;
}
int choose_chat_reasoning(int,int){
    if(editor)editor->action="/reasoning";
    return 0;
}
int unbound_scroll_key(int,int){return rl_ding();}
int scroll_up(int,int){scroll_transcript_page(-1);return 0;}
int scroll_down(int,int){scroll_transcript_page(1);return 0;}
int scroll_line_up(int,int){scroll_transcript(-1);return 0;}
int scroll_line_down(int,int){scroll_transcript(1);return 0;}
// In alternate-scroll mode the terminal turns each wheel step into one Up or
// Down key, so a single line per key matches the wheel's own step size.
int scroll_wheel_up(int,int){scroll_transcript(-1);return 0;}
int scroll_wheel_down(int,int){scroll_transcript(1);return 0;}
int scroll_top(int,int){scroll_transcript(-1000000);return 0;}
int scroll_bottom(int,int){scroll_transcript_bottom();return 0;}
int complete_chat_tab(int,int){
    if(!active_draft||!editor)return rl_ding();
    size_t at_pos=0;
    std::string query;
    std::string_view buffer(rl_line_buffer,static_cast<size_t>(rl_end));
    if(!find_at_mention(buffer,static_cast<size_t>(rl_point),at_pos,query))return rl_ding();
    active_draft->complete_file=true;
    editor->action="@";
    return 0;
}
int paste_clipboard_hotkey(int,int){
    if(!active_draft||!editor)return rl_ding();
    active_draft->paste_image=true;
    editor->action="/paste";
    return 0;
}
int insert_newline_glyph(int,int){
    if(!active_draft)return rl_ding();
    active_draft->newline_inserted=true;
    rl_insert_text("\xE2\x86\xB5");
    rl_redisplay();
    return 0;
}
int paste_bracketed(int,int){
    static constexpr std::string_view end_marker="\033[201~";
    std::string raw,tail;
    bool oversized=false;
    while(!interrupted){
        int key=rl_read_key();
        if(key<0)break;
        char ch=static_cast<char>(key);
        tail+=ch;
        if(tail.size()>end_marker.size())tail.erase(0,tail.size()-end_marker.size());
        if(raw.size()<65536+end_marker.size())raw+=ch;
        else oversized=true;
        if(tail==end_marker)break;
    }
    if(raw.ends_with(end_marker))raw.resize(raw.size()-end_marker.size());
    if(oversized){
        terminal_message(std::string("  ")+tr("输入超过 64 KB，请缩短后重试。")+"\n");
        return 0;
    }
    std::string normalized;
    normalized.reserve(raw.size());
    for(size_t i=0;i<raw.size();i++){
        char c=raw[i];
        if(c=='\r'){
            normalized+='\n';
            if(i+1<raw.size()&&raw[i+1]=='\n')i++;
            continue;
        }
        auto u=static_cast<unsigned char>(c);
        if((u<32&&c!='\n'&&c!='\t')||u==127)continue;
        normalized+=c;
    }
    size_t used=0;
    std::string clean=display_utf8(normalized,used,true);
    while(!clean.empty()&&clean.back()=='\n')clean.pop_back();
    if(clean.empty())return 0;
    if(clean.find('\n')==std::string::npos||!active_draft){
        for(char &c:clean)if(c=='\t')c=' ';
        rl_insert_text(clean.c_str());
        rl_redisplay();
        return 0;
    }
    size_t lines=1;
    for(char c:clean)if(c=='\n')lines++;
    char label[96];
    snprintf(label,sizeof label,tr("[粘贴 #%d · %zu 行]"),active_draft->next_paste++,lines);
    active_draft->pastes.push_back({label,std::move(clean)});
    rl_insert_text(label);
    rl_redisplay();
    return 0;
}
// Recall of earlier requests. The draft typed before the first recall is kept
// and comes back after stepping past the newest entry.
const std::vector<std::string> *recall_history=nullptr;
size_t recall_position=0;
bool recalling=false;
std::string recall_stash;
void recall_show(const std::string &text){
    rl_replace_line(text.c_str(),0);
    rl_point=rl_end;
}
int recall_previous(int,int){
    if(!recall_history||recall_history->empty())return rl_ding();
    if(!recalling){
        recall_stash.assign(rl_line_buffer,static_cast<size_t>(rl_end));
        recall_position=recall_history->size();
        recalling=true;
    }
    if(!recall_position)return rl_ding();
    recall_show((*recall_history)[--recall_position]);
    return 0;
}
int recall_next(int,int){
    if(!recalling||!recall_history)return rl_ding();
    if(recall_position+1<recall_history->size()){
        recall_show((*recall_history)[++recall_position]);
        return 0;
    }
    recalling=false;
    recall_show(recall_stash);
    recall_stash.clear();
    return 0;
}
int scroll_mouse(int,int){
    char report[32]{};
    for(size_t i=0;i<sizeof(report)-1;i++){
        int key=rl_read_key();
        if(key=='m'||key=='M'){
            int button=0,column=0,row=0;
            if(sscanf(report,"%d;%d;%d",&button,&column,&row)==3){
                const int code=button&~28;
                if(key=='M'&&code==64)scroll_transcript(-1);
                else if(key=='M'&&code==65)scroll_transcript(1);
                else scroll_transcript_mouse(row,column,code,key=='m');
            }
            break;
        }
        if(key!=';'&&(key<'0'||key>'9'))break;
        report[i]=static_cast<char>(key);
    }
    return 0;
}
struct EditorSession {
    EditorState state;
    Keymap scroll_keymap=nullptr;
    std::vector<std::pair<const char *,rl_command_func_t *>> previous_scroll_bindings;
    bool mouse_reporting=false;
    bool bracketed_paste=false;
    explicit EditorSession(const char *prompt){
        editor=&state;
        rl_readline_name="desk";
        rl_catch_signals=0;rl_catch_sigwinch=0;
        rl_callback_handler_install(prompt,accepted_line);state.installed=true;
        // Keep editing predictable and never complete or save private input.
        rl_bind_key('\t',rl_insert);
        rl_variable_bind("horizontal-scroll-mode","on");
    }
    // Bind a key for this session only; the destructor puts the previous
    // binding back. Sequences that are prefixes of longer ones stay untouched.
    bool bind(const char *sequence,rl_command_func_t *handler){
        if(!scroll_keymap)scroll_keymap=rl_get_keymap();
        int type=ISFUNC;
        auto previous=rl_function_of_keyseq(sequence,scroll_keymap,&type);
        if(type!=ISFUNC)return false;
        if(rl_bind_keyseq_in_map(sequence,handler,scroll_keymap))return false;
        previous_scroll_bindings.emplace_back(sequence,previous);
        return true;
    }
    void enable_chat_editing(ChatDraft *draft){
        active_draft=draft;
        rl_bind_key('\t',complete_chat_tab);
        bind("\026",paste_clipboard_hotkey);
        bind("\033\r",insert_newline_glyph);
        bind("\033\n",insert_newline_glyph);
        rl_variable_bind("enable-bracketed-paste","off");
        if(bind("\033[200~",paste_bracketed)){
            bracketed_paste=true;
            fputs("\033[?2004h",stdout);
            fflush(stdout);
        }
    }
    void enable_history(const std::vector<std::string> *history){
        recall_history=history;
        recalling=false;
        recall_stash.clear();
        if(!history)return;
        bind("\020",recall_previous);
        bind("\016",recall_next);
        bind("\033[1;3A",recall_previous);
        bind("\033[1;3B",recall_next);
    }
    void enable_scrolling(){
        if(!transcript_scrollable())return;
        bind("\033[5~",scroll_up);
        bind("\033[5;2~",scroll_up);
        bind("\033[6~",scroll_down);
        bind("\033[6;2~",scroll_down);
        bind("\033[1;5A",scroll_line_up);
        bind("\033[1;5B",scroll_line_down);
        bind("\033[1;5H",scroll_top);
        bind("\033[1;5F",scroll_bottom);
        // The chat input is one line without history, so Up/Down are free to
        // scroll the transcript, including terminals that translate wheel events
        // into arrow keys through alternate-scroll mode.
        bind("\033[A",scroll_wheel_up);
        bind("\033[B",scroll_wheel_down);
        bind("\033OA",scroll_wheel_up);
        bind("\033OB",scroll_wheel_down);
        // Ensure alternate-scroll mode is active on the alternate screen; do not
        // use XTSAVE/XTRESTORE (?1007s/?1007r) because Konsole saves the unset VT
        // bit even when profile AlternateScrolling is enabled, turning it off on
        // restore. Mode 1007 only affects the alternate screen anyway.
        fputs("\033[?1007h",stdout);
        fflush(stdout);

        mouse_reporting=bind("\033[<",scroll_mouse);
        if(mouse_reporting){
            fputs("\033[?1000h\033[?1002h\033[?1006h",stdout);
            fflush(stdout);
        }
    }
    ~EditorSession(){
        scroll_transcript_mouse(0,0,0,true);
        active_draft=nullptr;
        recall_history=nullptr;
        recalling=false;
        recall_stash.clear();
        if(bracketed_paste){
            fputs("\033[?2004l",stdout);
            fflush(stdout);
            rl_variable_bind("enable-bracketed-paste","on");
        }
        if(mouse_reporting){
            fputs("\033[?1006l\033[?1002l\033[?1000l",stdout);
            fflush(stdout);
        }
        rl_bind_key('\t',rl_insert);
        // Keep Readline's existing bindings. Use its usual bell for formerly
        // unbound keys so removing a sequence cannot leak a keymap node.
        for(const auto &[sequence,previous]:previous_scroll_bindings)
            rl_bind_keyseq_in_map(sequence,previous?previous:unbound_scroll_key,scroll_keymap);
        for(const char *sequence:{"\033OQ","\033OR","\033[12~","\033[13~"})rl_bind_keyseq(sequence,rl_insert);
        if(state.installed)rl_callback_handler_remove();
        editor=nullptr;
        activity_visible=false;
    }
};
}
void terminal_message(const std::string &text){
    if(text.empty())return;
    activity_visible=false;
    if(transcript_append(text))return;
    if(!editor||!editor->installed){fwrite(text.data(),1,text.size(),stdout);fflush(stdout);return;}
    std::string line=rl_line_buffer;
    int point=rl_point;
    rl_save_prompt();rl_replace_line("",0);rl_redisplay();
    fputs("\r\033[2K",stdout);
    fwrite(text.data(),1,text.size(),stdout);
    if(text.back()!='\n')putchar('\n');
    rl_restore_prompt();rl_replace_line(line.c_str(),0);rl_point=point;
    rl_on_new_line();rl_redisplay();fflush(stdout);
}
void terminal_activity(const std::string &text){
    if(transcript_set_activity(text))return;
    if(!editor||!editor->installed){
        activity_visible=false;
        return;
    }
    if(!activity_visible){
        if(text.empty())return;
        terminal_message(text+"\n");
        activity_visible=true;
        last_activity=text;
        return;
    }
    if(text==last_activity)return;
    last_activity=text;
    fputs("\0337\033[1A\r\033[2K",stdout);
    fwrite(text.data(),1,text.size(),stdout);
    fputs("\0338",stdout);
    fflush(stdout);
    if(text.empty())activity_visible=false;
}
char *editable_input(const char *prompt,const char *fallback,bool secret,const std::function<void()> &idle,ChatDraft *draft){
    if(input_closed)return nullptr;
    if(!secret&&isatty(STDIN_FILENO)&&isatty(STDOUT_FILENO)){
        if(terminal_eof()){input_closed=true;putchar('\n');return nullptr;}
        EditorSession session(prompt);
        if(draft){
            draft->paused=false;
            draft->complete_file=false;
            draft->paste_image=false;
            session.enable_chat_editing(draft);
            session.enable_scrolling();
            session.enable_history(draft->history);
            rl_bind_keyseq("\033OQ",choose_chat_model);
            rl_bind_keyseq("\033OR",choose_chat_reasoning);
            rl_bind_keyseq("\033[12~",choose_chat_model);
            rl_bind_keyseq("\033[13~",choose_chat_reasoning);
            if(!draft->text.empty()){
                rl_replace_line(draft->text.c_str(),0);
                rl_point=std::clamp(draft->cursor,0,rl_end);
                rl_redisplay();
            }
        }
        struct winsize previous{};ioctl(STDOUT_FILENO,TIOCGWINSZ,&previous);
        double slash_started=0;
        while(!session.state.done&&!interrupted){
            if(idle)idle();
            if(draft&&!session.state.done&&slash_started>0){
                if(rl_end==1&&rl_line_buffer[0]=='/'){
                    if(monotime()-slash_started>=0.25){
                        draft->text.clear();draft->cursor=0;
                        fputs("\r\033[2K",stdout);
                        return strdup("/");
                    }
                }else slash_started=0;
            }
            if(draft&&draft->approval_pending&&draft->approval_pending()){
                scroll_transcript_bottom();
                draft->text.assign(rl_line_buffer,static_cast<size_t>(rl_end));
                draft->cursor=rl_point;
                draft->paused=true;
                fputs("\r\033[2K",stdout);
                return nullptr;
            }
            struct pollfd ready{STDIN_FILENO,POLLIN,0};int rc=poll(&ready,1,50);
            if(interrupted)break;
            if(rc<0&&errno!=EINTR)break;
            struct winsize current{};
            if(!ioctl(STDOUT_FILENO,TIOCGWINSZ,&current)&&(current.ws_col!=previous.ws_col||current.ws_row!=previous.ws_row)){
                previous=current;
                fputs("\r\033[2K",stdout);
                if(idle)idle();
                if(transcript_scrollable()&&current.ws_row>=8)printf("\033[%d;1H\033[2K",current.ws_row-2);
                rl_on_new_line();
                rl_set_screen_size(current.ws_row,current.ws_col);
                rl_forced_update_display();
            }
            if(rc>0&&(ready.revents&(POLLIN|POLLHUP)))rl_callback_read_char();
            if(draft&&!session.state.done&&!session.state.action.empty()){
                draft->text.assign(rl_line_buffer,static_cast<size_t>(rl_end));
                draft->cursor=rl_point;
                fputs("\r\033[2K",stdout);
                return strdup(session.state.action.c_str());
            }
            if(draft&&!session.state.done&&rl_last_func==rl_insert&&rl_end==1&&rl_line_buffer[0]=='/'){
                if(slash_started==0)slash_started=monotime();
            }else if(rl_end!=1||rl_line_buffer[0]!='/'){
                slash_started=0;
            }
            if(!session.state.done&&rl_end>65536){
                rl_replace_line("",0);rl_crlf();puts(tr("输入超过 64 KB，请缩短后重试。"));rl_on_new_line();rl_redisplay();
            }
        }
        if(interrupted){
            if(draft){draft->text.clear();draft->cursor=0;}
            rl_replace_line("",0);rl_crlf();free(session.state.line);return nullptr;
        }
        if(!session.state.done)return nullptr;
        char *line=session.state.line;
        if(!line){
            if(draft){draft->text.clear();draft->cursor=0;}
            input_closed=true;putchar('\n');return nullptr;
        }
        std::string resolved=line;
        free(line);
        if(draft){
            draft->text.clear();draft->cursor=0;
            expand_draft_input(resolved,*draft);
        }
        if(resolved.size()>65536){puts(tr("输入超过 64 KB，请缩短后重试。"));return strdup("");}
        if(resolved.empty()&&fallback)return strdup(fallback);
        return strdup(resolved.c_str());
    }
    printf("%s",prompt);fflush(stdout);
    struct termios old{};bool changed=false;
    if(secret&&isatty(STDIN_FILENO)&&!tcgetattr(STDIN_FILENO,&old)){
        struct termios mode=old;mode.c_lflag&=~ECHO;
#ifdef IUTF8
        mode.c_iflag|=IUTF8;
#endif
        changed=!tcsetattr(STDIN_FILENO,TCSANOW,&mode);
    }
    char *line=nullptr;size_t capacity=0;ssize_t n=getline(&line,&capacity,stdin);
    if(changed){tcsetattr(STDIN_FILENO,TCSANOW,&old);putchar('\n');}
    if(n<0){if(feof(stdin)&&!interrupted)input_closed=true;free(line);clearerr(stdin);return nullptr;}
    if(n>65536){free(line);return nullptr;}
    while(n>0&&(line[n-1]=='\n'||line[n-1]=='\r'))line[--n]=0;
    if(!n&&fallback){free(line);return strdup(fallback);}
    return line;
}
char *input(const char *prompt,const char *fallback,bool secret){return editable_input(prompt,fallback,secret,{});}

void check_exit(const App &app){
    if(app.task_events<0&&!input_closed&&terminal_eof()){input_closed=true;interrupted=SIGINT;}
}
