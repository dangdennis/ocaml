(* TEST
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

external denied_primitive : unit -> string * int = "caml_sys_get_config"
external denied_primitive2 : int -> int -> int = "caml_int64_add"
external denied_primitive3 : int -> int -> int -> unit
  = "caml_obj_set_raw_field"
external denied_primitive4 : int -> int -> int -> int -> int = "caml_hash"
external denied_primitive5 : int -> int -> int -> int -> int -> unit
  = "caml_blit_bytes"

let expect_root_failure expected = function
  | Error (Actor.Root_failed actual) when actual = expected -> ()
  | Error (Actor.Root_failed actual) ->
      failwith ("expected " ^ expected ^ ", got " ^ actual)
  | _ -> failwith ("expected root failure: " ^ expected)

let () =
  expect_root_failure
    "unsupported operation at opcode DIVINT"
    (Actor.run (fun _ -> ignore (1 / 0)));
  expect_root_failure
    "unsupported primitive caml_sys_get_config/1"
    (Actor.run (fun _ -> ignore (denied_primitive ())));
  expect_root_failure
    "unsupported primitive caml_int64_add/2"
    (Actor.run (fun _ -> ignore (denied_primitive2 0 0)));
  expect_root_failure
    "unsupported primitive caml_obj_set_raw_field/3"
    (Actor.run (fun _ -> denied_primitive3 0 0 0));
  expect_root_failure
    "unsupported primitive caml_hash/4"
    (Actor.run (fun _ -> ignore (denied_primitive4 0 0 0 0)));
  expect_root_failure
    "unsupported primitive caml_blit_bytes/5"
    (Actor.run (fun _ -> denied_primitive5 0 0 0 0 0));
  print_endline "structured diagnostics: ok"
