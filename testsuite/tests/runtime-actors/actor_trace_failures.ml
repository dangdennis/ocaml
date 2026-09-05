(* TEST
 set OCAML_ACTOR_TRACE = "actor_trace_failures.ndjson";
 set OCAML_ACTOR_TRACE_BUFFER_EVENTS = "4096";
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
  let text_length = String.length text in
  let needle_length = String.length needle in
  let rec search offset =
    offset + needle_length <= text_length
    && (String.sub text offset needle_length = needle || search (offset + 1))
  in
  search 0

let require text needle =
  if not (contains text needle) then failwith ("missing trace field: " ^ needle)

let spawn_or_fail entry =
  match Actor.spawn entry with
  | Ok pid -> pid
  | Error _ -> failwith "spawn failed"

let monitor_or_fail pid =
  match Actor.monitor pid with
  | Ok monitor -> monitor
  | Error _ -> failwith "monitor failed"

let rec retain_live count tail =
  if count = 0 then tail else retain_live (count - 1) (count :: tail)

let () =
  let config =
    Actor.{ default_world_config with max_message_words = 1 }
  in
  begin match Actor.run_with_config config (fun _ ->
    let exhausted =
      match Actor.spawn_with_heap_limits
        Actor.{ initial_words = 64; maximum_words = 128 }
        (fun _ -> ignore (retain_live 1_000 []))
      with
      | Ok pid -> pid
      | Error _ -> failwith "heap-limited spawn failed"
    in
    begin match Actor.await_exit (monitor_or_fail exhausted) with
    | Actor.Heap_limit -> ()
    | _ -> failwith "expected child heap exhaustion"
    end;
    let cancelled = spawn_or_fail (fun inbox -> ignore (Actor.receive inbox)) in
    let cancelled_monitor = monitor_or_fail cancelled in
    begin match Actor.cancel cancelled with
    | Ok () -> ()
    | Error _ -> failwith "cancel failed"
    end;
    begin match Actor.await_exit cancelled_monitor with
    | Actor.Cancelled -> ()
    | _ -> failwith "expected cancelled exit"
    end;
    let quota = spawn_or_fail (fun inbox -> ignore (Actor.receive inbox)) in
    begin match Actor.send quota [0] with
    | Error Actor.Message_too_large -> ()
    | _ -> failwith "expected message quota rejection"
    end;
    begin match Actor.cancel quota with
    | Ok () -> ()
    | Error _ -> failwith "quota cleanup failed"
    end)
  with
  | Ok () -> ()
  | Error _ -> failwith "failure trace world failed"
  end;
  let trace = read_file (Sys.getenv "OCAML_ACTOR_TRACE") in
  require trace "\"reason\":\"heap_limit\"";
  require trace "\"reason\":\"cancelled\"";
  require trace "\"event\":\"send_rejected\"";
  require trace "\"reason\":\"quota\"";
  require trace "\"complete\":true";
  print_endline "actor trace failures: ok"
