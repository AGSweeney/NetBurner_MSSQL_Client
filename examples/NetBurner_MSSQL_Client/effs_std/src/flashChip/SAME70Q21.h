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
 * EFFS-STD configuration file for Microchip SAME70Q21 processor
 * onboard 2MB Flash memory. Used on the MODM7AE70 and SBE70LC.
 *-----------------------------------------------------------------*/

#ifndef _ONCHIPFLASH_H_
#define _ONCHIPFLASH_H_

#include "file/fsf.h"
#include "basictypes.h"
#include "hal.h"

#define FLASH_NAME "SAME70Q21"

// functions implemented
extern int fs_phy_OnChipFlash(FS_FLASH *flash);

// Start of Flash memory base address
#define FS_FLASHBASE (0x00400000)

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
 * The memory map below is for a MODM7AE70 with a 2MB bottom boot block flash.
 * This example will allocate 1.5MB for the application space, and 512KB for
 * the file system.
 *
 * Microchip SAME70Q21:
 * The memory is organized in sectors. Each sector has a size of 128KB. The
 * first sector is divided into 3 smaller sectors. The three smaller sectors are
 * organized in 2 sectors of 8KB and 1 sector of 112KB.
 * Total number of bytes for storage: 2,097,152. Application space: 1.97MB max.
 *
 *                             Address
 *                            ----------
 *    ---------------------- 005F FFFF (End of flash space)
 *    |  File System Data  |
 *    |  256KB             |
 *    |  128K x 2 Blocks   |
 *    |--------------------| 005C 0000 (Start of File System Data)
 *    |  DESC BLOCK 0/1    |
 *    |  128K x 2 Blocks   |
 *    |--------------------| 0058 0000 (Start of File System)
 *    |                    |
 *    | Application        |
 *    | 1.375MB            |
 *    | 128K x 11 Blocks   |
 *    |                    |
 *    |--------------------| 0040 6004
 *    |   8K User Params   |
 *    |--------------------| 0040 4000
 *    |  10K Configuration |
 *    |--------------------| 0040 1800
 *    |   6K Monitor       |
 *    |--------------------| 0040 0000 (Start of Flash space)
 *
 *
 * DISABLING BACKGROUND ERASE
 * To use the EFFS-STD file system on the MODM7AE70 and SBE70LC, you must enable the feature:
 * "Locate Application at Fixed Address in Flash". This disables the background flash erase
 * feature, would will erase the flash file system sectors. 
 *
 * If using NBEclipse:
 * - Right-click on the project and select "Properties"
 * - Navigate to C/C++ Build > Settings > NB Flashpack > General
 * - Check the checkbox for "Locate Application at Fixed Address in Flash" 
 *
 * If you used the NBEclipse import example in the project creation, the StdFFile.a library will
 * be included automatically. Otherwise, the library must be added manually. 
 * - Navigate to C/C++ Build > Settings > GNC C/C++ Linker > Libraries
 * - In the Libraries section, add "StdFFile"
 */

#define BLOCKSIZE      (16 * 512)    // Flash physical "sector" size, smallest erasable unit of flash
#define SECTORSIZE     (512)         // Flash sector size in bytes, smallest usable unit of flash
#define SECTORPERBLOCK (BLOCKSIZE / SECTORSIZE)

/*
 * Specify the total amount of flash memory in the system, and the amount
 * allocated to be used by the file system (the rest is used by the
 * application.
 */
#define FLASH_SIZE (2 * 1024 * 1024)      // Total physical flash, 2MB

/*
 * Number of sectors allocated to file system. Includes 2 Descriptor Blocks, and 1 block reserved
 * for file system operation. This means the usable storage space will be less than the amount
 * calculated below.
 * Total space reserved for EFFS: 256 * 512 bytes/sector = 131072 bytes.
 */
#define FS_SIZE    (256 * 512)            

#define FIRST_ADDR (FLASH_SIZE - FS_SIZE) // First file system address
#define BLOCKSTART 2                      // Starting file system data block. First 2 blocks are DESCRIPTORS

/*
 * Descriptor Blocks:
 * These blocks contain critical information about the file system, block allocation,
 * wear information and file/directory information. At least two descriptor blocks
 * must be included in the system, which can be erased independently. An optional
 * descriptor write cache may be configured which improves the performance of the
 * file system. Please refer to the EFFS-STD implementation guide for additional
 * information.
 */
#define DESCSIZE (16 * 512)   // size of one descriptor
#define DESCBLOCKSTART 0      // position of first descriptor
#define DESCBLOCKEND 1        // position of last descriptor
#define DESCCACHE 1024

#endif /* _ONCHIPFLASH_H_ */
