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
 { linux; arch_amd64; bytecode; }
*)

module G = Actor.Gen_server
let ok = function Ok x -> x | Error _ -> failwith "Gen_server failed"
let callbacks = {
  G.init = (fun () -> 0);
  handle_call = (fun amount state -> state + amount, state + amount);
  handle_cast = (fun amount state -> state + amount);
  terminate = (fun _ -> ());
}
let world config entry =
  match Actor.run_with_config config entry with
  | Ok () -> ()
  | Error (Actor.Root_failed s) -> failwith s
  | Error _ -> failwith "Gen_server world failed"
let config = { Actor.default_world_config with
  reductions_per_slice = 97;
  root_heap = { initial_words = 2048; maximum_words = 32768 };
  child_heap = { initial_words = 512; maximum_words = 8192 } }
let clean expected =
  let stats = Actor.stats () in
  assert (stats.live_actors = expected && stats.timers = 0);
  assert (stats.mailbox_messages = 0)
let close server monitor =
  ok (G.stop server); assert (Actor.await_exit monitor = Actor.Normal);
  clean 1; assert ((Actor.stats ()).monitors = 0)
let basic inbox =
  let client = G.client inbox in
  let server, monitor = ok (G.start client callbacks) in
  for i = 1 to 50 do
    ok (G.cast server 1);
    assert (G.call client server 1 ~timeout:1. = Ok (i * 2));
    clean 2; assert ((Actor.stats ()).monitors = 1)
  done;
  List.iter (fun timeout ->
    assert (G.call client server 1 ~timeout = Error G.Invalid_timeout))
    [nan; infinity; neg_infinity; -0.1; 86401.];
  close server monitor
let delayed inbox =
  let client = G.client inbox in
  let callbacks = { callbacks with handle_call = (fun amount state ->
    if amount = -1 then ignore (ok (Actor.Timer.sleep 0.02));
    let state = state + 1 in state, state) } in
  let server, monitor = ok (G.start client callbacks) in
  assert (G.call client server (-1) ~timeout:0.001 = Error G.Timeout);
  (* The first side effect happens, but its late reply cannot satisfy this. *)
  assert (G.call client server 0 ~timeout:1. = Ok 2);
  close server monitor
let failures inbox =
  let client = G.client inbox in
  (match G.start client { callbacks with init = (fun () ->
    failwith "init failure") } with
   | Error (G.Server_down (Actor.Uncaught_exception _)) -> ()
   | _ -> assert false);
  assert ((Actor.stats ()).monitors = 0);
  let server, monitor = ok (G.start client { callbacks with
    handle_call = (fun _ _ -> failwith "call failure") }) in
  (match G.call client server 0 ~timeout:1. with
   | Error (G.Server_down (Actor.Uncaught_exception _)) -> ()
   | _ -> assert false);
  (match Actor.await_exit monitor with
   | Actor.Uncaught_exception _ -> () | _ -> assert false);
  (match G.call client server 0 ~timeout:1. with
   | Error (G.Monitor_failed _) -> () | _ -> assert false);
  clean 1; assert ((Actor.stats ()).monitors = 0)
let unsupported inbox =
  let client = G.client inbox in
  let callbacks = {
    G.init = (fun () -> ());
    handle_call = (fun () () -> (fun () -> ()), ());
    handle_cast = (fun () () -> ()); terminate = (fun () -> ());
  } in
  let server, monitor = ok (G.start client callbacks) in
  (match G.call client server () ~timeout:1. with
   | Error (G.Send_failed (Actor.Unsupported_message _)) -> ()
   | _ -> assert false);
  close server monitor
let quota inbox =
  let client = G.client inbox in
  let server, monitor = ok (G.start client callbacks) in
  (match G.call client server 0 ~timeout:1. with
   | Error (G.Spawn_failed _) | Error (G.Monitor_failed _)
   | Error (G.Helper_failed _) | Error (G.Deadline_failed _) -> ()
   | _ -> assert false);
  close server monitor
let caller_death _ =
  let server, monitor = ok (Actor.spawn_monitored (G.run callbacks)) in
  let server = G.of_pid server in
  let caller, caller_monitor = ok (Actor.spawn_monitored (fun inbox ->
    let client = G.client inbox in
    ignore (G.call client server 0 ~timeout:1.))) in
  (* Cancellation is synchronized to the broker's result delivery: it has
     installed its caller monitor and the deadline actor is live or retired. *)
  let rec wait () =
    if (Actor.stats ()).live_actors < 5 then (Actor.yield (); wait ()) in
  wait ();
  ok (Actor.cancel caller);
  assert (Actor.await_exit caller_monitor = Actor.Cancelled);
  let rec drain () =
    if (Actor.stats ()).live_actors > 2 then (Actor.yield (); drain ()) in
  drain ();
  close server monitor
