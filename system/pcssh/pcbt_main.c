/****************************************************************************
 * apps/system/pcssh/pcbt_main.c
 *
 * Bluetooth CLI command for PicoCalc-Term.
 *
 * Usage:
 *   bt status              Show Bluetooth status
 *   bt on                  Enable Bluetooth
 *   bt off                 Disable Bluetooth
 *   bt scan                Scan for nearby devices
 *   bt pair <addr>         Pair with a device
 *   bt unpair <addr>       Unpair a device
 *   bt devices             List paired devices
 *
 * Note: Bluetooth support requires the CYW43 driver to be fully
 * implemented. Currently stub functions are provided.
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/****************************************************************************
 * External References — Bluetooth driver stubs
 ****************************************************************************/

typedef struct bt_scan_result_s
{
  char name[32];
  char addr[18];   /* "XX:XX:XX:XX:XX:XX" */
  int  rssi;
  int  type;       /* 0=classic, 1=BLE */
} bt_scan_result_t;

/* These will be implemented when CYW43 BT driver is ready */

__attribute__((weak)) int rp23xx_bt_enable(void)
{
  return -1;  /* Not yet implemented */
}

__attribute__((weak)) int rp23xx_bt_disable(void)
{
  return -1;
}

__attribute__((weak)) int rp23xx_bt_status(void)
{
  return -1;  /* -1=unavailable, 0=off, 1=on */
}

__attribute__((weak)) int rp23xx_bt_scan(bt_scan_result_t *results,
                                          int max_results)
{
  (void)results;
  (void)max_results;
  return -1;
}

__attribute__((weak)) int rp23xx_bt_pair(const char *addr)
{
  (void)addr;
  return -1;
}

__attribute__((weak)) int rp23xx_bt_unpair(const char *addr)
{
  (void)addr;
  return -1;
}

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void bt_usage(void)
{
  fprintf(stderr,
    "bt — PicoCalc Bluetooth manager\n"
    "\n"
    "Usage:\n"
    "  bt status        Show Bluetooth status\n"
    "  bt on            Enable Bluetooth\n"
    "  bt off           Disable Bluetooth\n"
    "  bt scan          Scan for devices\n"
    "  bt pair <addr>   Pair with device\n"
    "  bt unpair <addr> Unpair device\n"
    "  bt devices       List paired devices\n"
    "\n"
    "Note: Bluetooth requires CYW43 BT driver (pending)\n");
}

static int cmd_status(void)
{
  int st = rp23xx_bt_status();

  switch (st)
    {
      case 1:
        printf("Bluetooth: enabled\n");
        break;
      case 0:
        printf("Bluetooth: disabled\n");
        break;
      default:
        printf("Bluetooth: not available (driver pending)\n");
        break;
    }

  return EXIT_SUCCESS;
}

static int cmd_enable(void)
{
  int ret = rp23xx_bt_enable();
  if (ret < 0)
    {
      fprintf(stderr, "bt: enable failed (driver not available)\n");
      return EXIT_FAILURE;
    }

  printf("Bluetooth enabled.\n");
  return EXIT_SUCCESS;
}

static int cmd_disable(void)
{
  int ret = rp23xx_bt_disable();
  if (ret < 0)
    {
      fprintf(stderr, "bt: disable failed\n");
      return EXIT_FAILURE;
    }

  printf("Bluetooth disabled.\n");
  return EXIT_SUCCESS;
}

static int cmd_scan(void)
{
  bt_scan_result_t results[16];

  printf("Scanning for Bluetooth devices...\n");

  int count = rp23xx_bt_scan(results, 16);
  if (count < 0)
    {
      fprintf(stderr, "bt: scan failed (driver not available)\n");
      return EXIT_FAILURE;
    }

  if (count == 0)
    {
      printf("No devices found.\n");
      return EXIT_SUCCESS;
    }

  printf("%-20s  %-18s  %6s  %s\n", "NAME", "ADDRESS", "RSSI", "TYPE");
  printf("%-20s  %-18s  %6s  %s\n", "----", "-------", "----", "----");

  for (int i = 0; i < count; i++)
    {
      printf("%-20s  %-18s  %4ddBm  %s\n",
             results[i].name[0] ? results[i].name : "(unnamed)",
             results[i].addr,
             results[i].rssi,
             results[i].type == 1 ? "BLE" : "Classic");
    }

  printf("\n%d device(s) found.\n", count);
  return EXIT_SUCCESS;
}

static int cmd_pair(const char *addr)
{
  printf("Pairing with %s...\n", addr);

  int ret = rp23xx_bt_pair(addr);
  if (ret < 0)
    {
      fprintf(stderr, "bt: pairing failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  printf("Paired with %s.\n", addr);
  return EXIT_SUCCESS;
}

static int cmd_unpair(const char *addr)
{
  int ret = rp23xx_bt_unpair(addr);
  if (ret < 0)
    {
      fprintf(stderr, "bt: unpair failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  printf("Unpaired %s.\n", addr);
  return EXIT_SUCCESS;
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  if (argc < 2)
    {
      return cmd_status();
    }

  if (strcmp(argv[1], "status") == 0)
    {
      return cmd_status();
    }

  if (strcmp(argv[1], "on") == 0 || strcmp(argv[1], "enable") == 0)
    {
      return cmd_enable();
    }

  if (strcmp(argv[1], "off") == 0 || strcmp(argv[1], "disable") == 0)
    {
      return cmd_disable();
    }

  if (strcmp(argv[1], "scan") == 0)
    {
      return cmd_scan();
    }

  if (strcmp(argv[1], "pair") == 0)
    {
      if (argc < 3)
        {
          fprintf(stderr, "bt pair: device address required\n");
          return EXIT_FAILURE;
        }

      return cmd_pair(argv[2]);
    }

  if (strcmp(argv[1], "unpair") == 0)
    {
      if (argc < 3)
        {
          fprintf(stderr, "bt unpair: device address required\n");
          return EXIT_FAILURE;
        }

      return cmd_unpair(argv[2]);
    }

  if (strcmp(argv[1], "devices") == 0)
    {
      printf("Paired devices: (not yet implemented)\n");
      return EXIT_SUCCESS;
    }

  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
      bt_usage();
      return EXIT_SUCCESS;
    }

  fprintf(stderr, "bt: unknown command '%s'\n", argv[1]);
  bt_usage();
  return EXIT_FAILURE;
}
