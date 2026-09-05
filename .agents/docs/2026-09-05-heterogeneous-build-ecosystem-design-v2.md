# Heterogeneous C++ builds and their ecosystem, v2: closing the host surface

> Supersedes `.agents/docs/2026-09-05-multi-device-ecosystem-design.md` (v1) and
> keeps its task table. v1 asked how a device build should be shaped; this
> document asks what remains outside the ecosystem when it is, answers it with a
> measurement rather than a list, and states the one invariant the rest follows
> from.
>
> Status of the sections below: §2 is delivered and verified, §3 is a
> measurement taken on 2026-09-05, §4 is the plan that closes what §3 found.

---

## 1. The invariant

**Nothing reaches the host except PROPRIETARY vendor userspace that is in ABI
lockstep with a kernel module, and even that is linked rather than
redistributed.**

An open-source driver is not an exception: Mesa builds in a subos and ships as a
payload, so a machine with AMD, Intel or no GPU at all runs entirely on
packages. A closed-source component is either linked where it already is (the
sentinel packages) or fetched from the vendor's own published URL — never
copied into an xlings-res release.

Everything else — the X protocol stack, compression, ICU, an assembler, a C++
runtime, LLVM, a shader compiler, a software rasteriser — is a package. Where
one does not exist yet, the correct action is to publish it, not to widen the
exception.

Two consequences that are easy to state and were not being observed:

1. A *rule package* driving a second compiler must drive a compiler the
   ecosystem resolved. `mcpp.build.cuda` never invokes `/usr/bin/g++`: it takes
   the toolchain's own `g++`, and when nvcc's `crt/host_config.h` states a bound
   that excludes it, a `gcc` payload the project declared in
   `[xlings.workspace]`, and otherwise it refuses and names the declaration to
   add. There is no host branch in that decision.
2. A compiler the ecosystem resolved still has to be *told* where the
   ecosystem is. `MCPP_TOOLCHAIN_SYSROOT` and `MCPP_TOOLCHAIN_BINUTILS_DIR`
   carry the `--sysroot` and `-B` mcpp passes to its own compiler, so a rule
   package can forward them. Without them the payload compiler looks in
   `/usr/include` and the payload assembler is not found at all — the two
   variables are the mechanism that keeps the host out, not evidence of it.

## 2. What v1 delivered, with its evidence

| | criterion, as measured |
|---|---|
| Accelerator axis, constrained globs, action edges, probe channel, version floors | mcpp 2026.9.5.2, released; e2e 601/605/606/607/608 |
| Device objects reach a static library | e2e 608 asserts the `ar t` member list and an `nm` symbol, not the exit status — an empty archive also succeeds |
| CUDA lane, two routes | `12 24 36 48` on an RTX 4080, clang `-x cuda` and nvcc; no `/usr` path on any command line |
| A rule package owns the vendor spelling | `test_core_vendor_probes`: no vendor tool name in `src/` after comments are stripped, with its own denominator |
| Vulkan lane | `examples/10-vulkan-compute`: one artifact, three devices — discrete GPU, CPU rasteriser, and `--no-accel` — all `12 24 36 48` |
| A Vulkan device that needs no GPU | `xim:mesa-lavapipe@26.2.1`, published; `CPU llvmpipe (LLVM 22.1.8)` from a fresh extraction at an unrelated path |
| The ICD closure is computed, not listed | `compat.vulkan-runtime` seeds `ldd` from each ICD manifest's `library_path`; before, a machine with lavapipe installed enumerated no CPU device because nothing in a hand-written list could know that LLVM links ICU |

Four corrections v2 makes to v1, each found by building the second instance of
something v1 had built once:

* **The device-source table was one row per vendor.** `SourceKind::Device`
  documents itself as a graph role explicitly so the table does not grow per
  vendor, and held `.cu` and `.hip` alone. A shader in a constrained glob was
  refused with "no role for the extension '.comp'" while the same run told the
  rule package there were no device sources. Now 18 extensions: CUDA, HIP, the
  GLSL stages, HLSL, OpenCL C, Metal.
* **Rule-package naming did not follow its own specification.**
  `2026-08-29-build-rule-package-spec.md` I1/I8 say the module name is declared
  by the rule's source and that `mcpp.*` is reserved for rules the project
  maintains, enforced as a warning keyed on the package namespace.
  `mcpplibs.rules.cuda` was neither. Both rules are now `mcpp.build.cuda` and
  `mcpp.build.spirv` under the `mcpp` namespace, and the warning firing under
  the old namespace is the control that proves the check is live.
* **`ldd` on `PATH` is not the host's.** Under xlings it is the payload's own,
  whose default search path is its build prefix, so it answers `not found` for
  every host library — and that failure is indistinguishable from "this machine
  needs nothing", because both produce an empty match. Search paths are now
  supplied explicitly.
* **`DT_RUNPATH` is not a modern `DT_RPATH`.** A non-empty `RUNPATH` on a
  `dlopen`'d object switches off the executable's inherited `RPATH` for that
  object's dependencies, and an mcpp binary reaches its C library only through
  that inherited path. Measured both ways on one machine, minutes apart.

## 3. The host surface, measured

`compat.vulkan-runtime` now writes `HOST-SURFACE.txt` into the package at
install time: every farm entry with the file it points at, what was filled from
an installed payload, and what nothing could resolve. The classification below
is that file, intersected with what `xim-pkgindex` publishes, on a machine with
an NVIDIA driver and the distribution's Mesa.

| class | count | verdict |
|---|---|---|
| Vendor driver userspace (`libnvidia*`, `libGLX_nvidia`, `libvulkan_*`, `libcuda`, `libnvcuvid`) | 47 | **irreducible.** Already modelled as sentinel packages (`xim:libcuda-host-link`, `xim:nvidia-gl-host-link`, `xim:wsl-gl-host-link`) that link rather than redistribute |
| Version-locked to the host's Mesa (`libLLVM.so.20.1`) | 1 | **not irreducible — an artefact of using the host's Mesa at all.** The soname names the LLVM that build was linked against, so it cannot be substituted; the answer is not to substitute it but to stop loading the host's Mesa. See §3.1 |
| Published by `xim-pkgindex` already (`libX11`, `libxcb` and its 25 extension libraries, `libdrm` and its four `libdrm_*`, `libXau`, `libXdmcp`, `libXext`, `libxshmfence`, `libexpat`, `libz`, `libffi`, `libelf`, `libxml2`, `libtinfo`, `libwayland-client`, `libstdc++`) | 21 sonames, 25+4 more inside two of them | **reducible today by declaration** |
| Published by nobody | 6 upstream projects | **reducible by packaging**, listed below |

The six, and what needs them:

| project | sonames | reached through |
|---|---|---|
| `zstd` | `libzstd.so.1` | Mesa, LLVM |
| `xz` | `liblzma.so.5` | `libxml2` ← LLVM |
| `icu` | `libicuuc`, `libicudata` | LLVM 20+ |
| `libedit` | `libedit.so.2` | LLVM |
| `libbsd` + `libmd` | `libbsd.so.0`, `libmd.so.0` | `libX11` on distributions that link it |
| `xcb-util` family | `libxcb-util`, `-image`, `-keysyms`, `-icccm`, `-render-util`, `-cursor` | toolkits above X |

They are ordinary autotools/meson projects and the harness that builds the rest
of the stack in a subos (`.agents/tools/graphics/build-in-subos.sh`) applies
unchanged.

### 3.1 The driver taxonomy, which decides everything above

The table's second row is not a law of nature. It exists because the machine
loaded the *host's* Mesa, and the host's Mesa was linked against the host's
LLVM. A driver that is open source does not have to come from the host at all:

| driver | source | who provides it |
|---|---|---|
| llvmpipe / lavapipe (CPU) | Mesa | **payload** — `xim:mesa-lavapipe`, shipped |
| radeonsi, RADV (AMD) | Mesa | **payload** — `xim:mesa` already builds them |
| iris, anv (Intel), nouveau, zink, d3d12 | Mesa | **payload**, once the build gains `-Dvulkan-drivers=intel` and the `clc` chain its Intel Vulkan driver needs |
| NVIDIA proprietary userspace (`libcuda`, `libnvidia*`, `libGLX_nvidia`) | NVIDIA | **sentinel** — linked, never copied: `xim:libcuda-host-link`, `xim:nvidia-gl-host-link` |
| WSL2's D3D12 userspace | Microsoft, mounted by WSL | **sentinel** — `xim:wsl-gl-host-link` |

So the irreducible set is exactly *proprietary userspace in ABI lockstep with a
kernel module*, and even that is linked rather than redistributed. Everything
else on a machine — including a graphics driver — is a payload, and when the
payload driver is used, the second row of the table above disappears with it,
because the payload's LLVM is `xim:libllvm`.

The consequence for §4 is a task rather than an exception: extend the Mesa
payload's driver set, and prefer the payload ICD over the host's whenever one
covers the hardware. The host ICD path then exists for proprietary drivers
only.

### 3.2 The C++ runtime is a package, not an exception

`libstdc++.so.6` and `libgcc_s.so.1` are redistributable — GPL-3.0 with the
runtime library exception exists for exactly this — and `xim:gcc-runtime`
publishes them. The earlier reasoning for leaving them on the host was
directional and stated without its direction: substituting an **older**
libstdc++ under a host driver fails as a missing symbol version, but
substituting a **newer** one is what every distribution upgrade does, and
libstdc++ is backward compatible by design.

So the rule is a comparison, not an avoidance: the payload copy is used when its
`GLIBCXX`/`CXXABI` version set covers what the host's provides, and the host's
otherwise. The same holds for the third class generally — the substitution is
safe in the direction where the package is at least as new, and that is a
question a package can answer at install time rather than a hazard it has to
route around.

### 3.3 What remains deliberately unsubstituted

Only this: a soname the host provides at a version *newer* than any package in
this ecosystem. The farm then keeps the host's copy, records it in
`HOST-SURFACE.txt`, and the entry is a packaging backlog item rather than a
permanent exception.

## 4. The plan that closes it

Dependencies run downward; each task states the criterion that decides it.

### Tier A — packages that remove host libraries (xim-pkgindex)

| # | task | criterion | depends on |
|---|---|---|---|
| A1 | `zstd`, `xz`, `icu`, `libedit`, `libbsd`+`libmd` | each installs and `selfcontained-check.sh` reports no host reference | — |
| A2 | `xcb-util`, `-image`, `-keysyms`, `-wm`, `-renderutil`, `-cursor` | same | A1 (`libbsd`) |
| A3 | aarch64 payloads for the 21 sonames of the third class | the same probe passes on aarch64 | A1, A2 |
| A4 | `pocl` (CPU OpenCL), repacked from conda-forge as `mesa-lavapipe` was | `clinfo`-equivalent probe reports a CPU device with no GPU present | A1 |
| A5 | extend `xim:mesa`'s driver set (`-Dvulkan-drivers=amd,swrast,intel`, `-Dgallium-drivers=+iris,nouveau`) so hardware Vulkan and GL on open drivers are payloads | on an AMD or Intel machine, `HOST-SURFACE.txt` contains no driver entry at all | A1, `glslang` (published), the `clc` chain for anv |
| A6 | `gcc-runtime` version comparison in the farm: payload copy when its `GLIBCXX`/`CXXABI` set covers the host's | the probe still loads on a host newer than the payload, and the report says which copy was chosen | A1 |

### Tier B — the OpenCL adapter (mcpp-index)

| # | task | criterion | depends on |
|---|---|---|---|
| B1 | `compat.opencl-headers`, `compat.opencl` (Khronos ICD loader, shared, `libOpenCL.so.1`) | a probe links and reports the host platform | — |
| B2 | `compat.opencl-runtime` (library farm + `HOST-SURFACE.txt`) | a manifest naming `libnvidia-opencl.so.1` resolves under mcpp's loader | B1 |
| B3 | pocl's manifest into the subos vendors directory, plus a sentinel linking the host's | a machine with a GPU sees both platforms; `OCL_ICD_VENDORS` **replaces** rather than adds, which is why one merged directory is the only correct shape | A4, B2 |

### Tier C — rules and frameworks (mcpp)

| # | task | criterion | depends on |
|---|---|---|---|
| C1 | `mcpp.build.spirv` + `examples/10-vulkan-compute` (the module is `mcpp.rules.spirv` since round 3; see 6.2) | three devices, one artifact — **done** | — |
| C2 | `mcpp.build.sycl` driving the `dpcpp` payload | a SYCL kernel runs on the CUDA backend | — |
| C3 | `mcpp.build.hip` | `HIP_PLATFORM=nvidia` kernel runs | `hip-runtime` payload |
| C4 | llama.cpp Vulkan lane | tokens on lavapipe with no GPU | C1, and a `glslc` payload or a flag translator |

### Tier D — the ecosystem criterion

`HOST-SURFACE.txt` contains **only** proprietary vendor userspace, and only on
machines that have such a driver, plus -- on a host that uses a proprietary
driver -- the libraries that driver links which no installed payload covers,
each recorded with the reason (no payload publishes the soname, or the host
copy is newer than the payload). A dependency on those payloads is not
declared by the adapter: the 21 self-built graphics packages are x86_64-only
and a hard dependency would refuse aarch64 outright, so the adapter fills
from what the sandbox holds and states the remainder. The cases:

* no GPU, or an open driver — the file is empty, because the payload driver
  path touches no host file;
* AMD or Intel — empty after A5;
* NVIDIA — the `libnvidia*` family and `libcuda.so.1`, reached through a
  sentinel that links and does not copy.

The file is produced by the package at install time, so the claim is a
measurement on the user's own machine rather than a statement in this
document.

## 5. The dimensions the plan is judged on

