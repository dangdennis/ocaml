#define CAML_INTERNALS

#include "caml/actor_scheduler.h"
#include "caml/callback.h"
#include "caml/codefrag.h"
#include "caml/domain_state.h"
#include "caml/fix_code.h"
#include "caml/instruct.h"
#include "caml/memory.h"
#include "caml/misc.h"
#include "caml/mlvalues.h"
#include "caml/prims.h"
#include "caml/signals.h"

static opcode_t infinite_code[2];
static opcode_t exit_code[2];
static opcode_t poison_code[4];
static opcode_t poptrap_code[5];
static int code_ready;
static int poison_entries;

CAMLprim value caml_actor_test_scheduler_poison(value unit)
{
  CAMLparam1(unit);
  poison_entries++;
  CAMLreturn(Val_unit);
}

static int poison_primitive_index(void)
{
  for (int index = 0; index < caml_prim_table.size; index++) {
    if (Primitive1(index) == caml_actor_test_scheduler_poison) return index;
  }
  caml_fatal_error("scheduler poison primitive is not registered");
}

static void prepare_code(void)
{
  int primitive;

  if (code_ready) return;
  primitive = poison_primitive_index();

  caml_set_instruction(&infinite_code[0], BRANCH);
  infinite_code[1] = -1;

  caml_set_instruction(&exit_code[0], CONST1);
  caml_set_instruction(&exit_code[1], STOP);

  caml_set_instruction(&poison_code[0], CONST0);
  caml_set_instruction(&poison_code[1], C_CALL1);
  poison_code[2] = primitive;
  caml_set_instruction(&poison_code[3], STOP);

  caml_set_instruction(&poptrap_code[0], PUSHTRAP);
  poptrap_code[1] = 2;
  caml_set_instruction(&poptrap_code[2], POPTRAP);
  caml_set_instruction(&poptrap_code[3], CONST0);
  caml_set_instruction(&poptrap_code[4], STOP);

  caml_register_code_fragment(
    (char *)infinite_code, (char *)(infinite_code + 2),
    DIGEST_IGNORE, NULL);
  caml_register_code_fragment(
    (char *)exit_code, (char *)(exit_code + 2), DIGEST_IGNORE, NULL);
  caml_register_code_fragment(
    (char *)poison_code, (char *)(poison_code + 4), DIGEST_IGNORE, NULL);
  caml_register_code_fragment(
    (char *)poptrap_code, (char *)(poptrap_code + 5),
    DIGEST_IGNORE, NULL);
  code_ready = 1;
}

static void require_spawn(enum caml_actor_spawn_status status)
{
  if (status != CAML_ACTOR_SPAWN_OK) {
    caml_fatal_error("deterministic scheduler spawn failed");
  }
}

static struct caml_actor_snapshot require_snapshot(
  struct caml_actor_scheduler *scheduler, uintnat pid)
{
  struct caml_actor_snapshot snapshot;

  if (caml_actor_scheduler_snapshot(scheduler, pid, &snapshot)
      != CAML_ACTOR_PID_PRESENT) {
    caml_fatal_error("deterministic scheduler PID lookup failed");
  }
  return snapshot;
}

static void check_fairness(void)
{
  struct caml_actor_scheduler *scheduler =
    caml_actor_scheduler_create(2, 7);
  struct caml_actor_snapshot root_snapshot;
  struct caml_actor_snapshot child_snapshot;
  uintnat root;
  uintnat child;

  if (scheduler == NULL) caml_fatal_error("scheduler creation failed");
  require_spawn(caml_actor_scheduler_spawn_root_code(
    scheduler, infinite_code, sizeof(infinite_code), Atom(0), 0, 64, &root));
  require_spawn(caml_actor_scheduler_spawn_code(
    scheduler, infinite_code, sizeof(infinite_code), Atom(0), 0, 64, &child));
  if (root != 0 || child == root) {
    caml_fatal_error("invalid root or child PID");
  }

  for (uintnat dispatch = 0; dispatch < 128; dispatch++) {
    struct caml_actor_step step = caml_actor_scheduler_step(scheduler);
    uintnat expected = dispatch % 2 == 0 ? root : child;

    if (step.reason != CAML_ACTOR_STEP_REDUCTIONS
        || step.pid != expected) {
      caml_fatal_error("round-robin scheduler trace diverged");
    }
  }

  root_snapshot = require_snapshot(scheduler, root);
  child_snapshot = require_snapshot(scheduler, child);
  if (root_snapshot.lifecycle != CAML_ACTOR_LIFECYCLE_RUNNABLE
      || child_snapshot.lifecycle != CAML_ACTOR_LIFECYCLE_RUNNABLE
      || root_snapshot.dispatches != 64
      || child_snapshot.dispatches != 64
      || root_snapshot.reduction_stops != 64
      || child_snapshot.reduction_stops != 64) {
    caml_fatal_error("CPU-bound actors did not make equal progress");
  }
  caml_actor_scheduler_destroy(scheduler);
}

