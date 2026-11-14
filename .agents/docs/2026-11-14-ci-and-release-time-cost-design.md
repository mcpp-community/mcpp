---
subject: ci
status: active
---

# CI 与发布的时间成本：按测量调度的测试、分片、并行发布

**日期：** 2026-11-14
**状态：** 已实施（mcpp 2026.11.13.1，PR #930）。负责人于 2026-11-14 确认第 3 节的决定 1–4，并要求 P1–P8 中
有效且不破坏测试的各项一并实施；第 4 节记录未采纳项及其测量。

## 1. 测量

数据取自 PR #930 的 CI 运行（43323959609、43516054606、43538214072）、各作业的 mtest 存储与发布
v2026.11.12.1 的运行。

| 对象 | 测量 |
|---|---|
| PR 全量 CI | 墙钟 67–85 分钟，runner 时间 503–571 分钟，61–77 个作业 |
| macOS 作业 | 17 个、186 runner 分钟；排队合计 201 分钟，单个作业最长 36 分钟（其他主机单个最长 4.7 分钟） |
| Linux 构建作业（43538214072） | 共 35.9 分钟：构建 16.4，gate（`mcpp test e2e/basics`）18.6；全部 Linux 作业等待它 |
| Linux 单元作业（43516054606） | 45 分钟上限内只运行了 65 个程序，其工作量 3970 s；`cli/signals` 3 个用例 433 s |
| macOS 单元作业（macos-15） | 1931 s = 构建 398 + 程序 331 + 运行 1196；运行的工作量 3099 s（3 核）；`openkal/trees` 956 一个用例 806 s，在运行中途才开始 |
| Windows 单元作业 | 运行 487 s（工作量 1817 s，4 个工作线程），作业 19 分钟中其余主要是测试的构建 |
| 发布 v2026.11.12.1 | 109 分钟，各作业串行：canaries 48 → Linux 构建 27（创建 release）→ aarch64 29 / macOS 14 / Windows 8 → 封存与发布 5 |

`mcpp test` 在本文之前按发现顺序运行，部分按用例数排序；不读历史、不分片、不能只重跑失败部分。
一次失败的重跑重复整个 45 分钟的作业。

## 2. 关键路径

1. **Linux：** 构建 16 分钟 + gate 18.6 分钟，之后单元作业再次构建测试（约 15 分钟）并在一个作业内运行全部
   C++ e2e 程序，超过 45 分钟。
2. **macOS：** runner 并发不足，排队主导；两个镜像各运行完整的单元作业。
3. **发布：** canaries 门住构建，三个平台构建只因 Linux 作业创建 release 而等待它。

## 3. 决定与实现

| 编号 | 决定 | 实现 |
|---|---|---|
| P1 | `mcpp test` 按存储测得的用时调度 | `mcpp.build.test_schedule`：读取存储最近 30 次运行，按用时从长到短开始；`[test] split = "auto"` / `--split auto` 把测得长于份额的部分按用例分组（SPEC-017 O10） |
| P2 | 构建一次，分片运行 | `--shard I/N`（最长处理时间优先分配）；`ci-unit.yml` 的 archive 阶段由 bootstrap mcpp 构建全部测试并附上 main 的历史，与 build.yml 并行；run 阶段四个分片以本提交的 mcpp 运行 `--archive --shard`。archive 阶段在每个集合上都构建全部测试：bootstrap mcpp 的按变更选择没有 P3 的规则 |
| P3 | 按变更选择覆盖 C++ e2e | 运行本包程序的测试（其文件写出 `MCPP_BIN_`）受本包任何非测试源文件的变更影响。此前 `src/` 的变更选不中任何 e2e 程序（`--changed src/build/execute.cppm`：1 个测试 → 26 个） |
| P4 | macOS 预算 | `xcode-27` 在 PR 上运行 SDK 相关子集（`unit`、`e2e/basics`、`e2e/build*`、`e2e/macos`、`e2e/modules`、`e2e/runtime`、`e2e/toolchain`），main 与 `ci:full` 全量 |
| P5 | 发布并行 | 四个构建同时开始，产物留作 artifact，`create-release` 一次创建；canaries 只在手动发布带 `canaries` 时运行，与构建并行，门住 release 的创建 |
| P6 | 只重跑失败部分 | `--failed`；`mcpp-unit` action 在重跑的作业中取上一次尝试的存储并以 `--failed` 运行 |
| P7 | 合入前的 bench | `bench.yml`：取该提交成功 CI 运行的 Linux 二进制，只运行 `bench/` |
| — | gate 离开 Linux 构建 | gate 与 `mcpp-tests-linux-x86_64` 移入 archive 阶段；MinGW 交叉用例（198、240、248、923、1058）移入 run 阶段的 `cases` 作业 |

