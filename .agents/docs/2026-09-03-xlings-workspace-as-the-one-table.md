# `[xlings]` is mcpp's surface for xlings' local project mechanism

Date: 2026-09-03. Status: design, awaiting review. Not implemented.
Baseline: `origin/main` at `4d99864` (2026.9.2.1), and the xlings working tree
at `openxlings/xlings`. Every code reference was read at those trees.
Relates to: #531 (provisioning reads its result), #544 (per-platform values).

This document was rewritten after the review discussion rather than amended.
Section 15 records what the discussion changed, so that positions this document
no longer holds are still findable.

## 0. Summary

`[xlings]` is not a section of mcpp's own invention. It is mcpp's manifest
surface for **xlings' local project mechanism**: the project `.xlings.json`
that gives a directory its own environment. Every question below has the same
form — what does that mechanism already do, and is mcpp asking it correctly.

Three changes follow, and one of them is a defect rather than a design.

1. **`workspace` becomes the one table**; `deps` is retired from the manifest.
   The two state the same thing and differ only in what mcpp then does with the
   statement, and the mechanism they map onto has one authored surface.
2. ~~Provisioning moves into project scope.~~ **Withdrawn during
   implementation** (§3): the call site records a measurement showing that
   project scope puts payloads in a SubOS the compiler's `--sysroot` does not
   name. The scope stays global; what the declaration decides is unchanged,
   because resolution reads the project's `workspace` layer either way.
3. **`envs` is retired.** It has no reader in xlings and none in mcpp, and
   `docs/05` documents an effect it does not have.

Nothing here requires a new mechanism on either side. The manifest keeps its
correspondence with the file, and the file keeps both of its package-shaped
fields, because xlings reads them in two different places.

## 1. The mechanism mcpp is speaking to

xlings gives a directory its own environment when it finds a project
`.xlings.json`:

| Key | What xlings does with it |
|---|---|
| `deps` | the array a bare `xlings install` installs (`src/core/cmdprocessor.cpp:163-196`) |
| `workspace` | a version-resolution layer merged into the project's effective pins (`config.cpp:660`, `:846`) |
| `subos` | names the project's environment; absent means an anonymous one (`config.cpp:273-283`) |
| `index_repos`, `mirror`, `lang` | project overrides of the machine's settings |

**Discovery.** xlings walks the current directory upward for a `.xlings.json`,
stops at any directory that also holds a `subos/` — that signature is an xlings
home, never a project — and otherwise falls back to `XLINGS_PROJECT_DIR`
(`config.cpp:765-799`). mcpp writes `<project>/.mcpp/.xlings.json`, one level
below what a walk from the project root inspects, so the file is reached
through the environment variable.

**Resolution layers.** `merge_workspace_into_` assigns rather than inserts, so
in `merged_workspace` (`config.cpp:846-864`) the later layer wins:

| Project mode | Layers, in merge order | Effect |
|---|---|---|
| **Anonymous** — a project file, no `subos` | global, project manifest, project SubOS | the machine's environment with the project's entries laid over it |
| **Named** — `subos = "<name>"` | project manifest, project SubOS | an isolated space; the machine's layer is not merged |
| no project file | global | — |

The developer chooses the strength by naming a SubOS or not. Both rows are the
mechanism working as specified; neither is an omission.

**Where a project's SubOS lives.** Under the project, always
(`config.cpp:348-353`): `<projectDir>/.xlings/subos/<name>` when named,
`<projectDir>/.xlings/subos/_` when anonymous. For mcpp the project directory
is `<project>/.mcpp`.

**Installing an existing payload maps it rather than fetching it.** The
installer checks that the payload exists and is registered to the package, sets
`payloadInstalled` and skips the install hook
(`src/core/xim/installer.cpp:2740-2775`).

**Activation is automatic when nothing is active for that name**
(`activate_requested_targets`, same file):

```cpp
auto active = xvm::get_active_version(Config::effective_workspace(), match.name);
if ((active.empty() || useAfterInstall) && has_version(db, match.name, match.version))
    cmd_use(match.name, match.version, stream);
else if (!active.empty() && active != match.version)
    // declining to switch is a decision, and it used to be a silent one
```

