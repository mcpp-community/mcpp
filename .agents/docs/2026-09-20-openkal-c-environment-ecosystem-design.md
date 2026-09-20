---
subject: design
status: active
---

# openkal 生态：能力的时刻模型，以及 C 环境方案空间的划分

- 日期：2026-09-20
- 依据：`mcpp` `origin/main` 361874df（2026.9.18.4）、`mcpplibs/openkal` 871f8f0（SPEC 0.14）、`mcpplibs/openkal-musl`、`mcpplibs/mcpp-index` f3c69ac
- 上位关系：本文不是 mcpp#674 的设计稿，是它的上位。#674 与
  `.agents/docs/2026-09-19-issue-674-cenv-posix-preinclude-design.md` 所争的那一格，
  在本文的模型里不存在
- 评审记录：`.agents/docs/2026-09-20-issue-674-design-review.md`（实测与核查）

---

## 0. 结论摘要

1. **C 环境的问题不是"缺哪个宏"，是"在哪个时刻回答"。** openkal SPEC §6.2 已经
   规定了能力信息出现的三个时刻——依赖解析、链接、运行——并规定每个是**该信息最早
   能存在的时刻**。上游库的 `#ifdef _WIN32` / `#ifdef __linux__` 是在**预处理期**问
   一个最早只存在于解析期的问题。**这一格在规范的模型里不存在，所以落在这一格的
   每一个修法都不稳定。**

2. **顶层架构是"四层 × 三时刻"的交叉表。** 层的划分（kernel-abi / c-abi / c++-abi /
   构建工具）已经落地；时刻的划分写在 SPEC 里但还没有被当作架构使用。两者交叉之后，
   今天**空着两格**：解析期的"消费者逐条列举所需接口"，与运行期的"POSIX 有而 openkal
   没有的命名空间"。#674 的全部压力来自第一格空着。

3. **方案空间里有六个方案，按时刻分类之后自动排序。** 落在三时刻表内的四个
   （逐包配置 / 形态选择 / 合成节点 / 解析期列举）各自覆盖一类问题且互不重叠；
   落在表外的两个（引擎预置头、`presents = "linux"`）必然不稳定，且第二个已被
   SPEC §3.3 以另一个名字（`hosted`）撤回过一次。

4. **"编不过"可以是正确答案。** 强依赖 Linux 内核节点或 Win32 API 的库，在不提供
   它们的图上构建失败，是 §6.1 要的形状，不是缺陷。测量管线今天只有一种失败读法，
   这直接造成了"兼容率是引擎的分数"这个错误激励。

5. **新形态零引擎改动，这是分层是否成立的判据。** `openkal-win-ucrt`
   （`presents = "windows"` / LLP64 / wchar 16）走一遍 `cenv::realise` 得到**空令牌集**，
   因此不需要引擎改动，也不受 Clang-only 限制。这把 design §3.1 形态表的第三行从
   "不使用 openkal"改成"用 openkal，换一个 C 库形态"。

6. **合成节点层不是新层。** `openkal-musl` 的 `port/src/okm_fd.c` 已经写明它在造
   "POSIX 有而 openkal 没有"的东西，并已造了两样（描述符表、单一全局根的名字解析）。
   合成节点是第三样，判据同源，**上界由 openkal 接口集自动给出**，不靠纪律。

7. **声明必须被校验，而这条在接口一级今天是空的——本文一并关闭。** `[c-abi]`
   的探针量宏，不量"这个实现真的提供了哪些接口"。关闭它不需要新机制：openkal 的
   `SURFACE.txt` 已经是按接口分组的机读文件（104 个名字、16 个组），SPEC §9.2 已经
   规定拿它与产物做静态比对，`kal_interfaces()` 已经在运行期回答同一个问题。缺的只是
   把这三者接成**四级阶梯**，并加一条规则：**`provides-interfaces` 不得手写，由产物生成**。

8. **不优雅的地方集中在两处边缘**：借来的 `__CYGWIN__`、空着的解析期格子。
   骨架本身是简洁的。

---

## 1. 问题的重述

### 1.1 现象

`mcpp-index` 的 30-member 测量在 `x86_64-windows-gnu` 上记录 15 个失败
（`.xpkgindex/openkal-compat.json`，`measured: 2026-09-17`）。逐条读它们的守卫代码：

| 类别 | 成员 | 守卫 |
| --- | --- | --- |
| `_WIN32` / `__MINGW32__` 选错分支 | asio、eigen、catch2、re2、CLI11、lua、spdlog、doctest | `#ifdef _WIN32` → `winsock2.h` / `windows.h` / `io.h`；`#if defined(__MINGW32__)` → `__mingw_aligned_malloc` |
| 借来的 `__CYGWIN__` 被当作 Win32 可用 | mimalloc、sqlite3 | `#if defined(_WIN32) \|\| defined(__CYGWIN__)` |
| 本来就需要 configure | archive(xz)、curl、c-ares、libarchive | 生成的 `config.h` |

第一类在 `presents = "posix"` 真正生效后自愈（cygwin 三元组实测不定义 `_WIN32`
与 `__MINGW32__`）。第二类由我们自己保留 `__CYGWIN__` 造成。第三类在任何环境下
都要配置。

**没有一条是"缺 POSIX 头"。** #674 与其设计稿所依据的那条因果（musl 不传递包含
`<unistd.h>`）在本机实测下不成立：glibc 的 `<stdio.h>` 同样不传递包含它。

### 1.2 三时刻表是分类器

SPEC §6.2：

> Information therefore becomes available at three times, **each being the earliest
> at which it exists**.
>
> | Time | Mechanism | Question answered |
> | --- | --- | --- |
> | dependency resolution | the implementation package declares what it provides | may this program be built against this implementation |
> | link | an undefined symbol | was an interface used that the implementation does not provide |
> | run | a capability word | how does this implementation behave within an interface it provides |

一个库问"这个能力在不在"，按这张表，答案最早存在于**依赖解析**。而 `#ifdef` 发生在
**预处理**——比解析更早。

> **上游库不是问错了问题，是在一个比答案存在得更早的时刻问了它。**

这一句解释了为什么压 `_WIN32`、给 `__unix__`、留 `__CYGWIN__`、给 `__linux__`、
预置 `unistd.h` 这五个修法各自都能修好一批、又各自都会被下一个环境证伪：**它们都在
填一个规范模型里不存在的格子。**

### 1.3 这条判据不依赖测量

它不需要知道上游库怎么写，也不需要 30-member 的数字。任何新提案先问一句
"它在哪个时刻回答问题"，落在三时刻之外的就不必再往下看。

---

## 2. 五条设计事实

推导的地基，全部出自 openkal SPEC 0.14。

