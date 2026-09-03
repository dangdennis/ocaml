# OCaml heap-isolated actors roadmap

This document is the working roadmap for the experimental BEAM-inspired actor
runtime in this fork. It records the published fact boundary, the order of the
remaining work, and the gates required before a layer can be considered
complete.

The normative semantics of the implemented MVP remain in
[`runtime/README.actor-mvp.md`](runtime/README.actor-mvp.md). This roadmap does
not weaken that contract.

## Goal

The next meaningful product milestone is a normally compiled, supervised,
line-oriented TCP key/value service in which:

- each actor owns a private heap and mutable state;
- messages cross actor boundaries without retaining source pointers;
- module globals are readable only through the validated frozen image;
- the scheduler owns timers and operating-system resources;
- failures are observable and supervisors can perform bounded restarts; and
- unsupported language or runtime behavior fails closed.

Passing that milestone would demonstrate a useful bytecode actor runtime, not
only a heap-isolation test harness.

## Non-negotiable invariants

Every future layer must preserve these properties:

1. Every private heap block has exactly one live actor owner.
2. Actor allocation never falls back to OCaml's shared major heap.
3. Frozen state never points into an actor heap and cannot be mutated by an
   actor.
4. A mailbox contains no raw OCaml heap pointer.
5. Every received non-immediate value is owned by the receiving actor.
6. One actor's collection, failure, or destruction cannot invalidate another
   actor.
7. Scheduling occurs only at a boundary with no untracked live C roots.
8. Unknown opcodes, primitives, value layouts, and resource operations fail
   closed.
9. A failed spawn or send is transactional: it publishes no partial actor,
   PID, resource, or mailbox entry.
10. Runtime checks remain authoritative even when static tooling reports that a
    program appears compatible.

The actor runtime is not a security sandbox. Native execution, multiple
Domains, distribution, arbitrary C stubs, and shared mutable globals are not
part of the current roadmap.

## Published fact boundary

| Layer | Published branch / PR | State |
| --- | --- | --- |
| Actor MVP 0-7 | `actor-mvp/*`, PRs #1-#10 | Published as a stacked draft implementation |
| Layer 8: compatibility and observability baseline | `actor-real/08-contract-observability-baseline`, PR #11 | Published draft |
| Layer 9: frozen global reads | `actor-real/09-frozen-global-reads`, PR #12 | Published draft and Layer 10 base |
| Layer 10: primitive capabilities and core Stdlib compatibility | `actor-real/10-primitive-capabilities`, PR #13 | First audited primitive family published; full layer remains in progress |

Layer 10 now extends the authoritative implementation boundary through commit
`29786ee3cf`. Runtime enforcement and `ocamlactorcheck` consume one generated,
reviewed primitive-capability policy. Unknown, forbidden, misnamed, and
mis-arity primitive bindings fail closed. The first audited family implements
actor-owned array creation, access, mutation, blit, and fill, including checked
"unsafe" operations, frozen-destination rejection, overlap, GC movement, and
actor-local quota failure.

This is a deliberately narrow compatibility slice, not a claim that the full
Stdlib or Layer 10 is complete. Arbitrary C calls, `C_CALLN`, unsupported array
operations and representations, custom blocks, resources, callbacks, and other
unaudited primitive families remain forbidden. The next family must be selected
from the next observed broad-corpus failure rather than from a speculative
allowlist.

## Layer 10: primitive capabilities and core Stdlib compatibility

### Objective

Replace the narrow hand-maintained primitive fence with an auditable,
fail-closed capability policy, then admit enough ordinary Stdlib behavior to
run the reference service without weakening actor ownership.

### Work sequence

1. **Recover the fact boundary**
   - Record the Layer 9 base commit, Layer 10 branch, status, diff, tests, and
     first failure.
   - Preserve useful scratch work as a reviewable commit or patch.
   - Start again from the published Layer 9 tip if no recoverable artifact
     exists.

