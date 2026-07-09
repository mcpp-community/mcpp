# 项目本地模式下的索引作用域:别把默认全局的官方索引(xim)注入项目组(架构分析 + 修复设计)

**日期**: 2026-07-09
**范围**: `src/config.cppm`(`ensure_project_index_dir`)、`src/pm/index_spec.cppm`、`src/build/prepare.cppm`
**性质**: mcpp 侧架构缺陷分析与修复设计。这是 OpenCV 收录时 `mcpp test` 沙箱里 `cmake` 不可执行故障的**唯一正解所在**。
## 文档族与职责边界(三份配套,全部根治设计、无 workaround)

| # | 仓库 / 文档 | 角色 | 与本故障的关系 |
|---|---|---|---|
| ① | mcpp-index `2026-07-09-mcpp-builddep-loader-store-split-rootcause.md` | 根因总报告(现象/实证/归责) | 先读,了解全貌 |
| ② | **本篇** mcpp `2026-07-09-project-index-scope-global-infra-fix.md` | **本故障的唯一必需修复(根治)** | 修 ② 即可让 OpenCV 闭环 |
| ③ | openxlings/xlings `2026-07-09-scope-consistency-installed-check-and-loader-resolution.md` | xlings 侧**独立**健壮性缺口(根治) | 不由本故障触发、与 ② 互不依赖 |

**职责边界**:OpenCV/cmake 故障的**必需且唯一**修复是 ②(本篇)。③ 是一个**独立**的 additive loader 解析缺口(合法"项目消费者+全局 provider"场景),**不是本故障的必要条件**;两者可各自独立落地。**三份均采用根治设计,不提供也不采用任何 workaround。**

---

## 0. 结论(TL;DR)

- **缺陷**:mcpp 在项目本地模式下,把**官方全局基础设施索引 `xim`** 强注册为**项目作用域** index repo(`config.cppm:684-697`)。因 xlings 把"项目 index_repos 里的包"一律判 `PackageScope::Project`,导致 `xim` 的**全局工具**(cmake/glibc/gcc/make/binutils)被整体错误地"项目化"、装进项目 store —— 而它们本应装进全局 registry。由此引出 loader 不落项目 store、cmake interpreter 悬空、不可执行的连锁。
- **根**:**xlings 的作用域默认就是全局** —— 一个 repo 只有落进"项目组"(项目 `.xlings.json` 的 `index_repos` / 项目 discovered sub-index)才是 Project,其余(全局 repos、全局发现的 sub-index)恒 Global(`catalog.cppm:175-224`)。`xim` 及其 sub-index 本在全局组 → 本应 Global。**是 mcpp 主动把 `xim` 塞进了项目组**(`config.cppm:684-697` 把官方 xim append 进项目 `customRepos`),把一个默认全局的索引挪成了项目作用域。
- **多余**:xlings 原生就在全局解析 `xim`(全局 index_repos + registry 本地 clone `registry/data/xim-pkgindex/pkgs`),并经 additive 让项目可见。`xim:*` 本会 Global 解析 → 装 registry。mcpp 的注入不必要。
- **修复原则(不硬编码)**:**global 是默认;mcpp 只把用户在 `mcpp.toml [indices]` 声明的项目本地索引放进项目组,绝不注入用户未声明的官方 `xim`。** 删掉这次注入即根治;xim 及其 sub-index 留在全局 → registry;cmake/glibc/gcc/make 同店 → interpreter 天然指 registry glibc → 可执行。**xlings 无需改动。** 不建 `is_global_infra` 白名单(违背"global 默认"、且漏 sub-index)。

---

## 1. mcpp 项目本地模式的架构(先厘清哪些是对的)

- **触发**:工程 `mcpp.toml` 声明自定义 `[indices]`(如本地 path 索引)→ 进入项目本地模式(`make_project_xlings_env`,`config.cppm:100-105`;`prepare.cppm:1188` `useProjectEnv = idxSpec && !idxSpec->is_builtin()`)。
- **目的(正当)**:让**用户自定义索引**的包**项目作用域隔离**(不污染全局、可复现)。这本身没问题。
- **工具链走全局(正确)**:默认工具链 `gcc@16.1.0`(含 glibc)经 `make_xlings_env`(无 projectDir)装进 **registry**(`prepare.cppm:805-810`),global 作用域、共享。**对。**
- **一次调用挂双 store**:项目模式命令前缀同时设 `XLINGS_HOME=registry` + `XLINGS_PROJECT_DIR=<proj>/.mcpp`(`xlings.cppm:730-738`)。这本身也 OK —— xlings 支持 additive(project 叠加 global)。

