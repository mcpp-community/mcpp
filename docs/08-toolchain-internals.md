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
detect / probe                   ← triple, sysroot, payload paths (glibc, linux-headers)
        ▼
ToolchainLinkModel (single resolver for the C-library axis)
        ├──► flags.cppm        (main build compile/link flags)
        ├──► stdmod.cppm       (`import std;` BMI precompile)
        ├──► build_program     (build.mcpp host compiles)
        └──► cfg regeneration  (the human-facing clang++.cfg)
        ▼
hermetic link check (`-###` dry-run)  ← asserts CRT/loader resolve inside the sandbox
```

Two principles run through everything:

1. **Sandbox toolchains are self-contained.** A produced binary's CRT startup
   objects, libc, and dynamic linker come from sandbox payloads — never
   silently from the host. On a machine with no compiler and no
   `/usr/lib/**/Scrt1.o` (fresh WSL2, minimal containers), everything still
   works; on a machine *with* a host toolchain, nothing leaks in.
2. **Path knowledge has one owner per layer.** What used to be four divergent
   copies of "how to link against the payload glibc" is now one resolver
   (`linkmodel`); what used to be per-entry-path fixup behavior is now one
   pipeline. Divergence between copies is where an entire class of bugs came
   from (issue #195).

## 2. Toolchain resolution

A toolchain spec (`gcc@16.1.0`, `llvm@22.1.8`, `gcc@15.1.0-musl`) maps to an
xim package (`src/toolchain/registry.cppm`: `parse_toolchain_spec` →
`to_xim_package`, producing an `XimToolchainPackage` with the xim name,
version, and frontend candidates). The payload is resolved/auto-installed via
the xlings backend into the sandbox
(`$MCPP_HOME/registry/data/xpkgs/xim-x-<name>/<version>/`).

`detect`/`probe` (`src/toolchain/detect.cppm`, `probe.cppm`) then derive:

| Field | How |
|---|---|
| `targetTriple` | `<compiler> -dumpmachine` |
| `sysroot` | `-print-sysroot` (validated: must actually carry libc headers), with a remap fallback for xlings-built GCC whose baked build-time path doesn't exist locally |
| `payloadPaths` | sibling xpkg discovery: glibc payload (`include/` + `lib64|lib/`) and linux-headers payload — the *payload-first* fine-grained sysroot |
| runtime dirs | toolchain-private lib dirs for produced binaries' `-L`/`-rpath` |

Note the probe deliberately does **not** mine the clang cfg for `--sysroot`
anymore: the cfg is an output of this machinery, not an input (§5).

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
CLibMode::None           nothing usable — host defaults apply and the hermetic
                         check (§6) reports whatever leaks in
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

Sandbox payloads are prebuilt ELF trees. Two kinds of paths baked into them
are unknowable at packaging time and must be aligned to the *local* sandbox:
`PT_INTERP`/`RUNPATH` inside binaries, and the loader/rpath lines inside GCC
specs. `ensure_post_install_fixup(cfg, payloadRoot, pkg)` is the **single
entry** for that alignment, called from all three entry paths (explicit
install, default auto-install, manifest auto-install).

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
| `gcc` (glibc) | patchelf walk over the gcc payload **and the shared binutils payload** (PT_INTERP → sandbox loader, RUNPATH → glibc+gcc lib dirs); specs rewrite (baked loader/rpath → payload glibc, specs-grammar-aware — `%{...}` conditionals must never be corrupted) |
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
- Specs rewriting is content-aware (already-aligned specs are skipped).
  Extending the same check to the patchelf walk (compare
  `--print-interpreter`/`--print-rpath` before writing, so an already-aligned
  payload converges with **zero writes**) is a known follow-up.
- The long-term direction is for the *installer* (xlings) to own all
  writes — at install time and when a payload enters a new home — leaving
  mcpp read-only + verification. The pipeline here is the compatibility
  layer until then, and the self-healing mechanism for drift either way.

## 5. The clang cfg: direct-invocation support only

`bin/clang++.cfg` exists so that direct invocations of the bundled
`clang++` (outside mcpp) get a working, hermetic compiler configuration. mcpp's own builds never read it
(`--no-default-config` always). The fixup pipeline **regenerates** it
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
2. **Registry** (`src/toolchain/registry.cppm`): teach
   `parse_toolchain_spec`/`to_xim_package` the spec spelling, xim package
   name, and `frontendCandidates` (which binary is the C++ driver).
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
