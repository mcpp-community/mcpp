# 09 — Heterogeneous builds: one lesson, four programming models

Four subdirectories compute the same thing — `2.0 * [1,2,3,4] + [10,20,30,40]`,
printed as `12 24 36 48` — on a device, and the same thing on the CPU when
there is no device. They are not four lessons. They are one structure
instantiated four times, and reading any two of them shows which parts belong
to the structure and which to a vendor.

| directory | model | device compiler | what it adds to the structure |
|---|---|---|---|
| [`cuda/`](cuda/) | CUDA | the project's own clang (`-x cuda`), or nvcc | the driver relation stated as a fact and a floor, and two pairings nvcc cannot have |
| [`vulkan/`](vulkan/) | Vulkan compute | glslang or shaderc, to SPIR-V | a device output that is a header rather than an object, and one artifact that runs on three devices |
| [`sycl/`](sycl/) | SYCL | the dpcpp payload's clang | a second compiler with its own standard library, and a chained action for the device link |
| [`hip/`](hip/) | HIP | the project's own clang, NVIDIA platform | a programming model that is a header layer over another model's runtime |

Start with `cuda/`. The other three assume it.

## The structure

**An island.** The device translation units are compiled by a compiler that
does not accept C++20 modules, so the engine classifies them as device sources:
never scanned for imports, never producing a BMI, never offered to the C++
compiler. `SourceKind::Device` is a role in the graph, and the criterion for it
is which compiler consumes the file, not which dialect the file is written in.

**A seam.** The island's interface is `extern "C"` and free of standard-library
types, because the two sides do not share a C++ ABI. A module — `app.saxpy` in
all four — turns that C interface back into a C++ one. The seam is the single
place where the implementation underneath becomes a different model or a CPU
loop, and the single place a `cfg(accelerator = ...)` section has to apply.
Without it, every importer would be backend-specific.

The seam carries one more entry point, `saxpy_device_name()`, for a reason
worth stating: all four islands and all four CPU fallbacks produce the same
four numbers. Without a name in the output, a run that silently fell back to
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

**A rule package.** Every vendor spelling — `--cuda-gpu-arch`, `-gencode`,
`--target-env`, `-fsycl-targets`, `-fsycl-link` — lives in `mcpp:plugins`, a
package the project depends on and selects features from. The engine owns the
graph, the artifact's identity and the accelerator axis; it holds no vendor
name. A unit test refuses vendor probes in `src/`.

## The layers underneath

A device build reaches hardware through four layers, and each of the four
examples uses all of them. Confusing two of them is the most common way a
working build stops working on another machine.

| layer | owns | example |
|---|---|---|
| engine | the graph, the identity, the axis | mcpp itself |
| rule package | the spelling of one model | `mcpp.rules.cuda` in `mcpp:plugins` |
| payload | the binaries, versioned by the project | `xim:cuda-nvcc`, `xim:dpcpp`, `xim:glslang` |
| adapter | a built artifact's reach to something the host owns | `compat:cuda-driver`, `compat:vulkan-runtime`, `compat:sycl-runtime` |

The adapter layer exists for one reason. An mcpp-built program runs under
mcpp's own loader, which does not consult `/usr/lib`, so a bare-soname `dlopen`
from inside the program finds nothing. Anything the host must supply — the
NVIDIA driver, a Vulkan ICD — is reached by an index package that puts a
directory on the artifact's runtime search path. A project declares it as an
ordinary dependency and does not otherwise think about it.

Everything else is a payload, so the version is the project's choice rather
than the machine's, and no example here contains an absolute path or reads the
host's toolkit.

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
separable island and are not reached this way. `docs/20-heterogeneous-builds.md`
states the distinction and why it is a property of the model rather than a gap
in the tool.
