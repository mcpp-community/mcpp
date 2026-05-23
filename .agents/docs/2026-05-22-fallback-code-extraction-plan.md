# Fallback 代码提取方案 — 代码架构重构

**Date**: 2026-05-22
**Status**: Proposed

## 一、目标

把分散在 config.cppm、package_fetcher.cppm、probe.cppm 等文件中的 fallback **实现代码**
提取到 `src/fallback/` 下统一管理。调用方只 import 引用，保持简洁。

## 二、目标目录结构

```
src/fallback/
├── xpkg_copy.cppm           ← 从 ~/.xlings/ 拷贝 xpkg 到 sandbox
├── xlings_binary.cppm       ← xlings 二进制获取链（bundled/system/vendored）
├── config_migration.cppm    ← config.toml / .xlings.json 索引名迁移
├── sysroot_complete.cppm    ← sysroot 缺失头文件时 symlink 补全
└── legacy_dirs.cppm         ← 遗留 xpkg 目录扫描（1.0 移除）
```

## 三、逐个提取方案

### 3.1 `src/fallback/xpkg_copy.cppm`

**来源**：`package_fetcher.cppm:643-677`（copy_from_global lambda）

**提取函数**：
```cpp
export module mcpp.fallback.xpkg_copy;
import std;

export namespace mcpp::fallback {

// 从全局 xlings 目录（~/.xlings/data/xpkgs/）拷贝 xpkg 到 mcpp sandbox。
// 返回 true 如果成功拷贝。
bool copy_xpkg_from_global(
    const std::filesystem::path& sandboxVerdir);

} // namespace
```

**依赖**：`std::filesystem`、`mcpp.log`、`std::getenv`
**耦合度**：无（纯文件操作）
**调用方改动**：`package_fetcher.cppm` 的 `copy_from_global` lambda → 调用 `mcpp::fallback::copy_xpkg_from_global(verdir)`

### 3.2 `src/fallback/xlings_binary.cppm`

**来源**：`config.cppm:338-395`（acquire_xlings_binary）

**提取函数**：
```cpp
export module mcpp.fallback.xlings_binary;
import std;

export namespace mcpp::fallback {

struct AcquireResult {
    std::filesystem::path binary;   // 成功时填入
    std::string           error;    // 失败时填入
};

// 按优先级获取 xlings 二进制：
//   1. MCPP_VENDORED_XLINGS 环境变量
//   2. 系统 PATH 中 which xlings
//   3. 返回错误提示
AcquireResult acquire_xlings_binary(
    const std::filesystem::path& destBin,
    std::string_view pinnedVersion);

} // namespace
```

**依赖**：`std::filesystem`、`mcpp.platform`（which, exe_suffix, perms）
**耦合度**：低（只依赖 platform 工具函数）
**调用方改动**：`config.cppm` 的 `acquire_xlings_binary()` → 调用 `mcpp::fallback::acquire_xlings_binary()`

### 3.3 `src/fallback/config_migration.cppm`

**来源**：`config.cppm:268-335`（migrate 函数组）

**提取函数**：
```cpp
export module mcpp.fallback.config_migration;
import std;

export namespace mcpp::fallback {

// 将 config.toml 中 "mcpp-index" 重命名为 "mcpplibs"
void migrate_config_toml_index_names(const std::filesystem::path& configPath);

// 将 .xlings.json 中 "mcpp-index" 重命名为 "mcpplibs"
void migrate_xlings_json_index_names(const std::filesystem::path& xjsonPath);

} // namespace
```

**依赖**：`std::filesystem`、`std::string` 操作
**耦合度**：无（纯文本替换）
**调用方改动**：`config.cppm` 调 `mcpp::fallback::migrate_config_toml_index_names()`

### 3.4 `src/fallback/sysroot_complete.cppm`

**来源**：`probe.cppm:364-398`（ensure_sysroot_complete）

**提取函数**：
```cpp
export module mcpp.fallback.sysroot_complete;
import std;
import mcpp.toolchain.model;  // PayloadPaths

export namespace mcpp::fallback {

// 检查 sysroot 是否缺少头文件，缺则从 payload xpkg 创建 symlink 补全。
void ensure_sysroot_complete(
    const std::filesystem::path& sysroot,
    const mcpp::toolchain::PayloadPaths& pp);

} // namespace
```

**依赖**：`std::filesystem`、`mcpp.toolchain.model`（PayloadPaths）
**耦合度**：低（只依赖 PayloadPaths 数据结构）
**调用方改动**：`probe.cppm` 的 `ensure_sysroot_complete()` 导出 → 改为调用 `mcpp::fallback::ensure_sysroot_complete()`

### 3.5 `src/fallback/legacy_dirs.cppm`

**来源**：`package_fetcher.cppm:751-768`（last-resort dir scan）

**提取函数**：
```cpp
export module mcpp.fallback.legacy_dirs;
import std;

export namespace mcpp::fallback {

// 遍历 xpkgs/ 目录，按遗留命名模式查找匹配的 xpkg 目录。
// 标记 remove by 1.0。
std::optional<std::filesystem::path>
scan_legacy_install_dirs(
    const std::filesystem::path& xpkgsBase,
    std::string_view qualifiedName,
    std::string_view shortName);

} // namespace
```

**依赖**：`std::filesystem`
**耦合度**：低
**调用方改动**：`package_fetcher.cppm` 的内联扫描 → 调用 `mcpp::fallback::scan_legacy_install_dirs()`

## 四、不提取的

| 代码 | 原因 |
|------|------|
| `probe_sysroot()` 3-策略链 | 整个函数就是一个 fallback 链，提取后原函数变空了，不合理 |
| `install_with_progress()` | 属于 xlings 模块的核心功能，不是独立 fallback |
| `find_sibling_*()` | 已在 xlings.cppm 统一管理，不需要再移 |
| `resolve_package_name()` | 已在 compat.cppm 统一管理 |
| `multi_version_mangle` | 与 cli.cppm 内部数据结构（ResolvedRecord）紧耦合，提取代价大 |
| `ninja incremental retry` | 只有 2 行逻辑，不值得独立模块 |

## 五、resolve_xpkg_path 优先级修复

同时将 `resolve_xpkg_path()` 从 "copy → install" 改为 "install → copy"：

```cpp
// 1. sandbox 已有 → 直接返回
// 2. autoInstall → xlings install
// 3. fallback → mcpp::fallback::copy_xpkg_from_global()
// 4. error
```

## 六、调用方改动示例

### Before (package_fetcher.cppm)
```cpp
// 30 行 copy 逻辑内联在 resolve lambda 里
if (!std::filesystem::exists(verdir)) {
    const char* xhome = ...;
    for (auto& src : candidates) {
        if (exists(src)) { copy(src, verdir, ...); break; }
    }
}
```

### After
```cpp
// 1 行调用
if (!std::filesystem::exists(verdir))
    mcpp::fallback::copy_xpkg_from_global(verdir);
```

### Before (config.cppm)
```cpp
// 50 行 binary 获取链内联
if (auto* e = getenv("MCPP_VENDORED_XLINGS"); ...) { copy...; }
else if (auto found = which("xlings"); ...) { copy...; }
else { return error; }
```

### After
```cpp
auto result = mcpp::fallback::acquire_xlings_binary(destBin, pinnedVersion);
if (result.error.empty()) cfg.xlingsBinary = result.binary;
else return std::unexpected(ConfigError{result.error});
```
