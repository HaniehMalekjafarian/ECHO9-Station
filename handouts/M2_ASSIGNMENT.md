# ECHO-9 Milestone 2 (M2): User-Space Lockdep, Transitive PIP & Transactional Rollback

**Course:** Advanced Operating Systems & Concurrent Systems Engineering\
**Station Architecture:** ECHO-9 Deep Space Research Habitat\
**Milestone:** M2 — Deterministic Concurrency & Resilient Synchronization Runtime\
**Note:** Keep your Phase 1 commits. Phase 2 uses its own build targets and can run independently of the Phase 1 server.

---

## 1. Executive Scenario: The ECHO-9 Reactor Meltdown Crisis

During Cycle 442 aboard the ECHO-9 Deep Space Research Station, simultaneous failures push multiple priority-stratified agents into heavy lock contention:
1. **Unbounded Priority Inversion:** Low-priority `T_TELEMETRY` (prio 10) holds telemetry locks needed by critical `T_LIFE_SUPPORT` (prio 99) while preempted by CPU-bound scrubbers (`T_SCRUBBER`, prio 40).
2. **Circular Deadlocks:** Emergency coolant agents attempt cross-routing (`L_VALVE_A -> L_VALVE_B` vs `L_VALVE_B -> L_VALVE_A`), risking an orbital hang without diagnostics.
3. **Mid-Transaction Cancellation Corrupting Shared State:** Burst `T_AI_DIAG` threads are cancelled mid-update while mutating control rod depths (`rod_depth[0..3]`), holding temporary 16 KB buffers and open IPC pipe FDs.
4. **Heisenbug Schedule Windows:** Rare TOCTOU and lock-ordering bugs never manifest reliably without deterministic scheduling gates and Compiler-RT sanitizers (ASan+LSan and TSan).

You will implement the **AgentOS M2 Concurrency Runtime** cleanly inside the student-owned C modules under `src/`.

---

## 2. File Ownership Partitioning

### Student-Owned TODO Modules (YOU implement these):
- [`src/agentos_lockdep.c`](../src/agentos_lockdep.c): Engine lifecycle, thread registry (`M2.1–M2.2`), $O(V+E)$ Tarjan SCC / Three-Color DFS preemptive cycle detection distinguishing gray back-edges from diamond black cross-edges (`M2.3`), and async-signal-safe `DEADLOCK_DETECTED_PANIC` with `write()` + `backtrace_symbols_fd()` (`M2.4`).
- [`src/agentos_pip.c`](../src/agentos_pip.c): Transitive Priority Inheritance Protocol across multi-hop chains with dynamic waiter queue re-sorting (`M2.5`), and monotonic multi-lock decay across `Held(t)` (`M2.6`).
- [`src/agentos_tx.c`](../src/agentos_tx.c): RAII lexical transaction scope, pre-image byte journaling (`agentos_tx_write`), heap/FD registration (`agentos_tx_alloc`, `agentos_tx_transfer_alloc`, `agentos_tx_register_fd`), and strict **LIFO** unwind where **memory is restored before any locks are unlocked** (`M2.7–M2.9`).
- [`src/agentos_mutex.c`](../src/agentos_mutex.c): Contended mutex fast-path/slow-path, preemptive cycle check prior to sleeping, priority-ordered waiter queues with cancellation cleanup (`waiter_cancel_cleanup`), direct atomic ownership handoff, and Compiler-RT `__tsan_mutex_*` hooks (`M2.10–M2.12`).

### Instructor-Supplied Immutable Infrastructure (Do NOT modify):
- `include/agentos_sync.h`: Authoritative public C contract header.
- `src/harness_sched.c`: Deterministic raw `SYS_futex` schedule coordinator (`no_sanitize("thread")` isolated).
- `src/echo9_crisis_sim.c` & `src/echo9_sim_main.c`: 6-agent Reactor Meltdown crisis workload.
- `tests/test_atv_runner.c`: Public suite covering all 12 Adversarial Test Vectors (`ATV-01` .. `ATV-12`).
- Phase 2 targets in the root `Makefile`: Zero-warning (`-Wall -Wextra -Werror -pedantic -std=c11`) build and sanitizer driver.

---

## 3. Adversarial Test Vectors (ATV-01 .. ATV-12)

