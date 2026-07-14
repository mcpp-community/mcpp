# c++fly(0.0.91)单 PR 实施计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `standard = "c++fly"` 一键启用"resolved 工具链最新 -std= 档位 + 全部实验特性门(语言 + 标准库)",并顺手修复 `c++latest` 在 GNU 族误拼 `-std=c++latest` 的暗雷(设计 §11-Q5,已确认为真 bug)。

**Architecture:** 新模块 `mcpp.toolchain.cppfly`(三表数据 + 纯函数查询点)按〈CompilerId × major 版本 × stdlibId〉解析档位与旗标;产物经既有图全局方言旗标通道(`BuildPlan::dialectFlags` + prepare 的 `stdFlagAndDialect`)下发,两消费点通过同一对纯函数(`cppfly::std_flag` / `cppfly::effective_dialect_flags`)保证同源。设计文档:`.agents/docs/2026-07-14-std-features-experimental-gate-design.md`。

**Tech Stack:** C++23 modules(.cppm)、gtest 单测(`mcpp test`)、bash e2e(`tests/e2e/run_all.sh`)。

## Global Constraints

- 版本号:0.0.91(`mcpp.toml:3` `version` + `src/toolchain/fingerprint.cppm:21` `MCPP_VERSION` 两处)。
- 不写 `c++fly` 的 manifest 行为不变;`c++latest` 行为从"GNU 上产出非法 `-std=c++latest`"变为"解析到族最新档"(bug 修复,CHANGELOG 记 Fixed)。
- 已实机定案的表数据:reflection→GCC≥16 `-freflection`;contracts→GCC≥16 空串(随 `-std=c++26` 默认启用);libc++→`-fexperimental-library`;GCC≥14→c++26/GCC≤13→c++23;Clang≥17→c++2c;MSVC 由 `std_flag_for` 既有分支出 `/std:c++latest`。
- 单测跑法:`"$MCPP_FRESH" test`(自举:先 `mcpp build`);e2e:`MCPP=<fresh> tests/e2e/run_all.sh`(或单跑单脚本 `MCPP=<fresh> bash tests/e2e/<x>.sh`)。
- 本机注意:`~/.xlings` 的 g++ shim 坏(interp 烙死已消失的 /tmp 路径);构建全部通过 mcpp 自身工具链解析(`~/.mcpp/registry`)走,别依赖 PATH 上的 g++。
- commit 风格参照 git log:`feat(scope): ...` / `fix(scope): ...`,本 PR 最终 squash。

---

### Task 1: `src/toolchain/cppfly.cppm` — 三表 + 查询点

**Files:**
- Create: `src/toolchain/cppfly.cppm`
- Test: `tests/unit/test_cppfly.cpp`

**Interfaces (Produces):**
```cpp
namespace mcpp::toolchain::cppfly {
  struct FeatureState { std::string name, paper, flags, reason; bool enabled; };
  struct Resolution   { std::string stdCanonical; int stdLevel;
                        std::vector<std::string> flags;         // 全部启用门旗标,去重
                        std::vector<FeatureState> features; };  // enabled+skipped,summary 用
  int  compiler_major(const Toolchain& tc);                     // "16.1.0"→16;不可解析→0
  std::string latest_std_canonical(const Toolchain& tc, int* levelOut = nullptr);
  Resolution resolve(const Toolchain& tc);
  // c++latest/c++fly 感知的 std 旗标(其余 canonical 原样走 std_flag_for)
  std::string std_flag(const Toolchain& tc, std::string_view canonical, int level);
  // manifest 方言旗标 ∪ fly 门旗标(去重,保持声明序)——两消费点共用
  std::vector<std::string> effective_dialect_flags(const Toolchain& tc,
      bool experimental, std::vector<std::string> manifestDialectFlags);
}
```

- [ ] **Step 1: 写失败单测** `tests/unit/test_cppfly.cpp`(gtest 风格仿 `test_toolchain_dialect.cpp`;`make_tc(id, version, stdlibId)` 本地 helper):

