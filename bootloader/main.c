/* ESP32-P4 second-stage bootloader for native NOMMU Linux.
 *
 * Boots: ROM first-stage -> us (flash 0x2000) -> Linux (PSRAM 0x48000000).
 * There is no ESP-IDF in this chain.
 *
 * This file is the readable top level: every step is a named call, so the
 * whole boot flow fits on a screen. Register-level work lives in p4/, reused
 * ESP-IDF sources in vendor/.
 *
 * Build order is deliberate and each step must print before the next is
 * added -- with no display, UART0 is the only instrument on this board.
 * Currently implemented: steps 1-7 (through the flash payloads verified).
 *
 * The step NUMBERS are the plan's, so they do not run in numeric order:
 * step 7 (flash) sits before step 5 (PSRAM) because flash and PSRAM share
 * the MSPI controller and reading flash first keeps the two independent.
 * p4/p4_flash.c makes the full argument.
 */

#include <stdint.h>
#include <stddef.h>

#include "sdkconfig.h"

#include "p4/p4_wdt.h"
#include "p4/p4_analog.h"
#include "p4/p4_uart.h"
#include "p4/p4_clk.h"
#include "p4/p4_cache.h"
#include "p4/p4_psram.h"
#include "p4/p4_flash.h"
#include "p4/p4_sdmmc.h"
#include "p4/p4_emac.h"

/* The console rate the device tree and the kernel command line both assume:
 * chosen/stdout-path = "serial0:4000000n8", CONFIG_CMDLINE console=ttyS0,4000000.
 * Change one of the three and you must change all three. */
#define CONSOLE_BAUD 4000000
#define XTAL_HZ      40000000

/* The ceiling on rev 1.0. See p4_clk.h -- 400 MHz is rev >= 3 only. */
#define CPU_MHZ      360

/* Where the payloads land in PSRAM.
 *
 * KERNEL_PA is the base of the memory node the device tree declares
 * (memory@48000000, reg = <0x48000000 0x02000000>), which is also where the
 * kernel is linked to run.
 *
 * DTB_PA is 16 MB in. It used to be 8 MB, which was past the end of the 6 MB
 * kernel partition -- but the kernel partition grew to 10 MB when the root
 * filesystem became a built-in initramfs, and the copy would then have
 * written straight over the device tree. Moved rather than merely nudged, so
 * the same collision does not come back the next time the kernel grows:
 *
 *   0x48000000  kernel, 10 MB copied      -> ends 0x48A00000
 *   0x49000000  dtb                       -> 6 MB of headroom below it
 *   0x49600000  nommu_userspace_pool      -> reserved by the device tree
 *
 * Still inside the 32 MB memory node, so Linux can reach it, and it needs no
 * reserved-memory entry: the kernel reserves the FDT it was handed. */
#define KERNEL_PA    0x48000000u
#define DTB_PA       0x49000000u

/* ROM functions, resolved by vendor/ld/esp32p4.rom.eco0_4.ld at link time.
 * No code vendored for any of these. */
extern int  ets_printf(const char *fmt, ...);
extern void ets_delay_us(uint32_t us);

/* ROM cache maintenance, 0x4fc00414. Writes every dirty line back to memory;
 * needed before jumping to code we wrote through the cache. */
extern void Cache_WriteBack_All(void);

/* ROM, 0x4fc00018. Survives the reset it reports on. */
extern uint32_t esp_rom_get_reset_reason(int cpu_no);
extern const char *p4_reset_reason_name(uint32_t r);

/* jump.S. Never returns. */
extern void jump_to_linux(uint32_t entry, uint32_t dtb_pa);

#define BANNER "\r\n=== p4boot: ESP32-P4 second-stage bootloader ===\r\n"

