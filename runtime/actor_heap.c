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
#include "caml/address_class.h"
#include "caml/codefrag.h"
#include "caml/domain.h"
#include "caml/domain_state.h"
#include "caml/fail.h"
#include "caml/misc.h"
#include "caml/platform.h"
#include "caml/roots.h"
#include "caml/shared_heap.h"

struct caml_actor_heap {
  uintnat owner;
  char *mapping;
  uintnat mapping_bytes;
  uintnat reserved_space_bytes;
  uintnat committed_space_bytes;
  value *data_start[2];
  value *data_end[2];
  value *cursor;
  header_t *shadow_headers[2];
  value *forwarding;
  value *worklist;
  mlsize_t capacity_words;
  mlsize_t quota_words;
  mlsize_t used_words;
  uintnat blocks;
  uintnat shared_bypasses;
  uintnat collections;
  uintnat growths;
  unsigned active_space;
  int active;
  struct caml_actor_heap *next;
};

static struct caml_actor_heap *actor_heaps;
static caml_plat_mutex actor_heaps_lock = CAML_PLAT_MUTEX_INITIALIZER;

#ifdef DEBUG
#define ACTOR_HEAP_QUARANTINE_CAPACITY 8

struct actor_heap_quarantine_entry {
  char *mapping;
  uintnat mapping_bytes;
};

static struct actor_heap_quarantine_entry
  actor_heap_quarantine[ACTOR_HEAP_QUARANTINE_CAPACITY];
static uintnat actor_heap_quarantine_count;
static uintnat actor_heap_quarantine_next;
#endif

struct actor_value_lookup {
  struct caml_actor_heap *heap;
  value canonical;
  int exact;
  int malformed;
};

struct actor_space_lookup {
  value base;
  mlsize_t infix_offset;
  mlsize_t header_offset;
  int exact;
};

struct actor_gc_context {
  struct caml_actor_heap *heap;
  unsigned from_space;
  unsigned to_space;
  value *to_cursor;
  mlsize_t live_words;
  uintnat live_blocks;
  uintnat work_count;
  mlsize_t capacity_words;
  int valid;
};

static int actor_store_value_supported(struct caml_actor_heap *heap,
                                       value new_value);

static int actor_heap_registered(const struct caml_actor_heap *candidate)
{
  struct caml_actor_heap *heap;
  int found = 0;

  caml_plat_lock_non_blocking(&actor_heaps_lock);
  for (heap = actor_heaps; heap != NULL; heap = heap->next) {
    if (heap == candidate) {
      found = 1;
      break;
    }
  }
  caml_plat_unlock(&actor_heaps_lock);
  return found;
}

static int actor_runtime_supported(void)
{
#if defined(NATIVE_CODE) || !defined(__x86_64__) || !defined(__linux__)
  return 0;
#else
  return 1;
#endif
}

static void retire_actor_mapping(struct caml_actor_heap *heap)
{
#ifdef DEBUG
  struct actor_heap_quarantine_entry *entry =
    &actor_heap_quarantine[actor_heap_quarantine_next];
  caml_mem_decommit(heap->data_start[0], heap->committed_space_bytes);
  caml_mem_decommit(heap->data_start[1], heap->committed_space_bytes);
  if (actor_heap_quarantine_count == ACTOR_HEAP_QUARANTINE_CAPACITY) {
    caml_mem_unmap(entry->mapping, entry->mapping_bytes);
  } else {
    actor_heap_quarantine_count++;
  }
  entry->mapping = heap->mapping;
  entry->mapping_bytes = heap->mapping_bytes;
  actor_heap_quarantine_next =
    (actor_heap_quarantine_next + 1) % ACTOR_HEAP_QUARANTINE_CAPACITY;
#else
  caml_mem_unmap(heap->mapping, heap->mapping_bytes);
#endif
}

static int valid_code_pointer(value code)
{
  return (uintptr_t)code % sizeof(opcode_t) == 0
    && caml_find_code_fragment_by_pc((char *)code) != NULL;
}

static int add_overflows_uintnat(uintnat left, uintnat right)
{
  return left > CAML_UINTNAT_MAX - right;
}

static int mul_overflows_uintnat(uintnat left, uintnat right)
{
  return right != 0 && left > CAML_UINTNAT_MAX / right;
}

static int actor_tag_supported(mlsize_t wosize, tag_t tag,
                               reserved_t reserved)
{
  if (reserved != 0 || wosize == 0 || wosize > Max_wosize) return 0;

  if (tag < Forcing_tag) return 1;

  switch (tag) {
  case Closure_tag:
    return wosize >= 2;
  case String_tag:
    return wosize >= 1;
  case Double_tag:
    return wosize == Double_wosize;
  case Double_array_tag:
    return wosize % Double_wosize == 0;
  default:
    return 0;
  }
}

int caml_actor_heap_allocation_supported(mlsize_t wosize, tag_t tag,
                                         reserved_t reserved)
{
  return actor_tag_supported(wosize, tag, reserved);
}

static int range_contains(const struct caml_actor_heap *heap,
                          uintptr_t address)
{
  uintptr_t start = (uintptr_t)heap->mapping;
  uintptr_t end = start + heap->mapping_bytes;
  return address >= start && address < end;
}

static int space_contains(const struct caml_actor_heap *heap,
                          unsigned space, uintptr_t address)
{
  return address >= (uintptr_t)heap->data_start[space]
    && address < (uintptr_t)heap->data_end[space];
}

