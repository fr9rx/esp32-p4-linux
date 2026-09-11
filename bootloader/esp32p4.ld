/* Link map for the ESP32-P4 second-stage bootloader.
 *
 * We are loaded by the ROM first-stage from flash 0x2000 into SRAM, per the
 * segment headers esptool writes into the image. Everything lives in internal
 * SRAM; nothing of ours is ever in PSRAM, which is what makes the handoff to
 * Linux safe (we cannot be overwritten by what we load).
 *
 * WHY THE REGION ENDS AT 0x4FF29000 AND NOT 0x4FFC0000
 *
 * The P4 has 768 KB of SRAM at 0x4FF00000-0x4FFC0000, but we cannot have all
 * of it: the ROM is still live -- we call ets_printf, esp_rom_spiflash_read
 * and the cache routines out of it -- and on rev <= ECO4 silicon the ROM keeps
 * its own state in the upper part of that window. From IDF's
 * bootloader.memory.ld.in, for r <= ECO4:
 *
 *   0x4ff296b8 - 0x4ff3afc0   ROM shared buffers (download modes)
 *   0x4ff3afc0 - 0x4ff3fba4   ROM startup stacks
 *   0x4ff3fba4 - 0x4ff40000   ROM .bss and .data  (not reclaimable at all)
 *
 * So 0x4FF296B8 is the first address we must not touch. We stop just below it.
 * Note those addresses are revision-dependent -- the same file lists
 * 0x4ffa96b8 for r > ECO4 -- so this number is pinned to rev 1.0 along with
 * everything else in vendor/.
 *
 * 164 KB is ample: the bootloader is a few tens of KB, and it needs no large
 * staging buffer because esp_rom_spiflash_read() reads flash straight into
 * mapped PSRAM.
 */

ENTRY(_start)

MEMORY
{
    sram (RWX) : ORIGIN = 0x4ff00000, LENGTH = 0x29000   /* ends 0x4ff29000 */
}

SECTIONS
{
    .text :
    {
        KEEP(*(.text.start))        /* _start must be first: it is the entry */
        *(.text .text.*)
        . = ALIGN(4);
    } > sram

    .rodata :
    {
        *(.rodata .rodata.*)
        *(.srodata .srodata.*)
        . = ALIGN(4);
    } > sram

    .data :
    {
        *(.data .data.*)
        . = ALIGN(8);
        /* gp is placed 0x800 into the small-data area so that the full
         * +/-2 KB gp-relative range covers .sdata and .sbss. start.S loads
         * it with relaxation disabled -- see the comment there. */
        PROVIDE(__global_pointer$ = . + 0x800);
        *(.sdata .sdata.*)
        . = ALIGN(4);
    } > sram

    .bss (NOLOAD) :
    {
        . = ALIGN(4);
        _bss_start = .;
        *(.sbss .sbss.* .scommon)
        *(.bss .bss.*)
        *(COMMON)
        . = ALIGN(4);
        _bss_end = .;
    } > sram

    /* Stack grows down from the top of our region. Nothing is allocated
     * above _bss_end, so the whole remainder is stack. */
    _stack_top = ORIGIN(sram) + LENGTH(sram);

    /DISCARD/ :
    {
        *(.eh_frame)
        *(.eh_frame_hdr)
        *(.comment)
    }
}

/* ROM function addresses. These are two text files copied verbatim from
 * ESP-IDF v6.1-beta1 -- no code, just symbol = address assignments. eco0_4 is
 * the variant IDF selects for rev < 3 (CONFIG_ESP32P4_SELECTS_REV_LESS_V3).
 *
 * Verified: for every symbol we use (ets_printf, uart_tx_one_char,
 * ets_delay_us, esp_rom_spiflash_read, Cache_WriteBack_All) the two variants
 * carry identical addresses -- but that is luck, not contract, so we pin the
 * one that matches our silicon. */
INCLUDE vendor/ld/esp32p4.rom.eco0_4.ld
INCLUDE vendor/ld/esp32p4.rom.api.ld

/* Peripheral base addresses. IDF's *_struct.h headers declare the register
 * blocks as `extern` objects -- `extern lpperi_dev_t LPPERI;` and so on -- and
 * this script is where each one's address comes from. Without it, using any
 * of those structs fails at LINK time with an undefined reference to LPPERI
 * or LP_ANA_PERI rather than at compile time, which is a confusing place to
 * meet the problem for the first time. */
INCLUDE vendor/ld/esp32p4.peripherals.ld
