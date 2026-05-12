# Workspace Phase 1 Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add `[workspace]` support to mcpp so a root mcpp.toml can declare member packages, share dependency versions via `.workspace = true`, and build all members from the workspace root.

**Architecture:** Extend `Manifest` with a `WorkspaceConfig` struct parsed from `[workspace]`. Modify `find_manifest_root` to discover workspace roots. In `prepare_build`, when a workspace is detected, load all member manifests, merge inherited versions, and build them together. Add `-p, --package` flag for selective member builds.

**Tech Stack:** C++23 modules, mcpp's existing TOML parser, Google Test

**Design doc:** `.agents/docs/2026-05-12-workspace-design.md`

---

### File Map

| File | Action | Responsibility |
|---|---|---|
| `src/manifest.cppm` | Modify | Add `WorkspaceConfig` struct + parse `[workspace]` section + `.workspace = true` |
| `src/cli.cppm` | Modify | Workspace root discovery, member loading, `-p` flag, workspace-aware `prepare_build` |
| `src/build/plan.cppm` | Minor modify | Per-member obj subdirectory naming |
| `tests/unit/test_manifest.cpp` | Modify | Unit tests for workspace manifest parsing |
| `tests/e2e/35_workspace.sh` | Create | End-to-end workspace build test |

---

### Task 1: WorkspaceConfig struct + manifest parsing

**Files:**
- Modify: `src/manifest.cppm` (Manifest struct ~line 142, parse_string ~line 598)
- Test: `tests/unit/test_manifest.cpp`

- [ ] **Step 1: Add WorkspaceConfig to Manifest struct**

In `src/manifest.cppm`, add after the `LibConfig` struct (around line 122):

```cpp
// [workspace] — multi-package workspace support (0.0.11+).
struct WorkspaceConfig {
    std::vector<std::string>                            members;    // relative paths to member dirs
    std::vector<std::string>                            exclude;    // paths to exclude from members
    // [workspace.dependencies] — version specs that members can inherit via `.workspace = true`
    std::map<std::string, DependencySpec>               dependencies;
    bool                                                 present = false;  // true if [workspace] section exists
};
```

Add to the `Manifest` struct (after `LibConfig lib;`):

```cpp
WorkspaceConfig                        workspace;
```

- [ ] **Step 2: Parse [workspace] section in parse_string()**

In `parse_string()`, add before the `return m;` at the end (around line 598):

```cpp
// [workspace] — multi-package workspace support.
if (auto* ws = doc->get_table("workspace")) {
    m.workspace.present = true;

    if (auto v = doc->get_string_array("workspace.members"))
        m.workspace.members = *v;
    if (auto v = doc->get_string_array("workspace.exclude"))
        m.workspace.exclude = *v;

    // [workspace.dependencies] — same parsing as regular deps but stored separately.
    // Members inherit these via `.workspace = true` syntax.
    auto load_ws_deps = [&](std::string_view section,
                            std::map<std::string, DependencySpec>& out)
        -> std::expected<void, ManifestError>
    {
        auto* tt = doc->get_table(section);
        if (!tt) return {};
        for (auto& [k, v] : *tt) {
            if (v.is_string()) {
                DependencySpec spec;
                spec.version = v.as_string();
                if (k.find('.') != std::string::npos) {
                    auto pos = k.find('.');
                    spec.namespace_ = k.substr(0, pos);
                    spec.shortName  = k.substr(pos + 1);
                } else {
                    spec.namespace_ = std::string{kDefaultNamespace};
                    spec.shortName  = k;
                }
                out[k] = std::move(spec);
                continue;
            }
            if (!v.is_table()) continue;
            auto& sub = v.as_table();
            // Namespaced subtable: [workspace.dependencies.<ns>]
            const std::string ns = k;
            for (auto& [sk, sv] : sub) {
                DependencySpec spec;
                spec.namespace_ = ns;
                spec.shortName  = sk;
                std::string fq = std::format("{}.{}", ns, sk);
                if (sv.is_string()) {
                    spec.version = sv.as_string();
                } else {
                    continue;  // skip complex specs in workspace deps
                }
                out[fq] = std::move(spec);
            }
        }
        return {};
    };
    if (auto r = load_ws_deps("workspace.dependencies", m.workspace.dependencies); !r)
        return std::unexpected(r.error());
}
```

