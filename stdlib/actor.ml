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

type 'message pid = int
type 'message inbox = int
type monitor = int * int

type backtrace = {
  text : string;
  truncated : bool;
}

type exit_reason =
  | Normal
  | Uncaught_exception of { summary : string; backtrace : backtrace option }
  | Heap_limit
  | Mailbox_limit
  | Cancelled
  | Unsupported_operation of string
  | Runtime_failure of string

type monitor_error = Monitor_missing | Monitor_stale | Monitor_limit
type cancel_error = Cancel_missing | Cancel_stale | Cancel_self

type heap_limits = {
  initial_words : int;
  maximum_words : int;
}

let default_root_heap_limits = {
  initial_words = 1 lsl 18;
  maximum_words = 1 lsl 18;
}

let default_child_heap_limits = {
  initial_words = 1 lsl 16;
  maximum_words = 1 lsl 16;
}

type world_config = {
  root_heap : heap_limits;
  child_heap : heap_limits;
  max_actors : int;
  reductions_per_slice : int;
  max_message_words : int;
  max_mailbox_messages : int;
  max_mailbox_bytes : int;
  max_monitors : int;
  max_timers : int;
  max_net_resources : int;
  max_net_operations : int;
  max_net_buffered_bytes : int;
}

let default_world_config = {
  root_heap = default_root_heap_limits;
  child_heap = default_child_heap_limits;
  max_actors = 1_024;
  reductions_per_slice = 1_000;
  max_message_words = 1 lsl 16;
  max_mailbox_messages = 1 lsl 16;
  max_mailbox_bytes = 1 lsl 28;
  max_monitors = 1 lsl 16;
  max_timers = 1 lsl 16;
  max_net_resources = 1_024;
  max_net_operations = 1_024;
  max_net_buffered_bytes = 8 * 1024 * 1024;
}

type run_error =
  | Unsupported_runtime
  | Root_failed of string
  | Root_heap_exhausted
  | Deadlock

type spawn_error =
  | Actor_limit
  | Initial_heap_limit
  | Unsupported_capture of string

type spawn_monitored_error =
  | Monitored_spawn_error of spawn_error
  | Monitored_monitor_limit

type send_error =
  | No_such_actor
  | Message_too_large
  | Unsupported_message of string

type stats = {
  live_actors : int;
  runnable_actors : int;
  blocked_actors : int;
  total_spawned : int;
  total_exited : int;
  total_failed : int;
  total_dispatches : int;
  total_reduction_stops : int;
  messages_sent : int;
  messages_received : int;
  messages_dropped : int;
  mailbox_messages : int;
  mailbox_bytes : int;
  mailbox_quota_failures : int;
  current_heap_words : int;
  maximum_heap_words : int;
  heap_growths : int;
  actor_capacity : int;
  reduction_budget : int;
  message_word_limit : int;
  mailbox_message_limit : int;
  mailbox_byte_limit : int;
  monitors : int;
  peak_monitors : int;
  monitor_quota_failures : int;
  monitor_limit : int;
  timers : int;
  peak_timers : int;
  pending_timers : int;
  ready_timers : int;
  timers_expired : int;
  timers_cancelled : int;
  timer_quota_failures : int;
  timer_limit : int;
  net_resources : int;
  peak_net_resources : int;
  net_operations : int;
  peak_net_operations : int;
  net_buffered_bytes : int;
  peak_net_buffered_bytes : int;
  net_quota_failures : int;
  net_closed : int;
  net_errors : int;
  net_resource_limit : int;
  net_operation_limit : int;
  net_byte_limit : int;
}

