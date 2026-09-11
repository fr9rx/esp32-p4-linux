/* The handoff to Linux.
 *
 * jump_to_linux(a0 = entry, a1 = dtb_pa) -- never returns.
 *
 * The RISC-V Linux boot protocol wants, at the kernel's first instruction:
 *   a0 = hartid of the booting hart
 *   a1 = physical address of the flattened device tree
 * and nothing else guaranteed. Everything below is about making sure those
 * two registers are right and that nothing we did earlier is still in flight.
 *
 * No section attribute is needed. The whole bootloader is SRAM-resident and
 * nothing of ours lives in PSRAM, so this code cannot be overwritten by what
 * it loads -- the hazard that forced IRAM_ATTR on do_boot() in the old
 * hypervisor shim, where the loader was overwriting 0x48000000 while running
 * from it.
 */

    .section .text, "ax"
    .global jump_to_linux
    .type   jump_to_linux, @function

jump_to_linux:
    /* Stash both arguments before touching a0/a1, since a0 is about to be
     * overwritten with the hartid. */
    mv      t0, a0                  /* entry */
    mv      t1, a1                  /* dtb physical address */

    /* Interrupts off, twice over.
     *
     * csrci clears mstatus.MIE, the global enable. csrw mie, zero masks every
     * individual source as well. The second is insurance rather than
     * duplication: the global bit alone leaves sources armed, so anything
     * Linux enables in mstatus before it has installed its own vector table
     * would take a trap through OUR mtvec. Belt and braces cost two
     * instructions here and would cost a very confusing morning otherwise. */
    csrci   mstatus, 0x8
    csrw    mie, zero

    /* mstatus.FS is deliberately LEFT ALONE.
     *
     * The plan floated clearing it to Off, on the reasoning that we must not
     * hand over dirty FP state. But this bootloader is built -march=rv32imac
     * and emits no floating-point instructions at all, so there is no dirty
     * state to hand over -- and forcing FS=Off would newly *cause* a trap for
     * any FP instruction executed later, rather than preventing one. The
     * kernel is CONFIG_FPU=n and sets FS for itself in head.S. Doing nothing
     * is both the smaller deviation and the safer one. */

    /* Order the copies we just made against the fetch that is about to
     * happen. `fence` retires the stores; `fence.i` makes the instruction
     * stream see them.
     *
     * NEITHER IS SUFFICIENT ON ITS OWN HERE, and neither replaces the cache
     * writeback. On this core an instruction fetch does not observe dirty
     * L1-D lines, so the payload is pushed out of the cache in C -- see
     * p4_flash_copy(), which writes back and invalidates every range it
     * copies. These two are the architectural ordering on top of that. */
    fence
    fence.i

    li      a0, 0                   /* hartid: we boot core 0 only */
    mv      a1, t1                  /* FDT physical address */
    jr      t0

    .size   jump_to_linux, . - jump_to_linux
