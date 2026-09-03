(* TEST
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

let require_ok = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith "unsupported runtime"
  | Error Actor.Root_heap_exhausted -> failwith "root heap exhausted"
  | Error Actor.Deadlock -> failwith "deadlock"
  | Error (Actor.Root_failed message) -> failwith message

let root _inbox =
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

  let source = [| 1; 2; 3; 4 |] in
  let destination = Array.make 5 0 in
  Array.blit source 1 destination 0 3;
  assert (destination = [| 2; 3; 4; 0; 0 |]);
  Array.fill destination 1 3 9;
  assert (destination = [| 2; 9; 9; 9; 0 |]);
  Array.blit destination 0 destination 1 4;
  assert (destination = [| 2; 2; 9; 9; 9 |]);
  Array.unsafe_set destination 4 12;
  assert (Array.unsafe_get destination 4 = 12);

  let table = Hashtbl.create ~random:false 31 in
  assert (Hashtbl.length table = 0);

  for round = 1 to 400 do
    let moving = Array.make 512 round in
    assert (moving.(round land 511) = round)
  done;
  Actor.yield ();
  assert (!(values.(511)) = 11)

let () =
  require_ok (Actor.run root);
  begin match Actor.run (fun _ -> ignore (Array.make 270_000 0)) with
  | Error Actor.Root_heap_exhausted -> ()
  | _ -> failwith "array allocation escaped the actor heap limit"
  end;
  print_endline "primitive array make: ok"
