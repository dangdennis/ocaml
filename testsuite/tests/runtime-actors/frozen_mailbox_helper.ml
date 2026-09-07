type message = {
  left : string;
  right : string;
  values : int array;
}

let shared = "frozen-mailbox"
let message = { left = shared; right = shared; values = [| 10; 20; 30 |] }
let closure () = ignore message

let unchanged () =
  message.left == message.right
  && message.left = "frozen-mailbox"
  && message.values = [| 10; 20; 30 |]
