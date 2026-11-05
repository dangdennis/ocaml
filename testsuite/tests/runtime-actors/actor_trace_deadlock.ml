(* TEST
 set OCAML_ACTOR_TRACE = "actor_trace_deadlock.ndjson";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let read_file path =
  let input = open_in_bin path in
  let length = in_channel_length input in
  let contents = really_input_string input length in
  close_in input;
  contents

let contains text needle =
  let rec search offset =
    offset + String.length needle <= String.length text
    && (String.sub text offset (String.length needle) = needle
        || search (offset + 1))
  in
  search 0

let () =
  begin match Actor.run (fun inbox -> ignore (Actor.receive inbox)) with
  | Error Actor.Deadlock -> ()
  | _ -> failwith "expected deadlock"
  end;
  let trace = read_file (Sys.getenv "OCAML_ACTOR_TRACE") in
  if not (contains trace "\"outcome\":\"deadlock\"") then
    failwith "missing deadlock outcome";
  print_endline "actor trace deadlock: ok"
