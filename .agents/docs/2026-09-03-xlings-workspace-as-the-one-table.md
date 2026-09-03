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

The merge is in `mcpp.toml`, not in the file mcpp writes. `.xlings.json` keeps
both fields, because xlings reads them in two different places and neither
subsumes the other (section 13). One authored entry therefore materialises as
one `deps` element and one `workspace` member: not a translation, but one
statement written where each of its two halves is read.

The change does not weaken any existing behaviour: a declaration that cannot be
satisfied stays a hard build error, which is the property #531 was filed to
obtain.

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
cmake          = "3.28"
qemu-riscv     = "9.2.4-1"
picolibc-riscv = "xim:1.8.12"
make           = "latest"
gcc            = { linux = "15.1.0" }
llvm           = { macos = "20", default = "22" }
```

Three decisions the table needs, listed for review.

**W1. There is no `"*"`, and none should be invented.** xlings already has two
spellings for "not an exact version": a version **prefix**, which
`match_version` resolves to the highest match (`src/core/xvm/db.cpp:411` — `22`
selects `22.1.8`), and `latest`, which `cmd_use` resolves to the highest
installed version before writing (`commands.cpp:592-607`). Both are **input**
spellings: what lands in a workspace file is always a concrete version, which
is why a stored `latest` would fail at shim time rather than mean anything. The
manifest therefore accepts a version, a prefix, or `latest`, and "must exist,
version unconstrained" is spelled `latest`. Measured on three real subos files
on the development host: every stored value is concrete.

**W2. The namespace goes on the version, not on the key — and the key form was
checked rather than assumed.** Measured on the development host: 1635 targets
in the version database and 546 workspace entries in the default SubOS, and
**not one key contains a colon**. The colon appears on the other side:
`"mcpp": {"active": "xim:2026.8.30.2", …}`.

`ns:name` as a key was considered and does not work, for a reason stronger than
convention. A workspace key is looked up by the name a program is **invoked
as** (`get_active_version(workspace, program_name)`,
`src/core/xvm/shim.cpp:409-412`); nothing is ever invoked as
`xim:picolibc-riscv`, so such a key would be read by nobody — the shape §6
refuses for `deps`.

And the namespace is not a property of the tool. It qualifies **where a version
came from**, which is why one target legitimately carries both scoped and
unscoped versions at once. Measured, on this machine, for `mcpp` itself:

```
"mcpp": { "active": "xim:2026.8.30.2",
          "installed": ["2026.8.21.1", …, "xim:2026.8.28.2", "xim:2026.8.30.1"] }
