# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
#
# Optional Micro800→MSSQL gateway (LIBS_GATEWAY := 1).
# Implies Micro800Client. Does not alter the generic SQL example when unset.
#
# Note: HTML merge rule for src/htmldata.cpp is applied in the example makefile
# AFTER include boilerplate.mk so it is not overridden.

ifdef LIBS_GATEWAY

ifndef LIBS_MICRO800
LIBS_MICRO800 := 1
endif

GATEWAY_EXAMPLE_DIR := $(subst \,/,$(dir $(lastword $(MAKEFILE_LIST))))

# Compile-time gate for shared sources (web.cpp, main.cpp, sql_runtime.cpp).
CXXFLAGS += -DNB_GATEWAY_MICRO800=1
CFLAGS += -DNB_GATEWAY_MICRO800=1

NBINCLUDE += -I"$(GATEWAY_EXAMPLE_DIR)src" -I"$(GATEWAY_EXAMPLE_DIR)src/gateway"

# Onboard EFFS-STD for durable queue (COMPCODEFLAGS + libStdFFile).
include $(GATEWAY_EXAMPLE_DIR)effs_std.mak

CPP_SRC += \
	$(GATEWAY_EXAMPLE_DIR)src/gateway/gateway_types.cpp \
	$(GATEWAY_EXAMPLE_DIR)src/gateway/gateway_config.cpp \
	$(GATEWAY_EXAMPLE_DIR)src/gateway/gateway_plc_adapter.cpp \
	$(GATEWAY_EXAMPLE_DIR)src/gateway/gateway_serializer.cpp \
	$(GATEWAY_EXAMPLE_DIR)src/gateway/gateway_effs.cpp \
	$(GATEWAY_EXAMPLE_DIR)src/gateway/gateway_queue.cpp \
	$(GATEWAY_EXAMPLE_DIR)src/gateway/gateway_sql.cpp \
	$(GATEWAY_EXAMPLE_DIR)src/gateway/gateway_runtime.cpp \
	$(GATEWAY_EXAMPLE_DIR)src/gateway/gateway_web.cpp

GATEWAY_HTML_STAGING := $(GATEWAY_EXAMPLE_DIR)obj/html_merged

.PHONY: gateway-html-merge
gateway-html-merge:
	@mkdir -p "$(GATEWAY_HTML_STAGING)/images"
	@rm -f "$(GATEWAY_HTML_STAGING)"/* 2>/dev/null || true
	@rm -f "$(GATEWAY_HTML_STAGING)/images/"* 2>/dev/null || true
	@cp -f "$(GATEWAY_EXAMPLE_DIR)html/"*.html "$(GATEWAY_HTML_STAGING)/"
	@cp -f "$(GATEWAY_EXAMPLE_DIR)html/"*.css "$(GATEWAY_HTML_STAGING)/" 2>/dev/null || true
	@cp -f "$(GATEWAY_EXAMPLE_DIR)html/"*.csv "$(GATEWAY_HTML_STAGING)/" 2>/dev/null || true
	@cp -f "$(GATEWAY_EXAMPLE_DIR)html/images/"* "$(GATEWAY_HTML_STAGING)/images/" 2>/dev/null || true
	@cp -f "$(GATEWAY_EXAMPLE_DIR)html_gateway/"* "$(GATEWAY_HTML_STAGING)/"

endif
