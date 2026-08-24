# OS × 工具链 × 目标:组合矩阵与理由

2026-08-25。本文枚举三个轴的**实际**组合,给出每格成立或不成立的理由,
并标出其中的缺陷。事实取自 HEAD 的代码与本机实测,不取自推理。

与既有两份的分界:`2026-08-24-target-side-design.md` 讲**五层由谁供给**,
`2026-08-25-target-system-analysis.md` 讲**三元组的语法与语义**,
本文讲**三个轴交叉出来的格子**。

---

## 0. 三个轴

| 轴 | 取值 | 由什么决定 |
|---|---|---|
| **宿主 OS** | linux / macos / windows | mcpp 跑在哪台机器上 |
| **工具链** | `Family { Gcc, Llvm, Msvc }` × 载荷 | `[toolchain]`,或全局默认,或目标行的 `pin` |
| **目标** | `kKnownTargets` 的 16 行 | `--target`,或 `[build] target`,或宿主 |

⚠️ **三个轴不独立,而依赖方向是单向的**:目标 → 需要某种 C 库 →
只有某些(宿主,工具链)组合能供给它。反过来不成立:工具链不决定目标。

## 0.1 两种体系把这张表分成两半

| | 传统预构建体系 | 构建期体系 |
|---|---|---|
| C 库来自 | 工具链**载荷** | 依赖**图**,从源码构建 |
| 谁限制组合 | **载荷是否存在**(见 §2 的 `host_can_serve`) | 只需编译器能发这个目标的码 |
| 可达格子 | 少,且按宿主分 | 多,且几乎与宿主无关 |

⭐ **本文最重要的一句**:`host_can_serve()` 是**预构建体系**的判据。
构建期体系下它过于保守 —— clang 是天生的交叉编译器,而 C 库由图供给,
所以「这台宿主能不能服务这个目标」的答案与载荷覆盖无关。
今天 mcpp 用同一个谓词回答两个问题,§5 是它的后果。

---

## 1. 目标表(16 行,实测)

| 目标 | 档 | 备注 | pin(C 库载荷) | sysroot | 默认静态 |
|---|---|---|---|---|---|
| `x86_64-linux-gnu` | verified | | — | | 否 |
| `x86_64-linux-musl` | verified | | `gcc@16.1.0` | | 是 |
| `aarch64-linux-musl` | verified | | `gcc@16.1.0` | | 是 |
| `x86_64-windows-gnu` | verified | PE | `gcc@16.1.0` | | 是 |
| `x86_64-windows-msvc` | verified | PE | — | | 否 |
| `aarch64-macos` | verified | | — | | 否 |
| `riscv64-linux-musl` | planned | | — | | 是 |
| `aarch64-linux-gnu` | planned | | — | | 否 |
| `x86_64-macos` | planned | | — | | 否 |
| `riscv64-none-elf` | verified | bare | `llvm@22.1.8` | `xim:picolibc-riscv@1.8.12` | 是 |
| `riscv32-none-elf` | verified | bare | `llvm@22.1.8` | `xim:picolibc-riscv@1.8.12` | 是 |
| `aarch64-none-elf` | preview | bare | `llvm@22.1.8` | — | 是 |
| `x86_64-none-elf` | preview | bare | `llvm@22.1.8` | — | 是 |

⚠️ **`pin` 列的语义常被读错。** 它不是「这个目标首选哪个编译器」,而是
「**这个目标的 C 库由哪个载荷供给**」。`x86_64-linux-musl → gcc@16.1.0`
说的是 musl-gcc 载荷带着 musl;它对一个 C 库来自图的工程毫无意义 ——
这正是 2026.8.24.3 把该 pin 推迟到依赖图解析之后才施加的原因。

## 2. 宿主 × 目标:`host_can_serve()` 的实际规则

`src/toolchain/registry.cppm:586`。逐条抄录并给出理由:

| 目标 | linux 宿主 | macos 宿主 | windows 宿主 | 理由 |
|---|---|---|---|---|
| `*-linux-musl` | ✅ 任意 arch | ❌ | ✅ 仅同 arch | musl 载荷自包含,不需要宿主 sysroot |
| `*-linux-gnu` | ✅ 仅同 arch | ❌ | ❌ | glibc 目标还需要 `xim:glibc` / `xim:linux-headers`,它们只有本 arch 的 |
| `*-windows-gnu` | ✅ | ❌ | ✅ | mingw 交叉载荷有 linux-hosted 与 windows-hosted 两份 |
| `*-windows-msvc` | ❌ | ❌ | ✅ | MSVC 只在 Windows 上存在 |
| `aarch64-macos` | ❌ | ✅ | ❌ | 苹果 SDK 不可再分发 |
| `*-none-*`(裸机) | ✅ | ✅ | ✅ | ⭐ clang/lld 天生交叉,**不需要任何按宿主的载荷** |

⭐ 裸机那一行是整张表里唯一「三个宿主全绿」的,而理由不是覆盖得好,
是**它根本不需要载荷**。注释把这一点写得很准:

> ⚠️ Serviceable is not the same as complete: a target with no C library still
> links only `-nostdlib` programs.

## 3. 宿主 × 工具链:哪些族能装

`available_toolchain_indexes()`:

| 宿主 | 可装的族 | 索引名 |
|---|---|---|
| linux | Gcc, Llvm | `gcc`、`musl-gcc`、`llvm` |
| macos | Gcc, Llvm | 同上 |
| windows | Gcc, Llvm, **Msvc** | 另加 `msvc`(pinned toolset)、`mingw-gcc` |

`Family` 只有三个值,而载荷远不止三个(`musl-gcc`、`mingw-cross-gcc`、
`mingw-gcc`、`aarch64-linux-musl-gcc`…)。**族是语言方言的轴,载荷是分发的轴**,
两者一对多。

## 4. 三轴交叉:本机实测

宿主 = linux/x86_64。每格跑 `mcpp build --target X`,记录 c-abi 的实际供给者。

### 4.1 构建期体系(工程依赖 `openkal-llvm-runtime`)

| 目标 | 工具链 | c-abi 实际 | 诊断 |
|---|---|---|---|
| `x86_64-linux` | llvm | musl (graph) | — |
| `x86_64-linux-gnu` | llvm | musl (graph) | ⚠️ 名实不符警告 |
| `x86_64-linux-musl` | llvm | musl (graph) | — |
| `x86_64-windows` | llvm | musl (graph) | — |
| `x86_64-windows-gnu` | llvm | musl (graph) | **静默** ← §5.1 |
| `aarch64-macos` | llvm | musl (graph) | — |

⭐ **六格里五格的 c-abi 都是 musl,且都来自图。** 工具链恒为 llvm,
宿主恒为 linux。**目标的 OS 只改变 kernel-abi 那一层**
(`openkal-linux` / `openkal-windows` / `openkal-macos`),其余四层不变。
这是构建期体系的全部意义,一张表就能看完。

### 4.2 传统预构建体系(无 openkal 依赖)

| 目标 | 工具链(自动) | c-abi 实际 | 诊断 |
|---|---|---|---|
| `x86_64-linux` | gcc 16.1.0 | **gnu** (payload) | — |
| `x86_64-linux-gnu` | gcc 16.1.0 | **gnu** (payload) | — |
| `x86_64-linux-musl` | gcc 16.1.0 | **musl** (payload) | — |
| `x86_64-windows` | gcc 16.1.0 (mingw-cross) | **gnu** (payload) | — |
| `x86_64-windows-gnu` | gcc 16.1.0 (mingw-cross) | **gnu** (payload) | — |

⭐ **对照 4.1 的同一列**:同一个 `--target x86_64-windows-gnu`,
预构建下 c-abi 是 `gnu`,构建期下是 `musl`。**这不是同一个目标**,
而 mcpp 用同一个名字称呼它们 —— §5.2。

---

## 5. 缺陷

### 5.1 同形的两件事,一个报一个不报

```
构建期 + x86_64-linux-gnu     名字说 gnu，事实是 musl  →  ⚠️ 警告
构建期 + x86_64-windows-gnu   名字说 gnu，事实是 musl  →  ❌ 静默
```

