---
subject: ecosystem
status: active
---

# 测试与依赖解析补充方案：mcpp、mcpp test、mtest、mcpp-ide

**日期：** 2026-11-10
**状态：** 第二版，待审查（第一版的 T2 平台规则、T14 已按审查意见修改；新增分层原则、T15–T17 与自审）
**上游文档：** [2026-11-10-cross-platform-ecosystem-design.md](2026-11-10-cross-platform-ecosystem-design.md)
**相关：** mcpp#914、mcpp#915、mcpp#917、mcpp#407、mcpp#839；SPEC-017；mtest 测试协议版本 1；mtest#10；mcpp-ide 1.4.0

本文记录跨平台生态实施过程中在测试与依赖解析上出现的发现（F1–F11），给出补充决定（T1–T17），并列出归属、顺序、验收与自审。测量均在 mcpp-language-server 0.0.21 的独立克隆上进行（x86_64 Linux，`x86_64-linux-musl` 经 openkal-platform 0.24.4 的 hosted 形态，全局 BMI 缓存已就绪），使用 PR #916 的构建。

## 0. 分层原则

测试涉及四层，各层只依赖其下一层的规范：

| 层 | 角色 | 依赖的规范 | 禁止 |
|---|---|---|---|
| 编辑器插件（mcpp-ide、mcpp-xx） | 呈现测试树、结果、检查点与输出；发起运行与取消 | SPEC-017（mcpp 的记录、存储与会话） | 直接调用任何测试框架或其引擎；按框架名分支 |
| mcpp（构建系统与测试运行器） | 构建测试程序，按协议列出、选择、运行、调度与取消，产出记录与存储 | 测试协议（运行器一侧）、SPEC-004 的 `[test]` | 含有框架名或框架特有分支（SPEC-017 §1） |
| 测试协议 | 程序与运行器之间的契约：变量、事件、选择、报告 | — | 规定某一框架的写法 |
| 测试框架（mtest、GoogleTest 适配器、脚本适配器、C ABI） | 实现协议的程序一侧，写出事件；可提供自己的引擎（如 `mtest`） | 测试协议 | 要求运行器认识自己 |

由此：

- 插件需要的一切（测试树、每个用例的状态、检查点、期望与实际、每个用例的输出、并发运行与取消）都由 SPEC-017 规定、由 mcpp 提供。
- mtest 的引擎（`mtest run`、`mtest serve`）是 mtest 自己的工具：历史、分析、门禁、agent 的 MCP 接口；它不是插件与 mcpp 之间的一环。
- 协议的增加（例如检查计数、失败的源语句）在协议中规定、对所有框架可选；mtest 是第一个实现，mcpp 按协议读取，不按框架。

## 1. 决定摘要

| 编号 | 决定 | 层 / 归属 | 状态 |
|---|---|---|---|
| T1 | 被合并替换的版本撤回它发出的请求 | mcpp 解析器 | 已实现（da960014，e2e 1066） |
| T2 | 根上的覆盖：依赖条目的 `override = true`，不新增表 | mcpp 清单与解析器（#914 A） | 待实现 |
| T3 | 精确锁定共享依赖的检查；冲突报错给出 `override` 写法 | mcpp（#914 C） | 待实现 |
| T4 | 非叶子包的多版本改名不扩展 | mcpp（#914 B） | 决定 |
| T5 | 测试图与构建图各有图文件，共享目标文件 | mcpp test | 待实现 |
| T6 | 测试前的包构建走 `mcpp build` 的快路径 | mcpp test | 待实现 |
| T7 | 测试快路径与测试计划记录 | mcpp test（SPEC-017 §8） | 待实现 |
| T8 | 构建目录锁在测试运行前释放 | mcpp test（#915-1） | 待实现 |
| T9 | 测试进程中的内层 mcpp 得到与外层相同的配置 | mcpp（#915-2） | 待测量后实现 |
| T10 | `Compiling` 行只在实际编译时出现 | mcpp test | 待实现 |
| T11 | 失败的呈现：控制台布局（框架）与汇总、索引（运行器） | mtest 库；mcpp | 已实现（mtest 工作区；83024fa6） |
| T12 | 列举与运行以同一方式启动程序 | mtest 引擎 | 已实现（mtest#10） |
| T13 | mtest 自身的依赖改为兼容范围，平台改为 0.25.0 | mtest | 待实现 |
| T14 | mcpp-ide 只经 SPEC-017 获取与发起测试；新开 PR | mcpp-ide | 待实现 |
| T15 | 测试会话：并发与动态的运行、插入与取消 | mcpp（SPEC-017 §9） | 待实现 |
| T16 | 每个用例的输出、检查点与检查计数 | 协议（P5）；mcpp（SPEC-017 §5）；mtest 库 | 待实现 |
| T17 | mtest 0.6.0：框架与引擎的职责 | mtest | 待实现 |

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
- 记录是 SPEC-017 §8 规定的内部格式；T15 的会话以它回答"程序是否最新"。
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

