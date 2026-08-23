# 目标侧解析:预构建体系与构建期体系的统一架构

2026-08-23。本文提出一项架构改动:把「目标侧从哪里来」从散落各处的**推导**,改为管线中一次
显式的**解析**,并让预构建体系与构建期体系在同一个模型下按层共存。

## 0. 本文的证据边界

本文区分三类陈述并逐条标注:

* **实测** —— 本次会话跑出过输出的;
* **读码** —— 从 `src/` 的表与分支读出的,未单独跑验证;
* **推断** —— 由前两类推出的判断,可能被后续测量推翻。

⚠️ 这个区分在本次讨论中挽回过两处错误。其一,笔者据行号断言「拒绝先于依赖解析」,而行号
不是执行顺序;改用探针实测才确认。其二,笔者曾引用 `prepare.cppm:5796` 的注释作为「mcpp
已有先例」,而该注释与其机制**同属本分支**(`5875a53`,2026-08-22),是循环论证。凡未标注
实测的结论,应当按可能有错来读。

---

## 1. 问题:一件事,三处推导,一处已测缺陷

一次构建必须回答:**目标侧(平台、C 库、C++ 运行时)从哪里来。** 今天这件事在三个地方各被
推导一次,判据互不相同(读码):

| 位置 | 判据 |
|---|---|
| `prepare.cppm:1404` `openkalTargetSide` | 工具链族名是否为 `openkal-llvm`(字符串比较) |
| `flags.cppm:555` `graphTargetSide` | `targetCxxRuntime && !crossTargetFlag.empty()` |
| `flags.cppm` → `distribution` `graphCxxRuntime` | `targetCxxRuntime` |

**实测缺陷**:纯 C 程序在 openkal 栈上交叉到 macOS 时失败:

```
ld64.lld: error: …/xim-x-llvm/22.1.8/lib/x86_64-unknown-linux-gnu/libc++.so:
                 unhandled file type
```

因果链(读码 + 实测):纯 C 的依赖图里没有 `hosted-standard-library` 能力 ⇒
`targetCxxRuntime` 为假 ⇒ 分发契约的自足短路不触发 ⇒ 契约落到 `host-coupled` 并加
`-lc++` ⇒ 解析到载荷中**宿主**的 `libc++.so`。

⭐ 门(判据一)放行了这次构建,而链接行(判据二)没有相应替换。**同一件事的两处推导给出了
不同答案**,而两者都不是被声明的。

---

## 2. 根因:两个体系的事实可知时刻相反

mcpp 今天服务两种目标侧供给方式,而它们的**事实可知时刻是相反的**:

| | **预构建体系** | **构建期体系** |
|---|---|---|
| 目标侧是什么 | 一个目录(编译器载荷 / xpkg sysroot) | 一组包 |
| 谁决定目标可达 | `host_can_serve` —— 硬编码的闭表 | 图里有没有实现 —— 开集 |
| 目标集合 | `kKnownTargets`,封闭 | 加一个后端就多一批 |
| **事实何时可知** | **依赖解析之前** | **依赖解析之后** |
| 组合怎么选 | 版本号(一个载荷=一种组合) | `cfg(os)` 依赖 + features |

**实测**:目标可达性的拒绝发生在依赖解析之前。探针为一个依赖指向不存在目录、同时构建一个
本宿主无载荷的目标的工程:

```
$ mcpp build --target aarch64-macos      # 依赖指向不存在的目录
error: target 'aarch64-macos' cannot be built on this host …    ← 坏依赖从未走到

$ mcpp build                             # 同一工程,本机
   Resolving toolchain                                          ← 工具链先
error: path dependency 'definitely-not-here' … has no mcpp.toml  ← 依赖后
```

⇒ ⭐⭐ **管线只有一条,而两个体系的事实在这条管线上的可知时刻相反。** 于是第 1 节那三处
推导全部在做同一个动作:**在预构建体系的时刻,猜构建期体系的答案。** 三个猜法不一致是必然
的,因为被猜的东西在那个时刻还不存在。

⇒ 由此得到本文的核心主张:**修法不是找一个更好的猜法,而是不猜。**

---

## 3. 已测边界:编译器白送什么,什么必须由体系提供

「任意宿主交叉到任意目标」这件事有多少是编译器本来就给的,是划分体系边界的前提。**实测**,
同一个 clang 二进制(`xim-x-llvm/22.1.8`):

| `--target=` | 产物 |
|---|---|
| `x86_64-unknown-linux-gnu` | ELF 64-bit x86-64 |
| `arm64-apple-macos14.0` | Mach-O 64-bit arm64 |
| `aarch64-macos` | Mach-O 64-bit arm64 |
| `x86_64-w64-windows-gnu` | COFF amd64 |
| `riscv64-none-elf` | ELF 64-bit RISC-V |

⇒ **代码生成与目标格式与具体目标无关**:格式由三元组的 OS 决定,后端全部编在同一个二进制
里。链接器同理 —— 载荷带着 `ld.lld` / `ld64.lld` / `lld-link` 三个前端。

而两件事编译器**不给**:

**其一,平台元数据。实测** mcpp 自己的拼法与 LLVM 规范拼法产出不同:

```
aarch64-macos          → MinVersion { Version: 10.4 }              ← 无 Platform 字段
arm64-apple-macos14.0  → MinVersion { Platform: macos, Version: 14.0 }
```

格式对、架构对、平台元数据错(arm64 macOS 不存在 10.4)。⚠️ 这是「看起来对、实际错」的
形态,也是 `Triple::llvm_triple()` 必须翻译而非透传的原因。

**其二,系统侧。实测**未指定 `-fuse-ld=lld` 时链接落到宿主 binutils:

```
ld: unrecognised emulation mode: llvm
```

⇒ 边界因此可以画在确定的位置:

| | 谁提供 | 与具体目标有关 |
|---|---|---|
| 代码生成 + 目标格式 | 编译器(一个二进制全包) | 否 |
| 链接器 | 编译器载荷(lld 三前端) | 否 |
| ── 分界线 ── | | |
| 平台元数据(部署版本、ABI) | **mcpp 的目标表 + `llvm_triple()`** | 是 |
| 系统侧(头 / C 库 / 运行时) | **预构建体系 或 构建期体系** | 是 |

⚠️ 由此可知,「构建期体系」不需要证明交叉可行 —— 前两行本来就成立。它要解决的只有第四行。

---

## 4. 反例:两个体系按层混用

**实测**,生态中已存在与 openkal 无关的构建期供给:

| 包 | 从图供给什么 |
|---|---|
| `std-freestanding` | C++ 标准库的 freestanding 子集(作为模块) |
| `std-freestanding-nolibc` | 五个函数四个头的 C 表面,给没有 C 库的目标 |
| `std-freestanding-alloc-libc` | 分配器,`provides = ["freestanding-allocator"]`,转发给目标的 C 库 |
| `std-freestanding-alloc-kal` | 同一能力的另一实现,转发给 openkal |

其中 `std-freestanding` + picolibc 的组合是一次**按层混用**:

| 层 | 来源 |
|---|---|
| C 库 | 预构建 —— `xim:picolibc-riscv@1.8.12`(目标表的 sysroot 列) |
| C++ 标准库 | 构建期 —— `std-freestanding` |
| 分配器 | 构建期 —— `alloc-libc`,而它转发给上面那个预构建的 C 库 |

⇒ ⭐⭐ **一次构建可以同时属于两个体系,按层分开。**

**这条反例有两个后果。**其一,任何单一布尔值都不足以表达目标侧来源 —— 它说不出「C 库是
预构建的、C++ 标准库是图供给的」。其二,构建期体系**不应以 openkal 命名**:openkal 是它
今天最大的实例,不是唯一实例,以实例命名机制会在下一个实例出现时被迫分裂。

⚠️ 附带更正一处:`provides` 机制并非本分支首创。`freestanding-allocator` 是更早的独立
用户(feature 的 `requires` 匹配包级 `provides`)。本分支新增的只是 `hosted-standard-library`
这一个名字。

---

## 5. 模型:按层的来源解析

### 5.1 三层,而非两层

目标侧不是两层而是三层。⭐ 中间那一层在传统栈上是**隐式**的 —— C 库直接发 syscall 或调
Win32,没有名字;而 openkal 的全部意义正是**把它显式化并命名**。openkal 的自述即
「**a portable kernel ABI specification**」。

| 层 | 对应三元组的位 | 传统栈 | openkal 栈 |
|---|---|---|---|
| **kernel-abi** | **os** | 隐式(linux syscalls / win32 / darwin) | ⭐ **显式:openkal** |
| **c-abi** | **env** | glibc / musl / ucrt / libSystem | musl(经 openkal) |
| **c++** | 无对应位 | libstdc++ / libc++ / MSVC STL | libc++ 或 freestanding 子集 |

