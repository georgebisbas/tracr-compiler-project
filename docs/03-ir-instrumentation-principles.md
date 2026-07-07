# 03 — Compiler-level instrumentation principles (Devito as the exemplar)

> Part of the **tracr-compiler-project** context set. This is the *theory* doc: how code-generation
> frameworks parse their IR, identify critical zones, and inject profiling instrumentation as a
> compiler pass. Its practical companion is
> [04-codegen-instrumentation-blueprint.md](04-codegen-instrumentation-blueprint.md).
>
> **Why this doc exists:** the big-plan goal — PyPTO auto-generating TracR markers via a pass — is a
> solved *category* of problem. Rather than invent an approach, we study the canonical one (Devito),
> extract the reusable principles, and place PyPTO/TracR in that landscape.

Primary source: **Devito** (`https://github.com/devitocodes/devito`, checked out at
`/home/georgios/workspace/devito`). Devito compiles SymPy stencil PDEs → a schedule tree → the **IET**
(Iteration/Expression Tree, its low-level IR) → C. Its profiling instrumentation is a small set of IET
passes centered on `devito/passes/iet/instrument.py`.

---

## 1. The core idea

A code generator that lowers a high-level program to an IR can add profiling **the same way it adds any
other lowering**: as a **pass that pattern-matches IR nodes and rewrites them** to wrap regions with
telemetry. The program author writes no instrumentation; the compiler injects it at build time, gated by
an option. This decouples *what is measured* from *the domain logic being compiled*.

Every mature framework converges on the same handful of principles. Here they are, then Devito showing
each concretely, then where everyone else (and PyPTO) sits.

---

## 2. The reusable principles

1. **IR rewriting via the visitor / transformer pattern.** The IR is an immutable tree. Instrumentation
   is a *query* (find the nodes to wrap) followed by a *rewrite* (rebuild those nodes wrapped in
   telemetry). Nodes are never mutated in place — a `Transformer` produces a new tree.

2. **Separate region identification from measurement.** *Where* to profile is decided upstream and
   structurally — the compiler marks logical regions ("this loop nest is one unit of work") as neutral
   IR markers, carrying no timing concept. *How* to measure is a **separate, late pass** that only wraps
   those pre-existing markers. Changing the tracer never touches region logic, and vice-versa.

3. **The collector is a first-class IR object, passed by reference.** The timer / trace buffer is a real
   symbolic object that the normal signature-threading machinery propagates into every instrumented
   function and up through its callers — no special-casing. Results accumulate into a shared struct/buffer.

4. **Compile-time injection, gated by a policy.** The whole scheme is a pass toggled by an option (and
   usually *levels*: off / coarse / verbose). When off, nothing is emitted. Backends reuse the identical
   pass through an indirection layer.

5. **Read results back through the shared collector; symbolic-at-compile, numeric-at-runtime.** The
   generated code writes numbers into the struct; the host reads them back after the run and combines
   them with quantities computed *symbolically at compile time* (op counts, traffic) to derive metrics
   (GFLOP/s, operational intensity) — without re-deriving anything at runtime.

6. **Keep the hot path cheap; place markers at region granularity.** Wrap whole loop *nests*, never the
   innermost iteration — one telemetry pair per region per step, not per grid point. The per-marker cost
   is a single cheap operation (a `gettimeofday` + `+=`, or an array append).

---

## 3. Devito, concretely

### 3.1 Three touchpoints in the pipeline

The profiler is created once per operator build (`create_profile('timers')`,
`devito/operator/operator.py:206`) and touched at three ordered points:

| Phase | When | What |
| --- | --- | --- |
| **`analyze`** | during unbounded-IET lowering (`operator.py:471`) | walk each `Section`, compute **symbolic** op-counts / traffic / points per region — *before* any instrumentation |
| **`instrument`** | after target specialization (`operator.py:499`) | wrap each `Section` in a timer; **postponed until after specialization because more Sections may appear** |
| **`summary`** | at `op.apply()` (`operator.py:1072`) | read the timer struct back, combine with the symbolic analysis, print roofline metrics |

### 3.2 Region identification — the `Section` (principle 2)

