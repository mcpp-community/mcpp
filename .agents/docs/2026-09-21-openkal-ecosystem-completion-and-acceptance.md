---
subject: plan
status: active
---

# openkal 生态：完整性收尾与验收方案

- 日期：2026-09-21
- 依据：`.agents/docs/2026-09-20-openkal-c-environment-ecosystem-design.md`（下称「设计」）。
  本文不重述它的论证，只处理**它没有实现的部分**与**怎么验收**。
- 验收载体：`lsp-mcpp-private`（`mcpp-language-server`）

---

## 0. 本文回答三个问题

1. 2026.9.20.1 这一轮之后，生态还差什么。
2. 这些差项的**判据**分别是什么，谁负责，卡在什么上。
3. 怎么用一个**真实程序**验收整条链，而不是靠合成夹具。

第三点是本文与设计的主要差别。设计的验证是引擎侧的单测、e2e 与 30 成员测量；这些都
只回答「引擎与包各自对不对」。**没有任何一处回答「一个真实的、非玩具的 C++23 程序，
能不能在三个目标上只写一份源码」**，而那才是 openkal 存在的理由。

---

## 1. 分母：本轮已关闭的

给未实现项一个对照，不展开。

| 项 | 形态 |
| --- | --- |
| P0 探针 | 探针量它要核对的目标，而不是构建宿主 |
| P1 | `presents` 冻结；`[c-abi]` 的词汇不再增长 |
| P2 | `refused` 与 `fails` 在测量里分开计 |
| P5 | `[kernel-abi] provides/requires-interfaces`，解析期集合差 |
| P7-L1/L2 | 声明由产物派生（三个实现均已落地）；解析期比对 |
| `[c-abi-absent]` | C 库一级枚举例外而不枚举规则，24 行，CI 逐条断言 |
| 宿主面 | 五处 `fs::which` 穷举，两行补齐，扫描方式写进文档 |

**测量基线（旧图，runtime 0.12.0 / mcpp 2026.9.18.3）**：60 个 `成员 × 目标` 组合，
50 通过（其中 47 个 `runs (posix)`），10 失败。失败按根因：

| 数量 | 根因 | 成员 | 归属 |
| --- | --- | --- | --- |
| 4 | `windows.h`，经**我们自己定义的** `__CYGWIN__` | archive, sqlite3, mimalloc, c-ares | **P3** |
| 2 | `__cxa_thread_atexit` | spdlog, doctest | **C1** |
| 2 | `linux/` uapi 头 | curl, cmp-module | C2 |
| 1 | `arc4random_buf` | expat | C3 |
| 1 | `curl_off_t` | curl | C4 |

这张表是本文所有优先级的来源：**P3 一条覆盖 40% 的失败，P3 + C1 覆盖 60%。**

---

## 2. 未实现清单

按**谁负责**分组。每项给出：动机、判据、阻塞。

### 2.1 引擎侧（mcpp）

#### E1 — P3：撤掉 `__CYGWIN__` 借用

**状态**：**三步全部落地**。第一步 mcpp 2026.9.21.1（加名字）+ 2026.9.21.2（改大写）；
第二步 `openkal-musl@0.19.0` 与 `openkal-llvm-runtime@0.14.0`；第三步 mcpp 2026.9.21.2
停止定义借来的名字。下文保留论证；落地形态与被交叉验证挡下的那一版见本节末。

保留 `__CYGWIN__` 的本意是给「PE 格式 + POSIX C 环境」一个名字。实证否定了这个用法：

```c
/* mimalloc/atomic.h */
#if defined(_WIN32) || defined(__CYGWIN__)  // we use windows locks on cygwin, but otherwise treat it at unix
/* xz/tuklib_physmem.c */
#if defined(_WIN32) || defined(__CYGWIN__)
/* sqlite3.c:15456 —— SQLITE_OS_WIN 的判定列表 */
#  if defined(_WIN32) || defined(WIN32) || defined(__CYGWIN__) || \
```

上游用这个名字表达的是「**Win32 API 可用**」。**一个借来的名字，语义由借出方的历史
决定，不由我们的意图决定。**

**2026-09-21 新增的源码级验证**：`tuklib_physmem.c` 是一条 `#if/#elif` 链，两个宏都
不成立时**不包含任何平台头**，而函数体同样的链落到 `#endif` 后 `return ret;`（`ret = 0`，
「物理内存未知」，xz 自己处理）。**所以撤掉 `__CYGWIN__` 后该文件编译通过。**

> 这一条推翻了一个流传中的结论。`lsp-mcpp-private` 线上的分析把这个失败归给
> 「xz 不适配、上游不打算修」，引的是 openkal 0.13 记录第 5 节「xz 去掉 `_WIN32` 后
> 在 musl-Windows 上**解码失败**」。那是**运行期**的说法；现在撞上的是**编译期**
> 失败，两者被并成了一条。**编译这条是我们自己造成的，解码那条 P3 不自动回答。**

**形状**：`cenv.cppm` 的 Windows+Posix 分支加 `-U__CYGWIN__ -U__CYGWIN32__`，
`expectUndefined` 加两项。

**「对象格式是 PE」这一维**：默认**不给宏**。包问 `cfg(os = "windows")`，那不需要任何
宏。若重测证明确有第三方代码只能在预处理期问，回落是 mcpp 定义**自己的**名字
（`__mcpp_format_pe__`），不继续借别人的。

**判据**：30 成员重测，`windows.h` 组从 4 降到 0，且**总失败数不增加**。

**阻塞**：无技术阻塞。**风险点已点名**：libarchive 的生成配置头含
`#if defined(_WIN32) && !defined(__CYGWIN__)`，撤销会**翻转**它的分支。基线已有，重测
即可读出净值。

**先做的那一版被跨仓库交叉验证挡下了，这条记录比结论更值钱。**

第一次实现就是直接撤：加 `-U__CYGWIN__ -U__CYGWIN32__`，`expectUndefined` 加两项。本机
全绿，五个生态仓库里四个也绿——**openkal-llvm-runtime 红了**，libunwind 的
`static_assert` 失败：`Registers_x86_64` 装不进 `unw_context_t`。

根因是生态里有**两个已安装的公开头**有意读 `__CYGWIN__`，理由写在各自源码里：

| 包 | 头文件 | 它定尺寸的记录 |
| --- | --- | --- |
| openkal-musl | `port/include/bits/setjmp.h` | `jmp_buf` |
| openkal-llvm-runtime | `__libunwind_config.h` | `unw_context_t`、`unw_cursor_t` |

两者都写明：这是**已安装**的头，会被**应用程序自己的编译**读到，所以用不了包私有的
`OKM_TARGET_WINDOWS` / `OPENKAL_TARGET_WINDOWS`；而 `__CYGWIN__` 是 mcpp **target-wide**
保持定义的唯一名字。

**那个「四比零」数的是第三方读者，没数我们自己的。** 而自己这两个是承重的，且错了是
**静默**的——`setjmp.h` 自己的注释写着「a mismatch nothing reports until the record
overruns」。libunwind 那处有 `static_assert` 才响，属于运气。

