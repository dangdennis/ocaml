# Heap-isolated actors: MVP contract

This document is the normative contract for the experimental actor runtime.
The implementation is intentionally staged. The `Actor` module is unavailable
through PR 3, and no branch before PR 6 may claim heap isolation.

## Scope

The MVP supports Linux x86-64, bytecode, one OS thread, and one runtime-owned
scheduler. It provides private actor heaps, FIFO mailboxes, reduction-based
preemption, and actor-local failure. It is not a security sandbox.

Native code, multiple Domains, selective receive, links, monitors, timers,
blocking I/O, distribution, arbitrary C stubs, effects, and finalizers are out
of scope. Unsupported operations fail closed; they never silently use shared
runtime state.

## API

The public surface introduced by PR 4 has this shape:

```ocaml
module Actor : sig
  type 'message pid
  type 'message inbox

  type run_error =
    | Unsupported_runtime
    | Root_failed of string
    | Root_heap_exhausted
    | Deadlock

  type spawn_error =
    | Actor_limit
    | Initial_heap_limit
    | Unsupported_capture of string

  type send_error =
    | No_such_actor
    | Message_too_large
    | Unsupported_message of string

  val run : (unit inbox -> unit) -> (unit, run_error) result
  val spawn : ('message inbox -> unit) ->
    ('message pid, spawn_error) result
  val self : 'message inbox -> 'message pid
  val send : 'message pid -> 'message -> (unit, send_error) result
  val receive : 'message inbox -> 'message
  val yield : unit -> unit
end
```

These constructors are part of the PR 0 contract. Adding an error case later
is an API change and requires a contract revision. Error strings are copied to
the host only after they have a pointer-free representation; actor heap values
never escape through an error.

## Execution semantics

- `Actor.run` suspends the host bytecode computation and starts actor 0.
- The host heap is frozen while actor mode runs. Actors may read approved
  frozen values, but may not mutate them.
- Actor 0 receives a copied root closure. `spawn` copies the child closure and
  its supported environment into a new actor heap.
- A captured mutable value is never shared between parent and child.
- Each actor runs until it exits, blocks, yields, fails, or spends its current
  reduction budget. A scheduler switch occurs only at a bytecode instruction
  boundary and never inside a C primitive.
- `send` transactionally encodes a value into a pointer-free envelope.
  `receive` decodes the oldest envelope into the receiver heap. There is no
  selective receive.
- A PID is an immediate actor index plus a generation. A stale PID never names
  a newly created actor; sending to a dead or stale PID returns
  `No_such_actor`.
- An empty mailbox blocks only the receiving actor. If every live actor is
  blocked and no external event source exists, `run` reports deadlock.
- An uncaught exception or contained actor-heap exhaustion kills a non-root
  actor only. Root exit or root failure stops the actor world and retires all
  remaining actor state before the host resumes.

`Unsupported_runtime` covers non-bytecode or unsupported-platform entry.
`Actor_limit` reports PID-table exhaustion, while `Initial_heap_limit` reports
a child whose copied initial graph exceeds its quota. `Message_too_large`
reports an envelope quota violation. These quota checks happen before a child
or envelope becomes visible.

## PR 1 resumable-interpreter boundary

`caml_bytecode_interpreter_slice` is an internal mechanism, not an actor API.
A reduction is one dispatch of a bytecode instruction. A finite budget is
checked immediately before opcode fetch, so a zero budget executes no opcode
and a budget of one executes at most one. Debugger restarts and the internal
work needed to finish an opcode do not consume another reduction.

A reduction stop is observable only after the current opcode is complete.
In particular, a `C_CALL*` opcode returns from its primitive, restores its C
frame, removes its arguments, advances the program counter, and drains pending
runtime actions before a stop. The ordinary interpreter wrapper supplies an
unlimited budget and remains run-to-completion, including for nested callbacks.

While suspended, the active bytecode stack begins with the four-word frame
used by the debugger:

```text
accumulator | next pc | environment | extra arguments
```

The GC scans and updates the two OCaml values in that frame. Each resume
installs a fresh C exception context; no pointer into a returned C invocation
is stored in the state. The initial stack depth and caller trap offset remain
fixed until terminal return. The active trap offset is captured at every stop;
the caller offset is restored for the C interval and the active offset is
reinstalled on resume.

PR 1 deliberately supports only one suspended computation on the Domain's
current stack, resumed synchronously from the same outer C invocation. It does
not yet switch a suspended state away from the current stack, run unrelated
OCaml code between slices, or preserve independent debugger and backtrace
state. Separate actor stacks and complete host-context switching are later
claim gates. A finite slice that reaches a non-entry effect stack is rejected;
effects remain outside the MVP contract.

## Isolation invariants

Debug builds verify these rules at every mandatory checkpoint:

