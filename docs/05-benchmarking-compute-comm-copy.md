# 05 — Benchmarking compute, communication, and data movement

> Part of the **tracr-compiler-project** context set. Builds on
> [04-codegen-instrumentation-blueprint.md](04-codegen-instrumentation-blueprint.md) (the emission
> pattern) and [03-ir-instrumentation-principles.md](03-ir-instrumentation-principles.md) (the
> principles). Read [00](00-pypto-profiling.md) for what the current tools cover.
>
> **Why this doc exists:** a profiler that only times *compute* is half a tool. On a distributed AI
> accelerator the wall clock is a race between **compute**, **data movement (copy-in / copy-out)**, and
> **communication (collectives / point-to-point)** — and the wins come from *overlapping* them. This doc
> makes two things first-class in the plan:
> 1. **Benchmark all three cost classes** — compute, copy-in/out, and communication — in one correlated
>    timeline, single-node *and* multi-node.
> 2. **Select which IR regions to instrument** — loops (and which loop level), scopes, task dispatches,
>    copy ops, collectives — via a predicate over the IR, not an all-or-nothing switch.

---

## 1. The three cost classes on our stack

Everything that consumes wall-clock time on an Ascend deployment falls into one of three classes. A
useful profiler must attribute time to each and expose their **overlap**.

| Class | What it is on our stack | Where it happens | Current visibility ([00](00-pypto-profiling.md)) |
| --- | --- | --- | --- |
| **Compute** | AICore kernels — matmul on **AIC** (cube), elementwise/reduction on **AIV** (vector) | On-chip AICore clusters | Good: L2 swimlane (dispatch), PMU pipe utilization (cube/vec busy) |
| **Data movement (copy-in/out)** | **H2D/D2H** over PCIe/UB (host↔chip), and **on-chip** GM↔L2↔L1/UB moves via the **MTE** engines | Host bus + on-chip MTE2 (load) / MTE3 (store) | Partial: PMU reports `mte2_busy`/`mte3_busy` cycles; H2D/D2H itself is largely uninstrumented |
| **Communication** | **HCCL** collectives (allreduce one/two-phase/ring, allgather, reduce-scatter, broadcast, all-to-all, EP dispatch/combine) and cross-rank remote reads/writes through the **comm-domain window** | Inter-chip / inter-node network; L3 layer | **Gap**: L3 multi-process swimlane is *not supported today* |

Grounding: the host attaches to the chip over **PCIe** (x86) or **UB/HCCS** (Kunpeng), microsecond-scale
latency, so the runtime submits a *graph* not per-task dispatches. **GM (HBM)** is shared within a
`device_id`. In L3, **each rank is a separate OS process** (forked `Worker`), the host drives them over
IPC, and cross-rank traffic happens *inside the kernel* via an HCCL **comm-domain window** allocated by
`orch.allocate_domain(...)`. (See the Ascend chip-architecture and L3 allreduce walkthroughs.)

### The metric each class needs

| Class | Primary metrics | Roofline / model |
| --- | --- | --- |
| Compute | GFLOP/s, **operational intensity** (FLOP/byte), pipe utilization (cube/vec), occupancy (`block_dim`) | compute-bound vs memory-bound roofline |
| Copy | effective **GB/s**, H2D vs D2H asymmetry, transfer-size histogram, bus (PCIe/UB) utilization | bandwidth ceiling of the bus / HBM |
| Comm | **algbw vs busbw** (algorithm vs bus bandwidth, OSU-style), latency, message rate, **overlap %** with compute, **load imbalance** across ranks | ring/mesh bandwidth model; α–β (latency–bandwidth) cost |

The headline derived metric is **overlap efficiency**: `1 − wall / (t_compute + t_copy + t_comm)`. If the
three are perfectly serialized, overlap = 0; if fully hidden behind compute, overlap → 1. You cannot
compute this without instrumenting all three on one clock.

---

## 2. Mapping the three classes onto TracR

TracR's model ([01](01-tracr-profiling.md)) already has the three primitives we need — we just have to
use them deliberately:

**(a) Event category → marker type + lane.** Give each class its own marker types and its own
**channels** so the timeline reads as parallel swimlanes:

| Category | Example marker types | Channel (lane) | `extraId` carries |
| --- | --- | --- | --- |
| Compute | `Kernel_AIC`, `Kernel_AIV` (today: `Running_Task_Single/Pair`) | per-core lanes (`AICube_i`, `AIVector_i`) | kernel `func_id` |
| Copy | `H2D`, `D2H`, `OnChip_MTE_Load/Store` | dedicated `Copy`/`DMA` lane(s) per engine or per stream | **byte count** (→ GB/s in post) |
| Comm | `Collective_ReduceScatter`, `_AllGather`, `_RingStep`, `Remote_Read`, `Barrier_Wait` | per-rank `Comm` lane | message bytes / peer rank |

**(b) Flow events = the communication arrows.** `INSTRUMENTATION_FLOW_START(ch, flowId)` /
`FLOW_END(ch, flowId)` draw an arrow from one event to another **across threads, cores, or ranks/procs**
([01](01-tracr-profiling.md) §3). This is precisely message-passing visualization: emit `FLOW_START`
next to a remote write / send, `FLOW_END` at the matching remote read / recv, with a globally-unique
`flowId`. The result is the causal graph of a collective — the ring hops, the mesh exchanges — drawn as
arrows in Perfetto/Paraver. **PR #1173 does not use flows yet**; the plan should, for comms.

**(c) Multi-proc sync = multi-node correlation.** TracR writes one `proc.<pid>` folder per process; the
Simpler integration uses `proc.<1000+device_id>` per device ([02](02-tracr-in-simpler-pr1173.md) §9). The
postprocessor merges all procs by timestamp and shifts each to a common `sync_start` anchor, *assuming
all ranks hit `INSTRUMENTATION_START()` at nearly the same instant* — for us that anchor should be a
**collective barrier right after `allocate_domain`**, the moment all ranks are provably live. This makes
TracR multi-node by construction; it is already "multi-device today" per the PR.

---

## 3. Selective IR instrumentation — choosing *what* to trace

An all-or-nothing `ENABLE_TRACR` is too blunt for large runs. The plan must let you **select regions by a
predicate over the IR**, exactly as Devito's `track_subsections` selects node classes by verbosity
([03](03-ir-instrumentation-principles.md) §3.3). The selector is the union of these axes:

| Axis | Selects on | Example |
| --- | --- | --- |
| **Category** | compute / copy / comm | "trace comms only" for a collective-tuning run |
| **Node type** | `pl.range` / `pl.pipeline` loop, `PTO2_SCOPE`, `rt_submit_task`, `allocate_domain`, `make_tensor_external` / dump (copy), collective kernel | "wrap every task dispatch and every collective" |
| **Loop level / depth** | outer vs inner loop nest | **never the innermost loop** — wrap the nest, one marker pair per region per step ([03](03-ir-instrumentation-principles.md) principle 6) |
| **Cost threshold** | estimated FLOPs / bytes of the region | "skip regions below N FLOP" to keep marker density sane |
| **Name / tag** | an explicit user annotation | a `pl.profile_tag("attention")` analogous to the existing `pl.dump_tag` ([00](00-pypto-profiling.md) §6) |

**Compiler realization.** This is a filter on the codegen walk: the pass visits IR nodes it already
traverses, applies the selector predicate, and only emits `MARK_SET/RESET` (and `FLOW_*` for comm
edges) for matches — tagging each with its category (→ channel/eventId) and identity (→ `extraId`). It is
the direct analogue of Devito's `MapNodes(Section, NodeType).visit(iet)` + `verbosity_mapper`
distinguishing `HaloUpdateCall` / `HaloWaitCall` / `ComputeCall` / `BusyWait`
([03](03-ir-instrumentation-principles.md) §3.3). The selector is the configuration surface described in
[04 §1](04-codegen-instrumentation-blueprint.md) (`ProfilingLevel`), generalized from a single scalar to
`{category set, node-type set, max loop depth, cost floor, tags}`.

**Why selection matters for benchmarking rigor:** every marker you emit is a record and a (tiny) hot-path
cost. On a multi-rank, many-round run the buffer fills (TracR default 1M records/thread,
[01](01-tracr-profiling.md) §7) and the dump grows. Selecting only the regions under test keeps the
signal high, the buffer bounded, and the perturbation minimal — the whole reason TracR beats the L2
swimlane ([02](02-tracr-in-simpler-pr1173.md) §2).

---

## 4. Single-node benchmarking (intra-chip)

On one `device_id` the question is: *where does the chip spend time, and is anything idle while something
else is saturated?*

