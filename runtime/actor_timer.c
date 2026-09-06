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
#include <errno.h>
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "caml/actor_timer.h"
#include "caml/misc.h"

/* Actor worlds are restricted to a single live Domain. Allocate IDs across
   worlds, not from a per-world counter, and permanently stop before wrap. */
static uintnat next_timer_id = 1;
struct timer_record {
  uintnat id, owner, heap_index;
  uint64_t deadline;
};
struct caml_actor_timers {
  struct timer_record *records;
  uintnat *heap;
  uintnat capacity, heap_count;
  struct caml_actor_timer_stats stats;
  struct caml_actor_timer_backend backend;
  uint64_t last_now;
  int sampled, failed, fail_allocation, exhausted;
};

static void increment(uintnat *counter)
{
  if (*counter < (uintnat)Max_long) ++*counter;
}

#if defined(__linux__) && !defined(NATIVE_CODE)
static int real_now(void *context, uint64_t *now)
{
  struct timespec ts;
  (void)context;
  if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0 || ts.tv_sec < 0
      || (uint64_t)ts.tv_sec > (UINT64_MAX - 999999999) / 1000000000)
    return 0;
  *now = (uint64_t)ts.tv_sec * 1000000000 + (uint64_t)ts.tv_nsec;
  return 1;
}
static int real_wait(void *context, uint64_t deadline)
{
  struct timespec ts;
  int status;
  (void)context;
  ts.tv_sec = (time_t)(deadline / 1000000000);
  ts.tv_nsec = (long)(deadline % 1000000000);
  if (ts.tv_sec < 0 || (uint64_t)ts.tv_sec != deadline / 1000000000)
    return -1;
  status = clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL);
  return status == 0 ? 1 : status == EINTR ? 0 : -1;
}
#endif

struct caml_actor_timers *caml_actor_timers_create(
  uintnat limit, const struct caml_actor_timer_backend *backend)
{
  struct caml_actor_timers *t;
  if (limit == 0 || limit > (uintnat)Max_long
      || limit > SIZE_MAX / sizeof(struct timer_record)
      || limit > SIZE_MAX / sizeof(uintnat)) return NULL;
  t = calloc(1, sizeof(*t));
  if (t == NULL) return NULL;
  t->stats.limit = limit;
  if (backend != NULL) t->backend = *backend;
#if defined(__linux__) && !defined(NATIVE_CODE)
  else {
    t->backend.now = real_now;
    t->backend.wait = real_wait;
  }
#endif
  return t;
}
void caml_actor_timers_destroy(struct caml_actor_timers *t)
{
  if (t == NULL) return;
  free(t->records);
  free(t->heap);
  free(t);
}
static int sample(struct caml_actor_timers *t, uint64_t *now)
{
  if (t->failed) return 0;
  if (t->backend.now == NULL || !t->backend.now(t->backend.context, now)
      || (t->sampled && *now < t->last_now)) {
    t->failed = 1;
    return 0;
  }
  t->sampled = 1;
  t->last_now = *now;
  return 1;
}
static int earlier(struct caml_actor_timers *t, uintnat a, uintnat b)
{
  struct timer_record *x = &t->records[a], *y = &t->records[b];
  return x->deadline < y->deadline
    || (x->deadline == y->deadline && x->id < y->id);
}
static void swap(struct caml_actor_timers *t, uintnat a, uintnat b)
{
  uintnat tmp = t->heap[a];
  t->heap[a] = t->heap[b]; t->heap[b] = tmp;
  t->records[t->heap[a]].heap_index = a;
  t->records[t->heap[b]].heap_index = b;
}
static void up(struct caml_actor_timers *t, uintnat i)
{
  while (i > 0 && earlier(t, t->heap[i], t->heap[(i-1)/2])) {
    swap(t, i, (i-1)/2); i = (i-1)/2;
  }
}
static void remove_heap(struct caml_actor_timers *t, uintnat i)
{
  uintnat removed = t->heap[i];
  --t->heap_count;
  if (i < t->heap_count) {
    t->heap[i] = t->heap[t->heap_count];
    t->records[t->heap[i]].heap_index = i;
    if (i > 0 && earlier(t, t->heap[i], t->heap[(i-1)/2])) up(t, i);
    else {
      while (i * 2 + 1 < t->heap_count) {
        uintnat child = i * 2 + 1;
        if (child+1 < t->heap_count
            && earlier(t, t->heap[child+1], t->heap[child])) child++;
        if (!earlier(t, t->heap[child], t->heap[i])) break;
        swap(t, i, child); i = child;
      }
    }
  }
  t->records[removed].heap_index = (uintnat)-1;
  --t->stats.pending;
}
static struct timer_record *lookup(struct caml_actor_timers *t,
                                  uintnat owner, uintnat id)
{
  if (id == 0) return NULL;
  for (uintnat i = 0; i < t->capacity; i++) {
    if (t->records[i].id == id && t->records[i].owner == owner)
      return &t->records[i];
  }
  return NULL;
}
static int grow(struct caml_actor_timers *t)
{
  uintnat capacity = t->capacity == 0 ? 16 : t->capacity * 2;
  struct timer_record *records;
  uintnat *heap;
  if (capacity > t->stats.limit || capacity < t->capacity)
    capacity = t->stats.limit;
  if (t->fail_allocation) { t->fail_allocation = 0; return 0; }
  records = calloc(capacity, sizeof(*records));
  heap = malloc(capacity * sizeof(*heap));
  if (records == NULL || heap == NULL) {
    free(records); free(heap); return 0;
  }
  if (t->capacity > 0) {
    memcpy(records, t->records, t->capacity * sizeof(*records));
    memcpy(heap, t->heap, t->heap_count * sizeof(*heap));
  }
  free(t->records); free(t->heap);
  t->records = records; t->heap = heap; t->capacity = capacity;
  return 1;
}
/* Compute ceil(seconds * 10^9) exactly for the binary64 input. Multiplying
   doubles first can round DOWN by many nanoseconds at large durations. Split
   the 53-bit significand product into two 64-bit limbs (also portable to MSVC). */
