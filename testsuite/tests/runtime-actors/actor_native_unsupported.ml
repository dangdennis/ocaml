(* TEST
 {
   native;
 }
*)

let invoked = ref false

let () =
  begin match Actor.stats () with
  | _ -> assert false
  | exception Invalid_argument message ->
      assert (message = "Actor.stats outside an actor world")
  | exception _ -> assert false
  end;
  begin match Actor.run (fun _ -> invoked := true) with
  | Error Actor.Unsupported_runtime -> ()
  | _ -> assert false
  end;
  assert (not !invoked);
  print_endline "native actor guard: ok"