static enum caml_actor_heap_verify_error validate_layout(
  const struct caml_actor_heap *heap)
{
  value *cursor = heap->cursor;
  unsigned space = heap->active_space;
  uintnat blocks = 0;

  if (space > 1
      || cursor < heap->data_start[space]
      || cursor > heap->data_end[space]
      || (mlsize_t)(heap->data_end[space] - cursor)
           != heap->used_words) {
    return CAML_ACTOR_HEAP_VERIFY_MALFORMED;
  }

  while (cursor < heap->data_end[space]) {
    mlsize_t remaining = heap->data_end[space] - cursor;
    mlsize_t offset = cursor - heap->data_start[space];
    header_t header = Hd_hp(cursor);
    header_t expected = heap->shadow_headers[space][offset];
    mlsize_t wosize;

    if (expected == 0 || header != expected) {
      return CAML_ACTOR_HEAP_VERIFY_MALFORMED;
    }
    wosize = Wosize_hd(expected);
    if (Reserved_hd(expected) != 0 || Color_hd(expected) != NOT_MARKABLE
        || wosize == 0 || wosize >= remaining
        || Tag_hd(expected) == Infix_tag) {
      return CAML_ACTOR_HEAP_VERIFY_MALFORMED;
    }
    if (!actor_tag_supported(wosize, Tag_hd(expected), 0)) {
      return CAML_ACTOR_HEAP_VERIFY_UNSUPPORTED_TAG;
    }
    cursor += Whsize_wosize(wosize);
    blocks++;
  }

  if (cursor != heap->data_end[space] || blocks != heap->blocks) {
    return CAML_ACTOR_HEAP_VERIFY_MALFORMED;
  }
  return CAML_ACTOR_HEAP_VERIFY_OK;
}

static int valid_infix_pointer(value base, mlsize_t wosize, value candidate)
{
  uintptr_t base_address = (uintptr_t)base;
  uintptr_t candidate_address = (uintptr_t)candidate;
  mlsize_t field;
  mlsize_t start;
  header_t infix_header;
  value info;
  value nested_info;

  if (wosize < 2) return 0;
  info = Closinfo_val(base);
  if (!Is_long(info)) return 0;
  start = Start_env_closinfo(info);
  if (start < 2 || start > wosize || (start - 2) % 3 != 0) return 0;

  if (candidate_address <= base_address
      || candidate_address >= base_address + Bsize_wsize(wosize)
      || (candidate_address - base_address) % sizeof(value) != 0) {
    return 0;
  }
  field = (candidate_address - base_address) / sizeof(value);
  if (field < 3 || field >= start || (field - 3) % 3 != 0) return 0;

  infix_header = (header_t)Field(base, field - 1);
  nested_info = Field(base, field + 1);
  return Tag_hd(infix_header) == Infix_tag
    && Reserved_hd(infix_header) == 0
    && Color_hd(infix_header) == 0
    && Wosize_hd(infix_header) == field
    && valid_code_pointer(Field(base, field))
    && Is_long(nested_info)
    && Start_env_closinfo(nested_info) == start - field;
}

static void lookup_actor_value(value candidate,
                               struct actor_value_lookup *lookup)
{
  uintptr_t address = (uintptr_t)candidate;
  struct caml_actor_heap *heap;

  lookup->heap = NULL;
  lookup->canonical = 0;
  lookup->exact = 0;
  lookup->malformed = 0;

  caml_plat_lock_non_blocking(&actor_heaps_lock);
  for (heap = actor_heaps; heap != NULL; heap = heap->next) {
    if (!range_contains(heap, address)) continue;

    lookup->heap = heap;
    if (heap->active_space > 1) {
      lookup->malformed = 1;
      break;
    }
    if (space_contains(heap, heap->active_space, address)) {
      volatile header_t *header = Hp_val(candidate);

      if ((value *)header >= heap->data_start[heap->active_space]
          && (value *)header < heap->data_end[heap->active_space]) {
        mlsize_t offset = (value *)header
          - heap->data_start[heap->active_space];
        header_t expected =
          heap->shadow_headers[heap->active_space][offset];

        if (expected != 0 && *header == expected) {
          lookup->canonical = candidate;
          lookup->exact = 1;
          break;
        }
      }
    }
    if (validate_layout(heap) != CAML_ACTOR_HEAP_VERIFY_OK) {
      lookup->malformed = 1;
      break;
    }

    for (value *cursor = heap->cursor;
         cursor < heap->data_end[heap->active_space]; ) {
      mlsize_t wosize = Wosize_hp(cursor);
      value base = Val_hp(cursor);

      if (candidate == base
          || (Tag_hp(cursor) == Closure_tag
              && valid_infix_pointer(base, wosize, candidate))) {
        lookup->canonical = base;
        lookup->exact = 1;
        break;
      }
      cursor += Whsize_wosize(wosize);
    }
    break;
  }
  caml_plat_unlock(&actor_heaps_lock);
}

static int canonical_atom(value candidate)
{
  for (uintnat tag = 0; tag < Num_tags; tag++) {
    if (candidate == Atom((tag_t)tag)) return 1;
  }
  return 0;
}

static void lookup_space_value(const struct caml_actor_heap *heap,
                               unsigned space, value candidate,
                               struct actor_space_lookup *lookup)
{
  volatile header_t *header;

  lookup->base = 0;
  lookup->infix_offset = 0;
  lookup->header_offset = 0;
  lookup->exact = 0;
  if (!Is_block(candidate)
      || !space_contains(heap, space, (uintptr_t)candidate)) {
    return;
  }

  header = Hp_val(candidate);
  if ((value *)header >= heap->data_start[space]
      && (value *)header < heap->data_end[space]) {
    mlsize_t offset = (value *)header - heap->data_start[space];

    if (heap->shadow_headers[space][offset] != 0
        && Val_hp((value *)header) == candidate) {
      lookup->base = candidate;
      lookup->header_offset = offset;
      lookup->exact = 1;
      return;
    }
  }

  for (value *cursor = heap->cursor;
       space == heap->active_space
         && cursor < heap->data_end[space]; ) {
    mlsize_t wosize = Wosize_hp(cursor);
    value base = Val_hp(cursor);

    if (Tag_hp(cursor) == Closure_tag
        && valid_infix_pointer(base, wosize, candidate)) {
      lookup->base = base;
      lookup->infix_offset = candidate - base;
      lookup->header_offset = cursor - heap->data_start[space];
      lookup->exact = 1;
      return;
    }
    cursor += Whsize_wosize(wosize);
  }
}

