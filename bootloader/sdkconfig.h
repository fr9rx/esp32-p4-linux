/* Hand-written replacement for ESP-IDF's generated sdkconfig.h.
 *
 * ESP-IDF sources include "sdkconfig.h" expecting Kconfig to have produced it.
 * Kconfig is part of the build system we removed, so we write it ourselves.
 * That is a feature, not a workaround: every configuration choice this
 * bootloader makes is visible in this one file instead of spread across
 * menuconfig defaults.
 *
 * Only symbols the vendored code actually reads belong here. To find them:
 *
 *   grep -rhoE "CONFIG_[A-Z0-9_]+" vendor/src vendor/include | sort -u
 *
 * Target: ESP32-P4 rev v1.0.
 */
#pragma once

/* ---- target ---------------------------------------------------------- */
#define CONFIG_IDF_TARGET_ESP32P4               1

/* Must be a string literal: esp_private/periph_ctrl.h pastes it into a
 * deprecation message with string concatenation, so an integer or a missing
 * definition is a syntax error rather than a missing-symbol error. */
#define CONFIG_IDF_TARGET                       "esp32p4"

/* The P4's HP cores are RISC-V. Vendored code branches on this in a few
 * places -- esp_fault.h, for instance, picks the illegal-instruction encoding
 * from it (`unimp` for riscv, `ill.n` for xtensa) and #errors out if neither
 * is set. */
#define CONFIG_IDF_TARGET_ARCH_RISCV            1

/* rev 1.0 is "less than v3", which is what selects hw_ver1 registers, the
 * eco0_4 ROM, the CPLL/SPLL step-down in p4_analog.c, and the 360 MHz CPU
 * ceiling. If this is ever wrong, PSRAM mistrains rather than failing
 * loudly -- see docs/BOOTLOADER-LINK.md. */
#define CONFIG_ESP32P4_SELECTS_REV_LESS_V3      1

/* ---- logging --------------------------------------------------------- */
/* 0 = none. Vendored code sprinkles ESP_EARLY_LOGx around; at level 0 those
 * compile to nothing, which keeps both the image and the console clean. Our
 * own ets_printf calls in main.c are the console output, and they say more
 * useful things than the library's would. */
#define CONFIG_BOOTLOADER_LOG_LEVEL             3
#define CONFIG_LOG_MAXIMUM_LEVEL                3
#define CONFIG_LOG_DEFAULT_LEVEL                3

/* Log formatter generation. 1 is the original printf-style path; 2 adds a
 * structured/binary variant. At level 0 nothing is emitted either way, so
 * this only has to be a valid number for the macros to compile. */
#define CONFIG_BOOTLOADER_LOG_VERSION           1
#define CONFIG_LOG_VERSION                      1

/* ---- power management ------------------------------------------------ */
/* PSRAM half-sleep wake delay, in microseconds. Referenced by
 * esp_psram_impl_resume_from_halfsleep_mode(), which this bootloader never
 * calls -- we bring PSRAM up once and hand it to Linux, never sleeping. It
 * has to be defined for the file to compile; IDF's own default is 155. */
#define CONFIG_PM_SLP_SPIRAM_HALFSLEEP_EXIT_WAIT_DELAY  155

/* ---- HAL ------------------------------------------------------------- */
/* HAL_ASSERT compiles to nothing at level 0. A bootloader has nowhere to
 * report an assertion to before UART0 is up, and after that our own printfs
 * are more informative than a file/line. */
#define CONFIG_HAL_DEFAULT_ASSERTION_LEVEL      0

/* ---- cache / MMU ----------------------------------------------------- */
/* Read by p4_cache.c when initialising the two HALs. Values match what the
 * hypervisor line's IDF sdkconfig uses on this same board. */
#define CONFIG_CACHE_L2_CACHE_SIZE              0x20000   /* 128 KB */
#define CONFIG_CACHE_L2_CACHE_LINE_SIZE         64
#define CONFIG_CACHE_L1_CACHE_LINE_SIZE         64

/* 64 KB, the only option the P4 offers (soc/esp32p4/Kconfig.mmu). Also passed
 * on the compiler command line as SOC_MMU_PAGE_SIZE -- see the Makefile for
 * why that one cannot live here. */
#define CONFIG_MMU_PAGE_SIZE                    0x10000

/* ---- PSRAM ----------------------------------------------------------- */
/* 32 MB HEX (16-bit) AP PSRAM at 0x48000000. HEX rather than quad is what
 * this package has; esp_psram_impl_ap_hex.c is the driver that matches. */
#define CONFIG_SPIRAM                           1

