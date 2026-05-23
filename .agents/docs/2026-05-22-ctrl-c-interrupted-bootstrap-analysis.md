# 分析：Ctrl+C 中断 bootstrap 后 mcpp 进入不可用状态

**Date**: 2026-05-22

## 一、复现步骤

```bash
# 1. 全新 mcpp（无 ~/.mcpp/）
mcpp toolchain default llvm@20.1.7
# → 开始 bootstrap（bundled xlings → init → patchelf → ninja）
# → 在 patchelf 下载过程中按 Ctrl+C 中断

# 2. 再次运行
mcpp toolchain install llvm
# → ninja bootstrap 失败（warning: exit 1）
# → LLVM 下载后安装失败（exit 1）

# 3. 再次运行
mcpp toolchain install llvm
# → 这次成功安装

# 4. 但 clang++ 无法运行
mcpp run
# → error: clang++ exited with status 127

# 5. 删除 ~/.mcpp/ 重新来过
rm -rf ~/.mcpp/
mcpp toolchain install llvm
# → 一切正常
```

## 二、问题链分析

### 阶段 1：Ctrl+C 中断导致 bootstrap 半完成

Bootstrap 分 3 步（`src/xlings.cppm:801-871`）：

```
1. ensure_init()      → xlings self init（创建 subos 目录结构）
2. ensure_patchelf()  → 安装 xim:patchelf@0.18.0
3. ensure_ninja()     → 安装 xim:ninja@1.12.1
```

每一步的幂等检查：
- `ensure_init()`：检查 `subos/default/.xlings.json` 是否存在
- `ensure_patchelf()`：检查 `xpkgs/xim-x-patchelf/0.18.0/bin/patchelf` 是否存在
- `ensure_ninja()`：检查 `xpkgs/xim-x-ninja/` 下是否有 `ninja` 二进制

**Ctrl+C 在 patchelf 下载过程中中断**：
- `ensure_init()` 已完成 → marker 文件已写入 ✅
- `ensure_patchelf()` 下载中被中断 → **patchelf xpkg 目录可能处于半解压状态**

### 阶段 2：第二次运行 — ninja 失败

```
mcpp toolchain install llvm
→ Bootstrap ninja into mcpp sandbox (one-time)
→ warning: failed to bootstrap ninja into mcpp sandbox (exit 1)
```

`ensure_patchelf()` 的幂等检查看到 patchelf 二进制不存在（因为上次中断了），
但可能 xpkg 目录已存在（半解压）。xlings 可能认为 patchelf "已安装"（目录存在）
但实际不完整。

ninja 安装失败可能是因为 xlings 内部状态不一致（`install_with_progress` 的直接
调用模式可能因为之前的 patchelf 残留而失败）。

### 阶段 3：LLVM 安装 — exit 1

```
Downloading xim:llvm@20.1.7
error: install failed: xlings install of 'xim:llvm@20.1.7' failed (exit 1)
```

这里走的是新的 install-first 流程（PR #70）：
1. `resolve_quick()` → sandbox 没有 LLVM
2. `install()` → xlings install（NDJSON interface）→ exit 1
3. `copy_from_global()` → `~/.xlings/` 里也没有（全新环境）
4. → 失败

xlings install 失败（exit 1）的原因可能是：
- ninja 不可用（bootstrap 失败了）
- xlings 内部状态被之前的中断破坏

### 阶段 4：第三次运行 — 成功

```
mcpp toolchain install llvm
→ Installed llvm@20.1.7
```

第二次运行虽然报错，但可能已经部分完成了一些操作（如 ninja 重试成功、
LLVM 部分下载后 xlings 做了断点续传），第三次运行时环境已经恢复。

### 阶段 5：clang++ status 127 — 缺少依赖

```
mcpp run
→ clang++ exited with status 127
```

**status 127 = command not found 或 shared library not found**。

这说明虽然 LLVM 安装成功了，但它的某个依赖缺失：

可能原因：
1. **glibc xpkg 未安装**：安装流程中 `resolve_xpkg_path("xim:glibc", autoInstall=true)`
   失败了但被忽略（best-effort，见 `cli.cppm:3740-3747` 注释 "Best-effort"）
