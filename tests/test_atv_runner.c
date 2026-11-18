#include "agentos_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <sched.h>
#include <errno.h>

#define CHECK(cond, name, reason)                                           \
    do {                                                                    \
        if (!(cond)) {                                                      \
            printf("FAIL: %s - %s\n", (name), (reason));                    \
            return false;                                                   \
        }                                                                   \
    } while (0)

#define TEST_PASS(name) do { printf("PASS: %s\n", (name)); return true; } while (0)

/* Helper: forked runner to test operations expected to panic */
static int run_in_fork(void (*func)(void)) {
    pid_t pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    }
    if (pid == 0) {
        if (!getenv("AGENTOS_DEBUG")) {
            int devnull = open("/dev/null", O_WRONLY);
            if (devnull >= 0) {
                dup2(devnull, STDERR_FILENO);
                close(devnull);
            }
        }
        func();
        _exit(0);
    }
    int status = 0;
    waitpid(pid, &status, 0);
    if (WIFEXITED(status)) {
        return WEXITSTATUS(status);
    }
    return -1;
}

/* =========================================================================
 * ATV-01: Self-Deadlock (Relock) Trap
 * ========================================================================= */
static void atv01_child_body(void) {
    if (agentos_engine_init(NULL) != AGENTOS_OK) _exit(1);
    agentos_thread_node_t *t1 = NULL;
    if (agentos_thread_register("t1", AGENTOS_PRIO_LOW_TELEMETRY, &t1) != AGENTOS_OK) _exit(1);

    agentos_mutex_t lock_a;
    if (agentos_mutex_init(&lock_a, "L_A", 1) != AGENTOS_OK) _exit(1);

    if (agentos_mutex_lock(&lock_a) != AGENTOS_OK) _exit(1);
    /* Relock self: must trigger panic immediately */
    agentos_mutex_lock(&lock_a);
    _exit(0);
}

static bool test_atv01_self_deadlock(void) {
    const char *tname = "ATV-01: Self-Deadlock (Relock) Trap";
    int code = run_in_fork(atv01_child_body);
    CHECK(code == 99, tname, "Expected exit code 99 from DEADLOCK_PANIC");
    TEST_PASS("ATV-01: Self-Deadlock (Relock) Trap caught and exited with DEADLOCK_PANIC (99)");
}

/* =========================================================================
 * ATV-02: 4-Hop Diamond + Cycle Hybrid Graph (Tarjan vs Naive DFS)
 * ========================================================================= */