`cmd_use` resolves the whole binding group through `resolve_binding_selection`
and writes one workspace entry **per member** (`xvm/commands.cpp:763-772`),
which is why using either a package root or one of its programs selects the
same release, and why a root and its programs carry one version.

## 2. Change one: `workspace` is the one table

### 2.1 Why the split cannot be defended

**The general form is weaker than its own shorthand.** `docs/05` §2.13 states
that `[toolchain]` is "the ergonomic shorthand for the compiler" and
`[xlings.workspace]` is "the general form". The shorthand installs
(`resolve_xpkg_path(…, autoInstall=…)`, `prepare.cppm:2212` and `:3067`); the
general form installs nothing.

**Nothing compares the two when both name one package.** `deps = ["make@4.4"]`
beside `workspace.make = "4.5"` provisions 4.4, writes both into the file, and
no code path in mcpp reads the pair.

**The authored surface of the mechanism is `workspace`.** A published example
is `mcpp-community/d2mcpp/.xlings.json`:

```json
{
  "workspace": {
    "d2x": "2026.08.02.2",
    "mdbook": "0.4.43",
    "code": "",
    "mcpp": { "linux": "2026.8.2.1", "macosx": "2026.8.2.1", "windows": "2026.8.2.1" }
  }
}
```

### 2.2 The schema

```toml
[xlings.workspace]
cmake                   = "3.28"
code                    = ""                     # present; version unconstrained
picolibc-riscv          = "xim:1.8.12"           # namespace on the version
"xim:qemu-user-aarch64" = "7.2.0"                # or on the key - quotes REQUIRED
gcc                     = { linux = "15.1.0" }
llvm                    = { macosx = "20", default = "22" }
```

**W1. "Version unconstrained" is the empty string.** `"code": ""` is already in
use above. The resolver returns it unchanged (`src/core/xvm/db.cppm:383`) and an
empty value reads downstream as claiming no version (`Config::version_origin`'s
`claims`, `config.cpp:1108-1112`). It maps to an install target with no
`@version`, which is what `deps = ["cmake"]` means today. No `*` and no
`latest` is introduced: both are input spellings, resolved before anything is
stored.

**W2. The namespace may ride either half, and mcpp normalises.** Four rules
keep that one fact rather than two:

1. mcpp materialises the file's own convention only — bare target as the key,
   scope on the version — so the file never holds two spellings.
2. The install address is assembled from the pair, whichever half carried it.
3. Stating it twice and differently (`"xim:foo" = "other:1.0"`) is a hard error
   naming both halves, as is naming one package under both spellings.
4. The key form needs quotes. TOML bare keys are `[A-Za-z0-9_-]` and mcpp's
   lexer matches (`modules/libs/src/toml.cppm:150`); measured on the 2026.9.2.1
   binary, `xim:picolibc-riscv = "1.8.12"` fails with
   `mcpp.toml:6:4: error: expected …`, which says nothing about namespaces, so
   the documentation shows the quotes.

**W3. The per-platform value form is xlings' own.** Native keys are `linux`,
`windows`, `macosx` and `default` (`platform::OS_NAME`, resolved by
`resolve_platform_workspace_value_`). mcpp additionally accepts `macos`, a
superset that stays; the documentation shows `macosx` as the aligned spelling.

### 2.3 What mcpp writes into the file

One authored entry materialises as one `deps` element **and** one `workspace`
member. That is not a translation layer: the file's two fields have two
consumers in xlings — `deps` for a bare `xlings install`, `workspace` for
resolution — and neither subsumes the other. Writing only `workspace` would
leave a project that builds where the packages happen to be installed and fails
on a clean machine.

If xlings later installs from `workspace`, the `deps` half of the emission can
be dropped without touching any manifest.

### 2.4 Migration: one release

Three manifests declare `[xlings] deps` — `aarch64-virt-rt`, `riscv-virt-rt`,
`std-freestanding`, one entry each of the form `xim:<name>@<version>` — and
mcpp's own `mcpp.toml` declares no `[xlings]` section. A deprecation window
buys nothing at that size.

**Corrected during implementation (2026-09-03): step 1 refuses in the root
manifest and advises in a dependency's.** The denominator argument counts
manifests that declare the key, and it is the wrong denominator: those three
packages are *consumed*, and a consumer pins an exact version. A refusal that
reached a dependency's manifest would make `riscv-virt-rt@0.6.0` unbuildable on
the new engine for everyone who pinned it, and no republished version reaches
them until each consumer re-pins. That is the shape this repository has already
paid for once — a consumer must ship before the thing it depends on moves.

