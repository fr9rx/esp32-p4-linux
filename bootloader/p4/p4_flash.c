/* Flash reading and partition-table walking.
 *
 * WHERE THIS RUNS IN THE BOOT FLOW, AND WHY IT MATTERS
 *
 * main.c calls this BEFORE PSRAM comes up, which is earlier than the build
 * order in the plan suggests. The reason is that flash and PSRAM share the
 * MSPI controller: esp_psram_impl_enable() runs the DQS tuning, which
 * rewrites MSPI timing registers, and the ROM's flash routines are driving
 * the same block through SPI1. Reading the table first means these reads
 * happen with the controller exactly as the ROM left it -- 40 MHz DIO,
 * configured well enough to have loaded us -- so a failure here is a bug in
 * this file and nothing else.
 *
 * The payload copy at step 8 necessarily happens after PSRAM is up, because
 * PSRAM is where it copies to. If flash reads survive the tuning, step 8's
 * CRC matches the one step 7 computed beforehand; if the tuning disturbs the
 * flash side, step 8 mismatches and this comment says where to look. That
 * split is deliberate: it turns an untested assumption into a checked one.
 *
 * NO FLASH DRIVER HERE. esp_rom_spiflash_read() lives at 0x4fc00158 and comes
 * from the pinned ROM linker script; the first stage already configured the
 * chip through it. IDF's bootloader re-does that configuration in
 * bootloader_init_spi_flash() to change mode and speed. We keep 40 MHz DIO --
 * sdkconfig.h says why: flash speed only affects boot time, since the rootfs
 * is on the SD card, and raising it couples us to the PSRAM tuning that is
 * already the riskiest step.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "p4_flash.h"

#include "esp_rom_spiflash.h"
#include "hal/cache_hal.h"
#include "esp_private/mspi_timing_tuning.h"

extern int   ets_printf(const char *fmt, ...);
extern void *memcpy(void *dst, const void *src, size_t n);

/* The ROM's table-driven CRC-32, reached as esp_rom_crc32_le through
 * vendor/ld/esp32p4.rom.api.ld, which carries
 *
 *     PROVIDE ( esp_rom_crc32_le = crc32_le );
 *
 * It inverts on the way in and on the way out, which is what makes chaining
 * work: crc32_le(crc32_le(0, a, na), b, nb) equals crc32_le(0, ab, na + nb).
 * Starting from 0 gives the ordinary zlib/PNG CRC-32, so the number printed
 * here is directly comparable to what flashmap.py computes with zlib.crc32.
 */
extern uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t *buf, uint32_t len);

/* Fixed by convention and by the ROM first-stage, which looks here. */
#define P4_PART_TABLE_OFFSET   0x8000u
#define P4_PART_TABLE_LEN      0x1000u
#define P4_PART_MAX_ENTRIES    (P4_PART_TABLE_LEN / 32u)

#define P4_PART_MAGIC          0x50AAu   /* a real entry */
#define P4_PART_MAGIC_MD5      0xEBEBu   /* the trailing checksum entry */

/* ESP-IDF's on-flash entry, unchanged. This is a wire format we read, not a
 * struct we own, so the field order and widths are not ours to choose.
 *
 * Not marked packed: every field already sits at its natural alignment and
 * the compiler inserts nothing. The assertion below is what guarantees that,
 * rather than trusting the arithmetic -- padding here would misread every
 * entry after the first.
 */
typedef struct {
    uint16_t magic;
    uint8_t  type;
    uint8_t  subtype;
    uint32_t offset;
    uint32_t size;
    uint8_t  label[16];
    uint32_t flags;
} p4_part_entry_t;

_Static_assert(sizeof(p4_part_entry_t) == 32,
               "partition entry must be exactly 32 bytes");

/* Staging buffer for the CRC pass. In .bss rather than on the stack: 4 KB is
 * a lot of stack for a bootloader, and a static is naturally word-aligned,
 * which the ROM read requires. 4 KB is also the flash sector size, so a chunk
 * never straddles more sectors than it must.
 */
static uint32_t s_buf[1024];

