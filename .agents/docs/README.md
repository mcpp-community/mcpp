# Design records

The reasoning behind changes to mcpp: what was measured, what was decided,
and what a later measurement refuted. A record describes the moment its
change was made and is not edited afterwards, so **nothing here is a
statement about the present**. What mcpp does today is in
[docs/](../../docs/README.md); what is guaranteed is in
[docs/specs/](../../docs/specs/README.md).

**This file is generated** by `.github/tools/gen_agents_index.py` and is
checked in CI. A new record declares front matter:

```yaml
---
subject: heterogeneous            # a short, reused word
status: landed                    # active | landed | superseded | abandoned
superseded_by: 2026-09-07-....md  # when status is superseded
---
```

283 records.

## By subject

Records that declare one. Everything else is listed by date below.

### build-program

- [Four upstream asks from a UI framework: what each one is under mcpp's design, and the combined plan](2026-09-13-four-upstream-asks-from-a-ui-framework.md) — active

### docs

- [The documentation as a book: a chapter-by-chapter design](2026-09-08-the-documentation-as-a-book.md) — active
- [A curriculum for the examples, a reference for the documentation, and a check with a denominator](2026-09-08-examples-curriculum-and-documentation-plan.md) — superseded by [2026-09-08-documentation-architecture-three-trees.md](2026-09-08-documentation-architecture-three-trees.md)
- [Three documentation trees, three audiences, and the rule for citing between them](2026-09-08-documentation-architecture-three-trees.md) — active

### heterogeneous

- [A dlopen surface no closure walks, and a process with two unwinders](2026-09-09-dlopen-surface-and-two-unwinders.md) — landed
- [The island boundary's names: one rule for both lanes, and the check that makes it true](2026-09-08-island-boundary-names.md) — active
- [Implementation plan: the island boundary's names](2026-09-08-island-boundary-names-implementation-plan.md) — active

### modules

- [Two answers and two silences: the scanner's second grammar, and the manifest keys nothing reads](2026-09-09-two-answers-and-two-silences.md) — active

### plugins

- [The category the plugin taxonomy does not name, and what a platform actually decomposes into](2026-09-11-distribution-plugins-and-platform-decomposition.md) — active

### targets

- [A verified Web run that asked the host for node](2026-09-12-a-verified-web-run-that-asked-the-host-for-node.md) — landed
- [Implementation plan: a UI framework on Android, iOS and Web (#622)](2026-09-12-622-implementation-plan.md) — landed
- [A UI framework on Android, iOS and Web: where each item of #622 lands, and the three it does not list](2026-09-12-622-a-ui-framework-on-android-ios-and-web.md) — landed
- [SDK toolchains, the payload/engine seam, and openkal across iOS, Android and Web](2026-09-11-sdk-toolchains-and-ios-local-verification.md) — landed
- [Where a platform's knowledge belongs: iOS, Android and Web across the engine, the index and the plugins](2026-09-11-platform-targets-design-review.md) — active

### triage

- [The engine gaps left open after the SDK batch](2026-09-12-engine-gaps-after-the-sdk-batch.md) — landed
- [Six open issues: what each one actually is, and what would answer it](2026-09-11-six-open-issues-analysis.md) — active

## By date

### 2026-09

- [Four upstream asks from a UI framework: what each one is under mcpp's design, and the combined plan](2026-09-13-four-upstream-asks-from-a-ui-framework.md) — active
- [The engine gaps left open after the SDK batch](2026-09-12-engine-gaps-after-the-sdk-batch.md) — landed
- [A verified Web run that asked the host for node](2026-09-12-a-verified-web-run-that-asked-the-host-for-node.md) — landed
- [Implementation plan: a UI framework on Android, iOS and Web (#622)](2026-09-12-622-implementation-plan.md) — landed
- [A UI framework on Android, iOS and Web: where each item of #622 lands, and the three it does not list](2026-09-12-622-a-ui-framework-on-android-ios-and-web.md) — landed
- [Six open issues: what each one actually is, and what would answer it](2026-09-11-six-open-issues-analysis.md) — active
- [SDK toolchains, the payload/engine seam, and openkal across iOS, Android and Web](2026-09-11-sdk-toolchains-and-ios-local-verification.md) — landed
- [Where a platform's knowledge belongs: iOS, Android and Web across the engine, the index and the plugins](2026-09-11-platform-targets-design-review.md) — active
- [The category the plugin taxonomy does not name, and what a platform actually decomposes into](2026-09-11-distribution-plugins-and-platform-decomposition.md) — active
- [Two answers and two silences: the scanner's second grammar, and the manifest keys nothing reads](2026-09-09-two-answers-and-two-silences.md) — active
- [A dlopen surface no closure walks, and a process with two unwinders](2026-09-09-dlopen-surface-and-two-unwinders.md) — landed
- [The documentation as a book: a chapter-by-chapter design](2026-09-08-the-documentation-as-a-book.md) — active
- [The island boundary's names: one rule for both lanes, and the check that makes it true](2026-09-08-island-boundary-names.md) — active
- [Implementation plan: the island boundary's names](2026-09-08-island-boundary-names-implementation-plan.md) — active
- [A curriculum for the examples, a reference for the documentation, and a check with a denominator](2026-09-08-examples-curriculum-and-documentation-plan.md) — superseded by [2026-09-08-documentation-architecture-three-trees.md](2026-09-08-documentation-architecture-three-trees.md)
- [Three documentation trees, three audiences, and the rule for citing between them](2026-09-08-documentation-architecture-three-trees.md) — active
- [第六轮生态复核:工具平面的目标轴](2026-09-07-round6-ecosystem-review.md)
- [一个包一个版本:xlings 地址的身份,以及 2026.9.6.5 之后的文档对齐](2026-09-07-package-identity-and-doc-alignment.md)
- [A module-first surface for graphics acceleration and heterogeneous computing](2026-09-07-module-first-heterogeneous-surface.md)
- [mcpp.toml 语义与风格的统一](2026-09-07-mcpp-toml-unified-semantics-design.md)
- [异构计算与图形的跨平台生态:完整矩阵与补齐方案](2026-09-07-heterogeneous-cross-platform-ecosystem.md)
- [通用构建基础设施:缺口、归属与验证](2026-09-07-general-build-infrastructure-gaps-design.md)
- [Ecosystem review — round 5b](2026-09-06-round5b-ecosystem-review.md)
- [Ecosystem review — round 5, F1 (llama.cpp on Vulkan)](2026-09-06-round5-f1-ecosystem-review.md)
- [The heterogeneous ecosystem, v3: what is built, what is not, and what decides each](2026-09-06-ecosystem-plan-v3.md)
- [多设备生态:实施计划与任务表](2026-09-05-multi-device-implementation-plan.md)
- [多设备加速生态:载荷、适配面、验证 lane 与框架验证](2026-09-05-multi-device-ecosystem-design.md)
- [Heterogeneous C++ builds and their ecosystem, v2: closing the host surface](2026-09-05-heterogeneous-build-ecosystem-design-v2.md)
- [加速器支持 实施计划](2026-09-05-accelerator-support-implementation-plan.md)
- [加速器支持:完整设计方案](2026-09-05-accelerator-support-design.md)
- [具名 runner、通用命令面、部分后端,与生态闭环](2026-09-04-named-runners-and-the-universal-command-surface.md)
- [Four gaps left by the ecosystem batch, and what to do about each](2026-09-04-four-gaps-after-the-ecosystem-batch.md)
- [商业级可用:mcpp × xlings 的裸机与嵌入式总体方案](2026-09-04-commercial-grade-baremetal-embedded-plan.md)
- [`[xlings]` is mcpp's surface for xlings' local project mechanism](2026-09-03-xlings-workspace-as-the-one-table.md)
- [The runner beyond bare metal: design](2026-09-02-runner-beyond-baremetal-design.md)
- [Issue #544: runner beyond bare metal — implementation plan](2026-09-02-issue544-runner-implementation-plan.md)
### 2026-08

- [Issue #540, verified: seven filed findings, six confirmed, one misaimed, four more underneath](2026-08-31-issue540-seven-audit-findings.md)
- [Project build hooks as owned intervals (#496)](2026-08-30-project-build-hooks-owned-intervals.md)
- [Three issues, measured: #532, #533, #534](2026-08-30-issues-532-533-534-analysis.md)
- [Four issues, measured: #527 (workspace half), #529, #535, #537](2026-08-30-issues-527-529-535-537-analysis-and-design.md)
- [Cross-repo fix plan: #532, #533, #534](2026-08-30-cross-repo-fix-plan-532-533-534.md)
- [The build-rule package: identity enforced, shape documented](2026-08-29-build-rule-package-spec.md)
- [issue #519:依赖的链接形态 —— 一条不变量,两个高度](2026-08-28-issue519-dependency-linkage-form.md)
- [目标侧被解析出来了,只发给了一个编译单元](2026-08-27-openkal-native-path-three-issues.md)
- [openkal 生态:接口判据、面的补全,与端口层的组合](2026-08-27-openkal-ecosystem-design-plan.md)
- [#516 解决方案:glob walk 在 Windows 上撞到 ANSI 代码页拼不出的目录名就崩](2026-08-27-issue516-windows-acp-glob-walk-fix.md)
- [支持矩阵:56 格实测,四处待修、一处生态空缺](2026-08-26-the-support-matrix-measured.md)
- [目标矩阵:六张表(载荷体系 / openkal 体系 × 三个构建机)](2026-08-26-target-matrix-six-tables.md)
- [目标矩阵:应该是什么,现在是什么,差在哪](2026-08-26-target-matrix-should-be-versus-is.md)
- [已经解析出的答案,没有被用来做决定](2026-08-26-resolved-but-not-consulted.md)
- [声明了却没被兑现:第八条,以及文档与词表的一次实测对账](2026-08-26-declared-but-not-made-to-exist.md)
- [`crossTarget` 非空被当成「系统来自图」](2026-08-26-cross-target-implies-graph.md)
- [aarch64 Linux:让整个生态可用,并且可测](2026-08-26-aarch64-linux-ecosystem-closure.md)
- [一个谓词族,六处缺陷:目标侧的分层判据](2026-08-25-the-two-layer-predicate-family.md)
- [目标体系分析:三元组承载了四件事,而它只有三段](2026-08-25-target-system-analysis.md)
- [OS × 工具链 × 目标:组合矩阵与理由](2026-08-25-os-toolchain-target-matrix.md)
- [mcpp 目标词表:一套规范,映射到各编译器](2026-08-25-mcpp-target-vocabulary-spec.md)
- [mcpp 目标侧设计](2026-08-24-target-side-design.md)
- [目标侧架构:五层、四来源、四规则](2026-08-24-target-side-architecture.md)
- [目标侧来自依赖图之后:七项优化方案](2026-08-24-graph-target-side-optimization-plan.md)
- [目标侧解析:预构建体系与构建期体系的统一架构](2026-08-23-target-side-resolution-architecture.md)
- [三项未完成事项的详细方案](2026-08-21-three-remaining-items-plan.md)
- [裸机生态的四项未完成事项:方案](2026-08-21-freestanding-outstanding-four.md)
- [裸机方向的优化方案](2026-08-21-baremetal-optimization-plan.md)
- [mcpp 在内核 / 嵌入式 / freestanding 方向的评估](2026-08-21-baremetal-ecosystem-assessment.md)
- [PR #455–#459 深度 review:裸机 freestanding 从「能编」到「能用」](2026-08-20-pr455-459-freestanding-review.md)
- [`mcpp pack` 生产侧与消费侧模型:整体架构 review](2026-08-20-pack-and-consumer-model-review.md)
- [openkal 0.4: what one portable program found, and what it says about the method](2026-08-20-openkal-portable-program-findings.md)
- [openkal: implementation plan and outcome](2026-08-20-openkal-implementation-plan.md)
- [openkal 设计方案:通用内核 ABI 规范](2026-08-20-openkal-design.md)
- [openkal: plan for industrial completeness](2026-08-20-openkal-completeness-plan.md)
- [openarch:接口层与多指令集后端的实现方案](2026-08-20-openarch-implementation-plan.md)
- [openarch 实现方案:arch 机制层](2026-08-20-openarch-implementation-design.md)
- [`mcpp pack` 的 `kind = "shared"` 产物带走了构建机:#460 的实测、根因与优化方案](2026-08-20-issue460-shared-library-runpath.md)
- [裸机 / 嵌入式 / 内核方向的生态定位与缺口(设计方案)](2026-08-20-freestanding-ecosystem-positioning.md)
- [freestanding 生态实施计划与依赖图](2026-08-20-freestanding-ecosystem-implementation-plan.md)
- [裸机:用户面能感受到的变化(场景 + 伪代码)](2026-08-20-baremetal-user-facing-scenarios.md)
- [裸机 / freestanding 支持 — 实施计划(2026-08-19)](2026-08-19-freestanding-baremetal-implementation-plan.md)
- [mcpp 裸机 / freestanding 支持 — 架构设计方案(2026-08-19)](2026-08-19-freestanding-baremetal-design.md)
- [裸机 / freestanding — 第三阶段:从「能跑」到「能用」](2026-08-19-baremetal-phase3-usable-plan.md)
- [裸机 / freestanding — 全生态打通实施计划(第二阶段)](2026-08-19-baremetal-ecosystem-closure-plan.md)
- [Windows 动态库分发、cl.exe 消费,与 `.ixx` 的默认支持](2026-08-18-windows-shared-library-and-module-extensions.md)
- [四项遗留的统一分析:判据挂错轴,以及 cl.exe 到底怎么办](2026-08-18-open-items-analysis-and-axis-discipline.md)
- [C++ 裸机 / freestanding:深度调研与 mcpp 路线分析(2026-08-18)](2026-08-18-freestanding-baremetal-analysis.md)
- [Windows 三条轴:落地报告(mcpp 2026.8.17.1)](2026-08-17-windows-three-axes-final-report.md)
- [库分发:`mcpp pack <target>` 与二进制包(2026-08-17)](2026-08-17-library-distribution-design.md)
- [mcpp 分发架构:全面分析与方案(2026-08-17)](2026-08-17-distribution-architecture-analysis-and-design.md)
- [Windows 工具链的三条轴:来源、SDK、运行时(2026-08-16)](2026-08-16-windows-toolchain-three-axes-design.md)
- [工具链架构 review:两种来源、选择与切换、构建与分发(2026-08-16)](2026-08-16-toolchain-architecture-review.md)
- [MSVC × xlings 生态打通 —— 综合报告(2026-08-16)](2026-08-16-msvc-ecosystem-final-report.md)
- [MSVC 在 xlings 生态里打通 —— 跨仓库计划、依赖与验收(2026-08-16)](2026-08-16-msvc-ecosystem-cross-repo-plan.md)
- [MSVC 纳入 mcpp 工具链体系 —— 设计 + 验证方案(2026-08-16)](2026-08-16-msvc-as-a-managed-toolchain.md)
- [xmake + clang 的 `import std`:错误消息把人指向了死路(2026-08-15)](2026-08-15-xmake-clang-import-std.md)
- [改一处实现,重编多少?—— 模块写法、编译器、与 BMI 的实际行为(2026-08-15)](2026-08-15-module-edit-granularity.md)
- [#426 #427 与 main 当前红 —— 核实与修复方案(2026-08-15)](2026-08-15-issues-426-427-analysis.md)
- [#412 #415 #416 #417 #418 #421 #422 —— 逐条核实与修复方案(2026-08-15)](2026-08-15-issues-412-422-analysis.md)
- [bench 可断续:每个测量点独立落盘,进度可算可显示(2026-08-15)](2026-08-15-bench-resumable-design.md)
- [bench 改为「本地真跑、CI 不跑」:方案与 Linux 实测计划(2026-08-14)](2026-08-14-bench-local-first-design.md)
- [mcpp 构建性能:架构层面的分析与方案(2026-08-13)](2026-08-13-build-performance-architecture.md)
- [构建性能优化:综合报告(2026-08-13)](2026-08-13-build-optimization-status.md)
- [模块化 C++ 构建性能深度分析与优化方案](2026-08-12-modular-build-performance-deep-analysis.md)
- [mcpp 冷构建深度优化方案](2026-08-12-cold-build-optimization-plan.md)
- [`bench/` 构建引擎基准套件 —— 架构与实施计划](2026-08-12-bench-suite-architecture-and-plan.md)
- [源文件角色表 与 build.mcpp 运行上限 —— 把两个硬编码变成两条声明](2026-08-11-source-kind-table-and-build-program-timeout.md)
- [`$ORIGIN` 被 SubOS farm 遮蔽 —— helloegui 运行期 undefined symbol 分析与修复方案](2026-08-11-runtime-search-origin-precedence-analysis.md)
- [`$ORIGIN` 优先级 + 共享库运行时契约 —— 实施计划](2026-08-11-origin-precedence-implementation-plan.md)
- [实施计划:运行期搜索闭包 与 binding 降级](2026-08-11-graphics-runtime-search-closure-implementation-plan.md)
- [图形栈剩下的那一半:链接期看得见、运行期看不见](2026-08-11-graphics-runtime-search-closure-and-binding-degradation.md)
- [PR #400 收尾设计方案 —— 重新判定阻塞点，并把串行收口改成并行](2026-08-10-pr400-completion-design.md)
- [图形栈全面不可用 —— 三层独立故障,和一条没有主人的依赖链](2026-08-10-graphics-stack-usability-design.md)
- [实施计划:图形栈闭合与分发档位](2026-08-10-graphics-closure-implementation-plan.md)
- [图形栈打通:一个标签、一条没人依赖的边、一个被钉住的 pin](2026-08-10-graphics-closure-and-distribution-tiers-design.md)
- [验收记录:图形栈闭合与分发档位(2026.8.10.2)](2026-08-10-graphics-closure-acceptance.md)
- [xlings × mcpp 生态契约收敛与优化设计](2026-08-09-xlings-mcpp-ecosystem-convergence-design.md)
- [PR #400 中文交接文档](2026-08-09-pr400-handoff-zh.md)
- [mcpp Template, Runtime, Graphics, and AUR Validation Ledger](2026-08-09-mcpp-template-runtime-graphics-aur-validation.md)
- [mcpp Template, Runtime, Graphics, and AUR Convergence Implementation Plan](2026-08-09-mcpp-template-runtime-graphics-aur-implementation-plan.md)
- [mcpp 模板、运行时、图形栈与 AUR 聚焦设计](2026-08-09-mcpp-template-runtime-graphics-aur-focused-design.md)
- [mcpp Open Issue 全量深度核验报告](2026-08-09-issue-triage-full-sweep.md)
- [xlings 运行时底座 —— 实施计划](2026-08-08-xlings-runtime-substrate-implementation-plan.md)
- [机器可读输出协议 —— 拆分实施计划](2026-08-08-wire-protocol-implementation-plan.md)
- [载荷版本与契约漂移:四个缺陷,一条线](2026-08-08-payload-version-and-contract-drift-design.md)
- [机器可读输出协议 —— 对 RFC #379 的核对与修正](2026-08-08-machine-readable-output-protocol-design.md)
- [Configure-Only Compile Database Implementation Plan](2026-08-08-configure-only-cdb-implementation-plan.md)
- [`build --configure-only` 与可靠 CDB 设计](2026-08-08-configure-only-cdb-design.md)
- [编译器是能力 —— 跨仓实施计划](2026-08-08-compiler-as-capability-implementation-plan.md)
- [xlings 作为 mcpp 的运行时底座:运行时身份、链接契约与环境契约](2026-08-07-xlings-as-runtime-substrate-design.md)
- [两处「模型比生态少一层」:Windows 资源输入 与 版本身份](2026-08-07-windows-resources-and-version-identity-design.md)
- [依赖提供物与构建期输入:两个缺口,同一个形状](2026-08-06-provisions-and-build-inputs.md)
- [命令长度:把「靠崩溃发现的规模上限」从架构上消掉](2026-08-06-command-length-architecture.md)
- [issue #355：依赖产出的 host 工具（codegen 工具链缺口）](2026-08-05-issue355-dependency-host-tools-design.md)
- [build.mcpp 机制架构设计：一个 hook，多种节点](2026-08-05-build-mcpp-extensibility-architecture.md)
- [`ci-fresh-install` 11 个 job 全红 —— 两个独立缺陷的修复方案](2026-08-04-ci-fresh-install-two-defects.md)
- [Windows → Linux 交叉工具链(路径 A:canadian-cross payload)— 设计方案](2026-08-03-windows-host-linux-cross-design.md)
- [issue #344：全局 build cache 的对象地址必须与消费方无关](2026-08-03-issue344-cache-object-address-design.md)
- [索引版本下限(E0006)不应让旧客户端不可用 —— 分析与方案](2026-08-03-index-floor-should-degrade-not-brick.md)
- [索引的可用性不得决定 mcpp 的可用性 —— 纯 mcpp 侧优化方案](2026-08-03-index-availability-must-not-decide-mcpp-availability.md)
- [B3 — 产物命名按 target 而非 host(交叉构建每次重链)— 修复方案](2026-08-03-b3-target-aware-artifact-naming.md)
- [Windows 可用性 — 实施计划](2026-08-02-windows-usability-implementation-plan.md)
- [Windows 可用性设计:裸机无感可用 + build.mcpp 全方言 + 测试面补齐](2026-08-02-windows-usability-design.md)
- [深度分析:issue #336(macOS 静态 libc++)与 mcpp-index PR #142(boost-ext.ut)](2026-08-02-issue336-pr142-analysis.md)
- [宿主编译单一生产者:让 build.mcpp 的能力等于 mcpp 的能力](2026-08-02-host-compile-single-producer-design.md)
- [宿主编译单一生产者 — 实施计划](2026-08-02-host-compile-implementation-plan.md)
- [issue #331 逐条核验 + Windows 无 MSVC 默认工具链分析](2026-08-01-issue331-windows-msvc-triage.md)
### 2026-07

- [`mcpp test --workspace`:跨平台模型一致性、输出差异与 macOS 停滞的根因分析](2026-07-31-test-workspace-observability-analysis.md)
- [`mcpp test` 可观测性与有界性 —— 实施计划](2026-07-31-test-observability-implementation-plan.md)
- [C++20 作为一等 `standard` 档位:`import std` 全平台可用性设计](2026-07-31-cpp20-standard-support-design.md)
- [C++20 档位支持 — 实施计划(单 PR,目标 2026.7.31.1)](2026-07-31-cpp20-implementation-plan.md)
- [索引刷新策略收敛：从「时间驱动」到「解析驱动」— 设计](2026-07-30-issue315-index-refresh-policy-design.md)
- [索引刷新策略收敛 — 实施计划](2026-07-30-issue315-implementation-plan.md)
- [BMI staging 原语 + BMI 缓存根收敛 — 实施计划](2026-07-30-issue311-implementation-plan.md)
- [BMI staging 原语 + BMI 缓存根收敛 — 设计](2026-07-30-issue311-bmi-staging-and-cache-root-design.md)
- [依赖构建产物的全局缓存收敛 — 设计](2026-07-30-dep-build-cache-scoping-design.md)
- [依赖构建产物的全局缓存收敛 — 实施计划](2026-07-30-dep-build-cache-implementation-plan.md)
- [日期版本号 + xlings pin 收敛 — 实施计划](2026-07-27-date-version-and-xlings-pin-plan.md)
- [日期版本号 + xlings pin 收敛 — 设计](2026-07-27-date-version-and-xlings-pin-design.md)
- [裸名 wire address 修复 — Implementation Plan（mcpp 0.0.109）](2026-07-26-bare-name-wire-address-implementation-plan.md)
- [裸名依赖的 wire address 收敛 — Design（mcpp 0.0.109）](2026-07-26-bare-name-wire-address-design.md)
- [xlings#381 索引键补齐命名空间维度 — 设计/优化方案](2026-07-25-xlings381-index-namespace-keying-design.md)
- [SPEC-001 落地 — Implementation Plan(mcpp 0.0.106)](2026-07-25-spec001-implementation-plan.md)
- [mcpp `name` / `namespace` 规范实现(定稿)](2026-07-25-name-namespace-canonical-implementation-spec.md)
- [`package.name` / `package.namespace` 双向验证报告](2026-07-25-name-namespace-bidirectional-verification-report.md)
- [#278 包身份双侧收敛 — Implementation Plan](2026-07-25-issue278-implementation-plan.md)
- [#278 包身份口径收敛 — 索引侧 + 依赖侧完整方案](2026-07-25-issue278-descriptor-name-form-canonicalization-design.md)
- [mcpp test 演进批次二 — Implementation Plan](2026-07-24-test-batch2-plan.md)
- [mcpp test 架构评估与设计方案](2026-07-24-mcpp-test-design-review.md)
- [批次三:#273 沙盒围栏 + CI wine 缓存 — Implementation Plan](2026-07-24-issue273-containment-plan.md)
- [嵌入式平台支持 — 方案设计 (Embedded Platform Support)](2026-07-24-embedded-platform-support-design.md)
- [mcpp test: per-test isolation, filter, JSON output — Implementation Plan](2026-07-23-test-isolation-json-plan.md)
- [v0.0.102 批次设计 —— #261 / #257 / #258 / #254(+#256 收尾)](2026-07-22-v0.0.102-batch-254-261-design.md)
- [索引组织迁移 + 采纳 xlings 0.4.68 per-repo artifact 来源 — 设计方案](2026-07-22-issue267-269-index-artifact-and-org-migration-design.md)
- [Issue #267/#269 索引组织迁移 + artifact 采纳 — 实施计划(0.0.103)](2026-07-22-issue267-269-impl-plan.md)
- [Issue 分析报告 —— #254 / #256 / #257 / #258 / #259 / #261](2026-07-22-issue-triage-254-261.md)
- [契约准入(Contract Admission)设计 —— 索引级 + 包级 floor](2026-07-22-contract-admission-design.md)
- [Issue #253 —— per-feature `flags` + per-OS `features` 设计](2026-07-20-issue-253-feature-flags-and-per-os-features-design.md)
- [v0.0.99 批次设计 —— #243 feature 转发 + #238 xlings 升级 + #230 windows 复验](2026-07-19-v0.0.99-feature-forwarding-238-230-design.md)
- [#233 对象路径消歧的两个后续缺口(#240 / #239)——根因分析与统一修复方案](2026-07-19-object-path-disambiguation-followups-239-240-design.md)
- [大型源码直编包全平台化:平台三修(#247/#248/#249)+ build.mcpp 构建期生成能力 + 描述符复杂度治理](2026-07-19-large-source-pkg-platform-fixes-and-buildmcpp-generation-design.md)
- [#230–#243 批次总账 + 架构评估(治理文档)](2026-07-19-issues-230-243-batch-ledger-and-architecture-assessment.md)
- [Issue #243 —— feature 依赖转发(`dep/feat`)设计](2026-07-19-issue-243-feature-forwarding-design.md)
- [mcpp 0.0.97 架构级修复 —— 实施设计与任务拆分(单 PR / 逐簇 commit)](2026-07-18-v0.0.97-architectural-remediation-implementation-plan.md)
- [Issue #215+ 分类分析与架构级修复方案](2026-07-18-issue-triage-215plus-architectural-remediation.md)
- [mcpp 通用构建能力需求清单(G1–G9)](2026-07-17-mcpp-feature-requests.md)
- [汇编源一等公民 + 通用构建能力(G1–G9)设计方案](2026-07-17-asm-sources-and-general-build-capabilities-design.md)
- [工具链 × 目标 命名统一 — 设计方案(实现 + 显示 + 使用)](2026-07-15-toolchain-target-naming-unification-design.md)
- [Linux → Windows MinGW 交叉工具链 — 设计方案](2026-07-15-mingw-linux-cross-windows-design.md)
- [c++fly:一键启用"最新标准 + 全部实验特性"(语言 + 标准库)设计](2026-07-14-std-features-experimental-gate-design.md)
- [c++fly(0.0.91)单 PR 实施计划](2026-07-14-single-pr-091-implementation-plan.md)
- [工具链后端抽象层 + MSVC 原生构建 + MinGW 生态入驻 — 设计方案](2026-07-13-toolchain-backend-abstraction-msvc-mingw-design.md)
- [0.0.90 单 PR 实施计划(post-089 路线图全量)](2026-07-13-single-pr-090-implementation-plan.md)
- [0.0.89 后路线:std 模块方言旗标一致性(#210)+ 遗留优化清单 — 设计方案](2026-07-13-post-089-roadmap-and-std-dialect-flags-design.md)
- [MSVC System-Toolchain Detection — Implementation Plan](2026-07-13-msvc-system-toolchain-implementation-plan.md)
- [MSVC System-Toolchain Detection — Design](2026-07-13-msvc-system-toolchain-detection-design.md)
- [编译器方言触点审计(0.0.88 基线)](2026-07-13-compiler-dialect-touchpoint-audit.md)
- [项目本地模式下的索引作用域:别把默认全局的官方索引(xim)注入项目组(架构分析 + 修复设计)](2026-07-09-project-index-scope-global-infra-fix.md)
- [Scanner backend abstraction: per-package opt-in, P1689 as lingua franca, plan-vs-ddi reconciliation (Design)](2026-07-08-scanner-backend-abstraction-design.md)
- [Root-cause remediation for the 0.0.85 rollout incidents (Design)](2026-07-08-root-cause-remediation-design.md)
- [Index version semantics + descriptor grammar v2 (long brackets) + single-source-of-truth lint (Design)](2026-07-08-index-version-semantics-and-descriptor-grammar-design.md)
- [Descriptor & index evolution — 0.0.85 release train roadmap (cross-repo)](2026-07-08-descriptor-index-evolution-roadmap.md)
- [Hermetic toolchain link model — one-shot cross-repo fix for issue #195](2026-07-07-hermetic-toolchain-link-model-design.md)
### 2026-06

- [Workspace-aware `mcpp test` + zero-shell self-contained mcpp-index (Design)](2026-06-30-workspace-test-and-zero-shell-index-design.md)
- [Bare OS-alias sugar for `[target.*]` conditional tables (Design)](2026-06-30-target-bare-alias-sugar-design.md)
- [L3 `build.mcpp` — native imperative build program (implementation design)](2026-06-30-l3-build-mcpp-implementation-design.md)
- [The `mcpp` build-module library for `build.mcpp` (Architecture & Design)](2026-06-30-build-mcpp-module-library-design.md)
- [Windows Runtime-DLL Deployment & `compat.openblas` Windows Support (Design)](2026-06-29-windows-runtime-dll-deployment-and-openblas.md)
- [mcpp.toml: Build Environment, Platform-Conditional Config, and `build.mcpp` (Design)](2026-06-29-manifest-environment-and-platform-design.md)
- [Feature System v2 — Stage 2: feature-activated optional dependencies (Design)](2026-06-29-feature-optional-dependencies-s2-design.md)
- [Feature System v2 — Capability-Oriented Model (Design)](2026-06-29-feature-capability-model-design.md)
- [分析报告:`abi:` 能力检查把「libc ABI」与「C++ stdlib」混为一谈 —— glfw 在 clang/libc++ 下误报 ABI mismatch](2026-06-27-glfw-abi-glibc-vs-libcxx-conflation-analysis.md)
- [设计:ABI/工具链兼容性「维度化」模型 —— 一步到位的单 PR 方案](2026-06-27-abi-compat-model-single-pr-design.md)
- [LLVM libatomic 自包含缺口分析与修复设计(libatomic.so.1 cannot open)](2026-06-26-llvm22-libatomic-self-containment-design.md)
- [Identity-First Package Resolution — Filename Is Not a Key](2026-06-26-identity-first-resolution-no-filename.md)
- [gtest_main 冲突修复:src 轨 feature 控制 + dev 轨 main 检测 + `mcpp add --dev`](2026-06-25-gtest-main-feature-and-add-dev-design.md)
- [依赖入口对象的条件链接（fix `mcpp test` duplicate `main`）设计方案](2026-06-25-dependency-archive-linking-design.md)
- [compile_commands.json 测试覆盖缺失：分析报告与设计方案](2026-06-25-cdb-test-coverage-design.md)
- [离线优先的索引刷新 + mcpp-index 发布机制](2026-06-24-offline-first-index-and-mcpp-index-publish.md)
- [macOS `mcpp build`:`library not found for -lSystem` 根因与修复](2026-06-24-macos-link-lsystem-sdk.md)
- [Issue #43 分诊与关闭记录 — macOS 全新安装首跑](2026-06-24-issue43-macos-first-run-triage.md)
- [Termux / Android (aarch64) 适配分析报告](2026-06-23-termux-android-adaptation.md)
- [原生 aarch64 musl-gcc 用 musl 1.2.5 重建(canadian-cross)— 复现指南](2026-06-23-aarch64-musl-gcc-canadian-cross-rebuild.md)
- [mcpp 后续修复:统一汇总 + 方案设计 + PR 拆分](2026-06-22-mcpp-followups-design.md)
- [aarch64 / Android 支持 —— 跨仓库 MVP 顶层设计方案](2026-06-22-aarch64-android-mvp-design.md)
- [跨仓库修复方案:残缺/被删依赖被 `.mcpp_ok` 盲区放过](2026-06-21-xcb-and-install-integrity-cross-repo-fix.md)
- [Package Resolution Architecture — Identity-First Locator Design](2026-06-20-package-resolution-architecture.md)
- [Runtime Launch Hygiene & Multi-Distro Coverage Implementation Plan](2026-06-19-runtime-launch-and-multi-distro-plan.md)
- [Pack Mode Redesign — Two-Axis Model, Clearer Names, `system` Mode](2026-06-19-pack-mode-redesign.md)
- [Per-Target 构建配置设计:配置发散归编译单元,目标只携本地标志](2026-06-18-per-target-build-config-design.md)
- [CLI Modularization — Architecture & Implementation Plan](2026-06-10-cli-modularization.md)
- [Library / Component Download Progress — Design](2026-06-09-library-download-progress-design.md)
- [mcpp.toml Schema 所有权设计:语法封闭 · 词汇开放](2026-06-04-manifest-schema-ownership.md)
- [mcpp core: runtime closure (rpath) + toolchain defaults](2026-06-03-runtime-closure-and-toolchain-defaults.md)
- [mcpp 模板系统(package-based templates)— 设计 v2](2026-06-03-package-templates.md)
- [mcpp: GL Runtime Closure Plan](2026-06-03-gl-runtime-closure-plan.md)
- [Module-First Usage Requirements Architecture](2026-06-02-usage-requirements-architecture.md)
- [2026-06-02 imgui mcpp dependency fixes](2026-06-02-imgui-mcpp-dependency-fixes.md)
- [Dotted Dependency Selector Architecture](2026-06-02-dotted-dependency-selectors.md)
- [C++ 标准一等配置设计](2026-06-01-cpp-standard-first-class-design.md)
- [CI 工具链缓存优化分析](2026-06-01-ci-toolchain-cache-optimization.md)
### 2026-05

- [Index Refresh And Dependency Cache Label Fix](2026-05-31-index-refresh-cache-labels-plan.md)
- [mcpp 0.0.35: Package-Owned Build Metadata Plan](2026-05-30-package-owned-build-flags-plan.md)
- [BMI Cache And Custom Index Build Fix](2026-05-30-bmi-cache-custom-index-fix.md)
- [mcpp build 报错输出优化方案](2026-05-26-build-error-output-optimization-plan.md)
- [设计方案：中断安装统一恢复机制](2026-05-23-interrupted-install-recovery-design.md)
- [mcpp 可观察性设计方案](2026-05-22-observability-design.md)
- [Bug 分析：LLVM 共享库 RUNPATH 失效的完整链路](2026-05-22-llvm-runpath-bug-analysis.md)
- [Fix: LLVM shared libraries have stale RUNPATH after install](2026-05-22-fix-llvm-shared-lib-runpath.md)
- [Fallback 代码提取方案 — 代码架构重构](2026-05-22-fallback-code-extraction-plan.md)
- [mcpp Fallback 架构设计方案](2026-05-22-fallback-architecture-design.md)
- [设计方案：ensure_base_init_ok + mcpp self init --force](2026-05-22-ensure-base-init-design.md)
- [分析：Ctrl+C 中断 bootstrap 后 mcpp 进入不可用状态](2026-05-22-ctrl-c-interrupted-bootstrap-analysis.md)
- [resolve_xpkg_path() 的 copy 优先级问题分析](2026-05-22-copy-priority-analysis.md)
- [设计方案：Payload-first 工具链环境管理](2026-05-21-payload-first-sysroot-design.md)
- [Linux sysroot 缺少内核头文件导致 std module 预编译失败](2026-05-21-linux-sysroot-missing-kernel-headers.md)
- [MSVC STL Discovery — msvc.cppm 模块设计](2026-05-20-msvc-stl-discovery-plan.md)
- [Windows 平台成熟度提升方案](2026-05-19-windows-platform-maturity-plan.md)
- [Windows 成熟度提升 V2 方案](2026-05-19-windows-maturity-v2-plan.md)
- [Windows E2E 与 macOS 对齐方案](2026-05-19-windows-e2e-parity-plan.md)
- [Windows Pack Design](2026-05-19-pack-windows-design.md)
- [Windows LLVM/Clang 支持设计方案](2026-05-17-windows-llvm-support-design.md)
- [mcpp](2026-05-16-readme-draft.md)
- [macOS Support Design — LLVM/Clang 自含工具链方案](2026-05-16-macos-support-design.md)
- [macOS LLVM 默认工具链跨平台适配方案](2026-05-16-macos-llvm-default-toolchain.md)
- [`[indices]` 功能增强设计方案](2026-05-16-indices-enhancement-design.md)
- [mcpp 功能特性清单](2026-05-16-feature-list.md)
- [MCPP 跨平台 Clang/LLVM 支持分析报告](2026-05-16-cross-platform-clang-analysis.md)
- [std.compat 支持 + cxx_scan restat + 增量零重编 E2E](2026-05-15-stdcompat-restat-e2e.md)
- [Fingerprint 稳定性与 Fast-Path 一致性 — 优化方案](2026-05-15-fingerprint-stability-and-fastpath-coherence.md)
- [Clang 编译管线平权 + 工具链抽象层设计](2026-05-15-clang-parity-and-toolchain-abstraction.md)
- [MCPP LLVM/Clang Toolchain Support Analysis and Design](2026-05-13-llvm-clang-toolchain-support-design.md)
- [Workspace Phase 1 Implementation Plan](2026-05-12-workspace-implementation-plan.md)
- [mcpp Workspace 设计方案](2026-05-12-workspace-design.md)
- [compile_commands.json 设计方案](2026-05-12-compile-commands-design.md)
- [mcpp 构建优化深度分析报告](2026-05-12-build-optimization-analysis.md)
- [Namespace Field Design — mcpp 0.0.6](2026-05-11-namespace-field-design.md)
- [2026-05-08 — 包索引仓库配置 (Package-Index Repo Configuration)](2026-05-08-package-index-config.md)

## Undated

Records written before the date prefix was the convention.

- [Fix: xlings 包内 mcpp 的 MCPP_HOME 检测](fix-xlings-package-home-detection.md)
- [LLVM 工具链安装失败分析](llvm-install-failure-analysis.md)
- [Platform Abstraction Layer — Architecture & Implementation Plan](platform-abstraction-plan.md)
- [Remaining Platform Macros Outside src/platform/ — Analysis Report](platform-remaining-ifdefs-report.md)

## todos/

Work items rather than records of a decision.

- [aarch64-linux 生态适配分析报告](todos/2026-06-22-aarch64-linux-ecosystem-support-analysis.md)
- [aarch64 glibc-world / LLVM 工具链构建计划(deferred)](todos/2026-06-23-aarch64-glibc-world-llvm-buildout-plan.md)
- [TODO: e2e 套件按耗时分片(并行 matrix)](todos/2026-06-24-e2e-suite-sharding.md)
- [macOS 首跑遗留问题(已记录,待处理)](todos/2026-06-24-macos-first-run-remaining.md)