2. **Write the policy tests first**
   - Generate the runtime policy and `ocamlactorcheck` classification from one
     reviewed source of truth.
   - Test known-safe, actor-local, scheduler-aware, forbidden, and unknown
     primitives.
   - Prove that unknown or mismatched names and arities fail closed.
   - Require an explicit audit note for every admitted primitive family.

3. **Admit arrays as the first family**
   - Begin with `caml_array_make/2`, the first corpus failure.
   - Route allocations to the current actor heap.
   - Permit mutation only when the destination is owned by the current actor.
   - Reject writes to frozen, foreign, runtime, or malformed values before any
     side effect.
   - Cover allocation failure, bounds behavior, blits, fills, aliases, and GC
     movement where applicable.

4. **Advance one audited family at a time**
   - Re-run the corpus after each family and let the next failure determine the
     next slice.
   - Expected areas include strings and bytes, container helpers, comparison
     and hashing, numeric operations, and boxed/custom numeric values.
   - Custom blocks, finalizers, resources, callbacks, arbitrary C stubs, and
     other layouts without an ownership contract remain forbidden.

5. **Close the mailbox compatibility gap**
   - Copy supported immutable graphs from the frozen snapshot into the existing
     pointer-free envelope.
   - Support mixed actor-owned and frozen-origin graphs while preserving aliases
     and cycles.
   - Decode only into the receiver's heap; source provenance must not appear in
     the wire format.
   - Continue rejecting closures, resources, mutable frozen state, and
     unsupported layouts transactionally.

6. **Exercise a package canary**
   - Compile and run a small pure-OCaml package representative of the reference
     service.
   - Record unsupported operations precisely instead of broadening the allowlist
     speculatively.

### Milestone tracker

Checked items are published and verified facts; unchecked items remain part of
Layer 10.

- [x] Recover the exact Layer 9 base and publish the Layer 10 working branch.
- [x] Generate runtime and checker policy from one reviewed source of truth.
- [x] Test safe, actor-local, scheduler-aware, forbidden, unknown, name-mismatch,
      and arity-mismatch classifications.
- [x] Admit the checked core array family: make, safe and unsafe get/set, blit,
      and fill.
- [x] Cover array aliases, bounds, overlap, GC movement, frozen destinations,
      transactional validation, and actor-local allocation quota failure.
- [x] Pass the focused actor loop, normal, debug, instrumented, tooling,
      benchmark-smoke, ASan, UBSan, and repository-hygiene gates on a fresh
      GitHub runner for `29786ee3cf`.
- [ ] Re-run the broad Stdlib compatibility corpus and record its next exact
      unsupported primitive or representation boundary.
- [ ] Admit subsequent primitive families one audited slice at a time.
- [ ] Support the approved frozen-origin immutable graph subset across
      mailboxes as receiver-owned copies.
- [ ] Compile and run the pure-OCaml package canary.
- [ ] Pass the final full Layer 10 gates and open the stacked draft PR against
      the exact Layer 9 remote tip.

### Current next action

Re-run the broad Stdlib compatibility corpus from `29786ee3cf`. Preserve the
first reproducible failure as a red test, classify its primitive family and
ownership effects, and implement only that audited family. Do not broaden the
policy before that failure is known.

### Completion gates

Layer 10 is complete only when:

- the capability policy has one generated, reviewed source of truth;
- unknown primitives fail closed in both runtime and tooling tests;
- array and subsequently admitted primitive families obey allocation and write
  ownership rules;
- the broad Stdlib corpus reaches a documented, deliberate boundary;
- supported frozen-origin protocol values can cross a mailbox as receiver-owned
  copies;
- existing actor tests pass on normal, debug, and instrumented runtimes;
- ASan and UBSan pass;
- benchmark smoke and relevant upstream bytecode tests pass;
- a fresh GitHub runner reproduces the result; and
- the branch is committed, pushed, and opened as a stacked draft PR based on
  the exact Layer 9 remote tip.

## Layer 11: elastic heaps and configurable limits

