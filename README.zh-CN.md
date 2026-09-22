# mcpp

> 以模块为先的现代 C++ 构建工具。mcpp 完全由 C++23 模块写成，并已实现自举。

[English](README.md) | **简体中文**

[![Release](https://img.shields.io/github/v/release/mcpp-community/mcpp)](https://github.com/mcpp-community/mcpp/releases)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-ok-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

| [文档](docs/zh/) · [快速开始](docs/zh/01-getting-started.md) · [mcpp.toml 指南](docs/zh/04-mcpp-toml.md) · [示例项目](docs/zh/03-examples.md) · [工具链管理](docs/zh/20-toolchains.md) |
|:---:|
| [包索引 mcpp-index](https://mcpplibs.github.io/mcpp-index/) · [模块化库 mcpplibs](https://github.com/mcpplibs) · [社区论坛](https://forum.d2learn.org/category/20) · [Issues](https://github.com/mcpp-community/mcpp/issues) · [Releases](https://github.com/mcpp-community/mcpp/releases) |
| [![ci-linux](https://github.com/mcpp-community/mcpp/actions/workflows/ci-linux.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-linux.yml) [![ci-macos](https://github.com/mcpp-community/mcpp/actions/workflows/ci-macos.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-macos.yml) [![ci-windows](https://github.com/mcpp-community/mcpp/actions/workflows/ci-windows.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-windows.yml) |
| 支持的插件 · [mcpp-language-server（mcppls）](https://github.com/Sunrisepeak/mcpp-language-server) —— C++20/23 模块语言服务器，面向 VS Code、Zed、CLion、Neovim、AI Agent（MCP）与 CI |

<p align="center">
  <img src="https://github.com/user-attachments/assets/6c85896e-9a37-4f62-acfb-d37a4eae2363" alt="mcpp demo" width="720">
</p>

## 核心特性

- **模块化构建系统**：以 C++ 模块为先。`import std` 自动处理，文件级增量构建，模块依赖自动分析，无需配置
- **构建插件与异构硬件**：`build.mcpp` 与规则包扩展构建；CUDA、HIP、SYCL、Vulkan/SPIR-V 与 Ascend C 各对应一个规则包
- **包管理与模块化库生态**：SemVer 约束、锁文件、跨项目 BMI 缓存、自定义索引；[mcpplibs](https://github.com/mcpplibs) 中的库在 `mcpp.toml` 中加两行即可 `import`
- **工具链管理与交叉编译**：`family@version` 按需安装；`--target` 把同一次构建切换到 Windows、macOS、Cortex-M 或 RISC-V 裸机，一份源码经 openkal 可构建到多个带操作系统的目标
- **环境与运行时**：由 xlings 提供用户态环境。工具链与依赖留在隔离的沙盒中，runner 把产物送到开发板或模拟器上运行
- **纯模块化自举**：mcpp 完全由 C++23 模块接口单元写成，并由它自己构建

## mcpp 的定位

mcpp 专为 **C++23 模块化开发**设计。需要使用 `import std`、模块接口单元（`.cppm`）、模块分区等现代 C++ 特性的项目，在 Linux、macOS ARM64 与 Windows x86_64 上都能得到顺畅、友好的开发体验。

C++ 通常把下面五项工作交给五个工具，mcpp 用一个命令完成全部五项。第二行列出每一列通常对应的工具。

| mcpp | 构建系统 | 构建插件 | 包管理 | 工具链管理 | 环境与运行时 |
|---|---|---|---|---|---|
| **最接近的工具** | CMake + Ninja | CMake modules、xmake rules | vcpkg、Conan | rustup、nvm | conda、Nix |

> [!NOTE]
> **早期版本**：mcpp 仍在积极开发中，接口与行为可能在后续版本中调整。
> 欢迎对现代 C++ 模块化构建工具感兴趣的开发者[参与贡献](#参与贡献)。
> 问题、反馈与想法可以在 [issues](https://github.com/mcpp-community/mcpp/issues) 中提出。

## 快速开始

### 安装

**使用 xlings 安装**（推荐）

```bash
xlings install mcpp -y
```

<details>
<summary>尚未安装 xlings：点击查看安装命令</summary>

**Linux / macOS**
```bash
curl -fsSL https://d2learn.org/xlings-install.sh | bash
```

**Windows — PowerShell**
```powershell
irm https://d2learn.org/xlings-install.ps1.txt | iex
```

> xlings 的更多信息见 [xlings.d2learn.org](https://xlings.d2learn.org)

</details>

<details>
<summary>可选：短命令（<code>mp</code>、<code>mbuild</code>、<code>mrun</code> 等）</summary>

```bash
xlings install mcpp-short-cmd -y
```

该包注册 30 个 shim，`mcpp build` 即可写成 `mbuild`。命名规则：除最后一个词外，每个词取首字母，最后一个词保留全拼，例如 `mcpp self doctor` → `msdoctor`。`mp` 即不带子命令的 `mcpp`。这些短命令指向 `mcpp` 这个 shim，而不是某个固定的二进制，因此 `xlings use mcpp <ver>` 会同时切换它们。

| 短命令 | 展开 | 短命令 | 展开 |
| --- | --- | --- | --- |
| `mp` | `mcpp` | `mexpkg` | `mcpp emit xpkg` |
| `mnew` | `mcpp new` | `mxparse` | `mcpp xpkg parse` |
| `mbuild` | `mcpp build` | `mtinstall` | `mcpp toolchain install` |
| `mrun` | `mcpp run` | `mtlist` | `mcpp toolchain list` |
| `mtest` | `mcpp test` | `mtdefault` | `mcpp toolchain default` |
| `mclean` | `mcpp clean` | `mcdir` | `mcpp cache dir` |
| `madd` | `mcpp add` | `mclist` | `mcpp cache list` |
| `mremove` | `mcpp remove` | `mcinfo` | `mcpp cache info` |
| `mupdate` | `mcpp update` | `mcgc` | `mcpp cache gc` |
| `msearch` | `mcpp search` | `milist` | `mcpp index list` |
| `mpublish` | `mcpp publish` | `miadd` | `mcpp index add` |
| `mpack` | `mcpp pack` | `miremove` | `mcpp index remove` |
| `msdoctor` | `mcpp self doctor` | `miupdate` | `mcpp index update` |
| `msenv` | `mcpp self env` | `msconfig` | `mcpp self config` |
| `msversion` | `mcpp self version` | `msexplain` | `mcpp self explain` |

</details>

**其他安装方式**

<details>
<summary><b>方式 1</b>：一键安装脚本（Linux x86_64/aarch64、macOS ARM64）</summary>

```bash
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

该脚本不支持 Windows，Windows 请使用上文 PowerShell 的 xlings 安装方式。脚本安装到 `~/.mcpp/`，并把它加入 shell 的 PATH。删除 `~/.mcpp` 即完成卸载。

</details>

<details>
<summary><b>方式 2</b>：Homebrew（macOS / Linux）</summary>

```bash
brew install mcpp-community/mcpp/mcpp-m
```

这一条命令会 tap [`mcpp-community/homebrew-mcpp`](https://github.com/mcpp-community/homebrew-mcpp)，并安装同一份预编译的 release 二进制。macOS 要求 Apple 芯片与 macOS 14；每个用户的数据仍在各自的 `~/.mcpp/` 中。

Homebrew 中的 `mcpp` 是一个无关的 C 预处理器，因此 formula 名为 `mcpp-m`，安装出的命令仍是 `mcpp`。

**Homebrew 6 对第三方 tap 设置了信任门。** 上面的全限定命令被视为显式意图，可以直接使用；其他写法，包括短名 `brew install mcpp-m`、`mcpp` 别名以及之后的升级，都会被拒绝：

```
Refusing to load formula mcpp-community/mcpp/mcpp-m from untrusted tap
mcpp-community/mcpp.
```

信任该 tap 一次之后，上述写法均可使用：

```bash
brew trust mcpp-community/mcpp
```

</details>

<details>
<summary><b>方式 3</b>：Arch Linux（AUR）</summary>

```bash
yay -S mcpp-bin      # 预编译 release 二进制
yay -S mcpp-m        # 或源码构建（用 mcpp-bin 自举）
```

`mcpp` 命令安装到系统级位置，每个用户的数据仍在各自的 `~/.mcpp/` 中。Arch 上 `mcpp` 这个名字属于一个无关的 C 预处理器，因此包名为 `mcpp-bin` / `mcpp-m`（见 [`scripts/aur/`](scripts/aur/)）。稳定版 release 的自动同步只管理 `mcpp-bin`；`mcpp-m` 与 `mcpp-git` 仍由人工维护，版本可能有意滞后。

</details>

<details>
<summary><b>方式 4</b>：由 AI 助手安装</summary>

将以下提示词发给 AI 编码助手（Claude Code、Cursor、Copilot 等）：

```
阅读 https://github.com/mcpp-community/mcpp 的 README，
安装 mcpp，创建一个 C++23 模块项目，然后构建并运行它。
仓库中的 .agents/skills/mcpp-usage/SKILL.md 提供了详细的使用指南。
```

</details>

### 创建、构建与运行项目

```bash
mcpp new hello
cd hello
mcpp build
mcpp run
```

> 首次构建会初始化环境并获取工具链，耗时较长。

### 项目结构

```
hello/
├── mcpp.toml             ← 工程描述
├── src/
│   └── main.cpp          ← import std; 直接可用
└── tests/
    └── test_smoke.cpp    ← `mcpp test` 自动发现
```

```toml
# mcpp.toml
[package]
name        = "hello"
version     = "0.1.0"
description = "A modular C++23 package"
license     = "Apache-2.0"
```

内置脚手架依赖约定，不生成 `[targets.hello]`：mcpp 由 `src/main.cpp` 推断出 binary target，`mcpp test` 自动发现 `tests/test_smoke.cpp`。

### 使用模块化库

在 `mcpp.toml` 中添加两行依赖，即可引入 [mcpplibs](https://github.com/mcpplibs) 社区的模块化库：

```toml
[dependencies]
cmdline = "0.0.2"
```

然后在代码中直接 `import`：

```cpp
import mcpplibs.cmdline;
```

> 其他依赖写法（版本约束、命名空间、Git 引用、本地路径等）见 [mcpp.toml 指南 —— 依赖管理](docs/zh/04-mcpp-toml.md)。

## 功能概览

<details>
<summary><b>构建系统</b></summary>

- 原生支持 C++20/23/26 模块（接口单元、实现单元、模块分区），另有 `c++latest` / `c++fly` 两种实验模式
- `import std` / `import std.compat` 全自动预编译与缓存
- 三层增量优化：前端脏检查 + 逐文件 P1689 dyndep + BMI copy-if-different restat
- 带指纹的 BMI 缓存：按编译器、编译标志与标准库计算哈希，跨项目共享
- Ninja 后端：自动生成 build.ninja，并行编译
- 自动生成 `compile_commands.json`（可直接供 clangd / ccls 使用）；`mcpp build --configure-only` 可在编译普通源码之前刷新它
- C 语言一等支持：自动识别 `.c` 文件，支持 C/C++ 混合项目
- 用户自定义 cflags / cxxflags / ldflags / c_standard

</details>

<details>
<summary><b>工具链管理</b></summary>

- 内置 GCC 16.1.0 与 LLVM/Clang 20.1.7，一条命令安装
- 按宿主选择默认值：Linux x86_64 使用原生 glibc GCC，其他 Linux 架构使用 musl GCC，macOS 以及有可用 MSVC 的 Windows 使用 LLVM，裸 Windows 使用 MinGW-w64 GCC
- 多版本共存：`mcpp toolchain install gcc 16` / `mcpp toolchain install llvm 20`
- 隔离沙盒：所有工具链位于 `~/.mcpp/registry/`，不改动系统
- 按平台指定：`linux = "gcc@16"`、`macos = "llvm@20"`
- GCC 与 Clang 的编译管线对等（由 `BmiTraits` 抽象层驱动）

</details>

<details>
<summary><b>交叉编译、裸机与设备</b></summary>

- `mcpp build --target <triple>`：一个选项；该目标所需的工具链 payload 自动解析并安装
- 目标从 `x86_64-linux-gnu` 覆盖到 Cortex-M、Cortex-A 与 RISC-V 裸机，完整列表见[平台支持](#平台支持)
- freestanding 目标不带操作系统：C 库、启动代码、内存布局与模拟器由板级支持包提供，而不由 mcpp 提供
- runner 用于运行构建机上无法运行的产物：`mcpp run --runner flash`、`--list-runners`、`mcpp why runners`
- `mcpp new --template riscv-virt-rt`：按名字实例化某个包自带的板级模板
- 基于 openkal 的交叉编译：一个可移植程序，可以为内核接口与 C 库均由包提供的目标构建

</details>

<details>
<summary><b>异构构建与加速器</b></summary>

- `[build] accel = "cuda12.9+{sm_89}, vulkan1.2"`：一次构建可以指定一个或多个设备后端，该构建中 `cfg(accelerator = "cuda")` 为真
- 目前有规则包的编程模型共五个：CUDA、HIP、SYCL、Vulkan/SPIR-V 与 Ascend C
- 设备翻译单元由各自的编译器编译，产物进入普通链接；主机与设备之间的边界代码由 mcpp 生成，不需要写两遍
- 用带约束的 glob 选择设备源码：`{ glob = "src/kernels/**/*.cu", accel = "cuda12.9+{sm_89}" }`
- 引擎中不包含任何厂商名，因此第六个后端是一个包，而不是一次引擎改动

</details>

<details>
<summary><b>包管理与依赖</b></summary>

- SemVer 约束解析：`^`、`~`、范围、精确版本
- 三级解析：约束合并 → 多版本 mangling 回退 → 精确匹配
- 锁文件 mcpp.lock（v2 格式：索引快照 + 命名空间）
- 命名空间：`[dependencies.myteam] foo = "1.0"`
- 自定义包索引：`[indices] acme = "git@..."` / `{ path = "..." }`
- 项目级索引隔离（`.mcpp/` 目录，不影响全局状态）
- 依赖来源：索引 / Git / 本地路径

</details>

<details>
<summary><b>工作空间</b></summary>

- `[workspace] members = ["libs/*", "apps/*"]`
- 统一的锁文件与 target 目录
- 版本集中管理：`[workspace.dependencies]` + `.workspace = true`
- 选择性构建：`mcpp build -p member-name`
- 配置继承：工具链、构建标志与索引从根级联到各成员

</details>

<details>
<summary><b>打包与发布</b></summary>

- `mcpp pack`：四种 Linux 发布模式，即 system / vendored（默认）/ self-contained / static；`bundle-project` 与 `bundle-all` 保留为兼容别名
- musl 全静态二进制：单文件分发，不依赖 glibc（目标须为对应架构的 Linux x86_64 或 aarch64）
- `mcpp publish`：生成 xpkg.lua 并发布到包索引
- 通过 patchelf 自动修正 RPATH（Linux）

</details>

<details>
<summary><b>扩展构建</b></summary>

- `build.mcpp`：为 mcpp 没有现成规则的构建步骤编写的构建程序，它与 mcpp 通过一套带版本号的指令协议通信，而不是由 mcpp 猜测其行为
- `mcpp::action` 以显式的输入与输出声明一项工作，生成的文件因此进入增量构建图，而不是游离在图外
- 规则包把这类步骤提供给其他项目：包声明一个 rule 模块，使用方以 feature 的形式选用它
- payload、运行时适配包与板级支持包都是普通的包：工具、驱动或开发板，都由安装库的同一个解析器安装

</details>

<details>
<summary><b>开发体验</b></summary>

- `mcpp new`：创建模块化项目；`--template [ns.]name[@version][:tname]` 与 `mcpp add` 使用同一种精确身份写法，选用**包自带的模板**。包中只有一个模板时，即使未写 `default = true`，它也是默认模板；存在歧义时用 `--list-templates [ns.]name[@version]` 列出可选模板
- `mcpp run [-- args]`：构建并运行
- `mcpp test [pattern] [-- args]`：自动发现并运行测试（按名字过滤；`--list`、`--timeout <s>`、`--message-format json`）
- `mcpp search`：搜索包索引
- `mcpp add / remove / update`：依赖管理
- 命令行上的 profile 与 feature：`build` 与 `run` 接受 `--release` / `--profile <name>`，`build`、`run` 与 `test` 接受 `--features <list>`
- `mcpp why [toolchain|runtime|deps|runners]`：解释已解析的构建决策；`--format json` 供程序读取
- `mcpp emit sbom`：为刚记录的那次解析生成 CycloneDX 格式的物料清单（SBOM）
- `mcpp --offline` / `MCPP_OFFLINE=1`：只使用本地已有的状态
- `mcpp explain E0001`：错误码的详细解释
- `mcpp self doctor`：环境自诊断

</details>

## 性能对比

测量对象是构建 **mcpp 自身**：锁定的工作负载有 137 个模块接口单元、57k 行代码，每个单元都 `import std;`。四个构建引擎使用**同一个编译器二进制**。每格数据是 **3 次采样的中位数**，以及相对 cmake 的倍率。所有列来自**同一次运行**。

<!-- columns: mcpp=mcpp@2026.8.13.1; mcpp +优化=mcpp@2026.8.13.1+schedule=on; mcpp (旧版)=mcpp@2026.8.11.3; cmake=cmake; xmake=xmake -->
| 场景 | `mcpp` | `mcpp +优化` | `mcpp (旧版)` | `cmake` | `xmake` |
|---|---|---|---|---|---|
| `cold` | 86.69s · 1.1x | **35.73s · 2.6x** | 86.75s · 1.1x | 91.74s · 1.0x | 90.54s · 1.0x |
| `noop` | **0.16s · 2.0x** | 0.18s · 1.8x | 0.24s · 1.3x | 0.32s · 1.0x | 0.38s · 0.8x |
| `touch-hub` | **0.42s · 197.7x** | 0.42s · 197.2x | 81.72s · 1.0x | 83.21s · 1.0x | 82.48s · 1.0x |
| `edit-body` | 80.87s · 1.1x | **29.83s · 2.9x** | 81.19s · 1.1x | 85.30s · 1.0x | 84.33s · 1.0x |
| `edit-comment` | **0.40s · 207.0x** | **0.40s · 207.0x** | 79.11s · 1.1x | 83.21s · 1.0x | 82.15s · 1.0x |

<sub>`cold` 尚未构建过 · `noop` 没有任何改动 · `touch-hub` 只更新 mtime，内容不变 · `edit-body` 在函数体内做一次真实修改 · `edit-comment` 在 hub 接口中添加一行注释。<br>
<br>
`mcpp` = mcpp@2026.8.13.1，即被测版本 · `mcpp +优化` = **与 `mcpp` 同一个二进制**，开启 opt-in 的 `[build] bmi_schedule = "on"`（默认关闭）· `mcpp (旧版)` = mcpp@2026.8.11.3，即上一个已发布版本。<br>
Linux x86_64 · i9-13900K · gcc 16.1.0 · n=3 · 锁定的工作负载 `a749e9f` ·
cmake 4.4.2 / xmake 3.1.0 · 所有大于 1s 的中位数，其 min/max 与中位数相差不超过 4% ·
数据：[`standard-20260814-linux-x86_64`](bench/results/standard-20260814-linux-x86_64/)。</sub>

* **`touch-hub` 与 `edit-comment` 两行的差异来自级联抑制。**
  cmake 与 xmake 按时间戳判断，重新编译全部下游单元；mcpp 把编译器刚产出的 BMI 与上一份比较，接口未变时跳过级联。这是默认行为，不需要配置。`mcpp (旧版)` 一列测得上一个发布版为 81.72s，与 cmake 处于同一量级，因此这一效果是本版本新增的。
* **`edit-body` 是对照行，mcpp 在这一行有意不快。**
  扰动在接口单元中插入一条语句，GCC 记录的源码位置随之移动，BMI 因此改变，每个导入者都需要重新构建。在这一行上快的引擎，是跳过了本应完成的工作。一次修改是否引起级联，取决于函数体写在哪里：原地等长的修改，或写在独立 `.cpp` 中的函数体，都不引起级联，结果落在约 200x 的那一档。实测见 [`.agents/docs/2026-08-15-module-edit-granularity.md`](.agents/docs/2026-08-15-module-edit-granularity.md)。
* **`bmi_schedule` 为 opt-in，默认关闭**（`auto` 解析为 off）。
  它把代码生成移出关键路径，因此只在确实需要级联时才有收益：`cold` 86.69s → 35.73s，`edit-body` 80.87s → 29.83s；在 mcpp 本已跳过级联的两行上没有收益。调度出错时的表现是静默失效而不是报错，因此默认值不会依据单台机器的数据改变。

**[方法、锁定的版本与完整数据 → `bench/README.zh-CN.md`](bench/README.zh-CN.md)** ·
[English](bench/README.md)

## 平台支持

mcpp 的身份模型有两条正交的轴：**工具链**是 `family@version`（family ∈ gcc | llvm | msvc），**目标**是三段式 triple `arch-os[-env]`。交叉编译只需 `mcpp build --target <triple>`，对应的工具链 payload 会自动解析并安装。`mcpp toolchain list` 显示本机的实时状态。

**宿主**（mcpp 本身运行的平台）：Linux x86_64 / aarch64、macOS arm64、Windows x86_64。

**目标**（`--target` 接受的值；表中的行及其档位取自 `modules/toolchain-model/src/triple.cppm`，`mcpp toolchain list` 为本机报告的也是这一份）：

| Target | 约定工具链 | 档位 |
|---|---|:---:|
| `x86_64-linux-gnu`    | gcc（*Linux 默认*）或 llvm | verified |
| `x86_64-linux-musl`   | gcc 16，全静态 | verified |
| `aarch64-linux-musl`  | gcc 16，全静态；从 x86_64 交叉编译（qemu）或原生构建 | verified |
| `x86_64-windows-gnu`  | gcc 16 MinGW-w64；Windows 上原生构建，Linux 上交叉编译（wine）（*无 Visual Studio 时的 Windows 默认*） | verified |
| `x86_64-windows-msvc` | `msvc@system`（探测 VS/BuildTools）或 llvm ¹（*有 Visual Studio 时的 Windows 默认*） | verified |
| `x86_64-windows-musl` | llvm 22；带 musl C 库的 PE，gcc 无法产出；系统部分由依赖图提供 | preview |
| `aarch64-macos`       | llvm（*macOS 默认*） | verified |
| `riscv64-none-elf` · `riscv32-none-elf` | llvm 22；裸机，`xim:picolibc-riscv` ² | verified |
| `thumbv6m-none-eabi` · `thumbv7m-none-eabi` | llvm 22；Cortex-M0/M0+/M1、Cortex-M3 ² | verified |
| `thumbv7em-none-eabihf` · `thumbv8m.main-none-eabi` | llvm 22；Cortex-M4F/M7F 硬浮点，Cortex-M33/M55 软浮点 ² | verified |
| `armv7a-none-eabi` · `armv7a-none-eabihf` | llvm 22；Cortex-A 32 位，第一个带 MMU 的目标 ² | verified |
| `aarch64-none-elf` · `x86_64-none-elf` | llvm 22；裸机，默认不带 C 库 ² | preview |
| `thumbv7em-none-eabi` · `thumbv8m.base-none-eabi` · `thumbv8m.main-none-eabihf` | llvm 22；Cortex-M4/M7 软浮点、M23、M33F/M55F ² | preview |
| `riscv64-linux-musl` · `aarch64-linux-gnu` · `x86_64-macos` | — | planned |
| `wasm32-emscripten` | `emsdk@6.0.9`；Emscripten 自带 sysroot 与 libc++ 模块接口；`mcpp run` 使用 payload 声明的 `node`（`xim:node`）运行模块，不使用 PATH 上的 `node` | verified |
| `x86_64-linux-android` | `android-ndk@30.0.16248370`；bionic 来自 NDK，一个 payload 服务两个 ABI；已在 API 24 的 x86_64 模拟器镜像上运行 | verified |
| `aarch64-linux-android` | 同一个 payload、同样的构建；已在 qemu-user 上配合系统镜像自带的 bionic 运行，平台模拟器无法在 x86_64 宿主上做到这一点 | verified |
| `aarch64-ios-sim` | llvm 22 加上本机的 iPhoneSimulator SDK（mcpp 定位该 SDK，不安装它）；已通过 `simctl-run` 在模拟器上运行 ³ | verified |
| `aarch64-ios` | 真机采用同样的分工；产物标记为 iOS 平台，在设备上运行需要开发者自己的签名 ³ | preview |
| `x86_64-ios-sim` | 同一次构建；尚未运行过，因为模拟器运行宿主的架构，而测量所用的机器是 Apple 芯片 ³ | preview |

`verified`：该行的镜像已构建**并运行**过，qemu 与 wine 均计入 · `preview`：可以构建和链接，尚无模拟器运行记录 · `planned`：已在词表中登记，尚未接通。针对这类目标的构建会被拒绝，而不会被尝试。

> Linux release 二进制是 x86_64 与 aarch64 的 musl 全静态构建（`x86_64-linux-musl` 与 `aarch64-linux-musl`）。
> 旧写法（`x86_64-w64-mingw32`、`gcc@16.1.0-musl`、`mingw-cross@…`、`musl-gcc@…`）作为别名**永久接受**，并归一化为上表中的规范形式。
>
> ¹ Windows 上 llvm 使用 MSVC ABI，因此需要已安装的 **MSVC BuildTools 或 Visual Studio**（UCRT、Windows SDK、MSVC STL）。这一点无需手动处理：mcpp 首次运行时探测是否有可用的 MSVC，探测不到就默认使用 `x86_64-windows-gnu`（winlibs MinGW-w64）。该工具链完全自包含，不需要 Visual Studio，并支持 `import std`。在未做任何配置的 Windows 上，`mcpp new && mcpp build` 可直接使用。`mcpp.toml` 中显式写出的 `[toolchain]` 始终按原样生效：mcpp 只修正它自己选择的默认值，不改动用户的设置。
>
> ² 裸机各行不带操作系统。clang 与 lld 本身就是交叉编译器，因此任何能安装 LLVM payload 的宿主都能产出这些目标。C 库、启动代码、内存布局与模拟器由板级支持包提供，而不由 mcpp 提供，见 [40 —— 裸机与 freestanding 目标](docs/zh/40-baremetal.md)。
>
> ³ 三个 iOS 目标需要 macOS 宿主，编译器仍来自生态：`xim:llvm` 能为 iOS 部署目标产出 arm64 Mach-O。本机提供的是 SDK。SDK 位于 Xcode 中且不可再分发，因此 mcpp 通过 `xcrun` 定位它，方式与定位 macOS SDK 相同；找不到时，mcpp 报出该 SDK 的名字并拒绝构建。模拟器会话由 `xim:apple-simulator-tools` 提供。见 [20 —— 工具链管理](docs/zh/20-toolchains.md)。

## 文档

[`docs/zh/`](docs/zh/README.md) 是使用手册。章节号的第一位数字表示它所属的部分；索引另附一份反查表，可以从一个 manifest 键或一条命令查到负责它的章节。

| 部分 | 入口章节 |
|---|---|
| `0x` 基础 | [01 快速开始](docs/zh/01-getting-started.md) · [04 mcpp.toml 工程文件指南](docs/zh/04-mcpp-toml.md) · [09 按场景选命令](docs/zh/09-commands-by-scenario.md) |
| `1x` 发布 | [10 发布打包](docs/zh/10-pack-and-release.md) · [11 发布一个库到 mcpp-index](docs/zh/11-publishing-a-library.md) · [12 分发预编译库](docs/zh/12-binary-distribution.md) |
| `2x` 工具链与目标 | [20 工具链管理](docs/zh/20-toolchains.md) · [21 目标三元组](docs/zh/21-the-target-triple.md) · [24 基于 openkal 的交叉构建](docs/zh/24-openkal-cross.md) |
| `3x` 扩展 mcpp | [30 构建程序：`build.mcpp`](docs/zh/30-build-mcpp.md) · [31 编写规则包](docs/zh/31-authoring-a-rule-package.md) · [34 编写板级支持包](docs/zh/34-authoring-a-bsp.md) |
| `4x` 设备与加速器 | [40 裸机与 freestanding 目标](docs/zh/40-baremetal.md) · [41 在设备上运行](docs/zh/41-devices.md) · [42 异构硬件构建](docs/zh/42-heterogeneous-builds.md) |
| `5x` 面向程序的契约 | [50 机器可读输出](docs/zh/50-machine-output.md) · [51 受支持的版本与兼容性](docs/zh/51-supported-versions.md) · [规范](docs/specs/README.md) |
| `9x` mcpp 自身 | [90 从源码构建与参与贡献](docs/zh/90-build-from-source.md) · [92 发布 mcpp](docs/zh/92-release.md) |

[`examples/`](examples/) 下的每个目录都是一个可以构建的项目，[03 —— 示例项目](docs/zh/03-examples.md) 说明每个示例演示什么。任意命令的完整选项可通过 `mcpp <cmd> --help` 查看。

**AI 辅助学习**：将以下提示词发给 AI 编码助手，可以快速了解 mcpp：

```
阅读 https://github.com/mcpp-community/mcpp 仓库的
.agents/skills/mcpp-usage/SKILL.md 和 docs/ 目录下的文档，
说明如何用 mcpp 创建一个带依赖的 C++23 模块项目。
```

## 使用 mcpp 的项目

以下是用 mcpp 构建的实际项目，包括可直接 `import` 的 C++23 模块，以及 mcpp 所依赖的工具链底座：

| 项目 | 说明 |
| --- | --- |
| [mcpp](https://github.com/mcpp-community/mcpp) | mcpp 自身，由 C++23 模块写成，完全自举 |
| [xlings](https://github.com/openxlings/xlings) | mcpp 所依赖的工具链与包管理底座 |
| [tinyhttps](https://github.com/mcpplibs/tinyhttps) | 极简的 C++23 HTTP/HTTPS 客户端，支持 SSE 流式传输 |
| [llmapi](https://github.com/mcpplibs/llmapi) | 现代 C++ LLM API 客户端（兼容 OpenAI） |
| [imgui-m](https://github.com/mcpplibs/imgui-m) | 以 C++23 模块包形式提供的 Dear ImGui |
| [cmdline](https://github.com/mcpplibs/cmdline) | 命令行解析库 / 框架（mcpp 自身使用） |

更多模块化库见 [mcpplibs](https://github.com/mcpplibs) · 包索引见 [mcpp-index](https://mcpplibs.github.io/mcpp-index/)

## 参与贡献

欢迎通过 Issue 与 PR 参与开发。项目接受借助 AI Agent 完成的贡献。

**基本流程**

1. 创建 Issue：Bug 修复、新功能或改进，先在 [issues](https://github.com/mcpp-community/mcpp/issues) 中发起讨论
2. 实现改动：Fork 仓库并创建分支，按改动范围验证（行为改动运行 `mcpp build` 与相关测试；纯文档改动核对示例与链接）
3. 提交 PR：使用 `gh pr create`，并确保 CI 通过
4. CI 必须通过：CI 未通过的 PR 不会被合入

**提交信息规范**：使用 `feat:` / `fix:` / `test:` / `docs:` / `refactor:` 前缀

**AI Agent 贡献**：仓库中的 [`.agents/skills/mcpp-contributing/SKILL.md`](.agents/skills/mcpp-contributing/SKILL.md) 给出完整的 Agent 贡献流程与项目结构说明。将以下提示词发给 AI 助手即可：

```
阅读 https://github.com/mcpp-community/mcpp 仓库的
.agents/skills/mcpp-contributing/SKILL.md，
按照其中的指南为 mcpp 项目提交一个贡献。
```

## 社区 & 生态

- [社区论坛](https://forum.d2learn.org/category/20)：交流群（QQ：1067245099）
- [mcpp-index](https://mcpplibs.github.io/mcpp-index/)：默认包索引
- [mcpplibs](https://github.com/mcpplibs)：模块化 C++ 库集合

### 致谢

项目依赖与灵感来源：

- [xlings](https://github.com/openxlings/xlings)：工具链 / 包管理底座
- [mcpplibs.cmdline](https://github.com/mcpplibs/cmdline)：CLI 框架
- [ninja](https://github.com/ninja-build/ninja)：底层构建引擎
- [xmake](https://github.com/xmake-io/xmake)：跨平台构建工具
- [cargo](https://github.com/rust-lang/cargo)：Rust 包管理器
