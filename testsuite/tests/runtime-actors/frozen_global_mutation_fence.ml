(* TEST
 modules = "frozen_global_mutation_helper.ml";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

let expect_fence label expected root unchanged =
  match Actor.run root with
  | Error (Actor.Root_failed message)
    when message = expected ->
      assert (unchanged ());
      print_endline (label ^ ": " ^ expected)
  | Error (Actor.Root_failed message) ->
      failwith (label ^ ": unexpected root failure: " ^ message)
  | Error Actor.Unsupported_runtime ->
      failwith (label ^ ": unsupported runtime")
  | Error Actor.Root_heap_exhausted ->
      failwith (label ^ ": root heap exhausted")
  | Error Actor.Deadlock ->
      failwith (label ^ ": deadlock")
  | Ok () ->
      failwith (label ^ ": mutation escaped the frozen-image fence")

let root_ref _ = Frozen_global_mutation_helper.mutate_ref ()
let root_record _ = Frozen_global_mutation_helper.mutate_record ()
let root_field1 _ = Frozen_global_mutation_helper.mutate_field1 ()
let root_field2 _ = Frozen_global_mutation_helper.mutate_field2 ()
let root_field3 _ = Frozen_global_mutation_helper.mutate_field3 ()
let root_field4 _ = Frozen_global_mutation_helper.mutate_field4 ()
let root_float_record _ =
  Frozen_global_mutation_helper.mutate_float_record ()
let root_array _ = Frozen_global_mutation_helper.mutate_array ()
let root_bytes _ = Frozen_global_mutation_helper.mutate_bytes ()
let root_float_array _ = Frozen_global_mutation_helper.mutate_float_array ()

let () =
  expect_fence "ref" "unsupported operation at opcode OFFSETREF" root_ref
    (fun () -> Frozen_global_mutation_helper.ref_value () = 3);
  expect_fence "record" "unsupported operation at opcode SETFIELD0" root_record
    (fun () -> Frozen_global_mutation_helper.record_values () = (5, 7));
  expect_fence "record field 1"
    "unsupported operation at opcode SETFIELD1" root_field1
    (fun () ->
      Frozen_global_mutation_helper.wide_record_values ()
      = (11, 13, 17, 19, 23));
  expect_fence "record field 2"
    "unsupported operation at opcode SETFIELD2" root_field2
    (fun () ->
      Frozen_global_mutation_helper.wide_record_values ()
      = (11, 13, 17, 19, 23));
  expect_fence "record field 3"
    "unsupported operation at opcode SETFIELD3" root_field3
    (fun () ->
      Frozen_global_mutation_helper.wide_record_values ()
      = (11, 13, 17, 19, 23));
  expect_fence "record generic field"
    "unsupported operation at opcode SETFIELD" root_field4
    (fun () ->
      Frozen_global_mutation_helper.wide_record_values ()
      = (11, 13, 17, 19, 23));
  expect_fence "float record"
    "unsupported operation at opcode SETFLOATFIELD" root_float_record
    (fun () ->
      Frozen_global_mutation_helper.float_record_values () = (29.0, 31.0));
  expect_fence "array" "unsupported operation at opcode SETVECTITEM" root_array
    (fun () -> Frozen_global_mutation_helper.array_value () = 11);
  expect_fence "bytes" "unsupported operation at opcode SETBYTESCHAR" root_bytes
    (fun () -> Frozen_global_mutation_helper.bytes_value () = 'a');
  expect_fence "float array"
    "unsupported primitive caml_floatarray_unsafe_set/3" root_float_array
    (fun () -> Frozen_global_mutation_helper.float_array_value () = 17.0)
