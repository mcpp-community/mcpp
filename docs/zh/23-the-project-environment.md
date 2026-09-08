# 23 —— 项目环境

项目可以声明自己在哪个环境里构建。这一条声明决定项目链接哪个 C 库、以及它的
构建程序找到哪些工具——于是同一份 `mcpp.toml` 在开发机和 CI 上是同一个构建,
不论这两台机器上还装了别的什么。

```toml
[xlings]
subos = "tools"

[xlings.workspace]
"xim:qemu-riscv" = "9.2.4-1"
```

可运行的工程:`examples/07-project-subos/`。

## 1. SubOS 是什么

SubOS 是一个目录,里面是一份用户态:它自己的 `bin`、自己的库视图、自己那套已
装包版本,以及一个自述用的 `subos_info` 块。mcpp 把它当作「这个项目对着什么
构建」的答案,而且是回答这个问题的唯一机制——不是编译器所在路径,不是
`XLINGS_ACTIVE_SUBOS`,也不是当前 shell。

存在两种,区别在于目录落在哪里:

| 声明 | 目录 | 与谁共享 |
|---|---|---|
| 未声明 | mcpp 初始化的 `subos/default` | 机器上的每个项目 |
| `subos = "default"` | 同一个目录,只是被显式点名 | 机器上的每个项目 |
| `subos = "<name>"` | `<project>/.mcpp/.xlings/subos/<name>/` | 不共享 |

第三行是隔离的那种。它属于该项目,就放在清单旁边,删掉项目它也随之消失。

## 2. 这条声明决定什么

**C 库。** payload-first 的构建链接的是某一个确定的 glibc,而「哪一个」是项目
的性质而非机器的性质。第 8 章讲绑定本身、降级规则,以及一个不自述的 SubOS 会
让这些规则变成什么。

**构建程序看见哪些工具**(mcpp 2026.8.25.1+)。被声明环境的 `bin` 放在
`build.mcpp` 运行时 `PATH` 的最前面:

```
PATH=<被声明环境的 bin>:<mcpp 自己启动时的 PATH>
```

因此构建程序里把 `qemu-system-riscv64` 写成裸名,拿到的就是那个环境里的副本。
这条通道的契约见第 7 章。

**只对声明了的项目生效。** 没有 `[xlings].subos` 的项目拿到的是 mcpp 启动时
的 `PATH`,逐字节不变。把一个共享目录放到每个项目前面,会让「构建看见什么」
取决于这台机器上还装过什么——同一台机器上的两个项目彼此一致,而同一个项目在
两台机器上不一致。是声明本身把它放到了前面。

**前置而非替换。** 构建程序理应会调 `git`、`python3` 或 shell,这些都不在
SubOS 里。前置让被声明的环境成为默认答案;其余的仍在它后面可达。

### 2.1 哪些版本钉生效(2026.9.3+)

指名一个环境,同时改变了工具的版本从哪来。工程自己 `[xlings.workspace]` 里的条目
总是胜出;不同的是它们叠在什么之上:

| 工程声明了 | 它没点名的工具,版本来自 |
|---|---|
| `[xlings.workspace]`,无 `subos` | 机器的环境 |
| `[xlings.workspace]` 与 `subos = "<名>"` | 那个环境自己的 workspace;机器的不适用 |

第二行就是隔离的含义。指名的环境有自己的已安装集合,把机器的版本带进去会指向那里
不存在的版本 —— 所以原本依赖「机器上装了就能用」的工程,一旦指名环境,就必须把用到
的都声明出来。

在工程内执行的 `xlings use` 压过这两者,直到 mcpp 重写环境为止:它是最后合并的那
一层,而人做出的动作应当压过一份文件。

## 3. 这条声明不决定什么

`[xlings.workspace]` 声明的是「环境里要有哪些包」,而每个包的载荷目录另有通道交付,
即 `MCPP_XPKG_<NAME>_DIR`。这与 `PATH` 是两个问题,答案也保持分开:需要某个包
的数据文件(比如 protoc 自带的 well-known `.proto`)的构建程序问目录,需要
**运行**某个程序的构建程序问 `PATH`。

**工作区成员的声明不是工作区的声明。** 工作区构建中由工作区根持有这个选择;成员的
`[xlings]` 只在该成员作为独立根被构建时生效。

**依赖的声明是另一回事,而且它被采纳**(`[xlings] deps` 自 2026.9.5.4,下面那条版本
规则自 2026.9.6.6)。板级支持包知道哪个模拟器够得到它那台机器,规则包知道它驱动哪个
工具包;要消费者把这些再写一遍,正是这类包存在的意义所反对的重复。依赖声明的东西会被
装上,而 `MCPP_XPKG_<NAME>_DIR` 在那个依赖自己的构建程序里为它作答。

