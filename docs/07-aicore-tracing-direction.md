# 07 — New direction: TracR lanes on the AICore

**Opened:** 2026-08-26. Supersedes nothing; extends [06](06-execution-plan.md) §6 and doc
[05](05-benchmarking-compute-comm-copy.md) §8 (comm marker source).

Full feasibility write-up with the pipeline diagram:
<https://claude.ai/code/artifact/5fa1e7ad-c5a3-4f5e-ba5b-691315fa86da>

---

## 1. Why

M2 gave us the L3 picture: host scheduling lanes, per-chip lanes, host→device flow arrows (including the
group fan-out), and copy cost lanes. What it cannot give us is the **collective itself**.

The allreduce data path is direct device-to-device: `CommRemotePtr` resolves into
`ctx->windowsIn[peer]` and the AICore reads and writes peer HBM itself
(`examples/workers/l3/allreduce/kernels/aiv/allreduce_onephase_kernel.cpp:54-56`). The host is control
plane only; the AICPU never observes the transfer. Both production collectives issue `TWAIT` inline on
the AICore, so **no marker above the core can see the wait**. M2b works around this with hand-placed
`get_sys_cnt_aicore()` timestamps read back into a stdout table — useful numbers, but not a lane and not
an arrow.

Reaching the AICore tier is therefore the difference between profiling *around* communication and
profiling communication.

## 2. The reframe

The framing that made this look expensive was "get TracR to compile under CCEC" — port a header-only
library with `thread_local` state, heap buffers and a filesystem flush into a restricted kernel compiler.

That is not the problem that needs solving. **A `.bts` file is a flat array of 16-byte `Payload`
structs** — no header, no magic number, no version field. `load_bts_file` does
`filesize / sizeof(Payload)` and reads the lot (`tracr/postprocessing/tracr_process.cpp:172`).
`tracr_process` has no idea who produced the bytes.

So the goal is: **AICore writes 16-byte records into GM; the host serializes them as a lane.** TracR's
recording runtime is never involved on the device side.

## 3. Ground truth verified before committing

| # | Question | Answer | Evidence |
| --- | --- | --- | --- |
| 1 | Can AICore write timestamped records to GM on silicon? | **Already shipping** | `src/common/platform/include/aicore/chip_swimlane_collector_aicore.h` — `chip_swimlane_aicore_reserve_task_record()` / `_commit_task_record()`; 32 B records, `PLATFORM_AICORE_BUFFER_SIZE = 1024` slots per core, AICPU-rotated, `dcci` for coherency, per-core local state in place of `thread_local` |
| 2 | Can the host emit a TracR lane it never recorded? | **Already shipping** | `tools/tracr_simpler_api.hpp` — `HostCopyTraces2BTS()`. M2a's `HostCopy_0` lanes render today from vectors that never touched TracR's runtime |
| 3 | Do AICore and AICPU share a timebase? | **Yes — same counter, same unit** | Both report in `PLATFORM_PROF_SYS_CNT_FREQ` (50 MHz, 20 ns/tick); `swimlane_converter.py` merges both record kinds through a single `build_clock_alignment()` with no inter-domain offset |

Nothing in simpler's `docs/investigations/` has considered and rejected AICore-side trace recording. The
one adjacent verdict — AICore cannot reach the SPR/MMIO window
(`docs/investigations/2026-06-aicore-mmio-to-spr.md`) — constrains a *different* mechanism; the GM path
this uses is the one the swimlane already runs on.

**Neither swimlane plans for this.** Chip swimlane is documented as incore-level scope only, with L3
composition visible through an orchestrator phase summary rather than nested scopes. Core swimlane is
offline `msprof` camodel replay that deliberately bypasses AICPU orchestration, so by construction it
cannot observe a live collective.

## 4. Phases

Numbering is **D** (device-tier tracing) to keep it distinct from the M-milestones in
[06](06-execution-plan.md) §4. D1–D2 deliver the lane; D3 delivers the arrow; D4 is the onboard
prerequisite for both.

| # | Phase | Scope | Exit criterion |
| --- | --- | --- | --- |
| **D1** | **Vertical slice — one AICore marker to a rendered lane** | Reuse the M2b timing `ChipTensor` as a fixed-capacity Payload buffer. AICore writes `Payload`s instead of raw int64; host serializes them to `thread.<n>/traces.bts`. **No swimlane pool, no rotation.** | `allreduce -p a2a3sim -d 0-1` produces an AICore lane inside each device proc in `perfetto.json`, spans in correct time order |
| **D2** | **Real transport — fifth swimlane pool kind** | Payload pool alongside the existing four (AICore task, AICPU task, sched phase, orch phase); reuse `TypedBuffer`, free queue, rotation. Add a dropped-record counter surfaced to the host. | Markers survive a run that overflows one buffer; drop count reported; no measurable change to a non-traced run |
| **D3** | **Cross-device flow arrows** | Packed `flow_id` (§6), emitted at `TNOTIFY` on the sender and `TWAIT` return on the receiver. | Arrows between two device procs in `perfetto.json`, one per peer message, no unmatched-endpoint warnings |
| **D4** | **Onboard clock alignment** | Wire `simpler_setup/tools/clock_correlation.py` into the TracR path. | Onboard multi-device trace with host, AICPU and AICore lanes on one timeline |

