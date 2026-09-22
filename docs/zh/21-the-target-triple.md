# 21 —— 目标三元组

**读者：** 正在为另一台机器构建的人。

**本章回答的那一个问题：** 目标如何命名，哪些目标受支持、各处于什么档位，
以及哪些构建机能服务每一个目标。

**不在这里：** manifest 如何以目标为条件，那是
[22 —— 目标侧](22-target-side.md)；没有操作系统的目标，那是
[40 —— 裸机](40-baremetal.md)。

目标三元组写作 `<arch>-<os>` 或 `<arch>-<os>-<env>`。本章说明每一段的含义、
第三段何时可以省略，以及为什么这个答案在 mcpp 同时支持的两套体系之间并不
相同。

## 两套体系，同一套拼写

mcpp 用两种方式之一解析目标侧 —— 平台接口、C 库、编译器运行时与 C++
运行时 —— 而一个工程通常落在其中之一，且并不需要显式选择。

**预构建体系。** 一份工具链 payload 为一个目标构建，并随身携带那个目标的
C 库。选中 `x86_64-linux-musl` 就是选中 musl-gcc 的 payload，选中
`x86_64-linux-gnu` 就是选中一份 glibc 的 payload。三元组的第三段在解析期
是承重的，因为它正是挑选 payload 的方式。

**构建期体系。** 目标侧以包的形式出现在依赖图中，由正在运行的那个编译器
从源码编译。第三段不选中任何东西，因为图已经做出了决定。这正是
[第 15 章](24-openkal-cross.md)所描述的体系。

两者的差别在于第三段**做什么**，而不在于怎么拼。工程不声明自己属于哪
一种，由依赖图决定，构建随后报告它解析出了什么。

## 各段

| 段 | 内容 | 例 |
|---|---|---|
| `arch` | 指令集 | `x86_64`、`aarch64`、`riscv64`、`wasm32` |
| `os` | 操作系统，或 `none` | `linux`、`windows`、`macos`、`ios`、`emscripten`、`none` |
| `env` | 见下文 —— 它在每个平台上是不同的一根轴 | `gnu`、`musl`、`msvc`、`android`、`elf` |

第三段值得留意，因为它在不同平台上命名的并不是同一类东西：

| 平台 | `env` 命名的对象 | 取值 |
|---|---|---|
| `linux` | **C 库** | `gnu`（glibc）、`musl`、`android`（bionic） |
| `windows` | **对象 ABI** | `gnu`（Itanium C++ ABI）、`msvc`（微软的） |
| `none` | **对象格式** | `elf` |
| `ios` | **真机还是模拟器** | `sim`（模拟器）、缺省（真机） |
| `macos`、`emscripten` | 什么都不命名；该平台不带这一段 | — |

`sim` 是 Apple 这一侧唯一带段的行，而它命名的既不是 C 库也不是 ABI：模拟器
构建有自己的 SDK（`iPhoneSimulator.sdk`），产出自己的对象，取
`-mios-simulator-version-min`，而真机取 `-miphoneos-version-min`。两个
目标，因此两个身份。Apple 把它拼成 OS 段末尾的 `-simulator`，Rust 拼成
`aarch64-apple-ios-sim`；两种拼法在这里都能解析，并且都规范化为
`aarch64-ios-sim`。

`android` 是一个 **C 库**，因此它落在 `musl` 所在的位置上，OS 段仍是
`linux`。这个安放方式就是这处建模决定的全部：内核**就是** Linux，所以
ELF、`unix` family、`nasm -f elf64` 都已经是对的；而一个 `os = "android"`
会让这三者默认全部出错，并要求在每一处站点都给出一个新答案。它与 `gnu`
的差别是 bionic、加载器路径与 SDK —— 这正是 `env` 这一段存在的意义。

在 Windows 上这一段经常被读错，因为 `gnu` 这个词暗示了一个并不在场的
C 库。对一份按构建期体系为 `x86_64-windows-gnu` 构建的产物实测：

| 观测 | 值 |
|---|---|
| 导入的库 | `ntdll`、`KERNEL32`、`SHELL32` —— 没有 `msvcrt`，没有 `ucrtbase` |
| Itanium 修饰符号（`_Z…`） | 4507 |
| MSVC 修饰符号（`?…`） | 0 |

没有任何 GNU 的东西在场：编译器是 clang，链接器是 lld，编译器运行时是
compiler-rt，C 库是 musl，C++ 运行时是 libc++，平台是 openkal。`gnu` 是
LLVM 词表里「非 MSVC 那套 ABI」的标签，继承自 MinGW，而 clang 需要这个
拼法来选中正确的内部工具链。mcpp 无法为它改名。

### 对象格式是一根轴，不是一次推导

三元组的二进制格式过去根本不是独立存在的东西：它在每一处需要它的地方从
`os` 重新推导一遍。`is_pe()` 问一遍 `os == "windows"`，产物命名再问一遍，
打包器问第三遍。当答案只有两个取值时，这样做还负担得起。

`wasm32` 是 mcpp 词表里第一个格式不属于这两者之一的目标，而第三个取值会
把这些推导变成**每一处这样的站点都要新增一次判断**——而漏掉的那一处
不会报错，它会静默地答成 ELF，因为 ELF 正是这棵树里每一个 `else` 分支
所假设的东西。于是格式现在是一个单一答案：

