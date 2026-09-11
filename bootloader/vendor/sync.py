#!/usr/bin/env python3
"""Vendor the ESP-IDF sources this bootloader reuses, and pin them.

The goal is a tree that builds with no ESP-IDF installed. Run this once
against an IDF checkout; the result is committed and the checkout is no
longer needed.

    python vendor/sync.py --idf /c/esp/v6.1-beta1/esp-idf

What it does: starting from ROOTS, follow every `#include "..."` (quoted
includes only -- angle-bracket ones come from the toolchain), resolve each
against SEARCH_DIRS, and copy what it finds into vendor/include preserving
the path the source used. Then write MANIFEST.md recording the IDF version
and every file taken.

Why a script rather than a copied directory: the include closure of the
PSRAM and clock code is not obvious by inspection -- it reaches through
esp_private/ and hal/ in ways that are easy to get subtly wrong by hand,
and easy to get wrong *differently* the second time. This makes re-pinning
to a new IDF a one-command, reviewable diff.

Conditional includes are followed unconditionally, so the result is a
superset of what any single configuration compiles. That is deliberate:
a header copied and unused costs nothing, a header missed costs a build.
"""

import argparse
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

# Sources compiled into the bootloader, relative to IDF's components/.
# Grows as the build order advances; see ../main.c for the step list.
ROOTS = [
    # Step 3: the analog-bus transaction layer, needed to reach the CPLL/SPLL
    # and BIAS registers.
    #
    # NOT esp_hw_support/regi2c_ctrl.c -- that one includes freertos/FreeRTOS.h
    # unconditionally (its NON_OS_BUILD guard is only in the header) because it
    # wraps every access in a mutex. With NON_OS_BUILD defined, the header
    # redirects regi2c_ctrl_write_reg_mask -> regi2c_impl_write_mask, i.e.
    # straight to the lock-free implementation below, so the locked wrapper is
    # never referenced. Single hart with interrupts off: nothing to serialise.
    "esp_hal_regi2c/esp32p4/regi2c_impl.c",

    # Step 4b: CPU to 360 MHz (the rev-1.0 ceiling) and MPLL to 400 MHz.
    #
    # rtc_clk.c provides rtc_clk_cpu_freq_set_config(), rtc_clk_mpll_enable()
    # and rtc_clk_mpll_configure() -- everything the CPU and MPLL need.
    #
    # esp_clk_tree_common.c is deliberately NOT here, even though it defines
    # the esp_clk_tree_mpll_acquire()/_freq_set() pair that the PSRAM impl
    # calls. It includes <freertos/FreeRTOS.h> unconditionally, purely for
    # portMUX_TYPE spinlocks around a reference count, and those two functions
    # are thin wrappers over the rtc_clk calls above. Rather than fake a
    # FreeRTOS header, p4/p4_clk.c supplies the pair directly -- see the
    # comment there for why dropping the refcount is correct here.
    "esp_hw_support/port/esp32p4/rtc_clk.c",

    # rtc_clk.c branches on efuse_hal_chip_revision() when programming the
    # CPU PLL. Vendored rather than stubbed to return 100 (= rev 1.0): this
    # reads the actual silicon, so a board that is not what we assumed
    # reports itself instead of being quietly mis-clocked. Both files are
    # FreeRTOS-free.
    "hal/efuse_hal.c",
    "hal/esp32p4/efuse_hal.c",

    # Step 5: PSRAM. The chip/controller bring-up plus the DQS timing tuning
    # it cannot work without -- 200 MHz needs tuning, and so does 80; there is
    # no speed setting on this part that skips it.
    "esp_psram/device/esp_psram_impl_ap_hex.c",
    "esp_hw_support/mspi/mspi_timing_tuning/mspi_timing_tuning.c",
    "esp_hw_support/mspi/mspi_timing_tuning/tuning_scheme_impl/mspi_timing_by_dqs.c",
    "esp_hw_support/mspi/mspi_timing_tuning/port/esp32p4/mspi_timing_config.c",

    # esp_psram_impl_enable() trains the chip but does NOT make it addressable;
    # mapping is a separate mmu_hal_map_region() call that p4_psram.c makes.
    # cache_hal comes along because the mapped window has to be invalidated
    # before first use.
    "hal/mmu_hal.c",
    "hal/cache_hal.c",

    # clk_hal_xtal_get_freq_mhz(), which p4_clk.c hands to
    # rtc_clk_mpll_configure() when PSRAM asks for MPLL.
    "esp_hal_clock/esp32p4/clk_tree_hal.c",
]

