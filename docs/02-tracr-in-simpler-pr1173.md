# 02 — TracR in Simpler: how PR #1173 works

> Part of the **tracr-compiler-project** context set. Read [00-pypto-profiling.md](00-pypto-profiling.md)
> (current PyPTO profiling) and [01-tracr-profiling.md](01-tracr-profiling.md) (the TracR contract)
> first.
>
> **Why this doc exists:** PR [hw-native-sys/simpler#1173](https://github.com/hw-native-sys/simpler/pull/1173)
> is the **hand-written reference implementation** of "profile Simpler with TracR." The big-plan
> compiler pass will *automate* exactly the instrumentation this PR places by hand. So this PR is
> both the proof that TracR works on-device and the concrete spec for what PyPTO should generate.

**PR:** *"TraCR: Upstream into Simpler as a first-class profiler"* by Noah Andrés Baumann
(`noabauma`), into `main`, +788 lines across ~24 files.

---

## 1. What the PR does, in one paragraph

It **vendors TracR as a git submodule** at `tools/tracr/` and wires it into the
`tensormap_and_ringbuffer` runtime behind a single build flag, **`BUILD_TRACR=ON`**. When
enabled, the AICPU scheduler threads and per-core AICore task execution are instrumented with
TracR markers; timestamps come from the **device hardware counter**; each thread appends to a
**per-thread ring buffer in device memory**; and at the end of a run the host **downloads the raw
buffers**, writes them as `.bts` files under `~/ascend/tracr_<N>/`, and you post-process them with
`tracr_process` into a **Perfetto** timeline. It replaces an out-of-tree development branch with an
in-tree, supported tool.

---

## 2. The motivation: overhead lands *outside* the measured region

The whole reason for TracR is fine-grained branch-to-branch tuning, where the profiler's own cost
must be smaller than the effect you're chasing. Measured on `Case1` paged attention (a2a3 onboard,
vanilla `aicpu_execute` ≈ **39.8 ms**):

| Profiler | `aicpu_execute` overhead | Where the cost lands |
| --- | --- | --- |
| **L2 swimlane** (level 4) | **+3.7 ms (+9%)** | **inside** the scheduling loop |
| **TracR** | **~0 ms in-loop** (+0.5–0.9 ms one-shot dump) | **outside** the scheduling loop |

The PR's argument against the current L2 swimlane collector (see [00](00-pypto-profiling.md) §3):

- **It perturbs the region it measures** — +9%, front-loaded, so it distorts *relative* comparisons.
- **It is lossy by design** — device ring buffers cap at **2¹⁶ records per stream** and silently
  drop the rest; on `Case1` the report was pinned at exactly `65,536` aicore + `65,536` aicpu records.
- **Its "zero overhead" over many iterations is just the data loss** — it stops collecting once full.

TracR on the same run collected **330,833 records** (no drops) into a **1M-record/thread** buffer at
16 B / nanosecond HW-counter resolution, with the scheduling region within noise of vanilla
(~40.4 ms vs ~39.7 ms). Cheap append on the hot path (`_traces[idx++] = payload`), one bulk ~5 MB
copy to HBM at the end.

---

## 3. The core adaptation: device-collect / host-download

TracR's vanilla model (see [01](01-tracr-profiling.md) §6) assumes a host process whose threads each
flush their own `.bts` file at finalize. **Simpler's orchestration + scheduler threads run on AICPU
on the device**, which cannot write host files. PR #1173 bridges that gap with three moves:

1. **`TRACR_DISABLE_FLUSH`** — turn off TracR's own file writing; buffers stay in memory.
2. **`USE_HW_COUNTER`** — timestamps from the device hardware counter (ns), not `clock_gettime`.
3. A **shared device buffer + host download** — one big device allocation sized for all AICPU
   threads; each thread `memcpy`s its ring buffer into its slice at finalize; the host copies the
   whole thing back and writes the `.bts` + `metadata.json` tree itself.

```
   device (AICPU)                                   host
   ─────────────                                    ────
   thread t: _traces[idx++] = payload   ── finalize ─▶  memcpy into shared
                                                        device buffer[t*CAPACITY]
                                             (run end)
   shared device buffer  ── copy_from_device ──────▶  StoreTracrData()
   per-thread sizes[]    ── copy_from_device ──────▶  TracrData2BTS()  → ~/ascend/tracr_<N>/
                                                       StoreTracrMetaData() → metadata.json
                                                       tracr_process → perfetto.json
```

---

## 4. Build wiring

