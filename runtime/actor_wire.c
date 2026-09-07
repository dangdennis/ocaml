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
#include "caml/actor_wire.h"
#include "caml/actor_world.h"
#include "caml/mlvalues.h"
#include "caml/shared_heap.h"

#define ACTOR_WIRE_MAGIC UINT64_C(0x6163746f72776972)

enum actor_wire_token_kind {
  ACTOR_WIRE_TOKEN_IMMEDIATE = 1,
  ACTOR_WIRE_TOKEN_ATOM,
  ACTOR_WIRE_TOKEN_NODE
};

struct actor_wire_token {
  uintnat kind;
  uintnat payload;
};

struct actor_wire_node {
  mlsize_t wosize;
  uintnat tag;
  uintnat payload_index;
  uintnat scanned;
};

struct caml_actor_envelope {
  uint64_t magic;
  mlsize_t graph_words;
  uintnat node_count;
  uintnat token_count;
  uintnat raw_bytes;
  struct actor_wire_token root;
  struct actor_wire_node *nodes;
  struct actor_wire_token *tokens;
  unsigned char *raw;
};

struct actor_wire_source_node {
  value source;
  header_t header;
  const value *payload;
  mlsize_t wosize;
  tag_t tag;
  int frozen;
};

struct actor_wire_map_entry {
  value source;
  uintnat index_plus_one;
};

struct actor_wire_context {
  struct caml_actor_heap *source_heap;
  struct actor_wire_source_node *nodes;
  uintnat node_count;
  uintnat node_capacity;
  struct actor_wire_map_entry *map;
  uintnat map_capacity;
  uintnat map_used;
  mlsize_t graph_words;
  mlsize_t quota_words;
  uintnat token_count;
  uintnat raw_bytes;
  enum caml_actor_wire_encode_status status;
};

static int canonical_atom(value candidate, uintnat *tag_out)
{
  for (uintnat tag = 0; tag < Num_tags; tag++) {
    if (candidate == Atom((tag_t)tag)) {
      if (tag_out != NULL) *tag_out = tag;
      return 1;
    }
  }
  return 0;
}

static uintnat wire_hash(value source)
{
  uintnat hash = ((uintnat)source) >> 3;

  hash ^= hash >> 11;
  hash *= (uintnat)2654435761U;
  hash ^= hash >> 13;
  return hash;
}

static int map_lookup(const struct actor_wire_context *context,
                      value source, uintnat *index)
{
  uintnat mask;
  uintnat slot;

  if (context->map_capacity == 0) return 0;
  mask = context->map_capacity - 1;
  slot = wire_hash(source) & mask;
  while (context->map[slot].index_plus_one != 0) {
    if (context->map[slot].source == source) {
      *index = context->map[slot].index_plus_one - 1;
      return 1;
    }
    slot = (slot + 1) & mask;
  }
  return 0;
}

static void map_insert_unchecked(struct actor_wire_context *context,
                                 value source, uintnat index)
{
  uintnat mask = context->map_capacity - 1;
  uintnat slot = wire_hash(source) & mask;

  while (context->map[slot].index_plus_one != 0) {
    slot = (slot + 1) & mask;
  }
  context->map[slot].source = source;
  context->map[slot].index_plus_one = index + 1;
  context->map_used++;
}