```cpp
#include <gtest/gtest.h>
import std;
import mcpp.toolchain.cppfly;
import mcpp.toolchain.model;
using namespace mcpp::toolchain;

namespace {
Toolchain tc_of(CompilerId id, std::string ver, std::string stdlib = "libstdc++") {
    Toolchain tc; tc.compiler = id; tc.version = std::move(ver);
    tc.stdlibId = std::move(stdlib); return tc;
}
bool has_flag(const std::vector<std::string>& v, std::string_view f) {
    return std::find(v.begin(), v.end(), f) != v.end();
}
const cppfly::FeatureState* feature(const cppfly::Resolution& r, std::string_view n) {
    for (auto& f : r.features) if (f.name == n) return &f;
    return nullptr;
}
} // namespace

TEST(CppFly, CompilerMajor) {
    EXPECT_EQ(cppfly::compiler_major(tc_of(CompilerId::GCC, "16.1.0")), 16);
    EXPECT_EQ(cppfly::compiler_major(tc_of(CompilerId::Clang, "22.1.8")), 22);
    EXPECT_EQ(cppfly::compiler_major(tc_of(CompilerId::GCC, "")), 0);
}

TEST(CppFly, LatestStdCanonical) {
    int lvl = 0;
    EXPECT_EQ(cppfly::latest_std_canonical(tc_of(CompilerId::GCC, "16.1.0"), &lvl), "c++26");
    EXPECT_EQ(lvl, 26);
    EXPECT_EQ(cppfly::latest_std_canonical(tc_of(CompilerId::GCC, "13.2.0")), "c++23");
    EXPECT_EQ(cppfly::latest_std_canonical(tc_of(CompilerId::Clang, "22.1.8")), "c++2c");
    EXPECT_EQ(cppfly::latest_std_canonical(tc_of(CompilerId::MSVC, "19.44")), "c++26");
}

TEST(CppFly, ResolveGcc16) {
    auto r = cppfly::resolve(tc_of(CompilerId::GCC, "16.1.0"));
    EXPECT_EQ(r.stdCanonical, "c++26");
    EXPECT_TRUE(has_flag(r.flags, "-freflection"));
    auto* refl = feature(r, "reflection");
    ASSERT_NE(refl, nullptr); EXPECT_TRUE(refl->enabled); EXPECT_EQ(refl->paper, "P2996");
    auto* con = feature(r, "contracts");
    ASSERT_NE(con, nullptr); EXPECT_TRUE(con->enabled); EXPECT_TRUE(con->flags.empty());
    EXPECT_FALSE(has_flag(r.flags, "-fexperimental-library"));  // libstdc++ 无门
}

TEST(CppFly, ResolveGcc15SkipsByVersion) {
    auto r = cppfly::resolve(tc_of(CompilerId::GCC, "15.1.0"));
    EXPECT_TRUE(r.flags.empty());
    auto* refl = feature(r, "reflection");
    ASSERT_NE(refl, nullptr); EXPECT_FALSE(refl->enabled);
    EXPECT_NE(refl->reason.find("16"), std::string::npos);  // 提示最低版本
}

TEST(CppFly, ResolveClangLibcxx) {
    auto r = cppfly::resolve(tc_of(CompilerId::Clang, "22.1.8", "libc++"));
    EXPECT_EQ(r.stdCanonical, "c++2c");
    EXPECT_TRUE(has_flag(r.flags, "-fexperimental-library"));
    auto* refl = feature(r, "reflection");
    ASSERT_NE(refl, nullptr); EXPECT_FALSE(refl->enabled);
    auto* lib = feature(r, "experimental-library");
    ASSERT_NE(lib, nullptr); EXPECT_TRUE(lib->enabled);
}

TEST(CppFly, ResolveMsvcAllSkipped) {
    auto r = cppfly::resolve(tc_of(CompilerId::MSVC, "19.44", "msvc-stl"));
    EXPECT_TRUE(r.flags.empty());
    for (auto& f : r.features) EXPECT_FALSE(f.enabled);
}

TEST(CppFly, StdFlagResolvesLatestAndFly) {
    auto gcc16 = tc_of(CompilerId::GCC, "16.1.0");
    EXPECT_EQ(cppfly::std_flag(gcc16, "c++fly", 1000), "-std=c++26");
    EXPECT_EQ(cppfly::std_flag(gcc16, "c++latest", 999), "-std=c++26");   // Q5 修复
    EXPECT_EQ(cppfly::std_flag(gcc16, "c++23", 23), "-std=c++23");        // 原样
    EXPECT_EQ(cppfly::std_flag(tc_of(CompilerId::Clang, "22.1.8"), "c++fly", 1000), "-std=c++2c");
    EXPECT_EQ(cppfly::std_flag(tc_of(CompilerId::MSVC, "19.44"), "c++fly", 1000), "/std:c++latest");
}

TEST(CppFly, EffectiveDialectFlagsDedup) {
    auto gcc16 = tc_of(CompilerId::GCC, "16.1.0");
    auto out = cppfly::effective_dialect_flags(gcc16, true, {"-freflection", "-fchar8_t"});
    EXPECT_EQ(std::count(out.begin(), out.end(), std::string("-freflection")), 1);
    EXPECT_TRUE(has_flag(out, "-fchar8_t"));
    auto off = cppfly::effective_dialect_flags(gcc16, false, {"-fchar8_t"});
    EXPECT_EQ(off, (std::vector<std::string>{"-fchar8_t"}));
}
```

