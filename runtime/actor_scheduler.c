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

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "caml/actor_heap.h"
#include "caml/actor_scheduler.h"
#include "caml/actor_world.h"
#include "caml/codefrag.h"
#include "caml/debugger.h"
#include "caml/domain.h"
#include "caml/domain_state.h"
#include "caml/fiber.h"
#include "caml/gc_ctrl.h"
#include "caml/interp.h"
#include "caml/misc.h"
#include "caml/prims.h"
#include "caml/signals.h"

#define ACTOR_SLOT_NONE UINT32_MAX

#if !defined(NATIVE_CODE)

CAMLextern value caml_int_compare(value left, value right);

struct caml_actor_slot {
  enum caml_actor_lifecycle lifecycle;
  enum caml_actor_failure failure;
  uintnat generation;
  uintnat pid;
  uintnat dispatches;
  uintnat reduction_stops;
  uint32_t ready_next;
  int queued;
  struct stack_info *stack;
  struct caml_actor_heap *heap;
  struct caml_bytecode_state bytecode;
};

struct caml_actor_scheduler {
  caml_domain_state *domain;
  int domain_unique_id;
  uintnat capacity;
  uintnat reduction_budget;
  struct caml_actor_slot *slots;
  uint32_t ready_head;
  uint32_t ready_tail;
  uint32_t current;
  int root_published;
  int test_request_minor_gc_after_switch;

  struct stack_info *host_stack;
  int64_t host_stack_id;
  struct caml_exception_context *host_external_raise;
  struct caml__roots_block *host_local_roots;
  intnat host_trap_sp_off;
  intnat host_trap_barrier_off;
  int64_t host_trap_barrier_block;
  intnat host_backtrace_active;
};

static int scheduler_runtime_supported(void)
{
#if !defined(__linux__) || !defined(__x86_64__)
  return 0;
#else
  return 1;
#endif
}

static uintnat max_pid_generation(void)
{
  return ((uintnat)Max_long) >> CAML_ACTOR_PID_INDEX_BITS;
}

static uintnat make_pid(uintnat generation, uint32_t index)
{
  return (generation << CAML_ACTOR_PID_INDEX_BITS) | index;
}

static int approved_initial_environment(value environment)
{
  if (Is_long(environment)) return 1;
  for (uintnat tag = 0; tag < Num_tags; tag++) {
    if (environment == Atom((tag_t)tag)) return 1;
  }
  return 0;
}

static int host_context_matches(const struct caml_actor_scheduler *scheduler)
{
  caml_domain_state *domain = Caml_state_opt;

  return domain != NULL
    && scheduler != NULL
    && domain == scheduler->domain
    && domain->unique_id == scheduler->domain_unique_id
    && domain->actor_scheduler == scheduler
    && domain->actor_heap == NULL
    && domain->current_stack == scheduler->host_stack
    && domain->external_raise == scheduler->host_external_raise
    && domain->local_roots == scheduler->host_local_roots
    && domain->gc_regs == NULL
    && domain->trap_sp_off == scheduler->host_trap_sp_off
    && domain->trap_barrier_off == scheduler->host_trap_barrier_off
    && domain->trap_barrier_block == scheduler->host_trap_barrier_block
    && domain->backtrace_active == scheduler->host_backtrace_active
    && scheduler->current == ACTOR_SLOT_NONE;
}

static int refresh_host_context(struct caml_actor_scheduler *scheduler)
{
  caml_domain_state *domain = Caml_state_opt;

  if (domain == NULL
      || scheduler == NULL
      || domain != scheduler->domain
      || domain->unique_id != scheduler->domain_unique_id
      || domain->actor_scheduler != scheduler
      || domain->actor_heap != NULL
      || domain->current_stack == NULL
      || domain->current_stack->id != scheduler->host_stack_id
      || domain->external_raise != scheduler->host_external_raise
      || domain->local_roots != scheduler->host_local_roots
      || domain->gc_regs != NULL
      || domain->trap_sp_off != scheduler->host_trap_sp_off
      || domain->trap_barrier_off != scheduler->host_trap_barrier_off
      || domain->trap_barrier_block != scheduler->host_trap_barrier_block
      || scheduler->current != ACTOR_SLOT_NONE) {
    return 0;
  }
  scheduler->host_stack = domain->current_stack;
  scheduler->host_backtrace_active = domain->backtrace_active;
  return 1;
}

