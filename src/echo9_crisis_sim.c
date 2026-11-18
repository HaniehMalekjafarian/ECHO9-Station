#include "agentos_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sched.h>
#include <sys/wait.h>

typedef struct {
    agentos_mutex_t l_telem;
    agentos_mutex_t l_core_state;
    agentos_mutex_t l_valve_a;
    agentos_mutex_t l_valve_b;
    agentos_mutex_t l_power_bus;

    uint16_t rod_depth[8];
    uint32_t protected_valve_count;
    uint32_t protected_telemetry_seq;

    _Atomic int32_t core_temperature;
    _Atomic int32_t coolant_pressure;
    _Atomic int32_t habitat_oxygen;
    _Atomic uint32_t valve_actuations;
} echo9_station_state_t;

static echo9_station_state_t station;

void echo9_station_init(void) {
    memset(&station, 0, sizeof(station));
    agentos_mutex_init(&station.l_telem, "L_TELEM", 1);
    agentos_mutex_init(&station.l_core_state, "L_CORE_STATE", 2);
    agentos_mutex_init(&station.l_valve_a, "L_VALVE_A", 3);
    agentos_mutex_init(&station.l_valve_b, "L_VALVE_B", 4);
    agentos_mutex_init(&station.l_power_bus, "L_POWER_BUS", 5);

    for (int i = 0; i < 8; ++i) {
        station.rod_depth[i] = 0x0100; /* Baseline 256 units */
    }
    station.protected_valve_count = 0;
    station.protected_telemetry_seq = 0;
    atomic_init(&station.core_temperature, 350);
    atomic_init(&station.coolant_pressure, 100);
    atomic_init(&station.habitat_oxygen, 98);
    atomic_init(&station.valve_actuations, 0);
}

void echo9_station_destroy(void) {
    agentos_mutex_destroy(&station.l_telem);
    agentos_mutex_destroy(&station.l_core_state);
    agentos_mutex_destroy(&station.l_valve_a);
    agentos_mutex_destroy(&station.l_valve_b);
    agentos_mutex_destroy(&station.l_power_bus);
}

echo9_station_state_t *echo9_get_station(void) {
    return &station;
}

/* =========================================================================
 * Phase 1: Transitive Priority Inversion & Oxygen Crisis Stabilization
 * ========================================================================= */
typedef struct {
    _Atomic int ai_holding_core;
    _Atomic int telem_holding_lock;
    _Atomic int release_ai;
    _Atomic int stop_scrubbers;
    _Atomic int life_done;
    _Atomic int failure;
    _Atomic int64_t life_wait_ns;

    agentos_thread_node_t *t_ai;
    agentos_thread_node_t *t_telem;
    agentos_thread_node_t *t_life;
} phase1_ctx_t;

static void *p1_ai_diag_fn(void *arg) {
    phase1_ctx_t *ctx = (phase1_ctx_t *)arg;
    if (agentos_thread_register("T_AI_DIAG", AGENTOS_PRIO_MED_AI_DIAG, &ctx->t_ai) != AGENTOS_OK) {
        atomic_store(&ctx->failure, 1);
        return NULL;
    }
    if (agentos_mutex_lock(&station.l_core_state) != AGENTOS_OK) {
        atomic_store(&ctx->failure, 1);
        agentos_thread_unregister(ctx->t_ai);
        return NULL;
    }
    atomic_store(&ctx->ai_holding_core, 1);

    while (!atomic_load(&ctx->release_ai)) {
        sched_yield();
    }

    if (agentos_thread_get_effective_prio(ctx->t_ai) != AGENTOS_PRIO_CRIT_LIFESUPPORT) {
        atomic_store(&ctx->failure, 1);
    }

    station.protected_telemetry_seq++;
    agentos_mutex_unlock(&station.l_core_state);

    if (agentos_thread_get_effective_prio(ctx->t_ai) != AGENTOS_PRIO_MED_AI_DIAG) {
        atomic_store(&ctx->failure, 1);
    }

    agentos_thread_unregister(ctx->t_ai);
    return NULL;
}

