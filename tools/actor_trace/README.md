# Actor trace viewer

Set `OCAML_ACTOR_TRACE` to a host-visible path before running an actor bytecode
program. The runtime writes versioned NDJSON without message payloads or raw
pointers. The bundled demo creates 200 workers with different live-heap sizes,
so the viewer exercises a genuinely dense actor graph from runtime events.

For the current macOS checkout, compile and run through the Linux amd64 image:

```sh
docker run --rm --platform linux/amd64 \
  -e OCAML_ACTOR_TRACE=/work/actor-trace.ndjson \
  -v "$PWD:/work" -w /work \
  ocaml-actor-debug:bookworm \
  sh -lc 'cp tools/actor_trace/demo.ml /tmp/actor_demo.ml && \
    ./runtime/ocamlrun ./ocamlc \
    -use-runtime ./runtime/ocamlrun -nostdlib -I stdlib \
    -o /tmp/actor-demo.byte /tmp/actor_demo.ml && \
    ./runtime/ocamlrun /tmp/actor-demo.byte'
```

Then start the dependency-free local viewer:

```sh
python3 tools/actor_trace/server.py actor-trace.ndjson
```

Open <http://127.0.0.1:8765>. The viewer polls the trace, supports live runs and
completed replay, and marks traces with dropped events as incomplete.

`OCAML_ACTOR_TRACE_BUFFER_EVENTS` sets the bounded in-memory event capacity.
It defaults to 4096 and accepts values from 1 through 1,000,000. The buffer is
drained only when the scheduler has returned to host context.

See [schema-v1.md](schema-v1.md) for the event contract and privacy boundary.
