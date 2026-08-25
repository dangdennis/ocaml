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

#include <stdio.h>

#include "caml/actor_heap.h"
#include "caml/actor_scheduler.h"
#include "caml/actor_world.h"
#include "caml/alloc.h"
#include "caml/codefrag.h"
#include "caml/domain_state.h"
#include "caml/fiber.h"
#include "caml/fix_code.h"
#include "caml/instruct.h"
#include "caml/memory.h"
#include "caml/misc.h"
#include "caml/mlvalues.h"
#include "caml/prims.h"
#include "caml/shared_heap.h"
#include "caml/signals.h"

#define REQUIRE(condition, failure) \
  do { if (!(condition)) { code = (failure); goto cleanup; } } while (0)

#define CODE_WORDS(code) (sizeof(code) / sizeof((code)[0]))

CAMLextern value caml_int_compare(value left, value right);

static opcode_t setfield_code[9];
static opcode_t setvect_code[11];
static opcode_t setbytes_code[12];
static opcode_t setdouble_code[9];
static opcode_t offsetref_code[6];
static opcode_t compare_code[7];
static opcode_t compare_wrong_arity_code[4];
static opcode_t calln_code[7];
static opcode_t poison_code[6];
static opcode_t one_past_code[6];
static opcode_t setglobal_code[4];
static opcode_t object_code[3];
static opcode_t debugger_code[2];
static opcode_t unsupported_tag_code[4];
static opcode_t quota_code[4];
static opcode_t poptrap_code[5];
static int code_ready;
static int poison_entries;

CAMLprim value caml_actor_test_runtime_fence_poison(
  value left, value right)
{
  CAMLparam2(left, right);
  poison_entries++;
  CAMLreturn(Val_unit);
}

static int primitive_index(c_primitive primitive)
{
  for (int index = 0; index < caml_prim_table.size; index++) {
    if ((c_primitive)caml_prim_table.contents[index] == primitive) {
      return index;
    }
  }
  return -1;
}

static void register_code(opcode_t *code, mlsize_t words)
{
  caml_register_code_fragment(
    (char *)code, (char *)(code + words), DIGEST_IGNORE, NULL);
}

