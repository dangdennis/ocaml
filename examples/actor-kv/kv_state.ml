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

module Keys = Map.Make (String)
type command = Execute of Kv_protocol.command * Actor.Net.socket *
  (Actor.Net.socket * Kv_protocol.reply) Actor.pid

type message = (unit, unit, command) Actor.Gen_server.message

let execute max_entries (Execute (command, socket, destination)) entries =
    let entries, reply = match command with
      | Kv_protocol.Ping -> entries, Kv_protocol.Pong
      | Get key -> entries,
          (match Keys.find_opt key entries with None -> Missing | Some v ->
            Value v)
      | Set (key, value) ->
          if not (Keys.mem key entries) && Keys.cardinal entries >= max_entries
          then entries, Full
          else Keys.add key value entries, Stored
      | Del key ->
          if Keys.mem key entries then Keys.remove key entries, Deleted
          else entries, Missing
      | Crash_state -> failwith "injected state failure"
      | Quit | Shutdown | Crash -> entries, Invalid
    in
    (* A dead/disconnected recipient must not take down the shared state. *)
    ignore (Actor.send destination (socket, reply));
    entries

let run ~max_entries inbox =
  Actor.Gen_server.run {
    init = (fun () -> Keys.empty);
    handle_call = (fun () entries -> (), entries);
    handle_cast = execute max_entries;
    terminate = (fun _ -> ());
  } inbox
