(* TEST
 modules = "runtime_fence_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

external exercise : unit -> int = "caml_actor_test_runtime_fence"
external poison : int -> int -> unit = "caml_actor_test_runtime_fence_poison"

let _keep_poison_primitive () = poison 0 1

let () =
  match exercise () with
  | 0 -> print_endline "runtime fence: ok"
  | code ->
      failwith (Printf.sprintf "runtime fence seam failed: %d" code)
