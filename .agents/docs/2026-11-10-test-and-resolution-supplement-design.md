---
subject: ecosystem
status: active
---

# 测试与依赖解析补充方案：mcpp、mcpp test、mtest、mcpp-ide

**日期：** 2026-11-10
**状态：** 第一版，待审查
**上游文档：** [2026-11-10-cross-platform-ecosystem-design.md](2026-11-10-cross-platform-ecosystem-design.md)
**相关：** mcpp#914、mcpp#915、mcpp#917、mcpp#407、mcpp#839；mtest#10；mcpp-ide 1.4.0（#10）

本文记录跨平台生态实施过程中在测试与依赖解析上出现的九项发现（F1–F9），给出补充决定（T1–T14），并列出归属、顺序与验收标准。测量均在 mcpp-language-server 0.0.21 的独立克隆上进行（x86_64 Linux，`x86_64-linux-musl` 经 openkal-platform 0.24.4 的 hosted 形态，全局 BMI 缓存已就绪），使用 PR #916 的构建。

## 0. 决定摘要

| 编号 | 决定 | 归属 | 状态 |
|---|---|---|---|
| T1 | 被合并替换的版本撤回它发出的请求 | mcpp 解析器 | 已实现（e2e 1066） |
| T2 | 根上的覆盖：依赖条目的 `override = true`，不新增表 | mcpp 清单与解析器（#914 A） | 待实现 |
| T3 | 精确锁定共享依赖的检查，冲突报错给出 `override` 写法 | mcpp（#914 C） | 待实现 |
| T4 | 非叶子包的多版本改名不扩展 | mcpp（#914 B） | 决定 |
| T5 | 测试图与构建图各有图文件，共享目标文件 | mcpp test | 待实现 |
| T6 | 测试前的包构建走 `mcpp build` 的快路径 | mcpp test | 待实现 |
| T7 | 测试快路径：无改动时不规划 | mcpp test | 待实现 |
| T8 | 构建目录锁在测试运行前释放 | mcpp test（#915-1） | 待实现 |
| T9 | 测试进程中的内层 mcpp 得到与外层相同的配置 | mcpp（#915-2） | 待测量后实现 |
| T10 | `Compiling` 行只在实际编译时出现 | mcpp test | 待实现 |
| T11 | 失败的呈现：用例之下逐条列出位置、语句、两侧取值与是否中止；汇总分程序与用例计数 | mtest 0.5.3、mcpp | 已实现 |
| T12 | 列举阶段与运行阶段以同一方式启动程序 | mtest 0.5.3 | 已实现 |
| T13 | mtest 自身的共享依赖改为兼容范围，平台改为 0.25.0 | mtest 0.5.3 | 待实现 |
| T14 | IDE 的并发、发现与参数一致性 | mcpp-ide 1.4.x | 待实现 |

## 1. 发现

### F1 一次无改动的 `mcpp test` 有八成时间在规划

`mcpp test test_fs` 在无任何改动时的逐行计时：

| 阶段 | 用时 |
|---|---|
| 包本身的构建（为 `MCPP_BIN_<NAME>`）：宿主图与目标图各解析一次 | 2.65 s |
| 测试构建的规划：宿主图与目标图再各解析一次 | 2.3 s |
| ninja（无事可做） | 0.81 s |
| 运行 | 0.04 s |
| 合计 | 5.8 s |

同一工程上 `mcpp build` 的快路径为 0.06 s。

### F2 测试图与构建图互相使对方的快路径失效

一个配置只有一个输出目录和一个 `build.ninja`。`mcpp test` 写入测试图（含开发依赖与测试程序），`mcpp build` 写入构建图；快路径检查到图的形态不同即放弃（#407）。测量：`mcpp test` 之后的 `mcpp build` 从 0.06 s 变为 2.61 s。测试前的包构建也不走快路径，因此每次 `mcpp test` 都规划两次。

### F3 构建目录锁在测试运行期间未释放

`dirlock.cppm` 的设计说明"`mcpp test` 在第一个测试运行前释放"，代码只在 `--print-launch` 分支释放（`execute.cppm` 的 `ctx->buildLock.reset()`）。测试程序在同一工作区调用 `mcpp build` 时等待锁直至超时（#915）。

### F4 测试环境中的内层 mcpp 得到另一个配置指纹

`mcpp test` 给测试进程的 `PATH` 前置 subos 的 `bin`；内层 `mcpp build` 在此环境中解析出另一个输出目录，从头构建（#915）。改变指纹的具体输入尚未测量。

