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

let write socket text =
  let bytes = Bytes.of_string text in
  let rec loop offset =
    if offset = Bytes.length bytes then true
    else match Actor.Net.write socket bytes offset (Bytes.length bytes -
      offset) with
      | Ok n when n > 0 -> loop (offset + n)
      | _ -> false
  in loop 0

let run ~state ~test_control ~shutdown inbox =
  let socket, attached = Actor.receive inbox in
  assert (attached = Kv_protocol.Attached);
  let buffer = Bytes.create (Kv_protocol.max_line + 2) in
  let self = Actor.self inbox in
  let process line =
    match Kv_protocol.parse ~test_control line with
    | Error () -> write socket "ERROR\n"
    | Ok Quit -> ignore (write socket "BYE\n"); false
    | Ok Shutdown ->
        if write socket "BYE\n" then shutdown ();
        false
    | Ok Crash -> failwith "injected connection failure"
    | Ok command ->
        match Actor.send state (Kv_state.Execute (command, socket, self)) with
        | Error _ -> false
        | Ok () ->
            let returned, reply = Actor.receive inbox in
            assert (returned = socket);
            write socket (Kv_protocol.render reply)
  in
  let rec lines used start offset =
    if offset = used then
      (if start > 0 then Bytes.blit buffer start buffer 0 (used - start);
       read (used - start))
    else if Bytes.get buffer offset = '\n' then (
      let length = offset - start in
      let length = if length > 0 && Bytes.get buffer (offset - 1) = '\r'
        then length - 1 else length in
      if process (Bytes.sub_string buffer start length)
      then lines used (offset + 1) (offset + 1))
    else lines used start (offset + 1)
  and read used =
    if used = Bytes.length buffer then ignore (write socket "ERROR\n")
    else match Actor.Net.read socket buffer used (Bytes.length buffer - used)
      with
      | Ok n when n > 0 -> lines (used + n) 0 used
      | _ -> ()
  in
  read 0;
  ignore (Actor.Net.close socket)
