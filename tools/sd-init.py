#!/usr/bin/env python3
"""Take over the microSD card from the board itself, and put the rootfs on it.

    python tools/sd-init.py COM17 [--phase table|fs|copy|all]

DESTRUCTIVE. Erases everything on the card. Written to replace a Raspberry Pi
OS install (512 MiB FAT boot at sector 16384, 115.6 GiB ext4 root at sector
1064960) with a Linux root plus a data partition.

WHY THE BOARD DOES THE WORK

There is no card reader on the host, so the card is only reachable through the
ESP32-P4. The rootfs doing the partitioning is therefore the initramfs one --
it needs fdisk/mkfs/dd itself -- and the rootfs it writes is a copy of itself.
board/busybox.fragment enables the applets for exactly this.

TWO THINGS LEARNED THE HARD WAY ON THIS BOARD

1. Small writes in bulk wedge it. `dd bs=512 count=8192` (4 MiB as 8192
   separate sector writes) stalled long enough for the MWDT to reset the
   board mid-run -- reset reason 0x07. The same 4 MiB as `bs=1M count=4`
   completes in under a second. So every bulk write here uses bs=1M, and the
   one place a single sector must be written (the MBR) builds the sector in
   tmpfs first and writes it in one go.

2. `mkfs.ext2` is fragile. Merely running it with no arguments, to print its
   usage, was enough to trigger another MWDT reset. It is used here on as
   small a partition as makes sense, with a modest inode count, to keep its
   metadata writes small -- and the phase split means a failure there does not
   cost the partitioning work.

WHY dd AND printf RATHER THAN fdisk

fdisk is interactive, and driving its prompts blind over a serial line is a
poor way to repartition a disk. The table is built here instead, byte by byte,
and reviewable before it runs. The board has no base64, so the bytes travel as
printf octal escapes -- fine, because an MBR is almost all zeros.
"""
import os
import sys
import time

import serial

BAUD = 4000000
PROMPT = b"# "

# Geometry. Card size is read off the board at run time from
# /sys/block/mmcblk0/size rather than written down here: this constant was
# 243507200 (a 116 GiB card) and the next card in the slot was 14.5 GiB, at
# which point a hardcoded total silently describes a p2 running off the end of
# the device.
P1_START = 2048                    # 1 MiB in, conventional alignment
ROOT_SECTORS = 1048576             # 512 MiB root
#
# 512 MiB, not the 2 GiB first tried, and not the whole card. The rootfs is
# 8.4 MB, so this is already ~60x headroom, and mkfs cost scales with size --
# see note 2 above. Everything above it is p2, left for data.
P2_START = P1_START + ROOT_SECTORS

# The Raspberry Pi partitions being removed, so their superblocks are zeroed
# by address rather than hoped over. In MiB, since all bulk writes use bs=1M.
OLD_P1_START_MIB = 16384 * 512 // 2**20      # 8 MiB   (FAT boot sector)
OLD_P2_START_MIB = 1064960 * 512 // 2**20    # 520 MiB (ext4 superblock)

PART_TYPE_LINUX = 0x83

# Real directories to copy. /proc, /sys, /dev, /tmp, /run and /mnt are virtual
# or mount points: they must exist and be empty on the target. Copying a
# mounted devtmpfs would drag in hundreds of dynamic nodes.
COPY_DIRS = "bin etc lib lib32 sbin usr var root opt media linuxrc data init"

# Static console nodes. The kernel mounts devtmpfs over /dev for a real root,
# so these are insurance -- but they are what getty opens if it does not.
DEV_NODES = (("console", 5, 1), ("null", 1, 3), ("zero", 1, 5),
             ("tty", 5, 0), ("ttyS0", 4, 64))


def esc(data):
    """Bytes as printf octal escapes: the only binary channel available."""
    return "".join("\\%03o" % b for b in data)


def entry(ptype, lba_start, count):
    """One 16-byte MBR partition entry.

    CHS fields are the 0xFE/0xFF/0xFF "use LBA" placeholder: Linux reads only
    the LBA fields, and real geometry does not fit in CHS on a 116 GB card.
    """
    out = (bytes([0x00, 0xFE, 0xFF, 0xFF, ptype, 0xFE, 0xFF, 0xFF])
           + lba_start.to_bytes(4, "little")
           + count.to_bytes(4, "little"))
    assert len(out) == 16
    return out


