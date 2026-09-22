# 06 —— Feature 与能力

**读者：** 手上有可选内容的作者 —— 一份额外的源、一个额外的依赖，或者在多个后端
之间做选择。

**本章回答的那一个问题：** 一个包怎样提供可选内容，消费方又怎样请求它。

**不在这里：** 一次构建面向哪些设备后端 —— 那看起来像 feature，却不是 —— 它是
[42 —— 异构硬件构建](42-heterogeneous-builds.md)。在此之前：
[05 —— 依赖与解析](05-dependencies.md)。在此之后：[07 —— 工作空间](07-workspace.md)。

Feature 是一个包提供可选内容的方式：一个编译宏、一份额外的源文件、一个额外的
依赖，或者在多个后端之间的一次选择。本章是声明与消费 feature 的参考。

相关文档：[04 —— mcpp.toml](04-mcpp-toml.md) 是 manifest 其余部分的字段参考；
[`examples/11-features`](../../examples/11-features/) 是一个把三种形态都声明了
一遍、并且用 dev-dependency 写了测试的包；[42 —— 异构硬件构建](42-heterogeneous-builds.md)
是这套机制最大的消费方，因为每一条加速器 lane 都是一个 feature。

## `[features]` —— Feature（Cargo 风格，可加性）

```toml
[features]
default = ["base"]        # Default activation set
base    = []
docking = ["extra"]       # Activating docking implies activating extra (transitive closure)
extra   = []
```

- 激活来源：包自己的 `default` 集合，并上显式请求（根包经由
  `mcpp build --features a,b`；依赖经由长形式依赖 spec 的 `features = [...]`
  与 `backend = "..."` 糖衣）。
- 每个被激活的 feature，在该包编译时得到宏 `-DMCPP_FEATURE_<NAME>`（名字转大写，
  非字母数字字符变 `_`，例如 `backend-a` → `MCPP_FEATURE_BACKEND_A`）。
- **严格校验**：目标包声明了 `[features]` 表时，请求一个未声明的 feature 会产生
  warning，在 `--strict` 下是错误。不声明 `[features]` 的包接受任意请求（纯宏用法）。

### 依赖的 feature

`<依赖>/<feature>` 形式的记号，打开某个依赖的一个 feature。依赖以消费方 manifest
所写的键命名（`spike.fw`、`compat.opencv`），记号是可加的：它只打开依赖更多的
部分，从不把依赖本身拉进来。

```toml
[dependencies]
spike.fw = { path = "../fw" }

[features]
windows-installer = ["spike.fw/installer"]   # the same as `forward = [...]` in the table form
```

- **命令行上**(mcpp 2026.9.16.1+),`mcpp build --features spike.fw/installer` 为
  一条命令打开同一个 feature，效果与根的转发相同；`run`、`test`、`pack`、
  `emit build-database` 与 `why deps` 同样接受这个记号。它从不是根自己的 feature,
  也从不变成宏。
- **校验**会读遍写下该转发的那份 manifest 里的每一张依赖表：`[dependencies]`、
  `[build-dependencies]`、`[dev-dependencies]` 与 `[feature-deps.<name>]`，覆盖
  所有行。一个键只在别的行、或未激活的 feature 下声明，仍算已声明；在当前这一行
  上，该转发到达不了任何边，不产生效果。没有任何表声明的键会被报告，`--strict`
  下报告为错误。命令行上的这条检查不论根是否声明 `[features]` 都一样执行。

### 表形式 —— 让 feature 贡献的不止是隐含 feature

`[features]` 的条目除了写成数组，还可以写成**表**，从而让该 feature 除了隐含
feature 之外，再携带包自有的预处理 `defines`、feature 门控的源 glob(`sources`,
mcpp 0.0.95+ —— 列出的 glob 离开默认构建，仅在 feature 激活时才编译，与 index
描述符的 `features.<f>.sources` 完全对等；这正是 vendored 大库最高频的形态：
*feature = 一组源文件 + 一个 define*)、feature 门控的 per-glob 编译旗标
(`flags`,mcpp 0.0.101+)，以及 capability 的 `requires` / `provides`（见下文
*`provides` / `requires`*）：

