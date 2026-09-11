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
在依赖图安装之后进行,因此安装钩子还会通过 `MCPP_CXX_STDLIB` 与 `MCPP_COMPILER` 收到解析结果
([32 —— 编写载荷](32-authoring-a-payload.md)),可以在编译任何东西之前拒绝。钩子不得把另一种变体构建进
同一个存储目录,否则第一个消费者就会替之后所有消费者决定变体。

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
| **精确三元组** | 某个具体目标(同时承载 `toolchain` / `linkage` / `sysroot` / `runner`,见 [04 §2.7.3](04-mcpp-toml.md)) | `[target.x86_64-linux-musl]` |

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
  还有带 `libraries` / `link_library_dirs` 的 `runtime`(mcpp 2026.8.29.1+)。
- **`runtime` 是链接行中与方言无关的那一半。** `build.ldflags` 按 GNU 拼法书写,
  而原生 `cl.exe` 不接受 `-L`。这两个键表达同一件事而不承诺拼法:mcpp 按目标
  渲染成 `-L<dir>` + `-l<name>` 或 `/LIBPATH:<dir>` + `<name>.lib`。它们就是
  顶层 `[runtime]`(见 [04 —— mcpp.toml](04-mcpp-toml.md) §2.11)已有的同两个键,
  此处只是让它们按目标生效,并未引入新词汇。`[runtime]` 的其余键在这里会被报出
  并忽略,因为它们不是按目标区分的。

  ```toml
  # 只在 Windows 上链接,并按实际编译器的拼法书写。
  [target.windows.runtime]
  libraries = ["user32", "gdi32"]
  ```

  一个谓词下只写这一张表时,它与其他情形一样生效。在 mcpp 2026.9.9.1 之前并非
  如此:除非同一谓词下还写了别的东西,该块会被解析后丢弃。
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
- **优先级**:精确三元组表胜过 `cfg`/别名表;多个命中的谓词表,其 flag 按序拼接。
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
threads = true
```

目标的某些性质不是单个翻译单元可以自行选择的 flag。线程支持即是一例:在 WebAssembly 上,每个目标文件、
预编译的标准库模块与链接必须在共享内存与原子操作上保持一致,只要有一个翻译单元未启用它们,链接就会失败,
或模块拒绝加载。这类性质写作 `[target.<selector>.abi]` 的有类型成员,而不是 `cxxflags` 中的 flag,
引擎因此能把它施加到每个必须一致的单元上,并把它与包的需求相比较。

| 成员 | 类型 | 渲染为 | 到达 |
|---|---|---|---|
| `threads` | 布尔 | 在既非 PE 也非 freestanding 的目标上为 `-pthread`;在 PE 与 freestanding 目标上不产生任何 flag | 标准库模块的预构建、依赖扫描、所有包的每个 C 与 C++ 翻译单元,以及链接 |

该成员经由方言 flag 进入依赖缓存键,因此未启用线程时构建的依赖不会被启用线程的构建复用。未知成员,以及
不是布尔值的 `threads`,都会被拒绝。

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

## 当前边界

- **依赖不能以加速器为条件。** 加速器这一层是从依赖图解析出来的,因此由它选择的依赖
  会决定它自己在问的那个答案。mcpp 会报告该谓词并忽略它;包要么无条件、要么以平台为
  条件,而由加速器选择的是 `[build] sources`。
- 一般而言,层也不能选择依赖,理由相同:任何早于图的推导,都是在推断一个尚不存在的
  事实。
