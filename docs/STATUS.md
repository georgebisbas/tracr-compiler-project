# STATUS — TracR compiler-pass work

> Fast-moving snapshot of the working state. The durable plan is
> [06-execution-plan.md](06-execution-plan.md). Update this file freely.

**Last updated:** 2026-08-26

---

## Current repo / branch / commit state

| Repo | Branch | Commit | Notes |
| --- | --- | --- | --- |
| **pypto** | `tracr-codegen-pass` | `0583106e` ("merging newest main") | Working branch for the pass (M1 landed at `616a86b7`). **0 behind `main`, 8 ahead** — fully caught up. Pushed to the fork: [github.com/noabauma/pypto/tree/tracr-codegen-pass](https://github.com/noabauma/pypto/tree/tracr-codegen-pass). Push must use **SSH** (the `gh`/HTTPS token lacks `workflow` scope and the branch history touches `.github/workflows/`). |
| ↳ pypto `runtime/` submodule | `tracr_l3` | `4cc3d13d` | Tracks the L3 branch (pin == simpler `tracr_l3` HEAD exactly). |
| **simpler** | `tracr_l3` (off `tracr`) | `65b383d6` ("Draw TraCR flow arrows for group NEXT_LEVEL submits") | M2 steps 1–3 + M2a + M2b + group flows committed. Both clones (`~/src/simpler` and `~/src/pypto/runtime`) are on this commit and clean. |
| **tracr** | `main` | `916b0fd` | Unchanged; postprocessing deltas deferred to M2/M3. |
| **tracr-compiler-project** | `main` | `7b830c5` ("adding updates on our roadmap") | This repo. |

ℹ️ Docs 00–05 + `docs/README.md` are now present (brought in by `68ca7b9`, "docs: add TracR profiling &
compiler-instrumentation context set"); `06`/`STATUS` were rebased on top. `origin`
(`github.com/georgebisbas/tracr-compiler-project`) still isn't fetchable/pushable from this host (no SSH
key) — arrange a key or push from the system that has one.

---

## Done

- **M0** — TracR-in-simpler baseline runs end-to-end (hand-written markers).
- Created `pypto:tracr-codegen-pass` off `main`; pinned `runtime/` submodule to simpler tracr.
- **M1 — DONE & verified.** The pass auto-emits `INSTRUMENTATION_MARK_SET(g_TraCR_thread_idx,
  PTO2_SCOPE_, <idx>)` (SET-only) before every generated `PTO2_SCOPE` in
  `src/codegen/orchestration/orchestration_codegen.cpp`, plus the TracR `#include`s — **unconditional**,
  gated by the existing `BUILD_TRACR` / `ENABLE_TRACR` build mechanism (Option A). 3 golden blocks in
  `tests/ut/codegen/test_orchestration_codegen.py` updated. **Verified** via the qwen3-14b decode
  oneliner (inside the `pypto3-hw-native-sys:cann9` container): `PTO2_SCOPE_` spans now appear in the
  generated-orchestration lane. **Committed as `616a86b7` ("M1 done") and pushed** to the fork branch
  [tracr-codegen-pass](https://github.com/noabauma/pypto/tree/tracr-codegen-pass).
- **M2 step 1 — DONE & validated (sim).** The L3 host-**scheduling** lane via PyTraCR: PyTraCR built as a
  native pybind11 cmake target (`install(TARGETS tracr DESTINATION .)`); guarded helper
  `python/simpler/_tracr_l3.py`; `orchestrator.py` / `worker.py` emit `AllocateDomain` / `SubmitNextLevel`
  / `Drain` on an `L3_Orchestrator` lane (main-thread only). Validated on
  `examples/workers/l3/allreduce/main.py -p a2a3sim -d 0-1`; host lane aligned with device lanes via
  `USE_HW_COUNTER` + host `start_time=0` (fixing the sync_start/normalization mismatch). Manual markers in
  the hand-written `paged_attention_orch.cpp` example removed (redundant now the pass emits them).
- **M2 step 2 — per-chip lanes: DONE & exercised (sim).** The two transient dispatch bodies
  (`worker.py` `_ensure_comm_base` comm-init, `_dispatch_control_domain` alloc/release) `THREAD_INIT`/
  `FINALIZE` and emit `CommInit` / `AllocDomainChip` / `ReleaseDomainChip` on per-chip lanes (chip `i` →
  lane `i+1`; helper gained `thread_init`/`mark_set_chip`/… + `Chip_i` channel names). Exercised by the
  same allreduce sim run that validated step 3.
- **M2 step 3 — host→device flows: DONE & validated (sim).** Arrows render from the `L3_Orchestrator`
  lane's `SubmitNextLevel` events into each chip's device `Orchestrating` event.
  - **Path:** host [orchestrator.py `submit_next_level`] stamps a per-run `flowId`
    (`_tracr.next_flow_id()` → `cfg.flow_id`) and emits `FLOW_START` on the orchestrator channel right
    after the `SubmitNextLevel` mark → the id rides `CallConfig` (7th int32) through the whole-struct
    mailbox memcpy → the child's `DeviceRunner::run()` copies it onto `runtime->dev.flow_id` per run →
    the AICPU orchestrator emits `FLOW_END` right after the `Orchestrating` marker
    (`aicpu_executor.cpp`), gated on `flow_id != 0`. `_tracr_l3.py` gained `next_flow_id()`/`flow_start()`.
  - **Carrier finding (durable):** `CallConfig` is **never uploaded to the AICPU** — the device sees only
    `runtime->dev` (`DeviceRuntimeLaunchDesc`), which the host fills field-by-field from `CallConfig`
    (like `aicpu_thread_num`). So "read the mailbox `CallConfig` on-device" is impossible; a `dev.flow_id`
    field is the only clean carrier (and the idiomatic one). 6 wire/serializer sites kept in sync:
    `call_config.h` (static_assert 6→7), nanobind `def_rw`, `remote_wire` encode/decode, Python `_CFG_FMT`
    reader, trb+hbg `DeviceRuntimeLaunchDesc` + accessors, both `Runtime` ctors.
  - **Scope:** a2a3 only (a5 compiles clean — no code references the new accessor; the shared `CallConfig`
    field is inert there); `submit_next_level_group` (1 submit → N runs) and cross-node `flowId`
    uniqueness deferred. 13 files in `~/src/pypto/runtime`, committed as `b5ee0ca5` ("adding new l3
    flows from host orch to dev"); pypto submodule bumped to it in `f93a4a06`.
  - **Fixed in passing:** `_relocate_proc()` left an empty `<base>/tracr/` container after moving the host
    proc out — now `rmdir`'d.
- **M2a — copy cost lanes: DONE & RENDERING CONFIRMED (sim, 2026-08-26).** Host H2D/D2H spans with
  `extra` = transferred bytes, at the one chokepoint every copy funnels through:
  `copy_to_device` / `copy_from_device` in `src/common/platform/{sim,onboard}/host/device_runner_base.cpp`.
  - **Design decision — fold into the per-device proc, not a separate host proc.** `host_runtime` is
    compiled with `TRACR_DISABLE_FLUSH`, so the `INSTRUMENTATION_*` macros keep traces in memory and never
    write them out; a host-side marker would be silently discarded. So copies record into
    `tools/tracr_host_copy.hpp` (one lane per recording thread, RAII `Span` emitting the SET/RESET pair
    together) and `StoreTracrData()` serializes them as extra `thread.<n>` lanes + `HostCopy_<i>` channel
    names inside the existing `proc.<1000+device_id>`. No TracR proc lifecycle, no `tracr.cmake` change,
    and copies land visually beside the device work they feed. The rejected alternative (a real host TracR
    proc per chip-worker) would flush once at process teardown, so all runs' copies would pile into
    `tracr_0` — worse for the cold-vs-warm comparison, and TracR hard-`exit()`s on lifecycle misuse.
  - **Self-instrumentation suppressed:** `StoreTracrData` downloads the device trace buffers via
    `copy_from_device` (tens of MB per AICPU thread); an RAII `Suppress` keeps that profiling overhead out
    of the lanes.
  - New marker types `CopyH2D` / `CopyD2H` appended to the `MARKER_TYPES` X-macro (indices 18/19 — nothing
    shifted). Committed as `1193a806` ("adding M2a approach from claude"); lands for a5 too (shared files).
  - **Confirmed in a real trace** from `allreduce/main.py -p a2a3sim -d 0-1`: `tracr_process` *does* pick up
    `thread.<n>` folders beyond the AICPU-thread count and match them to the appended channel names — the
    open question is now closed. `perfetto.json` carries a `HostCopy_0` lane inside **each** device proc
    with bytes in `extra_id`:
    `proc.1006 / proc.1007`, tid 78, 3 copies each — 1024 B @ ~53 µs, 23,610,304 B @ ~3.5 ms, 2512 B @ ~24 µs.
- **Merges caught both repos up to main (2026-08).** pypto absorbed 50 main commits (now 0 behind);
  simpler merged the updated `tracr`. Two structural changes from main collided with the TracR work and
  were resolved by taking main's shape and re-applying the instrumentation: `DeviceRunner::run()` →
  `prepare_execution()` + `PreparedExecution` (so `set_flow_id` now sits beside the sibling `dev`-field
  stamps), and the **L2→Chip rename** (`enable_l2_swimlane` → `enable_chip_swimlane`, per codestyle rule
  13). The `_CFG_FMT` conflict was the dangerous one — ours had 7 int32 (incl. `flow_id`), theirs 6 with
  the renamed field; kept 7 + the rename, verified Python and C++ both compute **1148 bytes**.
- **Onboard multi-device now runs.** `examples/workers/l3/allreduce/main.py -p a2a3 -d 0-1` passes
  **10/10** on 910B2. The first attempt died with an AICore `VEC instruction error: the ub address out of
  bounds` → `507018`; it cleared after the failed run's own `force_reset_device()` at finalize, i.e. it was
  poisoned device state, not a code defect (see Traps).

- **M2b — cross-device comm timing: SPIKE LANDED & validated (sim).** The Phase-2 allreduce barrier is
  timestamped **on the AICore**, because both production collectives issue `TWAIT` inline on the AICore —
  the AICPU never observes the wait, so no AICPU-side marker can see it. Three files in
  `examples/workers/l3/allreduce/`:
  - `kernels/aiv/allreduce_onephase_kernel.cpp` — `get_sys_cnt_aicore()` (50 MHz, 20 ns/tick) read into a
    4th `ChipTensor` arg (`args[3]`): slot 0 = barrier entry, slot 1 = notifies issued, slot `2 + peer` =
    that peer's `TWAIT` return. Per-peer slots are what make the *identity* of the remote rank observable.
  - `kernels/orchestration/allreduce_onephase_orch.cpp` — `expected_arg_count = 6`, `params.add_output(timing)`.
  - `main.py` — int64 `host_timings` tensor per rank (`OUTPUT_EXISTING`), both `CoreCallable.build` and
    `ChipCallable.build` signatures widened to `[IN, OUT, INOUT, OUT]`, kernel include dirs extended to the
    per-backend `aicore/` dir, and `report_barrier_timing()` converting ticks → µs.
  - **Validated:** `-p a2a3sim -d 0-1` exits 0, both ranks match golden, and the barrier report prints
    (rank 0: notify 6.0 µs, total 377.9 µs, peer1 371.9 µs; rank 1: notify 0.6 µs, total 1399.7 µs, peer0
    1399.1 µs). **The absolute numbers are simulator artifacts** — sim runs one OS thread per AICore, so the
    inter-rank skew is host-scheduler noise. The spike proves the *mechanism*; onboard is what will give
    physically meaningful numbers.
  - Deliberately **not** the "proper" path (a 5th swimlane pool kind), which is 8 files × 2 arch trees.
    Committed as `4cc3d13d`, rebased onto the post-merge `tracr_l3` (the only conflict was main's
    `PTO2OrchestrationConfig` → `OrchestrationConfig` rename, codestyle rule 9).

- **Group flow arrows — DONE & validated (sim, 2026-08-26).** L3 multi-device traces now draw one arrow
  per chip. Previously only `submit_next_level` stamped `cfg.flow_id`; the L3 examples dispatch through
  `submit_next_level_group`, which never did, so their traces had **zero** flow events.
  - **Why one shared id was not enough:** `tracr_process.cpp:620-634` pairs flow starts to ends *by index*
    within a single id, so 1 start + N ends draws one arrow and drops the rest (`flow_alltoall.cpp` states
    the rule: "a flowId must be unique per message and shared by both endpoints").
  - **Carrier problem:** a group is one `TaskSlot` with one `CallConfig` (`orchestrator.cpp:794`,
    `s.config = config`), so there is nowhere to put N distinct ids per member.
  - **Solution — base + group_index.** `submit_next_level_group` reserves a block of `group_size`
    consecutive ids (`_tracr_l3.next_flow_id_block`) and opens one arrow per member; `cfg.flow_id` carries
    only the **base**, and each dispatch adds its own `group_index` to recover its id —
    `worker_manager.cpp` (local mailbox) and `remote_endpoint.cpp` (remote L3). A single submit has
    `group_index == 0`, so its id is unchanged and step 3 behaviour is untouched.
  - **Validated:** `allreduce -p a2a3sim -d 0-1` → 4 flow events, 2 matched pairs, distinct ids:
    `id=1 L3_Orchestrator → AICPU_3 @ proc.1000`, `id=2 L3_Orchestrator → AICPU_2 @ proc.1001`.
    Trace kept at `~/tmp/tracr_l3_flows/perfetto.json`. Committed as `65b383d6`.

- **M2b onboard — MEASURED (2026-08-26, 6 runs, a2a3 910B2, devices 0-1, UNLOCKED).** The barrier is
  ~4 orders of magnitude cheaper than the skew around it, and sim hid this completely (sim showed both
  ranks symmetric at ~350-450 us).

  | run | rank 0 wait | rank 1 wait | late arriver |
  | --- | --- | --- | --- |
  | 1 | 0.5 us | 5037.3 us | rank 0 |
  | 2 | 5762.9 us | 0.5 us | rank 1 |
  | 3 | 0.5 us | 10278.7 us | rank 0 |
  | 4 | 2823.8 us | 0.5 us | rank 1 |
  | 5 | 3955.2 us | 0.5 us | rank 1 |
  | 6 | 0.5 us | 9222.9 us | rank 0 |

  - **The D2D primitive is sub-microsecond and rock stable.** `TNOTIFY` costs **0.8-1.4 us** across all 12
    per-rank measurements; the *winner's* wait is **exactly 0.5 us** in all six runs — that is the latency of
    observing a peer notify that has already landed.
  - **The barrier is dominated by arrival skew of 2.8-10.3 ms**, which is **not systematic to either rank**
    (late arriver alternated 3x rank 0 / 3x rank 1) and varies 3.6x run to run.
  - **Consequence for M3.** AICore-level markers would instrument the ~0.9 us part. The 3-10 ms part is host
    dispatch / AICPU orchestration — the tier M2 steps 1-3 + group flows already instrument. **Onboard data
    says M3 is not the highest-value next step**; making the existing host+device lanes trustworthy onboard
    (M2c) is.
  - **Caveats.** `task-submit` is no longer on PATH on this box (host or container), so all six runs were
    **unlocked** — skew is exactly the quantity most vulnerable to a co-tenant. Devices showed no processes
    before the runs. Also: the onboard log carries only 2 `[STRACE]` lines where sim emits dozens, so the
    per-chip `chip.run.bind` spans needed to attribute the skew are **not available today** — itself an
    argument for the M2c/instrumentation work.

## In progress / next action — M2 (L3 multi-device)

Compute = M1 (done); L3 host **scheduling** = M2 steps 1–3 (done, sim); copy cost = M2a (implemented);
comm cost = M2b (spike landed & validated in sim). Nothing is blocked. In priority order:

1. **D1 — AICore TracR lane, vertical slice.** New direction, fully scoped in
   [07-aicore-tracing-direction.md](07-aicore-tracing-direction.md). Both production collectives issue
   `TWAIT` inline on the AICore, so the collective is invisible to every marker above the core — reaching
   that tier is the difference between profiling *around* communication and profiling it. **The reframe:
   TracR never has to run under CCEC.** A `.bts` file is a flat array of 16-byte `Payload` structs
   (`tracr_process.cpp:172`), so the AICore only has to *write the records* and the host to serialize
   them; both halves already exist here (chip swimlane writes 32 B records to GM from AICore on silicon;
   M2a's `HostCopyTraces2BTS` already emits lanes TracR never recorded). D1 tests that claim cheaply by
   reusing the M2b timing `ChipTensor` as a fixed-capacity Payload buffer — no swimlane-pool integration.
   D2 (real transport), D3 (D2D arrows), D4 (onboard clock) follow.

2. **M2c onboard clock sync — now clearly the top item.** Onboard multi-device runs, but the host lane is
   not aligned to the device clock, so the `L3_Orchestrator` lane and every flow arrow land in the wrong
   place the moment you leave sim. The 6-run onboard result above makes this the gating work: the thing
   worth seeing is a 3-10 ms inter-chip arrival skew, and seeing it requires the host and both device lanes
   on one trustworthy timeline. **Check `simpler_setup/tools/clock_correlation.py` first** — it already does
   two-anchor device->host alignment with drift correction and an uncertainty bound, and every run already
   emits the `[CLOCK_ANCHOR]` lines it consumes. This may be wiring, not research. TracR's own `sync_end`
   interpolation is unimplemented, so this is a ready-made replacement.
3. **Onboard multi-clock sync (M2c)** — now the *gating* item, because onboard multi-device works but the
   host lane is not aligned to it: host CPU counter ≠ device AICPU counter, so on a2a3 onboard the
   `L3_Orchestrator` lane and the step-3 flow arrows can land in the wrong place. Sim is aligned
   (`USE_HW_COUNTER` + host `start_time=0`). Needs a recorded host↔device offset or a barrier-anchored
   `sync_start`. Until it lands, **validate multi-device in sim** and treat onboard host-lane placement as
   unverified.
4. **Run the M2b spike onboard.** Sim numbers are host-scheduler noise (one OS thread per AICore), so the
   barrier report only proves the mechanism. `-p a2a3 -d 0-1` on 910B2 is what yields real notify/wait
   costs. After that, decide whether to promote the spike to the "proper" path — a 5th swimlane pool kind
   feeding `chip_swimlane_collector.cpp`, so the timestamps land in the merged trace as a comm lane rather
   than a stdout table. That is 8 files × 2 arch trees, so it is worth doing only once the numbers justify
   it. The device→device *arrow* stays blocked on TracR-in-CCEC either way.

**Deferred, with reasons:** cross-node `flowId` uniqueness (needs a rank prefix; the `remote_wire` path already carries the field);
a5 port of steps 3/M2a (a2a3-only by choice — a5 compiles clean); on-chip MTE flows. **Device→device flows are no longer
blocked on "TracR into CCEC"** — see [07](07-aicore-tracing-direction.md); they are scoped as D3, gated on D1/D2
rather than on a compiler port.

**Known structural quirk:** the PyTraCR host proc only ever lands in `tracr_<PYPTO_RUN_SAMPLE_ID>`, while
device procs march `tracr_0, tracr_1, …` (one per run, `sampleID++`). So warm runs have device + copy lanes
but **no** `L3_Orchestrator` lane. For a trace with host + device together, run a single iteration.

---

## Environment traps (each cost a debugging session)

- **`BUILD_TRACR=ON` is a *build*-time flag — setting it only on the run gives you no trace at all.**
  `CMakeLists.txt:38` reads it at configure time. A plain `pip install --no-build-isolation -e .` rebuilds
  the runtime **without** TracR, and the next run then completes normally and silently writes nothing.
  Always `BUILD_TRACR=ON pip install --no-build-isolation -e .` after touching runtime C++.
- **`~/src/tracr/build/postprocessing/tracr_process` needs GLIBC_2.38, which `quirky_robinson` lacks.**
  Use one of the copies built inside the container instead — e.g.
  `/mounted_home/src/simpler-lazyv2-tracr/build/output/bin/tracr_process .` run from the `tracr_<N>` dir.
  `main.py` does **not** generate `perfetto.json`; that is a separate postprocessing step.
- **Not every `simpler-cann9` container can load the venv's `_task_interface.so`.** The extension links
  `CXXABI_1.3.15`; `serene_napier` / `dazzling_shaw` / `relaxed_chatterjee` ship an older
  `/lib/aarch64-linux-gnu/libstdc++.so.6` and fail at *import* with `version 'CXXABI_1.3.15' not found`.
  `quirky_robinson` (`pypto3-hw-native-sys:cann9`) has it and runs the L3 examples fine. The error looks
  like a broken build; it is a wrong-container error. Check with
  `strings /lib/aarch64-linux-gnu/libstdc++.so.6 | grep -c CXXABI_1.3.15` before debugging anything else.
- **Always activate the venv — a bare `python` picks up a pre-rename `simpler_setup` and the kernel will
  not compile.** In `quirky_robinson` the system install at
  `/usr/local/python3.12.13/lib/python3.12/site-packages/simpler_setup/_assets/src/common/task_interface/tensor.h`
  still declares `struct alignas(64) Tensor` (pre L2→Chip rename); the checkout declares `ChipTensor`. Run
  the L3 examples without the venv and every `__gm__ ChipTensor *` cast in the kernel fails with
  `'ChipTensor' was not declared in this scope; did you mean 'Tensor'?` — including the pre-existing
  input/output/scratch casts, so it is **not** caused by the M2b spike. The staleness is **per container**:
  `serene_napier`'s system copy has `ChipTensor`, `quirky_robinson`'s does not — never generalise from one.
  With the venv on, `import simpler_setup` resolves to `/mounted_home/src/simpler/simpler_setup/__init__.py`
  (the `_editable_skbc_simpler.pth` editable install) and the same command passes. Working invocation:

  ```bash
  . /mounted_home/src/simpler/.venv/bin/activate
  BUILD_TRACR=ON PYPTO_RUN_SAMPLE_ID=0 \
    python /mounted_home/src/simpler/examples/workers/l3/allreduce/main.py -p a2a3sim -d 0-1
  ```
- **ptoas v0.51+ moved the binary** from `$PTOAS_ROOT/ptoas` to `$PTOAS_ROOT/bin/ptoas`. pypto main
  absorbed this in `0d4d9b09` (2026-07-30) via `_ptoas_locate.py` (`PTOAS_RELATIVE_PATHS = ("ptoas",
  "bin/ptoas")`, launcher-first). Any pypto branch **older than that commit** checks only the first path
  and does **not** fall back to PATH when `PTOAS_ROOT` is set-but-invalid → `skip_ptoas=True` silently →
  "This is a compile-only artifact … cannot be executed". The symptom impersonates a broken container, but
  `main` works on the *same* container. Fix: merge main (done). Workaround for a stale branch:
  `unset PTOAS_ROOT` (the image's PATH already has `bin/`); do **not** point `PTOAS_ROOT` at `bin/` on a
  pre-0.51 install — `<root>/ptoas` is a launcher that sets `LD_LIBRARY_PATH`, and bypassing it fails on
  `libMLIR*.so`. Then delete the cached `skip_ptoas` artifact under `pypto-lib/build_output/` or it keeps
  failing.
- **A failed onboard run poisons the device; its own finalize force-resets it.** An AICore fault
  (`VEC instruction error: the ub address out of bounds` → `507018`) reproduced once and then passed 10/10.
  The failing run logged `bounded device drain failed … force reset will follow in finalize`, so the next
  run got a clean card. Treat a one-off onboard AICore fault as *possibly* stale device state — re-run
  before investigating, but do not call it fixed without repeats.
- **`ASCEND_PROCESS_LOG_PATH` is an output path only** — nothing in `src/`, `python/`, or `simpler_setup/`
  reads it. It cannot change execution; it only decides where the driver writes the device log. Still set
  it before any onboard run you expect to fail: the harness does not, so otherwise the evidence lands in
  the shared `~/ascend/log/debug/` and is unattributable.
- **AICore fault triage is documented upstream:** `docs/troubleshooting/device-error-codes/aicore-fault.md`
  in simpler. Its worked example *is* these allreduce collectives — F2 (static UB budget) cleared the
  kernels (#1489, ≤132 KiB of 192 KiB UB at every rank count), and the real cause was corrupted dispatch
  state (#1477, whose fix `ae2611d8` is already in the tree).

---

## Build & run recipe

Generalized from `~/Documents/Things_I_learned.md` (which uses `/mounted_home/src/...`; on this host the
repos are under `~/src/...`). NPU device index is `-d <N>`; platform `-p a2a3`.

```bash
# 1. Build simpler runtime with TracR, then install pypto dev build
cd ~/src/pypto/runtime && rm -rf build && BUILD_TRACR=ON pip install --no-build-isolation -e '.[test]'
cd ~/src/pypto        && rm -rf build && pip install --no-build-isolation -e .

# 2a. Single-device model (models live in pypto-lib; note the flat qwen3_14b/ path)
cd ~/src/pypto-lib
BUILD_TRACR=ON PYTHONPATH="$PWD" PYPTO_RUN_SAMPLE_ID=0 \
    python3 models/qwen3_14b/decode_fwd.py -p a2a3 -d <dev>

# 2b. L3 multi-device (2..16 chips; sim first, then onboard). Other runnable L3 examples with the
#     same -p/-d interface: ffn_tp_parallel, ep_dispatch_combine, dual_domain_overlap (-d 0-2),
#     multi_chip_dispatch, domain_rank_map (-d 0-2).
cd ~/src/pypto/runtime
BUILD_TRACR=ON PYPTO_RUN_SAMPLE_ID=0 python examples/workers/l3/allreduce/main.py -p a2a3sim -d 0-1
BUILD_TRACR=ON PYPTO_RUN_SAMPLE_ID=0 python examples/workers/l3/allreduce/main.py -p a2a3   -d 0-1

# 3. Post-process traces -> perfetto.json (open at ui.perfetto.dev)
~/src/pypto/runtime/build/output/bin/tracr_process ~/ascend/tracr_0

# (keep a copy; PYPTO_RUN_SAMPLE_ID keeps repeated runs separate as tracr_<N>)
```

Notes on the recipe:

- `BUILD_TRACR=ON` is needed at **run** time too, not just for the build — `kernel_compiler.py` reads it
  via `os.getenv` to add `-DENABLE_TRACR` when it compiles kernels.
- Onboard work on this shared box goes through `task-submit --device <list> --run "..."`, and should be
  gated by `.claude/skills/onboard-arch-precheck/check.sh a2a3` first.
- Optional swimlane comparison run: `--enable-dep-gen --enable-chip-swimlane 4` (renamed from
  `--enable-l2-swimlane`). Not supported for L3 tests yet — `conftest.py` rejects it there.
