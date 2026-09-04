(* TEST
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let require_ok = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith "unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "root heap exhausted"
  | Error Actor.Deadlock -> failwith "deadlock"
  | Error (Actor.Root_failed message) -> failwith message

let spawn_or_fail entry =
  match Actor.spawn entry with
  | Ok pid -> pid
  | Error _ -> failwith "spawn failed"

let monitor_or_fail pid =
  match Actor.monitor pid with
  | Ok monitor -> monitor
  | Error _ -> failwith "monitor failed"

let cancel_or_fail pid =
  match Actor.cancel pid with
  | Ok () -> ()
  | Error _ -> failwith "cancel failed"

let rec register count pid monitors =
  if count = 0 then monitors
  else register (count - 1) pid (monitor_or_fail pid :: monitors)

let rec await_all expected = function
  | [] -> ()
  | monitor :: rest ->
      begin match expected, Actor.await_exit monitor with
      | `Normal, Actor.Normal -> ()
      | `Cancelled, Actor.Cancelled -> ()
      | _ -> failwith "unexpected stress exit"
      end;
      await_all expected rest

let check_reuse_and_fanout () =
  let config = Actor.{
    Actor.default_world_config with
    max_actors = 2;
    max_monitors = 8;
  }
  in
  require_ok (Actor.run_with_config config (fun _ ->
    let rec cycle remaining previous =
      if remaining = 0 then ()
      else
        let child = spawn_or_fail (fun _ -> ()) in
        begin match previous with
        | None -> ()
        | Some stale ->
            assert (((Obj.magic stale : int) land 0xffff)
                    = ((Obj.magic child : int) land 0xffff));
            assert ((Obj.magic stale : int) <> (Obj.magic child : int));
            begin match Actor.monitor stale with
            | Error Actor.Monitor_stale -> ()
            | _ -> failwith "stale monitor retargeted after PID reuse"
            end;
            begin match Actor.cancel stale with
            | Error Actor.Cancel_stale -> ()
            | _ -> failwith "stale cancel retargeted after PID reuse"
            end
        end;
        let monitors = register 8 child [] in
        let expected =
          if remaining land 1 = 0 then begin
            cancel_or_fail child;
            `Cancelled
          end else
            `Normal
        in
        await_all expected monitors;
        let stats = Actor.stats () in
        assert (stats.monitors = 0);
        assert (stats.peak_monitors = 8);
        assert (stats.monitor_quota_failures = 0);
        cycle (remaining - 1) (Some child)
    in
    cycle 200 None));
  print_endline "monitor PID reuse and fanout stress: ok"

let check_watcher_cleanup () =
  let config = Actor.{
    Actor.default_world_config with
    max_actors = 3;
    max_monitors = 8;
  }
  in
  require_ok (Actor.run_with_config config (fun root_inbox ->
    let root = Actor.self root_inbox in
    let target = spawn_or_fail (fun inbox -> ignore (Actor.receive inbox)) in
    ignore (spawn_or_fail (fun _ ->
      ignore (register 8 target []);
      match Actor.send root () with
      | Ok () -> ()
      | Error _ -> failwith "watcher completion send failed"));
    ignore (Actor.receive root_inbox);
    let cleaned = Actor.stats () in
    assert (cleaned.monitors = 0);
    assert (cleaned.peak_monitors = 8);
    cancel_or_fail target));
  print_endline "retired watcher monitor cleanup: ok"

let () =
  check_reuse_and_fanout ();
  check_watcher_cleanup ()
