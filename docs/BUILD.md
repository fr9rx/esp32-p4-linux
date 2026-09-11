# Building the kernel and rootfs

Native NOMMU Linux 6.18.35 for the **ESP32-P4-Function-EV-Board**, headless,
UART console at 4 Mbps, root filesystem on microSD.

This tree is the Linux half only. The bootloader is a separate piece of
work — see [the contract](#what-the-bootloader-must-do) at the bottom for
exactly what it has to have done before it jumps here.

---

## Where everything comes from

The drivers are not ours. They are the
[why2025-linux](https://github.com/mrbreaker/why2025-linux) patch series —
a working native RV32 NOMMU port for the WHY2025 badge, which is the same
ESP32-P4 rev v1.0 silicon. That series is the reason this project is a
matter of weeks instead of a year: it carries the two drivers that exist
nowhere else, the **CLIC v0.9 irqchip** and the **SYSTIMER clocksource**,
plus the ESP32-P4 cache ops and the `esp32_uart` console variant.

What is ours is the board adaptation: this board has no display, no
ESP32-C6, no keypad and no sensors, and its root filesystem lives on the
SD card rather than in flash.

| | |
|---|---|
| kernel | Linux 6.18.35 LTS |
| buildroot | 2025.02.15 (the checkout under `~/why2025-linux/buildroot`) |
| patches | 12 of why2025's 35, plus six of ours — see below |
| toolchain | riscv32 uClibc, `rv32imac`, ilp32, `BINFMT_FLAT` |

### Patch series

Kept from why2025, unchanged:

| # | what |
|---|---|
| 0001 | **baseline** — CLIC v0.9 irqchip, SYSTIMER clocksource, `esp32_uart`, ESP32-P4 cache ops, entry.S signal fixes for CLIC's `MINHV` |
| 0002 | `gpio-esp32p4` — bank-aware GPIO + IO_MUX |
| 0007 | `dw_mmc` fixes — multi-slot via `snps,slot-id`, cached-descriptor IDMAC fallback for P4 cache coherency. **Mandatory here**, since root is on SD |
| 0010 | `linux,nommu-userspace-pool` — reserved pool for large NOMMU `mmap`-anon |
| 0014 | riscv signal `mcause` hardening |
| 0015 | ESP32-P4 cache-thunk hardening |
| 0016 | SYSTIMER hardening — `-ETIME` on a missed one-shot, torn 52-bit read fix |
| 0019 | `esp32p4_wdt` — TIMG0 MWDT, and the only working `reboot` on this SoC |
| 0023 | force `MPIE=1` on return to userspace |
| 0031 | **run user tasks at physical M-mode** — avoids the CLIC delivery erratum |
| 0034 | arm the MWDT at `subsys_initcall` |

Modified:

| # | what |
|---|---|
| 0022 | SYSTIMER CLIC slot edge → level. **Driver hunk only** — its DTS hunk edited the badge DTS and is folded into ours instead |

Ours:

| # | what |
|---|---|
| 0012 | `arch/riscv/boot/dts/espressif/esp32p4-function-ev.dts` + its Makefile. Replaces why2025's 0012 (badge DTS) and 0020 (badge watchdog node) |
| 0035 | `dw_mmc` FIFO-mode device-tree property |
| 0036 | `dw_mmc` IDMAC descriptor-ring invalidate. Without it the OWN-bit poll reads the stale line the CPU wrote itself, spins 100 ms per descriptor, and any directory read hangs |
| 0037 | `__sramtext` — hot kernel text linked into internal SRAM at 0x4FF40000. Default off |
| 0039 | `arch_dma_set_uncached()` through the +0x40000000 non-cacheable alias, which is what makes `dma_alloc_coherent()` real on this part |
| 0040 | `ARCH_HAS_VALID_PHYS_ADDR_RANGE` so `/dev/mem` reaches peripheral space and the aliases — register debugging on a running kernel without a reflash |

Dropped — 22 patches, all badge hardware we do not have: MIPI-DSI and the
ST7703 panel (0003, 0004, 0033), the ESP32-C6 and everything behind it
(0005, 0008, 0011, 0017, 0018, 0021, 0024, 0030, 0035), the TCA8418 keypad
(0006), the BME680/BMI270 sensors (0009, 0013), the WS2812B LEDs (0032).

> Worth knowing: several of those dropped patches exist **only** to work
> around boot freezes in the C6 path — 0030 and 0035 are entirely about the
> Bluetooth registration burst wedging cold boots, and 0011/0018 about a
> backlight SPI race. Removing that hardware removes those failure classes
> by construction, so first-try boot reliability here should start above
> why2025's measured 96.7%.

---

## Build

Buildroot needs Linux, so this builds in WSL. Sources live in this repo on
the Windows side and are rsync'd across; `output/` must be on the Linux
filesystem.

```bash
# from Windows, one shot:
wsl -e bash -lc '
export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
SRC=/mnt/c/Users/LENOVO/Projects/esp32-p4-linux
DST=$HOME/p4ev-linux
rsync -a --delete --exclude output --exclude dl "$SRC"/{config,patches,board,dts} "$DST"/
find "$DST" -type f \( -name "*.sh" -o -name "*.patch" -o -name "*.config" \
    -o -name "*_defconfig" -o -name "*.fragment" -o -name "fstab" \
    -o -name "*.dts" -o -name "*.lua" -o -name "*.S" \) -print0 \
  | xargs -0 sed -i "s/\r$//"
chmod +x "$DST"/board/*.sh
make -C $HOME/why2025-linux/buildroot O="$DST/output" \
     BR2_DEFCONFIG="$DST/config/p4ev_defconfig" defconfig
make -C $HOME/why2025-linux/buildroot O="$DST/output" -j3
'
```

Output lands in `~/p4ev-linux/output/images/`:

```
Image                       the kernel, flat, uncompressed
esp32p4-function-ev.dtb     the device tree
rootfs.ext4                 256 MB ext4 image, dd-able onto the card
rootfs.tar                  same tree as a tarball, for a card you already formatted
```

### Four things that will waste your time

**1. Sanitize `PATH` first.** WSL inherits the Windows `PATH`, which
contains `Program Files`. Buildroot checks for this and stops with
`Your PATH contains spaces, TABs, and/or newline (\n) characters.`
The `export PATH=...` line above is not optional.

**2. `-j3`, not `-j$(nproc)`.** This WSL instance has 4 GB. The GCC
cross-toolchain build OOMs above `-j3`.

**3. Strip CRLF on the way in.** The rsync copies from an NTFS mount. A
`\r` on a patch context line makes the hunk fail to apply; a `\r` in a
shell script makes it fail to execute with a confusing error.

**4. Buildroot drops symbols silently.** `BR2_PACKAGE_BINUTILS` has
`depends on BR2_USE_WCHAR`, and why2025's uClibc is built without wchar —
so `make defconfig` quietly discarded both `BR2_PACKAGE_BINUTILS` and
`BR2_PACKAGE_BINUTILS_TARGET`, and the first sign would have been a
rootfs with no assembler and no error anywhere. `BR2_TOOLCHAIN_BUILDROOT_WCHAR=y`
in `config/p4ev_defconfig` is what fixes it. **After any defconfig change,
grep the resulting `output/.config` for what you asked for** rather than
assuming it took.

---

## The configs are generated deltas, not hand-edited files

`config/kernel.config` and `config/p4ev_defconfig` are derived from
why2025's originals (kept alongside as `*.why2025` / `*.ref`) by scripts
that declare the change set. That keeps `diff` against upstream meaningful
when they push updates, instead of drowning it in a 105 KB reformat.

Kernel config delta:

| change | why |
|---|---|
| `CONFIG_EXT4_FS=y` | root filesystem; why2025 had it off to reclaim partition budget |
| `CONFIG_SQUASHFS`, `CONFIG_MTD` off | no flash rootfs any more |
| `CONFIG_CMDLINE` | SD root, 4 Mbps console |
| `CONFIG_DRM`, `FB`, `FRAMEBUFFER_CONSOLE`, `LOGO` off | headless |
| `CONFIG_CFG80211`, `BT`, `PWM` off | no C6 |
| `CONFIG_IIO`, `INPUT_KEYBOARD`, `NEW_LEDS` off | badge-only parts |
| `CONFIG_KALLSYMS=y` | why2025 disabled it to fit 6.5 MB; the cuts above pay for it, and without symbol names every oops on a headless board is bare hex |

Command line (`CONFIG_CMDLINE_FORCE=y`, so this wins over the DTB's
`bootargs`):

```
earlycon console=ttyS0,4000000 root=/dev/mmcblk0p1 rootfstype=ext4 rootwait rw loglevel=8 idle=poll
```

`rootwait` is mandatory — MMC enumerates asynchronously, and without it the
kernel panics on "unable to mount root fs" before the card appears.

---

## The on-board build kit

`as`, `objcopy`, `lua`, and `mkflt` — enough to write, assemble, link and
run a program on the board itself.

```
# cp /usr/share/hello-onboard.S /home && cd /home
# mkflt hello-onboard.S && ./hello-onboard
```

`ld` is deliberately absent, and **not** to save space: it does not *run*
here. `binfmt_flat` makes one physically contiguous allocation per exec
with no demand paging, `ld` is 5,283,840 bytes, and this allocator's
ceiling is `MAX_PAGE_ORDER` = 10 = **4 MB**. It fails at exec every time.
`/usr/lib/mkflt.lua` — a linker written in Lua — replaces it: it lays the
sections out, applies the rv32 relocations and writes the bFLT header,
calling `objcopy` for each section's bytes. `/usr/share/stress.S` is its
regression test.

`board/checkflat.sh` enforces that 4 MB ceiling at build time, so an
oversized binary fails the build rather than failing at exec on the board.

> **Its `LIMIT` is 4 MB here, not the 8 MB the hypervisor tree uses.** That
> tree patches `CONFIG_ARCH_FORCE_MAX_ORDER` to 11; this one has no such
> patch and `config/kernel.config` has no `ARCH_FORCE_MAX_ORDER`, so the
> kernel default of 10 applies. If you ever add that patch, raise both
> together — a `LIMIT` that is too high is worse than no check at all,
> because a 6 MB binary would then pass the build and fail on the board.
>
> Note patch 0010's userspace pool does **not** raise this. It backs
> `mmap`-anon through `do_mmap_private`; program text still comes from the
> buddy allocator as one contiguous block.

---

## Preparing the SD card

One ext4 partition, and this is now the first setup in the project with
**persistent writable storage** — the hypervisor line's romfs root was
read-only with `/home` on tmpfs.

```bash
sudo parted /dev/sdX mklabel msdos
sudo parted /dev/sdX mkpart primary ext4 1MiB 100%
sudo mkfs.ext4 -L p4root /dev/sdX1
sudo mount /dev/sdX1 /mnt/card
sudo tar -xf output/images/rootfs.tar -C /mnt/card
sudo umount /mnt/card
```

Or `dd` the image and grow it:

```bash
sudo dd if=output/images/rootfs.ext4 of=/dev/sdX1 bs=4M conv=fsync
sudo resize2fs /dev/sdX1
```

Flash then carries only the bootloader, `Image` and the `.dtb` — on a
16 MB chip that is a lot of headroom, and it is the moment to design in
A/B kernel slots if you want them.

---

## What the bootloader must do

Linux has no ESP32-P4 clock, pinctrl or LDO driver, so several DTS nodes
are deliberate fictions — fixed-clock stubs and always-on fixed regulators
describing state the bootloader is expected to have established. None of
them fails loudly. They compute a wrong divisor or probe an unpowered
device.

1. **UART0** at 4000000 8N1, SCLK = XTAL (40 MHz), `clkdiv = 10`.
   40e6/4e6 is exactly 10 — no fractional divisor, unlike 115200's
   347 + 4/16. On a headless board this is the only way to see a failure,
   so bring it up before anything else.
2. **SDMMC clocks** in `HP_SYS_CLKRST`: source PLL_F160M, host divider 4 →
   the 40 MHz biu/ciu the DTS stubs claim.
3. **SDMMC slot-0 IOMUX**, function 0: `GPIO39=D0 GPIO40=D1 GPIO41=D2
   GPIO42=D3 GPIO43=CLK GPIO44=CMD`. These are the P4's dedicated slot-0
   pins (ESP-IDF `sdmmc_pins.h SDMMC_SLOT0_IOMUX_*`), not board wiring.
4. **LDO channel 4 → 3.3 V** for the microSD, via `PMU.ext_ldo[4]`
   (`PMU_EXT_LDO_P1_0P2A_REG` @ `PMU_BASE + 0x1d8`). Skip it and `dw_mmc`
   probes an empty slot: `rootwait` then hangs forever with no message.
5. PSRAM up (32 MB HEX at 80 MHz), kernel at `0x48000000`, DTB at
   `0x48800000`, `a0` = hartid (0), `a1` = DTB physical address,
   `mstatus.MIE` clear, `fence.i` before the jump.
6. Optionally **arm the MWDT** before jumping. Patch 0034 moves the kernel's
   arm to `subsys_initcall` but says freezes before ~0.3 s remain uncovered
   and "boot-shim arming is the eventual answer" — that gap is yours to
   close.

Note that patch 0031 runs user tasks at physical M-mode, so there is **no
U-mode PMP grant to set up**. What matters is the inverse: do not leave
locked PMP entries that restrict M-mode. ESP-IDF's
`esp_cpu_configure_region_protection()` does exactly that (entries 0–4, 6,
15 all carry `PMP_L`); dropping IDF removes the constraint rather than
adding one.

### One trap in the DTS worth repeating

`uart0` deliberately has **no `clock-frequency` property**.
`drivers/tty/serial/earlycon.c:310` reads that property into
`port->uartclk`, and `esp32_uart.c` then does

```c
if (device->port.uartclk != BASE_BAUD * 16)
        esp32_uart_set_baud(&device->port, device->baud);
```

so a present `clock-frequency` makes earlycon **reprogram** the baud from
the command-line string, fighting whatever the bootloader set. Absent,
`uartclk` stays 0, `earlycon.c:148` defaults it to `BASE_BAUD * 16`, the
test is false, and earlycon inherits the bootloader's baud — which is what
we want. The hypervisor DTS in the sibling repo *does* carry
`clock-frequency` on its 16550 node; do not copy that habit across.
