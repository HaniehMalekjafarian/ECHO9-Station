#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
MODE="${1:-ollama}"
if [[ "$MODE" != "ollama" ]]; then
  echo "Usage: bash demo/run_demo.sh [ollama]" >&2
  exit 2
fi
# The C server enforces private runtime directory ownership, verifies the
# exclusive lock, and safely recovers a socket left behind by SIGKILL.
# Do not remove any socket here: that could disrupt a live instance.
RUNTIME_DIR="${AGENTOS_RUNTIME_DIR:-/tmp/agentos-$(id -u)}"
printf 'AgentOS runtime directory: %s\n' "$RUNTIME_DIR"
# The instructor demo always points at its own C server.  AGENTOS_SOCKET
# is reserved for standalone bridge tests and may refer to a different server.
unset AGENTOS_SOCKET
export AGENTOS_BACKEND="$MODE"
export OLLAMA_MODEL="${OLLAMA_MODEL:-qwen2.5:0.5b}"
export OLLAMA_URL="${OLLAMA_URL:-http://host.docker.internal:11434/api/generate}"
make
./build/agentos &
SERVER_PID=$!
cleanup() {
  trap - EXIT INT TERM
  kill -TERM "$SERVER_PID" 2>/dev/null || true
  wait "$SERVER_PID" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
sleep .2
if ! kill -0 "$SERVER_PID" 2>/dev/null; then
  echo 'AgentOS failed to start.' >&2
  exit 1
fi
UI_PORT="${AGENTOS_UI_PORT:-8765}"
printf '\nOpen http://localhost:%s in your browser (forward that port in VS Code if needed).\n' "$UI_PORT"
printf 'Backend: %s | Model: %s\n' "$MODE" "$OLLAMA_MODEL"
python3 demo/mission_control.py