**Architecture.** The engine owns the graph and knows no vendor name; a rule
package owns the spelling; a payload owns the binaries; a sentinel owns the
irreducible host link. Four layers, and each of the four corrections in §2 was a
value that had leaked across one of those boundaries.

**Stability.** Every widening in this round is provably inert on existing
builds: device extensions are absent from the default globs and were previously
a hard error; the ICD closure only adds symlinks for versioned sonames, which
the linker never resolves; the farm's gap-filling is monotone.

**Elegance.** No new primitive was introduced. Shaders reach a rule package
through the same constrained glob that carries `.cu`; the SPIR-V header is an
ordinary `role = "source"` action; the target environment is read from the same
`accel` axis that carries `sm_89`.

**User experience.** A build states what it is for once, in the manifest.
Failures name the declaration to add: the nvcc host-compiler bound, the missing
glslang, the accelerator that names no architecture.

**Compatibility.** `mcpplibs:rules-cuda@0.1.0` stays in the index, frozen, with
a pointer to `mcpp:rules-cuda@0.2.0`; the pattern the `mcpplibs:llamacpp` →
`ggml-org:llamacpp` move established.

**Cross-platform.** The gap is honest: the third-class packages are x86_64-only
today, which is why the farm falls back to the host rather than declaring them
as dependencies — a hard dependency on an x86_64-only package would break
aarch64 Linux outright. A3 closes it; until it does, the fallback is the
behaviour aarch64 already has.

**Consistency.** Both rule packages now carry the module name their own
specification requires, and the check that enforces it was verified in both
directions.

**Seamless upgrade.** No manifest key changed. A project that never mentions a
shader or an accelerator builds exactly as before, which the default-glob
assertion states over the whole list rather than over the two names that
happened to be there.

**Test coverage.** e2e 609 asserts all 18 extensions with the table as its
denominator — and caught a `;`-vs-newline splitter in the rule package that was
correct for exactly one shader.

## 6. Round 3 (2026-09-05): directives, corrections and the implementation schedule

### 6.1 Directives added in this round

1. Every library or tool a build reaches is published in `xim-pkgindex` or
   `mcpp-index`. Nothing is taken from the host except proprietary driver
   userspace, which is linked in place or fetched from the vendor's own URL.
   Redistributable payloads carry a CN mirror (`gitcode.com/xlings-res/...`,
   `gitcode.com/mcpp-res/...`).
2. Official build plugins live in one repository, `mcpp-community/mcpp-plugins`,
   published as the package `mcpp:plugins`. Module names follow
   `mcpp.rules.<x>` for rule packages and `mcpp.tools.<x>` for build-time
   utilities; the members of the collection are selected through features.
   The rule packages under `examples/` in the mcpp repository are withdrawn;
   an example consumes the index package like any other project.
3. Chapter 20 of the manual is renamed from "Accelerators" to "Heterogeneous
   Builds", with a subtitle naming GPU and AI accelerator targets and mixed
   host/device compilation. The `accel` manifest key is unchanged.
4. Documentation and code comments carry no emoji or decorative symbols.
5. The plan below is the record of what is done; a task is closed only by its
   criterion.

### 6.2 Corrections to sections 2 to 5

* `mcpp.build.<x>` was the wrong prefix for a plugin module. mcpp's own engine
  modules are named `mcpp.build.plan`, `mcpp.build.prepare` and so on, so a
  plugin under that prefix shares a family name with the engine it drives. The
  rule-package specification withdrew `mcpp.rules.*` only because, at the time,
  a host module's name was its bare package name and could not contain a dot
  (I1 in `2026-08-29-build-rule-package-spec.md`). I1 is implemented, the
  objection no longer holds, and `mcpp.rules.*` is reinstated.
* A feature-controlled collection needs one engine extension: a host-module
  package contributes every module interface unit among its feature-resolved
  sources, the lib root first, rather than the lib root alone. Nothing else in
  the host-module path assumes one unit per package; `build_host_module` is
  already per unit and the compile loop already accumulates BMIs in order.
* pocl's ICD does not need a merged vendors directory. The Khronos loader
  enumerates `OCL_ICD_FILENAMES` and then the vendors directory
  (`khrIcdOsVendorsEnumerate` in `loader/linux/icd_linux.c`), so a payload ICD
  is added through the environment while the host's `/etc/OpenCL/vendors`
  stays in effect. B3 of section 4 is replaced accordingly.

### 6.3 Schedule

Dependencies run downward within a repository and across the arrows noted.
Status is one of `done`, `open`, `deferred (reason)`.

#### mcpp (single PR from `feat/vulkan-spirv`, version 2026.9.5.3)

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| M1 | Host-module packages contribute every interface unit among their feature-resolved sources | e2e 610: a consumer enabling `rules-a` imports `mcpp.rules.a`; without the feature the import fails as an unknown module; both features enable both; a package with neither a lib root nor a unit keeps today's diagnostic. Unit tests for the interface-unit detector | — | done |
| M2 | Module family `mcpp.rules.*` / `mcpp.tools.*`; specification I8 and section 7 corrected; docs 05 and 07 (both languages); examples 09 and 10 consume `mcpp:plugins` from the index | `grep -r 'mcpp\.build\.\(cuda\|spirv\)'` over docs, examples and tests is empty; the reserved-prefix warning still fires for `mcpplibs` and not for `mcpp` (e2e 309) | M1 | done |
| M3 | Chapter 20 renamed to `20-heterogeneous-builds.md` (both languages); every reference updated | `check_docs_style.sh` passes; no reference to `20-accelerators.md` remains | — | done |
| M4 | No emoji in docs, README, CHANGELOG, code comments, or the two design documents | a grep over the emoji ranges returns nothing outside program output strings | — | done |
| M5 | Version 2026.9.5.3 in `mcpp.toml` and `modules/versioning`; CHANGELOG; unit and e2e suites; PR; CI green; self-review | e2e summary has no failure other than 168 (pre-existing on `origin/main`); all CI jobs green on the PR head | M1–M4 | done: unit 102/102, e2e 304 passed with 168 as on main, PR #566 37/37 green, merged as 03b5074 |
| M6 | Release, mirror, index bump, bootstrap pin | both mirrors return 200 and identical bytes for every asset; `origin/main:pkgs/m/mcpp.lua` in xim-pkgindex names 2026.9.5.3 as `latest`; `.xlings.json` pin bumped | M5 | done: release run 33964745434 green (six jobs); four assets on `xlings-res/mcpp` at GitHub and GitCode return 200 with the upstream sha256; bump PR openxlings/xim-pkgindex#763 merged (7cc3f43), `Publish Index Artifact` green (run 33966046795); `xlings install mcpp@2026.9.5.3` yields `mcpp 2026.9.5.3` at the store path; bootstrap pin bumped on main (893011b9) |

