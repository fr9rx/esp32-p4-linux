/* Analog-domain bring-up for the ESP32-P4 second-stage bootloader.
 *
 * Mirrors ESP-IDF v6.1-beta1's bootloader_hardware_init() and
 * bootloader_ana_reset_config() from
 * components/bootloader_support/src/esp32p4/bootloader_esp32p4.c.
 *
 * Everything here has to happen before any clock is raised: the PLL trim is
 * about keeping the analog supply adequate while CPLL, SPLL and (shortly)
 * MPLL all run, and the bias trim is what makes that supply adequate.
 */

#include <stdint.h>
#include <stdbool.h>

#include "p4_analog.h"

#include "esp_private/regi2c_ctrl.h"
#include "hal/regi2c_ctrl_ll.h"
#include "hal/brownout_ll.h"
#include "soc/regi2c_cpll.h"
#include "soc/regi2c_syspll.h"
#include "soc/regi2c_bias.h"

extern void ets_delay_us(uint32_t us);

void p4_analog_init(void)
{
    /* 1. The analog i2c master is the bus every register below sits behind.
     *    IDF leaves its clock permanently on for the whole bootloader rather
     *    than gating per access -- there is no power argument during a boot
     *    that lasts under a second, and gating it around each transaction is
     *    an easy way to get a half-written analog register. */
    _regi2c_ctrl_ll_master_enable_clock(true);
    regi2c_ctrl_ll_master_configure_clock();

    /* 2. Step the PLLs DOWN. This is rev-1.0-specific and it is the least
     *    guessable thing in the whole bring-up sequence.
     *
     *    IDF's comment, verbatim: "On ESP32P4 ECO0, the default (power on
     *    reset) CPLL and SPLL frequencies are very high, lower them to avoid
     *    bias may not be enough in bootloader". So the PLLs come out of reset
     *    faster than the analog supply can comfortably feed, and the fix is
     *    to trim them before doing anything else.
     *
     *    Guarded on the revision because IDF guards it: on rev >= 3 the
     *    reset defaults are sane and this would be trimming for no reason.
     *
     *    Note IDF also calls bootloader_init_mspi_clock() here, but only for
     *    rev > 1 -- deliberately NOT mirrored, since ours is rev 1.0. */
#if CONFIG_ESP32P4_SELECTS_REV_LESS_V3
    REGI2C_WRITE_MASK(I2C_CPLL,   I2C_CPLL_OC_DIV_7_0,   6);  /* CPLL -> 400 MHz */
    REGI2C_WRITE_MASK(I2C_SYSPLL, I2C_SYSPLL_OC_DIV_7_0, 8);  /* SPLL -> 480 MHz */
    ets_delay_us(100);                                        /* let them settle */
#endif

    /* 3. Bias trim. Both fields to 10, matching IDF. This is the "enough
     *    bias" half of the step above: with three PLLs about to be running
     *    (CPLL for the CPU, SPLL, MPLL for PSRAM at 200 MHz) the default
     *    regulator setting is not generous enough. */
    REGI2C_WRITE_MASK(I2C_BIAS, I2C_BIAS_DREG_1P1,     10);
    REGI2C_WRITE_MASK(I2C_BIAS, I2C_BIAS_DREG_1P1_PVT, 10);

    /* 4. Brownout detector, mode 1, resets the chip.
     *
     *    Protection rather than enablement -- nothing below depends on it --
     *    but it belongs here because a brownout during PSRAM training is
     *    exactly the sort of fault that would otherwise present as
     *    "PSRAM is flaky" rather than "the supply sagged". */
    brownout_ll_ana_reset_enable(true);
}