**落地形态（第一步）**：`cenv.cppm` 的 Windows+Posix 分支加 `-D__MCPP_TARGET_WINDOWS__=1`
（2026.9.21.1 发的是小写拼法，2026.9.21.2 在它还没有消费者时改成大写——命名约定见
`src/toolchain/predefines.cppm` 与 `docs/21`），`expectDefined` 加该项——探针核对，一个
没生效的 `-D` 是校验失败不是沉默。实测：三个通道（`.c`/`.cpp`/`.S`）全部到达；预定义为
`__unix__` + `__MCPP_TARGET_WINDOWS__`，`_WIN32` 仍不存在。

**为什么名字是 `__MCPP_TARGET_WINDOWS__` 而不是设计里写的 `__mcpp_format_pe__`**：两个
消费者要的不是「目标文件格式」，是**调用约定**（Win64 的寄存器保存区大小）。在这个目标上
两者同变，但按消费者实际问的那个问题命名更诚实。

**第二、三步（已落地）**：两个包改读新名字并保留 `|| defined(__CYGWIN__)`，于是在本次改动
两侧的引擎上都能构建；2026.9.21.2 的 `cenv.cppm` 在编译行加 `-U__CYGWIN__` /
`-U__CYGWIN32__`，`expectUndefined` 同步。

**次序是被测出来的，不是被断言的。** 它成立于**仓库之间**，所以任何一个仓库里的测试都
检查不到它。在 2026.9.21.2 的引擎上，拿**已发布的** `openkal-llvm-runtime@0.13.0` 为
`x86_64-windows-gnu` 构建一个 openkal 程序：

```
static assertion failed: x86_64 registers do not fit into unw_context_t
static assertion failed: UnwindCursor<> does not fit in unw_cursor_t
```

那就是「先做第三步」的读数，也就是第二步必须先发布的理由。`setjmp.h` 是沉默的那一半——
它自己的注释写着「a mismatch nothing reports until the record overruns」。

**残余窗口被点名而不是被说没有**：把 `openkal-musl` **精确**钉在 0.18.0 或更早、同时把
引擎升过 2026.9.21.2 的工程，会拿到那个静默的 `#else`。在引擎发布之前把索引的 `latest`
移到 0.19.0，是把窗口压到「精确钉」的办法；这里没有任何机制能把它关掉，因为引擎无从知道
一个包安装出去的头读了哪些宏。

#### E1c — 交叉验证协议的一个结构性漏洞：没有人建 Windows-over-openkal

**2026-09-21 实测发现，在用协议验 E1 的过程中。** 逐 job 核对两个包的交叉验证运行：

| 仓库 | 它的 CI 建的目标 | 有没有 Windows 目标 |
|---|---|---|
| `openkal-llvm-runtime` | `runtime`(linux + riscv 裸机)、`host-dimension` 矩阵(macOS 宿主、**Windows 宿主 → 每个目标**)、两个「产物在那个系统上跑」 | **有** |
| `openkal-musl` | linux/gcc、linux/llvm、macos/llvm、cross-link(Linux↔macOS)、「跨建的产物在那边启动」 | **没有** |

**先写下更正，因为我第一次读错了。** 只看 runtime 运行里的第一个 job，我下过结论说
「整个生态没有一个包建 Windows-over-openkal」。那是错的：`host-dimension` 的 Windows
行实测建了 `x86_64-windows-gnu`，并解析到 `openkal-musl@0.19.0`。**E1 的 Windows 那条腿
确实被验到了**，结论成立。

**真正的缺口比那小，但仍然是缺口：`openkal-musl` 自己没有任何 Windows 格子。** 而本轮
它改的恰恰是 `bits/setjmp.h`——一个**只在 Windows 目标上有分支**的已安装头。它今天被验到
是**传递的**：runtime 的 CI 把 musl 当 path dep 拉进去，顺带编了它。一个包的关键改动
由另一个仓库的 CI 代为验证，是[[a-passing-criterion-that-measured-nothing]] 的邻居——
它今天对，是因为恰好有人在别处建了那个目标。

**为什么补这个格子不是顺手的事，是实测出来的。** 想写一个最小判据——一个 `setjmp` /
`longjmp` 程序，带 `_Static_assert(sizeof(jmp_buf) >= 32 * sizeof(unsigned long long))`
——只声明 `openkal-musl = "0.19.0"` 一个依赖，为 `x86_64-windows-gnu` 构建：

- **编译过了**，而那正是判据要抓的那一层：短 `jmp_buf` 是静默的，编译期断言是唯一能让它
  变响的地方。
- **链接不过**：`cpow.o` 等一批目标文件的引用无人解析。openkal-musl 单独不是一条完整的
  链接——openkal 实现与 compiler-rt 由 `openkal-llvm-runtime` 组装。

所以 musl 侧的 Windows 格子要么只做**编译期**断言（能抓这个缺陷，且便宜），要么把 runtime
当 path dep 拉进来（就是 runtime 的 CI 反过来做的那件事）。**两种都是新增工作量，不是
把矩阵加一行。** 记在这里，下一个接手的人不必再量一遍。

**唯一建那个目标的是 mcpp 自己的 `openkal-cross`**（3 宿主 × 3 目标）。而它把要验的
生态分支写死成 `OPENKAL_BRANCH: main`——于是一个**需要生态协同提交**的引擎改动，在那个
提交落地之前既无法验证、也无法合入（因为正是这个 job 会红）。

**修法**：`openkal-cross.yml` 增加 `workflow_dispatch` 输入 `openkal_ref`，留空时行为
不变。这是协议的另一半：生态仓库早就能用 `MCPP_SOURCE_REF` 指到 mcpp 的 PR 分支，反向
一直没有。

**次序上的后果**：`openkal-cross` 里那个示例通过 `path = "../.."` 依赖 runtime，而
runtime 的清单从**索引**钉 `openkal-musl = "0.19.0"`。所以用 `openkal_ref` 验之前，
musl 0.19.0 必须先登记进索引。

#### E1d — 「四个成员」是二，而那个分组按诊断而不是按真因（2026-09-21 重测）

撤销之后的 30 成员重测：**总失败 10→5，新增失败零**。但判据的另一半「`windows.h` 组
4→0」没有达成——是 **4→2**，而未达成的原因不是撤销无效，是**这个组从一开始就数错了**。

| 成员 | 记的真因 | 实测真因 | 撤销后 |
|---|---|---|---|
| `archive`（经 xz） | `__CYGWIN__` | **对** | **清了** |
| `sqlite3` | `__CYGWIN__` | **对** | **清了** |
| `c-ares` | `__CYGWIN__` | **错**：`ares_setup.h:81` 是 `#ifdef HAVE_WINDOWS_H`，而那个宏由 `mcpp-index` 自己的配方在 `windows = {` 块里 `#define`（`compat.c-ares.lua:1283`） | 仍红 |
| `mimalloc` | `__CYGWIN__` | **错**：已经不走到任何头文件 | 仍红 |

**四个是按「诊断」分的组。** 四个当时都停在 `windows.h`，于是被记成同一类。
**按诊断分组不是按真因分组**，这样数出来的数目会高估一次撤销能修掉多少。

这条是[[reasons-written-from-memory-kill-good-fixes]] 的镜像：结论（「撤销是对的」）被
复查并成立，而**理由里的那个数目从未被复查**，它被从上一份记录继承了三次——写进引擎注释、
CHANGELOG 和 PR 正文，直到重测把它否掉。

**判据达成一半比没写判据更有价值**：没达成的那一半正是它指出分母错了的地方。

#### E1e — `mimalloc` 暴露替身三元组的一个代价，与宏无关

