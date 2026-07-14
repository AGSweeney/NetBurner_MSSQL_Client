/* Revision: 3.5.8 */

/******************************************************************************
* Copyright 1998-2024 NetBurner, Inc.  ALL RIGHTS RESERVED
*
*    Permission is hereby granted to purchasers of NetBurner Hardware to use or
*    modify this computer program for any use as long as the resultant program
*    is only executed on NetBurner provided hardware.
*
*    No other rights to use this program or its derivatives in part or in
*    whole are granted.
*
*    It may be possible to license this or other NetBurner software for use on
*    non-NetBurner Hardware. Contact sales@Netburner.com for more information.
*
*    NetBurner makes no representation or warranties with respect to the
*    performance of this computer program, and specifically disclaims any
*    responsibility for any damages, special or consequential, connected with
*    the use of this program.
*
* NetBurner
* 16855 W Bernardo Dr
* San Diego, CA 92127
* www.netburner.com
******************************************************************************/

/*
 * MULTI-PLATFORM NOTE
 * This example supports several NetBurner platforms from one code base. The only
 * platform-specific *code* lives in this file (the flash driver and the geometry
 * header selected below); the only other per-platform settings are the memory
 * ranges in the makefile. Everything else -- the mount logic and the HTTP/FTP/
 * serial interaction code -- is shared and identical across platforms.
 *
 * If you target a SINGLE platform, you can delete the #if/#elif branches for the
 * other platforms here (keep only your platform's flashChip include and driver)
 * and likewise delete the other platforms' blocks in the makefile.
 *
 * The RT platforms (SOMRT1061, MODRT1171) are special: their EFFS-STD "system
 * file system" is created and mounted by the platform library before UserMain(),
 * so their branches below only supply a flash-name string and (for MODRT1171)
 * delegate to the platform driver -- the application does not create the FS.
 */

/* Choose the proper flash chip for the platform */
#if (defined MOD5441X)
#include "flashChip/MX29GL256F.h"
#elif ((defined NANO54415) || (defined SB800EX))
#include "flashChip/MX25L6406E.h"
#elif ((defined MODM7AE70) || (defined SBE70LC))
#include "flashChip/SAME70Q21.h"
#elif (defined SOMRT1061)
    // EFFS Flash functions defined by processor source in arch/cortex-m7/cpu/MIMXRT10xx/EffsStd.cpp
#elif (defined MODRT1171)
    // EFFS Flash functions defined by processor source in arch/cortex-m7/cpu/MIMXRT11xx/source/EffsStd.cpp
#else
#error "*** EFFS STD FLASH PLATFORM NOT DEFINED ***"
#endif

#include <string.h>

#if (defined MODM7AE70 || defined SBE70LC)
extern "C" uint32_t _FlashGetSize_Bytes();
uint32_t FlashGetSize_Bytes()
{
    return _FlashGetSize_Bytes() - FS_SIZE;
}
#endif

/*-------------------------------------------------------------------------------------
 * The EFFS STD HTTP standard flash file system example consists of may files and
 * features and works on all 3.x platforms. It is identical with the exception of
 * this example driver file. To help with the maintence of the example, this file
 * contains two large #ifdef sections: one to support parallel flash, and the other
 * to support SPI flash.
 *------------------------------------------------------------------------------------*/

/*====================================================================================*/
/*====================================================================================*/
/*====================================================================================*/

/*------------------------------------------------------------------------------
 * EFFS STD flash file system driver example for parallel flash based devices.
 * This driver example is currently in use with the NetBurner MOD5441x and MODM7AE70
 * platforms.
 *----------------------------------------------------------------------------*/

#if ((defined MOD5441X) || (defined MODM7AE70) || (defined SBE70LC))

/**
 * OnChipFlash_GetFlashName
 */
void OnChipFlash_GetFlashName(char *pName)
{
    strcpy(pName, FLASH_NAME);
    return;
}

/**
 *  GetBlockAddr
 *
 *  Get a logical block physical relative address in flash
 *  relsector - relative sector in the block (<sectorperblock)
 *
 *  INPUTS
 *
 *  block - block number
 *  relsector - rel
 *
 *  RETURNS
 *
 *  relative physical address of block
 */
static long GetBlockAddr(long block, long relsector)
{
    long rc = FIRST_ADDR + block * BLOCKSIZE + relsector * SECTORSIZE;
    if (rc < FIRST_ADDR || rc > FLASH_SIZE) { rc = FIRST_ADDR; }
    return (rc);
}

