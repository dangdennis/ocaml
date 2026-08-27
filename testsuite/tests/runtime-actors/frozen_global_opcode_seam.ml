(* TEST
 modules = "frozen_global_opcode_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

external exercise : unit -> int = "caml_actor_test_frozen_global_opcodes"

let () =
  match exercise () with
  | 0 -> print_endline "frozen global opcode seam: ok"
  | code ->
      failwith (Printf.sprintf "frozen global opcode seam failed: %d" code)
