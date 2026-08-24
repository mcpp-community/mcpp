# mcpp 目标侧设计

2026-08-24 · 设计定稿

**读者**:mcpp 贡献者、索引作者、运行时/平台包作者。
**推导过程**(十余条实测、缺陷考古、落地顺序)在
`2026-08-24-graph-target-side-optimization-plan.md`;本文只讲设计。

---

## 0. 一句话

> **一次构建的目标侧由五个层构成;每层恰好一个供给者;供给者可以是编译器载荷、
> 预制载荷或依赖图;引擎知道有哪五层,永远不知道有哪些实现。**

---

## 1. 架构

### 1.1 五层

| 层 | 是什么 | 实现举例 |
|---|---|---|
| `compiler` | 谁在编译 | `llvm` / `gcc` / `msvc` |
| `compiler-runtime` | 编译器自己的运行时:builtins、展开器 | `compiler-rt`+`libunwind` / `libgcc` |
| `kernel-abi` | 平台接口:系统调用或它的等价物 | `linux` / `windows` / `darwin` / `openkal` / 无 |
| `c-abi` | C 库 | `glibc` / `musl` / `picolibc` / 无 |
| `c++-abi` | C++ 库与它的 ABI 运行时 | `libc++`+`libc++abi` / `libstdc++` / MSVC STL / 无 |

**是层的三个条件**,少一个就不该是层:野外至少有两个可互换的实现;
可以独立于其它层被替换;对下一层有明确的「被谁配置过」关系。

⚠️ **`compiler-runtime` 独立于 `c++-abi`。** builtins(`__udivti3` 一类)
是一个**纯 C 程序**就需要的东西。把它算进 C++ 运行时,等于说 C 程序不需要整数除法。

⚠️ **`kernel-abi` 在传统栈上没有名字。** 一个 C 库直接发系统调用,那道缝没被命名。
命名它,才使得一份 C 库源码能坐在四个平台上。

### 1.2 四来源

| 来源 | 含义 | 何时可知 |
|---|---|---|
| `Payload` | 编译器载荷自带 | 依赖解析**之前** |
| `Xpkg` | 一份被点名的预制载荷 | 依赖解析**之前** |
| `Graph` | 依赖图里的包 | 依赖解析**之后** |
| `None` | 没有,且这是一个陈述 | — |

⚠️ **`None` 是答案不是缺口。** 裸机没有内核;不依赖 C 库就是没有 C 库。

⭐ **两个来源在图之前可知、两个在图之后才可知 ⇒ 目标侧只能在依赖解析之后
解析一次。** 在那之前的任何推断,都是对一个尚不存在的事实作推断;
多处推断必然互相矛盾。

### 1.3 四规则

**规则一 · 每层恰好一个供给者。**
C 库、内核接口、C++ 运行时是**互斥的选择**,不是可叠加的贡献。
两个供给者在解析期报错并指出双方。
⚠️ 失败模态是这条规则的理由:选错不会链接失败,会得到一个能跑、偶尔崩的程序。

**规则二 · 每层必须为它下面那层配置过。**
一份 libc++ 的 `__config_site` 记录了它对着哪个 C 库配置;`libgcc` 是为 gcc 配置的。
**「为谁配置过」是可声明的事实,不是可推断的。**

推论:载荷的 C++ 运行时只在 C 库也来自同一载荷时可用;`compiler-runtime`
必须与 `compiler` 同族;声明了 `requires` 的包遇到不匹配的编译器必须被拒绝。

**规则三 · 引擎只在跨来源时接线。**

| 组合 | 谁表达两层的关系 | 引擎 |
|---|---|---|
| 两层都在 `Graph` | 包之间的普通依赖 | 不介入 |
| 两层都在 `Payload` | 载荷自洽 | 不介入 |
| 一层预制、一层在图 | 只有引擎同时知道两边的地址 | 必须接线 |

⇒ **把一层从预制挪进图,引擎要做的事就少一件。** 这是「一份源码到达四个平台
却不需要改引擎」的机制原因。

