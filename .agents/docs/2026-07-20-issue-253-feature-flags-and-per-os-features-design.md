# Issue #253 —— per-feature `flags` + per-OS `features` 设计

> 日期:2026-07-20
> 基线:mcpp **0.0.100**(HEAD;`MCPP_VERSION = "0.0.100"` @ `src/toolchain/fingerprint.cppm:21`)
> 范围:GitHub issue **#253** —— xpkg 描述符 (1) `features.<name>.flags`(feature 级 per-glob 编译 flags);(2) per-OS `features`(feature 的 sources/flags 按 OS 差异化)。阻塞用例:compat.opencv 5.0.0 的 `dnn` feature off-linux 腿(macOS NEON / windows)+ base 构建的死 glob 告警。
> 所有 file:line 锚点均在 0.0.100 HEAD 上核实;关键行为均用 HEAD 二进制(`target/x86_64-linux-gnu/44c8eb92797aa242/bin/mcpp`)实证。
> 架构主线沿用批次总账 [2026-07-19-issues-230-243-batch-ledger-and-architecture-assessment.md](2026-07-19-issues-230-243-batch-ledger-and-architecture-assessment.md) §5.2:**一数据模型、多文法、单一收敛漏斗;同一决策不许两处推导**。

---

## 0. 结论速览

| # | 问题 | 裁决 | 工作量级 |
|---|------|------|----------|
| 1 | `features.<name>.flags` 不存在(HEAD 实证:`unknown mcpp-segment key 'features.foo.flags'`) | **新增第七张 feature 表 `featureFlags`,复用既有 `GlobFlags` 类型;激活时在 `apply()` 折入 `buildConfig.globFlags` 同一漏斗**,scanner/backend/fingerprint 零改动 | 小(约 4 处,每处 <30 行) |
| 2 | per-OS `features` | **文法层已经免费生效**(per-OS 段=文本拼接后同一解析循环,HEAD 实证 `linux.features.bar` 被注册)——工作 = 把 additive-append 合并语义**测试锁定 + 文档承认**,不建任何新机制 | 极小(测试+文档) |
| 2b | `features.dnn.macosx.sources`(feature 内嵌 per-OS 键)备选 | **显式否决**:会造成第二个 OS 分发点(与 `mcpp.<os>` 拼接并存)= "同一决策两处推导"的架构债 | — |
| 3 | (顺带发现)per-OS 拼接键 = **宿主 OS** 编译期常量,与 `target_cfg` 的 resolved-target 轴不一致 | 交叉编译场景的既有缺陷,**不阻塞 #253**(native mac/windows 腿 host==target);单独开 follow-up issue | 另行 |

死 glob 告警的消除是 #1 的自然结果:flags 规则随 feature 激活才进场,feature off 时规则不存在,告警无从发出;feature on 而 glob 仍空 → 告警照发(此时它是真问题)。

### 0.1 裁决记录(2026-07-20,实现启动)

1. **传播语义分界接受**:feature `flags` 私有、per-TU、不传播(`defines`=接口开关,`flags`=构建配方细节)。e2e 以"消费者 TU 探针宏不在场"锁死。
2. **TOML 平价文法入本 PR**:`[features].<f>` 表格形加 `flags` 数组-of-inline-tables,与 `[build].flags` 共享解析 helper(一数据模型两文法)。
3. **host/target 轴缺陷(§2.3)单独开 issue**,本 PR 不动拼接键;issue 引用本文 §2.3。
4. **实现期补强(fingerprint)**:核实发现 per-package fingerprint(`canonical_package_build_metadata`,`prepare.cppm:264-317`)只序列化 cflags/cxxflags/ldflags/genfiles,**不含 globFlags**——今天靠"descriptor 随版本冻结"+"feature 开关必然改 cflags(`-DMCPP_FEATURE_*`)"间接成立。本 PR 顺手在该函数补 globFlags 全量序列化(与根侧 `:254-260` 同款),使 featureFlags 与既有 per-glob 规则的缓存语义都密不透风。`featureOrigin` 不序列化(可由 feature 集推导,而 feature 集已入 cflags)。

---

## 1. 现状核实(HEAD = 0.0.100)

### 1.1 feature 模型:七减一张表,单一激活漏斗

xpkg `features` 分支(`src/manifest/xpkg.cppm:1138-1248`)与 TOML `[features]`(`src/manifest/toml.cppm:231-257`)解析到同一组 keyed-by-featureName 的表:

