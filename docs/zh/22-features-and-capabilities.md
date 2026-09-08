# 22 —— Feature 与能力

Feature 是一个包提供可选内容的方式:一个编译宏、一份额外的源文件、一个额外的
依赖,或者在多个后端之间做选择。本章是声明与消费 feature 的参考。

相关文档:[05 —— mcpp.toml](05-mcpp-toml.md) 是 manifest 其余部分的字段参考;
[`examples/11-features`](../../examples/11-features/) 是一个把三种形态都声明了
一遍、并且用 dev-dependency 写测试的包;[20 —— 异构硬件构建](20-heterogeneous-builds.md)
是这套机制最大的消费者,因为每条加速器 lane 都是一个 feature。

## `[features]` —— Feature(Cargo 风格,可加性)

### 表形式 —— 让 feature 贡献的不止是隐含 feature

`[features]` 的条目除了写成数组,还可写成**表**,从而让该 feature 在隐含 feature
之外,携带包自有的预处理 `defines`、feature 门控的源 glob(`sources`,mcpp
0.0.95+——列出的 glob 离开默认构建,仅当 feature 激活时才编译,与 index 描述符的
`features.<f>.sources` 完全对等;这正是 vendored 大库最高频的形态:*feature =
一组源文件 + 一个 define*)、feature 门控的 per-glob 编译旗标(`flags`,mcpp
0.0.101+),以及 capability 的 `requires` / `provides`(见下文*`provides` / `requires`*):

```toml
[features]
default    = []
# 数组简写:仅隐含 feature。
docking    = ["extra"]
extra      = []
# 表形式:激活时贡献一个包自有的宏。
mpl2only   = { defines = ["EIGEN_MPL2_ONLY"] }
# 表形式:宏 + 一个隐含 feature。
fast_math  = { defines = ["APP_FAST=1"], implies = ["extra"] }
# 表形式:feature 门控源 + 与其同居的 per-glob 旗标。
simd       = { sources = ["src/simd/**"], flags = [
                 { glob = "src/simd/**/*.avx2.cpp", cxxflags = ["-mavx2"] } ] }
```

- **表形式恰好接受** `implies`、`forward`、`defines`、`sources`、`flags`、
  `requires`、`provides`。其余键会被报成一条 schema 警告并忽略(mcpp 2026.9.1.1+);
  `deps` 单独报为「保留」(它是计划中的而不是写错的),并指向 `[feature-deps.<name>]`。在该版本之前,`[features]`
  是唯一一个完全没有 schema 检查的结构化段落 —— 把 `include_dirs` 误写进 feature 里
  会零诊断地构建成功,而同样的错误写在 `[build]` 里会被报出来。
- `defines` 为**裸**宏名(不带 `-D`);feature 激活时每个脱糖为 `-D<x>`,加到该包
  自己的编译上——与 `[targets.*] defines` 完全一致。按约定仅限包**自有**的带命名
  空间宏:feature **不**注入自由的包级 `cflags`/`ldflags`,否则会破坏加性的 feature
  并集模型。链接旗标来自 provider 依赖(见下文*`provides` / `requires`*),而非 feature。
- 每个激活的 feature 仍会得到自动的 `-DMCPP_FEATURE_<NAME>`,`defines` 与之叠加。
- `flags`(mcpp 0.0.101+)与 `[build].flags`([05 §2.3](05-mcpp-toml.md))共用同一有序 inline-table 数组
  文法(`glob` 必填,加 `cflags`/`cxxflags`/`asmflags`/`defines`;与
  `[[build.flags]]` 一样也接受 `[[features.<name>.flags]]` 拼写)。feature 激活时
  条目追加在 base `[build].flags` **之后**(feature 按名
  序),"last flag wins" 使 feature 规则可覆盖更宽的 base 规则;未激活时条目根本
  不存在(不会有死 glob 告警)。这让 feature 的组内专属旗标与其 `sources` 同居,
  而不必写成 base 规则、在 feature-off 构建里留下必死的 glob。与 `defines` 不同,
  feature `flags` 是**私有 per-TU 构建旗标**——永不传播给消费者(与 `[build].flags`
  同契约),因此不破坏加性模型:glob 限定作用面、顺序确定、无跨包效应。


### 作为构建规则的 feature(mcpp 2026.9.7.1+)

