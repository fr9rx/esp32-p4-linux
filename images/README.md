# images/

Build output lands here. None of it is tracked by git — it is roughly 34 MB
of binaries, all reproducible from the sources in this repository.

| file | what | built by |
|---|---|---|
| `Image` | the kernel that boots with root on the SD card. **This is the normal one.** | buildroot (`make linux-reconfigure`) |
| `Image.initramfs` | the same kernel with the whole rootfs embedded as an initramfs, for provisioning a card with no network | buildroot with `BR2_TARGET_ROOTFS_INITRAMFS` |
| `Image.sram` | built with `CONFIG_ESP32P4_SRAM_TEXT` (patch 0037) | buildroot, that config |
| `esp32p4-function-ev.dtb` | the device tree | `tools/mkdtb.sh dts/esp32p4-function-ev.dts images/esp32p4-function-ev.dtb` |
| `rootfs.tar` | the root filesystem as a tarball | buildroot (`make rootfs-tar`) |
| `rootfs.ext2` | the root filesystem as a **journalled ext4 image** — despite the name, which is buildroot's | buildroot (`make rootfs-ext2`) |

## Two traps

**`bootloader/Makefile` defaults to `KERNEL ?= $(IMAGES)/Image.initramfs`.**
A bare `make flash-kernel` therefore flashes the *provisioning* kernel, not
the one you probably want. Pass it explicitly:

```sh
make flash-kernel KERNEL=../images/Image
```

**`rootfs.ext2` is ext4.** The name comes from
`BR2_TARGET_ROOTFS_EXT2` with `BR2_TARGET_ROOTFS_EXT2_4=y`; the filesystem
inside has `has_journal` and mounts as ext4. The journal is not decorative —
it is the difference between surviving a hard reset and losing the card, and
this board has no `e2fsck` to repair one (`e2fsprogs` is MMU-gated in
buildroot).

## Putting a filesystem on the card without a card reader

The board cannot create a journalled filesystem itself — busybox `mkfs.ext2`
makes no journal, and `mkfs.ext4` cannot be built for this target. Build the
image on the host instead and stream it over the network:

```sh
# host: compress with a small dictionary, or the board's xzcat cannot
# decompress it -- the default needs a 64 MB dictionary and NOMMU refuses
xz --check=crc32 --lzma2=preset=6,dict=64KiB -c images/rootfs.ext2 > rootfs.ext4.xz
python3 -m http.server 8099

# board: straight onto the partition, nothing buffered
hget -q http://<host>:8099/rootfs.ext4.xz | xzcat > /dev/mmcblk0p1
sync
```

256 MB of filesystem arrives as about 5.6 MB on the wire and takes roughly
70 seconds end to end. `tools/sd-init.py` is the older path, for when there
is no network.