- [ ] **Step 2: 跑单测确认失败**(模块不存在 → 编译错)
  Run: `"$MCPP_FRESH" test 2>&1 | tail -20` — Expected: FAIL(`mcpp.toolchain.cppfly` 未找到)

- [ ] **Step 3: 实现 `src/toolchain/cppfly.cppm`**:

```cpp
// mcpp.toolchain.cppfly — the `standard = "c++fly"` capability: per-compiler
// support & mapping for "latest -std= level + every enableable experimental
// gate (language + stdlib)". Pure data tables + query functions, the fourth
// sibling of CommandDialect / BmiTraits / ProviderCapabilities — and the
// first consumer of Toolchain::version for gating.
//
// Also owns the "latest level for this toolchain" table that fixes
// standard = "c++latest" mis-spelling -std=c++latest on the GNU family.
//
// Table facts pinned by on-machine probes (gcc 16.1.0, 2026-07-14):
// contracts are enabled by -std=c++26 alone (__cpp_contracts defined);
// reflection additionally needs -freflection (__cpp_impl_reflection).
// See .agents/docs/2026-07-14-std-features-experimental-gate-design.md §2/§11-Q3.

export module mcpp.toolchain.cppfly;

import std;
import mcpp.toolchain.dialect;
import mcpp.toolchain.model;

export namespace mcpp::toolchain::cppfly {

struct FeatureState {
    std::string name;      // "reflection"
    std::string paper;     // "P2996" — summary/diagnostics
    std::string flags;     // enabled: extra flags ("" = enabled by std level alone)
    std::string reason;    // skipped: why
    bool enabled = false;
};

struct Resolution {
    std::string stdCanonical;               // "c++26" / "c++2c" / ...
    int stdLevel = 26;
    std::vector<std::string> flags;         // union of enabled gate flags, deduped
    std::vector<FeatureState> features;     // enabled + skipped, declaration order
};

int compiler_major(const Toolchain& tc);
std::string latest_std_canonical(const Toolchain& tc, int* levelOut = nullptr);
Resolution resolve(const Toolchain& tc);
std::string std_flag(const Toolchain& tc, std::string_view canonical, int level);
std::vector<std::string> effective_dialect_flags(const Toolchain& tc,
    bool experimental, std::vector<std::string> manifestDialectFlags);

} // namespace mcpp::toolchain::cppfly

namespace mcpp::toolchain::cppfly {

namespace {

// ── Table 1: family × min major → latest supported -std= canonical ──────
struct LatestStdRule { CompilerId family; int minMajor; std::string_view canonical; int level; };
constexpr LatestStdRule kLatestStd[] = {
    { CompilerId::GCC,   14, "c++26", 26 },
    { CompilerId::GCC,    0, "c++23", 23 },
    { CompilerId::Clang, 17, "c++2c", 26 },
    { CompilerId::Clang,  0, "c++23", 23 },
    { CompilerId::MSVC,   0, "c++26", 26 },   // spelled /std:c++latest by std_flag_for
};

// ── Table 2: experimental language-feature gates (version-gated) ────────
struct GateRule { CompilerId family; int minMajor; std::string_view flags; };
struct Gate { std::string_view name, paper; std::span<const GateRule> rules; };
constexpr GateRule kReflectionRules[] = { { CompilerId::GCC, 16, "-freflection" } };
constexpr GateRule kContractsRules[]  = { { CompilerId::GCC, 16, "" } };  // on by -std=c++26 (probe-pinned)
constexpr Gate kGates[] = {
    { "reflection", "P2996", kReflectionRules },
    { "contracts",  "P2900", kContractsRules  },
};

// ── Table 3: stdlib experimental gates (stdlib dimension) ───────────────
struct StdlibGateRule { std::string_view name, stdlibId, flags; };
constexpr StdlibGateRule kStdlibGates[] = {
    { "experimental-library", "libc++", "-fexperimental-library" },
};

void add_unique(std::vector<std::string>& v, std::string_view f) {
    if (f.empty()) return;
    if (std::find(v.begin(), v.end(), f) == v.end()) v.emplace_back(f);
}

} // namespace

int compiler_major(const Toolchain& tc) {
    int major = 0;
    for (char c : tc.version) {
        if (c < '0' || c > '9') break;
        major = major * 10 + (c - '0');
    }
    return major;
}

std::string latest_std_canonical(const Toolchain& tc, int* levelOut) {
    const int major = compiler_major(tc);
    for (auto& r : kLatestStd) {
        if (r.family != tc.compiler) continue;
        if (major >= r.minMajor) {
            if (levelOut) *levelOut = r.level;
            return std::string(r.canonical);
        }
    }
    if (levelOut) *levelOut = 26;   // Unknown family: newest ratified level
    return "c++26";
}

Resolution resolve(const Toolchain& tc) {
    Resolution r;
    r.stdCanonical = latest_std_canonical(tc, &r.stdLevel);
    const int major = compiler_major(tc);
    for (auto& g : kGates) {
        FeatureState st{ std::string(g.name), std::string(g.paper), {}, {}, false };
        const GateRule* hit = nullptr;
        for (auto& rule : g.rules)
            if (rule.family == tc.compiler) { hit = &rule; break; }
        if (!hit) {
            st.reason = std::format("{}: unsupported", tc.compiler_name());
        } else if (major < hit->minMajor) {
            st.reason = std::format("{} {} < {}", tc.compiler_name(), major, hit->minMajor);
        } else {
            st.enabled = true;
            st.flags = std::string(hit->flags);
            add_unique(r.flags, hit->flags);
        }
        r.features.push_back(std::move(st));
    }
    for (auto& sg : kStdlibGates) {
        FeatureState st{ std::string(sg.name), "stdlib", {}, {}, false };
        if (tc.stdlibId == sg.stdlibId) {
            st.enabled = true;
            st.flags = std::string(sg.flags);
            add_unique(r.flags, sg.flags);
        } else {
            st.reason = std::format("stdlib is {}, gate applies to {}",
                tc.stdlibId.empty() ? "unknown" : tc.stdlibId, sg.stdlibId);
        }
        r.features.push_back(std::move(st));
    }
    return r;
}

std::string std_flag(const Toolchain& tc, std::string_view canonical, int level) {
    std::string resolvedCanonical(canonical);
    int resolvedLevel = level;
    // c++latest (999) / c++fly (1000): resolve to the family's real latest
    // level — the raw canonical is not a valid -std= spelling on GNU.
    if (level >= 999) resolvedCanonical = latest_std_canonical(tc, &resolvedLevel);
    return std_flag_for(dialect_for(tc), resolvedCanonical, resolvedLevel);
}

std::vector<std::string> effective_dialect_flags(const Toolchain& tc,
    bool experimental, std::vector<std::string> manifestDialectFlags)
{
    if (!experimental) return manifestDialectFlags;
    for (auto& f : resolve(tc).flags) add_unique(manifestDialectFlags, f);
    return manifestDialectFlags;
}

} // namespace mcpp::toolchain::cppfly
```