type 'message run_request =
  int * int * int * int * ('message inbox -> unit)

external run_request : 'message run_request -> (unit, run_error) result
  = "caml_actor_run"

external run : ('message inbox -> unit) -> (unit, run_error) result
  = "caml_actor_run"

let run_with_heap_limits ~root ~child entry =
  run_request
    (root.initial_words, root.maximum_words,
     child.initial_words, child.maximum_words, entry)

type 'message configured_run_request =
  int * int * int * int * int * int * int * int * int * int * int *
  int * int * int *
  ('message inbox -> unit)

external configured_run_request : 'message configured_run_request ->
  (unit, run_error) result
  = "caml_actor_run"

let run_with_config config entry =
  configured_run_request
    (config.root_heap.initial_words, config.root_heap.maximum_words,
     config.child_heap.initial_words, config.child_heap.maximum_words,
     config.max_actors, config.reductions_per_slice,
     config.max_message_words, config.max_mailbox_messages,
     config.max_mailbox_bytes, config.max_monitors, config.max_timers,
     config.max_net_resources, config.max_net_operations,
     config.max_net_buffered_bytes, entry)

type 'message spawn_request =
  int * int * ('message inbox -> unit)

external spawn_request : 'message spawn_request ->
  ('message pid, spawn_error) result
  = "caml_actor_spawn"

external spawn : ('message inbox -> unit) ->
  ('message pid, spawn_error) result
  = "caml_actor_spawn"

let spawn_with_heap_limits limits entry =
  spawn_request
    (limits.initial_words, limits.maximum_words, entry)

type 'message monitored_spawn_request = int * ('message inbox -> unit)

external monitored_spawn_request : 'message monitored_spawn_request ->
  (('message pid * monitor), spawn_monitored_error) result
  = "caml_actor_spawn"

let spawn_monitored entry = monitored_spawn_request (2, entry)

type 'message monitor_request = int * 'message pid

external monitor_request : 'message monitor_request ->
  (monitor, monitor_error) result
  = "caml_actor_spawn"

let monitor pid = monitor_request (0, pid)

type 'message cancel_request = int * 'message pid

external cancel_request : 'message cancel_request ->
  (unit, cancel_error) result
  = "caml_actor_spawn"

let cancel pid = cancel_request (1, pid)

external self : 'message inbox -> 'message pid
  = "caml_actor_self"

external send : 'message pid -> 'message ->
  (unit, send_error) result
  = "caml_actor_send"

external receive : 'message inbox -> 'message
  = "caml_actor_receive"

external await_exit : monitor -> exit_reason
  = "caml_actor_receive"

type await_any_exit_request = int * monitor list

external await_any_exit_request : await_any_exit_request ->
  int * exit_reason
  = "caml_actor_receive"

let await_any_exit monitors = await_any_exit_request (1, monitors)

type 'message event = Message of 'message | Exit of int * exit_reason
external receive_or_exit_request : int * ('message inbox * monitor list) ->
  'message event = "caml_actor_receive"
let receive_or_exit inbox monitors = receive_or_exit_request (4, (inbox,
  monitors))

external yield : unit -> unit
  = "caml_actor_yield"

external stats : unit -> stats
  = "caml_actor_stats"

module Timer = struct
  type t = int
  type error = Invalid_duration | Timer_limit | Timer_unavailable
             | Invalid_timer | Timer_unsupported
  external after_request : int * float -> (t, error) result
    = "caml_actor_spawn"
  external cancel_request : int * t -> (bool, error) result
    = "caml_actor_spawn"
  external await_request : int * t -> (unit, error) result
    = "caml_actor_receive"
  let after seconds = after_request (3, seconds)
  let cancel timer = cancel_request (4, timer)
  let await timer = await_request (2, timer)
  let sleep seconds =
    if seconds = 0. then (yield (); Ok ())
    else match after seconds with Error e -> Error e | Ok t -> await t
end

module Gen_server = struct
  type error =
    | Invalid_timeout
    | Request_id_exhausted
    | Spawn_failed of spawn_monitored_error
    | Monitor_failed of monitor_error
    | Send_failed of send_error
    | Server_down of exit_reason
    | Deadline_failed of exit_reason
    | Helper_failed of exit_reason
    | Timeout

  type ('request, 'reply, 'cast, 'state) callbacks = {
    init : unit -> 'state;
    handle_call : 'request -> 'state -> 'reply * 'state;
    handle_cast : 'cast -> 'state -> 'state;
    terminate : 'state -> unit;
  }

  type 'reply answer = Answer of 'reply | Rejected of send_error
  type ('request, 'reply, 'cast) message =
    | Call of 'request * 'reply answer pid
    | Cast of 'cast
    | Stop
  type ('request, 'reply, 'cast) t =
    ('request, 'reply, 'cast) message pid
  type 'reply delivery = Started of int
                       | Completed of int * ('reply, error) result
  type 'reply client = {
    inbox : 'reply delivery inbox;
    mutable sequence : int;
  }

  let client inbox = ignore (self inbox); { inbox; sequence = 0 }
  let next client =
    ignore (self client.inbox);
    if client.sequence = max_int then Error Request_id_exhausted
    else (client.sequence <- client.sequence + 1; Ok client.sequence)

  let rec loop callbacks inbox state =
    match receive inbox with
    | Call (request, destination) ->
        let reply, state = callbacks.handle_call request state in
        (match send destination (Answer reply) with
         | Ok () -> ()
         | Error error -> ignore (send destination (Rejected error)));
        loop callbacks inbox state
    | Cast request ->
        loop callbacks inbox (callbacks.handle_cast request state)
    | Stop -> callbacks.terminate state

  let run callbacks inbox = loop callbacks inbox (callbacks.init ())
  let of_pid pid = pid
  let cast server request = send server (Cast request)
  let stop server = send server Stop
  let retire pid monitor = ignore (cancel pid); ignore (await_exit monitor)

  let rec startup client monitor id =
    match receive_or_exit client.inbox [monitor] with
    | Exit (_, reason) -> Error (Server_down reason)
    | Message (Started found) when found = id -> Ok ()
    | Message _ -> startup client monitor id

  let start client callbacks =
    match next client with
    | Error error -> Error error
    | Ok id ->
        let destination = self client.inbox in
        match spawn_monitored (fun inbox ->
          let state = callbacks.init () in
          (match send destination (Started id) with
           | Error _ -> failwith "Gen_server startup acknowledgement failed"
           | Ok () -> loop callbacks inbox state)) with
        | Error error -> Error (Spawn_failed error)
        | Ok (server, monitor) ->
            match startup client monitor id with
            | Error error -> Error error
            | Ok () -> Ok (server, monitor)

  let rec parked inbox owner =
    match receive_or_exit inbox [owner] with
    | Exit _ -> ()
    | Message _ -> parked inbox owner

  let deliver inbox destination owner id result =
    match send destination (Completed (id, result)) with
    | Error _ -> failwith "Gen_server result delivery failed"
    | Ok () -> parked inbox owner

  let wait_reply inbox server timer owner =
    match receive_or_exit inbox [server; timer; owner] with
    | Message (Answer reply) -> Some (Ok reply)
    | Message (Rejected error) -> Some (Error (Send_failed error))
    | Exit (0, reason) -> Some (Error (Server_down reason))
    | Exit (1, Normal) -> Some (Error Timeout)
    | Exit (1, reason) -> Some (Error (Deadline_failed reason))
    | Exit _ -> None

  let request inbox server timer request =
    match send timer () with
    | Error error -> Some (Error (Send_failed error))
    | Ok () ->
        match send server (Call (request, self inbox)) with
        | Error error -> Some (Error (Send_failed error))
        | Ok () -> None

  let broker inbox destination id server timer argument =
    match monitor destination with
    | Error _ -> ignore (cancel timer)
    | Ok owner ->
        let finish result =
          ignore (cancel timer);
          deliver inbox destination owner id result
        in
        match monitor server, monitor timer with
        | Error error, _ | _, Error error ->
            finish (Error (Monitor_failed error))
        | Ok server_monitor, Ok timer_monitor ->
            let result = match request inbox server timer argument with
              | Some result -> Some result
              | None -> wait_reply inbox server_monitor timer_monitor owner
            in
            match result with
            | None -> ignore (cancel timer)
            | Some result -> finish result

  let rec wait client id monitor =
    match receive_or_exit client.inbox [monitor] with
    | Exit (_, reason) -> Error (Helper_failed reason)
    | Message (Completed (found, result)) when found = id -> result
    | Message _ -> wait client id monitor

  let call_with_id client server argument timeout id =
    match spawn_monitored (fun inbox ->
      let () = receive inbox in
      match Timer.sleep timeout with
      | Ok () -> ()
      | Error _ -> failwith "Gen_server deadline registration failed") with
    | Error error -> Error (Spawn_failed error)
    | Ok (timer, timer_monitor) ->
        let destination = self client.inbox in
        match spawn_monitored (fun inbox ->
          broker inbox destination id server timer argument) with
        | Error error ->
            retire timer timer_monitor;
            Error (Spawn_failed error)
        | Ok (helper, helper_monitor) ->
            let result = wait client id helper_monitor in
            (* [wait] consumes the monitor only on the helper-exit path. *)
            (match result with
             | Error (Helper_failed _) -> ()
             | _ -> retire helper helper_monitor);
            retire timer timer_monitor;
            result

  external compare_float : float -> float -> int = "%compare"

  let call client server argument ~timeout =
    if compare_float timeout 0. < 0 ||
       compare_float timeout 86400. > 0 ||
       compare_float timeout nan = 0
    then Error Invalid_timeout
    else match next client with
      | Error error -> Error error
      | Ok id -> call_with_id client server argument timeout id
end

module Net = struct
  type listener = int
  type socket = int
  type error = Invalid_argument | Invalid_handle | Descriptor_limit
             | Operation_limit | Buffer_limit | Resource_unavailable
             | Unsupported_platform | No_such_actor | Mailbox_limit
             | Transfer_self | Busy | Io_error of int

  external request : int * (int * int * 'buffer * int * int * int) ->
    ('a, error) result = "caml_actor_receive"

  let listen ~address ~port =
    request (3, (0, 0, address, port, 0, 0))

  let local_port listener = request (3, (1, listener, "", 0, 0, 0))
  let accept listener = request (3, (2, listener, "", 0, 0, 0))
  let read socket bytes offset length =
    request (3, (3, socket, bytes, offset, length, 0))

  let write socket bytes offset length =
    request (3, (4, socket, bytes, offset, length, 0))

  let close socket = request (3, (5, socket, "", 0, 0, 0))
  let close_listener listener = request (3, (6, listener, "", 0, 0, 0))
  let transfer socket destination =
    request (3, (7, socket, "", 0, 0, destination))
  let transfer_with socket destination message =
    request (3, (8, socket, message, 0, 0, destination))
end

module Supervisor = struct
  type restart = Permanent | Transient | Temporary

  type child = Child : {
    id : string;
    restart : restart;
    start : int -> 'message inbox -> unit;
    on_start : 'message pid -> unit;
    on_exit : exit_reason -> unit;
  } -> child

  type intensity = {
    max_restarts : int;
    within : int;
  }

  type error =
    | Invalid_configuration of string
    | Initial_start_failed of string * spawn_monitored_error
    | Restart_failed of string * spawn_monitored_error
    | Restart_intensity_exceeded of string
    | Restart_attempt_exhausted of string
    | Clock_moved_backwards

  type running = Running : {
    id : string;
    restart : restart;
    start : int -> 'message inbox -> unit;
    on_start : 'message pid -> unit;
    on_exit : exit_reason -> unit;
    attempt : int;
    pid : 'message pid;
    monitor : monitor;
  } -> running

  let rec reverse_append source destination =
    match source with
    | [] -> destination
    | head :: tail -> reverse_append tail (head :: destination)

  let reverse source = reverse_append source []

  let rec has_id id = function
    | [] -> false
    | Child child :: tail -> id = child.id || has_id id tail

  let validate intensity children =
    if intensity.max_restarts <= 0 then
      Error (Invalid_configuration "max_restarts must be positive")
    else if intensity.within <= 0 then
      Error (Invalid_configuration "within must be positive")
    else
      let rec loop seen = function
        | [] ->
            if seen = [] then
              Error (Invalid_configuration "at least one child is required")
            else Ok ()
        | Child child :: tail ->
            if child.id = "" then
              Error (Invalid_configuration "child id must not be empty")
            else if has_id child.id seen then
              Error (Invalid_configuration "child ids must be unique")
            else loop (Child child :: seen) tail
      in
      loop [] children

  let spawn_child (Child child) attempt =
    match spawn_monitored (child.start attempt) with
    | Error error -> Error error
    | Ok (pid, monitor) ->
        child.on_start pid;
        Ok (Running {
          id = child.id;
          restart = child.restart;
          start = child.start;
          on_start = child.on_start;
          on_exit = child.on_exit;
          attempt;
          pid;
          monitor;
        })

  let restart_child (Running child) =
    match spawn_monitored (child.start (child.attempt + 1)) with
    | Error error -> Error error
    | Ok (pid, monitor) ->
        child.on_start pid;
        Ok (Running {
          id = child.id;
          restart = child.restart;
          start = child.start;
          on_start = child.on_start;
          on_exit = child.on_exit;
          attempt = child.attempt + 1;
          pid;
          monitor;
        })

  let running_id (Running child) = child.id
  let running_monitor (Running child) = child.monitor
  let running_attempt (Running child) = child.attempt

  let abnormal = function
    | Normal | Cancelled -> false
    | Uncaught_exception _ | Heap_limit | Mailbox_limit
    | Unsupported_operation _ | Runtime_failure _ -> true

  let should_restart (Running child) reason =
    match child.restart with
    | Permanent -> true
    | Transient -> abnormal reason
    | Temporary -> false

  let notify_exit (Running child) reason = child.on_exit reason

  let rec cancel_running = function
    | [] -> ()
    | Running child :: tail ->
        begin match cancel child.pid with
        | Ok () | Error Cancel_missing | Error Cancel_stale -> ()
        | Error Cancel_self -> invalid_arg "Actor.Supervisor.cancel self"
        end;
        let reason = await_exit child.monitor in
        child.on_exit reason;
        cancel_running tail

  let shutdown running = cancel_running (reverse running)

  let rec start_all started = function
    | [] -> Ok (reverse started)
    | (Child child as spec) :: tail ->
        begin match spawn_child spec 0 with
        | Ok running -> start_all (running :: started) tail
        | Error error ->
            cancel_running started;
            Error (Initial_start_failed (child.id, error))
        end

  let rec monitors = function
    | [] -> []
    | running :: tail -> running_monitor running :: monitors tail

  let rec split_at index before = function
    | [] -> invalid_arg "Actor.Supervisor.await_any_exit index"
    | head :: tail ->
        if index = 0 then (head, before, tail)
        else split_at (index - 1) (head :: before) tail

  let keep_recent cutoff history =
    let rec loop kept count = function
      | [] -> (kept, count)
      | timestamp :: tail ->
          if timestamp >= cutoff then loop (timestamp :: kept) (count + 1) tail
          else loop kept count tail
    in
    loop [] 0 history

  let record_restart clock intensity last_time history id =
    let now = clock () in
    if now < 0 || now < last_time then Error Clock_moved_backwards
    else
      let recent, recent_count =
        keep_recent (now - intensity.within) history
      in
      if recent_count >= intensity.max_restarts then
        Error (Restart_intensity_exceeded id)
      else Ok (now, now :: recent)

  let run_one_for_one ~clock ~intensity children =
    match validate intensity children with
    | Error _ as error -> error
    | Ok () ->
        begin match start_all [] children with
        | Error _ as error -> error
        | Ok initial ->
            let rec loop last_time history running =
              match running with
              | [] -> Ok ()
              | _ ->
                  let index, reason = await_any_exit (monitors running) in
                  let exited, before, after = split_at index [] running in
                  notify_exit exited reason;
                  let remaining = reverse_append before after in
                  if not (should_restart exited reason) then
                    loop last_time history remaining
                  else if running_attempt exited = max_int then begin
                    shutdown remaining;
                    Error (Restart_attempt_exhausted (running_id exited))
                  end
                  else
                    begin match
                      record_restart clock intensity last_time history
                        (running_id exited)
                    with
                    | Error error ->
                        shutdown remaining;
                        Error error
                    | Ok (next_time, next_history) ->
                        begin match restart_child exited with
                        | Error error ->
                            shutdown remaining;
                            Error (Restart_failed (running_id exited, error))
                        | Ok replacement ->
                            let next =
                              reverse_append before (replacement :: after)
                            in
                            loop next_time next_history next
                        end
                    end
            in
            loop (-1) [] initial
        end
end
