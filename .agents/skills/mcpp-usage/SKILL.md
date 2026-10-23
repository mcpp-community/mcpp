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
| `mcpp test [pattern] [-- args]` | 发现并运行 `tests/**/*.cpp` 测试 |
| `mcpp add <pkg>[@ver]` | 添加依赖 |
| `mcpp remove <pkg>` | 移除依赖 |
| `mcpp update [pkg]` | 更新依赖 |
| `mcpp search <keyword>` | 搜索包 |
| `mcpp toolchain list` | 查看工具链 |
| `mcpp toolchain install gcc 16` | 安装工具链 |
| `mcpp pack` | 打包 |
| `mcpp why [toolchain｜runtime｜deps]` | 解释解析出的构建决策 |
| `mcpp --offline` | 只使用已有本地状态 |
| `mcpp self doctor` | 环境诊断 |
| `mcpp explain <CODE>` | 错误码解释 |

## 安装

```bash
# 推荐；Windows 请在 PowerShell 中运行
xlings install mcpp -y

# 或 Unix release 一键脚本（仅 Linux x86_64/aarch64 与 macOS ARM64）
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

一键脚本不支持 Windows；Windows 使用 PowerShell 的 xlings 安装命令。安装到
`~/.mcpp/` 后会自动加入 PATH。首次使用时，mcpp 按宿主选择默认工具链并
安装到隔离沙盒：Linux 通常为 GCC，macOS 为 LLVM，Windows 在有可用 MSVC
时为 LLVM，否则为面向 `x86_64-windows-gnu` 的 MinGW-w64 GCC。

## 创建项目

```bash
mcpp new hello && cd hello
mcpp build
mcpp run
```

生成的项目包含最小 manifest 和可立即运行的 smoke test：

```toml
[package]
name = "hello"
version = "0.1.0"
description = "A modular C++23 package"
license = "Apache-2.0"
```

`src/main.cpp` 会自动推断为 binary target，`tests/test_smoke.cpp` 会由
`mcpp test` 自动发现；无需手写 `[targets.hello]`。

## mcpp.toml 配置

### 传统头文件库的模块化封装

`mcpplibs.mwrap.<库名>` 为传统 C/C++ 库提供模块入口；API namespace 保留上游，例如
`import mcpplibs.mwrap.opencv.core;` 后仍使用 `cv::Mat`。包版本对齐上游，封装修订最多追加一个数字尾号。
具体已发布的版本/feature 先查索引，不把文档中的候选库当已发布包。

每个新建或重构的 -m 库提供包自带快捷模板：

```bash
mcpp new vision --template mcpplibs.mwrap.opencv@5.0.0.1
mcpp new --list-templates mcpplibs.mwrap.opencv@5.0.0.1
```

模板语法为 `[namespace.]name[@version][:template]`，只有一个默认模板时可以省略模板名。
包模板中的 `{{self.name}}` 是完整包选择器，应用 `"{{self.name}}"` 作为单个依赖 key，
不要放到另一层 namespace 表下。模板必须实际创建、构建、运行，而不只做字符串检查。

feature 是加法并集；消费者可在依赖声明中使用 `default-features = false` 关闭该边的默认集合，
显式 features 仍会启用，其他依赖边的请求不会被取消。根项目没有 `--no-default-features` 命令行选项。
可选大依赖由包作者显式门控。模块可导入不等于实际后端已启用，窗口/推理/设备能力需分别验证。
宏不能经 import 传递；`.macros` 只提供可保持语义的常量/函数映射，原始宏仍需兼容头。

维护规范与包装流程见 [mcpp-index mwrap 规范](https://github.com/mcpplibs/mcpp-index/blob/main/docs/mwrap.md)。
修改普通消费者项目时按现有包契约使用，不自动替用户迁移其他依赖。

```toml
[package]
name = "myapp"
version = "0.1.0"

[targets.myapp]
kind = "bin"                # bin / lib / shared; tests are discovered from tests/**/*.cpp
main = "src/main.cpp"

[dev-dependencies]
gtest = "1.15.2"            # 仅测试使用；SemVer: ^, ~, 范围, 精确

[toolchain]
default = "gcc@16.1.0"
```

更多配置：https://github.com/mcpp-community/mcpp/blob/main/docs/04-mcpp-toml.md

## 工具链

```bash
mcpp toolchain list                   # 查看已装
mcpp toolchain install gcc 16         # 装 GCC 16
mcpp toolchain install llvm 20        # 装 LLVM 20
mcpp toolchain default gcc@16.1.0    # 设默认
mcpp build --target x86_64-linux-musl # 需要全静态 Linux 产物时显式选择
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
| command not found | 重开终端。Unix release 安装脚本应确认 `~/.mcpp/bin` 在当前 shell 的 `PATH` 中；经 xlings 安装则确认 xlings 当前激活的 bin 目录。Windows 不要执行 `source`，重开 PowerShell 后用 `Get-Command mcpp.exe` 验证命令已激活。 |
| 编译错误 | `mcpp clean && mcpp build`，确认 `mcpp toolchain list` |
| 依赖找不到 | `mcpp index update`，确认 `mcpp search <name>` |
| 需要无网络构建 | 使用 `mcpp --offline` 或设置 `MCPP_OFFLINE=1`；缺失的工具链/依赖会直接报错 |

## 问题反馈

1. **项目 Issue**：https://github.com/mcpp-community/mcpp/issues
   - 描述复现步骤、期望/实际行为、mcpp 版本和 OS
   - 可用 `gh issue create` 提交

2. **社区论坛**：https://forum.d2learn.org/category/20
   - 使用疑问和讨论

如果 AI Agent 无法直接创建 Issue，请提示用户手动创建并提供整理好的问题描述。