static int duration_ticks(double seconds, uint64_t *duration)
{
  int exponent, shift;
  uint64_t significand, low_product, high_product, low, high, ticks, remainder;
  if (!isfinite(seconds) || seconds < 0.) return 0;
  if (seconds == 0.) { *duration = 0; return 1; }
  significand = (uint64_t)ldexp(frexp(seconds, &exponent), 53);
  shift = 53 - exponent;
  if (shift <= 0) return 0;
  low_product = (significand & UINT32_MAX) * UINT64_C(1000000000);
  high_product = (significand >> 32) * UINT64_C(1000000000);
  low = low_product + (high_product << 32);
  high = (high_product >> 32) + (low < low_product);
  if (shift >= 128) { *duration = 1; return 1; }
  if (shift >= 64) {
    unsigned upper_shift = (unsigned)shift - 64;
    ticks = high >> upper_shift;
    remainder = low | (high & ((UINT64_C(1) << upper_shift) - 1));
  } else {
    if ((high >> shift) != 0) return 0;
    ticks = (low >> shift) | (high << (64 - shift));
    remainder = low & ((UINT64_C(1) << shift) - 1);
  }
  if (remainder != 0) {
    if (ticks == UINT64_MAX) return 0;
    ticks++;
  }
  *duration = ticks;
  return 1;
}

