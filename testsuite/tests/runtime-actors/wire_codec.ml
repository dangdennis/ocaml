(* TEST
 modules = "wire_codec_stubs.c";
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
  text : string;
  number : float;
  numbers : float array;
}

external exercise : ('a -> unit) -> graph -> int
  = "caml_actor_test_wire_codec"

let () =
  let shared = ref 17 in
  let link = ref None in
  let cycle = Node link in
  link := Some cycle;
  let graph = {
    left = shared;
    right = shared;
    cycle;
    text = Bytes.to_string (Bytes.of_string "actor-wire");
    number = 42.5;
    numbers = [| 1.25; 9.5 |];
  } in
  let entry _ =
    if !(graph.left) < 0 then ignore graph.text
  in
  match exercise entry graph with
  | 0 -> print_endline "pointer-free wire codec: ok"
  | code -> failwith (Printf.sprintf "wire codec seam failed: %d" code)
