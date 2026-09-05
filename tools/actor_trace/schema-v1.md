# Actor trace schema v1

The trace is newline-delimited JSON. `seq` is a monotonically increasing
logical sequence within one actor world. Numeric actor IDs are the runtime's
generation-tagged PIDs; consumers may derive the slot with `pid mod 65536` and
the generation with `pid / 65536`.

## Events

- `world_start`: `schema`, `seq`, `word_bytes`, `actor_capacity`,
  `reduction_budget`, `message_word_limit`, `mailbox_message_limit`,
  `mailbox_byte_limit`, and `monitor_limit`.
- `spawn`: `seq`, `actor`, `parent`, `heap_used`, `heap_capacity`, and
  `heap_maximum`. Root and children of the root use parent `0`; `actor`
  distinguishes the root event.
- `state`: `seq`, `actor`, `state`, per-actor `mailbox_messages` and
  `mailbox_bytes`, `heap_used`, `heap_capacity`, `heap_maximum`, `collections`,
  and `growths`. State is sampled only at verified scheduler boundaries.
- `send`: `seq`, `message`, `sender`, `receiver`, encoded `bytes`, and the
  destination mailbox counts after transactional publication.
- `send_rejected`: `seq`, `sender`, `receiver`, and a bounded `reason` enum.
  Its v1 values are `missing`, `quota`, `unsupported`, and `resource`. It never
  creates a message identity.
- `receive`: the matching `message`, `sender`, and `receiver`, encoded `bytes`,
  and destination mailbox counts after successful decode and consumption.
- `drop`: the matching message identity and endpoints when retirement discards
  an unconsumed envelope.
- `exit`: `seq`, `actor`, bounded `reason`, and `dropped_messages` observed
  before heap destruction or PID-generation advancement.
- `world_end`: `schema`, `seq`, `outcome`, `events_dropped`, and `complete`.
  A later actor world using the same destination replaces the file; one trace
  represents one world.

Unknown event names may be ignored. A consumer must reject an unsupported
`world_start.schema` rather than interpreting it as v1.

## Privacy and ownership boundary

Records contain only bounded scalars, enums, counts, and generation-tagged
integer identities. They contain no OCaml values, message payloads, raw
pointers, heap addresses, exception summaries, or backtraces. The bounded
buffer and trace file are scheduler-owned. File output happens only after the
scheduler has returned to host context. A destination or allocation failure
emits one host-side diagnostic, disables tracing, and does not alter the actor
world result.
