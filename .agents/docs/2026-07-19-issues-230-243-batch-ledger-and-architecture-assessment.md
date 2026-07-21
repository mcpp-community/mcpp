# #230–#243 批次总账 + 架构评估(治理文档)

> 日期:2026-07-19 · 基线:mcpp **0.0.98**(HEAD,branch `fix/object-path-239-240`)
> 目的:本文件是 issue **#230–#243** 这一批次的**唯一权威总账**——每个 issue 的分类 / 状态 / 根因子系统 / 已核实 file:line 锚点 / **根因级(非 workaround)修复方案** / 对应 commit·PR / 验证方式,一格不漏。
> **交付纪律(用户指令,强约束)**:
> 1. **不做 workaround 级别的修复**——每个修复改的是**根因收敛点**,不加旁路开关 / 特判豁免。凡是只能在 mcpp 仓外(xlings)根治的,必须**如实标注**,不得用 mcpp 侧 band-aid 冒充"已修"。
> 2. 批次**全部完成后**,必须补一份**整体架构评估**(§4:设计合理性 / 稳定性 / 是否留下架构债),记录在本文件。
> 3. **所有相关修复 / 实现都必须清楚对应到 issue**(§1 总账即此纪律的落地)。

---

## 0. 建立在 0.0.97 已确立的架构主线之上(不重复推导)

本批次全部沿用 0.0.97 两份设计文档确立的架构原则(见
`.agents/docs/2026-07-18-issue-triage-215plus-architectural-remediation.md` /
`.agents/docs/2026-07-18-v0.0.97-architectural-remediation-implementation-plan.md`):

