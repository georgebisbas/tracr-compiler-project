# 04 — A code-generator instrumentation blueprint

> Part of the **tracr-compiler-project** context set. Companion to
> [03-ir-instrumentation-principles.md](03-ir-instrumentation-principles.md) (the *principles*, with
> Devito as the worked example). This doc is the *practical blueprint*: the concrete emission pattern
> for injecting profiling markers from a C++ code generator, adapted to our stack (PyPTO orchestration
> codegen → **TracR**), and generalized to a pluggable backend so the same pass can also target
> **Tracy** (host CPU) or **NVIDIA NVTX** (GPU).
>
> Prerequisites: [00](00-pypto-profiling.md) (where PyPTO emits code), [01](01-tracr-profiling.md)
> (the TracR contract), [02](02-tracr-in-simpler-pr1173.md) (the hand-written reference).

---

## 0. The principle in one sentence

**Automated profiling instrumentation belongs in the emission pass, not the domain logic:** the code
generator injects low-overhead telemetry calls (marker push/pop) around regions it already
understands, gated by a declarative policy, so tracing stays decoupled from *what* is being compiled.

This is the same idea Devito implements as an IET pass (doc 03). Here we express it as an emission-time
pattern you can drop into a string-emitting C++ generator — which is exactly the shape of PyPTO's
`OrchestrationStmtCodegen` ([00](00-pypto-profiling.md) §9).

---

## 1. The declarative policy: a `ProfilingLevel`

Control marker density with a compiler policy, so you never edit generation logic to change what gets
traced — you flip a flag:

```cpp
enum class ProfilingLevel {
    None,     // strip all markers (production / zero-cost)
    Coarse,   // top-level domain stages: layers, pipelines, scopes
    Fine      // fine-grained: per-loop, per-task-dispatch, per-buffer
};

// Level is only ONE of three policy axes. The other two — which cost classes to
// trace, and which IR regions to select — are developed in doc 05; sketched here:
enum class CostClass { Compute, Copy, Comm };      // §see 05: compute / copy-in-out / communication

struct RegionSelector {                            // which IR nodes to wrap (05 §3)
    std::set<NodeKind> nodeTypes;                  // loops, scopes, dispatches, copies, collectives
    int  maxLoopDepth = 1;                         // never wrap the innermost loop
    // + optional cost floor (skip trivial regions) and name/tag allow-list
};

struct GeneratorConfig {
    ProfilingLevel      profLevel  = ProfilingLevel::Coarse;
    std::set<CostClass> categories = {CostClass::Compute};   // add Copy/Comm to benchmark them
    RegionSelector      selector;                            // which regions (empty = level defaults)
    MarkerBackend       backend    = MarkerBackend::TracR;   // see §2
    bool                flows      = false;                  // emit FLOW_* for comm edges (05 §2b)
};
```

### This is not new to our stack — map it to knobs that already exist

| `ProfilingLevel` | PyPTO / Simpler analogue ([00](00-pypto-profiling.md) §3) | What the generator emits |
| --- | --- | --- |
| `None` | `ENABLE_TRACR` undefined → zero-cost stubs; all DFX flags off | nothing (macros vanish at preprocessing) |
| `Coarse` | ≈ `L2SwimlaneLevel` 3–4 (`SCHED_PHASES` / `ORCH_PHASES`) | markers around **scopes + orchestration/scheduler phases** (`Orchestrating`, `Scheduling`, `PTO2_SCOPE_`, `Phase1–4`) |
| `Fine` | ≈ `L2SwimlaneLevel` 1–2 + PMU | the above **plus per-task-dispatch** markers (`Running_Task_Single/Pair` with `func_id`) on per-core lanes |

The point: the marker set PR #1173 placed by hand ([02](02-tracr-in-simpler-pr1173.md) §5) is
naturally partitioned into a `Coarse` tier (phases/scopes) and a `Fine` tier (per-task). A policy-driven
generator just chooses which tiers to emit.

### Level is one axis of three

A production profiler needs more than density. The full policy is **three orthogonal axes**:

1. **Level** (above) — how *dense* the markers are within a traced region.
2. **Cost class** — *what* to trace: **compute**, **data copy-in/out**, **communication**. A compute-only
   profiler is half a tool; the wall clock is a race between all three, and the wins are in *overlapping*
   them. Each class gets its own marker types, lanes, and (for comms) flow arrows.
3. **Region selector** — *which* IR nodes to wrap: loops (and which loop level), scopes, task dispatches,
   copy ops, collectives — chosen by a predicate over the IR, not an all-or-nothing switch.

