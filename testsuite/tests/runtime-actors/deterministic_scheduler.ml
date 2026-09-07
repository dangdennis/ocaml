(* TEST
 modules = "deterministic_scheduler_stubs.c";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

external run : (unit -> unit) -> unit
  = "caml_actor_test_deterministic_scheduler"
external poison : unit -> unit = "caml_actor_test_scheduler_poison"

let _keep_poison_primitive () = poison ()

let grow_host_stack () =
  Printexc.record_backtrace (not (Printexc.backtrace_status ()));
  let rec descend n = if n = 0 then 0 else 1 + descend (n - 1) in
  ignore (descend 20_000)

let () =
  run grow_host_stack;
  print_endline "deterministic scheduler: ok"
