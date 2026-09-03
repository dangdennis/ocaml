(* TEST
 modules = "elastic_heaps_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

external exercise : unit -> int = "caml_actor_test_elastic_heaps"

let () =
  match exercise () with
  | 0 -> print_endline "elastic actor heaps: ok"
  | code ->
      failwith
        (Printf.sprintf "elastic actor heap seam failed: %d" code)