static int registered_code_range(code_t code, asize_t code_size)
{
  struct code_fragment *fragment =
    caml_find_code_fragment_by_pc((char *)code);

  return code_size >= sizeof(opcode_t)
    && code_size % sizeof(opcode_t) == 0
    && (uintptr_t)code % sizeof(opcode_t) == 0
    && fragment != NULL
    && (uintnat)code_size
         <= (uintnat)(fragment->code_end - (char *)code);
}

static void enqueue_tail(struct caml_actor_scheduler *scheduler,
                         uint32_t index)
{
  struct caml_actor_slot *slot = &scheduler->slots[index];

  CAMLassert(!slot->queued);
  CAMLassert(slot->lifecycle == CAML_ACTOR_LIFECYCLE_RUNNABLE);
  slot->ready_next = ACTOR_SLOT_NONE;
  slot->queued = 1;
  if (scheduler->ready_tail == ACTOR_SLOT_NONE) {
    scheduler->ready_head = index;
  } else {
    scheduler->slots[scheduler->ready_tail].ready_next = index;
  }
  scheduler->ready_tail = index;
}

static void enqueue_head(struct caml_actor_scheduler *scheduler,
                         uint32_t index)
{
  struct caml_actor_slot *slot = &scheduler->slots[index];

  CAMLassert(!slot->queued);
  CAMLassert(slot->lifecycle == CAML_ACTOR_LIFECYCLE_RUNNABLE);
  slot->ready_next = scheduler->ready_head;
  slot->queued = 1;
  scheduler->ready_head = index;
  if (scheduler->ready_tail == ACTOR_SLOT_NONE) {
    scheduler->ready_tail = index;
  }
}

static uint32_t dequeue_head(struct caml_actor_scheduler *scheduler)
{
  uint32_t index = scheduler->ready_head;
  struct caml_actor_slot *slot;

  if (index == ACTOR_SLOT_NONE) return index;
  slot = &scheduler->slots[index];
  scheduler->ready_head = slot->ready_next;
  if (scheduler->ready_head == ACTOR_SLOT_NONE) {
    scheduler->ready_tail = ACTOR_SLOT_NONE;
  }
  slot->ready_next = ACTOR_SLOT_NONE;
  slot->queued = 0;
  return index;
}

static void release_slot_resources(struct caml_actor_slot *slot)
{
  if (slot->stack != NULL) {
    caml_free_stack(slot->stack);
    slot->stack = NULL;
  }
  if (slot->heap != NULL) {
    caml_actor_heap_destroy(slot->heap);
    slot->heap = NULL;
  }
}

struct caml_actor_scheduler *caml_actor_scheduler_create(
  uintnat capacity, uintnat reduction_budget)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_scheduler *scheduler;
  struct caml_actor_slot *slots;

  if (!scheduler_runtime_supported()
      || domain == NULL || !caml_domain_alone()
      || domain->actor_scheduler != NULL || domain->actor_heap != NULL
      || domain->gc_regs != NULL || caml_debugger_in_use
      || capacity < 2 || capacity > CAML_ACTOR_PID_INDEX_MASK + 1
      || reduction_budget == 0
      || capacity > SIZE_MAX / sizeof(*slots)) {
    return NULL;
  }

  scheduler = calloc(1, sizeof(*scheduler));
  if (scheduler == NULL) return NULL;
  slots = calloc(capacity, sizeof(*slots));
  if (slots == NULL) {
    free(scheduler);
    return NULL;
  }

  scheduler->domain = domain;
  scheduler->domain_unique_id = domain->unique_id;
  scheduler->capacity = capacity;
  scheduler->reduction_budget = reduction_budget;
  scheduler->slots = slots;
  scheduler->ready_head = ACTOR_SLOT_NONE;
  scheduler->ready_tail = ACTOR_SLOT_NONE;
  scheduler->current = ACTOR_SLOT_NONE;
  scheduler->host_stack = domain->current_stack;
  scheduler->host_stack_id = domain->current_stack->id;
  scheduler->host_external_raise = domain->external_raise;
  scheduler->host_local_roots = domain->local_roots;
  scheduler->host_trap_sp_off = domain->trap_sp_off;
  scheduler->host_trap_barrier_off = domain->trap_barrier_off;
  scheduler->host_trap_barrier_block = domain->trap_barrier_block;
  scheduler->host_backtrace_active = domain->backtrace_active;

  slots[0].generation = 0;
  slots[0].ready_next = ACTOR_SLOT_NONE;
  for (uintnat index = 1; index < capacity; index++) {
    slots[index].generation = 1;
    slots[index].ready_next = ACTOR_SLOT_NONE;
  }
  domain->actor_scheduler = scheduler;
  return scheduler;
}

