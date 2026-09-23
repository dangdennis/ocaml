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

let ok = function Ok x -> x | Error _ -> failwith "network benchmark failed"
let () =
  let count = 500 in
  let start = Sys.time () in
  ok (Actor.run (fun _ ->
    for _ = 1 to count do
      let listener = ok (Actor.Net.listen ~address:"127.0.0.1" ~port:0) in
      assert (ok (Actor.Net.local_port listener) > 0);
      ok (Actor.Net.close_listener listener)
    done;
    let stats = Actor.stats () in
    assert (stats.net_resources = 0 && stats.net_closed = count)));
  Printf.printf "network listener churn: %d operations in %.6fs CPU\n"
    count (Sys.time () -. start)
