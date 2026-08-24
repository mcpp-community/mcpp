# mcpp 目标词表:一套规范,映射到各编译器

2026-08-25 · 规范设计方案(待 review,尚未实施)

本文提出 mcpp 拥有**自己的**目标词表,而不是转述某个编译器的。
每一条设计都给出实测依据与拒绝的替代方案。

前置阅读:`2026-08-25-os-toolchain-target-matrix.md`(三个轴的实测矩阵)、
`2026-08-25-target-system-analysis.md`(现状缺陷)。

---

## 0. 为什么需要自己的词表

现有三套词表,互不一致,而 mcpp 三套都要打交道:

| 词表 | 例 | 谁读 |
|---|---|---|
| GCC / autoconf | `x86_64-w64-mingw32` | 预构建载荷的编译器,以**文件名**的形式 |
| LLVM | `x86_64-w64-windows-gnu` | `clang --target=` |
| mcpp | `x86_64-windows-gnu` | 目标表、输出目录、`cfg()`、打包 ABI tag |

⭐ **第三套已经存在了**,只是它今天在**转述** LLVM 而不是自己定义。
转述的代价实测可见:

### 0.1 LLVM 词表的第三段没有单一含义

| 平台 | `env` 命名 | 实测依据 |
|---|---|---|
| linux | **C 库** | `gnu`→glibc,`musl`→musl |
| windows | **对象 ABI** | `gnu`→`_Z1fi`,`msvc`→`?f@@YAHH@Z` |
| none | **对象格式** | `elf` |

同一段在三个平台上是三条不同的轴。这直接造成两处缺陷:
`cfg(env = "gnu")` 在两个平台上语义不同;
以及 Windows 上**两个完全不同的 C 库共用 `-gnu` 一个名字**
(MinGW CRT 与 musl,实测体积 587KB vs 9.8MB、依赖 `msvcrt.dll` vs 不依赖)。

### 0.2 LLVM 词表拼不出真实存在的目标

实测 llvm 22.1.8:

```
clang++ --target=x86_64-pc-windows-musl -c t.cpp
    #5  llvm::MCWinCOFFStreamer::emitCGProfileEntry(...)   ← ICE,不是诊断
```

Windows 上四个非 MSVC 环境 `gnu`/`cygnus`/`itanium`/`musl`,前三个能编,
只有 `musl` 崩;预定义宏显示它从未被建模(无 `__MINGW32__`)。

⭐ **而 musl on Windows 真实存在**——`openkal-musl` 就是它。
一个词表若拼不出已经存在的东西,它就不能作为 mcpp 的身份来源。

### 0.3 GCC 词表把实现放进 OS 位

`x86_64-w64-mingw32`:`mingw32` 占据 OS 段,`w64` 占据 vendor 段。
这是 autoconf 的世界观(MinGW 是一个独立 OS),LLVM 已经拒绝了它并重拼为
`windows-gnu`。mcpp 更没有理由继承它。

---

## 1. 设计:三段,每段一条轴

```
<arch> - <os> - <libc>
```

**没有 vendor 段。** 实测 LLVM 那一段几乎总被填成 `unknown`,不承重;
苹果的 `apple` 与 MinGW 的 `w64` 是各自项目的标识,不是目标的性质。
mcpp 在映射时补出编译器需要的 vendor,自己不保存它。

### 1.1 第三段恒定表示 C 库实现

⭐ **这是与 LLVM 的核心分歧,也是本方案的全部价值。**

| mcpp 目标 | 第三段的含义 | 对应实现 |
|---|---|---|
| `x86_64-linux-gnu` | C 库 | glibc |
| `x86_64-linux-musl` | C 库 | musl |
| `x86_64-windows-gnu` | C 库 | **MinGW CRT**(封装 `msvcrt.dll`) |
| `x86_64-windows-musl` | C 库 | **musl**(openkal 之上) |
| `x86_64-windows-msvc` | C 库 | UCRT |
| `aarch64-macos` | (无) | libSystem —— 该平台只有一个,不需要名字 |
| `riscv64-none` | (无) | **没有 C 库** |

⚠️ **`x86_64-linux-gnu` 里的 `gnu` 严格说是项目名而非库名(glibc)。**
保留它是为了兼容既有工程与 LLVM,而不是因为它准确。规范应当**接受
`glibc` 作为别名**并在文档中说明二者等价,新写的工程用哪个都对。