/* CONFIG_SPIRAM_USE_8LINE_MODE is deliberately NOT defined.
 *
 * The name reads like "this is an 8-line part", but it is not a description --
 * it is an override that forces a 16-line HEX PSRAM to run on 8 data lanes:
 *
 *     mode_reg.mr8.x16 = 1;
 *     #if CONFIG_SPIRAM_USE_8LINE_MODE
 *         mode_reg.mr8.x16 = 0;
 *     #endif
 *
 * IDF defaults it to n. Setting it cost an evening: PSRAM trained, reported
 * itself correctly (32768 KB, vendor 0xd), and then read back with the bytes
 * skewed -- because half the data lanes were not being used. That looks
 * identical to a signal-integrity failure, and I misread it as one. */

/* 80 MHz. IDF defaults this part to 200 MHz; 200 does not work HERE YET, and
 * the reason is still open. Recorded properly because the symptom is precise
 * and someone (possibly future me) should be able to pick this up cold.
 *
 * WHAT 200 MHz DOES: everything except read correctly. MPLL comes up at 400,
 * the chip trains, tuning completes, and it identifies itself perfectly --
 * AP Memory gen 4, 256 Mbit, good-die Pass, and it accepts the 200 MHz
 * setting (Readlatency 0x04 = 14 cycles @ Fixed). Then reads through the
 * mapped window come back offset by exactly 8 bytes:
 *
 *     read(0x48000008) == what was written to 0x48000000
 *     read(0x4800000c) == what was written to 0x48000004
 *     ... 100% of 8388608 words
 *
 * Eight bytes is four beats on a 16-bit DDR bus: a read-latency mismatch on
 * the cache/AXI path, not noise and not marginal signal integrity. Note the
 * tuning tunes MSPI_ID_3 (manual transactions) while the mapped window uses
 * MSPI_ID_2, and the suspicion is that the tuned dummy-cycle result is not
 * reaching ID_2 in our reduced environment.
 *
 * RULED OUT along the way, each verified on hardware:
 *   - CONFIG_SPIRAM_USE_8LINE_MODE was wrongly set (a real bug, now fixed --
 *     it forced a 16-line part onto 8 lanes). Fixing it changed the symptom
 *     but not the outcome.
 *   - CPU clock: fails identically with the core left at 40 MHz.
 *   - Ordering: fails whether PSRAM comes before or after the CPU raise.
 *     (IDF's order is PSRAM first -- cpu_start.c:645 vs 830 -- and we now
 *     match it regardless, because it is the proven one.)
 *   - Stale cache lines over the remapped window: invalidating after the map
 *     is correct and now done, but was not the cause.
 *
 * NOT yet investigated: flash-side MSPI setup. IDF's bootloader configures
 * flash MSPI before PSRAM and the two share the controller; we leave flash
 * as the ROM set it (40 MHz DIO). That is the most promising next thread.
 *
 * 80 MHz passes a walking-address test over all 32 MB, twice -- once with the
 * CPU at 40 MHz and again at 360 MHz. It selects MPLL 320 with a bus divider
 * of 4 (AP_HEX_PSRAM_MPLL_DEFAULT_FREQ_MHZ is 320 for 80M, 400 for 200M).
 *
 * To retry 200: flip both symbols below and watch step 6. It reports the
 * failure rate and the first four bad words, which is what makes the 8-byte
 * offset visible at all.
 *
 * 120 and 160 MHz are not options. The Kconfig choice offers only 20/80/200/
 * 250, and the latency constants have exactly three branches (250M, 200M,
 * else) -- an in-between speed would silently use the 10-cycle latencies
 * meant for 80 MHz. Given 200 already fails with a latency-shaped symptom,
 * inventing latency parameters is the wrong direction. */
#define CONFIG_SPIRAM_SPEED_80M                 1
#define CONFIG_SPIRAM_SPEED                     80

/* ---- flash ----------------------------------------------------------- */
/* 40 MHz DIO, matching what the ROM was told at boot and what espflash writes
 * into the image header. Flash and PSRAM share the MSPI timing machinery, so
 * this participates in the tuning decisions even though we never raise it. */
#define CONFIG_ESPTOOLPY_FLASHFREQ_40M          1

/* ---- deliberately NOT defined ---------------------------------------- */
/*
 * CONFIG_SPIRAM_ECC_ENABLE
 *   ECC costs PSRAM capacity and we want all 32 MB for Linux.
 *
 * CONFIG_SPIRAM_TIMING_TUNING_POINT_VIA_TEMPERATURE_SENSOR
 *   Re-tunes as the die warms. Needs the temperature sensor driver and a
 *   running system to re-tune from; a bootloader trains once and hands over.
 *
 * CONFIG_FREERTOS_UNICORE
 *   No FreeRTOS here at all.
 */
/*
 * CONFIG_ESP_ROM_HAS_REGI2C_IMPL
 *   Some targets get the analog-bus transaction code from ROM. The P4 does
 *   not -- its esp_rom_caps.h has no such entry -- so we compile IDF's
 *   implementation from vendor/src/regi2c_impl.c instead. Defining this
 *   would silently drop to ROM stubs that are not there.
 *
 * CONFIG_LIBC_PICOLIBC, CONFIG_STAGE_PATCH
 *   Referenced by vendored headers on paths we do not take.
 */
