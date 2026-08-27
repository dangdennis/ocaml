(* TEST
 modules = "runtime_fence_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

external exercise : (int -> bool) -> (int -> bool) -> int
  = "caml_actor_test_runtime_fence"
external poison : int -> int -> unit = "caml_actor_test_runtime_fence_poison"

let _keep_poison_primitive () = poison 0 1

let rec even value = value = 0 || odd (value - 1)
and odd value = value <> 0 && even (value - 1)

let () =
  match exercise even odd with
  | 0 -> print_endline "runtime fence: ok"
  | code ->
      failwith (Printf.sprintf "runtime fence seam failed: %d" code)
