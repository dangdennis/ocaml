#define CAML_INTERNALS

#include "caml/alloc.h"
#include "caml/backtrace.h"
#include "caml/callback.h"
#include "caml/codefrag.h"
#include "caml/domain_state.h"
#include "caml/fail.h"
#include "caml/fiber.h"
#include "caml/fix_code.h"
#include "caml/interp.h"
#include "caml/instruct.h"
#include "caml/memory.h"
#include "caml/mlvalues.h"
#include "caml/signals.h"

extern value caml_gc_compaction(value);
extern value caml_gc_minor(value);

static int primitive_depth;
static int primitive_entries;
static int primitive_exits;

static opcode_t exact_reduction_code[2];
static int exact_reduction_code_ready;

static value clear_stack_parent(caml_domain_state *domain_state)
{
  struct stack_info *parent = Stack_parent(domain_state->current_stack);

  if (parent == NULL) {
    return Val_unit;
  } else {
    value cont = caml_alloc_2(Cont_tag, Val_ptr(parent), Val_long(0));
    Stack_parent(domain_state->current_stack) = NULL;
    return cont;
  }
}

static void restore_stack_parent(caml_domain_state *domain_state, value cont)
{
  CAMLassert(Stack_parent(domain_state->current_stack) == NULL);
  if (Is_block(cont)) {
    struct stack_info *parent = Ptr_val(caml_continuation_use(cont));
    Stack_parent(domain_state->current_stack) = parent;
  }
}

static value make_observation(int raised, value data)
{
  CAMLparam1(data);
  CAMLlocal3(observation, pair, trace);

  if (raised) {
    trace = caml_get_exception_raw_backtrace(Val_unit);
    pair = caml_alloc_2(0, data, trace);
    observation = caml_alloc_1(1, pair);
  } else {
    observation = caml_alloc_1(0, data);
  }
  CAMLreturn(observation);
}

CAMLprim value caml_actor_test_allocating_primitive(value count)
{
  CAMLparam1(count);
  CAMLlocal1(list);
  int n = Int_val(count);

  primitive_depth++;
  primitive_entries++;
  list = Val_emptylist;
  for (int i = 0; i < n; i++) {
    list = caml_alloc_2(0, Val_long(i), list);
  }
  primitive_exits++;
  primitive_depth--;

  CAMLreturn(Val_long(n == 0 ? 0 : n + Long_val(Field(list, 0))));
}

CAMLprim value caml_actor_test_run_uninterrupted(value closure)
{
  CAMLparam1(closure);
  CAMLlocal1(result);
  value callback_result = caml_callback_exn(closure, Val_unit);

  if (Is_exception_result(callback_result)) {
    result = make_observation(1, Extract_exception(callback_result));
  } else {
    result = make_observation(0, callback_result);
  }
  CAMLreturn(result);
}

CAMLprim value caml_actor_test_exact_reductions(value unit)
{
  CAMLparam1(unit);
  struct caml_bytecode_state state;
  caml_domain_state *domain_state = Caml_state;
  struct caml__roots_block *roots_before = domain_state->local_roots;
  struct caml_exception_context *raise_before = domain_state->external_raise;
  value *sp_before = domain_state->current_stack->sp;
  intnat trap_before = domain_state->trap_sp_off;
  enum caml_bytecode_stop_reason reason;
  value result = Val_unit;

  if (!exact_reduction_code_ready) {
    caml_set_instruction(exact_reduction_code, CONST1);
    caml_set_instruction(exact_reduction_code + 1, STOP);
    caml_register_code_fragment(
      (char *)exact_reduction_code,
      (char *)exact_reduction_code + sizeof(exact_reduction_code),
      DIGEST_IGNORE, NULL);
    exact_reduction_code_ready = 1;
  }

  caml_bytecode_state_init(
    &state, exact_reduction_code, sizeof(exact_reduction_code),
    Val_unit, 0);

  reason = caml_bytecode_interpreter_slice(&state, 0, &result);
  if (reason != CAML_BYTECODE_STOP_REDUCTIONS
      || domain_state->current_stack->sp[0] != Val_int(0)
      || (code_t)domain_state->current_stack->sp[1] != exact_reduction_code) {
    caml_fatal_error("zero budget executed a bytecode instruction");
  }

  reason = caml_bytecode_interpreter_slice(&state, 1, &result);
  if (reason != CAML_BYTECODE_STOP_REDUCTIONS
      || domain_state->current_stack->sp[0] != Val_int(1)
      || (code_t)domain_state->current_stack->sp[1]
           != exact_reduction_code + 1) {
    caml_fatal_error("one budget did not execute exactly one instruction");
  }

  caml_set_action_pending(domain_state);
  reason = caml_bytecode_interpreter_slice(&state, 0, &result);
  if (reason != CAML_BYTECODE_STOP_REDUCTIONS
      || domain_state->action_pending
      || domain_state->current_stack->sp[0] != Val_int(1)
      || (code_t)domain_state->current_stack->sp[1]
           != exact_reduction_code + 1) {
    caml_fatal_error("reduction stop did not drain pending actions");
  }

  reason = caml_bytecode_interpreter_slice(&state, 1, &result);
  if (reason != CAML_BYTECODE_STOP_VALUE || result != Val_int(1)
      || domain_state->current_stack->sp != sp_before
      || domain_state->local_roots != roots_before
      || domain_state->external_raise != raise_before
      || domain_state->trap_sp_off != trap_before) {
    caml_fatal_error("invalid terminal bytecode reduction state");
  }

  CAMLreturn(Val_unit);
}

