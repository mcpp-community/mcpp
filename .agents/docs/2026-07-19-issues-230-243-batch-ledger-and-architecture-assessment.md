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
| **#237** | 诊断缺口 | ✅ 已修待发 | xpkg 描述符 `mcpp` 段未知键在 **build 路径**静默丢弃(`xpkg parse` 才硬报错) | `xpkg.cppm:1324-1333`(else 分支入 `xpkgUnknownKeys`,build 不消费);消费点仅 `cmd_xpkg.cppm:122/186`;字段 `types.cppm:351` | build 时消费 `xpkgUnknownKeys` → 响亮告警(前向兼容,非硬错)+ `closest_known_xpkg_key`(封闭词表别名 + Levenshtein 回退)did-you-mean;在 `prepare.cppm` 描述符采纳点(funnel)发。单测 `XpkgUnknownKeys.CollectedAndDidYouMean` | `c20520b`(0.0.99) |
| **#238** | bug | ⛔ **根因在 xlings**(mcpp 诊断✅) | xlings `install_packages` 多 index_repos(≥2)解析静默 exit 1 | mcpp 侧只:写多仓 `xlings.cppm:1109-1117`、shell-out `:1016/1022`、吞失败 `package_fetcher.cppm:343`(NDJSON 无 `error` 事件 `progress.cppm:23`);触发链 = #224 root `[indices]` 继承 | mcpp 侧**诊断已落地**(`00b66c5`):`read_seeded_index_repos`(空格容错解析,修了复用 compact `extract_string` 的假 0 仓 bug)+ `format_install_failure_diagnostic` 点名目标/仓清单/`≥2` 缺口提示,`captured_error()` 保留子进程输出,`MCPP_VERBOSE` 记原始调用。单测 `PmPackageFetcher.*`。**根因仍须落 xlings**:已提交 **openxlings/xlings#374** | mcpp 诊断:`00b66c5`(0.0.99);根因:**openxlings/xlings#374** |
| **#239** | bug | 🟢 已修待发 | #233 消歧对越根/绝对 relPath 逃逸 `obj/` + `@` 撞 ninja 引号×#235 depfile | `plan.cppm` 消歧前缀 | `safe_object_prefix` 逐分量净化(去根/`.`丢/`..`→`__up`/非可移植→`_`),逐分量单射保 #233 唯一性 | `b7f32f6`(本 branch,0.0.98) |
| **#240** | bug | 🟢 已修待发 | #233 消歧后 entry-main link 输入用陈旧扁平 `obj/main.o` | `plan.cppm` entry-main 独立重算 + census 盲区 | 对象路径收敛单一 `object_for`;entry-main 纳入普查 / 复用已扫描单元对象 | `b7f32f6`(本 branch,0.0.98) |
| **#241** | enhancement | ✅ 已修待发 | build.mcpp G3 环境契约缺 per-dep 路径 | `build_program.cppm` contract_env(无 per-dep);sanitizer `sanitize_feature_env`;rerun hash;调用点 `prepare.cppm` 依赖 build.mcpp 站 + `dependencyEdges` 图;struct `BuildProgramEnv` | `BuildProgramEnv.depDirs` + `contract_env` 发 `MCPP_DEP_<NAME>_DIR`(同 feature sanitize,自动进 rerun hash)+ `mcpp::dep_dir()` 模块助手;用权威 consumer→dep 边图注入(覆盖 feature 激活依赖)。**根因**:消除包侧自导航 store 布局。作用域=依赖侧 build.mcpp(root 工程 build.mcpp 早于依赖解析,列后续)。e2e 125 | `df3750b`(0.0.99) |
| **#242** | enhancement | ✅ 已修待发 | 消费端无法关默认 feature 集 | `feature_closure` 无条件 seed default;dep-spec 键白名单缺 `default-features` `toml.cppm`;`DependencySpec`(`dep_spec.cppm`)无 `defaultFeatures` | `default-features` 键→`DependencySpec.defaultFeatures`(默认 true)→穿进 `feature_closure` 单一 `seedDefault` 门(false 时跳过 default seed);root/工程 build.mcpp 仍 seed。**Cargo 平价,单一收敛点**。单测 + e2e 126 | `bba624d`(0.0.99) |
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

## 4. 【待补】批次全部完成后的整体架构评估

> 本节在 #230–#243 **全部落地**后填写,评估维度(现在先锁定标准,避免事后放水):

1. **设计合理性**:本批全部修复是否都落在**单一汇聚点**,而非新增特判 / 旁路?逐条对照 §0 主线打分。
2. **一致性**:manifest 文法 / xpkg 描述符 / feature 模型三处是否收敛为**一数据模型多文法**(#243 是关键检验点),还是又分叉出平行实现?
3. **稳定性 / 回归面**:是否复用了"build+test 双路径""合并后全量 e2e + 多平台 CI"来堵 per-commit 单测的盲区?有无跨构建 / 共享库运行期未验的假绿?
4. **架构债清算**:§2 登记的 `"$out.d"`×`@` 脆性、#238 的 xlings 侧根因、#241 的 payload-mount 子项——是清了、还是显式转为后续 issue?**不允许静默遗留。**
5. **issue↔实现映射完整性**:§1 总账是否每条都有 commit/PR 且状态如实(尤其 #238 不得冒充已修)。

**评估触发条件**:§1 表中不再有 🔧/📐/⛔/🟢(全部 ✅ 或显式转 issue 的 ⛔)。

---

## 5. 执行与记账纪律

- 每落地一个 issue:更新 §1 对应行的状态 + commit,并在 CHANGELOG 追加(带 `#nnn`)。
- 凡"根因在仓外 / 转后续"的,§1 标 ⛔ 并附外部 issue 链接,**不得**在本仓用 band-aid 标记已修。
- 本批次的发布仍走既定链路:release 四平台 → 镜像 xlings-res(gh+gtc)→ xim-pkgindex 索引 → bump bootstrap pin;随后 mcpplibs #79 CI pin 升级解阻塞(#240 是 #79 的合并门)。
