# 设计方案：中断安装统一恢复机制

**Date**: 2026-05-23
**Status**: Proposed

## 一、问题本质

所有中断问题的模式完全一致：

```
xlings install <package>
  → 创建 xpkg 目录
  → 下载/解压中
  → Ctrl+C / 网络断开 / 进程被杀
  → xpkg 目录残留（存在但不完整）

再次安装：
  → xlings 看到目录已存在 → 认为"已安装" → 跳过
  → 实际不完整 → 后续操作失败
  → 永久卡死
```

受影响的所有场景：
| 场景 | 残留位置 | 当前修复 |
|------|---------|---------|
| bootstrap patchelf | `xpkgs/xim-x-patchelf/` | ✅ ensure_patchelf 已修复 |
| bootstrap ninja | `xpkgs/xim-x-ninja/` | ✅ ensure_ninja 已修复 |
| toolchain install llvm | `xpkgs/xim-x-llvm/` | ❌ 未修复 |
| toolchain install gcc | `xpkgs/xim-x-gcc/` | ❌ 未修复 |
| sysroot deps glibc | `xpkgs/xim-x-glibc/` | ❌ 未修复 |
| sysroot deps linux-headers | `xpkgs/*-x-linux-headers/` | ❌ 未修复 |
| modular lib install | `xpkgs/mcpplibs-x-*/` | ❌ 未修复 |

**需要一个统一机制**，不是逐个修补。

## 二、设计方案

### 方案 A：Lock 文件机制

**原理**：安装开始时创建 `.installing` lock 文件，安装完成时删除。
如果下次看到 lock 文件存在 → 上次安装被中断 → 自动清理重来。

```
xpkgs/xim-x-llvm/20.1.7/
  .installing          ← 安装进行中的标记
  bin/                 ← 安装完成后才出现
  lib/
```

**流程**：
```
install(pkg):
  dir = xpkgs/<pkg>/<ver>/
  lock = dir / ".installing"

  if exists(dir):
    if exists(lock):
      # 上次中断 → 清理后重装
      log("detected interrupted install, cleaning up")
      remove_all(dir)
    else:
      # 安装完整 → 跳过
      return ok

  mkdir(dir)
  write(lock)           ← 标记开始
  ... download, extract, elfpatch ...
  remove(lock)          ← 标记完成
```

**用户体验**：
```bash
$ mcpp toolchain install llvm 20.1.7
 Downloading xim:llvm@20.1.7 [===>        ] 47MB/266MB
^C

$ mcpp toolchain install llvm 20.1.7
   Repairing llvm@20.1.7 (interrupted install detected, re-downloading)
 Downloading xim:llvm@20.1.7 [===>        ] ...
   Installed llvm@20.1.7
```

| 优点 | 缺点 |
|------|------|
| 精准检测（100% 判断中断 vs 完整） | 需要在 xlings install 前后加 lock 逻辑 |
| 自动恢复，用户无感 | lock 文件本身也可能残留（kill -9 场景） |
| 所有包类型统一处理 | — |

### 方案 B：完整性标记文件

**原理**：安装完成后写一个 `.installed` 标记文件。没有标记 = 不完整。

```
xpkgs/xim-x-llvm/20.1.7/
  .mcpp_installed      ← 安装完成的标记（由 mcpp 写入）
  bin/
  lib/
```

**流程**：
```
resolve_or_install(pkg):
  dir = xpkgs/<pkg>/<ver>/
  marker = dir / ".mcpp_installed"

  if exists(dir) && exists(marker):
    return ok            # 完整安装

  if exists(dir) && !exists(marker):
    # 目录存在但无标记 → 不完整 → 清理
    log("incomplete install detected, cleaning up")
    remove_all(dir)

  install(pkg)           # 全新安装
  write(marker)          # 标记完成
```

**用户体验**：同方案 A（自动检测、自动恢复）

