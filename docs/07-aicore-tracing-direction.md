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
| **D1** ✅ | **Vertical slice — one AICore marker to a rendered lane** — **DONE 2026-08-26** | Reuse the M2b timing `ChipTensor` as a fixed-capacity Payload buffer. AICore writes `Payload`s instead of raw int64; host serializes them to `thread.<n>/traces.bts`. **No swimlane pool, no rotation.** | `allreduce -p a2a3sim -d 0-1` produces an AICore lane inside each device proc in `perfetto.json`, spans in correct time order |
| **D2a** ✅ | **Shared emitter + drop counting** — **DONE 2026-08-27** | `tracr_aicore_emit.h` owns the device-side contract: Payload wire layout, count/dropped header, set/reset helpers. Self-initializing header, overflow drops and counts. | Emitter reusable outside the example; a buffer too small to hold the run truncates at a span boundary and reports the drop count |
| **D2b** ✅ | **Designated-writer guard** — **DONE 2026-08-27, verified onboard** | `kTracrDisabled` as a capacity sentinel makes every non-designated worker a no-op. Runtime-allocated per-core buffers are deferred until SPMD needs them: `block_dim>1` is not implemented, so no race exists today. | Both chips record their own lane; a wrong-writer predicate is caught rather than silently losing a lane |
| **D3** ✅ | **Cross-device flow arrows** — **DONE 2026-08-27, verified onboard** | Arrow tail at each `TNOTIFY`, head at the matching `TWAIT` return; both endpoints derive the packed id independently. | 4 arrows between the two device procs, all matched, no id collision with the host→device group flows |
| **D4** | **Onboard clock alignment** | Wire `simpler_setup/tools/clock_correlation.py` into the TracR path. | Onboard multi-device trace with host, AICPU and AICore lanes on one timeline |

**D1 first, deliberately.** It is the cheapest possible test of the central claim, it touches no shared
infrastructure, and if the lane does not render, D2–D4 are all built on sand. It also reuses a mechanism
already proven in M2b (an AICore-written `ChipTensor` read back by the host), so the only genuinely new
thing under test is the payload format and the `.bts` sink.

### D1 result (2026-08-26)

**Done and rendering.** `allreduce -p a2a3sim -d 0-1` emits AICore spans onto the `AIVector_0` lane of
both device procs, beside the AICPU-recorded `Running_Task_Single` on `AIVector_2`:

```
pid=1000  AIVector_0  Phase2   dur=5.9us    extra_id=0     (rank 0 issuing notifies)
pid=1000  AIVector_0  Barrier  dur=347.9us  extra_id=1     (rank 0 waiting on peer 1)
pid=1001  AIVector_0  Phase2   dur=0.3us    extra_id=1
pid=1001  AIVector_0  Barrier  dur=341.2us  extra_id=0     (rank 1 waiting on peer 0)
```

Trace kept at `~/tmp/tracr_d1/perfetto.json`. Commits: `a83cf360` (emitter), plus the two corrections
below. **The central claim holds: `tracr_process` needed no changes at all.**

Two things the slice taught us, both of which change D2's design:

1. **`channel_names` is sized by the run's actual core count.** A sim run with 8 AICube / 16 AIVector puts
   `AIVector_0` at index 12; the 24/48 layout puts it at 28. No channel index is knowable at kernel
   compile time — the kernel writes a placeholder and the host stamps the resolved index when packing,
   exactly as `HostCopyTraces2BTS` already does. Only the *event* ids are compile-time constants, and they
   are re-verified against the emitted metadata on every run.
2. **The trace buffer needs an explicit record count.** It is an `OUTPUT_EXISTING` tensor, so the host's
   zeros are never staged to the device and untouched words hold stale GM. Decoding until a zero timestamp
   appeared silently picked up six records of unrelated memory with timestamps from a different clock
   domain. Word 0 now carries the count. **The same latent flaw was in M2b**, whose `!= 0` per-peer check
   could equally have read stale slots — it simply never happened to.

Deliberately still open, for D2: the kernel writes from whichever AIV core runs it, with no block-index
guard — the same assumption M2b made. A multi-block kernel would have several cores racing on one buffer,
which is precisely what the per-core pool structure exists to fix.

### D2a result (2026-08-27)

**Done.** The emitter moved to `src/common/platform/include/aicore/tracr_aicore_emit.h` and the allreduce
kernel now uses it rather than carrying its own copy. Two behaviours verified in sim:

- **Normal run** — unchanged output: 4 payloads / 2 spans per rank, no drops, golden matched.
- **Overflow probe** (buffer deliberately sized to 2 payloads) — 2 written, **2 dropped and reported**, and
  the lane truncated *at a span boundary*: a complete SET/RESET pair, still monotonic, no orphaned RESET.
  Kernel correctness unaffected. This is the `IGNORE_IF_FULL` behaviour §6 argues for over `PERIODIC`,
  demonstrated rather than assumed.

**No cache maintenance is needed in this design**, and that is a real benefit of the fixed-buffer choice
over the pool: the buffer has a single reader, on the host, after the kernel completes, so ordering comes
from the task-completion path that already precedes the copy back. The per-record `dcci`+`dsb` that costs
the swimlane ~0.4–0.5 µs only becomes necessary when the AICPU drains buffers *while* a core keeps
writing. A marker here is therefore ~0.15–0.2 µs, not 0.6–0.7 µs.

**Scope correction.** When D2's options were weighed, the fixed-buffer path was costed at "~150 lines,
touches no existing file". That was optimistic: it holds for the emitter, but *per-core* buffers do not
follow from it. A user kernel cannot learn its own core index — `block_idx` is a parameter of
`aicore_execute` and never reaches kernel code — so runtime-allocated per-core buffers need the
`KernelArgs` slot-table chain (host allocates, AICPU populates, AICore kernel entry stashes, getter
lazy-resolves), the same one `chip_swimlane_aicore_rotation_table` uses. That is D2b: ~6 files including a
wire-struct field and both platform kernel entries. Still far below the fifth-pool-kind option, but not
free.

### D2b result (2026-08-27) — first onboard run

**Done, and verified on real silicon** (`-p a2a3 -d 0-1`, 910B2). Both chips render their AICore comm
lane, all ranks match golden:

```
pid=1000  AIVector_0  Phase2   dur=      0.7us  extra_id=0
pid=1000  AIVector_0  Barrier  dur=      0.4us  extra_id=1
pid=1001  AIVector_0  Phase2   dur=      0.7us  extra_id=1
pid=1001  AIVector_0  Barrier  dur=  13045.3us  extra_id=0
```

**The first physically meaningful comm numbers this project has produced.** Sim reported ~340 µs
symmetric waits, which was host-scheduler noise; onboard shows a **13 ms inter-chip arrival skew** —
rank 1 reached the barrier ~13 ms before rank 0 and blocked there, while rank 0 walked straight through in
0.4 µs. That is the straggler signal doc [05](05-benchmarking-compute-comm-copy.md) exists to expose, and
it is invisible from any tier above the core.

**The onboard run immediately caught a bug the sim run could not.** The guard first tested the
zero-argument hardware `get_block_idx()`. A single-block task lands on whichever physical core is free, so
on rank 1 that index was non-zero and the guard **silently muted the entire lane** — rank 0 recorded, rank
1 reported "no payloads". The fix is the *logical* SPMD index, `get_block_idx(args)` from
`runtime/common/intrinsic.h` (which kernels may include; the a5 examples do), and that is 0 for a
single-block task on every chip.

The lesson generalises past this bug: **a wrong writer-predicate fails silently, by losing a lane.** That
is a worse failure mode than the race it guards against, which cannot occur while `block_dim>1` is
unimplemented. Any future change to the predicate must be validated on more than one chip.

### D3 result (2026-08-27) — the first cross-device arrows

**Done, verified onboard** (910B2, 2 chips). Both directions of the barrier pair correctly, and the
host→device group flows coexist with no collision:

```
tail id=0x00010000 (src=0 dst=1 seq=0)  pid=1000 AIVector_0   rank 0's notify ...
head id=0x00010000 (src=0 dst=1 seq=0)  pid=1001 AIVector_0   ... releases rank 1's wait
tail id=0x01000000 (src=1 dst=0 seq=0)  pid=1001 AIVector_0
head id=0x01000000 (src=1 dst=0 seq=0)  pid=1000 AIVector_0
tail id=0x00000001                      pid=<host> L3_Orchestrator   (group flow, unchanged)
head id=0x00000001                      pid=1000  AICPU_2
```

The trace now answers *why* a rank stalled, not just how long: rank 1 blocked at the barrier for 27.5 ms
(13.0 ms in the previous run — the skew itself varies run to run), and the arrow names exactly which
notify from rank 0 ended it. Commit `fce8109b`.

