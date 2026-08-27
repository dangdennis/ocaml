type command =
  | Ping
  | Get of string
  | Put of string * string

let classify = function
  | Ping -> 0
  | Get _ -> 1
  | Put _ -> 2