2. **patchelf 不可用**：post-install fixup 中 `patchelf_walk` 需要 patchelf，
   但 patchelf 的 bootstrap 失败了 → fixup 跳过 → LLVM 共享库 RUNPATH 未修正
3. **clang++ 的 PT_INTERP 指向不存在的 glibc loader**：如果 glibc 没装，
   `ld-linux-x86-64.so.2` 不在 sandbox 里，clang++ 无法启动

最可能的原因是 **glibc 未正确安装 + patchelf 不可用导致 fixup 跳过**。

## 三、根因

```
Ctrl+C 中断 bootstrap
    ↓
patchelf xpkg 半解压（目录存在但二进制缺失）
    ↓
后续运行 patchelf 幂等检查：目录存在但 binary 不存在 → 重新安装
但 xlings 可能认为 "已安装"（xpkg 目录存在）→ 安装跳过 → patchelf 仍不可用
    ↓
ninja bootstrap 失败（可能依赖 patchelf 或 xlings 内部状态不一致）
    ↓
LLVM install 第一次失败（exit 1）
    ↓
LLVM install 第二次成功（xlings 内部恢复/重试）
但 glibc 可能未正确安装（best-effort 忽略了错误）
且 patchelf 仍不可用 → post-install fixup 跳过
    ↓
clang++ 启动失败（status 127）：
  - 未修正的 PT_INTERP 指向不存在的 loader
  - 或 RUNPATH 指向不存在的路径
    ↓
删除 ~/.mcpp/ 重来 → 干净的 bootstrap → 一切正常
```

## 四、核心问题

### 问题 1：Bootstrap 不具备中断恢复能力

`ensure_patchelf()` 的幂等检查只看最终二进制是否存在，但**不清理半解压的残留目录**。
如果 xlings 的 `install` 命令看到 xpkg 目录已存在就跳过安装，那半解压的目录会
永远卡在不完整状态。

**修复方向**：
- 在 `ensure_patchelf()` / `ensure_ninja()` 中，如果 marker 不存在但 xpkg 目录
  存在，先删除残留目录再重新安装
- 或者在 bootstrap 失败后给用户明确提示：`hint: rm -rf ~/.mcpp/ and retry`

### 问题 2：sysroot 依赖安装是 best-effort，失败被静默忽略

```cpp
// src/cli.cppm:3740-3747
for (auto dep : {"xim:glibc", "xim:linux-headers"}) {
    auto depPayload = fetcher.resolve_xpkg_path(dep, /*autoInstall=*/true, &progress);
    // Best-effort: linux-headers may not be in the index.
    // glibc is usually a dependency of gcc/llvm and already installed.
}
```

glibc 安装失败被静默忽略。但 glibc 对 LLVM 是**必需的**（提供 libc.so、ld-linux
loader）。如果 glibc 安装失败，后续的 LLVM 安装即使成功也无法正常工作。

**修复方向**：
- glibc 应该是 hard dependency，失败应该 abort
- 只有 linux-headers 才是 best-effort

### 问题 3：post-install fixup 静默跳过

```cpp
// src/cli.cppm:3865-3866
if (!glibcLibDir.empty() && std::filesystem::exists(patchelfBin)) {
    // patchelf_walk ...
}
// 如果 patchelfBin 不存在，整个 fixup 静默跳过
```

patchelf 不可用时，fixup 整个跳过但没有警告。用户看到 "Installed" 成功消息，
但实际上工具链处于不可用状态。

**修复方向**：
- 如果 patchelfBin 不存在，输出 warning
- 或者将 patchelf 可用性作为工具链安装的前提条件检查

### 问题 4：无整体健康检查

mcpp 在 `toolchain install` 成功后不验证工具链是否实际可用（比如跑一下
`clang++ --version`）。成功消息可能是假的。

**修复方向**：
- 安装后跑一次 `<compiler> --version` 做 sanity check
- 失败时提示用户问题所在

## 五、影响的代码位置

| 位置 | 问题 | 严重度 |
|------|------|--------|
| `xlings.cppm:833-848` ensure_patchelf | 不清理半解压残留 | 高 |
| `xlings.cppm:851-871` ensure_ninja | 同上 | 高 |
| `cli.cppm:3740-3747` sysroot deps | glibc 失败被静默忽略 | 高 |
| `cli.cppm:3865-3866` LLVM fixup | patchelf 缺失时静默跳过 | 中 |
| `cli.cppm:3878` Installed 消息 | 无 sanity check | 中 |

