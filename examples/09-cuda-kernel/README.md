# 09 — A CUDA kernel behind a seam module

What this example demonstrates, and what it does not.

## The shape

```
app/
  src/kernels/saxpy.cu    the island: a device translation unit, never scanned,
                          no BMI, and compiled only when the build asks for CUDA
  src/cpu/saxpy.cpp       the same interface implemented for the host, compiled
                          instead when it does not
  include/saxpy/saxpy.h   the island's interface: extern "C", no std types
  src/app.cppm            the seam: a module that turns the C interface back
                          into a C++ one
  src/main.cpp            an ordinary consumer, which imports the seam and
                          never sees the header
  build.mcpp              hands the device sources to the rule package
rules-cuda/               a build-rule package that knows how to compile them
```

Three properties are load-bearing.

**The island is not in the module graph.** No device compiler accepts C++20
modules, so `.cu` is classified as a device translation unit: never scanned for
imports, never producing a BMI. Its header is classified as a header, so
editing one still invalidates the fast path.

**The island's interface is `extern "C"` and free of standard-library types.**
The device unit may be compiled by a compiler mcpp did not choose, so the two
sides do not share a C++ ABI and must not exchange anything that depends on
one. The island also uses no standard library itself, which keeps it from
linking a second copy of the C++ runtime into a program whose own copy came
from mcpp's toolchain.

**The seam exists for backend substitution, not for the module boundary.** It
is the one place where the island underneath becomes a CPU implementation, or
could become HIP, without any consumer of `app.saxpy` changing, and the one
place a `cfg(accelerator = ...)` section has to apply. Remove it and every
importer becomes backend-specific.

## Two builds from one source tree

The device axis is written once, in the manifest, and the source set follows
it:

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

A glob may carry the accelerator it is for. `mcpp build` compiles the `.cu`
and the CPU file is absent; `mcpp build --no-accel` compiles the CPU file and
the `.cu` is absent — not excluded by a hand-written condition, but by the
constraint the glob states. The two land in different artifact directories
because the device axis is part of the build's identity, so switching between
them does not rebuild from scratch.

An `--accel` that does not cover a constrained glob is refused before anything
is compiled, with `accel-mismatch` on the machine-readable channel.

## Where the toolkit comes from

The project names it:

```toml
[xlings.workspace]
"xim:cuda-nvcc"   = "12.9.86"
"xim:cuda-cudart" = "12.9.79"
```

These are payloads, so the version is the project's choice and not the
machine's. The rule package resolves them with `mcpp::xpkg_dir` and builds the
whole invocation from what it finds — the compiler, the include directories
and the library search paths. No path in this example is absolute, and a build
here touches nothing of the host's CUDA:

```
$ mcpp build -v | grep -c '/usr/local/cuda\|/usr/bin/nvcc'
0
```

**The payload's headers have to be named.** nvcc adds
`<its own directory>/../include` by itself, and on the 12.x line that holds
`crt/` but not `cuda_runtime.h`, which lives in the `cuda-cudart` component. An
earlier revision of this rule left it out, and nvcc resolved `cuda_runtime.h`
from `/usr/include` and then read the host's `crt/host_config.h` beside it. The
build failed with the host toolkit's complaint while using the payload's
compiler.

## Two routes, and why the primary one is clang

The rule package compiles the device unit either way:

- **clang** (`-x cuda --cuda-path=<payload>`) is the default and what
  `[toolchain] default = "llvm@22.1.8"` selects. The compiler that builds the
  rest of the project builds the device unit too. There is no second host
  compiler, no host-compiler bound, and no CUDA host header in the way.
- **nvcc** (`-ccbin <host g++>`) is taken when the project's toolchain is GCC.
  It drives a second compiler, and that is where its constraints come from.

`MCPP_EXAMPLE_CUDA_ROUTE=clang|nvcc` overrides the choice, and the rule
declares `rerun_if_env_changed` for it.

Two pairings nvcc cannot have, both stated before the compile rather than
discovered inside it:

- **A host compiler past the bound.** nvcc states a maximum GCC major in its
  own `crt/host_config.h`. The rule reads it, uses the project's toolchain when
  it fits, otherwise a `xim:gcc` payload the project declared for this purpose,
  and otherwise refuses naming the declaration to add. Measured: GCC 16 under
  nvcc 12.9 fails inside GCC's own `<type_traits>` even with
  `-allow-unsupported-compiler` — that escape hatch admits a compiler one step
  past the bound, not a standard library two majors newer.
