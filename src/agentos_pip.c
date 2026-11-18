#include "agentos_sync.h"

/* M2-STUDENT: YOU implement this module (Pillar C: Transitive Priority Inheritance).
 * The placeholders compile cleanly under -Wall -Wextra -Werror -std=c11;
 * they intentionally DO NOT propagate priorities or recompute multi-lock decay.
 */

int32_t agentos_thread_get_effective_prio(const agentos_thread_node_t *node) {
    /* M2.5 TODO: Atomically load node->eff_prio (or AGENTOS_PRIO_MIN if NULL). */
    (void)node;
    return AGENTOS_PRIO_MIN;
}

void agentos_pip_propagate_on_block(agentos_thread_node_t *waiter, agentos_mutex_t *target_lock) {
    /* M2.5 TODO (Called with wfg_lock held):
     * Walk the transitive blocking chain (bounded to AGENTOS_MAX_THREADS hops):
     * - Update cur_lock->max_waiter_prio = max(max_waiter_prio, waiter->eff_prio).
     * - If cur_lock->owner->eff_prio < incoming_prio, boost owner->eff_prio,
     *   increment pip telemetry counters, and re-sort owner in its downstream
     *   mutex waiter queue if cur_lock->owner->waiting_on_lock != NULL. */
    (void)waiter;
    (void)target_lock;
}

void agentos_pip_recompute_on_unlock(agentos_thread_node_t *releaser, agentos_mutex_t *unlocked_mutex) {
    /* M2.6 TODO (Called with wfg_lock held):
     * Never drop directly to base_prio when other contended locks remain held!
     * Compute P_eff = max(base_prio, max_{L in Held(releaser)} L->max_waiter_prio),
     * store atomically in releaser->eff_prio, and propagate decay downstream
     * if releaser is currently waiting on another lock. */
    (void)releaser;
    (void)unlocked_mutex;
}
