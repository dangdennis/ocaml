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

type 'message pid = int
type 'message inbox = int
type monitor = int * int

type backtrace = {
  text : string;
  truncated : bool;
}

type exit_reason =
  | Normal
  | Uncaught_exception of { summary : string; backtrace : backtrace option }
  | Heap_limit
  | Mailbox_limit
  | Cancelled
  | Unsupported_operation of string
  | Runtime_failure of string

type monitor_error = Monitor_missing | Monitor_stale | Monitor_limit
type cancel_error = Cancel_missing | Cancel_stale | Cancel_self

type heap_limits = {
  initial_words : int;
  maximum_words : int;
}

let default_root_heap_limits = {
  initial_words = 1 lsl 18;
  maximum_words = 1 lsl 18;
}

let default_child_heap_limits = {
  initial_words = 1 lsl 16;
  maximum_words = 1 lsl 16;
}

type world_config = {
  root_heap : heap_limits;
  child_heap : heap_limits;
  max_actors : int;
  reductions_per_slice : int;
  max_message_words : int;
  max_mailbox_messages : int;
  max_mailbox_bytes : int;
  max_monitors : int;
}

let default_world_config = {
  root_heap = default_root_heap_limits;
  child_heap = default_child_heap_limits;
  max_actors = 1_024;
  reductions_per_slice = 1_000;
  max_message_words = 1 lsl 16;
  max_mailbox_messages = 1 lsl 16;
  max_mailbox_bytes = 1 lsl 28;
  max_monitors = 1 lsl 16;
}

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
  monitors : int;
  peak_monitors : int;
  monitor_quota_failures : int;
  monitor_limit : int;
}

type run_request =
  int * int * int * int * (unit inbox -> unit)

external run_request : run_request -> (unit, run_error) result
  = "caml_actor_run"

external run : (unit inbox -> unit) -> (unit, run_error) result
  = "caml_actor_run"

let run_with_heap_limits ~root ~child entry =
  run_request
    (root.initial_words, root.maximum_words,
     child.initial_words, child.maximum_words, entry)

type configured_run_request =
  int * int * int * int * int * int * int * int * int * int *
  (unit inbox -> unit)

external configured_run_request : configured_run_request ->
  (unit, run_error) result
  = "caml_actor_run"

let run_with_config config entry =
  configured_run_request
    (config.root_heap.initial_words, config.root_heap.maximum_words,
     config.child_heap.initial_words, config.child_heap.maximum_words,
     config.max_actors, config.reductions_per_slice,
     config.max_message_words, config.max_mailbox_messages,
     config.max_mailbox_bytes, config.max_monitors, entry)

type 'message spawn_request =
  int * int * ('message inbox -> unit)

external spawn_request : 'message spawn_request ->
  ('message pid, spawn_error) result
  = "caml_actor_spawn"

external spawn : ('message inbox -> unit) ->
  ('message pid, spawn_error) result
  = "caml_actor_spawn"

let spawn_with_heap_limits limits entry =
  spawn_request
    (limits.initial_words, limits.maximum_words, entry)

type 'message monitor_request = int * 'message pid

external monitor_request : 'message monitor_request ->
  (monitor, monitor_error) result
  = "caml_actor_spawn"

let monitor pid = monitor_request (0, pid)

type 'message cancel_request = int * 'message pid

external cancel_request : 'message cancel_request ->
  (unit, cancel_error) result
  = "caml_actor_spawn"

let cancel pid = cancel_request (1, pid)

external self : 'message inbox -> 'message pid
  = "caml_actor_self"

external send : 'message pid -> 'message ->
  (unit, send_error) result
  = "caml_actor_send"

external receive : 'message inbox -> 'message
  = "caml_actor_receive"

external await_exit : monitor -> exit_reason
  = "caml_actor_receive"

external yield : unit -> unit
  = "caml_actor_yield"

external stats : unit -> stats
  = "caml_actor_stats"
