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

type message = {
  left : string;
  right : string;
  values : int array;
}

let shared = "frozen-mailbox"
let message = { left = shared; right = shared; values = [| 10; 20; 30 |] }
let closure () = ignore message

let unchanged () =
  message.left == message.right
  && message.left = "frozen-mailbox"
  && message.values = [| 10; 20; 30 |]
