#ifndef DESK_UI_HPP
#define DESK_UI_HPP
#include <string>
#include <string_view>
#include <vector>
// Shared visual language for every page and chat renderer.
// Colour is dropped automatically when the terminal cannot show it.
enum class Tone { Plain, Dim, Bold, Accent, Success, Warning, Orange, Error, Rule, Code };
bool ui_color();
std::string ui_paint(Tone tone,std::string_view text,bool bold=false);
// Raw SGR for a tone, or "" when colour is off.
const char *ui_sgr(Tone tone);
std::string ui_number(std::string_view text,Tone normal=Tone::Dim);
// Display width in terminal cells; UTF-8 aware without depending on the locale.
// SGR colour sequences count as zero width.
int ui_width(std::string_view text);
std::string ui_clip(std::string_view text,int width);
// Clip to the terminal width minus a margin; leaves the text untouched when the
// width is unknown (pipes, some test harnesses), so nothing is lost there.
std::string ui_fit(std::string_view text,int margin);
std::vector<std::string> ui_wrap(std::string_view text,int width);
std::string ui_box(std::string_view title,std::string_view body,int width);
// Streaming Markdown renderer for assistant replies. Text can arrive in any
// chunking; headings, lists, quotes, **bold**, `code` and fenced blocks are
// styled as soon as they can be recognised. Without colour the text passes
// through unchanged apart from the hanging indent. Terminal control bytes in
// model output are always dropped so a reply cannot drive the terminal.
class MarkdownStream {
public:
    MarkdownStream(bool color,int indent);
    std::string feed(std::string_view text);
    std::string finish();
private:
    enum class Kind { Plain, Heading, Bullet, Quote, Code };
    bool color_;
    int indent_;
    bool first_line_=true,line_start_=true,in_code_=false,bold_=false,code_span_=false;
    bool star_held_=false;
    Kind kind_=Kind::Plain;
    std::string pending_,out_;
    void indent();
    bool classify(bool at_newline);
    void inline_text(std::string_view text);
    void inline_char(char c);
    void restyle();
    void end_line(bool newline);
};
std::string ui_markdown(std::string_view text,int indent);
// Glyphs that give the interface its shape.
const char *ui_bullet();
const char *ui_assistant_marker(bool color);
const char *ui_result();
const char *ui_prompt();
const char *ui_spinner(size_t frame);
size_t ui_spinner_frames();
#endif