- **主线诊断**:mcpp 反复"过早把结构化意图压平成字符串 / 把特判散落各处",再在下游补救。**每个修复都要把意图保留到单一汇聚点(choke point / funnel),而不是再加一个特判。**
- **单一漏斗不变量**:per-package 的有效 source / feature / cfg 求值必须在**所有**构建路径(root + version-dep + path/git-dep)且 **`mcpp build` 与 `mcpp test` 双路径**上评估(#218/#229 的教训)。#242/#243 必须**扩展**这个已有漏斗(`prepare.cppm` 的 per-package feature loop),不能另开旁路。
- **封闭文法要"响亮失败"**:0.0.97 的 `[[x]]`-非白名单硬报错立了先例;#237 是它到 xpkg `mcpp` 段构建路径的直接延伸。
- **工具 / 资源供给走单一同步"边前供给"门**(`Fetcher::resolve_xpkg_path(..., autoInstall=true)`:先刷索引 → 阻塞安装 → 校验 payload → 硬报错)。#238(xlings install 路径)、#241(依赖 payload 可用性)在此框架内。
- **命名空间路由是表查找,不是硬编码短路**(R6 移除默认命名空间→内建索引的两处硬编码)。#238 恰恰在"root `[indices]` 继承(#224)× 多仓"这个**0.0.97 预期终态**上暴露。
- **标准警示**:0.0.97 自身的 C3 `shell_quote_arg` 过度引号回归,只在合并后全量 e2e + 多平台 CI 才现形(逐 commit 单测没抓到)。**跨构建 / 共享库运行期破坏会躲过 per-commit 单测**——本批次同样适用(本次 #239 的 ninja `@`-引号 × #235 depfile 就是同类交互 bug)。

---

## 1. 批次总账(#230–#243,一格不漏)

图例:状态 ✅已发布 / 🟢已修待发(本地 branch)/ 🔧机械根因修(可实施)/ 📐需设计 / ⛔根因在仓外 / 🔒已修待关闭。
"根因级"= 改的是根因收敛点;凡标 workaround 者必须给出为何暂无根因路径。

| # | 类型 | 状态 | 根因子系统 | 已核实锚点 | 根因级修复方案 | commit / PR |
|---|------|------|-----------|-----------|---------------|-------------|
| **#230** | bug(win) | 🔒 已修(0.0.96) | scanner glob 走 symlink 逃逸 + 窄串转换抛异常→`__fastfail`→裸 127 | `scanner.cppm` symlink 守卫 + `.mcpp` prune;`src/main.cpp` 兜底 catch 返回 70 | 已根治(`df985df` / #231)。**动作:mcpp-index windows CI pin ≥0.0.96 复验后关闭** | 已发布 0.0.96 |
| **#237** | 诊断缺口 | ✅ 已修待发 | xpkg 描述符 `mcpp` 段未知键在 **build 路径**静默丢弃(`xpkg parse` 才硬报错) | `xpkg.cppm:1324-1333`(else 分支入 `xpkgUnknownKeys`,build 不消费);消费点仅 `cmd_xpkg.cppm:122/186`;字段 `types.cppm:351` | build 时消费 `xpkgUnknownKeys` → 响亮告警(前向兼容,非硬错)+ `closest_known_xpkg_key`(封闭词表别名 + Levenshtein 回退)did-you-mean;在 `prepare.cppm` 描述符采纳点(funnel)发。单测 `XpkgUnknownKeys.CollectedAndDidYouMean` | `c20520b`(0.0.98) |
| **#238** | bug | ⛔ **根因在 xlings**(mcpp 诊断✅) | xlings `install_packages` 多 index_repos(≥2)解析静默 exit 1 | mcpp 侧只:写多仓 `xlings.cppm:1109-1117`、shell-out `:1016/1022`、吞失败 `package_fetcher.cppm:343`(NDJSON 无 `error` 事件 `progress.cppm:23`);触发链 = #224 root `[indices]` 继承 | mcpp 侧**诊断已落地**(`00b66c5`):`read_seeded_index_repos`(空格容错解析,修了复用 compact `extract_string` 的假 0 仓 bug)+ `format_install_failure_diagnostic` 点名目标/仓清单/`≥2` 缺口提示,`captured_error()` 保留子进程输出,`MCPP_VERBOSE` 记原始调用。单测 `PmPackageFetcher.*`。**根因仍须落 xlings**:已提交 **openxlings/xlings#374** | mcpp 诊断:`00b66c5`(0.0.98);根因:**openxlings/xlings#374** |
| **#239** | bug | 🟢 已修待发 | #233 消歧对越根/绝对 relPath 逃逸 `obj/` + `@` 撞 ninja 引号×#235 depfile | `plan.cppm` 消歧前缀 | `safe_object_prefix` 逐分量净化(去根/`.`丢/`..`→`__up`/非可移植→`_`),逐分量单射保 #233 唯一性 | `b7f32f6`(本 branch,0.0.98) |
| **#240** | bug | 🟢 已修待发 | #233 消歧后 entry-main link 输入用陈旧扁平 `obj/main.o` | `plan.cppm` entry-main 独立重算 + census 盲区 | 对象路径收敛单一 `object_for`;entry-main 纳入普查 / 复用已扫描单元对象 | `b7f32f6`(本 branch,0.0.98) |
| **#241** | enhancement | ✅ 已修待发 | build.mcpp G3 环境契约缺 per-dep 路径 | `build_program.cppm` contract_env(无 per-dep);sanitizer `sanitize_feature_env`;rerun hash;调用点 `prepare.cppm` 依赖 build.mcpp 站 + `dependencyEdges` 图;struct `BuildProgramEnv` | `BuildProgramEnv.depDirs` + `contract_env` 发 `MCPP_DEP_<NAME>_DIR`(同 feature sanitize,自动进 rerun hash)+ `mcpp::dep_dir()` 模块助手;用权威 consumer→dep 边图注入(覆盖 feature 激活依赖)。**根因**:消除包侧自导航 store 布局。作用域=依赖侧 build.mcpp(root 工程 build.mcpp 早于依赖解析,列后续)。e2e 125;**架构评审补**:canonical+short 双发 + 碰撞守卫(`4449077`) | `df3750b` + `4449077`(0.0.98) |
| **#242** | enhancement | ✅ 已修待发 | 消费端无法关默认 feature 集 | `feature_closure` 无条件 seed default;dep-spec 键白名单缺 `default-features` `toml.cppm`;`DependencySpec`(`dep_spec.cppm`)无 `defaultFeatures` | `default-features` 键→`DependencySpec.defaultFeatures`(默认 true)→穿进 `feature_closure` 单一 `seedDefault` 门(false 时跳过 default seed);root/工程 build.mcpp 仍 seed。**Cargo 平价,单一收敛点**。单测 + e2e 126;**架构评审补**:传递边 opt-out 收敛到 `aggregatedRequest`(e2e 127) | `bba624d` + `4449077`(0.0.98) |
| **#243** | enhancement | 📐 设计✅ / 实现待后续 PR | manifest `[features]` 缺条件依赖(**实为已存在**)+ 依赖 feature 转发(`dep/feat`,真缺口) | 条件依赖已存在:`[feature-deps.<name>]` `toml.cppm:699-710` + xpkg `features.x.deps` `xpkg.cppm:1135-1161` → `featureDeps`;转发无处表达;请求集两处不自洽(`mergeActiveFeatureDeps` vs `apply()` `:2801-2806`,`:2653` 传递性 TODO) | 设计定稿 `.agents/docs/2026-07-19-issue-243-feature-forwarding-design.md`:`[features]` 内 `dep/feat` token 转发;引入**单一** per-package 请求 feature 漏斗 `requestedFeaturesByPkg`(顺带补 `:2653` 传递性),与 #242 加性组合。**实现列 0.0.99+ 独立 PR** | 设计:本 branch;实现:后续 |

> 上批(#224–#235 + R6)已于 **0.0.97** 发布,总账见
> `.agents/docs/2026-07-18-v0.0.97-architectural-remediation-implementation-plan.md`;本文件不重复,仅以 §0 继承其框架。

---

## 2. 已完成工作的架构评估(#239/#240)—— 根因 or workaround?

**结论:根因级,非 workaround。** 依据:

- **#240** 的修复不是"link 端把 `obj/main.o` 特判成消歧路径",而是**把对象路径分配收敛成单一来源 `object_for`**——scanner 单元与 synthesized entry-main 走**同一函数 + 同一碰撞普查**,link 输入与编译边"永不背离"是**结构性保证**,不是打补丁。这正是 §0"保留意图到单一汇聚点"主线的落地:#233 的病根就是"对象路径这件事拆散在两处算"。
- **#239** 的修复不是"发现 `@` 就删掉",而是**前缀构造对任意 relPath 保证'向下且 shell 安全'**——覆盖越根 `..`、绝对根、非可移植字符三类,逐分量单射保住 #233 的唯一性(L1b 断言兜底残余)。
- **稳定性**:常见单二进制工程(main 唯一)对象路径**字节不变**;干净 relPath 的碰撞项亦字节不变;仅在"真的发生碰撞"时行为改变,且改后才正确(此前直接构建失败)。已验:单测 35/35、e2e 117/118/02/03/07/08/09 + 新增 123/124 全绿、非 glob main 两边界(唯一→扁平 / 碰撞→消歧)真跑通。
- **留下的已知架构债(如实记录)**:#235 的 depfile 规则 `"$out.d"` 对**任何**含 `@` 的对象路径本就脆(ninja 1.12.1 实测含 `@` 即单引号包裹)。本次靠净化前缀**绕开**了触发面,但**未根治** `"$out.d"` 自身的脆性——若将来对象路径经其它途径合法带 `@`,仍会复现。**候选后续**:ninja depfile 侧改用不受外层引号影响的写法(独立 issue,不在本批次范围;已在此登记以免遗忘)。

---

## 3. 剩余开口工作的根因计划(#237 / #241 / #242 / #243 / #238)

- **#237 / #241 / #242**:均为**机械级根因修**,锚点已核实(§1),各自独立小改面,不触碰构建图。可各成一 commit(或并入 0.0.98 后续 PR)。
- **#243**:**需先设计**(feature 转发是传递性的,且与 #242 的 opt-out、per-package feature 漏斗、xpkg 已有平价点交互)。走 brainstorming → 设计文档 → 实施。
- **#238**:**根因不在本仓**。mcpp 侧只能做诊断改进(把 `fetch failed (exit 1)` 变成携带真实原因的错误事件),**必须同时在 xlings 仓开根因 issue**(多 index_repos 解析),否则按"不做 workaround"纪律**不能标记为已修**。

---

## 4. 整体架构评估(批次落地后)

> 方法:实现全部落地后,派一个**独立对抗性 reviewer**(冷上下文)审全量 batch diff（`05155ff..HEAD`),按"workaround 味 / 单漏斗违背 / 回归 / 一致性债 / 测试是否真门控"五维找问题并给可复现触发。下述结论基于其发现 + 逐条核实 + 实测。

### 4.1 设计合理性(单一汇聚点 vs 特判)——逐条

| # | 判定 | 依据 |
|---|------|------|
| #239/#240 | ✅ 根因/单漏斗 | `object_for`/`safe_object_prefix` 是真正的单一对象路径分配器,scanner 单元与 synthesized entry-main 同源;残余非单射由 L1b 响亮断言兜底。非特判。 |
| #237 | ✅ 根因 | 单一描述符采纳点 funnel(`warn_unknown_xpkg_keys` 两站同调)+ 封闭词表随 parser else-if 同步。 |
| #238 | ✅ 正确归属 | 根因在 xlings(openxlings/xlings#374);mcpp 侧只从自有上下文重建可操作诊断,纯函数可测,不吞子进程输出。非冒充修复。 |
| #241 | ✅ 根因(修正后) | 用权威 consumer→dep 边图注入(非名字猜测)。review 指出"canonical vs declared 名"矛盾 → 已改为**canonical + short 双发**并加碰撞守卫。 |
| #242 | ⚠️→✅ 根因(修正后) | `seedDefault` 门本身干净,但 review 证实 **feature 请求集在解析与激活两处独立推导且对传递边不自洽**(激活只扫 root 直接依赖)——`default-features=false` 对传递依赖被静默丢弃,甚至编译默认门控源却不解析其依赖。**已收敛**:见 §4.6。 |

### 4.2 一致性(一数据模型多文法)

- **feature 条件依赖**:TOML `[feature-deps]` 与 xpkg `features.x.deps` 已收敛到单一 `Manifest::featureDeps`(#243 核实)。✅
- **feature 请求集**:此前是**反例**——解析用 per-edge `spec.features`,激活用 root 直接边重推,两处漂移。§4.6 已把二者收敛到唯一权威源(`dependencyEdges` 图的 `aggregatedRequest`)。✅(本批次最大的架构收益)
- **残留不一致(如实登记)**:xpkg 描述符 `deps` 仍是**纯版本串**,不支持 per-dep `features`/`default-features`;#242 的 opt-out 只在 TOML 面。属**先于本批**的表面不对称(xpkg deps 从来没有 per-dep features),消费端 opt-out 也只在 root(TOML)面有意义 → 非本批回归,列为一致性债。

### 4.3 稳定性 / 回归面

- feature 激活是本批风险最高的改动面;已跑**全部 feature e2e**(67/71/72/79/80/81/82/83/100/106/125/126)+ 新增 127 + 单测 35/35 全绿,直接依赖行为逐字节不变(root 边携带 root spec)。
- 对象路径改动对常见工程逐字节不变(§2)。
- **未做多平台/跨构建 e2e**(本机仅 Linux)——沿用"合并后全量 e2e + 多平台 CI"堵盲区,故**发版前置条件 = CI 全绿**(见 §5,用户要求)。

### 4.4 架构债清算(不静默遗留)

| 债 | 处置 |
|----|------|
| `"$out.d"`×`@` depfile 脆性(§2) | 本批靠净化前缀绕开触发面;`"$out.d"` 自身脆性**未根治** → 候选独立 issue(登记在案)。 |
| #238 多仓解析根因 | 已开 **openxlings/xlings#374**,mcpp 侧诊断到位。 |
| #241 payload-mount 子项(裸文件 payload 实体在共享 runtimedir) | 未做(fetcher/store 侧改动),issue 中为"顺带"项 → 后续。 |
| #243 转发实现 | 设计定稿,列 0.0.99+ 独立 PR。 |
| **新发现**:传递依赖的 active feature 集变化跨"就地重建"未触发该依赖重编(e2e 127 control 需 `clean` 才对) | fingerprint 未覆盖传递 feature 态 → 登记为候选后续(非本批引入的正确性回归:首次干净构建正确,只是增量重建缓存过旧)。 |

### 4.5 issue↔实现映射完整性

§1 每行均有 commit + 状态如实;#238 明确标 ⛔(根因在 xlings),未冒充已修。✅

### 4.6 已执行的架构优化(review 头号建议)

**把 feature 请求集收敛到唯一权威漏斗。** `DependencyEdge` 现携带 per-edge `requestedFeatures + defaultFeatures`;新 `aggregatedRequest(depPkgIndex)` 对某依赖包的**所有入边**做 union(features)/OR(default-features)(Cargo 菱形语义),feature 激活与依赖 build.mcpp env **共同消费**之。一次改动同时:(a) 修 #242 传递边 opt-out 丢失(commit `4449077`);(b) 退休 `prepare.cppm` 长期的"传递 dep→dep feature 请求未传播"限制;(c) 与 #243 设计的 `requestedFeaturesByPkg` 漏斗同向(#243 实现可直接在此之上做转发注入)。e2e 127 门控。

**结论**:本批全部修复均为根因级,无 workaround 冒充;唯一 review 抓到的真隐患(#242 传递边)已按"收敛到单漏斗"原则修掉而非打补丁。剩余为显式登记的后续项(§4.4),无静默遗留。

---

## 5. 执行与记账纪律

- 每落地一个 issue:更新 §1 对应行的状态 + commit,并在 CHANGELOG 追加(带 `#nnn`)。
- 凡"根因在仓外 / 转后续"的,§1 标 ⛔ 并附外部 issue 链接,**不得**在本仓用 band-aid 标记已修。
- 本批次的发布仍走既定链路:release 四平台 → 镜像 xlings-res(gh+gtc)→ xim-pkgindex 索引 → bump bootstrap pin;随后 mcpplibs #79 CI pin 升级解阻塞(#240 是 #79 的合并门)。

---

## 6. 架构不变量(跨批次,持续生效)

### 6.1 同一决策不许两处推导(§5.2 起,持续)

已命中的实例:#242 的 feature 请求集解析×激活两处不自洽;#253 的 per-OS `features`;**0.0.102 新增两例**——
- **#254**:平台选择在 xpkg 通道按宿主编译期常量推导,在 manifest 通道按 resolved target 推导。native 构建 host == target 使两者巧合一致,缺陷因此对三平台 CI 完全不可见。收敛手段是**类型化**(`mcpp.platform.axis`),让"传错轴"成为编译错误而非运行期巧合。
- **#258**:"哪些构建输入可被条件化"在 cfg 轴与 feature 轴各推导一遍,且答案不同。收敛手段是把该集合提炼为**类型**(`BuildInputs`)而非一张手工维护的键表——表会与结构体漂移,类型不会。

判据:当一个决策需要在两处表达时,优先找一个**类型**来承载它,其次才是把两处调用同一个函数。

### 6.2 失败必须响,不许静默降级(0.0.102 新立)

> 任何"因为条件不满足而少做一步"的分支,必须要么返回错误,要么经 `mcpp::diag::degraded` 上报;
> `log::debug` / `log::verbose` **不算**用户可见。
> 丢弃 `std::expected` 返回值(`(void)expr` 或不接收)在本仓视为缺陷。

由来:0.0.102 批次的四条 issue 里有三条的实质是同一失效模式——引擎遇到没准备好的情况时安静地少做一点事。#257 在 clang 上不发 depfile(构建"成功",产物用陈旧 BMI);#258 在条件段静默丢弃 `flags`(构建"成功",规则从未生效);#259 的 sysroot 兜底是死代码且返回值被 `(void)` 丢弃(安装"成功",二进制不能 exec)。

值得注意的是本仓在**别处**早已把这条纪律做得很好(xpkg 未知键 → `xpkgUnknownKeys` + did-you-mean;`scan_overrides` 零命中 → 硬错;死 glob → 告警并点名归属 feature)。纪律一直存在,只是没有被提升为跨模块不变量,于是新代码没有默认继承它。

`diag::degraded` 的 `impact` 参数是这条不变量的执行机制:它强制作者写出"用户会因此遭遇什么"。上述三个 bug 当年缺的恰好就是这句话。

### 6.3 code review checklist 增补

- 每个 `if (cond) return;` / `continue` / `catch` 静默吞掉的分支:用户看得见吗?
- 每个 `std::expected` 返回值:被检查了吗?`(void)` 丢弃需要理由。
- 新增一个"可条件化 / 可传播 / 可覆盖"的字段时:它进的是哪个**类型**?该类型是否已经表达了这个语义?
