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
#include "caml/codefrag.h"
#include "caml/domain.h"
#include "caml/domain_state.h"
#include "caml/debugger.h"
#include "caml/fail.h"
#include "caml/fiber.h"
#include "caml/major_gc.h"
#include "caml/roots.h"
#include "caml/signals.h"
#include "caml/shared_heap.h"

struct caml_actor_frozen_entry {
  value block;
  header_t header;
  value *payload;
  mlsize_t closure_env;
};

struct caml_actor_frozen_identity {
  value identity;
  uintnat index_plus_one;
};

struct caml_actor_frozen_graph {
  struct caml_actor_frozen_entry *entries;
  uintnat count;
  uintnat capacity;
  struct caml_actor_frozen_identity *identities;
  uintnat identity_used;
  uintnat identity_capacity;
  enum caml_actor_global_status status;
};

struct caml_actor_heap_interval {
  uintptr_t start;
  uintptr_t finish;
  value block;
  header_t header;
};

struct caml_actor_heap_index {
  struct caml_actor_heap_interval *intervals;
  uintnat count;
  uintnat capacity;
  enum caml_actor_global_status status;
};

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
  struct caml_actor_frozen_identity *identities;
  uintnat identity_used;
  uintnat identity_capacity;
  int global_prepared;
  value global_data;
  mlsize_t global_count;
  uintnat global_entry_plus_one;
};

static int heap_interval_compare(const void *left, const void *right)
{
  const struct caml_actor_heap_interval *left_interval = left;
  const struct caml_actor_heap_interval *right_interval = right;

  if (left_interval->start < right_interval->start) return -1;
  if (left_interval->start > right_interval->start) return 1;
  return 0;
}

static int heap_index_visit_block(
  value block, header_t header, void *data)
{
  struct caml_actor_heap_index *index = data;
  struct caml_actor_heap_interval *intervals;
  uintptr_t start = (uintptr_t)block;
  mlsize_t wosize = Wosize_hd(header);
  uintnat capacity;

  /* Zero-sized heap blocks cannot be valid frozen graph identities, but an
     unrelated one must not make the allocator snapshot unusable. */
  if (wosize == 0) return 1;
  if (start % sizeof(value) != 0
      || wosize > (UINTPTR_MAX - start) / sizeof(value)) {
    index->status = CAML_ACTOR_GLOBAL_INVALID_IMAGE;
    return 0;
  }
  if (index->count == index->capacity) {
    capacity = index->capacity == 0 ? 256 : 2 * index->capacity;
    if (capacity < index->capacity
        || capacity > SIZE_MAX / sizeof(*intervals)) {
      index->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
      return 0;
    }
    intervals = realloc(
      index->intervals, capacity * sizeof(*intervals));
    if (intervals == NULL) {
      index->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
      return 0;
    }
    index->intervals = intervals;
    index->capacity = capacity;
  }
  index->intervals[index->count].start = start;
  index->intervals[index->count].finish =
    start + Bsize_wsize(wosize);
  index->intervals[index->count].block = block;
  index->intervals[index->count].header = header;
  index->count++;
  return 1;
}

static int heap_index_build(caml_domain_state *domain,
                            struct caml_actor_heap_index *index)
{
  memset(index, 0, sizeof(*index));
  index->status = CAML_ACTOR_GLOBAL_OK;
  if (!caml_shared_heap_visit_blocks(
        domain->shared_heap, heap_index_visit_block, index)) {
    if (index->status == CAML_ACTOR_GLOBAL_OK) {
      index->status = CAML_ACTOR_GLOBAL_INVALID_IMAGE;
    }
    return 0;
  }
  if (index->count > 1) {
    qsort(index->intervals, index->count,
          sizeof(*index->intervals), heap_interval_compare);
  }
  for (uintnat slot = 0; slot < index->count; slot++) {
    struct caml_actor_heap_interval *interval =
      &index->intervals[slot];

    if (interval->finish <= interval->start
        || Hd_val(interval->block) != interval->header
        || (slot != 0
            && index->intervals[slot - 1].finish > interval->start)) {
      index->status = CAML_ACTOR_GLOBAL_INVALID_IMAGE;
      return 0;
    }
  }
  return 1;
}