- **Decompose the AICPU side**: scheduler phases (`Phase1–4`, `Drain`), orchestration, dispatch overhead
  — already marked by PR #1173 ([02](02-tracr-in-simpler-pr1173.md) §8). This is the "is it
  dispatch-bound?" question the L2 swimlane answers, but at TracR fidelity with no in-loop cost.
- **Decompose the AICore side**: per-core `Running_Task_*` spans on `AICube`/`AIVector` lanes, correlated
  with **PMU** pipe utilization (cube/vec/MTE busy cycles). Rule of thumb: cube-heavy kernels want
  `max(mte2_busy, cube_busy) ≈ total`; if both compute and MTE2 are far below 100% the K-loop is serial
  (`pl.range` instead of `pl.pipeline`) — a DSL fix ([00](00-pypto-profiling.md) §3).
- **Decompose data movement**: mark H2D/D2H at the `copy_from_device` / `allocate_tensor` boundaries the
  runtime already has ([02](02-tracr-in-simpler-pr1173.md) §9), and on-chip MTE moves where they matter.
  Copy time that is *not* overlapped with compute is pure waste — the timeline shows it as a gap on the
  compute lane aligned with activity on the copy lane.
- **Overlap check**: the payoff. Are copies double-buffered behind compute? Is the scheduler filling
  cores while the previous batch drains? The parallel lanes make this visual; the derived overlap % makes
  it a number.

---

## 5. Multi-node benchmarking (inter-chip / distributed)

This is where the current tooling is weakest (L3 multi-process swimlane unsupported) and TracR's
per-proc, flow-aware, sync-anchored model pays off most.

- **Per-rank *and* aggregate.** One `proc.<device_id>` per rank ([02](02-tracr-in-simpler-pr1173.md) §9);
  the postprocessor merges them into one Perfetto trace. Report per-rank timelines *and* reduced stats
  (min/median/p95 across ranks) — an average hides the straggler that actually gates the collective.
- **Collective decomposition.** Mark the phases of each algorithm — reduce-scatter vs allgather, each ring
  step, each mesh exchange — as distinct comm events, and connect the actual transfers with **flows**
  (§2b). This turns "allreduce took 2 ms" into "ring step 3 waited 400 µs on rank 5."
- **Load imbalance / stragglers.** A collective is a barrier: the slowest rank sets the pace. A
  `Barrier_Wait` span on fast ranks (idle, waiting) directly measures imbalance; the flow arrows show
  which peer everyone is blocked on.
- **Bandwidth metrics.** From marked transfer bytes + span duration, derive **algbw** (bytes /
  collective time) and **busbw** (algbw × the algorithm's bandwidth factor, e.g. `2(P−1)/P` for
  ring-allreduce) — the OSU/`pytorch-hccl-tests` convention. busbw is the number you compare against the
  link ceiling.
- **Compute↔comm overlap across the batch.** In pipelined/TP/EP models the win is hiding comm behind
  compute of the next micro-batch. The correlated timeline is the only way to confirm the overlap
  actually happens on-device rather than just in the schedule.

### The multi-node clock problem (call it out)

Per-device HW-counter timestamps ([02](02-tracr-in-simpler-pr1173.md) §3) are **not automatically
comparable across chips/nodes** — counters start at different origins and can drift. TracR's `sync_start`
shift assumes a common start instant; for distributed runs that assumption must be *enforced*, not
hoped for:

- Anchor on a **barrier** right after `allocate_domain` (all ranks provably live) and treat that as t=0
  per proc.
- For absolute cross-node alignment, record a known offset (clock exchange) or rely on **flow causality**
  (a `FLOW_END` cannot precede its `FLOW_START`) to bound skew rather than trusting raw timestamps.
- Within a single multi-die chip, dies driven by one AICPU share a time base; across nodes they do not.

This is the classic distributed-tracing correlation problem (Paraver and Perfetto both solve it with sync
events) — the plan must budget for it explicitly.

---

## 6. Benchmarking methodology for large-scale distributed experiments

Instrumentation is necessary but not sufficient; the measurement discipline is what makes numbers
trustworthy. Baked into the plan:

1. **Warm-up then steady state.** Drop the first N iterations (allocation, JIT of first dispatch, cold
   caches, HCCL window setup). Report steady-state rounds only. (The runtime's benchmark harness already
   does timed rounds — [00](00-pypto-profiling.md) §baseline.)
