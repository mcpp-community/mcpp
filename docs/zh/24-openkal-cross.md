# 24 —— 基于 openkal 的交叉构建

**读者：** 从本机为另一个操作系统做交叉构建的人。

**本章回答的那一个问题：** 一份源码树怎样为多个 hosted target 构建，而本机
并没有它们各自的工具链。

**不在这里：** 没有操作系统的 target，那是 [40 —— 裸机](40-baremetal.md)；
以及 target 怎样命名，那是 [21 —— 目标三元组](21-the-target-triple.md)。

传统的交叉构建由载荷承担。一份工具链为一个 target 构建，它的驱动器只有一个
答案，到达第二个 target 意味着获取第二份工具链。因此一个发行方必须发布的
载荷数，等于它所支持的宿主-目标对数。

openkal 改变的是「被交叉的是什么」。目标侧 —— 平台接口、C 库、编译器运行时
与 C++ 运行时 —— 变成一组由依赖图解析、并由当前运行的编译器从源码编译的包。
留给编译器的只剩代码生成，而一个 Clang 二进制会发出它被构建时所支持的每一种
对象格式。

本文陈述这个模型、一个工程要书写的内容、生态供给的内容，以及已被实测过的
界限。

## 论断

一个由 N 个平台与 M 个架构构成的生态，需要的是一个接口的 N 份实现，而不是
N×M 份工具链。这个计数来自目标侧所在的位置：一个从源码构建的包，是为编译器
被要求发出的那个 target 而构建的，因此一份平台实现只需写一次，就能到达编译器
支持的每一个架构。

这个论断由一个三宿主 × 三目标的矩阵验证，每一格构建同一份源码并运行其结果。

## 三层，三组宏（mcpp 2026.9.18+）

一次基于 openkal 的构建要回答三个不同的问题，而在这项能力落地之前，一个宏
（`_WIN32`）同时回答了其中两个 —— 这正是每一次 openkal-Windows 失败的根因，
它们的诊断无一例外指向某份缺失的平台头文件：代码本想问的是「这是不是
openkal」，用的却是一个实际含义为「Windows CRT 是否在场」的宏。

