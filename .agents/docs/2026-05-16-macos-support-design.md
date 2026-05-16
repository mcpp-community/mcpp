# macOS Support Design — LLVM/Clang 自含工具链方案

Date: 2026-05-16

## 目标

在 macOS (ARM64) 上达到与 Linux LLVM 工具链同等可用水平：
- 非模块 C/C++ 编译
- `import std` 支持
- 多模块项目 + dyndep
- BMI 缓存 + 增量构建

## 设计原则

1. **从 upstream LLVM 切入**，不依赖 Apple Clang
2. **最小宿主依赖** — 仅需 macOS SDK（CommandLineTools），编译器/libc++/linker/runtime 全用 xlings 自含的 LLVM 包
3. **不引入 xlings 外部依赖** — 通过 xlings 包生态获取工具链

## 前提条件

xlings LLVM 包（`xim:llvm@20.1.7`）macOS ARM64 版本：
- 已有分发包：`LLVM-20.1.7-macOS-ARM64.tar.xz`
- 包含：clang++, lld, llvm-ar, libc++, compiler-rt, libunwind
- 安装时生成 `clang++.cfg` 自动配置 sysroot + libc++ 路径

macOS SDK（`/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk`）提供：
- 系统头文件（`<unistd.h>`, `<mach/mach.h>` 等）
- libSystem（macOS 的 libc 等价物）
- 这是 macOS 平台的硬限制，无法绕过

## 代码改动清单

### 1. `src/toolchain/probe.cppm` — 平台感知的运行时路径发现

**问题**：`discover_compiler_runtime_dirs()` 硬编码 `x86_64-unknown-linux-gnu` 和 Linux 系统路径。

**方案**：
```cpp
// discover_compiler_runtime_dirs: 加 macOS 分支
if (looksLikeLlvm) {
    append_existing_unique(dirs, root / "lib");
#if defined(__linux__)
    append_existing_unique(dirs, root / "lib" / "x86_64-unknown-linux-gnu");
    append_existing_unique(dirs, "/lib/x86_64-linux-gnu");
    append_existing_unique(dirs, "/usr/lib/x86_64-linux-gnu");
    append_existing_unique(dirs, "/usr/lib64");
#elif defined(__APPLE__)
    append_existing_unique(dirs, root / "lib" / "aarch64-apple-darwin");
    append_existing_unique(dirs, root / "lib" / "darwin");
#endif
}
```

**问题**：`discover_link_runtime_dirs()` 硬编码 `x86_64-unknown-linux-gnu` fallback。

**方案**：改为条件编译，macOS 下不添加 Linux 路径。

**问题**：`probe_sysroot()` 在 Apple Clang / upstream LLVM on macOS 下 `-print-sysroot` 返回空。

**方案**：加 macOS fallback：
```cpp
// 在 probe_sysroot 末尾，如果结果为空且在 macOS 上：
#if defined(__APPLE__)
if (s.empty()) {
    auto xcrun_r = run_capture("xcrun --show-sdk-path 2>/dev/null");
    if (xcrun_r) {
        auto sdk = trim_line(*xcrun_r);
        if (!sdk.empty() && std::filesystem::exists(sdk)) return sdk;
    }
}
#endif
```

### 2. `src/build/flags.cppm` — macOS 链接 flags 适配

**问题**：
- `-Wl,-rpath,<dir>` 在 macOS ld64 上语法相同，但 ELF-only flags 如 `--enable-new-dtags` 不存在
- `-static` 在 macOS 上不可用（libSystem 必须动态链接）
- `-static-libstdc++` 对 Clang 已跳过（现有代码已处理）

**方案**：
```cpp
// flags.cppm: compute_flags 链接部分
std::string full_static = "";
#if !defined(__APPLE__)
full_static = (f.linkage == "static") ? " -static" : "";
#endif
// macOS 不支持 full static，忽略该配置
```

rpath 语法：macOS ld64 支持 `-rpath <path>`（通过 `-Wl,-rpath,<path>`），行为与 Linux 相同，无需改动。

### 3. `src/pack/pack.cppm` — macOS 打包支持（Phase 2）

**问题**：patchelf 只适用于 ELF。macOS 用 Mach-O，需要 `install_name_tool` 或直接用 `@rpath`。

**方案（MVP 先跳过）**：macOS 首版不做 `mcpp pack`，focus on `mcpp build` 可用。后续用 `install_name_tool -add_rpath` 替代 patchelf。

### 4. `install.sh` — 增加 darwin-arm64

```bash
case "${uname_s}-${uname_m}" in
    Linux-x86_64)   PLAT="linux-x86_64" ;;
    Darwin-arm64)   PLAT="darwin-arm64" ;;
    Darwin-x86_64)  PLAT="darwin-x86_64" ;;
    *)              ... ;;
esac
```

macOS 上 `sha256sum` 不存在，改用 `shasum -a 256`。

### 5. `src/cli.cppm` — patchelf_walk 跳过 macOS

现有的 `patchelf_walk` / specs fixup 是 ELF-only。macOS 上跳过：
```cpp
#if !defined(__APPLE__)
// existing patchelf logic
#endif
```

### 6. CI Workflow — `.github/workflows/ci-macos.yml`

轻量 macOS 验证 CI：
- 运行环境：`macos-14`（ARM64 runner）
- 步骤：安装 xlings → 安装 mcpp → `mcpp build` → `mcpp test`
- 不跑全量 E2E（先验证核心编译链路）

## 验证计划

1. `mcpp build` 能在 macOS + xlings LLVM 20 下编译 hello world
2. `import std` 模块项目能编译通过
3. mcpp 自身能在 macOS 上 self-host 编译（长期目标）

## 不做的事

- Apple Clang 支持（用 upstream LLVM 即可）
- macOS 上的 musl 静态链接（不适用）
- `mcpp pack` 的 macOS Mach-O 支持（Phase 2）
- Universal binary（arm64 + x86_64 fat binary）

## 风险

1. xlings LLVM macOS 包的 `clang++.cfg` 尚不完整（缺 lld/compiler-rt 配置）— 需要 xlings 上游补全
2. `ld64.lld` 稳定性 — LLVM 20 的 Mach-O lld 已较成熟，但可能有边缘 case
3. macOS SDK 版本差异 — CommandLineTools vs Xcode SDK 路径不同，需 fallback 链
