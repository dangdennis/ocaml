(* TEST
 flags = "-g";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let require_ok context = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith (context ^ ": unsupported")
  | Error Actor.Root_heap_exhausted -> failwith (context ^ ": root heap")
  | Error Actor.Deadlock -> failwith (context ^ ": deadlock")
  | Error (Actor.Root_failed message) -> failwith (context ^ ": " ^ message)

let spawn_or_fail entry =
  match Actor.spawn entry with
  | Ok pid -> pid
  | Error _ -> failwith "spawn failed"

let monitor_or_fail pid =
  match Actor.monitor pid with
  | Ok monitor -> monitor
  | Error Actor.Monitor_missing -> failwith "monitor missing"
  | Error Actor.Monitor_stale -> failwith "monitor stale"

let rec retain_live n retained =
  if n = 0 then retained
  else retain_live (n - 1) (n :: retained)

let rec explode depth =
  if depth = 0 then raise (Failure "deep monitor boom")
  else 1 + explode (depth - 1)

let rec repeat_utf8 count text =
  if count = 0 then text
  else repeat_utf8 (count - 1) ("\195\169" ^ text)

let long_utf8 = repeat_utf8 150 ""

let valid_utf8 text =
  let length = String.length text in
  let rec loop offset =
    if offset = length then true
    else
      let lead = Char.code text.[offset] in
      let width =
        if lead < 0x80 then 1
        else if lead >= 0xc2 && lead <= 0xdf then 2
        else if lead >= 0xe0 && lead <= 0xef then 3
        else if lead >= 0xf0 && lead <= 0xf4 then 4
        else 0
      in
      if width = 0 || offset + width > length then false
      else
        let rec continuations index =
          if index = width then loop (offset + width)
          else
            let byte = Char.code text.[offset + index] in
            byte land 0xc0 = 0x80 && continuations (index + 1)
        in
        continuations 1
  in
  loop 0

let expect_normal = function
  | Actor.Normal -> ()
  | _ -> failwith "expected normal exit"

let check_normal_and_fifo () =
  require_ok "normal" (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let child = spawn_or_fail (fun _ -> Actor.yield ()) in
    let monitor = monitor_or_fail child in
    begin match Actor.send root () with
    | Ok () -> ()
    | Error _ -> failwith "self send failed"
    end;
    expect_normal (Actor.await_exit monitor);
    ignore (Actor.receive root_inbox)));
  print_endline "normal monitor and mailbox isolation: ok"

let check_failures () =
  require_ok "failures" (Actor.run (fun _ ->
    let failed = spawn_or_fail (fun _ -> raise (Failure "monitor boom")) in
    begin match Actor.await_exit (monitor_or_fail failed) with
    | Actor.Uncaught_exception { summary; backtrace = Some backtrace } ->
        assert (summary = "Failure(\"monitor boom\")");
        assert (String.length backtrace.text > 0);
        assert (not backtrace.truncated)
    | _ -> failwith "expected exception exit"
    end;

    let deep = spawn_or_fail (fun _ -> ignore (explode 80)) in
    begin match Actor.await_exit (monitor_or_fail deep) with
    | Actor.Uncaught_exception { summary; backtrace = Some backtrace } ->
        assert (summary = "Failure(\"deep monitor boom\")");
        assert (String.length backtrace.text > 0);
        assert backtrace.truncated
    | _ -> failwith "expected truncated exception backtrace"
    end;

    let utf8 = spawn_or_fail (fun _ -> raise (Failure long_utf8)) in
    begin match Actor.await_exit (monitor_or_fail utf8) with
    | Actor.Uncaught_exception { summary; _ } ->
        assert (String.length summary <= 255);
        assert (valid_utf8 summary)
    | _ -> failwith "expected bounded UTF-8 exception summary"
    end;

    let exhausted = spawn_or_fail (fun _ ->
      ignore (retain_live 40_000 [])) in
    begin match Actor.await_exit (monitor_or_fail exhausted) with
    | Actor.Heap_limit -> ()
    | _ -> failwith "expected heap limit"
    end;

    let unsupported = spawn_or_fail (fun _ -> ignore (1 / 0)) in
    begin match Actor.await_exit (monitor_or_fail unsupported) with
    | Actor.Unsupported_operation message ->
        assert (message = "unsupported operation at opcode DIVINT")
    | _ -> failwith "expected unsupported operation"
    end));
  print_endline "structured monitored failures: ok"

let check_identity_and_fanout () =
  require_ok "identity" (Actor.run (fun _ ->
    let child = spawn_or_fail (fun _ -> Actor.yield ()) in
    let first = monitor_or_fail child in
    let second = monitor_or_fail child in
    expect_normal (Actor.await_exit first);
    expect_normal (Actor.await_exit second);
    begin match Actor.monitor child with
    | Error Actor.Monitor_stale -> ()
    | _ -> failwith "retired PID did not become stale"
    end;
    let missing : unit Actor.pid = Obj.magic ((1 lsl 16) lor 7) in
    begin match Actor.monitor missing with
    | Error Actor.Monitor_missing -> ()
    | _ -> failwith "free PID slot did not report missing"
    end));
  print_endline "monitor identity and fanout: ok"

let check_foreign_and_consumed_fail_closed () =
  require_ok "foreign monitor" (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    let target = spawn_or_fail (fun _ -> Actor.yield ()) in
    let target_monitor = monitor_or_fail target in
    let thief = spawn_or_fail (fun thief_inbox ->
      let stolen = Actor.receive thief_inbox in
      ignore (Actor.await_exit stolen)) in
    let thief_monitor = monitor_or_fail thief in
    begin match Actor.send thief target_monitor with
    | Ok () -> ()
    | Error _ -> failwith "monitor transfer setup failed"
    end;
    begin match Actor.await_exit thief_monitor with
    | Actor.Unsupported_operation message ->
        assert (message = "unsupported primitive caml_actor_receive/1")
    | _ -> failwith "foreign monitor did not fail closed"
    end;
    expect_normal (Actor.await_exit target_monitor);

    let consumer = spawn_or_fail (fun _ ->
      let child = spawn_or_fail (fun _ -> ()) in
      let monitor = monitor_or_fail child in
      expect_normal (Actor.await_exit monitor);
      ignore (Actor.await_exit monitor)) in
    begin match Actor.await_exit (monitor_or_fail consumer) with
    | Actor.Unsupported_operation message ->
        assert (message = "unsupported primitive caml_actor_receive/1")
    | _ -> failwith "consumed monitor did not fail closed"
    end;
    ignore root));
  print_endline "foreign and consumed monitors fail closed: ok"

let () =
  check_normal_and_fifo ();
  check_failures ();
  check_identity_and_fanout ();
  check_foreign_and_consumed_fail_closed ()
