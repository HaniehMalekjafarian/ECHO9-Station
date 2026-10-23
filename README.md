# AgentOS / ECHO-9 — Phase 1: Processes and IPC

**Phase 1: Processes, ProcessManager/PCBs, and Unix-socket IPC** · **One-week milestone** · **No student-managed threads**.

Start with [the full assignment](docs/M1_Assignment.md), the [fixed ProcessManager API](docs/P1_PROCESS_API.md), and the [setup guide](docs/SETUP.md). The [public D-Day overview](docs/D_DAY_OVERVIEW.md) explains the assessment format without revealing private tests.

## Your implementation

- `src/main.c`: complete **M1.1–M1.4**, the four sequential Unix-socket server TODOs.
- `src/process_manager.c`: implement **P1.5–P1.9**, including PCB registration, lifecycle transitions, zombies, wait/reap, disabled identities and reactivation.

All other provided C modules and headers are supplied project infrastructure and must not be modified. The logical AgentOS PID is **not** a Linux kernel PID; real agent `pthread` execution begins in Phase 2.

## First build and testing (Ubuntu Dev Container)

```bash
cd /workspace
make doctor
make clean && make                         # starter must compile
python3 tests/test_process_manager_unit.py # EXPECTED TO FAIL until PCB TODOs are done
make test                                  # 13 public suites; EXPECTED TO FAIL until both modules are implemented
```

A failing PCB assertion in the untouched starter is intentional, not a broken installation. You can test the ProcessManager independently while the socket TODOs are still incomplete. The public test harness uses a deterministic local Ollama-compatible HTTP fixture, so you **do not** need real Ollama for `make test`.

## Live ECHO-9 demo (after implementation)

Run Ollama with `qwen2.5:0.5b` on your **host**. In **one** Ubuntu terminal:

```bash
cd /workspace
OLLAMA_MODEL=qwen2.5:0.5b bash demo/run_demo.sh ollama
```

This script **starts the C server and the Python UI bridge itself**. **Do not run `make run` at the same time.** Forward port `8765` in VS Code. The browser routes are `/` (Mission Control), `/station` (dynamic ECHO-9 world), `/workbench` (raw protocol), and `/processes` (native PCB roster, events, add/terminate/reap/remove/reactivate). The station and Process Monitor accept section-matched tasks **only for already registered active agents**; the raw protocol continues to demonstrate automatic registration.

In another Ubuntu terminal, `python3 clients/test_agent.py` exercises a real inference and `python3 clients/manage_agent.py list` displays the native PCB registry. See [setup](docs/SETUP.md) for details and troubleshooting.

## Submission and safety

Follow [the assignment](docs/M1_Assignment.md). Submit your team repository URL, exact SHA, final tag, `README_SUBMISSION.md`, `AI_USE.md`, and required student source. Do **not** add `build/`, model weights, private test cases, or instructor solutions. AI assistance is allowed but must be disclosed and understood for the individual oral defense.