### F5 `Compiling` 行无条件打印

测试路径在构建前为每个包打印 `Compiling`（`test_announce`），随后阶段 A 的进度模型又在实际工作时打印一次。无改动时仍显示 `Compiling`，同一个包有时出现两次。`mcpp build` 只在包实际有编译工作时打印。

### F6 合并替换的版本留下了它的请求

mtest 工作区中，`lib` 要求 `mcpplibs.os ^0.4.1`（解析为 0.4.4，其依赖为 openkal 0.25 一族），`engine` 精确锁定 `0.4.1`（openkal 0.24 一族）。合并把 os 降为 0.4.1 后，0.4.4 及其依赖 openkal-linux 0.26.0 发出的请求仍在工作队列与约束串中，解析以"irreconcilable versions"拒绝了一个本身一致的图。报错把请求归于已不在图中的版本。

### F7 同一包的不同精确版本没有出口

库包把被广泛共享的依赖（os、openkal）写成精确版本时，下游无法统一版本；"Pin one version explicitly" 在冲突一方是他人的精确锁定时无从执行（#914）。mtest 引擎与一致性套件的 `mcpplibs.os = "0.4.1"` 是现成的例子。

### F8 失败的呈现不可读

- 失败细节先于所属用例的 `FAIL` 行出现；
- 布尔检查只报"the expectation did not hold"，没有语句与取值；
- `fatal` 检查不说明用例在此中止；
- 作为用例运行的 suite 在汇总中计作一个失败；
- mcpp 的汇总"0 passed; 1 failed"计的是程序，未标单位，并在每个程序下重复列出用例失败；
- 路径为绝对路径。

### F9 IDE 侧的并发与参数

- mcpp-ide 的"同一时刻一条命令"门（`mcpp.runtime.concurrency`）在 test 与 run 的整个生命周期内持有，门被占时拒绝而不排队；
- 用例发现（`test/cases` 的列举）不带 `--release` 与 profile，测试任务在 profile 为 release 时带 `--release`，二者是两个配置，改动后各自编译一遍；
- 后台发现与用户发起的运行争用同一构建目录锁。

## 2. 决定

### T1 被替换的版本撤回它的请求（已实现）

- 解析记录按请求者保存约束（`constraintsBy`）。
- 合并把 `<ns>.<name>@<旧版本>` 记为已替换（`supersededRequesters`）。
- 工作队列中由已替换版本发出的项不再处理；合并只受仍在图中的请求者的约束。
- e2e 1066 复现了 F6 的次序：修复前失败，修复后通过。

### T2 根上的覆盖：`override = true`

清单不新增表，在依赖条目上增加一个关键字：

```toml
[dependencies]
mcpplibs.os = { version = "0.4.2", override = true }

[workspace.dependencies]          # 工作区根：作用于整张图，成员不因此直接依赖它
mcpplibs.os = { version = "0.4.2", override = true }
```

- **语义：** 该条目的来源取代图中对同一包的其他全部要求，图中该包只有一个版本。来源可以是版本、`path` 或 `git`；后两者用于在本地修复一个被广泛依赖的包时整图替换。
- **生效范围：**
  - 只在根包自身的依赖表与工作区根的 `[workspace.dependencies]` 中生效；
  - 库包中的 `override` 被忽略并警告：覆盖是应用的决定，不沿依赖链传播。
- **作用的图跟随所在的表：**
  - `[dependencies]` 作用于目标图与测试图；
  - `[dev-dependencies]` 只作用于测试图；
  - `[build-dependencies]` 只作用于宿主图。
- **可追溯：**
  - 每条被取代的要求打印一行：`Overrode mcpplibs.os: mcpplibs.mtest@0.5.2 asked 0.4.1 → 0.4.2 (mcpp.toml:12)`；
  - `mcpp.lock` 记录覆盖结果与覆盖标记，`--locked` 据此核对；
  - `mcpp tree --why <pkg>` 列出每个请求者的要求与最终版本。
- **与平台版本组：** 覆盖平台 `[platform.versions]` 中的成员被拒绝。错误说明该成员属于哪个平台的版本组，并给出两条出路：选择包含所需版本的平台发布，或 `platform = "none"`。版本组是一起构建与测试过的组合，覆盖其成员得到的是未经测试的组合。
- **实现位置：** 在 `step4b_identity_version_merge` 之前，由根的覆盖表决定一个包的唯一版本；合并只在没有覆盖时发生。

