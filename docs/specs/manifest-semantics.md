# SPEC-004:mcpp.toml 的语义与风格

| 项 | 值 |
|---|---|
| **规范编号** | SPEC-004 |
| **标题** | `mcpp.toml` 的平面划分、条件化形状、解析轴与命名规约 |
| **状态** | **草案(Draft)** |
| **版本** | 1.13 |
| **最后修改** | 2026-10-06 |
| **最低实现版本** | 条件化形状:mcpp **2026.8.29.1**(`[target.<selector>.build-dependencies]` 起齐备);目标轴:mcpp **2026.9.6.4** |
| **作者/维护** | mcpp-community |
| **相关设计文档** | `.agents/docs/2026-09-07-mcpp-toml-unified-semantics-design.md`<br>`.agents/docs/2026-06-04-manifest-schema-ownership.md`<br>`.agents/docs/2026-09-03-xlings-workspace-as-the-one-table.md`<br>`.agents/docs/2026-09-25-issue-690-workspace-build-inheritance-consistency.md`<br>`.agents/docs/2026-09-27-eight-reports-by-home-and-one-optimisation-plan.md`<br>`.agents/docs/2026-10-05-std-module-pair-msvc-lto-and-export-discovery-design.md`<br>`.agents/docs/2026-10-06-windows-x86-arch-vocabulary-and-rc-follow-ups-design.md` |
| **相关使用文档** | [docs/04 —— mcpp.toml 字段参考](../04-mcpp-toml.md) |

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
一个条目按什么解析、键怎么命名。它不列举字段——字段在 docs/04。

它回答的是一个新字段或新 section 该长什么样,以及一份 manifest 为什么这样组织。
§8 另陈述编译 flag 列表中一个元素代表哪些参数，§9 陈述工作空间继承与构建需求的作用域。

**边界。** 本规范不覆盖字段的准入条件,那由 docs/90「新增一个 manifest 字段:准入标准」
规定,本规范不重复它,只在 §6 引用并补充一条。

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
`build-dependencies`、`feature-deps.<f>`、`runtime`、`xlings`、`feature-xlings.<f>`、
`targets.<name>`(mcpp 2026.9.14.2+),以及 `abi`、`requires_abi`、`feature-requires-abi`、
`runners`。实现不读取的 `<section>` **必须**报出,不得静默忽略(mcpp 2026.9.14.2+)。

**状态:已实现**(上列 section)。

`[build]` 之下的 `dialect_cxxflags` 是图级联的方言开关,不是逐包可叠加的构建输入
(`BuildConfig::dialectCxxflags`;§9 第 10 条)。写在 `[target.<selector>.build]`
之下时,它接受与本节其它键相同的条件形状,但按图级联规则解析而不是按包解析:
只有一次构建的根(命令的包,或 `-p` 选中的成员)对这个列表贡献,依赖包自己声明
的这个键不到达任何命令。向量按 `[workspace.build]`、根的 `[build]`、再到每个
命中的 `[target.<selector>.build]`(按 §3.1.1 的次序)追加,如同一个可叠加的构建输入。

**状态:已实现(mcpp 2026.9.28.1)。**

### 3.1.1 条件声明替换同一身份的无条件声明

在 `<selector>` 命中的行上,`[target.<selector>.dependencies]` 中某个身份的声明
**替换**该身份在 `[dependencies]` 中的声明;身份按键规范化后的 `(namespace, name)`
比较,而非按键的字面。多个命中的 section 按选择器的具体程度生效:更具体的后生效,
因此替换较宽泛的;具体程度相同时,按选择器文本的字典序。同一规则适用于
`dev-dependencies`、`build-dependencies` 与 `feature-deps.<f>`。这与条件化标量
「最后一个命中者为准」是同一条规则;可叠加的构建输入(`build`)按同一次序追加。

选择器的具体程度是它固定的目标三元组成分的个数:三元组固定全部成分,高于任何
`cfg(...)`;`os = "…"` 与 `linux`、`macos`、`windows` 固定操作系统及其族;
`family = "…"` 与 `unix` 固定族;`arch = "…"`、`env = "…"` 各固定该成分;`all(…)`
固定其各项的并集;`any(…)`、`not(…)`、层键与 `accelerator` 不固定任何成分。于是三元组
高于操作系统,操作系统高于族,`cfg(all(os = "linux", arch = "aarch64"))` 高于
`cfg(os = "linux")`。

清单顺序无法作为规则:TOML 的表不带键的顺序,解析器以有序映射保存键,实现因此只能
观察到选择器文本的字典序。按具体程度排序使结果只取决于选择器说了什么,而不取决于它
怎样拼写;字典序只用于打破平局。工作空间的条件表先于成员的条件表生效,各自按本条
排序(§9 第 2 条)。

只写选项而不写来源(`path`/`version`/`git`/`workspace`)的条件表不是对既有依赖的
修饰,按文法它声明的是另一个包;实现**必须**把这种写法报出,并给出补全来源后的声明。

