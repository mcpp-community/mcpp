---
subject: design
status: landed
---

# Issues #693 to #696: triage against mcpp's contracts, and one repair plan

- Issues: mcpp-community/mcpp#693 (Windows: a working directory outside the ANSI code
  page ends `mcpp --version` with 0xC0000409), #694 (musl targets compile every
  optimizing profile at `-Og`), #695 (`c_standard` on a dependency is accepted and
  ignored), #696 (links over a graph-supplied C library still search the host's
  `/usr/lib`). All four were filed on 2026-09-25. They are issues, not pull requests.
- Basis: `origin/main` 82867ad7 (mcpp 2026.9.25.1). Every citation of mcpp code is to that
  commit. Other repositories: `openxlings/xlings` 84572b0, `mcpplibs/mcpp-index` 93781cf,
  `mcpplibs/openkal-musl` f1f789f (0.19.1).
- Measurements:
  - Linux x86_64, Ubuntu 24.04.1, glibc 2.39. The released mcpp 2026.9.25.1 was invoked by its
    xlings store path. Toolchains: llvm@22.1.8, musl-gcc 15.1.0 and 16.1.0 (x86_64 native,
    aarch64 cross), qemu-aarch64.
  - Windows, through the temporary draft PR #697 on `windows-latest` (Windows Server 2025,
    ACP 1252), with the released mcpp, the released xlings, and three toolchain rows (§6.2).
  - The reporters measured on CachyOS (glibc 2.44) with 2026.9.21.3, and on Windows (ACP 936)
    with 2026.9.25.1.
- Status: proposed. Nothing is implemented.
  - Revision 1 (2026-09-25): the triage.
  - Revision 2 (2026-09-26): records D1 to D4 as accepted; adds the Windows measurements for
    #693, the UTF-8 model (§6.4) and the answer to D5; ends with a self-review (§11).
  - Section 9 lists what remains for review.

---

## 0. Summary

| | Report | Verdict | Where the repair lives | The silent part |
|---|---|---|---|---|
| **#694** | musl targets compile every optimizing profile at `-Og` | **Engine defect.** A May 2026 workaround for a musl-gcc 15.1.0 ICE is keyed on the target triple, not on the compiler. Its trigger no longer reproduces on any input available today (§3.2). It also fixes the optimization level of both published Linux binaries of mcpp at `-Og`. | engine | the build prints `Finished release [optimized]` |
| **#695** | `c_standard` on a dependency is ignored; the root's value reaches every dependency | **Engine defect.** It violates P1 and P4 of the #690 record, contradicts docs/04 and docs/07, and disagrees with mcpp's own cache key and fingerprint. The #690 record classified the key incorrectly. | engine, after an ecosystem measurement | the key is parsed, hashed and never applied |
| **#696** | a link over a graph-supplied C library searches the host's library directories | **Engine defect (hermeticity), plus an ecosystem data gap.** The graph branch replaced the payload's `--sysroot` with nothing, and clang without a sysroot searches `/`. openkal-musl does not install the empty archives that musl's own `make install` provides. | openkal-musl (data) first, then engine | a glibc object inside a musl static image, and the hermetic check passes |
| **#693** | `mcpp --version` exits 0xC0000409, with no output, in a non-ACP directory | **The silent exit is an xlings defect**, measured: the xlings shim in front of mcpp throws during start-up, and `mcpp.exe` itself prints its version. **mcpp has a larger defect the report did not reach**, also measured: every build fails with an internal exception when the project or the mcpp home (the user profile) lies under a non-ASCII name. That includes names the ACP can represent, which on a Chinese system means any Chinese directory or user name. mcpp has no declared text encoding. | xlings (the silent exit); engine (a UTF-8 model, §6.4) | the xlings shim exits with no output |

### 0.1 Classification of every finding

| Issue | Finding | Class |
|---|---|---|
| #694 | `-Og` on musl targets for every compiler and every optimizing profile | engine defect |
| #694 | `Finished … [optimized]` derived from the declared level, not the level used | engine defect (reporting) |
| #694 | the reporter's `forced-o2` profile | usage-side workaround; valid, and it reaches only the root package |
| #694 | `mcpp.toml:52-56` says the aarch64 cross toolchain is gcc 15.1.0 | stale documentation |
| #695 | `c_standard` dropped on every non-root package; the root's value applied to all | engine defect |
| #695 | `cache_key.cppm:582` says the value "reaches its own C units" | engine defect (a false comment next to correct keying) |
| #695 | the #690 record lists `c_standard` as root-only and "consistent" | design-record error |
| #695 | compat.libaio, compat.libdrm, compat.libinput define `_GNU_SOURCE` and blame the value | usage-side workaround; valid, but the comments misattribute the cause |
| #695 | 13 descriptors and openkal-musl declare a standard that was never applied | ecosystem data; measured valid with clang on Linux (§4.6) |
| #696 | host library directories on graph links (ELF; PE through the MinGW driver) | engine defect |
| #696 | the hermetic check does not inspect `-L` and passes | engine defect (check coverage) |
| #696 | openkal-musl lacks the empty archives musl installs for `m`, `rt`, `pthread`, `crypt`, `util`, `xnet`, `resolv`, `dl` | ecosystem data gap |
| #696 | descriptors and users that link `-lm` on Linux | usage; correct |
| #693 | a silent exit with 0xC0000409 | **xlings defect** (F-693a, measured): an unhandled `std::system_error` in start-up, with no exception boundary |
| #693 | every build in a non-ASCII directory that the ACP can represent fails with an internal JSON exception, and so does every build under a non-ASCII user profile | **engine defect** (F-693b, F-693f, measured); not in the report, and wider than it |
| #693 | every build in a directory outside the ACP fails with an internal narrowing exception | engine defect (F-693c, measured) |
| #693 | the build-program contract has no encoding | engine defect (F-693d, measured) |
| #693 | on Linux, a directory name that is not UTF-8 fails the same way | engine defect (F-693e, measured) |
| #693 | process creation and environment through the -A APIs | engine design debt (W4e) |
| #693 | MinGW `as`, `ld`, `collect2` and `ar` without a UTF-8 manifest | external limitation; harmless while they receive relative paths (measured), to be named if that changes |
| #693 | a directory name outside the ACP | usage; correct |

### 0.2 Three statements

1. **No reported usage is wrong.** Every manifest, descriptor and command in the four reports
   uses a documented feature correctly. The usage side has only data that the engine defects
   hid.
   - Thirteen descriptors and openkal-musl declare a `c_standard` that has never been applied to
     them as dependencies. Measured with clang on Linux, all fourteen build at their declared
     value (§4.6).
   - Three descriptors define `_GNU_SOURCE` instead of declaring `gnu11`, and their comments
     say the value has no effect. The cause is the dependency position, not the value.
2. **Every defect has a silent part, and the silent part does more damage.** Each engine path
   overrides, drops or bypasses a declared value without a diagnostic. In #694 the build's own
   label contradicts the flags it used. In #696 the check that exists for this class of error
   reports success. In #693 the silent part is xlings's: mcpp's own failures print an error,
   although the error is an internal exception rather than a diagnostic (§7).
3. **Two structural rules cover three of the four.**
   - The file-level flag variables carry only graph-wide values. #695 is a case where they did
     not, and #690 F7 was the case before it.
   - What the build reports is what it used. #694's label, #695's cache key and fingerprint, and
     #696's hermetic check each describe something the build did not do (§7).

---

## 1. Method

- Each finding is marked *measured* or *reasoned*. A measured finding has a command and its
  output in Appendix A. A reasoned finding cites a code path. Measurements made by a reporter
  are attributed to the reporter. Where this review repeated one, it says so.
- Statements about the implementation were read from `origin/main` with `git show`. The local
  checkout was five commits behind and was not used.
- Probe builds of mcpp itself ran in a detached worktree of 82867ad7 under the session scratchpad.
  One temporary profile was appended to `mcpp.toml` (§3.2) and removed afterwards. Nothing was
  committed.
- The criterion for "a flag reached a unit" is that unit's entry in `compile_commands.json`.
  Build success is never used as the criterion.
