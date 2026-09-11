#!/usr/bin/env python3
"""Host-side tool for everything that needs to know the flash layout.

partitions.csv is the single source of truth. This script turns it into the
binary table the bootloader walks, and computes the CRC32s the bootloader
prints at step 7 so they can be compared without eyeballing.

  flashmap.py binary partitions.csv partitions.bin
  flashmap.py crc    partitions.csv kernel ../images/Image
  flashmap.py region partitions.csv kernel     -> "0x00020000 0x00600000"

The `region` form exists so the Makefile's flash targets can take offsets
from partitions.csv instead of repeating them. Its output is deliberately in
the argument order `espflash erase-region` wants.

WHY NOT `espflash partition-table`
----------------------------------
espflash uses esp-idf-part, which refuses any table without a partition of
type 'app':

    Error: No partition of type 'app' was found in the partition table

We have no app partition and never will -- nothing downstream of us loads an
ESP-IDF application, we boot a Linux Image. Satisfying that check would mean
putting a fictional entry in the table, so we generate the table instead.
(esp-idf-part also panics outright on custom *data subtypes*, which was the
first layout attempted: `called Option::unwrap() on a None value` at
esp-idf-part-0.6.0/src/partition/mod.rs:157. Custom *types* -- 0x40 and 0x41,
which is what ESP-IDF reserves 0x40..0xFE for -- are the correct encoding
anyway.)

The output format is ESP-IDF's own, unchanged: 32-byte entries plus the
trailing MD5 entry, exactly what gen_esp32part.py emits. Only the validation
rules differ.
"""

import csv
import hashlib
import struct
import sys
import zlib

# One entry: magic, type, subtype, offset, size, 16-byte label, flags.
ENTRY_FMT = "<2sBBLL16sL"
ENTRY_SIZE = 32
assert struct.calcsize(ENTRY_FMT) == ENTRY_SIZE

MAGIC = b"\xAA\x50"      # ESP_PARTITION_MAGIC, little-endian 0x50AA
MAGIC_MD5 = b"\xEB\xEB"  # ESP_PARTITION_MAGIC_MD5

# The named types ESP-IDF defines. Anything else must be numeric, which is
# what our two partitions use.
TYPES = {"app": 0x00, "data": 0x01}


def parse(path):
    """partitions.csv -> [(name, type, subtype, offset, size)]."""
    out = []
    with open(path, newline="") as f:
        for lineno, row in enumerate(csv.reader(f), 1):
            row = [c.strip() for c in row]
            # Skip blank lines, comments, and a trailing empty field from a
            # line that ends in a comma.
            while row and row[-1] == "":
                row.pop()
            if not row or row[0].startswith("#"):
                continue
            if len(row) != 5:
                sys.exit(f"{path}:{lineno}: expected 5 fields, got {len(row)}: {row}")
            name, ptype, psub, off, size = row
            out.append((
                name,
                TYPES[ptype] if ptype in TYPES else int(ptype, 0),
                int(psub, 0),
                int(off, 0),
                int(size, 0),
            ))
    return out


def check(parts):
    """Only the invariants that would actually break the bootloader."""
    # The table lives at 0x8000 and is 4 KB; the first partition must clear it.
    for name, _t, _s, off, size in parts:
        if off < 0x9000:
            sys.exit(f"{name}: offset 0x{off:x} overlaps the bootloader or the table")
        # esp_rom_spiflash_read() requires 4-byte alignment, and we round
        # partition offsets to a 4 KB sector so a partition can be erased
        # without touching its neighbours.
        if off % 0x1000:
            sys.exit(f"{name}: offset 0x{off:x} is not 4 KB aligned")
        if size % 0x1000:
            sys.exit(f"{name}: size 0x{size:x} is not a whole number of 4 KB sectors")
    ordered = sorted(parts, key=lambda p: p[3])
    for a, b in zip(ordered, ordered[1:]):
        if a[3] + a[4] > b[3]:
            sys.exit(f"{a[0]} (0x{a[3]:x}+0x{a[4]:x}) overlaps {b[0]} (0x{b[3]:x})")


def cmd_binary(csv_path, out_path):
    parts = parse(csv_path)
    check(parts)
    blob = b""
    for name, ptype, psub, off, size in parts:
        label = name.encode()
        if len(label) > 16:
            sys.exit(f"{name}: label longer than 16 bytes")
        blob += struct.pack(ENTRY_FMT, MAGIC, ptype, psub, off, size,
                            label.ljust(16, b"\x00"), 0)
    # The MD5 entry covers only the real entries before it, which is why it is
    # appended rather than reserved. gen_esp32part.py does the same.
    blob += MAGIC_MD5 + b"\xFF" * 14 + hashlib.md5(blob).digest()
    with open(out_path, "wb") as f:
        f.write(blob)
    print(f"{out_path}: {len(blob)} bytes, {len(parts)} partitions + md5")
    for name, ptype, psub, off, size in parts:
        print(f"  {name:<10} type 0x{ptype:02x} sub 0x{psub:02x} "
              f"0x{off:08x} + 0x{size:08x} ({size // 1024} KB)")


def cmd_crc(csv_path, name, payload):
    """The CRC32 the bootloader should print for this partition.

    Over the WHOLE partition, not just the payload, because that is what the
    bootloader can compute: it walks the table and knows the partition size,
    but nothing on flash records how many of those bytes are payload. So the
    tail is included, and it reads 0xFF because `make flash-kernel` erases the
    region before writing it. Reflash without erasing and this number stops
    matching -- which is the intended loud failure, not a bug.
    """
    parts = {p[0]: p for p in parse(csv_path)}
    if name not in parts:
        sys.exit(f"no partition named {name} in {csv_path}; have "
                 f"{', '.join(parts)}")
    _n, _t, _s, off, size = parts[name]
    with open(payload, "rb") as f:
        data = f.read()
    if len(data) > size:
        sys.exit(f"{payload} is {len(data)} bytes, larger than the "
                 f"{size}-byte {name} partition")
    crc = zlib.crc32(data)
    # Chained rather than materialised: the padding can be megabytes.
    pad = size - len(data)
    chunk = b"\xFF" * min(pad, 1 << 16)
    while pad:
        n = min(pad, len(chunk))
        crc = zlib.crc32(chunk[:n], crc)
        pad -= n
    print(f"{name}: 0x{off:08x} + 0x{size:08x}, payload {len(data)} bytes "
          f"({100 * len(data) // size}%), 0x{size - len(data):x} bytes of 0xFF pad")
    print(f"  expected CRC32 0x{crc:08x}")


def cmd_region(csv_path, name):
    """Offset and size of one partition, for the Makefile to pass to espflash."""
    parts = {p[0]: p for p in parse(csv_path)}
    if name not in parts:
        sys.exit(f"no partition named {name} in {csv_path}; have "
                 f"{', '.join(parts)}")
    _n, _t, _s, off, size = parts[name]
    print(f"0x{off:08x} 0x{size:08x}")


if __name__ == "__main__":
    if len(sys.argv) >= 2 and sys.argv[1] == "binary" and len(sys.argv) == 4:
        cmd_binary(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 2 and sys.argv[1] == "crc" and len(sys.argv) == 5:
        cmd_crc(sys.argv[2], sys.argv[3], sys.argv[4])
    elif len(sys.argv) >= 2 and sys.argv[1] == "region" and len(sys.argv) == 4:
        cmd_region(sys.argv[2], sys.argv[3])
    else:
        sys.exit(__doc__)
