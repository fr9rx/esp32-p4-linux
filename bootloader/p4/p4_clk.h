/* Clock bring-up for the ESP32-P4 second-stage bootloader. */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Record the crystal frequency where the rest of the clock code can read it
 * back. Call once, before anything else here.
 *
 * Not cosmetic: without it rtc_clk falls back to "assume 40MHz", and the same
 * register feeds the MPLL configuration that PSRAM training depends on. */
void p4_clk_init(void);

/* Raise the CPU to `mhz`. Returns false if the frequency is not one the
 * hardware can express, leaving the clock untouched.
 *
 * 360 is the ceiling on rev 1.0 silicon: IDF's Kconfig.cpu reads
 * `default ESP_DEFAULT_CPU_FREQ_MHZ_360 if ESP32P4_SELECTS_REV_LESS_V3`,
 * else 400 -- so 400 MHz is rev >= 3 only, and the "400MHz" in the why2025
 * DTS header comment does not apply to this board.
 *
 * Must run BEFORE PSRAM init: the MSPI timing tables are indexed by core
 * clock as well as module clock, so training against a clock that is about
 * to change trains against the wrong one. */
bool p4_clk_cpu_set_mhz(uint32_t mhz);

/* Report the CPU frequency the hardware is actually running at, read back
 * from the clock registers rather than assumed. */
uint32_t p4_clk_cpu_get_mhz(void);
