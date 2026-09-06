(* TEST
 modules = "actor_timer_store_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)
external check : unit -> unit = "caml_actor_test_timer_store"
let () = check (); print_endline "actor timer store: ok"
