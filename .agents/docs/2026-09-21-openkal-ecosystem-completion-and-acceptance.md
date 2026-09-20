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

**状态**：**已实现**（mcpp 2026.9.21.1）。下文保留论证；落地形态见本节末。

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

**落地形态（2026.9.21.1）**：`cenv.cppm` 的 Windows+Posix 分支加两个 `-U` token，
`expectUndefined` 加两项——**后者才是承重的**：探针会把实现出的配置的预定义与这两张表
比对，一个没生效的 `-U` 是一次校验失败，不是一次沉默。

直接量过一次，用钉住的 clang、按 mcpp 实际发出的 token 顺序：

```
echo | clang -dM -E -x c - -U__CYGWIN__ -U__CYGWIN32__ \
         --target=x86_64-pc-cygwin -U__CYGWIN__ -U__CYGWIN32__
  -> __unix__ 定义；__CYGWIN__ 消失；_WIN32 仍不存在
```

**`.S` 行上这对 token 出现两次**，`.c`/`.cpp` 各一次。`cEnvTokens` 会被加进包的
asmflags，而形如 `-D`/`-U`/`-I` 的 token 同时经由「把 define 带进汇编」的通道到达；
`--target=` 与 `-fno-short-wchar` 不是那个形状，所以只出现一次。`-U X` 两次等于一次，
且真正的判据是探针，所以 e2e 不对次数作断言——那会把 flag 管线的实现细节钉死，而不是
钉住被测性质。

#### E2 — P7-L3：链接期集合差

**状态**：未实现。设计称其为「本轮与设计最大的偏差」。

`requires-interfaces` 今天只在**解析期**被比对，而解析期比的是**声明**。声明由产物
派生（L1 已保证），但**没有任何东西在链接期检查程序实际引用的符号是否落在声明的接口
集合内**。

**判据**：一个程序引用了实现未提供的接口里的符号，链接期被点名拒绝，且**声明正确时
零额外链接开销**。

**阻塞**：需要把 `SURFACE.txt` 的「接口 → 符号」映射带到链接期。材料齐（`SURFACE.txt`、
`kal_interfaces()`），是工作量不是未知数。

#### E3 — 未被回答的 requirement 没有提示

**状态**：未实现。记录：记忆 `an-unanswered-requirement-looks-like-a-confirmed-one`。

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

**状态**：第一层可修但**不能只修第一层**；第二层未定位。记录：
`.agents/docs/2026-09-20-cxa-thread-atexit-finding.md`。

只补符号会把一个**构建期的响亮失败**换成一个**运行期的静默失败**：链接过了，
`thread_local` 的析构不跑。补丁试过并**主动回退**，因为验证显示析构确实没执行。

**判据**：最小复现（五行，文档里有）在 `x86_64-windows-gnu` 上**链接通过且析构函数
真的执行**——两个条件缺一不可。只断言链接通过是错的判据。

**阻塞**：第二层未定位（emutls 在 PE 上的注册路径）。这是本清单里**唯一一个真正的
未知数**。

#### C2 — `linux/` uapi 头（curl, cmp-module）

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

#### C4 — `memset_pattern16`（macOS release）

Darwin libc 扩展。`-O2` 下编译器把填充循环换成它，openkal-musl 没有。

**注意**：这不是「程序调用了它」，是**编译器合成的调用**。所以它必须由 C 库提供，
程序侧无法规避。

**判据**：`aarch64-macos` 的 release 构建链接通过。这条是 `lsp-mcpp-private` macOS
验收的**硬阻塞**。

### 2.3 实现侧（openkal-*）

#### I1 — P4：合成节点层

**状态**：未实现。设计 §5.4。

写得完全正确的 POSIX 代码——`open("/dev/urandom")`、`fopen("/dev/null","w")`、
`/dev/fd/N`、`/tmp`——在 openkal-Windows 上会坏，**而没有任何一层负责**。这是唯一一类
「代码本来就对」的失败。

**它不是新层**：`openkal-musl` 的 `port/src/okm_fd.c` 已经造了两样同源的东西（描述符表、
单一全局根的名字解析）。合成节点是第三样。

**判据**：节点集合**写死**，不迭代扩张；conformance 逐条断言；集合外的路径行为不变。

#### I2 — P6：`openkal-win-ucrt` 形态

**状态**：未实现。设计 §5.6。已验证**引擎侧改动数为 0**。

**判据**：一个 `presents = "windows"` 的 C 库在同一引擎下构建，引擎不认识任何新名字。

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

---

## 5. 执行顺序

依赖关系是真实的，不可交换。

```
E1 (P3, 撤 __CYGWIN__)
  ├─ 解锁 30 成员测量的 4 个失败
  └─ 解锁 lsp-mcpp-private 的 Windows 目标（经 xz）
       │
C4 (memset_pattern16)
  └─ 解锁 lsp-mcpp-private 的 macOS release
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

| 编号 | 判据 | 怎么算通过 | 阻塞 |
| --- | --- | --- | --- |
| E1 | 30 成员重测 | `windows.h` 组 4→0 且总失败不增 | 无 |
| E2 | 引用未提供接口的符号 | 链接期被点名拒绝；声明正确时零开销 | 工作量 |
| E3 | 提供方什么都不声明 | 构建成功**且**报告里有 unchecked 一行 | 无 |
| C1 | 五行最小复现 | 链接通过**且析构真的执行** | **第二层未定位** |
| C2 | curl / cmp-module | 记为 `refused` 而非 `fails` | 无 |
| C3 | `arc4random_buf` | 「应当有的符号」CI 断言 | 无 |
| C4 | `aarch64-macos --profile release` | 链接通过 | 无 |
| I1 | 写死的节点集合 | conformance 逐条断言 | 无 |
| I2 | `presents = "windows"` 的 C 库 | 引擎改动数为 0 | 无 |
| **A1** | 三目标 `mcpp build` | 全绿 | E1, C4 |
| **A2** | 三目标 `mcpp test` + 真跑 | 全绿 | A1 |
| **A3** | 三份 `os.cppm` | **除六行外逐字节相同** | A1 |

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
