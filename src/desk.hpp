#ifndef DESK_H
#define DESK_H
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif
#ifdef __clang__
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wunused-function"
#endif
#include <json-c/json.h>
#ifdef __clang__
#pragma clang diagnostic pop
#endif
#include <stdbool.h>
#include <stddef.h>
#include <signal.h>
#include <limits.h>
#include <sys/types.h>
#include <cstdint>
#include "platform.hpp"
#include "http.hpp"
#include <memory>
#include <atomic>
#include <string>
#include <string_view>
#include <vector>
#include <functional>
#include <optional>
#define VERSION "1.14.3"
#define CONTEXT_LIMIT 128000
// Large enough for real source files (roughly 6000 lines); the model context
// trims older content separately, so the limit only guards memory per file.
#define FILE_LIMIT 262144
#define EDIT_FILE_LIMIT (4*1024*1024)
#define SLICED_FILE_LIMIT (16*1024*1024)
#define COMMAND_OUTPUT_LIMIT (8*1024*1024)
#define RESPONSE_LIMIT 32000000
using J = json_object;
struct JsonDeleter {
    void operator()(J *value) const noexcept { json_object_put(value); }
};
using JsonPtr = std::unique_ptr<J,JsonDeleter>;
struct App {
    J *store=nullptr, *config=nullptr, *messages=nullptr;
    char root[PATH_MAX]{}, config_dir[PATH_MAX]{}, error[512]{};
    bool yes=false, auto_edit=false, shell=true, desktop=false, record=false, verbose=false, progress_visible=false;
    bool progress_animated=false;
    bool conversation_worker=false, guided_approval=false;
    std::string conversation_id{};
    size_t session_revision=0;
    size_t config_revision=0;
    std::string progress_text{};
    double progress_updated=0;
    bool thinking=false;
    double thinking_started=0;
    int max_steps=0;
    std::intptr_t task_events=-1;
    const char *state="等待任务";
    void *desktop_data=nullptr;
    void *mcp_data=nullptr;
    // Receives assistant text as it streams in; empty leaves the whole reply to
    // be printed once the response completes.
    std::function<void(const std::string &)> stream_text;
    // Tokens reported by the provider during the current task, so the live
    // status line and the closing summary can show them.
    std::uint64_t task_tokens=0;
    bool progress_line=false;
    CurlHandle api_http{};
    // Headless runs (stdout redirected or --json): the final reply goes to
    // reply_out (or the JSON result) and is not echoed to the human stream.
    FILE *reply_out=nullptr;
    bool json_output=false;
    // When non-null, run_task appends one {name,target,ok} entry per tool call.
    J *trace=nullptr;
    // Set when an approval was needed but no interactive terminal could answer.
    bool approval_unavailable=false;
    // True inside delegate_research so only read-only tools are exposed.
    bool subagent=false;
    bool sandbox=true;
    bool workspace_hooks_approved=false;
    std::string turn_id{};
    std::vector<std::string> allowed_commands{};
};
extern std::atomic<int> interrupted;
extern std::atomic<bool> input_closed;
void progress(App *app,const char *text);
void check_exit(const App &app);
void progress_clear(App *app);
void progress_tick(App *app);
std::string elapsed_label(double seconds);
std::string token_label(std::uint64_t tokens);
const char *tr(const char *text);
const char *language_code();
bool set_language(const char *code);
bool language_init(int argc,char **argv);
bool save_language(App &app,const char *code);
J *obj(void);
J *arr(void);
J *str(const char *s);
J *copy(J *j);
J *get(J *j,const char *key);
const char *sget(J *j,const char *key);
size_t alen(J *j);
J *at(J *j,size_t index);
int iget(J *j,const char *key,int fallback);
bool bget(J *j,const char *key);
bool tool_failed(J *result);
inline std::uint64_t add_tokens(std::uint64_t total,std::uint64_t amount){
    return amount>UINT64_MAX-total?UINT64_MAX:total+amount;
}
J *load_tokens(const App &app);
std::uint64_t model_tokens(J *usage,J *config);
bool record_tokens(App &app,J *config,std::uint64_t amount);
int reorder_models(App *app,const std::vector<std::string> &order);
bool valid_utf8(std::string_view text) noexcept;
std::string display_utf8(std::string_view text,size_t &used,bool final=false);
void put(J *j,const char *key,J *value);
void add(J *j,J *value);
J *err(const char *message);
void seterr(App *a,const char *message);
char *read_bytes(const char *path,size_t limit,size_t *length);
int path_suffix(char out[PATH_MAX],const char *base,const char *suffix);
int mkdirs(const char *path,unsigned int mode);
int atomic_file(const char *path,const void *data,size_t len,unsigned int mode);
// inline_row lays the choices out on one line (left/right to move) so a prompt
// can sit in a single input row, such as the approval answer under its panel.
std::optional<int> terminal_choice(const std::vector<std::string> &items,int initial,int top,
                                   const std::function<void()> &idle={},bool transient=false,bool inline_row=false,
                                   bool allow_escape=false,const std::function<void()> &on_resize={});
