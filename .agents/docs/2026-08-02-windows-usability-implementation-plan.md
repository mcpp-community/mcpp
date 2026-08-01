# Windows 可用性 — 实施计划

> **For agentic workers:** 本计划按任务逐条执行,每个任务自带测试循环与提交点。
> 步骤用 `- [ ]` 复选框跟踪。

**Goal:** 让 mcpp 在裸 Windows(无 MSVC STL)上无感可用,补齐含空格路径与 build.mcpp 的
方言/环境缺口,给 build.mcpp 接上 `import std;`,并把测试面从 1 个 Windows 镜像扩到
3 轴(含无 MSVC 轴)。

**Architecture:** 五个正交组件 A–E,见 `.agents/docs/2026-08-02-windows-usability-design.md`。
A(默认值分档)、B(引号收敛)、D(`import std;`)互不触碰同一函数,可并行推进;
C(build.mcpp 方言+环境)串行且依赖 B 的 argv[0] 修复;E(CI/e2e)随对应组件落地。

**Tech Stack:** C++23 modules,mcpp 自举构建(`mcpp build` / `mcpp test`),
gtest 单测(`tests/unit/`),bash e2e(`tests/e2e/`,`# requires:` 能力门),
GitHub Actions。

---

## Global Constraints

- **版本号**:本批次发 `2026.8.2.1`。规范 `YYYY.M.D.N`,月日不补零,`.0` 保留给稳定版。
- **版本号只改两处**:`mcpp.toml:3` 与 `src/toolchain/fingerprint.cppm:21`。
  **`.xlings.json:3` 是 bootstrap pin,是自举起点,不随本次发布走** —— 它只在 release
  完成、包已上架之后单独 bump。提前改会让全部 CI 去装一个不存在的版本。
- **校验**:任何版本改动后必须 `bash .github/tools/check_version_pins.sh` 通过。
- **不引入 clang + libc++ 的任何形态**(设计 §7.1)。
- **不新增配置字段**:意图来源分档复用现有的两层配置(项目 `mcpp.toml` vs 全局
  `config.toml`),不落盘任何新状态。
- **同一决策只推导一次**:新增的 MSVC 可用性判据必须与 `prepare.cppm:1284` 现有的
  「有 VC tools 缺 SDK」检查**合并成一处**,不得并排两个 if。
- **e2e `# requires:` 必须在脚本第 2 行**,否则等同没写。
- **单测风格**:`#include <gtest/gtest.h>` + `import std;` + `import mcpp.<module>;`。
- **分支**:`feat/windows-usability`,单 PR,合入用 bypass squash。

---

## File Structure

| 文件 | 责任 | 本计划中的变化 |
|---|---|---|
| `src/toolchain/msvc.cppm` | VS/SDK 发现 | **+** `has_usable_msvc()` 谓词 |
| `src/toolchain/triple.cppm` | triple 词表 + 版本 pin | `kFirstRunMacWin` 拆三 |
| `src/toolchain/dialect.cppm` | 命令方言表 | **+** 四个链接/语言字段 |
| `src/build/prepare.cppm` | 构建前置:工具链解析、依赖、计划 | 首跑分档、意图来源、修复门 |
| `src/build/flags.cppm` | 全局编译/链接 flag 组装 | **+** `include_token()` 导出 |
| `src/build/ninja_backend.cppm` | ninja 文件生成 | `local_include_flags` 改调 helper |
| `src/build/build_program.cppm` | build.mcpp 编译/执行/指令解析 | 方言化、env、`import std;` |
| `src/platform/process.cppm` | 进程执行 | Windows `argv[0]` 引号 |
| `tests/unit/test_windows_defaults.cpp` | **新** — A 组件单测 | 新建 |
| `tests/unit/test_build_flags.cpp` | flags 单测 | **+** `include_token` 用例 |
| `tests/unit/test_dialect.cpp` | **新** — 方言字段单测 | 新建 |
| `tests/e2e/179_spaced_paths.sh` | **新** — 含空格路径 | 新建 |
| `tests/e2e/180_msvc_build_mcpp.sh` | **新** — MSVC × build.mcpp | 新建 |
| `tests/e2e/181_build_mcpp_import_std.sh` | **新** — build.mcpp `import std;` | 新建 |
| `tests/e2e/182_windows_no_msvc_fallback.sh` | **新** — 回退 + 意图分档 | 新建 |
| `tests/e2e/112_build_mcpp_cross.sh` | 交叉 build.mcpp | **+** `import std;` 断言 |
| `.github/workflows/ci-fresh-install.yml` | 全新安装验证 | windows-fresh 拆三轴 |
| `.github/workflows/ci-windows.yml` | Windows CI | MSVC 步骤补 180 |
| `docs/03-toolchains.md` `docs/07-build-mcpp.md` `README*.md` | 用户文档 | 同步新行为 |

---

## Task 0: 分支与版本号

**Files:**
- Modify: `mcpp.toml:3`
- Modify: `src/toolchain/fingerprint.cppm:21`

- [ ] **Step 1: 建分支**

```bash
git checkout -b feat/windows-usability
```

- [ ] **Step 2: bump 两处版本号(且仅两处)**

`mcpp.toml:3`:`version = "2026.8.1.1"` → `version = "2026.8.2.1"`
`src/toolchain/fingerprint.cppm:21`:`MCPP_VERSION = "2026.8.1.1"` → `"2026.8.2.1"`

`.xlings.json:3` **保持 `2026.8.1.1` 不动**。

- [ ] **Step 3: 校验 pin 一致性**

Run: `bash .github/tools/check_version_pins.sh`
Expected: 退出码 0,输出 `mcpp version: building=2026.8.2.1 (fingerprint=2026.8.2.1) bootstrap pin=2026.8.1.1`

- [ ] **Step 4: 提交**

```bash
git add mcpp.toml src/toolchain/fingerprint.cppm .agents/docs/
git commit -m "chore: bump to 2026.8.2.1 + windows usability design/plan docs"
```

---

## Task 1: `has_usable_msvc()` 谓词

**Files:**
- Modify: `src/toolchain/msvc.cppm`(export 区 `:25-40`,实现区 `:239` 附近)
- Test: `tests/unit/test_windows_defaults.cpp`(新建)