| 优点 | 缺点 |
|------|------|
| 比 lock 更简单（只需写入一次） | 从 ~/.xlings/ 拷贝的包没有 .mcpp_installed 标记 |
| 不需要在安装前加标记 | 需要在 copy fallback 后也写标记 |
| 天然防 kill -9（没完成就没标记） | 第一次升级时已有包都没标记（需兼容） |

### 方案 C：安装前清理（当前 ensure_patchelf 的模式推广）

**原理**：每次安装前检查目录是否"看起来完整"（有 bin/ 或特征文件），
不完整就清理重来。不引入任何新文件。

```
resolve_or_install(pkg):
  dir = xpkgs/<pkg>/<ver>/

  if exists(dir):
    if looks_complete(dir):   # 有 bin/ 或 lib/ 或 include/
      return ok
    else:
      log("incomplete install, cleaning up")
      remove_all(dir)

  install(pkg)
```

`looks_complete()` 启发式检查：
- xim 工具链（gcc/llvm）：检查 `bin/` 目录存在且非空
- xim 工具（patchelf/ninja）：检查特定二进制存在
- mcpplibs 库：检查 `include/` 或 `.cppm` 文件存在

**用户体验**：同方案 A/B

| 优点 | 缺点 |
|------|------|
| 不引入新文件（无 .lock、无 .installed） | 启发式检查可能误判（极端情况） |
| 向后兼容（已有安装不受影响） | 不同包类型需要不同的完整性检查 |
| 实现简单 | 无法区分"刚创建目录还没开始下载"和"下载了一半" |

### 方案 D：install 失败时自动清理 + 重试（推荐）

**原理**：不在检查阶段做，而是在 install **失败时**主动清理残留并重试一次。
配合方案 B 的标记文件做完整性验证。

**核心流程**：
```
resolve_or_install(pkg):
  dir = xpkgs/<pkg>/<ver>/
  marker = dir / ".mcpp_ok"

  # 1. 已安装且完整？
  if exists(marker):
    return ok

  # 2. 目录存在但不完整？清理
  if exists(dir):
    log("cleaning incomplete install of {}", pkg)
    remove_all(dir)

  # 3. 安装
  result = xlings_install(pkg)

  if result.ok && exists(dir):
    write(marker)        # 标记完成
    return ok

  # 4. 安装失败 → 清理残留 → 走 fallback
  if exists(dir):
    remove_all(dir)

  # 5. copy fallback
  if copy_from_global(dir):
    write(marker)
    return ok

  return error("install failed, try: mcpp self init --force")
```

**用户体验**：
```bash
# 场景 1：中断后重试 — 自动恢复
$ mcpp toolchain install llvm 20.1.7
 Downloading xim:llvm@20.1.7 [===>        ] 47MB/266MB
^C

$ mcpp toolchain install llvm 20.1.7
    Cleaning incomplete install of llvm@20.1.7
 Downloading xim:llvm@20.1.7 [=====>      ] ...
   Installed llvm@20.1.7

# 场景 2：网络持续失败 — 明确提示
$ mcpp toolchain install llvm 20.1.7
    Cleaning incomplete install of llvm@20.1.7
error: install failed: xlings install of 'xim:llvm@20.1.7' failed (exit 1)
  hint: check network connection and retry, or `mcpp self init --force`
```

| 优点 | 缺点 |
|------|------|
| 自动恢复，用户几乎无感 | 需要引入 `.mcpp_ok` 标记文件 |
| 失败时也清理（不留残留） | — |
| install 失败 + copy fallback 失败才报错 | — |
| 兼容已有安装（无标记 → 检查 bin/ 存在性） | — |
| 统一处理所有包类型 | — |

## 三、对比总结

