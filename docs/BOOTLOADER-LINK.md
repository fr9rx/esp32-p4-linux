# Bootloader link manifest

What to compile and link so the bootloader is *calling functions*, not
reimplementing them. Every claim here was resolved against ESP-IDF
v6.1-beta1 sources for **ESP32-P4 rev v1.0 (hw_ver1)**; pin that version
next to your code, because a future IDF is a porting event, not an update.

The shape:

```c
void bootloader_after_init(void)          /* weak override, never returns */
{
    uart0_set_baud(4000000);              /* yours, ~30 lines */
    esp_psram_impl_enable();              /* IDF: 1,630 lines you don't write */
    psram_selftest();                     /* yours -- walking ones over 32 MB */
    sdmmc_clocks_and_iomux();             /* yours -- HP_SYS_CLKRST + slot-0 pins */
    ldo4_set_3v3();                       /* yours -- PMU_EXT_LDO_P1_0P2A_REG +0x1d8 */
    load_partition("kernel", (void *)0x48000000);
    load_partition("dtb",    (void *)0x48800000);
    arm_mwdt();                           /* optional; closes patch 0034's gap */
    jump_to_linux(0x48000000, 0x48800000);
}
```

---

## A. What you get for free

Build as a bootloader (IDF calls this `NON_OS_BUILD`) and these come with
it. No action required.

| component | what it gives you |
|---|---|
| `bootloader_support/src/**` | **zero files include `freertos/`** — verified. This is the cleanest thing in IDF to reuse |
| `bootloader_support/src/esp32p4/bootloader_esp32p4.c` | `bootloader_init()` — the whole CPU bring-up |
| `esp_hw_support` (non-OS subset) | `cpu.c`, `port/esp32p4/esp_cpu_intr.c`, `esp_memory_utils.c`, `port/esp32p4/cpu_region_protect.c` |
| `esp_rom`, `hal`, `soc`, `efuse`, `spi_flash` (bootloader subset) | ROM entry points, LL headers, register defs, flash reads |

`bootloader_init()` covers, in order: watchdogs silenced (RTC + super WDT +
MWDT flashboot), `regi2c` master clock, **the rev≤1 CPLL→400M / SPLL→480M
step-down and BIAS `DREG_1P1` = 10**, brownout + super-WDT reset config,
`rtc_clk_init()`, console, cache + MMU, flash init.

That last set is why forking beats reimplementing: the rev-1.0 PLL
step-down in particular is a one-line-looking thing you would never guess.

## B. What you must add to the link — five files

The PSRAM stack is an **app** component. `esp_hw_support/CMakeLists.txt`
puts the whole mspi block inside `if(NOT non_os_build)`, so a bootloader
build gets none of it. Add these:

| file | lines | why |
|---|---|---|
| `esp_psram/device/esp_psram_impl_ap_hex.c` | 605 | **`esp_psram_impl_enable()` — your entry point** |
| `esp_hw_support/mspi/mspi_timing_tuning/mspi_timing_tuning.c` | 724 | DQS timing tuning driver |
| `esp_hw_support/mspi/mspi_timing_tuning/tuning_scheme_impl/mspi_timing_by_dqs.c` | 242 | the P4 HEX scheme |
| `esp_hw_support/mspi/mspi_timing_tuning/port/esp32p4/mspi_timing_config.c` | 60 | P4 port glue |
| `esp_hw_support/port/esp_clk_tree_common.c` | — | defines `esp_clk_tree_mpll_acquire()` / `esp_clk_tree_mpll_freq_set()`, which the PSRAM impl calls |

**All five verified free of `freertos/` includes.**

> **Do NOT add `esp_psram/system_layer/esp_psram.c`.** It *is*
> FreeRTOS-tainted, and you do not need it: `esp_psram_chip_init()` there
> is a two-line wrapper whose whole body is `esp_psram_impl_enable()`. The
> rest of that file registers PSRAM with the heap allocator and
> `esp_himem` — meaningless in a bootloader. Call the impl directly.

> **CORRECTION — `esp_psram_impl_enable()` does not map PSRAM into the
> address space.** It initialises the chip and the MSPI controller only.
> The mapping is in the system layer above, at `esp_psram.c:326`:
>
> ```c
> mmu_hal_map_region(1, MMU_TARGET_PSRAM0, v_start, paddr, size, &actual_len);
> ```
>
> So after the impl call PSRAM is trained but **nothing is addressable at
> `0x48000000`** until you write the MMU entries yourself. `mmu_id` is 1
> because `SOC_MMU_PER_EXT_MEM_TARGET=1` on the P4. Good news: `mmu_hal.c`
> is not gated on `non_os_build` (only on `PURE_RAM_APP`), so it is a call
> rather than a port.

Remember `CONFIG_SPIRAM_SPEED_80M` still requires tuning on this chip
(`mspi_timing_tuning_configs.h`: `#elif CONFIG_SPIRAM_SPEED_80M` →
`MSPI_TIMING_PSRAM_NEEDS_TUNING 1`). There is no speed setting that lets
you skip it, which is why those 1,026 tuning lines are not optional.