Replace the fixed two-semispaces-per-actor model with controlled growth up to a
per-actor limit. Add explicit world and spawn configuration for:

- initial and maximum heap size;
- actor count;
- mailbox messages and bytes;
- message graph and serialization work;
- reductions or other execution budgets; and
- future timer and resource counts.

Allocation failure must remain actor-local and deterministic. Heap growth must
not introduce shared ownership or allow a collection to inspect another
actor's heap.

## Layer 12: structured exits and monitors

Introduce pointer-free structured exit reasons, bytecode backtrace data,
monitor references, down notifications, and `await_exit` behavior.

Required cases include normal exit, uncaught exception, heap or mailbox quota,
unsupported operation, killed actor, and scheduler/resource failures. Monitor
delivery must tolerate stale PIDs and races without reviving identities or
leaking actor-owned data.

## Layer 13: supervision

Build supervision on monitors rather than special scheduler shortcuts:

- one-for-one child restart;
- bounded restart intensity and time windows;
- deterministic child ordering and shutdown;
- explicit permanent, transient, and temporary restart policy if the smaller
  contract proves insufficient; and
- tests for crash loops, quota failures, failed restarts, and supervisor death.

Links, trap-exit behavior, and selective receive remain deferred unless the
reference service demonstrates that they are necessary.

## Layer 14: timers and scheduler event waiting

Add scheduler-owned monotonic timers and cancellation. Waiting on a timer or
external event must not be diagnosed as actor deadlock. Tests must use a
controllable clock or deterministic seam rather than wall-clock sleeps.

The scheduler should then wait for the earliest timer or I/O event when there
is no runnable actor, while retaining deterministic replay for injected event
sequences.

## Layer 15: scheduler-owned nonblocking I/O

Add a minimal nonblocking TCP surface backed by a scheduler reactor (initially
Linux `epoll`):

- `listen`, `accept`, `read`, `write`, and `close`;
- generation-tagged handles instead of heap pointers or bare descriptors;
- exactly one actor owner for every live resource;
- atomic ownership transfer where required;
- automatic cancellation and cleanup when an actor exits; and
- quotas for descriptors, buffered bytes, and pending operations.

No actor may block the scheduler thread in an ordinary I/O primitive.

## Layer 16: reference service and hardening

The acceptance application is a supervised line-oriented TCP key/value
service with separate modules:

- a root supervisor;
- a state actor that exclusively owns the map;
- a listener actor that owns the listening socket;
- one monitored actor per accepted connection; and
- protocol request and response values that cross mailboxes normally.

The final gate includes concurrent clients, intentional actor crashes, bounded
restarts, quota exhaustion, disconnect races, resource-leak checks, a pure
OCaml package canary, fuzzing of wire and resource boundaries, and sustained
soak testing.

## Deferred work

Only after the reference service passes should the project reassess:

- broader Stdlib and external-package compatibility;
- selective receive and its scanning/fairness contract;
- links and richer OTP-style behaviors;
- native actors with compiler-inserted safepoints and precise stack maps;
- multiple parallel scheduler threads or Domains;
- richer operating-system resources;
- distribution and remote messaging; and
- hot code loading.

Native support is a separate compiler/runtime feasibility stack. The current
bytecode interpreter supplies resumability, reduction accounting, allocation
routing, write fencing, and primitive enforcement; native execution must first
replace all of those mechanisms explicitly.

## Development method

Every layer follows the same loop:

1. Establish the exact published base and a clean worktree.
2. Add a failing contract or compatibility test.
3. Implement the smallest audited slice that makes it pass.
4. Run the focused test and the nearest regression set.
5. Run normal, debug, instrumented, sanitizer, benchmark, and upstream gates in
   proportion to the change.
6. Audit ownership, failure atomicity, cleanup, and generated artifacts.
7. Commit and publish one comprehensible stacked draft PR.
8. Record the next concrete failure before widening scope.

Do not infer compatibility from compilation, a checker report, or a README
claim. The executable runtime tests and the public CI result define the fact
boundary.
