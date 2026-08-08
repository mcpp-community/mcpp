# 08 — Toolchain Internals

> How mcpp's toolchain machinery works under the hood, and how to extend it
> with new toolchains, new architectures, and (eventually) embedded targets.
> Companion to [03 — Toolchain Management](03-toolchains.md), which covers the
> user-facing CLI. This document is for contributors and maintainers.

## 1. The model in one picture

```
mcpp.toml [toolchain]  /  global default  /  `mcpp toolchain install`
        │  (three entry paths — ONE shared pipeline)
        ▼
resolve payload (xim:gcc / xim:llvm / xim:musl-gcc xpkg under the sandbox)
        ▼
ensure_post_install_fixup()      ← idempotent convergence (marker-gated)
        ▼
resolve runtime binding          ← which libc the artifact will load (§2.1) — an
                                   answer, not a search
        ▼
detect / probe                   ← triple, sysroot, payload paths (glibc, linux-headers)
        ▼
ToolchainLinkModel (single resolver for the C-library axis)
        ├──► flags.cppm        (main build compile/link flags)
        ├──► stdmod.cppm       (`import std;` BMI precompile)
        ├──► build_program     (build.mcpp host compiles)
        └──► cfg regeneration  (the human-facing clang++.cfg)
        ▼
hermetic link check (`-###` dry-run)  ← checks sandbox CRT/loader resolution
```

Two principles run through everything:

1. **Sandbox toolchains are hermetically checked by default.** For the normal
   payload-first or sysroot path, a produced binary's CRT startup objects,
   libc, and dynamic linker must resolve under allowed sandbox prefixes. This
   is not an unconditional containment guarantee: `CLibMode::None` falls back
   to host defaults, system/PATH compilers are an explicit host-world choice,
   and `[build] allow_host_libs = true` or `MCPP_ALLOW_HOST_LIBS=1` opt out of
   the host-library check. On a machine with no compiler and no
   `/usr/lib/**/Scrt1.o` (fresh WSL2, minimal containers), the normal sandbox
   path still works.
2. **Path knowledge has one owner per layer.** What used to be four divergent
   copies of "how to link against the payload glibc" is now one resolver
   (`linkmodel`); what used to be per-entry-path fixup behavior is now one
   pipeline. Divergence between copies is where an entire class of bugs came
   from (issue #195).

## 2. Toolchain resolution

Since 0.0.93 identity is two orthogonal axes: a `ToolchainSpec` is
`(family ∈ gcc|llvm|msvc, version, target Triple)`. `triple.cppm` is the
single triple parser + the closed known-target vocabulary; `compat.cppm` is
the only file that knows legacy spellings (`gcc@15.1.0-musl`, `musl-gcc`,
`mingw`, `mingw-cross`, `clang`, `<triple>-gcc` — normalized on parse,
permanently accepted). `to_xim_package` is a *(family, target, host)* payload
mapping producing an `XimToolchainPackage` with the xim name, version, and
frontend candidates — this is where host-split distribution names like
`mingw-cross-gcc` (Linux host) vs `mingw-gcc` (Windows host) live; they are
current distribution-layer identity, not user-facing spellings. The payload
is resolved/auto-installed via the xlings backend into the sandbox
(`$MCPP_HOME/registry/data/xpkgs/xim-x-<name>/<version>/`). See
`.agents/docs/2026-07-15-toolchain-target-naming-unification-design.md`.

`detect`/`probe` (`src/toolchain/detect.cppm`, `probe.cppm`) then derive:

| Field | How |
|---|---|
| `targetTriple` | `<compiler> -dumpmachine` |
| `sysroot` | `-print-sysroot` (validated: must actually carry libc headers), with a remap fallback for xlings-built GCC whose baked build-time path doesn't exist locally |
| `payloadPaths` | the resolved runtime binding (§2.1) names the glibc payload exactly; linux-headers is still discovered as a sibling. No binding ⇒ no payload-first, by design |
| runtime dirs | toolchain-private lib dirs for produced binaries' `-L`/`-rpath` |

Note the probe deliberately does **not** mine the clang cfg for `--sysroot`
anymore: the cfg is an output of this machinery, not an input (§5).

### 2.1 The runtime binding — which libc, decided once

A payload-first build links against a specific glibc, and *which* one is a
fact about the environment, not something to be inferred. mcpp resolves it in
order:

1. `[xlings] subos = "<name>"` — the named subos, a sibling of the active one,
   describes its own runtime in the `subos_info` block of its `.xlings.json`
   (xlings 2026.8.5.1+).
2. The active subos, same block.
3. *Compatibility.* A subos created before that block existed cannot answer.
   The value baked into the toolchain itself then stands in — gcc's specs,
   clang's cfg. This is the value the artifact **would** load, so compile side
   and run side still agree; it retires itself the moment the subos can speak.

No binding is a **refusal**, not a default: `CLibMode::PayloadFirst` is
declined rather than picking a libc.

The rule this replaced asked a directory for "the glibc" and took the first
entry `readdir` yielded. With one glibc installed that is always right, so
nothing forced it to be correct — and a dependency carrying `xim:glibc@>=2.38`
is enough to install a second one. When that happened in mcpp-index, the
compile side took 2.44 while the artifact's interpreter, frozen in gcc's specs
at install time, still named 2.39; binaries referenced `GLIBC_2.42` symbols
against a runtime without them, and the failures surfaced on packages
unrelated to the dependency that pulled the second glibc in. Directory order
is not a decision procedure.

Because the binding decides what the artifact loads, it is part of the
toolchain fingerprint (11 fields, not 10) — two builds differing only in
runtime must not share a cache entry.

## 3. The link model (`src/toolchain/linkmodel.cppm`)

`ToolchainLinkModel` answers exactly one question — *how do we compile and
link against this toolchain's C library* — and every consumer derives its
flags from it:

```
CLibMode::PayloadFirst   glibc/linux-headers xpkgs found (the normal bundled-LLVM
                         and no-usable-sysroot GCC case)
                           compile: -isystem (clang) / -idirafter (gcc) payload headers
                           link:    -B <glibcLib>   ← CRT discovery (Scrt1.o/crti.o/crtn.o;
                                                       the driver never consults -L for these)
                                    -L <glibcLib> [+ -rpath + --dynamic-linker for clang]