def read_until(ser, needles, timeout, echo=True):
    deadline = time.time() + timeout
    buf = b""
    while time.time() < deadline:
        chunk = ser.read(4096)
        if chunk:
            buf += chunk
            if echo:
                sys.stdout.write(chunk.decode("utf-8", "replace"))
                sys.stdout.flush()
            for n in needles:
                if n in buf:
                    return n, buf
        else:
            time.sleep(0.01)
    return None, buf


def reset(ser):
    ser.setDTR(False)
    ser.setRTS(True)
    time.sleep(0.1)
    ser.setRTS(False)
    time.sleep(0.05)
    ser.setDTR(False)


def login(ser):
    print("--- resetting and waiting for login ---", flush=True)
    if not read_until(ser, [b"login:"], 150, echo=False)[0]:
        sys.exit("no login prompt")
    ser.write(b"root\r\n")
    if not read_until(ser, [PROMPT], 30, echo=False)[0]:
        sys.exit("no shell prompt")
    print("--- logged in ---", flush=True)


def run(ser, cmd, timeout=120):
    print(f"\n$ {cmd}", flush=True)
    ser.write(cmd.encode() + b"\r\n")
    hit, buf = read_until(ser, [PROMPT], timeout)
    out = buf.decode("utf-8", "replace")
    if not hit:
        print(f"\n!!! timed out after {timeout}s", flush=True)
        if "reset reason" in out:
            print("!!! the board RESET during that command", flush=True)
    return out


def card_sectors(ser):
    """Total 512-byte sectors, from the board.

    /sys/block/mmcblk0/size is always in 512-byte units regardless of the
    card's own block size, which is what the MBR wants too.
    """
    out = run(ser, "cat /sys/block/mmcblk0/size")
    for line in out.splitlines():
        line = line.strip()
        if line.isdigit() and int(line) > 1024:
            return int(line)
    return 0


def phase_table(ser, total_sectors):
    """Erase the Pi's table and superblocks, write ours, re-read it."""
    p2_sectors = total_sectors - P2_START
    print("\n=== erasing the Raspberry Pi table and superblocks ===", flush=True)
    # Three targeted 4 MiB wipes, not the whole card: 116 GB over a 20 MHz SD
    # bus would take hours. These cover the MBR, the old FAT boot sector, and
    # the old ext4 primary superblock -- which is what makes either
    # filesystem unmountable.
    run(ser, "dd if=/dev/zero of=/dev/mmcblk0 bs=1M count=4 conv=notrunc", 300)
    run(ser, f"dd if=/dev/zero of=/dev/mmcblk0 bs=1M seek={OLD_P1_START_MIB} "
             f"count=4 conv=notrunc", 300)
    run(ser, f"dd if=/dev/zero of=/dev/mmcblk0 bs=1M seek={OLD_P2_START_MIB} "
             f"count=4 conv=notrunc", 300)

    print("\n=== writing the new partition table ===", flush=True)
    p1 = entry(PART_TYPE_LINUX, P1_START, ROOT_SECTORS)
    p2 = entry(PART_TYPE_LINUX, P2_START, p2_sectors)

    # Build the sector in tmpfs, then write it to the card once. Doing the
    # byte-level seeks directly on the block device would be 34 separate
    # read-modify-write cycles on the same sector.
    run(ser, "dd if=/dev/zero of=/tmp/mbr bs=512 count=1")
    run(ser, f"printf '{esc(p1)}' | dd of=/tmp/mbr bs=1 seek=446 conv=notrunc")
    run(ser, f"printf '{esc(p2)}' | dd of=/tmp/mbr bs=1 seek=462 conv=notrunc")
    run(ser, f"printf '{esc(bytes([0x55, 0xAA]))}' | dd of=/tmp/mbr bs=1 "
             f"seek=510 conv=notrunc")
    run(ser, "xxd /tmp/mbr | tail -5")          # eyes on it before it lands
    run(ser, "dd if=/tmp/mbr of=/dev/mmcblk0 bs=512 count=1 conv=notrunc")
    run(ser, "sync", 300)

    # partprobe rather than a reboot: the applet list has it, and a warm
    # reset on this board lands in the flash-read problem documented in
    # bootloader/p4/p4_flash.c.
    run(ser, "partprobe /dev/mmcblk0", 120)
    out = run(ser, "cat /proc/partitions")
    return "mmcblk0p2" in out


