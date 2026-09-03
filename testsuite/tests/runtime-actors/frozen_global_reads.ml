(* TEST
 modules = "frozen_global_helper.ml";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

external unsafe_string_get : string -> int -> char = "%string_unsafe_get"

type child_message =
  | Continue
  | Frozen_graph of Frozen_global_helper.graph

let child_reads_globals root inbox =
  assert (Frozen_global_helper.immediate_constant = 37);
  let graph = Frozen_global_helper.immutable_graph in
  assert (graph.left.number = 41);
  assert (graph.left == graph.right);
  assert (graph.loop.next == graph.loop);
  begin match Actor.receive inbox with
  | Frozen_graph copied ->
      assert (copied.left == copied.right);
      assert (copied.left != graph.left);
      assert (copied.loop.next == copied.loop)
  | Continue -> assert false
  end;
  begin match Actor.receive inbox with
  | Continue ->
      begin match Actor.send root () with
      | Ok () -> ()
      | Error _ -> assert false
      end
  | Frozen_graph _ -> assert false
  end

let exercise_child_and_frozen_send root_inbox =
  let root = Actor.self root_inbox in
  match Actor.spawn (child_reads_globals root) with
  | Error _ -> assert false
  | Ok child ->
      (* Layer 10 copies the frozen snapshot into the pointer-free envelope.
         The frozen graph and following local constructor retain FIFO order. *)
      begin match Actor.send child
        (Frozen_graph Frozen_global_helper.immutable_graph) with
      | Ok () -> ()
      | Error _ -> assert false
      end;
      begin match Actor.send child Continue with
      | Ok () -> ()
      | Error _ -> assert false
      end;
      ignore (Actor.receive root_inbox)

let rec churn_private_heap retained remaining checksum =
  if remaining = 0 then checksum
  else
    let cell = remaining, checksum in
    let number, retained_checksum = cell in
    churn_private_heap retained (remaining - 1)
      (retained_checksum + (number land 1))

let root root_inbox =
  assert (Frozen_global_helper.immediate_constant = 37);
  assert (Frozen_global_helper.character_constant = 'g');
  assert (unsafe_string_get Frozen_global_helper.string_constant 0 = 'f');

  let graph = Frozen_global_helper.immutable_graph in
  assert (graph.left.number = 41);
  assert (unsafe_string_get graph.left.text 0 = 's');
  assert (graph.left == graph.right);
  begin match graph.chain with
  | [47; 53; 59] -> ()
  | _ -> assert false
  end;
  assert (graph.loop.mark = 43);
  assert (graph.loop.next == graph.loop);

  assert (Frozen_global_helper.catch_frozen_exception () = 61);
  assert (Frozen_global_helper.even 200);
  assert (Frozen_global_helper.odd 201);

  exercise_child_and_frozen_send root_inbox;

  (* Keep the complete mutually-recursive closure group in an actor-private
     pair across collection.  Whichever member is the infix identity must be
     accepted as a private-GC external edge. *)
  let frozen_group =
    Frozen_global_helper.even, Frozen_global_helper.odd
  in
  let retained = graph, frozen_group in
  let before = Actor.stats () in
  Actor.yield ();
  let checksum = churn_private_heap retained 120_000 0 in
  assert (checksum = 60_000);
  Actor.yield ();
  let after = Actor.stats () in
  assert (after.total_reduction_stops > before.total_reduction_stops);

  (* The global aliases and cycle must remain exact shared identities after
     suspension, several scheduler slices, and a forced private collection. *)
  assert (graph.left == graph.right);
  assert (graph.loop.next == graph.loop);
  assert (Frozen_global_helper.catch_frozen_exception () = 61);
  let retained_graph, (frozen_even, frozen_odd) = retained in
  assert (retained_graph == graph);
  assert (frozen_even == Frozen_global_helper.even);
  assert (frozen_odd == Frozen_global_helper.odd);
  assert (frozen_even 200);
  assert (frozen_odd 201)

let expect_unsupported opcode root =
  match Actor.run root with
  | Error (Actor.Root_failed message)
    when message = "unsupported operation at opcode " ^ opcode -> ()
  | Ok () -> failwith ("unsafe frozen read succeeded at " ^ opcode)
  | Error Actor.Unsupported_runtime -> failwith "unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "root heap exhausted"
  | Error Actor.Deadlock -> failwith "deadlock"
  | Error (Actor.Root_failed message) ->
      failwith ("unexpected frozen read failure: " ^ message)

let custom_global_root _ =
  assert
    (Frozen_global_helper.custom_constant ==
     Frozen_global_helper.custom_constant)

let captured_custom_root _ =
  ignore (Frozen_global_helper.captured_custom ())

let partial_custom_root _ =
  ignore (Frozen_global_helper.partial_custom 1 2)

let ephemeron_global_root _ =
  assert
    (Frozen_global_helper.ephemeron_constant ==
     Frozen_global_helper.ephemeron_constant)

let () =
  match Actor.run root with
  | Ok () ->
      expect_unsupported "GETGLOBALFIELD" custom_global_root;
      expect_unsupported "ENVACC2" captured_custom_root;
      expect_unsupported "RESTART" partial_custom_root;
      expect_unsupported "GETGLOBALFIELD" ephemeron_global_root;
      print_endline "frozen global reads: ok"
  | Error Actor.Unsupported_runtime -> failwith "unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "root heap exhausted"
  | Error Actor.Deadlock -> failwith "deadlock"
  | Error (Actor.Root_failed message) ->
      failwith ("root failed: " ^ message)