CLibMode::Sysroot        a usable --sysroot (GCC include-fixed world, self-contained
                         musl sysroots, the macOS SDK)
                           link:    --sysroot, plus --dynamic-linker and -L/-rpath
                                    for the payload when one is known — the
                                    sysroot says where headers live, not which
                                    loader runs the result
CLibMode::None           nothing usable — host defaults apply; the hermetic
                         check (§6) rejects that leakage unless an explicit
                         host-library exception is in effect
```

`ClangDriverModel` is the companion for bundled LLVM: mcpp always passes
`--no-default-config` (bypassing the install-time cfg for reproducibility)
and re-provides libc++ headers/libs plus
`-fuse-ld=lld --rtlib=compiler-rt --unwindlib=libunwind` explicitly.

**Loader resolution** is data-driven, never hardcoded: a per-arch triple map
(x86_64 / aarch64 / riscv64 / loongarch64 / i686, glibc and musl spellings),
then a `ld-*.so*` glob of the payload as the fallback for arches the map
doesn't know. A third source — declared metadata persisted by the installer
(`.xpkg-exports.json`) — was implemented, evaluated, and **removed**: its
only consumer would have been this resolver, the two sources above already
cover every real payload (the entire 0.0.83 verification matrix ran green
without the file ever existing), and a general-purpose package manager
shouldn't carry a mechanism whose sole reader is one downstream tool. If an
installed-state metadata DB ever appears, it must be designed with xlings
itself as its first consumer; mcpp can then re-add a reader.

## 4. The unified post-install fixup pipeline (`src/toolchain/post_install.cppm`)

Sandbox payloads are prebuilt ELF trees whose baked `PT_INTERP`/`RUNPATH` are
unknowable at packaging time and must be aligned to the *local* sandbox.
`ensure_post_install_fixup(cfg, payloadRoot, pkg)` is the **single entry** for
that alignment, called from all three entry paths (explicit install, default
auto-install, manifest auto-install).

> This pipeline used to rewrite GCC's `specs` file as well, so that produced
> binaries got a loader and an rpath. That is no longer done, and §5 is why.

> Historical note: before 0.0.83 each path remembered — or forgot — its own
> subset. The manifest path ran *nothing*, which is how a freshly
> auto-installed llvm kept a stale, environment-dependent cfg (issue #195),
> and how gcc once shipped a sandbox that couldn't find `stdlib.h`. "Which
> command you installed with" must never decide "whether the toolchain
> works".

**Trigger semantics — ask every build, act once:**

```
every build → ensure() → read <payload>/.mcpp-fixup.json
                          marker == {schema, kind, rev, glibcLib}?  → return   (ms-level)
                          mismatch → run the fixup for this kind, write marker
