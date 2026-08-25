(* TEST
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

type cycle = Node of cycle option ref

type graph = {
  left : int ref;
  right : int ref;
  cycle : cycle;
}

type mixed =
  | Good of int
  | Bad of (unit -> unit)

let require_ok = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith "unexpected unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "unexpected root heap limit"
  | Error Actor.Deadlock -> failwith "unexpected deadlock"
  | Error (Actor.Root_failed message) -> failwith message

let check_fifo_and_wakeup () =
  require_ok (Actor.run (fun inbox ->
    begin match Actor.send (Actor.self inbox) () with
    | Ok () -> ()
    | Error _ -> ignore (1 / 0)
    end;
    ignore (Actor.receive inbox)));
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    match Actor.spawn (fun child_inbox ->
      let first = Actor.receive child_inbox in
      let second = Actor.receive child_inbox in
      if first <> -11 || second <> 22 then ignore (1 / 0);
      match Actor.send root () with
      | Ok () -> ()
      | Error _ -> ignore (1 / 0)) with
    | Error _ -> ignore (1 / 0)
    | Ok child ->
        begin match Actor.send child (-11) with
        | Ok () -> ()
        | Error _ -> ignore (1 / 0)
        end;
        begin match Actor.send child 22 with
        | Ok () -> ()
        | Error _ -> ignore (1 / 0)
        end;
        ignore (Actor.receive root_inbox)))

let check_graph_copy () =
  let shared = ref 17 in
  let link = ref None in
  let cycle = Node link in
  let graph = { left = shared; right = shared; cycle } in
  link := Some cycle;
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    match Actor.spawn (fun child_inbox ->
      let received = Actor.receive child_inbox in
      if received.left != received.right then ignore (1 / 0);
      if !(received.left) <> 17 then ignore (1 / 0);
      begin match received.cycle with
      | Node received_link ->
          begin match !received_link with
          | Some received_cycle ->
              if received_cycle != received.cycle then ignore (1 / 0)
          | None -> ignore (1 / 0)
          end
      end;
      received.left := 99;
      match Actor.send root () with
      | Ok () -> ()
      | Error _ -> ignore (1 / 0)) with
    | Error _ -> ignore (1 / 0)
    | Ok child ->
        begin match Actor.send child graph with
        | Ok () -> ()
        | Error _ -> ignore (1 / 0)
        end;
        shared := 23;
        ignore (Actor.receive root_inbox)));
  assert (!shared = 17)

let check_transactional_rejection () =
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    match Actor.spawn (fun child_inbox ->
      begin match Actor.receive child_inbox with
      | Good 7 -> ()
      | _ -> ignore (1 / 0)
      end;
      match Actor.send root () with
      | Ok () -> ()
      | Error _ -> ignore (1 / 0)) with
    | Error _ -> ignore (1 / 0)
    | Ok child ->
        let captured = ref 0 in
        begin match Actor.send child (Bad (fun () -> captured := 1)) with
        | Error (Actor.Unsupported_message _) -> ()
        | _ -> ignore (1 / 0)
        end;
        begin match Actor.send child (Good (!captured + 7)) with
        | Ok () -> ()
        | Error _ -> ignore (1 / 0)
        end;
        ignore (Actor.receive root_inbox)))

let check_quota_rejection () =
  let large = Array.make 70_000 0 in
  let small = [| 1; 2; 3 |] in
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    match Actor.spawn (fun child_inbox ->
      let received = Actor.receive child_inbox in
      if Array.length received <> 3 then ignore (1 / 0);
      match Actor.send root () with
      | Ok () -> ()
      | Error _ -> ignore (1 / 0)) with
    | Error _ -> ignore (1 / 0)
    | Ok child ->
        begin match Actor.send child large with
        | Error Actor.Message_too_large -> ()
        | _ -> ignore (1 / 0)
        end;
        begin match Actor.send child small with
        | Ok () -> ()
        | Error _ -> ignore (1 / 0)
        end;
        ignore (Actor.receive root_inbox)))

let check_stale_pid () =
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    match Actor.spawn (fun _ -> ()) with
    | Error _ -> ignore (1 / 0)
    | Ok stale ->
        Actor.yield ();
        match Actor.spawn (fun child_inbox ->
          let received = Actor.receive child_inbox in
          if received <> 2 then ignore (1 / 0);
          match Actor.send root () with
          | Ok () -> ()
          | Error _ -> ignore (1 / 0)) with
        | Error _ -> ignore (1 / 0)
        | Ok current ->
            begin match Actor.send stale 1 with
            | Error Actor.No_such_actor -> ()
            | _ -> ignore (1 / 0)
            end;
            begin match Actor.send current 2 with
            | Ok () -> ()
            | Error _ -> ignore (1 / 0)
            end;
            ignore (Actor.receive root_inbox)))

let check_receive_heap_exhaustion () =
  let filler = Array.make 10_000 0 in
  let payload = Array.make 60_000 0 in
  let small = [| 0 |] in
  require_ok (Actor.run (fun _ ->
    match Actor.spawn (fun child_inbox ->
      if Array.length filler < 0 then ignore (1 / 0);
      ignore (Actor.receive child_inbox)) with
    | Error _ -> ignore (1 / 0)
    | Ok child ->
        begin match Actor.send child payload with
        | Ok () -> ()
        | Error _ -> ignore (1 / 0)
        end;
        Actor.yield ();
        match Actor.send child small with
        | Error Actor.No_such_actor -> ()
        | _ -> ignore (1 / 0)))

let check_deadlock () =
  match Actor.run (fun inbox -> ignore (Actor.receive inbox)) with
  | Error Actor.Deadlock -> ()
  | _ -> assert false

let () =
  check_fifo_and_wakeup ();
  print_endline "mailbox FIFO and wakeup: ok";
  check_graph_copy ();
  print_endline "message graph copy: ok";
  check_transactional_rejection ();
  print_endline "unsupported send rejection: ok";
  check_quota_rejection ();
  print_endline "message quota rejection: ok";
  check_stale_pid ();
  check_receive_heap_exhaustion ();
  check_deadlock ();
  print_endline "mailbox lifecycle: ok"