static int prepare_code(void)
{
  int compare_primitive;
  int poison_primitive;

  if (code_ready) return 1;
  compare_primitive = primitive_index((c_primitive)caml_int_compare);
  poison_primitive = primitive_index(
    (c_primitive)caml_actor_test_runtime_fence_poison);
  if (compare_primitive < 0 || poison_primitive < 0) return 0;

  caml_set_instruction(&setfield_code[0], CONST0);
  caml_set_instruction(&setfield_code[1], MAKEBLOCK1);
  setfield_code[2] = 0;
  caml_set_instruction(&setfield_code[3], PUSH);
  caml_set_instruction(&setfield_code[4], CONST1);
  caml_set_instruction(&setfield_code[5], PUSH);
  caml_set_instruction(&setfield_code[6], ACC1);
  caml_set_instruction(&setfield_code[7], SETFIELD0);
  caml_set_instruction(&setfield_code[8], STOP);

  caml_set_instruction(&setvect_code[0], CONST0);
  caml_set_instruction(&setvect_code[1], MAKEBLOCK1);
  setvect_code[2] = 0;
  caml_set_instruction(&setvect_code[3], PUSH);
  caml_set_instruction(&setvect_code[4], CONST2);
  caml_set_instruction(&setvect_code[5], PUSH);
  caml_set_instruction(&setvect_code[6], CONST0);
  caml_set_instruction(&setvect_code[7], PUSH);
  caml_set_instruction(&setvect_code[8], ACC2);
  caml_set_instruction(&setvect_code[9], SETVECTITEM);
  caml_set_instruction(&setvect_code[10], STOP);

  caml_set_instruction(&setbytes_code[0], CONST0);
  caml_set_instruction(&setbytes_code[1], MAKEBLOCK1);
  setbytes_code[2] = String_tag;
  caml_set_instruction(&setbytes_code[3], PUSH);
  caml_set_instruction(&setbytes_code[4], CONSTINT);
  setbytes_code[5] = 65;
  caml_set_instruction(&setbytes_code[6], PUSH);
  caml_set_instruction(&setbytes_code[7], CONST0);
  caml_set_instruction(&setbytes_code[8], PUSH);
  caml_set_instruction(&setbytes_code[9], ACC2);
  caml_set_instruction(&setbytes_code[10], SETBYTESCHAR);
  caml_set_instruction(&setbytes_code[11], STOP);

  caml_set_instruction(&setdouble_code[0], CONST0);
  caml_set_instruction(&setdouble_code[1], MAKEBLOCK1);
  setdouble_code[2] = Double_tag;
  caml_set_instruction(&setdouble_code[3], PUSH);
  caml_set_instruction(&setdouble_code[4], MAKEFLOATBLOCK);
  setdouble_code[5] = 1;
  caml_set_instruction(&setdouble_code[6], SETFLOATFIELD);
  setdouble_code[7] = 0;
  caml_set_instruction(&setdouble_code[8], STOP);

  caml_set_instruction(&offsetref_code[0], CONST0);
  caml_set_instruction(&offsetref_code[1], MAKEBLOCK1);
  offsetref_code[2] = 0;
  caml_set_instruction(&offsetref_code[3], OFFSETREF);
  offsetref_code[4] = 1;
  caml_set_instruction(&offsetref_code[5], STOP);

  caml_set_instruction(&compare_code[0], CONST2);
  caml_set_instruction(&compare_code[1], PUSH);
  caml_set_instruction(&compare_code[2], CONST1);
  caml_set_instruction(&compare_code[3], C_CALL2);
  compare_code[4] = compare_primitive;
  caml_set_instruction(&compare_code[5], CONST0);
  caml_set_instruction(&compare_code[6], STOP);

  caml_set_instruction(&compare_wrong_arity_code[0], CONST1);
  caml_set_instruction(&compare_wrong_arity_code[1], C_CALL1);
  compare_wrong_arity_code[2] = compare_primitive;
  caml_set_instruction(&compare_wrong_arity_code[3], STOP);

  caml_set_instruction(&calln_code[0], CONST2);
  caml_set_instruction(&calln_code[1], PUSH);
  caml_set_instruction(&calln_code[2], CONST1);
  caml_set_instruction(&calln_code[3], C_CALLN);
  calln_code[4] = 2;
  calln_code[5] = compare_primitive;
  caml_set_instruction(&calln_code[6], STOP);

  caml_set_instruction(&poison_code[0], CONST2);
  caml_set_instruction(&poison_code[1], PUSH);
  caml_set_instruction(&poison_code[2], CONST1);
  caml_set_instruction(&poison_code[3], C_CALL2);
  poison_code[4] = poison_primitive;
  caml_set_instruction(&poison_code[5], STOP);

  caml_set_instruction(&one_past_code[0], CONST2);
  caml_set_instruction(&one_past_code[1], PUSH);
  caml_set_instruction(&one_past_code[2], CONST1);
  caml_set_instruction(&one_past_code[3], C_CALL2);
  one_past_code[4] = caml_prim_table.size;
  caml_set_instruction(&one_past_code[5], STOP);

  caml_set_instruction(&setglobal_code[0], CONST0);
  caml_set_instruction(&setglobal_code[1], SETGLOBAL);
  setglobal_code[2] = 0;
  caml_set_instruction(&setglobal_code[3], STOP);

  caml_set_instruction(&object_code[0], CONST0);
  caml_set_instruction(&object_code[1], GETMETHOD);
  caml_set_instruction(&object_code[2], STOP);

  caml_set_instruction(&debugger_code[0], EVENT);
  caml_set_instruction(&debugger_code[1], STOP);

  caml_set_instruction(&unsupported_tag_code[0], CONST0);
  caml_set_instruction(&unsupported_tag_code[1], MAKEBLOCK1);
  unsupported_tag_code[2] = Custom_tag;
  caml_set_instruction(&unsupported_tag_code[3], STOP);

  caml_set_instruction(&quota_code[0], CONST0);
  caml_set_instruction(&quota_code[1], MAKEBLOCK1);
  quota_code[2] = 0;
  caml_set_instruction(&quota_code[3], STOP);

  caml_set_instruction(&poptrap_code[0], PUSHTRAP);
  poptrap_code[1] = 2;
  caml_set_instruction(&poptrap_code[2], POPTRAP);
  caml_set_instruction(&poptrap_code[3], CONST0);
  caml_set_instruction(&poptrap_code[4], STOP);

  register_code(setfield_code, CODE_WORDS(setfield_code));
  register_code(setvect_code, CODE_WORDS(setvect_code));
  register_code(setbytes_code, CODE_WORDS(setbytes_code));
  register_code(setdouble_code, CODE_WORDS(setdouble_code));
  register_code(offsetref_code, CODE_WORDS(offsetref_code));
  register_code(compare_code, CODE_WORDS(compare_code));
  register_code(compare_wrong_arity_code,
                CODE_WORDS(compare_wrong_arity_code));
  register_code(calln_code, CODE_WORDS(calln_code));
  register_code(poison_code, CODE_WORDS(poison_code));
  register_code(one_past_code, CODE_WORDS(one_past_code));
  register_code(setglobal_code, CODE_WORDS(setglobal_code));
  register_code(object_code, CODE_WORDS(object_code));
  register_code(debugger_code, CODE_WORDS(debugger_code));
  register_code(unsupported_tag_code, CODE_WORDS(unsupported_tag_code));
  register_code(quota_code, CODE_WORDS(quota_code));
  register_code(poptrap_code, CODE_WORDS(poptrap_code));
  code_ready = 1;
  return 1;
}