分片的一致性：每个分片从同一个 artifact 取得同一份历史；`--shard` 不把本组（`GITHUB_RUN_ID`）的分片运行读作
历史，因此在同一存储上先后运行的分片也得到相同的分配。本地验证：`e2e/test` 与 `e2e/run` 的 25 个测试分三片，
每个恰好运行一次，三片 42 / 36 / 40 s。

## 4. 未采纳与测量

- **P3 的覆盖图收窄。** 最新的覆盖图（e2e-coverage 43337527734，691 个用例）中，`src/` 文件被执行它的用例覆盖的比例
  中位数为 99%，四分位 95%–99%。按覆盖图为 C++ e2e 收窄，几乎任何源文件的变更仍选中全部用例，不实施。
- **P8 缓存测试构建目录。** 在已构建的树上 touch 全部源文件（等同一次新的 checkout）后，`mcpp build` 重新执行了
  487 个边中的 487 个：恢复的 `target/` 不被复用；另外，以本提交的 mcpp 构建测试时引擎指纹每次都变。不实施。
- **iOS 与小作业合并。** 三个 iOS 作业分属不同 runner（其中一个为 Intel Mac），不能合并；跳过的作业不占 runner。

## 5. 预期与核验

| 路径 | 之前 | 之后（估计，待 CI 核验） |
|---|---|---|
| Linux 测试 | 构建 35.9 分钟后单作业 > 45 分钟 | archive 与构建并行约 20 分钟，四个分片约 10–15 分钟 |
| macOS 单元（macos-15） | 运行 1196 s | 最长者先开始：约 max(806, 3099/3) ≈ 1033 s |
| 一次失败的重跑 | 整个作业 | 只运行失败的用例 |
| 发布 | 109 分钟 | 不带 canaries 约 35 分钟（aarch64 29 + 创建与封存 + 发布） |

第 5 节的估计以合入前 CI 与下一次发布的实测核验，并补入本节。

### 5.1 首次运行（43544563640）

| 对象 | 测量 |
|---|---|
| Linux 构建作业 | 10.4 分钟（之前 35.9） |
| archive 阶段 | 22.0 分钟，与构建并行：gate 19.8 分钟（构建 617 s），归档 26 个测试，取得 main 的 7 次运行 |
| 四个分片 | 02:35–02:39 开始；1/4 只分得 1 个测试（1.5 分钟），其余 18–26 分钟 |
| Linux 测试路径 | 自运行开始 57 分钟走完（之前：构建 35.9 分钟后单作业在 45 分钟上限处被终止） |

分片不均的原因：main 最近一次成功运行的存储来自旧布局，只测得单元程序；C++ e2e 程序没有测量，取了单元
程序的平均值，估计之和远小于实际，最长的单元程序独占一片。修正后，没有测量的测试取其最近目录中已测测试
的平均值，各级目录都没有时取已测测试中最长者，未见过的测试因而按数量分散（SPEC-017 O10 第 1 条）。
本 PR 合入后 main 的四个分片存储覆盖全部测试，此后的估计都来自测量。

分片首次在 Linux 上把全部 C++ e2e 程序跑完，暴露了此前被 45 分钟上限截断而从未完成的失败：

- **共享 registry 被案例 home 改写。** registry 链接到共享 home 的案例 home 把自己的 store 写进共享
  registry 的配置，之后共享 home 的安装落入已删除的案例目录（`install path missing after fetch`，
  openkal/sets 963、openkal/layers 994）。
