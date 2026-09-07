type leaf = {
  number : int;
  text : string;
}

type cycle = {
  mark : int;
  next : cycle;
}

type graph = {
  left : leaf;
  right : leaf;
  chain : int list;
  loop : cycle;
}

let immediate_constant = 37
let character_constant = 'g'
let string_constant = "frozen"

(* Present in the frozen global graph, but deliberately not actor-readable in
   Layer 9.  These payloads have runtime-specific layouts rather than ordinary
   scanned OCaml fields. *)
let custom_constant = 0x1122_3344_5566_7788L

let captured_custom =
  let value = 0x0102_0304_0506_0708L in
  fun () -> value

let consume_custom captured left right =
  ignore captured;
  left + right

let partial_custom = consume_custom custom_constant

let ephemeron_constant : (int, int) Ephemeron.K1.t =
  Ephemeron.K1.make 1 2

let shared_leaf = { number = 41; text = "shared" }

let rec self_cycle = {
  mark = 43;
  next = self_cycle;
}

let immutable_graph = {
  left = shared_leaf;
  right = shared_leaf;
  chain = [47; 53; 59];
  loop = self_cycle;
}

exception Frozen_error of int

let frozen_exception = Frozen_error 61

let catch_frozen_exception () =
  try raise frozen_exception with
  | Frozen_error payload -> payload

let rec even value =
  value = 0 || odd (value - 1)

and odd value =
  value <> 0 && even (value - 1)
