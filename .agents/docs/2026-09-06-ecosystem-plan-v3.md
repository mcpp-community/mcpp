# The heterogeneous ecosystem, v3: what is built, what is not, and what decides each

> Supersedes `2026-09-05-multi-device-ecosystem-design.md` (v1) and
> `2026-09-05-heterogeneous-build-ecosystem-design-v2.md` (v2) as the standing
> plan. Those two stay as the record of how rounds 1 to 4 were decided; this
> document is the one to read before starting round 5.
>
> Every number below was measured against the published indices on 2026-09-06,
> not read off a task table. Section 1 says why that distinction is not
> pedantry.

---

## 1. Read the index, not the checklist

The v1 task table records `pocl` and `mesa-lavapipe` as open. Both have been
published for a day. Two rows of the v2 table read `open` for examples that
had shipped and been verified, because the files were renamed and the rows were
not. In the same round, four separate CHECKS passed while measuring the wrong
object.

So the standing rule for this plan, and the reason it opens with it:

**A status is a claim about the ecosystem, and the ecosystem is the only thing
that can settle it.** Before treating any row below as done, ask the index. The
verification script `2026-09-06-round4-verify.sh` is the executable form of
that question for what exists today.

## 2. The invariant, unchanged

Nothing reaches the host except proprietary vendor userspace in ABI lockstep
with a kernel module, and even that is linked rather than redistributed.

Round 4 added one clause it turned out to need. The invariant is about what a
build **reaches**, and a build reaches things it never names: an implicit
include search, a compiler's default C++ standard library, a soname resolved
from a loader cache. A check that reads only what a build **says** cannot see
the difference. See section 7.

## 3. The layers, and the one round 4 made explicit

| layer | owns | example |
|---|---|---|
| engine | the graph: the accelerator axis, constrained globs, action edges, fingerprints, artifact identity | `mcpp` |
| rule package | the spelling: which compiler, which flags, which probe | `mcpp.rules.sycl` |
| payload | the binaries, and their internal reachability | `xim:dpcpp` |
| **adapter** | **the runtime reach: what an artifact must find through the loader** | `compat.sycl-runtime` |
| sentinel | the irreducible host link | `xim:libcuda-host-link` |

The adapter layer existed before round 4 but was read as "a farm of symlinks".
It is not: it is the layer that decides **what a consumer must know**. When
`compat.sycl-runtime` gained the driver hop, a SYCL project stopped having to
declare CUDA. That is a boundary decision, not plumbing, and the test for it is
one sentence: *does a consumer have to name something that is not its own
concern?*

## 4. Measured state, 2026-09-06

### Built and verified

| | state |
|---|---|
| engine | accelerator axis, constrained globs with `accel`, `mcpp::action` roles, chained actions, probe channel (`fact`/`floor`), device objects into static libraries, `.sycl`, feature-selected host-module collections |
| rules | `mcpp:plugins` 0.2.0: `rules-cuda`, `rules-hip`, `rules-spirv` (glslang and glslc), `rules-sycl`, `tools-embed` |
| payloads | `cuda-*` (24 components, two lines), `dpcpp`, `hip-nvidia`, `shaderc`, `glslang`, `pocl`, `mesa-lavapipe` |
| adapters | `compat.cuda-driver`, `cudart`, `opencl`, `opencl-runtime`, `vulkan`, `vulkan-runtime`, `sycl-runtime` |
| lanes | CUDA, HIP (NVIDIA platform), SYCL, Vulkan/SPIR-V -- each with an example answering `12 24 36 48` on a device and again with `--no-accel` |
| verification | V3 (sandbox, CN mirror) and V4 (host, RTX 4080): 0 assertions failed each |

### Not built

| | size | what decides it |
|---|---|---|
| **nine frameworks** (llama.cpp, ncnn, CUTLASS, oneDNN, Kokkos, OpenCV CUDA, FAISS, ONNX Runtime, libtorch) | **the bulk of what remains** | each is work in that project's own `-m` repository, not an index entry. Two of the nine exist in the index at all; none has a device backend built |
| Intel `anv` in the Mesa payload | medium | a libclc and SPIRV-LLVM-Translator chain, then a Mesa rebuild |
| HIP's AMD platform | medium | a ROCm runtime and device library in `xim-pkgindex` |
| aarch64 for the third-class libraries | medium | `xim:glibc`, `gcc-runtime`, `ncurses`, `libxcb` publish no aarch64 asset |
| OpenMP offload, stdpar | not planned | no separable island exists; see docs/20 |
| Metal | not planned | no macOS device to verify against |

**Honest overall figure: the foundation is done and the proof that it carries
weight is not.** Engine, rules, payloads and adapters are essentially complete;
the framework tier is barely started. If the plan is weighted by remaining
effort rather than by row count, frameworks are more than three quarters of it.

## 5. Round 5: the framework tier

The four lanes prove a rule package can drive four compilers. They do not prove
the ecosystem can build something a person would deploy. That is what this tier
is for, and it is deliberately ordered so the first entry is a gate.

