#include "tasks.hpp"

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
    if(reset)reset_session(&app);
    if(J *replaced=get(settings,"messages");json_object_is_type(replaced,json_type_array)){
        json_object_put(app.messages);
        app.messages=copy(replaced);
    }
}