- [ ] **Step 4: 跑单测确认通过** — Run: `"$MCPP_FRESH" test`(至少 CppFly.* 全 PASS)
- [ ] **Step 5: Commit** — `git add src/toolchain/cppfly.cppm tests/unit/test_cppfly.cpp && git commit -m "feat(toolchain): cppfly registry — latest-std + experimental gates per compiler"`

---

### Task 2: manifest `c++fly` 值 + 消费点接线(plan/prepare/fingerprint)+ summary

**Files:**
- Modify: `src/manifest/types.cppm:27-32`(CppStandardConfig)、`types.cppm:528-539`(normalize)
- Modify: `src/build/plan.cppm:322-331`(std flag + dialect flags)
- Modify: `src/build/prepare.cppm:2586-2596`(stdFlagAndDialect)、`prepare.cppm:2642-2649`(fingerprint)
- Test: `tests/unit/test_manifest.cpp`(normalize 断言;若 normalize 测试实际在 test_toml.cpp 则加在彼处,先 grep `normalize_cpp_standard` 定位)

**Interfaces:**
- Consumes: Task 1 的 `cppfly::std_flag` / `cppfly::effective_dialect_flags` / `cppfly::resolve`。
- Produces: `CppStandardConfig::experimental`(bool,`normalize_cpp_standard("c++fly")` 置 true,level=1000)。

