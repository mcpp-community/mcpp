# Runtime Launch Hygiene & Multi-Distro Coverage Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Stop `mcpp run`/`test`/fast-build from leaking the target's `LD_LIBRARY_PATH` (bundled glibc) into the host `/bin/sh`, and prove the fix across newer-glibc Linux distros in CI.

**Architecture:** Introduce a single direct-exec launcher seam in `mcpp::platform::process` (no shell, child-scoped env) and route the three `ScopedEnv + std::system/capture` call sites through it. The produced binary is *already* self-contained (bundled dynamic-linker + RUNPATH baked into gcc specs / clang cfg), so we only guard that invariant; we do not rebuild it. Distribution stays a separate `mcpp pack` concern (static-musl default). A containerized CI matrix on rolling/newest-glibc distros makes the regression observable.

**Tech Stack:** C++23 modules (`.cppm`), gtest unit tests (`tests/unit/`), shell e2e tests (`tests/e2e/`), GitHub Actions (`.github/workflows/`), xlings/xim toolchain delivery.

---

## Background — Root Cause (evidence)

User on a newest-glibc distro (host glibc 2.42) ran `mcpp run` and got:

```
sh: …/xim-x-glibc/2.39/lib64/libc.so.6: version `GLIBC_2.42' not found (required by /lib64/libtinfo.so.6)
```

- The failing process is **`sh`**, not the target `hello` — the target never executed.
- Chain (`src/build/execute.cppm:392-400`, `build_run_target`):
  1. `runtime_library_path_key()` → `"LD_LIBRARY_PATH"` on Linux (`src/platform/env.cppm:121`).
  2. `runtimeLibraryDirs` includes the **bundled glibc 2.39 lib dir** (`src/build/plan.cppm:278`).
  3. `ScopedEnv` calls `setenv` on **mcpp's own process** (`src/platform/env.cppm:74-90`).
  4. `std::system(cmd)` spawns the **host `/bin/sh`**, which inherits that `LD_LIBRARY_PATH`.
  5. Host `sh` loads bundled libc 2.39 first, then host `/lib64/libtinfo.so.6` (built against 2.42) → symbol-version mismatch → `sh` aborts before exec'ing the target.
- The same `ScopedEnv + shell` pattern exists at **three** sites: `execute.cppm:331` (fast-build → ninja via `capture`), `:400` (`run`), `:539` (`test`).

**Why the target itself is fine:** the payload gcc is post-install patched so produced binaries carry a bundled `--dynamic-linker` + `-rpath` (`src/toolchain/post_install.cppm:78-156`, `fixup_gcc_specs`; clang via `fixup_clang_cfg`). So `hello` uses the bundled loader + bundled libc and needs **no** runtime env. The injected `LD_LIBRARY_PATH` is redundant for normal binaries and is needed only for the narrow `dlopen()` closure (`src/build/plan.cppm:270-273`). The fix is therefore **launcher hygiene**, not artifact relinking.

## Design Decisions

- **B (do now):** a direct-exec seam — `process::run_exec` / `process::capture_exec`. No `/bin/sh`, so the loader env can never reach the launcher; env is applied to the **child only** (via `setenv` *after* `fork`, in the child address space). This also removes shell quoting/exit-code/signal hazards. Universal across toolchain modes and platforms.
- **D (guard, already built):** assert produced binaries point `PT_INTERP` at the bundled loader for payload toolchains. No new linker flags — the specs/cfg patch already does this. A failing guard means the patch regressed.
- **Distribution (separate layer):** absolute-path D binaries are for the dev loop, not distribution. `mcpp pack` already has `Static` (no PT_INTERP), `BundleProject`, `BundleAll`. Recommend static-musl as the default distributable; document, don't rebuild.
- **CI:** GitHub runners are ubuntu/macos/windows only; use `container:` images to cover Fedora/Arch/Tumbleweed/Debian-testing (newest glibc = the reproduction surface).

## File Structure

| File | Responsibility | Change |
|------|----------------|--------|
| `src/platform/process.cppm` | central process runner; add no-shell direct-exec entry points | Modify |
| `tests/unit/test_process_run_exec.cpp` | prove `run_exec` does not mutate parent env + child sees injected env | Create |
| `src/build/execute.cppm` | route `run`/`test`/fast-build launches through the seam | Modify (3 sites) |
| `tests/e2e/74_run_no_loader_env_leak.sh` | guard: produced binary uses bundled loader; `run` succeeds with hostile parent env | Create |
| `tests/e2e/30_pack_modes.sh` | extend Mode B with a *registry-absent* relocation check (static + bundle-all already covered) | Modify |
| `.github/workflows/ci-fresh-install.yml` | add `linux-distro-matrix` job across newest-glibc distros | Modify |
| `docs/35-pack-design.md` | document the three-tier distribution story (static default) | Modify |

---

### Task 1: Direct-exec launcher seam (`run_exec` + `capture_exec`)

**Files:**
- Modify: `src/platform/process.cppm` (declarations after `:71`; impl after `:217`)
- Test: `tests/unit/test_process_run_exec.cpp`

- [ ] **Step 1: Write the failing test**

Create `tests/unit/test_process_run_exec.cpp`:

```cpp
#include <gtest/gtest.h>
#include <cstdlib>