void caml_actor_scheduler_destroy(struct caml_actor_scheduler *scheduler)
{
  if (scheduler == NULL) return;
  if (!refresh_host_context(scheduler)) {
    caml_fatal_error("actor scheduler destroyed outside its host context");
  }

  scheduler->ready_head = ACTOR_SLOT_NONE;
  scheduler->ready_tail = ACTOR_SLOT_NONE;
  for (uintnat index = 0; index < scheduler->capacity; index++) {
    scheduler->slots[index].queued = 0;
    release_slot_resources(&scheduler->slots[index]);
  }
  scheduler->domain->actor_scheduler = NULL;
  free(scheduler->slots);
  free(scheduler);
}

static enum caml_actor_spawn_status spawn_code(
  struct caml_actor_scheduler *scheduler, int root,
  code_t code, asize_t code_size,
  value initial_env, intnat initial_extra_args,
  mlsize_t heap_quota_words, uintnat *pid_out)
{
  caml_domain_state *domain;
  struct caml_actor_slot *slot;
  struct caml_actor_heap *heap;
  struct stack_info *stack;
  uint32_t index = ACTOR_SLOT_NONE;
  uintnat pid;
  int64_t stack_id;

  if (pid_out != NULL) *pid_out = 0;
  if (!refresh_host_context(scheduler)
      || code == NULL || code_size == 0 || pid_out == NULL
      || heap_quota_words == 0
      || !approved_initial_environment(initial_env)
      || !registered_code_range(code, code_size)) {
    return CAML_ACTOR_SPAWN_UNSUPPORTED;
  }

  if (root) {
    if (scheduler->root_published
        || scheduler->slots[0].lifecycle != CAML_ACTOR_LIFECYCLE_FREE) {
      return CAML_ACTOR_SPAWN_LIMIT;
    }
    index = 0;
  } else {
    for (uintnat candidate = 1; candidate < scheduler->capacity;
         candidate++) {
      if (scheduler->slots[candidate].lifecycle
          == CAML_ACTOR_LIFECYCLE_FREE) {
        index = (uint32_t)candidate;
        break;
      }
    }
    if (index == ACTOR_SLOT_NONE) return CAML_ACTOR_SPAWN_LIMIT;
  }

  slot = &scheduler->slots[index];
  pid = make_pid(slot->generation, index);
  heap = caml_actor_heap_create(pid, heap_quota_words);
  if (heap == NULL) return CAML_ACTOR_SPAWN_HEAP_UNAVAILABLE;

  stack_id = -((int64_t)pid + 1);
  stack = caml_alloc_stack_noexc(
    caml_fiber_wsz, Val_unit, Val_unit, Val_unit, stack_id);
  if (stack == NULL) {
    caml_actor_heap_destroy(heap);
    return CAML_ACTOR_SPAWN_STACK_UNAVAILABLE;
  }

  domain = scheduler->domain;
  domain->current_stack = stack;
  caml_bytecode_state_init(
    &slot->bytecode, code, code_size, initial_env, initial_extra_args);
  stack = domain->current_stack;
  domain->current_stack = scheduler->host_stack;

  slot->lifecycle = CAML_ACTOR_LIFECYCLE_RUNNABLE;
  slot->failure = CAML_ACTOR_FAILURE_NONE;
  slot->pid = pid;
  slot->dispatches = 0;
  slot->reduction_stops = 0;
  slot->stack = stack;
  slot->heap = heap;
  if (root) scheduler->root_published = 1;
  enqueue_tail(scheduler, index);
  *pid_out = pid;
  return CAML_ACTOR_SPAWN_OK;
}