Critical zones are decided far upstream, at the **schedule-tree** stage, not in the instrument pass.
`attach_section` (`devito/ir/stree/algorithms.py:305`) wraps a candidate subtree in a `NodeSection`
unless `reuse_section` (`algorithms.py:272`) decides it belongs with the previous one (same iteration
dimensions, etc.). That heuristic *is* the region-identification logic, kept entirely separate from
timing. When the schedule tree is lowered to the IET, each `NodeSection` becomes a `Section` with a
stable sequential name (`section0`, `section1`, … — `devito/ir/iet/algorithms.py:50`).

The `Section` node itself (`devito/ir/iet/nodes.py:1243`) is **just a `List` with a name** — it generates
no code of its own (`visit_Section`, `visitors.py:561`); it is a *purely logical marker* in the tree:

```python
class Section(List):
    is_Section = True
    def __init__(self, name, body=None, is_subsection=False):
        super().__init__(body=body); self.name = name; self.is_subsection = is_subsection
```

### 3.3 Measurement — the `TimedList` and the instrument pass (principles 1, 6)

`instrument()` (`instrument.py:16`) builds **one** `Timer` from `profiler.all_sections`, then runs three
`@iet_pass`-decorated sub-passes:

- **`track_subsections`** (`instrument.py:30`) — refines structure *before* timing: adds finer
  sub-Sections (MPI halo calls, busy-waits) gated by a verbosity level, and groups multi-pass temporaries.
- **`instrument_sections`** (`instrument.py:107`) — the actual injection; delegates to
  `profiler.instrument(iet, timer)`, which wraps each `Section`'s body in a `TimedList`:

  ```python
  for i in sections:
      mapper[i] = i._rebuild(body=TimedList(timer=timer, lname=i.name, body=i.body))
  return Transformer(mapper, nested=True).visit(iet)   # profiling.py:132
  ```

