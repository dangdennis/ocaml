(* TEST
 {
   native;
 }
*)

let invoked = ref false

let expect_invalid_argument expected action =
  match action () with
  | _ -> assert false
  | exception Invalid_argument message -> assert (message = expected)
  | exception _ -> assert false

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
  let pid : unit Actor.pid = Obj.magic 0 in
  expect_invalid_argument "Actor.monitor outside an actor world"
    (fun () -> Actor.monitor pid);
  expect_invalid_argument "Actor.cancel outside an actor world"
    (fun () -> Actor.cancel pid);
  let monitor : Actor.monitor = Obj.magic (0, 1) in
  expect_invalid_argument "Actor.await_exit outside an actor world"
    (fun () -> Actor.await_exit monitor);
  assert (not !invoked);
  print_endline "native actor guard: ok"
