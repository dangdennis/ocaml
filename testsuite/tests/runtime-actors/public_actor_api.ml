(* TEST
 modules = "actor_public_global.ml";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let require_ok = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith "unexpected unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "unexpected root heap limit"
  | Error Actor.Deadlock -> failwith "unexpected deadlock"
  | Error (Actor.Root_failed message) -> failwith message

let check_entry_copy_and_cleanup () =
  for expected = 0 to 7 do
    let captured = ref expected in
    require_ok (Actor.run (fun inbox ->
      ignore (Actor.self inbox);
      incr captured;
      Actor.yield ()));
    assert (!captured = expected)
  done

let check_spawn_copy () =
  require_ok (Actor.run (fun _ ->
    let captured = ref 10 in
    match Actor.spawn (fun _ -> incr captured) with
    | Error _ -> ignore (1 / 0)
    | Ok _ ->
        captured := 20;
        Actor.yield ();
        if !captured <> 20 then ignore (1 / 0)))

let check_recursive_spawn_copy () =
  require_ok (Actor.run (fun _ ->
    let captured = ref 0 in
    let rec first inbox =
      if !captured < 0 then second inbox
    and second inbox =
      if !captured < 0 then first inbox else captured := 1
    in
    match Actor.spawn second with
    | Error _ -> ignore (1 / 0)
    | Ok _ ->
        captured := 2;
        Actor.yield ();
        if !captured <> 2 then ignore (1 / 0)))

let check_global_fence () =
  begin match Actor.run (fun _ -> incr Actor_public_global.state) with
  | Error (Actor.Root_failed _) -> ()
  | _ -> assert false
  end;
  assert (!(Actor_public_global.state) = 7)

let check_unsupported_capture () =
  let custom = Int64.of_int 17 in
  match Actor.run (fun _ -> ignore custom) with
  | Error (Actor.Root_failed _) -> ()
  | _ -> assert false

let check_finalisable_capture () =
  let finalisable = ref 23 in
  Gc.finalise (fun _ -> ()) finalisable;
  match Actor.run (fun _ -> ignore finalisable) with
  | Error (Actor.Root_failed _) -> ()
  | _ -> assert false

let check_initial_heap_limit () =
  let oversized = Array.make 270_000 0 in
  match Actor.run (fun _ -> ignore oversized) with
  | Error Actor.Root_heap_exhausted -> ()
  | _ -> assert false

let check_root_exception () =
  match Actor.run (fun _ -> ignore (1 / 0)) with
  | Error (Actor.Root_failed _) -> ()
  | _ -> assert false

let rec retain_live count tail =
  if count = 0 then tail else retain_live (count - 1) (count :: tail)

let elastic_root = Actor.{ initial_words = 64; maximum_words = 8_192 }
let elastic_child = Actor.{ initial_words = 64; maximum_words = 4_096 }
let tiny_child = Actor.{ initial_words = 64; maximum_words = 128 }
let oversized_child = Actor.{ initial_words = 64; maximum_words = 8_192 }
let invalid_heap = Actor.{ initial_words = 0; maximum_words = 128 }

let check_elastic_heap_limits () =
  require_ok
    (Actor.run_with_heap_limits ~root:elastic_root ~child:elastic_child
       (fun root_inbox ->
         let root = Actor.self root_inbox in
         let live = retain_live 1_000 [] in
         if List.length live <> 1_000 then ignore (1 / 0);
         begin match Actor.spawn (fun _ ->
           let child_live = retain_live 600 [] in
           if List.length child_live <> 600 then ignore (1 / 0);
           ignore (Actor.send root ())) with
         | Error _ -> ignore (1 / 0)
         | Ok _ -> ignore (Actor.receive root_inbox)
         end));
  require_ok
    (Actor.run_with_heap_limits ~root:elastic_root ~child:elastic_child
       (fun _ ->
         begin match Actor.spawn_with_heap_limits tiny_child (fun _ ->
           ignore (retain_live 1_000 [])) with
         | Error _ -> ignore (1 / 0)
         | Ok _ -> Actor.yield ()
         end;
         if (Actor.stats ()).total_failed <> 1 then ignore (1 / 0);
         match Actor.spawn_with_heap_limits invalid_heap (fun _ -> ()) with
         | Error Actor.Initial_heap_limit ->
             begin match
               Actor.spawn_with_heap_limits oversized_child (fun _ -> ())
             with
             | Error Actor.Initial_heap_limit -> ()
             | _ -> ignore (1 / 0)
             end
         | _ -> ignore (1 / 0)));
  begin match
    Actor.run_with_heap_limits ~root:tiny_child ~child:elastic_child
      (fun _ -> ignore (retain_live 1_000 []))
  with
  | Error Actor.Root_heap_exhausted -> ()
  | _ -> assert false
  end;
  begin
    try
      ignore
        (Actor.run_with_heap_limits ~root:invalid_heap ~child:elastic_child
           (fun _ -> ()));
      assert false
    with Invalid_argument _ -> ()
  end

let config_with_limits ~max_actors ~reductions_per_slice ~max_message_words =
  Actor.{
    Actor.default_world_config with
    max_actors;
    reductions_per_slice;
    max_message_words;
  }

let expect_invalid_config config =
  try
    ignore (Actor.run_with_config config (fun _ -> ()));
    assert false
  with Invalid_argument _ -> ()

let check_world_config () =
  assert (Actor.default_world_config.root_heap =
          Actor.default_root_heap_limits);
  assert (Actor.default_world_config.child_heap =
          Actor.default_child_heap_limits);
  assert (Actor.default_world_config.max_actors = 1_024);
  assert (Actor.default_world_config.reductions_per_slice = 1_000);
  assert (Actor.default_world_config.max_message_words = 1 lsl 16);
  assert (Actor.default_world_config.max_mailbox_messages = 1 lsl 16);
  assert (Actor.default_world_config.max_mailbox_bytes = 1 lsl 28);
  let actor_limited =
    config_with_limits ~max_actors:2 ~reductions_per_slice:1_000
      ~max_message_words:(1 lsl 16)
  in
  require_ok (Actor.run_with_config actor_limited (fun _ ->
    begin match Actor.spawn (fun _ -> Actor.yield ()) with
    | Error _ -> ignore (1 / 0)
    | Ok _ -> ()
    end;
    match Actor.spawn (fun _ -> ()) with
    | Error Actor.Actor_limit -> ()
    | _ -> ignore (1 / 0)));
  let short_slices =
    config_with_limits ~max_actors:2 ~reductions_per_slice:1
      ~max_message_words:(1 lsl 16)
  in
  require_ok (Actor.run_with_config short_slices (fun _ ->
    if (Actor.stats ()).total_reduction_stops = 0 then ignore (1 / 0)));
  let long_slices =
    config_with_limits ~max_actors:2 ~reductions_per_slice:100_000
      ~max_message_words:(1 lsl 16)
  in
  require_ok (Actor.run_with_config long_slices (fun _ ->
    if (Actor.stats ()).total_reduction_stops <> 0 then ignore (1 / 0)));
  let message_limited =
    config_with_limits ~max_actors:2 ~reductions_per_slice:1_000
      ~max_message_words:1
  in
  require_ok (Actor.run_with_config message_limited (fun root_inbox ->
    match Actor.spawn (fun inbox -> ignore (Actor.receive inbox)) with
    | Error _ -> ignore (1 / 0)
    | Ok child ->
        begin match Actor.send child [0] with
        | Error Actor.Message_too_large -> ()
        | _ -> ignore (1 / 0)
        end;
        begin match Actor.send child [] with
        | Ok () -> ()
        | Error _ -> ignore (1 / 0)
        end;
        ignore root_inbox));
  expect_invalid_config
    (config_with_limits ~max_actors:1 ~reductions_per_slice:1_000
       ~max_message_words:(1 lsl 16));
  expect_invalid_config
    (config_with_limits ~max_actors:65_537 ~reductions_per_slice:1_000
       ~max_message_words:(1 lsl 16));
  expect_invalid_config
    (config_with_limits ~max_actors:2 ~reductions_per_slice:0
       ~max_message_words:(1 lsl 16));
  expect_invalid_config
    (config_with_limits ~max_actors:2 ~reductions_per_slice:1_000
       ~max_message_words:0);
  expect_invalid_config
    Actor.{ Actor.default_world_config with max_mailbox_messages = 0 };
  expect_invalid_config
    Actor.{ Actor.default_world_config with max_mailbox_bytes = 0 };
  expect_invalid_config
    Actor.{ Actor.default_world_config with root_heap = invalid_heap }

let () =
  check_entry_copy_and_cleanup ();
  print_endline "public actor entry: ok";
  check_spawn_copy ();
  check_recursive_spawn_copy ();
  print_endline "spawn capture copy: ok";
  check_global_fence ();
  print_endline "global mutation fence: ok";
  check_unsupported_capture ();
  check_finalisable_capture ();
  print_endline "capture rejection: ok";
  check_initial_heap_limit ();
  check_root_exception ();
  print_endline "public actor failures: ok";
  check_elastic_heap_limits ();
  print_endline "elastic heap limits: ok";
  check_world_config ();
  print_endline "world configuration: ok"