static bool test_atv02_diamond_cycle(void) {
    const char *tname = "ATV-02: 4-Hop Diamond + Cycle Hybrid Graph";
    CHECK(agentos_engine_init(NULL) == AGENTOS_OK, tname, "engine_init failed");

    agentos_thread_node_t *t1 = NULL, *t2 = NULL, *t3 = NULL, *t4 = NULL;
    CHECK(agentos_thread_register("T1", 10, &t1) == AGENTOS_OK, tname, "thread_register T1 failed");
    CHECK(agentos_thread_register("T2", 20, &t2) == AGENTOS_OK, tname, "thread_register T2 failed");
    CHECK(agentos_thread_register("T3", 30, &t3) == AGENTOS_OK, tname, "thread_register T3 failed");
    CHECK(agentos_thread_register("T4", 40, &t4) == AGENTOS_OK, tname, "thread_register T4 failed");

    agentos_mutex_t l1, l2, l3, l4;
    CHECK(agentos_mutex_init(&l1, "L1", 1) == AGENTOS_OK, tname, "mutex_init failed");
    CHECK(agentos_mutex_init(&l2, "L2", 2) == AGENTOS_OK, tname, "mutex_init failed");
    CHECK(agentos_mutex_init(&l3, "L3", 3) == AGENTOS_OK, tname, "mutex_init failed");
    CHECK(agentos_mutex_init(&l4, "L4", 4) == AGENTOS_OK, tname, "mutex_init failed");

    /* Construct Acyclic Diamond Convergence first:
     * T1 -> L1 -> T2 -> L3 -> T4
     * T1/T3 -> L2/L3 -> T4
     */
    l1.owner = t2;
    l2.owner = t3;
    l3.owner = t4;
    l4.owner = t2;

    t2->waiting_on_lock = &l3;
    t3->waiting_on_lock = &l3;
    t4->waiting_on_lock = NULL;

    agentos_thread_node_t *cycle_nodes[AGENTOS_MAX_THREADS];
    size_t cycle_len = 0;

    /* Must return FALSE: cross-edges into T4 are NOT cycles */
    bool acyclic_check = agentos_wfg_detect_cycle_tarjan(t2, cycle_nodes, &cycle_len);
    CHECK(acyclic_check == false, tname, "False positive cycle detected on acyclic diamond");

    /* Now close back-edge: T4 requests L4 (held by T2) -> T4 -> T2 -> T4 */
    t4->waiting_on_lock = &l4;
    bool has_cycle = agentos_wfg_detect_cycle_tarjan(t4, cycle_nodes, &cycle_len);

    CHECK(has_cycle == true, tname, "Failed to detect diamond+backedge cycle");
    CHECK(cycle_len == 2, tname, "Expected exact 2-node cycle");
    CHECK((cycle_nodes[0] == t4 && cycle_nodes[1] == t2) ||
          (cycle_nodes[0] == t2 && cycle_nodes[1] == t4), tname, "Wrong cycle nodes");

    agentos_mutex_destroy(&l1);
    agentos_mutex_destroy(&l2);
    agentos_mutex_destroy(&l3);
    l4.owner = NULL;
    t2->waiting_on_lock = NULL;
    t3->waiting_on_lock = NULL;
    t4->waiting_on_lock = NULL;
    agentos_engine_destroy(NULL);
    TEST_PASS("ATV-02: 4-Hop Diamond + Cycle Hybrid Graph correctly resolved by Tarjan SCC");
}

/* =========================================================================
 * ATV-03: Cycle-Induced PIP Infinite Loop Prevention
 * ========================================================================= */
static void atv03_child_body(void) {
    if (agentos_engine_init(NULL) != AGENTOS_OK) _exit(1);
    agentos_thread_node_t *t1 = NULL, *t2 = NULL;
    if (agentos_thread_register("T1", 90, &t1) != AGENTOS_OK) _exit(1);
    if (agentos_thread_register("T2", 20, &t2) != AGENTOS_OK) _exit(1);

    agentos_mutex_t la, lb;
    if (agentos_mutex_init(&la, "L_A", 1) != AGENTOS_OK) _exit(1);
    if (agentos_mutex_init(&lb, "L_B", 2) != AGENTOS_OK) _exit(1);

    lb.owner = t1;
    la.owner = t2;
    t2->waiting_on_lock = &lb;

    /* T1 (prio 90) attempts to lock LA: forms cycle T1 -> T2 -> T1 */
    agentos_thread_set_current(t1);
    agentos_mutex_lock(&la);
    _exit(0);
}

static bool test_atv03_cycle_pip_safety(void) {
    const char *tname = "ATV-03: Cycle-Induced PIP Infinite Loop";
    int code = run_in_fork(atv03_child_body);
    CHECK(code == 99, tname, "Expected deadlock panic (exit 99) before PIP infinite loop");
    TEST_PASS("ATV-03: Cycle-Induced PIP Infinite Loop prevented by preemptive Tarjan check");
}

/* =========================================================================
 * ATV-04: 3-Hop Transitive PIP Chain
 * ========================================================================= */
