(* TEST
 modules = "actor_timers_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)
external clock : int -> unit = "caml_actor_test_timer_clock"
external waits : unit -> int = "caml_actor_test_timer_waits"
external real : unit -> unit = "caml_actor_test_timer_real"
external signal_before_wait : unit -> unit = "caml_actor_test_timer_signal"
external signal_mask : unit -> unit = "caml_actor_test_timer_signal_mask"
let ok = function Ok x -> x | Error _ -> failwith "unexpected error"
let world = function
  | Ok () -> ()
  | Error (Actor.Root_failed detail) -> failwith detail
  | Error _ -> failwith "world failed"
let invalid t = assert (Actor.Timer.await t = Error Actor.Timer.Invalid_timer)

let () =
  clock 0;
  world (Actor.run (fun _ ->
    assert (Actor.Timer.after (-1.) = Error Actor.Timer.Invalid_duration);
    assert (Actor.Timer.after nan = Error Actor.Timer.Invalid_duration);
    assert (Actor.Timer.after infinity = Error Actor.Timer.Invalid_duration);
    assert (Actor.Timer.after 1e100 = Error Actor.Timer.Invalid_duration);
    let t = ok (Actor.Timer.after 1.) in
    assert (ok (Actor.Timer.cancel t)); invalid t;
    assert (Actor.Timer.cancel t = Error Actor.Timer.Invalid_timer);
    let ready = ok (Actor.Timer.after (-0.)) in
    assert (not (ok (Actor.Timer.cancel ready))); invalid ready;
    ok (Actor.Timer.sleep 0.);
    assert ((Actor.stats ()).timers = 0)));
  assert (waits () = 0);
  clock 0;
  world (Actor.run_with_config
    { Actor.default_world_config with max_timers = 2 } (fun _ ->
      let a = ok (Actor.Timer.after 0.) in
      let b = ok (Actor.Timer.after 1.) in
      assert (Actor.Timer.after 1. = Error Actor.Timer.Timer_limit);
      assert ((Actor.stats ()).timers = 2);
      ok (Actor.Timer.await a);
      assert (ok (Actor.Timer.cancel b));
      let s = Actor.stats () in
      assert (s.timers = 0 && s.timer_quota_failures = 1 && s.timer_limit = 2)));
  clock 0;
  world (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let _, m = ok (Actor.spawn_monitored (fun inbox ->
      let me = Actor.self inbox in
      let helper, hm = ok (Actor.spawn_monitored (fun _ ->
        ok (Actor.send me 10); ok (Actor.send me 20))) in
      ignore helper;
      ok (Actor.Timer.sleep 1.);
      assert (Actor.receive inbox = 10);
      assert (Actor.receive inbox = 20);
      assert (Actor.await_exit hm = Actor.Normal);
      assert ((Actor.stats ()).timers = 0);
      ok (Actor.send root ()))) in
    Actor.receive root_inbox;
    assert (Actor.await_exit m = Actor.Normal)));
  assert (waits () = 1);
  clock 1;
  world (Actor.run (fun _ -> ok (Actor.Timer.sleep 1.)));
  assert (waits () = 2);
  clock 0;
  assert (Actor.run (fun inbox ->
    ignore (ok (Actor.Timer.after 1.)); Actor.receive inbox)
    = Error Actor.Deadlock);
  assert (waits () = 0);
  (* Registration, not await, defines the deadline. *)
  clock 0;
  world (Actor.run (fun _ ->
    let t = ok (Actor.Timer.after 1.) in
    ok (Actor.Timer.sleep 2.);
    ok (Actor.Timer.await t)));
  assert (waits () = 1);
  (* Timer polling also happens while an actor remains runnable. *)
  clock 5;
  world (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let cpu = ok (Actor.spawn (fun _ -> while true do Actor.yield () done)) in
    let _, m = ok (Actor.spawn_monitored (fun _ ->
      ok (Actor.Timer.sleep 0.01); ok (Actor.send root ()))) in
    Actor.receive root_inbox;
    ok (Actor.cancel cpu);
    assert (Actor.await_exit m = Actor.Normal)));
  assert (waits () = 0);
  List.iter (fun mode ->
    clock mode;
    match Actor.run (fun _ -> ok (Actor.Timer.sleep 1.)) with
    | Error (Actor.Root_failed _) -> ()
    | _ -> failwith "backend failure was not contained") [2; 3; 4; 6; 7];
  assert (waits () = 64);
  let signals = ref 0 in
  let old_handler = Sys.signal Sys.sigusr1 (Sys.Signal_handle (fun _ ->
    incr signals)) in
  clock 8;
  (match Actor.run (fun _ -> ok (Actor.Timer.sleep 1.)) with
   | Error (Actor.Root_failed _) -> ()
   | _ -> failwith "signal wait was not contained");
  Gc.minor ();
  assert (!signals = 1 && waits () = 1);
  signal_before_wait ();
  (match Actor.run (fun _ -> ok (Actor.Timer.sleep 60.)) with
   | Error (Actor.Root_failed _) -> ()
   | _ -> failwith "kernel signal wait was not contained");
  Gc.minor ();
  assert (!signals = 2);
  signal_mask ();
  Sys.set_signal Sys.sigusr1 old_handler;
  real ();
  world (Actor.run (fun _ -> ok (Actor.Timer.sleep 0.001)));
  print_endline "actor timers: ok"
