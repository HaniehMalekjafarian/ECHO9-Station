#ifndef AGENTOS_PROCESS_MANAGER_H
#define AGENTOS_PROCESS_MANAGER_H

/* Expanded P1 fixed student-facing ABI, version 2.0.
 * Student implements this API in src/process_manager.c.
 * All snapshots are owned copies, never borrowed mutable PCB pointers.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define AGENTOS_PM_AGENT_ID_MAX 128
#define AGENTOS_PM_ROLE_MAX 32
#define AGENTOS_PM_REQUEST_ID_MAX 128
#define AGENTOS_PM_DEFAULT_CAPACITY 64
#define AGENTOS_PM_DEFAULT_DISABLED_CAPACITY 256

typedef uint64_t agentos_pid_t;

typedef enum {
    PM_STATE_NEW,
    PM_STATE_READY,
    PM_STATE_RUNNING,
    PM_STATE_WAITING,
    PM_STATE_TERMINATED
} pm_state_t;

typedef enum {
    PM_WAIT_NONE,
    PM_WAIT_AWAITING_WORK,
    PM_WAIT_INFERENCE,
    PM_WAIT_CONTROLLED_IO
} pm_wait_reason_t;

typedef enum {
    PM_EXIT_NONE,
    PM_EXIT_OK,
    PM_EXIT_FAILED,
    PM_EXIT_KILLED,
    PM_EXIT_ABORTED
} pm_exit_status_t;

typedef enum {
    PM_OK = 0,
    PM_PENDING,
    PM_NOT_FOUND,
    PM_INVALID_ARGUMENT,
    PM_DUPLICATE,
    PM_DISABLED,
    PM_FULL,
    PM_ALREADY_TERMINATED,
    PM_INVALID_TRANSITION,
    PM_NOT_TERMINATED,
    PM_PID_EXHAUSTED
} pm_result_t;

typedef struct {
    agentos_pid_t pid;
    char agent_id[AGENTOS_PM_AGENT_ID_MAX + 1];
    char role[AGENTOS_PM_ROLE_MAX + 1];
    pm_state_t state;
    pm_wait_reason_t wait_reason;
    pm_exit_status_t exit_status;
    uint64_t created_seq;
    uint64_t last_transition_seq;
    uint64_t generation;
    char current_request_id[AGENTOS_PM_REQUEST_ID_MAX + 1];
} pm_snapshot_t;

typedef struct {
    size_t capacity;          /* Includes zombies; default 64. */
    size_t disabled_capacity; /* Independent memory-only registry, default 256. */
} pm_config_t;

typedef struct agentos_pm agentos_pm_t;

typedef enum {
    PM_EVENT_CREATED,
    PM_EVENT_STATE_CHANGED,
    PM_EVENT_TERMINATED,
    PM_EVENT_REAPED,
    PM_EVENT_ID_DISABLED,
    PM_EVENT_ID_REACTIVATED
} pm_event_type_t;

typedef struct {
    pm_event_type_t type;
    uint64_t seq;
    /* Copy the ID even for a removed identity that never had a PCB. */
    char agent_id[AGENTOS_PM_AGENT_ID_MAX + 1];
    bool has_process;
    pm_state_t previous_state; /* Meaningful for PM_EVENT_STATE_CHANGED. */
    pm_snapshot_t process;     /* Valid only when has_process is true. */
} pm_event_t;

typedef void (*pm_event_callback_t)(const pm_event_t *event, void *user);

/* All functions are P1 single-threaded. P2 will define synchronization.
 * Unsuccessful operations do not mutate state or generate lifecycle events. */
agentos_pm_t *pm_create(const pm_config_t *cfg,
                         pm_event_callback_t callback, void *user);
void pm_destroy(agentos_pm_t *manager);

pm_result_t pm_add(agentos_pm_t *manager, const char *agent_id,
                   const char *role, pm_snapshot_t *created);
pm_result_t pm_auto_register(agentos_pm_t *manager, const char *agent_id,
                             pm_snapshot_t *process, bool *was_created);
pm_result_t pm_find_by_pid(const agentos_pm_t *manager, agentos_pid_t pid,
                            pm_snapshot_t *process);
pm_result_t pm_find_by_id(const agentos_pm_t *manager, const char *agent_id,
                           pm_snapshot_t *process);
pm_result_t pm_is_disabled(const agentos_pm_t *manager, const char *agent_id,
                            bool *disabled);
/* Disabled names are copied into caller-owned buffers. */
size_t pm_list_disabled(const agentos_pm_t *manager,
    char (*out)[AGENTOS_PM_AGENT_ID_MAX + 1], size_t capacity);
/* Returns full count; writes at most capacity snapshots sorted by PID. */
size_t pm_list(const agentos_pm_t *manager, pm_snapshot_t *out, size_t capacity);

pm_result_t pm_transition(agentos_pm_t *manager, agentos_pid_t pid,
                           pm_state_t target, pm_wait_reason_t why);
pm_result_t pm_terminate(agentos_pm_t *manager, agentos_pid_t pid,
                          pm_exit_status_t status);
/* A still-live process returns PM_PENDING promptly; a zombie is reaped once. */
pm_result_t pm_wait(agentos_pm_t *manager, agentos_pid_t pid,
                     pm_snapshot_t *final_snapshot);
pm_result_t pm_reap(agentos_pm_t *manager, agentos_pid_t pid,
                     pm_snapshot_t *final_snapshot);

/* Explicit removal is distinct from ordinary termination. When active work
 * exists, supplied P1 controls defer it until a safe boundary. Removing an
 * unknown identity registers a disabled tombstone without creating a PCB. */
pm_result_t pm_disable(agentos_pm_t *manager, const char *agent_id);
/* Reactivation succeeds only after an old zombie has been reaped; assigns a
 * fresh PID. If creation fails, the identity remains disabled. */
pm_result_t pm_reactivate(agentos_pm_t *manager, const char *agent_id,
                           const char *role, pm_snapshot_t *created);

#endif