```

Eight versions of one target, some from the xim index and some not. Moving the
namespace onto the key would split that into two targets, and `mcpp` on `PATH`
would resolve to whichever half won — which is the same defect in the small
that `ar` from two providers would be in the large.

So the form is:

```toml
[xlings.workspace]
picolibc-riscv = "xim:1.8.12"
```

the key being the xvm target and the namespace riding the value, exactly as the
file writes it. mcpp reconstructs the install address `xim:picolibc-riscv@1.8.12`
from the pair when it provisions, so nothing is lost, and the C library mcpp
injects (§4) is expressible in the same shape.

A key containing a colon is a **hard error naming the correct form**, rather
than a second accepted spelling: one fact, one way to write it, is the whole
argument of this document applied to itself.

**W3. The per-platform value form is unchanged.** It is already accepted on
both keys (2026.9.2.1) and it survives the merge unmodified.

## 4. What mcpp writes into `.xlings.json`

**Decided (2026-09-03), and revised the same day against the xlings source
(§13): the merge belongs in `mcpp.toml`, and the file keeps both fields.**

One authoring key. When mcpp materialises it, an entry becomes a `deps` element
*and* a `workspace` member, because the file's two fields have two different
consumers in xlings and neither subsumes the other: `deps` is the install
trigger read by a bare `xlings install` (`src/core/cmdprocessor.cpp:163`), and
`workspace` is a version-resolution layer merged into the project's effective
pins (`src/core/config.cpp:660`, `:846`). Emitting both is not a translation
layer — it is the faithful materialisation of one statement, "use this at this
version, provision it if absent", into the two places xlings reads those two
halves.

The earlier draft of this section proposed writing only `workspace` and letting
xlings provision from it. §13 measures that xlings does not: `deps` is the only
key its install path reads. Writing only `workspace` would have produced a
project that builds where the packages happen to be installed and fails on a
clean machine, which is the failure this document exists to avoid.

If xlings later provisions from `workspace`, the `deps` half of the emission
can be dropped without touching `mcpp.toml` or any manifest. That is the
end-state, and it is a change on the xlings side, not here.

One consequence for section 3. The target's C library that mcpp appends
(`prepare.cppm:3186-3194`) arrives from the target row as
`xim:picolibc-riscv@1.8.12`, an install address. As a workspace entry it is
`picolibc-riscv = "xim:1.8.12"` — the same two facts, in the shape the file
already uses. mcpp splits the address once, at the point it builds the entry;
nothing downstream sees two spellings.

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

## 6. Migration: one release, not a deprecation window

The population is three manifests. `aarch64-virt-rt`, `riscv-virt-rt` and
`std-freestanding` each declare one `[xlings] deps` entry of the form
`xim:<name>@<version>`; mcpp's own `mcpp.toml` declares no `[xlings]` section
at all. A three-phase deprecation exists to give an ecosystem time it does not
need here, so the migration is a single release: the packages are edited and
republished with the new form, and `deps` is refused in the same version that
introduces the merged reader.

What "refused" must mean, and this is the part that does not bend: `deps` stops
being honoured by becoming a **hard error that names the replacement**, never
by becoming a key nobody reads. `[xlings]` has no unknown-key sweep — no
`kKnownXlings` list exists in `toml.cppm` — so a silently dropped key would be
read by nobody and reported by nobody, which is the shape #531 exists to
prevent.

Ordering, because the three packages are consumed by projects that may be built
with either engine:

1. The merged reader ships, accepting `workspace` and refusing `deps` with a
   message naming the line to write.
2. The three packages are republished with `[xlings.workspace]` and an mcpp
   floor at that version.
3. An index sweep confirms no other published manifest declares `deps`. The
   local checkouts are not the ecosystem; the sweep is what makes the claim.

Step 3 gates nothing on the mcpp side — it is a check that the denominator was
what it looked like. If it turns up manifests nobody knew about, the refusal in
step 1 becomes an advisory for one release and the window opens after all.

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

Answered on 2026-09-03 and kept here with their answers, because a question
that was open is part of how the design was reached.

1. **Does xlings provision from `workspace`?** No. `deps` is the only key any
   install path reads (§13.2). Hence §4: the merge is in `mcpp.toml` and the
   file keeps both fields.
2. **Do workspace keys accept a namespace prefix?** The file's keys never carry
   one; the namespace belongs to the install address (§3 W2). mcpp accepts it
   in the authored key and writes the bare target name.
3. **Is `"*"` already spelled something else?** Yes, two ways: a version prefix
   and `latest`, both resolved before anything is stored (§3 W1). No new
   spelling is introduced.
4. **Does binding a package determine its programs' versions?** Yes, and the
   expansion happens when the entry is honoured rather than when it is merged
   (§15.1).
5. **The migration window.** Answered by the denominator: three manifests, so
   no window (§6). The index sweep confirms the denominator rather than gating
   the change.

The one decision left for review is D8 (§15.1): the provisioning pass sends
`useAfterInstall: true`, so a declared version becomes the active one in the
project's own layer rather than being installed beside whatever is already
active.

## 12. The section as a whole

The proposal changes one field of a section whose other fields are unaffected.
This is what `[xlings]` is at `4d99864`, so that a review of the change can see
what it is being made against.

### 12.1 Field correspondence

| `mcpp.toml` | `.xlings.json` | Shape | Readers in mcpp |
|---|---|---|---|
| `[xlings] deps` | `deps` | array of package references | five (§1); retired by this proposal |
| `[xlings.workspace]` | `workspace` | object, name to version | one, the materialisation; five after this proposal |
| `[xlings] subos` | `subos` | string | the materialisation, and `select_runtime` |
| `[xlings.envs]` | `envs` | object, name to value | one, the materialisation |
| `[indices]` (not under `[xlings]`) | `index_repos` | array of repo objects | `ensure_project_index_dir` |
| — | `lang`, `mirror` | strings | written by mcpp unconditionally |

Names and meanings correspond one to one, and mcpp adds no key of its own. The
file is written by `seed_xlings_json` (`src/xlings/xlings.cppm`), each field
emitted only when non-empty.

### 12.2 Three places mcpp is not a pure mirror

Stated because a "1:1, no translation layer" claim is checkable, and these are
the exceptions to it.

1. **mcpp appends an entry the manifest did not write.** The target's C library
   is added to the package channel, deduplicated, when the target row names one
   (`prepare.cppm:3186-3194`). It rides that channel rather than having one of
   its own so that one materialisation can be wrong instead of two. It is also
   the reason §3's W2 is a prerequisite.
2. **What is written is already resolved for this host.** A per-platform value
   is collapsed at manifest load (`resolve_host_value`), so the file is a
   materialisation for this machine rather than a copy of the declaration. §7
   is the consequence.
3. **`lang` and `mirror` are mcpp's, not the manifest's.** They come from
   mcpp's own configuration and are always present in the file.

### 12.3 Ownership: who declares the environment

One rule, in `mcpp.xlings.runtime_selection`, whose header states what it
deliberately does not read: the process environment, xlings' active or current
state, the compiler path, and dependency manifests. Allowing any of them would
make one `mcpp.toml` mean different ABIs in different shells.

- In a workspace build the **workspace root** owns the declaration, even after
  the package manifest switches to a selected member. An independently built
  member is its own owner.
- A dependency's `[xlings]` is never consulted and never propagated. A
  library's declaration applies when it is a root, not when its sources are
  consumed by another root.
- The file is written under the owner's root
  (`<ownerRoot>/.mcpp/.xlings.json`). When the owner is not the directory mcpp
  writes into, two files are written: indices to the work root, the environment
  to the owner root.
- Nothing is written at all unless the project declares indices, or declares
  `[xlings]`, or the target row names a C library
  (`materializeRootRuntime`, `prepare.cppm:3179-3181`).

### 12.4 `subos`: presence is semantic, and mcpp only reads

- **Absent** selects mcpp's initialised, release-verified `McppDefault`.
  **`subos = "default"`** is an explicit `NamedSubos("default")`. A string alone
  cannot distinguish absence from an empty value, which is why the manifest
  carries `subosDeclared` beside it.
- The name is validated as a portable identifier (letters, digits, `.`, `_`,
  `-`); anything else is a manifest error naming the value.
- There is **no CLI or environment override**, and no implicit following of
  xlings' active or current SubOS.
- A named SubOS that does not exist is a **hard error**, never a fallback:
  falling back would substitute a different environment for the one the
  manifest named. Creating and populating one is xlings' layer
  (`xlings subos new`); mcpp reads an environment and never creates one.
- An environment that exists but carries no `subos_info` **degrades**: the
  runtime binding reports inconclusive, a note is printed, and the build
  continues.
- On Linux the selection also fixes the loader and C library contract, so two
  SubOS names produce separately fingerprinted objects.
- Only a **declared** SubOS puts its `bin/` at the front of `build.mcpp`'s
  `PATH` (`projectSubosBin` is non-empty only for `Mode::NamedSubos`,
  `prepare.cppm:1389`). A project that declares nothing inherits the `PATH`
  mcpp was started with, byte for byte.

### 12.5 `envs` is not the environment a program runs in

Two channels are easy to confuse and are unrelated:

- `[xlings.envs]` is materialised into `.xlings.json` and read by xlings for
  the **tool** environment. mcpp has exactly one reader for it, the
  materialisation.
- `compute_subos_env` (`src/build/execute.cppm:418`) builds the environment a
  built program is **run** with, and it derives from `plan.runtimeBinding` —
  the SubOS's own `subos_info` — not from `[xlings.envs]`.

A value written under `[xlings.envs]` therefore does not reach `mcpp run`'s
child. Whether it should is a separate question from this proposal and is not
answered here.

### 12.6 What this proposal does not touch

`subos`, `envs`, `[indices]`, the ownership rule, the write conditions and the
`PATH` contract are unchanged. The change is confined to which of the two
package-shaped fields exists, and to the resolution loss §7 describes, which
the merged field inherits.

## 13. What the four fields actually do, measured in the xlings source

Read at `/home/speak/workspace/github/openxlings/xlings`, 2026-09-03. Section 4
and question Q1 of section 11 are answered here; section 12.5 is corrected.

### 13.1 How the file is found at all

xlings locates a project config by walking the current directory upward for a
`.xlings.json`, stopping at any directory that also contains a `subos/` — that
signature means "an xlings home", never a project — and, failing that, by
reading `XLINGS_PROJECT_DIR` (`src/core/config.cpp:765-799`).

mcpp writes `<project>/.mcpp/.xlings.json` and passes
`XLINGS_PROJECT_DIR=<project>/.mcpp`, so the file is reached through the
environment variable, not the walk. A person standing in the project root and
running `xlings` does not see it: the walk looks for `<project>/.xlings.json`,
one level up from where mcpp writes. Measured on a real materialisation
(`mcpplibs/riscv-virt-rt/.mcpp/.xlings.json`), whose `.mcpp/` holds no `subos/`
and therefore does not trip the home boundary.

### 13.2 `deps` is the only install trigger, on both sides

`install_from_project_config` (`src/core/cmdprocessor.cpp:163-196`) is the
no-argument `xlings install`. It reads `deps`, errors when the key is absent or
is not an array, and installs each entry through
`xmake xim -P <home> -- <target> -y`. It reads no other key.

mcpp's own provisioning does not use that path: it calls the `install_packages`
capability with targets it read from `mcpp.toml` itself. So the `deps` array in
the file serves a different consumer — a person running bare `xlings install` —
than the pass that makes `mcpp build` work.

### 13.3 `workspace` is a version layer, and a named subos drops the global one

The project file's `workspace` object is read into `projectWorkspace_`
(`config.cpp:660-662`) and becomes one layer of the effective pins.
`merged_workspace` (`config.cpp:846-864`) resolves them:

| Project subos mode | Layers merged, later winning |
|---|---|
| `Named` (the file declares `subos`) | project manifest, then that subos's own workspace |
| `Anonymous` (project file, no `subos`) | global, then project manifest, then the project subos |
| no project config | global only |

**Pinning a few and inheriting the rest is what the Anonymous row does.** A
project that writes `[xlings.workspace]` and no `subos` starts from the global
workspace and merges its own entries on top, so every tool it does not name
keeps the machine's version. This is the common shape for an mcpp project and
it works as an author would expect.

**A named subos does not inherit the global layer, and that is deliberate
rather than an omission.** A named SubOS is a different environment with its
own installed set; carrying the host's pins into it would name versions that
environment does not have. Its own `workspace` — stored in
`<subos>/.xlings.json` and merged as the last layer — is what it inherits from
instead. A SubOS created with `xlings subos new <name> --from <base>` receives
the base's workspace map by copy at creation (`src/core/subos.cpp:978`); that
is a one-time inheritance, not a live link.

**Nothing falls back silently when no layer names a tool.** The shim reads
`Config::effective_workspace()` and, finding no active version, produces a
diagnostic rather than choosing one (`src/core/xvm/shim.cpp:408-460`:
`xvm.no_active_version`, or the "installed in this subos, but no version is
active" form). The `qemu-aarch64-static is not installed in this subos (_)`
line measured during the 2026.9.2.1 verification is that path.

What mcpp does not state today is the first two paragraphs: that declaring
`[xlings] subos` changes which pins apply, and that the environment's own
workspace replaces the global one. That belongs in `docs/17`.

**A `workspace` entry installs nothing.** No install path reads it. This
answers Q1: xlings does not provision from `workspace`, which is why section 4
was revised rather than kept.

### 13.3.1 What a workspace key is: an xvm target, of any kind

Measured on the development host's default SubOS, 546 entries:

```
binutils = 2.42     ar = 2.42      as = 2.42      ld = 2.42
gcc      = 16.1.0   g++ = 16.1.0   cc = 16.1.0
mcpp     = xim:2026.8.30.2
Scrt1.o, crt1.o, crti.o, crtn.o, glibc.files.1 … glibc.files.101
```

Package roots, the programs of those packages, and file assets all live in one
namespace, each with `{active, installed}`. A package root and its programs
carry the **same version** because they are members of one release and
`cmd_use` wrote them together (§15.1) — that identity is the group expansion's
own footprint in the data.

So "the workspace holds packages" and "the workspace holds programs" are both
half-right: it holds xvm targets, and a package's root is one of them. Writing
the package in a manifest is therefore a legitimate entry, and its programs
receive the same version when the entry is honoured. Writing a program is
equally legitimate and selects the same release. What a key never carries is a
namespace; that rides the value (§3 W2).

### 13.4 `envs` has no reader anywhere

Every `envs` consumer in the xlings source is one of two structures, and
neither is the flat object mcpp writes:

1. `xvm`'s `VData::envs` — environment variables attached to **one program's**
   shim, stored in the version database and applied when the shim runs
   (`src/core/xvm/db.cpp:724`, `src/core/xvm/shim.cpp:337`). Set through
   `xvm add --env`, not through any project file.
2. A SubOS's `subos_info.envs` — "an object of **provider sections**" keyed by
   binding (`src/core/subos/manifest.cpp:197-290`), part of the environment's
   own metadata.

Searching the whole source for `contains("envs")` and `["envs"]` outside those
two files and the doctor that checks them returns nothing. There is no reader
for a flat name-to-value `envs` object in a project or home `.xlings.json`.

**So `[xlings.envs]` is written by mcpp and read by nobody.** It does not reach
a built program's environment either: `compute_subos_env`
(`src/build/execute.cppm:418`) derives that from `plan.runtimeBinding`, the
SubOS's own `subos_info`, and never consults `[xlings.envs]`. The sentence in
`docs/05-mcpp-toml.md` §2.13 that calls it "env vars applied to the tool
environment" describes an effect that does not occur.

**Decided (2026-09-03): `envs` is retired, not wired.** xlings never supported
a project-level flat `envs` object and does not need to: `[xlings]` exists to
align with xlings' project-level isolated environment, and the two `envs`
structures that do exist there belong to a program's shim and to a SubOS's own
metadata. Neither is something a consuming project should be writing.

It follows the same three phases §6 gives `deps`, and for a stronger reason: a
key read by nobody is the shape #531 exists to prevent, and this one is
additionally documented as having an effect. Phase 1 is the documentation
correction, which can ship immediately and independently of everything else in
this document — `docs/05-mcpp-toml.md` §2.13 and its Chinese twin currently
state an effect that does not occur.

### 13.5 Corrections this section makes to the rest of the document

| Where | Was | Is |
|---|---|---|
| §4 | mcpp writes only `workspace` | mcpp writes both fields; the merge is in `mcpp.toml` |
| §11 Q1 | open | answered: xlings does not provision from `workspace` |
| §12.1 | `envs` reader: "the materialisation" | no reader on either side |
| §12.5 | `[xlings.envs]` is read by xlings for the tool environment | it is read by nothing |

## 14. What mcpp's own documentation must say

The inheritance rule of §13.3 is the behaviour an author most needs and the one
mcpp states nowhere. It is not introduced by this proposal; it is being written
down because the proposal makes `workspace` the key everybody writes.

**`docs/05-mcpp-toml.md` §2.13** gains the rule as a table, next to the
`[xlings.workspace]` description:

| The project declares | The version of a tool it did not name comes from |
|---|---|
| `[xlings.workspace]`, no `subos` | the machine's global workspace; the project's own entries win over it |
| `[xlings.workspace]` and `subos = "<name>"` | that SubOS's own workspace; the global one does not apply |
| neither | the machine's global workspace |

with one sentence for why the middle row is not an omission: a named SubOS is a
different environment with its own installed set, and carrying the host's pins
into it would name versions that are not there.

**`docs/17-the-project-environment.md`** gains the consequence, because that
chapter is where `subos` is chosen: declaring a SubOS changes which version
pins apply, and a project that relied on the machine's pins has to state them
itself once it names an environment. The chapter already says mcpp reads an
environment and never creates one; this is the other half of what the
declaration decides.

**`docs/13-baremetal.md`** needs no change: it declares packages, not versions.

Each with its `docs/zh/` twin, which CI enforces.

A criterion, so the paragraph is not the only record: a project that pins a
tool and declares a SubOS resolves that tool to its pin, and a tool it does not
pin resolves inside the SubOS rather than to the host's global choice. It is an
e2e over two SubOS environments and one tool installed at two versions.

## 15. Self-review

Read against the code a second time, 2026-09-03. Three findings; the first is
the one that changes the proposal.

### 15.1 Withdrawn: the expansion happens, in the action rather than in the merge

The first version of this section claimed that a project-file `workspace` entry
cannot pin a package's programs, because the merge is per-key and the shim
looks up a program's own name. Both halves are true and the conclusion does not
follow. It reads the declaration as something mcpp only writes down, and this
proposal is that mcpp acts on it.

The mechanism, read through:

1. mcpp provisions each entry through the `install_packages` capability.
2. `xim`'s installer, having installed, calls `xvm::cmd_use(name, version)`
   for the requested target (`src/core/xim/commands.cpp:693`).
3. `cmd_use` resolves the whole release with `resolve_binding_selection` and
   writes one workspace entry **per member**
   (`src/core/xvm/commands.cpp:763-772`); the installer's own note says the
   same — "`cmd_use` creates shims for every member of the release it switches
   to" (`installer.cpp:1986`).
4. `Config::workspace_mut()` returns the **project's** SubOS workspace whenever
   a project config is loaded (`config.cpp:1163-1168`), so those per-member
   entries land in the project's own layer, which the merge applies last. The
   machine's global choice is not disturbed.

So naming a package root in the manifest does pin its programs: the group is
expanded when the entry is honoured, and what lands in the file is already
per-member. The declaration is the input to that action, not a layer expected
to expand itself.

**What the review did find is one flag.** Activation after install is
conditional (`installer.cpp` `activate_requested_targets`):

```cpp
auto active = xvm::get_active_version(Config::effective_workspace(), match.name);
if ((active.empty() || useAfterInstall) && has_version(db, match.name, match.version))
    cmd_use(match.name, match.version, stream);