| File | Role |
| --- | --- |
| `.gitmodules` | Adds submodule `tools/tracr` → `https://github.com/huawei-csl/TracR.git` (pinned at commit `b0f103a80`). |
| `.github/workflows/{ci,sanitizers}.yml` | Add `submodules: recursive` to every checkout so the submodule is present in CI. |
| `tools/tracr.cmake` | Defines `tracr_enable(target)` and the `BUILD_TRACR` option (env-overridable, default OFF). |
| `tools/tracr_postprocessing_script.cmake` | Builds the host-side `tracr_process` binary (Linux-only; only when `BUILD_TRACR`), copies the Paraver `state.cfg`. |
| `src/a2a3/platform/{onboard,sim}/{aicpu,host}/CMakeLists.txt` | Call `tracr_enable(...)` on `aicpu_kernel` / `host_runtime`. |
| `simpler_setup/kernel_compiler.py` | Adds TracR include dirs to orchestration compiles, and when `BUILD_TRACR=ON` injects `-DENABLE_TRACR -DTRACR_DISABLE_FLUSH -DUSE_HW_COUNTER` into the **PyPTO-generated orchestration `.so`**. |

**`tracr_enable(target)`** (in `tools/tracr.cmake`) does two things:
- Always adds TracR's include dirs (`tracr/include`, `tools/`, `tracr/extern` for nlohmann json) as
  **SYSTEM PRIVATE** (so TracR's headers don't trip Simpler's `-Wall -Wextra -Werror`).
- When `BUILD_TRACR` is ON, defines **`ENABLE_TRACR`**, **`TRACR_DISABLE_FLUSH`**, **`USE_HW_COUNTER`**,
  plus optional cache knobs **`TRACR_CAPACITY`** (default ≈ 1M traces/thread ≈ 17 MB/thread) and
  **`TRACR_POLICY`** (empty = abort-if-full; `TRACR_POLICY_PERIODIC` = overwrite; `TRACR_POLICY_IGNORE_IF_FULL` = drop).

> **Two compile surfaces need the flags.** The runtime libraries get them via CMake `tracr_enable`.
> But the orchestration `.so` is compiled *at runtime* by PyPTO's `kernel_compiler.py`, so the PR
> injects the same defines there separately — this is precisely the compile path a future PyPTO pass
> would emit markers into.

---

## 5. The marker set — `tools/tracr_simpler_markers.hpp`

A single X-macro defines all Simpler marker types and generates both the `enum` and the name array
(so registration and labels never drift):

```cpp
#define MARKER_TYPES \
    X(Orchestrating) X(Read_Dimensions) X(Reshape_Kernels) X(Pre_Loop_Info) \
    X(PTO2_SCOPE_) X(Scheduling) X(Phase1) X(Phase2) X(Phase3) X(Phase3b) \
    X(Phase4) X(Drain) X(Initializing) X(De_Initializing) X(DLL_loading) \
    X(Allocating) X(Running_Task_Single) X(Running_Task_Pair) X(Barrier)

enum MarkerType { /* X -> name, */ MARKERTYPE_COUNT };
constexpr std::string_view MarkerTypeNames[] = { /* X -> #name */ };
```

It also defines the per-thread identity used everywhere else:

- `std::atomic<int> g_TraCR_thread_idx_counter{0}` — hands out a dense thread index.
- `thread_local int g_TraCR_thread_idx{-1}` — this thread's index (its TracR **channel**).
- `tracr_getcpu()` — a portable `sched_getcpu()` shim (`-1` on non-Linux, e.g. the macOS packaging CI).

---

## 6. The channel model — mapping threads and cores to lanes

TracR channels are visualization lanes ([01](01-tracr-profiling.md) §2). This PR uses two channel
ranges:

| Lane range | Who | Channel expression | Marked with |
| --- | --- | --- | --- |
| `0 … aicpu_thread_num-1` | AICPU threads (orchestrator + schedulers) | `g_TraCR_thread_idx` | Orchestrating, Scheduling, Phase1–4, Drain, Barrier, Initializing, … |
| `aicpu_thread_num … +worker_count-1` | AICore units (AICube + AIVector) | `sched_thread_num_ + 1 + core_id` | `Running_Task_Single` / `Running_Task_Pair` (SET at dispatch, RESET at completion) |

`StoreTracrMetaData` writes the matching `channel_names` into `metadata.json`:
`AICPU_0…`, then `AICube_0…` (`worker_count/3` of them), then `AIVector_0…`
(`2*worker_count/3` of them), then `INVALID` — i.e. the a2a3 1 cube : 2 vector ratio.

**`extraId` carries identity.** The dispatch site sets the marker's `extraId` to the kernel's
`func_id` (`slot_state.task->kernel_id[subslot]`, where `subslot` picks the AIC vs AIV kernel of a
paired task); post-processing maps `func_id → kernel name` via the run's `kernel_config.py`. Other
sites use `extraId` for the scope iteration index, batch size, or `sched_getcpu()`.

---

## 7. Lifecycle — `TRACR_START()` / `TRACR_FINALIZE()`

Defined in `src/a2a3/runtime/tensormap_and_ringbuffer/aicpu/aicpu_executor.cpp` and called at the
top/bottom of `aicpu_execute()`:

- **`TRACR_START()`** — `g_TraCR_thread_idx = counter.fetch_add(1)`. Thread `0` calls
  `INSTRUMENTATION_START()` (creates the proc + main thread); every other thread calls
  `INSTRUMENTATION_THREAD_INIT()`. This satisfies TracR's "exactly one START, one THREAD_INIT per
  worker" contract using the dense thread index as the channel.
- **`TRACR_FINALIZE(runtime)`** — under `ENABLE_TRACR`, copies this thread's used buffer
  (`tracrThread->_traces`, length `_traceIdx`) into the shared device buffer at offset
  `g_TraCR_thread_idx * TraCR::CAPACITY`, and records `_traceIdx` into the per-thread sizes array.
  Then thread `0` calls `INSTRUMENTATION_END()` (and resets the counter); others call
  `INSTRUMENTATION_THREAD_FINALIZE()`. Finally `g_TraCR_thread_idx = -1`.

This is the device-side half of the "collect into a shared buffer" design from §3 — no file I/O on
device, one `memcpy` per thread at the very end.

---

## 8. Where the markers are placed

**`aicpu_executor.cpp`** (the AICPU entry + phase spans): `Initializing` (extra = cpu),
`Allocating`, `Orchestrating` (extra = thread_idx), `Scheduling` (extra = thread_idx),
`De_Initializing`; `TRACR_START()` at entry, `MARK_RESET` + `TRACR_FINALIZE()` at exit.

**`scheduler_dispatch.cpp`** (`resolve_and_dispatch`, the hot scheduling loop): per-phase spans
`Phase1` (completion check), `Phase2` (drain), `Phase3` (wiring), `Phase3b` (dummy drain), `Phase4`
(MIX dispatch), `Drain` (deferred release), and — under `INDEP_ORCH` — a `Barrier` span while the
scheduler waits for the orchestrator. In `prepare_subtask_to_core`, `Running_Task_Single` /
`Running_Task_Pair` are SET on the core's lane with `extraId = func_id`.

**`scheduler_completion.cpp`** (`check_running_cores_for_completion`): `MARK_RESET` on the core's
lane when its task completes — closing the `Running_Task_*` span opened at dispatch.

**`scheduler_cold_path.cpp`** (`deinit`): `MARK_RESET` across all per-core lanes on teardown.

**`examples/.../paged_attention/kernels/orchestration/paged_attention_orch.cpp`** (an *orchestration*
example — the same kind of file PyPTO generates): `Read_Dimensions`, `Reshape_Kernels`,
`Pre_Loop_Info` (extra = batch), and `PTO2_SCOPE_` (extra = scope iteration index) inside the
`PTO2_SCOPE()` loop. **This is the clearest preview of what a PyPTO pass would emit.**

**`runtime.h`** (both `tensormap_and_ringbuffer` and `host_build_graph`): adds `void *tracrData_` and
`void *tracrDataSizes_` (device pointers) with getters/setters; in TMARB they live in
`DeviceRuntimeLaunchDesc`.

---

## 9. Host side — alloc, download, store (`tools/tracr_simpler_api.hpp`)

Called from `src/a2a3/platform/{onboard,sim}/host/device_runner.cpp`, both guarded by
`#ifdef ENABLE_TRACR`:

- **`DevAllocTraCR(runner, runtime)`** — *before launch*. Allocates the shared trace buffer
  (`sizeof(Payload) * aicpu_thread_num * CAPACITY`) and the sizes array
  (`aicpu_thread_num * sizeof(size_t)`) in device memory; stores the pointers on `runtime`.
- **`StoreTracrData(runner, runtime)`** — *after the run*. `copy_from_device` both buffers to host,
  sets `tracr_dir = ~/ascend/tracr_<sampleID>/proc.<1000+device_id>`, calls `TracrData2BTS`, frees
  the device memory, and writes metadata. `sampleID` comes from `PYPTO_RUN_SAMPLE_ID` (so repeated
  runs don't clobber each other).
- **`TracrData2BTS(...)`** — writes each thread's slice to `thread.<t+1>/traces.bts` (raw `Payload`
  binary — exactly TracR's `.bts` format from [01](01-tracr-profiling.md) §6).
- **`StoreTracrMetaData(runtime)`** — writes `metadata.json` with the `channel_names` (§6),
  `markerTypes` (from `MarkerTypeNames`), and `pid`/`start_time`/`tid`.

The result is a standard TracR trace tree that `tracr_process` consumes with no special-casing.

---

## 10. `INDEP_ORCH` — a bundled orthogonal mode

The PR also adds an **`INDEP_ORCH`** build option (env-overridable) that makes the orchestrator run
**independently** — it finishes building the whole task graph before schedulers start dispatching
(a spin-wait `Barrier` on `orchestrator_done_` in `resolve_and_dispatch`). To hold a whole graph it
bumps several PTO2 buffer sizes (task window `16384 → 65536`, heap `256 MB → 512 MB`/ring, wiring
queue `1024 → 65536` — in both runtimes' `pto_runtime2_types.h`). This is separable from TracR but
bundled because it produces cleaner, separated orch-vs-sched phases in the trace.

---

## 11. How to use it

```bash
# 1. Build with TraCR on (submodule must be checked out)
BUILD_TRACR=ON pip install --no-build-isolation -e '.[test]'

# 2. Run any tensormap_and_ringbuffer case as usual (optionally set a sample id)
PYPTO_RUN_SAMPLE_ID=0 python tests/st/<scene>/test_<name>.py -p a2a3 -d 0

# 3. Traces land in ~/ascend/tracr_<N>/proc.<1000+dev>/thread.<t>/traces.bts
#    Post-process to Perfetto and open at ui.perfetto.dev
./build/output/bin/tracr_process ~/ascend/tracr_0/   # default = perfetto -> perfetto.json
```

Multi-device works today (one `proc.<1000+device_id>` folder per device). A5 support is future
work (pending hardware).

---

## 12. What this teaches the big plan

PR #1173 is, in effect, a **worked example of the compiler pass's output** — done by hand. Mapping it
onto "PyPTO generates TracR markers via a pass" ([00](00-pypto-profiling.md) §10):

| Concern | In PR #1173 (manual) | In the big plan (automated) |
| --- | --- | --- |
| Markers in **orchestration** code | Hand-edited into `paged_attention_orch.cpp` | **Emitted by a PyPTO pass** at the `EmitIndentedLine` choke point in `orchestration_codegen.cpp` |
| Marker registry | Static X-macro in `tracr_simpler_markers.hpp` | Pass emits/extends a registry keyed to compiler-known regions (scopes, tasks, phases) |
| Channel assignment | Hand-coded (`g_TraCR_thread_idx`, `sched_thread_num_+1+core_id`) | Stays in the **runtime** — the compiler emits markers; the runtime owns thread/core→lane mapping |
| Lifecycle (`START`/`FINALIZE`) | Hand-written in `aicpu_executor.cpp` | Stays in the **runtime** (it's runtime-thread machinery, not per-model) |
| Device buffer + host download | `tracr_simpler_api.hpp` + `device_runner.cpp` | Stays in the **runtime** |
| Build flag plumbing | `tracr.cmake` + `kernel_compiler.py` | Reused as-is; the pass just emits the marker calls that these flags gate |

**The key insight:** most of this PR is *runtime plumbing that stays put*. The part the compiler pass
must own is narrow and specific — **emitting `INSTRUMENTATION_MARK_SET/RESET` calls into the
generated orchestration C++**, with a stable eventId per region and a meaningful `extraId` (the
compiler already knows the `func_id`, scope index, batch, etc.). The `paged_attention_orch.cpp`
edits in this PR are the template: reproduce those automatically for every compiled program, and the
big plan is done.

### What this PR does *not* yet cover

PR #1173 instruments **compute + scheduling** on a single device (it is "multi-device today," one
`proc.<1000+device_id>` per device). It does **not** yet mark **data copy-in/out** (H2D/D2H, on-chip MTE
moves), it does **not** mark **communication** (HCCL collectives), and it does **not use TracR flows** —
so cross-rank message arrows and the compute/copy/comm overlap picture aren't captured. Extending the
marker set to those cost classes, adding flows for comm edges, and making regions **selectable** is the
subject of [05-benchmarking-compute-comm-copy.md](05-benchmarking-compute-comm-copy.md). The good news:
the runtime plumbing this PR built (device buffer, host download, per-device proc folders, HW-counter
timestamps) already generalizes to all three.
