# mcpp.toml 语义与风格的统一

2026-09-07。本文调查 `mcpp.toml` 现有的组织规律,把其中未成文的部分写成规则
(SPEC-004),并补上规则暴露出的唯一结构性缺口。

**验收标准就是当前需求**:一个包声明的目标输入(`xim:glibc`、`xim:linux-headers`)
能按目标解析,而不是按宿主。§5 给出判据。

## 1. 起因

本轮把 `mcpp.rules.sycl` 改成显式声明 C 库之后,SYCL 示例的 manifest 变成这样:

```toml
[xlings.workspace]
"xim:dpcpp"         = "7.1.0"   # 在构建机上跑的编译器
"xim:gcc"           = "15.1.0"  # 设备单元编译时对着的 C++ 标准库
"xim:cuda-nvcc"     = "12.9.86"
"xim:glibc"         = ""        # 设备单元编译时对着的 C 库
"xim:linux-headers" = ""
```

五个条目,两种事实。前者是**宿主**事实,后者是**目标**事实。`[xlings]` 在加载时
一律按宿主平台解析(`host_platform_key()`),所以后两条今天写在错误的轴上。它没有
出错,是因为设备 lane 还没有做交叉构建——非交叉时两条轴恰好一致。

问题不是"少了一个字段"。问题是这份 manifest 里已经有三种条件写法,而它们的关系
从未被陈述过。

## 2. 现状调查

### 2.1 条件化有三种并存的写法

| # | 形状 | 覆盖 | 轴 |
|---|---|---|---|
| A | `[target.<selector>.<section>]` | `build`、`dependencies`、`dev-dependencies`、`build-dependencies`、`feature-deps.<f>`、`runtime` | 目标 |
| B | `[feature-<section>.<f>]` | `feature-deps`、`feature-xlings` | 门 |
| C | 值对象平台键 `{ linux=…, macosx=…, default=… }` | 仅 `[xlings]` | 宿主 |

