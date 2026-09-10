# 08 — Plan: PyPTO auto-generates comm markers and D2D arrows

**Opened:** 2026-08-28. Realises [06](06-execution-plan.md) M1/M3 for the **communication** cost class,
on the runtime substrate [07](07-aicore-tracing-direction.md) proved on silicon.

Goal: **a user writes no markers.** Compile any pypto-lib model and its cross-device communication
appears in the trace as spans and arrows.

---

## 1. Why codegen and not the runtime

The obvious idea — instrument simpler's comm layer once, get arrows everywhere — does not work, and it is
worth recording why so nobody re-proposes it.

pypto-lib models express communication as `pld.system.notify` / `pld.system.wait` (55 and 35 call sites in
`deepseek_v4_pro` alone). PyPTO's codegen lowers those **straight to the PTO ISA** — `pld.system.notify`
is documented as *"Lowers to inline peer-offset arithmetic + addptr + make_tensor_view + partition_view +
TNOTIFY at codegen"* (`src/ir/op/distributed/system.cpp:214`). They never pass through simpler's
`send_notification()`.

Simpler owns the *window and CommContext setup* (`pld.window`, `pld.alloc_window_buffer` — 396 uses each);
it does not own the notify/wait instructions. The one exception proves it: `pld.system.defer_wait` hard
-requires a simpler runtime (`#error "pld.system.defer_wait requires a Simpler runtime that provides
pto_async_kernel_api.h"`), and it is 4 call sites out of ~90.

So generated kernels need generated markers. Hand-written kernels (32 files across `examples/` and
`tests/st/worker/collectives/`) are a **separate, second** effort against simpler's wrappers — out of
scope here.

## 2. What the corpus actually looks like

**Surveyed 2026-08-28: 316 `pld.system.notify` / `pld.system.wait` call sites** across pypto (138),
pypto-serving (90), pypto-lib (64) and pypto-ccfusion (24). Classifier:
`docs/tools/classify_comm_sites.py`.

| | share | outcome |
| --- | --- | --- |
| One rank-valued offset axis | **190 (60%)** | **Full arrows** |
| All-constant offsets (aggregate cell) | **102 (32%)** | Span only — permanently, see §9 |
| Task-indexed / expert-major / computed / no offsets | 24 (8%) | Bail to span |

60% is the target. The other 40% degrade to spans, which is still strictly more than exists today.

### The op asymmetry

```
pld.system.notify   target, peer, offsets, value      <- peer is an EXPLICIT operand
pld.system.wait     signal, offsets, expected         <- no peer; encoded in offsets
```

At the notify lowering site (`src/backend/common/pto_ops_distributed.cpp:454`), `op->args_[1]` **is** the
peer — already passed to `EmitCommRemoteView(binding, op->args_[1], codegen)`. Tails are free.

### Heads: classify the *signal*, not the call site

A positional rule (`offsets[0]` is the rank) was the obvious guess and it is **wrong**. Two real
counterexamples:

```python
# rank axis is SECOND  (pypto tests/st/distributed/collectives/test_l3_allreduce_ring.py)
signal: pld.DistributedTensor[[total_rounds, n_ranks]]     offsets=[rs_round, my_rank]

# rank axis EXISTS but is indexed by a constant  (pypto-lib .../prefill_kv_allgather.py)
gather_signal: pld.DistributedTensor[[TP_SIZE, 1]]         offsets=[0, 0]
```

The rule that survives both is **shape-driven**, and it classifies a *signal tensor*, not an individual
call:

1. Find the signal axis whose extent is `world_size` / `n_ranks` / `TP_SIZE`.
2. If every notify/wait on that signal indexes that axis with a **rank-valued variable**
   (`my_rank` on notify, the loop variable on wait) → **arrow-able**; the remaining offset components
   are `seq`.
3. If that axis is indexed by a **constant** → **aggregate**; span only.
4. Anything else → bail to span.

### Two more discriminators, from reading `alltoallv_gmm.py`

That file was the one most likely to break the model, and it does — in two ways the offsets analysis alone
does not catch:

```python
# 1. NO rank axis exists.  signal[[LOCAL_EXPERTS, max_m_tiles]] is expert-major,
#    and `expected` is a count of contributions, not a single message.
notify(target=recv_tile_done, peer=destination, offsets=[expert, (destination_slot + row_delta) // M_TILE])
wait  (signal=recv_tile_done,                   offsets=[expert, m_block], expected=expected_chunks)

# 2. SELF-notify: peer is the local rank. This is intra-chip task ordering, not D2D.
notify(target=tile_done, peer=my_rank, offsets=[task, 0])
wait  (signal=tile_done,               offsets=[task, 0], expected=1)
```

Corpus-wide: **21 self-notifies** (`peer=my_rank` ×16, `peer=self_rank` ×5) and **73 waits with
`expected != 1`**.

- **`peer` is the local rank → emit no tail at all.** Unambiguous, and it must be checked *before* the
  offsets analysis: these sites often have perfectly well-formed offsets and would otherwise produce a
  tail with no possible head. The same shape appears in PyPTO's builtin allgather template, whose
  self-clearing epilogue does `TNOTIFY(self_sig, -1)` on local addresses.
- **`expected != 1` is a hint, not a verdict.** It means several contributions land in one cell — usually
  aggregate, but a wait for the k-th message from *one* named peer is still arrow-able with `seq = k`.
  Flag for inspection; do not auto-classify.

**Classify per signal, not per call.** A notify whose peer is explicit is always arrow-*tail*-able, but
emitting a tail whose matching wait is aggregate produces an unmatched endpoint — worse than no arrow.
The decision must be made once for the signal and applied to both ends.

Step 2 also yields `seq` for free in both observed layouts: `[world_size, M_TILES]` gives the chunk index,
`[total_rounds, n_ranks]` gives the round.

## 3. Phases

Listed in **execution order**. Labels are kept stable from the original plan, so C2 precedes C1.

| # | Phase | Deliverable | Exit criterion |
| --- | --- | --- | --- |
| **C0** | **Submodule bump** | Point pypto `tracr-codegen-pass` at simpler `tracr_l3` | The branch compiles against the AICore emitter and the restored marker ids |
| **C2** | **Buffer injection** | `InjectTracrBuffer` pass, modelled on `InjectGMPipeBuffer`. Includes **run-scoped reset** (below). | Buffer reaches every instrumented kernel with no example-level plumbing, and survives multiple launches |
| **C1** | **Spans, no arrows** | Wrap each lowered `notify` / `wait` in a marker pair, via an `extern "C"` call declared in the `.pto` (§5.1). No peer analysis. | A compiled model shows comm spans on the AICore lane; zero user markers |
| **C3** | **Arrow tails** | `FLOW_START` at notify, using `op->args_[1]` | Tails present with correct `(src,dst)`; heads still absent |
| **C4** | **Signal classification + arrow heads** | Per-signal rank-axis analysis (§2), three-way: arrow-able / aggregate / bail. Applied to both ends of a signal. | Arrows pair in a compiled model; aggregate and irregular signals emit spans with no unmatched endpoints |
| **C5** | **`seq`** | Derive from `value` / `expected` | Multi-round and ring collectives pair correctly |

**C2 before C1, revised 2026-09-10.** The original order put spans first on the reasoning that they need
no analysis. They also need a buffer, and there is none to borrow: `CommContext` is fully packed (1056 B,
`comm_layout.h` pins every offset) and byte-mirrored in pto-isa as `HcclDeviceContext`, and its
`workSpace` field is a live SDMA/URMA allocation. So C1's exit criterion depends on C2's parameter.

**Run-scoped reset is part of C2, not a detail.** `tracr_aicore_reset` currently runs at kernel entry and
zeroes the count header. That is correct for a hand-written kernel launched once, and wrong for a compiled
model: every launch would wipe the previous launch's records and only the last would survive. Reset must
become once per run. It is not a one-line move, because the trace buffer is an `OUTPUT_EXISTING` tensor
whose host-side zeros are never staged to the device (the D1 finding, [07](07-aicore-tracing-direction.md)
§4).

**C1 still before C3.** Spans prove the whole emit-compile-render path on generated code. If C1 does not
render, nothing after it matters.

### C0 is done, and its blocker cleared itself

**Resolved 2026-09-10.** C0 landed as the `main -> tracr-codegen-pass` merge `9de7d2ae`. The only
conflict was the `runtime` submodule pointer, resolved to simpler `tracr_l3` (`6a48fa38`); main's side
carries no TracR work, and `tracr_l3` is the only branch with the AICore emitter.