enum caml_actor_spawn_status caml_actor_scheduler_spawn_root_code(
  struct caml_actor_scheduler *scheduler,
  code_t code, asize_t code_size,
  value initial_env, intnat initial_extra_args,
  mlsize_t heap_quota_words, uintnat *pid)
{
  return spawn_code(scheduler, 1, code, code_size, initial_env,
                    initial_extra_args, heap_quota_words, pid);
}

enum caml_actor_spawn_status caml_actor_scheduler_spawn_code(
  struct caml_actor_scheduler *scheduler,
  code_t code, asize_t code_size,
  value initial_env, intnat initial_extra_args,
  mlsize_t heap_quota_words, uintnat *pid)
{
  return spawn_code(scheduler, 0, code, code_size, initial_env,
                    initial_extra_args, heap_quota_words, pid);
}

struct caml_actor_step caml_actor_scheduler_step(
  struct caml_actor_scheduler *scheduler)
{
  struct caml_actor_step step = { CAML_ACTOR_STEP_IDLE, 0 };
  struct caml_actor_heap_verify_result verified;
  enum caml_bytecode_stop_reason reason;
  caml_domain_state *domain;
  struct caml_actor_slot *slot;
  uint32_t index;
  value result = Val_unit;

  if (!refresh_host_context(scheduler)) {
    caml_fatal_error("actor scheduler entered outside its host context");
  }
  if (caml_check_pending_actions() && !caml_actor_world_is_frozen()) {
    step.reason = CAML_ACTOR_STEP_HOST_ACTION;
    return step;
  }
  index = dequeue_head(scheduler);
  if (index == ACTOR_SLOT_NONE) return step;
  slot = &scheduler->slots[index];
  domain = scheduler->domain;
  step.pid = slot->pid;

  slot->lifecycle = CAML_ACTOR_LIFECYCLE_RUNNING;
  slot->dispatches++;
  scheduler->current = index;
  domain->current_stack = slot->stack;
  domain->local_roots = NULL;
  domain->backtrace_active = 0;
  domain->trap_barrier_off = 0;
  domain->trap_barrier_block = INT64_MIN;

  if (!caml_actor_heap_activate(slot->heap)) {
    slot->stack = domain->current_stack;
    domain->current_stack = scheduler->host_stack;
    domain->local_roots = scheduler->host_local_roots;
    domain->backtrace_active = scheduler->host_backtrace_active;
    domain->trap_barrier_off = scheduler->host_trap_barrier_off;
    domain->trap_barrier_block = scheduler->host_trap_barrier_block;
    scheduler->current = ACTOR_SLOT_NONE;
    slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
    slot->failure = CAML_ACTOR_FAILURE_INTERNAL;
    step.reason = CAML_ACTOR_STEP_FAILED;
    return step;
  }
  if (scheduler->test_request_minor_gc_after_switch) {
    scheduler->test_request_minor_gc_after_switch = 0;
    caml_request_minor_gc();
  }

  reason = caml_bytecode_interpreter_slice(
    &slot->bytecode, scheduler->reduction_budget, &result);
  slot->stack = domain->current_stack;
  caml_actor_heap_deactivate();
  domain->current_stack = scheduler->host_stack;
  domain->local_roots = scheduler->host_local_roots;
  domain->backtrace_active = scheduler->host_backtrace_active;
  domain->trap_barrier_off = scheduler->host_trap_barrier_off;
  domain->trap_barrier_block = scheduler->host_trap_barrier_block;
  scheduler->current = ACTOR_SLOT_NONE;

  if (!host_context_matches(scheduler)) {
    caml_fatal_error("actor scheduler did not restore its host context");
  }
  verified = caml_actor_heap_verify(slot->heap);
  if (verified.error != CAML_ACTOR_HEAP_VERIFY_OK
      || caml_actor_heap_shared_bypasses(slot->heap) != 0) {
    slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
    slot->failure = CAML_ACTOR_FAILURE_INVALID_HEAP;
    step.reason = CAML_ACTOR_STEP_FAILED;
    return step;
  }

