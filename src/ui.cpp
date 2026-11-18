#include "ui.hpp"
#include "platform.hpp"
#include <cstring>

bool ui_color(){return terminal_supports_color();}

namespace {
const char *sgr(Tone tone){
    switch(tone){
    case Tone::Dim:return "\033[38;5;247m"; // grey, not SGR 2: faint text vanishes on translucent backgrounds
    case Tone::Bold:return "\033[1m";
    case Tone::Accent:return "\033[38;5;111m";
    case Tone::Success:return "\033[38;5;114m";
    case Tone::Warning:return "\033[38;5;179m";
    case Tone::Orange:return "\033[38;5;208m";
    case Tone::Error:return "\033[38;5;174m";
    case Tone::Rule:return "\033[38;5;240m";
    case Tone::Code:return "\033[38;5;216m";
    case Tone::Plain:break;
    }
    return "";
}
// Length of a CSI escape (ESC [ ... final) at the front of text, else 0.
size_t escape_length(std::string_view text){
    if(text.size()<2||text[0]!='\033'||text[1]!='[')return 0;
    size_t i=2;
    while(i<text.size()&&!(text[i]>='@'&&text[i]<='~'))i++;
    return i<text.size()?i+1:text.size();
}
// Decode one UTF-8 code point; returns the byte length, 0 on a bad sequence.
size_t decode(std::string_view text,uint32_t &code){
    if(text.empty())return 0;
    auto first=static_cast<unsigned char>(text[0]);
    size_t width=first<0x80?1:first<0xe0?2:first<0xf0?3:first<0xf4?4:0;
    if(!width||width>text.size())return 0;
    // A lead byte carries 8-width payload bits: 7 for ASCII, 5 for a 3-byte lead.
    code=first&((1u<<(8-width))-1);
    for(size_t i=1;i<width;i++){
        auto byte=static_cast<unsigned char>(text[i]);
        if((byte&0xc0)!=0x80)return 0;
        code=(code<<6)|(byte&0x3f);
    }
    return width;
}
// East Asian wide and fullwidth ranges take two cells; combining marks take none.
// Emoji with default emoji presentation (Unicode East Asian Width "W") are
// listed too: terminals draw ✅ ❌ ✨ 🚀 ⭐ two cells wide.
int cell_width(uint32_t code){
    if(code<32||code==127)return 0;
    if((code>=0x0300&&code<=0x036f)||(code>=0x200b&&code<=0x200f)||(code>=0xfe00&&code<=0xfe0f))return 0;
    if((code>=0x1100&&code<=0x115f)||(code>=0x2e80&&code<=0xa4cf)||
       (code>=0xac00&&code<=0xd7a3)||(code>=0xf900&&code<=0xfaff)||
       (code>=0xfe30&&code<=0xfe6f)||(code>=0xff00&&code<=0xff60)||
       (code>=0xffe0&&code<=0xffe6)||(code>=0x1f300&&code<=0x1f64f)||
       (code>=0x1f900&&code<=0x1f9ff)||(code>=0x20000&&code<=0x3fffd))return 2;
    static constexpr uint32_t emoji[][2]={
        {0x231a,0x231b},{0x23e9,0x23ec},{0x23f0,0x23f0},{0x23f3,0x23f3},{0x25fd,0x25fe},
        {0x2614,0x2615},{0x2648,0x2653},{0x267f,0x267f},{0x2693,0x2693},{0x26a1,0x26a1},
        {0x26aa,0x26ab},{0x26bd,0x26be},{0x26c4,0x26c5},{0x26ce,0x26ce},{0x26d4,0x26d4},
        {0x26ea,0x26ea},{0x26f2,0x26f3},{0x26f5,0x26f5},{0x26fa,0x26fa},{0x26fd,0x26fd},
        {0x2705,0x2705},{0x270a,0x270b},{0x2728,0x2728},{0x274c,0x274c},{0x274e,0x274e},
        {0x2753,0x2755},{0x2757,0x2757},{0x2795,0x2797},{0x27b0,0x27b0},{0x27bf,0x27bf},
        {0x2b1b,0x2b1c},{0x2b50,0x2b50},{0x2b55,0x2b55},{0x1f004,0x1f004},{0x1f0cf,0x1f0cf},
        {0x1f18e,0x1f18e},{0x1f191,0x1f19a},{0x1f200,0x1f2ff},{0x1f680,0x1f6ff},
        {0x1f7e0,0x1f7eb},{0x1fa70,0x1faff}};
    for(const auto &range:emoji)if(code>=range[0]&&code<=range[1])return 2;
    return 1;
}
}

