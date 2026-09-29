# STATUS — TracR compiler-pass work

> Current state only. Durable plan: [06-execution-plan.md](06-execution-plan.md).
> As-built record of the comm markers: [08-codegen-comm-markers-plan.md](08-codegen-comm-markers-plan.md).
> History lives in git; this file is rewritten, not appended to.

**Last updated:** 2026-09-29

---

## Where the work is

| Repo | Branch | Commit | vs `origin/main` |
| --- | --- | --- | --- |
| **pypto** | `tracr-codegen-pass` | `cd389845` ("C5 done") | **22 ahead, 115 behind**; 9 files, +796/−9 |
| ↳ `runtime/` submodule | `tracr_l3` | pin == simpler HEAD | — |
| **simpler** | `tracr_l3` | `90de9ee50` ("removing host side tracing") | **90 ahead, 183 behind**; 90 files, +2805/−64 |
| **tracr** | `main` | `916b0fd` | unchanged — no TracR-side change was needed |

⚠️ **`~/src/simpler` is on branch `tracr` and does NOT carry this work.** Use the worktree
**`~/src/simpler-tracr_l3`** (holds the branch), or the read-only clone at `~/src/pypto/runtime`.

`origin` for this doc repo (`github.com/georgebisbas/tracr-compiler-project`) is not fetchable from this
host (no SSH key).

---

## Done — the goal is met

**PyPTO's compiler emits every TracR communication marker. No user hand-placement anywhere, and
device-to-device arrows render on real silicon.**

- **M0–M1** — TracR-in-simpler baseline; the pass auto-emits `PTO2_SCOPE_` spans in generated
  orchestration (`616a86b7`), verified on the qwen3-14b decode oneliner.
- **M2** — L3 host lane via PyTraCR, copy-cost lanes (M2a), barrier-timing spike (M2b).
  **The host lane was retired on 2026-09-29** (see below), which supersedes M2c.
- **D1–D3** — TracR lanes on the AICore. Full record: [07](07-aicore-tracing-direction.md).
  The reframe that unlocked it: a `.bts` file is a flat array of 16-byte `Payload` structs, so the AICore
  only has to *write records* — TracR never has to run under CCEC.
- **C0–C5** — codegen-emitted comm markers and arrows. Full record:
  [08](08-codegen-comm-markers-plan.md).

### Onboard results (a2a3, 4 ranks on devices 4–7; `04_barrier` takes exactly 2)

| example | CommNotify / CommWait | flow ids | matched |
| --- | --- | --- | --- |
| `04_barrier` (2 ranks) | 2 / 2 | 2 | **2/2** |
| `08_allreduce_mesh` | 12 / 12 | 12 | **12/12** |
| `09_allreduce_two_phase` | 24 / 24 | 24 | **24/24** |
| `12_broadcast` | 12 / 12 | 12 | **12/12** |
| `13_allgather` | 12 / 12 | 12 | **12/12** |
| `14_reduce_scatter` | 12 / 12 | 12 | **12/12** |
| `15_all_to_all` | 12 / 12 | 12 | **12/12** |
| `10_allreduce_ring` | 24 / 24 | 8 | 0/8 — tails only, by design |

`08` yields the complete mesh: every ordered pair (a≠b) exactly once, each arrow starting on the sender's
device proc and ending on the receiver's. `10_allreduce_ring` is the honest negative — its wait uses
`offsets=[rs_round, left]`, two non-constant components, so the head rule declines rather than guessing
which is the rank. Covering it needs a *runtime* seq (its slot coordinate is a round index).

Traces in both formats: `~/tmp/tracr_110*_<example>/` (`perfetto.json` + `tracr.prv`/`.pcf`/`.row`).

### Arrow coverage (AST classifier mirroring the implemented rules, 71 files)

| corpus | notify → tail | wait → head |
| --- | --- | --- |
| `pypto/examples/distributed` | 15/15 (100%) | 8/14 (57%) |
| `pypto/tests/st` | 28/28 (100%) | 9/26 (34%) |
| `pypto-ccfusion` | 9/12 (75%) | 10/12 (83%) |
| **`pypto-lib` production models** | **103/103 (100%)** | **78/80 (97%)** |
| **total** | **155/158 (98%)** | **105/132 (79%)** |

The low test-suite numbers are honest refusals: `offsets=[0, 0]` is an aggregate one-cell signal whose
sender is genuinely unrecoverable. The production corpus is at **97%**.

### Host lane retired (2026-09-29)

`L3_Orchestrator` + `Chip_*` started ~20.1 s before the first device event and spanned 1.0 s, against
~10 ms of device work — merged, device activity was **0.05%** of the timeline. `_tracr.start()` /
`_tracr.end()` removed from `worker.py` (`90de9ee50`); every other `_tracr.*` call is inert by design, so
no host proc is produced. This also drops the 4 host→device arrows and makes M2c (onboard host↔device
clock sync) moot. Call sites, `_tracr_l3.py` and the `CallConfig.flow_id` wire field were left in place —
re-enabling is two lines.

---

## Open

1. **Merge `main` into both branches.** The only thing between here and a PR. pypto is 115 behind,
   simpler 183 behind. See "Is it PR-able" below.
2. **Ring coverage** — needs a runtime `seq` rather than the compile-time slot constants C5 uses.
3. **C4b static blind spot** — `peer=my_rank` where `my_rank` is a *kernel parameter* rather than
   `pld.rank(ctx)` is not detected (`tests/st/distributed/test_l3_self_notify_credit_reset.py`). The
   runtime `src == dst` guard catches it; the compile-time rule does not.
