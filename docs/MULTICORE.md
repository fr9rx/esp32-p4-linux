# The second HP core

**Status: unused. `CONFIG_SMP` is off and should stay off for now.**

The ESP32-P4 has two HP cores (`SOC_CPU_CORES_NUM (2U)`) and ESP-IDF runs
FreeRTOS SMP across both of them, so this is a software gap, not a hardware
one. why2025 states the position in a single line of `HARDWARE.md` —
"Linux runs on **one** HP core in M-mode, NOMMU. The second HP core is
unused" — with no rationale, so read it as *not done*, not *proven
impossible*.

This file records what it would actually take, because the answer is not
"set `CONFIG_SMP=y`" and the reason is not obvious.

---

## The blocker: there is no IPI source

riscv Linux takes its inter-processor interrupts from exactly four in-tree
providers. Grep confirms it — these are every caller of
`riscv_ipi_set_virq_range()` in 6.18:

| provider | available here? |
|---|---|
| `drivers/clocksource/timer-clint.c` | **no** — the P4 has no CLINT. That is precisely why why2025 had to write a SYSTIMER clocksource in the first place |
| `arch/riscv/kernel/sbi-ipi.c` | **no** — excluded by construction: `config RISCV_SBI ... depends on !RISCV_M_MODE` |
| `drivers/irqchip/irq-riscv-imsic-early.c` | **no** — IMSIC is AIA, not on this chip |
| `drivers/irqchip/irq-aclint-sswi.c` | **no** — ACLINT SSWI, not on this chip |

With none of them, `riscv_ipi_set_virq_range()` is never called,
`ipi_virq_base` stays 0, and `arch/riscv/kernel/smp.c:176` trips

```c
if (WARN_ON_ONCE(!ipi_virq_base))
        return;
```

on every CPU that tries to enable IPIs. No reschedule IPI and no
`smp_call_function` means the scheduler cannot move work between cores —
`CONFIG_SMP=y` would build and then not work.

why2025's CLIC driver is **176 lines with no SMP awareness at all**: no
per-CPU handling, no IPI ops. It is a single-hart driver.

## The hardware to build one on

ESP-IDF's `hal/esp32p4/include/hal/crosscore_int_ll.h` uses

```
HP_SYSTEM_CPU_INT_FROM_CPU_0_REG
HP_SYSTEM_CPU_INT_FROM_CPU_1_REG
```

— one software interrupt per source core, in the `HP_SYSTEM` block, inside
the peripheral window. That is what IDF's `esp_ipc` rides on, and it is the
raw material for an IPI provider.

One catch: a single bit per core, so IPI *type* cannot be distinguished in
hardware, and riscv wants eight logical IPIs. The standard answer is
`ipi_mux_create()` (`kernel/irq/ipi-mux.c`) — one physical interrupt plus a
software bitmap, which is how several other platforms do it.

## Starting the core is the easy half

`arch/riscv/kernel/cpu_ops.c:16` already defaults to `cpu_ops_spinwait`,
and `RISCV_BOOT_SPINWAIT` is `default y if RISCV_SBI_V01 || RISCV_M_MODE`.
The secondary hart spins on `__cpu_spinwait_stack_pointer` /
`__cpu_spinwait_task_pointer` until the boot hart fills them in. So the
kernel side of bring-up is already there; what is missing is a bootloader
that parks HP core 1 at the kernel entry, and a `cpu@1` node in the DTS.

## What would actually be dangerous

Not the IPI. The two things underneath it:

**1. The cache ops are not SMP-safe.** Patch 0015 runs every ROM cache
thunk under `local_irq_save` because, in its own words, "the ROM sync
engine is non-reentrant; ESP-IDF spinlocks it." `local_irq_save` protects a
core from its own interrupts and does nothing whatsoever against the other
core — two harts in a non-reentrant ROM routine is a real hazard, and it
would need a genuine spinlock as IDF uses.

There is also a circularity: `local_flush_icache_all()` is local, and under
SMP riscv broadcasts icache flushes *by IPI* — the thing that does not
exist yet.

**2. The CLIC erratum is uncharacterized with two harts.** Patch 0031
exists because of a silicon interrupt-delivery latch that wedged the kernel
under process churn, and the fix is to keep every `mret` M-to-M so the
privilege-dropping path that trips it never runs. Whether that latch
behaves the same way when two harts are taking CLIC-delivered interrupts is
entirely unknown, and `docs/RUNTIME-WEDGE.md` in why2025 shows what finding
it cost the first time.

## Recommendation

Leave it. The payoff is small for what this machine is: it is memory-bound
long before it is CPU-bound — 32 MB, `binfmt_flat` making one physically
contiguous allocation per exec, a 4 MB ceiling, and no compaction because
NOMMU cannot migrate pages. A second core adds no memory. In exchange you
would be building new code on the one part of the stack with a known
silicon bug.

**If you want the second core, asymmetric use is far cheaper than SMP.**
Core 1 running bare-metal firmware for one job, talking to Linux through
shared memory and that same cross-core interrupt, gets you the core without
touching the scheduler, the cache ops, or the erratum.

**If you want real SMP later**, the order is:

1. IPI provider on `HP_SYSTEM_CPU_INT_FROM_CPU_x`, plus `ipi_mux`
2. make the cache ops SMP-safe (real spinlock, IPI-broadcast icache flush)
3. bootloader releases core 1 into the spinwait entry
4. `cpu@1` in the DTS
5. `CONFIG_SMP=y`, `CONFIG_NR_CPUS=2`

Note that none of 1–3 does anything observable without 4, so this cannot be
landed or tested incrementally — which is a large part of why it is not
worth starting until something concrete needs it.