The asymmetry is not a hedge. A root manifest is the author's own file and they
can fix it in the same minute they read the message; a dependency's manifest is
not theirs to edit, and refusing it punishes the wrong person.

1. The merged reader ships. `[xlings] deps` in the **root** manifest is a hard
   error naming the line to write; in a **dependency's** manifest it is
   honoured and reported once, naming the package.
2. The three packages are republished with `[xlings.workspace]` — **not in
   this cycle, and the reason is a silent regression rather than caution.** An
   older mcpp parses `[xlings.workspace]` perfectly well and provisions nothing
   from it, so a package that migrated before its consumers moved would stop
   installing its emulator on every older engine, with no message anywhere.
   That is worse than the advisory it would silence. The migration is safe once
   the index's `latest` mcpp provisions from the table, and a floor in the
   package makes the older engine refuse instead of degrade.
3. An index sweep confirms no other published manifest declares it. When the
   advisory has been silent across a release, the dependency path refuses too.

What this cycle does instead is verify: the three packages build on the new
engine, produce the advisory, and provision exactly as before.

**A limit of the advisory, stated rather than implied.** It rides
`schemaWarnings`, which `prepare` prints for the ROOT manifest and escalates
under `--strict`. A dependency's schema warnings are attached and not printed
today — a pre-existing gap in how mcpp surfaces them, not one this change
introduces. So a consumer of an unmigrated package is not told; the package's
own author is, the moment they build it. Surfacing dependency schema warnings
is worth doing and is a change of its own, because it would also surface every
unrelated warning those manifests carry.

**What does not bend:** `deps` is honoured or refused with a message, never
dropped in silence. `[xlings]` has no unknown-key sweep — no `kKnownXlings` list exists in
`toml.cppm` — so a removed key would be read by nobody and reported by nobody,
which is the shape #531 exists to prevent.

## 3. Change two, withdrawn during implementation: the scope stays global

The design proposed moving the provisioning call from `make_xlings_env` to
`make_project_xlings_env`, on the reasoning that the install should write where
the shim reads. Implementing it turned up a comment at the call site recording
that this was tried and measured:

> GLOBAL scope, and the scope is the whole point. The obvious alternative —
> `install_packages` against `make_project_xlings_env` — installs at PROJECT
> scope, and that measurably does not work: on a fresh `MCPP_HOME` the headers
> land in `<proj>/.mcpp/.xlings/subos/_/usr/include` while `--sysroot` names
> `<MCPP_HOME>/registry/subos/default`, so `#include <gbm.h>` still failed with
> the dependency installed and declared. Two SubOS views, and the payload in
> the one the compiler does not read.

The premise the design rested on — that the install destination is chosen by
package scope rather than by transport — is contradicted by that measurement.
**Change two is withdrawn.** The call keeps `make_xlings_env`.

### 3.1 What that leaves true, and what it leaves unsolved

**The declaration still wins at resolution.** mcpp materialises
`[xlings.workspace]` into the project file's `workspace` object, and that is a
layer `merged_workspace` applies over the machine's (§1). So a project
declaring a version resolves to it inside the project regardless of where the
payload was installed. Provisioning only has to make the payload exist, which
global scope does.

**Two views still disagree about the installed set.** A payload installed into
the registry is not in the project SubOS's `installed[]`, so a shim invoked in
the project can resolve the declared version and still report
`… is not installed in this subos (_)`. That is the line the 2026.9.2.1
verification recorded, and the reason the runner resolves a program through the
declared payload's `bin/` before consulting `PATH`.

That mismatch is real and is not addressed here. It is a question about which
environment mcpp's own `--sysroot` names — the registry SubOS today — and
answering it means changing where mcpp points the compiler, not where it points
an install. That is a larger change than this document, and the runner lookup
is a working compensation for its user-visible half.

**What is not done, and why it is not a gap in this change:** nothing in the
merged table depends on the scope. `deps` and `workspace` are two projections
of one entry either way, and the entry provisions and resolves exactly as it
did before.

## 4. Change three: `envs` is retired

Every `envs` consumer in xlings is one of two structures, and neither is the
flat object mcpp writes:

1. `xvm`'s `VData::envs` — variables attached to **one program's** shim, stored
   in the version database (`xvm/db.cpp:724`, `xvm/shim.cpp:337`), set through
   `xvm add --env`.
2. A SubOS's `subos_info.envs` — an object of **provider sections** keyed by
   binding (`subos/manifest.cpp:197-290`).

Searching the source for `contains("envs")` and `["envs"]` outside those files
and the doctor that checks them returns nothing. mcpp's own run environment
comes from `plan.runtimeBinding` (`execute.cppm:418`), never from
`[xlings.envs]`.

So the key is written by mcpp and read by nobody, while `docs/05` §2.13 calls
it "env vars applied to the tool environment". It is retired on the path of
§2.4, and **the documentation correction ships first and independently**: a
sentence stating an effect that does not occur is the more urgent half.

## 5. The packaging map, and the loss 2026.9.2.1 introduced for it

A published package's install-time edge lives in its descriptor as
`xpm.<platform>.deps`. mcpp does not emit it: `src/publish/xpkg_emit.cppm`
mentions neither `xlings` nor `deps`, and nothing in `src/pack` or
`src/publish` writes a platform `deps` table. That is why `riscv-virt-rt`
carries a thirty-line comment about the release where the hand-written edge was
removed and the C library stopped being installed.

**The per-platform resolution shipped in 2026.9.2.1 is lossy for that path.**
`XlingsConfig` holds values already resolved for the running host, and the
unresolved entries are discarded. An emitter needs every platform at once, so a
manifest loaded on Linux cannot produce `xpm.windows.deps`, and packing on
macOS would emit a descriptor missing the Linux edge with nothing said.

The fix is additive, and the merged table inherits the same loss:

```cpp
struct XlingsConfig {
    std::map<std::string, std::string> workspace;   // resolved for THIS host
    // The declaration as written, per platform, for the descriptor emitter,
    // which needs every platform at once. The build path never reads this.
    std::map<std::string, std::map<std::string, std::string>> workspaceByPlatform;
};
```

`resolve_host_value` already knows which platform each value belongs to.

A package still on `deps` emits no edge, as before. Its declaration is
host-resolved at load, so the per-platform information a descriptor needs is
gone by the time the emitter runs, and writing the host's answer into all three
blocks would be the machine-dependent descriptor this section exists to
prevent. The advisory tells its author how to obtain one.

## 6. Documentation

**`docs/05` §2.13** gains the inheritance rule as a table, next to
`[xlings.workspace]`:

| The project declares | The version of a tool it did not name comes from |
|---|---|
| `[xlings.workspace]`, no `subos` | the machine's global workspace; the project's own entries win over it |
| `[xlings.workspace]` and `subos = "<name>"` | that SubOS's own workspace; the global one does not apply |
| neither | the machine's global workspace |

with one sentence for why the middle row is not an omission, the two namespace
spellings **with the quotes shown**, `""` for an unconstrained version, and
`macosx` as the aligned platform key.

**`docs/17`** gains the consequence where `subos` is chosen: declaring a SubOS
changes which pins apply, and a project that relied on the machine's tools has
to declare them once it names an environment. It also gains §3.4's first
paragraph.

**`docs/05` §2.13's `envs` sentence is corrected** (§4), independently and
first.

`docs/13` needs no change: it declares packages, not versions. Each file with
its `docs/zh/` twin, which CI enforces.

## 7. Axes

**Structure.** One declaration site for what the project's environment
contains, one reader set, one provisioning pass — and the pass now runs in the
scope its effects are read from. The count of things that can disagree about a
package's version drops from two to zero.

**Stability.** Every decision is a value the project wrote or an answer xlings
gave. The scope fix removes a shared mutable workspace from the path, which is
the class of state two projects can fight over.

**Compatibility.** No manifest key is added. A manifest written for this loads
on an older mcpp, where a `workspace` entry is a pin that installs nothing —
what it means there today.

**Upgrading.** The stamp is keyed on the declared set, so the first build after
the change re-provisions once per project and then behaves as before. No cache
is invalidated and no output path changes.

**Consistency.** The general form gains what its shorthand already does, and
§2.13's claim about `[toolchain]` becomes true rather than aspirational.

