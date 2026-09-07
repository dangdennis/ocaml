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

CAMLextern enum caml_actor_world_status caml_actor_world_freeze(void);
CAMLextern enum caml_actor_world_status caml_actor_world_thaw(void);

CAMLextern int caml_actor_world_is_frozen(void);
CAMLextern int caml_actor_world_register_frozen(value candidate);
CAMLextern int caml_actor_world_value_is_frozen(value candidate);

#endif /* CAML_INTERNALS */

#endif /* CAML_ACTOR_WORLD_H */