`[target.<selector>.targets.<name>] kind` 是 `[targets.<name>] kind` 的按行形式:
只接受库目标,只在 `lib` 与 `shared` 之间选择,在命中的行上约束该包的链接形态,
与无条件的 `kind = "shared"` 相同。

`linkage = "static" | "shared"` 与 `kind` 并列,可写在 `[targets.<name>]` 与其按行形式中,
陈述库目标的**默认**链接形态:它不收窄可选形态的集合,只在消费者没有陈述时给出答案。
形态按以下顺序决定,前者优先:根工程依赖边上的 `linkage`,根工程写下的
`dependency_linkage`,包的 `linkage`,`static`。与包默认值不同的显式陈述**必须**被遵从,
且不得记为降级;实现**必须**输出一条同时点名两条陈述的信息。同一张表中 `kind = "shared"`
与 `linkage` 并存、一行同时陈述 `kind` 与 `linkage`、`linkage` 写在程序目标上,均**必须**
被拒绝;按行合并时后命中的陈述替换先前的陈述,无论两者各是 `kind` 还是 `linkage`。

**状态:已实现**(mcpp 2026.9.14.2;`linkage` 为 2026.9.15.2;多个命中的条件表按具体程度生效为 2026.9.28.2,mcpp#728)。

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

工具的**消费者**看到的是两条轴的并集:`xpkg_dir` 与供给都读折叠后的一张表,所以一个
规则包不需要知道某个载荷是由哪条轴声明的。

由此产生一个必须说清的后果:**物化出来的 `.mcpp/.xlings.json` 描述的是最后一次构建的
目标。** 该文件是 mcpp 对"这次构建的环境"的物化,不是对"这个工程"的物化;同一个工程
先按 A 构建再按 B 构建,文件里是 B。读它的东西**必须**按这个含义读。

**一个已发布的包,其诊断里给出的写法必须是它声明的引擎下界能接受的那一种。** 诊断
文本是作者会逐字抄走的东西,推荐一种旧引擎会拒绝的写法就是把升级悬崖搬进了别人的
工程。目标轴要求 mcpp 2026.9.6.4,因此在下界低于该版本的包里,诊断**应当**继续给出
顶层写法。

**状态:已实现**(2026.9.6.4)。

### 4.3.1 工具的 selector 禁止命名**被解析的**层

`[target.<selector>.xlings…]` 的 `<selector>` **禁止**命名由依赖解析回答的五个层
(`c-abi`、`c++-abi`、`compiler`、`compiler-runtime`、`kernel-abi`)。

理由是**时序**而非风格:这五个层由依赖解析回答,因此命名它们的谓词被推迟到第二遍合并,
而那一遍在工具供给之后、每个包的 build.mcpp 之后。在那里被接受的条目会被**声明却永远
装不上**,产生的失败形态是最坏的一种:构建成功,工具不在。

`accelerator` **被接受**,且划分依据正是上面那条时序而不是这个键的主题。它不由任何东西
解析:它是 `--accel` 或 `[build] accel`,在第一个包被查找之前就已读入,因此以它为谓词的
条目在**第一**遍合并里就已折叠,与任何其他条目一样被安装。曾经把它一并拒绝的代价落在
每一个带设备孤岛的工程的每一次构建上——厂商工具包只能无条件声明或完全不声明,于是
一次 CPU-only 构建为它并不编译的设备下载数 GB。

实现**必须**在第一个载荷被取回之前拒绝被禁止的谓词,消息**必须**同时点出工具与谓词,
并指出两条出路:按目标条件化,或用 `[feature-xlings.<f>]` 做门——feature 在任何东西被
供给之前就已知。

**状态:已实现**(2026.9.6.4;`accelerator` 的接纳为 2026.9.6.5)。

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

### 4.5 一个包一个版本

`[xlings.workspace]` / `[xlings] deps` / `[feature-xlings.<f>]` 里一条地址的身份是
`(namespace, name)`。**版本永远是这个包上的约束,不是它名字的一部分。** 命名空间缺省
为 `xim`,与 `[<ns>:]<name>[@<version>]` 的解析一致。

一次构建里同一身份**只安装一个版本**,由两步决定:

1. **裁决**——离产物更近的声明赢:工程 > 它依赖的包。同一份 manifest 内的两条轴仍按
   §4.3 的「更具体的赢」。**不带版本的声明弃权**:它陈述了「要这个包」而没有陈述「要
   哪一版」,因此不参与这个问题。全都不带版本时,结果就是那个裸地址。
2. **校验**——赢家**必须**满足每一条落败的**要求**。`>=` / `^` / `~` / 逗号组合是
   要求;不带运算符的裸版本是**选择**,由裁决处理而不是校验。两条精确钉写得不同,是
   两个选择,较近的那条赢并**必须**被报告;精确钉不满足某条要求时,实现**必须**拒绝
   并同时点出两侧各自的声明与出路。

校验是**一次比较**,不是在索引里搜索。选哪一版由裁决决定,因此实现不需要「有哪些版本
可选」这个输入,也就不需要约束求解器。代价是明确的:一些求解器本可满足的组合会被拒绝
(工程写 `>=8.0`、规则写 `8.5.0`、索引最新 8.3),而拒绝消息里写着出路。

版本位接受范围表达式,并且**必须**在两个方向上都被求解:`>=2026.1` 装到满足它的最高
版本,`>=2099.1` 被拒绝。实现**必须**让 `mcpp::xpkg_dir` 回答范围——安装了却答「不
存在」,是让规则包无法声明下界的那个缺口。

`xpkg_dir` 对一条地址的回答**必须**是 xlings 为它安装的那个载荷:先取 xlings 报告的
解析结果,没有时按 xlings 的版本文法在已安装的版本目录中选择(SPEC-001 §10.1)。
`libglvnd@1.7` 因此回答 `1.7.0.1`。

**状态:已实现**(2026.9.6.6;按 xlings 文法回答自 2026.9.27.1,mcpp#712)。

### 4.6 宿主构建读取宿主三元组的行

不带 `--target` 的构建以宿主为目标。`[target.<宿主三元组>]` 对它的描述与对任何其他目标
的描述相同,**必须**被应用:`toolchain`、`linkage`、`cxx_runtime` 等键的效果与
`--target <宿主三元组>` 相同。行的查找与 `--target` 使用同一个与拼写无关的比较,
`x86_64-unknown-linux-gnu` 找到 `[target.x86_64-linux-gnu]`。命令行的 `--toolchain`
(`MCPP_TOOLCHAIN`)仍优先于行的 `toolchain`。

该比较中,架构段的 `amd64`、`arm64` 与 `x86` 依次与 `x86_64`、`aarch64` 与 `i686` 相同:
`[target.x86-windows-msvc]` 即 `i686-windows-msvc` 的行,交给工具的也是后者的拼写。
`i386`、`i486` 与 `i586` 是不同的目标,**禁止**与 `i686` 相互替代。

**状态:已实现**(mcpp 2026.9.27.1,mcpp#704;`x86` 自 mcpp 2026.10.5.3)。

### 4.7 载荷的来源与供给时机

一条载荷声明陈述**要哪个包**;它从哪里来,以及什么时候装,是另外两个问题。

**`[xlings.overrides]` 陈述来源。** 键是 §4.5 的身份 `[<ns>:]<name>`,**禁止**带版本;
值是一个路径,或一个表,表中**必须**恰好写 `program`(一个文件,或一个在 `PATH` 上查找
的程序名)或 `root`(与载荷同布局的目录)之一,并可写 `version`。

- 被覆盖的包**禁止**被供给:它不进入安装列表,不参与离线判定。
- 它仍参与 §4.5 的校验:写了 `version` 时,实现**必须**按该版本校验每一条落败的要求,
  不满足则拒绝并点出两侧;没写时**必须**记一条 note,点出未被校验的要求。
- `mcpp::xpkg_dir` **必须**回答覆盖所指的 root,`mcpp::xpkg_program` 回答它所指的程序,
  `mcpp::xpkg_source` 回答 `override`。
- 覆盖**只**由一次构建的**根**陈述:根清单、环境变量 `MCPP_XLINGS_OVERRIDE_<NS>_<NAME>`
  或 `config.toml`,优先级依此顺序由高到低。依赖写它**必须**被拒绝,拒绝消息点出该包与
  正确的位置。

**`provision = "on-request"` 陈述供给时机。** 条目表接受它与 `version`、`when` 并列;
缺省为 `eager`,即构建程序运行前供给。标为 `on-request` 的包:

- 在所有声明它的 manifest 都这样写时,构建程序运行前**禁止**被供给;
- 构建程序以 `mcpp::xpkg_request` 请求它;实现**必须**把一次运行中的全部请求合为一次
  安装,并只重跑请求过的那些程序,丢弃它们该次运行的其余陈述;
- 规划(`mcpp emit build-database`)**禁止**因此安装任何东西,**必须**记一条
  `MCPP_BUILD_DATABASE_PAYLOAD_DEFERRED` note;
- 请求一个没有任何 manifest 如此声明的包**必须**被拒绝。

**状态:已实现**(mcpp 2026.10.1.3,mcpp#755)。

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

### 5.3 只在一个平台生效的键带平台前缀

一个键的效果只存在于一个平台或 ABI 时,它的名字**必须**带该平台的前缀:`windows_`
(PE),今天的实例是 `[targets.<n>]` 下的 `windows_subsystem`、`windows_entry`、
`windows_code_page` 与 `windows_auto_export`。不带前缀的中立名字留给在每一行都有含义的
键:这样的键**必须**在每一行都产生效果,或在不能产生效果的行上被拒绝。

带前缀的键在其它平台上不产生任何内容,也不被拒绝,因此同一份 manifest 在各个平台上都可用;
它的条件化仍按 §3.1 写成 `[target.<selector>.targets.<n>]`。

**状态:已实现(mcpp 2026.10.5.1,#766)。**

## 6. 新增条件化的准入

除 docs/90「新增一个 manifest 字段:准入标准」的准入条件外,新的条件化需求**必须**先尝试用
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
5. §4.3.1 的判据:一份用**被解析的**层谓词声明工具的 manifest 被拒绝,且拒绝发生
   在任何下载之前;而同一份 manifest 把谓词换成 `accelerator` 时构建通过并装上工具。
   两个方向都要跑:只跑拒绝那半,一个把所有层谓词都拒掉的实现同样通过。
6. §4.5 的判据**必须**同时观察「装了什么」和「答了什么」。只断言 store 里有一个版本
   目录,会在一个装 A 而 `xpkg_dir` 答 B 的实现上通过;只断言答案,会在一个装两份的
   实现上通过(`tests/e2e/627_one_package_one_version.sh`)。
7. §4.5 的拒绝判据**必须**带反向腿:把钉抬到满足要求后同一份工程构建通过。否则一个
   「凡工程与依赖同时声明同一个包就拒绝」的实现也会通过
   (`tests/e2e/628_a_pin_below_a_stated_floor_is_refused.sh`)。
8. §3.1.1 的判据:同一身份在无条件表与命中的条件表中各声明一次、只有条件表写
   `linkage = "shared"` 时,命中的行链接共享库,不命中的行静态链接,解析记录给出
   声明所在的表(`tests/e2e/677_a_conditional_dependency_replaces_the_unconditional_one.sh`);
   按行 `kind` 在命中行上给出共享库与原因 `row-kind`,不命中行为 `default`
   (`tests/e2e/678_a_row_states_a_library_form.sh`)。
9. §3.1.1 `linkage` 的判据:包的默认值为 `shared` 时,不陈述的消费者得到共享库与原因
   `package-default`;边上的 `linkage = "static"` 与写下的 `dependency_linkage = "static"`
   都得到静态形态与原因 `requested`,且 `--strict` 下构建通过;按行 `linkage` 替换无条件的
   `kind = "shared"`(`tests/e2e/692_a_package_states_its_default_link_form.sh`)。
10. §8 的判据取自三处并要求一致:程序打印每个宏收到的值,`compile_commands.json` 与
    `mcpp emit build-database` 列出的词,三者都等于按 §8 读出的词;同一份断言在 Linux、
    macOS 与 Windows 上不变(`tests/e2e/736_compile_flag_words_reach_the_compiler_and_the_databases.sh`)。
    规则本身由读回性质陈述:任意词经实现的拼写读回为它自身,经宿主引号读回也为它自身,
    且 POSIX 宿主上由 `/bin/sh` 实测(`modules/manifest/tests/test_flag_words.cpp`、
    `tests/unit/test_compile_commands.cpp`)。
11. §8 `defines` 集合语义与 §9 第 1 至 3 条的判据:同一成员分别作为命令构建的包与作为兄弟
    成员的 `path` 依赖时,每个工作空间词在 C 与 C++ 编译单元中各恰好出现一次且先于成员
    自己的词;成员重写的宏名只出现成员的值,`!NAME` 移除的宏名不出现;作为依赖的成员
    解析自己的 `x.workspace = true` 条目;通过 `git` 引用的仓库成员收到其仓库的
    `[workspace.build]`(`tests/e2e/770_workspace_member_as_dependency.sh`)。第 3 条的
    内部错误与第 4 条的键表由单元测试陈述(`tests/unit/test_workspace_inheritance.cpp`)。
12. §9 第 6 条的判据:根包私有目录中的 `limits.h` 不到达依赖的 C 与 C++ 编译单元;依赖
    的编译命令在两个仅 include 设置不同的根包下相同;根包自己的编译单元中每个根包目录
    恰好出现一次;依赖因此找不到头文件时,报错之后指出消费者目录
    (`tests/e2e/765_a_consumer_include_directory_stays_in_the_consumer.sh`)。
13. §9 第 1 条索引归档一种情况的判据:描述符指向归档内一个省略 `version` 的成员时,该成员
    取得归档工作空间的版本与 `defines`(`tests/e2e/774_an_index_member_inherits_its_archive_workspace.sh`)。
    第 5 条的判据:成员目录内的 `emit xpkg`、`publish --dry-run`、`toolchain list` 读取继承后的
    清单(`tests/e2e/773_commands_outside_the_build_read_the_effective_manifest.sh`)。
14. §9 第 7 条的判据:发布归档中的清单写出继承来的 `version`、`license` 与 `cxxflags`,兄弟边
    以版本边出现且描述符的 `deps` 列出它,原清单以 `mcpp.toml.orig` 保留;归档的使用方在新旧两个
    客户端上都能构建;两次发布的归档逐字节相同;缺少 `version` 的兄弟 `path` 边被拒绝且报错给出
    应写的一行;无需修改的包的归档与此前逐字节相同
    (`tests/e2e/772_a_published_member_is_self_contained.sh`)。
15. §8 对链接 flag 的判据:`[build] ldflags` 与构建程序的 `mcpp::link_flag` 中写出的
    `-Wl,-rpath,$ORIGIN/../lib` 原样到达程序的运行路径,不出现 `/../lib`;依赖传播的同一
    元素同样原样到达;含空格的 `link_search` 目录是一个参数
    (`tests/e2e/795_a_link_flag_reaches_the_linker_as_written.sh`)。
16. §4.5 按 xlings 文法回答的判据:xlings 发布的版本选择向量在 mcpp 的实现上逐条得到相同
    结果(`modules/versioning/tests/data/semver-vectors.tsv`,
    `modules/versioning/tests/test_xpkg_version.cpp`);`libglvnd@1.7` 在只装有 `1.7.0.1`
    时回答该目录(`tests/unit/test_freestanding.cpp`)。
17. §4.6 的判据**必须**带对照腿:没有行时默认构建自包含,写了宿主行
    `cxx_runtime = "toolchain-coupled"` 后普通构建需要 `libstdc++.so.6`,行以另一种拼写
    书写时同样生效(`tests/e2e/802_a_host_build_applies_its_host_row.sh`)。
18. §9 第 8 至 10 条的判据:成员得到根的条目与条件行,自己声明的同一个包保留自己的地址;
    未解析的 `workspace = true` 在三张依赖表中都被点名拒绝;成员工具的工具链取工作空间的
    `[toolchain]`,自己声明时取自己的(`tests/unit/test_workspace_inheritance.cpp`)。
19. §10.2 的判据**必须**两个方向都跑:只写 `features = ["codegen"]` 的消费方得到工具并编译
    它生成的源,不启用该特性的消费方什么都不构建;`tools` 指名非 `bin` 目标时加载被拒绝
    (`tests/e2e/800_a_feature_provides_its_host_tools.sh`)。
20. §10.3 的判据:程序构建到 `bin/` 并可运行,依赖的代码不在消费方中,占位符到达 action,
    `mcpp pack` 的归档含该程序;有 musl 工具链时,`--target x86_64-linux-musl` 下它为目标构建
    (`tests/e2e/801_a_dependency_program_is_shipped_with_the_consumer.sh`)。

21. §9 第 1 条的根包与第 10 条的 profile:虚拟根的 `[profile.release]` 到达成员;成员自己的
    同名表替换它;带 `[package]` 的工作空间在根构建与 `--workspace` 下都只有一张图,根包的
    编译命令恰好含一次 `[workspace.build]` 的词(`tests/e2e/885_workspace_profiles_and_the_root_package.sh`,
    `tests/unit/test_workspace_plan.cpp`)。
22. §9 第 11 条的判据**必须**在产物上读取:成员共享库的 RUNPATH 含它自己构建程序的标记而不含
    另一成员的,`-p` 下相同;非工作空间中依赖的共享库含自己与 profile 的标记而不含根包与兄弟
    依赖的;只由根声明的搜索路径不再到达依赖的共享库,构建给出 `link/root-flags` 提示,依赖
    自己声明后链接通过(`tests/e2e/884_a_shared_library_links_with_its_own_closure.sh`)。
23. §11 的判据(`tests/e2e/887_msvc_lto_and_export_discovery.sh`,需要 cl.exe):cl.exe 上
    `lto = true` 以 `/GL` 编译、以 `/LTCG` 链接;省略 `windows_auto_export` 的 DLL 所链接的包以
    `/GL-` 编译并报告一次;陈述 `true` 与写入 `/GL` 被拒绝;陈述 `false` 时完整使用 `/GL`。

## 8. flag 列表的元素

`cflags`、`cxxflags`、`asmflags` 与 `ldflags` 的一个元素是一段文本,代表零个或多个词;
编译器或链接器收到的参数就是这些词,按列表顺序排列。本节在元素的文本上陈述(TOML 或 Lua
先去掉自己的转义)。该读法对这四个键的每一个出现位置相同:`[build]`、`[targets.<n>]`、
`flags` 的 glob 条目、feature、`[profile.<n>]`、`[target.<selector>.build]`、xpkg 描述符,
以及构建程序的 `mcpp:cflag=`、`mcpp:cxxflag=` 与 `mcpp:link-flag=` 指令。

1. 未加引号的空格与制表符分隔词,连续的分隔符等同于一个。
2. `'` 开启一段单引号区域,区域内的字符按字面取到下一个 `'` 为止。
3. `"` 开启一段双引号区域,区域内 `\"` 代表 `"`、`\\` 代表 `\`,其余字符按字面取到下一个
   未转义的 `"` 为止。
4. 引号区域之外,`\` 后跟空格、制表符、`"`、`'` 或 `\` 时代表该字符;其余 `\` 按字面。
5. 相邻的区域与字面片段组成一个词;一个只含 `''` 或 `""` 的词是空词。
6. 未闭合的引号区域延伸到元素末尾。
7. 除上述字符外,任何字符没有特殊含义;实现**禁止**对 `$`、`*`、`?`、`;`、`|`、`&`、`<`、
   `>`、`` ` ``、`~` 做展开或解释。
8. 例外:以 `-D` 或 `/D` 开头且含空格(U+0020)的元素代表一个词,即元素文本本身,规则 1 至 7
   不适用。自 mcpp#234 起,每个版本在宿主读取之前都把这样的元素整体加引号,该例外保持其含义不变。

`defines` 的一个条目 `X` 是一个值:它代表一个词 `-D` 与 `X` 的拼接,**禁止**按上述规则
读取。

`defines` 是按宏名构成的集合(mcpp 2026.9.25.1 起)。条目的宏名是 `=` 之前的文本,
无 `=` 时为整个条目。条目按包接收它们的顺序读取:`[workspace.build]`、包自己的
`[build]`、各个命中的 `[target.<selector>.build]`。实现**必须**满足:同一宏名的后一个
条目在原位替换前一个;条目 `!NAME` 移除宏名 `NAME`;一个包的每个编译单元对每个宏名
至多收到一个 `-D` 词;`defines` 条目取代同一个包的 `cflags`、`cxxflags` 中读作单个词
且宏名相同的 `-D` 词。实现向这四个列表插入一个词 `w` 时,**必须**使用一个按上述规则读回恰为 `w` 的拼写;
构建程序的 `mcpp:link-lib=`、`mcpp:link-search=` 与 `mcpp:link-script=` 指令由名字或路径构成的
值属于这种插入。

实现**必须**把每个词原样交给编译器或链接器,与宿主的命令行读取规则(POSIX `sh`、MSVCRT)
无关;`compile_commands.json` 与构建数据库(SPEC-005 R3.7)列出的参数**必须**是这些词。
依赖的 `ldflags` 传播给消费者时按词传播,包内相对的搜索路径按词解析为绝对路径。
规则 7 对链接 flag 的一个推论:`$ORIGIN` 等加载器记号原样到达链接器。为 shell 或 ninja
手工转义的写法(`\$ORIGIN`、`'$$ORIGIN'`)按上述规则读取,不再是转义。

`dialect_cxxflags` 与 `std-module-flags` 不在本节范围内。

**状态:已实现(mcpp 2026.9.17.1;`defines` 的集合语义 mcpp 2026.9.25.1;`ldflags` 与链接指令
mcpp 2026.9.26.2,#703)。**

## 9. 工作空间继承与构建需求的作用域

1. 工作空间成员**必须**恰好接收一次 `[workspace.package]`、`[workspace.build]` 与
   `x.workspace = true` 条目的继承,无论它是命令构建的包、带 `[package]` 的工作空间根
   自己的 `path` 依赖所到达的成员、另一个成员的 `path` 依赖、通过 `git` 引用的托管在
   git 上的工作空间的成员,还是索引包归档内的成员(描述符的 `mcpp` 字段指向该成员的
   清单)。后两种情况按该成员所在仓库或归档的工作空间根继承,相对路径以该根为锚点;
   在归档内查找工作空间根时**禁止**越出该版本的安装根。工作空间的上下文(它是哪个
   工作空间、工作空间根在哪里)取决于清单**在哪里**,与命令走的是哪条分支无关——带
   `[package]` 的工作空间根按自身构建时,同样要在解析任何依赖之前建立这一上下文。
   `-p`/`--package` 首先按成员的包身份(限定名 `<namespace>.<name>`,其次是裸包名)
   为其命名,目录路径与目录名是回落拼法。带 `[package]` 的工作空间根自己的包是成员
   `"."`,同样恰好一次地接收 `[workspace.package]` 与 `[workspace.build]`,无论命令选中
   的是它、它与其它成员,还是只有其它成员。
2. 向量按工作空间、成员、命中的 `[target.<selector>.build]` 的顺序追加,命中的条件表
   之间按 §3.1.1 的具体程度排序;`defines` 按 §8 的集合语义合并。标量仅在成员未
   **声明**该键时取工作空间的值。
3. 继承**必须**在 `defines` 展开之前、在清单被固定进构建图之前完成。实现**必须**拒绝
   把含有未展开 `defines` 的清单固定进构建图,并报告内部错误。
4. 可继承的 `[build]` 键集合只陈述一次。解析、已知键检查与报错文本**必须**取自同一
   陈述。
5. 读取成员清单的每一条命令**必须**读取继承后的有效清单。
6. 一个包的私有构建需求,包括它的 `include_dirs` 与 `include_dirs_after`,**禁止**到达
   另一个包的编译单元。使用需求只从依赖流向它的消费者,**禁止**从消费者流入依赖。
   一个依赖的编译命令**必须**与构建它的工程无关;依赖缓存键**必须**包含到达该命令的
   全部输入。
7. 工作空间成员的发布形态**必须**自包含:发布的清单写出继承来的值,兄弟成员之间的
   `path` 边以版本边发布,无法以版本表达的 `path` 边**必须**被拒绝发布。
8. 成员**必须**继承工作空间根的 `[xlings.workspace]` 条目,包括
   `[target.<selector>.xlings.workspace]` 的条件行(按行继承,合并时由选择器决定)。继承是
   隐式的,与 `[toolchain]` 相同,因为载荷描述的是构建运行的环境,不是依赖图的边。成员自己
   声明的同一个包(身份为 `(namespace, name)`)优先。`[feature-xlings.<f>]` 不被继承:特性
   属于声明它的包。
9. 一条 `x.workspace = true` 条目在继承之后仍未解析时,实现**必须**在它进入构建的每个位置
   (根包、`-p` 选中的成员、`path` 与 `git` 依赖、索引依赖)拒绝它,并点名条目所在的表与
   名称。带 `[package]` 的工作空间根按它自己的 `[workspace.dependencies]` 解析自己的
   `workspace = true` 条目。
10. `[toolchain]`、`[target.<triple>]`、`[indices]` 与 `[profile.<name>]` 是根位置的键:它们
    为整个依赖图选择编译器、目标行、索引与构建 profile,因此只在成员作为一次构建的根时继承。
    profile 按名字继承,成员自己声明的同名表整体替换工作空间的。作为宿主工具构建的成员是其
    子构建的根,同样继承这三项(§10.1)。`[build] dialect_cxxflags`(及其条件形式
    `[target.<selector>.build] dialect_cxxflags`)同样是根位置的键:它是 §3.1
    所述的图级联方言开关,只在包作为一次构建的根时被渲染并到达命令。与前三项相同,
    一个包声明它不被诊断——一个依赖包为自己将来作为根的构建合法地声明这些键,这一条
    只是把已有行为写成明文规则。

11. 一个包的 `ldflags`(含其构建程序的链接指令)是它的使用需求,按第 6 条只流向消费者。
    根包自己的程序与共享库以根的链接行链接:根包的 `ldflags` 与它所到达的每个包的
    `ldflags`。其它包拥有的共享库**必须**以图级链接 flag 加上其拥有者所到达的包的
    `ldflags` 链接,**禁止**接收根包或无关包的私有 `ldflags`。图级链接 flag 是 profile 的
    `ldflags` 与 `[target.<selector>.abi]` 为链接渲染的词;它们到达每一个镜像。

**状态:已实现(第 1 至 7 条 mcpp 2026.9.25.1;第 8 至 10 条 mcpp 2026.9.27.1,mcpp#713、
#714、#710;第 10 条的 `dialect_cxxflags` 为 mcpp 2026.9.28.1,#717;第 1 条的根包、第 10 条的
`[profile.<name>]` 与第 11 条为 mcpp 2026.10.5.2,#771)。**

## 10. 依赖的程序

一条依赖边可以取得依赖包的 `bin` 目标,而不链接它的代码。取得的方式由边决定,因为程序
在哪台机器上运行决定了它为哪个目标构建。

### 10.1 `tools`:在构建机器上运行的程序

`x = { ..., tools = ["<bin>"] }` 取得依赖为构建机器构建的程序:它在一次嵌套的子构建中构建,
发布到全局工具库,构建程序以 `mcpp::dep_bin("<x>", "<bin>")` 取得路径。

- 子构建的工具链由请求它的构建决定一次:`--toolchain`(`MCPP_TOOLCHAIN`)优先;否则取
  工具包自己的声明——应用它所在工作空间的根位置键(§9 第 10 条)之后,先宿主行的
  `toolchain`,再 `[toolchain]`;都没有时取请求方为构建程序使用的宿主工具链。决定的结果
  传给子构建,并写入工具库的键,因此键与产物不会不一致。
- 工具库键中的源树摘要不包含带有自己 `mcpp.toml` 的子目录:工作空间根作为工具包时,其成员
  的改动不使工具重建。

**状态:已实现**(mcpp#355;工具链的决定与源树摘要自 mcpp 2026.9.27.1,mcpp#710、#705)。

### 10.2 特性的 `tools`

`[features.<f>] tools = ["<bin>"]` 陈述启用特性 `f` 需要本包的程序 `<bin>` 在构建机器上
运行。启用该特性的依赖边,等同于在边上写了 `tools = ["<bin>"]`;不启用时不构建。条目
**必须**指名本包的一个 `bin` 目标,否则清单在加载时被拒绝,消息列出本包的 `bin` 目标。

**状态:已实现**(mcpp 2026.9.27.1,mcpp#709)。

### 10.3 `artifacts`:随消费方发布的程序

`x = { ..., artifacts = ["<bin>"] }` 取得依赖的 `bin` 目标,以**消费方**的目标与 profile
构建,作为消费方计划中的一个链接单元,输出到消费方的 `bin/`。

- 只经 `artifacts` 边到达的包的代码不链接进消费方;同一个包另经普通边到达时照常链接。
- 构建程序的 action 以 `${mcpp.artifact:<x>/<bin>}` 引用该程序的路径,可用于命令与输入;
  名称不对应一个 `artifacts` 条目时,规划失败并点名该占位符。
- `mcpp run` 不选择该程序;`mcpp pack` 把它放在消费方程序旁。
- 交叉构建(`--target`)中该程序为目标构建,与 `tools` 为构建机器构建相对。

**状态:已实现**(mcpp 2026.9.27.1,mcpp#711)。

## 11. 链接期优化与导出发现

1. `lto = true` 在每一个能兑现它的编译器上兑现:gcc 与 clang 以 `-flto` 编译与链接,cl.exe
   以 `/GL` 编译、以 `/LTCG` 链接与归档。一个行无法兑现时**禁止**声称兑现。
2. `[targets.<n>] windows_auto_export` 区分「省略」与「陈述 `true`」。在 cl.exe 上 LTO 生效
   时,导出需要被发现的 PE 共享库:
   - 省略该键:其对象所来自的包以 `/GL-` 编译,DLL 仍以 `/LTCG` 链接,构建报告一次降级;
   - 陈述 `true`:规划期拒绝(`lto-export-discovery`);
   - 陈述 `false`:不受影响。
3. 写进这类 DLL 所链接的包的 flag 中的 `/GL`,无论 LTO 是否生效,都在规划期被拒绝。
4. `mcpp pack` 产出的静态库**禁止**携带 LTO 中间表示;其对象不以 LTO 编译,构建报告一次。
5. `[test] windows_code_page` 为所发现的每个测试程序陈述 Windows 代码页,取值与 target 键相同。

**状态:已实现(mcpp 2026.10.5.2,#770)。**

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| 1.0 | 2026-09-07 | 首版。平面(§2)、条件化唯一形状(§3)、两条解析轴(§4)、命名规约(§5)、条件化准入(§6)。目标轴列为未实现。 |
| 1.1 | 2026-09-07 | 目标轴落地(mcpp 2026.9.6.4):§4.3.1 工具 selector 禁止命名目标侧层;`[target.<selector>.xlings…]` 与 `[target.<selector>.feature-xlings.<f>]` 转为已实现;§4.3 补两条轴同时命名一个包时的取舍与按包去重;§4.4 转为已实现;§7 补第 4 条判据。 |
| 1.2 | 2026-09-07 | 一个包一个版本(mcpp 2026.9.6.6):新增 §4.5(身份=`(namespace, name)`,版本是约束;裁决与校验两步;范围必须双向可解且可被 `xpkg_dir` 回答);§4.3.1 改为「禁止命名**被解析的**层」,`accelerator` 明确被接受(2026.9.6.5);§7 补第 5 条的反向腿与第 6、7 条判据。 |
| 1.3 | 2026-09-14 | 条件依赖声明替换同一身份的无条件声明,`targets.<name>` 成为可条件化的 section,不读取的 section 必须报出(mcpp 2026.9.14.2):新增 §3.1.1 与 §7 第 8 条判据。 |
| 1.4 | 2026-09-15 | 库目标的默认链接形态 `linkage`(mcpp 2026.9.15.2):§3.1.1 补默认值的语义、优先顺序与拒绝条件;§7 补第 9 条判据。 |
| 1.5 | 2026-09-17 | 编译 flag 列表元素的读法(mcpp 2026.9.17.1,#655):新增 §8 与 §7 第 10 条判据。 |
| 1.6 | 2026-09-25 | 工作空间继承与构建需求的作用域(mcpp 2026.9.25.1,#690):§8 补 `defines` 的集合语义;新增 §9 与 §7 第 11 至 14 条判据。 |
| 1.7 | 2026-09-26 | §8 的读法扩展到 `ldflags` 与构建程序的链接指令(mcpp 2026.9.26.2,#703):`$ORIGIN` 原样到达链接器;§7 补第 15 条判据。 |
| 1.8 | 2026-09-27 | mcpp 2026.9.27.1:§4.5 的版本位按 xlings 文法回答(#712);新增 §4.6 宿主构建读取宿主三元组的行(#704);§9 补第 8 至 10 条(#713、#714、#710);新增 §10 依赖的程序:`tools`、特性的 `tools`、`artifacts`(#709、#711);§7 补第 16 至 20 条判据。 |
| 1.9 | 2026-09-28 | mcpp 2026.9.28.1:§9 第 1 条补上带 `[package]` 的工作空间根自己的 `path` 依赖所到达的成员,`-p` 先按包的身份解析(#725);§3.1 接受 `[target.<selector>.build] dialect_cxxflags`,§9 第 10 条把它列为根位置的键(#717);§3.1.1 的状态改为部分实现,多个命中的条件表的先后见 mcpp#728。 |
| 1.10 | 2026-09-28 | 多个命中的条件表按选择器的具体程度生效,三元组高于操作系统高于族,字典序只打破平局(mcpp 2026.9.28.2,mcpp#728,2026-09-28 设计 D7):§3.1.1 陈述规则与具体程度,§3.1 与 §9 第 2 条的「按清单顺序」随之更正;§3.1.1 转为已实现。 |
| 1.11 | 2026-10-05 | 新增 §5.3:只在一个平台生效的键带平台前缀(mcpp 2026.10.5.1,#766;`auto_export` 在发布前更名为 `windows_auto_export`)。§1 与 §6 引用的字段准入条件改指 docs/90,字段参考改指 docs/04。 |
| 1.12 | 2026-10-05 | mcpp 2026.10.5.2:§9 第 1 条补上带 `[package]` 的工作空间根自己的包;第 10 条把 `[profile.<name>]` 列为根位置的键;新增第 11 条链接 flag 的作用域(#771);新增 §11 链接期优化与导出发现(#770)与 `[test] windows_code_page`;§7 补第 21 至 23 条判据。 |
| 1.13 | 2026-10-06 | mcpp 2026.10.5.3:§4.6 陈述架构段的等同拼写,`x86` 即 `i686`;`i386` 至 `i586` 不与 `i686` 相互替代。 |
