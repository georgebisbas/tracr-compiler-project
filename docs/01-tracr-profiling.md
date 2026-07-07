# 01 — TracR profiling: what TracR needs to profile code

> Part of the **tracr-compiler-project** context set. See [00-pypto-profiling.md](00-pypto-profiling.md)
> for how profiling works in PyPTO today, and [02-tracr-in-simpler-pr1173.md](02-tracr-in-simpler-pr1173.md)
> for the first real integration of TracR into the Simpler runtime.
>
> **Why this doc exists:** the big-plan goal is for PyPTO to *auto-generate* TracR
> instrumentation via a compiler pass. Before we can generate markers, we must know
> exactly what TracR requires of the code it instruments. This doc is that contract.

Source repo: `TracR` (`https://github.com/huawei-csl/TracR`, mirrored at `/home/georgios/workspace/TracR`).

---

## 1. What TracR is, in one paragraph

TracR ("tracer") is a **header-only, nanoscale C++ instrumentation library** for
tracing multi-threaded programs. You sprinkle *markers* into your code; each marker
appends a fixed **16-byte record** to a **lock-free, per-thread ring buffer**. At
shutdown each thread dumps its buffer to a raw binary `.bts` file, and a separate
`tracr_process` tool merges all buffers by timestamp into a **Perfetto** or
**Paraver** timeline. It is **compile-time gated**: with `-DENABLE_TRACR` undefined,
every macro expands to nothing (true zero cost), so instrumentation can be left in
the source permanently and toggled by the build.

There is **no `.so`/`.a`** — everything a caller needs is three headers under
`TracR/include/tracr/`:

| Header | Contents |
| --- | --- |
| `tracr.hpp` | The public macro API (the only header a user `#include`s). |
| `tracr_core.hpp` | The `instrumentation_*()` functions the macros call + global state. |
| `marker_management_engine.hpp` | `Payload`, `NanoTimer`, `TraCRThread`, `TraCRProc`, the ring buffer, flushing. |

`postprocessing/tracr_process.cpp` is the only separately-compiled binary.

---

## 2. The mental model: SET / RESET on channels

TracR records **spans** using a **SET → RESET** model:

- `MARK_SET(channel, event, extra)` opens an event of type `event` on lane `channel`.
- The next `MARK_SET` **or** `MARK_RESET(channel)` on that same channel closes it.
- The postprocessor reconstructs a duration from each open→close pair.

Three orthogonal identifiers describe every record:

| Field | Type | Meaning |
| --- | --- | --- |
| **channelId** | `uint16_t` | The visualization **lane** (0-based, caller-chosen). One "row" in the timeline — usually a thread, core, or logical stream. |
| **eventId** | `uint16_t` | The **event type** — maps to a text label + a color. You get one back when you *register* a marker. |
| **extraId** | `uint32_t` | Optional **per-event tag** (task id, size, kernel func_id…). `UINT32_MAX` = "none". |

Plus a `uint64_t` nanosecond **timestamp** captured automatically at each call.

**Flows** (`FLOW_START`/`FLOW_END`) draw arrows between two already-open events —
the classic use is connecting a send to its matching receive.

---

## 3. The public API surface

All macros live in `TracR/include/tracr/tracr.hpp` and forward to `TraCR::instrumentation_*()`
in `tracr_core.hpp`.

### Lifecycle

| Macro | Args | Behavior |
| --- | --- | --- |
| `INSTRUMENTATION_START()` | none | Create the process-global `TraCRProc`, build the `tracr/proc.<pid>/` folder tree, **auto-init the calling (main) thread**. Aborts if a proc already exists. |
| `INSTRUMENTATION_END()` | none | Flush the main thread buffer, write `metadata.json`, tear down. **Aborts unless exactly one thread remains** — every worker thread must have finalized first. |
| `INSTRUMENTATION_THREAD_INIT()` | none | Allocate this thread's `thread_local` buffer; `++num_threads`. Call once at the top of every **non-main** thread. Aborts if the thread already has one. |
| `INSTRUMENTATION_THREAD_FINALIZE()` | none | Flush this thread's `.bts`, destroy its buffer, `--num_threads`. Call once before the thread exits. |

### Registering event types (returns an `eventId`)

Registration is **front-loaded and comparatively expensive (~3 µs)** and **not
thread-safe** — do it once, on one thread, before spawning workers, and keep the
returned ids.

| Macro | Args | Returns |
| --- | --- | --- |
| `INSTRUMENTATION_MARK_ADD(label)` | `const std::string&` | `uint16_t eventId`; auto-assigns the next color (counter starts at 23). |
| `INSTRUMENTATION_MARK_W_COLOR_ADD(label, colorId)` | `const std::string&, uint16_t` | `uint16_t eventId`; uses an explicit Paraver palette color. Aborts if that color was already used. |

