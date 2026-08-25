# 目标矩阵:六张表(载荷体系 / openkal 体系 × 三个构建机)

2026-08-26 · 现状测量 + 差异分析(待 review,尚未实施)

取代 [`2026-08-26-target-matrix-should-be-versus-is.md`](2026-08-26-target-matrix-should-be-versus-is.md)
与 [`2026-08-26-the-support-matrix-measured.md`](2026-08-26-the-support-matrix-measured.md)
的表格部分。

---

## 0. 方法、判据、记号

⭐ **入口只有一个:`mcpp build --target <t>`。** 判据取两样:构建报告的原文(五层
来源),以及生成的 `build.ninja` 里 **mcpp 真正下发的 flag**。

⚠️ **不直接问编译器。** 绕开被测对象去问它的组件,得到的是组件的默认行为而不是
mcpp 的行为 —— 前一版文档就这么错过一次。

### 记号

| 记号 | 含义 |
|---|---|
| 无标记 | Linux x86_64 宿主上**真跑过** |
| **推** | 由源码逻辑推出,**无本地测量** |
| `-` | 该轴对这一格无意义 |
| **不支持** | 明确不支持,并给出原因 |

### 两道门是分开的

一格能不能构建,先后过两道**互相独立**的门:

1. **tier 门** —— `tier == "planned"` 直接拒绝(`registered but not…`),与宿主无关
2. **`host_can_serve` 门** —— 这个宿主能否由**载荷**服务这个目标

⚠️ 第 2 道门为假,不等于不能构建 —— 系统还可以来自依赖图。三张 openkal 表就是这一
情形。

### 三个构建机上 `host_can_serve` 的逐格取值

按源码原文求值(纯平台判定,不读机器状态):

| mcpp target | linux/x86_64 | windows/x86_64 | macos/aarch64 |
|---|---|---|---|
| `x86_64-linux-gnu` | ✅ | ❌ | ❌ |
| `x86_64-linux-musl` | ✅ | ✅ | ❌ |
| `aarch64-linux-musl` | ✅ | ❌ | ❌ |
| `x86_64-windows-gnu` | ✅ | ✅ | ❌ |
| `x86_64-windows-musl` | ❌ | ✅ | ❌ |
| `x86_64-windows-msvc` | ❌ | ✅ | ❌ |
| `aarch64-macos` | ❌ | ❌ | ✅ |
| `x86_64-macos` | ❌ | ❌ | ✅ |
| `riscv64-linux-musl` | ✅ | ❌ | ❌ |
| `aarch64-linux-gnu` | ❌ | ❌ | ❌ |
| 四个 `*-none-elf` | ✅ | ✅ | ✅ |

---

# 第一部分:载荷体系(无 openkal 依赖)

## 表 1 — 载荷体系 · 构建机 linux/x86_64 · **应该**

| mcpp target | 目标机 OS | 编译器 | 编译器 target | sysroot | c-abi | c++-abi | openkal | 构建配置特殊说明 |
|---|---|---|---|---|---|---|---|---|
| `x86_64-linux-gnu` | linux | gcc | `x86_64-unknown-linux-gnu` | 载荷自带 | glibc | libstdc++ | - | — |
| `x86_64-linux-gnu` | linux | llvm | `x86_64-unknown-linux-gnu` | `xim:glibc` + `xim:linux-headers` | glibc | libc++ | - | 载荷已装;gcc 分支接了,llvm 分支未接 |
| `x86_64-linux-musl` | linux | gcc | `x86_64-unknown-linux-musl` | 载荷自带 | musl | libstdc++ | - | 驱动 `x86_64-linux-musl-g++` |
| `x86_64-linux-musl` | linux | llvm | `x86_64-unknown-linux-musl` | 需 musl sysroot | musl | libc++ | - | — |
| `aarch64-linux-musl` | linux | gcc | `aarch64-unknown-linux-musl` | 载荷自带 | musl | libstdc++ | - | 交叉 |
| `aarch64-linux-musl` | linux | llvm | `aarch64-unknown-linux-musl` | 需 musl sysroot | musl | libc++ | - | 交叉 |
| `x86_64-windows-gnu` | windows | gcc | `x86_64-w64-windows-gnu` | 载荷自带 | gnu(MinGW CRT) | libstdc++ | - | 包名 `mingw-cross-gcc` |
| `x86_64-windows-gnu` | windows | llvm | `x86_64-w64-windows-gnu` | 需 MinGW sysroot | gnu | libc++ | - | — |
| `riscv64-none-elf` | 无 | llvm | `riscv64-none-elf` | `xim:picolibc-riscv@1.8.12` | picolibc | 无 | - | `-march/-mabi/-mcmodel` 由 freestanding 表定 |
| `riscv32-none-elf` | 无 | llvm | `riscv32-none-elf` | `xim:picolibc-riscv@1.8.12` | picolibc | 无 | - | 同上 |
| `aarch64-none-elf` | 无 | llvm | `aarch64-none-elf` | 无(表里为空) | 无 | 无 | - | preview;须 `sysroot = ""` |
| `x86_64-none-elf` | 无 | llvm | `x86_64-none-elf` | 无 | 无 | 无 | - | preview;须 `sysroot = ""` |
| 四个 `*-none-elf` | 无 | gcc | - | - | - | - | - | **不支持**:宿主 g++ 发不出这些目标 |
| `x86_64-windows-musl` | windows | - | - | - | - | - | - | **不支持**:无此载荷,须走 openkal |
| `x86_64-windows-msvc` | windows | - | - | - | - | - | - | **不支持**:MSVC 只在 windows 宿主 |
| `aarch64-macos` | macos | - | - | - | - | - | - | **不支持**:macOS SDK 只在 macos 宿主 |
| `riscv64-linux-musl` | linux | - | - | - | - | - | - | **不支持**:tier=planned |
| `aarch64-linux-gnu` | linux | - | - | - | - | - | - | **不支持**:tier=planned |
| `x86_64-macos` | macos | - | - | - | - | - | - | **不支持**:tier=planned |

