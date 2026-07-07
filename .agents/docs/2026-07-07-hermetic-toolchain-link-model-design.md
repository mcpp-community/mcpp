# Hermetic toolchain link model — one-shot cross-repo fix for issue #195

Fixes [mcpp-community/mcpp#195] (llvm toolchain link fails with
`ld.lld: cannot open Scrt1.o/crti.o/crtn.o` on Linux) at the architecture
level: one coordinated change set across **mcpp**, **xim-pkgindex**,
**xlings-res** and (optionally) **xlings**, shipped as mcpp **0.0.83**.

Companion analysis precedents: PR #62 (payload-first sysroot, removed the
subos override), PR #119 (single deployment-target resolver on macOS — the
same "one resolver, many consumers" cure applied here to the Linux C-library
axis), PR #124 (macOS floor + static libc++ default).

---

## 1. Problem statement (what #195 actually is)

`mcpp` links Clang-with-cfg toolchains with `--no-default-config` and
re-provides all paths itself. On the payload path (glibc xpkg present — the
normal result of installing llvm, whose deps declare `xim:glibc`), the
compile side is complete (`-isystem` payload headers) but the **link side
provides only `-L`/`-rpath`/`--dynamic-linker` and no `-B`**
(`src/build/flags.cppm`, `payload_ld`). The Clang driver locates CRT startup
objects (`Scrt1.o`, `crti.o`, `crtn.o`) through `-B` prefixes and
sysroot-derived paths — **never through `-L`** — so it passes the bare file
names to `lld`, which fails.

