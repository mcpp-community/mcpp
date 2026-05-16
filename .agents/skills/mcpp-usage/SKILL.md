---
name: mcpp-usage
description: Use when helping users install, configure, or use mcpp — the C++23 modular build tool. Covers installation, project creation, building, dependency management, toolchain management, workspace setup, packaging, diagnostics, and troubleshooting.
---

# mcpp 基础用法

## Overview

mcpp 是一个现代 C++ 模块化构建工具，纯 C++23 模块编写，已实现自举。一条命令安装，`import std` 开箱即用。

- 仓库：https://github.com/mcpp-community/mcpp
- 文档：https://github.com/mcpp-community/mcpp/tree/main/docs
- 包索引：https://github.com/mcpp-community/mcpp-index
- 模块化库：https://github.com/mcpplibs

## Quick Reference

| 命令 | 用途 |
|---|---|
| `mcpp new <name>` | 创建项目 |
| `mcpp build` | 构建 |
| `mcpp run [-- args]` | 构建并运行 |
| `mcpp test [-- args]` | 运行测试 |
| `mcpp add <pkg>[@ver]` | 添加依赖 |
| `mcpp remove <pkg>` | 移除依赖 |
| `mcpp update [pkg]` | 更新依赖 |
| `mcpp search <keyword>` | 搜索包 |
| `mcpp toolchain list` | 查看工具链 |
| `mcpp toolchain install gcc 16` | 安装工具链 |
| `mcpp pack` | 打包 |
| `mcpp self doctor` | 环境诊断 |
| `mcpp explain <CODE>` | 错误码解释 |

## 安装

```bash
# 推荐
xlings install mcpp -y

# 或一键脚本
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

安装到 `~/.mcpp/`，自动加入 PATH。首次运行自动安装 GCC 工具链到隔离沙盒。

## 创建项目

```bash
mcpp new hello && cd hello
mcpp build
mcpp run
```

生成的 `mcpp.toml`：

```toml
[package]
name = "hello"

[targets.hello]
kind = "bin"
main = "src/main.cpp"
```

## mcpp.toml 配置

```toml
[package]
name = "myapp"
version = "0.1.0"

[targets.myapp]
kind = "bin"                # bin / lib / shared / test
main = "src/main.cpp"

[dependencies]
gtest = "1.15.2"            # SemVer: ^, ~, 范围, 精确

[toolchain]
default = "gcc@16.1.0"
```

更多配置：https://github.com/mcpp-community/mcpp/blob/main/docs/05-mcpp-toml.md

## 工具链

```bash
mcpp toolchain list                   # 查看已装
mcpp toolchain install gcc 16         # 装 GCC 16
mcpp toolchain install llvm 20        # 装 LLVM 20
mcpp toolchain default gcc@16.1.0    # 设默认
```

## 工作空间

```toml
[workspace]
members = ["libs/*", "apps/*"]

[workspace.dependencies]
gtest = "1.15.2"
```

```bash
mcpp build -p member-name    # 构建指定成员
```

## 自定义索引

```toml
[indices]
my-index = "git@gitlab.example.com:team/index.git"
local = { path = "/path/to/index" }

[dependencies.my-index]
internal-lib = "1.0.0"
```

## 常见问题

| 问题 | 解决 |
|---|---|
| 首次构建慢 | 正常，需下载工具链。后续使用缓存 |
| command not found | 重启终端或 `source ~/.bashrc` |
| 编译错误 | `mcpp clean && mcpp build`，确认 `mcpp toolchain list` |
| 依赖找不到 | `mcpp index update`，确认 `mcpp search <name>` |

## 问题反馈

1. **项目 Issue**：https://github.com/mcpp-community/mcpp/issues
   - 描述复现步骤、期望/实际行为、mcpp 版本和 OS
   - 可用 `gh issue create` 提交

2. **社区论坛**：https://forum.d2learn.org/category/20
   - 使用疑问和讨论

如果 AI Agent 无法直接创建 Issue，请提示用户手动创建并提供整理好的问题描述。
