/****************************************************************************
 * apps/examples/pcterm/pcterm_main.c
 *
 * PicoCalc-Term OS entry point — full bootstrap with splash animation.
 *
 * Boot sequence:
 *   1. Mount SD card (/mnt/sd) — optional, media only
 *   2. Load hostname (from /flash/etc/hostname)
 *   3. Load settings (from /flash/etc/settings.json)
 *   4. Initialize LVGL (display + keyboard drivers)
 *   5. Show boot splash animation
 *   6. Create status bar
 *   7. Initialize app framework (register built-in apps)
 *   8. Initialize package manager (scan SD for third-party apps)
 *   9. Hide splash, launch home screen (launcher)
 *  10. Enter main event loop
 *
 * Filesystem layout:
 *   /flash/          — internal SPI flash (LittleFS, always mounted)
 *   /flash/etc/      — system config: hostname, settings.json, passwd
 *   /flash/home/picocalc — user home directory
 *   /mnt/sd/         — SD card (FAT32, optional; music, video, packages)
 *
 ****************************************************************************/

#include <nuttx/config.h>

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <stdint.h>
#include <syslog.h>
#include <dirent.h>
#include <sys/stat.h>

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
#include "pcterm/login.h"
#include "pcterm/user.h"
#include "pcterm/vconsole.h"

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
 * Name: boot_play_startup_chime
 *
 * Description:
 *   Play a short startup chime on the PWM audio driver. This is best-effort
 *   and non-fatal: boot continues even if audio is unavailable.
 *
 ****************************************************************************/

extern int rp23xx_audio_write(const int16_t *samples, size_t count);
extern int rp23xx_audio_initialize(void);

static void boot_play_startup_chime(void)
{
  struct chime_note_s
  {
    uint16_t hz;
    uint16_t ms;
    uint16_t amp;
  };

  static const struct chime_note_s notes[] =
    {
      { 523,  90, 5200 },
      { 659,  90, 4700 },
      { 784, 170, 4200 }
    };

  const uint32_t sample_rate = BOARD_AUDIO_PWM_FREQ;
  int16_t pcm[256 * 2]; /* stereo interleaved */

  int init_ret = rp23xx_audio_initialize();
  if (init_ret < 0)
    {
      syslog(LOG_WARNING, "BOOT: startup chime init unavailable (%d)\n",
             init_ret);
      return;
    }

  for (unsigned int n = 0; n < sizeof(notes) / sizeof(notes[0]); n++)
    {
      uint32_t total = (sample_rate * notes[n].ms) / 1000;
      uint32_t sent = 0;
      uint32_t phase = 0;
      uint32_t step = (uint32_t)(((uint64_t)notes[n].hz << 32) / sample_rate);
      uint32_t ramp = total / 6;

      if (ramp < 1)
        {
          ramp = 1;
        }

      while (sent < total)
        {
          uint32_t frames = total - sent;
          if (frames > 256)
            {
              frames = 256;
            }

          for (uint32_t i = 0; i < frames; i++)
            {
              uint32_t pos = sent + i;
              uint32_t gain = 256;

              if (pos < ramp)
                {
                  gain = (pos * 256) / ramp;
                }
              else if (pos > total - ramp)
                {
                  gain = ((total - pos) * 256) / ramp;
                }

              int32_t sample = (phase & 0x80000000u)
                               ? (int32_t)notes[n].amp
                               : -(int32_t)notes[n].amp;

              sample = (sample * (int32_t)gain) / 256;
              pcm[i * 2] = (int16_t)sample;
              pcm[i * 2 + 1] = (int16_t)sample;

              phase += step;
            }

          size_t remain = frames * 2;
          size_t offset = 0;
          int retries = 0;

          while (remain > 0)
            {
              int wrote = rp23xx_audio_write(&pcm[offset], remain);
              if (wrote < 0)
                {
                  syslog(LOG_WARNING,
                         "BOOT: startup chime unavailable (%d)\n", wrote);
                  return;
                }

              if (wrote == 0)
                {
                  if (++retries > 50)
                    {
                      /* Buffer full and not draining — ISR may not be
                       * running.  Bail out instead of blocking forever.
                       */

                      syslog(LOG_WARNING,
                             "BOOT: chime buffer stall — skipping\n");
                      return;
                    }

                  usleep(2000);
                  continue;
                }

              retries = 0;
              offset += (size_t)wrote;
              remain -= (size_t)wrote;
            }

          sent += frames;
        }

      usleep(10000);
    }
}

