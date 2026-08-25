type seed = int64

let default_seed = 0x5eed5eed5eed5eedL

let valid_seed_syntax input =
  let length = String.length input in
  let first_digit =
    if length > 0 && (input.[0] = '+' || input.[0] = '-') then 1 else 0
  in
  if first_digit = length then false
  else
    let digits = String.sub input first_digit (length - first_digit) in
    if String.starts_with ~prefix:"0x" digits ||
       String.starts_with ~prefix:"0X" digits
    then
      String.length digits > 2 &&
      String.for_all
        (function
          | '0' .. '9' | 'a' .. 'f' | 'A' .. 'F' -> true
          | _ -> false)
        (String.sub digits 2 (String.length digits - 2))
    else String.for_all (function '0' .. '9' -> true | _ -> false) digits

let seed_of_string input =
  let input = String.trim input in
  if input = "" then Error "actor seed is empty"
  else if not (valid_seed_syntax input) then
    Error (Printf.sprintf "invalid actor seed: %S" input)
  else
    try Ok (Int64.of_string input)
    with Failure _ -> Error (Printf.sprintf "invalid actor seed: %S" input)

let seed_from_env () =
  match Sys.getenv_opt "ACTOR_SEED" with
  | None -> Ok default_seed
  | Some input -> seed_of_string input

module Prng = struct
  type t = { mutable state : int64 }

  let create state = { state }

  let next_u64 t =
    let multiplier = 0x5851f42d4c957f2dL in
    let increment = 0x14057b7ef767814fL in
    t.state <- Int64.add (Int64.mul t.state multiplier) increment;
    t.state

  let int t bound =
    if bound <= 0 then invalid_arg "Actor_test_harness.Prng.int";
    let positive = Int64.logand (next_u64 t) Int64.max_int in
    Int64.to_int (Int64.rem positive (Int64.of_int bound))
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

let is_name_char = function
  | 'a' .. 'z' | 'A' .. 'Z' | '0' .. '9'
  | '.' | '_' | ':' | '-' -> true
  | _ -> false

let check_name kind name =
  if name = "" || not (String.for_all is_name_char name) then
    invalid_arg (Printf.sprintf "invalid actor trace %s: %S" kind name)

let canonical_fields fields =
  let fields = List.sort (fun (a, _) (b, _) -> String.compare a b) fields in
  let rec check previous = function
    | [] -> ()
    | (key, _) :: rest ->
        check_name "field name" key;
        if Some key = previous then
          invalid_arg (Printf.sprintf "duplicate actor trace field: %S" key);
        check (Some key) rest
  in
  check None fields;
  fields

let event ~step ?actor ~op fields =
  if step < 0 then invalid_arg "negative actor trace step";
  Option.iter
    (fun actor ->
      if actor < 0 then invalid_arg "negative actor trace actor ID")
    actor;
  check_name "operation" op;
  { step; actor; op; fields = canonical_fields fields }

let trace ~seed events =
  let rec validate expected acc = function
    | [] -> { seed; events = List.rev acc }
    | item :: rest ->
        if item.step <> expected then
          invalid_arg
            (Printf.sprintf
               "actor trace step %d found where step %d was expected"
               item.step expected);
        let item = event ~step:item.step ?actor:item.actor ~op:item.op
            item.fields in
        validate (expected + 1) (item :: acc) rest
  in
  validate 0 [] events

let is_unreserved = function
  | 'a' .. 'z' | 'A' .. 'Z' | '0' .. '9'
  | '.' | '_' | '~' | '-' -> true
  | _ -> false

let percent_encode input =
  let output = Buffer.create (String.length input) in
  String.iter
    (fun character ->
      if is_unreserved character then Buffer.add_char output character
      else Printf.bprintf output "%%%02X" (Char.code character))
    input;
  Buffer.contents output