Why it was invisible until now: on hosts that have a system C toolchain
installed (dev machines, CI runners with `libc6-dev`), the driver silently
falls back to the **host's** `/lib/x86_64-linux-gnu/Scrt1.o`. The link
"succeeds" by mixing host CRT + payload glibc + payload loader. On hosts
without one (the reporter's fresh WSL2 Ubuntu), the bug surfaces
immediately. Verified both ways with `clang++ -###`: without `-B` the CRT
resolves to host paths (or bare names); with `-B<glibc-payload-lib>` all
three CRT objects resolve inside the payload and the binary links and runs.

## 2. Root architecture defects (why a one-line `-B` is not enough)

The knowledge "how to compile/link against the payload glibc" currently has
**five divergent implementations across two repos** and no owner:

| # | Copy | Behavior today |
|---|------|----------------|
| 1 | `mcpp src/build/flags.cppm` (main build) | bypasses cfg (`--no-default-config`), compile side complete, **link side missing `-B`** ← #195 |
| 2 | `mcpp src/toolchain/stdmod.cppm` (std module precompile) | independent re-implementation of the compile side |
| 3 | `mcpp src/build/build_program.cppm` `host_base_flags` (build.mcpp host) | **trusts** the cfg for Clang |
| 4 | `mcpp src/toolchain/post_install.cppm` `fixup_clang_cfg` | **rewrites** the cfg (deletes `--sysroot`, comment promises "mcpp provides sysroot via payload paths" — copy #1 never delivered it on the link side) |
| 5 | `xim-pkgindex pkgs/l/llvm.lua` `__install_linux_cfg` | generates the cfg **from the install-time environment** (subos present → `--sysroot=<subos>`; absent → warn *"clang will use system sysroot"* — host fallback by design) |

Consequences found while tracing #195:

- **D1 — cfg trust is undefined.** flags bypasses it, `probe_sysroot`
  step 2 (`fallback/probe_sysroot.cppm parse_clang_cfg_sysroot`) *mines* it
  for `--sysroot`, `build_program` trusts it, `fixup_clang_cfg` rewrites it
  and deletes the very line probe mines (so probe's cfg path is dead on any
  fixed-up install). Four attitudes toward one file.
- **D2 — cfg is a non-reproducible artifact.** Same xpkg version produces
  different cfgs depending on whether a subos existed at install time, and
  on *which mcpp code path installed it* (see D3). The #195 reporter's cfg
  carried `--sysroot=<subos>`; a fixed-up install carries no `--sysroot` and
  a payload loader line.
- **D3 — post-install fixups are not part of the pipeline.** Explicit
  `mcpp toolchain install` runs gcc + llvm fixups
  (`src/toolchain/lifecycle.cppm`); the default-toolchain auto-install runs
  only the gcc fixup (`src/build/prepare.cppm`); the **manifest
  `[toolchain]` auto-install path runs no fixup at all** — the exact path a
  `mcpp.toml` llvm user takes. The gcc side already had this bug once
  ("stdlib.h not found", fixed by sharing the fixup with one auto-install
  path); llvm re-created it on another path. Structural, not incidental.
- **D4 — hermeticity is a promise, not an assertion.** Nothing at build,
  test or packaging time verifies that resolved CRT/libc/loader paths lie
  inside the sandbox. Host fallback makes CI green a false signal.
- **D5 — `ld-linux-x86-64.so.2` is hardcoded ~10× in mcpp**
  (`flags.cppm`, `pack.cppm` ×3, `lifecycle.cppm` ×3, `post_install.cppm`
  ×5) **and again in xim-pkgindex** (`llvm.lua`, `gcc.lua`,
  `gcc-specs-config.lua`, …), while `glibc.lua` already declares
  `exports.runtime = { loader, abi }` *precisely so consumers don't
  hardcode this* — and no consumer reads it. aarch64-glibc is silently
  broken across the whole chain.
- **D6 — test blind spot.** llvm e2e (36–41, 47) pin `llvm@20.1.7` (zero
  coverage of 22.x) and run on hosts whose system CRT masks the entire
  category via D4.
- **D7 — packaging has no admission gate.** Precedent: a slim-repacked
  llvm asset shipped without a runtime library it needed, caught only by
  users. Same class as #195: install-time artifacts are never smoke-linked.

## 3. Design

### 3.1 `ToolchainLinkModel` — one resolver, all consumers (mcpp)

New module `src/toolchain/linkmodel.cppm`:

```cpp
export namespace mcpp::toolchain {

enum class CLibMode { PayloadFirst, Sysroot, HostSDK, SelfContained /*musl*/ };

struct ToolchainLinkModel {
    CLibMode mode;
    std::filesystem::path crtDir;      // -B: where Scrt1.o/crti.o/crtn.o live
    std::vector<std::filesystem::path> libDirs;   // -L + -Wl,-rpath
    std::filesystem::path loader;      // -Wl,--dynamic-linker (empty ⇒ omit)
    std::vector<std::filesystem::path> systemIncludes; // compile-side -isystem
    // canonical flag renderers so every consumer emits IDENTICAL strings:
    std::string compile_flags(bool clang) const;   // -isystem… (+ --sysroot when mode==Sysroot)
    std::string link_flags() const;                // -B… -L… -rpath… --dynamic-linker=…
};

ToolchainLinkModel resolve_link_model(const Toolchain& tc);

} // namespace
```

Resolution (Linux, glibc world; musl/macOS/windows return their existing
behavior unchanged under `SelfContained`/`HostSDK`):

1. `payloadPaths` present → `PayloadFirst`: `crtDir = libDirs[0] =
   glibcLib`, `systemIncludes = {glibcInclude, linuxInclude}`,
   `loader` per §3.2.
2. else usable `tc.sysroot` → `Sysroot`: `--sysroot` on both sides (GCC
   include-fixed requirement preserved).
3. else → empty model (current behavior; the hermeticity check in §3.5
   turns silent host fallback into a diagnosed warning).

Consumers — all four divergent copies converge on the model:

- `flags.cppm`: `compile_toolchain_flags`/`link_toolchain_flags`/`payload_ld`
  are computed **from the model**. This is where the `-B` lands (fixing
  #195), as a model field, not a patch.
- `stdmod.cppm`: `sysroot_flag` assembly replaced by
  `model.compile_flags()`. (Identical strings also keep the
  `std-module.json` `std_build_commands` cache key honest.)
- `build_program.cppm host_base_flags`: Clang branch stops trusting the cfg;
  uses the model like everything else. (GCC branch already matches the
  model's Sysroot/`-B` shape.)
- `post_install.cppm fixup_clang_cfg`: regenerates the cfg **from the
  model** (see §3.3) instead of line-patching xlings' output.

### 3.2 Loader/ABI from data, not hardcodes (mcpp + pkgindex)

`resolve_loader(glibcLib, targetTriple)` in `linkmodel.cppm`, in priority
order:

1. **Declared metadata** — if the payload carries persisted exports
   (`<payload>/.xpkg-exports.json`, written by xlings at install time once
   the optional xlings change in §4.4 lands), use `runtime.loader`.
2. **Triple map** — `x86_64 → ld-linux-x86-64.so.2`,
   `aarch64 → ld-linux-aarch64.so.1`, `riscv64 → ld-linux-riscv64-lp64d.so.1`,
   musl triples → `ld-musl-<arch>.so.1`.
3. **Glob fallback** — first `ld-*.so*` in `glibcLib`.

All ~10 mcpp hardcode sites (`flags.cppm:367`, `pack.cppm`,
`lifecycle.cppm`, `post_install.cppm`) switch to this resolver. This
resolves the aarch64-glibc loader gap as a by-product — do **not** fix
`flags.cppm:367` as an isolated issue; it is one instance of D5.

### 3.3 cfg role: humans only; deterministic content (mcpp + pkgindex)

- **mcpp never reads the cfg again.** Build flags: always
  `--no-default-config` + full model (unchanged direction, now complete).
  `parse_clang_cfg_sysroot` is removed from the probe chain (it is dead on
  fixed-up installs anyway) and demoted to a verbose diagnostic log.
- **The cfg exists so a human running `clang++` directly gets a working,
  hermetic compiler.** It is generated deterministically from the model:
  `-B<glibcLib>`, `-L<glibcLib>`, `--dynamic-linker`, rpath, libc++
  `-isystem`s — no `--sysroot=<subos>`, no dependence on install-time
  environment.
- **`llvm.lua __install_linux_cfg` (xim-pkgindex)** is rewritten to the same
  deterministic recipe: resolve the sibling `xim:glibc` payload (guaranteed
  by the package's own `deps`), read `exports.runtime.loader` from
  `glibc.lua` (finally consuming the mechanism built for this), and
  **fail the install** if the payload is missing — delete the
  `warn + "clang will use system sysroot"` host-fallback branch.
  Also: name the versioned cfg from the actual major
  (`clang-<major>.cfg`), not the hardcoded `clang-20.cfg`.
- Compat: an old mcpp (≤0.0.82) against the new cfg behaves no worse than
  against today's fixed-up cfg (both lack `--sysroot`); the #195 bug in old
  binaries is fixed only by upgrading mcpp, which is expected.

### 3.4 Fixups become a pipeline stage (mcpp)

New single entry `toolchain::ensure_post_install_fixup(cfg, payload, pkg)`:

- Dispatches by package kind (gcc → patchelf + specs; llvm → patchelf(lib)
  + cfg regeneration from the model; musl → none).
- **Idempotent via a content-fingerprinted marker**
  (`<payload>/.mcpp-fixup.json`: schema version + fixup-input hash — glibc
  lib dir, loader path, mcpp fixup code version). A marker whose inputs
  drifted re-runs the fixup (lesson from the `.mcpp_ok` blind-spot class:
  markers must witness content, not just "a process once exited 0").
- Called from **one** place: the payload-resolution seam all three current
  call sites share (after `resolve_xpkg_path` for toolchain packages), so
  explicit install, default auto-install and manifest `[toolchain]`
  auto-install can never diverge again. The three existing scattered calls
  are deleted.
- Ownership guard (inherited/symlinked payloads are not patched) moves into
  the entry point and now covers llvm too (today only gcc has it).

### 3.5 Hermeticity: from promise to assertion (mcpp)

- **Build-time check** (Linux glibc/musl toolchains): once per toolchain
  fingerprint (cached next to the BMI cache, not per link), run the driver
  with `-### … -o /dev/null` on a trivial input, extract CRT + loader
  paths, and assert every one lies under an allowed prefix (the toolchain
  payload root, the glibc payload root, the sandbox registry). Violation ⇒
  hard error naming the leaked host path, with escape hatch
  `[build] allow_host_libs = true` (and `MCPP_ALLOW_HOST_LIBS=1`) for users
  who genuinely want host linking.
- This converts D4's silent contamination into a first-class diagnostic and
  is the regression fence for the whole category, not just #195.

### 3.6 Tests & CI (mcpp)

1. **Unit**: `linkmodel` resolution against fabricated payload trees
   (payload-first / sysroot / none; x86_64 + aarch64 loader; glob fallback).
2. **e2e `86_llvm_hermetic_link.sh`**: build an `import std` project with
   the llvm toolchain, re-run the link command with `-###`, assert
   `Scrt1.o/crti.o/crtn.o` + `--dynamic-linker` all resolve inside the
   sandbox (`$MCPP_HOME`/registry) — catches both "link fails" and "links
   against the host" regressions on any machine.
3. **Parametrize llvm e2e**: 36–41/47 read `MCPP_E2E_LLVM_VERSION`
   (default: newest installed payload) instead of the pinned `20.1.7`.
4. **Hermetic CI job**: a container job on a minimal image with **no host
   toolchain** (e.g. `debian:stable-slim` without gcc/libc6-dev) that
   installs the llvm toolchain and runs the llvm e2e subset + test 86. This
   is the only environment class that reproduces #195 faithfully; standard
   runners cannot.

### 3.7 Packaging admission gate (xlings-res)

The llvm slim-repack workflow gains a release gate: in the same minimal
container, install the candidate asset + `glibc` + `linux-headers`, then
compile, **link and run** an `import std` hello and run the `-###` prefix
assertion. Assets that cannot produce a self-contained binary are not
published (would have caught both the missing-runtime-library precedent and
the #195 environment class at the source).

## 4. Work breakdown — PRs, order, versions

All mcpp-side work ships in **one PR** (commit-staged for reviewability);
the other repos land small coordinated PRs. Nothing here changes package
asset contents, so no xpkg version bumps are needed — only mcpp releases.

### 4.1 PR-1 — mcpp `feat(toolchain): hermetic link model` → **v0.0.83**

Commit plan inside the single PR:

1. `feat(toolchain): linkmodel module + loader resolver` (new code + unit
   tests; no behavior change yet).
2. `fix(build): link CRT discovery via linkmodel (-B payload glibc)` —
   flags/stdmod/build_program/post_install converge on the model; deletes
   the four copies. **This commit alone closes #195.**
3. `refactor(toolchain): single post-install fixup pipeline` (marker,
   one call site, ownership guard for llvm).
4. `refactor(probe): drop clang-cfg sysroot mining` (diagnostic only).
5. `feat(build): hermeticity assertion + allow_host_libs escape hatch`.
6. `refactor: loader hardcodes → linkmodel (pack/lifecycle/post_install)`.
7. `test(e2e): 86_llvm_hermetic_link + llvm version parametrization`.
8. `ci: hermetic no-host-toolchain container job`.
9. `chore: 0.0.83 + changelog`.

Merge gate: full e2e matrix (linux/macos/windows self-host) green **plus**
the new hermetic container job green.

### 4.2 PR-2 — xim-pkgindex `fix(llvm): deterministic hermetic cfg`

- `pkgs/l/llvm.lua`: rewrite `__install_linux_cfg` per §3.3 (payload-based,
  reads `glibc.lua exports.runtime`, fail-fast, versioned cfg name).
- `pkgs/g/glibc.lua`: `exports.runtime` stays the single source of truth;
  add per-arch loader entries when aarch64 assets land (tracked, not
  blocking).
- Independent of PR-1 (mcpp bypasses the cfg either way); land after PR-1
  so the hermetic CI job exists to validate it end-to-end via a fresh
  install in mcpp's smoke path.

### 4.3 PR-3 — xlings-res `ci(llvm): hermetic admission smoke` (§3.7)

Workflow-only change in the llvm repack repo; no asset changes.

### 4.4 PR-4 (optional, non-blocking) — xlings `feat(xim): persist xpkg exports`

Write declared `exports` (e.g. `runtime.loader`/`abi`) to
`<install_dir>/.xpkg-exports.json` at install time so consumers read
declared metadata instead of conventions. mcpp's resolver treats this as
priority 1 (§3.2) but works without it (triple map + glob), so this PR can
trail without blocking the release train. Ships in the next regular xlings
patch release if taken.

### 4.5 Release train (0.0.83)

Follows the automated ecosystem-publish pipeline established for 0.0.82:

1. Merge PR-1 → tag `v0.0.83` → release workflow builds + publishes
   assets on GitHub.
2. Pipeline mirrors the release into `xlings-res/mcpp` (**both** GitHub and
   GitCode remotes — CN mirror must not lag, or CN-side installs fetch a
   stale binary).
3. Pipeline opens the **xim-pkgindex bump PR** (mcpp 0.0.82 → 0.0.83);
   PR-2 (llvm.lua) merges alongside or immediately after it (same repo,
   separate PRs — keep the bump PR mechanical).
4. Post-merge verification on a clean environment: `xlings install mcpp`
   resolves 0.0.83; then the #195 reproduction —
   `mcpp new hello` + `[toolchain] linux = "llvm@22.1.8"` + `mcpp run` —
   succeeds in the no-host-toolchain container.
5. Bump the workspace bootstrap pin to 0.0.83
   (`ci: workspace mcpp bootstrap pin -> 0.0.83`) only after step 4 passes.

## 5. Compatibility & migration

- **Existing installs**: first build with 0.0.83 hits the fixup pipeline's
  marker check (no marker → fixup runs), regenerating the cfg from the
  model and patching anything the old scattered paths missed. No user
  action; no reinstall.
- **BMI caches**: the flag strings change (new `-B`, canonical model
  rendering) ⇒ `std-module.json` metadata mismatch ⇒ std module rebuilds
  once per fingerprint. Expected, self-healing.
- **Hermeticity check rollout risk**: environments that intentionally link
  host libs (system OpenGL passthrough etc. use dep `[runtime]`
  library_dirs, which stay allowed — the assertion covers only CRT/loader
  resolution). If an unforeseen legitimate layout trips it, the escape
  hatch keeps users unblocked while the allowlist is extended.
- **Windows/macOS**: untouched code paths (`HostSDK`/PE deploy model);
  e2e must show zero diffs there.

## 6. Acceptance criteria

1. #195 reproduction passes in the hermetic container (fresh env, no host
   toolchain, llvm 22.1.8 via manifest `[toolchain]`, `mcpp run` prints).
2. `-###` assertion: CRT + loader resolve inside the sandbox on **every**
   Linux llvm e2e, on hosts both with and without a system toolchain.
3. `grep -rn "ld-linux-x86-64" src/` returns only `linkmodel.cppm`'s triple
   map (mcpp) / only `glibc.lua exports` (pkgindex).
4. One call site for post-install fixups; all three install paths produce
   byte-identical cfg for the same payload.
5. Full existing e2e baseline unchanged on macOS/Windows/musl.
6. Release train completed through the bootstrap-pin bump with the CN
   mirror verified.

## 7. Explicitly out of scope (tracked separately)

- aarch64 glibc-world enablement beyond the loader resolver (payload assets
  for aarch64 glibc/llvm don't exist yet; the resolver removes the code
  blocker).
- Identity-first package resolution (`probe_payload_paths` picking the
  first glibc version dir without identity checks) — same family as the
  existing identity-first refactor plan; the linkmodel takes the payload it
  is handed and does not widen the guessing.
- gcc-specs / musl worlds beyond keeping their current behavior green.