# Headers we need but that nothing in ROOTS' include closure reaches, because
# they define register *addresses* used by our own code in p4/ rather than by
# any vendored source. Same resolution rules; they seed the queue alongside
# ROOTS, so anything they pull in comes along too.
EXTRA_HEADERS = [
    # Analog-bus register maps for the rev-1.0 PLL step-down and BIAS trim.
    "soc/regi2c_cpll.h",       # I2C_CPLL_OC_DIV_7_0   -> CPLL 400M
    "soc/regi2c_syspll.h",     # I2C_SYSPLL_OC_DIV_7_0 -> SPLL 480M
    "soc/regi2c_bias.h",       # I2C_BIAS_DREG_1P1 / _PVT
    "hal/brownout_ll.h",       # BOD mode1 reset enable
    "hal/clk_tree_hal.h",      # clk_hal_xtal_get_freq_mhz(), used by p4_clk.c
    "esp_private/rtc_clk.h",   # rtc_clk_mpll_enable/disable/configure
    # LDO channels. Channel 2 powers PSRAM *and MPLL* on this part and is
    # required, not optional -- ESP_LDO_RESERVE_PSRAM defaults to y. Channel 4
    # powers the microSD slot at step 11.
    "hal/ldo_ll.h",
    # EMAC. GMAC_CTRL0 lives in HP_SYSTEM rather than in the EMAC block or in
    # HP_SYS_CLKRST, and it carries PHY_INTF_SEL -- the RMII/MII selector.
    # Nothing else we vendor reaches this header.
    "soc/hp_system_reg.h",
    # GPIO output set/clear, for the PHY reset strobe on GPIO 51. The RMII
    # pads themselves are IOMUX (io_mux_reg.h, already in the closure); this
    # is only needed because PHY reset is a plain GPIO.
    "soc/gpio_reg.h",
    # EMAC MAC registers, for the MDIO scan. GMIIADDRESS/GMIIDATA and their
    # GB/GW/CR/GR/PA field positions -- the management interface is the one
    # part of Ethernet the bootloader can test on its own, and "is there a
    # PHY, and at what address" is not a question Linux can answer here:
    # /dev/mem refuses peripheral space along with everything else past
    # high_memory.
    "soc/emac_reg.h",
]

# Linker scripts. Pure `symbol = address` text, no code, but load-bearing:
# without them the ROM entry points and the peripheral struct instances are
# undefined at link time.
LD_FILES = [
    # ROM function addresses. eco0_4 is the variant IDF selects for rev < 3,
    # and the board's own boot banner confirms it: "ESP-ROM:esp32p4-eco2".
    "esp_rom/esp32p4/ld/esp32p4.rom.eco0_4.ld",
    "esp_rom/esp32p4/ld/esp32p4.rom.api.ld",
    # Peripheral base addresses. IDF's *_struct.h headers declare things like
    # `extern lpperi_dev_t LPPERI;` and this is where the address comes from --
    # so a missing include here shows up as an undefined reference to LPPERI
    # rather than as a missing header.
    "soc/esp32p4/ld/esp32p4.peripherals.ld",
]

