(* TEST
 modules = "actor_timers_stubs.c";
 set OCAML_ACTOR_TRACE = "actor_timer_trace.ndjson";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)
external clock : int -> unit = "caml_actor_test_timer_clock"
let ok = function Ok x -> x | Error _ -> failwith "timer trace failed"
let contains text needle =
  let rec loop i = i + String.length needle <= String.length text
    && (String.sub text i (String.length needle) = needle || loop (i+1)) in
  loop 0
let () =
  clock 0;
  ok (Actor.run (fun _ ->
    let cancelled = ok (Actor.Timer.after 2.) in
    assert (ok (Actor.Timer.cancel cancelled));
    ok (Actor.Timer.sleep 1.);
    ignore (ok (Actor.Timer.after 100.))));
  let input = open_in_bin (Sys.getenv "OCAML_ACTOR_TRACE") in
  let trace = really_input_string input (in_channel_length input) in
  close_in input;
  List.iter (fun text -> assert (contains trace text)) [
    "\"schema\":2"; "\"timer_limit\":65536";
    "\"action\":\"created\""; "\"action\":\"cancelled\"";
    "\"action\":\"expired\""; "\"action\":\"consumed\"";
    "\"deadline_ns\":\"1000000000\"";
    "\"event\":\"timer_cleanup\""; "\"count\":1";
    "\"complete\":true"
  ];
  print_endline "actor timer trace: ok"
