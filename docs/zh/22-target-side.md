# 22 —— 目标侧

**读者:**要用同一份 manifest 服务多个目标的作者。

**本章回答的那一个问题:**manifest 怎样说「只在那里」,以及哪些东西可以这样被
条件化。

**不在这里:**目标名的词汇表,那是 [21 —— 目标三元组](21-the-target-triple.md);
以及加速器这条轴 —— 它在图之后才解析,属于
[42 —— 异构硬件构建](42-heterogeneous-builds.md)。

一次构建在发出任何命令行之前必须回答一个问题:目标的编译器运行时、平台接口、
C 库与 C++ 运行时从哪里来。mcpp 在依赖图解析完成之后解析该问题一次,
其后的每一个阶段读取同一个结果。

本文规定该模型、约束它的规则、工程需要书写的内容,以及包需要声明的内容。

## 五个层

一次构建的目标侧由五个层构成。

| 层 | 内容 | 实现举例 |
|---|---|---|
| `compiler` | 执行编译的程序 | `llvm`、`gcc`、`msvc` |
| `compiler-runtime` | 编译器自身的运行时:整数与浮点 builtins、展开器 | `compiler-rt` 与 `libunwind`、`libgcc` |
| `kernel-abi` | 平台接口或其等价物 | `linux`、`windows`、`darwin`、`openkal` |
| `c-abi` | C 库 | `glibc`、`musl`、`picolibc`、`ucrt`、`libSystem` |
| `c++-abi` | C++ 库及其 ABI 运行时 | `libc++` 与 `libc++abi`、`libstdc++`、MSVC STL |

### 成为层的判据

一个部件成为层,当三个条件同时成立:野外至少存在两个可互换的实现;
它可以独立于相邻层被替换;它与其下方的层之间存在确定的「曾为谁配置」关系。
三者缺一,该部件属于相邻的层而非独立的一层。

`compiler-runtime` 独立于 `c++-abi`,因为 builtins 是一个 C 程序所需要的东西。
把它算作 C++ 运行时的一部分,等价于断言 C 程序不需要整数除法,
而该断言已经产生过一次实测缺陷:一个交叉到 macOS 的 C 程序被询问是否存在
C++ 运行时,回答不存在,链接行因而保留了编译器载荷自带的 `libc++`。

`kernel-abi` 在传统栈上没有名字 —— 在那里,C 库直接发出系统调用或直接调用
平台入口。命名该接缝,是一份 C 库实现能够坐落在多个平台之上的前提。

## 四种来源

每一层由四种来源之一供给。

| 来源 | 含义 | 可知时刻 |
|---|---|---|
| `payload` | 编译器载荷自带 | 依赖解析之前 |
| `prebuilt` | 一份被点名的预制载荷供给 | 依赖解析之前 |
| `graph` | 依赖图中的包供给 | 依赖解析之后 |
| `—` | 无人供给,且这是一个陈述 | — |

四种来源中两种在依赖解析之前可知、两种在其之后才可知。因此目标侧只在图存在的
那一点解析一次。更早的任何推断都是对一个尚不存在的事实作推断,
而对该事实的多处独立推断不会一致。

缺席的层是答案而非缺口。裸机目标没有内核;不依赖任何 C 库的工程没有 C 库。

## 三条规则

### 每层恰好一个供给者

C 库、平台接口与 C++ 运行时是互斥的选择,而非可叠加的贡献。
同一层出现两个供给者是错误,在解析期报出,并同时指出两个包及各自进入图的路径。

严格性的依据是失败模态:选错供给者不会使链接失败,
它产出一个能够运行且间歇性不能运行的程序。

### 为其下方的层配置过

