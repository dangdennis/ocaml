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

let spawn_monitored_or_fail entry =
  match Actor.spawn_monitored entry with
  | Ok pair -> pair
  | Error (Actor.Monitored_spawn_error _) -> failwith "spawn failed"
  | Error Actor.Monitored_monitor_limit -> failwith "monitor limit"

let cancel_or_fail pid =
  match Actor.cancel pid with
  | Ok () -> ()
  | Error _ -> failwith "cancel failed"

let expect_normal = function
  | Actor.Normal -> ()
  | _ -> failwith "expected normal exit"

let check_atomic_spawn_and_wait_order () =
  let config = Actor.{
    Actor.default_world_config with
    max_actors = 4;
    reductions_per_slice = 1;
    max_monitors = 3;
  }
  in
  require_ok "atomic wait" (Actor.run_with_config config (fun root_inbox ->
    let root = Actor.self root_inbox in
    let _, first = spawn_monitored_or_fail (fun _ -> ()) in
    let _, second = spawn_monitored_or_fail (fun _ -> ()) in
    begin match Actor.send root () with
    | Ok () -> ()
    | Error _ -> failwith "self send failed"
    end;
    Actor.yield ();
    let selected, reason = Actor.await_any_exit [second; first] in
    assert (selected = 0);
    expect_normal reason;
    expect_normal (Actor.await_exit first);
    ignore (Actor.receive root_inbox);
    let stats = Actor.stats () in
    assert (stats.monitors = 0);
    assert (stats.total_spawned = 3)));
  print_endline "atomic monitored spawn and ordered exit wait: ok"

let check_monitor_quota_rollback () =
  let config = Actor.{
    Actor.default_world_config with
    max_actors = 4;
    max_monitors = 1;
  }
  in
  require_ok "quota rollback" (Actor.run_with_config config (fun _ ->
    let child, monitor =
      spawn_monitored_or_fail (fun inbox -> ignore (Actor.receive inbox))
    in
    begin match Actor.spawn_monitored (fun _ -> ()) with
    | Error Actor.Monitored_monitor_limit -> ()
    | _ -> failwith "monitor quota did not reject monitored spawn"
    end;
    let stats = Actor.stats () in
    assert (stats.total_spawned = 2);
    assert (stats.live_actors = 2);
    assert (stats.monitors = 1);
    assert (stats.monitor_quota_failures = 1);
    cancel_or_fail child;
    begin match Actor.await_exit monitor with
    | Actor.Cancelled -> ()
    | _ -> failwith "expected cancelled child"
    end));
  print_endline "monitored spawn quota rollback: ok"

let expect_root_failure context entry =
  match Actor.run entry with
  | Error (Actor.Root_failed _) -> ()
  | _ -> failwith (context ^ ": expected fail-closed root")

let check_invalid_wait_sets () =
  expect_root_failure "empty" (fun _ ->
    ignore (Actor.await_any_exit []));
  expect_root_failure "duplicate" (fun _ ->
    let _, monitor = spawn_monitored_or_fail (fun _ -> ()) in
    ignore (Actor.await_any_exit [monitor; monitor]));
  expect_root_failure "consumed" (fun _ ->
    let _, monitor = spawn_monitored_or_fail (fun _ -> ()) in
    expect_normal (Actor.await_exit monitor);
    ignore (Actor.await_any_exit [monitor]));
  print_endline "invalid exit wait sets fail closed: ok"

let check_foreign_wait_set () =
  require_ok "foreign" (Actor.run (fun _ ->
    let target, target_monitor =
      spawn_monitored_or_fail (fun inbox -> ignore (Actor.receive inbox))
    in
    let thief, thief_monitor =
      spawn_monitored_or_fail (fun inbox ->
        let stolen = Actor.receive inbox in
        ignore (Actor.await_any_exit [stolen]))
    in
    begin match Actor.send thief target_monitor with
    | Ok () -> ()
    | Error _ -> failwith "monitor transfer setup failed"
    end;
    begin match Actor.await_exit thief_monitor with
    | Actor.Unsupported_operation _ -> ()
    | _ -> failwith "foreign wait set did not fail closed"
    end;
    cancel_or_fail target;
    begin match Actor.await_exit target_monitor with
    | Actor.Cancelled -> ()
    | _ -> failwith "expected target cancellation"
    end));
  print_endline "foreign exit wait set fails closed: ok"

let () =
  check_atomic_spawn_and_wait_order ();
  check_monitor_quota_rollback ();
  check_invalid_wait_sets ();
  check_foreign_wait_set ()