static void *p1_telem_fn(void *arg) {
    phase1_ctx_t *ctx = (phase1_ctx_t *)arg;
    if (agentos_thread_register("T_TELEMETRY", AGENTOS_PRIO_LOW_TELEMETRY, &ctx->t_telem) != AGENTOS_OK) {
        atomic_store(&ctx->failure, 1);
        return NULL;
    }
    if (agentos_mutex_lock(&station.l_telem) != AGENTOS_OK) {
        atomic_store(&ctx->failure, 1);
        agentos_thread_unregister(ctx->t_telem);
        return NULL;
    }
    atomic_store(&ctx->telem_holding_lock, 1);

    /* Blocks on L_CORE_STATE held by T_AI_DIAG */
    if (agentos_mutex_lock(&station.l_core_state) != AGENTOS_OK) {
        agentos_mutex_unlock(&station.l_telem);
        atomic_store(&ctx->failure, 1);
        agentos_thread_unregister(ctx->t_telem);
        return NULL;
    }

    /* Once awakened, verify boosted priority was 99 */
    if (agentos_thread_get_effective_prio(ctx->t_telem) != AGENTOS_PRIO_CRIT_LIFESUPPORT) {
        atomic_store(&ctx->failure, 1);
    }

    station.protected_telemetry_seq++;
    agentos_mutex_unlock(&station.l_core_state);
    agentos_mutex_unlock(&station.l_telem);

    /* Must have decayed back to base priority 10 */
    if (agentos_thread_get_effective_prio(ctx->t_telem) != AGENTOS_PRIO_LOW_TELEMETRY) {
        atomic_store(&ctx->failure, 1);
    }

    agentos_thread_unregister(ctx->t_telem);
    return NULL;
}

static void *p1_scrubber_fn(void *arg) {
    phase1_ctx_t *ctx = (phase1_ctx_t *)arg;
    agentos_thread_node_t *ts = NULL;
    if (agentos_thread_register("T_SCRUBBER", AGENTOS_PRIO_MED_SCRUBBER, &ts) != AGENTOS_OK) {
        return NULL;
    }
    volatile uint32_t parity = 0;
    while (!atomic_load(&ctx->stop_scrubbers)) {
        parity ^= 0x5A5A5A5Au;
        sched_yield();
    }
    (void)parity;
    agentos_thread_unregister(ts);
    return NULL;
}

static void *p1_life_support_fn(void *arg) {
    phase1_ctx_t *ctx = (phase1_ctx_t *)arg;
    if (agentos_thread_register("T_LIFE_SUPPORT", AGENTOS_PRIO_CRIT_LIFESUPPORT, &ctx->t_life) != AGENTOS_OK) {
        atomic_store(&ctx->failure, 1);
        return NULL;
    }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    agentos_mutex_lock(&station.l_telem);
    agentos_mutex_lock(&station.l_power_bus);

    clock_gettime(CLOCK_MONOTONIC, &t1);
    int64_t ns = ((int64_t)(t1.tv_sec - t0.tv_sec) * 1000000000LL) + (t1.tv_nsec - t0.tv_nsec);
    atomic_store(&ctx->life_wait_ns, ns);

    atomic_store(&station.habitat_oxygen, 100);
    agentos_mutex_unlock(&station.l_power_bus);
    agentos_mutex_unlock(&station.l_telem);

    atomic_store(&ctx->life_done, 1);
    agentos_thread_unregister(ctx->t_life);
    return NULL;
}

static int run_phase1(bool verbose) {
    phase1_ctx_t ctx;
    memset(&ctx, 0, sizeof(ctx));

    pthread_t th_ai, th_telem, th_scr1, th_scr2, th_life;
    if (pthread_create(&th_ai, NULL, p1_ai_diag_fn, &ctx) != 0) return -1;
    while (!atomic_load(&ctx.ai_holding_core) && !atomic_load(&ctx.failure)) sched_yield();
    if (atomic_load(&ctx.failure)) { pthread_join(th_ai, NULL); return -1; }

    if (pthread_create(&th_telem, NULL, p1_telem_fn, &ctx) != 0) return -1;
    /* Wait until T_TELEMETRY is waiting on L_CORE_STATE */
    agentos_lockdep_engine_t *eng = agentos_get_engine();
    for (int sp = 0; sp < 500000; ++sp) {
        agentos_wfg_lock(eng);
        uint32_t wc = station.l_core_state.waiter_count;
        agentos_wfg_unlock(eng);
        if (wc >= 1 || atomic_load(&ctx.failure)) break;
        sched_yield();
    }

    pthread_create(&th_scr1, NULL, p1_scrubber_fn, &ctx);
    pthread_create(&th_scr2, NULL, p1_scrubber_fn, &ctx);

    pthread_create(&th_life, NULL, p1_life_support_fn, &ctx);
    /* Wait until T_LIFE_SUPPORT blocks on L_TELEM */
    for (int sp = 0; sp < 500000; ++sp) {
        agentos_wfg_lock(eng);
        uint32_t wc = station.l_telem.waiter_count;
        agentos_wfg_unlock(eng);
        if (wc >= 1 || atomic_load(&ctx.life_done) || atomic_load(&ctx.failure)) break;
        sched_yield();
    }

    /* Check transitive 2-hop PIP boost before releasing T_AI_DIAG */
    int32_t p_telem = agentos_thread_get_effective_prio(ctx.t_telem);
    int32_t p_ai = agentos_thread_get_effective_prio(ctx.t_ai);

    atomic_store(&ctx.release_ai, 1);

    pthread_join(th_life, NULL);
    atomic_store(&ctx.stop_scrubbers, 1);
    pthread_join(th_scr1, NULL);
    pthread_join(th_scr2, NULL);
    pthread_join(th_telem, NULL);
    pthread_join(th_ai, NULL);

    if (atomic_load(&ctx.failure) || p_telem != AGENTOS_PRIO_CRIT_LIFESUPPORT ||
        p_ai != AGENTOS_PRIO_CRIT_LIFESUPPORT) {
        if (verbose) fprintf(stderr, "[Phase 1] FAILED: p_telem=%d p_ai=%d\n", p_telem, p_ai);
        return -1;
    }

    double ms = (double)atomic_load(&ctx.life_wait_ns) / 1000000.0;
    if (verbose) {
        printf("[ECHO-9 Phase 1] 2-hop Transitive PIP 99 propagated cleanly; T_LIFE_SUPPORT woke in %.3f ms\n", ms);
    }
    return 0;
}