原因是 `check_request()` 的第一行豁免了非「C 库轴」的平台。豁免的理由
(「Windows 上 `gnu` 命名对象 ABI 而非 C 库」)**只对了一半**:那一段
在 Windows 上捆着两件事 —— 对象 ABI(被兑现)与 MinGW 的 C 运行时
(被图替换)。第二件正是本函数存在的意义。

⚠️ 裸机的 `elf` 必须继续豁免:它在任何平台上都不命名 C 库,
对它说「名字请求了 `elf` C ABI」是胡话而不只是噪声。

**修法**:判据从「轴 == CLibrary」改为「轴 ∈ {CLibrary, ObjectAbi}」。
误伤由既有的 `fromGraph()` 挡住 —— §4.2 实测预构建五格的 c-abi 都是
payload 来源,不进警告。

### 5.2 ⭐⭐ musl on Windows 存在,而 mcpp 没有它的名字

§4 的两张表并排放,这一条就无法回避:同一个 `x86_64-windows-gnu`,
预构建下 C 库是 MinGW 的,构建期下是 **musl**。产物实测:

| 观测 | 值 |
|---|---|
| 导入的库 | `ntdll`、`KERNEL32`、`SHELL32` —— 无 `msvcrt`,无 `ucrtbase` |
| Itanium 修饰符号 | 4507 |
| MSVC 修饰符号 | 0 |

**MinGW 的 C 运行时一个都没链进来。那就是 Windows 上的 musl。**
而 mcpp 的身份、输出目录、`cfg(env=)` 与打包的 ABI tag 全都写着 `gnu` ——
恰恰是这个 C 库唯一不是的东西。

⚠️ **我此前把这条否掉的理由是错的**,理由是「LLVM 的三元组词表没有
`x86_64-windows-musl` 这个拼写」。那句话本身是对的,但它是关于**交给
clang 的字符串**的,而 mcpp 的规范形式与它**本来就是两个字符串** ——
报告里那个箭头两侧就是:

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu
       ^^^^^^^^^^^^^^^^^^   ^^^^^^^^^^^^^^^^^^^^^^
       mcpp 的身份           交给 clang 的
```

用编译器的词表限制 mcpp 自己的词表,是把两个轴混成了一个。

**修法是一个名字,不是一套机制。** 但在给出它之前,必须先纠正本文
上一稿里的一处错误 —— 那一稿把两种体系的列并进了一张表:

```
   ✗ 错的:
   mcpp 目标              → 交给 clang               c-abi
   x86_64-windows-gnu     → x86_64-w64-windows-gnu   MinGW CRT