### Recording events (the hot path)

| Macro | Args |
| --- | --- |
| `INSTRUMENTATION_MARK_SET(channelId, eventId, extraId)` | `extraId` defaults to `UINT32_MAX`. Builds a `Payload` and appends it. |
| `INSTRUMENTATION_MARK_RESET(channelId)` | Closes the open event on the channel (stores `eventId = EVENTID_RESET`). |

### Flow events (arrows)

| Macro | Args | Contract |
| --- | --- | --- |
| `INSTRUMENTATION_FLOW_START(channelId, flowId)` | `uint16_t, uint32_t` | Both endpoints share the same `flowId`; the `flowId` must be **globally unique** in the trace; call while an event is *open* on that channel. |
| `INSTRUMENTATION_FLOW_END(channelId, flowId)` | `uint16_t, uint32_t` | The arrow attaches to the enclosing open event. |

### Channel metadata (call once, before `END`)

| Macro | Args | Effect |
| --- | --- | --- |
| `INSTRUMENTATION_ADD_CHANNEL_NAMES(json_array)` | `nlohmann::json` array | Names each lane; also sets `num_channels`. |
| `INSTRUMENTATION_ADD_NUM_CHANNELS(n)` | `uint16_t` | Declares lane count; lanes get generic names. |

### Control / introspection

`INSTRUMENTATION_ON()` / `INSTRUMENTATION_OFF()` toggle the (non-atomic) `enable_tracr`
bool; `INSTRUMENTATION_TRACE_PATH(path)` sets the output dir (**must precede `START`**);
`INSTRUMENTATION_ACTIVE` is a compile-time `true`/`false` constant for gating your own
non-TracR code; plus `INSTRUMENTATION_IS_PROC_READY()`, `_NUM_TRACR_THREADS()`,
`_PROC_EXISTS()`, `_THREAD_EXISTS()`, `_GET_JSON_STR()`.

### The zero-cost-stub mechanism

`tracr.hpp` is `#ifdef ENABLE_TRACR`. When **defined**, macros map to real calls.
When **undefined**, they expand to:

- nothing (recording macros become `(void)(arg)` casts, so no "unused variable" warnings),
- literal fallbacks so assignments still compile (`MARK_ADD` → `0`, `IS_PROC_READY` → `false`, `GET_JSON_STR` → `""`).

So instrumented source always compiles both ways; the build flag alone decides whether
any code is emitted.

---

## 4. The integration contract — what a caller MUST do

This is the exact sequence a program (or a code generator) must produce. Derived from
`TracR/examples/tracr/simple.cpp` and `pthread.cpp`.

**Once, at startup, on the main thread:**
1. *(optional)* `INSTRUMENTATION_TRACE_PATH("./out/")` — before START.
2. `INSTRUMENTATION_START()`.
3. Register **every** event type and **store each returned `eventId`** (file-scope
   globals if worker threads need them). Must be single-threaded.

**Per non-main thread:** `INSTRUMENTATION_THREAD_INIT()` at entry,
`INSTRUMENTATION_THREAD_FINALIZE()` before exit. (The main thread is exempt — START/END
handle it.)

**Per event, on any initialized thread:** `MARK_SET(channel, eventId, extra)` … work …
`MARK_RESET(channel)`. Flow calls go *between* a SET and its RESET.

**Once, at shutdown, on the main thread:** declare channels
(`ADD_NUM_CHANNELS` or `ADD_CHANNEL_NAMES`), then `INSTRUMENTATION_END()`. **All worker
threads must already be joined and finalized** or END aborts.

### State the caller must track

| What | Type | Notes |
| --- | --- | --- |
| **eventIds** | `uint16_t` | Returned by registration; passed to every `MARK_SET`. |
| **channelIds** | `uint16_t` | Caller-chosen, 0-based, must be `< num_channels`. `simple.cpp` uses a single lane `0`; `pthread.cpp` uses the thread index so each thread gets its own lane. |
| **flowIds** | `uint32_t` | Globally unique; shared by both flow endpoints. |
| **extraId** | `uint32_t` | Optional tag; `UINT32_MAX` = none. |

### ⚠️ Threading rules (these bite)

- **Registration is not thread-safe** — do it once on one thread before workers start.
- **Recording is lock-free per thread** — each thread owns its own buffer, no cross-thread
  sync. But there is **no null check on the hot path**: calling `MARK_SET`/`RESET`/`FLOW_*`
  from a thread that never called `THREAD_INIT` is **undefined behavior**.
- **`enable_tracr` is a plain, non-atomic `bool`** — `ON`/`OFF` toggling is best-effort and
  may race by one event (intentional, to keep the hot path free of fences).

