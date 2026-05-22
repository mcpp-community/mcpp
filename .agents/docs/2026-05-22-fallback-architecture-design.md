# mcpp Fallback 架构设计方案

**Date**: 2026-05-22
**Status**: Proposed

## 一、现状问题

mcpp 代码中有 30+ 个 fallback/workaround 模式，分散在 cli.cppm、package_fetcher.cppm、
probe.cppm、compat.cppm、config.cppm、xlings.cppm 等十几个文件中。存在以下问题：

1. **分散不可见**：fallback 逻辑嵌入业务代码，不经过深度阅读无法发现
2. **优先级混乱**：如 copy workaround 短路了 install，导致 install 路径几乎不被执行
3. **缺乏文档**：只有行内注释，没有全局视图
4. **难以审计**：不知道哪些 fallback 是临时的（计划移除）vs 永久的（架构需要）
5. **测试盲区**：fallback 路径很少被测试到

## 二、Fallback 分类

### 按生命周期

| 类型 | 含义 | 处理策略 |
|------|------|---------|
| **Permanent** | 架构上永远需要（如多平台适配） | 明确文档化，保持维护 |
| **Compat** | 向后兼容（计划在 1.0 移除） | 标注移除版本，追踪移除进度 |
| **Workaround** | 绕过外部 bug（如 xlings XLINGS_HOME） | 标注上游 issue，定期检查是否可移除 |

### 按功能域

| 域 | 数量 | 示例 |
|----|------|------|
| 包获取与安装 | 8 | copy workaround, install fallback, xpkg 路径查找 |
| 工具链探测 | 5 | 编译器查找、sysroot 探测、std 模块源码 |
| 二进制获取 | 3 | xlings bundled/system/vendored |
| 构建系统 | 2 | ninja 增量失败重建、dyndep 模式 |
| 依赖解析 | 3 | SemVer 合并、多版本 mangle |
| 向后兼容 | 6 | config 迁移、dotted key、legacy API |
| Sysroot 补全 | 2 | kernel headers/glibc symlink |

## 三、设计方案：src/fallback/ 统一管理

### 3.1 目录结构

```
src/fallback/
├── README.md                    ← 全局 fallback 索引和约定
├── registry.cppm                ← fallback 注册表模块
├── xpkg_resolve.cppm            ← 包获取 fallback 链
├── xlings_binary.cppm           ← xlings 二进制获取 fallback 链
├── toolchain_probe.cppm         ← 工具链探测 fallback 链
└── compat.cppm                  ← 向后兼容 fallback（已有 pm/compat.cppm 迁入）
```

### 3.2 核心模块：fallback::registry

`src/fallback/registry.cppm` — fallback 元数据注册表

