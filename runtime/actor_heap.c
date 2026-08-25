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

#include "caml/actor_heap.h"
#include "caml/address_class.h"
#include "caml/codefrag.h"
#include "caml/domain.h"
#include "caml/domain_state.h"
#include "caml/fail.h"
#include "caml/misc.h"
#include "caml/platform.h"
#include "caml/shared_heap.h"

struct caml_actor_heap {
  uintnat owner;
  char *mapping;
  uintnat mapping_bytes;
  value *data_start;
  value *data_end;
  value *cursor;
  header_t *shadow_headers;
  mlsize_t quota_words;
  mlsize_t used_words;
  uintnat blocks;
  uintnat shared_bypasses;
  int active;
  struct caml_actor_heap *next;
};

static struct caml_actor_heap *actor_heaps;
static caml_plat_mutex actor_heaps_lock = CAML_PLAT_MUTEX_INITIALIZER;

struct actor_value_lookup {
  struct caml_actor_heap *heap;
  value canonical;
  int exact;
  int malformed;
};

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

static int range_contains(const struct caml_actor_heap *heap,
                          uintptr_t address)
{
  uintptr_t start = (uintptr_t)heap->mapping;
  uintptr_t end = start + heap->mapping_bytes;
  return address >= start && address < end;
}

static enum caml_actor_heap_verify_error validate_layout(
  const struct caml_actor_heap *heap)
{
  value *cursor = heap->cursor;
  uintnat blocks = 0;

  if (cursor < heap->data_start || cursor > heap->data_end
      || (mlsize_t)(heap->data_end - cursor) != heap->used_words) {
    return CAML_ACTOR_HEAP_VERIFY_MALFORMED;
  }

  while (cursor < heap->data_end) {
    mlsize_t remaining = heap->data_end - cursor;
    mlsize_t offset = cursor - heap->data_start;
    header_t header = Hd_hp(cursor);
    header_t expected = heap->shadow_headers[offset];
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

  if (cursor != heap->data_end || blocks != heap->blocks) {
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
    if (validate_layout(heap) != CAML_ACTOR_HEAP_VERIFY_OK) {
      lookup->malformed = 1;
      break;
    }

    for (value *cursor = heap->cursor; cursor < heap->data_end; ) {
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

struct caml_actor_heap *caml_actor_heap_create(uintnat owner,
                                                mlsize_t quota_words)
{
  struct caml_actor_heap *heap;
  uintnat quota_bytes;
  uintnat committed_bytes;
  uintnat guards_bytes;
  uintnat mapping_bytes;
  char *mapping;
  header_t *shadow_headers;

  if (!actor_runtime_supported()
      || Caml_state_opt == NULL || !caml_domain_alone()
      || quota_words == 0
      || mul_overflows_uintnat(quota_words, sizeof(value))
      || mul_overflows_uintnat(quota_words, sizeof(header_t))) {
    return NULL;
  }
  quota_bytes = Bsize_wsize(quota_words);
  committed_bytes = caml_mem_round_up_pages(quota_bytes);
  if (committed_bytes < quota_bytes
      || mul_overflows_uintnat(2, caml_plat_pagesize)) {
    return NULL;
  }
  guards_bytes = 2 * caml_plat_pagesize;
  if (add_overflows_uintnat(committed_bytes, guards_bytes)) return NULL;
  mapping_bytes = committed_bytes + guards_bytes;

  heap = malloc(sizeof(*heap));
  if (heap == NULL) return NULL;
  shadow_headers = calloc(quota_words, sizeof(*shadow_headers));
  if (shadow_headers == NULL) {
    free(heap);
    return NULL;
  }
  mapping = caml_mem_map(mapping_bytes, 1);
  if (mapping == NULL) {
    free(shadow_headers);
    free(heap);
    return NULL;
  }
  if (caml_mem_commit(mapping + caml_plat_pagesize,
                      committed_bytes) == NULL) {
    caml_mem_unmap(mapping, mapping_bytes);
    free(shadow_headers);
    free(heap);
    return NULL;
  }

  heap->owner = owner;
  heap->mapping = mapping;
  heap->mapping_bytes = mapping_bytes;
  heap->data_start = (value *)(mapping + caml_plat_pagesize);
  heap->data_end = heap->data_start + quota_words;
  heap->cursor = heap->data_end;
  heap->shadow_headers = shadow_headers;
  heap->quota_words = quota_words;
  heap->used_words = 0;
  heap->blocks = 0;
  heap->shared_bypasses = 0;
  heap->active = 0;

  caml_plat_lock_non_blocking(&actor_heaps_lock);
  for (struct caml_actor_heap *other = actor_heaps;
       other != NULL; other = other->next) {
    if (other->owner == owner) {
      caml_plat_unlock(&actor_heaps_lock);
      caml_mem_unmap(mapping, mapping_bytes);
      free(shadow_headers);
      free(heap);
      return NULL;
    }
  }
  heap->next = actor_heaps;
  actor_heaps = heap;
  caml_plat_unlock(&actor_heaps_lock);
  return heap;
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

  caml_mem_unmap(heap->mapping, heap->mapping_bytes);
  free(heap->shadow_headers);
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
  if (whsize > heap->quota_words - heap->used_words) {
    if (error != NULL) *error = CAML_ACTOR_HEAP_ALLOC_QUOTA;
    return 0;
  }

  header = heap->cursor - whsize;
  header_offset = header - heap->data_start;
  actor_header = Make_header_with_reserved(
    wosize, tag, NOT_MARKABLE, reserved);
  CAMLassert(heap->shadow_headers[header_offset] == 0);
  Hd_hp(header) = actor_header;
  heap->shadow_headers[header_offset] = actor_header;
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

uintnat caml_actor_heap_owner(const struct caml_actor_heap *heap)
{
  return heap->owner;
}

mlsize_t caml_actor_heap_quota_words(const struct caml_actor_heap *heap)
{
  return heap->quota_words;
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

void caml_actor_heap_note_shared_bypass(struct caml_actor_heap *heap)
{
  heap->shared_bypasses++;
}

enum caml_actor_heap_store_status caml_actor_heap_check_store(
  const volatile value *field, value new_value)
{
  struct caml_actor_heap *heap = caml_actor_heap_current();
  struct actor_value_lookup lookup;
  int target_is_field = 0;

  if (heap == NULL) return CAML_ACTOR_HEAP_STORE_INACTIVE;
  if (validate_layout(heap) != CAML_ACTOR_HEAP_VERIFY_OK) {
    return CAML_ACTOR_HEAP_STORE_INVALID;
  }

  for (value *cursor = heap->cursor; cursor < heap->data_end; ) {
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
  if (Is_long(new_value)) return CAML_ACTOR_HEAP_STORE_OK;
  if (new_value == 0) return CAML_ACTOR_HEAP_STORE_INVALID;

  lookup_actor_value(new_value, &lookup);
  if (lookup.heap != NULL) {
    return lookup.heap == heap && lookup.exact && !lookup.malformed
      ? CAML_ACTOR_HEAP_STORE_OK : CAML_ACTOR_HEAP_STORE_INVALID;
  }
  if (canonical_atom(new_value)) return CAML_ACTOR_HEAP_STORE_OK;
  return CAML_ACTOR_HEAP_STORE_INVALID;
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

  for (value *cursor = heap->cursor; cursor < heap->data_end; ) {
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
