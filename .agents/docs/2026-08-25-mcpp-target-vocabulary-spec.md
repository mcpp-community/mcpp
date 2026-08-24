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
| `aarch64-macos` | (省略) | 由图决定:有 openkal 则 musl,否则 libSystem |
| `aarch64-macos-musl` | C 库 | **musl**(openkal 之上) |
| `riscv64-none` | (无) | **没有 C 库** |

⚠️ **macOS 也需要这一段,而我第一稿写的是「该平台只有一个 C 库」。**
实测(mcpp 2026.8.24.6,构建期体系):

```
Target aarch64-macos → arm64-apple-macos14.0
       c-abi   musl   (openkal-musl@0.3.3, graph)
```

⭐ **openkal 让 macOS 上也有了 musl**,与 libSystem 并存 —— 与 Windows
完全同形。所以「该平台只有一个」这句话在 openkal 出现之前成立,现在不成立,
而我在同一份文档的 §5 里已经记下了这个实测却没有回头改这张表。

⚠️ **`aarch64-macos` 不带第三段时该解析成哪一个?** 与 Linux/Windows 一致:
**不作请求**,由图决定 —— 有 openkal 依赖就是 musl,没有就是 libSystem。
这正是「省略即不作请求」这条规则在第三个平台上的同一次应用。

⭐ **libSystem 不该有名字,而理由是实测出来的。**

macOS 的三元组形状与另外两个平台不同 —— OS 段里带部署目标,且**没有
env 段**:

```
arm64-apple-macos14.0
     ^vendor ^os + 部署目标
```

clang 接受任意 env 后缀却不赋予任何含义,实测:

```
arm64-apple-macos14.0-gnu   → 原样返回,未规范化
arm64-apple-macos14.0-musl  → 原样返回,未规范化
```

原样保留而不像 Windows 那样重拼,说明 **Darwin 那一支根本不看 env 段**。

于是给 libSystem 起名会得到一个**在编译器侧没有对应物**的字符串。而它
本来也不需要名字:按「省略 = 不作请求,由图决定」,两种情况都被覆盖 ——

| 图里有 openkal | 结果 |
|---|---|
| 有 | `c-abi musl (graph)` |
| 没有 | libSystem(payload) |

⚠️ **这与 Linux/Windows 不对称,而不对称是有理由的。** 那两个平台上
`gnu` 是 LLVM 词表里真实存在、且映射到真实编译器行为的值(Windows 上
它决定对象 ABI);macOS 上没有任何对应物可映射。规范因此定:
**macOS 的第三段只有一个合法值 `musl`,libSystem 由省略表达。**

⚠️ 连带:openkal 路径上 mcpp 完全不碰 libSystem,所以这个命名问题
在构建期体系里从一开始就不存在 —— 它只在预构建路径上被问起,
而那条路径今天用的正是省略式。

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
| **libc 取值** | `gnu`(别名 `glibc`)`musl` `msvc` `ucrt`(预留);macOS 上仅 `musl` |
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
(macOS 显式钉住 libSystem 的问题已移到 §6,作为一条明确记录在案的代价。)

---

## 6. 已知代价:记录在案,遇到真实用例再解

本节记录**明知而未解**的东西。它们不是遗漏,而是判断「现在解的收益不明」
之后留下的账;每一条都给出**重新打开它的触发条件**,以免后来者需要重新
把上下文推导一遍。

### 6.1 macOS 上无法显式请求 libSystem

规范定为「libSystem 由省略表达」(§1.1)。代价是**说不出它**:

```
aarch64-macos          图里有 openkal → musl        图里没有 → libSystem
aarch64-macos-musl     显式请求 musl
aarch64-macos-???      ← 没有这个写法
```

于是一个**装了 openkal 依赖、却想在 macOS 上用 libSystem** 的工程,
无法表达这个意图。

⚠️ **Linux 上没有这个缺口**:`x86_64-linux-gnu` 恰恰就是「显式钉住 glibc」,
即使图里有 openkal-musl 也能说出来(今天的行为是报出分歧并以图为准 ——
见 `check_request`,那本身也是一条待议的策略)。

**不解的理由**:没有真实用例。给 libSystem 起的任何名字在 clang 侧都没有
对应物(实测 Darwin 不看 env 段),所以它只能是 mcpp 单方面的标记 ——
为一个没人提出过的需求引入一个只有一半意义的名字,代价比收益清楚。

**重新打开它的触发条件**,满足任一即可:

1. 出现一个工程,依赖图里有 openkal 而在 macOS 上确实需要 libSystem
2. openkal 支持了「同一构建里按目标选择 C 库」,使这个组合从边缘变成常规
3. LLVM 在 Darwin 一支开始解释 env 段,使这个名字有了可映射的对象

**届时的候选做法**(不预先择一):`aarch64-macos-system`、
`aarch64-macos-apple`,或者引入一个与三元组正交的表达(如
`[target.X] c-abi = "system"`)—— 最后一条可能更合适,因为它承认
「这不是三元组该回答的问题」。

### 6.2 其余待定项

`ucrt` 如何进表、反向映射的规则、裸机去 `-elf` 的时机 —— 见 §5。
它们与 6.1 的差别是:那些是**尚未测**,这一条是**测过了并决定不做**。

---

## 7. 验收:`aarch64-linux-musl` 端到端可用