import std;
import mcpp.platform.process;

using namespace mcpp::platform;

// The regression that matters: launching with an injected loader var must NOT
// mutate the parent (mcpp) environment — that mutation is exactly what leaked
// into /bin/sh and crashed it on newer-glibc hosts.
TEST(RunExec, DoesNotMutateParentEnvironment) {
    ::setenv("MCPP_TEST_LEAK", "sentinel", 1);
    int rc = process::run_exec({"/bin/true"},
                               {{"MCPP_TEST_LEAK", "injected"}});
    EXPECT_EQ(rc, 0);
    const char* v = ::getenv("MCPP_TEST_LEAK");
    ASSERT_NE(v, nullptr);
    EXPECT_STREQ(v, "sentinel");   // parent unchanged → no leak
}

TEST(RunExec, ChildSeesInjectedEnv) {
    // `sh -c '[ "$X" = injected ]'` exits 0 only if the child received X.
    int rc = process::run_exec(
        {"/bin/sh", "-c", "[ \"$MCPP_TEST_INJECT\" = injected ]"},
        {{"MCPP_TEST_INJECT", "injected"}});
    EXPECT_EQ(rc, 0);
}

TEST(RunExec, PropagatesChildExitCode) {
    EXPECT_EQ(process::run_exec({"/bin/sh", "-c", "exit 7"}), 7);
}

TEST(RunExec, ReturnsErrorWhenProgramMissing) {
    EXPECT_NE(process::run_exec({"/no/such/program/mcpp-xyz"}), 0);
}

TEST(CaptureExec, CapturesStdoutWithoutShell) {
    auto r = process::capture_exec({"/bin/echo", "hello world"});
    EXPECT_EQ(r.exit_code, 0);
    EXPECT_EQ(r.output, "hello world\n");
}
```

- [ ] **Step 2: Run the test to verify it fails**

Run: `mcpp test`
Expected: FAIL — `run_exec` / `capture_exec` are not declared in `mcpp.platform.process`.

- [ ] **Step 3: Add the declarations**

In `src/platform/process.cppm`, inside `export namespace mcpp::platform::process { … }`, after the `capture_with_env` declaration (around `:56`), add:

```cpp
// Launch a program DIRECTLY (no shell), inheriting stdio. argv[0] is the
// program (PATH-searched). `extraEnv` is applied to the CHILD ONLY — the
// calling process environment is never mutated, so a target's loader vars
// (LD_LIBRARY_PATH) cannot poison mcpp itself or any sibling host process.
// Returns a platform-normalized exit code, or 127 if exec fails.
int run_exec(const std::vector<std::string>& argv,
             const std::vector<std::pair<std::string, std::string>>& extraEnv = {});

