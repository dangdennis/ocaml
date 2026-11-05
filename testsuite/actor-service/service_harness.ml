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

external setup : unit -> unit = "caml_actor_service_setup"
external finish : unit -> unit = "caml_actor_service_finish"

let () =
  let mode = if Array.length Sys.argv > 1 then Sys.argv.(1) else "normal" in
  let config = { Kv_service.default_config with port = 0;
    max_connections = 4; max_entries = 8;
    connection_seconds = (if mode = "timeout" then 1. else 30.);
    max_restarts = 2; test_control = true } in
  let defaults = Kv_service.world_config config in
  let world_config = { defaults with
    child_heap = { initial_words = 2048; maximum_words = 65536 };
    max_actors = (if mode = "quota" then 6 else defaults.max_actors);
    max_net_resources = (if mode = "descriptors" then 2 else
      defaults.max_net_resources);
    max_net_operations = (if mode = "operations" then 1 else
      defaults.max_net_operations);
    max_net_buffered_bytes =
      (if mode = "buffers" then 8 else defaults.max_net_buffered_bytes);
    max_mailbox_bytes = (if mode = "mailboxes" then 16 else
      defaults.max_mailbox_bytes) } in
  setup ();
  (match Actor.run_with_config world_config (fun inbox ->
    let report = Kv_service.serve config inbox in
    (match mode, report with
     | "state", Error (Kv_service.State_failed (Actor.Uncaught_exception _))
       -> ()
     | ("storm" | "descriptors" | "buffers" | "operations" | "mailboxes"),
       Error Kv_service.Restart_limit -> ()
     | ("normal" | "soak" | "timeout" | "restart" | "quota"), Ok summary ->
         assert (summary.accepted > 0);
         if mode = "normal" then assert (summary.failed > 0);
         if mode = "timeout" then assert (summary.timed_out > 0);
         if mode = "restart" then assert (summary.listener_restarts = 1);
         if mode = "quota" then assert (summary.rejected > 0)
     | _, Error (Kv_service.State_failed (Actor.Unsupported_operation name)) ->
         failwith ("state: " ^ name)
     | _ -> failwith "unexpected service result");
    let s = Actor.stats () in
    (if mode = "buffers" || mode = "operations" || mode = "descriptors"
     then assert (s.net_quota_failures > 0));
    (if mode = "mailboxes" then assert (s.mailbox_quota_failures > 0));
    assert (s.live_actors = 1 && s.monitors = 0 && s.timers = 0);
    assert (s.net_resources = 0 && s.net_operations = 0 &&
      s.net_buffered_bytes = 0)) with
   | Ok () -> ()
   | Error (Actor.Root_failed text) -> failwith text
   | Error _ -> failwith "service world failed");
  finish ();
  print_endline "PASS"