Axes 2 and 3 are the subject of **[05-benchmarking-compute-comm-copy.md](05-benchmarking-compute-comm-copy.md)**,
which develops the compute/copy/comm decomposition, the IR region selector, and single- vs multi-node
benchmarking. This doc's emission pattern is the vehicle; doc 05 is what you point it at.

---

## 2. A pluggable marker backend

The push/pop shape is backend-specific; the *decision to emit* is not. Abstract the backend so one pass
serves all three:

| Backend | Target | Marker model | Push | Pop | Static-string story |
| --- | --- | --- | --- | --- | --- |
| **TracR** *(our target)* | Ascend AICPU + AICore, host | **explicit SET/RESET** on a channel | `INSTRUMENTATION_MARK_SET(ch, EventId, extra)` | `INSTRUMENTATION_MARK_RESET(ch)` | best: label is a pre-registered **`uint16_t` eventId**; the string lives only in `metadata.json` |
| **Tracy** | Host CPU threads | **RAII scope** | `{ ZoneScopedN("name");` | `}` | static literal baked into a source-location struct |
| **NVTX** | NVIDIA GPU timeline (Nsight) | **explicit push/pop** | `nvtxRangePushA("name");` | `nvtxRangePop();` | static literal argument |

Two structural facts drive everything below:

- **TracR is explicit-pop, like NVTX — not RAII, like Tracy.** So a TracR-emitting generator follows the
  *flat push/pop* shape (no injected braces), and it must guarantee a `RESET` on **every** exit path of
  the region. (PR #1173 does exactly this: it emits `INSTRUMENTATION_MARK_RESET` at the single function
  exit right before `TRACR_FINALIZE`, and pairs the per-core dispatch `SET` with a `RESET` at task
  completion — [02](02-tracr-in-simpler-pr1173.md) §7–8.)
- **TracR already solves the "no dynamic string on the hot path" problem better than Tracy/NVTX.** Its
  hot-path record carries a 2-byte `eventId`, not a string; the human label is registered once and
  stored in metadata ([01](01-tracr-profiling.md) §3, §5). So our generator emits an **enum**, never a
  formatted string.

---

## 3. The generator pattern

### 3a. The canonical string-emitting form

This is the reusable pattern (backend-agnostic push/pop, policy-gated, indentation-aware):

```cpp
class CppCodeGenerator {
    GeneratorConfig config;
    std::stringstream out;
    int indent = 0;
    void emitIndent() { out << std::string(indent * 4, ' '); }

    void emitProfilePush(const std::string& zone, uint32_t extra = 0) {
        if (config.profLevel == ProfilingLevel::None) return;
        emitIndent();
        switch (config.backend) {
            case MarkerBackend::TracR:  // explicit SET on the current thread's channel
                out << "INSTRUMENTATION_MARK_SET(g_TraCR_thread_idx, " << zone
                    << ", " << extra << ");\n";                      // 'zone' is an eventId enum
                break;
            case MarkerBackend::NVTX:
                out << "nvtxRangePushA(\"" << zone << "\");\n";
                break;
            case MarkerBackend::Tracy:                                // RAII: open a scope block
                out << "{\n"; indent++; emitIndent();
                out << "ZoneScopedN(\"" << zone << "\");\n";
                break;
        }
    }

    void emitProfilePop() {
        if (config.profLevel == ProfilingLevel::None) return;
        switch (config.backend) {
            case MarkerBackend::TracR:
                emitIndent(); out << "INSTRUMENTATION_MARK_RESET(g_TraCR_thread_idx);\n";
                break;
            case MarkerBackend::NVTX:
                emitIndent(); out << "nvtxRangePop();\n";
                break;
            case MarkerBackend::Tracy:
                indent--; emitIndent(); out << "}\n";                 // close the RAII scope
                break;
        }
    }
public:
    void compileScope(const std::string& zoneEvent, uint32_t id, const std::string& bounds) {
        emitProfilePush(zoneEvent, id);
        emitIndent(); out << "for (int i = 0; i < " << bounds << "; ++i) {\n";
        indent++; emitIndent(); out << "compute_payload(i);\n";
        indent--; emitIndent(); out << "}\n";
        emitProfilePop();
    }
};
```

*(Adapted from the canonical Tracy/NVTX blueprint; the TracR branch and the `extra`/eventId argument are
the additions that make it fit our stack.)*

