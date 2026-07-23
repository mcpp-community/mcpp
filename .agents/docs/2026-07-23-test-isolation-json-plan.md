# mcpp test: per-test isolation, filter, JSON output — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `mcpp test` becomes exercise-grade: one broken test no longer kills the run (per-test compile isolation), tests in subdirectories get path-based names, a positional pattern filters which tests run, and `--message-format json` emits machine-readable NDJSON — the four upstream capabilities required by the d2mcpp "exercises as tests" redesign (`d2mcpp/.agents/docs/2026-07-23-exercises-as-tests-design.md` §4), each shaped as a generic cargo/ctest-parity feature.

**Architecture:** `run_tests` (src/build/execute.cppm) moves from "one monolithic backend build, then run everything" to a two-phase flow: Phase A builds package-level artifacts (every non-TestBinary link unit) — failure there is a *package error*; Phase B builds each test's own ninja target individually via a new `BuildOptions::ninjaTargets` knob and runs it on success — a compile failure is attributed to *that test only*. Test names become `tests/`-relative paths (`00-intro/0`, not stem), which is what makes chapter subdirectories usable at all (duplicate stems currently hard-error). The filter selects at Phase B only — the plan always contains all tests so `compile_commands.json` stays complete for clangd.

**Tech Stack:** C++23 modules (mcpp builds itself with mcpp), ninja backend, bash e2e scripts under `tests/e2e/`.

## Global Constraints

- Repo: `/home/speak/workspace/github/mcpp-community/mcpp`, branch `feat/test-isolation-json` off `main`.
- Code comments and commit messages in English (repo convention; d2mcpp/d2x use Chinese, mcpp does not).
- No new dependencies. JSON emission via a local `json_escape` (same shape as the one in `src/cli/cmd_xpkg.cppm:28`).
- In `--message-format json` mode stdout carries NDJSON **only**; all human/status text must be absent (via `ui::set_quiet`) or on stderr. Consumers ignore unparseable lines, but we do not rely on that.
- JSON record schema is fixed by the d2mcpp design doc §4.3 (field names verbatim): `test`, `status` (`pass|compile_fail|run_fail`), `exit_code`, `signal`, `compile_output`, `run_output`; package-level record `{"error":"package",...}`.
- Every task ends with: mcpp still builds itself (`mcpp build` in repo root exits 0).
- Build/verify commands assume the locally installed `mcpp` bootstrap binary in PATH; e2e scripts take the freshly built binary via `MCPP=<path>`.

**Dev-loop commands used throughout:**

```bash
cd /home/speak/workspace/github/mcpp-community/mcpp
mcpp build                                   # self-build (glibc, fast) — dev loop
BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)   # freshly built binary
```

---

### Task 0: Branch

**Files:** none

- [ ] **Step 1: Create branch**

```bash
cd /home/speak/workspace/github/mcpp-community/mcpp
git checkout main && git pull --ff-only
git checkout -b feat/test-isolation-json
```

---

### Task 1: Path-based test names (subdirectory support)

Currently `run_tests` names each test by file **stem** and hard-errors on duplicates, so `tests/00-a/0.cpp` + `tests/01-b/0.cpp` cannot coexist. Name them by `tests/`-relative path without extension instead. `target_output` (src/build/plan.cppm:192) maps name → `bin/<name>`, so `00-a/0` yields nested `bin/00-a/0` — ninja creates output directories itself; no change needed there.

**Files:**
- Modify: `src/build/execute.cppm` (run_tests, discovery block around line 705–722)
- Test: `tests/e2e/118_test_subdir_names.sh` (new)

**Interfaces:**
- Produces: test names of the form `<relpath-no-ext>` with `/` separators (`generic_string()`), e.g. `00-intro/0`, `smoke` for a top-level `tests/smoke.cpp` (unchanged for flat layouts). Task 3/4/5 rely on `LinkUnit::targetName` carrying exactly this name.

- [ ] **Step 1: Write the failing e2e test**

Create `tests/e2e/118_test_subdir_names.sh` (executable):

```bash
#!/usr/bin/env bash
# requires:
# tests/ subdirectories: same stem in two dirs must coexist, names are relative paths
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests/00-a pkg/tests/01-b
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "subdirs"
version = "0.1.0"
standard = "c++23"
EOF
cat > tests/00-a/0.cpp <<'EOF'
int main() { return 0; }
EOF
cat > tests/01-b/0.cpp <<'EOF'
int main() { return 0; }
EOF

out=$("$MCPP" test 2>&1) || { echo "mcpp test failed: $out"; exit 1; }
[[ "$out" == *"00-a/0 ... ok"* ]] || { echo "missing path-based name 00-a/0: $out"; exit 1; }
[[ "$out" == *"01-b/0 ... ok"* ]] || { echo "missing path-based name 01-b/0: $out"; exit 1; }
[[ "$out" == *"2 passed"* ]]     || { echo "expected 2 passed: $out"; exit 1; }
echo OK
```