### T14 mcpp-ide 只经 SPEC-017

mcpp-ide 1.4.0（#10）已合入；本节在其上以一个新 PR 实现。

- **数据来源：** 测试树、状态、检查点、输出均来自 mcpp：`--cases` 的记录（parts、位置）、case-stream 与 T15 的会话事件、T16 的字段。插件不调用 `mtest`，不按框架名分支。
- **运行：** 有 `test-session` feature 时经 T15 的会话发起；连续点击多个用例时，每次点击是一个 `run` 或 `add` 请求，立即调度，不排队、不拒绝。没有该 feature 时回退到每次一个 `mcpp test` 进程，多个进程并发（T8 之后无锁冲突）。
- **取消：** 测试树上的停止按钮发 `cancel {case}` 或 `cancel {run}`。
- **并发门：** test 与 run 不再占用工程的独占门；clean、toolchain install、toolchain default 等破坏性或全局操作保留。
- **发现与运行的参数一致：** 二者以同一个参数构造函数生成 profile 与 target 参数。
- **发现：** 编辑停止后延迟执行；有会话时由会话的计划记录回答，不再启动构建。
- **呈现（对应 F11）：**
  - 每个失败检查一条 TestMessage：位置可跳转，正文为源语句；有 `expected`/`actual` 时以差异视图呈现；`fatal` 标明用例在此中止；
  - 用例的输出只显示该用例的部分（T16 的 `output`），归属为 `shared` 时注明；
  - `begin` 时显示运行中，`end` 时更新；持续时间、跳过原因、重试次数随状态显示；
  - 用例节点显示 T16 的检查计数（`2 of 3 checks failed`）；
  - 输出面板使用 T11 的分层布局。

### T15 测试会话（SPEC-017 §9）

`mcpp test --serve` 在标准流上以 JSON-RPC 2.0 提供一个工程的长期测试会话，由 feature `test-session` 宣告。

- **状态：** 会话持有 T7 的测试计划记录与一个调度器；调度器的并发数与 `mcpp test` 运行测试的并发数相同（`[build] jobs` 与机器的 `default_jobs`，未设时为处理器数），并按协议 P7 的资源池约束用例。
- **方法：**

  | 方法 | 参数 | 结果 |
  |---|---|---|
  | `test/plan` | — | 程序、parts、用例及其位置（与 `--cases` 的记录相同） |
  | `test/run` | 选择：程序、`case` 模式、`kind`、`tag` | `run` 标识，立即返回 |
  | `test/add` | `run`、选择 | 把用例加入一个进行中的运行 |
  | `test/cancel` | `run`，或 `run` 与 `case` | 结束所指的运行或用例 |
  | `test/status` | 可选 `run` | 进行中的运行与用例 |
  | `shutdown` | — | 结束会话 |

- **通知：** `test/case`（`begin`、`end`、`output`、`failure`，字段与 case-stream 记录相同）、`test/program`（程序记录）、`test/run`（运行结束与计数）、`test/build`（构建开始、等待构建目录、结束）。
- **调度：**
  - 一个 `run` 请求的用例立即进入调度器，与其他运行的用例共享并发数与资源池；
  - 一个程序的多个被选用例在一个进程中运行（`MTEST_SELECT`）；用例声明 `isolation = "case"` 或会话以 `isolate` 运行时，每个用例一个进程；
  - 同一程序已在运行时，新请求的用例以另一个进程运行，不等待前一个进程。
