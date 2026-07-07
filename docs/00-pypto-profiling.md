# 00 — How profiling works in PyPTO

> Part of the **tracr-compiler-project** context set. Next: [01-tracr-profiling.md](01-tracr-profiling.md)
> (what TracR needs to profile code) and [02-tracr-in-simpler-pr1173.md](02-tracr-in-simpler-pr1173.md)
> (the first real TracR integration in the Simpler runtime).
>
> **Why this doc exists:** the big-plan goal is for PyPTO to auto-generate TracR
> instrumentation via a compiler pass. To know what that pass replaces or complements,
> we first need a clear map of how PyPTO profiles code *today* — what it emits, what the
> runtime collects, and where in the compiler a new pass would attach.

Source repos: `pypto` (compiler) and its vendored `pypto/runtime/` (which *is* the
Simpler PTO2 runtime — the same code as the `simpler` working dir). Paths below are
relative to `pypto/`.

---

## 0. The one distinction that avoids all confusion

PyPTO the **compiler** (`python/`, `src/`, `include/`) and the **runtime** it targets
(`runtime/`, = Simpler) are different layers, and *most* profiling happens in the
**runtime**, not the compiler. The compiler injects almost nothing into the code it
generates. Keep this split in mind:

```
  ┌─────────────────────── pypto compiler ───────────────────────┐
  │  Python DSL → IR → passes → codegen                          │
  │  • times ITSELF (compile profiling)                          │
  │  • emits almost no instrumentation into generated code       │
  └───────────────┬──────────────────────────────────────────────┘
                  │ generates orchestration C++  +  .pto kernels
                  ▼
  ┌─────────────────────── simpler runtime ──────────────────────┐
  │  executes on Host ↔ AICPU ↔ AICore                           │
  │  • device DFX collectors: L2 swimlane, PMU, dep_gen, …       │
  │  • host [STRACE] timing                                      │
  │  • everything fine-grained lives HERE                        │
  └───────────────────────────────────────────────────────────────┘
```

---

## 1. The four mechanisms at a glance

PyPTO has **four distinct profiling/observability mechanisms**. They are easy to
conflate; only **one** injects anything into generated code.

| # | Mechanism | What it measures | Layer | Turn on with |
| --- | --- | --- | --- | --- |
| 1 | **Compile profiling** | Wall time of the *compiler* (parse / each pass / codegen) | pypto host | `PYPTO_COMPILE_PROFILING=1`, `ir.compile(profiling=True)`, `RunConfig(compile_profiling=True)` |
| 2 | **Runtime DFX** | On-device execution: kernel/dispatch timing, pipe utilization, deps, tensor I/O | simpler runtime | `RunConfig` flags / pytest `--enable-*` |
| 3 | **Host `[STRACE]` timing** | Per-stage host run timing (device_wall / orch / sched) | simpler runtime (host) | always-on log markers, read by `bench.py` |
| 4 | **Simulator Insight trace** | Cycle-accurate intra-kernel AICore pipeline | Ascend `msprof op simulator` | export tool + `clean_sim_trace` |

The runtime-DFX mechanism (2) is itself a *taxonomy* of tools mapped to profiling
**levels** (this is the canonical playbook framing):

| Level | What you learn | Primary artifact |
| --- | --- | --- |
| **Compile** | Pass / codegen / device time split | `report/pipeline_profile.{txt,json}` |
| **L2** | Kernel dispatch, AICPU scheduler, inter-kernel gaps | `dfx_outputs/l2_swimlane_records.json` |
| **L1 PMU** | Per-task pipe utilization (MTE / cube / vec) | `dfx_outputs/pmu.csv` |
| **L1 Insight** | Intra-kernel pipeline timeline (MindStudio) | `OPPROF_*/simulator/…` |
| **Debug** | Task deps, scope timing, tensor I/O | `deps.json`, `scope_stats/`, `tensor_dump/` |

> Capture order matters: **baseline → L2 swimlane → L1 (PMU/Insight)**. Don't optimize
> tiles while the L2 swimlane still shows a dispatch-bound or idle-core problem.

---

## 2. Compile profiling — timing the compiler itself

Implemented in `python/pypto/compile_profiling.py` (`CompileProfiler` class, thread-local
`current()`, a `stage()` context manager, `summary()`/`to_json()`/`write_report()`).

Four opt-in surfaces (see `docs/en/dev/01-compile-profiling.md`):

1. **Env var** `PYPTO_COMPILE_PROFILING=1` — zero code change.
2. **`ir.compile(program, profiling=True)`** — report written to `<output_dir>/report/`.
3. **`with CompileProfiler(): …`** — the context-manager form.
4. **`RunConfig(compile_profiling=True)`** — forwarded into `ir.compile(profiling=...)`.