```toml
[features]
default    = []
# Array shorthand: just implied features.
docking    = ["extra"]
extra      = []
# Table form: contribute a package-owned define when active.
mpl2only   = { defines = ["EIGEN_MPL2_ONLY"] }
# Table form: a define + an implied feature.
fast_math  = { defines = ["APP_FAST=1"], implies = ["extra"] }
# Table form: feature-gated sources + per-glob flags that co-locate with them.
simd       = { sources = ["src/simd/**"], flags = [
                 { glob = "src/simd/**/*.avx2.cpp", cxxflags = ["-mavx2"] } ] }
```

- **表形式恰好接受** `implies`、`forward`、`defines`、`sources`、`flags`、
  `requires`、`provides`。其余键会被报为一条 schema warning 并忽略
  (mcpp 2026.9.1.1+);`deps` 单独报为"保留"，并指向 `[feature-deps.<name>]`。
  该版本之前，`[features]` 是唯一一个完全没有 schema 检查的结构化段落 ——
  把 `include_dirs` 误写进 feature 里会零诊断地构建成功，而同样的错误写在
  `[build]` 里会被报出来。
- `defines` 是**裸**宏名（不带 `-D`）;feature 激活时，每个都在该包自己的编译上
  脱糖为 `-D<x>` —— 与 `[targets.*] defines` 完全一致。按约定，它们仅限于包
  **自有**的、带命名空间的宏：feature **不**注入自由的包级 `cflags`/`ldflags`,
  否则会破坏可加的 feature 并集模型。链接旗标来自 provider 依赖（见下文
  *`provides` / `requires`*），而不是来自 feature。
- 每个激活的 feature 仍会得到自动的 `-DMCPP_FEATURE_<NAME>`,`defines` 叠加在
  它之上。
- `flags`（mcpp 0.0.101+）与 `[build].flags`（[04 §2.3](04-mcpp-toml.md)）共用
  同一套有序、inline-table 数组的文法（`glob` 必填，加 `cflags`/`cxxflags`/
  `asmflags`/`defines`；与 `[[build.flags]]` 一样，也接受
  `[[features.<name>.flags]]` 这种 array-of-tables 拼法）。feature 激活时，
  条目追加在 base `[build].flags` **之后**（各 feature 按名字排序）,"最后一条
  旗标胜出"使 feature 规则能覆盖更宽的 base 规则；未激活时，这些条目根本不存在
  （不会产生死 glob 告警）。这让 feature 的组内专属旗标与它的 `sources` 同居，
  而不必写成 base 规则、在 feature 关闭的构建里留下一条注定命中不到的 glob。
  与 `defines` 不同，feature 的 `flags` 是**私有的、per-TU 的构建旗标**——它们
  从不传播给消费方（与 `[build].flags` 同一契约），因此不破坏可加模型：由 glob
  限定作用面，顺序确定，没有跨包效应。


### 作为构建规则的 feature(mcpp 2026.9.7.1+)

两个键能把一个 feature 变成其他包可以使用的构建规则。它们是消费方只写一条
依赖边、不必写构建程序的原因。

```toml
[features.rules-spirv]
sources           = ["rules/spirv.cppm"]
rule_module       = "mcpp.rules.spirv"
device_extensions = [".comp", ".vert", ".frag", ".glsl"]
```

`device_extensions` 陈述这条规则编译哪些**设备源**扩展名。激活了该 feature 的
消费方会把它们分类为设备源 —— 不做 import 扫描、不产出 BMI、由 mcpp 不驱动的
编译器编译。这与 `[build] module_extensions` 是同一个形状：mcpp 知道设备源
*是什么*，不知道 `.cu` 是 CUDA，所以**一门新的设备语言不需要引擎改动**。
[42 —— 异构硬件构建](42-heterogeneous-builds.md) 里"第六个后端是一个包而不是
一次引擎改动"这句话由此才成立，而不只是愿望；`.slang` 已经从 mcpp 的内置表中
移除，现在正是经由这条路到达的。

`rule_module` 给出消费方的构建程序为够到这条规则而 import 的模块，以及它要调用
的 `compile()` 所在。它是**声明**出来的，而不是从源码扫描出来的，因为那个程序
必须在任何东西被编译**之前**写出来，而一次为了决定写什么、去扫描依赖源码的构建，
会把两者的先后顺序颠倒。

由此得出两件事，而且都没有把任何包名放进 mcpp:

- **`host-module = true` 被隐含。** 一个点名了规则模块的 feature，已经说明那是
  使用它的唯一方式，所以依赖边不必再说一遍。
- **没有 `build.mcpp` 的包会得到一个。** mcpp 把这些规则描述的程序写进构建目录
  并编译它。自带程序的包保留自己的那份：合成只填补缺席，绝不覆盖已有的；而
  生成出来的那份，正是这个工程本来要手写的那份，所以接管它是一次复制加一次
  编辑。

feature 仍然**按名字**请求：

```toml
[build-dependencies.mcpp]
plugins = { version = "0.3.0", features = ["rules-spirv"] }
```

早先的一版设计，从工程源码里出现的扩展名推导这个集合。它被撤销了，原因有二：
两个包可能认领同一个扩展名 —— 第三方写一条处理 CUDA 的规则是会发生的事 ——
而且 manifest 的职责是描述这次构建，派生出来的 feature 集合不再做到这一点。

两个键必须成对出现。只写其一是一条没有任何东西能据以行动的声明，会在解析期
被拒绝，而不是留到消费方的构建里才发现。


## `provides` / `requires` —— 能力（后端选择）

**能力**（capability）是一个共享的抽象名字（例如 `blas`）。包可以 *provide*
（提供）一种能力；feature 可以 *require*（需要）一种能力而不点名某个具体包，
解析器会从依赖图中绑定**恰好一个** provider。这样就能在多个可互换的后端
（OpenBLAS / MKL / …）之间选出一个，而不必把选择写死进库里。

```toml
# A provider package satisfies a capability for any dependent that requires it.
[package]
name     = "compat.openblas"
version  = "0.3.0"
provides = ["blas", "lapack"]
```

```toml
# A consumer requires the abstract capability via one of its features.
[features]
use_blas = { defines = ["EIGEN_USE_BLAS"], requires = ["blas"] }

# When >1 provider is in the graph, pick one (else the build errors and lists them).
[capabilities]
blas = "compat.openblas"     # equivalently: mcpp build --cap blas=compat.openblas

[dependencies]
compat.openblas = "0.3.0"    # the provider must be a real dependency in the graph
```

保留前缀 `mcpp:` 命名本引擎解析的目标侧层，这些名字对照一个闭集做校验。包级的
`requires` 数组承载对称的陈述 —— 某个目标侧层必须解析出什么，本包才可用。

```toml
[package]
name     = "acme.llvm-runtime"
version  = "0.1.0"
provides = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
requires = ["mcpp:compiler=llvm"]
```

对产物 ABI 开关的需求，用 `requires_abi` 陈述，写在包上或某个 feature 上，而不是
写成一层：

```toml
[package]
requires_abi = { threads = true }

[features]
mt = { requires_abi = { threads = true } }
```

只有根 manifest 能设置这个开关（`[target.<selector>.abi]`，见
[22 —— 目标侧](22-target-side.md)）；根未满足的需求会在编译之前被拒绝，拒绝信息
点名包与 feature。安装钩子针对某一个 C++ 标准库编译静态库的包，把该实现陈述为
层需求，即 `requires = ["mcpp:c++-abi=libstdc++"]`，原因见同一章。

作为标准库的包，在 `[build]` 下陈述它的 `std` 模块源，它所需的 flag 在那里与
任何其他构建输入一样，可以条件化。

```toml
[build]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc++"]

[target.'cfg(c-abi = "musl")'.build]
std-module-flags = ["-D_GNU_SOURCE"]
```

五个层、约束它们的规则，以及相应的诊断，见 [22 —— 目标侧](22-target-side.md)。

绑定是**确定性**的：

| 图中某被需要能力的 provider 数量 | 结果 |
|---|---|
| 恰好一个 | 自动绑定（无需配置） |
| `[capabilities]` pin / `--cap` 指定了一个 | 以 pin 为准 |
| 零个 | **报错**：没有包提供 `<cap>` |
| 两个及以上，且未 pin | **报错**并列出候选 —— 绝不静默猜测 |

被绑定 provider 的链接/头文件旗标，经由普通的依赖机制流向消费方；capability
层是那道*选择与校验*步骤，把"静默选错后端"或"缺后端"变成构建期的显式报错。

