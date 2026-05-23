# resolve_xpkg_path() 的 copy 优先级问题分析

**Date**: 2026-05-22

## 一、当前流程

`resolve_xpkg_path()` (`src/pm/package_fetcher.cppm:580-718`) 的执行顺序：

```
resolve_xpkg_path(target, autoInstall)
│
├─ resolve() ← 第一次调用
│   ├─ sandbox 里有？→ 直接返回 ✅
│   ├─ sandbox 里没有？→ 检查 ~/.xlings/
│   │   ├─ ~/.xlings/ 里有？→ copy 到 sandbox → 返回 ✅
│   │   └─ ~/.xlings/ 里没有？→ 返回 error
│   └─ 返回 error
│
├─ resolve() 成功？→ return（不会触发 install）
│
├─ autoInstall=false？→ return error
│
├─ install()  ← 只有 resolve() 失败且 autoInstall=true 才走到这里
│   └─ xlings interface install_packages
│
└─ resolve() ← 第二次调用（install 后再 resolve）
    └─ 同上逻辑（sandbox → copy → error）
```

## 二、问题：copy 短路了 install

**核心问题**：只要 `~/.xlings/` 里有这个包，`resolve()` 就会直接 copy 并返回成功，
**永远不会走到 `install()` 路径**。

### 场景 1：用户之前用系统 xlings 装过 LLVM

```
~/.xlings/data/xpkgs/xim-x-llvm/20.1.7/  ← 存在（旧版本）
~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/  ← 不存在

resolve():
  sandbox 没有 → 检查 ~/.xlings/ → 有 → copy → 返回成功
  ↑ 完全跳过 install，即使 ~/.xlings/ 里的版本可能有问题
```

**后果**：
- mcpp 拿到的是 xlings 全局环境的旧包，可能跟 mcpp sandbox 不兼容
- ELF RUNPATH 指向 `~/.xlings/...`（这就是 libatomic bug 的根源）
- mcpp 无法确保拿到的包是用 `XLINGS_HOME=~/.mcpp/registry` 安装的

### 场景 2：全局也没有，需要全新安装

```
~/.xlings/data/xpkgs/xim-x-llvm/20.1.7/  ← 不存在
~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/  ← 不存在

resolve():
  sandbox 没有 → 检查 ~/.xlings/ → 也没有 → 返回 error

install():
  xlings interface install_packages → exitCode=0
  但 LLVM 实际没装到 sandbox（xlings bug）
  也没装到 ~/.xlings/（安装可能不完整）

resolve()（第二次）:
  sandbox 没有 → ~/.xlings/ 也没有 → 返回 "xpkg payload missing"
```

**后果**：全新安装完全失败（就是你遇到的情况）

### 场景 3：sandbox 里已有

```
~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/  ← 存在

resolve():
  sandbox 有 → 直接返回成功
  ↑ 不检查版本、完整性、RUNPATH 正确性
```

**后果**：如果之前拷贝的包有问题（比如 RUNPATH 错误），不会自动修复

## 三、问题分层

| 层 | 问题 | 严重度 |
|----|------|--------|
| **优先级反转** | copy 优先于 install，导致 install 路径几乎不被执行 | 高 |
| **来源不可信** | 从 `~/.xlings/` 拷贝的包不是为 mcpp sandbox 构建的 | 高 |
| **无完整性检查** | copy 后不验证包是否完整、路径是否正确 | 中 |
| **install 路径不可靠** | xlings NDJSON interface 安装大包时返回成功但未实际安装 | 高 |
| **无版本/时间戳校验** | 不检查 `~/.xlings/` 的包是否比 sandbox 的更新 | 低 |

## 四、理想的执行流程

```
resolve_xpkg_path(target, autoInstall)
│
├─ 1. sandbox 里有且完整？→ 直接返回 ✅
│
├─ 2. autoInstall?
│   ├─ 是 → install()（用 XLINGS_HOME=sandbox 安装到 sandbox）
│   │   ├─ 成功且 sandbox 里有？→ 返回 ✅
│   │   └─ 失败 → 走 fallback
│   └─ 否 → 走 fallback
│
├─ 3. fallback: ~/.xlings/ 里有？
│   ├─ 是 → copy + post-copy fixup → 返回 ✅
│   └─ 否 → 返回 error
│
└─ 4. 返回结果
```

关键变化：**install 优先于 copy**。copy 只是 fallback，不是首选路径。

## 五、修复方案

### 方案 A：调换 install 和 copy 的优先级

将 `resolve()` 中的 copy workaround 移到 `install()` 之后：

```
resolve_xpkg_path(target, autoInstall):
  1. check sandbox → return if exists
  2. if autoInstall → install via xlings
  3. check sandbox again → return if exists
  4. FALLBACK: copy from ~/.xlings/ (workaround)
  5. check sandbox again → return if exists
  6. error: payload missing
```

**优点**：install 路径得到优先执行，copy 只是最后兜底
**缺点**：如果 install 慢或失败，用户体验变差（之前可以秒拷贝）

### 方案 B：install 优先 + copy fallback + 超时

```
resolve_xpkg_path(target, autoInstall):
  1. check sandbox → return if exists
  2. if autoInstall → try install (with timeout)
  3. check sandbox → return if exists
  4. copy from ~/.xlings/ if available
  5. post-copy fixup (patchelf RUNPATH)
  6. return or error
```

**优点**：兼顾速度（install 失败时快速 fallback）和正确性
**缺点**：增加超时逻辑的复杂度

### 方案 C：install 优先 + install 直接调用（非 NDJSON）

之前排查发现 NDJSON interface 路径安装大包不可靠。`install_with_progress()`
已有"直接调用" fallback（`std::system("xlings install ... -y")`）。

将工具链安装改为使用 `install_with_progress()`（直接调用模式）而非
`install()`（NDJSON interface 模式）：

```
resolve_xpkg_path(target, autoInstall):
  1. check sandbox → return if exists
  2. if autoInstall → install_with_progress (direct mode)
  3. check sandbox → return if exists
  4. copy from ~/.xlings/ as fallback
  5. return or error
```

**优点**：
- 修复了 NDJSON interface 安装大包不可靠的问题
- install 正确执行时，包直接装到 sandbox，无需 copy
- copy 只在 install 真正失败时兜底

**缺点**：需要在 package_fetcher 层引入 install_with_progress

### 方案 D：保持 copy 优先但增加 post-copy fixup（当前状态）

当前 PR #67 的做法：保持 copy 优先，但在工具链 post-install 时修正 RUNPATH。

**优点**：改动最小，已实施
**缺点**：
- copy 仍然优先于 install，install 路径几乎不被测试
- 依赖 `~/.xlings/` 有正确的包（全新机器无 `~/.xlings/` 则完全失败）
- 每个工具链都需要写对应的 fixup

## 六、建议

**短期（已完成）**：方案 D — post-copy fixup 兜底

**中期（推荐）**：方案 C — install 优先 + 直接调用模式
- 修改 `resolve_xpkg_path()` 的流程顺序
- 工具链安装使用 `install_with_progress()`（直接调用）
- copy 降级为 fallback
- 这是最务实的方案，解决了优先级反转和 NDJSON 不可靠两个问题

**长期**：方案 C + 在 copy fallback 后统一做 RUNPATH fixup
- 将 patchelf fixup 从各工具链的 post-install 提取到 copy 出口统一处理
- 未来加新工具链不会再遗漏
