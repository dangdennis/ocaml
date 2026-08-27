(* TEST
 modules = "reference_service_protocol.ml";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let () =
  match Actor.run (fun _ ->
    assert (Reference_service_protocol.classify
              (Reference_service_protocol.Get "health") = 1)) with
  | Ok () ->
      print_endline "real program contract: frozen globals work"
  | Error (Actor.Root_failed message) ->
      failwith ("unexpected rejection: " ^ message)
  | Error _ ->
      failwith "unexpected actor result"
