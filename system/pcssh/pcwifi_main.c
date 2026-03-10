/****************************************************************************
 * apps/system/pcssh/pcwifi_main.c
 *
 * Wi-Fi CLI command for PicoCalc-Term.
 *
 * Usage:
 *   wifi status            Show connection status
 *   wifi scan              Scan for available networks
 *   wifi connect <ssid> [password]   Connect to a network
 *   wifi disconnect        Disconnect from current network
 *   wifi ip                Show IP address
 *   wifi saved             Show saved networks (from settings)
 *   wifi autoconnect [on|off]  Toggle auto-connect
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <arch/board/board.h>
#include "pcterm/config.h"

/****************************************************************************
 * External References
 ****************************************************************************/

typedef struct wifi_scan_result_s
{
  char ssid[33];
  int  rssi;
  int  auth;
} wifi_scan_result_t;

extern int  rp23xx_wifi_scan(wifi_scan_result_t *results, int max_results);
extern int  rp23xx_wifi_connect(const char *ssid, const char *passphrase);
extern int  rp23xx_wifi_disconnect(void);
extern int  rp23xx_wifi_status(void);
extern const char *rp23xx_wifi_ip(void);

/****************************************************************************
 * Private Functions
 ****************************************************************************/

static void wifi_usage(void)
{
  fprintf(stderr,
    "wifi — PicoCalc Wi-Fi manager\n"
    "\n"
    "Usage:\n"
    "  wifi status                  Show connection status\n"
    "  wifi scan                    Scan for networks\n"
    "  wifi connect <ssid> [pass]   Connect to network\n"
    "  wifi disconnect              Disconnect\n"
    "  wifi ip                      Show IP address\n"
    "  wifi saved                   Show saved network\n"
    "  wifi autoconnect [on|off]    Toggle auto-connect\n");
}

static const char *auth_type_str(int auth)
{
  switch (auth)
    {
      case 0: return "OPEN";
      case 1: return "WEP";
      case 2: return "WPA";
      case 3: return "WPA2";
      case 4: return "WPA3";
      default: return "???";
    }
}

static int rssi_bars(int rssi)
{
  if (rssi >= -50) return 4;
  if (rssi >= -60) return 3;
  if (rssi >= -70) return 2;
  if (rssi >= -80) return 1;
  return 0;
}

static int cmd_status(void)
{
  int st = rp23xx_wifi_status();

  if (st > 0)
    {
      printf("Wi-Fi: connected\n");
      printf("IP:    %s\n", rp23xx_wifi_ip());
    }
  else if (st == 0)
    {
      printf("Wi-Fi: disconnected\n");
    }
  else
    {
      printf("Wi-Fi: driver not available\n");
    }

  return EXIT_SUCCESS;
}

static int cmd_scan(void)
{
  wifi_scan_result_t results[16];

  printf("Scanning...\n");

  int count = rp23xx_wifi_scan(results, 16);
  if (count < 0)
    {
      fprintf(stderr, "wifi: scan failed (%d)\n", count);
      return EXIT_FAILURE;
    }

  if (count == 0)
    {
      printf("No networks found.\n");
      return EXIT_SUCCESS;
    }

  printf("%-32s  %6s  %4s  %s\n", "SSID", "SIGNAL", "BARS", "AUTH");
  printf("%-32s  %6s  %4s  %s\n", "----", "------", "----", "----");

  for (int i = 0; i < count; i++)
    {
      int bars = rssi_bars(results[i].rssi);
      char bar_str[8];
      int j;

      for (j = 0; j < 4; j++)
        {
          bar_str[j] = (j < bars) ? '#' : '.';
        }

      bar_str[4] = '\0';

      printf("%-32s  %4ddBm  %s  %s\n",
             results[i].ssid,
             results[i].rssi,
             bar_str,
             auth_type_str(results[i].auth));
    }

  printf("\n%d network(s) found.\n", count);
  return EXIT_SUCCESS;
}

