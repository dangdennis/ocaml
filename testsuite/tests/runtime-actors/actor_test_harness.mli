(** Deterministic support for the actor runtime tests.

    This module deliberately has no dependency on the actor implementation. *)

type seed = int64

val default_seed : seed
val seed_of_string : string -> (seed, string) result
val seed_from_env : unit -> (seed, string) result

module Prng : sig
  type t

  val create : seed -> t
  val next_u64 : t -> int64
  val int : t -> int -> int
end

type event = {
  step : int;
  actor : int option;
  op : string;
  fields : (string * string) list;
}

type trace = {
  seed : seed;
  events : event list;
}

val event :
  step:int -> ?actor:int -> op:string ->
  (string * string) list -> event

val trace : seed:seed -> event list -> trace
val encode : trace -> string
val decode : string -> (trace, string) result
val read_replay_from_env : unit -> (trace option, string) result

type divergence = {
  index : int;
  expected : event option;
  actual : event option;
  reason : string;
}

val replay : expected:trace -> actual:trace ->
  (unit, divergence) result

val string_of_divergence : divergence -> string

exception Harness_failure of string

val fail : test:string -> trace -> string -> 'a
