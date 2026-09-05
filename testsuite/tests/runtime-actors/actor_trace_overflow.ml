(* TEST
 set OCAML_ACTOR_TRACE = "actor_trace_overflow.ndjson";
 set OCAML_ACTOR_TRACE_BUFFER_EVENTS = "1";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let read_file path =
  let input = open_in_bin path in
  let contents = really_input_string input (in_channel_length input) in
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
  begin match Actor.run (fun _ ->
    for _ = 1 to 20 do Actor.yield () done)
  with
  | Ok () -> ()
  | Error _ -> failwith "overflow changed actor result"
  end;
  let trace = read_file (Sys.getenv "OCAML_ACTOR_TRACE") in
  if not (contains trace "\"event\":\"world_end\"") then
    failwith "missing trace footer";
  if not (contains trace "\"complete\":false") then
    failwith "overflow was not reported";
  print_endline "actor trace overflow: ok"
