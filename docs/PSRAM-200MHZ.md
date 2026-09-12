# Bringing up ESP32-P4 HEX PSRAM at 200 MHz without ESP-IDF

This is the complete record of getting 32 MB of AP Memory HEX (x16 DDR) PSRAM
running at **200 MHz** from a bare-metal second-stage bootloader — no ESP-IDF
framework, no FreeRTOS, no `app_main`. Just `main()` in M-mode calling
vendored IDF bring-up sources directly.

It had been stuck at 80 MHz across several sessions with a precise but
misleading symptom. The answer was **two independent bugs stacked on top of
each other**, neither in the chip, the board, or the vendored code. Both were
in the hand-written configuration around it.

Written at length on purpose. The fix is four lines; the value is in how the
measurements were taken and how many confident wrong answers came first.

- **Board:** ESP32-P4 Function EV Board, rev **v1.0**, 40 MHz XTAL
- **PSRAM:** AP Memory gen 4, 256 Mbit, HEX x16 DDR, `vendor id 0x0d`
- **IDF sources vendored from:** `v6.1-beta1`
- **Result:** 200 MHz, 9 of 9 cold boots, Linux 6.18.35 on top of it

---

## 1. The short version

If you only want the fix:

```c
/* bootloader/sdkconfig.h */
#define CONFIG_SPIRAM_MODE_HEX                  1   /* was missing entirely */
#define CONFIG_SPIRAM_SPEED_200M                1
#define CONFIG_SPIRAM_SPEED                     200
```

and in `main()`, the CPU raise must sit **between** PSRAM training and the
first burst write to PSRAM:

```c
p4_psram_init(&psram_bytes);     /* train + map, core still at 40 MHz */
p4_clk_cpu_set_mhz(360);         /* <-- must be here */
p4_psram_test(psram_bytes, &bad);/* first burst writes happen in here */
```

Everything below is why.

---

## 2. What bring-up actually consists of

Without IDF there is no `esp_psram_init()` doing this for you. The sequence
that has to happen, in this order:

| # | step | who does it here |
|---|---|---|
| 1 | Power LDO channel 2 to 1.8 V — feeds **both** the PSRAM chip and MPLL | `p4_ldo.c` |
| 2 | MPLL to 400 MHz (320 for 80 MHz PSRAM) | `p4_clk.c` → IDF `rtc_clk_mpll_configure()` |
| 3 | Enable + reset the MSPI module clock, select MPLL as source | IDF `esp_psram_impl_enable()` |
| 4 | Set bus divider, enable the DLL | IDF, same |
| 5 | Write MR0/MR4/MR8 — latency, drive, x16, burst length | IDF `s_init_psram_mode_reg()` |
| 6 | Read back MR1/MR2 to identify the chip and its size | IDF |
| 7 | Configure MSPI_ID_2 (the cache path): cmd, addr, dummy, DDR, line mode, AXI | IDF `s_config_mspi_for_psram()` |
| 8 | **DQS timing tuning** — sweep phase, then delayline | IDF `mspi_timing_psram_tuning()` |
| 9 | Map PSRAM into the address space via the MMU | **ours** (`p4_psram.c`) |
| 10 | Enable the cache bus for that window, then invalidate it | **ours** |
| 11 | Raise the CPU | **ours** |
| 12 | Verify before trusting it | **ours** |

Steps 9–12 are what `esp_psram.c` would have done. We replaced that file
rather than vendoring it, because it carries a dynamic vaddr allocator we do
not need — PSRAM has a fixed home at `SOC_EXTRAM_LOW` (`0x48000000`) and we
are the only thing running.

**That replacement is where both bugs lived.** Not in the copied code — in
the configuration and ordering we wrote around it.

### The thing that is easy to miss

`esp_psram_impl_enable()` brings up the chip *and leaves nothing
addressable*. Until `mmu_hal_map_region()` runs, a load from `0x48000000`
does not reach PSRAM. Two separate things, and the second is ours:

```c
/* mmu_id is 1, not 0: SOC_MMU_PER_EXT_MEM_TARGET is set on the P4, so
 * flash and PSRAM have separate MMUs and 1 is PSRAM's. */
mmu_hal_map_region(1, MMU_TARGET_PSRAM0, 0x48000000, 0, map_len, &mapped);
cache_ll_l1_enable_bus(0, cache_ll_l1_get_bus(0, 0x48000000, mapped));
cache_hal_invalidate_addr(0x48000000, mapped);
```

---

## 3. The symptom we started with

Flipping `CONFIG_SPIRAM_SPEED` to 200 and booting:

```
p4boot: mpll: xtal 40 MHz -> mpll 400 MHz
I (0) hex_psram: vendor id    : 0x0d (AP)
I (0) hex_psram: density      : 0x07 (256 Mbit)
I (0) hex_psram: good-die     : 0x06 (Pass)
I (0) hex_psram: BitMode      : 0x01 (X16 Mode)
I (0) hex_psram: Readlatency  : 0x04 (14 cycles@Fixed)
I (0) MSPI Timing: Enter psram timing tuning
p4boot: PSRAM 32768 KB mapped at 0x48000000 (200 MHz)
p4boot:   bad 0x48000000: wrote 0xeda5a5a5 read 0xa5a5eda5
p4boot:   bad 0x48000004: wrote 0xeda5a5a1 read 0xa5a5eda5
p4boot:   bad 0x48000008: wrote 0xeda5a5ad read 0xa5a5a5a5
p4boot:   bad 0x4800000c: wrote 0xeda5a5a9 read 0xeda5a5a1
p4boot:   8388608 of 8388608 words bad (100% )
```

Everything *except* moving data works. The chip identifies itself perfectly,
accepts the 200 MHz latency setting, and reports as a good die. Then 100% of
8,388,608 words come back wrong.

Note `read(0x4800000c) == wrote(0x48000004)`. That is an **8-byte offset**,
and a previous session had recorded exactly that and concluded "read-latency
mismatch on the cache/AXI path". It is a completely reasonable reading of the
evidence. It is also wrong, and it cost a lot of time, because it points at
dummy cycles and dummy cycles were never involved.

### The test that produced it

Worth describing, because its design is what made the rest legible:

```c
/* Address-in-address, not a fixed pattern. A constant passes happily if the
 * address lines are mistrained -- every location holds the same value, so
 * aliasing is invisible. Writing each word's own address means a stuck or
 * swapped address line shows up as a mismatch, and the value that comes back
 * names the location it actually reached. XOR with a constant so a word of
 * all-zeros or a floating bus cannot pass by accident. */
for (i = 0; i < words; i++)
    mem[i] = (uint32_t)&mem[i] ^ 0xA5A5A5A5u;
```

and it **counts** mismatches rather than bailing on the first, because
*"a handful of bad words at one boundary is a mapping or cache problem,
every word bad is a timing or lane problem, and a scattered few is marginal
signal integrity"* — and stopping at the first one cannot tell those apart.

---

## 4. Bug 1 — the tuning threw away its own result

### Finding it

The first useful move was raising the log level so IDF's own tuning debug
appears (`CONFIG_LOG_MAXIMUM_LEVEL` / `LOG_DEFAULT_LEVEL` / 
`BOOTLOADER_LOG_LEVEL` to 4). That printed:

```
D MSPI Timing: psram_freq_mhz: 20 mhz, bus clock div: 20
D MSPI Timing: psram_freq_mhz: 200 mhz, bus clock div: 2
D MSPI Timing: test nums: 1, test result:     <- phase pass
D MSPI Timing: [0][good][1] [1][good][1] [2][good][1] [3][good][1]
D MSPI Timing: test nums: 100, test result:   <- delayline pass
D MSPI Timing: [0][bad][0] [1][bad][0] [2][bad][0] [3][bad][0] [4][bad][99]
D MSPI Timing: [5][good][100] ... [27][good][100]
D MSPI Timing: [28][bad][0] [29][bad][0] [30][bad][0]
```

