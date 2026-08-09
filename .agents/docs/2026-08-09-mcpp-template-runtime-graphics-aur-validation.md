# mcpp Template, Runtime, Graphics, and AUR Validation Ledger

> Started: 2026-08-09 Asia/Shanghai
>
> Implementation issue: https://github.com/mcpp-community/mcpp/issues/398
>
> This ledger separates baseline, RED, GREEN, CI, release, and public-ecosystem evidence. A running, skipped, cancelled, or superseded job is never recorded as pass.

## 1. Scope and repository boundaries

The mcpp implementation is developed in one branch and one pull request:

- worktree: `/home/speak/.config/superpowers/worktrees/mcpp/template-runtime-graphics-aur`
- branch: `feat/template-runtime-graphics-aur`
- base: `mcpp-community/mcpp main@80291ca01a982c1e8c00e43bfa97ffe68516e6d7`
- focused issue: `mcpp-community/mcpp#398`
- umbrella issue: `mcpp-community/mcpp#397`

External repositories retain their own history and review boundaries. Their implementation, if required after the mcpp release, uses separate repository PRs; it does not create a second mcpp implementation PR.

The main mcpp checkout contains user-owned untracked design/triage files. This worktree was created from `origin/main`; no tracked or untracked file in the main checkout is moved or deleted.

## 2. Live baseline

Captured after fresh `git fetch origin main --prune` on 2026-08-09:

| Repository | local HEAD | `origin/main` | local state | latest release |
|---|---|---|---|---|
| `mcpp-community/mcpp` | `80291ca01a982c1e8c00e43bfa97ffe68516e6d7` | same | isolated feature worktree clean before docs | `v2026.8.8.4`, published 2026-08-08 10:51:31Z |
| `openxlings/xlings` | `2913a0949af3b26192a6c1d8f12b78f976bea6b8` on `feat/version-grammar` | `f203b6b9a5e3e0d3f707468ed56cdb8b50cc7acc` | dirty user checkout; must use a new worktree | `v2026.8.9.2`, published 2026-08-08 20:38:57Z |
| `mcpplibs/mcpp-index` | `b86fc7c0c80a93f4ccdf797c13ec0cca2557d6eb` | same | clean main | no GitHub release list |
| `openxlings/xim-pkgindex` | `576ef09b69becefca00de8c94c6ba17d5cdb1ee4` | `e8029381beb2e0c83c4ec4318f01bd18c523890a` | local main behind; must use a new worktree | no GitHub release list |

Open PR inventory at capture time:

- mcpp: #387, #372 (Draft), #353, #351 (Draft), #272.
- xlings: none.
- mcpp-index: #150 (Draft), #61.
- xim-pkgindex: none.

Local executables visible before the isolated cold build:

- PATH mcpp: `mcpp 2026.8.6.2`.
- latest existing source-built binary: `mcpp 2026.8.8.4` at `target/x86_64-linux-gnu/09887d532ce30543/bin/mcpp` in the main checkout.
- PATH xlings: `xlings 2026.8.9.2`.

The release version proposed for this implementation is `2026.8.9.1`; the tag is re-queried immediately before changing `mcpp.toml`.

## 3. Immutable boundary snapshots

Approved design hashes:

- focused design: `e06821b73102049e1f2184a2967e5dd81dfb6f05389a763dad326d139d0e3f25`
- parent convergence design: `ad93db80a3222f6536629a15d9b5ba430b222e179b91de01131c619ecbd3bd65`
- initial implementation plan: `aea2963f593ef00e71b2876f52a773014a28098f513f82260063dcd336acb09e`

`scripts/aur/mcpp-m/**` baseline:

| File | SHA256 |
|---|---|
| `.SRCINFO` | `9d8b8279aadfa6b1915850fbbfe33c2ef237456f94dd706e726e3169b60be0f8` |
| `PKGBUILD` | `545fe0de51f0cf7e5979871ba8f8d6cd3233c485338a9768f57e53c0a6f8fa7f` |
| `mcpp.sh` | `cbab68984b02c415f8ae42bf9417647b842e900fbf7bf292067e4a384b924f8f` |
| sorted aggregate | `afb8a647e04483a86985119e07086016f49d55f177ee6257094c336d226113c6` |