**绑定选中的是 provider，它不裁剪链接行。** 依赖包的目标文件一律进入消费方的
链接，与它的能力是否被绑定无关。实测：两个包都提供同一能力，都定义
`cap_probe`；未 pin 时，解析按上表报错；用 `[capabilities]` pin 其中一个之后，
构建走到链接器才失败 ——

```
ld: obj/mcpplibs_pa/src/impl.o: in function `cap_probe':
    multiple definition of `cap_probe'; obj/mcpplibs_pb/src/impl.o: first defined here
```

这一点对**多个 provider 定义同一批符号**的能力有影响 —— 一个全程序单例（例如
`operator new`），或一个名字集合固定的 C 接口。对这类能力而言，图中出现两个
provider 是一个**待修的缺陷**，而不是一个可以 pin 的歧义：pin 会把一个点名两个
候选的报错，换成一个点名 mangled 符号的报错。可互换的**库**（各 BLAS 实现导出
不同的符号集合，按链接各自选一个）不受此影响。

### `exclusive` —— 包声明自己是唯一提供者

上一段描述的是一个引擎**看不见**的缺陷：要看出两个 provider 定义了同一批符号，
需要它们的目标文件，而绑定 capability 时那些文件还不存在；而"一律拒绝重复
provider"这条规则，又会打断同一段里那个合法的 BLAS 用例。

所以由包自己声明：

```toml
[package]
name      = "compat.cublas"
provides  = ["gpu-blas"]
exclusive = ["gpu-blas"]
```

两个都提供 `gpu-blas` 的包，只要其中至少一个声明了独占，就在绑定 capability
时被拒绝 —— 在任何东西被编译之前，并点名该能力与双方 provider:

```
error: capability 'gpu-blas' is provided by more than one package, and they
       declare it EXCLUSIVE.
         providers: [compat.cublas, compat.rocblas]
         exclusive: [compat.cublas, compat.rocblas]
       Two implementations of one interface define the same symbols, so the
       link would resolve every call to whichever archive it reached first.
       Keep one of them — a `[capabilities]` pin selects a provider for a
       REQUIREMENT and cannot make two definitions of one symbol safe.
```

这个拒绝在 `--format json` 里报 `exclusive-capability`（见第 11 章）。

### `version-floor` —— 对机器的要求超出它所有

关于一台机器的某些事实约束着能为它构建什么，而忽略它们时的失败来得很晚：一个
针对比它将遇到的驱动更新的运行时构建出来的程序，链接得干干净净，却在第一次
使用时失败，而且错误消息不点名任何一侧。

包声明它需要什么：

```toml
[[runtime.requirements]]
kind  = "version-floor"
value = "cuda.driver >= 12.0"
```

而某个在**安装期**（探测本该发生的地方）确立了机器上某项事实的包，声明它：

```toml
[runtime]
provides = ["cuda.driver=12.4"]
```

mcpp 在绑定 capability 时比较二者，并在任何东西被编译之前拒绝，报
`version-floor-unmet`:

```
error: `toolkitnew` requires cuda.driver >= 13.0, and cuda.driver is stated as 12.4.
         stated by: driverfact
```

**没有任何厂商词汇抵达引擎。** 它读到的是一个名字、一个关系和一个版本；
`cuda.driver` 只是流过它的数据，一个 mcpp 从未听说过的后端，比较方式完全相同。

**没人回答过的下界是沉默的。** 一台从未声明过自己有什么的机器，不是"未满足
下界"的机器，而是"没人问过"的机器。把"我们不知道"变成"不行"，正是这个机制
要避免的那种失败，而且有一条直接判据：`tests/e2e/603_version_floor.sh` 构建了
一个下界指向无人提供之物的工程。

**引擎陈述目标的平台下限**(mcpp 2026.9.14.2+)。在编译器接受一个最低平台版本
的那一行上，引擎以该平台自己的说法，把这个版本陈述为一项事实，包像对待其他
事实一样，对它写下界：

| 事实 | 适用行 | 设定来源 |
|---|---|---|
| `android.api-level` | `*-linux-android` | `[target.<triple>] min_api_level`，否则取工具链支持的最低级别 |
| `ios.deployment-target` | iOS 真机与模拟器各行 | `[build] ios_deployment_target`，否则取定位到的 SDK 版本 |
| `macos.deployment-target` | macOS 各行 | `[build] macos_deployment_target`，否则取 mcpp 在 macOS 上的默认值 |

