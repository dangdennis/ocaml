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
struct caml_actor_prepared_send;
struct caml_actor_envelope;

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
  CAML_ACTOR_LIFECYCLE_BLOCKED,
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

enum caml_actor_unsupported_kind {
  CAML_ACTOR_UNSUPPORTED_NONE = 0,
  CAML_ACTOR_UNSUPPORTED_OPCODE,
  CAML_ACTOR_UNSUPPORTED_PRIMITIVE
};

struct caml_actor_unsupported {
  enum caml_actor_unsupported_kind kind;
  uintnat operation;
  int arity;
};

enum caml_actor_step_reason {
  CAML_ACTOR_STEP_IDLE = 0,
  CAML_ACTOR_STEP_REDUCTIONS,
  CAML_ACTOR_STEP_YIELD,
  CAML_ACTOR_STEP_BLOCKED,
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
  CAML_ACTOR_CONTROL_BLOCKED,
  CAML_ACTOR_CONTROL_UNSUPPORTED,
  CAML_ACTOR_CONTROL_HEAP_EXHAUSTED
};

enum caml_actor_send_status {
  CAML_ACTOR_SEND_OK = 0,
  CAML_ACTOR_SEND_NO_SUCH_ACTOR,
  CAML_ACTOR_SEND_QUOTA,
  CAML_ACTOR_SEND_RESOURCE_UNAVAILABLE,
  CAML_ACTOR_SEND_INVALID_CONTEXT
};

enum caml_actor_monitor_status {
  CAML_ACTOR_MONITOR_OK = 0,
  CAML_ACTOR_MONITOR_MISSING,
  CAML_ACTOR_MONITOR_STALE,
  CAML_ACTOR_MONITOR_PENDING,
  CAML_ACTOR_MONITOR_READY,
  CAML_ACTOR_MONITOR_RESOURCE_UNAVAILABLE,
  CAML_ACTOR_MONITOR_INVALID_CONTEXT
};

enum caml_actor_exit_kind {
  CAML_ACTOR_EXIT_NORMAL = 0,
  CAML_ACTOR_EXIT_EXCEPTION,
  CAML_ACTOR_EXIT_HEAP_LIMIT,
  CAML_ACTOR_EXIT_MAILBOX_LIMIT,
  CAML_ACTOR_EXIT_CANCELLED,
  CAML_ACTOR_EXIT_UNSUPPORTED,
  CAML_ACTOR_EXIT_RUNTIME_FAILURE
};

struct caml_actor_exit_reason {
  enum caml_actor_exit_kind kind;
  struct caml_actor_unsupported unsupported;
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
  struct caml_actor_unsupported unsupported;
};

struct caml_actor_scheduler_stats {
  uintnat live_actors;
  uintnat runnable_actors;
  uintnat blocked_actors;
  uintnat total_spawned;
  uintnat total_exited;
  uintnat total_failed;
  uintnat total_dispatches;
  uintnat total_reduction_stops;
  uintnat messages_sent;
  uintnat messages_received;
  uintnat messages_dropped;
  uintnat mailbox_messages;
  uintnat mailbox_bytes;
  uintnat mailbox_quota_failures;
  uintnat current_heap_words;
  uintnat maximum_heap_words;
  uintnat heap_growths;
  uintnat actor_capacity;
  uintnat reduction_budget;
  uintnat message_word_limit;
  uintnat mailbox_message_limit;
  uintnat mailbox_byte_limit;
};

CAMLextern struct caml_actor_scheduler *caml_actor_scheduler_create(
  uintnat capacity, uintnat reduction_budget);
CAMLextern struct caml_actor_scheduler *caml_actor_scheduler_create_configured(
  uintnat capacity, uintnat reduction_budget,
  mlsize_t child_initial_heap_words, mlsize_t child_maximum_heap_words,
  mlsize_t message_quota_words, uintnat mailbox_message_limit,
  uintnat mailbox_byte_limit);
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
caml_actor_scheduler_prepare_root_closure_sized(
  struct caml_actor_scheduler *scheduler, value closure,
  mlsize_t initial_heap_words, mlsize_t maximum_heap_words,
  struct caml_actor_prepared_spawn **prepared);
CAMLextern enum caml_actor_spawn_status
caml_actor_scheduler_prepare_closure(
  struct caml_actor_scheduler *scheduler, value closure,
  mlsize_t heap_quota_words, struct caml_actor_prepared_spawn **prepared);
CAMLextern enum caml_actor_spawn_status
caml_actor_scheduler_prepare_closure_sized(
  struct caml_actor_scheduler *scheduler, value closure,
  mlsize_t initial_heap_words, mlsize_t maximum_heap_words,
  struct caml_actor_prepared_spawn **prepared);