static int heap_index_find(
  const struct caml_actor_heap_index *index, value candidate,
  value *block, header_t *header)
{
  uintptr_t address = (uintptr_t)candidate;
  uintnat low = 0;
  uintnat high = index->count;
  const struct caml_actor_heap_interval *interval;

  while (low < high) {
    uintnat middle = low + (high - low) / 2;

    if (index->intervals[middle].start <= address) {
      low = middle + 1;
    } else {
      high = middle;
    }
  }
  if (low == 0) return 0;
  interval = &index->intervals[low - 1];
  if (address >= interval->finish
      || (address - interval->start) % sizeof(value) != 0) {
    return 0;
  }
  *block = interval->block;
  *header = interval->header;
  return 1;
}

static void heap_index_destroy(struct caml_actor_heap_index *index)
{
  free(index->intervals);
  memset(index, 0, sizeof(*index));
}

static int canonical_atom(value candidate)
{
  for (uintnat tag = 0; tag < Num_tags; tag++) {
    if (candidate == Atom((tag_t)tag)) return 1;
  }
  return 0;
}

static uintnat frozen_hash(value identity)
{
  uintnat hash = ((uintnat)identity) >> 3;

  hash ^= hash >> 11;
  hash *= (uintnat)2654435761U;
  hash ^= hash >> 13;
  return hash;
}