两个键把一个 feature 变成其它包可以使用的构建规则。它们是消费者只写一条依赖边、
不写构建程序的原因。

```toml
[features.rules-spirv]
sources           = ["rules/spirv.cppm"]
rule_module       = "mcpp.rules.spirv"
device_extensions = [".comp", ".vert", ".frag", ".glsl"]
```

`device_extensions` 陈述这条规则编译哪些**设备源**扩展名。激活了该 feature 的消费者
会把它们分类为设备源 —— 不扫描 import、不产 BMI、由 mcpp 不驱动的编译器编译。这与
`[build] module_extensions` 是同一个形状:mcpp 知道设备源*是什么*,不知道 `.cu` 是
CUDA,所以**一门新设备语言不需要引擎改动**。
[20 — 异构硬件构建](20-heterogeneous-builds.md) 里那句「第六个后端是一个包而不是一次
引擎改动」由此才成立;`.slang` 已从 mcpp 的内置表中移除,现在正是经由这条路到达的。

`rule_module` 给出消费者的构建程序为够到这条规则而 import 的模块,以及它调用的
`compile()` 所在。它是**声明**的而不是从源码扫描出来的,因为那个程序必须在任何东西
被编译**之前**写出来,而一次为了决定写什么而去扫描依赖源码的构建会把两者的顺序颠倒。

由此得出两件事,而且都不把任何包名放进 mcpp:

- **`host-module = true` 被推出来。** 一个点名了规则模块的 feature 已经说过那是使用
  它的唯一方式,所以依赖边不必再说一遍。
- **没有 `build.mcpp` 的包会得到一个。** mcpp 把这些规则描述的程序写进构建目录并编译
  它。自带程序的包保留自己的:合成只填补缺席、绝不覆盖;而生成出来的那份就是这个工程
  本来要手写的那份,所以接管它是一次复制加一次编辑。

feature 仍然**按名字**请求:

```toml
[build-dependencies.mcpp]
plugins = { version = "0.3.0", features = ["rules-spirv"] }
```

早先的一版设计从工程源码里出现的扩展名推导这个集合。它被撤销了,因为两个包可能认领
同一个扩展名 —— 第三方写一条处理 `.cu` 的规则是会发生的事 —— 也因为 manifest 的职责
是描述这次构建,而派生出来的 feature 集合让文件不再陈述它。

两个键必须成对出现。只写其一是一条没有任何东西能据以行动的声明,会在解析期被拒绝,
而不是留到消费者的构建里。


## `provides` / `requires` —— 能力(后端选择)

**capability(能力)** 是一个共享的抽象名字(如 `blas`)。包可以 *provide*(提供)
一种能力;feature 可以 *require*(需要)一种能力而非点名某个具体包,解析器会从依赖
图中绑定**恰好一个** provider。这样就能在多个可互换后端(OpenBLAS / MKL / …)中选其
一,而不必把选择写死进库里。

```toml
# provider 包为任何 require 它的依赖方满足某能力。
[package]
name     = "compat.openblas"
version  = "0.3.0"
provides = ["blas", "lapack"]
```

```toml
# 消费方经由自己的某个 feature 来 require 这个抽象能力。
[features]
use_blas = { defines = ["EIGEN_USE_BLAS"], requires = ["blas"] }

# 图中有 >1 个 provider 时,选其一(否则构建报错并列出候选)。
[capabilities]
blas = "compat.openblas"     # 等价于:mcpp build --cap blas=compat.openblas

[dependencies]
compat.openblas = "0.3.0"    # provider 必须是图中真实存在的依赖
```

保留前缀 `mcpp:` 命名本引擎解析的目标侧层,这些名字对照一个闭集校验。
包级 `requires` 数组承载对称的陈述 —— 某个目标侧层必须解析为什么,
本包才可用。

```toml
[package]
name     = "acme.llvm-runtime"
version  = "0.1.0"
provides = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
requires = ["mcpp:compiler=llvm"]
```

作为标准库的包在 `[build]` 下陈述它的 `std` 模块源,
其所需的 flag 在那里与任何其它构建输入一样可条件化。

```toml
[build]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc++"]

[target.'cfg(c-abi = "musl")'.build]
std-module-flags = ["-D_GNU_SOURCE"]
```

