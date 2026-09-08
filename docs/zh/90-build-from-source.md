# 90 —— 从源码构建与参与贡献

**读者:**要构建并修改 mcpp 本身的贡献者。

**本章回答的那一个问题:**mcpp 怎样从源码构建、它自己的测试怎样组织,以及一次
贡献要满足什么。

**不在这里:**用户怎样构建自己的工程,那是 [01 —— 快速开始](01-getting-started.md);
以及一次发布怎样切出来,那是 [92 —— 发布 mcpp](92-release.md)。

> mcpp 采用自托管模式 —— 通过 mcpp 自身从源码构建 mcpp。
> 任何已具备可运行 mcpp 二进制的环境均可完成源码构建。

## 准备

参照 [01 — 快速开始](01-getting-started.md) 安装一份现成的 mcpp,
然后克隆仓库:

```bash
git clone https://github.com/mcpp-community/mcpp
cd mcpp
```

## 构建与测试

```bash
mcpp build              # 使用现成 mcpp 编译当前源码 → ./target/.../bin/mcpp
mcpp run -- --version   # 运行刚构建出的产物
mcpp test               # 构建并运行 tests/**/*.cpp 中发现的 C++ 测试(包含 tests/unit)
```

`mcpp test` 不执行 `tests/e2e/` 下的 shell 端到端套件;应单独让它使用刚构建的二进制。

首次构建会自动拉取默认工具链,详见
[20 — 工具链管理](20-toolchains.md)。

如需生成与 release 一致的全静态二进制(对应 `release.yml` 走的路径):

```bash
mcpp build --target x86_64-linux-musl
# → target/x86_64-linux-musl/.../bin/mcpp 为全静态 ELF
```

## 源码结构

mcpp 是一个**工作空间**。`modules/` 里是引擎 import 的九个包;`src/` 是引擎本身。

```
modules/                  工作空间成员,被 src/ import
├── manifest/             manifest 与描述符解析
├── platform/             操作系统抽象
├── toolchain-model/      三元组、方言、指纹、链接模型
├── buildmcpp/            build.mcpp 契约:协议、指令表、provisions
├── source-kind/          源文件角色分类
├── versioning/           本二进制的版本,以及 SemVer 约束解析
├── dyndep/               ninja dyndep 产出
├── libs/                 内嵌的文本格式解析器
└── log/                  分级日志

src/
├── main.cpp              入口
├── cli.cppm  cli/        命令分发与各条命令
├── build/                构建编排与 ninja 后端
├── modgraph/             P1689 模块扫描与依赖图
├── pm/                   解析器与包管理命令
├── toolchain/            探测、指纹、std 模块
├── pack/  publish/       mcpp pack、mcpp publish 与 xpkg 产出
├── fetcher/  fallback/   下载、安装、回退解析
├── bmi_cache/            跨工程 BMI 缓存
├── runtime/  xlings/     运行时契约与 xlings 桥
├── freestanding/         裸机目标、链接行与 runner
└── scaffold/             `mcpp new` 与模板

tests/
├── unit/                 108 个 C++ 测试,由 `mcpp test` 发现
└── e2e/                  370 个对真实二进制运行的 shell 脚本
```

## 测试组织

两层,回答的是不同的问题。

**单元测试**(`tests/unit/`,108 个)是 `mcpp test` 发现的 C++ 程序。它们检验一个模块
的契约,不需要二进制、也不需要文件系统状态。

**端到端测试**(`tests/e2e/NN_<name>.sh`,370 个)拿真实的 `mcpp` 二进制跑真实工程。
`run_all.sh` 是 CI 的入口。`mcpp test` **不**运行它们。

```bash
MCPP=<新构建的 mcpp 二进制> bash tests/e2e/02_new_build_run.sh
```

**由能力闸门决定哪些会跑。** e2e 脚本开头几行声明它需要什么,不具备该能力的 runner
会跳过它:

```bash
#!/usr/bin/env bash
# requires: elf gcc
```