def phase_fs(ser):
    """Make the root filesystem."""
    print("\n=== making the root filesystem on p1 ===", flush=True)
    # ext2, not ext4: e2fsprogs depends on BR2_USE_MMU and this is a NOMMU
    # target, so busybox mkfs.ext2 is the only maker available. No journal.
    # -i 65536 keeps the inode table small (~8k inodes for 512 MiB), which is
    # both plenty for a 250-file rootfs and much less metadata to write.
    out = run(ser, "mkfs.ext2 -F -i 65536 -L p4root /dev/mmcblk0p1", 900)
    if "reset reason" in out:
        return False
    return "mkfs" not in out.lower() or "error" not in out.lower()


def phase_copy(ser):
    """Copy the running rootfs onto the card.

    -t ext2 is explicit, and it matters. The kernel is built with the
    standalone CONFIG_EXT2_FS driver precisely because the ext4 subsystem --
    which claims the ext2 name when CONFIG_EXT4_USE_FOR_EXT23 is set -- hangs
    this board on the first write to a mounted filesystem. Raw block writes
    are fine; a 4 KB write to a file was not. Naming the driver keeps a future
    kernel from quietly handing ext2 back to ext4.
    """
    print("\n=== copying the rootfs onto the card ===", flush=True)
    out = run(ser, "mkdir -p /mnt/sd && mount -t ext2 /dev/mmcblk0p1 /mnt/sd "
                   "&& echo MOUNTED", 300)
    if "MOUNTED" not in out:
        return False
    run(ser, f"for d in {COPY_DIRS}; do [ -e /$d ] && cp -a /$d /mnt/sd/ ; "
             f"done; echo COPIED", 900)
    run(ser, "mkdir -p /mnt/sd/dev /mnt/sd/proc /mnt/sd/sys /mnt/sd/tmp "
             "/mnt/sd/run /mnt/sd/mnt && chmod 1777 /mnt/sd/tmp && echo DIRS")
    for name, major, minor in DEV_NODES:
        run(ser, f"mknod /mnt/sd/dev/{name} c {major} {minor}")
    run(ser, "sync", 600)
    run(ser, "ls /mnt/sd")
    run(ser, "df -h /mnt/sd")
    out = run(ser, "umount /mnt/sd && echo UNMOUNTED", 300)
    run(ser, "sync", 300)
    return "UNMOUNTED" in out


def main():
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    port = sys.argv[1] if len(sys.argv) > 1 else "COM17"
    phase = "all"
    if "--phase" in sys.argv:
        phase = sys.argv[sys.argv.index("--phase") + 1]

    with serial.Serial(port, BAUD, timeout=0.1) as ser:
        reset(ser)
        login(ser)

        total = card_sectors(ser)
        if total < P2_START + 2048:
            sys.exit(f"!!! could not read a plausible card size (got {total})")
        p2_sectors = total - P2_START

        print(f"\nNew partition table on a {total * 512 / 2**30:.1f} GiB card:")
        print(f"  p1  LBA {P1_START:>9}  {ROOT_SECTORS:>9} sectors "
              f"{ROOT_SECTORS * 512 / 2**20:8.0f} MiB  rootfs (ext2)")
        print(f"  p2  LBA {P2_START:>9}  {p2_sectors:>9} sectors "
              f"{p2_sectors * 512 / 2**30:8.2f} GiB  data (unformatted)")

        if phase in ("table", "all"):
            if not phase_table(ser, total):
                sys.exit("!!! the new partition table did not take")
            print("\n*** partition table OK ***", flush=True)
        if phase in ("fs", "all"):
            if not phase_fs(ser):
                sys.exit("!!! mkfs failed")
            print("\n*** filesystem OK ***", flush=True)
        if phase in ("copy", "all"):
            if not phase_copy(ser):
                sys.exit("!!! copy failed")
            print("\n*** rootfs copied ***", flush=True)

    print("\n--- done ---")
    return 0


if __name__ == "__main__":
    sys.exit(main())
