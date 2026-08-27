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

#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>

#include "caml/actor_scheduler.h"
#include "caml/actor_world.h"
#include "caml/alloc.h"
#include "caml/codefrag.h"
#include "caml/fiber.h"
#include "caml/fix_code.h"
#include "caml/instruct.h"
#include "caml/memory.h"
#include "caml/mlvalues.h"

#define CODE_WORDS(code) (sizeof(code) / sizeof((code)[0]))

CAMLextern value caml_realloc_global(value size);

static opcode_t getglobal_ok[10];
static opcode_t pushgetglobal_ok[11];
static opcode_t getglobalfield_ok[11];
static opcode_t pushgetglobalfield_ok[12];
static opcode_t getglobal_oob[3];
static opcode_t pushgetglobal_oob[4];
static opcode_t getglobalfield_global_oob[4];
static opcode_t pushgetglobalfield_global_oob[5];
static opcode_t getglobalfield_field_oob[4];
static opcode_t pushgetglobalfield_field_oob[5];
static opcode_t getglobalfield_immediate[4];
static opcode_t pushgetglobalfield_immediate[5];
static opcode_t getglobal_negative[3];
static opcode_t pushgetglobal_negative[4];
static opcode_t getglobalfield_negative_global[4];
static opcode_t pushgetglobalfield_negative_global[5];
static opcode_t getglobalfield_negative_field[4];
static opcode_t pushgetglobalfield_negative_field[5];
static opcode_t setglobal_code[4];

static void register_code(opcode_t *code, mlsize_t words)
{
  caml_register_code_fragment(
    (char *)code, (char *)(code + words), DIGEST_IGNORE, NULL);
}