int main(void)
{
    /* Step 1. If this line appears, the image format, the linker script, the
     * ROM handoff and our .text placement are all correct. It is the cheapest
     * possible proof that we are executing at all.
     *
     * Still at the ROM's 115200 here -- the reprogram to 4 Mbps is step 4. */
    ets_printf(BANNER);
    ets_printf("p4boot: alive, running from SRAM\r\n");


    /* Step 2. Silence the watchdogs the ROM leaves armed.
     *
     * Skipping this does not look like a watchdog problem: the board simply
     * resets a second or two in, which reads exactly like a crash in whatever
     * code happens to be running at the time. */
    p4_wdt_disable_all();
    ets_printf("p4boot: watchdogs disabled\r\n");

    /* Step 3. Analog domain: the analog-i2c master, the rev-1.0 CPLL/SPLL
     * step-down, the bias trim that makes it hold, and the brownout detector.
     *
     * First step whose failure mode is a dead console rather than a message,
     * so the print after it is the whole test. */
    p4_analog_init();
    ets_printf("p4boot: analog init done (CPLL 400M, SPLL 480M, bias trimmed)\r\n");

    /* Step 4. Console to 4 Mbps.
     *
     * Everything after this line arrives at the new rate, so reconnect at
     * 4000000 to read it. That is the test: the banner below is either clean
     * text or it is noise, with no ambiguity.
     *
     * Source stays XTAL at 40 MHz because the device tree says so -- the
     * kernel's esp32_uart derives its own divisor from a 40 MHz fixed-clock
     * node, so moving the source would make the DTS wrong. 40e6/4e6 is
     * exactly 10 with no fractional part, which 115200 cannot manage. */
    ets_printf("p4boot: switching console to %d baud...\r\n", CONSOLE_BAUD);
    if (p4_uart0_set_baud(CONSOLE_BAUD, XTAL_HZ)) {
        ets_printf("\r\np4boot: console now at %d baud (xtal %d Hz)\r\n",
                   CONSOLE_BAUD, XTAL_HZ);
    } else {
        /* Still readable at the old rate, which is the point of returning a
         * status rather than hanging. */
        ets_printf("p4boot: BAUD CHANGE REFUSED, staying at ROM rate\r\n");
    }

    /* Say why we are here.
     *
     * Deliberately after the baud switch, not with the opening banner: at
     * 115200 this line lands in the noise that a 4 Mbps console shows before
     * the switch, which is exactly where you cannot read it.
     *
     * On a board being brought up, most reboots are not the one you intended,
     * and a bootloader banner looks identical whether it followed a power-on,
     * a watchdog bite, a brownout or a CPU lockup. Those are unrelated
     * investigations, and this picks the right one before time is spent
     * guessing. */
    {
        uint32_t rr = esp_rom_get_reset_reason(0);
        ets_printf("p4boot: reset reason 0x%02x (%s)\r\n",
                   (unsigned)rr, p4_reset_reason_name(rr));
    }

    /* Step 7. Locate the payloads in flash and prove they are intact.
     *
     * BEFORE PSRAM, which is out of numeric order and deliberate. Flash and
     * PSRAM share the MSPI controller: the DQS tuning inside step 5 rewrites
     * timing registers that the ROM's flash routines are also driving. Doing
     * the reads here means they happen with the controller exactly as the ROM
     * left it, so anything that fails is this code's fault and nothing else's.
     * Step 8 will re-CRC after the tuning has been through those registers,
     * which turns "the tuning probably left flash alone" into a checked
     * claim. p4/p4_flash.c has the long version.
     *
     * The table is dumped before the lookups because "kernel partition not
     * found" and "the table was never written" are very different problems
     * and cost nothing to tell apart. */
    if (!p4_flash_init()) {
        ets_printf("p4boot: flash: could not set chip size -- cannot continue\r\n");
        for (;;) { }
    }
    /* Prove the flash can be read repeatably before trusting a single CRC of
     * it. After a warm reset it cannot -- see p4_flash.h. Refusing here turns
     * "boots a corrupted kernel and dies somewhere strange" into one line of
     * console, and a hard reset (or the reset button) clears it. */
    if (!p4_flash_selftest()) {
        ets_printf("p4boot: flash reads are not repeatable -- refusing to boot\r\n");
        ets_printf("p4boot: power-cycle or press reset; a cold boot clears this\r\n");
        for (;;) { }
    }

    p4_part_dump();

    p4_partition_t kernel;
    p4_partition_t dtb;

    if (!p4_part_find(P4_PART_TYPE_KERNEL, P4_PART_SUBTYPE_NONE, &kernel)) {
        ets_printf("p4boot: no kernel partition (type 0x%02x) -- "
                   "has `make flash-parts` run?\r\n", P4_PART_TYPE_KERNEL);
        for (;;) { }
    }
    if (!p4_part_find(P4_PART_TYPE_DTB, P4_PART_SUBTYPE_NONE, &dtb)) {
        ets_printf("p4boot: no dtb partition (type 0x%02x)\r\n", P4_PART_TYPE_DTB);
        for (;;) { }
    }

    /* CRC covers the WHOLE partition, not just the payload.
     *
     * Nothing on flash records how long the payload is, and inventing a
     * container to say so would tie this bootloader to the kernel it boots --
     * a bootloader should not need rebuilding when the kernel changes. So the
     * 0xFF tail is included, which is stable because `make flash-kernel`
     * erases the region before writing it. `make crc` prints the numbers to
     * expect; they are computed from partitions.csv and the same artifacts. */
    uint32_t kernel_crc = 0;
    uint32_t dtb_crc = 0;

    ets_printf("p4boot: crc32 of %s (0x%08x + 0x%08x)\r\n",
               kernel.label, (unsigned)kernel.offset, (unsigned)kernel.size);
    if (!p4_flash_crc32(kernel.offset, kernel.size, &kernel_crc)) {
        ets_printf("p4boot: kernel CRC READ FAILED -- cannot continue\r\n");
        for (;;) { }
    }
    ets_printf("p4boot:   kernel crc32 0x%08x\r\n", (unsigned)kernel_crc);

    ets_printf("p4boot: crc32 of %s (0x%08x + 0x%08x)\r\n",
               dtb.label, (unsigned)dtb.offset, (unsigned)dtb.size);
    if (!p4_flash_crc32(dtb.offset, dtb.size, &dtb_crc)) {
        ets_printf("p4boot: dtb CRC READ FAILED -- cannot continue\r\n");
        for (;;) { }
    }
    ets_printf("p4boot:   dtb crc32 0x%08x\r\n", (unsigned)dtb_crc);

    /* Record the crystal frequency where rtc_clk and clk_hal can read it
     * back. Must precede anything that configures a PLL -- MPLL included. */
    p4_clk_init();

    /* Cache and MMU HALs. Not a numbered step -- it is the prerequisite the
     * numbered ones assume. Both HALs hold configuration that every later
     * call reads, and without it esp_psram_impl_enable() hangs inside a cache
     * suspend that never returns. */
    p4_cache_init();
    ets_printf("p4boot: cache/MMU HAL initialised\r\n");

    /* Step 5. PSRAM: train the chip, then map it.
     *
     * Two separate things. esp_psram_impl_enable() brings up the chip and the
     * MSPI controller; nothing is addressable at 0x48000000 until
     * mmu_hal_map_region() has run and the cache bus has been enabled. */
    size_t psram_bytes = 0;
    if (!p4_psram_init(&psram_bytes)) {
        ets_printf("p4boot: PSRAM INIT FAILED -- cannot continue\r\n");
        for (;;) { }
    }
    ets_printf("p4boot: PSRAM %u KB mapped at 0x%08x (%d MHz)\r\n",
               (unsigned)(psram_bytes / 1024), P4_PSRAM_VADDR, CONFIG_SPIRAM_SPEED);

    /* Step 6 runs BEFORE the CPU clock changes, so that the whole PSRAM
     * sequence -- train, map, verify -- happens under one set of conditions.
     * Testing at a different core clock than we trained at would leave an
     * ambiguity if it failed. */
    {
        uint32_t bad0 = 0;
        if (!p4_psram_test(psram_bytes, &bad0)) {
            ets_printf("p4boot: PSRAM TEST FAILED at 0x%08x (CPU still %d MHz)\r\n",
                       (unsigned)bad0, p4_clk_cpu_get_mhz());
            for (;;) { }
        }
        ets_printf("p4boot: PSRAM %u KB verified at %d MHz CPU\r\n",
                   (unsigned)(psram_bytes / 1024), p4_clk_cpu_get_mhz());
    }

    /* Step 4b. CPU to its ceiling -- AFTER PSRAM, not before.
     *
     * 360 MHz, not 400: IDF's Kconfig.cpu caps rev < 3 silicon at 360, and
     * this board is rev 1.0. rtc_clk_cpu_freq_mhz_to_config() refuses 400
     * rather than mis-clocking, so the readback below is the check.
     *
     * ORDER MATTERS AND I HAD IT BACKWARDS. This used to run before PSRAM,
     * on the reasoning that MSPI timing tables are indexed by core clock. That
     * is true of the table-driven tuning schemes; the P4's PSRAM uses the DQS
     * sweep in mspi_timing_by_dqs.c, which has no tables at all. Meanwhile
     * ESP-IDF's own order is unambiguous -- esp_psram_chip_init() at
     * cpu_start.c:645, esp_clk_init() at line 830 -- PSRAM first, CPU second.
     * Training at 360 MHz produced a PSRAM that answered every read with the
     * halfwords swapped. */
    ets_printf("p4boot: CPU at %d MHz, raising to %d...\r\n",
               p4_clk_cpu_get_mhz(), CPU_MHZ);
    if (p4_clk_cpu_set_mhz(CPU_MHZ)) {
        ets_printf("p4boot: CPU now at %d MHz (readback)\r\n",
                   p4_clk_cpu_get_mhz());
    } else {
        ets_printf("p4boot: CPU %d MHz REFUSED, still at %d MHz\r\n",
                   CPU_MHZ, p4_clk_cpu_get_mhz());
    }

    /* Step 6. Do not trust it until it has been tested.
     *
     * This is the highest-value check in the bootloader. A mistrained DQS
     * does not announce itself -- PSRAM answers every read, just with the
     * wrong bits -- and every downstream symptom points somewhere else. If
     * this fails, drop CONFIG_SPIRAM_SPEED to 80 in sdkconfig.h; 80 MHz is
     * known to work on this board. */
    uint32_t bad = 0;
    if (!p4_psram_test(psram_bytes, &bad)) {
        ets_printf("p4boot: PSRAM TEST FAILED at 0x%08x -- refusing to boot Linux\r\n",
                   (unsigned)bad);
        for (;;) { }
    }
    ets_printf("p4boot: PSRAM %u KB verified\r\n", (unsigned)(psram_bytes / 1024));

    /* Is there an uncached window? Answered here because it cannot be
     * answered from Linux -- /dev/mem refuses addresses past high_memory, so
     * a read of the alias returns EFAULT before it reaches the bus.
     *
     * Deliberately after the PSRAM test and before the kernel copy: the
     * walking-address test has already been over every byte, so this is free
     * to scribble, and the copy that follows overwrites what it scribbles.
     *
     * Not fatal either way. A false result means Linux keeps doing
     * descriptor cache maintenance by hand, which is where it already is. */
    (void)p4_psram_alias_test();

    /* Step 8. Flash -> PSRAM, and do not believe the copy either.
     *
     * The CRCs compared here are the ones step 7 took over flash BEFORE the
     * PSRAM tuning ran. So a match proves three things at once: the copy
     * arrived intact, PSRAM holds it (p4_flash_copy writes back and
     * invalidates, so these reads come from the chip rather than from our own
     * dirty cache lines), and the DQS tuning did not disturb the flash side
     * of the shared MSPI controller. */
    ets_printf("p4boot: copying kernel to 0x%08x\r\n", KERNEL_PA);
    if (!p4_flash_copy(kernel.offset, kernel.size, (void *)(uintptr_t)KERNEL_PA)) {
        ets_printf("p4boot: kernel COPY FAILED -- refusing to boot\r\n");
        for (;;) { }
    }
    uint32_t kernel_copy_crc = p4_mem_crc32((const void *)(uintptr_t)KERNEL_PA,
                                            kernel.size);
    if (kernel_copy_crc != kernel_crc) {
        ets_printf("p4boot: kernel CRC MISMATCH: flash 0x%08x, psram 0x%08x\r\n",
                   (unsigned)kernel_crc, (unsigned)kernel_copy_crc);
        ets_printf("p4boot: refusing to boot a kernel that did not copy cleanly\r\n");
        for (;;) { }
    }
    ets_printf("p4boot:   kernel verified in PSRAM (crc32 0x%08x)\r\n",
               (unsigned)kernel_copy_crc);

    ets_printf("p4boot: copying dtb to 0x%08x\r\n", DTB_PA);
    if (!p4_flash_copy(dtb.offset, dtb.size, (void *)(uintptr_t)DTB_PA)) {
        ets_printf("p4boot: dtb COPY FAILED -- refusing to boot\r\n");
        for (;;) { }
    }
    uint32_t dtb_copy_crc = p4_mem_crc32((const void *)(uintptr_t)DTB_PA, dtb.size);
    if (dtb_copy_crc != dtb_crc) {
        ets_printf("p4boot: dtb CRC MISMATCH: flash 0x%08x, psram 0x%08x\r\n",
                   (unsigned)dtb_crc, (unsigned)dtb_copy_crc);
        for (;;) { }
    }
    ets_printf("p4boot:   dtb verified in PSRAM (crc32 0x%08x)\r\n",
               (unsigned)dtb_copy_crc);

    /* A last look at what the payload actually starts with, before we jump
     * into it. Costs one line and distinguishes "jumped into an empty
     * partition" from "jumped into a kernel that then died quietly" -- which
     * from the console look identical. */
    ets_printf("p4boot: first words at 0x%08x: 0x%08x 0x%08x\r\n",
               KERNEL_PA,
               (unsigned)*(volatile uint32_t *)(uintptr_t)KERNEL_PA,
               (unsigned)*(volatile uint32_t *)(uintptr_t)(KERNEL_PA + 4));

    /* Step 11. Power, clock and pin-mux the SD slot.
     *
     * Before the jump, not after -- obviously -- but also before it for a
     * less obvious reason: there is no ESP32-P4 pinctrl or clock-controller
     * driver upstream, so mainline dw_mmc probes the controller assuming
     * someone else already turned it on. Skipping this does not produce a
     * missing card, it produces a bus fault the moment the snps,dw-mshc node
     * is probed, and the board resets with no message. That is precisely how
     * this failed the first time: Linux reached
     *
     *   gpio gpiochip0: Static allocation of GPIO base is deprecated
     *
     * and rebooted, with dw-mshc next in the tree.
     *
     * A failure here is NOT fatal to booting. The kernel command line has
     * rootwait, so without a card Linux waits at the console rather than
     * panicking -- and a console we can read is worth more than an early
     * halt. So this reports and continues. */
    if (!p4_sdmmc_init()) {
        ets_printf("p4boot: SDMMC init failed -- booting anyway; "
                   "expect Linux to wait on rootwait\r\n");
    }

    /* Step 11b. The Ethernet MAC, on the same terms.
     *
     * Also not fatal, and for a better reason than SDMMC's: nothing in the
     * boot path needs the network. If this is wrong the kernel either fails
     * to find a PHY or brings up an interface that never carries a frame,
     * and in both cases there is still a console to read about it on. */
    /* Before the EMAC claims GPIO 50: is anything driving it? */
    p4_emac_refclk_probe();

    if (!p4_emac_init()) {
        ets_printf("p4boot: EMAC init failed -- booting anyway, no Ethernet\r\n");
    } else {
        /* Ask the bus whether a PHY is there, before Linux gets the chance to
         * say "no phy found" without saying why. Costs a few milliseconds and
         * separates three failures that are indistinguishable from Linux:
         * MDC/MDIO muxing, the MDC divider, and no PHY on the board at all. */
        p4_emac_mdio_scan();
    }

    /* Steps 9/10. Hand over.
     *
     * Cache_WriteBack_All() belongs here even though p4_flash_copy() already
     * wrote back each range it copied: this is the total, cheap version of a
     * guarantee we cannot afford to get subtly wrong, and it also covers
     * anything else that touched PSRAM (the step 6 walking-address test wrote
     * all 32 MB of it). On this core an instruction fetch does not observe
     * dirty L1-D lines, so a missed writeback means jumping into stale
     * memory -- which cost a boot once already. */
    ets_printf("p4boot: cache writeback, then jumping to 0x%08x (dtb 0x%08x)\r\n",
               KERNEL_PA, DTB_PA);
    Cache_WriteBack_All();

    jump_to_linux(KERNEL_PA, DTB_PA);

    /* jump_to_linux() does not return. If it somehow does, say so rather than
     * falling off the end of main() into start.S's parking loop. */
    ets_printf("p4boot: RETURNED FROM THE KERNEL -- this should be impossible\r\n");
    for (;;) { }
}
