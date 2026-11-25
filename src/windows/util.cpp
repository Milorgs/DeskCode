#include "win.hpp"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
namespace {
HANDLE main_thread=nullptr;
BOOL WINAPI control(DWORD event){
    if(event!=CTRL_C_EVENT&&event!=CTRL_BREAK_EVENT&&event!=CTRL_CLOSE_EVENT&&event!=CTRL_LOGOFF_EVENT&&event!=CTRL_SHUTDOWN_EVENT)return FALSE;
    interrupted=event==CTRL_C_EVENT||event==CTRL_BREAK_EVENT?SIGINT:SIGTERM;
    if(main_thread)CancelSynchronousIo(main_thread);return TRUE;
}
void file_errno(){DWORD code=GetLastError();errno=code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND?ENOENT:EACCES;}
}
void platform_initialize(){
    SetConsoleCP(CP_UTF8);SetConsoleOutputCP(CP_UTF8);
    _setmode(_fileno(stdout),_O_BINARY);_setmode(_fileno(stderr),_O_BINARY);
    if(!main_thread){DuplicateHandle(GetCurrentProcess(),GetCurrentThread(),GetCurrentProcess(),&main_thread,0,FALSE,DUPLICATE_SAME_ACCESS);SetConsoleCtrlHandler(control,TRUE);}
}
char *read_bytes(const char *path,size_t limit,size_t *length){
    auto target=fs_path(path);WinHandle file(CreateFileW(target.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,OPEN_EXISTING,FILE_FLAG_OPEN_REPARSE_POINT,nullptr));
    if(!file){file_errno();return nullptr;}
    BY_HANDLE_FILE_INFORMATION info{};LARGE_INTEGER size{};
    if(!GetFileInformationByHandle(file.get(),&info)||!GetFileSizeEx(file.get(),&size)||GetFileType(file.get())!=FILE_TYPE_DISK||(info.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))||size.QuadPart<0||static_cast<unsigned long long>(size.QuadPart)>limit){errno=EFBIG;return nullptr;}
    size_t cap=size.QuadPart>0?static_cast<size_t>(size.QuadPart):(std::min)(limit,static_cast<size_t>(4096));
    char *data=static_cast<char *>(malloc(cap+1));if(!data)return nullptr;size_t used=0;
    for(;;){
        if(used==cap){
            char byte=0;DWORD count=0;
            if(!ReadFile(file.get(),&byte,1,&count,nullptr)){free(data);file_errno();return nullptr;}
            if(!count)break;
            if(used==limit){free(data);errno=EFBIG;return nullptr;}
            size_t next_cap=(std::min)(limit,(std::max)(cap*2,cap+static_cast<size_t>(4096)));
            char *grown=static_cast<char *>(realloc(data,next_cap+1));
            if(!grown){free(data);return nullptr;}
            data=grown;cap=next_cap;data[used++]=byte;continue;
        }
        DWORD count=0;
        if(!ReadFile(file.get(),data+used,static_cast<DWORD>((std::min)(cap-used,static_cast<size_t>(65536))),&count,nullptr)){free(data);file_errno();return nullptr;}
        if(!count)break;
        used+=count;
    }
    data[used]=0;if(length)*length=used;return data;
}
int mkdirs(const char *path,unsigned int mode){
    fs::path target=fs_path(path),current;PrivateAccess access;
    if(mode==0700&&!access.descriptor)return -1;
    for(const auto &part:target){
        current/=part;if(current==current.root_path())continue;
        DWORD attributes=GetFileAttributesW(current.c_str());
        if(attributes==INVALID_FILE_ATTRIBUTES){
            if(!CreateDirectoryW(current.c_str(),mode==0700?&access.attributes:nullptr)&&GetLastError()!=ERROR_ALREADY_EXISTS){file_errno();return -1;}
            attributes=GetFileAttributesW(current.c_str());
        }
        if(attributes==INVALID_FILE_ATTRIBUTES||!(attributes&FILE_ATTRIBUTE_DIRECTORY)||(attributes&FILE_ATTRIBUTE_REPARSE_POINT)){errno=EACCES;return -1;}
    }
    if(mode==0700&&!access.apply(target)){file_errno();return -1;}
    return 0;
}
int atomic_file(const char *path,const void *data,size_t length,unsigned int mode){
    fs::path target=fs_path(path);char id[33];id_string(id);fs::path temporary=target;temporary+=L".tmp-"+wide(id);
    PrivateAccess access;if(mode==0600&&!access.descriptor)return -1;
    WinHandle file(CreateFileW(temporary.c_str(),GENERIC_WRITE,0,mode==0600?&access.attributes:nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr));
    if(!file){file_errno();return -1;}
    bool ok=win_write(file.get(),std::string_view(static_cast<const char *>(data),length))&&FlushFileBuffers(file.get());file.reset();
    DWORD attributes=GetFileAttributesW(target.c_str());
    if(ok&&attributes!=INVALID_FILE_ATTRIBUTES){
        if(attributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))ok=false;
        else if(mode==0600&&!access.apply(target))ok=false;
        else ok=ReplaceFileW(target.c_str(),temporary.c_str(),nullptr,0,nullptr,nullptr)!=FALSE;
    }else if(ok)ok=MoveFileExW(temporary.c_str(),target.c_str(),MOVEFILE_WRITE_THROUGH)!=FALSE;
    if(!ok){file_errno();DeleteFileW(temporary.c_str());return -1;}return 0;
}
char *input(const char *prompt,const char *fallback,bool secret){
    if(input_closed)return nullptr;
    fputs(prompt,stdout);fflush(stdout);HANDLE handle=GetStdHandle(STD_INPUT_HANDLE);DWORD mode=0;
    std::string line;bool console=GetConsoleMode(handle,&mode)!=FALSE,ok=true;
    if(console){
        if(secret&&!SetConsoleMode(handle,mode&~ENABLE_ECHO_INPUT))return nullptr;
        std::wstring text;wchar_t buffer[1024];DWORD count=0;
        while(!interrupted){
            if(!ReadConsoleW(handle,buffer,1024,&count,nullptr)||!count){ok=false;break;}
            text.append(buffer,count);if(text.size()>65536){ok=false;break;}if(text.find(L'\n')!=std::wstring::npos)break;
        }
        if(secret){SetConsoleMode(handle,mode);putchar('\n');}
        if(!text.empty()&&(text.front()==4||text.front()==26))ok=false;
        if(ok&&!interrupted)line=utf8(text);
    }else{
        bool pipe=GetFileType(handle)==FILE_TYPE_PIPE;
        while(!interrupted){
            if(pipe){DWORD available=0;if(!PeekNamedPipe(handle,nullptr,0,nullptr,&available,nullptr)){ok=false;break;}if(!available){Sleep(20);continue;}}
            char byte;DWORD count=0;if(!ReadFile(handle,&byte,1,&count,nullptr)||!count){ok=false;break;}if(byte=='\n')break;line+=byte;if(line.size()>65536){ok=false;break;}
        }
    }
    if(!ok&&!interrupted)input_closed=true;
    if(!ok||interrupted||line.size()>65536)return nullptr;
    while(!line.empty()&&(line.back()=='\r'||line.back()=='\n'))line.pop_back();
    return _strdup(line.empty()&&fallback?fallback:line.c_str());
}