static void check_host_action_deferral(void)
{
  struct caml_actor_scheduler *scheduler =
    caml_actor_scheduler_create(2, 7);
  struct caml_actor_snapshot snapshot;
  struct caml_actor_step step;
  uintnat root;

  if (scheduler == NULL) caml_fatal_error("scheduler creation failed");
  require_spawn(caml_actor_scheduler_spawn_root_code(
    scheduler, poptrap_code, sizeof(poptrap_code), Atom(0), 0, 64, &root));
  caml_actor_scheduler_test_request_minor_gc_after_switch(scheduler);
  step = caml_actor_scheduler_step(scheduler);
  snapshot = require_snapshot(scheduler, root);
  if (step.reason != CAML_ACTOR_STEP_HOST_ACTION || step.pid != root
      || !Caml_check_gc_interrupt(Caml_state)
      || snapshot.lifecycle != CAML_ACTOR_LIFECYCLE_RUNNABLE
      || snapshot.dispatches != 1 || snapshot.reduction_stops != 0) {
    caml_fatal_error("actor slice processed a host pending action");
  }
  caml_process_pending_actions();
  step = caml_actor_scheduler_step(scheduler);
  if (step.reason != CAML_ACTOR_STEP_EXITED || step.pid != root) {
    caml_fatal_error("POPTRAP did not resume after host action deferral");
  }
  caml_actor_scheduler_destroy(scheduler);
}

static void check_stale_pid_and_primitive_fence(void)
{
  struct caml_actor_scheduler *scheduler =
    caml_actor_scheduler_create(3, 4);
  struct caml_actor_snapshot snapshot;
  struct caml_actor_step step;
  uintnat old_pid;
  uintnat new_pid;
  uintnat poison_pid;

  if (scheduler == NULL) caml_fatal_error("scheduler creation failed");
  require_spawn(caml_actor_scheduler_spawn_code(
    scheduler, exit_code, sizeof(exit_code), Atom(0), 0, 64, &old_pid));
  step = caml_actor_scheduler_step(scheduler);
  if (step.reason != CAML_ACTOR_STEP_EXITED || step.pid != old_pid) {
    caml_fatal_error("finite actor did not exit");
  }
  snapshot = require_snapshot(scheduler, old_pid);
  if (snapshot.lifecycle != CAML_ACTOR_LIFECYCLE_EXITED
      || !caml_actor_scheduler_retire(scheduler, old_pid)) {
    caml_fatal_error("finite actor was not retired");
  }

  require_spawn(caml_actor_scheduler_spawn_code(
    scheduler, exit_code, sizeof(exit_code), Atom(0), 0, 64, &new_pid));
  if (new_pid == old_pid
      || (new_pid & CAML_ACTOR_PID_INDEX_MASK)
           != (old_pid & CAML_ACTOR_PID_INDEX_MASK)
      || caml_actor_scheduler_snapshot(scheduler, old_pid, &snapshot)
           != CAML_ACTOR_PID_STALE
      || caml_actor_scheduler_retire(scheduler, old_pid)) {
    caml_fatal_error("stale PID revived after slot reuse");
  }
  snapshot = require_snapshot(scheduler, new_pid);
  if (snapshot.lifecycle != CAML_ACTOR_LIFECYCLE_RUNNABLE) {
    caml_fatal_error("replacement PID is not runnable");
  }
  step = caml_actor_scheduler_step(scheduler);
  if (step.reason != CAML_ACTOR_STEP_EXITED || step.pid != new_pid
      || !caml_actor_scheduler_retire(scheduler, new_pid)) {
    caml_fatal_error("replacement actor did not exit and retire");
  }

  poison_entries = 0;
  require_spawn(caml_actor_scheduler_spawn_code(
    scheduler, poison_code, sizeof(poison_code), Atom(0), 0, 64,
    &poison_pid));
  step = caml_actor_scheduler_step(scheduler);
  snapshot = require_snapshot(scheduler, poison_pid);
  if (step.reason != CAML_ACTOR_STEP_FAILED || step.pid != poison_pid
      || snapshot.lifecycle != CAML_ACTOR_LIFECYCLE_FAILED
      || snapshot.failure != CAML_ACTOR_FAILURE_UNSUPPORTED
      || poison_entries != 0) {
    caml_fatal_error("denied actor primitive was entered");
  }
  if (!caml_actor_scheduler_retire(scheduler, poison_pid)
      || caml_actor_scheduler_step(scheduler).reason
           != CAML_ACTOR_STEP_IDLE) {
    caml_fatal_error("scheduler did not become idle");
  }
  if (caml_actor_scheduler_snapshot(scheduler, old_pid, &snapshot)
      != CAML_ACTOR_PID_STALE) {
    caml_fatal_error("old PID stopped being stale");
  }
  caml_actor_scheduler_destroy(scheduler);
}

