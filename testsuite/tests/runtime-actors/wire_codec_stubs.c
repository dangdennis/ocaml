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

#define CAML_INTERNALS

#include <string.h>

#include "caml/actor_copy.h"
#include "caml/actor_heap.h"
#include "caml/actor_wire.h"
#include "caml/actor_world.h"
#include "caml/memory.h"
#include "caml/mlvalues.h"

#define REQUIRE(condition, failure) \
  do { if (!(condition)) { code = (failure); goto cleanup; } } while (0)

static int find_environment_field(value closure, value captured,
                                  mlsize_t *field_out)
{
  mlsize_t start = Start_env_closinfo(Closinfo_val(closure));

  for (mlsize_t field = start; field < Wosize_val(closure); field++) {
    if (Field(closure, field) == captured) {
      *field_out = field;
      return 1;
    }
  }
  return 0;
}

CAMLprim value caml_actor_test_wire_codec(value entry, value graph)
{
  CAMLparam2(entry, graph);
  struct caml_actor_copy_result copied = {
    CAML_ACTOR_COPY_INTERNAL, NULL, 0
  };
  struct caml_actor_wire_encode_result encoded = {
    CAML_ACTOR_WIRE_ENCODE_INTERNAL, NULL
  };
  struct caml_actor_wire_encode_result rejected;
  struct caml_actor_wire_encode_result quota;
  struct caml_actor_heap *target_heap = NULL;
  struct caml_actor_heap_verify_result verification;
  enum caml_actor_wire_decode_status decode_status;
  enum caml_actor_world_status world_status;
  value source_graph;
  value target_graph = Val_unit;
  value target_cycle;
  value target_link;
  value target_some;
  mlsize_t environment_field = 0;
  int source_active = 0;
  int target_active = 0;
  int frozen = 0;
  int code = 0;

  world_status = caml_actor_world_freeze();
  REQUIRE(world_status == CAML_ACTOR_WORLD_OK, 1);
  frozen = 1;
  copied = caml_actor_copy_closure(entry, 0xD000, 512);
  REQUIRE(copied.status == CAML_ACTOR_COPY_OK
          && copied.heap != NULL, 2);
  REQUIRE(find_environment_field(entry, graph, &environment_field), 3);
  source_graph = Field(copied.closure, environment_field);

  REQUIRE(caml_actor_heap_activate(copied.heap), 4);
  source_active = 1;
  quota = caml_actor_wire_encode(source_graph, 1);
  REQUIRE(quota.status == CAML_ACTOR_WIRE_ENCODE_TOO_LARGE
          && quota.envelope == NULL, 5);
  rejected = caml_actor_wire_encode(copied.closure, 512);
  REQUIRE(rejected.status == CAML_ACTOR_WIRE_ENCODE_UNSUPPORTED_VALUE
          && rejected.envelope == NULL, 6);
  encoded = caml_actor_wire_encode(source_graph, 512);
  REQUIRE(encoded.status == CAML_ACTOR_WIRE_ENCODE_OK
          && encoded.envelope != NULL
          && caml_actor_wire_verify(encoded.envelope), 7);
  caml_actor_heap_deactivate();
  source_active = 0;
  caml_actor_heap_destroy(copied.heap);
  copied.heap = NULL;
  REQUIRE(caml_actor_wire_verify(encoded.envelope), 8);

  target_heap = caml_actor_heap_create(0xD001, 512);
  REQUIRE(target_heap != NULL && caml_actor_heap_activate(target_heap), 9);
  target_active = 1;
  decode_status = caml_actor_wire_decode(
    encoded.envelope, target_heap, &target_graph);
  REQUIRE(decode_status == CAML_ACTOR_WIRE_DECODE_OK
          && caml_actor_heap_owns_value(target_heap, target_graph), 10);
  REQUIRE(Field(target_graph, 0) == Field(target_graph, 1)
          && Long_val(Field(Field(target_graph, 0), 0)) == 17, 11);
  target_cycle = Field(target_graph, 2);
  target_link = Field(target_cycle, 0);
  target_some = Field(target_link, 0);
  REQUIRE(Field(target_some, 0) == target_cycle, 12);
  REQUIRE(caml_string_length(Field(target_graph, 3)) == 10
          && memcmp(String_val(Field(target_graph, 3)),
                    "actor-wire", 10) == 0, 13);
  REQUIRE(Double_val(Field(target_graph, 4)) == 42.5, 14);
  REQUIRE(Wosize_val(Field(target_graph, 5)) == 2 * Double_wosize
          && Double_flat_field(Field(target_graph, 5), 0) == 1.25
          && Double_flat_field(Field(target_graph, 5), 1) == 9.5, 15);
  verification = caml_actor_heap_verify(target_heap);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 16);

cleanup:
  if (source_active || target_active) caml_actor_heap_deactivate();
  if (copied.heap != NULL) caml_actor_heap_destroy(copied.heap);
  if (target_heap != NULL) caml_actor_heap_destroy(target_heap);
  caml_actor_wire_destroy(encoded.envelope);
  if (frozen) {
    world_status = caml_actor_world_thaw();
    if (world_status != CAML_ACTOR_WORLD_OK && code == 0) code = 17;
  }
  CAMLreturn(Val_int(code));
}
