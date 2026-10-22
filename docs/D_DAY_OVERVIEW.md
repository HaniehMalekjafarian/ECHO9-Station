# M1-P1 v2.0 — Public D-Day and Individual Oral Defense Overview

Follow [the assignment](M1_Assignment.md) for grading, submission, AI use, and deadlines.

D-Day assesses both student-owned modules: `src/main.c` (four Unix-socket server TODOs) and `src/process_manager.c` (PCB registration, transitions, zombies, wait/reap, identity removal/reactivation). You are not required to rewrite the supplied dispatcher, protocol parser, control plane, or ECHO-9 UI.

## Topics to prepare

- **IPC correctness:** newline framing, 16,384-byte maximum, partial/interrupted send, fragmented reads, deadlines, malformed data, disconnects, and shutdown.
- **Process modeling:** logical PID versus Linux PID versus `agent_id` versus request ID; stable identity and snapshots; legal state transitions and rejection without side effects.
- **Lifecycle:** TERMINATED zombie persistence, immutable exit status, nonblocking `pm_wait` with PENDING for live agents, exactly one successful reap and safe memory cleanup.
- **Dynamic agents:** valid-request automatic registration, disabled identities, removing known/unknown names, reactivation with a fresh PID, and in-memory restart behavior.
- **Integration:** real ProcessManager events, native `/processes` view, sequential inference lifecycle, and why no student-managed pthreads are used in P1.

Every team member should be able to explain their code's invariants, pointer ownership, errors, and design decisions and demonstrate a normal lifecycle and a failure case. AI-assisted code must also be understood and disclosed. D-Day may include specification-compliant cases not covered by public tests; You are responsible for making sure your systems works correctly and reliably.

