# 12 —— 分发预编译库

[English](../12-binary-distribution.md) | **简体中文**

**读者：** 交付编译产物而不是源码的发布方。

**本章回答的那一个问题：** 怎样交付二进制，以及消费方的构建如何判断其中哪一个合用。

**不在这里：** 发布源码，那是 [11 —— 发布一个库](11-publishing-a-library.md)；以及
兼容性 tag 的加速器字段，那是 [42 —— 异构硬件构建](42-heterogeneous-builds.md)。

> 把一个库以**接口 + 预编译二进制**的形式分发，而不是发源码。
> 适用于闭源分发、离线环境，以及构建产物已经由构建农场产出的场景。
>
> 相关文档：[10 - 打包与发布](10-pack-and-release.md) 说明**应用程序**的打包；
> [11 - 发布一个库](11-publishing-a-library.md) 是源码分发的路径。

## 概述

`mcpp pack <target>` 构建一个库目标，产出一个**普通的 mcpp 包**——一份正常的
`mcpp.toml`、消费方要编译的接口、以及它随后链接的二进制。消费方使用它的方式与
使用任何其他依赖完全相同。没有新的 manifest 段，没有新的归档格式，也没有新的
解析路径。

```bash
mcpp pack mathkit                              # a static library package
mcpp pack mathkit-shared                       # a dynamic one (ELF, Mach-O, PE/MinGW)
mcpp pack mathkit --target x86_64-linux-gnu \
                  --target aarch64-linux-gnu   # one package, two legs
```

## 打包内容的决定依据

只由 `[targets.<name>].kind` 决定，没有别的：

| `kind` | `mcpp pack <name>` 产出 | `--mode` |
|---|---|---|
| `bin` | 一个应用 bundle（见 [10](10-pack-and-release.md)） | 四档深度 |
| `lib` | 一个**静态库包** | — |
| `shared` | 一个**动态库包** | — |

没有 `--lib` 旗标，也没有 `--artifact static|shared`。`kind` 本来就是 mcpp
记录「一个产物是什么」的地方；再加一个旗标就是同一件事的第二种说法，而两种
说法可能互相矛盾。要同时发布两种形态，就声明两个目标——这本来也是
`mcpp build` 要同时产出两者所必需的：

```toml
[targets.mathkit]
kind = "lib"

[targets.mathkit-shared]
kind   = "shared"
soname = "libmathkit.so.1"
```

`mcpp pack` 不带名字运行时，会选择唯一可打包的目标；找到多个候选时则列出它们。

## 两种接口模式

一个包可以同时携带两种，消费方可以只用其中一种，也可以两种都用。

```
mathkit-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23/
├── mcpp.toml
├── include/          ← TEXT interface: #include, never compiled
├── interface/        ← MODULE interface: the consumer compiles it
└── lib/<triple>/     ← the artifacts
```

| | `include/` | `interface/` |
|---|---|---|
| 是谁的输入 | 预处理器的 | **编译器**的 |
| 消费方要编译它吗 | 否 | **是**，编出 BMI |
| 约束的是什么 | libc ABI | 编译器、C++ 标准库、C++ 档位 |
| 能裁剪吗 | **不能**——见下 | **不能**，它是算出来的 |

`lib/` 按**三元组**分目录，不按 OS 分。MinGW 与 MSVC 同为 Windows，却分别产出
`libfoo.a` 与 `foo.lib`。

**共享**库包以库的两个名字*同时*携带它：消费方链接 `lib<target>.so`，加载器随后
按 `SONAME` 查找。声明了 `soname` 时，这是两个不同的文件名，只发布构建出的
那个文件，链接能成功而启动会失败。未声明 `soname` 的库把自己的文件名记为它的
`SONAME`(mcpp 2026.9.14.2+)，于是它的两个名字就是同一个文件。

### 两个集合都不可裁剪的原因

同一个包的**源码**分发会把它 `include_dirs` 里的每一个头文件都放到消费方的
include 路径上。若二进制包只发布其中一部分，同一个库的公开面就会因为分发形式
不同而不同。而且「哪些头文件是公开的」这件事，布局本身已经回答了：`include/`
公开，`src/` 不公开。一个私有头文件放在 `include/` 下，是工程布局的错误，不是
一个打包选项。