五个层、约束它们的规则与相应诊断,见 [14 - 目标侧](14-target-side.md)。

绑定是**确定性**的:

| 图中某被需要能力的 provider 数量 | 结果 |
|---|---|
| 恰好一个 | 自动绑定(无需配置) |
| `[capabilities]` pin / `--cap` 指定了一个 | 以 pin 为准 |
| 零个 | **报错**:没有包提供 `<cap>` |
| 两个及以上且未 pin | **报错**并列出候选——绝不静默猜测 |

被绑定 provider 的链接/头文件旗标经由常规依赖机制流到消费方;capability 层是那道
*选择与校验* 步骤,把"静默选错后端 / 缺后端"变成构建期的显式报错。

**绑定选中的是 provider,它不裁剪链接行。** 依赖包的目标文件一律进入消费方的链接,
与它的能力是否被绑定无关。实测:两个包都提供同一能力且都定义 `cap_probe`,未 pin 时
解析按上表报错;按提示用 `[capabilities]` pin 其中一个之后,构建走到链接器才失败——

```
ld: obj/mcpplibs_pa/src/impl.o: in function `cap_probe':
    multiple definition of `cap_probe'; obj/mcpplibs_pb/src/impl.o: first defined here
```

这一点对**多个 provider 定义同一批符号**的能力有影响 —— 全程序单例(例如
`operator new`),或名字集合固定的 C 接口。对这类能力,图中出现两个 provider 是**待修的
缺陷**而非可 pin 的歧义:pin 会把一个点名两个候选的报错,换成一个点名 mangled 符号的报错。
可互换的**库**(各 BLAS 实现导出不同的符号集合,按链接选其一)不受此影响。

### `exclusive` —— 包声明自己是唯一提供者

上一段描述的是一个引擎**看不见**的缺陷:要看出两个 provider 定义了同一批符号,
需要它们的目标文件,而绑定 capability 时那些还不存在;而「一律拒绝重复 provider」
又会打断同一段里那个合法的 BLAS 用例。

所以由包自己声明:

```toml
[package]
name      = "compat.cublas"
provides  = ["gpu-blas"]
exclusive = ["gpu-blas"]
```

两个都提供 `gpu-blas` 的包,只要其中至少一个声明了独占,就在绑定 capability 时
被拒绝 —— 在任何东西被编译之前,并点名该能力与双方:

```
error: capability 'gpu-blas' is provided by more than one package, and they
       declare it EXCLUSIVE.
         providers: [compat.cublas, compat.rocblas]
         exclusive: [compat.cublas, compat.rocblas]
```

该拒绝在 `--format json` 里报 `exclusive-capability`(见第 11 章)。

### `version-floor` —— 对机器的要求高于它所有

有些关于机器的事实约束着能为它构建什么,而忽略它们时的失败来得很晚:
一个针对比它将遇到的驱动更新的运行时构建出来的程序,**干净地链接**,
在第一次使用时失败,而消息里两侧都没有。

包声明它需要什么:

```toml
[[runtime.requirements]]
kind  = "version-floor"
value = "cuda.driver >= 12.0"
```

而某个在**安装期**(探测该发生的地方)确立了机器某项事实的包,声明它:

```toml
[runtime]
provides = ["cuda.driver=12.4"]
```

mcpp 在绑定 capability 时比较二者,并在任何东西被编译之前拒绝,
报 `version-floor-unmet`:

```
error: `toolkitnew` requires cuda.driver >= 13.0, and this machine has 12.4.
         stated by: driverfact
```

**没有任何厂商词汇抵达引擎。** 它读到的是一个名字、一个关系和一个版本;
`cuda.driver` 是流过的数据,一个 mcpp 从未听说过的后端比较方式完全相同。

**没人回答的下界是沉默的。** 一台从未声明自己有什么的机器,不是「未满足下界」的机器,
而是「没人问过」的机器。把「我们不知道」变成「不行」正是这个机制要避免的失败,
并且有直接判据:`tests/e2e/603_version_floor.sh` 会构建一个下界指向无人提供之物的工程。

**它是关于这个包自己的符号的声明**,所以一条指向本包并不提供的能力的条目会被报为
schema 警告:那里没有可独占的东西。而无人声明独占的能力行为完全不变 —— 两个 BLAS
实现照常共存,既有的「两个或更多、未 pin」报错也仍然只在**有人 require** 该能力时出现。

