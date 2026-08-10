# Configure-Only Compile Database Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 实现 `mcpp build --configure-only`，在不编译普通翻译单元、不链接目标且不写构建成功缓存的前提下，基于真实 `BuildPlan` 原子发布包含源码与测试 TU 的 `compile_commands.json`。

**Architecture:** CLI 继续使用现有 build selectors 和 workspace fan-out；共享的测试发现模块为 `mcpp test` 与 configure-only 合成相同测试 target；`prepare_build()` 仍是解析工具链、依赖、feature、模块图与编译参数的唯一入口。独立的 `run_configure_plan()` 只物化 clangd 立即需要的已存在 BMI，然后以 Ninja backend dry-run 生成 `build.ninja` 和 CDB；CDB 发布失败在普通 build/test 中警告继续，在 configure-only 中返回失败。

**Tech Stack:** C++23 named modules、`std::expected`、nlohmann JSON (`mcpp.libs.json`)、Ninja backend、GoogleTest、跨平台 Bash E2E、GitHub Actions Linux/macOS/Windows。

---

## Scope Guard

本计划只实现 `.agents/docs/2026-08-08-configure-only-cdb-design.md` 已确认的 A 部分：

- `mcpp build --configure-only`；
- 普通源码和 `tests/**/*.cpp` 的真实 CDB；
- test TU 的 dev-dependencies 与 `[build].flags`；
- std BMI 和已命中缓存的依赖 BMI 物化；
- 单文件原子 CDB 发布；
- 普通 build/test 警告继续、configure-only 严格失败；
- 人类输出和退出码。

不得加入：JSON/NDJSON、`ide` 子命令、snapshot/ID/envelope、`current.json`、`FileLock`、`invalidatedBy`、metadata/dependency graph、`.xlings.json` pin、新 toolchain/profile/feature/capability/workspace 语义。

## Execution Setup

所有命令从 worktree 根目录执行：

```bash
cd /Users/cltx/projects/mcpp/mcpp/target/worktrees/configure-only-cdb
git status --short --branch
mcpp build --no-color
FRESH_MCPP="$(find "$PWD/target" -type f \( -path '*/bin/mcpp' -o -path '*/bin/mcpp.exe' \) -print0 | xargs -0 ls -1t | head -1)"
"$FRESH_MCPP" --version
```

预期：分支为 `codex/configure-only-cdb`；只包含本计划已知改动；fresh binary 输出当前 `mcpp.toml` 中的版本。

## File Map

| File | Responsibility |
|---|---|
| `src/build/test_targets.cppm` | 共享 `tests/**/*.cpp` 发现、member scoping、稳定命名和 `[build].flags` 合成；不打印 UI，不解析 dev-dependencies。 |
| `src/build/configure.cppm` | 物化 std/cached dependency BMI，并运行只配置的 Ninja dry-run；不触碰 BMI populate 或 `.build_cache`。 |
| `src/build/execute.cppm` | `mcpp test` 改用共享发现 API；保留 list、build、run 和 summary 行为。 |
| `src/platform/fs.cppm` | 提供不先删除 destination 的跨平台 `replace_file()`。 |
| `src/build/compile_commands.cppm` | 校验、合并、稳定排序、同目录临时文件写入和原子发布 CDB。 |
| `src/build/backend.cppm` | 增加默认关闭的 `requireCompileDatabase`，并在结果中携带最终 CDB 条目数。 |
| `src/build/ninja_backend.cppm` | 将 CDB writer 结果落实为普通构建 warning 或 configure-only fatal；dry-run 在 spawn Ninja 前返回。 |
| `src/cli/cmd_build.cppm` | 解析 configure-only 模式，复用 workspace fan-out 与 selectors，并绕过 build fast path。 |
| `src/cli.cppm` | 注册并展示 `--configure-only`。 |
| `tests/unit/test_test_targets.cpp` | 共享测试发现契约。 |
| `tests/unit/test_platform_fs.cpp` | replace-existing 与失败保留 destination。 |
| `tests/unit/test_compile_commands.cpp` | 原子 writer、JSON 校验、mtime、失败保留和稳定排序。 |
| `tests/unit/test_configure.cpp` | 只物化 BMI、不物化 cached object 的配置前置步骤。 |
| `tests/unit/test_test_options.cpp` | `requireCompileDatabase` 默认值回归。 |
| `tests/e2e/202_configure_only_cdb.sh` | CLI、坏源码、测试/dev-dep flags、无对象/链接产物、workspace、失败策略和 build-cache 契约。 |
| `tests/e2e/01_help_and_version.sh` | 顶层 help 暴露 configure-only。 |
| `README.md`、`docs/00-getting-started.md`、`docs/zh/00-getting-started.md` | 用户入口、行为边界和 trust/副作用说明。 |

### Task 1: Extract Shared Test Target Discovery

**Files:**
- Create: `src/build/test_targets.cppm`
- Create: `tests/unit/test_test_targets.cpp`
- Modify: `src/build/execute.cppm:1026-1157`

- [ ] **Step 1: Write failing discovery tests**

新增 `tests/unit/test_test_targets.cpp`，先导入尚不存在的模块并覆盖嵌套命名、glob flags、member scoping 和损坏 manifest 的 best-effort 行为：

