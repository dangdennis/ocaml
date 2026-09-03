#define CAML_INTERNALS

#include "caml/actor_heap.h"
#include "caml/actor_world.h"
#include "caml/memory.h"
#include "caml/mlvalues.h"

#define REQUIRE(condition, failure) \
  do { if (!(condition)) { code = (failure); goto cleanup; } } while (0)

static value allocate_block(struct caml_actor_heap *heap, mlsize_t wosize,
                            intnat payload,
                            enum caml_actor_heap_alloc_error *error)
{
  value block = caml_actor_heap_try_alloc(heap, wosize, 0, 0, error);

  if (block != 0) {
    for (mlsize_t field = 0; field < wosize; field++) {
      caml_initialize(&Field(block, field), Val_long(payload + field));
    }
  }
  return block;
}

CAMLprim value caml_actor_test_elastic_heaps(value unit)
{
  CAMLparam1(unit);
  CAMLlocal4(first, second, garbage, trial);
  struct caml_actor_heap *heap = NULL;
  struct caml_actor_heap *fixed = NULL;
  enum caml_actor_heap_alloc_error error;
  uintnat collections_before;
  int frozen = 0;
  int code = 0;

  first = second = garbage = trial = Val_unit;
  REQUIRE(caml_actor_heap_create_sized(10, 0, 64) == NULL, 1);
  REQUIRE(caml_actor_heap_create_sized(10, 65, 64) == NULL, 2);

  fixed = caml_actor_heap_create(11, 32);
  REQUIRE(fixed != NULL, 3);
  REQUIRE(caml_actor_heap_capacity_words(fixed) == 32, 4);
  REQUIRE(caml_actor_heap_quota_words(fixed) == 32, 5);
  caml_actor_heap_destroy(fixed);
  fixed = NULL;

  REQUIRE(caml_actor_world_freeze() == CAML_ACTOR_WORLD_OK, 6);
  frozen = 1;
  heap = caml_actor_heap_create_sized(12, 32, 128);
  REQUIRE(heap != NULL, 7);
  REQUIRE(caml_actor_heap_capacity_words(heap) == 32, 8);
  REQUIRE(caml_actor_heap_quota_words(heap) == 128, 9);
  REQUIRE(caml_actor_heap_activate(heap), 10);

  first = allocate_block(heap, 9, 100, &error);
  REQUIRE(first != 0 && error == CAML_ACTOR_HEAP_ALLOC_OK, 11);
  garbage = allocate_block(heap, 9, 200, &error);
  REQUIRE(garbage != 0 && error == CAML_ACTOR_HEAP_ALLOC_OK, 12);
  garbage = Val_unit;
  trial = allocate_block(heap, 11, 300, &error);
  REQUIRE(trial != 0 && error == CAML_ACTOR_HEAP_ALLOC_OK, 13);
  REQUIRE(caml_actor_heap_capacity_words(heap) == 32, 14);
  REQUIRE(Long_val(Field(first, 0)) == 100, 15);

  REQUIRE(caml_actor_heap_reserve(heap, 16), 16);
  REQUIRE(caml_actor_heap_used_words(heap) == 22, 17);
  REQUIRE(caml_actor_heap_capacity_words(heap) == 64, 18);
  second = allocate_block(heap, 15, 400, &error);
  REQUIRE(second != 0 && error == CAML_ACTOR_HEAP_ALLOC_OK, 19);
  REQUIRE(caml_actor_heap_quota_words(heap) == 128, 20);
  REQUIRE(caml_actor_heap_owns_value(heap, first), 21);
  REQUIRE(caml_actor_heap_owns_value(heap, second), 22);
  REQUIRE(Long_val(Field(first, 8)) == 108, 23);
  REQUIRE(Long_val(Field(second, 14)) == 414, 24);

  collections_before = caml_actor_heap_collections(heap);
  trial = caml_actor_heap_try_alloc(heap, 128, 0, 0, &error);
  REQUIRE(trial == 0 && error == CAML_ACTOR_HEAP_ALLOC_QUOTA, 25);
  REQUIRE(caml_actor_heap_capacity_words(heap) == 64, 26);
  REQUIRE(caml_actor_heap_collections(heap) == collections_before, 27);
  REQUIRE(Long_val(Field(first, 0)) == 100, 28);
  REQUIRE(Long_val(Field(second, 0)) == 400, 29);

cleanup:
  if (caml_actor_heap_current() != NULL) caml_actor_heap_deactivate();
  first = second = garbage = trial = Val_unit;
  caml_actor_heap_destroy(fixed);
  caml_actor_heap_destroy(heap);
  if (frozen) (void)caml_actor_world_thaw();
  CAMLreturn(Val_int(code));
}
