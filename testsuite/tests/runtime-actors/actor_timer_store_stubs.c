#define CAML_INTERNALS
#include <math.h>
#include "caml/actor_timer.h"
#include "caml/misc.h"

struct fake_clock { uint64_t now; int broken; uintnat count, ids[256]; };
static int now(void *p, uint64_t *n)
{
  struct fake_clock *f = p; *n = f->now; return !f->broken;
}
static int wait_until(void *p, uint64_t n)
{
  struct fake_clock *f = p; f->now = n; return 1;
}
static void ready(void *p, uintnat owner, uintnat id, uint64_t deadline)
{
  struct fake_clock *f = p;
  if (owner != 9 || deadline > f->now || f->count >= 256)
    caml_fatal_error("invalid timer expiry");
  f->ids[f->count++] = id;
}
#define CHECK(e) do { if (!(e)) caml_fatal_error("timer store: " #e); } while (0)
CAMLprim value caml_actor_test_timer_store(value unit)
{
  struct fake_clock f = {0};
  struct caml_actor_timer_backend b = {&f, now, wait_until};
  struct caml_actor_timers *t = caml_actor_timers_create(4, &b);
  struct caml_actor_timer_stats s;
  uintnat a, c, d, old;
  uint64_t deadline;
  (void)unit;
  CHECK(t != NULL);
  CHECK(caml_actor_timer_after(t, 9, -1., &a) == CAML_ACTOR_TIMER_DURATION);
  CHECK(caml_actor_timer_after(t, 9, NAN, &a) == CAML_ACTOR_TIMER_DURATION);
  CHECK(caml_actor_timer_after(t, 9, INFINITY, &a) == CAML_ACTOR_TIMER_DURATION);
  CHECK(caml_actor_timer_after(t, 9, 1e100, &a) == CAML_ACTOR_TIMER_DURATION);
  caml_actor_timers_test_fail_allocation(t);
  CHECK(caml_actor_timer_after(t, 9, 1., &a) == CAML_ACTOR_TIMER_UNAVAILABLE);
  caml_actor_timers_stats(t, &s); CHECK(s.count == 0 && s.pending == 0);
  CHECK(caml_actor_timer_after(t, 9, 1., &a) == CAML_ACTOR_TIMER_OK);
  CHECK(caml_actor_timer_after(t, 9, 1., &c) == CAML_ACTOR_TIMER_OK);
  CHECK(caml_actor_timer_after(t, 9, 2., &d) == CAML_ACTOR_TIMER_OK);
  CHECK(caml_actor_timer_after(t, 9, 3., &old) == CAML_ACTOR_TIMER_OK);
  CHECK(caml_actor_timer_after(t, 9, 0., &old) == CAML_ACTOR_TIMER_LIMIT);
  CHECK(caml_actor_timer_peek(t, 8, a, NULL) == CAML_ACTOR_TIMER_INVALID);
  CHECK(caml_actor_timer_peek(t, 9, a, &deadline) == CAML_ACTOR_TIMER_PENDING);
  CHECK(deadline == 1000000000);
  f.now = deadline - 1;
  CHECK(caml_actor_timers_poll(t, 64, ready, &f)); CHECK(f.count == 0);
  f.now++;
  CHECK(caml_actor_timers_poll(t, 1, ready, &f));
  CHECK(f.count == 1 && f.ids[0] == a);
  CHECK(caml_actor_timers_poll(t, 64, ready, &f));
  CHECK(f.count == 2 && f.ids[1] == c);
  caml_actor_timers_stats(t, &s); CHECK(s.count == 4 && s.pending == 2);
  caml_actor_timer_consume(t, 9, a, 0);
  CHECK(caml_actor_timer_peek(t, 9, a, NULL) == CAML_ACTOR_TIMER_INVALID);
  caml_actor_timer_consume(t, 9, d, 1);
  caml_actor_timers_retire(t, 9);
  caml_actor_timers_stats(t, &s); CHECK(s.count == 0 && s.pending == 0);
  for (int i = 0; i < 200; i++) {
    CHECK(caml_actor_timer_after(t, 9, 1e-12, &c) == CAML_ACTOR_TIMER_OK);
    CHECK(caml_actor_timer_peek(t, 9, c, &deadline) == CAML_ACTOR_TIMER_PENDING);
    CHECK(deadline == f.now + 1);
    caml_actor_timer_consume(t, 9, c, 1);
    CHECK(caml_actor_timer_peek(t, 9, a, NULL) == CAML_ACTOR_TIMER_INVALID);
  }
  CHECK(caml_actor_timer_after(t, 9, -0., &a) == CAML_ACTOR_TIMER_OK);
  CHECK(caml_actor_timer_peek(t, 9, a, NULL) == CAML_ACTOR_TIMER_OK);
  caml_actor_timers_destroy(t);
  t = caml_actor_timers_create(4, &b);
  CHECK(caml_actor_timer_after(t, 9, 0., &c) == CAML_ACTOR_TIMER_OK);
  CHECK(a != c && caml_actor_timer_peek(t, 9, a, NULL) == CAML_ACTOR_TIMER_INVALID);
  f.now = UINT64_MAX;
  CHECK(caml_actor_timer_after(t, 9, 1., &a) == CAML_ACTOR_TIMER_DURATION);
  f.now--;
  CHECK(caml_actor_timer_peek(t, 9, c, NULL) == CAML_ACTOR_TIMER_CLOCK_ERROR);
  caml_actor_timers_destroy(t);
  t = caml_actor_timers_create(4, &b);
  caml_actor_timers_test_exhaust_identity(t);
  CHECK(caml_actor_timer_after(t, 9, 0., &a) == CAML_ACTOR_TIMER_UNAVAILABLE);
  caml_actor_timers_destroy(t);
  return Val_unit;
}
