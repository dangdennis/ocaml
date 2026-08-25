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

/* Internal semispace actor heaps and their ownership verifier. */

#ifndef CAML_ACTOR_HEAP_H
#define CAML_ACTOR_HEAP_H

#ifdef CAML_INTERNALS

#include "mlvalues.h"

struct caml_actor_heap;

enum caml_actor_heap_alloc_error {
  CAML_ACTOR_HEAP_ALLOC_OK = 0,
  CAML_ACTOR_HEAP_ALLOC_QUOTA,
  CAML_ACTOR_HEAP_ALLOC_UNSUPPORTED
};

enum caml_actor_heap_verify_error {
  CAML_ACTOR_HEAP_VERIFY_OK = 0,
  CAML_ACTOR_HEAP_VERIFY_MALFORMED,
  CAML_ACTOR_HEAP_VERIFY_UNSUPPORTED_TAG,
  CAML_ACTOR_HEAP_VERIFY_INVALID_EDGE,
  CAML_ACTOR_HEAP_VERIFY_FOREIGN_EDGE,
  CAML_ACTOR_HEAP_VERIFY_HOST_YOUNG_EDGE,
  CAML_ACTOR_HEAP_VERIFY_UNAPPROVED_EXTERNAL_EDGE,
  CAML_ACTOR_HEAP_VERIFY_INVALID_CODE_POINTER
};

struct caml_actor_heap_verify_result {
  enum caml_actor_heap_verify_error error;
  uintnat source_owner;
  uintnat target_owner;
  mlsize_t source_field;
};

enum caml_actor_heap_store_status {
  CAML_ACTOR_HEAP_STORE_INACTIVE = 0,
  CAML_ACTOR_HEAP_STORE_OK,
  CAML_ACTOR_HEAP_STORE_INVALID
};

CAMLextern struct caml_actor_heap *caml_actor_heap_create(
  uintnat owner, mlsize_t quota_words);
CAMLextern void caml_actor_heap_destroy(struct caml_actor_heap *heap);

CAMLextern int caml_actor_heap_activate(struct caml_actor_heap *heap);
CAMLextern void caml_actor_heap_deactivate(void);
CAMLextern struct caml_actor_heap *caml_actor_heap_current(void);

CAMLextern value caml_actor_heap_try_alloc(
  struct caml_actor_heap *heap, mlsize_t wosize, tag_t tag,
  reserved_t reserved, enum caml_actor_heap_alloc_error *error);
CAMLextern value caml_actor_heap_alloc_or_raise(
  struct caml_actor_heap *heap, mlsize_t wosize, tag_t tag,
  reserved_t reserved);
CAMLextern int caml_actor_heap_allocation_supported(
  mlsize_t wosize, tag_t tag, reserved_t reserved);
CAMLextern int caml_actor_heap_collect(struct caml_actor_heap *heap);
CAMLextern int caml_actor_heap_reserve(
  struct caml_actor_heap *heap, mlsize_t words);

CAMLextern int caml_actor_heap_owns_value(
  const struct caml_actor_heap *heap, value value);
CAMLextern int caml_actor_heap_contains_address(value candidate);
CAMLextern uintnat caml_actor_heap_owner(
  const struct caml_actor_heap *heap);
CAMLextern mlsize_t caml_actor_heap_quota_words(
  const struct caml_actor_heap *heap);
CAMLextern mlsize_t caml_actor_heap_used_words(
  const struct caml_actor_heap *heap);
CAMLextern uintnat caml_actor_heap_blocks(
  const struct caml_actor_heap *heap);
CAMLextern uintnat caml_actor_heap_shared_bypasses(
  const struct caml_actor_heap *heap);
CAMLextern uintnat caml_actor_heap_collections(
  const struct caml_actor_heap *heap);
CAMLextern void caml_actor_heap_note_shared_bypass(
  struct caml_actor_heap *heap);

CAMLextern enum caml_actor_heap_store_status caml_actor_heap_check_store(
  const volatile value *field, value new_value);
CAMLextern enum caml_actor_heap_store_status
caml_actor_heap_check_field_store(value block, mlsize_t field,
                                  value new_value);
CAMLextern enum caml_actor_heap_store_status
caml_actor_heap_check_vector_store(value block, mlsize_t field,
                                   value new_value);
CAMLextern enum caml_actor_heap_store_status
caml_actor_heap_check_bytes_store(value block, mlsize_t byte);
CAMLextern enum caml_actor_heap_store_status
caml_actor_heap_check_double_store(value block, mlsize_t field);
CAMLextern enum caml_actor_heap_store_status
caml_actor_heap_check_offsetref(value block);
CAMLextern struct caml_actor_heap_verify_result caml_actor_heap_verify(
  const struct caml_actor_heap *heap);

#endif /* CAML_INTERNALS */

#endif /* CAML_ACTOR_HEAP_H */
