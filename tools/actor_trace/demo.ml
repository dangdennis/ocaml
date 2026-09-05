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

let spawn_or_fail entry =
  match Actor.spawn entry with
  | Ok pid -> pid
  | Error _ -> failwith "spawn failed"

let worker root inbox =
  let words = Actor.receive inbox in
  let retained = List.init words Fun.id in
  if List.length retained <> words then failwith "work failed";
  Actor.yield ();
  send_or_fail root ()

let root inbox =
  let root_pid = Actor.self inbox in
  let workers = List.init 200 (fun _ -> spawn_or_fail (worker root_pid)) in
  List.iteri
    (fun index pid -> send_or_fail pid ((index + 1) * 10))
    workers;
  List.iter (fun _ -> ignore (Actor.receive inbox)) workers

let () =
  let config = Actor.{
    default_world_config with
    root_heap = { initial_words = 128; maximum_words = 16_384 };
    child_heap = { initial_words = 128; maximum_words = 16_384 };
    reductions_per_slice = 10_000;
  }
  in
  match Actor.run_with_config config root with
  | Ok () -> print_endline "actor trace demo completed"
  | Error Actor.Unsupported_runtime ->
      prerr_endline "needs Linux amd64 bytecode"
  | Error Actor.Root_heap_exhausted -> prerr_endline "root heap exhausted"
  | Error Actor.Deadlock -> prerr_endline "actor world deadlocked"
  | Error (Actor.Root_failed message) -> prerr_endline message
