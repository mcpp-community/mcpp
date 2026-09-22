# 23 —— 项目环境

**读者：** 构建需要编译器之外工具的作者。

**本章回答的那一个问题：** 工程如何声明其构建运行的环境，以及该声明决定什么。

**不在这里：** 构建程序如何使用这些工具，见
[30 —— 构建程序：`build.mcpp`](30-build-mcpp.md)；选择编译器，见
[20 —— 工具链管理](20-toolchains.md)。

工程可以声明自己构建所在的环境。这一条声明决定工程链接哪一份 C 库、
构建程序能找到哪些工具——因此同一份 `mcpp.toml` 在开发者的笔记本上和在 CI 上
是同一次构建，无论这两台机器上还装了别的什么。

```toml
[xlings]
subos = "tools"

[xlings.workspace]
"xim:qemu-riscv" = "9.2.4-1"
```

可运行的工程：`examples/07-project-subos/`。

## 1. SubOS 的定义

SubOS 是一个承载用户空间的目录：它有自己的 `bin`、自己的库视图、自己安装的
包版本，以及一个描述自身的 `subos_info` 块。mcpp 把它当作「本工程针对什么构建」
这一问题的答案，而且它是回答这一问题的唯一机制——不是编译器的路径，
不是 `XLINGS_ACTIVE_SUBOS`，也不是 shell。

存在两种形态，差别在于目录所在的位置：

| 声明 | 目录 | 共享范围 |
|---|---|---|
| 不声明 | mcpp 初始化的 `subos/default` | 这台机器上的每个工程 |
| `subos = "default"` | 同一目录，显式点名 | 这台机器上的每个工程 |
| `subos = "<name>"` | `<project>/.mcpp/.xlings/subos/<name>/` | 不共享 |

第三行是隔离的那一种。它属于工程本身，与 manifest 放在一起，
删除工程也就删除了它。

## 2. 这条声明的决定范围

**C 库。** 一次 payload-first 构建链接的是某一份具体的 glibc，
是哪一份是关于工程的事实，而不是关于机器的事实。第 8 章讲述这项绑定、
它的降级规则，以及一个不描述自身的 SubOS 对它们的影响。

**构建程序能看到的工具**（mcpp 2026.8.25.1+）。声明的环境的 `bin` 被放到
`build.mcpp` 运行时所用 `PATH` 的最前面：

```
PATH=<the declared environment's bin>:<the PATH mcpp itself was started with>
```

因此，构建程序写下裸名 `qemu-system-riscv64` 时，拿到的是声明环境内的那一份。
第 7 章讲述这一点所依赖的契约。

**只对声明了环境的工程生效。** 没有 `[xlings].subos` 的工程拿到的 `PATH`
与 mcpp 被启动时的 `PATH` 逐字节相同。把一个共享目录放在每个工程前面，
会让一次构建看到的内容取决于这台机器上还装了别的什么——同一机器上的两个工程
会彼此一致，而同一个工程在两台机器上却不会一致。声明它，才会把它放到那个位置。

**是前置，不是替换。** 构建程序合理地会调用 `git`、`python3` 或某个 shell，
它们都不住在 SubOS 里。放在最前面只是让声明的环境成为默认答案；
其余一切仍然可以在它后面被找到。

### 2.1 生效的版本钉（2026.9.3+）

点名一个环境同时改变了工具版本的来源。工程自己 `[xlings.workspace]` 中的
条目永远胜出——既胜过这里的环境，也按第 3 节的规则胜过依赖的声明——
不同的是它们各自叠加在什么之上：

| 工程的声明 | 它未点名的工具，其版本来自 |
|---|---|
| `[xlings.workspace]`，不带 `subos` | 机器自身的环境 |
| `[xlings.workspace]` 且 `subos = "<name>"` | 该环境自己的 workspace；机器的不生效 |

第二行就是「隔离」的含义。一个具名环境有它自己已安装的集合，
把机器的版本带进去只会点名一些根本不在那里的版本——因此工程一旦点名一个环境，
就必须重新声明它原本依赖机器所提供的工具。

在工程内部执行的一次 `xlings use` 优先于以上两者，直到 mcpp 重写该环境为止：
它是最后被合并的一层，而一个人主动做出的动作理应胜过一份文件。