  switch (reason) {
    case CAML_BYTECODE_STOP_REDUCTIONS:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_RUNNABLE;
      slot->reduction_stops++;
      enqueue_tail(scheduler, index);
      step.reason = CAML_ACTOR_STEP_REDUCTIONS;
      break;
    case CAML_BYTECODE_STOP_HOST_ACTION:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_RUNNABLE;
      enqueue_head(scheduler, index);
      step.reason = CAML_ACTOR_STEP_HOST_ACTION;
      break;
    case CAML_BYTECODE_STOP_UNSUPPORTED:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
      slot->failure = CAML_ACTOR_FAILURE_UNSUPPORTED;
      step.reason = CAML_ACTOR_STEP_FAILED;
      break;
    case CAML_BYTECODE_STOP_HEAP_EXHAUSTED:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
      slot->failure = CAML_ACTOR_FAILURE_HEAP_EXHAUSTED;
      step.reason = CAML_ACTOR_STEP_FAILED;
      break;
    case CAML_BYTECODE_STOP_VALUE:
      if (Is_block(result)) {
        slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
        slot->failure = CAML_ACTOR_FAILURE_INVALID_RESULT;
        step.reason = CAML_ACTOR_STEP_FAILED;
      } else {
        slot->lifecycle = CAML_ACTOR_LIFECYCLE_EXITED;
        step.reason = CAML_ACTOR_STEP_EXITED;
      }
      break;
    case CAML_BYTECODE_STOP_EXCEPTION:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
      slot->failure = CAML_ACTOR_FAILURE_EXCEPTION;
      step.reason = CAML_ACTOR_STEP_FAILED;
      break;
    default:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
      slot->failure = CAML_ACTOR_FAILURE_INTERNAL;
      step.reason = CAML_ACTOR_STEP_FAILED;
      break;
  }
  return step;
}

static enum caml_actor_pid_lookup lookup_slot(
  const struct caml_actor_scheduler *scheduler, uintnat pid,
  const struct caml_actor_slot **slot_out)
{
  uint32_t index = (uint32_t)(pid & CAML_ACTOR_PID_INDEX_MASK);
  uintnat generation = pid >> CAML_ACTOR_PID_INDEX_BITS;
  const struct caml_actor_slot *slot;

  if (slot_out != NULL) *slot_out = NULL;
  if (scheduler == NULL || index >= scheduler->capacity) {
    return CAML_ACTOR_PID_MISSING;
  }
  slot = &scheduler->slots[index];
  if (generation != slot->generation
      || slot->lifecycle == CAML_ACTOR_LIFECYCLE_RETIRED) {
    return CAML_ACTOR_PID_STALE;
  }
  if (slot->lifecycle == CAML_ACTOR_LIFECYCLE_FREE) {
    return CAML_ACTOR_PID_MISSING;
  }
  if (slot_out != NULL) *slot_out = slot;
  return CAML_ACTOR_PID_PRESENT;
}

enum caml_actor_pid_lookup caml_actor_scheduler_snapshot(
  const struct caml_actor_scheduler *scheduler, uintnat pid,
  struct caml_actor_snapshot *snapshot)
{
  const struct caml_actor_slot *slot;
  enum caml_actor_pid_lookup lookup = lookup_slot(scheduler, pid, &slot);

  if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
  if (lookup != CAML_ACTOR_PID_PRESENT || snapshot == NULL) return lookup;
  snapshot->lifecycle = slot->lifecycle;
  snapshot->failure = slot->failure;
  snapshot->pid = slot->pid;
  snapshot->dispatches = slot->dispatches;
  snapshot->reduction_stops = slot->reduction_stops;
  return lookup;
}

