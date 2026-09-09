# mcpp

> 一个 现代C++ 模块化构建工具 — 纯 C++23 模块编写，已实现自举

[English](README.md) | **简体中文**

[![Release](https://img.shields.io/github/v/release/mcpp-community/mcpp)](https://github.com/mcpp-community/mcpp/releases)
[![C++23](https://img.shields.io/badge/C%2B%2B-23-blue.svg)](https://en.cppreference.com/w/cpp/23)
[![Module](https://img.shields.io/badge/module-ok-green.svg)](https://en.cppreference.com/w/cpp/language/modules)
[![License](https://img.shields.io/badge/license-Apache_2.0-blue.svg)](LICENSE)

| [文档](docs/zh/) · [快速开始](docs/zh/01-getting-started.md) · [mcpp.toml 指南](docs/zh/04-mcpp-toml.md) · [示例项目](docs/zh/03-examples.md) · [工具链管理](docs/zh/20-toolchains.md) |
|:---:|
| [包索引 mcpp-index](https://mcpplibs.github.io/mcpp-index/) · [模块化库 mcpplibs](https://github.com/mcpplibs) · [社区论坛](https://forum.d2learn.org/category/20) · [Issues](https://github.com/mcpp-community/mcpp/issues) · [Releases](https://github.com/mcpp-community/mcpp/releases) |
| [![ci-linux](https://github.com/mcpp-community/mcpp/actions/workflows/ci-linux.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-linux.yml) [![ci-macos](https://github.com/mcpp-community/mcpp/actions/workflows/ci-macos.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-macos.yml) [![ci-windows](https://github.com/mcpp-community/mcpp/actions/workflows/ci-windows.yml/badge.svg?branch=main)](https://github.com/mcpp-community/mcpp/actions/workflows/ci-windows.yml) |

<p align="center">
  <img src="https://github.com/user-attachments/assets/6c85896e-9a37-4f62-acfb-d37a4eae2363" alt="mcpp demo" width="720">
</p>

## 核心特性

- **模块化构建系统** — 专注 C++ 模块：`import std` 自动处理，文件级增量构建，模块依赖自动分析，零手动配置
- **构建插件与异构硬件编程** — `build.mcpp` 与规则包扩展构建；CUDA、HIP、SYCL、Vulkan/SPIR-V 与 Ascend C 各是一个规则包
- **包管理与模块化库生态** — SemVer 约束、锁文件、跨项目 BMI 缓存、自定义索引；[mcpplibs](https://github.com/mcpplibs) 的库两行引入即可 `import`
- **工具链管理与通用交叉构建** — `family@version` 按需安装；`--target` 让同一次构建换到 Windows、macOS、Cortex-M 或 RISC-V 裸机，一份源码经 openkal 触及多个有操作系统的目标
- **环境与运行时** — xlings 提供的用户态环境：工具链与依赖都留在隔离沙盒里，runner 把产物送上板子或模拟器
- **纯模块化自举** — mcpp 完全由 C++23 模块接口单元写成，并用这条流水线构建自己

## 为什么选择 mcpp

mcpp 专门为 **C++23 模块化开发** 打造。如果你想在项目中使用 `import std`、模块接口单元（`.cppm`）、模块分区等现代 C++ 特性，mcpp 在 Linux、macOS ARM64 和 Windows x86_64 上能为你提供便捷且友好的开发体验。

C++ 通常把这五件事分给五个工具，而 mcpp 用一条命令承担全部五件。第二行是每一列
在既有认知里通常对应的东西。

| mcpp | 通用构建系统 | 构建插件 | 包管理 | 工具链管理 | 环境与运行时 |
|---|---|---|---|---|---|
| **最接近的** | CMake + Ninja | CMake modules、xmake rules | vcpkg、Conan | rustup、nvm | conda、Nix |

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

<details>
<summary>可选 —— 短命令（<code>mp</code>、<code>mbuild</code>、<code>mrun</code> …）</summary>

```bash
xlings install mcpp-short-cmd -y
```

装 30 个短命令，`mcpp build` 就是 `mbuild`。命名规则：除最后一个词外每词取首字母，
最后一个词写全 —— `mcpp self doctor` → `msdoctor`；`mp` 就是裸 `mcpp`。
它们指向 `mcpp` 这个 shim 而非固定路径，所以 `xlings use mcpp <ver>` 也会一起切换。

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

**其他方式**

<details>
<summary><b>方式 1</b> — 一键安装脚本（Linux x86_64/aarch64、macOS ARM64）</summary>

```bash
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

该脚本不支持 Windows；请使用上方 PowerShell 的 xlings 安装方式。它会安装到
`~/.mcpp/`，并自动加入 shell PATH。删除 `~/.mcpp` 即可干净卸载。

</details>

<details>
<summary><b>方式 2</b> — Homebrew（macOS / Linux）</summary>

```bash
brew install mcpp-community/mcpp/mcpp-m
```

一条命令即可，会自动 tap [`mcpp-community/homebrew-mcpp`](https://github.com/mcpp-community/homebrew-mcpp)
并安装同一份预编译 release 二进制。macOS 需要 Apple 芯片 + macOS 14；
每个用户的数据仍在各自的 `~/.mcpp/`。

Homebrew 上 `mcpp` 属于一个无关的 C 预处理器，所以公式名是 `mcpp-m`，
装出来的命令仍然是 `mcpp`。

**Homebrew 6 对第三方 tap 加了信任门。** 上面那条全限定命令会被当作显式意图、
可以直接用；但**其它任何拼写**——短名 `brew install mcpp-m`、`mcpp` 别名、
以及之后的升级——都会被拒：

```
Refusing to load formula mcpp-community/mcpp/mcpp-m from untrusted tap
mcpp-community/mcpp.
```

信任这个 tap 一次，它们就都能用了：

```bash
brew trust mcpp-community/mcpp
```

</details>

<details>
<summary><b>方式 3</b> — Arch Linux（AUR）</summary>

```bash
yay -S mcpp-bin      # 预编译 release 二进制
yay -S mcpp-m        # 或源码构建（用 mcpp-bin 自举）
```

系统级安装 `mcpp` 命令，每个用户的数据仍在各自的 `~/.mcpp/`。
Arch 上 `mcpp` 这个名字属于一个无关的 C 预处理器，所以包名是
`mcpp-bin` / `mcpp-m`（详见 [`scripts/aur/`](scripts/aur/)）。
稳定 release 的自动对账只管理 `mcpp-bin`；`mcpp-m` 与 `mcpp-git` 仍由人工维护，
可能有意滞后。

</details>

<details>
<summary><b>方式 4</b> — 让 AI 助手帮你安装</summary>

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

内置脚手架采用约定优于配置，不写 `[targets.hello]`：`src/main.cpp` 会推断出
binary target，`mcpp test` 会自动发现 `tests/test_smoke.cpp`。

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

> 更多依赖配置方式（版本约束、命名空间、Git 引用、本地路径等）参见 [mcpp.toml 指南 — 依赖管理](docs/zh/04-mcpp-toml.md)。

## 功能概览

<details>
<summary><b>构建系统</b></summary>

- C++20/23/26 模块原生支持（接口单元、实现单元、模块分区），另有 `c++latest` / `c++fly` 实验模式
- `import std` / `import std.compat` 全自动预编译与缓存
- 三层增量优化：前端脏检查 + 逐文件 P1689 dyndep + BMI copy-if-different restat
- 指纹化 BMI 缓存：按编译器/标志/标准库哈希，跨项目共享
- Ninja 后端：自动生成 build.ninja，并行编译
- compile_commands.json 自动生成（clangd / ccls 即用）；`mcpp build --configure-only` 可在编译普通源码之前先刷新它
- C 语言一等支持：`.c` 文件自动检测，混合 C/C++ 项目
- 用户自定义 cflags / cxxflags / ldflags / c_standard

</details>

<details>
<summary><b>工具链管理</b></summary>

- 内置 GCC 16.1.0 + LLVM/Clang 20.1.7，一键安装
- 首次运行按宿主选择：Linux x86_64 使用原生 glibc GCC，其他 Linux 架构使用 musl GCC，macOS 与具备可用 MSVC 的 Windows 使用 LLVM，裸 Windows 使用 MinGW-w64 GCC
- 多版本共存：`mcpp toolchain install gcc 16` / `mcpp toolchain install llvm 20`
- 隔离沙盒：所有工具链在 `~/.mcpp/registry/`，不影响系统
- 按平台指定：`linux = "gcc@16"`, `macos = "llvm@20"`
- GCC + Clang 编译管线平权（`BmiTraits` 抽象层驱动）

</details>

<details>
<summary><b>交叉编译、裸机与设备</b></summary>

- `mcpp build --target <triple>` — 一个开关;该目标所需的工具链载荷会自动解析并安装
- 从 `x86_64-linux-gnu` 到 Cortex-M、Cortex-A 与 RISC-V 裸机,完整的表见[平台支持](#平台支持)
- freestanding 目标不带操作系统:C 库、启动代码、内存布局与模拟器随板级支持包走,而不随 mcpp 走
- runner 用来触及构建机器上跑不了的产物 —— `mcpp run --runner flash`、`--list-runners`、`mcpp why runners`
- `mcpp new --template riscv-virt-rt` — 由包自带的板级模板,按名字实例化
- 基于 openkal 的交叉编译:一个可移植程序,为内核接口与 C 库都来自包的目标构建

</details>

<details>
<summary><b>异构构建与加速器</b></summary>

- `[build] accel = "cuda12.9+{sm_89}, vulkan1.2"` — 一次构建可以点名一个或多个设备后端,该构建里 `cfg(accelerator = "cuda")` 为真
- 目前有规则包的编程模型有五个:CUDA、HIP、SYCL、Vulkan/SPIR-V 与 Ascend C
- 设备翻译单元由它自己的编译器编译,产物进入普通链接;主机与设备之间的边界是生成的,不是写两遍的
- 带约束的 glob 用来挑出设备源码:`{ glob = "src/kernels/**/*.cu", accel = "cuda12.9+{sm_89}" }`
- 引擎里不含任何厂商名,因此第六个后端是一个包,而不是一次引擎改动

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

- `mcpp pack`：四种 Linux 发布模式 — system / vendored（默认）/ self-contained / static；`bundle-project` 与 `bundle-all` 仍是兼容别名
- musl 全静态二进制：单文件可分发，无 glibc 依赖（匹配的 Linux x86_64 或 aarch64 target）
- `mcpp publish`：生成 xpkg.lua + 发布到包索引
- 自动 patchelf 修正 RPATH（Linux）

</details>

<details>
<summary><b>扩展构建</b></summary>

- `build.mcpp` — 为 mcpp 没有现成规则的那一步写的构建程序,说的是一套带版本号的指令协议,而不是靠猜
- `mcpp::action` 用显式的输入与输出声明一份工作,于是生成物参与增量图,而不是待在图外
- 规则包把那一步带给别的项目:包声明一个 rule 模块,消费者以 feature 的形式选中它
- payload、运行时适配器与板级支持包都是普通的包 —— 一个工具、一个驱动或一块板子,由安装库的那个解析器安装

</details>

<details>
<summary><b>开发体验</b></summary>

- `mcpp new` — 创建模块化项目；`--template [ns.]name[@version][:tname]` 与 `mcpp add` 使用同一精确身份风格并选择**包自带模板**。只有一个模板时即使未写 `default = true` 也自动成为默认；歧义时用 `--list-templates [ns.]name[@version]` 列举
- `mcpp run [-- args]` — 构建并运行
- `mcpp test [pattern] [-- args]` — 自动发现并运行测试(按名字过滤;`--list`、`--timeout <s>`、`--message-format json`)
- `mcpp search` — 搜索包索引
- `mcpp add / remove / update` — 依赖管理
- 命令行上的 profile 与 feature:`--release` / `--profile <name>`(`build`、`run`),`--features <list>`(`build`、`run`、`test`)
- `mcpp why [toolchain|runtime|deps|runners]` — 解释已解析的构建决策;`--format json` 供程序读取
- `mcpp emit sbom` — 为刚刚记录下来的这次解析产出一份 CycloneDX 物料清单
- `mcpp --offline` / `MCPP_OFFLINE=1` — 仅使用已存在的本地状态
- `mcpp explain E0001` — 错误码详细解释
- `mcpp self doctor` — 环境自诊断

</details>

## 性能对比

用**四个构建引擎**编译 **mcpp 自己** —— 锁定的工作负载有 137 个模块接口单元、
57k 行,每一个都 `import std;` —— 并且**给它们同一个编译器二进制**。每格是 **3 轮的中位数**,以及
相对 cmake 的倍率。所有列出自**同一次跑**。

<!-- columns: mcpp=mcpp@2026.8.13.1; mcpp +优化=mcpp@2026.8.13.1+schedule=on; mcpp (旧版)=mcpp@2026.8.11.3; cmake=cmake; xmake=xmake -->
| 场景 | `mcpp` | `mcpp +优化` | `mcpp (旧版)` | `cmake` | `xmake` |
|---|---|---|---|---|---|
| `cold` | 86.69s · 1.1x | **35.73s · 2.6x** | 86.75s · 1.1x | 91.74s · 1.0x | 90.54s · 1.0x |
| `noop` | **0.16s · 2.0x** | 0.18s · 1.8x | 0.24s · 1.3x | 0.32s · 1.0x | 0.38s · 0.8x |
| `touch-hub` | **0.42s · 197.7x** | 0.42s · 197.2x | 81.72s · 1.0x | 83.21s · 1.0x | 82.48s · 1.0x |
| `edit-body` | 80.87s · 1.1x | **29.83s · 2.9x** | 81.19s · 1.1x | 85.30s · 1.0x | 84.33s · 1.0x |
| `edit-comment` | **0.40s · 207.0x** | **0.40s · 207.0x** | 79.11s · 1.1x | 83.21s · 1.0x | 82.15s · 1.0x |

<sub>`cold` 还没编过 · `noop` 什么都没改 · `touch-hub` 只碰 mtime,内容不变 · `edit-body` 真的改了一个函数体 · `edit-comment` 在 hub 接口里加一行注释。<br>
<br>
`mcpp` = mcpp@2026.8.13.1,被测的这一版 · `mcpp +优化` = **和 `mcpp` 同一个二进制**,开了 opt-in 的 `[build] bmi_schedule = "on"`(默认关闭) · `mcpp (旧版)` = mcpp@2026.8.11.3,上一个已发布版。<br>
Linux x86_64 · i9-13900K · gcc 16.1.0 · n=3 · 锁定的工作负载 `a749e9f` ·
cmake 4.4.2 / xmake 3.1.0 · 所有大于 1s 的中位数 min/max 都在 ±4% 以内 ·
数据:[`standard-20260814-linux-x86_64`](bench/results/standard-20260814-linux-x86_64/)。</sub>

* **`touch-hub` 与 `edit-comment` 两行由级联抑制决定。**
  cmake 与 xmake 按时间戳判断,重编全部下游单元;mcpp 将编译器刚产出的 BMI 与上
  一份比较,接口未变则不触发级联。这是默认行为,无需任何配置。`mcpp (旧版)` 一列
  测得上一个发布版为 81.72s,与 cmake 同量级,因此该效果在本版本中才生效。
* **`edit-body` 是对照行,mcpp 在这一行有意不快。** 扰动往接口单元里插入一条语句,
  GCC 记录的声明位置随之移动,BMI 因而改变,每一个导入者都欠一次重建 —— 在这一行
  跑得快的引擎,漏掉的是它欠下的工作。一次改动欠不欠级联取决于函数体写在哪里:
  原地等长的修改,或者写在独立 `.cpp` 里的函数体,都不欠级联,落在约 200x 的那一档。
  实测见
  [`.agents/docs/2026-08-15-module-edit-granularity.md`](.agents/docs/2026-08-15-module-edit-granularity.md)。
* **`bmi_schedule` 为 opt-in,默认关闭**(`auto` 解析为 off)。它将代码生成移出关键
  路径,因此只在级联确实欠着时才有收益 —— `cold` 86.69s → 35.73s、`edit-body`
  80.87s → 29.83s,而在 mcpp 本已跳过级联的两行上没有收益。调度错误的表现是静默
  失效而非报错,因此不以单台机器的证据变更默认值。

**[方法、锁定的版本、完整数据 → `bench/README.zh-CN.md`](bench/README.zh-CN.md)** ·
[English](bench/README.md)

## 平台支持

mcpp 的身份模型是两条正交轴:**工具链** = `family@version`(family ∈ gcc | llvm | msvc),
**目标** = 三段 triple `arch-os[-env]`。交叉编译只需 `mcpp build --target <triple>`——
对应的工具链包会自动解析并安装。`mcpp toolchain list` 查看本机实时状态。

**宿主**(mcpp 本身运行在哪):Linux x86_64 / aarch64、macOS arm64、Windows x86_64。

**目标**(`--target` 接受什么;表里的行与它们的档位取自
`modules/toolchain-model/src/triple.cppm`,也就是 `mcpp toolchain list` 为本机
报告的那一份):

| Target | 约定工具链 | 档位 |
|---|---|:---:|
| `x86_64-linux-gnu`    | gcc(*Linux 默认*)或 llvm | verified |
| `x86_64-linux-musl`   | gcc 16,全静态 | verified |
| `aarch64-linux-musl`  | gcc 16,全静态——x86_64 交叉(qemu)或原生 | verified |
| `x86_64-windows-gnu`  | gcc 16 MinGW-w64——Windows 原生,Linux 交叉(wine)(*无 Visual Studio 时的 Windows 默认*) | verified |
| `x86_64-windows-msvc` | `msvc@system`(探测 VS/BuildTools)或 llvm ¹(*有 Visual Studio 时的 Windows 默认*) | verified |
| `x86_64-windows-musl` | llvm 22——带 musl C 库的 PE,没有 gcc 能产出它;系统由依赖图供给 | preview |
| `aarch64-macos`       | llvm(*macOS 默认*) | verified |
| `riscv64-none-elf` · `riscv32-none-elf` | llvm 22——裸机,`xim:picolibc-riscv` ² | verified |
| `thumbv6m-none-eabi` · `thumbv7m-none-eabi` | llvm 22——Cortex-M0/M0+/M1、Cortex-M3 ² | verified |
| `thumbv7em-none-eabihf` · `thumbv8m.main-none-eabi` | llvm 22——Cortex-M4F/M7F 硬浮点、Cortex-M33/M55 软浮点 ² | verified |
| `armv7a-none-eabi` · `armv7a-none-eabihf` | llvm 22——Cortex-A 32 位,第一条带 MMU 的行 ² | verified |
| `aarch64-none-elf` · `x86_64-none-elf` | llvm 22——裸机,默认不带 C 库 ² | preview |
| `thumbv7em-none-eabi` · `thumbv8m.base-none-eabi` · `thumbv8m.main-none-eabihf` | llvm 22——Cortex-M4/M7 软浮点、M23、M33F/M55F ² | preview |
| `riscv64-linux-musl` · `aarch64-linux-gnu` · `x86_64-macos` | — | planned |

`verified` 该行的镜像已被构建**并运行**过,qemu 与 wine 都算 · `preview` 可构建
可链接,未记录过模拟器运行 · `planned` 已登记在词表中,尚未接线 —— 面向这类目标
的构建会被拒绝,而不是被尝试。

> Linux release 二进制为 x86_64 与 aarch64 的 musl 全静态构建
> (`x86_64-linux-musl` 与 `aarch64-linux-musl`)。
> 旧拼写——`x86_64-w64-mingw32`、`gcc@16.1.0-musl`、`mingw-cross@…`、`musl-gcc@…`——
> 作为别名**永久接受**,归一到上表的 canonical 形式。
>
> ¹ Windows 上 llvm 打的是 MSVC ABI,因此依赖已安装的 **MSVC BuildTools 或
> Visual Studio**(UCRT、Windows SDK、MSVC STL)。这件事你不需要自己安排:mcpp 首跑会
> 探测是否有可用的 MSVC,探不到就默认走 `x86_64-windows-gnu`(winlibs MinGW-w64)——
> 完全自包含、不需要 Visual Studio、`import std` 可用。无需安装或配置,裸 Windows 上
> `mcpp new && mcpp build` 直接可用。而 `mcpp.toml` 里显式写的 `[toolchain]` 永远按你
> 写的执行——mcpp 只修正自己选的默认值,不改你的。
>
> ² 裸机的那些行不带操作系统:clang 与 lld 天生就是交叉编译器,因此任何能安装
> LLVM 载荷的宿主都能产出这些目标。C 库、启动代码、内存布局与模拟器随板级支持包
> 走,而不随 mcpp 走 —— 见
> [40 — 裸机与 freestanding 目标](docs/zh/40-baremetal.md)。

## 文档

[`docs/zh/`](docs/zh/README.md) 是手册。章节号的第一位说明它属于哪一部分;索引
另有一份反向查表 —— 从读者眼前的一个 manifest 键或一条命令,查到拥有它的那一章。

| 部分 | 从这里开始 |
|---|---|
| `0x` 基础 | [01 快速开始](docs/zh/01-getting-started.md) · [04 mcpp.toml 清单](docs/zh/04-mcpp-toml.md) · [09 按场景查命令](docs/zh/09-commands-by-scenario.md) |
| `1x` 发布 | [10 发布打包](docs/zh/10-pack-and-release.md) · [11 发布一个库](docs/zh/11-publishing-a-library.md) · [12 分发预编译库](docs/zh/12-binary-distribution.md) |
| `2x` 工具链与目标 | [20 工具链管理](docs/zh/20-toolchains.md) · [21 目标三元组](docs/zh/21-the-target-triple.md) · [24 基于 openkal 的交叉编译](docs/zh/24-openkal-cross.md) |
| `3x` 扩展 mcpp | [30 构建程序](docs/zh/30-build-mcpp.md) · [31 编写规则包](docs/zh/31-authoring-a-rule-package.md) · [34 编写板级支持包](docs/zh/34-authoring-a-bsp.md) |
| `4x` 设备与加速器 | [40 裸机与 freestanding 目标](docs/zh/40-baremetal.md) · [41 触及设备](docs/zh/41-devices.md) · [42 异构构建](docs/zh/42-heterogeneous-builds.md) |
| `5x` 给程序的契约 | [50 机器可读输出](docs/zh/50-machine-output.md) · [51 支持的版本](docs/zh/51-supported-versions.md) · [规范](docs/specs/README.md) |
| `9x` mcpp 自身 | [90 从源码构建](docs/zh/90-build-from-source.md) · [92 发布 mcpp](docs/zh/92-release.md) |

[`examples/`](examples/) 下的每一个目录都是一个能构建的工程,
[03 — 示例项目](docs/zh/03-examples.md) 说明哪一个教什么。任意命令的完整选项
可通过 `mcpp <cmd> --help` 查阅。

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
| [mcpp](https://github.com/mcpp-community/mcpp) | mcpp 自身 —— 由 C++23 模块写成,完全自举 |
| [xlings](https://github.com/openxlings/xlings) | mcpp 依赖的工具链与包管理底座 |
| [tinyhttps](https://github.com/mcpplibs/tinyhttps) | 极简 C++23 HTTP/HTTPS 客户端,支持 SSE 流式 |
| [llmapi](https://github.com/mcpplibs/llmapi) | 现代 C++ LLM API 客户端(OpenAI 兼容) |
| [imgui-m](https://github.com/mcpplibs/imgui-m) | Dear ImGui 的 C++23 模块封装包 |
| [cmdline](https://github.com/mcpplibs/cmdline) | 命令行解析库/框架(mcpp 自身在用) |

更多模块化库 → [mcpplibs](https://github.com/mcpplibs) · 包索引 → [mcpp-index](https://mcpplibs.github.io/mcpp-index/)

## 参与贡献

欢迎通过 Issue 和 PR 参与项目开发。项目接受开发者使用 AI Agent 参与开发与贡献。

**基本流程**

1. 创建 Issue — Bug 修复、新功能、优化等，先在 [issues](https://github.com/mcpp-community/mcpp/issues) 创建讨论
2. 实现改动 — Fork 仓库，创建分支，并按改动范围验证（行为改动运行 `mcpp build` 与相关测试；纯文档改动复核示例和链接）
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
- [mcpp-index](https://mcpplibs.github.io/mcpp-index/) — 默认包索引
- [mcpplibs](https://github.com/mcpplibs) — 模块化 C++ 库集合

### 致谢

项目依赖和灵感来源：

- [xlings](https://github.com/openxlings/xlings) — 工具链 / 包管理底座
- [mcpplibs.cmdline](https://github.com/mcpplibs/cmdline) — CLI 框架
- [ninja](https://github.com/ninja-build/ninja) — 底层构建引擎
- [xmake](https://github.com/xmake-io/xmake) — 跨平台构建工具
- [cargo](https://github.com/rust-lang/cargo) — Rust 包管理器
