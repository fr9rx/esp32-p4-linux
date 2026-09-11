/* Clock bring-up for the ESP32-P4 second-stage bootloader.
 *
 * Two jobs:
 *   1. Raise the CPU to its ceiling (360 MHz on rev 1.0).
 *   2. Supply esp_clk_tree_mpll_acquire()/_freq_set(), which the vendored
 *      PSRAM implementation calls for itself.
 *
 * The heavy lifting is all in vendor/src/rtc_clk.c; this file is the thin
 * layer that decides policy and replaces one ESP-IDF source we could not use.
 */

#include <stdint.h>
#include <stdbool.h>

#include "p4_clk.h"
#include "p4_ldo.h"

#include "esp_err.h"
#include "esp_private/rtc_clk.h"
#include "hal/clk_tree_hal.h"
#include "soc/rtc.h"

extern int ets_printf(const char *fmt, ...);

void p4_clk_init(void)
{
    /* Stamp the crystal frequency into RTC_XTAL_FREQ_REG.
     *
     * IDF does this in rtc_clk_init(), which we do not vendor. Without it,
     * rtc_clk.c cannot read back a valid value and falls back with:
     *
     *   rtc_clk(warn): invalid RTC_XTAL_FREQ_REG value, assume 40MHz
     *
     * The assumption happens to be right on this board, so everything works
     * -- which is exactly why it is worth fixing rather than tolerating.
     * clk_hal_xtal_get_freq_mhz() reads the same register, and p4_clk.c hands
     * its result to rtc_clk_mpll_configure() when PSRAM comes up. A wrong
     * value there does not fail; it mistrains PSRAM, silently.
     *
     * SOC_XTAL_FREQ_40M is 40 -- the enum values are the MHz numbers. The
     * board's own boot banner and espflash board-info both report a 40 MHz
     * crystal, so this is measurement rather than assumption. */
    rtc_clk_xtal_freq_update(SOC_XTAL_FREQ_40M);
}

bool p4_clk_cpu_set_mhz(uint32_t mhz)
{
    rtc_cpu_freq_config_t cfg;

    /* Asks the hardware whether this frequency is expressible with the
     * available PLL/divider combinations, rather than us assuming. On rev 1.0
     * this succeeds for 360 and fails for 400. */
    if (!rtc_clk_cpu_freq_mhz_to_config(mhz, &cfg)) {
        return false;
    }
    rtc_clk_cpu_freq_set_config(&cfg);
    return true;
}

uint32_t p4_clk_cpu_get_mhz(void)
{
    rtc_cpu_freq_config_t cfg;
    rtc_clk_cpu_freq_get_config(&cfg);
    return cfg.freq_mhz;
}

/* ---------------------------------------------------------------------------
 * MPLL, for PSRAM.
 *
 * These two are normally defined in esp_hw_support/port/esp_clk_tree_common.c,
 * which we do not vendor: it includes <freertos/FreeRTOS.h> unconditionally
 * to get portMUX_TYPE for spinlocks around a reference count. Faking a
 * FreeRTOS header to get two wrappers was the worse trade.
 *
 * What we drop, and why it is safe here:
 *
 *   - the spinlocks. One hart, mstatus.MIE clear. Nothing to serialise
 *     against.
 *   - the reference count. Upstream tracks how many peripherals share MPLL so
 *     that a second consumer cannot change a frequency the first is relying
 *     on. In this bootloader PSRAM is the only consumer, ever, and it sets
 *     the frequency exactly once.
 * What we do NOT drop, having tried:
 *
 *   - the LDO acquire. An earlier version of this file skipped it, reasoning
 *     that a CONFIG_ESP_LDO_RESERVE_PSRAM-guarded block was optional. It is
 *     not -- that symbol defaults to y and the channel powers PSRAM *and*
 *     MPLL. Without it esp_psram_impl_enable() spins forever, with no fault
 *     for the trap handler to report. See p4_ldo.c.
 *
 * The signatures match ESP-IDF's exactly, because vendor/src/esp_psram_impl_ap_hex.c
 * calls them by name and must link against these.
 * ------------------------------------------------------------------------ */

static uint32_t s_mpll_freq_hz;

esp_err_t esp_clk_tree_mpll_acquire(void)
{
    /* Power first. MPLL shares LDO channel 2 with the PSRAM chip, and
     * enabling the PLL against an unpowered rail is how this hung. */
    if (!p4_ldo_enable(P4_LDO_CHAN_PSRAM, P4_LDO_MV_PSRAM)) {
        ets_printf("p4boot: mpll: LDO ch%d enable REFUSED\r\n", P4_LDO_CHAN_PSRAM);
        return ESP_FAIL;
    }
    rtc_clk_mpll_enable();
    return ESP_OK;
}

void esp_clk_tree_mpll_release(void)
{
    rtc_clk_mpll_disable();
    s_mpll_freq_hz = 0;
}

esp_err_t esp_clk_tree_mpll_freq_set(uint32_t expt_freq_hz, uint32_t *real_freq_hz)
{
    uint32_t xtal_mhz = clk_hal_xtal_get_freq_mhz();
    ets_printf("p4boot: mpll: xtal %u MHz -> mpll %u MHz\r\n",
               (unsigned)xtal_mhz, (unsigned)(expt_freq_hz / 1000000u));

    /* thread_safe = false: the parameter exists so callers inside an RTOS can
     * ask for internal locking. There is no thread to be safe from. */
    rtc_clk_mpll_configure(xtal_mhz, expt_freq_hz / 1000000u, false);
    s_mpll_freq_hz = expt_freq_hz;

    if (real_freq_hz != NULL) {
        *real_freq_hz = s_mpll_freq_hz;
    }
    return ESP_OK;
}
