#include "agentos_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <linux/futex.h>
#include <sched.h>
#include <errno.h>

extern agentos_thread_node_t *agentos_thread_current(void);

typedef struct {
    agentos_sched_mode_t mode;
    uint64_t prng_state;
    const agentos_sched_step_t *trace;
    size_t trace_len;
    size_t current_step;
    bool active;

    /* Trace hash recorded for reproducibility checks (RUB-E1) */
    uint64_t trace_hash;
    uint64_t step_count;

    /* Per-thread futex park gate (values: 0 = parked, 1 = runnable) */
    int park_futex[AGENTOS_MAX_THREADS];
    int internal_spin;
} agentos_sched_harness_internal_t;

static agentos_sched_harness_internal_t harness;

/*
 * Low-level futex park/wake & lock primitives.
 * Marked with __attribute__((no_sanitize("thread"))) so ThreadSanitizer
 * does NOT register a false happens-before edge between scheduled threads!
 */
__attribute__((no_sanitize("thread")))
static void harness_spin_lock(void) {
    while (__atomic_exchange_n(&harness.internal_spin, 1, __ATOMIC_RELAXED) != 0) {
        sched_yield();
    }
}

__attribute__((no_sanitize("thread")))
static void harness_spin_unlock(void) {
    __atomic_store_n(&harness.internal_spin, 0, __ATOMIC_RELAXED);
}

__attribute__((no_sanitize("thread")))
static void raw_futex_park(int *addr) {
    while (__atomic_load_n(addr, __ATOMIC_RELAXED) == 0) {
        syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, 0, NULL, NULL, 0);
    }
    __atomic_store_n(addr, 0, __ATOMIC_RELAXED);
}

__attribute__((no_sanitize("thread")))
static void raw_futex_wake(int *addr) {
    __atomic_store_n(addr, 1, __ATOMIC_RELAXED);
    syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}

/* 64-bit Xorshift PRNG */
__attribute__((no_sanitize("thread")))
static uint64_t xorshift64(uint64_t *state) {
    uint64_t x = *state;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    *state = x;
    return x;
}

__attribute__((no_sanitize("thread")))
void agentos_sched_init(agentos_sched_mode_t mode, uint64_t prng_seed,
                        const agentos_sched_step_t *trace, size_t trace_len) {
    memset(&harness, 0, sizeof(harness));
    harness.mode = mode;
    harness.prng_state = (prng_seed != 0) ? prng_seed : 0xEC409000ULL;
    harness.trace = trace;
    harness.trace_len = trace_len;
    harness.current_step = 0;
    harness.active = (mode != AGENTOS_SCHED_PASSTHROUGH);
    harness.trace_hash = 0x9E3779B97F4A7C15ULL;
    harness.step_count = 0;
    harness.internal_spin = 0;

    for (int i = 0; i < AGENTOS_MAX_THREADS; ++i) {
        harness.park_futex[i] = 0;
    }
}

__attribute__((no_sanitize("thread")))
void agentos_sched_shutdown(void) {
    harness_spin_lock();
    harness.active = false;
    harness_spin_unlock();
    for (int i = 0; i < AGENTOS_MAX_THREADS; ++i) {
        raw_futex_wake(&harness.park_futex[i]);
    }
}

__attribute__((no_sanitize("thread")))
uint64_t agentos_sched_get_trace_hash(void) {
    return harness.trace_hash;
}

__attribute__((no_sanitize("thread")))
static void mix_trace(uint32_t tid_slot, uint32_t tag_id) {
    uint64_t v = ((uint64_t)tid_slot << 32) | (uint64_t)tag_id;
    harness.trace_hash ^= v + 0x9E3779B97F4A7C15ULL + (harness.trace_hash << 6) + (harness.trace_hash >> 2);
    harness.step_count++;
}

__attribute__((no_sanitize("thread")))
void agentos_sched_point_intercept(uint32_t tag_id, const char *file, int line) {
    (void)file;
    (void)line;

    if (!harness.active || harness.mode == AGENTOS_SCHED_PASSTHROUGH) {
        return;
    }

    agentos_thread_node_t *cur = agentos_thread_current();
    if (!cur) return;
    uint32_t my_slot = cur->tid_slot;

    if (harness.mode == AGENTOS_SCHED_EXPLICIT_STEP) {
        bool consumed = false;
        for (;;) {
            harness_spin_lock();
            if (!harness.active || !harness.trace || harness.current_step >= harness.trace_len) {
                for (int i = 0; i < AGENTOS_MAX_THREADS; ++i) {
                    if ((uint32_t)i != my_slot) raw_futex_wake(&harness.park_futex[i]);
                }
                harness_spin_unlock();
                return;
            }
            const agentos_sched_step_t *step = &harness.trace[harness.current_step];
            if (consumed) {
                if (step->target_tid_slot == my_slot) {
                    harness_spin_unlock();
                    return;
                }
                uint32_t wake_slot = step->target_tid_slot;
                __atomic_store_n(&harness.park_futex[my_slot], 0, __ATOMIC_RELAXED);
                if (wake_slot < AGENTOS_MAX_THREADS && wake_slot != my_slot) {
                    raw_futex_wake(&harness.park_futex[wake_slot]);
                }
                harness_spin_unlock();
                raw_futex_park(&harness.park_futex[my_slot]);
                continue;
            }

            if (step->target_tid_slot == my_slot) {
                if (step->expected_tag != 0 && step->expected_tag != tag_id) {
                    /* Intermediate schedule gate on current turn's thread: allow progress */
                    harness_spin_unlock();
                    return;
                }
                bool cancel_now = step->inject_cancel;
                mix_trace(my_slot, tag_id);
                harness.current_step++;
                consumed = true;

                if (cancel_now) {
                    harness_spin_unlock();
                    pthread_cancel(pthread_self());
                    pthread_testcancel();
                    return;
                }

                if (harness.current_step < harness.trace_len) {
                    uint32_t nxt = harness.trace[harness.current_step].target_tid_slot;
                    if (nxt != my_slot) {
                        __atomic_store_n(&harness.park_futex[my_slot], 0, __ATOMIC_RELAXED);
                        if (nxt < AGENTOS_MAX_THREADS) {
                            raw_futex_wake(&harness.park_futex[nxt]);
                        }
                        harness_spin_unlock();
                        raw_futex_park(&harness.park_futex[my_slot]);
                        continue;
                    }
                } else {
                    for (int i = 0; i < AGENTOS_MAX_THREADS; ++i) {
                        if ((uint32_t)i != my_slot) raw_futex_wake(&harness.park_futex[i]);
                    }
                }
                harness_spin_unlock();
                return;
            }

            /* Not my turn yet: wake the active step's thread and park */
            uint32_t wake_slot = step->target_tid_slot;
            __atomic_store_n(&harness.park_futex[my_slot], 0, __ATOMIC_RELAXED);
            if (wake_slot < AGENTOS_MAX_THREADS && wake_slot != my_slot) {
                raw_futex_wake(&harness.park_futex[wake_slot]);
            }
            harness_spin_unlock();
            raw_futex_park(&harness.park_futex[my_slot]);
        }
    } else if (harness.mode == AGENTOS_SCHED_PCT_PRNG) {
        harness_spin_lock();
        if (!harness.active) {
            harness_spin_unlock();
            return;
        }
        mix_trace(my_slot, tag_id);
        uint64_t roll = xorshift64(&harness.prng_state) % 100;
        if (roll < 35) {
            harness_spin_unlock();
            sched_yield();
            return;
        }
        harness_spin_unlock();
    }
}