static int run_actor(struct caml_actor_scheduler *scheduler,
                     opcode_t *actor_code, mlsize_t words,
                     mlsize_t quota_words,
                     enum caml_actor_failure expected_failure)
{
  struct caml_actor_snapshot snapshot;
  struct caml_actor_step step;
  enum caml_actor_spawn_status spawn_status;
  uintnat pid;

  spawn_status = caml_actor_scheduler_spawn_code(
    scheduler, actor_code, words * sizeof(opcode_t), Atom(0), 0,
    quota_words, &pid);
  if (spawn_status != CAML_ACTOR_SPAWN_OK) {
    fprintf(stderr, "actor spawn failed: status=%d\n", spawn_status);
    return 0;
  }
  step = caml_actor_scheduler_step(scheduler);
  if (caml_actor_scheduler_snapshot(scheduler, pid, &snapshot)
      != CAML_ACTOR_PID_PRESENT) {
    fprintf(stderr, "actor snapshot lookup failed\n");
    return 0;
  }
  if (expected_failure == CAML_ACTOR_FAILURE_NONE) {
    if (step.reason != CAML_ACTOR_STEP_EXITED
        || snapshot.lifecycle != CAML_ACTOR_LIFECYCLE_EXITED
        || snapshot.failure != CAML_ACTOR_FAILURE_NONE) {
      fprintf(stderr, "actor exit mismatch: step=%d lifecycle=%d failure=%d\n",
              step.reason, snapshot.lifecycle, snapshot.failure);
      return 0;
    }
  } else if (step.reason != CAML_ACTOR_STEP_FAILED
             || snapshot.lifecycle != CAML_ACTOR_LIFECYCLE_FAILED
             || snapshot.failure != expected_failure) {
    fprintf(stderr,
            "actor failure mismatch: step=%d lifecycle=%d failure=%d\n",
            step.reason, snapshot.lifecycle, snapshot.failure);
    return 0;
  }
  if (!caml_actor_scheduler_retire(scheduler, pid)) {
    fprintf(stderr, "actor retirement failed\n");
    return 0;
  }
  step = caml_actor_scheduler_step(scheduler);
  if (step.reason != CAML_ACTOR_STEP_IDLE) {
    fprintf(stderr, "actor scheduler remained ready: step=%d\n", step.reason);
    return 0;
  }
  return 1;
}