- [ ] **Step 2: Run it to verify it fails**

```bash
chmod +x tests/e2e/118_test_subdir_names.sh
mcpp build && BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)
MCPP="$PWD/$BIN" bash tests/e2e/118_test_subdir_names.sh
```

Expected: FAIL with `duplicate test name '0'`.

- [ ] **Step 3: Implement path-based naming**

In `src/build/execute.cppm` `run_tests`, replace the naming block (currently `auto name = f.stem().string();` plus the duplicate-stem error) with:

```cpp
    // 2. Synthesize a Target for each test file.
    //    Name = path relative to tests/, extension dropped, '/' separators —
    //    so tests/00-a/0.cpp and tests/01-b/0.cpp coexist as '00-a/0' and
    //    '01-b/0' (stems alone would collide). Flat layouts keep their old
    //    names ('tests/smoke.cpp' → 'smoke').
    std::vector<mcpp::manifest::Target> testTargets;
    std::set<std::string> seenNames;
    for (auto& f : testFiles) {
        auto rel  = std::filesystem::relative(f, testRoot / "tests");
        auto name = rel.replace_extension("").generic_string();
        if (!seenNames.insert(name).second) {
            mcpp::ui::error(std::format(
                "duplicate test name '{}' (two test files map to the same name)", name));
            return 2;
        }
        mcpp::manifest::Target t;
        t.name = name;
        t.kind = mcpp::manifest::Target::TestBinary;
        // Relative to the member/package root prepare_build will operate on.
        t.main = std::filesystem::relative(f, testRoot).string();
        testTargets.push_back(std::move(t));
    }
```

- [ ] **Step 4: Run the e2e test to verify it passes**

```bash
mcpp build && BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)
MCPP="$PWD/$BIN" bash tests/e2e/118_test_subdir_names.sh
```

Expected: `OK`. Also confirm no regression on flat layout: `MCPP="$PWD/$BIN" bash tests/e2e/100_feature_sources_test_mode.sh` (existing test-mode e2e) still passes.

- [ ] **Step 5: Commit**

```bash
git add src/build/execute.cppm tests/e2e/118_test_subdir_names.sh
git commit -m "feat(test): name tests by tests/-relative path, enabling subdirectories"
```

---

### Task 2: `BuildOptions::ninjaTargets` — build a subset of the plan

