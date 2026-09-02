# Issue #544: runner beyond bare metal — implementation plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `[target.<triple>].runner` is honoured on every target, a spawn failure is never silent, `mcpp test` reports unrunnable artifacts as not-run with exit 2, `[xlings]` values can be given per host platform, and the whole change ships as one PR, one release, and one sandbox verification.

**Architecture:** `choose_runner` stays the single read point and stops gating on `os == none`; the launcher layer carries the spawn errno up instead of re-spawning; the manifest resolves `[xlings]` per-platform values against the host at load time so every downstream reader keeps seeing a flat list. Design: `.agents/docs/2026-09-02-runner-beyond-baremetal-design.md` (sections 0 and 4 are normative).

**Tech Stack:** C++23 modules (mcpp builds itself with `mcpp build`), gtest unit tests under `tests/unit/` run by `mcpp test`, bash e2e under `tests/e2e/` run by `tests/e2e/run_all.sh` or individually with `MCPP=<bin> bash tests/e2e/<n>.sh`.

## Global Constraints

- Baseline is `origin/main` at `aeba151` (version `2026.9.1.1`). Work in a fresh worktree; never switch branches in the shared checkout.
- All code goes through one PR with squash merge; CI must be green on the PR and on `origin/main` after merge.
- Prose in docs and the PR is English academic register, declarative, no emoji, no question headings (`.agents/skills/mcpp-docs-style/SKILL.md`). Every `docs/*.md` change has a `docs/zh/` twin; CI enforces the pair (`.github/tools/check_docs_style.sh`).
- New e2e scripts carry no `# requires:` guard beyond `unix-shell`; both CI shards lack llvm and a `requires` skip exits 0.
- Exit code 2 means "could not start"; 1 keeps meaning "ran and failed"; 0 means every test ran and passed.
- Platform keys accepted in `[xlings]` values: `linux`, `macos` (alias `macosx`), `windows`, `default`. Any other key is a hard manifest error.
- Version bump: `mcpp.toml` `[package].version` and `modules/versioning/src/version.cppm` `MCPP_VERSION` in the same commit; `.xlings.json` bootstrap pin stays at the released version until the new release is indexed.

---

### Task 1: Worktree and baseline

**Files:** none modified.

- [ ] **Step 1: Create the worktree from `origin/main`**

```bash
cd ~/workspace/github/mcpp-community/mcpp
git fetch origin
git worktree add ../mcpp-issue544 -b fix/runner-hosted-targets origin/main
cd ../mcpp-issue544
git log --oneline -1          # must be aeba151
```

- [ ] **Step 2: Build the engine and run the unit suite once**

```bash
mcpp self config --mirror CN
mcpp build 2>&1 | tail -3
mcpp test unit/test_process_run_exec 2>&1 | tail -3
mcpp test unit/test_manifest 2>&1 | tail -3
```

Expected: both `ok.` The built binary is `target/<triple>/<fp>/bin/mcpp`; record its path as `$MCPP` for the e2e runs below (`MCPP=$(ls -d target/*/*/bin/mcpp | head -1)`).

---

### Task 2: Launcher carries the spawn errno; no second spawn; `run_exec` never silent

**Files:**
- Modify: `modules/platform/src/unix/bounded_process.cppm:59-66` (struct), `:227-230` (spawn site)
- Modify: `modules/platform/src/windows/bounded_process.cppm:67-77` (struct), the `CreateProcess` failure site
- Modify: `modules/platform/src/process.cppm` — `run_exec` (:552-577), `capture_exec` (:580-625), `BoundedOutcome` (:661-666), `dispatch_bounded` (:707-726), `run_exec_deadline` (:731-750), `capture_exec_deadline` (:840-862), and the exported declarations near `:110` and `:175`
- Test: `tests/unit/test_process_run_exec.cpp`

**Interfaces:**
- Produces: every launcher gains a trailing `int* spawn_error = nullptr`. Contract: when the child could not be spawned, the launcher returns 127 (or `exit_code = 127` in `RunResult`) and (a) if `spawn_error` is non-null, stores the errno there and prints nothing; (b) if null, prints `spawn_failure(argv.front(), err)` to stderr (`run_exec`) or into `output` (`capture_exec`). `DeadlineRun` and `BoundedOutcome` gain `int spawn_error = 0`; `supported == false && spawn_error != 0` means "spawned attempted and refused" and the deadline wrappers do not fall back in that case.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_process_run_exec.cpp` inside the `#if !defined(_WIN32)` block:

```cpp
TEST(RunExec, MissingProgramReportsOnStderrWhenCallerDoesNotAsk) {
    testing::internal::CaptureStderr();
    int rc = process::run_exec({"/no/such/program/mcpp-run-xyz"});
    auto err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 127);
    EXPECT_NE(err.find("/no/such/program/mcpp-run-xyz"), std::string::npos) << err;
    EXPECT_NE(err.find("error 2"), std::string::npos) << err;
}

TEST(RunExec, MissingProgramTypesErrnoWhenCallerAsks) {
    int spawnErr = 0;
    testing::internal::CaptureStderr();
    int rc = process::run_exec({"/no/such/program/mcpp-run-xyz"}, {}, &spawnErr);
    auto err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 127);
    EXPECT_EQ(spawnErr, ENOENT);
    EXPECT_TRUE(err.empty()) << err;   // the caller owns the report
}

TEST(RunExec, UnloadableArtifactIsENOEXEC) {
    // A file with the executable bit and no loader magic: the kernel refuses
    // it with ENOEXEC. posix_spawnp does not retry through /bin/sh the way
    // execvp does, so the errno reaches the caller untouched.
    auto dir = std::filesystem::temp_directory_path() / "mcpp-enoexec-test";
    std::filesystem::create_directories(dir);
    auto f = dir / "not-an-elf";
    { std::ofstream o(f); o << "\x7f" "NOT"; }
    std::filesystem::permissions(f, std::filesystem::perms::owner_all);
    int spawnErr = 0;
    int rc = process::run_exec({f.string()}, {}, &spawnErr);
    EXPECT_EQ(rc, 127);
    EXPECT_EQ(spawnErr, ENOEXEC);
}

TEST(RunExecDeadline, SpawnFailureIsTypedAndNotRetried) {
    int spawnErr = 0;
    bool timedOut = false;
    testing::internal::CaptureStderr();
    int rc = process::run_exec_deadline({"/no/such/program/mcpp-dl-xyz"}, {},
                                        std::chrono::milliseconds(5000), &timedOut,
                                        &spawnErr);
    auto err = testing::internal::GetCapturedStderr();
    EXPECT_EQ(rc, 127);
    EXPECT_EQ(spawnErr, ENOENT);
    EXPECT_FALSE(timedOut);
    EXPECT_TRUE(err.empty()) << err;
}

TEST(CaptureExecDeadline, SpawnFailureIsTypedInOutcome) {
    int spawnErr = 0;
    bool timedOut = false;
    auto r = process::capture_exec_deadline({"/no/such/program/mcpp-cdl-xyz"}, {},
                                            std::chrono::milliseconds(5000), &timedOut,
                                            {}, &spawnErr);
    EXPECT_EQ(r.exit_code, 127);
    EXPECT_EQ(spawnErr, ENOENT);
    EXPECT_TRUE(r.output.empty()) << r.output;
}
```

Add `#include <cerrno>` and `#include <fstream>` at the top of the file.

- [ ] **Step 2: Run to verify they fail**

