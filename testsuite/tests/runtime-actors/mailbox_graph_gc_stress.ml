(* TEST
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

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

let send_or_fail pid message =
  match Actor.send pid message with
  | Ok () -> ()
  | Error _ -> failwith "send failed"

let root root_inbox =
  let root = Actor.self root_inbox in
  let rounds = 1 in
  let child =
    match Actor.spawn (fun child_inbox ->
      let rec loop remaining =
        if remaining > 0 then begin
          ignore (Actor.receive child_inbox);
          send_or_fail root ();
          loop (remaining - 1)
        end
      in
      loop rounds) with
    | Ok pid -> pid
    | Error _ -> failwith "spawn failed"
  in
  let rec payload remaining tail =
    if remaining = 0 then tail
    else payload (remaining - 1) (remaining :: tail)
  in
  let message = payload 128 [] in
  for _ = 1 to rounds do
    send_or_fail child message;
    ignore (Actor.receive root_inbox)
  done

let () =
  match Actor.run root with
  | Ok () -> print_endline "mailbox graph GC stress: ok"
  | Error Actor.Unsupported_runtime -> failwith "unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "root heap exhausted"
  | Error Actor.Deadlock -> failwith "deadlock"
  | Error (Actor.Root_failed message) -> failwith message
