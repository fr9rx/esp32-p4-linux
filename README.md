# esp32-p4-linux

**Native** NOMMU Linux 6.18.35 on an ESP32-P4 Function EV Board. No
hypervisor, no ESP-IDF at runtime, no display — a serial console at 4 Mbps
and a root filesystem on the microSD card.

This is the successor to the hypervisor line in the sibling
`esp32-p4-linux-hypervisor` repo, which boots Linux as a U-mode guest under a
hand-written M-mode monitor by trap-and-emulate. That works, and every
emulated device costs traps: the console tops out around 80 kB/s at about
1.56 traps per character. Running natively removes the monitor entirely —
Linux owns the machine, in M-mode, on real hardware.

```
                      ESP32-P4 rev v1.0, HP core, M-mode, NOMMU
  flash (16 MB)         bootloader 0x2000 + table 0x8000
                        kernel 0x20000 (12 MB) + dtb 0xC20000 (64 KB)
  PSRAM (32 MB)         kernel at 0x48000000, DTB at 0x49000000
                        10 MB reserved at 0x49600000 for NOMMU user mmap
  microSD               ext4 root, journalled -- persistent, writable
  EMAC                  DesignWare GMAC, RMII -- DHCP, DNS, TCP, TLS
  UART0                 console, 4 Mbps
```

It boots to a login prompt in about two seconds.

## What you need

| | |
|---|---|
| board | ESP32-P4 Function EV Board, **rev v1.0** (400 MHz parts are rev ≥ 3 and are not what this targets) |
| card | a microSD card. Its contents are destroyed |
| cable | USB for the serial console, and Ethernet if you want the network |
| host | Linux or WSL for the build; `espflash` for flashing; a riscv32 bare-metal gcc for the bootloader |

Recovery is total. Flash holds only this bootloader and two data
partitions, and the ROM first stage is untouched, so nothing here can brick
the board — `espflash write-bin` puts back anything.

## Quick start

```sh
# 1. the bootloader (riscv32-esp-elf gcc on PATH)
cd bootloader && make

# 2. kernel + rootfs (buildroot; see docs/BUILD.md for the tree setup)
make                       # in your buildroot output dir

# 3. the device tree
tools/mkdtb.sh dts/esp32p4-function-ev.dts images/esp32p4-function-ev.dtb

# 4. flash it. NOTE the KERNEL= override: the Makefile still defaults to
#    Image.initramfs, which is the provisioning kernel, not this one.
cd bootloader
make flash flash-parts flash-dtb
make flash-kernel KERNEL=../images/Image
```

Then open the console at **4000000** baud. It reaches a login prompt
(`root`, no password) in about two seconds.

`docs/BUILD.md` has the full procedure and the four things that waste your
time.

## Split of work

| half | what | where |
|---|---|---|
| **bootloader** | clocks, PSRAM, SDMMC clocks + IOMUX + LDO4, UART0, load kernel and DTB, jump | written separately, in C, reusing ESP-IDF's bring-up sources without the framework |
| **Linux** | DTS, patch series, kernel config, rootfs | this repo |

