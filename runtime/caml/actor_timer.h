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

/* Scheduler-owned timer storage and host event backend. */
#ifndef CAML_ACTOR_TIMER_H
#define CAML_ACTOR_TIMER_H
#ifdef CAML_INTERNALS
#include <stdint.h>
#include "mlvalues.h"

#define CAML_ACTOR_DEFAULT_TIMERS 65536
#define CAML_ACTOR_TIMER_BATCH 64
struct caml_actor_timers;
struct caml_actor_timer_backend {
  void *context;
  int (*now)(void *, uint64_t *);
  /* Return 1 on wake, 0 on interruption, -1 on fatal error. */
  int (*wait)(void *, uint64_t);
};
enum caml_actor_timer_status {
  CAML_ACTOR_TIMER_OK,
  CAML_ACTOR_TIMER_DURATION,
  CAML_ACTOR_TIMER_LIMIT,
  CAML_ACTOR_TIMER_UNAVAILABLE,
  CAML_ACTOR_TIMER_INVALID,
  CAML_ACTOR_TIMER_UNSUPPORTED,
  CAML_ACTOR_TIMER_PENDING,
  CAML_ACTOR_TIMER_CLOCK_ERROR
};
struct caml_actor_timer_stats {
  uintnat count, peak, pending, expired, cancelled, quota_failures, limit;
};
CAMLextern struct caml_actor_timers *caml_actor_timers_create(
  uintnat limit, const struct caml_actor_timer_backend *backend);
CAMLextern void caml_actor_timers_destroy(struct caml_actor_timers *timers);
CAMLextern enum caml_actor_timer_status caml_actor_timer_after(
  struct caml_actor_timers *, uintnat owner, double seconds, uintnat *id);
CAMLextern enum caml_actor_timer_status caml_actor_timer_peek(
  struct caml_actor_timers *, uintnat owner, uintnat id, uint64_t *deadline);
CAMLextern uint64_t caml_actor_timer_deadline(
  struct caml_actor_timers *, uintnat owner, uintnat id);
CAMLextern void caml_actor_timer_consume(
  struct caml_actor_timers *, uintnat owner, uintnat id, int cancelled);
/* Expire up to budget timers; callback contains scalar identities only. */
CAMLextern int caml_actor_timers_poll(
  struct caml_actor_timers *, uintnat budget,
  void (*ready)(void *, uintnat, uintnat, uint64_t), void *context);
CAMLextern int caml_actor_timers_wait(struct caml_actor_timers *, uint64_t);
CAMLextern void caml_actor_timers_retire(struct caml_actor_timers *, uintnat);
CAMLextern void caml_actor_timers_stats(const struct caml_actor_timers *,
                                     struct caml_actor_timer_stats *);
/* Host-only test seams, never admitted as actor primitives. */
CAMLextern void caml_actor_timers_test_signal_before_wait(void);
CAMLextern void caml_actor_timers_test_fail_allocation(
  struct caml_actor_timers *);
CAMLextern void caml_actor_timers_test_exhaust_identity(
  struct caml_actor_timers *);
#endif
#endif