### 3b. Where this lives in PyPTO

You don't write a new `CppCodeGenerator` — PyPTO already has one. The mapping:

| Blueprint element | PyPTO reality ([00](00-pypto-profiling.md) §8–9) |
| --- | --- |
| `out << ...; emitIndent()` | `CodeEmitter::EmitLine()` / `EmitIndentedLine()` in `orchestration_codegen.cpp` |
| `CppCodeGenerator` | `OrchestrationStmtCodegen : public CodegenBase` |
| `compileScope(...)` visiting a loop | the visitor methods that walk `PTO2_SCOPE()` blocks and task submissions |
| `GeneratorConfig.profLevel` | a new option on `RunConfig` / `ir.compile(...)`, sibling to the existing DFX flags |
| `emitProfilePush/Pop` | new helper methods on the codegen that call `EmitIndentedLine(...)` with the TracR call string |

So the concrete work is: add `emitProfilePush/Pop` helpers to the orchestration codegen and call them at
the scope/task boundaries the visitor already traverses. This is the compiler-pass version of the
by-hand edits in `paged_attention_orch.cpp` ([02](02-tracr-in-simpler-pr1173.md) §8).

---

## 4. Generated output

For a `Coarse`-level scope named `MatMul_Layer1`:

**TracR** *(the real target — flat SET/RESET, eventId enum, no runtime string):*

```cpp
INSTRUMENTATION_MARK_SET(g_TraCR_thread_idx, MatMul_Layer1, /*extraId=*/1);
for (int i = 0; i < 1024; ++i) {
    compute_payload(i);
}
INSTRUMENTATION_MARK_RESET(g_TraCR_thread_idx);
```

This is byte-for-byte the shape PR #1173 hand-wrote for `PTO2_SCOPE_`
([02](02-tracr-in-simpler-pr1173.md) §5–8), with `MatMul_Layer1` being an `enum MarkerType` value from
the `MARKER_TYPES` X-macro (so the string only appears in `metadata.json`).

**Tracy** *(host CPU, RAII — the scope closes itself even on exception / early return):*

```cpp
{
    ZoneScopedN("MatMul_Layer1");
    for (int i = 0; i < 1024; ++i) {
        compute_payload(i);
    }
}
```

**NVTX** *(NVIDIA GPU timeline — flat push/pop):*

```cpp
nvtxRangePushA("MatMul_Layer1");
for (int i = 0; i < 1024; ++i) {
    compute_payload(i);
}
nvtxRangePop();
```

Same DSL, same pass, three timelines — selected by one config flag.

---

## 5. Best practices, annotated for our stack

The canonical guidance, with how PyPTO/TracR already satisfies (or must satisfy) each:

1. **RAII vs. explicit pop.** RAII (`{ ZoneScoped; … }`) is exception/early-return safe; explicit
   push/pop is not. **TracR is explicit-pop**, so the generator must emit a `RESET` on every exit path.
   Two safe strategies:
   - Emit markers only at **structural boundaries the compiler controls** (a `PTO2_SCOPE()` block, a
     task dispatch) where entry and exit are matched by construction — this is what PR #1173 does.
   - Or generate a tiny **RAII guard** wrapper around `MARK_SET`/`MARK_RESET` and emit that instead, to
     survive early `return`/`break` inside a scope. (Devito sidesteps this entirely because its
     `TimedList` header/footer wrap a whole tree node — doc 03.)

2. **No dynamic strings on the hot path.** Never emit `ZoneScopedN("Loop_" + std::to_string(i))`. Bake
   **static literals**. TracR goes further: the hot-path call takes a **`uint16_t` eventId**; the label
   is registered once and stored in metadata ([01](01-tracr-profiling.md) §3). Our generator emits the
   enum, and uses the free `extraId` field for the dynamic part (iteration index, batch, `func_id`) —
   which is an integer, not a string.

3. **Header injection.** The generator must emit the backend's include. **Already handled in our stack:**
   `kernel_compiler.py` adds the TracR include dirs and, under `BUILD_TRACR=ON`, injects
   `-DENABLE_TRACR -DTRACR_DISABLE_FLUSH -DUSE_HW_COUNTER`; the generated orchestration units
   `#include <tracr/tracr.hpp>` and `<tracr_simpler_markers.hpp>` ([02](02-tracr-in-simpler-pr1173.md)
   §4). A backend-agnostic generator would switch this to `<tracy/Tracy.hpp>` or `<nvtx3/nvToolsExt.h>`
   per config.

