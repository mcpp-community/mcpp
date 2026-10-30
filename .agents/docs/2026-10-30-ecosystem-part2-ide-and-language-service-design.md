---
subject: editors
status: active
---

# 生态设计 part 2：mcpp 插件与 mcppls 的分层、入口、测试、版本感知与分发

日期：2026-10-31（rev 10，实施）。基线：mcpp 2026.10.30.1（`13e2da04`）、mcpp-ide #3（0.2.0，`734d856` + `49ae757`）、
mcppls #75（0.0.20，`4660eb6`）。前文：[part 1](2026-10-29-ecosystem-target-sets-packaging-and-editors-design.md)（§9 mcppls 单包、
§10 编辑器）。相关 issue：mcpp#868（锁文件键）、Sunrisepeak/mcppls-clangd#12（clangd 映像）。相关规范：mcpp SPEC-015 §4、docs/08、
docs/50；mcppls S3、S4；mcpp-ide protocol。

**修订记录**

- rev 1：综合 2026-10-30 的六轮讨论。已定：六项合为一份设计、一轮实现；mcppls 的状态经命令行提供；IDE 插件对 mcppls
  保留安装上的硬依赖、不绑定版本；按项目感知 mcpp 版本的缺陷在 mcpp-ide 0.2.0 先修；clangd 映像暂缓（mcppls-clangd#12）；mcppls
  的包体积不是约束，分发与安装的稳定性是。
- rev 2（本稿）：多视角自我评审（架构、兼容与升级、安全、性能、跨平台、交付），18 条处理见 §16。主要变化：Neovim 的后端是独立
  的 xlings 包，改为精确版本依赖（§3）；探测清除 `XLINGS_PROJECT_DIR`、处理"锁定版本未安装"、按需且限并发（§4.2）；`features`
  只收客户端据以开关的能力，并为早于它的 mcpp 冻结一张对照表（§4.3）；0.2.0 先行修复改为"按项目的正确版本"比较（§4.7）；
  `mcppls status` 列出实例、动作 id 归 mcppls（§5.2、§5.3）；入口与用例位置不再经 mcppls 命令行（§6.3、§7.3），mcppls 0.0.21
  只新增 `status`；动态列举只对静态扫描到框架宏的程序执行（§7.3）；后端在同一进程内以 LSP 服务规则层（§8）；无后端回退保留为
  声明的降级模式（§8）；工作区信任（§4.2、§7.3）。
- rev 3（本稿）：按评审决定改写（不读 `.xlings.json`；首批框架取 mcpp-index 提供的；未发布的 PR 追加、已发布的仓库新 PR 分检查点；
  #866、#867 合入，后续见 §17），并做第二轮多视角评审（架构、稳定性、简洁、跨平台、语义、框架、可观察性、兼容、性能、体验），
  22 条处理见 §18。主要变化：运行 mcpp 的约定写成一条生态规则，后端与 mcppls 共用（§4.0）；版本变化从 job 的信封得知，不再在
  每个 job 前另起进程（§4.2）；`features` 首批减为两项，并定义它与 `kinds`、`commands` 的分工（§4.3）；先行修复一节删除，§4 全部
  进入 0.2.0（§4.7）；IDE 与 mcppls 看到的 mcpp 不一致时报告（§5.2）；后端崩溃的恢复与可观察性（§3.1、§3.2）；用例的标识、
  动态与静态的对应、过滤参数的转义（§7.3）；自定义宏的覆盖改为按框架追加宏名（§7.3）；规则层在不受信任的工作区可用（§8）；
  协议升为 2（§8）；状态栏不新增项（§6.1）；#3 内部的检查点顺序（§14）。
- rev 4（本稿）：补测六项数据（§20），第三轮全局评审 12 条（§21）。主要变化：`features` 也写入每个信封的 `mcpp` 字段，探测只需一次
  `self env`（§4.3）；mcppls 运行 mcpp 时同样清除 `XLINGS_PROJECT_DIR`（§4.6）；`polyglot` 的签名顺序与缓存目录并入 SPEC-014 已有的
  单文件规则（§19.6.2）；`mcppls status` 预算有了实测依据（§5.2）；设备的默认路径有了 xbus 的实测延迟（§19.7）。
- rev 5（本稿）：设备路线已定（§19.8）；`polyglot` 以 `--pkg-format polyglot` 选择，不新增 `--carrier`（§19.6.2）；设备与 ok 生态的
  完整路线写入 openkal、openhal 各自的仓库与规范，本设计只保留分工与对它们的要求。
- rev 6：本轮的规范清单与 openlifa 一轮（§23，openlifa/openlifa#1）；第四轮多视角评审 10 条（§24），其中定下文件映射的
  最小接口（§23.2）、`polyglot` 与 `--pkg-format` 列表、`mcpp run`、`mcpp emit xpkg` 的关系（§19.6.2）。
- rev 7（本稿）：评审通过（`mcpp.path` 优先、文件映射的最小接口、`polyglot` 的三条边界）。clangd 的 ok 映像进入本轮
  （mcppls-clangd#12，§12、§25.3）；文件映射成为独立的可选接口 `openkal.map`（§23.2）；实施计划与任务依赖（§25）。

## 0. 摘要

mcpp 生态的编辑器侧由四层组成：mcpp（构建与项目）、mcppls（C++ 语言语义）、mcpp-ide 后端（mcpp 的项目知识与 IDE 会话）、各
IDE 插件（mcpp-vscode、mcpp-clion、mcpp-nvim）。0.2.0 之前，这四层的职责在三处不清楚：mcpp 的知识有一部分写在 VS Code 的 TS
里；插件对 mcppls 的依赖级别、版本门槛、信息通道三个 IDE 各不相同；后端对 mcpp 版本的判断不按项目，与项目实际运行的 mcpp
可能不是同一个。

本设计的模型：

- **分层（§3）。** 编译器能回答的归 mcppls；mcpp 自己的知识（清单、构建规则、项目、job、测试框架适配）归后端；插件只把后端的
  结果接到所在 IDE 的原生 API。
- **按项目的 mcpp（§4）。** 后端在项目目录内经 xlings shim 探测 mcpp，得到该项目实际使用的版本与能力；功能按 mcpp 声明的能力
  开关，不按版本号比较。
- **mcppls 契约（§5）。** 插件对 mcppls 保留安装依赖，不绑定版本；mcppls 新增 `mcppls status --root <dir> --format json`，后端
  读取并转给所有 IDE；功能按能力启用，缺一项只关一项。
- **入口与测试（§6、§7）。** 两个 IDE 提供同一组入口（`main` 旁运行与调试、右键、标题栏或工具栏、状态栏），入口的位置由命令表
  声明；测试有两个粒度：程序与用例，用例经后端的测试框架适配表发现与运行，只对确认属于该框架的程序列举。
- **清单与构建程序（§8）。** `mcpp.toml` 与 `build.mcpp` 的规则层从 TS 下沉到后端；后端的会话进程本身成为 LSP 服务器，服务
  四个编辑器；迁移以黄金输出保证 VS Code 不退化；无后端的平台保留声明的降级模式。
- **CLion 原生化（§9）。** 结构化的界面（项目树、库、缓存、设置）用原生组件渲染后端的视图模型；JCEF 只用于图与包详情页；删除
  浏览器里的 `vscode` 模拟层。
- **分发（§10）。** mcppls 单包的升级路径、各渠道上限、Windows 与 macOS 的安装细节在发布流程中验证。

## 1. 现状（实测）

| # | 事实 | 出处 |
|---|---|---|
| F1 | 后端每个会话只定位一个 mcpp（`mcpp.path`，PATH，xlings `subos/current/bin`），握手一次，握手不指定 cwd | mcpp-ide `src/mcpp.cppm` `locate`、`handshake`；`src/server.cppm` `locate_mcpp` |
| F2 | job 在项目根目录执行（`o.cwd = request.root`）；项目可用 `.xlings.json` 的 `workspace.mcpp` 锁定 mcpp 版本，xlings shim 在项目目录内选用锁定版本 | `src/jobs.cppm:240`；mcpp-ide 自己的 `.xlings.json` |
| F3 | 功能开关按握手版本比较：target 集合要求 `>= 2026.10.30.1` | `src/server.cppm:887` |
| F4 | `mcpp --protocol-version` 给出 `envelope`、`kinds`、`commands`，没有特性列表 | mcpp 2026.10.30.1 |
| F5 | VS Code 对 mcppls 是 `extensionDependencies`（0.9.0 起）；CLion 是 `<depends>`；Neovim 无依赖 | 各自清单 |
| F6 | 版本门槛两套：VS Code `VERIFIED_MCPPLS_MINIMUM = 0.0.4`，后端 `kRequired = 0.0.18` | `mcppls/contract.ts`、`src/languageservice.cppm` |
| F7 | VS Code 读 mcppls 扩展导出的 API 对象（`lastStatus()`、`otherCppFeatures()`），代码注释称"尽力而为，不是契约"；CLion 与 Neovim 只有后端的版本状态 | `mcppls/stateSource.ts`、`otherCppFeatures.ts` |
| F8 | mcppls 的命令行有 `serve`、`check`、`report`（等待引擎就绪，默认 60 s）、`print-environment`、`version`，没有轻量的状态命令 | mcppls `src/cli/commands.cpp` |
| F9 | `mcpp.toml` 有两份实现：VS Code TS（`src/toml`，7 类诊断、补全、hover、跳转）与后端 `src/manifest.cppm`（补全、hover、大纲，411 行）；0.2.0 中后端这份没有调用方；CLion 从未支持 `mcpp.toml` | `src/toml/*`、`src/manifest.cppm`、`plugin.xml` |
| F10 | `build.mcpp`：编译器层由 mcppls 提供；SPEC-007 规则、指令补全与片段在 VS Code TS（`src/buildscript`） | mcppls docs/10；mcpp-vscode docs/build-script.md |
| F11 | VS Code 没有 CodeLens、没有 `editor/context`；`editor/title` 只有 Run 与 Test；测试项的位置固定在文件第 0 行 | `package.json` menus；`src/ide/testing.ts:47` |
| F12 | CLion 有 `main` 旁 ▶ 与上下文运行配置，`main` 用正则识别；没有状态栏组件；锤子按钮对 mcpp 配置是否可用未验证 | `run/McppProducers.kt` |
| F13 | CLion 的工具窗口与设置页是 JCEF 中的 VS Code 页面，浏览器里模拟 `vscode` 模块；设置页 `isModified()` 恒为 false、`apply()` 为空 | `webui/src/vscode.ts`、`settings/McppConfigurable.kt` |
| F14 | 命令表 56 行；CLion 不处理 `forward`（21 行）、`problems`、`editor`、`quickMenu`，点击后回答"本 IDE 不可用" | `spec/commands.json`、`actions/Dispatch.kt` |
| F15 | VS Code 在没有后端时用 `execFile` 直接运行 mcpp 与 mcppls（`runMcpp` 的 7 个调用方） | `src/cli/process.ts` |
| F16 | 后端已有视图模型（项目摘要、库、缓存、工具链、targets、两种图）、`settings/registry`、`commands/list` | `src/views.cppm`、`src/server.cppm` |
| F17 | mcpp 只认程序级测试：一个 `tests/**/*.cpp` 是一个程序，退出码为约定，用例选择经 `--` 交给程序 | mcpp docs/08 "Current limitations" |
| F18 | xlings canary 47 min，其中测试程序的构建 1750 s、运行 57 s；与前五次发布（40–51 min）一致 | release run 40999598911 |

## 2. 原则

1. **一份知识一个归属。** mcpp 的知识（清单的键、构建程序的规则、测试框架的参数、项目与 job）只在后端实现一次；C++ 语义只在
   mcppls 实现一次。插件不实现判断。
2. **按能力，不按版本。** 一方的版本只用于说明；功能是否可用由对方声明的能力决定。缺一项能力只关掉依赖它的功能。
3. **硬依赖不等于强绑定。** 安装上可以必需；运行时任何版本组合都能工作，差异只表现为能力的有无，并在界面上说明。
4. **原生优先。** 结构化的界面用 IDE 的原生组件；只有富可视化内容用网页。
5. **入口一致。** 两个 IDE 的入口集合相同，形式各用原生方式。
6. **VS Code 不退化。** 迁移以黄金输出与 0.9.0 一致性门禁验收。

## 3. 分层

| 层 | 负责 | 不负责 |
|---|---|---|
| mcpp | 构建、项目、测试程序、机器输出与能力声明（`--protocol-version`）、`self env` | 编辑器、测试框架的用例 |
| mcppls | C++ 语义（含 `build.mcpp` 的编译器层）、kit、engine；状态命令与动作声明（§5.2）；编辑器中的文档符号（§6.3 的位置来源） | mcpp 的清单与规则、测试框架 |
| mcpp-ide 后端 | 按项目的 mcpp（§4）；mcppls 状态的汇总（§5）；清单与构建规则（§8）；测试框架适配（§7）；视图模型；入口判定（§6） | IDE 的界面 |
| 插件 | 把后端的结果接到 IDE 的原生 API；入口与界面的原生形式 | 任何判断 |

