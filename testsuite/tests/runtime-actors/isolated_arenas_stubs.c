#define CAML_INTERNALS

#include "caml/actor_heap.h"
#include "caml/alloc.h"
#include "caml/domain_state.h"
#include "caml/memory.h"
#include "caml/mlvalues.h"
#include "caml/shared_heap.h"

#define REQUIRE(condition, failure) \
  do { if (!(condition)) { code = (failure); goto cleanup; } } while (0)

CAMLprim value caml_actor_test_isolated_arenas(value unit)
{
  CAMLparam1(unit);
  CAMLlocal2(host_young, host_major);
  struct caml_actor_heap *saved_heap = caml_actor_heap_current();
  struct caml_actor_heap *heap_a = NULL;
  struct caml_actor_heap *heap_b = NULL;
  struct caml_actor_heap_verify_result verification;
  enum caml_actor_heap_alloc_error allocation_error;
  value a_root = Val_unit;
  value a_child = Val_unit;
  value a_large = Val_unit;
  value a_after_b = Val_unit;
  value b_block = Val_unit;
  value trial = Val_unit;
  value *young_ptr_before;
  header_t saved_header;
  mlsize_t used_before;
  uintnat allocated_words_before;
  uintnat allocated_words_direct_before;
  uintnat blocks_before;
  uintnat bypasses_before;
  int code = 0;

  if (saved_heap != NULL) CAMLreturn(Val_int(1));
  host_young = caml_alloc_small(1, 0);
  caml_initialize(&Field(host_young, 0), Val_long(41));
  host_major = caml_alloc_shr(Max_young_wosize + 1, 0);
  for (mlsize_t field = 0; field < Max_young_wosize + 1; field++) {
    caml_initialize(&Field(host_major, field), Val_long(field));
  }
  heap_a = caml_actor_heap_create(0, 768);
  heap_b = caml_actor_heap_create(1, 64);
  REQUIRE(heap_a != NULL && heap_b != NULL, 2);
  young_ptr_before = Caml_state->young_ptr;
  allocated_words_before = Caml_state->allocated_words;
  allocated_words_direct_before = Caml_state->allocated_words_direct;

  REQUIRE(caml_actor_heap_activate(heap_a), 3);
  a_root = caml_alloc_small(2, 0);
  a_child = caml_alloc_small(2, 0);
  caml_initialize(&Field(a_root, 0), Val_unit);
  caml_initialize(&Field(a_root, 1), Val_unit);
  caml_initialize(&Field(a_child, 0), Val_long(17));
  caml_initialize(&Field(a_child, 1), Val_unit);

  a_large = caml_alloc_shr(Max_young_wosize + 1, 0);
  for (mlsize_t field = 0; field < Max_young_wosize + 1; field++) {
    caml_initialize(&Field(a_large, field), Val_long(field));
  }
  caml_actor_heap_deactivate();

  REQUIRE(caml_actor_heap_activate(heap_b), 4);
  b_block = caml_alloc_small(1, 0);
  caml_initialize(&Field(b_block, 0), Val_long(29));
  caml_actor_heap_deactivate();

  REQUIRE(Caml_state->young_ptr == young_ptr_before, 27);
  REQUIRE(Caml_state->allocated_words == allocated_words_before, 28);
  REQUIRE(Caml_state->allocated_words_direct
          == allocated_words_direct_before, 29);

  REQUIRE(caml_actor_heap_owns_value(heap_a, a_root), 5);
  REQUIRE(caml_actor_heap_owns_value(heap_a, a_child), 6);
  REQUIRE(caml_actor_heap_owns_value(heap_a, a_large), 7);
  REQUIRE(!caml_actor_heap_owns_value(heap_b, a_root), 8);
  REQUIRE(caml_actor_heap_owns_value(heap_b, b_block), 9);
  REQUIRE(!caml_actor_heap_owns_value(heap_a, b_block), 10);
  REQUIRE(Wosize_val(a_large) == Max_young_wosize + 1, 11);
  REQUIRE(Tag_val(a_large) == 0, 12);

  REQUIRE(caml_actor_heap_activate(heap_a), 13);
  a_after_b = caml_alloc_small(1, 0);
  caml_initialize(&Field(a_after_b, 0), Val_long(37));
  REQUIRE(caml_actor_heap_owns_value(heap_a, a_after_b), 40);
  REQUIRE(Caml_state->young_ptr == young_ptr_before, 41);
  REQUIRE(Caml_state->allocated_words == allocated_words_before, 42);
  REQUIRE(Caml_state->allocated_words_direct
          == allocated_words_direct_before, 43);
  caml_initialize(&Field(a_root, 0), a_child);
  caml_initialize(&Field(a_root, 1), a_large);
  verification = caml_actor_heap_verify(heap_a);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 14);
  verification = caml_actor_heap_verify(heap_b);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 15);

  bypasses_before = caml_actor_heap_shared_bypasses(heap_a);
  REQUIRE(bypasses_before == 0, 16);
  REQUIRE(caml_shared_try_alloc(
            Caml_state->shared_heap, 1, 0, 0) == NULL, 17);
  REQUIRE(caml_actor_heap_shared_bypasses(heap_a)
          == bypasses_before + 1, 18);

  Field(a_root, 0) = b_block;
  verification = caml_actor_heap_verify(heap_a);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_FOREIGN_EDGE, 19);
  REQUIRE(verification.source_owner == 0, 20);
  REQUIRE(verification.target_owner == 1, 21);
  caml_modify(&Field(a_root, 0), a_child);
  verification = caml_actor_heap_verify(heap_a);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 22);

  Field(a_root, 0) = (value)&Field(a_child, 1);
  verification = caml_actor_heap_verify(heap_a);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_INVALID_EDGE, 30);
  caml_modify(&Field(a_root, 0), a_child);
  verification = caml_actor_heap_verify(heap_a);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 31);

  saved_header = Hd_val(a_child);
  Hd_hp(Hp_val(a_child)) = Hd_with_tag(saved_header, 1);
  verification = caml_actor_heap_verify(heap_a);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_MALFORMED, 32);
  Hd_hp(Hp_val(a_child)) = saved_header;
  verification = caml_actor_heap_verify(heap_a);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 33);

  Field(a_root, 0) = host_young;
  verification = caml_actor_heap_verify(heap_a);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_HOST_YOUNG_EDGE, 34);
  caml_modify(&Field(a_root, 0), a_child);
  Field(a_root, 0) = host_major;
  verification = caml_actor_heap_verify(heap_a);
  REQUIRE(verification.error
          == CAML_ACTOR_HEAP_VERIFY_UNAPPROVED_EXTERNAL_EDGE, 35);
  caml_modify(&Field(a_root, 0), a_child);
  Field(a_root, 0) = Atom(42);
  verification = caml_actor_heap_verify(heap_a);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 36);
  caml_modify(&Field(a_root, 0), a_child);

  used_before = caml_actor_heap_used_words(heap_a);
  blocks_before = caml_actor_heap_blocks(heap_a);
  trial = caml_actor_heap_try_alloc(
    heap_a, 1, Custom_tag, 0, &allocation_error);
  REQUIRE(trial == 0
          && allocation_error == CAML_ACTOR_HEAP_ALLOC_UNSUPPORTED, 23);
  REQUIRE(caml_actor_heap_used_words(heap_a) == used_before
          && caml_actor_heap_blocks(heap_a) == blocks_before, 24);

  trial = caml_actor_heap_try_alloc(
    heap_a, caml_actor_heap_quota_words(heap_a), 0, 0,
    &allocation_error);
  REQUIRE(trial == 0
          && allocation_error == CAML_ACTOR_HEAP_ALLOC_QUOTA, 25);
  REQUIRE(caml_actor_heap_used_words(heap_a) == used_before
          && caml_actor_heap_blocks(heap_a) == blocks_before, 26);

cleanup:
  if (caml_actor_heap_current() != NULL) caml_actor_heap_deactivate();
  a_root = Val_unit;
  a_child = Val_unit;
  a_large = Val_unit;
  a_after_b = Val_unit;
  b_block = Val_unit;
  trial = Val_unit;
  caml_actor_heap_destroy(heap_b);
  caml_actor_heap_destroy(heap_a);
  CAMLreturn(Val_int(code));
}