```
fatal error: error in backend: Target OS doesn't support __builtin_thread_pointer() yet.
```

`presents = "posix"` 在 Windows 上实现成 `--target=x86_64-pc-cygwin`。LLVM 没有为那个 OS
实现 `__builtin_thread_pointer()`，而 mimalloc 用它取线程局部堆指针。

**这是这次替换的第一个被测量到的、超出宏名之外的代价。** 此前 `cenv.cppm` 关于替身三元组
的全部记录都只讨论「预处理器看到什么」——ABI 不变、链接不变、只有宏可见性变。这条说明
**代码生成器也看得见那个 OS**。

**可能的修法（都未验证）**：mimalloc 的配方关掉那条路径（它有 `MI_*` 配置）；或替身三元组
换一个 LLVM 实现了该 builtin 的 OS。**先量再选**——这条记在这里正是为了下一个人不必
从三十个成员的诊断里重新走一遍。

#### 一条测量工具的缺陷：`mimalloc` 的真诊断被兜底吃掉了

重测里 `mimalloc` 记的是 `fails: error: build failed`——`compat.py` 的 `first_diagnostic`
三条模式都没匹配（后端错误不带 `file:line:` 也不含 `FAIL`），于是兜底取了**最后一行**，
而最后一行是 mcpp 自己的总结。真因要本机复现才拿得到。

**判据的兜底不能是「最后一行」**：它在匹配失败时给出一个看起来像诊断的东西，比留空更坏。

#### E1b — 引擎拥有的宏改为全大写（2026.9.21.2）

**状态**：已落地，与 E1 第三步同一个 PR（用户要求：不分开，免得发布周期太长）。

`__mcpp_target_<os>__` → `__MCPP_TARGET_<OS>__`，`__openkal__` → `__OPENKAL__`。

**约定按「名字是什么」分，不按谁写的分。** 厂商名与产品名大写（`__APPLE__`、`_WIN32`、
`__MINGW32__`、`__GNUC__`），系统种类名小写（`__linux__`、`__unix__`）。mcpp **拥有**的
每一行都属于第一类。小写那一版的推理是「它们在守卫里与 `__linux__` 并排，所以跟它一致」
——**那是把「相邻」当成了「同类」**，而 `__APPLE__` 在同样那些守卫里却是大写。

**撤销小写拼法的代价是量出来的。** 分母是全生态每一个仓库，逐文件类型扫过：

| 撤销的名字 | 安装头里的读者 | 包源码/清单里的读者 | 第三方读者 | 暴露 | 步数 |
|---|---|---|---|---|---|
| `__CYGWIN__` | **6 处**（两个头） | — | **4 个成员** | 上游二十年 | 3 |
| `__mcpp_target_<os>__` | 0 | 0 | 0（按构造） | 1 个发布 | 1 |
| `__openkal__` | 0 | 0 | 0（按构造） | 3 天 | 1 |

「按构造」是指：两个名字都是在这里发明的，不可能有上游代码握着它们。**两次撤销之间规则
没变，变的是数目**——而一次不由数目支撑的撤销，正是本项目已经犯错过一次的那种论证
（E1 的第一个形态）。

用户先要求「两个拼法并存、文档推荐大写」，在看到代价读数后改为「代价不大就取消」。取消。

#### E2 — P7-L3：链接期集合差

**状态**：未实现。设计称其为「本轮与设计最大的偏差」。

`requires-interfaces` 今天只在**解析期**被比对，而解析期比的是**声明**。声明由产物
派生（L1 已保证），但**没有任何东西在链接期检查程序实际引用的符号是否落在声明的接口
集合内**。

**判据**：一个程序引用了实现未提供的接口里的符号，链接期被点名拒绝，且**声明正确时
零额外链接开销**。

**阻塞（2026-09-21 更正）**：**不是工作量，是一个设计决定。** 原文写「材料齐，是工作量
不是未知数」，实查否掉了这个前提：

1. **`SURFACE.txt` 不被安装。** `mcpplibs/openkal/mcpp.toml` 里没有任何一条把它装出去，
   所以链接期的引擎拿不到它。
2. **`docs/22` 有一条明写的原则挡在前面**：「**引擎不认识两个集合里的任何一个成员**。对
   它们做的唯一操作是集合差，所以规范可以新增一个接口而不需要发布 mcpp。」要在链接期
   点名**接口**，引擎就必须持有「接口 → 符号」的映射；映射放进引擎会直接违反这条。

所以真正要先回答的是：**映射住在哪，而引擎能不能读它却不学会这个生态的词汇。** 两条候选：

| 形态 | 代价 |
|---|---|
| 规范包安装 `SURFACE.txt`，清单新增一个键指向它 | 引擎要学会一种文件格式；新键 → 索引 floor 一轮 |
| 只报「解析出的实现声明了这 N 个接口，缺的符号不在它的导出里」 | 零新键、零新格式；**但点不出接口名**，弱于本项判据 |

第二条是今天就能做的，第一条才满足判据。**在做出这个决定之前，把 E2 排进任何一个 PR 都是
在实现一个还没选定的形状**——这正是「凭印象写下的『为什么不行』会否掉正确修法」的镜像:
凭印象写下的「为什么可以」同样会带来一个错的实现。

**后果**：E2 退出本轮（2026.9.21.2）。它不阻塞验收（A1/A2/A3 不依赖它），批次表已把它放在
第四批。

#### E3 — 未被回答的 requirement 没有提示

**状态**：**已落地**（mcpp 2026.9.21.2，与 E1/E1b 同一个 PR）。实际输出：

```
        note kernel-abi interfaces: fakekernel@0.1.0 states none, 2 requirements unchecked
```

判据落在 `tests/e2e/743` 的第三、四条腿上。**第四条腿是必需的**：没有它，第三条腿在一个
无条件打印这行的引擎上同样会绿——两条腿互为对照，这正是「判据通过了但什么都没测到」
那一类缺陷的形状。断言里包含**数目**（该夹具声明两条），因为数目是夹具唯一决定的那部分，
一个报「1」或「0」的提示仍会匹配所有只认标识符的断言。

三种情形两种绿：提供方声明且包含 → 构建（**已确认**）；声明但不包含 → 拒绝；
**什么都没声明 → 构建（从没被检查过）**。第三种是有意的（`provides-interfaces` 晚于
那些包出现），但它让「是」与「没问成」同读数。

生态侧本轮已补齐（三个实现都声明了），所以这条现在只对**第三方实现**成立。

**形状**：target-side 报告里加一行，形如
`kernel-abi interfaces: <provider> states none, N requirement(s) unchecked`。

**判据**：一个声明了 `requires-interfaces` 的包，配一个什么都没声明的提供方，构建
**成功**且输出里**有**这一行；提供方声明了则**没有**这一行。

### 2.2 C 库侧（openkal-musl）

#### C1 — `__cxa_thread_atexit`

**状态**：**两层都已定位并修复**，发在 `openkal-llvm-runtime@0.15.0`。记录：
`.agents/docs/2026-09-20-cxa-thread-atexit-finding.md` §7。

**第二层的真因**：`__thread DtorList* dtors` 在 `-femulated-tls` 下由 emutls 提供，而
emutls 把每线程的块挂在它自己的一个 pthread key 后面；那个 key 的析构先释放了本线程的块，
之后每次读都新分配一个**清零**的块。判据是同一线程里 `&dtors` 三次不同
（`...6a8` / `...6c8` / `...708`）。

