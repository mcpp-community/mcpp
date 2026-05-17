# Windows LLVM/Clang 支持设计方案

Date: 2026-05-17

## 1. 目标

在 Windows x86_64 上用 xlings LLVM（clang++ / clang-cl）支持 mcpp 构建 C++23 模块项目，达到与 Linux/macOS 同等的核心可用水平：
- `mcpp build` / `mcpp run` / `mcpp test`
- `import std` 支持
- 多模块项目 + 增量编译
- 自举（mcpp 编译自己）

## 2. 现状分析

### 2.1 xlings LLVM Windows 包

**已有**（xlings-res/llvm 20.1.7）：
```
bin/clang++.exe, clang.exe, clang-cl.exe   ← 编译器
bin/lld-link.exe                           ← MSVC 兼容链接器
bin/llvm-ar.exe, llvm-lib.exe              ← 归档工具
bin/llvm-rc.exe, llvm-mt.exe               ← 资源编译器
lib/clang/20/lib/windows/                  ← compiler-rt
```

**没有**：
- ❌ libc++ 头文件和库（`include/c++/v1/` 不存在）
- ❌ `std.cppm` / `std.compat.cppm` 模块源码
- ❌ `clang-scan-deps.exe`（P1689 模块扫描器）

### 2.2 含义

Windows LLVM 包设计为 **MSVC 兼容模式**：
- 使用 MSVC 的 STL（`<iostream>` 等来自 Visual Studio）
- 使用 MSVC 的 C runtime（ucrt）
- 通过 `clang-cl.exe` 驱动（接受 `/std:c++latest`、`/EHsc` 等 MSVC 风格参数）
- 链接用 `lld-link.exe`（MSVC `link.exe` 兼容）

这是 **Windows 上的行业标准做法**——即使用 Clang，也通常走 MSVC ABI。

### 2.3 C++23 Modules 在 Windows MSVC STL 上的状态

MSVC STL 从 VS 2022 17.5 起支持 `import std;`：
- 需要 `/std:c++latest` 或 `/std:c++23`
- 模块文件格式：`.ifc`（不是 `.pcm` 或 `.gcm`）
- 但 **clang-cl 目前不支持 MSVC 的 `.ifc` 格式**

**clang++ (GNU 驱动) on Windows**：
- 可以用 `-stdlib=libc++` 但需要自带 libc++
- 当前 xlings Windows LLVM 包没有 libc++
- 如果补充 libc++，可以用与 Linux/macOS 相同的 `.pcm` 模块模型

### 2.4 mcpp 代码现状

| 组件 | Windows 状态 | 需要改动 |
|------|-------------|---------|
| 平台检测（`_WIN32`） | ✅ 已有 | 无 |
| CompilerId::MSVC | ✅ enum 定义 | 需要实现 |
| `probe.cppm` | ❌ 用 Unix shell | 需要 Windows 移植 |
| `flags.cppm` | ❌ 全是 Unix flags | 需要 MSVC flags |
| `ninja_backend.cppm` | ❌ shell 命令 | 需要 cmd/PowerShell 适配 |
| `config.cppm` | ❌ `/proc/self/exe` | 需要 `GetModuleFileName` |
| `install.sh` | ⚠️ bash only | Windows 需要 PowerShell |

## 3. 技术方案

### 3.1 两条路径对比

| 方案 | 路径 | 优点 | 缺点 |
|------|------|------|------|
| **A: clang++ + libc++** | GNU 驱动 + 自带 libc++ | 与 Linux/macOS 统一，`.pcm` 格式 | 需要补充 libc++ 到 LLVM 包 |
| **B: clang-cl + MSVC STL** | MSVC 兼容驱动 | Windows 原生，ABI 兼容 | 全新编译模型（`/std:c++latest`），`.ifc` 格式不兼容 |

**推荐方案 A**：用 `clang++.exe`（GNU 驱动）+ 补充 libc++。理由：
1. 与 Linux/macOS 共享同一套模块编译逻辑（`.pcm`、`-fmodule-file=`）
2. 不依赖 Visual Studio 安装
3. mcpp 核心代码改动最小（只需要处理路径分隔符和 shell 命令差异）

### 3.2 前提条件

1. **xlings LLVM Windows 包需要补充 libc++**：
   - `include/c++/v1/` — libc++ 头文件
   - `share/libc++/v1/std.cppm` + `std.compat.cppm` — 模块源
   - `lib/libc++.lib` (或 `.a`) — 静态库
   - `bin/clang-scan-deps.exe` — P1689 扫描器

2. **或者**：在 LLVM Windows 包中生成 `clang++.cfg` 配置 libc++ 路径

### 3.3 mcpp 代码改动清单

#### Phase 1: 核心编译（让 `mcpp build` 在 Windows 上工作）