```

⚠️ **这一行不自洽。** `MinGW CRT` 是**预构建**路径的答案,而
`x86_64-w64-windows-gnu` 是**clang** 路径上的字符串。实测预构建路径的
编译行:

```
x86_64-w64-mingw32-g++        ← 编译器
（没有任何 --target= 参数）
```

⭐ **预构建路径根本不传三元组。** 那个载荷的编译器天生只发一个目标,
目标由「装了哪份载荷」决定,不由命令行决定。把它的 c-abi 与 clang 的
`--target` 并列,等于把两个体系混成一个 —— 这与 §5.2 开头批评的
「用编译器的词表限制 mcpp 的词表」是同一类错误,只是方向相反。

正确的表必须按体系分开:

**传统预构建体系**(载荷决定目标,无 `--target`)

| mcpp 目标 | 编译器 | c-abi |
|---|---|---|
| `x86_64-linux-gnu` | `g++`(glibc 载荷) | glibc |
| `x86_64-linux-musl` | `x86_64-linux-musl-gcc` | musl |
| `x86_64-windows-gnu` | `x86_64-w64-mingw32-g++` | MinGW CRT |
| `x86_64-windows-msvc` | `cl.exe` | UCRT |

**构建期体系**(clang + 图,`--target` 决定)

| mcpp 目标 | 交给 clang | c-abi |
|---|---|---|
| `x86_64-linux`(或 `-musl`) | `x86_64-unknown-linux-musl` | musl(图) |
| `x86_64-windows`(今日拼作 `-gnu`) | `x86_64-w64-windows-gnu` | **musl**(图) |

最后一格就是缺名字的那一格。

### 5.2.1 ⚠️ clang 为什么没有 `windows-musl`:实测是它会崩

「LLVM 没有这个拼写」这句话我说了三遍,而它不准确。实测 llvm 22.1.8:

| 三元组 | `-dumpmachine` | 编译 |
|---|---|---|
| `x86_64-pc-windows-musl` | `x86_64-pc-windows-musl`(**原样保留**) | **崩溃** |
| `x86_64-w64-windows-musl` | `x86_64-w64-windows-musl`(原样保留) | **崩溃** |
| `x86_64-unknown-windows-musl` | 原样保留 | **崩溃** |

```
PLEASE submit a bug report to https://github.com/llvm/llvm-project/issues/
Stack dump:
0.  clang++ --target=x86_64-pc-windows-musl -c t.cpp
2.  Code generation
```

⭐ **三元组解析器认识它,代码生成路径没有实现它。** 加上
`-nostdinc -ffreestanding` 的纯 C 也一样崩 —— 所以这不是缺头文件、
缺 sysroot 或缺库,是 LLVM 里 `(windows, musl)` 这个组合根本没有后端处理。

**崩点被抓到了**,它很说明问题:

```
#5  llvm::MCWinCOFFStreamer::emitCGProfileEntry(...)
```

⚠️ 崩在 **COFF 写出器**。clang 已经认定「输出 COFF」,却没有一条
Windows-musl 的路径把 streamer 需要的状态配齐 —— 这不是「不支持所以拒绝」,
是「没人写过所以走到就死」。ICE,不是诊断。

**决定性对照:Windows 上四个非 MSVC 环境,只有 musl 崩。**

| `x86_64-pc-windows-…` | 结果 |
|---|---|
| `gnu` | 编译通过 |
| `cygnus` | 编译通过 |
| `itanium` | 编译通过 |
| **`musl`** | **崩溃** |

环境识别本身就已经断了。预定义宏:

| 三元组 | 宏 |
|---|---|
| `…-windows-gnu` | `__GNUC__` `__MINGW32__` `_WIN32` |
| `…-windows-msvc` | `_MSC_VER` `_WIN32` |
| `…-windows-musl` | `__GNUC__` `_WIN32` ← **`__MINGW32__` 缺席** |

⭐ **`windows-musl` 落进了一个没人认领的组合**:OS 是 Windows(所以输出
COFF),env 不是 msvc(所以走 Itanium 那一支),但**不是 MinGW** ——
下游每一处按 env 分支的地方都没有它这一支。

**「clang 为什么有 windows-gnu」的答案由此对称地给出**:MinGW-w64 是一个
成建制的目标 —— LLVM 里有专门的 ToolChain 类、有 `__MINGW32__` 预定义、
有对应的 COFF/SEH/Itanium-ABI 配置路径。`cygnus` 与 `itanium` 同理,
各有分支。`musl` 在 Windows 侧从未被建模。

于是结论是一句更准的话:**mcpp 无法把 `windows-musl` 交给 clang,
不是因为拼写不存在,而是因为交过去会 ICE。** 而这恰恰证明了 §5.2 的分工
是唯一可行的:

```
mcpp 的身份        x86_64-windows-musl     ← 回答「C 库是谁」
交给 clang 的      x86_64-w64-windows-gnu  ← 回答「遵循哪套对象 ABI」
```

两个字符串必须不同,**不是设计偏好,是上游约束**。

### 5.2.1b 编在 clang 里的是「支持」,不是「CRT」

两件同名不同物的东西,分不开就会得出「既然 clang 内置 MinGW,就该用它的
C 库」这种结论:

| | 在哪 | 编在 clang 二进制里? |
|---|---|---|
| MinGW **支持** —— `__MINGW32__`、`MinGW` ToolChain 类、COFF/SEH/Itanium 配置路径 | clang 内部的 C++ 代码 | **是** |
| MinGW **CRT** —— `crt2.o`、`libmsvcrt.a`、`libmingw32.a`、mingw 头 | 独立载荷 `xim:mingw-cross-gcc` | **否** |

实测:llvm 载荷里一个 `crt2.o` / `libmingw32*` / `libmsvcrt*` 都没有;
它们全在 `xim-x-mingw-cross-gcc/16.1.0/x86_64-w64-mingw32/lib/`。

⭐ **于是 `x86_64-w64-windows-gnu` 这个三元组上,C 库那一格是空的、由外部填。**
填 MinGW CRT 是传统载荷路径,填 musl 是 openkal 路径 —— 两条路走同一套 ABI
约定,链进去的东西完全不同。

这就是 §5.2 那个分工的**机制层面**理由,而不只是命名问题:交给 clang 的
三元组只说明 ABI,**说不出 C 库是谁**,而那一格恰恰是 mcpp 要管的。

### 5.2.1c 两个 C 库的实测差别是结构性的

同一份源码,同为 `--target x86_64-windows-gnu`,只换 C 库来源:

| | MinGW CRT | musl / openkal |
|---|---|---|
| 体积 | 587,894 | **9,815,552**(16.7×) |
| 依赖的 DLL | `KERNEL32`、**`msvcrt.dll`** | `ntdll`、`KERNEL32`、`SHELL32`、`api-ms-win-core-synch` |

⭐ **关键在 `msvcrt.dll` 那一格。** MinGW 的 C 库不是自足的 —— 它是一层
薄封装,`printf`/`malloc` 真正的实现在目标机器自带的那份 DLL 里,
所以 587KB 只是「你的代码 + 胶水」。musl 是自足的:C 库、libc++、
libc++abi、libunwind、compiler-rt 全部静态链入,只经 openkal 调 Win32 原语。

| 维度 | MinGW CRT | musl |
|---|---|---|
| 版本漂移 | 受制于目标机 `msvcrt.dll` 的行为 | 无 —— 就是构建时那份 |
| 体积 | 小 | 大 16.7 倍 |
| 同一份源码跨平台 | **不成立** —— Windows 上是 msvcrt 封装,与 Linux 的 C 库无共同实现 | **成立** —— 三平台同一份 musl 源码 |

最后一行是这套东西存在的理由:构建期体系下 `c-abi musl` 在
linux / windows / macos 三个目标上**是同一个包**,只有 `kernel-abi` 那一层换。

### 5.2.2 ⚠️ 加这一行会打断一个包,而失败形状是链接期缺符号

实测:加行之后 `--target x86_64-windows-musl` 构建失败于

```
ld.lld: error: undefined symbol: __declspec(dllimport) MultiByteToWideChar
```

对比两种拼法的链接行:

```
-gnu   -lkernel32 -lntdll -lshell32 -lsynchronization  ← 四个 Win32 导入库
-musl  （一个都没有）
```

真因在 `openkal-windows@0.1.3` 的清单里:

```toml
[target.'cfg(all(windows, env = "gnu"))'.build]
ldflags = ["-lntdll", "-lsynchronization", "-lshell32", "-lkernel32"]
```

⭐ **包用 `env = "gnu"` 表达「对象 ABI 是 Itanium」**,而那个键在 Windows 上
同时承载 C 库与对象 ABI 两件事(§5.1)。新目标一来条件就落空,
而落空的表现是**链接期缺符号**,不是「配置没生效」这种一眼可见的形状。

⚠️ **这条是加行连带暴露的,不是加行造成的。** 那个条件今天就在表达一件
它表达不了的事;只是在 `-gnu` 是唯一非 MSVC 拼法时,写错和写对不可区分。

### 5.2.3 前置是一条链,不是一处

实测把整条链走了一遍,前置比预想的深一层:

```
mcpp 加 x86_64-windows-musl 这一行
    ↑ 需要