Generic backend knob: explicit ninja targets (LinkUnit `output` paths, relative to the plan's outputDir). Empty = current behavior (build everything). This is the mechanism Task 3 uses for both phases.

**Files:**
- Modify: `src/build/backend.cppm:12-16` (BuildOptions)
- Modify: `src/build/ninja_backend.cppm:1104-1230` (NinjaBackend::build)

**Interfaces:**
- Produces: `BuildOptions::ninjaTargets` (`std::vector<std::string>`); when non-empty, `NinjaBackend::build` passes them as ninja goal targets and `producedArtifacts` lists only the matching link units. Diagnostics on failure are naturally scoped to what was built.

- [ ] **Step 1: Extend BuildOptions**

In `src/build/backend.cppm`:

```cpp
struct BuildOptions {
    bool                        verbose       = false;
    bool                        dryRun       = false;
    std::size_t                 parallelJobs = 0;
    // Explicit ninja goal targets (LinkUnit::output paths, relative to the
    // plan's outputDir). Empty = build the full plan (default behavior).
    std::vector<std::string>    ninjaTargets;
};
```

- [ ] **Step 2: Thread targets into the ninja invocation**

In `src/build/ninja_backend.cppm` `NinjaBackend::build`, after the `-j` option block (line ~1196), append:

```cpp
    // Explicit goal targets: ninja builds only these outputs (and their
    // prerequisites). Used by `mcpp test` to isolate per-test compiles.
    for (auto& t : opts.ninjaTargets)
        nargv.push_back(t);
```

And scope `producedArtifacts` in the success branch (replace the existing loop at line ~1217):

```cpp
    if (ok) {
        if (opts.verbose && !out.empty())
            std::fputs(out.c_str(), stdout);
        std::set<std::string> want(opts.ninjaTargets.begin(), opts.ninjaTargets.end());
        for (auto& lu : plan.linkUnits) {
            if (!want.empty() && !want.contains(lu.output.generic_string())) continue;
            r.producedArtifacts.push_back(plan.outputDir / lu.output);
            for (auto const& alias : lu.runtimeAliases) {
                r.producedArtifacts.push_back(plan.outputDir / alias);
            }
        }
    }
```

- [ ] **Step 3: Verify no behavior change with empty ninjaTargets**

```bash
mcpp build && BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)
MCPP="$PWD/$BIN" bash tests/e2e/02_new_build_run.sh
MCPP="$PWD/$BIN" bash tests/e2e/118_test_subdir_names.sh
```

Expected: both `OK` / pass. (Dedicated behavior coverage for non-empty ninjaTargets lands with Task 3's e2e, which exercises it on both phases.)

- [ ] **Step 4: Commit**

```bash
git add src/build/backend.cppm src/build/ninja_backend.cppm
git commit -m "feat(build): BuildOptions::ninjaTargets — build an explicit subset of the plan"
```

---

### Task 3: Per-test compile isolation (two-phase run_tests)

The acceptance test from the design doc: a project with one passing, one runtime-failing, one non-compiling test must produce **three results**, with the passing one actually executed.

**Files:**
- Modify: `src/build/execute.cppm` (run_tests, build+run sections — replaces the single `backend->build` at line ~766 and the run loop at ~805)
- Test: `tests/e2e/119_test_isolation.sh` (new)

**Interfaces:**
- Consumes: `BuildOptions::ninjaTargets` (Task 2), path-based names (Task 1).
- Produces: per-test result records in an internal `struct TestResult { std::string name; enum class St { Pass, CompileFail, RunFail } status; int exitCode; std::string compileOutput; std::string runOutput; };` collected in a `std::vector<TestResult> results` — Task 5 serializes exactly these fields. Human output per test: `<name> ... ok` / `<name> ... FAIL (exit N)` / `<name> ... FAIL (compile)`. Package-level (Phase A) failure keeps today's behavior: `ui::error` + diagnostics to stderr, return 1.

- [ ] **Step 1: Write the failing e2e test**

Create `tests/e2e/119_test_isolation.sh` (executable):

```bash
#!/usr/bin/env bash
# requires:
# Per-test compile isolation: a non-compiling test fails alone; others build & run.
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "isolation"
version = "0.1.0"
standard = "c++23"
EOF
cat > tests/ok.cpp <<'EOF'
int main() { return 0; }
EOF
cat > tests/runfail.cpp <<'EOF'
int main() { return 1; }
EOF
cat > tests/nocompile.cpp <<'EOF'
int main() { D2X_YOUR_ANSWER x = 1; return x; }
EOF

set +e
out=$("$MCPP" test 2>&1)
code=$?
set -e
[[ $code -eq 1 ]] || { echo "expected exit 1, got $code: $out"; exit 1; }
[[ "$out" == *"ok ... ok"* ]]                 || { echo "passing test did not run: $out"; exit 1; }
[[ "$out" == *"runfail ... FAIL (exit 1)"* ]] || { echo "runtime failure not reported: $out"; exit 1; }
[[ "$out" == *"nocompile ... FAIL (compile)"* ]] || { echo "compile failure not isolated: $out"; exit 1; }
[[ "$out" == *"1 passed"* && "$out" == *"2 failed"* ]] || { echo "bad summary: $out"; exit 1; }

# Package-level failure stays a hard error: break a src module, all-red is wrong,
# 'build failed' is right.
mkdir -p src
cat > src/isolation.cppm <<'EOF'
export module isolation;
this is not C++;
EOF
set +e
out2=$("$MCPP" test 2>&1)
code2=$?
set -e
[[ $code2 -ne 0 ]] || { echo "package error must fail: $out2"; exit 1; }
[[ "$out2" != *"nocompile ... FAIL (compile)"* ]] || { echo "package error misattributed to tests: $out2"; exit 1; }
echo OK
```

- [ ] **Step 2: Run it to verify it fails**

```bash
chmod +x tests/e2e/119_test_isolation.sh
mcpp build && BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)
MCPP="$PWD/$BIN" bash tests/e2e/119_test_isolation.sh
```

Expected: FAIL — today the whole run dies with `error: build failed` and no test executes.

- [ ] **Step 3: Implement the two-phase flow**

In `src/build/execute.cppm` `run_tests`, replace step 5 (build everything) through step 6 (run loop) with:

```cpp
    struct TestResult {
        std::string name;
        enum class St { Pass, CompileFail, RunFail } status;
        int         exitCode = 0;
        std::string compileOutput;
        std::string runOutput;
    };
    std::vector<TestResult> results;

    auto backend = mcpp::build::make_ninja_backend();

    // Phase A: package-level artifacts (everything that is not a test
    // binary — libs, deps). A failure here is the PACKAGE's fault, not any
    // single test's: report it as a build error, never as 52 red tests.
    std::vector<std::string> pkgTargets;
    for (auto& lu : ctx->plan.linkUnits)
        if (lu.kind != mcpp::build::LinkUnit::TestBinary)
            pkgTargets.push_back(lu.output.generic_string());
    if (!pkgTargets.empty()) {
        mcpp::build::BuildOptions aOpts;
        aOpts.ninjaTargets = pkgTargets;
        auto a = backend->build(ctx->plan, aOpts);
        if (!a) {
            std::fflush(stdout);
            mcpp::ui::error(a.error().message);
            if (!a.error().diagnosticOutput.empty()) {
                std::fputs(a.error().diagnosticOutput.c_str(), stderr);
                if (a.error().diagnosticOutput.back() != '\n')
                    std::fputc('\n', stderr);
            }
            return 1;
        }
    }

    // Phase B: each test is built as its own ninja goal, so a compile
    // failure is attributed to exactly that test; the rest still build+run.
    auto t0 = std::chrono::steady_clock::now();

    auto runtimeEnvKey = mcpp::platform::env::runtime_library_path_key();
    auto runtimeEnvValue = mcpp::platform::env::prepend_path_list(
        runtimeEnvKey, ctx->plan.runtimeLibraryDirs);

    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::TestBinary) continue;

        mcpp::build::BuildOptions bOpts;
        bOpts.ninjaTargets = {lu.output.generic_string()};
        auto b = backend->build(ctx->plan, bOpts);
        if (!b) {
            std::println("{} ... FAIL (compile)", lu.targetName);
            results.push_back({lu.targetName, TestResult::St::CompileFail, 0,
                               b.error().diagnosticOutput, {}});
            continue;
        }

        auto exe = ctx->outputDir / lu.output;
        mcpp::ui::status("Running", std::format("bin/{}", lu.targetName));

        std::vector<std::string> argv;
        argv.push_back(exe.string());
        for (auto& a : passthrough) argv.push_back(a);

        std::vector<std::pair<std::string, std::string>> childEnv;
        if (!runtimeEnvKey.empty() && !runtimeEnvValue.empty())
            childEnv.emplace_back(runtimeEnvKey, runtimeEnvValue);

        // Prepend the sandbox's subos/default/bin to the CHILD PATH so test
        // binaries that shell out to bootstrapped tools (patchelf, ninja) find
        // them — applied to the child only, not via a leaky shell prefix.
        if constexpr (!mcpp::platform::is_windows) {
            if (auto xpkgs = mcpp::xlings::paths::xpkgs_from_compiler(ctx->tc.binaryPath)) {
                auto registryDir = xpkgs->parent_path().parent_path();
                auto sandboxBin  = registryDir / "subos" / "default" / "bin";
                if (std::filesystem::exists(sandboxBin)) {
                    std::array<std::filesystem::path, 1> extra{sandboxBin};
                    auto pathVal = mcpp::platform::env::prepend_path_list("PATH", extra);
                    if (!pathVal.empty()) childEnv.emplace_back("PATH", pathVal);
                }
            }
        }

        int exitCode = mcpp::platform::process::run_exec(argv, childEnv);
        if (exitCode == 0) {
            std::println("{} ... ok", lu.targetName);
            results.push_back({lu.targetName, TestResult::St::Pass, 0, {}, {}});
        } else {
            std::println("{} ... FAIL (exit {})", lu.targetName, exitCode);
            results.push_back({lu.targetName, TestResult::St::RunFail, exitCode, {}, {}});
        }
    }
```

Then rewrite step 7 (summary) on top of `results`:

```cpp
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - t0);

    int passed = 0, failed = 0;
    std::vector<std::string> failures;
    for (auto& r : results) {
        if (r.status == TestResult::St::Pass) ++passed;
        else { ++failed; failures.push_back(r.name); }
    }

    std::println("");
    if (failed == 0) {
        mcpp::ui::status("test result",
            std::format("ok. {} passed; 0 failed; finished in {:.2f}s",
                        passed, static_cast<double>(elapsed.count()) / 1000.0));
        return 0;
    }
    mcpp::ui::error(std::format(
        "test result: FAILED. {} passed; {} failed; finished in {:.2f}s",
        passed, failed, static_cast<double>(elapsed.count()) / 1000.0));
    std::println("");
    std::println("failures:");
    for (auto& n : failures) std::println("    {}", n);
    // Compile diagnostics per failed test, on stderr, after the summary —
    // grouped so a learner scrolls to exactly their test's errors.
    for (auto& r : results) {
        if (r.status != TestResult::St::CompileFail || r.compileOutput.empty()) continue;
        std::println(stderr, "\n--- {} (compile) ---", r.name);
        std::fputs(r.compileOutput.c_str(), stderr);
    }
    return 1;
```

Keep the existing M3.2 BMI-populate loop (`ctx->depsToPopulate`) — move it to right after Phase A's successful build (deps are package-level artifacts). Keep `mcpp::ui::finished("test", ...)`: call it after Phase A with `a->elapsed` when `pkgTargets` is non-empty, otherwise drop that line for this run (the per-test `... ok/FAIL` lines plus the summary carry the timing story).

- [ ] **Step 4: Run the e2e tests**

```bash
mcpp build && BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)
MCPP="$PWD/$BIN" bash tests/e2e/119_test_isolation.sh
MCPP="$PWD/$BIN" bash tests/e2e/118_test_subdir_names.sh
MCPP="$PWD/$BIN" bash tests/e2e/100_feature_sources_test_mode.sh
```

Expected: all `OK`/pass. If the tests-only fixture (no `src/`) errors in `prepare_build`, that is a bug to fix in this task — the test-mode path (src/build/plan.cppm:780-789 `inTestMode`) already skips non-test targets; adjust whatever remains that assumes a lib/bin target exists.

- [ ] **Step 5: Commit**

```bash
git add src/build/execute.cppm tests/e2e/119_test_isolation.sh
git commit -m "feat(test): per-test compile isolation — a broken test no longer kills the run"
```

---

### Task 4: `mcpp test <pattern>` filter

Positional pattern, substring match on the path-based test name, applied at **Phase B selection only** — the plan still contains every test so `compile_commands.json` stays complete (clangd depends on it; a filtered build must not clobber it down to one entry).

**Files:**
- Modify: `src/cli.cppm:252-268` (test subcommand: add positional arg)
- Modify: `src/cli/cmd_build.cppm:114-152` (cmd_test: read positional, thread through)
- Modify: `src/build/execute.cppm` (run_tests signature + Phase B selection)
- Test: `tests/e2e/120_test_filter.sh` (new)

**Interfaces:**
- Consumes: `TestResult`/two-phase flow (Task 3).
- Produces: `export struct TestOptions { std::string filter; };` in `mcpp::build` (execute.cppm), extended by Task 5 with a `format` member. New signature: `run_tests(std::span<const std::string> passthrough, BuildOverrides overrides = {}, TestOptions testOpts = {})`.

- [ ] **Step 1: Write the failing e2e test**

Create `tests/e2e/120_test_filter.sh` (executable):

```bash
#!/usr/bin/env bash
# requires:
# `mcpp test <pattern>`: substring filter on path-based test names
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests/00-a pkg/tests/01-b
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "filter"
version = "0.1.0"
standard = "c++23"
EOF
echo 'int main() { return 0; }' > tests/00-a/0.cpp
echo 'int main() { return 0; }' > tests/01-b/0.cpp

out=$("$MCPP" test 00-a 2>&1) || { echo "filtered run failed: $out"; exit 1; }
[[ "$out" == *"00-a/0 ... ok"* ]] || { echo "filtered test did not run: $out"; exit 1; }
[[ "$out" != *"01-b/0"* ]]        || { echo "filter leaked other tests: $out"; exit 1; }
[[ "$out" == *"1 passed"* ]]      || { echo "bad summary: $out"; exit 1; }

set +e
out2=$("$MCPP" test does-not-exist 2>&1)
code2=$?
set -e
[[ $code2 -eq 2 ]] || { echo "no-match should exit 2, got $code2: $out2"; exit 1; }
[[ "$out2" == *"no tests match"* ]] || { echo "missing no-match error: $out2"; exit 1; }
echo OK
```

- [ ] **Step 2: Run it to verify it fails**

```bash
chmod +x tests/e2e/120_test_filter.sh
mcpp build && BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)
MCPP="$PWD/$BIN" bash tests/e2e/120_test_filter.sh
```

Expected: FAIL — the positional is not declared, so `mcpp test 00-a` errors or ignores the argument and runs both tests.

- [ ] **Step 3: Declare the positional and thread it through**

`src/cli.cppm`, test subcommand — add before the `.option(...)` chain:

```cpp
        .subcommand(cl::App("test")
            .description("Build + run all tests/**/*.cpp (after `--`, args go to each test binary)")
            .arg(cl::Arg("pattern")
                .help("Run only tests whose name contains PATTERN (optional)"))
```

`src/cli/cmd_build.cppm` `cmd_test` — read it and pass through (both the fan-out loop and the single-package call):

```cpp
    mcpp::build::TestOptions to;
    if (parsed.positional_count() > 0) to.filter = parsed.positional(0);
```

…and change both `mcpp::build::run_tests(passthrough, mo)` / `run_tests(passthrough, ov)` calls to `run_tests(passthrough, mo, to)` / `run_tests(passthrough, ov, to)`.

`src/build/execute.cppm` — new options struct + signature:

```cpp
export struct TestOptions {
    std::string filter;   // substring match on the path-based test name; empty = all
};

export int run_tests(std::span<const std::string> passthrough,
                     BuildOverrides overrides = {},
                     TestOptions testOpts = {}) {
```

Phase B selection (inside the loop from Task 3, first lines):

```cpp
    for (auto& lu : ctx->plan.linkUnits) {
        if (lu.kind != mcpp::build::LinkUnit::TestBinary) continue;
        if (!testOpts.filter.empty()
            && lu.targetName.find(testOpts.filter) == std::string::npos) continue;
```

And before Phase B, the no-match guard:

```cpp
    if (!testOpts.filter.empty()) {
        bool any = false;
        for (auto& lu : ctx->plan.linkUnits)
            if (lu.kind == mcpp::build::LinkUnit::TestBinary
                && lu.targetName.find(testOpts.filter) != std::string::npos) { any = true; break; }
        if (!any) {
            mcpp::ui::error(std::format("no tests match '{}'", testOpts.filter));
            return 2;
        }
    }
```

Also update the "Compiling X (test)" announcement loop (step 4 of run_tests) to apply the same filter, so announcements match what actually builds.

- [ ] **Step 4: Run the e2e tests**

```bash
mcpp build && BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)
MCPP="$PWD/$BIN" bash tests/e2e/120_test_filter.sh
MCPP="$PWD/$BIN" bash tests/e2e/119_test_isolation.sh
```

Expected: both `OK`.

- [ ] **Step 5: Commit**

```bash
git add src/cli.cppm src/cli/cmd_build.cppm src/build/execute.cppm tests/e2e/120_test_filter.sh
git commit -m "feat(test): positional pattern filters tests by path-based name"
```

---

### Task 5: `--message-format json` (NDJSON)

One record per executed/attempted test + a package-level error record + a trailing summary record. stdout is pure NDJSON (`ui::set_quiet` silences status lines, which all go to stdout; errors already go to stderr). Run output is captured via `capture_exec` instead of streamed.

**Files:**
- Modify: `src/cli.cppm` (test subcommand: `--message-format` option)
- Modify: `src/cli/cmd_build.cppm` (cmd_test: parse + validate the value)
- Modify: `src/build/execute.cppm` (TestOptions::format, JSON emission, capture path)
- Test: `tests/e2e/121_test_json.sh` (new)

**Interfaces:**
- Consumes: `TestOptions` + `TestResult` (Tasks 3–4).
- Produces (the d2mcpp Provider consumes this verbatim):
  - per test: `{"test":"<name>","status":"pass|compile_fail|run_fail","exit_code":N,"signal":N|null,"compile_output":"...","run_output":"..."}`
  - package failure: `{"error":"package","compile_output":"..."}` then exit 1
  - no-match: `{"error":"no-tests-matched","filter":"..."}` then exit 2
  - summary (last line): `{"summary":{"passed":N,"failed":N,"elapsed_ms":N}}`

- [ ] **Step 1: Write the failing e2e test**

Create `tests/e2e/121_test_json.sh` (executable):

```bash
#!/usr/bin/env bash
# requires:
# --message-format json: NDJSON per test, package error record, pure stdout
set -e

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT

cd "$TMP"
mkdir -p pkg/tests
cd pkg
cat > mcpp.toml <<'EOF'
[package]
name = "jsonout"
version = "0.1.0"
standard = "c++23"
EOF
cat > tests/ok.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("hello from ok"); return 0; }
EOF
echo 'int main() { return 1; }' > tests/runfail.cpp
echo 'int main() { D2X_YOUR_ANSWER x = 1; return x; }' > tests/nocompile.cpp

set +e
out=$("$MCPP" test --message-format json 2>/dev/null)
code=$?
set -e
[[ $code -eq 1 ]] || { echo "expected exit 1, got $code"; exit 1; }

# stdout must be pure NDJSON: every line parses as a JSON object.
while IFS= read -r line; do
    [[ -z "$line" ]] && continue
    [[ "$line" == "{"* ]] || { echo "non-JSON line on stdout: $line"; exit 1; }
done <<< "$out"

echo "$out" | grep -q '"test":"ok","status":"pass"'            || { echo "missing pass record"; exit 1; }
echo "$out" | grep -q '"test":"runfail","status":"run_fail"'    || { echo "missing run_fail record"; exit 1; }
echo "$out" | grep -q '"exit_code":1'                            || { echo "missing exit_code"; exit 1; }
echo "$out" | grep -q '"test":"nocompile","status":"compile_fail"' || { echo "missing compile_fail record"; exit 1; }
echo "$out" | grep -q 'D2X_YOUR_ANSWER'                          || { echo "compile_output missing diagnostics"; exit 1; }
echo "$out" | grep -q 'hello from ok'                            || { echo "run_output not captured"; exit 1; }
echo "$out" | grep -q '"summary":{"passed":1,"failed":2'         || { echo "missing summary"; exit 1; }

# Invalid format value → usage error
set +e
"$MCPP" test --message-format yaml >/dev/null 2>&1
[[ $? -eq 2 ]] || { echo "invalid format should exit 2"; exit 1; }
set -e
echo OK
```

- [ ] **Step 2: Run it to verify it fails**

```bash
chmod +x tests/e2e/121_test_json.sh
mcpp build && BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)
MCPP="$PWD/$BIN" bash tests/e2e/121_test_json.sh
```

Expected: FAIL — `--message-format` is not a known option.

- [ ] **Step 3: Implement**

`src/cli.cppm`, test subcommand options:

```cpp
            .option(cl::Option("message-format").takes_value().value_name("FMT")
                .help("Output format: human (default) | json (NDJSON, one record per test)"))
```

`src/cli/cmd_build.cppm` `cmd_test`:

```cpp
    if (auto mf = parsed.value("message-format")) {
        if (*mf == "json")       to.format = mcpp::build::TestMessageFormat::Json;
        else if (*mf != "human") {
            mcpp::ui::error(std::format("unknown --message-format '{}' (human|json)", *mf));
            return 2;
        }
    }
```

`src/build/execute.cppm`:

```cpp
export enum class TestMessageFormat { Human, Json };

export struct TestOptions {
    std::string        filter;                              // substring match; empty = all
    TestMessageFormat  format = TestMessageFormat::Human;
};
```

At the top of `run_tests` (right after the manifest-root check):

```cpp
    const bool json = (testOpts.format == TestMessageFormat::Json);
    // JSON mode: stdout carries NDJSON only. All ui::status/info lines print
    // to stdout, so silence them wholesale; errors already go to stderr.
    if (json) mcpp::ui::set_quiet(true);
```

Local JSON helpers (file-scope, next to the other run_tests helpers; same escaping shape as `json_escape` in src/cli/cmd_xpkg.cppm:28 — keep both local, they are 15 lines and live on opposite sides of a module boundary):

```cpp
namespace {
std::string test_json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"':  out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n";  break;
            case '\r': out += "\\r";  break;
            case '\t': out += "\\t";  break;
            default:
                if (static_cast<unsigned char>(c) < 0x20)
                    out += std::format("\\u{:04x}", static_cast<unsigned char>(c));
                else out += c;
        }
    }
    return out;
}
} // namespace
```

Emission points:

1. Package-level (Phase A) failure — before `return 1`:

```cpp
        if (json)
            std::println("{{\"error\":\"package\",\"compile_output\":\"{}\"}}",
                         test_json_escape(a.error().diagnosticOutput));
```

2. No-match (Task 4 guard) — before `return 2`:

```cpp
        if (json)
            std::println("{{\"error\":\"no-tests-matched\",\"filter\":\"{}\"}}",
                         test_json_escape(testOpts.filter));
```

3. Per-test — the run step uses `capture_exec` in JSON mode so output is captured (human mode keeps `run_exec` streaming):

```cpp
        int exitCode;
        std::string runOutput;
        if (json) {
            auto rr = mcpp::platform::process::capture_exec(argv, childEnv);
            exitCode  = rr.exit_code;
            runOutput = std::move(rr.output);
        } else {
            exitCode = mcpp::platform::process::run_exec(argv, childEnv);
        }
```

…store `runOutput` in the `TestResult`, and after each result is recorded emit its record immediately (streaming NDJSON, one line per test as it finishes):

```cpp
        if (json) {
            auto& r = results.back();
            const char* st = r.status == TestResult::St::Pass ? "pass"
                           : r.status == TestResult::St::CompileFail ? "compile_fail"
                                                                     : "run_fail";
            std::string signal = (r.exitCode > 128 && r.exitCode < 128 + 65)
                ? std::to_string(r.exitCode - 128) : "null";
            std::println("{{\"test\":\"{}\",\"status\":\"{}\",\"exit_code\":{},\"signal\":{},"
                         "\"compile_output\":\"{}\",\"run_output\":\"{}\"}}",
                         test_json_escape(r.name), st, r.exitCode, signal,
                         test_json_escape(r.compileOutput), test_json_escape(r.runOutput));
            std::fflush(stdout);
        }
```

In JSON mode suppress the human per-test lines (`... ok` / `... FAIL`) — wrap each `std::println("{} ... ", ...)` in `if (!json)`.

4. Summary — in the summary section, before returning (both branches):

```cpp
    if (json)
        std::println("{{\"summary\":{{\"passed\":{},\"failed\":{},\"elapsed_ms\":{}}}}}",
                     passed, failed, elapsed.count());
```

And in JSON mode skip the human summary/failures printing and the grouped stderr compile diagnostics (they are already inside the records).

- [ ] **Step 4: Run the e2e tests**

```bash
mcpp build && BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)
MCPP="$PWD/$BIN" bash tests/e2e/121_test_json.sh
MCPP="$PWD/$BIN" bash tests/e2e/119_test_isolation.sh
MCPP="$PWD/$BIN" bash tests/e2e/120_test_filter.sh
```

Expected: all `OK`.

- [ ] **Step 5: Verify compile_commands completeness under filter (design-doc "4th item")**

```bash
cd $(mktemp -d)
mkdir -p pkg/tests/00-a pkg/tests/01-b && cd pkg
printf '[package]\nname = "cc"\nversion = "0.1.0"\nstandard = "c++23"\n' > mcpp.toml
echo 'int main() { return 0; }' > tests/00-a/0.cpp
echo 'int main() { D2X_YOUR_ANSWER x = 1; return x; }' > tests/01-b/0.cpp
"$MCPP" test 00-a >/dev/null 2>&1
grep -c '"file"' target/*/*/compile_commands.json
```

Expected: **2** entries (both tests present even though one was filtered out and the other doesn't compile). If fewer, fix in this task: compile_commands is written from the full plan in `NinjaBackend::build` (ninja_backend.cppm:1118-1120), which already runs before ninja — investigate what dropped an entry.

- [ ] **Step 6: Commit**

```bash
git add src/cli.cppm src/cli/cmd_build.cppm src/build/execute.cppm tests/e2e/121_test_json.sh
git commit -m "feat(test): --message-format json — NDJSON records for tooling (d2x provider et al.)"
```

---

### Task 6: musl static build + regression sweep + changelog

The deliverable the d2mcpp work consumes locally: a statically linked `mcpp` with the new capabilities. Static musl also makes the nested-mcpp `LD_LIBRARY_PATH` glibc-poisoning crash structurally impossible for the mcpp binary itself (the design doc's "顺带修复" — the provider-side `unsetenv` workarounds get removed later in the d2mcpp repo, not here).

**Files:**
- Modify: `CHANGELOG.md` (new Unreleased entries)
- Modify: any `docs/*.md` lines describing `mcpp test` behavior (found via grep below)

- [ ] **Step 1: Full e2e regression with the glibc dev binary**

```bash
cd /home/speak/workspace/github/mcpp-community/mcpp
mcpp build && BIN=$(ls -d target/x86_64-linux-gnu/*/bin/mcpp | head -1)
MCPP="$PWD/$BIN" bash tests/e2e/run_all.sh 2>&1 | tail -20
```

Expected: same pass set as `main` (run once on `main` first if a baseline is needed). Investigate any new failure — especially e2e scripts that assert on `mcpp test` output wording.

- [ ] **Step 2: musl static build**

```bash
mcpp build --target x86_64-linux-musl
MUSL_BIN=$(ls -d target/x86_64-linux-musl/*/bin/mcpp | head -1)
file "$MUSL_BIN"
```

Expected: `statically linked` in the `file` output.

- [ ] **Step 3: Run the four new e2e scripts against the musl binary**

```bash
for t in 118_test_subdir_names 119_test_isolation 120_test_filter 121_test_json; do
    MCPP="$PWD/$MUSL_BIN" bash tests/e2e/$t.sh || { echo "FAILED: $t"; break; }
done
```

Expected: four × `OK`.

- [ ] **Step 4: Nested LD_LIBRARY_PATH immunity check**

Simulates the d2x→provider→nested-mcpp chain: a poisoned `LD_LIBRARY_PATH` pointing at the sandbox glibc must not crash the static binary.

```bash
MUSL_ABS=$(realpath "$MUSL_BIN")
GLIBC_LIB=$(dirname $(ls ~/.mcpp/registry/data/xpkgs/xim-x-glibc/*/lib64/libc.so.6 2>/dev/null | head -1) 2>/dev/null)
cd $(mktemp -d) && mkdir -p pkg/tests && cd pkg
printf '[package]\nname = "nest"\nversion = "0.1.0"\nstandard = "c++23"\n' > mcpp.toml
echo 'int main() { return 0; }' > tests/t.cpp
LD_LIBRARY_PATH="$GLIBC_LIB" "$MUSL_ABS" test
```

Expected: `t ... ok`, no segfault. (With a glibc-linked mcpp this environment reproduces the dynamic-linker crash the provider currently works around with `unsetenv`.)

- [ ] **Step 5: Changelog + docs sync**

Add to `CHANGELOG.md` under an Unreleased heading (create it if absent, matching the file's existing entry style):

```markdown
- `mcpp test`: per-test compile isolation — a test that fails to compile is reported
  as that test's failure (`FAIL (compile)`); remaining tests still build and run.
  Package-level build failures (lib/deps) remain hard errors.
- `mcpp test`: tests are named by their `tests/`-relative path (`00-intro/0`),
  so test files in subdirectories no longer collide on stem.
- `mcpp test <pattern>`: run only tests whose name contains PATTERN.
- `mcpp test --message-format json`: NDJSON output for tooling — one record per
  test (status/exit_code/signal/compile_output/run_output), package-error and
  summary records.
```

Sync user docs:

```bash
grep -rn "mcpp test" docs/*.md README.md README.zh-CN.md
```

Update every hit that describes test naming, failure behavior, or CLI usage to match the new behavior (at minimum the command list blurbs; keep edits to the lines the grep surfaces).

- [ ] **Step 6: Commit**

```bash
git add CHANGELOG.md docs/ README.md README.zh-CN.md
git commit -m "docs: changelog + doc sync for mcpp test isolation/filter/json"
```

- [ ] **Step 7: Report the musl binary path**

Print the absolute `$MUSL_BIN` path in the final summary — the d2mcpp adaptation work (next plan) points `.d2x.json`/CI at this binary for local testing.

---

## Out of scope (tracked in the d2mcpp design doc, not this plan)

- Provider rewrite / `_current` manifest deletion in d2mcpp (separate plan, consumes this binary).
- Removing `unsetenv("LD_LIBRARY_PATH")` workarounds in d2mcpp `runner.cppm` / d2x `platform.cppm` (after the musl binary is adopted end-to-end).
- Per-test cxxflags carriage in mcpp.toml (design doc §7 open item — decide when the d2mcpp migration reaches the two affected exercises).
- Windows/macOS validation of the new test flow.
