# 目标三元组

目标三元组写作 `<arch>-<os>` 或 `<arch>-<os>-<env>`。本章说明每一段的含义、
第三段何时可以省略,以及为何这个答案在 mcpp 同时支持的两种体系下并不相同。

## 两种体系,同一套拼写

mcpp 解析目标侧 —— 平台接口、C 库、编译期运行时与 C++ 运行时 —— 有两条路径,
而一个工程通常在没有显式选择的情况下就落在其中之一。

**传统预构建体系。** 一份工具链载荷为一个目标而构建,并随身携带那个目标的
C 库。选中 `x86_64-linux-musl` 就是选中 musl-gcc 载荷,选中
`x86_64-linux-gnu` 就是选中一份 glibc 的。三元组的第三段在解析期是承重的,
因为它正是挑选载荷的方式。

**构建期体系。** 目标侧以包的形式出现在依赖图中,由正在运行的那个编译器
从源码构建。第三段不选中任何东西,因为图已经决定了。这是
[第 15 章](15-openkal-cross.md)所描述的体系。

两者的差别在于第三段**做什么**,而不在于它怎么拼。工程不声明自己属于哪一种;
由依赖图决定,构建则报告它解析出了什么。

## 各段

| 段 | 内容 | 例 |
|---|---|---|
| `arch` | 指令集 | `x86_64`、`aarch64`、`riscv64` |
| `os` | 操作系统,或 `none` | `linux`、`windows`、`macos`、`none` |
| `env` | 见下 —— 它在每个平台上是不同的轴 | `gnu`、`musl`、`msvc`、`elf` |

第三段值得留意,因为它在各处命名的并不是同一类东西:

| 平台 | `env` 命名 | 取值 |
|---|---|---|
| `linux` | **C 库** | `gnu`(glibc)、`musl` |
| `windows` | **对象 ABI** | `gnu`(Itanium C++ ABI)、`msvc`(微软的) |
| `none` | **对象格式** | `elf` |
| `macos` | 无;该平台不带这一段 | — |

⭐ 在 Windows 上这一段经常被读错,因为 `gnu` 这个词暗示了一个并不在场的 C 库。
对一份按构建期体系为 `x86_64-windows-gnu` 构建的产物实测:

| 观测 | 值 |
|---|---|
| 导入的库 | `ntdll`、`KERNEL32`、`SHELL32` —— 无 `msvcrt`,无 `ucrtbase` |
| Itanium 修饰符号(`_Z…`) | 4507 |
| MSVC 修饰符号(`?…`) | 0 |

没有任何 GNU 的东西在场:编译器是 clang,链接器是 lld,编译期运行时是
compiler-rt,C 库是 musl,C++ 运行时是 libc++,平台是 openkal。`gnu` 是
LLVM 词表里「非 MSVC 的那套 ABI」的标签,继承自 MinGW,而 clang 需要这个
拼写来选中正确的内部工具链。mcpp 改不了它。

## 省略第三段

`<arch>-<os>` 在每个平台上都是一个完整的目标:

```bash
mcpp build --target x86_64-linux      # = x86_64-linux-gnu
mcpp build --target x86_64-windows    # = x86_64-windows-gnu
mcpp build --target riscv64-none      # = riscv64-none-elf
mcpp build --target aarch64-linux     # = aarch64-linux-musl
mcpp build --target aarch64-macos     # macOS 本来就没有这一段可省
```

省略它不改变任何身份。输出目录、缓存键与 `cfg()` 谓词的主语都取规范形式,
因此两种拼法共用一个指纹,第二次构建是缓存命中而不是又一次完整构建。

不同的是**请求被记录成了什么**。三元组既是身份 —— 身份必须完整 ——
也是请求 —— 请求必须能什么都不说;mcpp 两者都保留:为身份填上那一段,
同时记住这次填充是一次填充。

### 补全取自词表,不取自一个固定的词

上面第四行正是这两种角色必须分开的理由。把 `aarch64-linux` 按词法填成
`aarch64-linux-gnu`,而那一行是 `planned` —— `aarch64-linux-musl` 才是
`verified`。2026.8.26.2 之前,tier 闸问的是填充后的值:

```
$ mcpp build --target aarch64-linux
  error: target 'aarch64-linux-gnu' is registered but not yet supported (planned)
$ mcpp build --target aarch64-linux-musl
  Finished dev [unoptimized + debuginfo] in 0.99s
```

被问的问题是「aarch64 的 Linux」。被回答的问题是「aarch64-linux-**gnu**」,
而报错引用的三元组在那条命令里根本不存在。`riscv64-linux` 更严重:填充产生的
那一行完全不在词表里,于是一个**已登记**的目标族被报成 `unknown target`。

省略了这一段的请求,按下列顺序对着已知目标表补全:

1. 词法默认命中一个受支持的行 —— 用它(`x86_64-linux` → `gnu`);
2. 该 `(arch, os)` 下恰好一个受支持的行 —— 用它(`aarch64-linux` → `musl`);
3. 一个受支持的都没有 —— 保留词法形式,并对着**确实存在**的那些行给出诊断
   (`riscv64-linux` → 「planned;该系统已登记的行:`riscv64-linux-musl`」);
4. 多个受支持而词法默认不在其中 —— 拒绝并列出候选。今天没有任何
   `(arch, os)` 是这个形状。

规则 1 排在最前,使这件事能自己退休:`aarch64-linux-gnu` 从 `planned` 升级的
那一天,词法答案重新胜出,不需要有人回来改任何东西。

**写出这一段就是退出补全。** 写出来的段是请求而不是空缺,因此
`--target aarch64-linux-gnu` 仍会撞上 `planned` 行的拒绝 —— 那正是用显式
`[target.<triple>] toolchain` 提前加入某一行的逃生口。

### 该用哪种拼法

**在构建期体系下,省略它。** 图供给 C 库与各运行时,那一段陈述的是一个
不会被查询的请求。在这种体系下,`x86_64-windows` 不只是比
`x86_64-windows-gnu` 短 —— 它更准确,因为并没有任何 GNU 的东西参与。

**在传统预构建体系下,当这个选择有意义时写出它。**
`x86_64-linux-musl` 与 `x86_64-linux-gnu` 选中不同的载荷、产出不同的产物。
写出那一段正是作出这个选择的方式。

**在 Windows 上,想要微软那套 ABI 时写 `msvc`。** `gnu` 是默认填充,
而 `msvc` 是不同的对象 ABI 而非不同的 C 库,因此那一段在两种体系下都有意义。

## 构建报告了什么

报告以写下的目标为标题,并把它解析为编译器自己的拼写:

```
      Target x86_64-windows → x86_64-w64-windows-gnu
             kernel-abi        openkal        (openkal-windows@0.1.3, graph)
             c-abi             musl           (openkal-musl@0.3.3, graph)
             c++-abi           libc++         (openkal-llvm-runtime@0.1.2, graph)
```

有两条诊断挂在第三段上,适用哪一条由上面那张表决定。

**当该段命名 C 库、而图供给了另一个时,构建报出这个分歧。** 以图为准,
所以这是报出而非拒绝 —— 两种写法下产物相同,不准确的只是名字:

```
warning: the target name asks for the `gnu` C ABI and the dependency graph supplies `musl`.
       The graph decides, so the build below uses `musl` — the name is what is inaccurate,
       not the artifact. Drop the segment to say what is actually meant:
           --target x86_64-linux
```

