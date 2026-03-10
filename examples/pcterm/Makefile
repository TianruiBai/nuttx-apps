############################################################################
# apps/examples/pcterm/Makefile
#
# PicoCalc-Term OS — full build with TermOS core, all built-in apps,
# LVGL display/input port, and boot splash animation.
#
# Source layout (outside this directory):
#   $(PCTERM_ROOT)/pcterm/src/       — TermOS core sources
#   $(PCTERM_ROOT)/pcterm/include/   — TermOS headers
#   $(PCTERM_ROOT)/apps/*/           — Built-in application sources
############################################################################

include $(APPDIR)/Make.defs

# --- Project root (parent of nuttx-apps/) ---

PCTERM_ROOT = $(APPDIR)/..

# --- Program metadata ---

PROGNAME  = $(CONFIG_EXAMPLES_PCTERM_PROGNAME)
PRIORITY  = $(CONFIG_EXAMPLES_PCTERM_PRIORITY)
STACKSIZE = $(CONFIG_EXAMPLES_PCTERM_STACKSIZE)
MODULE    = $(CONFIG_EXAMPLES_PCTERM)

# --- Entry point (this directory) ---

MAINSRC = pcterm_main.c

# --- Include paths ---

CFLAGS += ${INCDIR_PREFIX}$(PCTERM_ROOT)/pcterm/include
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/include/netutils

# wolfSSH / wolfSSL headers (needed by apps/pcssh/ SSH client)
ifneq ($(CONFIG_CRYPTO_WOLFSSH),)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/wolfssh/wolfssh
endif
ifneq ($(CONFIG_CRYPTO_WOLFSSL),)
CFLAGS += ${INCDIR_PREFIX}$(APPDIR)/crypto/wolfssl/wolfssl
CFLAGS += -DWOLFSSL_USER_SETTINGS
endif

# --- VPATH: directories containing source files ---

VPATH += :$(PCTERM_ROOT)/pcterm/src
VPATH += :$(PCTERM_ROOT)/apps/settings
VPATH += :$(PCTERM_ROOT)/apps/pcedit
VPATH += :$(PCTERM_ROOT)/apps/pcfiles
VPATH += :$(PCTERM_ROOT)/apps/pcterm
VPATH += :$(PCTERM_ROOT)/apps/pcaudio
VPATH += :$(PCTERM_ROOT)/apps/pccsv
VPATH += :$(PCTERM_ROOT)/apps/pcssh
VPATH += :$(PCTERM_ROOT)/apps/pcwireless
VPATH += :$(PCTERM_ROOT)/apps/pcvideo
VPATH += :$(PCTERM_ROOT)/apps/pcweb

# Also search in this directory for dependency tracking

DEPPATH += --dep-path $(PCTERM_ROOT)/pcterm/src
DEPPATH += --dep-path $(PCTERM_ROOT)/apps/settings
DEPPATH += --dep-path $(PCTERM_ROOT)/apps/pcedit
DEPPATH += --dep-path $(PCTERM_ROOT)/apps/pcfiles
DEPPATH += --dep-path $(PCTERM_ROOT)/apps/pcterm
DEPPATH += --dep-path $(PCTERM_ROOT)/apps/pcaudio
DEPPATH += --dep-path $(PCTERM_ROOT)/apps/pccsv
DEPPATH += --dep-path $(PCTERM_ROOT)/apps/pcssh
DEPPATH += --dep-path $(PCTERM_ROOT)/apps/pcwireless
DEPPATH += --dep-path $(PCTERM_ROOT)/apps/pcvideo
DEPPATH += --dep-path $(PCTERM_ROOT)/apps/pcweb

# =========================================================================
# Additional source files (CSRCS)
# =========================================================================

# --- TermOS core (pcterm/src/) ---

CSRCS += boot_splash.c
CSRCS += lv_port_disp.c
CSRCS += lv_port_indev.c
CSRCS += statusbar.c
CSRCS += launcher.c
CSRCS += app_framework.c
CSRCS += config.c
CSRCS += hostname.c
CSRCS += audio_service.c
CSRCS += package_manager.c
CSRCS += terminal_widget.c
CSRCS += app_state.c
CSRCS += psram_stubs.c
CSRCS += link_stubs.c

# --- setjmp / longjmp (ARM assembly) ---

ASRCS += setjmp_stubs.S

# --- Settings app (apps/settings/) — 8 files ---

CSRCS += settings_main.c
CSRCS += settings_wifi.c
CSRCS += settings_display.c
CSRCS += settings_keyboard.c
CSRCS += settings_audio.c
CSRCS += settings_storage.c
CSRCS += settings_system.c
CSRCS += settings_packages.c
CSRCS += settings_power.c

# --- Text Editor (apps/pcedit/) — 8 files ---

CSRCS += pcedit_main.c
CSRCS += pcedit_buffer.c
CSRCS += pcedit_file.c
CSRCS += pcedit_render.c
CSRCS += pcedit_syntax.c
CSRCS += pcedit_vi.c
CSRCS += pcedit_search.c
CSRCS += pcedit_plugin.c

# --- File Explorer (apps/pcfiles/) — 2 files ---

CSRCS += pcfiles_main.c
CSRCS += pcfiles_ops.c

# --- Terminal (apps/pcterm/) — 2 files ---

CSRCS += pcterm_local.c
CSRCS += pcterm_serial.c
CSRCS += pcterm_nsh.c

# --- Audio Player (apps/pcaudio/) — 5 files ---

CSRCS += pcaudio_main.c
CSRCS += pcaudio_decoder.c
CSRCS += pcaudio_playlist.c
CSRCS += pcaudio_ui.c
CSRCS += pcaudio_player.c

# --- CSV Editor (apps/pccsv/) — 4 files ---

CSRCS += pccsv_main.c
CSRCS += pccsv_parser.c
CSRCS += pccsv_edit.c
CSRCS += pccsv_table.c

# --- SSH Client (apps/pcssh/) — 5 files ---

CSRCS += pcssh_main.c
CSRCS += pcssh_client.c
CSRCS += pcssh_connections.c
CSRCS += pcssh_sftp.c
CSRCS += pcssh_scp.c

# --- Wireless Manager (apps/pcwireless/) — 1 file ---

CSRCS += pcwireless_main.c

# --- Video Player (apps/pcvideo/) — 4 files ---

CSRCS += pcvideo_main.c
CSRCS += pcvideo_pcv.c
CSRCS += pcvideo_playback.c
CSRCS += pcvideo_ui.c

# --- Web Browser (apps/pcweb/) — 6 files ---

CSRCS += pcweb_main.c
CSRCS += pcweb_http.c
CSRCS += pcweb_html.c
CSRCS += pcweb_render.c
CSRCS += pcweb_nav.c
CSRCS += pcweb_image.c

# =========================================================================

include $(APPDIR)/Application.mk
