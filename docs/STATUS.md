# STATUS — TracR compiler-pass work

> Fast-moving snapshot of the working state. The durable plan is
> [06-execution-plan.md](06-execution-plan.md). Update this file freely.

**Last updated:** 2026-07-15

---

## Current repo / branch / commit state

| Repo | Branch | Commit | Notes |
| --- | --- | --- | --- |
| **pypto** | `tracr-codegen-pass` | `7fb0e4cb` (off `main` `5fd56e7f`) | Working branch for the pass. Commit pins the `runtime/` submodule to `c39b1f03`. |
| ↳ pypto `runtime/` submodule | `tracr` | `c39b1f03` | = simpler tracr HEAD. (`main` had recorded `438d5cb1`.) |
| **simpler** | `tracr` | `c39b1f03` | Tracks `upstream/tracr`. Reference impl + runtime plumbing. Latest: "adding one tracr marker back (DLL_loading)". |
| **tracr** | `main` | `916b0fd` | Unchanged; postprocessing deltas deferred to M2/M3. |
| **tracr-compiler-project** | `main` | rebased onto `68ca7b9` | This repo. `06`/`STATUS` replayed on top of the context-doc set. |

ℹ️ Docs 00–05 + `docs/README.md` are now present (brought in by `68ca7b9`, "docs: add TracR profiling &
compiler-instrumentation context set"); `06`/`STATUS` were rebased on top. `origin`
(`github.com/georgebisbas/tracr-compiler-project`) still isn't fetchable/pushable from this host (no SSH
key) — arrange a key or push from the system that has one.

---

## Done

- **M0** — TracR-in-simpler baseline runs end-to-end (hand-written markers).
- Created `pypto:tracr-codegen-pass` off `main`; pinned `runtime/` submodule to simpler tracr `c39b1f03`.

## In progress / next action

- **M1** — implement the pass to auto-emit the Coarse orchestration markers (see
  [06-execution-plan.md](06-execution-plan.md) §5). Three edits: `ProfilingLevel` on `RunConfig`;
  `emitProfilePush/Pop` at `EmitIndentedLine()`; wire around the `RuntimeScopeStmt` emission + entry phases.
- **Immediate step:** read-only map of `orchestration_codegen.cpp` to pin the exact scope-loop emission
  site and the entry point for phase markers, and trace how a `RunConfig` flag reaches codegen.

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