## 表 2 — 载荷体系 · 构建机 linux/x86_64 · **现在**(全部实测)

| mcpp target | 目标机 OS | 编译器 | 编译器 target | sysroot | c-abi | c++-abi | openkal | 构建配置特殊说明 |
|---|---|---|---|---|---|---|---|---|
| `x86_64-linux-gnu` | linux | gcc | `x86_64-unknown-linux-gnu` | 载荷自带 | gnu | libstdc++ | - | ✅ |
| `x86_64-linux-gnu` | linux | llvm | `x86_64-unknown-linux-gnu` | **无** | gnu | — | - | ❌ **A**:`Scrt1.o (outside the sandbox)` |
| `x86_64-linux-musl` | linux | gcc | `x86_64-unknown-linux-musl` | 载荷自带 | musl | libstdc++ | - | ✅ |
| `x86_64-linux-musl` | linux | llvm | `x86_64-unknown-linux-musl` | **无** | musl | — | - | ❌ **A** |
| `aarch64-linux-musl` | linux | gcc | `aarch64-unknown-linux-musl` | 载荷自带 | musl | libstdc++ | - | ✅ |
| `aarch64-linux-musl` | linux | llvm | `aarch64-unknown-linux-musl` | **无** | musl | — | - | ❌ **A** |
| `x86_64-windows-gnu` | windows | gcc | `x86_64-w64-windows-gnu` | 载荷自带 | gnu | libstdc++ | - | ✅ `ldflags = -lstdc++exp` |
| `x86_64-windows-gnu` | windows | llvm | `x86_64-w64-windows-gnu` | **无** | gnu | — | - | ❌ **B**:`ldflags` 为空 |
| `riscv64-none-elf` | 无 | llvm | — | `sysroot=""` | 无 | 无 | - | ✅ 产出 RISC-V ELF |
| `riscv64-none-elf` | 无 | llvm | — | **表里的 picolibc** | picolibc | 无 | - | ❌ **C**:干净环境不安装(#510) |
| `riscv32-none-elf` | 无 | llvm | — | `sysroot=""` | 无 | 无 | - | ✅ |
| `aarch64-none-elf` | 无 | llvm | — | `sysroot=""` | 无 | 无 | - | ✅ |
| `x86_64-none-elf` | 无 | llvm | — | `sysroot=""` | 无 | 无 | - | ✅ |
| 四个 `*-none-elf` | 无 | gcc | - | - | - | - | - | ❌ **D**:g++ 收到 `-mabi=lp64d` |
| `x86_64-windows-musl` | windows | 两者 | - | - | - | - | - | ✅ 拒绝 `cannot be built on this host` |
| `x86_64-windows-msvc` | windows | 两者 | - | - | - | - | - | ✅ 拒绝 |
| `aarch64-macos` | macos | 两者 | - | - | - | - | - | ✅ 拒绝 |
| `riscv64-linux-musl` | linux | 两者 | - | - | - | - | - | ✅ `registered but not…` |
| `aarch64-linux-gnu` | linux | 两者 | - | - | - | - | - | ✅ `registered but not…` |
| `x86_64-macos` | macos | 两者 | - | - | - | - | - | ✅ `registered but not…` |

## 表 3 — 载荷体系 · 构建机 windows/x86_64 与 macos/aarch64 · **现在(推)**

⚠️ **本表全部为源码推导,无任何测量。**

| 构建机 | mcpp target | 目标机 OS | 编译器 | 编译器 target | sysroot | c-abi | c++-abi | openkal | 构建配置特殊说明 |
|---|---|---|---|---|---|---|---|---|---|
| windows | `x86_64-windows-msvc` | windows | msvc | `x86_64-pc-windows-msvc` 推 | Windows SDK 推 | ucrt 推 | MSVC STL 推 | - | 需本机 VS;`payload_libc_name("windows","")`=ucrt |
| windows | `x86_64-windows-gnu` | windows | gcc | `x86_64-w64-windows-gnu` 推 | 载荷自带 推 | gnu 推 | libstdc++ 推 | - | 包名 **`mingw-gcc`**(非 cross) |
| windows | `x86_64-windows-musl` | windows | llvm | `x86_64-w64-windows-gnu` 推 | ⚠️ 无载荷 | — | — | - | ⚠️ `host_can_serve` 为真而载荷不存在,**存疑** |
| windows | `x86_64-linux-musl` | linux | gcc | `x86_64-unknown-linux-musl` 推 | 载荷自带 推 | musl 推 | libstdc++ 推 | - | 包名 `x86_64-linux-musl-gcc` |
| windows | 四个 `*-none-elf` | 无 | llvm | 各自 推 | 同表 1 | 同表 1 | 无 | - | — |
| windows | 其余目标 | — | - | - | - | - | - | - | **不支持**(`host_can_serve` 为假或 planned) |
| macos | `aarch64-macos` | macos | llvm | `arm64-apple-macos14.0` 推 | macOS SDK 推 | **libSystem** 推 | libc++ 推 | - | `-isysroot` + `-mmacosx-version-min` |
| macos | `x86_64-macos` | macos | - | - | - | - | - | - | **不支持**:tier=planned |
| macos | 四个 `*-none-elf` | 无 | llvm | 各自 推 | 同表 1 | 同表 1 | 无 | - | — |
| macos | 其余目标 | — | - | - | - | - | - | - | **不支持**:`host_can_serve` 全为假 |

---

# 第二部分:openkal 体系(`openkal-musl` + `openkal-llvm-runtime`)

⚠️ **前置:编译器必须是 llvm。** `openkal-llvm-runtime` **就是** libc++/libc++abi/
libunwind,用 gcc 构建它不是一件存在的事;包用 `requires` 声明了这一点,mcpp 在编译
前拒绝。**gcc × openkal 在三个构建机上一律不支持** —— 这是生态空缺(缺
`openkal-gcc-runtime`),不是引擎缺陷。

## 表 4 — openkal 体系 · 构建机 linux/x86_64 · **应该**

| mcpp target | 目标机 OS | 编译器 | 编译器 target | sysroot | c-abi | c++-abi | openkal | 构建配置特殊说明 |
|---|---|---|---|---|---|---|---|---|
| `x86_64-linux-gnu` | linux | llvm | `x86_64-unknown-linux-gnu` | 图供给 | musl(图) | libc++(图) | openkal-linux | 三层全来自图 |
| `x86_64-linux-musl` | linux | llvm | `x86_64-unknown-linux-musl` | 图供给 | musl(图) | libc++(图) | openkal-linux | — |
| `aarch64-linux-musl` | linux | llvm | `aarch64-unknown-linux-musl` | 图供给 | musl(图) | libc++(图) | openkal-linux | 交叉 |
| `x86_64-windows-gnu` | windows | llvm | `x86_64-w64-windows-gnu` | 图供给 | **musl**(图) | libc++(图) | openkal-windows | ⚠️ c-abi 与载荷体系**不同** |
| `x86_64-windows-musl` | windows | llvm | `x86_64-w64-windows-gnu` | 图供给 | musl(图) | libc++(图) | openkal-windows | 与上一行同一 LLVM 三元组 |
| `aarch64-macos` | macos | llvm | `arm64-apple-macos14.0` | 图供给 + **平台 SDK** | musl(图) | libc++(图) | openkal-macos | ⚠️ Darwin 的内核接口是 libSystem,`-isysroot` 必须存活 |
| `riscv64-none-elf` | 无 | llvm | `riscv64-none-elf` | 图供给 | musl(图) | libc++(图) | openkal-opensbi | — |
| `x86_64-windows-msvc` | windows | - | - | - | - | - | - | **不支持**:MSVC ABI 与图不兼容 |
| 三个 planned 目标 | — | - | - | - | - | - | - | **不支持**:tier=planned |
| 任意目标 | — | gcc | - | - | - | - | - | **不支持**:`requires` 拒绝 gcc |

## 表 5 — openkal 体系 · 构建机 linux/x86_64 · **现在**(全部实测)

| mcpp target | 目标机 OS | 编译器 | 编译器 target | sysroot | c-abi | c++-abi | openkal | 构建配置特殊说明 |
|---|---|---|---|---|---|---|---|---|
| `x86_64-linux-gnu` | linux | llvm | `x86_64-unknown-linux-gnu` | 图供给 | musl(图) | libc++(图) | openkal-linux@0.5.4 | ✅ |
| `x86_64-linux-musl` | linux | llvm | `x86_64-unknown-linux-musl` | 图供给 | musl(图) | libc++(图) | openkal-linux@0.5.4 | ✅ |
| `aarch64-linux-musl` | linux | llvm | `aarch64-unknown-linux-musl` | 图供给 | musl(图) | libc++(图) | openkal-linux@0.5.4 | ✅ |
| `x86_64-windows-gnu` | windows | llvm | `x86_64-w64-windows-gnu` | 图供给 | musl(图) | libc++(图) | openkal-windows@0.1.5 | ✅ **c-abi 由 gnu 变 musl** |
| `x86_64-windows-musl` | windows | llvm | `x86_64-w64-windows-gnu` | 图供给 | musl(图) | libc++(图) | openkal-windows@0.1.5 | ✅ 产出 PE32+ |
| `aarch64-macos` | macos | llvm | `arm64-apple-macos14.0` | 图供给 | musl(图) | libc++(图) | openkal-macos@0.3.4 | ✅ **linux→macOS 交叉成立** |
| `riscv64-none-elf` | 无 | llvm | — | 图供给 | musl(图) | libc++(图) | — | ✅ |
| `x86_64-windows-msvc` | windows | llvm | - | - | - | - | - | ✅ 拒绝(需 MSVC ABI) |
| 三个 planned 目标 | — | llvm | - | - | - | - | - | ✅ `registered but not…` |
| 全部 14 个 | — | **gcc** | - | - | - | - | - | ✅ `requires the compiler family to be llvm` |

## 表 6 — openkal 体系 · 构建机 windows/x86_64 与 macos/aarch64 · **现在(推)**

⚠️ **本表全部为源码推导,无任何测量。** 但有一条**间接证据**:`openkal-cross.yml` 的
3 宿主 × 3 目标矩阵在这两个宿主上是绿的(目标为 `x86_64-linux-gnu`、`aarch64-macos`、
`x86_64-windows-gnu`)。

| 构建机 | mcpp target | 目标机 OS | 编译器 | 编译器 target | sysroot | c-abi | c++-abi | openkal | 构建配置特殊说明 |
|---|---|---|---|---|---|---|---|---|---|
| windows | `x86_64-linux-gnu` | linux | llvm | `x86_64-unknown-linux-gnu` 推 | 图供给 推 | musl(图)推 | libc++(图)推 | openkal-linux | CI 绿(间接) |
| windows | `x86_64-windows-gnu` | windows | llvm | `x86_64-w64-windows-gnu` 推 | 图供给 推 | musl(图)推 | libc++(图)推 | openkal-windows | CI 绿(间接) |
| windows | `aarch64-macos` | macos | llvm | `arm64-apple-macos14.0` 推 | 图供给 推 | musl(图)推 | libc++(图)推 | openkal-macos | CI 绿(间接) |
| windows | `x86_64-windows-msvc` | windows | - | - | - | - | - | - | **不支持**:MSVC ABI 与图不兼容 |
| windows | 四个 `*-none-elf` | 无 | llvm | 各自 推 | 图供给 推 | musl(图)推 | libc++(图)推 | opensbi/uefi | — |
| windows | 三个 planned | — | - | - | - | - | - | - | **不支持**:tier=planned |
| windows | 任意 | — | gcc | - | - | - | - | - | **不支持**:`requires` |
| macos | `x86_64-linux-gnu` | linux | llvm | `x86_64-unknown-linux-gnu` 推 | 图供给 推 | musl(图)推 | libc++(图)推 | openkal-linux | CI 绿(间接) |
| macos | `aarch64-macos` | macos | llvm | `arm64-apple-macos14.0` 推 | 图供给 + SDK 推 | musl(图)推 | libc++(图)推 | openkal-macos | ⚠️ 本机 macOS + 图,`-isysroot` 路径 |
| macos | `x86_64-windows-gnu` | windows | llvm | `x86_64-w64-windows-gnu` 推 | 图供给 推 | musl(图)推 | libc++(图)推 | openkal-windows | CI 绿(间接) |
| macos | 四个 `*-none-elf` | 无 | llvm | 各自 推 | 图供给 推 | musl(图)推 | libc++(图)推 | opensbi/uefi | — |
| macos | `x86_64-windows-msvc` / 三个 planned | — | - | - | - | - | - | - | **不支持** |
| macos | 任意 | — | gcc | - | - | - | - | - | **不支持**:`requires` |

---

## 差异行

**载荷体系(表 1 vs 表 2)四行不符;openkal 体系(表 4 vs 表 5)全部一致。**

| 差异 | 表 1 说应该 | 表 2 实测 | 影响 |
|---|---|---|---|
| **A** | llvm × 三个 linux 目标:`xim:glibc`/musl sysroot 就位 | sysroot **无**,clang 找宿主的,hermetic 拒绝 | **llvm 在 hosted linux 目标上不可用** |
| **B** | llvm × windows-gnu:MinGW sysroot 就位 | `ldflags` 为空,无 `-B/-L/--sysroot` | **不可用,且诊断无关**(`unknown file type`) |
| **C** | 目标行的 picolibc 就位 | 声明而不安装(#510) | **干净环境下裸机不可用** |
| **D** | 裸机行的 pin 是能力陈述,gcc 不该接手 | 用户显式声明 gcc 时无条件让位 | 诊断指向 `-mabi` 而非决定 |

### A 与 B 同源,而 A 有一处关键事实

两者都是:**clang 靠 `--target=` 切目标,目标 sysroot 必须由外部给,mcpp 没给。**
gcc 不需要 —— 它一个目标一份载荷,驱动自带 sysroot(实测存在
`xim-x-mingw-cross-gcc/16.1.0/x86_64-w64-mingw32/`)。

⭐⭐ **A 不是「缺少 sysroot」,是「同一份东西 gcc 接上了而 llvm 没接」。** 实测两条
链接线:

```
gcc  × x86_64-linux-gnu   ldflags:
  -Wl,--dynamic-linker=…/xim-x-glibc/2.44/lib64/ld-linux-x86-64.so.2
  -L…/xim-x-glibc/2.44/lib64
  -Wl,-rpath,…/xim-x-glibc/2.44/lib64
  -B…/xim-x-binutils/2.42/bin

llvm × x86_64-linux-gnu   ldflags:
  --target=x86_64-unknown-linux-gnu --no-default-config -fuse-ld=lld
  -L…/xim-x-llvm/22.1.8/lib/x86_64-unknown-linux-gnu
  -L…/xim-x-gcc-runtime/15.1.0/lib64
  (没有任何 xim-x-glibc)
```

而缺的正是 glibc 的启动对象,**它们就在那份载荷里**:

```
$ ls …/xim-x-glibc/2.44/lib/ | grep -E '^(Scrt1|crti|crtn)\.o$'
crti.o  crtn.o  Scrt1.o
```

⭐ 所以 A 的一句话是:**`xim:glibc` 装了、gcc 的链接线接了、llvm 的没接。** 缺的不
是载荷,是 llvm 分支上对应的 `-L`/`-B`/`--dynamic-linker`。这比缺一份载荷容易修得
多,也把 A 的优先级提到 B 之前。

⚠️ **hermetic 检查对 A 报而对 B 不报,未查清。未实测。**

---

## 表 3 与表 6 的「推」如何消除

⭐ **把表 2 与表 5 的扫描做成脚本,在三个构建机的 CI 上各跑一次,产出同一张表。**
那样矩阵就不再是一份会悄悄过期的文档,而是一次测量的输出。

⚠️ 表 3 里有一格现在就存疑,**须优先测**:windows 宿主 × `x86_64-windows-musl`
—— `host_can_serve` 返回真(`is_windows` 分支),而这个目标**没有任何载荷**,它的
系统只能来自图。若真如此,windows 宿主上无依赖地请求它会走到与 B 相同的形态。

---

## 建议顺序

1. **A** —— `xim:glibc` 已在,查为何没接上。收益最大、形态最清楚。
2. **B** —— 与 A 同源;并查 hermetic 为何不报。
3. **表 3 的存疑格**(windows × windows-musl),它可能是 B 的第二个实例。
4. **C**(#510)、**D** —— 已有判据,可独立进行。
5. **脚本化 + 三宿主 CI**,消除表 3 与表 6 的全部「推」。