| # | 出处 | 事实 |
| --- | --- | --- |
| **F1** | §6.1 | 不提供的接口在**链接期缺席**。"A conforming implementation shall not provide an interface whose operations report a lack of support at run time; the specification treats **run-time refusal as a defect** and not as a means of expressing partiality." |
| **F2** | §3.3 | 规范**不命名环境类别**。命名过的 `hosted` 已撤回："a name that describes a class of environment **is falsified by an environment nobody had in mind**. This one was falsified **inside its own ecosystem within a release**." 替代做法是消费者**逐条列举**，"which is where a convention among consumers belongs"。 |
| **F3** | §6.2 | 三个时刻，每个是该信息**最早能存在**的时刻（表见 §1.2）。 |
| **F4** | §6.5 | 由"产物如何生产"决定的可用性，**在依赖解析时**给或不给，用包的 feature 表达。"a path no artifact takes is **a path nothing has verified**"。 |
| **F5** | §7.1 / §5.2 | 实现不得需要兼容层，判据机械："an implementation that must maintain a translation table, a registry, or **a name resolver** ... indicates that **the specification has taken a shape borrowed from one environment**, and the shape is at fault"。§5.2 进一步划线："Mapping is not simulation ... whereas **reproducing a foreign namespace of names, descriptors or paths does not**"。 |

**F5 的适用范围要读准**：它约束 **openkal 的实现**。消费者（C 库、C++ 运行时）不受它
约束——`openkal-musl` 的 `okm_fd.c` 正是在消费者一侧复制描述符与路径命名空间，并在
文件头写明理由。这条分工是本文 §5.4 方案成立的前提。

---

## 3. 顶层架构：四层 × 三时刻

### 3.1 层（已落地）

| 层 | 供给者 | 保证 |
| --- | --- | --- |
| `kernel-abi = openkal` | 规范 + 每目标一个实现 | 每个 `kal_*` 操作在每个平台行为相同 |
| `c-abi` | `openkal-musl` / 将来的 `openkal-win-ucrt` / `openkal-picolibc` | 一种 C 环境形态；做不到的明确拒绝并列出 |
| `c++-abi` | `openkal-llvm-runtime` | 为那个 C 库配置过的 C++ 运行时 |
| 构建工具 | `mcpp` | 图供给的层由图整个供给；不搜索宿主头 |

**形态由程序选择，方式是依赖哪个入口包；库不得选择形态**（openkal design §3.1）。

### 3.2 时刻（写在 SPEC 里，尚未被当作架构使用）

见 §1.2 的表。

### 3.3 交叉表：每一格由谁负责

| | 依赖解析 | 链接 | 运行 |
| --- | --- | --- | --- |
| **kernel-abi** | 实现包 `provides` 哪些接口；**消费者 `requires` 哪些接口（空）** | 未定义符号 | `kal_<iface>_props` 能力字 |
| **c-abi** | 选哪个形态（`openkal-musl` / `win-ucrt` / `picolibc`）；`[c-abi]` 声明被探针校验 | 符号缺席 | **POSIX 命名空间的合成（空）** |
| **c++-abi** | 与 C 库配套 | 符号缺席 | — |
| **构建工具** | 层解析、平台 SDK 依赖、`refuse` 开关 | — | — |

### 3.4 今天空着的两格

- **解析期 × kernel-abi**：SPEC §3.3 明说消费者应逐条列举所需接口，"in its own
  package"，但 mcpp 的清单里没有承载它的字段。于是"这个能力在不在"无处可问，
  全部被挤到预处理期。**#674 的压力来自这一格。**
- **运行期 × c-abi**：POSIX 有而 openkal 没有的命名空间（`/dev/null`、`/dev/urandom`、
  `/proc/self/*`、`/tmp`）。写得完全正确的 POSIX 代码在 openkal 上会坏，且没有任何
  一层负责。

其余各格都已有机制。**本文提出的两个新东西，恰好各补一格。**

而"声明被校验"这条原则在**接口一级**横跨全部三个时刻，本文在 §5.7 单独处理：
它不是第三个新东西，是把已有的三件材料（`SURFACE.txt`、SPEC §9.2 的静态比对、
`kal_interfaces()`）接成一条阶梯。

---

## 4. 方案空间

### 4.1 六个方案与它们的时刻

| | 方案 | 层 | 时刻 | 在三时刻表内 |
| --- | --- | --- | --- | --- |
| A | 逐包适配（`target_cfg` / `generated_files`） | 索引描述符 | 编译期，包自己的构建配置 | 合法（不是能力问答） |
| B | 引擎预置 POSIX 头（#674 Path C） | mcpp 引擎 | **预处理期** | **否** |
| C | `presents = "linux"` 形态 | C 库声明 | **预处理期** | **否** |
| D | `openkal-win-ucrt` | 新形态包 | 依赖解析 | 是 |
| E | 合成节点层（`openkal-posix`） | C 库 port 层 | 运行 | 是 |
| F | 解析期逐条列举 | 清单 + 引擎 | 依赖解析 | 是 |

### 4.2 综合对比

| | A | B | C | D | E | F |
| --- | --- | --- | --- | --- | --- | --- |
| 引擎改动 | 0 | 1 处（连带炸 4 处） | 1 处 | **0（已验证）** | 0 | 新字段 + 解析规则 |
| 覆盖面 | 一个包 | 全图 | 全图 | 整个程序 | 全图，路径类 | 全图，能力类 |
| 上界 | 无，逐个加 | **无终点** | 无，类别名会被证伪 | 明确 | **openkal 接口集** | 十六个接口名 |
| 失败模式 | 编译期，指名 | `.cppm` / `.S` 硬失败 | 静默选错分支 | 解析期拒绝 | `ENOENT`，调用方可辨 | 解析期拒绝 |
| 可撤销 | 一行 | 移动全部输出目录 | 破坏性 | 换依赖 | 改合成表 | 改清单 |
| 可验证 | 30-member | 单测空转 | 探针量宏不量能力 | realise 空令牌集 | conformance 逐节点断言 | 解析期断言 |
| 与设计事实 | 符合 | **F3 表外** | **F2 撤回过类别名** | §3.1 形态第三行 | 延续 `okm_fd.c` | §3.3 明文 |

### 4.3 B 与 C 为什么必然不稳定

- **B**：`-include` 经 `cEnvTokens` 广播进 `cflags` / `cxxflags` / `asmflags` / std 模块 /
  探针 argv。实测四处失败：模块接口单元 ill-formed（`import std` 不可用）、GAS 报
  `invalid instruction mnemonic`、`appendUniqueFlags` 按串去重把第二个 `-include` 吃掉
  使 `sys/stat.h` 变成位置参数、探针无 include path。且它修不了 §1.1 里任何一条诊断。
