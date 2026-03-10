############################################################################
# apps/system/pctime/Makefile
############################################################################

include $(APPDIR)/Make.defs

ifneq ($(CONFIG_SYSTEM_PCTIME),)
PROGNAME  += $(CONFIG_SYSTEM_PCTIME_PROGNAME)
PRIORITY  += $(CONFIG_SYSTEM_PCTIME_PRIORITY)
STACKSIZE += $(CONFIG_SYSTEM_PCTIME_STACKSIZE)
MODULE    += $(CONFIG_SYSTEM_PCTIME)
MAINSRC   += pctime_main.c
endif

ifneq ($(CONFIG_SYSTEM_PCNTP),)
PROGNAME  += $(CONFIG_SYSTEM_PCNTP_PROGNAME)
PRIORITY  += $(CONFIG_SYSTEM_PCNTP_PRIORITY)
STACKSIZE += $(CONFIG_SYSTEM_PCNTP_STACKSIZE)
MODULE    += $(CONFIG_SYSTEM_PCNTP)
MAINSRC   += pcntp_main.c
endif

include $(APPDIR)/Application.mk
