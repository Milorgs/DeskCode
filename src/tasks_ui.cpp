#include "tasks.hpp"
#include <algorithm>
#include <cstring>

std::string usage_event(const App &app){
    std::string message(USAGE_EVENT_SIZE,'\0');
    message[0]='u';
    char *cursor=message.data()+1;
    for(std::uint64_t value:{app.turn_usage.input,app.turn_usage.output,app.turn_usage.cache_read,
                             app.session_usage.input,app.session_usage.output,app.session_usage.cache_read}){
        memcpy(cursor,&value,sizeof value);
        cursor+=sizeof value;
    }
    for(double value:{app.turn_usage.cost,app.session_usage.cost}){
        memcpy(cursor,&value,sizeof value);
        cursor+=sizeof value;
    }
    *cursor=static_cast<char>((app.turn_usage.priced?1:0)|(app.session_usage.priced?2:0));
    return message;
}
void parse_usage_event(const char *bytes,TokenUsage &turn,TokenUsage &session){
    const char *cursor=bytes+1;
    for(std::uint64_t *value:{&turn.input,&turn.output,&turn.cache_read,&session.input,&session.output,&session.cache_read}){
        memcpy(value,cursor,sizeof *value);
        cursor+=sizeof *value;
    }
    for(double *value:{&turn.cost,&session.cost}){
        memcpy(value,cursor,sizeof *value);
        cursor+=sizeof *value;
    }
    turn.priced=*cursor&1;
    session.priced=(*cursor&2)!=0;
}
void put_usage(J *state,const TokenUsage &turn,const TokenUsage &session){
    auto number=[](std::uint64_t value){return json_object_new_int64(static_cast<int64_t>(std::min<std::uint64_t>(value,INT64_MAX)));};
    put(state,"turn_input",number(turn.input));
    put(state,"turn_output",number(turn.output));
    put(state,"turn_cache_read",number(turn.cache_read));
    put(state,"turn_cost",json_object_new_double(turn.cost));
    put(state,"turn_priced",json_object_new_boolean(turn.priced));
    put(state,"session_input",number(session.input));
    put(state,"session_output",number(session.output));
    put(state,"session_cache_read",number(session.cache_read));
    put(state,"session_cost",json_object_new_double(session.cost));
    put(state,"session_priced",json_object_new_boolean(session.priced));
}

// Conversation workers retain their context, while explicit model changes reset it.
J *conversation_settings(const App &app,bool reset,J *replace_messages){
    J *settings=obj();
    put(settings,"config",copy(app.config));
    put(settings,"reset",json_object_new_boolean(reset));
    if(replace_messages)put(settings,"messages",copy(replace_messages));
    put(settings,"yes",json_object_new_boolean(app.yes));
    put(settings,"auto_edit",json_object_new_boolean(app.auto_edit));
    put(settings,"shell",json_object_new_boolean(app.shell));
    put(settings,"sandbox",json_object_new_boolean(app.sandbox));
    put(settings,"desktop",json_object_new_boolean(app.desktop));
    put(settings,"record",json_object_new_boolean(app.record));
    put(settings,"verbose",json_object_new_boolean(app.verbose));
    put(settings,"max_steps",json_object_new_int(app.max_steps));
    put(settings,"plan_mode",json_object_new_boolean(app.plan_mode));
    return settings;
}
void apply_conversation_settings(App &app,J *settings){
    if(!settings)return;
    bool reset=bget(settings,"reset")||!same_model(app.config,get(settings,"config"));
    desktop_clear(&app);
    json_object_put(app.config);app.config=copy(get(settings,"config"));
    app.yes=bget(settings,"yes");app.auto_edit=bget(settings,"auto_edit");
    app.shell=bget(settings,"shell");app.desktop=bget(settings,"desktop");
    if(get(settings,"sandbox"))app.sandbox=bget(settings,"sandbox");
    app.record=bget(settings,"record");app.verbose=bget(settings,"verbose");
    app.max_steps=iget(settings,"max_steps",0);
    app.plan_mode=bget(settings,"plan_mode");
    if(reset)reset_session(&app);
    if(J *replaced=get(settings,"messages");json_object_is_type(replaced,json_type_array)){
        json_object_put(app.messages);
        app.messages=copy(replaced);
    }
}
