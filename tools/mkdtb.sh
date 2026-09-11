#!/bin/sh
# mkdtb.sh <in.dts> <out.dtb>
# Preprocess + compile a device tree the way the kernel build does.
#  - riscv cross gcc as the preprocessor (Git Bash ships no cpp)
#  - dtc is a native Windows binary, so its arguments need Windows paths
# Verified: reproduces images/esp32p4-function-ev.dtb byte for byte.
set -e
SP="$(cd "$(dirname "$0")" && pwd)"
CROSS=/c/Espressif/tools/riscv32-esp-elf/esp-15.2.0_20251204/riscv32-esp-elf/bin/riscv32-esp-elf-
"${CROSS}gcc" -E -nostdinc -I"$SP/dt-include" -undef -x assembler-with-cpp -D__DTS__ "$1" -o "$SP/.pp.dts"
dtc -I dts -O dtb -o "$(cygpath -m "$2")" "$(cygpath -m "$SP/.pp.dts")" 2>/dev/null
