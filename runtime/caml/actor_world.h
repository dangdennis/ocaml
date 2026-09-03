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

/* Internal freeze boundary for one actor world. */

#ifndef CAML_ACTOR_WORLD_H
#define CAML_ACTOR_WORLD_H

#ifdef CAML_INTERNALS

#include "mlvalues.h"

struct caml_actor_world;

enum caml_actor_world_status {
  CAML_ACTOR_WORLD_OK = 0,
  CAML_ACTOR_WORLD_UNSUPPORTED,
  CAML_ACTOR_WORLD_BUSY,
  CAML_ACTOR_WORLD_CORRUPTED
};

enum caml_actor_global_status {
  CAML_ACTOR_GLOBAL_OK = 0,
  CAML_ACTOR_GLOBAL_BUSY,
  CAML_ACTOR_GLOBAL_INVALID_IMAGE,
  CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE
};

CAMLextern enum caml_actor_world_status caml_actor_world_freeze(void);
CAMLextern enum caml_actor_world_status caml_actor_world_thaw(void);

CAMLextern int caml_actor_world_is_frozen(void);
CAMLextern int caml_actor_world_register_frozen(value candidate);
CAMLextern int caml_actor_world_value_is_frozen(value candidate);
CAMLextern int caml_actor_world_value_is_approved(value candidate);

/* Return the immutable C-owned snapshot for an exact frozen block identity.
   The payload remains owned by the actor world and is valid until thaw. */
CAMLextern int caml_actor_world_frozen_snapshot(
  value candidate, header_t *header, const value **payload);

/* Transactionally validate and snapshot the bytecode executable's global
   image.  Preparation runs in the frozen host context before an actor
   scheduler or heap is installed. */
CAMLextern enum caml_actor_global_status
caml_actor_world_prepare_global_image(void);

/* Checked actor-mode reads from the prepared global image. */
CAMLextern int caml_actor_world_read_global(
  uintnat index, value *result);
CAMLextern int caml_actor_world_read_global_field(
  uintnat index, mlsize_t field, value *result);
CAMLextern int caml_actor_world_read_frozen_field(
  value block, mlsize_t field, value *result);
CAMLextern int caml_actor_world_read_frozen_closure_env(
  value closure, mlsize_t field, value *result);
CAMLextern int caml_actor_world_frozen_closure_wosize(
  value closure, mlsize_t *wosize);

#endif /* CAML_INTERNALS */

#endif /* CAML_ACTOR_WORLD_H */
