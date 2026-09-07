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
#include <string.h>

#include "caml/actor_heap.h"
#include "caml/actor_scheduler.h"
#include "caml/actor_wire.h"
#include "caml/actor_world.h"
#include "caml/alloc.h"
#include "caml/backtrace.h"
#include "caml/domain_state.h"
#include "caml/fail.h"
#include "caml/instruct.h"
#include "caml/memory.h"
#include "caml/mlvalues.h"
#include "caml/opnames.h"
#include "caml/prims.h"

#define ACTOR_MVP_CAPACITY 1024
#define ACTOR_MVP_REDUCTIONS 1000
#define ACTOR_MVP_ROOT_HEAP_WORDS ((mlsize_t)1 << 18)
#define ACTOR_MVP_CHILD_HEAP_WORDS ((mlsize_t)1 << 16)
#define ACTOR_MVP_MESSAGE_WORDS ((mlsize_t)1 << 16)
#define ACTOR_MVP_MAILBOX_MESSAGES ((uintnat)1 << 16)
#define ACTOR_MVP_MAILBOX_BYTES ((uintnat)1 << 28)
#define ACTOR_MVP_MONITORS ((uintnat)1 << 16)

enum actor_run_outcome {
  ACTOR_RUN_OK = 0,
  ACTOR_RUN_UNSUPPORTED,
  ACTOR_RUN_ROOT_FAILED,
  ACTOR_RUN_ROOT_HEAP_EXHAUSTED,
  ACTOR_RUN_DEADLOCK
};

static const char *actor_run_outcome_name(enum actor_run_outcome outcome)
{
  switch (outcome) {
  case ACTOR_RUN_OK: return "ok";
  case ACTOR_RUN_UNSUPPORTED: return "unsupported_runtime";
  case ACTOR_RUN_ROOT_FAILED: return "root_failed";
  case ACTOR_RUN_ROOT_HEAP_EXHAUSTED: return "root_heap_exhausted";
  case ACTOR_RUN_DEADLOCK: return "deadlock";
  }
  return "unknown";
}

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
  CAMLparam1(field);
  CAMLlocal1(block);

  block = actor_try_alloc(1, tag);

  if (block != 0) Field(block, 0) = field;
  CAMLreturn(block);
}

static value actor_alloc_two(tag_t tag, value first, value second)
{
  CAMLparam2(first, second);
  CAMLlocal1(block);

  block = actor_try_alloc(2, tag);
  if (block != 0) {
    Field(block, 0) = first;
    Field(block, 1) = second;
  }
  CAMLreturn(block);
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
  CAMLparam0();
  CAMLlocal2(detail, error);

  switch (status) {
  case CAML_ACTOR_SPAWN_LIMIT:
    detail = Val_int(0); /* Actor_limit */
    break;
  case CAML_ACTOR_SPAWN_INITIAL_HEAP_LIMIT:
    detail = Val_int(1); /* Initial_heap_limit */
    break;
  default: {
    value message = actor_copy_string("unsupported closure capture");

    if (message == 0) CAMLreturn(0);
    detail = actor_alloc_one(0, message); /* Unsupported_capture */
    if (detail == 0) CAMLreturn(0);
    break;
  }
  }
  error = actor_alloc_one(1, detail); /* Error */
  CAMLreturn(error);
}

static value actor_send_error(int kind, const char *text)
{
  CAMLparam0();
  CAMLlocal1(detail);

  if (kind == 0) {
    detail = Val_int(0); /* No_such_actor */
  } else if (kind == 1) {
    detail = Val_int(1); /* Message_too_large */
  } else {
    value message = actor_copy_string(text);

    if (message == 0) CAMLreturn(0);
    detail = actor_alloc_one(0, message); /* Unsupported_message */
    if (detail == 0) CAMLreturn(0);
  }
  CAMLreturn(actor_alloc_one(1, detail)); /* Error */
}

