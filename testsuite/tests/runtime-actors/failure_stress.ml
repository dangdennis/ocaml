(* TEST
 modules = "actor_test_harness.ml";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

open Actor_test_harness

let require_ok = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith "unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "root heap exhausted"
  | Error Actor.Deadlock -> failwith "deadlock"
  | Error (Actor.Root_failed message) -> failwith message

let rec retain_live n retained =
  if n = 0 then retained
  else retain_live (n - 1) (n :: retained)

let rec yield_many n =
  if n > 0 then begin
    Actor.yield ();
    yield_many (n - 1)
  end

let action_name = function
  | 0 -> "exception"
  | 1 -> "heap-exhaustion"
  | _ -> "report"

let make_plan seed count =
  let random = Prng.create seed in
  List.init count (fun index -> index + 1, Prng.int random 8)

let make_trace seed plan =
  trace ~seed (List.mapi (fun step (actor, action) ->
    event ~step ~actor ~op:(action_name action) []) plan)

let check_replay expected =
  match read_replay_from_env () with
  | Error message -> fail ~test:"tests/runtime-actors/failure_stress.ml"
      expected message
  | Ok None -> ()
  | Ok (Some replayed) ->
      begin match replay ~expected ~actual:replayed with
      | Ok () -> ()
      | Error divergence ->
          fail ~test:"tests/runtime-actors/failure_stress.ml" expected
            (string_of_divergence divergence)
      end

let send_or_fail pid message =
  match Actor.send pid message with
  | Ok () -> ()
  | Error _ -> raise Exit

let spawn_or_fail entry =
  match Actor.spawn entry with
  | Ok pid -> pid
  | Error _ -> raise Exit

let rec check_stale = function
  | [] -> ()
  | pid :: rest ->
      begin match Actor.send pid () with
      | Error Actor.No_such_actor -> check_stale rest
      | _ -> raise Exit
      end

let run_plan plan =
  let expected = List.fold_left (fun count (_, action) ->
    if action = 0 then count else count + 1) 0 plan in
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let rec spawn_all pids = function
      | [] -> pids
      | (id, action) :: rest ->
          let pid = spawn_or_fail (fun _ ->
            if action = 0 then raise Exit;
            send_or_fail root ();
            if action = 1 then ignore (retain_live 40_000 [])) in
          spawn_all (pid :: pids) rest
    in
    let pids = spawn_all [] plan in
    for _ = 1 to expected do
      ignore (Actor.receive root_inbox)
    done;
    yield_many 512;
    check_stale pids))

let () =
  let seed =
    match seed_from_env () with
    | Ok seed -> seed
    | Error message -> failwith message
  in
  let plan = make_plan seed 24 in
  let expected_trace = make_trace seed plan in
  check_replay expected_trace;
  begin
    try run_plan plan
    with exn ->
      fail ~test:"tests/runtime-actors/failure_stress.ml" expected_trace
        (Printexc.to_string exn)
  end;
  Printf.printf "seeded failure stress: ok (seed=0x%016Lx)\n" seed