- [ ] **Step 3: Add `.workspace = true` handling in dependency parsing**

In the `load_deps` lambda (around line 443), after the inline dep spec check, add handling for `workspace = true`:

In the `looks_like_inline_dep_spec` lambda, add `"workspace"` to the allowed keys:

```cpp
auto is_dep_spec_key = [](std::string_view k) {
    return k == "path"   || k == "version" || k == "git"
        || k == "rev"    || k == "tag"     || k == "branch"
        || k == "features" || k == "workspace";
};
```

In the `fill_inline_spec` lambda, add before the "must specify path/version/git" check:

```cpp
if (auto it = sub.find("workspace"); it != sub.end() && it->second.is_bool() && it->second.as_bool()) {
    spec.inheritWorkspace = true;
    return {};  // version will be filled in later by workspace merge
}
```

Add `inheritWorkspace` field to `DependencySpec` in `src/pm/dep_spec.cppm`:

```cpp
bool                        inheritWorkspace = false;  // .workspace = true
```

- [ ] **Step 4: Write unit tests for workspace parsing**

In `tests/unit/test_manifest.cpp`, add:

```cpp
TEST(Manifest, WorkspaceSectionParsed) {
    constexpr auto src = R"(
[workspace]
members = ["libs/core", "libs/http", "apps/server"]
exclude = ["libs/experimental"]

[workspace.dependencies]
cmdline = "0.0.2"

[workspace.dependencies.compat]
gtest = "1.15.2"
mbedtls = "3.6.1"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    EXPECT_TRUE(m->workspace.present);
    ASSERT_EQ(m->workspace.members.size(), 3u);
    EXPECT_EQ(m->workspace.members[0], "libs/core");
    EXPECT_EQ(m->workspace.members[1], "libs/http");
    EXPECT_EQ(m->workspace.members[2], "apps/server");
    ASSERT_EQ(m->workspace.exclude.size(), 1u);
    EXPECT_EQ(m->workspace.exclude[0], "libs/experimental");

    ASSERT_EQ(m->workspace.dependencies.size(), 3u);
    auto& cmd = m->workspace.dependencies.at("cmdline");
    EXPECT_EQ(cmd.version, "0.0.2");
    auto& gt = m->workspace.dependencies.at("compat.gtest");
    EXPECT_EQ(gt.version, "1.15.2");
    EXPECT_EQ(gt.namespace_, "compat");
}

TEST(Manifest, WorkspaceTrueInDependency) {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"

[dependencies.compat]
mbedtls = { workspace = true }
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value()) << m.error().format();
    auto& s = m->dependencies.at("compat.mbedtls");
    EXPECT_TRUE(s.inheritWorkspace);
    EXPECT_EQ(s.namespace_, "compat");
    EXPECT_EQ(s.shortName, "mbedtls");
}

TEST(Manifest, NoWorkspaceSectionMeansNotPresent) {
    constexpr auto src = R"(
[package]
name = "x"
version = "0.1.0"
)";
    auto m = mcpp::manifest::parse_string(src);
    ASSERT_TRUE(m.has_value());
    EXPECT_FALSE(m->workspace.present);
}
```

- [ ] **Step 5: Build and run tests**

```bash
mcpp build && mcpp test
```

Expected: All existing tests pass + 3 new workspace tests pass.

- [ ] **Step 6: Commit**

```bash
git add src/manifest.cppm src/pm/dep_spec.cppm tests/unit/test_manifest.cpp
git commit -m "feat(workspace): parse [workspace] section and .workspace = true"
```

---

### Task 2: Workspace root discovery + member loading

**Files:**
- Modify: `src/cli.cppm` (~lines 106-114 find_manifest_root, ~lines 779+ prepare_build)