- **An old toolkit and a new C library.** Toolkit 12.9's
  `crt/math_functions.h` redeclares the C23 functions `cospi`, `sinpi` and
  `rsqrt` for the host without `noexcept`; glibc 2.41 and later declare them
  with it, and since C++17 that is part of the function type. The compile stops
  with six `exception specification is incompatible` errors naming a glibc
  header and a CUDA header, and no decision. The rule reads the C library's
  `bits/mathcalls.h` through `mcpp::toolchain_sysroot()` and refuses the pair,
  naming the 13.x toolkit as the way out. The clang route does not include that
  header at all.

A second compiler also has to be told where it is. `mcpp::toolchain_sysroot()`
and `mcpp::toolchain_binutils_dir()` are the `--sysroot` and `-B` mcpp passes
to its own compiler; without forwarding them, NVIDIA's `crt/host_config.h`
stops at `features.h: No such file or directory`.

Everything about a compiler's spelling lives in the rule package. The engine
owns the graph, the artifact's identity and the architecture set; it does not
own `-gencode` or `--cuda-gpu-arch`.

## What the rule reports before the first compile

The rule states machine facts through the build program's own channel, and mcpp
compares them:

```
mcpp:fact=cuda.driver=12.4
mcpp:floor=cuda.driver >= 12.0
```

The fact comes from opening the driver's own library through the
`libcuda-host-link` sentinel and asking it for its version; the floor comes
from the toolkit the project named. mcpp refuses a build whose floor is not met
and says so in one sentence, because the failure it prevents is not a build
failure:

```
error: `cuda-saxpy` requires cuda.driver >= 13.0, and this machine has 12.4.
```

A separate advisory covers PTX: embedded PTX emitted by a toolkit newer than
the driver cannot be JIT-compiled by that driver, so hardware outside the named
architecture set will not run. The named architectures still do, so this is a
warning rather than a refusal.

The engine holds no vendor name for any of this. `cuda.driver` is a string
flowing from a declaration to a comparison; a second backend needs no engine
change. A unit test refuses vendor probes in `src/`.

## Verified

On an NVIDIA RTX 4080 (compute capability 8.9), driver 550.144.03 reporting
CUDA 12.4, with an LLVM 22.1.8 toolchain:

```
$ mcpp run
     Running `target/x86_64-linux-gnu/<accel>/bin/cuda-saxpy`
12 24 36 48

$ mcpp run --no-accel
     Running `target/x86_64-linux-gnu/<host>/bin/cuda-saxpy`
12 24 36 48
```

which is `2.0 * [1,2,3,4] + [10,20,30,40]`, computed on the device in the first
case and on the host in the second. The two artifact directories differ, and
the CPU one contains no `cudaMalloc`.

The nvcc route is not exercisable on that machine: the 12.9 toolkit meets the
driver and not the C library, and the 13.3 toolkit meets the C library and not
the driver. Both refusals are the ones described above, and both name the way
out.

## Where the driver comes from

The CUDA runtime is linked statically, so the artifact carries every
redistributable component. That leaves exactly one host dependency,
`libcuda.so.1` — the driver's userspace library, which NVIDIA's licence forbids
redistributing and which is in ABI lockstep with the kernel module.

Reaching it takes two packages, one per layer.

`libcuda-host-link` in xim owns the question of *where* the host's copy is. It
installs a symlink to whatever the machine has, so every GPU consumer reads one
path instead of reimplementing an `ldconfig` probe.

`compat.cuda-runtime` in mcpp-index owns the mcpp-side question of how a
**built artifact** reaches it. mcpp's private loader does not consult
`/usr/lib`, so a bare-soname `dlopen` from inside a built program finds nothing;
the package declares a directory on the artifact's runtime search path and links
through the sentinel. It is declared here as an ordinary dependency:

```toml
[dependencies.compat]
cuda-runtime = "2026.09.05"
```

Without it the program builds and links, then reports

```
cudaMalloc: CUDA driver version is insufficient for CUDA runtime version
```

which is what the runtime says when it cannot open the driver at all.
