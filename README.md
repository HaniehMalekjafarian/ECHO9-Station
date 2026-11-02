# ECHO-9 Station — AgentOS

**Operating Systems Project | Ferdowsi University of Mashhad**

### Welcome to ECHO-9

ECHO-9 is an orbital station operated by specialized AI agents. Some monitor crew health, others maintain the reactor, conduct research, or navigate through space.

These agents share a local AI core, but they need reliable infrastructure to communicate, manage their execution states, coordinate work, and recover from failures.

That infrastructure is **AgentOS**.

Your mission is to design and progressively develop it.

AgentOS is a user-space systems project written primarily in C and developed on Linux. You are not implementing a new kernel. Instead, you will build real systems-programming components and models of operating-system mechanisms, connecting them to a working AI-powered station.

### The five phases

AgentOS is developed across five cumulative phases. Each phase introduces new operating-system mechanisms and extends the same ECHO-9 infrastructure.

| Phase | Main topic | What you will build |
|---|---|---|
| **P1 — First Contact** | Processes and IPC | Complete the existing ProcessManager and Unix-socket server, implementing request communication, logical PCBs, and process lifecycle management. |
| **P2 — Concurrent Operations** | Threads and synchronization | Introduce safe parallel agents using threads, mutexes, semaphores, bounded buffers, readers-writers mechanisms, and condition variables. |
| **P3 — Mission Scheduling** | Scheduling and multicore policies | Implement six scheduling algorithms, cooperative preemption, scheduling metrics, deadline handling, and logical multicore comparisons. |
| **P4 — Resource Crisis** | Deadlocks and resource allocation | Implement resource-allocation mechanisms, deadlock prevention, Banker's algorithm and graph-based avoidance, deadlock detection, and recovery. |
| **P5 — Memory and Recovery** | Memory and recovery | Implement memory allocation, segmentation, paging, TLB mechanisms, page replacement, and persistence. |

**One station. One evolving system.**

Each phase extends the work of previous phases. Students are expected to preserve earlier functionality while integrating new mechanisms into AgentOS.

Detailed requirements, implementation boundaries, and evaluation criteria are provided separately for each phase.

### How the station works

Requests can originate from the supplied browser interfaces or directly from Python clients.

The browser interfaces communicate through a Python bridge, while direct clients use a Unix-domain socket. Both ultimately reach the C AgentOS server.

AgentOS validates requests, dispatches work, manages logical process information, and communicates with the local Ollama inference backend.

The responses and lifecycle events become visible through the supplied interfaces:

- **Mission Control:** Submit requests and observe responses.
- **Process Monitor:** Inspect logical PCBs and lifecycle events.
- **Protocol Workbench:** Experiment with valid, malformed, and incomplete requests.
- **Live Station View:** Observe agent activity in the ECHO-9 environment.

These interfaces are supplied infrastructure. Your task is to make the underlying systems components work correctly.

### Phase 1 — First Contact

Phase 1 focuses on two student-owned files:

- `src/main.c` — implement M1.1–M1.4, the sequential Unix-socket server.
- `src/process_manager.c` — implement P1.5–P1.9, the logical ProcessManager.

All other provided C modules, headers, public tests, and UI infrastructure are supplied and should remain unchanged.

Do not introduce threads in Phase 1. Concurrency is introduced in Phase 2.

**Official Phase 1 handout:** [P1 — First Contact](handouts/P1_First_Contact.pdf).

Read the handout before implementing anything. It contains the full requirements, setup instructions, assessment rules, AI policy, and submission requirements.

### Important technical contracts

The authoritative ProcessManager C interface is `include/process_manager.h`.

Logical AgentOS PIDs are not Linux process IDs. Agent identities are case-sensitive UTF-8 labels of up to 128 bytes. Default capacities are 64 active-or-zombie PCBs and 256 disabled identities.

Allowed state transitions are:

| Current state | Allowed next states |
|---|---|
| NEW | READY, TERMINATED |
| READY | RUNNING, TERMINATED |
| RUNNING | READY, WAITING, TERMINATED |
| WAITING | READY, TERMINATED |
| TERMINATED | Reap and remove the PCB |

Invalid operations must not modify state or generate events. Terminated PCBs remain as zombies until collected. A live `pm_wait` is nonblocking and returns `PM_PENDING`. A terminated PCB can be successfully reaped only once.

The Unix-socket protocol uses newline-delimited UTF-8 JSON, with one request and one response per connection. Requests and responses are limited to 16,384 serialized bytes each, excluding the newline. Request receiving has a two-second inactivity timeout and a three-second total deadline.

Inference requests use protocol version `1`, `request_id`, `agent_id`, `operation` set to `infer`, and `payload.prompt`. The supplied parser defines the JSON response format and protocol error responses.

### Development environment

The project uses an Ubuntu 24.04 Dev Container for consistent compilation and testing.

Required tools include Git, Docker, Visual Studio Code with Dev Containers, and Ollama running on the host computer.

The local model for Phase 1 is `qwen2.5:0.5b`.

Inside the Ubuntu Dev Container, start with:

`make doctor`

`make clean && make`

Then run the tests:

`python3 tests/test_process_manager_unit.py`

`make test`

The untouched starter is intentionally incomplete. Compilation should succeed, but functional tests are expected to fail until the student TODOs are implemented.

The test suite uses a deterministic local backend fixture and does not require live Ollama inference.

Start the live station with:

`OLLAMA_MODEL=qwen2.5:0.5b bash demo/run_demo.sh ollama`

Forward port `8765` from the Dev Container. Do not start a second server with `make run` while the demo script is running.

### Working with GitHub

Each team develops the project in its own private repository. The original ECHO-9 repository serves as the upstream source for future phases.

Use meaningful commits and feature branches, review changes through pull requests, and keep the project's evolution visible in Git history.

The supplied GitHub Actions workflow automatically builds the project and runs public tests. In the initial student starter, some test jobs are expected to fail.

### Submission and academic integrity

Follow the official Phase 1 PDF for the detailed requirements.

Each team must include its final source code, one PDF report named `P1_StudentID1_StudentID2.pdf`, and one `AI_USE.md` declaration in its private repository. The repository URL, final tag, and exact commit SHA must also be submitted.

AI tools may support learning and conceptual understanding, but may not implement the student-owned code or perform the prohibited work identified in the official assignment. All relevant AI interactions must be disclosed.

Every team member must be prepared to explain their implementation and engineering decisions during the individual oral defense.

---

**ECHO-9 is waiting for its operating system.**
