(* TEST
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let require_ok context = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith (context ^ ": unsupported")
  | Error Actor.Root_heap_exhausted -> failwith (context ^ ": root heap")
  | Error Actor.Deadlock -> failwith (context ^ ": deadlock")
  | Error (Actor.Root_failed message) -> failwith (context ^ ": " ^ message)

let send_or_fail pid message =
  match Actor.send pid message with
  | Ok () -> ()
  | Error _ -> raise Exit

let spawn_or_fail entry =
  match Actor.spawn entry with
  | Ok pid -> pid
  | Error _ -> raise Exit

let expect_retired pid =
  match Actor.send pid () with
  | Error Actor.No_such_actor -> ()
  | _ -> raise Exit

let rec retain_live n retained =
  if n = 0 then retained
  else retain_live (n - 1) (n :: retained)

let rec yield_many n =
  if n > 0 then begin
    Actor.yield ();
    yield_many (n - 1)
  end

let check_child_exception_isolation () =
  require_ok "child exception" (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let failed = spawn_or_fail (fun _ -> raise Exit) in
    let peer = spawn_or_fail (fun _ -> send_or_fail root ()) in
    ignore (Actor.receive root_inbox);
    expect_retired failed;
    expect_retired peer))

let check_child_exhaustion_isolation () =
  require_ok "child exhaustion" (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let exhausted = spawn_or_fail (fun _ ->
      send_or_fail root ();
      ignore (retain_live 40_000 [])) in
    let peer = spawn_or_fail (fun _ -> send_or_fail root ()) in
    ignore (Actor.receive root_inbox);
    ignore (Actor.receive root_inbox);
    yield_many 512;
    expect_retired exhausted;
    expect_retired peer))

let check_root_shutdown_and_cleanup () =
  begin match Actor.run (fun _ -> raise Exit) with
  | Error (Actor.Root_failed _) -> ()
  | _ -> assert false
  end;
  require_ok "after root exception" (Actor.run (fun _ -> ()));
  begin match Actor.run (fun _ -> ignore (retain_live 140_000 [])) with
  | Error Actor.Root_heap_exhausted -> ()
  | _ -> assert false
  end;
  require_ok "after root exhaustion" (Actor.run (fun _ -> ()));
  begin match Actor.run (fun inbox -> ignore (Actor.receive inbox)) with
  | Error Actor.Deadlock -> ()
  | _ -> assert false
  end;
  require_ok "after deadlock" (Actor.run (fun _ -> ()))

let () =
  check_child_exception_isolation ();
  check_child_exhaustion_isolation ();
  print_endline "child failure containment: ok";
  check_root_shutdown_and_cleanup ();
  print_endline "root shutdown and cleanup: ok"