4. **Cross-device clock skew** — arrows can render backwards in time between device lanes. Separate from
   C5, which fixes id collision, not alignment.
5. **Pre-existing, not ours:** simpler `tests/ut` segfaults in `_tracr_l3.py:129 mark_set` on a
   TracR-enabled build (proved by A/B against `HEAD~1`); now unreachable since the host lane is off, but
   the PyTraCR bug is still there. pypto
   `test_orchestration_codegen_graph.py::test_generated_orchestration_compiles_against_the_pinned_runtime`
   fails on `tracr/tracr.hpp: No such file` — dates to `616a86b7`.

---

## Is it PR-able?

**pypto — yes, easily.** 9 files, +796/−9, and 390 of those additions are tests. The non-test surface is
6 files, all codegen. Merge `main` first (115 behind). One line is a general bug fix that could go
separately: `python/bindings/modules/ir.cpp` adds `.none()` to `start_offset` (nanobind 3.0.1 needs it to
accept an explicit `None`) — check whether main has absorbed it.

**simpler — split it.** 90 files / +2805 against main is one PR too many things. The natural seam is the
branch structure: `tracr_l3` is 33 commits on top of `tracr`, and `tracr` is the older PR #1173 lineage
(TracR vendoring, markers, host API). Suggested split:

1. **TracR vendoring + marker plumbing** — whatever of `tracr` is not already upstream.
2. **AICore TracR lane (D1–D3)** — `src/common/platform/include/aicore/tracr_aicore_*.h`, the per-arch
   kernel entries, the host readback. 12 files, +657.
3. **Generated-kernel buffer path (C2)** — `GlobalContext::tracr_aicore_slice`, the AICPU publish, the
   four `scheduler_cold_path.cpp`, `kernel_compiler.py` include dirs. Touches a host↔device wire struct,
   so it wants its own review.

Each is independently testable and none needs the others to compile.

---

## Build & run recipe

```bash
# simpler: build the worktree that holds tracr_l3 (NOT ~/src/simpler)
cd ~/src/simpler-tracr_l3
git submodule update --init tools/tracr          # PyTraCR source, else cmake fails
python3 -m venv --system-site-packages .venv && source .venv/bin/activate
pip install pybind11                             # BUILD_TRACR needs it
PYTHONNOUSERSITE=1 BUILD_TRACR=ON pip install --no-build-isolation -e .

# pypto: build, then INSTALL the extension (cmake alone is not enough — see traps)
cd ~/src/pypto && cmake --build build --parallel 8
cp build/python/bindings/pypto_core*.so ~/.local/lib/python3.12/site-packages/pypto/

# run a collective, 4 ranks (BUILD_TRACR is needed at RUN time too)
BUILD_TRACR=ON PYPTO_RUN_SAMPLE_ID=1 \
  ~/src/simpler-tracr_l3/.venv/bin/python examples/distributed/08_allreduce_mesh.py -p a2a3 -d 4,5,6,7

# post-process — perfetto, and paraver (which needs state.cfg in the CWD)
~/src/simpler/build/output/bin/tracr_process ~/ascend/tracr_1 perfetto
cd ~/src/simpler-tracr_l3/tools/tracr/postprocessing/paraver
~/src/simpler/build/output/bin/tracr_process ~/ascend/tracr_1 paraver
```

Everything runs inside `pypto3-hw-native-sys:cann9` (pypto/device work) or `simpler-cann9` (simpler
builds); `$HOME` mounts at `/mounted_home` and the user is root.

---

## Environment traps (each cost a debugging session)

- **`BUILD_TRACR=ON` is needed at build *and* run time.** `CMakeLists.txt` reads it at configure time;
  `kernel_compiler.py` reads it via `os.getenv` to add `-DENABLE_TRACR` when compiling kernels. Miss
  either and the run completes normally and writes nothing.
- **A simpler *worktree* build silently compiles the OTHER checkout's sources.** `--system-site-packages`
  lets `~/.local`'s editable install (pointing at `~/src/simpler`) win, so `PROJECT_ROOT` resolves to the
  wrong tree. Nothing errors — only a feature vanishes. Detect:
  `strings build/lib/a2a3/onboard/host_build_graph/libhost_runtime.so | grep -oE "/.*/src/simpler[a-z0-9_-]*" | sort | uniq -c`.
  A "full" runtime build finishing in ~1 minute is another tell. Fix: `PYTHONNOUSERSITE=1` **and**
  `rm -rf build`.
- **`cmake --build build` does not update the pypto module Python imports.** The editable install keeps
  its own copy under `~/.local`. Check with `python -c "import pypto.pypto_core as c; print(c.__file__)"`.
- **Paraver output needs `state.cfg` in the process CWD** — `tracr_process.cpp:378` uses a relative path,
  so `cd <trace> && tracr_process . paraver` fails and writes no `.prv`. Worth fixing in TracR.
- **`git worktree move` refuses on a worktree with submodules**, so renaming one is remove + re-add — and
  a rebuild, since the venv, the editable `.pth` and `build/cache` all bake absolute paths.
- **An out-of-line `[aicore]` function in a generated kernel silently produces wrong results**, even if
  never called; `elf_parser.py` only checks `kernel_entry` is at `.text` offset 0, which it still is. Fix
  is `always_inline`. Mechanism never explained.
- **ptoas re-declares every `func.func private` as `static __aicore__ void`**, not `extern "C"`.
- **Poisoned device state:** a killed onboard run leaves its devices failing with `507901` /
  `hdc disconnect`. Re-run a one-off failure before investigating it.
