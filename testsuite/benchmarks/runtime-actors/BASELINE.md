# Layer 8 actor baseline

This is an informational five-sample baseline captured on 2026-08-25 from
the Layer 8 working tree with `ACTOR_BENCH_SCALE=1`.

- Linux 6.18.35, x86-64
- AMD EPYC 9V74 virtual CPU, 9 online vCPUs
- GCC 13.3.0
- OCaml 5.5.0 in-tree bytecode runtime

Median operations per second:

| Workload | Median |
| --- | ---: |
| Pure integer loop | 73,297,662 |
| Same loop in one actor world | 41,220,115 |
| Empty actor worlds | 6,045 |
| Spawn and acknowledge | 10,431 |
| Ping-pong round trips | 5,472 |
| 128-node graph-copy round trips | 1,501 |

On this host the actor-world integer loop delivered 56% of the ordinary
bytecode loop rate. That number includes scheduler reduction accounting and
world entry/exit; it is not a cross-machine performance claim. Raw samples
are in `baseline.csv`. Re-run the documented command on the same idle machine
before treating a difference as a regression.
