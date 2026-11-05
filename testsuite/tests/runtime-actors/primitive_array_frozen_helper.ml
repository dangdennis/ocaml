let destination = [| 41; 42; 43 |]

let set () = destination.(0) <- 0
let fill () = Array.fill destination 0 2 0

let unchanged () =
  destination.(0) = 41 && destination.(1) = 42 && destination.(2) = 43
