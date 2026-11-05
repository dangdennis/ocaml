# Mainline resync: 2026-11-05

All 18 open actor PRs were rebased as a dependency stack onto OCaml upstream
`trunk` at `f0c10233335c90a21fd705e916900141f043258b`. The fork's default branch
is named `trunk`. PR #1 now targets it; subsequent PRs retain their predecessor
as base so that their diffs remain incremental.

The 75 original commits were preserved. Compatibility resolutions retain
upstream continuation handling, bounds checking and locale handling; use the
canonical opcode tables and renamed heap size-class table; and refresh test
syntax, generated dependencies and numeric inventory expectations.

The final stack source tree passed a Linux amd64 warning-as-error bytecode
build with debug and instrumented runtimes enabled. Normal, debug and
instrumented actor suites each passed 45 tests with one expected native skip.
Tooling passed (actor checker: 1; dumpobj: 7), callback (14 passed/2 skipped),
backtrace (23/3), effects (23/1), and both affected expect tests (1 each).
Dependency regeneration, whitespace checks, tracked-file ignore checks and
focused typo checks also passed. Folding compatibility fixes into their
originating PR commits preserved the tested tree exactly.

These checks validate the combined stack. Every intermediate PR was not built
separately. The earlier sanitizer and macOS results in `LAYER_16_VALIDATION.md`
precede this rebase. GitHub Actions remains disabled, so fresh hosted CI is
still pending.

Original heads are retained in local `refs/backup/resync-20261105/` refs and a
Git bundle at `/private/tmp/ocaml-resync-state/before.bundle`.