**The `TaskTensor` gate is gone.** pypto migrated its emitted code to `Tensor`, so generated
orchestration now matches what `tracr_l3` provides. Zero `TaskTensor` string literals remain. This was
the external dependency blocking the C-phase end-to-end exit criteria, and it needed no work from us.

Checked after resolving, because a clean textual merge is not a working one:

- `comm_layout.h`'s `static_assert`s on `::CommContext` offsets still parse against `tracr_l3`. This is
  the **only** hard compile coupling between pypto's C++ and the runtime submodule.
- Generated orchestration's includes resolve. `orchestration_api.h` and `tensor.h` both exist, and the
  codegen emits neither the old `pto_orchestration_api.h` nor `TaskTensor`.
- M1's marker emission survived at both `PTO2_SCOPE_` sites.

**Debt carried, not blocking.** `tracr_l3` is 63 commits behind simpler main and does not contain
`39ce891d`, the commit pypto main pins. A `main -> tracr_l3` merge is still owed on the simpler side; a
dry run shows **16 conflicts**, concentrated in the usual TraCR re-weave surface (the a2a3/a5 aicpu
executors, the schedulers, and all four `device_runner.cpp`). Nothing in the C phases depends on closing
it, but it should not be left to grow, and the traps are already catalogued in the tracr merge workflow.

## 4. C2 in detail — the buffer, and why it is the cheap part here

Hand-written kernels pay ~120 lines of plumbing per kernel (extra `ChipTensor`, `add_output`,
`expected_arg_count`, both callable signatures). **Codegen pays none of it**, because PyPTO emits the
kernel *and* the orchestration, so it can inject the buffer on both sides.

`InjectGMPipeBuffer` (`src/ir/transforms/inject_gm_pipe_buffer_pass.cpp`) is an exact precedent:

```cpp
auto new_params = func->params_;
new_params.push_back(gm_var);                    // line 175-176
new_directions.push_back(ParamDirection::Out);   // line 178
result->params_ = new_params;
...
new_call->args_.push_back(gm_param);             // line 199 — propagate to callers
```

Its docstring describes exactly the shape needed: *"Adds a fresh `__gm_pipe_buffer` Out-tensor parameter
to each, propagating the parameter upward through callers except Orchestration functions, which instead
get a per-call-site placeholder `tensor.create`."*

`InjectTracrBuffer` mirrors it: find functions containing `pld.system.notify` / `pld.system.wait`, add a
`__tracr_buffer` Out-tensor param, propagate upward, orchestration gets the `tensor.create`. Sizing is
codegen's, exactly as the pipe buffer's is.

**This also removes the need for D2b's deferred runtime-allocation work on the codegen path.** That work
remains relevant only for hand-written kernels.

## 5. Emission shape

M1 already emits markers into generated **orchestration** and its approach carries over
(`src/codegen/orchestration/orchestration_codegen.cpp:146-151`):

```cpp
// Emitted unconditionally: the macros expand to zero-cost no-ops unless the
// orchestration unit is compiled with -DENABLE_TRACR (BUILD_TRACR=ON) ...
oss << "#include <tracr/tracr.hpp>\n";
oss << "#include <tracr_simpler_markers.hpp>\n\n";
```

The AICore path cannot use TraCR's macros — that is the whole finding of [07](07-aicore-tracing-direction.md).
It uses `aicore/tracr_aicore_emit.h` instead, which already provides the primitives C1–C5 need:
`tracr_aicore_mark_set` / `_mark_reset` / `_flow_start` / `_flow_end` / `_flow_id`.

### 5.1 How a C++ marker call reaches an op lowered to PTO IR

**Established 2026-09-10, after a wrong turn worth recording.**

Comm ops do not lower to C++ in PyPTO. `MakeNotifyCodegenPTO` / `MakeWaitCodegenPTO`
(`src/backend/common/pto_ops_distributed.cpp:454`, `:518`) emit **PTO/MLIR text**:

```cpp
codegen.Emit("pto.comm.tnotify(" + partition_view + ", " + value_ssa + " ...");
```

That file is the *only* emitter for `pld.system.notify` / `pld.system.wait`. The full pipeline is:

```
PyPTO ──emits──> ptoas/*.pto ──ptoas 0.60──> ptoas/*.cpp ──PyPTO prepends prologue──> kernels/aiv/*.cpp
```

