# mcpp

> 一个 **现代C++** 构建工具 - *`模块化 + 依赖/工具链管理 + 包索引 + 打包发布`*

[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-ok-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

`mcpp` 使用 C++23 完全模块化的工程结构进行开发, 且已经初步完成自举 (使用工具本身从源码构建出自己). 具备 项目构建、依赖自动处理、工具链管理、构建产物打包导出、模块化包索引等基础功能.

> [!CAUTION]
> **初步开发中（WIP）** — 工具任然处于探索阶段。
> 接口、命令、行为可能在后续版本调整。问题 / 反馈 / 想法欢迎在
> [issues](https://github.com/mcpp-community/mcpp/issues) 留言。

## 快速开始

### 安装

<details>
  <summary>点击获取xlings安装命令</summary>

---

#### Linux/MacOS

```bash
curl -fsSL https://d2learn.org/xlings-install.sh | bash
```

#### Windows - PowerShell

```bash
irm https://d2learn.org/xlings-install.ps1.txt | iex
```

> tips: xlings -> [details](https://xlings.d2learn.org)

---

</details>

**`方法一: 使用xlings包管理器进行安装(推荐)`**

```bash
xlings install mcpp -y
```

**`方法二: 使用一键安装命令`**

```bash
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

注: 会装到 `~/.mcpp/`，自动加进 shell PATH。删除 `~/.mcpp` 即可卸载。

### 创建项目 & 构建运行

```bash
mcpp new hello
cd hello
mcpp build
mcpp run
```

注: 初次构建需要初始化环境并获取工具链, 可能需要一些时间

## 文档

- [快速开始](docs/00-getting-started.md)
- [示例项目](docs/01-examples.md)
- [发布打包](docs/02-pack-and-release.md)
- [工具链管理](docs/03-toolchains.md)
- [从源码构建 & 参与贡献](docs/04-build-from-source.md)
- [mcpp.toml 工程文件指南](docs/05-mcpp-toml.md)
- [工作空间 (Workspace)](docs/06-workspace.md)

任意命令的完整选项可通过 `mcpp <cmd> --help` 查阅。

## 平台支持

OS × 工具链矩阵：

| OS / arch        | gcc (glibc) | gcc (musl) | clang / llvm | msvc |
|------------------|:-----------:|:----------:|:------------:|:----:|
| Linux x86_64     | ✅ | ✅ *默认* | 🔄 | — |
| Linux aarch64    | 🔄 | 🔄 | 🔄 | — |
| macOS x86_64     | — | — | 🔄 | — |
| macOS aarch64    | — | — | 🔄 | — |
| Windows x86_64   | — | — | 🔄 | 🔄 |

图例：✅ 已支持 ｜ 🔄 计划中 

> *默认*：v0.0.1 release 二进制走 musl 全静态路径，Linux x86_64 可以直接运行。

## 社区 & 生态

- [社区论坛](https://forum.d2learn.org/category/20) - 交流群(Q: 1067245099)
- [mcpp-index](https://github.com/mcpp-community/mcpp-index) — 默认包索引
- [mcpplibs](https://github.com/mcpplibs) - 一个"模块化"的现代C++库集合

项目依赖的和部分有灵感启发来源的工具和库

  - [xlings](https://github.com/d2learn/xlings) — 工具链 / 包管理底座
  - [mcpplibs.cmdline](https://github.com/mcpplibs/cmdline) — mcpp 自身用的 CLI 框架
  - [ninja](https://github.com/ninja-build/ninja) — a small build system with a focus on speed
  - [xmake](https://github.com/xmake-io/xmake) — A cross-platform build utility based on Lua
  - [cargo](https://github.com/rust-lang/cargo) — The Rust package manager