Host xlings configuration snapshot:

- scope: sorted SHA256 rows for `/home/speak/.xlings/.xlings.json` and `subos/*/.xlings.json`; contents were not copied.
- aggregate: `f218aadf3792ee815c8535ce0ca0bb53f634fecbdbc5d0d47db442052d786d1b`.

All stateful verification uses a separate temporary root and must reproduce both aggregate hashes before completion.

## 4. Baseline verification

Isolated root: `/tmp/mcpp-focused-398.XVUEhz`.

| Gate | Command shape | Status | Evidence |
|---|---|---|---|
| cold source build | isolated `MCPP_HOME`, `XLINGS_HOME`, vendored xlings 2026.8.9.2; `mcpp build --no-cache` | PASS | cold bootstrap installed `glibc@2.44` and resolved `gcc@16.1.0`; release build finished in 64.80s; binary `target/x86_64-linux-gnu/72abd390cce53924/bin/mcpp` reports `2026.8.8.4` |
| unit suite | fresh baseline binary, isolated homes; `mcpp test` | PASS | `68 passed; 0 failed`, 89.06s total |
| template E2E | isolated homes; `MCPP=<baseline binary> bash tests/e2e/69_package_templates.sh` | PASS | exit 0, `OK` |
| runtime E2E | isolated homes; scripts 74, 166 and 201 | PASS with one fixture rerun | #74 proved payload glibc 2.44 PT_INTERP; #166 proved private glibc env inclusion/exclusion; #201 passed both sysroot/payload-first modes after using a non-`/tmp` isolated HOME |
| AUR tests | inventory + `bash -n scripts/aur/update.sh` | PASS for syntax; coverage absent | baseline has no reconciler tests; Task 9 adds fixture coverage |
| diff check | `git diff --check origin/main...HEAD` before edits | PASS | no tracked diff before documentation was added |

### Baseline fixture diagnosis

The first #201 run exited 1 because the isolated `MCPP_HOME` itself was `/tmp/mcpp-focused-398.XVUEhz/.mcpp`. The test intentionally rejects every RUNPATH beginning with `/tmp/` as evidence of another temporary home, so it classified the current, valid isolated home as pollution. The observed two paths were the current glibc and gcc payload directories, not a stale foreign home.

The hypothesis was tested by hard-link cloning the disposable state into the non-`/tmp` isolated root `/home/speak/.cache/mcpp-focused-398.IE23if`, letting the normal fixup rebind paths, and running the unmodified script again. It exited 0 and proved both link modes load nothing from the host. No product or test source was changed for this baseline result.

## 5. RED/GREEN ledger

Each behavior entry is appended with:

1. exact test command;
2. RED exit code and why the old code failed;
3. production change;
4. GREEN exit code and relevant assertions;
5. refactor/full-gate result.

### 5.1 Exact PackageSelector and dependency identity (Task 2)