## C. The shim — two functions

`esp_psram_impl_ap_hex.c:428` uses `PERIPH_RCC_ATOMIC()`, which is not a
plain macro:

```c
#define PERIPH_RCC_ATOMIC()                                                 \
    for (int _rc_cnt = 1, __DECLARE_RCC_ATOMIC_ENV __attribute__((unused)); \
         _rc_cnt ? (periph_rcc_enter(), 1) : 0;                             \
         periph_rcc_exit(), _rc_cnt--)
```

`periph_rcc_enter()` / `periph_rcc_exit()` live in
`esp_hw_support/periph_ctrl.c`, which `NON_OS_BUILD` excludes — so the
link fails with two undefined references. Supply them:

```c
/* periph_ctrl_shim.c -- non-OS replacements.
 *
 * The real ones take a spinlock (periph_ctrl.c uses portMUX_TYPE +
 * esp_os_enter_critical_safe). In a bootloader there is one hart, no
 * scheduler and mstatus.MIE is clear, so there is nothing to serialize
 * against and no-ops are correct rather than merely convenient.
 *
 * If you ever start the second HP core before PSRAM init, this stops
 * being true -- see docs/MULTICORE.md.
 */
void periph_rcc_enter(void) { }
void periph_rcc_exit(void)  { }
```

That is the entire shim. Nothing else in the five files needs one.

## D. Two symbols to check at link, not assume

- **`spi_flash_disable_cache()` / `spi_flash_restore_cache()`** — called by
  `mspi_timing_tuning.c`. Defined in `esp_rom/patches/esp_rom_spiflash.c`,
  which a bootloader build normally has. If the link complains, that is
  the file to add.
- **`spi_flash_timing_is_tuned()`** — *not* a dependency. It is **defined
  by `mspi_timing_tuning.c` itself** and consumed by `spi_flash`. It shows
  up in a symbol scan looking like a missing reference; it is an export.

## E. Include directories

```
esp_psram/include
esp_psram/device/include
esp_hw_support/include
esp_hw_support/include/esp_private
esp_hw_support/mspi/mspi_timing_tuning/include
esp_hw_support/mspi/mspi_timing_tuning/tuning_scheme_impl/include
soc/esp32p4/register/hw_ver1        <-- rev 1.0. NOT hw_ver3
hal/esp32p4/include
```

The `hw_ver1` choice is the one to get right. The two register trees
differ in 461 `#define`s for `spi_mem_c_reg.h` and 94 for
`hp_sys_clkrst_reg.h` — MSPI and clocks, exactly the two blocks this
manifest touches. Pointing at `hw_ver3` on rev-1.0 silicon would compile
and then mistrain PSRAM.

## F. Wiring it up

Use `bootloader_components/` in your project — IDF's
`examples/custom_bootloader/bootloader_extra_dir` is the worked example
for adding components to the bootloader's link, and `bootloader_hooks`
shows the weak-symbol override.

`bootloader_hooks.h` declares both hooks weak and documents them as "meant
to be defined by a user project":

```c
void __attribute__((weak)) bootloader_before_init(void);
void __attribute__((weak)) bootloader_after_init(void);
```

`call_start_cpu0()` calls `bootloader_after_init()` immediately after
`bootloader_init()` succeeds. Because yours never returns, IDF's
partition-select and app-load tail never runs — you do not have to delete
it or fork anything to bypass it.

## G. What is genuinely yours to write

Everything below is small, and none of it exists in IDF in a form you can
call:

1. **UART0 at 4 Mbps.** SCLK = XTAL 40 MHz, `clkdiv = 10` exactly (40e6/4e6
   — no fractional part, unlike 115200's 347 + 4/16).
2. **PSRAM self-test.** Walking ones over all 32 MB *before* you trust it.
   A mistrained DQS does not announce itself; it corrupts.
3. **SDMMC clocks + slot-0 IOMUX.** `HP_SYS_CLKRST`, source PLL_F160M,
   host divider 4 → the 40 MHz the DTS `sdmmc_biu_clk`/`sdmmc_ciu_clk`
   stubs claim. Pins are the P4's fixed slot-0 set: GPIO39–44.
4. **LDO channel 4 → 3.3 V.** `PMU_EXT_LDO_P1_0P2A_REG` at `PMU_BASE +
   0x1d8`. Skip it and `dw_mmc` probes an empty slot and `rootwait` hangs
   forever with nothing on the console.
5. **Partition read → PSRAM copy.** Kernel to `0x48000000`, DTB to
   `0x48800000`.
6. **The jump.** `a0` = hartid (0), `a1` = DTB physical address,
   `mstatus.MIE` clear, `fence.i`.

And one thing you explicitly do *not* need: **no PMP setup.** why2025
patch 0031 runs user tasks at physical M-mode, so there is no U-mode grant
to program. What matters is the inverse — do not leave *locked* PMP
entries restricting M-mode, which is what IDF's
`esp_cpu_configure_region_protection()` does (entries 0–4, 6, 15 carry
`PMP_L`). Dropping the IDF app removes that constraint rather than adding
one.
