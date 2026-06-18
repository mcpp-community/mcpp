# Per-Target 构建配置设计:配置发散归编译单元,目标只携本地标志

> 2026-06-18 · 状态: 已实施(0.0.55,①②③④⑤;⑥ 明确不做) · 代码锚点基于 main@47d026f (v0.0.54)
>
> **实施结果**:`[targets.<name>]` 新增 `defines`/`cxxflags`/`cflags`(入口作用域,④)与
> `required_features`(门禁,⑤);不支持键发 warning/`--strict` error(②);`mcpp test` 接
> `--profile`/`--features`/`--strict`(①);docs/05(及 zh)新增决策指引(③)。
> 单测 `tests/unit/test_manifest.cpp` 覆盖 ②④;e2e `57/58/59` 覆盖 ④⑤①。
> 验证:`mcpp build` 自举通过;`mcpp test` 18/18;新 e2e 三项 + 多目标/workspace/静态/共享/
> 包级 flags 回归集全过。
> 关联: [GitHub Issue #131](https://github.com/mcpp-community/mcpp/issues/131)(`[targets.<name>]` 求 cxxflags 覆盖)
>      `.agents/docs/2026-06-04-manifest-schema-ownership.md`(语法封闭·词汇开放)
>      `.agents/docs/2026-05-30-package-owned-build-flags-plan.md`(per-unit flags 管线由此建立)

## 0. 问题

Issue #131 请求 `[targets.<name>]` 支持按目标覆盖 `cxxflags`,类比 xmake `target:add("cxflags")` /
CMake `target_compile_options` / Cargo `[profile.*.package.*]`。两个动机示例:

- **场景 A**:`[targets.server] cxxflags=["-DBUILD_SERVER=1"]` / `[targets.client] cxxflags=["-DBUILD_CLIENT=1"]`
  —— 一个工程出两个二进制,各带各的宏。
- **场景 B**:`[targets.test_contracts] cxxflags=["-fcontract-evaluation-semantic=observe"]`
  —— 测试目标要在不同求值语义下编译。

本设计的结论:**#131 不应按字面("通用 per-target cxxflags over 共享编译池")实现**。该字面诉求
与 mcpp 的 compile-once 模型冲突,且需求已被现有三轴(workspace / features / profile)基本覆盖;
真正缺的是两处小改 + 文档,外加两个可选的、严格受限的便利原语。

## 1. 设计判据(判定法)

> **配置发散在"编译单元(包)"边界,不在"链接单元(target)"边界。**
> target 只能携带它**独占、且不影响共享镜像一致性**的本地标志;凡是会沿模块图传染、
> 或影响 ABI/链接一致性的设置,禁止 per-target。

依据:

- **target 是链接期概念**(哪些 `.o` 链在一起);**编译 flags 是编译期概念**(一个 `.o` 怎么编出来)。
  #131 的别扭本质是把编译期配置挂到链接期实体上、还跨越共享编译池——阻抗失配。
- **Cargo 用规则编码了同一条边界**:`[profile.*.package.*]` 允许 `opt-level`/`codegen-units`
  这类**本地优化**,却**明令禁止** `panic`/`lto`/`rpath` 这类必须全局一致的项,且任意 `rustflags`
  至今只在 nightly `-Z profile-rustflags`(#10271,未稳定)。Cargo `[[bin]]` 也**不支持** per-bin
  features/flags(只有 `required-features` 门禁)。#7916 是 feature resolver v2 的 `dev_dep`
  追踪(已闭),属 feature 并集机制,**不是** per-target features。
- 与本仓 `manifest-schema-ownership` 的铁律 C 一致:**包级旋钮统一收敛进 features**,糖键入核需
  领域中立 + 1:1 脱糖。per-target 任意 flags 不满足此约束。

## 2. 现状代码盘点(main@47d026f)

| 机制 | 代码锚点 | 现状 |
|---|---|---|
| `Target` 结构(name/kind/main/soname) | `src/manifest.cppm:55-60` | **无任何 flags 字段** |
| `[targets.<name>]` 解析(只读 kind/main) | `src/manifest.cppm:560-579` | 其余键**静默忽略** |
| `[build].cxxflags` 解析(全局一份) | `src/manifest.cppm:895-896` / `BuildConfig:104` | 仅项目级 |
| compile-once:每源一 CompileUnit | `src/build/plan.cppm:295-322` | 共享源**编一次**,被多 target 共享 |
| per-unit flags 管线(已建) | `CompileUnit{packageCflags,packageCxxflags}` `plan.cppm:23-24`;ninja 边变量 `unit_cxxflags` `ninja_backend.cppm:494,541-545,588-592` | #131 可复用 |
| 入口 main 按 target 单独建 CU(**④ 挂载点**) | `src/build/plan.cppm:495-509`(`main_cu`,已灌 `packageCxxflags`) | 入口天然 target 独占 |
| target 独占性:仅入口 main | `plan.cppm:336-348`(entryFilesAcrossTargets);`:427-441`(非入口对象链进每个 target) | 非入口源**不可** per-target 独占 |
| features:additive、按包全局、只出宏 | `src/build/prepare.cppm:2078-2152`(`-DMCPP_FEATURE_*` 推入 `buildConfig.cxxflags` `:2097-2102`) | 共享 lib **编一份** |
| profile:整构建模式(含 cxxflags 逃生舱) | `BuildConfig` 优化旋钮;`prepare.cppm` 合入 active profile | build 可选,见缺口 |
| `mcpp build` 解析 `--profile/--features` | `src/cli/cmd_build.cppm:29-31`;`BuildOverrides` `prepare.cppm:281-287` | OK |
| `mcpp test` **不解析任何 flag** | `cmd_test` `src/cli/cmd_build.cppm:66-71`;`run_tests(passthrough)` `src/build/execute.cppm:406` | **缺口①** |
| fingerprint 含 compile-flags hash | `src/toolchain/fingerprint.cppm:7` | per-target flags 自动覆盖 |

**为什么共享源做不到 per-target 不同 flag**:一个 `.cppm` 编出**一个 `.o` + 一个 BMI**,被多个
target 链接。要让它按 target 带不同宏,就得把编译节点**复制成多份** `obj/<variant>/`,且 BMI 按
`(模块,变体)` 索引——变体会沿 import 图**传染**整片子图。这正是 compile-once 要避免的成本,也是
本设计拒绝 ⑥ 的原因。

## 3. 需求拆解(把 #131 还原成三类真实需求)

1. **变体二进制**(场景 A):宏若只碰各 app **自己的代码** → workspace 拆包;若必须穿透共享 core
   → 那是 core 的互斥变体,features(并集)给不了同构建两版,需分次构建或运行期分发。
2. **测试换模式**(场景 B):求值语义/sanitizer 是**整构建模式**,须到达被测库 → profile,而非 target。
3. **目标独占源的本地标志**:仅作用于该 target 入口/独占源(`-Wno-x`、`-DVERSION=`)→ 唯一真正
   适合 per-target flags 的窄缝(入口独占,无 compile-once 冲突)。

## 4. 方案

### ① [P0·小] `mcpp test` 接受 `--profile` / `--features`

- **解决**:场景 B 的规范解是 profile,但 `mcpp test` 今天丢弃所有 flag(`cmd_build.cppm:66-71`),
  profile 机器无法用于测试构建。
- **改动**:`cmd_test` 照搬 `cmd_build` 的 overrides 解析(`cmd_build.cppm:29-31`);
  `run_tests`(`execute.cppm:406`)增收 `BuildOverrides` 并透传至 `prepare_build`(已有 `includeDevDeps=true` 路径)。
- **语义正确性**:profile 整构建统一 → observe/sanitizer **到达被测库**;复用 fingerprint;不破 compile-once。
- **用法**:`mcpp test --profile contracts`,其中 `[profile.contracts] cxxflags=["-fcontracts","-fcontract-evaluation-semantic=observe"]`。

### ② [P0·小] `[targets.<name>]` 不支持键给显式提示

- **解决**:今天写 `[targets.x] cxxflags=[...]` 被静默丢弃(`manifest.cppm:560-579`),零反馈——
  正是 #131 的 footgun。
- **改动**:target 解析识别 `cxxflags`/`cflags`/`defines`/`features`/`required_features` 等键。
  - 若实施 ④/⑤ → 对应键变为受限支持;
  - 其余未支持键 → warning(`--strict` 下报错,沿用现有 strict 通道),提示指向正确机制
    (workspace / features / profile)。

### ③ [P1·小] docs 增"Per-target / per-binary 构建配置"决策指引

- 在 `docs/05-mcpp-toml.md` 增一节(紧接 §2.2 targets),给三轴决策:
  - 多二进制不同配置(宏在各自代码)→ **workspace member**(docs/06);
  - 共享 lib 的可选能力 → **features**(§2.8,additive、按包、编一份);
  - 整构建模式 → **profile + `--profile`**(§2.9,build 与 ① 后的 test);
  - 并说明**为何不是 per-target cxxflags**(compile-once / BMI 一致性 / 与 Cargo `[[bin]]` 同款限制)。

### ④ [P2·中] 严格"入口/独占源作用域"的 per-target `cxxflags` / `defines`

- **解决**:场景 A 中"宏只碰各自 main"的便利——免拆 workspace。入口源 target 独占 →
  编一次只进一个 target → **无共享、无 compile-once 冲突**。
- **挂载点(现成)**:`plan.cppm:495-509` 已按 target 构造 `main_cu` 并灌 `packageCxxflags`。
  改动 = `Target` 加 `cxxflags`/`defines`(`manifest.cppm:55-60` + 解析 `:560-579`);
  构造 `main_cu` 时把 `t.cxxflags`(及 `defines` 脱糖成 `-D`)**追加**到 `main_cu.packageCxxflags`;
  ninja 边下发与 fingerprint **零额外改动**(复用 `unit_cxxflags` + compile-flags hash)。
- **铁律护栏**:
  - 只作用于该 target **独占源**(今日 = 入口 main);
  - 若标志会落到**被多 target 共享的对象** → **报错**,绝不静默半生效;
  - `defines` 设为一等键(可校验、跨工具链可移植);`cxxflags` 为裸标志逃生舱。
  - 文档明写"不穿透共享模块"。
- **局限(诚实标注)**:今日独占源仅入口 main;要覆盖"server 专属 helper 模块"需再上
  `[targets.X] sources=[...]`(per-target 源归属),规模更大,**本设计不含**。

### ⑤ [P2·小-中] `[targets.<name>] required_features = [...]`(借 Cargo 门禁)

- **是什么**:目标门禁——仅当所列 feature 全部激活时才构建该 target,否则跳过(不报错)。
  **只门控,不激活**(feature 仍靠 `--features`/`default`)。
- **解决**:① 可选工具/example/平台专属二进制(缩小默认构建面);② 场景 A 的**互斥变体**——
  `mcpp build --features server` 只出 server,且 `server` 宏(按包全局)**穿透共享 core**,
  client 被门禁挡掉;互斥变体分次构建是正确模型。
- **不解决**:同构建内两 target 不同配置(features 并集,撞 compile-once)。
- **改动**:`Target` 加 `requiredFeatures`(`manifest.cppm:55-60` + 解析);
  link-unit 循环(`plan.cppm:464`)按**激活 feature 集**(`prepare.cppm:2078-2152` 算出)过滤 target;
  **不碰 CompileUnit/fingerprint**——纯链接期选择,零变体成本。
- **与 ④ 互补**:④ = 一次构建多 bin、各自 main 上本地标志(到不了共享);
  ⑤ = 互斥变体分次构建、配置穿透共享 core。

### ⑥ [❌·大] 通用 per-target cxxflags over 共享池 / 变体分区 —— 不做

- 即 #131 字面诉求。代价:BMI 按 `(模块,变体)` 翻倍 + 沿 import 图传染整片子图。
- 需求已被 workspace(场景 A)+ features(共享可选开关)+ profile(整构建模式)+ ④/⑤ 覆盖。
- 与判据(§1)及 schema-ownership 铁律 C 冲突。**除非出现 workspace+feature 都解不掉的硬实例,否则不上。**

## 5. 实施清单

最小闭环 = **① + ② + ③**(全小改),即可让 #131 两场景都有规范、正确、今日可用的答案,且消灭静默坑。
④/⑤ 视产品是否要给"单目录多二进制"再加便利,可后续独立 PR。

- [ ] **①** `run_tests` 增收 `BuildOverrides`(`execute.cppm:406`);`cmd_test` 解析 `--profile/--features/--strict`(`cmd_build.cppm:66-71`)并透传
- [ ] **①** e2e:`mcpp test --profile <observe>` 下被测库以该 profile 编译(对照默认 enforce)
- [ ] **②** target 解析识别未支持键并 warning(`--strict` 报错),文案指向 workspace/features/profile(`manifest.cppm:560-579`)
- [ ] **②** 单测:`test_manifest` 覆盖 `[targets.x] cxxflags=[...]` 触发提示
- [ ] **③** `docs/05-mcpp-toml.md` 新增决策节;`docs/zh` 同步
- [ ] **④**(可选)`Target{cxxflags,defines}` + 解析;`main_cu` 追加 target flags(`plan.cppm:495-509`);共享源冲突时报错
- [ ] **④**(可选)e2e:两 bin 各带不同 `-D`,各自 main `#ifdef` 断言;验证共享对象不受影响
- [ ] **⑤**(可选)`Target{requiredFeatures}` + 解析;link-unit 按激活集过滤(`plan.cppm:464`)
- [ ] **⑤**(可选)e2e:`--features X` 只出对应 target

## 6. 验证

```bash
mcpp test                                   # 单测 + e2e 全过
mcpp build --profile contracts              # ① 前置:build 侧 profile 可用
mcpp test  --profile contracts              # ① 后:测试在 observe 下编(被测库受影响)
```

## 7. 设计原则沉淀

- **配置发散归编译单元(包),不归链接单元(target)**;target 只携独占、不影响共享一致性的本地标志。
- **可发散的(本地优化/独占标志)与必须一致的(ABI/链接/共享 BMI)划清界**——照抄 Cargo
  `[profile.*.package.*]` 允许 opt-level 却禁 panic/lto/rpath 的规矩。
- **包级变体收敛进 features**(additive、并集、按包编一份);跨构建选择用 `required_features` 门禁;
  整构建模式用 profile。三者皆不触碰 compile-once。