let hex_value = function
  | '0' .. '9' as c -> Some (Char.code c - Char.code '0')
  | 'a' .. 'f' as c -> Some (Char.code c - Char.code 'a' + 10)
  | 'A' .. 'F' as c -> Some (Char.code c - Char.code 'A' + 10)
  | _ -> None

let percent_decode input =
  let length = String.length input in
  let output = Buffer.create length in
  let rec loop index =
    if index = length then Ok (Buffer.contents output)
    else if input.[index] <> '%' then begin
      Buffer.add_char output input.[index];
      loop (index + 1)
    end else if index + 2 >= length then
      Error "truncated percent escape in actor trace"
    else
      match hex_value input.[index + 1], hex_value input.[index + 2] with
      | Some high, Some low ->
          Buffer.add_char output (Char.chr ((high lsl 4) lor low));
          loop (index + 3)
      | _ -> Error "invalid percent escape in actor trace"
  in
  loop 0

let add_event output item =
  let actor =
    match item.actor with
    | None -> "-"
    | Some actor -> string_of_int actor
  in
  Printf.bprintf output "%d\t%s\t%s" item.step actor item.op;
  List.iter
    (fun (key, value) ->
      Printf.bprintf output "\t%s=%s" key (percent_encode value))
    (canonical_fields item.fields);
  Buffer.add_char output '\n'

let encode input_trace =
  let trace = trace ~seed:input_trace.seed input_trace.events in
  let output = Buffer.create 256 in
  Printf.bprintf output "actor-trace-v1\tseed=%016Lx\n" trace.seed;
  List.iter (add_event output) trace.events;
  Buffer.contents output

let parse_hex_seed input =
  if String.length input <> 16 ||
     not (String.for_all (fun c -> Option.is_some (hex_value c)) input)
  then Error "actor trace seed must contain 16 hexadecimal digits"
  else
    let value = ref 0L in
    String.iter
      (fun c ->
        let digit = Option.get (hex_value c) in
        value := Int64.logor (Int64.shift_left !value 4)
            (Int64.of_int digit))
      input;
    Ok !value

let split_field input =
  match String.index_opt input '=' with
  | None -> Error (Printf.sprintf "actor trace field lacks '=': %S" input)
  | Some index ->
      let key = String.sub input 0 index in
      let encoded =
        String.sub input (index + 1) (String.length input - index - 1)
      in
      match percent_decode encoded with
      | Error _ as error -> error
      | Ok value -> Ok (key, value)

let parse_actor = function
  | "-" -> Ok None
  | input ->
      try
        let actor = int_of_string input in
        if actor < 0 then Error "negative actor ID in actor trace"
        else Ok (Some actor)
      with Failure _ ->
        Error (Printf.sprintf "invalid actor ID in actor trace: %S" input)

let rec parse_fields acc = function
  | [] -> Ok (List.rev acc)
  | input :: rest ->
      match split_field input with
      | Error _ as error -> error
      | Ok field -> parse_fields (field :: acc) rest

let parse_event expected input =
  match String.split_on_char '\t' input with
  | step :: actor :: op :: fields ->
      begin
        try
          let step = int_of_string step in
          if step <> expected then
            Error
              (Printf.sprintf
                 "actor trace step %d found where step %d was expected"
                 step expected)
          else
            match parse_actor actor, parse_fields [] fields with
            | Ok actor, Ok fields ->
                begin
                  try Ok (event ~step ?actor ~op fields)
                  with Invalid_argument message -> Error message
                end
            | Error message, _ | _, Error message -> Error message
        with Failure _ ->
          Error (Printf.sprintf "invalid actor trace step: %S" step)
      end
  | _ -> Error "actor trace event must contain step, actor, and operation"

