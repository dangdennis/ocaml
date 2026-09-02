# Heap-isolated actors: MVP contract

This document is the normative contract for the experimental actor runtime.
The implementation is intentionally staged. The `Actor` module is unavailable
through PR 4a, and no branch before PR 6 may claim heap isolation.

## Scope

The MVP supports Linux x86-64, bytecode, one OS thread, and one runtime-owned
scheduler. It provides private actor heaps, FIFO mailboxes, reduction-based
preemption, and actor-local failure. It is not a security sandbox.

Native code, multiple Domains, selective receive, links, monitors, timers,
blocking I/O, distribution, arbitrary C stubs, effects, and finalizers are out
of scope. Unsupported operations fail closed; they never silently use shared
runtime state.

## API

The complete MVP public surface has this shape. PR 4b introduces `run`,
`spawn`, `self`, and `yield`; PR 5 adds `send` and `receive` without changing
the types or constructors below:

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

  type stats = {
    live_actors : int;
    runnable_actors : int;
    blocked_actors : int;
    total_spawned : int;
    total_exited : int;
    total_failed : int;
    total_dispatches : int;
    total_reduction_stops : int;
    messages_sent : int;
    messages_received : int;
    messages_dropped : int;
    mailbox_messages : int;
  }

  val run : (unit inbox -> unit) -> (unit, run_error) result
  val spawn : ('message inbox -> unit) ->
    ('message pid, spawn_error) result
  val self : 'message inbox -> 'message pid
  val send : 'message pid -> 'message -> (unit, send_error) result
  val receive : 'message inbox -> 'message
  val yield : unit -> unit
  val stats : unit -> stats
end
```

These constructors are part of the PR 0 contract. Adding an error case later
is an API change and requires a contract revision. Error strings are copied to
the host only after they have a pointer-free representation; actor heap values
never escape through an error.

`stats` is available only inside the actor world. Its lifetime counters
saturate at `max_int`; `runnable_actors` includes the actor taking the
snapshot, and `messages_dropped` counts unread envelopes discarded at actor
retirement.

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

## PR 2 fixed-arena boundary

An active internal actor heap redirects both small and direct-large runtime
allocation into one fixed, downward-growing arena. The payload mapping has an
inaccessible guard page at each end of its page-rounded extent, a separate
logical word quota, and a stable owner identifier; owner zero denotes the
future root actor and is already accepted by the arena layer. Allocation
checks the supported tag and complete block
size before moving the cursor, so unsupported or over-quota requests do not
change arena state. The stock minor pointer, shared major heap,
major-allocation accounting, and memory profiler are not touched by a
successful arena allocation.

Creation and activation fail outside Linux x86-64 bytecode or when more than
one Domain is running. Every allocation also rechecks that the requesting
arena is the exclusively active arena of the current Domain.

The live-arena registry classifies actor ranges before any stock runtime
address test. A fixed out-of-arena shadow ledger records every allocation
boundary and header. The verifier first compares the arena against that ledger
and proves exact block tiling, then scans every allocated block, including
unreachable blocks. An edge may target an immediate, a canonical static atom,
an exact block owned by the same arena, or a validated infix pointer into a
same-owner closure. Stock-young, unregistered host/shared, malformed-interior,
and foreign-owner pointers are rejected. Closure code pointers must be aligned
and name a registered code fragment. No frozen host value is approved before
PR 4a.

`caml_modify` and `caml_initialize` bypass stock barriers only for a checked
same-owner destination and value. A last-resort guard rejects any direct call
to `caml_shared_try_alloc` while an arena is active. Atomic operations, bulk
array operations, raw bytecode writes, arbitrary primitives, urgent-GC paths,
and switching OCaml execution between arenas have not yet been audited. PR 2
therefore proves physical separation and explicit ownership verification only;
it adds neither runnable actors nor independent collection, and it makes no
heap-isolation claim.

## PR 3 deterministic-scheduler boundary

The internal scheduler owns a fixed slot table, one FIFO ready queue, and one
detached bytecode stack and arena per published slot. Slot zero at generation
zero is the root and has PID and arena owner zero. Other PIDs encode a
generation above a 16-bit slot index. Reaping advances the generation before
the slot is reusable, never wraps it, and permanently retires an exhausted
slot. PID lookup checks both parts before accessing actor state.

One dispatch installs exactly one actor stack and arena, disables backtrace
recording and effect trap barriers, and runs a finite interpreter slice. The
slice spills its accumulator, program counter, environment, extra arguments,
and trap offset before the scheduler restores the host stack and C context.
The scheduler refreshes the saved stack pointer after every slice because
stack growth may replace it, verifies the outgoing arena, and appends a
reduction-stopped actor to the ready-queue tail. The PR 3 seam proves an exact
alternating trace for two CPU-bound actors and equal progress without explicit
yielding.

Stock pending actions are never processed with an actor stack or arena
installed. They produce a pointer-free host-action stop and remain pending for
the restored host. All C primitives and effect instructions are default-denied
before entry; the denied-primitive test proves that its C body is not called.
Actor backtraces, debugger use, and multiple Domains remain unsupported.

This is still an internal scheduler over trusted, registered synthetic
bytecode with immediate or static-atom initial state. It deliberately does not
run host closures as actors, scan detached actor roots, contain allocation
exhaustion,
or audit allocating and mutation opcodes. The `Actor` module remains
unavailable. Freeze/copy, a safe primitive and opcode subset, independent
collection, and public lifecycle semantics remain later gates, so PR 3 makes
neither a public-actor nor a heap-isolation claim.

## PR 4a runtime-fence boundary

PR 4a adds an internal, transactional actor-world fence. Freeze moves through
`PREPARING`, `FROZEN`, and `THAWING`; it publishes `PREPARING` before doing
runtime work so nested entry fails as busy. Any failed precondition or pending
action exception rolls back the partially prepared world. Thaw must run on the
same Domain. It checks the exact stock-runtime snapshot, reports corruption if
anything changed, and then clears the fence.

Frozen values are approved by an exact ledger, not by an address-range or
"not young" test. Registration accepts only canonical stock-major block
bases. Each entry records the block header and a snapshot of every payload
word; duplicate registration and thaw verify both. Static atoms remain a
separate explicit case. Unregistered blocks, interior pointers, malformed
headers, and changed payloads are rejected.

Pending stock-runtime work present at entry is drained before the world
becomes `FROZEN`. Work discovered while canonicalizing the host heap, or while
actors run, remains pending. The actor scheduler and interpreter do not process
it; it becomes eligible only after actor state is gone and thaw has restored
the host world.

Between actor slices, frozen-world orchestration is trusted C code. It may use
non-OCaml storage, but must not invoke OCaml callbacks or allocate in the stock
minor heap. Stock shared-heap allocation is explicitly fenced while the world
is frozen.

An actor dispatch temporarily detaches the host C-root (`local_roots`) chain
and restores the identical pointer after the actor stack and heap are removed.
Actor allocation uses a nonraising arena attempt. An unsupported allocation
stops as unsupported, while quota exhaustion becomes a pointer-free,
actor-local heap-exhausted scheduler result instead of entering stock GC or
raising through host runtime state.

Mutation opcodes preflight the destination tag, bounds, owner, and right-hand
side before computing a write address or changing memory. This covers field,
vector, float-array, byte-string, and `OFFSETREF` writes. Global access,
object-cache operations, debugger opcodes, and effects fail closed.
`caml_modify` remains a last-resort checked backstop.

The primitive policy is exact and executable-specific. The only allowed call
is `C_CALL2` whose resolved function pointer is `caml_int_compare`; the
primitive index is bounds-checked first. Every other `C_CALL*` is denied before
entering C.

PR 4a exposes no `Actor` module, accepts no general host closure as actor input,
and contains no closure-graph copier. `GETGLOBAL`, its push/field variants, and
`SETGLOBAL` remain denied, so there is no general global access. Public entry
and closure copying belong to PR 4b; any approved global-read surface remains
a later claim. PR 4a also adds neither mailboxes nor independent collection,
so it makes no public actor or heap-isolation claim.

## PR 4b public-entry boundary

PR 4b exposes `Actor.run`, `spawn`, `self`, and `yield` on Linux x86-64
bytecode. Native code retains the same interface, but `run` returns
`Unsupported_runtime` without invoking its closure. The mailbox operations in
the final API remain absent until PR 5.

`run` freezes the host world before creating the scheduler. It copies the root
closure into actor zero, enters it with the actor-zero inbox immediate, and
destroys every actor heap and stack before thawing the host. Only pointer-free
lifecycle status crosses that teardown boundary; the OCaml `result` and any
error string are allocated after thaw.

The closure copier walks iteratively and preflights the complete captured
graph before creating an unpublished target heap. It preserves cycles,
aliases, recursive closure groups, and valid infix-closure offsets. Every
reachable supported block is copied, including host blocks under the freeze,
so a captured mutable value is never retained as a frozen edge. Immediates,
canonical atoms, and validated same-image code pointers are the only retained
identities. Finalisable values, unsupported tags, malformed closures, foreign
or malformed interior pointers, and over-quota graphs are rejected without
publishing a child.

Child spawn has a prepare/commit boundary. The copied heap and initialized
stack remain outside the PID table and ready queue until the parent has
successfully allocated its `Ok pid` value. A failed copy or parent-result
allocation destroys the unpublished child. Actor entry uses the runtime's
registered bytecode callback return frame, and explicit yield becomes visible
to the scheduler only after its `C_CALL1` primitive has returned and the
interpreter has completed that instruction.

All `GETGLOBAL` and `PUSHGETGLOBAL` forms remain denied, as does `SETGLOBAL`.
Actor-safe code in this stage must lexically capture the data and helper
closures it needs; the `Actor` operations are declared as direct externals so
their calls do not require a module-global lookup. The actor primitive
allowlist adds only the exact one-argument `spawn`, `self`, and `yield`
functions to PR 4a's exact two-argument integer comparison.

This is a safe-language, same-compiler-image boundary, not an `Obj` sandbox.
Closure and infix layouts and every code pointer are validated, but PR 4b does
not yet perform a complete reachable control-flow decode that could make an
`Obj`-forged interior code pointer safe. The interpreter still dynamically
fails closed before every denied opcode or primitive in ordinary compiler
output.

PR 4b still uses fixed bump arenas. It adds neither mailboxes nor private
collection and therefore does not make the heap-isolated-actor MVP claim. Its
gate is narrower: supported lexical captures are copied, module-global access
is rejected, and no supported spawn can create a cross-actor mutable pointer.

## PR 5 pointer-free mailbox boundary

PR 5 completes the MVP API surface with `Actor.send` and FIFO `receive`. A
mailbox entry owns a dedicated wire envelope in scheduler-managed C memory.
The envelope represents values with explicit token kinds, signed immediates,
static-atom tags, node indices, block tags and sizes, and copied raw bytes. It
contains no OCaml `value` field and no pointer into a sender or receiver heap.

The encoder iteratively discovers the complete message graph before creating
an envelope. Ordinary scanned blocks become indexed nodes; strings, bytes,
floats, and flat float arrays become raw payloads. Node references preserve
cycles and aliases inside the message without preserving physical identity
across actors. Closures, infix entries, lazy and forcing states, continuations,
objects, custom and abstract blocks, weak structures, foreign heap values,
malformed blocks, and over-quota graphs are rejected before publication.

Send publication has its own prepare/commit transaction. The destination PID
and generation are checked, the completed envelope and mailbox link remain
unpublished, and the sender allocates its public `Ok ()` result before commit.
Commit appends exactly one entry and wakes a blocked destination at the ready
queue tail. A dead, exited, failed, retired, or stale PID returns
`No_such_actor`; an unsupported or over-quota send changes no mailbox.

`receive` peeks at the oldest envelope and preflights its complete decoded
word count against the receiver's remaining arena quota before allocating any
block. It allocates every target node in the receiver heap, resolves indices,
verifies the resulting heap, and only then consumes and frees the envelope.
The wire seam destroys the complete sender heap before decoding and still
proves the decoded graph's contents, aliases, and cycles.

On an empty mailbox the receive primitive returns its inbox unchanged and
requests a block only after its `C_CALL1` has completed. The interpreter
rewinds that exact call before spilling the actor state. A later send marks the
actor runnable, and resume retries `receive` against the now nonempty mailbox.
No C frame or heap pointer is retained while blocked. If no actor is runnable,
the scheduler reports idle and `run` maps the all-blocked state to `Deadlock`.

Debug builds audit every mailbox link and envelope at send, receive, and
context-switch boundaries. Actor retirement and scheduler destruction free
all queued envelopes before releasing the actor slot or thawing the host.

PR 5 still uses the fixed bump arenas from PR 2. Successful decode consumes
arena space permanently, and a receive whose decoded graph does not fit kills
only that actor with contained heap exhaustion. Independent moving collection
and the full heap-isolation claim remain gated on PR 6.

## PR 6 private copying collector

PR 6 replaces each fixed bump arena with two private, guarded semispaces of
the actor's full heap quota. Allocation remains downward-growing in the active
space. When an allocation does not fit, a stop-the-scheduler copying collection
traces only the current actor and swaps the two spaces. The other space has its
own shadow-header ledger; forwarding and worklist tables are allocated when the
actor heap is created, so collection never needs fallible metadata allocation
after it starts rewriting roots.

Before moving anything, the collector verifies the complete source heap and
all active roots. It scans the current actor's C local roots, bytecode stack,
and saved GC-register parameter through the normal local-root scanner. Direct
bytecode allocations first spill the accumulator and environment into the
actor stack, then restore their moved values after collection. Ordinary
scanned fields are forwarded iteratively. Closures retain verified immutable
code and infix entries while only their environments are traced. Strings,
bytes, floats, and flat float arrays are copied as unscanned payloads.

Every copied block is indexed by its source-space header offset. This preserves
cycles, aliases, and valid infix identities without installing forwarding
headers in the source graph. The collector rejects a root or edge owned by any
other actor and never scans another actor's stack or heap. Before returning, it
verifies the destination heap and proves that every active root now names the
new semispace or approved frozen state. The abandoned space's shadow ledger is
then cleared, making stale pointers fail ownership checks.

Message receive reserves the envelope's complete graph size before allocating
decoder targets. Reservation may first collect the receiver; after it succeeds,
the decoder cannot trigger another move while its temporary C target table is
live. If the post-collection live graph plus the complete message still exceeds
the fixed actor quota, only that actor receives the contained heap-exhaustion
outcome.

The moving-GC acceptance test allocates several times each actor's quota in two
actors, preserves closure environments, aliases, and a cycle across repeated
moves, forces collection immediately before decoding a large string envelope,
and checks that the waiting actor's state is unchanged while its peer collects.
Together with the owner verifier, stock-allocation fence, closure copier, and
pointer-free mailbox proofs in the lower stack, this is the first layer that
meets the MVP's heap-isolation claim. The final PR 7 layer adds the containment,
retirement, stress/replay, and development-loop gate described next.

### Failure containment and retired heaps

An uncaught exception, unsupported operation, invalid-heap checkpoint, or
contained heap-quota failure transitions only the current child to `failed`.
The host scheduler records a pointer-free failure category, retires that child,
destroys its mailbox, stack, and heap, advances the PID generation, and keeps
dispatching peers. The same failures in actor zero stop the actor world and are
converted to `Root_failed` or `Root_heap_exhausted` only after all actor-owned
resources have been destroyed and the host heap has thawed. An empty ready
queue with blocked actors follows the same cleanup path and returns `Deadlock`.

Debug runtimes decommit both semispaces of each destroyed actor and retain the
eight most recent address reservations as `PROT_NONE` quarantine mappings.
This makes an accidental stale heap dereference fail deterministically while
bounding retained virtual address space; the oldest quarantine entry is
unmapped when the ring fills. Normal and instrumented runtimes unmap retired
heaps immediately.

The public failure-containment test forces child exception and live-heap quota
failure alongside a healthy peer, then checks that all three PIDs are stale.
It also runs fresh actor worlds after root exception, root heap exhaustion, and
deadlock to prove cleanup and thaw are reusable. A seeded 24-child stress test
mixes exception, heap-exhaustion, and normal-report actions and accepts a
canonical `ACTOR_TRACE` replay. See `HACKING.actor-mvp.md` for the fast build,
test, debug, and replay loop.

## Layer 9 frozen global reads

Layer 9 prepares an indexed, exact snapshot of the host global graph before
publishing actor zero. Actor execution admits `GETGLOBAL`, `PUSHGETGLOBAL`,
`GETGLOBALFIELD`, and `PUSHGETGLOBALFIELD` only when the referenced value and
field match that snapshot. Direct field and closure-environment reads remain
checked, so custom or abstract blocks, weak values, ephemerons, finalizable
values, and runtime metadata cannot become actor-visible through an approved
container or closure. Ordinary scanned immutable graphs, strings, floats,
double arrays, and same-image closures are readable. Aliases, cycles, and
closure identities stay shared and exact; these frozen blocks are never moved
by an actor collection.

`SETGLOBAL`, inline writes, and mutating primitives remain forbidden and fail
before changing host state. The snapshot is revalidated while actor mode is
active and before thaw completes. This supports ordinary read-only module
lookups; it does not make shared mutable module state actor-local.

The mailbox boundary is narrower. A non-immediate message graph must be owned
by the sending actor, so a string or data graph read directly from frozen
globals is rejected as `Unsupported_message` and no envelope is published.
Copying supported frozen graphs into messages is deferred to a later layer.

`ocamlactorcheck` is a first-pass advisory inventory for compiler-produced
bytecode. It lists global-read and `SETGLOBAL` instructions and `C_CALL*` sites,
including available symbol names, primitive names, and arities. It leaves
primitive capabilities unclassified and does not perform control-flow or
reachability analysis, validate branch targets or frozen graphs, enumerate
every mutation opcode, or certify runtime compatibility. Runtime checks remain
authoritative.

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
- PR 4a proves the internal freeze/thaw fence, exact frozen-value approval,
  nonraising actor allocation, and the initial opcode/primitive fence.
- PR 4b proves public entry, spawn copying, and safe-language closure.
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
- PR 4a: freeze/thaw rollback and snapshot checks hold; pending host actions
  are deferred; fenced writes and unsafe primitives fail before side effects.
- PR 4b: captured refs diverge after spawn; frozen global mutation fails
  closed; unsupported closure captures publish no actor.
- PR 5: FIFO wakeup, sender/receiver mutation independence, cycle and alias
  preservation, and transactional rejection of unsupported messages.
- PR 6: repeated moving GC in actor A neither scans nor changes actor B; all
  saved roots survive movement; whole-heap exit reclamation is complete.
- PR 7: child exception and quota exhaustion leave peers alive; root failure
  shuts down cleanly; deadlock, fault injection, and seeded replay agree with
  the reference model.

The full upstream bytecode tests remain the compatibility gate in every PR.
