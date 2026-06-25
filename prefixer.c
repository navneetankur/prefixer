#include <errno.h>
// sync-v1-0730275q: workspace/c/prefixer.c
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define BUF_SIZE 4096
#define PREFIX_MAX 256
#define CONT_SUFFIX " [cont]\n"

static void die(const char *msg) {
  perror(msg);
  exit(1);
}

static void write_lossy_to_stdout(const void *buf, size_t len) {
  const char *p = buf;

  while (len > 0) {
    ssize_t n = write(STDOUT_FILENO, p, len);

    if (n < 0) {
      if (errno == EINTR)
        continue;

      return; /* drop remainder */
    }

    p += n;
    len -= (size_t)n;
  }
}

static void write_all(int fd, const void *buf, size_t len) {
  const char *p = buf;

  while (len > 0) {
    ssize_t n = write(fd, p, len);

    if (n < 0) {
      if (errno == EINTR) {
        continue;
      } else if (errno == EAGAIN || errno == EPIPE || errno == EWOULDBLOCK) {
        if (fd != STDOUT_FILENO)
          write_lossy_to_stdout(p, len);

        return;
      }

      die("write");
    }

    p += n;
    len -= (size_t)n;
  }
}

static int open_input(const char *path) {
  if (strcmp(path, "-") == 0)
    return STDIN_FILENO;

  int fd = open(path, O_RDONLY);

  if (fd < 0)
    die(path);

  return fd;
}

static int open_output(const char *path) {
  if (strcmp(path, "-") == 0)
    return STDOUT_FILENO;

  int fd = open(path, O_WRONLY | O_APPEND | O_NONBLOCK);
  return fd;
}

static void set_nonblock(int fd) {
  int flags = fcntl(fd, F_GETFL);

  if (flags < 0)
    die("fcntl");

  if (fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0)
    die("fcntl");
}

int main(int argc, char **argv) {
  if (argc != 4) {
    fprintf(stderr, "usage: %s PREFIX INPUT OUTPUT\n", argv[0]);

    return 1;
  }

  const char *name = argv[1];

  if (name[0] == '-') {
    fprintf(stderr, "prefix may not begin with '-'\n");

    return 1;
  }

  signal(SIGPIPE, SIG_IGN);

  int in_fd = open_input(argv[2]);
  bool fallback_open = false;
  const char *const out_filename = argv[3];
  int out_fd = open_output(out_filename);
  if (out_fd < 0) {
    out_fd = STDOUT_FILENO;
    fallback_open = true;
  }
  set_nonblock(STDOUT_FILENO);

  char prefix[PREFIX_MAX];

  int prefix_len = snprintf(prefix, sizeof(prefix), "[%s ] ", name);

  if (prefix_len < 0 || (size_t)prefix_len >= sizeof(prefix)) {
    fprintf(stderr, "prefix too long\n");
    return 1;
  }

  const size_t suffix_len = sizeof(CONT_SUFFIX) - 1;

  /*
   * Buffer layout:
   *
   * | prefix | payload | suffix |
   */

  char buf[BUF_SIZE];

  memcpy(buf, prefix, (size_t)prefix_len);

  char *payload = buf + prefix_len;

  const size_t payload_size = sizeof(buf) - (size_t)prefix_len - suffix_len;

  memcpy(payload + payload_size, CONT_SUFFIX, suffix_len);

  size_t used = 0;

  bool continued = false;

  for (;;) {
    ssize_t n = read(in_fd, payload + used, payload_size - used);

    if (n < 0) {
      if (errno == EINTR)
        continue;

      die("read");
    }

    if (n == 0)
      break;
    if (fallback_open) {
      int new_fd = open_output(out_filename);
      if (new_fd >= 0) {
        out_fd = new_fd;
        fallback_open = false;
      }
    }

    used += (size_t)n;

    size_t last_newline = 0;

    for (size_t i = 0; i < used; i++) {
      if (payload[i] == '\n')
        last_newline = i + 1;
    }

    if (last_newline > 0) {
      buf[prefix_len - 3] = continued ? '+' : ' ';

      write_all(out_fd, buf, (size_t)prefix_len + last_newline);

      continued = false;

      size_t leftover = used - last_newline;

      memmove(payload, payload + last_newline, leftover);

      used = leftover;
    }

    if (used == payload_size) {
      buf[prefix_len - 3] = continued ? '+' : ' ';

      write_all(out_fd, buf, sizeof(buf));

      used = 0;
      continued = true;
    }
  }

  if (used > 0) {
    buf[prefix_len - 3] = continued ? '+' : ' ';

    payload[used++] = '\n';

    write_all(out_fd, buf, (size_t)prefix_len + used);
  }

  return 0;
}
