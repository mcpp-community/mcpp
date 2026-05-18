# Windows 平台成熟度提升方案

> 基于 PR #52 code review 反馈，针对 Windows 支持从"可自举"到"生产就绪"的优化路径。

## 当前状态

| 能力 | Linux | macOS | Windows | 差距 |
|------|-------|-------|---------|------|
| self-host | ✅ | ✅ | ✅ | — |
| `mcpp test` (unit) | ✅ | ✅ | ❌ | 缺 clang-scan-deps |
| E2E 覆盖 | 46/46 | 33/46 | 22/48 | 26 项 skip |
| `mcpp pack` | ✅ (musl static) | ✅ (手动) | ❌ (CI 手写 zip) | pack 不支持 PE |
| release workflow | ✅ | ✅ | ✅ | build-windows job 已加 |
| MSVC 工具链 | N/A | N/A | 模型预留 | detect 不支持 |
| 默认工具链回退 | gcc@15.1.0-musl | llvm@20.1.7 | llvm@20.1.7 | ✅ 已修 |

## 优化方案（按优先级）

### P0: 补齐 release workflow + 减少 E2E skip — DONE

**目标：** Windows 二进制进入正式 release 发布流程。

#### 1. release.yml 加 build-windows job — DONE

`release.yml` 中已有 `build-windows` job，产出 `mcpp-<version>-windows-x86_64.zip` + sha256，上传到 GitHub Release。

#### 2. 修复高价值 E2E skip 项

按投入产出排序：

| 测试 | 修复方式 | 工作量 |
|------|----------|--------|
| `02_new_build_run.sh` | 检查 `bin/hello` 或 `bin/hello.exe` | 小 |
| `16_test_failing.sh` | 调查 Windows 上 exit code 传递 | 小 |
| `35_workspace.sh` | 同上，binary 名加 `.exe` 检查 | 小 |
| `36_llvm_toolchain.sh` | 同上 | 小 |
| `19_bmi_cache_reuse.sh` | 修复 `cp_bmi` rule 的混合路径 | 中 |
| `24_git_dependency.sh` | CRLF + Windows 路径处理 | 中 |
| `38_self_config_mirror.sh` | xlings mirror cmd.exe 路径 | 中 |

**预计可把 E2E 从 22 passed 提升到 ~30 passed。**

### P1: PlatformTraits 抽象 — DONE (infrastructure)

**目标：** 减少散落的 `#if defined(_WIN32)` / `#if defined(__APPLE__)`。

`src/platform.cppm` 已创建，集中平台差异（exe_suffix、lib_prefix、null_redirect、shell_quote 等）。

**受益文件：** `plan.cppm`、`flags.cppm`、`ninja_backend.cppm`、`probe.cppm`、`clang.cppm`、`config.cppm`（待各文件迁移到 `mcpp::platform` 命名空间）

### P2: ToolchainProvider 重构 — TODO (src/provider.cppm 待创建)

**目标：** 把工具链行为从散落的 `if (isClang)` / `if (isGcc)` 收敛到 provider 接口。

当前工具链代码分散在：
- `gcc.cppm` — GCC 行为
- `clang.cppm` — Clang/libc++ 行为 + MSVC STL fallback
- `llvm.cppm` — xlings 包映射
- `detect.cppm` — 只处理 GCC/Clang
- `flags.cppm` — 编译/链接 flags 按平台分支
- `ninja_backend.cppm` — 构建规则按平台分支

建议拆成明确的 provider：

```
ToolchainProvider (interface)
  ├── GccProvider          — GCC + glibc/musl
  ├── ClangLibcxxProvider  — Clang + libc++ (Linux/macOS)
  ├── ClangMsvcProvider    — Clang + MSVC STL (Windows)
  └── MsvcProvider         — cl.exe (未来)
```