static value actor_monitor_error(enum caml_actor_monitor_status status)
{
  value detail;

  if (status == CAML_ACTOR_MONITOR_STALE) {
    detail = Val_int(1); /* Monitor_stale */
  } else if (status == CAML_ACTOR_MONITOR_LIMIT) {
    detail = Val_int(2); /* Monitor_limit */
  } else {
    detail = Val_int(0); /* Monitor_missing */
  }

  return actor_alloc_one(1, detail); /* Error */
}

static value actor_cancel_error(enum caml_actor_monitor_status status)
{
  value detail;

  if (status == CAML_ACTOR_MONITOR_STALE) {
    detail = Val_int(1); /* Cancel_stale */
  } else if (status == CAML_ACTOR_MONITOR_SELF) {
    detail = Val_int(2); /* Cancel_self */
  } else {
    detail = Val_int(0); /* Cancel_missing */
  }
  return actor_alloc_one(1, detail); /* Error */
}

static value actor_exit_reason(
  const struct caml_actor_exit_reason *reason)
{
  CAMLparam0();
  CAMLlocal4(detail, text, backtrace, backtrace_option);
  char message[160];

  switch (reason->kind) {
  case CAML_ACTOR_EXIT_NORMAL:
    CAMLreturn(Val_int(0)); /* Normal */
  case CAML_ACTOR_EXIT_EXCEPTION:
    text = actor_copy_string(
      reason->summary[0] == '\0'
        ? "uncaught actor exception" : reason->summary);
    if (text == 0) CAMLreturn(0);
    backtrace_option = Val_int(0); /* None */
    if (reason->backtrace[0] != '\0') {
      backtrace = actor_copy_string(reason->backtrace);
      if (backtrace == 0) CAMLreturn(0);
      backtrace = actor_alloc_two(
        0, backtrace, Val_bool(reason->backtrace_truncated));
      if (backtrace == 0) CAMLreturn(0);
      backtrace_option = actor_alloc_one(0, backtrace); /* Some */
      if (backtrace_option == 0) CAMLreturn(0);
    }
    detail = actor_alloc_two(0, text, backtrace_option);
    CAMLreturn(detail); /* Uncaught_exception */
  case CAML_ACTOR_EXIT_HEAP_LIMIT:
    CAMLreturn(Val_int(1)); /* Heap_limit */
  case CAML_ACTOR_EXIT_MAILBOX_LIMIT:
    CAMLreturn(Val_int(2)); /* Mailbox_limit */
  case CAML_ACTOR_EXIT_CANCELLED:
    CAMLreturn(Val_int(3)); /* Cancelled */
  case CAML_ACTOR_EXIT_UNSUPPORTED:
    if (reason->unsupported.kind == CAML_ACTOR_UNSUPPORTED_OPCODE) {
      const char *name = reason->unsupported.operation
          < FIRST_UNIMPLEMENTED_OP
        ? names_of_instructions[reason->unsupported.operation]
        : "UNKNOWN";

      snprintf(message, sizeof(message),
               "unsupported operation at opcode %s", name);
    } else if (reason->unsupported.kind
                 == CAML_ACTOR_UNSUPPORTED_PRIMITIVE) {
      const char *name = reason->unsupported.operation
          < (uintnat)caml_prim_name_table.size
        ? caml_prim_name_table.contents[reason->unsupported.operation]
        : "unknown";

      snprintf(message, sizeof(message), "unsupported primitive %s/%d",
               name, reason->unsupported.arity);
    } else {
      snprintf(message, sizeof(message), "unsupported actor operation");
    }
    text = actor_copy_string(message);
    if (text == 0) CAMLreturn(0);
    CAMLreturn(actor_alloc_one(1, text)); /* Unsupported_operation */
  default:
    text = actor_copy_string("actor runtime failure");
    if (text == 0) CAMLreturn(0);
    CAMLreturn(actor_alloc_one(2, text)); /* Runtime_failure */
  }
}

