#include "desk.hpp"
#include <cstdlib>
#include <cstring>
#include <algorithm>
#include "ui.hpp"
#include <cctype>
#include <cerrno>
#include <cstdio>
#ifdef _WIN32
#include <windows.h>
#include <cstdarg>
#include <unordered_map>
#else
#include <sys/ioctl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#endif
#ifdef _WIN32
namespace {
int fallback_terminal_columns=0;
bool fallback_terminal_ansi=false;
}
void set_worker_terminal(int columns,bool ansi){
    fallback_terminal_columns=columns;
    fallback_terminal_ansi=ansi;
}
#endif
int terminal_rows(){
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if(!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE),&info))return 0;
    return info.srWindow.Bottom-info.srWindow.Top+1;
#else
    struct winsize size{};
    return ioctl(STDOUT_FILENO,TIOCGWINSZ,&size)==0?size.ws_row:0;
#endif
}
int terminal_columns(){
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if(!GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE),&info))return fallback_terminal_columns;
    return info.srWindow.Bottom>=info.srWindow.Top?info.srWindow.Right-info.srWindow.Left+1:fallback_terminal_columns;
#else
    struct winsize size{};
    return ioctl(STDOUT_FILENO,TIOCGWINSZ,&size)==0?size.ws_col:0;
#endif
}
bool terminal_supports_ansi(){
#ifdef _WIN32
    HANDLE output=GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD mode=0;
    if(!GetConsoleMode(output,&mode))return fallback_terminal_ansi;
    constexpr DWORD vt=ENABLE_PROCESSED_OUTPUT|ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    return (mode&vt)==vt||SetConsoleMode(output,mode|vt);
#else
    const char *term=env_value("TERM");
    return isatty(STDOUT_FILENO)&&term&&*term&&strcmp(term,"dumb")!=0;
#endif
}
bool terminal_supports_color(){
    const char *no_color=env_value("NO_COLOR");
    return (!no_color||!*no_color)&&terminal_supports_ansi();
}
#ifdef _WIN32
std::wstring wide(std::string_view value){
    if(value.empty())return {};
    int size=MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0);
    if(!size)throw std::runtime_error("Invalid UTF-8");
    std::wstring result(static_cast<size_t>(size),L'\0');
    MultiByteToWideChar(CP_UTF8,MB_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size);return result;
}
std::string utf8(std::wstring_view value){
    if(value.empty())return {};
    int size=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),nullptr,0,nullptr,nullptr);
    if(!size)throw std::runtime_error("Invalid UTF-16");
    std::string result(static_cast<size_t>(size),'\0');
    WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),static_cast<int>(value.size()),result.data(),size,nullptr,nullptr);return result;
}
const char *env_value(const char *name){
    static thread_local std::unordered_map<std::string,std::string> values;
    auto key=wide(name);DWORD size=GetEnvironmentVariableW(key.c_str(),nullptr,0);if(!size)return nullptr;
    std::wstring value(size,L'\0');DWORD used=GetEnvironmentVariableW(key.c_str(),value.data(),size);if(!used||used>=size)return nullptr;
    value.resize(used);values[name]=utf8(value);return values[name].c_str();
}
int asprintf(char **result,const char *format,...){
    *result=nullptr;va_list args;va_start(args,format);va_list count;va_copy(count,args);int size=_vscprintf(format,count);va_end(count);
    if(size<0){va_end(args);return -1;}*result=static_cast<char *>(malloc(static_cast<size_t>(size)+1));
    if(!*result){va_end(args);return -1;}vsnprintf(*result,static_cast<size_t>(size)+1,format,args);va_end(args);return size;
}
#endif
std::string config_base(){
    const char *value=env_value("XDG_CONFIG_HOME");if(value&&*value)return value;
#ifdef _WIN32
    value=env_value("LOCALAPPDATA");if(value&&*value)return value;
    value=env_value("USERPROFILE");return std::string(value?value:".")+"/AppData/Local";
#else
    value=env_value("HOME");return std::string(value?value:".")+"/.config";
#endif
}
std::string state_base(){
    const char *value=env_value("XDG_STATE_HOME");if(value&&*value)return value;
#ifdef _WIN32
    value=env_value("LOCALAPPDATA");if(value&&*value)return value;
    value=env_value("USERPROFILE");return std::string(value?value:".")+"/AppData/Local";
#else
    value=env_value("HOME");return std::string(value?value:".")+"/.local/state";
#endif
}
char *canonical_directory(const char *path,char *out){
    std::error_code ec;auto resolved=fs::canonical(fs_path(path),ec);if(ec||!fs::is_directory(resolved,ec))return nullptr;
    auto text=path_text(resolved);if(text.size()>=PATH_MAX)return nullptr;memcpy(out,text.c_str(),text.size()+1);return out;
}
bool path_inside(const fs::path &root,const fs::path &path){
#ifdef _WIN32
    // generic_u8string() uses '/', while canonical Windows paths can use '\\'.
    // Normalize separators before comparing individual components.
    auto preferred_root=root,preferred_path=path;
    preferred_root.make_preferred();preferred_path.make_preferred();
    auto a=preferred_root.begin(),b=preferred_path.begin();
    const auto a_end=preferred_root.end(),b_end=preferred_path.end();
#else
    auto a=root.begin(),b=path.begin();
    const auto a_end=root.end(),b_end=path.end();
#endif
    for(;a!=a_end;++a,++b){
        if(b==b_end)return false;
#ifdef _WIN32
        if(CompareStringOrdinal(a->native().c_str(),-1,b->native().c_str(),-1,TRUE)!=CSTR_EQUAL)return false;
#else
        if(*a!=*b)return false;
#endif
    }
    return true;
}
bool path_link(const fs::path &path){
#ifdef _WIN32
    DWORD attributes=GetFileAttributesW(path.c_str());return attributes!=INVALID_FILE_ATTRIBUTES&&(attributes&FILE_ATTRIBUTE_REPARSE_POINT);
#else
    std::error_code ec;return fs::is_symlink(fs::symlink_status(path,ec));
#endif
}
#ifndef _WIN32
void platform_initialize(){}
const char *env_value(const char *name){return std::getenv(name);}
#endif

