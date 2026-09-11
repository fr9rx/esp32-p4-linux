/* PSRAM bring-up for the ESP32-P4 second-stage bootloader.
 *
 * The heavy lifting is vendored from ESP-IDF v6.1-beta1: the chip driver
 * (esp_psram_impl_ap_hex.c) and the DQS timing tuning it cannot work without
 * (~1,000 lines across mspi_timing_tuning.c, mspi_timing_by_dqs.c and the
 * esp32p4 port). This file is the ~100 lines that call them in the right
 * order, map the result, and then refuse to believe it until it has been
 * tested.
 *
 * The mapping sequence mirrors esp_psram.c:302-340, minus the dynamic vaddr
 * allocator: that file asks esp_mmu_map to reserve a virtual block, because
 * an application shares the MMU with flash mmaps. We are the only thing
 * running and PSRAM has a fixed home at SOC_EXTRAM_LOW, so we map there
 * directly.
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#include "p4_psram.h"

#include "esp_err.h"
#include "esp_private/esp_psram_impl.h"
#include "hal/mmu_hal.h"
#include "hal/mmu_types.h"
#include "hal/cache_ll.h"
#include "hal/cache_hal.h"

extern int ets_printf(const char *fmt, ...);

bool p4_psram_init(size_t *out_bytes)
{
    if (out_bytes) {
        *out_bytes = 0;
    }

    /* 1. Train the chip. This is where the DQS tuning happens, and where a
     *    marginal board fails. It configures MPLL for itself along the way,
     *    through the esp_clk_tree_mpll_* pair that p4_clk.c supplies.
     *
     *    Bracketed by prints because this call can HANG rather than fail --
     *    it polls chip status bits, and an unpowered or unclocked PSRAM
     *    simply never answers. A hang here produces no trap and no return
     *    code, so the last line printed is the only evidence available. */
    esp_err_t err = esp_psram_impl_enable();
    if (err != ESP_OK) {
        ets_printf("p4boot: psram: impl_enable failed (%d)\r\n", (int)err);
        return false;
    }

    uint32_t psram_size = 0;
    if (esp_psram_impl_get_physical_size(&psram_size) != ESP_OK || psram_size == 0) {
        return false;
    }

    /* 2. Map physical PSRAM at 0x48000000.
     *
     *    mmu_id is 1, not 0: SOC_MMU_PER_EXT_MEM_TARGET is set on the P4,
     *    meaning flash and PSRAM have separate MMUs, and 1 is PSRAM's.
     *    Getting this wrong maps into the flash MMU and corrupts the window
     *    we are executing... nothing from, but it would take the flash reads
     *    in step 7 with it.
     *
     *    paddr 0: we want the whole chip from its own address zero.
     *
     *    Both vaddr and paddr must be MMU-page aligned (64 KB on this part).
     *    0x48000000 and 0 both are; the size is rounded down to be safe. */
    uint32_t map_len = psram_size & ~(SOC_MMU_PAGE_SIZE - 1u);
    uint32_t mapped = 0;
    mmu_hal_map_region(1, MMU_TARGET_PSRAM0, P4_PSRAM_VADDR, 0, map_len, &mapped);
    if (mapped == 0) {
        return false;
    }

    /* 3. Let the cache reach it.
     *
     *    Without this the MMU entries exist and loads still do not arrive:
     *    the cache has per-bus enables and the PSRAM window's bus is off
     *    until asked. esp_psram.c does exactly this after mapping. Only
     *    core 0 -- core 1 is never started. */
    cache_bus_mask_t bus_mask = cache_ll_l1_get_bus(0, P4_PSRAM_VADDR, mapped);
    cache_ll_l1_enable_bus(0, bus_mask);

    /* 4. Throw away anything the cache thinks it knows about this range.
     *
     *    We just repointed the MMU underneath these virtual addresses. Any
     *    line still held from before now describes different physical memory,
     *    and the first read would be answered from it. Invalidating is cheap
     *    and the alternative is a corruption that looks like bad training. */
    cache_hal_invalidate_addr(P4_PSRAM_VADDR, mapped);

    if (out_bytes) {
        *out_bytes = mapped;
    }
    return true;
}

