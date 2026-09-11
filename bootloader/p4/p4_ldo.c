/* Internal LDO regulators for the ESP32-P4 second-stage bootloader.
 *
 * The P4 has general-purpose LDO channels that power things outside the core
 * domain -- the SPI flash (channel 1, already on at reset), the PSRAM chip
 * and MPLL (channel 2), and on this board the microSD slot (channel 4).
 *
 * Upstream this is esp_ldo_regulator.c, a driver with handles, reference
 * counts and a mutex. We want one shot at two channels, so this calls the
 * header-only LL directly and reproduces the ordering from
 * esp_ldo_acquire_channel() (esp_hw_support/ldo/esp_ldo_regulator.c:84-100).
 *
 * WHY THIS FILE EXISTS AT ALL, which is worth recording:
 *
 * PSRAM init hung. Not a fault -- the trap handler stayed quiet, so it was a
 * genuine spin on a status bit. The cause was that the MPLL acquire path in
 * ESP-IDF does this first:
 *
 *     #if CONFIG_ESP_LDO_RESERVE_PSRAM
 *         esp_ldo_acquire_channel(&ldo_mpll_config, &s_ldo_chan);
 *     #endif
 *
 * and I had reimplemented esp_clk_tree_mpll_acquire() without it, on the
 * assumption that a CONFIG-guarded block was optional. It is not: that
 * symbol defaults to y, and the Kconfig help says the channel powers "PSRAM
 * and MPLL". Unpowered, the chip cannot answer and the driver waits forever.
 */

#include <stdint.h>
#include <stdbool.h>

#include "p4_ldo.h"
#include "hal/ldo_ll.h"

bool p4_ldo_enable(int chan_id, int voltage_mv)
{
    if (!ldo_ll_is_valid_ldo_channel(chan_id)) {
        return false;
    }

    /* Channels are 1-based, units 0-based. */
    const int unit = LDO_ID2UNIT(chan_id);

    /* Current limit ON across the voltage change: it holds inrush down while
     * the rail comes up into whatever capacitance is on the board. Released
     * once enabled, or the limit itself becomes the supply ceiling. */
    ldo_ll_enable_current_limit(unit, true);

    uint8_t dref = 0;
    uint8_t mul = 0;
    bool use_rail_voltage = false;
    ldo_ll_voltage_to_dref_mul(unit, voltage_mv, &dref, &mul, &use_rail_voltage);
    ldo_ll_adjust_voltage(unit, dref, mul, use_rail_voltage);

    /* SW ownership: the hardware-owned mode hands the channel to the power
     * management state machine, which is meaningful in a running system with
     * sleep states. There is no PMU policy here -- we turn it on and leave. */
    ldo_ll_set_owner(unit, LDO_LL_UNIT_OWNER_SW);

    ldo_ll_enable_ripple_suppression(unit, true);
    ldo_ll_enable(unit, true);
    ldo_ll_enable_current_limit(unit, false);

    return true;
}