⚠️ 每层是**接口**与**实现**两件事,输出必须分开显示:`openkal@0.5.1` 是接口,
`openkal-macos@0.3.1` 是实现。传统栈上两者常常是同一个物件,这本身是信息。

### 5.2 结构

```cpp
enum class Origin { Payload, Xpkg, Graph, None };

struct Layer {
    Origin      origin;
    std::string interface;      // 接口:openkal / linux / win32 / darwin / musl / glibc / libc++
    std::string impl;           // 实现:包名@版本,或载荷内的 xpkg 引用
    bool        subset = false; // C++ 层:是否为 freestanding 子集
};

struct TargetSide {
    std::string llvmTriple;     // ⭐ 目标表的翻译结果,§3 实测其差异承重
    Layer       kernelAbi;      // ← triple 的 os 位
    Layer       cAbi;           // ← triple 的 env 位
    Layer       cxx;            // 三元组无对应位
};
```

| 场景 | `kernelAbi` | `cAbi` | `cxx` |
|---|---|---|---|
| 传统本机 / 载荷交叉 | `Payload` | `Payload` | `Payload` |
| 裸机 + picolibc | `None` | `Xpkg{picolibc}` | `None` |
| **裸机 + picolibc + std-freestanding** | `None` | **`Xpkg{picolibc}`** | **`Graph{std-freestanding}`** |
| 零 libc | `None` | `None` | `None` |
| openkal C++ | `Graph{openkal-*}` | `Graph{openkal-musl}` | `Graph{openkal-llvm-runtime}` |
| **openkal 纯 C** | `Graph{openkal-*}` | `Graph{openkal-musl}` | **`None`** |
| 直接用 openkal | `Graph{openkal-*}` | **`None`** | `None` |

⭐ 第 1 节那条实测缺陷对应第六行;第三行是第 4 节的混用反例;第七行是「只要那 48 个接口」的
程序。三者在这个模型里**天然可表达**,不需要任何例外分支。

⭐⭐ **第二行与第五行的对照是本模型最有价值的一处**:同一个 `riscv64-none-elf`,picolibc 路线
`kernelAbi = None`(裸机没有内核),openkal 路线 `kernelAbi = Graph{openkal-opensbi}`。⇒ 同一份
源码之所以能落到裸机,正是因为那里**仍然有一个被命名的内核接口**。这件事今日在任何输出里都
看不见。

### 5.3 命名

构建期体系即 `Origin::Graph`。openkal 是它的一个实例,`std-freestanding` 是另一个,Zig 的
运行时模型是同一件事的外部先例。⚠️ 层名(`kernel-abi` / `c-abi` / `c++`)与来源名
(`payload` / `xpkg` / `graph` / 无)**互相正交**,不得合并 —— 合并正是 `openkal-llvm` 那个
族名的错误。

---

## 6. 管线重排

```
今天                                重构后
1. 读清单                           1. 读清单
2. 解析工具链                       2. 解析工具链(只决定编译器)
3. ⚠️ 可达性门 ← 构建期事实不存在    3. 解析依赖图
4. 解析依赖图                       4. ⭐ 目标侧解析 → TargetSide
5. 建能力表                         5. 可达性门 ← 读 TargetSide
6. 算 flags(再猜一遍)              6. 算 flags ← 读 TargetSide
```

⇒ 门与 flags 读**同一个已解析的值**。三处推导消失 —— 不是被统一,是不再需要。

**对预构建体系工程的影响**:同样的报错,只是发生在依赖解析之后。这类工程本来就没有目标侧
依赖,解析很便宜。

**对构建期体系工程的影响**:天然正确,不需要例外。且报错质量提升 —— 从「这台机器造不出这
个目标」变为 openkal 6.1 条款的 `undefined symbol: kal_*`,后者点名缺失的接口。

---

## 7. 目标表的权威范围收缩(而非降级)

第 3 节的实测表明,目标表承载的平台元数据是图无法供给的。因此目标表继续是权威,但权威范围
需要写清:

| 目标表回答 | 状态 |
|---|---|
| 目标名如何翻译成 LLVM 三元组、部署版本是多少 | **永远权威** |
| 该目标默认用哪个预构建 sysroot | 权威(预构建体系的默认值) |
| ~~该目标可不可达~~ | **不再权威** —— 可达性是依赖图的函数 |

⚠️ 最后一行是「普通依赖会让可达目标变多」的必然结论。一个包让一个目标从不可达变为可达,这
在多数包管理器中不成立,而它正是构建期体系的定义性性质。

---

## 8. 分阶段落地

**本轮范围**:阶段 0–3 全部,加 §10.1 记录的 `linkage` 诊断缺口;模块化(§14)与之并行推进。

| 阶段 | 内容 | 验收 |
|---|---|---|
| **0** | 测本机 openkal 构建今天实际链接的是什么 | ✅ **已完成,见 §10.1** —— 推断被推翻,本机路径正确 |
| **1** | 三处推导 → 一个字段(消费端统一,生产端暂由族名填充) | 纯 C 的 e2e 先红后绿;既有 CI 全绿 |
| **2** | 门移到解析之后;`TargetSide` 改由图解析产出;删 `Family::OpenkalLlvm`、`family_serves_every_target`、`openkalTargetSide`、`graphTargetSide`;清除 §10.1 的惰性 rpath 残留 | 例外分支为零;报错变为 `kal_*` |
| **3** | `Origin` 四值 + 按层结构;`sysroot` 四种来源统一;§13 的非法组合被拒并给出理由;构建输出打印解析结果 | §4 的混用组合可表达 |
| **3b** | `[build] linkage = "dynamic"` 在目标侧来自图时发出诊断而非静默失效 | 见 §15 场景 9 |

阶段 1 的消费端改动在阶段 2、3 中全部保留,不是返工。

⚠️ **阶段 1 的单一字段与 §4 的结论并不矛盾,但其范围必须写清。** §4 证明单一布尔值表达不了
按层混用;而阶段 1 的字段**只替换第 1 节那三处推导所在的 hosted 路径**,`std-freestanding` +
picolibc 一类走的是 freestanding 分支(`isFreestandingTarget` 提前短路),阶段 1 不触及。

⇒ 阶段 1 **不增加**表达力,只消除同一问题上的三处分歧;表达力在阶段 3 才提升。把阶段 1 当作
终点会重犯本文所批评的错误 —— 用一个不足以表达事实的量去承载一个多层的事实。

**阶段 3 的可解释性产出**:

```
    Resolved   llvm@22.1.8 → aarch64-macos
    Target      aarch64-macos → arm64-apple-macos14.0
      kernel-abi  openkal@0.5.1 (openkal-macos@0.3.1, graph)
      c-abi       musl          (openkal-musl@0.3.1, graph)
      c++         libc++        (openkal-llvm-runtime@0.1.0, graph)
```

⭐ 打印的是**实际解析出来的事实**,而非清单里一句可能过期的声明。这是本设计选择「不引入新
清单字段」的理由:可见性由输出承担,而输出不会过期。

---

## 9. 被否决的方案及其理由

本设计在讨论中经历四个被否决的形态,记录理由以免重复。

| 方案 | 形态 | 否决理由 |
|---|---|---|
| **A 包声明**(原形态) | `openkal-musl` 加 `provides = ["target-system"]` | 单一名字表达不了三层;且能力名是自由字符串、零校验,在其旁再加一个是放大问题 —— ⚠️ **见下方说明,该否决理由已被 §17 消解** |
| **B 工程声明** | `[build] sysroot = "openkal-musl"` | **实测**:`openkal-musl` 不在用户的 `[dependencies]` 里(是 `openkal-llvm-runtime` 的传递依赖)⇒ 要求用户陈述别人的事实,且供给者换 C 库时用户那行即失效 |
| **C 目标定义包** | 包可以向 `kKnownTargets` 加行 | 解的是另一个问题(目标词表可扩展),与本问题正交,可后置 |
| **D 独立工具链** | `openkal-llvm` / `openkal-gcc` 各为一套 | 名字命名的是 2×N×M 空间里的一个点,只表达「运行时是 llvm」一维;C 库与平台后端两维无法表达 |

⚠️⚠️ **关于方案 A 的否决必须说清,否则本文自相矛盾:§17 采用的正是「包声明能力」这个机制。**

被否决的是它**当时的形态**,而非机制本身。当时的两条理由,如今各自有了答案:

| 当时的理由 | §17 的答案 |
|---|---|
| 单一名字 `target-system` 表达不了目标侧 | §5 证明有**三层**,§17.2 给出三个名字 |
| 能力名自由、零校验,拼错则静默回退 | §17.3 的保留前缀:`mcpp:*` 是封闭集合,拼错即报错 |

⇒ 记录此事的目的不是修饰,而是留下判断依据:**一个机制被否决时,要分清否决的是机制还是它
当时的形状。** 本文在讨论中曾把二者混为一谈,并因此多绕了两轮。