static int reserve_map(struct actor_wire_context *context)
{
  struct actor_wire_map_entry *old_map = context->map;
  uintnat old_capacity = context->map_capacity;
  uintnat new_capacity;

  if (old_capacity != 0 && context->map_used + 1 <= old_capacity / 2) {
    return 1;
  }
  new_capacity = old_capacity == 0 ? 32 : 2 * old_capacity;
  if (new_capacity < old_capacity
      || new_capacity > SIZE_MAX / sizeof(*context->map)) {
    context->status = CAML_ACTOR_WIRE_ENCODE_RESOURCE_UNAVAILABLE;
    return 0;
  }
  context->map = calloc(new_capacity, sizeof(*context->map));
  if (context->map == NULL) {
    context->map = old_map;
    context->status = CAML_ACTOR_WIRE_ENCODE_RESOURCE_UNAVAILABLE;
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

static int reserve_node(struct actor_wire_context *context)
{
  struct actor_wire_source_node *nodes;
  uintnat capacity;

  if (context->node_count < context->node_capacity) return 1;
  capacity = context->node_capacity == 0
    ? 32 : 2 * context->node_capacity;
  if (capacity < context->node_capacity
      || capacity > SIZE_MAX / sizeof(*nodes)) {
    context->status = CAML_ACTOR_WIRE_ENCODE_RESOURCE_UNAVAILABLE;
    return 0;
  }
  nodes = realloc(context->nodes, capacity * sizeof(*nodes));
  if (nodes == NULL) {
    context->status = CAML_ACTOR_WIRE_ENCODE_RESOURCE_UNAVAILABLE;
    return 0;
  }
  context->nodes = nodes;
  context->node_capacity = capacity;
  return 1;
}

static enum caml_actor_wire_encode_status validate_source_node(
  const struct actor_wire_source_node *node)
{
  if (node->wosize == 0 || Reserved_hd(node->header) != 0
      || (!node->frozen && Color_hd(node->header) != NOT_MARKABLE)) {
    return CAML_ACTOR_WIRE_ENCODE_INVALID_SOURCE;
  }
  if (node->tag < Forcing_tag) return CAML_ACTOR_WIRE_ENCODE_OK;
  switch (node->tag) {
  case String_tag: {
    mlsize_t bytes = Bsize_wsize(node->wosize);
    const unsigned char *payload = (const unsigned char *)node->payload;
    unsigned padding = payload[bytes - 1];

    if (padding >= sizeof(value)
        || payload[bytes - 1 - padding] != 0) {
      return CAML_ACTOR_WIRE_ENCODE_INVALID_SOURCE;
    }
    return CAML_ACTOR_WIRE_ENCODE_OK;
  }
  case Double_tag:
    return node->wosize == Double_wosize
      ? CAML_ACTOR_WIRE_ENCODE_OK
      : CAML_ACTOR_WIRE_ENCODE_INVALID_SOURCE;
  case Double_array_tag:
    return node->wosize % Double_wosize == 0
      ? CAML_ACTOR_WIRE_ENCODE_OK
      : CAML_ACTOR_WIRE_ENCODE_INVALID_SOURCE;
  default:
    return CAML_ACTOR_WIRE_ENCODE_UNSUPPORTED_VALUE;
  }
}

static int add_uintnat(uintnat left, uintnat right, uintnat *sum)
{
  if (right > CAML_UINTNAT_MAX - left) return 0;
  *sum = left + right;
  return 1;
}

static int discover_value(struct actor_wire_context *context,
                          value candidate)
{
  struct actor_wire_source_node node;
  enum caml_actor_wire_encode_status status;
  uintnat ignored;
  uintnat index;
  mlsize_t block_words;
  const value *frozen_payload;

  if (Is_long(candidate) || canonical_atom(candidate, &ignored)) return 1;
  if (map_lookup(context, candidate, &index)) return 1;

  node.source = candidate;
  node.frozen = 0;
  if (caml_actor_heap_owns_value(context->source_heap, candidate)) {
    node.header = Hd_val(candidate);
    node.payload = Op_val(candidate);
  } else if (caml_actor_world_frozen_snapshot(
               candidate, &node.header, &frozen_payload)) {
    node.payload = frozen_payload;
    node.frozen = 1;
  } else {
    context->status = CAML_ACTOR_WIRE_ENCODE_INVALID_SOURCE;
    return 0;
  }
  node.wosize = Wosize_hd(node.header);
  node.tag = Tag_hd(node.header);
  status = validate_source_node(&node);
  if (status != CAML_ACTOR_WIRE_ENCODE_OK) {
    context->status = status;
    return 0;
  }

  block_words = Whsize_wosize(node.wosize);
  if (context->graph_words > context->quota_words
      || block_words > context->quota_words - context->graph_words) {
    context->status = CAML_ACTOR_WIRE_ENCODE_TOO_LARGE;
    return 0;
  }
  if (node.tag < Forcing_tag) {
    if (!add_uintnat(
          context->token_count, node.wosize,
          &context->token_count)) {
      context->status = CAML_ACTOR_WIRE_ENCODE_TOO_LARGE;
      return 0;
    }
  } else if (!add_uintnat(
               context->raw_bytes, Bsize_wsize(node.wosize),
               &context->raw_bytes)) {
    context->status = CAML_ACTOR_WIRE_ENCODE_TOO_LARGE;
    return 0;
  }
  if (!reserve_node(context) || !reserve_map(context)) return 0;
  index = context->node_count++;
  context->nodes[index] = node;
  context->graph_words += block_words;
  map_insert_unchecked(context, candidate, index);
  return 1;
}

static int discover_graph(struct actor_wire_context *context, value root)
{
  if (!discover_value(context, root)) return 0;
  for (uintnat index = 0; index < context->node_count; index++) {
    const value *payload = context->nodes[index].payload;
    mlsize_t wosize = context->nodes[index].wosize;
    tag_t tag = context->nodes[index].tag;

    /* [discover_value] may grow [context->nodes], invalidating pointers into
       it.  Snapshot this node's traversal metadata before discovering any
       child. */
    if (tag >= Forcing_tag) continue;
    for (mlsize_t field = 0; field < wosize; field++) {
      if (!discover_value(context, payload[field])) {
        return 0;
      }
    }
  }
  return 1;
}

static int encode_token(const struct actor_wire_context *context,
                        value source, struct actor_wire_token *token)
{
  uintnat payload;

  if (Is_long(source)) {
    token->kind = ACTOR_WIRE_TOKEN_IMMEDIATE;
    token->payload = (uintnat)Long_val(source);
    return 1;
  }
  if (canonical_atom(source, &payload)) {
    token->kind = ACTOR_WIRE_TOKEN_ATOM;
    token->payload = payload;
    return 1;
  }
  if (!map_lookup(context, source, &payload)) return 0;
  token->kind = ACTOR_WIRE_TOKEN_NODE;
  token->payload = payload;
  return 1;
}

static struct caml_actor_envelope *build_envelope(
  struct actor_wire_context *context, value root)
{
  struct caml_actor_envelope *envelope;
  uintnat token_index = 0;
  uintnat raw_index = 0;

  if (context->node_count > SIZE_MAX / sizeof(*envelope->nodes)
      || context->token_count > SIZE_MAX / sizeof(*envelope->tokens)) {
    context->status = CAML_ACTOR_WIRE_ENCODE_RESOURCE_UNAVAILABLE;
    return NULL;
  }
  envelope = calloc(1, sizeof(*envelope));
  if (envelope == NULL) goto unavailable;
  if (context->node_count != 0) {
    envelope->nodes = calloc(
      context->node_count, sizeof(*envelope->nodes));
    if (envelope->nodes == NULL) goto unavailable;
  }
  if (context->token_count != 0) {
    envelope->tokens = calloc(
      context->token_count, sizeof(*envelope->tokens));
    if (envelope->tokens == NULL) goto unavailable;
  }
  if (context->raw_bytes != 0) {
    envelope->raw = malloc(context->raw_bytes);
    if (envelope->raw == NULL) goto unavailable;
  }

  envelope->magic = ACTOR_WIRE_MAGIC;
  envelope->graph_words = context->graph_words;
  envelope->node_count = context->node_count;
  envelope->token_count = context->token_count;
  envelope->raw_bytes = context->raw_bytes;
  if (!encode_token(context, root, &envelope->root)) goto internal;

  for (uintnat index = 0; index < context->node_count; index++) {
    struct actor_wire_source_node *source = &context->nodes[index];
    struct actor_wire_node *target = &envelope->nodes[index];
    header_t frozen_header;
    const value *frozen_payload;

    if (source->frozen) {
      if (!caml_actor_world_frozen_snapshot(
            source->source, &frozen_header, &frozen_payload)
          || frozen_header != source->header
          || frozen_payload != source->payload) {
        goto internal;
      }
    } else if (Hd_val(source->source) != source->header) {
      goto internal;
    }
    target->wosize = source->wosize;
    target->tag = source->tag;
    if (source->tag < Forcing_tag) {
      target->scanned = 1;
      target->payload_index = token_index;
      for (mlsize_t field = 0; field < source->wosize; field++) {
        if (!encode_token(
              context, source->payload[field],
              &envelope->tokens[token_index++])) {
          goto internal;
        }
      }
    } else {
      uintnat bytes = Bsize_wsize(source->wosize);

      target->scanned = 0;
      target->payload_index = raw_index;
      memcpy(envelope->raw + raw_index, source->payload, bytes);
      raw_index += bytes;
    }
  }
  if (token_index != envelope->token_count
      || raw_index != envelope->raw_bytes
      || !caml_actor_wire_verify(envelope)) {
    goto internal;
  }
  return envelope;

unavailable:
  context->status = CAML_ACTOR_WIRE_ENCODE_RESOURCE_UNAVAILABLE;
  caml_actor_wire_destroy(envelope);
  return NULL;
internal:
  context->status = CAML_ACTOR_WIRE_ENCODE_INTERNAL;
  caml_actor_wire_destroy(envelope);
  return NULL;
}

static void free_context(struct actor_wire_context *context)
{
  free(context->map);
  free(context->nodes);
}

struct caml_actor_wire_encode_result caml_actor_wire_encode(
  value message, mlsize_t quota_words)
{
  struct caml_actor_wire_encode_result result;
  struct actor_wire_context context;
  struct caml_actor_heap_verify_result verification;

  result.status = CAML_ACTOR_WIRE_ENCODE_INVALID_SOURCE;
  result.envelope = NULL;
  memset(&context, 0, sizeof(context));
  context.source_heap = caml_actor_heap_current();
  context.quota_words = quota_words;
  context.status = CAML_ACTOR_WIRE_ENCODE_OK;
  if (context.source_heap == NULL || quota_words == 0
      || !caml_actor_world_is_frozen()) {
    return result;
  }
  verification = caml_actor_heap_verify(context.source_heap);
  if (verification.error != CAML_ACTOR_HEAP_VERIFY_OK
      || caml_actor_heap_shared_bypasses(context.source_heap) != 0) {
    return result;
  }
  if (!discover_graph(&context, message)) goto finished;
  result.envelope = build_envelope(&context, message);

finished:
  result.status = context.status;
  free_context(&context);
  return result;
}

static int token_valid(const struct caml_actor_envelope *envelope,
                       const struct actor_wire_token *token)
{
  switch (token->kind) {
  case ACTOR_WIRE_TOKEN_IMMEDIATE:
    return (intnat)token->payload <= Max_long
      && (intnat)token->payload >= Min_long;
  case ACTOR_WIRE_TOKEN_ATOM:
    return token->payload < Num_tags;
  case ACTOR_WIRE_TOKEN_NODE:
    return token->payload < envelope->node_count;
  default:
    return 0;
  }
}

int caml_actor_wire_verify(const struct caml_actor_envelope *envelope)
{
  mlsize_t graph_words = 0;
  uintnat expected_tokens = 0;
  uintnat expected_raw = 0;

  if (envelope == NULL || envelope->magic != ACTOR_WIRE_MAGIC
      || (envelope->node_count != 0 && envelope->nodes == NULL)
      || (envelope->token_count != 0 && envelope->tokens == NULL)
      || (envelope->raw_bytes != 0 && envelope->raw == NULL)
      || !token_valid(envelope, &envelope->root)) {
    return 0;
  }
  for (uintnat index = 0; index < envelope->node_count; index++) {
    const struct actor_wire_node *node = &envelope->nodes[index];
    mlsize_t block_words;

    if (node->wosize == 0 || node->tag >= Num_tags) return 0;
    block_words = Whsize_wosize(node->wosize);
    if (block_words > CAML_UINTNAT_MAX - graph_words) return 0;
    graph_words += block_words;
    if (node->scanned) {
      if (node->tag >= Forcing_tag
          || node->payload_index != expected_tokens
          || expected_tokens > envelope->token_count
          || node->wosize > envelope->token_count - expected_tokens) {
        return 0;
      }
      for (mlsize_t field = 0; field < node->wosize; field++) {
        if (!token_valid(
              envelope,
              &envelope->tokens[expected_tokens + field])) {
          return 0;
        }
      }
      expected_tokens += node->wosize;
    } else {
      uintnat bytes = Bsize_wsize(node->wosize);

      if (node->tag != String_tag && node->tag != Double_tag
          && node->tag != Double_array_tag) {
        return 0;
      }
      if ((node->tag == Double_tag && node->wosize != Double_wosize)
          || (node->tag == Double_array_tag
              && node->wosize % Double_wosize != 0)
          || node->payload_index != expected_raw
          || expected_raw > envelope->raw_bytes
          || bytes > envelope->raw_bytes - expected_raw) {
        return 0;
      }
      if (node->tag == String_tag) {
        unsigned padding = envelope->raw[expected_raw + bytes - 1];

        if (padding >= sizeof(value)
            || envelope->raw[expected_raw + bytes - 1 - padding] != 0) {
          return 0;
        }
      }
      expected_raw += bytes;
    }
  }
  return graph_words == envelope->graph_words
    && expected_tokens == envelope->token_count
    && expected_raw == envelope->raw_bytes;
}

int caml_actor_wire_encoded_bytes(
  const struct caml_actor_envelope *envelope, uintnat *encoded_bytes)
{
  uintnat words;

  if (encoded_bytes != NULL) *encoded_bytes = 0;
  if (encoded_bytes == NULL || !caml_actor_wire_verify(envelope)
      || envelope->graph_words == CAML_UINTNAT_MAX) {
    return 0;
  }
  words = envelope->graph_words + 1; /* Canonical root token. */
  if (words > CAML_UINTNAT_MAX / sizeof(value)) return 0;
  *encoded_bytes = words * sizeof(value);
  return 1;
}

static int decode_token(const struct caml_actor_envelope *envelope,
                        const value *targets,
                        const struct actor_wire_token *token,
                        value *decoded)
{
  if (!token_valid(envelope, token)) return 0;
  switch (token->kind) {
  case ACTOR_WIRE_TOKEN_IMMEDIATE:
    *decoded = Val_long((intnat)token->payload);
    return 1;
  case ACTOR_WIRE_TOKEN_ATOM:
    *decoded = Atom((tag_t)token->payload);
    return 1;
  case ACTOR_WIRE_TOKEN_NODE:
    if (targets == NULL || targets[token->payload] == 0) return 0;
    *decoded = targets[token->payload];
    return 1;
  default:
    return 0;
  }
}

enum caml_actor_wire_decode_status caml_actor_wire_decode(
  const struct caml_actor_envelope *envelope,
  struct caml_actor_heap *target_heap, value *message)
{
  struct caml_actor_heap_verify_result verification;
  value *targets = NULL;
  enum caml_actor_wire_decode_status status =
    CAML_ACTOR_WIRE_DECODE_INTERNAL;

  if (message != NULL) *message = Val_unit;
  if (target_heap == NULL || message == NULL
      || caml_actor_heap_current() != target_heap
      || !caml_actor_world_is_frozen()
      || !caml_actor_wire_verify(envelope)) {
    return status;
  }
  if (!caml_actor_heap_reserve(target_heap, envelope->graph_words)) {
    return CAML_ACTOR_WIRE_DECODE_HEAP_EXHAUSTED;
  }
  if (envelope->node_count > SIZE_MAX / sizeof(*targets)) return status;
  if (envelope->node_count != 0) {
    targets = calloc(envelope->node_count, sizeof(*targets));
    if (targets == NULL) {
      return CAML_ACTOR_WIRE_DECODE_RESOURCE_UNAVAILABLE;
    }
  }

  for (uintnat index = 0; index < envelope->node_count; index++) {
    const struct actor_wire_node *node = &envelope->nodes[index];
    enum caml_actor_heap_alloc_error error;

    targets[index] = caml_actor_heap_try_alloc(
      target_heap, node->wosize, (tag_t)node->tag, 0, &error);
    if (targets[index] == 0 || error != CAML_ACTOR_HEAP_ALLOC_OK) {
      status = error == CAML_ACTOR_HEAP_ALLOC_QUOTA
        ? CAML_ACTOR_WIRE_DECODE_HEAP_EXHAUSTED
        : CAML_ACTOR_WIRE_DECODE_INTERNAL;
      goto finished;
    }
  }

  for (uintnat index = 0; index < envelope->node_count; index++) {
    const struct actor_wire_node *node = &envelope->nodes[index];

    if (node->scanned) {
      for (mlsize_t field = 0; field < node->wosize; field++) {
        value decoded;

        if (!decode_token(
              envelope, targets,
              &envelope->tokens[node->payload_index + field],
              &decoded)) {
          goto finished;
        }
        Field(targets[index], field) = decoded;
      }
    } else {
      memcpy(Op_val(targets[index]),
             envelope->raw + node->payload_index,
             Bsize_wsize(node->wosize));
    }
  }
  if (!decode_token(envelope, targets, &envelope->root, message)) {
    goto finished;
  }
  verification = caml_actor_heap_verify(target_heap);
  if (verification.error != CAML_ACTOR_HEAP_VERIFY_OK
      || (Is_block(*message)
          && !canonical_atom(*message, NULL)
          && !caml_actor_heap_owns_value(target_heap, *message))) {
    goto finished;
  }
  status = CAML_ACTOR_WIRE_DECODE_OK;

finished:
  free(targets);
  return status;
}

void caml_actor_wire_destroy(struct caml_actor_envelope *envelope)
{
  if (envelope == NULL) return;
  envelope->magic = 0;
  free(envelope->raw);
  free(envelope->tokens);
  free(envelope->nodes);
  free(envelope);
}
