# 工具链安装路径问题 — 分析报告 v2

## 问题描述

通过 xlings 安装的 mcpp (`xlings install mcpp`) 执行 `mcpp toolchain install llvm` 时报错：

```
Installing llvm 20.1.7 via mcpp's xlings
Downloading xim:libxml2@2.13.5
error: install failed: xpkg payload missing:
  /home/speak/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry/data/xpkgs/xim-x-llvm/20.1.7
```

## 核心问题：HOME 和 bin 位置分离是否可行？

**回答：完全可行，且是正确的设计方向。**

### mcpp 对 MCPP_HOME 的使用方式

MCPP_HOME 是一个**纯数据目录**，mcpp 不假设自己的二进制在 `MCPP_HOME/bin/` 中：

```
MCPP_HOME/                    ← 纯数据根目录
├── bin/                      ← 仅用于 create_directories，不用于查找 mcpp 自身
├── registry/                 ← xlings 沙箱（XLINGS_HOME）
│   ├── bin/xlings            ← vendored xlings 二进制
│   ├── subos/default/        ← sandbox 结构
│   └── data/xpkgs/           ← 工具链包安装位置
├── bmi/                      ← BMI 缓存
├── cache/                    ← 元数据缓存
├── log/                      ← 日志
└── config.toml               ← 全局配置
```

代码中 `cfg.binDir` 只出现在 `create_directories()` 中，**没有任何地方用它来查找 mcpp 二进制**。mcpp 通过 `self_exe_path()` 找到自己，通过 `MCPP_HOME` 找到数据。两者完全独立。

### 三种部署场景对比

| 场景 | mcpp 二进制位置 | MCPP_HOME | 二进制和HOME同位？ |
|------|----------------|-----------|-------------------|
| Release tarball | `~/mcpp-0.0.20/bin/mcpp` | `~/mcpp-0.0.20/` | ✓ 同位（自包含） |
| `~/.mcpp` 独立安装 | `~/.mcpp/bin/mcpp` | `~/.mcpp/` | ✓ 同位 |
| **xlings 安装** | `~/.xlings/.../xim-x-mcpp/0.0.20/bin/mcpp` | **应该是 `~/.mcpp/`** | ✗ 分离 |
| CI | `~/.xlings/.../bin/mcpp` | `/home/runner/.mcpp`（显式） | ✗ 分离（正常工作） |

**CI 已经证明了 HOME 和 bin 分离可以正常工作**——CI 中 mcpp 二进制在 xlings 目录中，MCPP_HOME 在 `~/.mcpp/`，一切正常。

## 当前问题的根因

当 mcpp 在 xlings 包内运行时，`home_dir()` 的自包含检测逻辑将 **xlings 包目录** 误判为**自包含安装根**：

```
二进制: ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/bin/mcpp
                                                  ^^^^
                                                  parent = "bin" ✓
home_dir() 返回: ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/
                 ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
                 这是 xlings 包目录，不是有效的 MCPP_HOME
```

导致嵌套沙箱：

```
~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/          ← MCPP_HOME（错误）
  └── registry/                                    ← XLINGS_HOME（嵌套）
      └── data/xpkgs/xim-x-llvm/20.1.7/          ← 800MB+ 工具链装到这里
```

**问题**：
1. xlings 可能不尊重嵌套的 XLINGS_HOME，安装到全局 `~/.xlings/` 而非 registry/
2. 即使安装成功，800MB+ 的工具链嵌套在 xlings 包目录内，卸载/更新 mcpp 时全部丢失
3. 多个 mcpp 版本各自维护独立工具链，浪费磁盘

## 修复方案（更新）

### 方案 A：xlings 包内运行时 fallback 到 `~/.mcpp/`（推荐）