→ 问题**不在**"用了项目本地模式",而在**把哪些索引归入了项目作用域**。

## 2. 缺陷:把全局基础设施索引 `xim` 归进了项目作用域

`ensure_project_index_dir`(`config.cppm:662-767`)做了两件事:

1. **`config.cppm:684-697`(致命)**:把官方 `xim` 索引(`officialIndex = cfg.xlingsHome()/data/xim-pkgindex`)以名字 `xim` **append 进项目 `customRepos`**,随后 `seed_xlings_json`(`:707`)写进项目 `.mcpp/.xlings.json` 的 `index_repos`。
   - 后果:xlings `catalog` 把 `project_index_repos()` 里的包判 `PackageScope::Project` → `storeRoot=project_data_dir/xpkgs`。→ **凡从 `xim` 解析到的包(cmake/glibc/gcc/make/binutils)全部项目化、装项目 store。**
2. **`config.cppm:745-765`(冗余)**:把 `xim` 索引 symlink/copy 进项目 data dir(`<proj>/.mcpp/.xlings/data/xim-pkgindex`)。意图注释 `:745-748`:"让 `xim:python@latest` 之类能解析,免于回退到 remote 索引更新"。

**根(违背"global 默认")**:xlings 的作用域默认全局——只有落进"项目组"才是 Project(`catalog.cppm:175-224`)。`config.cppm:684-697` 这段是**无条件**把官方 `xim` 注入项目 `customRepos`(不经 `is_builtin` 判定、也不是用户声明),等于**主动把一个默认全局的索引搬进项目组**。`xim` 本是 xlings 全局默认索引(`config.cppm:556-561`),连同其动态 sub-index 都应留在全局——不该被注入项目。

**证据(实测)**:项目 `.mcpp/.xlings.json` 的 `index_repos = [compat(本地,对), xim(官方全局,被项目化)]`;项目 store 里出现 `xim-x-cmake/gcc/glibc/make/binutils`,而 fresh registry **无 cmake**(即 cmake 只被装进了项目)。

## 3. 为什么"强加"既多余又有害

- **多余**:xlings 原生就把 `xim` auto-add 进 **globalIndexRepos_**(`config.cppm:556-561`),catalog 同时扫 project+global 两套 repo,项目模式 additive over global。registry 里已有 `xim-pkgindex` 本地 clone。→ **`xim:cmake` 本会 Global 解析 → 装 registry → 对项目可见,无需 mcpp 再把 xim 加进项目 repos。**
- **有害**:一旦项目化,全局工具装进项目 store;其 loader dep glibc 又被工具链 bootstrap 装在 registry 并记录,项目安装时被"已装"复用跳过 → 项目 store 的 glibc 空 → elfpatch 把 cmake interpreter 指向空目录 → 悬空。**故障链的起点就是这次错误的作用域归属。**

## 4. 修复设计

### 设计原则:**global 是默认,项目化是显式且仅限用户声明**
xlings 的作用域已由"repo 落在哪一组"决定(`catalog.cppm:175-224`):`project_index_repos()` → Project;`global_index_repos()` / discovered 全局 sub-index(`xim-indexrepos.json`)→ Global;discovered 项目 sub-index → Project。**即"不落进项目组的一切,默认全局"。** `xim` 及其 sub-index 本在全局组/全局发现 → 天然 Global。

因此**不建任何硬编码"全局基础设施"白名单**(既违背"global 默认",又漏掉动态发现的 sub-index)。正确原则只有一句:

> **mcpp 只把"用户在 `mcpp.toml [indices]` 里声明的项目本地索引"放进项目组;绝不注入用户未声明的索引(尤其官方 `xim`)。**

### 根治方案(单一设计;不含 workaround)
1. **删除 `config.cppm:684-697`**(`ensure_project_index_dir` 里把官方 `xim` append 进项目 `customRepos` 的整段)。→ mcpp 不再把 xim 从全局组搬进项目组;xim 及其 sub-index 留在全局 → Global → registry。
2. **收敛 `config.cppm:745-765`**(项目 data dir 的 xim 副本暴露)——它是"项目化 xim"的配套产物,删注入后不再需要。
3. **保留 `:672` 遍历中"用户声明的本地自定义索引 → 项目组"的逻辑**(这是项目本地隔离的正当用途,不动)。
4. **全局侧保证 `xim:*` 本地可解析(回应原动机、免 remote)**:确保全局 `xim` 条目/发现指向 **registry 的本地 clone**(`registry/data/xim-pkgindex`),使 `xim:*` 以 Global 解析、装 registry、不触发 remote fetch。原 `:745-748` 的"避免 remote 索引更新"顾虑,在全局侧被正确满足。

