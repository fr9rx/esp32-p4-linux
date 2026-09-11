#!/bin/sh
# Post-build fixups for the ESP32-P4 Function EV Board rootfs.
#
# Buildroot calls this with TARGET_DIR as $1 and exports BUILD_DIR,
# HOST_DIR and STAGING_DIR.
#
# NOT IDEMPOTENT-SAFE BY DESIGN, and that matters: output/target persists
# between builds, so anything here that edits a file in place will see its
# own previous output on the next run. Everything below is therefore
# expressed as "install" or "remove", never as an in-place edit -- the
# /etc/fstab and /etc/profile seds that the hypervisor tree's post-build
# does are handled by board/overlay/ here instead, which buildroot
# re-applies from scratch every time.
set -e
TARGET_DIR="$1"
test -n "$TARGET_DIR"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"

# ---------------------------------------------------------------------
# 1. Trim binutils to the two tools that are actually usable here.
#
#   as       ~1.29 MB   assembles
#   objcopy  ~0.96 MB   -O binary; mkflt.lua reads section bytes through it
#   ld       ~5.28 MB   REMOVED -- and not to save space. It does not RUN.
#                       binfmt_flat makes one physically contiguous
#                       allocation per exec and this allocator tops out at
#                       MAX_PAGE_ORDER=10 = 4 MB, so ld fails at exec every
#                       time, not intermittently:
#                         nommu: Allocation of length 5283840 ... failed
#                         binfmt_flat: Unable to allocate RAM ... errno -12
#                       /usr/lib/mkflt.lua replaces it.
#   elf2flt              REMOVED -- it converts ld's output, so it goes
#                       with ld.
#   objdump  ~1.39 MB   REMOVED. Add it back to BINUTILS_KEEP if you want
#                       on-board disassembly; it is under the 4 MB ceiling,
#                       it is just big.
BINUTILS_KEEP="as objcopy"

