# ESP32-P4 second-stage bootloader.
#
# Plain make. No ESP-IDF project, no CMake, no Kconfig, no Python environment.
# Two external tools: a bare-metal riscv32 gcc, and espflash (a single
# standalone binary). Sources we reuse from ESP-IDF are copied into vendor/
# and pinned -- see vendor/MANIFEST.md.
#
#   make            build bootloader.elf and bootloader.bin
#   make flash      write it to flash 0x2000
#   make monitor    open the console
#   make info       identify the connected chip
#   make size       section sizes and the SRAM budget check
#   make dis        disassemble to bootloader.lst
#   make restore    put the original ESP-IDF bootloader back
#   make clean
#
# Payloads, for step 7 onward:
#   make partitions.bin   generate the flash layout from partitions.csv
#   make flash-parts      write the partition table to 0x8000
#   make flash-kernel     erase + write ../images/Image
#   make flash-dtb        erase + write ../images/esp32p4-function-ev.dtb
#   make flash-all        the bootloader, the table and both payloads
#   make crc              the CRC32s the board should print at step 7

TARGET   := bootloader

# --- toolchain -------------------------------------------------------------
# Any bare-metal-capable riscv32 gcc works; we only ever build with
# -ffreestanding -nostdlib.
#
# Default is Espressif's riscv32-esp-elf. Build from Git Bash or cmd, NOT from
# WSL: gcc.exe cannot locate cc1 across the interop boundary and fails with
# "cannot execute 'cc1'". If you want to build inside WSL, point CROSS at a
# Linux-hosted toolchain instead -- buildroot's, from the kernel side of this
# project, has defaults of exactly rv32imac_zicsr_zifencei / ilp32:
#   make CROSS=$$HOME/p4ev-linux/output/host/bin/riscv32-linux-
CROSS   ?= /c/Espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin/riscv32-esp-elf-
CC      := $(CROSS)gcc
OBJCOPY := $(CROSS)objcopy
OBJDUMP := $(CROSS)objdump
SIZE    := $(CROSS)size
NM      := $(CROSS)nm

# espflash: a single standalone binary, no Python environment behind it. It
# also speaks ELF directly, so `save-image` replaces esptool's elf2image and
# `monitor` replaces miniterm.
ESPFLASH ?= espflash
PORT     ?= COM17
BAUD     ?= 921600
# The console runs at 4 Mbps from step 4 onward, which is what the DTS and the
# kernel command line both assume. The ROM banner and our first few lines come
# out at 115200 before the switch -- use `make monitor MONBAUD=115200` to read
# those.
MONBAUD  ?= 4000000

FLASH_MODE ?= dio
FLASH_FREQ ?= 40mhz
FLASH_SIZE ?= 16mb

# --- architecture ----------------------------------------------------------
# rv32imac, NOT imafc, even though the core has an FPU.
#
# Emitting no floating-point instructions at all means mstatus.FS stays Off and
# we cannot hand Linux dirty FP state. The kernel is CONFIG_FPU=n (riscv's FPU
# support requires D, and this core has F only), so there is nothing to gain
# from -march=rv32imafc here and one whole class of handoff bug to avoid.
ARCH := -march=rv32imac_zicsr_zifencei -mabi=ilp32

# Two values IDF injects from its BUILD SYSTEM rather than a header, in
# components/soc/CMakeLists.txt. They are not in any sdkconfig, so dropping
# CMake means supplying them here or getting silent fallbacks. IDF's own
# comment on the first one is "Replace this value in an adaptive way, if
# Kconfig isn't available on your platform" -- which is us.
#
#   SOC_MMU_PAGE_SIZE  64 KB on the P4 (soc/esp32p4/Kconfig.mmu offers no
#                      other option). ext_mem_defs.h has a 0x10000 fallback
#                      that happens to match, but relying on a fallback for
#                      the granularity of the PSRAM mapping is not a habit
#                      worth having.
#   SOC_XTAL_FREQ_MHZ  40, confirmed by the chip itself via espflash
#                      board-info and by the ROM boot banner.
SOC_DEFINES := -DSOC_MMU_PAGE_SIZE=0x10000 -DSOC_XTAL_FREQ_MHZ=40

