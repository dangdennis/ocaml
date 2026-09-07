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
#include "caml/actor_world.h"
#include "caml/domain.h"
#include "caml/domain_state.h"
#include "caml/debugger.h"
#include "caml/fail.h"
#include "caml/major_gc.h"
#include "caml/roots.h"
#include "caml/signals.h"
#include "caml/shared_heap.h"

struct caml_actor_world {
  enum caml_actor_world_state {
    CAML_ACTOR_WORLD_PREPARING,
    CAML_ACTOR_WORLD_FROZEN,
    CAML_ACTOR_WORLD_THAWING
  } state;
  caml_domain_state *domain;
  int domain_unique_id;
  value *young_ptr;
  value *young_end;
  uintnat major_cycles_completed;
  struct global_heap_state global_heap_state;
  struct caml_actor_frozen_entry *frozen;
  uintnat frozen_count;
  uintnat frozen_capacity;
};

struct caml_actor_frozen_entry {
  value block;
  header_t header;
  value *payload;
};

static int actor_world_runtime_supported(void)
{
#if defined(NATIVE_CODE) || !defined(__x86_64__) || !defined(__linux__)
  return 0;
#else
  return 1;
#endif
}

static int actor_world_available(caml_domain_state *domain)
{
  return domain != NULL
    && caml_domain_alone()
    && domain->inside_stw_handler == 0
    && atomic_load(&caml_scan_roots_hook) == NULL
    && atomic_load(&domain->requested_external_interrupt) == 0
    && !caml_incoming_interrupts_queued()
    && !caml_debugger_in_use
    && domain->actor_world == NULL
    && domain->actor_scheduler == NULL
    && domain->actor_heap == NULL
    && domain->gc_regs == NULL;
}

static int actor_world_preparing(
  caml_domain_state *domain, const struct caml_actor_world *world)
{
  return domain != NULL
    && caml_domain_alone()
    && domain->inside_stw_handler == 0
    && atomic_load(&caml_scan_roots_hook) == NULL
    && atomic_load(&domain->requested_external_interrupt) == 0
    && !caml_incoming_interrupts_queued()
    && !caml_debugger_in_use
    && domain->actor_world == world
    && world->state == CAML_ACTOR_WORLD_PREPARING
    && domain->actor_scheduler == NULL
    && domain->actor_heap == NULL
    && domain->gc_regs == NULL;
}

static int actor_world_heap_state_stable(
  caml_domain_state *domain, const struct caml_actor_world *world)
{
  return domain != NULL
    && world != NULL
    && world->domain == domain
    && world->domain_unique_id == domain->unique_id
    && domain->young_ptr == world->young_ptr
    && domain->young_end == world->young_end
    && caml_major_cycles_completed == world->major_cycles_completed
    && caml_global_heap_state.MARKED
         == world->global_heap_state.MARKED
    && caml_global_heap_state.UNMARKED
         == world->global_heap_state.UNMARKED
    && caml_global_heap_state.GARBAGE
         == world->global_heap_state.GARBAGE;
}

static void rollback_actor_world(caml_domain_state *domain,
                                 struct caml_actor_world *world)
{
  if (domain != NULL && domain->actor_world == world) {
    domain->actor_world = NULL;
  }
  for (uintnat index = 0; index < world->frozen_count; index++) {
    free(world->frozen[index].payload);
  }
  free(world->frozen);
  free(world);
}

enum caml_actor_world_status caml_actor_world_freeze(void)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;

  if (!actor_world_runtime_supported()) {
    return CAML_ACTOR_WORLD_UNSUPPORTED;
  }
  if (!actor_world_available(domain)) return CAML_ACTOR_WORLD_BUSY;

  world = calloc(1, sizeof(*world));
  if (world == NULL) return CAML_ACTOR_WORLD_UNSUPPORTED;
  world->state = CAML_ACTOR_WORLD_PREPARING;
  world->domain = domain;
  world->domain_unique_id = domain->unique_id;
  domain->actor_world = world;

  if (caml_check_pending_actions()) {
    caml_result result = caml_process_pending_actions_res();

    if (caml_result_is_exception(result)) {
      rollback_actor_world(domain, world);
      caml_get_value_or_raise(result);
    }
  }
  if (!actor_world_preparing(domain, world)) {
    rollback_actor_world(domain, world);
    return CAML_ACTOR_WORLD_BUSY;
  }

  caml_finish_major_cycle(0);
  caml_reset_major_pacing(false);
  if (caml_check_pending_actions()) {
    caml_result result = caml_process_pending_actions_res();

    if (caml_result_is_exception(result)) {
      rollback_actor_world(domain, world);
      caml_get_value_or_raise(result);
    }
  }
  if (!actor_world_preparing(domain, world)) {
    rollback_actor_world(domain, world);
    return CAML_ACTOR_WORLD_BUSY;
  }

  /* Promote anything allocated by the callbacks above, then leave any newly
     discovered finalisers and signals pending until after thaw. */
  caml_finish_major_cycle(0);
  caml_reset_major_pacing(false);
  if (domain->young_ptr != domain->young_end) {
    rollback_actor_world(domain, world);
    return CAML_ACTOR_WORLD_CORRUPTED;
  }

  world->young_ptr = domain->young_ptr;
  world->young_end = domain->young_end;
  world->major_cycles_completed = caml_major_cycles_completed;
  world->global_heap_state = caml_global_heap_state;
  world->state = CAML_ACTOR_WORLD_FROZEN;
  return CAML_ACTOR_WORLD_OK;
}