**当该段命名的是别的东西时,报告说出那是什么。** 在那里发警告是错的:
它会在每一次合法的 MinGW 构建上出现,并且描述了一条名字从未涉及的轴。
该提示只在使用者主动写出那一段时出现,且命名那套 **ABI 本身**而非任何一层:

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu   (gnu selects the Itanium C++ ABI, not a C library)
```

⭐ 而它**不对应报告里的任何一行**,这正是要点。五层记录的是每一层
**由谁供给**;那一段命名的是这些**对象遵循哪套约定**,是若干层必须一致的
横切事项。把它读成 `c++-abi libc++` 是第二个错误答案,因为 libstdc++
坐在同一套 ABI 上。

## 三套词表,以及它们为何不同

一个三元组由三方书写,而三方并不共用一套约定;mcpp 在它们之间翻译。
知道手上这个字符串属于哪一套,第三段带来的困惑就消掉大半。

### 形状

```
<arch> - <vendor> - <os> - <env>
```

每一段都可省,省掉的由编译器补。`vendor` 是历史遗留,今天几乎不承重 ——
除非目标有理由说别的,否则一律填 `unknown`:

```
x86_64-linux-gnu   →  x86_64-unknown-linux-gnu
aarch64-macos      →  aarch64-unknown-macos
```

三段写法是四段省掉一段,省的是哪一段由「能否解析」决定。

### GCC 与 clang 选目标的方式根本不同

| | GCC | clang |
|---|---|---|
| 目标 | **构建编译器时**就定死 | **运行时**选 |
| 怎么问 | `-dumpmachine` | `-dumpmachine`,或 `--target=` |
| 交叉编译 | 换**另一个可执行文件** | 传一个 flag |

实测:

```
g++                     →  x86_64-linux-gnu       (只能发这个)
x86_64-w64-mingw32-g++  →  x86_64-w64-mingw32     (只能发这个)
clang++                 →  x86_64-unknown-linux-gnu,而 --target= 可改
```

⭐ 这就是传统预构建体系**不传 `--target`** 的原因:那份载荷的编译器以它唯一
能发的目标命名,选目标等于选载荷。也是构建期体系只需要一个编译器的原因。

### MinGW 按 GCC 的约定给自己命名

MinGW 自己的三元组是 `x86_64-w64-mingw32`:

| 段 | 值 | 为什么 |
|---|---|---|
| arch | `x86_64` | |
| vendor | `w64` | 项目名 `mingw-w64`,用以区别于已停滞的原 `mingw32` 项目 |
| os | **`mingw32`** | ⭐ MinGW 把**自己**放在 OS 位 |
| env | (无) | 三段就是全名 |

这套约定源自 autoconf 的 `config.guess`,那里 OS 段命名的是目标的运行环境 ——
而在 GNU 工具链的世界观里 MinGW **就是**一个独立环境:有自己的头文件、
自己的 C 运行时、自己的 `configure` 分支。名字里的 `32` 是历史遗留,
`w64` 才表示这是那个支持 64 位的项目。

LLVM 不接受这个世界观。它认为 OS 是 `windows`,MinGW 只是其上的一种 ABI
环境,于是把这个名字重拼 —— 实测:

```
x86_64-w64-mingw32  →  x86_64-w64-windows-gnu
x86_64-pc-mingw32   →  x86_64-pc-windows-gnu
```

```
GCC   x86_64 - w64     - mingw32 - (无)
                ^vendor   ^os
LLVM  x86_64 - unknown - windows - gnu
                ^vendor   ^os       ^env
```

⭐ **`mingw32` 从 OS 位被拆成 `windows` 加 `gnu`。** `gnu` 这个取值之所以
存在,正是因为 LLVM 需要给拆剩下的那一半起个名字。它的含义是
「MinGW/Itanium 这一支 ABI」,在 Windows 上从来不是「C 库是 glibc」——
同一个词在不同操作系统下承担不同职责,这是 LLVM 词表的既有事实,
不是 mcpp 的发明。

### mcpp 保留的那一套

| 词表 | 例 | 谁读 |
|---|---|---|
| GCC / autoconf | `x86_64-w64-mingw32` | 预构建载荷的编译器,以文件名的形式 |
| LLVM | `x86_64-w64-windows-gnu` | `clang --target=` |
| **mcpp** | `x86_64-windows-gnu` | 目标表、输出目录、`cfg()`、打包的 ABI tag |

mcpp 自己那套必须能映到前两套。构建报告里那个箭头就是这个映射,
从第三套到第二套:

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu
       ^ mcpp                ^ LLVM
```

⚠️ 把第三套词表独立出来,正是 mcpp 能命名 LLVM 命名不了的东西的原因。
实测 llvm 22.1.8:`windows` 配 `musl` 环境能被三元组解析器接受,
而编译器会崩:

```
clang++ --target=x86_64-pc-windows-musl -c t.cpp
    #5  llvm::MCWinCOFFStreamer::emitCGProfileEntry(...)
```

