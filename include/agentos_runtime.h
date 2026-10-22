#ifndef AGENTOS_RUNTIME_H
#define AGENTOS_RUNTIME_H
#include "process_manager.h"
#include <json-c/json.h>

/* Instructor-supplied integration. P1 ProcessManager remains student-owned. */
int agentos_runtime_init(void);
void agentos_runtime_destroy(void);
agentos_pm_t *agentos_runtime_pm(void);
void agentos_runtime_request_begin(const char *agent_id, agentos_pid_t pid);
void agentos_runtime_inference_begin(agentos_pid_t pid);
void agentos_runtime_inference_end(agentos_pid_t pid);
void agentos_runtime_request_end(agentos_pid_t pid);
json_object *agentos_runtime_events(unsigned long long after_seq);
json_object *agentos_runtime_snapshot_json(const pm_snapshot_t *p);
json_object *agentos_runtime_roster(void);
unsigned long long agentos_runtime_id(void);
#endif