let decode_unchecked input =
  let lines = String.split_on_char '\n' input in
  match List.rev lines with
  | "" :: reversed_lines ->
      begin
        match List.rev reversed_lines with
        | header :: event_lines ->
            begin
              match String.split_on_char '\t' header with
              | ["actor-trace-v1"; seed_field]
                when String.starts_with ~prefix:"seed=" seed_field ->
                  let seed = String.sub seed_field 5
                      (String.length seed_field - 5) in
                  begin
                    match parse_hex_seed seed with
                    | Error _ as error -> error
                    | Ok seed ->
                        let rec events expected acc = function
                          | [] ->
                              begin
                                try Ok (trace ~seed (List.rev acc))
                                with Invalid_argument message -> Error message
                              end
                          | line :: rest ->
                              match parse_event expected line with
                              | Error _ as error -> error
                              | Ok item ->
                                  events (expected + 1) (item :: acc) rest
                        in
                        events 0 [] event_lines
                  end
              | _ -> Error "invalid actor-trace-v1 header"
            end
        | [] -> Error "actor trace is empty"
      end
  | _ -> Error "actor trace must end with a newline"

let decode input =
  match decode_unchecked input with
  | Error _ as error -> error
  | Ok trace when encode trace = input -> Ok trace
  | Ok _ -> Error "actor trace is not in canonical actor-trace-v1 form"

let read_file path =
  try
    let input = open_in_bin path in
    Fun.protect
      ~finally:(fun () -> close_in_noerr input)
      (fun () -> Ok (really_input_string input (in_channel_length input)))
  with Sys_error message -> Error message

let read_replay_from_env () =
  match Sys.getenv_opt "ACTOR_TRACE" with
  | None -> Ok None
  | Some path ->
      begin
        match read_file path with
        | Error message ->
            Error (Printf.sprintf "cannot read ACTOR_TRACE %S: %s"
                     path message)
        | Ok input ->
            match decode input with
            | Ok trace -> Ok (Some trace)
            | Error message ->
                Error (Printf.sprintf "cannot parse ACTOR_TRACE %S: %s"
                         path message)
      end

type divergence = {
  index : int;
  expected : event option;
  actual : event option;
  reason : string;
}

let normalized_event item =
  { item with fields = canonical_fields item.fields }

let replay ~expected ~actual =
  if expected.seed <> actual.seed then
    Error {
      index = 0;
      expected = None;
      actual = None;
      reason = Printf.sprintf "seed mismatch: %016Lx <> %016Lx"
          expected.seed actual.seed;
    }
  else
    let rec compare index expected actual =
      match expected, actual with
      | [], [] -> Ok ()
      | expected :: expected_rest, actual :: actual_rest ->
          if normalized_event expected = normalized_event actual then
            compare (index + 1) expected_rest actual_rest
          else
            Error {
              index;
              expected = Some expected;
              actual = Some actual;
              reason = "event mismatch";
            }
      | expected :: _, [] ->
          Error {
            index;
            expected = Some expected;
            actual = None;
            reason = "actual trace ended early";
          }
      | [], actual :: _ ->
          Error {
            index;
            expected = None;
            actual = Some actual;
            reason = "actual trace has an extra event";
          }
    in
    compare 0 expected.events actual.events

let event_summary = function
  | None -> "<none>"
  | Some item ->
      let output = Buffer.create 64 in
      add_event output item;
      String.trim (Buffer.contents output)

let string_of_divergence divergence =
  Printf.sprintf
    "first divergence at event %d: %s\nexpected: %s\nactual: %s"
    divergence.index divergence.reason
    (event_summary divergence.expected)
    (event_summary divergence.actual)

exception Harness_failure of string

let fail ~test trace message =
  let seed = Printf.sprintf "%016Lx" trace.seed in
  let report =
    Printf.sprintf
      "actor harness failure: %s\nseed: 0x%s\ntrace:\n%s\n\
       reproduce: ACTOR_SEED=0x%s ACTOR_TRACE=trace.txt \
       make -C testsuite one TEST=%s"
      message seed (encode trace) seed test
  in
  raise (Harness_failure report)
