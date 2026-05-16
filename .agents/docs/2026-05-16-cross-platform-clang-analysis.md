# MCPP 跨平台 Clang/LLVM 支持分析报告

> 2026-05-16 — Linux Clang 平权现状 + 跨平台设计路线

## 一、Linux Clang 已达 GCC 平权

通过 PR #34 (Clang module pipeline parity) 到 `c7c1c3e` (std.compat + restat)，12 个核心 Blocker 已全部解决：

| 能力 | GCC | Clang | 状态 |
|---|---|---|---|
| 工具链检测 & 安装 | `gcc@16` | `llvm@20` | ✅ 对等 |
| 非模块 C/C++ 编译 | ✅ | ✅ | ✅ 对等 |
| `import std` | `bits/std.cc` → `gcm.cache/std.gcm` | `std.cppm` → `pcm.cache/std.pcm` | ✅ 对等 |
| `import std.compat` | GCC 隐式覆盖 | `std.compat.cppm` → `pcm.cache/std.compat.pcm` | ✅ 对等 |
| 多模块项目 (dyndep) | `-fdeps-format=p1689r5` | 同一 flag (去掉 `-fmodules`) | ✅ 对等 |
| BMI 缓存 (跨项目) | `gcm.cache/*.gcm` | `pcm.cache/*.pcm` | ✅ 对等 |
| 增量重编 | ✅ | ✅ + restat | ✅ 对等 |
| Fingerprint 隔离 | ✅ | ✅ (独立 fingerprint) | ✅ 对等 |
| E2E 测试覆盖 | 6+ 个 | 6 个 (36-41) | ✅ 对等 |

核心抽象层 `BmiTraits` (`model.cppm:46-56`) 是平权的关键。

## 二、跨平台差距分析

### 2.1 macOS (Apple Clang) — 难度：中等

**可复用**：BmiTraits、clang.cppm 检测 (已识别 `apple clang version`)、P1689 扫描、两步编译

**Blocker (6 个)**：

| # | 问题 | 说明 |
|---|---|---|
| M1 | Apple Clang 版本差异 | Apple Clang 版本号与 upstream 不同 |
| M2 | libc++ 模块源码路径 | macOS 的 libc++ 在 Xcode SDK 内 |
| M3 | Sysroot 发现 | 需要 `xcrun --show-sdk-path` |
| M4 | 链接器差异 | macOS 用 `ld64`/`ld_prime`，`-rpath` 语法不同 |
| M5 | 运行时库 | macOS 自带 libc++，不需要额外 rpath |
| M6 | xlings 包管理 | macOS 上包名/路径适配 |

### 2.2 Windows (MSVC) — 难度：高

**Blocker (10+ 个)**：全新编译器族 (`.ifc`)、编译命令语法 (`/std:c++latest`)、P1689 (`/scanDependencies`)、路径分隔符、Ninja shell 可移植性、VS 环境发现 (`vswhere`)、链接器差异等。

### 2.3 Windows (Clang-cl) — 难度：中高

相比 MSVC 稍简单，可作为 Windows 支持第一步。

## 三、架构设计

### 3.1 分层架构

```
┌─────────────────────────────────────────────┐
│              mcpp CLI / Build Pipeline       │  ← 平台无关
├─────────────────────────────────────────────┤
│           BmiTraits + CompilerProfile        │  ← 编译器抽象 (已有)
├──────────┬──────────┬──────────┬────────────┤
│ gcc.cppm │clang.cppm│ msvc.cppm│apple_clang │  ← 编译器实现
├──────────┴──────────┴──────────┴────────────┤
│              PlatformTraits (新增)            │  ← 平台抽象
├──────────┬──────────┬───────────────────────┤
│  linux   │  macos   │  windows              │  ← 平台实现
├──────────┴──────────┴───────────────────────┤
│           xlings / Registry                  │  ← 包管理
└─────────────────────────────────────────────┘
```

### 3.2 建议新增 PlatformTraits

```cpp
struct PlatformTraits {
    std::string_view os;           // "linux" | "macos" | "windows"
    std::string_view exeExt;       // "" | ".exe"
    std::string_view staticLibExt; // ".a" | ".lib"
    std::string_view sharedLibExt; // ".so" | ".dylib" | ".dll"
    std::string_view copyCmd;      // "cp -p" | "copy"
    std::string_view cmpCmd;       // "cmp -s" | "fc /b"
    std::string_view rmCmd;        // "rm -f"  | "del /f"
};
```

### 3.3 实施路线

```
Phase 1 (已完成): Linux GCC + Linux Clang 平权
    ↓
Phase 2: macOS Apple Clang (复用 clang.cppm, Unix-like)
    ↓
Phase 3: Windows Clang-cl (新增 PlatformTraits, Ninja shell 可移植化)
    ↓
Phase 4: Windows MSVC (新增 msvc.cppm, .ifc, vswhere)
```

## 四、关键决策

1. **macOS**: 复用 `clang.cppm`，内部 `is_apple_clang()` 分支
2. **Windows 首选**: 先 Clang-cl (复用 `.pcm`)，后 MSVC
3. **Ninja shell 可移植**: backup-compare-restore 抽取为 `mcpp internal bmi-guard` 子命令
4. **xlings 跨平台**: `publisher.cppm` 已有三平台支持，包管理层基本就绪