#### mcpp-plugins (new repository, single PR, version 0.1.0)

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| P1 | Repository with `mcpp.toml` (`mcpp:plugins`), `src/plugins.cppm` (`mcpp.plugins`), `rules/cuda.cppm` (`mcpp.rules.cuda`), `rules/spirv.cppm` (`mcpp.rules.spirv`), features `rules-cuda`, `rules-spirv`, README stating the naming rule and the mcpp floors | a consumer with `features = ["rules-spirv"]` builds a shader through it | M1 | done (PR #1 open) |
| P2 | CI: one consumer fixture per feature, built with the pinned mcpp | green on the PR head | P1, M6 for the spirv fixture | done: the first two runs exposed host leaks on the developer machine (clang's CUDA wrapper found `curand_mtgp32_kernel.h` and then `nv/target` in the host's /usr/include); `mcpp.rules.cuda` now requires `xim:libcurand` and `xim:cuda-cccl` on the clang route, adds their include directories, and refuses without them naming both entries; fixture and example 09 (mcpp#567) declare them; run 3 green |
| P3 | Release `v0.1.0`; GitHub archive and a GitCode release asset with identical bytes | both URLs return 200 and one sha256 | P2 | done: PR #1 merged (be6d7ce), tag v0.1.0, GitHub release; `gitcode.com/mcpp-res/mcpp-plugins` release 0.1.0 carries the byte-identical archive (200, 26,953 bytes, sha256 adf1f9d6...) |

#### mcpp-index (PR #349)

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| I1 | `pkgs/m/mcpp.plugins.lua` (GLOBAL and CN URLs, floor 2026.9.5.3); `mcpplibs:rules-cuda` kept, marked superseded | `mcpp add mcpp:plugins` resolves in a sandbox | P3, M6 | done: `pkgs/m/mcpp.plugins.lua` (GLOBAL and CN, one sha256), `mcpplibs:rules-cuda` marked superseded; with the checkout as the `mcpp` index the released 2026.9.5.3 downloads `mcpp.plugins v0.1.0`, compiles it, and the SPIR-V probe answers `magic=07230203` |
| I2 | `compat.vulkan-runtime` 2026.09.05: the pattern list is reduced to proprietary vendor userspace; an ICD's needs are computed by closing over the manifests' libraries; a farmed soname an installed payload also provides is re-pointed at the payload when the payload's versioned symbol set covers the host copy's (the `GLIBCXX`/`CXXABI` nodes of libstdc++ included); `HOST-SURFACE.txt` states the class of every entry | measured on this machine: 37 vendor entries, 20 payload substitutions, 8 host sonames no installed payload provides (`libbsd`, `libedit`, `libicudata`, `libicuuc`, `liblzma`, `libmd`, `libzstd`, `libtinfo`), 8 host Mesa ICDs, 3 host copies newer than the payload (`libdrm_amdgpu`, `libLLVM.so.20.1`, `libxml2`); example 10 still answers `12 24 36 48` | — | done: pattern list reduced to vendor userspace; closure from the manifests; payload-first with the symbol-set criterion; measured here 37 vendor, 20 substitutions, 8 host sonames without payload (`libbsd`, `libedit`, `libicudata`, `libicuuc`, `liblzma`, `libmd`, `libzstd`, `libtinfo`), 8 host Mesa ICDs, 3 host copies newer than the payload; example 10 still `12 24 36 48` |
| I3 | `compat.opencl-headers`, `compat.opencl` verified with a probe (`tests/examples/opencl`, a workspace member); `compat.opencl-runtime` 2026.09.05 farms the libraries the host manifests name, their closure and the vendor family, prefers payloads, records the surface; payload entries of `OCL_ICD_FILENAMES` are left to the payload | the probe enumerates the NVIDIA platform on this machine and zero platforms on a runner; the pocl platform once X2 is installed | X2 for the pocl half | done for the host half: `tests/examples/opencl` lists `NVIDIA CUDA / RTX 4080` here and zero platforms on the Linux runner; with `OCL_ICD_FILENAMES=<pocl>/lib/libpocl.so` the same loader lists `Portable Computing Language` and `NVIDIA CUDA` in one process |
| I4 | CI green; merge; index artifact published | `Publish Index Artifact` green on the merge commit | I1–I3 | done: PR #349 green (10 pass; the `full sweep` job skips by design), merged as 754d775, `Publish Index Artifact` green on it (run 33968256961) |

| I5 | `compat.vulkan-runtime` 2026.09.06: the payload set is declared as `xpm.linux.deps.runtime` rather than discovered in the store; `PAYLOAD_PACKAGES` maps each soname to the package declared for it and reports a declaration that did not take effect; both adapters refuse a payload built for another machine (ELF `e_machine`); the Vulkan adapter gains the aarch64 multiarch directories | a fresh environment substitutes the same set a developer machine does; the report contains no line saying a declaration did not take effect | I4, X5 | done: merged as 0bbf3ad, index artifact published; measured in a fresh subos, 24 payload substitutions against 1, and four host sonames without a payload against thirty |
| I6 | Both adapters 2026.09.07: a soname carried by more than one installed payload is decided by symbol coverage rather than by which store path sorts last; `compat.vulkan` and `compat.opencl` move their pins | in a fresh subos, `libX11.so.6` comes from `xim:libX11` rather than staying on the host with `xim:mesa-lavapipe`'s bundled copy named as the reason | I5 | done: merged as f15d4fe and published; 25 substitutions against 24, and `libdrm_amdgpu` moved to a payload copy that covers the host's where the dedicated package's did not |

#### xim-pkgindex (PR #762)

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| X1 | `zstd`, `xz`, `icu`, `libedit`, `libmd`, `xcb-util`, `xcb-util-image`, `xcb-util-keysyms`, `xcb-util-renderutil`, `xcb-util-wm`, `xcb-util-cursor`: conda-forge repacks for x86_64 and aarch64, published to `xlings-res/<name>` on both mirrors, recipes with `deps`, `exports`, `declare_libs`, headers and `.pc` | each installs; every `DT_NEEDED` of each payload library resolves inside payloads; CI green | — | done, pending CI: all eleven on both mirrors (44/44 GET checks by the agent, two re-verified by sha256 here); finding: an aarch64 install fails today because `xim:glibc`, `xim:gcc-runtime`, `xim:ncurses`, `xim:libxcb` publish no aarch64 asset and `deps` are per OS |
| X1b | `libbsd` built in the subos harness (not on conda-forge), x86_64 | same | X1 (`libmd`) | done, pending CI: libbsd 0.12.2 built by the subos harness (exit 0, inputs and payload host-free), `DT_NEEDED` libmd.so.0 and glibc only; two defects found by the agent and fixed: libbsd's `-isystem` self-overlay loses to the harness's `-I` (patched), and upstream's `make install` writes `lib/libbsd.so` as an ld script naming `/usr/lib` (replaced by a symlink); both mirrors verified |
| X2 | `pocl` 7.1 (CPU OpenCL) repacked for both architectures; its ICD reached through `OCL_ICD_FILENAMES` in the subos environment | `clinfo`-equivalent probe reports the pocl platform with no GPU | — | done: pocl 7.1 for both architectures on both mirrors; after the index published, `xlings install pocl` from `xim:` installs 7.1 and the Khronos loader with `OCL_ICD_FILENAMES=<payload>/lib/libpocl.so` lists `Portable Computing Language` (cpu-haswell) and `NVIDIA CUDA` in one process; the recipe declares that variable and never `OCL_ICD_VENDORS` (the agent had wired the latter; corrected); finding: xlings 2026.9.3.2 does not persist a new package's `subos.env` declaration (openxlings/xlings#584), so the verification sets the variable explicitly |
| X3 | `mesa-lavapipe` aarch64 payload | `archs` lists both; the aarch64 tarball is on both mirrors | — | done, pending CI: the aarch64 lavapipe payload (99,534,504 bytes) on both mirrors; four-file closure difference, none `DT_NEEDED` by `libvulkan_lvp.so` |
| X4 | `xim:mesa` gains the Intel Vulkan driver | on an Intel machine `HOST-SURFACE.txt` has no driver entry | libclc and SPIRV-LLVM-Translator payloads | deferred: anv requires `intel_clc`, which needs a libclc and clang chain this index does not publish yet; the chain is a separate packaging round and is recorded here rather than approximated |
| X5 | CI green; merge; index artifact published | `Publish Index Artifact` green on the merge commit | X1–X3 | done: PR #762 green on its pull_request-event runs (the push-event `linux-install-test` sees only the last push and fails on libbsd by construction, as the workflow header states), merged as 2d2a2ee; `Publish Index Artifact` green on it (run 33967729241) |

#### mcpp, second round (PR #570, version 2026.9.5.4)

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| M7 | The fast path compares every declared build-program input, not only glob path sets | e2e 612: editing a file declared with `rerun_if_changed` changes what the binary prints, and an unchanged input still takes the fast path | — | done: verified failing on 2026.9.5.3 and passing on this branch |
| M8 | A version conflict on a package with no named C++ module names both versions and both requesters | the message example 10 produced now says which two versions are in the graph | — | done |
| M9 | The markers removed from `bench/`, `tools/`, `scripts/`, `mcpp.toml` and `README.zh-CN.md`; the Chinese target table gains the row and the words its English counterpart has | a sweep over the emoji ranges returns nothing outside program output | — | done |
| M10 | Release 2026.9.5.4, mirror, index bump, bootstrap pin | both mirrors return 200 and identical bytes; xim-pkgindex names it as `latest` | M7-M9 | done: release run 33979414813 green on all four platform jobs; the four archives compare byte for byte between GitHub and GitCode; openxlings/xim-pkgindex#764 merged (7fd026e) with `Publish Index Artifact` green; bootstrap pin bumped on main |

#### mcpp-plugins, second round (PR #2, version 0.1.1)

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| P4 | `mcpp.tools.embed`, the first member of the tools half, selected by `tools-embed` | the fixture activating only that feature embeds a data file, and its second run proves an edit reaches the binary | M7 | done, CI pending on the 2026.9.5.4 release |
| P5 | The markers removed from the rule sources; version 0.1.1; release and mirror; index descriptor; the two examples move their pin | the same two URLs return 200 with one sha256 | P4, M10 | done: PR #2 merged (f1bec00), tag v0.1.1, GitHub release, GitCode mirror byte-identical at 30,644 bytes; mcpp-index#352 merged (d4b36d7) and published; the examples and both manifest chapters pin 0.1.1 in mcpp#571 |

#### Verification

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| V1 | Fresh subos created for this run, CN mirror configured for both tools: install mcpp 2026.9.5.4 from the index, build examples 09 and 10 against `mcpp:plugins` 0.1.1, run 10 on the lavapipe payload, run the OpenCL probe on pocl | `12 24 36 48` from example 10 with no GPU; the pocl platform enumerated; every declared payload substitution took effect (see 6.4 for why an empty host surface is not the criterion) | M10, P5, I6, X5 | **done, green.** A machine that had never seen this ecosystem: `mcpp 2026.9.5.4` from the store path, both mirrors CN, index snapshot `d4b36d7`, `mcpp.rules.spirv` from the collection produced a SPIR-V header, example 10 answered `12 24 36 48` on the lavapipe payload and again with `--no-accel`, **25 payload substitutions against 6 host entries with every declared payload in effect**, example 09 answered on the CPU variant and compiled its device variant, and the Khronos loader listed `Portable Computing Language` through `OCL_ICD_FILENAMES` |
| V2 | The same on this host with the GPU: example 10 on the host ICD, example 09 on both routes | `12 24 36 48` in every case; the host entries of `HOST-SURFACE.txt` are proprietary userspace, host Mesa drivers, and the recorded symbol-set and soname gaps only | V1 | **done, green (0 assertions failed).** Same five identity readings, example 10 on the payload driver and with `--no-accel`, the farm written by 2026.09.07 with 25 substitutions and 6 host entries, example 09 `12 24 36 48` **on the RTX 4080**, and the OpenCL loader listing `NVIDIA CUDA` and then pocl beside it |

### 6.5 What the two runs corrected in the harness itself

Three assertions failed on something other than the ecosystem, and each is the same shape: a criterion pointed at the wrong object.

**The mirror was read from the tool's stdout.** `xlings config` renders its banner through a ui layer that prints nothing when stdout is a pipe, so a command substitution read an empty string and reported a mirror that was in fact CN. The check now reads the file the tool writes.

**The report was chosen by `ls | head -1`.** A store that has seen three adapter versions holds three reports, and the first one alphabetically is the oldest: the host run reported 20 substitutions and 11 host entries from the 2026.09.05 farm while the 2026.09.07 farm beside it had 25 and 6. The pinned version is now asked for by name, with the newest as the fallback for a store that holds one.

**pocl's presence was read from an exit code.** `xlings install` returns non-zero for a package that is already installed, so a payload that had installed correctly a minute earlier was reported as `not installable here`. The criterion is now the library the loader needs.

### 6.4 What the first verification run measured, and the two criteria it retired

The first sandbox run failed six assertions. One was a defect in the ecosystem, two were criteria that could not hold by construction, and three were the machine's disk filling up mid-run. They are separated here because only the first is a change to a package.

**The defect: a substitution that the environment decided.** `compat.vulkan-runtime` 2026.09.05 re-points a farmed soname at an installed payload when the payload's symbol set covers the host copy's, and *installed* was left to chance -- the pass looked in the store and took what an earlier, unrelated install had put there. The same package therefore produced twenty payload substitutions on the developer machine and one in a fresh subos, and the report read `no installed payload provides this soname` for sonames this index publishes. A published package whose behaviour is that much better on the machine that wrote it is the shape recorded in `.agents/docs` as a development overlay verifying a world that does not exist. 2026.09.06 declares the set (row I5). Measured against a project-local index on this host: 24 substitutions against 20, seven host entries against eleven, and a Vulkan probe still enumerating two devices.

**The first retired criterion: an empty host surface in a sandbox.** A subos shares the host's `/usr`, which the run confirmed: 5354 entries in `/usr/lib/x86_64-linux-gnu` inside the sandbox, `libnvidia-*` among them. The farm therefore sees the proprietary driver there exactly as it does on the host, and `HOST-SURFACE.txt empty in the sandbox` was unreachable by construction rather than a statement about the ecosystem. What a sandbox does measure is the substitution invariant: every soname the adapter declares a payload for is taken from that payload, and a declaration that did not take effect says so in the report.

**The second retired criterion: the exit code of a mirror write.** Both tools print their configuration on stderr, so reading it through `2>/dev/null` returned an empty string and reported a mirror that was in fact set to CN.

**Two properties of the sandbox that the run established.** A subos root has no `/run`, so the inherited `XDG_RUNTIME_DIR` names a directory that does not exist; Mesa falls back to it when `memfd` is unavailable and lavapipe fails with `Failed to create anonymous file for memory allocations` before it reports a device. And the registry's index snapshot is shared with whatever wrote it last: the run resolved against a snapshot four commits behind `main`, where `compat.opencl-headers` reported `download artifact missing` because the recipe was not in it. The verification script now redirects the first and refreshes the second before it measures anything.

## 7. Round 4 (2026-09-06): the second device API, and the compiler each one needs

Rounds 1 to 3 closed the host surface and delivered two lanes, CUDA and Vulkan,
each through a rule package in `mcpp:plugins`. What they did not deliver is the
evidence that the shape generalises: a single lane proves a rule package can
drive one vendor's compiler, and the claim this ecosystem makes is that the
engine knows no vendor at all. Round 4 is the second instance of that claim,
taken in the direction where it is most likely to break -- a device API that is
not CUDA (`HIP`), and a compilation model that is not the island (`SYCL`, where
the device compiler consumes ordinary C++ and emits a fat object).

### 7.1 What round 3 left open, and which of it this round closes

| row | item | round 4 |
|---|---|---|
| C2 | `mcpp.rules.sycl` driving the `dpcpp` payload | closed |
| C3 | `mcpp.rules.hip` | closed, on the NVIDIA platform, which is the platform this machine can decide |
| C4 | llama.cpp Vulkan lane | its blocker is closed (`glslc` has no payload); the framework itself is a packaging round of its own and stays open |
| A5 / X4 | Intel `anv` in the Mesa payload | its two missing links are closed (`libclc`, `llvm-spirv`); the Mesa rebuild stays open, and the reason is stated in 7.5 |
| A3 | aarch64 for the third class | stays open; the blocker X1 recorded is unchanged |
| T1.2 | `llvm-offload` | withdrawn: the `dpcpp` payload carries `clang-linker-wrapper`, `clang-offload-bundler` and `llvm-offload-binary`, which is the whole of what the row asked for, and round 4 makes that payload reachable through a rule |

### 7.2 The one engine change, and why it is one row

SYCL is compiled by a second compiler from a translation unit that is ordinary
C++ -- there is no `.cu` to route. The engine's device-extension table is the
mechanism that routes a source away from mcpp's own compiler, so the table gains
`.sycl` and nothing else changes. The table's own comment states the two
conditions under which an addition is inert, and both hold: device extensions
are absent from `default_source_globs`, so no glob widens, and a file with one of
these extensions named in `sources` is today a hard error, so nothing silently
changes role.

The alternative -- letting a constrained glob carry `.cpp` -- was rejected. It
would make one extension mean two things depending on which glob matched it
first, and the seam discipline round 2 settled on (device code reaches the
program only through an `extern "C"` header) is legible exactly because the file
name says which side of the seam a unit is on.

### 7.3 Schedule

Status is one of `done`, `open`, `deferred (reason)`.

#### xim-pkgindex (single PR)

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| Z1 | `xim:shaderc` 2026.3: `glslc` and `libshaderc`, conda-forge repack with its closure, x86_64 and aarch64, both mirrors | `glslc --version` from the payload; a `.comp` compiles to SPIR-V; every `DT_NEEDED` of every payload object resolves inside payloads | - | done: both mirrors byte-identical; `glslc` compiles a `.comp` to a module whose first word is `07230203`; `ldd` on all six programs names nothing outside the store |
| Z2 | `xim:hip-nvidia` rocm-7.2.4: the HIP headers plus `hipother`'s `nvidia_detail`, architecture-independent | a `.hip` translation unit compiles against it with `__HIP_PLATFORM_NVIDIA__` and links against the CUDA payload | - | done: three source trees, not two -- `hip_runtime_api.h` includes `amd_detail/amd_hip_runtime_pt_api.h` unconditionally on either platform, so `ROCm/clr` is required to parse; `hip_version.h` is generated from the tree's own VERSION |
| Z3 | `xim:libclc` 22.1.8 and `xim:llvm-spirv` 22.1.2, conda-forge repacks | each installs; `llvm-spirv --version` answers; the libclc bitcode for `nvptx64` and `spirv64` is present | - | withdrawn: see 7.5. Both exist on conda-forge and both were confirmed repackable, but nothing in this round consumes them -- the Mesa rebuild that would is the deferred item. A payload with no consumer is the "claim rather than a feature" this ecosystem already rejects |
| Z4 | CI green; merge; index artifact published | `Publish Index Artifact` green on the merge commit | Z1, Z2 | done: PR #768 merged as `62538ee`, `Publish Index Artifact` green on it; a fixture then resolved `xim:hip-nvidia` from the published index and answered `12 24 36 48`. The PR also carries a dpcpp repair CI found: see 7.5 |

#### mcpp (single PR, version 2026.9.6.1)

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| N1 | `.sycl` in the device-extension table | a unit test over the whole table with the table as its denominator; e2e: a `.sycl` in a constrained glob reaches `MCPP_DEVICE_SOURCES`, and the same content named `.cpp` is still C++ | - | done: e2e 613, five sections; the unit case verified to run BY NAME rather than by the suite being green. The row's original criterion was wrong and the run said so -- see 7.5 |
| N2 | `examples/11-sycl-compute`: one SYCL kernel behind a seam, the CUDA backend and the host device from one artifact | `12 24 36 48` on the RTX 4080 and again with `--no-accel` | N1, Q1 | open |
| N3 | `examples/12-hip-compute`: a `.hip` kernel through `mcpp.rules.hip` on the NVIDIA platform | `12 24 36 48`, and no `/usr` path on any command line | Q2, Z2 | open |
| N4 | Chapter 20 gains the SYCL and HIP lanes and the table of which rule drives which compiler; both languages | `check_docs_style.sh` passes; the Chinese chapter has the rows the English one has | - | done: the lane table, the two-chunk `accel` explanation, the HIP header-layer note and the two-C++-runtimes note, in both languages; "Not implemented" corrected -- the whole-target shape is no longer among the missing |
| N5 | Version 2026.9.6.1, CHANGELOG, unit and e2e suites, PR, CI green, self-review | no e2e failure other than the ones already failing on `origin/main` | N1-N4 | open |
| N6 | Release, mirror, index bump, bootstrap pin | both mirrors return 200 and identical bytes for every asset; xim-pkgindex names it `latest` | N5 | open |

#### mcpp-plugins (single PR, version 0.2.0)

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| Q1 | `mcpp.rules.sycl`, feature `rules-sycl` | a fixture compiles a `.sycl` unit with the `dpcpp` payload and links it into a C++23-modules program; the rule refuses with the declaration to add when the payload is absent | N1 | done: `12 24 36 48` on an RTX 4080 through `mcpp run`, zero `/usr` paths on any command line, and the device-link wrapper asserted as its own file |
| Q2 | `mcpp.rules.hip`, feature `rules-hip` | a fixture compiles a `.hip` unit on the NVIDIA platform; the rule names the platform it selected and why | Z2 | done: `12 24 36 48` on an RTX 4080, zero `/usr` paths; the AMD platform is refused by name with the reason rather than approximated |
| Q3 | `mcpp.rules.spirv` gains the `glslc` route now that a payload exists | both compilers produce a header the same program includes; the rule states which one it used | Z1 | done: the two command lines share almost nothing -- glslc's `-mfmt=c` is a bare initialiser list and its `-S` means "emit assembly" where glslang's names a stage -- so the rule writes the declaration itself and keys its `mcpp::fact` on the flavour |
| Q4 | CI: one consumer fixture per feature, built with the pinned mcpp | green on the PR head | Q1-Q3, N6 | open |
| Q5 | Release `v0.2.0`; GitHub archive and a GitCode asset with identical bytes; index descriptor | both URLs return 200 and one sha256 | Q4 | open |

#### mcpp-index (single PR)

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| R1 | `compat.sycl-runtime` 2026.09.06; then `mcpp:plugins` 0.2.0 in `pkgs/m/mcpp.plugins.lua`, floor 2026.9.6.1 | the adapter: a workspace member loads `libsycl.so.9` by soname under mcpp's own loader. the descriptor: `mcpp add mcpp:plugins` resolves in a sandbox and the new features are selectable | Q5, N6 | adapter done (PR #354 merged as `e17fc88`, index artifact published); descriptor open |
| R2 | CI green; merge; index artifact published | `Publish Index Artifact` green on the merge commit | R1 | open |

#### Verification

| # | task | criterion | depends on | status |
|---|---|---|---|---|
| V3 | A fresh subos, CN mirror for both tools: install mcpp 2026.9.6.1 from the index, build examples 09 through 12 against `mcpp:plugins` 0.2.0 | every example builds; example 10 answers on lavapipe; the device half of 09, 11 and 12 compiles and says cleanly that the sandbox has no device | Z4, N6, Q5, R2 | open |
| V4 | The same on this host with the GPU | `12 24 36 48` from 09, 11 and 12 on the RTX 4080 | V3 | open |

### 7.4 The completion rule

This round is not finished when its pull requests merge. It is finished when
every row above reads `done` against its own criterion and the ecosystem has
been exercised end to end on a machine that starts with none of it installed:
the released engine fetched from the index, the rule collection fetched from
the index, every payload fetched from its mirror, and each example answering.
A row whose work is written but not measured stays `open`; a row that cannot be
measured here says so and names what would decide it. V3 and V4 are that
measurement, and no part of this round is reported as complete before they are
green.

### 7.5 What this round's work corrected, and what it withdrew

Six changes to the plan above, each with what decided it.

**Z3 withdrawn: two payloads with no consumer.** `libclc` 22.1.8 and
`llvm-spirv` 22.1.2 are both on conda-forge, both repackable, and both are what
the deferred Intel `anv` build would need. Publishing them now would put two
packages in the index that nothing in this ecosystem reaches, which is the
"claim rather than a feature" `mcpp.rules.spirv` already refuses for itself.
They are published when the Mesa rebuild that consumes them is.

**N1's criterion was wrong, and the run said so.** It read "a `.sycl` outside a
constrained glob is still a hard error". It is not: an extension the table
names is accepted wherever it is globbed, and the hard error applied only while
the extension was absent from the table. The compatibility guarantee is the
default-glob section, which asserts the WHOLE list. The retired assertion was
replaced by one that measures something: the same content named `.cpp` is still
compiled as C++ and does not reach the build program -- a test naming only
`.sycl` would pass on a table that had started classifying by content.

**A dpcpp repair that took three attempts, and the third is the shape.** Five
of its programs --
`sycl-ls`, `sycl-prof`, `sycl-trace`, `sycl-sanitize`, `syclbin-dump` -- ship
with neither DT_RPATH nor DT_RUNPATH and could not start once installed. The
`[cuda:gpu] NVIDIA CUDA BACKEND` recorded in that recipe's own comment had been
measured with LD_LIBRARY_PATH set. Two attempts were needed:

* `selfcontain.seal`, the idiom the repacked payloads use, sets the
  INTERPRETER as well as the search path. Behind the ecosystem's private loader
  there is no host fallback, and CI's dependency-closure check refused it,
  naming four sonames with no provider in the index. The check was describing a
  real regression: the sealed payload enumerated no platforms where the
  unsealed one had found the GPU.
* `elfpatch.set_rpath` on BOTH halves made `sycl-ls` work and made every
  CONSUMER worse, which appeared only when an mcpp artifact reached the payload
  through a runtime adapter: a non-empty DT_RUNPATH on a payload library
  switches OFF the inherited DT_RPATH of whatever loaded it, so
  `libur_adapter_cuda.so.0` stopped seeing the artifact's own farm and failed
  first on `libcuda.so.1` and then on `libnvidia-ml.so.1`. Every consumer would
  have had to re-farm the payload's entire external closure.
* `patchelf --force-rpath` on the PROGRAMS and nothing on the libraries is the
  answer, and it is one tag rather than two paths: DT_RUNPATH is honoured for
  an object's own DT_NEEDED and not for a dlopen beneath it, DT_RPATH for both
  at any depth. The programs' RPATH therefore serves their adapters too, while
  the libraries keep inheriting from whoever loads them. `elfpatch` cannot
  express this -- `set_rpath` writes RUNPATH and `patch_elf_loader_rpath` also
  swaps the interpreter -- so the recipe calls patchelf directly with
  `xim:patchelf` as a build dep, which is what godot.lua and libglvnd.lua do
  for the same documented reason.

⭐ The regression was not visible from the payload. `sycl-ls` reported the GPU
in all three attempts from the second onward; what differed was whether a
CONSUMER of the payload still worked, and only building the example again
showed it.

**The SYCL link needs `-l:libstdc++.so.6`, not `-lstdc++`.** clang's driver
treats `-lstdc++` as a selector for the C++ standard library and rewrites it to
`-lc++` under `-stdlib=libc++`, so the flag disappears from the link line with
no diagnostic and `std::cerr` comes back undefined from inside a SYCL header.
Found by reading `clang -###`, after the symbol was verified to exist in the
library the `-L` named.

**`compat.sycl-runtime` was not in the plan and the round does not close
without it.** The rule can satisfy `-lsycl` at link time from the payload, and
the artifact then fails mcpp's runtime closure check. Three of the adapter's
entries were added by a failure rather than by design: the dlopen chain
(`libur_loader.so.0`, `libumf.so.1`), the C++ runtime that `compat.cudart`
deliberately does not farm, and `libz.so.1` -- which only CI found, because
this machine had zlib installed for unrelated reasons.

Its 2026.09.07 version moves one more thing across the boundary. A SYCL
artifact using the CUDA back end had to declare `compat:cuda-runtime` beside
it, because the adapter reaches `libcuda.so.1` through the artifact's own path.
That is a true statement about the implementation and a poor one to put in a
user's manifest: which back end the SYCL runtime dlopens is the runtime's
business. The adapter now declares the sentinel and links the same versioned
soname `compat.cuda-runtime` links, so a project that writes SYCL declares one
compat entry and no CUDA at all.

**Five pull requests across three repositories, not three.** The dependency
order is a cycle if each repository takes one: the plugin collection's CI needs
the released engine AND the index adapter, while the engine's examples need the
released collection. Round 3 met the same shape and resolved it the same way.
The order is: mcpp (engine) and mcpp-index (adapter) in parallel, then
mcpp-plugins, then mcpp-index (descriptor), then mcpp (examples).

### 7.6 Dynamic updates

| date | change | reason |
|---|---|---|
| 2026-09-06 | table created | - |
| 2026-09-06 | Z3 withdrawn | above |
| 2026-09-06 | N1's second criterion replaced | above |
| 2026-09-06 | `compat.sycl-runtime` added to the index row | the runtime closure check refuses the artifact without it |
| 2026-09-06 | the round is five PRs, not three | the release order is a cycle otherwise |
| 2026-09-06 | dpcpp corrected a second time (xim #769) | the first correction fixed the payload's own programs and broke every consumer of it; the regression was invisible from the payload |
| 2026-09-06 | `compat.sycl-runtime` 2026.09.07 (mcpp-index #355) | a SYCL project should not have to declare CUDA; the adapter owns the hop its runtime needs |
| 2026-09-06 | the SYCL island gains an async handler and an inner catch; the rule warns when `accel` names sycl and no device | exercising the rule's other branch showed the island could not keep the promise its own comment made; see 7.7 |

### 7.7 What exercising the second branch found

`mcpp.rules.sycl` has two branches: `accel = "sycl, cuda12.9+{sm_89}"` compiles
ahead of time for that architecture, and `accel = "sycl"` compiles to SPIR-V and
leaves the device to the runtime. The examples take the first. Taking the second
built correctly and then died on an RTX 4080 with

    terminate called after throwing an instance of 'sycl::_V1::exception'
    terminate called recursively

and no message of the program's own -- contradicting the fixture's own comment,
which said the island turns its failures into a return code.

Two hypotheses were wrong before the backtrace settled it, and both were worth
fixing anyway because each terminates on some machine:

* An exception unwinding through the three `sycl::buffer` destructors. A buffer
  destructor blocks until the work that reads it has finished, so a catch
  outside the scope unwinds through three destructors that are themselves
  finishing failed work. The catch moved inside the scope.
* The queue's asynchronous handler. SYCL delivers most device errors to it, and
  a queue constructed without one gets the default handler, which calls
  `std::terminate`. No catch intercepts that, because it never travels as an
  exception through the frame. A handler is installed.

The actual cause is neither. `ProgramManager::getDeviceImage` throws inside the
SYCL scheduler because the CUDA back end does not consume SPIR-V, and that path
reaches neither the caller's frame nor the queue's handler. Nothing the island
can write catches it.

So the rule says it at build time instead, as an advisory rather than a
refusal: the SPIR-V form is correct and portable, and the rule cannot know the
machine's devices -- which is exactly why the statement belongs before the
first kernel rather than in a crash with no message.

The general shape, which is the reusable part: **a promise a comment makes
about error handling is only tested by the configuration that fails.** Every
run on this machine took the path where nothing goes wrong, and the comment
read as true for as long as that held.

### 7.8 Ecosystem review: what a consumer sees after this round

A project that wants a GPU now writes the same four things whichever
programming model it picks, and the fourth is the only one that names the
model.

```toml
[dependencies.mcpp]
plugins = { version = "0.2.0", features = ["rules-sycl"], host-module = true }

[dependencies.compat]
sycl-runtime = "2026.09.07"     # the runtime the artifact reaches through the loader

[xlings.workspace]              # payloads: the project picks the versions
"xim:dpcpp"     = "7.1.0"
"xim:gcc"       = "15.1.0"
"xim:cuda-nvcc" = "12.9.86"

[build]
accel = "sycl, cuda12.9+{sm_89}"
```

Four properties hold across all four lanes, and each is enforced by something
rather than asserted here.

**The device is spelled once.** `cuda12.9+{sm_89}` is character for character
the same in the CUDA, HIP and SYCL manifests. The first chunk names the
programming model; the second names the device. A rule reads the chunk it owns
and ignores the rest, so adding a fifth model adds no spelling.

**The engine still knows no vendor.** `test_core_vendor_probes` strips comments
from `src/` and refuses any vendor tool name, with the file count as its own
denominator. Three rules and four payloads later, that test is unchanged.

**No device command line reaches the host.** Asserted per lane in the plugin
collection's CI as a separate step from the build, because all three lanes
compiled successfully on a developer machine while reading `/usr/include` --
twice in this round alone, once for the SYCL unit's C++ standard library and
once for clang's CUDA installation. Measured here: zero `/usr` paths across
CUDA, HIP and SYCL.

**A payload is reachable, not merely installed.** The round's largest single
defect was a payload whose five reporting programs could not start; the round's
second was the correction to it, which fixed the payload and broke every
consumer. Both are now assertions: the programs start and carry DT_RPATH, and
the payload's libraries carry no search path at all.

Three things this round could not close, each with what would decide it:

| open | what would decide it |
|---|---|
| Intel `anv` in the Mesa payload | a libclc and SPIRV-LLVM-Translator chain, then a Mesa rebuild; both packages exist on conda-forge and were confirmed repackable, and are withheld because nothing yet consumes them |
| the AMD platform of HIP | a ROCm runtime and device library in `xim-pkgindex`. The rule refuses it by name today rather than emitting an object nothing can link |
| llama.cpp's Vulkan lane | its blocker is gone -- `glslc` is published -- and the remaining work is in that project's own `-m` repository rather than here |

And one thing measured rather than assumed: `accel = "sycl"` without a device
compiles to SPIR-V, which the CUDA back end does not consume. The build is
correct, the failure is the machine's, and the rule now says so before the
first kernel. See 7.7.
