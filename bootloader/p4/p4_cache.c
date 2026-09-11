/* Cache and MMU initialisation for the ESP32-P4 second-stage bootloader.
 *
 * Mirrors bootloader_init_ext_mem() from ESP-IDF v6.1-beta1
 * (components/bootloader_support/src/bootloader_init.c:139), minus the flash
 * MMU work that follows it there -- we read flash through the ROM's SPI
 * routines rather than by mapping it.
 *
 * WHY: both HALs carry configuration state that their other entry points
 * read. cache_hal_init() records the core count and the L2 geometry;
 * mmu_hal_init() records the page size and resets the entries. Skip them and
 * nothing fails at the call -- but the first cache_hal_suspend() or
 * mmu_hal_map_region() then runs against zeroed configuration.
 *
 * That is not hypothetical. It is why esp_psram_impl_enable() hung: the MSPI
 * timing tuning calls spi_flash_disable_cache() -> cache_hal_suspend(), and
 * with an uninitialised cache HAL that never came back.
 */

#include <stdint.h>

#include "p4_cache.h"
#include "sdkconfig.h"

#include "hal/cache_hal.h"
#include "hal/mmu_hal.h"

void p4_cache_init(void)
{
    cache_hal_config_t cache_config = {
        /* One core. The second HP core is never started -- see
         * docs/MULTICORE.md for why Linux does not use it either. */
        .core_nums = 1,
        .l2_cache_size = CONFIG_CACHE_L2_CACHE_SIZE,
        .l2_cache_line_size = CONFIG_CACHE_L2_CACHE_LINE_SIZE,
    };
    cache_hal_init(&cache_config);

    mmu_hal_config_t mmu_config = {
        .core_nums = 1,
        .mmu_page_size = CONFIG_MMU_PAGE_SIZE,
    };
    mmu_hal_init(&mmu_config);
}
