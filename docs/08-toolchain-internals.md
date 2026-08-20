# 08 — Toolchain Internals

> How mcpp's toolchain machinery works under the hood, and how to extend it
> with new toolchains, new architectures, and (eventually) embedded targets.
> Companion to [03 — Toolchain Management](03-toolchains.md), which covers the
> user-facing CLI. This document is for contributors and maintainers.

## 1. The model in one picture

```
mcpp.toml [xlings].subos / mcpp-managed default runtime
        ▼
resolve runtime binding          ← which libc the artifact will load (§2.1) — an
                                   answer, not a search
        ▼
resolve toolchain payload        ← project/default/install paths share one pipeline
        ▼
ensure_post_install_fixup()      ← exact glibc@version, marker-gated; never readdir-first
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
        ▼
link → internal ELF physics check    ← validates the artifact and resolved closure (§6.1)
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
fact about the root project's local development OS, not something to infer
from a compiler path or shell. mcpp has exactly two selection modes:

1. No `[xlings].subos`: `McppDefault`, the initialized `subos/default` in the
   xlings home selected by global mcpp configuration.
2. `[xlings] subos = "<name>"`: `NamedSubos(name)`. Explicit `"default"`
   remains a named selection; other names resolve in the root project's local
   xlings scope.

The workspace root owns the selection during a workspace build. Member and
dependency declarations do not merge or propagate; the same member's
declaration applies only when that source is an independent root. Neither
`XLINGS_ACTIVE_SUBOS`, `current`, the compiler's owner home, nor a CLI/env
override is a third selection rung.

A named environment that does not exist is a hard error, never a fallback to
default/active/compiler-baked state — the request cannot be satisfied, and
substituting a different one would make one `mcpp.toml` mean different ABIs on
different machines.

A SubOS that exists but **does not describe itself** is a different case, and it
degrades. `declared = false` is recorded with a note the caller prints; runtime
rules report `inconclusive` instead of a verdict, and no payload-first binding is
available (mcpp declines rather than guessing a libc version, so the hermeticity
check will report the host fallback). The build is not stopped. Likewise a
`subos_info` schema **newer** than this mcpp understands is read for the fields it
knows, with a note — the same rule the reader itself documents: publishing data
must not invalidate the program that reads it. Refusing outright is what stopped
every `mcpp build` and `mcpp test` on Windows in 2026.8.10.2, where xlings writes
no such block and the facts it carries do not exist (openxlings/xlings#543).

mcpp reads the SubOS once into a
`RuntimeBinding` snapshot, feeds its libc identity into payload probing, and
reuses the same snapshot for configure/link/run/test and the fast-path cache.
On Linux the snapshot also records the canonical selected loader/libc directory
and the optional creation-host glibc floor. These are evidence used by the
post-link validator, not another selection mechanism.

The resolved SubOS view is authoritative when it canonically names one managed
glibc payload. Older xlings state may retain `runtime = "glibc@2.39"` after the
view has atomically moved to the managed 2.44 payload; mcpp records the physical
2.44 identity and path in that case. If an older view contains a broken link,
mcpp may resolve only the exact payload named by `runtime`; it never enumerates
installed versions or chooses a nearest/newest one. Both paths consume xlings
facts and preserve one RuntimeBinding rather than introducing an mcpp policy.

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

Because the binding decides what the artifact loads, the complete canonical
contract hash is field 11 of the toolchain fingerprint — two named SubOS
environments that happen to use the same libc still must not share a cache
entry when their providers or environment declarations differ.

### 2.2 Generic runtime providers and artifacts

`RuntimeBinding` also carries the provider-neutral facts selected by xlings for
the chosen development OS. `subos_info.runtime_contract` is an optional,
additive schema-1 block:

```json
{
  "providers": [
    {"capability": "display.present", "provider": {
      "namespace": "xim", "name": "display-runtime", "version": "1.0.0",
      "source": "xim-pkgindex@<revision>"}}
  ],
  "artifacts": [
    {"role": "driver", "provider": {
      "namespace": "xim", "name": "display-runtime", "version": "1.0.0",
      "source": "xim-pkgindex@<revision>"},
     "path": "${subosdir}/lib/runtime/provider.so",
     "provenance": "subos_view", "abi": "elf-x86_64",
     "digest": "sha256:...", "host_fingerprint": "..."}
  ]
}
```

The binding parser resolves `${subosdir}` and relative artifact paths once,
sorts the facts, and includes them in the contract hash and cache snapshot. In
the build plan these selected provider facts precede descriptor-declared
fallbacks. Descriptor requirements and artifacts are independently stamped
with their resolved requester's/provider's canonical PackageId, so equal short
names in different namespaces remain distinct end to end.

The ownership boundary is deliberate: mcpp-index expresses generic runtime
requirements; xlings/xim chooses and diagnoses the host graphics/runtime stack;
mcpp consumes only the selected provider/artifact facts and generic LinkIntent.
mcpp has no hardware, driver-vendor, WSL, or ICD selection path. Its source gate
rejects introducing such provider-specific branches or coupling those terms to
a launched probe.

`LinkIntent` keeps `linkLibraryDirs`, `transitiveNeededDirs`, and
`runtimeSearchDirs` separate. The last category is never rendered as `-L`:
ELF receives rpath plus `-rpath-link` only for the transitive category, Mach-O
receives rpath/framework flags, and PE receives link-library paths plus explicit
deploy-file copy edges. The exact RuntimeBinding, canonical identities, link
intent, search mechanism, and post-link verdict are persisted in
`resolution.json` schema 2. `mcpp why runtime` only interprets that stored file;
re-diagnosis belongs to `xlings doctor`.

### 2.3 The run-time search closure (`src/platform/runtime_search.cppm`)

mcpp passes `--sysroot=<subos>` on the compile **and** link lines, so a library
the SubOS provides — `-lGL`, `-lX11`, `-lwayland-client` — resolves with no
flags from the user. The run-time search path has to be derived from the same
decision, or a link succeeds and the artifact cannot start.

The closure is one ordered list, each entry tagged with where it came from:

| origin | example | mutable? | ships? |
|---|---|---|---|
| `payload` | `<store>/xim-x-glibc/2.39/lib64` | no — written once at install | no |
| `package` | a dependency's `[runtime]` dir | no | no |
| `subos_farm` | `<subos>/lib` | **yes** — rewritten by every `xlings install` | no |
| `host_default` | `/usr/lib/x86_64-linux-gnu` | n/a — the target's own | n/a |

**Order is decreasing mutability, and the farm is last.** That is the whole
invariant: payload-first keeps `libc` / `libm` / `libstdc++` resolving from the
pinned payload, leaving the farm to supply only what nothing else does.
Farm-first would let a later `xlings install` change which libc an *already
linked* artifact loads.

Two guards decide whether the farm applies at all. The format must have a
search path (ELF; Mach-O and PE get nothing, matching the loader-tag contract),
and the target must be this host's runtime — a cross target gets no farm, since
it belongs to the host SubOS.

`host_default` enters the *model* only when the artifact will really run under
the host loader. A hermetic artifact's `PT_INTERP` names a private loader with
different built-in defaults, so including `/usr/lib` there models the wrong
loader — and since a developer machine usually has its own `libGL.so.1`, doing
so reported "resolved" for binaries that exited 127.

The closure is recorded in `resolution.json` under `runtime.search.closure` and
printed by `mcpp why runtime`. A `DT_NEEDED` that nothing on a hermetic
artifact's path can satisfy is a **proven** failure (`unresolvable`), not an
inconclusive one, and it fails the build. Measured with `LD_DEBUG=libs`: the
private loader's built-in default path is the glibc payload's own *build-time*
prefix, a directory that does not exist on the machine — `/usr/lib` is never
consulted.

Three things narrow that proof, and each of them is a case where mcpp knows less
than the wording suggests:

- **The artifact's format decides, not the binding's.** A cross build runs with
  this host's binding while producing a PE or Mach-O. ELF rules do not apply to
  the artifact whatever the binding says.
- **Only an unfindable SONAME proves anything.** "I could not read this object"
  and "I stopped after 512 objects" are statements about the *check*; a check
  that could not look has proven nothing, so those stay inconclusive.
- **`[build] allow_host_libs` opts out of both phases.** It already switches off
  the link-time hermeticity check; once resolution is the user's responsibility
  mcpp reports rather than blocks, because they may run under `LD_LIBRARY_PATH`
  or on a machine where the library sits where the private loader looks.

mcpp also declares `XLINGS_SUBOS_LD_PATHS=0` for every process it spawns. That
is the opt-out from xlings' linker-wrapper path injection
(openxlings/xlings#540): mcpp wants the wrapper's `--disable-new-dtags` and
must refuse its `-rpath "$XLINGS_SUBOS_LIB"`, because that variable names the
*active shell's* SubOS — mcpp keeps its own xlings home under
`<mcpp home>/registry`, so it generally points at a different farm backed by a
different physical glibc payload. mcpp emits the farm entry it derived from the
binding it actually selected.

## 3. The link model (`src/toolchain/linkmodel.cppm`)

`ToolchainLinkModel` answers exactly one question — *how does mcpp compile and
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
> command used to install it" must never decide "whether the toolchain
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

### 6.1 Post-link Linux runtime physics (`elf_runtime.cppm`)

The hermetic check answers a pre-link question: what does the driver appear to
resolve? The runtime-physics check answers the stronger post-link question:
what did the newly produced ELF actually record and what will its closure
load? `[build] allow_host_libs = true` deliberately relaxes the first check;
it does not suppress physical impossibilities in the second.

For each newly linked Linux executable/shared object, mcpp parses ELF64
little-endian program/dynamic/GNU-version tables internally—no `readelf`,
`patchelf`, or `ldd` subprocess on the build path—and records `PT_INTERP`,
`DT_RPATH`/`DT_RUNPATH`, `DT_NEEDED`, and required/defined `GLIBC_*` versions.
It resolves the declared closure using the artifact search paths, selected
runtime/toolchain directories, and known host library directories, then applies:

- **Rule B (same source):** `PT_INTERP` and every resolved `libc.so.6` must be
  the canonical payload selected by `RuntimeBinding`. A host loader plus private
  libc, or two private libc payloads, is a proven pre-main failure.
- **Rule A (version floor):** every closure request for `GLIBC_x.y` must be no
  newer than the selected libc's exported GNU version definitions. Linking a
  host DSO is allowed when this holds; mcpp is checking physics, not imposing a
  no-host-library policy.

Verdicts are typed: `Pass`, `ProvenMismatch`, or `Inconclusive`. Proven A/B
mismatches fail the build with canonical requester/provider/artifact paths and
a copyable SubOS remediation. Missing loader-cache/hardware closure data is
reported as inconclusive, never relabelled green. macOS and Windows use the
same interface as a typed no-op and never receive ELF/glibc rules.

The verdict is stored as `.mcpp-runtime-verdicts.json` beside `build.ninja`,
keyed by artifact stat fingerprint plus the complete runtime contract hash.
Hot no-op builds require a current passing record, compare artifact stats
before/after Ninja, and perform zero ELF parses. An unexpected relink drops to
the full path before success or execution. `mcpp self doctor` reports the same
stored verdict rather than re-probing a potentially different current host.

Post-install alignment follows the same identity rule: `glibc@2.44` resolves
only `<xpkgs>/xim-x-glibc/2.44/{lib64,lib}`. A missing/stale exact payload is an
error; another installed version is never a fallback.

### 6.2 Where a runtime search path is allowed to live (`runtime_env_contract.cppm`)

There are two ways to tell a loader where to look, and they differ by blast
radius, not by convenience:

| | reaches | |
| --- | --- | --- |
| `DT_RUNPATH` | the one object that carries it, and its `dlopen()` | per-binary |
| `LD_LIBRARY_PATH` | the process **and every process it ever spawns** | inherited, forever |

That second row is why the private libc payload is **binary-scoped**. A glibc's
`libc.so.6` and its `ld.so` are version-locked through `GLIBC_PRIVATE`: 2.44's
libc carries an undefined `__pointer_chk_guard` that only 2.44's own loader
exports. An mcpp-built program is fine — `PT_INTERP` names the private loader.
`/bin/sh` is not: its `PT_INTERP` names the **host** loader and no environment
variable can override it, so a `popen()`/`system()` child dies during
relocation, before `main`, with no output (mcpp#401; mcpp#291 is the same shape
one hop closer in, killing mcpp's own nested host tools).

So mcpp never publishes the private libc directory through the environment. It
does not need to: wherever a payload exists the link model already emits
`-Wl,-rpath,<glibc>` beside `--dynamic-linker`, which covers the case the
directory exists for — a `dlopen()` whose own `DT_NEEDED` closure does not
consult the executable's RUNPATH.

This is a scope, not a condition. "Only export it when a dependency might
`dlopen()`" still exports it, and the child that dies does not care why. Plain
dependency runtime directories keep their environment scope: they have no
loader coupling, so a host binary that stumbles onto them is at worst confused.

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
   `linkmodel.cppm::loader_filename` (the glob fallback holds until
   then);
2. ship payload assets for the arch (glibc, linux-headers, the toolchain
   itself) — the aarch64-linux-musl cross target is the working precedent
   (`[target.aarch64-linux-musl]`, cross frontend resolution via the spec's
   `targetTriple`);
3. nothing else: `-B`/`-L`/loader emission, the fixup pipeline, and the
   hermetic check are all name-agnostic.

### 7.3 Embedded and bare-metal toolchains

`riscv64-none-elf` and `riscv32-none-elf` are implemented, and the user-facing
account is [13 — Bare-Metal and Freestanding Targets](13-baremetal.md). This
section records how the resulting shape relates to the hosted model above.

Three of this section's earlier predictions held:

- **No dynamic linker.** `loader` stays empty, which every renderer already
  permitted; the deployment story is flashing rather than ELF interp.
- **The target side needs no fixup.** Host-run compiler binaries still want
  PT_INTERP/RUNPATH alignment, identical to today's gcc kind.
- **MCU flags, linker-script handling and a run story genuinely needed new
  design.** All three landed, and above this document's layer as predicted:
  ISA flags come from a one-row-per-target table in
  `src/freestanding/target.cppm`, the linker script arrives through the
  `link-script` build directive, and execution through `runner`.

One prediction was wrong, and the correction is the load-bearing part of the
design. The C library does **not** live inside the toolchain payload, so this
is not `CLibMode::Sysroot` with a different sysroot. picolibc is a separate
payload named by the target's own table row
(`sysroot = xim:picolibc-riscv@1.8.12` in `src/toolchain/triple.cppm`), on the
same footing as that row's compiler `pin`. Resolving it from the target rather
than from the toolchain is what keeps a bare-metal *package* from having to
name a libc, exactly as a hosted package never names glibc.

The freestanding link line is also **replaced** rather than extended — see
`src/freestanding/linkline.cppm` — because every hosted link flag is wrong
there rather than merely unnecessary. Anything appended to the ordinary ldflags
earlier in the pipeline is discarded, which is why the target sysroot's `-L`
is emitted on that line and not with the generic flags.

### 7.4 Non-ELF platforms

macOS (Mach-O) and Windows (PE) intentionally bypass most of this document:
macOS resolves its C world from the SDK (`CLibMode::Sysroot`) with its own
libc++ linkage handling; Windows has no rpath — mcpp deploys runtime DLLs
next to the produced exe, which is the platform's native equivalent of
everything §3–§4 does for ELF.

**Shared libraries a project PRODUCES** (`kind = "shared"`) do differ per format,
and the difference is not a flag spelling — it is what the artifact records about
itself:

| format | what the producer emits | what the consumer links |
|---|---|---|
| ELF | `-Wl,-soname,<n>` when declared | `-L` + `-l`, `-Wl,-rpath,$ORIGIN` |
| Mach-O | `-Wl,-install_name,@rpath/<file>` **always** | `-L` + `-l`, `-Wl,-rpath,@loader_path` |
| PE / MinGW | `-Wl,--out-implib,<lib>` | the **import library**, `-Wl,-Bdynamic` first |
| PE / MSVC | refused (no auto-export; see docs/12) | — |

Three of those rows are new: everything but ELF was refused before, native or
cross. Two details had to change for them to be usable rather than merely
allowed. Mach-O's install name defaults to the path the library was LINKED at, so
emitting it only when a `soname` was declared would have left every other
`.dylib` recording a build directory — fine on the machine that built it, `image
not found` anywhere else. And the choice was made with `#if defined(__APPLE__)`
on the HOST, which is right only by coincidence on a native macOS build and wrong
for any cross link; it is decided from the target now, like `target_output`
already was.

