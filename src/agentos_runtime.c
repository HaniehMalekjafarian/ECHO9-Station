#define _POSIX_C_SOURCE 200809L
#include "agentos_runtime.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define RING_SIZE 512
static agentos_pm_t *manager;
static json_object *history[RING_SIZE];
static size_t head, count;
static unsigned long long runtime_id;


static const char *state_name(pm_state_t s){
    switch(s){case PM_STATE_NEW:return "NEW";case PM_STATE_READY:return "READY";
      case PM_STATE_RUNNING:return "RUNNING";case PM_STATE_WAITING:return "WAITING";
      case PM_STATE_TERMINATED:return "TERMINATED";}return "UNKNOWN";
}
static const char *wait_name(pm_wait_reason_t w){
    switch(w){case PM_WAIT_NONE:return "NONE";case PM_WAIT_AWAITING_WORK:return "AWAITING_WORK";
      case PM_WAIT_INFERENCE:return "INFERENCE";case PM_WAIT_CONTROLLED_IO:return "CONTROLLED_IO";}
    return "UNKNOWN";
}
static const char *exit_name(pm_exit_status_t x){
    switch(x){case PM_EXIT_NONE:return "NONE";case PM_EXIT_OK:return "EXIT_OK";
      case PM_EXIT_FAILED:return "EXIT_FAILED";case PM_EXIT_KILLED:return "EXIT_KILLED";
      case PM_EXIT_ABORTED:return "EXIT_ABORTED";}return "UNKNOWN";
}
json_object *agentos_runtime_snapshot_json(const pm_snapshot_t *p){
    json_object *o=json_object_new_object(); if(!o||!p)return o;
    json_object_object_add(o,"pid",json_object_new_int64((int64_t)p->pid));
    json_object_object_add(o,"agent_id",json_object_new_string(p->agent_id));
    json_object_object_add(o,"role",json_object_new_string(p->role));
    json_object_object_add(o,"state",json_object_new_string(state_name(p->state)));
    json_object_object_add(o,"wait_reason",json_object_new_string(wait_name(p->wait_reason)));
    json_object_object_add(o,"exit_status",json_object_new_string(exit_name(p->exit_status)));
    json_object_object_add(o,"created_seq",json_object_new_int64((int64_t)p->created_seq));
    json_object_object_add(o,"last_transition_seq",json_object_new_int64((int64_t)p->last_transition_seq));
    json_object_object_add(o,"generation",json_object_new_int64((int64_t)p->generation));
    return o;
}
static const char *event_name(pm_event_type_t t){
    switch(t){case PM_EVENT_CREATED:return "AGENT_CREATED";
      case PM_EVENT_STATE_CHANGED:return "AGENT_STATE_CHANGED";
      case PM_EVENT_TERMINATED:return "AGENT_TERMINATED";
      case PM_EVENT_REAPED:return "AGENT_REAPED";
      case PM_EVENT_ID_DISABLED:return "AGENT_ID_DISABLED";
      case PM_EVENT_ID_REACTIVATED:return "AGENT_ID_REACTIVATED";}
    return "UNKNOWN";
}
static void on_event(const pm_event_t *e,void *user){
    (void)user;
    json_object *o=json_object_new_object();if(!o)return;
    /* seq owned by C manager, monotonic even across tombstone events. */

    json_object_object_add(o,"schema_version",json_object_new_int(1));
    json_object_object_add(o,"runtime_id",json_object_new_int64((int64_t)runtime_id));
    json_object_object_add(o,"seq",json_object_new_int64((int64_t)e->seq));
    json_object_object_add(o,"source",json_object_new_string("controlled"));
    json_object_object_add(o,"event",json_object_new_string(event_name(e->type)));
    json_object_object_add(o,"agent_id",json_object_new_string(e->agent_id));
    if(e->has_process){
        json_object_object_add(o,"process",agentos_runtime_snapshot_json(&e->process));
        json_object_object_add(o,"previous_state",json_object_new_string(state_name(e->previous_state)));
    }
    if(count==RING_SIZE){json_object_put(history[head]);}
    else count++;
    history[head]=o;head=(head+1)%RING_SIZE;
}
int agentos_runtime_init(void){
    if(manager)return 0;
    struct timespec t={0};(void)clock_gettime(CLOCK_MONOTONIC,&t);
    runtime_id=((unsigned long long)getpid()<<32)^((unsigned long long)t.tv_sec*1000000ull+(unsigned long long)t.tv_nsec/1000);
    manager=pm_create(NULL,on_event,NULL);
    return manager?0:-1;
}
void agentos_runtime_destroy(void){
    pm_destroy(manager);manager=NULL;
    for(size_t i=0;i<count;i++)json_object_put(history[(head+RING_SIZE-count+i)%RING_SIZE]);
    count=0;head=0;
}
agentos_pm_t *agentos_runtime_pm(void){return manager;}
unsigned long long agentos_runtime_id(void){return runtime_id;}
void agentos_runtime_request_begin(const char *id,agentos_pid_t pid){
    (void)id;
    pm_snapshot_t p;
    if(pm_find_by_pid(manager,pid,&p)!=PM_OK)return;
    if(p.state==PM_STATE_NEW || p.state==PM_STATE_WAITING)
        (void)pm_transition(manager,pid,PM_STATE_READY,PM_WAIT_NONE);
    (void)pm_transition(manager,pid,PM_STATE_RUNNING,PM_WAIT_NONE);
}
void agentos_runtime_inference_begin(agentos_pid_t pid){
    (void)pm_transition(manager,pid,PM_STATE_WAITING,PM_WAIT_INFERENCE);
}
void agentos_runtime_inference_end(agentos_pid_t pid){
    (void)pm_transition(manager,pid,PM_STATE_READY,PM_WAIT_NONE);
    (void)pm_transition(manager,pid,PM_STATE_RUNNING,PM_WAIT_NONE);
}
void agentos_runtime_request_end(agentos_pid_t pid){
    (void)pm_transition(manager,pid,PM_STATE_WAITING,PM_WAIT_AWAITING_WORK);
}
json_object *agentos_runtime_roster(void){
    json_object *list=json_object_new_array();if(!list||!manager)return list;
    size_t n=pm_list(manager,NULL,0);
    if(n>AGENTOS_PM_DEFAULT_CAPACITY)n=AGENTOS_PM_DEFAULT_CAPACITY;
    pm_snapshot_t p[AGENTOS_PM_DEFAULT_CAPACITY];
    size_t count_now=pm_list(manager,p,n);
    if(count_now<n)n=count_now;
    for(size_t i=0;i<n;i++)json_object_array_add(list,agentos_runtime_snapshot_json(&p[i]));
    return list;
}
json_object *agentos_runtime_events(unsigned long long since){
    json_object *a=json_object_new_array(); if(!a)return NULL;
    for(size_t i=0;i<count;i++){
        json_object *o=history[(head+RING_SIZE-count+i)%RING_SIZE];
        json_object *s=NULL;
        if(json_object_object_get_ex(o,"seq",&s) &&
           (unsigned long long)json_object_get_int64(s)>since)
            json_object_array_add(a,json_object_get(o));
    }
    return a;
}
