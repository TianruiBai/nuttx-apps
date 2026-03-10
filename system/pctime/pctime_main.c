/****************************************************************************
 * apps/system/pctime/pctime_main.c
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <arch/board/board.h>

static void pctime_usage(void)
{
  fprintf(stderr,
          "Usage:\n"
          "  time\n"
          "  time set YYYY-MM-DD HH:MM:SS\n");
}

static void pctime_print_now(void)
{
  struct timespec ts;
  struct tm tm;

  if (rp23xx_aon_walltime_gettime(&ts) < 0)
    {
      clock_gettime(CLOCK_REALTIME, &ts);
    }

  localtime_r(&ts.tv_sec, &tm);

  printf("%04d-%02d-%02d %02d:%02d:%02d\n",
         tm.tm_year + 1900,
         tm.tm_mon + 1,
         tm.tm_mday,
         tm.tm_hour,
         tm.tm_min,
         tm.tm_sec);
}

static int pctime_set(const char *date, const char *clockstr)
{
  int year;
  int month;
  int day;
  int hour;
  int minute;
  int second;

  if (sscanf(date, "%d-%d-%d", &year, &month, &day) != 3)
    {
      return -1;
    }

  if (sscanf(clockstr, "%d:%d:%d", &hour, &minute, &second) != 3)
    {
      return -1;
    }

  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year = year - 1900;
  tm.tm_mon = month - 1;
  tm.tm_mday = day;
  tm.tm_hour = hour;
  tm.tm_min = minute;
  tm.tm_sec = second;
  tm.tm_isdst = -1;

  time_t t = mktime(&tm);
  if (t == (time_t)-1)
    {
      return -1;
    }

  struct timespec ts;
  ts.tv_sec = t;
  ts.tv_nsec = 0;

  (void)clock_settime(CLOCK_REALTIME, &ts);
  (void)rp23xx_aon_walltime_settime(&ts);

  return 0;
}

int main(int argc, FAR char *argv[])
{
  if (argc == 1)
    {
      pctime_print_now();
      return EXIT_SUCCESS;
    }

  if (argc == 4 && strcmp(argv[1], "set") == 0)
    {
      if (pctime_set(argv[2], argv[3]) < 0)
        {
          fprintf(stderr, "time: invalid date/time format\n");
          pctime_usage();
          return EXIT_FAILURE;
        }

      pctime_print_now();
      return EXIT_SUCCESS;
    }

  pctime_usage();
  return EXIT_FAILURE;
}
