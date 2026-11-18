#ifndef AGENTOS_SYNC_H
#define AGENTOS_SYNC_H

#define _GNU_SOURCE

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdatomic.h>
#include <pthread.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
=========================================================================
* 1. SYSTEM CONSTANTS & ERROR CODES
=========================================================================
*/
#define AGENTOS_MAX_THREADS          64
#define AGENTOS_MAX_LOCKS            128
#define AGENTOS_MAX_STACK_FRAMES     32
#define AGENTOS_TX_MAX_UNDO_RECORDS  256
#define AGENTOS_TX_ARENA_CAPACITY    65536

#define AGENTOS_PRIO_MIN             1
#define AGENTOS_PRIO_LOW_TELEMETRY   10
#define AGENTOS_PRIO_MED_SCRUBBER    40
#define AGENTOS_PRIO_MED_AI_DIAG     50
#define AGENTOS_PRIO_HIGH_COOLANT    80
#define AGENTOS_PRIO_CRIT_LIFESUPPORT 99

typedef enum {
    AGENTOS_OK                  = 0,
    AGENTOS_ERR_INVALID_ARG     = -1,
    AGENTOS_ERR_NOT_OWNER       = -2,
    AGENTOS_ERR_BUSY            = -3,
    AGENTOS_ERR_TIMEDOUT        = -4,
    AGENTOS_ERR_NOMEM           = -5,
    AGENTOS_ERR_TX_OVERFLOW     = -6,
    AGENTOS_ERR_DEADLOCK_PANIC  = -99
} agentos_err_t;

/* Forward declarations */
typedef struct agentos_thread_node agentos_thread_node_t;
typedef struct agentos_mutex agentos_mutex_t;
typedef struct agentos_tx_journal agentos_tx_journal_t;

/*
=========================================================================
* 2. COMPILER-RT THREADSANITIZER DYNAMIC ANNOTATION HOOKS (PILLAR E)
=========================================================================
*/
#if defined(__SANITIZE_THREAD__)
#define AGENTOS_TSAN_ENABLE 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define AGENTOS_TSAN_ENABLE 1
#endif
#endif

#ifndef AGENTOS_TSAN_ENABLE
#define AGENTOS_TSAN_ENABLE 0
#endif

#if AGENTOS_TSAN_ENABLE
extern void __tsan_mutex_create(void *addr, unsigned flags);
extern void __tsan_mutex_destroy(void *addr, unsigned flags);
extern void __tsan_mutex_pre_lock(void *addr, unsigned flags);
extern void __tsan_mutex_post_lock(void *addr, unsigned flags, int recursion);
extern int  __tsan_mutex_pre_unlock(void *addr, unsigned flags);
extern void __tsan_mutex_post_unlock(void *addr, unsigned flags);
#else
#define __tsan_mutex_create(addr, flags)          ((void)0)
#define __tsan_mutex_destroy(addr, flags)         ((void)0)
#define __tsan_mutex_pre_lock(addr, flags)        ((void)0)
#define __tsan_mutex_post_lock(addr, flags, rec)  ((void)0)
#define __tsan_mutex_pre_unlock(addr, flags)      ((void)0)
#define __tsan_mutex_post_unlock(addr, flags)     ((void)0)
#endif

/*
=========================================================================
* 3. PILLAR C: USER-SPACE LOCKDEP DAG/WFG & PRIORITY INHERITANCE STRUCTS
=========================================================================
*/

/**
 * Stack trace snapshot captured at lock acquisition or wait initiation.
 * Used by backtrace_symbols_fd() during DEADLOCK_DETECTED_PANIC.
 */
typedef struct {
    void *frames[AGENTOS_MAX_STACK_FRAMES];
    int frame_count;
    const char *call_site_file;
    int call_site_line;
} agentos_stack_trace_t;

/**
 * Waiter queue entry linking a blocked thread into a mutex's priority wait list.
 */
typedef struct agentos_waiter_node {
    agentos_thread_node_t *thread;
    pthread_mutex_t waiter_mutex; /* Per-waiter sleep mutex isolating global wfg_lock */
    pthread_cond_t wake_cond;     /* Per-waiter condition variable for directed handoff */
    _Atomic bool granted;         /* True when ownership is transferred on unlock */
    struct agentos_waiter_node *next;
    struct agentos_waiter_node *prev;
} agentos_waiter_node_t;

/**
 * Resource vertex in the Bipartite Graph representing a PIP/Lockdep-tracked mutex.
 */
struct agentos_mutex {
    uint32_t lock_id;           /* 0 .. AGENTOS_MAX_LOCKS - 1 */
    const char *name;           /* Symbolic lock name (e.g., "L_VALVE_A") */
    uint32_t lock_class_id;     /* Static class ID for lock-order matrix */

    /* Ownership & Waiter State (Protected by global wfg_lock) */
    agentos_thread_node_t *owner;           /* Current holder (NULL if unlocked) */
    agentos_waiter_node_t *waiters_head;    /* Priority-ordered doubly-linked list of waiters */
    uint32_t waiter_count;
    int32_t max_waiter_prio;                /* Cached \Pi(L_k) = max_{w} P_eff(w) */

