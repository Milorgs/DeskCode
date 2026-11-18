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
struct TranscriptViewport {
    int first=1,last=0,columns=0;
    std::string transcript,activity;
    std::vector<std::string> lines;
    size_t top=0;
    bool following=true;
    // The chat is open but the frame is suspended; keep collecting lines.
    bool recording=false;
    bool dragging=false;
    int drag_offset=0;
    size_t drag_start_top=0;
    int drag_start_row=0;

    int text_columns() const {
        return columns>=24?columns-3:std::max(1,columns);
    }

    size_t height() const {
        return last>=first?static_cast<size_t>(last-first+1):0;
    }

    size_t total() const {
        return lines.size()+!activity.empty();
    }

    size_t tail() const {
        const size_t h=height(),count=total();
        return count>h?count-h:0;
    }

    bool has_scrollbar() const {
        return last>=first&&columns>=24&&tail()>0;
    }

    // Proportional thumb {offset, length} within 0..height()-1. When scrolled
    // strictly between top and bottom on a tall enough track, keep a one-cell
    // gap at each end so the user can always tell they are not at an extreme.
    std::pair<size_t,size_t> scrollbar_thumb() const {
        const size_t h=height(),count=total(),max_top=tail();
        if(!h||!max_top)return {0,h};
        const size_t len=std::clamp((h*h+count/2)/count,size_t{1},h>1?h-1:size_t{1});
        const size_t travel=h-len;
        if(!travel||!top)return {0,len};
        if(top>=max_top)return {travel,len};
        size_t pos=(top*travel+max_top/2)/max_top;
        if(travel>=3)pos=std::clamp(pos,size_t{1},travel-1);
        return {pos,len};
    }

    void append_rows(std::string_view text){
        const int limit=text_columns();
        std::string row,style;
        int cells=0,indent=0;
        bool leading=true;
        for(size_t i=0;i<text.size();){
            auto c=static_cast<unsigned char>(text[i]);
            if(c==0x1b&&i+1<text.size()&&text[i+1]=='['){
                const size_t start=i;
                i+=2;
                while(i<text.size()&&!(text[i]>='@'&&text[i]<='~'))i++;
                if(i<text.size()&&text[i++]=='m'){
                    auto sgr=text.substr(start,i-start);
                    row+=sgr;
                    if(sgr=="\033[0m"||sgr=="\033[m")style.clear();
                    else style+=sgr;
                }
                continue;
            }
            if(c=='\n'){
                lines.push_back(std::move(row));
                row=style;
                cells=0;
                indent=0;
                leading=true;
                i++;
                continue;
            }
            if(c=='\t'){
                leading=false;
                int count=std::min(8-cells%8,limit-cells);
                row.append(count,' ');
                cells+=count;
                i++;
                continue;
            }
            if(c<32||c==127){i++;continue;}
            if(leading&&c==' '&&indent<4&&indent+8<limit)indent++;
            else leading=false;
            size_t size=c<0x80?1:c<0xe0?2:c<0xf0?3:c<0xf8?4:1;
            size=std::min(size,text.size()-i);
            auto glyph=text.substr(i,size);
            int width=ui_width(glyph);
            if(cells+width>limit){
                lines.push_back(std::move(row));
                row=style;
                cells=0;
                if(indent>0&&indent+width<limit){
                    row.append(static_cast<size_t>(indent),' ');
                    cells=indent;
                }
            }
            row+=glyph;
            cells+=width;
            i+=glyph.size();
        }
    }

    void draw_scrollbar_cell(int row,std::pair<size_t,size_t> thumb) const {
        const size_t slot=static_cast<size_t>(row-first);
        const bool in_thumb=slot>=thumb.first&&slot<thumb.first+thumb.second;
        printf("\033[%d;%dH",row,columns-1);
        if(in_thumb)fputs(ui_paint(following?Tone::Dim:Tone::Accent,"┃").c_str(),stdout);
        else fputs(ui_paint(Tone::Rule,"│").c_str(),stdout);
    }

    void draw_scrollbar() const {
        if(!has_scrollbar())return;
        const auto thumb=scrollbar_thumb();
        fputs("\0337",stdout);
        for(int row=first;row<=last;row++)draw_scrollbar_cell(row,thumb);
        fputs("\0338",stdout);
        fflush(stdout);
    }