**Artifacts:** `<output_dir>/report/pipeline_profile.txt` and `.json`.

**How it's wired:** it is *not* a special code path — it is a **`CallbackInstrument`** on
the pass pipeline (`pass_manager.py::_run_with_profiling`), with `before_pass`/`after_pass`
hooks that time each pass. When off, the overhead is a single null-check per stage boundary
(`_stage()` returns `nullcontext()`). This matters for the big plan: **the pass pipeline
already supports instrument hooks** (see §7).

---

## 3. Runtime DFX — device profiling flags

These are declared on `RunConfig` (`python/pypto/runtime/runner.py`) and mirrored as pytest
flags (`tests/st/conftest.py`). The canonical flag ↔ `CallConfig` ↔ artifact table lives in
`docs/en/dev/03-runtime-dfx.md`.

| `RunConfig` field | pytest flag | Bare default | Artifact (`dfx_outputs/`) |
| --- | --- | --- | --- |
| `enable_l2_swimlane: bool` | `--enable-l2-swimlane` | `True` | `l2_swimlane_records.json` |
| `enable_pmu: int` | `--enable-pmu [N]` | `2` (PIPE_UTILIZATION) | `pmu.csv` |
| `enable_dump_tensor: int` | `--dump-tensor [LEVEL]` | `1` (partial) | `tensor_dump/` |
| `enable_dep_gen: bool` | `--enable-dep-gen` | `True` | `deps.json` |
| `enable_scope_stats: bool` | `--enable-scope-stats` | `True` | `scope_stats/scope_stats.jsonl` |

Notes:
- Enabling **any** DFX flag auto-forces `save_kernels=True` (`runner.py::any_dfx_enabled`), so
  `<work_dir>/dfx_outputs/` survives the run. All DFX artifacts land there
  (`CallConfig.output_prefix`).
- **L2 swimlane levels** (`L2SwimlaneLevel`): `0` DISABLED, `1` AICORE_TIMING, `2` AICPU_TIMING,
  `3` SCHED_PHASES, `4` ORCH_PHASES. Higher = more spans = more perturbation.
- **PMU event types** (`--enable-pmu N`): `1` ARITHMETIC, `2` PIPE_UTILIZATION *(default)*,
  `4` MEMORY, `5` MEMORY_L0, `6` RESOURCE_CONFLICT, `7` MEMORY_UB, `8` L2_CACHE. Override via
  `SIMPLER_PMU_EVENT_TYPE`.
- Deprecated alias: `runtime_profiling` / `--runtime-profiling` → `enable_l2_swimlane`.

> **The L2 swimlane is the key baseline for the big plan.** It is PyPTO's current answer to
> "which kernel / dispatch is slow," it produces Perfetto-viewable JSON — and it is exactly
> what TracR aims to beat on overhead and fidelity (see [02](02-tracr-in-simpler-pr1173.md)).

---

## 4. Host `[STRACE]` timing → benchmark stats

`runtime/src/common/log/include/common/strace.h` defines RAII host span markers that log
`[STRACE] … name=… ts=… dur=…` lines, gated on the `SIMPLER_PROFILING` build macro (default on).
PyPTO reads them back in `python/pypto/runtime/bench.py` (`BenchmarkStats`), exposing
`device_wall_us` and orch/sched slices. This is how a per-run device/host timing number is
obtained **without** paying device-DFX overhead — it's the coarse, always-on "bird's-eye"
split (~5 phase spans), not fine-grained kernel timing.

---

## 5. Simulator MindStudio Insight (L1, cycle-accurate)

Intra-kernel AICore pipeline behavior comes from the Ascend **`msprof op simulator`**
(cycle-accurate camodel), not a normal run:

- Export per-kernel traces from a compiled case (`export_all_kernel_insight.py` in pypto-lib;
  `incore-profiling` skill / `incore_profile.py` for `build_output/` kernels). Each kernel gets
  `…/OPPROF_*/simulator/` with `visualize_data.bin` + `trace.json`.
- The official `trace.json` is lossy vs `visualize_data.bin`; PyPTO's `clean_sim_trace`
  (`python/pypto/tools/clean_sim_trace.py`, see `docs/en/dev/04-simulator-trace-cleaning.md`)
  recovers per-instruction metrics and de-clutters SET_FLAG/WAIT_FLAG noise into a richer
  Perfetto view.
- **AICore-only.** The op simulator returns `ACL_ERROR_RT_FEATURE_NOT_SUPPORT` (207000) on the
  AICPU KFC path — for the full AICPU + orchestrator timeline you need hardware
  `msprof --application`.