---

## 5. Data model & payload

`struct Payload` — exactly **16 bytes** (`static_assert(sizeof(Payload)==16)`), so 4
records per 64-byte cache line:

```cpp
struct Payload {
    uint16_t channelId;   // lane [0, 65535]
    uint16_t eventId;     // event type -> label + color
    uint32_t extraId;     // user tag; UINT32_MAX = none
    uint64_t timestamp;   // nanoseconds
};
```

**Reserved eventIds** sit at the top of the `uint16_t` range: `EVENTID_RESET = 65535`,
`EVENTID_FLOW_START = 65534`, `EVENTID_FLOW_END = 65533`. Registration hands out ids
starting from 0 and must stay **below 65533** — so ~65,533 user marker types max.

**eventId → label/color** is held in three parallel structures on `TraCRProc`
(`_markerLabels`, `_markerColorIds`, `_markerTypes`), all populated at registration.

**channelId → lane:** in Perfetto each channel becomes a track (`tid = channelId + 1`);
in Paraver each channel becomes a "thread" within the proc's task. Lanes are named by
the `channel_names` / `num_channels` metadata.

**Timestamp / clock** — `NanoTimer::now()`:
- **default:** `clock_gettime(CLOCK_MONOTONIC_RAW)`.
- **`-DUSE_HW_COUNTER`:** raw hardware counter (`rdtsc` on x86_64, `mrs cntvct_el0` on
  AArch64) converted to ns via the counter frequency. ⚠️ The shipped **Meson dependency
  forces `-DUSE_HW_COUNTER` on** even though the README lists it as off, so the clock
  source silently differs between the Meson build and a hand-rolled `g++` build.

---

## 6. Output & post-processing

### On-disk layout

```
<trace_path>/tracr/
  proc.<pid>/               # pid = getpid() -> distinct folder per process/MPI rank
    metadata.json
    thread.<tid>/           # tid = SYS_gettid, one per initialized thread
      traces.bts
```

### `metadata.json`

`pid`, `start_time` (ns, captured at `START`), `end_time` (ns, refreshed at dump),
`markerLabels` (array indexed by eventId), `markerColorIds`, `markerTypes`
(colorId-string → label), and `channel_names` / `num_channels` if the caller declared them.
The postprocessor *also reads* an optional `extraIdLabels` map (extraId → human name) but
the library never writes it — it's a hook for external trace producers (used by the
Simpler integration, see doc 02).

### `.bts` files

Raw, headerless, contiguous little-endian array of 16-byte `Payload` structs. The loader
computes the count as `filesize / sizeof(Payload)`.

### `tracr_process` — the converter

`tracr_process <tracr-folder> [perfetto|paraver|dump]` (default **perfetto**). It scans all
`proc.*` folders, loads every `thread.*/traces.bts`, does a **heap-based k-way merge by
timestamp**, and normalizes each proc's timestamps to its own `sync_start` anchor.