static bool test_atv04_transitive_pip(void) {
    const char *tname = "ATV-04: 3-Hop Transitive PIP Chain";
    CHECK(agentos_engine_init(NULL) == AGENTOS_OK, tname, "engine_init failed");
    agentos_thread_node_t *tl = NULL, *tml = NULL, *tmh = NULL, *th = NULL;
    CHECK(agentos_thread_register("T_L", 10, &tl) == AGENTOS_OK, tname, "register failed");
    CHECK(agentos_thread_register("T_ML", 20, &tml) == AGENTOS_OK, tname, "register failed");
    CHECK(agentos_thread_register("T_MH", 30, &tmh) == AGENTOS_OK, tname, "register failed");
    CHECK(agentos_thread_register("T_H", 99, &th) == AGENTOS_OK, tname, "register failed");

    agentos_mutex_t l1, l2, l3;
    CHECK(agentos_mutex_init(&l1, "L1", 1) == AGENTOS_OK, tname, "mutex_init failed");
    CHECK(agentos_mutex_init(&l2, "L2", 2) == AGENTOS_OK, tname, "mutex_init failed");
    CHECK(agentos_mutex_init(&l3, "L3", 3) == AGENTOS_OK, tname, "mutex_init failed");

    l3.owner = tmh;
    l2.owner = tml;
    tml->waiting_on_lock = &l3;
    l3.max_waiter_prio = 20;

    l1.owner = tl;
    tl->waiting_on_lock = &l2;
    l2.max_waiter_prio = 10;

    agentos_lockdep_engine_t *eng = agentos_get_engine();
    agentos_wfg_lock(eng);
    agentos_pip_propagate_on_block(th, &l1);
    agentos_wfg_unlock(eng);

    CHECK(agentos_thread_get_effective_prio(tl) == 99, tname, "T_L not boosted to 99");
    CHECK(agentos_thread_get_effective_prio(tml) == 99, tname, "T_ML not boosted to 99");
    CHECK(agentos_thread_get_effective_prio(tmh) == 99, tname, "T_MH not boosted to 99");

    agentos_engine_destroy(NULL);
    TEST_PASS("ATV-04: 3-Hop Transitive PIP Chain propagates priority 99 across all 3 hops");
}

/* =========================================================================
 * ATV-05: Nested Multi-Lock Partial Unwind PIP Decay
 * ========================================================================= */
static bool test_atv05_multi_lock_pip_decay(void) {
    const char *tname = "ATV-05: Nested Multi-Lock Partial Unwind PIP Decay";
    CHECK(agentos_engine_init(NULL) == AGENTOS_OK, tname, "engine_init failed");
    agentos_thread_node_t *tl = NULL, *th1 = NULL, *th2 = NULL;
    CHECK(agentos_thread_register("T_L", 10, &tl) == AGENTOS_OK, tname, "register failed");
    CHECK(agentos_thread_register("T_H1", 90, &th1) == AGENTOS_OK, tname, "register failed");
    CHECK(agentos_thread_register("T_H2", 70, &th2) == AGENTOS_OK, tname, "register failed");
    agentos_thread_set_current(tl);

    agentos_mutex_t la, lb;
    CHECK(agentos_mutex_init(&la, "L_A", 1) == AGENTOS_OK, tname, "mutex_init failed");
    CHECK(agentos_mutex_init(&lb, "L_B", 2) == AGENTOS_OK, tname, "mutex_init failed");

    CHECK(agentos_mutex_lock(&la) == AGENTOS_OK, tname, "lock LA failed");
    CHECK(agentos_mutex_lock(&lb) == AGENTOS_OK, tname, "lock LB failed");

    agentos_lockdep_engine_t *eng = agentos_get_engine();
    agentos_wfg_lock(eng);
    agentos_pip_propagate_on_block(th1, &la);
    agentos_pip_propagate_on_block(th2, &lb);
    agentos_wfg_unlock(eng);

    CHECK(agentos_thread_get_effective_prio(tl) == 90, tname, "Expected boosted prio 90");

    CHECK(agentos_mutex_unlock(&la) == AGENTOS_OK, tname, "unlock LA failed");
    CHECK(agentos_thread_get_effective_prio(tl) == 70, tname, "Expected decay to 70 while holding LB");

    CHECK(agentos_mutex_unlock(&lb) == AGENTOS_OK, tname, "unlock LB failed");
    CHECK(agentos_thread_get_effective_prio(tl) == 10, tname, "Expected decay back to base 10");

    agentos_mutex_destroy(&la);
    agentos_mutex_destroy(&lb);
    agentos_engine_destroy(NULL);
    TEST_PASS("ATV-05: Nested Multi-Lock Partial Unwind PIP Decay (90 -> 70 -> 10)");
}

/* =========================================================================
 * ATV-06: Cancelled Waiter Priority Ghosting (ASan Use-After-Free check)
 * ========================================================================= */
typedef struct {
    agentos_mutex_t *mutex;
    _Atomic int ready;
    _Atomic int exited_early;
} atv06_arg_t;