static int prepare_code(opcode_t immediate_index, opcode_t block_index,
                        opcode_t invalid_index)
{
  caml_set_instruction(&getglobal_ok[0], GETGLOBAL);
  getglobal_ok[1] = immediate_index;
  caml_set_instruction(&getglobal_ok[2], BEQ);
  getglobal_ok[3] = 101;
  getglobal_ok[4] = 5;
  caml_set_instruction(&getglobal_ok[5], CONST0);
  caml_set_instruction(&getglobal_ok[6], MAKEBLOCK1);
  getglobal_ok[7] = 0;
  caml_set_instruction(&getglobal_ok[8], STOP);
  caml_set_instruction(&getglobal_ok[9], STOP);

  caml_set_instruction(&pushgetglobal_ok[0], CONST0);
  caml_set_instruction(&pushgetglobal_ok[1], PUSHGETGLOBAL);
  pushgetglobal_ok[2] = immediate_index;
  caml_set_instruction(&pushgetglobal_ok[3], BEQ);
  pushgetglobal_ok[4] = 101;
  pushgetglobal_ok[5] = 5;
  caml_set_instruction(&pushgetglobal_ok[6], CONST0);
  caml_set_instruction(&pushgetglobal_ok[7], MAKEBLOCK1);
  pushgetglobal_ok[8] = 0;
  caml_set_instruction(&pushgetglobal_ok[9], STOP);
  caml_set_instruction(&pushgetglobal_ok[10], STOP);

  caml_set_instruction(&getglobalfield_ok[0], GETGLOBALFIELD);
  getglobalfield_ok[1] = block_index;
  getglobalfield_ok[2] = 0;
  caml_set_instruction(&getglobalfield_ok[3], BEQ);
  getglobalfield_ok[4] = 103;
  getglobalfield_ok[5] = 5;
  caml_set_instruction(&getglobalfield_ok[6], CONST0);
  caml_set_instruction(&getglobalfield_ok[7], MAKEBLOCK1);
  getglobalfield_ok[8] = 0;
  caml_set_instruction(&getglobalfield_ok[9], STOP);
  caml_set_instruction(&getglobalfield_ok[10], STOP);

  caml_set_instruction(&pushgetglobalfield_ok[0], CONST0);
  caml_set_instruction(&pushgetglobalfield_ok[1], PUSHGETGLOBALFIELD);
  pushgetglobalfield_ok[2] = block_index;
  pushgetglobalfield_ok[3] = 1;
  caml_set_instruction(&pushgetglobalfield_ok[4], BEQ);
  pushgetglobalfield_ok[5] = 107;
  pushgetglobalfield_ok[6] = 5;
  caml_set_instruction(&pushgetglobalfield_ok[7], CONST0);
  caml_set_instruction(&pushgetglobalfield_ok[8], MAKEBLOCK1);
  pushgetglobalfield_ok[9] = 0;
  caml_set_instruction(&pushgetglobalfield_ok[10], STOP);
  caml_set_instruction(&pushgetglobalfield_ok[11], STOP);

  caml_set_instruction(&getglobal_oob[0], GETGLOBAL);
  getglobal_oob[1] = invalid_index;
  caml_set_instruction(&getglobal_oob[2], STOP);

  caml_set_instruction(&pushgetglobal_oob[0], CONST0);
  caml_set_instruction(&pushgetglobal_oob[1], PUSHGETGLOBAL);
  pushgetglobal_oob[2] = invalid_index;
  caml_set_instruction(&pushgetglobal_oob[3], STOP);

  caml_set_instruction(&getglobalfield_global_oob[0], GETGLOBALFIELD);
  getglobalfield_global_oob[1] = invalid_index;
  getglobalfield_global_oob[2] = 0;
  caml_set_instruction(&getglobalfield_global_oob[3], STOP);

  caml_set_instruction(&pushgetglobalfield_global_oob[0], CONST0);
  caml_set_instruction(
    &pushgetglobalfield_global_oob[1], PUSHGETGLOBALFIELD);
  pushgetglobalfield_global_oob[2] = invalid_index;
  pushgetglobalfield_global_oob[3] = 0;
  caml_set_instruction(&pushgetglobalfield_global_oob[4], STOP);

  caml_set_instruction(&getglobalfield_field_oob[0], GETGLOBALFIELD);
  getglobalfield_field_oob[1] = block_index;
  getglobalfield_field_oob[2] = 2;
  caml_set_instruction(&getglobalfield_field_oob[3], STOP);

  caml_set_instruction(&pushgetglobalfield_field_oob[0], CONST0);
  caml_set_instruction(
    &pushgetglobalfield_field_oob[1], PUSHGETGLOBALFIELD);
  pushgetglobalfield_field_oob[2] = block_index;
  pushgetglobalfield_field_oob[3] = 2;
  caml_set_instruction(&pushgetglobalfield_field_oob[4], STOP);

  caml_set_instruction(&getglobalfield_immediate[0], GETGLOBALFIELD);
  getglobalfield_immediate[1] = immediate_index;
  getglobalfield_immediate[2] = 0;
  caml_set_instruction(&getglobalfield_immediate[3], STOP);

  caml_set_instruction(&pushgetglobalfield_immediate[0], CONST0);
  caml_set_instruction(
    &pushgetglobalfield_immediate[1], PUSHGETGLOBALFIELD);
  pushgetglobalfield_immediate[2] = immediate_index;
  pushgetglobalfield_immediate[3] = 0;
  caml_set_instruction(&pushgetglobalfield_immediate[4], STOP);

  caml_set_instruction(&getglobal_negative[0], GETGLOBAL);
  getglobal_negative[1] = -1;
  caml_set_instruction(&getglobal_negative[2], STOP);

  caml_set_instruction(&pushgetglobal_negative[0], CONST0);
  caml_set_instruction(&pushgetglobal_negative[1], PUSHGETGLOBAL);
  pushgetglobal_negative[2] = -1;
  caml_set_instruction(&pushgetglobal_negative[3], STOP);

  caml_set_instruction(
    &getglobalfield_negative_global[0], GETGLOBALFIELD);
  getglobalfield_negative_global[1] = -1;
  getglobalfield_negative_global[2] = 0;
  caml_set_instruction(&getglobalfield_negative_global[3], STOP);

  caml_set_instruction(
    &pushgetglobalfield_negative_global[0], CONST0);
  caml_set_instruction(
    &pushgetglobalfield_negative_global[1], PUSHGETGLOBALFIELD);
  pushgetglobalfield_negative_global[2] = -1;
  pushgetglobalfield_negative_global[3] = 0;
  caml_set_instruction(&pushgetglobalfield_negative_global[4], STOP);

  caml_set_instruction(
    &getglobalfield_negative_field[0], GETGLOBALFIELD);
  getglobalfield_negative_field[1] = block_index;
  getglobalfield_negative_field[2] = -1;
  caml_set_instruction(&getglobalfield_negative_field[3], STOP);

  caml_set_instruction(
    &pushgetglobalfield_negative_field[0], CONST0);
  caml_set_instruction(
    &pushgetglobalfield_negative_field[1], PUSHGETGLOBALFIELD);
  pushgetglobalfield_negative_field[2] = block_index;
  pushgetglobalfield_negative_field[3] = -1;
  caml_set_instruction(&pushgetglobalfield_negative_field[4], STOP);

  caml_set_instruction(&setglobal_code[0], CONST3);
  caml_set_instruction(&setglobal_code[1], SETGLOBAL);
  setglobal_code[2] = immediate_index;
  caml_set_instruction(&setglobal_code[3], STOP);

  register_code(getglobal_ok, CODE_WORDS(getglobal_ok));
  register_code(pushgetglobal_ok, CODE_WORDS(pushgetglobal_ok));
  register_code(getglobalfield_ok, CODE_WORDS(getglobalfield_ok));
  register_code(
    pushgetglobalfield_ok, CODE_WORDS(pushgetglobalfield_ok));
  register_code(getglobal_oob, CODE_WORDS(getglobal_oob));
  register_code(pushgetglobal_oob, CODE_WORDS(pushgetglobal_oob));
  register_code(
    getglobalfield_global_oob, CODE_WORDS(getglobalfield_global_oob));
  register_code(pushgetglobalfield_global_oob,
                CODE_WORDS(pushgetglobalfield_global_oob));
  register_code(
    getglobalfield_field_oob, CODE_WORDS(getglobalfield_field_oob));
  register_code(pushgetglobalfield_field_oob,
                CODE_WORDS(pushgetglobalfield_field_oob));
  register_code(
    getglobalfield_immediate, CODE_WORDS(getglobalfield_immediate));
  register_code(pushgetglobalfield_immediate,
                CODE_WORDS(pushgetglobalfield_immediate));
  register_code(getglobal_negative, CODE_WORDS(getglobal_negative));
  register_code(
    pushgetglobal_negative, CODE_WORDS(pushgetglobal_negative));
  register_code(getglobalfield_negative_global,
                CODE_WORDS(getglobalfield_negative_global));
  register_code(pushgetglobalfield_negative_global,
                CODE_WORDS(pushgetglobalfield_negative_global));
  register_code(getglobalfield_negative_field,
                CODE_WORDS(getglobalfield_negative_field));
  register_code(pushgetglobalfield_negative_field,
                CODE_WORDS(pushgetglobalfield_negative_field));
  register_code(setglobal_code, CODE_WORDS(setglobal_code));
  return 1;
}