| Gate | RED evidence | Production change | GREEN evidence |
|---|---|---|---|
| shared parser | Selector tests initially could not compile because `PackageSelector` / `parse_package_selector` did not exist; unsafe-segment rows then exposed missing character validation | Added one O(length), no-I/O parser, default `mcpplibs` normalization, dotted formatter, and diagnostic-only legacy coordinate helper | `test_pm_compat`: 17 tests passed, including bare/dotted/nested/invalid rows |
| manifest/xpkg parsing | Updated manifest expectations produced 3 failures under ordered-candidate behavior; invalid TOML/xpkg selectors were previously not hard parse failures | Routed direct, nested, feature, target and xpkg dependency inputs through the shared exact parser | `test_manifest`: 148 passed; `test_pm_index_route`: 7 passed; `test_pm_package_fetcher`: 10 passed |
| add/exact sibling | `12_add_command.sh` first showed `capi.lua` written/resolved with the old default-prefix candidate and no migration diagnostic; a later RED left both legacy `"acme.util"` and canonical `[dependencies.acme] util` rows | `mcpp add` now accepts canonical `[ns.]name@version`, retains `ns:name` as a warned one-release alias, probes only one PackageId, removes an equivalent legacy flat row, and writes/upserts the canonical table | `12_add_command.sh`: `capi.lua` cannot be stolen by `(mcpplibs.capi,lua)`; warning names both exact selectors; legacy source shape becomes one canonical row; malformed input leaves TOML byte-stable |
| scoped remove | Added same-name `[dev-dependencies]` before `[dependencies]`; RED removed the dev row and left the regular row | Limited flat removal to the exact `[dependencies]` body and kept nested namespace removal exact | Rebuilt source binary; the same E2E preserves the dev row and removes only the regular dependency |
| exact build miss | `162_bare_name_namespace_scope.sh` initially passed an exact miss into install-time compatibility retries | Every version dependency is identity-validated before install; miss reports the exact coordinate and did-you-mean remains diagnostic-only | `162`: bare gtest stays `(mcpplibs,gtest)` while explicit `(compat,gtest)` resolves; `165` proves the descriptor supplies the exact wire namespace |
| lock migration | New `203_exact_selector_lock_migration.sh` first failed because Form-B synthesis received the ambiguous manifest map key instead of the resolved short name | Existing v2 lock identity anchors an old dotted selection for one release train; unlocked selection never falls back; Form-B synthesis uses canonical short name | `203`: warning names old/new selectors, build/run use the locked old identity, rewritten lock retains its namespace |

Task 2 refactor/full gates on the isolated source binary
`target/x86_64-linux-gnu/aee81584bf66d3f8/bin/mcpp`:

- `mcpp build --no-cache`: PASS, release build completed in 64.12s.
- `mcpp test --no-cache`: PASS, **68 passed; 0 failed**, 86.37s.
- focused E2E set `12, 27, 62, 63, 78, 79, 162, 165, 203`: all PASS/OK.
- changed shell scripts `bash -n`: PASS.
- `git diff --check`: PASS.

### 5.2 Typed exact TemplateSpec and deterministic provider selection (Task 3)

| Gate | RED evidence | Production change | GREEN evidence |
|---|---|---|---|
| grammar | New `test_scaffold` could not build because typed `parse_template_spec` / `select_template` APIs did not exist | Added fixed-order `:` then `@` parsing over the shared PackageSelector; exact safe version keys; atomic tname validation; one-release `pkg:` list marker | 9 scaffold tests pass for every valid design row and empty/double-delimiter/range/unsafe failures |
| exact provider identity | Updated E2E is red on the baseline binary at the first new exact-identity diagnostic; baseline implementation probes empty/compat short names and cannot retain an explicit foreign/nested PackageId | Registry template fetch now uses one normalized coordinate through `IndexRoute::lookup_descriptor`; no short-name or compat probe remains | E2E same-short `(mcpplibs,tpl-demo)` / `(acme,tpl-demo)` selects acme exactly; nested `(mcpplibs.capi,lua)` survives output and injected selector |
| deterministic version | Baseline accepted only its ad-hoc version flow and exposed no payload provenance | Shared SemVer resolver chooses latest stable, explicit prerelease is validated, indirect aliases are refused, and xpkg version entries retain declared SHA256 | E2E omits `2.0.0-rc.1` for default selection, accepts its exact pin, rejects `latest`, rejects unpublished `9.9.9`, and prints descriptor/payload provenance |
| default template | Old code failed whenever no `default=true` existed, including a package with exactly one template | Shared selection implements sole-template auto-default, unique explicit default, explicit tname, and deterministic provider errors | Unit/E2E cover sole auto-default, multiple ambiguous choices, duplicate defaults, missing templates directory, and explicit disambiguation |
| migration/CI surfaces | Legacy `pkg:` silently meant listing and native fresh-install only exercised a bare short name | `pkg:` warns with copyable `--list-templates`; fresh-install Linux/macOS/Windows lanes now use explicit `mcpplibs.imgui` and assert resolved identity | Local legacy-list E2E passes; native lanes are configured and remain pending PR/latest-head CI evidence |

Task 3 local gates on source binary
`target/x86_64-linux-gnu/94da92f90aedbe7f/bin/mcpp`:

- source build: PASS, full release rebuild completed in 72.92s.
- `mcpp test --no-cache`: PASS, **69 passed; 0 failed**, 33.07s.
- `test_scaffold`: 9/9 behavior rows PASS; `test_manifest`: 148/148 PASS.
- `69_package_templates.sh`: PASS with exact/default/nested/alias/provider/wire assertions.
- `bash -n tests/e2e/69_package_templates.sh` and `git diff --check`: PASS.

### 5.3 Portable, durable, transactional scaffolding (Task 4 / #380)

| Gate | RED evidence | Production change | GREEN evidence |
|---|---|---|---|
| portable project name | New scaffold tests failed to compile because `mcpp.scaffold.project_name` did not exist | Added an ASCII package/directory validator and isolated Windows device-basename policy under `mcpp.platform.project_name`; `cmd_new` validates before config/index/network/staging work | Unit rows accept bare/qualified names and reject empty, absolute, separators, dot components, C0/DEL, Windows-invalid/device names, trailing dot/space, and the legacy `PROJECT` marker |
| single-pass rendering | Baseline only repeatedly replaced the literal `PROJECT`/two legacy variables | Added the complete eight-field `RenderVars` vocabulary, strict unknown/unterminated-token diagnostics, and append-only single-pass rendering | Unit test proves every canonical project/provider identity renders and an inserted `{{template.name}}` sequence is not rescanned; E2E renders qualified/nested identities |
| exact dependency edit | New manifest-editor tests failed to compile because `upsert_dependency_text` did not exist; baseline scaffold used substring presence/insertion | Added one parsed, source-preserving dependency editor shared by `mcpp add` and scaffold injection; it writes default tables or `[dependencies.<namespace>]` plus short key and reparses/verifies the exact PackageId/version/features | `test_manifest` 150/150; same-short `compat.widget` survives while exact `acme.widget` is injected idempotently; add E2E remains green |
| transaction and I/O | Baseline created the final directory before rendering and ignored several stream/filesystem results; exclusive platform rename API was absent | Both builtin/package paths write a same-parent `.mcpp-new-*` tree, close+sync files, validate the complete manifest, then commit with Linux `renameat2(RENAME_NOREPLACE)`, macOS exclusive rename, or Windows write-through no-replace move | 18 scaffold tests cover rollback plus deterministic read/write/copy/rename failures; E2E 204 covers path escape, unknown token, type collision, symlink rejection, no final tree, and no staging residue |

Task 4 focused local evidence on source binary
`target/x86_64-linux-gnu/94da92f90aedbe7f/bin/mcpp`:

- source build: PASS, release rebuild completed in 47.89s after the shared manifest editor change.
- full unit suite: PASS, **69 test binaries passed; 0 failed**, 16.77s cached refactor gate.
- `test_scaffold`: **18/18** behavior tests PASS; `test_manifest`: **150/150** PASS.
- `02_new_build_run.sh`: PASS, generated builtin project builds and runs.
- `12_add_command.sh`, `69_package_templates.sh`, and `204_new_transactional_scaffold.sh`: all PASS/`OK`.
- `mcpp-m` aggregate remains `afb8a647e04483a86985119e07086016f49d55f177ee6257094c336d226113c6`.
- host xlings config aggregate remains `f218aadf3792ee815c8535ce0ca0bb53f634fecbdbc5d0d47db442052d786d1b`.
- macOS/Windows native compilation and filesystem semantics remain pending latest-HEAD PR CI; they are not inferred from this Linux run.

### 5.4 Root-local RuntimeSelection and immutable RuntimeBinding (Task 5)