## 发布的接口单元

**lib root 的模块闭包**——按约定是 `src/<包名尾段>.cppm`，或 `[lib].path` 所
指的文件。该单元 purview 内传递地 import 到的一切，都会被发布；其余都不发布。

```
src/mathkit.cppm    export module mathkit;  export import :api;   → published
src/api.cppm        export module mathkit:api;                    → published
src/secret.cppm     module mathkit:secret;         ← implementation partition
src/impl.cpp        module mathkit;                                → withheld
```

`mcpp pack` 会打印两张清单：

```
   Interface mathkit.cppm, api.cppm
    Withheld capi.c, impl.cpp, secret.cppm
```

**对闭源分发来说，要紧的是第二张清单。**

> **`.m.o` 不是判据。** 实现分区产出的 BMI 与对象，与接口单元完全一样。按
> 「会不会产出 BMI」来挑选发布集，就会把 `secret.cppm` 一并发布出去。

如果被发布的接口**确实** import 了一个实现分区，消费方没有那份源码就编译不出
来——于是 `mcpp pack` 会停下：

```
error: the published interface imports mathkit:secret , which no unit in this
       build provides.
```

要么重构，让接口够不到它；要么把它改成 `export module` 分区，并接受它的源码
被发布。

## 兼容性 tag

每一个产物都记录它是为哪一套工具链构建的：

```
x86_64-linux-gnu-gcc16-libstdcxx16-c++23      # a C++ module interface
x86_64-linux-gnu                              # an extern "C" interface only
```

即 `<arch>-<os>-<env>`，当接口是 C++ 时再加上 `<compiler><major>`、
`<stdlib><major>`、`c++<level>`。

**更短的 tag 是一句真实的声明，不是漏写。** 一个接口全是 `extern "C"` 的库，
只约束 libc ABI、不约束 C++ ABI，所以它只发布三段就停止——并因此能链进任何
编译器。未命名的维度就是不关心，于是这类库的 tag 是每个三元组一个，而不是每个
三元组配每个编译器各一个。没有任何东西需要配置：形状本身就是声明。

`c++` 档位按**下限**比对，不按相等比对：消费方用更高的档位构建没问题，更低则
不行。

## 消费端的构建检查

两件事，而且两者都是不检查就会静默出错的：

**接口仍然与它的二进制配对。**

```
error: acme.mathkit@0.1.0: 'interface' does not match what was packaged.
  recorded fnv1a:25b2cf2a79d71c40
  found    fnv1a:fe404d5be85118ff
```

这道闸门的存在，是因为另一种结果被实测过。把随包发布的接口里一个结构体的两个
`int` 成员互换——Itanium ABI 不 mangle 字段顺序——消费方编译通过、链接通过、
运行通过，并打印出被互换的错误数据，而任何工具都不会报出一句诊断。一个摘要
挡不住发布者一开始就发出一对不匹配的接口与二进制（只有原子式地一次产出两者
才能防住这一点），但它能挡住这一对配对在事后被拆开。

**二进制是为这一套工具链构建的。**

```
error: acme.mathkit@0.1.0: no prebuilt artifact matches this toolchain.
  your toolchain : x86_64-linux-gnu-gcc16-libstdcxx16-c++23
  published tags :
                   x86_64-linux-gnu-gcc15-libstdcxx15-c++23
  closest is x86_64-linux-gnu-gcc15-libstdcxx15-c++23, and it differs on:
    compiler  needs gcc15, this build has gcc16
    stdlib    needs libstdcxx15, this build has libstdcxx16
```

诊断里列出它**确实拥有**哪些 tag 是刻意的：一句「没找到」会让读者去找一个
已经在自己磁盘上的包。

## 消费方式

三种写法，同一条代码路径：

```toml
# a directory (copied directly to a colleague)
mathkit = { path = "vendor/mathkit-0.1.0-x86_64-linux-gnu-gcc16-libstdcxx16-c++23" }

# a private git repo
mathkit = { git = "ssh://git@internal/mathkit-dist.git", tag = "v0.1.0" }

# an index entry — identical in shape to a source package's
mathkit = "0.1.0"
```