# Where quoted includes are resolved from, in order. Relative to components/.
# EVERY per-target directory here must be the esp32p4 one. IDF ships the same
# header name for a dozen chips -- soc/rtc.h and hal/gpio_ll.h exist under
# esp32, esp32s3, esp32c6 and so on -- and resolution is first-match, so a
# generic or wrong-target directory in this list silently compiles another
# chip's register layout. That failure surfaces as PSRAM or clocks
# misbehaving, not as a build error.
SEARCH_DIRS = [
    "esp_hw_support/include",
    "esp_hw_support/include/soc",
    "esp_hw_support/port/include",
    "esp_hw_support/port/esp32p4/include",
    "esp_hw_support/ldo/include",
    "spi_flash/include",
    "esp_mm/include",
    "esp_hw_support/mspi/mspi_timing_tuning/include",
    "esp_hw_support/mspi/mspi_timing_tuning/tuning_scheme_impl/include",
    # The DQS tuning tables for this silicon. Same filename exists under every
    # target's port directory, so this must be the esp32p4 one.
    "esp_hw_support/mspi/mspi_timing_tuning/port/esp32p4",
    "esp_psram/include",
    "esp_psram/device/include",
    "hal/include",
    "hal/esp32p4/include",
    "hal/platform_port/include",
    "esp_hal_regi2c/esp32p4/include",
    "esp_hal_regi2c/include",
    "riscv/include",
    "esp_hal_pmu/esp32p4/include",
    "esp_hal_pmu/include",
    "esp_hal_wdt/esp32p4/include",
    "esp_hal_clock/esp32p4/include",
    "esp_hal_clock/include",
    "esp_hal_gpio/esp32p4/include",
    "esp_hal_gpio/include",
    "esp_hal_mspi/esp32p4/include",
    "esp_hal_mspi/include",
    "esp_hal_gpspi/include",
    "heap/include",
    "esp_rom/include",
    "esp_rom/esp32p4/include",
    "esp_rom/esp32p4",              # esp_rom_caps.h sits here, not under include/
    "esp_rom/esp32p4/include/esp32p4",   # "rom/gpio.h" resolves from here
    "esp_common/include",
    "esp_system/include",
    "log/include",
    "soc/include",
    "soc/esp32p4/include",
    # hw_ver1, NOT hw_ver3: rev 1.0 silicon. The two trees differ by 461
    # #defines in spi_mem_c_reg.h and 94 in hp_sys_clkrst_reg.h -- exactly
    # MSPI and clocks, exactly what this bootloader programs.
    "soc/esp32p4/register/hw_ver1",
]

# Headers we deliberately supply ourselves rather than take from IDF.
# sdkconfig.h is generated by Kconfig, which is the build system we removed;
# ours is hand-written and lives in the bootloader root.
# assert.h is the C library's, from the toolchain -- esp_assert.h spells it
# with quotes but it is <assert.h> all the same.
PROVIDED = {"sdkconfig.h", "assert.h"}

INCLUDE_RE = re.compile(rb'^\s*#\s*include\s+"([^"]+)"', re.MULTILINE)

# "esp32s3/rom/spi_flash.h" and friends: another chip's headers, reached only
# through #if CONFIG_IDF_TARGET_xxx branches we never compile. Matching esp32p4
# is deliberately excluded -- if OUR target's header goes missing, that is a
# real problem and should not be filed under "expected".
OTHER_TARGET_RE = re.compile(
    r"^esp32(?!p4/)(c\d+|s\d+|h\d+|p\d+)?/", re.IGNORECASE)


def idf_version(idf):
    try:
        out = subprocess.run(["git", "-C", idf, "describe", "--tags", "--dirty"],
                             capture_output=True, text=True, timeout=30)
        if out.returncode == 0 and out.stdout.strip():
            return out.stdout.strip()
    except Exception:
        pass
    vf = os.path.join(idf, "version.txt")
    if os.path.exists(vf):
        with open(vf) as fh:
            return fh.read().strip()
    return "unknown"


