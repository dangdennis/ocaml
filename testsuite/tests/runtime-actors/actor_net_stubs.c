#define CAML_INTERNALS
#include <errno.h>
#include <string.h>
#include "caml/actor_scheduler.h"
#include "caml/misc.h"
#include "caml/signals.h"
#include <signal.h>

#define CHECK(e) do { if (!(e)) caml_fatal_error("net: " #e); } while (0)

struct fake_io {
  int live[32], ready, accepts, reads, writes, closed, waits, mode;
  int read_offset, output_length, watched, fail_watch, fail_poll;
  uintnat watch[32], stale;
  char output[128];
  uint64_t ticks;
};
static struct fake_io fixture;

static int descriptor(struct fake_io *f)
{
  for (int i = 1; i < 32; i++) {
    if (!f->live[i]) { f->live[i] = 1; return i; }
  }
  return -EMFILE;
}

static int fake_listen(void *p, const char *address, int port, int *bound)
{
  (void)address; (void)port;
  *bound = 12345;
  return descriptor(p);
}

static int fake_accept(void *p, int fd)
{
  struct fake_io *f = p;
  CHECK(f->live[fd]);
  f->accepts++;
  if (f->mode == 9) return -ECONNABORTED;
  if (!f->ready || f->mode == 0 || f->mode == 6) return -EAGAIN;
  return descriptor(f);
}

static intnat fake_read(void *p, int fd, char *bytes, uintnat size)
{
  struct fake_io *f = p;
  uintnat count = 5 - f->read_offset;
  CHECK(f->live[fd]);
  f->reads++;
  if (f->mode == 8 && f->reads == 1) return -EINTR;
  if (count > size) count = size;
  if (count > 3) count = 3;
  memcpy(bytes, "hello" + f->read_offset, count);
  f->read_offset += count;
  return count;
}

static intnat fake_write(void *p, int fd, const char *bytes, uintnat size)
{
  struct fake_io *f = p;
  CHECK(f->live[fd]);
  f->writes++;
  if (f->mode == 8 && f->writes == 1) return -EAGAIN;
  if (size > 2) size = 2;
  CHECK(f->output_length + size <= sizeof(f->output));
  memcpy(f->output + f->output_length, bytes, size);
  f->output_length += size;
  return size;
}

static void fake_close(void *p, int fd)
{
  struct fake_io *f = p;
  CHECK(f->live[fd] && f->watch[fd] == 0);
  f->live[fd] = 0;
  f->closed++;
}

static int fake_watch(void *p, int fd, uintnat id, int writing)
{
  struct fake_io *f = p;
  (void)writing;
  CHECK(f->watch[fd] == 0);
  if (f->fail_watch) return -ENOMEM;
  f->watch[fd] = id;
  f->watched++;
  return 0;
}

static int fake_unwatch(void *p, int fd)
{
  struct fake_io *f = p;
  CHECK(f->watch[fd] != 0);
  f->stale = f->watch[fd];
  f->watch[fd] = 0;
  f->watched--;
  return f->mode == 5 ? -EIO : 0;
}

static int fake_poll(void *p, uintnat *ids, int capacity)
{
  struct fake_io *f = p;
  int count = 0;
  if (f->fail_poll || f->mode == 3) return -EIO;
  if (f->mode == 2 || f->mode == 5 || f->mode == 8) f->ready = 1;
  if (f->stale != 0) ids[count++] = f->stale;
  if (!f->ready) return count;
  /* Deliberately reversed and duplicated; the engine must normalize it. */
  for (int i = 31; i > 0 && count + 2 <= capacity; i--) {
    if (f->watch[i] != 0) {
      ids[count++] = f->watch[i];
      ids[count++] = f->watch[i];
    }
  }
  return count;
}

static int fake_wait(void *p, struct caml_actor_timers *timers, uint64_t deadline)
{
  struct fake_io *f = p;
  (void)timers;
  f->waits++;
  if (f->mode == 4) return -1;
  if (f->mode == 7) { caml_record_signal(SIGUSR1); return 0; }
  if (f->mode == 6) return 0;
  if (deadline != UINT64_MAX) f->ticks = deadline;
  f->ready = 1;
  return 1;
}

static int fake_now(void *p, uint64_t *now)
{
  struct fake_io *f = p;
  *now = f->ticks;
  f->ticks += 1000000;
  return 1;
}

