---
subject: plan
status: active
---

# #690: implementation plan

- Design: [2026-09-25-issue-690-workspace-build-inheritance-consistency.md](2026-09-25-issue-690-workspace-build-inheritance-consistency.md)
- Base: `origin/main` b4824697 (mcpp 2026.9.24.1). Release target: **mcpp 2026.9.25.1**.
- Delivery: one pull request in `mcpp-community/mcpp` that carries every workstream, followed by the release chain in the repositories listed in section 5. The design record's per-workstream pull requests (its section 7) are merged into one, as requested in review. Each workstream keeps its own criterion inside that pull request.

---

## 1. Tasks

| Id | Workstream | Content | Files (owner) |
|---|---|---|---|
| T1 | W1 | Build inheritance moves to the dependency load site. `makePackageRoot` stops inheriting. Snapshot post-condition. | `src/build/prepare.cppm` (lead) |
| T2 | W2 | One `[workspace.build]` key table. `ios_deployment_target` accepted. | `modules/manifest/src/toml.cppm` (lead) |
| T3 | W3 | A git-hosted member inherits its repository's `[workspace.build]`. | `src/build/prepare.cppm` (lead) |
| T4 | W4 | `load_effective_manifest`. `publish`, `pack`, `emit xpkg` and `toolchain list` read it. | `src/project.cppm`, `src/publish/pipeline.cppm`, `src/pack/route.cppm`, `src/toolchain/lifecycle.cppm` (agent B) |
| T5 | W5 | Normalised published manifest, sibling-edge rewrite, reproducible archive from git objects, descriptor `deps` from the normalised manifest. | `src/pm/publisher.cppm`, `modules/libs/src/toml.cppm` (serialiser), new `src/publish/normalize.cppm` (agent B) |
| T6 | W6 | Root include broadcast removed (C, C++, NASM). `kCacheEpoch` 3 to 4. Consumer-include advice on a dependency's missing header. Command-identity unit test. | `src/build/flags.cppm`, `src/build/cache_key.cppm`, `src/build/ninja_backend.cppm`, `src/build/execute.cppm` (agent A) |
| T7 | W7 | `[build] defines` is a keyed set. `!NAME` removes an inherited entry. | `src/build/prepare.cppm` (`fold_build_defines_into_flags`, lead) |
| T8 | harness | `_inherit_toolchain.sh` links payloads per version, so a version installed by a test lands in the test's home and not in the developer's registry (#293 recurrence, measured 2026-09-25). | `tests/e2e/_inherit_toolchain.sh` (lead) |
| T9 | docs | `docs/07`, `docs/04`, `docs/11` (English and 简体中文), SPEC-004 §8 amendment and §9, CHANGELOG, version 2026.9.25.1. | lead, after T1 to T8 |

## 2. Dependencies

```
T2 ----------------------------+
T1 --> T3 --> T7 --------------+--> T9 --> PR --> CI --> review --> merge --> release chain
T4 --> T5 ---------------------+
T6 ----------------------------+
T8 ----------------------------+
```

- T3 edits the same load site as T1 and follows it.
- T7 edits the fold that T1's post-condition names, and follows T1.
- T5 needs T4's loader.
- T6 and T8 are independent of every other task.
- Agents A (T6) and B (T4, T5) work in their own worktrees from the plan commit. The lead integrates by cherry-pick. File ownership in section 1 is exclusive. A task that needs a line in another owner's file reports it instead of editing it.

## 3. Review angles

Each angle names the property it requires and the evidence that shows it.

| Angle | Requirement | Evidence |
|---|---|---|
| Architecture | One pipeline per member. One effective-manifest loader. Private requirements stay inside their package. | Removal of the `makePackageRoot` inheritance block and of the `flags.cppm` broadcast. Every raw `manifest::load` of a project manifest is reviewed and recorded. |
| Stability | No silent state. Internal invariants fail loudly. | Snapshot post-condition. Cache epoch increment. Harness fix T8. |
| Simplicity | No new manifest keys. One new value form (`!NAME`). | Diff of the parser's key table. |
| User experience | Every refusal names the file, the key and the fix. The dependency-header advice names the consumer directory. | e2e output assertions. |
| Compatibility | Older engines read every published normalised manifest. No previously valid manifest is refused by the build. | e2e with the published 2026.9.24.1 binary as a consumer of a normalised archive. |
| Cross-platform | Windows, macOS and Linux CI. Path relativisation and archive creation use git and `std::filesystem`, not host tools with platform-specific flags. | CI matrix. |
| Consistency | `-p lib` and `-p app` compile `lib` identically. A git consumer compiles the member as its repository does. | e2e 321 counts. Git-member e2e. |
| Seamless upgrade | No user action is required. The cost is one cold dependency-cache rebuild (epoch 4) and one rebuild of fingerprints whose duplicated flags disappear. | CHANGELOG. |
| Test coverage | Every workstream has a criterion that fails with its fix removed. | Section 4. |

## 4. Criteria per task

| Task | Criterion | Kind |
|---|---|---|
| T1 | e2e 321: each workspace word exactly once, in both positions, in `.cpp` and `.c` entries, workspace before member. | e2e |
| T1 | Snapshot with unfolded `defines` yields the internal error. | unit |
| T2 | A workspace declaring every table row parses, and every value is inherited. | unit |
| T3 | `file://` repository member: the consumer build passes an `#error` guard, and the flag occurs once. | e2e |
| T4 | In a member that omits `version`, `emit xpkg` and `publish --dry-run` succeed. In a member without `[toolchain]`, `toolchain list` marks the workspace's toolchain. | e2e |
| T5 | The archive's `mcpp.toml` carries the inherited values and a version edge. `mcpp.toml.orig` is present. The descriptor lists the sibling. A consumer of the unpacked archive builds, under the new engine and under 2026.9.24.1. An edge without `version` is refused with the sibling's version in the message. Two runs produce byte-identical archives. The serialiser round-trips. | e2e, unit |
| T6 | A root private `limits.h` containing `#error` does not reach a path dependency or an index dependency. The dependency's compile command is identical under two roots that differ in include settings. The root's units carry each root directory once. The advice names the root directory. | e2e, unit |
| T7 | `defines = ["X=1"]` inherited and `defines = ["X=2"]` in the member emit only `-DX=2`. `!X` emits no `-DX`. | e2e, unit |
| T8 | After a test installs a version that the developer registry lacks, the developer registry still lacks it. | e2e harness check |

## 5. Release chain

1. PR in `mcpp-community/mcpp` with the version 2026.9.25.1 in `mcpp.toml` and `modules/versioning/src/version.cppm`, and the internal xlings pin at the latest xlings release (2026.9.20.1). All required checks green. Squash merge.
2. `origin/main` HEAD run green.
3. `release.yml` dispatched. Each archive is uploaded to GitCode with the local tools the moment it appears (`tools/mirror_res.sh` from xlings, GitCode leg only). GET 200 and byte comparison on both mirrors.
4. `openxlings/xim-pkgindex` bump pull request merged. The published index artifact is read to confirm `latest`.
5. `.xlings.json` (the mcpp that builds mcpp) is not moved: review of 2026-09-25 asked for no separate bootstrap pull request unless a build needs it.
6. `mcpp-community/mcpp-index`: one pull request that moves its CI pins to 2026.9.25.1, which runs every member on every platform against the release. Before the release, 16 members were run locally with the candidate binary (all passed), and no member declares `include_dirs`, so the W6 change has no consumer-side reliance to break there.
7. Sandbox verification: `.agents/docs/2026-09-25-issue-690-verify.sh` in `xlings subos use <n> --sandbox --cmd ...`, with the CN mirror set for both xlings and mcpp inside the sandbox, and the same script against 2026.9.24.1 as the control.