```

The marker is a *content-fingerprinted cache*, not an event flag: it encodes
the fixup revision and the glibc payload it was aligned against. The
"act" branch therefore fires exactly once per
`(payload × fixup-rev × glibc-fingerprint)` — first use, plus the two
re-convergence events that genuinely require rewriting (a fixup-logic
upgrade via `kFixupRev`, or the glibc payload changing underneath). mcpp
asks on every build because the events that invalidate a payload (xlings
swapping glibc, a payload inherited from another home) happen outside
mcpp's sight — trust-but-verify is the only reliable semantic.

**Per-kind actions:**

| kind | actions |
|---|---|
| `gcc` (glibc) | patchelf walk over the gcc payload **and the shared binutils payload** (PT_INTERP → sandbox loader, RUNPATH → glibc+gcc lib dirs) — so that *gcc itself* runs. Nothing is written into `specs` |
| `llvm` | patchelf walk over `lib/` only (runtime `.so` RUNPATH; `bin/` is left alone to preserve xlings-set RUNPATHs); deterministic cfg regeneration (§5) |
| `musl-gcc` | nothing — self-contained sysroot, static world |

**Safety invariants** (each earned by a real incident):

- **Never patch in place.** patchelf operates on a copy which is then
  atomically `rename()`d in: the payload can contain libraries the *current
  process* (a self-hosted, dynamically linked mcpp) or a concurrent build
  has mmapped, and rewriting a live mapping's backing file corrupts the
  running process (observed: exit-time SIGSEGV in `_dl_fini`). `rename` gives
  new content a fresh inode; live processes keep the old one.
- **Ownership guard.** Payloads that resolve outside this home's registry
  (symlink-inherited from another `MCPP_HOME`) are never patched — their
  owner already converged them, and patching through the symlink would brick
  the owner's toolchain.
- Extending content-awareness to the patchelf walk (compare
  `--print-interpreter`/`--print-rpath` before writing, so an already-aligned
  payload converges with **zero writes**) is a known follow-up.
- The long-term direction is for the *installer* (xlings) to own all
  writes — at install time and when a payload enters a new home — leaving
  mcpp read-only + verification. The pipeline here is the compatibility
  layer until then, and the self-healing mechanism for drift either way.

## 5. The compiler is a capability, not a configuration

A payload ships two separable things: the ability to compile, and an opinion
about how to link. mcpp wants the first and supplies the second itself — the
link line is where a build's decisions belong, because it is the thing that
varies per build. Both compilers now follow that rule; only the mechanism
differs.

**clang** — `--no-default-config` on every mcpp invocation.

**gcc** — `-specs=` with a generated file. `<compiler> -dumpspecs` prints the
**built-in** specs (unaffected by anything on disk); mcpp extracts the `*link:`
body, drops the loader and rpath lines from it, and writes the result to the
build directory. A `-specs=` file without a leading `+` *replaces* the rule it
names, so the payload's own opinion is overridden without the payload being
touched. Two consequences worth stating: it is per-build, so a second project
on the same machine is unaffected; and it needs no write access to the
toolchain, so an inherited or read-only payload works.

Removing gcc's baked `*link:` also removes what it provided. mcpp therefore
supplies `--dynamic-linker` and every rpath entry explicitly on the link
line — the loader, the glibc lib dir, and gcc's own `lib64` (libgcc_s). Each
of those was found by removing the specs and watching what broke.

Why not keep rewriting `specs`? Because the file is shared and the value is
per-build. The rewrite had a single-path needle and a two-path replacement, so
every home that ever ran it left one entry behind: **68** stale `RUNPATH`
entries, all naming deleted `mktemp` directories, in every gcc artifact one
developer machine produced. Nothing detected them, because a dead RUNPATH
entry costs only search time. e2e `201_gcc_no_specs_pollution.sh` asserts on
the artifact, not the specs file — what a user ships is what matters.

### 5.1 The clang cfg

`bin/clang++.cfg` exists so that direct invocations of the bundled
`clang++` (outside mcpp) get a working, hermetic compiler configuration. The fixup pipeline **regenerates** it
deterministically from the link model — same payload ⇒ byte-identical cfg on
every machine and install path — rather than line-patching whatever an
install produced. On Linux that means CRT discovery (`-B`), payload loader +
rpath, lld/compiler-rt/libunwind, and bundled libc++ for the C++ drivers; on
macOS it keeps the historical shape (`--sysroot=<SDK>` + payload libc++
headers — the C++ *runtime link* stays with the platform's
`needs_explicit_libcxx` handling in the main build).

## 6. The hermetic link check (`src/build/hermetic.cppm`)

Before running a build with a sandbox toolchain on Linux, mcpp dry-runs the
driver with the exact link flags (`-### -x c++ /dev/null`) and asserts every
CRT object and the *effective* dynamic linker (last occurrence wins) resolve
under allowed sandbox prefixes. This turns both silent failure modes into
one actionable diagnostic: bare CRT names that lld can't open (the #195
symptom on clean machines) and quiet host-CRT contamination (which made
green CI a false signal on machines with a host toolchain). The verdict is
cached per flag-set (`.mcpp-hermetic-ok`); escape hatches:
`[build] allow_host_libs = true` or `MCPP_ALLOW_HOST_LIBS=1`. System/PATH
compilers are exempt — using the host world explicitly is the user's choice.

