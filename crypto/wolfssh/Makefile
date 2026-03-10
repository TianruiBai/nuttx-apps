############################################################################
# apps/crypto/wolfssh/Makefile
#
# wolfSSH embedded SSH library build for NuttX
#
############################################################################

include $(APPDIR)/Make.defs

# wolfSSH source directory and remote
WOLFSSH_DIR = $(APPDIR)/crypto/wolfssh/wolfssh
WOLFSSH_REMOTE ?= https://github.com/wolfSSL/wolfssh.git

# When wolfSSH sources are present, build them.
# Otherwise this is a no-op (the Kconfig symbol still exists so that
# application code can #ifdef on CONFIG_CRYPTO_WOLFSSH).

ifneq ($(CONFIG_CRYPTO_WOLFSSH),)

wolfssh:
	@if [ ! -f "$(WOLFSSH_DIR)/src/ssh.c" ]; then \
		echo "Fetching wolfSSH sources..."; \
		git clone "$(WOLFSSH_REMOTE)" "$(WOLFSSH_DIR)"; \
	fi

context:: wolfssh

ifneq ($(wildcard $(WOLFSSH_DIR)/src/ssh.c),)

CFLAGS += -I$(WOLFSSH_DIR)
CFLAGS += -DWOLFSSL_USER_SETTINGS
CFLAGS += -DWOLFSSH_USER_SETTINGS

CSRCS += $(WOLFSSH_DIR)/src/ssh.c
CSRCS += $(WOLFSSH_DIR)/src/internal.c
CSRCS += $(WOLFSSH_DIR)/src/io.c
CSRCS += $(WOLFSSH_DIR)/src/log.c
CSRCS += $(WOLFSSH_DIR)/src/port.c
CSRCS += $(WOLFSSH_DIR)/src/wolfterm.c

ifeq ($(CONFIG_CRYPTO_WOLFSSH_SCP),y)
CSRCS += $(WOLFSSH_DIR)/src/wolfscp.c
endif

ifeq ($(CONFIG_CRYPTO_WOLFSSH_SFTP),y)
CSRCS += $(WOLFSSH_DIR)/src/wolfsftp.c
endif

endif # wolfssh sources present
endif # CONFIG_CRYPTO_WOLFSSH

distclean::
	$(call DELDIR, $(WOLFSSH_DIR))

include $(APPDIR)/Application.mk