工程与依赖命名**同一个包**时,只装它的一个版本:身份是 `(namespace, name)`,版本是这个
包上的约束。离产物更近的声明赢,并且覆盖会被报出来;不满足对方所陈述之要求的钉会被拒绝
并点出两侧。见 [03 — mcpp.toml](03-mcpp-toml.md) 的「一个包一个版本」。

## 4. 只读取环境,从不创建环境

mcpp 解析被声明的名字,并读取它找到的东西。解析不到的名字是硬失败:

```
error: selected SubOS 'tools' does not exist at …/.mcpp/.xlings/subos/tools;
create/bootstrap that environment instead of falling back to active/default
```

回退到 default 或回退到当前活跃的那个,等于用另一个环境顶替清单点名的那个,而
这恰恰会让一份 `mcpp.toml` 意味着两个不同的构建。创建并填充 SubOS 属于 xlings
这一层——`xlings subos new`——mcpp 去管理 SubOS 状态则是把分层倒置。

一个存在但**不携带 `subos_info` 块**的环境是**降级而非失败**:运行时绑定报
`inconclusive`,没有 payload-first 绑定可用,打印一条提示,构建继续。完整规则见
第 8 章。

## 5. 什么时候值得用私有环境

- **产物取决于版本的代码生成器。** `protoc`、`flatc`、着色器编译器:它的输出是
  下游一切的输入,所以项目钉住生产者,而不是指望机器上那个恰好兼容。
- **构建程序要运行的模拟器。** 若干裸机包把「在 QEMU 里启动产物」作为验证的一
  部分;是哪个 QEMU 属于「验证了什么」的一部分。
- **CI 与开发机不一致的项目**,两边都没错,而构建不该察觉到差异。
- **同一台机器上两个项目需要同一工具的不同版本。** 共享目录意味着必有一方落
  败;私有环境让这个问题不成立。

代价一侧:隔离环境是一个必须被创建并填充的目录,而这笔账由首次构建来付。
2026.8.29 起 mcpp 会做这件事 —— 声明在 `[xlings.workspace]` 里的包在首次使用时被供给,
一个尚不存在的具名 `[xlings] subos` 会被创建而不是被拒绝 —— 但代价是实打实的:
干净机器上的第一次构建会先下载安装,然后才编译。工具很普通、版本也无所谓的项目,
不声明、直接继承机器的那份更划算。

在 `--offline` / `MCPP_OFFLINE` 或 `MCPP_NO_AUTO_INSTALL` 下,mcpp 转为拒绝而不是
安装,并列出包名以便手动供给 —— 与 `[toolchain]` 遵守的是同样两个开关,理由也相同:
一次没被要求的下载,不该由构建替工程决定。

这份声明在每台构建本工程的宿主上都会供给,宿主装不了的包是错误,不是被跳过的条目。
只存在于某一个宿主平台的工具因此按平台声明(2026.9.2.1):
`deps = [{ linux = "qemu-user-aarch64" }]` 在 Linux 上声明这个模拟器,在别处什么都不声明。
键与解析规则见本章。

**哪些命令会安装它。** 一条条目可以带档位 —— `{ version = "0.24.0", when = "run" }` ——
`[feature-xlings.<feature>]` 则把工具挂在某个 feature 上。用不到的工具因此不会被下载:
见本章。不写档位就是从前的行为。

**runner。** `[xlings.workspace]` 下的程序也是 `[target.<triple>].runner` 查找其第一个元素
的首选位置,在 `PATH` 之前([03 §2.7.3](03-mcpp-toml.md))。两个键合起来,在 CI 宿主上供给用户态模拟器,
并通过它执行交叉构建的产物,而清单不必写出载荷的路径。

## 6. 什么该写在别处

| 需求 | 写在哪里 |
|---|---|
| 程序链接的库 | `[dependencies]` |
| 编译器 | `[toolchain]`,第 3 章 |
| 依赖产出的宿主工具 | `tools = [...]`,第 7 章 |
| 环境里要有的工具 | `[xlings.workspace]` |
| 只有某个命令或某个 feature 需要的工具 | `when = "run"`、`[feature-xlings.<f>]` |
| 用哪个环境 | `[xlings] subos` |

## 7. `[xlings]` —— manifest 键

```toml
[xlings.workspace]                 # 这个工程的环境里有什么
cmake                    = "3.28"
"xim:picolibc-riscv"     = "1.8.12"        # 带命名空间的包 —— 必须带引号
code                     = ""              # 存在即可,版本不限
llvm                     = { macosx = "20", default = "22" }
```

