/* Reset entry for the ESP32-P4 second-stage bootloader.
 *
 * The ROM first-stage has already loaded our segments from flash into SRAM and
 * jumps here. Nothing is set up: no stack, no gp, and .bss holds whatever was
 * in SRAM beforehand.
 *
 * Reviewed and approved before first flash.
 */

    .section .text.start, "ax"
    .global _start
    .type   _start, @function
_start:
    /* gp, for gp-relative access to .sdata/.sbss.
     *
     * .option norelax is load-bearing and its failure mode is silent: with
     * relaxation on, the linker rewrites `la gp, __global_pointer$` into a
     * gp-relative load of gp itself, which is circular -- gp ends up holding
     * garbage and every small-data access is wrong. This is the standard
     * incantation for exactly that reason. */
    .option push
    .option norelax
    la      gp, __global_pointer$
    .option pop

    la      sp, _stack_top

    /* Interrupts off for the whole of init. The ROM may have left CLIC
     * sources enabled, and clearing mstatus.MIE alone would leave them
     * pending rather than masked. */
    csrw    mie, zero
    csrci   mstatus, 0x8

    /* Point mtvec at our own handler, in DIRECT mode (low two bits zero).
     *
     * Until this runs, any exception goes wherever the ROM left mtvec, and a
     * fault is indistinguishable from a hang on a board whose only output is
     * a serial line. Set it before anything that can fault -- which is
     * everything after this instruction. */
    la      t0, trap_entry
    csrw    mtvec, t0

    /* Zero .bss. The ROM loads .data for us (it is a real segment in the
     * image), but .bss is NOLOAD and holds whatever the previous boot left
     * in SRAM. */
    la      t0, _bss_start
    la      t1, _bss_end
1:
    bgeu    t0, t1, 2f
    sw      zero, 0(t0)
    addi    t0, t0, 4
    j       1b
2:

    call    main

    /* main() jumps to Linux and never returns. If it does, something failed
     * in a way that could not report itself -- park rather than run off into
     * whatever follows in memory. */
3:
    wfi
    j       3b
    .size   _start, . - _start