if [ -x "$TARGET_DIR/usr/bin/as" ]; then
    # binutils installs its whole tool set TWICE: once in /usr/bin and
    # again in /usr/<target-triple>/bin, as hardlinks. du counts them
    # once. Most image formats do not store hardlinks, so the second copy
    # is a full second copy on the card.
    for triple_dir in "$TARGET_DIR"/usr/riscv32-*linux-*; do
        if [ -d "$triple_dir" ]; then
            rm -rf "$triple_dir"
            echo "post-build: removed duplicate tool set ${triple_dir#$TARGET_DIR}" >&2
        fi
    done

    for f in "$TARGET_DIR"/usr/bin/*; do
        b=$(basename "$f")
        case " $BINUTILS_KEEP " in
            *" $b "*) continue ;;
        esac
        # Only touch things binutils owns, so this never eats an
        # unrelated package's binary.
        case "$b" in
            addr2line|ar|c++filt|elfedit|gprof|ld|ld.bfd|nm|objdump|\
            ranlib|readelf|size|strings|strip|dwp|elf2flt|flthdr|gp-*)
                if [ -f "$f" ] && [ ! -L "$f" ]; then
                    rm -f "$f"
                fi
                ;;
        esac
    done
    kept=$(cd "$TARGET_DIR/usr/bin" && ls $BINUTILS_KEEP 2>/dev/null | tr '\n' ' ')
    echo "post-build: binutils trimmed to: $kept" >&2
else
    echo "post-build: NOTE no target as(1) found -- is BR2_PACKAGE_BINUTILS_TARGET set?" >&2
fi

# ---------------------------------------------------------------------
# 2. The on-board build kit.
#
# Three files turn `as` and `objcopy` into a complete toolchain:
#
#   /usr/bin/mkflt              as -mno-relax, then mkflt.lua
#   /usr/lib/mkflt.lua          a linker: lays sections out, applies the
#                               rv32 relocations, writes the bFLT header
#   /usr/share/hello-onboard.S  an example that builds and runs on-board
#   /usr/share/stress.S         the regression test for mkflt.lua's
#                               relocation handling
#
# The third item in the chain is the one people forget: the kernel
# executes bFLT, and its other loader (binfmt_elf_fdpic) refuses a plain
# ELF on NOMMU because riscv has no FDPIC ABI. ELF in, bFLT out, or it
# does not execute.
if [ -f "$SCRIPT_DIR/mkflt.sh" ] && [ -f "$SCRIPT_DIR/mkflt.lua" ]; then
    install -D -m 0755 "$SCRIPT_DIR/mkflt.sh"  "$TARGET_DIR/usr/bin/mkflt"
    install -D -m 0644 "$SCRIPT_DIR/mkflt.lua" "$TARGET_DIR/usr/lib/mkflt.lua"
    for example in hello-onboard.S stress.S; do
        if [ -f "$SCRIPT_DIR/$example" ]; then
            install -D -m 0644 "$SCRIPT_DIR/$example" \
                "$TARGET_DIR/usr/share/$example"
        fi
    done
    echo "post-build: installed mkflt, mkflt.lua and the example sources" >&2
else
    echo "post-build: NOTE mkflt.sh/mkflt.lua missing from $SCRIPT_DIR" >&2
fi

# ---------------------------------------------------------------------
# 3. bFLT stack sizes, for every flat binary in the image.
#
# Under NOMMU the stack is part of the single contiguous exec allocation,
# so it is fixed at build time and cannot grow at runtime. elf2flt's
# default is 4 kB. There is no guard page either, so an overrun does not
# fault at the boundary -- it writes straight through into .bss and the
# program dies later, somewhere else, with a wild pointer:
#
#   nano[94]: unhandled signal 11 code 0x2 at 0x49778c3e
#   epc : 49778c3e  sp : 497c7cf0  s0 : 497c8528
#   badaddr: 005d001f  cause: 38000007     (7 = store access fault)
#
# sp to s0 there is 2104 bytes -- one frame taking half of a 4 kB stack.
#
# THIS USED TO BE A LIST OF THREE NAMES (busybox, lua, curl) and that is
# exactly why nano crashed: it was never on the list, and neither were
# less, grep, sed, xz, diff, as or objcopy. 28 of the 32 flat binaries in
# the image still had the 4 kB default. A list that has to be maintained
# by hand is a list that will be wrong again, so this sweeps the whole
# tree instead and raises anything below the floor.
#
# It only ever raises, never lowers, so a binary that asked for more at
# link time keeps it -- hget links with -s131072 for its TLS handshake.
#
# 32 kB is the floor rather than something larger because the exec
# allocation is rounded up to a power-of-two page count: busybox at
# ~613 kB and at ~645 kB land in the same 1 MB bucket, so the stack is
# free. Going to 64 kB would push nano (472 kB) from the 512 kB bucket
# into the 1 MB one and double its RAM. Anything that genuinely needs
# more should say so at link time.
FLTHDR="$HOST_DIR/bin/riscv32-linux-flthdr"
FLAT_STACK_MIN=32768
if [ -x "$FLTHDR" ]; then
    raised=0
    kept=0
    for f in $(find "$TARGET_DIR" -type f -perm -u+x 2>/dev/null); do
        head -c 4 "$f" 2>/dev/null | grep -q bFLT || continue
        cur=$("$FLTHDR" -p "$f" 2>/dev/null |
              sed -n 's/.*Stack Size: *0x\([0-9a-fA-F]*\).*/\1/p')
        [ -n "$cur" ] || continue
        if [ $((0x$cur)) -lt $FLAT_STACK_MIN ]; then
            "$FLTHDR" -s $FLAT_STACK_MIN "$f"
            raised=$((raised + 1))
        else
            kept=$((kept + 1))
        fi
    done
    echo "post-build: bFLT stacks: $raised raised to $FLAT_STACK_MIN," \
         "$kept already larger" >&2
fi

# elf2flt leaves an unstripped ELF beside every flat binary it makes, and
# those are far larger than the binaries themselves.
find "$TARGET_DIR" -name '*.gdb' -type f -delete

