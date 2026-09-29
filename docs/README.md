# tracr-compiler-project — context docs

Groundwork for the big plan: **PyPTO auto-generates TracR instrumentation markers via a compiler
pass**, so any compiled program can be profiled on Ascend hardware without hand-editing generated code.

**That goal is met (2026-09-29): the pass emits every communication marker, and device-to-device arrows
render on real silicon.** Docs 00–05 are the shared context, read in order as a narrative from *how we
profile today* to *how the pass should be built*. 06–08 are the plans, now annotated with what shipped;
[STATUS.md](STATUS.md) is the live state and the only file to read for "where are we".

| # | Doc | What it covers |
| --- | --- | --- |
| 00 | [00-pypto-profiling.md](00-pypto-profiling.md) | **How PyPTO profiles today.** The four mechanisms (compile profiling, runtime DFX / L2 swimlane / PMU, host `[STRACE]`, simulator Insight), what PyPTO actually emits into generated code (almost nothing — only orchestration C++), the pass-pipeline instrument hooks, and the codegen injection choke point. |
| 01 | [01-tracr-profiling.md](01-tracr-profiling.md) | **What TracR needs to profile code.** The full instrumentation contract: the macro API, the minimal integration sequence, the 16-byte Payload / channel / event model, `.bts` → Perfetto/Paraver output, build flags, and hot-path design. |
| 02 | [02-tracr-in-simpler-pr1173.md](02-tracr-in-simpler-pr1173.md) | **The first real integration.** How simpler PR #1173 wired TracR into the runtime by hand: device-collect / host-download design, build wiring, the marker set, the channel model, the `TRACR_START/FINALIZE` lifecycle — and a map of what stays in the runtime vs. what the pass must generate. |
| 03 | [03-ir-instrumentation-principles.md](03-ir-instrumentation-principles.md) | **The compiler-level principles.** How code generators inject profiling as an IR pass — with Devito as the deep worked example — plus a taxonomy of how LLVM, MLIR, Halide, TVM, Kokkos, NVTX/Tracy/ittnotify do it, and where PyPTO/TracR sits. |
| 04 | [04-codegen-instrumentation-blueprint.md](04-codegen-instrumentation-blueprint.md) | **The practical blueprint.** The concrete emission pattern (a `ProfilingLevel` policy + a pluggable TracR/Tracy/NVTX backend), mapped onto PyPTO's codegen, with generated-output examples and best practices — a minimal end-to-end design for the pass. |
| 05 | [05-benchmarking-compute-comm-copy.md](05-benchmarking-compute-comm-copy.md) | **Benchmarking scope (HPC).** Instrument **all three cost classes** — compute, data copy-in/out, communication — in one correlated timeline, with a **region selector** to choose which IR parts (loops, scopes, dispatches, copies, collectives) to trace, and single- vs multi-node methodology (flows for comm edges, per-rank correlation, overlap, straggler analysis). |
| 06 | [06-execution-plan.md](06-execution-plan.md) | **The build plan.** Repo/branch map, verified ground truth, the M0–M3 milestones, and the TracR-side deltas. |
| 07 | [07-aicore-tracing-direction.md](07-aicore-tracing-direction.md) | **Reaching the AICore — D1–D3 DONE.** Why collectives are invisible from above the core, the reframe (feed TracR's payload format rather than port TracR to CCEC), the verified preconditions, and the phases. D4 dropped with the host lane. |
| 08 | [08-codegen-comm-markers-plan.md](08-codegen-comm-markers-plan.md) | **Zero-user-marker comm tracing — C0–C5 DONE, as built.** Why the runtime is the wrong layer, what each step does, the two C2 designs and why the first was impossible, the peer-from-offsets rule and its measured coverage, and the shape constraints ptoas imposes. |
| — | [STATUS.md](STATUS.md) | **Current state.** Branch/commit map, onboard results, coverage numbers, what is open, whether it is PR-able, the build recipe, and the environment traps. |

## The throughline

```
  00  current PyPTO profiling ....... the baseline we integrate with / improve on
   │
  01  the TracR contract ............ the tool we're adopting (what it needs)
   │
  02  TracR-in-Simpler (PR #1173) ... the hand-written reference implementation
   │
  03  instrumentation principles .... the theory (Devito + the wider landscape)
   │
  04  codegen blueprint ............. how to build the pass on our stack
   │
  05  benchmarking scope ............ compute + copy + comm, selectable IR, single/multi-node
   │
   ▼
  06  execution plan ................ milestones M0-M3
   │
  07  reaching the AICore ........... D1-D3: TracR payloads written from the core itself
   │
  08  codegen comm markers .......... C0-C5: the pass emits them; D2D arrows on silicon
   │
   ▼
 [done]  a PyPTO compiler pass that emits TracR markers automatically
```

The key insight held up: most of the machinery already existed (TracR itself; the runtime plumbing in
PR #1173; the codegen choke point in PyPTO). Two findings were needed beyond it. **TracR never had to be
ported to CCEC** — a `.bts` file is a flat array of 16-byte payloads, so the AICore only has to write
records (07). And **ptoas passes a declaration-only `func.func private` through**, so PTO IR can call a
marker with no dialect change (08) — which is what let the comm markers come from the compiler rather
than from a user's hands.

## Repos referenced

- **pypto** — the compiler (`/home/georgios/workspace/hw-native-sys/pypto`)
- **simpler** — the PTO2 runtime, and where TracR is integrated (`/home/georgios/workspace/hw-native-sys/simpler`)
- **TracR** — the instrumentation library (`/home/georgios/workspace/TracR`, upstream `huawei-csl/TracR`)
- **devito** — the exemplar for IR-based instrumentation (`/home/georgios/workspace/devito`)
