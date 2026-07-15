# mcpp

> 一个 现代C++ 模块化构建工具 — 纯 C++23 模块编写，已实现自举

[English](README.md) | **简体中文**

[![Release](https://img.shields.io/github/v/release/mcpp-community/mcpp)](https://github.com/mcpp-community/mcpp/releases)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-ok-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

| [文档](docs/zh/) · [快速开始](docs/zh/00-getting-started.md) · [mcpp.toml 指南](docs/zh/05-mcpp-toml.md) · [示例项目](docs/zh/01-examples.md) · [工具链管理](docs/zh/03-toolchains.md) |
|:---:|
| [包索引 mcpp-index](https://github.com/mcpp-community/mcpp-index) · [模块化库 mcpplibs](https://github.com/mcpplibs) · [社区论坛](https://forum.d2learn.org/category/20) · [Issues](https://github.com/mcpp-community/mcpp/issues) · [Releases](https://github.com/mcpp-community/mcpp/releases) |
| [![ci-linux](https://github.com/mcpp-community/mcpp/actions/workflows/ci-linux.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-linux.yml) [![ci-macos](https://github.com/mcpp-community/mcpp/actions/workflows/ci-macos.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-macos.yml) [![ci-windows](https://github.com/mcpp-community/mcpp/actions/workflows/ci-windows.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-windows.yml) |

<p align="center">
  <img src="https://github.com/user-attachments/assets/6c85896e-9a37-4f62-acfb-d37a4eae2363" alt="mcpp demo" width="720">
</p>

## 核心特性

- **C++23 模块原生支持** — `import std` 自动处理，文件级增量构建，模块依赖自动分析，零手动配置
- **纯模块化自举** — mcpp 自身由 43+ 个 C++23 模块组成，用自己构建自己，模块系统经实战验证
- **开箱即用** — 一条命令安装，内置 GCC 16 / LLVM 20 工具链，自动下载到隔离沙盒，不污染系统
- **集成依赖管理** — SemVer 约束解析、锁文件、跨项目 BMI 缓存、自定义包索引
- **多包工作空间** — Workspace 统一锁文件与版本管理，适合大型项目

## 为什么选择 mcpp

mcpp 专门为 **C++23 模块化开发** 打造。如果你想在项目中使用 `import std`、模块接口单元（`.cppm`）、模块分区等现代 C++ 特性，mcpp 在 Linux 和 macOS ARM64 上能为你提供便捷且友好的开发体验：

- **默认模块化** — `mcpp new` 创建的项目模板直接使用 C++23 模块，`import std` 开箱即用
- **文件级增量构建** — 基于 P1689 dyndep 的三层优化（前端脏检查 + 逐文件扫描 + BMI restat），只重编真正变化的模块
- **一键创建 & 构建** — `mcpp new hello && cd hello && mcpp build`，工具链自动安装，无需手动配置编译器和构建系统
- **模块化生态** — [mcpplibs](https://github.com/mcpplibs) 提供一系列可直接 `import` 的 C++ 模块化库，支持自定义包索引

> [!NOTE]
> **早期版本** — mcpp 仍在积极开发中，接口和行为可能在后续版本调整。
> 欢迎对现代 C++ 模块化构建工具感兴趣的开发者[参与贡献](#参与贡献)。
> 问题 / 反馈 / 想法欢迎在 [issues](https://github.com/mcpp-community/mcpp/issues) 留言。

## 快速开始

### 安装

**使用 xlings 安装**（推荐）

```bash
xlings install mcpp -y
```

<details>
<summary>还没有 xlings？点击查看安装命令</summary>

**Linux / macOS**
```bash
curl -fsSL https://d2learn.org/xlings-install.sh | bash
```

**Windows — PowerShell**
```powershell
irm https://d2learn.org/xlings-install.ps1.txt | iex
```

> xlings 详情 → [xlings.d2learn.org](https://xlings.d2learn.org)

</details>

**其他方式**

<details>
<summary><b>方式 1</b> — 一键安装脚本</summary>

```bash
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

安装到 `~/.mcpp/`，自动加进 shell PATH。删除 `~/.mcpp` 即可干净卸载。

</details>

<details>
<summary><b>方式 2</b> — Arch Linux（AUR）</summary>

```bash
yay -S mcpp-bin      # 预编译 release 二进制
yay -S mcpp-m        # 或源码构建（用 mcpp-bin 自举）
```

系统级安装 `mcpp` 命令，每个用户的数据仍在各自的 `~/.mcpp/`。
Arch 上 `mcpp` 这个名字属于一个无关的 C 预处理器，所以包名是
`mcpp-bin` / `mcpp-m`（详见 [`scripts/aur/`](scripts/aur/)）。

</details>

<details>
<summary><b>方式 3</b> — 让 AI 助手帮你安装</summary>

将以下提示词复制给你的 AI 编码助手（Claude Code / Cursor / Copilot 等）：

```
阅读 https://github.com/mcpp-community/mcpp 的 README，
帮我安装 mcpp 并创建一个 C++23 模块项目，构建并运行。
项目的 .agents/skills/mcpp-usage/SKILL.md 有详细的使用指南。
```

</details>

### 创建项目 & 构建运行

```bash
mcpp new hello
cd hello
mcpp build
mcpp run
```

> 注：首次构建会初始化环境并获取工具链，可能需要一些时间。

### 项目结构

```
hello/
├── mcpp.toml             ← 工程描述
└── src/
    └── main.cpp          ← import std; 直接可用
```

```toml
# mcpp.toml
[package]
name = "hello"

[targets.hello]
kind = "bin"
main = "src/main.cpp"
```

### 使用模块化库

在 `mcpp.toml` 中添加两行依赖，即可引用 [mcpplibs](https://github.com/mcpplibs) 社区模块化库：

```toml
[dependencies]
cmdline = "0.0.2"
```

然后在代码中直接 `import`：

```cpp
import mcpplibs.cmdline;
```

> 更多依赖配置方式（版本约束、命名空间、Git 引用、本地路径等）参见 [mcpp.toml 指南 — 依赖管理](docs/zh/05-mcpp-toml.md)。

## 功能概览

<details>
<summary><b>构建系统</b></summary>

- C++20/23 模块原生支持（接口单元、实现单元、模块分区）
- `import std` / `import std.compat` 全自动预编译与缓存
- 三层增量优化：前端脏检查 + 逐文件 P1689 dyndep + BMI copy-if-different restat
- 指纹化 BMI 缓存：按编译器/标志/标准库哈希，跨项目共享
- Ninja 后端：自动生成 build.ninja，并行编译
- compile_commands.json 自动生成（clangd / ccls 即用）
- C 语言一等支持：`.c` 文件自动检测，混合 C/C++ 项目
- 用户自定义 cflags / cxxflags / ldflags / c_standard

</details>

<details>
<summary><b>工具链管理</b></summary>

- 内置 GCC 16.1.0 + LLVM/Clang 20.1.7，一键安装
- musl-gcc 全静态工具链（默认）
- 多版本共存：`mcpp toolchain install gcc 16` / `mcpp toolchain install llvm 20`
- 隔离沙盒：所有工具链在 `~/.mcpp/registry/`，不影响系统
- 按平台指定：`linux = "gcc@16"`, `macos = "llvm@20"`
- GCC + Clang 编译管线平权（`BmiTraits` 抽象层驱动）

</details>

<details>
<summary><b>包管理与依赖</b></summary>

- SemVer 约束解析：`^`、`~`、范围、精确版本
- 三级解析：约束合并 → 多版本 mangling 回退 → 精确匹配
- 锁文件 mcpp.lock（v2 格式：索引快照 + 命名空间）
- 命名空间系统：`[dependencies.myteam] foo = "1.0"`
- 自定义包索引：`[indices] acme = "git@..."` / `{ path = "..." }`
- 项目级索引隔离（`.mcpp/` 目录，不污染全局）
- 依赖来源：索引 / Git / 本地路径

</details>

<details>
<summary><b>工作空间</b></summary>

- `[workspace] members = ["libs/*", "apps/*"]`
- 统一锁文件 + 统一 target 目录
- 版本集中管理：`[workspace.dependencies]` + `.workspace = true`
- 选择性构建：`mcpp build -p member-name`
- 配置继承：工具链、构建标志、索引从根级联到成员

</details>

<details>
<summary><b>打包与发布</b></summary>

- `mcpp pack`：三种 Linux 发布模式 — static（musl全静态）/ bundle-project / bundle-all
- musl 全静态二进制：单文件可分发，无 glibc 依赖（Linux x86_64）
- `mcpp publish`：生成 xpkg.lua + 发布到包索引
- 自动 patchelf 修正 RPATH（Linux）

</details>

<details>
<summary><b>开发体验</b></summary>

- `mcpp new` — 创建模块化项目;`--template <pkg>[@ver][:<tmpl>]` 使用**库自带模板**(如 `--template imgui`),`--list-templates <pkg>` 列举
- `mcpp run [-- args]` — 构建并运行
- `mcpp test [-- args]` — 自动发现并运行测试
- `mcpp search` — 搜索包索引
- `mcpp add / remove / update` — 依赖管理
- `mcpp explain E0001` — 错误码详细解释
- `mcpp self doctor` — 环境自诊断

</details>

## 平台支持

mcpp 的身份模型是两条正交轴:**工具链** = `family@version`(family ∈ gcc | llvm | msvc),
**目标** = 三段 triple `arch-os[-env]`。交叉编译只需 `mcpp build --target <triple>`——
对应的工具链包会自动解析并安装。`mcpp toolchain list` 查看本机实时状态。

**宿主**(mcpp 本身运行在哪):Linux x86_64 / aarch64、macOS arm64、Windows x86_64。

**目标**(`--target` 接受什么;本表与代码内词汇表同源):

| Target | 约定工具链 | 状态 |
|---|---|:---:|
| `x86_64-linux-gnu`    | gcc(*Linux 默认*)或 llvm | ✅ |
| `x86_64-linux-musl`   | gcc 16,全静态 | ✅ |
| `aarch64-linux-musl`  | gcc 16,全静态——x86_64 交叉(qemu 实测)或原生 | ✅ |
| `x86_64-windows-gnu`  | gcc 16 MinGW-w64——Windows 原生,Linux 交叉(wine 实测) | ✅ |
| `x86_64-windows-msvc` | `msvc@system`(探测 VS/BuildTools)或 llvm ¹(*Windows 默认*) | ✅ |
| `aarch64-macos`       | llvm(*macOS 默认*) | ✅ |
| `riscv64-linux-musl`  | — | 🔄 |
| `aarch64-linux-gnu`   | — | 🔄 |
| `x86_64-macos`        | — | 🔄 |

✅ 已验证——CI 端到端构建**并真实执行**产物(含 qemu/wine)｜ 🔄 计划中

> Linux release 二进制为 musl 全静态构建(`x86_64-linux-musl`)。
> 旧拼写——`x86_64-w64-mingw32`、`gcc@16.1.0-musl`、`mingw-cross@…`、`musl-gcc@…`——
> 作为别名**永久接受**,归一到上表的 canonical 形式。
>
> ¹ Windows 上 llvm 依赖已安装的 **MSVC BuildTools 或 Visual Studio**(UCRT、Windows
> SDK、MSVC STL)。MinGW 路线(`--target x86_64-windows-gnu`,或
> `mcpp toolchain default gcc@16 --target x86_64-windows-gnu`)完全不需要 Visual Studio。

## 文档

- [快速开始](docs/zh/00-getting-started.md) — 5 分钟完成 install → new → build → run
- [示例项目](docs/zh/01-examples.md)
- [发布打包](docs/zh/02-pack-and-release.md)
- [工具链管理](docs/zh/03-toolchains.md)
- [从源码构建](docs/zh/04-build-from-source.md)
- [mcpp.toml 指南](docs/zh/05-mcpp-toml.md)
- [工作空间](docs/zh/06-workspace.md)

任意命令的完整选项可通过 `mcpp <cmd> --help` 查阅。

**AI 辅助学习**：你可以将以下提示词发给 AI 编码助手，让它帮你快速了解 mcpp：

```
阅读 https://github.com/mcpp-community/mcpp 仓库的
.agents/skills/mcpp-usage/SKILL.md 和 docs/ 目录下的文档，
告诉我如何用 mcpp 创建一个带依赖的 C++23 模块项目。
```

## 谁在使用 mcpp

用 mcpp 构建的真实项目 —— 可直接 `import` 的 C++23 模块,以及它所依赖的工具链:

| 项目 | 说明 |
| --- | --- |
| [mcpp](https://github.com/mcpp-community/mcpp) | mcpp 自身 —— 43+ 个 C++23 模块,完全自举 |
| [xlings](https://github.com/openxlings/xlings) | mcpp 依赖的工具链与包管理底座 |
| [tinyhttps](https://github.com/mcpplibs/tinyhttps) | 极简 C++23 HTTP/HTTPS 客户端,支持 SSE 流式 |
| [llmapi](https://github.com/mcpplibs/llmapi) | 现代 C++ LLM API 客户端(OpenAI 兼容) |
| [imgui-m](https://github.com/mcpplibs/imgui-m) | Dear ImGui 的 C++23 模块封装包 |
| [cmdline](https://github.com/mcpplibs/cmdline) | 命令行解析库/框架(mcpp 自身在用) |

更多模块化库 → [mcpplibs](https://github.com/mcpplibs) · 包索引 → [mcpp-index](https://github.com/mcpp-community/mcpp-index)

## 参与贡献

欢迎通过 Issue 和 PR 参与项目开发。项目接受开发者使用 AI Agent 参与开发与贡献。

**基本流程**

1. 创建 Issue — Bug 修复、新功能、优化等，先在 [issues](https://github.com/mcpp-community/mcpp/issues) 创建讨论
2. 实现改动 — Fork 仓库，创建分支，实现并验证（`mcpp build` + E2E 测试）
3. 提交 PR — 使用 `gh pr create`，确保 CI 通过
4. CI 必须通过 — CI 不通过的 PR 不会被合入

**提交信息规范**：`feat:` / `fix:` / `test:` / `docs:` / `refactor:` 前缀

**AI Agent 贡献**：项目的 [`.agents/skills/mcpp-contributing/SKILL.md`](.agents/skills/mcpp-contributing/SKILL.md) 提供了完整的 Agent 贡献流程和项目结构说明。将以下提示词发给 AI 助手即可：

```
阅读 https://github.com/mcpp-community/mcpp 仓库的
.agents/skills/mcpp-contributing/SKILL.md，
按照指南帮我给 mcpp 项目提交一个贡献。
```

## 社区 & 生态

- [社区论坛](https://forum.d2learn.org/category/20) — 交流群 (Q: 1067245099)
- [mcpp-index](https://github.com/mcpp-community/mcpp-index) — 默认包索引
- [mcpplibs](https://github.com/mcpplibs) — 模块化 C++ 库集合

### 致谢

项目依赖和灵感来源：

- [xlings](https://github.com/d2learn/xlings) — 工具链 / 包管理底座
- [mcpplibs.cmdline](https://github.com/mcpplibs/cmdline) — CLI 框架
- [ninja](https://github.com/ninja-build/ninja) — 底层构建引擎
- [xmake](https://github.com/xmake-io/xmake) — 跨平台构建工具
- [cargo](https://github.com/rust-lang/cargo) — Rust 包管理器
