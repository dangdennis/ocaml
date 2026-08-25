(* TEST
 modules = "actor_test_harness.ml";
 {
   bytecode;
 }
*)

open Actor_test_harness

let require condition =
  if not condition then failwith "actor harness self-test failed"

let require_ok = function
  | Ok value -> value
  | Error message -> failwith message

let contains haystack needle =
  let haystack_length = String.length haystack in
  let needle_length = String.length needle in
  let rec search index =
    if index + needle_length > haystack_length then false
    else if String.sub haystack index needle_length = needle then true
    else search (index + 1)
  in
  search 0

let () =
  require (seed_of_string "42" = Ok 42L);
  require (seed_of_string "0x2a" = Ok 42L);
  require (Result.is_error (seed_of_string "not-a-seed"));
  require (Result.is_error (seed_of_string "0b101010"));

  let random = Prng.create 0x0123456789abcdefL in
  let vector = List.init 3 (fun _ -> Prng.next_u64 random) in
  require (vector = [
    0x2ce32d2335df4552L;
    0xca18dd5ae3c45eb9L;
    0x860f53667996eed4L;
  ]);

  let expected = trace ~seed:default_seed [
    event ~step:0 ~actor:1 ~op:"spawn"
      ["z", "space value"; "child", "2"];
    event ~step:1 ~op:"deadlock" [];
  ] in
  let encoded =
    "actor-trace-v1\tseed=5eed5eed5eed5eed\n\
     0\t1\tspawn\tchild=2\tz=space%20value\n\
     1\t-\tdeadlock\n"
  in
  require (encode expected = encoded);
  let decoded = require_ok (decode encoded) in
  require (decoded = expected);
  require (replay ~expected ~actual:decoded = Ok ());
  require (Result.is_error (decode
    "actor-trace-v1\tseed=5EED5EED5EED5EED\n"));
  require (Result.is_error (decode
    "actor-trace-v1\tseed=5eed5eed5eed5eed\n\
     0\t1\tspawn\tz=last\tchild=2\n"));

  let actual = trace ~seed:default_seed [
    event ~step:0 ~actor:1 ~op:"spawn"
      ["child", "2"; "z", "space value"];
    event ~step:1 ~op:"exit" [];
  ] in
  begin
    match replay ~expected ~actual with
    | Ok () -> failwith "expected an actor trace divergence"
    | Error divergence ->
        require (divergence.index = 1);
        require (divergence.reason = "event mismatch");
        require (contains (string_of_divergence divergence)
                   "first divergence at event 1")
  end;

  begin
    try fail ~test:"tests/runtime-actors/harness_selftest.ml"
          expected "deliberate"
    with Harness_failure report ->
      require (contains report "seed: 0x5eed5eed5eed5eed");
      require (contains report "ACTOR_TRACE=trace.txt");
      require (contains report encoded)
  end
