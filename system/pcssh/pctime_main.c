/****************************************************************************
 * apps/system/pcssh/pctime_main.c
 *
 * Full-featured time command for PicoCalc-Term.
 * Similar to Unix date/time utilities.
 *
 * Usage:
 *   time                        Print current date/time (like `date`)
 *   time -v                     Verbose: date, TZ, epoch, uptime
 *   time set YYYY-MM-DD HH:MM:SS   Set date and time
 *   time epoch                  Print Unix epoch seconds
 *   time uptime                 Print system uptime
 *   time tz                     Print current timezone
 *   time tz SET <value>         Set timezone (e.g. "EST5EDT")
 *   time date                   Print date only (YYYY-MM-DD)
 *   time clock                  Print time only (HH:MM:SS)
 *   time iso                    Print ISO 8601 format
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <errno.h>

#include <arch/board/board.h>

/****************************************************************************
 * Private Data
 ****************************************************************************/

static const char * const g_weekday[] =
{
  "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char * const g_month[] =
{
  "Jan", "Feb", "Mar", "Apr", "May", "Jun",
  "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void pctime_usage(void)
{
  fprintf(stderr,
          "Usage:\n"
          "  time                           Show current date/time\n"
          "  time -v                        Verbose (date, TZ, epoch, uptime)\n"
          "  time set YYYY-MM-DD HH:MM:SS  Set date and time\n"
          "  time epoch                     Unix epoch seconds\n"
          "  time uptime                    System uptime\n"
          "  time tz                        Show timezone\n"
          "  time tz set <value>            Set timezone\n"
          "  time date                      Date only (YYYY-MM-DD)\n"
          "  time clock                     Time only (HH:MM:SS)\n"
          "  time iso                       ISO 8601 format\n");
}

static void pctime_gettime(struct timespec *ts)
{
  if (rp23xx_aon_walltime_gettime(ts) < 0)
    {
      clock_gettime(CLOCK_REALTIME, ts);
    }
}

static void pctime_print_default(void)
{
  struct timespec ts;
  struct tm tm;

  pctime_gettime(&ts);
  localtime_r(&ts.tv_sec, &tm);

  /* Unix-style: "Wed Jun 18 14:30:45 UTC 2025" */

  const char *tz = getenv("TZ");
  if (tz == NULL) tz = "UTC";

  printf("%s %s %2d %02d:%02d:%02d %s %04d\n",
         g_weekday[tm.tm_wday],
         g_month[tm.tm_mon],
         tm.tm_mday,
         tm.tm_hour,
         tm.tm_min,
         tm.tm_sec,
         tz,
         tm.tm_year + 1900);
}

static void pctime_print_verbose(void)
{
  struct timespec ts;
  struct timespec mono;
  struct tm tm;

  pctime_gettime(&ts);
  clock_gettime(CLOCK_MONOTONIC, &mono);
  localtime_r(&ts.tv_sec, &tm);

  const char *tz = getenv("TZ");
  if (tz == NULL) tz = "UTC";

  printf("Date:     %04d-%02d-%02d\n",
         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
  printf("Time:     %02d:%02d:%02d\n",
         tm.tm_hour, tm.tm_min, tm.tm_sec);
  printf("Day:      %s\n", g_weekday[tm.tm_wday]);
  printf("Timezone: %s\n", tz);
  printf("Epoch:    %ld\n", (long)ts.tv_sec);

  unsigned long up = (unsigned long)mono.tv_sec;
  unsigned int days = up / 86400;
  unsigned int hours = (up % 86400) / 3600;
  unsigned int mins = (up % 3600) / 60;
  unsigned int secs = up % 60;

  if (days > 0)
    {
      printf("Uptime:   %ud %uh %um %us\n", days, hours, mins, secs);
    }
  else if (hours > 0)
    {
      printf("Uptime:   %uh %um %us\n", hours, mins, secs);
    }
  else
    {
      printf("Uptime:   %um %us\n", mins, secs);
    }
}

static int pctime_set(const char *date_str, const char *time_str)
{
  int year, month, day, hour, minute, second;

  if (sscanf(date_str, "%d-%d-%d", &year, &month, &day) != 3)
    {
      return -1;
    }

  if (sscanf(time_str, "%d:%d:%d", &hour, &minute, &second) != 3)
    {
      return -1;
    }

  struct tm tm;
  memset(&tm, 0, sizeof(tm));
  tm.tm_year  = year - 1900;
  tm.tm_mon   = month - 1;
  tm.tm_mday  = day;
  tm.tm_hour  = hour;
  tm.tm_min   = minute;
  tm.tm_sec   = second;
  tm.tm_isdst = -1;

  time_t t = mktime(&tm);
  if (t == (time_t)-1)
    {
      return -1;
    }

  struct timespec ts;
  ts.tv_sec  = t;
  ts.tv_nsec = 0;

  (void)clock_settime(CLOCK_REALTIME, &ts);
  (void)rp23xx_aon_walltime_settime(&ts);

  return 0;
}

static void pctime_print_epoch(void)
{
  struct timespec ts;
  pctime_gettime(&ts);
  printf("%ld\n", (long)ts.tv_sec);
}

static void pctime_print_uptime(void)
{
  struct timespec mono;
  clock_gettime(CLOCK_MONOTONIC, &mono);

  unsigned long up = (unsigned long)mono.tv_sec;
  unsigned int days  = up / 86400;
  unsigned int hours = (up % 86400) / 3600;
  unsigned int mins  = (up % 3600) / 60;
  unsigned int secs  = up % 60;

  printf("up ");
  if (days > 0)
    {
      printf("%u day%s, ", days, days > 1 ? "s" : "");
    }

  printf("%02u:%02u:%02u\n", hours, mins, secs);
}

static void pctime_print_tz(void)
{
  const char *tz = getenv("TZ");
  printf("%s\n", tz ? tz : "UTC (default)");
}

static void pctime_set_tz(const char *value)
{
  if (setenv("TZ", value, 1) < 0)
    {
      fprintf(stderr, "time: failed to set TZ: %s\n", strerror(errno));
    }
  else
    {
      tzset();
      printf("TZ=%s\n", value);
    }
}

static void pctime_print_date(void)
{
  struct timespec ts;
  struct tm tm;
  pctime_gettime(&ts);
  localtime_r(&ts.tv_sec, &tm);
  printf("%04d-%02d-%02d\n",
         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
}

static void pctime_print_clock(void)
{
  struct timespec ts;
  struct tm tm;
  pctime_gettime(&ts);
  localtime_r(&ts.tv_sec, &tm);
  printf("%02d:%02d:%02d\n", tm.tm_hour, tm.tm_min, tm.tm_sec);
}

static void pctime_print_iso(void)
{
  struct timespec ts;
  struct tm tm;
  pctime_gettime(&ts);
  localtime_r(&ts.tv_sec, &tm);
  printf("%04d-%02d-%02dT%02d:%02d:%02d\n",
         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
         tm.tm_hour, tm.tm_min, tm.tm_sec);
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  if (argc == 1)
    {
      pctime_print_default();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "-v") == 0 || strcmp(argv[1], "--verbose") == 0)
    {
      pctime_print_verbose();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "set") == 0)
    {
      if (argc != 4)
        {
          fprintf(stderr, "time set: expected YYYY-MM-DD HH:MM:SS\n");
          return EXIT_FAILURE;
        }

      if (pctime_set(argv[2], argv[3]) < 0)
        {
          fprintf(stderr, "time: invalid date/time format\n");
          return EXIT_FAILURE;
        }

      pctime_print_default();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "epoch") == 0)
    {
      pctime_print_epoch();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "uptime") == 0)
    {
      pctime_print_uptime();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "tz") == 0)
    {
      if (argc >= 4 && strcmp(argv[2], "set") == 0)
        {
          pctime_set_tz(argv[3]);
          return EXIT_SUCCESS;
        }

      pctime_print_tz();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "date") == 0)
    {
      pctime_print_date();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "clock") == 0)
    {
      pctime_print_clock();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "iso") == 0)
    {
      pctime_print_iso();
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
      pctime_usage();
      return EXIT_SUCCESS;
    }

  fprintf(stderr, "time: unknown command '%s'\n", argv[1]);
  pctime_usage();
  return EXIT_FAILURE;
}
