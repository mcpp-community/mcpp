# 目标体系分析:三元组承载了四件事,而它只有三段

2026-08-25。本文分析的是 **target 体系** —— 三元组的语法与语义、目标表、
目标与工具链的关系、以及使用者必须知道多少才能写出一条 `--target`。

与 `2026-08-24-target-side-design.md` 的分界:那份讲**目标侧**(五层由谁供给),
本文讲**目标本身**(它是什么、怎么写、被谁读)。两者在 `env` 段相交,
而那个交点正是本文最大的一处缺陷。

所有断言取自 2026-08-25 的 HEAD(2026.8.24.5 待发)与实测,不取自记忆。

---

## 0. 一句话

三元组同时是**身份**、**请求**、**表的键**和**目录名**,四种角色对
「缺席」的要求互相冲突;当前实现只在 Linux 上调和了这个冲突,
其余平台把冲突转嫁给了使用者。

## 0.1 分界:第三段在两种体系下做的是两件事

⭐ 本文所有关于「第三段」的讨论,真正的分界在这里,而它此前没有被写下来。

| | 传统预构建体系 | 构建期体系 |
|---|---|---|
| 目标侧来自 | 工具链**载荷** | 依赖**图**,从源码构建 |
| 第三段的作用 | **挑选载荷** —— 解析期承重 | **不选中任何东西** —— 图已经决定 |
| `x86_64-linux-musl` | 选中 musl-gcc 载荷 | 与 `x86_64-linux` 产出相同 |
| 该不该写 | 该写,它是作出选择的方式 | 不该写,它陈述一个不被查询的请求 |

于是「第三段能不能省略」不是一个可以按平台一刀切的问题:
**在构建期体系下它应当总是可省,而在预构建体系下写出它才是准确的。**
两者共用同一套语法,所以语法必须允许省略,而由**报告**说明这次解析
落在哪一种体系里 —— 那正是 `(…, graph)` 与 `(…, payload)` 两个来源标记
已经在做的事。

⚠️ 这条分界解释了 §4 那个不对称为什么代价特别大:被强制写出第三段的两个
平台(windows、none),恰恰是构建期体系最常用的两个。

---

## 1. 现状:一条 `--target` 会被读几次

```
--target x86_64-windows-gnu
        │
        ├─► parse()          语法 → Triple{arch, os, env, envExplicit}
        │                    ⚠️ 此处发生「填充」,而填充只对 linux/macos 有规则
        ├─► find_known_target()  查 kKnownTargets(16 行 × 6 列)
        ├─► freestanding::resolve()  查第二张表 kTable(6 行 × 6 列)
        ├─► effective_sysroot()      表的 sysroot 列 vs 清单的 [target.X].sysroot
        ├─► cfg() 求值               os / arch / family / env 四个键
        ├─► 输出目录                 target/<canonical>/<fingerprint>/
        └─► targetside::resolve()    env 段作为 C 库**请求**参与五层解析
```

⭐ **同一个字符串在七处被读,而七处对它的期待不同。** 目录名要求它总是完整
(否则同一工程两种拼法产出两棵树);请求要求它能省略(否则无法表达
「我不指定 C 库」);表的键要求它精确匹配。

## 2. 表:两张,列的含义不齐

### 2.1 `kKnownTargets`(`src/toolchain/triple.cppm:207`,16 行)

| 列 | 谁读 | 决定什么 |
|---|---|---|
| `canonical` | 全部 | 表的键;也是输出目录名 |
| `tier` | `lifecycle.cppm:593` | `planned` 行在 `toolchain list` 里显示为不可装 |
| `note` | 同上 | 列表里的说明文字(`PE`/`bare`) |
| `pin` | `prepare.cppm` | **该目标的 C 库由哪个载荷供给**(不是「首选编译器」) |
| `sysroot` | `effective_sysroot` | 该行默认的 C 库包;空 = 零 libc 档 |
| `defaultStatic` | `lifecycle.cppm:550` + 链接 | 该目标默认静态链接 |