std::string ui_paint(Tone tone,std::string_view text,bool bold){
    const char *code=sgr(tone);
    if(!ui_color())return std::string(text);
    std::string result;
    if(bold)result+="\033[1m";
    result+=code;
    result.append(text);
    result+="\033[0m";
    return result;
}

const char *ui_sgr(Tone tone){return ui_color()?sgr(tone):"";}

std::string ui_number(std::string_view text,Tone normal){
    if(!ui_color()||!env_value("DESK_CONTRAST_NUMBERS"))return ui_paint(normal,text);
    const auto start=text.find_first_not_of(' ');
    if(start==text.npos)return std::string(text);
    return std::string(text.substr(0,start))+"\033[1;38;2;184;220;255m"
        +std::string(text.substr(start))+"\033[0m";
}

int ui_width(std::string_view text){
    int cells=0;
    while(!text.empty()){
        if(size_t skip=escape_length(text)){text.remove_prefix(skip);continue;}
        uint32_t code=0;
        size_t size=decode(text,code);
        if(!size){text.remove_prefix(1);cells++;continue;}
        cells+=cell_width(code);
        text.remove_prefix(size);
    }
    return cells;
}

std::string ui_clip(std::string_view text,int width){
    if(width<=0)return std::string(text);
    if(ui_width(text)<=width)return std::string(text);
    std::string result;
    int cells=0;
    bool styled=false;
    while(!text.empty()){
        if(size_t skip=escape_length(text)){
            result.append(text.substr(0,skip));
            styled=true;
            text.remove_prefix(skip);
            continue;
        }
        uint32_t code=0;
        size_t size=decode(text,code);
        if(!size)break;
        int advance=cell_width(code);
        if(cells+advance>width-1)break;
        result.append(text.substr(0,size));
        cells+=advance;
        text.remove_prefix(size);
    }
    result+="…";
    if(styled)result+="\033[0m";
    return result;
}

std::string ui_fit(std::string_view text,int margin){
    const int columns=terminal_columns();
    if(columns<=0)return std::string(text);
    return ui_clip(text,std::max(24,columns-margin));
}

std::vector<std::string> ui_wrap(std::string_view text,int width){
    std::vector<std::string> lines;
    if(width<=0){lines.emplace_back(text);return lines;}
    std::string current;
    // SGR sequences seen so far on this logical line; re-opened after a wrap so
    // a coloured diff line keeps its colour, and closed before the break.
    std::string active;
    int cells=0;
    // Spaces stay verbatim so indented bodies survive ui_box untouched; only
    // spaces that would open a wrapped line are dropped.
    bool wrapped=false;
    auto flush=[&]{
        if(!active.empty())current+="\033[0m";
        lines.push_back(current);
        current=active;
        cells=0;
        wrapped=true;
    };
    size_t index=0;
    while(index<text.size()){
        if(size_t skip=escape_length(text.substr(index))){
            auto sequence=text.substr(index,skip);
            current.append(sequence);
            if(sequence=="\033[0m"||sequence=="\033[m")active.clear();
            else if(sequence.back()=='m')active.append(sequence);
            index+=skip;
            continue;
        }
        // Take one code point at a time so wrapping never splits a character.
        uint32_t code=0;
        size_t size=decode(text.substr(index),code);
        if(!size)size=1;
        if(code==' '||code=='\t'){
            if(wrapped&&cells==0){index+=size;continue;}
            if(cells+1>width){flush();index+=size;continue;}
            current+=' ';
            cells++;
            index+=size;
            continue;
        }
        int advance=cell_width(code);
        if(cells+advance>width)flush();
        current.append(text.substr(index,size));
        cells+=advance;
        index+=size;
    }
    if(!current.empty()||lines.empty()){
        if(!active.empty())current+="\033[0m";
        lines.push_back(current);
    }
    return lines;
}

std::string ui_box(std::string_view title,std::string_view body,int width){
    if(width<10)return std::string(body);
    const int inner=width-4;
    std::string result;
    if(title.empty()){
        std::string top="╭";
        while(ui_width(top)<width-1)top+="─";
        top+="╮";
        result+=ui_paint(Tone::Rule,top);
    }else{
        std::string clipped=ui_clip(title,inner-3);
        int used=3+ui_width(clipped)+1;
        std::string tail;
        while(used+ui_width(tail)<width-1)tail+="─";
        tail+="╮";
        result+=ui_paint(Tone::Rule,"╭─ ");
        result+=ui_paint(Tone::Warning,clipped,true);
        result+=ui_paint(Tone::Rule," "+tail);
    }
    result+='\n';
    const std::string left_bar=ui_paint(Tone::Rule,"│")+" ";
    const std::string right_bar=" "+ui_paint(Tone::Rule,"│");
    std::string subject(body);
    size_t start=0;
    while(true){
        auto end=subject.find('\n',start);
        auto line=std::string_view(subject).substr(start,end==std::string::npos?std::string::npos:end-start);
        for(const auto &piece:ui_wrap(line,inner)){
            std::string row=left_bar;
            row+=piece;
            int pad=inner-ui_width(piece);
            if(pad>0)row.append(static_cast<size_t>(pad),' ');
            row+=right_bar;
            result+=row;
            result+='\n';
        }
        if(end==std::string::npos)break;
        start=end+1;
    }
    std::string bottom="╰";
    while(ui_width(bottom)<width-1)bottom+="─";
    bottom+="╯";
    result+=ui_paint(Tone::Rule,bottom);
    return result;
}