static int frozen_graph_lookup(
  const struct caml_actor_frozen_graph *graph,
  value identity, uintnat *index)
{
  uintnat mask;
  uintnat slot;

  if (graph->identity_capacity == 0) return 0;
  mask = graph->identity_capacity - 1;
  slot = frozen_hash(identity) & mask;
  while (graph->identities[slot].index_plus_one != 0) {
    if (graph->identities[slot].identity == identity) {
      if (index != NULL) {
        *index = graph->identities[slot].index_plus_one - 1;
      }
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

static void frozen_graph_insert_unchecked(
  struct caml_actor_frozen_graph *graph,
  value identity, uintnat index)
{
  uintnat mask = graph->identity_capacity - 1;
  uintnat slot = frozen_hash(identity) & mask;

  while (graph->identities[slot].index_plus_one != 0) {
    slot = (slot + 1) & mask;
  }
  graph->identities[slot].identity = identity;
  graph->identities[slot].index_plus_one = index + 1;
  graph->identity_used++;
}

static int frozen_graph_reserve_identities(
  struct caml_actor_frozen_graph *graph, uintnat additional)
{
  struct caml_actor_frozen_identity *old_identities = graph->identities;
  uintnat old_capacity = graph->identity_capacity;
  uintnat needed;
  uintnat capacity;

  if (additional > CAML_UINTNAT_MAX - graph->identity_used) {
    graph->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
    return 0;
  }
  needed = graph->identity_used + additional;
  if (old_capacity != 0 && needed <= old_capacity / 2) return 1;

  capacity = old_capacity == 0 ? 32 : old_capacity;
  while (needed > capacity / 2) {
    if (capacity > CAML_UINTNAT_MAX / 2) {
      graph->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
      return 0;
    }
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(*graph->identities)) {
    graph->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
    return 0;
  }
  graph->identities = calloc(capacity, sizeof(*graph->identities));
  if (graph->identities == NULL) {
    graph->identities = old_identities;
    graph->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
    return 0;
  }
  graph->identity_capacity = capacity;
  graph->identity_used = 0;
  for (uintnat slot = 0; slot < old_capacity; slot++) {
    if (old_identities[slot].index_plus_one != 0) {
      frozen_graph_insert_unchecked(
        graph, old_identities[slot].identity,
        old_identities[slot].index_plus_one - 1);
    }
  }
  free(old_identities);
  return 1;
}

static int frozen_graph_reserve_entries(
  struct caml_actor_frozen_graph *graph, uintnat additional)
{
  struct caml_actor_frozen_entry *entries;
  uintnat needed;
  uintnat capacity;

  if (additional > CAML_UINTNAT_MAX - graph->count) {
    graph->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
    return 0;
  }
  needed = graph->count + additional;
  if (needed <= graph->capacity) return 1;
  capacity = graph->capacity == 0 ? 32 : graph->capacity;
  while (needed > capacity) {
    if (capacity > CAML_UINTNAT_MAX / 2) {
      graph->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
      return 0;
    }
    capacity *= 2;
  }
  if (capacity > SIZE_MAX / sizeof(*entries)) {
    graph->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
    return 0;
  }
  entries = realloc(graph->entries, capacity * sizeof(*entries));
  if (entries == NULL) {
    graph->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
    return 0;
  }
  graph->entries = entries;
  graph->capacity = capacity;
  return 1;
}

static int frozen_graph_reserve_entry(
  struct caml_actor_frozen_graph *graph)
{
  return frozen_graph_reserve_entries(graph, 1);
}

static int valid_code_pointer(value candidate)
{
  struct code_fragment *fragment;

  if ((uintptr_t)candidate % sizeof(opcode_t) != 0) return 0;
  fragment = caml_find_code_fragment_by_pc((char *)candidate);
  return fragment != NULL
    && (char *)candidate + sizeof(opcode_t) <= fragment->code_end;
}

static int validate_frozen_closure(value closure, mlsize_t wosize,
                                   mlsize_t *closure_env)
{
  value info;
  mlsize_t start;

  if (wosize < 2) return 0;
  info = Closinfo_val(closure);
  if (!Is_long(info)) return 0;
  start = Start_env_closinfo(info);
  if (start < 2 || start > wosize || (start - 2) % 3 != 0
      || !valid_code_pointer((value)Code_val(closure))) {
    return 0;
  }

  for (mlsize_t header_field = 2; header_field < start;
       header_field += 3) {
    header_t header = (header_t)Field(closure, header_field);
    value nested_info = Field(closure, header_field + 2);

    if (Tag_hd(header) != Infix_tag || Reserved_hd(header) != 0
        || Color_hd(header) != 0
        || Wosize_hd(header) != header_field + 1
        || !valid_code_pointer(Field(closure, header_field + 1))
        || !Is_long(nested_info)
        || Start_env_closinfo(nested_info)
             != start - (header_field + 1)) {
      return 0;
    }
  }
  *closure_env = start;
  return 1;
}

static int frozen_graph_add_block(
  struct caml_actor_frozen_graph *graph, value block, header_t header,
  uintnat *new_index)
{
  mlsize_t wosize = Wosize_hd(header);
  mlsize_t closure_env = 0;
  uintnat identity_count = 1;
  value *payload;
  uintnat index;

  if (Hd_val(block) != header
      || wosize == 0 || Reserved_hd(header) != 0
      || Tag_hd(header) == Infix_tag
      || Tag_hd(header) == Cont_tag
      || Has_status_hd(header, caml_global_heap_state.GARBAGE)) {
    graph->status = CAML_ACTOR_GLOBAL_INVALID_IMAGE;
    return 0;
  }
  if (Tag_hd(header) == Closure_tag) {
    if (!validate_frozen_closure(block, wosize, &closure_env)) {
      graph->status = CAML_ACTOR_GLOBAL_INVALID_IMAGE;
      return 0;
    }
    identity_count += (closure_env - 2) / 3;
  }
  if (!frozen_graph_reserve_entry(graph)
      || !frozen_graph_reserve_identities(graph, identity_count)) {
    return 0;
  }
  if (wosize > SIZE_MAX / sizeof(value)) {
    graph->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
    return 0;
  }
  payload = malloc(Bsize_wsize(wosize));
  if (payload == NULL) {
    graph->status = CAML_ACTOR_GLOBAL_RESOURCE_UNAVAILABLE;
    return 0;
  }
  memcpy(payload, Op_val(block), Bsize_wsize(wosize));

  index = graph->count;
  graph->entries[index].block = block;
  graph->entries[index].header = header;
  graph->entries[index].payload = payload;
  graph->entries[index].closure_env = closure_env;
  frozen_graph_insert_unchecked(graph, block, index);
  for (mlsize_t header_field = 2; header_field < closure_env;
       header_field += 3) {
    mlsize_t field = header_field + 1;
    value infix = block + Bsize_wsize(field);

    frozen_graph_insert_unchecked(graph, infix, index);
  }
  graph->count++;
  if (new_index != NULL) *new_index = index;
  return 1;
}

static int frozen_entry_is_exact(
  const struct caml_actor_frozen_entry *entry)
{
  return Hd_val(entry->block) == entry->header
    && memcmp(Op_val(entry->block), entry->payload,
              Bsize_wsize(Wosize_hd(entry->header))) == 0;
}

static int frozen_entry_is_actor_visible(
  const struct caml_actor_frozen_entry *entry)
{
  tag_t tag = Tag_hd(entry->header);

  return tag < Forcing_tag
    || tag == Closure_tag
    || tag == Object_tag
    || tag == String_tag
    || tag == Double_tag
    || tag == Double_array_tag;
}

static int frozen_entry_header_is_exact(
  const struct caml_actor_frozen_entry *entry)
{
  return Hd_val(entry->block) == entry->header;
}

static void frozen_graph_destroy(struct caml_actor_frozen_graph *graph)
{
  for (uintnat index = 0; index < graph->count; index++) {
    free(graph->entries[index].payload);
  }
  free(graph->entries);
  free(graph->identities);
  memset(graph, 0, sizeof(*graph));
}

static struct caml_actor_frozen_graph frozen_graph_of_world(
  struct caml_actor_world *world)
{
  struct caml_actor_frozen_graph graph;

  graph.entries = world->frozen;
  graph.count = world->frozen_count;
  graph.capacity = world->frozen_capacity;
  graph.identities = world->identities;
  graph.identity_used = world->identity_used;
  graph.identity_capacity = world->identity_capacity;
  graph.status = CAML_ACTOR_GLOBAL_OK;
  return graph;
}

static void frozen_graph_store(struct caml_actor_world *world,
                               const struct caml_actor_frozen_graph *graph)
{
  world->frozen = graph->entries;
  world->frozen_count = graph->count;
  world->frozen_capacity = graph->capacity;
  world->identities = graph->identities;
  world->identity_used = graph->identity_used;
  world->identity_capacity = graph->identity_capacity;
}

static int frozen_graph_merge(
  struct caml_actor_frozen_graph *destination,
  struct caml_actor_frozen_graph *source)
{
  uintnat first = destination->count;

  if (!frozen_graph_reserve_entries(destination, source->count)
      || !frozen_graph_reserve_identities(
           destination, source->identity_used)) {
    return 0;
  }
  memcpy(&destination->entries[first], source->entries,
         source->count * sizeof(*source->entries));
  for (uintnat slot = 0; slot < source->identity_capacity; slot++) {
    if (source->identities[slot].index_plus_one != 0) {
      frozen_graph_insert_unchecked(
        destination, source->identities[slot].identity,
        first + source->identities[slot].index_plus_one - 1);
    }
  }
  destination->count += source->count;

  /* Payload ownership moved with the copied entries. */
  free(source->entries);
  free(source->identities);
  memset(source, 0, sizeof(*source));
  return 1;
}

static int frozen_graph_discover(
  const struct caml_actor_heap_index *heap_index,
  const struct caml_actor_frozen_graph *approved,
  struct caml_actor_frozen_graph *graph, value candidate)
{
  value base;
  header_t header;
  uintnat index;

  if (Is_long(candidate) || canonical_atom(candidate)) return 1;
  if (candidate == 0 || caml_actor_heap_contains_address(candidate)) {
    graph->status = CAML_ACTOR_GLOBAL_INVALID_IMAGE;
    return 0;
  }
  if ((approved != NULL
       && frozen_graph_lookup(approved, candidate, NULL))
      || frozen_graph_lookup(graph, candidate, NULL)) {
    return 1;
  }
  if (!heap_index_find(heap_index, candidate, &base, &header)) {
    graph->status = CAML_ACTOR_GLOBAL_INVALID_IMAGE;
    return 0;
  }

  if (approved != NULL && frozen_graph_lookup(approved, base, NULL)) {
    graph->status = CAML_ACTOR_GLOBAL_INVALID_IMAGE;
    return 0;
  }
  if (!frozen_graph_lookup(graph, base, &index)
      && !frozen_graph_add_block(graph, base, header, &index)) {
    return 0;
  }
  if (!frozen_graph_lookup(graph, candidate, NULL)) {
    graph->status = CAML_ACTOR_GLOBAL_INVALID_IMAGE;
    return 0;
  }
  return 1;
}

static int frozen_graph_scan(
  const struct caml_actor_heap_index *heap_index,
  const struct caml_actor_frozen_graph *approved,
  struct caml_actor_frozen_graph *graph)
{
  for (uintnat index = 0; index < graph->count; index++) {
    value block = graph->entries[index].block;
    header_t header = graph->entries[index].header;
    mlsize_t first;
    mlsize_t last;

    if (Tag_hd(header) == Closure_tag) {
      first = graph->entries[index].closure_env;
      last = Wosize_hd(header);
    } else if (Tag_hd(header) < No_scan_tag) {
      first = 0;
      last = Wosize_hd(header);
    } else {
      continue;
    }
    for (mlsize_t field = first; field < last; field++) {
      if (!frozen_graph_discover(
            heap_index, approved, graph, Field(block, field))) {
        return 0;
      }
    }
  }
  for (uintnat index = 0; index < graph->count; index++) {
    if (!frozen_entry_is_exact(&graph->entries[index])) {
      graph->status = CAML_ACTOR_GLOBAL_INVALID_IMAGE;
      return 0;
    }
  }
  return 1;
}

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

static int actor_world_global_data_stable(
  const struct caml_actor_world *world)
{
#if defined(NATIVE_CODE)
  /* Native actor worlds are rejected before preparation.  Keep this object
     linkable without the bytecode runtime's [caml_global_data] symbol. */
  return !world->global_prepared;
#else
  return !world->global_prepared
    || caml_global_data == world->global_data;
#endif
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
         == world->global_heap_state.GARBAGE
    && actor_world_global_data_stable(world);
}

static void rollback_actor_world(caml_domain_state *domain,
                                 struct caml_actor_world *world)
{
  struct caml_actor_frozen_graph graph = frozen_graph_of_world(world);

  if (domain != NULL && domain->actor_world == world) {
    domain->actor_world = NULL;
  }
  frozen_graph_destroy(&graph);
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
  struct caml_actor_frozen_graph graph;
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
        || !frozen_entry_is_exact(&world->frozen[index])) {
      status = CAML_ACTOR_WORLD_CORRUPTED;
    }
  }

  domain->actor_world = NULL;
  graph = frozen_graph_of_world(world);
  frozen_graph_destroy(&graph);
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
  struct caml_actor_frozen_graph approved;
  struct caml_actor_frozen_graph staged;
  struct caml_actor_heap_index heap_index;
  value base;
  header_t header;
  uintnat index;
  int registered = 0;

  if (domain == NULL || domain->actor_world == NULL
      || domain->actor_world->state != CAML_ACTOR_WORLD_FROZEN
      || domain->actor_scheduler != NULL || domain->actor_heap != NULL
      || !Is_block(candidate) || candidate == 0
      || caml_actor_heap_contains_address(candidate)) {
    return 0;
  }
  world = domain->actor_world;
  if (!actor_world_heap_state_stable(domain, world)) return 0;
  approved = frozen_graph_of_world(world);
  if (frozen_graph_lookup(&approved, candidate, &index)) {
    return frozen_entry_is_exact(&approved.entries[index]);
  }

  if (!heap_index_build(domain, &heap_index)) {
    heap_index_destroy(&heap_index);
    return 0;
  }
  if (!heap_index_find(&heap_index, candidate, &base, &header)
      || base != candidate) {
    heap_index_destroy(&heap_index);
    return 0;
  }
  (void)header;

  memset(&staged, 0, sizeof(staged));
  staged.status = CAML_ACTOR_GLOBAL_OK;
  if (frozen_graph_discover(
        &heap_index, &approved, &staged, candidate)
      && frozen_graph_scan(&heap_index, &approved, &staged)) {
    registered = frozen_graph_merge(&approved, &staged);
    /* A failed reserve may still have moved metadata allocations. */
    frozen_graph_store(world, &approved);
  }
  frozen_graph_destroy(&staged);
  heap_index_destroy(&heap_index);
  return registered;
}

int caml_actor_world_value_is_frozen(value candidate)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;
  struct caml_actor_frozen_graph graph;
  uintnat index;

  if (domain == NULL || domain->actor_world == NULL
      || domain->actor_world->state != CAML_ACTOR_WORLD_FROZEN
      || !Is_block(candidate) || candidate == 0) {
    return 0;
  }
  world = domain->actor_world;
  if (!actor_world_heap_state_stable(domain, world)) return 0;
  graph = frozen_graph_of_world(world);
  return frozen_graph_lookup(&graph, candidate, &index)
    && frozen_entry_is_exact(&graph.entries[index]);
}