**Cross-platform.** The per-platform form is xlings' own and is unchanged; §5
makes it survive to the one consumer that needs it unresolved.

**What a person sees.** One table instead of two, one question fewer when
writing it, and a declared version that is honoured on a machine that had
another one active.

## 8. Test criteria

Each must be observed failing before the corresponding change.

| # | Criterion | Note |
|---|---|---|
| C1 | A `workspace` entry for a package that is not installed provisions it, and its payload directory reaches `build.mcpp` as `MCPP_XPKG_<NAME>_DIR` | assert on the value the program read |
| C2 | A `workspace` entry that cannot be provisioned fails the build with the manual command in the message | the existing diagnostic, reached from the new input |
| C3 | One package named in both `deps` and `workspace` is refused naming both lines | must be seen failing on a manifest that today builds and silently provisions the `deps` version |
| C4 | Packing a project whose table has per-platform entries emits `xpm.<platform>.deps` for every platform, from any host | fails today: no emitter |
| C5 | An entry with `""` provisions the package and pins nothing | both halves |
| C6 | A project declaring a version different from the machine's active one builds and runs against the declared version, in Anonymous mode | fails today: the scope |
| C7 | The machine's global workspace file is unchanged after C6 | fails today: the scope |
| C8 | Two checkouts declaring different versions of one tool each get their own, in one session | fails today: the scope |
| C9 | A Named SubOS project resolves what it declared plus what its environment holds; a tool it did not declare fails naming that environment | makes the developer's choice visible |
| C10 | A declared version never installed fails at the shim with the "version this project asks for" wording | not a bare "not found" |
| C11 | The second build of an unchanged project provisions nothing and prints nothing | the stamp; the whole-project fast path never reaches this code, so touch a source first |
| C12 | A migrated `riscv-virt-rt` resolves its emulator on a clean machine | the ecosystem case, in a sandbox |
| C13 | `<project>/.mcpp` is loaded by xlings as a project directory, not skipped as an xlings home | §3.2's precondition; assert on an effect, since a skip degrades to the current behaviour and would otherwise look like success |

C3, C4, C6, C7 and C8 fail on the current engine. C11 is stated because this
repository has read that measurement wrongly twice.

## 9. Implementation surface

- `modules/manifest/src/toml.cppm`: `workspace` accepts the reference forms
  `deps` accepts (namespace on either half, `""`); `workspaceByPlatform`
  retained; the `deps` refusal; the duplicate-namespace error.
- `modules/manifest/src/types.cppm`: `XlingsConfig` fields and comments.
- `src/build/prepare.cppm`: the provisioning pass reads the merged table and
  calls with `make_project_xlings_env`; `fillXpkgDirs` and `xlingsDepBinDirs`
  read the merged table; the materialisation emits both file fields.
- `src/xlings/xlings.cppm`: `ProjectEnv` and `seed_xlings_json`.
- `src/publish/xpkg_emit.cppm`: the `xpm.<platform>.deps` map (new).
- `docs/05` §2.13, `docs/17`, and each `docs/zh/` twin.
- `tests/unit/test_manifest.cpp`; new e2e for C1, C2, C3, C5, C6, C7, C8, C9;
  the packing criterion C4.
- Ecosystem: `aarch64-virt-rt`, `riscv-virt-rt`, `std-freestanding` migrate
  after step 1, each with an mcpp floor.

## 10. Rejected and withdrawn

**Writing only `workspace` into the file.** xlings does not install from it
(§1); the project would build where the packages happen to be installed.

**`useAfterInstall: true`.** Not needed once the scope is right (§3.2), and it
cannot be verified: a forced switch that fails is a `log::warn` inside a call
that exits zero.

**An explicit `use_version` per entry.** Same reason. It was proposed to obtain
the exit code the forced install discards; with the merge doing the work there
is nothing to switch.

**A three-phase deprecation for `deps`.** The denominator is three manifests
(§2.4).

**Wiring `envs`.** Neither xlings structure is what a consuming project should
write (§4).

**A namespace-only key, or a namespace-only value.** Both are accepted, because
neither is more natural and the two are mechanically interconvertible; what is
refused is holding both at once with different values (§2.2 W2).

## 11. Open questions

None blocking. Two items are scheduling rather than design: which mcpp floor
the three migrating packages declare, and whether the index sweep of §2.4 is a
release gate or a one-off.

