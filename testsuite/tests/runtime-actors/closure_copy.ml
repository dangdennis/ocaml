(* TEST
 modules = "closure_copy_stubs.c";
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
}

external exercise :
  ('a -> unit) -> graph -> ('a -> int -> bool) ->
  ('a -> int -> bool) -> int
  = "caml_actor_test_closure_copy"

let () =
  let shared = ref 17 in
  let link = ref None in
  let cycle = Node link in
  link := Some cycle;
  let graph = {
    left = shared;
    right = shared;
    cycle;
    text = Bytes.to_string (Bytes.of_string "actor-copy");
    number = 42.5;
  } in
  let entry _ =
    if !(graph.left) < 0 then ignore graph.text
  in
  let rec even inbox n =
    ignore inbox;
    n = 0 || odd inbox (n - 1)
  and odd inbox n =
    ignore inbox;
    n <> 0 && even inbox (n - 1)
  in
  match exercise entry graph even odd with
  | 0 -> print_endline "closure graph copy: ok"
  | code -> failwith (Printf.sprintf "closure copy seam failed: %d" code)