- **C**：`posix` / `linux` / `windows` 都是**环境类别名**。F2 记录了同类名字
  （`hosted`）如何在一个发布周期内被自己的生态证伪。`presents = "posix"` 今天已经
  被证伪一次——openkal-Windows 呈现 POSIX，却没有 `fork`、没有 `/proc`、没有 epoll。
  再加一个值只会让同一个证伪重演。

### 4.4 失败的归类

| 失败类别 | 例子 | 归属 |
| --- | --- | --- |
| `_WIN32` / `__MINGW32__` 选错分支 | asio、eigen、catch2、re2、CLI11、lua、spdlog | 已解决，等抬 pins |
| 借来的 `__CYGWIN__` 被当 Win32 用 | mimalloc、sqlite3 | P3（撤掉借用） |
| 需要 Win32 API 本身 | c-ares、curl | D 或 F |
| 需要 Linux 内核节点 | libarchive 的 `linux/fs.h` | F，**且拒绝是正确答案** |
| 写 POSIX 但用到 kal 没有的命名空间 | `/dev/null`、`/dev/urandom`、`/tmp` | **E** |
| 包自己的构建配置 | zlib 的 `Z_HAVE_UNISTD_H` | A，永远归 A |

**A / D / F 处理的都是"代码本来就不可移植"；只有 E 处理的是"代码本来就对，是我们缺"。**

---

## 5. 采纳的方案

### 5.0 P0 —— 两处实现缺陷（先修，独立）

**P0.1 freestanding 的 c-abi 探针不带 `--target`，它量的是宿主。**

`prepare.cppm:3989` 对 freestanding 目标**不设** `crossTargetFlag`（注释："for a HOSTED
target only. Freestanding already emits its own `--target`"），而 freestanding 的
`--target` 在 `mcpp::freestanding::compile_prefix()`（`linkline.cppm:48`），探针不问它；
`cenv::realise` 对 freestanding 也只产 `-D__unix__` 与 wchar 令牌。于是探针 argv 是
`clang -D__unix__ -fno-short-wchar -ffreestanding -x c++ -E -dM -`——**没有任何 `--target`**。

实测（本机 Linux）：该命令输出 `__linux__ 1`，即它量的是宿主。三条佐证：
`--target=riscv64-none-elf` 时 `__linux__` 计数为 0（预定义随目标不随宿主）、
该三元组的 `__SIZEOF_WCHAR_T__` 是 4 而不是 2、`--target=x86_64-w64-windows-gnu`
在 Linux 上定义 `_WIN32`。

2026.9.18.3 的 CHANGELOG 把症状归因为"Windows 主机的 clang 即使带上 `--target=`
仍注入 `_WIN32`"，该归因不成立；两条"不匹配"由同一个原因产生。

**修法**：探针 argv 在 freestanding 时取 `freestanding::compile_prefix()` 的
`--target` 与 ISA 标志，随后 `hostStripMacros` 整个撤除。
**判据**：Linux 宿主上 `riscv64-none-elf` 的探针 dump 必须含 `__riscv`、**不含**
`__linux__`。今天这条会红。

**P0.2 `builtins` 的 Windows 行注释与实测不符。** `cenv.cppm` 称 clang 自带
`intrin.h` / `mm_malloc.h` 的问题"already closed by the EXISTING `-nostdlibinc`
isolation"。实测不成立：`-nostdlibinc` 不关 clang 的 resource dir（那是
`-nobuiltininc`），带着它仍复现 `intrin.h:12:15` 这条与 fmtlib 记录逐字符相同的
诊断。真正关掉它们的是 `_WIN32` / `__MINGW32__` 消失。结论不变，机制写错，改注释。

### 5.1 P1 —— 冻结 `presents` 的语义与取值集

**动机**：`presents` 是这套设计里唯一一个类别名，而 F2 刚撤回过类别名。它今天
站得住，因为它陈述的是**可被探针逐条测量的身份事实**，不是需求集合的简称——但这个
差别没有写在任何文档里，于是下一个读者会提议给它加值（本轮的 C 方案正是如此）。

**形状**：在 `docs/22-target-side.md` 的四键表下加一段：

> `presents` 的取值集**冻结**为 `posix` / `windows` / `none`。它回答的是**源码看到
> 哪些环境身份宏**，不回答任何能力是否存在。一个包不得由 `presents` 推断某个接口、
> 某个头或某个路径是否可用；那些问题由 §5.5 的解析期列举回答。取值集不增长，
> 理由与 SPEC §3.2 "the core set does not grow" 相同：一个错误地扩大的名字，是由
> 后来写实现的人发现的，不是由规范发现的。

**影响**：零命令行改动，纯文档。

### 5.2 P2 —— `fails` 拆出 `refused`

**动机**：强依赖 Linux 内核节点或 Win32 API 的库，在不提供它们的图上构建失败，是
F1 要的形状。测量管线今天只有一种失败读法，于是"兼容率"被当成引擎的分数，进而
持续产生把引擎推向表外格子的压力（#674 即是）。

**形状**：`tests/openkal/compat.py` 的 `status` 增加一个值。

| status | 含义 | 计入失败率 |
| --- | --- | --- |
| `runs` | 测试通过 | — |
| `builds` | 构建成功，测试未跑或未过 | — |
| `fails` | **本该能构建而没能** | 是 |
| `refused` | **这个包要的能力这个图不提供，拒绝是正确结果** | 否 |

`refused` 的判据不能是"诊断里出现了某个字符串"，而是**包在清单里声明了图不满足的
要求**（§5.5 的机制），或描述符里显式标注。没有声明的一律仍是 `fails`。

**影响**：`openkal_kind` facet 与站点展示同步；`docs/openkal-compat.md` 增加一节
说明"一个 `refused` 是正确答案"。

### 5.3 P3 —— 撤掉 `__CYGWIN__` 借用

**动机**：保留 `__CYGWIN__` 的理由是"第三方可移植代码需要一个名字指 PE 格式 +
POSIX C 环境"。F5 的判据是机械的：借用某个环境的形状，**错的是那个形状**。实证支持
这条判定——mimalloc 的守卫写着：

```c
#if defined(_WIN32) || defined(__CYGWIN__)  // we use windows locks on cygwin, but otherwise treat it at unix
```

上游用这个名字表达的是"**Win32 可用**"，不是"对象格式是 PE"。sqlite3 同形
（`__CYGWIN__` 在 `SQLITE_OS_WIN` 的检测列表里，随后 `#include "windows.h"`，
并且 `#ifdef __CYGWIN__ → #include <sys/cygwin.h>`，正是 record §6 点名的风险）。

**一个借来的名字，它的语义由借出方的历史决定，不由我们的意图决定。**