Run: `mcpp build && mcpp test unit/test_process_run_exec`
Expected: compile failure on the new `spawn_error` parameter (the tests name a signature that does not exist yet).

- [ ] **Step 3: Implement**

`modules/platform/src/unix/bounded_process.cppm`, struct:

```cpp
struct DeadlineRun {
    bool supported = false;
    int  exit_code = 0;
    bool timed_out = false;
    // Non-zero when `supported` is false BECAUSE the spawn was attempted and
    // refused (the errno posix_spawnp returned). Zero with `supported` false
    // means the platform has no bounded launcher at all. The two need
    // opposite treatment upstream: the first is reported, the second falls
    // back to the unbounded launcher.
    int  spawn_error = 0;
};
```

Spawn site (`:230`): `if (sp != 0) { out.spawn_error = sp; if (capture) ::close(fds[0]); return out; }`.

`modules/platform/src/windows/bounded_process.cppm`: same field; at the `CreateProcessW` failure site set `out.spawn_error = static_cast<int>(::GetLastError());`.

`modules/platform/src/process.cppm`:

```cpp
struct BoundedOutcome {
    bool        supported = false;
    int         exit_code = 0;
    bool        timed_out = false;
    int         spawn_error = 0;
    std::string output;
};
// dispatch_bounded: copy r.spawn_error into outcome.spawn_error on both branches.

int run_exec(const std::vector<std::string>& argv,
             const std::vector<std::pair<std::string, std::string>>& extraEnv,
             int* spawn_error)
{
    if (spawn_error) *spawn_error = 0;
    if (argv.empty()) return 127;
#if defined(__linux__) || defined(__APPLE__)
    ...
    pid_t pid = 0;
    if (int sp = ::posix_spawnp(&pid, cargv[0], nullptr, nullptr, cargv.data(), envp.data());
        sp != 0) {
        // A spawn failure is reported exactly once: by the caller when it
        // asked to see the errno, and here otherwise. It is never dropped —
        // "Running …", a blank line and exit 1 was the whole of #544's output.
        if (spawn_error) *spawn_error = sp;
        else std::fputs(spawn_failure(argv.front(), sp).c_str(), stderr);
        return 127;
    }
    ...
#else
    // std::system cannot distinguish a refused launch from a failed child;
    // spawn_error stays 0 and the shell's own message is what the user sees.
    ...
#endif
}
```

`capture_exec`: same shape; when `spawn_error` is non-null store and leave `output` empty, else format `spawn_failure` into `output` as today.

`run_exec_deadline` and `capture_exec_deadline`:

```cpp
    auto r = dispatch_bounded(...);
    if (!r.supported) {
        if (r.spawn_error != 0) {
            // Attempted and refused. Spawning again would only discard this
            // errno and pay for a second refusal.
            if (spawn_error) *spawn_error = r.spawn_error;
            else std::fputs(spawn_failure(argv.front(), r.spawn_error).c_str(), stderr);
            return 127;                     // capture variant: result.exit_code = 127
        }
        return run_exec(argv, extraEnv, spawn_error);   // no bounded launcher here
    }
```

Update the exported declarations (`process.cppm:110`, `:175`, and the `run_exec`/`capture_exec` declarations) with the trailing `int* spawn_error = nullptr`. `run_shell_deadline` is unchanged.

- [ ] **Step 4: Run to verify they pass**

Run: `mcpp build && mcpp test unit/test_process_run_exec`
Expected: `ok.` with the five new tests listed.

- [ ] **Step 5: Commit**

```bash
git add modules/platform tests/unit/test_process_run_exec.cpp
git commit -m "platform: carry the spawn errno up instead of re-spawning; run_exec is never silent (#544)"
```

---

### Task 3: Manifest — array keys in the sweep; `[xlings]` per-platform values

**Files:**
- Modify: `modules/manifest/src/toml.cppm:1350-1361` (`[xlings]`), `:1917-1959` (sweep)
- Modify: `modules/manifest/src/types.cppm:758-775` (`XlingsConfig`, comment only)
- Test: `tests/unit/test_manifest.cpp`

**Interfaces:**
- Produces: `mcpp::manifest::resolve_host_value(const toml::Value&, std::string_view hostPlatform) -> std::expected<std::optional<std::string>, std::string>` exported from the manifest module. Returns the string, `nullopt` when the value is a platform table with no matching key and no `default`, and an error string for a table with an unknown key or a non-string leaf. `mcpp::manifest::host_platform_key()` returns `"linux"`, `"macos"` or `"windows"` for the running binary. `XlingsConfig::deps` and `::workspace` keep their flat types; resolution happens at load.

- [ ] **Step 1: Write the failing tests**

