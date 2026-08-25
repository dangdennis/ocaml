(* TEST
 modules = "actor_public_global.ml";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let require_ok = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith "unexpected unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "unexpected root heap limit"
  | Error Actor.Deadlock -> failwith "unexpected deadlock"
  | Error (Actor.Root_failed message) -> failwith message

let check_entry_copy_and_cleanup () =
  for expected = 0 to 7 do
    let captured = ref expected in
    require_ok (Actor.run (fun inbox ->
      ignore (Actor.self inbox);
      incr captured;
      Actor.yield ()));
    assert (!captured = expected)
  done

let check_spawn_copy () =
  require_ok (Actor.run (fun _ ->
    let captured = ref 10 in
    match Actor.spawn (fun _ -> incr captured) with
    | Error _ -> ignore (1 / 0)
    | Ok _ ->
        captured := 20;
        Actor.yield ();
        if !captured <> 20 then ignore (1 / 0)))

let check_recursive_spawn_copy () =
  require_ok (Actor.run (fun _ ->
    let captured = ref 0 in
    let rec first inbox =
      if !captured < 0 then second inbox
    and second inbox =
      if !captured < 0 then first inbox else captured := 1
    in
    match Actor.spawn second with
    | Error _ -> ignore (1 / 0)
    | Ok _ ->
        captured := 2;
        Actor.yield ();
        if !captured <> 2 then ignore (1 / 0)))

let check_global_fence () =
  begin match Actor.run (fun _ -> incr Actor_public_global.state) with
  | Error (Actor.Root_failed _) -> ()
  | _ -> assert false
  end;
  assert (!(Actor_public_global.state) = 7)

let check_unsupported_capture () =
  let custom = Int64.of_int 17 in
  match Actor.run (fun _ -> ignore custom) with
  | Error (Actor.Root_failed _) -> ()
  | _ -> assert false

let check_finalisable_capture () =
  let finalisable = ref 23 in
  Gc.finalise (fun _ -> ()) finalisable;
  match Actor.run (fun _ -> ignore finalisable) with
  | Error (Actor.Root_failed _) -> ()
  | _ -> assert false

let check_initial_heap_limit () =
  let oversized = Array.make 270_000 0 in
  match Actor.run (fun _ -> ignore oversized) with
  | Error Actor.Root_heap_exhausted -> ()
  | _ -> assert false

let check_root_exception () =
  match Actor.run (fun _ -> ignore (1 / 0)) with
  | Error (Actor.Root_failed _) -> ()
  | _ -> assert false

let () =
  check_entry_copy_and_cleanup ();
  print_endline "public actor entry: ok";
  check_spawn_copy ();
  check_recursive_spawn_copy ();
  print_endline "spawn capture copy: ok";
  check_global_fence ();
  print_endline "global mutation fence: ok";
  check_unsupported_capture ();
  check_finalisable_capture ();
  print_endline "capture rejection: ok";
  check_initial_heap_limit ();
  check_root_exception ();
  print_endline "public actor failures: ok"