```toml
[xlings]
subos = "dev"                      # 指名的隔离环境
```

`[xlings]` 是 mcpp 对 **xlings local project 机制**的书写面:让一个目录拥有自己
环境的那份项目 `.xlings.json`。子段名与含义都是那份文件的,mcpp 原样物化进
`<project>/.mcpp/.xlings.json`,没有翻译层。

**`[xlings.workspace]` 是唯一的表。** 一条条目写出工程用哪个包、用哪个版本。
mcpp 既供给它——机器上没有就装,有就映射——也把它物化成解析用的钉,于是工程写下
的版本就是它的工具解析到的版本。

### 条目的形式

| 形式 | 含义 |
|---|---|
| `cmake = "3.28"` | 该版本 |
| `llvm = "22"` | 已装的最高 `22.*`;版本前缀会被解析 |
| `code = ""` | 存在即可,版本不限 |
| `"xim:picolibc-riscv" = "1.8.12"` | 来自 `xim` 索引的包 |
| `llvm = { macosx = "20", default = "22" }` | 按宿主平台 |

**带命名空间的包写成 `"<命名空间>:<名字>" = "<版本>"`,引号必需** —— TOML 的裸键
不能含冒号。这是**推荐形态,也是所有官方包使用的形态**:一条条目先点名一个包,
再说用它的哪个版本,所以命名空间属于名字。

命名空间写在版本上(`picolibc-riscv = "xim:1.8.12"`)同样接受,因为物化出来的
`.xlings.json` 里正是那种形态 —— 那里的键是 xvm target,scope 限定的是版本。
两套词汇,同一条条目。两半都写且不一致是错误;同一个包用两种拼法出现两次也是错误。

平台键是 xlings 自己的 —— `linux`、`macosx`、`windows`,外加 `default`。`macos`
与 `macosx` 是同一个平台的两套词汇(mcpp 的三元组说前者,描述符与 xlings 的项目
文件说后者),**凡是点名平台的地方两者都接受**。表里既没有本机这一项也没有
`default`,就表示在这里什么都不声明。

### 两条解析轴 —— 宿主与目标(mcpp 2026.9.6.4+)

一条工具条目回答的是两个不同问题中的一个,写在哪张表里决定了是哪一个:

| 写法 | 轴 | 按什么解析 |
|---|---|---|
| `[xlings.workspace]`,平台键写在值里 | 宿主 | 跑这次构建的机器 |
| `[target.<selector>.xlings.workspace]` | 目标 | 解析后的目标(`--target`,否则是宿主) |

两种写法都是正确的,谁也不取代谁。在构建机上执行的工具属于宿主轴;产物编译或链接
时对着的载荷属于目标轴。

```toml
[xlings.workspace]
"xim:dpcpp" = "7.1.0"              # 一个编译器,它在本机上跑

[target.'cfg(os = "linux")'.xlings.workspace]
"xim:glibc"         = ""           # 设备单元编译时对着的东西
"xim:linux-headers" = ""
```

非交叉构建时两条轴指向同一个平台,所以把目标事实写在宿主轴上的工程是碰巧正确的,
而且照常工作。它在第一次被交叉构建时不再正确。**凡是产物编译或链接时对着的东西,
推荐写在目标轴上。**

`[target.<selector>.feature-xlings.<feature>]` 把条件与门组合起来,与
`[target.<selector>.feature-deps.<feature>]` 同形:selector 说的是哪些目标,
feature 说的是要不要。

```toml
[target.'cfg(os = "linux")'.feature-xlings.backend-vulkan]
"xim:shaderc" = "2026.3"
```

**这里的 selector 禁止命名被解析的层。** `c-abi`、`c++-abi`、`compiler`、
`compiler-runtime`、`kernel-abi` 由依赖解析回答,而依赖解析发生在工具安装之后、
构建程序运行之后。按这些层条件化的工具会被声明却永远装不上——构建照常成功,工具
就是不在——所以这样的 manifest 会被拒绝,并把工具与谓词都点出来。改成按目标条件化,
或者用 feature 做门:`[feature-xlings.<feature>]` 在任何东西被供给之前就已知。

**`accelerator` 是例外,它被接受**(mcpp 2026.9.6.5)。它不由任何东西解析而来:
它是 `--accel`,或 `[build] accel`,在查找第一个包之前就已读入。以它为谓词的载荷
与三元组谓词在同一趟合并,并像其它载荷一样被安装。

```toml
[target.'cfg(accelerator = "cuda")'.xlings.workspace]
"xim:cuda-nvcc"   = "12.9.86"
"xim:cuda-cudart" = "12.9.79"
```