static void *atv06_waiter_thread(void *arg) {
    atv06_arg_t *a = (atv06_arg_t *)arg;
    agentos_thread_node_t *th = NULL;
    if (agentos_thread_register("T_H_WAIT", 99, &th) != AGENTOS_OK) {
        atomic_store(&a->exited_early, 1);
        return NULL;
    }

    atomic_store(&a->ready, 1);
    if (agentos_mutex_lock(a->mutex) != AGENTOS_OK) {
        atomic_store(&a->exited_early, 1);
        return NULL;
    }
    agentos_mutex_unlock(a->mutex);
    atomic_store(&a->exited_early, 1);
    return NULL;
}

static bool test_atv06_cancelled_waiter_ghosting(void) {
    const char *tname = "ATV-06: Cancelled Waiter Priority Ghosting";
    CHECK(agentos_engine_init(NULL) == AGENTOS_OK, tname, "engine_init failed");
    agentos_thread_node_t *tl = NULL;
    CHECK(agentos_thread_register("T_L", 10, &tl) == AGENTOS_OK, tname, "register failed");

    agentos_mutex_t lock;
    CHECK(agentos_mutex_init(&lock, "L_GHOST", 1) == AGENTOS_OK, tname, "mutex_init failed");
    CHECK(agentos_mutex_lock(&lock) == AGENTOS_OK, tname, "lock failed");

    atv06_arg_t arg = { .mutex = &lock };
    atomic_init(&arg.ready, 0);
    atomic_init(&arg.exited_early, 0);

    pthread_t th_handle;
    pthread_create(&th_handle, NULL, atv06_waiter_thread, &arg);

    agentos_lockdep_engine_t *eng = agentos_get_engine();
    for (int spins = 0; spins < 500000; ++spins) {
        if (atomic_load(&arg.exited_early)) break;
        agentos_wfg_lock(eng);
        uint32_t wc = lock.waiter_count;
        agentos_wfg_unlock(eng);
        if (wc > 0) break;
        sched_yield();
    }

    if (atomic_load(&arg.exited_early)) {
        pthread_join(th_handle, NULL);
        agentos_mutex_unlock(&lock);
        agentos_engine_destroy(NULL);
        CHECK(false, tname, "Waiter did not block on mutex (lock stub?)");
    }

    CHECK(agentos_thread_get_effective_prio(tl) == 99, tname, "Holder TL not boosted to 99");

    pthread_cancel(th_handle);
    pthread_join(th_handle, NULL);

    CHECK(agentos_thread_get_effective_prio(tl) == 10, tname, "Holder TL priority ghosted after waiter cancel");
    CHECK(agentos_mutex_unlock(&lock) == AGENTOS_OK, tname, "unlock failed after cancelled waiter");

    agentos_mutex_destroy(&lock);
    agentos_engine_destroy(NULL);
    TEST_PASS("ATV-06: Cancelled Waiter Priority Ghosting cleaned up safely");
}

/* =========================================================================
 * ATV-07: Overlapping Memory Write LIFO vs FIFO Rollback
 * ========================================================================= */
static bool test_atv07_lifo_memory_rollback(void) {
    const char *tname = "ATV-07: Overlapping Memory Write LIFO Rollback";
    CHECK(agentos_engine_init(NULL) == AGENTOS_OK, tname, "engine_init failed");
    agentos_thread_node_t *t = NULL;
    CHECK(agentos_thread_register("T_TX", 20, &t) == AGENTOS_OK, tname, "register failed");

    char buffer[16];
    memset(buffer, '0', 16);

    agentos_tx_journal_t tx;
    memset(&tx, 0, sizeof(tx));

    AGENTOS_TX_BEGIN(&tx);
        char a_buf[8];
        memset(a_buf, 'A', 8);
        if (agentos_tx_write(&tx, buffer, a_buf, 8) == AGENTOS_OK) {
            char b_buf[4];
            memset(b_buf, 'B', 4);
            agentos_tx_write(&tx, buffer, b_buf, 4);
        }
    AGENTOS_TX_END(&tx);

    bool restored = true;
    for (int i = 0; i < 16; ++i) {
        if (buffer[i] != '0') { restored = false; break; }
    }
    CHECK(restored, tname, "Overlapping writes were not restored in strict LIFO order");

    agentos_engine_destroy(NULL);
    TEST_PASS("ATV-07: Overlapping Memory Write LIFO vs FIFO Rollback verified");
}

