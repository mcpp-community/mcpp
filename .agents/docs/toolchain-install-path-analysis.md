# 工具链安装路径问题分析

## 问题描述

通过 xlings 安装的 mcpp (`xlings install mcpp`) 执行 `mcpp toolchain install llvm` 时报错：

```
Installing llvm 20.1.7 via mcpp's xlings
Downloading xim:libxml2@2.13.5
error: install failed: xpkg payload missing:
  /home/speak/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry/data/xpkgs/xim-x-llvm/20.1.7
```

## 路径推导链

mcpp 二进制位于 xlings 包目录中：

```
~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/bin/mcpp
```

### 1. MCPP_HOME 自动检测 (`config.cppm:home_dir()`)

用户未设置 `$MCPP_HOME` 环境变量，触发自动检测：
- 发现 `exe.parent_path().filename() == "bin"` ✓
- 无 `"target"` 祖先目录 ✓
- 返回 `exe.parent_path().parent_path()`

```
MCPP_HOME = ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/
```

### 2. Registry 路径计算 (`config.cppm:load_or_init()`)

```
registryDir   = MCPP_HOME / "registry"
              = ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry/

xlingsHome()  = registryDir  (无 override)
              = ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry/

xlingsBinary  = registryDir / "bin" / "xlings"
              = ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry/bin/xlings
```

### 3. xlings 命令构建 (`xlings.cppm:build_command_prefix()`)

```bash
cd ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry \
  && XLINGS_HOME=~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry \
     ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry/bin/xlings \
     interface install_packages --args '{"targets":["xim:llvm@20.1.7"]}'
```

### 4. xlings 安装位置

xlings 按照 `$XLINGS_HOME` 安装包：

```
$XLINGS_HOME/data/xpkgs/xim-x-llvm/20.1.7/
= ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry/data/xpkgs/xim-x-llvm/20.1.7/
```

### 5. mcpp 包查找位置 (`package_fetcher.cppm:resolve_xpkg_path()`)

```cpp
auto base = cfg_.xlingsHome() / "data" / "xpkgs";
// = ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry/data/xpkgs/

auto verdir = base / "xim-x-llvm" / "20.1.7";
// = ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry/data/xpkgs/xim-x-llvm/20.1.7/
```

路径一致——**mcpp 查找的路径和 xlings 安装的路径是同一个位置**。

## 根因分析

问题不是路径不一致，而是 **xlings 的 registry/bin/xlings 二进制不存在或 xlings 安装到了其他位置**。

当 mcpp 通过 xlings 安装时，形成了**嵌套沙箱**结构：

```
~/.xlings/                                    ← 全局 xlings 沙箱
  └── data/xpkgs/xim-x-mcpp/0.0.20/          ← mcpp 包目录 = MCPP_HOME
      ├── bin/mcpp                            ← mcpp 二进制
      └── registry/                           ← mcpp 的私有 xlings 沙箱
          ├── bin/xlings                      ← 应该有 vendored xlings（可能缺失）
          ├── subos/default/bin/              ← 应该有 sandbox bin（可能缺失）
          └── data/xpkgs/                     ← 工具链包应该安装到这里
              └── xim-x-llvm/20.1.7/         ← 目标位置
```

**可能失败的节点**：

1. **`registry/bin/xlings` 不存在**：mcpp 首次运行时的 `acquire_xlings_binary()` 复制 xlings 二进制到 registry/bin/。如果全局 xlings 不在 PATH 中或复制失败，xlings 二进制缺失
2. **`xlings self init` 未成功执行**：sandbox 目录结构未创建（`subos/default/` 等），导致后续命令失败
3. **xlings 安装到了全局目录**而非 XLINGS_HOME 指定的路径：xlings 的某些版本可能忽略 `XLINGS_HOME` 环境变量，始终安装到 `~/.xlings/`

## 为什么 CI 没有这个问题

CI 显式设置了 `MCPP_HOME`：

```yaml
env:
  MCPP_HOME: /home/runner/.mcpp
```

这绕过了 `home_dir()` 的自动检测，使用独立的 `~/.mcpp/` 作为 MCPP_HOME，不会产生嵌套沙箱问题。

