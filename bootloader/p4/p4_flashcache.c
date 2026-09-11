/* spi_flash_disable_cache() / spi_flash_restore_cache() for the bootloader.
 *
 * The MSPI timing tuning in vendor/src calls these around the window where it
 * rewrites MSPI timing registers. Flash and PSRAM share that controller, so
 * on a normal ESP-IDF system -- where the code doing the tuning is itself
 * executing out of flash through the cache -- the cache must be stood down
 * first or the next instruction fetch reads through a controller mid-retune.
 *
 * Upstream's versions live in spi_flash/cache_utils.c, an application-side
 * file that NON_OS_BUILD excludes, so we have to supply something.
 *
 * ON THIS CHIP, THE RIGHT SOMETHING IS NOTHING. Two facts combine:
 *
 *   1. soc_caps.h: SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE = 1. Internal SRAM is
 *      reached THROUGH the L1 cache on the P4, not around it.
 *   2. This bootloader is entirely SRAM-resident -- `make size` shows every
 *      section inside 0x4ff0xxxx -- and touches no flash between the start
 *      of tuning and its end.
 *
 * So suspending the cache does not protect anything here, and it does stop
 * our own instruction fetch. An earlier version of this file called
 * cache_hal_suspend(1, CACHE_TYPE_ALL) to be faithful to upstream, and
 * mspi_timing_psram_tuning() hung on the spot -- no trap, no message, because
 * the CPU never retired another instruction.
 *
 * If anything in this bootloader ever starts executing from or reading flash
 * during tuning, this reasoning expires. Nothing does today: the kernel copy
 * at step 7 happens long after PSRAM is up, and it uses the ROM's SPI
 * routines rather than a mapped window.
 */

#include <stdint.h>

void spi_flash_disable_cache(uint32_t cpuid, uint32_t *saved_state)
{
    (void)cpuid;    /* shared cache on this part; the argument is vestigial */

    if (saved_state) {
        *saved_state = 0;
    }
}

void spi_flash_restore_cache(uint32_t cpuid, uint32_t saved_state)
{
    (void)cpuid;
    (void)saved_state;
}