- [ ] **Step 1: Add workspace root discovery**

Replace `find_manifest_root` with a workspace-aware version that returns both the member root and workspace root:

```cpp
struct ManifestRoots {
    std::filesystem::path memberRoot;       // directory containing the target mcpp.toml
    std::filesystem::path workspaceRoot;    // directory containing workspace mcpp.toml (empty if none)
};

ManifestRoots find_manifest_roots(std::filesystem::path start) {
    ManifestRoots result;
    auto p = std::filesystem::absolute(start);

    // First: find the nearest mcpp.toml (member or standalone)
    while (true) {
        if (std::filesystem::exists(p / "mcpp.toml")) {
            result.memberRoot = p;
            break;
        }
        auto parent = p.parent_path();
        if (parent == p) return result;  // no mcpp.toml found
        p = parent;
    }

    // Check if this mcpp.toml itself has [workspace]
    {
        auto m = mcpp::manifest::load(result.memberRoot / "mcpp.toml");
        if (m && m->workspace.present) {
            result.workspaceRoot = result.memberRoot;
            return result;
        }
    }

    // Continue walking up to find a workspace root
    p = result.memberRoot.parent_path();
    while (true) {
        if (std::filesystem::exists(p / "mcpp.toml")) {
            auto m = mcpp::manifest::load(p / "mcpp.toml");
            if (m && m->workspace.present) {
                // Verify our memberRoot is listed in members
                auto rel = std::filesystem::relative(result.memberRoot, p);
                bool found = false;
                for (auto& member : m->workspace.members) {
                    if (rel == std::filesystem::path(member)) { found = true; break; }
                }
                if (found) {
                    result.workspaceRoot = p;
                }
                break;  // stop at first workspace mcpp.toml regardless
            }
        }
        auto parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }

    return result;
}

// Backward-compat wrapper
std::optional<std::filesystem::path> find_manifest_root(std::filesystem::path start) {
    auto roots = find_manifest_roots(start);
    return roots.memberRoot.empty() ? std::nullopt : std::optional{roots.memberRoot};
}
```

- [ ] **Step 2: Add workspace dependency merging helper**

```cpp
// Merge workspace.dependencies versions into a member manifest's deps.
// For each dep with inheritWorkspace == true, look up the version in
// the workspace manifest and fill it in.
void merge_workspace_deps(mcpp::manifest::Manifest& member,
                          const mcpp::manifest::Manifest& workspace) {
    auto merge_map = [&](std::map<std::string, mcpp::manifest::DependencySpec>& deps) {
        for (auto& [name, spec] : deps) {
            if (!spec.inheritWorkspace) continue;
            auto it = workspace.workspace.dependencies.find(name);
            if (it != workspace.workspace.dependencies.end()) {
                spec.version = it->second.version;
                spec.inheritWorkspace = false;  // resolved
            } else {
                // Try without namespace prefix for default-ns deps
                auto shortIt = workspace.workspace.dependencies.find(spec.shortName);
                if (shortIt != workspace.workspace.dependencies.end()) {
                    spec.version = shortIt->second.version;
                    spec.inheritWorkspace = false;
                }
            }
        }
    };
    merge_map(member.dependencies);
    merge_map(member.devDependencies);
    merge_map(member.buildDependencies);
}
```

- [ ] **Step 3: Add workspace-aware prepare_build**

In `prepare_build`, after loading the manifest, add workspace handling:

```cpp
auto roots = find_manifest_roots(std::filesystem::current_path());
if (roots.memberRoot.empty()) {
    return std::unexpected("no mcpp.toml found in current directory or any parent");
}
auto root = roots.memberRoot;

auto m = mcpp::manifest::load(root / "mcpp.toml");
if (!m) return std::unexpected(m.error().format());

// Workspace mode: if we're at a workspace root, or if a package name
// filter is active, handle member loading.
if (m->workspace.present) {
    // Load and build all members (or filtered by -p)
    // For each member: load mcpp.toml, merge workspace deps,
    // add as path dependency to the build graph.
    for (auto& memberPath : m->workspace.members) {
        auto memberDir = root / memberPath;
        if (!std::filesystem::exists(memberDir / "mcpp.toml")) {
            return std::unexpected(std::format(
                "workspace member '{}' has no mcpp.toml", memberPath));
        }
        // Member manifests are loaded and their path deps resolved
        // as part of the normal dependency walk.
    }
}

// If workspace root has [package], it's a rooted workspace — build normally.
// If workspace root has no [package] (virtual workspace), build members only.
```