崩点在 **COFF 写出器**:clang 已认定输出是 COFF,却没有一条 Windows-musl
的路径去把 streamer 需要的状态配齐。它认识的四个非 MSVC Windows 环境 ——
`gnu`、`cygnus`、`itanium`、`musl` —— 前三个都能编,只有 `musl` 死;
预定义宏说明了这个环境从未被建模:

| 三元组 | 宏 |
|---|---|
| `…-windows-gnu` | `__GNUC__` `__MINGW32__` `_WIN32` |
| `…-windows-msvc` | `_MSC_VER` `_WIN32` |
| `…-windows-musl` | `__GNUC__` `_WIN32` —— **无 `__MINGW32__`** |

所以 Windows 上的 musl C 库**无法**向 clang 命名,而 mcpp 仍然必须为它命名,
因为 mcpp 的名字回答的是另一个问题:**C 库是谁**,而 LLVM 的名字回答
**遵循哪套对象 ABI**。两者都需要,且不是同一个字符串。

## 编译器与 C 库是两个轴

目标命名的是一台机器。它不指定谁来编译,也不指定它的 C 库从哪来 —— 那是另外两个
选择,而同一个目标字符串在每种选择下都是不同的构建。

⚠️ **「来自载荷」不是某一份固定载荷**,而是所选编译器带来的那一份;gcc 与 clang
带法不同:gcc 一个目标一份载荷、驱动带三元组前缀,而一个 `clang++` 打它构建时支持
的每个目标。同一台宿主、同一份源码实测:

| 工具链 | 目标 | 实际运行的驱动 | c-abi | c++-abi |
|---|---|---|---|---|
| `gcc@16.1.0` | `x86_64-linux-musl` | `xim-x-musl-gcc/…/x86_64-linux-musl-g++` | musl | libstdc++ |
| `gcc@16.1.0` | `x86_64-windows-gnu` | `xim-x-mingw-cross-gcc/…/x86_64-w64-mingw32-g++` | gnu | libstdc++ |
| `llvm@22.1.8` | `x86_64-linux-musl` | `xim-x-llvm/…/clang++` | musl | libc++ |
| `llvm@22.1.8` | `x86_64-windows-gnu` | `xim-x-llvm/…/clang++` | gnu | libc++ |

clang 不会去 gcc 的载荷里取 C 库,gcc 也不会去 clang 的载荷里取。各带各的。

### 换另一个编译器,就要把另一份 C 库一起换上

每一行有宿主的目标都写着一个工具链,而那个名字是**约定**不是**能力**:它回答的是
*哪个载荷供给这个目标的 C 库*,所以一个自己供给 C 库的工程可以写另一个编译器。
不能做的是写另一个编译器而什么都不供给。

```toml
[toolchain]
default = "llvm@22.1.8"        # x86_64-linux-musl 这一行写的是 gcc
```

```
$ mcpp build --target x86_64-linux-musl
error: target 'x86_64-linux-musl' takes its C library from the 'gcc@16.1.0'
       payload, and 'llvm@22.1.8' has none here.
```

⚠️ **2026.8.26.1 之前这会把整个构建跑完,然后在链接上失败**,报
`crtbeginT.o (bare name — the linker cannot resolve it)`——对症状准确,对决定沉默。
clang 是可重定向的,自己不带 C 库,于是去够一份 gcc 安装;在恰好装了系统 mingw 的
机器上,同样写法用于 `x86_64-windows-gnu` 够到的是
`/usr/lib/gcc/x86_64-w64-mingw32/…`,那比失败更糟。

补上替代者,就是全部的差别:

```toml
[dependencies]
openkal-llvm-runtime = "0.1.3"   # → openkal-musl → openkal-<os>
[toolchain]
default = "llvm@22.1.8"
```

这就是 [`examples/06-openkal-cross`](../../examples/06-openkal-cross),也是那句
拒绝里为什么点名 openkal。

⚠️ **裸机行与 `x86_64-windows-musl` 行的工具链不是约定**,根本不可被推翻——
见[第 03 章](03-toolchains.md)。

### 而依赖图会整个替换这一轴

同样三个目标,图里有 `openkal-musl` 与 `openkal-llvm-runtime` —— 同法实测:

