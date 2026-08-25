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

CAMLprim value caml_actor_test_closure_copy(
  value entry, value graph, value even, value odd)
{
  CAMLparam4(entry, graph, even, odd);
  struct caml_actor_copy_result copied = {
    CAML_ACTOR_COPY_INTERNAL, NULL, 0
  };
  struct caml_actor_copy_result infix_copy = {
    CAML_ACTOR_COPY_INTERNAL, NULL, 0
  };
  struct caml_actor_copy_result quota_copy;
  struct caml_actor_heap_verify_result verification;
  enum caml_actor_world_status world_status;
  value infix;
  value target_graph;
  value target_cycle;
  value target_link;
  value target_some;
  mlsize_t environment_field = 0;
  int frozen = 0;
  int code = 0;

  world_status = caml_actor_world_freeze();
  REQUIRE(world_status == CAML_ACTOR_WORLD_OK, 1);
  frozen = 1;

  quota_copy = caml_actor_copy_closure(entry, 0xC000, 1);
  REQUIRE(quota_copy.status == CAML_ACTOR_COPY_GRAPH_TOO_LARGE
          && quota_copy.heap == NULL && quota_copy.closure == 0, 2);

  copied = caml_actor_copy_closure(entry, 0xC001, 256);
  REQUIRE(copied.status == CAML_ACTOR_COPY_OK
          && copied.heap != NULL
          && caml_actor_heap_owns_value(copied.heap, copied.closure), 3);
  verification = caml_actor_heap_verify(copied.heap);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 4);
  REQUIRE(find_environment_field(entry, graph, &environment_field), 5);
  target_graph = Field(copied.closure, environment_field);
  REQUIRE(caml_actor_heap_owns_value(copied.heap, target_graph)
          && target_graph != graph, 6);
  REQUIRE(Field(target_graph, 0) == Field(target_graph, 1)
          && Field(target_graph, 0) != Field(graph, 0), 7);

  target_cycle = Field(target_graph, 2);
  target_link = Field(target_cycle, 0);
  target_some = Field(target_link, 0);
  REQUIRE(Field(target_some, 0) == target_cycle, 8);
  REQUIRE(Field(target_graph, 3) != Field(graph, 3)
          && caml_string_length(Field(target_graph, 3)) == 10
          && memcmp(String_val(Field(target_graph, 3)),
                    "actor-copy", 10) == 0, 9);
  REQUIRE(Field(target_graph, 4) != Field(graph, 4)
          && Double_val(Field(target_graph, 4)) == 42.5, 10);

  infix = Tag_val(even) == Infix_tag ? even : odd;
  REQUIRE(Tag_val(infix) == Infix_tag, 11);
  infix_copy = caml_actor_copy_closure(infix, 0xC002, 128);
  REQUIRE(infix_copy.status == CAML_ACTOR_COPY_OK
          && infix_copy.heap != NULL
          && Tag_val(infix_copy.closure) == Infix_tag
          && caml_actor_heap_owns_value(
               infix_copy.heap, infix_copy.closure), 12);
  verification = caml_actor_heap_verify(infix_copy.heap);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 13);

cleanup:
  if (copied.heap != NULL) caml_actor_heap_destroy(copied.heap);
  if (infix_copy.heap != NULL) caml_actor_heap_destroy(infix_copy.heap);
  if (frozen) {
    world_status = caml_actor_world_thaw();
    if (world_status != CAML_ACTOR_WORLD_OK && code == 0) code = 14;
  }
  CAMLreturn(Val_int(code));
}