```cpp
#include <gtest/gtest.h>

import std;
import mcpp.build.test_targets;

namespace fs = std::filesystem;
using mcpp::build::discover_test_targets;

namespace {

struct TempProject {
    fs::path path = fs::temp_directory_path() /
        std::format("mcpp-test-targets-{}",
                    std::chrono::steady_clock::now().time_since_epoch().count());
    TempProject() { fs::create_directories(path); }
    ~TempProject() { std::error_code ec; fs::remove_all(path, ec); }
};

void write(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path);
    out << text;
}

} // namespace

TEST(TestTargets, DiscoversNestedNamesAndMatchingBuildFlags) {
    TempProject p;
    write(p.path / "mcpp.toml", R"(
[package]
name = "app"
version = "0.1.0"

[build]
flags = [{ glob = "tests/tagged/**/*.cpp", defines = ["TAGGED=1"], cxxflags = ["-Wextra"] }]
)");
    write(p.path / "tests/tagged/nested/smoke.cpp", "int main() { return 0; }");
    write(p.path / "tests/plain.cpp", "int main() { return 0; }");

    auto found = discover_test_targets(p.path, "");
    ASSERT_TRUE(found.has_value()) << found.error();
    ASSERT_EQ(found->targets.size(), 2u);
    auto tagged = std::ranges::find(found->targets, "tagged/nested/smoke",
                                    &mcpp::manifest::Target::name);
    ASSERT_NE(tagged, found->targets.end());
    EXPECT_EQ(tagged->defines, std::vector<std::string>({"TAGGED=1"}));
    EXPECT_EQ(tagged->cxxflags, std::vector<std::string>({"-Wextra"}));
}

TEST(TestTargets, ScopesDiscoveryToSelectedWorkspaceMember) {
    TempProject p;
    write(p.path / "mcpp.toml", "[workspace]\nmembers = [\"a\", \"b\"]\n");
    write(p.path / "a/mcpp.toml", "[package]\nname=\"a\"\nversion=\"0.1.0\"\n");
    write(p.path / "b/mcpp.toml", "[package]\nname=\"b\"\nversion=\"0.1.0\"\n");
    write(p.path / "a/tests/main.cpp", "int main() { return 0; }");
    write(p.path / "b/tests/main.cpp", "int main() { return 0; }");

    auto found = discover_test_targets(p.path, "a");
    ASSERT_TRUE(found.has_value()) << found.error();
    ASSERT_EQ(found->targets.size(), 1u);
    EXPECT_EQ(found->packageRoot, p.path / "a");
    EXPECT_EQ(found->targets.front().main, "tests/main.cpp");
}

TEST(TestTargets, InvalidManifestStillProvidesBestEffortInventory) {
    TempProject p;
    write(p.path / "mcpp.toml", "[package\nthis is invalid\n");
    write(p.path / "tests/broken.cpp", "this does not parse");

    auto found = discover_test_targets(p.path, "");
    ASSERT_TRUE(found.has_value()) << found.error();
    ASSERT_EQ(found->targets.size(), 1u);
    EXPECT_EQ(found->targets.front().name, "broken");
}
```

- [ ] **Step 2: Run the focused tests and verify RED**

```bash
mcpp test --no-color -- --gtest_filter='TestTargets.*'
```

预期：编译失败，明确提示找不到 `mcpp.build.test_targets`；不能因为没有匹配测试而显示成功。

- [ ] **Step 3: Implement the shared discovery module**

新增 `src/build/test_targets.cppm`，公开一个无 UI 副作用的结果类型和函数：

```cpp
export module mcpp.build.test_targets;

import std;
import mcpp.manifest;
import mcpp.modgraph.scanner;
import mcpp.project;

export namespace mcpp::build {

struct TestTargetSet {
    std::filesystem::path packageRoot;
    std::vector<mcpp::manifest::Target> targets;
};

std::expected<TestTargetSet, std::string>
discover_test_targets(const std::filesystem::path& manifestRoot,
                      std::string_view packageFilter);

} // namespace mcpp::build
```

实现严格复用当前 `run_tests()` 的算法：有效 workspace manifest 下调用 `resolve_member_dir()`；manifest 无法解析时保持 manifestRoot 以支持 `test --list` best-effort；展开 `tests/**/*.cpp`；用 tests-relative 去扩展名路径作为 name；重复 name 返回错误；匹配 package manifest 的 base `[build].flags`，把 defines/cflags/cxxflags 放入合成 `Target::TestBinary`。

核心循环必须保持如下字段映射：

```cpp
mcpp::manifest::Target target;
target.name = relative.replace_extension("").generic_string();
target.kind = mcpp::manifest::Target::TestBinary;
target.main = std::filesystem::relative(file, packageRoot).string();
```

只使用必要中文注释解释两个非直观约束：损坏 manifest 下的 best-effort inventory，以及 member root 必须与 `prepare_build()` 的 package filter 一致。

- [ ] **Step 4: Rewire `run_tests()` without changing list semantics**

在 `src/build/execute.cppm` 导入 `mcpp.build.test_targets`，把 1056-1126 的发现/合成代码替换为：

```cpp
auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
if (!root) {
    mcpp::ui::error("no mcpp.toml found in current directory or any parent");
    return 2;
}

auto discovered = discover_test_targets(*root, overrides.package_filter);
if (!discovered) {
    mcpp::ui::error(discovered.error());
    return 2;
}
auto testRoot = discovered->packageRoot;
auto testTargets = std::move(discovered->targets);
if (testTargets.empty()) {
    std::println("no tests found in tests/");
    return 0;
}
```

`--list` 后面的过滤、JSON record、summary、`prepare_build(includeDevDeps=true)` 和运行逻辑保持原样。

- [ ] **Step 5: Run unit and existing E2E tests and verify GREEN**

```bash
mcpp build --no-color
FRESH_MCPP="$(find "$PWD/target" -type f \( -path '*/bin/mcpp' -o -path '*/bin/mcpp.exe' \) -print0 | xargs -0 ls -1t | head -1)"
"$FRESH_MCPP" test --no-color -- --gtest_filter='TestTargets.*'
MCPP="$FRESH_MCPP" bash tests/e2e/159_test_list.sh
MCPP="$FRESH_MCPP" bash tests/e2e/157_test_glob_flags.sh
MCPP="$FRESH_MCPP" bash tests/e2e/90_workspace_test.sh
```

预期：unit 通过；三个 E2E 均输出 `OK`；损坏源码仍可 list；workspace 中同名测试不冲突。

- [ ] **Step 6: Commit the extraction**

```bash
git add src/build/test_targets.cppm src/build/execute.cppm tests/unit/test_test_targets.cpp
git commit -m "refactor(test): share test target discovery"
```

### Task 2: Add Cross-Platform Atomic Replacement Primitive

**Files:**
- Modify: `src/platform/fs.cppm`
- Create: `tests/unit/test_platform_fs.cpp`

- [ ] **Step 1: Write failing filesystem tests**

新增 `tests/unit/test_platform_fs.cpp`：