// Same as run_exec but captures stdout AND stderr combined (replaces the old
// `… 2>&1` redirect) into RunResult::output. Required because the only consumer
// (ninja fast-path) parses error text — which ninja writes to stderr — via
// is_stale_ninja_failure / filter_ninja_output. No shell → no quoting/injection.
RunResult capture_exec(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extraEnv = {});
```

- [ ] **Step 4: Add the POSIX + Windows implementation**

In `src/platform/process.cppm`, extend the module preamble (top `module;` block) so the impl has the needed headers:

```cpp
module;
#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <stdlib.h>    // _putenv_s, _spawnvpe, _environ
#include <process.h>   // _spawnvpe, _P_WAIT
#else
#include <unistd.h>    // fork, execvp, _exit, pipe, dup2, close, read
#include <sys/wait.h>  // waitpid
#endif
```

Then, in the implementation `namespace mcpp::platform::process { … }` (before its closing brace, after `run_passthrough`), add:

```cpp
int run_exec(const std::vector<std::string>& argv,
             const std::vector<std::pair<std::string, std::string>>& extraEnv)
{
    if (argv.empty()) return 127;
#if defined(_WIN32)
    // Build a child env block (parent env + overrides); no cmd.exe involved.
    std::vector<std::string> envStrings;
    for (char** e = _environ; e && *e; ++e) envStrings.emplace_back(*e);
    for (auto& [k, v] : extraEnv) envStrings.push_back(k + "=" + v);
    std::vector<const char*> envp;
    for (auto& s : envStrings) envp.push_back(s.c_str());
    envp.push_back(nullptr);

    std::vector<const char*> cargv;
    for (auto& a : argv) cargv.push_back(a.c_str());
    cargv.push_back(nullptr);

    intptr_t rc = _spawnvpe(_P_WAIT, cargv[0],
                            const_cast<char* const*>(cargv.data()),
                            const_cast<char* const*>(envp.data()));
    return rc < 0 ? 127 : static_cast<int>(rc);
#else
    pid_t pid = ::fork();
    if (pid < 0) return 127;
    if (pid == 0) {
        // Child only: mutate THIS address space, never the parent's.
        for (auto& [k, v] : extraEnv) ::setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> cargv;
        for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        _exit(127);  // exec failed
    }
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) { /* EINTR retry */ }
    return normalize_exit_code(status);
#endif
}

