(* TEST
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let () =
  match Actor.run (fun _ ->
    let child =
      match Actor.spawn (fun _ -> raise (Failure "no debug boom")) with
      | Ok pid -> pid
      | Error _ -> failwith "spawn failed"
    in
    let monitor =
      match Actor.monitor child with
      | Ok monitor -> monitor
      | Error _ -> failwith "monitor failed"
    in
    match Actor.await_exit monitor with
    | Actor.Uncaught_exception { summary; backtrace = None } ->
        assert (summary = "Failure(\"no debug boom\")")
    | _ -> failwith "unexpected no-debug exit")
  with
  | Ok () -> print_endline "no-debug exception summary: ok"
  | Error _ -> failwith "actor world failed"