```toml
[[runtime.requirements]]
kind  = "version-floor"
value = "android.api-level >= 23"
```

```
error: `fw` requires android.api-level >= 23, and this build targets 21.
         set by: [target.x86_64-linux-android] min_api_level
```

不陈述这类事实的行，让这条要求保持沉默，因此要求本身不需要选择器。下限不会替
依赖抬高：这个键设定的值，就是编译器实际面向的值，而应用要安装到哪些设备上，
是应用自己的决定。一个包陈述同名事实，不会替换引擎陈述的那一项。

**这是一条关于本包自己的符号的声明**，所以一条指向本包并不提供的能力的条目，
会被报为一条 schema warning：那里没有可独占的东西。而无人声明独占的能力，
行为完全不变 —— 两个 BLAS 实现照常共存，既有的"两个或更多、未 pin"报错也仍然
只在**有人 require** 该能力时才出现。

## `[feature-deps.<name>]` —— feature 拉取的依赖

在 `[feature-deps.<name>]` 下声明的依赖是**可选的**：仅当该 feature 激活时
（根的 `--features`，或某依赖 spec 的 `features = [...]`）才会被解析。
`[dependencies]` 中的依赖始终被解析；可选性由声明的*位置*表达，而不是由某个
标志位表达。

```toml
[features]
use_blas         = { defines = ["EIGEN_USE_BLAS"], requires = ["blas"] }
backend-openblas = { implies = ["use_blas"] }

# Pulled ONLY when `backend-openblas` is active. Each entry is a full dependency
# spec (version/path/git + its own features).
[feature-deps.backend-openblas]
compat.openblas = "0.3"
```

**写 `"^0.3.0"`，而不是 `"0.3.x"` 或 `"0.3"`。** 以索引中确定存在的一个包做
对照，判据取**构建成功**:

| 写法 | 结果 |
|---|---|
| `cmdline = "0.0.1"` | 构建通过 |
| `cmdline = "^0.0.1"` | 构建通过 |
| `cmdline = "0.0"` | 解析通过，随后 `install path missing after fetch` |
| `cmdline = "0.0.x"` | `E_NOT_FOUND`，点名的是那个存在的包 |

这三种结果值得分开看，因为有两个更弱的判据，各自会放行一种不可用的写法：
"没有 `E_NOT_FOUND`"放行两段式前缀，"解析通过"同样放行它。**只有对着真实索引
构建一次**才能定论。

这一点在此处比在 `[dependencies]` 里更要紧。一个取不回实现的 feature，等于
一个不存在的 feature；而开发期使用**path** 依赖的工程根本不查索引 —— 这个
失败只在发布之后才出现，而且出现在别人身上。

这一机制与能力（见上文 *`provides` / `requires`*）组合使用：单个
`backend-openblas` feature 既**拉取** provider（`compat.openblas`，其
`provides = ["blas"]`），又**开启**消费方的开关（`implies = ["use_blas"]`，
其 `requires = ["blas"]`）。图中只有一个 provider 时，能力自动绑定 ——
消费方只需写 `features = ["backend-openblas"]`。

在索引包的 Lua 描述符中，同样的内容写成内联形式：

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

同样这三件东西，也覆盖了"库希望**提供**一份实现，但不**强加**一份"的情形 ——
一个全程序单例，例如 `operator new`、一个日志 sink、一个 panic handler:

```toml
[features]
default   = []
# The consumer-side switch: "I use the part of this library that needs an allocator".
alloc     = { requires = ["freestanding-allocator"] }
# The built-in default: activating this one is enough.
alloc-kal = { implies = ["alloc"] }

# Resolved only when `alloc-kal` is active, so the library itself carries no
# dependency on the implementation.
[feature-deps.alloc-kal]
std-freestanding-alloc-kal = "0.1.x"
```

三种用法各一行：

| 消费方需要 | manifest 中的写法 |
|---|---|
| 不用会分配的那部分 | `std-freestanding = "0.2.0"` —— 分配器不进图 |
| 内置的默认实现 | `features = ["alloc-kal"]` —— 实现随之进图，不必知道它的包名 |
| 自己的或第三方的实现 | `features = ["alloc"]` 加一个 `provides = ["freestanding-allocator"]` 的包 |

