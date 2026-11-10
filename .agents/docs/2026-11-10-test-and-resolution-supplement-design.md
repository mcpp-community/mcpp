---
subject: ecosystem
status: active
---

# 测试与依赖解析补充方案：mcpp、mcpp test、mtest、mcpp-ide

**日期：** 2026-11-10
**状态：** 第三版，已实施（第二版审查：会话不在 mcpp 中实现，改为可选能力 O9；测试的规范与协议收敛为一份 mcpp test spec，分基础与可选，T18；第 8 节为与跨平台方案合并的全局自审；第 9 节为实施记录）
**上游文档：** [2026-11-10-cross-platform-ecosystem-design.md](2026-11-10-cross-platform-ecosystem-design.md)
**相关：** mcpp#914、mcpp#915、mcpp#917、mcpp#407、mcpp#839；SPEC-017；mtest 测试协议版本 1；mtest#10；mcpp-ide 1.4.0

本文记录跨平台生态实施过程中在测试与依赖解析上出现的发现（F1–F11），给出补充决定（T1–T18），并列出归属、顺序、验收与自审。测量均在 mcpp-language-server 0.0.21 的独立克隆上进行（x86_64 Linux，`x86_64-linux-musl` 经 openkal-platform 0.24.4 的 hosted 形态，全局 BMI 缓存已就绪），使用 PR #916 的构建。

## 0. 分层原则

测试涉及三层，各层只依赖 mcpp test spec（T18）：

| 层 | 角色 | 依赖的规范 | 禁止 |
|---|---|---|---|
| 编辑器插件（mcpp-ide、mcpp-xx） | 呈现测试树、结果、检查点与输出；发起运行与取消 | mcpp test spec（基础能力与它宣告的可选能力） | 直接依赖某一测试框架或其工具；按框架名分支 |
| mcpp（构建系统、测试规范与基础运行器） | 拥有 mcpp test spec（含协议）；实现基础能力 B1–B6：发现、构建、启动描述、列出、选择、批处理运行、记录与存储；汇总框架声明的能力 | — | 实现会话等可选的运行器工具；含有框架名或框架特有分支 |
| 测试框架及其工具（mtest、GoogleTest 适配器、脚本适配器、C ABI） | 实现协议的程序一侧；可附带运行器工具，实现可选能力（如 O9 会话） | mcpp test spec | 要求运行器或插件认识自己 |

由此：

- mcpp 只做规范与协议，以及测试的基础对接能力（B1–B6）；会话等可选能力由规范规定接口与语义，由框架附带的工具实现。
- 插件经 mcpp 得到基础能力与可选能力的宣告（B6），按能力启用视图；会话的入口由框架声明、经 mcpp 报告，插件只认规范的方法。
- 协议的增加（检查计数、失败的源语句等）在规范中作为可选能力规定；mtest 是第一个实现。

## 1. 决定摘要

| 编号 | 决定 | 层 / 归属 | 状态 |
|---|---|---|---|
| T1 | 被合并替换的版本撤回它发出的请求 | mcpp 解析器 | 已实现（da960014，e2e 1066） |
| T2 | 根上的覆盖：依赖条目的 `override = true`，不新增表 | mcpp 清单与解析器（#914 A） | 待实现 |
| T3 | 精确锁定共享依赖的检查；冲突报错给出 `override` 写法 | mcpp（#914 C） | 待实现 |
| T4 | 非叶子包的多版本改名不扩展 | mcpp（#914 B） | 决定 |
| T5 | 测试图与构建图各有图文件，共享目标文件 | mcpp test | 待实现 |
| T6 | 测试前的包构建走 `mcpp build` 的快路径 | mcpp test | 待实现 |
| T7 | 测试快路径与测试计划记录（B5 的依据） | mcpp test | 待实现 |
| T8 | 构建目录锁在测试运行前释放 | mcpp test（#915-1） | 待实现 |
| T9 | 测试进程中的内层 mcpp 得到与外层相同的配置 | mcpp（#915-2） | 待测量后实现 |
| T10 | `Compiling` 行只在实际编译时出现 | mcpp test | 待实现 |
| T11 | 失败的呈现：控制台布局（框架）与汇总、索引（运行器） | mtest 库；mcpp | 已实现（mtest 工作区；83024fa6） |
| T12 | 列举与运行以同一方式启动程序 | mtest 引擎 | 已实现（mtest#10） |
| T13 | mtest 自身的依赖改为兼容范围，平台改为 0.25.0 | mtest | 待实现 |
| T14 | mcpp-ide 只经 mcpp test spec；新开 PR | mcpp-ide | 待实现 |
| T15 | 会话：规范定义为 O9，由框架的工具实现 | 规范：mcpp；实现：mtest 0.6.0 | 待实现 |
| T16 | 每个用例的输出（O8）、检查点与检查计数（O3） | 规范；mcpp 与会话的运行器；mtest 库 | 待实现 |
| T17 | mtest 0.6.0：框架与工具的职责 | mtest | 待实现 |
| T18 | mcpp test spec：测试的规范与协议收敛为一份，基础 B1–B6 + 可选 O1–O9 | mcpp（SPEC-017 v2.0） | 待实现 |

## 2. 发现

