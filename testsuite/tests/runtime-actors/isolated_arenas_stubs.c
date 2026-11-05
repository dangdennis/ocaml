#define CAML_INTERNALS

#include <stdio.h>

#include "caml/actor_heap.h"
#include "caml/alloc.h"
#include "caml/callback.h"
#include "caml/domain_state.h"
#include "caml/memory.h"
#include "caml/mlvalues.h"
#include "caml/shared_heap.h"

#if defined(__linux__) && defined(__x86_64__)
#include <errno.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#define REQUIRE(condition, failure) \
  do { if (!(condition)) { \
    fprintf(stderr, "arena check %d failed at line %d: %s\n", \
            (failure), __LINE__, #condition); \
    code = (failure); goto cleanup; \
  } } while (0)

static int rejected_mutation(volatile value *field, value target, int initialize)
{
#if defined(__linux__) && defined(__x86_64__)
  int status;
  pid_t waited;
  pid_t child = fork();
  if (child == -1) return 0;
  if (child == 0) {
    struct rlimit no_core = { 0, 0 };
    setrlimit(RLIMIT_CORE, &no_core);
    close(STDERR_FILENO);
    if (initialize) caml_initialize(field, target);
    else caml_modify(field, target);
    _exit(0);
  }
  do { waited = waitpid(child, &status, 0); }
  while (waited == -1 && errno == EINTR);
  if (waited != child) return 0;
  return WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT;
#else
  return 0; /* The test is restricted to Linux x86-64 bytecode. */
#endif
}

/* Exercise the representation checks independently of allocation routing.
   Raw writes below deliberately corrupt one invariant at a time. */
static int exercise_representations(void)
{
  struct caml_actor_heap *heap = NULL;
  struct caml_actor_heap *other = NULL;
  enum caml_actor_heap_alloc_error error;
  value closure, infix, string, root, foreign;
  value code_pointer = (value)caml_bytecode_callback_code();
  header_t saved_header;
  int code = 0;

  REQUIRE(caml_actor_heap_create(100, 0) == NULL, 101);
  REQUIRE(caml_actor_heap_create(100, CAML_UINTNAT_MAX) == NULL, 102);
  heap = caml_actor_heap_create(100, 64);
  other = caml_actor_heap_create(101, 2);
  REQUIRE(heap != NULL && other != NULL, 103);
  REQUIRE(caml_actor_heap_create(100, 64) == NULL, 104);
  REQUIRE(caml_actor_heap_activate(other), 105);
  foreign = caml_actor_heap_try_alloc(other, 1, 0, 0, &error);
  REQUIRE(foreign != 0 && error == CAML_ACTOR_HEAP_ALLOC_OK, 106);
  Field(foreign, 0) = Val_unit;
  REQUIRE(caml_actor_heap_used_words(other) == 2, 107);
  REQUIRE(caml_actor_heap_try_alloc(other, 1, 0, 0, &error) == 0
          && error == CAML_ACTOR_HEAP_ALLOC_QUOTA, 108);
  REQUIRE(caml_actor_heap_used_words(other) == 2
          && caml_actor_heap_blocks(other) == 1, 109);
  REQUIRE(!caml_actor_heap_activate(heap), 110);
  REQUIRE(caml_actor_heap_try_alloc(heap, 1, 0, 0, &error) == 0
          && error == CAML_ACTOR_HEAP_ALLOC_UNSUPPORTED, 111);
  caml_actor_heap_deactivate();

  REQUIRE(caml_actor_heap_activate(heap), 112);
  {
    const tag_t unsupported[] = {
      Forcing_tag, Lazy_tag, Object_tag, Infix_tag, Forward_tag,
      Abstract_tag, Custom_tag
    };
    for (unsigned i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]);
         i++) {
      REQUIRE(caml_actor_heap_try_alloc(heap, 2, unsupported[i], 0, &error)
              == 0 && error == CAML_ACTOR_HEAP_ALLOC_UNSUPPORTED, 135);
    }
    REQUIRE(caml_actor_heap_try_alloc(heap, 0, 0, 0, &error) == 0
            && error == CAML_ACTOR_HEAP_ALLOC_UNSUPPORTED, 136);
    REQUIRE(caml_actor_heap_try_alloc(heap, 1, Closure_tag, 0, &error) == 0
            && error == CAML_ACTOR_HEAP_ALLOC_UNSUPPORTED, 137);
    REQUIRE(caml_actor_heap_try_alloc(heap, 1, 0, 1, &error) == 0
            && error == CAML_ACTOR_HEAP_ALLOC_UNSUPPORTED, 138);
    REQUIRE(caml_actor_heap_used_words(heap) == 0
            && caml_actor_heap_blocks(heap) == 0, 139);
  }
  closure = caml_actor_heap_alloc_or_raise(heap, 6, Closure_tag, 0);
  /* Two recursive closures followed by one shared environment field. */
  Field(closure, 0) = code_pointer;
  Field(closure, 1) = Make_closinfo(0, 5);
  Field(closure, 2) = Make_header(3, Infix_tag, 0);
  Field(closure, 3) = code_pointer;
  Field(closure, 4) = Make_closinfo(0, 2);
  Field(closure, 5) = Val_unit;
  infix = (value)&Field(closure, 3);
  string = caml_actor_heap_alloc_or_raise(heap, 1, String_tag, 0);
  for (mlsize_t i = 0; i < sizeof(value); i++) Byte_u(string, i) = 0;
  Byte_u(string, sizeof(value) - 1) = sizeof(value) - 1;
  root = caml_actor_heap_alloc_or_raise(heap, 2, 0, 0);
  Field(root, 0) = infix;
  Field(root, 1) = string;
  REQUIRE(caml_actor_heap_verify(heap).error == CAML_ACTOR_HEAP_VERIFY_OK,
          113);
  REQUIRE(caml_actor_heap_owns_value(heap, infix), 114);
  REQUIRE(caml_actor_heap_check_store(&Field(root, 0), infix)
          == CAML_ACTOR_HEAP_STORE_OK, 115);
  REQUIRE(caml_actor_heap_check_store(&Field(root, 0), foreign)
          == CAML_ACTOR_HEAP_STORE_INVALID, 116);
  REQUIRE(caml_actor_heap_check_store(&Field(foreign, 0), root)
          == CAML_ACTOR_HEAP_STORE_INVALID, 117);
  REQUIRE(caml_actor_heap_check_store(Hp_val(root), Val_unit)
          == CAML_ACTOR_HEAP_STORE_INVALID, 118);
  REQUIRE(caml_actor_heap_check_store(&Field(root, 0), 0)
          == CAML_ACTOR_HEAP_STORE_INVALID, 119);
  REQUIRE(caml_actor_heap_check_store(&Field(root, 0),
                                     (value)&Field(closure, 5))
          == CAML_ACTOR_HEAP_STORE_INVALID, 120);
  REQUIRE(rejected_mutation(&Field(root, 0), foreign, 0), 131);
  REQUIRE(rejected_mutation(&Field(root, 0), foreign, 1), 132);
  REQUIRE(rejected_mutation(&Field(foreign, 0), root, 0), 133);
  REQUIRE(Field(root, 0) == infix && Field(foreign, 0) == Val_unit, 134);

  Field(closure, 0) = 0;
  REQUIRE(caml_actor_heap_verify(heap).error
          == CAML_ACTOR_HEAP_VERIFY_INVALID_CODE_POINTER, 121);
  Field(closure, 0) = code_pointer;
  Field(closure, 0) = code_pointer + 1;
  REQUIRE(caml_actor_heap_verify(heap).error
          == CAML_ACTOR_HEAP_VERIFY_INVALID_CODE_POINTER, 140);
  Field(closure, 0) = code_pointer;
  Field(closure, 4) = Make_closinfo(0, 3);
  REQUIRE(!caml_actor_heap_owns_value(heap, infix), 122);
  REQUIRE(caml_actor_heap_verify(heap).error != CAML_ACTOR_HEAP_VERIFY_OK,
          123);
  Field(closure, 4) = Make_closinfo(0, 2);
  Field(closure, 1) = Make_closinfo(0, 7);
  REQUIRE(caml_actor_heap_verify(heap).error != CAML_ACTOR_HEAP_VERIFY_OK,
          124);
  Field(closure, 1) = Make_closinfo(0, 5);
  Field(root, 0) = Val_unit; /* The verifier must also scan unreachable blocks. */
  Field(closure, 5) = foreign;
  REQUIRE(caml_actor_heap_verify(heap).error
          == CAML_ACTOR_HEAP_VERIFY_FOREIGN_EDGE, 125);
  Field(closure, 5) = Val_unit;
  Field(root, 0) = infix;

  Byte_u(string, sizeof(value) - 1) = sizeof(value);
  REQUIRE(caml_actor_heap_verify(heap).error
          == CAML_ACTOR_HEAP_VERIFY_MALFORMED, 126);
  Byte_u(string, sizeof(value) - 1) = sizeof(value) - 1;
  Byte_u(string, 0) = 'x';
  REQUIRE(caml_actor_heap_verify(heap).error
          == CAML_ACTOR_HEAP_VERIFY_MALFORMED, 127);
  Byte_u(string, 0) = 0;

  saved_header = Hd_val(closure);
  Hd_hp(Hp_val(closure)) = Make_header(1, Closure_tag, NOT_MARKABLE);
  REQUIRE(caml_actor_heap_verify(heap).error
          == CAML_ACTOR_HEAP_VERIFY_MALFORMED, 128);
  Hd_hp(Hp_val(closure)) = saved_header;
  REQUIRE(caml_actor_heap_verify(heap).error == CAML_ACTOR_HEAP_VERIFY_OK,
          129);
  caml_actor_heap_deactivate();
  REQUIRE(caml_actor_heap_check_store(&Field(root, 0), foreign)
          == CAML_ACTOR_HEAP_STORE_INACTIVE, 130);

cleanup:
  caml_actor_heap_deactivate();
  caml_actor_heap_destroy(other);
  caml_actor_heap_destroy(heap);
  return code;
}

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
  code = exercise_representations();
  if (code != 0) CAMLreturn(Val_int(code));
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
