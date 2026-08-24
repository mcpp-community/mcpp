> ⚠️ **本文是推导过程中的工作稿,已被 `2026-08-24-target-side-design.md` 取代。**
> 它保留了几次自我修正的痕迹(`[layer.X]` 段、`sysroot` 保留论、报告打全限定名),
> 那些结论**都已被推翻**。要读设计,读定稿;本文只在想知道「为什么不那样」时有用。

# 目标侧架构:五层、四来源、四规则

2026-08-24。**这是设计文档,不是缺陷清单。**

`.agents/docs/2026-08-24-graph-target-side-optimization-plan.md` 记录了导出这份
设计的十余条实测与缺陷考古;本文只陈述**设计本身**,正面表述,不引用任何一个
具体 PR。它是 `docs/spec/SPEC-002` 的种子。

三个维度分别在 §1 / §2 / §3:**架构**、**语义一致性**、**使用侧**。

---

## 0. 一句话

> **一次构建的目标侧由五个层构成;每层恰好一个供给者;供给者可以是载荷、
> 预制包或依赖图;引擎知道有哪五层,永远不知道有哪些实现。**

这句话里每一个词都承重,§1 逐个展开。

---

## 1. 架构

### 1.1 五层

一次 C/C++ 构建的目标侧,是五样东西的组合。它们是**层**,不是**选项**:
每一层都为它下面那层配置过,换掉任何一层都要求上面的层同意。

| 层 | 是什么 | 实现举例 |
|---|---|---|
| `compiler` | 谁在编译 | `llvm` / `gcc` / `msvc` |
| `compiler-runtime` | **编译器自己的运行时**:整数/浮点 builtins、展开器 | `compiler-rt`+`libunwind` / `libgcc` / MSVC 的 |
| `kernel-abi` | 平台接口:系统调用或它的等价物 | `linux` / `windows` / `darwin` / `openkal` / 无 |
| `c-abi` | C 库 | `glibc` / `musl` / `picolibc` / 无 |
| `c++-abi` | C++ 库与它的 ABI 运行时 | `libc++`+`libc++abi` / `libstdc++` / MSVC STL / 无 |

**为什么恰好是这五个。** 每一层都满足三个条件,少一个就不该是层:

1. **野外至少有两个可互换的实现** —— 否则它不是接缝,是常量;
2. **可以独立于其它层被替换** —— 否则它属于相邻的层;
3. **对下一层有明确的「被谁配置过」关系** —— 这是规则二能成立的前提。

⚠️ **`compiler-runtime` 是独立的一层,不是 `c++-abi` 的一部分。**
builtins(`__udivti3`、`__muloti4`…)是**一个 C 程序**就需要的东西。把它算作
C++ 运行时的一部分,等于说一个纯 C 程序不需要整数除法 —— 而这个错误已经
被实测过:一个纯 C 程序交叉到 macOS 时,判据问「有没有 C++ 运行时」并答「没有」,
于是链接行保留了载荷自己的 libc++ 并把一个 Linux 共享对象递给了 Mach-O 链接器。

⚠️ **`kernel-abi` 是独立的一层,而在传统栈上它没有名字。**
一个 C 库直接发系统调用或直接调平台入口,那道缝没有被命名。命名它,
才使得一份 C 库源码能坐在四个平台上 —— 这是这套生态的核心贡献。

### 1.2 四来源

每一层的供给者来自四个地方之一(`targetside::Origin`):

| 来源 | 含义 | 何时可知 |
|---|---|---|
| `Payload` | 编译器载荷自带 | 依赖解析**之前** |
| `Xpkg` | 一份被点名的预制载荷 | 依赖解析**之前** |
| `Graph` | 依赖图里的包 | 依赖解析**之后** |
| `None` | 没有,且这是一个陈述 | — |

⚠️ **`None` 是答案,不是缺口。** 裸机没有内核,零 libc 档没有 C 库。
「这一层没有供给者」与「这一层还没解析」必须是两个可区分的状态。

⭐ **四来源里两个在图之前可知、两个在图之后才可知,所以目标侧只能在
依赖解析之后解析一次。** 在那之前作任何猜测,都是对一个尚不存在的事实猜测;
多处猜测必然互相矛盾。

### 1.3 四规则

**规则一 —— 每层恰好一个供给者。**

C 库、内核接口、C++ 运行时不是**可叠加的贡献**,是**互斥的选择**。
两个供给者是错误,必须在解析期拒绝并指出双方。

> 与 Cargo 的 `links` 键同构:全图至多一个包声明某个值。
> ⚠️ 失败模态是这条规则存在的理由:选错不会链接失败,会得到一个能跑、
> 偶尔崩的程序。

**规则二 —— 每层必须为它下面那层配置过。**

一份 libc++ 的 `__config_site` 记录了它是对着哪个 C 库配置的;
一份 `libgcc` 是为 gcc 配置的。**「为谁配置过」是可声明的事实,不是可推断的。**

推论:
- 载荷的 C++ 运行时只在 C 库也来自同一载荷时可用;
- `compiler-runtime` 必须与 `compiler` 同族 —— 否则一次链接解析 `__udivti3` 的方式
  会与同一次构建里其它链接不同;
- 图供给的 C++ 运行时若声明 `requires = ["mcpp:compiler=llvm"]`,gcc 必须被拒绝。

**规则三 —— 引擎只在「跨来源」时接线。**

| 组合 | 谁表达两层之间的关系 | 引擎 |
|---|---|---|
| 两层都在 `Graph` | 包之间的普通依赖 | 不介入 |
| 两层都在 `Payload` | 载荷自洽 | 不介入 |
| 一层预制、一层在图 | **只有引擎同时知道两边的地址** | 必须接线 |

⇒ **把一层从预制挪进图,引擎要做的事就变少一件。** 这不是巧合,
是规则三的直接推论,也是「一份源码到达四个平台却不需要改引擎」的原因。