### F1 一次无改动的 `mcpp test` 有八成时间在规划

| 阶段 | 用时 |
|---|---|
| 包本身的构建（为 `MCPP_BIN_<NAME>`）：宿主图与目标图各解析一次 | 2.65 s |
| 测试构建的规划：宿主图与目标图再各解析一次 | 2.3 s |
| ninja（无事可做） | 0.81 s |
| 运行 | 0.04 s |
| 合计 | 5.8 s |

同一工程 `mcpp build` 的快路径为 0.06 s。

### F2 测试图与构建图互相使对方的快路径失效

一个配置只有一个输出目录和一个 `build.ninja`。`mcpp test` 写入测试图，`mcpp build` 写入构建图；快路径检查到图的形态不同即放弃（#407）。`mcpp test` 之后的 `mcpp build` 从 0.06 s 变为 2.61 s；测试前的包构建也不走快路径。

### F3 构建目录锁在测试运行期间未释放

`dirlock.cppm` 的设计说明"`mcpp test` 在第一个测试运行前释放"，代码只在 `--print-launch` 分支释放。测试程序在同一工作区调用 `mcpp build` 时等待锁直至超时（#915）。

### F4 测试环境中的内层 mcpp 得到另一个配置指纹

`mcpp test` 给测试进程的 `PATH` 前置 subos 的 `bin`；内层 `mcpp build` 解析出另一个输出目录，从头构建（#915）。改变指纹的具体输入尚未测量。

### F5 `Compiling` 行无条件打印

测试路径在构建前为每个包打印 `Compiling`，阶段 A 的进度模型又在实际工作时打印一次；无改动时仍显示，同一个包有时出现两次。

### F6 合并替换的版本留下了它的请求

mtest 工作区中 `lib` 要求 `mcpplibs.os ^0.4.1`（解析为 0.4.4，openkal 0.25 一族），`engine` 精确锁定 `0.4.1`（openkal 0.24 一族）。合并把 os 降为 0.4.1 后，0.4.4 与 openkal-linux 0.26.0 的请求仍在工作队列与约束串中，一个一致的图被以"irreconcilable versions"拒绝，报错把请求归于已不在图中的版本。

### F7 同一包的不同精确版本没有出口

库包把被广泛共享的依赖写成精确版本时，下游无法统一版本（#914）。mtest 引擎与一致性套件的 `mcpplibs.os = "0.4.1"` 是现成的例子。

### F8 失败的呈现不可读

- 失败细节先于所属用例的 `FAIL` 行出现；
- 布尔检查只报"the expectation did not hold"；`fatal` 不说明中止；
- 作为用例运行的 suite 在汇总中计作一个失败；
- mcpp 的汇总计的是程序而未标单位，并在每个程序下重复列出用例失败；
- 路径为绝对路径。

### F9 IDE 的并发门拒绝而不排队，发现与运行参数不一致

mcpp-ide 的"同一时刻一条命令"门在 test 与 run 的整个生命周期内持有，被占时拒绝；用例发现不带 `--release` 与 profile，与 release 的测试任务是两个配置。

### F10 每次点击都是一次批处理

IDE 每点一个用例启动一次 `mcpp test`：规划、构建检查、运行、退出。没有长期的会话，因此：

- 一个运行进行时无法插入另一个用例，只能排队或拒绝；
- 无法取消某一个用例，只能杀掉整个 `mcpp test`；
- 并发的多个 `mcpp test` 各自调度，不共享 jobs 与资源池（协议 P7）。

### F11 插件呈现的信息少于协议已携带的

与 GoogleTest 的编辑器集成（每个用例的输出、每个失败断言一条可跳转的消息、期望与实际的差异视图、逐个用例实时更新）相比：

- 用例的输出与程序的输出混在一起，记录中没有每个用例的输出；
- 插件只显示首个失败；`expected`、`actual` 已在记录中而未用于差异视图；
- 用例结束前没有状态（#917 的 case-stream 已提供 `begin`/`end`，插件未使用）；
- 没有"N 个检查中 M 个失败"的信息，协议中也没有这一字段。

## 3. 决定

### T1 被替换的版本撤回它的请求（已实现）

- 解析记录按请求者保存约束（`constraintsBy`）；合并把 `<ns>.<name>@<旧版本>` 记为已替换（`supersededRequesters`）。
- 工作队列中由已替换版本发出的项不再处理；合并只受仍在图中的请求者的约束。
- e2e 1066 复现了 F6 的次序：修复前失败，修复后通过。

### T2 根上的覆盖：`override = true`

```toml
[dependencies]
mcpplibs.os = { version = "0.4.2", override = true }

[workspace.dependencies]          # 工作区根：作用于整张图，成员不因此直接依赖它
mcpplibs.os = { version = "0.4.2", override = true }
```

