(**************************************************************************)
(*                                                                        *)
(*                                 OCaml                                  *)
(*                                                                        *)
(*                             Dennis Dang                                *)
(*                                                                        *)
(*   Copyright 2026 Dennis Dang                                           *)
(*                                                                        *)
(*   All rights reserved.  This file is distributed under the terms of    *)
(*   the GNU Lesser General Public License version 2.1, with the          *)
(*   special exception on linking described in the file LICENSE.          *)
(*                                                                        *)
(**************************************************************************)

let scale =
  match Sys.getenv_opt "ACTOR_BENCH_SCALE" with
  | None -> 1
  | Some value -> max 1 (int_of_string value)

let iterations base = base * scale

let require_ok = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith "unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "root heap exhausted"
  | Error Actor.Deadlock -> failwith "deadlock"
  | Error (Actor.Root_failed message) -> failwith message

let measure name operations workload =
  let started = Sys.time () in
  workload ();
  let elapsed = Sys.time () -. started in
  let rate =
    if elapsed = 0. then infinity
    else float_of_int operations /. elapsed
  in
  Printf.printf "%s\t%d\t%.6f\t%.0f\n%!" name operations elapsed rate

let pure_loop count =
  let rec loop remaining accumulator =
    if remaining = 0 then accumulator
    else loop (remaining - 1) (accumulator + remaining)
  in
  ignore (loop count 0)

let actor_pure_loop count =
  require_ok (Actor.run (fun _ ->
    let rec loop remaining accumulator =
      if remaining = 0 then accumulator
      else loop (remaining - 1) (accumulator + remaining)
    in
    ignore (loop count 0)))

let empty_worlds count =
  for _ = 1 to count do
    require_ok (Actor.run (fun _ -> ()))
  done

let spawn_ack count =
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let send_or_fail pid message =
      match Actor.send pid message with
      | Ok () -> ()
      | Error _ -> failwith "send failed"
    in
    let rec loop remaining =
      if remaining > 0 then begin
        begin match Actor.spawn (fun _ ->
          send_or_fail root ()) with
        | Ok _ -> ()
        | Error _ -> failwith "spawn failed"
        end;
        ignore (Actor.receive root_inbox);
        Actor.yield ();
        loop (remaining - 1)
      end
    in
    loop count))

let ping_pong count =
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let send_or_fail pid message =
      match Actor.send pid message with
      | Ok () -> ()
      | Error _ -> failwith "send failed"
    in
    let child =
      match Actor.spawn (fun child_inbox ->
        let rec loop remaining =
          if remaining > 0 then begin
            ignore (Actor.receive child_inbox);
            send_or_fail root ();
            loop (remaining - 1)
          end
        in
        loop count) with
      | Ok pid -> pid
      | Error _ -> failwith "spawn failed"
    in
    for sequence = 1 to count do
      send_or_fail child sequence;
      ignore (Actor.receive root_inbox)
    done))

let graph_copy count =
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let send_or_fail pid message =
      match Actor.send pid message with
      | Ok () -> ()
      | Error _ -> failwith "send failed"
    in
    let child =
      match Actor.spawn (fun child_inbox ->
        let rec loop remaining =
          if remaining > 0 then begin
            ignore (Actor.receive child_inbox);
            send_or_fail root ();
            loop (remaining - 1)
          end
        in
        loop count) with
      | Ok pid -> pid
      | Error _ -> failwith "spawn failed"
    in
    let rec payload remaining tail =
      if remaining = 0 then tail
      else payload (remaining - 1) (remaining :: tail)
    in
    let message = payload 128 [] in
    for _ = 1 to count do
      send_or_fail child message;
      ignore (Actor.receive root_inbox)
    done))

let () =
  print_endline "benchmark\toperations\tseconds\toperations_per_second";
  let pure_operations = iterations 1_000_000 in
  measure "pure_loop" pure_operations (fun () -> pure_loop pure_operations);
  measure "actor_pure_loop" pure_operations
    (fun () -> actor_pure_loop pure_operations);
  let worlds = iterations 100 in
  measure "empty_world" worlds (fun () -> empty_worlds worlds);
  let spawns = iterations 1_000 in
  measure "spawn_ack" spawns (fun () -> spawn_ack spawns);
  let messages = iterations 10_000 in
  measure "ping_pong_round_trip" messages (fun () -> ping_pong messages);
  let graphs = iterations 1_000 in
  measure "graph_copy_128_round_trip" graphs (fun () -> graph_copy graphs)
