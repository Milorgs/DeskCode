#ifndef DESK_TASKS_HPP
#define DESK_TASKS_HPP
#include "desk.hpp"
class Tasks {
    struct Impl;
    std::unique_ptr<Impl> impl;
public:
    std::function<void()> on_idle;
    explicit Tasks(std::string executable="/proc/self/exe");
    ~Tasks();
    int start(App &app,const std::string &task,const std::string &workspace="");
    void refresh();
    J *snapshot();
    bool cancel(int id);
    bool remove(int id);
    bool resume(int id,const std::string &task,J *settings=nullptr);
    bool respond(int id,int answer);
    // dropped receives how many bytes were cut from the front, so callers
    // can keep an absolute position across truncation.
    std::string output(int id,bool *truncated=nullptr,std::uint64_t *dropped=nullptr);
    bool decide(int id,bool allow);
    char *readline(const char *prompt="\n› ");
};
class ResourceLock {
    std::intptr_t fd=-1;
public:
    ResourceLock(App &app,bool desktop);
    explicit ResourceLock(const char *config_dir);
    ~ResourceLock();
    ResourceLock(const ResourceLock &)=delete;
    ResourceLock &operator=(const ResourceLock &)=delete;
    bool acquired() const { return fd>=0; }
};
void task_event(App &app,char state);
// Report the running task's token count to the controller.
void task_tokens(App &app);
// Report how much of the context budget the latest request used (0..100).
void task_context(App &app,int percent);
void task_detail(App &app,std::string_view detail);
int task_worker(std::intptr_t fd);
J *conversation_settings(const App &app,bool reset,J *replace_messages=nullptr);
void apply_conversation_settings(App &app,J *settings);
#endif