VS Code 与 CLion 的插件包内携带后端，插件与后端版本恒等。Neovim 的后端是独立的 xlings 包（`mcpp-ide-nvim` 依赖
`xim:mcpp-ide`，当前不带版本）：改为依赖同一版本（`xim:mcpp-ide@<version>`），插件在 `initialize` 时核对后端的 `protocol` 并报告
不一致。由此插件与后端之间不存在版本错配；跨版本的边界只有两条：后端与 mcpp（§4）、后端与 mcppls（§5）。

### 3.1 后端的生命周期

后端由插件启动并持有。后端退出时，它启动的 mcpp job 随之结束（job 以 `bound_lifetime` 启动），界面把这些 job 报告为"后端已退出"
而不是失败；插件以退避重启后端（1 s、4 s、16 s），3 分钟内第 4 次退出后停止重启，进入 §8 的降级模式并给出"重新启动后端"与
"导出报告"两个动作。

### 3.2 可观察性

- **为什么不可用。** 每个被隐藏或置灰的功能都带原因：缺哪一项能力、来自哪一方（项目的 mcpp、mcppls、后端所在平台）、需要的
  版本；界面的提示与 `selfCheck` 读同一份数据（后端 `capabilities/explain`）。
- **报告。** 命令"mcpp：导出 IDE 报告"写一个脱敏的 zip：后端版本与协议、每个项目的探测结果（`self env` 与能力）、`mcppls status`、
  生效的设置、最近的后端日志与 job 记录；三个 IDE 同一个命令（命令表一行）。
- **日志。** 后端日志级别由设置 `mcpp.backend.logLevel` 决定，日志写入各 IDE 的日志位置（VS Code 的 output channel、CLion 的
  `idea.log`、Neovim 的 `:messages` 与日志文件）。

## 4. 按项目的 mcpp

### 4.0 运行 mcpp 的约定（生态规则）

任何为某个项目运行 mcpp 的工具（mcpp-ide 后端、mcppls、IDE 插件的降级模式）都这样运行：当前目录为项目根；程序名为 `mcpp`，
经 PATH 找到（即 xlings 的 shim），或用户显式设置的路径；环境中清除 `XLINGS_PROJECT_DIR`。这样版本的选择完全由项目目录决定，
不需要任何工具理解锁定文件。这条规则写入 mcpp docs/50（"工具怎样调用 mcpp"），后端与 mcppls 各自引用并以测试固定。

### 4.1 缺陷

F1–F3 合起来：握手在项目外执行，得到全局版本；job 在项目内执行，用锁定版本。面板显示的版本是错的；功能开关按错误的版本判断，
可能提供项目的 mcpp 不支持的功能；`mcpp.path` 指向具体文件时项目的锁定被静默绕过。

### 4.2 探测

- 探测以**项目**为单位：在项目根目录执行一次 `mcpp self env --format json`（信封带版本与 `features`，§4.3；更早的 mcpp 另执行
  `--protocol-version`）。执行始终经 shim、在项目
  目录中进行；后端不把 shim 解析成具体路径后缓存直接调用。
- 缓存以项目根为键，值为探测结果（版本、`program`、能力）。
- 失效：项目的 `mcpp.toml` 变化时、窗口重新获得焦点时重新探测。版本在两次探测之间改变（`xlings use`、锁定改动）时，下一个 job
  的信封会说出来：mcpp 的每个 JSON 信封都带 `mcpp.version`，与缓存不同即重新探测，不在 job 前另起进程。后端不读取、不监视
  `.xlings.json`：在项目目录内运行时 mcpp 的回答就是该项目的事实。
- 环境：按 §4.0 清除 `XLINGS_PROJECT_DIR`。xlings 先从 cwd 向上找 `.xlings.json`，找不到才读这个变量；IDE 从某个项目的终端启动时
  继承了它，没有锁定的项目就会用上另一个项目的锁定。subos 的选择（`XLINGS_ACTIVE_SUBOS` 等）保留，那是用户的选择。
- 锁定的版本未安装：shim 拒绝运行并给出 xlings 统一的"未安装"信息，不自动安装。探测结果为 `pinned-missing`，界面给出
  `xlings install mcpp@<version>`；构建入口在此状态下置灰并说明原因。
- 代价：按需探测（项目第一次被用到时），并发不超过 2；含多个项目的工作区启动时不一次性探测全部。
- 信任：探测执行项目锁定的 mcpp，只在受信任的工作区进行（VS Code workspace trust、CLion trusted project）；不受信任时显示全局
  mcpp 的信息并标注"未在项目内探测"。
- 设置了 `mcpp.path` 时执行按它（用户的显式选择），界面标注"由设置指定，不经项目的版本选择"；后端不另去探测 shim 作比较。

### 4.3 能力（mcpp 侧的改动）

- `mcpp --protocol-version` 新增 `features`：客户端据以开关功能的能力，名字稳定、只增不改；一个名字在有客户端依据它开关
  某项功能时才加入，文档写明使用方。首批两项：`target-sets`（后端的 target 选择与腿，SPEC-015 §1）、`pkg-format-list`（打包
  界面的多选，SPEC-014 §4.6）。
- 三者的分工：`kinds` 说输出的 schema，`commands` 说有哪些命令及其效果，`features` 说已有命令中的语义能力；一项能力只放在
  其中一处。
- `features` 同时写入每个 JSON 信封的 `mcpp` 字段（今天为 `{"version", "protocol"}`，§20 D2）：后端的探测只需一次
  `self env --format json`（经 shim 每次调用约 25–29 ms，§20 D1），任何 job 的信封也带着版本与能力，版本或能力变化时重新探测。
- 早于 `features` 的 mcpp：后端带一张冻结的对照表（已发布版本 → 能力；2026.10.30.1 为上面两项），只用于这些旧版本；
  有 `features` 的 mcpp 一律以其回答为准。
- `mcpp self env --format json` 的 `data` 新增 `program`（实际运行的程序的绝对路径）。
- 后端的所有版本比较（F3）改为查 `features`；`kContractFloor` 保留为"契约存在"的判断（早于契约的 mcpp 没有 `--protocol-version`）。

### 4.4 展示

后端的 `project/mcpp` 方法返回下表，三个 IDE 的项目视图"基本信息"与状态栏读它：

| 字段 | 来源 |
|---|---|
| 版本、程序路径 | `self env`（`mcppVersion`、`program`） |
| `mcpp.path` 是否生效 | 设置项；生效时标注"由设置指定" |
| 项目要求的下限及是否满足 | `mcpp.toml` 的 `mcpp = ">=…"`（由 mcpp 的 `manifest/check` 报告，后端不解析清单） |
| 协议能力 | `--protocol-version` 的 `features`、`kinds` |
| MCPP_HOME、默认工具链、xlings 的位置 | `self env` |
| 可用的新版本 | 可选；只提示，不改锁定 |

### 4.5 冲突与修复

| 情形 | 显示 | 修复动作 |
|---|---|---|
| 项目内的 mcpp 低于项目下限（`mcpp.toml` 的 `mcpp = ">=…"`，由 mcpp 报告） | "项目需要 ≥ X，项目内为 Y" | 复制 `xlings install mcpp@X` |
| 设置了 `mcpp.path` | "由设置指定：X" | 清除设置 |
| 锁定的版本未安装（`pinned-missing`） | "项目所需的 mcpp 未安装" 及 xlings 的原话 | 复制 `xlings install mcpp@<version>` |
| 缺少某个 feature | 对应功能隐藏或置灰，提示需要的 mcpp 版本 | — |

### 4.6 mcppls 与同一个 mcpp

mcppls 调用 mcpp（`emit build-database`）时按 §4.0 运行，使 IDE 后端与 mcppls 看到同一个 mcpp。核实（§20 D4）：mcppls 经 PATH
找到 `mcpp`（shim 本身）、以项目根为工作目录运行，符合；它使用捕获的登录 shell 环境，未清除 `XLINGS_PROJECT_DIR`，本轮补上，并以
conformance 夹具固定（§13）。两者仍不一致时（例如 mcppls 设置了自己的 mcpp 路径），后端比较探测结果与 `mcppls status` 的
`workspace.mcpp`，在项目视图中报告"IDE 与语言服务使用的 mcpp 不同"。

### 4.7 版本归属

§4 全部进入 mcpp-ide 0.2.0（#3，§14）。mcpp 2026.10.31.1 发布前，0.2.0 对 2026.10.30.1 及更早的 mcpp 使用冻结对照表；
发布后以 `features` 为准，两条路径都有测试。

## 5. mcppls 的依赖与契约

### 5.1 依赖级别

| IDE | 安装依赖 | 运行时 |
|---|---|---|
| VS Code | 保留 `extensionDependencies`（安装 mcpp-vscode 即装 mcppls） | 按能力 |
| CLion | 保留 `<depends>` | 按能力 |
| Neovim | 不变（提示 `xlings install mcppls-nvim`） | 按能力 |

两种清单的依赖声明都不带版本，版本只在运行时处理。VS Code 中删除 `VERIFIED_MCPPLS_MINIMUM`（F6），门槛只在后端一处：最低
支持 0.0.18（第一个读取 `.last-result.json` 的版本）；0.0.18 至 0.0.19 没有 `status`，使用 §5.3 的冻结能力表。

### 5.2 `mcppls status`

新命令：`mcppls status --root <dir> --format json`，毫秒级、不启动引擎、不等待就绪，输出 schema 版本化（S3 新增一节）：

```json
{
  "schema": 1,
  "server": { "version": "0.0.20", "platform": "linux-x64", "payload": "<dir>" },
  "capabilities": ["status", "kit-install", "module-graph", "other-cpp-features"],
  "workspace": {
    "root": "<dir>",
    "state": "ready | starting | degraded | stopped | none",
    "since": "2026-10-30T12:00:00Z",
    "engine": { "name": "clangd", "version": "23.1.0-mcppls.5", "restarts": 0 },
    "kit": { "name": "mcppls-kit-x86_64-linux-gnu", "state": "ok | missing", "install": "xlings install mcppls-kit-…@…" },
    "mcpp": { "version": "2026.10.30.1", "program": "<path>" },
    "problems": [{ "code": "kit-missing", "message": "…" }]
  },
  "instances": [{ "pid": 4242, "editor": "vscode", "state": "ready", "since": "…" }],
  "actions": [{ "id": "openLogDirectory", "title": "…", "vscode": "mcppls.revealCacheDirectory", "vscodeArguments": ["logs"] }]
}
```

其他 C++ 语言服务是否启用是编辑器的事实（VS Code 读取已安装扩展及其设置），命令行的 `status` 无从得知，不在文档中。

- 运行中的服务器在工作区缓存目录维护自己的状态，`status` 读取它并补上静态事实（版本、payload、kit 的解析）；状态文件是 mcppls
  的实现细节，契约是命令的输出。没有服务器在运行时 `state` 为 `none`。
- 同一工作区可能有多个服务器（两个编辑器同时打开）：输出 `instances[]`（`pid`、`editor`、`state`、`since`），进程已不存在的实例
  忽略；`workspace.state` 取存活实例中最好的一个。
- `actions[]`：mcppls 声明自己的动作及其在各编辑器中的 id（`{"id": "restart", "vscode": "mcppls.restartServer", "jetbrains":
  "…", "nvim": "…"}`），VS Code 命令的参数为 `vscodeArguments`。动作 id 由 mcppls S3-8-7 列出；`installKit` 只在工作区的
  kit 缺失时出现，参数与命令行都写明目标。动作 id 归 mcppls，mcpp-ide 不保存另一个产品的 id。
- 后端的 `languageService/status` 改为执行 `mcppls status`（经后端已有的 mcppls 定位，限时 5 s），返回版本、能力、状态与可用
  动作（`statusSource` 为 `status`、`frozen` 或 `none`）；三个 IDE 显示同一份信息。结果与上次不同（程序、版本、动作或工作区
  状态）时后端发出一次 `languageService/changed`。VS Code 不再读 mcppls 扩展的 API 对象（F7）。
- 刷新由事件驱动：job 结束、窗口获得焦点、面板打开时执行一次；面板可见时另有不短于 30 s 的轮询，不可见时不轮询。一次执行包括
  okrun 启动 okb，预算 200 ms 以内；实测 okrun 启动 15 MB 的 `mcppls.okb` 执行一条轻量命令约 35 ms（Linux x64，§20 D3）。
- `--root` 的比较按系统规范化：Windows 上盘符与分隔符统一、Windows 与 macOS 上不区分大小写；后端与 mcppls 用同一规则（S3 写明）。

### 5.3 动作