The bootloader's obligations are specified in
[`docs/BUILD.md`](docs/BUILD.md#what-the-bootloader-must-do) and repeated
in the header comment of the DTS, because several DTS nodes are fixed-clock
and fixed-regulator stubs that *assert* what the bootloader did rather than
describe hardware — and none of them fails loudly when the assertion is
false.

## The drivers are why2025's

The ESP32-P4 has no CLIC irqchip driver and no SYSTIMER clocksource in
mainline, and without those two "native Linux on a P4" is a year of work.
Both come from [why2025-linux](https://github.com/mrbreaker/why2025-linux),
a working port for the WHY2025 badge — the same rev v1.0 silicon. We take
12 of their 35 patches unmodified, one with a hunk removed, and add our own
board DTS. The 22 dropped patches are all badge hardware this board does
not have.

## Layout

| path | what |
|---|---|
| `dts/esp32p4-function-ev.dts` | the board device tree |
| `patches/linux/` | the trimmed kernel series, ours numbered 0012 |
| `config/kernel.config` | kernel config (a generated delta over why2025's) |
| `config/p4ev_defconfig` | buildroot config (likewise) |
| `config/*.why2025`, `config/*.ref` | the upstream originals, kept so `diff` stays meaningful |
| `board/` | rootfs overlay, busybox and uClibc fragments, post-build, and the on-board build kit |
| `board/hget/` | `hget`, the board's HTTP/HTTPS client — single-threaded C, written for this target |
| `tools/` | host-side scripts: device-tree build, card provisioning, serial helpers |
| `docs/BUILD.md` | how to build it, and the four things that waste your time |
| `docs/BOOTLOADER-LINK.md` | the exact IDF files to link, the two-function shim, and what's genuinely yours to write |
| `docs/MULTICORE.md` | why the second HP core is unused, and what using it would cost |
| `docs/HANDOFF.md`, `docs/NOTES.md` | carried over from the hypervisor line |
| `docs/NATIVE-DRIVERS.md` | the earlier plan for replacing emulated devices — superseded by this repo, kept for the register-level findings |

## What is on the image

The tool set is the union of this image's and the hypervisor guest's, which
had drifted apart — the hypervisor image had 17 packages this one lacked,
and this one had `binutils`, which the on-board assembly kit below depends
on:

```
as  objcopy  lua  mkflt          build and run programs on the board
nano  vi  less                   edit and page
grep  sed  diff  which  tree     the GNU ones, not just busybox
bc  xz  bzip2                    arithmetic and compression
memtester  dhrystone  whetstone  test the PSRAM, benchmark the core
free  fdisk  mkfs.ext2           inspect memory, partition and format the card
hget                             HTTP and HTTPS, mbedTLS -- see below
ping  nslookup  udhcpc  ifconfig network
ntpd  date                       set the clock; there is no RTC
find  time  mktemp               and the applets whose absence broke DHCP
```

`curl` is also installed and **does not work**: every transfer fails with
`CURLE_OUT_OF_MEMORY` (exit 27), including `file:///etc/fstab`, which
touches no network at all. Three explanations were tested on hardware and
all three were wrong — `RLIMIT_STACK` from 8 MB down to 128 KB (uClibc sizes
pthread stacks from it), bypassing the threaded resolver with `--resolve`,
and raising its bFLT stack from 4 KB to 256 KB. It is left in the image as
an open problem. `hget` is what to use.

The rootfs is 11.7 MB, affordable only because it lives on the card rather
than in an initramfs — the two changes pay for each other.

## On-board build kit

`as`, `objcopy`, `lua` and `mkflt` ship in the rootfs, so the board can
build and run its own programs:

```
# cp /usr/share/hello-onboard.S /home && cd /home
# mkflt hello-onboard.S && ./hello-onboard
```

`ld` is absent because it does not *run*: `binfmt_flat` makes one
physically contiguous allocation per exec, `ld` is 5.3 MB, and this
allocator's ceiling is 4 MB. `mkflt.lua` — a linker in Lua — replaces it.
`board/checkflat.sh` enforces the ceiling at build time so an oversized
binary fails the build instead of failing at exec on the board.

## Status

It runs. The board boots from flash to a login prompt in about two seconds,
with the root filesystem on the microSD card.

```
[    1.2] EXT4-fs (mmcblk0p1): mounted filesystem r/w with ordered data mode
[    1.3] Run /sbin/init as init process

# free
              total        used        free      shared  buff/cache   available
Mem:          16784        5272       10164           0        1348       10068
# df -h /
/dev/root               229.9M     10.2M    202.9M   5% /
```

Moving the root filesystem off an initramfs and onto the card is what made
the memory affordable; the 10 MB NOMMU user pool is the other half of the
budget, and it is reserved whether or not anything uses it.

Bootloader steps 1–11 are done; step 12 (SD high speed) is not. See
[`docs/STATUS-2026-09-11.md`](docs/STATUS-2026-09-11.md) for the current
state, then `STATUS-2026-09-10.md`, `STATUS-2026-09-09.md` and
`STATUS-2026-09-08.md` backwards for how it got here.

### Ethernet works

DHCP, DNS, routing and TCP, brought up by the board itself at boot with no
console:

```
eth0      Link encap:Ethernet  HWaddr 60:55:F9:FB:0C:31
          inet addr:192.168.1.28  Bcast:192.168.1.255  Mask:255.255.255.0
--- 8.8.8.8 ping statistics ---
3 packets transmitted, 3 packets received, 0% packet loss
```

5 MB over HTTP from a host on the same LAN takes 1.38 s (~30 Mbit/s, and the
other end was on Wi-Fi), in full-MTU frames, with zero errors and zero
watchdog resets.

The P4's EMAC is a Synopsys DesignWare GMAC register-for-register, so
mainline `stmmac` drives it with no glue file — the chip says so itself:

```
stmmaceth 50098000.ethernet: User ID: 0x11, Synopsys ID: 0x37
stmmaceth 50098000.ethernet: 	DWMAC1000
eth0: PHY [stmmac-0:01] driver [ICPlus IP101G] (irq=POLL)
eth0: Link is Up - 100Mbps/Full - flow control rx
```

`bootloader/p4/p4_emac.c` does what Linux has no driver to do: the EMAC bus
clock and reset, RMII mode, the RMII-input clock tree, the seven RMII pads on
IOMUX function 3, MDC/MDIO through the GPIO matrix, the PHY reset on GPIO 51,
and the station address. Its own DMA is the GMAC's integrated IDMAC — GDMA is
not involved.

**The station address comes from eFuse.** stmmac reads `MACADDRESS0` at probe
and, finding it blank, invents a random locally-administered address on every
boot — so the DHCP server hands out a new lease each time and nothing on the
LAN can recognise the board twice. The bootloader now reads eFuse BLOCK1 and
writes the real address in, which on the P4 belongs to Ethernet: it is the
one universally administered MAC the part has.

**Transmit used to hang, and the fix is four device-tree properties.** Left to
itself stmmac turns on store-and-forward in both directions, because
`dma_cap.tx_coe` is set on this block and that alone is enough for
`stmmac_dma_operation_mode()` to choose it. Store-and-forward means the DMA
pulls the whole frame into the FIFO before the MAC starts sending — and the
FIFOs here are a few hundred bytes, as ESP-IDF notes on the same register:
*"Disable Receive Store Forward (Rx FIFO is only 256B)"*. Small frames went
out; a 342-byte DHCP DISCOVER left the DMA parked in transmit state TS=3,
*reading frame data from host memory into the Tx FIFO*, forever — no error
bit, no underflow, no bus fault. `snps,force_thresh_dma_mode` (plus `snps,aal`,
`snps,mixed-burst` and `snps,no-pbl-x8`, matched to IDF) is the whole fix. The
measurements are in `docs/STATUS-2026-09-11.md` and in a comment above the
node in the DTS.

### The SD card works, and why it did not

`dw_mmc` polls the IDMAC descriptor OWN bit with a plain load, which is
correct only when the descriptor ring is uncached. On NOMMU there is no
second mapping to mark uncached, so `dma_alloc_coherent()` fails and the ring
is ordinary cached PSRAM — the poll then reads the stale line the CPU wrote
itself, spins 100 ms per descriptor and fails the transfer. One big `dd`
survived that; the many small requests a filesystem generates did not, which
is why reading any subdirectory used to hang forever.
`patches/linux/0036` adds the invalidate that was missing. Details and the
hardware evidence are in `docs/STATUS-2026-09-09.md`.

### Hot kernel text in SRAM

`patches/linux/0037` adds `__sramtext`: functions so marked are linked into
internal SRAM at `0x4FF40000` and copied there by `_start_kernel`, instead of
running from cached PSRAM. Default off, and it needs `!RELOCATABLE`
(`CONFIG_PHYS_RAM_BASE_FIXED`) because a PIE kernel would relocate a section
that has a real fixed home.

Measured, and reported as measured: with the CLIC, systimer and DMA cache
hooks moved into SRAM there is **no difference** on a shell loop or on SD
throughput — those eight functions run often enough that L1 already holds
them. What did pay is the fixed link the feature requires: `Image` drops
5,613,444 → 4,501,212 bytes, `MemTotal` gains 664 kB, and a whole relocation
pass disappears from boot. `images/Image.sram` is that build; `images/Image`
is the stock relocatable one.

## Known problems

Recorded here rather than left to be rediscovered:

- **`curl` fails every transfer** with `CURLE_OUT_OF_MEMORY` (exit 27),
  including `file://` URLs. Unsolved; `hget` exists because of it.
- **No `e2fsck`, and none possible.** `e2fsprogs` has
  `depends on BR2_USE_MMU` in buildroot (util-linux/libblkid), so a damaged
  filesystem cannot be repaired on the board. The journal is the mitigation.
- **No RTC.** The clock starts at the epoch and TLS rejects every
  certificate until it is set. `/etc/init.d/S45ntp` handles it when the
  network is up; without a network, `date -s` first.
- **10 Mbit/s links will not negotiate usefully.** ESP-IDF reprograms an
  ESP-specific RMII clock divider on a speed change; `stmmac` knows nothing
  about it and the bootloader sets the 100 Mbit/s value once.
- **`time` reports nonsense user time.** Real time is correct.
- Bootloader step 12 (SD high speed), 200 MHz PSRAM, and warm-reset flash
  instability are open. See the `docs/STATUS-*.md` series.

## Credits

The CLIC irqchip and SYSTIMER clocksource — without which none of this
starts — come from [why2025-linux](https://github.com/mrbreaker/why2025-linux),
a port for the WHY2025 badge on the same rev v1.0 silicon.

The bootloader reuses ESP-IDF's PSRAM, MSPI-timing and clock-tree sources
directly, vendored under `bootloader/vendor/` with their paths and versions
recorded in `bootloader/vendor/MANIFEST.md`.

## License

**MIT**, with two directories that carry their own terms and cannot be
relicensed:

| part | license |
|---|---|
| `patches/linux/` | **GPL-2.0-only.** These are patches against the Linux kernel; a patch is a derivative work of the file it modifies, so it inherits the kernel's license. Not a choice this repository makes. See `patches/linux/README.md` |
| `bootloader/vendor/` | **Apache-2.0** (63 of the files dual Apache-2.0 OR MIT). Vendored unmodified from ESP-IDF with the per-file SPDX headers intact; redistributed under Espressif's terms, not relicensed. Provenance in `bootloader/vendor/MANIFEST.md` |
| everything else | **MIT** — `bootloader/p4/`, `board/` (including `board/hget/`), `dts/`, `tools/`, `config/`, `docs/` |

A built bootloader binary links the vendored Apache-2.0 sources, so the
binary carries an Apache-2.0 attribution obligation even though the code
written here is MIT. The licenses are compatible; that is attribution, not
conflict. `hget` links mbedTLS, which is dual Apache-2.0 OR
GPL-2.0-or-later — taken as Apache-2.0, so MIT applies to `hget` itself.

See [`LICENSE`](LICENSE).