| 目标 | 格式 |
|---|---|
| `x86_64-linux-gnu`、`aarch64-linux-android`、`riscv64-none-elf` | ELF |
| `aarch64-macos`、`aarch64-ios` | Mach-O |
| `x86_64-windows-gnu`、`x86_64-windows-msvc` | PE |
| `wasm32-emscripten` | wasm |

它与「有没有一个操作系统可供链接」**不是**同一个问题。一个裸机 RISC-V
镜像是 ELF 且没有 OS；一个 wasm 模块有一层类 OS 的东西（Emscripten 的
POSIX 模拟层）却不是 ELF。把这两根轴合并成一根，正是这处改动所要
取代的错误。

## wasm 产物契约

`artifact_naming` 在每一行上说的是同一句话：可执行文件是**runner 执行
的那个文件**，按该行自己的约定命名。在 `wasm32-emscripten` 上，那个
文件是 JavaScript 启动器 —— payload 的 `node` 运行的、浏览器加载的正是
它 —— 所以这一行对它的约定是 `bin/<name>.js`，而不是 mcpp 2026.9.12.3
之前那种裸的、借用宿主约定的名字。面向 Emscripten 的两套权威构建系统
出于同一个原因，各自钉死了相同的后缀：Emscripten 自己的 CMake 工具链
设置 `CMAKE_EXECUTABLE_SUFFIX ".js"`，Rust 的 `wasm32-unknown-emscripten`
target spec 设置 `exe_suffix: ".js"`。

| kind | 文件 | 说明 |
|---|---|---|
| `bin`、`app` | `bin/<name>.js` | `bin/<name>.wasm` 是同一条链接边的隐式输出，随它一起暂存。emcc 用同一个词干写出的其余文件（`--preload-file` 产生的 `<name>.data`、`<name>.worker.js`、`<name>.wasm.map`）恰好在链接产出它们时一并出现 |
| `lib` | `lib/lib<name>.a` | — |
| `shared` | 被拒绝，点名 `-sSIDE_MODULE` | wasm 的 side module 需要一种 mcpp 不渲染的链接契约 |

`--no-entry` 不是 mcpp 解释的开关，它是一条普通的
`[target.'cfg(os = "emscripten")'.build] ldflags` 条目，`main` 仍然像
在每一行上一样只是指名一个翻译单元 —— 因此一个模块化的 Web 程序，如果
它的页面调用一个导出的工厂函数，就是一个 `main` 文件不定义 `main()` 的
`bin` 目标，在自己的 `ldflags` 行上带 `--no-entry` 链接。

执行 `bin/<name>.js` 的 runner 来自 payload 描述符
（`.mcpp-toolchain.json`）、工程的 `[target.<triple>] runner`，或者
某个依赖的 `mcpp::runner(...)`——与每一行相同的解析顺序。一个 `.js`
文件不带 shebang，因此一份在它的描述符获得 `runner` 字段之前就已装好
的 emsdk payload，需要先经过[第 20 章](20-toolchains.md)所述的那次索引
刷新，`mcpp run` 才能找到 runner。

## 省略第三段

`<arch>-<os>` 在每个平台上都是一个完整的目标：

```bash
mcpp build --target x86_64-linux      # = x86_64-linux-gnu
mcpp build --target x86_64-windows    # = x86_64-windows-gnu
mcpp build --target riscv64-none      # = riscv64-none-elf
mcpp build --target aarch64-linux     # = aarch64-linux-musl
mcpp build --target aarch64-macos     # macOS has no segment to decline
```

省略它不改变任何身份。输出目录、缓存键，以及 `cfg()` 谓词的对象，都取规范
形式，因此两种拼法共用同一个指纹，第二次构建是缓存命中，而不是又一次
完整构建。

不同的是**请求被记录成了什么**。三元组既充当身份 —— 身份必须完整 ——
又充当请求 —— 请求必须能够什么都不说；mcpp 两者都保留：为身份填上那一
段，同时记住这次填充只是一次填充。

### 补全取自词汇表，而不是取自一个固定的词

上面第四行正是这两种角色必须分开的理由。把 `aarch64-linux` 按词法填成
`aarch64-linux-gnu`，而那一行是 `planned`——`aarch64-linux-musl` 才是
`verified`。2026.8.26.2 之前，档位闸问的是填充之后的值：

```
$ mcpp build --target aarch64-linux
  error: target 'aarch64-linux-gnu' is registered but not yet supported (planned)
$ mcpp build --target aarch64-linux-musl
  Finished dev [unoptimized + debuginfo] in 0.99s
```

被问的问题是「aarch64，Linux」。被回答的问题却是「aarch64-linux-**gnu**」，
而报错引用的三元组在那条命令里根本不存在。`riscv64-linux` 更严重：填充
出来的那一行完全不在词汇表里，于是一个已登记的目标族被报成
`unknown target`。

省略了这一段的请求，按下列顺序对着已知目标表补全：

1. 词法默认值命中一个受支持的行 —— 采用它（`x86_64-linux` → `gnu`）；
2. 该 `(arch, os)` 下恰好一个受支持的行 —— 采用它（`aarch64-linux` →
   `musl`）；
