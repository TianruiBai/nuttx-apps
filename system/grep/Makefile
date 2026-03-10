############################################################################
# apps/system/grep/Makefile
#
# Minimal grep utility for NuttX NSH
#
############################################################################

include $(APPDIR)/Make.defs

# grep built-in application info

PROGNAME  = grep
PRIORITY  = SCHED_PRIORITY_DEFAULT
STACKSIZE = $(CONFIG_SYSTEM_GREP_STACKSIZE)
MODULE    = $(CONFIG_SYSTEM_GREP)

# grep source

MAINSRC = grep_main.c

include $(APPDIR)/Application.mk
