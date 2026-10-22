# AgentOS Workbench

Open `http://localhost:8765/workbench` after starting `OLLAMA_MODEL=qwen2.5:0.5b bash demo/run_demo.sh ollama`.

Workbench is supplied and is not graded as student code. It sends the raw text in the editor through the Python local bridge to **the running C Unix socket**, not directly to Ollama.

Experiment with a complete message, malformed JSON, missing fields, unsupported operations, fragmentation and incomplete framing. Inspect raw bytes, complete JSON envelopes, model latency and bridge/transport errors. Workbench uses a maximum input of 20 KB; backend generation may take up to 110 seconds. It observes the service; it is not a fake internal scheduler simulator.

Never bind the dashboard to a public interface or run untrusted probes from other computers.
