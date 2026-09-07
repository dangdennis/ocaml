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

module Int_map = Map.Make (Int)
module Int_set = Set.Make (Int)

let require_ok stage = function
  | Ok () -> Printf.printf "stdlib compatibility: %s ok\n" stage
  | Error Actor.Unsupported_runtime ->
      failwith (stage ^ ": unsupported runtime")
  | Error Actor.Root_heap_exhausted ->
      failwith (stage ^ ": root heap exhausted")
  | Error Actor.Deadlock -> failwith (stage ^ ": deadlock")
  | Error (Actor.Root_failed message) -> failwith (stage ^ ": " ^ message)

let lists_and_queues _inbox =
  let values = List.init 32 (fun index -> index + 1) in
  assert (List.length values = 32);
  assert (List.fold_left ( + ) 0 values = 528);
  let queue = Queue.create () in
  List.iter (fun value -> Queue.add value queue) values;
  assert (Queue.take queue = 1);
  assert (Queue.length queue = 31)

let strings_bytes_and_buffers _inbox =
  let text = String.concat ":" ["actor"; "stdlib"; "corpus"] in
  assert (String.length text = 19);
  assert (String.sub text 6 6 = "stdlib");
  let bytes = Bytes.of_string text in
  Bytes.set bytes 0 'A';
  Bytes.fill bytes 5 1 '-';
  assert (Bytes.sub_string bytes 0 12 = "Actor-stdlib");
  let buffer = Buffer.create 8 in
  Buffer.add_string buffer "actor";
  Buffer.add_char buffer ':';
  Buffer.add_substring buffer text 6 6;
  assert (Buffer.contents buffer = "actor:stdlib")

let hash_tables _inbox =
  let table = Hashtbl.create ~random:false 4 in
  for index = 0 to 63 do
    Hashtbl.replace table index (index * index)
  done;
  assert (Hashtbl.length table = 64);
  assert (Hashtbl.find table 12 = 144);
  Hashtbl.remove table 12;
  assert (not (Hashtbl.mem table 12));
  assert (Hashtbl.fold (fun _ value total -> total + value) table 0 > 0)

let maps_and_sets _inbox =
  let map =
    List.fold_left
      (fun map key -> Int_map.add key (key + 10) map)
      Int_map.empty [5; 1; 9; 3]
  in
  assert (Int_map.find 9 map = 19);
  assert (Int_map.cardinal map = 4);
  let set =
    List.fold_left (fun set value -> Int_set.add value set)
      Int_set.empty [3; 1; 4; 1; 5]
  in
  assert (Int_set.elements set = [1; 3; 4; 5])

let integer_conversion _inbox =
  assert (int_of_string "12345" = 12345);
  assert (Int.to_string (-42) = "-42");
  assert (Int.compare 7 9 < 0)

let float_arithmetic _inbox =
  let values = [| 1.25; 2.5; 5.0 |] in
  let total = Array.fold_left ( +. ) 0.0 values in
  assert (total = 8.75)

let float_format _inbox =
  assert (Float.to_string 8.75 = "8.75")

let float_compare _inbox =
  assert (Float.compare 8.75 8.0 > 0)

let () =
  require_ok "lists-and-queues" (Actor.run lists_and_queues);
  require_ok "strings-bytes-and-buffers"
    (Actor.run strings_bytes_and_buffers);
  require_ok "hash-tables" (Actor.run hash_tables);
  require_ok "maps-and-sets" (Actor.run maps_and_sets);
  require_ok "integer-conversion" (Actor.run integer_conversion);
  require_ok "float-arithmetic" (Actor.run float_arithmetic);
  require_ok "float-format" (Actor.run float_format);
  require_ok "float-compare" (Actor.run float_compare)
