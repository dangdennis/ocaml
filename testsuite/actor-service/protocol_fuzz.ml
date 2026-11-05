(**************************************************************************)
(*                                                                        *)
(*                                 OCaml                                  *)
(*                                                                        *)
(*                             Dennis Dang                                *)
(*                                                                        *)
(*   Copyright 2026 Dennis Dang                                           *)
(*                                                                        *)
(*   All rights reserved.  This file is distributed under the terms of    *)
(*   the GNU Lesser General Public License version 2.1, with the          *)
(*   special exception on linking described in the file LICENSE.          *)
(*                                                                        *)
(**************************************************************************)

let () =
  let cases () =
    let seed = ref 1234567 in
    let random n =
      seed := (!seed * 1103515245 + 12345) land 0x3fffffff; !seed mod n in
    for _ = 1 to 2000 do
      let text = String.init (random 700) (fun _ -> Char.chr (random 256)) in
      match Kv_protocol.parse ~test_control:false text with
      | Error () -> ()
      | Ok (Ping | Quit) -> ()
      | Ok (Get key | Del key) -> assert (Kv_protocol.key_valid key)
      | Ok (Set (key, value)) ->
          assert (Kv_protocol.key_valid key && Kv_protocol.value_valid value)
      | Ok (Shutdown | Crash | Crash_state) -> assert false
    done;
    for n = 0 to Kv_protocol.max_value do
      let value = String.make n 'x' in
      assert (Kv_protocol.parse ~test_control:false ("SET key " ^ value)
              = Ok (Set ("key", value)))
    done;
    assert (Kv_protocol.parse ~test_control:false "SHUTDOWN" = Error ());
    assert (Kv_protocol.parse ~test_control:false "CRASH_STATE" = Error ());
    assert (Kv_protocol.parse ~test_control:false "SET a bad\000value" = Error
      ())
  in
  cases ();
  (match Actor.run_with_config
    { Actor.default_world_config with
      root_heap = { initial_words = 1024; maximum_words = 4096 } }
    (fun _ -> cases ()) with Ok () -> () | _ -> assert false);
  print_endline "protocol fuzz: 4000 deterministic cases plus boundaries passed"