3. 一个受支持的都没有 —— 保留词法形式，并对着**确实存在**的那些行给出
   诊断（`riscv64-linux` → 「planned；该系统已登记的行：
   `riscv64-linux-musl`」）；
4. 多个受支持而词法默认值不在其中 —— 拒绝并列出候选。今天没有任何
   `(arch, os)` 呈这个形状。

规则 1 排在最前，使这条规则能自己退休：`aarch64-linux-gnu` 从
`planned` 升级的那一天，词法答案重新胜出，不需要任何人回来修改什么。

**写出这一段即是退出补全。** 写出来的段是一次请求而不是一处空缺，因此
`--target aarch64-linux-gnu` 仍会撞上 `planned` 行的拒绝 —— 那正是用
显式的 `[target.<triple>] toolchain` 提前加入某一行的逃生口。

### 采用的拼法

**在构建期体系下，省略它。** 图供给 C 库与各运行时，因此那一段陈述的是
一个不会被查询的请求。在这套体系下，`x86_64-windows` 不只是比
`x86_64-windows-gnu` 更短 —— 它更准确，因为其中并没有任何 GNU 的东西
参与。

**在预构建体系下，当这个选择有意义时写出它。** `x86_64-linux-musl` 与
`x86_64-linux-gnu` 选中不同的 payload，产出不同的产物。写出那一段正是
做出这个选择的方式。

**在 Windows 上，想要微软那套 ABI 时写 `msvc`。** `gnu` 是默认填充值，
而 `msvc` 是不同的对象 ABI 而非不同的 C 库，因此那一段在两套体系下都
有意义。

## 构建报告

报告以写下的目标为标题，并把它解析成编译器自己的拼法：

```
      Target x86_64-windows → x86_64-w64-windows-gnu
             kernel-abi        openkal        (openkal-windows@0.1.3, graph)
             c-abi             musl           (openkal-musl@0.3.3, graph)
             c++-abi           libc++         (openkal-llvm-runtime@0.1.2, graph)
```

有两条诊断挂在第三段上，适用哪一条由上面那张表决定。

**当该段命名一个 C 库，而图供给的是另一个时，构建报出这个分歧。** 以图
为准，所以这是一次报告而不是一次拒绝 —— 两种写法下产物相同，不准确的
只是名字：

```
warning: the target name asks for the `gnu` C ABI and the dependency graph supplies `musl`.
       The graph decides, so the build below uses `musl` — the name is what is inaccurate,
       not the artifact. Drop the segment to say what is actually meant:
           --target x86_64-linux
```

**当该段命名的是别的东西时，报告说出那是什么。** 在那种情形下发警告
是错的：它会在每一次合法的 MinGW 构建上触发，并且描述了一根名字从未
涉及的轴。这条提示只在使用者写出了那一段时才出现，并且命名的是 **ABI
本身**而不是任何一层：

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu   (gnu selects the Itanium C++ ABI, not a C library)
```

它**不对应报告里的任何一行**，这正是要点所在。五层记录的是每一层
**由谁供给**；而那一段命名的是这些**对象所遵循的约定**，是若干层必须
彼此一致的事项。把它读成 `c++-abi libc++` 是第二个错误答案，因为
libstdc++ 处在同一套 ABI 之上。

## 三套词汇表，以及它们不同的原因

一个三元组由三方书写，而三方并不共用一套约定；mcpp 在它们之间翻译。
弄清手上这个字符串属于哪一套，第三段带来的困惑就消掉了大半。

### 形状

```
<arch> - <vendor> - <os> - <env>
```

每一段都可以省略，缺失的部分由编译器补齐。`vendor` 是历史遗留，今天
几乎不承载任何信息 —— 除非目标有理由另作声明，否则一律填成
`unknown`：

```
x86_64-linux-gnu   →  x86_64-unknown-linux-gnu
aarch64-macos      →  aarch64-unknown-macos
```

三段式的拼法是四段式省掉一段之后的样子，省掉的是哪一段，由「能不能
解析」决定。

### GCC 与 clang 选目标的方式并不相同

| | GCC | clang |
|---|---|---|
| 目标 | 在编译器**构建**时就已定死 | 在**运行时**选定 |
| 怎么问 | `-dumpmachine` | `-dumpmachine`，或 `--target=` |
| 如何交叉编译 | 换一个**不同的可执行文件** | 传一个 flag |

实测：

```
g++                     →  x86_64-linux-gnu       (emits nothing else)
x86_64-w64-mingw32-g++  →  x86_64-w64-mingw32     (emits nothing else)
clang++                 →  x86_64-unknown-linux-gnu, and --target= changes it
```

这正是预构建体系**不传 `--target`** 的原因：那份 payload 的编译器以它
唯一能发出的那个目标命名，选目标就等于选 payload。这也是构建期体系
只需要一个编译器的原因。

### MinGW 按 GCC 的约定给自己命名

MinGW 自己的三元组是 `x86_64-w64-mingw32`：

| 段 | 值 | 原因 |
|---|---|---|
| arch | `x86_64` | |
| vendor | `w64` | 项目名是 `mingw-w64`，用以区别于已停滞的原始 `mingw32` 项目 |
| os | **`mingw32`** | MinGW 把**自己**放在 OS 段 |
| env | （缺席） | 三段就是全部名字 |

这套约定源自 autoconf 的 `config.guess`，其中 OS 段命名的是目标的运行
环境 —— 而在 GNU 工具链的世界观里，MinGW **就是**一个独立的环境：有
自己的头文件、自己的 C 运行时，也有自己的 `configure` 分支。名字里的
`32` 是历史遗留；`w64` 才表明这个名字指的是那个支持 64 位的项目。

LLVM 不接受这个世界观。它认定 OS 是 `windows`，MinGW 只是架在其上的
一种 ABI 环境，因此把这个名字重新拼过 —— 实测：

```
x86_64-w64-mingw32  →  x86_64-w64-windows-gnu
x86_64-pc-mingw32   →  x86_64-pc-windows-gnu
```


```
GCC   x86_64 - w64     - mingw32 - (none)
                ^vendor   ^os
