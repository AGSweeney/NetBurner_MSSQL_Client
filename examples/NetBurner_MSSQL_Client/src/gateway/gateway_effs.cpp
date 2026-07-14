// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Adam G. Sweeney <agsweeney@gmail.com>

#include "gateway_effs.h"

#include <cstdlib>
#include <cstring>

#include <nbrtos.h>

#include <file/flashdrv.h>
#include <file/fsf.h>
#include <file/fwerr.h>

#ifndef FS_NO_ERROR
#define FS_NO_ERROR FS_NOERR
#endif

#ifndef NOR_DRV_NUM
#define NOR_DRV_NUM 0
#endif

// Provided by NetBurner effsStdFlashDrv.cpp (C++ linkage).
int fs_phy_OnChipFlash(FS_FLASH *flash);

namespace gateway {
namespace {

bool g_mounted = false;
OS_SEM g_effs_lock;
bool g_lock_inited = false;

void EnsureLock()
{
    if (!g_lock_inited) {
        OSSemInit(&g_effs_lock, 1);
        g_lock_inited = true;
    }
}

bool EnsureSqlQueueDir()
{
    // Prefer root, then create SQLQUEUE (8.3-safe).
    fs_chdir("/");
    const int mk = fs_mkdir("SQLQUEUE");
    // Already exists is fine on most EFFS builds (non-zero may still mean exists).
    (void)mk;
    if (fs_chdir("SQLQUEUE") != FS_NO_ERROR) {
        // Try again after mkdir
        fs_mkdir("SQLQUEUE");
        if (fs_chdir("SQLQUEUE") != FS_NO_ERROR) {
            return false;
        }
    }
    fs_chdir("/");
    return true;
}

#if !defined(SOMRT1061) && !defined(MODRT1171)

bool MountOnboardFlash()
{
    fs_init();

    const long mem_size = fs_getmem_flashdrive(fs_phy_OnChipFlash);
    if (mem_size <= 0) {
        return false;
    }

    char *mem_ptr = static_cast<char *>(malloc(static_cast<size_t>(mem_size)));
    if (!mem_ptr) {
        return false;
    }

    int rc = fs_mountdrive(NOR_DRV_NUM, mem_ptr, mem_size, fs_mount_flashdrive, fs_phy_OnChipFlash);
    if (rc != FS_NO_ERROR) {
        // First boot / virgin FS region: format once, then remount.
        rc = fs_format(NOR_DRV_NUM);
        if (rc != FS_NO_ERROR) {
            return false;
        }
        rc = fs_mountdrive(NOR_DRV_NUM, mem_ptr, mem_size, fs_mount_flashdrive, fs_phy_OnChipFlash);
        if (rc != FS_NO_ERROR) {
            return false;
        }
    }

    fs_chdrive(NOR_DRV_NUM);
    return true;
}

#else

bool MountOnboardFlash()
{
    // RT platforms: system EFFS is already mounted before UserMain.
    fs_chdrive(NOR_DRV_NUM);
    return true;
}

#endif

} // namespace

bool EffsGatewayInit()
{
    EnsureLock();
    EffsLock();

    g_mounted = false;
    if (!MountOnboardFlash()) {
        EffsUnlock();
        return false;
    }
    if (!EnsureSqlQueueDir()) {
        EffsUnlock();
        return false;
    }

    g_mounted = true;
    EffsUnlock();
    return true;
}

bool EffsGatewayIsMounted()
{
    return g_mounted;
}

void EffsLock()
{
    EnsureLock();
    OSSemPend(&g_effs_lock, 0);
}

void EffsUnlock()
{
    if (g_lock_inited) {
        OSSemPost(&g_effs_lock);
    }
}

} // namespace gateway
