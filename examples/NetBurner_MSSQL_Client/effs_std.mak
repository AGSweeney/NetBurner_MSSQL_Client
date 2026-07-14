# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>
#
# EFFS-STD onboard flash for the gateway durable queue.
# Flash driver sources are staged under ./effs_std/src (copied from NNDK
# examples/_common/EFFS/STD) so CPP_SRC stays drive-letter-free on Windows Make.

ifndef GATEWAY_EXAMPLE_DIR
$(error GATEWAY_EXAMPLE_DIR must be set before including effs_std.mak)
endif

EFFS_STD_STAGE := $(GATEWAY_EXAMPLE_DIR)effs_std
NBINCLUDE += -I"$(EFFS_STD_STAGE)/src"

# RT platforms ship EFFS in the platform library — skip app driver + XTRALIB.
ifeq ($(filter $(PLATFORM),SOMRT1061 MODRT1171),)
CPP_SRC += $(EFFS_STD_STAGE)/src/effsStdFlashDrv.cpp
XTRALIB = $(NNDK_ROOT)/platform/$(PLATFORM)/original/lib/libStdFFile.a
DBXTRALIB = $(NNDK_ROOT)/platform/$(PLATFORM)/original/lib/libStdFFile.a
endif

# Reserve top of flash for EFFS-STD (must match HttpAFS / flashChip FS_SIZE).
ifeq ($(PLATFORM),MOD5441X)
COMPCODEFLAGS = 0xC0040000 0xC1F00000
endif
ifeq ($(PLATFORM),MOD54415)
COMPCODEFLAGS = 0xC0040000 0xC1F00000
endif
ifeq ($(PLATFORM),NANO54415)
COMPCODEFLAGS = 0x04000 0x700000
endif
ifeq ($(PLATFORM),MODM7AE70)
EXTRAPACKARGS += -cflag C:3
endif
