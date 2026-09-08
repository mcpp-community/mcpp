# 09 — Heterogeneous builds: one lesson, several programming models

Each subdirectory computes the same thing — `2.0 * [1,2,3,4] + [10,20,30,40]`,
printed as `12 24 36 48` — on a device, and the same thing on the CPU when
there is no device. They are not several lessons. They are one structure
instantiated several times, and reading any two of them shows which parts
belong to the structure and which to a vendor.

| directory | model | device compiler | what it adds to the structure |
|---|---|---|---|
| [`boundary/`](boundary/) | none — a C island | none | the boundary alone: a generated module the consumer imports, with no seam and no header anywhere in the project |
| [`cuda/`](cuda/) | CUDA | the project's own clang (`-x cuda`), or nvcc | the driver relation stated as a fact and a floor, and two pairings nvcc cannot have |
| [`vulkan/`](vulkan/) | Vulkan compute | glslang or shaderc, to SPIR-V | a device output that is a header rather than an object, and one artifact that runs on three devices |
| [`sycl/`](sycl/) | SYCL | the dpcpp payload's clang | a second compiler with its own standard library, and a chained action for the device link |
| [`hip/`](hip/) | HIP | the project's own clang, NVIDIA platform | a programming model that is a header layer over another model's runtime |
| [`cann/`](cann/) | Ascend C | the toolkit's BiSheng (`-x asc`) | a device object for hardware nobody in this repository has, and a host half that declines cleanly |
| [`multi-backend/`](multi-backend/) | CUDA **and** Vulkan | both of the above | the other shape: backends that are additive rather than a seam, chosen at run time |

Start with `boundary/`, which needs no device and isolates the interface
between an island and the C++ side. Then `cuda/`, which adds the device compiler
and a seam over that boundary. The four beside `cuda/` assume it, and
`multi-backend/` assumes two of them.

`boundary/` and the five model directories are one shape — a **seam**: exactly
one implementation exists in the artifact and the choice was made at build time.
`multi-backend/` is the other — several implementations in one artifact, chosen
when the program runs. A program can take either; a library that is compiled
once and consumed by people whose machines differ can only take the second.

## The structure

**An island.** The device translation units are compiled by a compiler that
does not accept C++20 modules, so the engine classifies them as device sources:
never scanned for imports, never producing a BMI, never offered to the C++
compiler. `SourceKind::Device` is a role in the graph, and the criterion for it
is which compiler consumes the file, not which dialect the file is written in.

**A seam.** The island's interface is `extern "C"` and free of standard-library
types, because the two sides do not share a C++ ABI. A module — `app.saxpy` in
each of the five model directories — turns that C interface back into a C++ one.
`boundary/` is the one that stops before this step, so the two can be read side
by side. The seam is the single
place where the implementation underneath becomes a different model or a CPU
loop, and the single place a `cfg(accelerator = ...)` section has to apply.
Without it, every importer would be backend-specific.

The seam carries one more entry point, `saxpy_device_name()`, for a reason
worth stating: every island and every CPU fallback here produces the same four
numbers. Without a name in the output, a run that silently fell back to
the CPU is indistinguishable from a run on a device -- in a set of examples
whose whole subject is which device ran the computation. Each backend fills it
in with the device it used, the CPU file fills it in with `cpu`, and both do so
only after a successful call: a device run that did not happen has no device to
report. `main` prints it after the result, never before.

**A constrained glob.** The device axis is written once, in the manifest, and
the source set follows it:

```toml
[build]
accel   = "cuda12.9+{sm_89} ptx>=89"
sources = [
  "src/*.cppm",
  "src/*.cpp",
  { glob = "src/kernels/**/*.cu", accel = "cuda12.9+{sm_89}" },
]

[target.'cfg(not(accelerator = "cuda"))'.build]
sources = ["src/cpu/*.cpp"]
```

