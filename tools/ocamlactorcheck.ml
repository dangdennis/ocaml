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

open Bytecode_shape
open Opcodes
open Opnames

type finding =
  | Global_read of {
      pc : int;
      opcode : string;
      global : int;
      field : int option;
    }
  | Global_write of {
      pc : int;
      global : int;
    }
  | Primitive_call of {
      pc : int;
      opcode : string;
      primitive : int;
      arity : int;
    }

exception Malformed of string

let malformed pc format =
  Printf.ksprintf
    (fun detail ->
      raise (Malformed (Printf.sprintf "at pc=%d: %s" pc detail)))
    format

let instruction_name pc opcode =
  if opcode < 0 || opcode >= Array.length names_of_instructions then
    malformed pc "unknown opcode %d" opcode;
  names_of_instructions.(opcode)

let read_word ic stop pc opcode operand =
  if stop - pos_in ic < 4 then
    malformed pc "truncated %s %s" opcode operand;
  inputu ic

let read_signed_word ic stop pc opcode operand =
  if stop - pos_in ic < 4 then
    malformed pc "truncated %s %s" opcode operand;
  inputs ic

let skip_words ic stop pc opcode count =
  let remaining = (stop - pos_in ic) / 4 in
  if count < 0 || count > remaining then
    malformed pc "truncated %s operands" opcode;
  seek_in ic (pos_in ic + (4 * count))

let check_index pc kind length index =
  if index < 0 || index >= length then
    malformed pc "%s index %d is out of range 0..%d"
      kind index (length - 1)

let primitive_arity opcode =
  if opcode = opC_CALL1 then Some 1
  else if opcode = opC_CALL2 then Some 2
  else if opcode = opC_CALL3 then Some 3
  else if opcode = opC_CALL4 then Some 4
  else if opcode = opC_CALL5 then Some 5
  else None

let scan_code ic code_size global_count primitive_count =
  let start = pos_in ic in
  if code_size < 0 || code_size > max_int - start then
    raise (Malformed "invalid CODE section length");
  let stop = start + code_size in
  if code_size mod 4 <> 0 then
    raise (Malformed "CODE section length is not a multiple of four");
  let findings = ref [] in
  while pos_in ic < stop do
    let pc = (pos_in ic - start) / 4 in
    let opcode = read_word ic stop pc "instruction" "word" in
    let name = instruction_name pc opcode in
    let shape =
      match List.assoc_opt opcode op_shapes with
      | Some shape -> shape
      | None -> malformed pc "missing operand shape for %s" name
    in
    if opcode = opGETGLOBAL || opcode = opPUSHGETGLOBAL then begin
      let global = read_word ic stop pc name "global operand" in
      check_index pc "global" global_count global;
      findings :=
        Global_read {pc; opcode = name; global; field = None} :: !findings
    end else if opcode = opGETGLOBALFIELD
               || opcode = opPUSHGETGLOBALFIELD then begin
      let global = read_word ic stop pc name "global operand" in
      let field = read_word ic stop pc name "field operand" in
      check_index pc "global" global_count global;
      findings :=
        Global_read {pc; opcode = name; global; field = Some field}
        :: !findings
    end else if opcode = opSETGLOBAL then begin
      let global = read_word ic stop pc name "global operand" in
      check_index pc "global" global_count global;
      findings := Global_write {pc; global} :: !findings
    end else match primitive_arity opcode with
    | Some arity ->
        let primitive = read_word ic stop pc name "primitive operand" in
        check_index pc "primitive" primitive_count primitive;
        findings :=
          Primitive_call {pc; opcode = name; primitive; arity} :: !findings
    | None when opcode = opC_CALLN ->
        let arity = read_word ic stop pc name "arity operand" in
        let primitive = read_word ic stop pc name "primitive operand" in
        check_index pc "primitive" primitive_count primitive;
        findings :=
          Primitive_call {pc; opcode = name; primitive; arity} :: !findings
    | None ->
        match shape with
        | Nothing -> ()
        | Uint | Getglobal | Setglobal | Primitive ->
            ignore (read_word ic stop pc name "operand")
        | Sint | Disp ->
            ignore (read_signed_word ic stop pc name "operand")
        | Uint_Uint | Getglobal_Uint | Uint_Primitive ->
            ignore (read_word ic stop pc name "first operand");
            ignore (read_word ic stop pc name "second operand")
        | Uint_Disp ->
            ignore (read_word ic stop pc name "first operand");
            ignore (read_signed_word ic stop pc name "displacement")
        | Sint_Disp ->
            ignore (read_signed_word ic stop pc name "first operand");
            ignore (read_signed_word ic stop pc name "displacement")
        | Pubmet ->
            ignore (read_signed_word ic stop pc name "tag operand");
            ignore (read_word ic stop pc name "cache operand")
        | Switch ->
            let sizes = read_word ic stop pc name "size operand" in
            let integer_cases = sizes land 0xffff in
            let tag_cases = sizes lsr 16 in
            skip_words ic stop pc name (integer_cases + tag_cases)
        | Closurerec ->
            let functions = read_word ic stop pc name "function count" in
            ignore (read_word ic stop pc name "variable count");
            skip_words ic stop pc name functions
  done;
  List.rev !findings