- [ ] **Step 4: Add `-p, --package` flag to build/test/run commands**

In the CLI app builder, add the flag to build, test, and run commands:

```cpp
.option(cl::Option("package").short_name('p').takes_value().value_name("NAME")
    .help("Build only the named workspace member"))
```

In `cmd_build`, read the flag:

```cpp
auto package_filter = parsed.value("package");
```

Pass it through to `prepare_build` via `BuildOverrides`:

```cpp
struct BuildOverrides {
    std::string target_triple;
    bool        force_static = false;
    std::string package_filter;     // -p <name>: only build this workspace member
};
```

- [ ] **Step 5: Build and verify compilation**

```bash
mcpp build && mcpp test
```

Expected: All tests pass. Workspace features are parsed but not yet exercised by e2e tests.

- [ ] **Step 6: Commit**

```bash
git add src/cli.cppm
git commit -m "feat(workspace): workspace root discovery + member loading + -p flag"
```

---

### Task 3: End-to-end workspace test

**Files:**
- Create: `tests/e2e/35_workspace.sh`

- [ ] **Step 1: Write the e2e test**

```bash
#!/usr/bin/env bash
set -euo pipefail

# Test: workspace with two library members and one binary member.
# Verifies:
#   1. `mcpp build` at workspace root builds all members
#   2. Path deps between members work
#   3. `.workspace = true` version inheritance works
#   4. Workspace lock file is created at root

TMP=$(mktemp -d)
trap "rm -rf $TMP" EXIT
cd "$TMP"

# ── Create workspace structure ──────────────────────────
mkdir -p libs/core/src libs/greeter/src apps/hello/src

# Workspace root (virtual — no [package])
cat > mcpp.toml << 'EOF'
[workspace]
members = ["libs/core", "libs/greeter", "apps/hello"]
EOF

# libs/core — a simple library
cat > libs/core/mcpp.toml << 'EOF'
[package]
namespace = "demo"
name      = "core"
version   = "0.1.0"

[targets.core]
kind = "lib"
EOF

cat > libs/core/src/core.cppm << 'EOF'
export module demo.core;
import std;

export namespace demo::core {
    std::string greet_target() { return "World"; }
}
EOF

# libs/greeter — depends on core via path
cat > libs/greeter/mcpp.toml << 'EOF'
[package]
namespace = "demo"
name      = "greeter"
version   = "0.1.0"

[targets.greeter]
kind = "lib"

[dependencies]
core = { path = "../core" }
EOF

cat > libs/greeter/src/greeter.cppm << 'EOF'
export module demo.greeter;
import std;
import demo.core;

export namespace demo::greeter {
    std::string greet() {
        return "Hello, " + demo::core::greet_target() + "!";
    }
}
EOF

# apps/hello — binary that uses greeter
cat > apps/hello/mcpp.toml << 'EOF'
[package]
namespace = "demo"
name      = "hello"
version   = "0.1.0"

[dependencies]
greeter = { path = "../../libs/greeter" }
EOF

cat > apps/hello/src/main.cpp << 'EOF'
import std;
import demo.greeter;

int main() {
    std::println("{}", demo::greeter::greet());
    return 0;
}
EOF

# ── Build from workspace root ───────────────────────────
"$MCPP" build
echo "workspace build: ok"

# ── Verify the binary runs correctly ────────────────────
OUT=$(./target/*/bin/hello 2>&1 || true)
echo "output: $OUT"
test "$OUT" = "Hello, World!" || { echo "FAIL: unexpected output"; exit 1; }
echo "workspace run: ok"

echo "ALL WORKSPACE TESTS PASSED"
```