带设备孤岛的工程应当用这种写法。没有它,厂商工具包只能无条件声明或者干脆不声明,
于是不带加速器的 `mcpp build`——最便宜的那次构建,也是 CI 通常跑的那次——会为一个
它根本没在编译的设备下载数 GB。

依赖同理:`[target.'cfg(accelerator = "cuda")'.dependencies]` 生效,而以被解析的层
为条件的依赖不生效,因为后者会决定它正在询问的那个答案。加速器这条路径上没有任何
循环。

条件只写在 selector 一处。selector 之下的值如果又带平台键,就是同一件事说了两遍,
会被拒绝,并把两半都指出来:

```
[target.cfg(os = "linux").xlings.workspace] xim:tool: the value carries platform
keys (linux, macosx), but [target.cfg(os = "linux")] already says which targets
this applies to.
```

`subos` 不按目标条件化:一个工程只有一个环境,所以 `[target.<selector>.xlings]`
拒绝这个键,而不是把它丢掉。

**已发布的描述符不为目标轴条目携带边**,`mcpp publish` 会说明这一点。描述符按平台
分块,而 selector 不是平台 —— `cfg(target_arch = "aarch64")` 不对应那份文件里的任何一块。
**使用者**装到的东西来自顶层 `[xlings.workspace]`;目标轴对"本包自己的构建对着什么"
仍然是正确的。

这两条轴所属的一般规则见 [SPEC-004](../specs/manifest-semantics.md)。

### `when` —— 哪些命令需要这个工具(mcpp 2026.9.4.2+)

```toml
[xlings.workspace]
"xim:qemu-arm"   = "9.2.4-1"                             # 每次构建都装,与从前一样
"xim:codegen"    = { version = "1.0",    when = "build" }
"xim:probe-rs"   = { version = "0.24.0", when = "run"   }
"xim:clang-tidy" = { version = "20",     when = "dev"   }
```

包依赖从一开始就有这条轴 —— `[dependencies]`、`[build-dependencies]`、
`[dev-dependencies]`。工具只有一张表,于是一个同时点名模拟器与调试探针的板级支持包
会把两个都装给每一位消费者,包括只想把库编出来的那一位。

| `when` | 由谁安装 | 是否传播到消费者 |
|---|---|---|
| *(不写)* | 每个构建命令 | 是 |
| `build` | 每个构建命令 | 是 |
| `run` | `mcpp run`、`mcpp test` | 是 |
| `dev` | 只有声明它的那个包作为根时 | **否** |

**不写 `when` 就是 2026.9.4.2 之前的行为**,所以没有任何清单需要改。收窄是可选动作,
不是作者必须回答的新问题。

`dev` 是唯一不传播的一档。它的含义是「声明它的那个包自己在被开发时」,所以依赖的
`dev` 条目永远不会为消费者安装。其余各档都会到达消费者 —— 这正是板级包知道自己机器
的意义:它声明一次模拟器,每一位消费者都拿得到。

档位写在**条目**上而不是另开一张表,理由与 `[dependencies]` 同时接受 `dep = "1.0"`
和 `dep = { version = "1.0", features = [...] }` 是同一条。带档位的条目**必须**写出
`version`,哪怕留空(`version = ""` 表示「存在即可,版本不限」)—— 否则
`{ when = "run" }` 与写错的 `version` 键无法区分。

### `[feature-xlings.<feature>]` —— 某个 feature 才需要的工具

```toml
[features]
default  = ["emulator"]
emulator = {}
hardware = {}

[feature-xlings.hardware]
"xim:probe-rs" = "0.24.0"
```

同一张表,按 feature 门控,拼法沿用 `[feature-deps.<feature>]`。**不要 `hardware`
的消费者永远不会下载探针驱动。** 这里的条目同样接受 `when`。

`[features]` 里没有声明过的 feature 名会作为 schema 警告报出:它对谁都不激活、什么
都不装,而这种工具的缺席只表现为「设备就是连不上」,是最难诊断的一种。

### 规则包自带它的环境(2026.9.6.6+)

上面那张表是工程**有主张**时写的。多数工程没有主张,写下的也就是空无一物:

```toml
[build-dependencies.mcpp]
plugins = { version = "0.3.0", features = ["rules-cuda"], host-module = true }
```

这一条边就是全部声明。规则包在选中它的那个 feature、它所服务的加速器之下,声明自己
需要哪些包、最低到哪一版:

```toml
# 写在规则包里,不写在你的工程里
[target.'cfg(accelerator = "cuda")'.feature-xlings.rules-cuda]
"xim:cuda-nvcc"   = ">=12.9.86"
"xim:cuda-cudart" = ">=12.9.79"
```

**两重门,都要开。** feature 说「要不要这个规则」,selector 说「哪些构建真的下载」。
同一个工程的 CPU-only 构建两道门都不过,一个字节都不装。

「需要哪些包、最低到哪一版」是规则作者的知识。在每个用它的工程里重写一遍,是一份会
悄悄过期的副本——规则动了,而那些工程不会跟着动。

### 一个包一个版本(2026.9.6.6+)

工具地址是 `[<ns>:]<name>[@<版本>]`,它的**身份是 `(namespace, name)` 二元组**。版本是
这个包上的约束,不是它名字的一部分,所以 `xim:glibc`、`xim:glibc@2.40` 与
`xim:glibc@>=2.38` 指的是同一个包。**一次构建只装它的一个版本。**

装哪一个,分两步决定。

**裁决——离产物更近的声明赢。** 工程压过它依赖的包,于是一个钉覆盖规则的要求:

```toml
# 工程侧,当它确实有主张时
[target.'cfg(accelerator = "cuda")'.xlings.workspace]
"xim:cuda-nvcc" = "13.3.33"
```

**不带版本的声明弃权:** 它陈述了「要这个包」而没有陈述「要哪一版」,因此不会仅仅
因为更近就压过一条下界。两条声明都带版本且不一致时,mcpp 会报出用了哪一条——一个
只能表现为「声明了两个版本而目录里有一个」的覆盖,是要读者自己去文件系统里重建的
事实。

**校验——赢家必须满足每一条落败的要求。** `>=`、`^`、`~` 以及逗号组合是**要求**。
不满足的钉被拒绝,并同时点出两侧:

```
error: `xim:cuda-nvcc` is pinned to 12.0.0 by this project, and mcpp:plugins
       requires >=12.9.86.
       One version of a package is installed, so the two cannot both hold.
       fix: pin a version satisfying >=12.9.86, or drop the pin and let the
       requirement decide.
```

裸版本是**选择**而不是要求:两条互不相同的精确钉走裁决并被报告,不被拒绝。只有被
陈述出来的要求才谈得上违反。

**这是一次比较,不是一次搜索。** 版本由裁决选定、再被检查,所以 mcpp 从不需要问索引
「有哪些版本」,也就不带约束求解器。代价被写出来而不是藏起来:一个求解器本可满足的
组合——工程写 `>=8.0`、规则写 `8.5.0`、而索引里最新是 8.3——会被拒绝,而拒绝消息里
写着怎么往下走。

范围在两个方向上都被求解。`>=2026.1` 装到满足它的最高已发布版本,`>=2099.1` 作为
不可满足被拒绝,而 `mcpp::xpkg_dir` 回答满足该范围的最高**已安装**版本——声明了下界
的规则找得到下界带进来的东西。

### 工程没点名的工具,其版本的来源

| 工程声明了 | 版本来自 |
|---|---|
| `[xlings.workspace]`,无 `subos` | 机器的环境,工程自己的条目叠在上面 |
| `[xlings.workspace]` 与 `subos = "<名>"` | 那个环境自己的 workspace;机器的不适用 |
| 两者都没有 | 机器的环境 |

中间那行不是遗漏。指名的环境有自己的已安装集合,把机器的版本带进去会指向那里不
存在的版本。**写 subos 就是要隔离,不写就是要机器的环境加上自己的条目。**

在工程内执行的 `xlings use` 压过这张表,直到 mcpp 重写环境为止——它是最后合并的
那一层。

### `deps`,已被取代

`deps = ["xim:qemu-riscv@9.2.4-1"]` 是同一句话在 2026.9.3 之前的拼法。它仍然生效,
并且会被报告一次,同时给出该写的 `[xlings.workspace]` 那一行。**不拒绝**——拒绝会
落到**依赖**的 manifest 上,而钉了那个包精确版本的工程改不了它。

### `envs`,已移除

`[xlings.envs]` 曾被物化进 `.xlings.json`,而没有任何东西读它:程序的环境由它自己
的包声明,环境的环境由那个环境声明。现在这个键是错误,并同时点名这两者。索引里没有
任何包用过它。

## 8. 相关章节

- [30 - build.mcpp](30-build-mcpp.md) —— 构建程序收到的契约,含它运行时的
  `PATH`。
- [91 - 工具链内部](91-toolchain-internals.md) —— 运行时选择、`RuntimeBinding`
  快照与降级规则。
- [03 - mcpp.toml](03-mcpp-toml.md) —— manifest 的其余部分。