⭐ **一套目标词表的价值只有在第二个架构上才被检验。** x86_64 上「按 OS 分」
与「按架构分」给出相同答案,所以判据用错了轴也看不出来。本节把
`aarch64-linux-musl` 定为规范的**验收目标**,并记录逐层剥出的三处缺陷。

来源:mcpp-community/mcpp#492 的使用者报告 —— 他需要同时出 x86_64 与
aarch64 两份静态二进制。

### 7.1 ✅ 已修:x87 例程按 OS 排,而它是架构的性质

```
truncxfhf2.c:13:36: error: unknown type name 'xf_float'; did you mean 'tf_float'?
```

`*xf*.c` / `*xc3.c` 是 x87 80 位 `long double` 例程。排除它们的两处条件是
`cfg(os = "macos")` 与 `cfg(os = "none")`,而 aarch64-linux 落进
`cfg(os = "linux")` 那一支,没有排除。

⚠️ **两处此前都是对的**:那两个 OS 今天恰好都蕴含「非 x87 架构」。
并且 linux 段**不排除**也是对的 —— x86 上 `long double` 真是 x87 80 位,
`-lgcc` 拿掉后 `ld.lld: error: undefined symbol: __mulxc3` 会真的出现。
**两个方向都会坏,错的只是轴。**

修法:`cfg(all(os = "linux", not(arch = "x86_64")))`。
已提 mcpplibs/openkal-llvm-runtime#5。

### 7.2 ⚠️ 未修:`--no-default-config` 本身改变目标特性

x87 修好之后撞到下一处:

```
error: precompiled file 'std.pcm' was compiled with the target feature '-fmv'
       but the current translation unit is not
error: current translation unit is compiled with the target feature
       '+outline-atomics' but the precompiled file 'std.pcm' was not
```

⭐ **根因不是三元组不一致**(两边都是 `aarch64-unknown-linux-musl`,
已从缓存里那条命令逐字核对),也不是缓存串目标(键含 `target_triple`,
两个 aarch64 条目正确地按工具链分开)。实测:

```
clang --target=aarch64-unknown-linux-musl                       →  +outline-atomics
clang --target=aarch64-unknown-linux-musl --no-default-config   →  -fmv
```

**`--no-default-config` 自己就改变目标特性。** std 模块带它编
(包的 `std-module-flags` 要求),普通 TU 不带。

⚠️ 而那个 flag 是**必需的**,包里的注释写明理由:载荷的 `clang++.cfg`
无条件塞进宿主 C 库的头,不排除它,模块会在 `<wchar.h>` 上失败。

于是这是**两个都成立的要求相撞**:

| 要求 | 来自 | 为什么必需 |
|---|---|---|
| std 模块要 `--no-default-config` | 包 | 否则宿主 C 库的头混进来 |
| std 模块与 TU 的目标特性必须一致 | clang | 否则 BMI 加载失败 |

⚠️ **x86_64 上 `--no-default-config` 不改变特性,所以这对矛盾从未暴露。**
aarch64 是第一个让它现形的目标 —— 这正是「第二个架构才检验抽象」的又一例。

**候选修法**(均未实测,按代价排):

1. 让普通 TU 也带 `--no-default-config` —— 一致了,但会去掉 cfg 里
   其它可能承重的东西,影响面未知
2. 把 std 模块的目标特性显式钉住(`-fno-mv` 或对称地补 `+outline-atomics`)
   —— 治标,且要为每个架构维护一张表
3. ⭐ 让 `stdModuleTargetFlags` 把「驱动配置对特性的影响」也算进去 ——
   即由引擎查询一次 `clang -### --no-default-config` 与不带的差集,
   把差异补给消费侧。治本,但要新增一次编译器探测

**验收判据**:`mcpp build --target aarch64-linux-musl` 在一个依赖
`openkal-llvm-runtime` 的工程上产出 aarch64 静态 ELF,且 `file` 报
`ELF 64-bit LSB executable, ARM aarch64, statically linked`。

### 7.3 ⚠️ 未查:`aarch64-linux-gnu` 仍是 `planned`

使用者撞到的原话是:

```
error: target 'aarch64-linux-gnu' is registered but not yet supported (planned)
```

⭐ 而他真正需要的是 **`aarch64-linux-musl`**(静态二进制),那一行是
`verified`。⚠️ 这是一处**文档/诊断问题而非能力问题**:诊断说了「没发布
工具链」,没说「你要的那个目标另有拼法」。`did_you_mean` 今天只对拼错的
名字生效,不对「档位不够但同类目标可用」的情况生效。

### 7.4 使用者报告里另外两条,未查

- **`std::random_device` 不可用** —— payload 的 `__config_site` 两份都写
  `_LIBCPP_HAS_RANDOM_DEVICE 0`,他本地改成 1 即修好 5 个编译错。
  ⚠️ 静态 musl 二进制读 `/dev/urandom` 应当可行,所以 `generic` 那份为何
  也关着需要单独查。
- **GMF 里 `#include <标准头>` 的 TU 编不过** —— `clang++.cfg` 把宿主
  libc++ 与 glibc 的 `-isystem` 塞进来,与 openkal 的 musl libc++ 头混合。
  ⭐ 与 7.2 是**同一个 cfg** 引起的两个症状,应一并考虑。
