(* TEST
 modules = "frozen_mailbox_helper.ml";
 {
   linux;
   arch_amd64;
   bytecode;
 }
*)

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

type cycle = Node of cycle option ref

let require_ok stage = function
  | Ok () -> ()
  | Error Actor.Unsupported_runtime -> failwith (stage ^ ": unsupported runtime")
  | Error Actor.Root_heap_exhausted -> failwith (stage ^ ": root heap exhausted")
  | Error Actor.Deadlock -> failwith (stage ^ ": deadlock")
  | Error (Actor.Root_failed message) -> failwith (stage ^ ": " ^ message)

let send_or_fail pid message =
  match Actor.send pid message with
  | Ok () -> ()
  | Error _ -> ignore (1 / 0)

let () =
  require_ok "direct" (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    match Actor.spawn (fun child_inbox ->
      let direct : Frozen_mailbox_helper.message =
        Actor.receive child_inbox
      in
      assert (direct.left == direct.right);
      assert (direct.values = [| 10; 20; 30 |]);
      direct.values.(0) <- 99;
      assert (direct.values.(0) = 99);
      send_or_fail root ()) with
    | Error _ -> ignore (1 / 0)
    | Ok child ->
        send_or_fail child Frozen_mailbox_helper.message;
        ignore (Actor.receive root_inbox)));
  require_ok "mixed" (Actor.run (fun root_inbox ->
    let root = Actor.self root_inbox in
    match Actor.spawn (fun child_inbox ->
      let (first, second, received_cycle :
          Frozen_mailbox_helper.message
          * Frozen_mailbox_helper.message * cycle) =
        Actor.receive child_inbox
      in
      assert (first == second);
      assert (first.left == first.right);
      begin match received_cycle with
      | Node link ->
          begin match !link with
          | Some linked -> assert (linked == received_cycle)
          | None -> assert false
          end
      end;
      first.values.(1) <- 77;
      assert (second.values.(1) = 77);
      send_or_fail root ()) with
    | Error _ -> ignore (1 / 0)
    | Ok child ->
        let link = ref None in
        let cycle = Node link in
        link := Some cycle;
        let frozen = Frozen_mailbox_helper.message in
        send_or_fail child (frozen, frozen, cycle);
        ignore (Actor.receive root_inbox)));
  require_ok "rejected closure" (Actor.run (fun _ ->
    match Actor.spawn (fun _ -> ()) with
    | Error _ -> ignore (1 / 0)
    | Ok child ->
        begin match Actor.send child Frozen_mailbox_helper.closure with
        | Error (Actor.Unsupported_message _) -> ()
        | _ -> ignore (1 / 0)
        end));
  assert (Frozen_mailbox_helper.unchanged ());
  print_endline "frozen mailbox graph copy: ok"
