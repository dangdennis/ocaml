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

let destination = [| 41; 42; 43 |]

let set () = destination.(0) <- 0
let fill () = Array.fill destination 0 2 0

let unchanged () =
  destination.(0) = 41 && destination.(1) = 42 && destination.(2) = 43