## 12. Appendix: measurements

| Claim | How it was checked |
|---|---|
| A workspace key is an xvm target of any kind | the development host's default SubOS: `binutils`/`ar`/`as`/`ld` all 2.42, `gcc`/`g++`/`cc` all 16.1.0, plus `crt1.o` and `glibc.files.N`; 546 entries |
| A package root and its programs carry one version | same, and `cmd_use` writes one entry per binding-group member |
| No key carries a namespace | 1635 version-database targets and 546 workspace entries, zero colons; the authored `d2mcpp` file likewise |
| A namespace does appear on a version | `"mcpp": {"active": "xim:2026.8.30.2", …}`, one target holding scoped and unscoped versions at once |
| `""` is the unconstrained spelling | `d2mcpp/.xlings.json`, and `claims` treating an empty value as no claim |
| The unquoted `ns:name` key is a TOML error | run on the 2026.9.2.1 binary: `mcpp.toml:6:4: error: expected …` |
| Provisioning runs in global scope | `make_xlings_env` has no `projectDir`; the file mcpp writes is one level below the walk |
| `envs` has no reader | exhaustive search of the xlings source |

## 13. Two artifacts share the name `.xlings.json`

They must not be measured for each other, and an earlier draft of this document
did so.

**The authored project file** is what a person writes and what mcpp
materialises: `workspace` maps a name to a version string or to a
platform-conditional object.

**A SubOS state file** is what `cmd_use` writes: the same key space, but each
value is an `{active, installed[]}` record and the keys are every member of
every release ever switched to.

The parser accepts both shapes and disambiguates by reserved keys — an `active`
or `installed` key marks the state form (`xvm/db.cppm:405-420`). One schema,
two stages; the project form is the one `[xlings]` mirrors.

## 14. `subos`, unchanged by this proposal and stated for completeness

- **Absent** selects mcpp's initialised `McppDefault`; **`subos = "default"`**
  is an explicit `NamedSubos("default")`. `subosDeclared` carries the
  difference, because a string alone cannot.
- The name is validated as a portable identifier; anything else is a manifest
  error naming the value.
- There is **no CLI or environment override**, and no implicit following of
  xlings' active SubOS.
- A named SubOS that does not exist is a **hard error**, never a fallback.
  Creating one is xlings' layer; mcpp reads an environment and never creates
  one.
- An environment with no `subos_info` **degrades**: the runtime binding reports
  inconclusive, a note is printed, the build continues.
- On Linux the selection also fixes the loader and C library contract.
- Only a **declared** SubOS puts its `bin/` at the front of `build.mcpp`'s
  `PATH` (`prepare.cppm:1389`).

## 15. What the review changed

Recorded because a position that was held and abandoned is part of how this was
reached, and because two of these were wrong in a way worth remembering.

| Held | Replaced by | Why |
|---|---|---|
| mcpp writes only `workspace` into the file | both fields (§2.3) | xlings installs from `deps` only |
| A manifest `workspace` entry cannot pin a package's programs | it can (§1) | expansion happens in `cmd_use`, which provisioning triggers; the merge was never where it happens |
| Activation lands in the project's own layer, so forcing is contained | it lands in the registry's global workspace (§3.1) | `make_xlings_env` carries no `projectDir` |
| `useAfterInstall: true`, then an explicit `use_version` per entry | neither (§3.2) | the merge already makes the declaration authoritative once the scope is right |
| `"*"` for an unconstrained version | `""` (§2.2 W1) | the authored form already exists and is in use |
| The namespace must go on the version; a colon in a key is an error | either half is accepted (§2.2 W2) | both are interconvertible; the error is holding two that disagree |
| A three-phase deprecation | one release (§2.4) | three manifests |
| Keys are program names | keys are xvm targets, roots included (§12) | measured |

### 15.1 What implementation found that the design did not

Four things, and three of them were only visible once the code ran.

**A nested `std::map` in an exported module truncates the BMI.** `XlingsConfig`
first carried `map<string, map<string, string>>`. The module compiled and the
BMI it wrote was unreadable: consumers failed with `Bad file data` and
`failed to load pendings for 'std::map'`, pointing at an unrelated file's
ordinary `std::map` alias. The emitter needs the addresses and never the inner
keys, so a `vector` costs nothing and the field is one. xlings' own source
carries a note about the same GCC 16 shape, with a different workaround.

