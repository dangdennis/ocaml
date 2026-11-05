(* TEST
 modules = "actor_net_stubs.c";
 set OCAML_ACTOR_TRACE = "actor_net_trace.ndjson";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)
external setup : int -> unit = "caml_actor_test_net_setup"
external clean : unit -> unit = "caml_actor_test_net_clean"
let ok = function Ok x -> x | Error _ -> failwith "net trace failed"
let contains text needle =
  let rec loop i = i + String.length needle <= String.length text
    && (String.sub text i (String.length needle) = needle || loop (i+1)) in
  loop 0
let () =
  setup 1;
  ok (Actor.run (fun _ ->
    let l = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
    let socket = ok (Actor.Net.accept l) in
    let bytes = Bytes.create 5 in
    assert (ok (Actor.Net.read socket bytes 0 5) = 3)));
  clean ();
  let input = open_in_bin (Sys.getenv "OCAML_ACTOR_TRACE") in
  let trace = really_input_string input (in_channel_length input) in
  close_in input;
  List.iter (fun text -> assert (contains trace text)) [
    "\"schema\":3"; "\"event\":\"net\"";
    "\"action\":\"listen\""; "\"action\":\"waiting\"";
    "\"action\":\"ready\""; "\"action\":\"accept\"";
    "\"action\":\"read\""; "\"bytes\":3";
    "\"action\":\"cleanup\""; "\"resources\":0,\"operations\":0,\"buffered_bytes\":0";
    "\"complete\":true"
  ];
  List.iter (fun text -> assert (not (contains trace text))) [
    "hello"; "127.0.0.1"; "12345"; "\"fd\":"
  ];
  print_endline "actor net trace: ok"
