(* TEST
 {
   native;
 }
*)

let invoked = ref false

let () =
  begin match Actor.run (fun _ -> invoked := true) with
  | Error Actor.Unsupported_runtime -> ()
  | _ -> assert false
  end;
  assert (not !invoked);
  print_endline "native actor guard: ok"
