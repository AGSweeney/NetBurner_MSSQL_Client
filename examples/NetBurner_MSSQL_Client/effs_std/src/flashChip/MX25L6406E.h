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

/*------------------------------------------------------------------------------
 * EFFS-STD configuration file for 8MB SPI flash chip used on the NANO54415 and
 * SB800EX.
 *
 * Macronix MX25L6406 8MB SPI Flash chip.
 *----------------------------------------------------------------------------*/

#ifndef _ONCHIPFLASH_H_
#define _ONCHIPFLASH_H_

#include "basictypes.h"
#include "hal.h"
#include "file/fsf.h"

// Function implemented
extern int fs_phy_OnChipFlash(FS_FLASH *flash);

#define FLASH_NAME "MX25L6406E"

/**
 * BLOCKSIZE
 * This defines the size of the blocks to be used in the file storage area.
 * This must be an erasable unit of the flash chip.  All blocks in the file
 * storage area must be the same size.  This may be different from the DESCSIZE
 * where the flash chip has different size erasable units available.
 *
 * SECTORSIZE
 * This defines the sector size.  Each block is divided into a number of
 * sectors.  This number is the smallest usable unit in the system and thus
 * represents the minimum file storage area.  For best usage of the flash
 * blocks, the sector size should always be a power of two.  See sector section
 * below for more information.
 *
 * SECTORPERBLOCK
 * This defines the number of sectors in a block.  It must always be true that:
 *
 *    SECTORPERBLOCK = BLOCKSIZE / SECTORSIZE
 *
 * The memory map below is for a module with a an 8MB SPI flash with a 4k sector size.
 * This example will allocate 1MB for the EFFS STD flash file system. There are a total
 * of 2048 4k sectors.
 *
 *                            Address In SPI Flash
 *                            -------
 *    ----------------------  0080 0000  (End of flash space)
 *    |  Config Reserve    |
 *    |  128k              |
 *    |--------------------|  007E 0000  (End of file system)
 *    |  File System Data  |
 *    |  896k - 128k desc  |
 *    |  4k x 192 Blocks   |
 *    |--------------------|  0072 0000  (Start of file system data)
 *    |  DESC BLOCKS 0/1   |
 *    |  128k              |
 *    |  4k x 32 Blocks    |
 *    |--------------------|  0070 0000  (Start of file system)
 *    |                    |
 *    |  Application       |
 *    |  ~7MB              |
 *    |                    |
 *    |--------------------|  0000 4000
 *    |  8k User Params    |
 *    |  4k x 2 Blocks     |
 *    |--------------------|  0000 2000
 *    |  8k Global config  |
 *    |  4k x 4 Blocks     |
 *    |--------------------|  0000 0000  (Start of flash space)
 */

/**
 * Start of flash memory base address.
 */
//#define FS_FLASHBASE   ( 0x00000000 )

/*
 * CHANGES TO COMPCODE FLAGS
 * In NBEclipse, or your command line makefile, change the following line
 * so the application will only occupy the specified application space.
 * The first parameter is the start of application space, and the second
 * is the address just below the file system space.
 *
 *    COMPCODEFLAGS = 0x04000 0x800000      // Original
 *
 *    COMPCODEFLAGS = 0x04000 0x700000      // Space for file system
 */

#define BLOCKSIZE (4 * 1024)   // Flash physical "sector" size, smallest erasable unit
#define SECTORSIZE (512)       // Flash sector size in bytes, smallest usable unit
#define SECTORPERBLOCK (BLOCKSIZE / SECTORSIZE)

/*
 * Specify the total amount of flash memory in the system, and the amount
 * allocated to be used by the file system (the rest is used by the
 * application).
 */
/* Example for a 1MB file system */
#define FLASH_SIZE (8 * 1024 * 1024)                           // Total flash space
#define CONFIG_RESERVE (128 * 1024)                            // Reserved at end of flash for system config
#define FS_SIZE (1 * 1024 * 1024 - CONFIG_RESERVE)             // Amount of Flash allocated to file system (896KB)
#define FIRST_ADDR (FLASH_SIZE - FS_SIZE - CONFIG_RESERVE)     // First Flash file system address (0x700000)
#define BLOCKSTART (2 * (DESCSIZE / BLOCKSIZE))                // First block where file system data starts, after blocks reserved for Descriptors

/*
 * Descriptor Blocks:
 * These blocks contain critical information about the file system, block
 * allocation, wear information, and file/directory information.  At least two
 * descriptor blocks must be included in the system, which can be erased
 * independently.  An optional descriptor write cache may be configured which
 * improves the performance of the file system.  Please refer to the EFFS-STD
 * implementation guide for additional information.
 */
/* Example for a 1MB file system */
#define DESCSIZE (64 * 1024)   // Size of one descriptor, 8 BLOCKS
#define DESCBLOCKSTART (0)     // Position of first descriptor
#define DESCBLOCKEND (1)       // Position of last descriptor
#define DESCCACHE (1024)

#endif /* _ONCHIPFLASH_H_ */
