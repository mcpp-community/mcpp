# 基于 openkal 的交叉构建

传统的交叉构建由载荷承担。一份工具链为一个目标而构建,它的驱动只有一个答案,
到达第二个目标意味着获取第二份工具链。因此一个发行方必须发布的载荷数,
等于它支持的宿主-目标对数。

openkal 改变了「被交叉的是什么」。目标侧 —— 平台接口、C 库、编译器运行时与
C++ 运行时 —— 成为一组由依赖图解析、并由当前运行的编译器从源码构建的包。
留给编译器的是代码生成,而一个 Clang 二进制发出它被构建时所包含的每一种对象格式。

本文陈述该模型、工程需要书写的内容、生态所供给的内容,以及已被实测的界限。

## 论断

一个由 N 个平台与 M 个架构构成的生态,需要的是一个接口的 N 份实现,
而不是 N×M 份工具链。这个计数来自目标侧所在的位置:一个从源码构建的包,
是为编译器被要求发出的那个目标而构建的,因此一份平台实现只写一次,
就到达编译器支持的每一个架构。

该论断由一个三宿主 × 三目标的矩阵验证,每一格构建同一份源码并运行其结果。

## 工程书写的内容

```toml
[dependencies]
openkal-llvm-runtime = "0.1.1"

[toolchain]
default = "llvm@22.1.8"
```

两行。第一行选定目标侧的三个层;第二行命名一个编译器,
并且对其余一切来自何处只字未提。

目标在命令行上给出:

```bash
mcpp build --target x86_64-linux
mcpp build --target aarch64-macos
mcpp build --target x86_64-windows-gnu
```

宿主目标不需要 `[target.<三元组>]` 段,源码中也不需要任何预处理指令。
可运行的示例见 [examples/06-openkal-cross](../../examples/06-openkal-cross)。

## 生态供给的内容

| 包 | 层 | 内容 |
|---|---|---|
| `openkal` | — | 规范,以及声明它的那些 C++ 模块 |
| `openkal-linux` | `kernel-abi` | 参考实现,建立在 Linux 系统调用之上 |
| `openkal-macos` | `kernel-abi` | 建立在 macOS 的系统调用面之上 |
| `openkal-windows` | `kernel-abi` | 建立在 Win32 与对象管理器之上,不使用任何 C 运行时符号 |
| `openkal-opensbi` | `kernel-abi` | 建立在 RISC-V 的 SBI 之上,无操作系统 |
| `openkal-uefi` | `kernel-abi` | 建立在 UEFI Boot Services 之上,在操作系统存在之前 |
| `openkal-musl` | `c-abi` | 被重定向到 openkal 的 musl,只移植一次 |
| `openkal-llvm-runtime` | `compiler-runtime`、`c++-abi` | compiler-rt builtins、libunwind、libc++abi 与 libc++,为 openkal-musl 配置 |

一个工程命名其中最后一个。其余由它的依赖推出。

## 编译器为何必须是 LLVM

`openkal-llvm-runtime` 把这项要求声明出来,而不是留待被发现:

```toml
requires = ["mcpp:compiler=llvm"]
```

它的源码是 libc++ 的,其中 `std` 模块源尤其由 Clang 编译。
把那份源码交给 GCC,会在 libc++ 自己的头文件深处失败,
其消息命名一个读者从未打开过的文件:

```
fatal error: __config: No such file or directory
```

有了这项声明,构建在编译任何东西之前拒绝该组合,
并指出选择一个满足它的编译器的那条命令。

## 目标如何被选定

mcpp 自身词表的目标行可以携带一条工具链约定。该约定命名的是
**供给该目标 C 库的那份载荷**,并且仅在两个条件同时成立时生效:
清单对该目标未作陈述,且依赖图中没有任何东西供给该目标的系统。

第二个条件只有在图被解析之后才可知。因此一个 C 库来自 `openkal-musl` 的工程
保留它所要求的编译器,而一个没有依赖的工程仍然收到该行所命名的载荷。
两种行为均经实测;提前按任一方向决定,对另一方向都是错的。

## 环境段

在 Linux 上,目标三元组的第三段命名 C 库。在 openkal 之下 C 库来自图,
因此一个命名了 C 库的三元组陈述的是一项图未必兑现的请求:

```
mcpp build --target x86_64-linux-gnu     # 请求 glibc
      c-abi   musl   (openkal-musl@0.3.3, graph)
```

以图为准。省略该段即不作请求,并产出相同的产物:

```
mcpp build --target x86_64-linux
```

该段存在且不一致时,构建报出这一点。它是报出而非拒绝,
因为该段是被忽略而非被违反:两种写法下产物相同。

在 Windows 上,同一段命名的是对象 ABI —— `gnu` 是 PE 加 GNU ABI,
`msvc` 是 PE 加微软的 —— 而两者都与不止一种 C 库相容。
因此该项报出被限定在该段命名 C 库的那些平台上。

## 裸机

一个没有操作系统的目标,是同一个模型,只是平台层由固件而非内核供给。
`riscv64-none-elf` 之上的 OpenSBI 运行与宿主目标相同的源码,含 `import std`,
因为它所使用的标准库是图供给的那份,而不是编译器自带的那份。

有两样东西必须声明,两者都是板子的性质而非默认值:

```toml
[target.riscv64-none-elf]
sysroot = ""
runner  = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
           "-no-reboot", "-bios", "default", "-kernel"]
```

`sysroot = ""` 选定零 libc 档。使用哪个机器模型与哪种固件模式是板子的事实,
而一个去猜测它的引擎,是另一块板子必须与之搏斗的引擎。

一台没有操作系统的 x86_64 机器经由 UEFI 到达。UEFI 应用是 PE/COFF,
经微软 x64 调用约定进入,因此它的目标是 `x86_64-windows-gnu` ——
与一个 Windows 程序相同的三元组 —— 由图解析出的平台实现将二者区分开。

## 已实测的界限

三条,记录在此是因为每一条都由构建而非由阅读发现。

**一个后端必须定义每一个能力字。** 规范的查询是属性对象上的 inline 函数,
因此一个仅仅询问是否存在文件系统的程序,会取 `kal_fs_props` 的地址。
一个省略了自身所缺层的能力字的后端,使该问题恰恰在这个问题为之存在的那类机器上
链接失败。

**同一层的两个供给者是错误而非选择。** C 库、平台接口与 C++ 运行时互斥。
选错不会使链接失败,它产出一个能够运行且间歇性不能运行的程序。

**载荷的 C++ 运行时不能位于外来的 C 库之上。** 它的 `__config_site` 记录了
它被构建时的配置。解析器的结构在默认路径上阻止该组合,
而一条诊断覆盖工程显式覆写该契约的那些路径。

## 参考

[docs/14 — 目标侧](14-target-side.md) 给出五个层、四种来源与规则。
[SPEC-002](../spec/target-side.md) 给出能力语法的规范性陈述。
