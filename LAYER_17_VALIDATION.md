# Layer 17 validation

Base: Layer 16 PR #19 at `b1c22b4eea13df7793ecd1508aacd67242c52fab`,
including the November 5, 2026 upstream resync. The implementation is on
`codex/17-reference-service` and remains a stacked draft.

## Application and lifecycle contract

`examples/actor-kv` is ordinary separately compiled OCaml code. A state actor
owns the bounded map; a listener owns its socket and pending admission; the
root owns every connection and deadline monitor. Worker failure affects one
connection. Listener failure drains its workers before rebinding the same
endpoint, preserving state. State failure ends the service. Restart attempts
have a finite lifetime budget; absolute connection leases bound idle and slow
peers, subject to scheduler delays.

World limits are derived from the connection cap. With cap N, the service
allows 3+2N actors, 2+2N monitors, N timers, N+2 network resources and N+1 network
operations. Frame, key, value, map, message, mailbox and staged-byte limits are
also finite. Test control commands are disabled by default.

Application-required extensions are typed root inboxes, `receive_or_exit`
(own mailbox plus owned exits) and `Net.transfer_with` (socket plus copied
metadata). Primitive names/arity and the capability allowlist are unchanged.
The service uses audited `Float.compare`. Compiler-inserted
`caml_ensure_stack_capacity` remains forbidden; service functions are kept small
enough to avoid that call on actor paths. This is a compatibility boundary,
not proof that arbitrary OCaml functions or packages work inside actors.

## Reproduced defects and focused gates

- The Layer 16 compiler rejected the new event API with `Unbound value
  Actor.receive_or_exit`; the new typed service therefore requires the small
  runtime extensions, rather than unsafe casts or host callbacks.
- Fixed-port restart failed with errno 98 (`EADDRINUSE`) when old connections
  were drained. `SO_REUSEADDR` before bind fixes the observed failure. The
  acceptance fixture pins replacement binds to the original actual port;
  a separate real test rejects simultaneous live listeners on that endpoint.
- A full global mailbox left a listener blocked after its admission decision
  could not be sent. A deterministic test hit its external watchdog before
  the fix. Assignment and rejection failures now cancel that listener; both
  paths drain all actors, monitors, timers and queued messages in the unit gate.
- `actor_service_events.ml` covers event priority/FIFO, mailbox and exit wakes,
  empty sets, cancellation, duplicate/foreign/consumed monitor rejection,
  metadata isolation under small heaps/GC, mailbox and graph-quota rollback,
  unsupported metadata, stale authority and recipient death before receive.
- `actor_service_fuzz.ml` checks 300 seeded cyclic/aliased message graphs and
  500 randomized resource lifetimes, ownership returns, cancellation, stale
  aliases, invalid ranges and quota recovery. Existing wire/resource failure
  fixtures remain in the full actor suite.
- Parser fuzzing checks 4,000 seeded byte strings across host and actor
  execution, plus value boundaries and disabled control commands.

## Commands and results

Linux amd64 Docker builds use the warning-as-error configuration:

```sh
./configure --disable-native-compiler --disable-native-toplevel \
  --enable-debug-runtime --enable-instrumented-runtime \
  --enable-ocamltest --enable-warn-error
make -j8
make -C testsuite one DIR=tests/runtime-actors
make -C testsuite one DIR=tests/runtime-actors USE_RUNTIME=d
make -C testsuite one DIR=tests/runtime-actors USE_RUNTIME=i
```

Each complete actor suite passed **47 tests, with one expected native skip**.
Separate source builds using the ASan and UBSan flags/environment from
`.github/workflows/actor-runtime.yml` passed the same **47 tests / one skip**.
ASan follows that workflow's `detect_leaks=0`; descriptor checks are separate.

The standalone application gate is:

```sh
sh tools/ci/actions/actor-service.sh --seconds 60 --report /tmp/service.json
ACTOR_SERVICE_RUNTIME=d sh tools/ci/actions/actor-service.sh --seconds 2
ACTOR_SERVICE_RUNTIME=i sh tools/ci/actions/actor-service.sh --seconds 2
```

All eleven external-client scenarios passed on normal, debug, instrumented,
ASan and UBSan runtimes. They cover concurrent clients, fragmented/pipelined
and maximum-size CRLF frames, empty values, full maps, admission limits,
worker/state/listener crashes, bounded restart storms, idle deadlines, partial
EOF, forced write backpressure with another client progressing, and actor,
descriptor, operation, byte and mailbox quota failures. Both admission rollback
unit cases passed on every variant. A failed-startup fixture also checks that
its parent process pipes/control descriptor are released.

Each world checks that only the root remains, with zero monitors, timers,
network resources, operations and staged bytes. The host fixture separately
checks actual `/proc/self/fd` counts after the actor world is destroyed.
Port zero and readiness notification avoid fixed-port races; the restart gate
then requires that same port. Watchdogs bound failures; no sleep determines
request ordering.

Callback (14 passed/2 skipped), backtrace (23/3), effects (23/1), actor checker
(1), dumpobj (7), and both affected expect tests (1 each) passed. The pinned
Astring 0.8.5 canary, actor/timer/network benchmark smoke, dependency generation,
CLI build/help/invalid-configuration checks, and source hygiene passed.
Trace-enabled restart/state-failure runs passed with complete schema v3 output,
zero final network gauges and no addresses, keys, values or control payloads.

A fresh macOS arm64 native/bytecode warning-as-error build and native guard
passed, including `receive_or_exit` rejection outside a world. GNU Make 4.4.1
and `ac_cv_func_pipe2=no ac_cv_func_dup3=no` select existing Unix fallbacks for
SDK declarations newer than the deployment target. No Unix source changes
were made. Mandatory line wrapping preserved canonical parsed OCaml output
and Python AST; final builds and acceptance were rerun after formatting.

## Sustained sample and evidence boundary

The raw 60-second report is
[`testsuite/benchmarks/runtime-actors/layer17-soak.json`](testsuite/benchmarks/runtime-actors/layer17-soak.json).
It records two concurrent external clients repeatedly connecting, pipelining
PING/SET/GET/QUIT and closing, with a four-connection/eight-entry service limit.
Latency measures the entire connection and four-command round trip. RSS is
whole-process memory, including runtime/emulation overhead, not actor heap use.

This is one local Linux amd64 Docker sample on a macOS arm64 host. It is not a
native-hardware comparison, large-heap/concurrency study or multi-hour soak.
GitHub Actions remains disabled, so fresh-runner platform/MSVC checks remain
pending. Source generation and review used OpenAI Codex assistance; this is
not independent maintainer review. The prototype has no general package,
security-sandbox, durable-storage, native/parallel-actor or production support
promise. Those remain separate decisions after review and larger trials.