int caml_actor_scheduler_retire(struct caml_actor_scheduler *scheduler,
                                uintnat pid)
{
  const struct caml_actor_slot *found;
  struct caml_actor_slot *slot;
  enum caml_actor_pid_lookup lookup;
  uint32_t index;

  if (!refresh_host_context(scheduler)) return 0;
  lookup = lookup_slot(scheduler, pid, &found);
  if (lookup != CAML_ACTOR_PID_PRESENT
      || (found->lifecycle != CAML_ACTOR_LIFECYCLE_EXITED
          && found->lifecycle != CAML_ACTOR_LIFECYCLE_FAILED)) {
    return 0;
  }

  index = (uint32_t)(pid & CAML_ACTOR_PID_INDEX_MASK);
  slot = &scheduler->slots[index];
  CAMLassert(!slot->queued);
  release_slot_resources(slot);
  slot->failure = CAML_ACTOR_FAILURE_NONE;
  slot->pid = 0;
  slot->dispatches = 0;
  slot->reduction_stops = 0;
  slot->ready_next = ACTOR_SLOT_NONE;
  if (index == 0 || slot->generation >= max_pid_generation()) {
    slot->lifecycle = CAML_ACTOR_LIFECYCLE_RETIRED;
  } else {
    slot->generation++;
    slot->lifecycle = CAML_ACTOR_LIFECYCLE_FREE;
  }
  return 1;
}

void caml_actor_scheduler_test_request_minor_gc_after_switch(
  struct caml_actor_scheduler *scheduler)
{
  if (!refresh_host_context(scheduler)) {
    caml_fatal_error("actor scheduler test injection outside host context");
  }
  scheduler->test_request_minor_gc_after_switch = 1;
}

int caml_actor_scheduler_is_running(void)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_scheduler *scheduler;

  if (domain == NULL) return 0;
  scheduler = domain->actor_scheduler;
  return scheduler != NULL && scheduler->current != ACTOR_SLOT_NONE;
}

int caml_actor_scheduler_primitive_allowed(uintnat primitive)
{
  if (primitive >= (uintnat)caml_prim_table.size) return 0;
  return (c_primitive)caml_prim_table.contents[primitive]
    == (c_primitive)caml_int_compare;
}

#else /* NATIVE_CODE */

struct caml_actor_scheduler *caml_actor_scheduler_create(
  uintnat capacity, uintnat reduction_budget)
{
  (void)capacity;
  (void)reduction_budget;
  return NULL;
}

void caml_actor_scheduler_destroy(struct caml_actor_scheduler *scheduler)
{
  (void)scheduler;
}

enum caml_actor_spawn_status caml_actor_scheduler_spawn_root_code(
  struct caml_actor_scheduler *scheduler,
  code_t code, asize_t code_size,
  value initial_env, intnat initial_extra_args,
  mlsize_t heap_quota_words, uintnat *pid)
{
  (void)scheduler;
  (void)code;
  (void)code_size;
  (void)initial_env;
  (void)initial_extra_args;
  (void)heap_quota_words;
  if (pid != NULL) *pid = 0;
  return CAML_ACTOR_SPAWN_UNSUPPORTED;
}

enum caml_actor_spawn_status caml_actor_scheduler_spawn_code(
  struct caml_actor_scheduler *scheduler,
  code_t code, asize_t code_size,
  value initial_env, intnat initial_extra_args,
  mlsize_t heap_quota_words, uintnat *pid)
{
  return caml_actor_scheduler_spawn_root_code(
    scheduler, code, code_size, initial_env, initial_extra_args,
    heap_quota_words, pid);
}

struct caml_actor_step caml_actor_scheduler_step(
  struct caml_actor_scheduler *scheduler)
{
  struct caml_actor_step step = { CAML_ACTOR_STEP_IDLE, 0 };
  (void)scheduler;
  return step;
}

enum caml_actor_pid_lookup caml_actor_scheduler_snapshot(
  const struct caml_actor_scheduler *scheduler, uintnat pid,
  struct caml_actor_snapshot *snapshot)
{
  (void)scheduler;
  (void)pid;
  if (snapshot != NULL) memset(snapshot, 0, sizeof(*snapshot));
  return CAML_ACTOR_PID_MISSING;
}

int caml_actor_scheduler_retire(struct caml_actor_scheduler *scheduler,
                                uintnat pid)
{
  (void)scheduler;
  (void)pid;
  return 0;
}

void caml_actor_scheduler_test_request_minor_gc_after_switch(
  struct caml_actor_scheduler *scheduler)
{
  (void)scheduler;
}

int caml_actor_scheduler_is_running(void)
{
  return 0;
}

int caml_actor_scheduler_primitive_allowed(uintnat primitive)
{
  (void)primitive;
  return 0;
}

#endif /* NATIVE_CODE */