**本清单里「唯一的未知数」是被它自己写下的判据关掉的**——发现文档 §6 写的第一步就是
「在 fallback 里打一行，看 `run_dtors` 到底有没有被调用」。它被调用了，而链表是空的。

**并且：§4 那条被判为「否」的假设其实是对的。** 那次探针的 `pthread_key_create` 排在第一次
访问 `thread_local` **之前**，而 musl 按创建顺序调 key 析构，于是 emutls 反而活得更久，
读到了期望值。真实情形顺序相反。**一个探针报不出它被构造成不会发生的那个顺序**——谓词是
对的，对象的构造把被测条件排除掉了。

**修法**：链表存进 key 自己的值（析构函数本来就被交给它），零新机制。

只补符号会把一个**构建期的响亮失败**换成一个**运行期的静默失败**：链接过了，
`thread_local` 的析构不跑。补丁试过并**主动回退**，因为验证显示析构确实没执行。

**判据（已通过）**：`examples/cxx` 两条断言，两个目标：

```
ok: a thread_local is constructed in a spawned thread
ok: and its destructor runs when that thread ends
```

`x86_64-linux-gnu` 与 `x86_64-windows-gnu`（wine）均 `failures: 0`。只断言链接通过、
或只断言构造发生，都会同时放过两层——这正是当初决定不发第一层补丁的理由，现在它变成了
判据本身的形状。

**阻塞**：无。本清单里唯一那个真正的未知数已关闭。

#### C2 — `linux/` uapi 头（curl, cmp-module）

**2026-09-21 更正：这是两件不同的事，本文初稿把它们并成了一类。** 逐条读了实测诊断与
配方之后：

| 成员 | 诊断 | 真正的归属 |
|---|---|---|
| `curl` | `lib/setopt.c:31: 'linux/tcp.h' file not found` | **配方缺陷**，与 C3/expat 同形状 |
| `cmp-module` | `asio/detail/config.hpp:899: 'linux/version.h' file not found` | **真的 C2** |

**curl 是配方缺陷。** `pkgs/c/compat.curl.lua` 生成的 `curl_config.h` 里有
`#define HAVE_LINUX_TCP_H 1`，位于 `#if defined(__linux__)` 之内。openkal 跑在 Linux
内核上，`__linux__` **是对的**；错的是配方把它读成了「glibc 的整套 Linux userspace 头
都装好了」。同一个块里还有 `HAVE_GLIBC_STRERROR_R`（openkal-musl 是 musl，不是 glibc，
这一条**主动是错的**）、`HAVE_SYS_EVENTFD_H`、`HAVE_FSETXATTR`、以及一条写死的宿主路径
`CURL_CA_BUNDLE "/etc/ssl/certs/..."`。**诚实的判据是 `__has_include(<linux/tcp.h>)`**
——它是 C 标准的、问的正是要问的那件事，而不是从「哪个内核」推断「哪些头存在」。

**cmp-module 才是 C2。** asio 的 `detail/config.hpp` 写的是：

```c
#if defined(__linux__)
# include <linux/version.h>        /* 在所有 ASIO_DISABLE_* 守卫之外 */
```

那个 `#include` **不受任何配置宏控制**——`-DASIO_DISABLE_EPOLL` 挡不住它。**源码不归
我们改，而没有任何清单键伸得进第三方的 `.c`/`.hpp` 里**（这正是 `predefines.cppm` 记的
第一条理由）。所以这个成员在面向 Linux 的 openkal 图里按构造建不起来。

**形态**：程序 `#include <linux/tcp.h>` 一类。openkal 不是 Linux，没有 uapi 头，
**这是正确的**。

**本文初稿把它写成「P5 应当转成 `refused`」，那是错的。** P5 比对的是 **openkal
接口**，而 Linux uapi 头不是任何 openkal 接口；引擎对它只能给出一条普通编译错误，与
其他编译错误不可区分。`compat.py` 的 `kind_of` 对 `fails` 刻意返回 `None`，并在注释里
写明理由：「没有声明或测量支持的标签，比没有标签更坏」——所以**没有现成机制可用**。

**正确形态**：这是成员**按构造不可移植**（无条件包含平台头），应当由 `members.toml`
上的一条**声明**表达，而不是由引擎猜。判据随之变成：该声明存在时，成员从兼容率的
**分母**里移除，且移除理由可追溯到那条声明。

**归属**：mcpp-index（测量的成员表），不是 C 库。

**已有机制比初稿以为的多一半。** `members.toml` 里已经有 `[excluded]` 表，它的语义正是
「在任何 openkal 图里都建不起来」——`cmp-module` 恰好符合。缺的不是表，是**第二个理由
类别**：现有六条都是「宿主程序 / 厂商二进制」，而 asio 这条是「上游源码无条件包含平台
头」。判据不变：声明存在时该成员从分母里移除，且理由可追溯到那条声明。

**curl 不进这张表**，它要修配方。两者分开之后，九条失败里这一类只剩一条。

#### C3 — `arc4random_buf`（expat）——**已修复，且归属与初稿不同**

**初稿写的是**「musl 有 `arc4random_buf`，openkal-musl 的移植未导出」。**这是错的。**
musl 1.2.5 **根本没有 arc4random**（`src/prng/` 是 rand48 一族），头文件里也没有这个名字。

**真因在索引配方**：`compat.expat.lua` 的 `generated_files["lib/expat_config.h"]` 是
**一份对所有目标通用的静态头**，里面从 glibc 的 configure 结果抄来了
`#define HAVE_ARC4RANDOM_BUF 1`。而这个宏**短路整条链**：

```c
#if defined(HAVE_ARC4RANDOM_BUF)
  arc4random_buf(&entropy, sizeof(entropy));   /* 无条件走这条，下面的回落永不到达 */
#elif defined(HAVE_ARC4RANDOM)
#else
#  elif defined(HAVE_GETRANDOM) || defined(HAVE_SYSCALL_GETRANDOM)
```

取消定义后落到 `HAVE_GETRANDOM`（同一份头里已定义，musl 与 glibc 都提供），再落到
`XML_DEV_URANDOM`。**没有任何目标因此失去高质量熵源。**

**状态**：已实现并验证（mcpp-index PR）。对着已发布栈（openkal-linux 0.15.0 /
openkal-musl 0.18.0 / openkal-llvm-runtime 0.13.0）构建，expat 2.7.1 编译通过、程序
链接并运行，产物 `nm -u` 里没有任何 arc4random 符号。

**归属**：mcpp-index 配方，不是 C 库。

#### C4 — `memset_pattern16`（macOS release）——**前提待重测，不要先动手**

Darwin libc 扩展。`-O2` 下编译器把填充循环换成它，openkal-musl 没有这个符号。这不是
「程序调用了它」，是**编译器合成的调用**。

**本文初稿据此把它列为 openkal-musl 的缺口，并当成 macOS 验收的硬阻塞。查下来前提
站不住。**

引擎**已经**在做本该阻止它的事：`cenv.cppm` 对 `builtins = "iso"` 且目标为
macOS/iOS 时发 `-fno-builtin-memset_pattern16`，而 openkal-musl 正是
`builtins = "iso"`。并且这个 flag **确实到达了那些包**——用 2026.9.21.1 对
`aarch64-macos` 发 build-database，`zstd` 的 29 个翻译单元（含报告点名的那条链上的
`zstd_decompress_block.c`）命令行上都带着它。