namespace {
struct TranscriptViewport {
    int first=1,last=0,columns=0;
    std::string transcript,activity;
    std::vector<std::string> lines;
    size_t top=0;
    bool following=true;
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
                row.append(static_cast<size_t>(count),' ');
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
TranscriptViewport transcript_view;
}
void reset_transcript(){transcript_view={};}
void scroll_transcript(int rows){transcript_view.scroll(rows);}
void scroll_transcript_page(int direction){
    transcript_view.scroll(direction*std::max(1,transcript_view.last-transcript_view.first));
}
bool scroll_transcript_mouse(int row,int col,int button,bool release){
    return transcript_view.scroll_mouse_at(row,col,button,release);
}
void scroll_transcript_bottom(){transcript_view.scroll(1000000);}
bool transcript_scrollable(){return transcript_view.last!=0;}
size_t transcript_hidden_lines(){
    if(!transcript_view.last||transcript_view.following)return 0;
    const size_t bottom=transcript_view.tail();
    return bottom>transcript_view.top?bottom-transcript_view.top:0;
}
void set_composer_layout(int input_row,int transcript_row,int first_row,bool keep_transcript){
    auto &view=transcript_view;
    view.dragging=false;
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
    printf("\033[%d;1H",input_row);
}
bool transcript_append(const std::string &text){
    auto &view=transcript_view;
    if(view.last){
        std::string body=text;
        if(body.back()!='\n')body+='\n';
        view.transcript+=body;
        view.append_rows(body);
        if(view.following){view.top=view.tail();view.draw();}
        else view.draw_scrollbar();
        return true;
    }
    if(view.recording){
        view.transcript+=text;
        if(text.back()!='\n')view.transcript+='\n';
        view.columns=0;
    }
    return false;
}
bool transcript_set_activity(const std::string &text){
    if(!transcript_view.last)return false;
    transcript_view.set_activity(text);
    return true;
}

std::optional<int> terminal_choice(const std::vector<std::string> &items,int initial,int top,const std::function<void()> &on_idle,bool transient,bool inline_row,bool allow_escape,const std::function<void()> &on_resize){
    if(items.empty())return 0;
    if(input_closed||interrupted==SIGTERM)return -1;
    if(!isatty(STDIN_FILENO)||!terminal_supports_ansi()||env_value("DESK_NO_ARROW"))return std::nullopt;
    interrupted=0;
    enum {idle=-1,end_input=-2,ignored=-3,up=256,down=257,home=258,end=259,page_up=260,page_down=261};
    const bool inline_scroll=inline_row&&transcript_scrollable();
#ifdef _WIN32
    HANDLE console=GetStdHandle(STD_INPUT_HANDLE);
    DWORD original=0;
    if(!GetConsoleMode(console,&original))return std::nullopt;
    struct RestoreConsoleMode {
        HANDLE handle;DWORD mode;bool changed=false;
        RestoreConsoleMode(HANDLE h,DWORD m,bool mouse):handle(h),mode(m){
            if(mouse)changed=SetConsoleMode(handle,(mode|ENABLE_MOUSE_INPUT|ENABLE_EXTENDED_FLAGS)&~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE))!=FALSE;
        }
        ~RestoreConsoleMode(){if(changed)SetConsoleMode(handle,mode);}
    } restore_console(console,original,inline_scroll);
    auto read_key=[&]() -> int {
        auto ready=WaitForSingleObject(console,50);
        if(ready==WAIT_TIMEOUT)return idle;
        if(ready!=WAIT_OBJECT_0)return end_input;
        INPUT_RECORD event{};
        DWORD count=0;
        if(!ReadConsoleInputW(console,&event,1,&count)||!count)return end_input;
        if(inline_scroll&&event.EventType==MOUSE_EVENT){
            const auto &mouse=event.Event.MouseEvent;
            if(mouse.dwEventFlags==MOUSE_WHEELED){
                short delta=static_cast<short>(HIWORD(mouse.dwButtonState));
                scroll_transcript(delta>0?-1:1);
            }else{
                bool pressed=(mouse.dwButtonState&FROM_LEFT_1ST_BUTTON_PRESSED)!=0;
                bool moved=(mouse.dwEventFlags&MOUSE_MOVED)!=0;
                scroll_transcript_mouse(mouse.dwMousePosition.Y+1,mouse.dwMousePosition.X+1,moved?32:0,!pressed);
            }
            return ignored;
        }
        if(event.EventType!=KEY_EVENT||!event.Event.KeyEvent.bKeyDown)return ignored;
        const auto &key=event.Event.KeyEvent;
        if(inline_scroll){
            if(key.wVirtualKeyCode==VK_UP){scroll_transcript(-1);return ignored;}
            if(key.wVirtualKeyCode==VK_DOWN){scroll_transcript(1);return ignored;}
            if(key.wVirtualKeyCode==VK_PRIOR){scroll_transcript_page(-1);return ignored;}
            if(key.wVirtualKeyCode==VK_NEXT){scroll_transcript_page(1);return ignored;}
        }
        if(key.wVirtualKeyCode==VK_UP||(inline_row&&key.wVirtualKeyCode==VK_LEFT))return up;
        if(key.wVirtualKeyCode==VK_DOWN||(inline_row&&key.wVirtualKeyCode==VK_RIGHT))return down;
        if(key.wVirtualKeyCode==VK_HOME)return home;
        if(key.wVirtualKeyCode==VK_END)return end;
        if(key.wVirtualKeyCode==VK_PRIOR)return page_up;
        if(key.wVirtualKeyCode==VK_NEXT)return page_down;
        if(key.wVirtualKeyCode=='D'&&(key.dwControlKeyState&(LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED)))return 4;
        return key.uChar.UnicodeChar;
    };
#else
    termios original{};
    if(tcgetattr(STDIN_FILENO,&original))return std::nullopt;
    termios mode=original;
    mode.c_lflag&=~static_cast<tcflag_t>(ICANON|ECHO);
    mode.c_cc[VMIN]=1;
    mode.c_cc[VTIME]=0;
    if(tcsetattr(STDIN_FILENO,TCSANOW,&mode))return std::nullopt;
    struct RestoreTerminal {
        termios mode;
        ~RestoreTerminal(){tcsetattr(STDIN_FILENO,TCSANOW,&mode);}
    } restore{original};
    auto read_key=[]() -> int {
        pollfd ready{STDIN_FILENO,POLLIN,0};
        int result=poll(&ready,1,50);
        if(result<0)return errno==EINTR?ignored:end_input;
        if(!result)return idle;
        unsigned char key=0;
        return read(STDIN_FILENO,&key,1)==1?static_cast<int>(key):end_input;
    };
#endif
    struct MenuCursor {
        bool hidden=env_value("DESK_KEEP_MENU_CURSOR")==nullptr;
        MenuCursor(){if(hidden)fputs("\033[?25l",stdout);}
        ~MenuCursor(){if(hidden){fputs("\033[?25h",stdout);fflush(stdout);}}
    } menu_cursor;
#ifndef _WIN32
    const bool native_selection=env_value("DESK_NATIVE_MOUSE_SELECTION")!=nullptr;
    struct InlineScrollModes {
        bool mouse=false;
        InlineScrollModes(bool scroll):mouse(scroll){
            if(!scroll)return;
            fputs(mouse?"\033[?1007h\033[?1000h\033[?1002h\033[?1006h":"\033[?1007h",stdout);
            fflush(stdout);
        }
        ~InlineScrollModes(){
            if(mouse){fputs("\033[?1006l\033[?1002l\033[?1000l",stdout);fflush(stdout);}
        }
    } scroll_modes(inline_scroll);
#endif
    size_t selected=initial>0&&static_cast<size_t>(initial)<=items.size()?static_cast<size_t>(initial-1):items.size()-1;
    int rows=terminal_rows(),columns=terminal_columns();
    auto capacity=[&](int height){return std::min(items.size(),static_cast<size_t>(std::max(1,height-top)));};
    size_t visible=capacity(rows);
    std::vector<std::string> labels=items;
    for(auto &label:labels)for(char &byte:label){
        if(static_cast<unsigned char>(byte)<32||byte==127)byte=' ';
    }
    size_t first=0;
    std::string typed;
    const bool color=terminal_supports_color();
    auto option_label=[&](size_t i,bool focused,int max_label=0){
        auto number=std::to_string((i+1)%items.size())+".";
        std::string text=max_label>0?ui_clip(labels[i],max_label):labels[i];
        if(!color)return number+" "+text;
        return ui_number(number)+" "+(focused?ui_paint(Tone::Accent,text,true):text);
    };
    if(inline_row)visible=1;
    auto draw=[&]{
        if(inline_row){
            // One row: "❯ Allow   Deny   Show full   Cancel", focused one in accent.
            // Narrow windows drop the padding of unfocused items and tighten
            // the gaps so the row never wraps onto the status line.
            int labels_cells=0;
            for(size_t i=0;i<items.size();i++)
                labels_cells+=static_cast<int>(std::to_string((i+1)%items.size()).size())+2+ui_width(labels[i]);
            const int gaps=static_cast<int>(items.size())-1;
            const int roomy_cells=labels_cells+2*static_cast<int>(items.size())+3*gaps;
            const bool compact=columns>0&&roomy_cells>columns;
            const char *gap=!compact?"   ":(labels_cells+2+2*gaps<=columns?"  ":" ");
            std::string row="\r\033[2K\033[?7l";
            for(size_t i=0;i<items.size();i++){
                const bool focused=i==selected;
                if(i)row+=gap;
                if(focused)row+="❯ ";
                else if(!compact)row+="  ";
                row+=option_label(i,focused);
            }
            row+="\033[?7h";
            fputs(row.c_str(),stdout);
            fflush(stdout);
            return;
        }
        if(selected<first)first=selected;
        if(selected>=first+visible)first=selected-visible+1;
        fputs("\033[?7l",stdout);
        for(size_t i=first;i<first+visible;i++){
            const bool focused=i==selected;
            // The focused row gets the accent colour and a bold label. Unselected
            // rows keep full brightness: dim text disappears on translucent
            // backgrounds, which is a common Konsole setup.
            const int max_label=columns>12?columns-8:0;
            auto label=option_label(i,focused,max_label);
            printf("\r\033[2K%s%s\n",focused?"❯ ":"  ",label.c_str());
        }
        fputs("\033[?7h",stdout);
        fflush(stdout);
    };
    draw();
    auto finish=[&](int choice){
        if(inline_row){fputs("\r\033[2K",stdout);fflush(stdout);}
        else if(transient){printf("\033[%zuA\r\033[J",visible);fflush(stdout);}
        return std::optional<int>{choice};
    };
    std::string escape;
    double escape_started=0;
    while(!interrupted){
        if(on_idle)on_idle();
        int height=terminal_rows(),width=terminal_columns();
        if(inline_row&&(height!=rows||width!=columns)){rows=height;columns=width;draw();}
        else if(height!=rows||width!=columns){
            if(on_resize){
                rows=height;columns=width;visible=capacity(rows);
                first=std::min(first,items.size()-visible);
                on_resize();
                draw();
            }else{
                size_t rewind=std::min(visible,capacity(height));
                printf("\033[%zuA\r\033[J",rewind);
                rows=height;columns=width;visible=capacity(rows);
                first=std::min(first,items.size()-visible);
                draw();
            }
        }
        int key=read_key();
        if(key==end_input){input_closed=true;break;}
        if(key==idle){
            if(!escape.empty()&&monotime()-escape_started>=0.2){
                if(escape=="\x1b"&&(transient||allow_escape))return finish(0);
                escape.clear();
            }
            continue;
        }
        if(key==ignored)continue;
        if(key=='\r'||key=='\n'){
            if(typed.empty())return finish(static_cast<int>((selected+1)%items.size()));
            auto value=strtoul(typed.c_str(),nullptr,10);
            if(value<items.size())return finish(static_cast<int>(value));
            typed.clear();
            continue;
        }
        if(key==4){
            if(inline_row||(transient&&!allow_escape)){input_closed=true;break;}
            if(allow_escape)return finish(0);
            continue;
        }
        if(key==27){escape="\x1b";escape_started=monotime();continue;}
        if(!escape.empty()){
            escape+=static_cast<char>(key);
            if(escape.size()==2&&(key=='['||key=='O'))continue;
            if(escape.size()==3&&escape[1]=='['&&key=='<')continue;
            if(escape.size()>2&&((key>='0'&&key<='9')||key==';')&&escape.size()<32)continue;
#ifndef _WIN32
            if(inline_scroll){
                if(escape.starts_with("\x1b[<")&&(key=='M'||key=='m')){
                    int button=0,col=0,row=0;
                    if(sscanf(escape.c_str()+3,"%d;%d;%d",&button,&col,&row)==3){
                        const int code=button&~28;
                        if(key=='M'&&code==64)scroll_transcript(-1);
                        else if(key=='M'&&code==65)scroll_transcript(1);
                        else scroll_transcript_mouse(row,col,code,key=='m');
                    }
                    escape.clear();
                    continue;
                }
                if(native_selection&&(escape=="\x1b[A"||escape=="\x1bOA")){scroll_transcript(-1);escape.clear();continue;}
                if(native_selection&&(escape=="\x1b[B"||escape=="\x1bOB")){scroll_transcript(1);escape.clear();continue;}
                if(escape=="\x1b[5~"||escape=="\x1b[5;2~"){scroll_transcript_page(-1);escape.clear();continue;}
                if(escape=="\x1b[6~"||escape=="\x1b[6;2~"){scroll_transcript_page(1);escape.clear();continue;}
                if(escape=="\x1b[1;5A"){scroll_transcript(-3);escape.clear();continue;}
                if(escape=="\x1b[1;5B"){scroll_transcript(3);escape.clear();continue;}
                if(escape=="\x1b[1;5H"){scroll_transcript(-1000000);escape.clear();continue;}
                if(escape=="\x1b[1;5F"){scroll_transcript(1000000);escape.clear();continue;}
            }
#endif
            if(escape=="\x1b[A"||escape=="\x1bOA")key=up;
            else if(escape=="\x1b[B"||escape=="\x1bOB")key=down;
            else if(inline_row&&(escape=="\x1b[D"||escape=="\x1bOD"))key=up;
            else if(inline_row&&(escape=="\x1b[C"||escape=="\x1bOC"))key=down;
            else if(escape=="\x1b[H"||escape=="\x1bOH"||escape=="\x1b[1~")key=home;
            else if(escape=="\x1b[F"||escape=="\x1bOF"||escape=="\x1b[4~")key=end;
            else if(escape=="\x1b[5~")key=page_up;
            else if(escape=="\x1b[6~")key=page_down;
            else key=ignored;
            escape.clear();
        }
        const size_t previous=selected;
        if(key==up||key==down){
            selected=(selected+(key==up?items.size()-1:1))%items.size();
            typed.clear();
        }else if(key==home||key==end){
            selected=key==home?0:items.size()-1;
            typed.clear();
        }else if(key==page_up||key==page_down){
            selected=key==page_up?(selected>visible?selected-visible:0):std::min(selected+visible,items.size()-1);
            typed.clear();
        }else if(key==127||key==8){
            if(!typed.empty())typed.pop_back();
        }else if(key>='0'&&key<='9'){
            if(typed.size()<20)typed.push_back(static_cast<char>(key));
        }
        if(!typed.empty()){
            auto value=strtoul(typed.c_str(),nullptr,10);
            if(value<items.size())selected=value?value-1:items.size()-1;
        }
        if(previous!=selected){if(!inline_row)printf("\033[%zuA",visible);draw();}
    }
    return finish(-1);
}
