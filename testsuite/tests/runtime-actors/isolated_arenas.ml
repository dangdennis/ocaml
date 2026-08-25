(* TEST
 modules = "isolated_arenas_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

external exercise : unit -> int
  = "caml_actor_test_isolated_arenas"

let () =
  match exercise () with
  | 0 -> print_endline "isolated arenas: ok"
  | code ->
      failwith
        (Printf.sprintf "isolated arena seam failed: %d" code)