static enum actor_run_outcome root_failure_outcome(
  const struct caml_actor_snapshot *snapshot,
  char *detail, size_t detail_size, const char **message)
{
  if (snapshot->failure == CAML_ACTOR_FAILURE_HEAP_EXHAUSTED) {
    return ACTOR_RUN_ROOT_HEAP_EXHAUSTED;
  }
  switch (snapshot->failure) {
  case CAML_ACTOR_FAILURE_EXCEPTION:
    *message = "uncaught root actor exception";
    break;
  case CAML_ACTOR_FAILURE_UNSUPPORTED:
    if (snapshot->unsupported.kind == CAML_ACTOR_UNSUPPORTED_OPCODE) {
      const char *name = snapshot->unsupported.operation
          < FIRST_UNIMPLEMENTED_OP
        ? names_of_instructions[snapshot->unsupported.operation]
        : "UNKNOWN";

      snprintf(detail, detail_size,
               "unsupported operation at opcode %s", name);
      *message = detail;
    } else if (snapshot->unsupported.kind
                 == CAML_ACTOR_UNSUPPORTED_PRIMITIVE) {
      const char *name = snapshot->unsupported.operation
          < (uintnat)caml_prim_name_table.size
        ? caml_prim_name_table.contents[snapshot->unsupported.operation]
        : "unknown";

      snprintf(detail, detail_size, "unsupported primitive %s/%d",
               name, snapshot->unsupported.arity);
      *message = detail;
    } else {
      *message = "unsupported operation in root actor";
    }
    break;
  case CAML_ACTOR_FAILURE_CANCELLED:
    *message = "root actor cancelled";
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

static value actor_run(value root,
                       mlsize_t root_initial_heap_words,
                       mlsize_t root_maximum_heap_words,
                       mlsize_t child_initial_heap_words,
                       mlsize_t child_maximum_heap_words,
                       uintnat actor_capacity,
                       uintnat reduction_budget,
                       mlsize_t message_quota_words,
                       uintnat mailbox_message_limit,
                       uintnat mailbox_byte_limit,
                       uintnat monitor_limit)
{
  CAMLparam1(root);
  CAMLlocal3(result, error, message_value);
  enum actor_run_outcome outcome = ACTOR_RUN_UNSUPPORTED;
  const char *message = "actor runtime unavailable";

#if !defined(NATIVE_CODE)
  char detail_message[256];
  struct caml_actor_scheduler *scheduler = NULL;
  struct caml_actor_prepared_spawn *prepared = NULL;
  enum caml_actor_spawn_status spawn_status;
  enum caml_actor_global_status global_status;
  enum caml_actor_world_status world_status;
  uintnat root_pid = 0;
  int frozen = 0;

  caml_load_actor_debug_info();
  world_status = caml_actor_world_freeze();
  if (world_status != CAML_ACTOR_WORLD_OK) goto finished;
  frozen = 1;
  global_status = caml_actor_world_prepare_global_image();
  if (global_status != CAML_ACTOR_GLOBAL_OK) {
    outcome = ACTOR_RUN_ROOT_FAILED;
    switch (global_status) {
    case CAML_ACTOR_GLOBAL_INVALID_IMAGE:
      message = "invalid actor global image";
      break;
    case CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE:
      message = "actor global image resources unavailable";
      break;
    default:
      message = "actor global image preparation unavailable";
      break;
    }
    goto cleanup;
  }
  scheduler = caml_actor_scheduler_create_configured(
    actor_capacity, reduction_budget,
    child_initial_heap_words, child_maximum_heap_words,
    message_quota_words, mailbox_message_limit, mailbox_byte_limit,
    monitor_limit);
  if (scheduler == NULL) goto cleanup;
  caml_actor_scheduler_trace_enable_from_environment(scheduler);

  spawn_status = caml_actor_scheduler_prepare_root_closure_sized(
    scheduler, root, root_initial_heap_words,
    root_maximum_heap_words, &prepared);
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
  caml_actor_scheduler_trace_flush(scheduler);

  for (;;) {
    struct caml_actor_step step = caml_actor_scheduler_step(scheduler);

    caml_actor_scheduler_trace_flush(scheduler);

    if (step.reason == CAML_ACTOR_STEP_REDUCTIONS
        || step.reason == CAML_ACTOR_STEP_YIELD
        || step.reason == CAML_ACTOR_STEP_BLOCKED) {
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
            outcome = root_failure_outcome(
              &snapshot, detail_message, sizeof(detail_message), &message);
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
  if (scheduler != NULL) {
    caml_actor_scheduler_trace_finish(
      scheduler, actor_run_outcome_name(outcome));
    caml_actor_scheduler_destroy(scheduler);
  }
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

CAMLprim value caml_actor_run(value root)
{
  CAMLparam1(root);
  CAMLlocal1(entry);
  mlsize_t root_initial = ACTOR_MVP_ROOT_HEAP_WORDS;
  mlsize_t root_maximum = ACTOR_MVP_ROOT_HEAP_WORDS;
  mlsize_t child_initial = ACTOR_MVP_CHILD_HEAP_WORDS;
  mlsize_t child_maximum = ACTOR_MVP_CHILD_HEAP_WORDS;
  uintnat actor_capacity = ACTOR_MVP_CAPACITY;
  uintnat reduction_budget = ACTOR_MVP_REDUCTIONS;
  mlsize_t message_quota_words = ACTOR_MVP_MESSAGE_WORDS;
  uintnat mailbox_message_limit = ACTOR_MVP_MAILBOX_MESSAGES;
  uintnat mailbox_byte_limit = ACTOR_MVP_MAILBOX_BYTES;
  uintnat monitor_limit = ACTOR_MVP_MONITORS;

  entry = root;
  if (Is_block(root) && Tag_val(root) == 0 && Wosize_val(root) == 1) {
    entry = Field(root, 0);
  } else if (Is_block(root)
             && Tag_val(root) == 0 && Wosize_val(root) == 5) {
    value root_initial_value = Field(root, 0);
    value root_maximum_value = Field(root, 1);
    value child_initial_value = Field(root, 2);
    value child_maximum_value = Field(root, 3);

    if (!Is_long(root_initial_value) || Long_val(root_initial_value) <= 0
        || !Is_long(root_maximum_value)
        || Long_val(root_maximum_value) < Long_val(root_initial_value)
        || !Is_long(child_initial_value) || Long_val(child_initial_value) <= 0
        || !Is_long(child_maximum_value)
        || Long_val(child_maximum_value) < Long_val(child_initial_value)) {
      caml_invalid_argument("Actor.run_with_heap_limits");
    }
    root_initial = Long_val(root_initial_value);
    root_maximum = Long_val(root_maximum_value);
    child_initial = Long_val(child_initial_value);
    child_maximum = Long_val(child_maximum_value);
    entry = Field(root, 4);
  } else if (Is_block(root)
             && Tag_val(root) == 0 && Wosize_val(root) == 11) {
    value root_initial_value = Field(root, 0);
    value root_maximum_value = Field(root, 1);
    value child_initial_value = Field(root, 2);
    value child_maximum_value = Field(root, 3);
    value actor_capacity_value = Field(root, 4);
    value reduction_budget_value = Field(root, 5);
    value message_quota_value = Field(root, 6);
    value mailbox_message_limit_value = Field(root, 7);
    value mailbox_byte_limit_value = Field(root, 8);
    value monitor_limit_value = Field(root, 9);

    if (!Is_long(root_initial_value) || Long_val(root_initial_value) <= 0
        || !Is_long(root_maximum_value)
        || Long_val(root_maximum_value) < Long_val(root_initial_value)
        || !Is_long(child_initial_value) || Long_val(child_initial_value) <= 0
        || !Is_long(child_maximum_value)
        || Long_val(child_maximum_value) < Long_val(child_initial_value)
        || !Is_long(actor_capacity_value)
        || Long_val(actor_capacity_value) < 2
        || (uintnat)Long_val(actor_capacity_value)
             > CAML_ACTOR_PID_INDEX_MASK + 1
        || !Is_long(reduction_budget_value)
        || Long_val(reduction_budget_value) <= 0
        || !Is_long(message_quota_value)
        || Long_val(message_quota_value) <= 0
        || !Is_long(mailbox_message_limit_value)
        || Long_val(mailbox_message_limit_value) <= 0
        || !Is_long(mailbox_byte_limit_value)
        || Long_val(mailbox_byte_limit_value) <= 0
        || !Is_long(monitor_limit_value)
        || Long_val(monitor_limit_value) <= 0) {
      caml_invalid_argument("Actor.run_with_config");
    }
    root_initial = Long_val(root_initial_value);
    root_maximum = Long_val(root_maximum_value);
    child_initial = Long_val(child_initial_value);
    child_maximum = Long_val(child_maximum_value);
    actor_capacity = Long_val(actor_capacity_value);
    reduction_budget = Long_val(reduction_budget_value);
    message_quota_words = Long_val(message_quota_value);
    mailbox_message_limit = Long_val(mailbox_message_limit_value);
    mailbox_byte_limit = Long_val(mailbox_byte_limit_value);
    monitor_limit = Long_val(monitor_limit_value);
    entry = Field(root, 10);
  }
  CAMLreturn(actor_run(
    entry, root_initial, root_maximum, child_initial, child_maximum,
    actor_capacity, reduction_budget, message_quota_words,
    mailbox_message_limit, mailbox_byte_limit, monitor_limit));
}

static value actor_spawn(value closure, mlsize_t initial_heap_words,
                         mlsize_t maximum_heap_words, int use_default)
{
#if defined(NATIVE_CODE)
  (void)closure;
  (void)initial_heap_words;
  (void)maximum_heap_words;
  (void)use_default;
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
  status = use_default
    ? caml_actor_scheduler_prepare_closure_default(
        scheduler, closure, &prepared)
    : caml_actor_scheduler_prepare_closure_sized(
        scheduler, closure, initial_heap_words,
        maximum_heap_words, &prepared);
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

static value actor_monitor(value target_value)
{
#if defined(NATIVE_CODE)
  (void)target_value;
  caml_invalid_argument("Actor.monitor outside an actor world");
#else
  struct caml_actor_scheduler *scheduler = Caml_state->actor_scheduler;
  enum caml_actor_monitor_status status;
  uintnat monitor_id;
  value token;
  value result;

  if (!Is_long(target_value) || Long_val(target_value) < 0) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  status = caml_actor_scheduler_monitor(
    scheduler, (uintnat)Long_val(target_value), &monitor_id);
  if (status == CAML_ACTOR_MONITOR_MISSING
      || status == CAML_ACTOR_MONITOR_STALE
      || status == CAML_ACTOR_MONITOR_LIMIT) {
    return actor_monitor_error(status);
  }
  if (status != CAML_ACTOR_MONITOR_OK) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  token = actor_alloc_two(0, Val_int(0), Val_long(monitor_id));
  if (token == 0) {
    caml_actor_scheduler_discard_monitor(scheduler, monitor_id);
    return Val_unit;
  }
  result = actor_alloc_one(0, token); /* Ok */
  if (result == 0) {
    caml_actor_scheduler_discard_monitor(scheduler, monitor_id);
    return Val_unit;
  }
  return result;
#endif
}

static value actor_cancel(value target_value)
{
#if defined(NATIVE_CODE)
  (void)target_value;
  caml_invalid_argument("Actor.cancel outside an actor world");
#else
  struct caml_actor_scheduler *scheduler = Caml_state->actor_scheduler;
  enum caml_actor_monitor_status status;

  if (!Is_long(target_value) || Long_val(target_value) < 0) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  status = caml_actor_scheduler_cancel(
    scheduler, (uintnat)Long_val(target_value));
  if (status == CAML_ACTOR_MONITOR_MISSING
      || status == CAML_ACTOR_MONITOR_STALE
      || status == CAML_ACTOR_MONITOR_SELF) {
    return actor_cancel_error(status);
  }
  if (status != CAML_ACTOR_MONITOR_OK) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  return actor_alloc_one(0, Val_unit); /* Ok */
#endif
}

CAMLprim value caml_actor_spawn(value request)
{
#if defined(NATIVE_CODE)
  if (Is_block(request) && Tag_val(request) == 0
      && Wosize_val(request) == 2 && Is_long(Field(request, 0))) {
    if (Long_val(Field(request, 0)) == 0) {
      caml_invalid_argument("Actor.monitor outside an actor world");
    }
    if (Long_val(Field(request, 0)) == 1) {
      caml_invalid_argument("Actor.cancel outside an actor world");
    }
  }
  return actor_spawn(request, 0, 0, 1);
#else
  struct caml_actor_heap *heap;
  value closure = request;
  value initial = Val_unit;
  value maximum = Val_unit;
  value operation = Val_unit;
  int use_default = 1;

  if (!caml_actor_scheduler_is_running()) {
    if (Is_block(request) && Tag_val(request) == 0
        && Wosize_val(request) == 2 && Is_long(Field(request, 0))) {
      if (Long_val(Field(request, 0)) == 0) {
        caml_invalid_argument("Actor.monitor outside an actor world");
      }
      if (Long_val(Field(request, 0)) == 1) {
        caml_invalid_argument("Actor.cancel outside an actor world");
      }
    }
    caml_invalid_argument("Actor.spawn outside an actor world");
  }
  heap = caml_actor_heap_current();
  if (caml_actor_heap_owns_value(heap, request)
      && Tag_val(request) == 0 && Wosize_val(request) == 2) {
    if (!caml_actor_heap_read_field(request, 0, &operation)
        || !caml_actor_heap_read_field(request, 1, &closure)
        || !Is_long(operation)) {
      caml_actor_scheduler_request_unsupported();
      return Val_unit;
    }
    if (Long_val(operation) == 0) return actor_monitor(closure);
    if (Long_val(operation) == 1) return actor_cancel(closure);
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  } else if (caml_actor_heap_owns_value(heap, request)
      && Tag_val(request) == 0 && Wosize_val(request) == 1) {
    if (!caml_actor_heap_read_field(request, 0, &closure)) {
      caml_actor_scheduler_request_unsupported();
      return Val_unit;
    }
  } else if (caml_actor_heap_owns_value(heap, request)
             && Tag_val(request) == 0 && Wosize_val(request) == 3) {
    if (!caml_actor_heap_read_field(request, 0, &initial)
        || !caml_actor_heap_read_field(request, 1, &maximum)
        || !caml_actor_heap_read_field(request, 2, &closure)) {
      caml_actor_scheduler_request_unsupported();
      return Val_unit;
    }
    use_default = 0;
  }
  if (use_default) return actor_spawn(closure, 0, 0, 1);
  if (!Is_long(initial) || Long_val(initial) <= 0
      || !Is_long(maximum) || Long_val(maximum) < Long_val(initial)) {
    return actor_spawn_error(CAML_ACTOR_SPAWN_INITIAL_HEAP_LIMIT);
  }
  return actor_spawn(
    closure, Long_val(initial), Long_val(maximum), 0);
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

CAMLprim value caml_actor_stats(value unit)
{
#if !defined(NATIVE_CODE)
  struct caml_actor_scheduler_stats stats;
  value record;

  if (!caml_actor_scheduler_stats(Caml_state->actor_scheduler, &stats)) {
    caml_invalid_argument("Actor.stats outside an actor world");
  }
  record = actor_try_alloc(26, 0);
  if (record == 0) return Val_unit;
#define Store_stat(field, value) \
  Field(record, (field)) = Val_long( \
    (value) > (uintnat)Max_long ? Max_long : (intnat)(value))
  Store_stat(0, stats.live_actors);
  Store_stat(1, stats.runnable_actors);
  Store_stat(2, stats.blocked_actors);
  Store_stat(3, stats.total_spawned);
  Store_stat(4, stats.total_exited);
  Store_stat(5, stats.total_failed);
  Store_stat(6, stats.total_dispatches);
  Store_stat(7, stats.total_reduction_stops);
  Store_stat(8, stats.messages_sent);
  Store_stat(9, stats.messages_received);
  Store_stat(10, stats.messages_dropped);
  Store_stat(11, stats.mailbox_messages);
  Store_stat(12, stats.mailbox_bytes);
  Store_stat(13, stats.mailbox_quota_failures);
  Store_stat(14, stats.current_heap_words);
  Store_stat(15, stats.maximum_heap_words);
  Store_stat(16, stats.heap_growths);
  Store_stat(17, stats.actor_capacity);
  Store_stat(18, stats.reduction_budget);
  Store_stat(19, stats.message_word_limit);
  Store_stat(20, stats.mailbox_message_limit);
  Store_stat(21, stats.mailbox_byte_limit);
  Store_stat(22, stats.monitors);
  Store_stat(23, stats.peak_monitors);
  Store_stat(24, stats.monitor_quota_failures);
  Store_stat(25, stats.monitor_limit);
#undef Store_stat
  return record;
#else
  (void)unit;
  caml_invalid_argument("Actor.stats outside an actor world");
#endif
}

CAMLprim value caml_actor_send(value pid_value, value message)
{
#if defined(NATIVE_CODE)
  (void)pid_value;
  (void)message;
  caml_invalid_argument("Actor.send outside an actor world");
#else
  struct caml_actor_scheduler *scheduler;
  struct caml_actor_prepared_send *prepared = NULL;
  struct caml_actor_wire_encode_result encoded;
  enum caml_actor_send_status send_status;
  mlsize_t message_quota_words;
  uintnat pid;
  value result;

  if (!caml_actor_scheduler_is_running()) {
    caml_invalid_argument("Actor.send outside an actor world");
  }
  if (!Is_long(pid_value)) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  scheduler = Caml_state->actor_scheduler;
  pid = (uintnat)Long_val(pid_value);
  send_status = caml_actor_scheduler_can_send(scheduler, pid);
  if (send_status == CAML_ACTOR_SEND_NO_SUCH_ACTOR) {
    caml_actor_scheduler_trace_send_rejected(
      scheduler, pid, CAML_ACTOR_TRACE_REJECT_MISSING);
    return actor_send_error(0, NULL);
  }
  if (send_status == CAML_ACTOR_SEND_QUOTA) {
    caml_actor_scheduler_record_mailbox_quota_failure(scheduler);
    caml_actor_scheduler_trace_send_rejected(
      scheduler, pid, CAML_ACTOR_TRACE_REJECT_QUOTA);
    return actor_send_error(1, NULL);
  }
  if (send_status != CAML_ACTOR_SEND_OK) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }

  if (!caml_actor_scheduler_message_quota_words(
        scheduler, &message_quota_words)) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }

  encoded = caml_actor_wire_encode(message, message_quota_words);
  if (encoded.status != CAML_ACTOR_WIRE_ENCODE_OK) {
    if (encoded.status == CAML_ACTOR_WIRE_ENCODE_TOO_LARGE) {
      caml_actor_scheduler_record_mailbox_quota_failure(scheduler);
      caml_actor_scheduler_trace_send_rejected(
        scheduler, pid, CAML_ACTOR_TRACE_REJECT_QUOTA);
      return actor_send_error(1, NULL);
    }
    if (encoded.status == CAML_ACTOR_WIRE_ENCODE_INVALID_SOURCE
        || encoded.status == CAML_ACTOR_WIRE_ENCODE_UNSUPPORTED_VALUE) {
      caml_actor_scheduler_trace_send_rejected(
        scheduler, pid, CAML_ACTOR_TRACE_REJECT_UNSUPPORTED);
      return actor_send_error(2, "unsupported message value");
    }
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }

  send_status = caml_actor_scheduler_prepare_send(
    scheduler, pid, encoded.envelope, &prepared);
  if (send_status != CAML_ACTOR_SEND_OK) {
    caml_actor_wire_destroy(encoded.envelope);
    if (send_status == CAML_ACTOR_SEND_NO_SUCH_ACTOR) {
      caml_actor_scheduler_trace_send_rejected(
        scheduler, pid, CAML_ACTOR_TRACE_REJECT_MISSING);
      return actor_send_error(0, NULL);
    }
    if (send_status == CAML_ACTOR_SEND_QUOTA) {
      caml_actor_scheduler_record_mailbox_quota_failure(scheduler);
      caml_actor_scheduler_trace_send_rejected(
        scheduler, pid, CAML_ACTOR_TRACE_REJECT_QUOTA);
      return actor_send_error(1, NULL);
    }
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  result = actor_alloc_one(0, Val_unit); /* Ok */
  if (result == 0) {
    caml_actor_scheduler_abort_send(prepared);
    return Val_unit;
  }
  if (!caml_actor_scheduler_commit_send(prepared)) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  return result;
#endif
}

#if !defined(NATIVE_CODE)
static value actor_await_exit(value monitor)
{
  struct caml_actor_scheduler *scheduler = Caml_state->actor_scheduler;
  struct caml_actor_exit_reason reason;
  enum caml_actor_monitor_status status;
  struct caml_actor_heap *heap = caml_actor_heap_current();
  value operation = Val_unit;
  value id_value = Val_unit;
  uintnat monitor_id;
  value result;

  if (!caml_actor_heap_owns_value(heap, monitor)
      || Tag_val(monitor) != 0 || Wosize_val(monitor) != 2
      || !caml_actor_heap_read_field(monitor, 0, &operation)
      || !caml_actor_heap_read_field(monitor, 1, &id_value)
      || !Is_long(operation) || Long_val(operation) != 0
      || !Is_long(id_value) || Long_val(id_value) <= 0) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  monitor_id = Long_val(id_value);
  status = caml_actor_scheduler_peek_exit(scheduler, monitor_id, &reason);
  if (status == CAML_ACTOR_MONITOR_PENDING) {
    if (!caml_actor_scheduler_request_blocked()) {
      caml_actor_scheduler_request_unsupported();
      return Val_unit;
    }
    return monitor;
  }
  if (status != CAML_ACTOR_MONITOR_READY) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  result = actor_exit_reason(&reason);
  if (result == 0) return Val_unit;
  if (!caml_actor_scheduler_consume_exit(scheduler, monitor_id)) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  return result;
}
#endif

CAMLprim value caml_actor_receive(value inbox)
{
#if defined(NATIVE_CODE)
  if (Is_block(inbox) && Tag_val(inbox) == 0 && Wosize_val(inbox) == 2
      && Is_long(Field(inbox, 0)) && Long_val(Field(inbox, 0)) == 0) {
    caml_invalid_argument("Actor.await_exit outside an actor world");
  }
  caml_invalid_argument("Actor.receive outside an actor world");
#else
  struct caml_actor_scheduler *scheduler;
  const struct caml_actor_envelope *envelope;
  enum caml_actor_wire_decode_status status;
  struct caml_actor_heap *heap;
  uintnat pid;
  value message = Val_unit;

  if (!caml_actor_scheduler_is_running()) {
    if (Is_block(inbox) && Tag_val(inbox) == 0 && Wosize_val(inbox) == 2
        && Is_long(Field(inbox, 0)) && Long_val(Field(inbox, 0)) == 0) {
      caml_invalid_argument("Actor.await_exit outside an actor world");
    }
    caml_invalid_argument("Actor.receive outside an actor world");
  }
  scheduler = Caml_state->actor_scheduler;
  pid = caml_actor_scheduler_current_pid();
  if (Is_block(inbox)) return actor_await_exit(inbox);
  if (!Is_long(inbox) || (uintnat)Long_val(inbox) != pid) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  envelope = caml_actor_scheduler_peek_current_message(scheduler);
  if (envelope == NULL) {
    if (!caml_actor_scheduler_request_blocked()) {
      caml_actor_scheduler_request_unsupported();
      return Val_unit;
    }
    return inbox;
  }

  heap = caml_actor_heap_current();
  status = caml_actor_wire_decode(envelope, heap, &message);
  if (status == CAML_ACTOR_WIRE_DECODE_HEAP_EXHAUSTED) {
    caml_actor_scheduler_request_heap_exhausted();
    return Val_unit;
  }
  if (status != CAML_ACTOR_WIRE_DECODE_OK
      || !caml_actor_scheduler_consume_current_message(scheduler)) {
    caml_actor_scheduler_request_unsupported();
    return Val_unit;
  }
  return message;
#endif
}
