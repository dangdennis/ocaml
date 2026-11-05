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

let ok = function Ok value -> value | Error _ -> failwith
  "unit operation failed"

let counters () = Kv_service.{
  accepted_count = 0; completed_count = 0; failed_count = 0;
  timeout_count = 0; rejected_count = 0; restart_count = 0 }

let context inbox state state_monitor = Kv_service.{
  config = { default_config with max_connections = 1 };
  root = Actor.self inbox; state; state_monitor;
  sessions = []; counters = counters () }

let listener inbox = ignore (Actor.receive inbox : Kv_listener.assignment)
let hold_message _ = ignore (Actor.Timer.sleep 60.)

let exercise at_capacity inbox =
  let state, state_monitor = ok (Actor.spawn_monitored (Kv_state.run
    ~max_entries:1)) in
  let listener, listener_monitor = ok (Actor.spawn_monitored listener) in
  let blocker, blocker_monitor = ok (Actor.spawn_monitored hold_message) in
  let context = context inbox state state_monitor in
  if at_capacity then (
    match Kv_service.prepare_session context with
    | None -> assert false | Some session -> context.sessions <- [session]);
  ok (Actor.send blocker ());
  assert ((Actor.stats ()).mailbox_messages = 1);
  Kv_service.admit context listener;
  assert (Actor.await_exit listener_monitor = Actor.Cancelled);
  Kv_service.stop_sessions context;
  Kv_service.retire blocker blocker_monitor;
  Kv_service.retire state state_monitor;
  assert (context.counters.rejected_count = 1);
  let stats = Actor.stats () in
  assert (stats.live_actors = 1 && stats.monitors = 0 && stats.timers = 0
          && stats.mailbox_messages = 0 && stats.mailbox_quota_failures > 0)

let () =
  List.iter (fun at_capacity ->
    match Actor.run_with_config
      { Actor.default_world_config with max_mailbox_messages = 1;
        root_heap = { initial_words = 4096; maximum_words = 65536 };
        child_heap = { initial_words = 2048; maximum_words = 4096 } }
      (exercise at_capacity) with
    | Ok () -> () | Error _ -> failwith "admission quota cleanup failed")
      [false; true];
  print_endline "admission decision rollback under full mailboxes: ok"
