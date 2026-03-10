/****************************************************************************
 * apps/system/dmesg/dmesg_main.c
 ****************************************************************************/

#include <nuttx/config.h>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int open_log_device(char *chosen, size_t len)
{
  static const char *candidates[] =
  {
    "/dev/kmsg",
    "/dev/ramlog",
    "/dev/syslog",
    NULL
  };

  int fd = -1;

  for (int i = 0; candidates[i] != NULL; i++)
    {
      fd = open(candidates[i], O_RDONLY);
      if (fd >= 0)
        {
          strncpy(chosen, candidates[i], len - 1);
          chosen[len - 1] = '\0';
          return fd;
        }
    }

  chosen[0] = '\0';
  return -1;
}

int main(int argc, FAR char *argv[])
{
  char devpath[32];
  char buf[256];
  ssize_t nread;
  int fd;

  (void)argc;
  (void)argv;

  fd = open_log_device(devpath, sizeof(devpath));
  if (fd < 0)
    {
      fprintf(stderr,
              "dmesg: cannot open kernel log (/dev/kmsg, /dev/ramlog, /dev/syslog): %s\n",
              strerror(errno));
      return EXIT_FAILURE;
    }

  while ((nread = read(fd, buf, sizeof(buf))) > 0)
    {
      ssize_t off = 0;
      while (off < nread)
        {
          ssize_t nw = write(STDOUT_FILENO, buf + off, nread - off);
          if (nw < 0)
            {
              close(fd);
              fprintf(stderr, "dmesg: write failed: %s\n", strerror(errno));
              return EXIT_FAILURE;
            }

          off += nw;
        }
    }

  if (nread < 0)
    {
      fprintf(stderr, "dmesg: read %s failed: %s\n", devpath, strerror(errno));
      close(fd);
      return EXIT_FAILURE;
    }

  close(fd);
  return EXIT_SUCCESS;
}
