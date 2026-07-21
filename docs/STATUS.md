# STATUS — TracR compiler-pass work

> Fast-moving snapshot of the working state. The durable plan is
> [06-execution-plan.md](06-execution-plan.md). Update this file freely.

**Last updated:** 2026-07-15

---

## Current repo / branch / commit state

| Repo | Branch | Commit | Notes |
| --- | --- | --- | --- |
| **pypto** | `tracr-codegen-pass` | `7fb0e4cb` + uncommitted M1 | Working branch for the pass; pushed to fork [`github.com/noabauma/pypto`](https://github.com/noabauma/pypto/tree/tracr-codegen-pass). **M1 codegen change lives here, uncommitted.** `runtime/` submodule bumped to `9cb023c3`. |
| ↳ pypto `runtime/` submodule | `tracr` | `9cb023c3` | = simpler tracr HEAD. |
| **simpler** | `tracr` | `9cb023c3` | Tracks `upstream/tracr`. Reference impl + runtime plumbing. Latest: "style: clang-format TraCR changes". |
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
  generated-orchestration lane. Change is uncommitted in `~/src/pypto`.

## In progress / next action

- **Commit M1** on `tracr-codegen-pass` (codegen change + golden updates). No AI co-author line (pypto rule).
- **Cleanup:** the hand-written manual markers in
  `examples/a2a3/.../paged_attention/kernels/orchestration/paged_attention_orch.cpp` are now redundant
  for pass-covered (generated) paths and are being removed (simpler working tree). **Keep** the runtime
  plumbing markers (`aicpu_executor.cpp`, `scheduler_*.cpp`) — the pass depends on them.
- **Next milestone — M2:** copy + comm cost classes + flows + barrier-anchored multi-node sync (the L3
  deliverable). Optional M1 polish first: per-iteration `extraId` (runtime loop index vs static scope id).

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