else if (!active.empty() && active != match.version)
    // declining to switch is a decision, and it used to be a silent one
```

Install activates only when **nothing is active yet**, unless the caller asks
otherwise. mcpp's provisioning pass sends `{"targets": […], "yes": true}` and
nothing else, so on a machine where another version of that name is already
active, the declared version is installed and not the one that runs.

That is tolerable for `deps`, whose meaning is "must exist". It is not
tolerable for a table whose meaning is "at this version": a constraint that
installs without activating is not a constraint. The capability already takes
the flag — `useAfterInstall`, documented as "Activate the installed version
even if another version is currently active" (`src/capabilities.cpp:81`) — so
the change is one field in the request mcpp already sends.

**Decision D8: the provisioning pass passes `useAfterInstall: true`.** Its
blast radius is the project's own SubOS layer, per point 4 above, which is what
makes it safe to do unconditionally rather than behind another key.

### 15.2 "Retired" means the manifest key, never the file field

Section 6 reads as though `deps` disappears. It does not: §4 keeps emitting the
`deps` array into `.xlings.json`, because that is the only key xlings' install
path reads. What is retired is the key an author writes in `mcpp.toml`. Every
occurrence of "retired" in sections 0, 6 and 12 means that and only that, and
the phases apply to the manifest surface alone.

### 15.3 Section 12.6 is no longer true as written

It says the proposal touches neither `subos` nor `envs`. It touches both,
though not their behaviour: §13.4 asks for a decision on `envs`, which is
written by mcpp and read by nobody, and §14 adds documentation for what `subos`
does to the inheritance chain. Neither changes an effect; both change what the
project states about itself, which is the part this document is for.

### 15.4 What survived the review unchanged

The two facts section 1 rests on. The general form still installs nothing while
its documented shorthand installs, and nothing still compares the two keys when
both name one package. §13 strengthened rather than weakened them: xlings makes
the same split in the same direction, which is why the merge belongs in the
manifest and not in the file.
