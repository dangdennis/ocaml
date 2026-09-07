(* TEST
 set OCAML_ACTOR_TRACE = "actor_trace.ndjson";
 set OCAML_ACTOR_TRACE_BUFFER_EVENTS = "4096";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let send_or_fail pid message =
  match Actor.send pid message with
  | Ok () -> ()
  | Error _ -> failwith "send failed"

let spawn_or_fail entry =
  match Actor.spawn entry with
  | Ok pid -> pid
  | Error _ -> failwith "spawn failed"

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

let require text needle =
  if not (contains text needle) then failwith ("missing trace field: " ^ needle)

let index_of text needle =
  let rec search offset =
    if offset + String.length needle > String.length text then
      failwith ("missing trace field: " ^ needle)
    else if String.sub text offset (String.length needle) = needle then offset
    else search (offset + 1)
  in
  search 0

let () =
  begin match Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let dropping = spawn_or_fail (fun _ -> ()) in
    send_or_fail dropping ();
    Actor.yield ();
    begin match Actor.send dropping () with
    | Error Actor.No_such_actor -> ()
    | _ -> failwith "stale send was not rejected"
    end;
    let rec churn generations =
      if generations > 0 then begin
        let retired = spawn_or_fail (fun _ -> ()) in
        Actor.yield ();
        begin match Actor.send retired () with
        | Error Actor.No_such_actor -> ()
        | _ -> failwith "reused PID generation was not retired"
        end;
        churn (generations - 1)
      end
    in
    churn 20;
    let child = spawn_or_fail (fun child_inbox ->
      let message = Actor.receive child_inbox in
      if message <> "secret-message" then failwith "message changed";
      send_or_fail root ())
    in
    send_or_fail child "secret-message";
    ignore (Actor.receive root_inbox))
  with
  | Ok () -> ()
  | Error _ -> failwith "actor trace world failed"
  end;
  let trace = read_file (Sys.getenv "OCAML_ACTOR_TRACE") in
  require trace "\"event\":\"world_start\"";
  require trace "\"event\":\"spawn\"";
  require trace "\"event\":\"state\"";
  require trace "\"event\":\"send\"";
  require trace "\"event\":\"send_rejected\"";
  require trace "\"event\":\"receive\"";
  require trace "\"event\":\"drop\"";
  require trace "\"event\":\"exit\"";
  require trace "\"event\":\"world_end\"";
  require trace "\"reason\":\"normal\"";
  require trace "\"complete\":true";
  require trace "\"actor\":131073";
  if index_of trace "\"event\":\"spawn\""
       >= index_of trace "\"event\":\"send\"" then
    failwith "send preceded committed spawn";
  if index_of trace "\"event\":\"exit\""
       >= index_of trace "\"event\":\"drop\"" then
    failwith "drop preceded actor exit";
  if index_of trace "\"event\":\"drop\""
       >= index_of trace "\"event\":\"send_rejected\"" then
    failwith "stale send preceded retirement drop";
  if contains trace "secret-message" then failwith "message payload leaked";
  print_endline "actor trace contract: ok"