- Windows facts come from a temporary draft PR (#697). Its branch removes every other
  workflow and runs one PowerShell script. Every observation is a `READING` line in the log;
  nothing is uploaded, and a step fails only when the probe itself cannot run. Each
  non-ASCII name is built from code points, so the script's source is ASCII. The PR is closed
  once its readings are recorded here (Appendix A.10).

---

## 2. Principles

The #690 record's P1 to P8 apply unchanged. Two of them decide these issues. P1 is position
independence: a package's compile inputs do not depend on which consumer reached it. P4 is scope:
a package's private build requirements reach only its own units, and nothing flows from a
consumer into a dependency. Five further rules are used below.

| | Rule | Source in mcpp | Elsewhere |
|---|---|---|---|
| **Q1** | A declared value is honoured, or refused with a diagnostic. It is never replaced or dropped silently. | `mcpp.diag`'s batch invariant: "a branch doing LESS because a precondition was not met owes the user an `impact` sentence" (`src/cli.cppm:115-117`). | Cargo warns on manifest keys it does not use. |
| **Q2** | Each question has one answerer. What the build reports is read from what the build used. | `src/build/execute.cppm:1034-1036` states this for the profile descriptor. | |
| **Q3** | The engine carries no toolchain-defect workarounds (decision D5, §9). A defect in a toolchain version is answered by the toolchain pin, which is data. A project that has to keep a defective version scopes its own mitigation with `[build] cxxflags` or `[build] flags = [{ glob, cxxflags }]`, which is usage. The engine's guarantee is that the level it realises is the level declared, and a property test enforces it. | The v0.0.1 comment named the compiler and a retirement condition that nothing executed; the code keyed the workaround on the target (§3.4). | A version pin is how every package manager excludes a defective release. |
| **Q4** | Host state reaches a build only through a named, minimal surface. When the graph supplies the C library, no host library directory, startup file or loader reaches the link. | The hermetic link model (`.agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md`). | Bazel and Nix sandboxes; `--sysroot` as the standard way to scope a cross driver. |
| **Q5** | mcpp's text is UTF-8 on every platform. Everything it hands to another program is in the encoding that program reads. Text that enters from outside is validated where it enters (§6.4). | #516 and #518; the measurements of §6.2. | Ninja, CMake, LLVM and Cargo are UTF-8 inside and convert at the boundary (§6.4). |

The routing rule for upstream reports applies to every item. The order of preference is usage,
project plugin, official plugin, ecosystem data, engine. An engine item is valid when the defect
is general. The rule filters features, not defects.

---

## 3. #694: the musl `-Og` workaround

### 3.1 The report

On a `*-linux-musl` target, every profile with a non-zero `opt` compiles at `-Og`, with GCC and
with Clang. docs/04 §2.9 says `release` is `-O2` and `dist` is `-O3`. The build still prints
`[optimized]`. The condition dates from v0.0.1 (92f1335e), whose comment gave the reason:
musl-gcc 15.1.0 hit an ICE in `tree-ssa-ccp` on libstdc++'s `std::format` (`__write_padded`) at
`-O2`. The comment also said `TODO(musl-gcc-upstream): remove once musl-gcc@16+ ships`. The
reporter reasoned, without checking the artefacts, that both published Linux binaries of mcpp
are compiled at `-Og`. The proposed fix keeps the workaround for libstdc++ only.

### 3.2 Verification

- **Reasoned.** `src/build/flags.cppm:984-988`:
  `opt_flag = isMuslTc && prof.optLevel != "0" ? " -Og" : …`. `isMuslTc` is
  `is_musl_target(plan.toolchain)` (L858), which reads only the target triple. The flag is part of
  the file-level `$cflags`/`$cxxflags` (L1072-1082), so it reaches every unit of every package.
- **Measured: the report's fixture on 2026.9.25.1.** `optlevel` with clang 22.1.8 and
  `--release --target x86_64-linux-musl`: 1621 entries in `compile_commands.json` carry `-Og`,
  and the 3 assembly entries carry no `-O`. The output ends with `Finished release [optimized]`.
  This repeats the reporter's result on the current release.
- **Measured: the release configuration of mcpp itself.** In the 82867ad7 worktree,
  `mcpp build --release --configure-only --target x86_64-linux-musl` resolves `gcc@16.1.0`
  (the manifest's pin), and so does the same command for `aarch64-linux-musl` (the target table's
  pin, `modules/toolchain-model/src/triple.cppm:450-451`). Every entry carries `-Og`, including
  all 178 units of the binary: 127 in `src/`, 48 in `modules/` and 3 in `mcpplibs.cmdline`.
  `release.yml` builds the published binaries with exactly these commands (L132, L348, L351),
  and `mcpp.toml:14` sets `default-profile = "release"`. **Both published Linux binaries have
  been compiled at `-Og` since v0.0.1.** This was established from the build configuration, not
  from the artefacts, which are stripped and record no switches.
- **Measured: the premise no longer holds.** The ICE was not reproduced on any input available
  on 2026-09-25:

  | Input | Compiler | Level | Result |
  |---|---|---|---|
  | The report's `std::format` line, header mode and `import std` module mode | musl-gcc 15.1.0 and 16.1.0, x86_64 and aarch64 | `-O2` (header mode also `-O3`) | compiles |
  | mcpp at 82867ad7, root units raised to `-O2` by a probe profile | x86_64-linux-musl-gcc 16.1.0 | 127 root units at `-Og -O2` | builds; `--version` runs |
  | the same | aarch64-linux-musl-gcc 16.1.0 (cross) | 127 root units at `-Og -O2` | builds; `--version` and `--help` run under qemu-aarch64 |
  | mcpp at v0.0.1 (92f1335e), the code that hit the ICE | x86_64-linux-musl-gcc 15.1.0 | 23 root units at `-Og -O2` | every unit compiles, with no ICE; the link fails on an unrelated GCC 15 module defect (`undefined reference to std::optional<std::filesystem::path>::optional(optional&&)`) |
  | mcpp at 82867ad7 | x86_64-linux-musl-gcc 15.1.0 | `-Og`, all 178 units | fails with `exposes TU-local entity`, a front-end check that does not depend on the optimization level |

  The last row means that current mcpp cannot be built with musl-gcc 15.1.0 at any level. For
  mcpp's own build, the workaround protects nothing. The probe has one limitation. Profile flags
  reach only the root package, so the 51 units outside `src/` stayed at `-Og`. The removal PR
  compiles all 178 units at `-O2` in CI, and that run is the final criterion.

### 3.3 Classification

This is an engine defect. The profiles, the docs and the reporter's manifests are all correct. The
reporter's `forced-o2` profile is a usage-side workaround, and it reaches only the root package's
units.

### 3.4 Root cause

1. **A compiler defect was answered in the engine, and keyed on the wrong axis (Q3).** An ICE
   is a property of one compiler version, so its place is the version pin, which is data. The
   code instead reads the target, so when clang gained musl targets through openkal, it
   inherited a GCC workaround.
2. **The override is silent, and the label reads the value that was overridden (Q1, Q2).**
   `execute.cppm:1039-1040` derives `[optimized]` from `bc.optLevel`, while the compile uses
   `opt_flag`. The comment above it says the descriptor "cannot disagree with the compiler flags",
   and on musl it does.
3. **The retirement condition had nothing to execute it.** Two later changes touched the line:
   53f85a68 (#24) shortened the comment and dropped the reason, and 40215eb2 (#109) added the
   `opt = 0` exception. Neither re-tested the premise, and the TODO's condition (musl-gcc 16 is
   pinned) has been true for some time.

### 3.5 Repair (W1)

- **W1a.** Delete the musl branch of `opt_flag`. The optimization flag becomes a function of the
  profile and the dialect only.
- **W1b.** Make the profile descriptor and the flag read one value (Q2), for example a single
  function over the plan that both `compute_flags` and `execute.cppm` call. Once W1a lands they
  agree anyway. The change is small, and it keeps them in agreement the next time something
  adjusts the level.
- **W1c.** Correct `mcpp.toml:52-56`, which says the aarch64 cross toolchain is gcc 15.1.0 from
  musl-cross-make. The target table pins `gcc@16.1.0`, and the probe resolved
  `aarch64-linux-musl-gcc@16.1.0`.
- **Not recommended: the report's narrower patch**, which keeps `-Og` when the standard library
  is libstdc++. It keeps a workaround whose trigger does not reproduce, on the one path where the
  cost falls on mcpp's own release binaries.
- **No workaround layer (D5).** The engine needs no mechanism for toolchain workarounds, and
  none is added. An inventory of `origin/main` for engine behaviour that responds to a compiler
  defect found exactly one case that changes a declared value, the `-Og` above. Every other
  match is one of two things. Some are a choice of mechanism that is the conventional contract,
  such as publishing clang's reduced BMI rather than its full BMI (`model.cppm:477-501`,
  measured). The rest arrange mcpp's own sources around a clang 22.1.8 miscompile, which
  concerns mcpp's code and not the builds of its users. The general answer has three parts:
  1. **The toolchain is data, so a defective version is answered in data.** The target table
     and `[toolchain]` pin a version without the defect, as `gcc@16.1.0` already does.
  2. **A project that must keep a defective version** mitigates it in its own manifest, scoped
     to the unit that triggers it: `[build] flags = [{ glob = "src/x.cpp", cxxflags = [...] }]`
     exists for this (`GlobFlags`, `modules/manifest/src/types.cppm:280`). The mitigation is
     then visible, reviewable and limited to the one translation unit.
  3. **W1d, a property test instead of a rule.** For every row of the target table, both
     compiler families and every profile level, the optimization token in `compute_flags` equals
     the dialect's spelling of the declared level. A future change that overrides a declared
     level fails this test, instead of depending on a reviewer remembering a rule.

### 3.6 Criteria

- W1d, a property test in `tests/unit/test_ninja_backend.cpp` next to the other
  `compute_flags` cases. For every row of `kKnownTargets`, with GCC and with Clang, and for
  `opt` in `0`, `1`, `2`, `3`, `s`, the realised optimization token is the dialect's spelling of
  the declared level: `-O2` and no `-Og` for `opt = 2` on `x86_64-linux-musl`, `-Os` for
  `opt = "s"`, and so on for every row.
- The report's fixture: `--release --target x86_64-linux-musl` produces zero `-Og` entries.
- The release workflow's two musl builds and the e2e suite pass, and the release job's
  `compile_commands.json` has zero `-Og` entries.

### 3.7 Risk

The published Linux binaries change optimization level for the first time since v0.0.1. The
exposure is low. mcpp's code is already compiled at `-O2` on every other release platform (macOS
and Windows use clang). Linux CI builds mcpp with a plain `mcpp build` (`ci-linux.yml:143`,
`ci-linux-e2e.yml:71`), which is gcc 16.1.0 on x86_64-linux-gnu under the release profile, so the
e2e suite already runs against an `-O2` build from the same compiler family. The one musl leg in
CI (`ci-linux.yml:238`) is the one that runs at `-Og`. The release notes state the change.

---

## 4. #695: `c_standard` on a dependency

### 4.1 The report

`[build] c_standard` takes effect only on the package being built. On a dependency it is parsed
and accepted but never used. This covers path dependencies, workspace members reached from
another member, and index packages, whether the key is in the package's `mcpp.toml` or in a
descriptor. The root's value (default `c11`) reaches every C unit of every dependency. The cache
key and the fingerprint record the value per package.

### 4.2 Verification

- **Measured: path dependency, 2026.9.25.1, clang 22.1.8, host target** (the report's `cdep`
  and `app` fixture). `cdep.c` fails its `__STRICT_ANSI__` guard, and the file-level line in
  `build.ninja` is `cflags = -std=c11`. The reporter's other positions (workspace member,
  `compat.zlib` descriptor, openkal-musl's 1343 units) were not repeated here. They use the same
  code path, which is unchanged on 82867ad7.
- **Reasoned.** The standard comes from the root manifest (`flags.cppm:1015-1016`) and is placed
  in the file-level `$cflags` (L1078-1082). A unit's `$unit_cflags` carry its package's `cflags`
  and no standard (`src/modgraph/scanner.cppm:1466-1477`). Meanwhile
  `src/build/cache_key.cppm:581-584` adds `__c_standard=<the package's own value>` under the
  comment "A package may pin its own C standard; it reaches its own C units". That comment is
  false. `src/build/prepare_inputs.cppm:735` fingerprints the package's value. `cache_key.cppm:529`
  also keys on the root's value, so the cache is sound (P5), but it is keyed on an input the
  package does not control.

### 4.3 Classification

This is an engine defect, and three sources say so.

- **The contract.** docs/04 lists `c_standard` in the same `[build]` table as `cflags`, as
  "Standard for C source files (default c11)". §3.4 of the same document shows it on a pure C
  library. The merge table in docs/07 treats it as a member-level scalar.
- **The #690 principles.** Under P1, a dependency's C compile depends on its consumer. Under P4,
  a consumer's value flows into its dependency.
- **The engine's own records.** The cache key and the fingerprint treat the value as belonging to
  the package.

The #690 record (§3.1) lists `c_standard` among the keys read "from the root manifest only" and
calls that "consistent". **That classification is wrong for C.** The C++ standard is graph-wide
for a reason: a BMI must be read at the level at which it was produced. A C translation unit
produces no BMI, and a program that links C objects compiled under different standards is
ordinary. The record needs a correction note in §3.1.

On the usage side, nothing is wrong. `compat.libaio`, `compat.libdrm` and `compat.libinput` use
`-D_GNU_SOURCE` because `gnu11` did nothing. That is a correct response to the defect. Their
comments, however, place the fault in the value ("accepted and silently emits `-std=c11`"). The
value works on a root package; it is the dependency position that drops it. After W3 lands, the
comments should be corrected. Switching those descriptors to `gnu11` is optional.

### 4.4 Root cause

The C standard travels on the file-level `$cflags`, a graph-wide channel. The per-package value
exists in three places (manifest, fingerprint, cache key), but no code places it on the package's
units. This is the same family as #690 F7, the include directories broadcast through the same
channel, which #691 removed.

### 4.5 Repair (W3)

- **Semantics.** A package's effective C standard is its own declared value, after workspace
  inheritance, or the engine default `c11`. It never comes from the consumer. The root is treated
  like any other package, so a root's `c_standard` reaches only the root's own C units (decision
  D2).
- **Mechanism.** The file-level `$cflags` carries the engine default, `-std=c11`, as a graph-wide
  constant. A package whose effective standard differs gets its dialect's spelling appended to its
  C units' per-unit flags, which come after `$cflags` in the `c_object` rule, so the later
  `-std=` wins on GCC and Clang. This is the mechanism `implementationStandardFlag` already uses
  for C++ (`src/build/plan.cppm:1725-1770`). Only the packages that declare a non-default value
  see their command lines change.
- **W3b, the MSVC dialect: a separate step, measured first.** Today no C `/std:` is emitted for
  `cl.exe` at all (`flags.cppm:1078`), so the key is silently dropped for every package, the
  root included. Mapping a declared `c11` to `/std:c11`, or `c17` to `/std:c17`, is not a
  neutral change.
  - Microsoft documents that both options also switch on the conforming preprocessor
    (`/Zc:preprocessor`), and that without them `cl` compiles C89 with Microsoft extensions.
  - 90 descriptors declare `c11`, so all of them would change mode on that row at once.
  - W3b therefore does two things. It maps only declared values, never the engine default.
    It reports the values `cl` cannot express (`c99`, `gnu*`) once per package as `degraded`.
  - It ships after mcpp-index CI has run the MSVC row against it.
  - The llvm row on Windows is not affected, because it drives clang with GNU spellings and
    takes W3 as is.
- **Cache.** Remove the root's `cStandard` from the dependency key (`cache_key.cppm:529`). After the
  change it no longer reaches any dependency command. Keep the per-package `__c_standard` entry,
  whose comment then becomes true. This also makes docs/04 §2.10 true for C dependencies: "A
  dependency's artifacts do not depend on who consumes them".