## 3. 这条声明不决定的部分

`[xlings.workspace]` 点名环境中应当存在的包，而每个包的 payload 目录会
另行以 `MCPP_XPKG_<NAME>_DIR` 的形式交付。这是与 `PATH` 不同的问题，
答案也不同：需要一个包的数据文件（比如 protoc 内置的那些 `.proto` 文件）的
构建程序去问目录，需要*运行*一个程序的构建程序去问 `PATH`。

**workspace 成员的声明不是 workspace 的声明。** 在一次 workspace 构建中，
workspace 根拥有这项选择；一个成员自己的 `[xlings]` 只在该成员作为独立的根
被构建时才生效。

**依赖的声明是另一回事，它会被采纳**（`[xlings] deps` 自 2026.9.5.4+，
下述版本规则自 2026.9.6.6）。一个板级支持包知道哪个模拟器能到达它的机器，
一个规则包知道自己的规则驱动哪套工具集；消费方若被迫重复其中任何一项，
正是这类包存在的意义所要去除的重复。依赖所声明的内容会被安装，
`MCPP_XPKG_<NAME>_DIR` 在该依赖自己的构建程序里为它给出答案。

当工程与依赖点名**同一个包**，安装的是它的一个版本：身份是
`(namespace, name)`，版本是施加于其上的约束。离产物更近的声明胜出，
且覆盖会被报告；不满足对侧所陈述要求的一个 pin 会被拒绝，并同时指出两侧。
参见本章「一个包一个版本」一节。

## 4. 只读取环境，从不创建环境

mcpp 解析一个声明的名字，并读取它找到的内容。一个解析不出的名字是硬错误：

```
error: selected SubOS 'tools' does not exist at …/.mcpp/.xlings/subos/tools;
create/bootstrap that environment instead of falling back to active/default
```

回落到默认环境或当前激活的环境，等于用另一个环境替换 manifest 点名的那一个，
而这恰恰会让同一份 `mcpp.toml` 意味着两次不同的构建。创建并填充一个 SubOS
是 xlings 那一层的事——`xlings subos new`——由 mcpp 去管理 SubOS 的状态
会颠倒这层分工。

一个存在、却不带 `subos_info` 块的环境**降级而非失败**：运行时绑定报告
`inconclusive`，没有 payload-first 绑定可用，打印一条提示，构建继续进行。
第 8 章给出完整规则。

## 5. 采用私有环境的条件

- **一个版本会改变其产出的生成器。** `protoc`、`flatc`、某个着色器编译器：
  它的输出是下游一切的输入，因此工程钉住这个产生者的版本，而不是寄望机器上
  恰好装着一份兼容的。
- **构建程序运行的模拟器。** 若干裸机包把产物启动在 QEMU 之下，
  以此作为证明产物可用的一部分；用的是哪个 QEMU，也是被证明的内容之一。
- **CI 与开发者机器不同的工程**，二者都没有错，而构建不应察觉这种不同。
- **同一台机器上两个工程需要同一工具的不同版本。** 共享一个目录意味着
  其中一个工程要吃亏；私有环境让这个问题根本不出现。

反过来看：一个隔离的环境是一个必须被创建并填充的目录，代价由第一次构建承担。
自 2026.8.29 起，mcpp 负责这项工作——一个声明的 `[xlings.workspace]` 条目
在第一次用到时被置备，一个尚不存在的具名 `[xlings] subos` 被创建而不是被拒绝——
但代价是真实的：干净机器上的第一次构建，会在编译任何东西之前先下载并安装。
一个工具寻常、版本无关紧要的工程，更适合什么都不声明，直接继承机器的环境。

在 `--offline` / `MCPP_OFFLINE` 或 `MCPP_NO_AUTO_INSTALL` 之下，mcpp 拒绝而非
安装，并点名这些包以便离线置备——与 `[toolchain]` 所遵守的同两个开关，
理由也相同：一次未被要求的下载，不是构建可以替工程做主的事。

这条声明会在构建工程的每一台宿主上被置备，一个宿主装不上的包是错误，
而不是被跳过的条目。因此只为一个宿主平台存在的工具要按平台声明
（2026.9.2.1）：`deps = [{ linux = "qemu-user-aarch64" }]` 只在 Linux 上声明
这个模拟器，在别处什么都不声明。相关的键与解析规则都在本章。