| 文件 | 改动 | 优先级 |
|------|------|--------|
| `probe.cppm` | `probe_compiler_binary`: Windows 用 `where.exe` 替代 `command -v` | P0 |
| `probe.cppm` | `run_capture`: Windows 用 `_popen`/`_pclose` | P0 |
| `probe.cppm` | `discover_*_dirs`: 添加 `#if defined(_WIN32)` 分支 | P0 |
| `flags.cppm` | Windows 链接 flags：无 `-rpath`（Windows 不支持） | P0 |
| `ninja_backend.cppm` | Shell 命令替换：`mkdir -p` → `cmd /c mkdir`, `cp` → `copy` | P0 |
| `ninja_backend.cppm` | `mcpp_exe_path`: 用 `GetModuleFileNameW` | P0 |
| `config.cppm` | 同上，exe 路径获取 | P0 |
| `clang.cppm` | Windows libc++ 路径发现 | P1 |
| `cli.cppm` | 默认工具链 `llvm@20.1.7` for Windows | P1 |

#### Phase 2: 可执行文件扩展名

| 位置 | 改动 |
|------|------|
| `ninja_backend.cppm` | 输出 `bin/mcpp.exe` 而非 `bin/mcpp` |
| `manifest.cppm` | `kind = "bin"` 产出 `.exe` |
| `cli.cppm` | `mcpp run` 查找 `.exe` |

#### Phase 3: CI + Release

| 改动 | 说明 |
|------|------|
| `.github/workflows/ci-windows.yml` | Windows CI（`windows-latest` runner） |
| `.github/workflows/bootstrap-windows.yml` | xmake 首次编译 |
| `release.yml` | 添加 Windows job |

### 3.4 Ninja shell 命令移植

这是最复杂的部分。当前 build.ninja 中的 shell 命令：

| 当前 (Unix) | Windows 等价 | 说明 |
|-------------|-------------|------|
| `mkdir -p $(dirname $out) && cp -f $in $out` | `cmd /c if not exist "$$(dir $out)" mkdir "$$(dir $out)" && copy /y $in $out` | 复制 BMI |
| `if [ -n "$bmi_out" ] && ...` | `cmd /c ...` 或 PowerShell | BMI restat 逻辑 |
| `cd ... && $cxx ...` | `cmd /c cd /d ... && $cxx ...` | 编译命令 |
| `env LD_LIBRARY_PATH=...` | 不需要（Windows 用 PATH） | 运行时路径 |

**建议**：在 `ninja_backend.cppm` 中按平台生成不同的 rule 命令，用 `#if defined(_WIN32)` 条件编译。

### 3.5 Windows 链接策略

```cpp
#if defined(_WIN32)
    // Windows: clang++ GNU driver links against libc++ automatically
    // No -rpath (not a thing on Windows)
    // No sysroot (not needed for MSVC ucrt)
    // Static libc++: -static-libc++ (or statically link libc++.a)
    f.ld = std::format("{}{}", full_static, b_flag);
#endif
```

Windows 产出的 `.exe` 运行时依赖：
- `ucrt` (Universal C Runtime) — Windows 10+ 自带
- `libc++.dll` 或静态链接 `libc++.a`
- `vcruntime140.dll` — 如果用 MSVC 兼容模式

## 4. 实施计划

### Step 1: 验证 xlings LLVM Windows 能否编译 C++23 模块

创建 `ci-windows.yml` 在 GitHub Actions `windows-latest` runner 上：
1. 安装 xlings
2. 安装 LLVM
3. 手动用 clang++ 编译 `import std`（如果 libc++ 可用）
4. 如果 libc++ 不可用，验证 clang-cl + MSVC STL

### Step 2: xmake bootstrap

用 xmake 在 Windows 上编译 mcpp（参考 mcpp-dev 的 xmake.lua）。

### Step 3: mcpp 代码适配

基于 CI 验证结果，逐步适配 probe/flags/ninja_backend。

### Step 4: Self-host + Release

mcpp 自举 → 打包 → release。

## 5. 风险

| 风险 | 影响 | 缓解 |
|------|------|------|
| xlings Windows LLVM 包无 libc++ | 无法用 `import std` | 需要上游补充或用 MSVC STL |
| ninja shell 命令移植复杂 | build.ninja 在 Windows 上不工作 | 可用 ninja 的 `msvc_deps_prefix` 特性 |
| `clang-scan-deps.exe` 缺失 | P1689 扫描不可用 | GCC 模式的 `-fdeps-format` 也可用 |
| Windows path separator (`\` vs `/`) | 路径拼接问题 | `std::filesystem` 已处理大部分 |

## 6. 依赖关系

```
xlings LLVM Windows 包 (libc++ 补充)
    ↓
CI 验证 (clang++ + import std)
    ↓
xmake bootstrap (产出 mcpp.exe)
    ↓
mcpp 代码适配 (probe/flags/ninja)
    ↓
self-host (mcpp.exe 编译 mcpp.exe)
    ↓
release
```
