(* TEST
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let ok = function Ok x -> x | Error _ -> failwith "unexpected net error"

let world = function
  | Ok () -> ()
  | Error (Actor.Root_failed detail) -> failwith detail
  | Error _ -> failwith "world failed"

let () =
  (try ignore (Actor.Net.listen ~address:"127.0.0.1" ~port:0);
       failwith "host net call succeeded"
   with Invalid_argument _ -> ());
  world (Actor.run (fun _ ->
    List.iter (fun address ->
      assert (Actor.Net.listen ~address ~port:0 = Error Actor.Net.Invalid_argument))
      ["localhost"; "127.0.0.01"; "256.0.0.1"; "1.2.3.4\000x"; "1.2.3.4."];
    assert (Actor.Net.listen ~address:"127.0.0.1" ~port:(-1)
            = Error Actor.Net.Invalid_argument)));
  world (Actor.run (fun _ ->
    let listener = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
    assert (ok (Actor.Net.local_port listener) > 0);
    ok (Actor.Net.close_listener listener);
    assert (Actor.Net.close_listener listener = Error Actor.Net.Invalid_handle)));
  world (Actor.run (fun inbox ->
    let root = Actor.self inbox in
    let foreign = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
    let child, monitor = ok (Actor.spawn_monitored (fun _ ->
      assert (Actor.Net.local_port foreign = Error Actor.Net.Invalid_handle);
      let listener = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
      ok (Actor.send root ());
      ignore (Actor.Net.accept listener);
      failwith "accept unexpectedly completed")) in
    Actor.receive inbox;
    ok (Actor.Net.close_listener foreign);
    ok (Actor.Timer.sleep 0.001);
    assert ((Actor.stats ()).net_operations = 1);
    ok (Actor.cancel child);
    assert (Actor.await_exit monitor = Actor.Cancelled);
    let stats = Actor.stats () in
    assert (stats.net_resources = 0 && stats.net_operations = 0
            && stats.net_buffered_bytes = 0)));
  print_endline "actor net: ok"