于是报告里「新栈照样失败」只剩三种可能，**没有一种在本机可判**（Linux 宿主编不了
macOS，缺 SDK）：

1. 那次测量跑在**加入该 flag 之前**的引擎上；
2. 合成出的是 `memset_pattern4`/`8`——引擎只发 `16` 那一个 flag，而 clang 的
   `-fno-builtin-` 家族是一个 idiom 一个 flag；但报告点名的确实是 `16`；
3. `-fno-builtin-memset_pattern16` 不足以关掉 LLVM 的 loop-idiom pass。

**读数已取到，C4 关闭：在当前栈上不复现。**

而且**是在本机取到的**——这推翻了本文另一处判断。openkal 的前提就是通用交叉构建，Linux
宿主经 openkal 栈可以构建 `aarch64-macos`：需要 Apple SDK 的是**平台路径**，不是 openkal
路径。我先前那个失败的探针没有依赖 openkal，于是走了平台路径，我把它读成了"macOS 本机
测不了"。

实测（mcpp 2026.9.21.1、llvm@22.1.8、`--profile release`、`--target aarch64-macos`，
经 openkal-macos 0.12.0 / openkal-musl 0.18.0 / openkal-llvm-runtime 0.13.0）：

| 成员 | 读数 |
| --- | --- |
| zstd 1.5.7 | 编译、链接通过；产物 `Mach-O 64-bit arm64, NOUNDEFS`；`memset_pattern` 引用 **0** |
| xz 5.8.3 | 编译、链接通过 |

两个都是报告点名的那条链上的。所以那份报告测的是**加入 `-fno-builtin-memset_pattern16`
之前**的引擎——三种可能里的第一种。

**归属**：无。不需要给 openkal-musl 加任何符号。

**留下的方法论**：一条"某平台本机测不了"的判断，要用**走 openkal 栈**的探针去验，不能用
走平台路径的。两者对 SDK 的要求完全不同。

#### C4' — 「关闭」是错的，而上面列出的第三种可能才是对的（2026-09-21 重测）

上面的关闭依据是 **zstd 与 xz** 链接通过、`memset_pattern` 引用为 0。**那两个是从旧报告
里抄来的对象，不是当前失败的那个。** `lsp-mcpp-private` 的 `test_archive` 在
`aarch64-macos` 上链接失败：

```
ld64.lld: error: undefined symbol: memset_pattern16
```

引用它的是 **libarchive 自己**：`archive_read_support_format_7zip.o`。zstd 与 xz 恰好
不触发那个 idiom，所以"量它们"回答的是另一个问题——
[[a-check-that-picks-its-object-by-convention]]。

**三种可能里，第 3 种是对的，而它当时被排除了。** A/B，用 build.ninja 里那条**真实的
编译命令**，只改 flag 这一维：

| flag | `memset_pattern16` 引用 | 目标文件 |
|---|---|---|
| 如实际构建（带 `-fno-builtin-memset_pattern16`） | **1** | 38200 |
| **把那个 flag 去掉** | **1** | — |
| `-fno-builtin` | 0 | 38888（+1.8%） |
| `-ffreestanding` | 0 | — |
| `-mllvm -disable-loop-idiom-memset` | 0 | 38152 |

**加与不加，引用数都是 1——那个 flag 什么都没做。**

**而且它不会报。** clang 对 `-fno-builtin-<name>` 不给任何反馈：

```
-fno-builtin-totally_not_a_function        accepted silently
```

`memset_pattern16` 是 LLVM TLI 的 libfunc 而不是 clang 的 builtin，所以这个 token
**看起来在做事，实际是个静默的空操作**。预处理后的源码里该符号出现 **0 次**，确认它确实
是优化器生成的。

**缺口是引擎侧的，而且是成对的：**

1. `builtins = "iso"` 发的 token 在它唯一点名的那个 case 上无效；
2. **那个 token 没有任何核对。** 同一个文件里 `-D`/`-U` 有探针核对
   `expectDefined` / `expectUndefined`，注释明写「一个没生效的 `-D` 是校验失败，不是
   沉默」——而 `builtinsTokens` 走的是另一条路，没有对应的 `expect*`。

**判据**：`builtinsTokens` 要像 `-D` 一样被探针核对。没有这一条，换成任何一个新 token
都可能重复这次静默——**一个只在「它起作用」这一假设下成立的机制，需要一条断言它起作用的
判据。**

**这个符号属于哪一层，查过了，而答案把问题从「bug 修法」变成「架构选择」。**

它不是程序调用的函数，是**编译器发出的辅助调用**——和 `memcpy`、`__udivti3` 同类。那一
类归**编译器运行时**。但实测：

```
grep -rln memset_pattern16  openkal-llvm-runtime/llvm/   →  0 处
```

**整个 LLVM 树里没有它的实现，compiler-rt 不带兜底。** 上游 LLVM 发出的是一个只有
Apple 的 libSystem 提供的调用。

于是 `builtins = "iso"` 遇到一个它**声明得了、强制不了**的情形：能强制它的那个 flag 不
工作，而 LLVM 没有提供第二个。四个候选，代价都已量：

| 候选 | 有效 | 代价 | 性质 |
|---|---|---|---|
| `-fno-builtin` | 是 | +1.8%（本翻译单元）；**连 ISO 函数的优化一起关掉** | 标准用户面，正确但过宽 |
| `-mllvm -disable-loop-idiom-memset` | 是 | −0.1% | **不是稳定接口**，LLVM 升级可能失效 |
| 在 openkal-musl 的 Apple 端口提供该符号 | 未验 | 约六行（`port/src/mach/` 已存在） | **与 `builtins = "iso"` 的字面意图相反** |
| 不动，把该成员声明为按构造失败 | — | 丢掉一个真实可用的组合 | 最弱 |

**第三个候选值得认真看**，因为它把问题问对了：`builtins = "iso"` 说的是「这个 C 库呈现
什么」，而编译器发出的辅助调用**不是程序请求的接口**，它更接近 ABI。一个声明自己只呈现
ISO C 的 C 库，仍然可能需要供给代码生成器为那个目标假定的辅助函数——正如它供给
`memcpy` 一样。

**这是拍板项，不是实现项。** 它决定 `builtins = "iso"` 到底是「关掉生成器的假定」还是
「声明程序可见的接口面」——两者在别的情形下也会分叉。

### 2.3 实现侧（openkal-*）

#### C4' — 「不复现」是在另一个版本上量的（2026-09-21 更正）

本文把 C4（`memset_pattern16`）记成「已通过，本机实测；关闭；不复现」。**那次测量用的是
较新的 runtime**，而 `lsp-mcpp-private` 的 CI 钉在 `openkal-llvm-runtime@0.10.0` 上，它
今天仍然红：

```
ld64.lld: error: undefined symbol: memset_pattern16
test_archive ... FAIL (compile, 0.83s)
```

所以正确的陈述是**「被某个 runtime 版本修掉了」**，不是「不复现」。两者对一个还钉在旧版本
的消费者来说结果完全不同——前者给出可执行的动作（升版本），后者让他去找一个不存在的
偶发因素。**一个缺陷的「不复现」必须带上它是在哪个版本上不复现的。**

