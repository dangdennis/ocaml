# Typed actor Gen_server

This is a small OTP-inspired server behavior for the experimental Linux
x86-64 bytecode runtime. It uses the public actor primitives; no capability
allowlist, write fence or allocation routing is relaxed.

From a built checkout:

```sh
mkdir -p /tmp/actor-counter
cp examples/actor-gen-server/counter.ml /tmp/actor-counter/
./runtime/ocamlrun ./ocamlc -I stdlib \
  -o /tmp/actor-counter/counter.byte /tmp/actor-counter/counter.ml
./runtime/ocamlrun /tmp/actor-counter/counter.byte
```

Expected: `counter: 42; orderly shutdown verified`.

## Behavior

`Actor.Gen_server` accepts a typed callback record with `init`, `handle_call`,
`handle_cast` and `terminate`. Initialization runs in the new server's heap.
`start` waits for initialization acknowledgement and returns the server and a
caller-owned monitor. That monitor must eventually be consumed with
`Actor.await_exit`. Initialization has no timeout: do not block indefinitely
in `init`. Use `run` inside an externally supervised actor when the supervisor
must own the monitor; `of_pid` wraps its typed PID.

A client owns a dedicated typed inbox, created by `Gen_server.client inbox`.
It supports one call at a time. Pass a server handle to other actors to make
independent clients; do not share a client or put application messages into
its private inbox. The compiler checks request, reply and cast types.
All captures and messages still obey actor graph restrictions.

`cast` and `stop` acknowledge enqueueing, not execution. The server processes
messages in FIFO order and owns its evolving state. `stop` invokes `terminate`
with the last state. Crashes, cancellation and heap exhaustion do not invoke
`terminate`; it is not a resource-cleanup guarantee.

`call client server request ~timeout` waits for a copied reply, server failure,
or a finite deadline from zero to 86,400 seconds. Timer delivery can run late.
The timeout begins when the deadline actor registers its timer, after helper
setup; it is not a hard wall-clock bound on setup or computation. Ready exits
take priority over a simultaneously ready reply. A timeout does not withdraw
the request or undo its side effects. The dedicated reply helper is retired
before return, so a late reply cannot satisfy a later call.

Each call reserves two helper actors, two caller-owned monitors, three
broker-owned monitors and at most one timer. Configure world quotas for these
charges plus the server and its persistent monitor. Normal/error return drains
both helpers. A broker monitors the caller and cancels the deadline sibling
when the caller dies. Unexpected simultaneous caller/broker death can leave
the deadline actor until its finite deadline expires; general nested
supervisor cancellation remains separate work.

Failure results distinguish spawn/monitor/send failure, server/deadline/helper
exit and timeout. If a reply graph cannot be sent, the server tries a small
error reply; if quota also prevents that, the call reaches its deadline or
observes a helper failure. The state transition is not rolled back.

## Tests

```sh
sh tools/ci/actions/actor-isolation.sh
python3 tools/ci/actions/actor-isolation-mutations.py \
  --report-dir /tmp/actor-isolation-mutations
make -C testsuite one TEST=tests/runtime-actors/actor_gen_server.ml
sh tools/ci/actions/actor-service.sh --seconds 60
```

The key/value state actor uses the cast behavior and its existing socket-reply
protocol. The counter and focused tests exercise synchronous calls. This is
not an Erlang/BEAM-compatible runtime or a complete OTP implementation.
