# 支持矩阵:56 格实测,四处待修、一处生态空缺

2026-08-26 · 现状测量 + 问题分析(待 review,尚未实施)

前置:[`2026-08-25-the-two-layer-predicate-family.md`](2026-08-25-the-two-layer-predicate-family.md)、
[`2026-08-26-declared-but-not-made-to-exist.md`](2026-08-26-declared-but-not-made-to-exist.md)

⚠️ **全部为实测,不是从代码推的。** 每一格都真跑过一次 `mcpp build --target …`,
取的是构建报告的原文。

⚠️ **宿主只有一台 Linux x86_64。** macOS 与 Windows 宿主那两列是空的 —— 本文不对
它们下任何结论,标注为**未测**。

---

## 0. 一句话

> **矩阵不是「目标 × 编译器」的二维,是「目标 × 编译器 × 系统由谁供给」的三维,
> 而第三维决定前两维的含义。**

同一个 `x86_64-windows-gnu`,C 库可以是 MinGW CRT 也可以是 musl;同一个
`riscv64-none-elf`,编译器可以是 clang 也可以是(错误地)宿主 g++。

---

## 1. 测量方法

- 宿主:Linux x86_64,`mcpp 2026.8.25.2`
- 工程:一个 `main.cpp`。⚠️ **裸机目标必须换探针** —— `<cstdio>` 是 C++ 头,
  而 `sysroot = ""` 的层没有 C++ 标准库;那四格用
  `extern "C" int main(int, char**, char**)`
- 两种模式:
  - **payload** —— 无依赖,系统来自所选编译器的载荷
  - **graph** —— `openkal-musl@0.3.5` + `openkal-llvm-runtime@0.1.3`
- 编译器:`gcc@16.1.0`、`llvm@22.1.8`(显式写进 `[toolchain] default`)
- 目标:`kKnownTargets` 全部 14 行
- 裸机目标补 `[target.<t>] sysroot = ""`

14 × 2 × 2 = **56 格**。

### 轴的完整定义

| 轴 | 本轮取值 |
|---|---|
| **构建机(宿主 OS × arch)** | Linux x86_64 —— 只有一台,见 §5 |
| **mcpp 目标** | `kKnownTargets` 全部 14 行 |
| **编译器族** | gcc、llvm(msvc 需 Windows 宿主) |
| **编译器三元组** | 由 mcpp 解析,是**结果**不是输入 |
| **系统来源** | payload / graph |
| c-abi / c++-abi | 由上面五轴决定,是**结果** |

⚠️ **判据一律取自 `mcpp build` 这个入口**:构建报告的原文,以及生成的
`build.ninja` 里 mcpp 真正下发的 flag。**不直接问编译器** —— 绕开被测对象去问它的
组件,得到的是组件的默认行为而非 mcpp 的行为(本文 §4.② 初稿就是这么错的)。

---

## 2. payload 模式(系统来自编译器载荷)

| mcpp 目标 | 编译器 | 实际驱动 | 编译器三元组 | c-abi | c++-abi |
|---|---|---|---|---|---|
| `x86_64-linux-gnu` | gcc | `xim-x-gcc/…/g++` | `x86_64-unknown-linux-gnu` | gnu | libstdc++ |
| `x86_64-linux-musl` | gcc | `xim-x-musl-gcc/…/x86_64-linux-musl-g++` | `x86_64-unknown-linux-musl` | musl | libstdc++ |
| `aarch64-linux-musl` | gcc | `xim-x-aarch64-linux-musl-gcc/…` | `aarch64-unknown-linux-musl` | musl | libstdc++ |
| `x86_64-windows-gnu` | gcc | `xim-x-mingw-cross-gcc/…/x86_64-w64-mingw32-g++` | `x86_64-w64-windows-gnu` | gnu | libstdc++ |
| `x86_64-linux-gnu` | llvm | `xim-x-llvm/…/clang++` | `x86_64-unknown-linux-gnu` | gnu | ⚠️ **①** |
| `x86_64-linux-musl` | llvm | `xim-x-llvm/…/clang++` | `x86_64-unknown-linux-musl` | musl | ⚠️ **①** |
| `aarch64-linux-musl` | llvm | `xim-x-llvm/…/clang++` | `aarch64-unknown-linux-musl` | musl | ⚠️ **①** |
| `x86_64-windows-gnu` | llvm | `xim-x-llvm/…/clang++` | `x86_64-w64-windows-gnu` | gnu | ⚠️ **②** |
| 四个 `*-none-elf` | gcc | `xim-x-gcc/…/g++` | — | — | ⚠️ **③** |
| 四个 `*-none-elf` | llvm | `xim-x-llvm/…/clang++` | — | — | ✅ 全过(④) |
| `x86_64-windows-musl` | 两者 | — | — | — | 拒绝(见 §4.1) |
| `x86_64-windows-msvc` | 两者 | — | — | — | 拒绝(宿主无 MSVC,正确) |
| `aarch64-macos` | 两者 | — | — | — | 拒绝(宿主无 SDK,正确) |
| `aarch64-linux-gnu` / `x86_64-macos` / `riscv64-linux-musl` | 两者 | — | — | — | `registered but not…`(tier=planned,正确) |