**形状**：`cenv.cppm` 的 Windows + Posix 分支恢复 §3.3 原始文本的
`-U__CYGWIN__ -U__CYGWIN32__`，`expectUndefined` 相应增加两项。

**"对象格式是 PE"这一维怎么办**：默认**不给宏**。包在清单里问
`cfg(os = "windows")`，那本来就是它该问的地方，而且不需要任何宏。若重测显示确有
第三方代码只能在预处理期问这件事，回落方案是由 mcpp 定义一个**自己的**名字
（例如 `__mcpp_format_pe__`），而不是继续借用别人的。

**风险**：libarchive 的生成配置头已含 `#if defined(_WIN32) && !defined(__CYGWIN__)`，
撤销会改变它的分支。这条必须由重测确认，不能先落地。

### 5.4 P4 —— 合成节点层

**动机**：写得完全正确的 POSIX 代码——`open("/dev/urandom")`、`fopen("/dev/null","w")`、
`/dev/fd/N`、`/tmp`——在 openkal-Windows 上会坏，而没有任何一层负责。这是唯一一类
"代码本来就对"的失败。

**它不是新层。** `openkal-musl` 的 `port/src/okm_fd.c` 文件头已经写明：

> **Two things POSIX has and openkal does not are built here**, and each is built
> **once for every environment rather than once per environment**.
> ... A **name** in POSIX is resolved against a single global root. openkal has no
> global root ... **one rule, in one place, rather than the same rule in every program.**

已经造了两样（描述符表、单一全局根的名字解析）。合成节点是第三样，判据同源。

**判据（写死，不迭代扩张）**：

> 一个名字可以被合成，当且仅当它同时满足两条：
> **(a) POSIX 有而 openkal 没有**；**(b) openkal 能回答它**。
> 两条不同时满足的，一律回答 `kal_node_absent` / `ENOENT`。

**上界由 openkal 接口集自动给出**，不靠纪律。因此它在结构上不可能"越做越像 Linux"，
这与 design §9 拒绝的"在 musl 上仿真 `windows.h`（永远做不完）"性质不同。

| 名字 | (a) | (b) | 合成 |
| --- | --- | --- | --- |
| `/dev/null` `/dev/zero` | 是 | 是（不需要接口） | 是 |
| `/dev/urandom` `/dev/random` | 是 | 是，`openkal.random` | 是 |
| `/dev/stdin` `/dev/stdout` `/dev/stderr` `/dev/fd/N` | 是 | 是，描述符表已在 | 是 |
| `/proc/self/exe` `/cmdline` `/environ` | 是 | 是，`openkal.env` / `openkal.process` | 是 |
| `/tmp` | 是 | 是，由 supplied directories 给 | 是 |
| `/proc/cpuinfo` `/proc/meminfo` | 是 | **否** | 否 |
| `/sys/**` `/proc/net/**` | 是 | **否** | 否 |

**失败模式合法**：合成不了的答 `ENOENT`，这是 §7.7 明文的 *absence as an answer*，
**不是** F1 禁止的"运行期报告不支持"。两者的分水岭是：`epoll_create` 答 `ENOSYS`
说的是"这个接口不支持"（缺陷）；`open("/proc/net/tcp")` 答 `ENOENT` 说的是
"这个名字不存在"（文件系统的正常语义）。

**位置**：放进 `openkal-musl` 的 `port/`，不单开包。它需要描述符表（已在 port 里）
与对可选接口的弱引用（port 里已有这个模式，见 `okm_fd.c` 对
`kal_process_channel_close` 的 `__attribute__((__weak__))`）。独立成包要把描述符表
变成新的 ABI 边界，为一个今天只有一个消费者的东西付这个代价不值；等第二个形态
（picolibc）真的也需要时再抽——第二个实例才能暴露接口是否完整。

**验证**：进 openkal-musl 的 conformance。对表内每个名字，在每个目标上断言可打开
且语义正确；对表外的代表性路径（`/proc/cpuinfo`、`/sys/class`），断言得到 `ENOENT`。
**表外的断言和表内的同等重要**：它是"上界真的存在"的唯一证据。

**限制，必须写在提案里**：对 `#ifdef __linux__` 门控的代码**无效**——catch2 的
`/proc/self/status` 根本不会被调用，因为 `__linux__` 不定义。**P4 的受益者是"写
POSIX 的代码"，不是"写 Linux 的代码"。** 它不替代 P5。

### 5.5 P5 —— 解析期逐条列举

**动机**：三时刻表里最早的那一格空着。SPEC §3.3 明说消费者应逐条列举
（"a consumer that needs five names five. That is five lines"），且"in its own package,
which is where a convention among consumers belongs"，但 mcpp 没有承载它的字段。

**形状**：按层泛化，引擎不认识任何接口名。

```toml
# 实现包：openkal-windows
[package]
provides = ["mcpp:kernel-abi=openkal"]

[kernel-abi]
provides-interfaces = [
    "openkal.abort", "openkal.stream", "openkal.memory",
    "openkal.env", "openkal.time", "openkal.fs", "openkal.process",
]

# 消费包：一个网络库
[package]
requires = ["mcpp:kernel-abi=openkal"]

[kernel-abi]
requires-interfaces = ["openkal.fs", "openkal.net"]
```

**引擎的规则只有一条**：`requires-interfaces ⊆ provides-interfaces`，否则在**解析期**
拒绝，并列出缺的名字。引擎不认识 `openkal.net` 是什么，它只做集合包含——与
`cenv.cppm` 的"通用知识，不含包名"同纪律。

**平台 SDK 那一半不需要新机制**，今天就能表达：

```toml
[target.'cfg(os = "windows")'.dependencies]
win32-headers = "1.0"
```

**与 P2 的接口**：一个包因 `requires-interfaces` 不满足而被拒，测量记 `refused`，
不计入失败率。这是 `refused` 唯一可靠的判据来源。

**为什么不由引擎推断**：`provides-interfaces` 只有实现包知道（它知道自己实现了
什么），`requires-interfaces` 只有消费包知道（它知道自己调了什么）。两者都不是
引擎能推导的，也不是另一方能替对方说的。

**但两者都不得被信任**——见 §5.7。P5 单独落地会引入两个纯被信任的声明，那与
§3.2 的原则相悖；P5 与 P7 是同一次改动的两半。

### 5.6 P6 —— `openkal-win-ucrt` 形态

**动机**：design §3.1 的形态表第三行今天是"**不使用 openkal**"——需要平台 C 运行时
就整个目标离开 openkal。加入这个形态后变成"**用 openkal，换一个 C 库形态**"：
`kal_*` 仍可用，程序仍是 openkal 程序。

**形状**：

```toml
[package]
name     = "openkal-win-ucrt"
provides = ["mcpp:c-abi=ucrt"]

[c-abi]
presents   = "windows"
data-model = "arch-default"   # Windows 上即 LLP64
wchar      = 16
builtins   = "platform"
```

