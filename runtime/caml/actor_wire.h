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

/* Pointer-free actor message envelopes. */

#ifndef CAML_ACTOR_WIRE_H
#define CAML_ACTOR_WIRE_H

#ifdef CAML_INTERNALS

#include "mlvalues.h"

struct caml_actor_envelope;
struct caml_actor_heap;

enum caml_actor_wire_encode_status {
  CAML_ACTOR_WIRE_ENCODE_OK = 0,
  CAML_ACTOR_WIRE_ENCODE_INVALID_SOURCE,
  CAML_ACTOR_WIRE_ENCODE_UNSUPPORTED_VALUE,
  CAML_ACTOR_WIRE_ENCODE_TOO_LARGE,
  CAML_ACTOR_WIRE_ENCODE_RESOURCE_UNAVAILABLE,
  CAML_ACTOR_WIRE_ENCODE_INTERNAL
};

enum caml_actor_wire_decode_status {
  CAML_ACTOR_WIRE_DECODE_OK = 0,
  CAML_ACTOR_WIRE_DECODE_HEAP_EXHAUSTED,
  CAML_ACTOR_WIRE_DECODE_RESOURCE_UNAVAILABLE,
  CAML_ACTOR_WIRE_DECODE_INTERNAL
};

struct caml_actor_wire_encode_result {
  enum caml_actor_wire_encode_status status;
  struct caml_actor_envelope *envelope;
};

/* Encode [message] from the current actor heap and approved frozen snapshot.
   The completed envelope contains no OCaml value, heap pointer, or source
   provenance and remains unpublished until its caller commits it. */
CAMLextern struct caml_actor_wire_encode_result caml_actor_wire_encode(
  value message, mlsize_t quota_words);

/* Decode without consuming the envelope.  The target must be the currently
   active actor heap.  A quota failure allocates nothing. */
CAMLextern enum caml_actor_wire_decode_status caml_actor_wire_decode(
  const struct caml_actor_envelope *envelope,
  struct caml_actor_heap *target_heap, value *message);

CAMLextern int caml_actor_wire_verify(
  const struct caml_actor_envelope *envelope);
CAMLextern int caml_actor_wire_encoded_bytes(
  const struct caml_actor_envelope *envelope, uintnat *encoded_bytes);
CAMLextern void caml_actor_wire_destroy(
  struct caml_actor_envelope *envelope);

#endif /* CAML_INTERNALS */

#endif /* CAML_ACTOR_WIRE_H */