1. Every private heap block has exactly one live actor owner.
2. A private heap edge targets the same actor or approved frozen state.
3. Frozen state never points into an actor heap.
4. Actor allocation never enters the stock shared major heap.
5. Mailboxes contain no raw OCaml `value` pointers.
6. Spawn and send preserve internal graph shape without cross-actor identity.
7. Actor GC scans only that actor's saved registers, stack, C roots, and heap.
8. Destroying one actor cannot invalidate another actor.
9. Stack and continuation pointers refer only to the owning actor.
10. The scheduler switches only when there are no untracked live C roots.
11. PID generations prevent stale-ID reuse.
12. Contained heap exhaustion uses preallocated scheduler failure state.

Code pointers, static atoms, closure interior pointers, and GC forwarding
pointers have explicit handling and are not ordinary heap edges. A test-only
fault injector must be able to place a foreign actor pointer in a heap and the
next explicit verifier call must reject it.

## Value support

The local execution, spawn-copy, and message-wire contracts are distinct.

Supported for local actor allocation:

- immediates and ordinary scanned data blocks;
- refs, arrays, strings, bytes, floats, and double arrays;
- closures and infix closures; and
- ordinary variant and exception values.

Supported by spawn copying:

- the local data forms above; and
- same-image closures, with immutable code pointers shared and supported
  environments copied.

Supported by the message wire format:

- immediates, tuples, records, variants, lists, refs, and arrays;
- strings, bytes, floats, and double arrays; and
- cycles and aliases within the encoded graph.

Rejected in all actor-facing copy paths unless a later contract says otherwise:

- objects, lazy values, forcing states, and effect continuations;
- arbitrary custom or abstract blocks, including boxed fixed-width integers;
- channels, bigarrays, weak values, ephemerons, and finalizable values; and
- closures as messages and all runtime or operating-system resources.

An unsupported send changes neither mailbox. An unsupported spawn publishes no
PID and leaves no live child.

## Primitive policy

Actor mode uses an audited allowlist of bounded C primitives. The audit covers
allocation, writes, blits, atomics, globals, callbacks, channels, Unix,
systhreads, Domains, dynlink, `Marshal`, `Obj`, custom blocks, finalizers, weak
values, ephemerons, statmemprof, and debugger hooks.

Inline bytecode writes and all calls to `caml_modify` or `caml_initialize` are
subject to the same owner and frozen-state checks. Any unclassified primitive
or allocation path raises an actor-mode unsupported-operation error.

## Deterministic test protocol

Actor tests use a fixed seed unless `ACTOR_SEED` supplies a decimal or `0x`
integer. They use the repository-local PRNG in `actor_test_harness.ml`, never a
self-initialized random source.

`ACTOR_TRACE`, when present, is a path to a replay trace. The canonical trace
grammar is:

```text
actor-trace-v1<TAB>seed=<16 lowercase hexadecimal digits><LF>
<step><TAB><actor-or--><TAB><operation><TAB><key>=<value>...<LF>
```

Steps start at zero and are contiguous. Actor IDs are non-negative decimal
integers. Operation names and field keys use letters, digits, `.`, `_`, `:`,
or `-`. Fields are ordered by key. Values use uppercase `%HH` byte escapes for
anything outside ASCII letters, digits, `.`, `_`, `~`, or `-`. Traces contain
decisions and injected faults, but never addresses, timestamps, or unstable
hash iteration order.

A failing randomized test reports its seed, complete canonical trace, first
replay divergence, and an `ACTOR_SEED` plus `ACTOR_TRACE` command.

## Claim gates

- PR 0 specifies this contract and makes the harness itself executable.
- PR 1 proves resumable bytecode and safe reduction stops.
- PR 2 proves disjoint allocation and mandatory owner verification.
- PR 4 proves spawn copying, the freeze boundary, and safe-language closure.
- PR 6 proves independent private collection. Only then is `heap-isolated` an
  accurate implementation claim.
- PR 7 proves failure containment, deterministic cleanup, and stress replay.

## Acceptance cases

Each case becomes an executable test in the PR named below. Keeping future
cases here, rather than as skipped tests, prevents uncompiled tests from
creating a false-green signal.

- PR 1: uninterrupted versus many-stop results, exceptions, and backtraces;
  stop only at opcode boundaries and after primitives have returned.
- PR 2: alternating contexts allocate in disjoint ranges; stock-major
  allocation and an injected foreign edge are rejected.
- PR 3: two CPU-bound actors both progress; stale PIDs never revive.
- PR 4: captured refs diverge after spawn; frozen global mutation and unsafe
  primitives fail closed.
- PR 5: FIFO wakeup, sender/receiver mutation independence, cycle and alias
  preservation, and transactional rejection of unsupported messages.
- PR 6: repeated moving GC in actor A neither scans nor changes actor B; all
  saved roots survive movement; whole-heap exit reclamation is complete.
- PR 7: child exception and quota exhaustion leave peers alive; root failure
  shuts down cleanly; deadlock, fault injection, and seeded replay agree with
  the reference model.

The full upstream bytecode tests remain the compatibility gate in every PR.