const char *ui_bullet(){return "⏺";}
const char *ui_assistant_marker(bool color){return color?"\033[97m●\033[0m ":"● ";}
const char *ui_result(){return "⎿";}
const char *ui_prompt(){return "❯";}

namespace {
// Braille spinner keeps the same visual weight at any terminal font.
const char *const spinner[]={"⠋","⠙","⠹","⠸","⠼","⠴","⠦","⠧","⠇","⠏"};
}
const char *ui_spinner(size_t frame){return spinner[frame%ui_spinner_frames()];}
size_t ui_spinner_frames(){return sizeof spinner/sizeof spinner[0];}

// ---------------------------------------------------------------------------
// Streaming Markdown renderer
// ---------------------------------------------------------------------------
namespace {
std::string paint_if(bool color,Tone tone,std::string_view text,bool bold=false){
    if(!color)return std::string(text);
    std::string result;
    if(bold)result+="\033[1m";
    result+=sgr(tone);
    result.append(text);
    result+="\033[0m";
    return result;
}
bool fence_start(std::string_view text){return text.starts_with("```")||text.starts_with("~~~");}
size_t fence_run(std::string_view text,char fence){
    size_t n=0;
    while(n<text.size()&&text[n]==fence)n++;
    return n;
}
bool blank(std::string_view text){
    for(char c:text)if(c!=' '&&c!='\t'&&c!='\r')return false;
    return true;
}
bool fence_prefix(std::string_view text){
    return text.size()<3&&(std::string_view("```").starts_with(text)||std::string_view("~~~").starts_with(text));
}
}

MarkdownStream::MarkdownStream(bool color,int indent):color_(color),indent_(indent<0?0:indent){}

void MarkdownStream::indent(){
    if(!first_line_)out_.append(static_cast<size_t>(indent_),' ');
}

void MarkdownStream::restyle(){
    if(!color_)return;
    out_+="\033[0m";
    if(kind_==Kind::Heading){out_+="\033[1m";out_+=sgr(Tone::Accent);}
    else if(kind_==Kind::Quote)out_+=sgr(Tone::Dim);
    if(bold_)out_+="\033[1m";
    if(code_span_)out_+=sgr(Tone::Code);
}

void MarkdownStream::inline_char(char c){
    // Inside `code` everything is literal, including ** and 2**10.
    if(code_span_&&c!='`'){out_+=c;return;}
    if(c=='*'){
        if(star_held_){star_held_=false;bold_=!bold_;restyle();}
        else star_held_=true;
        return;
    }
    if(star_held_){star_held_=false;out_+='*';}
    if(c=='`'){code_span_=!code_span_;restyle();return;}
    out_+=c;
}

void MarkdownStream::inline_text(std::string_view text){
    for(char c:text)inline_char(c);
}

void MarkdownStream::end_line(bool newline){
    if(star_held_){star_held_=false;out_+='*';}
    const bool styled=bold_||code_span_||kind_==Kind::Heading||kind_==Kind::Quote||kind_==Kind::Code;
    if(color_&&styled)out_+="\033[0m";
    bold_=false;code_span_=false;
    kind_=Kind::Plain;
    pending_.clear();
    if(newline){
        out_+='\n';
        first_line_=false;
    }
    line_start_=true;
}