另有两个被否决的实现手段:

* **切分 llvm 载荷**(`llvm-core` / `llvm-host-runtime`)。**实测**载荷构成为 `bin/` 756M、
  `lib/clang/` 112M、`include/c++/` 16M、`lib/<host>/` 9M ⇒ 切出宿主专属部分只省 25M
  (2.8%)。而「宿主目标侧」实际不在文件里,在 `bin/clang++.cfg` 的 12 行绝对路径中,
  `--no-default-config` 一个 flag 即整份关闭。⇒ 收益是第二道防线,不是修复;单独立项。
* **给包加种类标签**(bsp / sysroot / library)。`openkal-opensbi` 同时是平台实现与板级
  布局供给者,任一标签都是错的。⭐ 而**实测** `riscv-virt-rt` 不声明任何种类,其板级功能
  经 `build.mcpp` 的 `link_script` / `runner` 正常工作 —— mcpp 从不需要知道「BSP」是什么。

---

## 10. 风险与未决

| | |
|---|---|
| ✅ **阶段 0 已完成,推断被推翻** | 见 §10.1 |
| ⚠️ `targetCxxRuntime` 的其他流向 | 该值还流入 freestanding flag 计算(`-ffreestanding`、`-fasynchronous-unwind-tables`、`model.cppm:410` 的 `-fdwarf-exceptions` / `-femulated-tls` / visibility)。这些调用点的时机需确认在解析之后 |
|  ⚠️ 能力名零校验(§17.3 给出方案) | `provides` 是自由字符串,拼错一个字母则七处行为静默回退而构建「成功」。阶段 2 应加已知名集合与未知名警告 |
| ⚠️ `hosted-standard-library` 零测试 | **实测** `tests/` 全树无该字符串,而它带动七处行为。阶段 1 补测试 |
| ⚠️ `sysroot = ""` 的文档偏差 | 注释与错误信息称其为「no C library at all」,而 `same-source` 写着 `sysroot = ""` 且经 `openkal-llvm-runtime → openkal-musl` **有** C 库。字段实际选中的是「不放预构建目录」。此偏差需一并修正 |
| 门后移的代价 | 不可达目标将先解析完依赖才报错。换来的是报错说对了对象 |

### 10.1 阶段 0 的测量结果:推断被推翻(实测)

笔者曾**推断**本机 openkal 构建处于「openkal 的 libc++ + 载荷的 glibc」混合态,理由是
`crossTargetFlag` 为空使 `graphTargetSide` 为假。该推断**错误**。

探针为一个只依赖 `openkal-llvm-runtime`、工具链为 `openkal-llvm@22.1.8` 的本机工程,源码
使用 `import std`、异常与排序。**实测**:

| 观测 | 值 |
|---|---|
| 产物 | ELF 64-bit,**statically linked** |
| `ldd` | not a dynamic executable |
| `kal_*` 定义 | **48**(openkal 接口全集) |
| 未定义的 glibc 符号 | **0** |
| C 库身份 | musl 特征符号 7 处 |
| 运行 | `sorted: 2 4 7` / `caught: 42` / `unwound: true` / `import std over openkal: ok`,退出码 0 |

⇒ **本机路径今天是正确的,没有混合态。** 阶段 1 因此不必为该路径设计迁移。

⚠️ 但测量发现一处**惰性**残留。链接线上有载荷侧的贡献:

```
unit_ldflags = -nostdlib++ -Wl,-rpath,/home/speak/.mcpp/registry/subos/default/lib …
```

而**实测**该 rpath **没有落进产物**(`readelf -d` 报「There is no dynamic section」)。原因是
openkal 构建的产物必然静态 —— 图里的 C 库与平台实现都是从源码建出的静态库。

⚠️ 追加**实测**,含一处对笔者自己的更正。初次记录称「`[build] linkage = "dynamic"` 无效且不
发声」,**该记录有误**:`linkage` 根本不是 `[build]` 的键,mcpp 对它有明确诊断 ——

```
warning: [build] has unsupported key 'linkage' (ignored). Supported keys: …
```

—— 而笔者当时只读了输出的最后三行,没有看见它。⚠️ **这与本文批评的缺陷是同一种**:一次不
完整的观察被当作了一个事实。

重测后的真实情形:该指令写在 `[target.<triple>]` 下时确实**静默无效** ——

```
[target.x86_64-linux-gnu]
linkage = "dynamic"      →  产物仍为 statically linked,零诊断
```

⇒ 本轮为此补一条诊断,措辞不点名具体的表。

⇒ 结论:载荷贡献存在于链接线而不存在于产物,今日无害;但它是「按宿主决定、而事实属于目标」
的又一处残留,应在阶段 2 随例外分支一并清除。

---

## 11. 判据

本设计的验收不是「CI 绿」,而是下列各条可被单独证否:

1. 纯 C 程序在 openkal 栈上交叉到 macOS / Linux 成功链接(**已达成,实测**:`aarch64-macos` →
   Mach-O 64-bit arm64、`x86_64-linux-gnu` → ELF)。Windows 目标另需一个 builtins 供给者,
   见 §16.2 形态 3 的实测记录 —— 带上它之后同样成功(PE32+),而那属于打包粒度,不属本设计;
2. `std-freestanding` + picolibc 的按层混用在模型中可表达且构建不变;
3. `grep -r "openkal" src/` 在阶段 2 后不再出现于任何**行为分支**中(注释除外);
4. 构建输出打印的目标侧来源与实际链接线一致(阶段 3);
5. 生态八个仓库除 `same-source` 一行外零改动;
6. §13 的非法组合(`cxx.origin == Payload` 而 `cAbi.origin != Payload`)在解析阶段被拒绝,
   且报错陈述理由(「该 C++ 运行时是为载荷的 C 库 configure 的」),而非在链接期表现为一条
   指向错误目录的 `-L`;
7. **本机 openkal 构建的 §10.1 六项观测在改动后逐项不变** —— 该路径今日正确,任何改动都不得
   使其退化。这一条是回归判据,不是改进判据;
8. 旧拼法 `[toolchain] default = "openkal-llvm@22.1.8"` 在新引擎上构建成功,且产物与新拼法
   一致(§15.4 的无感升级路径);
9. `[build] linkage = "dynamic"` 在目标侧来自图时发出诊断,且产物仍为静态(§15 场景 9)。

---

## 12. 生态影响

**实测**:全生态只有一处写了 `openkal-llvm@` 工具链 —— `openkal-llvm-runtime/examples/same-source/mcpp.toml:58`。阶段 2 后该行变为 `llvm@22.1.8`。

其余七个仓库(openkal、openkal-musl、openkal-linux、openkal-macos、openkal-windows、
openkal-opensbi、openkal-uefi)**零改动**。

**实测**的依赖分层与本模型一致:

```
C++ 程序 → openkal-llvm-runtime → openkal-musl → openkal
纯 C 程序 ─────────────────────→ openkal-musl → openkal
```

而平台后端由 `openkal-musl` 的 `[target.'cfg(os = "…")'.dependencies]` 按目标选中,今日
已有五个:linux、macos、windows、opensbi、uefi。⇒ 组合数为 2×N×M 而包数为 2+N+M,组合由
依赖图承担,不需要任何新机制。

---

## 13. 两个体系的可组合性

**结论:应当可组合,而且已经在组合 —— 这不是一项待做的设计,是一项待承认的事实。**

第 4 节的 `std-freestanding` + picolibc 即为实例。因此问题不是「要不要允许」,而是「哪些组合
成立」。按 §5 的模型枚举:

三层各有四种来源,组合空间为 4³。逐格枚举既冗长又无必要,因为⭐**约束只有一条,而且它在每
一道缝上是同一条**:

> ⭐⭐ **每一层的实现,必须是为它下面那一层 configure 过的。**

这不是实现限制,而是性质。载荷里的 libc++ 是针对**载荷的 C 库** configure 出来的,它的
`__config_site` 记录了那次 configure 的结论(线程 API、`_LIBCPP_HAS_*` 一族)。把它放到另一个
C 库之上,不是「可能不兼容」,是**它从来没有为这件事被配置过**。

⚠️ 本生态已三次踩到这条性质的同一面:

* `chrono.cpp` 的 `__has_include` 是关于机器的提问,而包缺 `-nostdinc`;
* MSVC STL 的头在裸机目标上**从未被读到**,而症状看起来像编不过;
* 第 1 节那条 `unhandled file type` —— 载荷的 libc++ 落到了图供给的 C 库之上。

⇒ **判据是「那份实现有没有为这个目标 configure 过」,不是「语言是否相同」。**

### 13.1 该原则在两道缝上的具体形态

| 缝 | 约束 | 后果 |
|---|---|---|
| **c-abi ↔ c++** | `cxx.origin == Payload` 蕴含 `cAbi.origin == Payload` | 载荷的 libc++ 只配载荷的 C 库 |
| **kernel-abi ↔ c-abi** | 图供给的 C 库要求其下有对应的 kernel-abi 实现 | `openkal-musl` 调 `kal_*`,必须有一个 openkal 实现 |

