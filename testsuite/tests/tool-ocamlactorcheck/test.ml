(* TEST
 readonly_files = "fixture_helper.ml fixture.ml";
 not-windows;
 setup-ocamlc.byte-build-env;
 all_modules = "test.ml";
 ocamlc.byte;
 run;
 check-program-output;
*)

let input_binary_int_from_bytes bytes offset =
  let byte index = Char.code (Bytes.get bytes (offset + index)) in
  (byte 0 lsl 24) lor (byte 1 lsl 16) lor (byte 2 lsl 8) lor byte 3

let find_sections bytes =
  let length = Bytes.length bytes in
  if length < 16 then failwith "truncated bytecode executable";
  let section_count = input_binary_int_from_bytes bytes (length - 16) in
  let toc_start = length - 16 - (8 * section_count) in
  if section_count < 0 || toc_start < 0 then failwith "invalid section table";
  let total_section_bytes = ref 0 in
  for index = 0 to section_count - 1 do
    let entry = toc_start + (8 * index) in
    total_section_bytes :=
      !total_section_bytes + input_binary_int_from_bytes bytes (entry + 4)
  done;
  let section_start = ref (toc_start - !total_section_bytes) in
  let sections = ref [] in
  for index = 0 to section_count - 1 do
    let entry = toc_start + (8 * index) in
    let name = Bytes.sub_string bytes entry 4 in
    let section_length = input_binary_int_from_bytes bytes (entry + 4) in
    sections := (name, !section_start, section_length, entry) :: !sections;
    section_start := !section_start + section_length
  done;
  toc_start, List.rev !sections

let set_int32_le bytes offset value =
  for index = 0 to 3 do
    Bytes.set bytes (offset + index)
      (Char.chr ((value lsr (8 * index)) land 0xff))
  done

let set_int32_be bytes offset value =
  for index = 0 to 3 do
    Bytes.set bytes (offset + index)
      (Char.chr ((value lsr (8 * (3 - index))) land 0xff))
  done

let find_section name sections =
  match
    List.find_opt
      (fun (candidate, _, _, _) -> candidate = name)
      sections
  with
  | Some section -> section
  | None -> failwith ("missing " ^ name ^ " section")

let replace_section bytes toc_start sections name replacement =
  let _, start, length, _ = find_section name sections in
  let output_length = Bytes.length bytes - length + Bytes.length replacement in
  let output = Bytes.create output_length in
  Bytes.blit bytes 0 output 0 start;
  Bytes.blit replacement 0 output start (Bytes.length replacement);
  Bytes.blit bytes (start + length) output (start + Bytes.length replacement)
    (Bytes.length bytes - start - length);
  let new_toc_start = toc_start - length + Bytes.length replacement in
  let target_index =
    let rec loop index = function
      | [] -> assert false
      | (candidate, _, _, _) :: _ when candidate = name -> index
      | _ :: rest -> loop (index + 1) rest
    in
    loop 0 sections
  in
  set_int32_be output (new_toc_start + (8 * target_index) + 4)
    (Bytes.length replacement);
  output

let write_bytes filename bytes =
  let output = open_out_bin filename in
  output_bytes output bytes;
  close_out output

let () =
  if Array.length Sys.argv <> 4 then
    failwith "usage: test MODE SOURCE DESTINATION";
  let mode = Sys.argv.(1) in
  let source = Sys.argv.(2) in
  let destination = Sys.argv.(3) in
  let input = open_in_bin source in
  let length = in_channel_length input in
  let bytes = really_input_string input length |> Bytes.of_string in
  close_in input;
  let toc_start, sections = find_sections bytes in
  match mode with
  | "code" ->
      let _, code_start, code_length, _ = find_section "CODE" sections in
      if code_length < 4 then failwith "CODE section is too short";
      (* Replace the final STOP with GETGLOBAL but leave out its operand. *)
      set_int32_le bytes (code_start + code_length - 4) 53;
      write_bytes destination bytes
  | "data" | "symb" ->
      let replacement = Marshal.to_bytes 0 [] in
      let name = String.uppercase_ascii mode in
      replace_section bytes toc_start sections name replacement
      |> write_bytes destination
  | _ -> failwith ("unknown mutation mode: " ^ mode)
