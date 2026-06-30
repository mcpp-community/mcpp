# Workspace-aware `mcpp test` + zero-shell self-contained mcpp-index (Design)

Two coupled deliverables toward "把对库的测试做成真实工程、基于 mcpp 自包含、消除所有
shell":

- **Phase 1 (repo `mcpp`)** — make `mcpp test` workspace-aware: `mcpp test -p <member>`,
  `mcpp build|test --workspace`, and fix bare `mcpp test` at a workspace root.
- **Phase 2 (repo `mcpp-index`)** — turn each `tests/<lib>/` into a real mcpp test
  project (`mcpp test` + behavioral assertions), migrate the `smoke_compat_*.sh`
  heredocs into them, and delete every shell driver — CI becomes `mcpp test --workspace`.

Pairs with the typed `import mcpp;` build library
(`2026-06-30-l3-build-mcpp-implementation-design.md` §forward-note, task #20) as the
**0.0.79 "workspace-test + build.mcpp library"** release.

---

## 1. Problem (grounded in the code)

`mcpp build` is workspace-aware; `mcpp test` is not.

- `mcpp build -p <member>` works (`src/cli.cppm` build subcommand has
  `.option("package").short_name('p')`; `cmd_build` copies it to
  `BuildOverrides::package_filter` — `src/cli/cmd_build.cppm`; `prepare_build`
  resolves the member and **reassigns `root` to the member dir** —
  `src/build/prepare.cppm:415-509`).
- `mcpp test` has **no** `-p` / `--workspace` (`src/cli.cppm` test subcommand).
- **The bug:** `run_tests` (`src/build/execute.cppm:421-459`) calls
  `find_manifest_root()` → gets the **workspace** root → `expand_glob(*root,
  "tests/**/*.cpp")` → collects *every member's* `tests/.../main.cpp` →
  `seenNames.insert("main")` collides → `error: duplicate test name 'main'`. Test
  discovery runs **before** any member selection, on the unscoped workspace root.

Proof:
```
$ mcpp build   # virtual ws root → "Workspace building member 'tests/examples/build-mcpp'"  (picks ONE, arbitrarily)
$ mcpp test    # virtual ws root → error: duplicate test name 'main'                          (globs ALL, unscoped)
```

## 2. Architecture principle

**Scope, don't duplicate.** The root-test bug is *missing member scoping*, not a
missing second globber. The fix is to run the **same** member resolution `build`
uses *before* test discovery, so `tests/**/*.cpp` is globbed from the **member**
dir. `--workspace` is **thin orchestration**: a loop over members that calls the
**existing per-member pipeline** once each — no parallel build/test path.

Concretely, extract today's inline member logic into one shared helper and have
both `build` and `test` consume it:

```cpp
// src/build/workspace.cppm  (new, or fold into project.cppm)
namespace mcpp::build {
  struct MemberRef { std::string name; std::filesystem::path dir; };
  // Resolve which members a command acts on, from the (already-loaded) root
  // manifest + overrides. Encapsulates the selection rules in §3.
  std::expected<std::vector<MemberRef>, std::string>
  select_members(const manifest::Manifest& root, const std::filesystem::path& rootDir,
                 const BuildOverrides& ov, bool wantAll);
}
```

- `-p <name>` → 1 member (match by directory basename **or** member path — the
  rule already in `prepare.cppm:430-447`).
- `--workspace` (or bare at a **virtual** workspace) → **all** members.
- Bare at a **rooted** workspace (`[package]` + `[workspace]`) → the root package
  (members only via `--workspace`), matching today.

`prepare_build`'s existing per-member switch (load member manifest, `merge_workspace_deps`,
inherit toolchain/target/indices, `root = memberDir`) stays as the **single-member**
mechanism; `select_members` just decides the *set*, and the orchestration loop calls
`prepare_build(package_filter = member.name)` per member.

## 3. Behavior matrix (decision)

| invocation | virtual workspace | rooted workspace | plain package |
|---|---|---|---|
| `mcpp build` / `mcpp test` | **all members** | root package | the package |
| `... -p <m>` | member `<m>` | member `<m>` | error (no members) |
| `... --workspace` | all members | all members (+root) | error |
| `mcpp run` (+`-p`) | default/`-p` member | root/`-p` | the package |

