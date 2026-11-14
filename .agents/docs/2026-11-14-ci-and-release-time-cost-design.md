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
| D | provider 状态的写入次序 | `mcpp::home::Turn`（跨进程与线程、同一线程可重入）；锁位于 provider 状态的真实目录（`Provider::state_dir`，解析链接），安装、增删索引仓库、选择运行时、初始化沙箱都取它；读者在 Windows 上以可删除共享打开文件。xlings 侧的原子写入另提 issue，上游实现后本次序仍成立 |
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