- **语义：** 该条目的来源取代图中对同一包的其他全部要求，图中该包只有一个版本。来源可以是版本、`path` 或 `git`。
- **生效范围：** 只在根包自身的依赖表与工作区根的 `[workspace.dependencies]` 中生效；库包中的 `override` 被忽略并警告。
- **作用的图跟随所在的表：** `[dependencies]` 作用于目标图与测试图，`[dev-dependencies]` 只作用于测试图，`[build-dependencies]` 只作用于宿主图。
- **可追溯：** 每条被取代的要求打印一行（`Overrode mcpplibs.os: mcpplibs.mtest@0.5.2 asked 0.4.1 → 0.4.2 (mcpp.toml:12)`）；`mcpp.lock` 记录覆盖结果与覆盖标记，`--locked` 据此核对；`mcpp tree --why <pkg>` 列出每个请求者的要求与最终版本。
- **与平台版本组：** 覆盖生效并警告。SPEC-013 的原则是平台给出默认、工程可以覆盖（`[target.<selector>]` 覆盖平台的任意层，`platform = "..."` 选择其他发布或不选）；版本组成员不例外。警告说明该成员属于哪个平台的版本组、所得组合不在该组测试过的范围内；`mcpp tree --why` 显示"版本组给出 X，根上覆盖为 Y"。
- **实现位置：** `step4b_identity_version_merge` 之前，由根的覆盖决定一个包的唯一版本；合并只在没有覆盖时发生。

### T3 检查与报错

- 冲突报错给出可复制的 `override` 写法。
- 库包把被广泛共享的依赖写成精确版本时，`mcpp build` 给出提示，建议兼容范围；`mcpp publish` 把它作为必须处理的问题。"广泛共享"按数据判定：包声明 `[kernel-abi]`、提供 `mcpp:` 层，或被 index 中多个包依赖。

### T4 非叶子包的多版本改名不扩展

最常冲突的包（os、openkal 一族）都有 C 符号、头文件或进程级状态，不满足改名的前提（纯模块、私有依赖、依赖一致）。现有叶子包的改名保留。

### T5 测试图与构建图各有图文件

- 构建图写入 `build.ninja`，测试图写入 `test.ninja`；测试图的旁路文件（步骤记录、placements、c-abi 与 include 旁路、编译器上限）以及 `.ninja_log`、`.ninja_deps` 放在输出目录的 `test/` 下。
- 两个图共享目标文件、BMI 与链接产物的路径。共享产物的命令在两个图中一致（测量：交替运行时 ninja 无事可做），互不触发重建。
- 快路径、GC 与"首次规划"判断按图文件区分；旧布局（测试图写在 `build.ninja`）在下一次规划时被识别并改写。

### T6 测试前的包构建走快路径

为 `MCPP_BIN_<NAME>` 进行的包构建先调用 `try_fast_build`；放弃时才 `prepare_build`。

### T7 测试快路径与测试计划记录

- 测试图的规划写下**测试计划记录**：测试程序、各部分（parts）、启动方式（runner、run_under、环境）、协议目录，以及与 `try_fast_build` 相同的新鲜度依据（清单、锁、源文件树、路径依赖、运行时描述、工具链）。
- 无改动时，`mcpp test` 读取记录、运行 `ninja -f test.ninja`、直接运行测试，不规划。过滤（程序名、`--case`）在记录之上执行；改变图的选项与 `try_fast_build` 一样放弃快路径。
- 记录是 mcpp 的内部格式，不对外；对外的是 B5：`mcpp test --no-run --message-format json` 以它判断是否最新、只在需要时构建，并给出每个程序的启动描述。
- 目标：无改动的 `mcpp test` 在 mcppls 上低于 1 s。

### T8 构建目录锁在运行前释放

所有测试程序构建完成后、第一个测试运行前释放构建目录锁。运行期间测试程序可以在同一工作区调用 mcpp。

### T9 内层 mcpp 的配置一致

先测量 `PATH` 前缀改变了哪个指纹输入，再从指纹中去掉与运行有关、与配置无关的部分。测试环境的 `PATH` 是运行的需要，不进入配置身份。

### T10 `Compiling` 行只在实际编译时出现

测试路径不再预先打印包行；包行由进度模型在包实际有工作时打印。

### T11 失败的呈现（已实现）

框架一侧（mtest 库）的控制台布局：

```
  FAIL fs/values that differ  (2 of 3 checks failed)
       tests/test_out.cpp:8
           expect(eq(got, std::string("first")));
           expected  "first"
           actual    "second"
       tests/test_out.cpp:9
           expect(got == "first");
           did not hold
  FAIL fs/a fatal check stops the case  (1 of 1 checks failed, stopped)
       tests/test_out.cpp:14
           expect(fatal(v.has_value()));
           did not hold
           fatal: the rest of the case did not run
  ok   fs/a passing case
  FAIL fs  (2 of 3 cases failed)
1 passed, 2 failed, 0 skipped
```

运行器一侧（mcpp）：

- 不在每个程序下重复用例失败（框架的输出已在程序行之下）；
- 增加用例计数行 `cases: 1 passed, 2 failed, 0 skipped (in 1 program)`；
- `failures:` 成为索引，每个失败的程序下列出失败的叶子用例及其首个失败的位置；
- 程序计数行的文字不变。

这一呈现对任何实现协议的框架成立：运行器的汇总与索引只读协议事件。

### T12 列举与运行以同一方式启动程序（已实现）

`mtest run --run-under CMD` 与 `--env` 同时作用于列举阶段（P3）。

### T13 mtest 自身的依赖

