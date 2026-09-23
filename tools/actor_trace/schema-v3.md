# Actor trace schema v3

Version 3 retains the [v2 event contract](schema-v2.md) and adds scheduler-owned
network events. The viewer accepts schemas 1, 2 and 3 and rejects other versions.

A `net` event contains `seq`, owner `actor`, decimal-string `resource` and
`operation` identities, `action`, successful `bytes`, and world gauges
`resources`, `operations`, and `buffered_bytes`. Zero identities mean that the
event describes a world/owner aggregate or has no operation identity.

Actions are `listen`, `port`, `accept`, `read`, `write`, `close`,
`close_listener`, `transfer`, `waiting`, `ready`, and `cleanup`. Successful
public operations are recorded after commit/consumption. `waiting` records
suspension, `ready` records host-side completion, and `cleanup` records actor
or world retirement (actor zero denotes world cleanup). Cleanup precedes the
footer and monitor observers run only after owner cleanup. Transfer records
the old handle; ordinary mailbox events record delivery of the new authority.
No extra clock sampling is performed for tracing.

The gauges include reserved accepts and completed but unconsumed operations.
`bytes` is nonzero only for read/write completion; it does not promise peer
receipt. A handle is an identity checked against its actor owner, not a secret
capability. Events never contain socket descriptors, addresses, ports, bytes
from the network, OCaml values, or heap pointers. The bounded trace buffer is
flushed only from host context; trace failure leaves execution unchanged.

The network gauges appear in the viewer's world details. Timer precision,
completed-file replay, live tailing, and incomplete-trace indication retain
the v2 behavior. Network payload replay is deliberately outside this schema.