| Vector | Pillar | Requirement |
|---|---|---|
| **ATV-01** | C | Self-deadlock relock caught preemptively with exit code `99`. |
| **ATV-02** | C | 4-Hop diamond cross-edges return acyclic (`false`), added back-edge returns cycle (`true`, len 2). |
| **ATV-03** | C | Cyclic dependency triggers panic before PIP traversal loops infinitely. |
| **ATV-04** | C | Priority `99` propagates transitively across 3 hops (`T_L`, `T_ML`, `T_MH`). |
| **ATV-05** | C | Releasing 1 of 2 held locks steps priority down (`90 -> 70 -> 10`) without premature base drop. |
| **ATV-06** | C/D | Cancelling a waiting thread cleans WFG waiter lists and decays holder priority cleanly. |
| **ATV-07** | D | Overlapping writes restore pre-transaction memory in strict LIFO order. |
| **ATV-08** | D | Memory pre-images are restored while locks are still held prior to unlock. |
| **ATV-09** | D | 100 aborted transactions with heap ownership transfer & pipe FD pass ASan+LSan with zero leaks. |
| **ATV-10** | E | Deterministic `AGENTOS_SCHEDULE_POINT` intercepts TOCTOU windows & repeats seeded trace hashes 10x. |
| **ATV-11** | E | Schedule harness does not mask data races under ThreadSanitizer. |
| **ATV-12** | C/D/E/F | Full 4-phase ECHO-9 Reactor Meltdown passes (<5ms critical wake SLA, 50 cancellations, zero races). |

---

## 4. Build and Test Commands

Run these commands from the repository root in the Ubuntu Dev Container. The untouched starter builds. The functional tests and C crisis simulator are expected to fail until the TODOs are implemented.

```bash
make m2          # Build the Phase 2 library, tests, and simulator
make test-m2     # Run ATV-01 through ATV-12
make sim-m2      # Run the standalone four-phase crisis simulator
make asan-m2     # Run the tests with AddressSanitizer and LeakSanitizer
make tsan-m2     # Run the tests with ThreadSanitizer
```

GitHub Actions builds Phase 2 on pushes and pull requests. Select **AgentOS P2** and choose **Run workflow** to run the functional tests, simulator, and sanitizer checks.

For an optional browser demonstration, run `python3 demo/concurrency_visualizer.py` and open the [concurrency visualizer](http://127.0.0.1:8766/concurrency). It illustrates concurrency independently of the C runtime. The standalone station page is a preview. Live agent controls and inference use the Phase 1 bridge once Phase 1 is implemented.

---

## 5. Official 100-Point Grading Rubric

Evaluation runs in an isolated sandbox using immutable instructor harness/test drivers:
- `RUB-C1` (8 pts): Bipartite WFG construction & lock ownership tracking
- `RUB-C2` (12 pts): Preemptive Tarjan SCC cycle detection in $O(V+E)$
- `RUB-C3` (5 pts): Async-signal-safe panic output with `backtrace_symbols_fd()`
- `RUB-C4` (10 pts): Transitive PIP elevation & multi-lock monotonic decay
- `RUB-D1` (7 pts): Lexical cleanup scope & deferred cancellation safety
- `RUB-D2` (10 pts): LIFO pre-image rollback & memory-before-lock ordering
- `RUB-D3` (8 pts): Zero memory & FD leaks across 100 transactions (ASan+LSan)
- `RUB-E1` (10 pts): Schedule point interception & 10x seeded reproducibility
- `RUB-E2` (10 pts): Zero-suppression TSan compliance & race exposure
- `RUB-F1` (8 pts): ECHO-9 crisis deadlock & `<5ms` priority inversion survival
- `RUB-F2` (7 pts): Burst AI cancellation & rod-depth integrity
- `RUB-F3` (5 pts): Dual sanitizer stress pass under ASan+LSan & TSan

---

## 6. Strict Rules & Academic Integrity

1. **Zero compiler warnings:** Submissions failing `-std=c11 -Wall -Wextra -Werror -pedantic` receive `0` points.
2. **No sleep hacks:** Calling `sleep()`, `usleep()`, or `nanosleep()` in student modules is banned.
3. **No sanitizer suppressions:** Using `TSAN_OPTIONS="suppressions=..."` or `__attribute__((no_sanitize("thread")))` in student files causes automatic failure of Pillar E.
4. **No allocator inside panic handler:** Never call `malloc()`, `printf()`, or `backtrace_symbols()` inside `agentos_panic_deadlock()`.
