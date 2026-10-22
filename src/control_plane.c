#define _POSIX_C_SOURCE 200809L
#include "control_plane.h"
#include "agentos_runtime.h"
#include <json-c/json.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <time.h>

static const char *status(pm_result_t r){
    switch(r){case PM_OK:return "OK";case PM_PENDING:return "PENDING";
      case PM_NOT_FOUND:return "NOT_FOUND";case PM_INVALID_ARGUMENT:return "INVALID_ARGUMENT";
      case PM_DUPLICATE:return "DUPLICATE";case PM_DISABLED:return "IDENTITY_DISABLED";
      case PM_FULL:return "TABLE_FULL";case PM_ALREADY_TERMINATED:return "ALREADY_TERMINATED";
      case PM_INVALID_TRANSITION:return "INVALID_TRANSITION";
      case PM_NOT_TERMINATED:return "NOT_TERMINATED";case PM_PID_EXHAUSTED:return "PID_EXHAUSTED";}
    return "UNKNOWN";
}
static const char *str(json_object *o,const char *key){
    json_object *v=NULL;
    if(!json_object_object_get_ex(o,key,&v)||!json_object_is_type(v,json_type_string))return NULL;
    return json_object_get_string(v);
}
static int pid_arg(json_object *root,agentos_pid_t *out){
    json_object *v=NULL;
    if(!json_object_object_get_ex(root,"pid",&v)||!json_object_is_type(v,json_type_int))return 0;
    int64_t n=json_object_get_int64(v);if(n<=0)return 0;
    *out=(agentos_pid_t)n;return 1;
}
static json_object *new_response(pm_result_t r){
    json_object *o=json_object_new_object();
    json_object_object_add(o,"version",json_object_new_int(1));
    json_object_object_add(o,"status",json_object_new_string(r==PM_OK?"ok":"error"));
    json_object_object_add(o,"code",json_object_new_string(status(r)));
    return o;
}
int control_handle(const char *line,char *reply,size_t capacity){
    if(!line||!reply||capacity==0)return -1;
    /* Unlike wire v1, administrative frames must encode non-ASCII strings
     * with JSON Unicode escapes. Python's json.dumps default does this. */
    struct json_tokener *tok=json_tokener_new();if(!tok)return -1;
    json_tokener_set_flags(tok,JSON_TOKENER_STRICT);
    size_t len=strlen(line);
    json_object *root=json_tokener_parse_ex(tok,line,(int)len);
    int valid=json_tokener_get_error(tok)==json_tokener_success &&
      json_tokener_get_parse_end(tok)==len && json_object_is_type(root,json_type_object);
    json_object *reply_obj=NULL;
    if(!valid){reply_obj=new_response(PM_INVALID_ARGUMENT);goto done;}
    json_object *ver=NULL;
    if(!json_object_object_get_ex(root,"version",&ver)||
       !json_object_is_type(ver,json_type_int)||json_object_get_int(ver)!=1){
        reply_obj=new_response(PM_INVALID_ARGUMENT);goto done;
    }
    json_object *expire=NULL;
    if(json_object_object_get_ex(root,"expires_monotonic_ms",&expire)){
        struct timespec ts={0};
        if(!json_object_is_type(expire,json_type_int) ||
           clock_gettime(CLOCK_MONOTONIC,&ts)!=0){
            reply_obj=new_response(PM_INVALID_ARGUMENT);goto done;
        }
        int64_t now=(int64_t)ts.tv_sec*1000+ts.tv_nsec/1000000;
        if(json_object_get_int64(expire)<now){
            reply_obj=new_response(PM_PENDING);
            json_object_object_add(reply_obj,"code",json_object_new_string("EXPIRED_NO_ACTION"));
            goto done;
        }
    }
    const char *op=str(root,"operation");
    agentos_pm_t *m=agentos_runtime_pm();
    if(!op||!m){reply_obj=new_response(PM_INVALID_ARGUMENT);goto done;}
    if(strcmp(op,"list")==0){
        reply_obj=new_response(PM_OK);
        json_object_object_add(reply_obj,"runtime_id",json_object_new_int64((int64_t)agentos_runtime_id()));
        json_object_object_add(reply_obj,"processes",agentos_runtime_roster());
        char names[AGENTOS_PM_DEFAULT_DISABLED_CAPACITY][AGENTOS_PM_AGENT_ID_MAX+1];
        size_t n=pm_list_disabled(m,names,AGENTOS_PM_DEFAULT_DISABLED_CAPACITY);
        json_object *disabled=json_object_new_array();
        for(size_t i=0;i<n&&i<AGENTOS_PM_DEFAULT_DISABLED_CAPACITY;i++)
          json_object_array_add(disabled,json_object_new_string(names[i]));
        json_object_object_add(reply_obj,"disabled",disabled);
        goto done;
    }
    if(strcmp(op,"events")==0){
        unsigned long long since=0;
        json_object *v=NULL;
        if(json_object_object_get_ex(root,"since",&v)){
            if(!json_object_is_type(v,json_type_int)||json_object_get_int64(v)<0){
                reply_obj=new_response(PM_INVALID_ARGUMENT);goto done;
            }
            since=(unsigned long long)json_object_get_int64(v);
        }
        reply_obj=new_response(PM_OK);
        json_object_object_add(reply_obj,"runtime_id",json_object_new_int64((int64_t)agentos_runtime_id()));
        json_object_object_add(reply_obj,"events",agentos_runtime_events(since));
        goto done;
    }
    const char *id=str(root,"agent_id");const char *role=str(root,"role");
    pm_snapshot_t snapshot={0};
    pm_result_t r=PM_INVALID_ARGUMENT;
    if(strcmp(op,"add")==0)r=pm_add(m,id,role,&snapshot);
    else if(strcmp(op,"remove")==0)r=pm_disable(m,id);
    else if(strcmp(op,"reactivate")==0)r=pm_reactivate(m,id,role,&snapshot);
    else if(strcmp(op,"inspect")==0)r=pm_find_by_id(m,id,&snapshot);
    else if(strcmp(op,"terminate")==0||strcmp(op,"reap")==0||strcmp(op,"wait")==0){
        agentos_pid_t pid;
        if(pid_arg(root,&pid)){
            if(strcmp(op,"terminate")==0)r=pm_terminate(m,pid,PM_EXIT_KILLED);
            else if(strcmp(op,"reap")==0)r=pm_reap(m,pid,&snapshot);
            else r=pm_wait(m,pid,&snapshot);
        }
    }
    reply_obj=new_response(r);
    if(r==PM_OK && (strcmp(op,"add")==0||strcmp(op,"reactivate")==0||
        strcmp(op,"inspect")==0||strcmp(op,"reap")==0||strcmp(op,"wait")==0))
        json_object_object_add(reply_obj,"process",agentos_runtime_snapshot_json(&snapshot));
done:
    if(!reply_obj){json_object_put(root);json_tokener_free(tok);return -1;}
    const char *encoded=json_object_to_json_string_ext(reply_obj,JSON_C_TO_STRING_PLAIN);
    int n=encoded?snprintf(reply,capacity,"%s\n",encoded):-1;
    int ok=n>=0&&(size_t)n<capacity?0:-1;
    json_object_put(reply_obj);json_object_put(root);json_tokener_free(tok);
    return ok;
}
