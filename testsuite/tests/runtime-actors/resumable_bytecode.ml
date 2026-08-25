(* TEST
 modules = "resumable_bytecode_stubs.c";
 flags = "-g";
 ocamlrunparam += ",b=1";
 {
   bytecode;
 }
*)

type 'a observation = ('a, exn * Printexc.raw_backtrace) result

external run_uninterrupted : (unit -> 'a) -> 'a observation
  = "caml_actor_test_run_uninterrupted"

external run_sliced :
  int -> (unit -> 'a) -> 'a observation * int * int * int * int * bool
  = "caml_actor_test_run_sliced"

external allocating_primitive : int -> int
  = "caml_actor_test_allocating_primitive"

external exact_reductions : unit -> unit
  = "caml_actor_test_exact_reductions"

exception Marker of int
exception Uncaught of int
exception Deep_uncaught

let value = function
  | Ok value -> value
  | Error (exn, _) -> raise exn

let make_work () =
  let captured =
    Array.init 64
      (fun i -> ref (String.make (16 + i mod 5) (Char.chr (65 + i))))
  in
  fun () ->
    let total = ref 0 in
    for round = 0 to 399 do
      let scratch = Array.init 17 (fun i -> round + i) in
      total := !total + scratch.(round mod 17)
    done;
    Array.iteri
      (fun i item -> total := !total + String.length !item + i)
      captured;
    !total

let caught () =
  let rec loop acc n =
    if n = 0 then acc
    else
      let next =
        try
          if n mod 19 = 0 then raise (Marker n);
          acc + n
        with Marker marked -> acc + marked * 2
      in
      loop next (n - 1)
  in
  loop 0 250

let uncaught () =
  let rec descend n =
    if n = 0 then raise (Uncaught 42) else 1 + descend (n - 1)
  in
  descend 12

let with_primitive () =
  let total = ref 0 in
  for i = 1 to 80 do
    total := !total + allocating_primitive (20 + i mod 11)
  done;
  !total

let deep_uncaught () =
  let rec descend n =
    if n = 0 then raise Deep_uncaught else 1 + descend (n - 1)
  in
  descend 20_000

let check_successes () =
  let expected = value (run_uninterrupted (make_work ())) in
  List.iter
    (fun budget ->
      let observed, stops, forced_gcs, _, _, _ =
        run_sliced budget (make_work ())
      in
      assert (value observed = expected);
      assert (stops > 0);
      assert (forced_gcs > 0);
      if budget = 1 then assert (stops > 1_000))
    [1; 2; 7; 31]

let check_caught_exception () =
  let expected = value (run_uninterrupted caught) in
  let observed, stops, _, _, _, _ = run_sliced 3 caught in
  assert (value observed = expected);
  assert (stops > 0)

let check_uncaught_exception () =
  let summarize = function
    | Ok _ -> assert false
    | Error (exn, trace) ->
        let entries = Printexc.raw_backtrace_entries trace in
        let inner_length = max 0 (Array.length entries - 2) in
        Printexc.to_string exn,
        Array.length entries,
        Array.sub entries 0 inner_length
  in
  let expected = summarize (run_uninterrupted uncaught) in
  let observed, stops, _, _, _, _ = run_sliced 5 uncaught in
  assert (summarize observed = expected);
  assert (stops > 0)

let check_c_primitive_boundary () =
  let expected = value (run_uninterrupted with_primitive) in
  let observed, stops, _, entries, exits, _ =
    run_sliced 1 with_primitive
  in
  assert (value observed = expected);
  assert (stops > 1_000);
  assert (entries > 0);
  assert (entries = exits)

let check_stack_growth () =
  let observed, stops, _, _, _, stack_grew = run_sliced 17 deep_uncaught in
  begin
    match observed with
    | Error (Deep_uncaught, _) -> ()
    | Error (exn, _) -> raise exn
    | Ok _ -> assert false
  end;
  assert (stops > 0);
  assert stack_grew

let () =
  Printexc.record_backtrace true;
  exact_reductions ();
  check_successes ();
  check_caught_exception ();
  check_uncaught_exception ();
  check_c_primitive_boundary ();
  check_stack_growth ();
  print_endline "resumable bytecode: ok"
