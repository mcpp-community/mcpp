# 90 —— 从源码构建与参与贡献

**读者：** 要构建并修改 mcpp 本身的贡献者。

**本章回答的那一个问题：** mcpp 怎样从源码构建、它自己的测试怎样组织，以及
一次贡献要满足什么。

**不在这里：** 用户怎样构建自己的工程，那是
[01 —— 快速开始](01-getting-started.md)；以及一次发布怎样切出来，那是
[92 —— 发布 mcpp](92-release.md)。

> mcpp 是自托管的——mcpp 通过自身从源码构建 mcpp。
> 任何已经具备可运行 mcpp 二进制的环境，都可以从源码构建。

## 准备

按照 [01 —— 快速开始](01-getting-started.md) 安装一份可用的 mcpp，然后
克隆仓库：

```bash
git clone https://github.com/mcpp-community/mcpp
cd mcpp
```

## 构建与测试

```bash
mcpp build              # compile the current source with the existing mcpp → ./target/.../bin/mcpp
mcpp run -- --version   # run the artifact you just built
mcpp test               # build and run C++ tests discovered under tests/**/*.cpp (including tests/unit)
```

`mcpp test` 不运行 `tests/e2e/` 下的 shell 端到端套件；应针对刚构建出的
二进制单独运行这些脚本。

首次构建会自动拉取默认工具链，详见
[20 —— 工具链管理](20-toolchains.md)。

要生成与一次 release 完全一致的全静态二进制（`release.yml` 走的正是这条
路径）：

```bash
mcpp build --target x86_64-linux-musl
# → target/x86_64-linux-musl/.../bin/mcpp is a fully static ELF
```

## 源码结构

mcpp 是一个**工作空间**。`modules/` 下是引擎 import 的九个包；`src/` 是
引擎本身。

```
modules/                  workspace members, imported by src/
├── manifest/             manifest and descriptor parsing
├── platform/             operating-system abstraction
├── toolchain-model/      triples, dialects, fingerprints, the link model
├── buildmcpp/            the build.mcpp contract: protocol, directives, provisions
├── source-kind/          source-file role classification
├── versioning/           this binary's version, and SemVer requirements
├── dyndep/               ninja dyndep emission
├── libs/                 vendored text-format parsers
└── log/                  leveled logging

src/
├── main.cpp              entry point
├── cli.cppm  cli/        command dispatch and the commands
├── build/                build orchestration and the ninja backend
├── modgraph/             P1689 module scanning and the dependency graph
├── pm/                   the resolver and the package-management commands
├── toolchain/            detection, fingerprinting, the std module
├── pack/  publish/       mcpp pack, mcpp publish and xpkg generation
├── fetcher/  fallback/   download, installation, fallback resolution
├── bmi_cache/            the cross-project BMI cache
├── runtime/  xlings/     the runtime contract and the xlings bridge
├── freestanding/         bare-metal targets, link line and runner
└── scaffold/             `mcpp new` and templates

tests/
├── unit/                 108 C++ tests, discovered by `mcpp test`
└── e2e/                  370 shell scripts against a real binary
```

## 测试组织

两层，回答的是不同的问题。

**单元测试**（`tests/unit/`，108 个文件）是 `mcpp test` 发现的 C++
程序。它们检验一个模块的契约，不需要二进制、也不需要文件系统状态。

**端到端测试**（`tests/e2e/NN_<name>.sh`，370 个文件）拿真实的 `mcpp`
二进制跑真实工程。`run_all.sh` 是 CI 的入口。`mcpp test` **不**运行它们。

```bash
MCPP=<fresh-mcpp-binary> bash tests/e2e/02_new_build_run.sh
```

**由能力闸门决定哪些会跑。** e2e 脚本开头几行声明它需要什么，不具备该
能力的 runner 会跳过它：

```bash
#!/usr/bin/env bash
# requires: elf gcc
```

在用的有 `gcc`（80 个脚本）、`elf`、`unix-shell`、`llvm`、`jq` 与
`fresh-sandbox`。一个要求「任何 CI job 都不提供的能力」的脚本**在任何
地方都不会跑**，它的绿色什么都不说明——新增脚本时要核对确有 job 供给
它所要的能力。

## 写一条真的在测东西的判据

本仓库最可迁移的一条规则，也是被跳过时会静默失效的那一条：

> **写完判据之后，把修复拿掉，跑一次。** 从未被看见失败过的判据，不能说
> 它测到了什么。

