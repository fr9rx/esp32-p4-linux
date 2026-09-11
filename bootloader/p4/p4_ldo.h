/* Internal LDO regulators for the ESP32-P4 second-stage bootloader. */
#pragma once

#include <stdbool.h>

/* Channel 2 powers the PSRAM chip AND the MPLL that clocks it, at 1.8 V.
 * Required, not optional: IDF's ESP_LDO_RESERVE_PSRAM defaults to y, and
 * without this PSRAM never answers -- esp_psram_impl_enable() spins forever
 * with no fault and no message. */
#define P4_LDO_CHAN_PSRAM       2
#define P4_LDO_MV_PSRAM         1800

/* Channel 4 powers the microSD slot at 3.3 V (step 11). Same mechanism. */
#define P4_LDO_CHAN_SDMMC       4
#define P4_LDO_MV_SDMMC         3300

/* Bring an internal LDO channel up at the given voltage.
 *
 * Returns false for an invalid channel. Ordering inside follows ESP-IDF's
 * esp_ldo_acquire_channel(): current limit on while the voltage settles, then
 * off once enabled. */
bool p4_ldo_enable(int chan_id, int voltage_mv);
