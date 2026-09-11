/* Cache and MMU initialisation for the ESP32-P4 second-stage bootloader. */
#pragma once

/* Initialise the cache and MMU HALs.
 *
 * Both keep internal state -- how many cores, the L2 line size, the MMU page
 * size -- that every later cache_hal_* and mmu_hal_* call reads. ESP-IDF does
 * this in bootloader_init_ext_mem(); skipping it does not fail at the call
 * site, it makes the first *use* behave against zeroed configuration.
 *
 * Must run before anything touches the cache or maps a region, which in
 * practice means before PSRAM. */
void p4_cache_init(void);