/* =========================================================================
 * Phase 2: Mid-Mutation AI Inference Cancellation & Rollback Verification
 * ========================================================================= */
typedef struct {
    _Atomic int in_tx;
    _Atomic int failure;
    int read_fd;
} phase2_ctx_t;

static void p2_unregister_node(void *arg) {
    agentos_thread_node_t *node = (agentos_thread_node_t *)arg;
    if (node) agentos_thread_unregister(node);
}

static void *p2_ai_cancel_fn(void *arg) {
    phase2_ctx_t *ctx = (phase2_ctx_t *)arg;
    agentos_thread_node_t *t_ai = NULL;
    if (agentos_thread_register("T_AI_BURST", AGENTOS_PRIO_MED_AI_DIAG, &t_ai) != AGENTOS_OK) {
        atomic_store(&ctx->failure, 1);
        return NULL;
    }

    agentos_tx_journal_t tx;
    memset(&tx, 0, sizeof(tx));

    pthread_cleanup_push(p2_unregister_node, t_ai);
    AGENTOS_TX_BEGIN(&tx);
    if (agentos_mutex_lock(&station.l_core_state) == AGENTOS_OK &&
        agentos_mutex_lock(&station.l_valve_a) == AGENTOS_OK) {

        void *weights = agentos_tx_alloc(&tx, 16384);
        if (!weights) atomic_store(&ctx->failure, 1);
        else memset(weights, 0x55, 16384);

        if (ctx->read_fd >= 0) {
            agentos_tx_register_fd(&tx, ctx->read_fd);
        }

        uint16_t dirty_rods[4] = { 0xDEAD, 0xDEAD, 0xDEAD, 0xDEAD };
        if (agentos_tx_write(&tx, station.rod_depth, dirty_rods, sizeof(dirty_rods)) != AGENTOS_OK) {
            atomic_store(&ctx->failure, 1);
        }

        if (!atomic_load(&ctx->failure)) {
            AGENTOS_SCHEDULE_POINT(202);
            atomic_store(&ctx->in_tx, 1);
            for (;;) {
                pthread_testcancel();
                sched_yield();
            }
        }
    } else {
        atomic_store(&ctx->failure, 1);
    }
    AGENTOS_TX_END(&tx);
    pthread_cleanup_pop(1);

    return NULL;
}

static int run_phase2(bool verbose) {
    const int iterations = 50;
    for (int i = 0; i < iterations; ++i) {
        int pfds[2] = { -1, -1 };
        if (pipe(pfds) != 0) return -1;

        phase2_ctx_t ctx;
        atomic_init(&ctx.in_tx, 0);
        atomic_init(&ctx.failure, 0);
        ctx.read_fd = pfds[0];

        pthread_t th;
        if (pthread_create(&th, NULL, p2_ai_cancel_fn, &ctx) != 0) {
            close(pfds[0]);
            close(pfds[1]);
            return -1;
        }

        while (!atomic_load(&ctx.in_tx) && !atomic_load(&ctx.failure)) sched_yield();
        if (atomic_load(&ctx.failure)) {
            pthread_cancel(th);
            pthread_join(th, NULL);
            close(pfds[1]);
            return -1;
        }

        pthread_cancel(th);
        pthread_join(th, NULL);

        close(pfds[1]);

        /* Verify all 8 rods restored cleanly & no leaks/locks remain held */
        agentos_mutex_lock(&station.l_valve_a);
        for (int r = 0; r < 8; ++r) {
            if (station.rod_depth[r] != 0x0100) {
                agentos_mutex_unlock(&station.l_valve_a);
                if (verbose) fprintf(stderr, "[Phase 2] Rod corruption at iter %d!\n", i);
                return -1;
            }
        }
        agentos_mutex_unlock(&station.l_valve_a);
    }

    if (verbose) {
        printf("[ECHO-9 Phase 2] %d/%d Mid-Transaction AI cancellations rolled back cleanly (0x0100 intact)\n",
               iterations, iterations);
    }
    return 0;
}