**D1 first, deliberately.** It is the cheapest possible test of the central claim, it touches no shared
infrastructure, and if the lane does not render, D2–D4 are all built on sand. It also reuses a mechanism
already proven in M2b (an AICore-written `ChipTensor` read back by the host), so the only genuinely new
thing under test is the payload format and the `.bts` sink.

## 5. Cost

From simpler's own overhead investigation
(`docs/investigations/2026-07-aicore-swimlane-switch-overhead-and-ack-gate.md`), measured on a3:

| Primitive | Cost |
| --- | --- |
| `get_sys_cnt_aicore()` SPR read | ~100–200 ns |
| Record write-back with `dcci` + `dsb` | ~0.4–0.5 µs |
| Full per-task swimlane overhead | ~0.8 µs |
| **One marker, barrier deferred to publication** (derived) | **~0.15–0.25 µs** |
| Phase-2 barrier wait being measured | 443 µs (a2a3sim, 2 ranks) |

Deferring the barrier to buffer publication is not a hopeful assumption — it already landed for the
swimlane and was verified onboard on a2a3
(`docs/investigations/2026-06-chip-swimlane-defer-wmb.md`). At ~0.2 µs a marker, a handful around a
collective phase is under a tenth of a percent of the wait being measured.

## 6. Decided design points

### Buffer policy — `IGNORE_IF_FULL`, never `PERIODIC`

The AICore GM path **cannot crash on overflow**: `chip_swimlane_aicore_reserve_task_record` returns
`nullptr` once the slot index passes the buffer size ("refuse to write past the end if AICPU failed to
rotate") and commit early-returns on null. That is `IGNORE_IF_FULL` behaviour, inherited. TracR's
`std::exit`-on-full default governs its *own* host and AICPU thread buffers, which AICore records never
touch.

Where a TracR policy does apply, choose `IGNORE_IF_FULL`. `TRACR_POLICY_PERIODIC` writes
`_traces[_traceIdx % CAPACITY]`, so a wrapped buffer is **rotated, not time-ordered** — and
`tracr_process` takes its input as "one timestamp-sorted payload vector per thread folder"
(`tracr_process.cpp:156`), k-way merges on that precondition, and never sorts a loaded `.bts`.
`extract_sync_anchors` compounds it by reading `front()` / `back()` as min and max. A wrap therefore
yields mis-ordered output and corrupt anchors **with no diagnostic**. It also keeps the newest records,
so a `RESET` can outlive its `SET` and a `FLOW_END` its `FLOW_START` — dropping the very arrows D3
exists to draw.

### Flow id — packed, not hashed

The rule (`tracr/examples/tracr/flow_alltoall.cpp`) is that a flow id must be **unique per message and
shared by both endpoints**. A hash delivers only the second half: a collision merges two unrelated
arrows, and because `tracr_process` pairs starts to ends *by index within one id*
(`tracr_process.cpp:620-634`), it mispairs silently rather than warning.

`flow_id` is a uint32 riding in `Payload.extraId`, and the rank cap is
`K_MAX_SUPPORTED_RANKS = 16`, so the tuple packs bijectively with room to spare:

```
flow_id = (src << 24) | (dst << 16) | (seq & 0xFFFF)   // 256 ranks, 65k messages per ordered pair
```

Two disciplines make both sides agree with no handshake:

- **Orientation.** Sender computes `f(me, peer, seq)`, receiver computes `f(peer, me, seq)` — same
  function, mirrored arguments. "peer" is not a third term; it is whichever of the two the local side is
  not.
- **A sequence term.** Without it a pair repeats every round. That does not warn, because index-pairing
  makes N starts and N ends self-pair correctly — right up until one record drops, after which every
  later arrow is quietly wrong. `seq` must be a value both sides already know independently (a round or
  invocation index qualifies; anything data-dependent does not).

Use **global rank, not device id** — device ids are node-local, so two nodes would collide. This is the
cross-node `flowId` uniqueness item already deferred in [STATUS.md](STATUS.md).

## 7. Open risks

1. **Buffer pressure under marker density (D2).** The swimlane writes one record per task and still
   carries a documented caveat above 1024 tasks on one core. Markers around a collective's inner loop are
   denser by an order of magnitude. Pool depth and rotation rate need sizing against a real workload, not
   inheriting the task-record numbers.
2. **`seq` derivation for real collectives (D3).** The allreduce barrier is symmetric and one-notify-
   per-peer-per-run, so the round index works. A collective with an asymmetric or data-dependent message
   count needs a different scheme, and that has not been surveyed.
3. **Sim timing is not evidence (all phases).** Sim runs one OS thread per AICore, so inter-rank skew is
   host-scheduler noise. Sim proves mechanism; only onboard proves cost.

## 8. Relationship to the M-milestones

D1–D4 sit under [06](06-execution-plan.md) §4 M2 (multi-device comm cost class) and close two of the
three TracR-side deltas in §6 of that doc: the clock-correlation gap (D4, and mostly already solved by
`clock_correlation.py`) and the buffer-policy-at-scale question (D2, decided in §6 above). The
`extraId → bytes/bandwidth` delta is untouched by this direction.