⚠️ 第二条不等于「C 库必须有 kernel-abi」:picolibc 直接面向裸机,`kernelAbi = None` 是**正确**
的组合(§5 第二行)。约束是**为下层 configure 过**,不是**下层非空**。

### 13.2 成立的组合(按现存与可预见实例)

| 形态 | `kernelAbi` | `cAbi` | `cxx` | 证据 |
|---|---|---|---|---|
| 传统本机 / 载荷交叉 | `Payload` | `Payload` | `Payload` | 实测 |
| openkal C++(五个平台) | `Graph` | `Graph` | `Graph` | 实测 |
| **openkal 纯 C** | `Graph` | `Graph` | `None` | ⚠️ **今日为红** |
| 直接用 openkal | `Graph` | `None` | `None` | 读码 |
| freestanding C++ over openkal | `Graph` | `None` | `Graph`(子集) | 读码 |
| 裸机 + picolibc | `None` | `Xpkg` | `None` | 实测 |
| **裸机 + picolibc + 子集** | `None` | `Xpkg` | `Graph`(子集) | **实测** |
| 零 libc | `None` | `None` | `None` | 实测 |

⇒ 模型应在**解析阶段**拒绝违反 §13 原则的组合并陈述理由,而不是让它在链接期变成一条指向错误
目录的 `-L`。

⭐ 而 §5.2 的解析器结构使**默认路径根本构造不出**违规组合(见 §17 伪代码的末支):`cxx` 只在
`cAbi.origin == Payload` 时才落到 `Payload`。⇒ 该诊断只在**显式覆盖**路径上需要 —— 用户手写
`cxx_runtime = "toolchain-coupled"` 而 `cAbi` 来自图。

---

## 14. 模块化:按 xlings 的方式拆分

### 14.1 现状(实测)

| 目录 | 模块数 | 行数 |
|---|---|---|
| `build/` | 28 | **23167** |
| `toolchain/` | 20 | 8245 |
| `manifest/` | 4 | 5586 |
| `pm/` | 16 | 5414 |
| `pack/` | 15 | 5127 |
| `platform/` | 15 | 3985 |
| `modgraph/` | 5 | 2026 |
| 其余八个目录 | 27 | 5925 |

单文件前三:`build/prepare.cppm` **7220 行**、`build/ninja_backend.cppm` 2553 行、
`manifest/toml.cppm` 2266 行。⇒ 复杂度集中在 `build/`,而其中一个文件占该目录的三成。

### 14.2 xlings 的做法(实测)

`openxlings/xlings` 的 `modules/` 下有 cancellation、i18n、json、platform、sha256、theme、
tinyhttps 七个目录,而 `modules/platform` 的内容是 **`mcpp.toml` + `src/`** —— 即 **xlings
把自身拆成了 mcpp 包**。

⭐ mcpp 已经在消费同一形态的产物:`mcpp.toml` 里 `mcpplibs.cmdline = "0.0.1"`。而 mcpplibs
下已提取的候选有 `cmdline`、`libxpkg`、`primitives`、`tinyhttps`、`xfilesystem`。

⇒ **该模式在两个方向上都已建立**,本节只是把它应用到 mcpp 自身尚未拆分的部分。

### 14.3 可提取与不可提取

判据是**是否依赖 mcpp 的构建状态**(plan、toolchain、依赖图)。

| 部分 | 行数 | 判断 |
|---|---|---|
| `platform/` | 3985 | ✅ 可提取 —— 纯 OS 抽象;⭐ xlings 已有同名模块,存在共用可能 |
| `modgraph/` | 2026 | ✅ 可提取 —— C++ 模块依赖图,自足且可独立测 |
| `manifest/{toml,types}` | 3475 | ✅ 可提取 —— 清单语法与类型,不需要构建状态 |
| `pack/` | 5127 | ✅ 可提取 —— 分发格式,输入是产物不是计划 |
| `bmi_cache/`、`fetcher/`、`publish/` | 1050 | ✅ 可提取 |
| **`build/`、`toolchain/`** | 31412 | ❌ **不可提取** —— 这里正是耦合本身 |

⚠️ 不可提取的部分恰是最大的部分。**拆分不解决 `build/` 的复杂度**,它只把可分离的 15000 余行
移出视野,使 `build/` 的耦合暴露得更清楚。这一点应当明说,以免把拆分当成复杂度的解法。

### 14.4 ⚠️ 自举约束

被提取的包由**上一个已发布的 mcpp** 构建。因此提取出的模块**只能使用发布版 mcpp 已有的能力**。

本会话已在同一约束上失败过一次:`build.mcpp` 中调用 `mcpp::compiler()` 使三个仓库 CI 全红
(`no member named 'compiler'`),而为它准备的兜底 `mcpp::toolchain_dir()` **同样不在**那个版本
里 —— 兜底与被兜的一起失败。⇒ 提取任何模块前,须先确认其所需的清单键与构建能力在发布版中存在。

### 14.5 ⭐ 与本文架构的交点

§5 的目标侧解析是一个**纯函数**:

```
(清单, 依赖图, 目标表) → TargetSide
```

无 I/O、无全局状态、无 ninja。⇒ 它是一条天然的模块边界,而把它作为独立模块的直接收益是
**可独立测试** —— §10 记录 `hosted-standard-library` 今日零测试覆盖却带动七处行为,原因正是
该判断埋在 7220 行的 `prepare.cppm` 里,除跑一次完整构建外无从断言。

⇒ **建议:阶段 3 将目标侧解析实现为独立模块 `src/targetside/`(暂不外提为包),其单元测试即
§11 判据 2 与 4 的载体。** 待其稳定且发布版 mcpp 具备所需能力后,再考虑外提。

### 14.6 建议顺序

模块化与本文的架构改动**正交**,不应互相阻塞:

| | 内容 | 依赖关系 |
|---|---|---|
| M1 | `src/targetside/` 作为独立模块落地 | 属阶段 3,随架构走 |
| M2 | `platform/` 外提为包,评估与 xlings 同名模块共用 | 独立;先做自举能力核对 |
| M3 | `modgraph/`、`manifest/{toml,types}` 外提 | 独立 |
| M4 | `pack/` 外提 | 独立;⚠️ 其 e2e 只在 Linux 具备 pack 能力 |
| — | `build/` 的耦合 | ⚠️ 拆分**不解决**,需单独立项 |

---

## 14A. 发布闭环:进入 mcpp-index

⚠️ **实现完成、CI 全绿、PR 合入,都不等于使用者能用上。** 一个使用者写下的是

```toml
[dependencies]
openkal-llvm-runtime = "0.1.0"
```

而这一行经索引解析。若索引里没有该版本,上述整套设计对他不存在。

### 14A.1 今日的索引差距(实测)

| 包 | 分支版本 | 索引里最新 | 状态 |
|---|---|---|---|
| `openkal` | 0.6.0 | 0.5.2 | ⚠️ 落后一版 |
| `openarch` | 0.7.0 | 0.6.0 | ⚠️ 落后一版 |
| **`openkal-llvm-runtime`** | 0.1.0 | **不在索引** | ⚠️⚠️ **整个包缺席** |
| `openkal-{linux,macos,windows,opensbi,uefi}` | 与索引一致 | — | 需随能力声明升版 |
| `openkal-musl` | 0.3.1 | 0.3.1 | 同上 |
| `std-freestanding` | 0.5.0 | 0.5.0 | 同上 |

⭐ **`openkal-llvm-runtime` 不在索引**这一条尤其要紧:它是形态 2 的唯一入口。没有它,「一份
源码四种格式」这件事只能靠 git 引用完成,而 git 引用不是受支持的分发路径。

### 14A.2 本设计使全部包都需要重新发布

§17.2 要求八个包各加一行 `provides = ["mcpp:…"]`。清单内容改变 ⇒ 版本必须递增 ⇒ 索引必须
新增该版本条目。**没有例外**:索引服务的是发布的 tarball,而未发布的 tarball 里没有那一行,
新引擎在其上解析出的目标侧仍然是 `payload`。

⇒ 因此发布闭环包含,按顺序:

1. 各包升版并合入(§8 的合入顺序);
2. 打 tag、发 release;
3. 本地 gtc 立即补 CN 镜像资源(⚠️ 否则 CN 侧解析落空);
4. 在 `mcpp-index/pkgs/<首字母>/<包名>.lua` 增加版本条目,含 `GLOBAL` / `CN` 两个 URL 与
   `sha256`;
5. **判据是索引 main 的 latest 指向它**,而不是「发布成功」。

### 14A.3 判据

⚠️ 最终验证必须**通过索引**进行,而不是通过 git 分支引用:

```toml
[dependencies]
openkal-llvm-runtime = "0.1.0"     # 索引解析,不是 { git = "…" }
```

§16 的八种形态逐一构建并运行,全部经此路径。⇒ 这才是「使用者能用上」的证明;git 引用能跑只
证明代码正确,不证明分发正确。

---

## 15. 开发者使用侧:场景与变化

本节从写清单、跑命令、读输出的角度陈述本设计的全部可见后果。**判据是:除一行之外,现有工程
不需要任何修改。**

### 15.1 变化概览

| 使用者 | 清单要改吗 | 命令要改吗 | 看得到的差别 |
|---|---|---|---|
| 传统本机 / 载荷交叉 | 否 | 否 | 多一行目标侧来源;不可达目标的报错更晚更准 |
| 裸机 + picolibc | 否 | 否 | 多一行目标侧来源 |
| openkal C++ | **一行**(`openkal-llvm@` → `llvm@`) | 否 | 同上 |
| **openkal 纯 C** | 否 | 否 | ⭐ **从构建失败变为成功** |
| 生态包作者 | 否(七个仓库零改动) | 否 | — |
| 开发板 / BSP 作者 | 否 | 否 | — |

### 15.2 场景

**场景 1 — 传统本机构建。** 无变化。

```
   Resolving toolchain
    Resolved gcc@16.1.0 → …/xim-x-gcc/16.1.0/bin/g++
+   Target      x86_64-linux-gnu
+     kernel-abi  linux    (xim-x-linux-headers@5.11.1, payload)
+     c-abi       glibc    (xim-x-glibc@2.44, payload)
+     c++         libstdc++ (xim-x-gcc@16.1.0, payload)
   Compiling my-app v0.1.0 (.)
    Finished dev [unoptimized + debuginfo] in 0.31s
```

新增的四行是本设计唯一对这类工程可见的改动。它陈述的是**解析出来的事实**,不是清单里的声明。

**场景 2 — 载荷交叉(`--target x86_64-linux-musl`)。** 无变化,同样多四行:

```
+   Target      x86_64-linux-musl
+     kernel-abi  linux    (xim-x-linux-headers@5.11.1, payload)
+     c-abi       musl     (payload)
+     c++         libstdc++ (xim-x-gcc@16.1.0, payload)
```

⭐ 与形态 2 的 openkal 输出并排,差别一眼可见:此处 `c-abi = musl (payload)` 是**载荷里预建
好的** musl;openkal 那边是 `musl (openkal-musl@0.3.1, graph)` —— **同一个接口,两种来源**。

**场景 3 — openkal 交叉 C++。清单改一行:**

```diff
 [toolchain]
-default = "openkal-llvm@22.1.8"
+default = "llvm@22.1.8"
```

⭐ 这一行是本设计对整个生态**唯一**的清单改动(§12:全生态只有 `same-source` 写了它)。改动后:

```
    Resolved   llvm@22.1.8 → aarch64-macos
+   Target      aarch64-macos → arm64-apple-macos14.0
+     kernel-abi  openkal@0.5.1 (openkal-macos@0.3.1, graph)
+     c-abi       musl          (openkal-musl@0.3.1, graph)
+     c++         libc++        (openkal-llvm-runtime@0.1.0, graph)
```

⇒ 编译器与目标侧**在输出里分开**,因为它们本来就是两件事。开发者不再需要知道 `openkal-llvm`
这个只在 mcpp 内部有意义的名字。

**场景 4 — openkal 纯 C。⭐ 从失败变为成功。** 清单不变:

```toml
[package]
name = "sensor-fw"
version = "0.1.0"

[dependencies]
openkal-musl = { git = "https://github.com/mcpplibs/openkal-musl" }
```

```diff
 $ mcpp build --target aarch64-macos
-ld64.lld: error: …/xim-x-llvm/22.1.8/lib/x86_64-unknown-linux-gnu/libc++.so:
-                 unhandled file type
+   Target      aarch64-macos → arm64-apple-macos14.0
+     kernel-abi  openkal@0.5.1 (openkal-macos@0.3.1, graph)
+     c-abi       musl          (openkal-musl@0.3.1, graph)
+     c++         —
+    Finished dev [unoptimized + debuginfo] in 4.12s
```

⚠️ 注意 `c++ = —` 是**正确**的:一个 C 程序不需要 C++ 运行时。今日之所以失败,正是因为「没有
C++ 运行时」被当成了「目标侧不来自图」。

**场景 5 — 裸机 + picolibc。** 无变化:

```toml
[target.riscv64-none-elf]
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic", "-bios", "default", "-kernel"]
```
```
+   Target      riscv64-none-elf
+     kernel-abi  —
+     c-abi       picolibc-riscv (xim:picolibc-riscv@1.8.12, prebuilt)
+     c++         —
```

**场景 6 — 裸机 + picolibc + std-freestanding(按层混用)。** 无变化,而**来源现在说得清**:

```
+   Target      riscv64-none-elf
+     kernel-abi  —
+     c-abi       picolibc-riscv (xim:picolibc-riscv@1.8.12, prebuilt)      ← 预构建
+     c++         freestanding subset (std-freestanding@0.5.0, graph) ← 构建期
```

⭐ 中间两行来源不同,是 §13 可组合性的直接体现:一次构建同时属于两个体系,而输出讲清楚了。

**场景 7 — 非法组合被拒(新诊断)。** 一个工程若把载荷的 C++ 运行时用在图供给的 C 库之上:

```
error: the toolchain payload's C++ runtime cannot be used with a C library from
       the dependency graph.
         c-abi  musl   (openkal-musl@0.3.1, graph)
         c++    libc++ (xim-x-llvm@22.1.8, payload)   ← 冲突
       The payload's libc++ was configured against the payload's C library — its
       `__config_site` records that configuration. It was never configured for
       this one.
       Supply a C++ runtime from the graph (e.g. `openkal-llvm-runtime`), or use
       the payload for both.
```

⇒ 今日这一格**没有诊断**,它表现为链接期一条指向错误目录的 `-L`(§1)。

**场景 8 — 不可达目标:报错更晚,但说对了对象。**

```diff
 $ mcpp build --target aarch64-macos          # Linux 宿主,无 openkal 依赖
-error: target 'aarch64-macos' cannot be built on this host — no toolchain
-       payload exists that runs here and produces it.
-       this host can build: x86_64-linux-gnu, …
+   Resolving dependencies
+error: target 'aarch64-macos' cannot be built on this host.
+       No toolchain payload here produces it, and nothing in the dependency
+       graph supplies its system side.
+       this host can build with the payload alone: x86_64-linux-gnu, …
+       To build it anyway, depend on a package that implements the target's
+       system (see the openkal packages), or supply `[target.aarch64-macos]
+       toolchain = "…"`.
```

⚠️ **代价**:依赖先解析完才报错。对这类工程(无目标侧依赖)解析很便宜。**收益**:旧文案断言
「这台机器造不出来」,而加一个依赖就能造出来 —— 旧文案说的是错的。

**场景 9 — `linkage = "dynamic"` 不再静默失效(新诊断)。**

```toml
[build]
linkage = "dynamic"
```
```diff
 $ mcpp build          # openkal 栈
-    Finished dev [unoptimized + debuginfo] in 0.12s      ← 产物仍是静态,零提示
+warning: `[build] linkage = "dynamic"` has no effect when the target's system
+         comes from the dependency graph: openkal-musl and the platform
+         implementation are static archives built from source, and there is no
+         shared object to link against.
+    Finished dev [unoptimized + debuginfo] in 0.12s
```

⇒ 是 warning 不是 error:静态是**正确**的结果,只是与所写的不同。**实测**该指令今日无效且无
任何输出(§10.1)。

**场景 10 — 新开发板。** 无变化,仍是一个包:

```toml
[dependencies]
my-board-bsp = { git = "…" }        # build.mcpp 供给 link_script + runner
```

⭐ mcpp 从不知道「BSP」是什么概念(§9)。本设计不引入板级字段,新板成本仍为**零 mcpp 改动**。

### 15.3 不变的东西(同等重要)

| | 状态 |
|---|---|
| `[toolchain] default` 的写法与值域 | 不变(`openkal-llvm` 之外) |
| `[target.X] sysroot` 的值域 | 不变 —— 仍是 xpkg 引用或 `""`;⚠️ 只修其**注释与错误文案**(§10) |
| `[target.X] runner` / `cxx_runtime` / `linkage` | 不变 |
| `build.mcpp` 的全部指令 | 不变 |
| `provides` 的写法 | 不变;阶段 2 增加未知名警告 |
| 依赖声明、features、`cfg(os)` 依赖 | 不变 |
| 锁文件、缓存布局 | 不变(族名不落盘,落盘的是字符串) |

⚠️ **本设计不新增任何清单字段。** 这是有意的:§8 已论证可见性由**输出**承担,因为输出打印的是
解析出来的事实,而清单里的一行会在供给者变化时过期。