- **归档运行把 `MCPP_RUNTIME_FILES` 交给每个测试**（run/contract 672）。
- **MCPP_BOOT 是 xlings shim。** shim 按工作目录的项目解析版本，在测试自己的目录中找不到（workspace/inheritance 772）。
- **测试读取共享状态。** toolchain/layers 663 取共享缓存中最新的 std 元数据，读到并行程序的；cdb/root 781、
  build-program/hosttools 831、runtime/binding 200 与 cache/fastpath 断言构建被重放，而并行安装改写运行环境
  描述时构建按设计重新规划。
- **targets/graph 778** 从 mcpp 的输出中找链接行；冷构建的输出超过运行 mcpp 的 e2e 进程保留的 1 MiB
  标准错误（mtest `os::process::bounds::err_limit`），链接行被截去。
- **openkal/trees** 在四个工作线程旁超过 1200 s。
- **交叉构建与发布取错二进制。** `find -name mcpp` 可能取到自举构建保留的引擎副本 `engine/epoch-N/mcpp`。
- **Windows 写入冲突。** xlings 原地写 SubOS 描述，另一进程打开该文件时保存被拒（images/1033
  `Failed to write file`）；`config.toml` 的替换在读者频繁打开时两秒内未成功（unit ConfigValueIsReplacedWhole）。
- **无 Visual Studio 的 Windows 上运行 MSVC ABI 归档。** 归档运行仍编译项目的构建程序（mingw/182）。

### 5.2 收敛（负责人 2026-11-14 确认）

上述问题不逐点修补，按其所属的机制收敛：

| 编号 | 机制 | 实现 |
|---|---|---|
| A | registry 的归属 | `GlobalConfig::registryBorrowed` 在加载配置时判定一次（`[xlings] home` 指向别处，或 `registry` 经链接到达，且已有配置）；索引位置、`[index.repos]` 表的位置与上游的每个写入者都只问它 |
| B | 准备的意图 | `PrepareIntent {Build, Describe, Configure, RunBuilt}` 取代 `plan_only`、`configuration_only`、`runs_built_tests` 三个开关；各阶段读 `describes()`、`configures()`、`compiles()`；构建程序与宿主工具的阶段在驱动处按 `compiles()` 一次跳过 |
| C | 交给 runner 的内容 | `runner_handoff_env`：文件清单与运行契约（`MCPP_RUN_*`），构建的测试运行与归档运行共用；直接运行的程序两者都不得到；归档中的文件按其相对程序的位置命名 |
| D | provider 状态的写入次序 | `mcpp::home::Turn`（跨进程与线程、同一线程可重入）；锁位于 provider 状态的真实目录（`Provider::state_dir`，解析链接），安装、增删索引仓库、选择运行时、初始化沙箱都取它；读者在 Windows 上以可删除共享打开文件。xlings 已以暂存文件加 `MoveFileExW` 替换，Windows 拒绝替换他人未以可删除共享打开的文件，且只试一次；重试与 POSIX 语义的替换见 openxlings/xlings#715，上游实现后本次序不需改动 |
| E | CI 与测试的约定 | `install_pinned_mcpp.sh` 导出 payload 中的发布二进制为 `MCPP_BOOT`；`.github/tools/built_artifact.sh` 取构建报告（`--message-format json`）中的产物路径，取代对 `target/` 的搜索；测试以 mtest `timeout{}` 声明自身时长，运行器取其与 `--timeout` 的较大者（trees 40 分钟、sets 30 分钟），全局时限不变；断言重放的测试持有 `machine`；778 读取构建图而非输出 |

快速路径以共享 SubOS 描述的修改时间判定运行环境未变，是保守而正确的：并行安装可能改变程序加载时解析到的
库。按内容判定需要知道程序用到描述中的哪些条目，不在本次范围。

### 5.3 第二次运行（43553119208）

| 对象 | 测量 |
|---|---|
| Linux 构建作业 | 17.1 分钟（runner 间差异；上一次 10.4） |
| archive 阶段 | 38.5 分钟：gate 2103 s（构建 1118 s），归档 129 s；gate 先以 bootstrap 构建本包程序 mcpp，其工作与构建作业重复 |
| 四个分片 | 8.3、16.1、22.5、24.3 分钟；最长的单测为 openkal/sets 1242 s、openkal/trees 1023 s |
| 分片工作量 | 325 条记录合计 10770 s |

