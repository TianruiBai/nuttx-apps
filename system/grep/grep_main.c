/****************************************************************************
 * apps/system/grep/grep_main.c
 *
 * Minimal grep utility for NuttX.
 *
 * Usage: grep [-incv] PATTERN [FILE...]
 *
 * Options:
 *   -i  Case-insensitive matching
 *   -n  Print line numbers
 *   -c  Print only a count of matching lines
 *   -v  Invert match (print non-matching lines)
 *
 * If no FILE is given, reads from standard input (pipe-friendly).
 * Supports multiple files; prefixes output with filename when > 1 file.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define GREP_LINE_MAX  1024

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: strcasestr_local
 *
 * Description:
 *   Case-insensitive substring search (not always available in libc).
 *
 ****************************************************************************/

static FAR const char *strcasestr_local(FAR const char *haystack,
                                        FAR const char *needle)
{
  size_t nlen = strlen(needle);

  if (nlen == 0)
    {
      return haystack;
    }

  for (; *haystack; haystack++)
    {
      if (strncasecmp(haystack, needle, nlen) == 0)
        {
          return haystack;
        }
    }

  return NULL;
}

/****************************************************************************
 * Name: grep_file
 *
 * Description:
 *   Search a single file (or stdin) for lines matching pattern.
 *   Returns the number of matching lines.
 *
 ****************************************************************************/

static int grep_file(FAR FILE *fp, FAR const char *pattern,
                     FAR const char *filename, bool show_name,
                     bool ignore_case, bool show_linenum,
                     bool count_only, bool invert)
{
  char line[GREP_LINE_MAX];
  int linenum = 0;
  int matches = 0;

  while (fgets(line, sizeof(line), fp) != NULL)
    {
      linenum++;

      /* Strip trailing newline for cleaner output */

      size_t len = strlen(line);
      if (len > 0 && line[len - 1] == '\n')
        {
          line[len - 1] = '\0';
          len--;
        }

      /* Check for match */

      FAR const char *found;
      if (ignore_case)
        {
          found = strcasestr_local(line, pattern);
        }
      else
        {
          found = strstr(line, pattern);
        }

      bool matched = (found != NULL);
      if (invert)
        {
          matched = !matched;
        }

      if (matched)
        {
          matches++;

          if (!count_only)
            {
              if (show_name)
                {
                  printf("%s:", filename);
                }

              if (show_linenum)
                {
                  printf("%d:", linenum);
                }

              printf("%s\n", line);
            }
        }
    }

  if (count_only)
    {
      if (show_name)
        {
          printf("%s:", filename);
        }

      printf("%d\n", matches);
    }

  return matches;
}

/****************************************************************************
 * Name: show_usage
 ****************************************************************************/

static void show_usage(void)
{
  fprintf(stderr, "Usage: grep [-incv] PATTERN [FILE...]\n");
  fprintf(stderr, "  -i  Case-insensitive matching\n");
  fprintf(stderr, "  -n  Print line numbers\n");
  fprintf(stderr, "  -c  Print only count of matching lines\n");
  fprintf(stderr, "  -v  Invert match (non-matching lines)\n");
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  bool ignore_case = false;
  bool show_linenum = false;
  bool count_only = false;
  bool invert = false;
  int opt;
  int total_matches = 0;

  while ((opt = getopt(argc, argv, "incv")) != -1)
    {
      switch (opt)
        {
          case 'i':
            ignore_case = true;
            break;
          case 'n':
            show_linenum = true;
            break;
          case 'c':
            count_only = true;
            break;
          case 'v':
            invert = true;
            break;
          default:
            show_usage();
            return EXIT_FAILURE;
        }
    }

  if (optind >= argc)
    {
      show_usage();
      return EXIT_FAILURE;
    }

  FAR const char *pattern = argv[optind++];
  int nfiles = argc - optind;

  if (nfiles == 0)
    {
      /* Read from stdin */

      total_matches = grep_file(stdin, pattern, "(stdin)",
                                false, ignore_case, show_linenum,
                                count_only, invert);
    }
  else
    {
      bool show_name = (nfiles > 1);

      for (int i = optind; i < argc; i++)
        {
          FAR FILE *fp = fopen(argv[i], "r");
          if (fp == NULL)
            {
              fprintf(stderr, "grep: %s: No such file or directory\n",
                      argv[i]);
              continue;
            }

          total_matches += grep_file(fp, pattern, argv[i],
                                     show_name, ignore_case,
                                     show_linenum, count_only,
                                     invert);
          fclose(fp);
        }
    }

  return (total_matches > 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