int caml_actor_world_value_is_approved(value candidate)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;
  struct caml_actor_frozen_graph graph;

  if (domain == NULL || domain->actor_world == NULL
      || domain->actor_world->state != CAML_ACTOR_WORLD_FROZEN
      || !Is_block(candidate) || candidate == 0) {
    return 0;
  }
  world = domain->actor_world;
  if (!actor_world_heap_state_stable(domain, world)) return 0;
  graph = frozen_graph_of_world(world);
  {
    uintnat index;
    return frozen_graph_lookup(&graph, candidate, &index)
      && frozen_entry_is_actor_visible(&graph.entries[index])
      && frozen_entry_header_is_exact(&graph.entries[index]);
  }
}

enum caml_actor_global_status caml_actor_world_prepare_global_image(void)
{
#if defined(NATIVE_CODE)
  /* [caml_actor_world_freeze] rejects native execution, so no native caller
     can own a world that is eligible for global-image preparation. */
  return CAML_ACTOR_GLOBAL_BUSY;
#else
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;
  struct caml_actor_frozen_graph graph;
  struct caml_actor_heap_index heap_index;
  enum caml_actor_global_status status;
  uintnat global_entry;

  if (domain == NULL || domain->actor_world == NULL) {
    return CAML_ACTOR_GLOBAL_BUSY;
  }
  world = domain->actor_world;
  if (world->state != CAML_ACTOR_WORLD_FROZEN
      || world->domain != domain
      || world->domain_unique_id != domain->unique_id
      || domain->actor_scheduler != NULL
      || domain->actor_heap != NULL
      || domain->gc_regs != NULL
      || world->global_prepared
      || world->frozen_count != 0
      || world->identity_used != 0
      || !actor_world_heap_state_stable(domain, world)) {
    return CAML_ACTOR_GLOBAL_BUSY;
  }

