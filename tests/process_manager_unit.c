#include "process_manager.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

static size_t events;
static uint64_t last_sequence;
static void on_event(const pm_event_t *e,void *ctx){
    (void)ctx;assert(e->seq>last_sequence);last_sequence=e->seq;events++;
}
#define OK(call) assert((call)==PM_OK)
int main(void){
    pm_config_t cfg={.capacity=3,.disabled_capacity=2};
    agentos_pm_t *m=pm_create(&cfg,on_event,NULL);assert(m);
    pm_snapshot_t a={0},b={0},c={0};bool created=false;
    OK(pm_auto_register(m,"medical-agent",&a,&created));assert(created&&a.pid==1);
    assert(a.state==PM_STATE_NEW && a.created_seq==1);
    OK(pm_auto_register(m,"medical-agent",&b,&created));assert(!created&&a.pid==b.pid&&events==1);
    OK(pm_add(m,"navigation-agent","NAV",&b));assert(b.pid==2);
    OK(pm_add(m,"third-agent",NULL,&c));assert(c.pid==3);
    assert(pm_add(m,"fourth-agent",NULL,NULL)==PM_FULL);
    assert(pm_add(m,"medical-agent",NULL,NULL)==PM_DUPLICATE);
    assert(pm_add(m,"",NULL,NULL)==PM_INVALID_ARGUMENT);
    assert(pm_add(m,"A\xFF",NULL,NULL)==PM_INVALID_ARGUMENT);
    size_t before_events=events;
    assert(pm_transition(m,a.pid,PM_STATE_WAITING,PM_WAIT_INFERENCE)==PM_INVALID_TRANSITION);
    assert(pm_transition(m,a.pid,PM_STATE_NEW,PM_WAIT_NONE)==PM_INVALID_TRANSITION);
    assert(events==before_events);
    OK(pm_transition(m,a.pid,PM_STATE_READY,PM_WAIT_NONE));
    OK(pm_transition(m,a.pid,PM_STATE_RUNNING,PM_WAIT_NONE));
    OK(pm_transition(m,a.pid,PM_STATE_WAITING,PM_WAIT_INFERENCE));
    OK(pm_find_by_pid(m,a.pid,&a));assert(a.state==PM_STATE_WAITING&&a.wait_reason==PM_WAIT_INFERENCE);
    assert(pm_wait(m,a.pid,&a)==PM_PENDING);
    assert(pm_reap(m,a.pid,NULL)==PM_NOT_TERMINATED);
    OK(pm_transition(m,a.pid,PM_STATE_READY,PM_WAIT_NONE));
    OK(pm_transition(m,a.pid,PM_STATE_RUNNING,PM_WAIT_NONE));
    OK(pm_transition(m,a.pid,PM_STATE_WAITING,PM_WAIT_AWAITING_WORK));
    OK(pm_terminate(m,a.pid,PM_EXIT_KILLED));
    assert(pm_terminate(m,a.pid,PM_EXIT_FAILED)==PM_ALREADY_TERMINATED);
    assert(pm_auto_register(m,"medical-agent",NULL,NULL)==PM_ALREADY_TERMINATED);
    OK(pm_find_by_pid(m,a.pid,&a));assert(a.state==PM_STATE_TERMINATED&&a.exit_status==PM_EXIT_KILLED);
    assert(pm_transition(m,a.pid,PM_STATE_READY,PM_WAIT_NONE)==PM_INVALID_TRANSITION);
    assert(pm_add(m,"fourth-agent",NULL,NULL)==PM_FULL); /* zombie holds slot */
    OK(pm_wait(m,a.pid,&a));assert(a.exit_status==PM_EXIT_KILLED);
    assert(pm_wait(m,a.pid,NULL)==PM_NOT_FOUND);
    assert(pm_reap(m,a.pid,NULL)==PM_NOT_FOUND);
    OK(pm_add(m,"fourth-agent",NULL,&a));assert(a.pid==4);
    assert(pm_find_by_pid(m,1,&c)==PM_NOT_FOUND);
    pm_snapshot_t list[2];assert(pm_list(m,list,2)==3);assert(list[0].pid==2&&list[1].pid==3);
    OK(pm_disable(m,"future-agent"));
    assert(pm_auto_register(m,"future-agent",NULL,NULL)==PM_DISABLED);
    OK(pm_disable(m,"navigation-agent"));
    OK(pm_find_by_id(m,"navigation-agent",&b));
    assert(b.state==PM_STATE_TERMINATED&&b.exit_status==PM_EXIT_KILLED);
    assert(pm_disable(m,"never-agent")==PM_FULL);
    assert(pm_auto_register(m,"navigation-agent",NULL,NULL)==PM_DISABLED);
    assert(pm_reactivate(m,"navigation-agent",NULL,&b)==PM_DUPLICATE);
    OK(pm_reap(m,b.pid,NULL));
    OK(pm_reactivate(m,"navigation-agent","NAV",&b));
    assert(b.pid==5 && b.state==PM_STATE_NEW);
    bool disabled;
    OK(pm_is_disabled(m,"navigation-agent",&disabled));assert(!disabled);
    assert(pm_reactivate(m,"future-agent",NULL,&c)==PM_FULL);
    OK(pm_is_disabled(m,"future-agent",&disabled));assert(disabled);
    OK(pm_disable(m,"navigation-agent"));
    assert(pm_list_disabled(m,NULL,0)==2);
    pm_destroy(m);
    printf("PASS: PCB ownership, transitions, zombie lifecycle, wait/reap, identity disabling/reactivation, capacity, event order (%zu events)\n",events);
    return 0;
}
