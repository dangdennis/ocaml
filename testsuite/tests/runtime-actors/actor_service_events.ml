(* TEST
 modules = "actor_net_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)
external setup : int -> unit = "caml_actor_test_net_setup"
external clean : unit -> unit = "caml_actor_test_net_clean"
let ok = function Ok x -> x | Error _ -> failwith "service API operation failed"
let world = function
 | Ok () -> ()
 | Error (Actor.Root_failed s) -> failwith s
 | Error _ -> failwith "service API world failed"
let () =
 world (Actor.run (fun inbox ->
   let root = Actor.self inbox in
   let _, monitor = ok (Actor.spawn_monitored (fun _ ->
     ok (Actor.send root 11); ok (Actor.send root 12))) in
   for _ = 1 to 4 do Actor.yield () done;
   assert (Actor.receive_or_exit inbox [monitor] = Actor.Exit (0, Actor.Normal));
   assert (Actor.receive_or_exit inbox [] = Actor.Message 11);
   assert (Actor.receive_or_exit inbox [] = Actor.Message 12);
   let child, monitor = ok (Actor.spawn_monitored (fun inbox ->
     ignore (Actor.receive inbox))) in
   let _, sender = ok (Actor.spawn_monitored (fun _ ->
     ok (Actor.send root 13))) in
   assert (Actor.receive_or_exit inbox [monitor] = Actor.Message 13);
   assert (Actor.await_exit sender = Actor.Normal);
   ok (Actor.cancel child);
   assert (Actor.receive_or_exit inbox [monitor] = Actor.Exit (0, Actor.Cancelled))));
 (match Actor.run (fun inbox ->
   let _, m = ok (Actor.spawn_monitored (fun _ -> ())) in
   ok (Actor.send (Actor.self inbox) 1);
   ignore (Actor.receive_or_exit inbox [m; m])) with
  | Error (Actor.Root_failed _) -> () | _ -> assert false);
 world (Actor.run (fun inbox ->
   let root = Actor.self inbox in
   ignore (ok (Actor.spawn (fun _ -> ok (Actor.send root 21))));
   assert (Actor.receive_or_exit inbox [] = Actor.Message 21)));
 world (Actor.run (fun _ ->
   let child, monitor = ok (Actor.spawn_monitored (fun inbox ->
     ignore (Actor.receive_or_exit inbox []))) in
   Actor.yield (); ok (Actor.cancel child);
   assert (Actor.await_exit monitor = Actor.Cancelled)));
 (match Actor.run (fun inbox ->
   let root = Actor.self inbox in
   ignore (ok (Actor.spawn (fun foreign -> ok (Actor.send root foreign))));
   let foreign = Actor.receive inbox in
   ignore (Actor.receive_or_exit foreign [])) with
  | Error (Actor.Root_failed _) -> () | _ -> assert false);
 (match Actor.run (fun inbox ->
   let _, monitor = ok (Actor.spawn_monitored (fun _ -> ())) in
   ignore (Actor.await_exit monitor);
   ignore (Actor.receive_or_exit inbox [monitor])) with
  | Error (Actor.Root_failed _) -> () | _ -> assert false);
 setup 2;
 world (Actor.run_with_config
   { Actor.default_world_config with
     root_heap = { initial_words = 512; maximum_words = 2048 };
     child_heap = { initial_words = 256; maximum_words = 2048 } }
   (fun _ ->
   for _ = 1 to 30 do
     let l = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
     let s = ok (Actor.Net.accept l) in
     let data = Bytes.make 40 'a' in
     let worker, monitor = ok (Actor.spawn_monitored (fun inbox ->
       let socket, data = Actor.receive inbox in
       assert (Bytes.get data 0 = 'a');
       ok (Actor.Net.close socket))) in
     ok (Actor.Net.transfer_with s worker data);
     Bytes.set data 0 'b';
     assert (Actor.Net.close s = Error Actor.Net.Invalid_handle);
     assert (Actor.await_exit monitor = Actor.Normal);
     ok (Actor.Net.close_listener l)
   done;
   let stats = Actor.stats () in
   assert (stats.net_resources = 0 && stats.monitors = 0)));
 clean ();
 setup 2;
 world (Actor.run_with_config
   { Actor.default_world_config with max_mailbox_messages = 1 }
   (fun _ ->
     let l = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
     let s = ok (Actor.Net.accept l) in
     let worker, monitor = ok (Actor.spawn_monitored (fun inbox ->
       ignore (Actor.receive inbox))) in
     ok (Actor.send worker (s, 1));
     assert (Actor.Net.transfer_with s worker 2 = Error Actor.Net.Mailbox_limit);
     assert ((Actor.stats ()).mailbox_quota_failures = 1);
     ok (Actor.Net.close s);
     ok (Actor.cancel worker); ignore (Actor.await_exit monitor);
     ok (Actor.Net.close_listener l)));
 clean ();
 setup 2;
 world (Actor.run (fun _ ->
   let l = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
   let s = ok (Actor.Net.accept l) in
   let worker, monitor = ok (Actor.spawn_monitored (fun inbox ->
     ignore (Actor.receive inbox))) in
   assert (Actor.Net.transfer_with s worker (fun () -> ())
           = Error Actor.Net.Invalid_argument);
   ok (Actor.Net.close s);
   ok (Actor.cancel worker); ignore (Actor.await_exit monitor);
   ok (Actor.Net.close_listener l)));
 clean ();
 setup 2;
 world (Actor.run_with_config
   { Actor.default_world_config with max_message_words = 16 }
   (fun _ ->
     let l = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
     let s = ok (Actor.Net.accept l) in
     let worker, monitor = ok (Actor.spawn_monitored (fun inbox ->
       ignore (Actor.receive inbox))) in
     assert (Actor.Net.transfer_with s worker (Bytes.make 256 'q')
             = Error Actor.Net.Mailbox_limit);
     assert ((Actor.stats ()).mailbox_quota_failures = 1);
     ok (Actor.Net.close s);
     ok (Actor.cancel worker); ignore (Actor.await_exit monitor);
     ok (Actor.Net.close_listener l)));
 clean ();
 setup 2;
 world (Actor.run (fun _ ->
   let l = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
   let s = ok (Actor.Net.accept l) in
   let worker, monitor = ok (Actor.spawn_monitored (fun _ -> ())) in
   ok (Actor.Net.transfer_with s worker "unreceived");
   assert (Actor.await_exit monitor = Actor.Normal);
   ok (Actor.Net.close_listener l);
   assert ((Actor.stats ()).net_resources = 0)));
 clean ();
 print_endline "actor service events: ok"
