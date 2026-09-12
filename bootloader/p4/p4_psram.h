/* PSRAM bring-up for the ESP32-P4 second-stage bootloader. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Where PSRAM appears once mapped. Fixed by the SoC address map
 * (SOC_EXTRAM_LOW), and the same base the guest device tree declares as
 * memory@48000000. */
#define P4_PSRAM_VADDR   0x48000000u

/* Train the PSRAM chip and map it into the address space.
 *
 * Two distinct things, and the second is easy to miss: esp_psram_impl_enable()
 * brings up the chip and the MSPI controller but leaves nothing addressable.
 * Until mmu_hal_map_region() runs, a load from 0x48000000 does not reach
 * PSRAM.
 *
 * On success *out_bytes holds the mapped size. Returns false if the chip did
 * not come up or nothing could be mapped.
 *
 * Must run BEFORE the CPU is raised to its final clock. This header used to
 * say the opposite, on the reasoning that MSPI timing tables are indexed by
 * core clock -- true of the table-driven tuning schemes, but the P4's PSRAM
 * uses the DQS sweep in mspi_timing_by_dqs.c, which has no tables. ESP-IDF's
 * own order settles it: esp_psram_chip_init() at cpu_start.c:645,
 * esp_clk_init() at 830. Training at 360 MHz produced a PSRAM that answered
 * every read with the halfwords swapped.
 *
 * The CPU must then be raised BEFORE anything writes a burst here. At
 * 200 MHz a 40 MHz core cannot feed a burst write and the write channel
 * wedges permanently, silently, on the first cache-line writeback. So the
 * window for the raise is narrow: after this call, before p4_psram_test().
 * main.c holds the measurements and does it in that order. */
bool p4_psram_init(size_t *out_bytes);

/* Walking-ones test over the whole mapped window.
 *
 * The single most valuable check in this bootloader. A mistrained DQS does
 * not report itself -- PSRAM answers, just with the wrong bits, and every
 * later symptom (a kernel that faults in a different place each boot) points
 * anywhere except here.
 *
 * On failure *out_bad_addr holds the first address that read back wrong. */
bool p4_psram_test(size_t bytes, uint32_t *out_bad_addr);

/* Does the +0x40000000 non-cacheable alias actually bypass the cache?
 *
 * ESP-IDF says both PSRAM and internal SRAM are aliased uncached at
 * +0x40000000 (SOC_NON_CACHEABLE_OFFSET_{PSRAM,SRAM} in soc/soc.h, commented
 * "non-cacheable offset for memory behind the cache"). If that is true, this
 * platform can have a working dma_alloc_coherent() -- the CPU takes the
 * uncached alias and the device takes the real address -- and no driver needs
 * hand-written descriptor cache maintenance ever again.
 *
 * It is far too load-bearing an assumption to take on trust, and it cannot be
 * tested from Linux: /dev/mem refuses anything past high_memory, so a read of
 * 0x88000000 returns EFAULT from valid_phys_addr_range() before it reaches
 * the bus. Here we are in M-mode with the whole address space.
 *
 * The test writes through the alias, reads back through the cached window,
 * then writes through the cached window and reads through the alias WITHOUT
 * any cache maintenance in between. That last read is the proof: if the
 * cached write is invisible through the alias, the alias is genuinely not
 * going through the cache.
 *
 * Runs before the kernel is copied into PSRAM, so it may scribble freely.
 * Reports and returns; nothing here is fatal, because a false result means
 * "keep doing manual cache maintenance", not "do not boot". */
bool p4_psram_alias_test(void);