| Format | Output | Notes |
| --- | --- | --- |
| **Perfetto** *(default)* | `perfetto.json` (Chrome Trace Event JSON) | Open at [ui.perfetto.dev](https://ui.perfetto.dev). Each proc = a process, each channel = a track; SET→next becomes a complete `X` event; flows become `s`/`f` arrows; `extraId` goes under `args`. |
| **Paraver** | `tracr.prv` + `tracr.pcf` + `tracr.row` (+ copies `state.cfg`) | Flow pairs become Paraver communication records. Requires `state.cfg` in the working dir. |
| **Dump** | stdout | Chronological dump + reports channels with unbalanced SET/RESET — a debugging aid. |

**Multi-proc sync:** timestamps are shifted to each proc's `sync_start`, assuming all procs
hit `INSTRUMENTATION_START()` at (nearly) the same real instant — e.g. right after `MPI_Init`.

---

## 7. Build & integration

- **Build system:** Meson, C++17, header-only. `meson setup build && cd build && ninja`.
- **Downstream dependency object:** `InstrumentationBuildDep` (from `meson.build`). Because
  the library is header-only, "linking" just means adding this dependency (it contributes
  the `include/` + `extern/` include dirs).
- **As a subproject:** drop the repo at `subprojects/tracr` and use
  `dependency('TraCR', fallback: ['tracr', 'InstrumentationBuildDep'])`.
- **Enabling:** off by default — the consuming target must pass **`-DENABLE_TRACR`** in its
  own `cpp_args`; the dependency does not force it. Consuming code just does
  `#include <tracr/tracr.hpp>`.
- **Bundled third-party:** nlohmann/json is vendored at `extern/nlohmann/json.hpp`.

### Compile-time flag matrix

| Flag | Effect |
| --- | --- |
| `ENABLE_TRACR` | Turn macros into real calls (vs. no-ops). |
| `TRACR_CAPACITY` | Per-thread buffer size in **events** (default `1<<20` ≈ 1M events ≈ **16–17 MB/thread**). |
| `USE_HW_COUNTER` | HW timer instead of `clock_gettime`. |
| `TRACR_POLICY_PERIODIC` | Ring-buffer: overwrite oldest when full. |
| `TRACR_POLICY_IGNORE_IF_FULL` | Silently drop when full. |
| *(neither policy)* | **Abort** (`std::exit`) when the buffer fills — the default. |
| `TRACR_DISABLE_FLUSH` | Skip folder creation + `.bts` writing; keep traces in memory only. |
| `ENABLE_DEBUG` | Internal debug prints. |

---

## 8. Overhead / hot-path design

Why TracR is cheap enough to leave on during a measured run:

- **Per-thread, lock-free ring buffer** (`std::array<Payload, CAPACITY>` embedded in each
  `thread_local` object) — no cross-thread synchronization to record.
- **`store_trace` is an index write + increment.** The buffer-full test is marked
  `TRACR_UNLIKELY` so the common path is branch-predicted straight through.
- Each `MARK_SET`/`RESET` does: one `enable_tracr` check, one 16-byte `Payload`
  construction, one timestamp read (a single `clock_gettime` or one `rdtsc`/`mrs`), and the
  array store. **No allocation, no I/O, no locks.**
- **No flushing on the hot path** — buffers are written to `.bts` only at
  `THREAD_FINALIZE`/`END`, as one bulk write of the used range.
- **Zero cost when disabled** — the macros vanish at preprocessing.

`examples/tracr/performance_test.cpp` is the built-in micro-benchmark (1000 SET/RESET pairs,
average ns/marker).

---

## 9. Gotchas worth remembering

1. **`USE_HW_COUNTER` is forced on** by the Meson dependency — the clock source differs
   between Meson and manual `g++` builds.
2. **`INSTRUMENTATION_ON/OFF` are implemented** (they toggle `enable_tracr`) despite a
   "NOT YET IMPLEMENTED" comment in the disabled branch.
3. **`-DbuildPyTraCR=true`** appears in the GitLab CI but is not a defined Meson option in
   this repo — a stale reference to a Python binding not present here.
4. **END aborts** if worker threads haven't finalized, and **recording UB** if a thread
   skipped `THREAD_INIT`. Any code generator must emit the lifecycle calls in the right order.

---

## 10. What this means for the "big plan"

For PyPTO to emit TracR instrumentation via a compiler pass, the generated code must satisfy
**this contract**. Concretely, a pass has to produce, in the right places:

- **Exactly one** `INSTRUMENTATION_START()` on the entry thread and one `INSTRUMENTATION_END()`
  at teardown, with `THREAD_INIT`/`THREAD_FINALIZE` bracketing every worker thread.
- A **registration block** up front that assigns a stable `eventId` to each kind of region
  the compiler knows about (a kernel, a scope, a scheduler phase), storing them where the
  hot-path code can read them.
- `MARK_SET`/`MARK_RESET` pairs around each region, with a **channel mapping** decided by the
  compiler (which lane represents which thread/core/stream) and an **`extraId`** carrying the
  identity the compiler already has (e.g. a kernel `func_id`).
- `channel_names` / `num_channels` metadata so the timeline is readable.

The catch is that TracR's model was designed for a **single multi-threaded host process** that
flushes `.bts` files itself. PyPTO's orchestration C++ runs on **AICPU on the device**, where
threads can't write host files. That gap is exactly what PR #1173 solves by hand
(`TRACR_DISABLE_FLUSH`, device HW-counter timestamps, and a host-side download of the raw
buffers) — see [02-tracr-in-simpler-pr1173.md](02-tracr-in-simpler-pr1173.md). The compiler
pass will need to generate code that fits that device-collect / host-download shape, not the
vanilla file-flushing shape.

Two features above are the primitives for profiling **communication and multi-node** runs, and matter
more than they might first appear:

- **Flow events** (`FLOW_START`/`FLOW_END`, §3) are the message-passing arrows — connect a remote
  write/send to its matching read/recv across threads, cores, or ranks. They are how a collective's
  causal graph gets drawn.
- **Per-proc folders + multi-proc sync** (§6) make TracR multi-node by construction: one `proc.<pid>` per
  rank, merged and aligned to a common `sync_start`. For distributed runs the anchor must be enforced
  (a barrier), not assumed.

Both are put to work in [05-benchmarking-compute-comm-copy.md](05-benchmarking-compute-comm-copy.md),
which covers benchmarking compute + copy-in/out + communication, single- and multi-node.