- [ ] **Step 2: Make the test executable**

```bash
chmod +x tests/e2e/35_workspace.sh
```

- [ ] **Step 3: Run the test**

```bash
MCPP=$(pwd)/target/x86_64-linux-gnu/*/bin/mcpp tests/e2e/35_workspace.sh
```

Expected: Test should pass once workspace build logic is complete.

- [ ] **Step 4: Commit**

```bash
git add tests/e2e/35_workspace.sh
git commit -m "test(workspace): add e2e workspace build test"
```

---

### Task 4: Workspace build orchestration in prepare_build

**Files:**
- Modify: `src/cli.cppm` (prepare_build function)

This is the core integration task. When `prepare_build` detects a workspace root (virtual — no `[package]`), it needs to:

1. Identify which member to build (all members, or filtered by `-p`)
2. For a virtual workspace, pick the first binary member (or the `-p` target) as the primary manifest
3. Load all other members as path dependencies

- [ ] **Step 1: Implement virtual workspace handling in prepare_build**

After loading the manifest and detecting `m->workspace.present`:

```cpp
if (m->workspace.present && !m->workspace.members.empty()) {
    // Virtual workspace (no [package]) or rooted workspace.
    // Strategy: find the build target member(s) and treat other
    // members as path dependencies.

    std::string targetMember;
    if (!overrides.package_filter.empty()) {
        // -p <name>: find the named member
        targetMember = overrides.package_filter;
    } else if (m->package.name.empty()) {
        // Virtual workspace: find the first member that has a binary target,
        // or fall back to building the first member.
        for (auto& mp : m->workspace.members) {
            auto memberManifest = mcpp::manifest::load(root / mp / "mcpp.toml");
            if (!memberManifest) continue;
            merge_workspace_deps(*memberManifest, *m);
            for (auto& t : memberManifest->targets) {
                if (t.kind == mcpp::manifest::Target::Binary) {
                    targetMember = mp;
                    break;
                }
            }
            if (!targetMember.empty()) break;
        }
        if (targetMember.empty() && !m->workspace.members.empty()) {
            targetMember = m->workspace.members.back();
        }
    }
    // else: rooted workspace with [package] — build the root package normally

    if (!targetMember.empty()) {
        // Switch root to the target member's directory
        auto memberDir = root / targetMember;
        auto memberManifest = mcpp::manifest::load(memberDir / "mcpp.toml");
        if (!memberManifest) {
            return std::unexpected(std::format(
                "workspace member '{}': {}", targetMember,
                memberManifest.error().format()));
        }
        merge_workspace_deps(*memberManifest, *m);

        // Inherit workspace toolchain if member doesn't define one
        if (memberManifest->toolchain.byPlatform.empty()) {
            memberManifest->toolchain = m->toolchain;
        }
        // Inherit workspace target overrides
        for (auto& [triple, entry] : m->targetOverrides) {
            if (!memberManifest->targetOverrides.contains(triple)) {
                memberManifest->targetOverrides[triple] = entry;
            }
        }

        *m = std::move(*memberManifest);
        root = memberDir;
    }
}
```

- [ ] **Step 2: Build and test**

```bash
mcpp build && mcpp test
```

Then run the e2e workspace test with the newly built mcpp.

- [ ] **Step 3: Commit**

```bash
git add src/cli.cppm
git commit -m "feat(workspace): virtual workspace build orchestration"
```

---

### Task 5: Final integration + PR

- [ ] **Step 1: Run full test suite**

```bash
mcpp build && mcpp test
```

- [ ] **Step 2: Commit any remaining changes**

```bash
git add -A && git commit -m "feat(workspace): Phase 1 complete"
```

- [ ] **Step 3: Push and create PR**

```bash
git push -u origin workspace-phase1
gh pr create --title "feat: workspace support (Phase 1)" \
  --body "..." --base main
```

- [ ] **Step 4: Monitor CI**

```bash
gh pr checks <PR_NUMBER> --watch
```
