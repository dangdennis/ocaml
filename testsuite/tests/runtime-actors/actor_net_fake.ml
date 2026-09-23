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
external output : unit -> unit = "caml_actor_test_net_output"
let ok = function Ok x -> x | Error _ -> failwith "unexpected net error"
let world = function
  | Ok () -> ()
  | Error (Actor.Root_failed s) -> failwith s
  | Error _ -> failwith "world failed"
let listen () = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0)

let () =
  List.iter (fun mode ->
    setup mode;
    world (Actor.run (fun _ ->
      let l = listen () in
      let cpu = ok (Actor.spawn (fun _ -> while true do Actor.yield () done)) in
      let socket = ok (Actor.Net.accept l) in
      ok (Actor.cancel cpu);
      let bytes = Bytes.make 9 'x' in
      assert (Actor.Net.read socket bytes (-1) 1 = Error Actor.Net.Invalid_argument);
      assert (Actor.Net.read socket bytes 0 10 = Error Actor.Net.Invalid_argument);
      assert (ok (Actor.Net.read socket bytes 9 0) = 0);
      assert (ok (Actor.Net.write socket bytes 9 0) = 0);
      assert (ok (Actor.Net.read socket bytes 2 5) = 3);
      assert (ok (Actor.Net.read socket bytes 5 2) = 2);
      assert (ok (Actor.Net.read socket bytes 7 1) = 0);
      assert (Bytes.to_string bytes = "xxhelloxx");
      let rec write offset remaining =
        if remaining > 0 then
          let count = ok (Actor.Net.write socket bytes offset remaining) in
          assert (count > 0 && count <= remaining);
          write (offset + count) (remaining - count)
      in
      write 2 5;
      ok (Actor.Net.close socket);
      ok (Actor.Net.close_listener l);
      let s = Actor.stats () in
      assert (s.net_resources = 0 && s.net_operations = 0
              && s.net_buffered_bytes = 0)));
    output (); clean ()) [2; 8];
  setup 1;
  world (Actor.run (fun _ ->
    let l = listen () in
    let socket = ok (Actor.Net.accept l) in
    ok (Actor.Net.close socket)));
  clean ();
  setup 0;
  world (Actor.run (fun inbox ->
    let root = Actor.self inbox in
    let child, monitor = ok (Actor.spawn_monitored (fun _ ->
      let l = listen () in
      ok (Actor.send root ());
      ignore (Actor.Net.accept l);
      failwith "cancelled accept returned")) in
    Actor.receive inbox;
    ok (Actor.Timer.sleep 0.01);
    let s = Actor.stats () in
    assert (s.net_operations = 1 && s.net_resources = 2);
    ok (Actor.cancel child);
    assert (Actor.await_exit monitor = Actor.Cancelled);
    assert ((Actor.stats ()).net_resources = 0)));
  clean ();
  setup 2;
  world (Actor.run_with_config
    { Actor.default_world_config with max_mailbox_messages = 1 }
    (fun _ ->
      let _, owner_monitor = ok (Actor.spawn_monitored (fun own_inbox ->
        let me = Actor.self own_inbox in
        let l = listen () in
        let socket = ok (Actor.Net.accept l) in
        assert (Actor.Net.transfer socket me = Error Actor.Net.Transfer_self);
        let destination, monitor = ok (Actor.spawn_monitored (fun inbox ->
          let old = Actor.receive inbox in
          assert (Actor.Net.close old = Error Actor.Net.Invalid_handle);
          ok (Actor.send me old);
          let fresh = Actor.receive inbox in
          assert (fresh <> old);
          ok (Actor.Net.transfer fresh me);
          assert (Actor.Net.close fresh = Error Actor.Net.Invalid_handle))) in
        ok (Actor.send destination socket);
        assert (Actor.Net.transfer socket destination = Error Actor.Net.Mailbox_limit);
        ignore (Actor.receive own_inbox);
        ok (Actor.Net.transfer socket destination);
        assert (Actor.Net.close socket = Error Actor.Net.Invalid_handle);
        let returned = Actor.receive own_inbox in
        assert (returned <> socket);
        assert (Actor.Net.close socket = Error Actor.Net.Invalid_handle);
        ok (Actor.Net.close returned);
        assert (Actor.await_exit monitor = Actor.Normal);
        ok (Actor.Net.close_listener l))) in
      assert (Actor.await_exit owner_monitor = Actor.Normal);
      assert ((Actor.stats ()).net_resources = 0)));
  clean ();
  List.iter (fun mode ->
    setup mode;
    (match Actor.run (fun _ -> ignore (Actor.Net.accept (listen ()))) with
     | Error (Actor.Root_failed _) -> ()
     | _ -> failwith "backend failure escaped");
    clean ()) [3; 4; 5; 6];
  setup 0;
  assert (Actor.run (fun inbox -> ignore (listen ()); Actor.receive inbox)
          = Error Actor.Deadlock);
  clean ();
  print_endline "actor net fake: ok"
