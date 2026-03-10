/****************************************************************************
 * apps/system/pcssh/pcminipkg_main.c
 *
 * minipkg — Simple package manager CLI for PicoCalc-Term.
 *
 * Uses the pcpkg package infrastructure to install, remove, list,
 * search, and update third-party apps from SD card or network.
 *
 * Usage:
 *   minipkg list                  List installed packages
 *   minipkg info <name>           Show package details
 *   minipkg install <file.pcpkg>  Install package from SD card
 *   minipkg remove <name>         Remove installed package
 *   minipkg search <query>        Search catalog for packages
 *   minipkg update                Check for updates
 *   minipkg upgrade <name>        Download and upgrade a package
 *   minipkg scan                  Scan SD card for sideloaded packages
 *   minipkg catalog               Show cached catalog
 *   minipkg refresh               Refresh catalog from server
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>

#include "pcterm/package.h"

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void minipkg_usage(void)
{
  fprintf(stderr,
    "minipkg — PicoCalc package manager\n"
    "\n"
    "Usage:\n"
    "  minipkg list                  List installed packages\n"
    "  minipkg info <name>           Show package details\n"
    "  minipkg install <file.pcpkg>  Install from SD card\n"
    "  minipkg remove <name>         Remove package\n"
    "  minipkg search <query>        Search catalog\n"
    "  minipkg update                Check for updates\n"
    "  minipkg upgrade <name>        Download and upgrade\n"
    "  minipkg scan                  Scan SD for sideloaded .pcpkg\n"
    "  minipkg catalog               Show cached catalog\n"
    "  minipkg refresh               Refresh catalog from server\n");
}

/****************************************************************************
 * Name: cmd_list
 *
 * Description:
 *   List all installed packages with version and category.
 *
 ****************************************************************************/