/**
 *  EraseFlash
 *
 *  INPUTS
 *
 *  block - which block needs to be erased
 *
 *  RETURNS
 *  0 - always
 */
int EraseFlash(long block)
{
    FlashErase((void *)(FS_FLASHBASE + GetBlockAddr(block, 0)), BLOCKSIZE);
    return 0;
}

/**
 *  WriteFlash
 *
 *  Writing (programming) Flash device
 *
 *  INPUTS
 *
 *  data - where data is
 *  block - which block is programmed
 *  relsector - relative sector in the block (<sectorperblock)
 *  size - length of data
 *  relpos - relative position of data in block
 *
 *  RETURNS
 *
 *  0 - if ok
 *  1 - if there was any error
 */
int WriteFlash(void *data, long block, long relsector, long size, long relpos)
{
    FlashProgram((void *)(FS_FLASHBASE + GetBlockAddr(block, relsector) + relpos), data, size);
    return 0;
}

/**
 *  VerifyFlash
 *
 *  Compares data with what flash contains
 *
 *  INPUTS
 *
 *  data - where data is
 *  block - which block is programmed
 *  relsector - relative sector in the block (<sectorperblock)
 *  size - length of data
 *  relpos - relative position of data in block
 *
 *  RETURNS
 *
 *  0 - if ok
 *  1 - if there was any error
 */
int VerifyFlash(void *data, long block, long relsector, long size, long relpos)
{
    unsigned short *d = (unsigned short *)(FS_FLASHBASE + GetBlockAddr(block, relsector) + relpos);
    unsigned short *s = (unsigned short *)(data);
    long a = 0;

    size++;

    size >>= 1;
    size <<= 1;   // word align

    for (a = 0; a < size; a += 2)   // verify
    {
        if (*d++ != *s++)
        {
            return 1;   // failed
        }
    }
    return 0;   // ok
}

/**
 *  ReadFlash
 *
 *  read data from flash
 *
 *  INPUTS
 *
 *  data - where to store data
 *  block - block number which block to be read
 *  blockrel - relative start address in the block
 *  datalen - length of data in bytes
 *
 *  RETURNS
 *  0 - if successfully read
 */
int ReadFlash(void *data, long block, long blockrel, long datalen)
{
    long src = GetBlockAddr(block, 0) + blockrel;
    fsm_memcpy(data, (char *)(FS_FLASHBASE + src), datalen);
    return 0;
}

/**
 *  fs_phy_OnChipFlash
 *
 *  Identify a flash and fills FS_FLASH structure with data
 *
 *  INPUTS
 *
 *  flash - structure which is filled with data of flash properties
 *
 *  RETURNS
 *
 *  0 - if successfully
 *  1 - flash not valid
 */
int fs_phy_OnChipFlash(FS_FLASH *flash)
{
    flash->ReadFlash = ReadFlash;       // read content
    flash->EraseFlash = EraseFlash;     // erase a block
    flash->WriteFlash = WriteFlash;     // write content
    flash->VerifyFlash = VerifyFlash;   // verify content

    flash->maxblock = (FS_SIZE / BLOCKSIZE) - BLOCKSTART;
    flash->blocksize = BLOCKSIZE;
    flash->sectorsize = SECTORSIZE;
    flash->sectorperblock = SECTORPERBLOCK;
    flash->blockstart = BLOCKSTART;
    flash->descsize = DESCSIZE;
    flash->descblockstart = DESCBLOCKSTART;
    flash->descblockend = DESCBLOCKEND;
    flash->cacheddescsize = DESCCACHE;   // write cache size in descriptor

    return 0;
}
#endif

/*====================================================================================*/
/*====================================================================================*/
/*====================================================================================*/

/*------------------------------------------------------------------------------
 * EFFS STD flash file system driver example for SPI flash based devices.
 * References to "nano" refer to NetBurner ColdFire based SPI flash
 * architecture. This driver example is currently in use with the NetBurner
 * NANO54415 and SB800EX.
 *----------------------------------------------------------------------------*/

#if ((defined NANO54415) || (defined SB800EX))

#include <string.h>
#include <NanoStdFileSupport.h>
#include <stdio.h>
#include "flashChip/MX25L6406E.h"   // SPI flash chip

#define BLOCKS_PER_DESC (DESCSIZE / BLOCKSIZE)