RunResult capture_exec(
    const std::vector<std::string>& argv,
    const std::vector<std::pair<std::string, std::string>>& extraEnv)
{
    RunResult result;
    if (argv.empty()) { result.exit_code = 127; return result; }
#if defined(_WIN32)
    // Windows is unaffected by the glibc leak; reuse the shell capture path
    // with additive env for parity.
    std::string cmd;
    for (auto& a : argv) { cmd += '"'; cmd += a; cmd += "\" "; }
    return capture_with_env(cmd, extraEnv);
#else
    int fds[2];
    if (::pipe(fds) != 0) { result.exit_code = 127; return result; }
    pid_t pid = ::fork();
    if (pid < 0) { ::close(fds[0]); ::close(fds[1]); result.exit_code = 127; return result; }
    if (pid == 0) {
        ::close(fds[0]);
        ::dup2(fds[1], 1);            // stdout → pipe
        ::dup2(fds[1], 2);            // stderr → same pipe (replaces `2>&1`)
        ::close(fds[1]);
        for (auto& [k, v] : extraEnv) ::setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> cargv;
        for (auto& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
        cargv.push_back(nullptr);
        ::execvp(cargv[0], cargv.data());
        _exit(127);
    }
    ::close(fds[1]);
    std::array<char, 4096> buf{};
    ssize_t n;
    while ((n = ::read(fds[0], buf.data(), buf.size())) > 0)
        result.output.append(buf.data(), static_cast<size_t>(n));
    ::close(fds[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0) { /* EINTR retry */ }
    result.exit_code = normalize_exit_code(status);
    return result;
#endif
}
```

- [ ] **Step 5: Run the tests to verify they pass**

Run: `mcpp test`
Expected: PASS — all `RunExec` / `CaptureExec` cases green; existing suite unaffected.

- [ ] **Step 6: Commit**

```bash
git add src/platform/process.cppm tests/unit/test_process_run_exec.cpp
git commit -m "feat(platform): add no-shell run_exec/capture_exec with child-scoped env"
```

---

### Task 2: Route `mcpp run` through the seam

**Files:**
- Modify: `src/build/execute.cppm:389-401` (`build_run_target`)

- [ ] **Step 1: Replace the leaky launch block**

In `src/build/execute.cppm`, replace lines 389-401 (from `std::string cmd = mcpp::platform::shell::quote(...)` through `return mcpp::platform::process::extract_exit_code(rc) == 0 ? 0 : 1;`) with:

```cpp
    std::vector<std::string> argv;
    argv.push_back(exe.string());
    for (auto& a : passthrough) argv.push_back(a);

    std::vector<std::pair<std::string, std::string>> childEnv;
    auto runtimeEnvKey = mcpp::platform::env::runtime_library_path_key();
    auto runtimeEnvValue = mcpp::platform::env::prepend_path_list(
        runtimeEnvKey, ctx->plan.runtimeLibraryDirs);
    if (!runtimeEnvKey.empty() && !runtimeEnvValue.empty())
        childEnv.emplace_back(runtimeEnvKey, runtimeEnvValue);

    // Direct exec (no /bin/sh): the loader env reaches ONLY the target child,
    // never mcpp or a host shell. Fixes the bundled-glibc-vs-host-libtinfo
    // crash on newer-glibc distros.
    return mcpp::platform::process::run_exec(argv, childEnv) == 0 ? 0 : 1;
```

- [ ] **Step 2: Build to verify it compiles**

Run: `mcpp clean && mcpp build`
Expected: PASS — no references to the removed `ScopedEnv runtimeEnv` / `std::system` remain in `build_run_target`.

- [ ] **Step 3: Smoke-test run locally**

Run: `cd "$(mktemp -d)" && mcpp new smoke && cd smoke && mcpp run`
Expected: prints program output, exit 0. (On a same-or-older-glibc dev host this already worked; the real proof is Task 7.)

- [ ] **Step 4: Commit**

```bash
git add src/build/execute.cppm
git commit -m "fix(run): exec target directly, scope LD_LIBRARY_PATH to child (no sh leak)"
```

---

### Task 3: Route `mcpp test` through the seam

**Files:**
- Modify: `src/build/execute.cppm:508-549` (`run_tests` launch loop)

- [ ] **Step 1: Replace the `ScopedEnv` + `pathPrefix` + `std::system` launch**

In `src/build/execute.cppm`, replace the block from `std::optional<mcpp::platform::env::ScopedEnv> runtimeEnv;` (around `:508`) through the end of the per-test launch loop (the `int exitCode = mcpp::platform::process::extract_exit_code(std::system(cmd.c_str()));` line, around `:539`) so that:

1. The pre-loop `ScopedEnv runtimeEnv { … }` block (`:508-514`) is **deleted**.
2. Inside the `for (auto& lu : ctx->plan.linkUnits)` loop, the launch becomes:

```cpp
        std::vector<std::string> argv;
        argv.push_back(exe.string());
        for (auto& a : passthrough) argv.push_back(a);

        std::vector<std::pair<std::string, std::string>> childEnv;
        auto runtimeEnvKey = mcpp::platform::env::runtime_library_path_key();
        auto runtimeEnvValue = mcpp::platform::env::prepend_path_list(
            runtimeEnvKey, ctx->plan.runtimeLibraryDirs);
        if (!runtimeEnvKey.empty() && !runtimeEnvValue.empty())
            childEnv.emplace_back(runtimeEnvKey, runtimeEnvValue);

        // Sandbox subos/default/bin onto the child PATH so test binaries that
        // shell out to bootstrapped tools (patchelf, ninja) find them — applied
        // to the child only, not via a leaky shell prefix.
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
```

The surrounding `mcpp::ui::status("Running", …)` line and the `if (exitCode == 0) { … } else { … }` accounting are unchanged.

- [ ] **Step 2: Build to verify it compiles**

Run: `mcpp clean && mcpp build`
Expected: PASS — no `ScopedEnv` / `pathPrefix` / `std::system` left in `run_tests`.

- [ ] **Step 3: Run the test suite (dogfood)**

Run: `mcpp test`
Expected: PASS — tests still discovered, built, and run; summary prints pass counts.

- [ ] **Step 4: Commit**

```bash
git add src/build/execute.cppm
git commit -m "fix(test): exec test binaries directly with child-scoped env/PATH"
```

---

### Task 4: Route fast-build (ninja) through `capture_exec`

**Files:**
- Modify: `src/build/execute.cppm:319-334` (`try_fast_build` ninja invocation)

- [ ] **Step 1: Replace shell-built ninja command with an argv + capture_exec**

In `src/build/execute.cppm`, in `try_fast_build`, replace the command-string construction and `capture` call (the block building `std::string cmd = ninjaProgram; … cmd += " 2>&1";` through `auto r = mcpp::platform::process::capture(cmd);` and the surrounding `ScopedEnv`, around `:319-334`) with:

```cpp
    // All inputs are older than build.ninja → fast-path: just run ninja.
    std::vector<std::string> argv{ninjaProgram};
    if (!verbose) argv.push_back("--quiet");
    argv.push_back("-C");
    argv.push_back(outputDir.string());
    if (verbose) argv.push_back("-v");

    std::vector<std::pair<std::string, std::string>> childEnv;
    if (runtimeEnvKey != "-" && !runtimeEnvValue.empty())
        childEnv.emplace_back(runtimeEnvKey, runtimeEnvValue);

    auto t0 = std::chrono::steady_clock::now();
    auto r = mcpp::platform::process::capture_exec(argv, childEnv);
    std::string out = r.output;
    int status = r.exit_code;
```

Note: the `cmd += " 2>&1"` (`execute.cppm:324`) is gone because `capture_exec`
itself merges stderr into the captured pipe (`dup2(fds[1], 2)`). This preserves
the old contract: `out` still contains ninja's stderr error text, so
`is_stale_ninja_failure(out)` (`:336`) and `filter_ninja_output(out)` (`:341`)
keep working. Also remove the now-unused `std::optional<...ScopedEnv> scopedEnv;`
line (`:328-330`).

- [ ] **Step 2: Build to verify it compiles**

Run: `mcpp clean && mcpp build`
Expected: PASS.

- [ ] **Step 3: Verify fast-path still fires**

Run: `mcpp build && mcpp build`
Expected: second invocation prints `Finished release … ` quickly (fast-path), no errors.

- [ ] **Step 4: Commit**

```bash
git add src/build/execute.cppm
git commit -m "fix(build): run fast-path ninja via capture_exec (no shell env leak)"
```

---

### Task 5: e2e guard — no loader leak + bundled-loader self-containment

**Files:**
- Create: `tests/e2e/74_run_no_loader_env_leak.sh`

- [ ] **Step 1: Write the e2e guard**

Create `tests/e2e/74_run_no_loader_env_leak.sh` (match the numbered, `set -euo pipefail` style of existing e2e scripts):

```bash
#!/usr/bin/env bash
# 74: `mcpp run` must not leak the target's bundled-glibc LD_LIBRARY_PATH into
# the launcher shell, and the produced binary must use the BUNDLED dynamic
# linker (proof that the run path relies on the self-contained artifact, not
# on poisoning the environment). Regression for the newer-glibc `sh:` crash.
set -euo pipefail

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cd "$work"

mcpp new leakcheck
cd leakcheck

# 1. Hostile parent env: a bogus LD_LIBRARY_PATH must not break the launcher,
#    and must not be required for the target to run.
LD_LIBRARY_PATH="/nonexistent/poison:${LD_LIBRARY_PATH:-}" mcpp run

# 2. Self-containment: PT_INTERP points into the mcpp registry (bundled loader),
#    not host /lib64 — for payload (gcc) toolchains.
mcpp build
bin="$(find target -type f -name leakcheck | head -1)"
test -n "$bin"
if command -v readelf >/dev/null 2>&1; then
  interp="$(readelf -l "$bin" | sed -n 's/.*program interpreter: \(.*\)\]/\1/p' | tr -d ' ')"
  echo "interp=$interp"
  case "$interp" in
    */.mcpp/*|*/registry/*|*xpkgs*|"") echo "OK: bundled or static loader" ;;
    /lib64/*|/lib/*) echo "FAIL: host loader $interp"; exit 1 ;;
    *) echo "WARN: unrecognized interp $interp" ;;
  esac
fi

echo "74_run_no_loader_env_leak: OK"
```

- [ ] **Step 2: Make it executable and run it**

```bash
chmod +x tests/e2e/74_run_no_loader_env_leak.sh
./tests/e2e/74_run_no_loader_env_leak.sh
```
Expected: ends with `74_run_no_loader_env_leak: OK`.

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/74_run_no_loader_env_leak.sh
git commit -m "test(e2e): guard run-path loader-env hygiene + bundled interp"
```

---

### Task 6: Distribution — registry-absent relocation guard + docs

**Already implemented and e2e-guarded — do NOT rebuild:**
- `--static` (musl, no PT_INTERP) → `pack.cppm` `Mode::Static`; covered by `30_pack_modes.sh` Mode A.
- relocatable glibc bundle + loader wrapper → `--mode bundle-all` writes `ld.so --library-path … "$@"` (`pack.cppm:410-420`, `:630-643`); covered by `30_pack_modes.sh` Mode B.

The only real gap: `30_pack_modes.sh` runs the bundle-all wrapper while `~/.mcpp` still exists, so it does not *prove* registry-independence. This task adds that proof, then documents the tiers.

**Files:**
- Modify: `tests/e2e/30_pack_modes.sh` (append a registry-absent run to the Mode B section)
- Modify: `docs/35-pack-design.md`

- [ ] **Step 1: Add a registry-absent relocation assertion to Mode B**

In `tests/e2e/30_pack_modes.sh`, immediately before the final `echo "OK"` (line ~141), insert:

```bash
# Relocation proof: the bundle-all artifact must run with the mcpp registry
# absent. Copy it elsewhere and launch with a scrubbed environment (no HOME →
# no ~/.mcpp fallback, no inherited LD_LIBRARY_PATH). This is the property
# `--mode bundle-all` exists to provide: loader + libc travel with the bundle.
relocated="$TMP/relocated"
cp -r "$TMP/b/myapp-0.1.0-x86_64-linux-gnu-bundle-all" "$relocated"
env -i HOME=/nonexistent PATH=/usr/bin:/bin "$relocated/run.sh" > "$TMP/b-reloc.log" 2>&1 || {
    cat "$TMP/b-reloc.log"; echo "Mode B: relocated bundle failed with registry absent"; exit 1; }
grep -q 'Hello' "$TMP/b-reloc.log" || {
    cat "$TMP/b-reloc.log"; echo "Mode B: relocated output missing"; exit 1; }
```

- [ ] **Step 2: Run it**

Run: `./tests/e2e/30_pack_modes.sh`
Expected: ends with `OK` (Mode B now also proves registry-absent relocation).

- [ ] **Step 3: Document the three-tier distribution story**

In `docs/35-pack-design.md`, add a section near the mode table:

```markdown
## Choosing a distribution artifact

`mcpp run` binaries embed ABSOLUTE bundled-loader/RUNPATH paths (the dev-loop
form): reproducible locally, but not portable — copied to a machine without the
same registry path the kernel cannot find PT_INTERP. Distribution is a separate
transform via `mcpp pack`:

| Need | Mode | Result |
|------|------|--------|
| Portable single file (recommended) | `--static` (musl) | no PT_INTERP / RUNPATH; runs anywhere, same arch/kernel |
| glibc semantics, relocatable tree | `BundleAll` | `bin/` + `lib/` with `$ORIGIN`-relative RUNPATH |
| ship only 3rd-party `.so` | `BundleProject` (default) | host provides libc/libstdc++ |

Note: PT_INTERP cannot be `$ORIGIN`-relative (the kernel reads it literally
before `$ORIGIN` exists). So `--mode bundle-all` ships the bundled `ld.so` plus
a generated `run.sh` wrapper that invokes it as
`ld.so --library-path "$here/lib" "$here/bin/<name>" "$@"` — the loader is
resolved relatively at runtime. This mirrors cargo: the default dynamic build is
host-bound; portable distribution means switching to the static (musl) target.
```

- [ ] **Step 4: Commit**

```bash
git add tests/e2e/30_pack_modes.sh docs/35-pack-design.md
git commit -m "test(e2e)+docs: bundle-all registry-absent relocation guard + distribution tiers"
```

---

### Task 7: CI — newer-glibc multi-distro matrix

**Files:**
- Modify: `.github/workflows/ci-fresh-install.yml` (add a `linux-distro-matrix` job)

- [ ] **Step 1: Add the containerized matrix job**

In `.github/workflows/ci-fresh-install.yml`, after the `linux-fresh` job (before `macos-fresh`), add:

```yaml
  # ──────────────────────────────────────────────────────────────────
  # Newer/rolling-glibc distros — reproduction surface for the
  # bundled-glibc-vs-host-libtinfo `sh:` crash. Runs the released mcpp
  # inside distro containers on the ubuntu runner.
  # ──────────────────────────────────────────────────────────────────
  linux-distro-matrix:
    name: Linux distro (${{ matrix.distro }})
    runs-on: ubuntu-24.04
    container:
      image: ${{ matrix.image }}
    timeout-minutes: 45
    strategy:
      fail-fast: false
      matrix:
        include:
          - distro: fedora-latest
            image: fedora:latest
            setup: dnf -y install curl bash tar gzip xz git findutils binutils glibc-langpack-en
          - distro: arch
            image: archlinux:latest
            setup: pacman -Sy --noconfirm curl bash tar gzip xz git binutils
          - distro: tumbleweed
            image: opensuse/tumbleweed:latest
            setup: zypper -n install curl bash tar gzip xz git binutils
          - distro: debian-testing
            image: debian:testing
            setup: apt-get update && apt-get -y install curl bash tar gzip xz-utils git ca-certificates binutils findutils
          # Older-glibc legs — the REVERSE direction (bundled glibc 2.39 is
          # NEWER than host). These should pass before AND after the fix; they
          # prove the musl-static mcpp + bundled, self-contained toolchain run
          # end-to-end on old hosts, and catch a different break class (e.g. the
          # bundled glibc's kernel-version floor being too high for an old image).
          - distro: ubuntu-2004
            image: ubuntu:20.04
            setup: apt-get update && DEBIAN_FRONTEND=noninteractive apt-get -y install curl bash tar gzip xz-utils git ca-certificates binutils findutils
          - distro: debian-11
            image: debian:11
            setup: apt-get update && apt-get -y install curl bash tar gzip xz-utils git ca-certificates binutils findutils
    env:
      XLINGS_NON_INTERACTIVE: '1'
      HOME: /root
    steps:
      - uses: actions/checkout@v4

      - name: Install prerequisites (${{ matrix.distro }})
        run: ${{ matrix.setup }}

      - name: Install xlings + mcpp
        run: |
          curl -fsSL https://raw.githubusercontent.com/openxlings/xlings/main/tools/other/quick_install.sh | bash -s v0.4.38
          echo "$HOME/.xlings/subos/current/bin" >> "$GITHUB_PATH"

      - name: Configure mcpp
        run: |
          export PATH="$HOME/.xlings/subos/current/bin:$PATH"
          xlings update
          xlings install mcpp -y -g
          mcpp --version
          mcpp self config --mirror GLOBAL

      - name: "Regression: new → run (loader env must not crash /bin/sh)"
        run: |
          export PATH="$HOME/.xlings/subos/current/bin:$PATH"
          cd "$(mktemp -d)"
          mcpp new hello_distro
          cd hello_distro
          mcpp run

      - name: "Self-containment: produced binary uses bundled loader"
        run: |
          export PATH="$HOME/.xlings/subos/current/bin:$PATH"
          cd "$(mktemp -d)" && mcpp new hc && cd hc && mcpp build
          bin="$(find target -type f -name hc | head -1)"
          interp="$(readelf -l "$bin" | sed -n 's/.*program interpreter: \(.*\)\]/\1/p' | tr -d ' ')"
          echo "interp=$interp"
          case "$interp" in
            */.mcpp/*|*/registry/*|*xpkgs*) echo "OK bundled loader" ;;
            *) echo "FAIL host loader: $interp"; exit 1 ;;
          esac
```

- [ ] **Step 2: Validate the workflow YAML**

Run: `python3 -c "import yaml,sys; yaml.safe_load(open('.github/workflows/ci-fresh-install.yml')); print('yaml ok')"`
Expected: `yaml ok`.

- [ ] **Step 3: Lint with actionlint if available (optional)**

Run: `command -v actionlint >/dev/null && actionlint .github/workflows/ci-fresh-install.yml || echo "actionlint not installed — skipping"`
Expected: no errors, or the skip notice.

- [ ] **Step 4: Commit and push on a branch; observe the matrix**

```bash
git add .github/workflows/ci-fresh-install.yml
git commit -m "ci(fresh-install): add newer-glibc distro matrix (fedora/arch/tumbleweed/debian-testing)"
```

Push to a branch and trigger `workflow_dispatch`. Expected: on `main` (pre-fix) the `fedora-latest` / `arch` / `tumbleweed` legs reproduce the `sh:` crash at the `new → run` step; after Tasks 2-4 land, all legs are green.

---

## Self-Review

**1. Spec coverage**
- "Complete implementation plan" → Tasks 1-7 cover the seam, all three leak sites, the D-invariant guard, distribution, and CI. ✓
- "CI fresh-install: more Linux distro tests" → Task 7 adds Fedora/Arch/Tumbleweed/Debian-testing containers to `ci-fresh-install.yml`. ✓
- "Place under .agents/docs" → this file. ✓

**2. Placeholder scan** — no TBD/TODO; every code/edit step shows concrete code, exact paths, and expected output. ✓

**3. Type consistency** — `run_exec(argv, extraEnv)` and `capture_exec(argv, extraEnv)` signatures are identical across declaration (Task 1) and all call sites (Tasks 2-4). `RunResult{exit_code, output}` matches the existing struct (`process.cppm:37`). `prepend_path_list(key, span<path>)` reused as-is (`env.cppm:137`). ✓

**Implementation caveats (address during Task 1):**
- The POSIX `run_exec`/`capture_exec` call `setenv` in the child *between* `fork` and `execvp`. `setenv` is not async-signal-safe, which only matters if mcpp is multithreaded at the launch point — it is not today (the launch sites run sequentially in the main thread), so this is safe in practice. If mcpp ever gains threads, switch to building the `envp` array in the parent and `execvpe` (Linux) / `posix_spawn` with that `envp`.

**Known scope boundaries (intentional, not gaps):**
- The released Linux mcpp binary is **musl-static** (`release.yml:131` `mcpp build --target x86_64-linux-musl`; `:134` and `:194` assert `file … | grep -q 'statically linked'`), so it runs on *any* Linux regardless of host glibc — it is **not** a barrier to old-distro coverage. The matrix therefore includes both newer-glibc legs (Fedora/Arch/Tumbleweed/Debian-testing — the bug's reproduction surface) and older-glibc legs (Ubuntu 20.04 / Debian 11 — the safe reverse direction, where bundled glibc 2.39 ≥ host so `/bin/sh` never crashes; these pass before and after the fix and guard end-to-end compatibility).
- The one genuine open question on old-distro legs is whether **xlings** (the installer dependency, fetched separately) runs there — the matrix surfaces this empirically rather than us assuming. If an old leg fails purely at `Install xlings`, mark it `continue-on-error: true` and file an xlings-static follow-up; do not let it gate the fix.
- Both distribution forms are **already implemented and e2e-guarded**, not future work: `--static` (musl, no PT_INTERP) via `Mode::Static`, and the relocatable glibc bundle + loader wrapper via `--mode bundle-all` (`src/pack/pack.cppm:410-420`, `:630-643`), both covered by `tests/e2e/30_pack_modes.sh` (Modes A & B). Task 6 only adds a registry-absent relocation assertion + docs; there is no wrapper to build.

## Implementation notes (as-built, PR `fix/run-loader-env-leak`)

Two things surfaced during implementation that the plan above did not anticipate:

1. **A fourth leak site.** The *full* build also runs ninja through a shell with
   a `ScopedEnv` (`ninja_backend.cppm:737-751`), the same class as the fast-path.
   It only escaped the user's report because the build uses
   `compilerRuntimeDirs` (usually empty), not the target's bundled-glibc dirs.
   Fixed by routing it through `capture_exec` too (no shell, child-scoped env).
2. **`ninjaProgram` was shell-quoted at the source** (`ninja_backend.cppm:722`).
   `execvp` takes argv, not a shell string, so the literal quotes broke the
   fast-path (`No such file or directory`). Fixed by storing the program path
   **raw** in `BuildResult::ninjaProgram` and quoting locally only at the
   remaining shell-using site, plus a defensive unquote in `try_fast_build`
   for any legacy shell-quoted cache entry.

Verified locally: unit suite 19/19 (incl. 6 new `RunExec`/`CaptureExec`);
`tests/e2e/74` green; fast-path rebuild restored (0.00s); `mcpp run` works with
a hostile `LD_LIBRARY_PATH`; produced binary's `PT_INTERP` resolves into the
bundled registry loader. e2e `65` fails identically on baseline 0.0.55 (known
environmental LLVM issue), so it is not a regression.

## Execution Handoff

Plan complete and saved to `.agents/docs/2026-06-19-runtime-launch-and-multi-distro-plan.md`. Two execution options:

1. **Subagent-Driven (recommended)** — a fresh subagent per task, review between tasks, fast iteration.
2. **Inline Execution** — execute tasks in this session using executing-plans, batch with checkpoints.

Which approach?
