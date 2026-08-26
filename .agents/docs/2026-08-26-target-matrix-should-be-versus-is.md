# 目标矩阵:应该是什么,现在是什么,差在哪

2026-08-26 · 现状测量 + 差异分析(待 review,尚未实施)

取代 [`2026-08-26-the-support-matrix-measured.md`](2026-08-26-the-support-matrix-measured.md)
的表格部分,补齐它缺的轴(构建机、目标机、sysroot、构建配置)。

---

## 0. 方法与判据

⭐ **入口只有一个:`mcpp build --target <t>`。** 判据取两样东西:

1. 构建报告的原文(五层来源)
2. 生成的 `build.ninja` 里 **mcpp 真正下发的 flag**

⚠️ **不直接问编译器。** 绕开被测对象去问它的一个组件,得到的是那个组件的默认行为,
不是 mcpp 让它做了什么 —— 上一版文档的第 ② 条就是这么错的,那里跑了
`clang++ -print-search-dirs` 并据此说「CRT 来自宿主」。

### 表格的填法

| 来源 | 标记 |
|---|---|
| 本机(Linux x86_64)真跑过 | 无标记 |
| 由源码逻辑推出(macOS / Windows 宿主) | **推** |
| 该轴对这一格无意义 | `-` |
| 明确不支持 | **不支持** |

推导依据是三个纯平台函数,它们不读任何机器状态:

- `host_can_serve(target)` —— 宿主能否由载荷服务这个目标
- `to_xim_package(spec)` —— 哪个 xim 包被选中
- `payload_libc_name(os, env)` + `resolve()` 的四分支 —— c-abi 从哪来

---

## 1. 表一:理论上应该是什么

「应该」= 目标表的 tier 与 pin 所承诺的、且技术上成立的。

| # | mcpp target | 构建机 OS | 目标机 OS | 编译器 | 编译器 target | sysroot | c-abi | c++-abi | openkal | 构建配置特殊说明 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `x86_64-linux-gnu` | linux | linux | gcc | `x86_64-unknown-linux-gnu` | 载荷自带 | glibc | libstdc++ | 可替换全部三层 | — |
| 2 | `x86_64-linux-gnu` | linux | linux | llvm | `x86_64-unknown-linux-gnu` | **需 mcpp 提供** | glibc | libc++ | 可替换 | ⚠️ clang 不自带 glibc 启动对象 |
| 3 | `x86_64-linux-musl` | linux / windows | linux | gcc | `x86_64-unknown-linux-musl` | 载荷自带 | musl | libstdc++ | 可替换 | — |
| 4 | `x86_64-linux-musl` | 任意 | linux | llvm | `x86_64-unknown-linux-musl` | **需 mcpp 提供** | musl | libc++ | 可替换 | — |
| 5 | `aarch64-linux-musl` | linux / windows(同 arch) | linux | gcc | `aarch64-unknown-linux-musl` | 载荷自带 | musl | libstdc++ | 可替换 | 交叉 |
| 6 | `x86_64-windows-gnu` | linux / windows | windows | gcc | `x86_64-w64-windows-gnu` | 载荷自带 | gnu(MinGW CRT) | libstdc++ | 可替换 | 宿主分包:win→`mingw-gcc`,其他→`mingw-cross-gcc` |
| 7 | `x86_64-windows-gnu` | linux / windows | windows | llvm | `x86_64-w64-windows-gnu` | **需 mcpp 提供** | gnu | libc++ | 可替换 | ⚠️ clang 无 MinGW sysroot |
| 8 | `x86_64-windows-musl` | 任意 | windows | llvm | `x86_64-w64-windows-gnu` | **只能来自图** | musl | libc++ | **必需** | ⚠️ 与 #7 同一 LLVM 三元组;tier=preview |
| 9 | `x86_64-windows-msvc` | **windows** | windows | msvc | `x86_64-pc-windows-msvc` | Windows SDK | ucrt | MSVC STL | 不适用 | 需本机 Visual Studio |
| 10 | `aarch64-macos` | **macos** | macos | llvm | `arm64-apple-macos14.0` | macOS SDK | libSystem | libc++ | 可替换 | 需 `-isysroot`;`-mmacosx-version-min` |
| 11 | `aarch64-macos` | 任意 | macos | llvm | `arm64-apple-macos14.0` | **只能来自图** | musl | libc++ | **必需** | 交叉到 macOS,kernel-abi=openkal-macos |
| 12 | `riscv64-none-elf` | 任意 | 无 | llvm | `riscv64-none-elf` | `xim:picolibc-riscv@1.8.12` | picolibc | 无(freestanding 子集) | 可替换 | `-march/-mabi/-mcmodel` 由 freestanding 表定 |
| 13 | `riscv32-none-elf` | 任意 | 无 | llvm | `riscv32-none-elf` | `xim:picolibc-riscv@1.8.12` | picolibc | 无 | 可替换 | 同上 |
| 14 | `aarch64-none-elf` | 任意 | 无 | llvm | `aarch64-none-elf` | **无**(表里为空) | 无 | 无 | 可替换 | tier=preview;`sysroot=""` 是唯一可用形态 |
| 15 | `x86_64-none-elf` | 任意 | 无 | llvm | `x86_64-none-elf` | **无** | 无 | 无 | 可替换 | 同上 |
| 16 | `riscv64-linux-musl` | — | linux | — | — | — | — | — | — | **不支持**(tier=planned) |
| 17 | `aarch64-linux-gnu` | — | linux | — | — | — | — | — | — | **不支持**(tier=planned) |
| 18 | `x86_64-macos` | — | macos | — | — | — | — | — | — | **不支持**(tier=planned) |
| 19 | 任意 × gcc × openkal | — | — | gcc | — | — | — | — | **不支持** | `openkal-llvm-runtime` 是 libc++,`requires` 拒绝 gcc |

