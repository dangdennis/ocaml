# Supervised actor key/value service

A normally compiled OCaml TCP service for the experimental Linux x86-64
bytecode actor runtime. It uses separate protocol, state, connection, listener,
and root-supervisor modules. The service has no package dependencies.

From a built compiler checkout:

```sh
sh examples/actor-kv/build.sh /tmp/actor-kv
./runtime/ocamlrun /tmp/actor-kv/actor-kv.byte --port 7000
```

Connect to `127.0.0.1:7000` and send newline-delimited commands:

```text
SET greeting hello
GET greeting
PING
QUIT
```

The responses are `STORED`, `VALUE hello`, `PONG`, and `BYE`, each followed by
a newline. `DEL key` returns `DELETED` or `NOT_FOUND`; a missing `GET` also
returns `NOT_FOUND`. Invalid commands return `ERROR`. A new key at the map's
entry limit returns `FULL`; updating an existing key remains possible.

Keys contain 1–64 ASCII letters, digits, underscores or hyphens. Values contain
0–512 printable ASCII characters, including spaces. LF and CRLF are accepted;
fragmented and pipelined frames work. Oversized frames close the connection.
EOF discards an incomplete final frame.

Defaults are 16 simultaneous connections, 128 map entries, and a 30-second
absolute connection lifetime. `--connections N` and `--lease SECONDS` change
connection limits. The lease is an absolute lifetime, including active work,
not an idle timeout. Timers can run late. Connections above the admission limit are closed. Actor,
monitor, timer, mailbox and network limits are derived from the connection cap.

The root owns every worker and deadline monitor. Worker failure closes its
connection. Listener failure cancels and drains that listener's workers before
rebinding the same endpoint; the state actor and map survive. Listener restart
has a finite lifetime budget (three restarts by default). State failure ends
the service: this in-memory example does not silently restart with empty data.
The map is not durable across service restarts.

Interrupt the process to stop it. For acceptance testing only, `--test-control`
enables `CRASH`, `CRASH_STATE`, and `SHUTDOWN`; these commands are disabled by
default. The CLI binds loopback only.

Run the real external-client suite and a configurable soak:

```sh
sh tools/ci/actions/actor-service.sh --seconds 60 --report /tmp/actor-service.json
```

Use `--only restart` to reproduce fixed-port recovery. The harness binds port
zero initially and announces the actual endpoint through a host-only test
backend. Restart testing pins subsequent binds to that same port. Test fixtures
also inject accept errors, constrain the send buffer, and check actual OS
file-descriptor counts after each world. The application itself uses only the
public Actor API.

The root uses `Actor.receive_or_exit` for control messages and child exits.
A worker's mailbox carries `(socket, reply)` pairs: `Actor.Net.transfer_with`
atomically delivers `(fresh_socket, Attached)`, and the state actor later sends
copied replies carrying the same opaque token. Ordinary token copying grants
no socket authority; the runtime checks the current owner on every operation.

See [Layer 17 validation](../../LAYER_17_VALIDATION.md) for evidence and remaining
CI/review limits. DNS, TLS, outbound connections, durable storage, native actors
and parallel schedulers remain outside this example.