它能抓住三种形态，每一种在这里都至少发生过一次：

| 形态 | 形式 |
|---|---|
| 判据从没跑到 | 测试被闸在一个没有任何 job 提供的能力上 |
| 判据不可能失败 | 子串搜索，任何措辞都能满足它 |
| 判据施加在错误的对象上 | 夹具目录跨次累积，于是搜索回答的是更早那次构建 |

也要说出分母。「表里每一台宿主都被扫过」是一条判据；「扫描没发现问题」
不是——因为空的枚举同样什么都发现不了。

## 测试之外 CI 还跑的检查

`.github/tools/` 里有十八个脚本。第一个 PR 之前值得知道的有四个：

| 脚本 | 拒绝的内容 |
|---|---|
| `check_docs_style.sh` | 疑问句标题、参考章节里的第二人称、标题结构落后于英文的 简体中文 页 |
| `check_docs_structure.sh` | 章节引用设计记录、解析不到的 `docs/NN-*.md` 路径、翻译里少掉的表格 |
| `check_version_pins.sh` | 版本号只写在一处而别处没跟上 |
| `check_modules_wiring.sh` | 只接进了「三处必须知道它的地方」中一部分的工作空间成员 |

## 新增一个 manifest 字段：准入标准

> **语法封闭，词汇开放**：谁拥有解析语义，谁定义键；谁拥有领域知识，
> 谁定义值。

- mcpp 只定义**机制**（feature 并集/闭包、capability
  require/provide/override、profile→编译器旗标、platform→triple）；键与
  形状是固定的。feature 名、能力名、后端名这类领域词汇**只出现在值
  里**，绝不进入 mcpp 的代码。
- **不支持包自定义 toml 键**：键的合法性不得依赖"先解析目标包"，否则
  manifest 会失去静态可解析性（这是 lockfile / LSP / 审计的前提）。一个
  包的扩展点 = 固定机制之内的开放值域。
- 包级旋钮统一收敛进 feature；糖键（例如 `backend=`）要进入核心语法，须
  满足：①领域中立（是跨生态的通用模式）②与既有语法 1:1 脱糖、零新增
  解析语义。
- **一个键若只是重复了别处已经给出的答案，就不被采纳。** 一件事有两处
  可以陈述，就是两处可能互相矛盾，而这种失败是静默的——不论哪个读者
  在这场竞速里落败，读到的就是错的。库的打包
  （[12](12-binary-distribution.md)）是现成的例子：它新增了**零**个
  manifest 键，因为打什么由 `[targets.<n>].kind` 回答，发布哪个接口由
  `[lib]` 加模块图回答，哪些头文件是公开的由 `[build].include_dirs`
  回答，每个产物的证据由 `[[runtime.artifacts]]` 回答。
- 一个字段若描述的是某个**生成出来**的包**是什么**（而不是一次构建
  应当**做什么**），就属于 `[[runtime.artifacts]]`——见 §2.11。
  `provenance` 以 `mcpp-pack` 开头，正是这一标记让一个目录被认作生成出来
  的产物，而 mcpp 拒绝在其内部执行 `build`。

## Issue 与 PR 提交规范

### Issue

提交至 [github.com/mcpp-community/mcpp/issues](https://github.com/mcpp-community/mcpp/issues)，
建议附带以下信息：

- `mcpp self env` 的完整输出
- 失败命令的完整输出（配合 `MCPP_LOG_LEVEL=debug` 可获得更详细的信息）
- 操作系统、发行版、glibc 版本（可通过 `ldd --version` 查看）

### Pull Request

mcpp 处于早期迭代阶段，接口可能调整，提交 PR 前请注意：

1. 涉及 CLI 或 `mcpp.toml` schema 的改动，请先开 issue 对齐方向。
2. 单个 PR 只聚焦一处改动；commit 标题使用英文祈使式（`fix: ...` /
   `feat: ...`）。
3. 涉及行为改动或测试文档的改动，提交前针对刚构建出的二进制运行
   `mcpp test` 与相关的 E2E 脚本。纯文档改动应复核示例与链接；用
   `gh pr checks <pr-number>` 查看该 PR 实际的 required checks。

## 社区资源

- [社区论坛](https://forum.d2learn.org/category/20)
- 交流群 QQ：1067245099
- [mcpp-index](https://github.com/mcpplibs/mcpp-index)——默认包索引
- [mcpplibs](https://github.com/mcpplibs)——配套的模块化 C++ 库集合
