(* TEST
 modules = "actor_net_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)
external test : unit -> unit = "caml_actor_test_net_store"
let () = test (); print_endline "actor net store: ok"