---

## 2. 表二:现在实际是什么

构建机 = **Linux x86_64**(实测)。macOS / Windows 宿主标 **推**。

### 2.1 payload 模式(无 openkal 依赖)

| # | mcpp target | 构建机 OS | 目标机 OS | 编译器 | 编译器 target | sysroot | c-abi | c++-abi | openkal | 构建配置特殊说明 |
|---|---|---|---|---|---|---|---|---|---|---|
| 1 | `x86_64-linux-gnu` | linux | linux | gcc | `x86_64-unknown-linux-gnu` | 载荷自带 | gnu | libstdc++ | - | ✅ |
| 2 | `x86_64-linux-gnu` | linux | linux | llvm | `x86_64-unknown-linux-gnu` | **无** | gnu | — | - | ❌ **A**:hermetic 拒绝,crt 落宿主 |
| 3 | `x86_64-linux-musl` | linux | linux | gcc | `x86_64-unknown-linux-musl` | 载荷自带 | musl | libstdc++ | - | ✅ 驱动 `x86_64-linux-musl-g++` |
| 4 | `x86_64-linux-musl` | linux | linux | llvm | `x86_64-unknown-linux-musl` | **无** | musl | — | - | ❌ **A** |
| 5 | `aarch64-linux-musl` | linux | linux | gcc | `aarch64-unknown-linux-musl` | 载荷自带 | musl | libstdc++ | - | ✅ |
| 6 | `aarch64-linux-musl` | linux | linux | llvm | `aarch64-unknown-linux-musl` | **无** | musl | — | - | ❌ **A** |
| 7 | `x86_64-windows-gnu` | linux | windows | gcc | `x86_64-w64-windows-gnu` | 载荷自带 | gnu | libstdc++ | - | ✅ `ldflags = -lstdc++exp` |
| 8 | `x86_64-windows-gnu` | linux | windows | llvm | `x86_64-w64-windows-gnu` | **无** | gnu | — | - | ❌ **B**:`ldflags` 为空,无 `-B/-L/--sysroot` |
| 9 | `x86_64-windows-musl` | linux | windows | 两者 | - | - | - | - | - | ❌ 拒绝(`cannot be built on this host`) |
| 10 | `x86_64-windows-msvc` | linux | windows | 两者 | - | - | - | - | - | ✅ 拒绝(宿主无 MSVC,正确) |
| 11 | `x86_64-windows-msvc` | **windows** 推 | windows | msvc | `x86_64-pc-windows-msvc` 推 | Windows SDK 推 | ucrt 推 | MSVC STL 推 | - | 推:`host_can_serve` 返回 `is_windows` |
| 12 | `aarch64-macos` | linux | macos | 两者 | - | - | - | - | - | ✅ 拒绝(宿主无 SDK,正确) |
| 13 | `aarch64-macos` | **macos** 推 | macos | llvm | `arm64-apple-macos14.0` 推 | macOS SDK 推 | libSystem 推 | libc++ 推 | - | 推:`payload_libc_name("macos","")` |
| 14 | `riscv64-none-elf` | linux | 无 | llvm | — | `sysroot=""` 时无 | 无 | 无 | - | ✅ 产出 RISC-V ELF |
| 15 | `riscv64-none-elf` | linux | 无 | llvm | — | **表里的 picolibc** | picolibc | 无 | - | ❌ **C**:干净环境下不被安装(#510) |
| 16 | `riscv32/aarch64/x86_64-none-elf` | linux | 无 | llvm | — | `sysroot=""` | 无 | 无 | - | ✅ 三格全过 |
| 17 | 四个 `*-none-elf` | linux | 无 | **gcc** | — | - | - | - | - | ❌ **D**:g++ 收到 `-mabi=lp64d` |
| 18 | `riscv64-linux-musl` / `aarch64-linux-gnu` / `x86_64-macos` | linux | — | 两者 | - | - | - | - | - | ✅ `registered but not…`(正确) |

### 2.2 graph 模式(openkal-musl + openkal-llvm-runtime)

| # | mcpp target | 构建机 OS | 目标机 OS | 编译器 | 编译器 target | sysroot | c-abi | c++-abi | openkal | 构建配置特殊说明 |
|---|---|---|---|---|---|---|---|---|---|---|
| 19 | `x86_64-linux-gnu` | linux | linux | llvm | `x86_64-unknown-linux-gnu` | 图供给 | musl(图) | libc++(图) | linux | ✅ kernel-abi=openkal-linux |
| 20 | `x86_64-linux-musl` | linux | linux | llvm | `x86_64-unknown-linux-musl` | 图供给 | musl(图) | libc++(图) | linux | ✅ |
| 21 | `aarch64-linux-musl` | linux | linux | llvm | `aarch64-unknown-linux-musl` | 图供给 | musl(图) | libc++(图) | linux | ✅ |
| 22 | `aarch64-macos` | **linux** | macos | llvm | `arm64-apple-macos14.0` | 图供给 | musl(图) | libc++(图) | macos | ✅ 交叉到 macOS 成立 |
| 23 | `x86_64-windows-gnu` | linux | windows | llvm | `x86_64-w64-windows-gnu` | 图供给 | musl(图) | libc++(图) | windows | ✅ **c-abi 与 #7 不同** |
| 24 | `x86_64-windows-musl` | linux | windows | llvm | `x86_64-w64-windows-gnu` | 图供给 | musl(图) | libc++(图) | windows | ✅ |
| 25 | `riscv64-none-elf` | linux | 无 | llvm | — | 图供给 | musl(图) | libc++(图) | — | ✅ |
| 26 | 全部 14 个 | linux | — | **gcc** | - | - | - | - | - | ✅ `requires` 拒绝(**正确**) |
| 27 | `x86_64-windows-msvc` | linux | windows | llvm | - | - | - | - | - | ✅ 拒绝(需 MSVC ABI) |

---

## 3. 差异行

对照表一与表二,**四行不符**。其余各行两表一致。

| 差异 | 应该 | 现在 | 影响 |
|---|---|---|---|
| **A** | 表一 #2/#4:llvm × linux,mcpp 提供 sysroot | 表二 #2/#4/#6:mcpp 不提供,clang 找到宿主的,hermetic 拒绝 | **llvm 在 linux 目标上不可用**(无 openkal 时) |
| **B** | 表一 #7:llvm × windows-gnu,mcpp 提供 MinGW sysroot | 表二 #8:`ldflags` 为空,无 `-B/-L/--sysroot` | **llvm × windows-gnu 不可用**,且诊断无关(`unknown file type`) |
| **C** | 表一 #12/#13:目标行的 picolibc 就位 | 表二 #15:声明而不安装(#510) | **干净环境下裸机不可用** |
| **D** | 表一:裸机行的 pin 是能力陈述,gcc 发不出这些目标 | 表二 #17:用户显式声明 gcc 时无条件让位 | **诊断指向 `-mabi` 而非决定** |

### A 与 B 是同一件事

两者都是:**clang 靠 `--target=` 切目标,而目标的 sysroot 必须由外部给,mcpp 没给。**
gcc 不需要,因为它一个目标一份载荷、驱动自带 sysroot(实测 #3 #5 #7 的
`x86_64-w64-mingw32/` 目录)。

区别只在后果:
- **A** 被 hermetic 检查拦住,诊断准确(`Scrt1.o (outside the sandbox)`)
- **B** 没被拦住,失败于 `ld.lld: unknown file type`,**与真正的问题无关**

⚠️ **hermetic 检查为何对 A 报而对 B 不报,未查清。未实测。**

### C 与 D 已有 issue / 分析

- **C** = issue #510,判据是删掉裸机 CI 那两行手工安装
- **D** = 上一篇 §4.③,修法是 `targetPinIsCapability` 为真时对用户显式声明也做一次
  能力检查

---

## 4. 表一里被标「推」的格子

⚠️ 这些**没有任何本地测量**,只有源码推导。列出来是为了让它们可被反驳:

| 格 | 推导依据 |
|---|---|
| #11 windows 宿主 × msvc | `host_can_serve`:`if (target.os == "windows") return is_windows` |
| #13 macos 宿主 × aarch64-macos | `host_can_serve`:`if (target.os == "macos") return is_macos`;c-abi 由 `payload_libc_name("macos","")` = `libSystem` |
| 表一 #3 windows 宿主 × linux-musl | `host_can_serve` 的 windows 分支:`is_windows && is_musl && arch == host_arch` |
| 表一 #6 windows 宿主 × windows-gnu | `to_xim_package`:`is_windows → "mingw-gcc"`,否则 `"mingw-cross-gcc"` |

⭐ **建议把 §2 的扫描做成脚本,在三个宿主的 CI 上各跑一次**,这些「推」就变成实测,
而矩阵成为一次测量的输出而非一份会过期的文档。

---

## 5. 建议顺序

1. **B**:查清 hermetic 为何不报,以及 llvm × windows-gnu 是否本该有 sysroot。它现在
   既不可用也没有可读诊断,优先级最高。
2. **A**:与 B 同源,一并考虑 —— 是否为 llvm × 各 linux 目标提供 sysroot,还是明确
   声明「llvm 需要 openkal 才能用于 hosted 目标」并让诊断这么说。
3. **C**(#510)、**D**:已有判据,可独立进行。
4. **脚本化 §2 并接进三宿主 CI**,消除 §4 的全部「推」。
