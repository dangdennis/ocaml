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
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "caml/actor_io.h"
#include "caml/misc.h"

struct io_resource {
  uintnat id, owner, operation;
  int fd, port;
  enum caml_actor_io_kind kind;
};

struct io_operation {
  uintnat id, owner, resource, accepted, size, result;
  enum caml_actor_io_operation kind;
  enum caml_actor_io_status status;
  int error, watched;
  char *bytes;
};

struct caml_actor_io {
  struct io_resource *resources;
  struct io_operation *operations;
  struct caml_actor_io_stats stats;
  struct caml_actor_io_backend backend;
  int failed, fail_allocation, exhausted;
};

/* Single-Domain worlds share a checked identity source. Never recycle an
   authority or operation identity, including across successive worlds. */
static uintnat next_identity = 1;

static void increment(uintnat *n)
{
  if (*n < (uintnat)Max_long) ++*n;
}

static uintnat identity(struct caml_actor_io *io)
{
  if (io->exhausted || next_identity > (uintnat)Max_long) return 0;
  return next_identity++;
}

#if defined(__linux__) && !defined(NATIVE_CODE)
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

struct real_io { int reactor; };

static int real_listen(void *context, const char *address, int port, int *bound)
{
  struct sockaddr_in addr;
  socklen_t length = sizeof(addr);
  int fd, error;
  (void)context;
  memset(&addr, 0, sizeof(addr));
  addr.sin_family = AF_INET;
  addr.sin_port = htons((unsigned short)port);
  if (inet_pton(AF_INET, address, &addr.sin_addr) != 1) return -EINVAL;
  fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
  if (fd < 0) return -errno;
  if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0
      || listen(fd, 128) != 0
      || getsockname(fd, (struct sockaddr *)&addr, &length) != 0) {
    error = errno;
    close(fd);
    return -error;
  }
  *bound = ntohs(addr.sin_port);
  return fd;
}

