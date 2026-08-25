# Actor runtime benchmarks

These benchmarks provide raw feedback, not a shared-runner performance gate.
They time host-visible workloads around `Actor.run`, so actor code does not
need access to a process clock.

From the repository root:

```sh
bench_dir="$(mktemp -d)"
cp testsuite/benchmarks/runtime-actors/actor_bench.ml "$bench_dir/"
./runtime/ocamlrun ./ocamlc -I stdlib \
  -o "$bench_dir/actor_bench.byte" \
  "$bench_dir/actor_bench.ml"
ACTOR_BENCH_SCALE=1 ./runtime/ocamlrun "$bench_dir/actor_bench.byte"
```

The output is tab-separated and includes operation count, elapsed CPU time,
and operations per second. The two `pure_loop` rows expose actor-world
overhead for the same integer workload; ping-pong and graph-copy operations
are labeled as round trips. Run several samples on the same idle machine and
retain the raw output when comparing commits. `ACTOR_BENCH_SCALE` multiplies
every workload.