static int gc_root_valid(const struct caml_actor_heap *heap, value root)
{
  struct actor_space_lookup local;

  if (Is_long(root) || canonical_atom(root)) return 1;
  lookup_space_value(heap, heap->active_space, root, &local);
  if (local.exact) return 1;
  return caml_actor_world_value_is_approved(root);
}

static void validate_gc_root(void *data, value root,
                             volatile value *location)
{
  struct actor_gc_context *context = data;

  (void)location;
  if (context->valid && !gc_root_valid(context->heap, root)) {
    context->valid = 0;
  }
}

static value gc_copy_block(struct actor_gc_context *context,
                           const struct actor_space_lookup *source)
{
  struct caml_actor_heap *heap = context->heap;
  value forwarded = heap->forwarding[source->header_offset];
  header_t header;
  mlsize_t wosize;
  mlsize_t whsize;
  mlsize_t target_offset;
  value target;

  if (forwarded != 0) return forwarded;
  header = heap->shadow_headers[context->from_space]
                               [source->header_offset];
  if (header == 0 || Hd_val(source->base) != header) {
    context->valid = 0;
    return 0;
  }
  wosize = Wosize_hd(header);
  whsize = Whsize_wosize(wosize);
  if (whsize > (mlsize_t)(
        context->to_cursor - heap->data_start[context->to_space])
      || context->work_count >= context->capacity_words) {
    context->valid = 0;
    return 0;
  }
  context->to_cursor -= whsize;
  target_offset = context->to_cursor - heap->data_start[context->to_space];
  Hd_hp(context->to_cursor) = header;
  heap->shadow_headers[context->to_space][target_offset] = header;
  target = Val_hp(context->to_cursor);
  memcpy(Op_val(target), Op_val(source->base), Bsize_wsize(wosize));
  heap->forwarding[source->header_offset] = target;
  heap->worklist[context->work_count++] = target;
  context->live_words += whsize;
  context->live_blocks++;
  return target;
}

static void forward_gc_root(void *data, value root,
                            volatile value *location)
{
  struct actor_gc_context *context = data;
  struct actor_space_lookup source;
  value target;

  if (!context->valid || Is_long(root) || canonical_atom(root)) return;
  lookup_space_value(
    context->heap, context->from_space, root, &source);
  if (!source.exact && caml_actor_world_value_is_approved(root)) return;
  if (!source.exact) {
    context->valid = 0;
    return;
  }
  target = gc_copy_block(context, &source);
  if (target == 0) return;
  *location = target + source.infix_offset;
}

static void scan_gc_block(struct actor_gc_context *context, value block)
{
  mlsize_t wosize = Wosize_val(block);
  tag_t tag = Tag_val(block);
  mlsize_t first = 0;

  if (tag == Closure_tag) {
    value info = Closinfo_val(block);

    if (!Is_long(info)) {
      context->valid = 0;
      return;
    }
    first = Start_env_closinfo(info);
    if (first < 2 || first > wosize || (first - 2) % 3 != 0) {
      context->valid = 0;
      return;
    }
  } else if (tag >= Forcing_tag) {
    return;
  }
  for (mlsize_t field = first;
       context->valid && field < wosize; field++) {
    forward_gc_root(context, Field(block, field), &Field(block, field));
  }
}

static int actor_heap_prepare_capacity(struct caml_actor_heap *heap,
                                       mlsize_t capacity_words)
{
  header_t *shadow_headers[2] = { NULL, NULL };
  value *forwarding = NULL;
  value *worklist = NULL;
  uintnat committed_bytes;
  uintnat extra_bytes;

  if (capacity_words <= heap->capacity_words) {
    return capacity_words == heap->capacity_words;
  }
  if (capacity_words > heap->quota_words) return 0;

  for (unsigned space = 0; space < 2; space++) {
    shadow_headers[space] = calloc(
      capacity_words, sizeof(*shadow_headers[space]));
    if (shadow_headers[space] == NULL) goto failed;
    memcpy(shadow_headers[space], heap->shadow_headers[space],
           heap->capacity_words * sizeof(*shadow_headers[space]));
  }
  forwarding = calloc(capacity_words, sizeof(*forwarding));
  worklist = calloc(capacity_words, sizeof(*worklist));
  if (forwarding == NULL || worklist == NULL) goto failed;

  committed_bytes = caml_mem_round_up_pages(Bsize_wsize(capacity_words));
  if (committed_bytes < Bsize_wsize(capacity_words)
      || committed_bytes > heap->reserved_space_bytes) {
    goto failed;
  }
  extra_bytes = committed_bytes - heap->committed_space_bytes;
  if (extra_bytes != 0) {
    char *first = (char *)heap->data_start[0]
      + heap->committed_space_bytes;
    char *second = (char *)heap->data_start[1]
      + heap->committed_space_bytes;

    if (caml_mem_commit(first, extra_bytes) == NULL) goto failed;
    if (caml_mem_commit(second, extra_bytes) == NULL) {
      caml_mem_decommit(first, extra_bytes);
      goto failed;
    }
  }

  for (unsigned space = 0; space < 2; space++) {
    free(heap->shadow_headers[space]);
    heap->shadow_headers[space] = shadow_headers[space];
  }
  free(heap->forwarding);
  free(heap->worklist);
  heap->forwarding = forwarding;
  heap->worklist = worklist;
  heap->committed_space_bytes = committed_bytes;
  return 1;

failed:
  free(worklist);
  free(forwarding);
  free(shadow_headers[1]);
  free(shadow_headers[0]);
  return 0;
}