`kernels/aiv/*.cpp` is ptoas's output **verbatim**, under a prologue PyPTO writes (includes, `__gm__` /
`__aicore__` defines, the `__CPU_SIM` cache-op shims). Verified by diffing the two against a real
distributed build (`/tmp/build_output/L3AllGatherGemm_*`), where the comm ops appear as real C++:

```cpp
pto::comm::TNOTIFY(v19, v5, v4);
pto::comm::TWAIT(v23, v5, v3);
```

**The wrong turn.** This looked like a blocker: a marker needs a timestamp, the timestamp comes from
`get_sys_cnt_aicore()`, that is a C++ SPR read, and the PTO dialect has **no counter-read op** (confirmed —
`pto.total_cycles` is a cost-model field in `pto/costmodel/perf_sim/pipe_model.hpp`, not an ISA op, and
`ptoas` is a closed binary shipped as a wheel at `/opt/ptoas-bin`, so the dialect cannot be extended from
here). Each of those facts is true and the conclusion does not follow.

**The IR never needs to read the counter. It only needs to *call* the marker.** The counter read happens
inside the callee, in ordinary C++ compiled by ccec — exactly as in the D1–D3 hand-written kernels.

And a call is expressible, because **ptoas passes an externally-declared function straight through**. A
declaration-only `func.func private` plus a `func.call`:

```mlir
func.func private @tracr_mark(!pto.ptr<i64>, i32, i32, i32) -> ()
...
func.call @tracr_mark(%arg0, %c0_i32, %c7_i32, %c1_i32) : (!pto.ptr<i64>, i32, i32, i32) -> ()
```

compiles under `ptoas probe.pto -o probe.cpp --enable-insert-sync --pto-level=level3` to:

```cpp
extern "C" AICORE void tracr_mark(__gm__ int64_t*, int32_t, int32_t, int32_t);
...
  #if defined(__DAV_VEC__)
  tracr_mark(v1, v2, v4, v3);
  #endif // __DAV_VEC__
```

ptoas **synthesizes the `extern "C" AICORE` declaration itself**, and places the call inside the
correct core-kind guard. Since PyPTO owns the prologue, the matching definition is injected there and
lands in the same translation unit, so it still inlines. Reusable probe (declaration, call, definition,
decode) with its two build traps: [`docs/tools/ptoas_extern_probe/`](tools/ptoas_extern_probe/). The
emitted record decoded to `0x100070000` = channel 0, event 7, extra 1.

**Consequences for the plan:**

- No dialect change, no PTOAS/ISA-team dependency. C1–C5 stay inside PyPTO.
- The marker signature must be **`extern "C"`** and match the ptoas-emitted declaration exactly — no
  overloads, no default arguments. `tracr_aicore_emit.h`'s current C++-linkage inline functions need a
  thin `extern "C"` shim per marker kind.
- The buffer arrives as a `!pto.ptr<i64>` kernel parameter, which is what **C2** delivers. C1's exit
  criterion ("a compiled model shows comm spans") therefore depends on C2's parameter injection; the
  phases are more entangled than §3 implies.

**Verified under ccec too.** The production incore flags
(`--cce-aicore-only --cce-aicore-arch=dav-c220-vec -mllvm -cce-aicore-addr-transform -DMEMORY_BASE`,
`toolchain.py:160`) compile an `extern "C" [aicore]` function taking `__gm__ int64_t*`, with a
redundant re-declaration matching ptoas's, and emit the symbol:

```
0000000000000000 T tracr_mark
```

`get_sys_cnt()` resolves as a ccec builtin inside it with no include — so the timestamp read needs
nothing from simpler's headers on this path.

**Two gaps to close before C1**, both in simpler on `tracr_l3` — ✅ **DONE 2026-08-28, commit `631e0dd9`**:

1. **The header has no off-switch.** M1's "emit unconditionally, no-op without `-DENABLE_TRACR`" contract
   requires the emit functions to compile to nothing when TraCR is off, or every generated kernel pays for
   markers in production builds.
2. **`-DENABLE_TRACR` never reaches the AICore compile.** `kernel_compiler.py` adds it only in
   `_compile_orchestration_shared_lib` (line 635); `compile_incore` (line 461) passes no such flag. So the
   guard in (1) would be permanently off on the core.