**哪些命令会安装它。** 一个条目可以点名一个层级——
`{ version = "0.24.0", when = "run" }`——`[feature-xlings.<feature>]` 表
则把一个条目挂在某个 feature 上。工程用不到的工具就不会被下载：见本章。
省略层级即为历史行为。

**runner。** `[xlings.workspace]` 下的一个程序，也是
`[target.<triple>].runner` 为它的第一个元素最先查找的地方，先于 `PATH`
（[04 §2.7.3](04-mcpp-toml.md)）。这两个键合在一起，在 CI 宿主上置备一个
用户态模拟器，并通过它执行一个交叉构建出的产物，而 manifest 不必点名该
payload 的路径。

## 6. 应当写在别处的声明

| 需求 | 声明在 |
|---|---|
| 程序链接的一个库 | `[dependencies]` |
| 编译器 | `[toolchain]`，第 3 章 |
| 依赖产出的一个宿主工具 | `tools = [...]`，第 7 章 |
| 环境中存在的一个工具 | `[xlings.workspace]` |
| 只有某一个命令或某一个 feature 需要的工具 | `when = "run"`、`[feature-xlings.<f>]` |
| 使用哪一个环境 | `[xlings] subos` |

## 7. `[xlings]` —— manifest 键

```toml
[xlings.workspace]                 # what this project's environment contains
cmake                    = "3.28"
"xim:picolibc-riscv"     = "1.8.12"        # a namespaced package - quotes required
code                     = ""              # present; version unconstrained
llvm                     = { macosx = "20", default = "22" }
```

```toml
[xlings]
subos = "dev"                      # a named, isolated environment
```

`[xlings]` 是 mcpp 对**xlings 本地工程机制**的表面：即给一个目录赋予自己环境的
那份工程 `.xlings.json`。子表的名字及其含义都来自那份文件，mcpp 不加任何转译层，
把它们原样物化进 `<project>/.mcpp/.xlings.json`。

**`[xlings.workspace]` 是唯一的一张表。** 一个条目点名一个包，以及本工程使用它的
版本。mcpp 置备它——机器上没有就安装，有就映射——并把它物化为一个解析 pin，
使工程点名的版本正是其工具最终解析到的版本。

### 条目的形式

| 写法 | 含义 |
|---|---|
| `cmake = "3.28"` | 该版本 |
| `llvm = "22"` | 已安装的最高 `22.*`；版本前缀可以解析 |
| `code = ""` | 存在，版本不受约束 |
| `"xim:picolibc-riscv" = "1.8.12"` | `xim` 索引中的一个包 |
| `llvm = { macosx = "20", default = "22" }` | 按宿主平台区分 |

**带命名空间的包写作 `"<namespace>:<name>" = "<version>"`，引号是必须的**——
TOML 裸键不能包含冒号。这是推荐写法，也是每个官方包采用的写法：
一个条目点名一个包，然后说明它的哪个版本，因此命名空间归属于名字。

命名空间也可以写在版本那一侧（`picolibc-riscv = "xim:1.8.12"`），因为
物化后的 `.xlings.json` 正是这样携带它的——那里的一个键是一个 xvm target，
而 scope 限定了版本。两套词汇，一个条目。在两侧各写一次且值不同是错误，
在两种拼法下重复点名同一个包也是错误。

平台键是 xlings 自己的一套——`linux`、`macosx`、`windows`——加上 `default`。
`macos` 与 `macosx` 是同一个平台的两种写法（mcpp 的三元组用其中一种，
描述符与 xlings 的工程文件用另一种），两种写法在任何点名平台的地方都被接受。
一张表若既没有对应这台宿主的键，也没有 `default`，就是在这里什么都不声明。

### 两条解析轴 —— 宿主与目标（mcpp 2026.9.6.4+）

一个工具条目回答两个不同问题中的一个，由写它的那张表决定回答哪一个：

| 写法 | 轴 | 解析对象 |
|---|---|---|
| `[xlings.workspace]`，取值中的平台键 | 宿主 | 运行构建的机器 |
| `[target.<selector>.xlings.workspace]` | 目标 | 已解析的目标（`--target`，否则是宿主） |