每个 provider 声明：
- `frontend()` → 编译器路径
- `c_compiler()` → C 编译器
- `archive_tool()` → ar/llvm-ar/lib.exe
- `scanner()` → clang-scan-deps 路径
- `stdlib_id()` → libc++/libstdc++/msvc-stl
- `find_std_module()` → std.cppm/std.cc/std.ixx
- `compile_flags()` → 平台相关编译 flags
- `link_flags()` → 平台相关链接 flags
- `bmi_traits()` → .gcm/.pcm/.ifc

> **注：** `src/provider.cppm` 尚未创建；现有调用者（gcc.cppm、clang.cppm 等）待迁移。

### P3: 跨平台 Process Runner — DONE (infrastructure, callers pending)

**目标：** 消除 shell 字符串拼接，统一子进程执行。

`src/process.cppm` 已创建，提供 `ProcessOptions` / `ProcessResult` / `run()` 接口：
- POSIX: `fork/exec` + `pipe`
- Windows: `CreateProcessW` + `STARTUPINFOW`

> **注：** 现有调用点（`probe.cppm`、`xlings.cppm`、`stdmod.cppm`、`ninja_backend.cppm`、`config.cppm`）仍使用 `popen`，待逐步迁移到 `process::run()`。

### P4: `mcpp pack` Windows 支持

**目标：** `mcpp pack` 原生支持 Windows PE 打包。

当前 `pack.cppm` 依赖：
- `LD_TRACE_LOADED_OBJECTS` (Linux ELF)
- `patchelf` (RPATH 修改)
- `tar -czf` (打包格式)

Windows 需要：
- DLL 依赖收集（`dumpbin /dependents` 或 `llvm-objdump`）
- 无需 RPATH（DLL 在 exe 同目录自动找到）
- `.zip` 打包 + `.bat` wrapper

建议 pack 做成平台策略：

```
PackStrategy (interface)
  ├── LinuxElfPack   — ldd + patchelf + tar.gz
  ├── MacosMachoPack — otool + install_name_tool + tar.gz
  └── WindowsPePack  — dumpbin + zip + .bat
```

### P5: E2E 能力标签化 — DONE (infrastructure)

**目标：** 从"平台 skip 列表"升级为"能力标签"。

能力标签体系已全面落地：
- 全部 48 个 E2E 脚本头部均已声明 `# requires:` 行
- `run_all.sh` 自动检测平台能力（elf、gcc、musl、pack、symlink、scan-deps、import-std-libcxx、unix-shell、fresh-sandbox）并动态 skip
- 不再维护平台 skip 列表

支持的标签：

```bash
# requires: elf          — 需要 ELF 工具链
# requires: gcc          — 需要 GCC
# requires: symlink      — 需要 ln -sf（仅 Linux/macOS 或 Windows Developer Mode）
# requires: scan-deps    — 需要 clang-scan-deps
# requires: import-std-libcxx — 需要 import std (std.cppm via libc++)
# requires: pack         — 需要 mcpp pack
# requires: unix-shell   — 需要 bash 风格 shell（非 cmd.exe）
# requires: fresh-sandbox — 需要隔离 MCPP_HOME
```

## 实施顺序

```
P0 release + E2E 修复   ✅ DONE（release.yml build-windows job 已加）
  ↓
P1 PlatformTraits       ✅ DONE（src/platform.cppm 已创建，待调用者迁移）
  ↓
P2 ToolchainProvider    ← src/provider.cppm 待创建 + 调用者迁移
  ↓
P3 Process Runner       ✅ DONE（src/process.cppm 已创建，待调用者迁移）
  ↓
P4 mcpp pack Windows    ← 产品化打包
  ↓
P5 E2E 标签化           ✅ DONE（全部 48 个测试已标签化）
```

## 预期里程碑

| 阶段 | 目标 | Windows E2E 通过率 |
|------|------|-------------------|
| 当前 | self-host + 基础 E2E | 22/48 (46%) |
| P0 完成 | release + 高价值 E2E | ~30/48 (63%) |
| P1+P2 完成 | 平台抽象 + provider | ~35/48 (73%) |
| P3+P4 完成 | process runner + pack | ~40/48 (83%) |
| P5 完成 | 能力标签 | 动态评估 |