/* The size of the part on this board, confirmed by `espflash board-info`
 * ("Flash size: 16MB") and by the header espflash writes into our own image
 * (FLASH_SIZE in the Makefile).
 *
 * A constant rather than a JEDEC probe. Detection would mean an RDID through
 * esp_rom_spiflash_read_user_cmd() and trusting the byte order of what comes
 * back; this bootloader targets one board, the number is checked against the
 * chip at flash time by espflash, and a wrong value here fails loudly at the
 * first read past the end rather than silently. If this ever runs on a board
 * with different flash, `make info` prints the right number. */
#define P4_FLASH_SIZE   (16u * 1024u * 1024u)

bool p4_flash_init(void)
{
    uint32_t was = g_rom_flashchip.chip_size;

    /* Put the MSPI back the way the ROM had it, BEFORE reading any flash.
     *
     * A warm reset does not reset this block. An MWDT core reset -- which is
     * how `reboot` works on this SoC, the watchdog driver owning the machine
     * restart handler -- restarts the CPU while leaving the MSPI timing
     * registers holding whatever the PREVIOUS boot's PSRAM tuning wrote into
     * them. The ROM first-stage still manages to load us, but our own bulk
     * reads then come back subtly wrong, and the failure is silent:
     *
     *   cold boot:  kernel crc32 0xc52ac046   (correct)
     *   `reboot`:   kernel crc32 0x20fa9b69   (same flash, same code)
     *
     * The partition table survives -- it is one 32-byte read at a time -- so
     * the boot looks healthy right up until a corrupt kernel is copied into
     * PSRAM and jumped to.
     *
     * This is the same call ESP-IDF makes before tuning, and it does exactly
     * what is needed: flash clock source back to FLASH_CLK_SRC_ROM_DEFAULT
     * and both flash and PSRAM tuning registers cleared. control_spi1 = true
     * because SPI1 is the path the ROM's read routines drive, and the
     * din_num/din_mode registers are shared between SPI0 and SPI1 -- clearing
     * one without the other is the case IDF's own comment warns about.
     *
     * The cost is that flash drops to 20 MHz for our reads. Boot takes a
     * little longer; it is not a trade worth thinking about twice against
     * silently loading a corrupt kernel. */
    mspi_timing_enter_low_speed_mode(true);

    g_rom_flashchip.chip_size = P4_FLASH_SIZE;

    /* Printed rather than assumed. `was` should read 0x00200000; if a future
     * ROM already knows the real size this line says so, and if the pointer
     * at rom_spiflash_legacy_data were wrong for this silicon the value would
     * be obvious nonsense instead of a mysterious read failure later. */
    ets_printf("p4boot: flash: chip_size %u MB -> %u MB\r\n",
               (unsigned)(was / (1024u * 1024u)),
               (unsigned)(P4_FLASH_SIZE / (1024u * 1024u)));

    return g_rom_flashchip.chip_size == P4_FLASH_SIZE;
}

bool p4_flash_read(uint32_t offset, void *dest, uint32_t len)
{
    /* The ROM checks none of this and reads whatever the misalignment lands
     * on. Three cheap tests beat one wrong buffer. */
    if ((offset & 3u) || (len & 3u) || (((uintptr_t)dest) & 3u)) {
        ets_printf("p4boot: flash: unaligned read 0x%08x -> 0x%08x len %u\r\n",
                   (unsigned)offset, (unsigned)(uintptr_t)dest, (unsigned)len);
        return false;
    }

    esp_rom_spiflash_result_t r =
        esp_rom_spiflash_read(offset, (uint32_t *)dest, (int32_t)len);
    if (r != ESP_ROM_SPIFLASH_RESULT_OK) {
        ets_printf("p4boot: flash: read 0x%08x len %u failed (%d)\r\n",
                   (unsigned)offset, (unsigned)len, (int)r);
        return false;
    }
    return true;
}

/* Walk the table, handing every real entry to `visit` until it says stop.
 *
 * One entry read at a time rather than the whole 4 KB table: it needs no
 * second buffer, and it stops at the terminator. For our two-partition table
 * that is four 32-byte reads, not one 4 KB read.
 */