在用的有 `gcc`(80 个脚本)、`elf`、`unix-shell`、`llvm`、`jq` 与 `fresh-sandbox`。
一个要求「任何 CI job 都不提供的能力」的脚本**在任何地方都不会跑**,它的绿色什么都不
说明 —— 新增脚本时要核对确有 job 供给它所要的能力。

## 写一条真的在测东西的判据

本仓库最可迁移的一条规则,也是被跳过时会静默失效的那一条:

> **写完判据之后,把修复拿掉,跑一次。** 从未被看见失败过的判据,不能说它测到了什么。

它能抓住三种形态,每一种在这里都至少发生过一次:

| 形态 | 长什么样 |
|---|---|
| 判据从没跑到 | 测试被闸在一个没有任何 job 提供的能力上 |
| 判据不可能失败 | 子串搜索,任何措辞都能满足它 |
| 判据施加在错误的对象上 | 夹具目录跨次累积,于是搜索回答的是更早那次构建 |

也要说出分母。「表里每一台宿主都被扫过」是一条判据;「扫描没发现问题」不是 —— 因为
空的枚举同样什么都发现不了。

## 测试之外 CI 还跑的检查

`.github/tools/` 里有十八个脚本。第一个 PR 之前值得知道的有四个:

| 脚本 | 它拒绝什么 |
|---|---|
| `check_docs_style.sh` | 疑问句标题、参考章节里的第二人称、标题结构落后于英文的中文页 |
| `check_docs_structure.sh` | 章节引用设计记录、解析不到的 `docs/NN-*.md` 路径、翻译里少掉的表格 |
| `check_version_pins.sh` | 版本号只写在一处而别处没跟上 |
| `check_modules_wiring.sh` | 只接进了「三处必须知道它的地方」中的一部分的工作空间成员 |


## 新增一个 manifest 字段:准入标准

> **语法封闭,词汇开放**:谁拥有解析语义谁定义键;谁拥有领域知识谁定义值。

- mcpp 只定义**机制**(features 并集/闭包、capability require/provide/override、
  profile→编译器旗标、platform→triple),键与形状固定;feature 名、能力名、
  后端名等**领域词汇只出现在值里**,不进 mcpp 代码。
- **不支持包自定义 toml 键**:键合法性不得依赖"先解析目标包",否则 manifest
  失去静态可解析性(lockfile/LSP/审计的前提)。包的扩展点 = 固定机制内的开放值域。
- 包级旋钮统一收敛进 features;糖键(如 `backend=`)进入核心语法须满足:
  ① 领域中立(跨生态通用模式)② 1:1 脱糖、零新增解析语义。

## Issue 与 PR 提交规范



### Issue

提交至 [github.com/mcpp-community/mcpp/issues](https://github.com/mcpp-community/mcpp/issues),
建议附带以下信息:

- `mcpp self env` 的完整输出
- 失败命令的完整输出(配合 `MCPP_LOG_LEVEL=debug` 可获得更详细信息)
- 操作系统、发行版、glibc 版本(可通过 `ldd --version` 查看)

### Pull Request

mcpp 处于早期迭代阶段,接口可能调整,提交 PR 前请注意:

1. 涉及 CLI 或 `mcpp.toml` schema 的改动,建议先开 issue 对齐方向。
2. 单个 PR 聚焦单一改动;commit 标题使用英文 imperative 形式
   (`fix: ...` / `feat: ...`)。
3. 行为改动或测试文档改动在提交前运行 `mcpp test`，并让相关 E2E 脚本使用刚构建的
   二进制通过；纯文档改动复核示例和链接，并用 `gh pr checks <pr-number>` 确认 PR
   实际 required checks。

## 社区资源

- [社区论坛](https://forum.d2learn.org/category/20)
- 交流群 QQ: 1067245099
- [mcpp-index](https://github.com/mcpplibs/mcpp-index) — 默认包索引
- [mcpplibs](https://github.com/mcpplibs) — 配套的模块化 C++ 库集合