落地后:`xim` 的全部工具(cmake/glibc/gcc/make/binutils)及其 sub-index → Global → registry(同一全局 store);项目模式经 xlings additive 天然可见并复用;工具链(§1 已正确 global)与它们同店 → interpreter 天然自洽。**完整根治,不依赖任何 xlings 改动,且对 sub-index 机制天然正确(mcpp 一概不碰全局发现的 sub-index)。**

### ❌ 不要做(明确排除的 workaround)
- **不建 `is_global_infra` 硬编码白名单**:硬编码 `{mcpplibs,xim,awesome,scode,d2x}` 违背"global 默认",且漏掉 xim 动态 sub-index —— 正解是"别注入",而非"枚举谁是全局"。
- **不做"删注入但仍以其它方式把 xim 拉进项目"**的变体。
- **不在 xlings 侧迁就**:不"把 glibc 物化进项目 store",不"改 elfpatch 指项目空目录"——那是坐实错误作用域(见文档 ③ 反模式)。**正解在本仓:别把默认全局的索引搬进项目组。**

## 5. 与 xlings 的关系(职责边界)
- 修好本缺陷后:cmake/glibc/gcc/make 全 Global → registry(同一全局 store)→ cmake interpreter 天然指 registry glibc(存在)→ 可执行。**xlings 的 additive 模型本就支持此,无需任何改动。**
- xlings 侧另有一个**相互独立**的健壮性缺口(合法"项目消费者 + 全局 loader-provider"时,elfpatch 的 provider 解析口径),记录在 xlings 仓文档里,**不是本故障的必要条件、也不由本修复触发**。

## 6. 验证与最小复现
- **修前(现状)**:含自定义 `[indices]` 的工程 + compat 包声明 build-dep `xim:cmake` 并在 install() 执行它 → 任意机器(含 fresh `MCPP_HOME`)`mcpp test` → `cmake: cannot execute`(项目 store 的 xim-x-glibc 只有 `.xpkg.lua`)。现成:`mcpp-index` 仓 `tests/examples/opencv` + `pkgs/c/compat.opencv.lua`。
- **修后预期**:项目 `.xlings.json` 的 `index_repos` 只含本地 `compat`;`xim-x-cmake/glibc/gcc/make` 出现在 **registry** 而非项目 store;`cmake` 可执行;`compat.opencv` 的 `mcpp test --workspace` 三平台闭环(descriptor 与测试工程零改动)。
- **回归点**:确认移除项目 xim 后,`xim:*` 依赖仍能解析(靠 global auto-add + registry 本地 clone),且不触发 remote 索引 fetch。

## 7. 影响
- **compat.opencv**:本修复是其闭环的前置;修好后 descriptor(已是零兼容代码的 `pkginfo.build_dep` 形式)与 `tests/examples/opencv` **零改动**即通过 `mcpp test --workspace`,可进 CI 选跑、开 PR。
- **面向未来**:确立"**global 默认、只项目化用户声明的本地索引、绝不注入官方索引**"这一不变式,避免后续任何"全局工具被项目化"的复发;对 xim 动态 sub-index 天然正确(mcpp 不碰全局发现)。

## 8. 改动点清单(单一根治设计,不硬编码)
- `src/config.cppm:684-697` —— **删除**把官方 `xim` append 进项目 `customRepos` 的整段(不再把默认全局的索引注入项目组)。
- `src/config.cppm:745-765` —— 收敛项目 data dir 的 xim 副本暴露(注入删除后不再需要);由全局 `xim` 指 registry 本地 clone 满足解析(免 remote fetch)。
- `src/config.cppm:662-682`(`ensure_project_index_dir` 遍历)—— **保持不变**:仅项目化用户在 `[indices]` 声明的本地自定义索引(项目隔离的正当用途)。
- **不改** `is_builtin()`,**不新增** `is_global_infra` 白名单(违背"global 默认"、漏 sub-index)。
- e2e/工作区:`mcpp test --workspace` 覆盖"含全局 build-dep 工具的 compat 包"(opencv 即样例)+ 断言 xim 工具落在 registry 而非项目 store,防回归。
