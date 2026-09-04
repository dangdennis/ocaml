(**************************************************************************)
(*                                                                        *)
(*                                 OCaml                                  *)
(*                                                                        *)
(*                             Dennis Dang                                *)
(*                                                                        *)
(*   Copyright 2026 Dennis Dang                                           *)
(*                                                                        *)
(*   All rights reserved.  This file is distributed under the terms of    *)
(*   the GNU Lesser General Public License version 2.1, with the          *)
(*   special exception on linking described in the file LICENSE.          *)
(*                                                                        *)
(**************************************************************************)

(** Experimental heap-isolated actors.

    This MVP stage provides actor-world entry, spawning, actor identity, and
    cooperative yielding on Linux x86-64 bytecode runtimes. Native code and
    other platforms report [Unsupported_runtime] from {!run}.

    An [inbox] is an actor identity capability supplied to an actor entry
    function. Messages are copied through pointer-free FIFO envelopes. *)

type 'message pid
(** The generation-tagged identity of an actor. *)

type 'message inbox
(** The identity capability supplied to an actor entry function. *)

type monitor
(** A generation-safe scheduler capability for observing one actor exit. *)

type backtrace = {
  text : string;
  truncated : bool;
}
(** Bounded bytecode backtrace text. [truncated] records an explicit size
    limit rather than silently presenting incomplete data as complete. *)

type exit_reason =
  | Normal
  | Uncaught_exception of { summary : string; backtrace : backtrace option }
  | Heap_limit
  | Mailbox_limit
  | Cancelled
  | Unsupported_operation of string
  | Runtime_failure of string
(** Pointer-free reason copied from scheduler-owned exit data into the
    waiting actor's heap. [Mailbox_limit] is reserved for an actor-fatal
    mailbox condition; a recoverable {!send} rejection does not terminate
    its caller. *)

type monitor_error = Monitor_missing | Monitor_stale

type cancel_error = Cancel_missing | Cancel_stale | Cancel_self

type heap_limits = {
  initial_words : int;
  maximum_words : int;
}
(** Actor-private heap capacity in machine words. The initial value must be
    positive and no greater than the maximum. *)

val default_root_heap_limits : heap_limits
val default_child_heap_limits : heap_limits

type world_config = {
  root_heap : heap_limits;
  child_heap : heap_limits;
  max_actors : int;
  reductions_per_slice : int;
  max_message_words : int;
  max_mailbox_messages : int;
  max_mailbox_bytes : int;
}
(** Immutable actor-world limits. [max_actors] includes the root actor and must
    be between 2 and 65,536. Reduction and message limits must be positive.
    [max_message_words] bounds graph discovery and pointer-free serialization
    work for each send. [max_mailbox_messages] and [max_mailbox_bytes] bound
    the aggregate queued envelopes in the actor world. All limits must be
    positive. *)

val default_world_config : world_config

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
  mailbox_bytes : int;
  mailbox_quota_failures : int;
  current_heap_words : int;
  maximum_heap_words : int;
  heap_growths : int;
  actor_capacity : int;
  reduction_budget : int;
  message_word_limit : int;
  mailbox_message_limit : int;
  mailbox_byte_limit : int;
}
(** A deterministic snapshot of the current actor world's scheduler counts.
    [runnable_actors] includes the actor taking the snapshot.
    [messages_dropped] counts queued messages discarded when an actor retires.
    [mailbox_bytes] is the current aggregate encoded-byte charge and
    [mailbox_quota_failures] counts rejected graph, message-count, and byte
    quota attempts. Heap fields describe only the actor taking the snapshot;
    the remaining limit fields describe its immutable actor world. Counters
    saturate at [max_int]. *)

external run : (unit inbox -> unit) -> (unit, run_error) result
  = "caml_actor_run"
(** [run root] suspends the host computation and runs [root] as actor zero.
    The host computation resumes only after the actor world has been retired. *)

val run_with_heap_limits :
  root:heap_limits -> child:heap_limits ->
  (unit inbox -> unit) -> (unit, run_error) result
(** [run_with_heap_limits ~root ~child entry] uses elastic heaps for the root
    and for children created by {!spawn}. It raises [Invalid_argument] before
    entering the actor world when either limit pair is invalid. *)

val run_with_config : world_config ->
  (unit inbox -> unit) -> (unit, run_error) result
(** [run_with_config config entry] validates every limit before freezing the
    host world. It raises [Invalid_argument] for an invalid configuration. *)

external spawn : ('message inbox -> unit) ->
  ('message pid, spawn_error) result
  = "caml_actor_spawn"
(** [spawn entry] copies [entry] and its supported captured graph into a new
    actor, returning its identity after the child is published. *)

val spawn_with_heap_limits : heap_limits -> ('message inbox -> unit) ->
  ('message pid, spawn_error) result
(** [spawn_with_heap_limits limits entry] overrides the configured child heap
    limits for one spawn. Invalid limits return [Error Initial_heap_limit]. *)

val monitor : _ pid -> (monitor, monitor_error) result
(** [monitor pid] registers the calling actor to receive exactly one exit
    reason for the current generation of [pid]. Dead empty slots report
    [Monitor_missing]; mismatched or retired generations report
    [Monitor_stale]. *)

val cancel : _ pid -> (unit, cancel_error) result
(** [cancel pid] requests deterministic termination of the current generation
    of [pid]. The target cannot run again after a successful request and its
    monitors receive [Cancelled] through the ordinary exit boundary. Dead
    empty slots report [Cancel_missing] and mismatched or retired generations
    report [Cancel_stale]. Cancelling the calling actor reports [Cancel_self]
    rather than destroying its active heap. *)

external self : 'message inbox -> 'message pid
  = "caml_actor_self"
(** [self inbox] returns the identity associated with [inbox]. *)

external send : 'message pid -> 'message ->
  (unit, send_error) result
  = "caml_actor_send"
(** [send pid message] transactionally copies [message] into [pid]'s FIFO
    mailbox. Unsupported values, over-quota graphs, and sends exceeding the
    world's aggregate mailbox limits are not published. *)

external receive : 'message inbox -> 'message
  = "caml_actor_receive"
(** [receive inbox] returns the oldest message, blocking only the current
    actor while its mailbox is empty. *)

val await_exit : monitor -> exit_reason
(** [await_exit monitor] blocks only the calling actor until the monitored
    actor exits. Exit events do not enter or reorder the typed user mailbox.
    A forged, foreign, or already-consumed monitor fails closed. *)

external yield : unit -> unit
  = "caml_actor_yield"
(** [yield ()] places the current actor at the tail of the ready queue. *)

external stats : unit -> stats
  = "caml_actor_stats"
(** [stats ()] snapshots scheduler and mailbox counts. It raises
    [Invalid_argument] outside an actor world. *)
