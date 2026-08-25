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

#include "caml/actor_heap.h"
#include "caml/actor_scheduler.h"
#include "caml/actor_world.h"
#include "caml/alloc.h"
#include "caml/domain_state.h"
#include "caml/fail.h"
#include "caml/memory.h"
#include "caml/mlvalues.h"

#define ACTOR_MVP_CAPACITY 1024
#define ACTOR_MVP_REDUCTIONS 1000
#define ACTOR_MVP_ROOT_HEAP_WORDS ((mlsize_t)1 << 18)
#define ACTOR_MVP_CHILD_HEAP_WORDS ((mlsize_t)1 << 16)

enum actor_run_outcome {
  ACTOR_RUN_OK = 0,
  ACTOR_RUN_UNSUPPORTED,
  ACTOR_RUN_ROOT_FAILED,
  ACTOR_RUN_ROOT_HEAP_EXHAUSTED,
  ACTOR_RUN_DEADLOCK
};

#if !defined(NATIVE_CODE)

static value actor_try_alloc(mlsize_t wosize, tag_t tag)
{
  struct caml_actor_heap *heap = caml_actor_heap_current();
  enum caml_actor_heap_alloc_error error;
  value block = caml_actor_heap_try_alloc(
    heap, wosize, tag, 0, &error);

  if (block != 0) return block;
  if (error == CAML_ACTOR_HEAP_ALLOC_QUOTA) {
    caml_actor_scheduler_request_heap_exhausted();
  } else {
    caml_actor_scheduler_request_unsupported();
  }
  return 0;
}

static value actor_alloc_one(tag_t tag, value field)
{
  value block = actor_try_alloc(1, tag);

  if (block != 0) Field(block, 0) = field;
  return block;
}

static value actor_copy_string(const char *text)
{
  mlsize_t length = strlen(text);
  mlsize_t wosize = (length + sizeof(value)) / sizeof(value);
  mlsize_t offset_index;
  value string = actor_try_alloc(wosize, String_tag);

  if (string == 0) return 0;
  Field(string, wosize - 1) = 0;
  offset_index = Bsize_wsize(wosize) - 1;
  Byte(string, offset_index) = offset_index - length;
  memcpy(Bytes_val(string), text, length);
  return string;
}

static value actor_spawn_error(enum caml_actor_spawn_status status)
{
  value detail;
  value error;

  switch (status) {
  case CAML_ACTOR_SPAWN_LIMIT:
    detail = Val_int(0); /* Actor_limit */
    break;
  case CAML_ACTOR_SPAWN_INITIAL_HEAP_LIMIT:
    detail = Val_int(1); /* Initial_heap_limit */
    break;
  default: {
    value message = actor_copy_string("unsupported closure capture");

    if (message == 0) return 0;
    detail = actor_alloc_one(0, message); /* Unsupported_capture */
    if (detail == 0) return 0;
    break;
  }
  }
  error = actor_alloc_one(1, detail); /* Error */
  return error;
}

static enum actor_run_outcome root_failure_outcome(
  const struct caml_actor_snapshot *snapshot, const char **message)
{
  if (snapshot->failure == CAML_ACTOR_FAILURE_HEAP_EXHAUSTED) {
    return ACTOR_RUN_ROOT_HEAP_EXHAUSTED;
  }
  switch (snapshot->failure) {
  case CAML_ACTOR_FAILURE_EXCEPTION:
    *message = "uncaught root actor exception";
    break;
  case CAML_ACTOR_FAILURE_UNSUPPORTED:
    *message = "unsupported operation in root actor";
    break;
  case CAML_ACTOR_FAILURE_INVALID_HEAP:
    *message = "root actor heap invariant failed";
    break;
  case CAML_ACTOR_FAILURE_INVALID_RESULT:
    *message = "root actor returned an invalid value";
    break;
  default:
    *message = "root actor runtime failure";
    break;
  }
  return ACTOR_RUN_ROOT_FAILED;
}

#endif