- **Documents.** Update the docs/04 row: the standard applies to this package's C units, a
  dependency keeps its own, and the default is `c11`. The docs/07 table stays as written and
  becomes true. Append a correction note to the #690 record, §3.1.
- **Rejected: graph-wide with refusal on non-root packages.** It contradicts P1, P4 and the
  documentation. It would refuse 13 descriptors and openkal-musl that are written per package. It
  would also forbid something C permits without restriction.

### 4.6 Blast radius (P8)

Thirteen descriptors in mcpp-index declare a non-default standard: eleven declare `c99`,
`compat.ffmpeg` declares `c17` and `compat.freetype` declares `gnu11`. openkal-musl declares
`c99`. None of these has ever been compiled at its declared value as a dependency. The value is a
claim that has never been tested, and W3 is the first time it takes effect.

**Measured, with no engine change.** Under the current engine, a consumer that declares a
dependency's value applies that value to the dependency, so the per-package semantics can be
emulated one package at a time. The side effect is that the dependency's transitive C
dependencies receive the value too, so failures are attributed by object path. Each row below is a
control build (root default `c11`) and a treatment build (root = the declared value), with
2026.9.25.1 and clang 22.1.8:

| Package | Declared | C units at that standard in the treatment graph | Control | Treatment |
|---|---|---|---|---|
| compat.cjson 1.7.19 | `c99` | 1 | builds | builds |
| compat.ffmpeg 8.1.2 | `c17` | 2124 | builds | builds |
| compat.freetype 2.13.3 | `gnu11` | 60 | builds | builds |
| compat.glad 0.0.0-651a425 | `c99` | 1 | builds | builds |
| compat.hiredis 1.2.0 | `c99` | 7 | builds | builds |
| compat.libpng 1.6.43 | `c99` | 30 | builds | builds |
| compat.libuv 1.48.0 | `c99` | 35 | builds | builds |
| compat.lua 5.4.7 | `c99` | 32 | builds | builds |
| compat.md4c 0.5.3 | `c99` | 1 | builds | builds |
| compat.sdl2 2.32.10 | `c99` | 744 | builds | builds |
| compat.tray 0.0.0-8dd1358 | `c99` | 1 | builds | builds |
| compat.yyjson 0.12.0 | `c99` | 1 | builds | builds |
| compat.eui-neo 0.5.9.1 | `c99` | 641 | builds | builds |
| openkal-musl 0.19.1 (through openkal-llvm-runtime 0.15.1, `x86_64-linux-musl`) | `c99` | 1512 | builds | builds |

**All fourteen build at their declared standard.** Before accepting the result, the probe was
checked for the failure shape "the treatment was served from a cache compiled at another
standard", in three ways.

- No treatment build printed `Cached`.
- `.ninja_log` shows the treatment's object edges were executed as compiles, not restored. For
  compat.libpng, the control restored the objects from the cache (3 ms per edge, and the build
  printed `Cached compat.libpng v1.6.43 (15 units)`), while the treatment compiled them (29 to
  80 ms per edge).
- For the same object, the command hashes differ between control and treatment, and
  `SDL_audio.c` carries `-std=c99` in the treatment.

The measurement covers clang 22.1.8 on a Linux host only.

Two more sources of evidence apply before release: mcpp-index CI run against the W3 branch
through `MCPP_SOURCE_REF`, and openkal's own measurement set. A descriptor that fails at its
declared value is a data error. It is corrected in mcpp-index before the engine release, not
worked around in the engine.

### 4.7 Criteria

- The report's fixture builds, and `cdep.c`'s entry carries `-std=gnu11`.
- A root that declares `c99` leaves a dependency's C units at the dependency's own value, or at
  `c11` when the dependency declares none.
- A workspace member's declared value holds under `-p app` and under `-p lib`.
- A descriptor package (`compat.zlib`, `c11`) keeps `c11` while the root declares `c99`.
- On a musl target, openkal-musl's units carry `-std=c99`.
- Two consumers that differ only in `c_standard` share one cache entry for a C dependency: the
  second build prints `Cached`.

---

## 5. #696: host library directories on graph links

### 5.1 The report

When the C library comes from the dependency graph (openkal-musl), the link line that mcpp builds
still lets clang add the build machine's library directories. `-nostdlib` removes the startup
files and the default libraries, but not the search directories. A `-l` that the graph does not
answer is therefore looked up on the host. On aarch64-linux-musl, glibc's `libm.a` linker script
(`OUTPUT_FORMAT(elf64-x86-64)`) breaks the link. On x86_64-linux-musl, glibc objects are linked in
without any message. The hermetic check misses this too, because it inspects only startup objects
and the loader.

### 5.2 Verification

- **Measured: the driver on a second distribution** (Ubuntu 24.04, clang 22.1.8, the report's
  mcpp-free commands).

  | Target | `-L` added by the driver | `-lm` resolves to |
  |---|---|---|
  | aarch64-unknown-linux-musl | `/lib/../lib64`, `/usr/lib64`, `/lib`, `/usr/lib` | `unable to find library -lm` |
  | x86_64-unknown-linux-musl | eight directories, including `/usr/lib/gcc/x86_64-linux-gnu/13` and `/usr/lib/x86_64-linux-gnu` | `/lib/x86_64-linux-gnu/libm.a`, then `libm-2.39.a` and `libmvec.a` |

  The aarch64 symptom differs from CachyOS because Ubuntu keeps glibc's archives in the
  multiarch directory. The cause is the same. The outcome depends on the host's layout, which is
  the point of the report.
