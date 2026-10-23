# AgentOS / ECHO-9 — Phase 1 Setup and Troubleshooting

This guide is for the **expanded Phase 1**: sequential C Unix-domain-socket IPC **plus** a student-implemented logical PCB/ProcessManager. It explains how to run and observe the environment; see [the assignment](M1_Assignment.md) and [ProcessManager API](P1_PROCESS_API.md) for what to implement.

**Important:** The untouched starter is intentionally incomplete. `make` must work; functional checks will fail until you complete both `src/main.c` and `src/process_manager.c`. A logical AgentOS process is not a separate Linux process. **Do not use pthreads in P1.**

## 1. Prerequisites

- macOS or Windows with Docker Desktop (Linux containers), or a compatible Linux Docker setup.
- Visual Studio Code with the **Dev Containers** extension.
- Sufficient disk/memory for Ubuntu 24.04 and the lightweight model.
- For **live model demos only**: host-installed Ollama and `qwen2.5:0.5b`.
- Internet access for the first container build/model download.

**Development environment:** Ubuntu 24.04 through the supplied Dev Container. Initial installation and local Ollama integration were tested on macOS Apple Silicon. The latest section-aware UI and 13-suite reference were checked in Linux; Windows/AMD64 compatibility and GitHub Actions execution require separate verification.

## 2. Open AgentOS

1. Clone the AgentOS repository in a fresh folder. Work only with the supplied project files.
2. In VS Code use **File → Open Folder** and select the directory containing `Makefile`, `Dockerfile`, `.devcontainer/`, `src/`, and `tests/`.
3. Run **Dev Containers: Reopen in Container** from the command palette.
4. In the Ubuntu terminal:

```bash
cd /workspace
make doctor
make clean && make
```

A successful build of the untouched starter means the toolchain is installed; **it does not mean M1 is implemented**.

## 3. Run public tests without Ollama

```bash
python3 tests/test_process_manager_unit.py   # PCB module alone
make test                                   # 13 public suites, stops on first failure
```

- The PCB unit test in the original starter fails at the intentionally missing `pm_auto_register` logic. An assertion abort / `SIGABRT` from that test is an expected unfinished-work signal, not evidence of a broken Dev Container.
- A completed implementation should pass **all 13 suites**, including build dependencies, PCB unit/integration, transport, deadlines, shutdown, runtime isolation, UI security and ECHO-9.
- The test harness runs its own **temporary local Ollama-compatible HTTP fixture**. It does **not** need your host Ollama or grade exact LLM wording. Tests use a private runtime directory to avoid disturbing a live demo.
- For extra protocol-test tracebacks use `AGENTOS_TEST_TRACEBACK=1 python3 tests/test_m1.py`.

## 4. Install/check Ollama on the host for live demos

In a **host terminal** (not Ubuntu in Docker):

```bash
ollama pull qwen2.5:0.5b
ollama list
curl http://localhost:11434/api/tags
```

Run the Ollama application/service if necessary. **Do not launch a second `ollama serve` if port 11434 is already in use.** Do not commit model weights.

From the **Ubuntu Dev Container** confirm host access:

```bash
python3 - <<'PY'
import json
from urllib.request import urlopen
with urlopen('http://host.docker.internal:11434/api/tags', timeout=8) as response:
    models = json.load(response).get('models', [])
print('Container -> Ollama: CONNECTED')
print('Models:', ', '.join(m['name'] for m in models))
PY
```

`host.docker.internal` works with Docker Desktop; native Linux Docker may require a different host route.

## 5. Start live ECHO-9 (after completing the TODOs)

In **one** Ubuntu terminal:

```bash
cd /workspace
OLLAMA_MODEL=qwen2.5:0.5b bash demo/run_demo.sh ollama
```

The script starts **both** the C server and the Python UI bridge. **Do not run `make run` as well**; that starts another server and can conflict with the active runtime. Leave the demo running. Press `Ctrl+C` to stop it.

In VS Code's **Ports** tab forward container port `8765`, then open the forwarded address. The browser provides:

| Route | Purpose |
|---|---|
| `/` | Mission Control; observed bridge requests |
| `/station` | Dynamic ECHO-9 world: add agents, station-zone tasks, zombie transformation on termination and dust dissolve on reaping |
| `/workbench` | Raw newline-delimited inference requests |
| `/processes` | Native C PCB roster/events and local add/terminate/reap/remove/reactivate controls; tasks select a known agent in a matching section |

Keep this local developer UI on loopback; do not publicly expose privileged controls.

## 6. Live inference and ProcessManager

Open **another Ubuntu terminal** while the demo runs:

```bash
cd /workspace
python3 clients/test_agent.py
python3 clients/manage_agent.py list
python3 clients/manage_agent.py events --since 0
```

The inference client uses `medical-agent`. Its first valid inference automatically creates a logical PCB. Later requests reuse its PID. On completion its state normally becomes `WAITING / AWAITING_WORK`. Short-lived transitions may appear only in the **native event history** rather than in the roster between polls.

Management is easiest in `/processes`; the local CLI also supports:

```bash
python3 clients/manage_agent.py add --agent-id engineering-demo --role ENGINEERING
python3 clients/manage_agent.py list
python3 clients/manage_agent.py remove --agent-id engineering-demo
python3 clients/manage_agent.py reactivate --agent-id engineering-demo --role ENGINEERING
```

`terminate`, `wait`, and `reap` use **`--pid`** rather than `--agent-id` (obtain the PID from `list`). A zombie is retained after termination until reaping. Explicit removal also disables the identity; **an unknown removed name is disabled without ever creating a PCB**. Normal termination does not disable identity. Reactivation requires the old zombie to be reaped and assigns a fresh PID. A complete restart resets the in-memory P1 registry; durable recovery is introduced in P5.

## 7. Troubleshooting

| Symptom | What to check |
|---|---|
| Compiler or libraries missing | Ensure VS Code opened the Ubuntu Dev Container; rerun `make doctor`. |
| Fresh starter fails PCB unit test | Expected until `src/process_manager.c` is implemented. |
| Unix socket unavailable | Complete `src/main.c` and start the demo script, not a second server. |
| Ollama host unreachable | Check Ollama on the **host**; check Docker Desktop networking. |
| Browser unavailable | Check VS Code Ports and use its actual forwarded address. |
| UI states never visibly change | Read native event history; inference may finish faster than polling. |
| New unknown `agent_id` creates PCB | Expected in the raw protocol and Workbench; the managed Station and Process Monitor require registration before assigning a task. |
| Removed identity gives `AGENT_DISABLED` | Expected until explicit reactivation. |
| Port `8765` busy | Stop another demo or configure a distinct `AGENTOS_UI_PORT`. |
| Runtime socket conflict | Stop the other server or set a new absolute `AGENTOS_RUNTIME_DIR`; don't unlink a live server's socket. |

## 8. Submission and D-Day

Read [Assignment](M1_Assignment.md), [ProcessManager API](P1_PROCESS_API.md) and [D-Day Overview](D_DAY_OVERVIEW.md). Submit **both** student-owned C files, your repository URL/commit/tag, `README_SUBMISSION.md`, `AI_USE.md`, and student-authored tests as described in the assignment. Do not commit private evaluation materials or code you did not author.

## Role-aware station UI

Use **Add agent** to register a named MEDICAL, ENGINEERING, RESEARCH or NAVIGATION agent. MAINTENANCE agents created through the protocol appear in the Engineering bay. The web task form first selects a station section, then a **specific active agent** from that section. Unknown, terminated, disabled and wrong-role agents cannot receive section tasks through the UI. Other roles (such as GENERAL) can be inspected, but cannot receive station-section tasks until registered with an appropriate supported role.

The station derives characters from the real ProcessManager roster. Creation produces a short materialization effect; termination produces a visible zombie who stays until reaped; reaping triggers a particle dissolve. Agents can be manually reactivated under a new PID. Quick inference can finish before the next screen refresh; request journeys are clearly labeled visual replays, **not simulated kernel preemption**.

The `/api/request` legacy demonstration route, Workbench, direct JSON Unix-socket clients and test fixture intentionally retain the underlying v1 protocol auto-registration behavior. The HTTP section/role guard is a P1 usability check rather than an atomic security boundary if a process is concurrently removed between checking the roster and forwarding a request.