CAMLprim value caml_actor_run(value root)
{
  CAMLparam1(root);
  CAMLlocal3(result, error, message_value);
  enum actor_run_outcome outcome = ACTOR_RUN_UNSUPPORTED;
  const char *message = "actor runtime unavailable";

#if !defined(NATIVE_CODE)
  struct caml_actor_scheduler *scheduler = NULL;
  struct caml_actor_prepared_spawn *prepared = NULL;
  enum caml_actor_spawn_status spawn_status;
  enum caml_actor_world_status world_status;
  uintnat root_pid = 0;
  int frozen = 0;

  world_status = caml_actor_world_freeze();
  if (world_status != CAML_ACTOR_WORLD_OK) goto finished;
  frozen = 1;
  scheduler = caml_actor_scheduler_create(
    ACTOR_MVP_CAPACITY, ACTOR_MVP_REDUCTIONS);
  if (scheduler == NULL) goto cleanup;

  spawn_status = caml_actor_scheduler_prepare_root_closure(
    scheduler, root, ACTOR_MVP_ROOT_HEAP_WORDS, &prepared);
  if (spawn_status != CAML_ACTOR_SPAWN_OK) {
    if (spawn_status == CAML_ACTOR_SPAWN_INITIAL_HEAP_LIMIT) {
      outcome = ACTOR_RUN_ROOT_HEAP_EXHAUSTED;
    } else {
      outcome = ACTOR_RUN_ROOT_FAILED;
      message = spawn_status == CAML_ACTOR_SPAWN_UNSUPPORTED_CAPTURE
        ? "unsupported root closure capture"
        : "root actor resources unavailable";
    }
    goto cleanup;
  }
  root_pid = caml_actor_scheduler_prepared_pid(prepared);
  if (!caml_actor_scheduler_commit_prepared(prepared)) {
    prepared = NULL;
    outcome = ACTOR_RUN_ROOT_FAILED;
    message = "root actor publication failed";
    goto cleanup;
  }
  prepared = NULL;

  for (;;) {
    struct caml_actor_step step = caml_actor_scheduler_step(scheduler);

    if (step.reason == CAML_ACTOR_STEP_REDUCTIONS
        || step.reason == CAML_ACTOR_STEP_YIELD) {
      continue;
    }
    if (step.reason == CAML_ACTOR_STEP_IDLE) {
      outcome = ACTOR_RUN_DEADLOCK;
      break;
    }
    if (step.reason == CAML_ACTOR_STEP_HOST_ACTION) {
      outcome = ACTOR_RUN_ROOT_FAILED;
      message = "host action escaped the frozen actor world";
      break;
    }
    if (step.reason == CAML_ACTOR_STEP_EXITED
        || step.reason == CAML_ACTOR_STEP_FAILED) {
      if (step.pid == root_pid) {
        if (step.reason == CAML_ACTOR_STEP_EXITED) {
          outcome = ACTOR_RUN_OK;
        } else {
          struct caml_actor_snapshot snapshot;

          if (caml_actor_scheduler_snapshot(
                scheduler, root_pid, &snapshot)
              != CAML_ACTOR_PID_PRESENT) {
            outcome = ACTOR_RUN_ROOT_FAILED;
            message = "root actor status unavailable";
          } else {
            outcome = root_failure_outcome(&snapshot, &message);
          }
        }
        break;
      }
      if (!caml_actor_scheduler_retire(scheduler, step.pid)) {
        outcome = ACTOR_RUN_ROOT_FAILED;
        message = "child actor retirement failed";
        break;
      }
      continue;
    }

    outcome = ACTOR_RUN_ROOT_FAILED;
    message = "unknown actor scheduler result";
    break;
  }

cleanup:
  if (prepared != NULL) caml_actor_scheduler_abort_prepared(prepared);
  if (scheduler != NULL) caml_actor_scheduler_destroy(scheduler);
  if (frozen) {
    world_status = caml_actor_world_thaw();
    if (world_status != CAML_ACTOR_WORLD_OK) {
      outcome = ACTOR_RUN_ROOT_FAILED;
      message = "frozen host state changed during actor execution";
    }
  }

finished:
#endif
  if (outcome == ACTOR_RUN_OK) {
    result = caml_alloc_small(1, 0); /* Ok */
    Field(result, 0) = Val_unit;
    CAMLreturn(result);
  }

  if (outcome == ACTOR_RUN_ROOT_FAILED) {
    message_value = caml_copy_string(message);
    error = caml_alloc_small(1, 0); /* Root_failed */
    Field(error, 0) = message_value;
  } else if (outcome == ACTOR_RUN_ROOT_HEAP_EXHAUSTED) {
    error = Val_int(1);
  } else if (outcome == ACTOR_RUN_DEADLOCK) {
    error = Val_int(2);
  } else {
    error = Val_int(0); /* Unsupported_runtime */
  }
  result = caml_alloc_small(1, 1); /* Error */
  Field(result, 0) = error;
  CAMLreturn(result);
}

CAMLprim value caml_actor_spawn(value closure)
{
#if defined(NATIVE_CODE)
  (void)closure;
  caml_invalid_argument("Actor.spawn outside an actor world");
#else
  struct caml_actor_scheduler *scheduler;
  struct caml_actor_prepared_spawn *prepared = NULL;
  enum caml_actor_spawn_status status;
  uintnat pid;
  value result;

  if (!caml_actor_scheduler_is_running()) {
    caml_invalid_argument("Actor.spawn outside an actor world");
  }
  scheduler = Caml_state->actor_scheduler;
  status = caml_actor_scheduler_prepare_closure(
    scheduler, closure, ACTOR_MVP_CHILD_HEAP_WORDS, &prepared);
  if (status != CAML_ACTOR_SPAWN_OK) return actor_spawn_error(status);

  pid = caml_actor_scheduler_prepared_pid(prepared);
  result = actor_alloc_one(0, Val_long(pid)); /* Ok */
  if (result == 0) {
    caml_actor_scheduler_abort_prepared(prepared);
    return Val_unit;
  }
  if (!caml_actor_scheduler_commit_prepared(prepared)) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  return result;
#endif
}

CAMLprim value caml_actor_self(value inbox)
{
#if !defined(NATIVE_CODE)
  if (caml_actor_scheduler_is_running()) {
    uintnat pid = caml_actor_scheduler_current_pid();

    if (Is_long(inbox) && Long_val(inbox) == pid) return inbox;
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
#else
  (void)inbox;
#endif
  caml_invalid_argument("Actor.self outside an actor world");
}

CAMLprim value caml_actor_yield(value unit)
{
  (void)unit;
#if !defined(NATIVE_CODE)
  if (caml_actor_scheduler_is_running()) {
    if (!caml_actor_scheduler_request_yield()) {
      caml_actor_scheduler_request_unsupported();
    }
    return Val_unit;
  }
#endif
  caml_invalid_argument("Actor.yield outside an actor world");
}
