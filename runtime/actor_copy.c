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

#include "caml/actor_copy.h"
#include "caml/actor_heap.h"
#include "caml/actor_world.h"
#include "caml/codefrag.h"
#include "caml/domain.h"
#include "caml/domain_state.h"
#include "caml/fail.h"
#include "caml/finalise.h"
#include "caml/shared_heap.h"

enum actor_copy_origin {
  ACTOR_COPY_FROM_ACTOR,
  ACTOR_COPY_FROM_HOST
};

struct actor_copy_node {
  value source;
  value target;
  header_t source_header;
  mlsize_t wosize;
  mlsize_t closure_env;
  tag_t tag;
  enum actor_copy_origin origin;
};

struct actor_copy_map_entry {
  value source;
  uintnat index_plus_one;
};

struct actor_copy_context {
  struct caml_actor_heap *source_heap;
  struct actor_copy_node *nodes;
  uintnat node_count;
  uintnat node_capacity;
  struct actor_copy_map_entry *map;
  uintnat map_capacity;
  uintnat map_used;
  mlsize_t copied_words;
  mlsize_t quota_words;
  enum caml_actor_copy_status status;
};

struct actor_copy_reference {
  value base;
  mlsize_t infix_offset;
  enum actor_copy_origin origin;
};

static struct caml_actor_copy_result copy_failure(
  enum caml_actor_copy_status status)
{
  struct caml_actor_copy_result result = { status, NULL, 0 };
  return result;
}

const char *caml_actor_copy_status_message(
  enum caml_actor_copy_status status)
{
  switch (status) {
  case CAML_ACTOR_COPY_OK:
    return "ok";
  case CAML_ACTOR_COPY_UNSUPPORTED_RUNTIME:
    return "unsupported actor runtime";
  case CAML_ACTOR_COPY_INVALID_SOURCE:
    return "invalid capture source";
  case CAML_ACTOR_COPY_UNSUPPORTED_TAG:
    return "unsupported captured value";
  case CAML_ACTOR_COPY_INVALID_CLOSURE:
    return "malformed captured closure";
  case CAML_ACTOR_COPY_INVALID_CODE_POINTER:
    return "invalid captured code pointer";
  case CAML_ACTOR_COPY_FINALISABLE:
    return "finalisable captured value";
  case CAML_ACTOR_COPY_GRAPH_TOO_LARGE:
    return "initial actor heap limit";
  case CAML_ACTOR_COPY_RESOURCE_UNAVAILABLE:
    return "actor copy metadata unavailable";
  case CAML_ACTOR_COPY_INTERNAL:
  default:
    return "actor copy invariant failure";
  }
}

static int actor_copy_runtime_supported(void)
{
#if defined(NATIVE_CODE) || !defined(__x86_64__) || !defined(__linux__)
  return 0;
#else
  return 1;
#endif
}

static int canonical_atom(value candidate)
{
  for (uintnat tag = 0; tag < Num_tags; tag++) {
    if (candidate == Atom((tag_t)tag)) return 1;
  }
  return 0;
}

static uintnat copy_hash(value source)
{
  uintnat hash = ((uintnat)source) >> 3;

  hash ^= hash >> 11;
  hash *= (uintnat)2654435761U;
  hash ^= hash >> 13;
  return hash;
}