CAMLprim value caml_actor_test_run_sliced(value budget_value, value closure)
{
  CAMLparam2(budget_value, closure);
  CAMLlocal3(answer, cont, observation);
  struct caml_bytecode_state state;
  caml_domain_state *domain_state = Caml_state;
  enum caml_bytecode_stop_reason reason;
  intnat budget_int = Long_val(budget_value);
  uintnat budget;
  value result = Val_unit;
  int stops = 0;
  int forced_gcs = 0;
  int entries_before = primitive_entries;
  int exits_before = primitive_exits;
  const int narg = 1;
  const intnat required = narg + 3 + 4 + Stack_threshold_words;
  asize_t initial_stack_words =
    Stack_high(domain_state->current_stack)
    - Stack_base(domain_state->current_stack);

  if (budget_int <= 0) caml_invalid_argument("run_sliced: non-positive budget");
  budget = budget_int;

  if (domain_state->current_stack->sp - required
      < Stack_base(domain_state->current_stack)) {
    if (!caml_try_realloc_stack(required)) caml_raise_stack_overflow();
  }

  domain_state->current_stack->sp -= narg + 3;
  domain_state->current_stack->sp[0] = Val_unit;
  domain_state->current_stack->sp[narg] =
    (value)caml_bytecode_callback_code();
  domain_state->current_stack->sp[narg + 1] = Val_unit;
  domain_state->current_stack->sp[narg + 2] = Val_long(0);

  cont = clear_stack_parent(domain_state);
  caml_update_young_limit_after_c_call(domain_state);
  caml_bytecode_state_init(
    &state, Code_val(closure), 0, closure, narg - 1);

  /* A zero-reduction resume must stop without executing an instruction. */
  {
    struct caml__roots_block *roots_before = domain_state->local_roots;
    struct caml_exception_context *raise_before =
      domain_state->external_raise;
    intnat trap_before = domain_state->trap_sp_off;
    reason = caml_bytecode_interpreter_slice(&state, 0, &result);
    if (reason != CAML_BYTECODE_STOP_REDUCTIONS
        || roots_before != domain_state->local_roots
        || raise_before != domain_state->external_raise
        || trap_before != domain_state->trap_sp_off
        || primitive_depth != 0) {
      caml_fatal_error("invalid zero-reduction bytecode stop");
    }
  }

  do {
    struct caml__roots_block *roots_before = domain_state->local_roots;
    struct caml_exception_context *raise_before =
      domain_state->external_raise;
    intnat trap_before = domain_state->trap_sp_off;

    reason = caml_bytecode_interpreter_slice(&state, budget, &result);
    if (roots_before != domain_state->local_roots
        || raise_before != domain_state->external_raise
        || trap_before != domain_state->trap_sp_off
        || primitive_depth != 0) {
      caml_fatal_error("resumable bytecode leaked C invocation state");
    }

    if (reason == CAML_BYTECODE_STOP_REDUCTIONS) {
      stops++;
      if (stops == 1 || stops == 97 || stops == 509) {
        caml_gc_compaction(Val_unit);
        forced_gcs++;
      } else if (stops % 31 == 0) {
        caml_gc_minor(Val_unit);
        forced_gcs++;
      }
    }
  } while (reason == CAML_BYTECODE_STOP_REDUCTIONS);

  if (reason == CAML_BYTECODE_STOP_EXCEPTION) {
    domain_state->current_stack->sp += narg + 3;
  }
  restore_stack_parent(domain_state, cont);

  observation = make_observation(
    reason == CAML_BYTECODE_STOP_EXCEPTION, result);
  answer = caml_alloc_tuple(6);
  Store_field(answer, 0, observation);
  Store_field(answer, 1, Val_long(stops));
  Store_field(answer, 2, Val_long(forced_gcs));
  Store_field(answer, 3, Val_long(primitive_entries - entries_before));
  Store_field(answer, 4, Val_long(primitive_exits - exits_before));
  Store_field(
    answer, 5,
    Val_bool(
      Stack_high(domain_state->current_stack)
      - Stack_base(domain_state->current_stack) > initial_stack_words));
  CAMLreturn(answer);
}
