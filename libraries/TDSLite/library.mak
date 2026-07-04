# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
#
# Revision: 1.0.0
#
# TDSLite - MSSQL/TDS client library for NetBurner (header-only)
#
# Enable in your project makefile before boilerplate.mk:
#   LIBS_TDSLITE := 1
#   include $(TDSLITE_ROOT)/libraries/TDSLite/library.mak
#
# When installed under $(NNDK_ROOT)/libraries/TDSLite, set:
#   TDSLITE_ROOT := $(NNDK_ROOT)
#
# When developing from this repository, set:
#   TDSLITE_ROOT := /path/to/NetBurner_MSSQL_Client

ifdef LIBS_TDSLITE

ifndef TDSLITE_ROOT
TDSLITE_ROOT := $(NNDK_ROOT)
endif

NBINCLUDE += -I"$(TDSLITE_ROOT)/libraries/include"

endif