LLVM  x86_64 - unknown - windows - gnu
                ^vendor   ^os       ^env
```

**`mingw32` 从 OS 段被拆成 `windows` 加 `gnu` 两段。** `gnu` 这个取值
之所以存在，正是因为 LLVM 需要给拆分出来剩下的那一半起一个名字。它的
含义是「MinGW/Itanium 这一支 ABI」，在 Windows 上从来不意味着「C 库是
glibc」—— 同一个词在不同操作系统下承担不同的职责，这是 LLVM 词汇表
既有的事实，不是 mcpp 的发明。

### mcpp 保留的那一套

| 词汇表 | 例 | 读者 |
|---|---|---|
| GCC / autoconf | `x86_64-w64-mingw32` | 预构建 payload 的编译器，以文件名的形式读取 |
| LLVM | `x86_64-w64-windows-gnu` | `clang --target=` |
| **mcpp** | `x86_64-windows-gnu` | 目标表、输出目录、`cfg()`、打包出的 ABI tag |

mcpp 自己那套拼法必须能映射到另外两套上。构建报告里的那个箭头就是这个
映射，从第三套词汇表映到第二套：

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu
       ^ mcpp                ^ LLVM
```

让第三套词汇表保持独立，正是 mcpp 能够命名 LLVM 命名不了的东西的原因。
在 llvm 22.1.8 上实测：`windows` 配一个 `musl` 环境能被三元组解析器
接受，却会让编译器崩溃：

```
clang++ --target=x86_64-pc-windows-musl -c t.cpp
    #5  llvm::MCWinCOFFStreamer::emitCGProfileEntry(...)
```

崩溃发生在 **COFF 写出器**里：clang 已经认定输出是 COFF，却没有一条
Windows-musl 的路径去把 streamer 配置好。它认识的四个非 MSVC 的
Windows 环境 —— `gnu`、`cygnus`、`itanium`、`musl` —— 前三个都能编译，
只有 `musl` 会崩溃，而预定义宏说明了这个环境为什么从未被建模：

| 三元组 | 宏 |
|---|---|
| `…-windows-gnu` | `__GNUC__` `__MINGW32__` `_WIN32` |
| `…-windows-msvc` | `_MSC_VER` `_WIN32` |
| `…-windows-musl` | `__GNUC__` `_WIN32` —— **没有 `__MINGW32__`** |

所以 Windows 上的 musl C 库无法向 clang 命名，却仍然必须由 mcpp 命名，
因为 mcpp 的名字回答的是另一个问题 —— **哪一个 C 库**，而 LLVM 的名字
回答的是**哪一套对象 ABI**。两者都是必需的，而且不是同一个字符串。

## 编译器与 C 库是两根轴

目标命名的是一台机器。它不命名谁来为它编译，也不命名它的 C 库从哪里
来 —— 那是另外两个选择，同一个目标字符串在每种选择下对应不同的构建。

**「来自 payload」并不是某一份固定的 payload**，而是所选编译器带来的
那一份；gcc 与 clang 带来它的方式并不相同：gcc 一个目标一份 payload，
驱动带三元组前缀，而一个 `clang++` 就能发出它构建时支持的每一个目标。
在同一台宿主、同一份源码上实测：

| toolchain | target | driver that ran | c-abi | c++-abi |
|---|---|---|---|---|
| `gcc@16.1.0` | `x86_64-linux-musl` | `xim-x-musl-gcc/…/x86_64-linux-musl-g++` | musl | libstdc++ |
| `gcc@16.1.0` | `x86_64-windows-gnu` | `xim-x-mingw-cross-gcc/…/x86_64-w64-mingw32-g++` | gnu | libstdc++ |
| `llvm@22.1.8` | `x86_64-linux-musl` | `xim-x-llvm/…/clang++` | musl | libc++ |
| `llvm@22.1.8` | `x86_64-windows-gnu` | `xim-x-llvm/…/clang++` | gnu | libc++ |

clang 不会伸进 gcc 的 payload 里取 C 库，gcc 也不会伸进 clang 的。
各带各的。

### 换另一个编译器，就要一并供给另一份 C 库

每一个 hosted 行都写着一个工具链，而这个名字是**约定**而不是**能力**：
它回答的是*哪一份 payload 供给这个目标的 C 库*，因此一个自己供给 C 库
的工程可以写另一个编译器。不能做的是写另一个编译器却什么都不供给。

```toml
[toolchain]
default = "llvm@22.1.8"        # x86_64-linux-musl's row names gcc
```