**Interfaces:**
- Produces: `bool mcpp::toolchain::msvc::has_usable_msvc();`
  —— 当且仅当 `find_std_module_source()` 与 `find_windows_sdk()` 都返回值时为 `true`。
  非 Windows 平台恒为 `false`。

- [ ] **Step 1: 写失败测试**

`tests/unit/test_windows_defaults.cpp`:

```cpp
#include <gtest/gtest.h>

import std;
import mcpp.toolchain.msvc;
import mcpp.platform;

// has_usable_msvc() 的契约:两件齐才为真。非 Windows 恒假。
// 在 Windows CI 上这两条分别覆盖有 VS / 遮蔽后无 VS 的机器。
TEST(WindowsDefaults, HasUsableMsvcIsFalseOffWindows) {
    if constexpr (!mcpp::platform::is_windows) {
        EXPECT_FALSE(mcpp::toolchain::msvc::has_usable_msvc());
    } else {
        GTEST_SKIP() << "windows-specific path covered by e2e 182";
    }
}

// 谓词必须与它的两个组成部分一致 —— 不允许出现
// 「has_usable_msvc() 为真但 find_windows_sdk() 为空」这种自相矛盾。
TEST(WindowsDefaults, HasUsableMsvcAgreesWithItsParts) {
    const bool both = mcpp::toolchain::msvc::find_std_module_source().has_value()
                   && mcpp::toolchain::msvc::find_windows_sdk().has_value();
    EXPECT_EQ(mcpp::toolchain::msvc::has_usable_msvc(), both);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `mcpp test -- --gtest_filter='WindowsDefaults.*'`
Expected: 编译失败 —— `has_usable_msvc` 未声明。

- [ ] **Step 3: 实现**

`src/toolchain/msvc.cppm` export 区(紧跟 `find_windows_sdk` 声明之后)加:

```cpp
// True only when BOTH halves of a usable MSVC C++ setup are present:
// the STL's std module source AND the Windows SDK. Either alone is a
// half-installed state (VS with only the .NET workload; VC tools without
// the SDK) that fails at compile time instead of at selection time —
// which is exactly the bug this predicate exists to prevent.
bool has_usable_msvc();
```

实现区:

```cpp
bool has_usable_msvc() {
#if defined(_WIN32)
    return find_std_module_source().has_value() && find_windows_sdk().has_value();
#else
    return false;
#endif
}
```

- [ ] **Step 4: 跑测试确认通过**

Run: `mcpp test -- --gtest_filter='WindowsDefaults.*'`
Expected: PASS(Linux 上第一条通过、第二条 both=false 也通过)

- [ ] **Step 5: 提交**

```bash
git add src/toolchain/msvc.cppm tests/unit/test_windows_defaults.cpp
git commit -m "feat(toolchain): add msvc::has_usable_msvc() — STL and SDK both present"
```

---

## Task 2: pin 拆分 + 首跑分档

**Files:**
- Modify: `src/toolchain/triple.cppm:146-154`(pins 块)
- Modify: `src/build/prepare.cppm:1167-1183`(offline 硬错误)、`:1207-1232`(首跑)
- Test: `tests/unit/test_windows_defaults.cpp`

**Interfaces:**
- Consumes: `msvc::has_usable_msvc()`(Task 1)
- Produces:
  - `pins::kFirstRunMac` = `"llvm@20.1.7"`
  - `pins::kFirstRunWinMsvc` = `"llvm@20.1.7"`
  - `pins::kFirstRunWinGnu` = `"gcc@16.1.0"`
  - `pins::kFirstRunWinGnuTarget` = `"x86_64-windows-gnu"`
  - `kFirstRunMacWin` **删除**(全仓库无残留引用)

- [ ] **Step 1: 写失败测试**

追加到 `tests/unit/test_windows_defaults.cpp`:

```cpp
import mcpp.toolchain.triple;

// 拆分后的三个 pin 必须各自可解析,且 GNU 档位的 target 必须是已登记的
// verified 目标 —— 否则回退会把用户送进一个 mcpp 拒绝构建的 target。
TEST(WindowsDefaults, FirstRunPinsParse) {
    namespace pins = mcpp::toolchain::triple::pins;
    for (auto spec : { pins::kFirstRunMac, pins::kFirstRunWinMsvc,
                       pins::kFirstRunWinGnu }) {
        auto parsed = mcpp::toolchain::parse_toolchain_spec(std::string(spec));
        ASSERT_TRUE(parsed.has_value()) << spec;
        EXPECT_FALSE(parsed->version.empty()) << spec;
    }
}