### 5.4 被测程序只构建一次（负责人 2026-11-14 要求完成）

第二次运行中 archive 阶段的 38.5 分钟里，gate 先以 bootstrap 构建本包程序 mcpp（1118 s），archive 再把这个构建
作为 `MCPP_BIN_MCPP` 带给分片。于是该程序被构建两次（构建作业一次，archive 阶段一次），分片测试的是 archive
阶段的构建，而不是构建作业产出、其余作业都使用的二进制（规则 R1）。macOS 与 Windows 的单元作业同样在
`mcpp test` 的程序阶段以本提交的 mcpp 再构建一次（macos-15 上 331 s）。

| 编号 | 机制 | 实现 |
|---|---|---|
| F | 在别处构建的程序 | `mcpp test --no-programs`：本包程序只为名字而规划，不构建；测试从环境的 `MCPP_BIN_<NAME>` 取得各程序，环境未指定者在任何测试运行前被拒绝，不会从 `PATH` 取到同名程序。与 `--no-run --archive` 同用时归档不携带程序，清单以 `named_programs` 列出其变量，归档的运行从自身环境取得；归档的运行不接受该选项；环境对每个名字只指定一个程序，多腿被拒绝。`MCPP_BIN_<NAME>` 的拼写收敛为 `program_env_key` |
| G | CI 中被测的 mcpp | archive 阶段以 `--no-programs` 只构建测试；gate 移到 run 阶段的第一个分片，从归档运行；分片、gate 与 MinGW 用例作业以 `MCPP_BIN_MCPP` 指定构建作业的二进制；`mcpp-unit` 在构建模式下传 `--no-programs`，macOS 与 Windows 单元作业不再构建 mcpp。archive 阶段的 bootstrap 认识该选项时才使用它：固定版本升至 2026.11.13.1 后生效，无需修改工作流；此前归档携带 bootstrap 对本提交源码的构建，与本节之前相同 |
| H | 重放构建的产物报告 | 工作区的快速路径与单包的一样重述所重放构建的记录：`--message-format json` 给出 `artifact` 事件与信封中的 `data`。此前它不写记录，`built_artifact.sh` 在已构建的工作区上取不到产物 |

`mtest::e2e::run` 丢弃 `os::process::run` 报告的截断标志（标准错误保留 1 MiB），见 mcpp-community/mtest#22；
graph 778 已改为读取构建图，不依赖输出的完整性。

本地核验：`mcpp test e2e/basics --no-run --no-programs --archive` 的程序阶段 0.96 s（只规划），归档不含程序；
未指定 `MCPP_BIN_MCPP` 的运行以退出码 2 被拒绝；指定 `/bin/false` 时 basics 失败，说明测试使用的是所指定的程序。
用例 1085（重放构建报告产物，包与工作区）与 1086（`--no-programs` 的写出、拒绝、运行与本地运行）通过。

### 5.5 从缓存恢复的 store 中无法启动的工具链（mtest 0.8.4 发布）

**现象。** mtest v0.8.4 的发布运行（43556169215）在 web 行失败：`source imports std but toolchain 'clang 6.0.9
(wasm32-emscripten)' provides no std module source`。同一提交在 PR 运行（43554279064）中通过。

**与此前版本的对比。** 0.8.3 及以前没有 web 行；0.8.4 首次以 emsdk 构建。三次运行的区别只在缓存：PR 运行恢复的
store 不含 emsdk，现场安装后通过；main 运行同样现场安装、通过，并保存了含 emsdk 的 store（键 `722042…`）；tag 运行与
mtest#23 的 PR 运行恢复这份 store，emsdk 被视为已就位，均失败。失败与 mtest 的代码无关，是任何恢复含 emsdk 的 store
的运行都会遇到的。在 runner 上诊断：`bin/clang` 存在，执行时 `ENOENT`；其 `PT_INTERP` 为
`<home>/abi/linux-x86_64-glibc/ld-linux-x86-64.so.2`，`RUNPATH` 同样经过 `<home>/abi`，而 `<home>/abi` 不在缓存路径中。