```cpp
export module mcpp.fallback.registry;
import std;

export namespace mcpp::fallback {

enum class Lifecycle { permanent, compat, workaround };

struct Entry {
    std::string_view id;           // "xpkg.copy_from_global"
    std::string_view domain;       // "package"
    std::string_view description;  // 一句话描述
    Lifecycle        lifecycle;
    std::string_view removeBy;     // "1.0" or "" (permanent)
    std::string_view upstreamIssue; // "xlings#123" or ""
};

// 编译期注册所有 fallback（constexpr 数组）
constexpr std::array kEntries = {

    // ─── 包获取与安装 ───────────────────────────────────────
    Entry{
        .id          = "xpkg.copy_from_global",
        .domain      = "package",
        .description = "copy xpkg from ~/.xlings/ when sandbox install fails",
        .lifecycle   = Lifecycle::workaround,
        .removeBy    = "",
        .upstreamIssue = "xlings XLINGS_HOME propagation",
    },
    Entry{
        .id          = "xpkg.install_direct_before_ndjson",
        .domain      = "package",
        .description = "try direct xlings install before NDJSON interface",
        .lifecycle   = Lifecycle::workaround,
        .removeBy    = "",
        .upstreamIssue = "xlings NDJSON large package bug",
    },

    // ─── xlings 二进制获取 ──────────────────────────────────
    Entry{
        .id          = "xlings_binary.vendored_env",
        .domain      = "config",
        .description = "MCPP_VENDORED_XLINGS env override for Windows",
        .lifecycle   = Lifecycle::workaround,
        .removeBy    = "",
        .upstreamIssue = "Windows xlings runtime missing after copy",
    },
    Entry{
        .id          = "xlings_binary.system_which",
        .domain      = "config",
        .description = "find xlings in PATH when bundled unavailable",
        .lifecycle   = Lifecycle::permanent,
    },

    // ─── 工具链探测 ─────────────────────────────────────────
    Entry{
        .id          = "probe.sysroot_compiler",
        .domain      = "toolchain",
        .description = "gcc -print-sysroot",
        .lifecycle   = Lifecycle::permanent,
    },
    Entry{
        .id          = "probe.sysroot_cfg",
        .domain      = "toolchain",
        .description = "parse clang++.cfg for --sysroot",
        .lifecycle   = Lifecycle::permanent,
    },
    Entry{
        .id          = "probe.sysroot_xcrun",
        .domain      = "toolchain",
        .description = "macOS xcrun --show-sdk-path",
        .lifecycle   = Lifecycle::permanent,
    },
    Entry{
        .id          = "probe.sysroot_xlings_remap",
        .domain      = "toolchain",
        .description = "remap xlings build-time sysroot to registry path",
        .lifecycle   = Lifecycle::workaround,
        .upstreamIssue = "xlings bakes build-host path into gcc",
    },

    // ─── 向后兼容 ───────────────────────────────────────────
    Entry{
        .id          = "compat.dotted_package_name",
        .domain      = "manifest",
        .description = "split 'ns.name' legacy dotted form",
        .lifecycle   = Lifecycle::compat,
        .removeBy    = "1.0",
    },
    Entry{
        .id          = "compat.xpkg_lua_candidates",
        .domain      = "package",
        .description = "multi-candidate xpkg .lua file lookup",
        .lifecycle   = Lifecycle::compat,
        .removeBy    = "1.0",
    },
    Entry{
        .id          = "compat.install_dir_scan",
        .domain      = "package",
        .description = "last-resort scan xpkgs/ for matching dir",
        .lifecycle   = Lifecycle::compat,
        .removeBy    = "1.0",
    },
    Entry{
        .id          = "compat.config_index_migration",
        .domain      = "config",
        .description = "rename mcpp-index to mcpplibs in config files",
        .lifecycle   = Lifecycle::compat,
        .removeBy    = "1.0",
    },

    // ─── 构建系统 ───────────────────────────────────────────
    Entry{
        .id          = "build.ninja_incremental_retry",
        .domain      = "build",
        .description = "ninja incremental fail → full rebuild",
        .lifecycle   = Lifecycle::permanent,
    },
    Entry{
        .id          = "build.dyndep_opt_out",
        .domain      = "build",
        .description = "MCPP_NINJA_DYNDEP=0 disables P1689 scanning",
        .lifecycle   = Lifecycle::permanent,
    },

    // ─── 依赖解析 ───────────────────────────────────────────
    Entry{
        .id          = "deps.multi_version_mangle",
        .domain      = "dependency",
        .description = "cross-major version coexistence via name mangling",
        .lifecycle   = Lifecycle::permanent,
    },

    // ─── Sysroot 补全 ───────────────────────────────────────
    Entry{
        .id          = "sysroot.symlink_kernel_headers",
        .domain      = "toolchain",
        .description = "symlink linux-headers into sysroot if missing",
        .lifecycle   = Lifecycle::workaround,
        .upstreamIssue = "xlings sysroot may lack kernel headers",
    },
    Entry{
        .id          = "sysroot.symlink_glibc_headers",
        .domain      = "toolchain",
        .description = "symlink glibc headers into sysroot if missing",
        .lifecycle   = Lifecycle::workaround,
        .upstreamIssue = "xlings sysroot may lack glibc headers",
    },
};

// 查询接口
constexpr const Entry* find(std::string_view id) {
    for (auto& e : kEntries)
        if (e.id == id) return &e;
    return nullptr;
}

// 列出某个域的所有 fallback
void list_by_domain(std::string_view domain);

// 列出所有 workaround（用于定期审计）
void list_workarounds();

// 列出所有 compat（用于 1.0 清理）
void list_compat();

} // namespace mcpp::fallback
```

