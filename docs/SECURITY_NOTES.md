# AgentOS M1 demo UI security (instructor-only)

The Python Mission Control service is a **local development tool**, not a public
multi-user application. By default it binds to `127.0.0.1` inside its runtime.
Use the VS Code Dev Container port-forwarding feature to access it from macOS
or Windows. Avoid publicly shared port-forward links.

`/api/request` requires `Content-Type: application/json`. When an `Origin` header
is present, its authority must match the request `Host`; this blocks simple
cross-site browser requests. This does **not** supply authentication. Requests
made directly on the local machine, including those without an `Origin`, are
still possible.

Do not set `AGENTOS_UI_BIND=0.0.0.0` on a shared network. If the project later
needs LAN, classroom-wide, or cloud access, add authentication and explicit
network access restrictions first. The Unix socket API is likewise intended
for a controlled private development environment.

## C server runtime isolation

The C server now uses `/tmp/agentos-<effective-uid>/` by default, with directory
permissions `0700`, a private `agentos.sock`, and a per-directory `agentos.lock`.
Use `AGENTOS_RUNTIME_DIR=/absolute/private/project-path` to run independent
instances under one Linux user; keep that setting identical for the server,
Python clients, and Mission Control. The final runtime directory component
must not be a symlink, must be owned by the effective user, and must have mode
`0700`. The directory's parent must already exist. The runtime directory and
lock file are intentionally left behind after normal shutdown. The socket is
removed on graceful shutdown; stale socket cleanup occurs under the lock.

The default and custom directory settings provide local-user separation, not
authentication between clients of the same user. Anyone who can access the
Unix socket can send v1 requests. `agent_id` is untrusted user-supplied text.
Never treat this development setup as a secured multi-tenant service.