同一次 CI 还挖出一条更硬的：那个钉是 `mcpp 2026.9.17.3`，而索引的 `min_mcpp` 是
`2026.9.18.3`——**钉在下限之下**：

```
error: index requires mcpp >= 2026.9.18.3 but this is mcpp 2026.9.17.3 [E0006]
```

整个索引对那个客户端是关着的。它的每一条读数都不是关于这一波的，而是关于「打不开索引」的。
**一个落后于引擎的 CI 钉不是保守选择，是在测别的东西**；落到下限之下时，连那个「别的东西」
也不是了。

#### I1 — P4：合成节点层

**状态**：未实现。设计 §5.4。

写得完全正确的 POSIX 代码——`open("/dev/urandom")`、`fopen("/dev/null","w")`、
`/dev/fd/N`、`/tmp`——在 openkal-Windows 上会坏，**而没有任何一层负责**。这是唯一一类
「代码本来就对」的失败。

**它不是新层**：`openkal-musl` 的 `port/src/okm_fd.c` 已经造了两样同源的东西（描述符表、
单一全局根的名字解析）。合成节点是第三样。

**前提已实测（2026-09-21）**，而且比本文初稿写的更锋利——不是「会坏」，是三条全坏。
探针声明 `openkal-llvm-runtime = "0.14.0"`，同一份源码两个目标：

| 判据 | `x86_64-linux-gnu` | `x86_64-windows-gnu`（wine） |
|---|---|---|
| `open("/dev/urandom", O_RDONLY)` + 读 8 字节 | ok | **FAIL** |
| `fopen("/dev/null", "w")` | ok | **FAIL** |
| `fopen("/tmp/…", "w")` | ok | **FAIL** |
| 合计 | `failures: 0` | `failures: 3` |

**插入点是唯一的，而且不在 open 里。** `port/src/okm_fd.c:414` 的 `okm_resolve()` 是
**所有**路径型 syscall 的收口（`okm_syscall.c` 里六个以上调用点经过它）。只在 open 里认
合成节点，`stat("/dev/null")` 仍然会失败，而 POSIX 程序确实会 stat 它。

**`/tmp` 与前四个不是一回事。** 前四个是字符设备，`/tmp` 是目录映射；它更可能属于已有的
预打开目录那一套（`okm_preopen`）。混进同一张表会让这张表同时承担两种语义。

**判据**：节点集合**写死**，不迭代扩张；conformance 逐条断言；集合外的路径行为不变——
这一条要有**反向腿**：`/dev/sda`、`/dev/fd/999`、`/proc/self/maps` 必须仍然按原样失败，
否则这张表就成了吞掉所有 `/dev` 的黑洞。另：`openkal.random` 是可选接口（`kal_random_fill`
在 `okm_syscall.c` 里是弱引用），所以 `/dev/urandom` 在不提供它的实现上必须**仍然失败**。

#### I2 — P6：`openkal-win-ucrt` 形态

**状态**：未实现。设计 §5.6。已验证**引擎侧改动数为 0**。

**判据（已通过，2026-09-21 实测）**：一个最小的 `presents = "windows"` C 库
（`data-model = "llp64"`、`wchar = 16`）加一个消费者，为 `x86_64-windows-gnu` 构建，
**引擎零改动**：

```
c-abi             ucrt           (fake-win-ucrt@0.1.0, graph)
Finished dev
```

消费者的一个翻译单元里三条断言全过：`_WIN32` **在**（呈现 windows 就是保留平台自己的
宏）、`__unix__` **不在**、`__MCPP_TARGET_WINDOWS__` **在**（它答的是目标，不是环境）。

**所以 I2 剩下的全部是包本身**（一个真的基于 UCRT 的 C 库），不是引擎。顺带一条读数：
gcc 路线会 warn「宿主 C 库头仍会被搜索」并给出 `llvm@<version>` 的 hint，这正是
host 依赖规则在这里的表现。

#### I3 — 生态改用 `x86_64-windows-musl`（新增，2026-09-21）

**不是新增一个目标——它已经存在**（`triple.cppm`，tier `preview`，PE，pin
`llvm@22.1.8`）。缺的是两件已经到期的事。

**一、tier 可以晋级，而判据由那一行自己写下：**

> `verified` 在这张表里意味着产物**被构建并被运行**过。在 Linux 宿主上跑 PE 需要 wine，
> 而 openkal 的 CI 有那一步——所以这是可测的，tier 在**被测量之后**移动。

本机 + wine 实测（`openkal-llvm-runtime@0.14.0`，mcpp 2026.9.21.2）：

```
PE32+ executable (console) x86-64, for MS Windows, 18 sections
ok windows-musl, long=8, wchar=4          exit=0
```

`_WIN32` 不在、`long=8` 即 **LP64**（不是 Windows 的 LLP64）、`wchar=4`、输出目录
`target/x86_64-windows-musl/`。**但晋级要由 CI 给读数**，那一行说的是「openkal 的 CI
有那一步」——所以次序是：先把生态 CI 改到这个目标，读数由 CI 产出，再动那一行。

**二、生态还在用 `-gnu`，而那正是这一行要修的毛病：**

| | mcpp 的身份 | 交给 clang 的 | 真实 c-abi |
|---|---|---|---|
| 今天 | `x86_64-windows-gnu` | `x86_64-w64-windows-gnu` | **musl** |
| 应该 | `x86_64-windows-musl` | `x86_64-w64-windows-gnu` | musl |

两行交给 clang 的字符串**相同**，mcpp 的身份**不同**。用 `-gnu` 把「这个 C 库偏偏不是的
那个东西」写进了身份、输出目录、`cfg(env = …)` 与打包的 ABI 标签。

**描述符侧的风险面实测为零**：`grep -rn 'env = "gnu"' pkgs/` 命中 0 处；提到
`x86_64-windows-gnu` 的三个描述符全是注释。其中 `openkal-uefi.lua` 那条是要点——UEFI
消费方是 PE 目标的**另一种**用法，它继续用 `-gnu` 是对的。**retarget 的范围是
openkal-musl 那一栈，不是所有 PE 用户。**

**单独一轮，不并进 2026.9.21.2**：改测量的目标会改 30 成员测量的**分母**。本轮自己的
判据：同一个成员集合在两个目标上各测一次，逐格比对，任何一格从 `runs`/`builds` 退到
`fails` 都要点名。

### 2.4 生态数据侧

#### D1 — libarchive 这条路（`lsp-mcpp-private` 的打包）

`compat.libarchive` 无条件依赖 zlib/bzip2/lz4/zstd/xz，配置头写死 `HAVE_LIBLZMA 1`，
没有可关的 feature。后果：Windows 编不过（xz → `windows.h`，**E1 覆盖**），macOS
release 链接不过（`memset_pattern16`，**C4 覆盖**）。

**所以这条路的两个阻塞分别由 E1 与 C4 关闭，不需要单独决策。** 只有在 E1+C4 落地后
重测仍红时，才需要在「上游推一个不带编码器的 feature / 只保留解包 / 分平台走两条路」
之间拍板。**先测再拍板，不要先拍板。**

---

## 3. 为什么用 `lsp-mcpp-private` 验收

三条性质使它成为**比任何合成夹具都强**的载体。

**（一）它是真实的、非玩具的。** `mcpp-language-server`，C++23 模块，二十余个模块，
六个可执行目标（服务器、conformance、lspgen、devtools、两个 mock）。合成夹具只能证明
「引擎在我构造的那一小格里对」；它证明「一个有人用的程序能不能过」。