- **构建：** 调度前以计划记录判断程序是否最新；有改动时会话进行一次构建（同一时刻至多一次，持有构建目录锁），请求只等待它所需的程序；构建失败以 `test/build` 与受影响的用例的 `end`（`status: "fail"`，原因为构建失败）报告。
- **取消：**
  - 用例独占进程时，结束该进程（POSIX 为进程组，Windows 为作业对象），该用例的状态为 `cancelled`；
  - 用例与其他用例共用进程时，结束该进程，其中未结束的用例为 `cancelled`，已结束的保持其结论；其余被选而未开始的用例以另一个进程继续，除非取消的是整个运行；
  - `cancelled` 是运行器的状态，不是程序的结论：它出现在记录与存储中（SPEC-017 §5、§6），协议 P5.3 的状态集合不变；程序的结论只由它结束的用例决定。
- **存储：** 会话的每个运行与 `mcpp test` 的一次运行同样保存在 `target/mtest`（SPEC-017 §6）。
- **与 agent：** 会话是 JSON-RPC；同一组方法也可由 mcpp 的 MCP 入口提供（不在本轮）。mtest 的 `mtest serve` 保持为 mtest 自身的接口。

### T16 每个用例的输出、检查点与检查计数

- **协议（P5，可选字段，对所有框架）：**
  - `failure` 事件可带 `source`（失败处的源语句）与 `fatal`（为真时用例在此中止）；
  - `end` 事件可带 `checks`：`{"held": N, "failed": M}`，用例中检查的次数。
  - 运行器忽略不认识的字段（P5.4），旧框架不受影响。
- **运行器（SPEC-017 §5）：**
  - 记录与 case-stream 的 `end` 携带 `checks`，失败携带 `source` 与 `fatal`；
  - **每个用例的输出：** 运行器边运行边读报告（P5.1），把程序在一个用例的 `begin` 与 `end` 之间写出的标准输出与标准错误归于该用例，记录为 `output`；同一进程中有多个用例同时处于 `begin` 与 `end` 之间时，归属为 `shared`，并列出这些用例。归属在运行器中完成，协议不变。
- **mtest 库：** 写出 `checks`、`source` 与 `fatal`（T11 已统计与输出），并以 T11 的布局写控制台。

### T17 mtest 0.6.0：框架与引擎的职责

- **框架（库、适配器、C ABI）：** 实现协议的程序一侧，包括 T16 的字段；控制台布局为 T11。
- **引擎（`mtest`）：** 历史、分析、门禁、报告与 agent 的接口；T12 的启动修复；它可以运行 mcpp 构建的程序（mcpp 的存储即 mtest 的存储，P12），不在 mcpp 与插件之间。
- **规范：** 协议文档记录 T16 的字段；`docs/service.md` 说明 `mtest serve` 与 mcpp 会话的关系：前者是 mtest 的工具，后者是构建系统的接口。
- **版本：** 协议字段与控制台布局是新功能，mtest 以 0.6.0 发布（mtest#10 由 0.5.3 改为 0.6.0）。

## 4. 生态

| 仓库 | 内容 |
|---|---|
| openkal-image 0.5.5 | openkal 0.25 一族的范围，无源码改动（已发布、已镜像） |
| openkal-platform 0.25.0 | 版本组：openkal 0.25.0、linux 0.26.0、macos 0.23.0、windows 0.21.0、musl 0.29.0、llvm-runtime 0.20.0、image 0.5.5（已打 tag、已镜像，index 变更进行中） |
| mcpp | 默认平台改为 0.25.0；cenv 的映射表增加 FreeBSD 与 NetBSD（fe3b78c2） |

## 5. 顺序与依赖

```
openkal-platform 0.25.0 → mcpp-index（版本组的一次变更）
  → mtest 0.6.0：协议字段（T16）、T11、T12、T13（mtest#10）
  → mcpp（PR #916）：默认平台 0.25.0；T1–T3、T5–T10、T15、T16 的运行器一侧；SPEC-017 §5、§8、§9
  → mcpp 2026.11.10.1 发布
  → mcpp-ide 新 PR：T14
```

- T5 是 T6、T7 的前提；T7 是 T15 的前提；T8 是 T14 回退路径的前提。
- T16 的协议字段先在 mtest 中规定与实现，mcpp 按协议读取；mcpp 对没有这些字段的程序照常工作。
- T9 先测量。

## 6. 验收