### 15.4 升级路径

对已有工程:

1. 什么都不做 —— 除 `openkal-llvm@` 那一处外,全部工程行为不变;
2. 写了 `openkal-llvm@` 的工程:该拼法在阶段 2 后**继续被接受**(解析为 `llvm` + 图供给),并
   发出一条建议改写的提示,而不是报错。⇒ **无感升级**,不设截止版本。

⚠️ 该兼容路径需要一条测试:旧拼法在新引擎上构建成功且行为与新拼法一致。列为 §11 判据的补充。

---

## 16. 项目形态:开发者如何选择自己的目标侧

### 16.1 ⭐ 控制面只有三个旋钮

| 旋钮 | 控制什么 | 值域 |
|---|---|---|
| **`[dependencies]`** | **目标侧从哪来** —— 主旋钮 | 依赖哪些包 |
| `[toolchain] default` | 用哪个编译器 | `llvm@…` / `gcc@…` / `msvc@…` |
| `[target.X] sysroot` | 覆盖预构建目录 | xpkg 引用 / `""` / 缺省(用目标表) |

⭐⭐ **依赖列表就是控制面。** 这是本设计从使用者视角的中心主张:开发者不通过一个开关声明
「我要用 openkal」,而是**依赖了 openkal 的包,于是目标侧就来自它们**。没有第四个旋钮,也
不需要。

### 16.2 八种项目形态

以下清单均为可直接使用的形态;引用形式沿用生态今日的实际写法(`openkal` 已在注册表,
闭包分支上的包用 git 引用)。

---

**形态 1 —— 默认:什么都不写**

```toml
[package]
name    = "app"
version = "0.1.0"
```

三个宿主上的解析结果。⭐ **每一行的形状相同:`接口 (实现, 来源)`。**

```
── Linux ────────────────────────────────────────────────────────────
    Target      x86_64-linux-gnu
      kernel-abi  linux       (xim-x-linux-headers@5.11.1, payload)
      c-abi       glibc       (xim-x-glibc@2.44, payload)
      c++         libc++      (xim-x-llvm@22.1.8, payload)

── macOS ────────────────────────────────────────────────────────────
    Target      aarch64-macos → arm64-apple-macos14.0
      kernel-abi  darwin      (macOS SDK, payload)
      c-abi       libSystem   (macOS SDK, payload)      ⚠️ 与上一行同一个物件
      c++         libc++      (macOS SDK, payload)

── Windows ──────────────────────────────────────────────────────────
    Target      x86_64-windows-msvc
      kernel-abi  win32       (Windows SDK, payload)
      c-abi       ucrt        (Windows SDK, payload)
      c++         MSVC STL    (msvc@14.4x, payload)
```

⚠️ macOS 的 `kernel-abi` 与 `c-abi` 指向同一个物件(`libSystem` 兼两层),这本身是信息 ——
它解释了为何 macOS 上没有「换一个 C 库」这个动作,而 Linux 上有。

| | |
|---|---|
| 得到 | 完整标准库、`import std`、异常、RTTI |
| 可交叉到 | 载荷支持的目标(`mcpp toolchain list` 可查) |
| 不可交叉到 | 本宿主无载荷的目标(如 Linux 上的 macOS) |

---

**形态 2 —— openkal C++:一份源码,四种格式** ⭐

```toml
[package]
name    = "portable-app"
version = "0.1.0"

[dependencies]
openkal-llvm-runtime = { git = "https://github.com/mcpplibs/openkal-llvm-runtime" }
```

⭐ **同一份清单、同一份源码,四个 `--target` 的解析结果。三行里只有中间的实现名在变。**

```
── mcpp build ───────────────────────────────────────────────────────
    Target      x86_64-linux-gnu
      kernel-abi  openkal@0.5.1  (openkal-linux@0.5.1, graph)
      c-abi       musl           (openkal-musl@0.3.1, graph)
      c++         libc++         (openkal-llvm-runtime@0.1.0, graph)

── mcpp build --target aarch64-macos ────────────────────────────────
    Target      aarch64-macos → arm64-apple-macos14.0
      kernel-abi  openkal@0.5.1  (openkal-macos@0.3.1, graph)
      c-abi       musl           (openkal-musl@0.3.1, graph)
      c++         libc++         (openkal-llvm-runtime@0.1.0, graph)

── mcpp build --target x86_64-windows-gnu ───────────────────────────
    Target      x86_64-windows-gnu → x86_64-w64-windows-gnu
      kernel-abi  openkal@0.5.1  (openkal-windows@0.1.1, graph)
      c-abi       musl           (openkal-musl@0.3.1, graph)   ⭐ Windows 上的 musl
      c++         libc++         (openkal-llvm-runtime@0.1.0, graph)

── mcpp build --target riscv64-none-elf ─────────────────────────────
    Target      riscv64-none-elf
      kernel-abi  openkal@0.5.1  (openkal-opensbi@0.1.0, graph) ⭐ 裸机上仍有内核接口
      c-abi       musl           (openkal-musl@0.3.1, graph)
      c++         libc++         (openkal-llvm-runtime@0.1.0, graph)
```

⭐⭐ 两处标注是本设计使 openkal 的价值第一次可见的地方:

* **Windows 上的 musl** —— 传统 Windows 栈没有这个选项。musl 之所以能在那里,是因为它调
  `kal_*` 而不是 syscall,由 `openkal-windows` 在 Win32 上实现;
* **裸机上的 `kernel-abi`** —— 与形态 6(picolibc)对照,后者该行为 `—`。同一份源码之所以能
  落到裸机,正是因为那里仍有一个被命名的内核接口。

| | |
|---|---|
| 得到 | `import std`、异常、RTTI —— **实测**在裸机上亦成立(`unwound: true`) |
| 可交叉到 | ⭐ **ELF / Mach-O / PE / 裸机**,从任意宿主 |
| 代价 | 首次构建需从源码建 libc++ 一族;之后走依赖缓存 |
| 平台后端 | 由 `openkal-musl` 按 `cfg(os)` 自动选中,今日五个 |

⇒ **这是「基于 openkal 的应用」的默认形态。** 程序名此一个包,其余整条栈由图解析。

---

**形态 3 —— openkal 纯 C**

```toml
[dependencies]
openkal-musl = { git = "https://github.com/mcpplibs/openkal-musl" }
```

| | |
|---|---|
| 得到 | 完整 C 标准库,可移植到全部 openkal 平台 |
| 失去 | C++ 标准库(可写 C++ 但不能 `import std`) |

⚠️ **实测(2026-08-24):这一形态在 Windows 上缺 compiler-rt builtins。**

```
ld.lld: error: undefined symbol: __mulxc3   (musl 的 src/complex/cpowl.c)
        undefined symbol: __mulsc3 / __muldc3
```

builtins 是**编译器的支持库**,既不属于 kernel-abi、也不属于 c-abi 或 c++-abi。载荷里带着宿主
那一份(`lib/clang/22/lib/…`),所以 Linux 目标(宿主即目标)与 macOS 目标(未触及这些符号)
都链接成功,而 Windows 目标触及了它们且载荷没有那一份。

⇒ 今日的供给者是 `openkal-llvm-runtime`,它把 compiler-rt 与 libc++ 建在同一个包里。**实测**
纯 C 程序依赖它即成功:

```toml
[dependencies]
openkal-llvm-runtime = { git = "…" }   # 为 builtins 而非为 C++
```
```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu
  → PE32+ executable (console) x86-64
```

⚠️ 这是**打包粒度**的缺口,不是本设计的缺陷:一个 C 程序为了三个符号带上整份 libc++。
⇒ 后续应把 builtins 拆成独立包(`openkal-llvm-builtins`),届时形态 3 在三个平台上各只需
一个依赖。**本轮不做**,因为它是新包 + 新索引条目,与目标侧解析正交。

```
    Target      aarch64-macos → arm64-apple-macos14.0
      kernel-abi  openkal@0.5.1  (openkal-macos@0.3.1, graph)
      c-abi       musl           (openkal-musl@0.3.1, graph)
      c++         —                                            ⭐ C 程序不需要它
```

⚠️ **本形态今日构建失败**(§1),本设计的判据 1 即为它。

---

**形态 4 —— 直接用 openkal:只要那 48 个接口**

```toml
[dependencies]
openkal-linux           = { git = "…/openkal-linux", features = ["standalone"] }
std-freestanding-nolibc = "^0.2.0"     # ⭐ 见下

[target.x86_64-linux-gnu]
sysroot = ""
```

⭐ **实测(2026-08-24)必须补两处,而两处都是本形态的性质:**

其一,`std-freestanding-nolibc`。openkal-linux 自身要用 `memset`,而这一层之下没有 C 库:

```
ld.lld: error: undefined symbol: memset
```