**引擎改动：零。** 走一遍 `cenv::realise`：

| 步骤 | 结果 |
| --- | --- |
| `presents == Windows` on windows | "Already the base triple's own identity — nothing to add" → 零令牌 |
| `arch_default_data_model(Windows, x86_64)` = `Llp64`；`triple_native_data_model("windows")` = `Llp64` | 相等，已满足 → 零令牌 |
| `wchar = 16` vs `native_wchar_bits("windows") = 16` | 相等 → 零令牌 |
| `builtins = platform` | 零令牌 |

空令牌集。又因 2026.9.18.2 的"realise 先跑、编译器族门第二"修复，**空实现连 Clang
都不强制**，GCC 与 MSVC 同样可用。

**这是本设计的分层判据**：

> **一个新形态需要改多少处引擎？** 答案必须是零。不是零，说明分层漏了一维。

**配套**：c++-abi 一侧需要一个配 UCRT 的 libc++ 或直接用 MSVC STL。那是包的事。

### 5.7 P7 —— 声明的校验阶梯（接口一级）

**动机。** P5 引入两个声明（`provides-interfaces` / `requires-interfaces`）。若二者
只被信任，本文就在关闭一个缺口的同时开了一个更大的：`[c-abi]` 的探针量的是
`__unix__` / `_WIN32` / `long` / `wchar`，**没有任何东西量"这个实现真的提供了哪些
接口"**。design §3.2 的原则是「声明被校验，而不是被信任」，它在宏一级铺到了，在
接口一级今天是空的。

**关闭它不需要新机制。** 三件材料已经在仓库里：

| 材料 | 状态 | 形状 |
| --- | --- | --- |
| `openkal/SURFACE.txt` | 已有，**normative** | 104 个 `kal_` 名字，按 16 个接口分组（`# openkal.fs` 一类的标题）；"An implementation provides an interface in whole or not at all, so **the absence of a group below denotes an interface the implementation does not provide**" |
| SPEC §9.2 Surface | 已有，已规定 | "The names an implementation exports beginning with `kal_` shall be compared against `SURFACE.txt` ... **a static examination of the artefact**" |
| `kal_interfaces()` | 已有 | `include/openkal/version.h:91`，一个位字回答"哪些接口在场"；"says which interfaces exist, **not how they behave**" |

缺的只是把它们接成阶梯，并加一条生成规则。

#### 5.7.1 四级阶梯

每一级都是该事实**最早能被检查**的时刻，与 §6.2 同构：

| 级 | 时刻 | 被检查的是 | 机制 | 今天 |
| --- | --- | --- | --- | --- |
| **L1** | 实现**发布** | `provides-interfaces` 是否属实 | §9.2 的静态比对，按 `SURFACE.txt` 的分组把导出的 `kal_` 名字**反推**成接口集 | 比对已有；反推与写回清单**没有** |
| **L2** | 依赖**解析** | `requires ⊆ provides` | 集合包含（P5） | 无 |
| **L3** | **链接** | 消费者**实际用到的** ⊆ 它声明的 | 消费者对象的未定义 `kal_` 符号 ∩ `SURFACE.txt` → 映射到接口 → 集合比较 | 无 |
| **L4** | **运行** | 实际在场的接口 | `kal_interfaces()` 位字 | 已有 |

#### 5.7.2 关键规则：`provides-interfaces` 不得手写

> **实现包的 `provides-interfaces` 由产物生成，并断言"清单里的 = 产物里的"。**

实现包的 CI 跑 §9.2 的比对，按 `SURFACE.txt` 的分组反推接口集，写回清单；CI 同时
断言两者相等。这与 `generated_files` 同纪律——**一个从产物派生的声明，不可能与产物
不符**。它也顺便消灭一类会发生的事：实现新增一个接口却忘了改清单。

L1 因此不是"再加一道检查"，而是**取消手写**。

#### 5.7.3 为什么 L3 是必需的，不是锦上添花

L1 与 L2 都管不到**消费者**的声明。没有 L3，`requires-interfaces` 是纯粹被信任的：
一个包可以只声明 `openkal.fs` 却调 `kal_net_*`，解析期通过，**在提供 net 的实现上
链接也通过**——只有在不提供 net 的实现上才会炸，而那时错误出现在**用户的目标上**，
不是在包作者自己的构建里。

L3 把这次失败从"某个用户的某个目标"提前到"包作者自己的构建"。它也很便宜：mcpp 已经
有链接这一步，取消费者对象的未定义符号、与 `SURFACE.txt` 求交、映射到接口、比较集合。

**L3 的报告形态**：多用（用了没声明的）是**错误**，指名符号与它所属的接口；
少用（声明了没用的）是**提示**而不是错误——一个包可以按 feature 或按目标条件地使用
某个接口，当前这次解析里没用到不代表声明是错的。

**更正(2026-09-20，落地时发现)：L3 的代价被这份设计低估了。** 上文写 L3 的机制是
「取消费者对象的未定义符号、与 `SURFACE.txt` 求交、映射到接口」——**这要求引擎持有
符号到接口的映射**，而 §5.5 同时写着「引擎不认识这两个集合的任何一个成员」。两句话
不能同时成立。

映射只能来自图，因此 L3 的真实形状是实现包再声明一张表：

```toml
[kernel-abi]
provides-interfaces = ["openkal.fs", "openkal.stream", ...]

[kernel-abi.interface-symbols]        # 同样由 SURFACE.txt 生成
"openkal.fs"     = ["kal_fs_open", "kal_fs_close", ...]
"openkal.stream" = ["kal_stdin", "kal_stdout", ...]
```

这张表在每个实现的清单里约一百行，由同一个脚本生成，引擎只做「未定义符号 → 它属于
哪个接口 → 该接口在不在消费者的声明里」这一串查表与集合差，仍然不认识任何名字的含义。

代价与收益都变了，因此 L3 **不在本轮落地**，并且它的缺席有一个必须写下的后果：

> `requires-interfaces` 在消费者一侧**目前只被信任**。一个包可以声明少于它实际调用的
> 接口，解析期通过，在提供该接口的实现上链接也通过——只有在不提供的实现上才会炸，
> 而那时错误出现在**用户的目标上**，不在包作者自己的构建里。

这正是 §5.7.3 说 L3 要消除的那件事，它今天还在。

#### 5.7.4 风险边界：写错不会静默

这是"这两个事实可以由声明承载"的根据——不是因为声明可信，而是因为**每一种写错都响亮**：