std::vector<std::string> approval_choices(bool preview,bool allow_session_rule=false);
char *input(const char *prompt,const char *fallback,bool secret);
void terminal_message(const std::string &text);
void terminal_activity(const std::string &text);
struct PastedChunk {
    std::string marker;
    std::string content;
};
struct ChatDraft {
    std::string text;
    int cursor=0;
    bool paused=false;
    bool complete_file=false;
    bool paste_image=false;
    bool newline_inserted=false;
    int next_paste=1;
    std::vector<PastedChunk> pastes;
    std::function<bool()> approval_pending;
    // Earlier requests of this conversation, oldest first. Ctrl+P/Ctrl+N and
    // Alt+Up/Alt+Down recall them; Up/Down keep scrolling the transcript.
    const std::vector<std::string> *history=nullptr;
};
char *editable_input(const char *prompt,const char *fallback,bool secret,const std::function<void()> &idle,ChatDraft *draft=nullptr);
// first_row is where the transcript starts filling; 0 disables the composer.
// keep_transcript records messages while the composer is off (a window too
// small for it) so they reappear once the frame returns.
void set_composer_layout(int input_row,int transcript_row,int first_row=1,bool keep_transcript=false);
void reset_transcript();
void scroll_transcript(int rows);
void scroll_transcript_page(int direction);
bool scroll_transcript_mouse(int row,int col,int button=0,bool release=false);
void scroll_transcript_bottom();
bool transcript_scrollable();
size_t transcript_hidden_lines();
void expand_draft_input(std::string &text,ChatDraft &draft);
bool find_at_mention(std::string_view text,size_t cursor,size_t &at_pos,std::string &query);
std::vector<std::string> workspace_complete_files(const App &app,std::string_view query,size_t limit=10);
std::string capture_clipboard_image(App &app);
J *prepare_user_message(App *app,const char *task);
std::string user_message_text(J *message);
std::string load_project_guide(const App &app);
J *tool_code_outline(App *app,J *args);
J *tool_delegate_research(App *app,J *args);
void record_file_change(App *app,const char *rel_path,const char *backup_path,bool existed_before,unsigned int mode_before);
bool undo_last_turn(App &app,std::string &summary);
void prune_workspace_cache(const App &app);
bool glob_match(std::string_view pattern,std::string_view text,bool case_insensitive=false);
struct GitIgnore {
    std::vector<std::string> rules;
    void load(const char *root);
    bool ignored(std::string_view rel_path,bool is_dir) const;
};
bool safe_readonly_command(std::string_view command);
bool command_allowed_in_session(const App &app,std::string_view command);
void remember_allowed_command(App &app,std::string_view command);
J *tool_web_fetch(App *app,J *args);
J *tool_web_search(App *app,J *args);
J *tool_desktop_screenshot(App *app,J *args);
bool compact_conversation(App &app,std::string &summary);
bool sandbox_supported(void);
bool run_pre_shell_hook(App &app,const char *command,std::string &blocked_reason);
void run_post_edit_hook(App &app,const char *rel_path,J *result);
struct CustomCommand {
    std::string name,scope,summary;
};
std::vector<CustomCommand> list_custom_commands(const App &app);
bool expand_custom_command(const App &app,std::string_view input,std::string &expanded,std::string &label);
enum class Operation { Other, FileEdit, Shell };
bool approval_required(const App &app,Operation operation=Operation::Other);
const char *permission_name(const App &app);
const char *reasoning_name(const char *effort);
int approve(App *a,const char *description,Operation operation=Operation::Other,const char *title=nullptr);
J *history_list(const App &app);
J *history_read(const App &app,const std::string &id);
J *history_context(const App &app,const std::string &id);
bool history_save(const App &app,const std::string &id,const std::string &title,J *config,bool deleted=false,const char *approval_mode=nullptr);
void history_checkpoint(App &app,size_t first);
void id_string(char out[33]);
double monotime(void);
int store_load(App *a);
bool valid_reasoning_effort(J *config,J *value);
int config_valid(App *a,J *config);
J *config_default(const char *name);
int save_config(App *a,J *config,bool clear_session=true);
bool same_model(J *left,J *right);
int activate(App *a,const char *name);
int config_name_valid(const char *name);
int api_check(App *a,J *config);
J *api_models(App *a,J *config);
J *parse_tool_arguments(const char *text);
J *api_complete(App *a,J *config,J *messages,J *tools);
// Filled with streamed assistant text as it arrives; empty means wait for the
// complete response.
J *native_payload(J *config,J *messages,J *tools,bool prompt_cache=false);
J *native_response(App *a,J *data);
J *api_request(App *a,J *config,const char *endpoint,J *payload,void *stream=nullptr);
J *schemas(App *a);
J *tool_call(App *a,const char *name,J *arguments);
J *run_command(App *a,const char *command,int timeout_ms);
int resolve_file(App *a,const char *path,char out[PATH_MAX]);
char *sha256(const void *bytes,size_t len);
J *desktop_snapshot(App *a,const char *app,const char *filter=nullptr);
J *desktop_act(App *a,const char *ref,const char *action,const char *text);
void desktop_clear(App *a);
void desktop_destroy(App *a);
bool desktop_compiled(void);
void reset_session(App *a);
int run_task(App *a,const char *task);
J *context(App *a,J *tools,bool *compacted=nullptr);
void save_record(App *a);
void app_cleanup(App *a);
#endif