```cpp
std::filesystem::path home_dir() {
    // 1. 显式环境变量优先
    if (auto* e = std::getenv("MCPP_HOME"); e && *e)
        return std::filesystem::path(e);

    auto exe = mcpp::platform::fs::self_exe_path();
    if (exe.has_parent_path() && exe.parent_path().filename() == "bin") {
        auto candidate = exe.parent_path().parent_path();

        // 检测是否在 xlings 包目录中运行
        // 路径模式: .../xpkgs/xim-x-mcpp/<version>/bin/mcpp
        bool isXlingsPackage = false;
        for (auto p = candidate;
             p.has_parent_path() && p != p.root_path();
             p = p.parent_path()) {
            if (p.filename() == "xpkgs" && p.parent_path().filename() == "data") {
                isXlingsPackage = true;
                break;
            }
        }

        // xlings 包内：使用 ~/.mcpp/ 独立目录
        // 避免嵌套沙箱，工具链升级 mcpp 后不丢失
        if (isXlingsPackage) {
            // Windows: USERPROFILE, POSIX: HOME
            if constexpr (mcpp::platform::is_windows) {
                if (auto* e = std::getenv("USERPROFILE"); e && *e)
                    return std::filesystem::path(e) / ".mcpp";
            } else {
                if (auto* e = std::getenv("HOME"); e && *e)
                    return std::filesystem::path(e) / ".mcpp";
            }
        }

        // 非 xlings 包：自包含模式（release tarball）
        bool isDevPath = false;
        for (auto p = exe.parent_path();
             !p.empty() && p != p.parent_path();
             p = p.parent_path()) {
            if (p.filename() == "target") { isDevPath = true; break; }
        }
        if (!isDevPath)
            return candidate;
    }

    // 兜底
    if constexpr (mcpp::platform::is_windows) {
        if (auto* e = std::getenv("USERPROFILE"); e && *e)
            return std::filesystem::path(e) / ".mcpp";
    } else {
        if (auto* e = std::getenv("HOME"); e && *e)
            return std::filesystem::path(e) / ".mcpp";
    }
    return std::filesystem::current_path() / ".mcpp";
}
```

### 修复后各场景的路径

| 场景 | mcpp bin | MCPP_HOME | XLINGS_HOME | 工具链位置 |
|------|----------|-----------|-------------|-----------|
| Release tarball | `~/mcpp/bin/mcpp` | `~/mcpp/` | `~/mcpp/registry/` | `~/mcpp/registry/data/xpkgs/` |
| xlings 安装 | `~/.xlings/.../mcpp/bin/mcpp` | **`~/.mcpp/`** | **`~/.mcpp/registry/`** | **`~/.mcpp/registry/data/xpkgs/`** |
| CI | `~/.xlings/.../mcpp` | `~/.mcpp`（显式） | `~/.mcpp/registry/` | `~/.mcpp/registry/data/xpkgs/` |

**关键**：xlings 安装和 CI 场景现在行为完全一致。工具链安装到 `~/.mcpp/registry/` 中，升级 mcpp 版本不影响已安装的工具链。

### 全平台统一性

| 平台 | HOME 环境变量 | 默认 MCPP_HOME |
|------|-------------|---------------|
| Linux | `$HOME` | `$HOME/.mcpp` |
| macOS | `$HOME` | `$HOME/.mcpp` |
| Windows | `%USERPROFILE%` | `%USERPROFILE%\.mcpp` |

三平台行为一致：xlings 包内运行时统一使用 `~/.mcpp/`。

### 这个方案不影响什么

1. **Release tarball 用户**：二进制不在 `xpkgs/` 路径下，走原有自包含逻辑，不受影响
2. **CI**：显式设置了 `MCPP_HOME`，`home_dir()` 第一行就返回，不受影响
3. **已有 `~/.mcpp/` 数据**：方案只是让 xlings 安装的 mcpp 也使用这个目录，数据向前兼容
4. **mcpp 和 xlings 共存**：mcpp 的 xlings 沙箱在 `~/.mcpp/registry/`，与全局 `~/.xlings/` 独立

### 这个方案解决什么

1. **嵌套沙箱问题**：不再在 xlings 包目录内创建 registry/
2. **工具链持久性**：升级 mcpp (`xlings install mcpp@0.0.21`) 不丢失已安装的 LLVM/GCC
3. **磁盘浪费**：多版本 mcpp 共享同一个 `~/.mcpp/` 下的工具链
4. **xlings 兼容性**：不依赖 xlings 是否正确处理嵌套 XLINGS_HOME

## 受影响文件

| 文件 | 改动 |
|------|------|
| `src/config.cppm` | `home_dir()` 增加 xlings 包路径检测（~15 行） |
| 无其他改动 | 路径推导链后续自动生效 |