static bool part_walk(bool (*visit)(const p4_part_entry_t *e, void *ctx), void *ctx)
{
    for (uint32_t i = 0; i < P4_PART_MAX_ENTRIES; i++) {
        p4_part_entry_t e;

        if (!p4_flash_read(P4_PART_TABLE_OFFSET + i * sizeof(e), &e, sizeof(e))) {
            return false;
        }

        /* The MD5 entry sits after the real ones and is not a partition. We
         * do not verify it: that would need an MD5 implementation, and the
         * CRC32 of each payload is a stronger check on the thing we actually
         * care about. Skipping rather than stopping keeps this correct if a
         * future table ever places entries after it. */
        if (e.magic == P4_PART_MAGIC_MD5) {
            continue;
        }

        /* Erased flash reads 0xFFFF here, which is the ordinary end of the
         * table. Anything else non-magic means the table is damaged, and
         * either way there is nothing further to read. */
        if (e.magic != P4_PART_MAGIC) {
            return true;
        }

        if (visit && !visit(&e, ctx)) {
            return true;    /* the visitor found what it wanted */
        }
    }
    return true;
}

/* --- find ---------------------------------------------------------------- */

typedef struct {
    uint8_t         type;
    uint8_t         subtype;
    p4_partition_t *out;
    bool            found;
} find_ctx_t;

static bool find_visit(const p4_part_entry_t *e, void *ctx)
{
    find_ctx_t *f = (find_ctx_t *)ctx;

    if (e->type != f->type || e->subtype != f->subtype) {
        return true;    /* keep walking */
    }

    f->out->offset = e->offset;
    f->out->size   = e->size;
    memcpy(f->out->label, e->label, 16);
    f->out->label[16] = 0;
    f->found = true;
    return false;       /* stop */
}

bool p4_part_find(uint8_t type, uint8_t subtype, p4_partition_t *out)
{
    find_ctx_t f = { .type = type, .subtype = subtype, .out = out, .found = false };

    if (!out || !part_walk(find_visit, &f)) {
        return false;
    }
    return f.found;
}

/* --- dump ---------------------------------------------------------------- */

static bool dump_visit(const p4_part_entry_t *e, void *ctx)
{
    char label[17];

    (void)ctx;
    memcpy(label, e->label, 16);
    label[16] = 0;

    /* Label last, so the columns line up without a field width. The ROM's
     * printf documents only "float and long long are not supported", but it
     * is a reduced implementation and the only flag this project has actually
     * watched work on hardware is %08x's zero pad. Left-justification is
     * probably fine and is not worth finding out the hard way over column
     * alignment. */
    ets_printf("p4boot:   type 0x%02x sub 0x%02x  0x%08x + 0x%08x  %u KB  %s\r\n",
               (unsigned)e->type, (unsigned)e->subtype,
               (unsigned)e->offset, (unsigned)e->size,
               (unsigned)(e->size / 1024u), label);
    return true;
}

void p4_part_dump(void)
{
    ets_printf("p4boot: partition table at 0x%08x:\r\n",
               (unsigned)P4_PART_TABLE_OFFSET);
    if (!part_walk(dump_visit, NULL)) {
        ets_printf("p4boot:   <table unreadable>\r\n");
    }
}

/* --- crc ----------------------------------------------------------------- */

bool p4_flash_crc32(uint32_t offset, uint32_t len, uint32_t *out_crc)
{
    uint32_t crc = 0;
    uint32_t done = 0;
    uint32_t next_mark = 1u << 20;

    if (!out_crc || (offset & 3u)) {
        return false;
    }

    while (done < len) {
        uint32_t want = len - done;
        if (want > sizeof(s_buf)) {
            want = sizeof(s_buf);
        }

        /* The ROM reads whole words only, so a final partial chunk is read
         * rounded up and folded in short. That never happens for a partition,
         * whose size flashmap.py forces to a whole number of 4 KB sectors,
         * but it keeps the function correct for any caller. */
        uint32_t rd = (want + 3u) & ~3u;
        if (!p4_flash_read(offset + done, s_buf, rd)) {
            return false;
        }

        crc = esp_rom_crc32_le(crc, (const uint8_t *)s_buf, want);
        done += want;

        /* A marker every megabyte. Six megabytes read at 40 MHz DIO and
         * CRC'd with the CPU still at 40 MHz is a couple of seconds of
         * otherwise total silence, and on this board silence is
         * indistinguishable from a hang. */
        if (done >= next_mark) {
            ets_printf(".");
            next_mark += 1u << 20;
        }
    }

    if (next_mark > (1u << 20)) {
        ets_printf("\r\n");
    }

    *out_crc = crc;
    return true;
}