---

## 6. What PyPTO actually emits into generated code

This is the section that matters most for the compiler pass. **Answer: almost nothing, and
only on the orchestration (AICPU) path.** The `.pto` MLIR / AICore path emits **no** profiling
instrumentation at all.

Two things are emitted, both in `src/codegen/orchestration/orchestration_codegen.cpp`:

**(a) Selective tensor-dump markers — always emitted, latched host-side.**
`EmitSelectiveDumpCall()` emits `<task_var>.dump(v1, v2, …);` for params carrying the
`kAttrDumpVars` attribute. After simpler#953 the dump *level* (off/partial/full) is latched
host-side from `CallConfig.enable_dump_tensor`; **codegen only emits the per-task
`Arg::dump(...)` markers**. The DSL surfaces that seed them are `pl.dump_tag(t)`,
`pl.submit(..., dumps=[...])`, `pl.at(..., dumps=[...])` (parsed in
`python/pypto/language/parser/ast_parser.py`). This is I/O capture, **not timing**.

**(b) Orchestration timing markers — emitted but `#if`-gated OFF by default.**
Two sites wrap the dynamic dependency-vector construction with cycle timers:

```cpp
#if PTO2_ORCH_PROFILING
    uint64_t t0 = rt_orch_profile_now();
#endif
    // … build std::vector<PTO2TaskId> …
#if PTO2_ORCH_PROFILING
    rt_orch_profile_add_dynamic_dep_vector(rt_orch_profile_now() - t0, 0);
#endif
```

`PTO2_ORCH_PROFILING` defaults to **0** (`runtime/src/common/task_interface/profiling_config.h`),
so these compile to nothing unless the runtime is rebuilt with the macro on. **PyPTO exposes no
flag/env to flip it** — it's a runtime compile-time macro, alongside `PTO2_PROFILING` (default 1),
`SIMPLER_PROFILING` (default 1), `PTO2_SCHED_PROFILING` (0), `PTO2_TENSORMAP_PROFILING` (0).

**There is no general per-task timestamp / PMU / counter injection in codegen, and no
instrumentation pass.** PMU reads, swimlane records, dep_gen edges, and scope stats are all
collected by the **runtime**, not injected by the compiler.

---

## 7. Where profiling is collected & summarized

**Device DFX collection (runtime side).** Per-arch collectors under
`runtime/src/{a2a3,a5}/platform/` and `runtime/src/common/platform/`:
PMU (`pmu_collector*.cpp`), L2 swimlane (`l2_swimlane_collector*.cpp`),
dep_gen (`dep_gen_collector*.cpp`), tensor dump / scope stats
(`tensor_dump_collector.cpp`, `scope_stats_collector.cpp`). Each writes a fixed artifact under
`CallConfig.output_prefix`.

**Post-run dispatch (pypto side).** `runner.py::_collect_dfx_artifacts` fans out per enabled
flag (skipping silently if an artifact is missing), synthesizes a `func_id → name` map via
`_write_name_map` so tools show real kernel names, and either runs the converter (L2 swimlane,
onboard only) or prints a render hint (dep_gen / tensor dump / scope stats).

**Summarization tools** (`runtime/simpler_setup/tools/`): `swimlane_converter.py` (joins
`l2_swimlane_records.json` + `deps.json` → Chrome-trace `merged_swimlane_*.json`),
`sched_overhead_analysis.py` (AICPU scheduler-overhead / Tail-OH breakdown),
`deps_viewer` / `deps_to_graph`, `dump_viewer.py`, `scope_stats_plot.py`, `strace_timing.py`.