| Gate | RED evidence | Production change | GREEN evidence |
|---|---|---|---|
| two-mode selection | `test_runtime_selection` first failed to compile because `mcpp.xlings.runtime_selection` did not exist | Added presence-preserving manifest parsing and one pure `McppDefault`/`NamedSubos` selector with portable name validation and an explicit owner root | 10 runtime selection/binding tests pass; absence, explicit `default`, workspace override, standalone member and invalid names are distinct |
| exact binding | Binding tests then failed to compile because `mcpp.platform.runtime_binding` did not exist | Resolves only the configured default or exact root-owned named SubOS; missing directory, missing block, empty runtime and incompatible schema are hard errors; canonical provider/runtime/env snapshot gets a stable contract hash | Unit tests prove missing named cannot fall back, same-content `el8`/`dev` hashes differ, and serialized cache round-trip verifies its hash |
| lifecycle/cache | Baseline execute path derived a compiler-owned SubOS, honored `MCPP_SUBOS_DIR`, and re-read `.xlings.json` independently on full/fast run | BuildContext/BuildPlan/toolchain/fingerprint/cache carry one snapshot; fast paths require it and invalidate on contract mtime; full and cached run/test resolve environment only from that snapshot | E2E 200 proves first/cached run equality, ignored legacy override, mutation-triggered rebuild, old-cache miss and missing-contract error |
| root-local scope | A member/dependency declaration could become the manifest visible after workspace substitution or nested prepare | Selection happens before member substitution; workspace root materializes its own xlings config; dependency/tool sub-builds inherit the consumer snapshot and never consult their manifest SubOS | E2E 205 proves active-state independence, explicit-default identity, workspace-root ownership, standalone-member ownership and non-transitive path dependency behavior |
| xlings env semantics | New tests showed `set` overwrote both a caller value and an explicitly empty variable | `set` now follows xlings presence semantics; `prepend` retains ordered element-wise de-duplication | `test_subos_info`: 17/17 pass, including caller/empty preservation |

Task 5 local gates on source binary
`target/x86_64-linux-gnu/e49880a389812d1b/bin/mcpp`:

- source build: PASS, incremental release rebuild completed in 47.59s.
- full unit suite: PASS, **70 test binaries passed; 0 failed**, 87.78s.
- `test_manifest`: **151/151**; `test_runtime_selection`: **10/10**; `test_subos_info`: **17/17**; `test_fingerprint`: **8/8**.
- E2E `02`, `88`, `200`, `205`, `12`, `69`, and `204`: all PASS/`OK`.
- changed shell scripts `bash -n` and `git diff --check`: PASS.
- `mcpp-m` aggregate remains `afb8a647e04483a86985119e07086016f49d55f177ee6257094c336d226113c6`.
- host xlings config aggregate remains `f218aadf3792ee815c8535ce0ca0bb53f634fecbdbc5d0d47db442052d786d1b`.
- Linux behavior is locally exercised. The platform-native branch stores `runtimeId` while leaving `libc`/ELF fields empty off Linux; native macOS/Windows compilation remains a latest-HEAD PR CI gate, not a local claim.

### 5.5 Exact runtime payload and Linux ELF physics (Task 6 / #392 / #396)

| Gate | RED evidence | Production change | GREEN evidence |
|---|---|---|---|
| exact payload | `test_elf_runtime` initially failed to compile because `mcpp.platform.elf_runtime` did not exist; the old post-install helper returned the first loader-bearing glibc directory | Added exact `glibc@<version>` parsing/lookup, explicit malformed/missing/stale errors and fixup revision `hermetic-4-exact-runtime`; RuntimeBinding now carries canonical loader/lib dir and optional `host_glibc` | RuntimePayload unit rows prove 2.44 wins with 2.39 also installed and every malformed/absent identity refuses instead of falling back |
| internal ELF facts | The same RED had no typed artifact facts or GNU version-table reader | Added bounded ELF64-LE parsing of PT_INTERP, PT_DYNAMIC, DT_RPATH/RUNPATH/NEEDED and GNU verneed/verdef, preserving loader search order and invoking no shell tool | Synthetic stripped-shape fixture returns exact interpreter, ordered runpaths, needed soname, `GLIBC_2.40` need and `GLIBC_2.44` definition; text/truncated inputs refuse |
| Rule B | No post-link seam compared the emitted interpreter with the libc actually resolved through the closure | Closure resolution canonicalizes artifacts/providers and rejects host/private or multi-private libc mixtures against the selected RuntimeBinding | Unit rows reject interpreter/libc and transitive two-libc mixtures and accept one selected payload; E2E 206 forces host PT_INTERP + private libc and gets a canonical Rule-B hard failure |
| Rule A | No code compared a host DSO's GNU symbol floor with the selected libc exports | Validator compares every requester `GLIBC_*` need to the selected libc definitions; proven higher floors fail, missing closure data is typed inconclusive | Unit rows reject 2.42 > 2.39, accept equal/lower, and keep missing GPU closure inconclusive; E2E 206 links real host `libtinfo` under glibc 2.44 as the safe #392-shaped control |
| cache/doctor | A hot fast path could not know whether its artifact had ever been checked; doctor reparsing would observe a different current host | Stored verdict is keyed by artifact stat plus RuntimeBinding contract; hot paths require current PASS and compare pre/post-Ninja stats; mismatch/inconclusive records remain sticky without reparsing; doctor reads the stored record | Self-host artifact verdict is `pass`; bare build finishes in 0.01s with artifact/verdict stat unchanged; E2E 206 proves PASS no-op, cached mismatch remains red with unchanged verdict, and doctor prints the stored PASS |
| platform boundary | Linux-specific rules had no typed cross-platform seam | Validator compiles as a typed no-op off Linux; RuntimeBinding leaves ELF/libc-only data absent on macOS/Windows | Linux unit/E2E pass; native macOS/Windows compile/behavior remains an explicit PR CI gate, not inferred locally |