TEST(WindowsDefaults, GnuFallbackTargetIsVerified) {
    namespace triple = mcpp::toolchain::triple;
    auto t = triple::parse(std::string(triple::pins::kFirstRunWinGnuTarget));
    ASSERT_TRUE(t.has_value());
    const auto* known = triple::find_known_target(*t);
    ASSERT_NE(known, nullptr);
    EXPECT_EQ(known->tier, "verified");
    // 词表 pin 必须与回退用的工具链一致,否则两处推导会漂移。
    EXPECT_EQ(known->pin, triple::pins::kFirstRunWinGnu);
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `mcpp test -- --gtest_filter='WindowsDefaults.FirstRun*:WindowsDefaults.GnuFallback*'`
Expected: 编译失败 —— `kFirstRunMac` 等未声明。

- [ ] **Step 3: 改 pins 块**

`src/toolchain/triple.cppm:146-154` 替换为:

```cpp
namespace pins {
    // First-run auto-install defaults (prepare.cppm), per host platform/arch.
    //
    // macOS and Windows used to share ONE pin. They must not: Apple ships no
    // GCC so LLVM is the only self-contained choice there, while on Windows
    // clang targets the MSVC ABI and therefore needs Visual Studio's STL +
    // the Windows SDK — neither of which is preinstalled. A bare Windows box
    // got a default it could never build with, and no diagnostic.
    inline constexpr std::string_view kFirstRunMac          = "llvm@20.1.7";
    // Windows WITH a usable MSVC (STL + SDK): unchanged behavior — the MSVC
    // ABI is what lets a project link vcpkg / third-party .lib artifacts.
    inline constexpr std::string_view kFirstRunWinMsvc      = "llvm@20.1.7";
    // Windows WITHOUT one: winlibs GCC targeting PE/GNU. Fully self-contained
    // (static libstdc++/libgcc, its own UCRT), zero VS dependency, `import std`
    // works. Must stay equal to kKnownTargets["x86_64-windows-gnu"].pin.
    inline constexpr std::string_view kFirstRunWinGnu       = "gcc@16.1.0";
    inline constexpr std::string_view kFirstRunWinGnuTarget = "x86_64-windows-gnu";
    inline constexpr std::string_view kFirstRunLinuxX86_64  = "gcc@16.1.0";
    inline constexpr std::string_view kFirstRunLinuxOther   = "gcc@15.1.0-musl";
    // Suggested install spellings used by help / MCPP_NO_AUTO_INSTALL errors.
    inline constexpr std::string_view kSuggestLlvm          = "llvm 20.1.7";
    inline constexpr std::string_view kSuggestGccMusl       = "gcc 15.1.0-musl";
    inline constexpr std::string_view kSuggestGccMingw      = "gcc 16.1.0";
} // namespace pins
```

- [ ] **Step 4: 改首跑分支**

`src/build/prepare.cppm:1209-1215` 的 `if constexpr (is_macos || is_windows)` 拆开:

```cpp
        namespace pins = mcpp::toolchain::triple::pins;
        std::string defaultSpec;
        std::string defaultTargetSpec;   // empty = host target
        if constexpr (mcpp::platform::is_macos) {
            defaultSpec = std::string(pins::kFirstRunMac);
        } else if constexpr (mcpp::platform::is_windows) {
            // detection-first: the MSVC ABI is only a viable default when the
            // machine actually has the MSVC STL *and* the Windows SDK. Without
            // them, fall back to the self-contained winlibs GCC — same product
            // shape (PE, static), zero Visual Studio dependency.
            if (mcpp::toolchain::msvc::has_usable_msvc()) {
                defaultSpec = std::string(pins::kFirstRunWinMsvc);
            } else {
                defaultSpec       = std::string(pins::kFirstRunWinGnu);
                defaultTargetSpec = std::string(pins::kFirstRunWinGnuTarget);
            }
        } else if (mcpp::platform::host_arch == std::string_view("x86_64")) {
            defaultSpec = std::string(pins::kFirstRunLinuxX86_64);
        } else {
            defaultSpec = std::string(pins::kFirstRunLinuxOther);
        }
```

首跑 info 文案(`:1223-1231`)同步分档:Windows-GNU 档位说明「未检测到 Visual Studio,
使用自包含的 MinGW-w64 工具链」。

持久化(`:1265-1275`)在 `defaultTargetSpec` 非空时**同时写 `defaultTarget`**,并把
`overrides.target_triple` 设为它,使本次构建立即生效。

- [ ] **Step 5: 改 offline 硬错误分支**

`prepare.cppm:1167-1174` 的 macOS/Windows 合并分支拆开;Windows 且 `!has_usable_msvc()`
时建议 `mcpp toolchain install gcc 16.1.0` + `mcpp toolchain default gcc@16.1.0 --target x86_64-windows-gnu`,
而不是建议一条在该机器上不可用的 llvm 命令。

- [ ] **Step 6: 全仓库确认无 `kFirstRunMacWin` 残留**

Run: `grep -rn "kFirstRunMacWin" src/ docs/ .github/`
Expected: 无输出。

- [ ] **Step 7: 跑测试**

Run: `mcpp test -- --gtest_filter='WindowsDefaults.*'`
Expected: PASS

- [ ] **Step 8: 提交**

```bash
git add src/toolchain/triple.cppm src/build/prepare.cppm tests/unit/test_windows_defaults.cpp
git commit -m "feat(toolchain): detection-first Windows first-run default (winlibs GCC when no MSVC)"
```

---

## Task 3: 意图来源分档 + 存量修复门

**Files:**
- Modify: `src/build/prepare.cppm:947`/`:951`/`:1015`/`:1024`/`:1274`(来源标记)
- Modify: `src/build/prepare.cppm:1280-1292`(现有 MSVC 诊断 → 合并成统一门)

**Interfaces:**
- Consumes: `msvc::has_usable_msvc()`(Task 1)、pins(Task 2)
- Produces: 文件内的 `enum class TcOrigin { ManifestToolchain, GlobalDefault,
  TargetSection, TargetPin, FirstRun }` 与 `bool tc_origin_is_user_explicit(TcOrigin)`

- [ ] **Step 1: 写失败测试**

追加到 `tests/unit/test_windows_defaults.cpp` —— 分档策略是纯函数,可直接测:

```cpp
import mcpp.build.prepare;

// 分档判据:只有用户显式写下的两种来源算「显式」。mcpp 自选的三种可以被
// 自动修复。写错这张表的后果是两个方向的:把显式判成自选 → 静默推翻用户的
// ABI 选择;把自选判成显式 → 存量用户永远撞墙。
TEST(WindowsDefaults, OriginClassification) {
    using mcpp::build::TcOrigin;
    EXPECT_TRUE (mcpp::build::tc_origin_is_user_explicit(TcOrigin::ManifestToolchain));
    EXPECT_TRUE (mcpp::build::tc_origin_is_user_explicit(TcOrigin::TargetSection));
    EXPECT_FALSE(mcpp::build::tc_origin_is_user_explicit(TcOrigin::GlobalDefault));
    EXPECT_FALSE(mcpp::build::tc_origin_is_user_explicit(TcOrigin::TargetPin));
    EXPECT_FALSE(mcpp::build::tc_origin_is_user_explicit(TcOrigin::FirstRun));
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `mcpp test -- --gtest_filter='WindowsDefaults.OriginClassification'`
Expected: 编译失败。

- [ ] **Step 3: 加枚举与谓词并 export**

`src/build/prepare.cppm` export 区:

```cpp
// Where the resolved toolchain spec came from. Used to decide whether an
// unusable toolchain may be auto-repaired: mcpp may revise a default it
// picked itself, but must never silently overrule a choice the user wrote
// down (a project that needs the MSVC ABI to link vcpkg .lib artifacts is
// better served by an error than by a silent ABI swap).
enum class TcOrigin {
    ManifestToolchain,  // mcpp.toml [toolchain]        — user explicit
    TargetSection,      // mcpp.toml [target.X].toolchain — user explicit
    GlobalDefault,      // config.toml [toolchain] default
    TargetPin,          // triple.cppm vocabulary convention
    FirstRun,           // written by this very invocation
};

inline bool tc_origin_is_user_explicit(TcOrigin o) {
    return o == TcOrigin::ManifestToolchain || o == TcOrigin::TargetSection;
}
```

- [ ] **Step 4: 在五个赋值点打标记**

| 行 | 赋值 | 标记 |
|---|---|---|
| `:947` | `tcSpec = m->toolchain.for_platform(...)` | `ManifestToolchain` |
| `:951` | `tcSpec = (*cfg)->defaultToolchain` | `GlobalDefault` |
| `:1015` | `tcSpec = it->second.toolchain` | `TargetSection` |
| `:1024` | `tcSpec = std::string(known->pin)` | `TargetPin` |
| `:1274` | `tcSpec = defaultSpec` | `FirstRun` |

- [ ] **Step 5: 把现有诊断替换成统一门**

`prepare.cppm:1280-1292` 现在是:

```cpp
    if (tc->compiler == mcpp::toolchain::CompilerId::MSVC
        && tc->envOverrides.empty()) {
        return std::unexpected(std::format("msvc {} was detected at {}, ..."));
    }
```

替换为一个判据、两个分支:

```cpp
    // One decision, one derivation: "this build targets the MSVC ABI but the
    // machine has no usable MSVC" covers BOTH the old `msvc@system without a
    // Windows SDK` case and the (previously undiagnosed) `clang targeting the
    // MSVC ABI on a box with no Visual Studio at all` case. Splitting them
    // into two ifs is how the second one went unnoticed in the first place.
    const bool targetsMsvcAbi =
        tc->compiler == mcpp::toolchain::CompilerId::MSVC
        || mcpp::toolchain::is_msvc_target(*tc);
    if (targetsMsvcAbi && !mcpp::toolchain::msvc::has_usable_msvc()) {
        if (tc_origin_is_user_explicit(tcOrigin)
            || mcpp::platform::env::offline_mode()
            || mcpp::platform::env::no_auto_install()) {
            return std::unexpected(msvc_unavailable_guidance(*tc));
        }
        // mcpp picked this default itself and it cannot work here — revise it.
        // This gate runs on EVERY build, which is what repairs users who
        // already have `llvm@20.1.7` persisted from an older mcpp: the
        // first-run branch never fires again for them.
        ... 切到 kFirstRunWinGnu + kFirstRunWinGnuTarget,重写全局 config,
            info 一行,然后重新 detect() ...
    }
```

`msvc_unavailable_guidance()` 输出设计 §2.6 的文案,并在 `envOverrides.empty()` 且
探到了 VC tools 时保留原有的「装 Windows SDK 组件」措辞,不丢信息。

- [ ] **Step 6: 跑测试 + 全量单测**

Run: `mcpp test`
Expected: 全过。

- [ ] **Step 7: 提交**

```bash
git add src/build/prepare.cppm tests/unit/test_windows_defaults.cpp
git commit -m "feat(build): auto-repair an unusable MSVC default, diagnose an explicit one"
```

---

## Task 4: include token 引号收敛

**Files:**
- Modify: `src/build/flags.cppm:215-242`(抽出 helper 并 export)
- Modify: `src/build/ninja_backend.cppm:108-132`(改调 helper)
- Test: `tests/unit/test_build_flags.cpp`

**Interfaces:**
- Produces:
  `std::string mcpp::build::include_token(const mcpp::toolchain::CommandDialect& d,
                                          const std::filesystem::path& dir,
                                          std::string_view prefixOverride = {});`
  返回**已加前缀、已做 ninja `$` 转义、已加 shell 引号**的单个 token(不含前导空格)。
  `prefixOverride` 非空时取代 `d.includePrefix`(供 `-idirafter` / NASM `-I` 复用)。

- [ ] **Step 1: 写失败测试**

追加到 `tests/unit/test_build_flags.cpp`:

```cpp
import mcpp.toolchain.dialect;

// #331: 同一份 manifest include_dirs 走两条通道到命令行,只有 flags.cppm 那条
// 做了 shell 引号。含空格的路径在 per-TU 通道上裂成多个 shell 词。两条通道
// 必须调同一个 helper —— 抄一遍不算修好,下一条通道还会漏。
TEST(BuildFlags, IncludeTokenQuotesSpaces) {
    const auto& gnu = mcpp::toolchain::gnu_dialect();
    auto tok = mcpp::build::include_token(gnu, std::filesystem::path("/home/my dir/inc"));
    // 引号之后,交给 shell 时必须仍是单个词。
    EXPECT_NE(tok.find("my dir"), std::string::npos);
    EXPECT_TRUE(tok.starts_with("'-I") || tok.starts_with("\"-I") || tok.find('\'') != std::string::npos)
        << "unquoted token: " << tok;
}

TEST(BuildFlags, IncludeTokenUsesDialectPrefix) {
    const auto& msvc = mcpp::toolchain::msvc_dialect();
    auto tok = mcpp::build::include_token(msvc, std::filesystem::path("/tmp/inc"));
    EXPECT_NE(tok.find("/I"), std::string::npos);
    EXPECT_EQ(tok.find("-I"), std::string::npos);
}

TEST(BuildFlags, IncludeTokenPrefixOverride) {
    const auto& gnu = mcpp::toolchain::gnu_dialect();
    auto tok = mcpp::build::include_token(gnu, std::filesystem::path("/tmp/inc"), "-idirafter");
    EXPECT_NE(tok.find("-idirafter"), std::string::npos);
}
```

> 若 `gnu_dialect()` / `msvc_dialect()` 的实际取用函数名与此不同,改用
> `dialect.cppm` 中真实的取用入口(实现时以 `grep -n "dialect_for\|gnu_dialect" src/toolchain/dialect.cppm` 为准)。

- [ ] **Step 2: 跑测试确认失败**

Run: `mcpp test -- --gtest_filter='BuildFlags.IncludeToken*'`
Expected: 编译失败 —— `include_token` 未声明。

- [ ] **Step 3: 抽 helper**

`src/build/flags.cppm` export 区加声明,实现区把 `:222-242` 现有逻辑抽成:

```cpp
std::string include_token(const mcpp::toolchain::CommandDialect& d,
                          const std::filesystem::path& dir,
                          std::string_view prefixOverride) {
    std::string prefix(prefixOverride.empty() ? d.includePrefix : prefixOverride);
    return shell_quote_arg(escape_path(std::filesystem::path(prefix + dir.string())));
}
```

`:222-242` 改为调用它,行为不变(回归由现有 flags 单测兜住)。

- [ ] **Step 4: ninja_backend 改调 helper**

`src/build/ninja_backend.cppm:108-132` 的 `local_include_flags` 需要拿到 dialect。
签名从 `(const CompileUnit& cu, bool msvcDialect)` 改为
`(const CompileUnit& cu, const mcpp::toolchain::CommandDialect& d)`,调用点同步。
函数体:

```cpp
std::string local_include_flags(const CompileUnit& cu,
                                const mcpp::toolchain::CommandDialect& d) {
    const bool nasmUnit = is_nasm_source(cu.source);
    const bool msvcDialect = d.includePrefix == std::string_view("/I");
    std::string flags;
    for (auto const& inc : cu.localIncludeDirs) {
        flags += ' ';
        flags += mcpp::build::include_token(d, inc);
    }
    // #249 的三态降级保持不变,只把 quoting 并轨。
    for (auto const& inc : cu.localIncludeDirsAfter) {
        std::string_view pfx = nasmUnit ? "-I" : (msvcDialect ? "/I" : "-idirafter");
        flags += ' ';
        flags += mcpp::build::include_token(d, inc, pfx);
    }
    return flags;
}
```

- [ ] **Step 5: 跑测试**

Run: `mcpp test -- --gtest_filter='BuildFlags.*:NinjaBackend.*'`
Expected: PASS

- [ ] **Step 6: 自建回归(归一化 diff build.ninja)**

```bash
mcpp build -p mcpp 2>/dev/null || mcpp build
# 与 main 的 build.ninja 做归一化 diff,确认除 include token 的引号外无差异
```

- [ ] **Step 7: 提交**

```bash
git add src/build/flags.cppm src/build/ninja_backend.cppm tests/unit/test_build_flags.cpp
git commit -m "fix(build): quote per-TU include dirs — converge both channels on one helper (#331)"
```

---

## Task 5: `command_from_argv` 的 Windows argv[0] 引号

**Files:**
- Modify: `src/platform/process.cppm:234`
- Test: `tests/unit/test_platform_process.cpp`(若不存在则新建)

**Interfaces:**
- 无新导出;`command_from_argv` 行为变更(Windows 分支)。

- [ ] **Step 1: 写失败测试**

```cpp
#include <gtest/gtest.h>
import std;
import mcpp.platform.process;

// #331 #1a: Windows 走 cmd.exe /c 时 argv[0] 未加引号,payload 装在
// "C:\Program Files\..." 或用户名含空格的机器上,命令行在第一个空格处断开。
// 这条不是 MSVC 专属 —— 是全 Windows 的路径假设。
TEST(PlatformProcess, ArgvZeroWithSpacesStaysOneToken) {
    std::vector<std::string> argv = {
        "C:\\Program Files\\mcpp\\g++.exe", "-c", "main.cpp" };
    auto cmd = mcpp::platform::process::command_from_argv(argv);
    if constexpr (mcpp::platform::is_windows) {
        EXPECT_TRUE(cmd.starts_with('"')) << cmd;
        EXPECT_NE(cmd.find("\"C:\\Program Files\\mcpp\\g++.exe\""), std::string::npos) << cmd;
    } else {
        // POSIX 侧已有引号,断言不回退
        EXPECT_NE(cmd.find("Program Files"), std::string::npos) << cmd;
    }
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `mcpp test -- --gtest_filter='PlatformProcess.*'`
Expected: 若 `command_from_argv` 未导出则编译失败 → 先 export 再跑;Windows 上 FAIL。

- [ ] **Step 3: 实现**

`process.cppm:234` 的 Windows 分支对 `argv[0]` 加双引号(其余参数沿用现有规则)。

> **实测门**:`cmd.exe /c` 对整条命令的引号剥壳规则与 `CreateProcess` 不同。
> 本步骤的最终形态**必须由 Windows CI 的实跑结果确定**,不得只凭 Linux 侧推断合入。
> e2e 179(Task 10)在 Windows 上跑通即为验收。

- [ ] **Step 4: 跑测试**

Run: `mcpp test -- --gtest_filter='PlatformProcess.*'`
Expected: PASS(Linux);Windows 由 CI 验收。

- [ ] **Step 5: 提交**

```bash
git add src/platform/process.cppm tests/unit/test_platform_process.cpp
git commit -m "fix(platform): quote argv[0] on Windows — payload paths with spaces (#331)"
```

---

## Task 6: build.mcpp 支持 `import std;`

**Files:**
- Modify: `src/build/build_program.cppm:660-700`
- Modify: `src/build/prepare.cppm`(把 `host_tc_for_build_program()` 的结果与
  `m->package.standard` / `stdFlagAndDialect` 传进 build.mcpp 调用)
- Test: `tests/e2e/181_build_mcpp_import_std.sh`(新建)

**Interfaces:**
- Consumes: `mcpp::toolchain::stdmod::ensure_built(tc, standard, dialectFlags, macosDeploymentTarget)`
  → `StdModule{ bmiPath, objectPath, compatBmiPath, compatObjectPath }`(`stdmod.cppm:46/63`)
- Consumes: `prepare.cppm:1349` 的 `host_tc_for_build_program()` → `{frontend, Toolchain}`

- [ ] **Step 1: 写失败 e2e**

`tests/e2e/181_build_mcpp_import_std.sh`(注意 `# requires:` 必须在**第 2 行**):

```bash
#!/usr/bin/env bash
# requires: unix-shell
# 181_build_mcpp_import_std.sh — build.mcpp can `import std;`
#   mcpp 让用户全项目 import std,却要求构建脚本回退到 #include —— 这条测试
#   锁住那个缺口被补上。跨平台:import std 不是 Windows 专属问题。
set -e

TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
cd "$TMP"
"$MCPP" new imp_std >/dev/null 2>&1
cd imp_std

cat > build.mcpp <<'EOF'
import std;
int main() {
    std::vector<std::string> flags{"MCPP_FROM_IMPORT_STD"};
    for (auto const& f : flags) std::println("mcpp:cfg={}", f);
    std::println("mcpp:rerun-if-changed=build.mcpp");
    return 0;
}
EOF

cat > src/main.cpp <<'EOF'
import std;
int main() {
#ifdef MCPP_FROM_IMPORT_STD
    std::println("import-std-ok");
    return 0;
#else
    std::println("define missing");
    return 1;
#endif
}
EOF

out=$("$MCPP" build 2>&1) || { echo "FAIL: build: $out"; exit 1; }
run_out=$("$MCPP" run 2>&1) || { echo "FAIL: run: $run_out"; exit 1; }
[[ "$run_out" == *"import-std-ok"* ]] || { echo "FAIL: run output: $run_out"; exit 1; }

echo "PASS: build.mcpp import std"
```

- [ ] **Step 2: 跑 e2e 确认失败**

Run: `MCPP=$(pwd)/target/<fingerprint>/release/bin/mcpp bash tests/e2e/181_build_mcpp_import_std.sh`
Expected: FAIL —— build.mcpp 编译报错(找不到 std 模块)。

> **坑**:`target/` 下有多个指纹目录,`ls | head -1` 会取到旧版本的二进制。
> 用 `mcpp build` 的输出路径,或 `find target -name mcpp -newer mcpp.toml`。

- [ ] **Step 3: 实现**

`build_program.cppm` 检测扩展:

```cpp
    bool usesModule   = srcText.find("import mcpp") != std::string::npos;
    bool usesStd      = srcText.find("import std;") != std::string::npos
                     || srcText.find("import std.compat;") != std::string::npos;
    bool usesStdCompat= srcText.find("import std.compat;") != std::string::npos;
```

`usesStd` 为真时,调 `stdmod::ensure_built(hostTc, standard, stdFlagAndDialect, deploymentTarget)`,
然后按方言接入:

- GCC:把 `sm->bmiPath` 暂存到 `bdir/gcm.cache/std.gcm`(`std.compat.gcm` 同理),
  并令 `compileCwd = bdir`(与 `usesModule` 共用同一条 cwd 决策,不要写成两个 if);
- Clang:`compileArgv.push_back(std::string(traits.stdBmiUsePrefix) + sm->bmiPath.string())`,
  复用 `flags.cppm:358` 的同一前缀常量;
- 两者都把 `sm->objectPath`(及 compat 的)推进 `compileArgv`,位置同 `mcpp.o`。

**必须喂宿主工具链**:调用点传的是 `host_tc_for_build_program()` 返回的 `htc`,
**不是** `*tc`。喂 `*tc` 会在交叉构建下产出跑不了的 helper。

- [ ] **Step 4: 跑 e2e**

Run: `bash tests/e2e/181_build_mcpp_import_std.sh`
Expected: `PASS: build.mcpp import std`

- [ ] **Step 5: 更新 `kMcppModuleSource` 的注释**

`build_program.cppm:247` 的 *"no `#include`, no `import std;`"* 改为陈述现状
(内置模块用 C 级原语实现,与 build.mcpp 能否 `import std;` 无关),不改代码。

- [ ] **Step 6: 提交**

```bash
git add src/build/build_program.cppm src/build/prepare.cppm tests/e2e/181_build_mcpp_import_std.sh
git commit -m "feat(build.mcpp): support import std; via the shared stdmod cache"
```

---

## Task 7: e2e 112 扩交叉 `import std;` 断言

**Files:**
- Modify: `tests/e2e/112_build_mcpp_cross.sh`

- [ ] **Step 1: 追加断言**

在现有交叉 build.mcpp 用例之后,追加一个 `import std;` 形态的 build.mcpp,
断言构建通过且 helper 在**宿主**上真的跑起来(产生了它该产生的 marker 文件)。
这是 §5.2 host≠target 的锁:喂错工具链时 helper 会在 exec 阶段失败。

- [ ] **Step 2: 跑**

Run: `bash tests/e2e/112_build_mcpp_cross.sh`(需 `mingw-cross` 能力,本机无则 CI 验收)

- [ ] **Step 3: 提交**

```bash
git add tests/e2e/112_build_mcpp_cross.sh
git commit -m "test(e2e): lock host!=target for build.mcpp import std (112)"
```

---

## Task 8: `CommandDialect` 扩四字段

**Files:**
- Modify: `src/toolchain/dialect.cppm:22-60`
- Test: `tests/unit/test_dialect.cpp`(新建)

**Interfaces:**
- Produces(`CommandDialect` 新成员):
  - `std::string_view libFlag;`         GNU `"-l{}"`   / MSVC `"{}.lib"`
  - `std::string_view libSearchPrefix;` GNU `"-L"`     / MSVC `"/LIBPATH:"`
  - `std::string_view forceCxxLang;`    GNU `"-x c++"` / MSVC `"/TP"`
  - `std::string_view staticRuntime;`   GNU `"-static"`/ MSVC `"/MT"`
- Produces: `std::string lib_flag_for(const CommandDialect&, std::string_view name);`
  —— 因为 MSVC 是后缀形态(`foo.lib`),单个前缀常量表达不了。

- [ ] **Step 1: 写失败测试**

```cpp
#include <gtest/gtest.h>
import std;
import mcpp.toolchain.dialect;

// 链接侧字段:GNU 是前缀形态、MSVC 是后缀形态,所以 libFlag 必须是格式串
// 而非前缀 —— 用 std::string_view prefix 表达不了 `foo.lib`。
TEST(Dialect, LibFlagBothShapes) {
    EXPECT_EQ(mcpp::toolchain::lib_flag_for(mcpp::toolchain::gnu_dialect(),  "z"), "-lz");
    EXPECT_EQ(mcpp::toolchain::lib_flag_for(mcpp::toolchain::msvc_dialect(), "z"), "z.lib");
}

TEST(Dialect, LinkAndLangFieldsArePopulated) {
    for (auto const* d : { &mcpp::toolchain::gnu_dialect(),
                           &mcpp::toolchain::msvc_dialect() }) {
        EXPECT_FALSE(d->libFlag.empty());
        EXPECT_FALSE(d->libSearchPrefix.empty());
        EXPECT_FALSE(d->forceCxxLang.empty());
        EXPECT_FALSE(d->staticRuntime.empty());
    }
}
```

- [ ] **Step 2: 跑测试确认失败**

Run: `mcpp test -- --gtest_filter='Dialect.*'`
Expected: 编译失败。

- [ ] **Step 3: 实现** —— 加四个字段到 `CommandDialect`、填两个方言表、加 `lib_flag_for`。

- [ ] **Step 4: 跑测试**

Run: `mcpp test -- --gtest_filter='Dialect.*'`
Expected: PASS

- [ ] **Step 5: 提交**

```bash
git add src/toolchain/dialect.cppm tests/unit/test_dialect.cpp
git commit -m "feat(toolchain): add link/language fields to CommandDialect"
```

---

## Task 9: build.mcpp 方言化(L2)+ 环境(L3)+ MSVC 模块门

**Files:**
- Modify: `src/build/build_program.cppm:132-140`(指令面)、`:168-243`(host_base_flags)、
  `:677-700`(compileArgv + capture_exec)

- [ ] **Step 1: 指令面改为方言无关**

`parse_line` 不再拼 `-l` / `-L`,改为产出中立字段 `d.libs` / `d.libSearchDirs`;
翻译推迟到拼 argv 处,用 `lib_flag_for()` / `d.libSearchPrefix`。
理由:`mcpp:` 协议是声明式的,方言属于消费端。

- [ ] **Step 2: compileArgv 方言化**

`-O0` → `d.optPrefix`+`0`;`-x c++` → `d.forceCxxLang`;`-static` → `d.staticRuntime`;
`-o <bin>` → 按方言(MSVC 用 `/Fe:`)。

- [ ] **Step 3: host_base_flags 加 MSVC 分支**

返回空 —— MSVC 的搜索路径全部经由 `INCLUDE` / `LIB` 环境变量。

- [ ] **Step 4: capture_exec 传 env**

`capture_exec(compileArgv, {}, compileCwd)` → 传 `tc.envOverrides`(`model.cppm:46`)。
这是三层里的最后一层:没有它,方言全对 cl.exe 依然找不到 `<cstdio>`。

- [ ] **Step 5: MSVC × 模块化 build.mcpp 的 unsupported 门**

`usesModule || usesStd` 且方言为 MSVC 时,返回:

```
build.mcpp: `import mcpp;` / `import std;` are not yet supported under MSVC.
            Use `#include` in build.mcpp, or build with a GCC/Clang toolchain.
```

一个门、一条诊断,不写成两处。

- [ ] **Step 6: 跑现有 build.mcpp e2e 确认无回归**

Run: `for t in 89 92 110 111 124 125 143 144 145 164 168; do bash tests/e2e/${t}_*.sh || echo "FAIL $t"; done`
Expected: 全 PASS(GNU 路径行为不变)

- [ ] **Step 7: 提交**

```bash
git add src/build/build_program.cppm
git commit -m "feat(build.mcpp): dialect-aware compile/directives + MSVC env plumbing (#331)"
```

---

## Task 10: e2e 179 含空格路径

**Files:**
- Create: `tests/e2e/179_spaced_paths.sh`

- [ ] **Step 1: 写测试**

第 2 行 `# requires: unix-shell`。在一个**路径含空格**的临时目录里建工程,
`[build] include_dirs` 指向一个含空格的目录,放一个只能通过该 include 找到的头文件,
再放一个 build.mcpp,断言 build + run 全绿。跨平台。

- [ ] **Step 2: 跑,确认在修复前失败 / 修复后通过**

Run: `bash tests/e2e/179_spaced_paths.sh`

- [ ] **Step 3: 提交**

---

## Task 11: e2e 180 MSVC × build.mcpp

**Files:**
- Create: `tests/e2e/180_msvc_build_mcpp.sh`(第 2 行 `# requires: windows msvc`)

- [ ] **Step 1: 写测试** —— `#include` 形态的 build.mcpp,在 `msvc@system` 下 build + run;
      并断言 `import std;` 形态给出的是**明确的 unsupported 诊断**而非裸崩。
- [ ] **Step 2: 由 Windows CI 验收**(本机无 MSVC)
- [ ] **Step 3: 提交**

---

## Task 12: e2e 182 裸 Windows 回退 + 意图分档

**Files:**
- Create: `tests/e2e/182_windows_no_msvc_fallback.sh`(第 2 行 `# requires: windows`)

- [ ] **Step 1: 写测试**

在遮蔽了 MSVC 的环境下(由 CI job 提供,脚本自身先断言 `toolchain default msvc` 失败):
1. 全新 MCPP_HOME 下 `mcpp new` + `build` + `run` 必须全绿;
2. `toolchain list` 星在 `x86_64-windows-gnu`;
3. 写一个显式 `[toolchain] windows = "llvm@20.1.7"` 的工程 → 必须**硬失败**且诊断里含
   `x86_64-windows-gnu` 指路(验证意图分档没把显式判成自选)。

- [ ] **Step 2: 由 Windows CI 验收**
- [ ] **Step 3: 提交**

---

## Task 13: CI —— fresh-install 三轴 + 遮蔽自证门

**Files:**
- Modify: `.github/workflows/ci-fresh-install.yml:375-440`
- Modify: `.github/workflows/ci-windows.yml:267`(MSVC 步骤补 180)

- [ ] **Step 1: 把 `windows-fresh` 的步骤抽成可复用单元**,避免三处推导。
- [ ] **Step 2: 加 `windows-2022-fresh` / `windows-2025-fresh` 两轴。**
- [ ] **Step 3: 加 `windows-nomsvc-fresh`** —— 遮蔽 vswhere / VS 根 / 四个环境变量,
      **第一步就断言 `mcpp toolchain default msvc` 必须失败**(自证门),再跑 182。
- [ ] **Step 4: `ci-windows.yml` 的 MSVC 步骤追加 `tests/e2e/180_msvc_build_mcpp.sh`。**
- [ ] **Step 5: 提交。**

---

## Task 14: 文档同步

**Files:**
- Modify: `docs/03-toolchains.md`(Windows 默认行为、回退规则)
- Modify: `docs/07-build-mcpp.md`(`import std;` 现已支持;MSVC 限制)
- Modify: `README.md` / `README.zh.md`(把 winlibs 从脚注提到正文)
- Modify: `docs/zh/` 对应中文页

- [ ] **Step 1–2: 改文档、检查 `[build] linkage` 的三处错误措辞(设计 §7.2)。**
- [ ] **Step 3: 提交。**

---

## Task 15: PR → CI 全绿 → 合入

- [ ] **Step 1: 本机全量回归**

```bash
mcpp test                              # 单测
bash tests/e2e/run_all.sh              # e2e(本机能力范围内)
bash .github/tools/check_version_pins.sh
```

- [ ] **Step 2: 开 PR**

```bash
git push -u origin feat/windows-usability
gh pr create --title "feat(windows): usable on a bare Windows box (2026.8.2.1)" --body "..."
```

- [ ] **Step 3: 盯 CI,红了就修,直到全绿。**

关注点:Windows 三轴、MSVC 步骤、cross-build-test(112)、Linux/macOS 无回归。

- [ ] **Step 4: 合入**

```bash
gh pr merge --squash --admin --delete-branch
```

> **坑**:叠栈 PR 时 `--delete-branch` 会 CLOSE 子 PR 且不可 reopen。本批次是单 PR,无此风险。

---

## Task 16: Release + 生态闭环

- [ ] **Step 1: 打 tag 触发 release.yml,产出四平台产物。**
- [ ] **Step 2: 镜像到 xlings-res 双端(GitHub + GitCode)。**

publish-ecosystem 的 per-file 超时会杀大件 —— 失败时**本地 gtc 补传**,
token 走 `~/.config/gitcode-tool/config.json` 而非环境变量;沙箱 wrapper 的 gtc 坏,
用 repo 里的 gtc + `/usr/bin/python3`。

- [ ] **Step 3: 两端独立 GET + sha256 核验**(不信 workflow 的绿灯;
      `obs_callback 400 EOF` 是假错,对象可能已落盘,须回探下载 URL 判定)。
- [ ] **Step 4: 更新 xim-pkgindex(PR 须用 Sunrisepeak 账号合)。**
- [ ] **Step 5: clean-room 验证**

用隔离的 `XLINGS_HOME` 真装 `xlings install mcpp@2026.8.2.1`,别动本地 `~/.xlings`;
索引 artifact CDN 滞后可达 ~40min,`not found` ≠ 回归,重跑即绿。

- [ ] **Step 6: bump bootstrap pin**

包上架**之后**才改 `.xlings.json:3` → `2026.8.2.1`,单独提交/PR。

---

## 实施记录(执行中发现、计划里没写的东西)

| 发现 | 影响 |
|---|---|
| **`command_from_argv` 的裸 argv[0] 是故意的** —— 注释写明引号会被 `cmd /c` 剥掉 | 真修法不在 `command_from_argv` 一处:要按 cmd.exe `/c` 的文档规则给整条命令**再包一层外引号**,让 cmd 吃掉它,内层引号才能抵达。`run_exec` 必须只包不封 stdin(`mcpp run` 要交互) |
| **`command_from_argv` 只在非 Linux/macOS 分支编译** | Linux 上测不到。把 Windows 的字符串成形抽成宿主无关的 `windows_command_from_argv` / `windows_wrap_for_cmd_c`,Linux CI 也能守住这条规则 —— 这个分支在开发平台上根本不编译,正是裸 argv[0] 活这么久的原因 |
| **`local_include_flags` 的 `msvcDialect` 形参只用于 after-dirs** | 普通 include 硬编码 `-I`。收敛到 `include_token` 后 MSVC 方言下变成 `/I`,`test_ninja_backend.cpp` 里有一条断言编码的正是这个 bug,已更新 |
| **两层转义,顺序固定** | ninja 的 `$ ` 在内、shell 引号在外。写断言时必须知道文件里是 `'-I/opt/my$ dep/include'`,单测和 e2e 各踩了一次 |
| **`ensure_built` 的 `tc` 已经是宿主工具链** | `prepare.cppm` 调 `run_build_program(*m, *root, host->first, host->second, ...)`,host≠target 在调用点就闭合了,组件 D 无需自己解析 |
| **`Directives` 会被序列化进 build.mcpp 缓存** | 计划里的「产出中立字段」会改缓存格式。改为在 parse 时按方言翻译 —— 缓存键已含 `compiler <hash>`,换工具链自动失效,所以安全且零格式变更 |
| **`mcpp toolchain default msvc` 也写 `config.toml`** | 与 mcpp 自己持久化的默认同源,会被误判成可改写。收紧:`tc->compiler == MSVC` 一律视为用户显式 —— mcpp 从不自选 msvc@system |
| **`[build] linkage` 根本不被解析** | `toml.cppm:995` 只在 `[target.<triple>]` 下读。文档三处 + `prepare.cppm` 一处注释都在教一个静默失效的键,已改 |
| **`no-msvc` 需要是一个显式能力** | 不能由「非 msvc」推出:e2e 182 必须 REQUIRE 它,否则在有 MSVC 的机器上会走普通路径并通过,证明不了任何事 |

## Self-Review

**Spec coverage**

| 设计节 | 覆盖任务 |
|---|---|
| §2 组件 A | Task 1(谓词)、2(pin+首跑)、3(分档+修复门+诊断+offline 例外) |
| §3 组件 B | Task 4(include 收敛)、5(argv[0]) |
| §4 组件 C | Task 8(方言字段)、9(L2+L3+模块门) |
| §5 组件 D | Task 6(`import std;`)、7(交叉锁) |
| §6 组件 E | Task 10–13(e2e 179/180/182 + 112 扩 + CI 三轴) |
| §7 拒绝项 | Task 14(文档修正 `[build] linkage` 措辞);其余为「不做」,无任务 |
| §9 顺序 | Task 编号即顺序;1–6 可并行,8–9 依赖 5 |

**已知遗留**(设计中明确不做,此处记录以免误判为漏项):
ARM64 Windows;真实客户端 SKU 行为;MSVC × 模块化 build.mcpp(Task 9 Step 5 只给诊断);
内置 `mcpp` 模块改用 `import std;`(Task 6 Step 5 只改注释)。