**规则四 · 引擎硬编码层名,永不硬编码实现名。**
五个层名是编译进引擎的闭集;`openkal` / `musl` / `libc++` / `picolibc`
不出现在引擎任何一行代码里。

> 层由 C/C++ 构建模型固定、不增长,所以可以硬编码。
> 实现不可以,因为增长正是它们要做的事:**生态的组合是 N×M,包数是 N+M。**

⚠️ 推论一:**目标侧包不走预构建分发** —— 预构建资产数 = 目标 × 编译器版本 × C 库,
回到 N×M 那一侧。冷构建代价实测为「每台机器每(包版本 × 目标)一次」,可接受;
若将来成为问题,答案是**共享构建缓存**而不是预构建包。

⚠️ 推论二:**「目标侧从哪来」不得由工具链的名字表达。**
工具链族的命名空间里只能有编译器。

### 1.4 三元组是请求,`TargetSide` 是事实

`<arch>-<os>-<env>` 三个字段承载四条正交的轴:机器、平台接口、对象格式+调用 ABI、
C 库。第四条在图模型下不由名字决定,所以:

- `env` **空 = 未指定**,由供给方决定;
- `env` **非空 = 一条请求**,与解析出的事实矛盾时**拒绝**,不并排打印;
- 三元组永远不是「C 库是谁」的答案,`TargetSide.cAbi` 才是。

⚠️ macOS 今天就是这个形状(三元组没有 env 段),它是这条设计已被验证可行的证据。

---

## 2. 三个表面

⭐⭐ **五层是引擎的词汇,不是使用者的。** 它出现在报告与诊断里,不出现在清单里。

> 报告是**诊断输出** —— 可以也应该用精确的内部词汇,那正是它有用的原因。
> 清单是**配置输入** —— 必须用使用者本来就有的词。

| 受众 | 需要理解什么 | 词汇 |
|---|---|---|
| **使用者** | 什么都不需要 | 三元组、`[toolchain]`、`[dependencies]` |
| **普通库作者** | 传统平台轴(与今天一样) | `cfg(os / arch / family / env)`、`sources` |
| **运行时 / C 库 / 平台包作者** | 全部五层 | `provides` / `requires` / `cfg(kernel-abi / c-abi / c++-abi)` |

三条边界:

1. 层名不出现在**使用者**的清单里 —— 三元组和依赖表达一切;
2. 层名不出现在**普通库作者**的清单里 —— 他写 `cfg(windows)`,不需要学新东西;
3. 层名只属于**第三类作者**。一个库只有在刻意为某个内核接口写后端时才进入
   这套词汇 —— 那一刻它已经是这套生态的参与者。

⚠️ 判断新语法放哪一层,问:**最不懂行的那类受众会不会被迫看到它?** 会,就放错了。

---

## 3. 语义

### 语义一 · 每一层都是一条依赖

**使用者换掉任何一层的方式,永远是加一条依赖。** 没有第二种。

```toml
[dependencies]
openkal-llvm-runtime = "0.1"     # 一条依赖换掉三层
```

推论:**没有 `c-abi = …` 这类键。** `x86_64-linux-musl` 已经说了 musl,
再写一遍是同一句话说两遍。

推论:**零 libc 档不需要拼写。** 不依赖 = 没有。
使用者不需要知道「零 libc 档」这个概念存在。

推论:**一块板子要哪个 C 库,写在描述这块板子的那个包里。**

```toml
# acme/riscv-virt-rt
[dependencies]
openkal  = "0.6"
picolibc = "1.8"        # ⭐ 板子的性质,包自己说;新板子不需要 mcpp 发版
```

### 语义二 · 能力由声明得来,永不由名字猜

供给者**声明**它供给哪一层、接口叫什么、那一层的版本是多少。
图里的包与预制载荷用同一套语义:

```toml
# 依赖图里的包
provides = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
requires = ["mcpp:compiler=llvm"]
```

```lua
-- 预制载荷的描述符:布局描述而非构建输入,所以形状不同(它不被构建)
provides = { "mcpp:c-abi=musl" }
layer = { ["c-abi"] = { path = "${arch}-linux-musl", version = "1.2.5" } }
```