**规则四 —— 引擎硬编码层名,永不硬编码实现名。**

`compiler` / `compiler-runtime` / `kernel-abi` / `c-abi` / `c++-abi` 是编译进引擎的
闭集。`openkal` / `musl` / `libc++` / `picolibc` 不出现在引擎任何一行代码里。

**层名可以硬编码,因为层由 C/C++ 构建模型固定、不增长。
实现不可以,因为增长正是它们要做的事:生态的组合是 N×M,而包数是 N+M。**

⚠️ 这条规则的推论之一:**目标侧包不该走预构建分发。**
预构建资产数 = 目标 × 编译器版本 × C 库,那是 N×M 那一侧;源码包是 1。
冷构建的代价已实测为「每台机器每 (包版本 × 目标) 一次」(跨工程命中),
不构成放弃这条规则的理由。若它将来成为问题,答案是**共享构建缓存**,
不是预构建包 —— 那保留全部三条性质而只消掉重算。

### 1.4 目标三元组在这个模型里是什么

**三元组是一个请求,`TargetSide` 是事实。**

`<arch>-<os>-<env>` 三个字段承载四条正交的轴:机器、平台接口、对象格式+调用 ABI、
C 库。第四条在图模型下不再由名字决定,所以:

- `env` **空 = 未指定**,由供给方决定 —— 图模式下这是正确写法;
- `env` **非空 = 一条请求**,若与解析出的事实矛盾,**拒绝**,不并排打印;
- 三元组永远不是「C 库是谁」的答案,`TargetSide.cAbi` 才是。

⚠️ macOS 今天已经是这个形状(三元组没有 env 段),它是这条设计已被验证可行的证据。

---

## 2. 语义一致性

七条。每条是一句「某样东西恰好表示一件事」。

### S1 — 三元组是请求,`TargetSide` 是事实

`env` 三态:缺席(未指定)/ 指定 / 与事实矛盾(拒绝)。
与 `[target.X]` 的 C 库三态一一对应,不是新发明。

### S2 — `cfg()` 按事实求值,不按名字

包想问的是「C 库是不是 musl」,不是「三元组的第三段是不是 `musl`」。

```toml
[target.'cfg(c-abi = "musl")'.build]
[target.'cfg(kernel-abi = "openkal")'.build]
[target.'cfg(c++-abi = "libc++")'.build]
```

⚠️ 这三个维度只能用于 `[build]` 段。`[dependencies]` 里必须**显式拒绝** ——
`TargetSide` 在依赖解析之后才有,用它决定依赖会成环。这条限制要报错,
不能留给使用者去撞。

### S3 — 每层恰好一个供给者

规则一的语义面。冲突在解析期报错,错误里指出双方及各自的引入路径。

### S4 — 能力由**声明**得来,永不由**名字**猜

⭐ 这条是本设计与现状差距最大的一条。

一个供给者必须**声明**它供给哪一层、接口叫什么、那一层的版本是多少。
图里的包与预制载荷用**同一套声明**:

```toml
# 依赖图里的包
provides = ["mcpp:c++-abi=libc++", "mcpp:compiler-runtime=compiler-rt"]
```

```lua
-- 预制载荷的描述符 —— 布局描述,不是构建输入(见 S5 末)
provides = { "mcpp:c-abi=musl" }
layer = {
  ["c-abi"] = {
    path    = "${arch}-linux-musl",   -- 这一层在载荷里的位置
    version = "1.2.5",                -- 这一层的真实版本(不是载体的)
  },
}
```

三个后果,每个都消掉一处今天的混乱:

1. **接口名不再从包名切出来。** 一个叫 `musl-gcc` 的载荷可以诚实地声明
   「我供给 `c-abi`,接口是 `musl`」,报告写 `c-abi musl (…, prebuilt)`。
   **载荷叫什么名字是打包的事,不是语义的事。**
2. **层的版本不再是载体的版本。** musl 1.2.5 可以被说出来、被约束。
3. **载荷里哪一部分属于哪一层,是被声明的。** ⇒ 引擎只消费被声明的那个子目录,
   不会伸到别处去拿本不属于这一层的东西。

⚠️ 第 3 点是规则二的执行手段:**一个载荷同时装着两层的实现时,
消费其中一层不得连带消费另一层。**

### S5 — `provides` 说供给什么;细节放在它本来属于的地方

```toml
provides = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
requires = ["mcpp:compiler=llvm"]

[build]
sources           = ["llvm/libcxx/src/**/*.cpp", "..."]
include_dirs      = ["llvm/libcxx/include", "..."]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc", "-nostdinc++"]
```

⚠️ **本文初稿在这里发明了一个 `[layer.<名>]` 段,那是多余的。**

`std-module` 就是一个 `.cppm` 文件,`std-module-flags` 就是一组 flag ——
它们与 `sources` / `include_dirs` 是同一类东西,`[build]` 就是它们的家。
引擎自己的注释早就这么说了:

> The std module source is **one of this package's translation units in every
> way that matters**, and it reaches the C library's headers the same way the
> rest of them do.

⭐ **而放进 `[build]` 还白得一样能力:它立刻可以按目标侧条件化。**

```toml
[target.'cfg(c-abi = "musl")'.build]
std-module-flags = ["-D_GNU_SOURCE"]          # musl 的 locale 层需要

[target.'cfg(c-abi = "picolibc")'.build]
std-module-flags = ["-D_LIBCPP_HAS_NO_THREADS"]
```

这不是假想需求:`-D_GNU_SOURCE` 今天就写在一个包的 std-module-flags 里,
而它对 picolibc 是错的。`[layer.<名>]` 要重新发明一遍 `[target.'cfg(…)'.…]`
才能表达同一件事 —— **新段的代价不只是多一个段,是多一套条件化机制。**