消费方的 manifest 里没有任何一处写着「这个是预编译的」。

### 在包目录内直接构建被拒绝

```
error: … is a distribution package produced by `mcpp pack`, not a source tree.
```

它的 `interface/` 里是声明，定义在旁边的归档里。在那里构建会把声明编译出来，
产出一个近乎空的库，并报告成功。

## 单包多个 target

`--target` 可重复传递。生成的 manifest 里每个 target 各有一个条件块，消费方的
构建各自选择自己需要的那一个：

```toml
[target.'cfg(all(arch = "x86_64", os = "linux", env = "gnu"))'.build]
ldflags = ["-Llib/x86_64-linux-gnu", "-lmathkit"]

[target.'cfg(all(arch = "x86_64", os = "linux", env = "musl"))'.build]
ldflags = ["-Llib/x86_64-linux-musl", "-lmathkit"]
```

因为这次选择发生在**消费方的构建期**——那时解析出的 target 已知——一个
多 target 包的交叉编译天然正确，不需要索引侧或安装侧提供任何支持。

> 这些条件块一律是 `cfg(...)`，从不是裸的 `[target.'<三元组>']` 键。在 mcpp
> 2026.8.18.1 之前，裸三元组形式在没有显式 `--target` 时是失效的，于是用它的
> 包会在 CI 里正常工作，却在开发者的机器上静默丢掉这些 flag。mcpp 生成的是
> 在每一个客户端上含义都一致的那种写法。

## 依赖

一个静态归档**不携带**它所依赖的代码，所以包会把这些依赖记录下来，由消费方
解析：

```toml
[dependencies]
"compat.zlib" = "1.3.2"
```

`path` 与 `git` 依赖会被丢弃：它们指向发布者自己的磁盘，原样重新发布出去，
等于把一个在消费方机器上含义完全不同的地址交给了对方。一个依赖这类来源的库，
要么把那个依赖也一并发布，要么在打包前把它 vendor 进来。

## 旧版本 mcpp 的行为

**能构建。** 生成的 manifest 里每一个键都是既有的键，所以旧客户端能读懂这个
包并把它链接起来。它做不到的是执行上面那两道检查——它无从知道
`provenance = "mcpp-pack …"` 意味着什么。

这是一种降级而不是损坏，而且方向是对的。但这意味着**这道闸门只保护新客户端**，
面向混合版本用户群发布的任何包，都应当把这一点写进发布说明。

## 随包内容与刻意排除的部分

一个已发布的包必须能在不是发布者本人的机器上工作。两个步骤保证这一点，两者
都作用于打包器暂存的每一个产物。

### 构建机的加载器搜索路径会被移除

一次开发构建，会把工具链自己的目录烙进每一个共享对象：

```text
DT_RUNPATH = <home>/registry/data/xpkgs/xim-x-glibc/2.44/lib64
           : <home>/registry/data/xpkgs/xim-x-gcc/16.1.0/lib64
           : <home>/registry/subos/default/lib
```

这对开发构建来说是对的，对一个包来说却是致命的（issue #460），原因在于 ELF
加载器的一条规则：**一个携带任何 `DT_RUNPATH` 的对象，会让加载器在解析它自己
的依赖时，跳过整条继承而来的 `DT_RPATH` 链。** 于是消费方自己那条
`DT_RPATH`——载荷、包目录、SubOS farm，全都是在真正要运行它的那台机器上算出
来的——根本不会被查询，程序死于

```text
error while loading shared libraries: libstdc++.so.6: cannot open shared object file
```

**`$ORIGIN` 不是解法。** 在一个真实的包上、把构建机的 store 变得不可达之后
实测：

| 已发布 `.so` 上的状态 | 是否继承消费方的 `DT_RPATH` | 结果 |
|---|---|---|
| 失效的绝对路径 `DT_RUNPATH` | 否 | 失败 |
| **完全没有这条 tag** | **是** | **能跑** |
| `DT_RUNPATH = $ORIGIN` | 否 | 失败 |
| `DT_RUNPATH = ""` | 否 | 失败 |