| 动作 | VS Code | CLion | Neovim |
|---|---|---|---|
| 重启、重启引擎、重置缓存、导出诊断包、显示日志 | 能力表转发 mcppls 命令（已有） | `ActionManager` 按 id 调用 mcppls 插件的 action；不在时隐藏 | 调用 mcppls-nvim 的命令 |
| 安装 kit | 转发 `mcppls.installKit` | mcppls 插件的安装 action | `xlings install …` |
| 关闭其他 C++ 服务 | 转发（已有） | mcppls 插件的 `UseMcpplsOnly` 与 `RestoreClionEngine` | 不适用 |

命令表（`spec/commands.json`）的转发行写 mcppls 的动作 id（`{"kind": "forward", "mcppls": "restart"}`），后端的 `commands/list`
按编辑器取出该动作在此编辑器中的 id（`action.command`，VS Code 另带 `arguments`），mcppls 没有给出 id 的行不列出；收到
`languageService/changed` 的编辑器重新取命令表。VS Code 的能力表（`contract.ts`）以 mcppls 的动作 id 为键，只保存标题、确认
等级（`danger`）与缺失时的说明，命令 id 与参数来自 `languageService/status` 的 `serverActions`。没有 `status` 的 mcppls
（0.0.18–0.0.19）由后端的冻结表给出这两个版本的动作 id。CLion 不再回答"本 IDE 不可用"，没有对应 action 的行不出现在菜单中
（F14）；Neovim 只列出 mcppls-nvim 已定义的命令。

### 5.4 升级规则

1. 契约版本化（`schema` 加 `capabilities`），后端按能力启用功能；未知能力忽略，缺失能力隐藏对应功能。构建后的刷新
   （`mcpp.languageService.refreshAfterBuild` 的 `auto`）不再转发：0.0.18 起 mcppls 读取结果记录后自行刷新；`reload` 与
   `restart` 仍转发对应动作。
2. 后端不持有 mcppls 进程，只在调用时执行；mcppls 升级、重启、路径改变不影响 mcpp 插件，路径变化经 VS Code
   `extensions.onDidChange` 与 CLion 的插件状态监听重新定位。
3. mcppls 被禁用或未安装时，mcpp 插件的构建、运行、测试、项目页面照常工作；面板显示"C++ 语言服务未启用"与启用或安装动作。
4. 兼容性 CI：mcpp-ide 对 mcppls 最新发布版与最低支持版（0.0.18）各运行一次契约测试；mcppls 对最新的 mcpp-ide 后端运行一次。

## 6. 入口

### 6.1 对齐表

| 入口 | VS Code | CLion |
|---|---|---|
| `main` 旁运行与调试 | CodeLens "Run \| Debug"（新增，设置 `mcpp.codeLens.enable` 默认开） | gutter ▶（已有，改判定，§6.3） |
| 测试程序的 `main` | 测试项位置为 `main` 所在行（改 F11） | 同一 producer 生成 `mcpp test <name>`（已有） |
| 右键 | `editor/context`：Run、Debug、Build、Test（新增） | Run、Debug（已有）；Build（新增） |
| 标题栏或工具栏 | `editor/title`：Build、Run、Debug、Test（补 Build、Debug） | 运行组件（已有）；锤子接到 mcpp build（验证后实现） |
| 状态栏 | 已有的 mcpp 项显示项目的 mcpp 版本（文字与提示），不新增项；job 状态与快速菜单不变 | 一个状态栏组件，内容与 VS Code 的项相同（新增） |
| 快捷键 | `Ctrl+Alt+B/R/T`（已有） | 同一组，按 CLion 的 keymap 方式注册 |

### 6.2 命令表声明位置

`spec/commands.json` 每行新增 `placements`，一个以位置为键、以次序为值的对象（`{"title": 3, "context": 2, "gutter": 2}`；
位置为 `title`、`context`、`gutter`，快速菜单仍由 `quickMenu` 表示）。两个插件按表生成原生入口：VS Code 的 `editor/title`
与 `editor/context`，CLion 编辑器右键菜单中的一组引用（`EditorPopupMenu`）；新增命令时两边同时出现。VS Code 的清单由命令表生成（`mcpp-ide-dev gen vscode`），0.9.0 的一致性门禁允许新增、不允许改变：0.9.0 已有的条目
（例如 `editor/title` 的 Run、Test 及其 `when` 中的 `mcpp.editorTitleButtons`）逐字保留，新增的入口另起条目。`mcpp.build`
在 0.9.0 的清单中没有图标，标题栏需要图标，因此标题栏的构建是新命令 `mcpp.editor.build`（只在 VS Code，不进命令面板），
`mcpp.build` 的条目不变。

CLion 的快捷键以两个随插件提供的 keymap 表示（`mcpp`，父为 `$default`；`mcpp (macOS)`，父为 macOS 的 keymap），由命令表的
`keybinding` 生成：CLion 自带的 keymap 中这些组合键已有绑定（`Ctrl+Alt+B` 是 Go to Implementation），不覆盖；需要 VS Code
组合键的人在 Settings | Keymap 中选择。锤子（Build | Build Project）经 `ProjectTaskRunner` 对含 `mcpp.toml` 的项目执行
`mcpp build` 作业。

### 6.3 `main` 的判定

`main` 的位置来自正在服务这个文件的语言服务：CLion 用 PSI（▶ 落在后端读出的那一行上名为 `main` 的 PSI 元素），VS Code 用
当前语言服务器（mcppls）给出的文档符号（`vscode.executeDocumentSymbolProvider` 中顶层名为 `main` 的函数）。mcppls 的命令行
查询（`query`）要启动引擎，不适合在打开文件时调用，不采用。后端新增 `source/entries`（文件 → 程序还是测试、测试名、框架），只回答
种类，不回答位置。CLion 的正则（F12）与 VS Code 的第 0 行（F11）都改为"位置取自语言服务、种类取自后端"。语言服务不在
（mcppls 被禁用）时，位置取自后端的文本扫描（与 §7.3 的扫描同一实现），并在提示中标注为近似。

## 7. 测试：程序与用例

### 7.1 两个粒度

| 粒度 | 定义 | 运行 |
|---|---|---|
| 程序 | 一个 `tests/**/*.cpp`（mcpp 的约定，不变） | `mcpp test <name>` |
| 用例 | 程序内某个测试框架的一个测试 | `mcpp test <name> -- <框架的过滤参数> <框架的报告参数>` |

mcpp 核心不变（F17）：退出码是约定，`--` 后的参数原样交给程序；SPEC-015 §4 的归档运行同样透传。

### 7.2 测试框架适配表（后端）

| 框架 | 识别（依赖 + 宏） | 列举 | 过滤 | 报告 |
|---|---|---|---|---|
| GoogleTest | `compat.gtest`；宏 `TEST`、`TEST_F`、`TEST_P`、`TYPED_TEST` | `--gtest_list_tests` | `--gtest_filter=Suite.Name` | `--gtest_output=json:<file>` |
| Catch2 | `compat.catch2`；宏 `TEST_CASE`、`SCENARIO` | `--list-tests --reporter json` | 测试名作参数 | `--reporter json::out=<file>` |
| doctest | `compat.doctest`；宏 `TEST_CASE`、`SUBCASE` | `--list-test-cases` | `--test-case=<name>` | `--reporters=junit --out=<file>` |

首批是 mcpp-index 提供的框架（gtest 1.18.0、Catch2 3.15.2、doctest 2.4.12；Catch2 的 JSON reporter 自 3.5 起可用，§20 D6）。
`boost-ext.ut`（2.3.1）在核实其 runner 支持命令行过滤后加入，否则只提供程序级；index 不提供的框架（如
Boost.Test）不适配，其测试按程序级运行。

适配表是后端的数据文件（`spec/test-frameworks.json`），新增框架只加一行。一个测试程序属于某个框架，需要两个条件同时成立：
它链接该框架的包（经 mcpp 的 build database），且它的源文件含该框架的宏（§7.3）。

### 7.3 发现

- **静态**：后端按适配表对文件做文本扫描（跳过注释与字符串），找出框架宏的调用位置；宏名有辨识度（行首的 `TEST(`、
  `TEST_CASE(`），打开文件即可显示 ▶，不需要构建，也不依赖 mcppls。
- **动态**：构建后运行列举命令校正测试树（参数化、类型化与宏展开生成的用例）。**只对静态扫描找到了该框架宏的测试程序执行**：
  一个依赖了 gtest 却只写 `main` 的测试程序，收到 `--gtest_list_tests` 会把测试当场运行一遍，因此不能仅凭依赖判定。
- 框架按测试程序判定（同一项目中可以有框架测试与无框架测试并存）。用例写在自定义宏里时，设置 `mcpp.test.macros`（例如
  `{"gtest": ["MY_TEST"]}`）为该框架追加宏名；不提供"强制指定框架"，以免对不含该框架的程序执行列举。
- **用例的标识**：（项目、测试程序、框架、全名），全名取框架的写法（gtest 的 `Suite.Name`，Catch2 与 doctest 的测试名）。动态列举
  的用例按全名挂到静态节点上：gtest 的参数化用例去掉实例前缀与参数后缀（`Inst/Suite.Name/0` → `Suite.Name`）后匹配；匹配不到的
  挂在测试程序节点下。同一程序中重名的用例以出现顺序区分。