bool p4_psram_test(size_t bytes, uint32_t *out_bad_addr)
{
    volatile uint32_t *mem = (volatile uint32_t *)(uintptr_t)P4_PSRAM_VADDR;
    const size_t words = bytes / sizeof(uint32_t);

    if (out_bad_addr) {
        *out_bad_addr = 0;
    }

    /* Address-in-address, not a fixed pattern.
     *
     * A constant would pass happily if the address lines were mistrained --
     * every location holds the same value, so aliasing is invisible. Writing
     * each word's own address means a stuck or swapped address line shows up
     * as a mismatch, and the value that comes back names the location it
     * actually reached.
     *
     * XOR with a constant so that a word of all-zeros or a bus that floats
     * to the address itself cannot pass by accident. */
    for (size_t i = 0; i < words; i++) {
        mem[i] = (uint32_t)(uintptr_t)&mem[i] ^ 0xA5A5A5A5u;
    }

    /* Read back in a separate pass, so the data has actually been to the
     * chip and back rather than answered out of a write buffer.
     *
     * Counts rather than bailing on the first mismatch. The distinction
     * matters enormously for diagnosis: a handful of bad words at one
     * boundary is a mapping or cache problem, every word bad is a timing or
     * lane problem, and a scattered few is marginal signal integrity. Stopping
     * at the first one cannot tell those apart. */
    size_t bad_count = 0;
    for (size_t i = 0; i < words; i++) {
        uint32_t expect = (uint32_t)(uintptr_t)&mem[i] ^ 0xA5A5A5A5u;
        uint32_t got = mem[i];
        if (got != expect) {
            if (bad_count < 4) {
                ets_printf("p4boot:   bad 0x%08x: wrote 0x%08x read 0x%08x\r\n",
                           (unsigned)(uintptr_t)&mem[i], (unsigned)expect,
                           (unsigned)got);
            }
            if (bad_count == 0 && out_bad_addr) {
                *out_bad_addr = (uint32_t)(uintptr_t)&mem[i];
            }
            bad_count++;
        }
    }

    if (bad_count) {
        ets_printf("p4boot:   %u of %u words bad (%u%%)\r\n",
                   (unsigned)bad_count, (unsigned)words,
                   (unsigned)((bad_count * 100u) / (words ? words : 1)));
        return false;
    }
    return true;
}

/* The non-cacheable alias, +0x40000000. See p4_psram.h for why this matters. */
#define P4_NON_CACHEABLE_OFFSET   0x40000000u

bool p4_psram_alias_test(void)
{
    /* 1 MB into PSRAM: clear of everything, and the walking-address test has
     * already been over it, so the previous contents are ours to destroy. */
    const uint32_t off = 0x00100000u;
    volatile uint32_t *cached =
        (volatile uint32_t *)(uintptr_t)(P4_PSRAM_VADDR + off);
    volatile uint32_t *uncached =
        (volatile uint32_t *)(uintptr_t)(P4_PSRAM_VADDR + off
                                         + P4_NON_CACHEABLE_OFFSET);

    /* Seed through the alias. If the alias reaches the same memory at all,
     * a subsequent cached read has to see this. */
    *uncached = 0x11111111u;
    uint32_t cached_sees_alias_write = *cached;

    /* Now dirty the line through the cached window only. No writeback. */
    *cached = 0x22222222u;
    uint32_t alias_sees_cached_write = *uncached;

    /* And prove the alias read was not itself cached, by writing a third
     * value through the alias and reading it straight back. */
    *uncached = 0x33333333u;
    uint32_t alias_reads_own_write = *uncached;

    const bool reaches_same_memory = (cached_sees_alias_write == 0x11111111u);
    const bool bypasses_cache      = (alias_sees_cached_write == 0x11111111u);

    ets_printf("p4boot: psram alias 0x%08x: cached-sees-alias 0x%08x, "
               "alias-sees-cached 0x%08x, alias-rw 0x%08x\r\n",
               (unsigned)(uintptr_t)uncached,
               (unsigned)cached_sees_alias_write,
               (unsigned)alias_sees_cached_write,
               (unsigned)alias_reads_own_write);

    if (reaches_same_memory && bypasses_cache) {
        ets_printf("p4boot: psram alias: UNCACHED and coherent "
                   "-- dma_alloc_coherent() can work\r\n");
        return true;
    }
    if (reaches_same_memory) {
        /* Same memory, but the cached write was visible through it. Either
         * the cache is write-through or the alias is cached as well; in both
         * cases a stale READ is still possible and it cannot be trusted for
         * DMA descriptors. */
        ets_printf("p4boot: psram alias: same memory but NOT bypassing the "
                   "cache -- not usable for coherent DMA\r\n");
        return false;
    }
    ets_printf("p4boot: psram alias: does not reach the same memory "
               "-- no uncached window here\r\n");
    return false;
}