let obj_is_block obj tag size =
  not (Obj.is_int obj) && Obj.tag obj = tag && Obj.size obj = size

let obj_as_nonnegative_int section description obj =
  if not (Obj.is_int obj) then
    raise (Malformed
      (Printf.sprintf "%s section %s is not an integer"
        section description));
  let value : int = Obj.obj obj in
  if value < 0 then
    raise (Malformed
      (Printf.sprintf "%s section %s is negative"
        section description));
  value

let validate_data_root obj =
  (* OCaml arrays use the ordinary tag-zero scanned-block representation.
     Other tag-zero blocks are safe to traverse as arrays, too; the important
     property here is to reject roots on which typed Array operations would be
     invalid. *)
  if Obj.is_int obj || Obj.tag obj <> 0 then
    raise (Malformed "DATA section root is not an array");
  (Obj.obj obj : Obj.t array)

module Seen_objects = Hashtbl.Make (struct
  type t = Obj.t
  let equal left right = left == right
  (* Identity is the only relevant property.  A constant hash avoids
     structurally traversing a representation that is still being checked. *)
  let hash _ = 0
end)

let validate_global_map data_length obj =
  (* [Symtable.global_map] is [GlobalMap.t], whose representation is the
     two-field record [{ cnt; tbl }].  [tbl] is a Stdlib Map tree: [Empty]
     is integer zero and [Node { l; v; d; r; h }] is a five-field tag-zero
     block.  Validate that representation before handing it to the abstract
     Symtable API, so a malformed marshal root cannot make pattern matching
     dereference an immediate or a short block. *)
  if not (obj_is_block obj 0 2) then
    raise (Malformed "SYMB section root is not a global map");
  let count =
    obj_as_nonnegative_int "SYMB" "global count" (Obj.field obj 0)
  in
  let seen = Seen_objects.create 127 in
  let rec validate_tree depth tree =
    if Obj.is_int tree then begin
      let constructor : int = Obj.obj tree in
      if constructor <> 0 then
        raise (Malformed "SYMB section map has an invalid empty constructor");
      0, 0
    end else begin
      if depth > 1024 then
        raise (Malformed "SYMB section map is too deep");
      if not (obj_is_block tree 0 5) then
        raise (Malformed "SYMB section map has an invalid node");
      if Seen_objects.mem seen tree then
        raise (Malformed "SYMB section map is cyclic or shares a node");
      Seen_objects.add seen tree ();
      let key = Obj.field tree 1 in
      if Obj.is_int key
         || (Obj.tag key <> 0 && Obj.tag key <> 1)
         || Obj.size key <> 1
         || Obj.is_int (Obj.field key 0)
         || Obj.tag (Obj.field key 0) <> Obj.string_tag
      then
        raise (Malformed "SYMB section map has an invalid global name");
      let index =
        obj_as_nonnegative_int "SYMB" "global index" (Obj.field tree 2)
      in
      if index >= data_length then
        raise (Malformed
          (Printf.sprintf
            "SYMB section global index %d is outside DATA" index));
      let height =
        obj_as_nonnegative_int "SYMB" "map height" (Obj.field tree 4)
      in
      let left_height, left_nodes =
        validate_tree (depth + 1) (Obj.field tree 0)
      in
      let right_height, right_nodes =
        validate_tree (depth + 1) (Obj.field tree 3)
      in
      let expected_height = 1 + max left_height right_height in
      if height <> expected_height || abs (left_height - right_height) > 2 then
        raise (Malformed "SYMB section map has invalid balance metadata");
      expected_height, 1 + left_nodes + right_nodes
    end
  in
  let _height, nodes = validate_tree 0 (Obj.field obj 1) in
  if nodes > count then
    raise (Malformed "SYMB section map contains more globals than its count");
  (Obj.obj obj : Symtable.global_map)