- `engine` 与 `conformance` 的 `mcpplibs.os = "0.4.1"` 改为 `"^0.4.1"`；
- 工作区的 openkal 平台从 0.24.4 改为 0.25.0；
- CI 的 mcpp 改为 2026.11.10.1；
- 定时任务以 `mtest run --isolate` 运行 mtest 自身的测试，发现用例之间的顺序依赖；`writing-tests.md` 说明：一个用例建立它所需的状态，或由 suite 的 fixture 建立。

### T14 mcpp-ide 只经 mcpp test spec

mcpp-ide 1.4.0（#10）已合入；本节在其上以一个新 PR 实现。

- **数据来源：** 基础能力的记录（B3：列举与程序记录；O1 的 parts；O2 的 case-stream）、O3 与 O8 的字段，以及 O9 的会话事件。插件不按框架名分支；会话的命令取自 B6 的框架记录。
- **运行：** 工程的框架声明了 O9 时经会话发起；连续点击多个用例时，每次点击是一个 `run` 或 `add` 请求，立即调度，不排队、不拒绝。没有时回退到每次一个 `mcpp test` 进程，多个进程并发（T8 之后无锁冲突）。
- **取消：** 测试树上的停止按钮发 `cancel {case}` 或 `cancel {run}`。
- **并发门：** test 与 run 不再占用工程的独占门；clean、toolchain install、toolchain default 等破坏性或全局操作保留。
- **发现与运行的参数一致：** 二者以同一个参数构造函数生成 profile 与 target 参数。
- **发现：** 编辑停止后延迟执行；有会话时由 `test/plan` 回答，不再启动构建。
- **呈现（对应 F11）：**
  - 每个失败检查一条 TestMessage：位置可跳转，正文为源语句；有 `expected`/`actual` 时以差异视图呈现；`fatal` 标明用例在此中止；
  - 用例的输出只显示该用例的部分（O8 的 `output`），归属为 `shared` 时注明；
  - `begin` 时显示运行中，`end` 时更新；持续时间、跳过原因、重试次数随状态显示；
  - 用例节点显示 O3 的检查计数（`2 of 3 checks failed`）；
  - 输出面板使用 T11 的分层布局。

### T15 会话：规范定义（O9），框架的工具实现

会话不在 mcpp 中实现。mcpp test spec 把它规定为可选能力 O9（T18），由框架附带的运行器工具实现；mtest 0.6.0 的 `mtest serve` 是第一个实现。

- **接口（规范规定）：** JSON-RPC 2.0，标准流。

  | 方法 | 参数 | 结果 |
  |---|---|---|
  | `test/plan` | — | 程序、parts、用例及其位置（与 B3 的列举记录相同） |
  | `test/run` | 选择：程序、用例模式、kind、tag | 运行标识，立即返回 |
  | `test/add` | 运行标识、选择 | 把用例加入一个进行中的运行 |
  | `test/cancel` | 运行标识，或运行标识与用例 | 结束所指的运行或用例 |
  | `test/status` | 可选的运行标识 | 进行中的运行与用例 |
  | `shutdown` | — | 结束会话 |

  通知为 `test/case`（`begin`、`end`、`output`、`failure`，字段与 O2 的 case-stream 记录相同）、`test/program`（程序记录）、`test/run`（运行结束与计数）、`test/build`（构建开始、等待构建目录、结束）。
- **调度（规范规定行为，不规定算法）：**
  - 一个 `run` 的用例立即调度，与其他运行的用例共享并发数与资源池（O5）；
  - 同一程序已在运行时，新请求的用例以另一个进程运行，不等待；
  - 用例声明 `isolation = "case"` 或会话以 isolate 运行时，每个用例一个进程。
- **构建（经 mcpp 的基础能力 B5）：** 会话在调度前请 mcpp 判断程序是否最新并在需要时构建（`mcpp test --no-run --message-format json`，T7 之后无改动时不规划），再按 mcpp 给出的启动描述（argv、env、cwd、runner、run_under、协议目录）启动程序。构建状态与构建目录锁只在 mcpp 一处；会话中同一时刻至多一次构建请求。
- **取消（规范规定语义）：**
  - 用例独占进程时，结束该进程树（POSIX 进程组，Windows 作业对象），状态为 `cancelled`；
  - 用例与其他用例共用进程时，结束该进程，其中未结束的用例为 `cancelled`，已结束的保持结论；被选而未开始的用例以另一个进程继续，除非取消的是整个运行；
  - `cancelled` 是运行器的状态，出现在记录与存储中，不属于协议 P5.3 的结论集合，不影响程序退出状态的规则（P5.5）。
- **存储：** 会话的每个运行写入工程的运行存储（B4），与 `mcpp test` 的运行同一形式。
- **发现：** 框架在其包清单的测试框架条目中声明 `session = [命令...]`（B6）；mcpp 在 `--guide` 与框架记录中报告。插件只认规范的方法，不认识实现者。
- **mcpp 的职责：** 规范；B5（全部测试程序的启动描述，以及"只在需要时构建"）；B6（汇总并报告框架声明的能力与会话入口）；T7（B5 判断是否最新的依据）。

### T16 每个用例的输出、检查点与检查计数