static int actor_heap_collect_to_capacity(struct caml_actor_heap *heap,
                                          mlsize_t capacity_words)
{
  struct caml_actor_heap_verify_result verification;
  struct actor_gc_context context;
  caml_domain_state *domain = Caml_state_opt;
  value *old_target_end;

  if (heap == NULL || domain == NULL || domain->actor_heap != heap
      || !heap->active || !caml_domain_alone()
      || !caml_actor_world_is_frozen()
      || capacity_words < heap->capacity_words
      || capacity_words > heap->quota_words) {
    return 0;
  }
  verification = caml_actor_heap_verify(heap);
  if (verification.error != CAML_ACTOR_HEAP_VERIFY_OK) return 0;
  if (!actor_heap_prepare_capacity(heap, capacity_words)) return 0;

  memset(&context, 0, sizeof(context));
  context.heap = heap;
  context.from_space = heap->active_space;
  context.to_space = 1 - heap->active_space;
  context.capacity_words = capacity_words;
  old_target_end = heap->data_end[context.to_space];
  heap->data_end[context.to_space] =
    heap->data_start[context.to_space] + capacity_words;
  context.to_cursor = heap->data_end[context.to_space];
  context.valid = 1;
  caml_do_local_roots(
    validate_gc_root, 0, &context,
    domain->local_roots, domain->current_stack, domain->gc_regs);
  if (!context.valid) {
    heap->data_end[context.to_space] = old_target_end;
    return 0;
  }

  memset(heap->shadow_headers[context.to_space], 0,
         capacity_words * sizeof(*heap->shadow_headers[0]));
  memset(heap->forwarding, 0,
         capacity_words * sizeof(*heap->forwarding));
  caml_do_local_roots(
    forward_gc_root, 0, &context,
    domain->local_roots, domain->current_stack, domain->gc_regs);
  for (uintnat index = 0;
       context.valid && index < context.work_count; index++) {
    scan_gc_block(&context, heap->worklist[index]);
  }
  if (!context.valid) {
    caml_fatal_error("actor collector failed after root rewriting");
  }

  memset(heap->shadow_headers[context.from_space], 0,
         capacity_words * sizeof(*heap->shadow_headers[0]));
  for (unsigned space = 0; space < 2; space++) {
    heap->data_end[space] = heap->data_start[space] + capacity_words;
  }
  if (capacity_words > heap->capacity_words
      && heap->growths < CAML_UINTNAT_MAX) {
    heap->growths++;
  }
  heap->capacity_words = capacity_words;
  heap->active_space = context.to_space;
  heap->cursor = context.to_cursor;
  heap->used_words = context.live_words;
  heap->blocks = context.live_blocks;
  heap->collections++;
  verification = caml_actor_heap_verify(heap);
  if (verification.error != CAML_ACTOR_HEAP_VERIFY_OK) {
    caml_fatal_error("actor collector produced an invalid heap");
  }
  context.valid = 1;
  caml_do_local_roots(
    validate_gc_root, 0, &context,
    domain->local_roots, domain->current_stack, domain->gc_regs);
  if (!context.valid) {
    caml_fatal_error("actor collector left a stale root");
  }
  return 1;
}

int caml_actor_heap_collect(struct caml_actor_heap *heap)
{
  return heap != NULL
    && actor_heap_collect_to_capacity(heap, heap->capacity_words);
}

int caml_actor_heap_reserve(struct caml_actor_heap *heap,
                            mlsize_t words)
{
  if (heap == NULL || Caml_state_opt == NULL
      || Caml_state_opt->actor_heap != heap || !heap->active
      || words > heap->quota_words) {
    return 0;
  }
  if (heap->used_words <= heap->capacity_words
      && words <= heap->capacity_words - heap->used_words) {
    return 1;
  }
  if (!caml_actor_heap_collect(heap)) return 0;
  if (words <= heap->capacity_words - heap->used_words) return 1;
  if (heap->used_words > heap->quota_words - words) return 0;

  mlsize_t required_words = heap->used_words + words;
  mlsize_t capacity_words = heap->capacity_words;

  while (capacity_words < required_words) {
    capacity_words = capacity_words > heap->quota_words - capacity_words
      ? heap->quota_words : 2 * capacity_words;
  }
  return actor_heap_collect_to_capacity(heap, capacity_words)
    && words <= heap->capacity_words - heap->used_words;
}

static enum caml_actor_heap_verify_error verify_edge(
  const struct caml_actor_heap *source, mlsize_t source_field,
  value target, struct caml_actor_heap_verify_result *result)
{
  struct actor_value_lookup lookup;

  if (Is_long(target)) return CAML_ACTOR_HEAP_VERIFY_OK;
  result->source_field = source_field;
  if (target == 0) return CAML_ACTOR_HEAP_VERIFY_INVALID_EDGE;

  lookup_actor_value(target, &lookup);
  if (lookup.heap != NULL) {
    if (lookup.malformed || !lookup.exact) {
      return CAML_ACTOR_HEAP_VERIFY_INVALID_EDGE;
    }
    if (lookup.heap != source) {
      result->error = CAML_ACTOR_HEAP_VERIFY_FOREIGN_EDGE;
      result->source_owner = source->owner;
      result->target_owner = lookup.heap->owner;
      result->source_field = source_field;
      return result->error;
    }
    return CAML_ACTOR_HEAP_VERIFY_OK;
  }

  if (canonical_atom(target)) return CAML_ACTOR_HEAP_VERIFY_OK;
  if (caml_actor_world_value_is_approved(target)) {
    return CAML_ACTOR_HEAP_VERIFY_OK;
  }
  if (Is_young(target)) return CAML_ACTOR_HEAP_VERIFY_HOST_YOUNG_EDGE;
  return CAML_ACTOR_HEAP_VERIFY_UNAPPROVED_EXTERNAL_EDGE;
}