/****************************************************************************
 * Name: boot_mount_sd
 *
 * Description:
 *   Mount the SD card at /mnt/sd (FAT32, optional).
 *   Used for media files only (music, video, large app packages).
 *   System config lives on internal flash (/flash/etc) and is unaffected
 *   by SD card presence.
 *
 ****************************************************************************/

static int boot_mount_sd(void)
{
  struct stat st;

  /* Check if bringup already mounted the SD card */

  if (stat("/mnt/sd", &st) == 0 && S_ISDIR(st.st_mode))
    {
      /* Try to stat a sentinel — if the mount is live, opendir will work.
       * Alternatively just try to list the directory.
       */

      DIR *d = opendir("/mnt/sd");
      if (d != NULL)
        {
          closedir(d);
          syslog(LOG_INFO, "BOOT: SD card already mounted at /mnt/sd\n");

          /* Ensure expected SD media directories exist */

          mkdir("/mnt/sd/music",  0777);
          mkdir("/mnt/sd/video",  0777);
          mkdir("/mnt/sd/apps",   0777);
          return 0;
        }
    }

  /* SD not yet mounted — try to mount it */

  extern int rp23xx_sdcard_mount(void);
  int ret = rp23xx_sdcard_mount();

  if (ret < 0)
    {
      syslog(LOG_WARNING,
             "BOOT: SD card not available — media features disabled\n");
    }
  else
    {
      /* Ensure expected SD media directories exist */

      mkdir("/mnt/sd/music",  0777);
      mkdir("/mnt/sd/video",  0777);
      mkdir("/mnt/sd/apps",   0777);
    }

  return ret;
}

/****************************************************************************
 * Name: boot_init_flash_dirs
 *
 * Description:
 *   Ensure the Linux-like directory tree on /flash is present.
 *   Directories are already created by bringup after mounting LittleFS,
 *   but we call this here as a safety net for paths config depends on.
 *   Also writes /flash/etc/passwd on first boot.
 *
 ****************************************************************************/

static void boot_init_flash_dirs(void)
{
  char path[80];
  const char *username;

  mkdir("/flash/etc",              0755);
  mkdir("/flash/etc/appstate",     0755);
  mkdir("/flash/etc/ssh",          0755);
  mkdir("/flash/home",             0755);

  /* Create home directory for the configured user (default: "picocalc") */

  username = user_get_name();
  snprintf(path, sizeof(path), "/flash/home/%s", username);
  mkdir(path, 0755);
  snprintf(path, sizeof(path), "/flash/home/%s/.config", username);
  mkdir(path, 0755);

  /* Write /flash/etc/passwd if it doesn't exist yet (first boot) */

  struct stat st;
  if (stat("/flash/etc/passwd", &st) < 0)
    {
      user_write_passwd();
    }
}

/****************************************************************************
 * Name: boot_init_lvgl
 ****************************************************************************/

