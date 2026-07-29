# STATUS — TracR compiler-pass work

> Fast-moving snapshot of the working state. The durable plan is
> [06-execution-plan.md](06-execution-plan.md). Update this file freely.

**Last updated:** 2026-07-29

---

## Current repo / branch / commit state

| Repo | Branch | Commit | Notes |
| --- | --- | --- | --- |
| **pypto** | `tracr-codegen-pass` | `f93a4a06` ("updating runtime") | Working branch for the pass (M1 landed at `616a86b7`; later commits merge main + bump the runtime submodule). **Committed & pushed** to the fork: [github.com/noabauma/pypto/tree/tracr-codegen-pass](https://github.com/noabauma/pypto/tree/tracr-codegen-pass). Push must use **SSH** (the `gh`/HTTPS token lacks `workflow` scope and the branch history touches `.github/workflows/`). `runtime/` submodule now bumped to the L3 branch. |
| ↳ pypto `runtime/` submodule | `tracr_l3` | `b5ee0ca5` | Moved off `tracr` → now tracks the L3 branch (= simpler `tracr_l3` HEAD, the step-3 commit). |
| **simpler** | `tracr_l3` (off `tracr`) | `b5ee0ca5` ("adding new l3 flows from host orch to dev") | M2 steps 1–3 **committed**: PyTraCR L3 host-scheduling instrumentation + per-chip lanes + host→device flows, in `~/src/pypto/runtime`. Baseline `tracr` (`9cb023c3`) = the runtime plumbing it builds on. |
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

## In progress / next action — M2 (L3 multi-device)

Compute = M1 (done); L3 host **scheduling** = M2 steps 1–3 (done, sim). Remaining pieces, per docs 05/06:

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