- [ ] **Step 1: 写失败单测**(定位既有 normalize 测试后追加):

```cpp
TEST(NormalizeCppStandard, CppFly) {
    auto cfg = mcpp::manifest::normalize_cpp_standard("c++fly");
    ASSERT_TRUE(cfg.has_value());
    EXPECT_EQ(cfg->canonical, "c++fly");
    EXPECT_EQ(cfg->level, 1000);
    EXPECT_TRUE(cfg->experimental);
    EXPECT_FALSE(mcpp::manifest::normalize_cpp_standard("c++23")->experimental);
    auto bad = mcpp::manifest::normalize_cpp_standard("c++flyy");
    ASSERT_FALSE(bad.has_value());
    EXPECT_NE(bad.error().find("c++fly"), std::string::npos);  // 白名单消息含新拼写
}
```

- [ ] **Step 2: 跑单测确认失败**(experimental 字段不存在 → 编译错)
- [ ] **Step 3: 实现**——
  - `types.cppm` CppStandardConfig 加 `bool experimental = false;`;normalize 在 `c++latest` 分支后加:
```cpp
    if (s == "c++fly") {
        out.canonical = "c++fly";
        out.flag = "-std=c++26";      // GNU 静态兜底;真实拼写走 cppfly::std_flag
        out.level = 1000;             // > c++latest(999):latest 之上再加实验门
        out.gnuDialect = false;
        out.experimental = true;
        return out;
    }
```
    错误消息补 `c++fly`("expected c++23, c++26, c++2c, gnu++23, gnu++26, c++latest, or c++fly")。
  - `plan.cppm:322-331` 改为(import mcpp.toolchain.cppfly;顶部 import 区补):
