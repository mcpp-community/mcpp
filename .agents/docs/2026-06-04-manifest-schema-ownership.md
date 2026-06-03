# mcpp.toml Schema 所有权设计:语法封闭 · 词汇开放

> 2026-06-04 · 状态: 设计定稿待实施 · 代码锚点基于 main@3a8a3d4 (v0.0.47)
> 关联: agentdocs/2026-06-03-capability-architecture-rfc.md(能力架构 RFC)
>      mcpp-index/.agents/docs/2026-06-03-capability-runtime-metadata.md(index 侧 schema)

## 0. 问题

v0.0.47 引入了一批"三档旋钮"字段(`backend=`、`[runtime.<cap>] provider=`、
`[package] platforms`、`[features]`、`[profile.*]`),但**字段归属是临时混合的**:
`backend` 把 imgui 域词汇泄漏进 mcpp 语法层;provider 校验语义偏松("声明了该
capability 的包都算 provider");platforms/profiles 缺校验与逃生口;且全部
**未写进用户文档**(docs/05-mcpp-toml.md 截至 2.7 节)。本设计统一定型。

## 1. 设计原则(判定法)

> **语法封闭,词汇开放(closed grammar, open vocabulary):
> 谁拥有"解析语义"谁定义键;谁拥有"领域知识"谁定义值。**

三条铁律:

- **A. mcpp 只定义机制,不枚举领域词汇。** features 的并集/闭包、capability 的
  require/provide/override、profile→编译器旗标、platform→triple,这些解析语义
  归 mcpp,键与形状固定。feature 名、capability 名、后端名是领域知识——只出现在
  **值**里,绝不出现在 mcpp 代码中(与 doctor 去 #ifdef 同一原则在 schema 层的体现)。
- **B. 不允许包自定义 toml 键。** 键合法性若取决于"先解析目标包",manifest 即丧失
  静态可解析性,lockfile/LSP/审计/why 全部层次倒置(Cargo 十年验证的取舍)。
  包的扩展点 = **固定机制内的开放值域**,不是新键。
- **C. 包级旋钮统一收敛进 features;糖键入核仅当:① 领域中立(跨生态通用模式)
  ② 1:1 脱糖、零新增解析语义。**

## 2. 现状代码盘点(main@3a8a3d4)

| 机制 | 代码锚点 | 现状归属 |
|---|---|---|
| dep-spec 键白名单(含 `backend`) | `src/manifest.cppm:593-597 is_dep_spec_key` | 键 mcpp 固定 |
| `backend=` 脱糖 → `backend-<x>` feature | `src/manifest.cppm:631` | 纯糖,值开放 ✅ |
| `DependencySpec.features` | `src/pm/dep_spec.cppm`(features 字段) | 机制固定,值开放 ✅ |
| `[features]` 解析 | `src/manifest.cppm:213 featuresMap` / `:521` | 机制固定,值开放 ✅ |
| feature 激活(default∪requested,闭包,`-DMCPP_FEATURE_*`) | `src/cli.cppm:2969` | mcpp 固定 ✅ |
| `[runtime.<cap>] provider=` | `src/manifest.cppm:121 providerOverrides` / `:897`;应用+校验 `src/cli.cppm:3294-3310` | 形状固定,cap 名开放 ✅;**provider 语义偏松 ⚠️** |
| `[package] platforms` | `src/manifest.cppm:40`;`why` 展示 | 键固定;**值未校验 ⚠️** |
| `[profile.<name>]` | `src/manifest.cppm:188 Profile`;`src/build/flags.cppm` 应用 | 键固定(编译器域);**无 passthrough ⚠️** |
| capability 聚合(`CapabilityProvider`) | `src/build/plan.cppm:72-76` | mcpp 固定 ✅ |
| index 侧能力声明 | mcpp-index `compat.glfw.lua` `runtime.capabilities`(含 `abi:glibc`) | 值由生态定义 ✅;**无 provides= ⚠️** |

## 3. 设计决策(逐项定型)

### D1. `backend=` —— 定型为"通用约定糖",保留
- 理由:「库的多个可替换实现」是跨生态通用模式(GUI 后端 / DB driver / logger sink),
  满足铁律 C 两条件(领域中立 + 已是 1:1 脱糖)。
- 定型内容:
  1. 键名维持 `backend`(评估过 `impl`,`backend` 语感更明确;不再增设别名)。
  2. **约定写入文档**:库支持该旋钮 ⇔ 声明 `[features] backend-<name>` 系列
     (mcpp 不校验后端名本身,名字归库)。
  3. **strict 校验**(新增):若目标包的 `featuresMap` 已声明任何 `backend-*`
     feature,而请求的 `backend-<x>` 不在其中 → warning(`--strict` 下 error)。
     目标包零声明时不校验(允许纯 define 用法)。
- 不做:包注册自定义键(违反铁律 B)。

### D2. `provider=` —— 收紧为显式 `provides=` 语义
- index/包侧新增 `provides = ["opengl.glx.driver", ...]`(lua `mcpp` 段 +
  mcpp.toml `[runtime] provides`),声明"我兑现这些能力"。
- core 解析进 `RuntimeConfig.provides`;`plan.runtimeProviders` 改为:
  capability→provider 的映射**优先取 provides 声明者**;无任何 provides 时
  回退现状(声明 capabilities 者视为弱 provider,向后兼容)。
- `provider=` 覆盖校验升级:目标包必须 provides(或弱提供)该能力,否则 warning 照旧。
- capability 命名规范(文档化,不进代码):分层小写 `domain.sub.role`
  (`opengl.glx.driver`、`x11.display`)+ 前缀类 `abi:<name>`;mcpp-index 维护
  "知名能力名注册表"文档供 doctor/why 提示,**mcpp 代码不枚举**。

### D3. `platforms` —— 固定词表 + 校验
- 值域归 mcpp(它拥有 triple 体系):`linux | macos | windows`(后续随 target
  支持扩展)。解析时非法值 → warning(`--strict` 下 error)。

### D4. `[features]` —— strict 校验 + 传递传播(follow-up)
- strict 校验:dep spec 请求的 feature 不在目标包 `featuresMap` 且目标包**有**
  `[features]` 表时 → warning(`--strict` error);无表时不校验(纯 define 用法)。
- dep→dep 传递 feature 请求:已知缺口,独立 follow-up(影响解析器,本设计不展开)。

### D5. `[profile.<name>]` —— 固定键 + passthrough 逃生口
- 新增开放值域(固定形状内):`cflags = [...]`、`cxxflags = [...]`、`ldflags = [...]`,
  追加在 profile 解析后(I6 完备性;铁律 A 不破——键仍是 mcpp 的,值是用户的)。

### D6. Schema 所有权规范成文
- 本文件 §1 的判定法写入用户文档(见 P5),作为后续任何新字段的准入标准:
  新键须回答"解析语义归谁?能否用 features/capability 表达?是否领域中立纯糖?"

## 4. 实施计划(每阶段遵循:本地 build+test+e2e → 小步 commit → 分支 PR → 三平台 CI 绿 → squash 合入)

### P1 `provides=`(D2,跨 mcpp + mcpp-index)
- mcpp:`src/manifest.cppm` RuntimeConfig 增 `provides` + TOML/Lua 双解析
  (TOML `[runtime] provides`;Lua `mcpp.runtime.provides`);
  `src/build/plan.cppm` 聚合优先级(provides > capabilities 弱提供);
  `src/cli.cppm` provider 覆盖校验/why/doctor/resolution.json 同步展示 provides 来源。
- mcpp-index:`compat.glx-runtime.lua` 增 `provides = {"opengl.glx.driver"}`(PR);
  schema 文档同步。
- 验证:imgui 消费链 why/doctor 显示 provider=compat.glx-runtime(由 provides 而非弱提供);
  e2e 新增 `66_runtime_provides.sh`。

### P2 `backend=` 定型 + features strict(D1+D4)
- `src/cli.cppm` feature 激活处增 strict 校验(warning;`--strict` 全局 flag → error);
  `mcpp build --strict` 选项接入 BuildOverrides。
- 验证:imgui 0.0.3(声明 backend-* 前后)+ 故意写错 backend 名 → warning/error;
  e2e `67_features_strict.sh`。

### P3 `[profile.*]` passthrough(D5)
- `src/manifest.cppm` Profile 增三个 list 字段 + 解析;`src/build/flags.cppm`
  在 profile 应用处追加。
- 验证:`[profile.dist] cflags=["-fno-plt"]` 经 `--verbose` 可见;e2e `68_profile_passthrough.sh`。

### P4 `platforms` 校验(D3)
- `src/manifest.cppm` 解析处校验词表 → warning/strict-error。
- 验证:非法值 warning;e2e 并入 67。

### P5 **文档定型(必做收尾)** —— `docs/05-mcpp-toml.md`
- 新增章节(承接现有 2.7 编号):
  - **2.8 `[features]`**:语法、default 集、隐含闭包、`--features`、dep `features=[]`、
    `-DMCPP_FEATURE_<NAME>` 约定、strict 行为;
  - **2.9 `[profile.<name>]`**:内置 release/dev/dist、opt/debug/lto/strip、
    passthrough cflags/cxxflags/ldflags、`--profile`;
  - **2.10 `[runtime]`**:library_dirs/dlopen_libs/capabilities/**provides**、
    `[runtime.<capability>] provider=` 覆盖语义与校验;
  - **2.1 增补**:`[package] platforms`(词表 + CI 矩阵提示 + `why` 展示);
  - **2.5 增补**:dep spec `backend = "<impl>"` 糖(脱糖规则 + `backend-*` feature 约定);
  - **新增附录「Schema 所有权原则」**:§1 判定法 + 字段归属总表(给后续贡献者的准入标准)。
- 同步:`docs/03-toolchains.md` 提及 abi 能力强制;`docs/00-getting-started.md`
  提及 `new --template gui` 与 `why/doctor`。
- 验证:文档内示例逐个真实跑通后才提交(文档中的每段 toml 必须可构建)。

### P6 e2e 收口
- 上述 66/67/68 纳入 `tests/e2e/`(`run_all.sh` 自动发现);linux self-host CI 全量跑。

## 5. 验收
- [ ] provides= 端到端(index 声明 → why/doctor/resolution.json 显示 → 覆盖校验收紧)
- [ ] backend=/features strict 行为(warning 与 --strict error)
- [ ] profiles passthrough 旗标可观察
- [ ] platforms 非法值告警
- [ ] docs/05-mcpp-toml.md 含 2.8–2.10、2.1/2.5 增补、所有权附录,且所有示例可构建
- [ ] 新 e2e 全过,三平台 CI 绿
