# Layer 16: scheduler-owned nonblocking TCP

Execution checkpoint: 2026-09-23. Base: `codex/15-timers` at
`a24deb6ec91edb0b4db10551dbe70f2c4d967d44`, draft PR #18.
The base is clean and its current head passed 20 checks with eight skips.
Layer 15 supplies owned timers, distinct wait reasons, bounded expiry, and
signal-safe host waiting. Layer 16 extends that boundary with socket readiness.

Implementation branch: `codex/16-nonblocking-tcp`. The implementation remains
in progress until the exact published tip passes the gates below. Layer 17
retains the supervised service and its application-level policy.

Scope: Linux x86-64 bytecode; numeric IPv4 TCP servers. Other builds must
continue compiling. DNS, outbound connect, TLS, UDP, files, listener transfer,
per-operation timeouts, native actors, and multiple Domains remain deferred.

The reactor uses level-triggered epoll for bounded readiness batches. Its
pollable descriptor joins the timer layer's signal-safe ppoll wait, preserving
nanosecond deadlines and atomic signal-mask restoration. Pending operations
and completions contain only scalar identities and runtime-owned bytes.

## Public contract

The synchronous-looking operations suspend only the calling actor. The public
interface in `stdlib/actor.mli` defines the error constructors and full contract.

```ocaml
module Net : sig
  type listener
  type socket
  type error

  val listen : address:string -> port:int -> (listener, error) result
  val local_port : listener -> (int, error) result
  val accept : listener -> (socket, error) result
  val read : socket -> bytes -> int -> int -> (int, error) result
  val write : socket -> bytes -> int -> int -> (int, error) result
  val transfer : socket -> socket pid -> (unit, error) result
  val close : socket -> (unit, error) result
  val close_listener : listener -> (unit, error) result
end
```

`local_port` supports binding port zero in integration tests, avoiding fixed-port
races. Separate listener close completes a lifecycle missing from the earlier
API sketch. Returning a close result makes stale and foreign-handle outcomes
testable; this revises the sketch's `close : socket -> unit`.

The implementation follows these semantics:

- `accept` publishes a socket owned by the accepting actor. It suspends on
  would-block and returns recoverable connection/resource errors explicitly.
- Validate context, handle kind, current owner, offset, length, and destination
  ownership before any network side effect. Invalid arguments are distinct
  from stale/foreign handles, quota errors, and operating-system errors.
- `read` returns a short positive count, or zero for EOF on a positive-length
  request. A zero-length request returns zero immediately after validation.
  It never modifies bytes outside the requested actor-owned range.
- `write` returns a short positive count. Callers advance by that count; an
  error must not conceal bytes already accepted by the kernel. A successful
  write does not promise delivery to the remote application.
- A zero-length write returns zero after validation. A positive-length request
  with no progress suspends on would-block. Higher-level write-all and line
  parsing loops belong in OCaml.
- Permit one outstanding operation per actor, with one owner per resource.
  No asynchronous operation queue or concurrent read/write API is needed yet.
- Preserve buffered data before reporting EOF when readiness also reports
  hangup. Handle disconnect/reset as bounded errors; suppress socket SIGPIPE
  per send rather than altering the host's process-wide signal policy.
- Expected invalid/stale/foreign identities return explicit errors without
  touching a descriptor. Malformed runtime values and escaped ownership checks
  use the existing fail-closed containment boundary. Ordinary Unix primitives
  remain forbidden.
- Recoverable quota rejection leaves the actor alive. Actor heap exhaustion
  retains the existing structured exit behavior. Reactor-wide failure must
  terminate the world through a bounded runtime-failure path and cleanup,
  rather than strand all external waiters.

## Resource and suspension model

The scheduler owns raw descriptors, registration state, operation records,
and bounded byte storage. An actor holds only an abstract immediate token.
Every operation validates the token against the live resource table, resource
kind, actor PID generation, and current authority generation.

Tokens must not alias across close/reuse, transfer, actor reuse, or successive
actor worlds. Use checked identity allocation and fail on exhaustion rather
than wrap, including on 32-bit targets. The token is an ownership-checked
identity, not a security capability; copying its integer representation through
ordinary mail does not transfer authority.

Pending C records contain scalar identities, offsets, lengths, status, and
scheduler-owned bytes. They never retain an OCaml `value` or address into an
actor heap. The request and destination buffer remain reachable through the
actor's saved, GC-tracked bytecode state. On resumption, revalidate and recover
the buffer through those roots before copying returned bytes.

