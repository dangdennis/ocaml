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

let send_or_fail pid message =
  match Actor.send pid message with
  | Ok () -> ()
  | Error _ -> failwith "send failed"

let spawn_or_fail entry =
  match Actor.spawn entry with
  | Ok pid -> pid
  | Error _ -> failwith "spawn failed"

let word_bytes = Sys.word_size / 8

let check_host_rejection () =
  match Actor.stats () with
  | _ -> failwith "stats unexpectedly available on the host"
  | exception Invalid_argument message ->
      if message <> "Actor.stats outside an actor world" then
        failwith "stats raised the wrong Invalid_argument"
  | exception _ -> failwith "stats raised the wrong host exception"

let check_lifecycle_and_gauges () =
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let blocked = spawn_or_fail (fun child_inbox ->
      ignore (Actor.receive child_inbox)) in
    Actor.yield ();
    let waiting = Actor.stats () in
    assert (waiting.live_actors = 2);
    assert (waiting.runnable_actors = 1);
    assert (waiting.blocked_actors = 1);
    send_or_fail blocked ();
    let queued = Actor.stats () in
    assert (queued.mailbox_messages = 1);
    assert (queued.mailbox_bytes = word_bytes);
    Actor.yield ();
    let after_exit = Actor.stats () in
    assert (after_exit.total_exited = 1);
    assert (after_exit.mailbox_messages = 0);
    assert (after_exit.mailbox_bytes = 0);

    let exiting : unit Actor.pid = spawn_or_fail (fun _ -> ()) in
    send_or_fail exiting ();
    assert ((Actor.stats ()).mailbox_messages = 1);
    Actor.yield ();
    let after_drop = Actor.stats () in
    assert (after_drop.total_exited = 2);
    assert (after_drop.messages_dropped = 1);
    assert (after_drop.mailbox_messages = 0);
    assert (after_drop.mailbox_bytes = 0);

    let doomed = spawn_or_fail (fun _ -> ignore (1 / 0)) in
    ignore doomed;
    Actor.yield ();
    assert ((Actor.stats ()).total_failed = 1);

    ignore (spawn_or_fail (fun _ ->
      for _ = 1 to 20_000 do
        ignore ((Actor.stats ()).total_dispatches)
      done;
      send_or_fail root ()));
    ignore (Actor.receive root_inbox);

    ignore (spawn_or_fail (fun _ ->
      let rec burn remaining =
        if remaining > 0 then burn (remaining - 1)
      in
      burn 100_000;
      send_or_fail root ()));
    Actor.yield ();
    assert ((Actor.stats ()).total_reduction_stops > 0);
    ignore (Actor.receive root_inbox)))

let check_world_reset () =
  require_ok (Actor.run (fun _ ->
    let fresh = Actor.stats () in
    assert (fresh.live_actors = 1);
    assert (fresh.total_spawned = 1);
    assert (fresh.total_exited = 0);
    assert (fresh.total_failed = 0);
    assert (fresh.messages_sent = 0);
    assert (fresh.messages_received = 0);
    assert (fresh.messages_dropped = 0);
    assert (fresh.mailbox_messages = 0);
    assert (fresh.mailbox_bytes = 0);
    assert (fresh.mailbox_quota_failures = 0);
    assert (fresh.current_heap_words = 1 lsl 18);
    assert (fresh.maximum_heap_words = 1 lsl 18);
    assert (fresh.heap_growths = 0);
    assert (fresh.actor_capacity = 1_024);
    assert (fresh.reduction_budget = 1_000);
    assert (fresh.message_word_limit = 1 lsl 16);
    assert (fresh.mailbox_message_limit = 1 lsl 16);
    assert (fresh.mailbox_byte_limit = 1 lsl 28)))

let rec retain_live count tail =
  if count = 0 then tail else retain_live (count - 1) (count :: tail)

let check_configured_limits_and_growth () =
  let config = Actor.{
    Actor.default_world_config with
    root_heap = { initial_words = 64; maximum_words = 8_192 };
    max_actors = 2;
    reductions_per_slice = 37;
    max_message_words = 127;
    max_mailbox_messages = 3;
    max_mailbox_bytes = 8 * word_bytes;
  }
  in
  require_ok (Actor.run_with_config config (fun _root_inbox ->
    let initial = Actor.stats () in
    assert (initial.maximum_heap_words = 8_192);
    assert (initial.heap_growths = 0);
    assert (initial.actor_capacity = 2);
    assert (initial.reduction_budget = 37);
    assert (initial.message_word_limit = 127);
    assert (initial.mailbox_message_limit = 3);
    assert (initial.mailbox_byte_limit = 8 * word_bytes);
    let live = retain_live 1_000 [] in
    if List.length live <> 1_000 then ignore (1 / 0);
    let grown = Actor.stats () in
    assert (grown.current_heap_words > initial.current_heap_words);
    assert (grown.current_heap_words <= grown.maximum_heap_words);
    assert (grown.heap_growths > 0);
    let child = spawn_or_fail (fun inbox -> ignore (Actor.receive inbox)) in
    begin match Actor.send child [0; 1; 2] with
    | Error Actor.Message_too_large -> ()
    | _ -> ignore (1 / 0)
    end;
    let rejected = Actor.stats () in
    assert (rejected.mailbox_messages = 0);
    assert (rejected.mailbox_bytes = 0);
    assert (rejected.mailbox_quota_failures = 1)))

let () =
  check_host_rejection ();
  require_ok (Actor.run (fun root_inbox ->
    let initial = Actor.stats () in
    assert (initial.live_actors = 1);
    assert (initial.total_spawned = 1);
    assert (initial.total_dispatches >= 1);
    let root = Actor.self root_inbox in
    ignore (spawn_or_fail (fun _ -> send_or_fail root ()));
    assert ((Actor.stats ()).total_spawned = 2);
    ignore (Actor.receive root_inbox);
    let final = Actor.stats () in
    assert (final.messages_sent = 1);
    assert (final.messages_received = 1);
    assert (final.mailbox_messages = 0)));
  check_lifecycle_and_gauges ();
  check_configured_limits_and_growth ();
  check_world_reset ();
  print_endline "actor observability: ok"