**The tuning succeeds.** A clean 23-wide eye with crisp edges, 100 correct
reads out of 100 at every point in it. So the chip, the pins and the physical
timing are all fine at 200 MHz — and yet the mapped window is 100% wrong.

Everything after this was narrowing "works during the sweep, broken
afterwards".

### The bisect

Probes inserted at each step of `p4_psram_init()`:

```
SW post-enable bad 82/128   <- already broken here
SW post-map    bad 81/128
SW post-bus    bad 83/128
SW post-inval  bad 80/128
```

Broken the instant `esp_psram_impl_enable()` returns, so the MMU mapping and
the cache bus were innocent. That leaves a very small window: the end of the
sweep, `mspi_timing_enter_high_speed_early()`, and two
`enable_variable_dummy()` calls.

Re-applying the tuned registers by hand did not help. Re-running the *entire*
tuning and replaying the read immediately did not help either. So the sweep's
success genuinely did not survive leaving the sweep loop.

### The measurement that cracked it

Re-run the sweep's own scan — every DQS phase against every delayline — on
the state that init left behind:

```
                delayline index 0 .............................. 30
SW p0 (67.5°)   81 56 18  5  0  0  0 ...  0  0  0  0  0 17 34 65
SW p1 (78.75°) 110 88 58 41 16  4  2  0 ...  0  0  0  0 12 45
SW p2 (90°)    125 119 105 75 48 10  4  0 ...  0  0  0  0  0  0
SW p3 (101.25°)127 127 121 111 86 55 43  5  3  0 ...  0  0  0  0
```

The eye is enormous. And the value the hardware was *actually* sitting at —
`asis 81` — is exactly `(phase 0, delayline index 0)`.

Index 0, not index 15. The tuning had measured a 23-wide eye and then
selected its left-hand edge, every time.

### The cause

```c
/* mspi_timing_tuning_configs.h */
#define MSPI_TIMING_PSRAM_DTR_MODE          CONFIG_SPIRAM_MODE_HEX

/* mspi_timing_tuning.c, s_select_best_tuning_config() */
uint32_t best_point = 0;
...
#if MSPI_TIMING_PSRAM_DTR_MODE          /* undefined -> 0, silently */
    best_point = s_tuning_cfg_drv.psram_select_best_tuning_config(...);
#elif MSPI_TIMING_PSRAM_STR_MODE        /* also undefined -> 0 */
    best_point = ...
#endif
    s_tuning_cfg_drv.psram_set_best_tuning_config(timing_config, best_point);
```

`CONFIG_SPIRAM_MODE_HEX` was never defined in our hand-written
`sdkconfig.h`. An undefined identifier in `#if` evaluates to 0 with no
diagnostic. Both branches vanished, `best_point` kept its initialiser, and
the tuning always selected **config index 0** — for the phase pass and the
delayline pass alike.

The sweep still ran. It still measured the right answer. It still printed it.
Then it discarded it.

`SPIRAM_MODE_HEX` is a real IDF Kconfig symbol, `default y` for the P4. Only
the `sdkconfig.h` we wrote by hand lacked it.

### Why 80 MHz never noticed

At 80 MHz, index 0 lands *inside* the eye. PSRAM works, the walking-address
test passes over all 32 MB, and nothing anywhere suggests the tuning result
is being ignored. The bug was invisible for as long as the margin covered it.

### Finding the rest of that class

The same failure mode can hide in any other `CONFIG_` symbol the vendored
code tests but we never defined. `-Wundef` enumerates them:

```sh
make clean && make CFLAGS="... -Wundef ..." 2>&1 | grep "is not defined"
```

Most are other targets and harmless. One is worth noting:
`CONFIG_ESP_REV_MIN_FULL` is undefined, so `HAL_CONFIG(CHIP_SUPPORT_MIN_REV)`
is 0. That *happens* to select the correct paths for rev 1.0 silicon — the
MSPI wakeup workaround in, the rev-3-only AXI weight arbiter out — but it is
the same accident as bug 1 and should be defined as 100.