# ---------------------------------------------------------------------
# 4. hget -- the board's HTTP client.
#
# Compiled here rather than carried as a buildroot package because the
# source is three hundred lines in this repo and a package would mean a
# BR2_EXTERNAL tree to hold one file. It still builds every time the
# image does, with the same toolchain and against the same staging
# sysroot, which is the part that matters.
#
# WHY NOT JUST USE curl. curl is on the card and it does not work: every
# transfer fails with CURLE_OUT_OF_MEMORY (exit 27), including
# file:///etc/fstab, which touches no network at all. Three explanations
# were tested on hardware and all three were wrong -- RLIMIT_STACK from
# 8 MB down to 128 KB (uClibc sizes pthread stacks from it), bypassing
# the threaded resolver with --resolve, and raising curl's bFLT stack
# from 4 kB to 256 kB with flthdr. curl stays in the image; hget is what
# is expected to work.
#
# WHY NOT busybox wget. No TLS at all -- "not an http or ftp url" on any
# https URL. It has been removed from the busybox config, so this is now
# the only HTTP client that functions.
#
# TLS is linked in when mbedtls is in staging and left out when it is
# not, which is a real size difference and worth keeping honest:
#
#   hget, mbedTLS   ~741 kB      curl  ~1359 kB
#   hget, no TLS    ~125 kB
#
# The explicit -s131072 is not decoration. elf2flt defaults to a 4 kB
# stack, which is what curl got, and an mbedTLS handshake does not fit in
# 4 kB. On NOMMU there is no guard page to catch the overrun either -- it
# just walks into .bss.
HGET_SRC="$SCRIPT_DIR/hget/hget.c"
HGET_CC="$HOST_DIR/bin/riscv32-linux-gcc"
if [ -f "$HGET_SRC" ] && [ -x "$HGET_CC" ]; then
    if [ -f "$STAGING_DIR/usr/include/mbedtls/ssl.h" ]; then
        "$HGET_CC" -Os -fPIC -Wall -Wextra -D_GNU_SOURCE -DHGET_TLS \
            -I"$STAGING_DIR/usr/include" \
            -o "$TARGET_DIR/usr/bin/hget" "$HGET_SRC" \
            -L"$STAGING_DIR/usr/lib" -lmbedtls -lmbedx509 -lmbedcrypto \
            -Wl,-elf2flt="-r -s131072"
        echo "post-build: built hget (with mbedTLS)" >&2
    else
        "$HGET_CC" -Os -fPIC -Wall -Wextra -D_GNU_SOURCE \
            -o "$TARGET_DIR/usr/bin/hget" "$HGET_SRC" \
            -Wl,-elf2flt="-r -s65536"
        echo "post-build: built hget (no TLS -- mbedtls not in staging)" >&2
    fi
    chmod 0755 "$TARGET_DIR/usr/bin/hget"
    rm -f "$TARGET_DIR/usr/bin/hget.gdb"
else
    echo "post-build: NOTE hget source or cross gcc missing, not building it" >&2
fi

# ca-certificates ships its trust store twice: 289 individual PEMs in
# /etc/ssl/certs under their subject hashes, and the same certificates
# again concatenated into /etc/ssl/certs/ca-certificates.crt. That is
# 772 kB where 217 kB would do.
#
# The hashed files exist for OpenSSL's "CA directory" lookup, which
# nothing here uses: hget reads the bundle by path and is the only TLS
# client on the board that works. Dropping them saves ~555 kB, which is
# not cosmetic -- the provisioning kernel carries the whole rootfs as an
# initramfs and has to fit the 12 MB kernel partition.
#
# /usr/share/ca-certificates IS DELIBERATELY LEFT ALONE. An earlier
# version of this deleted it too, for another ~590 kB, and that was a
# bug: the package's install step regenerates ca-certificates.crt by
# concatenating that directory, so on the NEXT build -- with
# output/target still populated -- it regenerated an empty bundle and
# silently disabled certificate verification. This is the trap described
# at the top of this file, and the rule it implies is the fix: remove
# only what the build recreates from something else, never the source
# itself.
CA_CERTS_DIR="$TARGET_DIR/etc/ssl/certs"
CA_BUNDLE="$CA_CERTS_DIR/ca-certificates.crt"
if [ -d "$CA_CERTS_DIR" ]; then
    if [ -s "$CA_BUNDLE" ]; then
        before=$(du -sk "$CA_CERTS_DIR" 2>/dev/null | cut -f1)
        find "$CA_CERTS_DIR" -mindepth 1 ! -name 'ca-certificates.crt' \
            -delete 2>/dev/null || true
        after=$(du -sk "$CA_CERTS_DIR" 2>/dev/null | cut -f1)
        echo "post-build: CA store trimmed to the bundle (${before}K -> ${after}K)" >&2
    else
        echo "post-build: WARNING ca-certificates.crt is empty or missing --" \
             "TLS verification would fail; not trimming" >&2
    fi
fi

# busybox no longer provides wget, but output/target persists between
# builds, so last build's symlink is still sitting there pointing at a
# busybox that will answer "applet not found".
for stale in usr/bin/wget bin/wget; do
    if [ -L "$TARGET_DIR/$stale" ]; then
        rm -f "$TARGET_DIR/$stale"
        echo "post-build: removed stale $stale symlink" >&2
    fi
done

# ---------------------------------------------------------------------
# 5. The exec-ceiling guard. Must be last: it inspects the finished tree.
#
# NOTE the LIMIT inside checkflat.sh is 4 MB here, not the 8 MB the
# hypervisor tree uses -- that tree patches CONFIG_ARCH_FORCE_MAX_ORDER
# to 11 and this one does not. If you ever add such a patch, raise both
# together or the check silently stops meaning anything.
sh "$SCRIPT_DIR/checkflat.sh" "$TARGET_DIR" "$FLTHDR"