| 写错 | 后果 | 何时被发现 |
| --- | --- | --- |
| `provides` 多声明 | 解析通过，链接报未定义符号 | 链接（§6.1 的常规报告） |
| `provides` 少声明 | 解析期误拒，指名缺的接口 | 解析（响亮） |
| `requires` 多声明 | 在能力较少的实现上误拒 | 解析（响亮） |
| `requires` 少声明 | **L3 抓住**；无 L3 则在部分实现上链接失败 | 链接 |

**没有一格是静默的。** 加上 L1 取消手写、L3 抓消费者，四种写错里两种在自己的 CI 里
就被挡住，另两种落在响亮的位置。

#### 5.7.5 C 库一级：枚举例外，不枚举规则

同一条阶梯能不能照搬到 `c-abi`？**不能，而且不该。** openkal 有 16 个接口和一份
`SURFACE.txt`，所以可以**正向**枚举；C 库有约 1200 个 POSIX 函数，正向枚举就是
SPEC §3.3 撤回过的那件事（naming sets）。

但**反向枚举是有界的**。`openkal-musl` 的 README 已经列出了它不能提供的东西，约六项，
并且写下了正确的理由：

> The following are absent, and each is **refused rather than quietly accepted**,
> because **a facility that reports success and does nothing is the one kind of
> answer that leaves a program wrong without telling it.**

这段散文就是 §6.1 的原则，只是**没有任何东西在执行它**——而它已经被烧过一次，README
自己记录着：0.16.0 之前 `SIG_IGN` 对每个信号都被接受却一个都没安装，"a program that
asked not to be ended by the interrupt keystroke **was told it had succeeded and was
ended by it**"。

**形状**：把这份散文变成机读声明，并由 CI 断言它与产物一致。

```toml
# openkal-musl 的清单
[c-abi]
presents   = "posix"
data-model = "arch-default"
wchar      = 32
builtins   = "iso"

# 枚举例外，不枚举规则。form 是该缺席以何种形状到达调用方。
[c-abi.absent]
fork     = { targets = ["*"], form = "link"   }  # 符号不定义，链接期报
mprotect = { targets = ["*"], form = "enosys" }  # 定义存在，报 ENOSYS
sigaction-handler = { targets = ["*"], form = "enosys",
                      note = "非默认/非忽略的处置" }
```

两个价值，各自独立：

1. **CI 可断言。** `form = "link"` 的名字必须**不在**产物的定义里；`form = "enosys"`
   的必须**在**。这把一份从来没有执行者的散文变成一条会红的断言。
2. **mcpp 的诊断可以接话。** 链接报 `undefined reference to 'fork'` 时，mcpp 读到
   C 库的 `[c-abi.absent]`，补一句「`openkal-musl` 声明 `fork` 在 `x86_64-windows-gnu`
   上不可用（link 形式）」，而不是让用户自己去查一份 README。

**`form` 这个字段本身承重。** `link` 是 §6.1 要的形状；`enosys` 是需要辩护的例外，
README 今天为每一条都写了辩护（`mprotect`：「musl asks for a guard page ... and
proceeds without one when told this, so the honest answer is also the one it is
prepared for」）。把辩护变成一个字段，**"有多少例外"就成为一个看得见、会增长、可以
被 review 盯住的数字**——而不是散在一篇 README 里。

#### 5.7.6 实现代价

| 部件 | 在哪 | 规模 |
| --- | --- | --- |
| `SURFACE.txt` → 接口集的反推 | openkal `tools/` | 一个脚本，按 `# openkal.x` 标题分组 |
| L1 断言 + 写回清单 | 每个实现包的 CI | 复用上面的脚本 |
| L2 集合包含 | mcpp 解析期 | 一条规则，引擎不认识任何接口名 |
| L3 未定义符号比对 | mcpp 链接期 | 取对象的未定义符号、求交、映射、比较 |
| L4 | 已有 | — |
| `[c-abi.absent]` 断言 | openkal-musl 的 CI | 一个脚本 |
| `[c-abi.absent]` 诊断接话 | mcpp 链接失败路径 | 一处 |

**引擎侧新增两处**（L2 解析、L3 链接），且**都不认识任何具体名字**：L2 做集合包含，
L3 做集合差。名字的含义全部来自 `SURFACE.txt`，而那是 openkal 的 normative 文件，
不是引擎的知识。这与 `cenv.cppm` 的「GENERIC KNOWLEDGE, NO PACKAGE NAMES」同纪律。

#### 5.7.7 判据

- L1：把某个实现的一个接口的定义删掉，其 CI 必须红，并指名那个接口。
- L2：消费者声明一个实现不提供的接口，解析期必须拒绝并指名。
- L3：消费者调用一个**没有声明**的接口，链接期必须报错并指名符号与它所属的接口；
  **且在提供该接口的实现上同样报错**——否则这条判据只是重复了 §6.1。
- L4：已由 openkal conformance 覆盖。
- `[c-abi.absent]`：把 `fork` 的 `form` 从 `link` 改成 `enosys`，CI 必须红。

最后一条与 L3 的第二句是同一个形状：**一条判据必须在"机制本身会通过"的那一侧
也成立，否则它测的是机制不是声明。**

---

## 6. 使用方式与场景

### 场景一 —— 纯 POSIX 库放进 openkal（目标：无感）

```toml
# 库自己的清单：什么都不用写
[package]
name = "mylib"
```

构建 `x86_64-windows-gnu` + openkal 图：`_WIN32` 不定义、`__unix__` 定义、LP64、
wchar 32。库里的 `#ifdef _WIN32` 分支不选中，走 POSIX 分支。若它用到
`/dev/urandom`，由 P4 合成。**零适配。**

这是今天 asio、eigen、catch2、re2、CLI11、lua、spdlog 应当落入的场景——它们今天红，
只是因为测量的图里 `runtime` 还钉在 `0.10.0`，没有任何包声明 `[c-abi]`。

### 场景二 —— 需要 Win32 API 的库

```toml
[features]
default = ["posix-socket"]
posix-socket = { defines = ["MYLIB_POSIX_SOCKETS"] }
winsock      = { defines = ["MYLIB_WINSOCK"] }

[feature-deps.winsock]
win32-headers = "1.0"          # 平台 SDK 由图提供，不传给下游
```

选 `winsock` 而图里没有 `win32-headers` → **解析期**拒绝，指名缺什么。
不选就走 POSIX 路径。**不在预处理期猜。**

### 场景三 —— 需要 Linux 内核节点的库

```toml
[package]
requires = ["mcpp:kernel-abi=openkal"]

[kernel-abi]
requires-interfaces = ["openkal.fs", "openkal.event"]   # event 今天是 reserved
```

在任何图上都解析失败，测量记 `refused`，**不计入失败率**。
这就是"编不过是正确答案"的机器表达。

### 场景四 —— 程序需要 Windows C 运行时

```toml
# 程序的清单：换一个入口包，整棵图跟着换形态
[dependencies]
openkal-win-ucrt = "0.1"
```

