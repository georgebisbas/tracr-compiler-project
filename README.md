# tracr-compiler-project

Resources for the TracR profiling project — a new (better) version of the current
[TraCR](https://github.com/huawei-csl/TracR).

## The big plan

Have **PyPTO generate TracR instrumentation markers via a compiler pass**, so any compiled program can
be profiled on Ascend hardware without hand-editing generated code. Today the markers are placed by hand
(see [simpler PR #1173](https://github.com/hw-native-sys/simpler/pull/1173)); the goal is to automate
that placement as a policy-gated codegen pass — covering **compute, data copy-in/out, and
communication**, with the ability to **select which IR regions** (loops, scopes, dispatches, copies,
collectives) to trace, single- and multi-node.

## Context docs

Start with **[docs/README.md](docs/README.md)** — an ordered set of context docs that goes from how we
profile today, through the TracR contract and its first hand-written integration, to the compiler-level
principles and a concrete blueprint for the pass:

- [00 — How PyPTO profiles today](docs/00-pypto-profiling.md)
- [01 — What TracR needs to profile code](docs/01-tracr-profiling.md)
- [02 — TracR in Simpler: how PR #1173 works](docs/02-tracr-in-simpler-pr1173.md)
- [03 — Compiler-level instrumentation principles (Devito exemplar)](docs/03-ir-instrumentation-principles.md)
- [04 — A code-generator instrumentation blueprint](docs/04-codegen-instrumentation-blueprint.md)
- [05 — Benchmarking compute, communication, and data movement](docs/05-benchmarking-compute-comm-copy.md)
