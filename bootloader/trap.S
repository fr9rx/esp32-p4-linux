/* Trap handler for the ESP32-P4 second-stage bootloader.
 *
 * Without this, mtvec holds whatever the ROM left in it and any exception --
 * a bad load, a misaligned access, an illegal instruction -- either vanishes
 * into ROM code or spins silently. On a board whose only instrument is a
 * serial line, a silent hang and an unhandled fault look identical, and they
 * need completely different fixes.
 *
 * This never returns, so there is nothing to preserve: it reads the three
 * CSRs that say what happened, hands them to C, and that prints and halts.
 *
 * mtvec is written in DIRECT mode (low two bits 0), so every cause vectors
 * here rather than to an offset table. The P4's CLIC supports vectored mode,
 * but we take no interrupts -- start.S clears mie and mstatus.MIE -- so the
 * only thing arriving is a synchronous exception.
 */

    .section .text, "ax"
    .global trap_entry
    .align  4                   /* mtvec requires 4-byte alignment; the low
                                 * bits are the mode field, so a misaligned
                                 * handler silently selects vectored mode */
    .type   trap_entry, @function
trap_entry:
    csrr    a0, mcause
    csrr    a1, mepc
    csrr    a2, mtval

    /* Fresh stack. Whatever faulted may have been mid-way through corrupting
     * sp, and a trap handler that faults on its own prologue reports nothing
     * at all. */
    la      sp, _stack_top

    call    trap_report          /* noreturn */

1:  wfi
    j       1b
    .size   trap_entry, . - trap_entry