static int fake_timer_wait(void *p, uint64_t deadline)
{
  struct fake_io *f = p;
  f->ticks = deadline;
  return 1;
}

static struct caml_actor_io_backend backend(struct fake_io *f)
{
  return (struct caml_actor_io_backend) {
    f, fake_listen, fake_accept, fake_read, fake_write, fake_close,
    fake_watch, fake_unwatch, fake_poll, fake_wait, NULL
  };
}

CAMLprim value caml_actor_test_net_setup(value mode)
{
  struct caml_actor_io_backend b;
  struct caml_actor_timer_backend clock = { &fixture, fake_now, fake_timer_wait };
  memset(&fixture, 0, sizeof(fixture));
  fixture.mode = Int_val(mode);
  b = backend(&fixture);
  caml_actor_scheduler_test_io_backend(&b);
  caml_actor_scheduler_test_timer_backend(&clock);
  return Val_unit;
}

CAMLprim value caml_actor_test_net_clean(value unit)
{
  (void)unit;
  CHECK(fixture.watched == 0);
  for (int i = 0; i < 32; i++) CHECK(fixture.live[i] == 0);
  caml_actor_scheduler_test_io_backend(NULL);
  caml_actor_scheduler_test_timer_backend(NULL);
  return Val_unit;
}

CAMLprim value caml_actor_test_net_output(value unit)
{
  (void)unit;
  CHECK(fixture.output_length == 5 && memcmp(fixture.output, "hello", 5) == 0);
  return Val_unit;
}

struct notifications { uintnat count, ids[8]; };
static void notified(void *p, uintnat owner, uintnat id)
{
  struct notifications *n = p;
  CHECK(owner == 9 || owner == 10);
  CHECK(n->count < 8);
  n->ids[n->count++] = id;
}