/* =========================================================================
 * ATV-08: Premature Unlock Before Memory Restoration Race
 * ========================================================================= */
typedef struct {
    agentos_mutex_t *lock;
    int *shared_val;
    _Atomic int tx_ready;
    _Atomic int reader_saw_bad;
    _Atomic int reader_done;
} atv08_ctx_t;

static void *atv08_writer_fn(void *arg) {
    atv08_ctx_t *ctx = (atv08_ctx_t *)arg;
    agentos_thread_node_t *tw = NULL;
    if (agentos_thread_register("T_WRITER", 20, &tw) != AGENTOS_OK) return NULL;

    agentos_tx_journal_t tx;
    memset(&tx, 0, sizeof(tx));

    AGENTOS_TX_BEGIN(&tx);
        if (agentos_mutex_lock(ctx->lock) == AGENTOS_OK) {
            int new_val = -1;
            agentos_tx_write(&tx, ctx->shared_val, &new_val, sizeof(int));
            atomic_store(&ctx->tx_ready, 1);
            for (;;) {
                pthread_testcancel();
                sched_yield();
            }
        }
    AGENTOS_TX_END(&tx);
    return NULL;
}

static void *atv08_reader_fn(void *arg) {
    atv08_ctx_t *ctx = (atv08_ctx_t *)arg;
    agentos_thread_node_t *tr = NULL;
    if (agentos_thread_register("T_READER", 30, &tr) != AGENTOS_OK) {
        atomic_store(&ctx->reader_done, 1);
        return NULL;
    }

    if (agentos_mutex_lock(ctx->lock) == AGENTOS_OK) {
        if (*ctx->shared_val != 100) {
            atomic_store(&ctx->reader_saw_bad, 1);
        }
        agentos_mutex_unlock(ctx->lock);
    } else {
        atomic_store(&ctx->reader_saw_bad, 1);
    }
    atomic_store(&ctx->reader_done, 1);
    return NULL;
}

static bool test_atv08_memory_restore_before_unlock(void) {
    const char *tname = "ATV-08: Memory Restore Before Lock Release";
    CHECK(agentos_engine_init(NULL) == AGENTOS_OK, tname, "engine_init failed");

    agentos_mutex_t lock;
    CHECK(agentos_mutex_init(&lock, "L_ATV08", 1) == AGENTOS_OK, tname, "mutex_init failed");

    int shared_val = 100;
    atv08_ctx_t ctx = { .lock = &lock, .shared_val = &shared_val };
    atomic_init(&ctx.tx_ready, 0);
    atomic_init(&ctx.reader_saw_bad, 0);
    atomic_init(&ctx.reader_done, 0);

    pthread_t tw, tr;
    pthread_create(&tw, NULL, atv08_writer_fn, &ctx);

    for (int s = 0; s < 500000 && !atomic_load(&ctx.tx_ready); ++s) sched_yield();
    if (!atomic_load(&ctx.tx_ready)) {
        pthread_cancel(tw);
        pthread_join(tw, NULL);
        agentos_engine_destroy(NULL);
        CHECK(false, tname, "Writer failed to enter transaction (TODO stub)");
    }

    pthread_create(&tr, NULL, atv08_reader_fn, &ctx);
    agentos_lockdep_engine_t *eng = agentos_get_engine();
    for (int s = 0; s < 500000; ++s) {
        if (atomic_load(&ctx.reader_done)) break;
        agentos_wfg_lock(eng);
        uint32_t wc = lock.waiter_count;
        agentos_wfg_unlock(eng);
        if (wc > 0) break;
        sched_yield();
    }

    /* Cancel writer while reader is blocked waiting on L_ATV08 */
    pthread_cancel(tw);
    pthread_join(tw, NULL);
    pthread_join(tr, NULL);

    CHECK(atomic_load(&ctx.reader_saw_bad) == 0, tname, "Reader observed corrupted -1 (lock unlocked before memory restore!)");
    CHECK(shared_val == 100, tname, "Shared value not restored to 100");
    CHECK(lock.owner == NULL, tname, "Lock remained owned after unwind");

    agentos_mutex_destroy(&lock);
    agentos_engine_destroy(NULL);
    TEST_PASS("ATV-08: Memory restored while locks held prior to lock release");
}