```
$ mcpp build --target x86_64-linux-musl
error: target 'x86_64-linux-musl' takes its C library from the 'gcc@16.1.0'
       payload, and 'llvm@22.1.8' has none here.
```

**2026.8.26.1 之前，这会把整个构建跑完，才在链接阶段失败**，报出
`crtbeginT.o (bare name — the linker cannot resolve it)`——对症状的描述
准确，却对背后的决定只字不提。clang 是可重定向的，自己不带 C 库，于是
去够一份 gcc 安装；在一台恰好装有系统 mingw 的机器上，同样的写法用于
`x86_64-windows-gnu` 时，够到的是 `/usr/lib/gcc/x86_64-w64-mingw32/…`，
这比直接失败更糟。

补上替代者，就是全部的差别：

```toml
[dependencies]
openkal-llvm-runtime = "0.1.3"   # → openkal-musl → openkal-<os>
[toolchain]
default = "llvm@22.1.8"
```

这就是 [`examples/06-openkal-cross`](../../examples/06-openkal-cross)，也是
那条拒绝会在文本里点名 openkal 的原因。

**裸机某一行的工具链不是约定，`x86_64-windows-musl` 那一行的也不是**——
没有任何 gcc payload 能发出带 musl C 库的 PE，因此这些行完全无法被
覆盖。见[第 03 章](20-toolchains.md)。

### 而依赖图会把这根轴整个替换掉

同样三个目标，图里带有 `openkal-musl` 与 `openkal-llvm-runtime` —— 按
同样的方式实测：

| target | kernel-abi | c-abi | c++-abi |
|---|---|---|---|
| `x86_64-linux-musl` | openkal（openkal-linux，图） | musl（图） | libc++（图） |
| `x86_64-windows-gnu` | openkal（openkal-windows，图） | musl（图） | libc++（图） |
| `x86_64-windows-musl` | openkal（openkal-windows，图） | musl（图） | libc++（图） |

**对照两张表里的 `x86_64-windows-gnu`。** payload 供给时它的 C 库是
`gnu`—— 也就是 MinGW 的 CRT；图供给时是 `musl`。同一个目标字符串，两个
不同的 C 库，而直到 2026.8.24.6，mcpp 都没有办法说清是哪一个：同一条
`--target x86_64-windows-gnu` 产出的产物体积相差 16.7 倍，依赖的 DLL
也完全不同。

这正是 `x86_64-windows-musl` 作为独立名字存在的理由。它映射到与
`x86_64-windows-gnu` **相同的 LLVM 三元组**——LLVM 拼不出它——因此两者
在编译器眼里无法区分，全部的差别就在于用的是哪个 C 库。没有任何宿主
为它准备了 payload；它的 system 只能来自依赖图，这正是 `toolchain list`
所报告的 `via dependency graph`。

### 声明的环境能移动编译期三元组，移动不了链接期的那一个（mcpp 2026.9.18+）