| 目标 | kernel-abi | c-abi | c++-abi |
|---|---|---|---|
| `x86_64-linux-musl` | openkal(openkal-linux,图) | musl(图) | libc++(图) |
| `x86_64-windows-gnu` | openkal(openkal-windows,图) | musl(图) | libc++(图) |
| `x86_64-windows-musl` | openkal(openkal-windows,图) | musl(图) | libc++(图) |

⚠️ **看两张表里的 `x86_64-windows-gnu`。** 载荷供给时它的 C 库是 `gnu`,即 MinGW
CRT;图供给时是 `musl`。一个目标字符串,两个不同的 C 库 —— 而 mcpp 在 2026.8.24.6
之前无法说清是哪一个:同一条 `--target x86_64-windows-gnu` 产出的东西体积差 16.7
倍、依赖的 DLL 完全不同。

这就是 `x86_64-windows-musl` 作为独立名字存在的理由。它与 `x86_64-windows-gnu`
映射到**同一个 LLVM 三元组** —— LLVM 拼不出它 —— 所以两者在编译器那一侧无法区分,
全部差别就在于用的是哪个 C 库。任何宿主都没有为它准备的载荷;它的系统只能来自依赖
图,这正是 `toolchain list` 报的 `via dependency graph`。

## 构建机是第三条轴

上面两条轴 —— 用哪个编译器、C 库从哪来 —— 是工程做的选择。第三条不是:它是构建
运行在哪台机器上。

⭐ **这条轴是 mcpp 自己发布的那一组,而且是 (os, arch) 不是 os。**
`release.yml` 发布四份宿主二进制:

| 构建机 | 发布资产 | CI runner |
|---|---|---|
| `linux-x86_64` | `mcpp-<v>-linux-x86_64.tar.gz` | `ubuntu-24.04` |
| `linux-aarch64` | `mcpp-<v>-linux-aarch64.tar.gz` | `ubuntu-24.04-arm` |
| `macos-arm64` | `mcpp-<v>-macosx-arm64.tar.gz` | `macos-14` |
| `windows-x86_64` | `mcpp-<v>-windows-x86_64.zip` | `windows-2022` |

⚠️ **两台 Linux 不是同一台。** `x86_64-linux-gnu` 需要本机架构的 `xim:glibc` 与
`xim:linux-headers` 载荷,而它们只为宿主自己的架构存在 —— 所以那一行从
`linux-x86_64` 够得着,从 `linux-aarch64` 够不着;`aarch64-linux-gnu` 是镜像的
情形,两台上都是 `planned`。把它们并成 `linux`,一台会把另一台的行覆盖掉。

### 哪台构建机服务哪个目标

| target | tier | pin | linux-x86_64 | linux-aarch64 | macos-arm64 | windows-x86_64 |
|---|---|---|---|---|---|---|
| `x86_64-linux-gnu` | verified | — | ✅ 载荷 | — | — | — |
| `aarch64-linux-gnu` | planned | — | planned | planned | planned | planned |
| `x86_64-linux-musl` | verified | `gcc@16.1.0` | ✅ 载荷 | ✅ 载荷 | — | ✅ 载荷 |
| `aarch64-linux-musl` | verified | `gcc@16.1.0` | ✅ 载荷 | ✅ 载荷 | — | — |
| `riscv64-linux-musl` | planned | — | planned | planned | planned | planned |
| `x86_64-windows-gnu` | verified | `gcc@16.1.0` | ✅ 载荷 | ✅ 载荷 | — | ✅ 载荷 |
| `x86_64-windows-musl` | preview | `llvm@22.1.8` | ⚙ 图 | ⚙ 图 | ⚙ 图 | ✅ 载荷 |
| `x86_64-windows-msvc` | verified | — | — | — | — | ✅ 系统 |
| `aarch64-macos` | verified | — | — | — | ✅ SDK | — |
| `x86_64-macos` | planned | — | planned | planned | planned | planned |
| `riscv64-none-elf` | verified | `llvm@22.1.8` | ✅ 载荷 | ✅ 载荷 | ✅ 载荷 | ✅ 载荷 |
| `riscv32-none-elf` | verified | `llvm@22.1.8` | ✅ 载荷 | ✅ 载荷 | ✅ 载荷 | ✅ 载荷 |
| `aarch64-none-elf` | preview | `llvm@22.1.8` | ✅ 载荷 | ✅ 载荷 | ✅ 载荷 | ✅ 载荷 |
| `x86_64-none-elf` | preview | `llvm@22.1.8` | ✅ 载荷 | ✅ 载荷 | ✅ 载荷 | ✅ 载荷 |