openkal-windows ≥ 0.1.4        条件改为 not(env = "msvc")     ✅ 已发布并进索引
    ↑ 需要
openkal-musl 放开对它的钉死     `openkal-windows = "0.1.3"`   ❌ 未做
```

`openkal-musl@0.3.3` 的清单里写的是**精确版本**:

```toml
openkal-windows = { version = "0.1.3", features = ["standalone"] }
```

所以索引里有了 0.1.4 也到不了消费者 —— lockfile 忠实地钉在 0.1.3,
构建仍然撞上同一组缺符号:

```
ld.lld: error: undefined symbol: __declspec(dllimport) MultiByteToWideChar
```

⚠️ **这不是新增那一行造成的**,而是那一行让一条既有的版本钉死浮出水面。
今天 `-gnu` 是唯一非 MSVC 拼法,钉在 0.1.3 与钉在 0.1.4 行为相同,
所以钉死是隐形的。

⭐ **但它不阻塞 mcpp 侧的改动。** 新增一行目标是词表扩充,其正确性由
「名字是否映到正确的 LLVM 三元组、身份是否唯一」判定,而不由某个生态包
今天能不能构建判定。生态链条另行推进:`openkal-musl` 放宽依赖 → 发版 →
进索引,之后 `x86_64-windows-musl` 才端到端可用。

在那之前,该目标的档是 `preview` 而非 `verified` —— 而 `verified` 的定义
正是「构建**并运行**过」,所以档位本身就诚实地记录了这个状态。

**这使 §5.1(cfg 轴)从「应做」升为 §5.2 的前置**:必须先让包能表达
「对象 ABI 是 Itanium」而不借用 `env = "gnu"`,否则加行就是把一个
生态包弄坏。可选形状(未实测,需要设计):

- `cfg(all(windows, not(env = "msvc")))` —— 包侧一行改完,零引擎改动
- 引入 `cfg(abi = "itanium")` —— 更准,但要新增一条 cfg 轴

⚠️ 第一条今天就能用且不需要引擎改动,应先验证它是否覆盖全部三种工具链
(注释里说这个包「为三种工具链而写」)。

### 5.3 `host_can_serve()` 回答了两个问题

该谓词的每一条分支都在问「**载荷**是否覆盖这个(宿主,目标)对」。
在构建期体系下这个问题不成立 —— C 库来自图,编译器天生交叉。

后果实测于 `mcpp toolchain list`:

```
     TARGET                  NOTE                  TOOLCHAIN         STATUS
     x86_64-windows-gnu      PE, static, cross     gcc 16.1.0        installed
