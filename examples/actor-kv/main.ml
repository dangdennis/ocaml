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
  let port = ref Kv_service.default_config.port in
  let connections = ref Kv_service.default_config.max_connections in
  let seconds = ref Kv_service.default_config.connection_seconds in
  let test_control = ref false in
  Arg.parse [
    "--port", Arg.Set_int port, "TCP port on 127.0.0.1 (default 7000)";
    "--connections", Arg.Set_int connections,
      "Maximum live connections (default 16)";
    "--lease", Arg.Set_float seconds,
      "Absolute connection lifetime in seconds (default 30)";
    "--test-control", Arg.Set test_control,
      "Enable CRASH and SHUTDOWN test commands";
  ] (fun _ -> raise (Arg.Bad "unexpected positional argument"))
    "actor-kv [--port N] [--connections N] [--lease SECONDS]";
  let config = { Kv_service.default_config with port = !port;
    max_connections = !connections; connection_seconds = !seconds;
    test_control = !test_control } in
  if not (Kv_service.valid config) || !port = 0 then
    (prerr_endline "invalid configuration"; exit 2);
  Printf.printf
    "actor-kv: 127.0.0.1:%d, at most %d connections, lease %.3fs\n%!"
    !port !connections !seconds;
  match Actor.run_with_config (Kv_service.world_config config) (fun inbox ->
    match Kv_service.serve config inbox with
    | Ok _ -> () | Error _ -> failwith "service terminated") with
  | Ok () -> ()
  | Error _ -> prerr_endline "actor-kv: actor world terminated with an error";
    exit 1
