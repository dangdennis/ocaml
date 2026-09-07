(* TEST
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

type cycle = Node of cycle option ref

type preserved = {
  first : int ref;
  second : int ref;
  cycle : cycle;
}

type command = Collect of string

type garbage = Garbage of int * int * garbage option

let require_ok = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith "unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "root heap exhausted"
  | Error Actor.Deadlock -> failwith "deadlock"
  | Error (Actor.Root_failed message) -> failwith message

let rec make_garbage count checksum tail =
  if count = 0 then tail
  else
    make_garbage (count - 1) (checksum + 1)
      (Some (Garbage (count, checksum, tail)))

let check_preserved graph expected =
  if graph.first != graph.second then ignore (1 / 0);
  if !(graph.first) <> expected then ignore (1 / 0);
  match graph.cycle with
  | Node link ->
      begin match !link with
      | Some cycle when cycle == graph.cycle -> ()
      | _ -> ignore (1 / 0)
      end

let rec collect_repeatedly rounds graph expected =
  if rounds = 0 then check_preserved graph expected
  else begin
    let garbage = make_garbage 700 rounds None in
    begin match garbage with
    | Some (Garbage (1, checksum, _)) when checksum = rounds + 699 -> ()
    | _ -> ignore (1 / 0)
    end;
    check_preserved graph expected;
    if rounds mod 7 = 0 then Actor.yield ();
    collect_repeatedly (rounds - 1) graph expected
  end

let rec receive_after_garbage rounds inbox =
  if rounds = 0 then Actor.receive inbox
  else begin
    let garbage = make_garbage 120 rounds None in
    begin match garbage with
    | Some (Garbage (1, checksum, _)) when checksum = rounds + 119 -> ()
    | _ -> ignore (1 / 0)
    end;
    receive_after_garbage (rounds - 1) inbox
  end

let make_preserved value =
  let shared = ref value in
  let link = ref None in
  let cycle = Node link in
  link := Some cycle;
  { first = shared; second = shared; cycle }

let () =
  let payload = String.make 10_000 'x' in
  require_ok (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    match Actor.spawn (fun waiting_inbox ->
      let graph = make_preserved 41 in
      begin match receive_after_garbage 90 waiting_inbox with
      | Collect _ -> ()
      end;
      check_preserved graph 41;
      collect_repeatedly 180 graph 41;
      match Actor.send root () with
      | Ok () -> ()
      | Error _ -> ignore (1 / 0)) with
    | Error _ -> ignore (1 / 0)
    | Ok waiting ->
        let root_graph = make_preserved 73 in
        collect_repeatedly 220 root_graph 73;
        begin match Actor.send waiting (Collect payload) with
        | Ok () -> ()
        | Error _ -> ignore (1 / 0)
        end;
        ignore (Actor.receive root_inbox);
        check_preserved root_graph 73));
  print_endline "private copying GC: ok"
