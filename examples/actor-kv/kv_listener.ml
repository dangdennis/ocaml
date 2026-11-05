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

type assignment = Reject | Assign of (Actor.Net.socket * Kv_protocol.reply)
  Actor.pid
type event = Accepted of assignment Actor.pid | Stop

let run ~address ~port ~root inbox =
  let listener = match Actor.Net.listen ~address ~port with
    | Ok listener -> listener | Error _ -> failwith "listener bind failed" in
  let self = Actor.self inbox in
  let rec loop () =
    let socket = match Actor.Net.accept listener with
      | Ok socket -> socket | Error _ -> failwith "listener accept failed" in
    match Actor.send root (Accepted self) with
    | Error _ -> ignore (Actor.Net.close socket)
    | Ok () ->
        (match Actor.receive inbox with
         | Reject -> ignore (Actor.Net.close socket)
         | Assign destination ->
             (match Actor.Net.transfer_with socket destination
               Kv_protocol.Attached with
              | Ok () -> ()
              | Error _ -> ignore (Actor.Net.close socket);
                  ignore (Actor.cancel destination)));
        loop ()
  in loop ()