两种写法都正确，谁也不取代谁。在构建机器上执行的工具属于宿主轴；
被产出代码编译或链接所依赖的 payload 属于目标轴。

```toml
[xlings.workspace]
"xim:dpcpp" = "7.1.0"              # a compiler, and it runs here

[target.'cfg(os = "linux")'.xlings.workspace]
"xim:glibc"         = ""           # what the device units are compiled against
"xim:linux-headers" = ""
```

在一次原生构建中，两条轴点名的是同一个平台，因此在宿主轴上陈述目标事实的工程
碰巧是对的，并且能照常工作。这个工程第一次被交叉编译时，它就不再是对的了。
**对被产出代码所编译或链接依赖的任何东西，目标轴是推荐写法。**

`[target.<selector>.feature-xlings.<feature>]` 把条件与开关组合起来，
写法与 `[target.<selector>.feature-deps.<feature>]` 完全一致：
选择器说明适用于哪些目标，feature 说明是否启用。

```toml
[target.'cfg(os = "linux")'.feature-xlings.backend-vulkan]
"xim:shaderc" = "2026.3"
```

**这里的选择器不能点名一个已解析的层。** `c-abi`、`c++-abi`、`compiler`、
`compiler-runtime` 与 `kernel-abi` 都由依赖解析回答，而依赖解析发生在工具
安装完成、构建程序运行完成之后。以某一层为条件的工具会被声明，却永远不会被
安装——一次成功的构建里那个工具干脆缺席——因此这样的 manifest 会被拒绝，
并同时指出工具和该谓词。请改为以目标为条件，或以 feature 为开关：
`[feature-xlings.<feature>]` 在任何东西被置备之前就已经确定。

**`accelerator` 是例外，它被允许使用**（mcpp 2026.9.6.5）。它不是从任何东西
解析出来的：它就是 `--accel`，或者 `[build] accel`，在第一个包被查找之前
就已经读出。以它为条件的 payload，与三元组谓词在同一趟合并中被合并，
并像其他任何条目一样被安装。

```toml
[target.'cfg(accelerator = "cuda")'.xlings.workspace]
"xim:cuda-nvcc"   = "12.9.86"
"xim:cuda-cudart" = "12.9.79"
```

带设备孤岛的工程应当使用这种写法。没有它，厂商工具集就只能被无条件声明，
或者干脆不声明，于是不带加速器的 `mcpp build`——最廉价的一种构建，
也是 CI 通常跑的那一种——会为一个它根本没有在为之编译的设备下载数 GB 内容。

同一条规则也适用于依赖：`[target.'cfg(accelerator = "cuda")'.dependencies]`
会被采纳，而以一个已解析的层为条件的依赖不会，因为后者会替自己正在问的那个
问题决定答案。加速器身上没有任何循环。

条件只能写在选择器上。一个同时携带平台键的选择器下的取值，
是把同一件事说了两遍，会被拒绝，并同时指出两侧：

```
[target.cfg(os = "linux").xlings.workspace] xim:tool: the value carries platform
keys (linux, macosx), but [target.cfg(os = "linux")] already says which targets
this applies to.
```

`subos` 不能以目标为条件：一个工程只有一个环境，因此
`[target.<selector>.xlings]` 直接拒绝这个键，而不是丢弃它。

**一份已发布的描述符，通常不为目标轴的条目携带任何边**，`mcpp publish`
会说明这一点。描述符按平台各有一个块，而大多数选择器都不是一个平台——
`cfg(target_arch = "aarch64")` 点不到该文件里的任何一个块。**但只点名一个
操作系统的选择器就是一个平台**:`cfg(linux)`、`cfg(os = "linux")`
以及 windows/macos/unix 的等价写法，会折叠进对应的
`xpm.<platform>.deps` 块，而不只是引发那条提示。这个包的**消费方**
另外会安装什么，来自顶层的 `[xlings.workspace]`；目标轴对这个包自己的
构建所编译依赖的内容，仍然是准确的。

这两条轴所属的一般规则，见 [SPEC-004](../specs/manifest-semantics.md)。

### `when` —— 需要这个工具的命令（mcpp 2026.9.4.2+）

