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
let ok = function Ok x -> x | Error _ -> failwith "net stress operation failed"
let world = function
  | Ok () -> ()
  | Error (Actor.Root_failed s) -> failwith s
  | Error _ -> failwith "net stress world failed"
let listen () = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0)
let () =
  setup 2;
  world (Actor.run_with_config
    { Actor.default_world_config with
      root_heap = { initial_words = 512; maximum_words = 512 };
      child_heap = { initial_words = 256; maximum_words = 512 };
      max_actors = 4; max_net_resources = 3; max_net_operations = 1;
      max_net_buffered_bytes = 8 }
    (fun _ ->
      let previous = ref None in
      for _ = 1 to 200 do
        let listener = listen () in
        let socket = ok (Actor.Net.accept listener) in
        (match !previous with
         | None -> ()
         | Some old -> assert (Actor.Net.close old = Error Actor.Net.Invalid_handle));
        previous := Some socket;
        let bytes = Bytes.make 9 'x' in
        assert (Actor.Net.read socket bytes 0 9 = Error Actor.Net.Buffer_limit);
        ignore (ok (Actor.Net.read socket bytes 0 8));
        let worker, monitor = ok (Actor.spawn_monitored (fun inbox ->
          let socket = Actor.receive inbox in
          for _ = 1 to 30 do
            assert (Bytes.length (Bytes.make 40 'g') = 40)
          done;
          ok (Actor.Net.close socket))) in
        ok (Actor.Net.transfer socket worker);
        assert (Actor.Net.close socket = Error Actor.Net.Invalid_handle);
        assert (Actor.await_exit monitor = Actor.Normal);
        ok (Actor.Net.close_listener listener);
        let s = Actor.stats () in
        assert (s.net_resources = 0 && s.net_operations = 0
                && s.net_buffered_bytes = 0)
      done));
  clean ();
  setup 2;
  world (Actor.run_with_config
    { Actor.default_world_config with max_net_resources = 1 }
    (fun _ ->
      let l = listen () in
      assert (Actor.Net.accept l = Error Actor.Net.Descriptor_limit);
      assert ((Actor.stats ()).net_resources = 1);
      ok (Actor.Net.close_listener l)));
  clean ();
  setup 0;
  world (Actor.run_with_config
    { Actor.default_world_config with max_net_operations = 1 }
    (fun inbox ->
      let root = Actor.self inbox in
      let child, monitor = ok (Actor.spawn_monitored (fun _ ->
        let l = listen () in
        ok (Actor.send root ());
        ignore (Actor.Net.accept l))) in
      Actor.receive inbox;
      let l = listen () in
      assert (Actor.Net.accept l = Error Actor.Net.Operation_limit);
      ok (Actor.cancel child);
      assert (Actor.await_exit monitor = Actor.Cancelled);
      ok (Actor.Net.close_listener l)));
  clean ();
  setup 2;
  world (Actor.run (fun _ ->
    let starts = ref 0 in
    let child = Actor.Supervisor.Child {
      id = "listener"; restart = Actor.Supervisor.Transient;
      start = (fun attempt _ ->
        let l = listen () in
        ignore (ok (Actor.Net.accept l));
        if attempt = 0 then failwith "restart network owner");
      on_start = (fun _ -> incr starts);
      on_exit = (fun _ -> assert ((Actor.stats ()).net_resources = 0));
    } in
    ok (Actor.Supervisor.run_one_for_one ~clock:(fun () -> 0)
      ~intensity:{ max_restarts = 2; within = 1 } [child]);
    assert (!starts = 2 && (Actor.stats ()).net_resources = 0)));
  clean ();
  (* Signal arrival inside the wait must abort, clean up, then run handlers
     only after the host world thaws. *)
  setup 7;
  let signals = ref 0 in
  let previous = Sys.signal Sys.sigusr1 (Sys.Signal_handle (fun _ -> incr signals)) in
  (match Actor.run (fun _ -> ignore (Actor.Net.accept (listen ()))) with
   | Error (Actor.Root_failed _) -> ()
   | _ -> failwith "signal wait escaped");
  Gc.minor ();
  assert (!signals = 1);
  Sys.set_signal Sys.sigusr1 previous;
  clean ();
  print_endline "actor net stress: ok"
