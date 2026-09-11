/* Watchdog silencing for the ESP32-P4 second-stage bootloader. */
#pragma once

/* Disable every watchdog the ROM leaves armed: the LP super-watchdog (via
 * auto-feed), the LP/RTC watchdog, and TIMG0's main watchdog.
 *
 * Must be called early. The ROM arms these with flashboot-enable set so that a
 * bootloader which hangs does not leave the chip wedged; a bootloader that
 * intends to take a while -- ours trains PSRAM and copies 5.5 MB out of
 * flash -- has to turn them off or be reset midway. */
void p4_wdt_disable_all(void);