```
implies   → featuresMap            (types.cppm:372)
sources   → buildConfig.featureSources   (types.cppm:145)
defines   → buildConfig.featureDefines   (types.cppm:153)
requires/provides → featureRequires/featureProvides (types.cppm:381)
deps      → featureDeps            (types.cppm:388)
"dep/feat" → featureForwards       (types.cppm:399)
```

全部由 `feature_closure`(`prepare.cppm:441-459`)判定激活、`apply()`(`prepare.cppm:2870-2975`)统一消费:

- **defines**(`:2887-2916`):`-DMCPP_FEATURE_<F>` 私有;用户 defines 进 `privateBuild.cflags/cxxflags`(→ scanner 种子进**包内每个 TU**,`scanner.cppm:998-1003`)**并** 进 `publicUsage`(接口传播,`computeUsageRequirements` `:2054-2105`,激活后 `:3120` 重跑)。
- **sources**(`:2917-2974`):DROP 在 `!includeDevDeps` 门内、ADD 双模式都跑(0.0.94 eigen_blas 教训,注释 `:2929-2937`)。

未知 feature 子键**不再静默吞掉**:记入 `m.xpkgUnknownKeys`(`xpkg.cppm:1221-1224`),由收养点 `warn_unknown_xpkg_keys`(`prepare.cppm:60-73`)带 did-you-mean 报出。HEAD 实证:

```
$ mcpp xpkg parse probe.lua --json        # probe 带 features.foo.flags
error: probe.lua: unknown mcpp-segment key 'features.foo.flags' — silently ignored at build time by this mcpp version
```

即 issue 第 1 半的缺口属实,且**恰好落在架构准备好的扩展点上**。

### 1.2 per-glob flags:类型、匹配、告警

- 类型 `GlobFlags { glob; cflags; cxxflags; asmflags; defines }`(`types.cppm:125-131`),**有序** vector `buildConfig.globFlags`(`:158`),声明序=应用序(GNU last-wins);显式注释为**私有、不传播**(`types.cppm:123-124`)。
- xpkg 解析分支 `xpkg.cppm:960-1025`(entry 缺 `glob` 硬错 `:1017-1020`,未知子键硬错 `:996-1001`)。
- 匹配在 scanner:`apply_glob_flags`(`scanner.cppm:845-864`),brace 预展开(`:841-844`),命中计数 `globFlagHits[i]++`(`:853`),flags 落到该 unit 的 `packageCflags/packageCxxflags/packageAsmflags` → ninja/compile_commands 直出(`ninja_backend.cppm:760,843,893`;`compile_commands.cppm:117`)。
- **死 glob 告警**在 `scanner.cppm:933-939`:`"[build].flags glob '{}' matched no source file"`,故意是 warning 不是 error(cfg 门控源集在某些 target 上合法为空,`:830-834`)——但 opencv 的场景(feature off → mlas 规则死)属于"结构性必死",告警噪声由此而来。
- glob flags 全量序列化进 fingerprint(`prepare.cppm:254-260`,`globflags:/gc:/gxx:/gas:/gd:` 标签)。

### 1.3 per-OS 段:文本拼接 ⇒ `features` 已经参与(实证)

`synthesize_from_xpkg_lua` 对 per-OS 段的机制是**把命中 OS 的表体追加到 base 体后、跑同一个解析循环**(`xpkg.cppm:812-818`);非命中 OS 段被 skip-table 跳过(`:1432-1437`)。所以**mcpp 段的每个键天然支持 per-OS additive 覆盖**——`features` 也不例外。HEAD 实证(probe.lua,linux 宿主):

```
mcpp.features.foo         = { defines={FOO_BASE}, sources={base_foo.cpp} }
mcpp.linux.features.foo   = { sources={linux_foo.cpp}, defines={FOO_LINUX} }
mcpp.linux.features.bar   = { defines={BAR_LINUX_ONLY} }
→ parse OK, features 2      # bar 被注册;foo 的 per-OS 子键走同一分支
```

合并语义由解析分支代码直接决定(`xpkg.cppm:1158` `try_emplace` 幂等注册 + `:1209` `push_back` 追加):**per-OS 条目对同名 feature 是逐子键 append,新 feature 名直接注册**。issue 第 2 半"features 是中性顶层键、不能 per-OS"的表述,在文法层**已经不成立**——缺的是:该行为从未被测试锁定、从未写进文档、生成器(merge_opencv.lua)不知道可以用。