static enum caml_actor_heap_verify_error verify_closure(
  const struct caml_actor_heap *heap, value closure, mlsize_t wosize,
  mlsize_t block_offset, struct caml_actor_heap_verify_result *result)
{
  value info = Closinfo_val(closure);
  mlsize_t start;

  if (!Is_long(info)) return CAML_ACTOR_HEAP_VERIFY_MALFORMED;
  start = Start_env_closinfo(info);
  if (start < 2 || start > wosize || (start - 2) % 3 != 0) {
    return CAML_ACTOR_HEAP_VERIFY_MALFORMED;
  }
  if (!valid_code_pointer((value)Code_val(closure))) {
    return CAML_ACTOR_HEAP_VERIFY_INVALID_CODE_POINTER;
  }

  for (mlsize_t header_field = 2; header_field < start;
       header_field += 3) {
    header_t header = (header_t)Field(closure, header_field);
    value nested_info;

    if (Tag_hd(header) != Infix_tag || Reserved_hd(header) != 0
        || Color_hd(header) != 0
        || Wosize_hd(header) != header_field + 1
        || !valid_code_pointer(Field(closure, header_field + 1))) {
      return CAML_ACTOR_HEAP_VERIFY_MALFORMED;
    }
    nested_info = Field(closure, header_field + 2);
    if (!Is_long(nested_info)
        || Start_env_closinfo(nested_info) != start - (header_field + 1)) {
      return CAML_ACTOR_HEAP_VERIFY_MALFORMED;
    }
  }

  for (mlsize_t field = start; field < wosize; field++) {
    enum caml_actor_heap_verify_error error = verify_edge(
      heap, block_offset + 1 + field, Field(closure, field), result);
    if (error != CAML_ACTOR_HEAP_VERIFY_OK) return error;
  }
  return CAML_ACTOR_HEAP_VERIFY_OK;
}

static enum caml_actor_heap_verify_error verify_string(value string,
                                                        mlsize_t wosize)
{
  mlsize_t bytes = Bsize_wsize(wosize);
  unsigned padding = Byte_u(string, bytes - 1);

  if (padding >= sizeof(value)
      || Byte_u(string, bytes - 1 - padding) != 0) {
    return CAML_ACTOR_HEAP_VERIFY_MALFORMED;
  }
  return CAML_ACTOR_HEAP_VERIFY_OK;
}

struct caml_actor_heap *caml_actor_heap_create_sized(
  uintnat owner, mlsize_t initial_words, mlsize_t maximum_words)
{
  struct caml_actor_heap *heap;
  uintnat initial_bytes;
  uintnat maximum_bytes;
  uintnat committed_bytes;
  uintnat reserved_bytes;
  uintnat guards_bytes;
  uintnat mapping_bytes;
  char *mapping;
  header_t *shadow_headers[2] = { NULL, NULL };
  value *forwarding = NULL;
  value *worklist = NULL;

  if (!actor_runtime_supported()
      || Caml_state_opt == NULL || !caml_domain_alone()
      || initial_words == 0 || initial_words > maximum_words
      || mul_overflows_uintnat(maximum_words, sizeof(value))
      || mul_overflows_uintnat(maximum_words, sizeof(header_t))) {
    return NULL;
  }
  initial_bytes = Bsize_wsize(initial_words);
  maximum_bytes = Bsize_wsize(maximum_words);
  committed_bytes = caml_mem_round_up_pages(initial_bytes);
  reserved_bytes = caml_mem_round_up_pages(maximum_bytes);
  if (committed_bytes < initial_bytes || reserved_bytes < maximum_bytes
      || mul_overflows_uintnat(3, caml_plat_pagesize)
      || mul_overflows_uintnat(2, reserved_bytes)) {
    return NULL;
  }
  guards_bytes = 3 * caml_plat_pagesize;
  if (add_overflows_uintnat(2 * reserved_bytes, guards_bytes)) return NULL;
  mapping_bytes = 2 * reserved_bytes + guards_bytes;

  heap = malloc(sizeof(*heap));
  if (heap == NULL) return NULL;
  for (unsigned space = 0; space < 2; space++) {
    shadow_headers[space] = calloc(
      initial_words, sizeof(*shadow_headers[space]));
    if (shadow_headers[space] == NULL) goto metadata_failed;
  }
  forwarding = calloc(initial_words, sizeof(*forwarding));
  worklist = calloc(initial_words, sizeof(*worklist));
  if (forwarding == NULL || worklist == NULL) goto metadata_failed;
  mapping = caml_mem_map(mapping_bytes, 1);
  if (mapping == NULL) {
    goto metadata_failed;
  }
  if (caml_mem_commit(mapping + caml_plat_pagesize,
                      committed_bytes) == NULL
      || caml_mem_commit(
           mapping + 2 * caml_plat_pagesize + reserved_bytes,
           committed_bytes) == NULL) {
    caml_mem_unmap(mapping, mapping_bytes);
    goto metadata_failed;
  }

  heap->owner = owner;
  heap->mapping = mapping;
  heap->mapping_bytes = mapping_bytes;
  heap->reserved_space_bytes = reserved_bytes;
  heap->committed_space_bytes = committed_bytes;
  heap->data_start[0] = (value *)(mapping + caml_plat_pagesize);
  heap->data_start[1] = (value *)(
    mapping + 2 * caml_plat_pagesize + reserved_bytes);
  for (unsigned space = 0; space < 2; space++) {
    heap->data_end[space] = heap->data_start[space] + initial_words;
    heap->shadow_headers[space] = shadow_headers[space];
  }
  heap->active_space = 0;
  heap->cursor = heap->data_end[0];
  heap->forwarding = forwarding;
  heap->worklist = worklist;
  heap->capacity_words = initial_words;
  heap->quota_words = maximum_words;
  heap->used_words = 0;
  heap->blocks = 0;
  heap->shared_bypasses = 0;
  heap->collections = 0;
  heap->growths = 0;
  heap->active = 0;

  caml_plat_lock_non_blocking(&actor_heaps_lock);
  for (struct caml_actor_heap *other = actor_heaps;
       other != NULL; other = other->next) {
    if (other->owner == owner) {
      caml_plat_unlock(&actor_heaps_lock);
      caml_mem_unmap(mapping, mapping_bytes);
      free(worklist);
      free(forwarding);
      free(shadow_headers[1]);
      free(shadow_headers[0]);
      free(heap);
      return NULL;
    }
  }
  heap->next = actor_heaps;
  actor_heaps = heap;
  caml_plat_unlock(&actor_heaps_lock);
  return heap;

metadata_failed:
  free(worklist);
  free(forwarding);
  free(shadow_headers[1]);
  free(shadow_headers[0]);
  free(heap);
  return NULL;
}