### 3.3 Fallback 链模块：xpkg_resolve

`src/fallback/xpkg_resolve.cppm` — 包获取的 fallback 链

当前 `resolve_xpkg_path()` 的逻辑（copy 优先）重构为明确的链式结构：

```cpp
export module mcpp.fallback.xpkg_resolve;
import std;

export namespace mcpp::fallback::xpkg {

enum class Strategy {
    sandbox_exists,       // sandbox 里已有
    install_direct,       // xlings install (直接调用模式)
    install_ndjson,       // xlings interface install_packages
    copy_from_global,     // 从 ~/.xlings/ 拷贝
};

// 每个 strategy 的结果
struct StepResult {
    Strategy    strategy;
    bool        success;
    std::string detail;   // 成功：路径；失败：原因
};

// 执行 fallback 链，返回成功的 strategy 和路径
// 失败时返回所有尝试过的 step 和原因
struct ChainResult {
    bool success;
    std::filesystem::path resolvedPath;
    std::vector<StepResult> steps;  // 审计用：记录每一步的尝试
};

// 理想的执行顺序（install 优先于 copy）：
//
// 1. sandbox_exists   → 已有则直接用
// 2. install_direct   → 用 XLINGS_HOME=sandbox 直接安装
// 3. install_ndjson   → NDJSON interface fallback（提供进度条）
// 4. copy_from_global → 从 ~/.xlings/ 拷贝（最后兜底）
//
// 每一步成功则停止，失败则继续下一步。
// 所有步骤都记录到 steps 供日志/审计使用。

} // namespace
```

### 3.4 使用方式：调用方引用

原来分散的 fallback 逻辑保留在原位（不大规模移动代码），但通过以下方式关联到 registry：

```cpp
// src/pm/package_fetcher.cppm
#include "fallback/registry.cppm"  // conceptual

// 在 fallback 发生时记录
if (!std::filesystem::exists(verdir)) {
    mcpp::log::verbose("fetcher",
        std::format("[fallback:{}] {}",
            "xpkg.copy_from_global",
            mcpp::fallback::find("xpkg.copy_from_global")->description));
    // ... copy logic
}
```

### 3.5 CLI 命令：mcpp self fallbacks

提供 `mcpp self fallbacks` 命令，列出所有已注册的 fallback：

```
$ mcpp self fallbacks
Fallback Registry (18 entries)

WORKAROUNDS (need upstream fix):
  xpkg.copy_from_global          copy xpkg from ~/.xlings/ when sandbox install fails
  xpkg.install_direct_before_ndjson  try direct xlings install before NDJSON interface
  xlings_binary.vendored_env     MCPP_VENDORED_XLINGS env override for Windows
  probe.sysroot_xlings_remap     remap xlings build-time sysroot to registry path
  sysroot.symlink_kernel_headers symlink linux-headers into sysroot if missing
  sysroot.symlink_glibc_headers  symlink glibc headers into sysroot if missing

COMPAT (remove by 1.0):
  compat.dotted_package_name     split 'ns.name' legacy dotted form
  compat.xpkg_lua_candidates     multi-candidate xpkg .lua file lookup
  compat.install_dir_scan        last-resort scan xpkgs/ for matching dir
  compat.config_index_migration  rename mcpp-index to mcpplibs in config files

PERMANENT:
  xlings_binary.system_which     find xlings in PATH when bundled unavailable
  probe.sysroot_compiler         gcc -print-sysroot
  probe.sysroot_cfg              parse clang++.cfg for --sysroot
  probe.sysroot_xcrun            macOS xcrun --show-sdk-path
  build.ninja_incremental_retry  ninja incremental fail → full rebuild
  build.dyndep_opt_out           MCPP_NINJA_DYNDEP=0 disables P1689 scanning
  deps.multi_version_mangle      cross-major version coexistence via name mangling
```