## `[feature-deps.<name>]` —— 由 feature 拉取的依赖

在 `[feature-deps.<name>]` 下声明的依赖是**可选的**:仅当该 feature 激活时(根 `--features`,
或某依赖 spec 的 `features = [...]`)才会被解析。`[dependencies]` 中的依赖始终被解析;
可选性由声明的*位置*表达,而非某个标志位。

```toml
[features]
use_blas         = { defines = ["EIGEN_USE_BLAS"], requires = ["blas"] }
backend-openblas = { implies = ["use_blas"] }

# 仅当 `backend-openblas` 激活时才拉取。每个条目都是完整的依赖 spec
#(version/path/git + 其自身的 features)。
[feature-deps.backend-openblas]
compat.openblas = "0.3"
```

**写 `"^0.3.0"`,而不是 `"0.3.x"` 或 `"0.3"`。** 以索引中确定存在的包作对照,
判据取**构建成功**:

| 写法 | 结果 |
|---|---|
| `cmdline = "0.0.1"` | 构建通过 |
| `cmdline = "^0.0.1"` | 构建通过 |
| `cmdline = "0.0"` | 解析通过,随后 `install path missing after fetch` |
| `cmdline = "0.0.x"` | `E_NOT_FOUND`,点名的是包 —— 而该包存在 |

这三种结果值得分开,因为两个更弱的判据各自会放行一种不可用的写法:
「没有 `E_NOT_FOUND`」放行两段前缀,「解析通过」同样放行它。**只有对着真实索引构建
一次**才能定论。

这一点在此处比在 `[dependencies]` 中更要紧:**实现取不回来的 feature 等于不存在的
feature**,而开发期使用 **path** 依赖的工程根本不查索引 —— 该失败只在发布之后才出现,
而且是出现在别人身上。

该机制与能力(上文*`provides` / `requires`*)组合:单个 `backend-openblas` feature 既**拉取** provider
(`compat.openblas`,其 `provides = ["blas"]`),又**开启**消费方开关
(`implies = ["use_blas"]`,其 `requires = ["blas"]`)。当图中只有一个 provider 时,
能力自动绑定——消费方只需写 `features = ["backend-openblas"]`。

在索引包的 Lua 描述符中,等价写法为内联形式:

```lua
features = {
    use_blas         = { defines = { "EIGEN_USE_BLAS" }, requires = { "blas" } },
    ["backend-openblas"] = {
        implies = { "use_blas" },
        deps    = { ["compat.openblas"] = "0.3.x" },
    },
}
```

### 保持可替换的默认实现

同样这三件东西,也覆盖"库希望**提供**一份实现但不**强加**一份"的情形 —— 全程序单例,
例如 `operator new`、日志 sink、panic handler:

```toml
[features]
default   = []
# 消费方开关:"我用到了本库中需要分配器的那部分"。
alloc     = { requires = ["freestanding-allocator"] }
# 内置默认:激活它就够了。
alloc-kal = { implies = ["alloc"] }

# 仅在 `alloc-kal` 激活时解析,因此库本体不携带对该实现的依赖。
[feature-deps.alloc-kal]
std-freestanding-alloc-kal = "0.1.x"
```

三种用法各一行:

| 消费方需要 | 清单里怎么写 |
|---|---|
| 不用会分配的那部分 | `std-freestanding = "0.2.0"` —— 分配器不进图 |
| 默认实现 | `features = ["alloc-kal"]` —— 实现随之进图,**无需知道其包名** |
| 自己的或第三方的 | `features = ["alloc"]` 加一个 `provides = ["freestanding-allocator"]` 的包 |

有两条性质使该形状优于无条件随包提供实现。随包提供实现的库替程序做了本属程序的决定,
而且**撤销不掉**:feature 是**加性**的,消费方没有把某个默认**关掉**的手段。以及,由于
依赖包的目标文件无条件参与链接(上文*`provides` / `requires`*),随包的默认加上程序自备的那份是**重复定义**
而非替换 —— 让 C++ 标准库能提供可替换 `operator new` 的那套归档语义,对包依赖并不适用。
把实现放在开关之后,意味着两者**从不共存**。
