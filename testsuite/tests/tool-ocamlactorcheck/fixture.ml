module type Helper = sig
  val scalar : int
  val pair : int * int
  val identity : 'a -> 'a
end

external primitive1 : int -> int = "caml_int_compare"
external primitive2 : int -> int -> int = "caml_int_compare"
external primitive3 : int -> int -> int -> int = "caml_int_compare"
external primitive4 : int -> int -> int -> int -> int = "caml_int_compare"
external primitive5 : int -> int -> int -> int -> int -> int
  = "caml_int_compare"
external primitive6 : int -> int -> int -> int -> int -> int -> int
  = "caml_int_compare"
external array_make : int -> int -> int array = "caml_array_make"
external forbidden : unit -> string * int = "caml_sys_get_config"
external unknown : unit -> unit = "caml_actorcheck_unknown_primitive"

let read_global_field _argument = Fixture_helper.scalar
let read_global _argument = (module Fixture_helper : Helper)

let whole_global = (module Fixture_helper : Helper)
let global_field = Fixture_helper.scalar
let pushed_global = ((module Fixture_helper : Helper), 31)
let pushed_global_field = Fixture_helper.identity Fixture_helper.scalar
let projected_global_field =
  let left, _right = Fixture_helper.pair in
  left

let call1 = primitive1 1
let call2 = primitive2 1 2
let call3 = primitive3 1 2 3
let call4 = primitive4 1 2 3 4
let call5 = primitive5 1 2 3 4 5
let call6 = primitive6 1 2 3 4 5 6
let make_array = array_make 4 0
let call_forbidden = forbidden ()
let call_unknown = unknown ()