### 2.2 `freestanding::kTable`(`src/freestanding/target.cppm:135`,6 行)

裸机行的第二张表:`march` / `mabi` / `mcmodel` / `libdir` / `extra` /
`lldEmulation`。键仍是同一个 canonical 三元组。

⚠️ **两张表用同一个键而没有任何东西保证它们一致。** 一个裸机行可以出现在
`kKnownTargets` 而在 `kTable` 里缺席(反之亦然),后果是构建走到很深处才失败。
今天没出事,是因为两张表都只有一个人在维护。

## 3. 使用者必须知道的东西(实测清点)

写一条 `--target` 之前,使用者要知道:

1. **哪些三元组存在** —— `mcpp toolchain list` 的 Targets 段
2. **第三段写什么** —— 而它在每个平台上是不同的轴
3. **能不能省略第三段** —— 答案按平台不同,且没有任何文档说明规则
4. **省略之后身份是什么** —— 影响输出目录与缓存键
5. **`[target.X]` 里能写什么** —— `toolchain` / `linkage` / `runner` /
   `cxxRuntime` / `sysroot` 五个键

第 2、3 两条是本文的核心缺陷,下一节。

---

## 4. 缺陷一:`env` 段的轴按平台变化,而语法不区分

| 平台 | 第三段 | 它命名什么 | 可省略? |
|---|---|---|---|
| linux | `gnu` / `musl` | **C 库** | ✅ `x86_64-linux` |
| macos | (无) | — | ✅ 本来就没有 |
| windows | `gnu` / `msvc` | **对象 ABI** | ❌ `x86_64-windows` 被拒 |
| none | `elf` | **对象格式** | ❌ `riscv64-none` 被拒 |

实测(2026.8.24.5):

```
$ mcpp build --target x86_64-linux        ✓
$ mcpp build --target x86_64-windows      error: unknown target 'x86_64-windows'
```

⭐ **2026.8.24.3 立的规矩是「三元组是请求,而请求必须能什么都不说」,
而它只在 Linux 上实现了。** 其余平台上,使用者被强制写出第三段 ——
包括在 openkal 体系下写一个 `gnu`,而那次构建里**没有任何东西是 GNU 的**:

| 层 | 实际 | GNU? |
|---|---|---|
| 编译器 | clang | 否 |
| 链接器 | lld | 否 |
| compiler-runtime | compiler-rt | 否 |
| C 库 | musl | 否 |
| C++ 运行时 | libc++ | 否 |
| 平台 | openkal | 否 |

`gnu` 是 LLVM 词表里的历史标签(源自 MinGW),如今只表示「Windows 上非
MSVC 的那套 ABI」。clang 靠这个拼写选内部 ToolChain,mcpp 改不了它 ——
但**能不要求使用者写它**。

### 4.1 已做的缓解与它的不足