## 四、resolve_xpkg_path 优先级修复

### 4.1 当前流程（copy 优先，有问题）

```
resolve()
├── sandbox 有？→ 返回
├── ~/.xlings/ 有？→ copy → 返回     ← copy 在 install 前面
└── 返回 error

install()（只有 resolve 失败才走到）

resolve()（第二次，install 后）
├── sandbox 有？→ 返回
├── ~/.xlings/ 有？→ copy → 返回
└── 返回 error
```

### 4.2 修复后流程（install 优先）

```
resolve_quick()                      ← 只检查 sandbox，不做 copy
├── sandbox 有？→ 返回
└── 返回 null

install()                            ← install 优先
├── install_with_progress (直接调用模式)
└── 成功 → resolve_quick() 再检查 sandbox

resolve_with_fallback()              ← copy 是最后兜底
├── sandbox 有？→ 返回
├── ~/.xlings/ 有？→ copy + post-copy fixup → 返回
└── 返回 error
```

### 4.3 具体代码改动

```cpp
auto resolve_quick = [&]() -> std::optional<XpkgPayload> {
    if (!std::filesystem::exists(verdir)) return std::nullopt;
    // ... 构造 payload
    return payload;
};

auto resolve_with_copy_fallback = [&]() -> std::expected<XpkgPayload, CallError> {
    if (auto p = resolve_quick()) return *p;

    // FALLBACK: copy from global xlings
    mcpp::log::verbose("fetcher", "[fallback:xpkg.copy_from_global]");
    // ... copy logic (existing code)

    if (auto p = resolve_quick()) return *p;
    return std::unexpected(CallError{"xpkg payload missing: " + verdir.string()});
};

// Main flow:
// 1. Quick check
if (auto p = resolve_quick()) return *p;

// 2. Install (if auto)
if (autoInstall) {
    mcpp::log::verbose("fetcher", "triggering xlings install");
    auto inst = install(targets, handler);
    if (inst && inst->exitCode == 0) {
        if (auto p = resolve_quick()) return *p;
    }
}

// 3. Fallback: copy from global
return resolve_with_copy_fallback();
```

## 五、实施计划

### Phase 1：文档化（不改代码逻辑）
1. 创建 `src/fallback/registry.cppm`，编译期注册所有 fallback
2. 创建 `src/fallback/README.md`，fallback 约定和索引
3. 在现有 fallback 位置添加 `[fallback:xxx]` 日志标记

### Phase 2：resolve_xpkg_path 优先级修复
1. 重构为 `resolve_quick()` + `resolve_with_copy_fallback()` 两层
2. install 移到 copy 前面
3. install 使用 `install_with_progress()`（直接调用模式）

### Phase 3：mcpp self fallbacks 命令
1. 实现 CLI 子命令
2. 输出按 lifecycle 分类的 fallback 列表
3. 支持 `--workarounds` / `--compat` 过滤

### Phase 4：1.0 清理
1. 移除所有 `lifecycle = compat, removeBy = "1.0"` 的 fallback
2. 简化 xpkg 路径查找（只保留 canonical 路径）
3. 评估 workaround 是否仍需要（检查上游修复状态）

## 六、不做的事

- **不大规模移动代码**：fallback 逻辑保留在原位，只通过 registry 关联
- **不引入运行时开销**：registry 是 constexpr，零开销
- **不在 fallback 间做自动切换**：每个 fallback 的触发条件由业务代码控制
- **不做 fallback 的动态注册**：编译期确定，避免复杂度