关掉继承的是这条 tag 的**存在本身**，不是它的内容。所以 `mcpp pack` 删掉这个
条目，而不是改写它——而删掉并不是一种妥协，它就是正确答案：消费方自己的
`DT_RPATH` 是同一个闭包，只是在它真正有意义的那台机器上被解析出来的。

那条路径**字符串**仍留在 `.dynstr` 里，只是没有人再指向它。`.dynstr` 会被
链接器做尾部合并，一个更短的、仍在使用的字符串可能从这条已死字符串的中间
开始，删掉那些字节因此无法被证明是安全的；`patchelf --remove-rpath` 留下的
残留，在文件大小上与原来逐字节相同。**所以为这件事写的守卫必须读动态段的
条目，绝不能对文件字节做 `grep`**——见 `tests/e2e/_elf_tag.sh`。

在 Mach-O 上，打包器会读出 `LC_RPATH` 并在包将要携带它时发出警告；自动改写
（`install_name_tool -delete_rpath`）尚未实现，因为这个套件里目前还没有任何
测试会产出一个 `.dylib` 来为这次字节级编辑提供判据。

### 调试信息会被移除

旗标、按产物形态分的表格与 `--debug-symbols`，见
[docs/10](10-pack-and-release.md)。对**库**包来说最要紧的一条规则是：一个
静态归档只会被 `--strip-debug`，因为 `--strip-all` 会删掉归档自身的符号索引，
使消费方的链接失败并报
`archive has no index; run ranlib to add one`。

## 当前边界

| | 状态 |
|---|---|
| `kind = "lib"`（静态） | 每个 target，三个平台都测过 |
| `kind = "shared"` on Linux/ELF | —— 包同时携带链接名与 SONAME，不含构建机的加载器路径 |
| `kind = "shared"` on PE / MinGW(`*-windows-gnu`) | —— 包同时携带 `.dll` **及其**导入库 |
| `kind = "shared"` on Mach-O(`*-macos`) | —— install name 为 `@rpath/<file>`，`.dylib` 可重定位。`LC_RPATH` 只被报告，尚未被改写 |
| `kind = "shared"` on PE / MSVC(`*-windows-msvc`) | —— mcpp 生成 `.def`；见下文 |
| `kind = "shared"` on `*-musl` | musl target 是静态链接的 |
| 同一个包为同一个三元组携带两套 ABI(gcc **与** clang) | leg 的选择是 `cfg(arch/os/env)`；每套 ABI 各发布一个包 |
| 发布预编译 BMI | 未尝试；BMI 与编译器构建逐位绑定 |
| 把依赖打包进去 | 改为声明依赖（见上文） |
| 用**原生 `cl.exe`** 消费这种包 | —— 经由方言中立的链接意图；见下文 |

### MSVC ABI 上的符号导出

除非源码写了 `__declspec(dllexport)`，或一份 `.def` 文件列出了符号，MSVC 不会
从一个 DLL 导出任何东西。两者都没有时，导入库会是空的，每个消费方都会因为一批
明明就在对象文件里的符号拿到 unresolved externals。MinGW 的链接器会自动导出，
把这个问题整个遮住；lld-link 的 MSVC 形态刻意不这样做，因为 PE 的导出上限是
65535。

mcpp 从对象文件生成 `.def`——这正是 CMake 的 `WINDOWS_EXPORT_ALL_SYMBOLS`
自 3.4 起在做的事。它是构建图里的一个节点，输入就是链接所消费的那批对象，
因此导出面不会与「实际编译了什么」发生漂移；而且它直接读 COFF，不会 shell 出去
调用 `dumpbin`——那个工具存在于 Visual Studio 的开发者环境里，而 mcpp 在
Windows 上的默认工具链是 clang。

**有两条限制是任何工具都消不掉的**，与 CMake 为同一机制记录的正是同样两条：

| | |
|---|---|
| 导出的**数据** | 消费方的声明上仍需要 `__declspec(dllimport)`；没有它，链接器读到的是一个调用桩，而不是值本身 |
| **vtable** 被引用的类 | 整个类都要被标注，例如一个带虚函数的类的委托构造函数 |