Task 6 focused local evidence on source binary
`target/x86_64-linux-gnu/665e89fc782f5d0f/bin/mcpp`:

- two-stage self-host: first new implementation build PASS; RuntimeBinding-aware rebuild PASS in 65.06s with canonical glibc 2.44 loader/lib directory and host floor 2.39.
- `test_elf_runtime`: **10/10** tests PASS (exact payload, parser, Rule A/B, inconclusive).
- `206_runtime_binding_physics.sh`: PASS for real host-DSO control, post-link mismatch, sticky cached mismatch, doctor reuse, and hot zero-rewrite cache.
- regressions `200_subos_env_reaches_program.sh`, `205_root_local_subos.sh`, and `02_new_build_run.sh`: PASS/PASS/OK.
- full unit/refactor gate: PASS, **71 test binaries passed; 0 failed**, 50.99s (49.30s build + 1.54s run).
- `bash -n` for changed E2E and `git diff --check`: PASS.
- #392/#396 remain open until the merged release artifact repeats the exact acceptance E2E; local source evidence alone is not closure evidence.

### 5.6 Provider-neutral runtime provenance and LinkIntent (Task 7)

| Gate | RED evidence | Production change | GREEN evidence |
|---|---|---|---|
| structured descriptor contract | `test_runtime_contract` initially failed to compile because `RuntimeRequirement`, `RuntimeArtifact`, `LinkIntent`, canonical `PackageId`, and the resolver API did not exist; the first xpkg GREEN attempt then exposed a dangling `string_view` over a temporary parser body; review RED proved an explicit relative library file remained relative to the consumer instead of its declaring package | TOML and xpkg descriptors now read the same typed requirement/artifact/link-intent grammar; explicit library paths resolve at the declaring package; legacy `library_dirs`, `dlopen_libs`, and `capabilities` remain readable for one train and normalize only to runtime-search/requirement facts | `test_runtime_contract` TOML/xpkg rows pass and assert every structured channel, package-root path anchoring, plus retained legacy fields |
| exact ownership | The old runtime model retained only short provider strings and treated a requirement as a weak provider candidate | Build-plan resolution stamps every requirement/artifact/provider with namespace, name, version, and source provenance; only `provides` creates a descriptor provider; an explicit override is exact or uniquely compatible and missing/ambiguous cases hard-fail | Unit rows keep `alpha.backend@2.0.0` and `beta.backend@3.0.0` distinct and prove the requester is never self-promoted; E2E 66 rejects the legacy weak-requester override |
| xlings-selected facts | RuntimeBinding had no generic provider/artifact payload and its hash/cache could not preserve a host selection; review RED then proved equal-prefix facts were not ordered by source/provenance/digest and could make the contract hash input-order-dependent | Optional schema-1 `subos_info.runtime_contract` facts resolve `${subosdir}`/relative paths once, sort by their complete identity into RuntimeBinding, participate in its hash/serialization, and precede descriptor fallback facts in BuildPlan | `test_subos_info` and `test_runtime_selection` round-trip provider identity, artifact provenance, digest and host fingerprint; the full-fact ordering row and merge row prove deterministic selected-before-fallback behavior |
| link/search separation | Legacy `library_dirs` entered both `-L` and rpath; there was no typed seam to assert all output formats | Added a pure four-flavor LinkIntent renderer: runtime dirs never enter link lookup, ELF alone gets `-rpath-link`, Mach-O gets rpath/frameworks, PE gets link dirs while deploy files remain copy edges | `test_link_model_runtime_dirs` passes all flavor assertions; E2E 62 inspects a real Linux link and finds RUNPATH without the forbidden `-L` token |
| durable explanation | Runtime explanation could resolve again and provider provenance was not present in the stored schema; an added RED also showed `required=false` ABI facts were incorrectly promoted into hard compatibility constraints | `resolution.json` schema 2 stores the exact binding, canonical requirements/providers/artifacts, link intent, search mechanism, and synchronized post-link verdict; optional requirements remain recorded but stay out of hard ABI/doctor projections; `mcpp why runtime` reads only the newest stored file | E2E 207 builds successfully with an intentionally mismatched optional `abi:musl`, preserves same-short identities/artifact provenance, corrupts `mcpp.toml`, installs fake probe commands, then proves `why runtime` still succeeds without launching them |
| ownership boundary | No automated fence prevented mcpp from acquiring graphics-provider or hardware-probe policy | Added a source gate over executable code branches and process-launch neighborhoods; diagnostics redirect provider/host re-diagnosis to `xlings doctor` | Static gate passes over the complete `src/` tree; E2E 207 proves the live why path is probe-free |
| runtime-physics compatibility | Synthetic named SubOS fixtures contained only metadata, which became physically false after Task 6 began enforcing loader/libc closure | E2E 200/205 now reuse the managed default's exact libc/loader view while varying only environment and root ownership; no product fallback was introduced | E2E 200, 205, and 206 pass with truthful bindings; Task 6 Rule A/B enforcement remains intact |

