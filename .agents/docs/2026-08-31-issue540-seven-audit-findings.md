# Issue #540, verified: seven filed findings, six confirmed, one misaimed, four more underneath

> Status: **shipped in 2026.9.1.1.** Everything in §11 landed except D2b (an
> upstream xlings change) and D7b (a structural rewrite of the `[build]` read
> block, deliberately deferred). Item 8's decision was taken: **D9** — the layer
> predicates are implemented, so `docs/14` describes something that exists.
>
> One thing this document did not predict. Implementing D9 made the `c-abi`
> layer's interface name a value users write, and the engine reported `gnu`
> where `docs/14` (and e2e 296's own header) said `glibc` — `payload_libc_name`
> returned the triple's env segment verbatim. A vocabulary that was cosmetic
> while it was only printed became load-bearing the moment a predicate compared
> against it, so the layer now names the library and the request keeps the
> segment's spelling. §13 records it with the other reversals.
>

> Baseline: `origin/main` @ `aef5191`, the commit the report audits. The working
> checkout at the time of writing was 37 commits behind and predates the
> `modules/` split, so every line anchor below was read from a detached worktree
> at `aef5191`, never from the checkout. A reader who greps the checkout will
> conclude that `modules/manifest/src/toml.cppm` does not exist.
>
> Measured with: mcpp `2026.8.30.2` (the audited release), gcc 16.1.0,
> Linux x86_64. Five probe projects, each reproduced inline; four need nothing
> from the index and no network.
>
> **Revision.** Rewritten twice. The first pass read the code; the second read
> the design record — the commit messages and `.agents/docs` entries that say why
> each mechanism exists — and the third asked, for the one feature that looked
> unimplemented, what a package does today instead. Six conclusions changed
> across the two rewrites, and §12 and §6.7 are entirely new. What the earlier
> drafts got wrong is recorded in §13 rather than removed, because the reasoning
> that produced a wrong answer is more useful than a document that only ever
> agreed with itself.

---

## 0. Verdicts

| # | filed as | verdict | what it actually is |
|---|---|---|---|
| 1 | `--profile` help says release, resolver says dev | **confirmed** | one stale copy of a decision six other sites state correctly, one of them an e2e |
| 2 | `index update <name>` filters only project indices | **confirmed** | recorded in the source as a follow-up; the promise is in the CLI, the limitation is in a comment |
| 3 | exit code 4 missing from the machine-output table | **misaimed, and the real gap is wider** | no enveloped command can return 4. The design record assigned this whole contract to `docs/spec/`, which was never written (§4) |
| 4 | `[features]` swallows unknown keys silently | **confirmed** | the only structured manifest section with no schema check |
| 5 | `std-module` keys read but reported unsupported | **confirmed** | second recorded drift of a hand-maintained list, narrated in a comment eight lines below the defect |
| 6 | the `cfg(c-abi = ...)` example fails both legs | **confirmed, far larger than filed** | a documented feature of the normative target-side chapter, never wired, 8 doc sites, failing silently |
| 7 | docs/13 and docs/17 predate #531 | **confirmed, and it is the least of what is wrong there** | see §12 |
| — | not filed | **confirmed** | the conditional axis rejects two `BuildInputs` members, one of which the xpkg grammar for the same axis accepts (§6.2) |
| — | not filed | **confirmed, measured** | **#531's provisioning never checks whether it succeeded.** An unresolvable package name is reported as provisioned, the stamp makes it permanent, and the build succeeds (§12.1) |
| — | not filed | **confirmed, measured** | that path honours neither `MCPP_OFFLINE` nor `MCPP_NO_AUTO_INSTALL`, while the precedent it claims parity with honours both (§12.2) |
| — | not filed | **confirmed** | `mcpp::target_libc()` reports `targetSysrootPkg`, which `prepare.cppm:7293` feeds *into* target-side resolution as `payloadLibcRef`, while `docs/05:1181` calls it "which C library was resolved" (§6.7) |

Nothing in this set is a usage error. Nine of the eleven are a statement mcpp
makes about itself that mcpp does not honour, which is the class the report
correctly named.

---

## 1. The shapes the set shares

**A rule is stated in one place and enforced from a hand-written copy of it.**
`kKnownBuildKeys` (§5), `kKnownConditionalBuildKeys` and the xpkg
`target_cfg` list (§6.2) are transcriptions of a set that exists elsewhere in
machine-readable form: the read sites immediately above them, and `BuildInputs`'s
member list. All three have drifted, in different directions.
`kKnownBuildKeys` has drifted twice — the comment at `toml.cppm:1334-1345`
narrates the `bmi_schedule` occurrence in detail, and three keys eight lines
above it are in the same state.

The consequence is not a missing warning but a **warning that is false**:

```
warning: [build] has unsupported key 'std-module' (ignored). Supported keys: ...
```

The key is read at `toml.cppm:556`. The only sentence mcpp offers about it tells
the author the opposite of what happens, and the comment at `:1340` says so in
those words about the previous instance.

**An answer is computed, carried back, and never consulted.** §12.1 is the sharp
case: xlings reports `E_NOT_FOUND`, `call()` parses it into
`CallResult::exitCode`, the provisioning site tests only the `expected`'s error
state — which `call()` never sets — and reports success. The sibling install
path 750 lines away in the same file reads `r->exitCode` correctly.

**A predicate that answers false is indistinguishable from one that was never
understood.** `cfgpred` returns `false` for an unknown key and an unknown
bareword (`prepare_inputs.cppm:111`, `:118`), and a `[target.<pred>.build]`
section whose predicate is false is dropped without a word. This is what makes
§6 silent, and it is worth fixing whichever way §6 is decided.

---

## 2. Finding 1 — the profile default

### 2.1 Measured

```
$ mcpp build --help | grep profile
    --profile <NAME>     Build profile: release (default) | dev | dist | ...
$ mcpp build
    Finished dev [unoptimized + debuginfo] in 0.08s
```

### 2.2 The disagreeing copies

| site | says |
|---|---|
| `src/cli.cppm:351` (`build`), `:414` (`test`) | release is the default |
| `src/build/prepare.cppm:703-709` `resolve_profile_name` | `fallback = "dev"` |
| `src/build/prepare.cppm:692-694` (its comment) | "The global default is `dev`" |
| `docs/05-mcpp-toml.md:1574`, `docs/zh/05-mcpp-toml.md:1369` | `flag > default-profile > global dev` |
| `tests/e2e/87_build_default_profile.sh:3-4, 24` | asserts `-O0`, with the same Cargo/Meson rationale |
| `mcpp.toml:12-14` | pins `default-profile = "release"` *because* the global default is not release |
| `src/cli.cppm:509-510` (`pack`) | correct and explicit: "default: `[build] default-profile`, else release" |

Six agree, two disagree. `mcpp pack` is the only caller passing a `release`
fallback (`src/pack/pipeline.cppm:57`, `:89`), and its help says so.

A third stale copy the report did not reach — `src/build/prepare.cppm:767`:

```cpp
std::string profile;            // --profile <name> (default "release")
```

### 2.3 D1

`src/cli.cppm:351` and `:414` become `dev (default) | release | dist |
<[profile.*] name>`; correct the comment at `prepare.cppm:767`. Leave `pack`'s
help alone. No behaviour change; `87_build_default_profile.sh` already holds it.

---

## 3. Finding 2 — `index update <name>`

`src/cli.cppm:645` declares `.arg(cl::Arg("name").help("If given, update only
this index"))`. The rendered help does not print the argument description —

```
$ mcpp index update --help
USAGE:
    mcpp index update [OPTIONS]
    mcpp index update <name>
```

— so the misleading sentence is source-only, but the USAGE line still advertises
a per-index selection. `index_update` (`src/pm/index_management.cppm:133`) syncs
the global repos unconditionally (`:148-150`) and applies `filterName` only to
project-level custom indices. The source says so at `:127-131`, including why:
`xlings update` has no per-index mode to call, and calls it "a follow-up".

**The recorded intent is therefore to implement it.** D2b below is the direction;
D2a is what to do until the upstream change exists.

- **D2a (now).** State the actual contract in the argument help and the command
  description: `<name>` selects among the **project's** custom indices; the
  global repos always sync wholesale. Keep the `:127-131` note — it stays a
  follow-up, it stops being a discrepancy.
- **D2b (the intent).** Give `xlings update` a per-index mode and pass the filter
  through. xlings change first, mcpp second.

---

## 4. Finding 3 — the exit-code table, and the contract that was never written

This is where the report is wrong, and the correction is larger than the claim.

### 4.1 Why 4 does not belong in that table

The table at `docs/11-machine-output.md:103-110` (zh `:86-90`, identical) sits at
the end of §3 "Asking for machine output", governing the three kinds
`--protocol-version` advertises. Exit 4 comes from `config::load_or_init`
failures at eight sites — `src/pm/index_management.cppm:27, 64, 107, 118, 134,
187` and `src/doctor.cppm:87, 1194` — none of which has a `--format json` path.
`self env --format json`, the one that could have produced it, is built
specifically not to: `src/cli/cmd_self.cppm:23-36` explains that the machine path
avoids `load_or_init` so that asking where `$MCPP_HOME` is does not create it.

### 4.2 The code that table omits is 1

```
$ mcpp xpkg parse /nonexistent.lua --format json
error: cannot open '/nonexistent.lua'        # stderr; stdout empty
$ echo $?
1
```

`src/cli/cmd_xpkg.cppm` returns 1 at `:117`, `:143`, `:172`, `:188`, `:260`.
Several of those write JSON to stdout *before* returning 1 — a fact a client
needs and the table does not state.

### 4.3 What the design record actually asked for

`.agents/docs/2026-08-08-machine-readable-output-protocol-design.md` §R4
enumerates the measured codes — 127 unknown subcommand, 2 unknown option, 2
unknown value, 70 uncaught exception — which is exactly the table that shipped,
and then says:

> 光接管 parse error 不够,还要把 usage / runtime / internal 的 rc 映射写成契约,
> 并覆盖异常边界 —— 否则客户端仍然要靠猜。这条现在是 `docs/spec/` 的内容,不是代码。

So the table is R4's **usage/internal** half, shipped without the **runtime**
half. `ls docs/spec/` is `package-identity.md`, `README.md`, `target-side.md` —
the assigned contract was never written.

The reporter's instinct about 4 is right about the *contract* and wrong about the
*location*.

### 4.4 D3

1. Add `| 1 | the command failed — see stderr; when stdout carries an envelope,
   `diagnostics` |` to both tables, and one sentence scoping the table to the
   enveloped commands. Without that sentence the next audit reaches the same
   conclusion this one did.
2. Write the `docs/spec/` exit-code contract R4 assigned: usage (2), runtime (1),
   config (4), unknown command (127), internal (70), with the channel each uses.
   That is where 4 belongs.

Criterion: an e2e asserting `mcpp xpkg parse <unparseable> --format json` exits
1. `tests/e2e/93_xpkg_parse.sh` covers only the success shapes today. Assert the
exit code, not that stderr is non-empty.

---

## 5. Finding 5 — `kKnownBuildKeys`, second drift

### 5.1 Measured

```toml
[build]
std-module        = "gen/std.cppm"
std-compat-module = "gen/std.compat.cppm"
std-module-flags  = ["-D_GNU_SOURCE"]
```

```
warning: [build] has unsupported key 'std-compat-module' (ignored). ...
warning: [build] has unsupported key 'std-module' (ignored). ...
warning: [build] has unsupported key 'std-module-flags' (ignored). ...
    Finished dev [unoptimized + debuginfo] in 0.08s
```

All three are read at `toml.cppm:556-560` and take effect.

### 5.2 D5

Add the three keys to `kKnownBuildKeys` (`toml.cppm:1320-1328`). One line.

### 5.3 D7 — why a one-line fix is not the whole answer

This list is a transcription of the `doc->get_*("build.…")` calls above it. It
has drifted twice, the second time with a detailed narration of the first eight
lines below it, and §6.2 shows the same class in two more lists. A third
occurrence is a matter of time and the failure mode is a warning that lies.

- **D7a (recommended).** One unit test per vocabulary, loading a manifest that
  names every key the parser reads and asserting `schemaWarnings` is empty:
  `[build]`, `[target.<pred>.build]`, and the xpkg `target_cfg` block. The key
  list in the test is another copy — that is the objection — but it is a copy
  that **fails loudly** when it disagrees, which the current arrangement does
  not.
- **D7b.** Derive the lists. Replace the free-standing `get_string("build.X")`
  calls with a `{key, reader}` table and let both the parse and the allowlist
  iterate it, the way `mcpp:` directives are already table-driven. Structural;
  touches every read in the `[build]` block; a separate change from D5.

D5 and D7a together. **Do not ship D5 without D7a** — a one-line fix to a list
that has drifted twice is a third opportunity, not a repair.

Criterion for D7a: the test must fail against `aef5191`. Verify that before
believing it.

---

## 6. Finding 6 — `cfg(c-abi = ...)`: a documented feature with no implementation

### 6.1 Leg one: the predicate cannot ever be true

`cfgpred::Ctx` is built by `context_for(targetTriple)`
(`src/build/prepare_inputs.cppm:62-88`) from the triple and nothing else. The
grammar comment at `:90-91` states the vocabulary outright:

```
key  ∈ {os, arch, family, env}   bareword ∈ {windows, unix, linux, macos}
```

`c-abi` is therefore not a missing branch in `match_kv` (`:113-119`): the value
it would compare against is not in scope, and by `docs/spec/target-side.md` §3.5
it cannot be — the target side resolves only after dependency resolution, while
`merge_conditional_config` runs at `prepare.cppm:1958` from a triple-only
context.

Measured, against a predicate that is true of the host on its own terms:

```toml
[target.'cfg(env = "gnu")'.build]      defines = ["PROBE_ENV=1"]
[target.'cfg(c-abi = "glibc")'.build]  defines = ["PROBE_CABI=1"]
```

```
error: #error "cfg(c-abi=glibc) did NOT apply"
```

`PROBE_ENV` was defined, `PROBE_CABI` was not, and **no diagnostic was emitted
about the section that did nothing**. An author following `docs/14` gets a
successful build with the wrong C-library configuration.

### 6.2 Leg two: two lists for one axis, disagreeing with each other and with the type

`stdModuleFlags` **is a member of `BuildInputs`** (`types.cppm:262`), and its
comment states why in exactly the terms of the documented example:

> A MEMBER OF THIS TYPE AND NOT OF THE MANIFEST, for the same reason `defines`
> is: membership here is what makes the cfg axis carry it. […] `-D_GNU_SOURCE`
> is right for musl and glibc and wrong for picolibc, and while this lived
> beside the package's identity there was no spelling for that difference.

That is not an inference. Commit `61c7446` (#494), which moved the three keys
from `[package]` to `[build]`, states the purpose in its message —

> ⭐ 而放进 `[build]` 还白得一样能力:它立刻可以按目标侧条件化。

— and `.agents/docs/2026-08-24-target-side-architecture.md:222-232` gives the
worked example, `cfg(c-abi = "musl")` / `cfg(c-abi = "picolibc")`, verbatim as it
now appears in `docs/14`. The data model was reshaped **for** this. What was
never added is the read (`toml.cppm:1933-1947` does not include it) and the
allowlist entry.

`privateIncludeDirs` is worse, because the same axis accepts it in the other
grammar. The xpkg descriptor's `target_cfg` block reads it
(`xpkg.cppm:1416`) and names it in its own error text (`:1421-1425`), and
commit `e187d3f` (#515) records adding it there for exactly the reason at issue:

> `target_cfg` 的未知键报错列出「期望哪些键」,我加了 `private_include_dirs` 却没有
> 把它加进那份列表 —— 于是错误信息会把一个**已经被接受**的键说成不存在。补上。

The policy difference between the two grammars is deliberate and recorded —
`xpkg.cppm:1410-1413`, "Unknown sub-keys stay a HARD ERROR here […] mcpp.toml's
own unknown-key policy is a separate question — #263". The **vocabulary**
difference is not stated anywhere, and `kKnownConditionalBuildKeys`'s own comment
claims the opposite:

> The conditional axis carries BuildInputs and nothing else, so its vocabulary is
> exactly that struct's members. […] MUST stay in sync with the reads above and
> with types.cppm's BuildInputs.

| key | `BuildInputs` member | xpkg `target_cfg` | mcpp.toml `[target.<pred>.build]` |
|---|---|---|---|
| cflags, cxxflags, ldflags, sources, defines, flags, include_dirs, include_dirs_after | yes | accepted | accepted |
| `private_include_dirs` | yes | **accepted** | **rejected** |
| `std-module-flags` | yes | rejected | **rejected** |

Measured with a predicate that *is* true:

```toml
[target.'cfg(linux)'.build]
private_include_dirs = ["priv"]
std-module-flags     = ["-D_GNU_SOURCE"]
defines              = ["PROBE_TRUE_PREDICATE=1"]
```

```
warning: [target.cfg(linux).build] has unsupported key 'private_include_dirs' (ignored). ...
warning: [target.cfg(linux).build] has unsupported key 'std-module-flags' (ignored). ...
    Finished dev [unoptimized + debuginfo] in 0.08s
```

### 6.3 Blast radius

`cfg(c-abi = …)` appears at **8 sites**, three chapters, both languages:

| file | lines |
|---|---|
| `docs/14-target-side.md` | 239, 255, 258 |
| `docs/zh/14-target-side.md` | 203, 216, 219 |
| `docs/05-mcpp-toml.md` | 1380 |
| `docs/zh/05-mcpp-toml.md` | 1197 |

`docs/14` §"Adaptation To The Resolved Target Side" is a full section with its
constraints worked out: why a feature selection is the wrong spelling, and why
the feature is scoped to `[build]` ("The target side is resolved after dependency
resolution, so a dependency selected by one would form a cycle"). That is design
intent with the two-pass structure already anticipated.

### 6.4 What must not be done

**Rewriting the examples to `cfg(env = "musl")` is wrong.**
`docs/spec/target-side.md:137` (rule 3.4, marked implemented) states that the
triple's `env` segment must be treated as a **request** for the `c-abi`, never as
its answer — the answer may come from the graph, e.g. `openkal-musl` supplying
musl under a `-gnu` triple. Substituting one for the other documents a different
behaviour and buries the finding.

### 6.5 D6 — the two conditional keys (independent of the predicate work)

`read_list("std-module-flags", cc.inputs.stdModuleFlags)` and
`read_paths("private_include_dirs", cc.inputs.privateIncludeDirs)` at
`toml.cppm:1933-1947`, plus both keys in `kKnownConditionalBuildKeys`. This
completes `61c7446`'s stated purpose and removes the divergence from the xpkg
grammar. It makes `[target.'cfg(linux)'.build] std-module-flags` work **today**,
without any predicate change.

### 6.6 D8 — make the silence loud (highest value per line; do it early)

An unknown `cfg()` key or bareword returns `false` and the section vanishes.
Emit a schema warning instead.

Place the check in `toml.cppm`, beside the other unknown-key warnings, where the
predicate string is available at manifest load and `schemaWarnings` already
exists — not in the evaluator, which has no diagnostic channel and runs once per
call site.

⚠️ **Share the vocabulary; do not transcribe it.** Export the key and bareword
sets from `cfgpred` and have the validator call into them. Writing the list a
second time in `toml.cppm` creates the fourth copy of exactly the defect §1
names.

Scope it to the inside of `cfg(...)`. The bare-triple namespace has a documented
escape hatch — `matches()`: "Unparseable keys (the explicit-section escape hatch)
fall back to exact string comparison" — which must keep working.

Warning rather than error is the established policy and is what keeps forward
publication possible: `e187d3f` measured that an older engine reading a new key
warns in a root manifest and is silent in a dependency, and concluded that a
package may adopt a key before its consumers upgrade. The same holds here.

### 6.7 What a package can already do without any of this

The declarative axis is not the only spelling for "adapt to the resolved target
side", and most of `docs/14`'s example does not need it. `BuildProgramEnv` states
the design intent directly (`build_program.cppm`, field comments):

> The resolved toolchain's payload root and the target's own C library root.
> Both exist so a package can **ASK instead of DECLARE** […]
> Three more answers a board-support package would otherwise hardcode.
> ⚠️ THE COUPLING THESE REMOVE IS INVISIBLE IN A MANIFEST.

A `build.mcpp` receives `MCPP_TARGET_LIBC`, `MCPP_COMPILER`,
`MCPP_TARGET_SYSROOT`, `MCPP_TOOLCHAIN_DIR`, `MCPP_TARGET_BUILTINS_LIB` and
`MCPP_TARGET_LIBC_PROFILE` (`build_program.cppm:420-430`), and may emit
`mcpp:cfg=`, `mcpp:include-dir=`, `mcpp:include-dir-after=`, `mcpp:cxxflag=`.
So the `include_dirs` half of `docs/14`'s "Adaptation" section — `config/musl`
versus `config/picolibc` — is expressible today, imperatively, with no engine
change. `docs/05-mcpp-toml.md:1181-1184` documents the accessors.

Three things that channel does **not** reach, and they are what the decision is
actually about:

1. **`std-module-flags` has no directive.** The whole table —
   `cxxflag`/`cflag`/`cfg`/`include-dir`/`link-*`/`source`/`generated` — reaches
   the package's own translation units. The std module is compiled from
   `tc->stdModuleFlags` in the toolchain layer (`prepare.cppm:8122` →
   `clang.cppm:206`, `:361`), which no directive feeds. `-D_GNU_SOURCE` for musl
   and not for picolibc — the motivating example — is exactly this key.
2. **`target_libc()` answers the payload question, not the resolved one.**
   `e.targetLibc = tc->targetSysrootPkg` (`prepare.cppm:923`), and
   `targetSysrootPkg` is fed *into* the resolver at `:7293` as
   `in.payloadLibcRef` — it is an **input** to target-side resolution, not its
   output. When the C library comes from the dependency graph
   (`openkal-musl`), which is the case `docs/14` was written for, it is not the
   answer. `docs/05:1181` nevertheless calls it "which C library was resolved".
   **This is a defect in its own right, independent of the decision below, and
   it currently misleads package authors.**
3. **A dependency's `build.mcpp` runs too early.** The dep loop is at
   `prepare.cppm:6921`; the target side resolves at `:7351`. The ROOT program
   was already moved past it once — "L3: ROOT build.mcpp (moved after dependency
   resolution, design §3.1 item 4)" at `:7645` — and the dependency loop was
   not. The motivating package is a dependency.

### 6.8 D9 — implement the layer predicates

The cost is smaller than "the merge runs before the answer exists" suggests,
because the ordering leaves a window and something already ships in it.

The timeline, all in `prepare.cppm`:

| line | event | what it fixes in place |
|---|---|---|
| 1958 / 4123 / 5525 | `merge_conditional_config` + `fold_build_defines_into_flags` | triple-only context |
| 3246 / 5159 / 5594 | `packages[]` snapshots — `PackageRoot` holds a `Manifest` **by value** (`:3249`) | the merged inputs |
| **7351** | `resolvedTargetSide = tsd::resolve(in)` | the answer exists |
| ~7660–7800 | **build.mcpp directive tails are mirrored into `packages[0]`** | precedent, see below |
| 7810 | `modgraph::scan_packages*` over `packages` | `sources` and `defines` (P1689 needs the final `-D` set) |
| 8070 | `pkg.manifest.buildConfig.stdModuleFlags` collected → `tc->stdModuleFlags` | the std module compile |
| 8227 | `canonical_compile_flags` → the fingerprint | everything |
| 8500+ | `compute_flags` at plan time | the compile lines |

So **7351 → 7810 is an open window**, and mcpp already uses it for exactly this
shape. The build.mcpp path contributes build inputs after the snapshot and says
so in its own comment — "apply() mutated `*m`, but `packages[0].manifest` is a
[snapshot] … so mirror the directive TAILS into `packages[0]`" — mirroring
`sources`, `cflags`, `cxxflags` and the include dirs. A layer-conditional pass is
the same mechanism with a different producer.

`fold_build_defines_into_flags` is explicitly re-runnable: its comment states
"Idempotent: clearing the vector after folding makes repeated calls harmless",
and `bc.defines.clear()` at `:329` is what makes that true.

D9 is therefore: carry the resolved target side into `Ctx`, add the five layer
names (`compiler`, `compiler-runtime`, `kernel-abi`, `c-abi`, `c++-abi`) to
`match_kv`, and run one additional merge pass in that window. Note `61c7446`'s
constraint: the `compiler` layer reports the **family** (`llvm`), never the
driver (`clang`), because that is what users write.

Four things must be decided rather than discovered, and they are the actual
content of this item:

1. **Which `BuildInputs` members may be layer-conditioned.** Every one of them
   fits the window — `sources`/`defines` need only to precede 7810,
   `std-module-flags` 8070, `ldflags` plan time — so "all of them" is available.
   It still has to be *stated*, because the answer is what the warning text in
   `kKnownConditionalBuildKeys` will claim.
2. **Predicates must be classified, not merely evaluated.** `append()` is
   additive, so a second pass that re-runs the triple-predicate sections
   double-appends them. Each `[target.<pred>]` section needs a parse-time
   marker for whether its predicate names a layer, and each pass runs only its
   own half.
3. **All of `packages[]`, not just `packages[0]`.** The build.mcpp precedent
   patches the root only, which is correct for build.mcpp. `docs/14`'s
   motivating case is a *dependency* — a package supplying one C++ runtime over
   several C libraries — so the pass must cover every entry.
4. **Dependencies stay excluded.** `[target.<pred>.dependencies]` cannot be
   layer-conditioned; `docs/14` already states why (the selection would form a
   cycle with the resolution that produces the answer). Unchanged.

Criterion: a dependency package whose `[target.'cfg(c-abi = "musl")'.build]`
contributes an `include_dirs` entry must show that directory on the consumer's
compile line for a musl target and not for a glibc one — assert on
`compile_commands.json`, with both legs, because a pass that never fires and a
pass that always fires both produce a green single-leg test.

### 6.9 D16 — the third option: finish the channel that already exists

> **Not taken.** D9 shipped instead, for the cost-floor reason argued at the end
> of this section: a package wanting one conditional flag should not have to
> ship and run a C++ program. **D16a was taken anyway**, because it is a defect
> rather than an option — `docs/05` described `target_libc()` as the resolved C
> library when it names the payload, and that misleads authors under any of the
> three answers. D16b and D16c were not needed once the declarative axis worked.

Do not build a predicate axis. Close the three gaps in §6.7 instead:

- **D16a.** Make the resolved `c-abi` reachable from a build program — either by
  correcting `target_libc()` to report `resolvedTargetSide.cAbi` or by adding a
  sibling accessor beside it, and by saying in `docs/05` which question each
  answers. Required under D9 and D10 as well; §6.7 item 2 is a defect either way.
- **D16b.** Add `mcpp:std-module-flag=` to the directive table, plumbed to
  `tc->stdModuleFlags` at `prepare.cppm:8122`. One directive.
- **D16c.** Move the dependency `build.mcpp` loop past target-side resolution,
  following the precedent the root loop set at `:7645`.

`docs/14`'s "Adaptation" section is then rewritten around a build program, and
the `cfg(c-abi = …)` examples are withdrawn with D10.

**What actually separates D9 from D16.** Not `docs/14`'s stated argument — that
a feature selection "would oblige a project to restate what the target triple or
its dependency graph has already established". A build program does not restate
anything either; it *asks*, which is the same relation to the answer. That
argument rules out features and does not distinguish these two.

The real difference is the cost floor. D16 obliges a package that wants **one**
conditional flag to ship a C++ program and run it on every configure. D9 is
declarative and costs the author nothing per package, at the price of a second
evaluation phase in the engine and a permanent obligation on every future
`BuildInputs` member to declare which phase it belongs to.

D16 is also the only option whose parts are useful on their own: D16a is a
defect fix, D16b closes a gap in a documented table, and D16c aligns two call
sites that already disagree.

### 6.10 D10 — withdraw

If the layer predicates are not going to be implemented, withdraw the claim:
delete §"Adaptation To The Resolved Target Side" from `docs/14` and both
`docs/05` examples, in both languages, and say that conditioning on a resolved
layer has no spelling yet. Eight sites, four files.

Do not leave it as written. A normative chapter describing a mechanism that
evaluates to false in silence costs the reader's trust in the rest of the
chapter.

Criteria — D6: `[target.'cfg(linux)'.build] std-module-flags` reaches the
std-module compile; assert on the command line in `compile_commands.json`, not on
the absence of a warning. D8: a manifest with `cfg(nonsense = "x")` produces a
named warning and `--strict` fails.

---

## 7. Finding 4 — `[features]` has no schema check

### 7.1 Measured

```toml
[features]
fast = { implies = [], include_dirs = ["nope"], totally_bogus = 1 }
```

Zero diagnostics. The same `include_dirs` misplaced into `[build]` or
`[target.<pred>.build]` warns.

The parse region (`toml.cppm:439-508`) looks up known keys and never enumerates:
`implies`, `forward`, `defines`, `sources`, `requires`, `provides`, `flags`.

### 7.2 D4

Enumerate the feature table's keys against that set and warn on the rest.

Two decisions to state rather than assume:

- **`deps`.** The comment at `:437-438` calls `requires`/`provides`/`deps`
  "reserved for later stages". `requires` and `provides` are now read; `deps` is
  not. A reserved-but-unread key should warn, saying it is reserved — silence is
  what made `action.blocking` invisible for a release cycle.
- **Forward compatibility is not at risk.** `schemaWarnings` surface only for the
  ROOT manifest: `prepare.cppm:1238` runs on `m` before any dependency manifest
  is loaded, and `e187d3f` measured the same thing from outside ("出现在**依赖**
  清单里 ⇒ 静默接受,退 0"). A published package using a future feature key
  cannot make a consumer's build noisy.

---

## 8. Finding 7, part one — the documents

| file | line | says |
|---|---|---|
| `docs/13-baremetal.md` | 558 | "A declaration under `[xlings] deps` is not an install trigger […] it installs nothing" |
| `docs/zh/13-baremetal.md` | 500 | same |
| `docs/17-the-project-environment.md` | 113 | "a directory that has to be created and populated before the first build, and mcpp will not do it" |
| `docs/zh/17-the-project-environment.md` | 93 | same |

Since #531, `prepare.cppm:3131-3208` provisions declared `[xlings] deps` on the
first build, and the comment argues the case: "the same 'declare it and mcpp
provisions it on first use' contract `[toolchain]` has had all along […] A build
environment should not have two grades of declaration." It also notes that
provisioning is what creates a named `[xlings] subos` that does not yet exist —
precisely what `docs/17:113` says mcpp will not do.

**D11.** Update the documents, not the code. The rationale is sound and the
`[toolchain]` parallel is the right one. `docs/13`'s surrounding advice — a build
program should check `xpkg_dir` and warn when empty — stays correct; only the
"installs nothing" claim is false. Four files, both languages, as CI requires.

Sequence D11 **after** §12, so the documents describe the behaviour that exists
rather than being rewritten twice.

---

## 9. Finding 7, part two — the parity that was claimed and not delivered

The #531 comment invokes `[toolchain]` as its precedent. Reading that precedent
is what turned up §12; it is worth stating separately what the precedent does.

**It does not remember. It checks.** The toolchain path maps a spec plus a target
onto a payload, "installing it if absent — `autoInstall` was always true there"
(`prepare.cppm:2467-2469`). There is no "I already installed this" record. What
is persisted is the *choice* (`write_default_toolchain` into the global config),
never the fact of installation, because presence answers that question directly.

**It gates auto-install on two knobs, and names the one that fired**
(`prepare.cppm:2235-2251`):

> CI / offline / test opt-out: hard-error instead of silently pulling ~800 MB of
> toolchain. […] `--offline` / MCPP_OFFLINE subsumes MCPP_NO_AUTO_INSTALL: the
> older name only ever covered this one gate […] The old var is kept working (it
> predates offline mode and CI still exports it).

The #531 path does neither. §12 is what that costs.

---

## 10. Not recommended

- **Adding exit code 4 to `docs/11`'s table as filed** (§4.1). Write the
  `docs/spec/` contract instead; 4 belongs there.
- **Rewriting the `cfg(c-abi = …)` examples to `cfg(env = …)`** (§6.4). The spec
  states these are different questions.
- **Gating #531's provisioning behind an opt-out flag** (§8). A second grade of
  declaration is what it removed. The knob parity in §12.2 is a different thing.
- **Transcribing the cfg vocabulary into `toml.cppm` for D8** (§6.6). Export it.
- **Fixing `kKnownBuildKeys` without a test** (§5.3).
- **Adding an offline gate before fixing §12.1.** With the result unread, an
  offline failure is swallowed like any other; gating first would hide the
  larger defect behind a narrower one.

---

## 11. Sequencing

| | change | why here |
|---|---|---|
| 1 | **D12** — read the provisioning result (§12.1) | a declared package that does not exist is currently reported as provisioned, permanently. One-site fix with the correct idiom 750 lines away in the same file. |
| 2 | D13 + D14 — knob parity and presence over memory (§12.2, §12.3) | completes the parity §9 describes. D14 subsumes most of D13's edge cases. |
| 3 | D5 + D6 + D7a — the three `[build]` keys, the two conditional keys and their reads, and the test holding all three vocabularies | smallest self-contained group; stops four false warnings and completes `61c7446`'s stated purpose |
| 4 | D8 — an unknown cfg key is a diagnostic | highest value per line, correct under either D9 or D10, and what makes the 8 doc sites visible to their authors |
| 5 | D1 + D3 + D2a + D4 — the text corrections and the `[features]` check | mechanical; no interaction with anything above |
| 6 | D11 — `docs/13` and `docs/17`, four files | after 2, so the documents describe the gated behaviour |
| 7 | **D16a** — `target_libc()` reports the payload ref while `docs/05` calls it the resolved C library (§6.7 item 2) | a defect on its own terms, required under all three options below, and it misleads package authors today |
| 8 | D9 / D16 / D10 — the declarative axis, the imperative channel, or withdrawal | the only item needing a decision rather than an implementation. **Decided: D9**, plus D16a because it is a defect either way (§6.9) |
| 9 | D15 — the tests §12.4 names | after the behaviour they assert exists |

Items 1 through 7 are unambiguous. Item 8 wants a maintainer's answer, and D8
buys the time to give it: once an unimplemented predicate says so, the eight doc
sites stop being a silent trap while the decision is made.

**What item 8 is actually asking.** Not "is this feasible" — §6.8 shows the
window exists and that build.mcpp already contributes through it — and not "can a
package solve this at all", because §6.7 shows most of it is already solvable
imperatively. The question is narrower and is a contract question:

> Does the resolved target side become something a **manifest** can branch on, or
> does it stay something only a **build program** can ask about?

Answering "manifest" (D9) adds a second evaluation phase to the conditional axis
and obliges every future `BuildInputs` member to declare which phase it belongs
to. Answering "build program" (D16) keeps the engine simpler and charges every
package that wants one conditional flag the price of shipping and running a C++
program. Answering "neither" (D10) is four files and eight sites today, and
stops being cheap the moment one published package depends on it.

`docs/14` currently answers "manifest" and nothing implements it.

---

## 12. What verifying finding 7 turned up

### 12.1 The provisioning never checks whether it succeeded

Measured. A manifest naming a package that cannot exist:

```toml
[xlings]
deps = ["definitely-not-a-real-package-540"]
```

```
$ MCPP_OFFLINE=1 mcpp build
Provisioning [xlings] deps (definitely-not-a-real-package-540)
   Resolving toolchain
    ...
    Finished dev [unoptimized + debuginfo] in 0.12s
$ cat .mcpp/.xlings-deps.stamp
definitely-not-a-real-package-540
$ mcpp build
    Finished dev in 0.00s          # no "Provisioning" line — recorded as done
```

xlings reports the failure correctly. Running the exact command
`xlings.cppm:1390` builds:

```json
{"code":"E_NOT_FOUND","message":"package 'definitely-not-a-real-package-540' not found in the synced index …","recoverable":true}
{"exitCode":1,"kind":"result"}
```

and its **process** exit code is 0, by design — the NDJSON `result` line is where
the capability's status lives. `call()` handles this correctly
(`xlings.cppm:1274-1302`): it parses the result line into `CallResult::exitCode`,
records the `ErrorEvent` in `CallResult::error`, and returns
`std::expected<CallResult, std::string>` — **always in the value state**. The
`expected`'s error channel is for the call not happening at all.

The provisioning site (`prepare.cppm:3191-3204`) tests only that:

```cpp
auto r = mcpp::xlings::call(..., "install_packages", args.dump(), &progress);
if (!r) { /* "provisioning [xlings] deps failed: …" */ }
```

`r->exitCode` and `r->error` are never read, so the error branch — whose comment
promises "An ambiguous bare name ("mesa" matching two repos) lands here, and
xlings' own message names the candidates" — is unreachable for any failure xlings
reports through the protocol.

**The correct idiom is in the same file.** The dependency install path at
`prepare.cppm:3959` reads `if (r && r->exitCode != 0 && …)`, and
`package_fetcher.cppm:410` logs `r->exitCode` for its callers to check. This is a
single-site omission, not an interface problem and not an xlings problem.

The irony is exact: #531's own comment says the defect it fixed was "The
declaration looked accepted and did nothing — which is the worst shape a config
key can have." For a name that does not resolve, the fix reproduces that shape
and the stamp makes it permanent.

**D12.** Read `r->exitCode` and `r->error` at `prepare.cppm:3194`, and write the
stamp only on success. Reuse the existing message at `:3199-3203`, which becomes
reachable. One site.

### 12.2 Neither offline knob is honoured

Measured, same project:

```
$ MCPP_OFFLINE=1 mcpp build          → Provisioning …  Finished dev
$ MCPP_NO_AUTO_INSTALL=1 mcpp build  → Provisioning …  Finished dev
```

Against the precedent at `prepare.cppm:2235-2251`, which hard-errors on either
and names the knob that fired. `MCPP_NO_AUTO_INSTALL` is what CI exports to
prevent exactly this, and this path has never heard of it.

**D13.** Gate the install attempt on `offline_mode() || no_auto_install()`,
hard-error, name the knob that fired, and list the packages so they can be
installed out of band. Shape it on `:2244-2251`, which already solves the
"name the knob the user actually set" problem.

⚠️ Gate the **attempt**, not the block. Under D14 an already-satisfied dependency
must still build offline; a blanket refusal would turn a working offline build
into a hard failure, which is the regression `prepare.cppm:3123-3130` warns about
for the sysroot entry.

### 12.3 The stamp remembers the request; the precedent checks the effect

`<ownerRoot>/.mcpp/.xlings-deps.stamp` (`prepare.cppm:3134`) holds the declared
list, while the installation goes to **global** scope via `make_xlings_env` —
deliberately, and the comment explains at length why project scope was measured
not to work. So the record and the effect live in different places and nothing
reconciles them:

- the global package is removed, or `MCPP_HOME` is wiped or replaced (a CI
  matrix, a second home, a sandbox), while the project stamp persists — mcpp
  never re-provisions and says nothing;
- `mcpp clean` removes `target/` and optionally the cache
  (`execute.cppm:1958`, `:1967`) and never touches `.mcpp/`, so the usual
  recovery gesture does not clear it.

The existing comment has the right instinct — "Idempotence by CONTENT, not by
existence" — and applied it to the content of the *declaration* rather than of
the *effect*. §9's precedent does the opposite and needs no stamp at all.

**D14.** Make the fast path a presence check rather than a memory, matching
`[toolchain]`. Two ways, and the choice is a measurement, not an opinion:

- **D14a.** Drop the stamp; call `install_packages` every build. Exact parity
  with `[toolchain]`, and correct by construction. Costs one xlings round trip
  per build. **Measure that round trip against a no-op incremental build before
  choosing** — it is a process spawn, and the stamp exists because someone
  judged it too expensive without recording a number.
- **D14b.** Keep a stamp as a pure cache, but move it into the registry keyed by
  a hash of the list, so it is destroyed with the thing it records whenever the
  home is wiped or replaced, and is shared between projects declaring the same
  packages. Does not cover a user's `xlings remove`; no stamp does.

A presence check keyed on the declared names is the third option and the best
one, but it is blocked on a detail the code already records: `resolve_xpkg_path`
requires `<name>@<version>` and rejects a bare `mesa`, while a manifest is
entitled to name a package without pinning it. A bare-name presence primitive has
to exist first.

Note that D12 alone removes the sharpest edge: with the result read, the stamp is
only ever written for a provisioning that actually succeeded.

### 12.4 The path has no test

`grep -rn "xlings-deps.stamp" tests/` is empty.
`tests/e2e/88_xlings_environment.sh:51-52` asserts only that the deps are
*materialized into* `.xlings.json` — true before #531, and exactly the "declared
and nothing happened" state the change was made to end.

> **Confirmed harder than predicted, by implementing D12.** That test's fixture
> declares `deps = ["make@4.4", "cmake@3.28"]`. The index carries make **4.3**
> and cmake **4.4.2 / 4.0.2** — *neither version has ever existed*. The test
> passed anyway, for the whole life of #531, because the provisioning did not
> read its own result: xlings answered `E_NOT_FOUND` and mcpp called it done.
>
> So the repository's own most direct coverage of `[xlings] deps` was asserting
> materialization on top of an install that never happened. D12's first catch,
> before any user's, was this test. The fixture now names `ninja@1.12.1` —
> already installed anywhere mcpp can build, so provisioning short-circuits and
> the test still costs no download.
>
> ⭐ The general shape: **a fixture's values stop being arbitrary the moment
> something starts checking them.** These two were free-form strings for as long
> as the only assertion was "does this text reach that file".

**D15**, in the order they can be written:

1. **Knob parity, network-free.** `MCPP_NO_AUTO_INSTALL=1` with a declared dep
   must fail before any xlings call. Runs anywhere; no index needed.
2. **Failure is reported.** A declared name that cannot resolve must fail the
   build and must NOT write the stamp. Needs a synced index but no install —
   the probe in §12.1 is the test.
3. **Idempotence across two invocations**, with the denominator stated: a single
   run cannot distinguish "provisioned" from "provisioned twice".

---

## 13. Review record

Four conclusions of the first draft changed on re-reading the design record.
They are kept because each was wrong in a way worth naming.

**The exit-code finding was scoped too narrowly.** The first draft said "do not
add 4" and stopped. Reading R4 of the protocol design doc showed the table is
half of an assigned contract and the other half was never written, so the useful
answer is "add 1 here, and write the `docs/spec/` contract where 4 belongs" —
which makes the reporter half right rather than simply wrong.

**`private_include_dirs` was reported as drift and is more than that.** The first
draft had it as one missing entry in one list. It is a divergence between two
grammars for the same axis, where the *other* grammar accepts the key and its
commit message records why — which moves it from "someone forgot" to "two lists
that must agree do not".

**D12 and D13 were proposed as an offline gate and a stamp relocation, and the
real defect was underneath both.** The first draft never asked what
`install_packages` returns. Asking produced §12.1: the answer is parsed, carried
back in `CallResult`, and not read — so no gate would have helped, because the
failure it would report was already being discarded. The lesson is the one this
repository keeps recording: **verify the claimed parity by reading the precedent,
not by trusting the comment that claims it.** The comment said "the same contract
`[toolchain]` has had all along"; the precedent does not memoize and honours two
knobs, and this path does neither.

**D8 nearly created the defect it fixes.** The first draft said to validate cfg
keys in `toml.cppm` without saying where the key list comes from. That is a
fourth transcription of the vocabulary §1 is about. Corrected to export the set
from `cfgpred`.

**D9 was called architectural on the strength of one observation.** The first
draft saw that `merge_conditional_config` runs at three sites, all before the
target side resolves, and concluded that a second pass "at all three sites" was
needed and that the change was structural. Both halves were wrong. Reading
forward from the resolution point instead of backward from the merge showed a
~460-line window between it and the P1689 scan, and showed that build.mcpp
already contributes build inputs through that window by mirroring into
`packages[0]` — a shipped instance of the exact mechanism. The pass is one new
producer in an existing window, not three edits to an early phase.

The general lesson is the same one §12 records in a different key: **an ordering
constraint is a claim about two points, and reading only the earlier one gives an
answer that sounds rigorous and is wrong.** "The merge runs before the answer
exists" is true and does not imply what the first draft drew from it.

**The alternatives to D9 were stated without checking whether one already
existed.** The first draft framed item 7 as implement-or-withdraw and claimed
that withdrawal forces a package to be split per C library. Both were wrong.
`docs/14`'s "split per C library" sentence is about packages whose C libraries
require different **dependencies**, not different flags; and §6.7 shows that a
`build.mcpp` reading `MCPP_TARGET_LIBC` already expresses the `include_dirs` half
of the motivating example, through a channel whose field comments say it exists
so that "a package can ASK instead of DECLARE".

Asking what the package could already do also produced D16 and, more usefully,
D16a — `target_libc()` reports the payload reference that is fed *into*
target-side resolution while `docs/05` describes it as the resolution's output.
That is a live defect misleading authors right now, it was invisible from the
predicate framing, and it needs fixing whichever way item 8 is decided.

The shape to name: **when a feature looks unimplemented, ask what a user does
today instead.** The answer is either a workaround worth documenting, a second
mechanism that makes the feature redundant, or a defect in that second
mechanism. Here it was the third.

**And one this document could not have found without building it.** Every
version of §6 argued about whether `cfg(c-abi = …)` should exist. None of them
asked what the value on the right-hand side would be. It is `gnu` on an
ordinary Linux host — `payload_libc_name` returns the triple's env segment
verbatim — while `docs/14`'s table, the chapter documenting the feature, has
always listed `glibc`, `musl`, `picolibc`. So the implementation landed, every
mechanism worked, and the documented spelling matched nothing.

Nothing in the analysis was wrong; the question was absent. A predicate is a
comparison, and a comparison has two sides — the design work had examined the
grammar, the evaluation window, the merge, the snapshots, and never once the
vocabulary of the values. **A value that is only ever printed has no spelling
discipline, and promoting it to something users compare against imposes one
retroactively.** Worth checking whenever a reporting field becomes an input.