A 已能嵌 B(`[target.<pred>.feature-deps.<f>]`,mcpp-index#359)。
**A 不覆盖 `xlings` 与 `feature-xlings`。**

### 2.2 命名有两种 case,且自洽

```
kebab: dev-dependencies build-dependencies feature-deps feature-xlings
       default-features host-module version-info self-contained …
snake: include_dirs cxx_runtime module_extensions allow_host_libs
       dependency_linkage bmi_schedule scan_overrides required_features …
```

分界是 **Cargo 继承面 kebab,mcpp 自有构建面 snake**。这条规律从未写下,但没有反例。

### 2.3 已有的宪法

docs/05 附录 A 已立两条,方案必须服从:

* Closed syntax, open vocabulary——谁拥有解析语义谁定键,谁拥有领域知识谁定值。
* **A key that duplicates an answer another section already gives is not admitted.**

## 3. 被否掉的方案

### 3.1 新开 `[tools]` 与 `[toolchain-inputs]` 两张表

最初的提议。**否掉,因为它违反附录 A 第二条**:`[xlings.workspace]` 已经回答了
"这个项目要哪些载荷",再开一张表就是两个地方说同一件事,而两处可以不一致。

### 3.2 `[features.<f>.deps]` / `[features.<f>.xlings]`

把一个 feature 的全部效果收在它自己名下,直觉上更整齐。**否掉,因为它在真实
manifest 里是非法 TOML。**

`[features]` 的值允许写成内联表,而 TOML 禁止用子表扩展内联表。实测:

```toml
[features]
rules-sycl = { sources = ["rules/sycl.cppm"] }
[features.rules-sycl.deps]        # → Cannot declare ('features','rules-sycl','deps') twice
```

要让它合法必须禁掉内联写法。而 `[features]` 是 Cargo 兼容面,值还可以是数组
(`default = ["base"]`),数组开不了 section。代价是拆掉 Cargo 那一面。

### 3.3 `[feature.deps.<f>]`

合法 TOML,但顶层同时出现 `feature` 与 `features`,差一个字母;且相对
`feature-deps` 只是把连字符换成点,语义一分未增。**否掉。**

### 3.4 把 `feature-deps` 改名为 `feature-dependencies`

消除"全词 vs 简写"的不一致。**否掉**:`feature-deps` 与 `feature-xlings` 都已进入
已发布的描述符,改名会打断它们。按 SPEC-004 §5.2,不一致处写成规则并注明历史来源。

## 4. 方案

零新词表,零新 section,不改任何已发布的键。规范落在 SPEC-004,实现落在两处接线。

### 4.1 规范(SPEC-004)

1. **平面**(§2)——库依赖与工具是两个平面,解析规则不同。
2. **条件化唯一形状**(§3.1)——`[target.<selector>.<section>]`,条件在外。
3. **门的拼法**(§3.3)——顶层 `<限定词>-<section>`,与 `dev-dependencies` 同构;
   并记录 `[features.<f>.deps]` 被 TOML 否掉的理由。
4. **两条解析轴**(§4)——顶层 `[xlings]` 平台键 = 宿主轴;`[target.…]` 下 = 目标轴。
   前者不是遗留拼法,是宿主轴正确的写法。
5. **命名规约**(§5)——两种 case 的分界成文;已发布的键不改名。
6. **条件化准入**(§6)——新需求必须先尝试用形状 2 表达。

### 4.2 实现(唯一缺口)

在 cfg body 的 dispatcher 里增加 `xlings` 与 `feature-xlings` 两个分支,与既有的
`build` / `dependencies` / `feature-deps` 同形:

```toml
[target.'cfg(os = "linux")'.xlings.workspace]
"xim:glibc"         = ""
"xim:linux-headers" = ""

[target.'cfg(os = "linux")'.feature-xlings.backend-vulkan]
"xim:shaderc" = "2026.3"
```

四条实现约束:

* 值的读法复用 `split_when` + `make_xlings_entry`。
* **不复用 `platform_values`**:条件已在外层,内层再写平台键即两处条件,必须报错
  (SPEC-004 §4.4)。
* 目标轴条目折进 `m.xlings`,`applicable_xlings_addresses` 与
  `fillXpkgDirs` 两个既有读者因此无需知道有第二条轴存在。
* **selector 不得命名目标侧层**(实现时发现,SPEC-004 §4.3.1)。层由依赖解析回答,
  命名层的谓词被推迟到第二遍合并,而那一遍在工具供给之后、每个包的 build.mcpp 之后。
  在那里被接受的条目会被声明却永远装不上。必须拒绝。最初写在本文的
  `cfg(accelerator = "vulkan")` 示例因此是错的,已改。

### 4.3 不做

* 不改名任何已发布的键。
* 不废弃值对象平台键——它是宿主轴。
* 不动 `[features]` 的 Cargo 兼容面。

## 5. 验收标准

以当前需求为准,四条,全部可执行:

| # | 判据 | 结果 |
|---|---|---|
| V1 | SYCL 示例把四条目标输入移到 `[target.'cfg(os="linux")'.xlings.workspace]` 后仍然构建,且设备编译的 C 库搜索链里生态在宿主之前 | **通过**。`Provisioning [xlings.workspace] entries (xim:dpcpp@7.1.0, xim:cuda-nvcc@12.9.86, xim:gcc@15.1.0, xim:glibc, xim:linux-headers)` —— 五条都到齐,其中四条来自目标轴;`check_device_c_library.sh` 通过 |
| V2 | 目标轴条目按**目标**解析,宿主轴条目不随目标改变 | **通过**。`tests/unit/test_target_xlings_axis.cpp` 把同一份 manifest 按两个不同目标各解析一次 |
| V3 | 同时写了外层 selector 与内层平台键的 manifest 被**拒绝**,消息指出条件写了两遍 | **通过**(同一文件) |
| V4 | 回归:四个示例 + llama.cpp-m + mcpp-plugins 的现有 manifest **一个字不改**仍然构建 | **通过**(顶层平台键对象的解析路径未改动,`HostAxisKeepsItsPlatformKeys` 是它的单元断言) |
| V5 | selector 命名目标侧层的 manifest 被拒绝 | **通过**(e2e 619;旧引擎上同一份工程**构建成功**) |

V2 是这次改动的**核心判据**。V4 是破坏性为零的证明。V5 是实现过程中发现的缺口。

**非交叉构建对 V2 零信息量。** 两条轴在非交叉时恰好一致,所以在本机跑一次绿不构成
证据。判据因此不是"示例构建成功",而是"同一份 manifest 按两个不同的目标解析出不同的
条目集合"——这个测量在一台机器上、不需要交叉工具链就能做,而且把接线摘掉后六条断言里
五条当场变红,剩下那条正是宿主轴的回归断言。

## 6. 规模

| 项 | 规模 |
|---|---|
| 解析器 | cfg body 两个分支 + 一条"两处条件"的拒绝 |
| 供给 | `applicable_xlings_addresses` 增加目标轴来源 |
| 文档 | SPEC-004(已写);docs/05 §2.13 增"两条轴"一节并链到 SPEC-004 |
| 破坏性 | 零。纯增量,既有写法全部保持原义 |

## 7. 与本轮其他工作的关系

本轮在收尾验证里修的那条缺陷(依赖的 `[feature-xlings]` 在其 `build.mcpp` 之后才
被供给,mcpp#581)与本文**不是同一件事**:那是**时序**缺陷,本文是**轴**缺陷。两者
都由同一个包暴露,原因是它是第一个同时使用两条轴与一个门的真实包。
