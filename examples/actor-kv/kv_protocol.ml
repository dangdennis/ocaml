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

type command = Ping | Get of string | Set of string * string | Del of string
             | Quit | Shutdown | Crash | Crash_state

type reply = Attached | Pong | Value of string | Stored | Deleted | Missing
           | Full | Invalid

let max_key = 64
let max_value = 512
let max_line = max_key + max_value + 5

let key_valid key =
  let n = String.length key in
  n > 0 && n <= max_key &&
  let rec loop i = i = n ||
    (match key.[i] with
     | 'a'..'z' | 'A'..'Z' | '0'..'9' | '_' | '-' -> loop (i + 1)
     | _ -> false) in
  loop 0

let value_valid value =
  let n = String.length value in
  n <= max_value &&
  let rec loop i = i = n ||
    (let c = Char.code value.[i] in c >= 32 && c <= 126 && loop (i + 1)) in
  loop 0

let parse ~test_control line =
  let n = String.length line in
  if n > max_line then Error ()
  else if line = "PING" then Ok Ping
  else if line = "QUIT" then Ok Quit
  else if test_control && line = "SHUTDOWN" then Ok Shutdown
  else if test_control && line = "CRASH" then Ok Crash
  else if test_control && line = "CRASH_STATE" then Ok Crash_state
  else if n > 4 && String.sub line 0 4 = "GET " then
    let key = String.sub line 4 (n - 4) in
    if key_valid key then Ok (Get key) else Error ()
  else if n > 4 && String.sub line 0 4 = "DEL " then
    let key = String.sub line 4 (n - 4) in
    if key_valid key then Ok (Del key) else Error ()
  else if n > 4 && String.sub line 0 4 = "SET " then
    match String.index_from_opt line 4 ' ' with
    | None -> Error ()
    | Some i ->
        let key = String.sub line 4 (i - 4) in
        let value = String.sub line (i + 1) (n - i - 1) in
        if key_valid key && value_valid value then Ok (Set (key, value))
        else Error ()
  else Error ()

let render = function
  | Attached | Invalid -> "ERROR\n" | Pong -> "PONG\n"
  | Value value -> "VALUE " ^ value ^ "\n" | Stored -> "STORED\n"
  | Deleted -> "DELETED\n" | Missing -> "NOT_FOUND\n" | Full -> "FULL\n"