  if (!heap_index_build(domain, &heap_index)) {
    status = heap_index.status;
    heap_index_destroy(&heap_index);
    return status;
  }
  memset(&graph, 0, sizeof(graph));
  graph.status = CAML_ACTOR_GLOBAL_OK;
  if (!frozen_graph_discover(
        &heap_index, NULL, &graph, caml_global_data)
      || !frozen_graph_lookup(&graph, caml_global_data, &global_entry)
      || graph.entries[global_entry].block != caml_global_data
      || Tag_hd(graph.entries[global_entry].header) != 0
      || !frozen_graph_scan(&heap_index, NULL, &graph)) {
    status = graph.status == CAML_ACTOR_GLOBAL_OK
      ? CAML_ACTOR_GLOBAL_INVALID_IMAGE : graph.status;
    frozen_graph_destroy(&graph);
    heap_index_destroy(&heap_index);
    return status;
  }

  heap_index_destroy(&heap_index);
  frozen_graph_store(world, &graph);
  world->global_data = caml_global_data;
  world->global_count = Wosize_hd(world->frozen[global_entry].header);
  world->global_entry_plus_one = global_entry + 1;
  world->global_prepared = 1;
  return CAML_ACTOR_GLOBAL_OK;
#endif
}

static struct caml_actor_world *actor_world_for_global_read(
  caml_domain_state *domain)
{
  struct caml_actor_world *world;

  if (domain == NULL || domain->actor_world == NULL) return NULL;
  world = domain->actor_world;
  if (world->state != CAML_ACTOR_WORLD_FROZEN
      || world->domain != domain
      || world->domain_unique_id != domain->unique_id
      || !world->global_prepared
      || world->global_entry_plus_one == 0
      || world->global_entry_plus_one > world->frozen_count
      || !actor_world_heap_state_stable(domain, world)) {
    return NULL;
  }
  return world;
}

