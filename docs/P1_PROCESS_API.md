# ProcessManager API behavior — student reference

The final C types, return codes, field sizes and function declarations are in **`include/process_manager.h`**. Do not change that header or the supplied dispatcher/control protocol.

- One PCB per logical agent; `agent_id` is an exact case-sensitive UTF-8 label 1–128 bytes, never a Linux PID or connection ID. Automatic registration reuses a live PID, rejects a zombie, and rejects a disabled identity. Only valid complete `infer` frames can register an agent.
- Default caps: 64 live-plus-zombie PCB entries; 256 disabled IDs. `pm_list` and `pm_list_disabled` return *total* count while copying at most the caller's capacity, sorted by increasing PID / insertion order respectively. Snapshots are owned copies.
- Legal transitions are listed in the assignment. Target TERMINATED requires `pm_terminate` to capture a final immutable `exit_status`. WAITING requires an explicit reason; other states require `PM_WAIT_NONE`. On rejection **nothing** changes, including callback event order.
- After termination, a zombie remains in the table and uses a slot. `pm_wait` on a live PCB returns `PM_PENDING` without blocking; on a zombie it collects and reaps once. A separate `pm_reap` operation requires TERMINATED. Never return a dangling pointer. `pm_destroy(NULL)` is harmless.
- Removing/disable must store the identity in a separate bounded registry; it can terminate a live agent but may not free a zombie. Unknown-name removal must also block that name. Ordinary `pm_terminate` must never create a disabled identity. Reactivate only after old PCB collection; allocate a fresh PID. P1's disabled registry resets on full program restart.
- Emit one event for each successful state-changing operation, with increasing C-owned `seq` and copied metadata. Invalid operations produce no event. The event callback is supplied; do not attempt to write UI HTML inside the manager.
- This is a **single-threaded P1 model**. Phase 2 extends the same PCB/PID model with real joinable pthreads, not a second runtime. Do not introduce pthreads to pass P1.
