# Revision: 1.0.0

#NB_COPYRIGHT#

NAME = NetBurner_MSSQL_Client

SUPPORTED_PLATFORMS = NANO54415 MOD54415 SOMRT1061 MODRT1171 MODM7AE70

TDSLITE_ROOT ?= $(abspath ../..)

LIBS_TDSLITE := 1
include $(TDSLITE_ROOT)/libraries/TDSLite/library.mak

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