CAMLprim value caml_actor_test_net_store(value unit)
{
  struct fake_io f = {0};
  struct notifications n = {0};
  struct caml_actor_io_backend b = backend(&f);
  struct caml_actor_io *io = caml_actor_io_create(4, 2, 8, &b);
  struct caml_actor_io_stats stats;
  uintnat listener, other, op, op2, socket, result, fresh, old_op;
  int error = 0, port;
  const char *bytes;
  (void)unit;
  CHECK(io != NULL);
  caml_actor_io_test_fail_allocation(io);
  CHECK(caml_actor_io_listen(io, 9, "127.0.0.1", 0, &listener, &error)
        == CAML_ACTOR_IO_UNAVAILABLE);
  CHECK(caml_actor_io_listen(io, 9, "127.0.0.1", 0, &listener, &error)
        == CAML_ACTOR_IO_OK);
  CHECK(caml_actor_io_port(io, 10, listener, &port) == CAML_ACTOR_IO_HANDLE);
  caml_actor_io_test_fail_allocation(io);
  CHECK(caml_actor_io_submit(io, 9, listener, CAML_ACTOR_IO_ACCEPT,
                            NULL, 0, &op, &error) == CAML_ACTOR_IO_UNAVAILABLE);
  CHECK(caml_actor_io_submit(io, 9, listener, CAML_ACTOR_IO_ACCEPT,
                            NULL, 0, &op, &error) == CAML_ACTOR_IO_PENDING);
  old_op = op;
  CHECK(caml_actor_io_listen(io, 10, "127.0.0.1", 0, &other, &error)
        == CAML_ACTOR_IO_OK);
  CHECK(caml_actor_io_submit(io, 10, other, CAML_ACTOR_IO_ACCEPT,
                            NULL, 0, &op2, &error) == CAML_ACTOR_IO_PENDING);
  caml_actor_io_stats(io, &stats);
  CHECK(stats.resources == 4 && stats.operations == 2);
  CHECK(caml_actor_io_poll(io, notified, &n) && n.count == 0);
  f.ready = 1; f.mode = 1;
  CHECK(caml_actor_io_poll(io, notified, &n));
  CHECK(n.count == 2 && n.ids[0] == op && n.ids[1] == op2);
  CHECK(f.accepts == 4);
  CHECK(caml_actor_io_poll(io, notified, &n) && n.count == 2 && f.accepts == 4);
  CHECK(caml_actor_io_result(io, 9, listener, CAML_ACTOR_IO_ACCEPT, op,
                            &socket, &bytes, &error) == CAML_ACTOR_IO_OK);
  caml_actor_io_consume(io, op);
  caml_actor_io_retire(io, 10); /* Unconsumed accepted socket is closed. */
  CHECK(caml_actor_io_submit(io, 9, socket, CAML_ACTOR_IO_READ, NULL, 9,
                            &op, &error) == CAML_ACTOR_IO_BUFFER_LIMIT);
  caml_actor_io_test_fail_allocation(io);
  CHECK(caml_actor_io_submit(io, 9, socket, CAML_ACTOR_IO_READ, NULL, 4,
                            &op, &error) == CAML_ACTOR_IO_UNAVAILABLE);
  CHECK(caml_actor_io_submit(io, 9, socket, CAML_ACTOR_IO_READ, NULL, 4,
                            &op, &error) == CAML_ACTOR_IO_OK);
  CHECK(caml_actor_io_result(io, 9, socket, CAML_ACTOR_IO_READ, op,
                            &result, &bytes, &error) == CAML_ACTOR_IO_OK);
  CHECK(result == 3 && memcmp(bytes, "hel", 3) == 0);
  CHECK(caml_actor_io_result(io, 9, socket, CAML_ACTOR_IO_READ, op,
                            &result, &bytes, &error) == CAML_ACTOR_IO_OK);
  CHECK(f.reads == 1);
  caml_actor_io_consume(io, op);
  CHECK(caml_actor_io_prepare_transfer(io, 9, socket, &fresh) == CAML_ACTOR_IO_OK);
  caml_actor_io_commit_transfer(io, 9, socket, 10, fresh);
  CHECK(caml_actor_io_close(io, 9, socket, CAML_ACTOR_IO_SOCKET)
        == CAML_ACTOR_IO_HANDLE);
  CHECK(caml_actor_io_prepare_transfer(io, 10, fresh, &result) == CAML_ACTOR_IO_OK);
  caml_actor_io_commit_transfer(io, 10, fresh, 9, result);
  CHECK(caml_actor_io_close(io, 9, socket, CAML_ACTOR_IO_SOCKET)
        == CAML_ACTOR_IO_HANDLE);
  CHECK(caml_actor_io_close(io, 9, result, CAML_ACTOR_IO_SOCKET) == CAML_ACTOR_IO_OK);
  for (int i = 0; i < 200; i++) {
    f.ready = 0; f.mode = 0; f.stale = old_op;
    CHECK(caml_actor_io_submit(io, 9, listener, CAML_ACTOR_IO_ACCEPT,
                              NULL, 0, &op, &error) == CAML_ACTOR_IO_PENDING);
    CHECK(caml_actor_io_poll(io, notified, &n) && n.count == 2);
    old_op = op;
    caml_actor_io_retire(io, 9);
    CHECK(caml_actor_io_listen(io, 9, "127.0.0.1", 0, &listener, &error)
          == CAML_ACTOR_IO_OK);
  }
  caml_actor_io_retire(io, 0);
  caml_actor_io_stats(io, &stats);
  CHECK(stats.resources == 0 && stats.operations == 0 && stats.bytes == 0);
  /* Registration failure publishes a recoverable completion and returns
     its reserved accept slot on consumption. */
  CHECK(caml_actor_io_listen(io, 9, "127.0.0.1", 0, &listener, &error)
        == CAML_ACTOR_IO_OK);
  f.fail_watch = 1;
  CHECK(caml_actor_io_submit(io, 9, listener, CAML_ACTOR_IO_ACCEPT,
                            NULL, 0, &op, &error) == CAML_ACTOR_IO_SYSTEM);
  CHECK(error == ENOMEM);
  caml_actor_io_consume(io, op);
  caml_actor_io_stats(io, &stats);
  CHECK(stats.resources == 1 && stats.operations == 0);
  caml_actor_io_retire(io, 0);
  caml_actor_io_test_exhaust_identity(io);
  CHECK(caml_actor_io_listen(io, 9, "127.0.0.1", 0, &listener, &error)
        == CAML_ACTOR_IO_UNAVAILABLE);
  caml_actor_io_destroy(io);
  CHECK(f.watched == 0);
  for (int i = 0; i < 32; i++) CHECK(f.live[i] == 0);
  return Val_unit;
}
