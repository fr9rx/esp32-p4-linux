# patches/linux/

<!-- SPDX-License-Identifier: GPL-2.0-only -->

**These patches are GPL-2.0-only, not MIT.**

The repository root carries an MIT license. It does not apply here, and that
is not a decision this project gets to make: every file in this directory is
a patch against the Linux kernel, a patch is a derivative work of the file it
modifies, and Linux is GPL-2.0. They inherit that. This holds equally for the
patches written for this board and for the ones taken from
[why2025-linux](https://github.com/mrbreaker/why2025-linux).

The full text is the `COPYING` file at the root of any Linux kernel tree —
which is also the only place these patches are of any use, since they are
applied to one.

## Where each one came from

`docs/BUILD.md` has the detail; this is the summary.

**From why2025-linux, unchanged** — the drivers, without which this board
does not boot at all:

`0001` CLIC irqchip + SYSTIMER clocksource + `esp32_uart` + P4 cache ops ·
`0002` GPIO · `0007` dw_mmc · `0010` NOMMU userspace pool · `0014` signal
`mcause` hardening · `0015` cache-thunk hardening · `0016` SYSTIMER
hardening · `0019` MWDT · `0023` `MPIE` on user return · `0031` M-mode
userspace · `0034` early MWDT arming

**From why2025-linux, modified:** `0022` — the SYSTIMER edge→level driver
hunk only; its DTS hunk edited the badge device tree and is folded into ours.

**Written for this board:**

| patch | what it does |
|---|---|
| `0012` | the Function EV Board device tree, replacing why2025's badge DTS |
| `0035` | `dw_mmc` FIFO-mode device-tree property |
| `0036` | the IDMAC descriptor-ring invalidate that made the SD card usable — without it any directory read hangs |
| `0037` | `__sramtext`: hot kernel text linked into internal SRAM |
| `0039` | `arch_dma_set_uncached()` via the +0x40000000 non-cacheable alias, which is what makes `dma_alloc_coherent()` real on this part |
| `0040` | let `/dev/mem` reach peripheral space, so registers can be read on a running kernel without a reflash |

Each patch carries its own header explaining what it does and why; several
also record what was measured on hardware to justify it.