## 六、推荐修复优先级

1. **Bootstrap 中断恢复**：ensure_patchelf/ninja 发现残留目录时先清理再重装
2. **glibc 作为 hard dependency**：安装失败时 abort 并提示
3. **fixup 跳过时 warning**：patchelf 不可用时明确告知
4. **安装后 sanity check**：跑 `compiler --version` 验证

---

## 七、设计方案：完整性检查 + `mcpp self init`

### 7.1 设计目标

1. **零开销**：正常使用时不增加任何性能开销
2. **主动修复**：用户遇到异常时可以跑 `mcpp self init` 一键恢复
3. **被动检查**：关键操作前做轻量级完整性检查，发现问题时给出提示

### 7.2 `mcpp self init` 命令

**用途**：重新初始化 mcpp sandbox，修复中断/损坏导致的不一致状态。

**行为**：

```bash
$ mcpp self init
  Checking mcpp sandbox integrity...
   Repairing patchelf (incomplete installation detected)
   Repairing ninja (missing binary)
  Verifying glibc payload... ok
  Verifying default toolchain... ok (llvm@20.1.7)
  Sandbox ready.
```

**实现**（`src/cli.cppm` 新增 `cmd_self_init`）：

```cpp
int cmd_self_init(const mcpplibs::cmdline::ParsedArgs& parsed) {
    bool force = parsed.is_flag_set("force");
    auto cfg = mcpp::config::load_or_init();
    auto xlEnv = mcpp::config::make_xlings_env(*cfg);

    // 1. 目录结构
    mcpp::xlings::ensure_init(xlEnv, false);

    // 2. Bootstrap 工具（带修复）
    repair_bootstrap_tool(xlEnv, "patchelf", pinned::kPatchelfVersion);
    repair_bootstrap_tool(xlEnv, "ninja",    pinned::kNinjaVersion);

    // 3. 验证已安装的工具链
    if (!cfg->defaultToolchain.empty()) {
        verify_toolchain(*cfg, cfg->defaultToolchain);
    }

    ui::status("Ready", "sandbox initialized");
    return 0;
}
```

**`--force` 标志**：强制删除所有 bootstrap 工具并重新安装（不只是修复）。

### 7.3 `repair_bootstrap_tool()` — 修复半解压残留

```cpp
void repair_bootstrap_tool(const xlings::Env& env,
                           std::string_view tool,
                           std::string_view version)
{
    auto toolDir = xlings::paths::xim_tool(env, tool, version);
    auto binary  = toolDir / "bin" / tool;

    if (std::filesystem::exists(binary)) {
        // 工具完整，跳过
        ui::status("ok", std::format("{} {}", tool, version));
        return;
    }

    if (std::filesystem::exists(toolDir)) {
        // 目录存在但二进制缺失 → 半解压残留，清理后重装
        ui::info("Repairing", std::format("{} (incomplete installation detected)", tool));
        std::error_code ec;
        std::filesystem::remove_all(toolDir, ec);
    } else {
        ui::info("Installing", std::format("{} {}", tool, version));
    }

    install_with_progress(env, std::format("xim:{}@{}", tool, version), nullptr);
}
```

**核心逻辑**：如果 xpkg 目录存在但二进制不存在 → 判定为半解压残留 → 删除后重装。
这是 Ctrl+C 问题的直接修复。

### 7.4 轻量级完整性检查 — `sandbox_health_check()`

**不在每次命令都跑**。只在以下时机触发：

| 触发时机 | 检查内容 | 开销 |
|---------|---------|------|
| `mcpp self init` | 完整检查 + 修复 | 秒级（如需重装） |
| `mcpp self doctor` | 完整检查（已有，增强） | 毫秒级 |
| `mcpp toolchain install` | bootstrap 工具可用性 | 2 次 `stat()` |
| `mcpp build`/`run` | 无额外检查 | 零 |

**`mcpp build`/`run` 不做任何额外检查**（零开销）。
只在 `toolchain install` 时做最轻量的检查：验证 patchelf 和 ninja 二进制存在。

### 7.5 `ensure_patchelf` / `ensure_ninja` 增强