CI keeps this honest with a job that has **no host toolchain at all**
(`debian:stable-slim`, no gcc, no host `Scrt1.o`) — the only environment
class that faithfully reproduces the clean-machine failure mode, plus e2e
`86_llvm_hermetic_link.sh` which re-checks the `-###` resolution on every
machine.

## 7. Extending the machinery

### 7.1 Adding a new toolchain (new compiler family or distribution)

1. **Index side** (xim-pkgindex): a package with the payload assets and —
   critically — `deps` on whatever C library payload it needs (`xim:glibc`,
   `xim:linux-headers`). Follow the llvm/gcc packaging SOP including the
   admission gate (`verify-toolchain.sh`): completeness + hermetic CRT
   resolution + a real compile/link/run before an asset ships.
2. **Vocabulary + registry**: add the target row to `triple.cppm`'s
   `kKnownTargets` (tier/pin/defaultStatic), then teach
   `to_xim_package` (`src/toolchain/registry.cppm`) the *(family, target,
   host)* → xim package row and its `frontendCandidates` (which binary is
   the C++ driver). Legacy spellings, if any, go in `compat.cppm` only.
3. **Capabilities** (`src/toolchain/provider.cppm`): stdlib identity, BMI
   traits, and feature switches consumed by `flags.cppm`.