`✅ 载荷` 这里有工具链载荷产出它 · `⚙ 图` 没有载荷,但依赖可以供给系统 ·
`✅ 系统` 在机器上被找到,不是 mcpp 装的 · `✅ SDK` 平台自己的 ·
`—` 从这台宿主够不着 · `planned` 词表里有,还没有任何东西接线。

### 列背后的规则

| 目标类别 | 哪些构建机服务它 | 为什么 |
|---|---|---|
| `*-linux-musl` | Linux(任意架构)、Windows(仅同架构) | musl 载荷是自足的 |
| `*-linux-gnu` | Linux,且仅同架构 | 还要本机架构的 `xim:glibc` / `xim:linux-headers` |
| `x86_64-windows-gnu` | Linux、Windows | 一个身份,只在分发层按宿主分岔 |
| `x86_64-windows-msvc` | Windows | MSVC 是在机器上被找到的 |
| `x86_64-windows-musl` | 载荷只在 Windows;走图则任意宿主 | 没有 gcc 发得出 PE+musl,而 LLVM 拼不出这个三元组 |
| `aarch64-macos` | macOS | SDK 是那台机器的 |
| `*-none-elf` | 每一台 | clang 与 lld 按构造就是交叉编译器 |

⚠️ **一个 `—` 讲的是载荷,不是可能性。** `host_can_serve` 回答的是「这里有没有
载荷产出它」,而依赖图可以改为供给系统 —— 这就是 `x86_64-windows-musl` 在 Linux
上显示 `via dependency graph`、并在那里产出真正的 PE32+ 的原因。

### 而 CI 把每一台都测了

[`ci-target-matrix.yml`](../../.github/workflows/ci-target-matrix.yml) 在全部四台
宿主上跑。每台扫描它列出的每一行两遍 —— 只有载荷,以及图里加上
`openkal-musl` + `openkal-llvm-runtime` —— 与
[`tests/matrix/expected.tsv`](../../tests/matrix/expected.tsv) 比对,键是
`(mode, host, target, compiler)`。

⚠️ 每台把什么解析成自己的目标,与它的名字给人的印象并不一致:

| runner | 它解析出的宿主目标 |
|---|---|
| `ubuntu-24.04` | `x86_64-unknown-linux-gnu` |
| `ubuntu-24.04-arm` | `aarch64-unknown-linux-gnu` |
| `macos-14` | `arm64-apple-darwin23.6.0` —— **ARM**,不是 x86_64 |
| `windows-2022` | `x86_64-pc-windows-msvc` —— **msvc**,而那里的 mingw gcc 目标是 `-gnu` |

本章三条判据都曾假设了 Linux 上的那个巧合,并在其它宿主上被纠正。

⚠️ **每台的格数不是常数。** 它取决于那台机器装了什么,而同一台 runner 在相邻两轮
里被测到工具链不同。所以比对断言的是**扫描真的产出了行**、以及**期望表点名的每一
行都被跑到**,而不是一个总数:一格因为载荷没被恢复而消失,与一格通过了,在退出码
上没有区别。

## 自定义目标


不在 mcpp 表内的三元组需要一个显式段落,而这也是一块板子声明
「任何默认值都给不出的事实」的方式:

```toml
[target.riscv64-none-elf]
sysroot = ""
runner  = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
           "-no-reboot", "-bios", "default", "-kernel"]
```

`sysroot = ""` 选定零 libc 档:编译行上没有 C 库,链接上也没有。
⚠️ **缺席 `sysroot` 键是另一个答案** —— 它继承该目标行自己的默认值。
见[第 13 章](13-baremetal.md)。

## 参考

[第 14 章](14-target-side.md)讲五层以及每一层由谁供给。
[第 15 章](15-openkal-cross.md)完整讲构建期体系。
[第 03 章](03-toolchains.md)讲工具链轴,它是分开的:目标不决定编译器。