一个 `c-abi` 包的 `[c-abi]` 块（[22 —— C 环境](22-target-side.md#c-abi-包陈述它呈现的-c-环境mcpp-2026918)）
可以改变一次**编译**收到的 `--target=`——当图里的 C 库声明
`presents = "posix"` 时，`x86_64-windows-gnu` 会按 `x86_64-pc-cygwin`
编译——但不会改变本章其余部分所讲的那个三元组。解析出的三元组
（`mcpp toolchain list` 打印的拼法、输出目录名、`Target` 报告的标题行、
LINK 行）保持不变，仍是图与工具链解析出的那个；被替换的只是编译命令上
编译器自己的 `--target=` 记号，因为声明的环境身份宏与数据模型正是从
这里获得。如果有读者在 `compile_commands.json` 里 grep `--target=`，
找到一个本章从未列出的拼法，看到的正是这件事——触发条件与原因见
docs/22。

## 构建机是第三根轴

上面两根轴 —— 用哪个编译器、C 库从哪里来 —— 是工程做出的选择。第三根
不是：它是构建实际运行的那台机器。

**这根轴是 mcpp 自己发布的那一组目标，而且是 (os, arch)，不是 os。**
`release.yml` 发布四份宿主二进制：

| build host | release asset | CI runner |
|---|---|---|
| `linux-x86_64` | `mcpp-<v>-linux-x86_64.tar.gz` | `ubuntu-24.04` |
| `linux-aarch64` | `mcpp-<v>-linux-aarch64.tar.gz` | `ubuntu-24.04-arm` |
| `macos-arm64` | `mcpp-<v>-macosx-arm64.tar.gz` | `macos-14` |
| `windows-x86_64` | `mcpp-<v>-windows-x86_64.zip` | `windows-2022` |

**两台 Linux 宿主不是同一台。** `x86_64-linux-gnu` 需要本机架构的
`xim:glibc` 与 `xim:linux-headers` payload，而它们只为宿主自己的架构
存在 —— 因此那一行从 `linux-x86_64` 够得着，从 `linux-aarch64` 却够不
着；`aarch64-linux-gnu` 是镜像的情形，在两台上都是 `planned`。把它们
合并成 `linux`，会让一台的行覆盖掉另一台的行。

### 构建机与它们服务的目标

| target | tier | pin | linux-x86_64 | linux-aarch64 | macos-arm64 | windows-x86_64 |
|---|---|---|---|---|---|---|
| `x86_64-linux-gnu` | verified | — | 载荷 | — | — | — |
| `aarch64-linux-gnu` | planned | — | planned | planned | planned | planned |
| `x86_64-linux-musl` | verified | `gcc@16.1.0` | 载荷 | 载荷 | — | 载荷 |
| `aarch64-linux-musl` | verified | `gcc@16.1.0` | 载荷 | 载荷 | — | — |
| `riscv64-linux-musl` | planned | — | planned | planned | planned | planned |
| `x86_64-windows-gnu` | verified | `gcc@16.1.0` | 载荷 | 载荷 | — | 载荷 |
| `x86_64-windows-musl` | preview | `llvm@22.1.8` | 图 | 图 | 图 | 载荷 |
| `x86_64-windows-msvc` | verified | — | — | — | — | 系统 |
| `aarch64-macos` | verified | — | — | — | SDK | — |
| `x86_64-macos` | planned | — | planned | planned | planned | planned |
| `riscv64-none-elf` | verified | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `riscv32-none-elf` | verified | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `aarch64-none-elf` | preview | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `x86_64-none-elf` | preview | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `thumbv6m-none-eabi` | verified | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `thumbv7m-none-eabi` | verified | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `thumbv7em-none-eabi` | preview | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `thumbv7em-none-eabihf` | verified | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `thumbv8m.base-none-eabi` | preview | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `thumbv8m.main-none-eabi` | verified | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `thumbv8m.main-none-eabihf` | preview | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `armv7a-none-eabi` | verified | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `armv7a-none-eabihf` | verified | `llvm@22.1.8` | 载荷 | 载荷 | 载荷 | 载荷 |
| `aarch64-linux-android` | verified | `android-ndk@30.0.16248370` | 载荷 | 载荷 | 载荷 | — |
| `x86_64-linux-android` | verified | `android-ndk@30.0.16248370` | 载荷 | 载荷 | 载荷 | — |
| `aarch64-ios` | preview | `llvm@22.1.8` | — | — | SDK | — |
| `aarch64-ios-sim` | verified | `llvm@22.1.8` | — | — | SDK | — |
| `x86_64-ios-sim` | preview | `llvm@22.1.8` | — | — | SDK | — |
| `wasm32-emscripten` | verified | `emsdk@6.0.9` | 载荷 | 载荷 | 载荷 | 载荷 |

`载荷` 这里有工具链 payload 产出它 · `图` 没有 payload，但依赖能供给
system · `系统` 在机器上被找到，不是 mcpp 安装的 · `SDK` 平台自己的 ·
`—` 从这台宿主够不着 · `planned` 词汇表里已登记，还没有任何东西接线。

### 各列背后的规则

| 目标类别 | 服务它的构建机 | 原因 |
|---|---|---|
| `*-linux-musl` | Linux（任意架构）、Windows（仅同架构） | musl 的 payload 是自包含的 |
| `*-linux-gnu` | Linux，且仅同架构 | 还需要本机架构的 `xim:glibc` / `xim:linux-headers` |
| `x86_64-windows-gnu` | Linux、Windows | 一个身份，只在分发层按宿主拆分 |
| `x86_64-windows-msvc` | Windows | MSVC 是在机器上被找到的 |
| `x86_64-windows-musl` | payload 只在 Windows 上；图则任意宿主 | 没有 gcc 能发出 PE + musl，且 LLVM 拼不出这个三元组 |
| `aarch64-macos` | macOS | SDK 是那台机器自己的 |
| `*-none-elf` | 每一台宿主 | clang 与 lld 按构造就是交叉编译器 |
| `wasm32-emscripten`、`*-linux-android` | 每一台宿主 | SDK 自带 sysroot，且上游按宿主分别发布；一份归档服务每个 guest 架构 |

**一个 `—` 讲的是 payload，不是可能性。** `host_can_serve` 回答的是
「这里有没有 payload 能产出它」，而依赖图可以改为供给 system —— 这正是
`x86_64-windows-musl` 在 Linux 上显示 `via dependency graph`、并在那
里产出一个真正的 PE32+ 的原因。

**而两个 Android 行在 Windows 上的 `—`，是索引的答案，因此
`mcpp toolchain list` 在那里仍会显示它们。** Google 确实发布了 Windows
版的 NDK，也下载得到；它不包含的是 libc++ 的模块面（实测：没有
`std.cppm`，也没有 `std/*.inc`，而另外两台宿主各有 110 个），因此
`xim:android-ndk` 不声明 Windows 表 —— 一条永远无法服务「模块优先」
构建的条目，比没有这条目更糟。引擎不把这件事编码进去：一个索引服务
哪些宿主，会在没有引擎发布的情况下变化，而把它写成这里的一个常量，
正是 wasm 那一行自己的历史所展示的、会变陈旧的东西。因此 Windows 用户
会看到这一行、pin 也能解析，而 xim 在任何东西被下载之前，就以
`no payload for this platform` 拒绝，并点名那个包。

**两个 Android 行都是 `verified`，但用的是不同的载体。** x86_64 的产物
在平台自己的模拟器上执行。真机那一行的产物在 qemu-user 上、配系统镜像
自带的 bionic 运行 —— 这是从一台 x86_64 宿主能走通的路线，因为平台
模拟器会直接拒绝异构 guest（`QEMU2 emulator does not support arm64 CPU
architecture`）。一个档位断言的是产物**被构建并运行**过；它不断言是
哪个模拟器运行的。

在这两行上，`kind = "app"` 链接成一个共享库而不是可执行文件（见
[04 §2.2](04-mcpp-toml.md)）——由 `application_form` 对
`env == "android"` 给出这个形态，与 `is_android()` 所用的是同一个判据。

### 而 CI 把每一个都测过

[`ci-target-matrix.yml`](../../.github/workflows/ci-target-matrix.yml) 在
全部四台宿主上运行。每一台把它列出的每一行都扫描两遍 —— 只有 payload
一遍，图里加上 `openkal-musl` + `openkal-llvm-runtime` 再一遍 —— 并与
[`tests/matrix/expected.tsv`](../../tests/matrix/expected.tsv) 比对，
后者以 `(mode, host, target, compiler)` 为键。

一台 runner 把什么解析成自己的目标，和它的名字给人的印象并不一致：

| runner | 它解析出的宿主目标 |
|---|---|
| `ubuntu-24.04` | `x86_64-unknown-linux-gnu` |
| `ubuntu-24.04-arm` | `aarch64-unknown-linux-gnu` |
| `macos-14` | `arm64-apple-darwin23.6.0` —— **ARM**，不是 x86_64 |
| `windows-2022` | `x86_64-pc-windows-msvc` —— **msvc**，而那里的 mingw gcc 目标是 `-gnu` |

本章有三条判据都曾假设了这个只在 Linux 上成立的巧合，因而必须在其他
宿主上被纠正。

**每台宿主的格数不是一个常数。** 它取决于那台机器装了什么，而同一台
runner 在相邻两次运行中曾被测到装有不同的工具链。因此比对断言的是
**扫描确实产出了行**，以及**期望表点名的每一行都被跑到过**，而不是一个
总数：一格因为 payload 没被恢复而消失，和一格通过了，二者的读数无从
区分。

## mcpp 定义的宏

`src/toolchain/predefines.cppm` 同时是这张表的规范与实现：契约是那个
模块里的数据，发出宏的是它旁边的一个函数，而
`tests/unit/test_predefines.cpp` 双向断言两者一致。一个被发出却不在
表里的宏，或者一个在表里却从未被发出的宏，都会让构建失败，而不是漂移
进一次发布。

| 宏 | 何时定义 | 是否 mcpp 拥有 |
|---|---|---|
| `__MCPP_TARGET_<OS>__` | 每次构建一个，拼法取自三元组的 `os` 字段 | 是 |
| `__OPENKAL__` | 解析出的 `kernel-abi` 层接口名是 `openkal` | 是 |
| `__unix__` | `[c-abi]` 的实现在工具链不会定义它的地方供给它 | 否 |

**引擎不应该定义宏，而每一行都必须证明自己是正当的。** 包在它的
manifest 里陈述需要什么，引擎以**解析**作答 —— `cfg(os = "windows")`、
`cfg(c-abi = "musl")`、一项能力、一个 feature。那条路径可测试、可报告、
读一遍 manifest 就能看见；宏三样都不是。目前有两条理由站住了脚：

1. **源码不归我们编辑**，而它在预处理期发问。上游 C 用 `#if` 选择平台
   行为，没有任何 manifest 键能伸进第三方的 `.c` 里。
2. **读者是一份已安装的头文件。** 包自己的构建 define 可以写在它的
   manifest 里，但它**安装出去**的头文件，会被应用程序自己的编译读到，
   那些 define 永远够不到那里。

**`__MCPP_TARGET_<OS>__`——它的规则。** 为每一个目标侧的翻译单元定义，
始终如此，每次构建一个。拼法取自三元组自己的 `os` 字段并转为大写：
`x86_64-linux-gnu` 给出 `__MCPP_TARGET_LINUX__`，`x86_64-windows-gnu`
给出 `__MCPP_TARGET_WINDOWS__`，`riscv64-none-elf` 给出
`__MCPP_TARGET_NONE__`。引擎不学习任何操作系统的名字：三元组解析器
新增一个目标，它对应的宏就随之存在，引擎不需要任何改动。

*允许：* 在上层呈现的 C 环境已经压掉平台自己的宏时，用它得知目标的
操作系统；以及按目标的 ABI 为一份记录定尺寸。一个呈现 POSIX 的
Windows 目标有意没有 `_WIN32`——而调用约定仍然是 Win64，这正是
`openkal-musl` 的 `bits/setjmp.h` 与 `openkal-llvm-runtime` 的
`__libunwind_config.h` 各自据以定尺寸的事实。

*禁止：* 选择一个 manifest 本可以选择的头文件；或者在本项目控制得了的
包里，用它代替 `cfg(os = …)`。

**它总是被定义，不只是在某个东西被压掉的时候。** 条件式地发出会让它的
**缺席**含义不唯一：「不是 Windows」与「是 Windows，只是没有东西压掉
它的宏」会读成同一回事。一个缺席只对应一种含义的宏，才值得花一个
`-D`。

**命名。** 全大写，用 `__` 包裹，单词之间用 `_` 分隔；mcpp 拥有的名字
带 `__MCPP_` 前缀。

业界的约定按**一个名字是什么**划分，而不是按谁写它划分。厂商名或产品
名是大写 —— `__APPLE__`、`_WIN32`、`__MINGW32__`、`__GNUC__`。系统种类
名是小写 —— `__linux__`、`__unix__`、`__gnu_linux__`。mcpp **拥有**的
每一行都属于第一类：它命名的是 mcpp，或者是 openkal。「是哪一种系统」
这个问题由 `__linux__` 那一族回答，而那一族是 mcpp **供给**而非拥有的，
因此保持小写拼法，理由正在于此。

小写拼法先发布了出来，而产生它的那段推理值得留存，因为这个错误很容易
再犯一次：这些名字在真实的守卫里与 `__linux__` 并排出现，因此「与它
一致」看起来就像是一种一致性。**这是把「相邻」与「同类」混为一谈。**
`__APPLE__` 也出现在那些同样的守卫里，却是大写，因为它属于某个人 ——
守卫里凡是属于某个人的名字都是大写。

`__MCPP_` 这个前缀是承重的，但它只是一个**前缀**，不是规则的全部：一个
mcpp 拥有的名字，含义由 mcpp 自己规定，而 `__OPENKAL__` 同样是被拥有
的，它命名的是 *openkal*，而不是 mcpp。另一条路 —— **借用** —— 曾经
试过：`__CYGWIN__` 被保留了定义，为的是让需要「PE 目标文件格式、POSIX
C 环境」的代码有一个名字可用，而一次覆盖 30 个成员的测量发现，有成员
把它读成「Win32 可用」，并因此走向 `#include <windows.h>`——那正是
上游赋予它的含义。**一个借来的名字，含义由出借方的历史决定**，而不是
由借用方的意图决定。

**那个数字曾经是四，实际是二**（2026-09-21 按撤回该名字那一版的重新
测量更正）。`archive`（经由 xz）与 `sqlite3` 确实读它，两者都已清理。
另外两个之所以被归进同一组，是因为四者都停在 `windows.h` 这一步，而它
们的守卫并不相同：`c-ares` 经由 `#ifdef HAVE_WINDOWS_H` 到达那个
头文件，而这个宏是本生态自己的配方在它的 Windows 分支里定义的；
`mimalloc` 则根本不再到达任何头文件 —— 它死在代码生成器里，卡在一个
LLVM 没有为替代三元组的 OS 实现的 builtin 上。**按诊断分组，不等于按
真因分组**，这样统计出来的数字会高估一次撤回真正能修好多少。

`__unix__` 是印证这条规则的例外：mcpp **供给**它而不拥有它，因此它
保持标准拼法，mcpp 也无权改变它的含义。按在任何其他 POSIX 系统上同样
的方式读它即可。

**稳定性。** 这张表里的一条是**已发布的接口**，而撤回一条是**静默**
的：一个 `#if` 会选中另一条分支并照常编译，发生在没有任何人盯着的
机器上。构建工具手里没有任何机制能让这件事变响 —— 预处理器无法被
告知「为一个它找不到的名字报错」。

所以撤回从来不是由**阅读**来决定的，而是由**一次枚举读者的测量**来
决定，测量的结果决定这次撤回要走几步。本项目做过的两次撤回都在记录
上，而结果各不相同：

| withdrawn | readers found | steps |
|---|---|---|
| `__CYGWIN__`（2026.9.21.1） | 四个第三方成员，外加本生态安装出去的两份头文件里的六处 | 三步：发布替代品、迁移消费方、最后停止定义 |
| `__mcpp_target_<os>__`、`__openkal__`（2026.9.21.2） | 零 —— 本生态任何仓库里都没有一个源文件、也没有一份 manifest 读它们 | 一步 |

两次之间规则没有变，变的是数字。第二行之所以便宜，是有原因的，这个
原因值得写出来：两个名字都是在这里发明的，所以不可能有任何上游代码
握着它们；暴露的时间窗口，`__openkal__` 是三天，目标宏是一个发布
周期。**一次不是由数字支撑的撤回论证**，正是本项目已经犯错过一次的
那种论证。

## 自定义目标

不在 mcpp 表内的三元组需要一个显式的 section，这也是一块板子声明
「任何默认值都给不出的事实」的方式：


```toml
[target.riscv64-none-elf]
sysroot = ""
runner  = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
           "-no-reboot", "-bios", "default", "-kernel"]
```

`sysroot = ""` 选定零 libc 档位：编译行上没有 C 库，链接上也没有。
**缺席的 `sysroot` 键是另一个答案**—— 它继承该目标行自己的默认值。见
[第 13 章](40-baremetal.md)。

## 参考

[第 14 章](22-target-side.md)讲五层，以及每一层由谁供给。
[第 15 章](24-openkal-cross.md)完整讲构建期体系。
[第 03 章](20-toolchains.md)讲工具链这根轴，它是分开的：目标不决定
编译器。

## 当前边界

- 处于 `planned` 档位的行，含义是**词汇表里已登记，还没有任何东西
  接线**。点名这样一个目标会被解析器接受、被构建拒绝。
- `host_can_serve` 判断的是「这台机器能不能产出这个目标」，这与档位
  是两个不同的问题。mcpp 支持的目标仍可能从这台宿主够不着；出口是
  显式的 `[target.X] toolchain = "…"`。
