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

type capability = Pure | Actor_local | Scheduler_aware | Forbidden

type entry = {
  name : string;
  arity : int;
  capability : capability;
  family : string;
  audit : string;
}

type classification =
  | Allowed of entry
  | Denied of entry
  | Arity_mismatch of entry
  | Unknown

val entries : entry array
val classify : string -> int -> classification
val capability_name : capability -> string