- **协议（可选能力 O3，对所有框架）：**
  - `failure` 事件可带 `source`（失败处的源语句）与 `fatal`（为真时用例在此中止）；
  - `end` 事件可带 `checks`：`{"held": N, "failed": M}`。
  - 运行器忽略不认识的字段（P5.4），旧框架不受影响。
- **运行器（可选能力 O8）：** 运行器边运行边读报告（P5.1），把程序在一个用例的 `begin` 与 `end` 之间写出的标准输出与标准错误归于该用例，记录为 `output`；同一进程中有多个用例同时处于 `begin` 与 `end` 之间时，归属为 `shared`，并列出这些用例。mcpp 的批处理运行与 O9 的会话都实现它；协议不变。
- **mtest 库：** 写出 `checks`、`source` 与 `fatal`，并以 T11 的布局写控制台。

### T17 mtest 0.6.0：框架与工具

- **框架（库、适配器、C ABI）：** 实现协议的程序一侧，含 O3 的字段；控制台布局为 T11。
- **工具（`mtest`）：** 历史、分析、门禁、报告与 agent 的接口；T12 的启动修复；**O9 的会话实现**（`mtest serve` 增加 `test/*` 方法、共享调度器、按用例取消、`cancelled` 状态），经 B5 向 mcpp 取得构建与启动描述，不自己构建。
- **规范：** mtest 的 `spec/protocol.md` 改为指向 mcpp test spec，并列出 mtest 实现的可选能力（O2–O9）；mtest 的一致性套件按 mcpp test spec 的条款编号报告。
- **版本：** 0.6.0（mtest#10 由 0.5.3 改为 0.6.0）。

### T18 mcpp test spec：一份规范，基础 + 可选

把与测试有关的规范收敛为 `docs/specs/test.md`（SPEC-017 v2.0，"mcpp test"）：

- 并入：SPEC-017 v1（运行器、记录、存储）、SPEC-004 中的 `[test]` 键（SPEC-004 改为引用）、mtest 仓库的协议本体（版本 1 的条款编号 P1–P12 保留）、docs/50 §8 的测试流记录（docs/50 改为引用）。
- 规范由 mcpp 拥有；框架是实现。引擎中禁止框架名与框架特有分支（v1 §1 保留）。
- **基础能力（B）：** 每个框架与运行器必须实现；插件只依赖它们也能完整工作。

  | 编号 | 内容 | 实现方 |
  |---|---|---|
  | B1 | 测试程序的发现与 `[test]` 键（discover、programs、split、store） | mcpp |
  | B2 | 协议核心：识别（P2）、列出（P3）、选择（P4）、报告（P5）；L0 程序以退出状态判定 | 框架写，运行器读 |
  | B3 | 结果记录：列举记录（程序、用例、位置）、程序记录（用例结论、失败的位置与消息）| mcpp |
  | B4 | 运行存储：P12 的最小字段 | mcpp |
  | B5 | 构建与启动描述：程序是否最新、只在需要时构建（快路径），每个程序的启动描述（`--print-launch` 扩展到全部程序，`--no-run` 的记录携带它） | mcpp |
  | B6 | 框架声明：名字、guide、实现的可选能力（`capabilities`）、会话入口（`session`）；mcpp 汇总并报告 | 框架的包清单；mcpp |

- **可选能力（O）：** 由框架或其工具在 B6 中声明，由 mcpp 以 feature（docs/50 §4）宣告；每项写明实现方、声明方式与回退。

  | 编号 | 能力 | 实现方 | 没有时 |
  |---|---|---|---|
  | O1 | parts（一个程序以多个进程运行） | mcpp | 程序作为整体 |
  | O2 | case-stream（运行中的 `begin`/`end`） | 运行器 | 进程结束时更新 |
  | O3 | 检查点细节：`expected`、`actual`、`source`、`fatal`、`checks` | 框架 | 只有位置与消息 |
  | O4 | 指标与上下限（P6） | 框架；运行器读 | 无指标视图 |
  | O5 | 调度提示：isolation、timeout、retries、资源池（P3.3、P7） | 框架声明，运行器遵守 | 按程序整体运行 |
  | O6 | fixtures 与 components（P11） | 框架 | — |
  | O7 | 资源用量（P10） | 框架或运行器 | — |
  | O8 | 每个用例的输出归属 | 运行器 | 只有程序的输出 |
  | O9 | 会话（T15） | 框架的工具 | 插件以多个 `mcpp test` 进程并发 |

- **规范的结构：** §1 原则与分层；§2 基础能力 B1–B6；§3 可选能力 O1–O9，各一节（内容、实现方、声明、feature、回退、一致性检查）；§4 记录；§5 存储；§6 一致性：每个可选能力有协议一致性套件中的条款检查。
- **一致性套件：** 协议一致性套件按 B 与 O 的条款报告；一个框架声明的每项能力都必须通过对应条款。

## 4. 生态

| 仓库 | 内容 |
|---|---|
| openkal-image 0.5.5 | openkal 0.25 一族的范围，无源码改动（已发布、已镜像） |
| openkal-platform 0.25.0 | 版本组：openkal 0.25.0、linux 0.26.0、macos 0.23.0、windows 0.21.0、musl 0.29.0、llvm-runtime 0.20.0、image 0.5.5（已打 tag、已镜像，index 变更进行中） |
| mcpp | 默认平台改为 0.25.0；cenv 的映射表增加 FreeBSD 与 NetBSD（fe3b78c2） |

