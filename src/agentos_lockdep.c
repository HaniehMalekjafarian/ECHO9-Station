#include "agentos_sync.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* M2-STUDENT: YOU implement this module (Pillar C: Lockdep WFG & Tarjan SCC).
 * The placeholders compile cleanly under -Wall -Wextra -Werror -std=c11;
 * they intentionally DO NOT satisfy the concurrency or cycle-detection contract.
 *
 * Rules:
 * - Do NOT call sleep(), usleep(), or nanosleep().
 * - Do NOT add __attribute__((no_sanitize("thread"))) in student files.
 * - Inside agentos_panic_deadlock(), use ONLY async-signal-safe calls
 *   (e.g., write() + backtrace_symbols_fd()); never malloc() or printf().
 */

static agentos_lockdep_engine_t global_engine;

agentos_lockdep_engine_t *agentos_get_engine(void) {
    return &global_engine;
}

void agentos_wfg_lock(agentos_lockdep_engine_t *eng) {
    /* M2.1 TODO: Acquire engine->wfg_lock cleanly. */
    (void)eng;
}

void agentos_wfg_unlock(agentos_lockdep_engine_t *eng) {
    /* M2.1 TODO: Release engine->wfg_lock cleanly. */
    (void)eng;
}

agentos_err_t agentos_engine_init(agentos_lockdep_engine_t *engine) {
    /* M2.1 TODO: Initialize global/provided engine state, recursive wfg_lock,
     * thread TLS key, and clear adjacency structures. */
    (void)engine;
    return AGENTOS_ERR_INVALID_ARG;
}

agentos_err_t agentos_engine_destroy(agentos_lockdep_engine_t *engine) {
    /* M2.1 TODO: Free all remaining registered thread nodes and destroy wfg_lock. */
    (void)engine;
    return AGENTOS_OK;
}

agentos_err_t agentos_thread_register(const char *name, int32_t base_prio,
                                      agentos_thread_node_t **out_node) {
    /* M2.2 TODO: Allocate an agentos_thread_node_t in a free tid_slot (0..63),
     * initialize atomic base_prio/eff_prio and Tarjan state, store TLS node. */
    (void)name;
    (void)base_prio;
    if (out_node) *out_node = NULL;
    return AGENTOS_ERR_INVALID_ARG;
}

agentos_err_t agentos_thread_unregister(agentos_thread_node_t *node) {
    /* M2.2 TODO: Remove node from engine->threads[], clear TLS if matching, free node. */
    (void)node;
    return AGENTOS_ERR_INVALID_ARG;
}

void agentos_thread_set_current(agentos_thread_node_t *node) {
    /* M2.2 TODO: Bind current OS thread TLS key to node (used by single-thread unit mocks). */
    (void)node;
}

agentos_thread_node_t *agentos_thread_current(void) {
    /* M2.2 TODO: Return TLS-cached thread node or resolve by pthread_self(). */
    return NULL;
}

bool agentos_wfg_detect_cycle_tarjan(agentos_thread_node_t *start_node,
                                     agentos_thread_node_t **cycle_path_out,
                                     size_t *cycle_len_out) {
    /* M2.3 TODO: Implement O(V + E) Tarjan SCC / Three-Color DFS cycle detection
     * over the Bipartite WFG BEFORE blocking:
     * - Handle self-deadlock (cycle_len == 1) explicitly.
     * - Distinguish Gray back-edges (dfs_on_stack == true -> CYCLE) from Black
     *   cross-edges in diamond topologies (dfs_on_stack == false -> ACYCLIC).
     * - Reconstruct exact ordered cycle nodes into cycle_path_out / cycle_len_out. */
    (void)start_node;
    (void)cycle_path_out;
    (void)cycle_len_out;
    return false;
}

_Noreturn void agentos_panic_deadlock(const agentos_thread_node_t *caller,
                                      const agentos_mutex_t *target_lock,
                                      agentos_thread_node_t * const *cycle_nodes,
                                      size_t cycle_len) {
    /* M2.4 TODO: Emit structured DEADLOCK_DETECTED_PANIC report + per-thread
     * backtrace_symbols_fd() to STDERR_FILENO without heap allocation, then _Exit(99). */
    (void)caller;
    (void)target_lock;
    (void)cycle_nodes;
    (void)cycle_len;
    _Exit(1);
}