## 修复方案

### 方案 A：检测 xlings 包内运行，使用全局 xlings（推荐）

在 `home_dir()` 中增加 xlings 包路径检测：

```cpp
std::filesystem::path home_dir() {
    if (auto* e = std::getenv("MCPP_HOME"); e && *e)
        return std::filesystem::path(e);

    auto exe = mcpp::platform::fs::self_exe_path();
    if (exe.parent_path().filename() == "bin") {
        auto candidate = exe.parent_path().parent_path();
        
        // 检测是否在 xlings 包目录中运行
        // 路径模式: ~/.xlings/data/xpkgs/xim-x-mcpp/<version>/bin/mcpp
        // 或:       ~/.xlings/subos/default/data/xpkgs/xim-x-mcpp/<version>/bin/mcpp
        bool isXlingsPackage = false;
        for (auto p = candidate; p.has_parent_path() && p != p.root_path(); p = p.parent_path()) {
            if (p.filename() == "xpkgs" && p.parent_path().filename() == "data") {
                isXlingsPackage = true;
                break;
            }
        }
        
        if (isXlingsPackage) {
            // 在 xlings 包内：使用 ~/.mcpp 作为独立 home
            // 避免嵌套沙箱问题
            if (auto* e = std::getenv("HOME"); e && *e)
                return std::filesystem::path(e) / ".mcpp";
        }
        
        // 非 xlings 包内：保持原逻辑（自包含模式）
        bool isDevPath = false;
        for (auto p = exe.parent_path(); ...; p = p.parent_path()) {
            if (p.filename() == "target") { isDevPath = true; break; }
        }
        if (!isDevPath)
            return candidate;
    }

    if (auto* e = std::getenv("HOME"); e && *e)
        return std::filesystem::path(e) / ".mcpp";
    return std::filesystem::current_path() / ".mcpp";
}
```

**优点**：
- 自动处理，用户无需任何配置
- xlings 安装的 mcpp 使用独立的 `~/.mcpp/` 沙箱
- release tarball 安装的 mcpp 保持自包含模式
- CI 不受影响（显式 MCPP_HOME 优先）

**缺点**：
- 需要硬编码 xpkgs 路径模式检测

### 方案 B：使用全局 xlings 而非嵌套沙箱

修改 `make_xlings_env()` 和 `xlingsHome()`，当检测到 xlings 包内运行时，直接使用全局 xlings 的 XLINGS_HOME（`~/.xlings/`）：

```cpp
mcpp::xlings::Env make_xlings_env(const GlobalConfig& cfg) {
    // 如果 mcpp 在 xlings 包内，使用全局 xlings
    if (is_xlings_installed(cfg.mcppHome)) {
        auto globalXlings = find_global_xlings();  // ~/.xlings/subos/default/bin/xlings
        auto globalHome = find_global_xlings_home();  // ~/.xlings/
        return { globalXlings, globalHome };
    }
    return { cfg.xlingsBinary, cfg.xlingsHome() };
}
```

**优点**：
- 复用全局 xlings 沙箱，避免重复下载工具链
- 概念上更简洁

**缺点**：
- mcpp 的 sandbox 与全局 xlings 耦合
- 多版本 mcpp 可能冲突

### 方案 C：文档/提示（最小改动）

当检测到 xlings 包内运行且 sandbox 初始化失败时，提示用户设置 `MCPP_HOME`：

```
hint: mcpp detected it's running from a xlings package.
      Set MCPP_HOME=~/.mcpp to use an independent sandbox:
        export MCPP_HOME=~/.mcpp
```

## 推荐

**方案 A**：改动最小、用户体验最好、兼容性最强。

核心逻辑：如果 mcpp 二进制位于 `.xlings/data/xpkgs/xim-x-mcpp/` 路径下，说明是 xlings 安装的分发版，应使用 `~/.mcpp/` 作为独立 HOME，而不是在 xlings 包内创建嵌套沙箱。

## 受影响文件

| 文件 | 改动 |
|------|------|
| `src/config.cppm` | `home_dir()` 增加 xlings 包路径检测 |
| 无其他改动 | 路径推导链后续自动生效 |