三个后果:接口名不再从包名切出来(载荷叫什么是打包的事);层的版本不再是
载体的版本;**载荷里哪一部分属于哪一层是被声明的** ⇒ 消费一层不会连带消费另一层。

### 语义三 · 供给的细节属于 `[build]`

`std-module` 是一个 `.cppm`,`std-module-flags` 是一组 flag ——
与 `sources` / `include_dirs` 同类,`[build]` 就是它们的家。

```toml
[build]
sources           = ["llvm/libcxx/src/**/*.cpp", "…"]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc", "-nostdinc++"]
```

⭐ 放进 `[build]` 白得条件化能力:

```toml
[target.'cfg(c-abi = "musl")'.build]
std-module-flags = ["-D_GNU_SOURCE"]              # musl 的 locale 层需要
```

⚠️ 校验:**声明了 `std-module` 却没有对应的 `provides` 条目,是错误,不是被忽略。**

### 语义四 · 包适配已解析的目标侧,而不是被告知

一份 libc++ 源码可以配 musl、glibc、picolibc。**它不被告诉是哪一种,它去问。**

```toml
[target.'cfg(c-abi = "musl")'.build]
include_dirs = ["config/musl"]

[target.'cfg(c-abi = "picolibc")'.build]
include_dirs = ["config/picolibc"]
```

⇒ 使用者一个字都不用多写。要求一个 `features = ["musl"]`,
就是让他把已经说过的话再说一遍,而且两处可能不一致。

⚠️ **限制**:`cfg(*-abi = …)` 只能用于 `[build]`,不能用于 `[dependencies]`
(目标侧在依赖解析之后才有,用它决定依赖会成环)。
「不同 C 库要不同**依赖**」的出路是拆包,或依赖并集在 `[build]` 里选源码。
⚠️ 这条限制是**平的** —— 不因为 `c-abi` 恰好来自三元组就放宽:
一个清单不该「有时能写有时不能」。

**真正的 feature 仍然是 feature。** 判据:
**能从目标侧推出来的不做 feature;推不出来的才是 feature。**

### 语义五 · 报告用使用者自己的拼写

使用者在 `[dependencies]` 里写短名,命名空间由索引解析、`mcpp.lock` 记录。
报告引入他不使用的拼写,是在制造第二套词汇。

```
c++-abi   libc++   (openkal-llvm-runtime@0.1.1, graph)
```

全限定名只出现在三处:`-v`;**同一次构建里两个短名相同时**(此时短名不再是标识符);
诊断信息里。

---

## 4. 使用侧

### 4.1 报告:按需暴露

**默认只显示不来自编译器载荷的层。** 全部来自载荷 ⇒ 只有目标那一行。

```
      Target x86_64-linux-gnu                                    ← 零配置:一行
```

```
      Target x86_64-windows → x86_64-w64-windows-gnu
             kernel-abi        openkal      (openkal-windows@0.1.3, graph)
             compiler-runtime  compiler-rt  (openkal-llvm-runtime@0.1.1, graph)
             c-abi             musl         (openkal-musl@0.3.3, graph)
             c++-abi           libc++       (openkal-llvm-runtime@0.1.1, graph)
```

`-v` 显示全部五层。⚠️ **诊断是例外**:一条错误必须打印它所依据的全部层,
包括来自载荷的,否则读者看不到判断的依据。

约束:打**解析结果**不打清单意图(清单会过期,结果不会);
**接口与实现是两列**(`openkal` 是接口,`openkal-windows` 是实现)。

### 4.2 诊断:说决定,不说后果

⚠️ **一条编译器或链接器的原话,几乎总是一次诊断缺失。**

### 4.3 不必重复自己

> **mcpp 修订它自己的默认,永不修订你的。**

用户用明确命令设下的全局默认,不得被目标表的行推翻。真覆盖了,状态行要说出来。

### 4.4 逃生口

`[target.<triple>].toolchain` —— 只给某个目标换编译器。**这是唯一一个。**

