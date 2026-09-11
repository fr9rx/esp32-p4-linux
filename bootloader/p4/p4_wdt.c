/* Watchdog silencing for the ESP32-P4 second-stage bootloader.
 *
 * Register facts taken from ESP-IDF v6.1-beta1,
 * components/soc/esp32p4/register/hw_ver1/soc/{lp_wdt_reg.h,timer_group_reg.h}
 * and components/esp_hal_wdt/esp32p4/include/hal/lpwdt_ll.h. Addresses are
 * spelled out here rather than pulled through the IDF header tree so that the
 * first build stands alone -- the soc/hal headers come into vendor/ later,
 * when PSRAM needs them.
 *
 * Sequence mirrors IDF's own bootloader_init(): super-watchdog auto-feed
 * first, then flashboot-mode off on both the RTC and TIMG0 watchdogs, then
 * disable outright.
 */

#include <stdint.h>
#include "p4_wdt.h"

#define REG32(addr) (*(volatile uint32_t *)(uintptr_t)(addr))

/* DR_REG_LPAON_BASE 0x50110000 + 0x6000 */
#define LP_WDT_BASE                 0x50116000u
#define LP_WDT_CONFIG0_REG          (LP_WDT_BASE + 0x00)
#define LP_WDT_WPROTECT_REG         (LP_WDT_BASE + 0x18)
#define LP_WDT_SWD_CONFIG_REG       (LP_WDT_BASE + 0x1c)
#define LP_WDT_SWD_WPROTECT_REG     (LP_WDT_BASE + 0x20)

#define LP_WDT_WDT_EN               (1u << 31)
#define LP_WDT_WDT_FLASHBOOT_MOD_EN (1u << 12)
#define LP_WDT_SWD_AUTO_FEED_EN     (1u << 18)

/* Same key for LP_WDT, the super-watchdog and MWDT (lpwdt_ll.h / mwdt_ll.h). */
#define WDT_WKEY_VALUE              0x50D83AA1u

/* DR_REG_HPPERIPH1_BASE 0x500C0000 + 0x2000. This is the same TIMG0 the
 * kernel's esp32p4_wdt driver claims as watchdog@500c2000. */
#define TIMG0_BASE                  0x500C2000u
#define TIMG_WDTCONFIG0_REG         (TIMG0_BASE + 0x48)
#define TIMG_WDTWPROTECT_REG        (TIMG0_BASE + 0x64)

#define TIMG_WDT_EN                 (1u << 31)
#define TIMG_WDT_FLASHBOOT_MOD_EN   (1u << 14)

void p4_wdt_disable_all(void)
{
    /* 1. Super-watchdog: it cannot be disabled, only fed. Auto-feed is the
     *    supported way to keep it quiet, and IDF does exactly this. */
    REG32(LP_WDT_SWD_WPROTECT_REG) = WDT_WKEY_VALUE;
    REG32(LP_WDT_SWD_CONFIG_REG)  |= LP_WDT_SWD_AUTO_FEED_EN;
    REG32(LP_WDT_SWD_WPROTECT_REG) = 0;

    /* 2. LP/RTC watchdog: drop flashboot mode, then disable.
     *
     *    Flashboot mode is the bit that makes this fire during boot at all --
     *    clearing it before disabling matches IDF's order and avoids a window
     *    where the timeout is live but the enable bit has already moved. */
    REG32(LP_WDT_WPROTECT_REG) = WDT_WKEY_VALUE;
    REG32(LP_WDT_CONFIG0_REG) &= ~LP_WDT_WDT_FLASHBOOT_MOD_EN;
    REG32(LP_WDT_CONFIG0_REG) &= ~LP_WDT_WDT_EN;
    REG32(LP_WDT_WPROTECT_REG) = 0;

    /* 3. TIMG0 main watchdog, same treatment. */
    REG32(TIMG_WDTWPROTECT_REG) = WDT_WKEY_VALUE;
    REG32(TIMG_WDTCONFIG0_REG) &= ~TIMG_WDT_FLASHBOOT_MOD_EN;
    REG32(TIMG_WDTCONFIG0_REG) &= ~TIMG_WDT_EN;
    REG32(TIMG_WDTWPROTECT_REG) = 0;
}
