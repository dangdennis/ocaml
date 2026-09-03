type capability = Pure | Actor_local | Scheduler_aware | Forbidden

type entry = {
  name : string;
  arity : int;
  capability : capability;
  family : string;
  audit : string;
}

type classification =
  | Allowed of entry
  | Denied of entry
  | Arity_mismatch of entry
  | Unknown

val entries : entry array
val classify : string -> int -> classification
val capability_name : capability -> string