static void check_host_stack_refresh(value grow_host_stack)
{
  struct caml_actor_scheduler *scheduler =
    caml_actor_scheduler_create(2, 4);
  struct stack_info *stack_before = Caml_state->current_stack;
  struct caml_actor_step step;
  value callback_result;
  uintnat root;

  if (scheduler == NULL) caml_fatal_error("scheduler creation failed");
  require_spawn(caml_actor_scheduler_spawn_root_code(
    scheduler, exit_code, sizeof(exit_code), Atom(0), 0, 64, &root));
  callback_result = caml_callback_exn(grow_host_stack, Val_unit);
  if (Is_exception_result(callback_result)) {
    caml_fatal_error("host stack-growth callback failed");
  }
  if (Caml_state->current_stack == stack_before) {
    caml_fatal_error("host stack-growth callback did not replace the stack");
  }
  step = caml_actor_scheduler_step(scheduler);
  if (step.reason != CAML_ACTOR_STEP_EXITED || step.pid != root) {
    caml_fatal_error("scheduler did not refresh the relocated host stack");
  }
  caml_actor_scheduler_destroy(scheduler);
}

CAMLprim value caml_actor_test_deterministic_scheduler(value grow_host_stack)
{
  CAMLparam1(grow_host_stack);
  caml_domain_state *domain = Caml_state;
  value *young_ptr;
  uintnat allocated_words;
  uintnat allocated_words_direct;
  struct stack_info *host_stack;
  struct caml__roots_block *host_roots;
  struct caml_exception_context *host_raise;
  intnat host_trap;

  if (caml_check_pending_actions()) caml_process_pending_actions();
  prepare_code();
  check_host_action_deferral();
  check_host_stack_refresh(grow_host_stack);
  young_ptr = domain->young_ptr;
  allocated_words = domain->allocated_words;
  allocated_words_direct = domain->allocated_words_direct;
  host_stack = domain->current_stack;
  host_roots = domain->local_roots;
  host_raise = domain->external_raise;
  host_trap = domain->trap_sp_off;
  check_fairness();
  check_stale_pid_and_primitive_fence();

  if (domain->actor_scheduler != NULL || domain->actor_heap != NULL
      || domain->current_stack != host_stack
      || domain->local_roots != host_roots
      || domain->external_raise != host_raise
      || domain->trap_sp_off != host_trap
      || domain->gc_regs != NULL
      || domain->young_ptr != young_ptr
      || domain->allocated_words != allocated_words
      || domain->allocated_words_direct != allocated_words_direct) {
    caml_fatal_error("scheduler leaked actor or host runtime state");
  }
  CAMLreturn(Val_unit);
}