2026.8.24.5 在报告里给 `gnu` 加了落脚点:

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu   (gnu selects the Itanium C++ ABI, not a C library)
```

这解决了「读者把 `gnu` 误映到 `c-abi musl`」,**没有解决「使用者被迫写它」**。
一句解释一个本不该出现的词,比让那个词消失要差。

### 4.2 ✅ 已实现(方案 A)

`parse()` 补上 windows 与 freestanding 的填充,记为填充。实测:

```
--target x86_64-windows      Target x86_64-windows → x86_64-w64-windows-gnu     无加注
--target x86_64-windows-gnu  Target x86_64-windows-gnu → …  (gnu selects …)     有加注
--target riscv64-none        Target riscv64-none → riscv64-none-elf             无加注
--target riscv64-none-elf    Target riscv64-none-elf  (elf selects …)           有加注
```

⭐ **省略即无加注,写出才解释** —— 加注于是退化为「使用者主动提问时的回答」,
而不是对一个本不必出现的词的辩解。

⭐ **判据是一个指纹,不是「能构建」。** 同一个二进制、干净的 `target/`,
两种拼法都落进 `target/x86_64-windows-gnu/a9519e720fa9cd0b/`,第二次构建
0.10s 全缓存命中。若各自产生一个指纹,短拼法就只是「少打四个字符、
多编译一遍」。e2e `284_env_segment_is_optional_everywhere.sh` 守的正是这一条,
在未修复的 2026.8.24.4 上实测为红。

## 5. 缺陷二:`cfg()` 的谓词表与目标表不是同一套词

`prepare_inputs.cppm:107` 起的求值器认这些:

```
简写   windows | linux | macos | unix
键值   os= | arch= | family= | env=
组合   all(...) | any(...) | not(...)
```

⚠️ **没有 `freestanding` / `bare` 谓词。** 一个既要支持宿主又要支持裸机的包
无法写 `cfg(bare)`,只能逐个列出三元组 —— 而那正是「按宿主分的支」这类
缺陷的温床。裸机行在表里有 `bare` 这个 `note`,`cfg()` 却读不到它。

⚠️ **`env=` 被暴露为一个 cfg 轴**,而第 4 节说明那个轴按平台变化。
`cfg(env = "gnu")` 在 Linux 上意思是 glibc,在 Windows 上意思是 Itanium ABI。
同一个谓词,两个语义。

## 6. 缺陷三:`toolchain list` 的 TOOLCHAIN 列在 openkal 下误导

```
     TARGET                  NOTE                  TOOLCHAIN         STATUS
     x86_64-windows-gnu      PE, static, cross     gcc 16.1.0        installed
```

这一列的含义是「**哪个载荷**能服务该目标」。而一个 openkal 工程的目标侧
来自依赖图,它用的是 `llvm@22.1.8`,与这一行显示的 `gcc 16.1.0` 无关。

⭐ 这不是显示错误,是**列表回答的问题已经不是使用者要问的问题**。
载荷模型下「目标 → 工具链」是函数;图模型下不是。

## 7. 缺陷四:三元组字面量散落在十个文件

```
src/toolchain/triple.cppm       16 处   ← 表本身,应当
src/freestanding/target.cppm     5 处   ← 第二张表,应当
src/toolchain/model.cppm         2 处
src/platform/elf_runtime.cppm    2 处
src/pack/pipeline.cppm           2 处
src/pack/manifest_emit.cppm      2 处
src/toolchain/registry.cppm      1 处
src/pack/pack.cppm               1 处
src/pack/abi_tag.cppm            1 处
src/config.cppm                  1 处
```

⚠️ 表外的 **12 处**是加一个新目标时容易漏掉的地方,而漏掉不会有编译错误。

---

## 8. 优化方案

按「改动面 / 收益」排序,每条都给判据。

### A. 让第三段在每个平台都可省略 ⭐⭐ ✅ 已实现,见 §4.2

`parse()` 中补上 windows 与 freestanding 的填充,并把填充记为填充
(`envExplicit = false`):

```cpp
if (t.os == "linux"   && t.env.empty()) t.env = "gnu";
if (t.os == "windows" && t.env.empty()) t.env = "gnu";   // 新增
if (t.is_freestanding() && t.env.empty()) t.env = "elf"; // 新增
```

⚠️ **填 `gnu` 而不是宿主自己的 env。** `host_triple()` 在 Windows 上答
`msvc`,按它填会让同一条命令在不同宿主上产生不同的输出目录与缓存键 ——
目标的身份不允许依赖于它在哪里被构建。`gnu` 是每台宿主都能到达的那一行。

**判据**:
- `mcpp build --target x86_64-windows` 成功,且产物与 `-gnu` 的 strip 后一致
- 报告标题显示 `x86_64-windows`(写什么显示什么),**不出现** §4.1 的加注
- `--target x86_64-windows-msvc` 仍选中 MSVC ABI

**收益**:openkal 使用者再也不必写 `gnu`。§4.1 的加注退化为只在
使用者**主动**写出第三段时出现 —— 那时它才是一句有用的解释。

### B. `cfg(bare)` 谓词 ⭐ 应做

把目标表已有的 `note == "bare"` 暴露给 cfg 求值器:

```toml
[target."cfg(bare)"]
sysroot = ""
[dependencies."cfg(not(bare))"]
some-hosted-lib = "1"
```

**判据**:一个同时支持宿主与三种裸机三元组的包,其清单中三元组字面量
数量降为零。

⚠️ 不要同时引入 `cfg(hosted)`:`not(bare)` 已经表达它,两个名字表达
同一件事会让人猜它们是否有区别。

### C. 两张表的一致性由机器保证 ⭐ 应做

加一条单元测试:`kKnownTargets` 中 `note == "bare"` 的每一行,必须在
`freestanding::kTable` 中有对应行,反之亦然。

**判据**:删掉 `kTable` 中任意一行,该测试失败。

**成本**:约 15 行。**这是本文投入产出比最高的一条。**

### D. `toolchain list` 区分「载荷能服务」与「图能供给」 ⭐⭐ 值得做

当前的 TOOLCHAIN 列在图模型下答非所问(§6)。方案不是删掉它,而是让
STATUS 列说明来源:

```
     TARGET                NOTE                TOOLCHAIN      STATUS
     x86_64-windows-gnu    PE, static, cross   gcc 16.1.0     installed (payload)
     x86_64-windows-gnu    PE, static, cross   llvm 22.1.8    via graph
