---
subject: ecosystem
status: active
---

# 生态顶层方案：#885 收尾之后的工作流、原则与依赖

**日期：** 2026-11-08
**状态：** 第六版，实施中（T1–T5、U1 已确认；第 8 节为实施计划，第 9 节为实施记录）
**范围：** mcpp、mcpplibs.os、openkal、mcpp-plugins、mcppls、mcppls-clangd、mcpp-ide、mcpp-index、xim-pkgindex、mtest

本文是生态层面的顶层方案。它汇总三份设计与三个新 issue，给出贯穿各仓库的原则、全部工作流及其依赖、尚未收敛的事项，并从全局与生态的视角审查。各项的细节以所引设计为准。

| 文档 | 内容 |
|---|---|
| [2026-11-07-ecosystem-885-closeout-design.md](2026-11-07-ecosystem-885-closeout-design.md) | #885 收尾：D1–D20（mcpp 2026.11.7.1/.2、openkal 级联、IDE、mcppls、插件） |
| [2026-11-08-ecosystem-885-supplement-design.md](2026-11-08-ecosystem-885-supplement-design.md) | 补充：S1 目标角色、S2/S3 平台判断与映像中的平台事实、S4 系统接口、S5 依赖书写、S6 对比运行、S7 测试协议 |
| [mtest .agents/docs/2026-11-08-mtest-design.md](https://github.com/mcpp-community/mtest/blob/main/.agents/docs/2026-11-08-mtest-design.md) | 通用测试系统 mtest |
| mcpp#903 | 锁文件的 `targets` 随命令变化 |
| mcpp#904 | mcpp 本体的依赖书写 |
| mcpp#905 | GCC 16 模块：内联函数体经由 mcpplibs.os 时导入方失败（upstream-bug，已有规避） |
| mcpp#874 | 工作区的 `target/` 占用与 `mcpp test --workspace` 的重复编译（开放；主体已在 2026.10.31.3 实现，待在报告者的工作区验证） |

## 1. 贯穿生态的原则

| 编号 | 原则 | 出处 |
|---|---|---|
| P1 | **核心只定契约，实现在包中。** 构建插件、平台、测试协议皆如此：mcpp 给出规范与运行器一侧，具体实现属于包 | 既有原则；S7、mtest §2 |
| P2 | **两种角色，不分等级。** 生态应用以 ok 映像为唯一发布形态；原生目标是完整支持的构建目标 | S1 |
| P3 | **系统经由模块。** 源文件与测试不包含系统头文件、不含预处理平台分支；操作系统经由 mcpplibs.os，C 库经由 `std` 与 mcpplibs.mwrap.libc；原始 POSIX/Win32 不进入 os | S4 |
| P4 | **平台判断分两种。** `is_<平台>` 表示原生构建（编译期）；`can_run_on` 与 `host()` 表示运行平台（编译期能否、运行时是否）；发布映像的应用在自身的检查中禁止 `is_<平台>` | S2、S3 |
| P5 | **平台事实在 os 中，openkal 不收录产品特性。** 映像中的平台事实由 os 经可移植通道回答；热路径确有需要时以可选接口 `openkal.host` 扩展 | S3、附录 A.3 |
| P6 | **依赖一张表、身份键、精确版本、一种拼写** | S5 |
| P7 | **政策由工具执行。** 测量的有效性、能否重复、能否重试，由工具依声明判定，不依赖记忆 | S6；mtest §4.4 |
| P8 | **面向 Agent 的输出。** 结构化、带 Schema 与版本、简短、给出复现命令 | mtest §6 |
| P9 | **自身是自身的用户。** mtest 以自身测试自身；生态的应用以映像测试，与发布物一致 | mtest §9；D18 |
| P10 | **每个仓库每个版本一个 PR；确定的事项不拆到以后；编译器缺陷以 mcpp 的 upstream-bug issue 记录并在实现中规避** | 既有原则 |
| P11 | **底层与基础设施的变动符合规范，并经严格审查。** openkal 家族、openkal-platform、mcpplibs.os、mwrap 包装、okrun 与 okld 的每一处变动，对应其规范条款（openkal SPEC、mcpp SPEC），说明兼容性，经下游使用者的候选验证；mtest 落地后，规范一致性套件、接口面变化检测与审查记录成为它们的发布门禁 | mtest §3.2 |
| P12 | **一个仓库多个组件时，版本按系列表达，CI 按组件触发。** `X.Y` 为系列，由服务端（或后端）与全部插件共享；`Z` 为各组件自己的补丁号。改动所属的组件由一张声明的表判定，只运行该组件及依赖它的组件的检查；未声明的路径按全部组件处理 | W6；mcpp-ide 1.2.0 的版本规则 |

## 2. 工作流

### W1 #885 本轮（进行中）

| 仓库 | 版本 / PR | 状态 | 阻塞 |
|---|---|---|---|
| mcpp | 2026.11.7.2 | 已发布、镜像、入索引 | — |
| mcppls-clangd | 0.0.21（#18） | PR CI 通过；对比运行 `42395068307` 验证 win32 原生 zlib（`zs.lib`）；映像改用 compat.zlib-ng（本地提交，待索引发布后推送） | W5：compat.zlib-ng 2.3.3 已合入索引（mcpp-index#727），待发布 |
| mcppls | 0.0.21（#79） | #80 修复、头文件清零、过渡日志、依赖改写、mcpp 2026.11.7.2；CI 除 ux-xlings 外通过（U4-cold 冷阶段仅 2 轮，引擎占比 0.88，按无效测量重跑） | 引擎锁需 clangd 0.0.21 发布；W6 的采用时机（T5） |
| mcpp-ide | 1.2.0（#5） | 测试模块、`check-sources`、CLion 公开 API；CI 除 CLion 2026.2.4.1 外通过 | U3：窗口在动作后为 `visible=false`、所选内容为 Module Graph；测试已改为记录窗口可见性的时间线与窗口类型（`d5ced7a`） |
| mcpp-plugins | 0.21.0（#52） | S4 完成（`8d0afd7`、`6305d89`、`e77b920`）；本地 52 个用例与各消费者通过；CI 全部通过 | — |
| mcpp-index | — | #727（compat.zlib-ng 2.3.3）已合入；其余待 plugins 0.21.0 发布后的更新 | plugins 发布 |
| xim-pkgindex | — | 待 mcppls、mcpp-ide 发布后的 bump | 各自发布 |

收尾：生态级全局审查 → 各 PR squash 合入 → 发布与 GitCode 镜像 → 索引 → xlings 全生态验证 → 以 speak-agent 关闭 issue（#896、#897、#839、#840、#843、#803、mcppls-clangd#17、#887、#885 总账）与综合汇报。

**xlings 全生态验证（T2 已确认）。** 在一个新建的临时目录中进行，与本机的全局环境隔离：
- 环境：`HOME`、`XLINGS_HOME`、`MCPP_HOME` 与 `XDG_DATA_HOME`、`XDG_CONFIG_HOME`、`XDG_CACHE_HOME`、`XDG_STATE_HOME` 指向该目录之下；`env -u` 去除会话中的 `XLINGS_PROJECT_DIR`、`XLINGS_ACTIVE_SUBOS`、`XLINGS_SUBOS_MODE`、`XLINGS_SUBOS_LIB`、`XLINGS_BIN`、`XLINGS_SHIM_DEPTH`；工作目录为中性目录；不使用 shim。
- 步骤：`xlings update`；不带版本安装 `mcpp`、`mcppls`、`mcpp-ide`、`mcpp-ide-nvim`（必须解析到新版本，GLOBAL 与 CN 两个镜像各一次）；以安装的 mcpp 新建项目并 `build`、`run`、`run --print-launch`（第二次走快路径）、`test`、`--target x86_64-openkal` 的构建与运行；mcppls 的 `--version` 与一次 `diagnostics`；mcpp-ide 后端以 okrun 启动并完成初始化握手；nvim 插件在无界面模式下载入。
- 记录：每一步的版本、耗时与结果写入一份报告，附在 #885 的总账中。

### W2 mcpp 下一版本

| 项 | 来源 |
|---|---|
| 锁文件 `targets` 只随所规划的配置变化 | #903 |
| mcpp 本体依赖改写；带引号的点分键给出改写提示；`examples/02-with-deps`；源文件检查（头文件、预处理分支） | #904、S4、S5 |
| 目标角色写入 docs/24 与 docs/35；openkal 两行按 tier 定义评估为 `verified`。评估所见：mcpp 自身的 CI 只在 Linux 上运行映像（942、960、961、964 声明 `llvm`，而只有 Linux 分片提供它），不满足 docs/21 对 `verified` 的定义。为此新增测试 1033（以平台的全部层构建本机架构的映像并运行、测试，另一架构只构建），并由 `tests/e2e/host-requirements.tsv` 与 `check_e2e_coverage.py` 要求它在 Linux、macOS、Windows 上通过 | S1 |
| 测试协议的运行器一侧；`[test]` 的协议声明；`mcpp.core` 的上下文注入；受影响选择所需的构建图查询；`mcpp new` 默认使用 mtest | S7、mtest §11 |
| GCC 16 模块缺陷的跟踪与精简复现 | #905 |
| 构建在项目根目录写下 `.mcpp/compiler-runs/<hash>/`（编译生成的锁，`src/build/schedule/detach_codegen.cppm`，有意置于 `target/` 之外以免被 clean 删除），而该目录不被版本控制忽略：os 0.4.0 的 PR 曾误提交其中的锁文件。mcpp 创建 `.mcpp/` 时在其中写入内容为 `*` 的 `.gitignore`，使目录自我忽略 | 本轮发现 |
| 工作区的空间与重用：在报告者的工作区（Sunrisepeak/mui，112 个成员）以当前 mcpp 复测，按 issue 的表格给出各区域大小、一次 `mcpp test --workspace` 中重复构建的边数与时间、宿主工具每个键的变体数；与 issue 的期望逐条对照，未实现的部分在本版本实现：`[workspace] target-budget`（或等价的环境变量）、低空间警告以"上一次构建该配置写入的字节数"为阈值（当前为固定的 2 GiB）、std 模块构建的收集。后者在本机已复现：`~/.mcpp/build-cache/v1/std` 有 159 份构建（4.9 GiB，最早 2026-11-01），`mcpp cache prune --older-than 2d` 与 `cache gc --older-than 2d` 均未收集任何一份（gc 只覆盖包与宿主工具）。复测数据与差距写入 issue 后关闭 | #874 |

### W3 mcpplibs.os 0.4.0

`can_run_on`、`on<P>`（泛型 lambda）、R5 修订（平台模块在能回答的地方定义）、映像中的平台事实实现（`linux::translator`、`windows::elevated`、`windows::on_dev_drive`、`posix::effective_user`；`macos::translated` 恒为 `false`）、文档中的写法选择；os 的 CI 以映像在三个系统上运行这些函数。只增加接口。随后 mcppls 与 mcpp-ide 迁移，Dev Drive 提示与 PRoot 识别在映像中恢复。

**`is_image` 改名为 `is_openkal`（本轮讨论确定）。** `os::is_image` 的含义是"本构建以 openkal 目标构建，产物为 ok 映像"，即 `PLATFORM == platform::openkal`，并不是"内核层为 openkal"（os 的全部构建都在 openkal 之上）。原名有两处不清楚：
- 它是 `is_<平台>` 家族中唯一不以枚举值命名的成员（`is_linux` 对应 `platform::linux`，而 `is_image` 对应 `platform::openkal`），而 mcpp 清单对同一事实写作 `cfg(os = "openkal")`；
- "image" 单独出现时可指容器或磁盘映像。

处理：
- os 0.4.0 以 `is_openkal` 为正式名，定义为 `PLATFORM == platform::openkal`，注释写明"以 openkal 目标构建，即 ok 映像（openkal SPEC clause 12），不是'在 openkal 之上'"；
- `is_image` 保留为 `[[deprecated("use is_openkal")]]` 的别名，现有用户（mcppls `workspace.cpp:708`）得到迁移提示而不中断，在 mcppls 迁移到 os 0.4.0 时改写；
- README、CHANGELOG 与 `checks/discarded` 的两层写法示例同步改名。

### W4 mtest

`mcpp-community/mtest`（包 `mcpplibs.mtest`）：规范、引擎（ok 映像）、C++ 模块原生库、适配、C ABI 报告库（`libmtest.okso`）、宿主集成、前端；以自身测试自身；首批迁移五个项目；目标为 PR CI 墙钟中位数减半、发布 CI 关键路径缩短 30%。

### W5 映像中的 deflate（已确认，实施中）

探针确认了原因。darwin-arm64 上映像相对原生（链接 macOS 带 NEON 的 libz）的热启动处理器时间比值，四对逐一为：以 compat.zlib 构建 1.332、1.236、1.214、1.252（运行 `42369457883`）；不带 zlib 构建 0.973、1.050、1.107、1.088（运行 `42386660489`）。两组不相交，差距远大于 0.03 的离散界，故差距来自分片的解压。

实现为 mcpp-index 的 `compat.zlib-ng` 2.3.3（#727）：zlib 兼容形态，即 CMake 的 `WITH_RUNTIME_CPU_DETECTION=OFF` 构建，各函数在编译期绑定到架构基线保证的实现，aarch64 为 NEON（inflate_fast、adler32、chunkmemset、slide_hash、compare256、longest_match），x86_64 为 SSE2。映像无法询问处理器（openkal 不提供处理器特性），所以只构建基线之内的路径。Armv8 的 CRC32 指令不属于 aarch64 基线，需要整个程序的编译选项或运行时检测，因此不构建；它只影响 gzip 流，LLVM 写的 zlib 流以 adler32 校验。Q6 所说的"CRC"据此限定为不启用。验证：
- 索引 CI 的四条工作区腿（Linux GCC、Linux LLVM、macOS arm64、Windows）通过；
- 本地 `x86_64-openkal` 测试通过；
- `aarch64-openkal` 构建中 `inflate` 调用 `inflate_fast_neon`；
- 测试读取 madler zlib 写出的流。

mcppls-clangd 的映像依赖改为 `compat.zlib-ng = "2.3.3"`，对比运行给出结论后写入 CHANGELOG。

### W6 mcppls 的组件版本与按目录的 CI（T5 已确认：并入本轮；已实现）

**实现记录。**
- mcppls #79：`fd87271` 版本与系列检查（devtools `--series`/`--bump`/`--print --component`；`mcppls.base.series` 与 `initialize` 的拒绝；VS Code、CLion 的启动前检查；Zed 发送 `initializationOptions.plugin`）；`b0f1c18`、`9f51d1b` 发布（资产注明组件，`release check --component/--from`，`release.yml` 的 `component` 输入与组件标签，发布器按标签筛选）；`fcfd472`、`0a4f140` CI（`.github/components.tsv` 的 `@core`/`@flag`，`changes.sh` 与其用例，`server-digest` 标记与借用运行，`invariants` 与 `ci` 汇总作业）。
- mcpp-ide #5：`91b1437`，同一分类脚本，`@core backend`，`backend-digest` 标记，`checks` 与 `ci` 汇总作业。
- 两个仓库的 `main` 均无分支保护规则，`ci` 作为唯一必需检查在启用保护时设置。

**借用的实现选择。** 设计稿写的是以 Actions 缓存保存负载。实现改为借用 main 运行的产物：main 在构建负载之后上传名为 `server-digest-<摘要>` 的标记产物；插件 PR 按名称查询标记，得到该运行的 id，插件作业以 `run-id` 下载其 payload、stage 与 kit（payload action 已有此能力，夜间运行即以此测试 PR 的字节）。这样不复制数据，也不受缓存容量限制。mcppls 的产物保留 7 天；mcpp-ide 的后端产物保留 90 天。

**现状。** mcppls 的服务端与全部插件共用一个版本（`mcpp.toml` 的 `0.0.21`，`version --check` 使 VS Code、CLion、Zed、Claude Code 插件与之相同），每个 PR 都运行整个 CI。#79 的一次运行用时约 3 小时（`42385557959`，15:16–18:18），其中服务端、负载、kit、一致性与 ux 占大部分。CLion 或 Zed 的修复只改动 `editors/<插件>/`，也要重建服务端与负载，并等待全部检查。mcpp-ide 已采用系列版本（README "Versions"），但 CI 同样不按组件区分。

**版本（P12）。** 版本为 `X.Y.Z`：
- `X.Y` 为系列，由服务端与全部插件共享；`Z` 为各组件自己的补丁号。
- 组件为：服务端（`mcppls`：单文件程序、映像、xlings 包），以及插件 VS Code、CLion、Zed、Claude Code。Neovim 插件与 Copilot CLI 的配置片段没有版本，随系列发布。
- 系列发布的标签为 `vX.Y.0`，发布全部组件。组件补丁的标签为 `server-vX.Y.Z`、`vscode-vX.Y.Z`、`clion-vX.Y.Z`、`zed-vX.Y.Z`、`claude-code-vX.Y.Z`，只发布该组件。
- 兼容规则：插件接受同一系列的任何服务端，拒绝其他系列，并在提示中给出两个版本与匹配的安装命令。VS Code 与 CLion 内带负载，只有在用户指定服务端路径时才会出现不匹配；Zed 与 Neovim 在 `PATH` 上找到 `mcppls` 时检查系列。插件补丁内带其系列中最新的服务端补丁，所带服务端的版本记录在包内并显示于状态报告。
- 引擎（mcppls-clangd）保持自己的版本，服务端以精确版本锁定它；服务端补丁可以改变这一锁定。
- 工具：`mcppls-devtools version` 支持 `--series X.Y`（全部组件置为 `X.Y.0`）与 `--bump <组件>`（只增加该组件的 `Z`）；`--check` 检查全部组件同一 `X.Y`、二进制报告的常量与 `mcpp.toml` 一致。各组件的 `Z` 各自递增，所以每个市场（VS Code Marketplace、Open VSX、JetBrains、Zed）看到的版本都只增不减。
- 发布：`packaging/release.manifest.json` 为每项资产注明所属组件。系列发布检查全部资产，组件发布只检查该组件的资产；CHANGELOG 的组件补丁小节写在系列小节之下，并注明组件。
- 迁移：mcppls 的第 N 次发布 `0.0.N` 对应系列 `0.N`。`0.0.21` 以 `0.21.0` 发布，比 0.0.20 大，在各市场中合法；下一系列为 `0.22.0`。

**CI（P12）。**
- **组件表。** 仓库根的一张表声明组件与路径：
  - 服务端：`src/**`、`modules/**`、`mcpp.toml`、`mcpp.lock`、`vendor/**`、`packaging/**`、`conformance/**`、`tests/**`；
  - 各插件：`editors/vscode/**`、`editors/clion/**`、`editors/zed/**`、`editors/nvim/**`，Agent 集成为 `editors/{claude-code,copilot-cli,agents}/**`；
  - 文档：`docs/**`、`*.md`；
  - `.github/**`，以及表中未列出的路径，按全部组件处理。
- **分类。** 第一个作业按表把改动的文件分到组件，输出每个组件是否受影响。分类有自己的单元测试（以文件列表为输入的样例），与 mcpp 的 `classify_changes.py` 相同。
- **依赖传递。** 服务端受影响时运行全部作业，因为插件以新的服务端测试。只有插件受影响时，只运行该插件的构建与端到端测试，以及版本与仓库不变量的检查；只有文档受影响时，只运行文档检查。
- **负载的重用。** 负载以服务端输入的内容摘要为键，存入 Actions 缓存；main 的运行写入，PR 可读。只改插件的 PR 按摘要取回与 main 相同的负载，不重建服务端。缓存未命中则照常构建，结果正确，只是变慢。重用以内容摘要证明负载相同，与分类是否正确无关。
- **全量运行的场合。** 推送到 main、手动触发、夜间与发布运行全部作业，不做分类，使 main 的缓存保持有效、覆盖保持完整。
- **汇总作业。** 一个汇总作业作为分支保护的唯一必需检查：分类要求的作业全部成功、其余作业是因分类而跳过时，汇总作业成功。
- **性能政策。** 政策（P7）不变：被跳过的测量不算通过；发布前的对比运行仍是全量。

**mcpp-ide。** 版本规则已经如此。它的 CI 以同一张表的形式区分后端（`src/**`、`protocol/**`、`spec/**`、`webui/**`、`testsupport/**`、`tests/**`、`mcpp.toml`）与 `mide-plugins/<插件>/**`，并以同样的方式重用后端的映像包。

**审查。**

| 视角 | 所见 | 处理 |
|---|---|---|
| 兼容性 | 市场、xlings 与 mcpp-ide 对版本的依赖 | `0.21.0` 比已发布的全部版本都大；xim 按 SemVer 比较，`mcppls@0.21.0` 是新的最高版本；mcpp-ide 的 CLion 插件以插件 id 依赖 mcppls 的 CLion 插件，不约束版本 |
| 稳定性 | 重用的负载可能与源码不符 | 缓存键为服务端全部输入的内容摘要，加上 `.github/versions.env` 与运行器系统；负载内记录其摘要，测试开始时核对 |
| 稳定性 | 分类漏掉一个依赖，插件 PR 就会在旧服务端上通过 | 未声明的路径按全部组件处理；main 每次推送全量运行，漏判在合入后的第一次运行中暴露；组件表的覆盖由一项检查保证：仓库中的每个文件至少属于一个组件 |
| 用户体验 | 系列不匹配 | 插件给出两个版本与安装命令，不静默降级 |
| 一致性 | mcppls 与 mcpp-ide 的规则 | 同一原则（P12）、同样的标签形式与汇总作业；两者的组件表各自声明 |
| 测试覆盖 | 只改插件的 PR 不运行一致性与 ux | 这两组只测服务端与负载，而负载与 main 相同（由摘要证明）；插件自身的端到端测试照常运行 |

**预期。** 只改插件的 PR 不再构建服务端、负载与 kit，也不运行一致性与 ux，墙钟从约 3 小时降到该插件作业的时间（CLion 约 10 分钟，Zed 与 VS Code 约 20–30 分钟）。实施后以 mtest 的 CI 汇总测量（W4 §10.2）。

### W7 API 的生命周期（讨论确定）

**现状。** C++ 的 `[[deprecated("…")]]` 已使编译器在每个使用处警告，并随模块的 BMI 传递。它缺少四样东西：
- 版本信息：自哪一版弃用、哪一版移除、以什么替代；
- 汇总：依赖内部的使用同样告警，信息噪声大，`-Werror` 下直接失败；
- 机器可读的数据：IDE、Agent 与 `mcpp update` 无从知道升级会破坏什么；
- 更多等级：没有"实验性"一级，也无法表达整个包的弃用。

mcpp 对自身清单键已有完整的同类机制：`kDeprecations` 注册表、docs/51、带替代写法的警告、`mcpp why compat`、`--strict`，以及到期即失败的测试。W7 把它推广到包与包的 API。

**设计**（清单优先、核心最小：核心给出契约，编译器做编译器的事）：
1. **声明级，源码为唯一来源。** 使用标准属性，并约定 mcpp 能识别的消息语法：`[[deprecated("since 0.4.0, removed in 0.6.0: use is_openkal")]]`。编译器照常告警；mcpp 已有的模块扫描读取结构化数据。数据不写两份，因而不会漂移。
2. **包级与模块级，在清单中。**
   - `[package] status = "experimental" | "stable" | "deprecated"`，可带 `replaced-by`；
   - `[lib]` 下的模块可各自声明 `status`；
   - 实验性 API 需要使用方显式允许（`allow-experimental = ["<包>"]`）。
3. **构建时的使用方体验。**
   - 依赖内部的弃用告警不再重复；
   - 使用方自身代码的使用在构建末尾汇总一次（"3 处使用了 mcpplibs.os 的 2 个弃用 API，0.6.0 移除；`mcpp why deprecated`"）；
   - `--strict` 下转为错误；
   - 诊断以编译器的结构化格式读取（GCC JSON、Clang 与 MSVC 的 SARIF），不做文本匹配。
4. **升级时。** `mcpp update` 或锁文件的变化跨过移除版本时，在构建失败之前列出将被移除的 API 与替代。
5. **库作者的发布门禁。**
   - `mcpp publish` 在到达移除版本而声明仍在时拒绝；
   - 移除版本不晚于弃用版本时拒绝；
   - 移除一个此前未弃用的导出声明时拒绝（以模块扫描得到的导出清单与上一个已发布版本比较）；
   - 发布说明可由数据生成。
6. **数据的消费方。** `--message-format json`、`mcpp why deprecated`，以及 mcppls 中的删除线与"改用 X"的快速修复。
7. **mcpp 自身。** `kDeprecations` 改为同一机制的一个使用者，docs/51 的表与检查保持不变。

归属：mcpp 下一版本之后的独立版本，规范写入新 SPEC 与 docs/05、docs/51；W3 的 `is_openkal` 别名是它的第一个使用者。

## 3. 依赖

```
W1:  clangd-0.0.21 ──(engine lock)──▶ mcppls-0.0.21 ──▶ xim bump(mcppls)
       ▲   ▲                                              
       │   └── W5 deflate (若探针确认)                     
       └── win32 zlib 验证                                 
     plugins-0.21.0 ──▶ mcpp-index ──▶ close #887
     mcpp-ide-1.2.0 ──▶ xim bump(mcpp-ide, mcpp-ide-nvim)
     全部发布 ──▶ xlings 全生态验证 ──▶ issue 关闭与汇报

W2 (mcpp next) ──┬─▶ W4 mtest 的 mcpp 集成与 `mcpp new` 模板
                 └─▶ #903/#904 后，生态清单与锁不再需要手工修正
W3 (os 0.4.0) ──▶ mcppls / mcpp-ide 迁移（S2 写法、检查禁止 is_<平台>）
W4 (mtest)   ──▶ 首批五个项目迁移 ──▶ CI 时间目标的测量
```

W2、W3、W4 互不阻塞，可在 W1 的 CI 等待期间并行设计与实现；W4 的 mcpp 集成依赖 W2 的协议运行器。

## 4. 尚未收敛的事项

| 编号 | 事项 | 状态 |
|---|---|---|
| U1 | Q5：`is_<平台>` 与两层判断并存 | 已确认 |
| U2 | darwin 比值超限的原因与修法 | 已确认为 zlib；W5 实施中 |
| U3 | CLion 2026.2 无 JCEF 的说明不可见 | 第一轮诊断：`visible=false selected=Module Graph`；第二轮记录可见性时间线、窗口类型与自动隐藏 |
| U4 | xlings 全生态验证的方式 | 已确认：临时目录中重定向 `HOME` 等变量（W1 的验证步骤） |
| U5 | mcpp-vscode 的归档说明 | 先前约定只在确认后执行 |
| U6 | mcppls-clangd 的 CHANGELOG 记录哪次对比运行 | 以 win32 与 darwin 均通过的那次为准 |
| U8 | `os::is_image` 的语义不明确 | 已确定：改名 `is_openkal`，`is_image` 为弃用别名（W3） |
| U9 | API 生命周期的声明与提示 | 已确定：W7 |
| U7 | mcpplibs/os#19 的 hosted 行 `effective_user()` 为 1000 而 `id -u` 为 1001 | 已修复（`ee1d71e`）：hosted 安排中 C 库为 openkal-musl，其 `geteuid` 为常量 1000、无 `umask`；Linux 与 macOS 改为内核调用（与描述符、信号相同的 `src/posix/kernel.h`）；`unshare -r` 下旧实现为 1000/0，新实现为 0/0；CI 全部通过 |

## 5. 全局与生态视角的审查

| 视角 | 审查所见 | 处理 |
|---|---|---|
| 一致性 | 原则 P3–P6 刚在四个应用仓库落地，mcpp 本体与 os 尚未（mcpp 仍有带引号的键与 `"=0.3.1"`；os 依赖 openkal 用范围） | W2 统一 mcpp 本体；os 的 openkal 范围由平台版本组给出精确性（D12），在 os 文档中写明，不改为精确版本以免与组冲突 |
| 一致性 | 生态有四套测试方式与三套自写测试工具 | W4 收敛为 mtest；迁移在各项目的版本 PR 中整体完成，不保留双套 |
| 架构 | 平台判断、平台事实、系统接口三条规则分属 os、应用检查与插件文档 | 规则的权威文本在 os 文档（P4、P5）与 mcpp docs（P3）；各仓库的检查引用同一文本 |
| 架构 | mtest 的通用性可能被 mcpp 的需求带偏 | 规范与引擎不引用 mcpp 的概念；mcpp 是宿主适配之一 |
| 稳定性 | GCC 16 的模块缺陷已多次出现（#905；早先的 BMI 次序、MSVC 的 path 迭代器） | 规避写入插件开发规则（"经由 os 的函数不内联"）；W4 的 mtest 引擎同样遵循；#905 跟踪上游 |
| 稳定性 | 映像为唯一发布形态，任何映像特有的缺陷都直接到达全部用户 | 测试以映像在三个系统上运行（D18）；对比运行在发布前度量映像与原生的差距（S6）；P7 使政策不被绕过 |
| 性能 | 映像相对原生的冷启动与解压差距（F6） | 上限不放宽；W5 在映像中提供与系统 libz 相当的 deflate |
| 兼容性与无感升级 | os 0.4.0、mcpp 下一版本、mtest 的引入都触及生态的公共写法 | 只增加接口；精确版本使升级显式；L0 使现有测试无须改动；mcpp 的现有 `mcpp test` 行为保持 |
| 跨平台 | Windows 的进程启动开销影响 mtest 的隔离与 os 的可移植通道 | 隔离按声明；平台事实的查询结果缓存、只在调用时发生、移出启动关键路径 |
| 用户体验 | 映像用户在 W3 之前失去 Dev Drive 提示与 PRoot 识别 | mcppls 0.0.21 在日志与文档中说明；W3 后恢复 |
| Agent 可操作性 | 本轮 CI 的观察依赖手工轮询与日志检索 | W4 的 `mtest ci`、`mtest explain` 直接针对此项；在此之前，各 PR 的失败以结构化产物（如 CLion 的诊断文件）保存 |
| 测试覆盖 | 映像中的平台事实、插件的 os 接入、IDE 的无 JCEF 路径都是本轮才被测试覆盖的路径 | os 的映像测试（W3）；插件 CI 的源文件检查与各消费者；CLion 两个版本的产物诊断 |
| 过程 | 本轮出现过违反性能政策的重跑与未确认即发布的上游 issue | 已记录；P7 使前者由工具阻止；后者已有规则（只在 mcpp 内以 upstream-bug 记录） |
| 范围 | W2–W4 的范围大于 #885 | 三者各有独立设计与评审，不并入 W1；W1 只包含已确定的修复与收尾 |

## 6. 第二轮审查（多视角）

| 视角 | 审查所见 | 处理 |
|---|---|---|
| 依赖 | W5 的条件尚未判定：探针若排除 zlib，darwin 的修法未知，W1 的关键路径因此不确定 | 探针结果出来即判定：确认则实施 W5；排除则按冷启动的装载与页错误继续定位（冷启动 `page_faults` 比值 2.74–2.87），不放宽上限，并以新的探针缩小范围 |
| 依赖 | mcppls 0.0.21 依赖 clangd 0.0.21 的引擎锁，clangd 的阻塞会传递给 mcppls 与其 xim bump | 两者的其余部分先完成并通过 CI；锁的更新是最后一个提交 |
| 一致性 | #874 的主体在 2026.10.31.3 已实现，issue 仍开放，生态无人复测 | W2 以报告者的工作区复测并关闭；复测需要 10–25 GB，在 CI 运行器上进行（本机的磁盘余量不足，见下） |
| 资源 | 本机磁盘在本轮中一度只余 4 GB（`/tmp` 100%）。清理了已完成的临时目录（探针、已推送的 mwrap 仓库、研究目录中的构建产物）、各工作树只保留最近一次构建、`cache gc --older-than 2d`（2.6 GiB）与两天前的 std 构建（约 4.3 GiB），回到约 21 GB；其他会话的项目未触及 | 继续遵守"一次一个重任务"；大的复测（#874、对比运行）放在 CI；W4 的 mtest 存储设保留期 |
| 稳定性 | 验证环境若泄漏会话的 shim 变量，会把旧版本误判为索引问题 | W1 的验证步骤明确 `env -u` 的变量与中性工作目录 |
| 用户体验 | 验证只在 Linux 上进行，Windows 与 macOS 的安装路径没有真实覆盖 | 发布 CI 的 `ci-fresh-install` 覆盖三个系统；本机验证补 Linux 的完整链路与两个镜像 |
| 测试覆盖 | 本轮的修复（插件的 os 接入、IDE 的 `check-sources`、mcppls 的 #80）只在各自 CI 中验证 | 全局审查时按仓库核对：每项修复有对应的测试或检查，并在 PR 描述中列出 |
| 过程 | 主工作树曾被一条命令置为分离 HEAD（已恢复到 `main` 并快进） | 只在专用工作树中执行检出类命令 |

## 7. 待审查的问题

- **T4** 已确认：#874 归属本方案（W2），方案确定后与其他未收敛事项一并重新拆分实施计划。
- **T5** 已确认：W6 并入本轮，mcppls #79 以 `0.21.0` 发布，mcpp-ide #5 加入 CI 分类，各一次发布。原问题：W6 的采用时机。建议并入本轮：mcppls #79 以 `0.21.0` 发布（版本规则、devtools、发布清单、插件的系列检查、组件表与 CI 分类、负载重用），mcpp-ide #5 加入同样的 CI 分类。这符合"确定的事项不拆到以后"，代价是两个 PR 再增加一轮 CI。另一做法是作为 mcppls 0.22.0 与 mcpp-ide 1.3.0 的内容，本轮保持 `0.0.21`。

- **T6**（待确认）：W8 测试迁移到 mtest 与 CI 时间。测得：mcpp 一次 PR 74 个作业、627 runner 分钟、墙钟 56 分钟，关键路径为 mcpp 构建（约 13 分钟）之后的 Linux 单元测试（43 分钟；16 核本机 6 分钟，构建占 210 秒），e2e 720 个脚本在 Linux 累计约 94 分钟、分片内串行、分片依提交的耗时表；mcppls 88 个作业、446 runner 分钟、墙钟 92 分钟，关键路径为服务端与负载（约 70 分钟）之后的 stability。方案：e2e 作为 mtest 的 set（脚本即用例，`# requires:` 为标签，独占点为 `resources`，按存储的历史耗时最长优先分片并在分片内并行）；`mcpp test --changed <base>` 以构建图（记录的 `sources` 与传递导入）选单元测试，e2e 以 main 上定期的覆盖映射（用例的 `file:` 产物）选用例，未映射者视为受影响，main 与发布运行全部；测试程序可按目录合成一个程序以减少链接；mcppls 的一致性与 ux 场景为用例、测量声明 `serial:perf`、失败只重跑失败项；mtest 的存储以 main 的产物共享。待决：W8 在本轮发布之后开始还是并入本轮（建议之后，三个仓库各一个 PR）；既有 e2e 的判据是否一次改写为 `mtest_case`（建议只要求新增与改动的）；PR 只运行受影响集与冒烟集；先以探针测分片内并行、合成测试程序与 `target/` 复用的实际收益。

## 8. 实施计划

### 8.1 每个仓库每个版本一个 PR

| 仓库 | 版本 | PR / 分支 | 内容 | 工作流 |
|---|---|---|---|---|
| mcppls-clangd | 0.0.21 | #18 | 现有内容；win32 zlib；W5（若探针确认）；CHANGELOG 记录对比运行 | W1、W5 |
| mcppls | 0.0.21 | #79 | 现有内容；引擎锁为 clangd 0.0.21（最后一个提交） | W1 |
| mcpp-ide | 1.2.0 | #5 | 现有内容；CLion 无 JCEF 测试 | W1 |
| mcpp-plugins | 0.21.0 | #52 | 现有内容（CI 已通过） | W1 |
| mcpp-index | — | 一个 PR | plugins 0.21.0；W5 的 deflate 包（若需要）；mcpplibs.os 0.4.0；mcpplibs.mtest 0.1.0 | W1、W3、W4、W5 |
| mcpplibs/os | 0.4.0 | `release/0.4.0` | S2（`can_run_on`、`on<P>`）、S3（R5 修订与映像中的平台事实）、文档、映像测试 | W3 |
| mcpp-community/mtest | 0.1.0 | 新仓库 | 规范 v1、C++ 库（unit、bench、cts、e2e）、引擎（运行、存储、查询、explain、plan、ci、拓扑导出）、C ABI 报告库、GoogleTest 与脚本适配；自身以 mtest 测试；ok 映像发布 | W4 |
| mcpp | 2026.11.8.1 | #906（草稿）`release/2026.11.8.1` | #903、#904（含源文件检查）、S1 文档与 tier 评估、#874 的复测与剩余期望、S7 的协议运行器与 `mcpp.core` 上下文注入与构建图查询、`mcpp new` 默认使用 mtest、三份设计文档 | W2 |
| xim-pkgindex | — | 各发布的 bump PR | mcpp、mcppls、mcpp-ide、mcpp-ide-nvim | 各工作流 |

首批迁移（mcppls、mcpp-ide、mcpp-plugins、mcppls-clangd 迁移到 os 0.4.0 与 mtest）是这些项目各自的下一个版本，依赖 os 0.4.0 与 mtest 0.1.0 的发布；迁移前以 mtest 的 CI 汇总测量正式基线（W4 §10.2）。

### 8.2 依赖与波次

```
波次 1（进行中）  plugins 0.21.0 ─▶ index ─▶ close #887
                 mcpp-ide 1.2.0 ─▶ xim bump
                 clangd 0.0.21 ◀─ W5（条件）◀─ darwin 探针
                    └─▶ mcppls 0.0.21 ─▶ xim bump
波次 2（与波次 1 并行）
                 os 0.4.0 ─────────────┐
                 mtest 0.1.0 ──────────┼─▶ index ─▶ mcpp 2026.11.8.1（`mcpp new` 模板引用已发布的 mtest）
                 mcpp 2026.11.8.1 的其余部分（#903、#904、S1、#874、S7 运行器）可先行实现
波次 3            基线测量 ─▶ 首批迁移（各项目下一版本）─▶ 迁移后测量（M3）
收尾              各波次发布 ─▶ xlings 全生态验证 ─▶ issue 关闭 ─▶ 汇报
```

### 8.3 次序约束

- mtest 0.1.0 以已发布的 mcpp 2026.11.7.2 构建与测试（L0 与其自身引擎），不依赖 mcpp 2026.11.8.1；mcpp 2026.11.8.1 的协议运行器以 mtest 规范 v1 为准，两者在同一波次设计。
- os 0.4.0 只增加接口；mtest 引擎使用其 `can_run_on` 与 `host()`，因此 mtest 0.1.0 依赖 os 0.4.0 的发布。
- 每次发布后：取消 main CI、发布、GitCode 镜像（本地 gtc）、重新触发 main CI、索引或 bump PR 合入、CDN 生效后再进行依赖它的发布。

## 9. 实施记录（第六版）

### 9.1 新的发现与处理

| 发现 | 处理 | 版本 |
|---|---|---|
| Windows 的线程以 x87 双精度（控制字 0x027F）开始，okrun 进入映像时未建立 System V 的浮点初始状态；映像中 openkal-musl 的 `strtod("0.4")` 得到 `0x1.99999999998p-2`，mtest 一致性套件 P6.1 只在 Windows 失败 | SPEC 12.2 增加"浮点状态"一行、12.3 写明 loader 的义务；okrun 在入口、初始化与终结函数和每个上下文中建立该状态，openkal-bind 的调用在其中进行并恢复调用方的状态；`fpstate` 与 `bind` 示例在四个系统上检验 | openkal#63、openkal-image 0.5.3、openkal-platform 0.24.4、okrun 0.5.3（xim） |
| 平台组发布一个新成员后，早于 2026.11.7.1 的 mcpp 按范围取最新成员再以版本组拒绝（os 与 clangd 的 CI 用 2026.11.5.1） | 生态 CI 使用 mcpp 2026.11.7.2（在版本组内解析） | os#19、clangd#18、mcpp-index#730 |
| mcpp 的三个原有缺陷：依赖的 entry 导入 `std` 时 std 模块不构建；有起点的测试仍链接 dev-dependency 之下的 `gtest_main.cc`；共享库形式的 dev-dependency 不链接 | 均在 mcpp 2026.11.8.1 修复（e2e 1037） | #906 |
| 父包与其命名空间下的包在同一张表中（`mcpplibs.mtest` 与 `mcpplibs.mtest.gtest`）只能以引号键写出，而 #904 把带引号的点分键列为弃用 | 唯一写法的引号键不报弃用（e2e 1032、docs/05、docs/51） | #906 |
| mcppls 的日志状态在进程退出时被析构，退出期间写日志的分离线程在 Linux arm64 映像中 SIGSEGV | 日志状态不析构 | mcppls#79 |
| Linux e2e 的耗时表已过时（重命名的测试按中位数计），一个分片超出 22 分钟 | 由各分片报告重新测得三张耗时表；Linux 改为 8 个分片 | #906 |
| `mcpp new` 以 mtest 为 dev-dependency 之后，`mcpp build` 与 `mcpp test` 成为两个配置：mtest 经 mcpplibs.os 到达 openkal 的内核接口，配置因此带上 `__OPENKAL__`；dev-dependency 的包含目录进入包自己源文件的命令。结果是被测试的代码与发布的代码命令不同、两个构建目录各编译一遍、构建之后的编译数据库没有测试条目（e2e 77、783 失败），在 build 与 test 之间切换时重编包自己的源文件 | dev-dependency 只到达测试单元（`testBuild`）；只经 dev 边到达的内核接口只给 dev-only 包与测试单元，配置不变；C 库与 C++ 运行时仍属配置。`mcpp dyndep` 不改写未变的输出，规则的 `restat` 由此生效 | #906（docs/08、e2e 1037 K） |
| 测试计划修剪依赖中定义 `main` 的源文件时覆盖了全部依赖，openkal-musl 的映像入口 `okm_image.c` 声明 `main` 而被去掉，`--target x86_64-openkal` 的测试映像缺少运行时符号（e2e 1033、951） | 只修剪 dev-dependency 及其之下的包 | #906 |
| mcpp-ide 的发布运行 `startup_failure`：ci.yml 的作业请求 `actions: read`，被调用工作流的权限不能超过调用方 | release.yml 的 `ci` 作业授予 `actions: read` | mcpp-ide#6 |
| mcpp-plugins 0.21.0 仍精确依赖 mcpplibs.os 0.3.1，与 mcpp、mcppls、mcpp-ide 的 0.4.0 不一致 | 改为 0.4.0 后发布 | mcpp-plugins#52 |

### 9.2 S7 与 W4 的实现

- **mcpp 2026.11.8.1（#906）**：
  - 测试协议运行器：`mcpp test` 的用例记录、`--case`、`--cases`、`--seed`；未实现协议的程序收不到协议变量；`--no-run` 的记录带程序与源文件；特性 `test-protocol`。
  - 入口键 `test-protocol`、`test-main`、`test-guide`：不定义 `main` 的测试以提供 `test-main` 的入口为起点，由文件的导入在多个提供方之间选择；`gtest_main.cc` 照旧作为测试起点。
  - `mcpp test --guide`：列出测试框架及其指南与仓库，以及用例相关的命令；失败的运行指向它（协议 P2.5）。
  - `mcpp::context`；`mcpp new` 的测试使用 mtest，`--template lib` 生成模块、它导出的分区及分区的实现。
- **mtest 0.1.0**：已发布（mcpp-community/mtest#1）；引擎以 ok 映像的通用包发布（xim `mtest`）；`mcpplibs.mtest`、`.gtest`、`.capi` 进入 mcpp-index；协议增加 P2.5（指南）。
- **mcpp-ide 1.2.0（#5）**：实现协议的程序由 `mcpp test --cases` 列出、由 `--case` 运行，结果取自 mcpp 的记录；mtest 的用例在构建前即由源码中的 `"name"_test` 字面量给出；未实现协议的框架仍按 `spec/test-frameworks.json`。
- **已确认**：mtest 构建插件的语法糖（静态用例清单与清单属性优先，其余能力各自定稿），归入 W8 的 mtest 一项。

### 9.3 W3 的完成

mcpplibs.os 0.4.0 已发布并进入 mcpp-index；mcppls、mcpp-ide、mtest 与 mcpp 迁移到 0.4.0。mcppls 的映像经 `os::on<P>` 询问 Dev Drive 与 PRoot，此前映像对二者均不作判断。