### 1.2 对象 ABI 是**派生**属性,不是一段

Windows 上 ABI 与 C 库在实践中一一对应,因此不需要第四段:

| os | libc | 派生的对象 ABI | 实测符号 |
|---|---|---|---|
| windows | `gnu` | Itanium | `_ZN2ns4Base1fEi` |
| windows | `musl` | Itanium | 同上 |
| windows | `msvc` | MSVC | `?f@Base@ns@@UEAAHH@Z` |
| linux / macos | 任意 | Itanium | — |

规范把这张表写成**规范化的派生规则**,而不是让每个消费者自己猜。
`cfg(abi = "itanium")` 因此可以被支持,且与 `cfg(libc = "musl")` 正交 ——
这解决了 §0.1 里 `cfg(env=)` 语义不定的缺陷。

⚠️ **若将来出现「Itanium ABI + UCRT」这样的组合**(mingw-w64 确实支持
`ucrt` 作为 CRT 而保持 Itanium ABI),一一对应就破了。届时的做法是
**新增一个 libc 名**(`x86_64-windows-ucrt`),而不是加第四段 ——
因为破的是「libc 只有三种」而不是「一个 libc 对一套 ABI」。

### 1.3 裸机没有第三段

```
riscv64-none        ← 规范形式
riscv64-none-elf    ← 接受的别名(LLVM 拼法),规范化为上者
```

理由:第三段表示 C 库,而裸机没有 C 库。`elf` 是**对象格式**,
与 `arch`+`os` 一起已经决定,不需要单独命名。

⚠️ 这是一处**破坏性**变更:输出目录从 `target/riscv64-none-elf/` 变为
`target/riscv64-none/`。§4 给迁移方案。

### 1.4 不学 clang 的地方,以及各自的理由

| clang 的做法 | mcpp | 理由 |
|---|---|---|
| Windows 上 `gnu` = 对象 ABI | `gnu` = MinGW CRT | 第三段恒定表示 C 库(§1.1) |
| 无 `windows-musl` | **有** | 它真实存在(`openkal-musl`),且 clang 那条路是 ICE(§0.2) |
| 裸机带 `-elf` | 不带 | 那不是 C 库(§1.3) |
| 四段含 vendor | 三段 | vendor 不承重(§1) |
| `x86_64-unknown-linux-gnu` | `x86_64-linux-gnu` | 同上 |

**学 clang 的地方**:段的顺序、`arch` 与 `os` 的取值集合、`musl`/`msvc`
这些名字本身。偏离仅限于上表五处,每一处都有实测支撑。

---

## 2. 映射:一个词表到多个编译器

规范的核心是一张**双向映射表**,而不是散落的 `if`。

### 2.1 到 clang(`--target=`)

```
mcpp                    → clang
x86_64-linux-gnu        → x86_64-unknown-linux-gnu
x86_64-linux-musl       → x86_64-unknown-linux-musl
x86_64-windows-gnu      → x86_64-w64-windows-gnu
x86_64-windows-musl     → x86_64-w64-windows-gnu     ⭐ 与上一行相同
x86_64-windows-msvc     → x86_64-pc-windows-msvc
aarch64-macos           → arm64-apple-macos14.0
riscv64-none            → riscv64-unknown-none-elf
```

⭐ **`windows-gnu` 与 `windows-musl` 映到同一个 clang 三元组,这是设计而非
巧合。** 交给 clang 的字符串回答「遵循哪套对象 ABI」,mcpp 的名字回答
「C 库是谁」。两个问题,两个答案;第二个 clang 答不了(§0.2)。

⚠️ 映射**不是双射**,因此从 clang 三元组反推 mcpp 目标是不确定的。
规范应明确:**反向映射只在诊断中使用,不作为身份来源**。

### 2.2 到 GCC 载荷(编译器文件名)

GCC 的目标编死在可执行文件名里,所以映射的是**载荷与文件名**:

```
mcpp                    → 载荷 / 编译器
x86_64-linux-gnu        → xim:gcc          / g++
x86_64-linux-musl       → xim:musl-gcc     / x86_64-linux-musl-g++
x86_64-windows-gnu      → xim:mingw-cross-gcc / x86_64-w64-mingw32-g++
x86_64-windows-musl     → （无 GCC 路径）
x86_64-windows-msvc     → （MSVC,非 GCC)
riscv64-none            → （clang 专属:见 §2.3)
```