**What D3 does and does not draw.** It draws the *synchronization* edges — who released whom. It does not
draw the data movement, and that is structural rather than a gap: Phase 3 is a **pull**
(`TLOAD(recvTile, remoteG)` against `CommRemotePtr(commCtx, scratch, peer)`), so a rank reads its peer's
HBM directly and the peer's AICore executes nothing. A one-sided read has one endpoint, so no flow id
scheme can pair it. For "which peer's data did this rank pull, and when", the answer is a per-peer span
around each `TLOAD`, not an arrow.

Two further limits, neither hit by this collective:

- **Aggregate counters.** This barrier uses per-peer signal slots (rank r writes peer's `signal[r]` and
  waits on its own `signal[p]`), so identity is present at both ends. A collective that waits on one
  counter incremented by every peer cannot attribute its release to any sender — no pair, no arrow.
- **`seq` derivation.** A constant is correct here only because the barrier sends exactly one notify per
  peer per invocation. A ring allreduce needs the step index, a segmented reduce-scatter the chunk index,
  and both sides must derive it without communicating.

**An onboard run failed first with `VEC instruction error: the ub address out of bounds` plus a driver
`rtMemExportToShareableHandleV2 ... feature not support`, then passed unchanged on re-run** — the
poisoned-device-state signature already recorded in STATUS. Re-run before investigating an onboard AICore
fault.

## 5. Cost

From simpler's own overhead investigation
(`docs/investigations/2026-07-aicore-swimlane-switch-overhead-and-ack-gate.md`), measured on a3:

| Primitive | Cost |
| --- | --- |
| `get_sys_cnt_aicore()` SPR read | ~100–200 ns |
| Record write-back with `dcci` + `dsb` | ~0.4–0.5 µs |
| Full per-task swimlane overhead | ~0.8 µs |
| **One marker, D2 as designed** (derived: clock read + per-record `dcci`+`dsb`) | **~0.6–0.7 µs** |
| One marker *if* the AICore barrier can be deferred to publication | ~0.2 µs — **unproven**, see below |
| Phase-2 barrier wait being measured | 443 µs (a2a3sim, 2 ranks) |

**The deferral is not already proven for the AICore side.** The `defer-wmb` investigation
(`docs/investigations/2026-06-chip-swimlane-defer-wmb.md`) removed the per-task `wmb()` in
`chip_swimlane_aicpu_complete_task` — the *AICPU* collector. `chip_swimlane_aicore_commit_task_record`
still issues `dcci(record, SINGLE_CACHE_LINE, CACHELINE_OUT)` + `dsb` **per record**, and that asymmetry
looks deliberate: the AICore does not control publication. The AICPU rotates buffers and the core only
discovers it lazily, by re-reading `head->current_buf_seq` on its next reservation — so the AICPU can
publish and drain a buffer while the core still holds un-flushed records. Deferring on the AICore side is
therefore an open design question for D2, not an available pattern.

Either way the cost case holds: at 0.6–0.7 µs a marker, a handful around a collective phase is ~0.2% of
the 347 µs wait D1 measured. But D2 should be planned at 0.6–0.7 µs, not 0.2 µs.

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

## 9. Build environment (each of these cost a rebuild)

- **Build and run in `simpler-cann9`**, via `attach_docker_simpler.sh` + `build_simpler.sh`. The configured
  toolchain is `/usr/local/bin/g++-15`, which exists only in that image — `pypto3-hw-native-sys:cann9` has
  gcc-15 at `/usr/bin/`, so CMake's `project()` fails there with "not a full path to an existing compiler".
- **`BUILD_TRACR=ON` needs `pybind11`** (for `pybind11_add_module(tracr ...)` at `CMakeLists.txt:50`), and
  it is absent from the system site-packages of *both* images. A warm `build/` cmake cache hides this; a
  clean configure does not. `pip install pybind11` into the venv is the fix.
- **Do not `rm -rf build/` to troubleshoot.** It discards the cache that was satisfying the dependency
  above, turning a working tree into one that cannot configure at all.
- **`a2a3sim` cannot compile kernels in `simpler-cann9`** — the sim path uses the host g++ and hits
  `'_Float16' does not name a type` in `pto/common/type.hpp`. Onboard is unaffected (it compiles with
  `ccec`), so **prefer `-p a2a3` for verification**: it is both more reliable here and the only source of
  physically meaningful timing.
- A stale/mixed `build/` presents as `_ensure_comm_base failed ... control_comm_init ... FileNotFoundError`
  at multi-chip setup — which reads like an IPC or container problem and is not. A clean rebuild with the
  flags above fixes it.