```cpp
#include <gtest/gtest.h>

import std;
import mcpp.platform.fs;

namespace fs = std::filesystem;

namespace {
struct TempDir {
    fs::path path = fs::temp_directory_path() /
        std::format("mcpp-platform-fs-{}",
                    std::chrono::steady_clock::now().time_since_epoch().count());
    TempDir() { fs::create_directories(path); }
    ~TempDir() { std::error_code ec; fs::remove_all(path, ec); }
};
void write(const fs::path& p, std::string_view s) { std::ofstream(p) << s; }
std::string read(const fs::path& p) { std::ifstream in(p); return {std::istreambuf_iterator<char>(in), {}}; }
} // namespace

TEST(PlatformFs, ReplaceFileAtomicallyReplacesExistingFile) {
    TempDir t;
    auto source = t.path / "new.tmp";
    auto destination = t.path / "compile_commands.json";
    write(source, "new");
    write(destination, "old");
    std::error_code ec;
    EXPECT_TRUE(mcpp::platform::fs::replace_file(source, destination, ec)) << ec.message();
    EXPECT_FALSE(fs::exists(source));
    EXPECT_EQ(read(destination), "new");
}

TEST(PlatformFs, ReplaceFailureKeepsExistingDestination) {
    TempDir t;
    auto destination = t.path / "compile_commands.json";
    write(destination, "last-known-good");
    std::error_code ec;
    EXPECT_FALSE(mcpp::platform::fs::replace_file(t.path / "missing.tmp", destination, ec));
    EXPECT_TRUE(ec);
    EXPECT_EQ(read(destination), "last-known-good");
}
```

- [ ] **Step 2: Run focused tests and verify RED**

```bash
mcpp test --no-color -- --gtest_filter='PlatformFs.*'
```

预期：编译失败，`replace_file` 尚未声明。

- [ ] **Step 3: Implement `replace_file()`**

在 `src/platform/fs.cppm` 的 exported namespace 中增加：

```cpp
bool replace_file(const std::filesystem::path& source,
                  const std::filesystem::path& destination,
                  std::error_code& ec);
```

Windows 实现必须直接调用：

```cpp
if (MoveFileExW(source.wstring().c_str(), destination.wstring().c_str(),
                MOVEFILE_REPLACE_EXISTING)) {
    ec.clear();
    return true;
}
ec = std::error_code(static_cast<int>(GetLastError()), std::system_category());
return false;
```

POSIX 实现使用同文件系统 rename：

```cpp
std::filesystem::rename(source, destination, ec);
return !ec;
```

禁止在任何平台先 `remove(destination)`；添加中文注释说明这是 last-known-good CDB 的原子性边界。

- [ ] **Step 4: Run focused tests and verify GREEN**

```bash
mcpp build --no-color
FRESH_MCPP="$(find "$PWD/target" -type f \( -path '*/bin/mcpp' -o -path '*/bin/mcpp.exe' \) -print0 | xargs -0 ls -1t | head -1)"
"$FRESH_MCPP" test --no-color -- --gtest_filter='PlatformFs.*'
```

预期：两项测试通过；Windows CI 真实执行 `MoveFileExW` 分支，Linux/macOS 执行 rename 分支。

- [ ] **Step 5: Commit the filesystem primitive**

```bash
git add src/platform/fs.cppm tests/unit/test_platform_fs.cpp
git commit -m "feat(fs): add atomic file replacement"
```

### Task 3: Publish Compile Databases Atomically

**Files:**
- Modify: `src/build/compile_commands.cppm`
- Modify: `src/build/backend.cppm`
- Modify: `src/build/ninja_backend.cppm`
- Modify: `tests/unit/test_compile_commands.cpp`
- Modify: `tests/unit/test_test_options.cpp`

- [ ] **Step 1: Add failing writer and option tests**

在 `tests/unit/test_compile_commands.cpp` 增加临时目录 helper，并新增：

```cpp
TEST(CompileCommandsWriter, RejectsNonArrayFreshJson) {
    TempDir t;
    auto result = publish_compile_commands(
        t.path / "compile_commands.json", R"({"file":"not-an-array"})",
        [](const std::filesystem::path&) { return true; });
    ASSERT_FALSE(result.has_value());
    EXPECT_NE(result.error().message.find("JSON array"), std::string::npos);
}

TEST(CompileCommandsWriter, UnchangedContentKeepsMtime) {
    TempDir t;
    auto path = t.path / "compile_commands.json";
    auto content = cdb({entry((t.path / "a.cpp").string(), "-DOK")});
    std::ofstream(path) << content;
    auto before = std::filesystem::last_write_time(path);
    auto result = publish_compile_commands(
        path, content, [](const std::filesystem::path&) { return true; });
    ASSERT_TRUE(result.has_value()) << result.error().message;
    EXPECT_FALSE(result->changed);
    EXPECT_EQ(std::filesystem::last_write_time(path), before);
}

TEST(CompileCommandsWriter, ReplacementFailurePreservesOldDatabase) {
    TempDir t;
    auto path = t.path / "compile_commands.json";
    auto oldContent = cdb({entry((t.path / "old.cpp").string(), "-DOLD")});
    auto newContent = cdb({entry((t.path / "new.cpp").string(), "-DNEW")});
    std::ofstream(path) << oldContent;
    auto failReplace = [](const std::filesystem::path&,
                          const std::filesystem::path&,
                          std::error_code& ec) {
        ec = std::make_error_code(std::errc::permission_denied);
        return false;
    };
    auto result = publish_compile_commands(
        path, newContent, [](const std::filesystem::path&) { return true; }, failReplace);
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(read_file(path), oldContent);
    EXPECT_EQ(std::distance(std::filesystem::directory_iterator(t.path),
                            std::filesystem::directory_iterator{}), 1);
}

TEST(CompileCommandsMerge, SortsFinalEntriesByFile) {
    auto fresh = cdb({entry("/p/z.cpp", "-DZ"), entry("/p/a.cpp", "-DA")});
    auto merged = merge_compile_commands(
        fresh, "[]", [](const std::filesystem::path&) { return true; });
    EXPECT_LT(merged.find("/p/a.cpp"), merged.find("/p/z.cpp"));
}
```

在 `tests/unit/test_test_options.cpp` 增加：

```cpp
TEST(BuildOptions, CompileDatabaseIsOptionalByDefault) {
    BuildOptions options;
    EXPECT_FALSE(options.requireCompileDatabase);
}
```

- [ ] **Step 2: Run focused tests and verify RED**

```bash
mcpp test --no-color -- --gtest_filter='CompileCommandsWriter.*:CompileCommandsMerge.SortsFinalEntriesByFile:BuildOptions.CompileDatabaseIsOptionalByDefault'
```

预期：编译失败，因为 writer 结果类型、`publish_compile_commands()` 和 `requireCompileDatabase` 尚不存在。

- [ ] **Step 3: Add structured writer result and atomic publication**

在 `src/build/compile_commands.cppm` 导入 `mcpp.platform.fs`，公开：