这正是该包存在的理由 ——「a freestanding C++ library still needs 五个函数四个头」。

其二,mcpp 侧的链接行。驱动被指向一个 hosted 三元组时会自带 crt 启动件与动态加载器,而本形态
两者都不该有:

```
/usr/lib/gcc/x86_64-linux-gnu/13/crtbeginS.o (outside the sandbox)
/lib64/ld-linux-x86-64.so.2                  (outside the sandbox)
```

⇒ 本轮实现:`c-abi` 缺席时链接行加 `-nostdlib -static`。理由不是策略而是性质 —— **没有 C 库
就没有它的启动件,也没有属于这个程序的解释器。**

实测结果:

```
   Compiling openkal-linux / std-freestanding-nolibc v0.2.0
    Finished dev
产物: ELF 64-bit LSB executable, x86-64        运行: raw openkal   exit=0
```

| | |
|---|---|
| 得到 | 48 个 `kal_*`;`standalone` 表示这个实现即程序环境的全部 |
| 失去 | C 标准库 —— 没有 `printf`,有 `kal_stream_write` |
| 适用 | 内核、加载器、要求最小面积的程序 |

```
    Target      x86_64-linux-gnu
      kernel-abi  openkal@0.5.1  (openkal-linux@0.5.1, graph, standalone)
      c-abi       —
      c++         —
```

⭐ 三行里只有一行有内容 —— 这正是「只要那 48 个接口」在输出里的样子。

---

**形态 5 —— freestanding C++ 建在 openkal 之上,不要 C 库**

```toml
[dependencies]
openkal-linux    = { git = "…/openkal-linux", features = ["standalone"] }
std-freestanding = { git = "…/std-freestanding", features = ["alloc-kal", "nolibc"] }
```

| | |
|---|---|
| 得到 | C++ 标准库的 freestanding 子集(`import mcpplibs.std.freestanding`);分配器经 `alloc-kal` 转发到 openkal |
| 失去 | 完整标准库;`nolibc` 下**实测** 103 个头中 94 个可编 |
| 适用 | 要 C++ 抽象而不要 C 库的裸机 / 内核程序 |

```
    Target      x86_64-linux-gnu
      kernel-abi  openkal@0.5.1  (openkal-linux@0.5.1, graph, standalone)
      c-abi       —
      c++         freestanding subset (std-freestanding@0.5.0, graph)
                  ↳ allocator: openkal (std-freestanding-alloc-kal@0.1.0)
```

⚠️ `c++` 行显示 `subset` 而非 `libc++`,因为 `[package] std-module` 未声明 ⇒ `import std` 不
可用,可用的是 `import mcpplibs.std.freestanding`。⭐ 这个区分**不需要第二个能力名**,它由
清单里那个键本身承载(§17.2)。

⭐ `alloc-kal` 与 `alloc-libc` 是同一能力 `freestanding-allocator` 的两个实现 —— **换一个
feature 就换掉分配器的下层**,这是构建期体系内部的组合。

---

**形态 6 —— 裸机 + picolibc(传统路线)**

```toml
[target.riscv64-none-elf]
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic", "-bios", "default", "-kernel"]
```

| | |
|---|---|
| 得到 | picolibc 的 C 库 |
| 清单要写的 | **零依赖** —— C 库是目标的性质,由目标表给出 |

```
    Target      riscv64-none-elf
      kernel-abi  —                                              ⭐ 裸机没有内核
      c-abi       picolibc-riscv (xim:picolibc-riscv@1.8.12, prebuilt)
      c++         —
```

⭐⭐ **把这一段与形态 2 的 `riscv64-none-elf` 并排看**,是本文档最要紧的一处对照:同一个目标,
picolibc 路线 `kernel-abi = —`,openkal 路线 `kernel-abi = openkal (openkal-opensbi)`。⇒ 后者
之所以能跑同一份写给 Linux 的源码,原因就在这一行。

---

**形态 7 —— 裸机 + picolibc + freestanding 子集(按层混用)** ⭐

```toml
[dependencies]
std-freestanding = { git = "…/std-freestanding", features = ["alloc-libc"] }

[target.riscv64-none-elf]
runner = [...]
```

| | |
|---|---|
| 说明 | ⭐ **一次构建同时属于两个体系**;`alloc-libc` 把分配器转发给那个预构建的 C 库 |

```
    Target      riscv64-none-elf
      kernel-abi  —
      c-abi       picolibc-riscv (xim:picolibc-riscv@1.8.12, prebuilt)   ← 预构建
      c++         freestanding subset (std-freestanding@0.5.0, graph) ← 构建期
                  ↳ allocator: picolibc (std-freestanding-alloc-libc@0.1.0)
```

⭐ 中间两行来源不同,**两个体系在同一次构建里并存**,而输出把它讲清楚了。这在清单里读不出来。

⇒ 这是 §13 可组合性的现存实例,也是「单一布尔表达不了目标侧」的证据。

---

**形态 8 —— 零 libc**

```toml
[target.x86_64-none-elf]
sysroot = ""
```

| | |
|---|---|
| 得到 | 只有编译器内建与自己写的代码 |
| 说明 | `aarch64-none-elf` / `x86_64-none-elf` 两行目标表的 sysroot 列本就为空,**不写也落在此层** |

```
    Target      x86_64-none-elf
      kernel-abi  —
      c-abi       —
      c++         —
```

⚠️ `sysroot = ""` 的语义是「**不放预构建目录**」,不是「这个程序没有 C 库」—— 形态 2 在裸机
目标上也写它,而它经 openkal-musl **有** C 库。本设计一并修正该字段的注释与错误文案(§10)。

### 16.3 选择表

| 我要什么 | 形态 | 关键一行 |
|---|---|---|
| 就在本机跑,别麻烦 | 1 | (什么都不写) |
| 一份源码发到 Linux/macOS/Windows | **2** | `openkal-llvm-runtime` |
| 同上但只写 C | 3 | `openkal-musl` |
| 写内核 / 加载器,要最小面积 | 4 | `openkal-linux` + `standalone` |
| 内核里也想要 C++ 抽象 | 5 | `+ std-freestanding` + `alloc-kal` |
| RISC-V 裸机,用现成 C 库 | 6 | (目标表给 picolibc) |
| 同上但想要 C++ 子集 | 7 | `+ std-freestanding` + `alloc-libc` |
| 什么都不要 | 8 | `sysroot = ""` |

⚠️ **形态之间没有开关,只有依赖。** 从形态 3 升到形态 2 是把 `openkal-musl` 换成
`openkal-llvm-runtime`;从形态 6 升到形态 7 是加一个依赖。⇒ 不存在「切换到 openkal 模式」
这样一个动作。

### 16.4 如何确认自己选中了什么

⭐ 这正是 §15 那一行输出的用途:

```
$ mcpp build --target aarch64-macos
   Resolving toolchain
    Resolved   llvm@22.1.8 → aarch64-macos
    Target      aarch64-macos → arm64-apple-macos14.0
      kernel-abi  openkal@0.5.1  (openkal-macos@0.3.1, graph)
      c-abi       musl           (openkal-musl@0.3.1, graph)
      c++         libc++         (openkal-llvm-runtime@0.1.0, graph)
```

⇒ 不需要读清单去推断,也不需要读文档 —— **构建过程报告它实际解析出的结果**。清单里可能写着
一个已经过期的意图,而这三行不会。

⚠️ 而在本设计之前,这个问题**没有答案**:今日三处推导互不一致(§1),即使读遍清单也无法确定
链接线上会出现什么。这是本设计最直接的使用侧收益。

---

## 17. 识别机制:mcpp 如何区分这些形态

### 17.1 ⭐⭐ 硬编码层名,不硬编码实现

| mcpp 里 | 内容 | 性质 |
|---|---|---|
| **硬编码** | 三个**层名**:`kernel-abi` / `c-abi` / `c++-abi` | 封闭集合,可校验,**不含任何产品名** |
| **绝不硬编码** | `openkal` / `openkal-musl` / `openkal-linux` / `picolibc` / … | 实现,mcpp 一个都不认识 |

⇒ 这条分界是本设计能称为架构而非补丁的原因。今日 `prepare.cppm:1409` 里那句
`fam == "openkal-llvm"` 违反的正是它 —— 一个产品名被写进了引擎。

⚠️ 层名之所以可以硬编码,是因为**层是有限的且由 C/C++ 的构建模型决定**(内核接口、C 库、
C++ 运行时),不随生态增长;实现之所以不可以,是因为它们**正是要增长的东西**(§12:2×N×M)。

### 17.2 三个能力名,不是四个

| 层 | 能力名 | 今天 | 要加 |
|---|---|---|---|
| kernel-abi | `kernel-abi` | 无 | 五个平台后端 |
| c-abi | `c-abi` | 无 | `openkal-musl` |
| c++-abi | `c++-abi` | `hosted-standard-library`(改名) | `std-freestanding` |

