# 06 — Execution plan: building the PyPTO TracR marker pass

> Part of the **tracr-compiler-project** doc set. Docs [00](00-pypto-profiling.md)–[05](05-benchmarking-compute-comm-copy.md)
> are *context and theory*; this doc is the **plan of record** — how we tackle it, who owns what, the
> milestones, and the concrete first target. It is the "[future] PyPTO compiler pass" the doc-set
> throughline points at, now started.
>
> **Status snapshot** (branches, commits, next action) lives in [STATUS.md](STATUS.md); this doc is the
> durable plan and is expected to change slowly.

---

## 1. The reframe that drives everything

The big-plan goal (auto-generate TracR markers via a PyPTO codegen pass) sounds TracR-centric, but the
**center of gravity is PyPTO + simpler, not TracR**:

- **TracR is nearly feature-complete for this.** Flows (`FLOW_START/END`) already exist and render; the
  `tracr_process` postprocessor already does the multi-proc k-way merge, per-proc `sync_start` anchoring,
  and flow arrows. The comm-visualization primitives doc [05](05-benchmarking-compute-comm-copy.md) asks
  for are *present* — they're just unused by the hand-written reference (PR #1173).
- **The pass is a PyPTO compiler change.** It emits `INSTRUMENTATION_MARK_SET/RESET` *strings* into
  generated orchestration C++ at the `EmitIndentedLine()` choke point in
  `src/codegen/orchestration/orchestration_codegen.cpp` — pure compiler territory.
- **The runtime plumbing stays put.** `TRACR_START/FINALIZE`, the device trace buffer, host download,
  the marker-set header, and the phase/scheduler markers all live on simpler's `tracr` branch and are
  consumed unchanged via PyPTO's `runtime/` submodule.

So the work splits cleanly by repo. A TracR branch was the initial instinct, but TracR's slice is the
*smallest* of the three.

---

## 2. Repo / branch map

