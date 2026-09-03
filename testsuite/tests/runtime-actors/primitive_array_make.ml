(* TEST
 modules = "primitive_array_frozen_helper.ml";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

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

let require_ok stage = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith (stage ^ ": unsupported runtime")
  | Error Actor.Root_heap_exhausted -> failwith (stage ^ ": root heap exhausted")
  | Error Actor.Deadlock -> failwith (stage ^ ": deadlock")
  | Error (Actor.Root_failed message) -> failwith (stage ^ ": " ^ message)

let expect_rejected stage root unchanged =
  match Actor.run root with
  | Error (Actor.Root_failed _) -> assert (unchanged ())
  | Error Actor.Unsupported_runtime -> failwith (stage ^ ": unsupported runtime")
  | Error Actor.Root_heap_exhausted -> failwith (stage ^ ": root heap exhausted")
  | Error Actor.Deadlock -> failwith (stage ^ ": deadlock")
  | Ok () -> failwith (stage ^ ": frozen mutation succeeded")

let construction_root _inbox =
  assert (Array.length (Array.make 0 1) = 0);
  let initial = ref 7 in
  let values = Array.make 1024 initial in
  assert (Array.length values = 1024);
  assert (values.(0) == initial);
  values.(511) <- ref 11;
  assert (!(values.(511)) = 11);
  assert (!(values.(0)) = 7);
  let bounds_rejected =
    try
      ignore values.(Array.length values);
      false
    with Invalid_argument _ -> true
  in
  assert bounds_rejected;

  Actor.yield ();
  assert (!(values.(511)) = 11)

let operations_root _inbox =
  let source = [| 1; 2; 3; 4 |] in
  let destination = Array.make 5 0 in
  Array.blit source 1 destination 0 3;
  assert (destination.(0) = 2);
  assert (destination.(1) = 3);
  assert (destination.(2) = 4);
  assert (destination.(3) = 0);
  assert (destination.(4) = 0);
  Array.fill destination 1 3 9;
  assert (destination.(0) = 2);
  assert (destination.(1) = 9);
  assert (destination.(2) = 9);
  assert (destination.(3) = 9);
  assert (destination.(4) = 0);
  Array.blit destination 0 destination 1 4;
  assert (destination.(0) = 2);
  assert (destination.(1) = 2);
  assert (destination.(2) = 9);
  assert (destination.(3) = 9);
  assert (destination.(4) = 9);
  Array.unsafe_set destination 4 12;
  assert (Array.unsafe_get destination 4 = 12)

let frozen_set_root _inbox =
  Primitive_array_frozen_helper.set ()

let frozen_fill_root _inbox =
  Primitive_array_frozen_helper.fill ()

let stdlib_root _inbox =
  let table = Hashtbl.create ~random:false 31 in
  assert (Hashtbl.length table = 0);

  for round = 1 to 400 do
    let moving = Array.make 512 round in
    assert (moving.(round land 511) = round)
  done

let () =
  require_ok "construction" (Actor.run construction_root);
  require_ok "operations" (Actor.run operations_root);
  expect_rejected "frozen set rejection" frozen_set_root
    Primitive_array_frozen_helper.unchanged;
  expect_rejected "frozen fill rejection" frozen_fill_root
    Primitive_array_frozen_helper.unchanged;
  require_ok "stdlib and GC" (Actor.run stdlib_root);
  assert (Primitive_array_frozen_helper.unchanged ());
  begin match Actor.run (fun _ -> ignore (Array.make 270_000 0)) with
  | Error Actor.Root_heap_exhausted -> ()
  | _ -> failwith "array allocation escaped the actor heap limit"
  end;
  print_endline "primitive array make: ok"
