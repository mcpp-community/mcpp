# 01 —— 快速开始

**读者：** 还什么都没装的新用户。

**本章回答的那一个问题：** 从一台空机器开始，怎样把一个程序编译并运行起来。

**不在这里：** mcpp 有哪些部件——那是
[00 —— mcpp 是什么](00-what-mcpp-is.md)，本章假定它而不重复它；以及
manifest 可以写的每一个字段，那是
[04 —— mcpp.toml 工程文件指南](04-mcpp-toml.md)。在此之后：
[03 —— 示例项目](03-examples.md)。

> 5 分钟走完 install → new → build → run → pack 全流程。

## 安装

支持的宿主为 Linux x86_64 / aarch64、macOS ARM64 与 Windows x86_64。
GCC、xlings 以及其余构建依赖都由 mcpp 安装，都不需要预先具备。

**推荐方式是 [xlings](https://xlings.d2learn.org)**，它让 mcpp 与系统
环境保持隔离：

```bash
xlings install mcpp -y
```

<details>
<summary>其它方式：独立安装脚本，以及首次运行会装什么</summary>

在 Linux x86_64/aarch64 或 macOS ARM64 上，有一个内置 xlings 的一键脚本，
把一切装到 `~/.mcpp/` 下。它不支持 Windows——那里的入口是 README 中的
PowerShell xlings 命令。

```bash
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

mcpp 首次运行时会把一条默认工具链装进 `~/.mcpp/`，按宿主选择：

| 宿主 | 默认 |
|---|---|
| Linux x86_64 | `gcc@16.1.0` |
| 其它 Linux 架构 | `gcc@15.1.0-musl` |
| macOS | `llvm@20.1.7` |
| 有可用 MSVC 的 Windows | `llvm@20.1.7` |
| 没有 MSVC 的 Windows | 面向 `x86_64-windows-gnu` 的 `gcc@16.1.0` |

完整安装说明（含 Windows）见
[README 的「安装」小节](../../README.zh-CN.md#安装)。

</details>

安装完成后，启动一个新的 shell 会话，然后验证：

```bash
mcpp --version
# mcpp <installed version>
```

> [!TIP]
> 若 Unix release 安装脚本提示 `command not found`，通常是因为
> `~/.mcpp/bin` 尚未加入当前 shell 的 PATH。重启终端，或执行
> `source ~/.bashrc`（zsh 对应 `~/.zshrc`，fish 用 `exec fish`）以生效；
> 该安装方式下可直接用 `~/.mcpp/bin/mcpp` 调用。若通过 xlings 安装，应改
> 用 xlings 当前激活的 bin 目录。Windows 上请通过 PowerShell 的 xlings
> 命令安装，重启 PowerShell 而不是执行 `source`，并用
> `Get-Command mcpp.exe` 确认当前生效的命令。

## 创建项目

```bash
mcpp new hello && cd hello
```

生成的目录结构如下：

```
hello/
├── mcpp.toml            ← project manifest
├── src/
│   └── main.cpp
└── tests/
    └── test_smoke.cpp   ← runs with `mcpp test`
```

生成的 manifest 只含包元数据；mcpp 从 `src/main.cpp` 推断 binary
target。默认情况下该文件是一个 C++23 模块化的 hello world：

```cpp
import std;

int main() {
    std::println("Hello from hello!");
    std::println("Built with import std + std::println on modular C++23.");
}
```

### 从包模板创建

`mcpp new --template` 与 `mcpp add` 使用完全相同的包 selector 语法：

```bash
mcpp new gui-demo --template ocornut.imgui@1.92.8:docking
mcpp new --list-templates ocornut.imgui@1.92.8
```

文法是 `[namespace.]name[@version][:template]`。namespace、version 与
模板名各自独立可省略；省略 namespace 表示唯一的默认命名空间
`mcpplibs`，而不是按短名扫描整个索引。省略模板名时，mcpp 使用唯一那个
`default = true` 声明；若没有显式标记默认值而包只有一个模板，就自动使用
那个唯一的模板。存在多个模板却没有默认值是一个错误，会指向
`--list-templates`。没有另一套 `--variant` 词汇。

包身份、版本与模板会在提交目标目录之前全部解析完成。下载、渲染、hook 或
校验失败时，不会留下一个半成品的工程目录。

## 构建与运行

```bash
mcpp build
# Compiling hello v0.1.0 (.)

mcpp run
# Hello from hello!
# Built with import std + std::println on modular C++23.
```

首次构建会下载按宿主选择的默认工具链，期间显示进度与速度。下载完成后，
所有 mcpp 工程共用同一份沙盒。

### 在首次成功构建前配置编辑器

源码尚不可构建时，可以只生成编译数据库，而不编译普通翻译单元、也不链接
最终目标：

```bash
mcpp build --configure-only
# Configured hello (... compile commands)
```

该命令解析的包、workspace 成员、profile、feature、capability provider、
target 与 toolchain 与一次真实构建完全相同。生成的 `compile_commands.json`
覆盖普通源码与 `tests/**/*.cpp`，包含仅测试使用的依赖以及匹配的
`[build].flags`，因此 clangd/ccls 可以在工程仍处于编辑状态时为它建立
索引。这是一次配置操作，不是只读操作：`build.mcpp`、缺失的依赖或
toolchain、lock/resolution 元数据以及构建目录元数据都可能被更新。只应在
可信的 workspace 中运行。进程退出码与生成的 `compile_commands.json` 是
稳定的集成契约；标准输出仍是面向人的文本。

不允许写入工程目录的编辑器，在标准输出上取得同一份计划
*(mcpp 2026.9.15.1+)*：

```bash
mcpp emit build-database --format json
```

该文档是一份 S1 构建数据库：每个翻译单元及其编译命令、它提供与导入的
模块、工具链，以及标准库模块单元。`--spec compile-commands` 改为输出
`compile_commands.json` 的条目。字段列在
[50 —— 机器可读输出](50-machine-output.md)，规则见
[SPEC-005](../specs/build-database.md)。

## 增量编译与测试

```bash
mcpp build              # incremental build
mcpp clean              # clean target/
mcpp clean --stale      # drop only target/<triple>/<fingerprint>/ dirs no build still uses
                        # (--dry-run lists and deletes nothing; --older-than 3d keeps newer unrecorded ones)
mcpp test               # compile and run tests/**/*.cpp — one binary per file,
                        # framework-agnostic (bare main, or gtest via [dev-dependencies])
mcpp test <pattern>     # only tests whose name contains <pattern>
mcpp test --list        # enumerate tests without building
mcpp test --timeout 30  # kill a test still RUNNING after 30s (default 300; 0 = no limit)
mcpp test --build-timeout 120   # kill a compile/link still running after 120s (off by default)
```

**运行**那一半默认有界，这样无人值守的 CI 任务就不会被一个挂住的测试拖住
整个 job。两个期限覆盖不同的一半，互不蕴含：`--timeout` 约束测试
**进程**，`--build-timeout` 约束**一次 ninja 驱动**（包级构建、批量测试
构建、每个测试各自的构建分别计时）。**一次永不返回的链接属于
`--build-timeout` 管辖的情形；任何 `--timeout` 值都拦不住它。**

`--build-timeout` 默认关闭，这种不对称是**实测**得到的，而不是风格选择：
一个测试二进制运行超过五分钟不寻常，一次冷依赖构建运行超过十五分钟很
平常（mcpp-index 有一个成员从源码构建 OpenCV，Linux 上 1019 秒、
Windows 上 1289 秒）。给它一个默认上限会把「慢但正确」的构建判红。构建
可以跑多久是工程自身的性质，所以由工程来说。仅 POSIX 有效——期限运行器
在 Windows 上没有按句柄终止的路径，该值在那里被忽略。

## 添加依赖

在 `mcpp.toml` 中声明依赖：

```toml
[dependencies]
"mcpplibs.cmdline" = "^0.0.1"
```

`mcpp build` 会自动针对
[mcpp-index](https://github.com/mcpplibs/mcpp-index) 解析 SemVer
约束、拉取源码并加入构建图。完整示例见
[03 —— 示例项目](03-examples.md) 中的 `02-with-deps`。

## 生成发布包

`mcpp pack` 把构建产物与运行期依赖打包为可独立分发的 tarball：

```bash
mcpp pack                          # vendored by default: bundle project third-party .so files
mcpp pack --mode system            # rely on target-system libraries
mcpp pack --mode static            # fully static musl build
mcpp pack --mode self-contained    # bundle loader, libc, and dependencies
```

四种模式的差异及产物布局见
[10 —— 发布打包](10-pack-and-release.md)。`bundle-project` 与
`bundle-all` 仍然是 `vendored` 与 `self-contained` 的可用别名。

## 后续阅读

- [03 —— 示例项目](03-examples.md)——可直接运行的最小工程集合
- [10 —— 发布打包](10-pack-and-release.md)——构建可分发产物
- [20 —— 工具链管理](20-toolchains.md)——切换编译器与管理多个版本
- 任意命令的完整选项都可通过 `mcpp <cmd> --help` 查阅

## 更多入口

- GUI 起步：`mcpp new myapp --template ocornut.imgui@1.92.8:docking`
  （模板随包分发；省略 `:docking` 使用已声明的 default / 唯一模板，或
  运行 `mcpp new --list-templates ocornut.imgui@1.92.8`）。
- 解释默认决策：`mcpp why [toolchain|runtime|deps]`；宿主能力体检：
  `mcpp self doctor`；机器可读的解析清单：构建产物
  `target/<triple>/<fp>/resolution.json`。
- 离线运行：`mcpp --offline` 或 `MCPP_OFFLINE=1` 会阻止索引刷新、下载与
  工具链安装。在从未使用过的 home 中，它还会跳过首次使用时的沙箱引导
  （索引克隆、ninja、patchelf），只提示一次，并让该 home 保持未引导
  状态；需要这些工具的命令会各自报告缺失。
