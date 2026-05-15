# mcpp

> 一个 现代C++ 模块化构建工具 — 纯 C++23 模块编写，已实现自举

[![Release](https://img.shields.io/github/v/release/mcpp-community/mcpp)](https://github.com/mcpp-community/mcpp/releases)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Self-hosted](https://img.shields.io/badge/build-self--hosted-brightgreen)]()
[![Module](https://img.shields.io/badge/module-ok-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

## 核心特性

- **C++23 模块原生支持** — `import std` 自动处理，文件级增量构建，模块依赖自动分析，零手动配置
- **纯模块化自举** — mcpp 自身由 43+ 个 C++23 模块组成，用自己构建自己，模块系统经实战验证
- **开箱即用** — 一条命令安装，内置 GCC 16 / LLVM 20 工具链，自动下载到隔离沙盒，不污染系统
- **集成依赖管理** — SemVer 约束解析、锁文件、跨项目 BMI 缓存、自定义包索引
- **多包工作空间** — Workspace 统一锁文件与版本管理，适合大型项目

## 为什么选择 mcpp

mcpp 专门为 **C++23 模块化开发** 打造。如果你想在项目中使用 `import std`、模块接口单元（`.cppm`）、模块分区等现代 C++ 特性，mcpp 在 Linux 上能为你提供便捷且友好的开发体验：

- **默认模块化** — `mcpp new` 创建的项目模板直接使用 C++23 模块，`import std` 开箱即用
- **文件级增量构建** — 基于 P1689 dyndep 的三层优化（前端脏检查 + 逐文件扫描 + BMI restat），只重编真正变化的模块
- **一键创建 & 构建** — `mcpp new hello && cd hello && mcpp build`，工具链自动安装，无需手动配置编译器和构建系统
- **模块化生态** — [mcpplibs](https://github.com/mcpplibs) 提供一系列可直接 `import` 的 C++ 模块化库，支持自定义包索引

> [!NOTE]
> **早期版本** — mcpp 仍在积极开发中，接口和行为可能在后续版本调整。
> 问题 / 反馈 / 想法欢迎在 [issues](https://github.com/mcpp-community/mcpp/issues) 留言。

## 快速开始

### 安装

**方式一：使用 xlings 安装（推荐）**

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

**方式二：一键安装脚本**

```bash
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

安装到 `~/.mcpp/`，自动加进 shell PATH。删除 `~/.mcpp` 即可干净卸载。

**方式三：让 AI 助手帮你安装**

将以下提示词复制给你的 AI 编码助手（Claude Code / Cursor / Copilot 等）：

```
阅读 https://github.com/mcpp-community/mcpp 的 README，
帮我安装 mcpp 并创建一个 C++23 模块项目，构建并运行。
项目的 .agents/skills/ 目录下有更多使用指南供参考。
```

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
- 用户自定义 cflags / cxxflags / c_standard

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

- `mcpp pack`：三种模式 — static（musl全静态）/ bundle-project / bundle-all
- musl 全静态二进制：单文件可分发，无 glibc 依赖
- `mcpp publish`：生成 xpkg.lua + 发布到包索引
- 自动 patchelf 修正 RPATH

</details>

<details>
<summary><b>开发体验</b></summary>

- `mcpp new` — 创建模块化项目模板
- `mcpp run [-- args]` — 构建并运行
- `mcpp test [-- args]` — 自动发现并运行测试
- `mcpp search` — 搜索包索引
- `mcpp add / remove / update` — 依赖管理
- `mcpp explain E0001` — 错误码详细解释
- `mcpp self doctor` — 环境自诊断

</details>

## 平台支持

| OS / arch        | GCC (glibc) | GCC (musl) | Clang / LLVM | MSVC |
|------------------|:-----------:|:----------:|:------------:|:----:|
| Linux x86_64     | ✅ | ✅ *默认* | ✅ | — |
| Linux aarch64    | 🔄 | 🔄 | 🔄 | — |
| macOS            | — | — | 🔄 | — |
| Windows          | — | — | 🔄 | 🔄 |

✅ 已支持 ｜ 🔄 计划中

> *默认*：release 二进制走 musl 全静态，Linux x86_64 可直接运行，无 glibc 依赖。

## 文档

- [快速开始](docs/00-getting-started.md) — 5 分钟完成 install → new → build → run
- [示例项目](docs/01-examples.md)
- [发布打包](docs/02-pack-and-release.md)
- [工具链管理](docs/03-toolchains.md)
- [从源码构建](docs/04-build-from-source.md)
- [mcpp.toml 指南](docs/05-mcpp-toml.md)
- [工作空间](docs/06-workspace.md)

任意命令的完整选项可通过 `mcpp <cmd> --help` 查阅。

**AI 辅助学习**：你可以将以下提示词发给 AI 编码助手，让它帮你快速了解 mcpp：

```
阅读 https://github.com/mcpp-community/mcpp 的文档（docs/ 目录），
告诉我如何用 mcpp 创建一个带依赖的 C++23 模块项目。
项目的 .agents/skills/ 目录下有详细的使用指南和贡献指南。
```

## 参与贡献

欢迎通过 Issue 和 PR 参与项目开发。项目接受开发者使用 AI Agent 参与开发与贡献。

**基本流程**

1. 创建 Issue — Bug 修复、新功能、优化等，先在 [issues](https://github.com/mcpp-community/mcpp/issues) 创建讨论
2. 实现改动 — Fork 仓库，创建分支，实现并验证（`mcpp build` + E2E 测试）
3. 提交 PR — 使用 `gh pr create`，确保 CI 通过
4. CI 必须通过 — CI 不通过的 PR 不会被合入

**提交信息规范**：`feat:` / `fix:` / `test:` / `docs:` / `refactor:` 前缀

**AI Agent 贡献指南**：项目的 [`.agents/skills/contributing.md`](.agents/skills/contributing.md) 提供了完整的 Agent 贡献流程、工具使用和项目结构说明。将以下提示词发给 AI 助手即可：

```
阅读 https://github.com/mcpp-community/mcpp 仓库的 .agents/skills/contributing.md，
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