⚠️ **`x86_64-windows-musl` 没有 GCC 路径**,而这不是缺口:musl-on-Windows
是构建期体系的产物,其 C 库来自图而非载荷。规范应把「某目标在某工具链族下
无路径」记为**一等状态**,而不是让消费者从空字符串里推断。

### 2.3 到 MSVC

`x86_64-windows-msvc` → `cl.exe` / `clang-cl`,由 SDK 提供 C 库。
实测:clang 在 Linux 上能为该目标**编译**(产出 `?f@@YAHH@Z` 的 COFF),
但**链接**需要不可再分发的 MSVC SDK/CRT。规范应把这两件事分开记录 ——
「能否发码」与「能否链出成品」是不同的能力。

---

## 3. 规范应当定死的东西

| 条目 | 内容 |
|---|---|
| **语法** | `<arch>-<os>[-<libc>]`,段内字符集,大小写 |
| **arch 取值** | `x86_64` `aarch64` `riscv64` `riscv32` … |
| **os 取值** | `linux` `windows` `macos` `none` |
| **libc 取值** | `gnu`(别名 `glibc`)`musl` `msvc` `ucrt`(预留) |
| **省略规则** | 省略 libc = **不作请求**,由图或默认决定;身份仍完整 |
| **派生属性** | `abi`(itanium / msvc)、`format`(elf / pe / macho)、`freestanding` |
| **别名表** | `riscv64-none-elf` → `riscv64-none`,`x86_64-w64-mingw32` → `x86_64-windows-gnu` |
| **映射表** | §2 的三张,双向声明,反向仅用于诊断 |
| **cfg 谓词** | `os` `arch` `libc` `abi` `format` `bare`,**不再有 `env`** |

⭐ **`cfg(env = …)` 应当被弃用而非直接删除**:它今天在生态包里有使用者
(实测 `openkal-windows` 一处),删掉会让旧包在新引擎上加载失败 ——
这与 [[index-floor-must-degrade]] 记的教训同族。做法是接受它、
按 `libc` 求值、并在使用时告知新拼法。

---

## 4. 迁移

| 变更 | 影响面 | 做法 |
|---|---|---|
| 新增 `x86_64-windows-musl` | 加法,无破坏 | 已实施(本轮) |
| `cfg(libc=)` / `cfg(abi=)` | 加法 | 新增谓词,`env` 保留为别名 |
| 裸机去掉 `-elf` | **破坏**:输出目录改名 | 别名 + 一个发布周期的过渡,`-elf` 拼法仍接受 |
| 去掉 vendor 段 | 无 —— mcpp 本来就没保存 | 仅文档化 |

⚠️ **裸机那一条是唯一的破坏性变更,而它的收益最小**(只是名字更整齐)。
本方案建议**先不做**,或与一次主版本一同做。规范可以先把
`riscv64-none` 定为规范形式、`riscv64-none-elf` 定为别名,而**输出目录
继续用别名**,直到有别的理由动它。

---

## 5. 本文刻意没有断言的事

- **没有断言 `ucrt` 该怎么进表。** mingw-w64 支持 `ucrt` 作为 CRT 而保持
  Itanium ABI,这会破坏 §1.2 的一一对应。⚠️ 需要先实测「mingw-w64 + ucrt」
  在 mcpp 的载荷里是否可达,再决定是新增 libc 名还是引入第四段。
- **没有断言反向映射的完整规则。** §2.1 的映射不是双射,而诊断路径上
  确实需要「从 clang 三元组说出 mcpp 目标」。规则应当是「多对一时报出全部
  候选」还是「拒绝反推」,需要看诊断的实际用法再定。
- **没有断言 macOS 是否需要 libc 段。** 实测该平台 C 库唯一(libSystem),
  但构建期体系下 `c-abi` 报的是 `musl (graph)` —— 那是 openkal 在 macOS 上
  的 musl,与 libSystem 并存。⚠️ 这意味着 macOS 也可能需要
  `aarch64-macos-musl` 与 `aarch64-macos-system` 的区分,与 Windows 同形。
  本轮未展开,应作为下一步实测项。