struct caml_actor_heap *caml_actor_heap_create(uintnat owner,
                                                mlsize_t quota_words)
{
  return caml_actor_heap_create_sized(owner, quota_words, quota_words);
}

void caml_actor_heap_destroy(struct caml_actor_heap *heap)
{
  struct caml_actor_heap **cursor;
  int found = 0;

  if (heap == NULL) return;
  if (Caml_state_opt == NULL || !caml_domain_alone()) {
    caml_fatal_error("actor heaps require a single running Domain");
  }
  if (Caml_state_opt != NULL && Caml_state_opt->actor_heap == heap) {
    caml_fatal_error("attempt to destroy the active actor heap");
  }

  caml_plat_lock_non_blocking(&actor_heaps_lock);
  for (cursor = &actor_heaps; *cursor != NULL; cursor = &(*cursor)->next) {
    if (*cursor == heap) {
      if (heap->active) {
        caml_plat_unlock(&actor_heaps_lock);
        caml_fatal_error("attempt to destroy an active actor heap");
      }
      *cursor = heap->next;
      found = 1;
      break;
    }
  }
  caml_plat_unlock(&actor_heaps_lock);

  if (!found) caml_fatal_error("attempt to destroy an unknown actor heap");

  retire_actor_mapping(heap);
  free(heap->worklist);
  free(heap->forwarding);
  free(heap->shadow_headers[1]);
  free(heap->shadow_headers[0]);
  free(heap);
}

int caml_actor_heap_activate(struct caml_actor_heap *heap)
{
  caml_domain_state *domain_state = Caml_state;
  int activated = 0;

  if (!actor_runtime_supported() || heap == NULL || !caml_domain_alone()
      || domain_state->actor_heap != NULL) return 0;

  caml_plat_lock_non_blocking(&actor_heaps_lock);
  for (struct caml_actor_heap *registered = actor_heaps;
       registered != NULL; registered = registered->next) {
    if (registered == heap && !registered->active) {
      registered->active = 1;
      activated = 1;
      break;
    }
  }
  caml_plat_unlock(&actor_heaps_lock);
  if (!activated) return 0;
  domain_state->actor_heap = heap;
  return 1;
}

void caml_actor_heap_deactivate(void)
{
  caml_domain_state *domain_state = Caml_state;
  struct caml_actor_heap *heap = domain_state->actor_heap;

  if (heap == NULL) return;
  heap->active = 0;
  domain_state->actor_heap = NULL;
}

struct caml_actor_heap *caml_actor_heap_current(void)
{
  return Caml_state_opt == NULL ? NULL : Caml_state_opt->actor_heap;
}

value caml_actor_heap_try_alloc(struct caml_actor_heap *heap,
                                mlsize_t wosize, tag_t tag,
                                reserved_t reserved,
                                enum caml_actor_heap_alloc_error *error)
{
  mlsize_t whsize;
  mlsize_t header_offset;
  header_t actor_header;
  value *header;
  caml_domain_state *domain_state = Caml_state_opt;

  if (error != NULL) *error = CAML_ACTOR_HEAP_ALLOC_UNSUPPORTED;
  if (heap == NULL || domain_state == NULL || !caml_domain_alone()
      || domain_state->actor_heap != heap || !heap->active
      || !actor_tag_supported(wosize, tag, reserved)) return 0;
  if (wosize > CAML_UINTNAT_MAX - 1) return 0;
  whsize = Whsize_wosize(wosize);
  if (!caml_actor_heap_reserve(heap, whsize)) {
    if (error != NULL) *error = CAML_ACTOR_HEAP_ALLOC_QUOTA;
    return 0;
  }

  header = heap->cursor - whsize;
  header_offset = header - heap->data_start[heap->active_space];
  actor_header = Make_header_with_reserved(
    wosize, tag, NOT_MARKABLE, reserved);
  CAMLassert(
    heap->shadow_headers[heap->active_space][header_offset] == 0);
  Hd_hp(header) = actor_header;
  heap->shadow_headers[heap->active_space][header_offset] = actor_header;
#ifdef DEBUG
  for (mlsize_t field = 0; field < wosize; field++) {
    Op_hp(header)[field] = Debug_uninit_major;
  }
#endif
  heap->cursor = header;
  heap->used_words += whsize;
  heap->blocks++;
  if (error != NULL) *error = CAML_ACTOR_HEAP_ALLOC_OK;
  return Val_hp(header);
}

value caml_actor_heap_alloc_or_raise(struct caml_actor_heap *heap,
                                     mlsize_t wosize, tag_t tag,
                                     reserved_t reserved)
{
  enum caml_actor_heap_alloc_error error;
  value result = caml_actor_heap_try_alloc(
    heap, wosize, tag, reserved, &error);

  if (result != 0) return result;
  if (error == CAML_ACTOR_HEAP_ALLOC_QUOTA) caml_raise_out_of_memory();
  caml_fatal_error("unsupported allocation while an actor heap is active");
}

int caml_actor_heap_owns_value(const struct caml_actor_heap *heap,
                               value candidate)
{
  struct actor_value_lookup lookup;

  if (!Is_block(candidate)) return 0;
  lookup_actor_value(candidate, &lookup);
  return lookup.heap == heap && lookup.exact && !lookup.malformed;
}

int caml_actor_heap_contains_address(value candidate)
{
  struct actor_value_lookup lookup;

  if (!Is_block(candidate)) return 0;
  lookup_actor_value(candidate, &lookup);
  return lookup.heap != NULL;
}

int caml_actor_heap_value_is_storable(value candidate)
{
  struct caml_actor_heap *heap = caml_actor_heap_current();

  return heap != NULL && actor_store_value_supported(heap, candidate);
}

