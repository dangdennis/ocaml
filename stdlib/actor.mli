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

    An [inbox] is currently an actor identity capability supplied to an actor
    entry function. FIFO [send] and [receive] operations are intentionally not
    exposed until the pointer-free mailbox stage in PR 5. *)

type 'message pid
(** The generation-tagged identity of an actor. *)

type 'message inbox
(** The identity capability supplied to an actor entry function. *)

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

external run : (unit inbox -> unit) -> (unit, run_error) result
  = "caml_actor_run"
(** [run root] suspends the host computation and runs [root] as actor zero.
    The host computation resumes only after the actor world has been retired. *)

external spawn : ('message inbox -> unit) ->
  ('message pid, spawn_error) result
  = "caml_actor_spawn"
(** [spawn entry] copies [entry] and its supported captured graph into a new
    actor, returning its identity after the child is published. *)

external self : 'message inbox -> 'message pid
  = "caml_actor_self"
(** [self inbox] returns the identity associated with [inbox]. *)

external yield : unit -> unit
  = "caml_actor_yield"
(** [yield ()] places the current actor at the tail of the ready queue. *)