    void draw_row(int row,bool bar,std::pair<size_t,size_t> thumb) const {
        printf("\033[%d;1H\033[2K",row);
        size_t i=top+static_cast<size_t>(row-first);
        if(i<lines.size())fputs(lines[i].c_str(),stdout);
        else if(i==lines.size()&&!activity.empty())
            fputs(ui_paint(Tone::Accent,ui_clip("  "+activity,text_columns())).c_str(),stdout);
        fputs("\033[0m",stdout);
        if(bar)draw_scrollbar_cell(row,thumb);
    }

    void draw() const {
        if(last==0)return;
        const bool bar=has_scrollbar();
        const auto thumb=bar?scrollbar_thumb():std::pair<size_t,size_t>{0,0};
        fputs("\0337\033[?7l",stdout);
        for(int row=first;row<=last;row++)draw_row(row,bar,thumb);
        fputs("\033[?7h\0338",stdout);
        fflush(stdout);
    }

    void set_activity(const std::string &text){
        if(activity==text)return;
        const bool height_changed=activity.empty()!=text.empty();
        activity=text;
        if(height_changed){
            top=following?tail():std::min(top,tail());
            draw();
            return;
        }
        const size_t offset=lines.size()-top;
        if(offset>static_cast<size_t>(last-first))return;
        const bool bar=has_scrollbar();
        fputs("\0337\033[?7l",stdout);
        draw_row(first+static_cast<int>(offset),bar,bar?scrollbar_thumb():std::pair<size_t,size_t>{0,0});
        fputs("\033[?7h\0338",stdout);
        fflush(stdout);
    }

    void scroll(int rows){
        if(!last)return;
        size_t next=rows<0?top-std::min(top,static_cast<size_t>(-rows))
                          :std::min(tail(),top+static_cast<size_t>(rows));
        following=next==tail();
        if(next!=top){top=next;draw();}
    }

