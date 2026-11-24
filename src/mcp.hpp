#ifndef DESK_MCP_HPP
#define DESK_MCP_HPP
#include "desk.hpp"
struct McpPromptInfo {
    std::string server,name,description;
};
struct McpServerSummary {
    std::string name;
    size_t tools=0,resources=0,prompts=0;
    bool remote=false,oauth=false,has_token=false;
};
J *mcp_load(App &app);
bool mcp_save(App &app,J *servers);
bool mcp_import(App &app,const char *path);
bool mcp_import_json(App &app,const std::string &text);
bool mcp_prepare(App &app,bool explicit_test=false,const char *only=nullptr);
J *mcp_tools(App &app);
void mcp_append_tools(App &app,J *tools,bool readonly_only=false);
bool mcp_readonly_tool(const char *name);
J *mcp_call(App &app,const char *name,J *arguments);
std::vector<McpPromptInfo> mcp_list_prompts(App &app);
bool mcp_expand_prompt(App &app,std::string_view input,std::string &expanded,std::string &label);
std::vector<McpServerSummary> mcp_summaries(App &app);
int mcp_cli(App &app,int argc,char **argv,int first);
void mcp_close(App &app);
#endif
