# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
#
# Revision: 2.0.0
#
# HsesClient - NetBurner port of MotoHSES (Motoman HSES UDP client)
#
# Enable in your project makefile before boilerplate.mk:
#   LIBS_HSES := 1
#   include ../../libraries/HsesClient/library.mak
#
# Prefer a relative include path on Windows so CPP_SRC has no drive-letter colons.
#
# When installed under $(NNDK_ROOT)/libraries/HsesClient, set:
#   HSES_ROOT := $(NNDK_ROOT)
#
# When developing from this repository, set:
#   HSES_ROOT := /path/to/NetBurner_MSSQL_Client

ifdef LIBS_HSES

HSES_CLIENT_DIR := $(subst \,/,$(dir $(lastword $(MAKEFILE_LIST))))

ifndef HSES_ROOT
HSES_ROOT := $(NNDK_ROOT)
endif

NBINCLUDE += -I"$(HSES_ROOT)/libraries/include"

CPP_SRC += \
	$(HSES_CLIENT_DIR)src/moto_hses.cpp

endif