    /* Intrusive list pointers for owner->held_locks_head */
    agentos_mutex_t *owner_next_lock;
    agentos_mutex_t *owner_prev_lock;

    /* Diagnostic trace of when current owner acquired this lock */
    agentos_stack_trace_t acquire_trace;
};

/**
 * Thread vertex in the Bipartite Resource-Allocation & Wait-For Graph.
 */
struct agentos_thread_node {
    uint32_t tid_slot;                      /* 0 .. AGENTOS_MAX_THREADS - 1 */
    pthread_t os_thread;                    /* Underlying POSIX thread handle */
    char name[32];                          /* Symbolic agent identifier */

    /* Priority Inheritance Protocol (PIP) state */
    _Atomic int32_t base_prio;              /* Static default priority */
    _Atomic int32_t eff_prio;               /* Dynamic boosted priority */

    /* Bipartite Graph Edges */
    agentos_mutex_t *waiting_on_lock;       /* Outgoing request edge (at most 1) */
    agentos_mutex_t *held_locks_head;       /* Intrusive doubly-linked list of owned locks */
    uint32_t held_lock_count;

    /* Tarjan SCC / Three-Color DFS Traversal Scratchpad (Protected by wfg_lock) */
    int32_t dfs_index;                      /* Discovery timestamp (-1 if unvisited) */
    int32_t dfs_lowlink;                    /* Lowest reachable ancestor index */
    bool dfs_on_stack;                      /* True if currently on recursion stack (Gray) */
    agentos_thread_node_t *dfs_pred_thread; /* Predecessor thread for cycle reconstruction */
    agentos_mutex_t *dfs_pred_lock;         /* Lock edge traversed to reach this node */

    /* Active transaction journal & diagnostic backtrace */
    agentos_tx_journal_t *active_tx;
    agentos_stack_trace_t last_wait_trace;
};

/**
 * Global Lockdep Engine State containing the Bipartite Graph and Lock-Order Matrix.
 */
typedef struct {
    pthread_mutex_t wfg_lock;               /* Engine metadata serialization lock */
    agentos_thread_node_t *threads[AGENTOS_MAX_THREADS];
    agentos_mutex_t *locks[AGENTOS_MAX_LOCKS];
    uint32_t active_thread_count;
    uint32_t registered_lock_count;

    /* Historical Lock-Class Order Matrix: order_dag[A][B] == true iff L_A -> L_B observed */
    bool order_dag[AGENTOS_MAX_LOCKS][AGENTOS_MAX_LOCKS];

    /* Telemetry counters for verification rubric */
    _Atomic uint64_t total_acquisitions;
    _Atomic uint64_t pip_boost_events;
    _Atomic uint64_t pip_transitive_hops;
    _Atomic uint64_t cycles_detected;
} agentos_lockdep_engine_t;

/*
=========================================================================
* 4. PILLAR D: TRANSACTIONAL ROLLBACK JOURNAL ("CLEAN DEATH" CONTRACT)
=========================================================================
*/
typedef enum {
    AGENTOS_UNDO_LOCK   = 1, /* Release held agentos_mutex_t on abort */
    AGENTOS_UNDO_MEM    = 2, /* Restore pre-image bytes to shared memory */
    AGENTOS_UNDO_MALLOC = 3, /* Free uncommitted heap allocation */
    AGENTOS_UNDO_FD     = 4, /* Close uncommitted file/IPC descriptor */
    AGENTOS_UNDO_CUSTOM = 5  /* Execute custom compensation callback */
} agentos_undo_type_t;

typedef struct {
    agentos_undo_type_t type;
    bool disarmed; /* True if committed or manually released */
    union {
        struct {
            agentos_mutex_t *mutex;
        } lock_rec;
        struct {
            void *target_addr;
            size_t length;
            size_t arena_offset; /* Offset into journal->preimage_arena */
        } mem_rec;
        struct {
            void *heap_ptr;
        } malloc_rec;
        struct {
            int fd;
        } fd_rec;
        struct {
            void (*fn)(void *arg);
            void *arg;
        } custom_rec;
    } data;
} agentos_undo_record_t;

struct agentos_tx_journal {
    agentos_thread_node_t *owner_thread;
    bool in_transaction;
    bool committed;
    uint32_t record_count;
    size_t arena_used;
    agentos_undo_record_t records[AGENTOS_TX_MAX_UNDO_RECORDS];
    uint8_t preimage_arena[AGENTOS_TX_ARENA_CAPACITY];
};

/**
 * Lexical macro pair enforcing POSIX pthread_cleanup_push / pop pairing rules.
 * Do NOT exit this block via return, break, continue, or goto.
 */