```

一个 openkal 工程用的是 `llvm@22.1.8`,与这一行显示的 `gcc 16.1.0` 无关。
**列表回答的问题已经不是使用者要问的问题。**

⚠️ 不建议现在拆分该谓词:它有 6 个调用点,而拆分需要先回答
「一个尚未解析依赖图的命令(`toolchain list`)如何知道自己在哪种体系里」。
先把 §5.2 做掉,那一行会让 `x86_64-windows-musl` 出现在列表里且
`TOOLCHAIN` 列显示 `—`,本身就是一次诚实的表达。

### 5.4 `tier` 的含义在两种体系下不同

`verified` 在裸机行的注释里定义为「**镜像被构建并且被运行过**」。
而 `x86_64-windows-gnu` 标 `verified` 指的是预构建路径被验证过 ——
构建期路径是另一回事,今天由 openkal 的 3×3 矩阵单独覆盖。

⚠️ 同一个词标注两种不同的验证,而表里没有任何东西区分它们。
本文不提出改法(加一列或拆两张表都有代价),只记录这个歧义。

---

## 6. 落地顺序与依赖

```
5.2（windows-musl 一行）      ← 先做。它连带修正 5.1 的一半语义
    │
5.1（check_request 判据）     ← 依赖 5.2 的语义,不依赖它的代码
    │
5.3（host_can_serve 拆分）    ← 先做完 5.2 再评估;可能不必做
    │
5.4（tier 歧义）              ← 仅记录,不改
```

## 7. 本文刻意没有断言的事

- **没有断言 `x86_64-windows-musl` 该是哪个档。** `verified` 的定义是
  「构建**并运行**过」,而在 Linux 宿主上运行 PE 需要 wine。
  ⚠️ openkal 的 CI 已有 wine 步骤,所以这是可测的 —— 但要先测再标,
  不能先标再说。
- **没有断言 5.3 拆分后 `toolchain list` 该显示什么。** 完整方案要求它
  解析依赖图,而那会让一条只想看看有哪些目标的命令变慢。
- **没有断言 macOS 上是否也存在同一个命名缺陷。** `aarch64-macos`
  不带 env 段,所以 §5.2 的形状在那里不出现;但 c-abi 实测同样是
  `musl (graph)`,而目标名里没有任何地方说明这一点 —— 这与 Windows
  是同一个问题的另一种表现,本文未展开。