| 宏族 | 陈述 | 定义者 | 例子 |
|---|---|---|---|
| 内核 ABI | `kal_*` 可调用，且在每个平台上行为一致 | 提供 `mcpp:kernel-abi=openkal` 的那一层 | `__OPENKAL__` |
| C 环境 | 源码所见 C 环境的形状 | 提供 `mcpp:c-abi=<impl>` 的那一层，经由 [`[c-abi]`](22-target-side.md#c-abi-包陈述它呈现的-c-环境mcpp-2026918) | `__unix__`、`_WIN32`、`__MINGW32__` |
| 系统与架构 | 底层操作系统与处理器 | 目标三元组 | `__linux__`、`__APPLE__`、`__x86_64__` |

还有第四件事搭着 C 环境这一行的便车，但并不是同一个问题：链接器产出的
**对象文件格式** —— PE、ELF、Mach-O —— 是**目标三元组**的属性，不是
`presents` 选出的那个 C 环境的属性，二者可以互不一致。Windows 上
`presents = "posix"` 链接出来的仍然是 PE；第三方可移植代码给这个特定组合 ——
「PE 格式配上呈现 POSIX 的 C 环境」—— 起名字的唯一办法，是
`__CYGWIN__`/`__CYGWIN32__`，所以 Cygwin 式的实现让它们保持**已定义**，而不
是把它们折进上面三行里的任何一行 —— 完整的权衡说明见
[22 自己的说明](22-target-side.md#c-abi-包陈述它呈现的-c-环境mcpp-2026918)。
自 2026.9.21.1 起，`__CYGWIN__` 不再是那个事实的唯一名字：mcpp 为每一个
target 定义 `__MCPP_TARGET_<OS>__`（见 `docs/21`「mcpp 定义的宏」），于是
需要在一个被呈现的环境之下得知目标本身的源码，就有了一个 mcpp 自己拥有的
名字。`__CYGWIN__` 会在生态的已安装头文件迁移到新名字期间继续保留定义，之后
才撤销。把这两者中的任何一个当成第四个 C 环境宏来读，而不是把它们读成它们
实际是的样子 —— 一个关于**目标**的事实，而不是关于其上被呈现的环境的事实 ——
正是这一整节想要提前避免的那种混淆。

**`__OPENKAL__` 的规则。** 只要解析出的 `kernel-abi` 层的接口名是
`openkal`，引擎就为目标侧的每一个编译单元定义它 —— 取自层自己的取值，绝不
取自包名，因此第二个提供 `mcpp:kernel-abi=openkal` 的实现不需要引擎改动。

*允许的用法：* 用它来判断某处调用点是否要调用 `kal_*`。它在每个 target 上
含义相同，因此这样使用它绝不会把平台信息夹带进本该与实现无关的源码。

*禁止的用法：* 用它选择一个头文件、推断 `_WIN32` 是否为真、绕开一个缺失的
SDK，或者区分 `linux`/`windows`/`macos`。那些是 C 环境层或平台层的问题 ——
应改在清单里写 `cfg(c-abi = "…")` 或 `cfg(kernel-abi = "…")`（谓词语法见
[22 —— 对已解析目标侧的适配](22-target-side.md#对已解析目标侧的适配)）。

**平台单元。** 自身需要平台原生环境的包，从不通过读取 `__OPENKAL__` 或其他
任何宏来推断这件事 —— 边界写在清单里，不从源码推断，而跨越这道边界的一切
仍然只能是定宽类型（SPEC §5.4）。两类不同的包，经由两条不同的路径到达
`[package] c-environment = "platform"`（docs/22）：

- 一个提供 `mcpp:kernel-abi=<impl>` 的包（比如 openkal-windows），这个值对
  它是**推导**出来的，单凭 `provides` 就够 —— 这样的包按定义本身就是平台
  边界，因此它完全不需要自己写这个键，每一个已经发布过的实现也都自动覆盖，
  不必发新版本。
- 一个普通包，本身不是 kernel-abi 的提供者，却确实带有自己平台绑定的单元 ——
  比如 [06 私有依赖模式](06-features-and-capabilities.md#平台-sdk-依赖保持私有)
  下的一个平台 shim —— 需要**显式**声明这个键，因为引擎没有 `provides`
  条目可供推导（设计 §5.3）。

两种途径都能用得上的地方，显式声明的键永远优先于推导（docs/22 自己的优先级
说明）—— 但目前还没有办法反着写出「不是 platform」，因此实践中这条优先级
真正用得上的地方只有一种少见情形：一个 kernel-abi 的提供者，居然还需要图所
呈现的那个 C 环境。

## 工程要写的内容

```toml
[dependencies]
openkal-llvm-runtime = "0.1.1"

[toolchain]
default = "llvm@22.1.8"
```

两行。第一行选定目标侧的三个层；第二行命名一个编译器，并且对其余一切来自
何处只字未提。

target 在命令行上给出：

```bash
mcpp build --target x86_64-linux
mcpp build --target aarch64-macos
mcpp build --target x86_64-windows-gnu
```

一个 hosted target 不需要 `[target.<triple>]` 段，源码中也不需要任何预处理
指令。可运行的示例见 [examples/06-openkal-cross](../../examples/06-openkal-cross)。

## 生态供给的内容

| 包 | 层 | 内容 |
|---|---|---|
| `openkal` | — | 规范本身，以及声明它的那些 C++ 模块 |
| `openkal-linux` | `kernel-abi` | 参考实现，建立在 Linux 系统调用之上 |
| `openkal-macos` | `kernel-abi` | 建立在 macOS 的系统调用面之上 |
| `openkal-windows` | `kernel-abi` | 建立在 Win32 与对象管理器之上，不使用任何 C 运行时符号 |
| `openkal-opensbi` | `kernel-abi` | 建立在 RISC-V 的 Supervisor Binary Interface 之上，无操作系统 |
| `openkal-uefi` | `kernel-abi` | 建立在 UEFI Boot Services 之上，在操作系统存在之前 |
| `openkal-musl` | `c-abi` | 被重定向到 openkal 上的 musl，只移植过一次 |
| `openkal-llvm-runtime` | `compiler-runtime`、`c++-abi` | compiler-rt 内建函数、libunwind、libc++abi 与 libc++，为 openkal-musl 配置 |

一个工程命名其中最后一个，其余的由它的依赖推出。

## 编译器必须是 LLVM 的原因

`openkal-llvm-runtime` 把这项要求直接声明出来，而不是留给使用者自己去发现：

```toml
requires = ["mcpp:compiler=llvm"]
```

它的源码就是 libc++ 的源码，其中的 `std` 模块源尤其只由 Clang 编译。把那份
源码交给 GCC，会在 libc++ 自己的头文件深处失败，报错信息里点名的是一个读者
从未打开过的文件：

```
fatal error: __config: No such file or directory
```

有了这项声明，构建会在编译任何东西之前就拒绝这种组合，并指出用哪条命令可以
选中一个满足它的编译器。

## 目标的选定

mcpp 自身词表里的目标行可以携带一条工具链约定。这条约定命名的是**供给该
target C 库的那份载荷**，并且只在两个条件同时成立时才生效：清单对该 target
没有任何陈述，且依赖图中没有任何东西供给该 target 的系统。

第二个条件只有在图被解析完之后才可知。因此一个 C 库来自 `openkal-musl` 的
工程，仍然保留它自己要求的那个编译器，而一个没有依赖的工程，则仍然拿到该行
所命名的载荷。两种行为都经过实测；提前朝任何一个方向硬性决定，对另一个方向
都是错的。

## 环境段

在 Linux 上，目标三元组的第三段命名的是 C 库。而在 openkal 之下，C 库来自
依赖图，因此一个命名了 C 库的三元组，陈述的只是一项图未必会兑现的请求：

```
mcpp build --target x86_64-linux-gnu     # asks for glibc
      c-abi   musl   (openkal-musl@0.3.3, graph)
```

以图为准。省略这一段等于不作请求，产出的是同一个产物：

```
mcpp build --target x86_64-linux
```

当这一段存在且与图的结果不一致时，构建会报出这一点。它是一次报告而不是一次
拒绝，因为这一段是被忽略而不是被违反。在一台宿主上实测 `x86_64-linux` 与
`x86_64-linux-musl`：两个可执行文件本身不同，strip 之后逐字节相同。差异全在
调试信息里，它记录的是输出目录，而目录名又是按三元组起的。代码是同一份代码。

在 Windows 上，同一段命名的是对象 ABI —— `gnu` 表示 PE 配 GNU ABI，`msvc`
表示 PE 配微软的 ABI —— 而两者都与不止一种 C 库相容。因此这一项不一致报告的
适用范围，被限定在这一段确实命名一个 C 库的那些平台上。

沉默作为**诊断**是对的，作为**报告**却不够。读者看到

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu
       c-abi   musl   (openkal-musl@0.3.3, graph)
```

在其中找不到一行叫 `gnu`，于是把它映射到最像 C 库名字的那一行上去。

对这次构建的产物实测：

| 观测 | 值 |
|---|---|
| 导入的库 | `ntdll`、`KERNEL32`、`SHELL32` —— 没有 `msvcrt`，也没有 `ucrtbase` |
| Itanium 修饰符号（`_Z…`） | 4507 |
| MSVC 修饰符号（`?…`） | 0 |

第一行说明 `c-abi musl` 是老实的：MinGW 的 C 运行时一点没被链接进来。后两行
则是 `gnu` 真正选中的东西 —— Itanium C++ ABI，而不是微软那一套。

它**不对应报告里的任何一行**，而这正是要点所在。五个层记录的是每一层**由谁
供给**；`gnu` 命名的是这些**对象所遵循的约定**，这是一件若干层必须彼此一致
的横切事项。把它读成 `c++-abi libc++` 是第二个错误答案：libc++ 是标准库的
一个实现，libstdc++ 是另一个，两者都坐落在 Itanium ABI 之上。

因此报告直接说出那套 ABI 本身的名字 —— 它不出现在任何一行里，所以不会被
误当成某一层：

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu   (gnu selects the Itanium C++ ABI, not a C library)
```

这一段在不同平台上承载的是不同的轴 —— 在 Linux 上是 C 库，在 Windows 上是
对象 ABI，在没有操作系统时是对象格式 —— 因此记录的是**它究竟是哪一个**，而
不是一个只记录「是不是第一种」的布尔值。

## 这个模型下的 Android、Web 与 iOS

mcpp 在 2026.9.11.3 里新增目标行的这三个平台，并不是同一个问题。决定每一个
平台答案的，是它的实现相对一个 C 库应该落在哪一侧，而三个平台的答案各不
相同。

### Android 共用 Linux 的实现，一行都不用改

`openkal-linux` 写在 Linux 内核自己的系统调用接口上，不向任何 C 库借用任何
东西 —— 这正是它能被放到一个 C 库**底下**的原因。Android 的内核**就是**
Linux，给定架构上的系统调用 ABI 完全相同，而 `src/sys.h` 是按
`__x86_64__` / `__aarch64__` 分支的，也就是按**架构**而不是按操作系统分支。
它里面没有任何属于 glibc 或 bionic 的东西。

所以一个可移植的程序不需要新增任何一行。`cfg(os = "linux")` 对一个 Android
三元组**为真**，因为 Android 是 `linux` 这个 OS 上的一个 `env` 取值 ——
[21 —— 目标三元组](21-the-target-triple.md) 记着这处建模决定 —— 于是实现
由一个 Linux 消费者本来就会写的那一行选出：

```toml
[target.'cfg(os = "linux")'.dependencies]
openkal-linux = "0.12.0"
```

实测于 2026-09-11，一个只针对 openkal 写的程序 —— 没有 C 库，也没有
`import std`：

```
mcpp build --target x86_64-linux-android
       kernel-abi   openkal   (openkal-linux@0.12.0, graph)
    ->  ELF 64-bit LSB pie, x86-64, interpreter /system/bin/linker64

mcpp build --target aarch64-linux-android
    ->  ELF 64-bit LSB pie, ARM aarch64, same interpreter
```

而那个 x86_64 产物被推送到一台 API 24 的模拟器镜像上执行：

```
openkal: 1-2-3          exit 0
```

`openkal-linux` 自己也能为两个 Android target 原样编译，这是两条论断中较弱
的一条，值得分开陈述：前一条说的是**这份实现**构建得起来，后一条说的是**建
在它之上的程序**运行得起来。

### iOS 共用 macOS 的实现，SDK 是被定位而不是被打包的

同样的论证在 Apple 这一侧成立：iOS 与 macOS 共用 Darwin 内核、同一套系统
调用号、同一套调用约定，`openkal-macos` 也按同样的方式按架构分支。两者之间
真正不同的是 SDK 与部署目标标志，而这两样都属于构建工具而不属于实现 ——
所以 iOS 只是清单里的一行 `cfg`，不需要一个新包。

**被阻塞的是 SDK 本身，而解开它的方法是把问题问得更小。** iPhoneOS 与
iPhoneSimulator 的 SDK 随 Xcode 分发、不可再分发，这界定的是**打包**它们；
它并不界定**定位**它们：`aarch64-macos` 早在这三行存在之前，就已经以完全
相同的切分方式达到 `verified` —— `xim:llvm` 负责编译，机器自己的 macOS
SDK 通过 `xcrun` 被找到。iOS 的这几行采用的是同一种切分，只是加了第二个
SDK，所以它们把 `llvm@22.1.8` 钉死，且不带 `sysroot` 条目：那一列命名的是
一个包，而一个被定位到的目录不是包。

对本文而言，其后果是这几行不再是一个结构性的论证。`openkal-macos` 能为它们
编译，读者需要知道的只是：SDK 是一项具名的宿主依赖 —— 这个平台恰好新增了
两项，另一项是 `simctl` —— 而它的缺失会得到一次点名该 SDK 的拒绝，而不是一次
悄悄产出 macOS 产物的构建。

### Web 需要一份全新的实现，`openkal-emscripten` 正是它

Emscripten 是这三者中**改变模型**而不是扩展模型的那一个。那里没有内核，也
没有系统调用可发：Emscripten 在一个 JavaScript 宿主之上供给它自己的 C 库。
因此给它写的 openkal 实现不可能按 `openkal-linux` 的方式来写 —— 落在一个
C 库**底下**—— 而必须落在一个 C 库**之上**。规范恰好允许这一点（「一个实现
可以建立在一个 C 库之上、之下，或者根本不依赖 C 库」），所以这是一份**新
软件**，而不是一个共用决定。

`openkal-emscripten` 是这个生态里第一份按那个方向写的实现。那个方向让代码
变薄，却不让它变得容易：每个函数大体是一次转发加一次错误翻译，而它必须做对
的地方，恰恰是 C 库的词汇与 openkal 的词汇**不对应**的那些地方 —— 一个是
对齐而不是页的粒度、一个分辨率被浏览器有意变粗的单调时钟、一个在 node 下
存在却在页面里不存在的终端。

**一个不完整的面也可以是一个符合规范的面，而规范写明了该怎么做。** 6.2 条
给出三个时刻，各自是相应信息最早存在的时刻，而这份实现的三组接口各自得到一种
处理：

| 组 | 处理 | 理由 |
|---|---|---|
| `stream`、`fs`、`time`、`env`、`memory`、`random`、`abort`、`terminal` | 提供，转发到 Emscripten 自己的 libc | MEMFS 与 JavaScript 宿主服务了其中每一项 |
| `net`、`datagram`、`timeout` | 提供，由能力字报告哪些确实可用 | 调用是真的，承载它们的是一个 WebSocket 代理，所以 `kal_net_props` 既不声称支持 IPv6，也不声称支持半关闭 |
| `process`、`exec`、`space` | 不提供 | 那里没有 fork、没有 exec，也没有第二个地址空间 |

第三行是值得直说的那个决定：**一个缺席的符号，本身就是那份报告。** 实测：

```
wasm-ld: error: obj/main.o: undefined symbol: kal_process_spawn
```

这正是 6.2 条里的第二个时刻。若提供一个 `kal_process_spawn`、只让它返回
一个错误，会是规范所禁止的那种形状 —— 存在却永远失败，调用方无法把它同一个
条件区分开来 —— 并且会把一个在链接期就已知的事实，挪到运行期才能知道。

`openkal.task` 由一个 feature 承载，理由是这个平台特有的：线程需要
`-pthread`，而这个开关选定的是另一份 C 库构建、另一个内存模型和另一套加载器
契约。不带这个 feature 时，那个翻译单元是空的，那八个符号也不存在 —— 与上面
三个缺席的接口是同一种处理。带上它时它们就存在，`kal_interfaces()` 跟随的是
链接结果本身，而不是包自己发明出来的一个名字。

以上都不能取代载荷那条路。`wasm32-emscripten` 仍然走普通的那条路 ——
`xim:emsdk` 自带编译器、sysroot 与一份 libc++ 的模块面，所以一个使用
`import std` 的程序照样能为 Web 构建并运行，openkal 完全不参与，这正是该行
的 `verified` 层级所记录的东西。openkal 是一个程序想让一份源码立于若干平台
接口之上时才会用到的东西。

### 表

| 平台 | 实现 | 状态 |
|---|---|---|
| Linux（glibc、musl） | `openkal-linux` | 参考实现 |
| Android（两种 ABI） | `openkal-linux`，原样复用 | 构建通过；建在它之上的程序在一台模拟器上运行过 |
| macOS | `openkal-macos` | 建在 macOS 的系统调用面之上 |
| iOS、iOS 模拟器 | `openkal-macos`，原样复用 | Darwin 就是 Darwin；SDK 被定位而不是被打包 |
| Windows | `openkal-windows` | 建在 Win32 与对象管理器之上 |
| Web（Emscripten） | `openkal-emscripten` | 写在一个 C 库**之上**；十五个接口中的十二个 |

## 裸机

一个没有操作系统的 target，是同一个模型，只是平台层由固件而非内核供给。
`riscv64-none-elf` 之上的 OpenSBI 运行的是与 hosted target 相同的源码，含
`import std`，因为它所使用的标准库是依赖图供给的那份，而不是编译器自带的
那份。

有两样东西必须声明，两者都是板子本身的性质，而不是默认值：

```toml
[target.riscv64-none-elf]
sysroot = ""
runner  = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
           "-no-reboot", "-bios", "default", "-kernel"]
```

`sysroot = ""` 选定的是零 libc 档。使用哪种机器模型、哪种固件模式，是板子
自己的事实，而一个替板子去猜测的引擎，是另一块板子必须与之搏斗的引擎。

一个 hosted 交叉 target 使用同一个键，配上一个用户态模拟器（2026.9.2.1）。
在 x86_64 宿主上构建出的一个 `aarch64-linux-musl` 产物，在工程声明后经由
`qemu-aarch64-static` 执行；提供该模拟器的包，按能安装它的那些宿主去声明：

```toml
[xlings.workspace]
"xim:qemu-user-aarch64" = { linux = "" }

[target.aarch64-linux-musl]
runner = ["qemu-aarch64-static"]
```

没有这个键时，`mcpp run` 会报出内核的拒绝（`Exec format error`）以及应当
写的那个键，`mcpp test` 会把每个测试都报告为未运行并以 2 退出。能原生执行
该产物的宿主传 `--no-runner`。规则见 [04 —— mcpp.toml](04-mcpp-toml.md)
§2.7.3。

### 源码是同一份，程序却不是

「同一份源码」是一个关于工具链与标准库的断言，而它成立：`import std` 可用，
C++ 运行时是依赖图供给的那份，没有任何 `#if` 区分不同的 target。但它不是
「任何程序都能为任何 target 构建」的断言，规范对其中的原因写得很明确。

一个裸机后端只提供一部分接口，不提供另一部分。`openkal-opensbi` 提供
`abort`、`stream`、`memory`、`env` 与 `time`；它不提供文件系统也不提供
任务，因为这台机器根本没有这些东西。6.1 条把这种缺席定为一个链接期的事实：

> 一个实现不提供的接口，作为一个链接期定义是缺席的，使用它的消费方链接失败。

因此一个能力字回答的问题，比它初看上去要窄。它说的是一个实现**在它提供的
接口之内**如何表现 —— 名字是否区分大小写、一个时钟的粒度是多少。接口是否
存在这件事，由更早的东西回答：依赖图，退而求其次由链接器回答。

这个区分容易被丢掉，因为这类查询是数据对象上的内联函数：一个程序**仅仅是
提问**「有没有文件系统」，就已经取了 `kal_fs_props` 的地址，于是在整份源码
里没有任何一处文件系统调用的情况下链接失败。在后端里把那个字定义为零，能够
消掉这个报错，而这恰恰是该条款所禁止的唯一补法 —— 程序因此越过了链接器本该
在此处拦住它的那个点。这条路被走过，发布为
`openkal-opensbi@0.1.3`，随后又被撤回。

### 到达一台裸 x86_64 机器的两条路线

一台没有操作系统的 x86_64 机器有两种不同的到达方式，区别在于**谁在加载**
这个程序。

| 路线 | 目标 | 平台层 | 入口 |
|---|---|---|---|
| UEFI 应用 | `x86_64-windows-gnu` | `openkal-uefi` | 固件，Boot Services 可用 |
| 内核，或裸机 | `x86_64-none-elf` | 无，或 `openarch` | 复位向量，其下一无所有 |

一个 UEFI 应用是 PE/COFF，经微软 x64 调用约定进入。这两点都是 LLVM 工具链
已经具备的性质，因此它的 target 与一个 Windows 程序使用同一个三元组，固件
函数指针被直接调用。把它与一次 Windows 构建区分开的，是依赖图解析出的平台
接口实现，再加上三个选定 `IMAGE_SUBSYSTEM_EFI_APPLICATION` 的链接 flag。

一个内核没有固件服务可以调用。它的 target 是 `x86_64-none-elf`，即零 libc
档：编译行上没有 C 库，链接上没有库目录，`#include <stdio.h>` 无法解析。
程序在它自己的 `_start` 处被进入，直接触达硬件。

`openarch` 是这类程序赖以建立的那一层。它不是一个平台接口，也不应答
`mcpp:kernel-abi`；它是**架构机制**本身 —— 执行上下文、陷阱、逐 CPU 状态与
地址空间 —— 以一个跨越若干指令集的接口呈现，每个指令集配一个后端包。一个
内核依赖它，并供给自己的平台层，或者什么都不供给。

### x86_64 裸机需要引擎侧工作的原因

`riscv64-none-elf` 与 `aarch64-none-elf` 只是表里的两行，仅此而已：Clang
对两者都有 BareMetal 工具链，会自行驱动它们的链接并到达 `ld.lld`。它对
x86_64 没有这样的工具链，因此那个三元组落到通用的 GCC 工具链上，而后者的
链接器是宿主的 `g++`：

```
g++: error: unrecognized command-line option '-fuse-ld=…/ld.lld'
```

对裸 x86_64 三元组的每一种拼写都做过实测，且无法用任何 flag 纠正。因此这一
行携带一个链接器 emulation，由 mcpp 自己调用 `ld.lld` —— 这也是为什么必须
证明宿主工具链没有参与这样一次链接。

## 已实测的界限

三条，之所以记录在此，是因为每一条都是由构建发现的，而不是由阅读发现的。

**一个后端必须定义每一个能力字。** 规范里的查询是属性对象上的内联函数，因此
一个仅仅提问「有没有文件系统」的程序，就已经取了 `kal_fs_props` 的地址。一个
省略了自身所缺各层能力字的后端，会让这个问题恰恰在它为之存在的那一类机器上
链接失败。

**同一层出现两个供给者是一个错误，不是一个可选项。** C 库、平台接口与 C++
运行时三者互斥。选错并不会让链接失败，它产出的是一个能够运行、却又间歇性
不能运行的程序。

**一份载荷的 C++ 运行时不能建立在一个外来的 C 库之上。** 它的
`__config_site` 记录着它被构建时的配置。解析器的结构在默认路径上就阻止了
这种组合，而一条诊断覆盖了工程显式覆写该契约的那些路径。

## 边界

五个层各自只保证自己那一层，层与层之间的边界，正是每个包自身适配工作应当
所在的地方。

**`kernel-abi = openkal` 只保证跨越 `kal_*` 的行为，再远的事它不保证。**
规范本身的接口与平台无关；一项缺失的能力在链接期暴露，一项缺失的属性由 props
查询回答。它不说明上面架着哪个 C 库，不说明某个平台 SDK 是否可达，也不说明
包其余那部分源码是否可移植。

**`c-abi = musl` 是独立的一层，有独立的保证。** 建在 openkal 的
`kernel-abi` 之上，并不因此就是建在某个特定 C 库之上 —— musl 只是这一层的
一种实现，由与 `kernel-abi` 相同的依赖图解析得到；一处头文件或 CRT 差异
（`<io.h>`、`_WIN32` 所假定的 Windows CRT、`TargetConditionals.h`）永远是
`c-abi` 这一层的问题，从来不是 `kernel-abi` 那一层的问题。一个把分歧记到
「openkal」头上、而真正的分歧出在 musl 身上的包，是适配错了那根轴 ——
openkal 自己的头文件什么都不 `#include`，不与任何平台 SDK 冲突；会和宿主
SDK 的声明相冲突的，是 musl 的头文件（#662）。

**平台依赖是合法的，但必须来自依赖图，并且只对声明它的那个包私有。** 一个
绑定到某个平台的包 —— 它需要该平台的头文件或导入库才能实现某项功能 ——
应在 `[feature-deps.<feature>]` 下用 `visibility = "private"` 依赖该平台
SDK，使这个依赖只到达它自己的翻译单元，永远不会到达消费方。
[06 —— Feature 与能力](06-features-and-capabilities.md#平台-sdk-依赖保持私有)
陈述了这个模式与对应的清单写法。不合法的是转而依赖宿主上恰好装着的那一份 ——
这正是 #662 所关闭的头文件隔离缺口要堵住的东西：一个平台依赖若不在依赖图中，
应当是一次构建失败，而不是被静默换成别的东西。

**一个镜像只能有一个 C 运行时和一个 C++ 运行时。** 「platform-bound」指的是
平台的操作系统 API 表面，不是它的 C 库。一个 platform-bound 的包可以调用
Win32、WinSock 或 Cocoa —— 这些是带 C 接口的系统库 —— 前提是跨越这道边界的
只有句柄与普通值。它不能链接一个按 ucrt、msvcrt、libSystem 或 glibc 编译的
静态库，也不能让 CRT 所拥有的对象跨越这道边界：一个 `FILE*`、在一侧
`malloc` 而在另一侧 `free` 的内存、`errno`、locale 状态。一个只以针对平台
CRT 编译的静态库形式分发的厂商 SDK，在一个 openkal target 上按设计就是
`n/a`，这不是一处遗漏。

**在真正存在差异的那一层上适配。** 跑在 Linux 上的 musl 与跑在 openkal 上
的 musl，共享头文件与 CRT 的形状，因此一处头文件或 CRT 差异是一个 `c-abi`
问题：`cfg(c-abi = "musl")`。但两者**不**共享同一套设施 —— openkal 上没有
epoll，没有信号处理器，`chmod` 只能改动可执行位 —— 因此一处设施差异，首先
应由包**自己的** feature 开关回答，只要存在这样一个开关（比如某个事件循环
后端的选择），只有在一份描述文件需要自动做选择时，才退回到一个组合谓词：
`cfg(all(kernel-abi = "openkal", c-abi = "musl"))`。两种形式都不会进入包的
源码：一个 `cfg` 谓词是在依赖解析时于若干描述文件条目之间做的选择，不是一个
翻译单元能够测试的宏。

**源码不得侦测当前具体是哪一份实现。** 一处 `kal_*` 调用点不会去问自己此刻
跑在 `openkal-linux` 还是 `openkal-macos` 之上；一处 musl 调用点不会去问
自己下面是真正的 Linux 还是 openkal。实现只在依赖解析时被选择一次，而这一
选择之上的一切代码，读到的都只是一个接口。

## 参考

[docs/22 —— 目标侧](22-target-side.md) 给出五个层、四种来源，以及相应的
规则。[docs/06 —— Feature 与能力](06-features-and-capabilities.md) 给出
`[feature-deps.<name>]` 与依赖私有可见性。[SPEC-002](../specs/target-side.md)
给出能力语法的规范性陈述。
