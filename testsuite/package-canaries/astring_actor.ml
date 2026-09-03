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

type request =
  | Set of string * int
  | Check of string * int * unit Actor.pid
  | Stop

let parse line =
  match Astring.String.fields ~empty:false line with
  | ["SET"; key; value] when Astring.String.is_prefix ~affix:"item-" key ->
      Set (key, int_of_string value)
  | _ -> failwith "invalid canary command"

let send_or_fail destination message =
  match Actor.send destination message with
  | Ok () -> ()
  | Error _ -> failwith "package canary send failed"

let rec serve inbox table =
  match Actor.receive inbox with
  | Set (key, value) ->
      Hashtbl.replace table key value;
      serve inbox table
  | Check (key, expected, reply_to) ->
      assert (Hashtbl.find table key = expected);
      send_or_fail reply_to ();
      serve inbox table
  | Stop -> ()

let commands = ["SET item-alpha 17"; "SET item-beta 25"]

let () =
  match Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    match Actor.spawn (fun inbox ->
      serve inbox (Hashtbl.create ~random:false 4)) with
    | Error _ -> failwith "package canary spawn failed"
    | Ok worker ->
        List.iter (fun line -> send_or_fail worker (parse line)) commands;
        send_or_fail worker (Check ("item-beta", 25, root));
        ignore (Actor.receive root_inbox);
        send_or_fail worker Stop) with
  | Ok () -> print_endline "Astring actor package canary: ok"
  | Error Actor.Unsupported_runtime -> failwith "unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "root heap exhausted"
  | Error Actor.Deadlock -> failwith "deadlock"
  | Error (Actor.Root_failed message) -> failwith message
