#define CAML_INTERNALS
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <arpa/inet.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#include <string.h>
#include "caml/actor_scheduler.h"
#include "caml/misc.h"

#define CHECK(e) do { if (!(e)) caml_fatal_error("loopback: " #e); } while (0)
static struct caml_actor_io_backend real;
static pid_t client;
static int notification, baseline, backpressure, announced;

static int descriptors(void)
{
  DIR *directory = opendir("/proc/self/fd");
  struct dirent *entry;
  int count = 0;
  CHECK(directory != NULL);
  while ((entry = readdir(directory)) != NULL) {
    if (entry->d_name[0] != '.') count++;
  }
  closedir(directory);
  return count;
}

static int announcing_listen(void *context, const char *address, int port,
                             int *bound)
{
  int fd = real.listen(context, address, port, bound);
  if (fd >= 0) CHECK(write(notification, bound, sizeof(*bound)) == sizeof(*bound));
  return fd;
}

static int small_accept(void *context, int fd)
{
  int accepted = real.accept(context, fd);
  int size = 4096;
  if (accepted >= 0 && backpressure)
    CHECK(setsockopt(accepted, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) == 0);
  return accepted;
}

static int announcing_watch(void *context, int fd, uintnat id, int writing)
{
  int result = real.watch(context, fd, id, writing);
  if (result == 0 && writing && backpressure && !announced) {
    char byte = 1;
    CHECK(write(notification, &byte, 1) == 1);
    announced = 1;
  }
  return result;
}

static void run_client(int pipe)
{
  struct sockaddr_in address;
  char buffer[8192] = {0};
  int port, fd, count = 0;
  /* Watchdog only: protocol progress uses the port pipe and socket EOF. */
  alarm(30);
  CHECK(read(pipe, &port, sizeof(port)) == sizeof(port));
  fd = socket(AF_INET, SOCK_STREAM, 0);
  CHECK(fd >= 0);
  memset(&address, 0, sizeof(address));
  address.sin_family = AF_INET;
  address.sin_port = htons(port);
  CHECK(inet_pton(AF_INET, "127.0.0.1", &address.sin_addr) == 1);
  CHECK(connect(fd, (struct sockaddr *)&address, sizeof(address)) == 0);
  CHECK(send(fd, "he", 2, MSG_NOSIGNAL) == 2);
  CHECK(send(fd, "llo", 3, MSG_NOSIGNAL) == 3);
  CHECK(shutdown(fd, SHUT_WR) == 0);
  if (backpressure) {
    char notification;
    CHECK(read(pipe, &notification, 1) == 1);
  }
  close(pipe);
  for (;;) {
    ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
    if (n == 0) break;
    if (n < 0 && errno == EINTR) continue;
    CHECK(n > 0);
    for (ssize_t i = 0; i < n; i++) {
      int expected = backpressure ? ((count + i) % 65536) % 251
        : count + i < 5 ? "hello"[count + i] : -1;
      CHECK((unsigned char)buffer[i] == expected);
    }
    count += n;
  }
  CHECK(count == (backpressure ? 4 * 65536 : 5));
  close(fd);
  _exit(0);
}

CAMLprim value caml_actor_test_net_client(value mode)
{
  struct caml_actor_io_backend backend;
  int pipes[2];
  backpressure = Int_val(mode);
  announced = 0;
  baseline = descriptors();
  CHECK(pipe2(pipes, O_CLOEXEC) == 0);
  client = fork();
  CHECK(client >= 0);
  if (client == 0) { close(pipes[1]); run_client(pipes[0]); }
  close(pipes[0]);
  notification = pipes[1];
  CHECK(caml_actor_io_default_backend(&real));
  backend = real;
  backend.listen = announcing_listen;
  backend.accept = small_accept;
  backend.watch = announcing_watch;
  caml_actor_scheduler_test_io_backend(&backend);
  return Val_unit;
}

CAMLprim value caml_actor_test_net_join(value unit)
{
  int status;
  (void)unit;
  close(notification);
  CHECK(waitpid(client, &status, 0) == client);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  CHECK(!backpressure || announced);
  caml_actor_scheduler_test_io_backend(NULL);
  CHECK(descriptors() == baseline);
  return Val_unit;
}

static sigset_t signal_mask;
CAMLprim value caml_actor_test_net_signal(value unit)
{
  (void)unit;
  baseline = descriptors();
  CHECK(pthread_sigmask(SIG_SETMASK, NULL, &signal_mask) == 0);
  caml_actor_timers_test_signal_before_wait();
  return Val_unit;
}

CAMLprim value caml_actor_test_net_signal_clean(value unit)
{
  sigset_t current;
  (void)unit;
  CHECK(descriptors() == baseline);
  CHECK(pthread_sigmask(SIG_SETMASK, NULL, &current) == 0);
  for (int signal = 1; signal < NSIG; signal++)
    CHECK(sigismember(&current, signal) == sigismember(&signal_mask, signal));
  return Val_unit;
}
