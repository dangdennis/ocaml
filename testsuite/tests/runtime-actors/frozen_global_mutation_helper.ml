type mutable_record = {
  mutable payload : int;
  stable : int;
}

type wide_mutable_record = {
  mutable field0 : int;
  mutable field1 : int;
  mutable field2 : int;
  mutable field3 : int;
  mutable field4 : int;
}

type mutable_float_record = {
  mutable float_payload : float;
  stable_float : float;
}

external increment_ref : int ref -> unit = "%incr"
external set_array : 'a array -> int -> 'a -> unit = "%array_unsafe_set"
external set_bytes : bytes -> int -> char -> unit = "%bytes_unsafe_set"
external get_bytes : bytes -> int -> char = "%bytes_safe_get"
external set_float_array : float array -> int -> float -> unit
  = "%floatarray_unsafe_set"

let frozen_ref = ref 3
let frozen_record = { payload = 5; stable = 7 }
let frozen_wide_record = {
  field0 = 11;
  field1 = 13;
  field2 = 17;
  field3 = 19;
  field4 = 23;
}
let frozen_float_record = { float_payload = 29.0; stable_float = 31.0 }
let frozen_array = [|11; 13|]
let frozen_bytes = Bytes.of_string "ab"
let frozen_float_array = [|17.0; 19.0|]

let mutate_ref () = increment_ref frozen_ref
let mutate_record () = frozen_record.payload <- 23
let mutate_field1 () = frozen_wide_record.field1 <- 37
let mutate_field2 () = frozen_wide_record.field2 <- 41
let mutate_field3 () = frozen_wide_record.field3 <- 43
let mutate_field4 () = frozen_wide_record.field4 <- 47
let mutate_float_record () = frozen_float_record.float_payload <- 53.0
let mutate_array () = set_array frozen_array 0 29
let mutate_bytes () = set_bytes frozen_bytes 0 'z'
let mutate_float_array () = set_float_array frozen_float_array 0 31.0

let ref_value () = !frozen_ref
let record_values () = frozen_record.payload, frozen_record.stable
let wide_record_values () =
  frozen_wide_record.field0,
  frozen_wide_record.field1,
  frozen_wide_record.field2,
  frozen_wide_record.field3,
  frozen_wide_record.field4
let float_record_values () =
  frozen_float_record.float_payload, frozen_float_record.stable_float
let array_value () = frozen_array.(0)
let bytes_value () = get_bytes frozen_bytes 0
let float_array_value () = frozen_float_array.(0)