两者都靠标注来回答，而且**标注优先**：一个已经带 `/EXPORT:` 指令的对象——那
正是 `__declspec(dllexport)` 产生的——会让 mcpp 让开，什么都不生成。在其上再
叠加一份列表，会把同一批符号导出两次（`LNK4197`），还会把其余所有符号也一并
导出，用「全部」取代作者选定的那个公开面。这件事没有任何东西需要配置：对象
文件自己说了算。

超过 65535 个可导出符号时，mcpp 拒绝而不是截断。一个被截断的导出表能干净地
链接完成，随后在恰好需要那个掉出去的符号的消费方那里失败。

### 包里的链接旗标是 GNU 拼写

生成的 manifest 按下面这种形式为每个 leg 选择产物：

```toml
[target.'cfg(all(arch = "x86_64", os = "windows", env = "msvc"))'.build]
ldflags = ["-Llib/x86_64-windows-msvc", "-lmathkit"]
```

mcpp 用到的每一个 driver 都吃这一套——包括 Windows 上默认的、面向 MSVC ABI
的 clang。**原生 `cl.exe` 不吃这一套**：它不认 `-L`。所以包里会把同一句话再
写一遍，这一遍不带方言：

```toml
[target.'cfg(all(arch = "x86_64", os = "windows", env = "msvc"))'.runtime]
link_library_dirs = ["lib/x86_64-windows-msvc"]
libraries         = ["mathkit"]
```

mcpp 会按 target 把它们渲染成 `/LIBPATH:` + `<name>.lib`，或者 `-L` +
`-l<name>`，于是 `cl.exe` 的消费方也能链接这个包。这两个键不是新词表——
`[runtime]` 顶层一直就带着这两个键，这里只是让它们能够按 target 分开给出。

**两种拼写都会被写出来，而更新版本的 mcpp 会丢掉那一条 leg 的库引用，而不是
把两者叠加。** 旧版 mcpp 只读 `ldflags`，并静默忽略 `runtime` 块，所以去掉
`ldflags` 会让所有旧客户端拿不到任何链接 flag；而两者都生效，又会把 `-L` 重新
送回 `cl` 的命令行——那正是要避免的事。

有一条 leg 被刻意排除在这条规则之外：一条 **PE/MinGW 共享库**的 leg，链接行是
`-L… -Wl,-Bdynamic -lmathkit`，而 `-Wl,-Bdynamic` 只有紧邻它所启用的那个 `-l`
时才生效——mcpp 给 PE 可执行文件加了 `-static`，否则链接器会停在纯静态模式，
拒绝一个导入库。中立形式无法表达「先切换链接模式」，所以这条 leg 保留了能用
的那种拼写。这不付出任何代价：PE/MinGW 的 leg 不是 MSVC ABI 的 leg，`cl.exe`
永远读不到它。

改成直接写文件路径（`lib/<triple>/mathkit.lib`）是每个 driver 都能吃的拼写，
但同样行不通：ninja 执行链接命令时，cwd 是**输出目录**，而只有 include 家族的
前缀（`-I`、`-L` 等）会相对包根被绝对化，一个不带前缀的 token 就会被到错误的
地方去找——`ld: cannot find lib/x86_64-windows-gnu/libmathkit.a`。而 manifest
里写绝对路径，就不再可重定位了。

方言中立的通道确实存在——`[runtime] link_library_dirs` 与 `libraries`，
mcpp 会依目标把它们渲染成 `/LIBPATH:` + `name.lib` 或 `-L` + `-lname`——但只在
顶层读取，而一个包需要它**按每条 leg** 分别给出。实测：一个更早版本的 mcpp
读到 `[target.'cfg(…)'.runtime]` 时不会报错，而是静默忽略它。这对这个用途来说
是错的那一种容忍——把一条 leg 的链接 flag 移到那里，会让所有旧客户端一个链接
flag 都拿不到，所以要正确地补上这一块，要么两种拼写都携带（代价是在新客户端上
把库链接两遍），要么给这类包一个版本下界。

### 实现分区