Task 7 focused local evidence on the latest self-hosted source binary
`target/x86_64-linux-gnu/4091fed9f558ec8a/bin/mcpp`:

- self-host source builds: PASS; initial RuntimeBinding implementation completed in 68.66s and the post-review deterministic/optional/path refinement rebuild completed in 78.83s.
- focused units: `test_runtime_contract` **5/5**, `test_link_model_runtime_dirs` **5/5**, `test_runtime_selection` **10/10**, and `test_subos_info` **18/18** PASS.
- focused E2E `62_runtime_library_dirs.sh`, `66_runtime_provides.sh`, and `207_runtime_contract_provenance.sh`: PASS/`OK`.
- RuntimeBinding regressions `200_subos_env_reaches_program.sh`, `205_root_local_subos.sh`, and `206_runtime_binding_physics.sh`: PASS.
- full no-cache unit gate: PASS, **72 test binaries passed; 0 failed**, 109.40s (107.48s build + 1.78s run); after the review refinements, the complete latest-source refactor gate repeated **72/72** in 14.58s (12.98s build + 1.48s run).
- changed shell scripts `bash -n`, executable mode for E2E 207, and `git diff --check`: PASS.
- `mcpp-m` aggregate remains `afb8a647e04483a86985119e07086016f49d55f177ee6257094c336d226113c6`.
- host xlings config aggregate remains `f218aadf3792ee815c8535ce0ca0bb53f634fecbdbc5d0d47db442052d786d1b`.
- Linux semantics are locally exercised. Mach-O and PE flag spelling has pure unit coverage on Linux; native macOS/Windows compilation and behavior remain latest-HEAD PR CI gates, not inferred passes.

### 5.7 Immutable release desired state (Task 8)