// Decide what kind of line pending_ starts. Returns false while more input is
// needed; at a newline (or finish) it always decides.
bool MarkdownStream::classify(bool at_newline){
    std::string_view line(pending_);
    size_t lead=0;
    while(lead<line.size()&&line[lead]==' ')lead++;
    std::string_view body=line.substr(lead);
    if(in_code_){
        // Close only on the same fence character, at least as long as the
        // opening fence and with nothing after it; a ```python line inside a
        // ````markdown block is content.
        const size_t run=fence_char_?fence_run(body,fence_char_):0;
        if(lead<=3&&run>0&&!at_newline&&(run<fence_len_?run==body.size():blank(body.substr(run))))return false;
        if(lead<=3&&fence_len_&&run>=fence_len_&&blank(body.substr(run))){
            indent();
            out_+=paint_if(color_,Tone::Rule,"└─");
            in_code_=false;
            fence_char_=0;fence_len_=0;
            kind_=Kind::Plain;
            pending_.clear();
            return true;
        }
        kind_=Kind::Code;
        indent();
        out_+=paint_if(color_,Tone::Rule,"│ ");
        if(color_)out_+=sgr(Tone::Code);
        out_+=pending_;
        pending_.clear();
        return true;
    }
    if(body.empty()){
        if(!at_newline&&lead<=3)return false;
        if(at_newline){pending_.clear();return true;}
    }
    if(lead<=3&&!body.empty()){
        const char first=body[0];
        if(first=='`'||first=='~'){
            if(!at_newline&&fence_prefix(body))return false;
            if(fence_start(body)){
                if(!at_newline)return false;
                fence_char_=first;
                fence_len_=fence_run(body,first);
                std::string language(body.substr(fence_len_));
                while(!language.empty()&&(language.back()==' '||language.back()=='\t'))language.pop_back();
                while(!language.empty()&&language.front()==' ')language.erase(0,1);
                indent();
                out_+=paint_if(color_,Tone::Rule,"┌─");
                if(!language.empty())out_+=" "+paint_if(color_,Tone::Dim,language);
                in_code_=true;
                kind_=Kind::Plain;
                pending_.clear();
                return true;
            }
        }else if(first=='#'){
            size_t hashes=0;
            while(hashes<body.size()&&body[hashes]=='#')hashes++;
            if(hashes==body.size()&&hashes<=6&&!at_newline)return false;
            if(hashes<=6&&hashes<body.size()&&body[hashes]==' '){
                kind_=Kind::Heading;
                indent();
                restyle();
                std::string rest(body.substr(hashes+1));
                pending_.clear();
                inline_text(rest);
                return true;
            }
        }else if(first=='-'||first=='*'||first=='+'){
            if(body.size()==1&&!at_newline)return false;
            if(body.size()>=2&&body[1]==' '){
                kind_=Kind::Bullet;
                indent();
                out_.append(lead,' ');
                out_+=paint_if(color_,Tone::Accent,"•");
                out_+=' ';
                std::string rest(body.substr(2));
                pending_.clear();
                inline_text(rest);
                return true;
            }
            if(first!='+'&&body.find_first_not_of(first)==std::string_view::npos){
                if(!at_newline)return false;
                if(body.size()>=3){
                    indent();
                    out_+=paint_if(color_,Tone::Rule,"────────────────────────");
                    pending_.clear();
                    return true;
                }
            }
        }else if(first=='>'){
            kind_=Kind::Quote;
            indent();
            out_.append(lead,' ');
            out_+=paint_if(color_,Tone::Rule,"▎");
            out_+=' ';
            restyle();
            std::string rest(body.substr(1));
            if(!rest.empty()&&rest.front()==' ')rest.erase(0,1);
            pending_.clear();
            inline_text(rest);
            return true;
        }
    }
    kind_=Kind::Plain;
    indent();
    std::string text;
    text.swap(pending_);
    inline_text(text);
    return true;
}

std::string MarkdownStream::feed(std::string_view text){
    for(char c:text){
        const auto byte=static_cast<unsigned char>(c);
        if(c=='\r')continue;
        if((byte<32&&c!='\n'&&c!='\t')||byte==127)continue;
        if(!color_){
            if(line_start_&&c!='\n'){indent();line_start_=false;}
            out_+=c;
            if(c=='\n'){line_start_=true;first_line_=false;}
            continue;
        }
        if(line_start_){
            if(c=='\n'){classify(true);end_line(true);continue;}
            pending_+=c;
            if(classify(false))line_start_=false;
            continue;
        }
        if(c=='\n'){end_line(true);continue;}
        if(kind_==Kind::Code)out_+=c;
        else inline_char(c);
    }
    std::string chunk;
    chunk.swap(out_);
    return chunk;
}

std::string MarkdownStream::finish(){
    if(color_){
        if(line_start_&&!pending_.empty())classify(true);
        if(!line_start_||star_held_||bold_||code_span_)end_line(false);
        if(in_code_){in_code_=false;}
    }
    std::string chunk;
    chunk.swap(out_);
    return chunk;
}

std::string ui_markdown(std::string_view text,int indent){
    MarkdownStream stream(ui_color(),indent);
    std::string result=stream.feed(text);
    result+=stream.finish();
    return result;
}