Use a bounded staged operation with explicit states:
`prepared -> waiting -> completed -> consumed`, plus cancellation/abort.
Reserve metadata, byte quota, and required result storage before irreversible
I/O. Once the kernel has accepted a write or consumed input, keep its completion
until the resumed call consumes it; replaying the bytecode call must never
repeat that side effect. Actor death may discard a completion and closes the
socket; it cannot roll back bytes already observed by the peer.

Use existing audited primitive request encodings where practical, with explicit
name/arity/shape tests. The four-argument public read/write API must not quietly
admit arbitrary `C_CALLN`. If a new primitive is necessary, include an explicit
bootstrap design and tests; changing the public signature alone is insufficient.

## Reactor behavior and atomic transfer

Use nonblocking, close-on-exec sockets and a bounded ready-event batch. Watch
readability only for pending accept/read work and writability only for pending
write work. Retry would-block after readiness, and bound interrupted syscall
retries. Unregister completed interests to prevent writable-socket busy loops.

Poll readiness at bounded scheduler intervals even while actors remain
runnable, so CPU-heavy actors cannot starve I/O. With no runnable actors, wait
using Layer 15's earliest deadline. Open descriptors alone do not suppress
deadlock: an actual armed operation or timer must be able to wake an actor.

Normalize each injected readiness batch into a documented stable order, using
operation registration sequence as the tie breaker. Follow Layer 15's ordering
for cancellation, due timers, and external events, and test simultaneous cases.
Real network arrival order is external input, not promised deterministic.

Readiness records carry checked resource and registration identities, never
heap addresses or authority derived solely from a raw descriptor number.
Late or duplicate events after close, transfer, cancellation, or descriptor
reuse must be harmless.

`transfer socket destination` is a single prepare/commit transaction:

1. Validate the live destination PID, source ownership, and absence of pending
   operations/completions. Reject transfer to self explicitly.
2. Prepare a fresh authority token and ordinary pointer-free socket inbox
   envelope; reserve the destination's mailbox quota and all commit metadata.
3. Atomically change ownership, invalidate the old token/registration, and
   enqueue the new token; only then wake the destination.
4. Any preparation failure leaves the old owner, token, mailbox, and quotas
   unchanged. Once committed, destination death before receive closes its socket.

Changing authority on every transfer prevents an old alias from becoming valid
again if the same actor later receives the resource back. The target inbox is
`socket inbox`, matching the existing design notes; a richer handoff envelope
is a later extension if the reference service needs one.

On actor exit, cancel pending operations, invalidate registrations, close all
still-owned resources, and release byte/operation charges before the heap is
destroyed or PID generation advances. Preserve exit publication ordering;
observers must not run before cleanup completes. Root exit, cancellation,
deadlock, preparation failure, and reactor failure must also close the entire
world's resources. A restarted child gets no previous child's handles.

## Bounded work and observability

World limits default to: 1,024 owned descriptors, 1,024 pending operations,
8 MiB total staged I/O bytes, and 64 KiB maximum per syscall chunk. Cap reactor
dispatch at 64 events per pass. Every existing default and timer limit is
preserved.

Charge listeners and accepted sockets to the descriptor limit, including
prepared-but-unpublished resources. Account for the reactor's internal
descriptor separately in leak checks. Charge staged buffers and completed
but-unconsumed work until consumed or cancelled. Bound metadata independently
by the descriptor/operation tables, use checked arithmetic, and validate config
before freezing the host world. Bound the listen backlog explicitly.

Expose current/peak resources, pending operations, staged bytes, quota
rejections, and cleanup/error counts using saturating counters. Extend tracing
only with bounded scalar resource/operation IDs, event kinds, and byte counts.
Do not trace payloads, addresses, descriptors, or pointers. Version schema
changes explicitly and update replay fixtures/viewer compatibility; tracing
failure must remain semantically inert. The public trace is not a capture of
network bytes: deterministic I/O replay uses a separate private test fixture.

## Audited implementation slices