```cpp
    bool experimentalStd = false;
    if (auto stdCfg = mcpp::manifest::normalize_cpp_standard(manifest.package.standard)) {
        plan.cppStandard = stdCfg->canonical;
        experimentalStd  = stdCfg->experimental;
        // Spelled per-dialect AND per-toolchain-latest for c++latest/c++fly.
        plan.cppStandardFlag = mcpp::toolchain::cppfly::std_flag(
            tc, stdCfg->canonical, stdCfg->level);
    }
    for (auto& f : mcpp::toolchain::cppfly::effective_dialect_flags(
             tc, experimentalStd, mcpp::manifest::dialect_flags(manifest.buildConfig))) {
        plan.dialectFlags += ' ';
        plan.dialectFlags += f;
    }
```
  - `prepare.cppm:2586-2596` 同型改写(同一对函数 → 两点同源),并紧随其后加 summary(仅 experimental 时,一次):
```cpp
    std::string stdFlagAndDialect = mcpp::toolchain::cppfly::std_flag(
        *tc, m->cppStandard.canonical, m->cppStandard.level);
    for (auto& f : mcpp::toolchain::cppfly::effective_dialect_flags(
             *tc, m->cppStandard.experimental,
             mcpp::manifest::dialect_flags(m->buildConfig))) {
        stdFlagAndDialect += ' ';
        stdFlagAndDialect += f;
    }
    if (m->cppStandard.experimental) {
        auto fly = mcpp::toolchain::cppfly::resolve(*tc);
        std::string enabled, skipped;
        for (auto& f : fly.features) {
            auto& dst = f.enabled ? enabled : skipped;
            if (!dst.empty()) dst += ", ";
            dst += f.name;
            if (f.enabled && !f.flags.empty()) dst += std::format(" ({})", f.flags);
            if (!f.enabled) dst += std::format(" ({})", f.reason);
        }
        std::println("c++fly on {}: -std flag `{}`; enabled: {}; skipped: {}",
                     tc->label(), plan_std_flag_placeholder /* 用 stdFlagAndDialect 首段 */,
                     enabled.empty() ? "(none)" : enabled,
                     skipped.empty() ? "(none)" : skipped);
    }
```
    (实现时用局部变量存 `cppfly::std_flag(...)` 结果避免占位写法;summary 打到 stdout,e2e grep。)
  - fingerprint 卫生(`prepare.cppm:2645` 附近):
```cpp
    fpi.compileFlags        = canonical_compile_flags(*m)
                              + canonical_package_build_metadata(packages);
    if (m->cppStandard.experimental)
        for (auto& f : mcpp::toolchain::cppfly::resolve(*tc).flags)
            { fpi.compileFlags += ' '; fpi.compileFlags += f; }
```
- [ ] **Step 4: 全量单测通过** — Run: `"$MCPP_FRESH" test`
- [ ] **Step 5: 冒烟**——临时目录 `mcpp new flydemo && cd flydemo`,manifest 改 `standard = "c++fly"`,`mcpp build`(gcc16)成功且输出 summary 行;`standard = "c++latest"` 也构建成功(Q5 修复实证)。
- [ ] **Step 6: Commit** — `git commit -m "feat(manifest,build): standard=c++fly — latest std + all experimental gates; fix c++latest GNU spelling"`

---

### Task 3: e2e ×3

**Files:**
- Create: `tests/e2e/100_cppfly_reflection.sh`(gcc16 硬路径;开头 `# requires: gcc`,复用 98 的 payload 探测)
- Create: `tests/e2e/101_cppfly_llvm_soft.sh`(llvm 软路径;探测 llvm payload,无则 SKIP-INLINE)
- Modify: 确认 `tests/e2e/run_all.sh` 的编号 glob 覆盖三位数(不覆盖则改 glob)