4. **Fixup kind** (`post_install.cppm`): decide what post-install alignment
   the payload needs — gcc-like (patchelf + specs), llvm-like (lib patchelf +
   cfg), or none (self-contained). Wire it into
   `ensure_post_install_fixup`'s dispatch.
5. **e2e**: a hermetic-link test in the spirit of
   `86_llvm_hermetic_link.sh`, and coverage in the no-host-toolchain CI job.

### 7.2 Adding a new CPU architecture (Linux)

The machinery is already arch-parameterized; the work is data:

1. add the glibc/musl loader names to the triple map in
   `linkmodel.cppm::loader_filename` (the glob fallback covers you until
   then);
2. ship payload assets for the arch (glibc, linux-headers, the toolchain
   itself) — the aarch64-linux-musl cross target is the working precedent
   (`[target.aarch64-linux-musl]`, cross frontend resolution via the spec's
   `targetTriple`);
3. nothing else: `-B`/`-L`/loader emission, the fixup pipeline, and the
   hermetic check are all name-agnostic.

### 7.3 Embedded / bare-metal toolchains (outlook)

The model extends naturally to `arm-none-eabi`-class toolchains because the
hard parts of the hosted world *disappear* rather than multiply:

- **No dynamic linker**: `loader` stays empty — already legal everywhere
  (renderers omit `--dynamic-linker`; the pack/deploy story is flashing, not
  ELF interp).
- **No glibc payload**: newlib/picolibc live inside the toolchain's own
  sysroot ⇒ `CLibMode::Sysroot`, the exact mode self-contained musl uses
  today. `is_musl_target`-style self-containment detection generalizes to a
  capability flag ("ships own C library").
- **Fixup kind = none or gcc-like** depending on how the payload is built
  (a cross gcc payload still wants PT_INTERP/RUNPATH alignment for the
  *host-run* compiler binaries — that part is identical to today's gcc kind;
  the *target* side needs nothing).
- **Hermetic check** generalizes: assert crt0/semihosting stubs resolve
  inside the toolchain payload instead of Scrt1.o/loader.
- What genuinely needs new design: per-target `[target.'cfg(...)']` specs
  for MCU flags (`-mcpu`, `--specs=nosys.specs`), linker-script handling,
  and a run/flash story — build-graph concerns above this document's layer.

### 7.4 Non-ELF platforms

macOS (Mach-O) and Windows (PE) intentionally bypass most of this document:
macOS resolves its C world from the SDK (`CLibMode::Sysroot`) with its own
libc++ linkage handling; Windows has no rpath — mcpp deploys runtime DLLs
next to the produced exe, which is the platform's native equivalent of
everything §3–§4 does for ELF.

## 8. Source map

| Concern | File |
|---|---|
| spec → xim package, frontends | `src/toolchain/registry.cppm` |
| detect/probe (triple, sysroot, payloads) | `src/toolchain/detect.cppm`, `probe.cppm` |
| link model + loader resolution | `src/toolchain/linkmodel.cppm` |
| unified fixup pipeline (patchelf/specs/cfg, marker) | `src/toolchain/post_install.cppm` |
| install/lifecycle entry | `src/toolchain/lifecycle.cppm`; auto-install entries in `src/build/prepare.cppm` |
| flag assembly (main build) | `src/build/flags.cppm` |
| `import std;` precompile | `src/toolchain/stdmod.cppm` |
| build.mcpp host flags | `src/build/build_program.cppm` |
| hermetic link check | `src/build/hermetic.cppm` |
| regression fences | `tests/e2e/86_llvm_hermetic_link.sh`, unit `test_linkmodel.cpp`, `test_post_install.cpp`; the no-host-toolchain CI job in `ci-linux-e2e.yml` |

Design history: `.agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md`.
