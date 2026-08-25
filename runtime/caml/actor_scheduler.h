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

/* Internal deterministic actor scheduler. */

#ifndef CAML_ACTOR_SCHEDULER_H
#define CAML_ACTOR_SCHEDULER_H

#ifdef CAML_INTERNALS

#include "mlvalues.h"

struct caml_actor_scheduler;
struct caml_actor_prepared_spawn;

#define CAML_ACTOR_PID_INDEX_BITS 16
#define CAML_ACTOR_PID_INDEX_MASK \
  ((((uintnat)1) << CAML_ACTOR_PID_INDEX_BITS) - 1)

enum caml_actor_spawn_status {
  CAML_ACTOR_SPAWN_OK = 0,
  CAML_ACTOR_SPAWN_UNSUPPORTED,
  CAML_ACTOR_SPAWN_LIMIT,
  CAML_ACTOR_SPAWN_HEAP_UNAVAILABLE,
  CAML_ACTOR_SPAWN_STACK_UNAVAILABLE,
  CAML_ACTOR_SPAWN_INITIAL_HEAP_LIMIT,
  CAML_ACTOR_SPAWN_UNSUPPORTED_CAPTURE
};

enum caml_actor_lifecycle {
  CAML_ACTOR_LIFECYCLE_FREE = 0,
  CAML_ACTOR_LIFECYCLE_RUNNABLE,
  CAML_ACTOR_LIFECYCLE_RUNNING,
  CAML_ACTOR_LIFECYCLE_EXITED,
  CAML_ACTOR_LIFECYCLE_FAILED,
  CAML_ACTOR_LIFECYCLE_RETIRED
};

enum caml_actor_failure {
  CAML_ACTOR_FAILURE_NONE = 0,
  CAML_ACTOR_FAILURE_EXCEPTION,
  CAML_ACTOR_FAILURE_UNSUPPORTED,
  CAML_ACTOR_FAILURE_HEAP_EXHAUSTED,
  CAML_ACTOR_FAILURE_INVALID_HEAP,
  CAML_ACTOR_FAILURE_INVALID_RESULT,
  CAML_ACTOR_FAILURE_INTERNAL
};

enum caml_actor_step_reason {
  CAML_ACTOR_STEP_IDLE = 0,
  CAML_ACTOR_STEP_REDUCTIONS,
  CAML_ACTOR_STEP_YIELD,
  CAML_ACTOR_STEP_HOST_ACTION,
  CAML_ACTOR_STEP_EXITED,
  CAML_ACTOR_STEP_FAILED
};

enum caml_actor_pid_lookup {
  CAML_ACTOR_PID_MISSING = 0,
  CAML_ACTOR_PID_STALE,
  CAML_ACTOR_PID_PRESENT
};

enum caml_actor_control_request {
  CAML_ACTOR_CONTROL_NONE = 0,
  CAML_ACTOR_CONTROL_YIELD,
  CAML_ACTOR_CONTROL_UNSUPPORTED,
  CAML_ACTOR_CONTROL_HEAP_EXHAUSTED
};

struct caml_actor_step {
  enum caml_actor_step_reason reason;
  uintnat pid;
};

struct caml_actor_snapshot {
  enum caml_actor_lifecycle lifecycle;
  enum caml_actor_failure failure;
  uintnat pid;
  uintnat dispatches;
  uintnat reduction_stops;
};

CAMLextern struct caml_actor_scheduler *caml_actor_scheduler_create(
  uintnat capacity, uintnat reduction_budget);
CAMLextern void caml_actor_scheduler_destroy(
  struct caml_actor_scheduler *scheduler);

CAMLextern enum caml_actor_spawn_status caml_actor_scheduler_spawn_root_code(
  struct caml_actor_scheduler *scheduler,
  code_t code, asize_t code_size,
  value initial_env, intnat initial_extra_args,
  mlsize_t heap_quota_words, uintnat *pid);
CAMLextern enum caml_actor_spawn_status caml_actor_scheduler_spawn_code(
  struct caml_actor_scheduler *scheduler,
  code_t code, asize_t code_size,
  value initial_env, intnat initial_extra_args,
  mlsize_t heap_quota_words, uintnat *pid);

/* Public closure entry is prepared transactionally.  A prepared actor owns
   its heap and stack but is not visible through the PID table or ready queue
   until commit. */
CAMLextern enum caml_actor_spawn_status
caml_actor_scheduler_prepare_root_closure(
  struct caml_actor_scheduler *scheduler, value closure,
  mlsize_t heap_quota_words, struct caml_actor_prepared_spawn **prepared);
CAMLextern enum caml_actor_spawn_status
caml_actor_scheduler_prepare_closure(
  struct caml_actor_scheduler *scheduler, value closure,
  mlsize_t heap_quota_words, struct caml_actor_prepared_spawn **prepared);
CAMLextern uintnat caml_actor_scheduler_prepared_pid(
  const struct caml_actor_prepared_spawn *prepared);
CAMLextern int caml_actor_scheduler_commit_prepared(
  struct caml_actor_prepared_spawn *prepared);
CAMLextern void caml_actor_scheduler_abort_prepared(
  struct caml_actor_prepared_spawn *prepared);

CAMLextern struct caml_actor_step caml_actor_scheduler_step(
  struct caml_actor_scheduler *scheduler);
CAMLextern enum caml_actor_pid_lookup caml_actor_scheduler_snapshot(
  const struct caml_actor_scheduler *scheduler, uintnat pid,
  struct caml_actor_snapshot *snapshot);
CAMLextern int caml_actor_scheduler_retire(
  struct caml_actor_scheduler *scheduler, uintnat pid);

/* Deterministic test-only injection at the next installed-actor boundary. */
CAMLextern void caml_actor_scheduler_test_request_minor_gc_after_switch(
  struct caml_actor_scheduler *scheduler);

/* Interpreter hooks. */
CAMLextern int caml_actor_scheduler_is_running(void);
CAMLextern uintnat caml_actor_scheduler_current_pid(void);
CAMLextern int caml_actor_scheduler_request_yield(void);
CAMLextern int caml_actor_scheduler_request_unsupported(void);
CAMLextern int caml_actor_scheduler_request_heap_exhausted(void);
CAMLextern enum caml_actor_control_request
caml_actor_scheduler_take_control_request(void);
CAMLextern int caml_actor_scheduler_primitive_allowed(
  uintnat primitive, int arity);

#endif /* CAML_INTERNALS */

#endif /* CAML_ACTOR_SCHEDULER_H */