### T3 检查与报错

- 冲突报错给出可复制的 `override` 写法。
- 库包（有 `[targets]` 中 `kind = "lib"` 的包，或被他人依赖的包）把被广泛共享的依赖写成精确版本时，`mcpp build` 给出提示，建议兼容范围；`mcpp publish` 把它作为必须处理的问题。"广泛共享"的判定是数据而非硬编码：一个包声明 `[kernel-abi]`、提供 `mcpp:` 层，或被 index 中多个包依赖。

### T4 非叶子包的多版本改名不扩展

最常冲突的包（os、openkal 一族）都有 C 符号、头文件或进程级状态，不满足改名的前提（纯模块、私有依赖、依赖一致）。扩展的覆盖面小而风险高。现有叶子包的改名保留。

### T5 测试图与构建图各有图文件

- 一个输出目录中，构建图写入 `build.ninja`，测试图写入 `test.ninja`。
- 两个图各有一组旁路文件（步骤记录、placements、c-abi 与 include 旁路、编译器上限）：测试图的放在 `test/` 子目录下，`.ninja_log` 与 `.ninja_deps` 也分开。
- 两个图共享目标文件、BMI 与链接产物的路径。共享产物的命令在两个图中一致（测量：交替运行时 ninja 无事可做），因此互不触发重建。
- 快路径、GC 与"首次规划"判断按图文件区分；`is_plain_build_graph` 的检查保留，用于识别旧布局。

### T6 测试前的包构建走快路径

`run_tests` 中为 `MCPP_BIN_<NAME>` 进行的包构建先调用 `try_fast_build`；快路径放弃时才 `prepare_build`。有 T5 之后，构建图的记录不再被测试图覆盖，快路径可用。

### T7 测试快路径

- 测试图的规划写下一个记录：测试程序、各自的启动方式（runner、run_under、环境）、协议目录、选择与分部（parts）规则，以及与 `try_fast_build` 相同的新鲜度依据（清单、锁、源文件树、路径依赖、运行时描述、工具链）。
- 无改动时，`mcpp test` 读取记录、运行 `ninja -f test.ninja` 并直接运行测试，不规划。
- 过滤（程序名、`--case`）在记录之上执行。改变图的选项（`--target`、`--profile`、`--features` 等）与 `try_fast_build` 一样放弃快路径。
- 目标：无改动的 `mcpp test` 在 mcppls 上低于 1 s。

### T8 构建目录锁在运行前释放

所有测试程序构建完成后、第一个测试运行前释放 `ctx->buildLock`，与 `dirlock.cppm` 的设计一致。运行期间测试程序可以在同一工作区调用 mcpp。e2e 新增一个测试，在其中调用同一工作区的 `mcpp build`。

### T9 内层 mcpp 的配置一致

先测量 `PATH` 前缀改变了哪个指纹输入，再从指纹中去掉与运行时有关、与配置无关的部分。原则：测试环境的 `PATH` 是运行的需要，不进入配置身份。e2e 断言内层 `mcpp build` 与外层使用同一输出目录。

### T10 `Compiling` 行只在实际编译时出现

测试路径不再预先打印包行；包行由阶段 A 与阶段 B 的进度模型在包实际有工作时打印，与 `mcpp build` 一致。测试程序自身的 `Compiling <test> (test)` 也只在它实际编译或链接时出现。

### T11 失败的呈现（已实现）

mtest 库（0.5.3）：

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

- 用例的失败缓存在用例内，在用例行之后打印；
- 布尔检查读源码给出语句；比较给出两侧；
- `fatal` 说明中止；
- 用例行给出失败的检查数（通过的检查以一个无锁计数器统计）；
- 声明了用例的用例（suite）给出其中失败的用例数，不计入控制台汇总；
- 工作目录下的路径以相对形式给出；
- 协议的 `failure` 事件增加 `source` 字段（P5.4：未知字段被忽略）。

mcpp：

- 不再在每个程序下重复用例失败；
- 汇总后增加用例计数行 `cases: 1 passed, 2 failed, 0 skipped (in 1 program)`；
- `failures:` 成为索引：每个失败的程序下列出失败的叶子用例及其首个失败的位置。
- 程序计数行的文字不变（e2e 与工具匹配它）。

### T12 列举与运行以同一方式启动程序（已实现）

`mtest run --run-under CMD` 与 `--env` 同时作用于列举阶段（P3）。由 runner 才能启动的程序（另一系统的程序，在该系统的虚拟机或模拟器中运行）可经 mtest 列举与运行。