⚠️ 保留的只有那条**校验规则**,它与放在哪一段无关:
**声明了 `std-module` 却没有对应的 `provides` 条目,是错误,不是被忽略。**
(今天是被静默跳过,于是一个写错的清单构建成功而行为不对。)

**预制载荷的描述符形状不同,而这是对的。** 一个包**被构建**,所以它的层细节
是构建输入;一份载荷**不被构建**,所以它的层细节是**布局描述**:

```lua
provides = { "mcpp:c-abi=musl" }
layer = { ["c-abi"] = { path = "${arch}-linux-musl", version = "1.2.5" } }
```

两者形状不同,因为它们性质不同 —— 强行对称会让其中一边说不出自己要说的话。

`requires` 是规则二在包侧的表达。引擎硬编码键名与层名,实现名来自清单。

### S6 — 报告用使用者自己的拼写

⚠️ **本文初稿在这里主张「报告一律打全限定名」,那是错的。**

理由是:使用者在 `[dependencies]` 里写的是**短名**(`openkal-llvm-runtime`),
命名空间由索引解析、由 `mcpp.lock` 记录,他在任何地方都不打那个前缀。
报告里引入一个他不使用的拼写,是在制造第二套词汇 —— 而消除第二套词汇
正是本设计的目的。

```
c++-abi   libc++   (openkal-llvm-runtime@0.1.1, graph)      ← 与清单里的写法一致
```

**「谁供给了我的 libc」这个信任问题不属于目标侧报告。** 它属于依赖解析:
一个短名解析到了错误的命名空间,影响的是**整个构建**而不只是目标侧;
`mcpp.lock` 已经逐条记录 `namespace`,那才是该看的地方。

全限定名出现在三处,且只在这三处:

- `-v` / `MCPP_VERBOSE=1`;
- **同一次构建里两个短名相同**时(此时短名不再是标识符);
- 诊断信息里(错误必须无歧义)。

### S7 — 层名硬编码,实现名永不硬编码

规则四的语义面,并给出边界:

| | 性质 | 增长 | 谁赋予语义 | 结论 |
|---|---|---|---|---|
| 层的组合 | 组合 | N×M | 包自己 | **数据** |
| 有哪些层 | 词汇 | 不增长 | 引擎 | 闭集 |
| 有哪些 os / env token | 词汇 | 加法,一年一两个 | **引擎**(对象格式、库命名、strip 适用性…十余处行为) | 闭集 |
| 有哪些编译器族 | 词汇 | 加法 | **引擎**(flag 拼写、模块模型、BMI 格式) | 闭集 |

⚠️ 闭集不是保守,是**语义责任的归属**:一个包定义的 opaque `os`,
回答不了引擎那十余处要问它的问题。

⚠️ 推论:**「目标侧从哪来」不得由工具链的名字表达。**
工具链族的命名空间里只能有编译器。

### S8 — 三种受众,三套词汇,互不外溢

⭐ 本设计有**三个**表面,不是两个。混淆它们是「语义不清晰」的主要来源。

| 受众 | 需要理解什么 | 词汇 |
|---|---|---|
| **使用者** | 什么都不需要 | 三元组、`[toolchain]`、`[dependencies]` |
| **普通库作者** | 传统的平台轴 | `cfg(os / arch / family / env)`、`[lib]`、`sources` |
| **运行时 / C 库 / 平台包作者** | 全部五层 | `provides` / `requires` / `cfg(kernel-abi / c-abi / c++-abi)` |

三条边界规则:

1. **层名不出现在使用者的清单里。** 使用者用三元组和依赖表达一切(§3.0)。
2. **层名不出现在普通库作者的清单里。** 一个 JSON 解析器写零个 `cfg`;
   一个有平台后端的库写 `cfg(windows)` / `cfg(os = "linux")` ——
   那是它一直在用的轴,不需要学任何新东西。
3. **层名只属于第三类作者。** 一个库只有在**刻意要为某个内核接口写后端**时
   才写 `cfg(kernel-abi = "openkal")` —— 那一刻它就不是普通库了,
   它是这套生态的参与者,理解分层是它的工作内容。

⚠️ 判断一个新语法该放进哪一层,问:**最不懂行的那一类受众会不会被迫看到它?**
会,就放错了。

---

### S9 — 一个包适配已解析的目标侧,而不是被告知

一个供给某层的包常常支持**多种**下层实现:同一份 libc++ 源码可以配 musl、
配 glibc、配 picolibc。**它不需要被告诉是哪一种,它去问。**

```toml
# acme/llvm-runtime —— 一份源码,三种 C 库
provides = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
requires = ["mcpp:compiler=llvm"]

[target.'cfg(c-abi = "musl")'.build]
cxxflags = ["-D_GNU_SOURCE"]
include_dirs = ["config/musl"]

[target.'cfg(c-abi = "glibc")'.build]
include_dirs = ["config/glibc"]

[target.'cfg(c-abi = "picolibc")'.build]
cxxflags = ["-D_LIBCPP_HAS_NO_THREADS"]
include_dirs = ["config/picolibc"]
```

⭐ **使用者一个字都不用多写。** 他写了 `--target x86_64-linux-musl`,
或者他的图里有一个 `provides = ["mcpp:c-abi=musl"]` 的包 —— 两种情况下
`c-abi` 都已经被解析出来了,包直接读那个结果。

⚠️ **这是「不必重复自己」在包作者面的形式**:如果这里要求一个
`features = ["musl"]`,使用者就得把已经说过的话再说一遍,而且两处可能说得不一致。

**限制,以及它的形状。** `cfg(c-abi = …)` 只能用于 `[build]`,不能用于
`[dependencies]`(S2:目标侧在依赖解析之后才有,用它决定依赖会成环)。
所以「不同 C 库需要不同的**依赖**」表达不了。两个出路:

- 把那部分拆成每 C 库一个包,由使用者或上层选择;
- 依赖并集,在 `[build]` 里按 `cfg` 选择编译哪些源码 —— 多数情况够用。

⚠️ 这条限制是**平的**:不因为 `c-abi` 恰好来自三元组(解析前可知)而放宽。
一个清单不应该「有时能写、有时不能写」,那比一律禁止更难理解。

**真正的 feature 仍然是 feature。** 「要不要异常」「要不要 filesystem」
「哪种线程模型」不由目标侧决定,它们是 `features`,由使用者选择 ——
必要时按目标选择,用已有的条件依赖表:

```toml
[target.'cfg(os = "none")'.dependencies]
acme-llvm-runtime = { version = "0.1", features = ["no-exceptions"] }
```

判据:**能从目标侧推出来的,不做成 feature;推不出来的,才是 feature。**

---

### S10 — C 库是一条依赖,不是一个键;`sysroot` 退休

⚠️ 本文先前把 `[target.X].sysroot` 定为「明知的疤,保留」。**那个结论太保守了。**

#### 今天为什么是这个形状

引擎的目标表里有 `sysroot` 一列,它的注释说明了理由:

> 这条轴存在,是因为裸机是唯一一类没有它的目标 …… 在此之前每个裸机包都得自己
> 声明 `[xlings] deps = ["xim:picolibc-riscv@1.8.12"]`。**那不是这个包的依赖,
> 那是这个目标的性质**,而按包声明会把一个板级支持包绑死到一个 libc、
> 一个指令集、一个版本上。

诊断是对的,**解法把知识挪错了地方**:它从「每个包」挪到了「引擎的表」。
后果是 —— **一块新板子要想指定它的 C 库,需要发一个 mcpp 版本。**

#### 知识本来属于板级支持包

一块板子需要哪个 C 库,是**这块板子的性质**,而**板级支持包就是描述这块板子的
那个东西**。它既不是引擎的知识,也不该由每个使用者重复书写。

```toml
# acme/riscv-virt-rt —— 一块板子的支持包
[package]
namespace = "acme"
name      = "riscv-virt-rt"
version   = "0.4.0"

[dependencies]
picolibc = "1.8"            # ⭐ 这块板子要 picolibc。一行,包自己说。
```

使用者侧因此**一个字都不用写**:

```toml
[dependencies]
riscv-virt-rt = "0.4"
```

⭐ 一条依赖,而不是「一条依赖 + 一个 `[target.riscv64-none-elf].sysroot`」。
这正是「每多一个需求只多一行」。

#### 缺的那一块机制:包不能引用一份载荷的路径

⚠️ **实测:清单里不存在任何指向 xim 载荷的替换。** `${mcpp.out_dir}` 一类是
构建输出路径,而且属于 build.mcpp;没有 `${xim:…}`。
`[xlings] deps` 只声明「要装」,不给出「装在哪」。

⇒ 一个想把现成 `xim:picolibc-riscv` 载荷包装成能力供给者的包,写不出来。

**提案:一个替换,`${xim:<名>}`。**

```toml
[package]
namespace = "acme"
name      = "picolibc"
version   = "1.8.12"
provides  = ["mcpp:c-abi=picolibc"]

[xlings]
deps = ["xim:picolibc-riscv@1.8.12"]

[build]
sources      = []                                  # 不编译任何东西
include_dirs = ["${xim:picolibc-riscv}/include"]
ldflags      = ["-L${xim:picolibc-riscv}/lib", "-lc"]
```

一个机制,四处收益:

1. **任何现成载荷都能被包装成能力供给者** —— 不需要重新打包,不需要改引擎表;
2. **板级支持包能说出自己要哪个 C 库**,新板子不再需要 mcpp 发版;
3. **`sysroot` 列失去存在理由** —— 它做的事现在由一条普通依赖完成;
4. 与 §12 的结论一致:**把一层从预制挪进图,引擎要做的事就少一件**(规则三)。

⚠️ 这条机制还有一个前置缺陷要一起修:**依赖包的 `[xlings] deps` 今天一个都不装**
(只有根工程的会装)。判据必须是「把它拿走再装回来」,不能只看构建是否绿。

#### 零 libc 档不再需要拼写

今天 `sysroot = ""` 存在,是因为目标表**默认给了**一个 C 库,项目需要一种方式说「不要」。

C 库改由依赖供给之后:**不依赖 = 没有。** 零 libc 档变成**默认**,
而想要 picolibc 的人加一条依赖。

```
不写任何依赖              →  c-abi —          零 libc 档
[dependencies] picolibc   →  c-abi picolibc
```

⭐ **一个键、一种特殊拼写、一条「空字符串是有意义的」规则,同时消失。**

#### 迁移

⚠️ 目标表里现有的 `sysroot` 行不能直接删 —— 已发布的裸机工程会突然失去 C 库。
分两步:

1. 保留现有行,但命中时打一条提示:
   「这个 C 库来自目标表的默认;把它声明成依赖会更明确,并且让这块板子
   在没有 mcpp 新版本的情况下也能换 libc」;
2. 生态迁移完毕后删除该列,`[target.X].sysroot` 与 `sysroot = ""` 同时退休。

在此之前 `sysroot` 仍是逃生口 —— 但它不再是**推荐路径**,文档要明说。

---

---

## 3. 使用侧

设计目标按优先级:**零配置最短 → 每多一个需求只多一行 → 不必重复自己 →
报告即文档 → 诊断即修法。**

### 3.0 ⭐⭐ 支配性原则:五层是引擎的词汇,不是使用者的

**§1 的五层是内部模型。它出现在报告和诊断里,不出现在清单里。**

