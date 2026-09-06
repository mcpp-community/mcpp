# SPEC-004:mcpp.toml 的语义与风格

| 项 | 值 |
|---|---|
| **规范编号** | SPEC-004 |
| **标题** | `mcpp.toml` 的平面划分、条件化形状、解析轴与命名规约 |
| **状态** | **草案(Draft)** |
| **版本** | 1.1 |
| **最后修改** | 2026-09-07 |
| **最低实现版本** | 条件化形状:mcpp **2026.8.29.1**(`[target.<selector>.build-dependencies]` 起齐备);目标轴:mcpp **2026.9.6.4** |
| **作者/维护** | mcpp-community |
| **相关设计文档** | `.agents/docs/2026-09-07-mcpp-toml-unified-semantics-design.md`<br>`.agents/docs/2026-06-04-manifest-schema-ownership.md`<br>`.agents/docs/2026-09-03-xlings-workspace-as-the-one-table.md` |
| **相关使用文档** | [docs/05 —— mcpp.toml 字段参考](../05-mcpp-toml.md) |

## 规范用语

按 RFC 2119:**必须(MUST)/ 禁止(MUST NOT)** 强制;**应当(SHOULD)** 强烈建议,
偏离需理由;**可以(MAY)** 可选。

## 实现状态标记

| 标记 | 含义 |
|---|---|
| **已实现** | 当前实现与本条一致 |
| **部分实现** | 已有实现,语义或覆盖面有差异(差异已注明) |
| **未实现** | 本规范要求但尚未支持;当前行为已注明 |

---

## 1. 范围

本规范陈述 `mcpp.toml` 的**结构语义**:一个 section 属于哪个平面、条件写在哪里、
一个条目按什么解析、键怎么命名。它不列举字段——字段在 docs/05。

它回答的是一个新字段或新 section 该长什么样,以及一份 manifest 为什么这样组织。

**边界。** 本规范不覆盖字段的准入条件,那由 docs/05 附录 A(Schema Ownership
Principle)规定,本规范不重复它,只在 §6 引用并补充一条。

## 2. 平面

一份 manifest 的 section **必须**落在下列平面之一。平面是"这段话在谈什么",
不是"它长什么样"。

| 平面 | section | 谈的是 |
|---|---|---|
| 身份 | `[package]` | 这个包是谁 |
| 产物 | `[targets.<n>]`、`[lib]` | 要产出什么 |
| 编译 | `[build]`、`[profile.<n>]`、`[toolchain]` | 怎么编 |
| 库依赖 | `[dependencies]`、`[dev-dependencies]`、`[build-dependencies]` | 需要哪些 mcpp 包 |
| 工具与环境 | `[xlings]` | 需要哪些载荷与工具 |
| 门 | `[features]`、`[feature-deps.<f>]`、`[feature-xlings.<f>]` | 什么条件下要 |
| 条件 | `[target.<selector>.<section>]` | 在哪个目标上要 |
| 产物元数据 | `[runtime]`、`[resources]` | 产出的东西是什么 |
| 生命周期 | `[hooks]` | 构建前后跑什么 |

**状态:已实现。**

**库依赖与工具是两个平面,不是一个。** 库有模块与 ABI,参与解析与链接;载荷是可执行
的工具或被编译对着的输入,不参与模块图。二者的解析规则不同(§4),因此**禁止**把工具
写进 `[dependencies]`,也**禁止**把 mcpp 包写进 `[xlings]`。

## 3. 条件化的唯一形状

### 3.1 条件在外,section 在内

条件化**必须**写成:

```
[target.<selector>.<section>]
```

`<selector>` 是目标三元组或 `cfg(...)` 谓词。**禁止**把条件写成尾部键
(`[xlings.workspace.linux]`)或值的兄弟键。

今天接受 `<section>` 为:`build`、`dependencies`、`dev-dependencies`、
`build-dependencies`、`feature-deps.<f>`、`runtime`、`xlings`、`feature-xlings.<f>`。

**状态:已实现**(上列 section)。

### 3.2 门可以嵌进条件

`[target.<selector>.feature-deps.<f>]` 合法:条件决定这个门**拉进什么**,不决定
这个门**存不存在**。feature 本身**必须**无条件注册,否则在不匹配的平台上请求它会
触发"未知 feature"诊断。