**（二）它的平台面已经被压到可数。** `modules/os/{linux,macos,windows}/src/os.cppm`
各 20 行，导出同一个 `mcppls.os` 接口，只有六个常量：

```cpp
Family FAMILY; string_view FAMILY_NAME; string_view EXECUTABLE_SUFFIX;
char PATH_LIST_SEPARATOR; string_view VSCODE_TARGET; bool CASE_INSENSITIVE_PATHS;
```

**这六个都不是 POSIX 设施**，是打包与命名事实（可执行后缀、PATH 分隔符、VS Code 目标
三元组、路径大小写）。它们**本来就该由目标回答**，不归 openkal。

由此得到一个**极其锋利的判据**：**除这六个常量外，任何新出现的平台分支都是一处被点名
的 openkal 缺口。** 分母固定，信号无噪声。

**（三）它几乎不直接碰 C。** 实测：`src/` 与 `modules/` 下**没有任何直接 POSIX 调用**
（`select(` / `pipe(` 的命中全是它自己的同名函数），唯一的 C 头是
`archive.h` / `archive_entry.h`。

于是它的失败面**只有两处**：C++ 运行时（libc++/libc++abi/libunwind over musl over
openkal），与 libarchive 的传递 C 依赖。**一个失败落在哪一侧，一眼可辨。**

> 反面：正因为它不直接碰 C，它**测不到** C 库的大部分表面。所以它是
> **整合验收**，不替代 30 成员测量（那个才覆盖 C 表面）。两者分工，不重叠。

---

## 4. 验收判据

### 4.1 三级，逐级收紧

| 级 | 判据 | 目标 |
| --- | --- | --- |
| **A1 构建** | `mcpp build` 通过 | `x86_64-linux-gnu`、`x86_64-windows-gnu`、`aarch64-macos` |
| **A2 运行** | `mcpp test` 全部通过；`mcppls --version` 在目标上真的跑 | 同上（非本机经 runner） |
| **A3 形状** | `modules/os/*/src/os.cppm` 仍**只有那六个常量**，且三份文件的 `diff` 仅这六行 | 全目标 |

**A3 是本方案的核心判据。** A1/A2 回答「能不能跑」，A3 回答「**为了能跑，付出了几处
平台分支**」——那才是 openkal 的命题。一次 A1 绿而 A3 红的验收，等于用分支换通过，
结论与不做无异。

### 4.2 机械化

`modules/os` 的三份文件逐行比对可以写成脚本，进 `lsp-mcpp-private` 的 CI：

```
三份 os.cppm 除 FAMILY / FAMILY_NAME / EXECUTABLE_SUFFIX /
PATH_LIST_SEPARATOR / VSCODE_TARGET / CASE_INSENSITIVE_PATHS
六行外必须逐字节相同；出现第七处差异即红，并打印那一行。
```

**这条断言的价值在于它会因为「有人加了一处分支」而红，而不是因为「构建坏了」而红。**
前者是缓慢的、无人察觉的退化，正是它抓的。

### 4.3 与 30 成员测量的分工

| | 覆盖 | 不覆盖 |
| --- | --- | --- |
| 30 成员测量 | C 库表面（uapi、libc 扩展、`__CYGWIN__` 类分支） | 真实程序的整合、C++ 运行时深处 |
| `lsp-mcpp-private` | C++ 运行时、模块图、三目标一份源码 | 大部分 C 表面 |

**两者都绿才叫生态闭环。** 任一单独绿都不足以下结论。

### 4.4 两者的**目标集合**也不重叠，而这一条本文原先没写（2026-09-21 补）

分工写成了「30 成员测 C 表面、lsp 测整合」，**没有写它们问的目标不同**：

| | 目标 |
|---|---|
| 30 成员测量（`pins.toml`） | `x86_64-linux-gnu`、`x86_64-windows-gnu` —— **没有 macOS** |
| `lsp-mcpp-private` | 三个目标，**含 `aarch64-macos`** |

**后果是这一轮实测到的**：`archive` 在 30 成员测量里两列都是 `runs (posix)`，而同一个 libarchive
在 `lsp-mcpp-private` 的 macOS 上链接失败于 `memset_pattern16`。两个读数都是真的——
`memset_pattern16` 是 Apple 的 libc 函数，clang 只在 Apple 目标上生成它，linux 与
windows-gnu 上**按构造不会出现**。

**所以「openkal 生态里按 30 个成员规模扫 macOS 的东西，不存在」。** macOS 只被
`lsp-mcpp-private` 一个程序、以及各实现包自己的 CI 覆盖。

**这也限定了 E1 判据的适用范围**：「总失败 10→5、新增为零」是**在那两列上**成立的；
macOS 那一列从来没进过这个统计，所以这一类缺陷在那个数字里按构造不可见。

**要补这一列，先有一个决定要拍：** Linux 宿主能为 `aarch64-macos` **交叉构建**但**跑不了**，
所以那一列最多到 `builds`，不是 `runs`。而 `RANK` 里 `builds` 低于 `runs`——
**一个只能到 `builds` 的目标列，会让「有没有退步」这个判据在那一列上永远比另外两列松。**
是接受这个不对称、还是给那一列配 runner，是拍板项，不是实现项。

---

## 5. 执行顺序

依赖关系是真实的，不可交换。

```
E1 (P3, 撤 __CYGWIN__)
  ├─ 解锁 30 成员测量的 4 个失败
  └─ 解锁 lsp-mcpp-private 的 Windows 目标（经 xz）
       │
C4 (memset_pattern16) —— 已关闭，本机实测不复现
  └─ 不是阻塞
       │
       └──> A1/A2/A3 三目标验收   <── 本方案的终点
                 │
C1 (__cxa_thread_atexit) ──┘  （不阻塞 lsp-mcpp-private：它不用 thread_local 析构；
                                 阻塞的是 spdlog/doctest 那类消费者）

E2 (P7-L3)、E3、I1 (P4)、I2 (P6)、C2、C3 —— 与上面无依赖，可并行，不阻塞验收
```

**建议批次**：

| 批 | 内容 | 为什么在一起 |
| --- | --- | --- |
| **第一批** | E1 + 30 成员重测 | 代价最小、收益最大（4/10），且是后续的前置读数 |
| **第二批** | C4 + C3 + C2 归类 | 都是 C 库侧的符号/归类问题，一次 openkal-musl 发布带走 |
| **第三批** | `lsp-mcpp-private` A1/A2/A3 三目标验收 + A3 断言进 CI | 前两批的收敛点 |
| **第四批** | E2 (P7-L3)、E3 | 引擎侧新能力，与验收无依赖 |
| **第五批** | I1 (P4)、I2 (P6)、C1 第二层 | 独立工作量；C1 含唯一的未知数 |

---

## 6. 发布前的交叉验证协议

引擎是通用件，而生态是它唯一的真实负载。**mcpp 的 PR 在合入之前，应当由生态仓库拉着
那个 PR 分支跑一遍；两侧 CI 同时绿，才构成合入的依据。**

### 6.1 这条协议要防的事已经发生过