CAMLextern enum caml_actor_spawn_status
caml_actor_scheduler_prepare_closure_default(
  struct caml_actor_scheduler *scheduler, value closure,
  struct caml_actor_prepared_spawn **prepared);
CAMLextern uintnat caml_actor_scheduler_prepared_pid(
  const struct caml_actor_prepared_spawn *prepared);
CAMLextern int caml_actor_scheduler_commit_prepared(
  struct caml_actor_prepared_spawn *prepared);
CAMLextern void caml_actor_scheduler_abort_prepared(
  struct caml_actor_prepared_spawn *prepared);

/* Mailbox publication is transactional.  A prepared send owns its envelope
   but is not linked into the destination mailbox until commit. */
CAMLextern enum caml_actor_send_status caml_actor_scheduler_can_send(
  struct caml_actor_scheduler *scheduler, uintnat pid);
CAMLextern int caml_actor_scheduler_message_quota_words(
  struct caml_actor_scheduler *scheduler, mlsize_t *quota_words);
CAMLextern int caml_actor_scheduler_record_mailbox_quota_failure(
  struct caml_actor_scheduler *scheduler);
CAMLextern enum caml_actor_send_status caml_actor_scheduler_prepare_send(
  struct caml_actor_scheduler *scheduler, uintnat pid,
  struct caml_actor_envelope *envelope,
  struct caml_actor_prepared_send **prepared);
CAMLextern int caml_actor_scheduler_commit_send(
  struct caml_actor_prepared_send *prepared);
CAMLextern void caml_actor_scheduler_abort_send(
  struct caml_actor_prepared_send *prepared);
CAMLextern const struct caml_actor_envelope *
caml_actor_scheduler_peek_current_message(
  struct caml_actor_scheduler *scheduler);
CAMLextern int caml_actor_scheduler_consume_current_message(
  struct caml_actor_scheduler *scheduler);

/* Monitor records and exit reasons are scheduler-owned and pointer-free.
   Public values are constructed only by the observing actor. */
CAMLextern enum caml_actor_monitor_status caml_actor_scheduler_monitor(
  struct caml_actor_scheduler *scheduler, uintnat target_pid,
  uintnat *monitor_id);
CAMLextern enum caml_actor_monitor_status caml_actor_scheduler_peek_exit(
  struct caml_actor_scheduler *scheduler, uintnat monitor_id,
  struct caml_actor_exit_reason *reason);
CAMLextern int caml_actor_scheduler_consume_exit(
  struct caml_actor_scheduler *scheduler, uintnat monitor_id);
CAMLextern int caml_actor_scheduler_discard_monitor(
  struct caml_actor_scheduler *scheduler, uintnat monitor_id);

CAMLextern struct caml_actor_step caml_actor_scheduler_step(
  struct caml_actor_scheduler *scheduler);
CAMLextern enum caml_actor_pid_lookup caml_actor_scheduler_snapshot(
  const struct caml_actor_scheduler *scheduler, uintnat pid,
  struct caml_actor_snapshot *snapshot);
CAMLextern int caml_actor_scheduler_stats(
  const struct caml_actor_scheduler *scheduler,
  struct caml_actor_scheduler_stats *stats);
CAMLextern int caml_actor_scheduler_retire(
  struct caml_actor_scheduler *scheduler, uintnat pid);

/* Deterministic test-only injection at the next installed-actor boundary. */
CAMLextern void caml_actor_scheduler_test_request_minor_gc_after_switch(
  struct caml_actor_scheduler *scheduler);

/* Interpreter hooks. */
CAMLextern int caml_actor_scheduler_is_running(void);
CAMLextern uintnat caml_actor_scheduler_current_pid(void);
CAMLextern int caml_actor_scheduler_request_yield(void);
CAMLextern int caml_actor_scheduler_request_blocked(void);
CAMLextern int caml_actor_scheduler_request_unsupported(void);
CAMLextern int caml_actor_scheduler_request_heap_exhausted(void);
CAMLextern void caml_actor_scheduler_record_unsupported_opcode(
  uintnat opcode);
CAMLextern void caml_actor_scheduler_record_unsupported_primitive(
  uintnat primitive, int arity);
CAMLextern enum caml_actor_control_request
caml_actor_scheduler_take_control_request(void);
CAMLextern int caml_actor_scheduler_primitive_allowed(
  uintnat primitive, int arity);

#endif /* CAML_INTERNALS */

#endif /* CAML_ACTOR_SCHEDULER_H */
