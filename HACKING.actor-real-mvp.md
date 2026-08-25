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

`real_program_contract.ml` is the first executable slice. On this baseline it
proves that a separately compiled module is rejected precisely at `GETGLOBAL`.
Layer 9 changes that same contract to require successful execution.

## Frozen global image

Actor code may read only validated blocks in the exact global graph frozen by
`Actor.run`. Code pointers and immutable runtime metadata are shared. Actor
code may not execute `SETGLOBAL`, mutate any block reachable from the frozen
image, or observe a global registered after the world freezes. Mutable state
must be allocated in an actor heap or copied into a child at spawn.

The runtime remains authoritative. `ocamlactorcheck` is an advisory preflight
that reports rejected instructions, primitive capabilities, and identifiable
mutable-global risks without running the program.

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

The public shape frozen for Layers 11--13 is:

```ocaml
type limits = {
  initial_heap_words : int;
  max_heap_words : int;
  mailbox_messages : int;
  mailbox_bytes : int;
  timers : int;
  resources : int;
}

type world_config = {
  reductions_per_slice : int;
  max_actors : int;
  child_defaults : limits;
}

val default_limits : limits
val default_world_config : world_config
val run_with : world_config -> (unit inbox -> unit) ->
  (unit, run_error) result
val spawn_with : limits -> ('message inbox -> unit) ->
  ('message pid, spawn_error) result
```

Existing `run` and `spawn` remain convenience wrappers. Supervisor child
specifications carry the same `limits` record, so restart behavior does not
depend on ambient mutable configuration.

Heap words, mailbox message count, mailbox encoded bytes, actor count, timer
count, and scheduler-owned resource count all have explicit finite limits.
Quota exhaustion is actor-local and observable through monitors.

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
