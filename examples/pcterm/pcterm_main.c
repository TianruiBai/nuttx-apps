/****************************************************************************
 * apps/examples/pcterm/pcterm_main.c
 *
 * PicoCalc-Term OS entry point — full bootstrap with splash animation.
 *
 * Boot sequence:
 *   1. Mount SD card
 *   2. Load hostname
 *   3. Load settings
 *   4. Initialize LVGL (display + keyboard drivers)
 *   5. Show boot splash animation
 *   6. Create status bar
 *   7. Initialize app framework (register built-in apps)
 *   8. Initialize package manager (scan SD for third-party apps)
 *   9. Hide splash, launch home screen (launcher)
 *  10. Enter main event loop
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <syslog.h>

#include <nuttx/clock.h>
#include <nuttx/sched.h>
#include <lvgl/lvgl.h>
#include <arch/board/board.h>

#include "pcterm/app.h"
#include "pcterm/config.h"
#include "pcterm/hostname.h"
#include "pcterm/statusbar.h"
#include "pcterm/launcher.h"
#include "pcterm/package.h"
#include "pcterm/boot_splash.h"

/****************************************************************************
 * Pre-processor Definitions
 ****************************************************************************/

#define LVGL_TICK_MS         5      /* LVGL tick period */
#define STATUSBAR_UPDATE_MS  1000   /* Status bar refresh interval */
#define MAIN_LOOP_SLEEP_MS   5      /* Main loop sleep period */
#define SPLASH_STEP_DELAY_US 30000  /* 30 ms per splash animation step */

/****************************************************************************
 * Private Data
 ****************************************************************************/

static pc_config_t g_config;
static bool g_running = true;
static bool g_serial_nsh_started = false;

/****************************************************************************
 * Private Functions
 ****************************************************************************/

/****************************************************************************
 * Name: boot_mount_sd
 ****************************************************************************/

static int boot_mount_sd(void)
{
  extern int rp23xx_sdcard_mount(void);
  int ret = rp23xx_sdcard_mount();

  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "BOOT: SD card not available — running in limited mode\n");
    }

  return ret;
}

/****************************************************************************
 * Name: boot_init_lvgl
 ****************************************************************************/

extern int  lv_port_disp_init(void);
extern void lv_port_indev_init(void);

/****************************************************************************
 * Name: lv_nuttx_tick_cb
 *
 * Description:
 *   LVGL tick callback — returns elapsed milliseconds since boot.
 *   LVGL v9 requires a tick source via lv_tick_set_cb() so that
 *   its internal timer system can advance.  Without this callback,
 *   lv_timer_handler() never processes any timers and the display
 *   flush callback is never invoked after the first frame.
 *
 ****************************************************************************/

static uint32_t lv_nuttx_tick_cb(void)
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (uint32_t)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

static int boot_init_lvgl(void)
{
  int ret;

  lv_init();

  /* Register tick source — CRITICAL for LVGL v9.
   * Without this, LVGL timers never fire and the display
   * never refreshes after the initial frame.
   */

  lv_tick_set_cb(lv_nuttx_tick_cb);

  ret = lv_port_disp_init();
  if (ret < 0)
    {
      syslog(LOG_ERR, "BOOT: Display init failed (%d) — no GUI\n", ret);
      return ret;
    }

  lv_port_indev_init();

  syslog(LOG_INFO, "BOOT: LVGL initialized (display + keyboard)\n");
  return 0;
}

/****************************************************************************
 * Name: splash_pump
 *
 * Description:
 *   Set splash progress/status and call lv_timer_handler so the display
 *   is actually updated.
 *
 ****************************************************************************/

static void splash_pump(int pct, const char *msg)
{
  boot_splash_set_progress(pct);
  boot_splash_set_status(msg);
  lv_timer_handler();
  usleep(SPLASH_STEP_DELAY_US);
}

/****************************************************************************
 * Name: status_bar_timer_cb
 ****************************************************************************/

static void status_bar_timer_cb(lv_timer_t *timer)
{
  (void)timer;
  statusbar_state_t state;

  strncpy(state.hostname, hostname_get(), sizeof(state.hostname));
  state.wifi_connected  = false;
  state.wifi_rssi       = -80;
  state.wifi_text[0]    = '\0';
  state.audio_playing   = false;
  state.battery_percent = 0;
  state.battery_charging = false;
  state.battery_style   = pc_config_get()->battery_style;

  /* Battery state from south bridge */

  uint8_t battery_percent = 0;
  bool battery_charging = false;
  if (rp23xx_sb_get_battery(&battery_percent, &battery_charging) == 0)
    {
      state.battery_percent = battery_percent;
      state.battery_charging = battery_charging;
    }

  /* Wireless state (driver/stub API) */

  extern int rp23xx_wifi_status(void);
  int wifi_status = rp23xx_wifi_status();
  if (wifi_status > 0)
    {
      state.wifi_connected = true;
      state.wifi_rssi = -55;
      strncpy(state.wifi_text, LV_SYMBOL_WIFI " ON",
              sizeof(state.wifi_text) - 1);
      state.wifi_text[sizeof(state.wifi_text) - 1] = '\0';
    }
  else
    {
      state.wifi_connected = false;
      strncpy(state.wifi_text, LV_SYMBOL_CLOSE " OFF",
              sizeof(state.wifi_text) - 1);
      state.wifi_text[sizeof(state.wifi_text) - 1] = '\0';
    }

  struct timespec ts;
  if (rp23xx_aon_walltime_gettime(&ts) < 0)
    {
      clock_gettime(CLOCK_REALTIME, &ts);
    }

  struct tm tm;
  localtime_r(&ts.tv_sec, &tm);
  state.year   = tm.tm_year + 1900;
  state.month  = tm.tm_mon + 1;
  state.day    = tm.tm_mday;
  state.hour   = tm.tm_hour;
  state.minute = tm.tm_min;

  statusbar_update(&state);

  /* Tick backlight timeout / sleep timer */

  rp23xx_backlight_timer_tick();
}

