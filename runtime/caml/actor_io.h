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

#ifndef CAML_ACTOR_IO_H
#define CAML_ACTOR_IO_H
#ifdef CAML_INTERNALS

#include "mlvalues.h"
#include "actor_timer.h"

#define CAML_ACTOR_IO_RESOURCES 1024
#define CAML_ACTOR_IO_OPERATIONS 1024
#define CAML_ACTOR_IO_BYTES (8 * 1024 * 1024)
#define CAML_ACTOR_IO_CHUNK 65536
#define CAML_ACTOR_IO_BATCH 64

struct caml_actor_io;

/* Constant constructors in Actor.Net.error, followed by its Io_error block. */
enum caml_actor_io_status {
  CAML_ACTOR_IO_ARGUMENT = 0, CAML_ACTOR_IO_HANDLE,
  CAML_ACTOR_IO_RESOURCE_LIMIT, CAML_ACTOR_IO_OPERATION_LIMIT,
  CAML_ACTOR_IO_BUFFER_LIMIT, CAML_ACTOR_IO_UNAVAILABLE,
  CAML_ACTOR_IO_UNSUPPORTED, CAML_ACTOR_IO_MISSING,
  CAML_ACTOR_IO_MAILBOX_LIMIT, CAML_ACTOR_IO_SELF, CAML_ACTOR_IO_BUSY,
  CAML_ACTOR_IO_SYSTEM,
  CAML_ACTOR_IO_OK, CAML_ACTOR_IO_PENDING
};

enum caml_actor_io_kind { CAML_ACTOR_IO_LISTENER, CAML_ACTOR_IO_SOCKET };
enum caml_actor_io_operation {
  CAML_ACTOR_IO_ACCEPT, CAML_ACTOR_IO_READ, CAML_ACTOR_IO_WRITE
};

struct caml_actor_io_stats {
  uintnat resources, peak_resources, operations, peak_operations;
  uintnat bytes, peak_bytes, quota_failures, closed, errors;
  uintnat resource_limit, operation_limit, byte_limit;
};

/* Host-only backend. No callback retains an OCaml value or actor address.
   System-call callbacks return a nonnegative result or a negated errno.
   poll returns operation identities (not descriptors), at most capacity.
   wait returns 1 on wake, 0 on interruption, -1 on fatal failure. */
struct caml_actor_io_backend {
  void *context;
  int (*listen)(void *, const char *, int, int *);
  int (*accept)(void *, int);
  intnat (*read)(void *, int, char *, uintnat);
  intnat (*write)(void *, int, const char *, uintnat);
  void (*close)(void *, int);
  int (*watch)(void *, int, uintnat, int);
  int (*unwatch)(void *, int);
  int (*poll)(void *, uintnat *, int);
  int (*wait)(void *, struct caml_actor_timers *, uint64_t);
  void (*destroy)(void *);
};

CAMLextern int caml_actor_io_default_backend(struct caml_actor_io_backend *);
CAMLextern struct caml_actor_io *caml_actor_io_create(
  uintnat, uintnat, uintnat, const struct caml_actor_io_backend *);
CAMLextern void caml_actor_io_destroy(struct caml_actor_io *);
CAMLextern enum caml_actor_io_status caml_actor_io_listen(
  struct caml_actor_io *, uintnat, const char *, int, uintnat *, int *);
CAMLextern enum caml_actor_io_status caml_actor_io_port(
  struct caml_actor_io *, uintnat, uintnat, int *);
CAMLextern enum caml_actor_io_status caml_actor_io_close(
  struct caml_actor_io *, uintnat, uintnat, enum caml_actor_io_kind);
/* submit is called only once; completion remains charged until consume.
   The caller reserves its OCaml result before submitting irreversible I/O. */
CAMLextern enum caml_actor_io_status caml_actor_io_submit(
  struct caml_actor_io *, uintnat, uintnat, enum caml_actor_io_operation,
  const char *, uintnat, uintnat *, int *);
CAMLextern uintnat caml_actor_io_owner_operation(
  struct caml_actor_io *, uintnat);
CAMLextern enum caml_actor_io_status caml_actor_io_result(
  struct caml_actor_io *, uintnat, uintnat, enum caml_actor_io_operation,
  uintnat, uintnat *, const char **, int *);
CAMLextern void caml_actor_io_consume(struct caml_actor_io *, uintnat);
CAMLextern int caml_actor_io_poll(struct caml_actor_io *,
  void (*)(void *, uintnat, uintnat), void *);
CAMLextern int caml_actor_io_wait(
  struct caml_actor_io *, struct caml_actor_timers *, uint64_t);
CAMLextern enum caml_actor_io_status caml_actor_io_prepare_transfer(
  struct caml_actor_io *, uintnat, uintnat, uintnat *);
CAMLextern void caml_actor_io_commit_transfer(
  struct caml_actor_io *, uintnat, uintnat, uintnat, uintnat);
CAMLextern void caml_actor_io_retire(struct caml_actor_io *, uintnat);
CAMLextern void caml_actor_io_stats(
  const struct caml_actor_io *, struct caml_actor_io_stats *);
CAMLextern void caml_actor_io_test_fail_allocation(struct caml_actor_io *);
CAMLextern void caml_actor_io_test_exhaust_identity(struct caml_actor_io *);

#endif
#endif