一个实现只有在它曾被配置的层之上才可用。一份 `libc++` 构建把该配置记录在
自己的 `__config_site` 中;一份 `libgcc` 构建是为 GCC 配置的。
该关系由声明得来而非由推断得来 —— 见下文 [`requires`](#requires)。

由此得出两条推论。编译器载荷的 C++ 运行时,仅在 C 库同样来自该载荷时可用。
编译器运行时必须属于编译器自身的族,因为二者不一致的构建解析 `__udivti3`
的方式,将与同一程序中其它每一次链接不同。

### 跨来源接线

引擎仅在两层来自不同来源时为它们接线。

| 组合 | 关系的表达方 | 引擎 |
|---|---|---|
| 两层均来自 `graph` | 包之间的普通依赖 | 不介入 |
| 两层均来自 `payload` | 载荷自身一致 | 不介入 |
| 一层预制、一层来自图 | 只有引擎同时知道两边的地址 | 需要接线 |

因此把一层从预制载荷移入依赖图,减少而非增加引擎的工作。
这是一份源码在引擎不作改动的情况下到达多个平台的机制依据。

### 层名固定,实现不固定

五个层名是编译进引擎的闭集。填充它们的实现出现在包清单与索引中,
不出现在引擎的任何一行代码中。

层名可以固定,因为层由 C 与 C++ 的构建模型决定且不增长。
实现不可以,因为增长正是它们所做的事:一个生态的组合数是其实现数之积,
而包数是其和。

## 工程书写的内容

层名不出现在工程清单中。工程通过三个既有机制表达它的目标侧。

### 目标三元组

`--target <三元组>`,或 `[build] target`。OS 段选择平台接口。
env 段陈述一条对 C 库的请求;它是请求而非答案,解析出的值由构建报告。

省略该段即为不陈述:`x86_64-linux` 请求「供给该层的任何实现」,
`x86_64-linux-musl` 请求 musl。依赖图供给了另一个时以图为准,
构建会报出该名字不准确并给出应当使用的拼写。该请求是被忽略而非被违反,
因此产物两种写法下相同。

### 工具链

`mcpp toolchain default <族>@<版本>`、清单中的 `[toolchain]`,
或针对单一目标的 `[target.<三元组>].toolchain`。它选择 `compiler` 层 ——
唯一一个任何包都不能供给的层。

目标表的行可以携带一条约定,即其载荷供给该目标 C 库的工具链。
该约定在两个条件同时成立时生效:清单对该目标未作陈述,**且**依赖图中没有任何
东西供给该目标的系统。第二个条件只有在解析之后才可知,
因此工具链在那之后解析,而不在那之前。

### 依赖

其余每一层均通过依赖一个供给它的包来选择。一条依赖可以供给多个层,
也可以通过它自身的依赖带来更多供给者。

```toml
[dependencies]
openkal-llvm-runtime = "0.1"
```

## 构建报告的内容

构建打印它解析出的结果。清单中的一行陈述一个意图,该意图在其下方的包发生变化
时过期;报告陈述结果,因而不会过期。

默认情况下报告只列出编译器载荷未供给的层。零配置构建的五个层全部解析自同一份
载荷,五行 `(payload)` 回答的是无人提出的问题。

```
      Target x86_64-linux-gnu
```

```
      Target x86_64-windows-gnu → x86_64-w64-windows-gnu
             kernel-abi        openkal        (openkal-windows@0.1.3, graph)
             c-abi             musl           (openkal-musl@0.3.3, graph)
             c++-abi           libc++         (openkal-llvm-runtime@0.1.1, graph)
```

`MCPP_VERBOSE=1` 列出全部五层。诊断始终列出该判断所依据的每一层,
包含其中平凡的部分,因为省略证据的错误信息无法被其读者复核。

接口与实现是两列。`openkal` 是接口,`openkal-windows` 是它的一个实现;
合并二者会掩盖一份源码为何能够到达多台机器。

## 闭包可见性

上面五层的报告回答的是「每一层来自哪里」,不回答图中「还有哪些别的包」——一个绑定到某个
平台 SDK 的依赖,对这份报告而言和普通依赖一样不可见。两种手段补上这个缺口
(设计 2026-09-18 §6)。

**什么算平台依赖,精确定义。** 一个包带来平台依赖,当且仅当它自己这样说——
`provides = ["platform-sdk"]`,一个普通的、不带命名空间前缀的能力。这里的一切都不是从
头文件路径、链接 flag 或某个依赖的 `visibility` 推断出来的:推断会带来与保留前缀
`mcpp:` 为五层所要避免的完全同一种「拼错即静默失效」的失败模式,只是用在了这个引擎本来
就无法直接观测的事实上。[06 —— 让平台 SDK 依赖保持私有](06-features-and-capabilities.md#平台-sdk-依赖保持私有)
是这样的包自己清单所遵循的模式。

**报告。** 构建的 `Target` 报告新增一行,点名图中每一个声明了 `platform-sdk` 的包,
没有则为空:

```
      Target              platform-deps     —
```

```
      Target              platform-deps     some.windows-headers@1.0.0
```

按与五层相同的可见性规则打印:只在有内容可报告时打印,或在 `MCPP_VERBOSE` 下总是打印。

**拒绝开关。** `[build] platform-dependencies = "refuse"` 在图中存在这样的包时直接让
构建失败——这是「本次构建完全是基于其 kernel-abi 实现的闭包,不多不少」这句话的机器
可核验形式:

```toml
[build]
platform-dependencies = "refuse"
```

唯一接受的取值是 `"refuse"`;不写(默认)则允许平台依赖,即今天的行为。

## 包声明的内容

### provides

供给某一层的包在保留前缀 `mcpp:` 下声明它。

```toml
provides = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
```

语法为 `mcpp:<层>[=<实现>]`。层名对照闭集校验;拼写错误是错误,
而非一个被静默禁用的行为。前缀之外的名字属于特性系统,原样透传。

`mcpp:compiler` 可以被 require,不能被 provide。编译器是本引擎安装并驱动的
载荷,而族与族之间的差异 —— flag 拼写、模块模型、BMI 格式、驱动配置文件 ——
是引擎必须持有的事实,而非包能够描述的数据。

### requires

`requires` 是对称的另一半,也是在引擎中不出现实现名的前提下执行分层规则的机制。

```toml
requires = ["mcpp:compiler=llvm"]
```

由 `libc++` 源码构建的 C++ 运行时,其编译与其模块的编译均由 Clang 完成。
该事实属于包。引擎检查一条它能够一般性地陈述的关系 ——
被命名的层必须解析为被命名的实现 —— 并通过同时指出二者来报出不匹配;
一张编译进引擎的族表,对于它从未听说过的族无法做到这一点。

该检查在编译开始之前运行。它所拒绝的组合,否则将在该运行时自身的头文件深处失败,
其消息命名一个读者从未打开过的文件,以及一个 mcpp 从未作出的决定。

同一条声明也是「安装钩子从源码编译静态库」这类包的做法。这样的库针对某一个 C++ 标准库编译,无法链接进
使用另一个标准库的程序;而它安装到的存储目录按包名与版本区分,并不区分这一选择。因此这类包声明它所针对
的实现:

```toml
requires = ["mcpp:c++-abi=libstdc++"]
```

工具链解析出另一个 `c++-abi` 的工程随后会被拒绝,拒绝信息同时指出两个实现,而不是在链接时失败。这项检查
在工具链解析之后进行,而工具链在依赖图安装之后才解析,因此检查时安装钩子已经运行过;钩子收到本次构建的
目标,但收不到工具链的取值([32 —— 编写载荷](32-authoring-a-payload.md))。钩子不得把另一种变体构建进
同一个存储目录,否则第一个消费者就会替之后所有消费者决定变体。

### `c-abi` 包陈述它呈现的 C 环境(mcpp 2026.9.18+)

传统技术栈不需要陈述这件事:编译器载荷的目标三元组已经蕴含了环境。一旦某个包取代载荷供给
C 库,这一点就不再成立——openkal-musl 在 `x86_64-windows-gnu` 上生成 PE/Win64 代码,却向源码
呈现 POSIX 环境,因为它是 musl 的移植版,它之上每一处 `#ifdef _WIN32` 问的都是错误的层。
`[c-abi]` 块就是 C 库一次性陈述它到底呈现什么——且只有提供该层的包可以陈述。

```toml
# openkal-musl 的清单
[package]
provides = ["mcpp:c-abi=musl"]

[c-abi]
presents   = "posix"        # posix | windows | none
data-model = "arch-default" # arch-default | lp64 | llp64 | ilp32
wchar      = 32              # 16 | 32
builtins   = "iso"           # iso | platform(默认 platform)
```

**谁有资格声明。** 写了 `[c-abi]` 却没有在 `provides` 里列出 `mcpp:c-abi=<impl>` 的包,是在
陈述一件自己不提供的层的事实——这总是错的,而不只是不寻常,因此在清单解析阶段即被拒绝,
并指出缺失的 `provides` 条目。

**四个键与各自的封闭取值集合。**

| 键 | 取值 | 回答 |
|---|---|---|
| `presents` | `posix` / `windows` / `none` | 源码看到哪一族环境身份宏(`__unix__` 还是 `_WIN32` 还是都不定义) |
| `data-model` | `arch-default` / `lp64` / `llp64` / `ilp32` | `long` 有多宽 |
| `wchar` | `16` / `32` | `wchar_t` 有多宽 |
| `builtins` | `iso` / `platform`(默认) | 编译器是否可以假定平台 C 库自己的扩展在场 |

`presents`、`data-model`、`wchar` 没有默认值:块里漏写其中一个即被拒绝,并指出缺失的键——
「没写」不等于三个封闭取值中的任何一个。只有 `builtins` 有默认值 `platform`,即今天的行为。
未知的键或未知的取值永远是解析错误,并点名该键——绝不静默忽略。**不声明 `[c-abi]` 块的包
不改变任何东西**:解析出的目标侧、每条编译命令、每个缓存键,都与这项能力出现之前逐字节相同。

三件事互不推导,这是刻意保持分开的:`presents` 决定源码走哪条分支,`data-model`/`wchar`
决定 ABI。POSIX 不蕴含 LP64(32 位架构上是 ILP32),LP64 也不蕴含 POSIX。

**实现(realisation)。** 一旦 `c-abi` 层解析到声明了这个块的包,mcpp 就把请求转换成编译器
配置,作用于目标侧的每一个编译单元——C 库自己、C++ 运行时、编译器运行时的 builtins,以及
图中所有普通包——覆盖 C、C++、汇编编译,依赖扫描,以及 `std` 模块预编译。汇编(`.S`/`.s`)要
做到这一点需要自己单独的广播通道:`.S` 单元的命令行是独立组装的(`mcpp.build.flags::
CompileFlags::as`,不是 `::cc`/`::cxx`),而且它有意只从包的 C 标志里取出 `-D`/`-U`/`-I`
子集——对 C 编译器有意义的 `-std=` 或 `-O` 之类标志对 GAS 毫无意义——所以实现出来的环境
令牌(`--target=`、`-f[no-]short-wchar`,以及 `builtins = "iso"` 添加的部分)要原样再广播
一遍进这条更窄的通道(openkal-musl 尖峰实验发现并修好的缺口:同一个包里 `.c` 单元看到
`_WIN32` 未定义,`.S` 单元却仍看到它已定义——真实代码,比如 `okm_setjmp.S` 与上游
libunwind 的 `assembly.h`,正是按这个宏来选寄存器保存集的)。mcpp 保存的是一份
「请求到三元组与开关」的映射表,是不含包名的通用知识:

| 目标 | 请求 | 实现 |
|---|---|---|
| Linux | `posix` / `arch-default` | 默认三元组已经满足 |
| macOS | `posix` / `arch-default` | 默认三元组已经满足 |
| Windows | `posix` / `arch-default` | 采用 Cygwin 式语义:仅在编译行加 `--target=x86_64-pc-cygwin`;`__CYGWIN__`/`__CYGWIN32__` 保持定义(见下方说明);`data-model` 变为 LP64 是三元组切换的结果,不是另一个开关 |
| 任意目标 | `builtins = "iso"` | 关闭代码生成阶段假定平台 C 库在场的惯用法识别——本轮实测到的唯一一例是 Apple 目标上的 `-fno-builtin-memset_pattern16`;`src/toolchain/cenv.cppm` 记录了还核实过哪些、结论是不适用 |
| 其余情况 | | 明确拒绝,点名目标、请求与缺什么——不静默降级 |

Windows 一行是旗舰情形:`x86_64-w64-windows-gnu` 与 `x86_64-pc-cygwin` 生成的机器码完全一致——
同样的 PE 格式、同样的 Win64 调用约定、同样的 SEH——差别只在预处理器看到什么、`long` 有多宽。
因此实现只触及**编译**行;**链接**行保持图解析出的三元组,因为目标文件格式没有变化。

**`__CYGWIN__`/`__CYGWIN32__` 保持定义——这是 openkal-musl 尖峰实验带来的修订,不是设计
最初的陈述。** 最初试过取消定义它们,理由是图里没有真正的 Cygwin 用户态。第三方可移植
代码里,需要知道**目标文件格式**——不是 C 环境,也不是平台 API——的那部分,没有别的名字
能指代「PE 格式加呈现 POSIX 的 C 环境」这个组合,只有 `__CYGWIN__`;这样的代码不像本生态
自己的包那样可以打补丁。`presents = "posix"` 回答的是一个问题——源码看到哪些环境身份宏;
它不能顺带删掉唯一能回答另一个问题——这是什么目标文件格式——的宏。这是一项**留给 30 个
成员那轮实测去判定的权衡,不是已经定论的事实**:一个库伸手去够 `__CYGWIN__`,也可能伸手
去够一个这里并不存在的真正 Cygwin 接口(`sys/cygwin.h`、`cygwin_conv_path`)——如果定义它
带来的新失败比修好的还多,结论就会翻过来。

**`kernel-abi` 提供者的自身单元被推导落到平台边界上——它不必自己说出来**
(mcpp 2026.9.18+,PR 进行中根据 openkal-musl 尖峰实验做的修订)。提供
`mcpp:kernel-abi=<impl>` 的包(比如 openkal-windows)必须看到平台自身的
环境——它要 include 平台声明,`_WIN32` 对它必须为真——而且永远如此,这是
由定义决定的:这样的包的全部工作就是说平台自己的 ABI,所以它绝不可能是那个
想要图里「呈现」的 `[c-abi]` 环境、而不是三元组自身环境的包。mcpp 不等着被
告知这一点。任何 `provides` 里点名 `mcpp:kernel-abi=<impl>` 的包,默认就得到
`c-environment = "platform"`,不需要自己声明:

```toml
[package]
provides = ["mcpp:kernel-abi=openkal"]
# 没有 [package] c-environment 这一行——边界是从 provides 推导出来的
```

**为什么要推导,而不只是提供这个开关。** 开关本身没问题;它做不到的是追溯性地
修好一个已经发布、却没写这个开关的包。openkal-windows 0.8.0、openkal-macos
0.10.0、openkal-linux 0.13.0,以及未来任何 kernel-abi 实现,都能因此把边界
做对——不需要新发版本,也不需要跨仓库协调版本号——因为 `provides =
["mcpp:kernel-abi=<impl>"]` 正是它们本就已经声明的那一个事实。这里堵住的
失败是实测出来的,不是假设:openkal-windows 和图里其余部分一样,在 POSIX
替换下编译,`-fno-short-wchar` 给了它 32 位的 `wchar_t`,而它调用的 Win32
接口回传的却是真正的 16 位 UTF-16——于是一个 `wchar_t*` 循环把两个 UTF-16
码元读成了一个码点。把这条边界变成默认值,而不是一个包必须记得去写的清单
键,让这一类失败变得**无法被表达**,而不只是被记录在文档里。

**优先级:包自己清单里显式写的 `c-environment` 永远赢过推导。**
推导只在包什么都没写的时候才填 `cEnvironment`——如果一个包终究还是需要
呈现的环境,它仍然可以显式这样声明(今天还没有办法反着写「不是
platform」,因为 `"platform"` 仍是这个键唯一接受的取值)。这个显式键也仍然
是 §5.3 另一类情形——不是 kernel-abi 边界、但自身确实有平台绑定单元的普通
包——唯一的手段,那种情形下作者做出的确实是 mcpp 无法推导出来的选择:

```toml
[package]
# 一个普通包,不是 kernel-abi 提供者——这一个引擎推导不出来;作者要自己
# 声明它,因为平台绑定单元只是这个包所构建内容里真正的少数(设计 §5.3)
c-environment = "platform"
```

这是一条边界规则,由这一个开关记录下来,而不是引擎强制执行:这样的包对图其余部分暴露的
接口仍然只能用定宽类型(SPEC §5.4)。

**声明被校验,而不是被信任。** 声明要经过核对,绝不直接信任——这与 openkal 自己核对一致性声明
的做法一致。目标侧解析出上述开关之后,mcpp 用它们编译一个纯预处理探针(`-E -dM`,把预定义宏
全部打印出来——足够便宜,且不需要执行,这一点很重要,因为解析出的环境常常是交叉目标),读回
`__SIZEOF_LONG__`、`__SIZEOF_WCHAR_T__` 以及哪些环境身份宏被定义,与声明核对。不符即失败,并
同时打印声明值与实测值。结果按配置(编译器二进制身份 + 最终参数)缓存,同一配置解析两次只
编译一次探针。

**指纹。** 解析出的环境参与构建指纹(`compileFlags`,§92 的第 7 项):C 库声明 `lp64` 与
`llp64` 的两次构建,从同一份源码编译出 `long` 宽度不同的目标文件,因此二者绝不共享输出目录,
也不会复用对方产出的目标文件缓存。

**全局构建缓存的键也覆盖了这一点(mcpp 2026.9.18+,PR 进行中的修订,不是设计原文)。**
`~/.mcpp/build-cache/v1`——一次普通依赖编译跨项目、也跨 `mcpp` 升级复用的缓存——是与上面
构建指纹分开的另一套机制,按包逐一取键,只取真正到达该包自身编译命令行的那些轴
(`mcpp.build.cache_key`)。解析出的环境到达一个包的命令行,完全是通过引擎的**广播**
(与 `targetSideUsage`、`-D__openkal__` 同一条通道),从来不经过包自己声明的
`[build] cflags`/`cxxflags`——所以键的推导本身也得被告知去读广播后的值,而不只是声明的值。
这个缺口正是这样被发现的(协调者反馈,openkal-musl 尖峰实验):原地升级 `mcpp`、缓存目录
未清理时,给按新环境构建的镜像喂了按**旧**解析环境编译出的目标文件——一个镜像里混了两种
C 环境,而且没有任何诊断。`fill_package_config` 现在把 `PackageRoot::privateBuild.cflags`/
`cxxflags`/`asmflags`——广播之后的值——和包自己声明的标志一起折进键里,做法与它原本处理
include 目录的方式完全一致。`--cache=off`,或者干脆清空缓存目录,从来都不是键本身正确
的信号——这两条路径都是绕开了这个键,而不是证明了它。

**这次发布还把缓存的 epoch 提了一版,让已有条目全部作废——升级后第一次构建会是冷构建。**
键改对了,不代表用旧的、错误推导方式写下的条目就可以留着继续信:一个条目被污染,恰恰是
因为它记录的键和它实际编译时的输入从一开始就对不上——而修好之后最可能仍然拿到不变的键
的那个包,正好是这次修订里新推导进 `c-environment = "platform"` 的那一类(上文的
kernel-abi 提供者):它的 `privateBuild.cflags` 现在是空的,新键因此是从空内容算出来的,
跟旧键(同样是从空内容算出来的)一样;而磁盘上那份目标文件,却是带着替换令牌编译出来的。
没有更便宜的办法能把修复前写下的条目和修复后写下的条目分开,所以
`mcpp.build.cache_key::kCacheEpoch` 往上提了一版(2 → 3),让整个缓存无条件作废,而不是
去相信一个恰恰在最要紧的那些条目上靠不住的键相等判断。

**存储键——尚未补上,这里把它到底意味着什么讲清楚(对照真正写入 store 的那条代码路径核实
过,mcpp 2026.9.18+)。** 一个包的**安装钩子**能够、也确实会编译目标侧代码——目标文件、
静态库——而它装进的共享 store 只按包名与版本取键,这与 [requires](#requires) 已经记录的
C++ 运行时选择缺口同形。它和上面构建缓存的键不同、也不是这一个 PR 能用同样方式补上的地方
在于:安装钩子运行在工具链解析**之前**,这是真实的顺序约束,不是遗漏。`install_hook_env`
在正常路径上工具链相关的字段一律为空——`prepare.cppm` 要等依赖图装完之后才解析 `tc`,因为
解析目标侧本身可能要依赖图最终供给了哪个包的哪一层(`c-abi` 提供者正是这个图里的一个成员)。
钩子没有办法去问这次构建解析出了什么环境,因为在它运行的那一刻,压根还没有算出来——不是
mcpp 忘了传给它,是那时候真的没有可传的东西。

**失败模式,直说,不当成缺口表里的一行:** 一个编译了对环境敏感的 C 代码的钩子(凡是正确性
依赖 `wchar_t` 宽度、数据模型、或哪些环境身份宏被定义的代码)没有办法问这次构建实际解析出
什么,所以它只能按一种假设编译,然后指望每一个消费者都跟它假设的一样。一个图里声明的
`[c-abi]` 与之不符的项目,会拿到按一种 `wchar_t` 宽度编译的目标文件,去链接按另一种宽度
写的头文件——**没有任何东西会去核对这件事**:store 里不记录它装的是按哪种环境编译的,所以
根本没有可以核对的对象,只有一次悄无声息的错误链接。这与上面 C++ 运行时那条 `requires`
检查已经接受下来的、同一种形状的已知限制相同,不是这一个 PR 新引入的——`[c-abi]` 继承它,
是因为它继承了同一个 store。

**要真正补上它需要什么:** 要么 (a) 改成两阶段安装——把安装钩子做的任何目标侧编译推迟到目标侧
解析完成之后,解析出环境后再重新调用一次钩子(或者一个更晚的第二个钩子)——这会改动整个
代码库目前当作固定不变的安装/解析顺序;要么 (b) 把 `c++-abi` 那条 `requires` 形状检查的
做法,推广到 `c-abi`/`c-environment`——包声明它的 store 产物是按哪种环境构建的,工具链解析
完之后核对,不符就拒绝——这个方案本节已经设计好,但这个 PR 里没有实现。在其中一个真正落地
之前,过渡期的纪律和 C++ 运行时那条现有要求一样:这类包的安装钩子不得把一种以上的环境变体
构建进同一个 store 目录。

### 标准库模块源

作为标准库的包陈述它的 `std` 模块源在何处,以及该源需要什么。

```toml
[build]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc", "-nostdinc++"]
```

这些键属于 `[build]`,因为模块源是该包的一个翻译单元:
它以该包的 include 目录与定义被编译。属于 `[build]` 同时使这些 flag 可条件化,
而一个在多种 C 库之上供给同一 C++ 运行时的包需要这一点。

```toml
[target.'cfg(c-abi = "musl")'.build]
std-module-flags = ["-D_GNU_SOURCE"]
```

声明 `std-module` 而没有相应的 `provides` 条目是错误:该包描述了一个它并不供给的库。

这三个键的 `[package]` 写法仍被接受,且不可条件化。

### 标准库自身的语言级别(mcpp 2026.9.15.2+)

一张模块图以同一个标准编译,即根包的标准([07 —— 工作空间](07-workspace.md) §4.2)。
供给 C++ 层的包(`hosted-standard-library` 或 `mcpp:c++-abi=<impl>`)是唯一的例外:
它陈述了 `[package] standard` 时,它的每个既不提供也不导入模块的 C++ 翻译单元都恰好以该级别编译,
与图的级别无关。

```toml
[package]
standard = "c++23"
provides = ["hosted-standard-library", "mcpp:c++-abi=libc++"]
```

标准库以自己的级别构建,并在其他任何级别被使用:上游以 C++23 编译 libc++,而 libc++ 22 的
源码在 C++20 下无法编译。这一例外是安全的,因为它覆盖的单元不读也不写 BMI;该包的模块单元,
包括 `std` 与 `std.compat` 模块,仍按图的级别编译,因此没有模块被分裂。该级别被追加到每个被覆盖
单元自己的 flag 中,因此同样到达编译命令、依赖扫描、`compile_commands.json` 与
`mcpp emit build-database`。不陈述 `standard` 的供给者按图的级别编译。

这一例外不推广到其他包。声明依赖于 `__cplusplus` 的头文件会使普通库的目标文件与其消费者的
目标文件在没有诊断的情况下不一致,而标准库正是其接口被设计为在另一级别下使用的那一类包。

### 对已解析目标侧的适配

供给某一层的包经常支持其下方层的多个实现。它查询已解析的目标侧,而非被告知。

```toml
[target.'cfg(c-abi = "musl")'.build]
include_dirs = ["config/musl"]

[target.'cfg(c-abi = "picolibc")'.build]
include_dirs = ["config/picolibc"]
```

若此处要求一次特性选择,将迫使工程重述目标三元组或其依赖图已经确立的事实,
并允许两处陈述互相矛盾。

**当某一层由依赖图供给时,编译器不再搜索该层的宿主位置。**
`mcpp.toolchain.hostflags` 读取每一层的来源并关掉对应的编译器隐式搜索:
图供给 `c-abi` 时去掉编译器自带的 C 库搜索路径(`-nostdlibinc`),
图供给 `c++-abi` 时去掉其 C++ 搜索路径(`-nostdinc++`)—— 两者各自独立判断,
不取决于另一层是否也来自图。在此之前,一个只因为宿主头文件恰好补上了某个缺口
才能编译的包,会在一台机器上构建成功、在另一台机器上以不同方式失败;现在确定的
结果只有两种 ——「在图里找到」与「没找到」,不再有「用了这台机器上恰好装着的
那份 SDK」。包应当通过下面的层谓词来适配,而不是依赖宿主机器恰好装了什么。

谓词的键就是五个层名,值就是本章开头那张表里的接口名 —— 与 `Target` 报告打印的是
同一批字符串。它们可以与三元组键在 `all`/`any`/`not` 下组合:

```toml
[target.'cfg(all(linux, c-abi = "musl"))'.build]
cxxflags = ["-D_GNU_SOURCE"]
```

**层名的是库,不是三元组的 env 段。** 二者在 `musl` 上重合,在 `gnu` 上分叉:
在 Linux 上该段请求的是 glibc,在 Windows 上它命名的是工具链的 MinGW 形态,
而后者的 C 运行时与 MSVC 形态链接的是同一个 UCRT。写法是 `c-abi = "glibc"`,
而非 `c-abi = "gnu"`;与答案相对的那个「请求」是 `env = "gnu"` —— 另一个问题
(`docs/specs/target-side.md` §3.4)。

**`env` 与 `c-abi` 不可互换。** `env` 是三元组**请求**的东西;`c-abi` 是图与载荷
**回答**的东西。依赖图里的 `openkal-musl` 会在 `x86_64-linux-gnu` 三元组下供给 musl,
而只有 `c-abi` 看得见这件事。

这些谓词仅在 `[build]` 段中可用。目标侧在依赖解析之后才被解析,
因此由它选择的依赖将构成环;`[target.'cfg(<层> = …)'.dependencies]` 会被报出并忽略,
而不是被静默丢弃。一个在不同 C 库下需要不同依赖的包,
按 C 库拆分,或依赖其并集并在 `[build]` 中选择源码。

mcpp 不认识的键 —— 打错的字,或来自更新版本 mcpp 的谓词 —— 会被报成一条 schema
警告,并且该段不生效。它过去是静默地求值为假,而那与「这一段本就不该匹配」读数完全
相同。

## 诊断

四种情形由引擎而非由编译器报出。

| 情形 | 报出内容 |
|---|---|
| 被要求的实现不是解析出的那个 | 同时指出二者,以及选择它的命令 |
| 两个包供给同一层 | 同时指出二者,以及各自进入图的路径 |
| 某一层无人供给 | 指出该层,以及应当依赖的能力 |
| 载荷的 C++ 运行时位于外来 C 库之上 | 同时指出二者,以及两条出路 |

一条来自编译器或链接器的、关于目标侧组合的消息,表明缺少一条诊断。
在任何命令行被发出之前,引擎已经知道该组合不成立。

## 兼容性

三条规定保全既有清单与既有构建。

能力名 `hosted-standard-library` 继续表示 C++ 层。
同时携带两种拼写的包是一个供给者,其中命名了接口的那一条是被报告的一条。

工具链族拼写 `openkal-llvm` 归一为 `llvm`。它命名同一份载荷,
并携带一条关于目标侧的事实,而上述模型从包的声明中解析该事实。

保留前缀内的未知名字,在根工程自己的清单中是错误,在依赖的清单中是警告。
前者是作者正看着的一处拼写错误;后者是一份对着更新引擎写成的清单,
拒绝它将意味着层名词表永远不能被一个已发布的包扩展。
清单中其它位置的未知键被忽略。

该规定只约束此后的引擎。一个包若声明某个层名,
其使用者仍须运行不早于该层名被引入的那个版本。

## `[target.*]` —— 平台条件依赖与 flag

用 `[target.<sel>]` 表把依赖与构建 flag 限定到某个平台。选择器 `<sel>` 有三种形式:

| 选择器 | 含义 | 示例 |
|---|---|---|
| **裸 OS 别名** | 单个 OS / 族 —— 简洁且常用的形式 | `[target.windows]`、`[target.unix]` |
| **`cfg(...)` 谓词** | 复合条件(arch / env / 组合子) | `[target.'cfg(all(linux, not(arch = "aarch64")))']` |
| **精确三元组** | 某个具体目标(同时承载 `toolchain` / `linkage` / `sysroot` / `runner` / `min_api_level`,见 [04 §2.7.3](04-mcpp-toml.md)) | `[target.x86_64-linux-musl]` |

一个选择器可以承载平台条件的**依赖**与**构建 flag**:

```toml
# 简洁的裸别名形式 —— 仅在 Windows 上拉取并链接 OpenBLAS。
[target.windows.dependencies.compat]
openblas = "0.3.33"
[target.windows.build]
ldflags = ["-Llib", "-llibopenblas"]

# cfg(...) 用于复合谓词(文法:all/any/not 作用于 os/arch/family/env,
# 以及裸别名 windows/unix/linux/macos)。
[target.'cfg(all(linux, not(arch = "aarch64")))'.build]
cxxflags = ["-march=x86-64-v2"]
```

`[target.windows]` 与 `[target.'cfg(windows)']` 完全等价 —— 裸别名
`windows` / `linux` / `macos` / `unix` 都不是合法的目标三元组,因此不存在歧义。
单个 OS/族用裸形式,arch/env 条件与组合子用 `cfg(...)`。

- **可用键**:`dependencies` / `dev-dependencies` / `build-dependencies` /
  `feature-deps.<feature>`(mcpp 2026.8.6.2+ —— 见 [30 —— build.mcpp](30-build-mcpp.md);feature 本身无条件注册,
  只有它的依赖集合受限定),以及带 `cflags` / `cxxflags` / `ldflags` / `sources`
  的 `build`(mcpp 0.0.95+ —— 条件源码 glob,例如把 `src/x86/**/*.asm` 收在
  `cfg(arch = "x86_64")` 之后;`!` 排除 glob 在此同样有效),再加 `flags` 与
  `include_dirs` / `include_dirs_after`(mcpp 0.0.102+),
  以及 `private_include_dirs` 与 `std-module-flags`(mcpp 2026.9.1.1+),
  还有带 `frameworks` / `libraries` / `link_library_dirs` 的 `runtime`
  (mcpp 2026.8.29.1+;`frameworks` 自 2026.9.12.3 起),以及带 `kind` 的
  `targets.<name>`(mcpp 2026.9.14.2+;见
  [`targets.<name> kind`](#targetsname-kind--一个库在某一行上的形态mcpp-20269142))。
- **mcpp 不读取的子表会被报出**(mcpp 2026.9.14.2+):拼错的
  `[target.<sel>.dependecies]` 是一条警告,列出 `[target.<sel>]` 表拥有的各个段,
  `--strict` 下成为错误。
- **条件依赖声明替换无条件声明**(mcpp 2026.9.14.2+)。在选择器命中的行上,
  `[target.<sel>.dependencies]` 中某个身份的声明就是该身份的声明,无条件声明在
  该行上不生效。某一行上以不同形态链接的依赖写两次,每次都带来源:

  ```toml
  [dependencies]
  huxerui.huxerui = { version = "0.3.0" }

  [target.'cfg(env = "android")'.dependencies]
  huxerui.huxerui = { version = "0.3.0", linkage = "shared" }
  ```

  同一规则适用于 `dev-dependencies`、`build-dependencies` 与
  `feature-deps.<feature>`;多个命中的段按清单顺序生效,最后一个为准。
  `mcpp why deps` 给出每条请求来自哪张表([09 —— 按场景的命令](09-commands-by-scenario.md))。
  只写选项、不写来源的表 `huxerui.huxerui = { linkage = "shared" }` 声明的是
  名为 `huxerui.huxerui.linkage` 的包:mcpp 报出这一行并给出补全来源后的声明,
  解析随即失败。2026.9.14.2 之前的引擎保留无条件声明。
- **`runtime` 是链接行中与方言无关的那一半。** `build.ldflags` 按 GNU 拼法书写,
  而原生 `cl.exe` 不接受 `-L`。这些键表达同一件事而不承诺拼法:mcpp 把
  `libraries` / `link_library_dirs` 渲染成 `-L<dir>` + `-l<name>` 或
  `/LIBPATH:<dir>` + `<name>.lib`,把 `frameworks` 在 Mach-O 各行渲染成
  `-framework <name>`,其余各行不产生任何标志。它们就是顶层 `[runtime]`
  (见 [04 —— mcpp.toml](04-mcpp-toml.md) §2.11)已有的同几个键,此处只是
  让它们按目标生效,并未引入新词汇。此处的一条追加在顶层列表**之后**,不是
  替换——manifest 借此把 `UIKit` 挡在 macOS 链接之外、把 `AppKit` 挡在 iOS
  链接之外,同时在顶层共享两者都要的那些 framework。`[runtime]` 的其余键
  在这里会被报出并忽略,因为它们不是按目标区分的。

  ```toml
  # 只在 Windows 上链接,并按实际编译器的拼法书写。
  [target.windows.runtime]
  libraries = ["user32", "gdi32"]
  ```

  一个谓词下只写这一张表时,它与其他情形一样生效。在 mcpp 2026.9.9.1 之前并非
  如此:除非同一谓词下还写了别的东西,该块会被解析后丢弃。
- **`build.ldflags` 中的相对搜索路径属于写下它的包。** `-L<dir>` 与
  `-Wl,-rpath,<dir>` —— 无论写在这里、写在顶层 `[build] ldflags`,还是来自
  `mcpp::link_flag` —— 以该包目录下的绝对路径进入链接,依赖的 flag 传给消费者时
  也是如此。以加载器展开的记号开头的项按原样传递:`$ORIGIN` 及其他任何 `$` 记号,
  以及 `@executable_path`、`@loader_path`、`@rpath`(mcpp 2026.9.14.2+)。
- **`build` 接受的恰好是*可叠加的构建输入*集合** —— 那些以追加方式合并、
  并在谓词求值之后被消费的东西,也就是 `BuildInputs` 的成员表。`linkage`、`target`
  与档案开关刻意不在其中:它们是**目标选择的输入**(用一个针对 `target` 求值的谓词
  去条件化 `target` 是循环的),或者需要覆盖而非追加的语义。
  集合之外的键会被报出并忽略;消息里列出的正是它比对用的那份集合,因此不会与检查漂移。
- **按解析后的目标求值** —— 交叉构建取 `--target` 三元组,否则取宿主。因此原生
  Linux 构建**根本不会下载** `[target.windows]` 依赖。
- **谓词的键**:`os`、`arch`、`family`、`env` —— 三元组的坐标 —— 以及自 mcpp
  2026.9.1.1 起的五个目标侧层名 `compiler`、`compiler-runtime`、`kernel-abi`、
  `c-abi`、`c++-abi`(见[22 —— 目标侧](22-target-side.md))。`accelerator` 同样
  是这里的键,由本次构建自己的 `accel`(`--accel` 或 `[build] accel` 里的后端名)
  回答,因此它是对一个集合的成员判定;`accelerator = "none"` 则是一段用来说
  「本次构建没有命名任何后端」的写法,而不必枚举它不是的那些后端。裸词
  `linux` / `macos` / `windows` / `unix` 是对应 `os` / `family` 判定的糖。
  集合之外的键会被报成一条 schema 警告,且该段不生效 —— 它过去静默地求值为假,
  而那与「这一段本就不该匹配」读数完全相同。
- **被解析的层的谓词不能选择依赖。** 层是**从**依赖图解析出来的,因此由它选出的
  依赖会决定它正在询问的那个答案。`[target.'cfg(c-abi = "musl")'.dependencies]`
  会被报出并忽略;同一谓词下的 `build` 输入照常生效。`accelerator` 不在此列
  (mcpp 2026.9.6.5):它是构建的输入而不是图给出的答案,所以
  `[target.'cfg(accelerator = "cuda")'.dependencies]` 生效。
- **优先级**:精确三元组表胜过 `cfg`/别名表;多个命中的谓词表,其 flag 按序拼接,
  其依赖声明按清单顺序生效。
  条件项追加在无条件 `[build]` 项**之后**,因此在 GNU「最后一个 flag 生效」的
  规则下,条件规则会覆盖更宽的无条件规则。这正是让按 OS **移除**成为可表达的原因:

  ```toml
  [build]
  flags = [{ glob = "third_party/zlib/**", defines = ["HAVE_UNISTD_H=1"] }]

  # clang-MSVC 没有 <unistd.h>:撤销基础 define,加上 windows 的那个。
  [target.'cfg(windows)'.build]
  flags = [{ glob = "third_party/zlib/**",
             defines = ["NO_FSEEKO"], cflags = ["-UHAVE_UNISTD_H"] }]
  ```

- **未命中当前目标的条件 `flags` 条目根本不存在**,因此它不会产生
  「glob 未匹配到任何源文件」的警告。于是一份 manifest 可以同时携带三个 OS 的
  flag 表,而不会在另外两个上制造噪声 —— 与未启用 feature 的条目根本不存在是
  同一个道理。**无条件**表里的零命中 glob 仍然告警,因为那里它是真实缺陷。
- **`toolchain` / `linkage` / `sysroot` 仅限精确三元组** —— 它们描述某一个具体的交叉目标,
  因此写在 `[target.<triple>]` 下(见上),而不是裸别名或 `cfg(...)` 下。

### `sysroot` —— 目标的 C 库

`sysroot`(mcpp 2026.8.20.2+)覆盖目标表为某个三元组绑定的 C 库,与 `toolchain`
覆盖编译器 pin 同轴:一个指名目标所解析的编译器,另一个指名它的 C 库,两者在工程
有理由与之分歧之前都只由引擎决定。

```toml
[target.riscv64-none-elf]
sysroot = "xim:newlib-riscv@4.4"     # a different C library
```

```toml
[target.riscv64-none-elf]
sysroot = ""                          # no C library at all
```

**键缺席与键为空是两个不同的答案。** 缺席继承目标表的 C 库。存在且为空是
**零 libc 档**:不解析任何 C 库,不加入头文件与库目录,链接行上只有工程与其依赖
提供的内容,`#include <stdio.h>` 不再解析。内核与 bootloader 要的正是这一档,而把
两种情形合并会让这类工程静默地把目标的 C 库拿回去。

取值是 xpkg 引用或空字符串;裸名在解析清单时即被拒绝,因为接受它会导致什么都不安装,
然后在很晚的时候以「缺少 libc」失败。

构建程序可以询问供给 sysroot 的是哪个 C 库**载荷**:`mcpp::target_libc()` 返回该包的
名字,`mcpp::target_libc_profile()` 返回目标 ISA 档位对应的子目录。零 libc 档上两者均
为空。

**这与「目标侧解析出的 C 库是哪一个」不是同一个问题。** `target_libc()` 命名的是
mcpp 装上的那个载荷,而这个值是目标侧解析的一项**输入** —— 依赖图里的包可以改为供给
C 库,那时解析出的 `c-abi` 就不是这里返回的东西。要按已解析的层分支,请用层谓词:
`[target.'cfg(c-abi = "musl")'.build]`(见[22 —— 目标侧](22-target-side.md))。
这一段在 2026.9.1.1 之前写的是「解析到的是哪份 C 库」,那是两者里错的那一个。
参见[40 —— 裸机与 freestanding 目标](40-baremetal.md)。

### `abi` —— 整个产物共享的开关(mcpp 2026.9.12.2+)

```toml
[target.'cfg(os = "emscripten")'.abi]
threads    = true
exceptions = true
```

目标的某些性质不是单个翻译单元可以自行选择的 flag。线程支持即是一例:在 WebAssembly 上,每个目标文件、
预编译的标准库模块与链接必须在共享内存与原子操作上保持一致,只要有一个翻译单元未启用它们,链接就会失败,
或模块拒绝加载。异常是同一种形状:clang 把异常模型记进 BMI,并拒绝一个与之不一致的导入者。这类性质
写作 `[target.<selector>.abi]` 的有类型成员,而不是 `cxxflags` 中的 flag,引擎因此能把它施加到每个
必须一致的单元上,并把它与包的需求相比较。

| 成员 | 类型 | 渲染为 | 到达 |
|---|---|---|---|
| `threads` | 布尔 | 在既非 PE 也非 freestanding 的目标上为 `-pthread`;在 PE 与 freestanding 目标上不产生任何 flag | 标准库模块的预构建、依赖扫描、所有包的每个 C 与 C++ 翻译单元,以及链接 |
| `exceptions` *(mcpp 2026.9.12.3+)* | 布尔 | `-fexceptions`,只在 `os = "emscripten"` 上,经方言 flag 进入编译行,也进入链接行;在其余每个目标上什么都不产生,因为那里异常本来就是默认开启的 | 与 `threads` 相同的那一套 |

两个成员都经由方言 flag 进入依赖缓存键,因此未启用某个成员时构建的依赖不会被启用它的构建复用。未知
成员,以及不是布尔值的成员,都会被拒绝,并同时列出 `threads` 与 `exceptions`。

**没有 `exceptions`,观察到的失败在运行时,不在链接时。** 一个跨 `import std` 边界抛出异常的 Web
程序能正常编译并链接——Emscripten 的编译期异常支持不依赖这个 flag——只在 `throw` 真正执行时中止:

```
Aborted(Assertion failed: Exception thrown, but exception catching is not
enabled. Compile with -sNO_DISABLE_EXCEPTION_CATCHING or
-sEXCEPTION_CATCHING_ALLOWED=[..] to catch.)
```

**只有根 manifest 做决定。** 这个开关属于产物,而根包是唯一构建产物的包。依赖写下的
`[target.<selector>.abi]` 会被报告(`abi/dependency-table`),且不改变任何东西。依赖改为声明自己的需求:

```toml
[package]
requires_abi = { threads = true }          # 整个包需要线程

[features]
mt = { requires_abi = { threads = true } } # 只有这个 feature 需要线程
```

根包未满足的需求在任何编译开始之前被拒绝,拒绝信息指出包名以及提出需求的对象:

```
error: `wasmrt` requires the artefact's ABI to have threads (feature `mt`), and this build does not state it.
       Add to the root manifest, for the targets that need it:

           [target.'cfg(os = "<os>")'.abi]
           threads = true
```

若没有这项拒绝,不匹配会表现为预编译模块的配置错误,而该错误既不指出包,也不指出开关。

### `requires_abi` 在 target 轴上(mcpp 2026.9.12.3+)

`[package] requires_abi` 与 `[features.<f>] requires_abi` 是无条件的:它们要求
本包构建的每一个目标都打开某个开关。一个需求局限于某个平台的依赖——比如每个
hosted 行都要线程、Web 上一个都不要——直接写在已经承载它自己 `sources` 的
选择器上:

```toml
[target.'cfg(linux)']
requires_abi = { threads = true }

# 按 feature 的形式,命名沿用 feature-deps 与 feature-xlings
[target.'cfg(linux)'.feature-requires-abi]
mt = { threads = true }
```

需求集合是 `[package] requires_abi`、活跃 feature 各自的表、以及每一个命中的
选择器各自表的并集——不止一个选择器可以要求同一个成员,而它们各自都是真话。
它只对**命中已解析目标**的选择器生效:`[target.'cfg(windows)']` 下的需求对
一次 Linux 构建不施加任何东西,双向皆然——有它、没它,那次构建都不受影响。
根包未满足的需求在任何编译开始之前被拒绝,拒绝信息按原样点名那个选择器:

```
error: `wasmrt` requires the artefact's ABI to have threads ([target.'cfg(linux)']), and this build does not state it.
       Add to the root manifest, for the targets that need it:

           [target.'cfg(os = "<os>")'.abi]
           threads = true
```

**早于 2026.9.12.3 的引擎静默读过这个键——既不警告,也不报错。**
`[target.<sel>] requires_abi` 是目标选择器下一个取值为表的键,而旧引擎的
schema 清扫会跳过每一个取值为表的键,理由是它假定表就是条件通道;
`requires_abi` 在这里恰好是一个内联表,与那个假定同一种 TOML 形状,于是
不受任何报告地漏过同一次清扫。一个依赖这份拒绝来保护一次无条件线程构建的
包,因此要自己声明引擎下限
(`[build-dependencies.mcpp] version = ">= 2026.9.12.3"`),而不能指望旧客户端
自己发现这个缺口。

`--no-entry`(Emscripten 里没有 `main` 的模块用的 flag)不是 mcpp 解释的开关;
它是一条普通的 `[target.'cfg(os = "emscripten")'.build] ldflags` 条目,`main`
照样只是指出一个翻译单元——见[21 —— 目标三元组](21-the-target-triple.md#wasm-产物契约)。

### `targets.<name> kind` —— 一个库在某一行上的形态(mcpp 2026.9.14.2+)

```toml
[targets.huxerui]
kind = "lib"

[target.'cfg(env = "android")'.targets.huxerui]
kind = "shared"
```

`[targets.<name>] kind` 的按行形式([04 —— mcpp.toml](04-mcpp-toml.md) §2.2)。
在桌面各行上链接进应用、在 Android 上必须是唯一一份共享库的框架,在它自己的清单里
声明一次;每个消费者都只保留一行无条件依赖。

- 一行陈述 `kind`,在两种库形态 `lib` 与 `shared` 之间选择;或者 *(2026.9.15.2+)*
  陈述 `linkage`,给出该库在这些行上的默认形态而不约束它。同一行同时写两者会被拒绝,
  后命中的陈述替换先前的陈述,包括无条件表中的陈述。不是该包库目标(声明的或推断的)
  的名字、程序目标以及其他形态都被拒绝。
- 在命中的行上,该包被约束为共享形态,与 `[targets.<name>] kind = "shared"`
  的约束完全相同([04 —— mcpp.toml](04-mcpp-toml.md) 中的 `dependency_linkage`):
  不写 `linkage` 的消费者得到共享库,写 `linkage = "static"` 的消费者得到一条点名
  这一行的警告,`--strict` 下成为错误。`mcpp why deps` 以原因 `row-kind` 报告该形态。
  某行的 `linkage = "shared"` 让不写 `linkage` 的消费者得到共享库,原因为
  `package-default`;对消费者的 `linkage = "static"` 予以遵从,给出信息行而不是警告。
- 点名目标侧层的选择器不能承载这张表;该表被报出并忽略,因为库的形态是在解析
  回答该层的那张图时决定的。
- 2026.9.14.2 之前的引擎不读取这张表,也不报告。依赖它的包要写明这一引擎下限。

## 当前边界

- **依赖不能以加速器为条件。** 加速器这一层是从依赖图解析出来的,因此由它选择的依赖
  会决定它自己在问的那个答案。mcpp 会报告该谓词并忽略它;包要么无条件、要么以平台为
  条件,而由加速器选择的是 `[build] sources`。
- 一般而言,层也不能选择依赖,理由相同:任何早于图的推导,都是在推断一个尚不存在的
  事实。