# -MMD -MP generates a .d file beside every .o listing the headers that object
# depends on; the -include at the bottom feeds them back to make.
#
# Not optional. Without it, editing sdkconfig.h rebuilds nothing, and a
# configuration change appears to take effect in whichever files happened to
# recompile for other reasons -- which produced a build that reported 200 MHz
# PSRAM while parts of it had been compiled for 80. Stale-object bugs look
# exactly like hardware bugs.
CFLAGS := $(ARCH) -Os -g3 -MMD -MP \
          -ffreestanding -nostdlib -fno-builtin \
          -ffunction-sections -fdata-sections \
          -fno-common -fno-stack-protector \
          -Wall -Wextra -Werror=implicit-function-declaration \
          -DNON_OS_BUILD=1 -DBOOTLOADER_BUILD=1 \
          $(SOC_DEFINES) \
          -I. -Ip4 -Ivendor/include

ASFLAGS := $(ARCH) -g3 -I. -Ip4 -Ivendor/include

LDFLAGS := $(ARCH) -nostdlib -nostartfiles \
           -T esp32p4.ld \
           -Wl,--gc-sections \
           -Wl,-Map=$(TARGET).map \
           -Wl,--build-id=none

# libgcc, and only libgcc: rv32 has no 64-bit divide instruction, so any
# `uint64_t / x` becomes a call to __udivdi3 which -nostdlib excludes. It is
# the compiler's own runtime, not a C library -- no libc, no startup files, no
# allocator comes with it. Must come after the objects on the link line.
#
# (Amusing echo: __udivdi3 is the same missing symbol that keeps Rust from
# building for riscv32 in the Linux kernel -- see docs/BUILD.md.)
LDLIBS := -lgcc

# --- sources ---------------------------------------------------------------
SRCS_S := start.S \
          trap.S \
          jump.S
SRCS_C := main.c \
          p4/p4_wdt.c \
          p4/p4_analog.c \
          p4/p4_uart.c \
          p4/p4_clk.c \
          p4/p4_panic.c \
          p4/p4_string.c \
          p4/p4_flashcache.c \
          p4/p4_ldo.c \
          p4/p4_cache.c \
          p4/p4_psram.c \
          p4/p4_flash.c \
          p4/p4_sdmmc.c \
          p4/p4_emac.c

# Whatever vendor/sync.py pulled in. Wildcarded so re-pinning to a different
# IDF does not need a Makefile edit.
#
# find, not a stack of wildcards. vendor/src mirrors IDF's component layout
# (necessary: hal/efuse_hal.c and hal/esp32p4/efuse_hal.c share a basename),
# and that nests arbitrarily deep -- mspi_timing_by_dqs.c sits five levels
# down. A fixed set of wildcards silently misses anything deeper, and the
# symptom is an undefined reference to a function you can see in the tree.
SRCS_C += $(shell find vendor/src -name '*.c' 2>/dev/null)

OBJS := $(SRCS_S:.S=.o) $(SRCS_C:.c=.o)
DEPS := $(OBJS:.o=.d)

# --- rules -----------------------------------------------------------------
.PHONY: all clean flash monitor size dis info restore \
        flash-parts flash-kernel flash-dtb flash-all crc

all: $(TARGET).bin size

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.S
	$(CC) $(ASFLAGS) -c $< -o $@

$(TARGET).elf: $(OBJS) esp32p4.ld vendor/ld/esp32p4.rom.eco0_4.ld
	$(CC) $(LDFLAGS) $(OBJS) $(LDLIBS) -o $@

# Turn the ELF into the ESP image format the ROM first-stage expects: a header
# plus one segment per loadable section, each carrying its load address.
#
# Two flags matter here:
#
#   --ignore-app-descriptor  espflash normally insists on an ESP-IDF app
#                            descriptor and refuses without one. That struct
#                            describes an application; we are the bootloader
#                            that would otherwise be reading it.
#   (no --merge)             --merge would prepend a bootloader and partition
#                            table around the image. We ARE the bootloader.
$(TARGET).bin: $(TARGET).elf
	$(ESPFLASH) save-image --chip esp32p4 --ignore-app-descriptor \
	    -m $(FLASH_MODE) -f $(FLASH_FREQ) -s $(FLASH_SIZE) \
	    $< $@

# The SRAM budget check. Our region stops at 0x4ff29000 because the ROM keeps
# its shared buffers, stacks and .bss above that on rev <= ECO4 -- and we are
# still calling ROM code, so overrunning it is not survivable. Cheap to assert,
# expensive to discover on hardware.
size: $(TARGET).elf
	@$(SIZE) $<
	@echo "  image top (_bss_end): 0x`$(NM) $< | grep ' _bss_end$$'   | cut -d' ' -f1`"
	@echo "  stack top:            0x`$(NM) $< | grep ' _stack_top$$' | cut -d' ' -f1`"
	@echo "  ROM data begins:      0x4ff296b8  <- must stay above the stack top"