/* =========================================================================
 * ATV-09: Heap & FD Ownership Transfer Leak vs Double-Free (100x under LSan)
 * ========================================================================= */
static bool test_atv09_tx_alloc_fd_cleanup(void) {
    const char *tname = "ATV-09: Heap & FD Ownership Transfer Cleanup";
    CHECK(agentos_engine_init(NULL) == AGENTOS_OK, tname, "engine_init failed");
    agentos_thread_node_t *t = NULL;
    CHECK(agentos_thread_register("T_ATV09", 20, &t) == AGENTOS_OK, tname, "register failed");

    for (int iter = 0; iter < 100; ++iter) {
        int fds[2];
        CHECK(pipe(fds) == 0, tname, "pipe creation failed (FD leak?)");

        agentos_tx_journal_t tx;
        memset(&tx, 0, sizeof(tx));

        void *p1 = NULL;
        void *p2 = NULL;

        AGENTOS_TX_BEGIN(&tx);
            p1 = agentos_tx_alloc(&tx, 64);
            p2 = agentos_tx_alloc(&tx, 64);
            if (p1 && p2) {
                agentos_tx_transfer_alloc(&tx, p1);
                agentos_tx_register_fd(&tx, fds[0]);
            }
        AGENTOS_TX_END(&tx);

        CHECK(p1 != NULL && p2 != NULL, tname, "agentos_tx_alloc failed");
        memset(p1, 0xAA, 64);
        free(p1);
        close(fds[1]);
        /* Verify fds[0] was closed by abort handler (fcntl must return EBADF) */
        CHECK(fcntl(fds[0], F_GETFD) == -1 && errno == EBADF, tname, "Registered FD leaked on abort");
    }

    agentos_engine_destroy(NULL);
    TEST_PASS("ATV-09: Heap & FD Ownership Transfer Leak vs Double-Free verified (100x)");
}

/* =========================================================================
 * ATV-10: TOCTOU Window & Deterministic Schedule Point Reproducibility (10x)
 * ========================================================================= */
typedef struct {
    _Atomic int shared_flag;
    _Atomic int toctou_caught;
    agentos_thread_node_t *t1;
    agentos_thread_node_t *t2;
} atv10_ctx_t;

static void *atv10_step_thread1(void *arg) {
    atv10_ctx_t *c = (atv10_ctx_t *)arg;
    agentos_thread_set_current(c->t1);
    AGENTOS_SCHEDULE_POINT(400);
    if (atomic_load(&c->shared_flag) == 0) {
        AGENTOS_SCHEDULE_POINT(404);
        if (atomic_load(&c->shared_flag) != 0) {
            atomic_store(&c->toctou_caught, 1);
        }
        atomic_store(&c->shared_flag, 1);
    }
    return NULL;
}

static void *atv10_step_thread2(void *arg) {
    atv10_ctx_t *c = (atv10_ctx_t *)arg;
    agentos_thread_set_current(c->t2);
    AGENTOS_SCHEDULE_POINT(401);
    atomic_store(&c->shared_flag, 1);
    AGENTOS_SCHEDULE_POINT(402);
    return NULL;
}

static void *atv10_prng_worker(void *arg) {
    agentos_thread_node_t *n = (agentos_thread_node_t *)arg;
    agentos_thread_set_current(n);
    for (int i = 0; i < 20; ++i) {
        AGENTOS_SCHEDULE_POINT(410 + i);
    }
    return NULL;
}