static int cmd_list(void)
{
  int count = pcpkg_count();

  if (count <= 0)
    {
      printf("No packages installed.\n");
      return EXIT_SUCCESS;
    }

  printf("Installed packages (%d):\n", count);
  printf("  %-20s %-10s %-10s %s\n",
         "NAME", "VERSION", "SIZE", "PATH");
  printf("  %-20s %-10s %-10s %s\n",
         "----", "-------", "----", "----");

  pcpkg_entry_t entries[32];
  int n = pcpkg_list(entries, 32);

  if (n <= 0)
    {
      printf("  (none)\n");
      return EXIT_SUCCESS;
    }

  for (int i = 0; i < n; i++)
    {
      printf("  %-20s %-10s %-10u %s\n",
             entries[i].name,
             entries[i].version,
             entries[i].installed_size,
             entries[i].install_path);
    }

  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: cmd_info
 *
 * Description:
 *   Show detailed info for a specific installed package.
 *
 ****************************************************************************/

static int cmd_info(const char *name)
{
  pcpkg_manifest_t manifest;

  int ret = pcpkg_get_manifest(name, &manifest);
  if (ret < 0)
    {
      fprintf(stderr, "minipkg: package '%s' not found\n", name);
      return EXIT_FAILURE;
    }

  printf("Package: %s\n", manifest.name);
  printf("Version: %s\n", manifest.version);
  printf("Author:  %s\n", manifest.author);
  printf("Desc:    %s\n", manifest.description);
  printf("Category: %s\n", manifest.category);
  printf("Min RAM:  %u bytes\n", (unsigned)manifest.min_ram);
  printf("Network:  %s\n",
         manifest.requires_network ? "required" : "no");
  printf("Path:    /mnt/sd/apps/%s/\n", name);

  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: cmd_install
 *
 * Description:
 *   Install a .pcpkg file from SD card path.
 *
 ****************************************************************************/

static int cmd_install(const char *path)
{
  printf("Installing %s ...\n", path);

  int ret = pcpkg_install(path);
  if (ret < 0)
    {
      fprintf(stderr, "minipkg: install failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  printf("Package installed successfully.\n");
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: cmd_remove
 *
 * Description:
 *   Uninstall a package by name.
 *
 ****************************************************************************/

static int cmd_remove(const char *name)
{
  pcpkg_manifest_t manifest;

  /* Verify it exists first */

  if (pcpkg_get_manifest(name, &manifest) < 0)
    {
      fprintf(stderr, "minipkg: package '%s' not installed\n", name);
      return EXIT_FAILURE;
    }

  printf("Removing %s v%s ...\n", manifest.name, manifest.version);

  int ret = pcpkg_uninstall(name);
  if (ret < 0)
    {
      fprintf(stderr, "minipkg: removal failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  printf("Package '%s' removed.\n", name);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: cmd_search
 *
 * Description:
 *   Search the cached catalog for matching packages.
 *
 ****************************************************************************/

static int cmd_search(const char *query)
{
  pcpkg_catalog_t catalog;

  int ret = pcpkg_load_cached_catalog(&catalog);
  if (ret < 0)
    {
      fprintf(stderr,
              "minipkg: no catalog available (run 'minipkg refresh')\n");
      return EXIT_FAILURE;
    }

  printf("Search results for '%s':\n", query);

  int found = 0;

  for (int i = 0; i < catalog.count; i++)
    {
      pcpkg_catalog_entry_t *e = &catalog.entries[i];

      /* Case-insensitive substring match in name/description/category */

      if (strcasestr(e->name, query) != NULL ||
          strcasestr(e->description, query) != NULL ||
          strcasestr(e->category, query) != NULL)
        {
          printf("  %-20s %-10s %-12s %s\n",
                 e->name, e->version, e->category, e->description);
          found++;
        }
    }

  if (found == 0)
    {
      printf("  No matching packages found.\n");
    }
  else
    {
      printf("  %d package(s) found.\n", found);
    }

  free(catalog.entries);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: cmd_update
 *
 * Description:
 *   Check which installed packages have updates available.
 *
 ****************************************************************************/

static int cmd_update(void)
{
  pcpkg_catalog_t catalog;

  int ret = pcpkg_load_cached_catalog(&catalog);
  if (ret < 0)
    {
      fprintf(stderr,
              "minipkg: no catalog available (run 'minipkg refresh')\n");
      return EXIT_FAILURE;
    }

  pcpkg_entry_t installed[32];
  int n = pcpkg_list(installed, 32);
  if (n <= 0)
    {
      printf("No packages installed.\n");
      free(catalog.entries);
      return EXIT_SUCCESS;
    }

  int updates = 0;

  for (int i = 0; i < n; i++)
    {
      for (int j = 0; j < catalog.count; j++)
        {
          if (strcmp(installed[i].name, catalog.entries[j].name) == 0)
            {
              if (strcmp(installed[i].version,
                        catalog.entries[j].version) != 0)
                {
                  printf("  %-20s %s -> %s\n",
                         installed[i].name,
                         installed[i].version,
                         catalog.entries[j].version);
                  updates++;
                }
              break;
            }
        }
    }

  if (updates == 0)
    {
      printf("All packages are up to date.\n");
    }
  else
    {
      printf("%d update(s) available. Use 'minipkg upgrade <name>'.\n",
             updates);
    }

  free(catalog.entries);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: cmd_upgrade
 *
 * Description:
 *   Download and install the latest version of a package.
 *
 ****************************************************************************/

static int cmd_upgrade(const char *name)
{
  printf("Upgrading %s ...\n", name);

  /* Lookup download URL from cached catalog */

  pcpkg_catalog_t catalog;
  const char *url = NULL;

  if (pcpkg_load_cached_catalog(&catalog) == 0)
    {
      for (int i = 0; i < catalog.count; i++)
        {
          if (strcmp(catalog.entries[i].name, name) == 0)
            {
              url = catalog.entries[i].download_url;
              break;
            }
        }
    }

  if (url == NULL)
    {
      fprintf(stderr, "minipkg: package '%s' not found in catalog\n", name);
      free(catalog.entries);
      return EXIT_FAILURE;
    }

  int ret = pcpkg_download_and_install(url, name);
  free(catalog.entries);
  if (ret < 0)
    {
      fprintf(stderr, "minipkg: upgrade failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  printf("Package '%s' upgraded successfully.\n", name);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: cmd_scan
 *
 * Description:
 *   Scan SD card for sideloaded .pcpkg files and install them.
 *
 ****************************************************************************/

static int cmd_scan(void)
{
  printf("Scanning SD card for .pcpkg files ...\n");

  int ret = pcpkg_scan_and_install_sd();
  if (ret < 0)
    {
      fprintf(stderr, "minipkg: scan failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  if (ret == 0)
    {
      printf("No new packages found.\n");
    }
  else
    {
      printf("Installed %d package(s) from SD card.\n", ret);
    }

  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: cmd_catalog
 *
 * Description:
 *   Display the cached package catalog.
 *
 ****************************************************************************/

static int cmd_catalog(void)
{
  pcpkg_catalog_t catalog;

  int ret = pcpkg_load_cached_catalog(&catalog);
  if (ret < 0)
    {
      fprintf(stderr,
              "minipkg: no catalog cached (run 'minipkg refresh')\n");
      return EXIT_FAILURE;
    }

  printf("Package catalog (%d packages):\n", catalog.count);
  printf("  %-20s %-10s %-12s %s\n",
         "NAME", "VERSION", "CATEGORY", "DESCRIPTION");
  printf("  %-20s %-10s %-12s %s\n",
         "----", "-------", "--------", "-----------");

  for (int i = 0; i < catalog.count; i++)
    {
      printf("  %-20s %-10s %-12s %s\n",
             catalog.entries[i].name,
             catalog.entries[i].version,
             catalog.entries[i].category,
             catalog.entries[i].description);
    }

  free(catalog.entries);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Name: cmd_refresh
 *
 * Description:
 *   Fetch the latest catalog from the server and cache it.
 *
 ****************************************************************************/

static int cmd_refresh(void)
{
  printf("Refreshing catalog ...\n");

  pcpkg_catalog_t catalog;

  int ret = pcpkg_fetch_catalog(NULL, &catalog);
  if (ret < 0)
    {
      fprintf(stderr,
              "minipkg: failed to fetch catalog (%d)\n"
              "  Check network connection.\n", ret);
      return EXIT_FAILURE;
    }

  printf("Catalog refreshed: %d packages available.\n", catalog.count);
  free(catalog.entries);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  if (argc < 2)
    {
      minipkg_usage();
      return EXIT_FAILURE;
    }

  /* Ensure package manager is initialized */

  pcpkg_init();

  const char *cmd = argv[1];

  if (strcmp(cmd, "list") == 0 || strcmp(cmd, "ls") == 0)
    {
      return cmd_list();
    }

  if (strcmp(cmd, "info") == 0)
    {
      if (argc < 3)
        {
          fprintf(stderr, "minipkg info: package name required\n");
          return EXIT_FAILURE;
        }

      return cmd_info(argv[2]);
    }

  if (strcmp(cmd, "install") == 0 || strcmp(cmd, "i") == 0)
    {
      if (argc < 3)
        {
          fprintf(stderr, "minipkg install: .pcpkg file path required\n");
          return EXIT_FAILURE;
        }

      return cmd_install(argv[2]);
    }

  if (strcmp(cmd, "remove") == 0 || strcmp(cmd, "rm") == 0 ||
      strcmp(cmd, "uninstall") == 0)
    {
      if (argc < 3)
        {
          fprintf(stderr, "minipkg remove: package name required\n");
          return EXIT_FAILURE;
        }

      return cmd_remove(argv[2]);
    }

  if (strcmp(cmd, "search") == 0 || strcmp(cmd, "s") == 0)
    {
      if (argc < 3)
        {
          fprintf(stderr, "minipkg search: query required\n");
          return EXIT_FAILURE;
        }

      return cmd_search(argv[2]);
    }

  if (strcmp(cmd, "update") == 0)
    {
      return cmd_update();
    }

  if (strcmp(cmd, "upgrade") == 0)
    {
      if (argc < 3)
        {
          fprintf(stderr, "minipkg upgrade: package name required\n");
          return EXIT_FAILURE;
        }

      return cmd_upgrade(argv[2]);
    }

  if (strcmp(cmd, "scan") == 0)
    {
      return cmd_scan();
    }

  if (strcmp(cmd, "catalog") == 0)
    {
      return cmd_catalog();
    }

  if (strcmp(cmd, "refresh") == 0)
    {
      return cmd_refresh();
    }

  if (strcmp(cmd, "-h") == 0 || strcmp(cmd, "--help") == 0 ||
      strcmp(cmd, "help") == 0)
    {
      minipkg_usage();
      return EXIT_SUCCESS;
    }

  fprintf(stderr, "minipkg: unknown command '%s'\n", cmd);
  minipkg_usage();
  return EXIT_FAILURE;
}