> 报告是**诊断输出**,诊断输出可以、也应该用精确的内部词汇 ——
> 那正是它有用的原因。
> 清单是**配置输入**,配置输入必须用使用者本来就有的词。

⚠️ 把层名做成清单键(`c-abi = …`、`compiler = …`)是一个具体的错误,
它有两个可指认的坏处:

1. **它要求使用者学会引擎的分层**才能写清单。而使用者要表达的东西,
   用他已有的三样就能说完。
2. **它制造重复。** `[target.x86_64-linux-musl]` 里再写 `c-abi = "musl"`,
   是同一句话说两遍 —— 三元组的第三段**就是**那句话。

### 3.1 使用者只有三样东西

| 使用者写什么 | 它决定哪些层 |
|---|---|
| **目标三元组** `--target` / `[build] target` | `kernel-abi`(os 段)、`c-abi` 的**请求**(env 段) |
| **`[toolchain]`** / `mcpp toolchain default` | `compiler` |
| **`[dependencies]`** | 其余一切:`kernel-abi` / `c-abi` / `compiler-runtime` / `c++-abi` 的实现 |

```bash
mcpp build                                # 宿主。什么都不写
mcpp toolchain default llvm               # 换编译器。一条命令,全局
mcpp build --target x86_64-linux-musl     # 交叉,并请求 musl
mcpp build --target x86_64-linux          # 交叉,C 库由供给方决定(S1 的空 env)
```

换掉任何一层 = **加一条依赖**:

```toml
[dependencies]
openkal-llvm-runtime = "0.1"     # 供给 compiler-runtime + c++-abi
openkal-linux        = "0.5"     # 供给 kernel-abi
openkal-musl         = "0.3"     # 供给 c-abi
```

⭐ **三元组的 `env` 段就是使用者表达 C 库请求的方式,不需要第二个键。**
`x86_64-linux-musl` = 「我要 musl」;`x86_64-linux` = 「谁供给谁说了算」。
⇒ §1.4 的三态是**三元组自己的**三态,不是某个新键的三态。

⭐ **`toolchain` 保持它现在的名字。** 它是使用者已经有的词。
把它改叫 `compiler` 只为了与内部层名对齐,正是本节反对的那种越界。

### 3.2 逃生口:两个键,不属于日常表面

极少数情况需要点名,它们放在 `[target.<triple>]` 里,**是逃生口而不是接口**:

```toml
[target.x86_64-linux-musl]
toolchain = "llvm@22.1.8"                  # 只给这个目标换编译器

[target.riscv64-none-elf]
sysroot   = ""                             # 零 libc 档 —— 三元组表达不了的项目决定
```

```toml
[target.x86_64-linux-musl]
sysroot   = "xim:some-musl@1.2.5"          # 点名一份预制供给者,覆盖目标表的默认
```

判据:**一个键能进日常表面,当且仅当它表达的东西三元组、工具链、依赖三样都说不出来。**

⚠️ 按这条判据,`sysroot` **今天勉强及格,而它不该及格** —— 它表达的东西
(「这个目标用哪个 C 库」)本来就该由一条依赖说出来。它之所以还在,
是因为一个包引用不到载荷的路径(S10)。**S10 落地后 `sysroot` 退休**,
逃生口只剩 `toolchain` 一个。

⭐ **绝大多数工程一行都不写。** 目标表的行给出默认,`xim:` 地址留在引擎的表里 ——
那是引擎的事,不是使用者的事。

### 3.3 报告:唯一暴露五层的地方,而且按需暴露

报告是诊断输出,所以它用精确的内部词汇。但「零配置最短」同样适用于输出:
**五行里有五行是 `(payload)` 时,那五行没有信息。**

**默认:只显示不来自编译器载荷的层。** 全部来自载荷 ⇒ 只有目标那一行。

```
   Compiling app v0.1.0 (.)
      Target x86_64-linux-gnu                                   ← 零配置工程:一行
```

```
   Compiling app v0.1.0 (.)
      Target x86_64-linux-musl → x86_64-unknown-linux-musl
             compiler-runtime  compiler-rt  (mcpplibs/llvm-musl-runtime@0.1.0, graph)
             c-abi         musl           (xim:some-musl@1.2.5, prebuilt)
             c++-abi       libc++         (mcpplibs/llvm-musl-runtime@0.1.0, graph)
                                          ← compiler 与 kernel-abi 来自载荷,不占行
```

`-v` / `MCPP_VERBOSE=1` 显示全部五层,含 `(payload)` 的那些。

四条设计约束:

- **打的是解析结果,不是清单里的意图。** 清单会过期,结果不会。
  这也是本设计不为同一信息新增任何 manifest 字段的原因。
- **接口与实现是两列。** `openkal` 是接口,`openkal-windows` 是实现;
  合并它们会掩盖「一份源码到达四个平台,因为四个包应答同一个名字」。
- **来源词与层对齐。** `payload` / `prebuilt` / `graph` / `—`。
- **只显示与默认不同的那些**(上文);⚠️ 但**诊断不受此限** ——
  一条错误必须打印它所依据的全部层,包括来自载荷的。

### 3.4 诊断:说决定,不说后果

⚠️ **一条编译器或链接器的原话,几乎总是一次诊断缺失。**
凡是引擎已经知道结果不成立的组合,必须在编译开始之前说出来。

```
error: this C++ runtime requires an llvm-family compiler.
         c++-abi   libc++      (mcpplibs/llvm-musl-runtime@0.1.0, graph)
         compiler  gcc@16.1.0  (target default for x86_64-linux-musl)
       Yours outranks mcpp's default:
           mcpp toolchain default llvm@22.1.8
       or, for this target only:
           [target.x86_64-linux-musl]
           toolchain = "llvm@22.1.8"
```