- [ ] **Step 1: `100_cppfly_reflection.sh`** — 工程仅 `standard = "c++fly"`(无任何手写旗标),src/main.cpp 用 98 的反射示例(std::meta 遍历 Point 成员);断言:`mcpp run` 输出 `x 2`/`y 3`;stdout 含 `c++fly on` 与 `reflection (-freflection)`;`std-module.json` 的 std_flag 含 `-std=c++26` 且含 `-freflection`。
- [ ] **Step 2: `101_cppfly_llvm_soft.sh`** — 探测 `${MCPP_HOME}/registry/data/xpkgs/xim-x-llvm*` 有 clang++ payload,无则 SKIP;工程 `standard = "c++fly"` + 平凡 `import std;` main;断言:构建成功;stdout 含 `skipped: reflection`;compile_commands.json 含 `-std=c++2c`;stdlib 为 libc++ 时含 `-fexperimental-library`。
- [ ] **Step 3: c++latest 回归**——并入 100 号脚本尾部:同工程改 `standard = "c++latest"`,`mcpp build` 成功且 compile_commands.json 含 `-std=c++26`、不含 `-std=c++latest`。
- [ ] **Step 4: 本地跑三条**:`MCPP=<fresh> bash tests/e2e/100_cppfly_reflection.sh` 等,全 PASS(llvm 无 payload 则 SKIP 视为通过)。
- [ ] **Step 5: Commit** — `git commit -m "test(e2e): c++fly hard/soft paths + c++latest spelling regression"`

---

### Task 4: 版本号 + CHANGELOG + README

- [ ] `mcpp.toml:3` → `version = "0.0.91"`;`src/toolchain/fingerprint.cppm:21` → `MCPP_VERSION = "0.0.91"`。
- [ ] `CHANGELOG.md` 顶部加 `## [0.0.91] — 2026-07-14`:新增 `standard = "c++fly"`(语义三件套 + summary + 软跳过);修复 `c++latest` GNU 拼写;设计文档链接。
- [ ] README(两语言)manifest/standard 说明处补一行 `c++fly`(grep `c++latest` 定位;没有则跳过 README,CHANGELOG 已覆盖)。
- [ ] Commit — `git commit -m "chore(release): 0.0.91 — changelog + version bump"`

---

### Task 5: 本地全量验证 → PR → CI → squash 合入

- [ ] 分支 `feat/cppfly-091`;全量:`mcpp build && "$MCPP_FRESH" test && MCPP=$MCPP_FRESH tests/e2e/run_all.sh`(基线对照:环境性失败清单见记忆 2026-07-07,新失败为回归)。
- [ ] `gh pr create` 标题 `feat: standard=c++fly — one-knob latest std + experimental features (#design 2026-07-14) — 0.0.91`;正文含设计文档路径、实机探测结论、验收清单勾选。
- [ ] 等 CI 全绿(ci-linux + ci-linux-e2e + ci-macos + ci-windows);失败按 systematic-debugging 修。
- [ ] `gh pr merge --squash`(仓库惯例 bypass 分支保护:`--admin`)。

### Task 6: release 0.0.91 + xlings 生态闭环 + 真实验证

按记忆 `release-publish-pipeline` 执行(此阶段开始前重读该记忆文件):
- [ ] main 上触发 release(tag v0.0.91 / release.yml),产出各平台资产。
- [ ] 镜像资产到 xlings-res(gh + gtc 双端;**绝不删既有资产**,`--clobber` 只对非 serving 资产)。
- [ ] 更新 xim-pkgindex 的 mcpp 0.0.91 条目(gitee 自动镜像 github)。
- [ ] `xlings update` + `xlings install mcpp -y` 装出 0.0.91,`mcpp --version` 验证。
- [ ] bump 本仓 CI bootstrap pin(仿 `ce34a70^` 的 `ci: workspace mcpp bootstrap pin -> 0.0.89` 提交)。
- [ ] 真实端到端:xlings 装的 0.0.91 新建工程,`standard = "c++fly"`,gcc16 真实构建运行反射示例,记录输出作为汇报证据。

## Self-Review 备忘

类型一致性:`cppfly::std_flag(tc, canonical, level)` 三处调用(plan/prepare×2)签名一致;`effective_dialect_flags` 返回 vector<string> 两处消费者各自 join。Spec 覆盖:设计 §5.1-5.6/§9 验收 1-5 全部有任务对应(验收 2 的 llvm 硬错→软跳过 summary 即 e2e 101)。
