---
subject: review
status: landed
---

# #690: self-review before release, engine and ecosystem

- Pull request: mcpp-community/mcpp#691 (2026.9.25.1)
- Design: [2026-09-25-issue-690-workspace-build-inheritance-consistency.md](2026-09-25-issue-690-workspace-build-inheritance-consistency.md); plan: [2026-09-25-issue-690-implementation-plan.md](2026-09-25-issue-690-implementation-plan.md)
- Method: the full `src/` and `modules/` diff read against the principles of the design record (P1 to P8), each finding checked by a measurement or a code citation, and the ecosystem consumers enumerated.

---

## 1. Findings of the review, and what was done

| # | Finding | Evidence | Resolution |
|---|---|---|---|
| R1 | The archive commit made by `mcpp publish` honoured `commit.gpgSign`. On a host that signs commits, the object would carry a signature timestamp, which breaks reproducibility, or it would fail where no key is available. | Code: `git commit-tree` without `--no-gpg-sign`. After the fix, measured with `commit.gpgsign = true` and `gpg.program = /bin/false` on the repository: `publish --dry-run` succeeds twice with the same sha256. | `--no-gpg-sign` added. |
| R2 | The manifest blob was hashed with the repository's filters, and the scratch file lies inside the repository (`target/dist`). | Code: `git hash-object -w` without `--no-filters`. | `--no-filters` added. |
| R3 | The publisher carried a second copy of the inheritable key set, written in parallel with `kWorkspaceBuildKeys` because the two tasks started from the same base. | Code: `kStringVectors`, `kPathVectors`, `kScalars` in `normalize.cppm`. | Replaced by `kWorkspaceBuildKeys`. |
| R4 | A host-tool sub-build receives its dependency's manifest preloaded. After W1 that manifest is already inherited, and the sub-build's member branch would have inherited it a second time. | Code: `prepare_build` member branch before W4. | The preloaded manifest is treated as effective. Only its workspace is recorded, for the membership test of its own dependencies. |
| R5 | Two e2e scripts from the parallel tasks used the number 770, the same as the lead's. | Directory listing. | Renumbered to 772 and 773. |
| R7 | A host-tool sub-build merged its dependency's conditional sections a second time (design record F12). Found as an open item of this review and then measured: `-include once.h` from a matching section reached the tool twice, on 2026.9.24.1 as well. | e2e 775 fails on 2026.9.24.1 and passes on the candidate; the package builds on its own (control). | The sub-build receives `Manifest::beforeConditionalMerge`. |
| R6 | A member inside an index archive (a Form A descriptor pointing at `*/<dir>/mcpp.toml`) did not inherit its archive's workspace, which is the same position independence gap as F4 for a third route. | Measured, design record F11 and e2e 774. | Applied through `inherit_as_workspace_member`, bounded by the install root. |

## 2. Principles, checked

| Principle | Holds because | Residual |
|---|---|---|
| P1 position independence | One function (`inherit_as_workspace_member`) serves the sibling, git and index-archive routes. The root inherits at load through the effective loader. e2e 770 and 774 count the words in each position. | None known. |
| P2 merge, normalise, snapshot | `makePackageRoot` performs no merge and refuses unfolded `defines`. | The layer-conditional second pass folds again by design, and the keyed fold removes superseded words across passes (unit test `SecondPassRemovesAWordTheFirstPassFolded`). |
| P3 one source of truth | One key table, one loader, one inheritance function. | `inherit_workspace_build` in `project.cppm` still lists its fields explicitly. The unit test `EveryTableRowIsParsedAndInherited` fails if the table and that function disagree. |
| P4 scope | No include directory is broadcast; the std module, the scanner and every rule read per-unit includes. | A consumer-supplied configuration header has no channel. None is needed today (section 3). |
| P5 cache soundness | `kCacheEpoch` 4; a dependency's command is shown identical under two roots (e2e 765 (c)). | None known. |
| P6 published form | Normalised manifest, reproducible archive, `.orig` kept, released 2026.9.24.1 client builds it (e2e 772 with `MCPP_BOOT`). | `[indices]` inherited from the workspace is not written into the published manifest; a member whose dependencies resolve through a workspace-declared index publishes a manifest that names no index for them. This matches a non-member package, which also publishes no `[indices]`. |
| P7 loud invariants | Internal error text follows `plan.cppm`'s form. | None. |
| P8 measured blast radius | Section 3. | The full mcpp-index matrix runs after release, on the pin-moving pull request. |