static int cmd_connect(const char *ssid, const char *pass)
{
  printf("Connecting to '%s'...\n", ssid);

  int ret = rp23xx_wifi_connect(ssid, pass ? pass : "");
  if (ret < 0)
    {
      fprintf(stderr, "wifi: connection failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  printf("Connected to '%s'\n", ssid);
  printf("IP: %s\n", rp23xx_wifi_ip());

  /* Save to config */

  pc_config_t *cfg = pc_config_get();
  strncpy(cfg->wifi_ssid, ssid, sizeof(cfg->wifi_ssid) - 1);
  cfg->wifi_ssid[sizeof(cfg->wifi_ssid) - 1] = '\0';

  if (pass != NULL)
    {
      strncpy(cfg->wifi_pass, pass, sizeof(cfg->wifi_pass) - 1);
      cfg->wifi_pass[sizeof(cfg->wifi_pass) - 1] = '\0';
    }

  pc_config_save(cfg);
  return EXIT_SUCCESS;
}

static int cmd_disconnect(void)
{
  int ret = rp23xx_wifi_disconnect();
  if (ret < 0)
    {
      fprintf(stderr, "wifi: disconnect failed (%d)\n", ret);
      return EXIT_FAILURE;
    }

  printf("Disconnected.\n");
  return EXIT_SUCCESS;
}

static int cmd_ip(void)
{
  int st = rp23xx_wifi_status();
  if (st <= 0)
    {
      printf("Not connected.\n");
      return EXIT_FAILURE;
    }

  printf("%s\n", rp23xx_wifi_ip());
  return EXIT_SUCCESS;
}

static int cmd_saved(void)
{
  pc_config_t *cfg = pc_config_get();

  if (cfg->wifi_ssid[0] == '\0')
    {
      printf("No saved network.\n");
    }
  else
    {
      printf("SSID:        %s\n", cfg->wifi_ssid);
      printf("Password:    %s\n",
             cfg->wifi_pass[0] ? "(saved)" : "(none)");
      printf("Autoconnect: %s\n",
             cfg->wifi_autoconnect ? "on" : "off");
    }

  return EXIT_SUCCESS;
}

static int cmd_autoconnect(const char *arg)
{
  pc_config_t *cfg = pc_config_get();

  if (arg == NULL)
    {
      printf("autoconnect: %s\n",
             cfg->wifi_autoconnect ? "on" : "off");
      return EXIT_SUCCESS;
    }

  if (strcmp(arg, "on") == 0 || strcmp(arg, "1") == 0)
    {
      cfg->wifi_autoconnect = true;
    }
  else if (strcmp(arg, "off") == 0 || strcmp(arg, "0") == 0)
    {
      cfg->wifi_autoconnect = false;
    }
  else
    {
      fprintf(stderr, "wifi autoconnect: expected 'on' or 'off'\n");
      return EXIT_FAILURE;
    }

  pc_config_save(cfg);
  printf("autoconnect: %s\n",
         cfg->wifi_autoconnect ? "on" : "off");
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

  if (strcmp(argv[1], "scan") == 0)
    {
      return cmd_scan();
    }

  if (strcmp(argv[1], "connect") == 0)
    {
      if (argc < 3)
        {
          fprintf(stderr, "wifi connect: SSID required\n");
          return EXIT_FAILURE;
        }

      return cmd_connect(argv[2], argc >= 4 ? argv[3] : NULL);
    }

  if (strcmp(argv[1], "disconnect") == 0)
    {
      return cmd_disconnect();
    }

  if (strcmp(argv[1], "ip") == 0)
    {
      return cmd_ip();
    }

  if (strcmp(argv[1], "saved") == 0)
    {
      return cmd_saved();
    }

  if (strcmp(argv[1], "autoconnect") == 0)
    {
      return cmd_autoconnect(argc >= 3 ? argv[2] : NULL);
    }

  if (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)
    {
      wifi_usage();
      return EXIT_SUCCESS;
    }

  fprintf(stderr, "wifi: unknown command '%s'\n", argv[1]);
  wifi_usage();
  return EXIT_FAILURE;
}
