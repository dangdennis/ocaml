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

type config = {
  address : string;
  port : int;
  max_connections : int;
  max_entries : int;
  connection_seconds : float;
  max_restarts : int;
  test_control : bool;
}

let default_config = {
  address = "127.0.0.1"; port = 7000; max_connections = 16;
  max_entries = 128; connection_seconds = 30.; max_restarts = 3;
  test_control = false;
}

let world_config config =
  { Actor.default_world_config with
    root_heap = { initial_words = 4096; maximum_words = 65536 };
    child_heap = { initial_words = 2048; maximum_words = 262144 };
    max_actors = 3 + 2 * config.max_connections;
    max_message_words = 2048;
    max_mailbox_messages = max 64 (4 * config.max_connections);
    max_mailbox_bytes = max 65536 (8192 * config.max_connections);
    max_monitors = 2 + 2 * config.max_connections;
    max_timers = config.max_connections;
    max_net_resources = config.max_connections + 2;
    max_net_operations = config.max_connections + 1;
    max_net_buffered_bytes = (config.max_connections + 1) *
      (Kv_protocol.max_line + 2) }

type summary = {
  accepted : int; completed : int; failed : int; timed_out : int;
  rejected : int; listener_restarts : int;
}

type error = Invalid_config | Start_failed | State_failed of Actor.exit_reason
           | Restart_limit | Deadline_failed of Actor.exit_reason

type session = {
  worker : (Actor.Net.socket * Kv_protocol.reply) Actor.pid;
  monitor : Actor.monitor;
  deadline : unit Actor.pid;
  deadline_monitor : Actor.monitor;
}

type watch = State | Listener | Worker of session | Deadline of session

let cancel pid = ignore (Actor.cancel pid)
let retire pid monitor = cancel pid; ignore (Actor.await_exit monitor)
let stop_session session =
  retire session.worker session.monitor;
  retire session.deadline session.deadline_monitor

let valid config =
  config.port >= 0 && config.port <= 65535 &&
  config.max_connections > 0 && config.max_connections <= 128 &&
  config.max_entries > 0 && config.max_entries <= 1024 &&
  config.max_restarts >= 0 && config.max_restarts <= 100 &&
  Float.compare config.connection_seconds 0. > 0 &&
  Float.compare config.connection_seconds 3600. <= 0

type counters = {
  mutable accepted_count : int; mutable completed_count : int;
  mutable failed_count : int; mutable timeout_count : int;
  mutable rejected_count : int; mutable restart_count : int;
}

type context = {
  config : config;
  root : Kv_listener.event Actor.pid;
  state : Kv_state.message Actor.pid;
  state_monitor : Actor.monitor;
  mutable sessions : session list;
  counters : counters;
}

let increment value = if value = max_int then value else value + 1

let stop_sessions context =
  List.iter stop_session context.sessions;
  context.sessions <- []

let remove context session =
  context.sessions <- List.filter
    (fun entry -> entry.worker <> session.worker) context.sessions

let start_listener context =
  Actor.spawn_monitored
    (Kv_listener.run ~address:context.config.address ~port:context.config.port
       ~root:context.root)

let prepare_session context =
  let root = context.root in
  let shutdown () = ignore (Actor.send root Kv_listener.Stop) in
  match Actor.spawn_monitored
    (Kv_connection.run ~state:context.state
       ~test_control:context.config.test_control ~shutdown) with
  | Error _ -> None
  | Ok (worker, monitor) ->
      let seconds = context.config.connection_seconds in
      (match Actor.spawn_monitored (fun _ ->
         match Actor.Timer.sleep seconds with
         | Ok () -> () | Error _ -> failwith "connection deadline failed") with
       | Error _ -> retire worker monitor; None
       | Ok (deadline, deadline_monitor) ->
           Some {worker; monitor; deadline; deadline_monitor})

let reject context listener =
  context.counters.rejected_count <- increment context.counters.rejected_count;
  match Actor.send listener Kv_listener.Reject with
  | Ok () -> ()
  | Error _ -> cancel listener

let admit context listener =
  if List.length context.sessions >= context.config.max_connections then
    reject context listener
  else match prepare_session context with
    | None -> reject context listener
    | Some session ->
        match Actor.send listener (Kv_listener.Assign session.worker) with
        | Error _ ->
            stop_session session;
            context.counters.rejected_count <- increment
              context.counters.rejected_count;
            cancel listener
        | Ok () ->
            context.sessions <- session :: context.sessions;
            context.counters.accepted_count <-
              increment context.counters.accepted_count

let summary counters = {
  accepted = counters.accepted_count; completed = counters.completed_count;
  failed = counters.failed_count; timed_out = counters.timeout_count;
  rejected = counters.rejected_count; listener_restarts =
    counters.restart_count }

let watches context listener_monitor =
  (State, context.state_monitor) :: (Listener, listener_monitor) ::
  List.concat_map (fun session ->
    [Worker session, session.monitor; Deadline session,
      session.deadline_monitor])
    context.sessions

let rec loop context inbox listener listener_monitor =
  let watched = watches context listener_monitor in
  match Actor.receive_or_exit inbox (List.map snd watched) with
  | Actor.Message Kv_listener.Stop ->
      retire listener listener_monitor; stop_sessions context;
      retire context.state context.state_monitor; Ok (summary context.counters)
  | Actor.Message (Kv_listener.Accepted source) ->
      (* Old admission messages may remain queued after epoch cleanup. *)
      if source = listener then admit context listener;
      loop context inbox listener listener_monitor
  | Actor.Exit (index, reason) ->
      match fst (List.nth watched index) with
      | State ->
          retire listener listener_monitor; stop_sessions context;
          Error (State_failed reason)
      | Listener ->
          stop_sessions context;
          if context.counters.restart_count >= context.config.max_restarts then
            (retire context.state context.state_monitor; Error Restart_limit)
          else (
            context.counters.restart_count <- increment
              context.counters.restart_count;
            match start_listener context with
            | Ok (pid, monitor) -> loop context inbox pid monitor
            | Error _ -> retire context.state context.state_monitor; Error
              Start_failed)
      | Worker session ->
          retire session.deadline session.deadline_monitor;
          remove context session;
          (match reason with
           | Actor.Normal -> context.counters.completed_count <-
               increment context.counters.completed_count
           | _ -> context.counters.failed_count <- increment
             context.counters.failed_count);
          loop context inbox listener listener_monitor
      | Deadline session ->
          retire session.worker session.monitor; remove context session;
          match reason with
          | Actor.Normal ->
              context.counters.timeout_count <- increment
                context.counters.timeout_count;
              loop context inbox listener listener_monitor
          | _ ->
              retire listener listener_monitor; stop_sessions context;
              retire context.state context.state_monitor; Error
                (Deadline_failed reason)

let serve config inbox =
  if not (valid config) then Error Invalid_config
  else match Actor.spawn_monitored (Kv_state.run
    ~max_entries:config.max_entries) with
    | Error _ -> Error Start_failed
    | Ok (state, state_monitor) ->
        let counters = { accepted_count = 0; completed_count = 0; failed_count
          = 0;
          timeout_count = 0; rejected_count = 0; restart_count = 0 } in
        let context = { config; root = Actor.self inbox; state; state_monitor;
          sessions = []; counters } in
        match start_listener context with
        | Ok (pid, monitor) -> loop context inbox pid monitor
        | Error _ -> retire state state_monitor; Error Start_failed
