(**************************************************************************)
(*                                                                        *)
(*                                 OCaml                                  *)
(*                                                                        *)
(*                             Dennis Dang                                *)
(*                                                                        *)
(*   Copyright 2026 Dennis Dang                                           *)
(*                                                                        *)
(*   All rights reserved.  This file is distributed under the terms of    *)
(*   the GNU Lesser General Public License version 2.1, with the          *)
(*   special exception on linking described in the file LICENSE.          *)
(*                                                                        *)
(**************************************************************************)

(* TEST
 { linux; arch_amd64; bytecode; }
*)

type graph = { bytes : bytes; mutable cycle : graph option; alias : bytes }
type command = Graph of graph | Churn | Inspect | Stop | Exhaust
let ok = function Ok x -> x | Error _ -> failwith "isolation operation failed"
let rec garbage n tail = if n = 0 then tail else garbage (n - 1) (n :: tail)
let check graph expected =
  assert (graph.bytes == graph.alias);
  assert (Bytes.get graph.bytes 0 = expected);
  match graph.cycle with
  | Some link -> assert (link == graph)
  | None -> assert false
let graph byte =
  let bytes = Bytes.make 32 byte in
  let graph = { bytes; alias = bytes; cycle = None } in
  graph.cycle <- Some graph;
  graph
let worker root inbox =
  let data = match Actor.receive inbox with
    | Graph data -> data | _ -> assert false in
  check data 'a';
  Bytes.set data.bytes 0 'b';
  let rec loop () =
    match Actor.receive inbox with
    | Churn ->
        for _ = 1 to 20 do
          let live = garbage 600 [] in
          assert (List.length live = 600);
          check data 'b'; Actor.yield ()
        done;
        ok (Actor.send root (data, (Actor.stats ()).heap_growths));
        loop ()
    | Inspect ->
        check data 'b'; ok (Actor.send root (data, 0)); loop ()
    | Stop -> ()
    | _ -> assert false
  in loop ()
let spawn root initial maximum =
  let pid = ok (Actor.spawn_with_heap_limits
    { Actor.initial_words = initial; maximum_words = maximum }
    (worker root)) in
  let monitor = ok (Actor.monitor pid) in
  pid, monitor
let exhaust inbox =
  assert (Actor.receive inbox = Exhaust);
  let rec grow tail = grow (1 :: tail) in
  ignore (grow [])
let clean () =
  let stats = Actor.stats () in
  assert (stats.live_actors = 1 && stats.monitors = 0);
  assert (stats.mailbox_messages = 0 && stats.timers = 0)
let run inbox =
  let root = Actor.self inbox in
  let original = graph 'a' in
  let a, ma = spawn root 256 8192 in
  let b, mb = spawn root 512 16384 in
  ok (Actor.send a (Graph original));
  ok (Actor.send b (Graph original));
  Bytes.set original.bytes 0 'z';
  for _ = 1 to 12 do
    ok (Actor.send a Churn);
    ok (Actor.send b Churn);
    for _ = 1 to 2 do
      let copy, growths = Actor.receive inbox in
      check copy 'b'; assert (growths > 0);
      Bytes.set copy.bytes 0 'c'
    done;
    check original 'z'
  done;
  let doomed = ok (Actor.spawn_with_heap_limits
    { Actor.initial_words = 128; maximum_words = 512 } exhaust) in
  let md = ok (Actor.monitor doomed) in
  ok (Actor.send doomed Exhaust);
  assert (Actor.await_exit md = Actor.Heap_limit);
  ok (Actor.send a Inspect); ok (Actor.send b Inspect);
  for _ = 1 to 2 do let copy, _ = Actor.receive inbox in check copy 'b' done;
  ok (Actor.send a Stop); assert (Actor.await_exit ma = Actor.Normal);
  (* Repeated retirement/reuse must not resurrect a dead actor's graph. *)
  for _ = 1 to 20 do
    let child, monitor = spawn root 256 8192 in
    let fresh = graph 'a' in
    ok (Actor.send child (Graph fresh));
    ok (Actor.send child Inspect);
    let copy, _ = Actor.receive inbox in check copy 'b';
    ok (Actor.send child Stop);
    assert (Actor.await_exit monitor = Actor.Normal);
    assert (Actor.send child Inspect = Error Actor.No_such_actor);
    ok (Actor.send b Inspect);
    let copy, _ = Actor.receive inbox in check copy 'b'
  done;
  ok (Actor.send b Stop); assert (Actor.await_exit mb = Actor.Normal);
  check original 'z'; clean ()
let () =
  match Actor.run_with_config
    { Actor.default_world_config with
      reductions_per_slice = 37;
      root_heap = { initial_words = 2048; maximum_words = 32768 };
      child_heap = { initial_words = 256; maximum_words = 16384 } } run with
  | Ok () -> print_endline "heap isolation acceptance: ok"
  | Error (Actor.Root_failed detail) -> failwith detail
  | Error _ -> failwith "isolation world failed"
