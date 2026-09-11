#!/bin/sh
# Publish the device tree blob alongside the kernel image.
#
# Buildroot calls this with BINARIES_DIR as $1 and exports BASE_DIR.
#
# WHY THIS EXISTS
#
# BR2_LINUX_KERNEL_DTS_SUPPORT is off in this config (as it is in
# why2025's), so buildroot never asks the kernel for a DTB and never
# copies one into images/. The riscv build produces it anyway --
# arch/riscv/boot/dts/Makefile has `subdir-$(CONFIG_ARCH_ESPRESSIF) +=
# espressif`, and our patch 0012 adds the espressif Makefile entry -- so
# the blob is sitting in the kernel build tree; it just never gets
# published. This copies it out.
#
# The alternative would be turning DTS_SUPPORT on and naming the DTS via
# BR2_LINUX_KERNEL_INTREE_DTS_NAME, which is arguably tidier. It also
# changes which targets buildroot asks the kernel to build, and this
# arrangement is the one why2025 has actually booted. Not worth the churn.
#
# NOTE this FAILS rather than warns when the blob is missing. why2025's
# version printed a warning and carried on, which is reasonable when the
# panel is the primary console and a serial cable is the fallback. Here
# there is no display: a missing DTB means the board takes the kernel and
# stops, with nothing on the wire to say why. Better to fail the build.
set -e
BINARIES_DIR="$1"
test -n "$BINARIES_DIR"
: "${BASE_DIR:?post-image: BASE_DIR not set by buildroot}"

DTB_NAME="esp32p4-function-ev.dtb"

dtb=$(find "$BASE_DIR/build" -maxdepth 8 \
      -path "*/arch/riscv/boot/dts/espressif/$DTB_NAME" \
      -print -quit 2>/dev/null || true)

if [ -z "$dtb" ]; then
    echo "post-image: ERROR $DTB_NAME not found under $BASE_DIR/build" >&2
    echo "post-image: the kernel did not build it. Check that" >&2
    echo "post-image:   - CONFIG_ARCH_ESPRESSIF=y and CONFIG_SOC_ESP32P4=y" >&2
    echo "post-image:   - patch 0012 applied (it adds espressif/Makefile)" >&2
    exit 1
fi

cp -a "$dtb" "$BINARIES_DIR/$DTB_NAME"
echo "post-image: published $DTB_NAME ($(stat -c %s "$BINARIES_DIR/$DTB_NAME") bytes)" >&2

# A quick self-check on the thing the board will actually consume. dtc is
# in host/bin because buildroot builds it for other packages; if it is not
# there, skip rather than fail -- the copy above is the deliverable.
DTC="$BASE_DIR/host/bin/dtc"
if [ -x "$DTC" ]; then
    model=$("$DTC" -I dtb -O dts "$BINARIES_DIR/$DTB_NAME" 2>/dev/null \
            | sed -n 's/^\tmodel = "\(.*\)";$/\1/p')
    echo "post-image: model = ${model:-<unreadable>}" >&2
fi
