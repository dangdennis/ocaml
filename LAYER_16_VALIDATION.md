# Layer 16 validation

The implementation starts from PR #18's exact Layer 15 tip,
`a24deb6ec91edb0b4db10551dbe70f2c4d967d44`. Local validation uses Linux amd64
Docker, a warning-as-error bytecode build, and separate work directories/logs.
The new API test first failed against that base with `Unbound module Actor.Net`.

## Focused evidence

- `actor_net.ml`: real listener lifecycle, invalid addresses/ranges, owner
  rejection, cancellation, timer progress, and zero remaining charges.
- `actor_net_store.ml`: allocation/registration rollback, quota accounting,
  retained completions, stable duplicate/stale event handling, authority renewal,
  200 resource generations, identity exhaustion, and full cleanup.
- `actor_net_fake.ml`: exact-once accept/read/write, short I/O, EINTR/EAGAIN,
  EOF, zero-length requests, CPU progress, mailbox-full transfer rollback,
  transfer-back stale aliases, backend failures, bounded interrupted waits,
  and deadlock with open but unwaited resources.
- `actor_net_stress.ml`: 200 actor/resource generations under small heaps,
  descriptor/operation/byte quotas, transfer, supervision restart, and signals.
- `actor_net_loopback.ml`: an external client announces no timing assumptions;
  a port pipe coordinates connection. Fragmented echo, socket handoff, a
  256 KiB response with forced write backpressure, concurrent CPU/timer actors,
  real pre-kernel signal arrival, restored signal masks, and OS descriptor
  counts are checked. A 30-second alarm is only a failure watchdog.
- `actor_net_trace.ml`: schema v3 lifecycle events, zero cleanup gauges and
  exclusion of payloads, addresses, ports and descriptors.

## Acceptance commands

Configure with `--disable-native-compiler --disable-native-toplevel
--enable-debug-runtime --enable-instrumented-runtime --enable-ocamltest
--enable-warn-error`, then build with `make -j8`.

Run `make -C testsuite one DIR=tests/runtime-actors` on normal, debug
(`USE_RUNTIME=d`) and instrumented (`USE_RUNTIME=i`) runtimes. Each suite has
45 passing tests and one expected native-runtime skip. Separate ASan and
UBSan source builds use the flags from `.github/workflows/actor-runtime.yml`.
Both local sanitizer suites passed the same 45 tests with one expected skip.
The public draft's checks also record fresh-runner sanitizer validation.

Also run the callback, backtrace, effects, tool-ocamlactorcheck and tool-dumpobj
suites; the pinned Astring package canary; actor/timer/network benchmark smoke;
`make alldepend`; manual module/case checks; labelled-interface checks;
`tools/check-typo`; and `git diff --check`. The viewer's JavaScript syntax and
v1/v2/v3 replay model are checked, including 64-bit timer strings and network
cleanup gauges. CI now runs all three benchmark smoke tests.

## Evidence boundary

Actor execution remains Linux x86-64 bytecode only. Full Build and MSVC CI
check other compiler/runtime configurations; a skipped native actor test is
expected. Network readiness order is external input, with stable ordering only
within each injected/polled batch. These tests do not establish an application
support promise. Native/parallel actors, DNS, outbound connect, TLS, and the
Layer 17 application/soak gates remain outside this implementation.
