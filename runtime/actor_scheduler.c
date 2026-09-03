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

#include "caml/actor_copy.h"
#include "caml/actor_heap.h"
#include "caml/actor_scheduler.h"
#include "caml/actor_wire.h"
#include "caml/actor_world.h"
#include "caml/callback.h"
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
CAMLextern value caml_array_make(value len, value init);
CAMLextern value caml_actor_spawn(value closure);
CAMLextern value caml_actor_self(value inbox);
CAMLextern value caml_actor_yield(value unit);
CAMLextern value caml_actor_send(value pid, value message);
CAMLextern value caml_actor_receive(value inbox);
CAMLextern value caml_actor_stats(value unit);

enum caml_actor_primitive_capability {
  CAML_ACTOR_PRIMITIVE_PURE = 0,
  CAML_ACTOR_PRIMITIVE_ACTOR_LOCAL,
  CAML_ACTOR_PRIMITIVE_SCHEDULER_AWARE,
  CAML_ACTOR_PRIMITIVE_FORBIDDEN
};

struct caml_actor_primitive_policy_entry {
  const char *name;
  c_primitive function;
  int arity;
  enum caml_actor_primitive_capability capability;
};

#define CAML_ACTOR_NO_PRIMITIVE NULL
#define ACTOR_PRIMITIVE(name, function, arity, capability, family, audit) \
  { name, (c_primitive)(function), arity, \
    CAML_ACTOR_PRIMITIVE_ ## capability },
static const struct caml_actor_primitive_policy_entry
actor_primitive_policy[] = {
#include "actor_primitive_policy.def"
};
#undef ACTOR_PRIMITIVE
#undef CAML_ACTOR_NO_PRIMITIVE

struct caml_actor_slot {
  enum caml_actor_lifecycle lifecycle;
  enum caml_actor_failure failure;
  uintnat generation;
  uintnat pid;
  uintnat dispatches;
  uintnat reduction_stops;
  struct caml_actor_unsupported unsupported;
  uint32_t ready_next;
  int queued;
  struct stack_info *stack;
  struct caml_actor_heap *heap;
  struct caml_bytecode_state bytecode;
  struct caml_actor_prepared_send *mailbox_head;
  struct caml_actor_prepared_send *mailbox_tail;
  uintnat mailbox_length;
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
  enum caml_actor_control_request control_request;
  int test_request_minor_gc_after_switch;

  uintnat total_spawned;
  uintnat total_exited;
  uintnat total_failed;
  uintnat total_dispatches;
  uintnat total_reduction_stops;
  uintnat messages_sent;
  uintnat messages_received;
  uintnat messages_dropped;
  uintnat mailbox_messages;

  struct stack_info *host_stack;
  int64_t host_stack_id;
  struct caml_exception_context *host_external_raise;
  struct caml__roots_block *host_local_roots;
  intnat host_trap_sp_off;
  intnat host_trap_barrier_off;
  int64_t host_trap_barrier_block;
  intnat host_backtrace_active;
};

struct caml_actor_prepared_spawn {
  struct caml_actor_scheduler *scheduler;
  uint32_t index;
  uint32_t parent_index;
  uintnat pid;
  struct stack_info *stack;
  struct caml_actor_heap *heap;
  struct caml_bytecode_state bytecode;
};

struct caml_actor_prepared_send {
  struct caml_actor_scheduler *scheduler;
  uint32_t target_index;
  uintnat target_pid;
  struct caml_actor_envelope *envelope;
  struct caml_actor_prepared_send *next;
};

static void add_counter(uintnat *counter, uintnat amount)
{
  if (amount > CAML_UINTNAT_MAX - *counter) {
    *counter = CAML_UINTNAT_MAX;
  } else {
    *counter += amount;
  }
}

static void increment_counter(uintnat *counter)
{
  add_counter(counter, 1);
}

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

static int running_context_matches(
  const struct caml_actor_scheduler *scheduler)
{
  caml_domain_state *domain = Caml_state_opt;
  const struct caml_actor_slot *slot;

  if (domain == NULL || scheduler == NULL
      || domain != scheduler->domain
      || domain->unique_id != scheduler->domain_unique_id
      || domain->actor_scheduler != scheduler
      || scheduler->current == ACTOR_SLOT_NONE
      || scheduler->current >= scheduler->capacity
      || domain->current_stack == NULL
      || domain->local_roots != NULL
      || domain->gc_regs != NULL) {
    return 0;
  }
  slot = &scheduler->slots[scheduler->current];
  return slot->lifecycle == CAML_ACTOR_LIFECYCLE_RUNNING
    && domain->actor_heap == slot->heap
    && domain->current_stack->id == slot->bytecode.stack_id;
}

/* Control requests are recorded inside actor primitives, where CAMLparam may
   temporarily make [local_roots] non-NULL. No scheduler transition happens
   until the primitive returns and the interpreter takes the request. */
static int current_actor_context_matches(
  const struct caml_actor_scheduler *scheduler)
{
  caml_domain_state *domain = Caml_state_opt;
  const struct caml_actor_slot *slot;

  if (domain == NULL || scheduler == NULL
      || domain != scheduler->domain
      || domain->unique_id != scheduler->domain_unique_id
      || domain->actor_scheduler != scheduler
      || scheduler->current == ACTOR_SLOT_NONE
      || scheduler->current >= scheduler->capacity
      || domain->current_stack == NULL
      || domain->gc_regs != NULL) {
    return 0;
  }
  slot = &scheduler->slots[scheduler->current];
  return slot->lifecycle == CAML_ACTOR_LIFECYCLE_RUNNING
    && domain->actor_heap == slot->heap
    && domain->current_stack->id == slot->bytecode.stack_id;
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
  struct caml_actor_prepared_send *message = slot->mailbox_head;

  while (message != NULL) {
    struct caml_actor_prepared_send *next = message->next;

    caml_actor_wire_destroy(message->envelope);
    free(message);
    message = next;
  }
  slot->mailbox_head = NULL;
  slot->mailbox_tail = NULL;
  slot->mailbox_length = 0;
  if (slot->stack != NULL) {
    caml_free_stack(slot->stack);
    slot->stack = NULL;
  }
  if (slot->heap != NULL) {
    caml_actor_heap_destroy(slot->heap);
    slot->heap = NULL;
  }
}

#ifdef DEBUG
static int scheduler_mailboxes_valid(
  const struct caml_actor_scheduler *scheduler)
{
  for (uintnat index = 0; index < scheduler->capacity; index++) {
    const struct caml_actor_slot *slot = &scheduler->slots[index];
    const struct caml_actor_prepared_send *message = slot->mailbox_head;
    const struct caml_actor_prepared_send *last = NULL;

    for (uintnat count = 0; count < slot->mailbox_length; count++) {
      if (message == NULL || message->scheduler != NULL
          || message->target_index != index
          || message->target_pid != slot->pid
          || !caml_actor_wire_verify(message->envelope)) {
        return 0;
      }
      last = message;
      message = message->next;
    }
    if (message != NULL || last != slot->mailbox_tail
        || ((slot->mailbox_length == 0)
            != (slot->mailbox_head == NULL))) {
      return 0;
    }
  }
  return 1;
}
#endif

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
  memset(&slot->unsupported, 0, sizeof(slot->unsupported));
  slot->stack = stack;
  slot->heap = heap;
  if (root) scheduler->root_published = 1;
  increment_counter(&scheduler->total_spawned);
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

static enum caml_actor_spawn_status copy_status_to_spawn_status(
  enum caml_actor_copy_status status)
{
  switch (status) {
  case CAML_ACTOR_COPY_GRAPH_TOO_LARGE:
    return CAML_ACTOR_SPAWN_INITIAL_HEAP_LIMIT;
  case CAML_ACTOR_COPY_RESOURCE_UNAVAILABLE:
    return CAML_ACTOR_SPAWN_HEAP_UNAVAILABLE;
  case CAML_ACTOR_COPY_UNSUPPORTED_RUNTIME:
    return CAML_ACTOR_SPAWN_UNSUPPORTED;
  case CAML_ACTOR_COPY_INVALID_SOURCE:
  case CAML_ACTOR_COPY_UNSUPPORTED_TAG:
  case CAML_ACTOR_COPY_INVALID_CLOSURE:
  case CAML_ACTOR_COPY_INVALID_CODE_POINTER:
  case CAML_ACTOR_COPY_FINALISABLE:
    return CAML_ACTOR_SPAWN_UNSUPPORTED_CAPTURE;
  case CAML_ACTOR_COPY_INTERNAL:
  case CAML_ACTOR_COPY_OK:
  default:
    return CAML_ACTOR_SPAWN_UNSUPPORTED;
  }
}

static int copied_closure_code_range(value closure,
                                     code_t *code, asize_t *code_size)
{
  struct code_fragment *fragment;

  if (!Is_block(closure) || code == NULL || code_size == NULL) return 0;
  *code = Code_val(closure);
  fragment = caml_find_code_fragment_by_pc((char *)*code);
  if (fragment == NULL
      || (uintptr_t)*code % sizeof(opcode_t) != 0
      || (char *)*code + sizeof(opcode_t) > fragment->code_end) {
    return 0;
  }
  *code_size = fragment->code_end - (char *)*code;
  return 1;
}

static enum caml_actor_spawn_status prepare_closure(
  struct caml_actor_scheduler *scheduler, int root, value closure,
  mlsize_t heap_quota_words,
  struct caml_actor_prepared_spawn **prepared_out)
{
  struct caml_actor_prepared_spawn *prepared = NULL;
  struct caml_actor_copy_result copied;
  struct caml_actor_slot *slot;
  struct stack_info *saved_stack;
  struct stack_info *stack = NULL;
  caml_domain_state *domain;
  code_t code;
  asize_t code_size;
  uint32_t index = ACTOR_SLOT_NONE;
  uint32_t parent_index = ACTOR_SLOT_NONE;
  uintnat pid;

  if (prepared_out != NULL) *prepared_out = NULL;
  if (scheduler == NULL || prepared_out == NULL
      || heap_quota_words == 0) {
    return CAML_ACTOR_SPAWN_UNSUPPORTED;
  }
  if (root) {
    if (!refresh_host_context(scheduler) || scheduler->root_published
        || scheduler->slots[0].lifecycle
             != CAML_ACTOR_LIFECYCLE_FREE) {
      return CAML_ACTOR_SPAWN_LIMIT;
    }
    index = 0;
  } else {
    if (!running_context_matches(scheduler)) {
      return CAML_ACTOR_SPAWN_UNSUPPORTED;
    }
    parent_index = scheduler->current;
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
  prepared = malloc(sizeof(*prepared));
  if (prepared == NULL) return CAML_ACTOR_SPAWN_HEAP_UNAVAILABLE;
  memset(prepared, 0, sizeof(*prepared));

  copied = caml_actor_copy_closure(closure, pid, heap_quota_words);
  if (copied.status != CAML_ACTOR_COPY_OK) {
    enum caml_actor_spawn_status status =
      copy_status_to_spawn_status(copied.status);

    free(prepared);
    return status;
  }
  if ((root && !refresh_host_context(scheduler))
      || (!root && !running_context_matches(scheduler))
      || !copied_closure_code_range(
           copied.closure, &code, &code_size)) {
    caml_actor_heap_destroy(copied.heap);
    free(prepared);
    return CAML_ACTOR_SPAWN_UNSUPPORTED;
  }

  stack = caml_alloc_stack_noexc(
    caml_fiber_wsz, Val_unit, Val_unit, Val_unit, -((int64_t)pid + 1));
  if (stack == NULL) {
    caml_actor_heap_destroy(copied.heap);
    free(prepared);
    return CAML_ACTOR_SPAWN_STACK_UNAVAILABLE;
  }
  if (stack->sp - 8 < Stack_base(stack)) {
    caml_free_stack(stack);
    caml_actor_heap_destroy(copied.heap);
    free(prepared);
    return CAML_ACTOR_SPAWN_STACK_UNAVAILABLE;
  }

  stack->sp -= 4;
  stack->sp[0] = Val_long(pid);
  stack->sp[1] = (value)caml_bytecode_callback_code();
  stack->sp[2] = Val_unit;
  stack->sp[3] = Val_long(0);

  domain = scheduler->domain;
  saved_stack = domain->current_stack;
  domain->current_stack = stack;
  caml_bytecode_state_init(
    &prepared->bytecode, code, code_size, copied.closure, 0);
  stack = domain->current_stack;
  domain->current_stack = saved_stack;
  prepared->bytecode.return_trap_sp_off = scheduler->host_trap_sp_off;

  prepared->scheduler = scheduler;
  prepared->index = index;
  prepared->parent_index = parent_index;
  prepared->pid = pid;
  prepared->stack = stack;
  prepared->heap = copied.heap;
  *prepared_out = prepared;
  return CAML_ACTOR_SPAWN_OK;
}

enum caml_actor_spawn_status caml_actor_scheduler_prepare_root_closure(
  struct caml_actor_scheduler *scheduler, value closure,
  mlsize_t heap_quota_words,
  struct caml_actor_prepared_spawn **prepared)
{
  return prepare_closure(
    scheduler, 1, closure, heap_quota_words, prepared);
}

enum caml_actor_spawn_status caml_actor_scheduler_prepare_closure(
  struct caml_actor_scheduler *scheduler, value closure,
  mlsize_t heap_quota_words,
  struct caml_actor_prepared_spawn **prepared)
{
  return prepare_closure(
    scheduler, 0, closure, heap_quota_words, prepared);
}

uintnat caml_actor_scheduler_prepared_pid(
  const struct caml_actor_prepared_spawn *prepared)
{
  return prepared == NULL ? 0 : prepared->pid;
}

void caml_actor_scheduler_abort_prepared(
  struct caml_actor_prepared_spawn *prepared)
{
  if (prepared == NULL) return;
  if (prepared->stack != NULL) caml_free_stack(prepared->stack);
  if (prepared->heap != NULL) caml_actor_heap_destroy(prepared->heap);
  free(prepared);
}

int caml_actor_scheduler_commit_prepared(
  struct caml_actor_prepared_spawn *prepared)
{
  struct caml_actor_scheduler *scheduler;
  struct caml_actor_slot *slot;
  int root;
  int context_matches;

  if (prepared == NULL || prepared->scheduler == NULL) return 0;
  scheduler = prepared->scheduler;
  root = prepared->parent_index == ACTOR_SLOT_NONE;
  context_matches = root
    ? refresh_host_context(scheduler) : running_context_matches(scheduler);
  if (!context_matches || prepared->index >= scheduler->capacity
      || (!root && scheduler->current != prepared->parent_index)) {
    caml_actor_scheduler_abort_prepared(prepared);
    return 0;
  }
  slot = &scheduler->slots[prepared->index];
  if (slot->lifecycle != CAML_ACTOR_LIFECYCLE_FREE
      || slot->generation
           != prepared->pid >> CAML_ACTOR_PID_INDEX_BITS
      || make_pid(slot->generation, prepared->index) != prepared->pid
      || (root && scheduler->root_published)) {
    caml_actor_scheduler_abort_prepared(prepared);
    return 0;
  }

  slot->lifecycle = CAML_ACTOR_LIFECYCLE_RUNNABLE;
  slot->failure = CAML_ACTOR_FAILURE_NONE;
  slot->pid = prepared->pid;
  slot->dispatches = 0;
  slot->reduction_stops = 0;
  memset(&slot->unsupported, 0, sizeof(slot->unsupported));
  slot->stack = prepared->stack;
  slot->heap = prepared->heap;
  slot->bytecode = prepared->bytecode;
  prepared->stack = NULL;
  prepared->heap = NULL;
  if (root) scheduler->root_published = 1;
  increment_counter(&scheduler->total_spawned);
  enqueue_tail(scheduler, prepared->index);
  free(prepared);
  return 1;
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
#ifdef DEBUG
  CAMLassert(scheduler_mailboxes_valid(scheduler));
#endif
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
  increment_counter(&slot->dispatches);
  increment_counter(&scheduler->total_dispatches);
  memset(&slot->unsupported, 0, sizeof(slot->unsupported));
  scheduler->current = index;
  CAMLassert(scheduler->control_request == CAML_ACTOR_CONTROL_NONE);
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
    increment_counter(&scheduler->total_failed);
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
#ifdef DEBUG
  CAMLassert(scheduler_mailboxes_valid(scheduler));
#endif
  verified = caml_actor_heap_verify(slot->heap);
  if (verified.error != CAML_ACTOR_HEAP_VERIFY_OK
      || caml_actor_heap_shared_bypasses(slot->heap) != 0) {
    slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
    slot->failure = CAML_ACTOR_FAILURE_INVALID_HEAP;
    increment_counter(&scheduler->total_failed);
    step.reason = CAML_ACTOR_STEP_FAILED;
    return step;
  }

  switch (reason) {
    case CAML_BYTECODE_STOP_REDUCTIONS:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_RUNNABLE;
      increment_counter(&slot->reduction_stops);
      increment_counter(&scheduler->total_reduction_stops);
      enqueue_tail(scheduler, index);
      step.reason = CAML_ACTOR_STEP_REDUCTIONS;
      break;
    case CAML_BYTECODE_STOP_YIELD:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_RUNNABLE;
      enqueue_tail(scheduler, index);
      step.reason = CAML_ACTOR_STEP_YIELD;
      break;
    case CAML_BYTECODE_STOP_BLOCKED:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_BLOCKED;
      step.reason = CAML_ACTOR_STEP_BLOCKED;
      break;
    case CAML_BYTECODE_STOP_HOST_ACTION:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_RUNNABLE;
      enqueue_head(scheduler, index);
      step.reason = CAML_ACTOR_STEP_HOST_ACTION;
      break;
    case CAML_BYTECODE_STOP_UNSUPPORTED:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
      slot->failure = CAML_ACTOR_FAILURE_UNSUPPORTED;
      increment_counter(&scheduler->total_failed);
      step.reason = CAML_ACTOR_STEP_FAILED;
      break;
    case CAML_BYTECODE_STOP_HEAP_EXHAUSTED:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
      slot->failure = CAML_ACTOR_FAILURE_HEAP_EXHAUSTED;
      increment_counter(&scheduler->total_failed);
      step.reason = CAML_ACTOR_STEP_FAILED;
      break;
    case CAML_BYTECODE_STOP_VALUE:
      if (Is_block(result)) {
        slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
        slot->failure = CAML_ACTOR_FAILURE_INVALID_RESULT;
        increment_counter(&scheduler->total_failed);
        step.reason = CAML_ACTOR_STEP_FAILED;
      } else {
        slot->lifecycle = CAML_ACTOR_LIFECYCLE_EXITED;
        increment_counter(&scheduler->total_exited);
        step.reason = CAML_ACTOR_STEP_EXITED;
      }
      break;
    case CAML_BYTECODE_STOP_EXCEPTION:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
      slot->failure = CAML_ACTOR_FAILURE_EXCEPTION;
      increment_counter(&scheduler->total_failed);
      step.reason = CAML_ACTOR_STEP_FAILED;
      break;
    default:
      slot->lifecycle = CAML_ACTOR_LIFECYCLE_FAILED;
      slot->failure = CAML_ACTOR_FAILURE_INTERNAL;
      increment_counter(&scheduler->total_failed);
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

static int slot_accepts_messages(const struct caml_actor_slot *slot)
{
  return slot->lifecycle == CAML_ACTOR_LIFECYCLE_RUNNABLE
    || slot->lifecycle == CAML_ACTOR_LIFECYCLE_RUNNING
    || slot->lifecycle == CAML_ACTOR_LIFECYCLE_BLOCKED;
}

enum caml_actor_send_status caml_actor_scheduler_can_send(
  struct caml_actor_scheduler *scheduler, uintnat pid)
{
  const struct caml_actor_slot *slot;

  if (!running_context_matches(scheduler)) {
    return CAML_ACTOR_SEND_INVALID_CONTEXT;
  }
  if (lookup_slot(scheduler, pid, &slot) != CAML_ACTOR_PID_PRESENT
      || !slot_accepts_messages(slot)) {
    return CAML_ACTOR_SEND_NO_SUCH_ACTOR;
  }
  if (slot->mailbox_length == CAML_UINTNAT_MAX) {
    return CAML_ACTOR_SEND_RESOURCE_UNAVAILABLE;
  }
  return CAML_ACTOR_SEND_OK;
}

enum caml_actor_send_status caml_actor_scheduler_prepare_send(
  struct caml_actor_scheduler *scheduler, uintnat pid,
  struct caml_actor_envelope *envelope,
  struct caml_actor_prepared_send **prepared_out)
{
  struct caml_actor_prepared_send *prepared;
  enum caml_actor_send_status status;

  if (prepared_out != NULL) *prepared_out = NULL;
  if (prepared_out == NULL || envelope == NULL
      || !caml_actor_wire_verify(envelope)) {
    return CAML_ACTOR_SEND_INVALID_CONTEXT;
  }
  status = caml_actor_scheduler_can_send(scheduler, pid);
  if (status != CAML_ACTOR_SEND_OK) return status;
  prepared = calloc(1, sizeof(*prepared));
  if (prepared == NULL) return CAML_ACTOR_SEND_RESOURCE_UNAVAILABLE;
  prepared->scheduler = scheduler;
  prepared->target_index =
    (uint32_t)(pid & CAML_ACTOR_PID_INDEX_MASK);
  prepared->target_pid = pid;
  prepared->envelope = envelope;
  *prepared_out = prepared;
  return CAML_ACTOR_SEND_OK;
}

void caml_actor_scheduler_abort_send(
  struct caml_actor_prepared_send *prepared)
{
  if (prepared == NULL) return;
  caml_actor_wire_destroy(prepared->envelope);
  free(prepared);
}

int caml_actor_scheduler_commit_send(
  struct caml_actor_prepared_send *prepared)
{
  struct caml_actor_scheduler *scheduler;
  struct caml_actor_slot *slot;

  if (prepared == NULL || prepared->scheduler == NULL
      || prepared->envelope == NULL) {
    return 0;
  }
  scheduler = prepared->scheduler;
  if (caml_actor_scheduler_can_send(
        scheduler, prepared->target_pid) != CAML_ACTOR_SEND_OK
      || prepared->target_index >= scheduler->capacity) {
    caml_actor_scheduler_abort_send(prepared);
    return 0;
  }
  slot = &scheduler->slots[prepared->target_index];
  if (slot->pid != prepared->target_pid
      || !caml_actor_wire_verify(prepared->envelope)
      || scheduler->mailbox_messages == CAML_UINTNAT_MAX) {
    caml_actor_scheduler_abort_send(prepared);
    return 0;
  }
  prepared->scheduler = NULL;
  prepared->next = NULL;
  if (slot->mailbox_tail == NULL) {
    slot->mailbox_head = prepared;
  } else {
    slot->mailbox_tail->next = prepared;
  }
  slot->mailbox_tail = prepared;
  slot->mailbox_length++;
  increment_counter(&scheduler->messages_sent);
  scheduler->mailbox_messages++;
  if (slot->lifecycle == CAML_ACTOR_LIFECYCLE_BLOCKED) {
    slot->lifecycle = CAML_ACTOR_LIFECYCLE_RUNNABLE;
    enqueue_tail(scheduler, prepared->target_index);
  }
#ifdef DEBUG
  CAMLassert(scheduler_mailboxes_valid(scheduler));
#endif
  return 1;
}

const struct caml_actor_envelope *
caml_actor_scheduler_peek_current_message(
  struct caml_actor_scheduler *scheduler)
{
  const struct caml_actor_slot *slot;

  if (!running_context_matches(scheduler)) return NULL;
  slot = &scheduler->slots[scheduler->current];
  if (slot->mailbox_head == NULL) return NULL;
  if (slot->mailbox_length == 0
      || slot->mailbox_tail == NULL
      || !caml_actor_wire_verify(slot->mailbox_head->envelope)) {
    caml_fatal_error("invalid actor mailbox");
  }
  return slot->mailbox_head->envelope;
}

int caml_actor_scheduler_consume_current_message(
  struct caml_actor_scheduler *scheduler)
{
  struct caml_actor_prepared_send *message;
  struct caml_actor_slot *slot;

  if (!running_context_matches(scheduler)) return 0;
  slot = &scheduler->slots[scheduler->current];
  message = slot->mailbox_head;
  if (message == NULL || slot->mailbox_length == 0) return 0;
  slot->mailbox_head = message->next;
  if (slot->mailbox_head == NULL) slot->mailbox_tail = NULL;
  slot->mailbox_length--;
  increment_counter(&scheduler->messages_received);
  scheduler->mailbox_messages--;
  caml_actor_wire_destroy(message->envelope);
  free(message);
#ifdef DEBUG
  CAMLassert(scheduler_mailboxes_valid(scheduler));
#endif
  return 1;
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
  snapshot->unsupported = slot->unsupported;
  return lookup;
}

int caml_actor_scheduler_stats(
  const struct caml_actor_scheduler *scheduler,
  struct caml_actor_scheduler_stats *stats)
{
  if (scheduler == NULL || stats == NULL
      || !running_context_matches(scheduler)) {
    return 0;
  }

  memset(stats, 0, sizeof(*stats));
  stats->total_spawned = scheduler->total_spawned;
  stats->total_exited = scheduler->total_exited;
  stats->total_failed = scheduler->total_failed;
  stats->total_dispatches = scheduler->total_dispatches;
  stats->total_reduction_stops = scheduler->total_reduction_stops;
  stats->messages_sent = scheduler->messages_sent;
  stats->messages_received = scheduler->messages_received;
  stats->messages_dropped = scheduler->messages_dropped;
  stats->mailbox_messages = scheduler->mailbox_messages;

  for (uintnat index = 0; index < scheduler->capacity; index++) {
    const struct caml_actor_slot *slot = &scheduler->slots[index];

    switch (slot->lifecycle) {
    case CAML_ACTOR_LIFECYCLE_RUNNABLE:
    case CAML_ACTOR_LIFECYCLE_RUNNING:
      stats->live_actors++;
      stats->runnable_actors++;
      break;
    case CAML_ACTOR_LIFECYCLE_BLOCKED:
      stats->live_actors++;
      stats->blocked_actors++;
      break;
    default:
      break;
    }
  }
  return 1;
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
  CAMLassert(scheduler->mailbox_messages >= slot->mailbox_length);
  add_counter(&scheduler->messages_dropped, slot->mailbox_length);
  scheduler->mailbox_messages -= slot->mailbox_length;
  release_slot_resources(slot);
  slot->failure = CAML_ACTOR_FAILURE_NONE;
  slot->pid = 0;
  slot->dispatches = 0;
  slot->reduction_stops = 0;
  memset(&slot->unsupported, 0, sizeof(slot->unsupported));
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

uintnat caml_actor_scheduler_current_pid(void)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_scheduler *scheduler;

  if (domain == NULL || domain->actor_scheduler == NULL) return 0;
  scheduler = domain->actor_scheduler;
  if (!running_context_matches(scheduler)) return 0;
  return scheduler->slots[scheduler->current].pid;
}

static int request_control(enum caml_actor_control_request request)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_scheduler *scheduler;

  if (domain == NULL || domain->actor_scheduler == NULL) return 0;
  scheduler = domain->actor_scheduler;
  if (!current_actor_context_matches(scheduler)
      || scheduler->control_request != CAML_ACTOR_CONTROL_NONE) {
    return 0;
  }
  scheduler->control_request = request;
  return 1;
}

int caml_actor_scheduler_request_yield(void)
{
  return request_control(CAML_ACTOR_CONTROL_YIELD);
}

int caml_actor_scheduler_request_blocked(void)
{
  return request_control(CAML_ACTOR_CONTROL_BLOCKED);
}

int caml_actor_scheduler_request_unsupported(void)
{
  return request_control(CAML_ACTOR_CONTROL_UNSUPPORTED);
}

int caml_actor_scheduler_request_heap_exhausted(void)
{
  return request_control(CAML_ACTOR_CONTROL_HEAP_EXHAUSTED);
}

static void record_unsupported(
  enum caml_actor_unsupported_kind kind, uintnat operation, int arity)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_scheduler *scheduler;
  struct caml_actor_slot *slot;

  if (domain == NULL || domain->actor_scheduler == NULL) return;
  scheduler = domain->actor_scheduler;
  if (!current_actor_context_matches(scheduler)) return;
  slot = &scheduler->slots[scheduler->current];
  if (slot->unsupported.kind != CAML_ACTOR_UNSUPPORTED_NONE) return;
  slot->unsupported.kind = kind;
  slot->unsupported.operation = operation;
  slot->unsupported.arity = arity;
}

void caml_actor_scheduler_record_unsupported_opcode(uintnat opcode)
{
  record_unsupported(CAML_ACTOR_UNSUPPORTED_OPCODE, opcode, 0);
}

void caml_actor_scheduler_record_unsupported_primitive(
  uintnat primitive, int arity)
{
  record_unsupported(CAML_ACTOR_UNSUPPORTED_PRIMITIVE, primitive, arity);
}

enum caml_actor_control_request
caml_actor_scheduler_take_control_request(void)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_scheduler *scheduler;

  enum caml_actor_control_request request;

  if (domain == NULL || domain->actor_scheduler == NULL) {
    return CAML_ACTOR_CONTROL_NONE;
  }
  scheduler = domain->actor_scheduler;
  if (!running_context_matches(scheduler)
      || scheduler->control_request == CAML_ACTOR_CONTROL_NONE) {
    return CAML_ACTOR_CONTROL_NONE;
  }
  request = scheduler->control_request;
  scheduler->control_request = CAML_ACTOR_CONTROL_NONE;
  return request;
}

int caml_actor_scheduler_primitive_allowed(uintnat primitive, int arity)
{
  c_primitive function;
  const char *name;

  if (primitive >= (uintnat)caml_prim_table.size
      || primitive >= (uintnat)caml_prim_name_table.size) return 0;
  function = (c_primitive)caml_prim_table.contents[primitive];
  name = caml_prim_name_table.contents[primitive];
  if (name == NULL) return 0;
  for (mlsize_t index = 0;
       index < sizeof(actor_primitive_policy)
                 / sizeof(actor_primitive_policy[0]);
       index++) {
    const struct caml_actor_primitive_policy_entry *entry =
      &actor_primitive_policy[index];
    if (strcmp(name, entry->name) != 0) continue;
    return arity == entry->arity
      && entry->capability != CAML_ACTOR_PRIMITIVE_FORBIDDEN
      && entry->function != NULL
      && function == entry->function;
  }
  return 0;
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

enum caml_actor_spawn_status caml_actor_scheduler_prepare_root_closure(
  struct caml_actor_scheduler *scheduler, value closure,
  mlsize_t heap_quota_words,
  struct caml_actor_prepared_spawn **prepared)
{
  (void)scheduler;
  (void)closure;
  (void)heap_quota_words;
  if (prepared != NULL) *prepared = NULL;
  return CAML_ACTOR_SPAWN_UNSUPPORTED;
}

enum caml_actor_spawn_status caml_actor_scheduler_prepare_closure(
  struct caml_actor_scheduler *scheduler, value closure,
  mlsize_t heap_quota_words,
  struct caml_actor_prepared_spawn **prepared)
{
  return caml_actor_scheduler_prepare_root_closure(
    scheduler, closure, heap_quota_words, prepared);
}

uintnat caml_actor_scheduler_prepared_pid(
  const struct caml_actor_prepared_spawn *prepared)
{
  (void)prepared;
  return 0;
}

int caml_actor_scheduler_commit_prepared(
  struct caml_actor_prepared_spawn *prepared)
{
  (void)prepared;
  return 0;
}

void caml_actor_scheduler_abort_prepared(
  struct caml_actor_prepared_spawn *prepared)
{
  (void)prepared;
}

enum caml_actor_send_status caml_actor_scheduler_can_send(
  struct caml_actor_scheduler *scheduler, uintnat pid)
{
  (void)scheduler;
  (void)pid;
  return CAML_ACTOR_SEND_INVALID_CONTEXT;
}

enum caml_actor_send_status caml_actor_scheduler_prepare_send(
  struct caml_actor_scheduler *scheduler, uintnat pid,
  struct caml_actor_envelope *envelope,
  struct caml_actor_prepared_send **prepared)
{
  (void)scheduler;
  (void)pid;
  (void)envelope;
  if (prepared != NULL) *prepared = NULL;
  return CAML_ACTOR_SEND_INVALID_CONTEXT;
}

int caml_actor_scheduler_commit_send(
  struct caml_actor_prepared_send *prepared)
{
  (void)prepared;
  return 0;
}

void caml_actor_scheduler_abort_send(
  struct caml_actor_prepared_send *prepared)
{
  (void)prepared;
}

const struct caml_actor_envelope *
caml_actor_scheduler_peek_current_message(
  struct caml_actor_scheduler *scheduler)
{
  (void)scheduler;
  return NULL;
}

int caml_actor_scheduler_consume_current_message(
  struct caml_actor_scheduler *scheduler)
{
  (void)scheduler;
  return 0;
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

int caml_actor_scheduler_stats(
  const struct caml_actor_scheduler *scheduler,
  struct caml_actor_scheduler_stats *stats)
{
  (void)scheduler;
  if (stats != NULL) memset(stats, 0, sizeof(*stats));
  return 0;
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

uintnat caml_actor_scheduler_current_pid(void)
{
  return 0;
}

int caml_actor_scheduler_request_yield(void)
{
  return 0;
}

int caml_actor_scheduler_request_blocked(void)
{
  return 0;
}

int caml_actor_scheduler_request_unsupported(void)
{
  return 0;
}

int caml_actor_scheduler_request_heap_exhausted(void)
{
  return 0;
}

void caml_actor_scheduler_record_unsupported_opcode(uintnat opcode)
{
  (void)opcode;
}

void caml_actor_scheduler_record_unsupported_primitive(
  uintnat primitive, int arity)
{
  (void)primitive;
  (void)arity;
}

enum caml_actor_control_request
caml_actor_scheduler_take_control_request(void)
{
  return CAML_ACTOR_CONTROL_NONE;
}

int caml_actor_scheduler_primitive_allowed(uintnat primitive, int arity)
{
  (void)primitive;
  (void)arity;
  return 0;
}

#endif /* NATIVE_CODE */