Compile-time is the right mechanism — a runtime check would cost a branch per marker on the core.

**Verified onboard on 2 chips.** With TraCR on: 6 payloads / 2 spans / 2 flow endpoints per rank, 2 lanes
written. With it off: no payloads, no lanes, and the kernel compiles and runs with its marker calls
*unguarded* — which is the property codegen depends on. Both matched golden.

**Branch ordering.** (1) and (2) are simpler / `tracr_l3`; C1–C5 are pypto / `tracr-codegen-pass`. pypto's
`runtime/` submodule pins simpler, so the simpler changes land first and the pypto branch takes a
**submodule bump** before C1 can compile.

## 6. Design decisions already settled by D1–D3

Do not re-litigate these; each was verified on silicon
([07](07-aicore-tracing-direction.md) §6, D1/D2b results):

- **Channel index is resolved by name on the host**, never assumed in the kernel — `channel_names` is
  sized by the run's core count.
- **The buffer carries an explicit record count.** A zero timestamp is not a terminator.
- **Overflow drops and counts; it never wraps.** `tracr_process` requires each `.bts` pre-sorted and never
  sorts one itself.
- **Flow ids are packed, never hashed.** `tracr_process` pairs starts to ends by index within one id, so a
  collision mispairs silently.
- **The writer predicate must be the logical block index.** The hardware index mutes any chip whose task
  landed on a non-zero core — an onboard run lost an entire rank's lane to this.

## 7. Risks

1. **~40% of call sites will not produce arrows** — measured, not estimated (§2). 32% use an aggregate
   cell and can never produce them; 8% are irregular. **Accepted:** spans everywhere is still strictly
   more than exists today, and C1–C3 deliver spans and tails independently of C4. The residual risk is
   that the 8% irregular bucket hides a *pattern* rather than one-offs — `alltoallv_gmm.py` is the file
   most likely to, and has not been read line by line.
2. **Marker density in a 40-layer model.** The allreduce writes 30 payloads per rank; a full decode with
   55 notifies × layers could exhaust a fixed buffer. The drop counter makes this visible rather than
   silent, but sizing needs a real model run.
3. **Cost on the critical path.** ~0.15–0.25 µs per marker with no per-record barrier
   ([07](07-aicore-tracing-direction.md) §5). Against a 3.6 ms straggler skew that is noise; against a
   tight inner loop it may not be. `ProfilingConfig` (M3) is the escape hatch — instrument at a
   granularity the user selects.

## 8. Test strategy

- **C1:** compile one pypto-lib model with markers on; assert comm spans appear on the AICore lane. Sim is
  enough — this tests emission, not timing.
- **C2:** IR before/after test in `tests/ut/ir/transforms/` per the repo's pattern — assert the param is
  added and propagated, orchestration gets its `tensor.create`.
- **C3/C4:** onboard 2 and 4 rank runs; assert the arrow matrix is complete and no endpoint is unmatched.
  The `allreduce` example gives a known-good reference at 2/4/8 ranks.
- **C5:** `tests/st/worker/collectives/` has ring, bidirectional-ring, twophase, broadcast, all_to_all,
  allgather, reduce_scatter. Those are hand-written so they do not exercise codegen — but they are the
  right corpus for validating the **`seq` scheme** against topologies the one-notify-per-peer barrier
  does not cover.
- **Classification regression:** `docs/tools/classify_comm_sites.py` reports the arrow-able / aggregate /
  irregular split across all four repos. Run it when a model adds a collective: a new signal layout that
  lands in the irregular bucket is a silent loss of arrows otherwise. The 2026-08-28 baseline is
  190 / 102 / 24 of 316.
- **Reference workload:** `pypto-ccfusion/pypto-l3/allgather_mm.py` — both its collectives (barrier and
  chunked data publish) are arrow-able with `seq` available, so it is the natural end-to-end target.

## 9. Out of scope, permanently

Two things no amount of pass work recovers, both established in
[07](07-aicore-tracing-direction.md) §D3:

- **Pull-based data movement.** A rank reading a peer's HBM is one-sided; the peer's core executes
  nothing, so there is no second endpoint to pair.
- **Aggregate-counter waits.** If every peer increments one cell and the waiter blocks on the total, no
  per-peer identity exists at the wait. Span only.
