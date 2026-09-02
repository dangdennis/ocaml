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

open Opcodes

type shape =
  | Nothing
  | Uint
  | Sint
  | Uint_Uint
  | Disp
  | Uint_Disp
  | Sint_Disp
  | Getglobal
  | Getglobal_Uint
  | Setglobal
  | Primitive
  | Uint_Primitive
  | Switch
  | Closurerec
  | Pubmet

let inputu ic =
  let b1 = input_byte ic in
  let b2 = input_byte ic in
  let b3 = input_byte ic in
  let b4 = input_byte ic in
  (b4 lsl 24) + (b3 lsl 16) + (b2 lsl 8) + b1

let inputs ic =
  let b1 = input_byte ic in
  let b2 = input_byte ic in
  let b3 = input_byte ic in
  let b4 = input_byte ic in
  let b4' = if b4 >= 128 then b4 - 256 else b4 in
  (b4' lsl 24) + (b3 lsl 16) + (b2 lsl 8) + b1

let op_shapes = [
  opACC0, Nothing;
  opACC1, Nothing;
  opACC2, Nothing;
  opACC3, Nothing;
  opACC4, Nothing;
  opACC5, Nothing;
  opACC6, Nothing;
  opACC7, Nothing;
  opACC, Uint;
  opPUSH, Nothing;
  opPUSHACC0, Nothing;
  opPUSHACC1, Nothing;
  opPUSHACC2, Nothing;
  opPUSHACC3, Nothing;
  opPUSHACC4, Nothing;
  opPUSHACC5, Nothing;
  opPUSHACC6, Nothing;
  opPUSHACC7, Nothing;
  opPUSHACC, Uint;
  opPOP, Uint;
  opASSIGN, Uint;
  opENVACC1, Nothing;
  opENVACC2, Nothing;
  opENVACC3, Nothing;
  opENVACC4, Nothing;
  opENVACC, Uint;
  opPUSHENVACC1, Nothing;
  opPUSHENVACC2, Nothing;
  opPUSHENVACC3, Nothing;
  opPUSHENVACC4, Nothing;
  opPUSHENVACC, Uint;
  opPUSH_RETADDR, Disp;
  opAPPLY, Uint;
  opAPPLY1, Nothing;
  opAPPLY2, Nothing;
  opAPPLY3, Nothing;
  opAPPTERM, Uint_Uint;
  opAPPTERM1, Uint;
  opAPPTERM2, Uint;
  opAPPTERM3, Uint;
  opRETURN, Uint;
  opRESTART, Nothing;
  opGRAB, Uint;
  opCLOSURE, Uint_Disp;
  opCLOSUREREC, Closurerec;
  opOFFSETCLOSUREM3, Nothing;
  opOFFSETCLOSURE0, Nothing;
  opOFFSETCLOSURE3, Nothing;
  opOFFSETCLOSURE, Sint;
  opPUSHOFFSETCLOSUREM3, Nothing;
  opPUSHOFFSETCLOSURE0, Nothing;
  opPUSHOFFSETCLOSURE3, Nothing;
  opPUSHOFFSETCLOSURE, Sint;
  opGETGLOBAL, Getglobal;
  opPUSHGETGLOBAL, Getglobal;
  opGETGLOBALFIELD, Getglobal_Uint;
  opPUSHGETGLOBALFIELD, Getglobal_Uint;
  opSETGLOBAL, Setglobal;
  opATOM0, Nothing;
  opATOM, Uint;
  opPUSHATOM0, Nothing;
  opPUSHATOM, Uint;
  opMAKEBLOCK, Uint_Uint;
  opMAKEBLOCK1, Uint;
  opMAKEBLOCK2, Uint;
  opMAKEBLOCK3, Uint;
  opMAKEFLOATBLOCK, Uint;
  opGETFIELD0, Nothing;
  opGETFIELD1, Nothing;
  opGETFIELD2, Nothing;
  opGETFIELD3, Nothing;
  opGETFIELD, Uint;
  opGETFLOATFIELD, Uint;
  opSETFIELD0, Nothing;
  opSETFIELD1, Nothing;
  opSETFIELD2, Nothing;
  opSETFIELD3, Nothing;
  opSETFIELD, Uint;
  opSETFLOATFIELD, Uint;
  opVECTLENGTH, Nothing;
  opGETVECTITEM, Nothing;
  opSETVECTITEM, Nothing;
  opGETSTRINGCHAR, Nothing;
  opGETBYTESCHAR, Nothing;
  opSETBYTESCHAR, Nothing;
  opBRANCH, Disp;
  opBRANCHIF, Disp;
  opBRANCHIFNOT, Disp;
  opSWITCH, Switch;
  opBOOLNOT, Nothing;
  opPUSHTRAP, Disp;
  opPOPTRAP, Nothing;
  opRAISE, Nothing;
  opCHECK_SIGNALS, Nothing;
  opC_CALL1, Primitive;
  opC_CALL2, Primitive;
  opC_CALL3, Primitive;
  opC_CALL4, Primitive;
  opC_CALL5, Primitive;
  opC_CALLN, Uint_Primitive;
  opCONST0, Nothing;
  opCONST1, Nothing;
  opCONST2, Nothing;
  opCONST3, Nothing;
  opCONSTINT, Sint;
  opPUSHCONST0, Nothing;
  opPUSHCONST1, Nothing;
  opPUSHCONST2, Nothing;
  opPUSHCONST3, Nothing;
  opPUSHCONSTINT, Sint;
  opNEGINT, Nothing;
  opADDINT, Nothing;
  opSUBINT, Nothing;
  opMULINT, Nothing;
  opDIVINT, Nothing;
  opMODINT, Nothing;
  opANDINT, Nothing;
  opORINT, Nothing;
  opXORINT, Nothing;
  opLSLINT, Nothing;
  opLSRINT, Nothing;
  opASRINT, Nothing;
  opEQ, Nothing;
  opNEQ, Nothing;
  opLTINT, Nothing;
  opLEINT, Nothing;
  opGTINT, Nothing;
  opGEINT, Nothing;
  opOFFSETINT, Sint;
  opOFFSETREF, Sint;
  opISINT, Nothing;
  opGETMETHOD, Nothing;
  opGETDYNMET, Nothing;
  opGETPUBMET, Pubmet;
  opBEQ, Sint_Disp;
  opBNEQ, Sint_Disp;
  opBLTINT, Sint_Disp;
  opBLEINT, Sint_Disp;
  opBGTINT, Sint_Disp;
  opBGEINT, Sint_Disp;
  opULTINT, Nothing;
  opUGEINT, Nothing;
  opBULTINT, Uint_Disp;
  opBUGEINT, Uint_Disp;
  opPERFORM, Nothing;
  opRESUME, Nothing;
  opRESUMETERM, Uint;
  opREPERFORMTERM, Uint;
  opSTOP, Nothing;
  opEVENT, Nothing;
  opBREAK, Nothing;
  opRERAISE, Nothing;
  opRAISE_NOTRACE, Nothing;
]

let () =
  let opcode_count = opREPERFORMTERM + 1 in
  let seen = Array.make opcode_count false in
  List.iter
    (fun (opcode, _) ->
      if opcode < 0 || opcode >= opcode_count then
        invalid_arg "Bytecode_shape.op_shapes: opcode outside known range";
      if seen.(opcode) then
        invalid_arg "Bytecode_shape.op_shapes: duplicate opcode";
      seen.(opcode) <- true)
    op_shapes;
  Array.iter
    (fun present ->
      if not present then
        invalid_arg "Bytecode_shape.op_shapes: missing opcode")
    seen