void OnChipFlash_GetFlashName(char *pName)
{
    strcpy(pName, FLASH_NAME);
    return;
}

/****************************************************************************
 *
 * GetBufferBlockAddr
 *
 * Get a logical block physical relative address in flash
 * relsector - relative sector in the block (<sectorperblock)
 *
 * INPUTS
 *
 * block - block number
 * relsector - rel
 *
 * RETURNS
 *
 * relative physical address of block
 *
 ***************************************************************************/
static long GetBufferBlockAddr(long block, long relsector)
{
    long rc;
    if (block == 1) { block = BLOCKS_PER_DESC; }
    rc = block * BLOCKSIZE + relsector * SECTORSIZE;
    if (rc < 0 || rc > FS_SIZE) { rc = 0; }
    return (rc);
}

static long GetBlockAddr(long block, long relsector)
{
    return GetBufferBlockAddr(block, relsector) + FIRST_ADDR;
}

static uint8_t StdFlashBuffer[FS_SIZE];

#ifdef EXTRA_SLOW_VERIFY
static uint8_t CpyFlashBuffer[FS_SIZE];

void RawVerify(const char *name, unsigned long pos, int len)
{
    int i;
    NanoStdFlashInit(CpyFlashBuffer, FS_SIZE, FIRST_ADDR);
    for (i = 0; i < FS_SIZE; i++)
    {
        if (StdFlashBuffer[i] != CpyFlashBuffer[i])
        {
            printf("Verify error at %d (%08x) Buf=%02X Flash = %02X\r\n", i, i, StdFlashBuffer[i], CpyFlashBuffer[i]);
            printf("Error at %s for %08X %d\r\n", name, pos, len);
            return;
        }
    }
    printf("%s ok at %08X for %d\r\n", name, pos, len);
}
#else
#define RawVerify(x, y, z) (void)0
#endif

/****************************************************************************
 *
 * EraseFlash
 *
 * INPUTS
 *
 * block - which block needs to be erased
 *
 * RETURNS
 * 0 - always
 *
 ***************************************************************************/
int NanoEraseFlash(long block)
{
    int i;
    long offset = GetBufferBlockAddr(block, 0);
    long eraseSize = ((block == 0) || (block == 1)) ? BLOCKSIZE * BLOCKS_PER_DESC : BLOCKSIZE;

    if ((block == 0) || (block == 1))
    {
        long rawOffset = GetBlockAddr(block, 0);
        for (i = 0; i < BLOCKS_PER_DESC; i++)
        {
            NanoRawEraseFlash(rawOffset, BLOCKSIZE);
            rawOffset += BLOCKSIZE;
        }
    }
    else
    {
        long rawOffset = GetBlockAddr(block, 0);
        NanoRawEraseFlash(rawOffset, BLOCKSIZE);
    }

    for (i = 0; i < eraseSize; i++)
    {
        StdFlashBuffer[i + offset] = 0xFF;
    }
    RawVerify("Erase", offset, BLOCKSIZE);
    return 0;
}

/****************************************************************************
 *
 * WriteFlash
 *
 * Writing (programming) Flash device
 *
 * INPUTS
 *
 * data - where data is
 * block - which block is programmed
 * relsector - relative sector in the block (<sectorperblock)
 * size - length of data
 * relpos - relative position of data in block
 *
 * RETURNS
 *
 * 0 - if ok
 * 1 - if there was any error
 *
 ***************************************************************************/
int NanoWriteFlash(void *data, long block, long relsector, long size, long relpos)
{
    int i;
    long offset = (GetBufferBlockAddr(block, relsector) + relpos);
    unsigned char *pbData = (unsigned char *)data;
    long bo = GetBlockAddr(block, relsector);

    long start_p = (bo + relpos);
    long end_p = start_p + size;
    long tempStart_p, tempEnd_p;

    // Copy the flash write into the ram mirror
    for (i = 0; i < size; i++)
        StdFlashBuffer[i + offset] = pbData[i];

    // Adjust the programming area to cover full sectors in spi flash
    start_p = start_p & 0xFFFFFF00;   // round down to nearest 256 bytes
    end_p = end_p | 0xff;             // round up to nearest 256 bytes

    if (end_p & 1)   // make normal writes even lengths
        end_p++;

    if (end_p >= FLASH_SIZE)   // Check for end of physical flash
        end_p = (FLASH_SIZE - 1);

    // Do the actual write to spi flash
    size = end_p - start_p;
    tempStart_p = start_p;
    do
    {
        tempEnd_p = (size > BLOCKSIZE) ? (tempStart_p + BLOCKSIZE) : (tempStart_p + size);
        NanoRawWriteFlash(StdFlashBuffer + (offset & 0xFFFFFF00), tempStart_p, tempEnd_p - tempStart_p);
        offset += tempEnd_p - tempStart_p;
        size -= tempEnd_p - tempStart_p;
        tempStart_p = tempEnd_p;
    } while (tempEnd_p != end_p);
    RawVerify("Write", offset, BLOCKSIZE);

    return 0;
}