| Slice | Deliverable | Required gate before widening scope |
| --- | --- | --- |
| 1. Contract and fake backend | Freeze API, errors, quotas, ownership/token encoding, wait integration, and operation state machine; add failing tests | Would-block, stale/foreign/world tokens, short I/O, cancellation, allocation failure, and duplicate completion are reproducible without sleeps |
| 2. Resource lifecycle | Prepare/commit/abort resources, listener creation/close, owner tables and retirement cleanup | Rollback after each injected allocation/registration failure; zero resources after actor/world teardown; cross-platform stubs compile |
| 3. Reactor and accept | Linux epoll, pending accept, bounded polling, timer integration | No false deadlock, missed wakeup, starvation, duplicate accepted socket, or busy loop; accepted sockets are nonblocking and owned |
| 4. Read/write | Root-safe suspension, staged buffers, short I/O, exact-once completion | Fragmentation, EOF, reset, EAGAIN, EINTR, partial writes, byte quotas, heap pressure, and GC/root restoration pass |
| 5. Transfer and supervision | Atomic inbox handoff, authority renewal, monitored worker ownership | Failed transfer preserves source; recipient death and listener/worker restart close exactly the resources they own |
| 6. Stress and closeout | Stats, trace compatibility, real TCP canary, adversarial reuse and portability | Full local and fresh-runner gates pass on exact published tip; documentation records the remaining Layer 17 boundary |

Each implementation slice should become a narrow reviewed commit after its
focused tests pass. Publication is one stacked draft PR based on the exact
completed Layer 15 branch; recheck the base immediately before opening it.

Likely files within the `ocaml` checkout: `stdlib/actor.ml` and `.mli`, `runtime/actor.c`,
`runtime/actor_scheduler.c` and its header, new `runtime/actor_io.c` and header
for resource/backend code, `runtime/interp.c` only if the existing resume seam
is insufficient, primitive policy/tooling fixtures, build/configure manifests,
`testsuite/tests/runtime-actors`, trace tooling, and the runtime CI workflow.
Final placement follows the completed Layer 15 implementation.

## Completion gates and handoff

Deterministic seam tests cover readiness arriving before/after wait publication,
timer/readiness/cancel ties, EINTR/EAGAIN, short operations, stale event batches,
full quotas, failed allocation at every prepare step, transfer rollback, and
completed operations resumed exactly once. Preserve mailbox FIFO and monitor
delivery throughout. Verify no raw heap pointer survives a switch.

Stress at least 200 generations of actor, resource, descriptor, and transfer
reuse. Cover listener exit, blocked-worker cancellation, child restart, root
failure, cross-world stale handles, destination death before receive, and both
normal and failed world teardown. Assert resource, byte, and operation gauges
return to zero and actual OS descriptor counts return to baseline. ASan alone
does not prove descriptor cleanup.

Run a real loopback TCP echo canary with externally driven clients, fragmented
requests, backpressure, disconnects, and a concurrent CPU actor/timer. Use port
zero and explicit process/pipe handshakes; deadlines are watchdogs, not semantic
sleep assertions. This proves the backend, while the key/value service stays
in Layer 17.

Run normal, debug, instrumented, ASan, and UBSan actor suites; tooling and
primitive-fence tests; the Stdlib corpus and pinned Astring canary; benchmark
smoke; relevant callback/backtrace/effects suites; tracing enabled/disabled;
hygiene and full cross-platform build/MSVC gates. Use Linux amd64 Docker or a
space-free Linux checkout for local runtime tests. Rebuild `stdlib.cma` after
changing Actor's interface, and verify new tests appear in `ocamltest` discovery.

Require fresh-runner results at the exact implementation tip and recheck the
final documentation tip, PR base/head, and remote synchronization. Update the roadmap's published table and Layer 16 section.

The Layer 17 handoff is a tested `Actor.Net` API, operational limits, known
unsupported cases, CI evidence, and a working owned-socket handoff canary.
Explicitly carry forward the root-only supervision limitation: restarting a
listener does not yet imply cascading cancellation of all its connection
workers. Layer 17 must choose and test that service policy.

First executable boundary after Layer 15: a fake-backend test in which accept
would block, another actor and a timer still progress, cancellation removes the
wait, and delayed readiness after resource reuse cannot wake the new owner.

## Backend references

The Linux manual describes level-triggered readiness and stale cached events
in [epoll(7)](https://www.man7.org/linux/man-pages/man7/epoll.7.html).
The initial choice of level-triggered dispatch is this plan's design decision.
[accept(2)](https://man7.org/linux/man-pages/man2/accept.2.html) documents
`accept4` nonblocking/close-on-exec flags, and
[send(2)](https://www.man7.org/linux/man-pages/man2/send.2.html) documents
per-call `MSG_NOSIGNAL`. Check
[close(2)](https://man7.org/linux/man-pages/man2/close.2.html) when implementing
cleanup: do not blindly retry Linux close errors after descriptor release.