## 5. 顺序与依赖

```
openkal-platform 0.25.0 → mcpp-index（版本组的一次变更）
  → mcpp（PR #916）：T18 的规范（SPEC-017 v2.0）；默认平台 0.25.0；T1–T3、T5–T10；B5、B6；O3、O8 的运行器一侧
  → mtest 0.6.0（mtest#10）：O3 的字段、T11、T12、T13、O8 与 O9（`mtest serve`）；协议文档改为引用
  → mcpp 2026.11.10.1 发布
  → mcpp-ide 新 PR：T14
```

- T18 的规范先于两边的实现：mcpp 与 mtest 按同一份条款实现。
- T5 是 T6、T7 的前提；T7 是 B5 的前提；B5、B6 是 O9 的前提；T8 是 T14 回退路径的前提。
- mtest 0.6.0 的会话经 B5 调用 mcpp：它在 mcpp 的 PR 合入后以 PR 中的 mcpp 构建验证，发布在 mcpp 之后。
- T9 先测量。

## 6. 验收

| 项 | 标准 |
|---|---|
| T1 | e2e 1066 修复前失败、修复后通过；mtest 工作区以发布的 mcpp 解析成功 |
| T2 | 根上的 `override` 取代库的精确锁定并逐条打印；库中的 `override` 被警告并忽略；版本组成员的覆盖生效并警告；`--locked` 核对覆盖 |
| T3 | 冲突报错包含可复制的 `override` 写法 |
| T5、T6 | `mcpp test` 之后 `mcpp build` 仍走快路径（0.1 s 量级） |
| T7、B5 | 无改动的 `mcpp test` 在 mcppls 上低于 1 s；`mcpp test --no-run --message-format json` 无改动时不规划，并给出全部程序的启动描述 |
| T8 | 测试中调用同一工作区的 `mcpp build` 不等待锁 |
| T9 | 测试中的内层 `mcpp build` 与外层使用同一输出目录 |
| T10 | 无改动时不出现 `Compiling` 行 |
| T11、T12 | mtest 的 layout 测试与引擎测试；mcpp e2e 1035 |
| B6 | `--guide` 与框架记录列出每个框架声明的能力与会话入口；没有声明的框架只有基础能力 |
| O3、O8 | 一个用例写出的输出只出现在该用例的 `output`；两个并发用例的输出标为 `shared`；`checks` 与 mtest 的控制台计数一致；GoogleTest 适配器的程序没有 `checks` 时记录照常 |
| O9 | mtest 的 e2e 以 JSON-RPC 驱动 `mtest serve`：两个 `run` 并发；`add` 加入进行中的运行；`cancel` 一个长时间用例，状态为 `cancelled`、其余用例照常结束；源码改动后只请求 mcpp 构建一次；Linux、macOS、Windows 各一 |
| T18 | 协议一致性套件按 B 与 O 的条款报告；mtest 声明的每项能力通过对应条款 |
| T14 | 连续点击三个用例，三个并发运行，无"already running"；停止一个运行中的用例；差异视图显示期望与实际；发现与运行不出现两个输出目录；框架没有 O9 时以并发的 `mcpp test` 进程工作 |

## 7. 自审（本方案）

**架构与分层。** mcpp 只拥有规范与协议、实现基础能力；会话是可选能力，由框架的工具实现。插件只依赖规范：基础能力足以完整工作，可选能力按宣告启用。会话需要的构建状态与构建目录锁经 B5 由 mcpp 一处提供，会话不自己构建，因此"构建只在一处"与"会话不在 mcpp"同时成立。

**规范一致性。**
- T18 把四处来源收敛为一份，条款编号 P1–P12 保留，已有实现与引用不失效。
- O9 的 `cancelled` 只存在于运行器的记录与存储中，不改变 P5.3 的结论集合与 P5.5 的退出状态规则。
- O8 的归属由运行器完成，不要求框架改变输出方式。
- T2 与 SPEC-013 的"平台给默认、工程可覆盖"一致；覆盖不制造工作区成员的直接依赖。

**兼容性。**
- 清单只增加一个关键字；不认识 `override` 的旧 mcpp 把它作为未知键警告。
- 规范收敛不改变任何已发布字段；SPEC-004、docs/50 与 mtest 的协议文档改为引用。
- 记录、存储、协议只增字段；可选能力由 feature 宣告，新插件在旧 mcpp 或无 O9 的框架上回退。
- 图文件分开是一次性的布局迁移；程序计数行的文字不变。

**性能。** T5–T7 去掉无改动时的两次规划；B5 在无改动时不规划，会话每个请求的构建检查因此是毫秒级；共享调度器避免多个进程的超额并发；报告与输出按行流式处理。

**稳定性。** 会话中同一时刻至多一次构建请求，构建仍受构建目录锁约束；取消以进程组或作业对象结束整个进程树；会话异常退出时插件回退。T1 只影响已被替换的版本。

**跨平台。** 进程树的结束在 POSIX 用进程组、在 Windows 用作业对象；输出归属依赖报告的逐行追加（P5.1）；O9 的验收在三个系统上进行。