enum caml_actor_timer_status caml_actor_timer_after(
  struct caml_actor_timers *t, uintnat owner, double seconds, uintnat *id)
{
  uint64_t now, duration;
  uintnat index;
  struct timer_record *r;
  if (!duration_ticks(seconds, &duration)) return CAML_ACTOR_TIMER_DURATION;
  if (t->backend.now == NULL || t->backend.wait == NULL)
    return CAML_ACTOR_TIMER_UNSUPPORTED;
  if (!sample(t, &now)) return CAML_ACTOR_TIMER_CLOCK_ERROR;
  if (duration > UINT64_MAX - now) return CAML_ACTOR_TIMER_DURATION;
  if (t->stats.count == t->stats.limit) {
    increment(&t->stats.quota_failures); return CAML_ACTOR_TIMER_LIMIT;
  }
  if (t->exhausted || next_timer_id > (uintnat)Max_long)
    return CAML_ACTOR_TIMER_UNAVAILABLE;
  if (t->stats.count == t->capacity && !grow(t))
    return CAML_ACTOR_TIMER_UNAVAILABLE;
  for (index = 0; t->records[index].id != 0; index++) { }
  r = &t->records[index];
  r->id = next_timer_id++;
  r->owner = owner;
  r->deadline = now + duration;
  r->heap_index = t->heap_count;
  t->heap[t->heap_count++] = index;
  up(t, r->heap_index);
  t->stats.count++; t->stats.pending++;
  if (t->stats.count > t->stats.peak) t->stats.peak = t->stats.count;
  *id = r->id;
  return CAML_ACTOR_TIMER_OK;
}
enum caml_actor_timer_status caml_actor_timer_peek(
  struct caml_actor_timers *t, uintnat owner, uintnat id, uint64_t *deadline)
{
  struct timer_record *r = lookup(t, owner, id);
  uint64_t now;
  if (r == NULL) return CAML_ACTOR_TIMER_INVALID;
  if (!sample(t, &now)) return CAML_ACTOR_TIMER_CLOCK_ERROR;
  if (deadline != NULL) *deadline = r->deadline;
  return now >= r->deadline ? CAML_ACTOR_TIMER_OK : CAML_ACTOR_TIMER_PENDING;
}
uint64_t caml_actor_timer_deadline(struct caml_actor_timers *t,
                                  uintnat owner, uintnat id)
{
  struct timer_record *r = lookup(t, owner, id);
  CAMLassert(r != NULL);
  return r->deadline;
}
void caml_actor_timer_consume(struct caml_actor_timers *t,
                             uintnat owner, uintnat id, int cancelled)
{
  struct timer_record *r = lookup(t, owner, id);
  CAMLassert(r != NULL);
  if (r->heap_index != (uintnat)-1) {
    remove_heap(t, r->heap_index);
    if (!cancelled) increment(&t->stats.expired);
  }
  if (cancelled) increment(&t->stats.cancelled);
  r->id = 0; --t->stats.count;
}
int caml_actor_timers_poll(struct caml_actor_timers *t, uintnat budget,
  void (*ready)(void *, uintnat, uintnat, uint64_t), void *context)
{
  uint64_t now;
  if (t->failed) return 0;
  if (t->heap_count == 0) return 1;
  if (!sample(t, &now)) return 0;
  while (budget-- > 0 && t->heap_count > 0) {
    struct timer_record *r = &t->records[t->heap[0]];
    if (r->deadline > now) break;
    remove_heap(t, 0);
    increment(&t->stats.expired);
    if (ready != NULL) ready(context, r->owner, r->id, r->deadline);
  }
  return 1;
}
int caml_actor_timers_wait(struct caml_actor_timers *t, uint64_t deadline)
{
  int result;
  uint64_t now;
  if (t->failed || t->backend.wait == NULL) return -1;
  result = t->backend.wait(t->backend.context, deadline);
  if (result < 0 || !sample(t, &now)) {
    t->failed = 1;
    return -1;
  }
  return result > 0 && now >= deadline ? 1 : 0;
}
void caml_actor_timers_retire(struct caml_actor_timers *t, uintnat owner)
{
  for (uintnat i = 0; i < t->capacity; i++) {
    if (t->records[i].id != 0 && t->records[i].owner == owner)
      caml_actor_timer_consume(t, owner, t->records[i].id, 1);
  }
}
void caml_actor_timers_stats(const struct caml_actor_timers *t,
                            struct caml_actor_timer_stats *stats)
{
  *stats = t->stats;
}
void caml_actor_timers_test_fail_allocation(struct caml_actor_timers *t)
{
  t->fail_allocation = 1;
}
void caml_actor_timers_test_exhaust_identity(struct caml_actor_timers *t)
{
  t->exhausted = 1;
}
