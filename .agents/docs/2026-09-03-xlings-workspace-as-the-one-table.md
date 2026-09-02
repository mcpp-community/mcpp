# `[xlings]`: one table, and `deps` retired

Date: 2026-09-03. Status: design, awaiting review. Not implemented.
Baseline: `origin/main` at `4d99864` (2026.9.2.1). Every code reference below
was read at that commit.
Relates to: #531 (provisioning reads its result), #544 (per-platform values),
and the packaging map that does not exist yet (section 7).

## 0. Summary

`[xlings] deps` and `[xlings.workspace]` state the same thing about a project —
which package, at which version — and differ only in what mcpp then does with
the statement. That difference is not a property of the declaration, and the
proposal is to stop encoding it in the key: `[xlings.workspace]` becomes the
one table, an entry means "this project uses this at this version, provision it
if absent", and `deps` is retired through a deprecation path rather than a
removal.

Two things the change is not. It is not a translation layer: `[xlings]` mirrors
xlings' own `.xlings.json` 1:1, and the merged semantics is the one xlings
already has, so mcpp is following the schema rather than inventing one. And it
does not weaken any existing behaviour: a declaration that cannot be satisfied
stays a hard build error, which is the property #531 was filed to obtain.

## 1. What the two keys are today

Facts, read at `4d99864`.

| | `deps` | `[xlings.workspace]` |
|---|---|---|
| Shape | ordered array of package references | map, name to version |
| Reference form | `name`, `name@version`, `ns:name@version` | key is a bare name |
| Readers in mcpp | five | one |
| Provisioning | `install_packages`, result read, hard error, stamped on success | none |
| Reaches `build.mcpp` | `MCPP_XPKG_<NAME>_DIR` per installed payload | nothing |
| Reaches the runner lookup | payload `bin/`, in declaration order | nothing |

The five readers of `deps`: the parser (`modules/manifest/src/toml.cppm:1410`),
the materialisation into `.mcpp/.xlings.json`
(`src/build/prepare.cppm:3196`), the provisioning pass
(`prepare.cppm:3420-3482`), `fillXpkgDirs`
(`prepare.cppm:4549`), and `BuildContext::xlingsDepBinDirs`
(`prepare.cppm:8707`, added in 2026.9.2.1). The one reader of `workspace` is
the materialisation, `prepare.cppm:3197-3198`; mcpp never acts on it.