---

## 5. Bug 2 — no burst writes while the core is at 40 MHz

With bug 1 fixed, the failure moved:

```
p4boot:   bad 0x48000030: wrote 0xeda5a595 read 0xeda5eda5
p4boot:   8388596 of 8388608 words bad (99% )
```

The first 12 words — **48 bytes** — now read back correctly. Progress, but
still unusable.

### Splitting read from write

The key asymmetry, and it took far too long to see:

| operation at 200 MHz | result |
|---|---|
| read data that was written at 20 MHz during tuning | **0** of 128 bytes wrong |
| MR0/MR4 mode-register writes | land correctly — the readback proves it |
| 16 separate uncached 4-byte stores | **0** of 64 bytes wrong |
| first 64-byte cache-line writeback | 48 bytes land, then it stops |
| every writeback after that | nothing lands at all |
| single uncached stores after that | nothing lands either |

Reads were never the problem. Short writes were never the problem. A **burst**
write is, and once one has failed the write channel is dead for the rest of
the boot — raising the CPU afterwards does not recover it.

`MSPI_ID_3` (manual transactions) is the trusted witness throughout: its read
path is proven clean at 200 MHz and it does not involve the cache. When it
reads back the same wrong bytes the cache does, the damage is genuinely in
the chip and not in the reader.

### The shape of the damage

A 64-byte round trip, printed in full:

```
ref  ...e3eaf1f8ff060d141b222930373e454c 535a61686f767d848b9299a0a7aeb5bc
got  ...e3eaf1f8ff060d141b222930373e454c 454c454c454c454c454c454c454c535a
                                         ^^^^ one 2-byte DDR beat, repeated
```

48 bytes correct, then the beat at offset 46–47 (`45 4c`) repeats seven
times, then the beat that should have been next appears at the very end. That
is a **stalled burst**: the bus keeps being driven with the last beat while
the controller waits for data that is not arriving.

A word-level test made the offset explicit. Writing `0xA5A50000..0007`
through the cache and reading it back:

```
wrote  A5A50000 A5A50001 A5A50002 ...
read   766f6861 928b847d aea7a099 0000bcb5 0001a5a5 0002a5a5 0003a5a5
                                       ^^^^ the written data starts here
```

The written bytes appear **14 bytes late**; the head of the line keeps
whatever the cache line fill brought in.

### Extending it

Writing 512 bytes and mapping which 8-byte groups survived (`.` good,
`X` all bad):

```
SW b0 ......XX
SW b1 XXXXXXXX
SW b2 XXXXXXXX
...   XXXXXXXX
```

Not periodic. The first 48 bytes of the *entire* write land and nothing
after. The channel stalls once and never recovers.

### What the controller thought

```
SW k0 bad 16 intr 00000000 -> 00000018
SW k1 bad 64 intr 00000000 -> 00000018
SW k2 bad 64 intr 00000000 -> 00000018
SW word-after bad 64
```

`SPI_MEM_S_INT_RAW` reads `0x18` = `SLV_ST_END | MST_ST_END`, which is just
"transaction ended", twice. **No TX underflow, no RX overflow, no AXI error.**
The hardware does not think anything went wrong. That absence is itself
diagnostic — it ruled out the FIFO-error interpretations and it means nothing
in software will ever be told about this.

### The cause

The bootloader ran its walking-address test *before* raising the CPU, on
purpose, with a comment explaining why:

> Step 6 runs BEFORE the CPU clock changes, so that the whole PSRAM sequence
> — train, map, verify — happens under one set of conditions. Testing at a
> different core clock than we trained at would leave an ambiguity if it
> failed.

Perfectly sound reasoning. Also the bug. The core is at 40 MHz there while
the PSRAM bus is at 200.