```
error: two packages supply the C ABI, and it is a choice rather than a contribution.
         mcpplibs/openkal-musl@0.3.3   (via mcpplibs/openkal-llvm-runtime)
         acme/tinylibc@0.2.0           (a direct dependency)
       A build has exactly one C library.
```

```
error: this build requests the `gnu` C ABI, and its graph supplies `musl`.
       Write `--target x86_64-linux` to let the graph decide.
```

```
error: nothing supplies this target's C library.
         c-abi   —
       The compiler payload carries none for x86_64-linux-musl, and no
       dependency provides `mcpp:c-abi`.
       Name one for this target, or depend on a package that implements it:
           [target.x86_64-linux-musl]
           sysroot = "xim:some-musl@1.2.5"
```

```
error: the compiler runtime is not the compiler's own.
         compiler      llvm@22.1.8   (payload)
         compiler-runtime  libgcc      (xim:musl-gcc@16.1.0, prebuilt)
       Every translation unit in a build must agree on what a `throw` and an
       integer division compile into. Supply llvm's own (compiler-rt +
       libunwind) from the graph, or build with gcc.
```

⚠️ **每一条可粘贴的行都是承诺,包括其中的版本号。** 示例里的版本必须与索引
当天的 latest 一致,并在发版时同批更新。

### 3.5 不必重复自己

一条明确的产品承诺:

> **mcpp 修订它自己的默认,永不修订你的。**

推论:一个用户用明确命令设下的全局默认,**不得**被目标表的行推翻。
目标表的行是 mcpp 自己的默认,它的位次低于任何用户陈述。

若一次解析确实覆盖了什么,状态行必须说出来:

```
Resolved gcc@16.1.0  (target default for x86_64-windows-gnu, overriding your default llvm@22.1.8)
```

### 3.6 `mcpp toolchain list` 显示什么

⚠️ 一张按载荷构成的表,结构上无法列出图能到达的目标。

- Targets 表加 **SOURCE** 列:`payload` / `graph`;
- 在**工程上下文**里(cwd 有清单且有依赖)按图重算该表;
  无工程时退回载荷视图并注明;
- Available 段按**载荷包**去重,不按族 —— 同一份载荷不得因为有两个名字
  而被报成「未安装」。

---

## 4. 兼容性

⚠️ 三条硬约束,违反其中任何一条都会让已发布的包或已有的工程失效。

1. **老引擎遇到未知 `mcpp:` 层名或未知清单键,必须降级而不是让整份清单
   加载失败。** 这条已经付过学费:一个不认识的键曾让整份 manifest 无法加载,
   于是「给已发布的包加一个新键」成为不可能。
2. **旧拼写保留为别名。** `[target.X].sysroot` → `c-abi`;
   `[target.X].toolchain` → `compiler`;`hosted-standard-library` →
   `mcpp:c++-abi`。语义不变,只是不再是首选写法。
3. **规则一(唯一供给者)上线前必须先扫索引。** 若已存在两个包在同一层
   声明能力,打开检查会让现存工程直接构建失败。

---

## 5. 判据:怎么知道这个设计对了

不是「测试通过」,是**这五条同时成立**:

1. **一份新平台的实现,发一个包就能被使用**,引擎零改动、索引零改动。
2. **报告里的每一行都能被一个包或一份载荷的声明解释**,没有一行是猜出来的。
3. **每一个「编译器/链接器原话」的失败,都有一条更早的 mcpp 诊断。**
4. **零配置工程的清单里没有任何一行提到目标侧。**
5. **同一个事实在代码里只有一处推导。** 新增一个消费者时,它去问那一处,
   而不是新增第 N 条并行判据。

⚠️ 第 5 条是最容易退化的一条,也是最贵的一条:目标侧的判据曾在三处
各推一遍并互相矛盾,而收敛它们花掉的代价远大于当初写对的代价。

---

## 6. 明确不做

| 不做 | 理由 |
|---|---|
| 开放 `os` / `env` token 表 | 把语义责任推给包作者;引擎有十余处行为要问这个字段(S7) |
| 开放编译器族 | 同上;族的差异是引擎必须知道的,不是数据能描述的 |
| 命名空间准入门槛 | 把瓶颈换个地方;违反规则四;挡不住真风险(S6) |
| 目标侧包预构建分发 | 资产数回到 N×M 那一侧(规则四);冷构建成本已实测可接受 |
| 跨族借用编译器运行时 | 让同一次构建里的链接互相不一致;规则二的直接违反 |
| 为报告里的信息新增清单字段 | 清单陈述意图会过期,报告陈述结果不会(§3.2) |
| 发明新的三元组语法 | 三元组要与 LLVM/GNU 写法互认;要改的是「空」的含义,不是语法 |

---

## 7. 与当前实现的差距

本文是设计;差距、成因与实测证据在
`2026-08-24-graph-target-side-optimization-plan.md`。摘要:

| 设计条款 | 现状 |
|---|---|
| §1.1 五层 | 三层。`compiler` 与 `compiler-runtime` 未建模 |
| §1.2 四来源 | ✅ 已有(`targetside::Origin`) |
| §1.3 规则一 | ❌ 冲突时图遍历顺序第一个静默胜出 |
| §1.3 规则二 | ⚠️ 半条(只守「载荷 C++ 运行时 × 非载荷 C 库」) |
| §1.3 规则三 | ⚠️ 图×图、载荷×载荷成立;跨来源未接线 |
| §1.3 规则四 | ✅ 层名侧成立;⚠️ 工具链族里混入了一个目标侧策略 |
| §1.4 三元组是请求 | ❌ 解析时把「未指定」折叠成了「gnu」 |
| S2 `cfg` 按事实 | ❌ 只能问三元组的字段 |
| S4 声明而非猜 | ⚠️ 图侧声明,载荷侧从包名切 |
| S5 `requires` | ❌ 不存在 |
| S6 全限定名 | ❌ 报告只打 `name` |
| §3.4 不重复自己 | ❌ 全局默认会被目标表的行推翻 |
| §3.5 list 的 SOURCE 列 | ❌ 纯载荷视图 |