Viewers: Perfetto ([ui.perfetto.dev](https://ui.perfetto.dev)) for swimlane/cleaned traces;
MindStudio Insight for `visualize_data.bin`.

---

## 8. The pass pipeline & instrumentation hooks

Where a future instrumentation pass would attach:

- **Registration & order:** `python/pypto/ir/pass_manager.py::PassManager._register_passes()`
  defines the ordered pass lists per `OptimizationStrategy`. Each entry is a `(name, factory)`
  tuple where the factory binds a C++ pass (e.g. `passes.inline_functions()`,
  `passes.allocate_memory_addr()`). Passes are appended to a C++ `PassPipeline` and run via
  `self._pipeline.run(...)`.
- **C++ bindings:** `python/bindings/modules/passes.cpp` exposes the `passes` submodule, binds
  `PassPipeline` (`add_pass`/`run`), each pass factory, and the instrument machinery.
- **Pass core:** `src/ir/transforms/passes.cpp` (`Pass` pimpl, `CreateProgramPass`/
  `CreateFunctionPass` factories); concrete passes are the `src/ir/transforms/*_pass.cpp` files.
- **Instrument hooks:** `PassContext` carries `PassInstrument`s — existing ones are
  `VerificationInstrument`, `CallbackInstrument`, `ReportInstrument`, `DiagnosticInstrument`.
  Compile profiling (§2) is itself a `CallbackInstrument`; `ReportInstrument` emits the memory
  report keyed to `AllocateMemoryAddr`.

So a future IR-level instrumentation pass slots into the `_register_passes()` list as a new
`*_pass.cpp` bound in `passes.cpp`, and/or a new `PassInstrument`. **No such pass exists today** —
every current pass is a transform / lowering / analysis pass.

---

## 9. Codegen structure & the injection choke point

Entry point `python/pypto/backend/pto_backend.py::generate()` dispatches to single-chip /
multi-chip / distributed generators.

| Path | Generator | File | Emits instrumentation? |
| --- | --- | --- | --- |
| `.pto` MLIR / AICore | `PTOCodegen` | `src/codegen/pto/pto_codegen.cpp` | **No** |
| Orchestration C++ / AICPU | `GenerateOrchestration()` → `aicpu_orchestration_entry` | `src/codegen/orchestration/orchestration_codegen.cpp` | The two `#if PTO2_ORCH_PROFILING` timers + tensor-dump markers |
| Distributed L3 host | `DistributedCodegen` | `src/codegen/distributed/distributed_codegen.cpp` | No |

**The emit primitive:** `src/codegen/code_emitter.cpp` (`CodeEmitter::EmitLine()` /
`AppendRaw()`). In orchestration codegen the wrapper is **`EmitIndentedLine()`**
(`orchestration_codegen.cpp`) — the single choke point where a
`"<task>.mark(...);"` / timestamp / **TracR marker** call string would be written into the
generated orchestration output. The visitor that walks task-dispatch statements is
`OrchestrationStmtCodegen : public CodegenBase`.

---

## 10. What's missing today — and what it means for the big plan

Explicitly verified as **absent** in the PyPTO compiler:

- **No TracR integration** anywhere in `python/`, `src/`, `include/`, or `docs/`.
- **No `msprof` integration** in compiler code (only in runtime skills/docs).
- **No instrumentation / tracing / profiling transform pass** in `src/ir/transforms/`.
- **No PMU / timestamp injection into `.pto` MLIR** (AICore path emits none).
- **No PyPTO flag** to enable the runtime's `PTO2_ORCH_PROFILING` / `PTO2_SCHED_PROFILING`.

And structurally thin today:

- **Compute-/dispatch-centric.** The good tools (L2 swimlane, PMU) profile kernel dispatch and AICore
  pipe utilization. **Data copy-in/out** (H2D/D2H, on-chip MTE moves) and **communication** (HCCL
  collectives) have no first-class, correlated profiling — and **L2 swimlane across multi-chip processes
  is not supported** (the scene_test guard). Benchmarking compute + copy + comm together, single- and
  multi-node, is a stated goal — see [05-benchmarking-compute-comm-copy.md](05-benchmarking-compute-comm-copy.md).

**Implications for "PyPTO generates TracR markers via a compiler pass":**

1. It's greenfield — there is no existing instrumentation pass to extend, but the
   infrastructure to add one is in place (`_register_passes()` + `PassInstrument`).
2. The natural target is the **orchestration (AICPU) path**, because that's the only path that
   already carries injected statements and the only one where TracR-in-Simpler currently lives.
   The `.pto`/AICore path has no emission point for host-style markers.
3. The concrete injection point is `EmitIndentedLine()` in `orchestration_codegen.cpp` — a pass
   (or codegen hook) would emit `INSTRUMENTATION_MARK_SET/RESET(...)` around scopes and task
   dispatches, exactly the calls that PR #1173 today writes **by hand** into the same generated
   file (see [02-tracr-in-simpler-pr1173.md](02-tracr-in-simpler-pr1173.md)).
4. TracR is the intended replacement for the **L2 swimlane** as the fine-grained collector —
   the pass must emit markers that reproduce (and exceed) the swimlane's kernel/scheduler
   visibility, while keeping the collector's overhead *outside* the measured region.
5. The pass must cover **all three cost classes** — compute, data copy-in/out, and communication —
   selectably per IR region, single- and multi-node. That scope is designed in
   [05-benchmarking-compute-comm-copy.md](05-benchmarking-compute-comm-copy.md).

Read [01-tracr-profiling.md](01-tracr-profiling.md) next for the marker contract the pass must
satisfy, then [02-tracr-in-simpler-pr1173.md](02-tracr-in-simpler-pr1173.md) for the hand-written
reference implementation the pass will automate.