`_WIN32` 定义、LLP64、wchar 16，`kal_*` 仍然可用。**形态由程序选，库不得选。**

### 场景五 —— 包自己的构建配置

```lua
-- compat.zlib.lua：无条件应用，头里按事实自判断
cflags = { "-include", "mcpp_zlib_config.h" },
generated_files = {
    ["mcpp_generated/include/mcpp_zlib_config.h"] =
        "#if !defined(_WIN32)\n#define Z_HAVE_UNISTD_H 1\n#endif\n",
},
```

**判断的是事实（`_WIN32` 在不在），不是身份（C 库是不是 musl）。**
撤回前那条 `cfg(all(windows, c-abi = "musl"))` 是身份判断，是 `[c-abi]` 要消灭的写法。

**可度量的健康指标**：索引里 `cfg(c-abi = ...)` 的出现次数。今天是 **2**（都在
libarchive，且都在问"平台 SDK / 内核头在不在图里"——正是 P5 要补的那一维），
目标是 **0**。

### 场景六 —— 写一个新库，怎么写才在全生态可移植

1. 不问身份宏，问**事实**（`__has_include`、feature、或什么都不问走标准路径）。
2. 需要某个 openkal 接口，写进 `requires-interfaces`，让解析期回答。
3. 需要平台 SDK，写成 per-target 依赖 + feature。
4. 自己的构建开关写进描述符，无条件应用，头里按事实自判断。

---

## 7. 多维评估

| 维度 | 评估 | 可测量的判据 |
| --- | --- | --- |
| **兼容性** | 第一类失败（`_WIN32` 选错分支）自愈；第二类由 P3 关闭；第三类归 A；"需要平台能力"的由 P5 转成 `refused`。**兼容率不再是引擎的分数** | 30-member 重测：`fails` 与 `refused` 分开计 |
| **稳定性** | 所有方案落在三时刻表内，不存在"被下一个环境证伪"的类别名或借来的宏 | 索引里 `cfg(c-abi=...)` 次数 → 0；`presents` 取值集不增长 |
| **易用性** | 纯 POSIX 库零适配（场景一）；需要能力的包写五行（场景三）；换形态改一行依赖（场景四） | 一个新 compat 包进索引时需要写的 `target_cfg` 条数 |
| **简洁性** | 新增两个机制（P4 合成表、P5 接口列举），各补一格；撤掉一个借用（P3）；冻结一个名字（P1）。**净新增概念为一（接口列举），其余都是既有机制的延伸** | 新形态需要的引擎改动数 = 0（P6 已验证） |
| **可维护性** | P4 的上界由接口集自动给出；P5/P7 的引擎规则只有集合包含与集合差；P1/P2 是文档与字段 | 引擎里"认识某个具体名字"的处数不增加 |
| **可验证性** | 四个时刻各有一条会红的断言；`provides-interfaces` 由产物生成而非手写；C 库一级的例外表把一份无人执行的散文变成断言 | 删掉一个接口的定义，实现的 CI 必须红；调一个未声明的接口，链接期必须报错**且在提供该接口的实现上同样报错** |
| **未来扩展性** | 新形态 = 一个包；新接口 = SPEC 一行 + 两侧清单各一行；新平台 = 一个 kernel-abi 实现 | 加一个 `openkal.event` 需要动几个仓库：SPEC 1 + 实现 N + 消费者清单，引擎 0 |

**三处不优雅，本文全部关闭**：

| 边缘 | 状态 |
| --- | --- |
| 借来的 `__CYGWIN__` | P3 关闭（需重测确认） |
| 空着的解析期格子 | P5 关闭 |
| 探针只铺到宏、没铺到接口 | **P7 关闭**。四级阶梯（L1 发布 / L2 解析 / L3 链接 / L4 运行），材料全部已在仓库里（`SURFACE.txt`、SPEC §9.2、`kal_interfaces()`），引擎侧新增两处且都不认识任何具体名字。C 库一级枚举例外而不枚举规则（`[c-abi.absent]`） |

**P7 同时改变了"可验证性"这一维的整体评级**：在它之前，`[c-abi]` 只有宏一级被校验，
接口一级完全是信任；在它之后，四个时刻各有一条会红的断言，且 `provides-interfaces`
由产物生成而非手写。

---

## 8. 影响面

| 层 | 影响 | 性质 |
| --- | --- | --- |
| openkal 规范 | 零。不新增也不修改任何 `kal_*` | — |
| openkal 各实现 | P5/P7 要求填 `provides-interfaces`，且**由产物生成而非手写**；CI 增加 L1 断言 | 一次性，完全机械化 |
| openkal-musl | P4 在 `port/` 新增合成表与 conformance 断言；P7 把 README 的"absent"散文表变成 `[c-abi.absent]` 并加 CI 断言 | 新增能力，向后兼容 |
| openkal-llvm-runtime | 零 | — |
| mcpp 引擎 | P0 两处修复；P5 新增字段与解析规则；**P7 新增链接期集合差与 `[c-abi.absent]` 诊断接话**；P3 改两个令牌；P1 改文档 | 新增能力，向后兼容 |
| mcpp-index | P2 拆 `status`；libarchive 那 2 处身份判断改成 P5 声明 | 跟随 |
| 终端用户（用 openkal） | P3 改变 `__CYGWIN__`，该目标上的包重建一次 | 一次破坏性 |
| **不用 openkal 的用户** | **零。命令行逐字节不变** | 已由现有"未声明者不变"保证 |

---

## 9. 顺序

```
P0 探针修复 ─┐
P1 冻结语义 ─┼─ 互不依赖，可并行
P2 refused  ─┘
     │
     ├─→ 发布链走完，抬 pins.toml 的 runtime，重测 ──┬─→ P3 撤 __CYGWIN__（需重测数据）
     │                                              └─→ 兼容性基线重新确立
     │
P4 合成节点 ──── 独立，可与上并行
P5 接口列举 ─┬─ 同一次改动的两半，不得只落地 P5
P7 校验阶梯 ─┘   （L1 先行：它只改 openkal 各实现的 CI，不动引擎）
P6 win-ucrt ──── 需要 c++-abi 侧配套，最后
```

**P7 的 L1 可以独立先行**：把 `SURFACE.txt` → 接口集的反推脚本与 CI 断言做出来，
不依赖任何引擎改动，而且它产出的正是 P5 需要的那份 `provides-interfaces`。
`[c-abi.absent]` 的 CI 断言同样可以独立先行。

P3 **不得**先于重测落地：libarchive 的生成配置头含
`#if defined(_WIN32) && !defined(__CYGWIN__)`，撤销会改变它的分支。

---

## 10. 需要决定的问题