```cpp
struct CompileCommandsWriteResult {
    bool changed = false;
    std::size_t commandCount = 0;
};

struct CompileCommandsWriteError {
    std::string message;
};

using ReplaceFile = std::function<bool(const std::filesystem::path&,
                                       const std::filesystem::path&,
                                       std::error_code&)>;

std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
publish_compile_commands(
    const std::filesystem::path& path,
    std::string_view fresh,
    const std::function<bool(const std::filesystem::path&)>& fileExists,
    ReplaceFile replaceFile = mcpp::platform::fs::replace_file);

std::expected<CompileCommandsWriteResult, CompileCommandsWriteError>
write_compile_commands(const BuildPlan& plan, const CompileFlags& flags);
```

`publish_compile_commands()` 按以下顺序实现：

1. parse fresh，拒绝 discarded 或非数组；
2. 若旧文件可读则调用 `merge_compile_commands()`；
3. parse 最终文本并再次要求顶层数组；
4. 按 `file` 稳定排序；
5. 与旧文本相同则返回 `{false, size}`；
6. 在同目录写唯一 sibling temp，`flush()`、`close()` 并检查 stream 状态；
7. 调用 `replaceFile(temp, path, ec)`；
8. 失败时删除 temp 并返回带 path 和 OS error 的错误；不得删除 path；
9. 成功返回 `{true, size}`。

临时文件名使用时间戳加进程内原子序号，避免同一进程并发碰撞：

```cpp
static std::atomic<std::uint64_t> sequence{0};
auto temp = path.parent_path() /
    std::format(".{}.tmp.{}.{}", path.filename().string(),
                std::chrono::steady_clock::now().time_since_epoch().count(),
                sequence.fetch_add(1, std::memory_order_relaxed));
```

- [ ] **Step 4: Propagate required/optional CDB policy through the backend**

在 `src/build/backend.cppm` 增加：

```cpp
struct BuildOptions {
    bool requireCompileDatabase = false;
    // existing fields unchanged
};

struct BuildResult {
    std::size_t compileCommands = 0;
    // existing fields unchanged
};
```

在 `src/build/ninja_backend.cppm` 替换裸调用：

```cpp
auto cdb = write_compile_commands(plan, flags);
if (!cdb) {
    if (opts.requireCompileDatabase) {
        return std::unexpected(BuildError{
            std::format("cannot publish compile_commands.json: {}", cdb.error().message),
            plan.compileDbPath.empty() ? plan.projectRoot / "compile_commands.json"
                                       : plan.compileDbPath});
    }
    mcpp::ui::warning(std::format(
        "compile_commands.json was not updated: {}", cdb.error().message));
}
```

创建 dry-run 和真实 build 的 `BuildResult` 时都设置：

```cpp
r.compileCommands = cdb ? cdb->commandCount : 0;
```

普通调用点不设置 `requireCompileDatabase`，所以现有 build/test 在 CDB 失败时继续 Ninja；只有后续 configure runner 将其设为 true。

- [ ] **Step 5: Run focused and regression tests and verify GREEN**

```bash
mcpp build --no-color
FRESH_MCPP="$(find "$PWD/target" -type f \( -path '*/bin/mcpp' -o -path '*/bin/mcpp.exe' \) -print0 | xargs -0 ls -1t | head -1)"
"$FRESH_MCPP" test --no-color -- --gtest_filter='CompileCommands*:BuildOptions.CompileDatabaseIsOptionalByDefault'
MCPP="$FRESH_MCPP" bash tests/e2e/76_compile_commands_generated.sh
MCPP="$FRESH_MCPP" bash tests/e2e/77_cdb_preserves_test_entries.sh
```

预期：writer tests 通过；现有 build 仍生成合法 CDB；test entries 的 merge/prune 行为不变。

- [ ] **Step 6: Commit atomic CDB publication**

```bash
git add src/build/compile_commands.cppm src/build/backend.cppm src/build/ninja_backend.cppm tests/unit/test_compile_commands.cpp tests/unit/test_test_options.cpp
git commit -m "fix(build): publish compile database atomically"
```

### Task 4: Add Configure Prerequisite Staging and Runner

**Files:**
- Create: `src/build/configure.cppm`
- Create: `tests/unit/test_configure.cpp`

- [ ] **Step 1: Write failing BMI staging tests**

新增 `tests/unit/test_configure.cpp`。fixture 构造最小 `BuildPlan`，准备 std BMI、cached dependency BMI 和 cached object：

```cpp
#include <gtest/gtest.h>

import std;
import mcpp.build.configure;
import mcpp.toolchain.model;

namespace fs = std::filesystem;

TEST(ConfigurePrerequisites, StagesOnlyBmisNeededByLanguageTools) {
    TempDir t;
    mcpp::build::BuildPlan plan;
    plan.outputDir = t.path / "out";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    plan.stdBmiPath = t.path / "cache/std.pcm";
    plan.stdCompatBmiPath = t.path / "cache/std.compat.pcm";
    write_file(plan.stdBmiPath, "std-bmi");
    write_file(plan.stdCompatBmiPath, "std-compat-bmi");

    mcpp::build::CompileUnit dep;
    dep.servedFromCache = true;
    dep.providesModule = "demo.dep";
    dep.cachedBmi = t.path / "cache/demo.dep.pcm";
    dep.cachedObject = t.path / "cache/demo.dep.o";
    dep.object = "obj/demo.dep.o";
    write_file(dep.cachedBmi, "dep-bmi");
    write_file(dep.cachedObject, "dep-object");
    plan.compileUnits.push_back(dep);

    auto staged = mcpp::build::stage_configure_prerequisites(plan);
    ASSERT_TRUE(staged.has_value()) << staged.error();
    EXPECT_TRUE(fs::exists(plan.outputDir / "pcm.cache/std.pcm"));
    EXPECT_TRUE(fs::exists(plan.outputDir / "pcm.cache/std.compat.pcm"));
    EXPECT_TRUE(fs::exists(plan.outputDir / "pcm.cache/demo.dep.pcm"));
    EXPECT_FALSE(fs::exists(plan.outputDir / dep.object));
}

TEST(ConfigurePrerequisites, MissingCachedBmiFailsBeforePublication) {
    TempDir t;
    mcpp::build::BuildPlan plan;
    plan.outputDir = t.path / "out";
    plan.toolchain.compiler = mcpp::toolchain::CompilerId::Clang;
    mcpp::build::CompileUnit dep;
    dep.servedFromCache = true;
    dep.providesModule = "demo.dep";
    dep.cachedBmi = t.path / "missing/demo.dep.pcm";
    plan.compileUnits.push_back(dep);

    auto staged = mcpp::build::stage_configure_prerequisites(plan);
    ASSERT_FALSE(staged.has_value());
    EXPECT_NE(staged.error().find("demo.dep"), std::string::npos);
}
```

