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

(* TEST
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let require_ok = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith "unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "root heap exhausted"
  | Error Actor.Deadlock -> failwith "deadlock"
  | Error (Actor.Root_failed message) -> failwith message

let spawn_or_fail entry =
  match Actor.spawn entry with
  | Ok pid -> pid
  | Error _ -> failwith "spawn failed"

let send_or_fail pid message =
  match Actor.send pid message with
  | Ok () -> ()
  | Error _ -> failwith "send failed"

let expect_quota pid message =
  match Actor.send pid message with
  | Error Actor.Message_too_large -> ()
  | _ -> failwith "mailbox quota was not enforced"

let word_bytes = Sys.word_size / 8

type cycle = Node of cycle option ref

let config ~messages ~bytes =
  Actor.{
    Actor.default_world_config with
    max_actors = 3;
    max_mailbox_messages = messages;
    max_mailbox_bytes = bytes;
  }

let check_message_limit_and_recovery () =
  require_ok (Actor.run_with_config (config ~messages:1 ~bytes:1_024)
    (fun root_inbox ->
      let root = Actor.self root_inbox in
      let child = spawn_or_fail (fun child_inbox ->
        if Actor.receive child_inbox <> 1 then ignore (1 / 0);
        send_or_fail root ();
        if Actor.receive child_inbox <> 3 then ignore (1 / 0);
        send_or_fail root ())
      in
      send_or_fail child 1;
      expect_quota child 2;
      let full = Actor.stats () in
      assert (full.messages_sent = 1);
      assert (full.mailbox_messages = 1);
      ignore (Actor.receive root_inbox);
      send_or_fail child 3;
      ignore (Actor.receive root_inbox)))

let check_encoded_byte_limit () =
  require_ok (Actor.run_with_config
    (config ~messages:4 ~bytes:word_bytes)
    (fun _ ->
      let child = spawn_or_fail (fun child_inbox ->
        ignore (Actor.receive child_inbox))
      in
      Actor.yield ();
      assert ((Actor.stats ()).blocked_actors = 1);
      expect_quota child [0];
      assert ((Actor.stats ()).mailbox_messages = 0);
      assert ((Actor.stats ()).blocked_actors = 1);
      send_or_fail child []));
  let shared = ref 7 in
  let aliased = (shared, shared) in
  require_ok (Actor.run_with_config
    (config ~messages:4 ~bytes:(6 * word_bytes))
    (fun _ ->
      let child = spawn_or_fail (fun child_inbox ->
        let left, right = Actor.receive child_inbox in
        if left != right || !left <> 7 then ignore (1 / 0))
      in
      send_or_fail child aliased));
  let link = ref None in
  let cycle = Node link in
  link := Some cycle;
  require_ok (Actor.run_with_config
    (config ~messages:4 ~bytes:(7 * word_bytes))
    (fun _ ->
      let child = spawn_or_fail (fun child_inbox ->
        let (Node received_link as received) =
          Actor.receive child_inbox
        in
        match !received_link with
        | Some linked ->
            if linked != received then ignore (1 / 0)
        | None -> ignore (1 / 0))
      in
      send_or_fail child cycle))

let check_retirement_releases_quota () =
  require_ok (Actor.run_with_config
    (config ~messages:1 ~bytes:word_bytes)
    (fun _ ->
      let exiting : unit Actor.pid = spawn_or_fail (fun _ -> ()) in
      send_or_fail exiting ();
      Actor.yield ();
      let after_drop = Actor.stats () in
      assert (after_drop.messages_dropped = 1);
      assert (after_drop.mailbox_messages = 0);
      let current = spawn_or_fail (fun inbox -> ignore (Actor.receive inbox)) in
      send_or_fail current ()))

let check_stale_pid_precedes_quota () =
  require_ok (Actor.run_with_config (config ~messages:1 ~bytes:word_bytes)
    (fun _ ->
      let stale : unit Actor.pid = spawn_or_fail (fun _ -> ()) in
      Actor.yield ();
      let current = spawn_or_fail (fun inbox -> ignore (Actor.receive inbox)) in
      send_or_fail current ();
      match Actor.send stale () with
      | Error Actor.No_such_actor -> ()
      | _ -> failwith "stale PID did not fail closed"))

let () =
  check_message_limit_and_recovery ();
  check_encoded_byte_limit ();
  check_retirement_releases_quota ();
  check_stale_pid_precedes_quota ();
  print_endline "mailbox quotas: ok"
