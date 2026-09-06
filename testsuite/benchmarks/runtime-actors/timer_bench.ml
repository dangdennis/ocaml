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

(* Informational bounded timer lifecycle benchmark; no elapsed-time gate. *)
let ok = function Ok x -> x | Error _ -> failwith "timer benchmark failed"
let () =
  let count = match Sys.getenv_opt "ACTOR_TIMER_BENCH_COUNT" with
    | None -> 2000 | Some value -> int_of_string value in
  if count <= 0 || count > 65536 then invalid_arg "ACTOR_TIMER_BENCH_COUNT";
  let started = Sys.time () in
  ok (Actor.run (fun _ ->
    let timers = List.init count (fun _ -> ok (Actor.Timer.after 3600.)) in
    List.iter (fun timer -> assert (ok (Actor.Timer.cancel timer))) timers;
    let stats = Actor.stats () in
    assert (stats.timers = 0 && stats.peak_timers = count
            && stats.timers_cancelled = count)));
  Printf.printf "timers=%d cpu_seconds=%.6f\n" count (Sys.time () -. started)
