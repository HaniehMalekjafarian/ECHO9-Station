# ECHO-9 / AgentOS — M1-P1 Expanded v2.0: First Contact and Process Directory

**Duration:** one week. **Contribution to course project grade:** 10%. **Student language:** C / POSIX (Ubuntu 24.04). **No student-managed threads in P1.**

## The story: ECHO-9 needs an operating layer

The ECHO-9 orbital station has multiple specialist agents: Medical monitors oxygen, Navigation studies trajectories, Research investigates anomalies, and Maintenance checks the reactor. All use a shared local AI Core. But they have a single communication entrance, and the station must distinguish which agents actually exist, which are awaiting work, which have terminated, and which must not return without an explicit operator command.

You will build two foundational OS mechanisms: a **real, sequential C Unix-domain-socket server** that carries requests to Ollama, and a **logical PCB/ProcessManager** tracking the identities and states of station agents. One agent can submit many requests while retaining the same logical PID. An agent that terminates remains as a zombie until another operation collects its status. Removing an agent additionally prevents it from automatically returning. Operators can add or reactivate agents and observe real ProcessManager events in ECHO-9.

This is Phase 1 of a five-phase cumulative project. In P2 your ProcessManager is extended to manage actual pthread-backed agents; P3 uses its identities for scheduling; P4 for resource ownership and deadlocks; P5 for memory and blackout recovery. **The logical AgentOS PCB is not the Linux kernel's PCB, and logical RUNNING is not a host OS CPU scheduling claim.**

## Repository orientation

| Given file/folder | What it does | Your responsibility |
|---|---|---|
| `src/main.c` | Real Unix socket server, read deadlines and graceful shutdown | **Complete M1.1–M1.4 TODOs** |
| `include/process_manager.h` | Public PCB types, state/result enums, snapshots and fixed function interface | Read carefully; **do not modify ABI** |
| `src/process_manager.c` | Compiling starter for student-owned PCB registry and state machine | **Implement the functions** |
| `src/protocol.c` and `include/protocol.h` | Strict validated inference protocol v1 | Supplied; do not rewrite |
| `src/request_dispatcher.c` | Associates valid inference with logical PCB before backend call | Supplied |
| `src/agentos_runtime.c` | Supplied lifecycle transitions and native event stream | Supplied |
| `src/control_plane.c` | Trusted local management commands, separate from public `infer` | Supplied |
| `src/backend.c`, `src/ollama_backend.c` | Ollama backend connector | Supplied |
| `demo/` | Mission Control, Station, Workbench and native Process Monitor | Supplied; observe your code |
| `tests/` | Public tests, including PCB unit and live integration | Run; add your own tests |
| `.devcontainer/`, `Dockerfile`, `Makefile` | Linux development environment | Use as given |

The first valid `infer` from an unknown non-disabled `agent_id` automatically registers it. Subsequent requests reuse the existing PID; malformed and unsupported requests never create a PCB. Public v1 messages still allow only `operation: "infer"`. A client-provided `agent_id` is a label, **not authentication**; privileged add/kill/remove commands belong to the separate local control plane.

## Part A — Four original server TODOs (`src/main.c`)

**M1.1 — Listening endpoint.** Create and bind the private Unix stream socket, listen with backlog 8, preserve runtime ownership/lock safety, and leave the supplied control socket intact.

**M1.2 — Reliable response delivery.** Implement full writes under short `send()`, interruption, peer disconnect, and error. Return correct success/failure; never assume one `send()` suffices.

**M1.3 — One-client request lifecycle.** Read exactly one newline-delimited UTF-8 JSON frame (at most 16384 bytes); preserve timeout, incomplete, overlong and invalid input errors; call the supplied protocol handler and deliver exactly one complete response.

**M1.4 — Sequential accept loop.** Accept the next inference client, configure it, serve one request, and close it. Preserve existing signal shutdown and the *supplied* polling of the private control listener.

## Part B — Student ProcessManager (`src/process_manager.c`)

**P1.5 — Manager ownership:** initialize/destroy dynamically allocated storage and callback. Default bounds: 64 registered PCBs (including zombies), 256 disabled identities.

**P1.6 — PCB registration/lookup:** positive monotonic logical PID beginning at 1, never reused within one server instance; exact case-sensitive UTF-8 `agent_id` of 1–128 bytes; copied role/identity; duplicate detection; automatic registration reuse; lookup by PID/ID; ordered, caller-owned snapshots; capacity errors.

**P1.7 — State machine:** enforce the allowed transitions below; correct WAITING reason; rejected transition leaves PCB and event sequence unchanged.