- **`sync_sections`** (`instrument.py:125`) — for GPU/OpenACC, appends device barriers inside a
  `TimedList` when the region uses an async queue, so the timer captures true execution time, not just
  launch time (and only where a busy-wait isn't already forcing sync — principle 6).

The `TimedList` (`nodes.py:986`) is a `List` whose header/footer are `START`/`STOP` macros wrapping the
region — the timer syscalls sit at the region boundary, **never inside the inner loop**:

```python
super().__init__(header=c.Line(f'START({lname})'),
                 body=body,
                 footer=c.Line(f'STOP({lname},{timer.name})'))
# START(S):  struct timeval start_##S, end_##S; gettimeofday(&start_##S, NULL);
# STOP(S,T): gettimeofday(&end_##S,NULL); T->S += (end-start seconds);
```

`instrument_sections` returns those macro definitions as `{'headers': [...]}` so they're emitted into the
file preamble **only when instrumentation actually happened**.

### 3.4 The timer as a first-class object threaded by reference (principles 3, 5)

The `Timer` (`devito/types/misc.py:40`) is a `CompositeObject` that lowers to a ctypes struct
`struct profiler { double section0; double section1; … }` — one `double` per section — held **by pointer**
(`byref`, `devito/types/object.py:145`). Because `TimedList.functions` returns the timer
(`nodes.py:1029`), Devito's standard `update_args` machinery (`engine.py:692`) automatically threads that
struct pointer into every instrumented function's signature and up through the call graph — no special
casing. The `STOP` macro's `T->section_i += …` accumulates in place.

Read-back is a direct ctypes access after the run:

```python
time = max(getattr(args[self.name]._obj, name), 10e-7)   # profiling.py:201
```

The **advanced** profiler pairs this with the symbolic op/traffic/point counts captured at `analyze`
time, substitutes runtime arg values, and produces GFLOP/s, GPoints/s, and **operational intensity**
(`ops/traffic` — the x-axis of a roofline plot) in `PerformanceSummary` (`profiling.py:406`). This is the
"symbolic-at-compile, numeric-at-runtime" split: the performance model stays exact without any runtime
re-derivation.

### 3.5 Policy / levels (principle 4)

`configuration['profiling']` (env `DEVITO_PROFILING`, default `'basic'`, `devito/__init__.py:163`)
selects from `profiler_registry` (`profiling.py:524`): `basic`, `basic1/2`, `advanced`, `advanced1/2`,
`advisor`. The trailing digit is the verbosity feeding `track_subsections`; `basic` vs `advanced` toggles
the roofline analysis; different language backends reuse the identical pass via a `Target.instrument`
indirection (`devito/passes/iet/languages/targets.py:36`).

### 3.6 The external-tracer variant already lives here: Advisor mode

Tellingly, Devito's `AdvisorProfiler` (`profiling.py:357`) **abandons the timer-struct scheme entirely**
and instead injects **external marker-API calls**: it brackets the time loop with Intel's `ittnotify`
collection-control API (`__itt_resume()` / `__itt_pause()`, `profiling.py:390`) and declares
`ittnotify.h` as an include. This is the *same category* as NVTX, Tracy, and **TracR**: emit calls to an
external tracer instead of hand-rolled timers. Devito thus already demonstrates both instrumentation
families in one codebase (see §4).

### 3.7 Devito already separates compute from communication (and times async correctly)

Two mechanisms in Devito are the direct precedent for the compute / **copy** / **communication**
decomposition our plan needs (developed in [05](05-benchmarking-compute-comm-copy.md)):

- **Selecting *which* node types to instrument.** `track_subsections` (`instrument.py:30`) uses a
  `name_mapper` (`instrument.py:43`) that maps IR node classes to region names — `HaloUpdateCall`,
  `HaloWaitCall`, `RemainderCall`, `ComputeCall`, `BusyWait` — and a `verbosity_mapper` (`instrument.py:53`)
  that decides which of those become their own timed sub-Sections at verbosity 0/1/2. Devito **explicitly
  distinguishes communication nodes (MPI halo exchange) from compute nodes and busy-waits, and lets you
  select them by type and verbosity.** That is exactly the "select which IR parts — loops, collectives,
  copies" capability, and the model for our **region selector**
  ([05 §3](05-benchmarking-compute-comm-copy.md)).

- **Timing async data movement *correctly*.** `sync_sections` (`instrument.py:125`) walks every
  `TimedList` and, where a region uses an async device queue and isn't already a busy-wait, appends
  barrier/wait calls so the timer captures **true device-execution time, not just kernel-launch time**.
  This is the overlap-measurement problem in miniature: without the barrier you time the launch and miss
  the copy/comm — the same issue our plan faces when checking whether copies and collectives are hidden
  behind compute ([05 §4–5](05-benchmarking-compute-comm-copy.md)).

So Devito is not merely a compute profiler that happens to run under MPI — it is a worked example of
*category-aware, type-selective* instrumentation that separately accounts for compute, communication, and
async device time.

---

## 4. A taxonomy — how everyone does it

Instrumentation approaches split along two axes: **what is injected** and **what selects the region**.

### Family A — inject *hand-rolled timers* that write to a shared struct
Devito `basic`/`advanced` (`gettimeofday` → `struct profiler`). Exact performance model, self-contained,
no external tool. Downside: coarse (wall time per region only).

### Family B — inject *external tracer marker-API calls*
Emit push/pop (or resume/pause) into a third-party tracer that owns buffering, timestamps, and
visualization. This is the family the big plan lives in:

| Tool | Marker call | Model | Target |
| --- | --- | --- | --- |
| Intel **ittnotify** (Devito Advisor) | `__itt_resume` / `__itt_pause` | resume/pause | Intel Advisor/VTune |
| **NVTX** | `nvtxRangePushA` / `nvtxRangePop` | explicit push/pop | NVIDIA Nsight |
| **Tracy** | `ZoneScopedN("…")` | RAII scope | host CPU |
| **Perfetto SDK** | `TRACE_EVENT("cat","name")` | RAII scope | host / Chrome trace |
| **Kokkos** | `Kokkos::Profiling::pushRegion(name)` / `popRegion` | explicit push/pop | pluggable tools (kokkosp) |
| **TracR** (our target) | `INSTRUMENTATION_MARK_SET/RESET` | explicit SET/RESET on a channel | Ascend AICPU/AICore + host |

### Orthogonal axis — what selects the region
- **Uniform / automatic** — instrument *every* function. LLVM/GCC `-finstrument-functions`
  (`__cyg_profile_func_enter/exit`), LLVM **XRay** (`-fxray-instrument`, patchable nop "sleds" — near-zero
  cost until enabled), PGO counters (`-fprofile-instr-generate`). No IR-level region knowledge; blanket
  coverage.
- **Structural / region-based** — instrument compiler-known regions. Devito `Section`s, Kokkos parallel
  regions, and **PyPTO scopes/tasks/phases**. This is the sweet spot for a DSL compiler: it already knows
  the meaningful boundaries.
- **Sampling (not injection)** — e.g. **Halide**'s `profile` target feature spawns a background thread
  that samples which `Func` is running. No per-region markers; statistical. A useful contrast: cheap and
  unintrusive, but lossy and coarse.

### A third, separate thing — instrumenting the *compiler itself*
`PassInstrumentation` in **MLIR** (`runBeforePass`/`runAfterPass`), **TVM**'s `PassInstrument`, Devito's
`@iet_pass` pass-timing, and **PyPTO's `CallbackInstrument`** ([00](00-pypto-profiling.md) §2, §8) all
time the *passes*, not the generated program. Same visitor-hook idea, different subject. Worth not
conflating with program instrumentation.

---

## 5. Where PyPTO + TracR sit — and what we borrow

PyPTO/TracR is **Family B, structural, on a device target**. Mapping Devito's mechanism onto our stack:

| Devito | PyPTO / TracR analogue |
| --- | --- |
| `Section` (neutral region marker, named `section0…`) | `PTO2_SCOPE()` blocks, task submissions, orchestration phases — structural regions PyPTO already has ([00](00-pypto-profiling.md) §9) |
| `attach_section` / `reuse_section` (region identification, upstream) | region identification would live in the pass/visitor that walks these nodes — *to be built* |
| `instrument_sections` wrapping a `Section` in a `TimedList` | a pass emitting `MARK_SET/RESET` at the `EmitIndentedLine` choke point in `orchestration_codegen.cpp` |
| `Timer` = ctypes `struct profiler`, threaded by `byref`, read back via ctypes | TracR per-thread ring buffer in **device memory**, downloaded by the host after the run ([02](02-tracr-in-simpler-pr1173.md) §3, §9) |
| `configuration['profiling']` levels (basic/advanced/verbosity) | a `ProfilingLevel` policy (None/Coarse/Fine) mapped to `L2SwimlaneLevel` ([04](04-codegen-instrumentation-blueprint.md) §1) |
| `Target.instrument` backend indirection | a `MarkerBackend` switch (TracR / Tracy / NVTX — [04](04-codegen-instrumentation-blueprint.md) §2) |
| Advisor mode (`__itt_resume`/`pause`) — external tracer | TracR markers — the same "call an external tracer" idea |

**The differences that matter for us:**

- **Explicit-pop, not RAII/struct.** TracR is SET/RESET (like NVTX/ittnotify), so the generated code must
  guarantee a close on every exit path — a constraint Devito avoids because a `TimedList` header/footer
  wraps a whole tree node. The pass must emit matched pairs at structural boundaries (or a RAII guard).
  See [04](04-codegen-instrumentation-blueprint.md) §5.
- **Device collector, not a host struct.** Principle 3 (first-class collector, results read back) holds,
  but the "shared struct read via ctypes" becomes "device ring buffer copied back over the PCIe/HBM
  boundary." PR #1173 already implements that half ([02](02-tracr-in-simpler-pr1173.md)).
- **Identity in a tag, metric off-line.** Devito bakes op-counts symbolically for a roofline; TracR
  instead carries an integer `extraId` (e.g. `func_id`) and reconstructs names/metrics in
  post-processing. Different metric philosophy, same "don't compute strings/metrics on the hot path."

**What we borrow wholesale:** region identification upstream and structural (treat scopes/tasks/phases as
first-class, neutral markers); a late, policy-gated emission pass that only wraps those markers; the
collector as a first-class object whose results are read back after the run; and the discipline of
region-granularity placement so the hot path stays a single cheap append.

Next: [04-codegen-instrumentation-blueprint.md](04-codegen-instrumentation-blueprint.md) turns these
principles into the concrete emission pattern for our generator.
