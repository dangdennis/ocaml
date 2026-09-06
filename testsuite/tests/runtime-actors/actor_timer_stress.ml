(* TEST
 modules = "actor_timers_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)
external clock : int -> unit = "caml_actor_test_timer_clock"
external real : unit -> unit = "caml_actor_test_timer_real"
let ok = function Ok x -> x | Error _ -> failwith "timer stress failed"
let world = function
  | Ok () -> ()
  | Error (Actor.Root_failed s) -> failwith s
  | Error _ -> failwith "timer stress world failed"
let () =
  clock 0;
  world (Actor.run_with_config
    { Actor.default_world_config with max_timers = 8; max_actors = 4 }
    (fun root_inbox ->
      let root = Actor.self root_inbox in
      let _, manager_monitor = ok (Actor.spawn_monitored (fun inbox ->
        let manager = Actor.self inbox in
        let previous = ref None in
        for generation = 1 to 200 do
          let old = !previous in
          let child, monitor = ok (Actor.spawn_monitored (fun _ ->
            (match old with
             | None -> ()
             | Some t ->
               assert (Actor.Timer.await t = Error Actor.Timer.Invalid_timer));
            let timers = List.init 8 (fun _ -> ok (Actor.Timer.after 1.)) in
            assert (Actor.Timer.after 1. = Error Actor.Timer.Timer_limit);
            (* Allocation moves actor roots before the timer await request. *)
            for i = 1 to 1000 do ignore (Bytes.make (i mod 127 + 1) 'x') done;
            let t = List.hd timers in
            ok (Actor.send manager t);
            ok (Actor.Timer.await t))) in
          let token = Actor.receive inbox in
          previous := Some token;
          assert (Actor.Timer.cancel token = Error Actor.Timer.Invalid_timer);
          ok (Actor.cancel child);
          assert (Actor.await_exit monitor = Actor.Cancelled);
          let stats = Actor.stats () in
          assert (stats.timers = 0 && stats.pending_timers = 0
                  && stats.ready_timers = 0 && stats.peak_timers = 8);
          assert (stats.timers_cancelled = generation * 8)
        done;
        ok (Actor.send root ()))) in
      Actor.receive root_inbox;
      assert (Actor.await_exit manager_monitor = Actor.Normal)));
  (* Equal-deadline storms exceed the per-turn promotion batch. *)
  clock 0;
  world (Actor.run (fun _ ->
    let timers = List.init 200 (fun _ -> ok (Actor.Timer.after 1.)) in
    List.iter (fun t -> ok (Actor.Timer.await t)) (List.rev timers);
    let stats = Actor.stats () in
    assert (stats.timers = 0 && stats.timers_expired = 200)));
  (* Timer-owned child failure/restart uses ordinary monitor supervision. *)
  clock 0;
  world (Actor.run (fun _ ->
    let starts = ref 0 in
    let child = Actor.Supervisor.Child {
      id = "timed"; restart = Actor.Supervisor.Transient;
      start = (fun attempt _ ->
        ok (Actor.Timer.sleep 0.001);
        if attempt = 0 then failwith "restart timed child");
      on_start = (fun _ -> incr starts);
      on_exit = (fun _ -> ());
    } in
    ok (Actor.Supervisor.run_one_for_one ~clock:(fun () -> 0)
      ~intensity:{ max_restarts = 3; within = 1 } [child]);
    assert (!starts = 2 && (Actor.stats ()).timers = 0)));
  real ();
  print_endline "actor timer stress: ok"