2. **Distributions, not means.** Report min / median / p95 / p99 per rank and across ranks. Tail latency
   is the story in distributed collectives.
3. **Perturbation budget.** The profiler's own cost must be **smaller than the effect size** you're
   chasing. This is *the* TracR thesis: append on the hot path, dump once outside the measured region
   ([02](02-tracr-in-simpler-pr1173.md) §2). When hunting a 2–5 % branch delta, the L2 swimlane's +9%
   in-loop cost disqualifies it; TracR stays within noise.
4. **No silent data loss.** Size buffers (TracR `TRACR_CAPACITY`) for the *full* run at the chosen
   selectivity, or use an explicit policy (`PERIODIC` / `IGNORE_IF_FULL` / abort —
   [01](01-tracr-profiling.md) §7) — never let truncation masquerade as "zero overhead" (the swimlane
   2¹⁶-cap trap, [02](02-tracr-in-simpler-pr1173.md) §2).
5. **Capture order.** Baseline wall time → coarse (phase/scope) → fine (per-task/copy/comm) → PMU. Don't
   optimize inner tiles while a collective or a copy is the actual bottleneck
   ([00](00-pypto-profiling.md) §1).
6. **Reproducibility.** Pin platform (a2a3/a5, sim vs onboard), rank count, problem size, `block_dim`,
   `aicpu_thread_num`, and the profiling selector; record them with the trace. Use `PYPTO_RUN_SAMPLE_ID`
   ([02](02-tracr-in-simpler-pr1173.md) §9) to keep per-run traces separate.
7. **One change per iteration.** Re-baseline after each knob change; attribute deltas to a single cause.
8. **Sim vs hardware.** Correctness and structure validate in sim (`a2a3sim` thread-based HCCL window);
   real timing, bus/HCCL bandwidth, and PMU require onboard.

---

## 7. What the compiler pass must expose

Folding this into the pass design from [04 §7](04-codegen-instrumentation-blueprint.md), the config
surface grows from a single `ProfilingLevel` to:

```text
ProfilingConfig {
    categories : set<Compute | Copy | Comm>        # which cost classes to trace
    selector   : { node_types, max_loop_depth,     # which IR regions (§3)
                   cost_floor, tags }
    level      : None | Coarse | Fine              # density within selected regions
    backend    : TracR | Tracy | NVTX              # emit target (§04)
    flows      : bool                              # emit FLOW_* for comm edges
}
```

- **Compute** category → wrap task dispatches / kernel scopes (exists in PR #1173).
- **Copy** category → wrap H2D/D2H and significant on-chip MTE moves; `extraId` = bytes.
- **Comm** category → wrap collective phases + emit flows across ranks; `extraId` = bytes / peer.
- The **selector** is applied during the codegen walk; the **level** tunes density inside matches; the
  **runtime plumbing** (channels, `TRACR_START/FINALIZE`, device buffer, host download, per-rank proc
  folders) is reused unchanged from PR #1173 ([02](02-tracr-in-simpler-pr1173.md) §12).

---

## 8. Open questions to resolve in design

- **Cross-node time base** — barrier-anchored sync vs recorded clock offset vs flow-causality bounds (§5).
- **Buffer sizing at scale** — 1M records/thread × many ranks × many rounds; does the selector keep it
  bounded, or do we need periodic host drains? (PR #1173 does one dump at finalize.)
- **Copy instrumentation point** — is H2D/D2H best marked in the generated orchestration C++, or in the
  runtime's copy path (like `TRACR_START`)? Likely runtime for transfers, codegen for logical copy ops.
- **Comm marker source** — collectives live partly in kernels (AIV) and partly in orchestration; the pass
  must mark both sides and connect them with flows.
- **Overlap metric definition** — per-lane busy-union vs critical-path; pick one and report it
  consistently.

The throughline holds: *where and what* to measure is a structural, selectable, compile-time decision
([03](03-ir-instrumentation-principles.md)); *how cheaply* to measure it is TracR's out-of-loop,
per-thread, multi-proc design ([01](01-tracr-profiling.md), [02](02-tracr-in-simpler-pr1173.md)); and the
pass ([04](04-codegen-instrumentation-blueprint.md)) is what makes it automatic for every compiled
program — now across compute, copy, **and** communication, single-node and multi-node.