/****************************************************************************
 * Name: boot_start_serial_nsh
 *
 * Description:
 *   Start NuttShell on system serial console so users can immediately
 *   interact over UART after boot.
 *
 ****************************************************************************/

static void boot_start_serial_nsh(void)
{
#ifdef CONFIG_NSH_LIBRARY
  if (g_serial_nsh_started)
    {
      return;
    }

  extern int nsh_main(int argc, char *argv[]);

  int pid = task_create("nsh", 100, 4096, (main_t)nsh_main, NULL);
  if (pid < 0)
    {
      syslog(LOG_WARNING, "BOOT: Failed to start serial NSH\n");
      return;
    }

  g_serial_nsh_started = true;
  syslog(LOG_INFO, "BOOT: Serial NSH started (pid=%d)\n", pid);
#endif
}

/****************************************************************************
 * Public Functions
 ****************************************************************************/

/****************************************************************************
 * Name: main  (NuttX build system renames this to pcterm_main)
 ****************************************************************************/

int main(int argc, FAR char *argv[])
{
  int ret;

  printf("\n");
  printf("========================================\n");
  printf("     PicoCalc-Term  v0.1.0\n");
  printf("     NuttX / LVGL / RP2350B\n");
  printf("========================================\n");
  printf("\n");

  /* --- Step 1: Mount SD card --- */

  syslog(LOG_INFO, "BOOT: Step 1 — Mount SD card\n");
  boot_mount_sd();

  /* --- Step 2: Load hostname --- */

  syslog(LOG_INFO, "BOOT: Step 2 — Load hostname\n");
  hostname_init();
  syslog(LOG_INFO, "BOOT: Hostname = \"%s\"\n", hostname_get());

  /* --- Step 3: Load settings --- */

  syslog(LOG_INFO, "BOOT: Step 3 — Load settings\n");
  ret = pc_config_load(&g_config);
  if (ret < 0)
    {
      syslog(LOG_WARNING, "BOOT: Using default settings\n");
      pc_config_defaults(&g_config);
    }

  /* --- Step 4: Initialize LVGL --- */

  syslog(LOG_INFO, "BOOT: Step 4 — Initialize LVGL\n");
  ret = boot_init_lvgl();
  if (ret < 0)
    {
      syslog(LOG_EMERG, "BOOT: No display — halting.\n");
      for (;;) usleep(1000000);
    }

  /* --- Step 5: Show boot splash --- */

  syslog(LOG_INFO, "BOOT: Step 5 — Boot splash\n");
  lv_obj_t *screen = lv_scr_act();
  boot_splash_show(screen);
  splash_pump(5, "Initializing...");

  /* --- Step 6: Create status bar (behind the splash) --- */

  syslog(LOG_INFO, "BOOT: Step 6 — Create status bar\n");
  statusbar_set_position(g_config.statusbar_position);
  statusbar_init(screen);
  lv_timer_create(status_bar_timer_cb, STATUSBAR_UPDATE_MS, NULL);
  splash_pump(20, "Status bar ready");

  /* --- Step 7: Initialize app framework --- */

  syslog(LOG_INFO, "BOOT: Step 7 — Initialize app framework\n");
  splash_pump(30, "Loading apps...");
  app_framework_init();
  splash_pump(50, "Apps registered");

  /* --- Step 8: Initialize package manager --- */

  syslog(LOG_INFO, "BOOT: Step 8 — Initialize package manager\n");
  splash_pump(60, "Scanning packages...");
  pcpkg_init();
  splash_pump(75, "Packages loaded");

  /* --- Step 9: Launch home screen --- */

  syslog(LOG_INFO, "BOOT: Step 9 — Launch launcher\n");
  splash_pump(85, "Preparing launcher...");

  lv_obj_t *app_area = statusbar_get_app_area();
  launcher_set_arrange_mode(1);  /* alphabetical arrangement */
  launcher_init(app_area);
  splash_pump(95, "Almost ready...");

  /* Final splash animation ramp */

  splash_pump(100, "Welcome!");
  usleep(300000);  /* Show "Welcome!" for 300 ms */

  /* Remove splash — launcher is now visible */

  boot_splash_hide();

  /* Force full-screen redraw so the launcher appears immediately.
   * The splash covered the entire screen; after deletion we must
   * invalidate the whole display so LVGL repaints the status bar
   * and launcher grid on the next lv_timer_handler() call.
   */

  lv_obj_invalidate(lv_scr_act());
  lv_timer_handler();

  /* Start serial shell for UART access */

  boot_start_serial_nsh();

  syslog(LOG_INFO, "BOOT: PicoCalc-Term ready!\n");

  /* --- Step 10: Main event loop --- */

  while (g_running)
    {
      uint32_t time_till_next = lv_timer_handler();

      /* Check for deferred app launch from the launcher */

      const char *pending = launcher_get_pending_launch();
      if (pending != NULL)
        {
          app_framework_launch(pending);
          launcher_clear_pending_launch();
        }

      uint32_t sleep_ms = time_till_next < MAIN_LOOP_SLEEP_MS
                          ? time_till_next : MAIN_LOOP_SLEEP_MS;
      usleep(sleep_ms * 1000);
    }

  syslog(LOG_INFO, "PicoCalc-Term shutting down\n");
  return 0;
}