static int run_actor(struct caml_actor_scheduler *scheduler,
                     const char *label, opcode_t *code, mlsize_t words,
                     int expect_exit, opcode_t expected_opcode)
{
  struct caml_actor_snapshot snapshot;
  struct caml_actor_step step;
  uintnat pid;
  int matches;

  if (caml_actor_scheduler_spawn_code(
        scheduler, code, words * sizeof(opcode_t), Atom(0), 0, 64, &pid)
      != CAML_ACTOR_SPAWN_OK) {
    fprintf(stderr, "%s: actor spawn failed\n", label);
    return 0;
  }
  step = caml_actor_scheduler_step(scheduler);
  if (caml_actor_scheduler_snapshot(scheduler, pid, &snapshot)
      != CAML_ACTOR_PID_PRESENT) {
    fprintf(stderr, "%s: actor snapshot failed\n", label);
    return 0;
  }
  if (expect_exit) {
    matches = step.reason == CAML_ACTOR_STEP_EXITED
      && snapshot.lifecycle == CAML_ACTOR_LIFECYCLE_EXITED
      && snapshot.failure == CAML_ACTOR_FAILURE_NONE;
  } else {
    matches = step.reason == CAML_ACTOR_STEP_FAILED
      && snapshot.lifecycle == CAML_ACTOR_LIFECYCLE_FAILED
      && snapshot.failure == CAML_ACTOR_FAILURE_UNSUPPORTED
      && snapshot.unsupported.kind == CAML_ACTOR_UNSUPPORTED_OPCODE
      && snapshot.unsupported.operation == (uintnat)expected_opcode;
  }
  if (!matches) {
    fprintf(stderr,
            "%s: step=%d lifecycle=%d failure=%d unsupported=%d/%" PRIuMAX
            " expected=%s/%d\n",
            label, step.reason, snapshot.lifecycle, snapshot.failure,
            snapshot.unsupported.kind,
            (uintmax_t)snapshot.unsupported.operation,
            expect_exit ? "exit" : "opcode", expected_opcode);
  }
  if (!caml_actor_scheduler_retire(scheduler, pid)) {
    fprintf(stderr, "%s: actor retirement failed\n", label);
    return 0;
  }
  step = caml_actor_scheduler_step(scheduler);
  if (step.reason != CAML_ACTOR_STEP_IDLE) {
    fprintf(stderr, "%s: scheduler remained ready\n", label);
    return 0;
  }
  return matches;
}