`TempDir`、`write_file()` 与 Task 2 fixture 同形，但在本文件本地定义，避免测试间隐藏依赖。

- [ ] **Step 2: Run focused tests and verify RED**

```bash
mcpp test --no-color -- --gtest_filter='ConfigurePrerequisites.*'
```

预期：编译失败，模块与函数尚不存在。

- [ ] **Step 3: Implement `stage_configure_prerequisites()`**

新增 `src/build/configure.cppm`，在 module fragment 中包含 `<cstdio>`，导入 `mcpp.build.prepare`、`mcpp.build.stage`、`mcpp.build.ninja`、`mcpp.diag`、`mcpp.toolchain.model`、`mcpp.toolchain.registry` 和 `mcpp.ui`。公开：

```cpp
std::expected<std::size_t, std::string>
stage_configure_prerequisites(const BuildPlan& plan);

int run_configure_plan(BuildContext& ctx, bool verbose);
```

staging 使用 `stage::StageOptions{.verify = stage::Verify::Size}`，并严格限定为：

```cpp
// std 与 std.compat 只物化 BMI；clangd 不需要对应 object。
if (!plan.stdBmiPath.empty()) {
    stage_one(plan.stdBmiPath,
              mcpp::toolchain::staged_std_bmi_path(plan.toolchain, plan.outputDir),
              "std");
}
if (!plan.stdCompatBmiPath.empty()) {
    stage_one(plan.stdCompatBmiPath,
              mcpp::toolchain::staged_std_compat_bmi_path(plan.toolchain, plan.outputDir),
              "std.compat");
}

auto traits = mcpp::toolchain::bmi_traits(plan.toolchain);
for (const auto& unit : plan.compileUnits) {
    if (!unit.servedFromCache || !unit.providesModule || unit.cachedBmi.empty()) continue;
    std::string fileName;
    for (char ch : *unit.providesModule) fileName.push_back(ch == ':' ? '-' : ch);
    fileName += traits.bmiExt;
    stage_one(unit.cachedBmi, plan.outputDir / traits.bmiDir / fileName,
              *unit.providesModule);
}
```

任何 staging 失败都返回带 module 名和 `stage_file` 原始诊断的 error；此函数不得复制 `cachedObject`、`stdObjectPath` 或 `stdCompatObjectPath`。

- [ ] **Step 4: Implement independent `run_configure_plan()`**

执行顺序必须是：

```cpp
int run_configure_plan(BuildContext& ctx, bool verbose) {
    auto staged = stage_configure_prerequisites(ctx.plan);
    if (!staged) {
        mcpp::ui::error(staged.error());
        return 1;
    }

    auto backend = mcpp::build::make_ninja_backend();
    BuildOptions options;
    options.verbose = verbose;
    options.dryRun = true;
    options.requireCompileDatabase = true;
    auto result = backend->build(ctx.plan, options);
    if (!result) {
        mcpp::ui::error(result.error().message);
        if (!result.error().diagnosticOutput.empty())
            std::fputs(result.error().diagnosticOutput.c_str(), stderr);
        return 1;
    }
    if (!mcpp::diag::flush(ctx.strict)) return 1;

    mcpp::ui::status("Configured", std::format(
        "{} ({} compile command{})", ctx.manifest.package.name,
        result->compileCommands, result->compileCommands == 1 ? "" : "s"));
    return 0;
}
```

此函数中不得出现 `bmi_cache::populate_from()`、`write_build_cache()`、`ui::finished()`、`producedArtifacts` 或对 `target/.build_cache` 的写入。不要调用 `run_build_plan()`。

- [ ] **Step 5: Run focused tests and verify GREEN**

```bash
mcpp build --no-color
FRESH_MCPP="$(find "$PWD/target" -type f \( -path '*/bin/mcpp' -o -path '*/bin/mcpp.exe' \) -print0 | xargs -0 ls -1t | head -1)"
"$FRESH_MCPP" test --no-color -- --gtest_filter='ConfigurePrerequisites.*'
```

预期：只出现 BMI destination；cached object 和 std object 不存在；缺失 cached BMI 明确失败。

- [ ] **Step 6: Commit the configure execution layer**

```bash
git add src/build/configure.cppm tests/unit/test_configure.cpp
git commit -m "feat(build): add configure-only execution"
```

### Task 5: Add `build --configure-only` CLI Routing

**Files:**
- Modify: `src/cli.cppm:49-90,229-255`
- Modify: `src/cli/cmd_build.cppm:11-104`
- Create: `tests/e2e/202_configure_only_cdb.sh`
- Modify: `tests/e2e/01_help_and_version.sh`

- [ ] **Step 1: Write the failing CLI E2E shell**

新增 `tests/e2e/202_configure_only_cdb.sh`，line 2 保持空 capability：

```bash
#!/usr/bin/env bash
# requires:
set -euo pipefail

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/app/src" "$TMP/app/tests"
cat > "$TMP/app/mcpp.toml" <<'EOF'
[package]
name = "app"
version = "0.1.0"

[build]
flags = [{ glob = "tests/**/*.cpp", defines = ["TEST_CDB_FLAG=1"], cxxflags = ["-DTEST_CXX_FLAG=1"] }]
EOF
cat > "$TMP/app/src/main.cpp" <<'EOF'
int main( { this source is intentionally invalid
EOF
cat > "$TMP/app/tests/smoke.cpp" <<'EOF'
int main() { return 0; }
EOF

cd "$TMP/app"
out=$("$MCPP" build --configure-only 2>&1) || {
    echo "configure-only rejected syntax-error source: $out"; exit 1;
}
[[ "$out" == *"Configured app"* ]] || { echo "missing configured status: $out"; exit 1; }
[[ -s compile_commands.json ]] || { echo "compile_commands.json missing"; exit 1; }
grep -q 'src/main\.cpp' compile_commands.json || { cat compile_commands.json; exit 1; }
grep -q 'tests/smoke\.cpp' compile_commands.json || { cat compile_commands.json; exit 1; }
grep -q 'TEST_CDB_FLAG=1' compile_commands.json || { cat compile_commands.json; exit 1; }
grep -q 'TEST_CXX_FLAG=1' compile_commands.json || { cat compile_commands.json; exit 1; }

# 只允许配置元数据和 BMI；不得出现普通目标 object、binary 或成功缓存。
if find target -type f \( -name '*.o' -o -name '*.obj' -o -path '*/bin/*' \) | grep -q .; then
    echo "configure-only produced compile/link artifacts"; find target -type f; exit 1
fi
[[ ! -e target/.build_cache ]] || { echo "configure-only wrote target/.build_cache"; exit 1; }

echo OK
```

