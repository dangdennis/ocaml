# Layer 17: supervised reference service

Base: Layer 16 PR #19 at `b1c22b4eea13df7793ecd1508aacd67242c52fab`.
The base passed combined local Linux validation; hosted CI remains disabled.

Build a normally compiled, multi-module line-oriented TCP key/value service.
The state actor alone owns a bounded map. A listener owns its listening socket
and a single pending admission. The root supervisor owns every worker monitor
and every connection deadline; it admits at most a configured number of live
connections. Each connection worker owns its transferred socket.

Application-driven API gaps: typed socket handoff must carry copied metadata,
and the root must atomically wait for ordinary control messages or monitor
exits. Generalize the root inbox type (without an ABI change) and add these two
operations using existing primitive entry points,
with fail-closed monitor validation and transactional handoff tests first.
General network/timer wait sets remain deferred.

Lifecycle policy: a listener failure cancels and drains all workers and their
deadlines before rebinding. The state actor survives listener restart. State
failure terminates the service rather than silently rebuilding an empty map.
Connection failure affects only its connection. Restart attempts have a finite
lifetime budget. Absolute connection leases also bound idle and slow peers.
Shutdown drains owned children before the root returns.

Protocol: PING, SET key value, GET key, DEL key, QUIT. Keys, values, frames,
map entries, connection count, staged bytes and actor/world resources are
bounded. CRASH and SHUTDOWN are disabled by default and opt-in for tests.

Acceptance: real external clients with fragmented/pipelined requests,
concurrent connections, worker/listener failures, state persistence across
listener restart, quota pressure, partial disconnects, timeouts and shutdown.
Check runtime gauges and actual OS descriptors. Exercise deterministic parser
fuzzing and existing wire/resource adversarial fixtures, pinned package canary,
sustained configurable soak and raw performance measurements. Run the complete
actor suites, affected regressions and sanitizers after source is frozen.
Record exact evidence and unresolved platform/CI/review boundaries in the draft.