判据:**一个键能进使用者表面,当且仅当它表达的东西三元组、工具链、依赖
三样都说不出来。**

---

## 5. 使用示例

### 甲 · 使用者(什么都不需要理解)

**5.1 零配置**

```toml
[package]
name    = "hello"
version = "0.1.0"
```
```
$ mcpp build
      Target x86_64-linux-gnu
    Finished dev [unoptimized + debuginfo] in 0.4s
```

**5.2 交叉 —— 清单一个字不加**

```
$ mcpp build --target x86_64-linux-musl
      Target x86_64-linux-musl → x86_64-unknown-linux-musl
```

**5.3 换 C++ 运行时(要 llvm 而不是 gcc)**

```toml
[dependencies]
llvm-runtime = "0.1"
```
```
$ mcpp toolchain default llvm
$ mcpp build --target x86_64-linux-musl
      Target x86_64-linux-musl → x86_64-unknown-linux-musl
             compiler-runtime  compiler-rt  (llvm-runtime@0.1.0, graph)
             c-abi             musl         (musl@1.2.5, graph)
             c++-abi           libc++       (llvm-runtime@0.1.0, graph)
```

写的**全部**内容:一条依赖 + 一条命令。没有 `[target.…]`,没有层名,没有地址。

**5.4 openkal 全栈交叉到 Windows**

```toml
[dependencies]
openkal-llvm-runtime = "0.1"
```
```
$ mcpp build --target x86_64-windows
```

⭐ `--target x86_64-windows` **没有 env 段,这是正确写法** —— C 库由图决定,
使用者不该在三元组里对它作断言。写 `x86_64-windows-gnu` 会被拒绝(§5.12b)。

**5.5 裸机:要与不要 C 库,都只是「有没有那条依赖」**

```toml
[dependencies]
riscv-virt-rt = "0.4"          # 板级支持包,它自己依赖 picolibc
```
```
      Target riscv64-none-elf
             kernel-abi        —
             c-abi             picolibc     (picolibc@1.8.12, graph)
```

不要任何 C 库 —— **不写就是不要**:

```toml
[dependencies]
openarch = "0.7"               # 只要机器机制
```
```
             kernel-abi        —
             c-abi             —
```

### 乙 · 普通库作者(用他一直在用的轴)

**5.6 纯算法库:零个 `cfg`**

```toml
[package]
namespace = "acme"
name      = "json"
version   = "1.0.0"
```

**5.7 有平台后端的库:传统 os 轴**

```toml
[target.'cfg(os = "linux")'.build]
sources = ["src/backend_epoll.cpp"]

[target.windows.build]
sources = ["src/backend_iocp.cpp"]
```

⭐ **一个层名都没有。** 普通库作者永远不需要知道 mcpp 有五层。

### 丙 · 运行时 / C 库 / 平台包作者(需要理解五层)

**5.8 平台实现者:供给 `kernel-abi`**

```toml
[package]
namespace = "acme"
name      = "openkal-freertos"
version   = "0.1.0"

provides = ["mcpp:kernel-abi=openkal"]

[dependencies]
openkal = "0.6"                        # 规范包,声明接口

[build]
sources = ["src/**/*.cpp"]
```

**引擎零改动,索引零新增。** 使用者加一条依赖就到达了一个 mcpp 从未听说过的平台。

**5.9 运行时实现者:供给两层,支持三种 C 库**

```toml
[package]
namespace = "acme"
name      = "llvm-runtime"
version   = "0.1.0"

provides = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
requires = ["mcpp:compiler=llvm"]

[build]
sources = [
  "llvm/compiler-rt/lib/builtins/**/*.c",
  "llvm/libunwind/src/**/*.cpp",
  "llvm/libcxxabi/src/**/*.cpp",
  "llvm/libcxx/src/**/*.cpp",
]
include_dirs      = ["llvm/libcxx/include", "…"]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc", "-nostdinc++"]

[target.'cfg(c-abi = "musl")'.build]
include_dirs     = ["config/musl"]
std-module-flags = ["-D_GNU_SOURCE"]

[target.'cfg(c-abi = "picolibc")'.build]
include_dirs     = ["config/picolibc"]
std-module-flags = ["-D_LIBCPP_HAS_NO_THREADS"]
```