| 项 | 标准 |
|---|---|
| T1 | e2e 1066 修复前失败、修复后通过；mtest 工作区以发布的 mcpp 解析成功 |
| T2 | 根上的 `override` 取代库的精确锁定并逐条打印；库中的 `override` 被警告并忽略；版本组成员的覆盖生效并警告；`--locked` 核对覆盖 |
| T3 | 冲突报错包含可复制的 `override` 写法 |
| T5、T6 | `mcpp test` 之后 `mcpp build` 仍走快路径（0.1 s 量级） |
| T7 | 无改动的 `mcpp test` 在 mcppls 上低于 1 s；改动一个源文件后只重建受影响的部分 |
| T8 | 测试中调用同一工作区的 `mcpp build` 不等待锁 |
| T9 | 测试中的内层 `mcpp build` 与外层使用同一输出目录 |
| T10 | 无改动时不出现 `Compiling` 行 |
| T11、T12 | mtest 的 layout 测试与引擎测试；mcpp e2e 1035 |
| T15 | e2e 以 JSON-RPC 驱动会话：两个 `run` 并发；`add` 加入进行中的运行；`cancel` 一个长时间用例，状态为 `cancelled`、其余用例照常结束；有改动时只构建一次；Linux、macOS、Windows 各一 |
| T16 | 一个用例写出的输出只出现在该用例的 `output`；两个并发用例的输出标为 `shared`；`checks` 与 mtest 的控制台计数一致；GoogleTest 适配器的程序没有 `checks` 时记录照常 |
| T14 | 连续点击三个用例，三个并发运行，无"already running"；停止一个运行中的用例；差异视图显示期望与实际；发现与运行不出现两个输出目录 |

## 7. 自审

**架构与分层。** 插件只依赖 SPEC-017；mcpp 只依赖协议；协议的增加是可选字段；mtest 是实现之一。会话放在 mcpp 而非 mtest 引擎，是因为调度依赖构建（程序是否最新、构建目录锁）与测试计划，这两者属于构建系统；放在 mtest 引擎会让插件经由一个具体框架的工具获取 mcpp 的状态，违反第 0 节。

**规范一致性。**
- T2 与 SPEC-013 的"平台给默认、工程可覆盖"一致；与 `[workspace.dependencies]` 的继承规则一致：覆盖不制造成员的直接依赖。
- T15 的 `cancelled` 只存在于运行器的记录与存储中，不改变协议 P5.3 的结论集合，也不改变程序的退出状态规则（P5.5）。
- T16 的输出归属由运行器完成，不要求框架改变输出方式。

**兼容性。**
- 清单只增加一个关键字；不认识 `override` 的旧 mcpp 把它作为未知键警告，依赖照常解析。
- 记录、存储、协议都只增字段；会话由 feature 宣告，旧插件不受影响，新插件在旧 mcpp 上回退到批处理。
- 图文件分开是一次性的布局迁移；程序计数行的文字不变。

**性能。**
- T5–T7 去掉无改动时的两次规划；会话把计划记录留在内存中，每个请求只做新鲜度检查。
- 共享调度器避免多个 `mcpp test` 进程的超额并发。
- 读取报告、归属输出是逐行的流处理，不随运行长度增加内存。

**稳定性。**
- 会话中同一时刻至多一次构建，仍受构建目录锁约束，与外部 `mcpp build` 互斥。
- 取消以进程组或作业对象结束整个进程树，不留下孤儿进程；会话异常退出时插件回退到批处理。
- T1 的撤回只影响已被替换的版本，不改变无合并时的解析。

**跨平台。** 进程树的结束在 POSIX 用进程组、在 Windows 用作业对象（mcpplibs.os 的 job 单元）；输出归属依赖报告的逐行追加（P5.1），与系统无关；T15 的验收在三个系统上进行。

**用户体验。** 控制台、索引与插件的呈现使用同一组信息（位置、源语句、两侧、中止、计数、输出）；点击即运行，停止即停止；无改动时一秒内得到结果。

**测试覆盖。** 每项决定在第 6 节有可执行的验收；T15 与 T16 新增 e2e，覆盖三个系统；协议字段由 mtest 的一致性套件覆盖。

**风险与未决。**
- T15 是本轮最大的一项，集中在 mcpp 的测试路径；它依赖 T7，因此在 T5–T7 稳定之后实现。
- T9 的修法取决于测量结果。
- PR #916 的范围因此再增加；按"每个仓库一个 PR"保留在其中，提交按决定编号分开，便于审查。