uintnat caml_actor_heap_owner(const struct caml_actor_heap *heap)
{
  return heap->owner;
}

mlsize_t caml_actor_heap_quota_words(const struct caml_actor_heap *heap)
{
  return heap->quota_words;
}

mlsize_t caml_actor_heap_capacity_words(const struct caml_actor_heap *heap)
{
  return heap->capacity_words;
}

mlsize_t caml_actor_heap_used_words(const struct caml_actor_heap *heap)
{
  return heap->used_words;
}

uintnat caml_actor_heap_blocks(const struct caml_actor_heap *heap)
{
  return heap->blocks;
}

uintnat caml_actor_heap_shared_bypasses(const struct caml_actor_heap *heap)
{
  return heap->shared_bypasses;
}

uintnat caml_actor_heap_collections(const struct caml_actor_heap *heap)
{
  return heap->collections;
}

uintnat caml_actor_heap_growths(const struct caml_actor_heap *heap)
{
  return heap->growths;
}

void caml_actor_heap_note_shared_bypass(struct caml_actor_heap *heap)
{
  heap->shared_bypasses++;
}

static int actor_store_value_supported(struct caml_actor_heap *heap,
                                       value new_value)
{
  struct actor_space_lookup lookup;
  header_t expected;

  if (Is_long(new_value)) return 1;
  if (new_value == 0) return 0;
  if (heap != NULL && heap->active_space <= 1) {
    lookup_space_value(heap, heap->active_space, new_value, &lookup);
    if (lookup.exact) {
      expected = heap->shadow_headers[heap->active_space]
        [lookup.header_offset];
      return expected != 0 && Hd_val(lookup.base) == expected;
    }
  }
  return canonical_atom(new_value)
    || caml_actor_world_value_is_approved(new_value);
}

static int current_actor_value(
  struct caml_actor_heap *heap, value candidate,
  struct actor_space_lookup *lookup, header_t *header)
{
  header_t expected;

  if (heap == NULL || lookup == NULL || header == NULL
      || heap->active_space > 1 || !Is_block(candidate)) {
    return 0;
  }
  lookup_space_value(heap, heap->active_space, candidate, lookup);
  if (!lookup->exact) return 0;
  expected = heap->shadow_headers[heap->active_space]
    [lookup->header_offset];
  if (expected == 0 || Hd_val(lookup->base) != expected) return 0;
  *header = expected;
  return 1;
}

static int current_actor_block(value block, value *canonical,
                               mlsize_t *wosize, tag_t *tag)
{
  struct caml_actor_heap *heap = caml_actor_heap_current();
  struct actor_space_lookup lookup;
  header_t header;

  if (!current_actor_value(heap, block, &lookup, &header)
      || lookup.infix_offset != 0 || lookup.base != block) {
    return 0;
  }
  *canonical = lookup.base;
  *wosize = Wosize_hd(header);
  *tag = Tag_hd(header);
  return 1;
}

int caml_actor_heap_read_field(value block, mlsize_t field,
                               value *result)
{
  struct caml_actor_heap *heap = caml_actor_heap_current();
  value canonical;
  value current;
  mlsize_t wosize;
  tag_t tag;

  if (heap == NULL || result == NULL
      || !current_actor_block(block, &canonical, &wosize, &tag)
      || tag >= Forcing_tag || field >= wosize) {
    return 0;
  }
  current = Field(canonical, field);
  if (!actor_store_value_supported(heap, current)) return 0;
  *result = current;
  return 1;
}

int caml_actor_heap_read_closure_env(value closure, mlsize_t field,
                                     value *result)
{
  struct caml_actor_heap *heap = caml_actor_heap_current();
  struct actor_space_lookup lookup;
  header_t header;
  uintptr_t byte_offset;
  mlsize_t word_offset;
  mlsize_t wosize;
  mlsize_t environment_start;
  value info;
  value current;

  if (result == NULL
      || !current_actor_value(heap, closure, &lookup, &header)
      || Tag_hd(header) != Closure_tag
      || (uintptr_t)closure < (uintptr_t)lookup.base) {
    return 0;
  }
  byte_offset = (uintptr_t)closure - (uintptr_t)lookup.base;
  if (byte_offset % sizeof(value) != 0) return 0;
  word_offset = byte_offset / sizeof(value);
  wosize = Wosize_hd(header);
  if (word_offset > wosize || wosize - word_offset <= 1) return 0;
  info = Closinfo_val(closure);
  if (!Is_long(info)) return 0;
  environment_start = Start_env_closinfo(info);
  if (environment_start < 2 || field < environment_start
      || field >= wosize - word_offset) {
    return 0;
  }
  current = Field(closure, field);
  if (!actor_store_value_supported(heap, current)) return 0;
  *result = current;
  return 1;
}

int caml_actor_heap_closure_wosize(value closure, mlsize_t *wosize)
{
  struct caml_actor_heap *heap = caml_actor_heap_current();
  struct actor_space_lookup lookup;
  header_t header;
  value info;
  mlsize_t size;

  if (wosize == NULL
      || !current_actor_value(heap, closure, &lookup, &header)
      || lookup.infix_offset != 0 || lookup.base != closure
      || Tag_hd(header) != Closure_tag) {
    return 0;
  }
  size = Wosize_hd(header);
  if (size < 2) return 0;
  info = Closinfo_val(closure);
  if (!Is_long(info) || Start_env_closinfo(info) < 2
      || Start_env_closinfo(info) > size) {
    return 0;
  }
  *wosize = size;
  return 1;
}

enum caml_actor_heap_store_status caml_actor_heap_check_field_store(
  value block, mlsize_t field, value new_value)
{
  struct caml_actor_heap *heap = caml_actor_heap_current();
  value canonical;
  mlsize_t wosize;
  tag_t tag;

  if (heap == NULL) return CAML_ACTOR_HEAP_STORE_INACTIVE;
  if (!current_actor_block(block, &canonical, &wosize, &tag)
      || tag >= Forcing_tag || field >= wosize
      || !actor_store_value_supported(heap, new_value)) {
    return CAML_ACTOR_HEAP_STORE_INVALID;
  }
  return CAML_ACTOR_HEAP_STORE_OK;
}

