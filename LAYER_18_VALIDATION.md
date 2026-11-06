# Layer 18: heap isolation acceptance and typed Gen_server

Base: Layer 17 PR #20 at
`70aeb12473f222999e4dc7f90ffee356a3c438ff`.
Branch: `codex/18-isolation-gen-server`. Validation date: November 6, 2026.

## Implemented behavior

`Actor.Gen_server` supplies typed initialization, request/reply, cast and stop
callbacks over existing actor primitives. Initialization is acknowledged only
after the server creates its state. The caller owns the returned server
monitor. Existing supervisors can spawn `run` themselves and wrap that PID.
The key/value state actor now uses this behavior's cast path, retaining its
original socket-tagged reply protocol and resource limits. A standalone
counter example demonstrates synchronous typed calls.

Calls use a dedicated client inbox and monotonically increasing request IDs.
A per-call broker owns the private reply inbox and monitors the server, caller
and deadline sibling. The caller owns both helper monitors and drains both
helpers on return. The broker publishes exactly one result and parks until
retired, preventing its normal exit from overtaking result delivery. Late
server replies address a retired helper and cannot satisfy another call.
Mailbox pressure that prevents result delivery becomes a helper exit instead
of stranding the caller. Unsupported replies attempt a small error envelope.

No runtime C implementation, primitive name/arity, capability allowlist,
allocation route or write fence is changed by this layer. The float comparison
intrinsic avoids a new Stdlib dependency and uses the existing audited path.

## Executable heap-isolation claim

The acceptance command is:

```sh
sh tools/ci/actions/actor-isolation.sh
ACTOR_ISOLATION_RUNTIME=d sh tools/ci/actions/actor-isolation.sh
ACTOR_ISOLATION_RUNTIME=i sh tools/ci/actions/actor-isolation.sh
```

It groups six fixtures: explicit arena ownership/corrupt-edge checks,
independent copying collection, frozen-global mutation fences, wire validation,
the new multi-actor acceptance scenario and generated graph/resource tests.

The new scenario gives actors different initial/maximum heaps and uses a
37-reduction scheduling budget. Cyclic/aliased mutable graphs are copied to
both actors; mutations remain local. Repeated allocation exceeds each maximum
heap many times while live graphs remain intact, requiring successful private
collection and measured heap growth. A separate actor exhausts its 512-word
maximum and reports `Heap_limit`; peers continue. Twenty retire/recreate
cycles check stale identities and surviving graphs. All messages, monitors
and timers are drained. Existing scheduler-boundary verification checks heap
edges and rejects shared-allocation bypasses on every actor switch.

These are runtime ownership and mutable-heap isolation guarantees within the
supported actor subset. Immutable frozen data and code can be shared. Actors
share one address space; arbitrary C corruption is not contained. This is
strong executable evidence, not a machine-checked proof of the C runtime.

## Deliberate regression detection

```sh
python3 tools/ci/actions/actor-isolation-mutations.py \
  --report-dir /tmp/actor-isolation-mutations
```

The runner copies a built checkout outside the source tree, explicitly sets
`OCAMLSRCDIR` to that copy, checks an unmodified baseline, then separately:

- accepts a foreign heap edge in the ownership verifier;
- removes the actor-active shared-allocation rejection; and
- accepts a store into a foreign actor's field.

Each mutant must compile and then fail runtime arena assertions; compilation
failure alone does not count. All three were caught. Restoring the source and
rebuilding passed again. The original checkout and its runtime are never
mutated. Temporary builds are removed, and build/test transcripts remain in
the requested report directory. This samples three important guards; it does
not establish completeness against every possible isolation defect.

## Local results

Linux x86-64 Docker on macOS arm64:

- Warning-as-error build with debug and instrumented runtimes passed.
- Complete actor directory: **49 passed, one expected native skip, zero
  failures** on normal, debug, instrumented, ASan and UBSan runtimes.
- Expanded final Gen_server and isolation fixtures passed separately on all
  five variants after the final test additions.
- Gen_server cases cover sequential and eight concurrent clients, mutable
  request/reply copies, delayed replies after timeout, invalid deadlines,
  initialization/handler failure, unsupported reply graphs, orderly terminate
  callbacks, actor/monitor/timer/mailbox quota pressure, caller cancellation,
  dead-server identities and rejection of foreign client inboxes. Return
  boundaries check helper/timer/mailbox cleanup.
- All eleven real TCP service scenarios plus parser fuzz/admission rollback
  passed on all five variants with the migrated state actor.
- The counter compiled and printed
  `counter: 42; orderly shutdown verified`.
- Callback 14 passed/2 skipped; backtrace 23/3; effects 23/1; actor checker 1
  and dumpobj 7 passed. Dependency generation had no tracked changes.
- A macOS arm64 native/bytecode warning-as-error rebuild passed. The native
  guard, including Gen_server client rejection outside an actor world, passed
  with `OCAMLSRCDIR` explicitly set to the relocated validation checkout.
  This does not make native actors supported.
- ASan uses the existing workflow's `detect_leaks=0`. Runtime counters and
  actual OS-descriptor teardown checks are separate service assertions.

The final 60-second normal-runtime service run completed **75,577 connections
and 302,308 commands**, with clean runtime/OS-resource teardown. Its raw report
and source fingerprints are in
[`layer18-soak.json`](testsuite/benchmarks/runtime-actors/layer18-soak.json).
This is a local emulated sample with other validation activity on the host,
not a controlled performance comparison or production capacity measurement.

## Remaining boundaries

Calls reserve two extra actors, five extra monitors and at most one timer.
Deadlines bound reply waiting after helper setup, subject to scheduler delay;
startup initialization has no deadline. Timeout does not cancel a request or
undo committed state changes. Clients are actor-local and sequential; separate
actors can create independent clients for the same server. `terminate` runs
only for processed orderly stop, never as a guaranteed crash-cleanup hook.

The broker cancels its deadline sibling when the caller dies. Unexpected
simultaneous caller/broker death can leave the finite deadline actor alive
until expiry. General nested-supervisor cancellation, links, selective receive,
registration, distribution and hot upgrades remain outside this layer.

GitHub Actions was verified disabled before publication. The workflow now
includes the explicit gate and mutation checks, but no hosted success is
claimed. Independent maintainer review and fresh hosted/platform checks remain
pending. This remains an experimental single-Domain Linux x86-64 bytecode
runtime and an OTP-inspired library, not Erlang/BEAM compatibility.