### 1.4 opencv 用例映射

`compat.opencv.lua`(mcpp-index origin/main,9776 行)现状:
- `features.dnn.sources` 硬编码 x86 载荷(`*/3rdparty/mlas/lib/x86_64/*.S`、avx/avx2/avx512 生成内核)在**中性**顶层段;
- mlas 专属 defines(`BUILD_MLAS_NO_ONNXRUNTIME=1` 等)只能放 `linux.flags` 基础段,glob `**/3rdparty/mlas/**` 在 dnn-off 构建必死 → 告警。

---

## 2. 架构裁决

### 2.1 Part 1:`featureFlags` = 第七张表,激活时折入既有 glob-flags 漏斗

**数据模型**:`BuildConfig` 新增

```cpp
// 与 featureSources/featureDefines 并列(types.cppm:145/153 一带)
std::map<std::string, std::vector<GlobFlags>> featureFlags;
```

复用 `GlobFlags` 原类型,**不新建 per-feature flag 类型**(延续 0.0.100 "type/rule unification" 纪律)。

**消费(唯一新增决策点=零)**:`apply()` 内、`!includeDevDeps` 门**外**(双路径不变量,与 featureDefines 同位置,`prepare.cppm:2887-2916` 一带),对每个激活 feature `f`(按 map 序迭代 `featureFlags`、以 `active` 过滤 → 确定性顺序):

```cpp
for (auto& gf : featureFlags[f]) bc.globFlags.push_back(gf);
```

此后一切照旧:scanner 匹配、per-TU 落 flags、fingerprint 序列化、死 glob 告警——**下游三个消费者一行不改**。顺序语义:base 规则在前、feature 规则按 feature 名序在后,GNU last-wins ⇒ feature 规则可覆盖 base;同一 feature 在 base 段+per-OS 段都有 `flags` 时,per-OS 条目因拼接在后而排后(OS 特化胜出)——与既有 overlay 直觉一致。

**传播语义分界(必须写进文档的设计决定)**:feature `defines` 是**接口开关**(包内全 TU + Public/Interface 传播,§1.1);feature `flags` 是**构建配方细节**(私有、per-TU、不传播,继承 `types.cppm:123-124` 的既有契约)。opencv 的 mlas defines 恰好就该是后者——它们本来就不该泄给消费者。glob 允许匹配包内**任意**源(不限于该 feature 自己的 sources):既有语义免费获得"feature 激活时改 base 文件 flags"的表达力(如 `platform.cpp` 的 `-include unistd.h` 场景)。

**告警上下文**:`GlobFlags` 加一个 `std::string featureOrigin`(空=base 规则),仅用于把告警文本改为
`features.<f>.flags glob '...' matched no source file`——不进 fingerprint(fingerprint 逐字段显式序列化,`prepare.cppm:254-260`,新字段不加即不影响;单测锁定"加 origin 不变 fingerprint")。

**文法(两文法一模型)**:
1. xpkg:feature 分支(`xpkg.cppm:1160-1226`)加 `sub == "flags"` 案:把 base `flags` 分支的 entry 解析循环(`:970-1022`)抽成共享 helper `parse_glob_flags_entries(cur, dst)`,两处调用,错误文案不变。
2. TOML:`[features]` 表格形(`toml.cppm:231-257`)加 `flags` 数组-of-inline-table,复用 TOML 侧既有 `[build]` flags 数组解析器。#243 先例:两文法只共享一个数据模型与一个解析 helper,零重复逻辑。
3. 词表:`features.<f>.flags` 从 unknown 变 known(§1.1 的 did-you-mean 通道自动不再报);`kKnownXpkgKeys`(`xpkg.cppm:126`)如枚举 feature 子键则同步。

### 2.2 Part 2:per-OS `features` —— 承认、锁定、文档化,不建新机制

文法已生效(§1.3),裁决为:

1. **合并语义规范化(现状即正确,测试锁死)**:per-OS 段对 feature 是**逐子键 additive append**;per-OS 段可注册新 feature(仅该 OS 存在的 feature,如 windows-only `d3d`;在其他 OS 请求它 → 既有 unknown-feature 路径报错,行为正确,文档说明即可)。
2. **推荐写法(进 xpkg 文档 + 生成器模板)**:common/delta 模式——feature 身份与跨平台公共载荷(defines、protobuf/dnn 通用源)放中性 `features`;平台特化载荷(mlas x86 源、NEON 源、SIMD flags)放 `mcpp.<os>.features.<f>`。与大源包批次的 common/delta 拆分方针([2026-07-19-large-source-pkg…](2026-07-19-large-source-pkg-platform-fixes-and-buildmcpp-generation-design.md) §"common/delta 拆分")同构。
3. **`--all-os` 校验闭环**:`mcpp xpkg parse --all-os`(`cmd_xpkg.cppm:123-153`)天然逐 OS 重解析,新键自动被覆盖;mcpp-index CI 已在接入(PR#92 线)。补一条含 per-OS features + feature flags 的 fixture 进该命令的测试。

**显式否决的备选**:`features.dnn.macosx.sources`(feature 内嵌 OS 键)。理由:mcpp 已有**两根平台轴**——`mcpp.<os>` 拼接(host,parse 期)与 `target_cfg`(resolved target,prepare 期,`prepare.cppm:89-108/411-428`)。在 feature 内再开第三个 OS 分发点 = 同一"这段配置在哪个平台生效"的决策三处推导,正是批次总账定性的隐性架构债(#242 的事故形态)。per-OS 段作为唯一 owner,features 只是又一个参与键。

同理否决"拆成 per-OS 两个包":生态负担(版本对齐、消费者写条件依赖),且 §1.3 证明根本不需要。

### 2.3 顺带发现:per-OS 拼接的 host/target 轴缺陷(follow-up,不入本 PR)

拼接键 = `mcpp::platform::xpkg_platform`,**编译期宿主常量**(`platform/common.cppm:96-105`);而 `target_cfg` 按 resolved target 求值(`prepare.cppm:92-94` 仅 native 回退 host)。交叉编译(如 0.0.100 已打通的 linux→windows MinGW)时,dep 描述符的 `windows` 段**不会**被拼接——per-OS sources/flags/deps/xpm 资产全部选错。native 构建 host==target,故 #253 的实际解锁面(macOS/windows 原生腿)不受影响。修法方向:build/prepare 收养路径(`prepare.cppm:1621/:1772`)把 resolved target OS 作为 `osOverride` 传入(参数缝隙已存在,`xpkg.cppm:812`);pm/install 资产选择是否随动需单独辨析(工具类包 host、库源码包 target)。**单独开 issue,引用本节。**

---

## 3. 实现清单(单 PR 可容)

| # | 改动 | 位置 |
|---|------|------|
| 1 | `BuildConfig::featureFlags` + `GlobFlags::featureOrigin` | `types.cppm:125-158` 一带 |
| 2 | 抽 `parse_glob_flags_entries` helper;base `flags` 分支改调用;feature 分支加 `flags` 案 | `xpkg.cppm:960-1025 / 1160-1226` |
| 3 | TOML `[features].flags` 平价文法 | `toml.cppm:231-257` |
| 4 | `apply()` 激活折入(门外、双路径) | `prepare.cppm:2887-2916` 一带 |
| 5 | 告警文案按 `featureOrigin` 区分 | `scanner.cppm:933-939` |
| 6 | 文档:xpkg 参考加 `features.<f>.flags` + per-OS features 合并语义 + common/delta 推荐写法 | docs |

**不做(非目标)**:feature flags 的接口传播(违背 §2.1 分界);per-glob `ldflags`(不在 GlobFlags 模型,链接期无 per-TU 概念);per-OS default features;host/target 轴修复(§2.3 follow-up);`.o` 注入类能力。

## 4. 测试矩阵

**单测**(`tests/unit/test_manifest.cpp` 一带):
- xpkg/TOML 各:feature `flags` 解析(顺序、缺 glob 硬错、未知子键硬错文案复用);`features.<f>.flags` 不再入 `xpkgUnknownKeys`。
- per-OS 合并:同名 feature 子键 append、per-OS-only feature 注册、非命中 OS 段不生效(skip-table)。
- fingerprint:激活集变化 → globFlags 序列化变;`featureOrigin` 不进 fingerprint。

**e2e**(host-aware,续 `tests/e2e/` 现有末号 **145** → 146 起,入 `run_all.sh`):
- 146:dep 带 `features.X.flags`(glob 命中 X 的一个源,defines 里放探针宏,TU 内 `#ifdef` 断言)。
  - 不带 feature:构建成功且 **stderr 无 "matched no source file"**(死 glob 告警消除的回归锁);
  - `--features X`:探针宏在场;**双路径**同断言跑 `mcpp build` 与 `mcpp test`(0.0.94/0.0.97 不变量);
  - flags 不传播:消费者 TU 断言探针宏**不在场**(与 feature defines 传播形成对照)。
- 147:per-OS features——descriptor 在宿主 OS 段给 feature 补充源+flags,断言激活后该源被编译、flags 生效;非宿主段内容不可见。

**生态复验**:compat.opencv 迁移(§5)后 `mcpp build`(dnn off)stderr 零 flags 告警;`--features dnn` linux 腿字节级等价(flags 集合不变,只是搬家)。

## 5. 生态闭环(mcpp-index 侧,mcpp 发版后)

1. `merge_opencv.lua`/`gen_descriptor.py`:mlas/opencl 等 feature 专属 flag 规则从 `mcpp.<os>.flags` 迁入 `mcpp.<os>.features.dnn.flags`;`features.dnn.sources` 做 common/delta 拆分——中性段留 protobuf+dnn 通用源,x86 载荷(mlas x86_64 .S、avx* 生成内核)下沉 `linux/windows.features.dnn`,macosx 段放 NEON 载荷。
2. macOS/windows dnn 腿快照采集(snapshot-*-opencv.yml)→ 三平台 `opencv-dnn` 收官。
3. mcpp-index CI:`--all-os` fixture 覆盖新键(接 PR#92 线)。

## 5.5 实现期实录(2026-07-20,随实现动态更新)

1. **裁决 ③ 已落地**:host/target 轴缺陷开为 [issue #254](https://github.com/mcpp-community/mcpp/issues/254)(引用本文 §2.3),本 PR 不动拼接键。
2. **TOML AOT 封闭文法守卫(#227)拦下了 inline 拼写**:`is_array_of_tables` 对"元素全为表的数组"一视同仁,inline `flags = [{...}]` 在 `features.<f>` 下同样触发 allowlist 拒绝。修法=allowlist 引入 `*` 单段通配(`features.*.flags`),`aot_path_matches` 逐段匹配;副产品:`[[features.<name>.flags]]` AOT 拼写与 `[[build.flags]]` 一样合法(libs/toml 两拼写建同一形状,零额外分支)。文档与错误文案同步。
3. **e2e 146 首版踩到既有 test-mode 语义**:TOML 工程若 base `src/**` 覆盖 feature 源,`mcpp test`(feature off)因 drop 是 build-only 而**无旗标编译**该源——`#error` 探针误炸。修正=fixture 采用 opencv 真实形态(base sources 不覆盖 feature 源,`[build] sources = ["src/*.cpp"]`),四象限语义随之完全一致。这也确认了 xpkg 包(feature 源只在 featureSources)不受 drop-skip 影响,mlas 无旗标编译的担忧不成立。
4. **e2e 147 的 find 假阳性**:feature-on/off 两次构建 fingerprint 不同、目录并存,`.o` 扫描需先 `rm -rf target`。
5. 版本 0.0.101(`fingerprint.cppm` + `mcpp.toml` 两处),CHANGELOG 已记;e2e 定号 146/147(现有末号 145)。
6. 单测:`aot_path_matches` 经 `Manifest.FeatureFlagsTomlTableForm/FeatureFlagsTomlErrors` 间接锁定;per-OS 合并由 `SynthesizeFromXpkgLua.PerOsFeaturesAdditiveMerge` 以 linux/macosx 双 `osOverride` 腿锁定 append 序与不可见性。

## 6. 风险与不变量

- **双路径**:折入点在 `includeDevDeps` 门外——e2e 146 双模式断言锁死(0.0.94 featureSources 事故的同型防线)。
- **顺序确定性**:map 序 + 声明序,无迭代不确定源;fingerprint 覆盖顺序(`gc:/gxx:` 序列化天然含序)。
- **告警语义不回退**:feature 激活而 glob 空仍告警(真死 glob 不许静默);cfg 门控合法空集的既有豁免注释(`scanner.cppm:830-834`)不动。
- **零行为漂移**:不用新键的既有描述符,解析结果与 flags 集合逐字节不变(featureFlags 空表 → `apply()` 折入为 no-op)。