**是否为引入的缺陷。** 是，但不是本 PR 引入，也不在 mtest：
- 本机 2026-09-01 安装的 emsdk（当时 mcpp 自行修补 payload），其 clang 的 `PT_INTERP` 直接指向 store 中的 glibc
  payload，store 是自包含的；自运行库钉版本由提供者绑定（设计 2026-10-16 §7.1–7.2）起，xlings 2026.10.26.1 安装的
  同一 payload 指向 home 的链接 `<home>/abi`。store 因此不再自包含，链接属于 home 的状态。
- mcpp 在取用工具链时确保运行库存在并保持钉版本（`ensure_declared_runtime` → `pin_runtime`，链接缺失则重建），
  但“哪些 payload 运行在运行库上”由一张名单回答（`runtime_payload_kind`：gcc、llvm）。emsdk 的 clang 同样运行在
  该运行库上，却不在名单中；新安装时 xlings 安装其 glibc 依赖顺带建立链接，掩盖了这一点，从缓存恢复时便暴露。
  缺陷自 store 改为经 `<home>/abi` 链接时即潜伏，emsdk 首次从缓存恢复时显现。
- `em++ --version` 由 emcc 自己回答而不启动 clang，探测因此“成功”；随后两个 std 探测失败的原因被丢弃，报告只剩
  “没有 std 模块”，指向了错误的方向。

**设计层面的解决。**

| 编号 | 机制 | 实现 |
|---|---|---|
| I | 运行在运行库上的 payload 由事实回答 | `runtime_payload_kind(pkg, root)`：gcc、llvm 在安装前按配方回答；payload 就位后按其程序回答（`starts_through_its_home`：`bin/` 中第一个指名加载器的 ELF，其 `PT_INTERP` 是否位于该 payload 自身的提供者 home 内，home 由提供者的 layout 给出，两侧按文件系统解析）。不列名字，不写系统加载器路径；Mach-O 与 PE 不指名加载器，答案为否，各系统形式相同；新的工具链族无需登记。各取用点把 payload 目录传入 |
| J | 探测失败如实报告 | 带错误输出的探测失败时，错误信息附上其输出的最后一行；clang 的 std 探测未得到回答时，工具链记下原因（`stdModuleUnanswered`），`import std` 的拒绝信息随之给出“编译器未回答：…” |
| K | 提供者状态整体缓存 | mtest 的 CI 把 `data/xpkgs` 与 `abi` 一并缓存（mtest 0.8.5）；mcpp 自己的 CI 缓存整个 home，不受影响。缓存路径改变后旧的缓存条目不再被恢复 |
| L | 不依赖解释器的产物查询 | hermetic 容器中没有 python，`built_artifact.sh` 改为以 grep 与 sed（`-E`，GNU 与 BSD 相同）读取 `artifact` 事件：每行一个产物，其对象只有一个 `path` 键，不依赖键序 |

I 使从缓存恢复的 emsdk 在 mcpp 2026.11.13.1 起自动重建链接；K 使 mtest 在其固定的 mcpp 版本上即可工作。二者互不依赖。

### 5.6 第三次运行的 Windows 作业（43568062909）

| 失败 | 原因 | 处理 |
|---|---|---|
| unit RuntimePayload/APayloadStartsThroughItsHome | 用例在每个宿主上期望 `elf`；Windows 上 `runtime_payload_kind` 按设计为空（I：PE 不指名加载器，不钉运行库） | 期望随宿主，Windows 上为空；`starts_through_its_home` 的判定在各宿主上相同 |
| unit Config/ConfigValueIsReplacedWhole（本分支五次完成的 Windows 运行中四次失败） | 写者以重命名替换文件期间，Windows 对读者的打开与对该名字的查询都给出拒绝（被替换的副本处于待删除状态）。`read_whole` 以 `exists` 的失败判定文件不存在，在重命名期间放弃，读者得到空内容。5.1 记录的写者一侧（两秒内未替换成功）已由写者的重试解决，剩下的是读者一侧 | `read_whole` 以打开本身的错误判断：Windows 上除“不存在”以外的拒绝在两秒内重试；其他系统的重命名立即替换名字，读取失败即为最终结果 |
| e2e run/contract 1024 | 用例断言快速路径重放，却不持有 machine；并行程序向共享 home 安装、改变运行环境描述时，完整路径按设计重新规划（E 的约定）。其上方的注释称其写 config.toml，与内容不符 | 移入 contract-machine 套件，注释改为其持有 machine 的原因 |
| e2e openkal/sets 964 B（Linux 分片 2） | 本 PR 引入（5.2 B）。`RunBuilt` 不再供给宿主工具，归档的运行因此既找不到平台链接用的 `okld`，也找不到 runner `okrun`；此前的通过依赖共享 home 中别处留下的程序。本机可稳定复现：`runner 'okrun' for 'x86_64-openkal' was not found` | 两处按意图回答。不链接的准备（`compiles()` 为假或只描述）不要求链接与封装程序：`linksSomething` 一处计算，平台工具缺失与 ld.lld 缺失两个拒绝共用。平台声明为工具、本次准备未定位的 runner，取平台为本机架构构建的 runner 程序（`pack::runner_program`），与其他架构的 runner 同一机制。交给 runner 的文件清单与闭包记录写在归档的运行目录中、所描述的程序旁，不写入检出目录的输出树（用例 B 断言该树中没有测试的产物） |

