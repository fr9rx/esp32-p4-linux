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

/* HEX (x16) DDR mode. This one line is what makes 200 MHz work.
 *
 * It has exactly one consumer, and it does not look like much:
 *
 *     mspi_timing_tuning_configs.h:
 *         #define MSPI_TIMING_PSRAM_DTR_MODE   CONFIG_SPIRAM_MODE_HEX
 *
 *     mspi_timing_tuning.c, s_select_best_tuning_config():
 *         uint32_t best_point = 0;
 *         ...
 *     #if MSPI_TIMING_PSRAM_DTR_MODE
 *         best_point = s_tuning_cfg_drv.psram_select_best_tuning_config(...);
 *     #elif MSPI_TIMING_PSRAM_STR_MODE
 *         best_point = ...
 *     #endif
 *         s_tuning_cfg_drv.psram_set_best_tuning_config(timing_config, best_point);
 *
 * Undefined in an #if is 0, silently, with no warning. So with this symbol
 * missing BOTH branches vanish, `best_point` keeps its initialiser, and the
 * tuning always selects config index 0 -- for the DQS phase pass and the
 * delayline pass alike. The sweep still runs, still measures a perfectly
 * good eye, still prints it, and then throws the answer away.
 *
 * Measured here at 200 MHz, delayline id against bytes wrong out of 128,
 * phase 67.5 degrees:
 *
 *     id     0   1   2   3   4 ... 27   28  29  30
 *     bad   81  56  18   5   0 ...  0   17  34  65
 *              ^^ index 0, what was actually being used
 *                          ^^^^^^^^^^ the eye the sweep found and discarded
 *
 * At 80 MHz index 0 lands inside the eye, so the bug is invisible: PSRAM
 * works, the walking-address test passes over all 32 MB, and nothing
 * suggests the tuning result is being ignored. At 200 MHz index 0 is just
 * outside it, and the symptom is reads shifted two bytes late -- one DDR
 * beat on a x16 bus -- which reads exactly like a read-latency problem and
 * sent me through dummy-cycle sweeps of both MSPI controllers first.
 *
 * It is a real IDF Kconfig symbol (SPIRAM_MODE_HEX), not an invention; a
 * full IDF build for this part defines it. The bootloader's hand-written
 * sdkconfig.h simply never had it. */
#define CONFIG_SPIRAM_MODE_HEX                  1

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

/* 200 MHz, which is also what IDF defaults this part to. MPLL runs at 400
 * and the bus divider is 2 (AP_HEX_PSRAM_MPLL_DEFAULT_FREQ_MHZ is 400 for
 * 200M, 320 for 80M, and the divider is that over CONFIG_SPIRAM_SPEED).
 *
 * This used to be 80 MHz, with a long note saying 200 did not work and the
 * reason was open. It was two bugs on top of each other, and neither was in
 * the chip or the board:
 *
 *   1. CONFIG_SPIRAM_MODE_HEX was not defined, so the DQS tuning discarded
 *      its own result and always used config index 0. See the block above.
 *
 *   2. Nothing may write a BURST to PSRAM while the core is still at 40 MHz.
 *      The bootloader used to run its walking-address test before raising
 *      the CPU, deliberately, so that train-map-verify all happened at one
 *      clock. At 200 MHz that first burst wedges the write channel for the
 *      rest of the boot. main.c has the measurements; the raise now happens
 *      between training and the test.
 *
 * Each bug alone produced total corruption, so fixing either one on its own
 * looked like no progress at all -- which is most of why this took so long.
 *
 * docs/PSRAM-200MHZ.md is the full account: every measurement, every wrong
 * answer, and what the working register state looks like.
 *
 * Verified after both fixes: the walking-address test passes over all 32 MB,
 * six cold boots out of six, and Linux runs on it -- memtester clean, and
 * 6 MB of it takes 104 s where 80 MHz took 159 s.
 *
 * 120 and 160 MHz are not options. The Kconfig choice offers only 20/80/200/
 * 250, and the latency constants have exactly three branches (250M, 200M,
 * else) -- an in-between speed would silently use the 10-cycle latencies
 * meant for 80 MHz.
 *
 * 250 MHz is rev-3-only silicon (IDF gates SPIRAM_SPEED_250M on
 * !ESP32P4_SELECTS_REV_LESS_V3) and this board is rev 1.0. 200 has no such
 * gate. If this ever needs to go back to 80, both symbols below move
 * together. */
#define CONFIG_SPIRAM_SPEED_200M                1
#define CONFIG_SPIRAM_SPEED                     200

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