**状态:已实现**(mcpp-index#359)。

### 3.3 门的拼法

门**必须**拼成顶层的 `<限定词>-<section>`,与 `dev-dependencies` 同构:

```
dev-dependencies      build-dependencies     ← 限定词是用途
feature-deps          feature-xlings         ← 限定词是门
```

**禁止**为第二种门发明第二种语法。一个门拉进包和一个门拉进工具,是关于同一个门的
同一句话。

**不采用 `[features.<f>.deps]`,理由是 TOML 而非风格。** `[features]` 的值允许写成
内联表(`rules-sycl = { sources = [...] }`),而 TOML 禁止用子表扩展内联表——
`[features.rules-sycl.deps]` 在真实 manifest 里是**语法错误**。要让它合法必须禁掉
内联写法,而 `[features]` 是 Cargo 兼容面,其值还可以是数组(`default = ["base"]`),
数组开不了 section。

**状态:已实现。**

## 4. 解析轴

### 4.1 两条轴

一个条目按**宿主**还是按**目标**解析,由它谈的是什么决定,**不由**它写在哪里决定。

| 轴 | 谈的是 | 例 |
|---|---|---|
| 宿主 | 在构建机上执行的东西 | `xim:dpcpp`、`xim:shaderc`、`xim:ninja` |
| 目标 | 被编译对着的东西 | `xim:glibc`、`xim:linux-headers`、目标 sysroot |

交叉构建(宿主与目标不同)时二者分叉。非交叉时二者恰好一致,**因此这条差异在非交叉
构建上不可观测**。

### 4.2 写法与轴的对应

| 写法 | 轴 | 状态 |
|---|---|---|
| 顶层 `[xlings]`,值写成平台键对象 `{ linux=…, macosx=…, windows=…, default=… }` | 宿主 | **已实现** |
| `[target.<selector>.xlings…]` | 目标 | **已实现**(2026.9.6.4) |
| `[target.<triple>].sysroot` | 目标 | **已实现** |

顶层平台键对象**不是**遗留拼法,它是宿主轴**正确**的写法:工具必须能在这台机器上
执行。`[target.<triple>].sysroot` 是目标轴 xpkg 引用的既有先例。

### 4.3 两条轴的写法

`[target.<selector>.xlings.workspace]` 与
`[target.<selector>.feature-xlings.<f>]` 被接受,并按**目标**解析:

```toml
[xlings.workspace]
"xim:dpcpp" = "7.1.0"              # 宿主轴:它在这台机器上执行

[target.'cfg(os = "linux")'.xlings.workspace]
"xim:glibc"         = ""           # 目标轴:产物编译时对着它
"xim:linux-headers" = ""
```

产物编译或链接时对着的东西**应当**写在目标轴上;在构建机上执行的工具**应当**写在
顶层 `[xlings]`。把目标事实写在顶层在非交叉构建上恰好正确(§4.1),在交叉构建上不
正确;既有写法保持原义,**不**被废弃。

两条轴同时命名一个包时,`[target.<selector>]` 一侧是更具体的陈述,**必须**是被采用
的那一条;实现**应当**报告这次覆盖。去重按**包**而非按地址进行:`xim:glibc` 与
`xim:glibc@2.40` 是同一次安装的两个地址,两条都保留会让环境取决于供给顺序。

`[target.<selector>.xlings]` **禁止**接受 `subos`:一个工程只有一个环境,而不是每个
目标一个。实现**必须**报错而不是忽略。

**状态:已实现**(2026.9.6.4)。

### 4.3.1 工具的 selector 禁止命名目标侧层

`[target.<selector>.xlings…]` 的 `<selector>` **禁止**命名 docs/14 的五个目标侧层
(`accelerator`、`c-abi`、`c++-abi`、`compiler`、`compiler-runtime`、`kernel-abi`)。

理由是**时序**而非风格:层由依赖解析回答,因此命名层的谓词被推迟到第二遍合并,而那
一遍在工具供给之后、每个包的 build.mcpp 之后。在那里被接受的条目会被**声明却永远装
不上**,产生的失败形态是最坏的一种:构建成功,工具不在。

实现**必须**在第一个载荷被取回之前拒绝,消息**必须**同时点出工具与谓词,并指出两条
出路:按目标条件化,或用 `[feature-xlings.<f>]` 做门——feature 在任何东西被供给之前
就已知。

**状态:已实现**(2026.9.6.4)。

### 4.3.2 目标轴不进描述符

已发布的 xim 描述符按平台分块(`xpm.linux`、`xpm.macosx`、`xpm.windows`),而 selector
不是平台。因此目标轴条目**不产生**描述符边;实现**必须**在发布时报告,而不是把它映射到
某一块上——映射需要为每个平台假定一个代表三元组,而谓词不满足该三元组的条目会消失在
同一种沉默里。

**使用者**装到的东西来自顶层 `[xlings.workspace]`(即 §4.2 的宿主轴);目标轴对"本包
自己的构建对着什么"仍然正确。

**状态:已实现**(2026.9.6.4)。

### 4.4 条件不得写两遍

`[target.<selector>.xlings…]` 之下的值**禁止**再写平台键对象:条件已经在外层,
里面再写一层就是同一个事实的两处陈述,而两处可以不一致。实现**必须**报错而不是
择一,并在消息里点出外层 selector——那是作者要删掉的一半,也是他没有在看的一半。

**状态:已实现**(2026.9.6.4)。

## 5. 命名规约

### 5.1 两种 case,按面划分

| 面 | case | 例 |
|---|---|---|
| Cargo 继承面(依赖、feature、profile) | kebab | `dev-dependencies`、`default-features`、`host-module` |
| mcpp 自有构建面 | snake | `include_dirs`、`cxx_runtime`、`module_extensions` |

新键**应当**按它所在的面选 case。

**状态:已实现**(事实上一致,此前未成文)。

### 5.2 已发布的键不改名

已进入已发布描述符的键**禁止**改名。不一致处**应当**写成规则并注明历史来源,
而不是通过改名消除。

由此保留的已知不一致:`dev-dependencies` 用全词 `dependencies`,而 `feature-deps`
用简写 `deps`。`deps` 是 `dependencies` 的既有简写;两种拼法都在已发布的描述符里。

**状态:已实现。**

## 6. 新增条件化的准入

除 docs/05 附录 A 的准入条件外,新的条件化需求**必须**先尝试用
`[target.<selector>.<section>]` 表达。表达不了才讨论新语法,并**必须**在设计文档里
说明为什么表达不了。

**状态:本规范新增。**

## 7. 判据

本规范的可检验推论:

1. `[target.<selector>.<section>]` 之外**不存在**第二种条件写法(值的平台键对象
   除外,它按 §4.2 是轴而非条件)。
2. 目标轴与宿主轴的差异**只能**在解析后的目标与宿主不同时观测。因此验证 §4.3 的
   测试**必须**跨目标,非交叉的绿色对它零信息量。判据:同一份 manifest 按两个不同
   的目标各解析一次,目标轴条目随之出现与消失,而宿主轴条目两次都在
   (`tests/unit/test_target_xlings_axis.cpp`)。
3. §4.4 的"两处条件"**必须**报错,判据是一份同时写了外层 selector 与内层平台键的
   manifest 被拒绝。
4. §4.3 的"按包去重"判据:两条轴各写一次同一个包,`xlings.deps` 里该包**只出现
   一次**,且是 `[target.<selector>]` 那条。
5. §4.3.1 的判据:一份用层谓词声明工具的 manifest 被拒绝,且拒绝发生在任何下载之前。

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| 1.0 | 2026-09-07 | 首版。平面(§2)、条件化唯一形状(§3)、两条解析轴(§4)、命名规约(§5)、条件化准入(§6)。目标轴列为未实现。 |
| 1.1 | 2026-09-07 | 目标轴落地(mcpp 2026.9.6.4):§4.3.1 工具 selector 禁止命名目标侧层;`[target.<selector>.xlings…]` 与 `[target.<selector>.feature-xlings.<f>]` 转为已实现;§4.3 补两条轴同时命名一个包时的取舍与按包去重;§4.4 转为已实现;§7 补第 4 条判据。 |
