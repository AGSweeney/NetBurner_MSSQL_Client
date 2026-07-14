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

/*-------------------------------------------------------------------
 * EFFS-STD configuration file for 32MB Flash chip used on the MOD54415
 * and MOD54417.
 *
 * Macronix MX29GL256F 32MB Flash Chip.
 *-----------------------------------------------------------------*/

#ifndef _ONCHIPFLASH_H_
#define _ONCHIPFLASH_H_

#include "file/fsf.h"
#include "basictypes.h"
#include "hal.h"

#define FLASH_NAME "MX29GL256"

// functions implemented
extern int fs_phy_OnChipFlash(FS_FLASH *flash);

// Start of Flash memory base address
#define FS_FLASHBASE (0xC0000000)

/*
 * BLOCKSIZE
 * This defines the size of the blocks to be used in the file storage area.
 * This must be an erasable unit of the flash chip. All blocks in the file
 * storage area must be the same size. This maybe different from the DESCSIZE
 * where the flash chip has different size erasable units available.
 *
 * SECTORSIZE
 * This defines the sector size. Each block is divided into a number of sectors.
 * This number is the smallest usable unit in the system and thus represents the
 * minimum file storage area. For best usage of the flash blocks the sector size
 * should always be a power of 2. For more information see sector section below.
 *
 * SECTORPERBLOCK
 * This defines the number of sectors in a block. It must always be true that:
 * SECTORPERBLOCK = BLOCKSIZE/SECTORSIZE
 *
 *
 * The memory map below is for a MOD5441x with a 32MB bottom boot block flash.
 * This example will allocate 1.3MB for the file system.
 * - 2 blocks are reserved for the file descriptors
 * - 1 block is reserved as a free sector for the file system to operate
 *
 * A total of 10 blocks are allocated for the file system. Subtracting the 3 reserved
 * as described above, the total file system free space is: 7 * 128k = 896k (917,504 bytes).
 *
 * Macronix MX29GL256F, 256 blocks of 128KB each, Total of 32MBytes
 *
 * Note: this header file is also valid for the Spansion/Cypress 32MB flash
 *
 *                             Address
 *                            ----------
 *    ---------------------- C1FFF FFFF (End of flash space)
 *    |  File System Data  |
 *    |  1MB               |
 *    |  128K x 8 Blocks   |
 *    |--------------------| C1F0 0000 (Start of File System Data)
 *    |  DESC BLOCK 0/1    |
 *    | 128K x 2 Blocks    |
 *    |--------------------| C1EC 0000 (Start of File System)
 *    |                    |
 *    | Application        |
 *    | 31.232MB           |
 *    | 128K x 244 Blocks  |
 *    |                    |
 *    |--------------------| C004 0000
 *    | 128K User Params   |
 *    |--------------------| C002 0000
 *    | 128K System Params |
 *    |--------------------| C000 0000 (Start of Flash space)
 *
 *
 * CHANGES TO COMPCODE FLAGS
 * In NBEclipse, or your command line makefile, change the following line
 * so the application will only occupy the specified application space.
 * The first parameter is the start of application space, and the second
 * is the address just below the file system space.
 *
 *    COMPCODEFLAGS = 0xC0040000 0xC2000000     // Original
 *    COMPCODEFLAGS = 0xC0040000 0xC1F00000     // Space for file system, 1MB
 *
 */

/* WARNING: These settings are for MX29GL256F bottom boot block flash
 * components used on the Mod54415
 */
#define BLOCKSIZE (128 * 1024)   // Flash physical "sector" size, smallest erasable unit of flash
#define SECTORSIZE (1 * 1024)    // Flash sector size in bytes, smallest usable unit of flash
#define SECTORPERBLOCK (BLOCKSIZE / SECTORSIZE)

/*
 * Specify the total amount of flash memory in the system, and the amount
 * allocated to be used by the file system (the rest is used by the application.
 */
#define FLASH_SIZE (32 * 1024 * 1024)       // Total physical flash, 32MB

/*
 * Number of sectors allocated to file system. Includes 2 Descriptor Blocks, and 1 block reserved
 * for file system operation. This means the usable storage space will be less than the amount
 * calculated below.
 */
#define FS_SIZE (1280 * 1024)

#define FIRST_ADDR (FLASH_SIZE - FS_SIZE)   // first file system address
#define BLOCKSTART 2   // Starting file system data block. First 2 blocks are DESCRIPTORS

/*
 * Descriptor Blocks:
 * These blocks contain critical information about the file system, block allocation,
 * wear information and file/directory information. At least two descriptor blocks
 * must be included in the system, which can be erased independently. An optional
 * descriptor write cache may be configured which improves the performance of the
 * file system. Please refer to the EFFS-STD implementation guide for additional
 * information.
 */
#define DESCSIZE (128 * 1024)   // size of one descriptor
#define DESCBLOCKSTART 0        // position of first descriptor
#define DESCBLOCKEND 1          // position of last descriptor
#define DESCCACHE 2048

#endif /* _ONCHIPFLASH_H_ */