Append to `tests/unit/test_manifest.cpp` (follow the file's fixture helper for writing a manifest to a temp dir and calling `mcpp::manifest::load`; the helper used at `:3249` is the model):

```cpp
TEST(Manifest, TargetSweepReportsArrayTyposAndListsRunner) {
    constexpr auto src = R"(
[package]
name = "app"
version = "0.1.0"
[target.aarch64-linux-musl]
runnerX = ["qemu-aarch64-static"]
)";
    auto m = load_from_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().message;
    ASSERT_EQ(m->schemaWarnings.size(), 1u);
    EXPECT_NE(m->schemaWarnings[0].find("'runnerX'"), std::string::npos);
    EXPECT_NE(m->schemaWarnings[0].find("runner"), std::string::npos);
    EXPECT_NE(m->schemaWarnings[0].find("Supported keys: cxx_runtime, linkage, runner, sysroot, toolchain"),
              std::string::npos) << m->schemaWarnings[0];
}

TEST(Manifest, XlingsDepsAcceptPerPlatformEntries) {
    constexpr auto src = R"(
[package]
name = "app"
version = "0.1.0"
[xlings]
deps = ["xim:ninja", { linux = "qemu-user-aarch64" }, { windows = "nasm", default = "yasm" }]
)";
    auto m = load_from_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().message;
    const auto host = mcpp::manifest::host_platform_key();
    std::vector<std::string> want{"xim:ninja"};
    if (host == "linux") want.push_back("qemu-user-aarch64");
    want.push_back(host == "windows" ? "nasm" : "yasm");
    EXPECT_EQ(m->xlings.deps, want);
}

TEST(Manifest, XlingsWorkspaceAcceptsPerPlatformValues) {
    constexpr auto src = R"(
[package]
name = "app"
version = "0.1.0"
[xlings.workspace]
gcc = { linux = "15.1.0" }
llvm = { macos = "20", default = "22" }
)";
    auto m = load_from_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().message;
    const auto host = mcpp::manifest::host_platform_key();
    if (host == "linux") EXPECT_EQ(m->xlings.workspace.at("gcc"), "15.1.0");
    else EXPECT_EQ(m->xlings.workspace.count("gcc"), 0u);
    EXPECT_EQ(m->xlings.workspace.at("llvm"), host == "macos" ? "20" : "22");
}

TEST(Manifest, XlingsUnknownPlatformKeyIsAnError) {
    constexpr auto src = R"(
[package]
name = "app"
version = "0.1.0"
[xlings]
deps = [{ linxu = "qemu-user-aarch64" }]
)";
    auto m = load_from_string(src);
    ASSERT_FALSE(m.has_value());
    EXPECT_NE(m.error().message.find("linxu"), std::string::npos) << m.error().message;
    EXPECT_NE(m.error().message.find("linux, macos, windows, default"), std::string::npos)
        << m.error().message;
}

TEST(Manifest, ResolveHostValueTable) {
    using mcpp::manifest::resolve_host_value;
    auto d = mcpp::libs::toml::parse(R"(v = { linux = "a", macosx = "b", default = "c" })");
    ASSERT_TRUE(d.has_value());
    const auto& v = d->root().at("v");
    EXPECT_EQ(resolve_host_value(v, "linux").value().value(), "a");
    EXPECT_EQ(resolve_host_value(v, "macos").value().value(), "b");   // macosx alias
    EXPECT_EQ(resolve_host_value(v, "windows").value().value(), "c");
    auto e = mcpp::libs::toml::parse(R"(v = { linux = "a" })");
    EXPECT_FALSE(resolve_host_value(e->root().at("v"), "windows").value().has_value());
}
```

If `test_manifest.cpp` has no `load_from_string` helper, add one at the top of the file that writes `src` to `<temp>/mcpp.toml` and calls `mcpp::manifest::load(path)`; model it on the fixture at `:3240-3250`.

- [ ] **Step 2: Run to verify they fail**

Run: `mcpp build && mcpp test unit/test_manifest`
Expected: compile failure (`host_platform_key`, `resolve_host_value` undefined).

- [ ] **Step 3: Implement**

In `modules/manifest/src/toml.cppm`, before `load`:

```cpp
// The host platform in the vocabulary xlings' `.xlings.json` uses for a
// per-platform value, spelled with mcpp's OS names. `[xlings]` describes the
// environment of the machine mcpp runs on, so it is resolved against that
// machine when the manifest is loaded; every downstream reader keeps seeing a
// flat list, which is what keeps the provisioning pass, its stamp and the
// build-program hand-off unchanged.
export std::string_view host_platform_key() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__)
    return "macos";
#else
    return "linux";
#endif
}

// A value that is either a string or a table keyed by platform, as xlings'
// `workspace` allows: `"20"` or `{ linux = "15.1.0", default = "22" }`.
// `macosx` is accepted as xlings' own spelling of `macos`. A table with no
// entry for this host and no `default` resolves to nullopt, which the caller
// treats as "not declared on this host" — the same rule xlings applies.
// An unknown key is an error, not a dropped entry: a typo'd platform that
// silently declared nothing is the shape #531 was filed for.
export std::expected<std::optional<std::string>, std::string>
resolve_host_value(const mcpp::libs::toml::Value& v, std::string_view host) {
    if (v.is_string()) return std::optional<std::string>{v.as_string()};
    if (!v.is_table())
        return std::unexpected("expected a string or a { <platform> = \"...\" } table");
    static constexpr std::string_view kKnown[] = {"linux", "macos", "macosx", "windows", "default"};
    std::optional<std::string> chosen, fallback;
    for (auto& [k, val] : v.as_table()) {
        bool known = false;
        for (auto n : kKnown) if (k == n) { known = true; break; }
        if (!known)
            return std::unexpected(std::format(
                "unknown platform key '{}'; expected one of linux, macos, windows, default", k));
        if (!val.is_string())
            return std::unexpected(std::format("platform key '{}' must be a string", k));
        std::string_view canon = (k == "macosx") ? "macos" : std::string_view(k);
        if (canon == host) chosen = val.as_string();
        else if (canon == "default") fallback = val.as_string();
    }
    if (chosen) return chosen;
    return fallback;   // may be nullopt: not declared on this host
}
```

Replace the `[xlings]` block at `:1350-1361`:

```cpp
    // [xlings] — build environment (L-1). Subsections mirror .xlings.json 1:1,
    // including its per-platform value form, resolved here for this host.
    if (auto* arr = doc->get("xlings.deps"); arr && arr->is_array()) {
        std::size_t i = 0;
        for (auto& el : arr->as_array()) {
            auto r = resolve_host_value(el, host_platform_key());
            if (!r) return std::unexpected(error(origin,
                std::format("[xlings] deps[{}]: {}", i, r.error())));
            if (*r) m.xlings.deps.push_back(**r);
            ++i;
        }
    }
    ... subos unchanged ...
    if (auto* wt = doc->get_table("xlings.workspace"))
        for (auto& [k, val] : *wt) {
            auto r = resolve_host_value(val, host_platform_key());
            if (!r) return std::unexpected(error(origin,
                std::format("[xlings.workspace] {}: {}", k, r.error())));
            if (*r) m.xlings.workspace[k] = **r;
        }
```

(`doc->get` returns the raw value; check the `mcpp.libs.toml` accessor names in `modules/libs/toml` before writing — `get`, `get_table`, `as_array`, `is_table` are the names used elsewhere in this file.)

Sweep at `:1936-1959`:

```cpp
            static constexpr std::string_view kKnownTargetScalars[] = {
                "cxx_runtime", "linkage", "sysroot", "toolchain",
            };
            static constexpr std::string_view kKnownTargetArrays[] = { "runner" };
            for (auto& [key, value] : body) {
                if (value.is_table()) continue;   // the conditional channel
                const auto& known = value.is_array()
                    ? std::span<const std::string_view>(kKnownTargetArrays)
                    : std::span<const std::string_view>(kKnownTargetScalars);
                if (std::ranges::find(known, key) != known.end()) continue;
                // The list names every key this table reads, arrays included,
                // in one alphabetical line: a reader of the warning should not
                // have to know which type a key is to find it here.
                m.schemaWarnings.push_back(std::format(
                    "[target.{}] has unsupported key '{}' (ignored). Supported keys: "
                    "cxx_runtime, linkage, runner, sysroot, toolchain. "
                    "Per-role contracts go in [build].cxx_runtime's table form.",
                    triple, key));
            }
```

Update the comment block above it: the sweep now covers scalars and arrays; sub-tables remain excluded for the reason already recorded.

`types.cppm:763`: comment on `deps` — "resolved for this host at load; see `resolve_host_value`".

- [ ] **Step 4: Run to verify they pass**

Run: `mcpp build && mcpp test unit/test_manifest`
Expected: `ok.` Also run the existing `EveryBuildKeyTheParserReadsIsAccepted`-style negative controls; they must stay green.

- [ ] **Step 5: Commit**

```bash
git add modules/manifest tests/unit/test_manifest.cpp
git commit -m "manifest: [xlings] values per host platform; the target sweep reports array typos and lists runner (#544)"
```

---

### Task 4: Runner lookup and messages

**Files:**
- Create: `src/build/runner_lookup.cppm` (module `mcpp.build.runner_lookup`)
- Modify: `src/build/prepare.cppm:637-720` (`BuildContext` gains `xlingsDepBinDirs`), and the site after `fillXpkgDirs` (`:4549`) that has both `ctx` and the payload directories
- Test: `tests/unit/test_runner_lookup.cpp`

**Interfaces:**
- Produces:

```cpp
export namespace mcpp::build::runner_lookup {
struct Lookup {
    std::optional<std::filesystem::path> program;   // absolute, executable
    std::vector<std::filesystem::path>   searched;   // in order, for the message
};
// argv0 absolute or containing a separator: taken as-is when executable.
// Otherwise: <each depBinDir>/argv0, then each PATH entry (pathEnv split on
// the platform separator), first executable regular file wins.
Lookup locate(std::string_view argv0,
              std::span<const std::filesystem::path> depBinDirs,
              std::string_view pathEnv);
enum class SpawnClass { Unloadable, Other };
// ENOEXEC (and EBADARCH on macOS, when defined) → Unloadable; anything else → Other.
SpawnClass classify(int spawnErrno);
std::string not_found_message(std::string_view triple, std::string_view argv0,
                              std::span<const std::filesystem::path> searched);
std::string spawn_failed_message(std::string_view program, int spawnErrno);
std::string unrunnable_message(std::string_view triple, const std::filesystem::path& artifact,
                               int spawnErrno);
}
```

`BuildContext::xlingsDepBinDirs` is `std::vector<std::filesystem::path>`: `<payload>/bin` for each installed `[xlings] deps` payload of the runtime-owner manifest, declaration order.

- [ ] **Step 1: Write the failing tests** (`tests/unit/test_runner_lookup.cpp`)

```cpp
#include <gtest/gtest.h>
#include <cerrno>
#include <fstream>
import std;
import mcpp.build.runner_lookup;
using namespace mcpp::build::runner_lookup;

#if !defined(_WIN32)
namespace {
std::filesystem::path make_exe(const std::filesystem::path& dir, std::string_view name) {
    std::filesystem::create_directories(dir);
    auto p = dir / name;
    { std::ofstream o(p); o << "#!/bin/sh\nexit 0\n"; }
    std::filesystem::permissions(p, std::filesystem::perms::owner_all);
    return p;
}
}

TEST(RunnerLookup, PayloadBinBeatsPath) {
    auto root = std::filesystem::temp_directory_path() / "mcpp-runner-lookup";
    std::filesystem::remove_all(root);
    auto inPayload = make_exe(root / "payload" / "bin", "qemu-x");
    auto onPath    = make_exe(root / "path", "qemu-x");
    std::vector<std::filesystem::path> bins{root / "payload" / "bin"};
    auto l = locate("qemu-x", bins, (root / "path").string());
    ASSERT_TRUE(l.program.has_value());
    EXPECT_EQ(*l.program, inPayload);
}

TEST(RunnerLookup, PathIsSearchedAfterPayloads) {
    auto root = std::filesystem::temp_directory_path() / "mcpp-runner-lookup2";
    std::filesystem::remove_all(root);
    auto onPath = make_exe(root / "path", "qemu-y");
    std::vector<std::filesystem::path> bins{root / "payload" / "bin"};   // absent dir
    auto l = locate("qemu-y", bins, (root / "path").string());
    ASSERT_TRUE(l.program.has_value());
    EXPECT_EQ(*l.program, onPath);
    ASSERT_EQ(l.searched.size(), 2u);
    EXPECT_EQ(l.searched[0], root / "payload" / "bin");
}

TEST(RunnerLookup, NotFoundListsEveryDirectorySearched) {
    auto root = std::filesystem::temp_directory_path() / "mcpp-runner-lookup3";
    std::vector<std::filesystem::path> bins{root / "a" / "bin"};
    auto l = locate("nope", bins, (root / "p1").string() + ":" + (root / "p2").string());
    EXPECT_FALSE(l.program.has_value());
    ASSERT_EQ(l.searched.size(), 3u);
    auto msg = not_found_message("aarch64-linux-musl", "nope", l.searched);
    EXPECT_NE(msg.find("'nope'"), std::string::npos) << msg;
    EXPECT_NE(msg.find((root / "p2").string()), std::string::npos) << msg;
    EXPECT_NE(msg.find("[xlings]"), std::string::npos) << msg;
}

TEST(RunnerLookup, AbsoluteArgv0IsTakenAsIs) {
    auto root = std::filesystem::temp_directory_path() / "mcpp-runner-lookup4";
    auto abs = make_exe(root, "runner.sh");
    auto l = locate(abs.string(), {}, "");
    ASSERT_TRUE(l.program.has_value());
    EXPECT_EQ(*l.program, abs);
}

TEST(RunnerLookup, ClassifiesENOEXECAsUnloadable) {
    EXPECT_EQ(classify(ENOEXEC), SpawnClass::Unloadable);
    EXPECT_EQ(classify(EACCES), SpawnClass::Other);
    EXPECT_EQ(classify(ENOENT), SpawnClass::Other);
}

TEST(RunnerLookup, UnrunnableMessageNamesKernelAnswerTripleAndKey) {
    auto msg = unrunnable_message("aarch64-linux-musl", "/x/bin/app", ENOEXEC);
    EXPECT_NE(msg.find("Exec format error"), std::string::npos) << msg;
    EXPECT_NE(msg.find("aarch64-linux-musl"), std::string::npos);
    EXPECT_NE(msg.find("[target.aarch64-linux-musl]"), std::string::npos);
    EXPECT_NE(msg.find("runner = [\"qemu-aarch64-static\"]"), std::string::npos) << msg;
    EXPECT_NE(msg.find("--no-runner"), std::string::npos);
}
#endif
```

- [ ] **Step 2: Run to verify they fail**

Run: `mcpp build && mcpp test unit/test_runner_lookup`
Expected: build error, module `mcpp.build.runner_lookup` does not exist.

- [ ] **Step 3: Implement `src/build/runner_lookup.cppm`**

```cpp
// mcpp.build.runner_lookup — where the runner's program is, and what to say
// when it is not.
//
// The lookup is mcpp's own rather than posix_spawnp's for one measured reason:
// a bare name on PATH resolves to an xvm shim, and the shim answers for the
// current subos rather than for the package (e2e 130 in CI; `python3` on this
// machine). The payload's bin/ is the binary itself, so it is searched first.
// Doing the lookup here also splits "not found anywhere" (decided before any
// spawn) from a spawn-time ENOENT, which can then only mean "found, but its
// interpreter or loader is missing".
export module mcpp.build.runner_lookup;
import std;
import mcpp.platform;

export namespace mcpp::build::runner_lookup {

struct Lookup {
    std::optional<std::filesystem::path> program;
    std::vector<std::filesystem::path>   searched;
};

namespace detail {
inline bool executable_file(const std::filesystem::path& p) {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(p, ec)) return false;
    auto perms = std::filesystem::status(p, ec).permissions();
    using P = std::filesystem::perms;
    return (perms & (P::owner_exec | P::group_exec | P::others_exec)) != P::none;
}
}

inline Lookup locate(std::string_view argv0,
                     std::span<const std::filesystem::path> depBinDirs,
                     std::string_view pathEnv)
{
    Lookup out;
    std::filesystem::path a0(argv0);
    if (a0.is_absolute() || argv0.find('/') != std::string_view::npos
        || argv0.find('\\') != std::string_view::npos) {
        if (detail::executable_file(a0)) out.program = std::filesystem::absolute(a0);
        out.searched.push_back(a0.parent_path());
        return out;
    }
    for (auto const& d : depBinDirs) {
        out.searched.push_back(d);
        if (auto c = d / a0; detail::executable_file(c)) { out.program = c; return out; }
    }
    constexpr char sep = mcpp::platform::is_windows ? ';' : ':';
    for (auto part : std::views::split(pathEnv, sep)) {
        std::filesystem::path d(std::string_view(part.begin(), part.end()));
        if (d.empty()) continue;
        out.searched.push_back(d);
        if (auto c = d / a0; detail::executable_file(c)) { out.program = c; return out; }
    }
    return out;
}

enum class SpawnClass { Unloadable, Other };

inline SpawnClass classify(int e) {
    if (e == ENOEXEC) return SpawnClass::Unloadable;
#if defined(EBADARCH)
    if (e == EBADARCH) return SpawnClass::Unloadable;
#endif
    return SpawnClass::Other;
}

inline std::string errno_text(int e) { return std::generic_category().message(e); }

inline std::string not_found_message(std::string_view triple, std::string_view argv0,
                                     std::span<const std::filesystem::path> searched) {
    std::string dirs;
    for (auto const& d : searched) dirs += "\n           " + d.string();
    return std::format(
        "runner '{}' for '{}' was not found. Searched:{}\n"
        "       Declare the package that provides it under [xlings] deps, or "
        "install it on PATH.\n"
        "       Pass --no-runner to execute the artifact directly on this host.",
        argv0, triple, dirs);
}

inline std::string spawn_failed_message(std::string_view program, int e) {
    return std::format("runner '{}' could not be started: {} (error {})",
                       program, errno_text(e), e);
}

inline std::string unrunnable_message(std::string_view triple,
                                      const std::filesystem::path& artifact, int e) {
    return std::format(
        "this host cannot execute '{}': {} (error {}).\n"
        "       The artifact was built for '{}'. Declare how to run it here:\n"
        "\n"
        "           [target.{}]\n"
        "           runner = [\"qemu-aarch64-static\"]\n"
        "\n"
        "       The artifact path is appended, or substituted for `{{}}` if the "
        "template contains it.\n"
        "       A host that can execute it directly may pass --no-runner.",
        artifact.string(), errno_text(e), e, triple, triple);
}

} // namespace
```

Windows: `ENOEXEC` exists in `<cerrno>` on MSVC; `is_windows` is in `mcpp.platform`. Keep the module free of POSIX headers.

`BuildContext` in `prepare.cppm`: add after `depSourceRoots`:

```cpp
    // `<payload>/bin` of every installed `[xlings] deps` payload of the
    // runtime-owner manifest, in declaration order. Read by choose_runner's
    // lookup (mcpp.build.runner_lookup) so a runner may name a program the
    // project declared, without writing the payload's home-and-version path
    // into the manifest. Computed by the same resolution fillXpkgDirs uses for
    // build programs; a declared-but-absent payload contributes nothing.
    std::vector<std::filesystem::path> xlingsDepBinDirs;
```

Fill it next to `fillXpkgDirs` (`:4549`), from `runtimeOwnerManifest.xlings.deps` through `xpkg_payload`, appending `*dir / "bin"`. Confirm `ctx` is in scope at that point; if the lambda's enclosing function does not own `ctx`, compute the vector in the same function that builds the `BuildContext` and hand it in.

- [ ] **Step 4: Run to verify they pass**

Run: `mcpp build && mcpp test unit/test_runner_lookup`
Expected: `ok.`

- [ ] **Step 5: Commit**

```bash
git add src/build/runner_lookup.cppm src/build/prepare.cppm tests/unit/test_runner_lookup.cpp
git commit -m "build: runner lookup through declared payloads then PATH, with typed messages (#544)"
```

---

### Task 5: `choose_runner` on every target; `mcpp run`; `--no-runner`; fast-path marker

**Files:**
- Modify: `src/build/execute.cppm:53-95` (cache entry), `:150-215` (reader), `:280-300` (writer), `:420-451` (`choose_runner`), `:1100-1190` (`try_fast_run`), `:1191-1320` (`build_run_target`)
- Modify: `src/cli/cmd_build.cppm:194-218` (`cmd_run`), `src/cli.cppm:383-395` (option)
- Test: e2e in Task 7 (the unit-testable part is in Task 4)

**Interfaces:**
- `choose_runner(const BuildContext&, bool noRunner)`: `tmpl` filled for any target; `freestanding` unchanged in meaning; new field `bool ignored` when `noRunner` dropped a declared template (so the caller prints one note).
- `build_run_target(..., const std::string& target_triple = {}, bool no_runner = false)`.
- `BuildCacheEntry::runnerDeclared` (bool, line `runner=0|1`, written always; absent on old caches → false).

- [ ] **Step 1: `choose_runner`**

```cpp
struct RunnerChoice {
    std::vector<std::string> tmpl;
    bool freestanding = false;   // an EMPTY tmpl is fatal when true
    bool fromManifest = false;
    bool ignored = false;        // --no-runner dropped a declared template
};

RunnerChoice choose_runner(const BuildContext& ctx, bool noRunner = false) {
    RunnerChoice c;
    if (auto ft = mcpp::toolchain::triple::parse(ctx.tc.targetTriple))
        c.freestanding = ft->is_freestanding();
    // Both producers, for every target. The freestanding predicate used to
    // gate this read, which is how a runner declared under a hosted cross
    // triple was validated, documented and never consulted (#544). It now
    // decides one thing only: whether an absent runner is fatal.
    c.tmpl = ctx.manifest.buildConfig.runner;
    if (auto it = ctx.manifest.targetOverrides.find(ctx.tc.targetTriple);
        it != ctx.manifest.targetOverrides.end() && !it->second.runner.empty()) {
        c.tmpl = it->second.runner;
        c.fromManifest = !ctx.manifest.buildConfig.runner.empty();
    }
    if (noRunner && !c.tmpl.empty()) { c.tmpl.clear(); c.ignored = true; }
    return c;
}
```

`--no-runner` on a freestanding target with the template dropped: the existing `no_runner_message` path fires, which is the correct answer (there is nothing to execute directly).

- [ ] **Step 2: `mcpp run` launch (replace `execute.cppm:1264-1319`)**

```cpp
    const auto choice = choose_runner(*ctx, no_runner);
    if (choice.ignored)
        mcpp::ui::info("note", std::format(
            "--no-runner: ignoring the runner declared for {}", ctx->tc.targetTriple));
    if (choice.fromManifest)
        mcpp::ui::info("note", std::format(
            "[target.{}].runner overrides the runner a dependency supplied",
            ctx->tc.targetTriple));
    if (choice.freestanding && choice.tmpl.empty()) {
        std::println(stderr, "error: {}",
            mcpp::freestanding::no_runner_message(ctx->tc.targetTriple));
        return 2;
    }
    if (!choice.tmpl.empty()) {
        auto tmpl = choice.tmpl;
        const char* pathEnv = std::getenv("PATH");
        auto found = mcpp::build::runner_lookup::locate(
            tmpl.front(), ctx->xlingsDepBinDirs, pathEnv ? pathEnv : "");
        if (!found.program) {
            std::println(stderr, "error: {}", mcpp::build::runner_lookup::not_found_message(
                ctx->tc.targetTriple, tmpl.front(), found.searched));
            return 2;
        }
        tmpl.front() = found.program->string();
        argv = mcpp::freestanding::expand(tmpl, exe);
        for (auto& a : passthrough) argv.push_back(a);
        mcpp::ui::status("Running", std::format("`{} … {}`", choice.tmpl.front(),
            mcpp::ui::shorten_path(exe, pathCtx)));
    } else {
        argv.push_back(exe.string());
        for (auto& a : passthrough) argv.push_back(a);
        mcpp::ui::status("Running", std::format("`{}`", mcpp::ui::shorten_path(exe, pathCtx)));
    }
    std::println(""); std::fflush(stdout);
    ... childEnv as today ...
    int spawnErr = 0;
    int rc = mcpp::platform::process::run_exec(argv, childEnv, &spawnErr);
    if (spawnErr != 0) {
        using namespace mcpp::build::runner_lookup;
        if (!choice.tmpl.empty())
            std::println(stderr, "error: {}", spawn_failed_message(argv.front(), spawnErr));
        else if (classify(spawnErr) == SpawnClass::Unloadable)
            std::println(stderr, "error: {}", unrunnable_message(ctx->tc.targetTriple, exe, spawnErr));
        else
            std::println(stderr, "error: {}", spawn_failed_message(exe.string(), spawnErr));
        return 2;
    }
    return rc == 0 ? 0 : 1;
```

Add `import mcpp.build.runner_lookup;` to `execute.cppm`.

- [ ] **Step 3: Fast path**

`BuildCacheEntry`: add `bool runnerDeclared = false;` with a comment: "a runner declared for this target makes the fast path a miss, because the fast path has no manifest to read the template from and must not execute the artifact bare when the prepare path would not". Writer: `f << "runner=" << (e.runnerDeclared ? 1 : 0) << '\n';` after the `subos=` line. Reader: optional line `runner=`. Where the cache entry is populated (search `runTargets =` assignments in `run_build_plan`/`prepare` — `git grep -n "\.runTargets" src/build`), set `e.runnerDeclared = !choose_runner(ctx).tmpl.empty();`. In `try_fast_run`, after `if (!chosen) return std::nullopt;`: `if (match->runnerDeclared) return std::nullopt;`. Also make `try_fast_run` pass `&spawnErr` to `run_exec` and print `unrunnable_message`/`spawn_failed_message` on failure with exit 2, so the two doors behave the same.

- [ ] **Step 4: CLI**

`src/cli.cppm` run subcommand: `.option(cl::Option("no-runner").help("Execute the artifact directly, ignoring any [target.<triple>].runner (a host that can run it natively)"))`. Same option on `test`. `cmd_run`: `bool no_runner = parsed.is_flag_set("no-runner");` and pass it through. Help line at `src/cli.cppm:62` for `test`: append `--no-runner`.

- [ ] **Step 5: Build and smoke by hand**

```bash
mcpp build
cd $(mktemp -d) && $MCPP new probe >/dev/null && cd probe
printf '\n[target.x86_64-linux-gnu]\nrunner = ["/bin/sh", "-c", "echo RUNNER-SAW \"$@\"", "wrap"]\n' >> mcpp.toml
$MCPP run                       # expect RUNNER-SAW <artifact path>
$MCPP run                       # second run: fast path must still go through the runner (marker)
$MCPP run --no-runner           # expect the note and the program's own output
```

(Use the triple `$MCPP toolchain list` reports for the host if it is not `x86_64-linux-gnu`.)

- [ ] **Step 6: Commit**

```bash
git add src/build/execute.cppm src/cli/cmd_build.cppm src/cli.cppm
git commit -m "run: honour the declared runner on every target; --no-runner; typed launch failures (#544)"
```

---

### Task 6: `mcpp test` — `NotRun`, one reason, exit 2

**Files:**
- Modify: `src/build/execute.cppm:1324-1345` (`TestOptions` gains `bool noRunner`), `:1346-1356` (`TestRunSummary` gains `int notRun`, `std::string notRunReason`), `:1527-1560` (`TestResult::St::NotRun`, JSON), `:1711-1800` (workers), `:1855-1905` (argv build), `:1905-1950` (summary)
- Modify: `src/cli/cmd_build.cppm:221-262` (`to.noRunner = parsed.is_flag_set("no-runner")`), and the `--workspace` aggregation at `:280-395` (count `notRun` into the workspace summary line as `; N not run`)

- [ ] **Step 1: State and JSON**

```cpp
enum class St { Pass, CompileFail, RunFail, NotRun } status;
...
const char* st = r.status == TestResult::St::Pass ? "pass"
               : r.status == TestResult::St::CompileFail ? "compile_fail"
               : r.status == TestResult::St::NotRun ? "not_run"
                                                    : "run_fail";
// record gains `"reason":"<escaped or empty>"` after `timed_out`.
```

Add `std::string reason;` to `TestResult`.

- [ ] **Step 2: Runner resolution once, before Pass 2**

Where argv is built per test (`:1855-1875`), replace the freestanding gate:

```cpp
        const auto choice = choose_runner(*ctx, testOpts.noRunner);
        // resolved once per invocation, outside the loop: hoist `choice` and
        // the lookup above the `for (auto& lu : ...)` loop.
```

Above the loop:

```cpp
    const auto choice = choose_runner(*ctx, testOpts.noRunner);
    if (choice.ignored && !json) mcpp::ui::info("note", "--no-runner: ignoring the declared runner");
    if (choice.freestanding && choice.tmpl.empty()) { ...unchanged error, return 2... }
    std::vector<std::string> runnerTmpl = choice.tmpl;
    std::string invocationNotRunReason;      // non-empty ⇒ nothing is spawned
    if (!runnerTmpl.empty()) {
        const char* pathEnv = std::getenv("PATH");
        auto found = mcpp::build::runner_lookup::locate(
            runnerTmpl.front(), ctx->xlingsDepBinDirs, pathEnv ? pathEnv : "");
        if (found.program) runnerTmpl.front() = found.program->string();
        else invocationNotRunReason = mcpp::build::runner_lookup::not_found_message(
            ctx->tc.targetTriple, runnerTmpl.front(), found.searched);
    }
```

Per test: `argv = runnerTmpl.empty() ? {exe} : expand(runnerTmpl, exe)`.

- [ ] **Step 3: Workers**

Add `std::atomic<bool> hostCannotRun{false}; std::string hostCannotRunReason; std::mutex reasonMutex;` next to `next`. In the worker, before spawning:

```cpp
                if (!invocationNotRunReason.empty() || hostCannotRun.load()) {
                    std::scoped_lock lock(reportMutex);
                    if (!json) mcpp::ui::plain(std::format("{} ... not run", r.name));
                    results.push_back({r.name, TestResult::St::NotRun, 0, {}, {}, 0, false,
                                       invocationNotRunReason.empty() ? hostCannotRunReason
                                                                      : invocationNotRunReason});
                    emit_json(results.back());
                    continue;
                }
                int spawnErr = 0;
                ... capture_exec_deadline(..., &spawnErr) / run_exec_deadline(..., &spawnErr) ...
                if (spawnErr != 0) {
                    std::string reason = runnerTmpl.empty()
                        ? (runner_lookup::classify(spawnErr) == SpawnClass::Unloadable
                              ? std::format("this host cannot execute {} artifacts: {}",
                                            ctx->tc.targetTriple, runner_lookup::errno_text(spawnErr))
                              : runner_lookup::spawn_failed_message(r.argv.front(), spawnErr))
                        : runner_lookup::spawn_failed_message(r.argv.front(), spawnErr);
                    {
                        std::scoped_lock lock(reportMutex);
                        if (!hostCannotRun.exchange(true)) {
                            hostCannotRunReason = reason;
                            if (!json) mcpp::ui::warning(reason);   // printed once
                        }
                        if (!json) mcpp::ui::plain(std::format("{} ... not run", r.name));
                        results.push_back({r.name, TestResult::St::NotRun, 0, {}, {}, ms, false, reason});
                        emit_json(results.back());
                    }
                    continue;
                }
```

Keep `TestResult`'s field order consistent with these brace-initialisers (add `reason` last).

- [ ] **Step 4: Summary**

```cpp
    int passed = 0, failed = 0, notRun = 0;
    std::string notRunReason;
    for (auto& r : results) {
        if (r.status == TestResult::St::Pass) ++passed;
        else if (r.status == TestResult::St::NotRun) { ++notRun; if (notRunReason.empty()) notRunReason = r.reason; }
        else { ++failed; failures.push_back(r.name); }
    }
    summary.notRun = notRun; summary.notRunReason = notRunReason;
    // JSON summary: add "not_run":N,"not_run_reason":"…"
    ...
    const int rc = failed ? 1 : (notRun ? 2 : 0);
    if (json) { ...print...; return rc; }
    std::println("");
    auto counts = std::format("{} passed; {} failed", passed, failed);
    if (notRun) counts += std::format("; {} not run ({})", notRun, first_line(notRunReason));
    if (rc == 0) { mcpp::ui::status("test result", std::format("ok. {}; finished in {}", counts, timing)); return 0; }
    mcpp::ui::error(std::format("test result: {}. {}; finished in {}",
                                failed ? "FAILED" : "NOT RUN", counts, timing));
    if (failed) { println failures block as today }
    return rc;
```

`first_line` = the reason up to its first `\n`. Workspace fan-out in `cmd_build.cppm`: sum `sum.notRun`, print `; N not run` in the workspace line when non-zero, and make the workspace rc 2 when any member returned 2 and none returned 1.

- [ ] **Step 5: Build, then hand-check with a two-test project and the patched-artifact technique from Task 7**

- [ ] **Step 6: Commit**

```bash
git add src/build/execute.cppm src/cli/cmd_build.cppm
git commit -m "test: unrunnable artifacts are reported not-run once, with the reason, and exit 2 (#544)"
```

---

### Task 7: e2e 330

**Files:**
- Create: `tests/e2e/330_runner_hosted_targets.sh`

- [ ] **Step 1: Write the script**

```bash
#!/usr/bin/env bash
# requires: unix-shell
# 330_runner_hosted_targets.sh — [target.<triple>].runner on a hosted target,
# and what `mcpp run` / `mcpp test` say when the host cannot execute an artifact.
#
# Issue #544. Design: .agents/docs/2026-09-02-runner-beyond-baremetal-design.md §11.
#
# No emulator is needed and none is used: the runner is a shell script that
# records its argv, and the "unloadable artifact" is a real host binary whose
# ELF e_machine is patched to 0xffff after the build, which no binfmt entry and
# no native loader accepts (ENOEXEC on any Linux host, binfmt_misc or not).
# `requires: unix-shell` and nothing else: a `requires: gcc` or `requires: llvm`
# guard skips on both CI shards, and a skip exits 0.
set -uo pipefail
TMP=$(mktemp -d); trap "rm -rf $TMP" EXIT; cd "$TMP"
fail() { echo "FAIL: $*"; exit 1; }
MCPP="${MCPP:?set MCPP to the binary under test}"

HOST=$("$MCPP" toolchain list --format json 2>/dev/null | sed -n 's/.*"host":"\([^"]*\)".*/\1/p' | head -1)
[[ -n "$HOST" ]] || HOST=$(uname -m)-linux-gnu   # fallback: adjust if toolchain list has no host field
case "$(uname -s)" in Darwin) echo "SKIP: e_machine patching is ELF-only; the runner half runs below";; esac

"$MCPP" new app >/dev/null || fail "mcpp new"
cd app
mkdir -p tests
cat > src/main.cpp <<'EOF'
#include <cstdio>
int main() { std::puts("ARTIFACT-RAN"); return 0; }
EOF
cat > tests/one.cpp <<'EOF'
int main() { return 0; }
EOF
cat > "$TMP/runner.sh" <<'EOF'
#!/bin/sh
printf '%s\n' "$@" >> "$RUNNER_LOG"
exec "$@"
EOF
chmod +x "$TMP/runner.sh"
export RUNNER_LOG="$TMP/runner.log"

# ── 1. a declared runner is used on a hosted target, on both doors ─────────
printf '\n[target.%s]\nrunner = ["%s"]\n' "$HOST" "$TMP/runner.sh" >> mcpp.toml
out=$("$MCPP" run 2>&1) || fail "run through runner: $out"
grep -q "ARTIFACT-RAN" <<<"$out" || fail "artifact output missing: $out"
[[ -s "$RUNNER_LOG" ]] || fail "runner was not invoked"
exe=$(head -1 "$RUNNER_LOG")
[[ "$exe" == */bin/app ]] || fail "runner argv[0] is not the artifact: $exe"
: > "$RUNNER_LOG"
out=$("$MCPP" run 2>&1) || fail "second run (fast path): $out"
[[ -s "$RUNNER_LOG" ]] || fail "fast path bypassed the runner"

# ── 2. --no-runner executes directly and says so ───────────────────────────
: > "$RUNNER_LOG"
out=$("$MCPP" run --no-runner 2>&1) || fail "--no-runner: $out"
grep -q "ARTIFACT-RAN" <<<"$out" || fail "--no-runner lost the program output: $out"
grep -q "no-runner" <<<"$out" || fail "--no-runner printed no note: $out"
[[ ! -s "$RUNNER_LOG" ]] || fail "--no-runner still invoked the runner"

# ── 3. runner not found: error names the program, the search list, exit 2 ──
sed -i "s|runner = \[.*\]|runner = [\"mcpp-e2e-no-such-runner\"]|" mcpp.toml
out=$("$MCPP" run 2>&1); rc=$?
[[ $rc -eq 2 ]] || fail "missing runner: exit $rc, want 2: $out"
grep -q "runner 'mcpp-e2e-no-such-runner'" <<<"$out" || fail "missing runner not named: $out"
grep -q "Searched:" <<<"$out" || fail "search list missing: $out"
grep -q "ARTIFACT-RAN" <<<"$out" && fail "artifact ran without its runner"
out=$("$MCPP" test 2>&1); rc=$?
[[ $rc -eq 2 ]] || fail "test with missing runner: exit $rc, want 2: $out"
grep -qE "NOT RUN\. 0 passed; 0 failed; 1 not run \(runner 'mcpp-e2e-no-such-runner'" <<<"$out" \
    || fail "test summary: $out"

# ── 4. no runner, unloadable artifact (ELF only) ───────────────────────────
if [[ "$(uname -s)" == Linux ]]; then
  sed -i "/^\[target\.$HOST\]/,+1d" mcpp.toml
  "$MCPP" build >/dev/null 2>&1 || fail "rebuild without runner"
  bin=$(ls target/*/*/bin/app | head -1)
  printf '\xff\xff' | dd of="$bin" bs=1 seek=18 conv=notrunc status=none
  out=$("$MCPP" run 2>&1); rc=$?
  [[ $rc -eq 2 ]] || fail "unrunnable: exit $rc, want 2: $out"
  grep -q "Exec format error" <<<"$out" || fail "kernel answer missing: $out"
  grep -q "\[target.$HOST\]" <<<"$out" || fail "paste-able key missing: $out"
  grep -q "runner = \[" <<<"$out" || fail "runner example missing: $out"
  # the artifact must still be the patched one, or the criterion measured a rebuild
  od -An -tx1 -j18 -N2 "$bin" | grep -q "ff ff" || fail "artifact was rebuilt under the run"
  # mcpp test: one test and two tests, streaming and capturing paths
  cat > tests/two.cpp <<'EOF'
int main() { return 0; }
EOF
  for n in 1 2; do
    [[ $n -eq 1 ]] && rm -f tests/two.cpp
    "$MCPP" build >/dev/null 2>&1
    "$MCPP" test --list >/dev/null 2>&1
    # build the test binaries once so they exist to patch: run the suite (it runs them), then patch and rerun
    "$MCPP" test >/dev/null 2>&1 || true
    for t in target/*/*/tests/bin/*; do [[ -f "$t" ]] && printf '\xff\xff' | dd of="$t" bs=1 seek=18 conv=notrunc status=none; done
    out=$("$MCPP" test 2>&1); rc=$?
    [[ $rc -eq 2 ]] || fail "test unrunnable ($n): exit $rc, want 2: $out"
    grep -qE "NOT RUN\. 0 passed; 0 failed; $n not run \(this host cannot execute" <<<"$out" \
        || fail "test summary ($n): $out"
    [[ $(grep -c "cannot execute" <<<"$out") -eq 2 ]] || fail "reason printed other than once + summary ($n): $out"
    jout=$("$MCPP" test --message-format json 2>/dev/null)
    [[ $(grep -c '"status":"not_run"' <<<"$jout") -eq $n ]] || fail "json not_run count ($n): $jout"
    grep -q "\"not_run\":$n" <<<"$jout" || fail "json summary not_run ($n): $jout"
  done
fi

# ── 5. array-valued typo is reported and runner is in the list ─────────────
printf '\n[target.%s]\nrunnerX = ["x"]\n' "$HOST" >> mcpp.toml
out=$("$MCPP" build 2>&1)
grep -q "unsupported key 'runnerX'" <<<"$out" || fail "array typo not reported: $out"
grep -q "Supported keys: cxx_runtime, linkage, runner, sysroot, toolchain" <<<"$out" || fail "runner not listed: $out"
echo "PASS: 330_runner_hosted_targets"
```

Adjust the test-binary location (`target/*/*/tests/bin/*`) to where `mcpp test` actually places them (check `ls target/*/*/` after a run) and the host-triple discovery to what `mcpp toolchain list` prints; both are facts to read from the built engine, not to guess.

- [ ] **Step 2: Run against the unfixed engine to record the failure, then against the fixed one**

```bash
MCPP=$(which mcpp) bash tests/e2e/330_runner_hosted_targets.sh   # released 2026.8.17.1: must FAIL at section 1
MCPP=$PWD/target/*/*/bin/mcpp bash tests/e2e/330_runner_hosted_targets.sh   # must PASS
```

Also re-run the neighbours: `130`, `131`, `132`, `178`.

- [ ] **Step 3: Commit**

```bash
git add tests/e2e/330_runner_hosted_targets.sh
git commit -m "e2e: runner on hosted targets, not-run reporting, --no-runner (#544)"
```

---

### Task 8: Documentation (English and Chinese)

**Files:**
- Modify: `docs/05-mcpp-toml.md` (§2.7.1 table row; new §2.7.3 "Running an artifact this host cannot execute" placed after §2.7.2 with the two forms and the `[xlings] deps` consequence; §2.13 per-platform values), `docs/13-baremetal.md` (cross-reference under "An absent runner"), `docs/15-openkal-cross.md` (hosted example after `:190`), `docs/17-the-project-environment.md` (per-platform values), `docs/11-machine-output.md` (new §7 kind `mcpp.test` describing the NDJSON record and summary, with `not_run`)
- Modify: the five `docs/zh/` twins with the same content in Chinese.

- [ ] **Step 1: Write the English sections** (register per `.agents/skills/mcpp-docs-style/SKILL.md`; declarative headings; state the `[xlings] deps` consequence in one paragraph; state that `[xlings]` per-platform values are resolved against the host at load, that the keys are `linux`, `macos`, `windows`, `default`, that an entry without a match is not declared on that host, and that an unknown key is an error).
- [ ] **Step 2: Write the Chinese twins.**
- [ ] **Step 3: Run the style check and the doc parity check**

```bash
bash .github/tools/check_docs_style.sh
```

- [ ] **Step 4: Commit**

```bash
git add docs
git commit -m "docs: runner on hosted targets, not-run reporting, [xlings] per-platform values (#544)"
```

---

### Task 9: Version bump, PR, CI

- [ ] **Step 1: Bump** `mcpp.toml` version and `modules/versioning/src/version.cppm` to `2026.9.2.1` (next free `.N` for today; check `git tag -l 'v2026.9.2.*'`). Run `bash .github/tools/check_version_pins.sh`.
- [ ] **Step 2: Full local gates**: `mcpp build && mcpp test` (unit), `MCPP=... bash tests/e2e/330_runner_hosted_targets.sh`, `bash .github/tools/check_modules_wiring.sh`, `bash .github/tools/check_docs_style.sh`, `git diff origin/main --diff-filter=D --name-only` must be empty, `git log --oneline origin/main..HEAD` shows only this branch's commits.
- [ ] **Step 3: Push and open the PR** with a body in English academic register: the defect, the three decisions (no fallback, exit 2, `--no-runner`), the `[xlings]` per-platform addition, the test criteria, and the behaviour changes for existing manifests. Link #544.
- [ ] **Step 4: Watch CI** (`gh pr checks <n> --watch`); fix and push until every required check is green.

---

### Task 10: Self-review, merge, release, ecosystem verification

- [ ] **Step 1: Self-review** the PR diff against design §0 and §11 (each D1–D6 has a visible implementation or a stated deferral; every criterion row in §11 has an assertion in e2e 330; no criterion is a substring search of a line that could be silent). Use `/code-review` on the PR.
- [ ] **Step 2: Merge** with `gh pr merge <n> --squash --admin`; confirm `gh pr view <n> --json state,mergeCommit`; confirm the run on `origin/main`'s HEAD SHA is green.
- [ ] **Step 3: Release**: `git tag v2026.9.2.1 <merge-sha> && git push origin v2026.9.2.1`; watch `release.yml`; if `publish-ecosystem`'s GitCode leg times out, download the missing assets from the verified GitHub release and upload with `/usr/bin/python3 ~/.local/bin/gtc release upload xlings-res/mcpp <file> --tag 2026.9.2.1`, then GET-verify each asset from `https://gitcode.com/xlings-res/mcpp/releases/download/2026.9.2.1/<asset>` with `cmp`. Merge the index bump PR on `openxlings/xim-pkgindex` (switch `gh auth` to the account with push, switch back after), confirm `git show origin/main:pkgs/m/mcpp.lua` has `latest` at `2026.9.2.1`, then bump the bootstrap pin in `.xlings.json` through a PR.
- [ ] **Step 4: Sandbox verification**: `xlings update`; `xlings subos create e544-<date>`; inside `xlings subos use e544-<date> --sandbox --cmd "..."`: configure `mcpp self config --mirror CN`, install `mcpp@2026.9.2.1`, then run a base64-inlined script that (a) builds an openkal cross project for `aarch64-linux-musl` with `[xlings] deps = [{ linux = "qemu-user-aarch64" }]` and `runner = ["qemu-aarch64-static"]`, runs it, and asserts the program output; (b) runs `mcpp test --target aarch64-linux-musl` and asserts the summary counts; (c) without the runner asserts the `Exec format error` message and exit 2. Assert the installed version with `mcpp --version` at the start of the script, with a lower bound not an exact match.
- [ ] **Step 5: Record** the sandbox transcript summary in `.agents/docs/2026-09-02-runner-beyond-baremetal-design.md` §13 "Verification" with the exact commands and the lines observed.