static bool test_atv10_toctou_schedule_point(void) {
    const char *tname = "ATV-10: Deterministic Schedule Point & Reproducibility";
    CHECK(agentos_engine_init(NULL) == AGENTOS_OK, tname, "engine_init failed");

    agentos_thread_node_t *n1 = NULL, *n2 = NULL;
    CHECK(agentos_thread_register("T_S1", 20, &n1) == AGENTOS_OK, tname, "register n1 failed");
    CHECK(agentos_thread_register("T_S2", 20, &n2) == AGENTOS_OK, tname, "register n2 failed");

    /* 1. Explicit TOCTOU sequence: T1(400)->T1(404)[yield to T2]->T2(401)->T2(402)[unpark T1] */
    agentos_sched_step_t steps[4] = {
        { .target_tid_slot = n1->tid_slot, .expected_tag = 400, .inject_cancel = false },
        { .target_tid_slot = n1->tid_slot, .expected_tag = 404, .inject_cancel = false },
        { .target_tid_slot = n2->tid_slot, .expected_tag = 401, .inject_cancel = false },
        { .target_tid_slot = n2->tid_slot, .expected_tag = 402, .inject_cancel = false }
    };

    atv10_ctx_t ctx = { .t1 = n1, .t2 = n2 };
    atomic_init(&ctx.shared_flag, 0);
    atomic_init(&ctx.toctou_caught, 0);
    agentos_sched_init(AGENTOS_SCHED_EXPLICIT_STEP, 0, steps, 4);

    pthread_t th1, th2;
    pthread_create(&th1, NULL, atv10_step_thread1, &ctx);
    pthread_create(&th2, NULL, atv10_step_thread2, &ctx);
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    agentos_sched_shutdown();

    CHECK(atomic_load(&ctx.toctou_caught) == 1, tname, "Explicit schedule trace failed to force TOCTOU interleaving");

    /* 2. Seeded PCT_PRNG determinism check across 10 runs */
    uint64_t expected_hash = 0;
    for (int r = 0; r < 10; ++r) {
        agentos_sched_init(AGENTOS_SCHED_PCT_PRNG, 0xEC409000ULL, NULL, 0);
        atv10_prng_worker(n1);
        uint64_t h = agentos_sched_get_trace_hash();
        agentos_sched_shutdown();
        if (r == 0) expected_hash = h;
        CHECK(h != 0 && h == expected_hash, tname, "PRNG trace hash diverged across identical seeds");
    }

    agentos_engine_destroy(NULL);
    TEST_PASS("ATV-10: TOCTOU Schedule Point intercepted and executed reproducibly (10x)");
}

/* =========================================================================
 * ATV-11: Harness Synchronization Masking TSan Data Race
 * ========================================================================= */
typedef struct {
    agentos_mutex_t lock;
    int shared_val;
    agentos_thread_node_t *t1;
    agentos_thread_node_t *t2;
} atv11_ctx_t;

static void *atv11_safe_t1(void *arg) {
    atv11_ctx_t *c = (atv11_ctx_t *)arg;
    agentos_thread_set_current(c->t1);
    agentos_mutex_lock(&c->lock);
    c->shared_val += 1;
    agentos_mutex_unlock(&c->lock);
    AGENTOS_SCHEDULE_POINT(505);
    return NULL;
}

static void *atv11_safe_t2(void *arg) {
    atv11_ctx_t *c = (atv11_ctx_t *)arg;
    agentos_thread_set_current(c->t2);
    AGENTOS_SCHEDULE_POINT(506);
    agentos_mutex_lock(&c->lock);
    c->shared_val += 2;
    agentos_mutex_unlock(&c->lock);
    return NULL;
}

static bool test_atv11_tsan_harness_isolation(void) {
    const char *tname = "ATV-11: Harness TSan Isolation";
    CHECK(agentos_engine_init(NULL) == AGENTOS_OK, tname, "engine_init failed");
    agentos_thread_node_t *t1 = NULL, *t2 = NULL;
    CHECK(agentos_thread_register("T_TS1", 20, &t1) == AGENTOS_OK, tname, "register failed");
    CHECK(agentos_thread_register("T_TS2", 20, &t2) == AGENTOS_OK, tname, "register failed");

    atv11_ctx_t ctx = { .shared_val = 0, .t1 = t1, .t2 = t2 };
    CHECK(agentos_mutex_init(&ctx.lock, "L_TSAN", 1) == AGENTOS_OK, tname, "mutex_init failed");

    agentos_sched_step_t steps[2] = {
        { .target_tid_slot = t1->tid_slot, .expected_tag = 505, .inject_cancel = false },
        { .target_tid_slot = t2->tid_slot, .expected_tag = 506, .inject_cancel = false }
    };
    agentos_sched_init(AGENTOS_SCHED_EXPLICIT_STEP, 0, steps, 2);

    pthread_t th1, th2;
    pthread_create(&th1, NULL, atv11_safe_t1, &ctx);
    pthread_create(&th2, NULL, atv11_safe_t2, &ctx);
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);
    agentos_sched_shutdown();

    CHECK(ctx.shared_val == 3, tname, "Mutex-guarded shared value mismatch");
    agentos_mutex_destroy(&ctx.lock);
    agentos_engine_destroy(NULL);
    TEST_PASS("ATV-11: Harness synchronization isolated from TSan happens-before clock");
}