⭐ **gcc 一个目标一份载荷,clang 一份载荷打所有目标。** 这是两种体系,而不是同一
件事的两种写法:gcc 的驱动带三元组前缀(`x86_64-linux-musl-g++`),clang 始终是同
一个 `clang++` 加 `--target=`。

---

## 3. graph 模式(系统来自 openkal)

| mcpp 目标 | 编译器 | 编译器三元组 | kernel-abi | c-abi | c++-abi |
|---|---|---|---|---|---|
| `x86_64-linux-gnu` | llvm | `x86_64-unknown-linux-gnu` | openkal (openkal-linux) | musl | libc++ |
| `x86_64-linux-musl` | llvm | `x86_64-unknown-linux-musl` | openkal (openkal-linux) | musl | libc++ |
| `aarch64-linux-musl` | llvm | `aarch64-unknown-linux-musl` | openkal (openkal-linux) | musl | libc++ |
| `aarch64-macos` | llvm | `arm64-apple-macos14.0` | openkal (openkal-macos) | musl | libc++ |
| `x86_64-windows-gnu` | llvm | `x86_64-w64-windows-gnu` | openkal (openkal-windows) | musl | libc++ |
| `x86_64-windows-musl` | llvm | `x86_64-w64-windows-gnu` | openkal (openkal-windows) | musl | libc++ |
| `riscv64-none-elf` | llvm | — | — | musl | libc++ |
| **全部 14 个** | **gcc** | — | — | — | ⚠️ **⑤** |

⭐⭐ **看 `x86_64-windows-gnu` 在两张表里的 c-abi:payload 下是 `gnu`(MinGW CRT),
graph 下是 `musl`。** 一个目标名,两个 C 库。这正是 `2026.8.24.6` 给
`x86_64-windows-musl` 单独取名的理由 —— 同一个 LLVM 三元组
(`x86_64-w64-windows-gnu`,LLVM 拼不出 windows-musl),产物差 16.7 倍、依赖的 DLL
完全不同。

---

## 4. 逐处分析

### ① llvm × linux-*:C 运行时落到宿主(已被拦住)

```
error: hermetic link check failed — the sandbox toolchain resolves its C runtime
       outside the sandbox:
         /lib/x86_64-linux-gnu/Scrt1.o (outside the sandbox)
         /usr/lib/gcc/x86_64-linux-gnu/13/crtbeginS.o (outside the sandbox)
```

**这不是缺陷,是守卫在工作** —— clang 载荷不自带 glibc 的启动对象,mcpp 也没为这
个组合准备 sysroot,于是 clang 找到宿主的,而密闭性检查拒绝了。

⚠️ **但拒绝之后没有出路。** 消息说了「解析到沙箱外」,没说「怎样才能不解析到沙箱
外」。用 gcc 就能构建同一个目标,而消息里没有这句话。

**建议**:该拒绝在同一目标存在可用编译器时,附一句指出它。判据两向:**没有可用
替代时不得出现这句话**。

### ② llvm × windows-gnu:mcpp 没有为这个组合提供目标 sysroot

⚠️ **本条初稿的测量方式是错的。** 它直接跑
`clang++ --target=… -print-search-dirs` 看到 `/usr/x86_64-w64-mingw32/lib`,并据此
说「CRT 来自宿主」。**那不是 mcpp 的构建线** —— 绕开被测对象去问它的一个组件,得到
的是那个组件的默认行为,不是 mcpp 让它做了什么。

⭐ **正确的判据是 mcpp 真正下发的命令行**,即生成的 `build.ninja`:

| | gcc × windows-gnu | llvm × windows-gnu |
|---|---|---|
| `cxxflags` | `-std=c++23 -fmodules -O0 -g`(**无 `--target`**) | `--target=x86_64-w64-windows-gnu` |
| `ldflags` | `-lstdc++exp` | **空** |
| 目标 sysroot | **驱动自带**:`xim-x-mingw-cross-gcc/16.1.0/x86_64-w64-mingw32/` | **无人提供** |

gcc 那格不需要 mcpp 说任何话:`x86_64-w64-mingw32-g++` 是**为这个目标构建的交叉
编译器**,sysroot 在它自己的载荷里。llvm 那格是同一个 `clang++` 靠 `--target=` 切
目标,**头与库必须由外部给**,而 mcpp 给的是空 `ldflags`、没有 `-B`/`-L`/`--sysroot`。

构建最终失败于 `ld.lld: error: obj/main.o: unknown file type`,与真正的问题无关。

⚠️ **仍未查清:** 密闭性检查对 ① 报了而对 ② 没报。**未实测。**

