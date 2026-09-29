# 08 — As built: PyPTO auto-generates comm markers and D2D arrows

> **Status: DONE and validated on silicon (2026-09-29).** This was the C-series plan; it is now the
> record of what shipped. Current state and results: [STATUS.md](STATUS.md). The AICore substrate this
> builds on: [07](07-aicore-tracing-direction.md).

pypto `tracr-codegen-pass` @ `cd389845` · simpler `tracr_l3` @ `90de9ee50`

---

## 1. Why codegen and not the runtime

PyPTO lowers `pld.system.notify` / `pld.system.wait` straight to the PTO ISA, so simpler's comm layer
never sees them — a runtime marker cannot reach a pypto-lib model's communication. The markers therefore
have to come from the compiler. That is the whole reason this work exists.

## 2. What the corpus looks like

The two ops are asymmetric, and that asymmetry decides the whole design:

```
pld.system.notify   target, peer, offsets, value      <- peer is an EXPLICIT operand
pld.system.wait     signal, offsets, expected         <- no peer; encoded in offsets
```

So **tails are free** (the peer is `op->args_[1]`, already passed to `EmitCommRemoteView`) and **heads
need analysis**: recover the sender from the wait's offsets.

A positional rule ("`offsets[0]` is the rank") was the obvious guess and is **wrong** — the rank axis is
second in the ring collectives, and is sometimes a constant. The rule that shipped is structural:
**exactly one non-constant offset component is the sender; anything else bails to a span.**

### Measured coverage (AST classifier mirroring the shipped rules, 71 files)

| corpus | notify → tail | wait → head |
| --- | --- | --- |
| `pypto/examples/distributed` | 15/15 (100%) | 8/14 (57%) |
| `pypto/tests/st` | 28/28 (100%) | 9/26 (34%) |
| `pypto-ccfusion` | 9/12 (75%) | 10/12 (83%) |
| **`pypto-lib` production models** | **103/103 (100%)** | **78/80 (97%)** |
| **total** | **155/158 (98%)** | **105/132 (79%)** |

The misses are honest refusals, not failures: `offsets=[0, 0]` is an aggregate one-cell signal several
peers notify, so the sender is genuinely unrecoverable (§7). What matters is that the production model
corpus is at 97%.

---

## 3. What each step does

| | what | commit |
| --- | --- | --- |
| C0 | ptoas passes a declaration-only `func.func private` through, so PTO IR can call a marker with **no dialect change** | — |
| C1 | span around every lowered notify/wait (`CommNotify`=19, `CommWait`=20) | `34291442` |
| C2 | the record buffer reaches the AICore lane | §4 |
| C3 | arrow **tail** at every notify | `15076ea0` |
| C4 | arrow **head** at a wait whose sender is derivable | `acccea2c` |
| C4b | no arrow at a **self-notify**, plus a runtime `src == dst` guard | `6bbc619a` |
| C5 | `seq` from the signal slot's constant offsets | `cd389845` |

---

## 4. C2 — two designs, and why the first was wrong

The first attempt was an IR pass (`InjectTracrBuffer`) that appended a buffer parameter. **Wrong, and the
lesson generalises:** there are two compile stages — pypto emits IR → `.pto` → ptoas → `.cpp`, then
ccec compiles that `.cpp` with `-DENABLE_TRACR`. A parameter committed in stage 1 **cannot** be removed
by a preprocessor flag in stage 2, so the "costs nothing when off" contract is unsatisfiable that way.
Reverted in `b7005263`.

Rebuilt on the platform's shipping mechanism — address in a `KernelArgs` field, stashed by the kernel
entry into a `[[block_local]] static`, read through a weak accessor (the
`chip_swimlane_aicore_rotation_table` precedent). That works for platform-resident kernels.

Hardware testing then showed it does **not** work for generated ones: `_link_incore` builds a
**self-contained AICore image** whose `.text` is copied and entered at offset 0, resolving against
nothing, so a generated kernel cannot call the platform accessor at all. Second half of C2:
`GlobalContext::tracr_aicore_slice`, published by the AICPU kernel entry and filled per core in all four
`scheduler_cold_path.cpp`; the generated prologue defines its own accessor over a `[[block_local]]` slot
seeded from `get_tracr_aicore_slice(args)`.

