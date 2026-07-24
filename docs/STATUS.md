# STATUS — TracR compiler-pass work

> Fast-moving snapshot of the working state. The durable plan is
> [06-execution-plan.md](06-execution-plan.md). Update this file freely.

**Last updated:** 2026-07-24

---

## Current repo / branch / commit state

| Repo | Branch | Commit | Notes |
| --- | --- | --- | --- |
| **pypto** | `tracr-codegen-pass` | `616a86b7` ("M1 done") | Working branch for the pass — **committed & pushed** to the fork: [github.com/noabauma/pypto/tree/tracr-codegen-pass](https://github.com/noabauma/pypto/tree/tracr-codegen-pass). Push must use **SSH** (the `gh`/HTTPS token lacks `workflow` scope and the branch history touches `.github/workflows/`). `runtime/` submodule at `9cb023c3`. |
| ↳ pypto `runtime/` submodule | `tracr` | `9cb023c3` | = simpler tracr HEAD. |
| **simpler** | `tracr_l3` (off `tracr`) | M2 step-1 | PyTraCR L3 host-scheduling instrumentation; loaded into `~/src/pypto/runtime`. Baseline `tracr` (`9cb023c3`) = the runtime plumbing it builds on. |
| **tracr** | `main` | `916b0fd` | Unchanged; postprocessing deltas deferred to M2/M3. |
| **tracr-compiler-project** | `main` | rebased onto `68ca7b9` | This repo. `06`/`STATUS` replayed on top of the context-doc set. |

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

## In progress / next action — M2 (L3 multi-device)

Compute = M1 (done); L3 host **scheduling** = M2 step 1 (done, sim). Remaining pieces, per docs 05/06:

- **Step 2 — per-chip lanes: IMPLEMENTED (pending sim revalidation).** The two transient dispatch bodies
  (`worker.py` `_ensure_comm_base` comm-init, `_dispatch_control_domain` alloc/release) now
  `THREAD_INIT`/`FINALIZE` and emit `CommInit` / `AllocDomainChip` / `ReleaseDomainChip` on per-chip lanes
  (chip `i` → lane `i+1`; helper gained `thread_init`/`mark_set_chip`/… + `Chip_i` channel names). Python
  -only → no rebuild (editable serves from source); rerun the allreduce sim example to confirm.
- **Step 3 — flows (host → device):** `FLOW_START` at `submit_next_level` (host) → `FLOW_END` at the
  chip's device execution, sharing a globally-unique `flowId` (cross-boundary plumbing). Causal arrows
  from an orchestrator decision to the device work it triggers.
- **Copy cost class (M2a):** H2D/D2H markers in the runtime
  (`common/platform/*/host/device_runner_base.cpp`, `extra`=bytes). C++ runtime, single-node.
- **Comm cost class (M2b):** collective / remote-read-write markers on the AICPU scheduler TGET/TPUT path
  (`scheduler_dispatch.cpp`, `scheduler_cold_path.cpp`). C++ runtime — the actual data movement behind a
  collective.
- **Onboard multi-clock sync (M2c):** host CPU counter ≠ device AICPU counter → recorded host↔device
  offset / barrier-anchored `sync_start`. Needed for onboard multi-device (sim already aligned).

---

## Build & run recipe

Generalized from `~/Documents/Things_I_learned.md` (which uses `/mounted_home/src/...`; on this host the
repos are under `~/src/...`). NPU device index is `-d <N>`; platform `-p a2a3`.

```bash
# 1. Build simpler runtime with TracR, then install pypto dev build
cd ~/src/pypto/runtime && rm -rf build && BUILD_TRACR=ON pip install --no-build-isolation -e '.[test]'
cd ~/src/pypto        && rm -rf build && pip install --no-build-isolation -e .

# 2. Run a model (models live in pypto-lib)
cd ~/src/pypto-lib
BUILD_TRACR=ON PYPTO_RUN_SAMPLE_ID=0 python3 models/qwen3/14b/decode_fwd.py -p a2a3 -d <dev>

# 3. Post-process traces -> perfetto.json (open at ui.perfetto.dev)
~/src/pypto/runtime/build/output/bin/tracr_process ~/ascend/tracr_0

# (keep a copy; PYPTO_RUN_SAMPLE_ID keeps repeated runs separate as tracr_<N>)
```

Optional L2-swimlane comparison run: add `--enable-dep-gen --enable-l2-swimlane 4` to a runtime test.