```toml
[xlings.workspace]
"xim:qemu-arm"  = "9.2.4-1"                             # every build, as before
"xim:codegen"   = { version = "1.0",    when = "build" }
"xim:probe-rs"  = { version = "0.24.0", when = "run"   }
"xim:clang-tidy"= { version = "20",     when = "dev"   }
```

包依赖从一开始就有这条轴——`[dependencies]`、`[build-dependencies]`、
`[dev-dependencies]`。工具却只有一张表，于是一个同时点名了一个模拟器和一个
调试探针的板级支持包，会把两者都装给每一个消费方，包括那些只想编译这个库的。

| `when` | 安装方 | 是否到达消费方 |
|---|---|---|
| *（省略）* | 每一个执行构建的命令 | 是 |
| `build` | 每一个执行构建的命令 | 是 |
| `run` | `mcpp run`、`mcpp test` | 是 |
| `dev` | 只有声明它的那个包，当它作为根时 | **否** |

**省略 `when` 就是 2026.9.4.2 之前的行为，完全一致**，因此没有任何 manifest
需要为此改动。收窄是可选的，不是作者必须回答的问题。

`dev` 是唯一不传播的层级。它的含义是「*仅当声明它的这个包本身正在被开发时*」，
因此一个依赖的 `dev` 条目永远不会为消费方安装。其余每个层级都会到达消费方，
这正是板级支持包了解自己机器这件事的意义所在：它只声明一次模拟器，
每个消费方都能拿到。

层级写在条目本身而不是写成第二张表，理由与 `[dependencies]` 同时接受
`dep = "1.0"` 和 `dep = { version = "1.0", features = [...] }` 相同。
一个带作用域的条目必须点名 `version`，哪怕留空（`version = ""` 意为
「存在，版本不限」)，因为 `{ when = "run" }` 与一个拼错的 `version` 键
否则将无法区分。

### `[feature-xlings.<feature>]` —— 某个 feature 才需要的工具

```toml
[features]
default  = ["emulator"]
emulator = {}
hardware = {}

[feature-xlings.hardware]
"xim:probe-rs" = "0.24.0"
```

同一张表，以 feature 为开关，写法与 `[feature-deps.<feature>]` 一致。
一个从不要求 `hardware` 的消费方，永远不会下载探针驱动。这里的条目
接受 `when`，与无条件条目完全一样。

一个没有任何 `[features]` 表声明的 feature 名会被报成一条 schema 警告：
它不为任何人激活，也不安装任何东西，而一个工具的缺席若只表现为
「设备永远不可达」，那是最难诊断的一类问题。

### 规则包自带它的环境（2026.9.6.6+）

上面这张表，是工程有主张时要写的内容。大多数工程没有主张，
它们要写的就是什么都不写：

```toml
[build-dependencies.mcpp]
plugins = { version = "0.3.0", features = ["rules-cuda"], host-module = true }
```

这一条边就是全部的声明。规则包点名自己的规则需要哪些包、需要它们的哪个版本，
挂在选中它的那个 feature 和它所针对的加速器之下：

```toml
# in the rule package, not in your project
[target.'cfg(accelerator = "cuda")'.feature-xlings.rules-cuda]
"xim:cuda-nvcc"   = ">=12.9.86"
"xim:cuda-cudart" = ">=12.9.79"
```

两道闸，都必须打开。feature 回答「是否需要这条规则」；选择器回答
「究竟哪些构建真的会下载它」。同一工程的纯 CPU 构建两道闸都不打开，
什么都不安装。

是哪个包、它可以有多旧，这是规则作者的知识。让每个使用这条规则的工程都
重复一遍，得到的是一份会静默过期的拷贝——规则挪动了，而工程没有跟着动。

### 一个包一个版本（2026.9.6.6+）

一个工具地址是 `[<ns>:]<name>[@<version>]`，它的**身份是
`(namespace, name)` 这一对**。版本是施加在这个包上的约束，从来不是名字的
一部分，因此 `xim:glibc`、`xim:glibc@2.40`、`xim:glibc@>=2.38` 点名的都是
同一个包。每次构建安装它的一个版本。

选哪一个，分两步决定。

**裁决——离产物更近的声明胜出。** 工程胜过它所依赖的一个包，
因此一个 pin 会盖过某条规则的要求：