```

⚠️ **这需要解析依赖图**,而 `toolchain list` 目前不解析。若代价过大,
退而求其次:在表头加一行说明「TOOLCHAIN 列是载荷路径;由依赖图供给
目标侧的工程不使用它」。**一句准确的说明胜过一列精确的谎话。**

**判据**:在 openkal 工程目录下执行 `mcpp toolchain list`,输出不再暗示
该工程会使用 `gcc 16.1.0`。

### E. 把表外的 12 处三元组字面量收敛 ⭓ 可做可不做

逐个分类(§7)。有些是合理的(`elf_runtime.cppm` 判断 ELF 解释器路径),
有些是可以查表的。

⚠️ **先分类再决定,不要一次性重构。** 今天没有任何缺陷是由这 12 处引起的;
把它当成「加新目标时的检查清单」比当成技术债更贴近事实。可以先只加一条
测试:新目标进表时,断言这 12 处不需要跟着改 —— 若需要改,那一处就是
真正的耦合点。

### F. 明确拒绝的事

- **不引入 `--target` 的别名机制**(如 `--target windows`)。三元组是身份,
  别名会让同一目标有两个目录名。
- **不让填充依赖宿主**(见 A 的警告)。
- **不把 `env` 段的轴写进语法**(如 `x86_64-windows-abi:gnu`)。那是
  LLVM 的词表,mcpp 只能接受它;要减少的是**使用者不得不写它的次数**,
  不是重新发明拼写。

---

## 9. 依赖关系与落地顺序

```
C（两表一致性测试）   ← 独立,先做,15 行
    │
A（第三段可省略）      ← 独立于 C,但两者都动 triple.cppm,顺序做避免冲突
    │
    ├─► §4.1 的加注自动退化为「只在主动写出时出现」
    │
B（cfg(bare)）        ← 依赖 A 吗?不依赖。可并行
    │
D（toolchain list）    ← 独立;若做完整版则依赖图解析,代价最大
    │
E（字面量收敛）        ← 最后,且先分类
```

## 10. 本文刻意没有断言的事

- **没有断言 A 的改动面。** `parse()` 的填充影响 `envExplicit`,而后者
  在 §1 的七个读取点里有三个会读到。⚠️ 实施前必须先跑一次全量 e2e 的
  Windows 分片,而不是只看单元测试 —— 2026.8.24.3 那次同类改动
  (Linux 的填充)是靠 e2e 抓到回归的。
- **没有断言 D 的完整版是否值得。** 它要求 `toolchain list` 解析依赖图,
  那会让一条只想看看有哪些目标的命令变慢。退化方案(表头说明)的收益
  可能已经占了八成。
- **没有断言 §7 的 12 处里有几处是真耦合。** 那是一次分类工作,
  本文只给出计数。