- **过滤参数**按框架的语法转义：Catch2 测试名中的 `,`、`[`、`]`、`*`、`\`，doctest 的 `,`；gtest 的全名是两个标识符，不含其
  过滤语法的特殊字符，不转义。只来自源文件（未经列举）的 gtest 用例名同时匹配其实例：`S.N` 展开为 `S.N`、`*/S.N/*`（`TEST_P`）
  与 `S/*.N`（`TYPED_TEST`）。参数以 argv 传递，不经 shell。报告文件写在每次运行独立的临时目录，运行结束后删除。
- 列举与运行都执行项目的程序，只在受信任的工作区进行。
- 后端方法：`test/cases`（文件或程序 → 用例、位置、来源）、`test/runCases`（程序与用例 → job，结果按用例映射）。

### 7.4 IDE

| | VS Code | CLion |
|---|---|---|
| 用例节点 | Testing API 的子项，位置为宏所在行；▶ 与 ✓/✗ 由 VS Code 显示 | 每个宏一个 `RunLineMarkerContributor`；上次结果来自 `TestStateStorage` |
| 结果树 | Testing 视图 | SMTestRunner，后端事件转为 TeamCity service message |
| 调试一个用例 | 同一组参数，调试类型 `mcpp` | 同一组参数，CLion 调试器 |

调试需要测试程序的启动描述：mcpp 2026.10.31.2 增加 `mcpp test <name> --print-launch`（特性 `test-launch`，docs/50），输出与
`mcpp run --print-launch` 相同的 `mcpp.launch` 信封，`--` 之后是框架的过滤参数。这是 F17 之外 mcpp 核心唯一的变化：测试的
判定（退出码）与参数透传不变，只增加描述启动的方式。项目的 mcpp 没有 `test-launch` 时，测试只提供运行。
| 无框架的测试 | 粒度为程序，▶ 在 `main` 行 | 同左 |

## 8. 清单与构建程序的规则层

- **归属**：后端。`mcpp.toml` 的补全、hover、跳转与 7 类诊断规则，`build.mcpp` 的 SPEC-007 规则、指令补全与片段，从 TS（F9、F10）
  移到后端，与现有 `src/manifest.cppm` 合并为一份；`manifest/check`（mcpp 自己的校验）保留。
- **入口：同一进程。** 后端已是 LSP 基础协议（Content-Length 分帧）上的 JSON-RPC 2.0。会话本身成为 LSP 服务器：`initialize`
  采用 LSP 的形状，mcpp-ide 的参数放在 `initializationOptions`；现有方法改名为自定义请求 `mcpp-ide/*`；规则层以标准的
  `textDocument/*` 提供。VS Code 用 languageclient、CLion 用平台 LSP API 连接这一个进程；Neovim 与 Zed 把它配置为 `mcpp.toml`
  的语言服务器。不另起 `mcpp-ide lsp` 进程，避免两份项目状态与两次 mcpp 探测。
- **协议**：`initialize` 改形与方法改名使后端协议升为 2（`kProtocol = 2`，`protocol/protocol.schema.json` 与夹具同步）。插件与后端
  版本恒等（§3），无需兼容协议 1。
- **信任**：规则层只分析文本、不执行任何程序，在不受信任的工作区同样可用（与 0.9.0 的 TS 实现相同）；`manifest/check` 运行 mcpp，
  只在受信任时执行。
- **分工**：`build.mcpp` 的编译器层仍归 mcppls，两个服务器同时服务该文件，按 F10 的让位规则分工（mcppls 在场时后端不报
  `unknownSymbol`、不补全 import 后的模块名）。
- **迁移验收**：以现有 TS 实现在固定样例集（`mcpp.toml`、`build.mcpp` 各一组，覆盖 7 类规则与全部补全位置）上生成黄金输出（诊断、
  补全项、hover 文本、跳转位置），后端输出逐项一致后删除 TS 实现；设置开关（`mcpp.buildScript.*`、严重度）原样保留，作为参数
  传给后端。
- **实现（rev 9）**：VS Code 以 languageclient 连接（打包进 `dist/vendor`，VSIX 不带 node_modules）；Neovim 以 `vim.lsp`
  连接并附着到 `mcpp.toml`、`build.mcpp` 的缓冲区；CLion 的平台 LSP API 总是自行启动它所连接的服务器进程，与"同一进程"
  冲突，故 CLion 经会话已有的连接发送 `textDocument/*`，以补全贡献者、外部标注器、文档提供者与跳转处理器呈现结果。规则层
  合并时取两份实现中较完整者：表名经 schema 解读（`workspace.<表>` 即其镜像的表，选择器下的平台表即 `platform.<表>`，命名
  条目取最长的已知前缀），只在选择器下出现的表带选择器给出，schema 自身的数组表不报错，依赖版本取自 `mcpp search --format
  json` 的目录。黄金输出由 TS 实现在样例集的每个位置生成（含扫描器的结构与光标上下文、其解析器测试的全部文档），后端逐项一致
  后删除 TS 规则层；`src/toml/parser.ts` 只保留供项目页读取声明的结构读取。
- **无后端时（F15）**：后端经 okrun 运行，okrun 覆盖 Linux x86_64 与 aarch64、macOS arm64、Windows x86_64；VS Code 还运行在
  macOS x86_64、Windows arm64、Linux armhf 上。回退保留，定义为声明的降级模式：界面显示"后端不可用（本平台）"，只提供构建、运行、
  测试与项目页面，CI 有一个任务专门以无后端方式运行 Extension Host 测试。okrun 覆盖新平台后，降级模式在该平台自动不再出现。

## 9. CLion 原生化

沿用 mcpp-vscode 的信息结构（项目、库、缓存三个视图，面板，命令表，设置），实现用 CLion 的方式：

| 部分 | 实现 | 数据 |
|---|---|---|
| 项目树、库列表、缓存 | 工具窗口中的原生 Tree、List | 后端视图模型（F16） |
| 设置 | 原生 `Configurable`，由 `settings/registry` 生成表单；支持应用、取消与设置搜索（改 F13） | `settings/registry`、`settings/update` |
| 模块图、依赖图、包详情页 | JCEF 中的独立小页面，只接收后端数据 | `graph/*`、`library/*` |
| 运行、调试、测试 | 运行配置、producer、SMTestRunner（§6、§7） | 后端 |
| `mcpp.toml`、`build.mcpp` | 平台 LSP API 接后端（§8）；高亮由 CLion 自带的 TOML 插件提供 | 后端 |

界面字符串来自后端的 `i18n/bundle`，与 VS Code 页面共用一份翻译。删除 `webui/src/vscode.ts` 与桥接层；`webui` 只保留图与包详情页。VS Code 的界面不变。测试：后端视图模型的单元测试，加 CLion
headless 测试（运行配置的生成、gutter 入口、设置的应用与取消、工具窗口的渲染）。

## 10. mcppls 的分发与安装

体积不是约束；以下各项在 mcppls 0.0.20（#75）中逐项验证或实现：

| # | 项 | 做法 |
|---|---|---|
| D1 | 从 0.0.19 升级（每平台一个 payload → 一个资源；`mcppls-nvim` 依赖 `mcppls`） | 干净环境中先装 0.0.19 再升级，校验 `mcppls`、`mcppls-nvim` 与 VS Code、CLion 插件 |
| D2 | 渠道上限 | VS Marketplace、Open VSX、JetBrains Marketplace、GitCode release 附件的大小上限，发布前核对 |
| D3 | Windows 路径长度 | payload 中最长路径加安装目录不超过 260；CI 检查最长路径 |
| D4 | Windows 未签名程序 | `okrun.exe`、启动程序的 SmartScreen 与杀毒软件行为写入文档；签名另立 |
| D5 | 用户目录含空格或非 ASCII | CI 的安装测试使用这样的目录 |
| D6 | macOS quarantine | 浏览器下载的单文件与 zip：启动器检测并给出 `xattr -d com.apple.quarantine` 的说明；VSIX 与 xlings 安装不受影响 |
| D7 | Linux 覆盖 | 原生 clangd 的 glibc 基线（2.31）写入文档；`clangd.okb`（§25.3）使覆盖等于 okrun 的宿主，musl 发行版随之覆盖 |
| D8 | 运行中更新 | 只涉及 xlings 安装（IDE 插件更新由 IDE 放到新目录）：替换 payload 前停止服务器；Windows 上被占用的文件以"新目录 + 切换"替换 |

## 11. CI 效率

- `release_canaries.py` 在每个 canary 结束后打印 `.ninja_log` 中耗时最长的 20 个步骤（编译或链接、目标）。
- 依据数据再定：dev 配置的链接使用 lld 或 `-gsplit-dwarf`（mcpp 的优化，所有项目受益），或 canary 只运行代表性分片。F18 的
  1750 s 构建中链接所占比例在下一次发布后写入本节。

## 12. 不在本轮

- mcpp-vscode 0.9.0 的字节级一致性门禁何时解除：本轮不变。

clangd 的 ok 映像（mcppls-clangd#12）在本轮：三个阻塞项按所属的层拆分，见 §25.3。

## 13. 测试与验收

| 项 | 验收 |
|---|---|
| §4 | 后端单元测试：项目内外 mcpp 版本不同（以两个 mockmcpp 与假 shim 模拟）、`pinned-missing`、`mcpp.path`、继承的 `XLINGS_PROJECT_DIR` 被清除、job 信封的版本变化触发重新探测；`features` 与冻结对照表两条路径；VS Code、CLion 显示项目的实际版本 |
| §3 | 后端被杀后的退避重启与降级；"导出 IDE 报告"的内容与脱敏；每个置灰功能都有原因 |
| §4.6 | mcppls conformance 夹具：锁定版本的项目中 mcppls 调用的 mcpp 版本等于锁定版本 |
| §5 | `mcppls status` 的 schema 测试（含两个实例、一个已退出的实例）；后端对 0.0.18、0.0.20（无 `status`）与最新 mcppls 的契约测试；禁用 mcppls 后 mcpp 插件的构建与运行正常 |
| §6 | 入口对齐表每格一个 IDE 级测试（VS Code Extension Host、CLion headless） |
| §7 | gtest、Catch2、doctest 示例项目：每个用例旁 ▶ 可运行、可调试，运行后显示 ✓/✗；gtest 参数化用例挂到 `TEST_P` 行；名字含特殊字符的用例可单独运行；无框架测试的 ▶ 在 `main` 行；依赖 gtest 而只写 `main` 的测试程序不被列举；归档运行按用例过滤 |
| §8 | 黄金输出逐项一致；不受信任的工作区中规则层可用；Neovim 与 Zed 经 LSP 获得 `mcpp.toml` 的补全与诊断；无后端的 Extension Host 任务通过 |
| §9 | CLion headless：设置的应用与取消、设置搜索、原生树的渲染 |
| §10 | D1–D8 各一条发布检查 |

## 14. 交付与级联

未发布的 PR 追加本设计的内容，版本号不变；已发布的仓库新开一个 PR，按检查点分多个提交。

| 仓库 | PR | 版本 | 内容 |
|---|---|---|---|
| mcpp-ide | #3（未发布，追加） | 0.2.0 | §4 按项目探测；§5 契约与动作；§6 入口；§7 测试；§8 规则层与 LSP；§9 CLion 原生化；`mcpp-ide-nvim` 精确依赖同版本后端；mcppls 0.0.20 单包依赖（`49ae757`） |
| mcppls | #75（未发布，追加） | 0.0.20 | §5.2 `status`（含 `instances`、`actions`）；§4.6 核实与夹具；§10 D3–D8 |
| mcpp | 新 PR（2026.10.30.1 已发布） | 2026.10.31.1 | 检查点提交：§4.3 `features` 与 `self env` 的 `program`；mcpp#868；§17 的后续；§11 canary 诊断与 §4.0 的 docs/50 规则；§19.6.2 的载体模型 `polyglot`（`--pkg-format polyglot`、SPEC-014 的条款）；§25.3 的 C1（预构建归档是链接输入） |
| openkal 家族 | 规范、openkal-linux、openkal-macos、openkal-windows、openkal-image、openkal-musl、openkal-llvm-runtime、openkal-platform、mcpplibs.os 各一个 PR，按家族的精确钉住级联发布 | 次版本 | `openkal.map`（§23.2）及其实现；okrun 以映射读入映像；§25.3 定位到这些仓库的修复 |
| mcppls-clangd | 一个 PR | 0.0.18 | clangd 的 `<arch>-openkal` 映像与 `clangd.okb`（§25.3） |
| xim-pkgindex | 一个 PR | — | mcpp、openkal 家族、mcppls、mcpp-ide 的条目 |

按能力解耦后发布之间没有硬顺序；mcpp 先发布，使 mcppls 与 mcpp-ide 发布时即可用上 `features`。

mcpp 的新 PR 按检查点提交：① `features` 与 `program`；② #868；③ §17 的两项；④ §11 与 docs/50 的 §4.0 规则；⑤ 载体模型 `polyglot`。
#3 内部按检查点提交，每个检查点 CI 通过后再进行下一个：① §4 与 §3.1、§3.2；② §5 的消费端；③ §6 入口；④ §7 测试；⑤ §8 规则层
（先生成黄金输出，再迁移，再删除 TS）与协议 2；⑥ §9 CLion 原生化。

## 15. 评审项

1. 分层；Neovim 的后端精确版本依赖；后端的生命周期与退避重启（§3、§3.1）。
2. 可观察性：功能不可用的原因、"导出 IDE 报告"、日志级别（§3.2）。
3. 运行 mcpp 的生态规则：项目根为 cwd、经 PATH 或显式路径、清除 `XLINGS_PROJECT_DIR`；写入 mcpp docs/50（§4.0）。
4. 按项目探测：以项目根为键；版本变化从 job 信封得知；`pinned-missing`；按需、并发 ≤ 2；只在受信任的工作区；`mcpp.path` 优先且不比较（§4.2、§4.5）。
5. `features` 首批两项（`target-sets`、`pkg-format-list`）及其与 `kinds`、`commands` 的分工；冻结对照表；`self env` 新增 `program`（§4.3）。
6. IDE 与 mcppls 的 mcpp 不一致时报告（§4.6）。
7. 保留安装硬依赖；最低 mcppls 0.0.18，0.0.18–0.0.19 用冻结能力表（§5.1、§5.3）。
8. `mcppls status`：字段、`instances[]`、`actions[]`、事件驱动刷新与 200 ms 预算、`--root` 的规范化（§5.2）。
9. 入口对齐表；状态栏不新增项；CodeLens 有开关；`placements` 与 0.9.0 条目逐字保留（§6）。
10. `main` 的位置：语言服务优先，缺席时用后端的文本扫描并标注近似（§6.3）。
11. 测试：首批三个框架；"链接 + 含宏"；`mcpp.test.macros` 追加宏名而非强制框架；用例标识与动态—静态的对应；过滤参数的转义；报告文件的位置（§7）。
12. 规则层下沉、会话进程即 LSP 服务器、协议升为 2、不受信任时可用（§8）。
13. 黄金输出与"一致后删除 TS"（§8）。
14. 无后端回退为声明的降级模式（§8）。
15. CLion 原生化（§9）。
16. 分发 D1–D8 全部在 mcppls 0.0.20（§10）。
17. canary 诊断先行（§11）。
18. 交付：未发布的 PR 追加、新 PR 分检查点；#3 与 mcpp 新 PR 的检查点顺序（§14）。
19. #866、#867 的后续（§17）。

## 16. 自我评审的处理（rev 2）

| # | 视角 | 发现 | 处理 |
|---|---|---|---|
| 1 | 架构 | rev 1 称后端随所有插件打包；Neovim 的后端是独立 xlings 包，依赖不带版本 | `xim:mcpp-ide@<version>` 精确依赖，`initialize` 核对 `protocol`（§3） |
| 2 | 正确性 | xlings 先从 cwd 找 `.xlings.json`、找不到才读 `XLINGS_PROJECT_DIR`；IDE 继承这个变量时，无锁定的项目会用别的项目的锁定 | 探测与 job 清除该变量（§4.2） |
| 3 | 体验 | 锁定版本未安装时 shim 拒绝运行，rev 1 未处理 | `pinned-missing` 状态与安装动作（§4.2） |
| 4 | 性能 | 每个项目一次探测，多项目工作区启动时会并发几十个进程 | 按需、并发 ≤ 2（§4.2） |
| 5 | 安全 | 探测执行项目锁定的程序；列举测试执行项目的程序 | 只在受信任的工作区（§4.2、§7.3） |
| 6 | 简洁 | rev 1 的 `features` 六项中三项没有客户端据以开关 | 只收三项，并写明使用方（§4.3） |
| 7 | 兼容 | 早于 `features` 的 mcpp 会被判定为没有任何能力，2026.10.30.1 的 target 集合会被隐藏 | 冻结对照表（§4.3） |
| 8 | 简洁 | rev 1 的 0.2.0 先行修复用 `self env` 的 `legs` 判定 target 集合，需要额外传 `--target` 运行一次，是绕路 | 0.2.0 比较项目的正确版本，0.3.0 改能力（§4.7） |
| 9 | 正确性 | 两个编辑器同时打开一个工作区时有两个 mcppls 服务器；服务器崩溃后状态文件过期 | `instances[]`，按进程存活过滤（§5.2） |
| 10 | 架构 | rev 1 让 mcpp-ide 的命令表保存 mcppls 的 action id，是跨产品的硬编码 | 动作 id 由 mcppls 在 `status` 中声明（§5.2、§5.3） |
| 11 | 性能 | rev 1 的入口与用例位置经 mcppls 的 `query` 命令行，而 `query` 要启动引擎，不适合打开文件时调用 | 位置取自编辑器中已在服务的语言服务（PSI、文档符号）与后端的文本扫描；mcppls 0.0.21 只新增 `status`（§6.3、§7.3） |
| 12 | 安全与正确性 | 依赖了 gtest 但只写 `main` 的测试程序，收到 `--gtest_list_tests` 会被当场运行 | 动态列举只对静态扫描到框架宏的程序执行（§7.3） |
| 13 | 架构 | rev 1 另起 `mcpp-ide lsp` 进程，与会话进程各有一份项目状态、各探测一次 mcpp | 后端已是 LSP 基础协议上的 JSON-RPC，会话进程本身成为 LSP 服务器（§8） |
| 14 | 跨平台 | rev 1 删除无后端回退；okrun 不覆盖 macOS x86_64、Windows arm64、Linux armhf，这些平台上的 VS Code 会失去全部功能 | 回退保留为声明的降级模式，CI 专门覆盖（§8） |
| 15 | 一致性 | 命令表生成的入口可能改动 0.9.0 已有的条目 | 0.9.0 条目逐字保留，新增另起（§6.2） |
| 16 | 体验 | CLion 原生界面的字符串来源未定 | 共用后端的 `i18n/bundle`（§9） |
| 17 | 范围 | D8 写成也覆盖 IDE 插件更新，而插件更新由 IDE 放到新目录 | 只涉及 xlings 安装（§10） |
| 18 | 交付 | rev 1 的交付表有硬顺序（0.3.0 依赖 mcpp 与 mcppls 的新版本），与"按能力"的原则矛盾 | 冻结表使 0.3.0 能用现有版本，发布无硬顺序（§14） |

## 17. 外部贡献的后续（#866、#867）

两个 PR 按原样合入（安全检查：只改 `version_req`、`toml` 两个模块、它们的单元测试与文档；无工作流、脚本、网络与进程操作；
`Lexer::peek` 越界返回 `'\0'`，`\u` 的读取不越界）。以下在 mcpp 的新 PR 中补上：

| 来源 | 缺口 | 补法 |
|---|---|---|
| #866 | 兼容性说明称"裸版本不变"；而裸版本即 caret（`version_req` 中 `bare = caret`），解析器只在它恰为已发布的键时作精确地址，部分写法 `"0"`、`"0.0"` 同样改变（只匹配 0.0.0 → `<1.0.0`、`<0.1.0`），依赖冲突检查（`graph.cpp`）与地址核对（`address_set`）亦然 | 补裸部分版本的单元测试；docs/05 两种语言写明"裸版本即 caret，部分写法同样按通配"；CHANGELOG |
| #867 | 读取接受 `\b`、`\f`、`\u`、`\U` 后，值中可出现控制字符；`escape_string` 只转义 `"`、`\`、`\n`、`\t`、`\r`，`serialize`（`mcpp publish` 的规范化清单，#690）会写出含未转义控制字符的基本字符串，不是合法的 TOML | U+0000–U+001F 与 U+007F 全部转义（`\b`、`\f` 用短形式，其余 `\uXXXX`）；读入—写出—读入的往返测试；`\0` 是本读取器的扩展而非 TOML 1.0，写入 docs；CHANGELOG |

## 18. 第二轮自我评审的处理（rev 3）

| # | 视角 | 发现 | 处理 |
|---|---|---|---|
| 1 | 一致性 | §4.7 仍写"0.2.0 先修、0.3.0 改能力"，与"未发布的 PR 追加"的决定矛盾；§10 仍写 0.0.21 | §4 全部进入 0.2.0，冻结表覆盖发布前的间隙（§4.7）；§10 改为 0.0.20 |
| 2 | 一致性 | 不再读 `.xlings.json` 后，§4.5 的"设置指向 X，项目锁定 Y"无从得知 Y | `mcpp.path` 生效时只标注"由设置指定"，不比较（§4.2、§4.5） |
| 3 | 架构 | "在项目目录内经 shim 运行"只写在后端一侧，mcppls 另有一份描述，两处可能分叉 | 写成一条生态规则，写入 mcpp docs/50，后端与 mcppls 引用（§4.0） |
| 4 | 性能、简洁 | 每个 job 前另起一次 `--version` | 版本从 job 信封的 `mcpp.version` 得知，变化时才重新探测（§4.2） |
| 5 | 简洁 | 缓存键含"shim 解析出的程序路径"，而这个路径要探测后才知道 | 以项目根为键（§4.2） |
| 6 | 语义 | `features` 与已有的 `kinds`、`commands` 的边界未定；`test-archive` 没有 IDE 使用方，违反"有客户端开关才加名字" | 定义三者分工；首批减为两项（§4.3） |
| 7 | 兼容 | 最低支持 0.0.18，而冻结能力表只写了 0.0.20 | 冻结表覆盖 0.0.18–0.0.19（§5.1、§5.3） |
| 8 | 可观察性 | 后端与 mcppls 各探测 mcpp，两者不一致时无人知晓 | 比较两者，在项目视图报告（§4.6） |
| 9 | 稳定性 | 后端崩溃后的行为未定义；后端持有的 job 随之结束 | 退避重启、上限后降级，job 报告为"后端已退出"（§3.1） |
| 10 | 可观察性 | 功能被隐藏时用户不知原因；没有统一的问题报告 | 每个不可用功能带原因；"导出 IDE 报告"；日志级别（§3.2） |
| 11 | 性能 | `mcppls status` 的刷新频率未定，轮询会反复启动 okrun | 事件驱动，面板可见时 ≥ 30 s 轮询，单次 200 ms 预算（§5.2） |
| 12 | 跨平台 | `--root` 在 Windows（盘符、分隔符）与 macOS、Windows（大小写）上与 mcppls 记录的根可能比较不等 | 双方同一规范化规则，写入 S3（§5.2） |
| 13 | 体验 | 状态栏新增 mcpp 版本项会让 VS Code 状态栏多一项 | 并入已有的 mcpp 项；CLion 一个组件（§6.1） |
| 14 | 体验 | CodeLens 在每个 `main` 上方出现，有人不想要 | `mcpp.codeLens.enable`，默认开（§6.1） |
| 15 | 稳定性 | mcppls 被禁用时 VS Code 没有文档符号，CodeLens 位置无来源 | 退回后端的文本扫描，标注近似（§6.3） |
| 16 | 框架 | 用例的标识与动态—静态的对应未定义；gtest 参数化用例的名字与 `TEST_P` 不同 | 标识四元组；去前缀与后缀后匹配；匹配不到挂到程序节点（§7.3） |
| 17 | 跨平台、正确性 | 过滤参数含框架的元字符（gtest 的 `:`、`*`，Catch2 的 `,`、`[`）时会选错用例 | 按框架语法转义，argv 传递不经 shell（§7.3） |
| 18 | 语义、安全 | `mcpp.test.framework` 强制指定框架，会对不含该框架的程序执行列举，恰好违反 rev 2 的安全约束 | 改为 `mcpp.test.macros` 按框架追加宏名（§7.3） |
| 19 | 体验、安全 | 规则层并入会话进程后，是否在不受信任的工作区工作未写明；0.9.0 的 TS 实现是可用的 | 规则层只分析文本，不受信任时可用；`manifest/check` 仍受信任约束（§8） |
| 20 | 兼容 | `initialize` 改形与方法改名是协议的不兼容变化，rev 2 未给出协议版本 | 协议升为 2；插件与后端版本恒等，不兼容协议 1（§8） |
| 21 | 稳定性、交付 | #3 的范围变大，一次合入风险高 | #3 与 mcpp 新 PR 内部按检查点提交，每个检查点 CI 通过（§14） |
| 22 | 可观察性 | 测试报告文件与 job 的临时文件位置未定，失败后难以排查 | 每次运行独立的临时目录，结束删除；"导出 IDE 报告"包含最近的 job 记录（§7.3、§3.2） |

## 19. 一个文件到处运行（mcpp#849）：probe 数据与方案（待讨论）

### 19.1 probe

mcpplibs/openkal#54（临时，不合入）：cosmocc 4.0.2 构建一个 APE（998 KB，含 x86_64 与 aarch64 两份代码、x86_64 的 Mach-O），
在 Linux x64/arm64、macOS arm64/x64、Windows x64/arm64 上运行，测量 openkal 各区域对应的 libc 原语（52 项）与 APE 本身的行为。

| 项 | Linux x64 | Linux arm64 | macOS arm64 | macOS x64 | Windows x64 | Windows arm64 |
|---|---|---|---|---|---|---|
| 原语（通过/总数） | 52/52 | 50/50 | 48/50 | 51/52 | 52/55 | 52/55 |
| 不通过的原语 | — | — | RWX 映射被拒（W^X；RW→RX 可行） | — | 长路径 > 260 失败；以执行权限映射文件被拒 | 同左 |
| 运行的代码 | x86_64 | aarch64 | aarch64 | x86_64 | x86_64 | x86_64（经系统模拟） |
| 不经 shell 直接 execve | **ENOEXEC** | **ENOEXEC** | **ENOEXEC** | **ENOEXEC** | 可以 | 可以 |
| 经 `sh` 首次运行 | 41 ms，写出 9 KB 的 `$TMPDIR/.ape-1.10` | 87 ms，同左 | **2012 ms**：写出 loader 的 C 源码并用系统的 `cc` 编译 | 102 ms，写出 9 KB 的 Mach-O loader | 7.5 ms，不写盘 | 33 ms，不写盘 |
| 之后的启动（中位数） | 1.1 ms（原生 0.7） | 1.3 ms（0.7） | 6.0 ms（1.4） | 49.5 ms（10.9） | 7.4 ms（8.4） | 29.4 ms（14.2） |
| `assimilate` 转为本机格式后直接 execve | 0.3 ms | 0.4 ms | **不支持**（Apple Silicon 上 APE 只以 ELF 经 loader 运行） | 26.1 ms | — | — |
| 注册了 MZ 的 binfmt（如 wine-binfmt） | 被接管而失败；经 `sh` 可行 | 同左 | — | — | — | — |
| 签名与放行 | — | — | quarantine 不拦截命令行启动；`codesign -s -` 成功 | 同左 | **Authenticode 无法签名**（PowerShell 与 signtool 都拒绝：`0x800700C1`）；Defender 未报 | 同左 |
| 调用约定 | — | — | — | — | cosmo 代码在 Windows 上也是 SysV：调用 SysV 的被调者无需 thunk，`ms_abi` 显式调用同样可行 | 同左 |

（`fcntl` 记录锁在 macOS、Windows 上的失败是 probe 自身对同一文件先加了 `flock` 所致，不计入。）

### 19.2 读数

1. **APE 在 POSIX 上不是可执行文件，而是一个 shell 脚本。** 编辑器、Python、IDE 后端用 execve 启动它会得到 ENOEXEC；只有经 shell
   （或安装 `ape` loader、注册 binfmt、`assimilate` 转换）才能运行。作为被其他程序启动的服务器（mcppls、mcpp-ide 后端），APE 不可直接用。
2. **"什么都不用装"在 Apple Silicon 上不成立。** 首次运行要用系统的 `cc` 编译 loader（2 s），没有 Xcode 命令行工具的 Mac 无法运行；
   `assimilate` 在这里也不支持。
3. **首次运行写盘。** 每个 POSIX 系统写出一个 loader 到 `$TMPDIR`，与 SPEC-014 单文件"运行时不写盘"的条款不一致。
4. **Windows 上 APE 无法 Authenticode 签名**，发布可信的 Windows 程序时不可用；未签名程序的 SmartScreen 与杀毒问题（part 2 D4）无解。
5. **libc 原语的覆盖很好**（每个平台 48–52 项）；Windows 上的缺口是 MAX_PATH 与可执行的文件映射。#849 原文"Cosmopolitan 在 Windows 上以 MS ABI
   运行、需要 thunk"不成立：cosmo 的代码在 Windows 上也用 SysV。
6. 启动开销：Linux 与 Windows 上接近原生；macOS x64 约为原生的 4 倍。

### 19.3 Cosmopolitan 与 openkal

| 维度 | Cosmopolitan（APE） | openkal |
|---|---|---|
| 可移植的单位 | 一个程序，各系统的适配编译进程序 | 与系统无关的映像（ok image），各系统一个 runner |
| 接口 | POSIX libc 的模拟：面宽，现有代码可直接编译 | 封闭的 `kal_*` 规范：面窄、可验证（一致性测试）、能力式（目录授权） |
| 系统适配的位置 | 每个程序各带一份；修一个系统问题要重新构建程序 | runner 与实现中，一次实现，所有映像共用 |
| 多架构 | 一个文件两份代码 | okb（两份映像）；单文件按行 |
| 直接运行 | Windows 直接；POSIX 需 shell 或 loader | 需 runner，或单文件载体（原生程序，内存加载，不写盘） |
| 签名 | Windows 不能；macOS ad-hoc 可以 | 载体与 runner 是原生程序，各系统正常签名 |
| 主机库（GPU） | `cosmo_dlopen` 加 `ms_abi` 函数指针 | 规范中没有 |
| 沙箱 | `pledge`、`unveil` 自我约束（加载 GPU 库时关闭） | runner 授权的目录（`kal_fs_preopen`），由构造保证 |
| 系统 | Linux、macOS、Windows、三种 BSD | Linux、macOS、Windows |
| C++ | cosmocc | mcpp 的 LLVM 23、libc++ 与 `import std` |

### 19.4 同类项目

| 项目 | 做法 | 一个文件 | 所有系统 | 所有架构 | 需要安装运行时 | 首次写盘 |
|---|---|---|---|---|---|---|
| llamafile（Mozilla） | APE；llama.cpp 构建两次；权重以页对齐的 ZIP 嵌入并 mmap；GPU 用预构建的 DSO（`ggml-cuda.dll` 717 MB）经 `cosmo_dlopen` 加载 | 是（Windows 上限 4 GB） | 6 种（Windows 只有 AMD64） | x86_64、aarch64 | 否 | loader（POSIX） |
| redbean、Actually Portable Python/Lua | APE | 是 | 6 种 | 两种 | 否 | loader |
| WebAssembly + WASI（wasmtime、wasmer） | 字节码 + 运行时；`create-exe` 按系统产出 | 否（需运行时） | 是 | 是 | 是 | — |
| JVM JAR | 字节码 + JVM | 是 | 是 | 是 | 是 | — |
| Apple universal、Windows ARM64X | 一个系统内多架构 | 是 | 否 | 是 | 否 | — |
| PyInstaller、Nuitka onefile、.NET single-file、AppImage | 每系统一个，自解压 | 每系统一个 | 否 | 否 | 否 | 是（临时目录） |
| openkal 今天 | okb + okrun；单文件按行 | okb 是 / 单文件按行 | runner 所在的系统 | 是 | okb 需 okrun | 单文件不写盘 |

openkal 的位置：映像与 runner 的分工接近 WASM 与 JAR（可移植的单位加宿主），但映像是原生代码、无 JIT；单文件按行与
PyInstaller 一类相当，且不写盘。

### 19.5 openkal 能直接做的部分

| llamafile 的能力 | openkal 体系 |
|---|---|
| 一个文件同时含两个架构 | 已有：okb |
| 一个文件在所有系统运行 | 可做，不需要 Cosmopolitan：方案 B（下） |
| 嵌入大数据并 mmap（权重） | 需要 openkal 规范增加文件映射（与 mcppls-clangd#12 共用的前置项） |
| 运行时按 CPU 特性分派 | 编译器的 `target` 属性与 `cpuid`；ok 映像中可用，待验证 |
| 沙箱 | 已有，且更强（能力式） |
| GPU 与主机库 | 没有；需要规范增加"主机模块"（固定函数表、runner 加载原生 DSO 并转换调用约定），是一个独立的设计题 |

### 19.6 两条实现路线

- **A：openkal-cosmo。** 用 Cosmopolitan 的 libc 实现全部 `kal_*`，okrun 构建为一个 APE。一份 runner 覆盖 6 种系统（含 BSD），
  体积最小；代价：把规范重新实现一遍（Windows 上是 POSIX 模拟之上再实现，转换两次），并继承 19.2 的 1–4。
- **B：openkal 原生多格式载体。** 只借用 APE 的文件格式技巧，不用它的 libc：文件开头是原生的 Windows okrun（其 DOS 头里放 shell
  前导），其后是各系统的原生 okrun 与 okb。Windows 直接运行；POSIX 上前导把对应的原生 okrun 解到按摘要命名的缓存目录（SPEC-014
  启动器已有缓存目录的规则）并执行。复用经一致性测试的原生实现；macOS arm64 解出的是预构建、已签名的 Mach-O，不需要 `cc`；
  没有 cosmo 在 Windows 上的 MAX_PATH 与可执行映射的缺口。代价：体积为各系统 runner 之和；POSIX 上同样首次写盘、同样不能被直接
  execve；Windows 部分能否签名取决于 PE 的布局（APE 的 PE 布局被 signtool 拒绝，标准链接器产出的 PE 只替换 DOS 存根可能可以），
  需要第二个 probe。

两条路线都不适合作为被其他程序启动的服务器（19.2 第 1 点）：mcppls、mcpp-ide 后端与 IDE 载荷仍使用原生 runner 与按系统的包，
"一个文件"定位为命令行工具的下载形式。

### 19.6.1 路线 B 的 probe

mcpplibs/openkal#54 的第二个工作流（`probe-polyglot`）：mcpp 2026.10.30.1 在一台 Linux 上为 `--target openkal` 打出四个系统的单文件
（Linux x64 1.76 MB、Linux arm64 1.92 MB、macOS arm64 2.51 MB、Windows x64 1.90 MB；x86_64-macos 与 aarch64-windows-gnu 不构建，写为未覆盖），
`probe/polyglot/make_polyglot.py` 把 Windows 单文件的 PE 头后移、在 DOS 存根处写入 POSIX 前导（节表、调试目录与
`SizeOfHeaders` 随之修正），其余三个单文件附在 PE 之后，得到 8.08 MB 的一个文件（`file`：PE32+）。

| 项 | Linux x64 | Linux arm64 | macOS arm64 | macOS x64 | Windows x64 | Windows arm64 |
|---|---|---|---|---|---|---|
| 运行 | 是 | 是 | 是 | 文件中无该系统的载体：退出 126 并写明 | 是 | 是（x64 模拟） |
| 不经 shell 直接 execve | ENOEXEC | ENOEXEC | ENOEXEC | — | 可以 | 可以 |
| 首次运行（经 `sh`） | 18 ms，解出 1.76 MB 到 `~/.cache/openkal-polyglot/<摘要>/` | 14 ms | 120–280 ms | — | 6 ms，不写盘 | 46 ms，不写盘 |
| 之后的启动（中位数；原生单文件） | 9.4 ms（6.3） | 6.6 ms（4.0） | 14 ms（7.4） | — | 5.6 ms（5.6） | 36 ms（36） |
| Authenticode | — | — | — | — | **signtool 签名成功**（自签根证书不受信是唯一的验证错误）；签名后运行；Defender 无告警 | 同左 |
| 签名后的文件在 POSIX 上 | 运行 | 运行 | 运行 | — | — | — |
| macOS 放行 | — | — | 带 quarantine 的文件经 `sh` 运行；解出的 Mach-O 保留原有签名，不继承 quarantine | — | — | — |

与 APE 相比：Windows 上可以签名且启动等同原生；macOS arm64 不需要编译器；POSIX 上首次写盘与"不能被直接 execve"两点相同；
体积为各系统载体之和（8.08 MB 对 APE 的约 1 MB 加映像）。macOS x64 与 Windows arm64 原生的缺口来自 mcpp 不构建这两行，而不是格式。

### 19.6.2 决定（2026-10-30）

- 采用路线 B，作为载体模型之一：平台的 `carrier` 列表可声明多个模型，`row`（每个 `<os>-<arch>` 一个原生单文件，不写盘，
  能被直接 execve）为默认；`polyglot`（路线 B）可选，以容器名选择：`--pkg-format polyglot`（或 `[pack] format`），与
  `single-file` 并列，`--systems` 同样适用，不新增选项；`ape`（路线 A）保留为以后可加的模型。不认识的模型跳过并警告
  （SPEC-014 已有的规则）。
- SPEC-014 为 `polyglot` 增加一条：POSIX 上首次运行把本系统的载体写入 SPEC-014 §5.2 已有的单文件缓存目录
  （`<缓存根>/mcpp/single-file/<载荷的 sha256>/`，权限与校验规则相同），之后复用；`row` 的"运行时不写盘"不变。probe 中的
  `~/.cache/openkal-polyglot/` 只是 probe 的路径。
- 与其余选项：`polyglot` 与 `single-file` 一样不进 `--pkg-format` 列表（SPEC-014 §5.4）；`mcpp run --pkg-format polyglot` 在 POSIX
  上经 `sh` 启动它（ENOEXEC，§19.6.1）；`mcpp emit xpkg` 不接受 `polyglot`，xlings 的条目按系统使用 `row` 的单文件或树，
  `polyglot` 是直接下载的形式。
- 签名顺序：各系统的载体先按 SPEC-014 的签名钩子各自签名（macOS 的 codesign 等），再组装；组装后的整个文件最后做 Windows 的
  Authenticode（它把证书附在文件末尾，前面的字节与偏移不变，§19.6.1 实测签名后 POSIX 仍可运行）。
- 覆盖：`polyglot` 的系统与架构即各行单文件的并集（今天四对：Linux x64/arm64、macOS arm64、Windows x64），`runs_on` 照此写出；
  缺少的行在文件中如实报告（退出 126 并写明）。一台 Linux 即可构建全部四个载体（§20 D7）。
- openkal 规范增加文件映射（同时解除 mcppls-clangd#12）。

### 19.8 设备与 ok 生态的路线（已定，2026-10-30）

- **归属**：GPU、显示、音频、摄像头等按"我的 / 哪一个"的判别归 openhal；openkal 不增加设备能力。
- **事实**：ok 映像的导入只能是规范声明的名字与其他映像导出的名字，C 库与环境的名字一律拒绝（openkal SPEC §12.2）；映像 → okso
  （加载器查找与 C 库的 `dlopen`）与原生程序 → okso（`okrun_open_library` 与 openkal-bind）已有；映像 → 主机原生库没有。
- **路线**：R0 原生目标经 openhal 的 concept 与 `openhal-host` 直接使用主机库（今天可用）；R1 ok 映像经 openbus（`kal_net_connect_at`
  连本机路由、共享区域）使用设备，openkal 只补文件映射；R2 进程内快速路径为 openkal §12 的"加载器提供的具名扩展接口"：由 C 头文件
  定义、带版本，主机模块实现，加载器按头文件机械生成调用约定的转换（openkal-bind 的反向模式，与它现有的边界规则相同），映像的 note
  声明所需的扩展接口，缺少即在加载时拒绝；R2 在 R1 跑通并实测"ok 映像经 okrun 连 xbus"的开销后进入规范。不做：openkal 内的领域
  扩展（`kal_gpu_*`）、映像任意加载主机库。
- **归档**：R1、R2 与文件映射的设计写入 openkal 的仓库与规范；设备接口（GPU 等）写入 openhal 的规范；本设计只保留分工与上述要求。

### 19.7 待讨论

1–4 已定（19.6.2）。
5. 设备（GPU 等）：按"我的 / 哪一个"的判别归 openhal；ok 映像默认经 openbus 访问（`kal_net_connect_at` 连本机路由，大数据经
   共享区域，依赖文件映射）；进程内主机模块作为 openhal 的第三种绑定是否现在定义；GPU 计算接口的设计归 luban/specs/openhal。
   待确认。xbus 的实测（原生进程、经路由、本机 release）：调用往返 p50 32.8 µs、p99 65.5 µs，8 MB 区域按引用往返 p50 65.5 µs
   （§20 D5）。批量提交命令缓冲的用法在这个量级内；每个小操作一次调用的用法（CUDA 式细粒度启动，进程内约为个位数 µs）需要快速路径。

## 20. 补测数据（rev 4）

| # | 假设 | 测量 | 结果 |
|---|---|---|---|
| D1 | 按项目探测经 shim 的代价与锁定的生效方式（§4.2） | 干净环境中装 2026.10.29.1 与 2026.10.30.1，项目目录以 `.xlings.json` 锁定 2026.10.29.1 | 锁定的目录内 shim 选中 2026.10.29.1；**未锁定的目录在 `XLINGS_PROJECT_DIR` 指向锁定项目时也选中 2026.10.29.1**（证实 §4.2 清除该变量的必要）；经 shim 每次调用 25–29 ms，直接调用 5.8 ms |
| D2 | 信封带版本（§4.2） | `self env --format json` 的顶层 | `"mcpp": {"version": "2026.10.29.1", "protocol": {"min": 1, "max": 1}}`，每个信封都有 |
| D3 | `mcppls status` 的预算（§5.2） | okrun 启动 mcppls 0.0.19 的 15 MB okb，执行 `version` 等三条命令各 15 次 | 中位数 35 ms（Linux x64） |
| D4 | mcppls 调用 mcpp 的方式（§4.6） | 读 mcppls `src/project/mcpp.cpp` | 经 PATH 找到 `mcpp`、工作目录为项目根：符合；环境为捕获的登录 shell 环境，未清除 `XLINGS_PROJECT_DIR`：需补 |
| D5 | 设备经 openbus 的延迟（§19.7） | xbus 设计 §9 的 B4 实测 | 调用往返 p50 32.8 µs、p99 65.5 µs；stream 1.8 GB/s；8 MB 区域按引用往返 p50 65.5 µs |
| D6 | 测试框架的参数（§7.2） | mcpp-index 的版本 | gtest 1.18.0、Catch2 3.15.2、doctest 2.4.12、boost-ext.ut 2.3.1 |
| D7 | `polyglot` 的载体能否在一台机器上构建（§19.6.2） | 干净环境中 `mcpp pack --target openkal --pkg-format single-file --systems all` | 一台 Linux 写出 Linux x64/arm64、macOS arm64、Windows x64 四个单文件；macOS x64 与 Windows arm64 写为未覆盖 |

未测：ok 映像经 okrun 连 xbus 的额外开销；CLion 运行组件的锤子按钮对 mcpp 运行配置的行为（§6.1）；`boost-ext.ut` 的命令行过滤。

## 21. 第三轮全局评审的处理（rev 4）

| # | 视角 | 发现 | 处理 |
|---|---|---|---|
| 1 | 简洁、性能 | 探测要两次调用（`self env` 与 `--protocol-version`），经 shim 共约 55 ms；能力只在 `--protocol-version` 中 | `features` 写入信封的 `mcpp` 字段，一次 `self env` 即可；job 的信封同样携带（§4.3） |
| 2 | 一致性 | 运行 mcpp 的规则（§4.0）只落实在后端一侧，mcppls 未清除 `XLINGS_PROJECT_DIR`（D4） | mcppls 补上并以夹具固定（§4.6） |
| 3 | 一致性 | probe 的 `polyglot` 缓存在 `~/.cache/openkal-polyglot/`，与 SPEC-014 单文件已有的缓存目录与规则不同 | 并入已有的 `mcpp/single-file/<sha256>/` 与其规则（§19.6.2） |
| 4 | 安全、发布 | `polyglot` 的签名顺序未定：Authenticode 附在文件末尾，而各系统载体的签名在组装前 | 先签各载体、再组装、最后签整个文件（§19.6.2） |
| 5 | 可观察性 | `polyglot` 在缺少本系统载体时的行为只在 probe 中出现 | `runs_on` 为各行并集；缺少的行退出 126 并写明（§19.6.2） |
| 6 | 体验 | `--carrier polyglot` 是新增的选项名；单文件已有 `--pkg-format single-file` | 已定：`--pkg-format polyglot`，不新增选项（§19.6.2） |
| 7 | 性能 | `mcppls status` 的 200 ms 预算没有依据 | 实测 35 ms（D3），预算保留为跨平台上限（§5.2） |
| 8 | 架构 | 设备的默认路径（openbus）没有延迟依据 | xbus 实测写入，并给出默认路径与快速路径的界线（§19.7） |
| 9 | 范围 | 文件映射的级联（openkal 规范与三个实现）不被 mcpp、mcppls、mcpp-ide 本轮的任何一项依赖 | 作为并行的独立级联，不阻塞其余交付（§14） |
| 10 | 兼容 | `polyglot` 的载体覆盖受 mcpp 构建的行限制（macOS x64、Windows arm64 没有） | 与 `row` 相同，如实写为未覆盖；补行属于平台与目标表，不在本设计 |
| 11 | 范围 | §6.1 的 CLion 锤子按钮与 `boost-ext.ut` 的过滤未实测 | 实现时先测；不满足则锤子按钮改为工具栏的 mcpp build 动作，ut 只提供程序级 |
| 12 | 可读性 | 文档已近 650 行，已定与待定分散在各节 | 新增 §22 汇总待定项 |

## 22. 待定项汇总

0. 本轮的规范清单与 openlifa 一轮：已定（§23）。
1. 设备：已定（§19.8）。
2. `polyglot` 的选择方式：已定，`--pkg-format polyglot`（§19.6.2）。
3. `mcpp.path` 与项目锁定冲突时执行按设置（§4.2）：已定。
4. 交付：未发布的 #3、#75 追加本设计的内容，推迟 mcpp-ide 0.2.0 与 mcppls 0.0.20 的发布（§14）：已定。
5. 文件映射的最小接口（§23.2）与 `polyglot` 的三条边界（§19.6.2）：已定。

## 23. 本轮的规范清单与 openlifa 一轮（已定，2026-10-30）

### 23.1 本轮必须写出的规范

| part 2 的条目 | 规范 | 仓库 |
|---|---|---|
| §4.0 工具怎样运行 mcpp；§4.3 `features`（含信封的 `mcpp` 字段）、`self env` 的 `program` | docs/50（机器输出） | mcpp |
| §19.6.2 `--pkg-format polyglot`、缓存、签名顺序、与列表及 `run`、`emit xpkg` 的关系 | SPEC-014 新增条款 | mcpp |
| §5.2 `mcppls status`（实例、动作、能力，`--root` 的规范化） | S3 新增一节 | mcppls |
| §6–§9 入口、用例、规则层、CLion 原生化 | 协议 2（`protocol/protocol.schema.json`）、`spec/test-frameworks.json`、`spec/commands.json` 的 `placements` | mcpp-ide |
| §19.8 文件映射 | openkal SPEC 新增可选接口 `openkal.map`（23.2） | openkal |

openhal、openbus、opencaps、openas、openmui 不在本轮的依赖之内；xlings 不改。

### 23.2 文件映射的最小接口

openkal 新增一个可选接口 `openkal.map`（资源：一个文件的一段在地址空间中的映像；次版本，只增不改）。不并入 `openkal.space`：
openkal 的接口整体提供或整体不提供（§3），`openkal.space` 的资源是地址空间与在其中执行的上下文，一个能复制地址空间而没有文件的
环境若因这一项而不能提供 `openkal.space`，是把两个资源绑在了一起。`openkal.map` 以 `openkal.fs` 的已打开文件为输入。

| 操作 | 语义 |
|---|---|
| `kal_map_file(file, offset, length, access, sharing, out)` | 把一个已打开文件的一段映射进地址空间；`offset` 是 `KAL_MAP_ALIGNMENT`（65536，各环境分配粒度的最大者，Windows 为 64 KiB）的整数倍，否则以 `kal_err_invalid` 拒绝；`access` 为只读或读写；`sharing` 为私有（写入不回到文件）或共享（写入回到文件，其他映射与读取可见） |
| `kal_map_release(region)` | 解除映射 |
| `kal_map_sync(region)` | 把共享映射的写入写回文件 |

- 不提供可执行的文件映射：执行代码只经 `kal_exec_*`（W^X；§19.1 的 probe 中 Windows 拒绝可执行的文件映射）。
- 共享区域（openbus 的 region、进程间的大数据）由"授权目录下的文件 + 共享映射"构成，不另设操作。
- 映射之后文件被截短时，**共享**区域有三种结果，取哪一种由环境决定：截短被拒绝（Windows）；访问新末尾之后的整页是一次故障（Linux 的 SIGBUS）；这样的页读作零。任何一种都不读到被截去的字节，末页中新末尾之后的字节为零。**私有**区域与文件的任何改动相同，可能继续显示它显示过的内容（实测 macOS 的私有区域读到被截去的字节）。原先"不以零页代替"的要求被实测否定：要求一种结果，实现就得维护被映射文件的登记表以拒绝截短，即条款 7.1 所排除的兼容层。
- 一致性测试覆盖：只读与读写、私有与共享、偏移对齐的拒绝、两个映射之间的可见性、同步之后另一进程读到写入、截短之后不读到被截去的字节（子进程中进行）。
- 级联与依赖见 §25。

### 23.3 openlifa 一轮（不在本轮，记录于 openlifa/openlifa#1）

- 合入 openas 0.2、openmui 0.3 的 PR #1，并在 openlifa/openlifa 写出准入与维护准则：文本、版本、ABI、一致性、依赖、实现、平台、
  许可证、流程、分发。
- mcpp 的 SPEC-001 修订："一个包、两个名字"。新描述文件以 `formerly` 声明旧身份，两个名字解析到同一规范身份，锁文件写规范身份并
  两者都读，旧名永久有效；旧描述文件不变，老版本 mcpp 不受影响；C++ 的 `import` 名不变。
- 规则发布后，把 `mcpplibs/openkal`（规范、`SURFACE.txt`、头文件、一致性测试）转移到 openlifa：GitHub 重定向旧地址与 tag 归档，sha256
  不变，`mcpplibs/openkal` 之名此后不再使用；实现、openkal-image、openkal-platform 留在 mcpplibs。
- openbus、opencaps 待有第二个实现；openhal 先把两个定义统一为一份规范；opensal 以后；openwx 留在产品中；xlings 本轮不改。

## 24. 第四轮多视角评审的处理（rev 6）

| # | 视角 | 发现 | 处理 |
|---|---|---|---|
| 1 | 依赖 | 文件映射已定为本轮并行的级联，但没有最小接口，openkal、mcppls-clangd#12、#849 的数据嵌入都无从对齐 | 23.2 定下最小面与语义 |
| 2 | 跨平台 | Windows 的映射偏移必须是 64 KiB 的整数倍，其他系统是页大小 | 粒度取各环境最大者（23.2） |
| 3 | 安全 | 文件映射若允许执行，就绕过了 `kal_exec_*` 的 W^X | 不提供可执行的文件映射（23.2） |
| 4 | 一致性 | `polyglot` 与 `--pkg-format` 列表的关系未写；`single-file` 不进列表 | `polyglot` 同样不进列表（§19.6.2） |
| 5 | 体验 | `mcpp run --pkg-format polyglot` 在 POSIX 上直接执行会得到 ENOEXEC | 经 `sh` 启动（§19.6.2） |
| 6 | 分发 | `polyglot` 是否进 xlings 条目未定；xlings 的 shim 直接 execve 会遇到 ENOEXEC | `emit xpkg` 不接受 `polyglot`，xlings 用 `row` 或树（§19.6.2） |
| 7 | 兼容 | 信封的 `mcpp` 字段新增 `features` 是否破坏已有客户端 | docs/50 的信封只增不改，客户端忽略不认识的键；新增是兼容的 |
| 8 | 范围 | 双名字的机制若放进本轮，会让 mcpp 的 PR 多出一个与 part 2 无关的 SPEC 修订 | 放在 openlifa 一轮（23.3） |
| 9 | 交付风险 | #3 合入了 part 2 的全部 mcpp-ide 内容，体量大 | 维持检查点顺序（§14），每个检查点 CI 通过；0.9.0 的一致性门禁全程保留 |
| 10 | 可读性 | 文档按评审轮次累积，已定的条款分散 | §22、§23 汇总已定与待定；实现时以 §23.1 为规范清单 |


## 25. 实施计划与任务依赖（rev 8）

### 25.1 各视角对计划的约束

| 视角 | 约束 |
|---|---|
| 架构 | 每个修复落在它所属的层；消费者（clangd、mcppls）不绕过下层的缺陷 |
| 稳定性 | openkal 家族以精确钉住逐层推进：一层 CI 通过并发布后，下一层才钉住它 |
| 简洁 | `openkal.map` 三个操作、一个常量；okrun 改为映射读入后删除整文件读入的路径 |
| 兼容与无感升级 | openkal 次版本只增不改；openkal-musl 以弱引用使用 `kal_map_*`，实现未提供时文件 `mmap` 仍为 `ENOSYS`，LLVM 照旧回退为读取，映像不因此要求新的 okrun |
| 跨平台 | 对齐取 64 KiB；一致性测试在 Linux、macOS、Windows 上运行 |
| 用户体验 | mcppls 的包形状不变，`clangd.okb` 只替换引擎（part 1 §9.4）；IDE 插件与 mcppls 的契约不因引擎形式而变 |
| 测试覆盖 | 规范的一致性测试；musl 的 `mmap` 测试；okrun 的大映像常驻内存测试；clangd 四列 × 四系统 |

### 25.2 任务与依赖

| 任务 | 仓库（PR） | 内容 | 依赖 |
|---|---|---|---|
| M | mcpp（#869，2026.10.31.1） | 检查点 ① `features` 与 `program`；② mcpp#868；③ §17；④ §11 与 docs/50 的 §4.0；⑤ `polyglot`；⑥ C1（§25.3） | — |
| M2 | mcpp（2026.10.31.2） | `mcpp test <name> --print-launch`（`test-launch`，§7.4）；构建契约的目标侧（C2）；Windows 启动器的平台（K5 的发现）；本文 rev 8、rev 9 | M |
| M3 | mcpp（2026.10.31.3） | 有封装步骤的平台上，链接边的输出由一个函数给出，排序检查不再把封装边当作链接（C 的发现）；大型工作区的构建空间与复用（mcpp#874，[设计](2026-10-31-workspace-space-and-reuse-design.md)）；本文 rev 10 | M2 |
| K1 | openkal（0.18.0） | `openkal.map`：SPEC 条款、头文件与模块、`SURFACE.txt`、一致性测试 | — |
| K2 | openkal-linux、openkal-macos、openkal-windows | 实现 `openkal.map`；§25.3 定位到实现的修复 | K1 |
| K3 | openkal-image | okrun 以 `openkal.map` 读入映像与 okb（§25.3 B1） | K2 |
| K4 | openkal-musl | 文件 `mmap`、`munmap`、`msync` 经 `openkal.map`；§25.3 定位到 musl 的修复 | K1、K3（musl 钉住 openkal-ld） |
| K5 | openkal-llvm-runtime、openkal-platform、mcpplibs.os | 钉住新版本；platform 的版本组 | K4 |
| C | mcppls-clangd（0.0.18） | clangd 的 `x86_64-openkal`、`aarch64-openkal` 映像与 `clangd.okb`（§25.3） | K5、M3；补丁与隔离实验不依赖，先行 |
| L | mcppls #75（0.0.20） | §5.2 `status`；§4.6；§10 D3–D8；引擎改为 `clangd.okb`（§25.3 的验收） | C |
| I | mcpp-ide #3（0.2.0） | §14 的检查点 ①–⑥；mcppls 0.0.20 单包依赖 | L 的发布（CLion 的 mcppls 依赖）；开发不依赖 |
| X | mcpp-index、xim-pkgindex | openkal 家族的条目随级联逐个进入 mcpp-index（下游 CI 从已发布的索引解析，故不能合为一个 PR）；xim-pkgindex 一个 PR：okrun、mcppls、mcppls-nvim、mcpp-ide、mcpp-ide-nvim | 各自的发布 |

关键路径：K1 → K2 → K3 → K4 → K5 → C → L → I。M 与关键路径无依赖，最先发布，使 L 与 I 发布时即可用上 `features`。

### 25.3 clangd 的 ok 映像：阻塞项按层拆分

依据：mcppls-clangd#11 的 probe（run 40908632199）与本轮对 okrun、openkal-musl、openkal-linux 源码的核对。

| # | 阻塞项 | 事实 | 所属 | 做法 | 依赖 |
|---|---|---|---|---|---|
| B1 | 文件 mmap 与常驻内存（多 90–125 MiB） | (a) LLVM 对文件的 `mmap` 得到 `ENOSYS`，回退为读入；(b) okrun 的 `read_all` 以倍增读入整个文件（71.5 MB 的映像占 128 MiB 缓冲），映像运行期间不释放；(c) 映像的段复制进 `kal_exec` 区域 | (a) openkal 规范 → 三个实现 → openkal-musl；(b) openkal-image；(c) 不改，代码只经 `kal_exec_*` | (a) `openkal.map`，musl 的文件 `mmap` 经它；(b) okrun 映射映像文件，okb 中未压缩的成员直接在映射中装入，压缩成员解压到恰好大小的缓冲，装入后解除映射或释放；(c) 计入验收的基线 | K1 → K2 → K3、K4 |
| B2 | Windows 路径 | probe 第 8 条：路径风格在编译时决定，`C:\`、`C:/` 被当作相对名，资源目录找不到；第 12 条：默认三元组在编译时决定；第 9 条（隔离结果，mcppls-clangd run 41040892276）：`open`、`fstat`、`read`、`lseek`、`MemoryBuffer::getFile` 对 `D:/…`、`D:\…`、相对名全部成功，`EINVAL` 不在 LLVM 之下，而是 `is_absolute` 为假使 `make_absolute` 把当前目录接在 `D:/…` 之前 | LLVM 补丁（mcppls-clangd） | 补丁：`is_style_posix`、`is_style_windows` 在 `__OPENKAL__` 下于运行时由 `kal_env_system` 决定（`Style::native`）；clangd 的 `absoluteParent` 与大小写折叠同样按系统；默认三元组在运行时决定 | — |
| B3 | Linux 的 SIGSEGV | 两个架构都在解析 libstdc++ 头文件时崩溃；同一份 aarch64 映像在 macOS（SDK 的 libc++ 头文件）通过。在 openkal-platform 0.18.0（openkal-musl 0.22.9，`pread` 与 `__cxa_thread_atexit`）上重建的映像不再崩溃（gdb 下运行至结束），余下的是 7 个错误：`stddef.h` 找不到。映像的自身名（`kal_env_image`）是 okrun 收到的相对名，资源目录随之为相对路径，clangd 按编译命令的目录解析它 | LLVM 补丁（mcppls-clangd） | `getMainExecutable` 在 `__OPENKAL__` 下把映像名以启动时的当前目录变为绝对路径（openkal 规定该名按程序得到的外部名使用）；回归：Linux 上 `--check` 解析 libstdc++ 的标准库用例为 0 个错误 | — |
| B4 | 模块 worker | probe 第 11 条：worker 直接运行 `.oke` 而崩溃；`getrlimit`、`setrlimit` 不可用 | LLVM 补丁（series 0017） | worker 经 `kal_env_loader` 以"加载器 + 映像"启动；openkal 没有地址空间上限，worker 的内存上限在 ok 映像上由监督方按常驻内存执行 | — |
| C1 | 链接输入 | probe 第 13 条：`[build] ldflags` 中的预构建归档不是链接输入，改动它不重新链接 | mcpp | 链接参数中存在的文件记为链接步骤的隐式输入 | — |
| C2 | 构建途径 | probe 第 14 条：约定从编译命令读出，工具链文件由 Python 生成 | mcppls-clangd | 约定取自 `${mcpp.contract}`；生成器是一个小的 mcpp 项目 | — |

mcpplibs.os 不直接涉及：clangd 映像只用 openkal-musl 与 libc++；os 因精确钉住随 K5 升版本，mcppls 随之升级 os。

**验收**（mcppls-clangd#12 的完成条件，量化）：四个系统（Linux x64、Linux arm64、macOS arm64、Windows x64）× 四列
（`--version`、无头文件、`import m`、系统的 C++ 标准库）全部通过；解析时间不超过原生的 1.3 倍；常驻内存与原生之差不超过映像
装入的段的大小（B1(c) 的固有代价）。mcppls 的包中，满足验收的系统使用 `clangd.okb`，引擎的选择按系统写在包的清单中；
不满足的系统保留原生 clangd 并在 mcppls 的文档中写明差距，不阻塞其余系统。

### 25.4 并行

| 等待 | 同时进行 |
|---|---|
| M 的 CI | K1 的规范与一致性测试；B2、B3 的隔离实验（mcppls-clangd 的 probe 分支，CI） |
| K1、K2 的 CI | M 的后续检查点；L 的 `status` 与 D3–D8 |
| K3–K5 的 CI 与索引发布 | C 的构建途径与 LLVM 补丁；I 的检查点 |
| C、L 的发布（各约 1.5 h） | I 的检查点；GitCode 镜像 |

### 25.5 发布与验证

发布顺序：M（随即 gtc 补 GitCode、重新触发 main CI、xim-pkgindex 的 mcpp 条目）→ openkal 家族按 §25.2 的级联（每层发布后补
GitCode、mcpp-index 条目）→ C → L → I → xim-pkgindex PR。之后在干净的 xlings 环境中验证：裸 `xlings install` 解析到新版本；
mcpp 新建、构建、运行、`--pkg-format polyglot`；mcppls 在四种编辑器入口中可用，`mcppls status` 的输出；mcpp-ide 的按项目版本
感知（锁定版本的项目与未锁定的项目）。

### 25.6 进度（rev 9）

| 任务 | 状态 |
|---|---|
| M | 已合入并发布 2026.10.31.1 |
| K1–K4 | openkal 0.18.0、openkal-linux 0.19.0、openkal-macos 0.16.0、openkal-windows 0.14.0、openkal-image 0.1.5、openkal-musl 0.23.0 已发布；一致性套件的副本改在套件自身目录运行（wine 经 binfmt 以全路径启动时，副本按卷根运行，无法创建文件） |
| K5 | openkal-llvm-runtime 0.16.10、openkal-platform 0.19.0 已发布，mcpplibs.os 0.2.4 随索引发布进行。platform 0.19.0 的 CI 暴露 mcpp 2026.10.31.1 的缺陷：包的 Windows 启动器钉 `mcpplibs.os` 0.2.1（openkal 0.17.1）却建在项目所选的平台上，版本组指名 openkal 0.18.0 时图中有两个 openkal；M2 改为在 mcpp 的 openkal 行所钉的平台上构建启动器，platform 的 CI 以 M2 的分支在四个系统上通过 |
| C | B2、B3 定位到 LLVM 补丁（上表），补丁在 probe 分支验证 |
| L | `status` 的动作表扩充为 mcppls 的全部动作（S3-8-7） |
| M2 | 版本定为 2026.10.31.2：`test-launch`、构建契约的目标侧（C2 的前提）、Windows 启动器的平台 |
| I | 检查点 ①–⑤ 已推送：⑤ 为规则层移入后端（LSP，协议 2）、三个编辑器的接入、F15 的降级模式及其 CI 任务；⑥ CLion 原生界面在进行 |

### 25.7 进度（rev 10）

| 任务 | 状态 |
|---|---|
| M2 | 已合入并发布 2026.10.31.2，GitCode 已补，xim-pkgindex 的 mcpp 条目已合入 |
| K5 | mcpplibs.os 0.2.4 已发布，其 CI 的 ok 映像任务以 openkal-platform 0.19.0 构建；openkal-emscripten 0.7.0 随 openkal 0.18.0 发布（0.6.0 钉 openkal 0.17.1，os 的 emscripten 任务因此失败）；openkal-image 的 release-binaries 接受 `platform`，okrun 0.1.5 的二进制以 platform 0.19.0 构建 |
| C | B2、B3、B4 的修复与 Windows 的内存前导名（`getInMemoryPreamblePath` 在 `LLVM_ON_UNIX` 下为 POSIX 路径，Windows 上读 PCH 失败；`__OPENKAL__` 下按系统的路径风格命名）合为一个补丁。只为 ok 映像适配 LLVM 的补丁放在 `openkal/patches/`，有自己的 series 与台账，仅在构建 ok 映像时于主 series 与 overlay 之后应用（`apply.sh --openkal`）；原生引擎不带它。`clangd.okb` 的身份覆盖两个 series。映像任务在 mcpp 2026.10.31.2 上被排序检查拒绝：`prepare` 动作构建 clangd 的库，检查要求每条链接边等待它，却把发布 `bin/clangd.oke` 的封装边当作链接；M3 修复 |
| L | 引擎条目可为 ok 映像：`engines.clangd` 的 `path` 指向 `clangd.okb`、`runner` 指向 okrun，身份的平台为 `openkal`（S4-4-12）；锁文件行 `clangd = "mcppls-clangd-openkal"` 选择它；工作区平台升至 openkal-platform 0.19.0、os 0.2.4 |
| X | xim-pkgindex 的 CI 计划以引用的坐标与精确版本计算受影响的包：精确钉住的版本只在该版本条目或配方其余部分改变时成为依赖边；一次 mcpp 版本更新从 71 个包、10 个任务降为 4 个包、6 个任务 |
| CI | GitHub 于 2026-11-02 停用 macos-14，此前按计划使其任务失败（actions/runner-images#13518）。本轮各仓库的 macOS 任务改用 macos-15；构建 macOS 产物的任务写明 `MACOSX_DEPLOYMENT_TARGET=14.0`，支持下限仍为 macOS 14，由产物的 `minos` 断言。mcpp 的 ci-target-matrix 与 openkal-cross 的 macOS 任务随之改用 build.yml 的产物，不再自行构建 |
| 未覆盖 | openkal-opensbi（钉 openkal 0.16.0）与 openkal-uefi（0.14.1）落后于规范；二者为裸机实现，不在 os 与本轮的依赖中 |