A PSRAM burst cannot be paused once started, so write data has to arrive from
the AXI side at the bus rate — 200 MHz × 16 bits × 2 edges = **800 MB/s**. A
40 MHz core cannot feed that. At 80 MHz the ratio is survivable, which is
exactly why this had never shown up.

*(The starvation explanation is inference; the wedge, its permanence, and the
cure are all measured.)*

### The fix, and why the window is narrow

```
SW fast cpu 360 MHz k0 bad 0
SW fast cpu 360 MHz k1 bad 0
SW fast cpu 360 MHz k2 bad 0
```

Raise the CPU **between** training and the first write. Both edges are
load-bearing:

- **After training** — IDF's own order is unambiguous
  (`esp_psram_chip_init()` at `cpu_start.c:645`, `esp_clk_init()` at 830),
  and training at 360 MHz had previously produced a PSRAM that answered every
  read with the halfwords swapped.
- **Before any burst write** — this bug.

One control worth recording: the first attempt at this test raised the CPU
*after* a slow trial and reported `bad 64` — because the slow trial had
already wedged the channel. It was measuring the wedge, not the clock. Order
the experiment wrong and it tells you the opposite of the truth.

---

## 6. Everything that was ruled out

Most of the elapsed time went here. All measured on hardware, all negative:

| hypothesis | how it was tested | result |
|---|---|---|
| Read dummy cycles, cache path | `MSPI_ID_2` rd 10..40 × wr 2..24, variable dummy on and off | zero matches, ever |
| Read dummy cycles, manual path | `MSPI_ID_3` rd 16..36 | **completely flat**, ~41/64 bad at every value |
| Buffer alignment | stack vs static buffer, both 4-byte aligned | 82 vs 81 — identical behaviour |
| Stale tuned registers | re-apply tuned regs; re-run whole tuning and replay at once | still fails |
| Pin drive strength | `mspi_timing_ll_pin_drv_set()` 0,1,2,3 | no change |
| Split transactions | `enable_wr_splice`/`rd_splice` off | **worse** (offset 14 instead of 0) |
| Page size | 2048 → 1024 | no change |
| Bus divider | 400/2,3,4,5,6 = 200..66 MHz | slower is *worse* — tuning is frequency-specific |
| MPLL wrong | `div = 400/20-1 = 19`, `40 × 20 / 2 = 400` | correct |
| Vendored sources drifted | `diff` against `C:/esp/v6.1-beta1/esp-idf` | byte-identical |

The dummy-cycle result deserves a note: sweeping it changed **nothing at
all**, on either controller. That flatness is the tell — with variable dummy
enabled the controller derives the count from DQS and the register is inert.
A flat sweep is not a null result, it is evidence the knob is disconnected.

---

## 7. Lessons about instruments

The technical content above is worth less than this section.

**Every sweep run before the wedge was understood measured nothing.** The
first write round trip after init returned 16 bad and every one after
returned 64, so *whatever* was being swept, the output read:

```
SW drv0 16 64 64 64 64
SW dqs0 16 64 64 64 64 64 ...
```

That shape is the wedge, not the knob. It appeared in the drive-strength
sweep, the delayline sweep and the write-dummy sweep, and each time it was
briefly read as "the first value is best". A control — same operation
repeated with *no* configuration change at all — is what exposed it, and it
should have been the first thing run, not the tenth.

**Counting bad bytes was the wrong metric.** It conflates "the data is late"
with "the data is wrong". Switching the question to *where does `ref[0]`
actually appear* turned an unreadable bad-count into "14 bytes late", which
was immediately interpretable.

**Two stacked bugs look like no progress.** Either bug alone produced total
corruption. Fixing bug 1 moved the failure from "100% of words" to "99% of
words" — which looks like nothing and was in fact most of the way there.

**A successful self-test is not a working subsystem.** The tuning printed a
textbook eye every boot while the hardware ran at the eye's edge. Between the
measurement and the configuration sat one undefined macro.

This repeats a lesson already recorded in this project from the Ethernet
work, where *"the interrupt asserts once and never again"* turned out to be
an `awk` pattern matching the wrong column of `/proc/interrupts`:

> The measurement was fine; the instrument was wrong.

---

## 8. A trap worth knowing about

Halfway through, a diagnostic build pushed `bootloader.bin` from 24,416 to
24,672 bytes. We are written at `0x2000`; the partition table lives at
`0x8000`. espflash wrote straight through it, and the failure arrived one
boot later as:

```
p4boot: no kernel partition (type 0x40) -- has `make flash-parts` run?
```

which reads like a partition-table problem and is not one. The build now
refuses:

```make
@sz=`wc -c < $@`; \
 if [ $$sz -gt 24576 ]; then \
     echo "*** $@ is $$sz bytes; 0x2000 + $$sz runs past the partition"; \
     echo "*** table at 0x8000. Budget is 24576. Shrink before flashing."; \
     rm -f $@; exit 1; \
 fi
```

---

## 9. The working configuration

Everything that has to be true, in one place.

### sdkconfig.h

```c
#define CONFIG_SPIRAM                           1
#define CONFIG_SPIRAM_MODE_HEX                  1    /* bug 1 */
#define CONFIG_SPIRAM_SPEED_200M                1
#define CONFIG_SPIRAM_SPEED                     200
#define CONFIG_MMU_PAGE_SIZE                    0x10000
#define CONFIG_ESPTOOLPY_FLASHFREQ_40M          1

/* deliberately NOT defined:
 *   CONFIG_SPIRAM_USE_8LINE_MODE   forces a 16-line part onto 8 lanes
 *   CONFIG_SPIRAM_ECC_ENABLE       costs 1/8 of capacity
 *   CONFIG_SPIRAM_TIMING_TUNING_POINT_VIA_TEMPERATURE_SENSOR
 */
```

`CONFIG_SPIRAM_USE_8LINE_MODE` is worth its own warning. The name reads like
"this is an 8-line part", but it is an **override** that forces a 16-line HEX
PSRAM onto 8 data lanes. Setting it cost an evening in an earlier session:
PSRAM trained, reported itself correctly, and read back with the bytes
skewed — indistinguishable from a signal-integrity failure.

### Derived values at 200 MHz

| constant | 200 MHz | 80 MHz |
|---|---|---|
| `AP_HEX_PSRAM_MPLL_DEFAULT_FREQ_MHZ` | 400 | 320 |
| bus divider (`MPLL / CONFIG_SPIRAM_SPEED`) | 2 | 4 |
| `MSPI_TIMING_CORE_CLOCK_DIV` | 1 | 1 |
| `AP_HEX_PSRAM_RD_LATENCY` (MR0) | 4 → 14 cycles | 2 → 10 cycles |
| `AP_HEX_PSRAM_WR_LATENCY` (MR4) | 1 | 2 |
| `AP_HEX_PSRAM_RD_DUMMY_BITLEN` | `2*(14-1)` = 26 | `2*(10-1)` = 18 |
| `AP_HEX_PSRAM_WR_DUMMY_BITLEN` | `2*(7-1)` = 12 | `2*(5-1)` = 8 |

Registers read back from working hardware, for comparison when something
looks wrong:

```
SPI_MEM_S_SRAM_CLK    (ID_2 bus clock)  0x00010001   divider 2
SPI1_MEM_S_CLOCK      (ID_3 bus clock)  0x00010001   divider 2
SPI_MEM_S_SMEM_DDR                      0x00003023   ddr_en=1 var_dummy=1
                                                     rdat_swp=0 wdat_swp=0
                                                     outminbytelen=1
                                                     tx/rx_ddr_msk_en=1
SPI_MEM_S_INT_RAW     after a transfer  0x00000018   SLV_ST_END|MST_ST_END
```

### Clock ordering

```
LDO ch2 1.8 V  ->  MPLL 400  ->  PSRAM train (core 40 MHz)
                                      |
                                 raise core to 360
                                      |
                                 first burst write
```

### Speeds that are not options