/****************************************************************************
 *
 * VerifyFlash
 *
 * Compares data with flash containes
 *
 * INPUTS
 *
 * data - where data is
 * block - which block is programmed
 * relsector - relative sector in the block (<sectorperblock)
 * size - length of data
 * relpos - relative position of data in block
 *
 * RETURNS
 *
 * 0 - if ok
 * 1 - if there was any error
 *
 ***************************************************************************/
int NanoVerifyFlash(void *data, long block, long relsector, long size, long relpos)
{
    unsigned short *d = (unsigned short *)(StdFlashBuffer + GetBufferBlockAddr(block, relsector) + relpos);
    unsigned short *s = (unsigned short *)(data);
    long a;

    size++;

    size >>= 1;
    size <<= 1;   // word align

    for (a = 0; a < size; a += 2)   // verify
    {
        if (*d++ != *s++)
        {
            return 1;   // failed
        }
    }
    return 0;   // ok
}

/****************************************************************************
 *
 * ReadFlash
 *
 * read data from flash
 *
 * INPUTS
 *
 * data - where to store data
 * block - block number which block to be read
 * blockrel - relative start address in the block
 * datalen - length of data in bytes
 *
 * RETURNS
 * 0 - if sucessfully read
 *
 ***************************************************************************/
int NanoReadFlash(void *data, long block, long blockrel, long datalen)
{
    long src = GetBufferBlockAddr(block, 0) + blockrel;
    fsm_memcpy(data, StdFlashBuffer + src, datalen);
    return 0;
}

/****************************************************************************
 *
 * fs_phy_OnChipFlash
 *
 * Identify a flash and fills FS_FLASH structure with data
 *
 * INPUTS
 *
 * flash - structure which is filled with data of flash properties
 *
 * RETURNS
 *
 * 0 - if successfully
 * 1 - flash not valid
 *
 ***************************************************************************/

int fs_phy_OnChipFlash(FS_FLASH *flash)
{
    flash->ReadFlash = NanoReadFlash;       // read content
    flash->EraseFlash = NanoEraseFlash;     // erase a block
    flash->WriteFlash = NanoWriteFlash;     // write content
    flash->VerifyFlash = NanoVerifyFlash;   // verify content

    flash->maxblock = (FS_SIZE / BLOCKSIZE) - BLOCKSTART;
    flash->blocksize = BLOCKSIZE;
    flash->sectorsize = SECTORSIZE;
    flash->sectorperblock = SECTORPERBLOCK;
    flash->blockstart = BLOCKSTART;
    flash->descsize = DESCSIZE;
    flash->descblockstart = DESCBLOCKSTART;
    flash->descblockend = DESCBLOCKEND;
    flash->cacheddescsize = DESCCACHE;   // write cache size in descriptor

    static BOOL bInitialized = FALSE;
    if (!bInitialized)
    {
        NanoStdFlashInit(StdFlashBuffer, FS_SIZE, FIRST_ADDR);
        RawVerify("Init", 0, 0);
        bInitialized = TRUE;
    }

    return 0;
}
#elif (defined SOMRT1061)
    // EFFS Flash functions defined by processor source in arch/cortex-m7/cpu/MIMXRT10xx/EffsStd.cpp

#define FLASH_NAME "W25Q64JVXGIM"

void OnChipFlash_GetFlashName(char *pName)
{
    strcpy(pName, FLASH_NAME);
    return;
}

#elif (defined MODRT1171)

#include <file/fsm.h>

#define FLASH_NAME "GD25Q80CEIGR"

void OnChipFlash_GetFlashName(char *pName)
{
    strcpy(pName, FLASH_NAME);
    return;
}

extern int fs_phy_AFP0(FS_FLASH *flash);

int fs_phy_OnChipFlash(FS_FLASH *flash)
{
    return fs_phy_AFP0(flash);
}

#endif
