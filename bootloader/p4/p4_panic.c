/* Freestanding stand-ins for the few C library symbols vendored ESP-IDF code
 * reaches for.
 *
 * We link -nostdlib, so there is no libc. Only libgcc comes along, and that
 * supplies arithmetic helpers (__udivdi3 and friends) rather than anything
 * like this.
 */

#include <stdint.h>

extern int ets_printf(const char *fmt, ...);

/* Called by vendored HAL code down paths that "cannot happen" -- an
 * unsupported clock source, an out-of-range divider.
 *
 * Upstream this reboots. Here it stops, loudly, because in a bootloader a
 * reboot loop is the worst possible outcome: the board resets before you can
 * read why, and it looks like a hardware fault rather than a bad argument.
 * Halting leaves the message on the console and the debugger attachable.
 *
 * noreturn matters: callers assume control does not come back, and without it
 * the compiler generates a return path through a function that has none. */
/* periph_rcc_enter/exit: the two halves of PERIPH_RCC_ATOMIC(), which
 * vendored code wraps around peripheral clock-enable writes. Upstream they
 * take a spinlock in esp_hw_support/periph_ctrl.c, which NON_OS_BUILD
 * excludes from the build.
 *
 * No-ops are correct, not merely convenient: one hart, no scheduler, and
 * mstatus.MIE is clear for the whole of init. There is nothing to serialise
 * against. If the second HP core were ever started before PSRAM comes up,
 * this would stop being true -- see docs/MULTICORE.md. */
void periph_rcc_enter(void) { }
void periph_rcc_exit(void) { }

__attribute__((noreturn)) void abort(void)
{
    ets_printf("\r\np4boot: abort() -- unreachable path taken in vendored code\r\n");
    ets_printf("p4boot: halting rather than resetting, so this stays readable\r\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* ESP-IDF's log macros stamp every line with this. Nothing here has a tick
 * counter yet -- SYSTIMER belongs to Linux and we do not start it -- and a
 * timestamp adds nothing to a boot that takes under a second and prints in
 * strict order. Zero keeps the log macros linkable at levels above 0, which
 * is what makes the vendored PSRAM driver's own chip report readable. */
uint32_t esp_log_timestamp(void)
{
    return 0;
}

/* Decode the machine cause register. Only the synchronous exceptions are
 * listed -- interrupts never arrive here, because start.S clears mie and
 * mstatus.MIE and nothing re-enables them. */
static const char *trap_cause_name(uint32_t mcause)
{
    if (mcause & 0x80000000u) {
        return "interrupt (unexpected -- mie should be clear)";
    }
    switch (mcause) {
    case 0:  return "instruction address misaligned";
    case 1:  return "instruction access fault";
    case 2:  return "illegal instruction";
    case 3:  return "breakpoint";
    case 4:  return "load address misaligned";
    case 5:  return "load access fault";
    case 6:  return "store/AMO address misaligned";
    case 7:  return "store/AMO access fault";
    case 11: return "environment call from M-mode";
    default: return "unknown";
    }
}

/* Called from trap_entry in trap.S with the three registers that say what
 * happened. Never returns.
 *
 * mepc is the instruction that faulted -- look it up in bootloader.lst
 * (`make dis`). mtval is the offending address for an access fault, which on
 * this board most often means a load from PSRAM that is not mapped yet, or a
 * peripheral outside a window we are allowed to touch. */
__attribute__((noreturn))
void trap_report(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    ets_printf("\r\n=== p4boot: TRAP ===\r\n");
    ets_printf("  mcause 0x%08x  %s\r\n", (unsigned)mcause, trap_cause_name(mcause));
    ets_printf("  mepc   0x%08x  <- faulting instruction (find it in `make dis`)\r\n",
               (unsigned)mepc);
    ets_printf("  mtval  0x%08x  <- offending address, for access faults\r\n",
               (unsigned)mtval);
    ets_printf("=== halting ===\r\n");
    for (;;) {
        __asm__ volatile("wfi");
    }
}

/* What a failed assert() from <assert.h> lands in. Vendored MSPI tuning code
 * asserts on its own invariants, and with no libc there is nothing to catch
 * it.
 *
 * Worth printing properly rather than just halting: an assert firing inside
 * the timing tuning is a real diagnostic -- it says the tuning reached a
 * state it did not expect, which on this board most likely means PSRAM is
 * marginal at the configured speed. The file and line are how you tell that
 * apart from a plain crash. */
__attribute__((noreturn))
void __assert_func(const char *file, int line, const char *func, const char *expr)
{
    ets_printf("\r\np4boot: assertion failed: %s\r\n", expr ? expr : "?");
    ets_printf("p4boot:   at %s:%d, in %s()\r\n",
               file ? file : "?", line, func ? func : "?");
    abort();
}

/* Why did the board restart?
 *
 * Worth printing on every boot. A bootloader banner looks identical whether
 * it followed a power-on, a watchdog bite, a brownout or a CPU lockup, and
 * those call for completely different investigations. rtc_get_reset_reason
 * lives in ROM at 0x4fc00018 and survives the reset that it describes.
 *
 * RESET_REASON_CPU_LOCKUP is the interesting one for a kernel bring-up: the
 * ROM's own comment is "exception inside the exception handler", i.e. a fault
 * taken before anything could report it. That is exactly what a jump into a
 * peripheral whose clock is gated looks like from the outside.
 */
const char *p4_reset_reason_name(uint32_t r)
{
    switch (r) {
    case 0x01: return "power-on";
    case 0x03: return "software (core)";
    case 0x05: return "deep sleep / PMU power down";
    case 0x07: return "MWDT (core)";
    case 0x09: return "RWDT (core)";
    case 0x0B: return "MWDT (CPU)";
    case 0x0C: return "software (CPU)";
    case 0x0D: return "RWDT (CPU)";
    case 0x0F: return "BROWNOUT -- VDD sagged";
    case 0x10: return "RWDT (system)";
    case 0x12: return "super watchdog";
    case 0x13: return "power glitch";
    case 0x14: return "eFuse CRC error";
    case 0x16: return "USB JTAG";
    case 0x17: return "USB UART";
    case 0x18: return "JTAG reset command";
    case 0x1A: return "CPU LOCKUP -- fault inside the fault handler";
    case 0x1B: return "power supply glitch (>50ns)";
    default:   return "unknown";
    }
}
