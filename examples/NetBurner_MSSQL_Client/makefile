# Revision: 1.1.0

#NB_COPYRIGHT#

NAME = NetBurner_MSSQL_Client

SUPPORTED_PLATFORMS = NANO54415 MOD54415 MOD5441X SOMRT1061 MODRT1171 MODM7AE70

TDSLITE_ROOT ?= $(abspath ../..)

LIBS_TDSLITE := 1
include $(TDSLITE_ROOT)/libraries/TDSLite/library.mak

# Optional Micro800→MSSQL gateway. Leave unset for the generic SQL example.
# LIBS_GATEWAY := 1
# MICRO800_ROOT ?= $(abspath ../..)
# include ../../libraries/Micro800Client/library.mak
# include gateway.mak

ifdef LIBS_GATEWAY
MICRO800_ROOT ?= $(abspath ../..)
LIBS_MICRO800 := 1
include ../../libraries/Micro800Client/library.mak
include gateway.mak
endif

CPP_SRC += \
	src/main.cpp \
	src/sql_catalog.cpp \
	src/sql_nv.cpp \
	src/sql_results.cpp \
	src/sql_runtime.cpp \
	src/web.cpp

CREATEDTARGS += src/htmldata.cpp
CPP_SRC += src/htmldata.cpp

include $(NNDK_ROOT)/make/boilerplate.mk

# HTML rules AFTER boilerplate so they win over the default html/ wildcard.
ifndef LIBS_GATEWAY
src/htmldata.cpp: $(wildcard html/*.*)
	comphtml html -osrc/htmldata.cpp
else
src/htmldata.cpp: gateway-html-merge $(wildcard html/*.*) $(wildcard html_gateway/*.*)
	comphtml "$(GATEWAY_HTML_STAGING)" -osrc/htmldata.cpp
endif