---
## 8. 使用示例

⚠️ 每个示例给出**该受众写的全部内容**。没写出来的,就是没写。
示例按 S8 的三种受众分组。

---

### 甲 · 使用者(什么都不需要理解)

#### 8.1 零配置

```toml
[package]
name    = "hello"
version = "0.1.0"
```

```
$ mcpp build
      Target x86_64-linux-gnu
   Compiling hello v0.1.0 (.)
    Finished dev [unoptimized + debuginfo] in 0.4s
```

⭐ **一行目标,零层。** 五层全部来自编译器载荷,一层都不打印。

#### 8.2 交叉到静态 musl —— 清单一个字不加

```
$ mcpp build --target x86_64-linux-musl
      Target x86_64-linux-musl → x86_64-unknown-linux-musl
```

仍然一行:musl 的 gcc 载荷自洽。使用者只多打了一个 `--target`。

#### 8.3 换 C++ 运行时(要 llvm 而不是 gcc)

```toml
[package]
name    = "hello"
version = "0.1.0"

[dependencies]
llvm-runtime = "0.1"
```

```
$ mcpp toolchain default llvm
     Default set to llvm@22.1.8 (was: gcc@16.1.0)

$ mcpp build --target x86_64-linux-musl
      Target x86_64-linux-musl → x86_64-unknown-linux-musl
             compiler-runtime  compiler-rt  (llvm-runtime@0.1.0, graph)
             c-abi             musl         (xim:musl@1.2.5, prebuilt)
             c++-abi           libc++       (llvm-runtime@0.1.0, graph)
```

使用者写的**全部**内容:一条依赖 + 一条命令。

- 没有 `[target.…]` 段,没有任何层名,没有任何 `xim:` 地址;
- 包名用短名,与 `[dependencies]` 里的写法一致(S6);
- `c-abi` 那行的地址来自目标表,是引擎的默认;
- `compiler` 与 `kernel-abi` 来自载荷,不占行。

⚠️ 全局默认设过一次就够,目标表的行**不得**推翻它(§3.5)。

#### 8.4 openkal 全栈,交叉到 Windows

```toml
[dependencies]
openkal-llvm-runtime = "0.1"
```

```
$ mcpp build --target x86_64-windows
      Target x86_64-windows → x86_64-w64-windows-gnu
             kernel-abi        openkal      (openkal-windows@0.1.3, graph)
             compiler-runtime  compiler-rt  (openkal-llvm-runtime@0.1.1, graph)
             c-abi             musl         (openkal-musl@0.3.3, graph)
             c++-abi           libc++       (openkal-llvm-runtime@0.1.1, graph)
```

⭐ **`--target x86_64-windows` 没有 env 段,而这是正确写法** —— C 库由图决定,
使用者不该在三元组里对它作断言。写 `x86_64-windows-gnu` 会被拒绝(§8.13b)。

⭐ 一条依赖换掉了四层里的三层,而使用者不需要知道这件事。

#### 8.5 裸机:要 C 库和不要 C 库,都是「有没有那条依赖」

```toml
[dependencies]
riscv-virt-rt = "0.4"          # 板级支持包,它自己依赖 picolibc(S10)
```

```
$ mcpp build --target riscv64-none-elf
      Target riscv64-none-elf
             kernel-abi        —
             c-abi             picolibc     (picolibc@1.8.12, graph)
```

⚠️ `kernel-abi —` 是**陈述而不是缺口**:裸机没有内核。同一个目标在 openkal
的实现进入图时这一行会变成 `openkal` —— 这正是一份源码能同时到达裸机与宿主的原因。

不要任何 C 库 —— **不写就是不要**,没有特殊拼写:

```toml
[dependencies]
openarch = "0.7"               # 只要机器机制,不引任何 C 库
```

```
      Target riscv64-none-elf
             kernel-abi        —
             c-abi             —
```

⭐ 使用者不需要知道「零 libc 档」这个概念存在(S10)。

---

### 乙 · 普通库作者(用他一直在用的轴)

#### 8.6 一个纯算法库:零个 `cfg`

```toml
[package]
namespace = "acme"
name      = "json"
version   = "1.0.0"
```

#### 8.7 一个有平台后端的库:传统的 os 轴,不需要学任何新东西

```toml
[package]
namespace = "acme"
name      = "netkit"
version   = "1.0.0"

[target.'cfg(os = "linux")'.build]
sources = ["src/backend_epoll.cpp"]

[target.windows.build]
sources = ["src/backend_iocp.cpp"]
```

⭐ **这里没有一个层名。** 一个普通库作者永远不需要知道 mcpp 有五层。

⚠️ 只有当他**刻意要为某个内核接口写后端**时,才会进入丙类的词汇 ——
那一刻他就不是在写普通库,而是在参与这套生态(S8 规则三)。

---

### 丙 · 运行时 / C 库 / 平台包作者(需要理解五层)

#### 8.8 平台实现者:供给 `kernel-abi`

```toml
[package]
namespace   = "acme"
name        = "openkal-freertos"
version     = "0.1.0"
description = "An implementation of openkal on FreeRTOS."

provides = ["mcpp:kernel-abi=openkal"]

[dependencies]
openkal = "0.6"                        # 规范包,声明接口

[build]
sources = ["src/**/*.cpp"]
```

**引擎零改动,索引零新增,不需要任何人 review mcpp 的代码。**
使用者加一条依赖就到达了一个 mcpp 从未听说过的平台。

#### 8.9 运行时实现者:供给两层,并支持三种 C 库