4. **Place markers at the right granularity (added from our own findings).**
   - **Wrap loop *nests*, not the innermost iteration** — one push/pop per region per step, never per
     grid point (Devito's rule, doc 03; and why `Fine` should still mark *dispatches*, not inner ops).
   - **Keep the collector's cost outside the measured region.** This is the entire thesis of PR #1173:
     TracR appends to an in-memory ring on the hot path and dumps once at the end, so the +overhead
     lands *outside* the scheduling loop — unlike the L2 swimlane's +9% in-loop
     ([02](02-tracr-in-simpler-pr1173.md) §2).
   - **Carry identity in `extraId`.** The generator should pass the identifier it already knows (kernel
     `func_id`, scope index) so post-processing can name regions — TracR maps `func_id → kernel name`
     via the run's `kernel_config.py` ([02](02-tracr-in-simpler-pr1173.md) §6).

---

## 6. Answering the blueprint's tailoring questions — for us

The canonical write-up ends by asking two questions; here are our answers, which pin the design:

- **"Python, C++, or another language for the backend?"** → **Both, split by layer.** PyPTO's codegen is
  **C++** (`orchestration_codegen.cpp` emits C++ source strings via `CodeEmitter`), but it is *driven*
  from **Python** (`pto_backend.py`, the pass manager, `RunConfig`). So the *policy* (`ProfilingLevel`)
  is a Python-level option; the *emission* is C++ appending marker call strings. The marker registry is
  a C++ X-macro today ([02](02-tracr-in-simpler-pr1173.md) §5) — a generator could emit/extend it.

- **"Multiple host CPU threads, or heterogeneous devices?"** → **Heterogeneous devices.** We dispatch to
  **Ascend AICPU** (orchestration + scheduler threads) and **AICore** (AICube/AIVector units) — *not*
  CUDA/Vulkan. That is why **TracR is the real backend** (device HW-counter timestamps, device-collect /
  host-download, multi-device today — [02](02-tracr-in-simpler-pr1173.md) §3), and why the channel model
  maps AICPU threads and AICore cores to distinct lanes ([02](02-tracr-in-simpler-pr1173.md) §6). Tracy
  would fit the **host** runtime; NVTX only applies on NVIDIA GPUs, so for us it is illustrative of the
  identical push/pop pattern, not a runtime target.

---

## 7. Minimal end-to-end design for the PyPTO TracR pass

Bringing the blueprint together with the principles from doc 03:

1. **Policy.** Add a `ProfilingConfig` to `RunConfig` / `ir.compile(...)` (Python), plumbed to codegen
   like the existing DFX flags ([00](00-pypto-profiling.md) §3): the three axes — `level`, `categories`
   (compute/copy/comm), `selector` (which IR nodes) — plus `backend` and `flows`
   ([05 §7](05-benchmarking-compute-comm-copy.md)).
2. **Region identification + selection (the "Sections").** Our critical zones already exist structurally,
   like Devito's `Section`s: `PTO2_SCOPE()` blocks, task submissions, copy ops, collectives, and the
   orchestration-entry phases. The pass/visitor identifies these node kinds and applies the `selector`
   predicate ([05 §3](05-benchmarking-compute-comm-copy.md)) so only the chosen regions are wrapped — no
   new region analysis needed.
3. **Registry.** Assign a stable `eventId` per region kind (extend the `MARKER_TYPES` X-macro, or emit a
   generated one). Labels are static; the hot path uses the integer.
4. **Emission.** At each region boundary the visitor already reaches, call `emitProfilePush/Pop`, which
   append `INSTRUMENTATION_MARK_SET/RESET(...)` via `EmitIndentedLine` — gated by `ProfilingLevel` and,
   for `Fine`, also wrapping task dispatches with the `func_id` in `extraId`.
5. **Backend abstraction (optional, future).** Keep the push/pop shape behind a `MarkerBackend` switch so
   the same pass can emit Tracy for a host build or NVTX if a CUDA target ever appears.
6. **Runtime stays put.** Channel assignment, `TRACR_START/FINALIZE`, device alloc/download, and
   post-processing are **runtime plumbing that PR #1173 already provides**
   ([02](02-tracr-in-simpler-pr1173.md) §12) — the pass only emits the marker calls those pieces expect.

The result is Devito's architecture (doc 03) realized on our stack: *where to measure* is structural and
upstream; *how to measure* is a late, policy-gated emission concern; the collector is a first-class
runtime object; and the hot path stays a single cheap append.
