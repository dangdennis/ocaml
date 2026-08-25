/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*             Xavier Leroy, projet Cristal, INRIA Rocquencourt           */
/*                                                                        */
/*   Copyright 1996 Institut National de Recherche en Informatique et     */
/*     en Automatique.                                                    */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

/* The bytecode interpreter */

#ifndef CAML_INTERP_H
#define CAML_INTERP_H

#ifdef CAML_INTERNALS

#include "misc.h"
#include "mlvalues.h"

/* A suspended bytecode interpreter stores its GC-visible registers in the
   first four words of the current stack: accumulator, next program counter,
   environment, and extra arguments.  This is also the frame shape expected
   by the bytecode debugger. */
enum caml_bytecode_stop_reason {
  CAML_BYTECODE_STOP_REDUCTIONS,
  CAML_BYTECODE_STOP_HOST_ACTION,
  CAML_BYTECODE_STOP_UNSUPPORTED,
  CAML_BYTECODE_STOP_VALUE,
  CAML_BYTECODE_STOP_EXCEPTION
};

enum caml_bytecode_state_phase {
  CAML_BYTECODE_STATE_SUSPENDED,
  CAML_BYTECODE_STATE_RUNNING,
  CAML_BYTECODE_STATE_FINISHED
};

struct caml_bytecode_state {
  code_t prog;
  asize_t prog_size;
  int64_t stack_id;
  int64_t entry_stack_id;
  intnat trap_sp_off;
  intnat return_trap_sp_off;
  int entry_stack_words;
  int domain_unique_id;
  enum caml_bytecode_state_phase phase;
};

#define CAML_BYTECODE_REDUCTIONS_UNLIMITED CAML_UINTNAT_MAX

/* These entry points are synchronous and Domain-local.  The stack named by a
   suspended state must be installed as the Domain's current stack before the
   next resume.  Detached-stack ownership and root scanning are the caller's
   responsibility. */
CAMLextern void caml_bytecode_state_init(
  struct caml_bytecode_state *state,
  code_t prog, asize_t prog_size,
  value initial_env, intnat initial_extra_args);

CAMLextern enum caml_bytecode_stop_reason
caml_bytecode_interpreter_slice(
  struct caml_bytecode_state *state,
  uintnat max_reductions,
  value *result);

CAMLextern
value caml_bytecode_interpreter (code_t prog, asize_t prog_size,
                                 value initial_env, intnat initial_extra_args);

/* For backward compatibility */

Caml_inline value caml_interprete (code_t prog, asize_t prog_size)
{
  return caml_bytecode_interpreter(prog, prog_size, Atom(0), 0);
}

#endif /* CAML_INTERNALS */

#endif /* CAML_INTERP_H */