⭐ **hosted 与 freestanding 不需要各占一个能力名。** 二者的区别已由清单里的另一个键承载 ——
**实测**全生态只有 `openkal-llvm-runtime` 声明 `[package] std-module`,而 `prepare.cppm:5641`
正是读它:

```
声明 c++-abi + 声明 std-module  → 完整标准库,`import std` 可用
声明 c++-abi + 无 std-module    → freestanding 子集(如 import mcpplibs.std.freestanding)
```

⇒ 能力名回答「**这一层有没有人供给**」,`std-module` 回答「**供给到什么程度**」。两个问题,
两个键,不需要把答案编进名字。

⚠️ `hosted-standard-library` → `c++-abi` 是改名。代价最低的时机就是现在:**实测**它只在一个
包里出现(`openkal-llvm-runtime/mcpp.toml:18`),且尚未发布。

### 17.3 ⚠️ 校验:保留前缀,而非封闭整个 `provides`

`provides` 今日**零校验**(`toml.cppm:458` 直接读进数组)。但它同时服务两类用途:

| 用途 | 例 | 谁消费 |
|---|---|---|
| 目标侧层名 | `kernel-abi` | **mcpp 引擎** |
| 包之间的能力 | `freestanding-allocator` | 特性系统(包与包相互匹配) |

⇒ 把整个 `provides` 变成封闭集合会**打断第二类**(`freestanding-allocator` 会被拒)。因此:

```toml
provides = ["mcpp:kernel-abi"]          # mcpp 拥有的名字空间:封闭集合,拼错即报错
provides = ["freestanding-allocator"]   # 包之间的能力:自由,mcpp 不过问
```

⇒ 任何 `mcpp:*` 名字若不在封闭集合内即为**错误**而非静默忽略;其余名字照旧自由。这解决了
§10 记录的「拼错一个字母则七处行为静默回退」。

⚠️ 前缀写法需要定:`mcpp:kernel-abi` 与 xpkg 引用 `xim:picolibc-riscv` 的形状一致,这是选它的
理由;若不采用前缀,则需另一种把两类名字分开的办法。

### 17.4 解析器伪代码

```cpp
TargetSide resolve(const Manifest& root, const DepGraph& g,
                   const TargetRow& row, const Triple& t)
{
    TargetSide ts;
    ts.llvmTriple = t.llvm_triple(row.macosVersion);   // ⭐ §3:翻译本身承重

    // ── kernel-abi ← triple 的 os 位 ────────────────────────────
    if (auto* p = g.provider_of("mcpp:kernel-abi"))
        ts.kernelAbi = { Graph, p->declared_interface(), p->id() };
    else if (t.is_freestanding())
        ts.kernelAbi = { None };                        // 裸机没有内核
    else
        ts.kernelAbi = { Payload, t.os, payload.system_ref() };

    // ── c-abi ← triple 的 env 位 ────────────────────────────────
    if (auto* p = g.provider_of("mcpp:c-abi"))
        ts.cAbi = { Graph, p->declared_interface(), p->id() };
    else if (root.sysrootDeclared && root.sysroot->empty())
        ts.cAbi = { None };
    else if (auto x = root.sysroot.value_or(row.sysroot); !x.empty())
        ts.cAbi = { Xpkg, xpkg_interface(x), x };
    else
        ts.cAbi = { Payload, t.env_or("glibc"), payload.libc_ref() };

    // ── c++-abi(三元组无对应位)─────────────────────────────────
    if (auto* p = g.provider_of("mcpp:c++-abi"))
        ts.cxx = { Graph, p->declared_interface(), p->id(),
                   .subset = p->manifest.stdModule.empty() };   // ⭐ §17.2
    else if (ts.cAbi.origin == Payload)                          // ⭐⭐ §13 的原则
        ts.cxx = { Payload, payload.cxx_interface(), payload.cxx_ref() };
    else
        ts.cxx = { None };

    return ts;
}
```

⭐⭐ 倒数第二支即 §13 那条原则的**结构化形态**:载荷的 C++ 运行时只在 C 库也来自载荷时才被
选中。⇒ **默认路径构造不出违规组合**,该诊断因此只在显式覆盖路径上需要。

⚠️ `declared_interface()` 是包声明的接口名(`openkal` / `musl` / `libc++`),供输出使用。它
不参与任何判断 —— **mcpp 不认识这些名字,只转述它们**。这是 §17.1 那条分界在代码里的落点。

---

## 18. 落地记录(2026-08-24)

本节记录实际落地的结果与偏离,以便本文的判据能被逐条核对。

### 18.1 引擎侧

发布版本 **mcpp 2026.8.24.1**(PR #486,10/10 workflow 绿,零失败)。

| 新增 | |
|---|---|
| `src/targetside/model.cppm` | `Origin` / `Layer` / `TargetSide`、能力语法、解析器、层间约束、报告 |
| `tests/unit/test_targetside.cpp` | 16 条,覆盖 §5.2 的全表与 §13 的约束 |
| `tests/e2e/268` | 声明→解析→报告的接线;保留前缀的封闭性与非前缀名的放行 |
| `tests/e2e/269` | 旧拼法仍解析为同一驱动,且**不再决定任何事** |

| 删除 | |
|---|---|
| `openkalTargetSide` | prepare 里按族名的字符串比较 |
| `graphTargetSide` | flags 里 `targetCxxRuntime && crossTarget` 的推导 |
| `family_serves_every_target` | 零调用者 |
| 门上的 openkal 例外、pin 处的 openkal 例外 | 后者由「目标行的 pin 不覆盖用户显式声明」这条更一般的规则取代 |

`Family::OpenkalLlvm` **保留为拼法**,行为与 `Llvm` 完全一致,不设废弃期限。

### 18.2 实测结果对照判据

| 判据 | 结果 |
|---|---|
| 1 纯 C 交叉 | ✅ macOS → Mach-O 64-bit arm64;Linux → ELF。⚠️ Windows 需一个 builtins 供给者(§16.2 形态 3),带上后 → PE32+ |
| 2 按层混用可表达 | ✅ 形态 6/7 实测输出与模型逐行吻合 |
| 3 `src/` 无 openkal 行为分支 | ✅ 仅剩名字识别(兼容路径)与一处注释举例 |
| 4 输出与链接线一致 | ✅ 形态 1–8 逐一核对 |
| 5 生态改动最小 | ⚠️ **偏离**:实际每个包各加一行 `provides` 并升版(§17.2 要求如此),而非「零改动」。本文 §15.1 的「零改动」只对**不参与供给**的工程成立 |
| 6 非法组合被拒 | ✅ 结构上无法构造,诊断保留给显式覆盖路径 |
| 7 本机 openkal 六项观测不变 | ✅ 静态 / 无动态段 / 48 个 `kal_*` / 0 个未定义 glibc / musl / 输出四行相同 |
| 8 旧拼法仍工作 | ✅ e2e 269,故障注入确认有效 |
| 9 `linkage` 诊断 | ✅ 写在 `[target.<triple>]` 下时发出,产物仍为静态 |

### 18.3 实施中发现的、本文原先没有的三件事

1. ⭐ **`c-abi` 缺席的 hosted 目标需要 `-nostdlib -static`。** 驱动被指向 hosted 三元组时自带
   crt 启动件与动态加载器,而形态 4 两者都不该有。理由是性质而非策略:没有 C 库就没有它的
   启动件,也没有属于这个程序的解释器。
2. ⚠️ **`ldRuntimeFallback` 是第二条通道。** 目标侧来自图时,链接行的整体替换到不了它,
   宿主 subos 的 `-rpath` 因此留在 `unit_ldflags` 上。今日无害(产物必然静态),已清除。
3. ⚠️ **发布的 tarball 会固化开发分支引用。** 第一轮发布的九个版本携带正确的能力声明和错误的
   引用;补发第二轮七个版本,按层把每处 `branch = …` 换成索引里存在的版本。⇒ **「合入并发布」
   不等于「可用」,而「可用」的判据是发布物里没有指向分支的引用。**

### 18.4 生态发布

| 包 | 第一轮 | 第二轮(索引解析) |
|---|---|---|
| openkal | 0.6.0 | — |
| openkal-linux | 0.5.2 | **0.5.3** |
| openkal-macos | 0.3.2 | **0.3.3** |
| openkal-windows | 0.1.2 | **0.1.3** |
| openkal-opensbi | 0.1.1 | **0.1.2** |
| openkal-uefi | 0.1.1 | **0.1.2** |
| openkal-musl | 0.3.2 | **0.3.3** |
| **openkal-llvm-runtime** | **0.1.0(首次进索引)** | **0.1.1** |
| std-freestanding | 0.5.1 | — |
| openarch | 0.7.0 | — |

每个 CN 镜像对象在写入索引条目**之前**都被取回并与 GitHub 归档逐字节比对;十六个全部一致。
