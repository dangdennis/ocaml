#define CAML_INTERNALS
#include "caml/actor_scheduler.h"
#include "caml/misc.h"
static uint64_t ticks, increment_ticks;
static int waits, mode, calls;
static int fake_now(void *context, uint64_t *now)
{
  (void)context;
  calls++;
  if (mode == 3 && calls > 3) return 0;
  if (mode == 4 && calls > 3) { *now = 0; return 1; }
  *now = ticks;
  ticks += increment_ticks;
  return 1;
}
static int fake_wait(void *context, uint64_t deadline)
{
  (void)context;
  waits++;
  if (mode == 2) return -1;
  if (mode == 6) return 0;
  if (mode == 7) return 1;
  if (mode == 1 && waits == 1) return 0;
  if (ticks < deadline) ticks = deadline;
  return 1;
}
CAMLprim value caml_actor_test_timer_clock(value test_mode)
{
  struct caml_actor_timer_backend backend = {NULL, fake_now, fake_wait};
  mode = Int_val(test_mode); waits = 0; calls = 0;
  ticks = mode == 4 ? 1000000 : 0;
  increment_ticks = mode == 5 || mode == 3 || mode == 4 ? 1000000 : 0;
  caml_actor_scheduler_test_timer_backend(&backend);
  return Val_unit;
}
CAMLprim value caml_actor_test_timer_waits(value unit)
{
  (void)unit; return Val_int(waits);
}
CAMLprim value caml_actor_test_timer_real(value unit)
{
  (void)unit; caml_actor_scheduler_test_timer_backend(NULL); return Val_unit;
}