三处落点:**两个 `provides`**(它确实供给两层);**`requires`** 让「用 gcc 编它」
在编译开始前被拒;**std 模块的键在 `[build]` 里**,因此可以按 C 库条件化。

**5.10 板级支持包:说出这块板子要哪个 C 库**

```toml
[package]
namespace = "acme"
name      = "riscv-virt-rt"
version   = "0.4.0"

provides = ["mcpp:kernel-abi=openkal"]

[dependencies]
openkal  = "0.6"
picolibc = "1.8"                       # ⭐ 板子的性质,一行

[build]
sources = ["src/**/*.cpp", "src/start.S"]
```

**5.11 把一份现成载荷包装成能力供给者**

```toml
[package]
namespace = "acme"
name      = "picolibc"
version   = "1.8.12"

provides = ["mcpp:c-abi=picolibc"]

[xlings]
deps = ["xim:picolibc-riscv@1.8.12"]

[build]
sources      = []                                  # 不编译任何东西
include_dirs = ["${xim:picolibc-riscv}/include"]
ldflags      = ["-L${xim:picolibc-riscv}/lib", "-lc"]
```

⚠️ `${xim:<名>}` 是本设计新增的机制(§6),今天不存在。
名字取 `[xlings] deps` 里的**包名**;同一份清单不可能列出同名包的两个版本,
所以无歧义。

### 5.12 四种出错

**(a) 编译器与运行时不匹配**
```
error: this C++ runtime requires an llvm-family compiler.
         c++-abi   libc++      (llvm-runtime@0.1.0, graph)
         compiler  gcc@16.1.0  (mcpp's default)
       Yours outranks mcpp's default:
           mcpp toolchain default llvm@22.1.8
```

**(b) 三元组的请求与事实矛盾**
```
error: this build requests the `gnu` C ABI, and its graph supplies `musl`.
         requested  gnu    (from --target x86_64-windows-gnu)
         resolved   musl   (openkal-musl@0.3.3, graph)
       Write `--target x86_64-windows` to let the graph decide.
```

**(c) 两个包供给同一层**
```
error: two packages supply the C ABI, and it is a choice rather than a contribution.
         mcpplibs/openkal-musl@0.3.3   (via openkal-llvm-runtime)
         acme/tinylibc@0.2.0           (a direct dependency)
```
⚠️ 这一条打全限定名 —— 两个短名并列时短名不再是标识符(语义五)。

**(d) 没有人供给某一层**
```
error: nothing supplies this target's C library.
         compiler  llvm@22.1.8   (payload)
         c-abi     —
       Depend on a package that provides `mcpp:c-abi`.
```
⚠️ (d) 打印了一行 `(payload)` —— 诊断必须展示判断依据(§4.1)。

### 5.13 每类人要学几个概念

| 受众 | 概念 | 形式 |
|---|---|---|
| 使用者 | 什么都不学 | `mcpp build` |
| 使用者 | 目标三元组 | `--target x86_64-linux-musl` |
| 使用者 | 工具链 | `mcpp toolchain default llvm` |
| 使用者 | 依赖 | `[dependencies]` 一行 |
| 使用者(逃生口) | 按目标换编译器 | `[target.X].toolchain` |
| 普通库作者 | 传统平台轴 | `cfg(os / arch / family / env)` |
| 运行时/ABI 作者 | 供给与需求 | `provides` / `requires` |
| 运行时/ABI 作者 | 按目标侧适配 | `cfg(kernel-abi / c-abi / c++-abi)` |

⭐ 使用者:**4 个日常概念 + 1 个逃生口,零个层名**。
⭐ 普通库作者:**1 个,与今天完全一样**。
⭐ 只有最后两行需要理解五层,而写这类包的人本来就在做这件事。

---

## 6. 本设计新增的机制