**这一格是本文最需要先查清的。** 它当前既不能用,也没有一条说清楚为什么的诊断。

### ③ gcc × 裸机:宿主 g++ 收到 clang 专用 flag

```
g++: error: unrecognized argument in option '-mabi=lp64d'
g++: note: valid arguments to '-mabi=' are: ms sysv
```

目标行 pin 是 `llvm@22.1.8`,而**用户显式写了 `[toolchain] default = "gcc"` 就让位**
—— 这是既有设计(用户显式声明优先于行的约定)。

⚠️ **但让位之后没有检查这个编译器能否发出这个目标。** 裸机行的 pin 是**能力陈述**
而非约定(见前文第五条),让位给一个发不出该目标的编译器,应当在**决定处**拒绝,
而不是让 g++ 去报 `-mabi` 不认识。

**建议**:`targetPinIsCapability` 为真时,用户显式声明也要过一道「这个编译器能发出
这个目标吗」。⚠️ 判据必须两向:**能发出的组合不得被拒**。

### ④ llvm × 裸机:四格全部通过(初测是我的探针错)

重测后:

```
llvm@22.1.8  riscv64-none-elf   ✅ ELF 64-bit LSB executable, UCB RISC-V, RVC
llvm@22.1.8  riscv32-none-elf   ✅ ELF 32-bit LSB executable, UCB RISC-V, RVC
llvm@22.1.8  aarch64-none-elf   ✅ ELF 64-bit LSB executable, ARM aarch64
llvm@22.1.8  x86_64-none-elf    ✅ ELF 64-bit LSB executable, x86-64
```

⚠️ **初测的四个失败是探针的问题,不是 mcpp 的。** 那一版用 `#include <cstdio>`
—— 一个 C++ 头 —— 而 `sysroot = ""` 的裸机层没有 C++ 标准库。换成
`extern "C" int main(int, char**, char**)` 后四格全过。

⭐ **这与 issue #510 是两件事,不要混。** #510 是 `sysroot` 被声明而不被安装,在
**没有 `sysroot = ""` 覆盖**、由目标行提供 picolibc 的干净环境里出现;这四格显式
写了 `sysroot = ""`,即「本目标不要 C 库」,mcpp 照做且正确。

⚠️ 本文初稿把这两件事混成了一条 —— 记录在此,因为混淆的方式值得记:**两个失败发生
在同一个目标上,而原因不同**,`stdio.h not found`(库没装)与 `cstdio not found`
(层不存在)只差一个字母。

### ⑤ gcc × graph:被规范包正确拒绝

```
`openkal-llvm-runtime@0.1.3` requires the compiler family to be llvm
```

**这不是缺陷。** openkal-llvm-runtime **就是** libc++/libc++abi/libunwind,用 gcc 构建
它不是一件存在的事,包声明了这一点,mcpp 在编译前就拒绝了 —— 这正是 `requires` 机
制该有的行为。

⚠️ 但它意味着 **graph 模式下 gcc 这一整列都不可用**。若要让 gcc 也能用 openkal,需
要一个 gcc 的 C++ 运行时实现包(`openkal-gcc-runtime`),**目前不存在**。这是生态的
空缺,不是引擎的缺陷。

---

## 5. 矩阵的空白

| 轴 | 已测 | 未测 |
|---|---|---|
| 宿主 OS | Linux x86_64 | **macOS、Windows** |
| 编译器 | gcc、llvm | **msvc**(需 Windows 宿主) |
| 目标 | 14 行全部 | — |
| 系统来源 | payload、graph | — |

⚠️ **macOS 与 Windows 宿主那两列没有任何本地测量。** CI 覆盖到其中一部分
(`openkal-cross.yml` 的 3 宿主 × 3 目标、`ci-windows*`),但**不是这张矩阵的形状**。

⭐ **建议:把这张扫描做成一条可重复运行的脚本,在三个宿主的 CI 上各跑一次,产出同
一张表。** 那样「支持矩阵」就不再是一份文档,而是一次测量的输出 —— 与它描述的东西
同步,不会像本文这样在下一次改动后悄悄过期。

---

## 6. 建议的执行顺序

1. **查清 ②**:密闭性检查为何不报;`clang++.cfg` 的 Linux 加载器是否泄漏到 PE 目
   标;mcpp 是否本该为 llvm × windows-gnu 提供 sysroot。这一格现在既不可用也没有
   可读的诊断。
2. **③ 的能力检查**:让位给用户声明之后仍需确认「这个编译器能发出这个目标」。
3. **① 的出路**:拒绝时指出可用的替代编译器,两向判据。
4. **issue #510**(目标行 sysroot 声明而不安装)—— 与 ④ 是两件事,判据是删掉裸机
   CI 那两行手工安装。
5. **把扫描脚本化并接进三宿主 CI**(§5)。
6. **⑤ 记为生态空缺**,不作为引擎问题跟踪。