- **D1 P3 是否执行。** 它推翻一条已发布的设计决策（`__CYGWIN__` 保留），
  但那条决策自己写着"留给 30-member 测量翻转"。证据已在（mimalloc 的注释、
  sqlite3 的检测列表），但代价需要重测。**建议：执行，在重测之后。**
- **D2 P4 放 `port/` 还是独立包。** 建议 `port/`，理由见 §5.4。
- **D3 P5 的字段命名与归属层。** 本文提的是 `[kernel-abi] requires-interfaces /
  provides-interfaces`，按层泛化。另一种是并入 `requires` 列表。
  建议前者——`requires` 今天命名的是层，混入接口名会让一个列表有两套词汇。
- **D4 P2 的 `refused` 判据。** 本文要求它必须来自包的声明（P5），不得来自
  诊断字符串匹配。这意味着 P2 的完整形态依赖 P5；在 P5 之前，`refused` 只能由
  描述符显式标注。**建议：先上显式标注，P5 之后改为自动。**
- **D5 "对象格式是 PE"是否需要一个名字。** 建议先不给，让包问 `cfg(os="windows")`；
  若重测显示确有预处理期的需要，由 mcpp 定义自己的名字，不再借用。
- **D6 `provides-interfaces` 由产物生成，还是允许手写后校验？** 本文提的是**生成**
  （§5.7.2），理由是"从产物派生的声明不可能与产物不符"，而且顺带消灭"新增接口忘改
  清单"这一类。代价是实现包的 CI 多一个生成步骤。**建议：生成。**
- **D7 L3（链接期集合差）是否纳入本轮。** 它是 `requires-interfaces` 唯一的校验者，
  没有它 P5 的消费者一侧是纯被信任的。代价是 mcpp 链接期多一次符号读取。
  **建议：纳入，但可以先做成 warning，一个发布周期后转 error。**
- **D8 `[c-abi.absent]` 的 `form` 取值集。** 本文用 `link` / `enosys` 两个值。
  是否需要第三个（例如"接受但无效"——README 记录的 `tcsetattr` 部分字段即是）？
  **建议：需要，命名为 `accepted-no-effect`，因为它正是 0.16.0 之前那个真实缺陷的
  形状，给它一个名字才能被盯住。**

---

## 11. 附录：本文依据的实测

全部在本机（Linux x86_64，clang 22.1.0）执行，可逐条重跑。

```sh
# 三元组预定义：cygwin 不定义 _WIN32，mingw 定义
clang --target=x86_64-pc-cygwin       -x c -E -dM - </dev/null | grep -E '_WIN32|__CYGWIN__|__unix__|__SIZEOF_LONG__'
clang --target=x86_64-w64-windows-gnu -x c -E -dM - </dev/null | grep -E '_WIN32|__MINGW32__|__SIZEOF_LONG__'

# 预定义随目标不随宿主（否定 #673 的归因）
clang --target=riscv64-none-elf -x c -E -dM - </dev/null | grep -c __linux__          # 0
clang --target=riscv64-none-elf -x c -E -dM - </dev/null | grep __SIZEOF_WCHAR_T__    # 4

# freestanding 探针 argv 的实际形状：量的是宿主
clang -D__unix__ -fno-short-wchar -ffreestanding -x c++ -E -dM - </dev/null | grep __linux__   # 命中

# -include 进模块接口单元
printf '// gen\n\nmodule;\n\n#include <cstddef>\n\nexport module m;\n' > m.cppm
clang++ -std=c++23                   --precompile m.cppm -o /dev/null   # OK
clang++ -std=c++23 -include unistd.h --precompile m.cppm -o /dev/null   # error: 'module;' ... only at the start

# -include 进 GAS
printf '.text\n.globl f\nf:\n  ret\n' > a.S
clang -c -include unistd.h a.S -o a.o       # invalid instruction mnemonic '__begin_decls'

# appendUniqueFlags 去重之后的命令行
clang -c -include unistd.h sys/stat.h t.c   # no such file or directory: 'sys/stat.h'

# -nostdlibinc 不关 clang 自带头
printf '#include <intrin.h>\nint main(){}\n' > i.cpp
clang++ --target=x86_64-pc-cygwin -nostdlibinc -fsyntax-only i.cpp   # intrin.h:12:15

# glibc 的 stdio.h 不传递包含 unistd.h（否定 #674 设计稿 §1.2）
printf '#include <stdio.h>\nlong f(void){return lseek(0,0,0);}\n' > s.c
clang -fsyntax-only -std=c11 s.c            # call to undeclared function 'lseek'
```

索引侧：

```sh
cd ~/workspace/github/mcpplibs/mcpp-index
python3 -c "
import json; d=json.load(open('.xpkgindex/openkal-compat.json'))
print(d['pins'], d['measured'])
for n,i in sorted(d['members'].items()):
    t=i['targets']['x86_64-windows-gnu']
    if t.get('status')!='runs': print(n, t.get('status'), t.get('diagnostic','')[:110])
"
grep -rho 'cfg([^)]*c-abi[^)]*)' pkgs/ | sort | uniq -c     # 今天 2 处，都在 libarchive
sed -n '1,35p' tests/openkal/pins.toml                      # runtime = "0.10.0"
```

---

## 12. 参考

- openkal SPEC 0.14：§3.2（core set 不增长）、§3.3（撤回 `hosted`）、§3.4（拒绝统一名字解析）、§5.2（mapping 不是 simulation）、§6.1（运行期拒绝是缺陷）、§6.2（三时刻）、§6.5（解析期决定）、§7.1（naturalness）、§7.7（absence as an answer）
- openkal SPEC 0.14：§9（conformance procedure，四半）、§4.4（S/L/X 与 `kal_interfaces`）
- `openkal/SURFACE.txt` —— normative，104 个名字 / 16 个接口分组
- `openkal/include/openkal/version.h:55-91` —— `kal_interfaces` 的位字与"不是能力"的界定
- `openkal-musl` `README.md` "The following are absent" 表 —— 例外枚举的现成内容与理由
- `openkal-musl` `port/src/okm_fd.c` 文件头 —— 合成层已经存在的证据
- openkal design 2026-09-18：§3.1（形态表）、§3.2（`[c-abi]`）、§5（平台相关代码）、§9（备选与不做）、§12（自审）
- `mcpp` `src/toolchain/cenv.cppm`、`src/toolchain/cenv_probe.cppm`、`src/build/prepare.cppm`（3989、6380、10790、11148、12380、12641）
- `docs/22-target-side.md` §295-430
- `mcpp-index` `.xpkgindex/openkal-compat.json`、`tests/openkal/pins.toml`、`pkgs/c/compat.zlib.lua`、提交 700f7de
- 评审记录：`.agents/docs/2026-09-20-issue-674-design-review.md`