```toml
# the project, when it does have an opinion
[target.'cfg(accelerator = "cuda")'.xlings.workspace]
"xim:cuda-nvcc" = "13.3.33"
```

一个不点名版本的声明是弃权：它只说明这个包是需要的，对哪个版本什么都没说，
因此它不能仅凭距离更近就压过一条下限。当两条声明彼此不一致、且都点名了
版本时，mcpp 报告采用的是哪一个——一次仅表现为「声明了两个版本，
而磁盘上只有一个目录」的覆盖，是一个读者要靠自己从文件系统里重建的事实。

**校验——胜出者必须满足每一条落败的要求。** `>=`、`^`、`~`
以及逗号组合的写法都是要求。一个不满足某条要求的 pin 会被拒绝，
并同时指出两侧：

```
error: `xim:cuda-nvcc` is pinned to 12.0.0 by this project, and mcpp:plugins
       requires >=12.9.86.
       One version of a package is installed, so the two cannot both hold.
       fix: pin a version satisfying >=12.9.86, or drop the pin and let the
       requirement decide.
```

一个裸版本是一次*选择*，不是一条要求：两个不同的精确 pin 会被裁决并报告，
而不是被拒绝。只有一条被陈述的要求才能被违反。

**这是一次比较，不是一次搜索。** 版本先被裁决、再被校验，因此 mcpp
从不需要去问索引存在哪些版本，也不携带约束求解器。代价被明说而不是被隐藏：
一个求解器本可以满足的组合——工程的 `>=8.0`、规则的 `8.5.0`、索引中最新的
是 8.3——会被直接拒绝，拒绝信息说明如何处理。

范围在两个方向上都会被解析。`>=2026.1` 安装满足它的最高已发布版本，
`>=2099.1` 因无法满足而被拒绝，`mcpp::xpkg_dir` 给出满足该范围的
**已安装**最高版本——由此，一条声明了下限的规则能够找到那个下限
带进来的东西。

### 工程没点名的工具，其版本的来源

| 工程的声明 | 版本来自 |
|---|---|
| `[xlings.workspace]`，不带 `subos` | 机器自身的环境，工程自己的条目叠加在其上 |
| `[xlings.workspace]` 且 `subos = "<name>"` | 该环境自己的 workspace；机器的不生效 |
| 两者都不写 | 机器自身的环境 |

中间一行不是遗漏。一个具名环境有它自己已安装的集合，把机器的版本带进去
只会点名一些根本不在那里的版本。点名一个环境，是工程要求隔离的方式；
不写，则是工程要求「机器的环境，加上自己那些条目叠加在上面」的方式。

在工程内部执行的一次 `xlings use` 优先于这张表，直到 mcpp 重写该环境为止，
因为它是最后被合并的一层。

### `deps`，已被取代

`deps = ["xim:qemu-riscv@9.2.4-1"]` 是同一句陈述在 2026.9.3 之前的写法。
它仍然被采纳，并被报告一次，同时给出应当改写成的 `[xlings.workspace]`
那一行。它不会被拒绝，因为拒绝会触及*依赖*的 manifest，
而钉住了那个包某个精确版本的工程无法编辑它。

### `envs`，已移除

`[xlings.envs]` 过去被物化进 `.xlings.json`，却没有任何东西读取它：
一个程序的环境由它自己的包声明，一个环境的环境由那个环境本身声明。
这个键现在是错误，并同时点名两者。索引中没有任何东西用过它。

## 8. 相关章节

- [30 —— 构建程序：`build.mcpp`](30-build-mcpp.md) —— 构建程序收到的契约，
  包括它运行时所用的 `PATH`。
- [91 —— 工具链内幕](91-toolchain-internals.md) —— 运行时选择、
  `RuntimeBinding` 快照，以及降级规则。
- [04 —— mcpp.toml](04-mcpp-toml.md) —— manifest 的其余部分。

## 当前边界

- **工具不能以加速器为条件。** 加速器在依赖图之后才解析，因此这样的工具
  会被声明、却永远不会被安装——一次成功的构建里那个工具干脆缺席。
  写了这样一条的 manifest 会被拒绝。
- 离产物更近的声明胜出，覆盖会被报告。一个不满足对侧所陈述要求的 pin
  会被拒绝，并同时指出两侧，而不是与之并存安装。