#define AGENTOS_TX_BEGIN(tx_ptr)                                         \
    do {                                                                 \
        agentos_tx_begin_internal((tx_ptr));                            \
        pthread_cleanup_push(agentos_tx_cleanup_handler, (void *)(tx_ptr))

#define AGENTOS_TX_END(tx_ptr)                                           \
        pthread_cleanup_pop(!(tx_ptr)->committed);                       \
    } while (0)

/*
=========================================================================
* 5. PILLAR E: DETERMINISTIC SCHEDULE POINT HARNESS INTERFACES
=========================================================================
*/
typedef enum {
    AGENTOS_SCHED_PASSTHROUGH   = 0, /* Native OS scheduling */
    AGENTOS_SCHED_PCT_PRNG      = 1, /* Seeded Probabilistic Concurrency Testing */
    AGENTOS_SCHED_EXPLICIT_STEP = 2  /* Instructor step-by-step trace replay */
} agentos_sched_mode_t;

typedef struct {
    uint32_t target_tid_slot;
    uint32_t expected_tag;
    bool inject_cancel; /* Cancel thread at this schedule point */
} agentos_sched_step_t;

void agentos_sched_init(agentos_sched_mode_t mode, uint64_t prng_seed,
                        const agentos_sched_step_t *trace, size_t trace_len);
void agentos_sched_point_intercept(uint32_t tag_id, const char *file, int line);
void agentos_sched_shutdown(void);
uint64_t agentos_sched_get_trace_hash(void);

#define AGENTOS_SCHEDULE_POINT(tag_id) \
    agentos_sched_point_intercept((uint32_t)(tag_id), __FILE__, __LINE__)

/*
=========================================================================
* 6. CORE ENGINE PUBLIC API SIGNATURES (STUDENT IMPLEMENTATION TARGETS)
=========================================================================
*/

/* Engine & Thread Lifecycle */
agentos_err_t agentos_engine_init(agentos_lockdep_engine_t *engine);
agentos_err_t agentos_engine_destroy(agentos_lockdep_engine_t *engine);
agentos_err_t agentos_thread_register(const char *name, int32_t base_prio,
                                      agentos_thread_node_t **out_node);
agentos_err_t agentos_thread_unregister(agentos_thread_node_t *node);
int32_t agentos_thread_get_effective_prio(const agentos_thread_node_t *node);
void agentos_thread_set_current(agentos_thread_node_t *node);

/* Mutex Operations with Preemptive Lockdep & Transitive PIP */
agentos_err_t agentos_mutex_init(agentos_mutex_t *mutex, const char *name,
                                 uint32_t lock_class_id);
agentos_err_t agentos_mutex_destroy(agentos_mutex_t *mutex);
agentos_err_t agentos_mutex_lock_debug(agentos_mutex_t *mutex,
                                       const char *file, int line);
agentos_err_t agentos_mutex_trylock(agentos_mutex_t *mutex);
agentos_err_t agentos_mutex_unlock(agentos_mutex_t *mutex);

#define agentos_mutex_lock(m) agentos_mutex_lock_debug((m), __FILE__, __LINE__)

/* Cycle Detection & Panic Reporting */
bool agentos_wfg_detect_cycle_tarjan(agentos_thread_node_t *start_node,
                                     agentos_thread_node_t **cycle_path_out,
                                     size_t *cycle_len_out);

_Noreturn void agentos_panic_deadlock(const agentos_thread_node_t *caller,
                                      const agentos_mutex_t *target_lock,
                                      agentos_thread_node_t * const *cycle_nodes,
                                      size_t cycle_len);

/* Transactional Rollback & Clean Death Operations */
void agentos_tx_begin_internal(agentos_tx_journal_t *tx);
void agentos_tx_commit(agentos_tx_journal_t *tx);
void agentos_tx_cleanup_handler(void *tx_void_ptr);
agentos_err_t agentos_tx_write(agentos_tx_journal_t *tx, void *dest,
                               const void *src, size_t len);
void *agentos_tx_alloc(agentos_tx_journal_t *tx, size_t size);
agentos_err_t agentos_tx_transfer_alloc(agentos_tx_journal_t *tx, void *ptr);
agentos_err_t agentos_tx_register_fd(agentos_tx_journal_t *tx, int fd);

/* Priority Inheritance internals (also exposed for testing / PIP verification) */
void agentos_pip_propagate_on_block(agentos_thread_node_t *waiter, agentos_mutex_t *target_lock);
void agentos_pip_recompute_on_unlock(agentos_thread_node_t *releaser, agentos_mutex_t *unlocked_mutex);

/* Global engine accessor */
agentos_lockdep_engine_t *agentos_get_engine(void);
void agentos_wfg_lock(agentos_lockdep_engine_t *eng);
void agentos_wfg_unlock(agentos_lockdep_engine_t *eng);

/* Instructor ECHO-9 Reactor Crisis Simulation (Pillar F) */
void echo9_station_init(void);
void echo9_station_destroy(void);
int echo9_run_crisis_simulation(bool verbose);

#ifdef __cplusplus
}
#endif

#endif /* AGENTOS_SYNC_H */
