# AgentOS M1-P1 v2.0 — Build and Public Tests

Use the **Ubuntu 24.04 Dev Container**; the project Dockerfile installs GCC, `make`, `pkg-config`, json-c/libcurl development packages and Python 3.

```bash
make doctor
make
python3 tests/test_process_manager_unit.py   # PCB checks alone
make test                                   # all 13 public suites
make clean                                  # remove generated objects/binary
```

The untouched starter must **compile**, but PCB tests and the full suite are **expected to fail** until the student TODOs in `src/process_manager.c` and `src/main.c` are completed. A unit-test assertion abort (`SIGABRT`) signals a failed C assertion, often an unimplemented function. The suite stops on its first failure.

`make` builds `build/agentos` with incremental `build/*.o` and `build/*.d` dependencies (`-MMD -MP`). Do not commit generated files. Public tests run sequentially, including 100 repeated shutdown checks, with a 120-second timeout per suite.

`make test` sets `AGENTOS_BACKEND=ollama` but **redirects `OLLAMA_URL` to a local deterministic Ollama-compatible HTTP test fixture**. It does **not** require a live Ollama service, and does not grade exact model wording. Tests use a private temporary runtime directory, not the normal demo's socket.

After implementing the TODOs, for the real Ollama + UI demo run in **one** Ubuntu terminal:

```bash
OLLAMA_MODEL=qwen2.5:0.5b bash demo/run_demo.sh ollama
```

The demo script starts both the C server and the bridge. **Do not start `make run` at the same time**. `make run` is only for standalone server debugging. The demo script accepts only `ollama`, **not `mock`**. See [SETUP.md](SETUP.md) for port forwarding, `/processes`, and host Ollama.