An **unservable target is refused** rather than quietly built for the host:
`--target x86_64-windows-msvc` on Linux used to resolve the native `g++`, write
`target/x86_64-linux-gnu/`, and report success. The vocabulary tier says "mcpp
supports this target"; `host_can_serve` (`registry.cppm`) answers the different
question "can this machine produce it", and `prepare.cppm` now asks it — with an
explicit `[target.X] toolchain = "…"` as the escape hatch for a cross toolchain
supplied by the author.

### 7.5 Which axis decides a flag

Four flags changed in the 2026.8.18 round, and each had been keyed on the wrong
axis. Every one of those mistakes showed up the same way: an inexplicable
failure on exactly one platform, with a message that named neither the flag nor
the decision behind it.

There are three axes, and the question that picks between them is **who finally
reads this flag**.

| axis | the question | examples | how it is asked |
|---|---|---|---|
| **target format** | what kind of image is produced | `-fPIC` (PE code is position independent by design; clang refuses the flag outright) | `triple::parse(...)->is_pe()`, host fallback |
| **target ABI** | which linker will consume this | `--out-implib` vs `/IMPLIB:`, `/DEF:`, the SONAME / install-name form | `is_msvc_target(tc)`, `triple->is_msvc_env()` |
| **dialect** | which program mcpp is invoking | `-L` vs `/LIBPATH:`, `-I` vs `/I`, the archive command | `dialect_for(tc)`, `LinkStyle::SeparateLinker` |