## 3. Ecosystem review

- **mcpp-index members.** 166 test members. None declares `include_dirs` (the two matches are comments), so W6 removes nothing a member relied on. The root workspace declares no `[workspace.build]`, so W1 changes no member's flags. 16 members with C sources, `defines` and include directories (`cjson`, `zlib`, `brotli`, `c-ares`, `expat`, `libpng`, `sqlite3`, `pcre2`, `spdlog-compiled`, `fmtlib.fmt`, `yaml-cpp`, `xxhash`, `md4c`, `libffi`, `mimalloc`, `yyjson`) pass `mcpp test -p` with the candidate.
- **Index packages with a workspace in their archive.** 22 are installed on the measuring machine. None declares `[workspace.package]`, `[workspace.build]` or `[workspace.dependencies]`, so F11's inheritance changes none of them.
- **Descriptor comments.** `compat.godot-cpp.lua:163` states that "a consumer-side header shadow never reaches" the package. That statement was false for an uncached compile before this release and is true after it; no descriptor change is needed.
- **Published clients.** The normalised manifest uses only keys 2026.9.24.1 accepts. `!NAME` in `defines` needs 2026.9.25.1, and docs/04 states the floor.
- **Caches.** Epoch 4 orphans every dependency-cache entry once. mcpp-index's CI caches already key on `MCPP_VERSION`, so the pin move costs the same cold run it always does.
- **xlings.** The internal pin moves to 2026.9.20.1 (openxlings/xlings#610). `check_version_pins.sh` passes. The bootstrap `.xlings.json` does not move (review decision 2026-09-25).

## 4. Open items outside #690

- **The e2e harness still shares existing payload versions by link.** An in-place rewrite of an existing payload by a test still reaches the developer's registry (#293, first shape).

---

## 5. Landed (2026-09-25)

- mcpp#691 squash-merged as f176abdc; release 2026.9.25.1 (run 36072031458): all four builds, the sealed manifest and publish-ecosystem succeeded.
- GitCode `xlings-res/mcpp` 2026.9.25.1 was filled by a local top-up as each archive appeared. GET 200 at the upstream size for every archive and sidecar, and the four archives are byte-identical to the GitHub release (sha256 compared).
- openxlings/xim-pkgindex#873 merged (89645f8f). Its four sha256 values equal those of the downloaded archives. The published index artifact `xim-index-89645f8` (sha256 matching its pointer) names `latest = 2026.9.25.1` in all three platform blocks.
- mcpplibs/mcpp-index#465 merged (93781cf4): validate.yml pins 2026.9.25.1, and `latest_mcpp = 2026.9.25.1`. The full workspace sweep (`workflow_dispatch`, run 36075006806) passed 27 of 27 jobs on attempt 2. Attempt 1 had two shards cancelled at the 90-minute limit on the cold cache, with every member they reached passing, and one transient GitCode reachability failure whose asset was re-read and matched its sha256. The published artifact `mcpp-index-93781cf` carries the new `latest_mcpp`.
- CI on f176abdc: every workflow is green except the xcode-27 legs of ci-macos, ci-macos-e2e and ci-fresh-install. All three fail at `ld64.lld: could not load TAPI file ... unknown architecture` (mcpp#669, known red on main before this change).
- Sandbox, `.agents/docs/2026-09-25-issue-690-verify.sh`, each version in a fresh subos with the CN mirror set for xlings and mcpp, installed from the index:
  - 2026.9.25.1: passes 11, fails 0, skips 0.
  - 2026.9.24.1 (control): passes 4, fails 7. The seven failures are exactly the CHANGE sections, including 5b, where the fresh home reproduced the cross-project cache poisoning (`compare=1`).
- The measuring machine's `~/.mcpp` glibc 2.44.3 payload, corrupted by the harness defect, was removed and is reinstalled on demand. With the harness fix, test runs no longer write into the developer registry.