enum caml_actor_world_status caml_actor_world_thaw(void)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;
  enum caml_actor_world_status status = CAML_ACTOR_WORLD_OK;

  if (domain == NULL || domain->actor_world == NULL) {
    return CAML_ACTOR_WORLD_BUSY;
  }
  world = domain->actor_world;
  if (world->domain != domain
      || world->state != CAML_ACTOR_WORLD_FROZEN
      || world->domain_unique_id != domain->unique_id
      || domain->actor_scheduler != NULL
      || domain->actor_heap != NULL
      || domain->gc_regs != NULL) {
    return CAML_ACTOR_WORLD_BUSY;
  }
  world->state = CAML_ACTOR_WORLD_THAWING;

  if (!actor_world_heap_state_stable(domain, world)) {
    status = CAML_ACTOR_WORLD_CORRUPTED;
  }
  for (uintnat index = 0; index < world->frozen_count; index++) {
    if (status != CAML_ACTOR_WORLD_OK
        || !caml_shared_heap_contains_block(
             domain->shared_heap, world->frozen[index].block)
        || Hd_val(world->frozen[index].block)
          != world->frozen[index].header
        || memcmp(Op_val(world->frozen[index].block),
                  world->frozen[index].payload,
                  Bsize_wsize(Wosize_hd(world->frozen[index].header)))
             != 0) {
      status = CAML_ACTOR_WORLD_CORRUPTED;
    }
  }

  domain->actor_world = NULL;
  for (uintnat index = 0; index < world->frozen_count; index++) {
    free(world->frozen[index].payload);
  }
  free(world->frozen);
  free(world);
  return status;
}

int caml_actor_world_is_frozen(void)
{
  caml_domain_state *domain = Caml_state_opt;
  return domain != NULL
    && domain->actor_world != NULL
    && domain->actor_world->state == CAML_ACTOR_WORLD_FROZEN;
}

int caml_actor_world_register_frozen(value candidate)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;
  struct caml_actor_frozen_entry *entries;
  value *payload;
  uintnat capacity;
  header_t header;

  if (domain == NULL || domain->actor_world == NULL
      || domain->actor_world->state != CAML_ACTOR_WORLD_FROZEN
      || domain->actor_scheduler != NULL || domain->actor_heap != NULL
      || !Is_block(candidate) || candidate == 0
      || caml_actor_heap_contains_address(candidate)
      || !caml_shared_heap_contains_block(
           domain->shared_heap, candidate)) {
    return 0;
  }
  world = domain->actor_world;
  if (!actor_world_heap_state_stable(domain, world)) return 0;
  for (uintnat index = 0; index < world->frozen_count; index++) {
    if (world->frozen[index].block == candidate) {
      return Hd_val(candidate) == world->frozen[index].header
        && memcmp(Op_val(candidate), world->frozen[index].payload,
                  Bsize_wsize(Wosize_hd(world->frozen[index].header)))
             == 0;
    }
  }

  /* The public copier will add graph and tag preflight before calling this
     internal exact-block registration function. */
  header = Hd_val(candidate);
  if (Wosize_hd(header) == 0 || Tag_hd(header) == Infix_tag
      || Has_status_hd(header, caml_global_heap_state.GARBAGE)) {
    return 0;
  }
  if (world->frozen_count == world->frozen_capacity) {
    capacity = world->frozen_capacity == 0
      ? 16 : 2 * world->frozen_capacity;
    if (capacity < world->frozen_capacity
        || capacity > SIZE_MAX / sizeof(*entries)) {
      return 0;
    }
    entries = realloc(world->frozen, capacity * sizeof(*entries));
    if (entries == NULL) return 0;
    world->frozen = entries;
    world->frozen_capacity = capacity;
  }
  payload = malloc(Bsize_wsize(Wosize_hd(header)));
  if (payload == NULL) return 0;
  memcpy(payload, Op_val(candidate), Bsize_wsize(Wosize_hd(header)));
  world->frozen[world->frozen_count].block = candidate;
  world->frozen[world->frozen_count].header = header;
  world->frozen[world->frozen_count].payload = payload;
  world->frozen_count++;
  return 1;
}

int caml_actor_world_value_is_frozen(value candidate)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;

  if (domain == NULL || domain->actor_world == NULL
      || domain->actor_world->state != CAML_ACTOR_WORLD_FROZEN
      || !Is_block(candidate) || candidate == 0) {
    return 0;
  }
  world = domain->actor_world;
  for (uintnat index = 0; index < world->frozen_count; index++) {
    if (world->frozen[index].block == candidate) {
      return actor_world_heap_state_stable(domain, world)
        && caml_shared_heap_contains_block(
             domain->shared_heap, candidate)
        && Hd_val(candidate) == world->frozen[index].header
        && memcmp(Op_val(candidate), world->frozen[index].payload,
                  Bsize_wsize(
                    Wosize_hd(world->frozen[index].header))) == 0;
    }
  }
  return 0;
}