**用户体验。** 控制台、索引与插件使用同一组信息（位置、源语句、两侧、中止、计数、输出）；点击即运行，停止即停止；无改动时一秒内得到结果。

**风险与未决。**
- O9 的实现量在 mtest；它依赖 B5，而 B5 依赖 T5–T7。
- T9 的修法取决于测量结果。
- mtest 0.6.0 的发布排在 mcpp 之后（O9 经 B5 调用发布的 mcpp）。

## 8. 与跨平台生态方案合并的全局自审

本节把本方案与[跨平台生态方案](2026-11-10-cross-platform-ecosystem-design.md)（E1–E14、K1–K12、X1–X5、I1、RP）合并审查：当前状态、跨方案的依赖、发现的问题与处置。

### 8.1 当前状态（2026-11-10）

| 范围 | 项 | 状态 |
|---|---|---|
| mcpp PR #916 | E1–E14、X1/X2 的使用、I1 的前提、#917；T1、T11、cenv 的 BSD 行 | 已提交；CI 有两类未过（8.3 的 G1、G2） |
| mcpp PR #916 | T2、T3、T5–T10、B5、B6、O3/O8 运行器一侧、T18 规范、默认平台 0.25.0 | 待实现 |
| mcpp 的 mtest 迁移 | bench、CI（`feat/cross-platform-infra-mtest` 两个提交） | 待原测试集通过后追加 |
| openkal | K5、K6（openkal 0.25.0）、K11（musl 0.29.0）、llvm-runtime 0.20.0 | 已发布 |
| openkal | K2 openkal-posix 0.1.0 | 已发布、已镜像、已收录 |
| openkal | K8 openkal-freebsd | hosted 形态在 FreeBSD 虚拟机中通过一致性测试；standalone 形态待经 openkal-musl 验证 |
| openkal | openkal-image 0.5.5、openkal-platform 0.25.0 | 已发布、已镜像；platform 的 index 变更进行中 |
| openkal | K1（openkal-image 0.6.0） | 分支完成，待 mcpp 发布（需要 E13） |
| openkal | K9 | 待 K8 与 K1 |
| openkal | K7、K12 | 讨论稿已备，待提出 |
| xim | X1（`20261104-2`）、X2 | 已发布、已镜像、已收录 |
| mcpp-index | I1（#748） | 待合入 |
| mtest | mtest#10（0.5.3 → 0.6.0） | T11、T12 已实现；O3 字段、O8、O9、T13 待实现 |
| mcpp-ide | T14 | 待实现 |
| 运行器包 | RP（wine、qemu-user、系统虚拟机的 xpkg） | 未开始 |

### 8.2 跨方案的依赖

```
openkal-platform 0.25.0 进 index
  → mcpp #916 全绿（含默认平台 0.25.0）→ mcpp 2026.11.10.1 → xim 收录 → 真实安装验证
     → openkal-image 0.6.0（K1，需要 E13）
     → openkal-freebsd 0.1.0（K8，standalone 需要 cenv 的 BSD 行）→ openkal-musl 0.29.1（freebsd 条件）
     → openkal-platform 0.26.0（K1、K9：image 0.6.0、freebsd 的 defaults 与主机，mcpp 下限 2026.11.10.1）
     → mtest 0.6.0（O9 经 B5 调用发布的 mcpp）
     → mcpp-ide（T14）
  → mcpp 的下一版本把默认平台改为 0.26.0
```

mcpp 2026.11.10.1 的默认平台是 0.25.0，因为 0.26.0 需要该版本的 E13 才能构建；默认平台总是"本版本验证过的发布"，0.26.0 由下一版本采用，在此之前工程以 `platform = "openkal-platform@0.26.0"` 选择。

### 8.3 全局自审发现与处置