**Change from today:** bare build/test at a *virtual* workspace goes from "pick one
arbitrary member" → **all members** (Cargo-consistent; the pick-one was a
placeholder). `run` stays single-target (no `--workspace`). This is the only
behavior change; verify/adjust the workspace e2e (`tests/e2e/*workspace*`).

## 4. Phase 1 — implementation touch points

1. **CLI** (`src/cli.cppm`, test subcommand ~L244-256): add
   ```cpp
   .option(cl::Option("package").short_name('p').takes_value().value_name("NAME")
       .help("Run tests only for the named workspace member"))
   .option(cl::Option("workspace").help("Run tests for all workspace members"))
   ```
   and add `.option("workspace")` to the **build** subcommand (~L215-237).
2. **Parse** (`src/cli/cmd_build.cppm`): in `cmd_test`, copy `-p` →
   `ov.package_filter`; read `--workspace` → `bool all`. Same `--workspace` read in
   `cmd_build`.
3. **`select_members` helper** (new `src/build/workspace.cppm`): the §2 logic,
   extracted from `prepare.cppm:422-453` so both paths share it.
4. **`run_tests` scoping** (`src/build/execute.cppm:421-459`): before the
   `expand_glob`, resolve the member root for the single-member case (when
   `package_filter` set, via the shared helper) and glob from the **member dir**,
   not the workspace root. This kills the "duplicate main" bug by *scoping*.
5. **`--workspace` orchestration** (`src/cli/cmd_build.cppm`): when `all`, call
   `select_members(..., wantAll=true)` and loop — `cmd_build` runs
   `prepare_build`+`run_build_plan` per member; `cmd_test` runs `run_tests` per
   member (each with `package_filter = member.name`). Aggregate exit codes
   (first non-zero wins; print a per-member summary). Continue-on-failure so one
   member's failure still reports the rest.
6. **Tests** (`tests/e2e/90_workspace_test.sh`): a virtual workspace with 2 members
   each having `tests/<distinct>.cpp` asserting behavior; assert
   `mcpp test -p memberA` runs only A's tests, `mcpp test --workspace` runs both
   (no duplicate-stem error), bare `mcpp test` at the root runs both.
7. **Docs** (`docs/06-workspace.md` + zh): document `-p`/`--workspace` for
   build/test and the bare-at-root semantics.

## 5. Phase 2 — mcpp-index zero-shell restructure (repo `mcpp-index`)

### Target layout
```
mcpp-index/
  pkgs/                       # the index (recipes) — unchanged
  mcpp.toml                   # [workspace] members=tests/* ⊕ [indices] compat={path="."}
  tests/
    cjson/
      mcpp.toml               # [package] cjson-tests; [dependencies] compat.cjson
      tests/parse.cpp         # mcpp test — assert cJSON_Parse fields
    eigen/  tests/matmul.cpp   # assert A*B values
    nlohmann.json/ tests/roundtrip.cpp   # parse→dump→parse equality
    openblas/                 # [target.'cfg(windows)'.dependencies] openblas; tests/dgemm.cpp asserts [19 22;43 50] (no-op gate off-Windows)
    build-mcpp/               # build.mcpp generates a source; tests/ assert it linked
```

### Test mechanism
mcpp's native `mcpp test` discovers `tests/**/*.cpp`, builds each as a test binary,
runs it (non-zero = fail). Members use plain assertion `.cpp` (a failing `assert`/
non-zero return), no external framework required — keeps members dependency-light
and host-portable. (gtest remains available via `[dev-dependencies]` if a member
wants richer output, but is not required.)

### Migration (delete all shell)
- Convert each `smoke_compat_*.sh` heredoc body into the matching member's
  `tests/*.cpp` (the heredocs already contain the usage snippets).
- **Delete** `tests/smoke_compat_{core,imgui,archive,imgui_window}.sh`,
  `tests/smoke_imgui_module.sh`, `tests/smoke_compat_portable.sh`,
  `tests/run_workspace.sh`, `tests/run_example.sh`.
- **`validate.yml`** collapses: the per-suite jobs become one matrix or a single
  `mcpp test --workspace` step:
  ```yaml
  - run: mcpp test --workspace        # linux; the whole index, self-contained
  ```
  Windows/macOS jobs run the same command (platform-gated members like openblas
  self-gate via `cfg(windows)`; the `detect` job can still narrow to `-p <lib>` on
  PRs touching one recipe). No shell driver remains.