**P1.8 — Termination and reaping:** immutable exit status, TERMINATED zombie retained, `pm_wait` returns promptly with PENDING for living agents, completed wait/reap frees the PCB once, stale PID/double reap fails. No use-after-free.

**P1.9 — Dynamic population:** Add; Remove/Disable; Reactivate with a fresh PID after reaping; ordinary termination *does not* blacklist the name; explicit removal *does*. Unknown-name removal also blocks future registration. No persistent registry across a server restart (durability belongs in P5).

| State | Allowed next states |
|---|---|
| absent | NEW on creation |
| NEW | READY, TERMINATED |
| READY | RUNNING, TERMINATED |
| RUNNING | READY, WAITING, TERMINATED |
| WAITING | READY, TERMINATED |
| TERMINATED | one successful reap (PCB disappears; no sixth state) |

The authoritative C names and statuses appear in `include/process_manager.h`. Use internal data structures of your choice, but never retain caller-owned pointers as names or expose mutable PCB pointers. The supplied driver calls these functions to make the lifecycle visible in the UI. Do **not** implement pthreads, kernel `fork/exec`, preemptive scheduling, or Linux process ownership in this phase.

## Setup, tests and station demonstration

Run `make doctor` and `make` inside the Ubuntu development container. The unimplemented starter **compiles**; `make test` is expected to fail until your work is complete. There are **13 public suites**, with ProcessManager unit tests independent of socket work. Run `python3 tests/test_process_manager_unit.py` to isolate the PCB module, then `make test` for the integrated package. The suite starts its own deterministic Ollama-compatible HTTP fixture; a real LLM is not required for CI.

To demonstrate the actual station, run Ollama `qwen2.5:0.5b` on your host (follow `docs/SETUP.md`), then run **only** `OLLAMA_MODEL=qwen2.5:0.5b bash demo/run_demo.sh ollama` in one Ubuntu terminal. The script starts both the C server and the web bridge; **do not also run `make run`**. Forward port `8765` in VS Code. Open `/` for Mission Control, `/station` for ECHO-9, `/workbench` for raw JSON experiments, and **`/processes` for native PCBs and controls**. Show Add → Terminate (visual zombie) → Reap (dissolve) → Add again; separately demonstrate Remove → Reap → Reactivate, repeated inference using one PID, role-matched task assignment in the station, and an explicitly removed identity blocked until reactivation. New agents must be added through the managed station UI; Workbench can still demonstrate raw-protocol auto-registration.

P1 remains **sequential**: during slow Ollama inference, the C service does not accept/complete another request. Native WAITING/INFERENCE represents its controlled logical lifecycle; it does not mean that the underlying model has been paused or that the station has multiple concurrent C agent threads. Distinguish C events from Python bridge observations.

## Grade and oral defense

P1 contributes **10% of the total project grade**. Inside P1, the proposed rubric is **65% technical checks, 20% individual conceptual/code defense, 15% report/demo**. Technical allocation: M1.1=10, M1.2=10, M1.3=18, M1.4=7, PCB registration/ownership=12, transitions=12, terminate/wait/reap=12, disabled identities/reactivation=10, observed process/dispatcher/event integration=9 (all normalized within the technical component). The dispatcher and event bridge are **supplied**; the last category grades whether *your ProcessManager* behaves correctly in integration, not changes to supplied modules.

Every member will present their own work and answer conceptual and code-level questions: processes versus programs/threads; Linux sockets and descriptors; framing and partial sends; how PID differs from `agent_id` and `request_id`; legal transitions and why invalid ones cannot mutate state; zombie retention, wait/reap and one successful collector; memory ownership; bad inputs; and why unauthenticated inference labels cannot authorize removal. Private D-Day includes unforeseen lifecycle and IPC cases. See the [public D-Day overview](D_DAY_OVERVIEW.md) for the assessment format. We assess understanding, not copied code or exact model wording.

## AI use policy

AI tools are **allowed** for explanations, ideas, debugging, code and tests, but you are accountable for their correctness and for explaining every submitted function. Describe material AI assistance in `AI_USE.md`: where it helped, what you verified, and what you changed. Do not upload credentials, private TA material, examiners or other students' code to AI tools. Never present unverified generated code or another team's work as your own. Oral questions can target any AI-assisted implementation detail.

## Submission (end of week 1)

Submit the **Git repository URL, exact commit SHA and final release tag**, and the required source/report files: `src/main.c`, `src/process_manager.c`, `README_SUBMISSION.md`, `AI_USE.md` and any additional original tests. Document `make test` outcomes honestly, a station demonstration, bugs discovered/fixed, and each team member's contribution. Do not include `build/`, model weights or private instructor files. The course upload portal is announced separately. The public tests are not the full examiner.
