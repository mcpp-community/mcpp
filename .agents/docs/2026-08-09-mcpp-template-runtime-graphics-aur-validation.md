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

No RED/GREEN evidence has been claimed yet.

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
