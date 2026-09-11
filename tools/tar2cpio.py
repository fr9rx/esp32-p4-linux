#!/usr/bin/env python3
"""Convert buildroot's rootfs.tar into a newc cpio for CONFIG_INITRAMFS_SOURCE.

Why a converter, rather than `find . | cpio` over output/target:

  buildroot's output/target is NOT a faithful root filesystem -- it says so
  itself, in a file literally called THIS_IS_NOT_YOUR_ROOT_FILESYSTEM.
  Ownership is whatever the build user happened to be, and permissions like
  busybox's setuid bit are not applied. rootfs.tar is the finished image, made
  under fakeroot, so it already carries the right uid/gid, modes and symlinks.

  Reading that tar in Python means none of this needs root, which matters
  because it runs inside WSL as an ordinary user.

Two things are injected on the way through, both because an initramfs is not
the same as a real root filesystem:

  /init          what the kernel execs out of an initramfs. Without it the
                 kernel falls through to prepare_namespace() and tries to
                 mount root= from a real disk instead.
  /dev/*         buildroot's tar carries NO device nodes -- the target was
                 configured for dynamic /dev, so they were always going to
                 come from devtmpfs. That works for a real root filesystem and
                 not for an initramfs: CONFIG_DEVTMPFS_MOUNT explicitly does
                 not mount devtmpfs over an initramfs /dev. Without
                 /dev/console the first line of inittab
                     console::respawn:/sbin/getty -L console 0 vt100
                 has nothing to open, and the boot ends at a shell nobody can
                 reach.
"""
import os
import sys
import tarfile

MAGIC = b"070701"
S_IFDIR, S_IFREG, S_IFLNK = 0o40000, 0o100000, 0o120000
S_IFCHR, S_IFBLK, S_IFIFO = 0o20000, 0o60000, 0o10000

# The minimum set for a usable console, plus the entropy and null sinks that
# ordinary userspace assumes exist.
DEV_NODES = (
    ("dev/console", 0o600, 5, 1),
    ("dev/null",    0o666, 1, 3),
    ("dev/zero",    0o666, 1, 5),
    ("dev/full",    0o666, 1, 7),
    ("dev/random",  0o666, 1, 8),
    ("dev/urandom", 0o666, 1, 9),
    ("dev/tty",     0o666, 5, 0),
    ("dev/ttyS0",   0o620, 4, 64),
)


def entry(out, name, mode, uid, gid, nlink, mtime, data=b"", rmaj=0, rmin=0):
    """One newc record: a 110-byte ASCII header, then the NUL-terminated name,
    then the data -- header+name and data each padded to a 4-byte boundary."""
    name_b = name.encode() + b"\0"
    hdr = MAGIC + b"".join(b"%08X" % v for v in (
        0,              # ino: 0, i.e. no hardlink detection. See islnk below.
        mode, uid, gid, nlink, mtime, len(data),
        0, 0,           # devmajor/devminor OF THE FILE (not the rdev)
        rmaj, rmin,     # rdev, which is what matters for a device node
        len(name_b),
        0,              # check: unused by the newc format
    ))
    out.write(hdr)
    out.write(name_b)
    out.write(b"\0" * (-len(hdr + name_b) % 4))
    out.write(data)
    out.write(b"\0" * (-len(data) % 4))


def main(tar_path, cpio_path):
    seen = set()
    n_dev = n_lnk = n_reg = n_dir = 0

    with tarfile.open(tar_path) as tf, open(cpio_path, "wb") as out:
        for m in tf:
            name = m.name.lstrip("./")
            if not name or name in seen:
                continue
            seen.add(name)
            mode, uid, gid, mtime = m.mode, m.uid, m.gid, int(m.mtime)

            if m.isdir():
                entry(out, name, S_IFDIR | mode, uid, gid, 2, mtime)
                n_dir += 1
            elif m.issym():
                entry(out, name, S_IFLNK | 0o777, uid, gid, 1, mtime,
                      m.linkname.encode())
                n_lnk += 1
            elif m.isfile():
                data = tf.extractfile(m).read()
                # Mount devtmpfs so /dev gets the full dynamic set on top of
                # the static nodes below. The static ones are enough to reach a
                # console; this is what makes /dev/mmcblk0 and friends appear,
                # which is how the SD card becomes reachable from the shell.
                if name == "etc/fstab" and b"devtmpfs" not in data:
                    data += b"devtmpfs\t/dev\t\tdevtmpfs\tdefaults\t0\t0\n"
                    print("  added devtmpfs to /etc/fstab")
                entry(out, name, S_IFREG | mode, uid, gid, 1, mtime, data)
                n_reg += 1
            elif m.ischr():
                entry(out, name, S_IFCHR | mode, uid, gid, 1, mtime,
                      rmaj=m.devmajor, rmin=m.devminor)
                n_dev += 1
            elif m.isblk():
                entry(out, name, S_IFBLK | mode, uid, gid, 1, mtime,
                      rmaj=m.devmajor, rmin=m.devminor)
                n_dev += 1
            elif m.isfifo():
                entry(out, name, S_IFIFO | mode, uid, gid, 1, mtime)
                n_dev += 1
            elif m.islnk():
                # Hard link. Emitted as a second full copy rather than by
                # sharing an inode number: buildroot's tar has essentially
                # none, and a duplicated file is merely wasteful where a
                # mismatched ino would be wrong.
                src = tf.extractfile(m.linkname)
                entry(out, name, S_IFREG | mode, uid, gid, 1, mtime,
                      src.read() if src else b"")
                n_reg += 1

        if "init" not in seen:
            entry(out, "init", S_IFLNK | 0o777, 0, 0, 1, 0, b"sbin/init")
            n_lnk += 1
            print("  injected /init -> sbin/init")

        for dname, dmode, dmaj, dmin in DEV_NODES:
            if dname not in seen:
                entry(out, dname, S_IFCHR | dmode, 0, 0, 1, 0,
                      rmaj=dmaj, rmin=dmin)
                n_dev += 1
        print("  injected static /dev nodes: " +
              ", ".join(d[0].split("/")[1] for d in DEV_NODES))

        # Every cpio ends with this sentinel name and no data.
        entry(out, "TRAILER!!!", 0, 0, 0, 1, 0)

    print(f"  {n_dir} dirs, {n_reg} files, {n_lnk} symlinks, "
          f"{n_dev} device/fifo nodes")
    print(f"  wrote {cpio_path}: {os.path.getsize(cpio_path)} bytes")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