在 `tests/e2e/01_help_and_version.sh` 增加：

```bash
[[ "$out" == *"--configure-only"* ]] || { echo "--help missing --configure-only"; exit 1; }
```

- [ ] **Step 2: Run E2E and verify RED**

```bash
MCPP="$FRESH_MCPP" bash tests/e2e/202_configure_only_cdb.sh
MCPP="$FRESH_MCPP" bash tests/e2e/01_help_and_version.sh
```

预期：`202` 以 unknown option 失败，help test 因缺少 flag 失败。

- [ ] **Step 3: Register the CLI flag and help text**

在 `src/cli.cppm` 的 build subcommand 增加：

```cpp
.option(cl::Option("configure-only")
    .help("Resolve the build plan and write compile_commands.json without compiling or linking"))
```

在顶层 `print_usage()` 的 Build options 增加同名行；不要增加 JSON format 选项或 `ide` 子命令。

- [ ] **Step 4: Route single-package and workspace configure requests**

在 `src/cli/cmd_build.cppm` 导入 `mcpp.build.configure` 和 `mcpp.build.test_targets`，读取：

```cpp
const bool configureOnly = parsed.is_flag_set("configure-only");
```

抽出 cmd-local lambda，确保 workspace 与单 package 使用同一流程：

```cpp
auto configure = [&](mcpp::build::BuildOverrides selected) -> int {
    auto root = mcpp::project::find_manifest_root(std::filesystem::current_path());
    if (!root) {
        mcpp::ui::error("no mcpp.toml found in current directory or any parent");
        return 2;
    }
    auto tests = mcpp::build::discover_test_targets(*root, selected.package_filter);
    if (!tests) {
        mcpp::ui::error(tests.error());
        return 2;
    }
    const bool includeDevDeps = !tests->targets.empty();
    auto ctx = mcpp::build::prepare_build(
        print_fp, includeDevDeps, std::move(tests->targets), selected);
    if (!ctx) {
        mcpp::ui::error(ctx.error());
        return 2;
    }
    return mcpp::build::run_configure_plan(*ctx, verbose);
};
```

workspace fan-out 中按现有 continue-on-failure/首个非零规则调用 `configure(mo)`；单 package 在 fast-path 判断之前处理 `configureOnly` 并直接返回。必须保证 configure-only 永远不调用 `try_fast_build()` 或 `run_build_plan()`。

普通 build 路径保持 `includeDevDeps=false` 与现有 fast-path 条件不变。

- [ ] **Step 5: Build fresh binary and verify GREEN**

```bash
mcpp build --no-color
FRESH_MCPP="$(find "$PWD/target" -type f \( -path '*/bin/mcpp' -o -path '*/bin/mcpp.exe' \) -print0 | xargs -0 ls -1t | head -1)"
MCPP="$FRESH_MCPP" bash tests/e2e/202_configure_only_cdb.sh
MCPP="$FRESH_MCPP" bash tests/e2e/01_help_and_version.sh
```

预期：坏源码不触发编译但 CDB 含 source/test/flags；无 `.o/.obj`、无 bin、无 `.build_cache`；help 展示 flag。

- [ ] **Step 6: Commit CLI routing**

```bash
git add src/cli.cppm src/cli/cmd_build.cppm tests/e2e/202_configure_only_cdb.sh tests/e2e/01_help_and_version.sh
git commit -m "feat(cli): add build configure-only mode"
```

### Task 6: Complete Dev-Dependency, Workspace, and Publication-Failure Coverage

**Files:**
- Modify: `tests/e2e/202_configure_only_cdb.sh`

- [ ] **Step 1: Extend E2E with a local dev-dependency include path**

在脚本创建 app 前新增本地 dev package：

```bash
mkdir -p "$TMP/devkit/include" "$TMP/devkit/src"
cat > "$TMP/devkit/mcpp.toml" <<'EOF'
[package]
name = "devkit"
version = "0.1.0"

[build]
include_dirs = ["include"]
EOF
cat > "$TMP/devkit/src/devkit.cppm" <<'EOF'
export module devkit;
EOF
echo '#define DEVKIT_MARKER 1' > "$TMP/devkit/include/devkit.hpp"
```

给 app manifest 增加：

```toml
[dev-dependencies]
devkit = { path = "../devkit" }
```

测试源码改为 `#include <devkit.hpp>`，并用 Python 在 CDB 中精确找到 test entry，断言 arguments 含 devkit include path 和两个 test flags，而 main entry 不含 `TEST_CDB_FLAG`：

```bash
python3 - compile_commands.json "$TMP/devkit/include" <<'PY'
import json, os, sys
entries = json.load(open(sys.argv[1], encoding="utf-8"))
test = next(e for e in entries if e["file"].replace("\\", "/").endswith("/tests/smoke.cpp"))
main = next(e for e in entries if e["file"].replace("\\", "/").endswith("/src/main.cpp"))
test_args = test["arguments"]
assert any(os.path.normpath(sys.argv[2]) in os.path.normpath(a) for a in test_args), test_args
assert any("TEST_CDB_FLAG=1" in a for a in test_args), test_args
assert any("TEST_CXX_FLAG=1" in a for a in test_args), test_args
assert not any("TEST_CDB_FLAG=1" in a for a in main["arguments"]), main["arguments"]
PY
```

- [ ] **Step 2: Extend E2E with workspace fan-out and member selection**

同一脚本创建 virtual workspace `ws/{a,b}`，每个 member 有独立 `src/main.cpp` 和 `tests/main.cpp`。执行：