### T13 mtest 自身的依赖

- `engine` 与 `conformance` 的 `mcpplibs.os = "0.4.1"` 改为 `"^0.4.1"`，与 `lib` 一致；
- 工作区的 openkal 平台选择从 0.24.4 改为 0.25.0（与 os 0.4.4 的 openkal 0.25 一族一致）；
- CI 的 mcpp 改为 2026.11.10.1 发布后的版本。

用例之间的顺序依赖（一个用例依赖前面用例建立的文件）由 `mtest run --isolate`（每个用例一个进程）发现；mtest 在其 CI 的定时任务中以此方式运行自身测试，并在 `writing-tests.md` 中说明：一个用例建立它所需的状态，或由 suite 的 fixture 建立。

### T14 mcpp-ide

mcpp-ide 1.4.0 的 #10 已合入；本节的改动在 1.4.0 之上以一个 PR 实现。

- **并发门：**
  - test 与 run 不再占用工程的独占门：构建阶段的互斥由 mcpp 的构建目录锁保证，运行阶段无需互斥；
  - mcpp 报告等待锁时（`Waiting for the build directory`，事件流中的等待事件），IDE 显示为排队状态；
  - clean、toolchain install、toolchain default 等破坏性或全局操作保留独占门。
- **连续点击多个用例：** 同一程序的多个用例合并为一次 `mcpp test <program> --case A --case B`；不同程序的请求排队，不拒绝。
- **发现与运行的参数一致：** 用例发现与测试任务使用同一组 profile 与 target 参数（同一个参数构造函数），二者是同一个配置。
- **发现防抖与让路：** 后台发现在编辑停止后延迟执行；用户发起的命令运行时暂停发现，命令结束后再进行。

## 3. 生态

| 仓库 | 内容 |
|---|---|
| openkal-image 0.5.5 | openkal 0.25 一族的范围，无源码改动（已发布、已镜像） |
| openkal-platform 0.25.0 | 版本组：openkal 0.25.0、linux 0.26.0、macos 0.23.0、windows 0.21.0、musl 0.29.0、llvm-runtime 0.20.0、image 0.5.5（已打 tag、已镜像，index 变更进行中） |
| mcpp | 默认平台改为 0.25.0；cenv 的映射表增加 FreeBSD 与 NetBSD（`posix`/`arch-default` 与 Linux 相同） |

## 4. 顺序与依赖

```
openkal-platform 0.25.0 → mcpp-index（版本组的一次变更）
  → mcpp：默认平台 0.25.0；T1、T2、T3、T5–T11（PR #916）
  → mtest 0.5.3：T11、T12、T13（mtest#10）
  → mcpp 2026.11.10.1 发布
  → mcpp-ide：T14
```

T2 与 T3 不依赖其他决定；T5 是 T6 与 T7 的前提；T9 先测量。

## 5. 验收

| 项 | 标准 |
|---|---|
| T1 | e2e 1066 在修复前失败、修复后通过；mtest 工作区以发布的 mcpp 解析成功 |
| T2 | 根上的 `override` 取代库的精确锁定并逐条打印；库中的 `override` 被警告并忽略；平台版本组成员的覆盖被拒绝；`--locked` 核对覆盖 |
| T3 | 冲突报错包含可复制的 `override` 写法 |
| T5、T6 | `mcpp test` 之后 `mcpp build` 仍走快路径（0.1 s 量级） |
| T7 | 无改动的 `mcpp test` 在 mcppls 上低于 1 s；改动一个源文件后只重建受影响的部分 |
| T8 | 测试中调用同一工作区的 `mcpp build` 不等待锁 |
| T9 | 测试中的内层 `mcpp build` 与外层使用同一输出目录 |
| T10 | 无改动时不出现 `Compiling` 行 |
| T11、T12 | mtest 的 layout 测试与引擎测试；mcpp e2e 1035 |
| T14 | IDE 中连续点击三个用例，三个都运行，无"already running"提示；发现与运行不出现两个输出目录 |

## 6. 兼容性

- 清单只增加一个关键字；不认识 `override` 的旧 mcpp 把它作为未知键警告，依赖照常解析。
- 图文件分开后，旧布局（测试图写在 `build.ninja`）在下一次规划时被识别并改写，一次性。
- 协议的 `source` 字段与 mcpp 汇总的用例行都是增加；程序计数行的文字不变。
- IDE 的门只放宽 test 与 run；破坏性操作的保护不变。
