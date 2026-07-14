# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
#
# Revision: 1.0.0
#
# Micro800Client - optional EtherNet/IP / CIP client for Allen-Bradley Micro800
#
# Enable in your project makefile before boilerplate.mk:
#   LIBS_MICRO800 := 1
#   include $(MICRO800_ROOT)/libraries/Micro800Client/library.mak
#
# Prefer a relative include path on Windows so CPP_SRC has no drive-letter colons:
#   include ../../libraries/Micro800Client/library.mak
#
# When installed under $(NNDK_ROOT)/libraries/Micro800Client, set:
#   MICRO800_ROOT := $(NNDK_ROOT)
#
# When developing from this repository, set:
#   MICRO800_ROOT := /path/to/NetBurner_MSSQL_Client

ifdef LIBS_MICRO800

# Directory containing this library.mak (use for sources — avoids D: colon in make rules)
MICRO800_CLIENT_DIR := $(subst \,/,$(dir $(lastword $(MAKEFILE_LIST))))

ifndef MICRO800_ROOT
MICRO800_ROOT := $(NNDK_ROOT)
endif

NBINCLUDE += -I"$(MICRO800_ROOT)/libraries/include"

CPP_SRC += \
	$(MICRO800_CLIENT_DIR)src/util.cpp \
	$(MICRO800_CLIENT_DIR)src/enip_session.cpp \
	$(MICRO800_CLIENT_DIR)src/cip_path.cpp \
	$(MICRO800_CLIENT_DIR)src/cip_types.cpp \
	$(MICRO800_CLIENT_DIR)src/tag_format.cpp \
	$(MICRO800_CLIENT_DIR)src/list_identity.cpp \
	$(MICRO800_CLIENT_DIR)src/cip_tag_io.cpp

endif