static int real_accept(void *context, int fd)
{
  int accepted;
  (void)context;
  accepted = accept4(fd, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
  return accepted < 0 ? -errno : accepted;
}

static intnat real_read(void *context, int fd, char *bytes, uintnat size)
{
  ssize_t count;
  (void)context;
  count = recv(fd, bytes, size, 0);
  return count < 0 ? -errno : count;
}

static intnat real_write(void *context, int fd, const char *bytes, uintnat size)
{
  ssize_t count;
  (void)context;
  count = send(fd, bytes, size, MSG_NOSIGNAL);
  return count < 0 ? -errno : count;
}

static void real_close(void *context, int fd)
{
  (void)context;
  /* Linux releases the descriptor even when close reports an error. */
  close(fd);
}

static int real_watch(void *context, int fd, uintnat id, int writing)
{
  struct real_io *real = context;
  struct epoll_event event;
  if (real->reactor < 0) {
    real->reactor = epoll_create1(EPOLL_CLOEXEC);
    if (real->reactor < 0) return -errno;
  }
  memset(&event, 0, sizeof(event));
  event.events = writing ? EPOLLOUT : EPOLLIN;
  event.data.u64 = id;
  return epoll_ctl(real->reactor, EPOLL_CTL_ADD, fd, &event) < 0 ? -errno : 0;
}

static int real_unwatch(void *context, int fd)
{
  struct real_io *real = context;
  return epoll_ctl(real->reactor, EPOLL_CTL_DEL, fd, NULL) < 0 ? -errno : 0;
}

static int real_poll(void *context, uintnat *ids, int capacity)
{
  struct real_io *real = context;
  struct epoll_event events[CAML_ACTOR_IO_BATCH];
  int count;
  if (real->reactor < 0) return 0;
  if (capacity > CAML_ACTOR_IO_BATCH) return -EINVAL;
  count = epoll_wait(real->reactor, events, capacity, 0);
  if (count < 0) return errno == EINTR ? 0 : -errno;
  for (int i = 0; i < count; i++) ids[i] = events[i].data.u64;
  return count;
}

static int real_wait(void *context, struct caml_actor_timers *timers,
                     uint64_t deadline)
{
  struct real_io *real = context;
  return caml_actor_timers_wait_fd(timers, deadline, real->reactor);
}

static void real_destroy(void *context)
{
  struct real_io *real = context;
  if (real->reactor >= 0) close(real->reactor);
  free(real);
}
#endif

int caml_actor_io_default_backend(struct caml_actor_io_backend *backend)
{
  memset(backend, 0, sizeof(*backend));
#if defined(__linux__) && !defined(NATIVE_CODE)
  struct real_io *real = malloc(sizeof(*real));
  if (real == NULL) return 0;
  real->reactor = -1;
  *backend = (struct caml_actor_io_backend) {
    real, real_listen, real_accept, real_read, real_write, real_close,
    real_watch, real_unwatch, real_poll, real_wait, real_destroy
  };
#endif
  return 1;
}

struct caml_actor_io *caml_actor_io_create(
  uintnat resources, uintnat operations, uintnat bytes,
  const struct caml_actor_io_backend *backend)
{
  struct caml_actor_io *io;
  if (resources == 0 || resources > (uintnat)Max_long
      || resources > SIZE_MAX / sizeof(struct io_resource)
      || operations == 0 || operations > (uintnat)Max_long
      || operations > SIZE_MAX / sizeof(struct io_operation)
      || bytes == 0 || bytes > (uintnat)Max_long) return NULL;
  io = calloc(1, sizeof(*io));
  if (io == NULL) return NULL;
  io->stats.resource_limit = resources;
  io->stats.operation_limit = operations;
  io->stats.byte_limit = bytes;
  if (backend != NULL) io->backend = *backend;
  else if (!caml_actor_io_default_backend(&io->backend)) {
    free(io);
    return NULL;
  }
  return io;
}

static void *allocate(struct caml_actor_io *io, size_t count, size_t size)
{
  if (io->fail_allocation) {
    io->fail_allocation = 0;
    return NULL;
  }
  return calloc(count, size);
}

static struct io_resource *resource(struct caml_actor_io *io, uintnat id)
{
  if (io->resources == NULL || id == 0) return NULL;
  for (uintnat i = 0; i < io->stats.resource_limit; i++)
    if (io->resources[i].id == id) return &io->resources[i];
  return NULL;
}

static struct io_operation *operation(struct caml_actor_io *io, uintnat id)
{
  if (io->operations == NULL || id == 0) return NULL;
  for (uintnat i = 0; i < io->stats.operation_limit; i++)
    if (io->operations[i].id == id) return &io->operations[i];
  return NULL;
}

static enum caml_actor_io_status quota(struct caml_actor_io *io,
                                       enum caml_actor_io_status status)
{
  increment(&io->stats.quota_failures);
  return status;
}

static enum caml_actor_io_status reserve_resource(struct caml_actor_io *io,
  uintnat owner, enum caml_actor_io_kind kind, struct io_resource **out)
{
  struct io_resource *r = NULL;
  uintnat id;
  if (io->backend.listen == NULL) return CAML_ACTOR_IO_UNSUPPORTED;
  if (io->stats.resources == io->stats.resource_limit)
    return quota(io, CAML_ACTOR_IO_RESOURCE_LIMIT);
  if (io->resources == NULL) {
    io->resources = allocate(io, io->stats.resource_limit, sizeof(*r));
    if (io->resources == NULL) return CAML_ACTOR_IO_UNAVAILABLE;
  }
  id = identity(io);
  if (id == 0) return CAML_ACTOR_IO_UNAVAILABLE;
  for (uintnat i = 0; i < io->stats.resource_limit; i++) {
    if (io->resources[i].id == 0) { r = &io->resources[i]; break; }
  }
  CAMLassert(r != NULL);
  *r = (struct io_resource) { id, owner, 0, -1, 0, kind };
  ++io->stats.resources;
  if (io->stats.resources > io->stats.peak_resources)
    io->stats.peak_resources = io->stats.resources;
  *out = r;
  return CAML_ACTOR_IO_OK;
}

static void release_resource(struct caml_actor_io *io, struct io_resource *r)
{
  CAMLassert(r != NULL && r->id != 0 && r->operation == 0);
  if (r->fd >= 0) {
    io->backend.close(io->backend.context, r->fd);
    increment(&io->stats.closed);
  }
  memset(r, 0, sizeof(*r));
  --io->stats.resources;
}

enum caml_actor_io_status caml_actor_io_listen(struct caml_actor_io *io,
  uintnat owner, const char *address, int port, uintnat *id, int *error)
{
  struct io_resource *r;
  enum caml_actor_io_status status;
  if (port < 0 || port > 65535) return CAML_ACTOR_IO_ARGUMENT;
  status = reserve_resource(io, owner, CAML_ACTOR_IO_LISTENER, &r);
  if (status != CAML_ACTOR_IO_OK) return status;
  r->fd = io->backend.listen(io->backend.context, address, port, &r->port);
  if (r->fd < 0) {
    *error = -r->fd;
    release_resource(io, r);
    increment(&io->stats.errors);
    return *error == EINVAL ? CAML_ACTOR_IO_ARGUMENT : CAML_ACTOR_IO_SYSTEM;
  }
  *id = r->id;
  return CAML_ACTOR_IO_OK;
}

enum caml_actor_io_status caml_actor_io_port(struct caml_actor_io *io,
  uintnat owner, uintnat id, int *port)
{
  struct io_resource *r = resource(io, id);
  if (r == NULL || r->owner != owner || r->kind != CAML_ACTOR_IO_LISTENER)
    return CAML_ACTOR_IO_HANDLE;
  *port = r->port;
  return CAML_ACTOR_IO_OK;
}

enum caml_actor_io_status caml_actor_io_close(struct caml_actor_io *io,
  uintnat owner, uintnat id, enum caml_actor_io_kind kind)
{
  struct io_resource *r = resource(io, id);
  if (r == NULL || r->owner != owner || r->kind != kind || r->fd < 0)
    return CAML_ACTOR_IO_HANDLE;
  if (r->operation != 0) return CAML_ACTOR_IO_BUSY;
  release_resource(io, r);
  return CAML_ACTOR_IO_OK;
}

uintnat caml_actor_io_owner_operation(struct caml_actor_io *io, uintnat owner)
{
  if (io->operations == NULL) return 0;
  for (uintnat i = 0; i < io->stats.operation_limit; i++)
    if (io->operations[i].id != 0 && io->operations[i].owner == owner)
      return io->operations[i].id;
  return 0;
}

static void unwatch(struct caml_actor_io *io, struct io_operation *op)
{
  struct io_resource *r = resource(io, op->resource);
  if (op->watched) {
    if (io->backend.unwatch(io->backend.context, r->fd) < 0) io->failed = 1;
    op->watched = 0;
  }
}

static void progress(struct caml_actor_io *io, struct io_operation *op)
{
  struct io_resource *r = resource(io, op->resource);
  struct io_resource *accepted = resource(io, op->accepted);
  intnat count;
  CAMLassert(r != NULL && op->status == CAML_ACTOR_IO_PENDING);
  if (op->kind == CAML_ACTOR_IO_ACCEPT)
    count = io->backend.accept(io->backend.context, r->fd);
  else if (op->size == 0) count = 0;
  else if (op->kind == CAML_ACTOR_IO_READ)
    count = io->backend.read(io->backend.context, r->fd, op->bytes, op->size);
  else
    count = io->backend.write(io->backend.context, r->fd, op->bytes, op->size);
  /* A single attempt per dispatch bounds EINTR and readiness races. */
  if (count == -EAGAIN || count == -EWOULDBLOCK || count == -EINTR
      || (count == 0 && op->kind == CAML_ACTOR_IO_WRITE && op->size > 0))
    return;
  if (op->kind != CAML_ACTOR_IO_ACCEPT && count > (intnat)op->size) {
    io->failed = 1;
    return;
  }
  if (count < 0) {
    op->status = CAML_ACTOR_IO_SYSTEM;
    op->error = -count;
    increment(&io->stats.errors);
  } else {
    op->status = CAML_ACTOR_IO_OK;
    if (op->kind == CAML_ACTOR_IO_ACCEPT) {
      CAMLassert(accepted != NULL);
      accepted->fd = count;
      op->result = accepted->id;
    } else op->result = count;
  }
  unwatch(io, op);
}

enum caml_actor_io_status caml_actor_io_submit(struct caml_actor_io *io,
  uintnat owner, uintnat id, enum caml_actor_io_operation kind,
  const char *bytes, uintnat size, uintnat *operation_id, int *error)
{
  struct io_resource *r = resource(io, id), *accepted = NULL;
  struct io_operation *op = NULL;
  enum caml_actor_io_status status;
  uintnat op_id;
  char *buffer = NULL;
  if (r == NULL || r->owner != owner || r->fd < 0
      || r->kind != (kind == CAML_ACTOR_IO_ACCEPT
                      ? CAML_ACTOR_IO_LISTENER : CAML_ACTOR_IO_SOCKET))
    return CAML_ACTOR_IO_HANDLE;
  if (r->operation != 0 || caml_actor_io_owner_operation(io, owner) != 0)
    return CAML_ACTOR_IO_BUSY;
  if (kind != CAML_ACTOR_IO_ACCEPT && size == 0) {
    *operation_id = 0;
    return CAML_ACTOR_IO_OK;
  }
  if (size > CAML_ACTOR_IO_CHUNK) size = CAML_ACTOR_IO_CHUNK;
  if (io->stats.operations == io->stats.operation_limit)
    return quota(io, CAML_ACTOR_IO_OPERATION_LIMIT);
  if (size > io->stats.byte_limit - io->stats.bytes)
    return quota(io, CAML_ACTOR_IO_BUFFER_LIMIT);
  if (io->operations == NULL) {
    io->operations = allocate(io, io->stats.operation_limit, sizeof(*op));
    if (io->operations == NULL) return CAML_ACTOR_IO_UNAVAILABLE;
  }
  op_id = identity(io);
  if (op_id == 0) return CAML_ACTOR_IO_UNAVAILABLE;
  if (size != 0) {
    buffer = allocate(io, size, 1);
    if (buffer == NULL) return CAML_ACTOR_IO_UNAVAILABLE;
    if (kind == CAML_ACTOR_IO_WRITE) memcpy(buffer, bytes, size);
  }
  if (kind == CAML_ACTOR_IO_ACCEPT) {
    status = reserve_resource(io, owner, CAML_ACTOR_IO_SOCKET, &accepted);
    if (status != CAML_ACTOR_IO_OK) { free(buffer); return status; }
  }
  for (uintnat i = 0; i < io->stats.operation_limit; i++) {
    if (io->operations[i].id == 0) { op = &io->operations[i]; break; }
  }
  CAMLassert(op != NULL);
  *op = (struct io_operation) {
    op_id, owner, id, accepted == NULL ? 0 : accepted->id, size, 0,
    kind, CAML_ACTOR_IO_PENDING, 0, 0, buffer
  };
  r->operation = op_id;
  ++io->stats.operations;
  io->stats.bytes += size;
  if (io->stats.operations > io->stats.peak_operations)
    io->stats.peak_operations = io->stats.operations;
  if (io->stats.bytes > io->stats.peak_bytes)
    io->stats.peak_bytes = io->stats.bytes;
  progress(io, op);
  if (op->status == CAML_ACTOR_IO_PENDING) {
    int status = io->backend.watch(io->backend.context, r->fd, op_id,
                                  kind == CAML_ACTOR_IO_WRITE);
    if (status < 0) {
      op->status = CAML_ACTOR_IO_SYSTEM;
      op->error = -status;
      increment(&io->stats.errors);
    } else op->watched = 1;
  }
  *operation_id = op_id;
  *error = op->error;
  return op->status;
}

enum caml_actor_io_status caml_actor_io_result(struct caml_actor_io *io,
  uintnat owner, uintnat resource_id, enum caml_actor_io_operation kind,
  uintnat id, uintnat *result, const char **bytes, int *error)
{
  struct io_operation *op = operation(io, id);
  if (op == NULL || op->owner != owner || op->resource != resource_id
      || op->kind != kind) return CAML_ACTOR_IO_HANDLE;
  *result = op->result;
  *bytes = op->bytes;
  *error = op->error;
  return op->status;
}

static void release_operation(struct caml_actor_io *io,
                               struct io_operation *op, int consumed)
{
  struct io_resource *r = resource(io, op->resource);
  struct io_resource *accepted = resource(io, op->accepted);
  unwatch(io, op);
  if (accepted != NULL && (!consumed || op->status != CAML_ACTOR_IO_OK))
    release_resource(io, accepted);
  CAMLassert(r != NULL && r->operation == op->id);
  r->operation = 0;
  --io->stats.operations;
  io->stats.bytes -= op->size;
  free(op->bytes);
  memset(op, 0, sizeof(*op));
}

void caml_actor_io_consume(struct caml_actor_io *io, uintnat id)
{
  struct io_operation *op = operation(io, id);
  CAMLassert(op != NULL && op->status != CAML_ACTOR_IO_PENDING);
  release_operation(io, op, 1);
}

static int compare_ids(const void *a, const void *b)
{
  uintnat x = *(const uintnat *)a, y = *(const uintnat *)b;
  return x < y ? -1 : x > y;
}

int caml_actor_io_poll(struct caml_actor_io *io,
  void (*ready)(void *, uintnat, uintnat), void *context)
{
  uintnat ids[CAML_ACTOR_IO_BATCH];
  int count;
  if (io->failed) return 0;
  if (io->stats.operations == 0) return 1;
  count = io->backend.poll(io->backend.context, ids, CAML_ACTOR_IO_BATCH);
  if (count < 0 || count > CAML_ACTOR_IO_BATCH) { io->failed = 1; return 0; }
  qsort(ids, count, sizeof(*ids), compare_ids);
  for (int i = 0; i < count; i++) {
    struct io_operation *op;
    if (i != 0 && ids[i] == ids[i-1]) continue;
    op = operation(io, ids[i]);
    if (op == NULL || op->status != CAML_ACTOR_IO_PENDING) continue;
    progress(io, op);
    if (op->status != CAML_ACTOR_IO_PENDING) ready(context, op->owner, op->id);
  }
  return !io->failed;
}

int caml_actor_io_wait(struct caml_actor_io *io,
                       struct caml_actor_timers *timers, uint64_t deadline)
{
  if (io->failed || io->backend.wait == NULL) return -1;
  return io->backend.wait(io->backend.context, timers, deadline);
}

enum caml_actor_io_status caml_actor_io_prepare_transfer(
  struct caml_actor_io *io, uintnat owner, uintnat id, uintnat *fresh)
{
  struct io_resource *r = resource(io, id);
  if (r == NULL || r->owner != owner || r->kind != CAML_ACTOR_IO_SOCKET
      || r->fd < 0) return CAML_ACTOR_IO_HANDLE;
  if (r->operation != 0) return CAML_ACTOR_IO_BUSY;
  *fresh = identity(io);
  return *fresh == 0 ? CAML_ACTOR_IO_UNAVAILABLE : CAML_ACTOR_IO_OK;
}

void caml_actor_io_commit_transfer(struct caml_actor_io *io,
  uintnat owner, uintnat id, uintnat destination, uintnat fresh)
{
  struct io_resource *r = resource(io, id);
  CAMLassert(r != NULL && r->owner == owner && r->operation == 0 && fresh > 0);
  r->id = fresh;
  r->owner = destination;
}

void caml_actor_io_retire(struct caml_actor_io *io, uintnat owner)
{
  if (io == NULL) return;
  if (io->operations != NULL) {
    for (uintnat i = 0; i < io->stats.operation_limit; i++) {
      struct io_operation *op = &io->operations[i];
      if (op->id != 0 && (owner == 0 || op->owner == owner))
        release_operation(io, op, 0);
    }
  }
  if (io->resources != NULL) {
    for (uintnat i = 0; i < io->stats.resource_limit; i++) {
      struct io_resource *r = &io->resources[i];
      if (r->id != 0 && (owner == 0 || r->owner == owner))
        release_resource(io, r);
    }
  }
}

void caml_actor_io_destroy(struct caml_actor_io *io)
{
  if (io == NULL) return;
  caml_actor_io_retire(io, 0);
  if (io->backend.destroy != NULL) io->backend.destroy(io->backend.context);
  free(io->resources);
  free(io->operations);
  free(io);
}

void caml_actor_io_stats(const struct caml_actor_io *io,
                         struct caml_actor_io_stats *stats)
{
  memset(stats, 0, sizeof(*stats));
  if (io != NULL) *stats = io->stats;
}

void caml_actor_io_test_fail_allocation(struct caml_actor_io *io)
{
  io->fail_allocation = 1;
}

void caml_actor_io_test_exhaust_identity(struct caml_actor_io *io)
{
  io->exhausted = 1;
}