`mcpp pack` 把实现分区（`module M:part;`，不带 `export`）当作私有的：它的源码
留下，它的对象文件随归档一起发出去。

如果被发布的接口 *import* 了一个实现分区，消费方没有那份源码就编不出 BMI，
所以它**会**被发布——而 `mcpp pack` 会说出这件事：

```
warning: secret.cppm is an implementation partition, and the published interface
         reaches it — so its SOURCE is being published.
```

> 在 mcpp 2026.8.18.1 之前，扫描器把 `module M:part;` 记成**「requires
> `M:part`，provides 空」**——一个文件 requires 自己的名字，于是图里**没有**
> 一条从「import 该分区的单元」到「定义该分区的单元」的边，构建顺序因此不受
> 约束：GCC 与 macOS clang 靠各自的依赖扫描兜住了这一点，**Windows clang 则以
> `failed to read compiled module` 失败**。此前实现分区在 Windows 上无法使用，
> 原因即在于此。

### 类别无法判定的分区

`[scan_overrides."<glob>"]` 只说明一个文件提供哪些模块，却没有地方能说明那条
声明是否带 `export`；P1689 扫描器也可能省略 `is-interface`。无论哪种情况，
源码都会被发布——消费方没有它就编不出 BMI——而 `mcpp pack` 会指出当前属于
这两种情况中的哪一种：

```
warning: secret.cppm provides a module PARTITION and mcpp cannot tell which kind:
         the unit is declared in `[scan_overrides]`, which has nowhere to say
         whether the declaration carries `export`, …
```

> 在 2026.8.18.1 之前，这种情况会得到「它是接口」这个答案——恰恰是**不产生
> 任何警告**的那个答案——于是这样声明的实现分区被一声不响地发布了出去。
> 发布得太少，会让消费方编译失败并点名那个模块；发布得太多，会把私有源码
> 发布出去，而什么都不会失败。未知的情况必须发出声音。

## 验证范围

e2e 套件按宿主能力为每条测试开门，所以「套件是绿的」与「这一条确实跑了」是两句
不同的话。实际跑在哪里：

| 断言 | linux | macOS | windows |
|---|---|---|---|
| 布局、两种接口模式、闭包、两道检查、workspace 根、指名 target、`sources = []`、裸三元组谓词 | yes | | yes |
| 多 target 包，两条 leg 共用**同一个**产物名（`gnu` + `musl`） | yes | *不可能* | — |
| 多 target 包，两条 leg 各自**不同**的产物名（`msvc` + `mingw`） | — | *不可能* | yes |
| 跨越操作系统边界的多 target 包（含一条 PE leg） | yes | — | — |
| `lib.exe /REMOVE:` 确实移除了 | — | — | yes |
| PE 共享库：构建、打包、链接、运行 | (wine) | — | — |
| Mach-O 共享库离开构建树后仍可重定位 | — | yes | — |
| MSVC 因导出原因拒绝 `kind = "shared"` | — | — | yes |
| 一个已发布的 mcpp 消费本版本产出的包 | 仅本机 | 仅本机 | 仅本机 |
| 打包出的 `.so` 不携带构建机的加载器路径，**且把这个缺陷放回去时守卫能看见** | yes | — | — |
| 被 strip 过的静态归档仍可链接；被 strip 过的共享库仍可加载；`--no-strip` / `[pack] strip` / `--debug-symbols` 两侧都测过 | yes | — | — |
| ELF 编辑器在 ELF32 与大端序上的行为 | 单测 | 单测 | 单测 |

*不可能*不是一个缺口：一台 macOS 宿主只能服务一个 target(`host_can_serve`，
`registry.cppm`)，所以两条 leg 的包在那里根本产不出来。

最后一行如实记录了一个真实的空白：每个 CI job 都从一份已发布的 mcpp 自举，但
那个入口是 xvm 的一个 **shim**，在 e2e 套件的环境下它会回答「未安装」。因此
旧客户端检查的**静态那一半**（生成的 manifest 不含任何旧版 mcpp 读不懂的段）
在每个平台上都跑过，而**真实的那一半**——用上一个已发布版本的 mcpp 去构建
这个包——是手工跑的，不是由 CI 跑的。