static int map_lookup(const struct actor_copy_context *context,
                      value source, uintnat *index)
{
  uintnat mask;
  uintnat slot;

  if (context->map_capacity == 0) return 0;
  mask = context->map_capacity - 1;
  slot = copy_hash(source) & mask;
  while (context->map[slot].index_plus_one != 0) {
    if (context->map[slot].source == source) {
      *index = context->map[slot].index_plus_one - 1;
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

static void map_insert_unchecked(struct actor_copy_context *context,
                                 value source, uintnat index)
{
  uintnat mask = context->map_capacity - 1;
  uintnat slot = copy_hash(source) & mask;

  while (context->map[slot].index_plus_one != 0) {
    slot = (slot + 1) & mask;
  }
  context->map[slot].source = source;
  context->map[slot].index_plus_one = index + 1;
  context->map_used++;
}

static int reserve_map(struct actor_copy_context *context)
{
  struct actor_copy_map_entry *old_map = context->map;
  uintnat old_capacity = context->map_capacity;
  uintnat new_capacity;

  if (old_capacity != 0 && context->map_used + 1 <= old_capacity / 2) {
    return 1;
  }
  new_capacity = old_capacity == 0 ? 32 : 2 * old_capacity;
  if (new_capacity < old_capacity
      || new_capacity > SIZE_MAX / sizeof(*context->map)) {
    context->status = CAML_ACTOR_COPY_RESOURCE_UNAVAILABLE;
    return 0;
  }
  context->map = calloc(new_capacity, sizeof(*context->map));
  if (context->map == NULL) {
    context->map = old_map;
    context->status = CAML_ACTOR_COPY_RESOURCE_UNAVAILABLE;
    return 0;
  }
  context->map_capacity = new_capacity;
  context->map_used = 0;
  for (uintnat slot = 0; slot < old_capacity; slot++) {
    if (old_map[slot].index_plus_one != 0) {
      map_insert_unchecked(
        context, old_map[slot].source,
        old_map[slot].index_plus_one - 1);
    }
  }
  free(old_map);
  return 1;
}

static int reserve_node(struct actor_copy_context *context)
{
  struct actor_copy_node *nodes;
  uintnat capacity;

  if (context->node_count < context->node_capacity) return 1;
  capacity = context->node_capacity == 0
    ? 32 : 2 * context->node_capacity;
  if (capacity < context->node_capacity
      || capacity > SIZE_MAX / sizeof(*nodes)) {
    context->status = CAML_ACTOR_COPY_RESOURCE_UNAVAILABLE;
    return 0;
  }
  nodes = realloc(context->nodes, capacity * sizeof(*nodes));
  if (nodes == NULL) {
    context->status = CAML_ACTOR_COPY_RESOURCE_UNAVAILABLE;
    return 0;
  }
  context->nodes = nodes;
  context->node_capacity = capacity;
  return 1;
}

static int finalisable_table_contains(const struct finalisable *table,
                                      value candidate)
{
  if (table == NULL) return 0;
  for (uintnat index = 0; index < table->young; index++) {
    if (table->table[index].val == candidate) return 1;
  }
  return 0;
}

static int host_value_is_finalisable(value candidate)
{
  struct caml_final_info *info = Caml_state->final_info;

  for (; info != NULL; info = info->next) {
    if (finalisable_table_contains(&info->first, candidate)
        || finalisable_table_contains(&info->last, candidate)) {
      return 1;
    }
    for (struct final_todo *todo = info->todo_head;
         todo != NULL; todo = todo->next) {
      for (int index = 0; index < todo->size; index++) {
        if (todo->item[index].val == candidate) return 1;
      }
    }
  }
  return 0;
}

static int valid_code_pointer(value candidate)
{
  struct code_fragment *fragment;

  if ((uintptr_t)candidate % sizeof(opcode_t) != 0) return 0;
  fragment = caml_find_code_fragment_by_pc((char *)candidate);
  return fragment != NULL
    && (char *)candidate + sizeof(opcode_t) <= fragment->code_end;
}

static enum caml_actor_copy_status validate_closure(
  struct actor_copy_node *node)
{
  value closure = node->source;
  value info;
  mlsize_t start;

  if (node->wosize < 2) return CAML_ACTOR_COPY_INVALID_CLOSURE;
  info = Closinfo_val(closure);
  if (!Is_long(info)) return CAML_ACTOR_COPY_INVALID_CLOSURE;
  start = Start_env_closinfo(info);
  if (start < 2 || start > node->wosize || (start - 2) % 3 != 0) {
    return CAML_ACTOR_COPY_INVALID_CLOSURE;
  }
  if (!valid_code_pointer((value)Code_val(closure))) {
    return CAML_ACTOR_COPY_INVALID_CODE_POINTER;
  }

  for (mlsize_t header_field = 2; header_field < start;
       header_field += 3) {
    header_t header = (header_t)Field(closure, header_field);
    value nested_info = Field(closure, header_field + 2);

    if (Tag_hd(header) != Infix_tag || Reserved_hd(header) != 0
        || Color_hd(header) != 0
        || Wosize_hd(header) != header_field + 1
        || !Is_long(nested_info)
        || Start_env_closinfo(nested_info)
             != start - (header_field + 1)) {
      return CAML_ACTOR_COPY_INVALID_CLOSURE;
    }
    if (!valid_code_pointer(Field(closure, header_field + 1))) {
      return CAML_ACTOR_COPY_INVALID_CODE_POINTER;
    }
  }
  node->closure_env = start;
  return CAML_ACTOR_COPY_OK;
}

static int valid_infix_reference(const struct actor_copy_node *node,
                                 mlsize_t offset)
{
  mlsize_t field;
  header_t header;

  if (offset == 0) return 1;
  if (node->tag != Closure_tag || offset % sizeof(value) != 0) return 0;
  field = offset / sizeof(value);
  if (field < 3 || field >= node->closure_env
      || (field - 3) % 3 != 0) {
    return 0;
  }
  header = (header_t)Field(node->source, field - 1);
  return Tag_hd(header) == Infix_tag
    && Reserved_hd(header) == 0
    && Color_hd(header) == 0
    && Wosize_hd(header) == field;
}

static int canonicalize_source(struct actor_copy_context *context,
                               value candidate,
                               struct actor_copy_reference *reference)
{
  struct caml_heap_state *shared_heap = Caml_state->shared_heap;
  mlsize_t offset;
  value base;

  if (!Is_block(candidate) || candidate == 0 || canonical_atom(candidate)) {
    return 0;
  }

  if (context->source_heap != NULL
      && caml_actor_heap_owns_value(context->source_heap, candidate)) {
    reference->origin = ACTOR_COPY_FROM_ACTOR;
    if (Tag_val(candidate) == Infix_tag) {
      offset = Infix_offset_val(candidate);
      reference->base = candidate - offset;
      reference->infix_offset = offset;
    } else {
      reference->base = candidate;
      reference->infix_offset = 0;
    }
    return 1;
  }
  if (caml_actor_heap_contains_address(candidate)) return 0;

  if (caml_shared_heap_find_block(
        shared_heap, candidate, &base, &offset)) {
    reference->base = base;
    reference->infix_offset = offset;
    reference->origin = ACTOR_COPY_FROM_HOST;
    return 1;
  }
  if (Is_young(candidate)) return 0;
  return 0;
}

static enum caml_actor_copy_status validate_node(
  struct actor_copy_node *node)
{
  header_t header = node->source_header;

  if (Wosize_hd(header) == 0 || Reserved_hd(header) != 0
      || Tag_hd(header) == Infix_tag) {
    return CAML_ACTOR_COPY_INVALID_SOURCE;
  }
  if (node->origin == ACTOR_COPY_FROM_HOST
      && host_value_is_finalisable(node->source)) {
    return CAML_ACTOR_COPY_FINALISABLE;
  }

  if (node->tag < Forcing_tag) return CAML_ACTOR_COPY_OK;
  switch (node->tag) {
  case Closure_tag:
    return validate_closure(node);
  case String_tag: {
    mlsize_t bytes = Bsize_wsize(node->wosize);
    unsigned padding = Byte_u(node->source, bytes - 1);

    if (padding >= sizeof(value)
        || Byte_u(node->source, bytes - 1 - padding) != 0) {
      return CAML_ACTOR_COPY_INVALID_SOURCE;
    }
    return CAML_ACTOR_COPY_OK;
  }
  case Double_tag:
    return node->wosize == Double_wosize
      ? CAML_ACTOR_COPY_OK : CAML_ACTOR_COPY_INVALID_SOURCE;
  case Double_array_tag:
    return node->wosize % Double_wosize == 0
      ? CAML_ACTOR_COPY_OK : CAML_ACTOR_COPY_INVALID_SOURCE;
  default:
    return CAML_ACTOR_COPY_UNSUPPORTED_TAG;
  }
}

static int discover_value(struct actor_copy_context *context,
                          value candidate)
{
  struct actor_copy_reference reference;
  struct actor_copy_node node;
  enum caml_actor_copy_status status;
  uintnat index;
  mlsize_t block_words;

  if (Is_long(candidate) || canonical_atom(candidate)) return 1;
  if (!canonicalize_source(context, candidate, &reference)) {
    context->status = CAML_ACTOR_COPY_INVALID_SOURCE;
    return 0;
  }
  if (map_lookup(context, reference.base, &index)) {
    if (!valid_infix_reference(
          &context->nodes[index], reference.infix_offset)) {
      context->status = CAML_ACTOR_COPY_INVALID_CLOSURE;
      return 0;
    }
    return 1;
  }

  memset(&node, 0, sizeof(node));
  node.source = reference.base;
  node.source_header = Hd_val(reference.base);
  node.wosize = Wosize_hd(node.source_header);
  node.tag = Tag_hd(node.source_header);
  node.origin = reference.origin;
  status = validate_node(&node);
  if (status != CAML_ACTOR_COPY_OK) {
    context->status = status;
    return 0;
  }
  if (!valid_infix_reference(&node, reference.infix_offset)) {
    context->status = CAML_ACTOR_COPY_INVALID_CLOSURE;
    return 0;
  }

  if (node.wosize > CAML_UINTNAT_MAX - 1) {
    context->status = CAML_ACTOR_COPY_INVALID_SOURCE;
    return 0;
  }
  block_words = Whsize_wosize(node.wosize);
  if (block_words > context->quota_words - context->copied_words) {
    context->status = CAML_ACTOR_COPY_GRAPH_TOO_LARGE;
    return 0;
  }
  if (!reserve_node(context) || !reserve_map(context)) return 0;
  index = context->node_count++;
  context->nodes[index] = node;
  context->copied_words += block_words;
  map_insert_unchecked(context, reference.base, index);
  return 1;
}

static int discover_graph(struct actor_copy_context *context, value source)
{
  struct actor_copy_reference root;
  uintnat root_index;

  if (!discover_value(context, source)
      || !canonicalize_source(context, source, &root)
      || !map_lookup(context, root.base, &root_index)
      || context->nodes[root_index].tag != Closure_tag
      || !valid_infix_reference(
           &context->nodes[root_index], root.infix_offset)) {
    if (context->status == CAML_ACTOR_COPY_OK) {
      context->status = CAML_ACTOR_COPY_INVALID_CLOSURE;
    }
    return 0;
  }

  for (uintnat index = 0; index < context->node_count; index++) {
    value node_source = context->nodes[index].source;
    mlsize_t node_wosize = context->nodes[index].wosize;
    tag_t node_tag = context->nodes[index].tag;
    mlsize_t first = node_tag == Closure_tag
      ? context->nodes[index].closure_env : 0;

    if (node_tag >= Forcing_tag && node_tag != Closure_tag) continue;
    for (mlsize_t field = first; field < node_wosize; field++) {
      if (!discover_value(context, Field(node_source, field))) return 0;
    }
  }
  return 1;
}

static int translate_value(struct actor_copy_context *context,
                           value source, value *target)
{
  struct actor_copy_reference reference;
  uintnat index;

  if (Is_long(source) || canonical_atom(source)) {
    *target = source;
    return 1;
  }
  if (!canonicalize_source(context, source, &reference)
      || !map_lookup(context, reference.base, &index)
      || context->nodes[index].target == 0
      || !valid_infix_reference(
           &context->nodes[index], reference.infix_offset)) {
    return 0;
  }
  *target = context->nodes[index].target + reference.infix_offset;
  return 1;
}

static int allocate_targets(struct actor_copy_context *context,
                            struct caml_actor_heap *target_heap)
{
  for (uintnat index = 0; index < context->node_count; index++) {
    struct actor_copy_node *node = &context->nodes[index];
    enum caml_actor_heap_alloc_error error;

    if (Hd_val(node->source) != node->source_header) return 0;
    node->target = caml_actor_heap_try_alloc(
      target_heap, node->wosize, node->tag, 0, &error);
    if (node->target == 0 || error != CAML_ACTOR_HEAP_ALLOC_OK) return 0;
  }
  return 1;
}

static int fill_target_node(struct actor_copy_context *context,
                            struct actor_copy_node *node)
{
  if (Hd_val(node->source) != node->source_header) return 0;
  if (node->tag < Forcing_tag) {
    for (mlsize_t field = 0; field < node->wosize; field++) {
      value target;

      if (!translate_value(context, Field(node->source, field), &target)) {
        return 0;
      }
      Field(node->target, field) = target;
    }
    return 1;
  }
  if (node->tag == Closure_tag) {
    Code_val(node->target) = Code_val(node->source);
    Closinfo_val(node->target) = Closinfo_val(node->source);
    for (mlsize_t header_field = 2;
         header_field < node->closure_env; header_field += 3) {
      Field(node->target, header_field) =
        Make_header(header_field + 1, Infix_tag, 0);
      Field(node->target, header_field + 1) =
        Field(node->source, header_field + 1);
      Field(node->target, header_field + 2) =
        Field(node->source, header_field + 2);
    }
    for (mlsize_t field = node->closure_env;
         field < node->wosize; field++) {
      value target;

      if (!translate_value(context, Field(node->source, field), &target)) {
        return 0;
      }
      Field(node->target, field) = target;
    }
    return 1;
  }

  memcpy(Op_val(node->target), Op_val(node->source),
         Bsize_wsize(node->wosize));
  return 1;
}

static int fill_targets(struct actor_copy_context *context)
{
  for (uintnat index = 0; index < context->node_count; index++) {
    if (!fill_target_node(context, &context->nodes[index])) return 0;
  }
  return 1;
}

static void restore_source_heap(struct caml_actor_heap *source_heap,
                                struct caml__roots_block *source_roots)
{
  caml_actor_heap_deactivate();
  if (source_heap != NULL && !caml_actor_heap_activate(source_heap)) {
    caml_fatal_error("actor copier could not restore the source heap");
  }
  Caml_state->local_roots = source_roots;
}

static void free_context(struct actor_copy_context *context)
{
  free(context->map);
  free(context->nodes);
}

struct caml_actor_copy_result caml_actor_copy_closure_sized(
  value source, uintnat owner, mlsize_t initial_words,
  mlsize_t maximum_words)
{
  struct actor_copy_context context;
  struct caml_actor_copy_result result;
  struct actor_copy_reference root;
  struct caml_actor_heap *target_heap = NULL;
  struct caml_actor_heap_verify_result verification;
  struct caml__roots_block *source_roots = NULL;
  uintnat root_index;
  mlsize_t target_initial_words;
  int target_active = 0;

  if (!actor_copy_runtime_supported()) {
    return copy_failure(CAML_ACTOR_COPY_UNSUPPORTED_RUNTIME);
  }
  if (Caml_state_opt == NULL || !caml_domain_alone()
      || !caml_actor_world_is_frozen() || Caml_state->gc_regs != NULL
      || initial_words == 0 || initial_words > maximum_words) {
    return copy_failure(CAML_ACTOR_COPY_INVALID_SOURCE);
  }

  memset(&context, 0, sizeof(context));
  context.source_heap = caml_actor_heap_current();
  context.quota_words = maximum_words;
  context.status = CAML_ACTOR_COPY_OK;
  if (!discover_graph(&context, source)) goto failed;
  if (!caml_actor_world_is_frozen()
      || caml_actor_heap_current() != context.source_heap) {
    context.status = CAML_ACTOR_COPY_INTERNAL;
    goto failed;
  }

  target_initial_words = initial_words > context.copied_words
    ? initial_words : context.copied_words;
  target_heap = caml_actor_heap_create_sized(
    owner, target_initial_words, maximum_words);
  if (target_heap == NULL) {
    context.status = CAML_ACTOR_COPY_RESOURCE_UNAVAILABLE;
    goto failed;
  }
  source_roots = Caml_state->local_roots;
  Caml_state->local_roots = NULL;
  if (context.source_heap != NULL) caml_actor_heap_deactivate();
  if (!caml_actor_heap_activate(target_heap)) {
    if (context.source_heap != NULL
        && !caml_actor_heap_activate(context.source_heap)) {
      caml_fatal_error("actor copier could not restore its source heap");
    }
    Caml_state->local_roots = source_roots;
    context.status = CAML_ACTOR_COPY_INTERNAL;
    goto failed;
  }
  target_active = 1;

  if (!allocate_targets(&context, target_heap)
      || !fill_targets(&context)) {
    context.status = CAML_ACTOR_COPY_INTERNAL;
    goto failed;
  }
  restore_source_heap(context.source_heap, source_roots);
  target_active = 0;

  verification = caml_actor_heap_verify(target_heap);
  if (verification.error != CAML_ACTOR_HEAP_VERIFY_OK
      || caml_actor_heap_used_words(target_heap) != context.copied_words
      || !canonicalize_source(&context, source, &root)
      || !map_lookup(&context, root.base, &root_index)) {
    context.status = CAML_ACTOR_COPY_INTERNAL;
    goto failed;
  }
  result.status = CAML_ACTOR_COPY_OK;
  result.heap = target_heap;
  result.closure = context.nodes[root_index].target + root.infix_offset;
  if (!caml_actor_heap_owns_value(result.heap, result.closure)) {
    context.status = CAML_ACTOR_COPY_INTERNAL;
    target_heap = result.heap;
    goto failed;
  }
  free_context(&context);
  return result;

failed:
  if (target_active) {
    restore_source_heap(context.source_heap, source_roots);
  }
  if (target_heap != NULL) caml_actor_heap_destroy(target_heap);
  result = copy_failure(context.status);
  free_context(&context);
  return result;
}

struct caml_actor_copy_result caml_actor_copy_closure(
  value source, uintnat owner, mlsize_t quota_words)
{
  return caml_actor_copy_closure_sized(
    source, owner, quota_words, quota_words);
}