    bool scroll_mouse_at(int row,int col,int button,bool release){
        if(release){bool handled=dragging;dragging=false;return handled;}
        if(!has_scrollbar()){dragging=false;return false;}
        const auto [position,length]=scrollbar_thumb();
        if(button==0){
            if(col<columns-2||col>columns||row<first||row>last)return false;
            dragging=true;
            const int slot=row-first;
            if(slot>=static_cast<int>(position)&&slot<static_cast<int>(position+length)){
                drag_offset=slot-static_cast<int>(position);
                drag_start_top=top;
                drag_start_row=row;
                return true;
            }
            drag_offset=static_cast<int>(length/2);
        }else if(button!=32||!dragging)return false;
        const size_t travel=height()-length;
        if(!travel)return true;
        const int slot=std::clamp(row-first-drag_offset,0,static_cast<int>(travel));
        size_t next=static_cast<size_t>(slot)*tail()/travel;
        if(button==32&&row==drag_start_row)next=std::min(drag_start_top,tail());
        following=next==tail();
        if(next!=top){top=next;draw();}
        if(button==0){drag_start_top=top;drag_start_row=row;}
        return true;
    }
};
static TranscriptViewport transcript_view;
EditorState *editor=nullptr;
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
int scroll_up(int,int){transcript_view.scroll(-std::max(1,transcript_view.last-transcript_view.first));return 0;}
int scroll_down(int,int){transcript_view.scroll(std::max(1,transcript_view.last-transcript_view.first));return 0;}
int scroll_line_up(int,int){transcript_view.scroll(-1);return 0;}
int scroll_line_down(int,int){transcript_view.scroll(1);return 0;}
// In alternate-scroll mode the terminal turns each wheel step into one Up or
// Down key, so a single line per key matches the wheel's own step size.
int scroll_wheel_up(int,int){transcript_view.scroll(-1);return 0;}
int scroll_wheel_down(int,int){transcript_view.scroll(1);return 0;}
int scroll_top(int,int){transcript_view.scroll(-1000000);return 0;}
int scroll_bottom(int,int){transcript_view.scroll(1000000);return 0;}
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
                if(key=='M'&&code==64)transcript_view.scroll(-1);
                else if(key=='M'&&code==65)transcript_view.scroll(1);
                else transcript_view.scroll_mouse_at(row,column,code,key=='m');
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
        if(!transcript_view.last)return;
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
        transcript_view.dragging=false;
        recall_history=nullptr;
        recalling=false;
        recall_stash.clear();
        if(mouse_reporting){
            fputs("\033[?1006l\033[?1002l\033[?1000l",stdout);
            fflush(stdout);
        }
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
void reset_transcript(){transcript_view={};}
void scroll_transcript(int rows){transcript_view.scroll(rows);}
void scroll_transcript_page(int direction){
    transcript_view.scroll(direction*std::max(1,transcript_view.last-transcript_view.first));
}
bool scroll_transcript_mouse(int row,int col,int button,bool release){return transcript_view.scroll_mouse_at(row,col,button,release);}
void scroll_transcript_bottom(){transcript_view.scroll(INT_MAX);}
bool transcript_scrollable(){return transcript_view.last!=0;}
size_t transcript_hidden_lines(){
    if(!transcript_view.last||transcript_view.following)return 0;
    const size_t bottom=transcript_view.tail();
    return bottom>transcript_view.top?bottom-transcript_view.top:0;
}
void set_composer_layout(int input_row,int transcript_row,int first_row,bool keep_transcript){
    auto &view=transcript_view;
    view.first=first_row;
    view.last=input_row?transcript_row:0;
    view.recording=!view.last&&keep_transcript;
    if(!view.last)return;
    int columns=std::max(1,terminal_columns());
    if(columns!=view.columns){
        view.columns=columns;
        view.lines.clear();
        view.append_rows(view.transcript);
    }
    view.top=view.following?view.tail():std::min(view.top,view.tail());
    view.draw();
    // The input row hosts readline or the inline approval choices; either way
    // the cursor belongs there after a relayout.
    printf("\033[%d;1H",input_row);
}
void terminal_message(const std::string &text){
    if(text.empty())return;
    activity_visible=false;
    auto &view=transcript_view;
    if(view.last){
        std::string body=text;
        if(body.back()!='\n')body+='\n';
        view.transcript+=body;
        view.append_rows(body);
        if(view.following){view.top=view.tail();view.draw();}
        else view.draw_scrollbar();
        return;
    }
    if(view.recording){
        view.transcript+=text;
        if(text.back()!='\n')view.transcript+='\n';
        view.columns=0;
    }
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
    if(transcript_view.last){transcript_view.set_activity(text);return;}
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
            session.enable_scrolling();
            session.enable_history(draft->history);
            rl_bind_keyseq("\033OQ",choose_chat_model);
            rl_bind_keyseq("\033OR",choose_chat_reasoning);
            rl_bind_keyseq("\033[12~",choose_chat_model);
            rl_bind_keyseq("\033[13~",choose_chat_reasoning);
            if(!draft->text.empty()){
                rl_replace_line(draft->text.c_str(),0);
                rl_point=draft->cursor;
                rl_redisplay();
            }
        }
        struct winsize previous{};ioctl(STDOUT_FILENO,TIOCGWINSZ,&previous);
        while(!session.state.done&&!interrupted){
            if(idle)idle();
            if(draft&&draft->approval_pending&&draft->approval_pending()){
                transcript_view.scroll(INT_MAX);
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
                if(transcript_view.last&&current.ws_row>=8)printf("\033[%d;1H\033[2K",current.ws_row-2);
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
                draft->text.clear();draft->cursor=0;
                fputs("\r\033[2K",stdout);
                return strdup("/");
            }
            if(!session.state.done&&rl_end>65536){
                rl_replace_line("",0);rl_crlf();puts(tr("输入超过 64 KB，请缩短后重试。"));rl_on_new_line();rl_redisplay();
            }
        }
        if(draft){draft->text.clear();draft->cursor=0;}
        if(interrupted){rl_replace_line("",0);rl_crlf();free(session.state.line);return nullptr;}
        if(!session.state.done)return nullptr;
        char *line=session.state.line;
        if(!line){input_closed=true;putchar('\n');return nullptr;}
        if(strlen(line)>65536){free(line);puts(tr("输入超过 64 KB，请缩短后重试。"));return strdup("");}
        if(!*line&&fallback){free(line);return strdup(fallback);}
        return line;
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