**macOS canary（cxx aarch64-macos test）。** 自 canary 加入起，该格在 macOS runner 上每次都失败（9 次），矩阵所写的 `ok`
未经测量。测试程序经 mtest 依赖 mcpplibs.os → openkal-macos。openkal-macos 的构建程序在 `MCPP_TARGET_REQUESTED`
非空时把 `port/`（只列三个名字的 `libSystem.tbd`）加入链接搜索路径，依据是 mcpp 在构建程序契约中的陈述：在 Mac 上
`--target aarch64-macos` 时 mcpp 不把系统 SDK 放上链接（`build_program.cppm`）。而 mcpp 的链接参数在每个 macOS 链接上
都给出 `-isysroot <SDK>`（`flags.cppm`，2026-08-25 起在图供给 C 库时也保留）。`-L` 先于 SDK 被搜索，存根遮蔽了 SDK 的完整
`libSystem`，`strlen` 等因此未定义。缺陷不由本 PR 引入：mcpp 的两处陈述自 2026-08-25 起互相矛盾，canary 首次测量了这一组合。

解决需要一项跨仓库的契约：mcpp 向构建程序陈述链接所用的 SDK（为空即链接中没有系统的存根），由 `flags.cppm` 的同一判定给出；
openkal-macos 仅在其为空时提供自己的存根，旧版 mcpp 不设该变量时行为不变；同时更正 `build_program.cppm` 的陈述。该契约待负责人确认；
在此之前矩阵按测量记为 `fails` 并注明原因，修复后 canary 报告该格与矩阵不符，矩阵随之改回 `ok`。

**自我审查（本节各提交）。** 独立审查给出以下问题，均已处理：

| 问题 | 处理 |
|---|---|
| gate 在分片 1 中运行，其记录进入运行存储：作为最新一次运行，它改变分片 1 的测试时长，使分片 1 的分配与其他分片不同（有的测试运行两次、有的不运行）；分片 1 重试时，`--failed` 读到的是通过的 gate 而不是失败的那次尝试 | gate 先于历史运行；取历史前清空本作业的存储，gate 的记录不保留 |
| runner 的回退对所有准备生效，正常的 `mcpp run`/`test` 在未找到平台工具时也会构建 carrier | `BuildContext::intent` 记录准备的意图；回退只在 `RunBuilt` 下发生，其余准备仍报告 runner 未找到 |
| 写有 `named_programs` 的归档仍为 schema 1，只读 schema 1 的旧 mcpp 会在没有被测程序的情况下运行其测试 | 此类归档写为 schema 2，读取接受 1 与 2；其余归档仍为 schema 1 |
| 归档的运行在任何腿运行前要求全部所列程序，另一台机器运行的腿也被要求 | 在腿确定于本机运行后才向环境要求该腿的程序 |
| e2e-cases 以 MSYS 形式导出 `MCPP_BIN_MCPP` | 与 mcpp-unit 相同，以 `cygpath -w` 给出系统自身的拼写 |
| MSVC ABI 的 clang 也运行 libc++ 探测，可能记下不相干的“未回答” | 不对 MSVC ABI 的驱动提出该问题，其 std 模块按 STL 查找 |