### Result
The index repo is simultaneously (a) the package index and (b) a mcpp workspace
whose members really *use and test* every recipe — driven entirely by `mcpp`, no
`.sh`. "基于 mcpp 自包含" achieved.

## 6. Typed `import mcpp;` library (task #20) — DEFERRED (own follow-up)

A typed module **bundled in the mcpp binary**, emitting the existing stdout
`mcpp:` wire protocol, implemented with C-level I/O so neither it nor `build.mcpp`
needs `import std;`. **De-risking confirmed the module itself works** (GCC 16):

```cpp
module; #include <cstdio>
export module mcpp;
export namespace mcpp {
  inline void cxxflag(const char* f)   { std::printf("mcpp:cxxflag=%s\n", f); }
  inline void link_lib(const char* n)  { std::printf("mcpp:link-lib=%s\n", n); }
  // ...
}
```
`g++ -std=c++23 --sysroot=… -fmodules -c mcpp.cppm -o mcpp.o` → `gcm.cache/mcpp.gcm`
+ `mcpp.o`; then `g++ … -fmodules -x c++ build.mcpp -x none mcpp.o -o bin` (run
**from the dir containing `gcm.cache/`**) → `import mcpp;` resolves; the binary
emits the directives. No `import std;` needed.

**Why deferred (not in 0.0.79):** sound delivery needs two pieces this release
won't rush:
1. **cwd-capable spawn.** GCC C++ finds modules only via `gcm.cache/<m>.gcm`
   *relative to the compile CWD* — the named `-fmodule-file=mcpp=<path>` form is
   rejected ("valid for D but not for C++"), and `-fmodule-output=` is absent on
   GCC 16. So compiling `build.mcpp` must run with `cwd = target/.build-mcpp/`;
   `platform::process::capture_exec` has no `cwd` parameter yet (add one — child
   `chdir` after fork on POSIX / `lpCurrentDirectory` on Windows).
2. **Clang path.** Clang uses `.pcm` + `--precompile` + `-fmodule-file=mcpp=<pcm>`
   (different ABI/flags), untested here. Without it, `build.mcpp` using
   `import mcpp;` on a Clang host (macOS/Windows) would fail to compile — a partial
   feature. Both compiler paths must land together.

Plus: embed the module source in the binary, compile it **once** into
`target/.build-mcpp/` keyed on the toolchain (cache; don't rebuild), then convert
the build.mcpp docs/examples/test + the mcpp-index `build-mcpp` member to
`import mcpp;`. Tracked as task #20 for a focused 0.0.80. The string-protocol
substrate already ships `build.mcpp` today, so this is a pure ergonomic layer — no
functionality is blocked by deferring it.

## 7. Sequencing & releases

1. **mcpp PR A** — Phase 1 (workspace-aware test) + e2e + docs → **release 0.0.79**
   → full ecosystem loop (mirror → index → verify → pin).
2. **mcpp-index PR B** — Phase 2 restructure on 0.0.79; delete all shell; CI =
   `mcpp test --workspace` → its CI green → merge. (Primary user goal: zero-shell,
   self-contained, `-p`-addressable index.)
3. **(follow-up) mcpp 0.0.80** — typed `import mcpp;` library (§6), with the
   cwd-capable spawn + Clang `.pcm` path; then convert build.mcpp docs/examples and
   the mcpp-index `build-mcpp` member to `import mcpp;`.

## 8. Risks / soundness notes

- **Behavior change** (bare virtual-ws build/test → all members): the one
  compatibility-affecting change; covered by updating the workspace e2e and is the
  more-correct semantics. `run` is untouched (single-target).
- **No parallel code path**: `--workspace`/`-p` are orchestration over the proven
  per-member `prepare_build`/`run_tests`; the helper centralizes selection so the
  rules can't drift between `build` and `test` (the class of bug we're fixing).
- **Aggregate reporting**: `--workspace` must continue-on-failure and print a
  per-member pass/fail summary, else one red member hides the rest.
- **Per-member dep isolation** is the reason for the workspace (vs one mega
  `[package]`): members resolve independently, so conflicting transitive deps and
  platform-only libs (openblas/Windows) don't couple. Recorded as the rejected
  single-project alternative.