| Gate | RED evidence | Production change | GREEN evidence |
|---|---|---|---|
| manifest contract | The initial 11-case suite produced 12 failing assertions because no generator existed; fixtures cover duplicate normalized platform/arch, missing sidecars, payload/sidecar hash disagreement, draft/prerelease releases, wrong tag/version, missing required assets, duplicate release names, and invalid commit identity | Added schema-1 generation from the GitHub release JSON plus downloaded payloads; four current platforms are mandatory, every additional versioned platform payload is included, every digest is recomputed, and output is sorted deterministically | `test_release_manifest.py` passes **12/12**; randomized release-API asset order produces byte-identical output and every manifest hash equals the independently recomputed fixture payload hash |
| future platform closure | Review fixture added a valid FreeBSD/riscv64 versioned payload; the first implementation stayed green while silently omitting that row | Primary-platform discovery is provider-neutral and version-exact rather than a fixed four-platform allowlist; the four present release targets remain a required floor | The focused future-platform test changed RED-to-GREEN and asserts the complete row and SHA256 occur in the manifest |
| immutable publication | Previously each platform uploaded independently and downstream jobs inferred completeness from timing and filenames | New `release-manifest` job waits for all four uploaders, validates non-draft/non-prerelease identity, uploads only when absent, refuses a byte-different existing manifest, polls public API visibility, then refetches all public assets and regenerates/compares the manifest | Ruby/Psych parses `release.yml` and confirms six jobs, `publish-ecosystem` depends on `release-manifest`, Python bytecode compilation passes, and `git diff --check` is clean; live GitHub upload/refetch remains a PR/release CI gate |
| maintainer contract | Release docs had no machine-readable completeness boundary or reproducible local audit command | English and Chinese release docs define schema, row scope, immutability/rerun behavior, public refetch command, and checklist gates | Documentation examples invoke the same checked-in generator and explicitly recompute payload hashes rather than trusting manifest or sidecar values |

Task 8 focused local evidence:

- RED: missing generator yielded 12 assertion failures across 11 initial tests; the later additional-platform review row independently failed because FreeBSD/riscv64 was omitted.
- GREEN: `python3 tests/scripts/test_release_manifest.py` reports **12 tests OK** in 0.309s; `python3 -m py_compile` succeeds for generator and tests.
- Determinism: the complete fixture shuffles API inventory with seed 398, invokes the CLI twice to separate output files, and compares exact bytes.
- Public-inventory replay: downloaded the eight real versioned payload/sidecar assets from stable `v2026.8.8.4`, resolved tag commit `55a39d90fe98b5475fc394ac8487fe6804b2b84f`, and generated twice byte-identically. The four-row manifest SHA256 is `6614ba2db65c8c28cb5a9d3466bd6cf3007634221da76d25216785061c647edb`; every real sidecar and recomputed payload digest agreed.
- Workflow structure: Ruby/Psych parses `.github/workflows/release.yml`; all six jobs are present and ecosystem publication is gated by the immutable manifest job.
- Public GitHub upload/refetch and eventual-consistency polling cannot be truthfully claimed from local fixtures; they remain explicit latest-HEAD release CI evidence.

## 6. Pull request and CI

Not published yet. This section will record PR URL, local/remote HEAD, review state, all latest-head job IDs and terminal conclusions.

## 7. Merge and release

Not started. This section will record the authorized bypass squash commit, tag, GitHub release, release workflow/job IDs, asset inventory, checksums, `mcpp-release.json`, and GitCode mirror evidence.

## 8. AUR mcpp-bin

Current live snapshot from the preceding audit:

- GitHub desired release: `v2026.8.8.4`.
- AUR observed `mcpp-bin`: `2026.8.1.1-1`.
- latest failed AUR run: `31254088758`, after version and both checksums resolved, then AUR SSH returned maintenance.

No old failed run will be rerun. Recovery uses the reconciler from a fixed main/release state. `mcpp-m` is outside every verdict and publish path.

## 9. Cross-repository and public ecosystem verification

Not started. This section will record mcpp-index/xim/xlings PRs and commits, index artifact hashes, GitCode ranged/full downloads, cold-home package lifecycle, canonical template build/run, multiple SubOS/glibc bindings, and graphics PASS/FAIL/NOT_EXERCISED provenance.
