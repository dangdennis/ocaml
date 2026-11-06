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

module G = Actor.Gen_server
let ok = function Ok x -> x | Error _ -> failwith "counter operation failed"
let callbacks = {
  G.init = (fun () -> 0);
  handle_call = (fun increment state ->
    let state = state + increment in state, state);
  handle_cast = (fun increment state -> state + increment);
  terminate = (fun _ -> ());
}
let () =
  match Actor.run (fun inbox ->
    let client = G.client inbox in
    let server, monitor = ok (G.start client callbacks) in
    ok (G.cast server 40);
    assert (G.call client server 2 ~timeout:1. = Ok 42);
    ok (G.stop server);
    assert (Actor.await_exit monitor = Actor.Normal)) with
  | Ok () -> print_endline "counter: 42; orderly shutdown verified"
  | Error _ -> failwith "counter actor world failed"