有两条性质，使这个形状优于随包无条件提供实现。随包提供实现的库，替程序做了
一个本该属于程序的决定，而且**撤销不掉**:feature 是**可加的**，消费方没有
把某个默认实现**关掉**的手段。此外，由于依赖包的目标文件无条件参与链接
（见上文 *`provides` / `requires`*），随包的默认实现加上程序自备的一份，
是**重复定义**而不是替换 —— 让 C++ 标准库能提供一个可替换的 `operator new`
的那套归档语义，并不适用于普通的包依赖。把实现放在开关之后，意味着两者
**从不共存**。

### 平台 SDK 依赖保持私有

一个绑定到某个平台的包 —— 它需要那个平台的头文件才能实现某项功能，而不是为了
陈述自己的接口 —— 在 `[feature-deps.<feature>]` 下用 `visibility = "private"`
来依赖那个 SDK:

```toml
[features]
windows-crt = {}

# Resolved only on the row that activates it, and its headers reach ONLY
# this package's own translation units.
[feature-deps.windows-crt]
some.windows-headers = { version = "1.0", visibility = "private" }
```

`visibility` 是任意依赖 spec 的一个字段（默认 `public`，还可以是 `private`
或 `interface` —— 见 [05 —— 依赖](05-dependencies.md)）。让 SDK 不跨越包边界的
正是 `private`：该依赖的头文件目录、宏定义与 flag 只并入本包自己的构建，到此
为止，正如 `privateIncludeDirs` 让一个包**自己的**内部头文件不到达它的消费方。
一个依赖该平台绑定包、并激活 `windows-crt` 的消费方，会得到这项功能；它不会在
自己的 `-I` 列表里得到 `some.windows-headers` 的目录，甚至无法按名字
`#include` 它的头文件。

这正是 [24 —— openkal 与由依赖图供给的目标](24-openkal-cross.md) 为一个需要
超出其已声明层（`kernel-abi`、`c-abi`、`c++-abi`）之外平台头文件的包所指向
的模式：这条依赖是合法的，但它不能变成每一个消费方的问题。把它写成 `public`
（或者不写 `visibility`，二者等价）正是本节要指出的错误 —— 对声明它的包而言，
这样能工作；但这会把 SDK 的头文件不由分说地交给消费方，而消费方构建的目标上
很可能根本不该出现这个 SDK。

#### 让闭包也能看见它，而不只是私有（mcpp 2026.9.18+）

`visibility = "private"` 回答的是"这个依赖会不会泄漏到消费方的 `-I` 列表",
不回答"这个依赖到底在不在图里" —— 后者是
[22 —— 目标侧](22-target-side.md#闭包可见性) 要为**整个构建**回答的问题。
SDK 包自己陈述报告或拒绝所需要的那项事实：

```toml
[package]
name     = "some.windows-headers"
version  = "1.0.0"
provides = ["platform-sdk"]
```

`platform-sdk` 是一个普通的、不带命名空间前缀的能力 —— 像上文的 `blas`，不像
`mcpp:c-abi=<impl>` —— 因为它不指代引擎解析的任何一层，只是包对自己陈述的一项
事实。一次构建的 `Target` 报告会列出图中每一个声明了它的包（没有则为空）;
`[build] platform-dependencies = "refuse"` 会在它出现时直接让构建失败 —— 这是
"本次构建完全是基于其 kernel-abi 实现的闭包，不多不少"这句话的机器可核验形式。
同时声明 `provides = ["platform-sdk"]` 与 `visibility = "private"`，才是完整
的陈述：private 让头文件不出现在消费方的搜索路径上，`platform-sdk` 让这项事实
不从任何人的报告里消失。

## 当前边界

**默认 feature 在 manifest 里关掉，不在命令行上关掉。** 没有
`--no-default-features`。`mcpp build --features metrics` 激活的是
`default ∪ {metrics}`；要在不带 `default` 中某个成员的情况下构建，得改
`[features] default`。实测于 2026.9.8.1。

**依赖不能以加速器为条件。** `accelerator` 是从依赖图中解析出来的，因此由它
选择的依赖会决定它自己所问的那个答案。mcpp 会报告该谓词并忽略它。包要么无
条件，要么以平台为条件；由加速器选择的是 `[build] sources`。
