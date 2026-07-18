# Issue #243 —— feature 依赖转发(`dep/feat`)设计

> 日期:2026-07-19
> 基线:mcpp **0.0.98**(HEAD;`MCPP_VERSION = "0.0.98"` @ `src/toolchain/fingerprint.cppm:21`)
> 范围:GitHub issue **#243** —— manifest `[features]` 的 (1) feature 条件依赖 与 (2) 依赖 feature **转发**(Cargo `dep/feat` 语法)。阻塞用例:opencv 模块包的 `opencv.dnn` 接口(一个 feature 既要拉入一个依赖、又要打开该依赖的某个 feature)。
> 所有 file:line 锚点均在 0.0.98 源码上核实。
> 上游总账:[2026-07-19-issues-230-243-batch-ledger-and-architecture-assessment.md](2026-07-19-issues-230-243-batch-ledger-and-architecture-assessment.md) §1(#243 行)/§3;架构主线沿用 [2026-07-18-issue-triage-215plus-architectural-remediation.md](2026-07-18-issue-triage-215plus-architectural-remediation.md) 与 [2026-07-18-v0.0.97-architectural-remediation-implementation-plan.md](2026-07-18-v0.0.97-architectural-remediation-implementation-plan.md):**把结构化意图保留到唯一收敛点(single funnel),不另开旁路**。

---

## 0. 结论先行

**#243 的口子比标题窄。** 核实现状后:

- **(1) feature 条件依赖 —— 已存在。** 两种文法都落到同一数据模型 `Manifest::featureDeps`(`src/manifest/types.cppm:386`,`map<featureName, map<depKey, DependencySpec>>`):
  - TOML 面:专用 `[feature-deps.<name>]` 段(`src/manifest/toml.cppm:699-710`)。
  - xpkg 面:`features.<name>.deps = { ... }`(`src/manifest/xpkg.cppm:1135-1161`)。
  - 消费点:`prepare.cppm:2101-2109` 的 `mergeActiveFeatureDeps`,在 root(`:2113-2124`)与每个 dep(`:2596-2600`)两处把**激活了的** feature 的 feature-deps 折进该包的 `dependencies`。
  - 单测已锁:`tests/unit/test_manifest.cpp:422`(`FeatureDepsTomlSection`)、`:445`(`SynthesizeFromXpkgLua, FeatureDepsAndImplies`)。
  - **故第 (1) 半基本无需新增代码**;唯一可议的是"要不要在 `[features]` 里内联 `deps=` 以与 xpkg 文法对齐"——见 §3 的裁决:**不加,`[feature-deps]` 保持 canonical**。

- **(2) feature 转发 `dep/feat` —— 完全不存在,是 #243 的真正工作量。** 当前 `[features]` 解析只读 `implies/defines/sources/requires/provides`(`toml.cppm:211-236`),xpkg 同理(`xpkg.cppm:1167-1173`);没有任何地方表达"本包 feature F 激活时,顺带激活依赖 D 的 feature `feat`"。而 dep 的请求 feature 集在**两处**各自独立推导且不自洽:
  - 解析期 `mergeActiveFeatureDeps` 用 `spec.features`(dep)/`rootReq`(root)——决定**拉哪些可选依赖**;
  - 激活期 `apply()`(`prepare.cppm:2673-2777`)对 dep 只从 `m->dependencies[pkg].features`**直接边**重新推导 `req`(`:2801-2806`),传递性 dep→dep 请求被丢弃(源码 `:2653` 注释明确:"Transitive dep→dep feature requests are not yet propagated")。

**因此 #243 的转发是"强制把 per-package 请求 feature 集收敛成唯一漏斗"的契机。** 本设计不在既有两处特判旁再加第三处,而是引入**一个** per-package 请求 feature 累加器 `requestedFeaturesByPkg`,由解析期填充(消费边 `spec.features` ∪ 转发 ∪ #242 的 default 策略),被 `mergeActiveFeatureDeps` 与 `apply()` **共同消费**——顺带把 `:2653` 的传递性缺口一并补上(不是本 issue 硬要求,但同一漏斗自然收敛)。

---

## 1. 目标语义与用例

opencv 模块包(源码直编形态)大致:

```toml
# compat.opencv 的 mcpp.toml(片段)
[features]
# dnn 接口:既要拉入 protobuf 依赖(条件依赖,已支持),
# 又要打开 opencv 内部的 dnn 源集(本地 feature),
# 还要打开 protobuf 的 "lite" feature(转发,#243 新增)
dnn = ["dnn-sources", "compat.protobuf/lite"]

[feature-deps.dnn]
"compat.protobuf" = "3.21.x"    # 条件依赖:dnn 激活才拉 protobuf(已支持)
```

期望:`mcpp build --features dnn`(或消费者 `opencv = { version="…", features=["dnn"] }`)时——

1. `dnn-sources`(本地 feature)按 implies 展开;
2. `compat.protobuf` 被拉入依赖图(feature-deps,已支持);
3. **`compat.protobuf` 以其 `lite` feature 激活参与构建**(转发,新增)——即 protobuf 的 `lite` 定义/源集在 protobuf 的 TU 里生效,而**不带** `--features dnn` 时 protobuf 根本不进图(或即便进图也不带 `lite`)。

转发必须**传递**(protobuf 的 `lite` 若又转发到 abseil 的某 feature,链条继续)且在 **`mcpp build` 与 `mcpp test` 双路径**上都成立(0.0.97 确立的 per-package 双路径不变量,见 batch ledger §0 "单一漏斗不变量")。

---

## 2. 面向用户的语法(surface syntax)裁决

### 2.1 候选与选型

| 方案 | 拼写 | 评价 |
|---|---|---|
| **A(推荐)** Cargo 平价,复用 implies 数组 | `[features]`\n`F = ["local-feat", "dep/feat"]` | 零新键、零新段;`dep/feat`(含 `/`)是转发,裸名是本地 implies。与 Cargo `[features] F = ["dep/feat", ...]` 完全一致,用户零学习成本。表格形亦可:`F = { implies=[...] }`,`/` token 混在 implies 里同样识别。 |
| B 专用段 | `[feature-forward.F]`\n`"dep" = ["feat"]` | 又开一段,与已有 `[feature-deps]` 割裂;`[features]` 依然要跨段读才知道 F 全貌。否决。 |
| C 表格形显式键 | `F = { implies=[...], forward=["dep/feat"] }` | 更"自解释",但与 Cargo 分叉,且 `forward` 值仍是 `dep/feat` 串——只是把 A 的 token 从 `implies` 挪到新键。作为 A 的**可选补充**保留(见 §2.2),不作主推。 |

**决策:采用 A。** `[features]` 数组(及表格形的 `implies`)里,**含 `/` 的 token = 转发 `<depKey>/<depFeature>`,不含 `/` = 本地 implied feature**。理由:

1. **Cargo 平价、非 workaround**(batch ledger §1 对 #243 的方案栏原话:"Feature 记录加 `forward`/`deps-features`,active 时把列出的 feature 注入对应依赖请求集后再跑 `feature_closure`")。
2. **closed syntax, open vocabulary**(0.0.97 全局约束):只加一个固定形状的解析分流(按 `/` 切分),不加开放语义键。
3. 与 `[feature-deps]` 分工清晰:`[feature-deps]` 说"F 激活拉哪个包"(需要完整 DependencySpec:version/path/git),`dep/feat` 说"F 激活给那个包开哪个 feature"(只需两个字符串)。二者正交、可组合(§1 用例即二者同用)。

### 2.2 `deps` 内联问题(第 (1) 半的语法统一)

xpkg 文法在 `features.<name>.deps` 内联条件依赖(`xpkg.cppm:1135`),TOML 文法用独立 `[feature-deps.<name>]`。**是否要在 TOML `[features]` 表格形也加 `deps=`?**

**裁决:不加。** 一个 feature-dep 需要完整 DependencySpec(version/path/git/rev/features/visibility),内联进 `[features]` 数组会撑爆"数组=轻量列表"的直觉;`[feature-deps.<name>]` 是一个正经的 dep 段,复用 `load_deps`(`toml.cppm:705`),表达力完整。二者**已经是"一数据模型(`featureDeps`)两文法"**,满足 batch ledger §5.2 的一致性检验点。文档层把 `[feature-deps.<name>]` 记为 canonical TOML 面,`features.x.deps` 记为其 xpkg 文法等价物即可。**#243 不动这一半。**

---

## 3. 数据模型

### 3.1 新增字段(唯一)

`src/manifest/types.cppm`(紧邻 `featuresMap:370` / `featureDeps:386`,同族)新增:

```cpp
// Feature System v2 —— 依赖 feature 转发(Cargo `dep/feat` 平价)。
// 本包 feature F 激活时,把 <depFeature> 注入依赖 <depKey> 的请求 feature 集,
// 在该 dep 跑 feature_closure 之前。depKey 与 dependencies / featureDeps 同键
// (resolve_dependency_selector 的 stableMapKey)。转发是"加法":只增开
// 依赖的 feature,不改依赖是否被拉入(那是 featureDeps 的职责)。
std::map<std::string, std::vector<std::pair<std::string, std::string>>>
    featureForwards;   // featureName → [(depKey, depFeature)]
```

**不新增** `DependencySpec` 字段、**不新增**段。转发目标 depKey 复用 `dependencies` / `featureDeps` 的键空间(`resolve_dependency_selector(...).stableMapKey`,见 `xpkg.cppm:1150-1157` 的既有用法),使"转发到某 dep"与"依赖某 dep"天然对齐。

### 3.2 与既有 feature 数据的关系(single funnel)

```
[features] / xpkg features            →  featuresMap(implies)
[features].defines / xpkg defines     →  buildConfig.featureDefines
[features].sources / xpkg sources     →  buildConfig.featureSources
[features].requires/provides          →  featureRequires / featureProvides
[feature-deps.X] / xpkg features.X.deps  →  featureDeps            ← 已有(条件依赖)
[features] 数组内 "dep/feat" / xpkg 同  →  featureForwards        ← 新增(转发)
```

六张表都 keyed by featureName,由**同一个** `feature_closure`(`prepare.cppm:407-423`)判定"哪些 feature 激活",再各自消费。转发不引入平行的 feature 判定,只是**多一张激活后要消费的表**。

---

## 4. 解析改动(两文法,共用切分)

### 4.1 一个共享 helper

在 `prepare.cppm` 附近或 manifest 层加一个纯函数(供两文法与校验共用):

```cpp
// "compat.protobuf/lite" → {depKey:"compat.protobuf", depFeature:"lite"}
// 无 '/' → nullopt(调用方按本地 implied 处理)。depKey 走
// resolve_dependency_selector 归一到 stableMapKey,与 dependencies 同键。
std::optional<std::pair<std::string,std::string>> split_forward_token(std::string_view);
```

按**首个** `/` 切分(feature 名不含 `/`;depKey 可含 `.` 命名空间但不含 `/`)。depKey 侧过 `resolve_dependency_selector(..., OmittedMcpplibsPriority)`(同 `xpkg.cppm:1150`)归一,保证 `protobuf` 与 `compat.protobuf` 指向同一键。

### 4.2 TOML(`src/manifest/toml.cppm:211-236`)

`[features]` 循环里,无论数组形(`:213-215`)还是表格形的 `implies`(`:218`),把收进 `implied` 的每个 token 过 `split_forward_token`:命中 `/` → `m.featureForwards[fname].push_back(...)`,否则 → 原样进 `featuresMap[fname]`(implies)。**其余键(defines/sources/requires/provides)不变。**

### 4.3 xpkg(`src/manifest/xpkg.cppm:1167-1173`)

`sub == "implies"` 分支(`:1168`)当前直接把字符串塞进 `&m.featuresMap[fname]`。改为逐 token 过 `split_forward_token`,同 §4.2 分流。**`deps` 分支(`:1135-1161`)不变**(那是 featureDeps)。

> 两文法只共享 `split_forward_token` 一个切分点,零重复逻辑——满足 batch ledger §5.2"一数据模型多文法"。

---

## 5. 传播算法与注入点

### 5.1 现状的两处"各自推导"(问题根)

| 阶段 | 位置 | 用什么当 dep 的请求集 | 缺陷 |
|---|---|---|---|
| 解析期 拉可选依赖 | `mergeActiveFeatureDeps` @ `prepare.cppm:2600` | `spec.features`(该 dep 的消费边) | 只认单条消费边;转发无处注入 |
| 激活期 发 `-D`/源集 | `apply()` @ `prepare.cppm:2801-2806` | 从 `m->dependencies` **直接边**重推 | 传递 dep→dep 丢失(`:2653`);转发无从体现 |

两处对"dep 的请求 feature 集"各算一遍且不一致——这正是要消除的"散在多处特判"。

### 5.2 唯一漏斗:`requestedFeaturesByPkg`

引入一个 per-resolved-package 累加器(keyed by `packages[]` 下标,与 `dependencyEdges` 的 `consumer/dependencyPackageIndex` 同坐标,见 `:2864`):

```cpp
std::map<std::size_t /*pkgIndex*/, std::vector<std::string>> requestedFeaturesByPkg;
```

**填充规则(加法、单调增):**

1. **root**(`packages[0]`):`requestedFeaturesByPkg[0] = rootReq`(`parse_feature_request(overrides.features)`,`:2779`;#242 的 `default-features` 只影响 default seed,不影响这里)。
2. **每条消费边 P→D**(`recordDependencyEdge` @ `:2617`):把该边 `spec.features` 并入 `requestedFeaturesByPkg[depIdx]`(去重)。**这一步顺带补上 `:2653` 的传递性缺口**——D 不再只认 root 直接边。
3. **转发**:对每个包 P,`active(P) = feature_closure(P.manifest, requestedFeaturesByPkg[Pidx])`;对每个激活 feature F、每条 `P.featureForwards[F] = (Dkey, feat)`、每条 P→D 边(D 的 canonical 名匹配 Dkey),把 `feat` 并入 `requestedFeaturesByPkg[Didx]`。

规则 3 依赖 P 的 active 集,而 P 的 active 集又可能因**上游对 P 的转发**而增长 → **不动点迭代**:重复规则 2+3 直到无累加器增长。终止性:feature 集单调增且有限(全集 = 所有包所有 feature),故收敛;环(A/f→B/g→A/f)不会死循环——集合饱和即停,`feature_closure` 自身的 `seen`(`:414/417`)另挡 implies 环。

### 5.3 与解析期"拉可选依赖"的次序耦合

难点:转发的 `feat` 可能触发 **D 自身的 feature-deps**(D 的 `[feature-deps.feat]` 再拉一个包 E),而累加器的不动点若在**整图建完后**才跑,E 就漏拉了。

**方案:把转发注入**并进**既有 worklist 的增量展开**,而非事后独立一遍。具体在 dep 处理块(`prepare.cppm:2596-2643`):

1. `:2600` `mergeActiveFeatureDeps(*dep_manifest, requestedFeaturesByPkg[depIdx])` —— 把入参从 `spec.features` 换成**累加器**(已并入所有已知消费边 + 已知转发)。D 的 feature-deps 据此折进 `dep_manifest->dependencies`。
2. 紧接在把 D 的子依赖 push 进 worklist(`:2640-2643`)**之前**,插入转发注入:
   `active(D) = feature_closure(*dep_manifest, requestedFeaturesByPkg[depIdx])`;对每个激活 feature F、每条 `dep_manifest->featureForwards[F] = (childKey, feat)`,把 `feat` 追加到 `dep_manifest->dependencies[childKey].features`(即将 push 的 `child_spec`)。
3. 于是 child 被 push 时其 `spec.features` 已含转发的 `feat`;child 处理时其 `mergeActiveFeatureDeps` 用到的累加器(由规则 2 从这条边并入)也含 `feat` → child 的 feature-deps / 再转发递归展开。**传递性天然成立(树形 BFS 前向即够)。**

root 侧同构:在 `:2113-2124` 现有块内,`mergeActiveFeatureDeps(*m, rootReq)` 之后、seed worklist(`:2129`)之前,按 `m->featureForwards` 对 root 的直接依赖 `m->dependencies[childKey].features` 注入转发。

**菱形(D 有 ≥2 消费者,第二消费者晚于 D 首次 resolve 才加 feature)**:与既有 feature-deps / 传递请求**同一历史局限**。补法:当某条**晚到的**消费边或转发使 `requestedFeaturesByPkg[Didx]` **增长**且 D 已 resolve,则把 D **重新入队**做一次 feature-deps + 转发再展开(去重守卫:仅当累加器真增长才重入队 → 单调有限 → 终止)。菱形非 opencv.dnn 的 MVP 必需(dnn 是 root→opencv→protobuf 的树链),但漏斗设计天然容纳,建议实现时一并做以彻底消除 `:2653` 局限。

### 5.4 激活期消费累加器(消除第二处推导)

`apply()` 的调用点改为读累加器,而非重推:

- root:`apply(packages[0], requestedFeaturesByPkg[0])`(替 `:2798` 的 `rootReq`,值相同)。
- dep:`apply(packages[i], requestedFeaturesByPkg[i])`(替 `:2801-2806` 从 `m->dependencies` 重推的 `req`)。

`apply()` 内部(`:2673-2777`)一字不改:它对每个激活 feature 发 `-DMCPP_FEATURE_<F>`、并入 `featureDefines`(`:2698-2717`,含 Public/Interface 传播)、按 `featureSources` drop/add(`:2740-2776`)。转发的 `feat` 此刻已在 D 的累加器里 → D 的 `apply()` 自然激活 `feat`,其 defines/源集照常生效。**转发不需要在 `apply()` 里加任何转发专属分支**——它只是让 D 的请求集"多了 feat",复用全部既有激活机制。

> 双路径不变量:`apply()` 的 drop 是 `!includeDevDeps` 门(`:2742`),add 在两模式都跑(`:2731-2739` 的 eigen_blas 教训)。累加器与注入点都在 `apply()` 之前、与 `includeDevDeps` 无关,故 `mcpp build` 与 `mcpp test` 走同一累加器 → 转发在双路径一致(满足 batch ledger §0 单一漏斗不变量)。build.mcpp 环境契约(`:2852` `bpEnv.features = feature_closure(pkg.manifest, req)`)同样应改读累加器,使 dep 的 build.mcpp 看到转发进来的 feature。

---

## 6. 与 #242(consumer `default-features = false`)的组合

#242(batch ledger §1 / 待实施)加 `DependencySpec.defaultFeatures`(默认 `true`),穿进 `feature_closure`:为 `false` 时**跳过 default seed**(`prepare.cppm:411-412` 的 `pm.featuresMap["default"]` 注入)。

**组合规则(转发是加法,default-features 是减法,二者正交):**

- `feature_closure(pm, requested)` 的输入 = `(default seed if defaultFeatures) ∪ requested`。
- 转发注入到的是 **`requested`**(累加器 / `spec.features`),属于"显式请求",**不受** default-features 开关影响。
- 故 `dep = { version="…", default-features = false, features=[] }` 且上游转发 `dep/lite`:dep 的 `requested = {lite}`,default seed 被跳过 → **dep 只带 lite(+lite 的 implies),不带 dep 的默认 feature 集**。这正是 opencv"精简依赖"想要的:关掉 protobuf 默认,只按 dnn 需要打开 `lite`。
- 反之 default-features 默认 `true` 时:`requested(含转发) ∪ default`,转发的 feature 叠加在默认集之上。

一句话:**转发决定"额外开哪些 dep feature",default-features 决定"要不要 dep 的默认 feature 集";前者进 `requested`,后者门控 `default` seed,在 `feature_closure` 里并集,互不覆盖。** 实现次序上建议 #242 先落(它定义 `DependencySpec.defaultFeatures` 与 `feature_closure` 的 seed 开关),#243 在其上叠转发;若并行,两者都只在 `feature_closure` 入口与 DependencySpec 交汇,冲突面小。

---

## 7. 边界情形

| 情形 | 处理 |
|---|---|
| 转发到**未声明**的 dep(`F=["ghost/x"]` 但 `ghost` 不在 dependencies∪featureDeps) | F 激活时校验:depKey 在图中无匹配包 → **strict 报错 / 非 strict warning**,复用 `:2792-2797` / `:2807-2815` 既有 strict/warn 模式。F 未激活则不校验(与 `unknown_requested` 同惰性)。 |
| 转发到**可选/feature 激活**的 dep(dep 仅由某 feature-dep 拉入) | 允许。次序:`mergeActiveFeatureDeps`(`:2600`)先把 feature-dep 折进 `dependencies`,转发注入(§5.3 步骤 2)在其后 → dep 在场即注入。若拉入该 dep 的 feature-dep **未激活**(dep 缺席)而转发仍指向它 → 归入"转发到未声明 dep",warn。 |
| 转发一个 dep **未定义**的 feature(`protobuf/nonesuch`) | 转发的 `feat` 经累加器进 `spec.features`,被**既有**的 "dependency does not declare requested feature" 门(`:2807-2815`)校验 → strict 报错 / warn。**无需新增校验路径**(复用漏斗的红利:转发 feature 与显式 `features=[...]` 走同一校验)。 |
| 转发目标 feature 自身再转发(传递) | §5.3 步骤 3 递归覆盖;累加器不动点(§5.2)兜菱形。 |
| 环(A/f→B/g→A/f) | 累加器单调饱和即停(§5.2 终止性);`feature_closure` 的 `seen` 挡 implies 环。 |
| 同一 dep 被多条转发/消费边给不同 feature(菱形并集) | 累加器按包去重并集(§5.2 规则 2/3);增长则重入队(§5.3 菱形补法)。 |
| depKey 命名空间歧义(`protobuf` vs `compat.protobuf`) | `split_forward_token` 走 `resolve_dependency_selector` 归一到 stableMapKey,与 `dependencies` 同键匹配(同 `xpkg.cppm:1150-1157`)。 |

---

## 8. 测试计划

**单测(解析,加进现有 `tests/unit/test_manifest.cpp`,勿新建工程):**

- `TEST(Manifest, FeatureForwardTomlParse)`:`[features]\nF = ["local", "compat.protobuf/lite"]` → `m.featuresMap["F"] == {"local"}` 且 `m.featureForwards["F"] == {{"compat.protobuf","lite"}}`(校验 `/` 分流、命名空间归一)。
- `TEST(Manifest, FeatureForwardTableForm)`:表格形 `F = { implies = ["a", "dep/x"] }` 同样分流。
- `TEST(SynthesizeFromXpkgLua, FeatureForwardParse)`:xpkg `features.F.implies={"dep/x"}` → `featureForwards`(与 `FeatureDepsAndImplies:445` 并列)。
- 负例(可 e2e 承接):转发未声明 dep → strict 报错串。

**e2e(host-aware,编号续 `tests/e2e/` 现有末号 123 → 从 124 起,并入 `run_all.sh`):**

- 夹具:项目 `P` 依赖 `D`;`D` 有 feature `extra`,其 `[features].extra.defines = ["D_EXTRA=1"]`(或 `featureSources` 一个仅 extra 才编、导出符号的 `.cpp`)。`P` 的 `[features]`:`F = ["D/extra"]`,`P` 的 TU `#ifdef` 探测 `D_EXTRA`(或链接 D 的 extra 符号)。
  - `mcpp build`(不带 `--features F`)→ `D_EXTRA` 缺席 / 符号未定义(断言其**不出现**)。
  - `mcpp build --features F` → D 带 extra 编译,`D_EXTRA` 在场 / 符号解析,构建+运行通过。
  - **双路径**:同一断言在 `mcpp test` 复跑一遍(锁 0.0.97 双路径不变量)。
- 传递变体(可选,验证链条 + 菱形补法):`D/extra` 再转发 `E/y`,断言 `E` 的 y 定义仅在 `--features F` 下在场。
- 组合 #242 变体(若 #242 已落):`D = { …, default-features=false, features=[] }` + `F=["D/extra"]` → 仅 extra 在场,D 默认 feature 集**不**在场。

---

## 9. Commit / PR 计划

**这是独立后续 PR,非 0.0.98(HEAD)内容。** 建议目标 **0.0.99**(或 #237/#241/#242 机械修的同一后续批次,见 batch ledger §3)。

- **依赖次序**:建议 **#242 先落**(定义 `DependencySpec.defaultFeatures` 与 `feature_closure` 的 default-seed 开关),#243 叠其上;二者若并行,交汇点仅 `feature_closure` 入口 + DependencySpec,冲突面小,但 §6 的组合语义须在 #243 的 e2e 显式覆盖。
- **commit 拆分(单 PR,逐节点绿)**:
  1. 数据模型 + 两文法解析 + `split_forward_token`(`types.cppm` / `toml.cppm` / `xpkg.cppm`)+ 解析单测。
  2. 漏斗重构:`requestedFeaturesByPkg` 累加器 + `mergeActiveFeatureDeps` / `apply()` / build.mcpp env 三个消费点改读累加器 + 转发注入(§5.3/5.4)+ 菱形重入队(顺带闭 `:2653`)+ 校验复用。
  3. e2e 124(+ 传递/组合变体)并入 `run_all.sh`;host-aware。
  4. 版本 commit `0.0.99`(`fingerprint.cppm:21` + `mcpp.toml` + CHANGELOG,**仅末尾改版本号**)。
- **每 commit 不变量**:`mcpp build`(self-host)+ `mcpp test` 全绿;涉行为 commit 附 e2e;单测就近加进 `test_manifest.cpp`。
- **架构验收(batch ledger §5.2)**:交付后 manifest 文法 / xpkg 描述符 / feature 模型三处对"转发"须收敛为**一数据模型(`featureForwards`)多文法**,per-package 请求 feature 集收敛为**唯一漏斗** `requestedFeaturesByPkg`,不得再分叉出平行实现;`prepare.cppm:2653` 的传递性 TODO 应随本 PR 消除。

---

## 附:核实锚点清单(0.0.98)

- 版本真源:`src/toolchain/fingerprint.cppm:21`(`MCPP_VERSION = "0.0.98"`)。
- 条件依赖已存在:`toml.cppm:699-710`(`[feature-deps.<name>]`)、`xpkg.cppm:1135-1161`(`features.X.deps`)、`types.cppm:386`(`featureDeps`)、`prepare.cppm:2101-2109/2113-2124/2596-2600`(`mergeActiveFeatureDeps` + 两调用点)、`tests/unit/test_manifest.cpp:422/445`。
- `[features]` 解析(转发要挂入):`toml.cppm:196-236`(数组/表格形,implies/defines/sources/requires/provides)、`xpkg.cppm:1108-1195`(同,implies 分支 `:1167-1173`)。
- feature 判定唯一实现:`prepare.cppm:407-423`(`feature_closure`)、`:426-436`(`parse_feature_request`)。
- 激活期两处独立推导(要收敛):`prepare.cppm:2673-2777`(`apply`)、`:2798`(root)、`:2801-2806`(dep,直接边重推)、`:2653`(传递性 TODO 注释)、`:2807-2815`(dep feature 未声明校验,转发复用)。
- DependencySpec:`src/pm/dep_spec.cppm:28-51`(`features` @ `:42`;#242 将加 `defaultFeatures`)。
- dep-spec 键白名单(#242 将加 `default-features`):`toml.cppm:423-428`;inline 填充 `:437-477`(features @ `:455-458`,backend 糖 `:461-462`)。
- 消费边/依赖边坐标(累加器 keyed by):`prepare.cppm:2615-2617`(`packages.push_back` / `recordDependencyEdge`)、`:2864`(`dependencyEdges` 遍历)。
- build.mcpp 环境契约(应读累加器):`prepare.cppm:2852`。