**Compile-time ideology.** Every marker entry point, emit helper and generated prologue block is
`#ifndef ENABLE_TRACR` → nothing. Not gated: the `GlobalContext` field (8 bytes), the per-core store at
cold start, the AICPU publish, one dlsym — deliberately, because `GlobalContext` is a host↔device wire
struct compiled by three separate toolchains and an `#ifdef`'d field shifts layout per translation unit.
Same reasoning as the shipping `KernelArgs::tracr_aicore_data_base`. Runtime cost when off is zero.

---

## 5. Emission shape

Comm ops emit PTO/MLIR text, not C++, from the single emitter
`src/backend/common/pto_ops_distributed.cpp`. Codegen declares the markers as extern and ptoas passes
the calls through:

```mlir
func.call @tracr_mark_set(%chan, %c19_i32, %extra)          // CommNotify span opens
pto.comm.tnotify(%peer_pview, %value ...)
func.call @tracr_flow_start(%chan, %rank, %peer, %seq)      // tail, INSIDE the span
func.call @tracr_mark_reset(%chan)

func.func private @tracr_mark_set(i32, i32, i32)            // prologue declarations
```

Both flow endpoints sit **inside** their span: a TraCR flow attaches to whatever span is open on the
channel, so a tail emitted after the reset would attach to nothing.

The C++ definitions come from `aicore/tracr_aicore_emit.h`, included by a prologue block PyPTO writes
above ptoas's output. Two shape constraints, both learned the hard way:

- ptoas re-declares every `func.func private` as **`static __aicore__ void f(...)`**, not `extern "C"` —
  so the header's definitions must be `static`, or it is a static-after-non-static redeclaration.
- the prologue's accessor must be **`always_inline`**. An out-of-line `[aicore]` function in a generated
  image silently produces wrong results even when never called, and `elf_parser.py` does not reject it
  (`kernel_entry` is still at `.text` offset 0). Mechanism never explained.

---

## 6. C4b — self-notify, and C5 — seq

**C4b.** `pld.system.notify(peer=my_rank, ...)` is a rank signalling its own slot to order two of its own
kernels; nothing crosses a device boundary. Before the rule, `alltoallv_gmm.py`'s self-notify paired with
an unrelated wait and rendered a 1→1 arrow. `PTOCodegen::IsCommRankRead` looks through SSA temps to a
`pld.system.rank` call, since user code always writes `my_rank = pld.rank(ctx)` and passes the temp.
**Known blind spot:** a rank arriving as a *kernel parameter* is not detected
(`tests/st/distributed/test_l3_self_notify_credit_reset.py`); the runtime `src == dst` guard covers it.

**C5.** Both endpoints must agree on `seq` with no communication, so it can only come from what both
write identically. Exactly one offset component varies — the rank, already carried as src/dst — and every
*constant* component is the fixed slot coordinate, spelled the same on both sides:

```
barrier A   notify offsets=[0, my_rank]   wait offsets=[0, src]
barrier B   notify offsets=[1, my_rank]   wait offsets=[1, src]
```

Four bits per component keyed by position, high bit marking "a constant lives here" so a literal 0 at
position 0 differs from no constant. Wraps merge two slots onto one id — pre-C5 behaviour, never a
mis-pair, because both sides compute the identical function. Measured effect on
`09_allreduce_two_phase` (4 ranks): 24 messages went from **12 ids** to **24**.

---

## 7. Out of scope, permanently

- **All-constant offsets** (`offsets=[0, 0]`). Several peers notify one cell; the sender is not recoverable
  from the IR. Span only.
- **Two or more non-constant offsets** (the ring's `[rs_round, left]`). The rule declines rather than
  guessing which is the rank. Covering it needs a *runtime* seq, since the slot coordinate is a round
  index rather than a constant — that is a follow-on, not a fix.
- **Cross-node flow-id uniqueness** (needs a rank prefix) and **on-chip MTE flows**.

---

## 8. Tests

`tests/ut/codegen/distributed/test_distributed_pto_codegen.py` (131 passing) covers: marker pairs around
both ops, the no-comm negative, tail-inside-span, head id matching, the aggregate and ambiguous negatives,
self-notify suppression, a computed peer still getting a tail, and C5's two cases (two barriers get
different seq; a matching pair agrees on seq). `tests/ut/codegen/test_pto_codegen.py` covers the prologue
include. On-device results are in [STATUS.md](STATUS.md).
