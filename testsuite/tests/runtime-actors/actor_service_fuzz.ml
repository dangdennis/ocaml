(* TEST
 modules = "actor_net_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)
external setup : int -> unit = "caml_actor_test_net_setup"
external clean : unit -> unit = "caml_actor_test_net_clean"
let ok = function Ok value -> value | Error _ -> failwith "fuzz operation failed"
let world = function
 | Ok () -> ()
 | Error (Actor.Root_failed text) -> failwith text
 | Error _ -> failwith "fuzz world failed"
let generator () =
 let seed = ref 17421 in
 fun n -> seed := (!seed * 1103515245 + 12345) land 0x3fffffff; !seed mod n

type node = { id : int; mutable left : node option; mutable right : node option;
              payload : bytes }
type message = Graph of node array * int | Bad of (unit -> unit) | Stop

let checksum nodes =
 let edge = function None -> 0 | Some node ->
   assert (node == nodes.(node.id)); node.id + 1 in
 Array.fold_left (fun sum node ->
   sum + node.id + edge node.left + edge node.right + Char.code (Bytes.get node.payload 0))
   0 nodes

let graphs () =
 world (Actor.run_with_config
   { Actor.default_world_config with max_message_words = 4096;
     root_heap = { initial_words = 1024; maximum_words = 8192 };
     child_heap = { initial_words = 1024; maximum_words = 8192 } }
   (fun inbox ->
     let root = Actor.self inbox in
     let worker, monitor = ok (Actor.spawn_monitored (fun inbox ->
       let rec loop () = match Actor.receive inbox with
         | Stop -> ()
         | Bad _ -> assert false
         | Graph (nodes, expected) ->
             assert (checksum nodes = expected);
             Array.iter (fun node -> Bytes.set node.payload 0 '!') nodes;
             ok (Actor.send root expected); loop ()
       in loop ())) in
     let random = generator () in
     for _ = 1 to 300 do
       let nodes = Array.init (1 + random 32) (fun id ->
         { id; left = None; right = None; payload = Bytes.make 1 (Char.chr (65 + id mod 26)) }) in
       Array.iter (fun node ->
         node.left <- Some nodes.(random (Array.length nodes));
         node.right <- if random 3 = 0 then node.left
           else Some nodes.(random (Array.length nodes))) nodes;
       let expected = checksum nodes in
       (match Actor.send worker (Bad (fun () -> ())) with
        | Error (Actor.Unsupported_message _) -> () | _ -> assert false);
       ok (Actor.send worker (Graph (nodes, expected)));
       Array.iter (fun node -> Bytes.set node.payload 0 '?') nodes;
       assert (Actor.receive inbox = expected);
       Array.iter (fun node -> assert (Bytes.get node.payload 0 = '?')) nodes
     done;
     ok (Actor.send worker Stop);
     assert (Actor.await_exit monitor = Actor.Normal);
     assert ((Actor.stats ()).mailbox_messages = 0)))

let resources () =
 setup 2;
 world (Actor.run_with_config
   { Actor.default_world_config with max_net_resources = 2;
     max_net_operations = 2; max_net_buffered_bytes = 16;
     root_heap = { initial_words = 512; maximum_words = 4096 };
     child_heap = { initial_words = 256; maximum_words = 2048 } }
   (fun inbox ->
     let root = Actor.self inbox in
     let random = generator () in
     let previous = ref None in
     for step = 1 to 500 do
       let listener = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
       let socket = ok (Actor.Net.accept listener) in
       (match !previous with None -> () | Some old ->
         assert (Actor.Net.close old = Error Actor.Net.Invalid_handle));
       let current = ref (Some socket) in
       let bytes = Bytes.make 32 'x' in
       (match random 5 with
        | 0 ->
            assert (Actor.Net.read socket bytes (-1) 1 = Error Actor.Net.Invalid_argument);
            ignore (ok (Actor.Net.read socket bytes 0 (random 16)))
        | 1 ->
            assert (Actor.Net.read socket bytes 0 (17 + random 16)
                    = Error Actor.Net.Buffer_limit)
        | action ->
            let return_socket = action = 2 in
            let worker, monitor = ok (Actor.spawn_monitored (fun inbox ->
              if return_socket then (
                let received, metadata = Actor.receive inbox in
                assert (metadata = step);
                ok (Actor.Net.transfer received root)))) in
            (match Actor.Net.transfer_with socket worker step with
             | Error Actor.Net.No_such_actor -> ()
             | Ok () ->
                 assert (Actor.Net.close socket = Error Actor.Net.Invalid_handle);
                 current := if return_socket then Some (Actor.receive inbox) else None
             | Error _ -> assert false);
            if action = 4 then ignore (Actor.cancel worker);
            let reason = Actor.await_exit monitor in
            assert (reason = Actor.Normal || reason = Actor.Cancelled));
       (match !current with None -> () | Some owned -> ok (Actor.Net.close owned));
       ok (Actor.Net.close_listener listener);
       previous := Some socket;
       let stats = Actor.stats () in
       assert (stats.net_resources = 0 && stats.net_operations = 0
               && stats.net_buffered_bytes = 0 && stats.monitors = 0)
     done));
 clean ()

let () =
 graphs (); resources ();
 print_endline "service fuzz: 300 cyclic graphs and 500 resource schedules passed"
