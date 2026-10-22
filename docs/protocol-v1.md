
# AgentOS Protocol v1

## Transport
- Unix-domain stream socket
- Default path: `/tmp/agentos-<effective-uid>/agentos.sock`
- Runtime directory: owned by the running user with permissions `0700`
- Optional `AGENTOS_RUNTIME_DIR`: absolute path to an independent private directory (created when absent, rejected when unsafe)
- Lock file: `agentos.lock` within the same directory; prevents a second server using the same runtime directory
- Socket file is created with private permissions; a stale socket left after a crash is recovered under the lock
- Python clients and Mission Control inherit `AGENTOS_RUNTIME_DIR`; the UI-only `AGENTOS_SOCKET` override is available for standalone bridge tests
- Two projects can run simultaneously with distinct `AGENTOS_RUNTIME_DIR` values; they still need different dashboard TCP ports if both run Mission Control
- UTF-8 JSON messages (both raw valid UTF-8 and JSON `\u` escapes are supported)
- Each message ends with a newline (\n)
- Maximum request and response size: 16384 bytes each, excluding newline delimiter
- One request and one response per connection
- Inactivity timeout while receiving a request: 2 seconds
- Absolute elapsed-time request-read deadline: 3 seconds (monotonic clock)
- The deadline covers receiving the request, not AI inference or response sending

## Request
Required fields:
- version: integer, value 1
- request_id: nonempty string
- agent_id: nonempty string
- operation: "infer"
- payload.prompt: string

## Response sizing
- The limit applies to the serialized UTF-8 JSON bytes, not just the raw AI text.
- If the model answer would exceed the framed response limit after JSON escaping, AgentOS sends `RESPONSE_TOO_LARGE` while preserving a valid request ID.
- M1 model adapters also have a separate 4096-byte raw-response buffer; content exceeding that backend buffer currently produces `BACKEND_ERROR`.

## Success response
- version: 1
- request_id: same as request
- status: "ok"
- result.text: string

## Error response
- version: 1
- request_id: request ID or null
- status: "error"
- error.code: machine-readable string
- error.message: human-readable string

## Initial error codes
- BAD_REQUEST
- MESSAGE_TOO_LARGE
- UNSUPPORTED_OPERATION
- BACKEND_ERROR
- RESPONSE_TOO_LARGE (backend succeeded, but JSON-escaped result exceeded response limit)
- REQUEST_TIMEOUT (2 seconds without incoming bytes)
- REQUEST_DEADLINE_EXCEEDED (3 seconds total receiving time)

## Important rules
- A connection carries exactly one request.
- A response must end with a newline.
- The server must handle partial reads and writes.
- A malformed request must not crash the server.
- Agent IDs are labels, not authenticated identities in M1.
- Concurrency will be introduced in M2.