| # | 机制 | 用途 |
|---|---|---|
| M1 | `mcpp:compiler` / `mcpp:compiler-runtime` 两个层名 | 五层完整 |
| M2 | `requires = ["mcpp:<层>=<实现>"]` | 规则二在包侧的表达 |
| M3 | 载荷描述符的 `provides` + `layer.<名>.{path,version}` | 语义二在预制侧 |
| M4 | `cfg(kernel-abi / c-abi / c++-abi = …)`,仅限 `[build]` | 语义四 |
| M5 | `${xim:<名>}` 路径替换 | 包能引用一份载荷 ⇒ 语义一的板级包形态 |
| M6 | `std-module*` 从 `[package]` 移入 `[build]` | 语义三 |

⚠️ M5 有一个前置缺陷要一起修:**依赖包的 `[xlings] deps` 今天一个都不装**
(只有根工程的会装)。判据必须是「把它拿走再装回来」,不能只看构建是否绿。

---

## 7. 明确不做

| 不做 | 理由 |
|---|---|
| 开放 `os` / `env` token 表 | 把语义责任推给包作者;引擎有十余处行为要问这个字段 |
| 开放编译器族 | 族的差异是引擎必须知道的,不是数据能描述的 |
| 命名空间准入门槛 | 把瓶颈换个地方;违反规则四;挡不住真风险(不需要 `provides` 也能破坏构建) |
| 目标侧包预构建分发 | 资产数回到 N×M 那一侧;冷构建成本实测可接受 |
| 跨族借用编译器运行时 | 让同一次构建里的链接互相不一致;规则二的直接违反 |
| 为报告里的信息新增清单字段 | 清单陈述意图会过期,报告陈述结果不会 |
| `c-abi` / `compiler` 一类清单键 | 三元组与依赖已经说得出;层名不进使用者表面 |
| 发明新的三元组语法 | 三元组要与 LLVM/GNU 写法互认;要改的是「空」的含义,不是语法 |

---

## 8. 判据

不是「测试通过」,是这五条同时成立:

1. **一份新平台的实现,发一个包就能被使用** —— 引擎零改动、索引零改动;
2. **报告里每一行都能被某个声明解释**,没有一行是猜出来的;
3. **每个「编译器原话」的失败,都有一条更早的 mcpp 诊断**;
4. **零配置工程的清单里没有任何一行提到目标侧**;
5. **同一个事实在代码里只有一处推导** —— 新增消费者时去问那一处,
   而不是新增第 N 条并行判据。

⚠️ 第 5 条最容易退化也最贵:目标侧的判据曾在三处各推一遍并互相矛盾,
收敛它们的代价远大于当初写对的代价。

---

## 9. 与当前实现的差距

| 设计条款 | 现状 |
|---|---|
| §1.1 五层 | 三层;`compiler` 与 `compiler-runtime` 未建模 |
| §1.2 四来源 | ✅ 已有 |
| §1.3 规则一 | ❌ 冲突时图遍历顺序第一个静默胜出 |
| §1.3 规则二 | ⚠️ 半条 |
| §1.3 规则三 | ⚠️ 图×图、载荷×载荷成立;跨来源未接线 |
| §1.3 规则四 | ✅ 层名侧成立;⚠️ 工具链族里混入了一个目标侧策略 |
| §1.4 三元组是请求 | ❌ 解析时把「未指定」折叠成了 `gnu` |
| §2 三个表面 | ⚠️ 报告与清单未分层;`sysroot` 让层泄漏到使用者面 |
| 语义一 C 库是依赖 | ❌ 由引擎表的列供给 |
| 语义二 声明而非猜 | ⚠️ 图侧声明,载荷侧从包名切 |
| 语义三 `[build]` | ❌ 在 `[package]` 下,且不可条件化 |
| 语义四 包去问 | ❌ `cfg` 只能问三元组的字段 |
| 语义五 报告拼写 | ✅ 已是短名 |
| §4.3 不重复自己 | ❌ 全局默认会被目标表的行推翻 |
| M5 `${xim:<名>}` | ❌ 不存在 |

落地顺序见 `2026-08-24-graph-target-side-optimization-plan.md` §14.5 / §16.7。
