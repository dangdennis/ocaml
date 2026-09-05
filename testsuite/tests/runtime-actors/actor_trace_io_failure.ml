(* TEST
 set OCAML_ACTOR_TRACE = ".";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let () =
  begin match Actor.run (fun _ -> ()) with
  | Ok () -> ()
  | Error _ -> failwith "trace output failure changed the world result"
  end;
  print_endline "actor trace output failure: inert"
