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
}

external run : (unit inbox -> unit) -> (unit, run_error) result
  = "caml_actor_run"

external spawn : ('message inbox -> unit) ->
  ('message pid, spawn_error) result
  = "caml_actor_spawn"

external self : 'message inbox -> 'message pid
  = "caml_actor_self"

external send : 'message pid -> 'message ->
  (unit, send_error) result
  = "caml_actor_send"

external receive : 'message inbox -> 'message
  = "caml_actor_receive"

external yield : unit -> unit
  = "caml_actor_yield"

external stats : unit -> stats
  = "caml_actor_stats"