let concurrent inbox =
  let client = G.client inbox in
  let server, monitor = ok (G.start client callbacks) in
  let rec spawn count monitors =
    if count = 0 then monitors
    else
      let _, child_monitor = ok (Actor.spawn_monitored (fun inbox ->
        let client = G.client inbox in
        for _ = 1 to 10 do
          assert (ok (G.call client server 1 ~timeout:2.) > 0)
        done)) in
      spawn (count - 1) (child_monitor :: monitors)
  in
  List.iter (fun monitor -> assert (Actor.await_exit monitor = Actor.Normal))
    (spawn 8 []);
  assert (G.call client server 0 ~timeout:1. = Ok 80);
  close server monitor
let copied inbox =
  let client = G.client inbox in
  let callbacks = {
    G.init = (fun () -> ());
    handle_call = (fun data () -> Bytes.set data 0 'b'; data, ());
    handle_cast = (fun () () -> ()); terminate = (fun () -> ());
  } in
  let server, monitor = ok (G.start client callbacks) in
  let data = Bytes.make 32 'a' in
  let reply = ok (G.call client server data ~timeout:1.) in
  assert (Bytes.get data 0 = 'a' && Bytes.get reply 0 = 'b');
  Bytes.set reply 0 'c';
  let reply = ok (G.call client server data ~timeout:1.) in
  assert (Bytes.get reply 0 = 'b' && Bytes.get data 0 = 'a');
  close server monitor
let mailbox_pressure inbox =
  let client = G.client inbox in
  let server, monitor = ok (G.start client callbacks) in
  let blocker, blocked_monitor = ok (Actor.spawn_monitored (fun _ ->
    ignore (ok (Actor.Timer.sleep 60.)))) in
  ok (Actor.send blocker ());
  (match G.call client server 0 ~timeout:1. with
   | Error (G.Helper_failed _) -> () | _ -> assert false);
  ok (Actor.cancel blocker);
  assert (Actor.await_exit blocked_monitor = Actor.Cancelled);
  close server monitor
let termination inbox =
  let client = G.client inbox in
  let server, monitor = ok (G.start client { callbacks with
    terminate = (fun _ -> failwith "terminate callback ran") }) in
  ok (G.stop server);
  (match Actor.await_exit monitor with
   | Actor.Uncaught_exception _ -> () | _ -> assert false);
  clean 1; assert ((Actor.stats ()).monitors = 0)
let foreign_client inbox =
  let _, monitor = ok (Actor.spawn_monitored (fun _ ->
    ignore (G.client inbox))) in
  (match Actor.await_exit monitor with
   | Actor.Unsupported_operation _ -> () | _ -> assert false);
  clean 1

let () =
  List.iter (world config) [basic; delayed; failures];
  world config unsupported;
  List.iter (fun max_actors ->
    world { config with max_actors } quota) [2; 3];
  List.iter (fun max_monitors ->
    world { config with max_monitors } quota) [2; 3; 4; 5];
  world { config with max_timers = 1 } (fun inbox ->
    let held = ok (Actor.Timer.after 60.) in
    let client = G.client inbox in
    let server, monitor = ok (G.start client callbacks) in
    (match G.call client server 0 ~timeout:1. with
     | Error (G.Deadline_failed _) -> () | _ -> assert false);
    ignore (ok (Actor.Timer.cancel held));
    close server monitor);
  world config caller_death;
  List.iter (world config) [concurrent; termination; foreign_client];
  world config copied;
  world { config with max_mailbox_messages = 1 } mailbox_pressure;
  world { config with max_actors = 2 } (fun inbox ->
    let client = G.client inbox in
    let blocker, monitor = ok (Actor.spawn_monitored (fun inbox ->
      ignore (Actor.receive inbox))) in
    (match G.start client callbacks with
     | Error (G.Spawn_failed _) -> () | _ -> assert false);
    ok (Actor.cancel blocker);
    assert (Actor.await_exit monitor = Actor.Cancelled);
    clean 1);
  print_endline "typed Gen_server: ok"
