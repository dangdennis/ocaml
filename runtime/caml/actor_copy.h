/**************************************************************************/
/*                                                                        */
/*                                 OCaml                                  */
/*                                                                        */
/*                             Dennis Dang                                */
/*                                                                        */
/*   Copyright 2026 Dennis Dang                                           */
/*                                                                        */
/*   All rights reserved.  This file is distributed under the terms of    */
/*   the GNU Lesser General Public License version 2.1, with the          */
/*   special exception on linking described in the file LICENSE.          */
/*                                                                        */
/**************************************************************************/

/* Transactional same-image copying of an actor entry closure. */

#ifndef CAML_ACTOR_COPY_H
#define CAML_ACTOR_COPY_H

#ifdef CAML_INTERNALS

#include "mlvalues.h"

struct caml_actor_heap;

enum caml_actor_copy_status {
  CAML_ACTOR_COPY_OK = 0,
  CAML_ACTOR_COPY_UNSUPPORTED_RUNTIME,
  CAML_ACTOR_COPY_INVALID_SOURCE,
  CAML_ACTOR_COPY_UNSUPPORTED_TAG,
  CAML_ACTOR_COPY_INVALID_CLOSURE,
  CAML_ACTOR_COPY_INVALID_CODE_POINTER,
  CAML_ACTOR_COPY_FINALISABLE,
  CAML_ACTOR_COPY_GRAPH_TOO_LARGE,
  CAML_ACTOR_COPY_RESOURCE_UNAVAILABLE,
  CAML_ACTOR_COPY_INTERNAL
};

struct caml_actor_copy_result {
  enum caml_actor_copy_status status;
  struct caml_actor_heap *heap;
  value closure;
};

/* Copy [source] into a new, unpublished actor heap.  [source] must be a
   closure (a canonical closure base or one of its valid infix entries).

   The source is either the currently active actor heap or the frozen stock
   heap.  Every reachable supported block is copied, including blocks from
   the frozen heap.  Only immediates, canonical atoms, and same-image code
   pointers retain identity.

   On success, the caller owns [result.heap] and [result.closure] belongs to
   that heap.  On failure, [result.heap] is NULL and [result.closure] is zero;
   no partially built heap remains registered. */
CAMLextern struct caml_actor_copy_result caml_actor_copy_closure(
  value source, uintnat owner, mlsize_t quota_words);
CAMLextern struct caml_actor_copy_result caml_actor_copy_closure_sized(
  value source, uintnat owner, mlsize_t initial_words,
  mlsize_t maximum_words);

/* A stable, pointer-free diagnostic suitable for mapping to the public
   Unsupported_capture payload after the actor world has been torn down. */
CAMLextern const char *caml_actor_copy_status_message(
  enum caml_actor_copy_status status);

#endif /* CAML_INTERNALS */

#endif /* CAML_ACTOR_COPY_H */