| Repo | Location | Branch | Role in this project |
| --- | --- | --- | --- |
| **pypto** | `~/src/pypto` (vendors simpler as `runtime/` submodule) | **`tracr-codegen-pass`** (off `main`) | **The pass.** All new work: `ProfilingLevel` on `RunConfig` → `emitProfilePush/Pop` at `EmitIndentedLine()` → registry + region selector. |
| **simpler** | `~/src/simpler` | `tracr` (tracks `upstream/tracr`) | **Runtime plumbing** the pass emits into + the hand-written reference (PR #1173). Extend later to copy/comm markers + flows + barrier sync. |
| **tracr** | `~/src/tracr` | `main` | **Narrow postprocessing deltas** (later): multi-node clock correlation, `extraId`→bytes/bandwidth, buffer policy at scale. |
| **pypto-lib** | `~/src/pypto-lib` | — | Model definitions (e.g. `models/qwen3/14b/...`) used to exercise/verify the pass. |
| **tracr-compiler-project** | `~/src/tracr-compiler-project` | `main` | This planning repo (context docs + this plan). |

Ownership is collaborative: Georgios authored docs 00–05 and drives the PyPTO compiler direction; Noah
owns TracR + the simpler runtime and is doing the pass work on the `tracr-codegen-pass` branch.

---

## 3. Verified ground truth (as of this plan)

Findings from inspecting the actual `~/src/pypto` checkout — these anchor the pass work:

- **Injection choke point exists.** `EmitIndentedLine()` is confirmed as the emit primitive in
  `src/codegen/orchestration/orchestration_codegen.cpp` (~198 KB).
- **Structural regions exist.** The codegen already has a `RuntimeScopeStmt` visitor — that is the
  `PTO2_SCOPE()` region to wrap (Devito's `Section` analogue, doc [03](03-ir-instrumentation-principles.md)).
- **Policy home exists.** `RunConfig` (`python/pypto/runtime/runner.py:81`) carries the DFX flags
  (`enable_l2_swimlane`, `enable_pmu`, …) — the natural sibling location for a `profiling` /
  `ProfilingLevel` flag.
- **Greenfield confirmed.** No `tracr` / `INSTRUMENTATION_MARK` / `ProfilingLevel` / `emitProfile` code
  anywhere in pypto's `src/`, `include/`, or `python/`. Nothing to extend — but the infrastructure to add
  it is in place (doc [00](00-pypto-profiling.md) §7–9).
- **Registry already available.** The marker enum values (`PTO2_SCOPE_`, `Read_Dimensions`,
  `Reshape_Kernels`, `Pre_Loop_Info`, phases, `Running_Task_*`) are defined in simpler's
  `tools/tracr_simpler_markers.hpp`, and `simpler_setup/kernel_compiler.py` already injects the TracR
  include dirs and `-DENABLE_TRACR -DTRACR_DISABLE_FLUSH -DUSE_HW_COUNTER` into the orchestration `.so`
  under `BUILD_TRACR=ON`. The generated code just references the existing enum names.

---

## 4. Milestones

| # | Milestone | Repo(s) | State |
| --- | --- | --- | --- |
| **M0** | TracR-in-simpler baseline runs end-to-end (hand-written markers; PR #1173) | simpler `tracr` | **Done** — builds with `BUILD_TRACR=ON`, runs a model, `tracr_process` → Perfetto. |
| **M1** | **Pass auto-emits the Coarse orchestration markers PR #1173 placed by hand** | pypto `tracr-codegen-pass` | **Current.** See §5. |
| **M2** | L3 multi-device: **copy + comm** cost classes, **flows** for comm edges, **barrier-anchored** cross-rank sync | pypto + simpler + tracr | Planned (doc [05](05-benchmarking-compute-comm-copy.md)). |
| **M3** | **Region selector** + full `ProfilingConfig` (level / categories / selector / backend / flows) + optional Tracy/NVTX backends | pypto | Planned (docs [04](04-codegen-instrumentation-blueprint.md) §1, [05](05-benchmarking-compute-comm-copy.md) §7). |

Rationale for the ordering: M1 validates the *mechanism* against a known-good hand-written output (lowest
risk, highest signal) before widening scope in M2/M3.

---

## 5. Milestone 1 in detail — reproduce the hand edits automatically

Goal: for any compiled program, the pass emits the same **Coarse** orchestration markers PR #1173
hand-wrote into `paged_attention_orch.cpp` (doc [02](02-tracr-in-simpler-pr1173.md) §8) — no hand-editing
of generated code.

**Three edits, all on `pypto:tracr-codegen-pass`:**

1. **Policy flag.** Add a `ProfilingLevel` (`None` / `Coarse` / `Fine`) to `RunConfig`
   (`python/pypto/runtime/runner.py`), plumbed to codegen exactly like `enable_l2_swimlane`. `None` is the
   default → zero emission → today's behavior is unchanged until opted in.
2. **Emission helpers.** Add `emitProfilePush/Pop` to `orchestration_codegen.cpp` that call
   `EmitIndentedLine("INSTRUMENTATION_MARK_SET(g_TraCR_thread_idx, PTO2_SCOPE_, <idx>);")` /
   `INSTRUMENTATION_MARK_RESET(g_TraCR_thread_idx);`, and invoke them around the `RuntimeScopeStmt`
   emission (the `PTO2_SCOPE_` span) plus the function-entry phases
   (`Read_Dimensions` / `Reshape_Kernels` / `Pre_Loop_Info`).
3. **Registry.** None needed for the Coarse tier — the enum values already exist in
   `tracr_simpler_markers.hpp` (§3). The `extraId` carries the scope iteration index (or batch).

**Explicit-pop safety** (doc [04](04-codegen-instrumentation-blueprint.md) §5): TracR is SET/RESET, not
RAII, so emit the `RESET` at the matched structural boundary the codegen controls (scope block entry/exit)
— exactly what PR #1173 does — or generate a tiny RAII guard. Do not emit dynamic strings on the hot path;
the label is a `uint16_t` eventId, the dynamic part goes in the integer `extraId`.

**Verification (the before/after proof):**

```bash
# build with TracR, run qwen3-14b decode, post-process
cd ~/src/pypto/runtime && rm -rf build && BUILD_TRACR=ON pip install --no-build-isolation -e '.[test]'
cd ~/src/pypto        && rm -rf build && pip install --no-build-isolation -e .
cd ~/src/pypto-lib    && BUILD_TRACR=ON PYPTO_RUN_SAMPLE_ID=0 python3 models/qwen3/14b/decode_fwd.py -p a2a3 -d <dev>
~/src/pypto/runtime/build/output/bin/tracr_process ~/ascend/tracr_0    # -> perfetto.json
```

Success = `PTO2_SCOPE_` (and the entry-phase) spans now appear in the **generated-orchestration** lane of
the Perfetto trace. They are **absent today**, because codegen emits zero markers — the only markers in a
current run come from the runtime (scheduler/phase markers baked into the simpler submodule). That
absence-vs-presence is the clean signal that the pass works.

---

## 6. TracR-side deltas (deferred to M2/M3)

Kept narrow on purpose — these are the only genuine TracR-repo gaps:

1. **Multi-node clock correlation** in `tracr_process`. Today `sync_start` is each proc's min
   `start_time`, which assumes all ranks hit `INSTRUMENTATION_START()` simultaneously. Cross-chip HW
   counters have different origins and drift — needs a recorded clock-offset path or flow-causality
   bounding (doc [05](05-benchmarking-compute-comm-copy.md) §5).
2. **`extraId` → bytes/bandwidth** in postprocessing, for copy/comm GB/s. The `extraIdLabels` hook is read
   by the postprocessor but never written by the library.
3. **Buffer policy at scale** — one dump at finalize vs. periodic drains for long multi-rank runs.

Note: comm **arrows** are *not* a TracR gap (flows already work) — using them is a simpler-side task.

---

## 7. Open decisions / coordination points

- **TracR remotes & submodule bumps.** simpler's `tracr` submodule pins TracR; keep pins coordinated when
  TracR main advances. pypto's `runtime/` submodule is pinned per branch (see [STATUS.md](STATUS.md)).
- **Cross-node time base** — barrier-anchored sync vs recorded clock offset vs flow-causality bounds (§6.1).
- **Copy instrumentation point** — H2D/D2H likely marked in the runtime copy path; logical copy ops in
  generated orchestration (doc [05](05-benchmarking-compute-comm-copy.md) §8).
- **Comm marker source** — collectives live partly in kernels (AIV), partly in orchestration; mark both
  sides and connect with flows.
- **Overlap metric** — per-lane busy-union vs critical-path; pick one and report consistently.

---

The throughline holds: *where and what* to measure is a structural, selectable, compile-time decision;
*how cheaply* to measure is TracR's out-of-loop, per-thread, multi-proc design; and this pass is what makes
it automatic for every compiled program.