| 维度 | A: Lock 文件 | B: 完成标记 | C: 启发式检查 | D: 失败清理+标记 |
|------|------------|-----------|-------------|----------------|
| 精准度 | 高 | 高 | 中（启发式） | 高 |
| 新文件 | .installing | .mcpp_installed | 无 | .mcpp_ok |
| 自动恢复 | ✅ | ✅ | ✅ | ✅ |
| kill -9 安全 | ❌（lock 残留） | ✅ | ✅ | ✅ |
| 向后兼容 | ✅ | 需兼容逻辑 | ✅ | 需兼容逻辑 |
| 实现复杂度 | 中 | 低 | 低 | **低** |
| 失败残留清理 | 不处理 | 不处理 | 不处理 | **✅ 主动清理** |
| 统一性 | 统一 | 统一 | 需按类型 | **统一** |

## 四、推荐：方案 D

**方案 D 最优**，理由：

1. **kill -9 安全**：没有 lock 文件残留问题，"没有 .mcpp_ok = 不完整"
2. **失败也清理**：install 失败后主动删除残留，不留定时炸弹
3. **向后兼容**：没有 .mcpp_ok 的已有安装，fallback 到检查 `bin/` 存在性
4. **统一**：所有包类型（toolchain、bootstrap、modular lib）一个机制
5. **用户体验好**：中断后重试自动恢复，持续失败给明确提示

**标记文件名**：`.mcpp_ok`（简短、不与 xlings 冲突、语义明确）

## 五、实施要点

### 5.1 在 package_fetcher.cppm 的 resolve_xpkg_path 中统一处理

```cpp
auto resolve_quick = [&]() -> std::optional<XpkgPayload> {
    if (!std::filesystem::exists(verdir)) return std::nullopt;

    // 完整性检查：.mcpp_ok 标记 或 向后兼容检查
    auto marker = verdir / ".mcpp_ok";
    if (!std::filesystem::exists(marker)) {
        // 兼容：已有安装（升级前）没有标记，检查 bin/ 或 lib/
        bool looksComplete = std::filesystem::exists(verdir / "bin")
                          || std::filesystem::exists(verdir / "include")
                          || std::filesystem::exists(verdir / "lib");
        if (!looksComplete) {
            // 不完整 → 清理
            mcpp::log::verbose("fetcher",
                std::format("cleaning incomplete install: {}", verdir.string()));
            std::error_code ec;
            std::filesystem::remove_all(verdir, ec);
            return std::nullopt;
        }
    }

    // ... 构造 payload
};
```

### 5.2 安装成功后写标记

```cpp
// install 成功后
if (auto p = resolve_quick()) {
    // 写入完成标记
    auto marker = verdir / ".mcpp_ok";
    if (!std::filesystem::exists(marker)) {
        std::ofstream(marker) << "1";
    }
    return *p;
}
```

### 5.3 install 失败后清理残留

```cpp
// install 失败后
if (inst && inst->exitCode != 0) {
    // 清理可能的残留目录
    if (std::filesystem::exists(verdir)) {
        mcpp::log::verbose("fetcher", "cleaning failed install residue");
        std::error_code ec;
        std::filesystem::remove_all(verdir, ec);
    }
    mcpp::log::warn("fetcher", ...);
}
```

### 5.4 copy fallback 后也写标记

```cpp
// copy_from_global 成功后
if (auto p = resolve_quick()) {
    auto marker = verdir / ".mcpp_ok";
    if (!std::filesystem::exists(marker))
        std::ofstream(marker) << "1";
    return *p;
}
```

### 5.5 mcpp self init 的增强

`mcpp self init` 扫描所有 xpkgs 目录，清理没有 `.mcpp_ok` 且不完整的：

```cpp
for (auto& pkg : directory_iterator(xpkgsBase)) {
    for (auto& ver : directory_iterator(pkg)) {
        auto marker = ver / ".mcpp_ok";
        if (exists(marker)) continue;
        bool complete = exists(ver / "bin") || exists(ver / "include") || exists(ver / "lib");
        if (!complete) {
            log("cleaning incomplete: {}", ver);
            remove_all(ver);
        }
    }
}
```
