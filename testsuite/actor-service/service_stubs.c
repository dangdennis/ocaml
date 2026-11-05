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
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>
#include "caml/actor_scheduler.h"
#include "caml/misc.h"

#define CHECK(e) \
  do { if (!(e)) caml_fatal_error("service fixture: " #e); } while (0)
static struct caml_actor_io_backend real_backend;
static int baseline, fault_fd = -1, faults, pin_port, first_port;
static uintnat blocked_writes;

static int descriptors(void)
{
  DIR *directory = opendir("/proc/self/fd");
  struct dirent *entry;
  int count = 0;
  CHECK(directory != NULL);
  while ((entry = readdir(directory)) != NULL)
    if (entry->d_name[0] != '.') count++;
  closedir(directory);
  return count;
}

static int listen_ready(void *context, const char *address,
                        int port, int *bound)
{
  int requested = pin_port && first_port != 0 ? first_port : port;
  int result = real_backend.listen(context, address, requested, bound);
  if (result >= 0 && first_port == 0) first_port = *bound;
  if (result < 0 && pin_port)
    fprintf(stderr, "pinned listener bind failed: errno=%d\n", -result);
  if (result >= 0) { printf("READY %d\n", *bound); fflush(stdout); }
  return result;
}

static int accept_fault(void *context, int listener)
{
  unsigned char requested;
  int fd, size = 4096;
  if (fault_fd >= 0 && read(fault_fd, &requested, 1) == 1) faults = requested;
  if (faults > 0) { faults--; return -EIO; }
  fd = real_backend.accept(context, listener);
  if (fd >= 0)
    CHECK(setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
  return fd;
}

static intnat write_count(void *context, int fd, const char *bytes,
                          uintnat size)
{
  intnat result = real_backend.write(context, fd, bytes, size);
  if (result == -EAGAIN || result == -EWOULDBLOCK) {
    if (blocked_writes++ == 0) { puts("BLOCKED"); fflush(stdout); }
  }
  return result;
}

CAMLprim value caml_actor_service_setup(value unit)
{
  struct caml_actor_io_backend backend;
  const char *text = getenv("ACTOR_KV_FAULT_FD");
  (void)unit;
  baseline = descriptors();
  pin_port = getenv("ACTOR_KV_PIN_PORT") != NULL;
  if (text != NULL) {
    fault_fd = atoi(text);
    CHECK(fcntl(fault_fd, F_SETFL, O_NONBLOCK) == 0);
  }
  CHECK(caml_actor_io_default_backend(&real_backend));
  backend = real_backend;
  backend.listen = listen_ready;
  backend.accept = accept_fault;
  backend.write = write_count;
  caml_actor_scheduler_test_io_backend(&backend);
  return Val_unit;
}

CAMLprim value caml_actor_service_finish(value unit)
{
  (void)unit;
  caml_actor_scheduler_test_io_backend(NULL);
  CHECK(descriptors() == baseline);
  printf("CLEAN blocked_writes=%lu\n", (unsigned long)blocked_writes);
  fflush(stdout);
  return Val_unit;
}