CAMLprim value caml_actor_test_runtime_fence(value unit)
{
  CAMLparam1(unit);
  CAMLlocal2(registered_block, unregistered_block);
  struct caml_actor_scheduler *scheduler = NULL;
  struct caml_actor_heap *ledger_heap = NULL;
  struct caml_actor_heap_verify_result verification;
  enum caml_actor_heap_alloc_error allocation_error;
  enum caml_actor_world_status world_status;
  struct caml_actor_step step;
  struct caml_actor_snapshot snapshot;
  value actor_block;
  value interior;
  value global_before;
  uintnat pid;
  int frozen = 0;
  int code = 0;

  (void)unit;
  if (caml_check_pending_actions()) caml_process_pending_actions();
  REQUIRE(prepare_code(), 1);

  registered_block = caml_alloc_small(2, 0);
  Field(registered_block, 0) = Val_long(17);
  Field(registered_block, 1) = Val_long(19);
  unregistered_block = caml_alloc_small(1, 0);
  Field(unregistered_block, 0) = Val_long(23);

  world_status = caml_actor_world_freeze();
  REQUIRE(world_status == CAML_ACTOR_WORLD_OK, 2);
  frozen = 1;
  REQUIRE(caml_actor_world_is_frozen(), 3);
  REQUIRE(caml_actor_world_freeze() == CAML_ACTOR_WORLD_BUSY, 4);

  REQUIRE(!caml_actor_world_value_is_frozen(registered_block), 5);
  REQUIRE(caml_actor_world_register_frozen(registered_block), 6);
  REQUIRE(caml_actor_world_register_frozen(registered_block), 7);
  REQUIRE(caml_actor_world_value_is_frozen(registered_block), 8);
  REQUIRE(!caml_actor_world_value_is_frozen(unregistered_block), 9);
  REQUIRE(!caml_actor_world_register_frozen((value)2), 10);
  REQUIRE(!caml_actor_world_value_is_frozen((value)2), 11);
  interior = (value)&Field(registered_block, 1);
  REQUIRE(!caml_actor_world_register_frozen(interior), 12);
  REQUIRE(!caml_actor_world_value_is_frozen(interior), 13);
  REQUIRE(caml_shared_try_alloc(
            Caml_state->shared_heap, 1, 0, 0) == NULL, 53);

  ledger_heap = caml_actor_heap_create(0xF00D, 64);
  REQUIRE(ledger_heap != NULL, 14);
  REQUIRE(caml_actor_heap_activate(ledger_heap), 15);
  actor_block = caml_actor_heap_try_alloc(
    ledger_heap, 1, 0, 0, &allocation_error);
  REQUIRE(actor_block != 0
          && allocation_error == CAML_ACTOR_HEAP_ALLOC_OK, 16);
  REQUIRE(caml_actor_heap_check_field_store(
            actor_block, 0, registered_block)
          == CAML_ACTOR_HEAP_STORE_OK, 50);
  caml_modify(&Field(actor_block, 0), registered_block);
  REQUIRE(caml_actor_heap_check_field_store(
            actor_block, 0, unregistered_block)
          == CAML_ACTOR_HEAP_STORE_INVALID, 51);
  REQUIRE(caml_actor_heap_check_field_store(
            registered_block, 0, Val_long(29))
          == CAML_ACTOR_HEAP_STORE_INVALID, 52);
  caml_actor_heap_deactivate();
  verification = caml_actor_heap_verify(ledger_heap);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 17);
  Field(actor_block, 0) = unregistered_block;
  verification = caml_actor_heap_verify(ledger_heap);
  REQUIRE(verification.error
          == CAML_ACTOR_HEAP_VERIFY_UNAPPROVED_EXTERNAL_EDGE, 18);
  Field(actor_block, 0) = registered_block;
  verification = caml_actor_heap_verify(ledger_heap);
  REQUIRE(verification.error == CAML_ACTOR_HEAP_VERIFY_OK, 19);
  caml_actor_heap_destroy(ledger_heap);
  ledger_heap = NULL;

  scheduler = caml_actor_scheduler_create(2, 128);
  REQUIRE(scheduler != NULL, 20);
  REQUIRE(run_actor(scheduler, setfield_code, CODE_WORDS(setfield_code),
                    64, CAML_ACTOR_FAILURE_NONE), 21);
  REQUIRE(run_actor(scheduler, setvect_code, CODE_WORDS(setvect_code),
                    64, CAML_ACTOR_FAILURE_NONE), 22);
  REQUIRE(run_actor(scheduler, setbytes_code, CODE_WORDS(setbytes_code),
                    64, CAML_ACTOR_FAILURE_NONE), 23);
  REQUIRE(run_actor(scheduler, setdouble_code, CODE_WORDS(setdouble_code),
                    64, CAML_ACTOR_FAILURE_NONE), 24);
  REQUIRE(run_actor(scheduler, offsetref_code, CODE_WORDS(offsetref_code),
                    64, CAML_ACTOR_FAILURE_NONE), 25);
  REQUIRE(run_actor(scheduler, compare_code, CODE_WORDS(compare_code),
                    64, CAML_ACTOR_FAILURE_NONE), 26);

  poison_entries = 0;
  REQUIRE(run_actor(scheduler, poison_code, CODE_WORDS(poison_code),
                    64, CAML_ACTOR_FAILURE_UNSUPPORTED), 27);
  REQUIRE(poison_entries == 0, 28);
  REQUIRE(run_actor(scheduler, one_past_code, CODE_WORDS(one_past_code),
                    64, CAML_ACTOR_FAILURE_UNSUPPORTED), 29);
  REQUIRE(run_actor(scheduler, compare_wrong_arity_code,
                    CODE_WORDS(compare_wrong_arity_code), 64,
                    CAML_ACTOR_FAILURE_UNSUPPORTED), 49);
  REQUIRE(run_actor(scheduler, calln_code, CODE_WORDS(calln_code),
                    64, CAML_ACTOR_FAILURE_UNSUPPORTED), 48);

  global_before = Field(caml_global_data, 0);
  REQUIRE(run_actor(scheduler, setglobal_code,
                    CODE_WORDS(setglobal_code), 64,
                    CAML_ACTOR_FAILURE_UNSUPPORTED), 30);
  REQUIRE(Field(caml_global_data, 0) == global_before, 31);
  REQUIRE(run_actor(scheduler, object_code, CODE_WORDS(object_code),
                    64, CAML_ACTOR_FAILURE_UNSUPPORTED), 32);
  REQUIRE(run_actor(scheduler, debugger_code, CODE_WORDS(debugger_code),
                    64, CAML_ACTOR_FAILURE_UNSUPPORTED), 33);
  REQUIRE(run_actor(scheduler, unsupported_tag_code,
                    CODE_WORDS(unsupported_tag_code), 64,
                    CAML_ACTOR_FAILURE_UNSUPPORTED), 34);
  REQUIRE(run_actor(scheduler, quota_code, CODE_WORDS(quota_code),
                    1, CAML_ACTOR_FAILURE_HEAP_EXHAUSTED), 35);

  REQUIRE(caml_actor_scheduler_spawn_code(
            scheduler, poptrap_code, sizeof(poptrap_code), Atom(0), 0,
            64, &pid) == CAML_ACTOR_SPAWN_OK, 36);
  caml_actor_scheduler_test_request_minor_gc_after_switch(scheduler);
  step = caml_actor_scheduler_step(scheduler);
  REQUIRE(step.reason == CAML_ACTOR_STEP_EXITED && step.pid == pid, 37);
  REQUIRE(caml_actor_scheduler_snapshot(scheduler, pid, &snapshot)
            == CAML_ACTOR_PID_PRESENT
          && snapshot.lifecycle == CAML_ACTOR_LIFECYCLE_EXITED, 38);
  REQUIRE(Caml_check_gc_interrupt(Caml_state), 39);
  REQUIRE(caml_actor_scheduler_retire(scheduler, pid), 40);
  caml_actor_scheduler_destroy(scheduler);
  scheduler = NULL;
  REQUIRE(Caml_check_gc_interrupt(Caml_state), 41);

  world_status = caml_actor_world_thaw();
  frozen = 0;
  REQUIRE(world_status == CAML_ACTOR_WORLD_OK, 42);
  REQUIRE(!caml_actor_world_is_frozen(), 43);
  REQUIRE(Caml_check_gc_interrupt(Caml_state), 44);
  caml_process_pending_actions();
  REQUIRE(!Caml_check_gc_interrupt(Caml_state), 45);
  REQUIRE(caml_actor_world_thaw() == CAML_ACTOR_WORLD_BUSY, 46);

  world_status = caml_actor_world_freeze();
  REQUIRE(world_status == CAML_ACTOR_WORLD_OK, 54);
  frozen = 1;
  REQUIRE(caml_actor_world_register_frozen(registered_block), 55);
  Field(registered_block, 0) = Val_long(31);
  REQUIRE(!caml_actor_world_value_is_frozen(registered_block), 56);
  world_status = caml_actor_world_thaw();
  frozen = 0;
  REQUIRE(world_status == CAML_ACTOR_WORLD_CORRUPTED, 57);
  Field(registered_block, 0) = Val_long(17);
  REQUIRE(caml_actor_world_thaw() == CAML_ACTOR_WORLD_BUSY, 58);
  REQUIRE(Caml_state->actor_world == NULL
          && Caml_state->actor_scheduler == NULL
          && Caml_state->actor_heap == NULL, 47);

cleanup:
  if (caml_actor_heap_current() != NULL) caml_actor_heap_deactivate();
  if (ledger_heap != NULL) caml_actor_heap_destroy(ledger_heap);
  if (scheduler != NULL) caml_actor_scheduler_destroy(scheduler);
  if (frozen) (void)caml_actor_world_thaw();
  if (caml_check_pending_actions()) caml_process_pending_actions();
  CAMLreturn(Val_int(code));
}