```bash
cd "$TMP/ws"
"$MCPP" build --configure-only > configure-workspace.log
grep -q 'a/src/main\.cpp' compile_commands.json || { cat compile_commands.json; exit 1; }
grep -q 'b/src/main\.cpp' compile_commands.json || { cat compile_commands.json; exit 1; }
grep -q 'a/tests/main\.cpp' compile_commands.json || { cat compile_commands.json; exit 1; }
grep -q 'b/tests/main\.cpp' compile_commands.json || { cat compile_commands.json; exit 1; }

rm compile_commands.json
"$MCPP" build --configure-only -p a > configure-a.log
grep -q 'a/src/main\.cpp' compile_commands.json || { cat compile_commands.json; exit 1; }
if grep -q 'b/src/main\.cpp' compile_commands.json; then
    echo "-p a leaked member b into CDB"; cat compile_commands.json; exit 1
fi
```

manifest 使用现有 `[workspace] members = ["a", "b"]` 和最小 package metadata，不引入新的 selector 语义。

- [ ] **Step 3: Extend E2E with strict versus warning CDB publication policy**

创建语法正确的 `publish-policy` 工程，把 `compile_commands.json` 预先建为含 sentinel 的非空目录，使 temp 写成功但 replace 到 directory 失败：

```bash
mkdir compile_commands.json
echo keep > compile_commands.json/last-known-good

out=$("$MCPP" build 2>&1) || { echo "normal build failed on optional CDB: $out"; exit 1; }
[[ "$out" == *"compile_commands.json was not updated"* ]] || {
    echo "normal build did not warn: $out"; exit 1;
}
[[ -f compile_commands.json/last-known-good ]] || {
    echo "normal build removed prior destination"; exit 1;
}

rc=0
out=$("$MCPP" build --configure-only 2>&1) || rc=$?
[[ $rc -ne 0 ]] || { echo "configure-only accepted failed CDB publication"; exit 1; }
[[ -f compile_commands.json/last-known-good ]] || {
    echo "configure-only removed prior destination"; exit 1;
}
```

普通 build 还要断言最终 executable 存在，证明 warning 后 Ninja 确实继续；configure-only 不得输出 `Configured`。

- [ ] **Step 4: Run the extended E2E and fix only contract failures**

```bash
MCPP="$FRESH_MCPP" bash tests/e2e/202_configure_only_cdb.sh
```

预期：输出 `OK`。若失败，只修 Task 1-5 已定义的行为；不得为测试新增协议、selector 或 toolchain 分支。

- [ ] **Step 5: Run adjacent regression E2Es**

```bash
MCPP="$FRESH_MCPP" bash tests/e2e/18_devdeps_isolation.sh
MCPP="$FRESH_MCPP" bash tests/e2e/35_workspace.sh
MCPP="$FRESH_MCPP" bash tests/e2e/76_compile_commands_generated.sh
MCPP="$FRESH_MCPP" bash tests/e2e/77_cdb_preserves_test_entries.sh
MCPP="$FRESH_MCPP" bash tests/e2e/157_test_glob_flags.sh
MCPP="$FRESH_MCPP" bash tests/e2e/159_test_list.sh
```

预期：全部 `OK`；普通 build 仍不解析 dev-dependencies；现有 workspace/test/CDB 行为无回归。

- [ ] **Step 6: Commit complete E2E coverage**

```bash
git add tests/e2e/202_configure_only_cdb.sh
git commit -m "test(build): cover configure-only workflows"
```

### Task 7: Document the User Contract

**Files:**
- Modify: `README.md:217-223`
- Modify: `docs/00-getting-started.md:74-113`
- Modify: `docs/zh/00-getting-started.md:72-107`

- [ ] **Step 1: Add concise English and Chinese usage docs**

README feature list改为明确区分 build 与 configure-only：

```markdown
- `compile_commands.json` generated automatically; `mcpp build --configure-only`
  refreshes it for clangd/ccls without compiling ordinary translation units or
  linking final targets
```

英文 Getting Started 在 build/run 示例后加入：

```markdown
For editor setup before the source is buildable, run:

```bash
mcpp build --configure-only
# Configured hello (... compile commands)
```

This resolves the same package, workspace member, profile, features, capability
providers, target and toolchain as a real build, and writes a CDB containing
regular sources plus `tests/**/*.cpp`. It may still run `build.mcpp`, install
missing dependencies/toolchains, and update lock/resolution metadata, so IDEs
must invoke it only in trusted workspaces. It does not compile ordinary TUs,
link final artifacts, or mark the project as successfully built.
```

中文文档加入语义等价段落，使用“只配置”而不是“只读”；明确插件只应依赖退出码与根 `compile_commands.json`，不承诺机器 stdout。

- [ ] **Step 2: Verify documentation matches help and design**

```bash
rg -n "configure-only|只配置|trusted workspaces|可信工作区" README.md docs/00-getting-started.md docs/zh/00-getting-started.md
rg -n "NDJSON|snapshot|envelope|current.json|invalidatedBy" README.md docs/00-getting-started.md docs/zh/00-getting-started.md
```

预期：第一条命令命中三份文档；第二条无输出，避免把 RFC B/C 内容混入用户契约。

- [ ] **Step 3: Verify examples against the fresh binary**

```bash
"$FRESH_MCPP" --help | grep -F -- '--configure-only'
MCPP="$FRESH_MCPP" bash tests/e2e/01_help_and_version.sh
MCPP="$FRESH_MCPP" bash tests/e2e/202_configure_only_cdb.sh
```

预期：help 和文档使用同一 flag；两个 E2E 均 `OK`。

- [ ] **Step 4: Commit documentation**

```bash
git add README.md docs/00-getting-started.md docs/zh/00-getting-started.md
git commit -m "docs: explain configure-only compile database generation"
```

### Task 8: Full Verification, Diff Review, Push, and A-Part PR

**Files:**
- Review all files changed from `origin/main`

- [ ] **Step 1: Rebuild from the current branch and select the fresh binary**

```bash
mcpp build --no-color
FRESH_MCPP="$(find "$PWD/target" -type f \( -path '*/bin/mcpp' -o -path '*/bin/mcpp.exe' \) -print0 | xargs -0 ls -1t | head -1)"
"$FRESH_MCPP" --version
```

预期：build 成功，fresh binary 版本与 `mcpp.toml` 一致。

- [ ] **Step 2: Run the complete unit/integration suite**

```bash
"$FRESH_MCPP" test --no-color
```

预期：所有 test binaries 通过；不能把 cache hit、未发现测试或旧 binary 当作成功证据。

- [ ] **Step 3: Run focused cross-cutting E2Es with the fresh binary**