- **120 / 160 MHz** — the Kconfig choice offers only 20/80/200/250, and the
  latency constants have exactly three branches (250M, 200M, else). An
  in-between speed silently uses the 10-cycle latencies meant for 80 MHz.
- **250 MHz** — rev-3 silicon only. IDF gates `SPIRAM_SPEED_250M` on
  `!ESP32P4_SELECTS_REV_LESS_V3`; this board is rev 1.0. 200 MHz has no such
  gate, which is what made it worth chasing.

---

## 10. Verification

What "it works" was allowed to mean:

```
p4boot: PSRAM 32768 KB mapped at 0x48000000 (200 MHz)
p4boot: CPU at 40 MHz, raising to 360...
p4boot: CPU now at 360 MHz (readback)
p4boot: PSRAM 32768 KB verified
p4boot: psram alias: UNCACHED and coherent -- dma_alloc_coherent() can work
p4boot:   kernel verified in PSRAM (crc32 0xbc7fb3f3)
p4boot:   dtb verified in PSRAM (crc32 0x45e925ca)
```

- Walking-address test over all 32 MB: **9 cold boots out of 9**
- Kernel and DTB CRC32 after the copy into PSRAM: match flash
- Linux 6.18.35 boots to userspace; `memtester 6M 1` completes, no failures
- `md5sum` of a 4 MB tmpfs file, twice: identical
- SD card reads: identical across repeats
- DHCP `192.168.1.20`; gateway ping 0% loss at ~1.1 ms
- `dmesg` matches for oops/panic/segfault/bad page/corrupt: **0**

Throughput, same command and same harness either side of the change:

| | 80 MHz | 200 MHz |
|---|---|---|
| `memtester 6M 1` | 159.2 s | **104.4 s** |

1.52× on a real workload, which is roughly what a 2.5× bus-clock increase
should give once page allocation and the walking-pattern CPU work are
included.

---

## 11. Reproducing the investigation

The diagnostic harness is not committed — it was a single file wired into
`SRCS_C` and called from `main()`, rewritten about a dozen times. The shape
that worked:

```c
void p4_psram_check(const char *where);   /* called from main.c step 6 */
```

Useful properties, learned the hard way:

- **Keep it under the size budget.** 24,576 bytes total, including the
  vendored code. Debug log level 4 costs roughly 1.4 KB of format strings.
- **Read with `MSPI_ID_3`.** `mspi_timing_config_psram_read_data()` is
  non-static and its read path is proven clean; it is the only trustworthy
  witness for what is physically in the chip.
- **Use a fresh 64-byte-aligned line per iteration.** Rewriting a line that
  was already corrupted refills it from the corrupt copy and measures
  nothing.
- **Never sweep the write dummy with the cache live.** A bad value hangs
  `cache_hal_writeback_addr()` outright — two boots were lost to this.
- **Always include a no-change control.** See §7.

Host-side scripts used (in the session scratchpad, not the repo):
`bootlog.py` (reset + capture raw bytes to a file — the console is cp1252 and
the serial stream is not, so decoding on stdout throws), `p4test.py` (reset,
log in, run a battery), `reboots.py` (N cold boots, check each reaches
`login:` with PSRAM verified).

---

## 12. Files

| file | what changed |
|---|---|
| `bootloader/sdkconfig.h` | `CONFIG_SPIRAM_MODE_HEX`; speed 80 → 200; rationale rewritten |
| `bootloader/main.c` | CPU raise moved between training and first write |
| `bootloader/p4/p4_psram.h` | ordering contract documented on both edges |
| `bootloader/Makefile` | 24576-byte image size guard |
| `docs/STATUS-2026-09-12.md` | session summary |
| `docs/PSRAM-200MHZ.md` | this file |

Related reading in this repo: `docs/BOOTLOADER-LINK.md` for how the vendored
IDF sources are linked without the framework, `bootloader/vendor/MANIFEST.md`
for exactly which IDF files are compiled in and where they came from.
