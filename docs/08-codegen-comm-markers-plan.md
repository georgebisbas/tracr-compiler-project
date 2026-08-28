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

## 2. What makes this tractable

The op signatures are asymmetric, and the asymmetry is the whole design:

```
pld.system.notify   target, peer, offsets, value      <- peer is an EXPLICIT operand
pld.system.wait     signal, offsets, expected         <- no peer; encoded in offsets
```

At the notify lowering site (`src/backend/common/pto_ops_distributed.cpp:454`), `op->args_[1]` **is** the
peer — it is already passed to `EmitCommRemoteView(binding, op->args_[1], codegen)`. The arrow tail needs
no analysis at all.

The wait side needs one inference: recover the peer from `offsets`. In the slot-per-peer layout every
collective here uses, the slot index *is* the rank.

## 3. Phases

| # | Phase | Deliverable | Exit criterion |
| --- | --- | --- | --- |
| **C1** | **Spans, no arrows** | Wrap each lowered `notify` / `wait` in a marker pair. No peer analysis, no buffer sharing questions. | A compiled model shows comm spans on the AICore lane; zero user markers |
| **C2** | **Buffer injection** | `InjectTracrBuffer` pass, modelled on `InjectGMPipeBuffer` | Buffer reaches every instrumented kernel with no example-level plumbing |
| **C3** | **Arrow tails** | `FLOW_START` at notify, using `op->args_[1]` | Tails present with correct `(src,dst)`; heads still absent |
| **C4** | **Arrow heads** | Peer-from-`offsets` inference + conservative bail | Arrows pair in a compiled model; unprovable cases degrade to span-only |
| **C5** | **`seq`** | Derive from `value` / `expected` | Multi-round and ring collectives pair correctly |

**C1 before C3 deliberately.** Spans need no analysis and prove the whole emit-compile-render path on
generated code. If C1 does not render, nothing after it matters.

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

**One gap to close first:** that header has no off-switch. M1's "emit unconditionally, no-op without
`-DENABLE_TRACR`" contract requires the emit functions to compile to nothing when TraCR is off. Add the
guard before C1, or generated kernels pay for markers in production builds.

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

1. **Peer-from-`offsets` (C4) is the one unproven step.** Tractable for affine offsets over a loop
   variable; a real model may not oblige. **Mitigation:** C1–C3 deliver spans and tails without it, and
   the conservative bail degrades to exactly today's behaviour.
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

## 9. Out of scope, permanently

Two things no amount of pass work recovers, both established in
[07](07-aicore-tracing-direction.md) §D3:

- **Pull-based data movement.** A rank reading a peer's HBM is one-sided; the peer's core executes
  nothing, so there is no second endpoint to pair.
- **Aggregate-counter waits.** If every peer increments one cell and the waiter blocks on the total, no
  per-peer identity exists at the wait. Span only.