| # | 视角 | 发现 | 处置 |
|---|---|---|---|
| G1 | 稳定性 | Windows 与 macOS 的 e2e 在 PR 上稳定失败（15、16、77、78、100）："install path missing after fetch"。诊断（a5ce3190、d4c62042）显示 mcpp 期待的索引存储位于另一个测试的临时 home（`tmp.*/mcpp-home/xpkgs`）：一个隔离 home 的测试把它的存储写进了共享的 provider 配置 | 定位写入者；`own_indices` 只记录属于该 env 的 home 的存储；查找时忽略不存在的已声明存储并给出警告；e2e 断言隔离 home 的测试不改变外层配置 |
| G2 | 一致性 | openkal 交叉构建的格子失败：llvm-runtime main 需要 musl 0.29，默认平台 0.24.4 的版本组不含 | platform 0.25.0（已发布），index 变更后 mcpp 默认改为 0.25.0；格子按设计在生态先发布时变绿 |
| G3 | 分层 | 生态方案 §16 的"mtest 迁移"与本方案 T18 的关系未写明 | 迁移使用的是 mcpp test spec 的基础能力与 O2–O4；各仓库的 CI 只依赖记录与存储，不依赖 mtest 的内部格式 |
| G4 | 一致性 | 各仓库 `.github/versions.env` 固定 mtest 0.5.2；mtest 0.6.0 发布后版本分散 | 0.5.2 与 0.6.0 都实现基础能力，CI 的组合 action 只用基础能力与 `ci report`，不要求同步升级；此后改动到的仓库随改动升级 |
| G5 | 依赖 | P9（宿主不依赖）与 openkal-freebsd 的 CI 使用 vmactions 的 FreeBSD 虚拟机 | vmactions 是 CI 的运行环境，不是构建的宿主依赖；RP 的系统虚拟机 xpkg 完成后，`mcpp test` 经它在本地复现同一运行 |
| G6 | 简洁 | 版本组的修订节奏：一个成员的修复要求平台发一个补丁版本 | 保持（生态方案 §6.2 与 family_release 的规则）；本轮 0.25.0、0.26.0 两次，内容在 8.2 中确定 |
| G7 | 用户体验 | 默认平台与最新平台不同时，用户不知道有更新 | mcpp 现有的提示（"the index has openkal-platform X"）保留；0.26.0 发布后在 CHANGELOG 中说明选择方法 |
| G8 | 测试覆盖 | 本轮新增的规范面（T18 的 B/O、`override`、会话）各有 e2e 或一致性条款；跨仓库的组合（会话 + mcpp + 插件）只在 mcpp-ide 的端到端中覆盖 | mcpp-ide 的 PR 加入以 mock mcpp 与真实 mcpp 各跑一次的测试 |
| G9 | 范围 | PR #916 的范围进一步增大 | 按"每个仓库一个 PR"保留；提交按编号（E、T、B、O）分开；PR 描述按编号列出验证 |
| G10 | 资源 | 主机磁盘余量约 4 GB，其他工作区也在写入 | 只清理自己的 `target/` 与临时目录；大构建前检查余量 |

## 9. 实施记录

| 项 | 实施 | 验证 |
|---|---|---|
| T1 | 合并替换的版本撤回它的请求 | e2e 1066 |
| T2、T3 | 依赖条目上的 `override = true`（只在根上生效，库的覆盖给出警告并忽略）；锁文件记录 `override = true`，`--locked` 的漂移标注 `(override)`；平台组成员的覆盖给出警告；不可调和的请求在报错中给出 `override` 的写法；共享包的精确版本给出 `dependency/exact-pin` 提示，`mcpp publish` 拒绝库对共享包的精确版本 | e2e 1071；docs/05（中英文） |
| T4 | 不扩展（决定） | — |
| T5 | 测试图写入 `test.ninja`，与 `build.ninja` 共用 `.ninja_log` 与 `.ninja_deps`；两图的 dyndep 规则文本一致 | e2e 1069 |
| T6 | 测试前的包构建经构建快路径（工作区根的记录同样接受） | e2e 1069 |
| T7 | `test-plan.json` 与选择 `@test` 的构建缓存记录；接受条件为发现的测试与记录一致、`tests/**` 无变化、`ninja -n` 无工作 | e2e 1069；docs/08（中英文）；mcpp-language-server 的 `test_fs` 由 5.8 s 降至 0.13 s |
| T8 | 构建目录锁在测试运行前释放；协议目录按调用分开（`test-protocol/<pid>-<started_ns>/`） | e2e 1068 |
| T9 | 实测测试环境不改变配置指纹，无需改动；测试内的 `mcpp build` 由 e2e 覆盖 | e2e 1068 |
| T10 | `Compiling` 行只在实际编译时出现 | e2e 1069 |
| T11、T12 | 第一版已实现 | — |
| T13 | mtest 0.6.0 依赖 mcpplibs.os `^0.4.1` 与 openkal-platform 0.25.0 | mtest#10 CI |
| T14 | mcpp-ide 1.5.0：不认识任何框架；用例为程序的列举并保留至下一次列举；按标识运行；检查与输出；测试各占一条执行道 | mcpp-ide#11 |
| T15 | mtest 0.6.0 的 `mtest serve` 实现会话（`test/run`、`add`、`cancel`、`status` 与通知）；mcpp-ide 1.5.0 经会话运行用例，`mcpp.test.session` 选择 | mtest#10；mcpp-ide e2e/protocol；本地以真实 mcpp 与 `mtest serve` 验证 |
| T16 | 用例的检查计数与失败细节（O3）、输出（O8）：程序的 `output` 事件优先，否则单用例程序的全部输出，否则无 | e2e 1070；mtest 0.6.0 |
| T17 | mtest 0.6.0：库入口声明 `test-capabilities` 与 `test-session` | mtest#10 |
| T18 | `docs/specs/test.md`（SPEC-017 v2.0）：B1–B6、O1–O9、P1–P12；特性 `test-launch-records`、`test-frameworks`、`test-checks`、`test-output` | e2e 1070 |
| G1 | 借用他人注册表的 home 不改写注册表中索引的位置 | e2e 1067；docs/25（中英文） |

实施中的发现：openkal-linux 的 `bound_lifetime` 以 `PR_SET_PDEATHSIG` 实现，Linux 在创建子进程的线程结束时即发出该信号，而非进程结束时；在短寿命线程中创建、需与进程同寿命的子进程因此提前结束。mcpp-ide 的会话改为以输入结束为终止条件；该行为与 mcpplibs.os 的陈述之间的差异留待 openkal 讨论。