**The general form is weaker than its own shorthand.** `docs/05-mcpp-toml.md`
§2.13 states that `[toolchain]` is "the ergonomic shorthand for the compiler"
and `[xlings.workspace]` is "the general form". The shorthand installs:
`[toolchain]`'s spec reaches `resolve_xpkg_path(pkg.target(),
/*autoInstall=*/…)` at `prepare.cppm:2212`, and the `build.mcpp` host resolve
does the same at `:3067`. The general form installs nothing. A general form
that cannot express what its shorthand does is not general.

**Nothing compares the two when both name one package.** A manifest may write
`deps = ["make@4.4"]` and `[xlings.workspace] make = "4.5"`. mcpp provisions
4.4 and writes both statements into `.xlings.json`; no code path in mcpp reads
the pair. This is the drift shape the repository has paid for repeatedly, and
merging the keys removes it by construction rather than by adding a check.

## 2. Why they are one thing

A version constraint and an installation are the same statement seen at two
moments. "This project uses cmake 3.28" is what the build environment must be;
whether cmake is already present decides whether anything has to be fetched,
and that is a fact about the machine, not about the project. xlings' own
`workspace` carries that reading, which is why the mcpp side has one reader:
there was nothing for mcpp to decide.

Keeping two keys forces every author to answer a question the manifest should
not ask — "do I want this installed, or only pinned?" — whose honest answer is
always "installed if it is not there". The one case that looks like a
counterexample, pinning a tool the project may never invoke, is not one: an
entry naming a tool the project does not use is noise regardless of the key it
is written under.

## 3. The schema after the change

```toml
[xlings.workspace]
cmake             = "3.28"
qemu-riscv        = "9.2.4-1"
"xim:picolibc-riscv" = "1.8.12"
make              = "*"
gcc               = { linux = "15.1.0" }
llvm              = { macos = "20", default = "22" }
```

Three decisions the table needs, listed for review.

**W1. `"*"` means "any version, and it must be present".** `deps` accepts an
entry with no version (`deps = ["cmake"]`), and a map keyed by name has no way
to say "no constraint" other than a value that means it. Without `"*"` every
author who does not care about a version is forced to invent one, and a pinned
version nobody chose is worse than no pin.

**W2. A key may carry a namespace prefix.** `deps` accepts `xim:name`, and the
namespace is load-bearing: `parse_xpkg_ref` splits it and `xpkg_payload`
resolves against it. If a workspace key cannot hold a colon, the namespace has
to move into the value, and the value position is already taken by the
per-platform table form. Whether xlings' workspace keys accept a colon is
question Q1 of section 11; the answer decides between the key form above and a
`{ namespace = "xim", version = "1.8.12" }` value form, which would be a second
table shape and is worse.

**W3. The per-platform value form is unchanged.** It is already accepted on
both keys (2026.9.2.1) and it survives the merge unmodified.

## 4. What mcpp writes into `.xlings.json`

This is the decision the rest depends on, and it is xlings' to make.

**Option A: mcpp writes only `workspace`.** The materialisation stops emitting
a `deps` array; every entry lands in the `workspace` object, and xlings
provisions from it. This is the shape the proposal is written to, and it holds
only if xlings provisions from `workspace`. If it does not, a project that
migrated would build on a machine where the packages happen to be installed and
fail on a clean one, which is the failure mode with the longest detection
delay.

**Option B: mcpp derives a `deps` array from `workspace` when writing.** mcpp
keeps one table in `mcpp.toml` and emits both fields. This works whatever
xlings does, and it is a translation layer of exactly the kind §2.13 refuses.
It is acceptable only as a transitional step with a stated end.

The recommendation is A, conditional on Q1 and Q2. B is the fallback and must
be labelled transitional in the code rather than left to look permanent.

## 5. What provisioning means after the merge

The provisioning pass keeps its current contract, with the input widened from
`deps` to the merged table:

- The result is read, not assumed. `xlings::call` is in the value state
  whenever the child ran, and the capability's status is inside `CallResult`;
  the check stays `!called || childRc != 0` (`prepare.cppm:3448`).
- A failure is a hard build error naming the manual command, as today.
- The stamp is written only on success, and it is keyed on the hash of the
  declared set, so the merged table changes the hash and every project
  re-provisions once after upgrading. That is correct: the declared set is a
  different set.
- `MCPP_OFFLINE` and `MCPP_NO_AUTO_INSTALL` gate the install action and not the
  whole block, as today.

**The one behaviour change is for `workspace` entries that exist now.** They
start being provisioned. The population is small and the direction is toward
the documented claim rather than away from it: an entry that was a pin becomes
a pin that is also honoured. Section 9 makes it a criterion rather than an
assumption.

## 6. Migration, and what "retired" means

`deps` is not deleted. Three phases, each with a criterion.

**Phase 1 — the merged reader.** `workspace` gains provisioning, the payload
directory hand-off (`MCPP_XPKG_*_DIR`) and the runner lookup path. `deps` keeps
working exactly as it does and is documented as deprecated. A manifest that
names one package in both, with different versions, is a hard error naming both
lines: the drift of section 1 becomes unrepresentable at the moment the second
reader appears rather than later.

**Phase 2 — the warning.** A manifest using `deps` builds and prints one
advisory naming the `[xlings.workspace]` line to write instead. The advisory is
per package, so the message is the edit.

**Phase 3 — refusal, never silence.** `deps` stops being honoured and becomes a
hard error that names the replacement. It must not become an unknown key:
`[xlings]` has no unknown-key sweep (verified — no `kKnownXlings` list exists
in `toml.cppm`), so a removed key would be read by nobody and reported by
nobody, which is the shape #531 exists to prevent. Phase 3 is gated on an index
sweep showing no published manifest still uses `deps`, and on an mcpp floor in
the packages that migrate.

**The ecosystem denominator is small.** Measured across the local `mcpplibs`
checkouts: three manifests declare `[xlings] deps`
(`aarch64-virt-rt`, `riscv-virt-rt`, `std-freestanding`), one entry each, all
of the form `xim:<name>@<version>`. mcpp's own `mcpp.toml` declares no
`[xlings]` section. The index has to be swept before Phase 3; the local
denominator is not the ecosystem.

## 7. The packaging map, and a loss the current implementation has

The reason a `deps`-shaped list exists in the first place is that a published
package's install-time edge lives in the descriptor, as
`xpm.<platform>.deps`. That mapping does not exist in mcpp:
`src/publish/xpkg_emit.cppm` mentions neither `xlings` nor `deps`, and nothing
in `src/pack` or `src/publish` emits a platform `deps` table. A package that
declares `[xlings] deps` today gets a descriptor without it, and the edge is
written by hand — which is why `riscv-virt-rt` carries a thirty-line comment
about the release where the hand-written edge was removed and the C library
stopped being installed.

**The per-platform resolution shipped in 2026.9.2.1 is lossy for this path.**
`XlingsConfig::deps` and `::workspace` hold values already resolved for the
running host (`modules/manifest/src/types.cppm`, and `resolve_host_value` in
`toml.cppm`), and the unresolved entries are discarded. An emitter needs all
platforms at once: `xpm.linux.deps` and `xpm.windows.deps` are two tables, and
a manifest loaded on Linux can no longer produce the second. Packing on macOS
would emit a descriptor missing the Linux edge, and nothing would say so.

The fix is additive and belongs with this work because the merged table
inherits the same loss:

```cpp
struct XlingsConfig {
    std::map<std::string, std::string> workspace;   // resolved for THIS host
    // The declaration as written, per platform, for consumers that are not
    // this host: the descriptor emitter needs every platform's entries at
    // once. The build path never reads this.
    std::map<std::string, std::map<std::string, std::string>> workspaceByPlatform;
};
```

`resolve_host_value` already knows which platform each key belongs to; keeping
a second copy costs one insertion. The criterion is section 9's C4: packing on
one host emits every platform's edge, and it fails today because the emitter
does not exist.

## 8. Axes

**Structure.** One declaration site for "what this project's environment
contains", one reader set, one provisioning pass. The count of things that can
disagree about a package's version drops from two to zero.

**Compatibility.** Phase 1 adds no manifest key and removes none, so a manifest
written for it loads on an older mcpp; there, a `workspace` entry is a pin that
installs nothing, which is what it means today. The reverse direction —
an older manifest on a newer mcpp — is unchanged through Phase 2.

**Upgrading.** The provisioning stamp is keyed on the declared set, so the
first build after the merge re-provisions once per project and then behaves as
before. No cache is invalidated and no output path changes.

**Consistency.** The general form gains what its shorthand already does. The
`[toolchain]`/`[xlings.workspace]` relationship stated in §2.13 becomes true
rather than aspirational.

**Cross-platform.** The per-platform value form is unchanged; section 7 makes
it survive to the one consumer that needs the unresolved form.

**What a person sees.** One table instead of two, and one question fewer to
answer when writing it. Every failure keeps naming the package and the manual
command.

## 9. Test criteria

Each must be observed failing before the corresponding change.

| # | Criterion | Note |
|---|---|---|
| C1 | A `[xlings.workspace]` entry for a package that is not installed provisions it, and its payload directory reaches `build.mcpp` as `MCPP_XPKG_<NAME>_DIR` | Assert on the value the program read, not on a log line |
| C2 | A `workspace` entry that cannot be provisioned fails the build with the manual command in the message | The existing `deps` diagnostic, reached from the new input |
| C3 | One package named in both `deps` and `workspace` with different versions is a hard error naming both lines | Must be seen to fail on a manifest that today builds and silently provisions the `deps` version |
| C4 | Packing a project whose table has per-platform entries emits `xpm.<platform>.deps` for every platform, from any host | Fails today because the emitter does not exist; the assertion is on the emitted descriptor, not on the manifest |
| C5 | A `workspace` entry with `"*"` provisions the package and pins nothing | Both halves; a test that only checks the install cannot tell a wildcard from a version |
| C6 | A migrated `riscv-virt-rt` resolves its emulator on a clean machine | The ecosystem case, run in a sandbox, because "installed already" is the state that hides this |
| C7 | The second build of an unchanged project provisions nothing and prints nothing | The stamp; and the whole-project fast path never reaches this code, so the test must touch a source first |

C3 and C4 are the two that fail on the current engine. C7 is stated because
this repository has read that measurement wrongly twice.

## 10. Implementation surface

- `modules/manifest/src/toml.cppm`: `workspace` gains the reference forms
  `deps` accepts (namespace prefix, `"*"`); `workspaceByPlatform` retained;
  the `deps`/`workspace` conflict check; the Phase 2 advisory.
- `modules/manifest/src/types.cppm`: `XlingsConfig` fields and their comments.
- `src/build/prepare.cppm`: the provisioning pass, `fillXpkgDirs` and
  `xlingsDepBinDirs` read the merged table; the materialisation emits what
  section 4 decides.
- `src/xlings/xlings.cppm`: `ProjectEnv` and `seed_xlings_json`, per section 4.
- `src/publish/xpkg_emit.cppm`: the `xpm.<platform>.deps` map (new).
- `docs/05-mcpp-toml.md` §2.13, `docs/13-baremetal.md`, `docs/17`, and each
  `docs/zh/` twin, which CI enforces.
- `tests/unit/test_manifest.cpp`, a new e2e for C1/C2/C3, and the packing
  criterion C4.
- Ecosystem: `aarch64-virt-rt`, `riscv-virt-rt`, `std-freestanding` migrate
  after Phase 1 ships, each with an mcpp floor.

## 11. Open questions

1. **Does xlings provision from `workspace`?** Section 4 depends on it. If it
   does not, the answer decides between adding it there and Option B here.
2. **Do xlings' workspace keys accept a namespace prefix (`xim:name`)?** W2
   depends on it. A workspace key becomes a shim name in xvm, which is the
   reason to doubt it.
3. **Is `"*"` already spelled something else in xlings?** W1 should take the
   existing spelling rather than introduce one.
4. **Does a workspace entry bind the package or one program?** The
   `qemu-riscv` descriptor adds an umbrella node for the package name beside
   the two program nodes, so both are addressable; whether binding the package
   determines its programs' versions is the property the merged table relies on
   when a package ships several programs.
5. **Phase 3's floor.** Which mcpp version the migrating packages declare, and
   whether the index sweep is a release gate or a one-off.