/* Intentional unguarded race across AGENTOS_SCHEDULE_POINT(505) for TSan probe */
static int probe_racy_global = 0;
static void *probe_t1_fn(void *arg) {
    agentos_thread_set_current((agentos_thread_node_t *)arg);
    AGENTOS_SCHEDULE_POINT(504);
    probe_racy_global = 1;
    AGENTOS_SCHEDULE_POINT(505);
    return NULL;
}
static void *probe_t2_fn(void *arg) {
    agentos_thread_set_current((agentos_thread_node_t *)arg);
    AGENTOS_SCHEDULE_POINT(506);
    probe_racy_global = 2;
    return NULL;
}
static int run_tsan_race_probe(void) {
    agentos_engine_init(NULL);
    agentos_thread_node_t *t1 = NULL, *t2 = NULL;
    agentos_thread_register("PROBE1", 20, &t1);
    agentos_thread_register("PROBE2", 20, &t2);
    agentos_sched_step_t steps[3] = {
        { .target_tid_slot = t1->tid_slot, .expected_tag = 504, .inject_cancel = false },
        { .target_tid_slot = t1->tid_slot, .expected_tag = 505, .inject_cancel = false },
        { .target_tid_slot = t2->tid_slot, .expected_tag = 506, .inject_cancel = false }
    };
    agentos_sched_init(AGENTOS_SCHED_EXPLICIT_STEP, 0, steps, 3);
    pthread_t a, b;
    pthread_create(&a, NULL, probe_t1_fn, t1);
    pthread_create(&b, NULL, probe_t2_fn, t2);
    pthread_join(a, NULL);
    pthread_join(b, NULL);
    agentos_sched_shutdown();
    agentos_engine_destroy(NULL);
    return 0;
}

/* =========================================================================
 * ATV-12: Full ECHO-9 Reactor Meltdown Chaos Storm
 * ========================================================================= */
static bool test_atv12_echo9_chaos_storm(void) {
    const char *tname = "ATV-12: Full ECHO-9 Reactor Meltdown Chaos Storm";
    CHECK(echo9_run_crisis_simulation(false) == 0, tname, "Reactor crisis simulation invariants failed");
    TEST_PASS("ATV-12: Full ECHO-9 Reactor Meltdown Chaos Storm invariants verified");
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--probe-race") == 0) {
        return run_tsan_race_probe();
    }

    printf("=======================================================\n");
    printf("AgentOS M2 Adversarial Test Vector (ATV) Execution\n");
    printf("=======================================================\n");

    bool (*tests[12])(void) = {
        test_atv01_self_deadlock,
        test_atv02_diamond_cycle,
        test_atv03_cycle_pip_safety,
        test_atv04_transitive_pip,
        test_atv05_multi_lock_pip_decay,
        test_atv06_cancelled_waiter_ghosting,
        test_atv07_lifo_memory_rollback,
        test_atv08_memory_restore_before_unlock,
        test_atv09_tx_alloc_fd_cleanup,
        test_atv10_toctou_schedule_point,
        test_atv11_tsan_harness_isolation,
        test_atv12_echo9_chaos_storm
    };

    int passed = 0;
    for (int i = 0; i < 12; ++i) {
        if (tests[i]()) {
            passed++;
        }
    }

    printf("=======================================================\n");
    printf("SUMMARY: %d / 12 ADVERSARIAL TEST VECTORS PASSED\n", passed);
    printf("=======================================================\n");
    return (passed == 12) ? 0 : 1;
}