let global_symbols toc ic =
  let initial_data_obj : Obj.t =
    Bytesections.read_section_struct toc ic Bytesections.Name.DATA
  in
  let initial_data = validate_data_root initial_data_obj in
  let symbols = Array.make (Array.length initial_data) None in
  let global_map_obj : Obj.t =
    Bytesections.read_section_struct toc ic Bytesections.Name.SYMB
  in
  let global_map =
    validate_global_map (Array.length initial_data) global_map_obj
  in
  Symtable.iter_global_map
    (fun global index ->
      if index < 0 || index >= Array.length symbols then
        raise (Malformed
          (Printf.sprintf "SYMB global index %d is outside DATA" index));
      symbols.(index) <- Some (Symtable.Global.name global))
    global_map;
  symbols

let primitive_names toc ic =
  Bytesections.read_section_string toc ic Bytesections.Name.PRIM
  |> Misc.split_null_terminated
  |> Array.of_list

let print_global symbols index =
  Printf.printf " global=%d" index;
  match symbols.(index) with
  | None -> Printf.printf " symbol=<unnamed>"
  | Some symbol -> Printf.printf " symbol=%S" symbol

let print_finding symbols primitives = function
  | Global_read {pc; opcode; global; field} ->
      Printf.printf "pc=%d opcode=%s" pc opcode;
      print_global symbols global;
      Option.iter (Printf.printf " field=%d") field;
      Printf.printf " classification=global-read\n"
  | Global_write {pc; global} ->
      Printf.printf "pc=%d opcode=SETGLOBAL" pc;
      print_global symbols global;
      Printf.printf " classification=initialization-or-actor-forbidden\n"
  | Primitive_call {pc; opcode; primitive; arity} ->
      let name = primitives.(primitive) in
      Printf.printf "pc=%d opcode=%s primitive=%d:%S arity=%d"
        pc opcode primitive name arity;
      begin match Actor_primitive_policy.classify name arity with
      | Allowed entry ->
          Printf.printf " classification=%s family=%s\n"
            (Actor_primitive_policy.capability_name entry.capability)
            entry.family
      | Denied entry ->
          Printf.printf " classification=forbidden family=%s\n" entry.family
      | Arity_mismatch entry ->
          Printf.printf
            " classification=forbidden reason=arity-mismatch expected=%d \
             declared=%s family=%s\n"
            entry.arity
            (Actor_primitive_policy.capability_name entry.capability)
            entry.family
      | Unknown ->
          Printf.printf
            " classification=forbidden reason=unknown-primitive\n"
      end

let inspect filename =
  let ic = open_in_bin filename in
  Fun.protect ~finally:(fun () -> close_in_noerr ic) (fun () ->
    let toc = Bytesections.read_toc ic in
    let symbols = global_symbols toc ic in
    let primitives = primitive_names toc ic in
    let code_size =
      Bytesections.seek_section toc ic Bytesections.Name.CODE
    in
    let findings =
      scan_code ic code_size (Array.length symbols) (Array.length primitives)
    in
    Printf.printf "ocamlactorcheck: advisory bytecode inventory for %s\n"
      filename;
    List.iter (print_finding symbols primitives) findings;
    Printf.printf "notice: runtime actor enforcement remains authoritative\n")

let options = [
  "-args", Arg.Expand Arg.read_arg,
    "<file> Read additional newline separated command line arguments";
  "-args0", Arg.Expand Arg.read_arg0,
    "<file> Read additional NUL separated command line arguments";
]

let main () =
  let filename = ref None in
  let set_filename value =
    match !filename with
    | None -> filename := Some value
    | Some _ -> raise (Arg.Bad "only one bytecode executable is supported")
  in
  let usage = "ocamlactorcheck [OPTIONS] BYTECODE-EXECUTABLE" in
  try
    Arg.parse_expand options set_filename usage;
    begin match !filename with
    | None -> Arg.usage options usage; exit 2
    | Some filename -> inspect filename
    end
  with
  | Malformed detail ->
      Printf.eprintf "ocamlactorcheck: malformed bytecode %s\n" detail;
      exit 2
  | Bytesections.Bad_magic_number ->
      Printf.eprintf
        "ocamlactorcheck: malformed bytecode executable: bad magic\n";
      exit 2
  | Not_found ->
      Printf.eprintf
        "ocamlactorcheck: malformed bytecode executable: missing section\n";
      exit 2
  | End_of_file ->
      Printf.eprintf
        "ocamlactorcheck: malformed bytecode executable: truncated\n";
      exit 2
  | Failure detail | Invalid_argument detail ->
      Printf.eprintf "ocamlactorcheck: malformed bytecode executable: %s\n"
        detail;
      exit 2
  | Sys_error detail ->
      Printf.eprintf "ocamlactorcheck: cannot read input: %s\n" detail;
      exit 2

let () = main ()