enum caml_actor_heap_store_status caml_actor_heap_check_vector_store(
  value block, mlsize_t field, value new_value)
{
  value canonical;
  mlsize_t wosize;
  tag_t tag;

  if (caml_actor_heap_current() == NULL) {
    return CAML_ACTOR_HEAP_STORE_INACTIVE;
  }
  if (!current_actor_block(block, &canonical, &wosize, &tag)
      || tag != 0 || field >= wosize) {
    return CAML_ACTOR_HEAP_STORE_INVALID;
  }
  return caml_actor_heap_check_field_store(block, field, new_value);
}

enum caml_actor_heap_store_status caml_actor_heap_check_bytes_store(
  value block, mlsize_t byte)
{
  value canonical;
  mlsize_t wosize;
  tag_t tag;

  if (caml_actor_heap_current() == NULL) {
    return CAML_ACTOR_HEAP_STORE_INACTIVE;
  }
  if (!current_actor_block(block, &canonical, &wosize, &tag)
      || tag != String_tag || byte >= caml_string_length(canonical)) {
    return CAML_ACTOR_HEAP_STORE_INVALID;
  }
  return CAML_ACTOR_HEAP_STORE_OK;
}

enum caml_actor_heap_store_status caml_actor_heap_check_double_store(
  value block, mlsize_t field)
{
  value canonical;
  mlsize_t wosize;
  tag_t tag;

  if (caml_actor_heap_current() == NULL) {
    return CAML_ACTOR_HEAP_STORE_INACTIVE;
  }
  if (!current_actor_block(block, &canonical, &wosize, &tag)
      || tag != Double_array_tag
      || field >= wosize / Double_wosize) {
    return CAML_ACTOR_HEAP_STORE_INVALID;
  }
  return CAML_ACTOR_HEAP_STORE_OK;
}

enum caml_actor_heap_store_status caml_actor_heap_check_offsetref(
  value block)
{
  value canonical;
  mlsize_t wosize;
  tag_t tag;

  if (caml_actor_heap_current() == NULL) {
    return CAML_ACTOR_HEAP_STORE_INACTIVE;
  }
  if (!current_actor_block(block, &canonical, &wosize, &tag)
      || tag != 0 || wosize != 1 || !Is_long(Field(canonical, 0))) {
    return CAML_ACTOR_HEAP_STORE_INVALID;
  }
  return CAML_ACTOR_HEAP_STORE_OK;
}

enum caml_actor_heap_store_status caml_actor_heap_check_store(
  const volatile value *field, value new_value)
{
  struct caml_actor_heap *heap = caml_actor_heap_current();
  int target_is_field = 0;

  if (heap == NULL) return CAML_ACTOR_HEAP_STORE_INACTIVE;
  if (validate_layout(heap) != CAML_ACTOR_HEAP_VERIFY_OK) {
    return CAML_ACTOR_HEAP_STORE_INVALID;
  }

  for (value *cursor = heap->cursor;
       cursor < heap->data_end[heap->active_space]; ) {
    mlsize_t wosize = Wosize_hp(cursor);
    const volatile value *first = Op_hp(cursor);
    const volatile value *end = first + wosize;
    uintptr_t field_address = (uintptr_t)field;

    if (field_address >= (uintptr_t)first
        && field_address < (uintptr_t)end
        && (field_address - (uintptr_t)first) % sizeof(value) == 0) {
      target_is_field = 1;
      break;
    }
    cursor += Whsize_wosize(wosize);
  }
  if (!target_is_field) return CAML_ACTOR_HEAP_STORE_INVALID;
  return actor_store_value_supported(heap, new_value)
    ? CAML_ACTOR_HEAP_STORE_OK : CAML_ACTOR_HEAP_STORE_INVALID;
}

struct caml_actor_heap_verify_result caml_actor_heap_verify(
  const struct caml_actor_heap *heap)
{
  struct caml_actor_heap_verify_result result = {
    CAML_ACTOR_HEAP_VERIFY_OK, 0, 0, 0
  };
  enum caml_actor_heap_verify_error error;
  mlsize_t block_offset = 0;

  if (heap == NULL || Caml_state_opt == NULL || !caml_domain_alone()
      || !actor_heap_registered(heap)) {
    result.error = CAML_ACTOR_HEAP_VERIFY_MALFORMED;
    return result;
  }
  result.source_owner = heap->owner;
  error = validate_layout(heap);
  if (error != CAML_ACTOR_HEAP_VERIFY_OK) {
    result.error = error;
    return result;
  }

  for (value *cursor = heap->cursor;
       cursor < heap->data_end[heap->active_space]; ) {
    mlsize_t wosize = Wosize_hp(cursor);
    tag_t tag = Tag_hp(cursor);
    value block = Val_hp(cursor);

    if (tag < Forcing_tag) {
      for (mlsize_t field = 0; field < wosize; field++) {
        error = verify_edge(
          heap, block_offset + 1 + field, Field(block, field), &result);
        if (error != CAML_ACTOR_HEAP_VERIFY_OK) goto verification_failed;
      }
    } else if (tag == Closure_tag) {
      error = verify_closure(
        heap, block, wosize, block_offset, &result);
      if (error != CAML_ACTOR_HEAP_VERIFY_OK) goto verification_failed;
    } else if (tag == String_tag) {
      error = verify_string(block, wosize);
      if (error != CAML_ACTOR_HEAP_VERIFY_OK) goto verification_failed;
    }

    block_offset += Whsize_wosize(wosize);
    cursor += Whsize_wosize(wosize);
  }
  return result;

verification_failed:
  if (result.error == CAML_ACTOR_HEAP_VERIFY_OK) result.error = error;
  result.source_owner = heap->owner;
  return result;
}