extern int  lv_port_disp_init(void);
extern void lv_port_indev_init(void);
extern void lv_port_indev_keyboard_suspend(bool suspend);

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
  dprintf(STDERR_FILENO, "BOOT: splash %d%% — %s\n", pct, msg);
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
  bool gui_ready = false;

  /* Use write() to bypass stdio buffering — guaranteed to hit UART */

  static const char banner[] =
    "\n========================================\n"
    "     PicoCalc-Term  v0.1.0\n"
    "     NuttX / LVGL / RP2350B\n"
    "========================================\n\n";

  write(STDERR_FILENO, banner, sizeof(banner) - 1);

  syslog(LOG_INFO, "BOOT: Entered pcterm_main\n");
  dprintf(STDERR_FILENO, "BOOT: Entered pcterm_main\n");

  /* Start serial shell first so UART recovery is always available,
   * even if later GUI initialization fails.
   */

  boot_start_serial_nsh();
  dprintf(STDERR_FILENO, "BOOT: NSH start attempted\n");

  /* --- Step 0: Startup chime --- */

  syslog(LOG_INFO, "BOOT: Step 0 - Startup chime\n");
  boot_play_startup_chime();
  dprintf(STDERR_FILENO, "BOOT: Chime done\n");

  /* --- Step 1: Mount SD card (media only; non-fatal) --- */

  syslog(LOG_INFO, "BOOT: Step 1 — Mount SD card\n");
  boot_mount_sd();

  /* --- Step 1b: Ensure flash directory structure is ready --- */

  syslog(LOG_INFO, "BOOT: Step 1b — Init /flash directories\n");
  boot_init_flash_dirs();

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

  dprintf(STDERR_FILENO, "BOOT: Step 4 — LVGL init\n");
  syslog(LOG_INFO, "BOOT: Step 4 — Initialize LVGL\n");
  ret = boot_init_lvgl();
  if (ret < 0)
    {
      dprintf(STDERR_FILENO,
              "BOOT: Display init failed (%d) — serial only\n", ret);
      syslog(LOG_ERR, "BOOT: No display — switching to serial-only mode\n");
    }
  else
    {
      gui_ready = true;
      dprintf(STDERR_FILENO, "BOOT: LVGL ready\n");
    }

  if (!gui_ready)
    {
      dprintf(STDERR_FILENO,
              "BOOT: Serial-only mode (NSH on UART0)\n");

      while (g_running)
        {
          usleep(20000);
        }

      syslog(LOG_INFO, "PicoCalc-Term shutting down\n");
      return 0;
    }

  /* --- Step 5: Show boot splash --- */

  syslog(LOG_INFO, "BOOT: Step 5 — Boot splash\n");;

  /* Suspend keyboard I2C polling during splash to prevent southbridge
   * I2C reads (16 ms+ each at 10 kHz) from stalling lv_timer_handler.
   */

  lv_port_indev_keyboard_suspend(true);

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

  /* --- Step 9a: Initialize virtual consoles (tty0-3) --- */

  syslog(LOG_INFO, "BOOT: Step 9a — Initialize virtual consoles\n");
  vconsole_init(app_area);

  splash_pump(95, "Almost ready...");

  /* Final splash animation ramp */

  splash_pump(100, "Welcome!");
  usleep(300000);  /* Show "Welcome!" for 300 ms */

  /* Remove splash — launcher is now visible */

  boot_splash_hide();

  /* Re-enable keyboard I2C polling now that splash is gone. */

  lv_port_indev_keyboard_suspend(false);

  /* Force full-screen redraw so the launcher appears immediately.
   * The splash covered the entire screen; after deletion we must
   * invalidate the whole display so LVGL repaints the status bar
   * and launcher grid on the next lv_timer_handler() call.
   */

  lv_obj_invalidate(lv_scr_act());
  lv_timer_handler();

  /* --- Step 9b: Login screen (if enabled) --- */

  if (g_config.login_enabled && g_config.login_hash[0] != '\0')
    {
      syslog(LOG_INFO, "BOOT: Login required\n");
      login_show_if_needed(app_area);
    }

  /* --- Apply extended clock profile from settings --- */

  if (g_config.clock_profile != 2)  /* 2 = Standard 150 MHz (boot default) */
    {
      syslog(LOG_INFO, "BOOT: Applying clock profile %d\n",
             g_config.clock_profile);
      rp23xx_set_power_profile(g_config.clock_profile);
    }

  /* --- Apply startup mode (GUI or Console) --- */

  if (g_config.startup_mode == 1)  /* Console startup */
    {
      syslog(LOG_INFO, "BOOT: Console startup mode — switching to tty1\n");
      vconsole_switch(VCONSOLE_TTY1);
    }

  syslog(LOG_INFO, "BOOT: PicoCalc-Term ready!\n");

  /* --- Step 10: Main event loop --- */

  while (g_running)
    {
      uint32_t time_till_next = lv_timer_handler();

      /* Render active text console if its terminal grid is dirty */

      vconsole_render_if_dirty();

      /* Check for deferred app launch from the launcher (GUI mode) */

      if (!vconsole_is_text_active())
        {
          const char *pending = launcher_get_pending_launch();
          if (pending != NULL)
            {
              app_framework_launch(pending);
              launcher_clear_pending_launch();
            }
        }

      uint32_t sleep_ms = time_till_next < MAIN_LOOP_SLEEP_MS
                          ? time_till_next : MAIN_LOOP_SLEEP_MS;
      usleep(sleep_ms * 1000);
    }

  vconsole_shutdown();
  syslog(LOG_INFO, "PicoCalc-Term shutting down\n");
  return 0;
}