CAMLprim value caml_actor_test_frozen_global_opcodes(value unit)
{
  CAMLparam1(unit);
  CAMLlocal1(global_block);
  struct caml_actor_scheduler *scheduler = NULL;
  enum caml_actor_world_status world_status;
  header_t block_header;
  header_t global_header;
  mlsize_t old_globals;
  mlsize_t global_count;
  value read_value;
  value setglobal_before;
  opcode_t immediate_index;
  opcode_t block_index;
  opcode_t invalid_index;
  int frozen = 0;
  int rejected;
  int result = 0;

  (void)unit;
  old_globals = Wosize_val(caml_global_data);
  if (old_globals > (mlsize_t)INT32_MAX - 2) CAMLreturn(Val_int(1));
  caml_realloc_global(Val_long(old_globals + 2));
  global_count = Wosize_val(caml_global_data);
  if (global_count > (mlsize_t)INT32_MAX) CAMLreturn(Val_int(2));

  immediate_index = (opcode_t)old_globals;
  block_index = (opcode_t)(old_globals + 1);
  invalid_index = (opcode_t)global_count;
  caml_modify(&Field(caml_global_data, immediate_index), Val_long(101));
  global_block = caml_alloc_small(2, 0);
  Field(global_block, 0) = Val_long(103);
  Field(global_block, 1) = Val_long(107);
  caml_modify(&Field(caml_global_data, block_index), global_block);
  if (!prepare_code(immediate_index, block_index, invalid_index)) {
    CAMLreturn(Val_int(3));
  }

  world_status = caml_actor_world_freeze();
  if (world_status != CAML_ACTOR_WORLD_OK) CAMLreturn(Val_int(4));
  frozen = 1;
  if (caml_actor_world_prepare_global_image() != CAML_ACTOR_GLOBAL_OK) {
    result = 5;
    goto cleanup;
  }

  global_header = Hd_val(caml_global_data);
  atomic_store_relaxed(
    Hp_atomic_val(caml_global_data), Hd_with_tag(global_header, 1));
  rejected = !caml_actor_world_read_global(
    (uintnat)immediate_index, &read_value);
  atomic_store_relaxed(Hp_atomic_val(caml_global_data), global_header);
  if (!rejected) {
    result = 7;
    goto cleanup;
  }

  setglobal_before = Field(caml_global_data, immediate_index);
  Field(caml_global_data, immediate_index) = Val_long(102);
  rejected = !caml_actor_world_read_global(
    (uintnat)immediate_index, &read_value);
  Field(caml_global_data, immediate_index) = setglobal_before;
  if (!rejected) {
    result = 8;
    goto cleanup;
  }

  setglobal_before = Field(global_block, 0);
  Field(global_block, 0) = Val_long(104);
  rejected = !caml_actor_world_read_global_field(
    (uintnat)block_index, 0, &read_value);
  Field(global_block, 0) = setglobal_before;
  if (!rejected) {
    result = 9;
    goto cleanup;
  }

  block_header = Hd_val(global_block);
  atomic_store_relaxed(
    Hp_atomic_val(global_block), Hd_with_tag(block_header, 1));
  rejected = !caml_actor_world_read_global_field(
    (uintnat)block_index, 0, &read_value);
  atomic_store_relaxed(Hp_atomic_val(global_block), block_header);
  if (!rejected) {
    result = 31;
    goto cleanup;
  }
  if (!caml_actor_world_read_global_field(
        (uintnat)block_index, 0, &read_value)
      || read_value != Val_long(103)) {
    result = 32;
    goto cleanup;
  }

  scheduler = caml_actor_scheduler_create(2, 128);
  if (scheduler == NULL) {
    result = 6;
    goto cleanup;
  }

#define EXPECT(label, code, exits, opcode, failure) \
  do { \
    if (!run_actor(scheduler, (label), (code), CODE_WORDS(code), \
                   (exits), (opcode))) { \
      result = (failure); \
      goto cleanup; \
    } \
  } while (0)
  EXPECT("GETGLOBAL valid", getglobal_ok, 1, GETGLOBAL, 10);
  EXPECT("PUSHGETGLOBAL valid", pushgetglobal_ok, 1, PUSHGETGLOBAL, 11);
  EXPECT("GETGLOBALFIELD valid", getglobalfield_ok, 1,
         GETGLOBALFIELD, 12);
  EXPECT("PUSHGETGLOBALFIELD valid", pushgetglobalfield_ok, 1,
         PUSHGETGLOBALFIELD, 13);
  EXPECT("GETGLOBAL global bound", getglobal_oob, 0, GETGLOBAL, 14);
  EXPECT("PUSHGETGLOBAL global bound", pushgetglobal_oob, 0,
         PUSHGETGLOBAL, 15);
  EXPECT("GETGLOBALFIELD global bound", getglobalfield_global_oob, 0,
         GETGLOBALFIELD, 16);
  EXPECT("PUSHGETGLOBALFIELD global bound",
         pushgetglobalfield_global_oob, 0, PUSHGETGLOBALFIELD, 17);
  EXPECT("GETGLOBALFIELD field bound", getglobalfield_field_oob, 0,
         GETGLOBALFIELD, 18);
  EXPECT("PUSHGETGLOBALFIELD field bound", pushgetglobalfield_field_oob,
         0, PUSHGETGLOBALFIELD, 19);
  EXPECT("GETGLOBALFIELD immediate", getglobalfield_immediate, 0,
         GETGLOBALFIELD, 20);
  EXPECT("PUSHGETGLOBALFIELD immediate", pushgetglobalfield_immediate, 0,
         PUSHGETGLOBALFIELD, 21);
  EXPECT("GETGLOBAL negative", getglobal_negative, 0, GETGLOBAL, 25);
  EXPECT("PUSHGETGLOBAL negative", pushgetglobal_negative, 0,
         PUSHGETGLOBAL, 26);
  EXPECT("GETGLOBALFIELD negative global",
         getglobalfield_negative_global, 0, GETGLOBALFIELD, 27);
  EXPECT("PUSHGETGLOBALFIELD negative global",
         pushgetglobalfield_negative_global, 0,
         PUSHGETGLOBALFIELD, 28);
  EXPECT("GETGLOBALFIELD negative field",
         getglobalfield_negative_field, 0, GETGLOBALFIELD, 29);
  EXPECT("PUSHGETGLOBALFIELD negative field",
         pushgetglobalfield_negative_field, 0,
         PUSHGETGLOBALFIELD, 30);
  setglobal_before = Field(caml_global_data, immediate_index);
  EXPECT("SETGLOBAL", setglobal_code, 0, SETGLOBAL, 22);
  if (Field(caml_global_data, immediate_index) != setglobal_before) {
    result = 23;
    goto cleanup;
  }
#undef EXPECT

cleanup:
  if (scheduler != NULL) caml_actor_scheduler_destroy(scheduler);
  if (frozen) {
    world_status = caml_actor_world_thaw();
    if (world_status != CAML_ACTOR_WORLD_OK && result == 0) result = 24;
  }
  CAMLreturn(Val_int(result));
}