| # | project | lane(s) | criterion | why this order |
|---|---|---|---|---|
| F1 | llama.cpp Vulkan | SPIR-V | correct tokens on lavapipe, no GPU | its blocker is gone (`glslc` published) and its shader pipeline is the most mechanical of the nine |
| F2 | llama.cpp CUDA | CUDA | correct tokens on a device | round 3 reached "chain complete, blocked on a payload matrix"; re-measure against the current CCCL lines before assuming that still holds |
| F3 | Kokkos | CUDA, SYCL | its own unit tests pass under both backends from one source | the first entry that exercises TWO lanes on one source, which is the portability claim |
| F4 | oneDNN | SYCL | `benchdnn` on the CUDA backend | the first entry whose upstream build assumes an oneAPI environment rather than a compiler |
| F5-F9 | CUTLASS, OpenCV CUDA, FAISS, ONNX Runtime, libtorch | CUDA | each project's own test | ordered by how much of the build each imposes |

**F1 is the gate and must be finished before F2 starts.** Round 3's T5.1 earned
its place by exposing an engine defect (device objects were dropped from static
libraries) in its first hour. The value of a gate is that it fails early; that
is lost if two run in parallel.

**These do not land in mcpp-index as entries.** `ggml-org.llamacpp` and
`opencv.opencv` are already there; the work is in their `-m` repositories. Plan
the round as PRs to those, not as index edits.

## 6. Decided: group the four device examples

`examples/09-cuda-kernel`, `10-vulkan-compute`, `11-sycl-kernel` and
`12-hip-kernel` are one lesson in four programming models. They share the
kernel, the seam, the CPU fallback and the answer; they differ only in which
compiler the rule drives. Four consecutive numbers in a curriculum say "four
lessons", and a fifth model would say five.

Proposed:

```
examples/09-heterogeneous/
  README.md      the shared lesson: the seam, the constrained glob, the rule package
  cuda/          was 09-cuda-kernel
  vulkan/        was 10-vulkan-compute
  sycl/          was 11-sycl-kernel
  hip/           was 12-hip-kernel
```

**The cost, measured: 62 references across 14 files.** Of those, the ones in
`CHANGELOG.md` and in the dated `.agents/docs` files are HISTORICAL RECORDS --
they say what shipped under a released version, and rewriting them would make
the record state something that was not true at the time. So a rename leaves
dangling paths in the record by construction, which is normal for a record and
should not be repaired.

**Decided: do it, folded into round 5 rather than as a round of its own.** My
recommendation had been to isolate it, on the grounds that churn mixed with
behaviour change makes a regression hard to attribute. The decision is to
combine, and the attribution risk is real, so it is bought down rather than
ignored:

* the move lands as its **own commit**, first, containing no behaviour change;
* V4 runs on that commit before anything else in the round is written, so a
  rename that broke an example is caught while the rename is the only suspect;
* section 7's alignment work rides in the same commit, because it edits the
  same manifests and splitting them would create two churn commits instead of
  one.

**Not recommended: renaming without the shared README.** The grouping is only
worth its churn if the directory explains what the four have in common; four
subdirectories under a bare parent is the same four lessons with a longer path.

## 7. Aligning the documentation with the released ecosystem

A version reference in the documentation is one of two kinds, and treating them
alike is how a sweep like this damages a document.

**A floor marker states when something landed** -- `*(2026.9.5.2+)*`, `### The
probe channel: fact / floor (2026.9.5.2+)`. It is a historical fact. Bumping it
to the current release makes the document lie about its own subject, and most
of the version strings in `docs/` are this kind.

**A current pin states what a project should declare today** -- an example's
`mcpp.toml`, an instruction to a reader. This is what tracks the release.

Measured on 2026-09-06, the pins that are stale:

| where | has | should have | why |
|---|---|---|---|
| examples 09, 10 | `plugins = { version = "0.1.1" }` | `"0.2.0"` | 11 and 12 already pin 0.2.0; a reader comparing four examples sees two answers to one question |
| examples 09, 12 | `compat:cuda-runtime = "2026.09.05"` | `cuda-driver = "2026.09.05"` | that entry is frozen and renamed; its own recipe says "To migrate, change the key to `cuda-driver`". An example is the worst place to demonstrate a superseded spelling |

The criterion for this task is not "no old version string appears" -- that
criterion would delete the floor markers, which is the failure it must avoid.
It is:

* every `mcpp.toml` under `examples/` resolves against the current index, and
* every floor marker still names the release the feature actually landed in.

The second half is checked by not touching them: the sweep edits manifests and
reader instructions, and leaves `(20xx.x.x.x+)` alone.

## 8. The method this round produced, which outlives its features

Eight defects, six in work written for the round, none found by reading code.
Four shared one shape:

* the host-leak check read the command line, and an implicit include search is
  never on it -- so it reported clean on the machine that was leaking;
* the farm check chose a directory out of the store by sorting, and a store
  that has seen two adapter versions holds two farms;
* a payload lookup named one namespace and the payload was installed under the
  other;
* a program that could not start had its loader error attributed to an adapter,
  turning one defect into three reports of which the third named the wrong
  cause.

Round 3 recorded this shape twice already. Recurring four more times suggests
the lesson is not vigilance but mechanism:

> **A check that selects its own object must print which object it selected.**

Section G of the verification script now does, and the line
`note: falling back to the newest farm in the store` is what turned the last of
these from a false pass into a visible one. Apply this to any new check before
trusting its first green.

Two corollaries worth carrying into round 5:

* **Measure the compiler's search list, not its command line.** `clang -v`
  between `#include <...> search starts here` and `End of search list`. And
  make the criterion the C++ standard library rather than the absence of
  `/usr`: mcpp's own compiles leave `/usr/include` on that list as a last
  resort for C headers, so "zero /usr" is stricter than the engine it checks.
* **A payload is reachable, not merely installed.** `xim:dpcpp` passed every
  install check while five of its programs could not start.