dis: $(TARGET).elf
	$(OBJDUMP) -d $< > $(TARGET).lst
	@echo "wrote $(TARGET).lst"

# write-bin, not flash: `espflash flash` expects an application and would lay
# down a bootloader and partition table around it. We go to 0x2000 raw,
# because we are what the ROM loads.
flash: $(TARGET).bin
	$(ESPFLASH) write-bin -p $(PORT) -B $(BAUD) 0x2000 $<

# --- payloads --------------------------------------------------------------
# The kernel and DTB built by the Linux half of this project.
IMAGES ?= ../images
# Image.initramfs, not Image: the root filesystem is built into the kernel
# (config/kernel.config says why -- the SD card in the slot is a Raspberry Pi
# system disk, so it is not ours to reformat). Image is the older SD-root
# build, kept for when a spare card turns up:
#   make flash-kernel KERNEL=../images/Image
KERNEL ?= $(IMAGES)/Image.initramfs
DTB    := $(IMAGES)/esp32p4-function-ev.dtb

# partitions.csv is the only place the layout is written down. The recipes
# below ask flashmap.py for each partition's offset and size rather than
# repeating them here, so the CSV cannot drift out of step with the flashing
# commands.
#
# Note $$(...) -- shell command substitution at recipe-run time, not make's
# $(shell ...), which would run at parse time on every single invocation.
# `make` therefore still needs nothing but gcc; Python is required only by the
# payload targets.
PY ?= python

partitions.bin: partitions.csv flashmap.py
	$(PY) flashmap.py binary $< $@

# EVERY PAYLOAD TARGET ERASES ITS WHOLE PARTITION FIRST, and that is load
# bearing rather than tidiness:
#
#   - the table: write-bin lays down 96 bytes, so a previous table's later
#     entries would survive underneath and the bootloader would walk into
#     them. Its terminator check is "erased flash reads 0xFFFF".
#   - the payloads: the bootloader CRCs the whole partition, because nothing
#     on flash records how long the payload is. An un-erased tail changes
#     that CRC. See the step 7 comment in main.c.
# 0x8000 is not read from the CSV: the CSV cannot say where the table lives,
# because the table's own location is what the ROM first-stage has hardcoded.
# p4/p4_flash.c carries the same constant for the same reason.
flash-parts: partitions.bin
	$(ESPFLASH) erase-region -p $(PORT) 0x8000 0x1000
	$(ESPFLASH) write-bin -p $(PORT) -B $(BAUD) 0x8000 partitions.bin

flash-kernel: $(KERNEL) partitions.csv
	@set -- $$($(PY) flashmap.py region partitions.csv kernel); \
	 echo "erase $$1 + $$2 then write $(KERNEL)"; \
	 $(ESPFLASH) erase-region -p $(PORT) $$1 $$2 && \
	 $(ESPFLASH) write-bin -p $(PORT) -B $(BAUD) $$1 $(KERNEL)

flash-dtb: $(DTB) partitions.csv
	@set -- $$($(PY) flashmap.py region partitions.csv dtb); \
	 echo "erase $$1 + $$2 then write $(DTB)"; \
	 $(ESPFLASH) erase-region -p $(PORT) $$1 $$2 && \
	 $(ESPFLASH) write-bin -p $(PORT) -B $(BAUD) $$1 $(DTB)

flash-all: flash flash-parts flash-kernel flash-dtb

# What the board should print at step 7. Computed from partitions.csv and the
# same artifacts, over the whole partition including the 0xFF tail -- so these
# only match if the partitions were erased before being written.
crc: partitions.csv
	@$(PY) flashmap.py crc partitions.csv kernel $(KERNEL)
	@$(PY) flashmap.py crc partitions.csv dtb    $(DTB)

monitor:
	$(ESPFLASH) monitor -p $(PORT) -B $(MONBAUD) --non-interactive

info:
	$(ESPFLASH) board-info -p $(PORT)

# Restore the ESP-IDF bootloader that was on the board before we took 0x2000.
restore:
	$(ESPFLASH) write-bin -p $(PORT) -B $(BAUD) 0x0 backup/flash-0x0-0x10000.orig.bin

clean:
	rm -f $(OBJS) $(DEPS) $(TARGET).elf $(TARGET).bin $(TARGET).map $(TARGET).lst \
	      partitions.bin

# Header dependencies, generated by -MMD. Must be last, and silent on the
# first build when none exist yet.
-include $(DEPS)