**The removed key's first casualties were this repository's own fixtures.**
e2e 88 and 205 declared `[xlings.envs]` and went red on CI, not locally,
because the local run had not reached them. That is the expected shape of
removing a key that did nothing: the things that used it were the things that
did not depend on it working.

**One assertion could only fail on macOS, and did.**
`Manifest.XlingsWorkspaceAcceptsPerPlatformValues` compared
`host_platform_key()` against `"macos"`, and aligning that function with
xlings' `macosx` made the comparison false on exactly one of the three hosts.
The test was written in terms of the function so it would run everywhere; the
literal on the other side of the comparison is what defeated that.

**`deps` and `workspace` agreeing is not two entries.** Both feed the same
derived list, so the same statement written twice asked xlings to install one
package twice. It collapses, and only the advisory is produced.

The two that mattered: reading a SubOS **state** file as though it were an
authored **project** file, and asserting a blast radius without checking which
environment the call runs in. Both were arguments from the shape of the code
rather than from what it does.

## 16. Implementation plan

Eight tasks. T1 is the only one everything else waits on; T2, T3 and T4 are
independent of each other; T7 runs after the release.

```
T1 manifest ──┬── T2 provisioning scope ──┐
              ├── T3 xlings module        ├── T5 docs ── T6 tests ── T8 release
              └── T4 descriptor emitter ──┘                              │
                                                          T7 ecosystem ──┘
```

| # | Task | Files | Depends on |
|---|---|---|---|
| T1 | The merged table: parse, both namespace positions, `""`, per-platform, `workspaceByPlatform`, `deps` root-refusal and dependency-advisory, `envs` refusal | `modules/manifest/src/{toml,types}.cppm` | — |
| T2 | Provisioning in project scope; the materialisation feeds both file fields | `src/build/prepare.cppm` | T1 |
| T3 | `ProjectEnv` and `seed_xlings_json` lose `envs` | `src/xlings/xlings.cppm` | T1 |
| T4 | `xpm.<platform>.deps` in the emitted descriptor | `src/pm/publisher.cppm` | T1 |
| T5 | `docs/05` §2.13, `docs/17`, both `docs/zh/` twins | docs | T1-T4 |
| T6 | Unit tests and one e2e | `tests/` | T1-T4 |
| T7 | The three packages are verified on the new engine; the republish waits for the floor (§2.4) | `mcpplibs/{aarch64-virt-rt,riscv-virt-rt,std-freestanding}` | T8 |
| T8 | Version, CI, self-review, merge, release, sandbox | — | T5, T6 |

### 16.1 What each axis demands of the implementation

**Architecture.** One authored key, one derived list, one provisioning pass in
one scope. `XlingsConfig::deps` becomes the derived install addresses, so every
existing reader — the pass, `fillXpkgDirs`, `xlingsDepBinDirs`, the file's
`deps` array — keeps reading the field it reads today and none of them learns
about namespaces.

**Stability.** The scope change moves writes off a workspace shared by every
project on the machine. Nothing else about the pass changes: its result check,
its stamp, its offline gates.

**Simplicity.** No new mechanism on either side, and the diff is smaller than
the design: the parse produces two projections and everything downstream is
untouched.

**User experience.** A key that never worked stops being documented as working;
a key that did two jobs becomes one; and a version a project declares is the
one that runs.

**Compatibility.** No manifest key is added. `[xlings] deps` keeps working in a
dependency and is reported once. `[xlings.envs]` is refused, and no manifest in
the ecosystem uses it — measured across the local checkouts.

**Cross-platform.** `workspaceByPlatform` keeps the unresolved declaration, so
the descriptor a Linux host emits carries the Windows edge.

**Consistency.** The platform vocabulary becomes xlings' own (`macosx` shown,
`macos` still accepted), and the namespace may be written in either position
because both are already spellings the ecosystem uses.

**Upgrading without noticing.** The stamp key changes with the declared set, so
one re-provision per project and then nothing. No cache is invalidated, no
output path moves, and a manifest written for this loads on an older mcpp.

**Test coverage.** Five of the twelve criteria fail on the current engine and
are the ones that prove the change; C11 exists because this repository has
twice measured a fast path instead of the thing under test.