/* --- copy ---------------------------------------------------------------- */

uint32_t p4_mem_crc32(const void *addr, uint32_t len)
{
    return esp_rom_crc32_le(0, (const uint8_t *)addr, len);
}

bool p4_flash_copy(uint32_t offset, uint32_t len, void *dest)
{
    uint8_t *out = (uint8_t *)dest;
    uint32_t done = 0;
    uint32_t next_mark = 1u << 20;

    if ((((uintptr_t)dest) & 3u) || (offset & 3u)) {
        ets_printf("p4boot: flash: unaligned copy 0x%08x -> 0x%08x\r\n",
                   (unsigned)offset, (unsigned)(uintptr_t)dest);
        return false;
    }

    /* Straight from flash into PSRAM, with no staging buffer in between.
     *
     * The ROM read is CPU-driven -- it moves words out of the SPI FIFO with
     * ordinary stores -- so the destination can be any writable mapping,
     * PSRAM through the cache included. Staging through SRAM would double
     * every byte's journey for nothing.
     *
     * A whole partition at a time, not just the payload, so that the CRC
     * below is directly comparable to the one step 7 took over flash. */
    while (done < len) {
        uint32_t want = len - done;
        if (want > sizeof(s_buf)) {
            want = sizeof(s_buf);
        }
        if (!p4_flash_read(offset + done, out + done, want)) {
            return false;
        }
        done += want;

        if (done >= next_mark) {
            ets_printf(".");
            next_mark += 1u << 20;
        }
    }
    if (next_mark > (1u << 20)) {
        ets_printf("\r\n");
    }

    /* Push the copy out of the cache and then forget it.
     *
     * Two separate needs. Writeback: the stores above left dirty L1 lines,
     * and PSRAM does not hold the data until they drain -- the same hazard
     * that makes Cache_WriteBack_All() mandatory before jumping to a payload
     * we just wrote. Invalidate: without it the CRC below would be answered
     * out of those very lines, and would pass even if nothing reached the
     * chip. Order matters -- invalidating first would discard the copy. */
    cache_hal_writeback_addr((uint32_t)(uintptr_t)dest, len);
    cache_hal_invalidate_addr((uint32_t)(uintptr_t)dest, len);
    return true;
}

/* --- self test ------------------------------------------------------------ */

bool p4_flash_selftest(void)
{
    /* 64 KB is enough to be convincing and cheap enough to be unconditional.
     * Read from the kernel partition rather than address 0: the bootloader's
     * own image lives at 0x2000 and was demonstrably readable (we are running
     * from it), so it is the least informative region to test. */
    const uint32_t off = 0x20000u;
    const uint32_t len = 0x10000u;
    uint32_t a = 0;
    uint32_t b = 0;

    /* Announced before it runs, not after. After a warm reset the reads do
     * not merely come back wrong -- they can stall inside the ROM routine and
     * never return, and a hang whose last console line is about something
     * else is a genuinely hard thing to interpret. This line costs nothing
     * and names exactly what is stuck. */
    ets_printf("p4boot: flash: checking read stability...\r\n");

    if (!p4_flash_crc32(off, len, &a) || !p4_flash_crc32(off, len, &b)) {
        return false;
    }
    if (a != b) {
        ets_printf("p4boot: flash: READS ARE UNSTABLE -- "
                   "same 64 KB read twice gave 0x%08x then 0x%08x\r\n",
                   (unsigned)a, (unsigned)b);
        return false;
    }
    ets_printf("p4boot: flash: reads are stable (crc32 0x%08x twice)\r\n",
               (unsigned)a);
    return true;
}