void check_exit(const App &app){(void)app;}


// Read console events so chat menus and task updates do not wait for Enter.
namespace {
bool activity_visible=false;
std::string last_activity;
}
void terminal_message(const std::string &text){
    if(text.empty())return;
    activity_visible=false;
    if(transcript_append(text))return;
    fwrite(text.data(),1,text.size(),stdout);
    fflush(stdout);
}
void terminal_activity(const std::string &text){
    if(transcript_set_activity(text))return;
    if(!terminal_supports_ansi()){activity_visible=false;return;}
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
    HANDLE console=GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode=0;
    if(secret||!GetConsoleMode(console,&mode)||!terminal_supports_ansi())return input(prompt,fallback,secret);
    if(input_closed)return nullptr;
    struct RestoreConsoleMode {
        HANDLE handle;DWORD saved;bool changed=false;
        RestoreConsoleMode(HANDLE h,DWORD m,bool mouse):handle(h),saved(m){
            if(mouse)changed=SetConsoleMode(handle,(m|ENABLE_MOUSE_INPUT|ENABLE_EXTENDED_FLAGS)&~static_cast<DWORD>(ENABLE_QUICK_EDIT_MODE))!=FALSE;
        }
        ~RestoreConsoleMode(){scroll_transcript_mouse(0,0,0,true);if(changed)SetConsoleMode(handle,saved);activity_visible=false;}
    } restore_mode(console,mode,draft&&transcript_scrollable());

    std::wstring text=draft?wide(draft->text):std::wstring{};
    size_t point=draft?wide(draft->text.substr(0,std::min(draft->text.size(),static_cast<size_t>(std::max(0,draft->cursor))))).size():text.size();
    wchar_t pending_surrogate=0;
    if(draft){draft->paused=false;draft->complete_file=false;draft->paste_image=false;}
    bool recalling=false;
    size_t recall_position=0;
    std::wstring recall_stash;
    auto recall_set=[&](std::string_view utf8_text){
        text=wide(utf8_text);
        point=text.size();
    };
    auto previous=[&](size_t offset){
        if(offset)offset--;
        if(offset&&text[offset]>=0xdc00&&text[offset]<=0xdfff)offset--;
        return offset;
    };
    auto next=[&](size_t offset){
        if(offset<text.size()){
            if(text[offset]>=0xd800&&text[offset]<=0xdbff)offset++;
            offset++;
        }
        return std::min(offset,text.size());
    };
    auto cell_width=[](std::wstring_view value){
        int cells=0;
        for(size_t i=0;i<value.size();i++){
            wchar_t ch=value[i];
            if(ch>=0xd800&&ch<=0xdbff&&i+1<value.size()){cells+=2;i++;continue;}
            WORD type=0;
            GetStringTypeW(CT_CTYPE3,&ch,1,&type);
            if(type&(C3_NONSPACING|C3_DIACRITIC))continue;
            cells+=(type&C3_FULLWIDTH)?2:1;
        }
        return cells;
    };
    auto redraw=[&]{
        if(transcript_scrollable()&&terminal_rows()>=8)printf("\033[%d;1H",terminal_rows()-2);
        const int available=std::max(1,terminal_columns()-cell_width(wide(prompt))-1);
        size_t first=point;
        int cells=0;
        while(first){
            size_t begin=previous(first);
            int width=cell_width(std::wstring_view(text).substr(begin,first-begin));
            if(cells+width>=available)break;
            cells+=width;first=begin;
        }
        size_t last=point;
        while(last<text.size()){
            size_t end=next(last);
            int width=cell_width(std::wstring_view(text).substr(last,end-last));
            if(cells+width>available)break;
            cells+=width;last=end;
        }
        printf("\r\033[2K\033[?7l%s",prompt);
        auto before=utf8(std::wstring_view(text).substr(first,point-first));
        auto after=utf8(std::wstring_view(text).substr(point,last-point));
        fputs(before.c_str(),stdout);
        fputs("\0337",stdout);
        fputs(after.c_str(),stdout);
        fputs("\0338\033[?7h",stdout);
        fflush(stdout);
    };
    int prev_rows=terminal_rows(),prev_cols=terminal_columns();
    double slash_started=0;
    redraw();
    while(!interrupted){
        if(idle)idle();
        if(draft&&slash_started>0){
            if(text.size()==1&&text[0]==L'/'){
                if(monotime()-slash_started>=0.25){
                    draft->text.clear();draft->cursor=0;
                    fputs("\r\033[2K",stdout);
                    return _strdup("/");
                }
            }else slash_started=0;
        }
        if(draft&&draft->approval_pending&&draft->approval_pending()){
            scroll_transcript_bottom();
            draft->text=utf8(text);
            draft->cursor=static_cast<int>(utf8(std::wstring_view(text).substr(0,point)).size());
            draft->paused=true;
            fputs("\r\033[2K",stdout);
            fflush(stdout);
            return nullptr;
        }
        int cur_rows=terminal_rows(),cur_cols=terminal_columns();
        if(cur_rows!=prev_rows||cur_cols!=prev_cols){
            prev_rows=cur_rows;prev_cols=cur_cols;
            if(idle)idle();
            redraw();
        }
        if(WaitForSingleObject(console,50)!=WAIT_OBJECT_0)continue;
        INPUT_RECORD record{};
        DWORD count=0;
        if(!ReadConsoleInputW(console,&record,1,&count)||!count)break;
        if(record.EventType==WINDOW_BUFFER_SIZE_EVENT){if(idle)idle();redraw();continue;}
        if(record.EventType==MOUSE_EVENT&&transcript_scrollable()){
            const auto &mouse=record.Event.MouseEvent;
            if(mouse.dwEventFlags==MOUSE_WHEELED){
                short delta=static_cast<short>(HIWORD(mouse.dwButtonState));
                scroll_transcript(delta>0?-1:1);
            }else{
                bool pressed=(mouse.dwButtonState&FROM_LEFT_1ST_BUTTON_PRESSED)!=0;
                bool moved=(mouse.dwEventFlags&MOUSE_MOVED)!=0;
                scroll_transcript_mouse(mouse.dwMousePosition.Y+1,mouse.dwMousePosition.X+1,moved?32:0,!pressed);
            }
            redraw();
            continue;
        }
        if(record.EventType!=KEY_EVENT||!record.Event.KeyEvent.bKeyDown)continue;
        const auto &key=record.Event.KeyEvent;
        bool ctrl=(key.dwControlKeyState&(LEFT_CTRL_PRESSED|RIGHT_CTRL_PRESSED))!=0;
        bool alt=(key.dwControlKeyState&(LEFT_ALT_PRESSED|RIGHT_ALT_PRESSED))!=0;
        bool shift=(key.dwControlKeyState&SHIFT_PRESSED)!=0;
        if(ctrl&&key.wVirtualKeyCode=='D'){
            if(draft){draft->text.clear();draft->cursor=0;}
            input_closed=true;break;
        }
        if(draft&&(key.wVirtualKeyCode==VK_F2||key.wVirtualKeyCode==VK_F3)){
            draft->text=utf8(text);
            draft->cursor=static_cast<int>(utf8(std::wstring_view(text).substr(0,point)).size());
            fputs("\r\033[2K",stdout);
            return _strdup(key.wVirtualKeyCode==VK_F2?"/model":"/reasoning");
        }
        if(draft&&alt&&key.wVirtualKeyCode=='V'){
            draft->text=utf8(text);
            draft->cursor=static_cast<int>(utf8(std::wstring_view(text).substr(0,point)).size());
            draft->paste_image=true;
            fputs("\r\033[2K",stdout);
            return _strdup("/paste");
        }
        if(draft&&key.wVirtualKeyCode==VK_TAB){
            std::string current_utf8=utf8(text);
            size_t cursor_utf8=utf8(std::wstring_view(text).substr(0,point)).size();
            size_t at_pos=0;std::string query;
            if(find_at_mention(current_utf8,cursor_utf8,at_pos,query)){
                draft->text=std::move(current_utf8);
                draft->cursor=static_cast<int>(cursor_utf8);
                draft->complete_file=true;
                fputs("\r\033[2K",stdout);
                return _strdup("@");
            }
            continue;
        }
        // Prompt history recall (Ctrl+P/N, Alt+Up/Down, or Up/Down without viewport).
        if(draft&&draft->history&&!draft->history->empty()){
            bool want_prev=(ctrl&&key.wVirtualKeyCode=='P')||(alt&&key.wVirtualKeyCode==VK_UP)||(!transcript_scrollable()&&!ctrl&&!alt&&key.wVirtualKeyCode==VK_UP);
            bool want_next=(ctrl&&key.wVirtualKeyCode=='N')||(alt&&key.wVirtualKeyCode==VK_DOWN)||(!transcript_scrollable()&&!ctrl&&!alt&&key.wVirtualKeyCode==VK_DOWN);
            if(want_prev){
                if(!recalling){recall_stash=text;recall_position=draft->history->size();recalling=true;}
                if(recall_position>0)recall_set((*draft->history)[--recall_position]);
                redraw();continue;
            }
            if(want_next){
                if(recalling){
                    if(recall_position+1<draft->history->size())recall_set((*draft->history)[++recall_position]);
                    else{recalling=false;text=recall_stash;point=text.size();recall_stash.clear();}
                    redraw();
                }
                continue;
            }
        }
        // Viewport keyboard scrolling on Windows.
        if(transcript_scrollable()){
            if(key.wVirtualKeyCode==VK_PRIOR){scroll_transcript_page(-1);redraw();continue;}
            if(key.wVirtualKeyCode==VK_NEXT){scroll_transcript_page(1);redraw();continue;}
            if(ctrl&&key.wVirtualKeyCode==VK_HOME){scroll_transcript(-1000000);redraw();continue;}
            if(ctrl&&key.wVirtualKeyCode==VK_END){scroll_transcript_bottom();redraw();continue;}
            if(!alt&&(key.wVirtualKeyCode==VK_UP||key.wVirtualKeyCode==VK_DOWN)){
                scroll_transcript(key.wVirtualKeyCode==VK_UP?-1:1);
                redraw();continue;
            }
        }
        if(key.wVirtualKeyCode==VK_RETURN){
            if(draft&&(alt||shift)&&text.size()<65535){
                draft->newline_inserted=true;
                text.insert(point,1,L'\u21b5');
                point++;
                redraw();
                continue;
            }
            // Detect multi-line paste burst in the console input queue.
            DWORD queued=0;
            if(draft&&GetNumberOfConsoleInputEvents(console,&queued)&&queued>0){
                std::wstring pasted=L"\n";
                INPUT_RECORD peek{};DWORD peeked=0;
                while(PeekConsoleInputW(console,&peek,1,&peeked)&&peeked>0){
                    if(peek.EventType!=KEY_EVENT){ReadConsoleInputW(console,&peek,1,&peeked);continue;}
                    if(!peek.Event.KeyEvent.bKeyDown){ReadConsoleInputW(console,&peek,1,&peeked);continue;}
                    wchar_t ch=peek.Event.KeyEvent.uChar.UnicodeChar;
                    if(peek.Event.KeyEvent.wVirtualKeyCode==VK_RETURN){
                        ReadConsoleInputW(console,&peek,1,&peeked);
                        pasted+=L'\n';
                    }else if(ch>=32||ch==L'\t'){
                        ReadConsoleInputW(console,&peek,1,&peeked);
                        pasted+=ch;
                    }else break;
                    if(pasted.size()>65536)break;
                }
                while(!pasted.empty()&&pasted.back()==L'\n')pasted.pop_back();
                if(!pasted.empty()){
                    std::wstring full_chunk=std::wstring(text.substr(0,point))+pasted;
                    text.erase(0,point);point=0;
                    std::string chunk_utf8=utf8(full_chunk);
                    size_t lines=1;for(char c:chunk_utf8)if(c=='\n')lines++;
                    char label[96];
                    snprintf(label,sizeof label,tr("[粘贴 #%d · %zu 行]"),draft->next_paste++,lines);
                    draft->pastes.push_back({label,std::move(chunk_utf8)});
                    std::wstring wlabel=wide(label);
                    text.insert(0,wlabel);
                    point=wlabel.size();
                    redraw();
                    continue;
                }
            }
            putchar('\n');
            auto line=utf8(text);
            if(draft){
                draft->text.clear();draft->cursor=0;
                expand_draft_input(line,*draft);
            }
            if(line.size()>65536){puts(tr("输入超过 64 KB，请缩短后重试。"));return _strdup("");}
            return _strdup(line.empty()&&fallback?fallback:line.c_str());
        }
        if(key.wVirtualKeyCode==VK_LEFT)point=previous(point);
        else if(key.wVirtualKeyCode==VK_RIGHT)point=next(point);
        else if(key.wVirtualKeyCode==VK_HOME)point=0;
        else if(key.wVirtualKeyCode==VK_END)point=text.size();
        else if(key.wVirtualKeyCode==VK_BACK&&point){size_t begin=previous(point);text.erase(begin,point-begin);point=begin;}
        else if(key.wVirtualKeyCode==VK_DELETE&&point<text.size())text.erase(point,next(point)-point);
        else if(!ctrl&&key.uChar.UnicodeChar>=32&&text.size()<65536){
            const bool started_slash=draft&&text.empty()&&key.uChar.UnicodeChar=='/';
            wchar_t value=key.uChar.UnicodeChar;
            if(value>=0xd800&&value<=0xdbff){pending_surrogate=value;continue;}
            std::wstring insertion;
            if(value>=0xdc00&&value<=0xdfff){
                if(pending_surrogate)insertion+=pending_surrogate;
                else value=0xfffd;
            }else if(pending_surrogate)insertion+=static_cast<wchar_t>(0xfffd);
            pending_surrogate=0;
            insertion+=value;
            for(WORD i=0;i<key.wRepeatCount&&text.size()+insertion.size()<=65536;i++){
                text.insert(point,insertion);
                point+=insertion.size();
            }
            if(started_slash&&text.size()==1)slash_started=monotime();
            else slash_started=0;
        }
        if(text.size()!=1||text[0]!=L'/')slash_started=0;
        redraw();
    }
    putchar('\n');
    return nullptr;
}