```toml
[package]
namespace = "acme"
name      = "llvm-runtime"
version   = "0.1.0"

provides  = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
requires  = ["mcpp:compiler=llvm"]

[build]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc", "-nostdinc++"]
sources = [
  "llvm/compiler-rt/lib/builtins/**/*.c",
  "llvm/libunwind/src/**/*.cpp",
  "llvm/libcxxabi/src/**/*.cpp",
  "llvm/libcxx/src/**/*.cpp",
]
include_dirs = ["llvm/libcxx/include", "llvm/libcxxabi/include", "llvm/libunwind/include"]

# ⭐ 一份源码,三种 C 库 —— 去问已解析的目标侧,不要求使用者说第二遍(S9)
[target.'cfg(c-abi = "musl")'.build]
include_dirs     = ["config/musl"]
std-module-flags = ["-D_GNU_SOURCE"]              # musl 的 locale 层需要

[target.'cfg(c-abi = "glibc")'.build]
include_dirs = ["config/glibc"]

[target.'cfg(c-abi = "picolibc")'.build]
include_dirs     = ["config/picolibc"]
cxxflags         = ["-D_LIBCPP_HAS_NO_THREADS"]
std-module-flags = ["-D_LIBCPP_HAS_NO_THREADS"]   # ⭐ 白得的条件化(S5)
```

三处设计落点:

- **两个 `provides`**,因为它确实供给两层 —— builtins 是 C 程序也要的东西;
- **`requires`** 让「用 gcc 编它」在编译开始之前被拒绝,而不是在 libc++ 的
  头文件深处失败;
- **std 模块的三个键在 `[build]` 里**,与 `sources` / `include_dirs` 并列 ——
  它们是同一类东西,而且因此可以按目标侧条件化(下面三个 cfg 段之一)。

#### 8.10 板级支持包:说出这块板子要哪个 C 库

```toml
[package]
namespace = "acme"
name      = "riscv-virt-rt"
version   = "0.4.0"

provides = ["mcpp:kernel-abi=openkal"]      # 这块板子的 openkal 后端

[dependencies]
openkal  = "0.6"
picolibc = "1.8"                            # ⭐ 板子的 C 库,一行,包自己说

[build]
sources = ["src/**/*.cpp", "src/start.S"]
```

⭐ **新板子不需要 mcpp 发版。** 板子的 C 库是板子的性质,写在描述板子的那个包里。

#### 8.11 把一份现成载荷包装成能力供给者

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

⚠️ `${xim:<名>}` 是 S10 提案的新机制,今天不存在 —— 这个示例描述的是目标状态。

#### 8.12 C 库实现者:供给 `c-abi`

```toml
[package]
namespace = "acme"
name      = "tinylibc"
version   = "0.2.0"

provides = ["mcpp:c-abi=tinylibc"]

[dependencies]
openkal = "0.6"                        # 坐在 openkal 之上,而不是直接发系统调用

[build]
sources = ["src/**/*.c"]
```

⚠️ 使用者若同时引入了另一个供给 `c-abi` 的包,构建在解析期就会失败并指出双方
(规则一 / S3)—— 而不是链接成功、偶尔崩。

---

### 8.13 四种出错,以及它们各自的下一步

**(a) 编译器与运行时不匹配**

```
$ mcpp build                                  # 默认 gcc,依赖含 llvm-runtime
error: this C++ runtime requires an llvm-family compiler.
         c++-abi   libc++      (llvm-runtime@0.1.0, graph)
         compiler  gcc@16.1.0  (mcpp's default)
       Yours outranks mcpp's default:
           mcpp toolchain default llvm@22.1.8
```

**(b) 三元组的请求与事实矛盾**

```
$ mcpp build --target x86_64-windows-gnu      # 依赖含 openkal
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
       A build has exactly one C library.
```

⚠️ 这一条**打全限定名**,因为两个短名并列时短名不再是标识符(S6 第二种情形)。

**(d) 没有人供给某一层**

```
$ mcpp build --target x86_64-linux-musl       # toolchain llvm,没有相应依赖
error: nothing supplies this target's C library.
         compiler  llvm@22.1.8   (payload)
         c-abi     —
       The llvm payload carries no C library for this target, and no dependency
       provides `mcpp:c-abi`. Depend on a package that implements it, or name a
       prebuilt one:
           [target.x86_64-linux-musl]
           sysroot = "xim:musl@1.2.5"
```

⚠️ (d) 打印了一行 `(payload)`,而 §3.3 说平时不显示载荷层。
**诊断是例外,且这是明写的规则**:一条错误必须打印它所依据的全部层,
否则读者看不到判断的依据。

---

### 8.14 一张表:每类人一共要学几个概念

| 受众 | 概念 | 形式 |
|---|---|---|
| 使用者 | 什么都不学 | `mcpp build` |
| 使用者 | 目标三元组 | `--target x86_64-linux-musl` |
| 使用者 | 工具链 | `mcpp toolchain default llvm` |
| 使用者 | 依赖 | `[dependencies]` 一行 |
| 使用者(逃生口) | 按目标换编译器 | `[target.X].toolchain` |
| 使用者(逃生口) | 点名预制件(S10 后退休) | `[target.X].sysroot` |
| 普通库作者 | 传统平台轴 | `cfg(os / arch / family / env)` |
| 运行时/ABI 作者 | 供给与需求 | `provides` / `requires` |
| 运行时/ABI 作者 | 供给的细节 | `[build]` 里的 `std-module*`(与 sources 并列) |
| 运行时/ABI 作者 | 按目标侧适配 | `cfg(kernel-abi / c-abi / c++-abi)` |

⭐ **前六行是使用者的全部,其中后两行是逃生口。五个层名一次都没出现。**
⭐ **第七行是普通库作者的全部** —— 与今天完全一样,没有新东西要学。
⭐ **只有最后三行需要理解五层**,而写这类包的人本来就在做这件事。