**Clang targeting the MSVC ABI is the case that separates all three.** It speaks
the GNU dialect, produces MSVC-ABI objects, and emits a PE image. Ask it the
wrong question and:

- keyed on the dialect, it is handed `-Wl,--out-implib` and lld-link answers
  `ignoring unknown argument` followed by a missing file;
- keyed on the ABI, it is handed `/LIBPATH:`, which a compiler driver does not
  take;
- keyed on the compiler binary, it is handed `-fPIC` and refuses to run at all.

The failure mode is always the same shape: the flag is spelled for a
neighbouring platform, and the diagnostic comes from a program three steps away
from the decision. `ninja_backend`'s `pe_link_flag` is where the linker-facing
answers live; the dialect table says, where its entry used to be, why it cannot
answer them.

## 8. Source map

| Concern | File |
|---|---|
| spec → xim package, frontends | `src/toolchain/registry.cppm` |
| detect/probe (triple, sysroot, payloads) | `src/toolchain/detect.cppm`, `probe.cppm` |
| link model + loader resolution | `src/toolchain/linkmodel.cppm` |
| unified fixup pipeline (patchelf/specs/cfg, marker) | `src/toolchain/post_install.cppm` |
| install/lifecycle entry | `src/toolchain/lifecycle.cppm`; auto-install entries in `src/build/prepare.cppm` |
| root runtime selection/binding | `src/platform/xlings/runtime_selection.cppm`, `src/platform/runtime_binding.cppm`, `src/platform/xlings/subos_info.cppm` |
| generic runtime contract + LinkIntent | `src/manifest/types.cppm`, `src/build/plan.cppm`, `src/build/flags.cppm` |
| stored resolution explanation | `src/build/prepare.cppm`, `src/build/runtime_validation.cppm`, `src/doctor.cppm` |
| flag assembly (main build) | `src/build/flags.cppm` |
| `import std;` precompile | `src/toolchain/stdmod.cppm` |
| build.mcpp host flags | `src/build/build_program.cppm` |
| hermetic link check | `src/build/hermetic.cppm` |
| regression fences | `tests/e2e/86_llvm_hermetic_link.sh`, unit `test_linkmodel.cpp`, `test_post_install.cpp`; the no-host-toolchain CI job in `ci-linux-e2e.yml` |

Design history: `.agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md`.