static int frozen_value_is_approved(
  const struct caml_actor_frozen_graph *graph, value candidate)
{
  uintnat index;

  if (Is_long(candidate) || canonical_atom(candidate)) return 1;
  return candidate != 0
    && frozen_graph_lookup(graph, candidate, &index)
    && frozen_entry_is_actor_visible(&graph->entries[index])
    && frozen_entry_header_is_exact(&graph->entries[index]);
}

int caml_actor_world_read_global(uintnat index, value *result)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;
  struct caml_actor_frozen_graph graph;
  struct caml_actor_frozen_entry *global_entry;
  value current;

  if (result == NULL) return 0;
  world = actor_world_for_global_read(domain);
  if (world == NULL || index >= world->global_count) return 0;
  graph = frozen_graph_of_world(world);
  global_entry = &graph.entries[world->global_entry_plus_one - 1];
  if (!frozen_entry_header_is_exact(global_entry)) return 0;

  current = Field(world->global_data, index);
  if (current != global_entry->payload[index]
      || !frozen_value_is_approved(&graph, current)) {
    return 0;
  }
  *result = current;
  return 1;
}

int caml_actor_world_read_global_field(
  uintnat index, mlsize_t field, value *result)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;
  struct caml_actor_frozen_graph graph;
  struct caml_actor_frozen_entry *entry;
  value block;
  value current;
  uintnat block_index;

  if (result == NULL || !caml_actor_world_read_global(index, &block)) {
    return 0;
  }
  world = actor_world_for_global_read(domain);
  if (world == NULL || !Is_block(block) || block == 0) return 0;
  graph = frozen_graph_of_world(world);
  if (!frozen_graph_lookup(&graph, block, &block_index)) return 0;
  entry = &graph.entries[block_index];
  if (entry->block != block
      || Tag_hd(entry->header) >= Forcing_tag
      || field >= Wosize_hd(entry->header)
      || !frozen_entry_header_is_exact(entry)) {
    return 0;
  }

  current = Field(block, field);
  if (current != entry->payload[field]
      || !frozen_value_is_approved(&graph, current)) {
    return 0;
  }
  *result = current;
  return 1;
}

