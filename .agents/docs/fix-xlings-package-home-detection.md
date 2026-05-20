# Fix: xlings 包内 mcpp 的 MCPP_HOME 检测

## 问题

`xlings install mcpp` 安装的 mcpp 二进制位于 `~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/bin/mcpp`。
`home_dir()` 检测到 `bin/` 父目录后，将祖父目录作为 MCPP_HOME，导致嵌套沙箱：

```
MCPP_HOME = ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/
  → registry/ = ~/.xlings/data/xpkgs/xim-x-mcpp/0.0.20/registry/
    → XLINGS_HOME = 同上
      → 工具链安装到 registry/data/xpkgs/xim-x-llvm/20.1.7/  ← 嵌套失败
```

## 修复

在 `home_dir()` 的自包含模式检测中，增加 xlings 包路径排除：如果祖先目录中存在
`data/xpkgs` 模式，说明在 xlings 包内，fallback 到 `~/.mcpp/`。

同时提取 `default_mcpp_home()` 统一三平台的 HOME fallback（Windows 用 %USERPROFILE%）。

## 改动

仅 `src/config.cppm` 的 `home_dir()` 函数，约 20 行。
