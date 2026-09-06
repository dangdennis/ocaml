(* TEST
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

module S = Actor.Supervisor

let require_ok context = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith (context ^ ": unsupported")
  | Error Actor.Root_heap_exhausted -> failwith (context ^ ": root heap")
  | Error Actor.Deadlock -> failwith (context ^ ": deadlock")
  | Error (Actor.Root_failed message) -> failwith (context ^ ": " ^ message)

let intensity max_restarts within = S.{ max_restarts; within }

let clock_at value () = value

let check_transient_restart_and_generation () =
  require_ok "transient" (Actor.run (fun _ ->
    let pids : unit Actor.pid list ref = ref [] in
    let exits = ref 0 in
    let child = S.Child {
      id = "worker";
      restart = S.Transient;
      start = (fun attempt _ -> if attempt = 0 then raise Exit);
      on_start = (fun pid -> pids := pid :: !pids);
      on_exit = (fun _ -> incr exits);
    }
    in
    begin match
      S.run_one_for_one ~clock:(clock_at 0)
        ~intensity:(intensity 2 10) [child]
    with
    | Ok () -> ()
    | Error _ -> failwith "transient supervisor failed"
    end;
    assert (!exits = 2);
    match !pids with
    | replacement :: original :: [] ->
        assert (replacement <> original);
        begin match Actor.monitor original with
        | Error Actor.Monitor_stale -> ()
        | _ -> failwith "old child generation revived"
        end
    | _ -> failwith "unexpected transient start count"));
  print_endline "transient restart uses a fresh generation: ok"

let check_restart_policies () =
  require_ok "policies" (Actor.run (fun _ ->
    let temporary_starts = ref 0 in
    let transient_starts = ref 0 in
    let temporary = S.Child {
      id = "temporary";
      restart = S.Temporary;
      start = (fun _ _ -> raise Exit);
      on_start = (fun _ -> incr temporary_starts);
      on_exit = (fun _ -> ());
    }
    in
    let transient = S.Child {
      id = "transient-normal";
      restart = S.Transient;
      start = (fun _ _ -> ());
      on_start = (fun _ -> incr transient_starts);
      on_exit = (fun _ -> ());
    }
    in
    begin match
      S.run_one_for_one ~clock:(clock_at 0)
        ~intensity:(intensity 2 10) [temporary; transient]
    with
    | Ok () -> ()
    | Error _ -> failwith "policy supervisor failed"
    end;
    assert (!temporary_starts = 1);
    assert (!transient_starts = 1)));
  print_endline "temporary and transient policies terminate: ok"

let check_one_for_one_siblings () =
  require_ok "siblings" (Actor.run (fun _ ->
    let first_starts = ref 0 in
    let second_starts = ref 0 in
    let first = S.Child {
      id = "first";
      restart = S.Transient;
      start = (fun attempt _ -> if attempt = 0 then raise Exit);
      on_start = (fun _ -> incr first_starts);
      on_exit = (fun _ -> ());
    }
    in
    let second = S.Child {
      id = "second";
      restart = S.Temporary;
      start = (fun _ _ ->
        Actor.yield ();
        Actor.yield ();
        Actor.yield ());
      on_start = (fun _ -> incr second_starts);
      on_exit = (fun _ -> ());
    }
    in
    begin match
      S.run_one_for_one ~clock:(clock_at 0)
        ~intensity:(intensity 2 10) [first; second]
    with
    | Ok () -> ()
    | Error _ -> failwith "one-for-one supervisor failed"
    end;
    assert (!first_starts = 2);
    assert (!second_starts = 1)));
  print_endline "one-for-one restart preserves healthy siblings: ok"

let check_intensity_and_clock () =
  require_ok "intensity" (Actor.run (fun _ ->
    let starts = ref 0 in
    let child = S.Child {
      id = "loop";
      restart = S.Permanent;
      start = (fun _ _ -> ());
      on_start = (fun _ -> incr starts);
      on_exit = (fun _ -> ());
    }
    in
    begin match
      S.run_one_for_one ~clock:(clock_at 0)
        ~intensity:(intensity 2 10) [child]
    with
    | Error (S.Restart_intensity_exceeded "loop") -> ()
    | _ -> failwith "crash loop was not bounded"
    end;
    assert (!starts = 3)));
  require_ok "aging" (Actor.run (fun _ ->
    let calls = ref 0 in
    let clock () =
      let value = if !calls = 0 then 0 else 2 in
      incr calls;
      value
    in
    let child = S.Child {
      id = "aging";
      restart = S.Transient;
      start = (fun attempt _ -> if attempt < 2 then raise Exit);
      on_start = (fun _ -> ());
      on_exit = (fun _ -> ());
    }
    in
    match S.run_one_for_one ~clock ~intensity:(intensity 1 1) [child] with
    | Ok () -> ()
    | Error _ -> failwith "old restart did not age out"));
  require_ok "backwards clock" (Actor.run (fun _ ->
    let calls = ref 0 in
    let clock () =
      let value = if !calls = 0 then 2 else 1 in
      incr calls;
      value
    in
    let child = S.Child {
      id = "clock";
      restart = S.Permanent;
      start = (fun _ _ -> ());
      on_start = (fun _ -> ());
      on_exit = (fun _ -> ());
    }
    in
    match S.run_one_for_one ~clock ~intensity:(intensity 3 10) [child] with
    | Error S.Clock_moved_backwards -> ()
    | _ -> failwith "backwards clock was accepted"));
  print_endline "restart intensity and monotonic window: ok"

let blocked _ inbox = ignore (Actor.receive inbox)

let check_start_failure_and_reverse_shutdown () =
  let config = Actor.{ Actor.default_world_config with max_actors = 3 } in
  require_ok "initial failure" (Actor.run_with_config config (fun _ ->
    let shutdown = ref [] in
    let child id = S.Child {
      id;
      restart = S.Temporary;
      start = blocked;
      on_start = (fun _ -> ());
      on_exit = (fun reason ->
        begin match reason with
        | Actor.Cancelled -> ()
        | _ -> failwith "shutdown did not cancel child"
        end;
        shutdown := id :: !shutdown);
    }
    in
    begin match
      S.run_one_for_one ~clock:(clock_at 0)
        ~intensity:(intensity 1 1) [child "a"; child "b"; child "c"]
    with
    | Error (S.Initial_start_failed
        ("c", Actor.Monitored_spawn_error Actor.Actor_limit)) -> ()
    | _ -> failwith "initial actor quota failure was not reported"
    end;
    assert (!shutdown = ["a"; "b"]);
    assert ((Actor.stats ()).monitors = 0)));
  print_endline "start failure shuts down in reverse order: ok"

let check_restart_failure () =
  let config = Actor.{ Actor.default_world_config with max_actors = 3 } in
  require_ok "restart failure" (Actor.run_with_config config (fun _ ->
    let first = S.Child {
      id = "failing";
      restart = S.Transient;
      start = (fun _ _ -> raise Exit);
      on_start = (fun _ -> ());
      on_exit = (fun _ ->
        match Actor.spawn (fun _ -> Actor.yield ()) with
        | Ok _ -> ()
        | Error _ -> failwith "filler spawn failed");
    }
    in
    let second = S.Child {
      id = "peer";
      restart = S.Temporary;
      start = (fun _ _ -> Actor.yield ());
      on_start = (fun _ -> ());
      on_exit = (fun _ -> ());
    }
    in
    match
      S.run_one_for_one ~clock:(clock_at 0)
        ~intensity:(intensity 2 10) [first; second]
    with
    | Error (S.Restart_failed
        ("failing", Actor.Monitored_spawn_error Actor.Actor_limit)) -> ()
    | _ -> failwith "restart actor quota failure was not reported"));
  print_endline "restart failure is transactional: ok"

let check_invalid_configuration () =
  require_ok "invalid config" (Actor.run (fun _ ->
    let child id = S.Child {
      id;
      restart = S.Temporary;
      start = (fun _ _ -> ());
      on_start = (fun _ -> ());
      on_exit = (fun _ -> ());
    }
    in
    begin match
      S.run_one_for_one ~clock:(clock_at 0)
        ~intensity:(intensity 0 1) [child "one"]
    with
    | Error (S.Invalid_configuration _) -> ()
    | _ -> failwith "zero restart limit accepted"
    end;
    match
      S.run_one_for_one ~clock:(clock_at 0)
        ~intensity:(intensity 1 1) [child "same"; child "same"]
    with
    | Error (S.Invalid_configuration _) -> ()
    | _ -> failwith "duplicate child id accepted"));
  print_endline "supervisor configuration validation: ok"

let check_supervisor_failure_cleanup () =
  begin match Actor.run (fun _ ->
    let child = S.Child {
      id = "orphan";
      restart = S.Temporary;
      start = blocked;
      on_start = (fun _ -> raise Exit);
      on_exit = (fun _ -> ());
    }
    in
    ignore
      (S.run_one_for_one ~clock:(clock_at 0)
         ~intensity:(intensity 1 1) [child]))
  with
  | Error (Actor.Root_failed _) -> ()
  | _ -> failwith "supervisor callback failure did not fail root"
  end;
  require_ok "world reuse" (Actor.run (fun _ -> ()));
  print_endline "supervisor failure cleans the actor world: ok"

let () =
  check_transient_restart_and_generation ();
  check_restart_policies ();
  check_one_for_one_siblings ();
  check_intensity_and_clock ();
  check_start_failure_and_reverse_shutdown ();
  check_restart_failure ();
  check_invalid_configuration ();
  check_supervisor_failure_cleanup ()
