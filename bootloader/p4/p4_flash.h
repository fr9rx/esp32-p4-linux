/* Flash reading and partition-table walking for the ESP32-P4 bootloader.
 *
 * No driver here: the ROM's SPI flash routines are already configured (they
 * are how the first stage loaded us), and we reach them through the pinned
 * ROM linker script with no code vendored. This file is the partition table
 * parser and a streaming CRC on top of them.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Partition types, matching partitions.csv.
 *
 * ESP-IDF reserves type 0x00 for app images and 0x01 for its own data
 * flavours, and leaves 0x40..0xFE for the system that owns the flash. A Linux
 * kernel is neither, so both of ours are custom types with subtype 0. */
#define P4_PART_TYPE_KERNEL   0x40
#define P4_PART_TYPE_DTB      0x41
#define P4_PART_SUBTYPE_NONE  0x00

typedef struct {
    uint32_t offset;
    uint32_t size;
    char     label[17];   /* 16 on-flash bytes, plus room to terminate */
} p4_partition_t;

/* Tell the ROM how big the flash chip actually is. Call before any other
 * function here.
 *
 * NOT OPTIONAL, and the failure is entertainingly specific: the ROM keeps a
 * chip descriptor whose chip_size the first stage leaves at 2 MB, and
 * esp_rom_spiflash_read() range-checks every read against it. Reads below
 * 0x200000 succeed; the first one at or above it returns
 * ESP_ROM_SPIFLASH_RESULT_ERR. On this board that meant the partition table
 * (0x8000) parsed perfectly and the kernel CRC died 1.875 MB in, at flash
 * 0x00200000 exactly.
 *
 * ESP-IDF does the same assignment in bootloader_flash_update_size(), called
 * from bootloader_init_spi_flash() -- part of the init sequence we took over
 * when we replaced the second stage. */
bool p4_flash_init(void);

/* Are flash reads actually repeatable right now?
 *
 * CRCs the same region twice and compares. Sounds paranoid; it is not.
 * After a WARM reset -- an MWDT core reset, which is how `reboot` works on
 * this SoC -- the MSPI block keeps the previous boot's PSRAM tuning, and bulk
 * reads come back wrong. Worse, they come back DIFFERENTLY wrong each time:
 *
 *   cold boot   kernel crc32 0xc52ac046   (correct, repeatable)
 *   after reboot            0x20fa9b69
 *   after reboot            0x78e615df    same flash, same code
 *
 * so the reads are unstable rather than merely shifted. A single CRC cannot
 * tell that from a legitimately different kernel; two disagreeing CRCs of the
 * same bytes can only mean the reads themselves are unreliable.
 *
 * Returns true if the two passes agree. See p4_flash.c for what is and is not
 * understood about the underlying cause. */
bool p4_flash_selftest(void);

/* Read from flash into SRAM.
 *
 * offset, dest and len must all be 4-byte aligned: that is the ROM routine's
 * contract, not ours (esp_rom_spiflash.h -- "should be 4 bytes aligned" on
 * both the address and the length), and it is not checked by the ROM. An
 * unaligned call returns false here rather than reading something adjacent. */
bool p4_flash_read(uint32_t offset, void *dest, uint32_t len);

/* Find a partition by type and subtype. Returns false if the table has no
 * such entry, or if the table itself does not parse. */
bool p4_part_find(uint8_t type, uint8_t subtype, p4_partition_t *out);

/* Print the whole table. Cheap, and it is the difference between "partition
 * not found" and knowing whether the table was ever written to flash. */
void p4_part_dump(void);

/* CRC32 a flash region without staging all of it.
 *
 * Standard CRC-32 (the zlib/PNG one), computed by the ROM's table-driven
 * crc32_le, so `python flashmap.py crc ...` on the host produces the same
 * number for the same bytes.
 *
 * `len` may be any length; the region is read in aligned chunks and only len
 * bytes are folded in. */
bool p4_flash_crc32(uint32_t offset, uint32_t len, uint32_t *out_crc);

/* Copy a flash region into memory -- in practice into mapped PSRAM.
 *
 * On return the destination range has been written back out of the cache and
 * invalidated, so a read afterwards comes from PSRAM itself rather than from
 * our own dirty lines. That is what makes the CRC that follows a real check
 * of the copy instead of a check of the cache.
 *
 * dest and len must be 4-byte aligned, as for p4_flash_read(). */
bool p4_flash_copy(uint32_t offset, uint32_t len, void *dest);

/* CRC32 of a memory range, same convention as p4_flash_crc32(): comparable
 * to zlib.crc32 and to the numbers `make crc` prints. */
uint32_t p4_mem_crc32(const void *addr, uint32_t len);
