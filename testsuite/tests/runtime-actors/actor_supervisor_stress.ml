(* TEST
 set OCAML_ACTOR_TRACE = "actor_supervisor_stress.ndjson";
 set OCAML_ACTOR_TRACE_BUFFER_EVENTS = "100000";
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

let read_file path =
  let input = open_in_bin path in
  let length = in_channel_length input in
  let contents = really_input_string input length in
  close_in input;
  contents

let contains text needle =
  let text_length = String.length text in
  let needle_length = String.length needle in
  let rec search offset =
    offset + needle_length <= text_length
    && (String.sub text offset needle_length = needle || search (offset + 1))
  in
  search 0

let rec retain_live count tail =
  if count = 0 then tail else retain_live (count - 1) (count :: tail)

let check_restart_reuse_stress () =
  let config = Actor.{
    Actor.default_world_config with
    max_actors = 2;
    max_monitors = 1;
  }
  in
  require_ok "restart stress" (Actor.run_with_config config (fun _ ->
    let starts = ref 0 in
    let exits = ref 0 in
    let first : unit Actor.pid option ref = ref None in
    let previous : unit Actor.pid option ref = ref None in
    let clock_value = ref 0 in
    let clock () =
      let value = !clock_value in
      incr clock_value;
      value
    in
    let child = S.Child {
      id = "churn";
      restart = S.Transient;
      start = (fun attempt _ -> if attempt < 200 then raise Exit);
      on_start = (fun pid ->
        incr starts;
        begin match !first with None -> first := Some pid | Some _ -> () end;
        begin match !previous with
        | None -> ()
        | Some old ->
            let old_raw : int = Obj.magic old in
            let pid_raw : int = Obj.magic pid in
            assert (old_raw land 0xffff = pid_raw land 0xffff);
            assert (old_raw <> pid_raw)
        end;
        previous := Some pid);
      on_exit = (fun _ -> incr exits);
    }
    in
    begin match
      S.run_one_for_one ~clock
        ~intensity:S.{ max_restarts = 200; within = 1_000 } [child]
    with
    | Ok () -> ()
    | Error _ -> failwith "bounded churn did not recover"
    end;
    assert (!starts = 201);
    assert (!exits = 201);
    begin match !first with
    | Some pid ->
        begin match Actor.cancel pid with
        | Error Actor.Cancel_stale -> ()
        | _ -> failwith "first generation was revived"
        end
    | None -> failwith "missing first child"
    end;
    let stats = Actor.stats () in
    assert (stats.total_spawned = 202);
    assert (stats.total_failed = 200);
    assert (stats.monitors = 0);
    assert (stats.peak_monitors = 1)));
  print_endline "supervisor restart and PID reuse stress: ok"

let check_failure_classification () =
  require_ok "failure classification" (Actor.run (fun _ ->
    let heap_starts = ref 0 in
    let unsupported_starts = ref 0 in
    let cancelled_starts = ref 0 in
    let heap = S.Child {
      id = "heap";
      restart = S.Transient;
      start = (fun attempt _ ->
        if attempt = 0 then ignore (retain_live 40_000 []));
      on_start = (fun _ -> incr heap_starts);
      on_exit = (fun _ -> ());
    }
    in
    let unsupported = S.Child {
      id = "unsupported";
      restart = S.Transient;
      start = (fun attempt _ -> if attempt = 0 then ignore (1 / 0));
      on_start = (fun _ -> incr unsupported_starts);
      on_exit = (fun _ -> ());
    }
    in
    let cancelled = S.Child {
      id = "cancelled";
      restart = S.Transient;
      start = (fun _ inbox -> ignore (Actor.receive inbox));
      on_start = (fun pid ->
        incr cancelled_starts;
        match Actor.cancel pid with
        | Ok () -> ()
        | Error _ -> failwith "child cancellation failed");
      on_exit = (fun _ -> ());
    }
    in
    begin match
      S.run_one_for_one ~clock:(fun () -> 0)
        ~intensity:S.{ max_restarts = 4; within = 10 }
        [heap; unsupported; cancelled]
    with
    | Ok () -> ()
    | Error _ -> failwith "failure classification did not converge"
    end;
    assert (!heap_starts = 2);
    assert (!unsupported_starts = 2);
    assert (!cancelled_starts = 1);
    assert ((Actor.stats ()).monitors = 0)));
  print_endline "supervisor structured failure classification: ok"

let () =
  check_restart_reuse_stress ();
  check_failure_classification ();
  let trace = read_file (Sys.getenv "OCAML_ACTOR_TRACE") in
  if not (contains trace "\"event\":\"spawn\"") then
    failwith "supervision trace omitted spawns";
  if not (contains trace "\"reason\":\"heap_limit\"")
     || not (contains trace "\"reason\":\"unsupported\"")
     || not (contains trace "\"reason\":\"cancelled\"") then
    failwith "supervision trace omitted structured failures";
  if not (contains trace "\"complete\":true") then
    failwith "supervision trace incomplete";
  print_endline "supervisor trace remains complete: ok"
