# Developing the heap-isolated actor MVP

This fork targets Linux x86-64 bytecode. Native OCaml programs deliberately
report `Unsupported_runtime`; use the rebuilt bytecode compiler and runtime for
actor work.

## One-time setup

From the repository root:

```sh
./configure --enable-ocamltest --enable-warn-error
make -j"$(nproc)"
```

The warning-as-error configuration keeps runtime changes honest. A clean build
also produces the normal, debug, instrumented, and position-independent
bytecode runtimes used by the focused test loop.

## Fast feedback loop

After editing runtime C or headers:

```sh
make -j"$(nproc)" runtime-all
make -C testsuite one TEST=tests/runtime-actors/failure_containment.ml
```

After editing the public API or its tests, rebuild the bytecode compiler and
standard library before running the test:

```sh
make -j"$(nproc)" ocamlc
make -C testsuite one TEST=tests/runtime-actors/public_actor_api.ml
```

Run the complete actor directory before every commit:

```sh
make -C testsuite one DIR=tests/runtime-actors
make -C testsuite one DIR=tests/runtime-actors USE_RUNTIME=d
make -C testsuite one DIR=tests/runtime-actors USE_RUNTIME=i
```

The testsuite keeps generated files under each test directory by default. To
isolate logs and work directories while comparing variants, pass both settings
as make command-line variables:

```sh
make -C testsuite one DIR=tests/runtime-actors \
  OCAMLTESTDIR=/tmp/ocaml-actors-normal \
  TESTLOG=/tmp/ocaml-actors-normal.log
```

Do not accept a test command that says `Nothing to be done`, and confirm that
the summary reports a nonzero number of tests considered.

Some mounted workspaces create newly linked `.opt` tools without executable
bits. List them before changing permissions:

```sh
find . -type f -name '*.opt' ! -perm -111 -print
```

Then apply `chmod +x` only to the generated paths reported by that command. If
a parallel from-scratch bootstrap instead leaves `ocamlc` with a truncated
bytecode trailer, relink that target and finish serially:

```sh
make -j1 -W driver/main.cmo ocamlc
./runtime/ocamlrun ./ocamlc -version
make -j1
```

## Running a small actor program

Create `demo.ml`:

```ocaml
let () =
  match Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    match Actor.spawn (fun _ -> ignore (Actor.send root ())) with
    | Error _ -> failwith "spawn failed"
    | Ok _ -> ignore (Actor.receive root_inbox)) with
  | Ok () -> print_endline "hello from an isolated actor"
  | Error _ -> failwith "actor world failed"
```

Compile and run it with the in-tree toolchain:

```sh
./runtime/ocamlrun ./ocamlc -I stdlib -o demo.byte demo.ml
./runtime/ocamlrun ./demo.byte
```

For runtime assertions and backtraces:

```sh
OCAMLRUNPARAM=b ./runtime/ocamlrund ./demo.byte
```

## Wider gates

Use these after the focused loop is green:

```sh
make -j"$(nproc)"
make -C testsuite one DIR=tests/callback
make -C testsuite one DIR=tests/backtrace
make -C testsuite one DIR=tests/effects
make -C api_docgen
```

For a from-scratch gate:

```sh
make clean
make -j"$(nproc)"
make -C testsuite one DIR=tests/runtime-actors
```

## Reproducing seeded failures

The mixed-failure stress test uses the deterministic PRNG and trace format from
the actor harness:

```sh
ACTOR_SEED=0x5eed5eed5eed5eed \
  make -C testsuite one TEST=tests/runtime-actors/failure_stress.ml
```

When a failure report prints a canonical trace, save it as `trace.txt` and run:

```sh
ACTOR_SEED=0x5eed5eed5eed5eed ACTOR_TRACE=trace.txt \
  make -C testsuite one TEST=tests/runtime-actors/failure_stress.ml
```

## Stacked branch workflow

Each branch targets the previous branch, not the pinned base. For a new top
layer:

```sh
git switch actor-mvp/07-failure-hardening
git switch -c actor-mvp/next-layer
git push -u origin actor-mvp/next-layer
gh pr create --draft \
  --base actor-mvp/07-failure-hardening \
  --head actor-mvp/next-layer
```

If a lower layer changes, rebase each descendant in order and push with
`--force-with-lease`. Keep every PR draft until its own gate and all lower
layers are green. GitHub's stack view is presentation and merge orchestration;
the branch base chain remains the source of truth.
