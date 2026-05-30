# BMI Cache And Custom Index Build Fix

## Context

`xlings` now builds through `mcpp` and uses a local project index:

```toml
[indices]
xlings = { path = "mcpp" }
```

During a first build, `mcpp build` currently prints `Fetching custom index repos (first use)` followed by `xlings update` output, then emits warnings such as:

```text
warning: bmi cache populate failed for tinyhttps@0.2.3: expected build output missing: .../obj/tinyhttps.m.o
warning: bmi cache populate failed for xlings.libarchive@3.8.7: expected build output missing: .../obj/xxhash.o
```

The build itself can still succeed, but the dependency BMI cache is not populated for affected packages.

## Root Cause

There are two related build-tool issues.

1. Project custom-index bootstrap is too broad.

   `mcpp` seeds `.mcpp/.xlings.json` for non-builtin `[indices]` and treats the project index data as uninitialized when no `xim-indexrepos.json` or cloned `pkgs/` directory exists. It then runs `xlings update`, which prints all configured xlings index update output. For a local path index, the local `pkgs/` tree already exists and can be read directly, so the update path is mostly noise and can create extra first-build work.

2. BMI cache records object basenames instead of object paths relative to `obj/`.

   The build plan correctly writes objects under collision-avoiding subdirectories, for example:

   ```text
   obj/mcpplibs_tinyhttps_src/tinyhttps.m.o
   obj/xlings_libarchive_libarchive/xxhash.o
   obj/xlings_zlib_zlib-1.3.2/compress.o
   ```

   The cache populate list currently stores only `cu.object.filename()`, so it later searches for:

   ```text
   obj/tinyhttps.m.o
   obj/xxhash.o
   obj/compress.o
   ```

   Those files do not exist, so `populate_from()` returns `expected build output missing`.

There is also a cache-key correctness gap: all dependency cache entries currently use the default index name. Custom-index dependencies should carry their real namespace/index identity so cache entries from different indices do not collide.

## Fix Plan

1. Add regression coverage first.
   - Add an e2e regression that builds a local-path custom-index dependency with duplicate object basenames.
   - Assert that local-path indices do not trigger first-use `xlings update` noise.
   - Assert that the cache manifest is written under the custom index name and preserves nested object paths.
   - Assert that a second cold build reuses the dependency BMI cache.

2. Fix artifact path recording.
   - When collecting dependency artifacts in `src/cli.cppm`, store object paths relative to `ctx.outputDir / "obj"` instead of only the object filename.
   - Keep BMI filenames as basenames because compiler BMI output is intentionally flat in the active BMI directory.

3. Fix dependency cache identity.
   - Use the dependency namespace or custom index name in `CacheKey::indexName` instead of always using the global default index.
   - Preserve builtin/default behavior for existing `mcpplibs` packages.

4. Reduce local custom-index update noise.
   - Treat local `{ path = ... }` indices as initialized when the source path has a `pkgs/` directory.
   - Keep remote custom indices on the existing project `xlings update` path.

5. Verify.
   - Run targeted BMI cache unit tests.
   - Run the broader unit suite or project test command available in the repository.
   - Run a local `mcpp build` scenario when feasible to ensure the original warning no longer appears.

## Dynamic Notes

- If tests reveal that `CacheKey::indexName` is used as a namespace rather than a repository identity, prefer the package namespace as the cache partition and document the remaining repository-revision hardening as follow-up.
- If local path indices still need xlings project metadata for install operations, skip only the update call, not `.mcpp/.xlings.json` seeding.
- Implemented regression as `tests/e2e/49_bmi_cache_nested_custom_index.sh` because the defect is in CLI dependency resolution plus build-plan artifact collection, not only the lower-level cache copy functions.
- Local verification: `mcpp build` succeeded, then `MCPP=target/x86_64-linux-gnu/4d24c8b57fdbbbb4/bin/mcpp bash tests/e2e/49_bmi_cache_nested_custom_index.sh` returned `OK`.
- `mcpp test -- --gtest_filter=BmiCache.*` initially exposed an unrelated local environment issue: `~/.mcpp/registry/data/xpkgs/xim-x-binutils/2.42` only had `.xpkg.lua` and no `bin/as`. Re-running with a temporary `MCPP_HOME` pointed at the complete installed mcpp registry passed: 15 test binaries ok, 0 failed.