def resolve(inc, components, search_dirs, cur_dir):
    """Find `inc` next to the including file first, then along search_dirs."""
    cand = os.path.join(cur_dir, inc)
    if os.path.isfile(cand):
        return cand, None
    for d in search_dirs:
        base = os.path.join(components, d)
        cand = os.path.join(base, inc)
        if os.path.isfile(cand):
            return cand, inc
    return None, None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--idf", required=True, help="path to an ESP-IDF checkout")
    args = ap.parse_args()

    idf = os.path.abspath(args.idf)
    components = os.path.join(idf, "components")
    if not os.path.isdir(components):
        sys.exit("not an IDF checkout: %s" % idf)

    out_src = os.path.join(HERE, "src")
    out_inc = os.path.join(HERE, "include")
    out_ld = os.path.join(HERE, "ld")
    for d in (out_src, out_inc, out_ld):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)

    taken_src, taken_inc, taken_ld, missing = [], {}, [], []
    queue = []

    for ld in LD_FILES:
        src = os.path.join(components, ld)
        if not os.path.isfile(src):
            missing.append(ld)
            continue
        shutil.copy2(src, os.path.join(out_ld, os.path.basename(ld)))
        taken_ld.append((ld, os.path.basename(ld)))

    for root in ROOTS:
        src = os.path.join(components, root)
        if not os.path.isfile(src):
            missing.append(root)
            continue
        # Mirror the IDF path rather than flattening to the basename. IDF
        # reuses filenames across components and targets -- hal/efuse_hal.c and
        # hal/esp32p4/efuse_hal.c are different files with the same name -- and
        # flattening silently drops one, which then shows up as an undefined
        # reference with no clue as to why. Mirroring also keeps provenance
        # readable straight from the tree.
        rel = root.replace("\\", "/")
        dst = os.path.join(out_src, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(src, dst)
        taken_src.append((root, rel))
        queue.append(src)

    for extra in EXTRA_HEADERS:
        found, relname = resolve(extra, components, SEARCH_DIRS, components)
        if not found:
            missing.append(extra)
            continue
        rel = relname or extra
        dst = os.path.join(out_inc, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        shutil.copy2(found, dst)
        taken_inc[rel] = os.path.relpath(found, idf).replace("\\", "/")
        queue.append(found)

    seen = set()
    while queue:
        path = queue.pop()
        if path in seen:
            continue
        seen.add(path)
        with open(path, "rb") as fh:
            body = fh.read()
        for m in INCLUDE_RE.finditer(body):
            inc = m.group(1).decode()
            if inc in PROVIDED:
                continue
            found, relname = resolve(inc, components, SEARCH_DIRS,
                                     os.path.dirname(path))
            if not found:
                missing.append(inc)
                continue
            # Preserve the path the source spelled, so #include "hal/x.h"
            # keeps working with a single -Ivendor/include.
            rel = relname or inc
            dst = os.path.join(out_inc, rel)
            if rel not in taken_inc:
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                shutil.copy2(found, dst)
                taken_inc[rel] = os.path.relpath(found, idf).replace("\\", "/")
            queue.append(found)

    ver = idf_version(idf)
    with open(os.path.join(HERE, "MANIFEST.md"), "w", newline="\n") as fh:
        fh.write("# Vendored ESP-IDF sources\n\n")
        fh.write("Generated by `vendor/sync.py`. Do not edit by hand -- "
                 "re-run the script.\n\n")
        fh.write("| | |\n|---|---|\n")
        fh.write("| ESP-IDF version | `%s` |\n" % ver)
        fh.write("| source path | `%s` |\n" % idf.replace("\\", "/"))
        fh.write("| target | ESP32-P4 **rev v1.0** (hw_ver1, ROM eco0_4) |\n")
        fh.write("| compiled sources | %d |\n" % len(taken_src))
        fh.write("| headers | %d |\n" % len(taken_inc))
        fh.write("| linker scripts | %d |\n\n" % len(taken_ld))
        fh.write("Nothing here includes `freertos/` -- a bootloader has no "
                 "RTOS behind it.\n\n")
        fh.write("## Compiled into the bootloader\n\n")
        for orig, rel in sorted(taken_src):
            fh.write("- `src/%s` <- `components/%s`\n" % (rel, orig))
        fh.write("\n## Linker scripts\n\n")
        for orig, base in sorted(taken_ld):
            fh.write("- `ld/%s` <- `components/%s`\n" % (base, orig))
        fh.write("\n## Headers\n\n")
        for rel in sorted(taken_inc):
            fh.write("- `include/%s` <- `%s`\n" % (rel, taken_inc[rel]))
        if missing:
            expected, real = [], []
            for m in sorted(set(missing)):
                # Other targets' ROM headers, FreeRTOS, and xtensa live behind
                # #if branches this build never takes. The script follows
                # includes unconditionally (superset by design), so it tries
                # them anyway and does not find them. That is correct.
                if (OTHER_TARGET_RE.match(m) or m.startswith("freertos/")
                        or "xtensa" in m or m.startswith("xt_")):
                    expected.append(m)
                else:
                    real.append(m)
            fh.write("\n## Unresolved\n\n")
            if real:
                fh.write("**Check these** -- not found along the search path, "
                         "and not obviously from a branch we skip. Each is "
                         "either supplied by us, provided by the toolchain, "
                         "or a gap in `SEARCH_DIRS`:\n\n")
                for m in real:
                    fh.write("- `%s`\n" % m)
                fh.write("\n")
            if expected:
                fh.write("Expected -- other targets' headers and RTOS/xtensa "
                         "paths, reached only through `#if` branches this "
                         "build does not compile:\n\n")
                for m in expected:
                    fh.write("- `%s`\n" % m)

    print("IDF %s" % ver)
    print("  sources: %d" % len(taken_src))
    print("  headers: %d" % len(taken_inc))
    print("  ld:      %d" % len(taken_ld))
    if missing:
        print("  unresolved: %d" % len(set(missing)))
        for m in sorted(set(missing))[:15]:
            print("    %s" % m)


if __name__ == "__main__":
    main()
