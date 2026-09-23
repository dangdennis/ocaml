(* TEST
 modules = "actor_net_loopback_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)
external start_client : int -> unit = "caml_actor_test_net_client"
external join_client : unit -> unit = "caml_actor_test_net_join"
external signal_before_wait : unit -> unit = "caml_actor_test_net_signal"
external signal_clean : unit -> unit = "caml_actor_test_net_signal_clean"
let ok = function Ok x -> x | Error _ -> failwith "loopback operation failed"
let world = function
  | Ok () -> ()
  | Error (Actor.Root_failed s) -> failwith s
  | Error _ -> failwith "loopback world failed"

let () =
  for _ = 1 to 3 do
    start_client 0;
    world (Actor.run_with_config
      { Actor.default_world_config with
        root_heap = { initial_words = 1024; maximum_words = 8192 };
        child_heap = { initial_words = 512; maximum_words = 4096 } }
      (fun inbox ->
        let root = Actor.self inbox in
        let cpu = ok (Actor.spawn (fun _ -> while true do Actor.yield () done)) in
        let _, timer = ok (Actor.spawn_monitored (fun _ ->
          ok (Actor.Timer.sleep 0.001); ok (Actor.send root ()))) in
        let listener = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
        let socket = ok (Actor.Net.accept listener) in
        let worker, monitor = ok (Actor.spawn_monitored (fun inbox ->
          let socket = Actor.receive inbox in
          let bytes = Bytes.make 3 'x' in
          let rec write offset length =
            if length > 0 then
              let n = ok (Actor.Net.write socket bytes offset length) in
              assert (n > 0); write (offset + n) (length - n)
          in
          let rec echo () =
            let n = ok (Actor.Net.read socket bytes 0 3) in
            if n <> 0 then (write 0 n; echo ())
          in
          echo ();
          ok (Actor.Net.close socket))) in
        ok (Actor.Net.transfer socket worker);
        assert (Actor.Net.close socket = Error Actor.Net.Invalid_handle);
        Actor.receive inbox;
        assert (Actor.await_exit timer = Actor.Normal);
        assert (Actor.await_exit monitor = Actor.Normal);
        ok (Actor.cancel cpu);
        ok (Actor.Net.close_listener listener);
        let stats = Actor.stats () in
        assert (stats.net_resources = 0 && stats.net_operations = 0
                && stats.net_buffered_bytes = 0)));
    join_client ()
  done;
  start_client 1;
  world (Actor.run (fun _ ->
    let listener = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
    let socket = ok (Actor.Net.accept listener) in
    let request = Bytes.create 5 in
    let rec receive offset =
      if offset < 5 then
        let count = ok (Actor.Net.read socket request offset (5 - offset)) in
        assert (count > 0); receive (offset + count)
    in
    receive 0;
    let bytes = Bytes.init 65536 (fun i -> Char.chr (i mod 251)) in
    let rec write offset =
      if offset < 65536 then
        let count = ok (Actor.Net.write socket bytes offset (65536 - offset)) in
        assert (count > 0); write (offset + count)
    in
    for _ = 1 to 4 do write 0 done;
    ok (Actor.Net.close socket);
    ok (Actor.Net.close_listener listener)));
  join_client ();
  let signals = ref 0 in
  let previous = Sys.signal Sys.sigusr1
    (Sys.Signal_handle (fun _ -> incr signals)) in
  signal_before_wait ();
  (match Actor.run (fun _ ->
    let listener = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
    ignore (Actor.Net.accept listener)) with
   | Error (Actor.Root_failed _) -> ()
   | _ -> failwith "real signal wait escaped");
  Gc.minor ();
  assert (!signals = 1);
  signal_clean ();
  Sys.set_signal Sys.sigusr1 previous;
  print_endline "actor net loopback: ok"