`mcpp build` compiles the device unit; `mcpp build --no-accel` compiles the CPU
file instead. Neither is excluded by a hand-written condition: the glob states
the constraint, and an `--accel` that does not cover it is refused before
anything is compiled. The two variants land in different artifact directories
because the device axis is part of the build's identity, so alternating between
them does not rebuild from scratch.

**A boundary, generated or written.** The `extern "C"` declarations under the
seam are each entry point's signature stated a second time, at the one place a
disagreement is invisible: C language linkage does not mangle, and an island and
its host fallback are never in one link. `mcpp.tools.island` reads the marked
declarations out of both implementations and writes that header and a module
over it, so the signatures exist once. `boundary/`, `cuda/` and `sycl/` take
that route; `hip/`, `vulkan/` and `cann/` keep the header written by hand, so
the two can be read side by side. [`boundary/`](boundary/) states what each
rung costs.

**A rule package, which brings its own environment.** Every vendor spelling —
`--cuda-gpu-arch`, `-gencode`, `--target-env`, `-fsycl-targets`, `-fsycl-link`,
`--cce-aicore-arch` — lives in `mcpp:plugins`, a package the project depends on
and selects features from. The engine owns the graph, the artifact's identity
and the accelerator axis; it holds no vendor name. A unit test refuses vendor
probes in `src/`.

The rule also declares the **payloads** it needs, under the feature that
selects it and the accelerator it serves, so a project writes one edge and no
`[xlings.workspace]` block:

```toml
[build-dependencies.mcpp]
plugins = { version = "0.4.0", features = ["rules-cuda", "tools-island"], host-module = true }
```

`multi-backend/` is the one example here that also pins a version, and it does
so to demonstrate the override: the rule owns "which package, and no older than
what", the project owns "and exactly this one". One version is installed either
way — see *One package, one version* in `docs/04-mcpp-toml.md`.

## The layers underneath

A device build reaches hardware through four layers, and each of the model
directories uses all of them. Confusing two of them is the most common way a
working build stops working on another machine.

| layer | owns | example |
|---|---|---|
| engine | the graph, the identity, the axis | mcpp itself |
| rule package | the spelling of one model | `mcpp.rules.cuda` in `mcpp:plugins` |
| payload | the binaries, versioned by the rule and overridable by the project | `xim:cuda-nvcc`, `xim:dpcpp`, `xim:glslang` |
| adapter | a built artifact's reach to something the host owns | `compat:cuda-driver`, `compat:vulkan-runtime`, `compat:sycl-runtime` |

The adapter layer exists for one reason. An mcpp-built program runs under
mcpp's own loader, which does not consult `/usr/lib`, so a bare-soname `dlopen`
from inside the program finds nothing. Anything the host must supply — the
NVIDIA driver, a Vulkan ICD — is reached by an index package that puts a
directory on the artifact's runtime search path. A project declares it as an
ordinary dependency and does not otherwise think about it.

Everything else is a payload. Which package and how old it may be belongs to
the rule; *exactly which version* is the project's to override and nobody's to
discover from the machine. No example here contains an absolute path or reads
the host's toolkit, and only `multi-backend/` names a payload version at all.

## Running them

Each subdirectory's `app/` is a project:

```
cd examples/09-heterogeneous/cuda/app
mcpp run                 # on the device
mcpp run --no-accel      # the same numbers, on the CPU
```

The CPU variant needs no device and no driver, and is what a machine without
hardware runs. `vulkan/` additionally runs on a CPU *device* — Mesa's lavapipe,
delivered as a payload — which is a third case and is the one CI exercises.

## What is not here

The examples show the shape mcpp implements: device code in separate
translation units, compiled by a separate compiler, linked into an ordinary
program. Models that compile a whole target with an offloading compiler —
OpenMP `target`, C++ parallel algorithms with a device backend — have no
separable island and are not reached this way. `docs/42-heterogeneous-builds.md`
states the distinction and why it is a property of the model rather than a gap
in the tool.