- **Measured: through mcpp 2026.9.25.1** (the report's `fmaximum` fixture, x86_64-linux-musl).
  The build succeeds and the program prints `2`. `--why-extract` shows:

  ```
  obj/main.o	/usr/lib/x86_64-linux-gnu/libm-2.39.a(s_fmaximum.o)	fmaximum
  ```

  A glibc object sits inside a musl static image, and nothing reports it.
- **Measured: the repair's two halves.** `--sysroot=<empty directory>` removes every `-L` on
  both targets, and `-lm` then fails with `unable to find library -lm`. Adding `-L` to a directory
  that holds one empty `libm.a` (8 bytes, `!<arch>\n`) makes `-lm` resolve to it, and both
  targets link.
- **Reasoned: the mechanism.** On a payload link, `lm.link_flags()` carries
  `--sysroot=<subos>` (`flags.cppm:814`). The graph branch replaces the host's link model
  (L785-810 and L1944-2042) and emits no sysroot in its place. A clang with no sysroot derives its
  search directories from `/`, and on x86_64 from the host's GCC installation. **The replacement
  removed the one token that had kept the host out.** `src/build/hermetic.cppm` dry-runs the
  driver (L149) and checks CRT objects (L188) and the loader, but not `-L`. With `-nostdlib` there
  is nothing for it to find, so it writes `.mcpp-hermetic-ok` (L139), including for the aarch64
  link that then fails.

### 5.3 Classification

- **Engine defect**, under Q4. It is the link-side twin of #664, which closed the compile side
  with `-nostdlibinc`. The hermetic check does not cover the class it exists for.
- **Ecosystem data gap.** musl's own `make install` places empty archives for `m`, `rt`,
  `pthread`, `crypt`, `util`, `xnet`, `resolv` and `dl` next to `libc.a`, because libc holds all of
  their contents. openkal-musl does not provide them.
- **Usage: correct.** Linking `-lm` on Linux is right for glibc and for musl alike. In mcpp-index,
  30 of 233 descriptors link at least one of the eight names in code, with Lua comments removed.
  The counts are `-lm` 13, `-lpthread` 21, `-ldl` 10, `-lrt` 6 and `-lresolv` 2.

### 5.4 Repair (W2, three parts in a fixed order)

- **W2a (openkal-musl, data).** Ship the eight empty archives in a package directory, and add a
  package-relative `-L` for it to `[target.'cfg(os = "linux")'.build] ldflags`. mcpp already
  resolves a dependency's relative `-L` against the dependency's root and forwards it to the
  consumer's link line (`src/build/prepare.cppm:7064-7097`). No engine change is needed, which is
  why the routing rule puts this half in data. Release chain: openkal-musl, then
  openkal-llvm-runtime (pins are exact), then the index.
- **W2b (engine).** The graph branch passes `--sysroot=<an engine-owned empty directory>` to
  clang, first on ELF targets. On PE targets that use the MinGW driver it follows one inventory
  (measured in the table below: the same defect, the same repair). The inventory is the `-l` names
  that the index's `x86_64-windows-musl` builds resolve from a host MinGW today, read from
  `-Wl,--verbose` in index CI. Import libraries such as `-lws2_32` may be among them, and each must
  be answered by the graph (openkal-windows) before the PE half lands. A `-l` the graph does not
  answer then fails as `unable to find library`, which is the graph's true answer. Add a diagnostic for that failure shape on graph links. It names
  the graph's C library and its version, following the precedent that explains `file not found`
  under `-nostdlibinc` (`src/build/ninja_backend.cppm:140-156`).
- **W2c (engine).** Extend the hermetic check to graph links. Every `-L` in the driver's linker
  invocation must lie under an allowed prefix: the xpkgs registry, the build directory, or the
  root of a graph package. A later edit that brings a host directory back then fails in CI instead
  of passing silently.
- **Other object formats and drivers.**

  | Link | Host search directories | Status |
  |---|---|---|
  | PE through clang's MinGW driver (`x86_64-w64-windows-gnu`, which is what mcpp hands clang for `x86_64-windows-musl`; `triple.cppm:485`) | This Linux host has Ubuntu's mingw-w64 installed. The driver adds `-libpath:/usr/lib/gcc/x86_64-w64-mingw32/13-win32`, `/usr/x86_64-w64-mingw32/lib` and `/usr/x86_64-w64-mingw32/mingw/lib`, and `-lm` reads `/usr/x86_64-w64-mingw32/lib/libm.a`. With `--sysroot=<empty>`, only directories under the empty sysroot remain, and `-lm` is `unable to find library`. | **measured**: same defect, same repair |
  | PE through `lld-link` in MSVC mode | reads `%LIB%` unless `/lldignoreenv` is given | to be measured on a Windows host |
  | GCC over the graph | its configured sysroot and library directories (#664 handled the compile side separately) | to be measured |
  | Mach-O | ld64.lld searches `<syslibroot>/usr/lib` and `/usr/local/lib`. The SDK is the declared platform anchor and is legitimate. | to be measured |

  The criterion is the same for each: the linker line from `-###` names no directory outside the
  allowed prefixes. The PE row matters beyond Windows hosts. mcpp-index measures
  `x86_64-windows-musl` on Linux runners, where the result depends on whether the runner image
  has mingw-w64 installed.
- **Order and cliff.** W2a must be released, and pinned through the openkal chain, before W2b
  ships. Otherwise every openkal consumer on a Linux target that links `-lm` breaks at once.
  Consumers that pin an older openkal-llvm-runtime still break when W2b ships. D3 accepted that
  cliff (§9.1): W2b ships without a warning-only release, and its diagnostic names the version
  that fixes it.

### 5.5 Criteria

- The `fmaximum` fixture on x86_64-linux-musl stops at `undefined symbol: fmaximum`, and the
  linker's `--verbose` output contains no host path.
- The report's aarch64 `-lm` fixture links, and the binary runs under qemu-aarch64. `-###` lists
  only store, build-directory and package `-L`.
- A negative test: a graph link that receives `-L/usr/lib` through `ldflags` is refused by the
  hermetic check, which names the directory.
- mcpp-index gains `x86_64-linux-musl` and `aarch64-linux-musl` rows in its openkal measurement.
  Today it measures `x86_64-linux-gnu` and `x86_64-windows-musl` only (`tests/openkal/pins.toml`),
  which is why this has not shown up there.

---

## 6. #693: paths outside ASCII on Windows, and one text-encoding model

### 6.1 The report

On Windows with `GetACP() == 936`, mcpp 2026.9.25.1 exits with 0xC0000409 and prints nothing when
it runs `--version` or `build --configure-only` from a directory whose name contains U+1F9EA
(TEST TUBE), which code page 936 cannot represent. The same `mcpp.exe` works in an ASCII
directory. The Ninja that mcpp brings reports `Build file encoding: UTF-8`.

The report proposes three steps:
1. a UTF-8 `activeCodePage` manifest in `mcpp.exe` and `build.mcpp.exe`;
2. an explicit boundary, with UTF-8 inside and UTF-16 wide APIs at Win32;
3. verification of each downstream tool.

### 6.2 Measurement

This host cannot run Windows, so the facts below come from a temporary draft PR (#697). The
branch keeps one measurement workflow and removes every other; it is closed once the readings are
recorded here. The runs are 36162457075, 36163057371, 36163850821 and 36164076852. The first
run's build readings are void because of a defect in the probe, recorded in Appendix A.10.

**Setup.**
- Machine: `windows-latest`, Windows Server 2025, build 26100. The system ANSI code page is
  1252 and the OEM code page is 437.
- Binaries:
  - the released mcpp 2026.9.25.1 from its release zip;
  - a copy of the same `mcpp.exe` into which `mt.exe` embedded a UTF-8 `activeCodePage`
    manifest (called "the UTF-8 copy" below);
  - xlings 2026.9.20.1, installed the way a user installs it.
- Directories, written as code points:

  | Name used below | Path | In cp1252? |
  |---|---|---|
  | ascii | `C:\w\ascii` | yes |
  | café | `C:\w\caf` + U+00E9 | yes, but its cp1252 bytes differ from its UTF-8 bytes |
  | CJK | `C:\w\repro-` + U+6D4B U+8BD5 + `-` + U+1F9EA | no |

- Toolchains: llvm@20.1.7 with the runner's MSVC 14.51 and Windows SDK 10.0.26100, `cl.exe`
  through `msvc@system`, and MinGW-w64 `gcc@16.1.0`. The Ninja is 1.12.1, which reports
  `Build file encoding: UTF-8`.

**Q1. Which process ends with 0xC0000409.** Each entry point ran `--version` in each directory:

| Entry point | ascii | café | CJK |
|---|---|---|---|
| `mcpp.exe` from the release zip | 0 | 0 | **0** |
| `mcpp.bat` from the release zip | 0 | 0 | **0** |
| the UTF-8 copy | 0 | 0 | 0 |
| the xlings shim `mcpp.exe`, inside the workspace that pins mcpp | 0, `mcpp 2026.9.25.1` | 0 | **0xC0000409, no output** |
| `xlings --version` | 0 | 0 | **0xC0000409, no output** |

`cdb` was run on `xlings --version` in the CJK directory. It records a first-chance C++
exception (`e06d7363`) whose `what()` is "No mapping for the Unicode character exists in the
target multi-byte code page." That is the MSVC STL's `std::system_error` from narrowing a path.
The exception is unhandled (second chance), which reaches `std::terminate` and the fast fail.
The stack is eleven frames inside `xlings.exe` (stripped), so the throw happens during start-up,
before any command runs.

**The silent exit in #693 belongs to xlings, not to mcpp.** The reporter's `mcpp` resolved to
the xlings shim.

**Q2 and Q3. Builds** (`mcpp build`, llvm row, three fixtures: `import std`, a narrow
`build.mcpp`, and a `build.mcpp` that uses the wide environment and prints UTF-8):

| Directory | Released `mcpp.exe` | The UTF-8 copy |
|---|---|---|
| ascii | all three build and run | all three build and run |
| café | **every build fails**: `error: internal: unhandled exception: [json.exception.type_error.316] invalid UTF-8 byte at index 9: 0x2F` | the `import std` fixture builds and runs; `build.ninja` and `compile_commands.json` carry `63 61 66 C3 A9` (UTF-8) and no cp1252 form |
| CJK | every build fails: `error: internal: unhandled exception: No mapping for the Unicode character exists in the target multi-byte code page.` (exit 70) | the `import std` fixture builds and runs; `build.ninja` carries `E6 B5 8B E8 AF 95 2D F0 9F A7 AA` |

Build programs, with the UTF-8 copy:

| `build.mcpp` | café | CJK |
|---|---|---|
| narrow (`getenv`, `std::ofstream(std::string)`, bytes printed back) | The program writes its file and prints the path in cp1252 bytes. mcpp then fails with `internal: unhandled exception: No mapping for the Unicode character…`. | The program receives `C:\w\repro-??-??\…` and cannot open it. It exits 2, and mcpp reports that exit. |
| UTF-8 (wide environment and path, UTF-8 output) | builds and runs | builds and runs |

**The other two Windows rows, with the UTF-8 copy.** Two fixtures ran (`#include` and
`import std`); every cell is "builds and runs", with UTF-8 bytes in `build.ninja`:

| Row | ascii | café | CJK |
|---|---|---|---|
| MSVC (`cl.exe`, `msvc@system`) | builds and runs | builds and runs | builds and runs |
| MinGW-w64 (`gcc@16.1.0`, `x86_64-windows-gnu`) | builds and runs | builds and runs | builds and runs |

In the MinGW payload, `gcc.exe`, `g++.exe`, `cc1.exe` and `cc1plus.exe` declare a UTF-8
`activeCodePage`. `collect2.exe`, `as.exe`, `ld.exe` and `ar.exe` carry a manifest without one.
They still succeeded, because Ninja runs in the build directory and hands them relative, ASCII
paths. None of these projects produced a response file, so response-file encoding was not
exercised.

**A non-ASCII `MCPP_HOME`, with an ASCII project.** This is the shape of a Windows account
whose user name is not ASCII, because the default home is `%USERPROFILE%\.mcpp`:

| `MCPP_HOME` | Released `mcpp.exe` | The UTF-8 copy |
|---|---|---|
| `C:\mh-caf` + U+00E9 (in cp1252) | The toolchain downloads and installs. Every build then fails with the JSON exception of the café row above. | builds and runs |
| `C:\mh-` + U+6D4B U+8BD5 (not in cp1252) | fails at once: `error: cannot create 'C:\mh-??\bin'` | mcpp itself proceeds. The xlings it vendors cannot initialise its sandbox under that home: ``warning: `xlings self init` failed for sandbox at 'C:\mh-测试\registry'``, then `sandbox not initialized`. |

**The Linux twin (measured on this host).** A project directory whose name is not valid UTF-8
(the single byte 0xE9) fails the same way on Linux with mcpp 2026.9.25.1:
`internal: unhandled exception: [json.exception.type_error.316] invalid UTF-8 byte at index 123:
0x2F`.

### 6.3 Findings

| | Finding | Evidence | Home |
|---|---|---|---|
| **F-693a** | xlings throws an unhandled `std::system_error` during start-up when the working directory is outside the ACP. Its `main` has no exception boundary, so the process ends silently with 0xC0000409. | measured (Q1, cdb) | xlings |
| **F-693b** | In a directory the ACP can represent but that is not ASCII, every mcpp build fails with an internal JSON exception. mcpp holds paths as ACP bytes, and the JSON it writes requires UTF-8. On a Chinese system (ACP 936) this is every directory with a Chinese name, which is more common than the report's case: GBK byte sequences are, with rare accidental exceptions, not valid UTF-8. The first JSON document on that path is `compile_commands.json`, whose writer is `src/build/compile_commands.cppm`, and the file is absent after the failure. | measured on cp1252; the cp936 case and the writer are reasoned | mcpp |
| **F-693c** | Outside the ACP, every mcpp build fails with an internal narrowing exception. The failure is reported (exit 70), not silent. | measured | mcpp |
| **F-693d** | The build-program contract has no encoding. mcpp passes paths through the environment, which is converted through the program's own ACP, and reads the program's output as bytes. A narrow build program and mcpp disagree as soon as either one is not in the ACP. | measured | mcpp |
| **F-693e** | On POSIX, path bytes that are not UTF-8 reach the same JSON serializer. | measured (Linux) | mcpp |
| **F-693f** | F-693b applies to the home as well as the project. With 2026.9.25.1, a Windows user whose account name is not ASCII cannot build any project, even an ASCII one, when the name is in the ACP. That is the common case: a Chinese name on a Chinese system. Outside the ACP, the vendored xlings fails too, even under the UTF-8 copy. | measured on cp1252; the cp936 case is reasoned | mcpp; xlings for a home outside the ACP |

- **Usage:** correct everywhere. A directory name with any Unicode character is valid on every
  platform.
- **Root cause:** mcpp has no declared text encoding. On Windows its strings are in whatever the
  process ACP is. The formats it writes and the tools it drives assume UTF-8: JSON by
  definition, Ninja by its manifest, clang internally. On POSIX, bytes flow unvalidated into
  formats that accept only UTF-8.

### 6.4 One text-encoding model: UTF-8 everywhere

The review asked whether mcpp can adopt UTF-8 as its single model, weighing compatibility,
cross-platform behaviour and established build conventions. **It can, and the measurements
support it.** Established practice:

| Tool | Text inside | At the Windows boundary | Files it writes for other tools |
|---|---|---|---|
| Ninja 1.11 and later | bytes, read as UTF-8 | UTF-8 `activeCodePage` manifest (Windows 10 1903 and later); `ninja -t wincodepage` reports the build-file encoding it expects | — |
| CMake 3.2 and later | UTF-8 | wide APIs | build files in UTF-8; the Ninja generator had to stop writing ANSI once Ninja moved to UTF-8 |
| LLVM and clang | UTF-8 | wide APIs; the command line through `GetCommandLineW` | response files in the encoding each consumer reads: UTF-8 or UTF-16 for clang, UTF-16 for MSVC's `CL.exe` and `LINK.exe`, ANSI for GNU tools on MinGW (`clang::driver::ResponseFileSupport`) |
| MSVC tools | UTF-16 | wide | read response files and `.DEF` files as UTF-16 or UTF-8 **with a BOM**, and as ANSI otherwise |
| GNU tools on MinGW | narrow `argv` | the ANSI code page; a GCC patch (PR108865) embeds a UTF-8 manifest in the driver, and mcpp's `gcc@16.1.0` payload carries it in the driver and compilers but not in `as`, `ld` or `collect2` (measured) | read response files as ANSI only |
| Rust and Cargo | UTF-8 (WTF-8 for OS strings) | wide APIs only | — |
| Microsoft's guidance | — | `activeCodePage` UTF-8 lets code written against the -A APIs run in UTF-8 on Windows 10 1903 and later; convert with `CP_UTF8` explicitly, because `CP_ACP` equals `CP_UTF8` only under the manifest | — |

**The model for mcpp, M1 to M7:**

- **M1. One encoding inside.** Every string mcpp holds for a path, an argument, an environment
  value or file content is UTF-8, on every platform.
- **M2. Windows process setting.** `mcpp.exe` declares `activeCodePage` UTF-8. Measured
  sufficient: mcpp's own stages, and all three Windows rows end to end, in both kinds of
  non-ASCII directory. Every path that writes to a console must render UTF-8. `std::print`
  does, through `WriteConsoleW`. C stdio and iostream do not unless the console output code
  page is 65001, and this has to be checked in the implementation, because CI has no console.
- **M3. The programs mcpp runs as part of a build share M2.** This covers `build.mcpp` and
  rule-package programs (D4, measured necessary), and host tools run by actions (D6). It also
  covers the `xlings.exe` that mcpp vendors and runs with its own paths: under a home outside
  the ACP, mcpp in UTF-8 still fails until xlings is in UTF-8 too (measured, F-693f).
- **M4. Every file mcpp writes for another tool uses the encoding that tool reads.**
  - `build.ninja` is UTF-8. Ninja 1.11 or later is already required through
    `ninja_required_version = 1.11`. mcpp checks that the encoding Ninja declares
    (`-t wincodepage`) equals its own: UTF-8 under M2, or ANSI in the legacy mode of M7. A
    mismatch is refused by name.
  - `compile_commands.json` is UTF-8.
  - Response files are UTF-8 for LLVM tools and UTF-8 with a BOM for MSVC tools. For GNU tools
    on MinGW, a path the ANSI code page cannot represent is refused with a diagnostic that names
    the tool, instead of being written wrong.
  - Resource scripts are already compiled as UTF-8: the rc tool receives `/C 65001` or
    `--codepage=65001` (`src/build/prepare.cppm:14579-14582`). Nothing changes here.
- **M5. Validate at the point of entry.** Text that comes from outside mcpp is validated as
  UTF-8 where it enters: POSIX file names, build-program output, and POSIX environment values
  used as paths. Invalid input is refused with a diagnostic that names its source, never with
  an internal exception. No JSON writer lets an exception reach `main`.
- **M6. Scope.** The programs mcpp builds for the user (`mcpp run`, the artefacts) keep their
  own encoding model, and mcpp adds no manifest to them. A `[resources]` key that declares one is
  a possible later feature, and `[resources] files` already allows it. POSIX behaviour is
  unchanged apart from M5.
- **M7. Older hosts (D7).** Windows 10 1809 and earlier, and Windows Server 2019 (build 17763),
  ignore the manifest. mcpp checks `GetACP()` at start-up and reports the legacy mode in one
  `degraded` line when a path is not ASCII. W4e removes the dependency on the OS version.

**Compatibility.**
- **ASCII paths.** UTF-8 and every ANSI code page agree on ASCII, so for ASCII paths
  `build.ninja`, `compile_commands.json` and every command line are byte-identical. There are
  no rebuilds and no cache changes.
- **Non-ASCII project and home paths.** Every build under one already fails on 2026.9.25.1
  (F-693b, F-693c, F-693f), so no working configuration of this kind can regress. State that
  2026.9.25.1 installed under a non-ASCII home remains usable: the UTF-8 copy built with the
  toolchain the released binary had installed there (§6.2).
- **Names that #516 and #518 skipped.** A file outside the ACP inside an otherwise buildable
  project used to be skipped, with a report naming its directory. Under M2 it is no longer
  skipped, so a source glob that matched such a file now compiles it. Today's skip report
  names every affected directory, so the projects concerned can be found before the release.
- **Build programs.** A narrow build program in a non-ASCII directory fails today, because
  mcpp fails first. After M2 without M3 it would still fail (§6.2), which is why D4 and M3 are
  one change.
- **Readers of mcpp's piped output.** Programs that decode mcpp's piped output as the ANSI code
  page see UTF-8 for non-ASCII text. ASCII output is unchanged.

### 6.5 Repair (W4)

- **W4a (xlings, upstream).** Report F-693a and the xlings half of F-693f:
  - the reproduction: `xlings --version` in a directory outside the ACP, and `self init` under a
    home outside it;
  - the exception text, and the missing boundary in `main`.

  The suggested fix is the M2 manifest for `xlings.exe`, and therefore for its shims, plus an
  exception boundary. The report's symptom is xlings's, so this item closes #693 as reported.
  mcpp vendors xlings (`registry/bin/xlings.exe`), so a home outside the ACP also waits on
  this item.
- **W4b (mcpp).**
  - The M2 manifest for `mcpp.exe`, through mcpp's own `[resources] files` (an `RT_MANIFEST`
    at ordinal 1, docs/04 §2.15).
  - The same manifest for build programs, through the engine's host-compile path (M3, D4), and
    for host tools subject to D6.
  - The start-up check of M7.
- **W4c (mcpp).** M5: validation at the entry points, with diagnostics instead of internal
  exceptions. This covers build-program output, POSIX path names, and the JSON writers.
- **W4d (mcpp).** M4: the Ninja encoding check, and response files per tool. Measure
  response-file encoding with a project large enough to need one, which this measurement did
  not produce.
- **W4e (mcpp, later).** Wide APIs at the Win32 call sites, with explicit UTF-8 conversion for
  `std::filesystem::path`, which means construction from `std::u8string`. This removes the
  dependence on the manifest and on the OS version. The call sites are listed in Appendix A.11.
- **W4f (CI).** The measurement workflow becomes a regression job on `windows-latest`. It runs
  the ascii, café and CJK directories against the llvm, MSVC and MinGW rows, plus the
  build-program fixture. It asserts success, and it asserts UTF-8 bytes in `build.ninja` and
  `compile_commands.json`.
- **Terminate handler, demoted to optional hardening.** mcpp's own `main` reported every mcpp
  failure the measurement produced. A terminate handler would add coverage only for exceptions
  that cross a `noexcept` boundary, and no measured case needs it.

**Criteria.**
- W4f is green on all rows, including a non-ASCII `MCPP_HOME`. The row with a home outside the
  ACP turns green once X lands and mcpp's xlings pin moves to that release.
- On Linux, a directory whose name is not UTF-8 is refused with a diagnostic that names it.
- A narrow build program that prints a path which is not UTF-8 is reported by name, and mcpp
  does not stop with an internal exception.
- "Unicode paths are supported" is claimed only for the rows that W4f runs, as #518 required.

---

## 7. What the four have in common

**C1. Silence.**

| | What went unreported | What should have reported it |
|---|---|---|
| #694 | a profile's level replaced by `-Og` | the `Finished` descriptor, which instead printed `[optimized]` |
| #695 | a declared key dropped | the manifest reader; three descriptors recorded it as "the value does nothing" |
| #696 | a glibc object linked into a musl image | the hermetic check, which wrote its OK marker |
| #693 | a process that died | xlings's `main`, which has no exception boundary; mcpp's own failures were reported, but as internal exceptions |

The rule is Q1 together with P7. `mcpp.diag`'s `degraded` channel exists for exactly this case.

**C2. The file-level flag variables work as a broadcast channel.** #695's `-std=` travels on
`$cflags`, which every C unit of every package reads. #690 F7 was the include-directory case on
the same channel. #694's `-Og` travels there too, and there it is legitimate: the optimization
level a profile chooses is graph-wide, as in Cargo. In #694 the defect is the override, not the
channel.

The rule: file-level variables carry only graph-wide values, meaning the toolchain, the target,
the profile, and values that must be uniform (such as the C++ standard, for BMIs). Anything a
package declares for itself travels per unit.

This rule is enforced by construction and by a test, not by a document (D5). The direct form is
a flag model in which each token carries its scope (graph, package or unit), and the backend
builds the file-level variables from graph-scoped tokens only. A misplaced package value is then
a type error rather than a review finding. The minimal form, which W3 adds, is a negative test:
build a plan whose root declares every package-private key (`c_standard`, `include_dirs`,
`private_include_dirs`, `defines`, `cflags`, `cxxflags`), and assert that none of those values
appears in the file-level `$cflags` or `$cxxflags`. Reasoned from §4.2, the tree at 82867ad7
fails this test on `c_standard` alone, and passes it once W3 lands.

**C3. Two answerers per question.**

| Question | First answerer | Second answerer |
|---|---|---|
| which optimization level | the descriptor | the flag |
| which C standard | the cache key and fingerprint | the compile line |
| can the link reach the host | the hermetic check's model (CRT and loader) | the linker's actual inputs |
| which encoding | mcpp's strings (the ACP) | the JSON writer (UTF-8), Ninja (UTF-8), the build program (its own ACP) |

Each pair agrees on the configurations CI exercises and disagrees on the others. The
configurations where they agree are:

- glibc targets;
- graphs in which no dependency declares its own C standard;
- graph links in which the graph answers every `-l`;
- ASCII paths.

A green build on a configuration where both answerers agree carries no information about the
difference between them.

**C4. The coverage has the same shape.** Every defect sits on an axis that CI does not exercise:
musl with an optimizing profile, a C dependency with its own standard, a graph link on aarch64
or with an unanswered `-l`, and Windows with a non-UTF-8 ACP and a non-ASCII path. The criteria
above sit on those axes. The CI additions are:

1. a musl release build whose `compile_commands.json` is asserted;
2. e2e coverage of the per-package C standard in each position;
3. a graph link with `-lm` on aarch64-linux-musl under qemu, a PE graph link from a host that
   has mingw-w64 installed, and the hermetic negative test;
4. the W4f job: on a runner whose ACP is verified not to be 65001, the ascii, café and CJK
   directories against the llvm, MSVC and MinGW rows, a build program, and a non-ASCII home.

**C5. A design record can be wrong in a table cell.** #690 §3.1 labelled `c_standard`
"consistent" without a criterion. A classification that nothing measured is not a finding.

---

## 8. Plan

The tracks below group the work by issue. §12.1 gives the pull requests, one per repository,
and the order in which they land.

| Track | Content | Repository | Depends on | Size |
|---|---|---|---|---|
| **A** | W1a to W1d (#694): delete the workaround, one answerer for the level, the stale comment, the property test | mcpp | nothing | small |
| **B** | W3 (#695): semantics, mechanism, cache key, documents, and the C2 negative test | mcpp | mcpp-index CI through `MCPP_SOURCE_REF`, which adds GCC and the other hosts to the §4.6 sweep (clang on Linux: 14 of 14 build) | medium |
| **B2** | W3b: the MSVC dialect's `/std:` for declared C standards | mcpp | B, and mcpp-index CI on the MSVC row | small |
| **C1** | W2a: openkal-musl publishes the eight empty archives and the `-L` | openkal-musl | nothing | small |
| **C2** | openkal-llvm-runtime moves its openkal-musl pin; index registration | openkal-llvm-runtime, mcpp-index | C1 released and mirrored | small |
| **C3** | W2b for ELF links, and W2c (#696): graph-link `--sysroot`, the diagnostic, the hermetic `-L` check | mcpp | C2 in the index | medium |
| **C4** | mcpp-index gains linux-musl rows in its openkal measurement, and records which `-l` names its `x86_64-windows-musl` builds resolve from a host MinGW | mcpp-index | C3 released | small |
| **C5** | W2b for PE links through the MinGW driver | mcpp (and openkal-windows for any name C4 finds) | C4's inventory answered by the graph | small |
| **U0** | the measurement (§6.2, PR #697) | done | — | — |
| **U1** | W4b and W4f: the UTF-8 manifest for `mcpp.exe` and for build programs (and for host tools, per D6); the start-up check (M7); the Windows regression job over three directories and three rows | mcpp | nothing | medium |
| **U2** | W4c and W4d: validation at the entry points, response files per tool, `.rc` code page | mcpp | U1 | medium |
| **U3** | W4e: wide APIs at the Win32 call sites and explicit UTF-8 conversions | mcpp | U1 | medium to large |
| **X** | W4a: xlings start-up exception, the manifest for `xlings.exe` and its shims, an exception boundary in `main` | xlings (upstream issue) | nothing | small |

Ordering notes, following the release discipline already recorded for this repository:

- Registering a package and moving a pin are separate PRs.
- A consumer's pin moves only after the index entry has merged.
- Track B changes what 13 descriptors and openkal-musl compile, so it ships only after
  mcpp-index CI is green against the branch.
- Track A changes the optimization level of the published Linux binaries, so the release note
  states it.
- Tracks A, U1 and X depend on nothing and can go first. The U tracks are independent of
  tracks B and C.
- #693 as reported closes with X. mcpp's own share, U1 and U2, fixes the larger defect that
  the measurement found: every non-ASCII directory.
- U1 changes nothing for ASCII paths (§6.4, Compatibility), so it needs no ecosystem build
  measurement before release. Two things are still checked:
  - the index's Windows CI logs are searched for today's `path/codepage` skip reports, because
    those files stop being skipped;
  - the release note states that mcpp's piped output carries UTF-8 for non-ASCII text.

---

## 9. Decisions

### 9.1 Accepted in review (2026-09-26)

D1 to D4 were accepted on the first review, and D5 to D7 on the second.

| | Decision |
|---|---|
| **D1** (#694) | Remove the musl `-Og` workaround entirely (W1a). The report's narrower patch is not taken. |
| **D2** (#695) | A package's C standard is its own declared value, or the engine default `c11`; it never comes from the consumer (W3). |
| **D3** (#696) | openkal-musl ships musl's empty archives as data (W2a). W2b ships directly after the openkal chain, **with no warning-only release**. The cliff for consumers that pin an older openkal-llvm-runtime is handled by W2b's diagnostic, which names the version that fixes it. |
| **D4** (#693) | Build programs carry the same UTF-8 `activeCodePage` manifest as `mcpp.exe`. The measurement (§6.2) turned this from a recommendation into a requirement: with only `mcpp.exe` in UTF-8, a narrow build program fails in every non-ASCII directory. |
| **D5** | No workaround layer and no rules in a skill (§3.5, §7 C2). Defective toolchain versions are answered by the pin (data) and, in a project that keeps one, by per-file flags (usage). Two structural guarantees are enforced by tests: W1d (the level realised is the level declared) and the C2 negative test (no package-private value in a file-level variable). |
| **D6** | Host tools built for the graph carry the UTF-8 manifest by default. A target opts out with `windows_code_page = "legacy"` (§12.2), and the default yields to a manifest the package embeds itself (§12.4). |
| **D7** | On a Windows host that ignores the manifest, mcpp reports the legacy mode when a path is not ASCII, and keeps the skip-and-report behaviour of #516 and #518. Realised inside the diagnostics each such path reaches, which name the process code page (§12.4). |

---

## 10. Incidental observations

- `--toolchain gcc@15.1.0-musl` did not override `[target.x86_64-linux-musl] toolchain`. The
  build resolved `gcc@16.1.0` and printed nothing about the flag it ignored. Whether the per-target
  pin should outrank the flag was not investigated. The silence is the finding.
- mcpp at 82867ad7 cannot be built with musl-gcc 15.1.0 (`exposes TU-local entity`). v0.0.1
  compiles with it but does not link. On musl targets, mcpp's effective minimum GCC is 16.
- docs/04 §2.10 states that a dependency's artefacts do not depend on who consumes them. For C
  dependencies this is false until W3 lands.
- The #690 record needs a correction note on `c_standard` in §3.1.
- `compile_commands.json` is an auxiliary artefact for editors, yet a failure to write it stops the
  whole build (F-693b). After M5 it cannot fail on encoding. A failure to write an auxiliary
  artefact should still be reported as `degraded` rather than abort the build that produces
  it.

---

## 11. Self-review

Revision 2 was read end to end against four questions: whether each claim carries the evidence
it states, whether the sections agree, which risks the plan still hides, and what remains
unmeasured.

**Corrections made in this revision.**
- Revision 1 predicted that non-ASCII paths the ACP can represent would fail at the first compile
  edge, because Ninja reads the ACP bytes as UTF-8. The measurement found the failure earlier, in
  mcpp's own JSON writer (F-693b), and wider: it covers the user profile as well (F-693f). The
  conclusion held; the mechanism in revision 1 was wrong.
- Revision 1 left open which process dies in #693. It is xlings (F-693a), and the terminate
  handler proposed for mcpp is demoted to optional hardening.
- The cp936 statements in F-693b and F-693f are marked as reasoned from the cp1252 measurement.
  They were not measured on a cp936 machine.

**Risks found during this review and folded into the plan.**
- **W3b.** Mapping a C standard to `/std:c11` on `cl.exe` also switches on the conforming
  preprocessor, and 90 descriptors declare `c11`. The MSVC mapping is separated from W3 and
  measured first (track B2).
- **W2b on PE.** Revision 1 would have applied `--sysroot` to PE links through the MinGW driver
  at once. The index's `x86_64-windows-musl` builds may resolve import libraries from a host
  MinGW today, so an inventory (C4) precedes the PE half (C5).
- **M4.** In the legacy mode of M7, Ninja declares ANSI. The check therefore compares the two
  encodings; it does not require UTF-8.
- **Compatibility of M2.** Files that #516 and #518 skipped are no longer skipped. Today's skip
  reports in the index's Windows CI are read before the release.
- **X is on mcpp's path.** A home outside the ACP needs the xlings fix and a move of mcpp's
  xlings pin, not only the manifest in `mcpp.exe`.

**Probe defects of this review, recorded so that the readings can be trusted.**
- In the first Windows run, the loop variable `$fx` overwrote the fixtures directory `$Fx`.
  PowerShell names are case-insensitive, so every build "ran" in 0 s. Those readings are void,
  and the script now records a start error as a probe defect.
- The §4.6 sweep was checked for the treatment being served from a cache compiled at another
  standard. `.ninja_log` durations and command hashes rule it out.
- The #694 probe raised only the root package to `-O2`. The removal PR's CI covers the other 51
  units.

**What remains unmeasured.**

| Item | Where it gets measured |
|---|---|
| response-file encodings (M4, W4d); no project in the measurement produced one | W4d, with a project large enough to need one |
| console rendering under M2 through C stdio and iostream | the implementation, on a machine with a console, because CI has none |
| Windows hosts older than 1903 (M7, D7) | no such runner is available; reasoned from Microsoft's documentation |
| an actual cp936 machine | the reporter's machine can confirm; the mechanism is the one measured on cp1252 |
| host tools under the manifest (D6) | the implementation PR |
| GCC over graph links, Mach-O graph links, `lld-link` in MSVC mode (§5.4) | before W2b is extended to them |
| W3 with GCC and on non-Linux hosts | mcpp-index CI through `MCPP_SOURCE_REF` |

**Scope, checked against the routing rule.**
- Every engine item repairs a defect; none adds a product feature.
- W2a is data, and X is upstream in xlings.
- The one item that would be a feature, a user-facing `[resources]` key for the code page, is
  deferred, because `[resources] files` already allows it.

---

## 12. Implementation

### 12.1 Repositories, pull requests and order

Each repository receives one pull request. The exception is mcpp-index, because registering a
version and moving a pin are always separate changes. The order follows the release chain:
a dependency is tagged, mirrored and registered before anything pins it.

| # | Repository | Pull request | Depends on | Released as |
|---|---|---|---|---|
| 1 | openxlings/xlings | X: the UTF-8 `activeCodePage` manifest in `xlings.exe` (and therefore in its shims) through `[resources] files`; an exception boundary in `main`; a Windows CI step that runs xlings in a directory outside the ACP | nothing | xlings 2026.9.26.1, mirrored to GitCode |
| 2 | mcpplibs/openkal-musl | W2a: the eight empty archives and a package-relative `-L`; CI asserts that `-lm` resolves inside the package | nothing | 0.19.2, mirrored to GitCode |
| 3 | mcpplibs/mcpp-index | registers openkal-musl 0.19.2 | 2 | index artifact |
| 4 | mcpplibs/openkal-llvm-runtime | pins openkal-musl 0.19.2 | 3 | 0.15.2, mirrored to GitCode |
| 5 | mcpplibs/mcpp-index | registers openkal-llvm-runtime 0.15.2 | 4 | index artifact |
| 6 | mcpp-community/mcpp | W1a to W1d, W2b (ELF) and W2c, W3 with the W3b report, W4b to W4d, W4f, D6, D7, the documents, and `kXlingsVersion` moved to 2026.9.26.1 | 1 and 5 | mcpp 2026.9.26.x; GitCode by the local gtc; the xim-pkgindex bump |
| 7 | mcpplibs/mcpp-index | pins `tests/openkal/pins.toml` to runtime 0.15.2 and the new mcpp, adds the `x86_64-linux-musl` and `aarch64-linux-musl` rows, moves `latest_mcpp`, and corrects the three comments that blamed `gnu11` | 5 and 6 | index artifact |

Three items are split out, each for a stated reason.
- **The W3b mapping for `cl.exe`.** No CI row builds the index with `cl.exe`: the Windows row of
  `validate.yml` drives clang. Under P8, the mapping waits for such a row. Until then, this
  round reports the unapplied value (Q1) and does not change what `cl.exe` compiles.
- **W4e, wide APIs at the Win32 boundary.** It is all or nothing. Converting only the process
  calls would decode ACP strings as UTF-8 on the hosts that ignore the manifest. The manifest
  covers Windows 10 1903 and later, and D7 covers the rest.
- **W2b for PE links (C5).** It waits for the C4 inventory of the import libraries that the
  index's `x86_64-windows-musl` builds resolve from a host MinGW.

### 12.2 mcpp, by change

- **W1** (`src/build/flags.cppm`, `src/build/execute.cppm`, `mcpp.toml`,
  `tests/unit/test_ninja_backend.cpp`):
  - The musl branch of `opt_flag` goes.
  - One function names the realised optimization level, and both the flags and the
    `Finished` descriptor read it.
  - The stale aarch64 comment is corrected.
  - The W1d property test runs over every row of `kKnownTargets`.
- **W3** (`src/build/flags.cppm`, `src/build/plan.cppm`, `src/build/cache_key.cppm`,
  `docs/04`, `docs/07`, tests):
  - The file-level `$cflags` carries `-std=c11` as a graph-wide constant.
  - A package whose effective standard differs gets its spelling in its C units' flags,
    through the mechanism `implementationStandardFlag` uses for C++.
  - The root's standard leaves the dependency cache key.
  - The C2 negative test is added.
  - On the MSVC dialect, one line per build reports the declared standards that `cl.exe`
    does not apply (W3b).
- **W2** (`src/build/flags.cppm`, `src/build/hermetic.cppm`, `src/build/ninja_backend.cppm`,
  tests):
  - An ELF link over a graph-supplied C library passes `--sysroot` to an empty directory
    that mcpp owns.
  - A link that then fails with `unable to find library` names the graph's C library.
  - The hermetic check refuses a `-L` outside the allowed prefixes on such a link.
- **W4, D6, D7**:
  - **The manifest for user targets.** A new target key, `windows_code_page = "utf-8" |
    "legacy"`, uses the vocabulary of the manifest element itself. It embeds the manifest in a
    PE executable through the existing resource pipeline, where the synthesized script gains
    one `RT_MANIFEST` entry.
  - **Defaults.** Ordinary targets default to `legacy` (M6). Targets built as host tools
    default to `utf-8` (D6). Build programs always carry the manifest (D4): `build_program.cppm`
    compiles the resource with the host toolchain's rc tool and links it into
    `build.mcpp.exe`.
  - **mcpp's own executable** carries the manifest through `[resources] files`, because the
    bootstrap engine that builds mcpp does not know the new key.
  - **Validation (W4c).** Paths, build-program output and file names are validated as UTF-8
    where they enter (M5). A path mcpp cannot name is refused, or skipped and reported, and is
    never an internal exception. Failing to write `compile_commands.json` is a `degraded`
    report, not an abort.
  - **The Ninja check (W4d).** Ninja's declared encoding is compared with mcpp's own. MSVC
    response files carry a UTF-8 BOM when their content is not ASCII.
  - **The start-up check (D7)** reads `GetACP()`.
  - **The regression job (W4f)** turns the measurement of §6.2 into a CI job.

### 12.3 Review by angle

| Angle | How the plan answers it |
|---|---|
| Architecture | Each item lives where the routing rule puts it: stubs in data (openkal-musl), the silent exit upstream (xlings), and general defects in the engine. The engine gains no workaround layer (D5), and scope is carried by construction and enforced by tests (C2). |
| Stability | Every behaviour change was measured before it was planned: W1 on both musl targets, W3 on 14 packages, W2 on two distributions and on PE, W4 on three Windows rows. The changes that could not be measured are split out, not shipped blind. |
| Simplicity | One new key (`windows_code_page`). Everything else reuses existing mechanisms: the resource pipeline, the per-unit standard flag, the dependency `-L` normalisation, the `diag` channel. |
| User experience | Internal exceptions become diagnostics that name the path, the program or the tool. The `Finished` label states the level used. A cliff (D3) names the version that crosses it. |
| Compatibility | Manifests read by bootstrap engines gain no new key. For ASCII paths the output is byte-identical. Build programs and `mcpp.exe` change together (D4). |
| Cross-platform | The Windows rows (llvm, MSVC, MinGW) and the Linux twin are measured. macOS takes W3 like every host, and W1 and W2 do not reach it: no musl target, and Mach-O graph links are unchanged. |
| Consistency | C and C++ standards use one per-unit mechanism. The code-page vocabulary is Microsoft's. The key follows `windows_subsystem` and `windows_entry`. |
| Upgrade without surprise | Caches rebuild once only where the command line changes: musl builds (W1); C dependencies of a root that declared a non-default standard (W3); graph links (W2). A project with only ASCII paths sees no other difference. |
| Test coverage | W1d and the C2 negative test in the unit suite; e2e for the per-package standard, the graph link and the Linux non-UTF-8 refusal; the W4f job on Windows; the xlings and openkal-musl CI steps; the sandbox verification after release. |

### 12.4 Implementation record (2026-09-26)

**Status by repository.** Rows refer to §12.1.

| # | Pull request | State |
|---|---|---|
| 1 | openxlings/xlings#613 | merged as `2e3df6dd`; released as 2026.9.26.2 (2026.9.26.1 had been released separately earlier the same day); the four GitCode archives match the GitHub sha256 sidecars; xim-pkgindex#876 (the bot's +22/-3) merged with its hashes checked against the sidecars |
| 2 | mcpplibs/openkal-musl#43 | merged as `20b92683`; tag `0.19.2`; the GitHub archive (1157319 bytes, sha256 `e8043bcd...c82238`) and the GitCode copy compared byte for byte; the archive carries `port/lib/lib*.a`, eight files of eight bytes |
| 3 | mcpplibs/mcpp-index#467 | merged as `1529f5f3`; the index artifact `1529f5f` is served by both hosts with the pointer's digest |
| 4 | mcpplibs/openkal-llvm-runtime#30 | merged as `79671f6b`; tag `0.15.2`; archive 15631743 bytes, sha256 `c2219ad8...c0d0d9`, GitCode copy byte-identical; its mcpp.toml reads openkal-musl `0.19.2` |
| 5 | mcpplibs/mcpp-index#468 | merged as `d7872cc3`; the index artifact `d7872cc` is served by both hosts with the pointer's digest |
| 6 | mcpp-community/mcpp#698 | merged as `f61b4663`, whose tree equals the tested head `15317aab`; released as 2026.9.26.1 (§12.5) |
| 7 | mcpplibs/mcpp-index#469 | merged as `2af34182`: validate.yml, openkal-compat.yml and `latest_mcpp` name 2026.9.26.1, the openkal measurement uses runtime 0.15.2 and gains the two linux-musl rows, and the three `gnu11` comments are corrected; the full sweep is green on every platform (§12.5) |

**Where the implementation departs from §12.2, and why.**

- **W4c distinguishes the entry points.** A project directory or `MCPP_HOME` with no UTF-8
  spelling is refused before anything is written; a name inside a project is skipped and
  reported through the existing `path/codepage` channel of #516; a `build.mcpp` directive whose
  text is not UTF-8 is refused by its key. Skipping rather than refusing keeps the #516 contract
  for names that are test data. One function, `try_narrow`, decides all three, so the Windows
  code page and the POSIX byte case share one predicate. A serialiser failure in
  `compile_commands.json` becomes that document's write failure (a warning, or an error when the
  database is required), never an exception that reaches `main`.
- **D7 is realised inside the diagnostics it affects.** On a host that ignores the manifest,
  every non-ASCII path reaches one of three diagnostics (the refusal of a project or home, the
  skip report, the Ninja check), and each of them names the process code page. A separate
  start-up line would repeat them, and a legacy host with ASCII paths is unaffected and is told
  nothing.
- **The Ninja check compares against UTF-8, not against mcpp's code page.** Every path in
  `build.ninja` has passed the UTF-8 check, and every other string is UTF-8 by construction, so
  the file is UTF-8 in every mode. A Ninja reading ANSI would misread it on a legacy host too.
- **The byte order mark is written on every msvc response file, not only on non-ASCII ones.**
  Round 5 measured ASCII content with the mark as accepted by `cl.exe`, `link.exe` and `lib.exe`,
  and a conditional mark would need a per-edge variable for no behavioural gain.
- **The host-tool default (D6) yields to a manifest the package embeds itself.** Both would sit
  at ordinal 1; the package said nothing about code pages, and its own manifest is the one it
  ships. A declared `windows_code_page = "utf-8"` beside such a manifest is refused instead.
- **mcpp's own manifest** is `res/mcpp.rc`, which names `mcpp.exe.manifest` beside the script.
  Round 5b measured rc.exe and windres, and a local run measured llvm-rc, all finding the file
  there when run from another directory; mcpp resolves it there as a build input.
- **The rc scanner** tracks a manifest named by the numeric type `24` and treats a statement as
  a manifest only when a file name follows the type, so `FILEVERSION 24,1,0,0` is not one.
- **A target's entry `main` written in C takes its package's standard.** The entry is
  synthesized after `make_plan`'s unit loop, so a flag added only inside the loop missed it;
  found in self-review, one helper now serves both sites (unit test
  `ACEntryMainTakesItsPackagesStandard`).
- **The cache key records a package's C standard only when it differs from `c11`.** A package
  that spells the default and one that says nothing compile identically, so they share a key.
  The first CI run failed the unit test that encoded the old rule; the test now states the new
  one (`DeclaringTheDefaultCStandardKeysNothing`).
- **openkal-llvm-runtime pins openkal-musl exactly.** A root that pins openkal-musl 0.19.2
  beside runtime 0.15.1 is refused as irreconcilable (measured with e2e 778), so a consumer
  receives the archives by moving the runtime pin, which is what the #696 note says; the
  fixture pairs musl 0.19.2 with runtime 0.15.2.

**Test results on Linux** (the worktree at `fix/693-696`):

| Suite | Result |
|---|---|
| unit: modgraph, build_directives, ninja_backend, c_standard_per_package, hermetic_graph_link, compile_commands, build_resources | 57, 55, 92, 6, 4, 23 and 15 tests pass |
| e2e 776, a path with no UTF-8 spelling is named (new) | passes; released 2026.9.25.1 fails at its first criterion with the JSON exception |
| e2e 777, `c_standard` applies to the package that declares it (new) | passes; 2026.9.25.1 compiles `cdep` and `plain` at the consumer's `c99` |
| e2e 778, a graph link searches no host directory (new), leg B | passes; 2026.9.25.1 links leg B from the host's `libm` |
| e2e 778, legs A, C (aarch64 under qemu) and D (`-L/usr/lib` refused) | run once openkal-llvm-runtime 0.15.2 is in the index |
| CI run 1 of #698 | the two xcode-27 legs fail as on main (#669, lld 22 rejects `arm64e.x1` in the SDK stubs); `test_cache_key` failed on the old key rule (fixed) |
| e2e 190 | accepts the byte order mark before `$in_newline` |

The Windows rows run in CI through `.github/tools/check_unicode_paths.sh` (W4f): llvm, MSVC and
MinGW in an ASCII directory, `caf` + U+00E9 and U+6D4B U+8BD5, plus a path through `build.mcpp`.
CI run 1 of #698 (windows-latest, code page 1252; `mcpp.exe` self-hosted by the bootstrap
2026.9.24.1 with `res/mcpp.rc`):

```
  ok    llvm in 'ascii'        ok    msvc in 'ascii'        ok    mingw in 'ascii'
  ok    llvm in 'café'         ok    msvc in 'café'         ok    mingw in 'café'
  ok    llvm in '测试'          ok    msvc in '测试'          ok    mingw in '测试'
  ok    a path through build.mcpp in '测试'
OK: every row builds in every directory
```

### 12.5 Release and ecosystem readings (2026-09-26)

- **CI.** Run 3 of #698 (`15317aab`): 40 checks pass; the two that fail are the xcode-27 legs,
  which fail the same way on `main` (#669: `ld64.lld` 22.1.8 rejects `arm64e.x1` in that image's
  SDK stubs). The push run on `main` (`f61b4663`) reads the same: every workflow passes except
  those two legs.
- **The Windows artefact.** `mcpp-2026.9.26.1-windows-x86_64.zip` carries `RT_MANIFEST` ID 1
  with `activeCodePage` UTF-8 in `bin/mcpp.exe`, and the bundled `registry/bin/xlings.exe`
  (2026.9.26.2) carries the same; read with `llvm-readobj --coff-resources` from the CI artefact.
- **Release.** Run 36184397880, every job successful (four platform builds, the sealed manifest,
  `publish-ecosystem`). The Linux x86_64 archive was uploaded to GitCode from a CN host the
  moment it appeared; the other three by `publish-ecosystem`. All four GitCode archives match the
  GitHub sha256 sidecars, their sidecars answer 200, and the GitHub `xlings-res/mcpp` copies
  answer 200. xim-pkgindex#877 (+22/-3) was merged after its six hashes were compared with the
  sidecars; AUR, Homebrew and PyPI publishing succeeded on the merge commit.
- **Sandbox, the published artefacts only.** `xlings subos use v693 --sandbox`, CN mirror for
  both xlings and mcpp (read back from `~/.mcpp/registry/.xlings.json`); mcpp and xlings installed
  by `xlings install` from the published index and addressed by store path:

  ```
  ok  mcpp 2026.9.26.1; xlings 2026.9.26.2; xlings runs in a directory named in CJK
  ok  mirror = CN in /home/speak/.mcpp/registry/.xlings.json
  ok  #695: app at c99, cdep at gnu11, plain at c11; the program runs
  ok  #693: a project directory with no UTF-8 spelling is refused by name
  ok  #693: a file with no UTF-8 spelling is skipped and reported; the rest builds and runs
  ok  #696: the link carries the empty graph sysroot
  ok  #696: -lm answered by openkal-musl 0.19.2; the program prints 2
  ok  #696: an unanswered -lm fails and the note names openkal-musl 0.19.2
  ok  #694: Finished release [optimized]; the compile carries -O2, not -Og
  summary: 12 ok, 0 failed, 0 skipped
  ```

- **The index, with the release.** #469's sweep builds every member with 2026.9.26.1 on macOS,
  Linux (default and four llvm shards) and Windows, all green. The openkal measurement, now over
  four targets, reads:

  | target | runs | fails | the fails |
  |---|---|---|---|
  | `x86_64-linux-gnu` | 30 | 2 | curl (`__memcpy_chk`), cmp-module (asio); as published before |
  | `x86_64-windows-musl` | 30 (+1 builds) | 1 | curl; as published before |
  | `x86_64-linux-musl` (new) | 29 | 3 | curl (`__memset_chk`), cmp-module, mimalloc (`-latomic`) |
  | `aarch64-linux-musl` (new) | 29 | 3 | curl, cmp-module, mimalloc (`-latomic`) |

  No published label went lower. mimalloc's `-latomic` is the one cell #696 makes visible:
  `-lpthread` and `-lrt` are answered by openkal-musl's archives, and `-latomic`, the compiler
  runtime's library, is answered by nothing where the host's used to answer it silently. Its
  owner is the graph's compiler runtime (mcpplibs/openkal-llvm-runtime#31), and the right answer
  depends on whether the runtime's builtins carry the `__atomic_*` fallbacks, which is measured
  there rather than assumed here.

### 12.6 Self-review, engine and ecosystem

- **Each criterion was run against the previous release.** e2e 776, 777 and 778 (leg B) fail on
  2026.9.25.1 at the criterion they name, and pass on 2026.9.26.1; the sandbox script was run
  against both releases (§12.5 and Appendix A.15).
- **Found by the review, fixed before release.** A C entry `main` bypassed the per-unit C
  standard; the cache-key rule and the test that stated the old one; the exact runtime pin that
  makes openkal-musl and openkal-llvm-runtime move as a pair; a documentation claim that macOS
  file names need not be UTF-8; the `allow_host_libs` wording (it lifts the refusal).
- **Every consumer of a changed contract.** The C standard: the three index descriptors that
  worked around the defect keep their `-D` spellings, which work under every engine the index
  admits (#469). The graph link: every openkal e2e (285 to 294, 738, 778) passes on the new
  engine, and the index measurement gains the two linux-musl rows. The xlings pin: every
  workflow moves together (`check_version_pins.sh`), and the release bundles the new xlings.
- **Still open, each for a stated reason.** `-latomic` in the openkal graph
  (openkal-llvm-runtime#31, above); W3b (cl.exe's `/std:` for a declared C standard)
  waits for a CI row that builds the index with cl.exe; W2b for PE links through the MinGW
  driver waits for the C4 inventory; W4e (wide APIs, independent of the OS version) is all or
  nothing; the xcode-27 legs wait for an LLVM whose `ld64.lld` reads `arm64e.x1` (#669).

---

## Appendix A. Measurement record

The Linux commands ran on 2026-09-25 and 2026-09-26 under the session scratchpad; the Windows
runs are dated 2026-09-25 (UTC). `$M` is
`~/.xlings/data/xpkgs/xim-x-mcpp/2026.9.25.1/bin/mcpp` (`mcpp 2026.9.25.1`).

**A.1 #694 fixture** (`optlevel`, the report's manifest and `main.cpp`):

```
$ $M build --offline --release --target x86_64-linux-musl
    Finished release [optimized] in 4.14s
compile_commands.json optimization flags: {'-Og': 1621, '(none)': 3}
[      42] [left    ] [   mid   ] [***3.142****]
```

**A.2 mcpp's release configuration** (82867ad7 worktree, unmodified manifest):

```
$ $M build --offline --release --target x86_64-linux-musl  --configure-only   # Resolved gcc@16.1.0
$ $M build --offline --release --target aarch64-linux-musl --configure-only   # Resolved gcc@16.1.0
both: every compile_commands.json entry carries -Og (307 = 178 binary units + 127 tests + 2 gtest)
```

**A.3 The premise, minimal program** (`#include <format>` and `import std` variants of the report's
format line):

```
gcc 15.1.0 x86_64/aarch64, 16.1.0 x86_64/aarch64: header mode -O2 and -O3 rc=0;
module mode (std.cc and the TU both at -O2) rc=0. No internal compiler error.
```

**A.4 The premise, mcpp itself.** A probe profile was appended to the worktree's `mcpp.toml`:

```toml
[profile.o2probe]
opt      = 2
debug    = false
cxxflags = ["-O2"]
cflags   = ["-O2"]
```

```
x86_64-linux-musl  (x86_64-linux-musl-gcc 16.1.0):  Finished o2probe in 92.23s
  127 src/ units '-Og -O2'; 48 modules/ units '-Og'; 3 cmdline units '-Og'; binary: mcpp 2026.9.25.1
aarch64-linux-musl (aarch64-linux-musl-gcc 16.1.0): Finished o2probe in 92.45s
  same distribution; qemu-aarch64 runs --version and --help
v0.0.1 (92f1335e), gcc 15.1.0-musl: 23 src/ units '-Og -O2' compile, 0 ICE;
  link: undefined reference to std::optional<std::filesystem::__cxx11::path>::optional(...&&)
82867ad7, gcc 15.1.0-musl, --release (178 units -Og): 'exposes TU-local entity' x2, 0 ICE
```

**A.5 #695 fixture** (`cdep`/`app`, host target):

```
failed: obj/mcpplibs_cdep/src/cdep.o
cdep.c:2:2: error: "strict ISO mode: this package's c_standard = \"gnu11\" did not reach this unit"
build.ninja: cflags    = -std=c11
```

**A.6 #696, driver only** (clang 22.1.8, `start.c` containing `void _start(void) {}`):

```
aarch64-unknown-linux-musl  -###: "-L/lib/../lib64" "-L/usr/lib64" "-L/lib" "-L/usr/lib"
                            -lm:  ld.lld: error: unable to find library -lm
x86_64-unknown-linux-musl   -###: "-L/usr/lib/gcc/x86_64-linux-gnu/13"
                                  "-L/usr/lib/gcc/x86_64-linux-gnu/13/../../../../lib64"
                                  "-L/lib/x86_64-linux-gnu" "-L/lib/../lib64"
                                  "-L/usr/lib/x86_64-linux-gnu" "-L/usr/lib64" "-L/lib" "-L/usr/lib"
                            -lm:  /lib/x86_64-linux-gnu/libm.a, /usr/lib/x86_64-linux-gnu/libm-2.39.a,
                                  /usr/lib/x86_64-linux-gnu/libmvec.a
--sysroot=<empty>:          no -L on either target; -lm: unable to find library -lm
--sysroot=<empty> -L<dir with empty libm.a>: -lm resolves to that file; both targets link

x86_64-w64-windows-gnu and x86_64-pc-windows-gnu, identical -L lists
(MinGW driver; host has /usr/bin/x86_64-w64-mingw32-gcc):
  lld-link ... -libpath:/usr/lib/gcc/x86_64-w64-mingw32/13-win32
               -libpath:/usr/x86_64-w64-mingw32/lib -libpath:/usr/x86_64-w64-mingw32/mingw/lib
               pe.o /usr/x86_64-w64-mingw32/lib/libm.a
  with --sysroot=<empty>: only <empty>/x86_64-w64-mingw32/lib, <empty>/x86_64-w64-mingw32/mingw/lib,
               <empty>/lib; lld: error: unable to find library -lm
```

**A.7 #696 through mcpp** (`lmcross` with `fmaximum`, x86_64-linux-musl):

```
    Finished dev [unoptimized + debuginfo] in 2.94s
2
reference	extracted	symbol
obj/main.o	/usr/lib/x86_64-linux-gnu/libm-2.39.a(s_fmaximum.o)	fmaximum
```

**A.8 Ecosystem counts** (mcpp-index 93781cf, 233 descriptors; Lua comments removed before
matching, string literals kept). 30 descriptors link at least one of the eight names that musl
answers from libc: `-lm` 13, `-lpthread` 21, `-ldl` 10, `-lrt` 6, `-lresolv` 2, and `-lcrypt`,
`-lutil`, `-lxnet` 0. `c_standard` declarations in code: 90 `c11`, 11 `c99`, 1 `c17`
(compat.ffmpeg) and 1 `gnu11` (compat.freetype). The comments of compat.libaio, compat.libdrm and
compat.libinput record `gnu11` as having no effect.

**A.9 The §4.6 sweep.** Each probe is a package with an empty `main`, `[toolchain] default =
"llvm@22.1.8"`, one dependency at its latest Linux version, and in the treatment
`[build] c_standard = "<declared>"`. The table in §4.6 is `results.tsv` from the sweep script,
and the `.ninja_log` checks are quoted there.

**A.10 #693 on Windows** (PR #697, branch `measure/693-windows-acp`; the workflow and the script
are `.github/workflows/measure-693.yml` and `.github/measure-693/measure.ps1` on that branch).

| Run | Content | Note |
|---|---|---|
| 36162457075 | Q1 and Q2 | The Q1 readings are valid. **The Q2 readings are void**: PowerShell variable names are case-insensitive, so the loop variable `$fx` overwrote the fixtures directory `$Fx`, every copy failed, and every build "ran" in 0 s with no process started. The script did not record the start error at the time. Both defects were corrected in the next run. |
| 36163057371 | Q1 with the shim inside its workspace; cdb; Q2 and Q3 on the llvm row | cdb was present at `Windows Kits\10\Debuggers\x64`. WER produced neither events nor dumps, even after it was enabled. |
| 36163850821 | the MSVC and MinGW rows; MinGW manifests | Replicates the Q1 and Q2 readings of the previous run. |
| 36164076852 | a non-ASCII `MCPP_HOME` | Replicates every earlier reading. |

Selected reading lines, verbatim:

```
READING env.acp: system ACP=1252 OEMCP=437; GetACP() in pwsh=1252
READING q1.nonacp.release: exit=0x00000000 out=mcpp 2026.9.25.1
READING q1.nonacp.xlings-shim-in-workspace: exit=0xC0000409 out=(no output)
READING q1.nonacp.xlings: exit=0xC0000409 out=(no output)
cdb:  (340.17cc): C++ EH exception - code e06d7363 (first chance)
      "No mapping for the Unicode character exists in the target multi-byte code page."
      (340.17cc): C++ EH exception - code e06d7363 (!!! second chance !!!)
READING q2.latin1.release.hello: exit=0x00000046 ... error: internal: unhandled exception:
        [json.exception.type_error.316] invalid UTF-8 byte at index 9: 0x2F
READING q2.latin1.patched.hello.build-ninja: 4345 bytes; acp=0 utf8=4;
        first 'caf' -> 63 61 66 C3 A9 5C 70 61 ...
READING q2.nonacp.release.hello: exit=0x00000046 ... error: internal: unhandled exception:
        No mapping for the Unicode character exists in the target multi-byte code page.
READING q2.nonacp.patched.hello: exit=0x00000000 ... run: exit=0x00000000 hello from a project ...
READING q2.nonacp.patched.bmn: exit=0x00000002 ... build.mcpp: cannot open
        C:\w\repro-??-??\patched-bmn\target\.build-mcpp\out/gen.cpp
READING q4.gnu-manifest.cc1plus.exe: UTF-8
READING q4.gnu-manifest.ld.exe: manifest without activeCodePage
READING q5.home-latin1.release: config exit=0x00000000; build exit=0x00000046
READING q5.home-nonacp.release: ... error: cannot create 'C:\mh-??\bin'
READING ninja: ... ninja.exe version=1.12.1 wincodepage=Build file encoding: UTF-8
```

**A.11 The -A API call sites that W4e replaces** (82867ad7):
- `CreateProcessA`: `modules/platform/src/windows/bounded_process.cppm:333,476` and
  `src/build/schedule/detach_codegen.cppm:350,402`.
- `GetEnvironmentStringsA`: `bounded_process.cppm:211`.
- `std::system`: `modules/platform/src/process.cppm:572`.
- `_putenv_s`: `modules/platform/src/env.cppm:145`.
- Every narrowing through `try_narrow`, which is `path::generic_string()`, in
  `modules/manifest/src/glob.cppm:69-75`.

**A.12 The Linux twin of F-693b**, on this host with 2026.9.25.1:

```
$ mkdir "caf"$'\xe9'; cd "caf"$'\xe9'/hello && mcpp build --offline
error: internal: unhandled exception: [json.exception.type_error.316] invalid UTF-8 byte at index 123: 0x2F
```

**A.13 Response-file encodings of the MSVC tools** (PR #697 round 5, windows-latest, code page
1252, MSVC 14.51.36231, Ninja 1.12.1 from Chocolatey). One response file per tool, directory
and encoding; `cl-include` names an include directory, `cl-source` a source file, `link-out` and
`lib-out` the output file:

```
                      ascii   caf+U+00E9            U+6D4B U+8BD5
UTF-8, no BOM         pass    fail (cl exit 2; link and lib LNK1104 'cafÃ©')   fail
UTF-8 with BOM        pass    pass                  pass
UTF-16LE with BOM     pass    pass                  pass
code page 1252        pass    pass                  (not representable)
Ninja rspfile_content, no BOM:   ascii pass, caf+U+00E9 fail, U+6D4B U+8BD5 fail
Ninja rspfile_content, BOM:      pass in all three
READING R1n.wincodepage: Build file encoding: UTF-8
```

**A.14 Where the resource compilers look for a file a statement names** (round 5b and this
host). The script is `res/<x>.rc`, the tool runs from another directory with `/I` (or `-I`)
naming the project root:

```
READING R2.rc.beside: exit=0        (1 24 "m.manifest", the file beside the script)
READING R2.rc.rooted: exit=0        (1 24 "res/m.manifest", found through /I)
READING R2.windres.beside: exit=0
READING R2.windres.rooted: exit=0
llvm-rc 20.1.7 (Linux): beside exit=0; rooted through /I exit=0; rooted without /I exit=1
```

**A.15 The sandbox script against the previous release** (`xlings subos use v693c --sandbox`,
mcpp 2026.9.25.1 installed from the index, same script as §12.5). Every criterion the release
changes fails, and the ones the previous engine already met pass:

```
ok      mcpp 2026.9.25.1; xlings 2026.9.26.2; mirror = CN
FAILED  #695 per-package C standard
FAILED  #693 project directory (the build reaches the JSON writer)
FAILED  #693 a file inside a project
FAILED  #696 no graph sysroot on the link line
ok      #696 -lm answered by openkal-musl 0.19.2 (its own -L precedes the host's)
FAILED  #696 unanswered -lm: the build succeeds, from the host's libm
ok      #694 Finished release [optimized]
FAILED  #694 -Og in compile_commands.json, under that label
summary: 6 ok, 6 failed, 0 skipped
```