/* =========================================================================
 * Phase 3: High-Contention Valve Competition under TSan (500 iterations)
 * ========================================================================= */
static void *p3_valve_worker(void *arg) {
    const char *name = (const char *)arg;
    agentos_thread_node_t *tw = NULL;
    if (agentos_thread_register(name, AGENTOS_PRIO_HIGH_COOLANT, &tw) != AGENTOS_OK) {
        return NULL;
    }

    for (int i = 0; i < 250; ++i) {
        agentos_mutex_lock(&station.l_valve_a);
        agentos_mutex_lock(&station.l_valve_b);
        AGENTOS_SCHEDULE_POINT(303);
        station.protected_valve_count++;
        atomic_fetch_add(&station.valve_actuations, 1);
        agentos_mutex_unlock(&station.l_valve_b);
        agentos_mutex_unlock(&station.l_valve_a);
    }

    agentos_thread_unregister(tw);
    return NULL;
}

static int run_phase3(bool verbose) {
    station.protected_valve_count = 0;
    pthread_t t_pri, t_sec;
    pthread_create(&t_pri, NULL, p3_valve_worker, (void *)"T_COOLANT_PRI");
    pthread_create(&t_sec, NULL, p3_valve_worker, (void *)"T_COOLANT_SEC");
    pthread_join(t_pri, NULL);
    pthread_join(t_sec, NULL);

    if (station.protected_valve_count != 500) {
        if (verbose) fprintf(stderr, "[Phase 3] Count mismatch: %u != 500\n", station.protected_valve_count);
        return -1;
    }
    if (verbose) {
        printf("[ECHO-9 Phase 3] 500 concurrent valve actuations verified race-free under locks\n");
    }
    return 0;
}

/* =========================================================================
 * Phase 4: Three-Way Cycle Deadlock Injection in Child Fork
 * ========================================================================= */
static int run_phase4(bool verbose) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        int devnull = open("/dev/null", O_WRONLY);
        if (devnull >= 0) {
            dup2(devnull, STDERR_FILENO);
            close(devnull);
        }

        agentos_engine_init(NULL);
        echo9_station_init();
        agentos_thread_node_t *t_pri, *t_sec, *t_ai;
        agentos_thread_register("T_COOLANT_PRI", AGENTOS_PRIO_HIGH_COOLANT, &t_pri);
        agentos_thread_register("T_COOLANT_SEC", AGENTOS_PRIO_HIGH_COOLANT, &t_sec);
        agentos_thread_register("T_AI_DIAG", AGENTOS_PRIO_MED_AI_DIAG, &t_ai);

        /* T_COOLANT_PRI holds L_VALVE_A, waits on L_VALVE_B */
        station.l_valve_a.owner = t_pri;
        t_pri->waiting_on_lock = &station.l_valve_b;

        /* T_COOLANT_SEC holds L_VALVE_B, waits on L_CORE_STATE */
        station.l_valve_b.owner = t_sec;
        t_sec->waiting_on_lock = &station.l_core_state;

        /* T_AI_DIAG holds L_CORE_STATE and calls lock on L_VALVE_A -> 3-way cycle! */
        station.l_core_state.owner = t_ai;
        agentos_thread_set_current(t_ai);
        agentos_mutex_lock(&station.l_valve_a);
        _exit(0);
    }

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 99) {
        if (verbose) fprintf(stderr, "[Phase 4] Expected exit 99 on 3-way cycle!\n");
        return -1;
    }

    if (verbose) {
        printf("[ECHO-9 Phase 4] 3-way valve cycle preempted by Tarjan SCC (exit 99)\n");
    }
    return 0;
}

int echo9_run_crisis_simulation(bool verbose) {
    if (agentos_engine_init(NULL) != AGENTOS_OK) return -1;
    echo9_station_init();

    agentos_thread_node_t *main_node = NULL;
    if (agentos_thread_register("ECHO9_MAIN", AGENTOS_PRIO_HIGH_COOLANT, &main_node) != AGENTOS_OK) {
        return -1;
    }

    if (run_phase1(verbose) != 0) return -1;
    if (run_phase2(verbose) != 0) return -1;
    if (run_phase3(verbose) != 0) return -1;
    if (run_phase4(verbose) != 0) return -1;

    agentos_thread_unregister(main_node);
    echo9_station_destroy();
    agentos_engine_destroy(NULL);
    return 0;
}