2026-09-20 发了 2026.9.20.1；不到一天，E1（撤 `__CYGWIN__`）又要一次
2026.9.21.1。E1 的依据——四个成员因 `__CYGWIN__` 停在 `windows.h`——**在 2026.9.20.1
合入之前就已经可以测出来**：那份 30 成员测量用的是已发布引擎，但同一批成员完全可以
用 PR 分支构建的引擎先跑一遍。

**一次本可以合并的发布，变成了两次。** 每一次发布都要走完 tag、四平台构建、两端镜像、
逐资产 GET 核验、xim-pkgindex bump、索引 artifact 发布、消费方确认——协议省下的是
这整条链，不是一次构建。

### 6.2 机制已经在那里

生态各仓的 CI 都认 `MCPP_SOURCE_REF`：非空时从 mcpp 的那个 ref 现场构建引擎，并断言
PATH 上的 `mcpp` 就是构建出来的那一个（"the engine on PATH is the one under review"）。

| 仓库 | `MCPP_SOURCE_REF` |
| --- | --- |
| openkal, openkal-linux, openkal-windows, openkal-macos, openkal-musl, openkal-llvm-runtime | 已有 |
| **mcpp-index** | **没有** |
| **lsp-mcpp-private** | **没有** |

后两个恰是最该有的：mcpp-index 承载 30 成员测量（引擎改动的最大负载面），
lsp-mcpp-private 是本方案的验收载体。**补这两处，是本协议唯一的一次性工作量。**

### 6.3 顺序

```
1. mcpp 开 PR，自身 CI 绿
2. 在每个相关生态仓库开一个临时 PR（或 workflow_dispatch），
   MCPP_SOURCE_REF = mcpp 的那个 PR 分支
3. 两侧 CI 同时绿
4. 生态级全局 review（见 6.4）
5. 合入 mcpp PR，发布
6. 撤掉临时 PR 里的 MCPP_SOURCE_REF 覆盖，换成已发布的版本钉
```

第 6 步不可省。`MCPP_SOURCE_REF` 是**开发形态**；留在合入的分支里，生态仓库就永远在
验证一个未发布的引擎，而它自己的版本钉从此不被任何东西检查。

### 6.4 合入前的生态级 review

跨仓库 review 要回答的不是"这段代码对不对"，而是：

| 问 | 为什么 |
| --- | --- |
| 这次改动改变了哪些包的**命令行**？ | 引擎改动的影响面按构造是全图的 |
| 有没有包的**清单**因此需要改？ | 需要改，就意味着这不是一次纯引擎发布 |
| 索引 `min_mcpp` 要不要动？ | 动它会让停在下限的客户端**整个索引打不开** |
| 有没有**消费者**被钉在旧版本上而看不到这次改动？ | 精确钉是常态，改动不会自动到达 |
| 生态 CI 的绿，是**替换过工作树**的绿吗？ | 是的话，它对"已发布形态"零信息量 |

### 6.5 这条协议**不覆盖**什么

生态仓库的 CI 大多把整张图换成兄弟仓库的**工作树**（`tools/branch-graph.sh`、
`tools/working-trees.sh`）。所以它**按构造看不见**"某个版本还没注册进索引"这类缺口——
2026-09-20 就是这样红了一次 mcpp 的 `openkal-cross`：openkal-llvm-runtime 0.13.0 钉了
openkal-musl 0.18.0，而 0.18.0 当时还没进索引，消费者自己的 CI 全绿。

**交叉验证回答"引擎与生态的代码合不合得上"，不回答"发布物到不到得了"。** 后者只有
两个判据：一个不做工作树替换的第三方（mcpp 的 `openkal-cross` 就是），与沙箱里对已
发布物的验证。两者都不能被本协议替代。

---

## 7. 判据总表

状态截至 2026-09-21 收尾。

| 编号 | 判据 | 怎么算通过 | 状态 |
| --- | --- | --- | --- |
| E1 | 30 成员重测 | `windows.h` 组 4→0 且总失败不增 | **一半达成，而未达成的那半否掉了判据自己的分母**：总失败 10→5、新增失败为零；但该组实际只有两个成员读 `__CYGWIN__`，见 §2.1 E1d |
| E1b | 大写重命名 | 上一版红、本版绿 | **已通过**：验证脚本 B 段，`2026.9.21.1` fails=2 → `2026.9.21.2` fails=0 |
| E1c | 交叉验证协议的反向 | `openkal_ref` 留空时行为不变 | **已落地** |
| E2 | 引用未提供接口的符号 | 链接期被点名拒绝；声明正确时零开销 | **退出本轮**：阻塞是设计决定不是工作量，见 §2.1 |
| E3 | 提供方什么都不声明 | 构建成功**且**报告里有 unchecked 一行 | **已通过**：e2e 743 四条腿 + 验证脚本 C 段双向 |
| C1 | 最小复现 | 链接通过**且析构真的执行** | **已通过**：三层全部定位并修复，`openkal-llvm-runtime@0.15.0` |
| C2 | cmp-module | 从分母移除，理由可追溯，**且声明可被证伪** | **已通过**：`[not-portable]` + `compat.py check`，红/绿两向实测 |
| C2' | curl | 配方缺陷，不是不可移植 | **已定位未修**：两个目标两个不同真因，见 `docs/openkal-compat.md` |
| C3 | `arc4random_buf` | 「应当有的符号」CI 断言 | 已通过 |
| C4 | `aarch64-macos --profile release` | 本机实测 | **更正**：不是「不复现」，是被某个 runtime 版本修掉了——旧钉上仍在，见下 |
| I1 | 写死的节点集合 | conformance 逐条断言 | 未实现（第五批） |
| I2 | `presents = "windows"` 的 C 库 | 引擎改动数为 0 | 未实现（第五批） |
| **A1** | 三目标 `mcpp build` | 全绿 | **待跑**（须用已发布钉，见 §8 第二条） |
| **A2** | 三目标 `mcpp test` + 真跑 | 全绿 | 待跑 |
| **A3** | 三份 `os.cppm` | **除六行外逐字节相同** | **已机械化并进 CI**；四种失败形态逐个量红过 |

---

## 8. 本方案自身的失败模式

写下来，因为它们在本轮各出现过一次。

**（一）「绿了但布置没成立」。** A1 可以因为有人加了一处 `#ifdef` 而绿。**A3 存在就是
为了这个**，它必须与 A1 同时断言，不能事后补。

**（二）「判据取自会替换工作树的 CI」。** `lsp-mcpp-private` 若用 path 覆盖或分支克隆
解析 openkal，它的绿对「已发布形态」零信息量。**验收必须走已发布的版本钉**，
且先 `mcpp index update` 再构建。

**（三）「注册与动 pin 同一个 PR」。** `lsp-mcpp-private` 的
`openkal-llvm-runtime = "0.12.0"` 要移到 `0.13.0`；0.13.0 已注册并发布（2026-09-20），
所以这一步现在安全。**以后每次都要先确认。**

**（四）「拿一句记录当判据」。** E1 那条就是：「xz 不适配」是一句关于**解码**的记录，
被当成了关于**编译**的结论。**读到「上游不打算修」时，先把那段源码打开。**

---

## 9. 一句话

本轮把「能力何时被回答」这件事从预处理期移到了解析期，并让三个实现都说出自己提供什么。
**剩下的工作里，代价最小、收益最大的一条是撤掉一个我们自己借来的名字**；而验收该由一个
真实程序给出，判据不是「它能不能构建」，是「为了构建，它还剩几处平台分支」。
