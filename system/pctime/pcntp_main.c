/****************************************************************************
 * apps/system/pctime/pcntp_main.c
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>

#include <arch/board/board.h>
#include <netutils/ntpclient.h>

static void ntp_usage(void)
{
  fprintf(stderr,
          "Usage:\n"
          "  ntp start [server]\n"
          "  ntp stop\n"
          "  ntp status\n"
          "  ntp sync [server]\n");
}

static const char *ntp_geo_server(void)
{
  const char *tz = getenv("TZ");

  if (tz != NULL)
    {
      if (strstr(tz, "Asia") != NULL) return "asia.pool.ntp.org";
      if (strstr(tz, "Europe") != NULL) return "europe.pool.ntp.org";
      if (strstr(tz, "America") != NULL) return "north-america.pool.ntp.org";
      if (strstr(tz, "Australia") != NULL) return "oceania.pool.ntp.org";
      if (strstr(tz, "Africa") != NULL) return "africa.pool.ntp.org";
    }

  return "pool.ntp.org";
}

static int ntp_start_cmd(const char *server)
{
  int pid;

  if (server != NULL)
    {
      pid = ntpc_start_with_list(server);
    }
  else
    {
      pid = ntpc_start_with_list(ntp_geo_server());
    }

  if (pid < 0)
    {
      fprintf(stderr, "ntp: failed to start (%d)\n", pid);
      return EXIT_FAILURE;
    }

  printf("ntp: started (pid=%d)\n", pid);
  return EXIT_SUCCESS;
}

static int ntp_status_cmd(void)
{
  struct ntpc_status_s st;
  int ret = ntpc_status(&st);
  if (ret < 0)
    {
      fprintf(stderr, "ntp: status failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  printf("ntp: samples=%u\n", st.nsamples);
  if (st.nsamples > 0)
    {
      printf("ntp: last offset=%lld us delay=%lld us\n",
             (long long)st.samples[0].offset,
             (long long)st.samples[0].delay);
    }

  return EXIT_SUCCESS;
}

static int ntp_sync_cmd(const char *server)
{
  int ret = ntp_start_cmd(server);
  if (ret != EXIT_SUCCESS)
    {
      return ret;
    }

  for (int i = 0; i < 8; i++)
    {
      usleep(500000);

      struct ntpc_status_s st;
      if (ntpc_status(&st) == 0 && st.nsamples > 0)
        {
          struct timespec ts;
          if (clock_gettime(CLOCK_REALTIME, &ts) == 0)
            {
              (void)rp23xx_aon_walltime_settime(&ts);
            }

          printf("ntp: sync complete\n");
          (void)ntpc_stop();
          return EXIT_SUCCESS;
        }
    }

  fprintf(stderr, "ntp: sync timeout\n");
  (void)ntpc_stop();
  return EXIT_FAILURE;
}

int main(int argc, FAR char *argv[])
{
  if (argc < 2)
    {
      ntp_usage();
      return EXIT_FAILURE;
    }

  if (strcmp(argv[1], "start") == 0)
    {
      return ntp_start_cmd(argc >= 3 ? argv[2] : NULL);
    }

  if (strcmp(argv[1], "stop") == 0)
    {
      (void)ntpc_stop();
      printf("ntp: stopped\n");
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "status") == 0)
    {
      return ntp_status_cmd();
    }

  if (strcmp(argv[1], "sync") == 0)
    {
      return ntp_sync_cmd(argc >= 3 ? argv[2] : NULL);
    }

  ntp_usage();
  return EXIT_FAILURE;
}
