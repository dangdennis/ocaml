# Real-program actor MVP contract

This file fixes the compatibility and ownership decisions for the stacked
`actor-real/*` implementation. It is an engineering contract, not a claim
that the current stack already supports the whole surface below. The focused
tests record the boundary that each later layer moves.

## Compatibility target

The acceptance application is a supervised, line-oriented TCP key-value
service. Its modules are introduced incrementally in the runtime actor tests:

- `reference_service_protocol.ml` owns the request algebra and codec;
- a state actor will own the map and serialize `get`/`put` operations;
- a listener actor will own the listening socket;
- one monitored actor will own each accepted connection; and
- the root supervisor will enforce a bounded one-for-one restart policy.

`real_program_contract.ml` is the first executable slice. Layer 8 recorded the
old precise `GETGLOBAL` rejection. Layer 9 now requires a separately compiled
protocol module to execute successfully through the frozen global image.

## Frozen global image

Actor code may read only validated blocks in the exact global graph frozen by
`Actor.run`. Code pointers and immutable runtime metadata are shared. Actor
code may not execute `SETGLOBAL`, mutate any block reachable from the frozen
image, or observe a global registered after the world freezes. Mutable state
must be allocated in an actor heap or copied into a child at spawn.

Layer 9 admits `GETGLOBAL`, `PUSHGETGLOBAL`, `GETGLOBALFIELD`, and
`PUSHGETGLOBALFIELD` only through the validated snapshot. Subsequent field and
closure-environment reads are checked against the same image. Ordinary scanned
immutable graphs, strings, floats, double arrays, and same-image closures are
readable; custom and abstract blocks, weak values, ephemerons, finalizable
values, and other runtime-specific layouts remain unreadable. Inline mutation
opcodes and mutating primitives still fail before changing host state.

Non-immediate values read from the frozen image are not Layer 9 mailbox values.
The wire encoder accepts message graphs owned by the sending actor (plus
immediates and canonical atoms), so trying to send a frozen string or data
graph returns `Unsupported_message` without enqueueing an envelope. A later
compatibility layer may define copying for supported frozen message graphs.

The runtime remains authoritative. The current `ocamlactorcheck` is a
first-pass advisory inventory for compiler-produced bytecode: it lists linked
global reads and writes and `C_CALL*` sites with symbol or primitive names and
arities. Primitive findings remain `unclassified`. It does not analyze
reachability or branch targets, validate frozen value graphs, classify
primitive capabilities, identify all mutation paths, or prove that an
executable is actor-compatible.

## Primitive capabilities and diagnostics

Every linked primitive will be classified as `pure`, `actor_local`,
`scheduler_aware`, or `forbidden`; absence from the generated manifest means
`forbidden`. Rejections name the exact opcode or primitive and arity. Capture,
message, quota, and resource failures use stable operation-specific text until
the structured exit-reason API lands in Layer 12.

`Actor.stats ()` is an actor-world-only, deterministic snapshot. Its lifetime
counters are monotonic and saturate at `max_int`; `mailbox_messages` is a
current gauge, and `runnable_actors` includes the actor taking the snapshot.
Retirement records unread envelopes in `messages_dropped`. Wall-clock and
throughput measurements remain outside actor heaps in the benchmark harness.

## Monitoring, waiting, and failure data

The MVP uses a dedicated scheduler control-event queue for monitor exits. It
does not introduce a general wait-set or selective receive. `await_exit`
blocks only the current actor and composes with ordinary FIFO user messages
without placing an exit value into a typed user inbox.

Exit reasons carry a bounded UTF-8 backtrace string when bytecode backtraces
are available. Frames are not exposed as runtime pointers, and truncation is
explicit in the reason metadata.

## Limits

The current Layer 11 public configuration shape is:

```ocaml
type heap_limits = {
  initial_words : int;
  maximum_words : int;
}

type world_config = {
  root_heap : heap_limits;
  child_heap : heap_limits;
  max_actors : int;
  reductions_per_slice : int;
  max_message_words : int;
  max_mailbox_messages : int;
  max_mailbox_bytes : int;
}

val default_world_config : world_config
val run_with_config : world_config -> (unit inbox -> unit) ->
  (unit, run_error) result
val spawn_with_heap_limits : heap_limits -> ('message inbox -> unit) ->
  ('message pid, spawn_error) result
```

Existing `run` and `spawn` remain convenience wrappers. Supervisor child
specifications will carry explicit mailbox, timer, and resource limits as
those later Layer 11--13 slices land, so restart behavior does not depend on
ambient mutable configuration.

Heap words, mailbox message count, mailbox encoded bytes, actor count, timer
count, and scheduler-owned resource count all have explicit finite limits.
Quota exhaustion is actor-local and observable through monitors.

Layer 11 accounts queued mail globally at the scheduler boundary. An
envelope's canonical byte charge is one root-token word plus the unique graph
words discovered by the wire encoder, so aliases and cycles are charged once.
The message and byte limits are rechecked at prepare and commit. A rejected
send links no envelope, changes no mailbox gauge, and wakes no actor.

## Timers and socket ownership

Timers use scheduler monotonic time and a deterministic fake-clock backend in
tests. Outstanding timer or I/O waits prevent a false deadlock.

The scheduler owns raw descriptors. Actors hold generation-tagged immediate
handles, and actor exit closes every resource it still owns. An accepted
socket starts owned by the listener. `Actor.Net.transfer` atomically changes
ownership and enqueues the socket handle in the target's typed inbox; failure
does neither. Ordinary messages never carry raw descriptors.

The public monitoring and external-event shape is:

```ocaml
type monitor
type backtrace = { text : string; truncated : bool }
type exit_reason =
  | Normal
  | Uncaught_exception of { summary : string; backtrace : backtrace option }
  | Heap_limit
  | Mailbox_limit
  | Cancelled
  | Unsupported_operation of string
  | Runtime_failure of string
type monitor_error = Monitor_missing | Monitor_stale

val monitor : _ pid -> (monitor, monitor_error) result
val await_exit : monitor -> exit_reason

module Timer : sig
  val sleep : float -> unit
end

module Net : sig
  type listener
  type socket
  type error

  val listen : address:string -> port:int -> (listener, error) result
  val accept : listener -> (socket, error) result
  val read : socket -> bytes -> int -> int -> (int, error) result
  val write : socket -> bytes -> int -> int -> (int, error) result
  val transfer : socket -> socket pid -> (unit, error) result
  val close : socket -> unit
end
```

## MVP exclusions

Native actor execution, Domains, systhreads, dynlink, finalizers, weak values,
ephemerons, arbitrary custom blocks, bigarrays, effect continuations, blocking
Unix calls, selective receive, distributed actors, and hot code loading stay
outside this stack unless a later contract test explicitly admits them.