```bash
for test_script in \
  tests/e2e/01_help_and_version.sh \
  tests/e2e/18_devdeps_isolation.sh \
  tests/e2e/35_workspace.sh \
  tests/e2e/76_compile_commands_generated.sh \
  tests/e2e/77_cdb_preserves_test_entries.sh \
  tests/e2e/90_workspace_test.sh \
  tests/e2e/157_test_glob_flags.sh \
  tests/e2e/159_test_list.sh \
  tests/e2e/202_configure_only_cdb.sh; do
    MCPP="$FRESH_MCPP" bash "$test_script"
done
```

预期：每个脚本输出 `OK` 或其既有成功文本。

- [ ] **Step 4: Review the diff for scope and cache safety**

```bash
git diff --check origin/main...HEAD
git diff --stat origin/main...HEAD
git diff origin/main...HEAD -- src/build src/platform src/cli.cppm tests README.md docs/00-getting-started.md docs/zh/00-getting-started.md
rg -n "populate_from|write_build_cache|\.build_cache|ui::finished|run_build_plan" src/build/configure.cppm src/cli/cmd_build.cppm
rg -n "ide |NDJSON|snapshot|envelope|current.json|invalidatedBy|FileLock" src tests/e2e/202_configure_only_cdb.sh README.md docs/00-getting-started.md docs/zh/00-getting-started.md
```

预期：`git diff --check` 无输出；configure module 不含被禁止的 build-success side effects；第二个范围扫描不出现 A 部分外协议实现。`cmd_build.cppm` 中普通 build 原有 `run_build_plan()` 命中属于预期，必须人工确认 configure-only 分支不可达。

- [ ] **Step 5: Confirm branch state and commit any review-only corrections**

```bash
git status --short --branch
git log --oneline --decorate origin/main..HEAD
```

若自审只发现注释或窄小修正，修改后重新运行对应 focused tests，并提交：

```bash
git add \
  src/build/test_targets.cppm \
  src/build/configure.cppm \
  src/build/execute.cppm \
  src/build/compile_commands.cppm \
  src/build/backend.cppm \
  src/build/ninja_backend.cppm \
  src/platform/fs.cppm \
  src/cli/cmd_build.cppm \
  src/cli.cppm \
  tests/unit/test_test_targets.cpp \
  tests/unit/test_platform_fs.cpp \
  tests/unit/test_compile_commands.cpp \
  tests/unit/test_configure.cpp \
  tests/unit/test_test_options.cpp \
  tests/e2e/01_help_and_version.sh \
  tests/e2e/202_configure_only_cdb.sh \
  README.md docs/00-getting-started.md docs/zh/00-getting-started.md
git commit -m "fix(build): tighten configure-only guarantees"
```

上述命令仅列出本计划拥有的路径；`git add` 会忽略其中未变化的文件。提交前仍需用
`git diff --cached --name-only` 确认没有用户或其他任务的改动，禁止 `git add -A`。

- [ ] **Step 6: Rebase current `origin/main` only after preserving unrelated work**

```bash
git fetch origin
git status --short
git rebase origin/main
```

预期：worktree clean 后 rebase；若出现用户或其他任务的未提交修改，先停止并区分归属，不得丢弃。rebase 后重复 Step 1-4 的 build、full test、focused E2E 与 diff review。

- [ ] **Step 7: Push only the feature branch to the fork**

```bash
git push -u fork codex/configure-only-cdb
```

不得 push `main`，不得覆盖 `.xlings.json` pin。

- [ ] **Step 8: Create the A-part PR**

PR title：

```text
feat: generate compile database without building
```

PR body：

```markdown
## Summary
- add `mcpp build --configure-only` using the real `prepare_build()` / `BuildPlan` path
- include regular sources and `tests/**/*.cpp`, with dev-dependencies and matching `[build].flags`
- stage only std/cached dependency BMIs needed by language tooling
- publish `compile_commands.json` atomically while keeping normal build failures non-fatal

Part of #379.

## Scope
- human output and exit code only
- no IDE wire protocol, snapshots, IDs, metadata graph, file lock, or `.xlings.json` change

## Test plan
- [x] `mcpp build --no-color`
- [x] fresh binary `mcpp test --no-color`
- [x] focused CDB/test/workspace E2Es including `202_configure_only_cdb.sh`
- [ ] GitHub Actions Linux/macOS/Windows required checks
```

创建命令：

```bash
gh pr create \
  --repo mcpp-community/mcpp \
  --head wellwei:codex/configure-only-cdb \
  --base main \
  --title "feat: generate compile database without building" \
  --body-file /tmp/mcpp-configure-only-pr.md
```

`/tmp/mcpp-configure-only-pr.md` 只作为临时 PR body，不提交仓库；内容必须与上方正文一致。

- [ ] **Step 9: Monitor CI and classify failures before editing**

```bash
gh pr checks --repo mcpp-community/mcpp --watch
```

任一失败先读取本分支最新 run 的失败日志：

```bash
RUN_ID="$(gh run list --repo mcpp-community/mcpp --branch codex/configure-only-cdb \
  --limit 1 --json databaseId --jq '.[0].databaseId')"
gh run view --repo mcpp-community/mcpp "$RUN_ID" --log-failed
```

比较本分支相关测试、同平台 main baseline、capability gating 和缓存状态。只修可归因于
本 PR 的失败；不把既有环境/索引/toolchain 故障伪装成 configure-only 代码问题。

## Final Acceptance Checklist

- [ ] 坏 C++ 源码仍可成功生成 CDB。
- [ ] CDB 同时覆盖普通 source 和 tests，并携带 dev-dep include/defines 与 `[build].flags`。
- [ ] 无测试项目不解析无关 dev-dependencies。
- [ ] configure-only 不 spawn Ninja、不生成普通 object/bin、不写 `target/.build_cache`、不 populate BMI cache。
- [ ] std BMI 与已缓存 dependency BMI 在 CDB 发布前物化；缺失/失败时旧 CDB 不变。
- [ ] CDB 内容未变化不改 mtime。
- [ ] Windows replacement 不先删除旧文件；Linux/macOS 使用同文件系统 rename。
- [ ] 普通 build/test 的 CDB failure 只 warning 并继续；configure-only 同类 failure 返回非零。
- [ ] workspace bare fan-out 与 `-p` 选择沿用既有语义。
- [ ] 文档明确命令不是只读操作，IDE 必须遵守 workspace trust。
- [ ] 没有 A 部分外 wire protocol、snapshot、metadata 或 pin 改动。