当前的 ensure 函数只看 marker 是否存在。增强为**检测残留 + 修复**：

```cpp
void ensure_patchelf(const Env& env, bool quiet,
                     const BootstrapProgressCallback& cb)
{
    auto toolDir = paths::xim_tool(env, "patchelf", pinned::kPatchelfVersion);
    auto binary  = toolDir / "bin" / "patchelf";

    if (std::filesystem::exists(binary)) return;  // 完整，跳过

    // 半解压残留检测：目录存在但二进制不存在
    if (std::filesystem::exists(toolDir)) {
        if (!quiet) print_status("Repairing",
            "patchelf (incomplete installation, cleaning up)");
        std::error_code ec;
        std::filesystem::remove_all(toolDir, ec);
    }

    if (!quiet) print_status("Bootstrap",
        "patchelf into mcpp sandbox (one-time)");
    int rc = install_with_progress(env,
        std::format("xim:patchelf@{}", pinned::kPatchelfVersion), cb);
    // ...
}
```

**开销分析**：相比当前代码只多了一个 `exists(toolDir)` 调用（1 次 stat），
仅在 binary 不存在时执行。正常情况下 binary 存在直接返回，零额外开销。

### 7.6 工具链安装后 sanity check

在 `cli.cppm` 的 `Installed` 消息之前，加一次轻量验证：

```cpp
// 安装后 sanity check（1 次进程调用）
auto versionCmd = std::format("{}{} --version {}",
    ld_library_path_prefix,
    shell::quote(bin.string()),
    platform::null_redirect);
auto vr = platform::process::capture(versionCmd);
if (vr.exit_code != 0) {
    ui::warning(std::format(
        "installed {} but `{} --version` failed (exit {}). "
        "Try: mcpp self init",
        pkg.display_spec(), bin.filename().string(), vr.exit_code));
}
```

**开销**：1 次 `compiler --version` 调用（~10ms），只在 `toolchain install` 时执行，
不影响 `build`/`run`。

### 7.7 glibc 从 best-effort 升级为 hard dependency

```cpp
// Before (静默忽略):
for (auto dep : {"xim:glibc", "xim:linux-headers"}) {
    auto depPayload = fetcher.resolve_xpkg_path(dep, true, &progress);
    // 忽略结果
}

// After (glibc 必须成功):
auto glibcPayload = fetcher.resolve_xpkg_path("xim:glibc", true, &progress);
if (!glibcPayload) {
    ui::error("glibc is required but installation failed");
    ui::plain("  hint: mcpp self init");
    return 1;
}
// linux-headers 仍然 best-effort
fetcher.resolve_xpkg_path("xim:linux-headers", true, &progress);
```

### 7.8 CLI 注册

```cpp
.subcommand(cl::App("self")
    .description("Inspect and manage mcpp itself")
    .subcommand(cl::App("init")
        .description("Initialize or repair mcpp sandbox")
        .option(cl::Option("force")
            .help("Force reinstall all bootstrap tools")))
    .subcommand(cl::App("doctor") ...)
    .subcommand(cl::App("env") ...)
    // ...
```

### 7.9 完整的错误恢复用户体验

**正常使用**（零开销）：
```bash
$ mcpp build     # 不做任何额外检查
$ mcpp run       # 不做任何额外检查
```

**异常后恢复**：
```bash
$ mcpp toolchain install llvm
error: install failed: ...
  hint: try `mcpp self init` to repair sandbox

$ mcpp self init
  Checking mcpp sandbox integrity...
   Repairing patchelf (incomplete installation detected)
  Verifying glibc payload... ok
  Sandbox ready.

$ mcpp toolchain install llvm
   Installed llvm@20.1.7 → ...
```

**强制重置**：
```bash
$ mcpp self init --force
  Reinstalling patchelf...
  Reinstalling ninja...
  Sandbox ready.
```

### 7.10 性能影响总结

| 场景 | 额外开销 |
|------|---------|
| `mcpp build` | 零 |
| `mcpp run` | 零 |
| `mcpp toolchain install` | 2 次 stat（ensure 检查）+ 1 次 compiler --version |
| `mcpp self init` | 秒级（检查 + 可能重装） |
| `mcpp self doctor` | 毫秒级（检查 + 报告） |