int caml_actor_world_read_frozen_field(
  value block, mlsize_t field, value *result)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;
  struct caml_actor_frozen_graph graph;
  struct caml_actor_frozen_entry *entry;
  uintnat index;
  value current;

  if (result == NULL || !Is_block(block) || block == 0) return 0;
  world = actor_world_for_global_read(domain);
  if (world == NULL) return 0;
  graph = frozen_graph_of_world(world);
  if (!frozen_graph_lookup(&graph, block, &index)) return 0;
  entry = &graph.entries[index];
  if (entry->block != block
      || Tag_hd(entry->header) >= Forcing_tag
      || field >= Wosize_hd(entry->header)
      || !frozen_entry_header_is_exact(entry)) {
    return 0;
  }
  current = Field(block, field);
  if (current != entry->payload[field]
      || !frozen_value_is_approved(&graph, current)) {
    return 0;
  }
  *result = current;
  return 1;
}

int caml_actor_world_read_frozen_closure_env(
  value closure, mlsize_t field, value *result)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;
  struct caml_actor_frozen_graph graph;
  struct caml_actor_frozen_entry *entry;
  uintptr_t byte_offset;
  mlsize_t word_offset;
  mlsize_t absolute_field;
  uintnat index;
  value current;

  if (result == NULL || !Is_block(closure) || closure == 0) return 0;
  world = actor_world_for_global_read(domain);
  if (world == NULL) return 0;
  graph = frozen_graph_of_world(world);
  if (!frozen_graph_lookup(&graph, closure, &index)) return 0;
  entry = &graph.entries[index];
  if (Tag_hd(entry->header) != Closure_tag
      || (uintptr_t)closure < (uintptr_t)entry->block
      || !frozen_entry_header_is_exact(entry)) {
    return 0;
  }
  byte_offset = (uintptr_t)closure - (uintptr_t)entry->block;
  if (byte_offset % sizeof(value) != 0) return 0;
  word_offset = byte_offset / sizeof(value);
  if (word_offset > Wosize_hd(entry->header)
      || field > Wosize_hd(entry->header) - word_offset) {
    return 0;
  }
  absolute_field = word_offset + field;
  if (absolute_field < entry->closure_env
      || absolute_field >= Wosize_hd(entry->header)) {
    return 0;
  }
  current = Field(closure, field);
  if (current != entry->payload[absolute_field]
      || !frozen_value_is_approved(&graph, current)) {
    return 0;
  }
  *result = current;
  return 1;
}

int caml_actor_world_frozen_closure_wosize(
  value closure, mlsize_t *wosize)
{
  caml_domain_state *domain = Caml_state_opt;
  struct caml_actor_world *world;
  struct caml_actor_frozen_graph graph;
  struct caml_actor_frozen_entry *entry;
  uintnat index;

  if (wosize == NULL || !Is_block(closure) || closure == 0) return 0;
  world = actor_world_for_global_read(domain);
  if (world == NULL) return 0;
  graph = frozen_graph_of_world(world);
  if (!frozen_graph_lookup(&graph, closure, &index)) return 0;
  entry = &graph.entries[index];
  if (entry->block != closure || Tag_hd(entry->header) != Closure_tag
      || !frozen_entry_header_is_exact(entry)) {
    return 0;
  }
  *wosize = Wosize_hd(entry->header);
  return 1;
}
