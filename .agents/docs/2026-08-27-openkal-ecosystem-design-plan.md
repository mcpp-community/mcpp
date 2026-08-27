# openkal 生态:接口判据、面的补全,与端口层的组合

2026-08-27 · 设计方案 + 分批清单(**待 review,尚未实施**)

配套文档:[`2026-08-27-openkal-native-path-three-issues.md`](2026-08-27-openkal-native-path-three-issues.md)
—— 那份是**三个 issue 的缺陷分析与 mcpp 引擎侧的修复**;本份是 **openkal 及其生态
仓库的设计/优化升级**,两份不重复。

---

## 0. 一句话

> **openkal 提供面向内核的通用原子能力,组合由端口层做一次,既有生态继承它。**
>
> 于是「openkal 该不该有 X」这个问题,永远先问「X 能不能由已有的原子能力组合出来」,
> 而不是问「有没有程序需要 X」。

本文按这条把 27 条运行期失败重排,得到:**四个接口该加**(两小、一中、一大)、
**七条落在端口层**(其中可复用的那几条归 §2.7 的 `openkal-kit`)、**两条是应用自己的事**。

⚠️ 第四个(`openkal.space`,§2.4)是**复查掉两条错误理由之后**才进来的 —— 见 §1.3,
那两条错误本身比结论更值得读。

---

## 1. 判据:三次替换,最后一条是规范自带的

⚠️ **前两条都判错过,记录在此,因为犯错的方式值得记。**

| # | 判据 | 判错了什么 |
|---|---|---|
| 1 | 「是不是通用内核能力」 | ⚠️ 要人判断。**漏掉了就绪与终端**;把 `net` 判成「等第二个消费者」 |
| 2 | 「传统体系下已可移植的程序用到的,必须有」 | ⚠️ 好用的**筛**,但把 `poll` 误判进来 —— 可移植程序用它是因为 POSIX 给了它,不是因为它不可约 |
| 3 | **clause 10 基数 + 原子性 + 端口层组合** | ⭐⭐ **规范自带,且裁决了前两条的冲突** |

### 1.1 最终判据(三问,按顺序)

**问一:一个程序里有几个?**(SPEC clause 10 的基数表)

| | 一个程序里几个实现 | 语法上表现为 | 契约形式 |
|---|---|---|---|
| openarch | 一个,即处理器 | —— | concepts |
| **openkal** | **每个接口一个** | **「我的 X」**:我的标准输出、我的进程、我的时钟、我的网络栈 | **C ABI** |
| openhal | 多个,即那些设备 | **「哪一个 X」**:哪块声卡、哪个摄像头、哪个 UART | concepts |

⭐ **判别一个能力归谁,只需问它的名字里是「我的」还是「哪一个」。**
这条与 backend 是 MMIO 驱动还是 ALSA 封装**完全无关** —— 这正是「不关心具体
backend 实现」的意思。

**问二:能不能由已经命名的原子能力组合出来?**
能 ⇒ 它是**端口层**的事,不是 openkal 的。
判据在仓库里已有一句话形态(`okm_syscall.c:836`,解释 `openkal.random` 为何必须存在):

> **Entropy is not derivable from the other interfaces** … Neither bypassing the
> layer nor inventing entropy was acceptable, so the layer gained an interface.

**问三:它的形状是不是某个 OS 的形状?**
是 ⇒ 拆到原子为止。`socket`+`setsockopt`+`sockaddr` 是 POSIX 的形状;
「连到一个端点,得到一个流」才是原子。

### 1.2 ⚠️ 一处我说过头的,必须更正:`fork` 不是「硬天花板」

前几轮我写过「`fork` 永远不可能,是通用性的硬天花板」。**按判据二,这话是错的。**

clause 7.1 禁止的是 **`kal_fork` 这个操作**(复制地址空间与执行状态不能在每个环境上
忠实完成)。它**没有**禁止**地址空间的原子能力**:

```
创建一个地址空间 · 复制一个地址空间 · 在其中启动一个上下文
```

有了这三样,端口层**可以**把 `fork` 组合出来 —— Fuchsia 没有 `fork` 却有
`zx_process_create`/`zx_vmar_map`/`zx_thread_start`,seL4 亦然。

⚠️ **本节初稿把第三样写成「以给定寄存器状态启动」,那是错的** —— 那会逼 openkal
认识 `arch::context`,而 clause 10 不允许(C ABI ↔ concepts)。正确的形状与解法
见 §2.4。

按三问过一遍:

| 问 | 答 |
|---|---|
| 几个? | 一个地址空间管理器 ⇒ **openkal** ✔ |
| 可导出? | ❌ 只有环境能造地址空间 ✔ |
| OS 的形状? | ❌ Fuchsia/seL4/Linux 三种内核都有这个形状,**而 `fork` 才是被绑定的那一个** ✔ |

⇒ ⭐ **它通过,而且它比 `openkal.net` 更「内核」。⇒ 列入本方案,见 §2.4。**

### 1.3 ⚠️ 我为「不做」给过三条理由,两条是错的

初稿把它记成「候选但不在本批」,理由三条。逐条复查:

| # | 理由 | 复查 |
|---|---|---|
| 1 | 「面上最大的一次扩张」 | ❌ **算错了** —— 分解对了是 **5 个操作**,比 `openkal.net` 的 6 个还少(§2.4) |
| 2 | 「无 MMU 目标必然缺席」 | ❌ **这根本不是反对理由,它就是 openkal 的设计** —— clause 6.1:实现不提供的接口以**链接期缺席**表达。`openkal.fs` 在 opensbi 上就是缺席的。**由后端选择与实现,正是可组合设计的意思** |
| 3 | 「今天没有消费者」 | ⚠️ 这是**排期**理由,不是设计理由。⭐ 而且它反过来成立:见下 |

⭐⭐ **理由 3 反过来是「现在就要设计」的论据:`kal_process_spawn` 与地址空间原子能力
在语义上重叠。** 前者接一个**路径**,由环境完成加载;后者让程序自己造进程。
两者的关系 —— spawn 是原子之上的复合,还是一个独立接口 —— **必须一次决定**,
否则将来会出现「造一个进程有两条路」,而 clause 8 不允许事后改。

⇒ **设计现在做,实现按排期。** 这与「简洁」不冲突:简洁是**面的大小**,不是
**决定的推迟**。

---

## 2. openkal(规范包)—— 四个接口,一条记录

⚠️ **§2.8 是对本章的深度 review**(推翻了 §2.1/§2.3/§2.4 的部分内容);
**§2.9 是完整性验收** —— 面够不够用,以及唯一一个真实的原子缺口;
**§2.10 用四问回扫**,推翻了 §2.8 ⓹ 的结论并删掉了 §2.8 ⓸。
**§2.11 是第二次 review**,采纳截止时间方案并补上它的真实代价。
**§2.12 是六个接口的头文件草案**,**§2.13 是规范/ABI 级 review(六处发现)**,
**§2.14 是 clause 6.3 / clause 11 的增补文本**。
⭐ **只读一节的话读 §2.13** —— 写草案时撞出来的问题都在那里。

⚠️ **每一条的形状都不是它在 POSIX 里的形状。这是本章的要点。**

### 2.1 `openkal.terminal`(小)—— 独立接口,不是往 stream 里加

**依据**:「raw 模式」在不同资源上表现不同(终端 vs 普通文件),正是 clause 6.4 的
形状 —— 它当时裁定 `seek` 因此属于 `openkal.fs`(资源是 descriptor)而**不**属于
`openkal.stream`。同理,终端控制属于一个资源为「交互式流」的接口。

**入口已经存在**:`KAL_STREAM_PROP_INTERACTIVE`。

**面(约 2 操作 + 1 询问)**:

```
kal_terminal_set_raw(stream, int on)     不要解释我的输入（行编辑）
kal_terminal_set_echo(stream, int on)    不要回显
kal_terminal_size(stream, &cols, &rows)  显示多大；不知道就说不知道
kal_terminal_props
```

**不是 `termios`** —— 那是一个带六十个标志位的 POSIX 结构。

**各环境**:Linux `tcsetattr`;Windows Console API;裸机 UART **没有行编辑** ⇒
「关掉它」是**无事可做即完成**(`okm_opt.h` 已有这类先例:释放一个不可能取得的句柄);
尺寸报「不知道」。

⚠️ **未决**:`set_raw` 与 `set_echo` 是两个操作还是一个「输入处理模式」?
POSIX 把它们放在一个结构里是历史,不是必然。**倾向两个** —— 它们可以独立变化。

### 2.2 `kal_process_channel`(小)—— `openkal.process` 的一次加法

**缺口**:`kal_spawn_streams` 已经收流句柄(`process.h:17`),缺的是**取得一个跨得过
spawn 边界的流**。

```
kal_process_channel(struct kal_stream* parent_end,
                    struct kal_stream* child_end)
```

⭐ **比「给我子进程的 stdout」更原子**:调用方自己决定把 `child_end` 装到子进程的
哪一个流上,于是 `popen("r")`、`popen("w")`、双向捕获、`2>&1` 都由端口层组合出来。

⚠️ **进程内的**流对**不属于这里** —— 可由缓冲 + `kal_task_wait/wake` 导出 ⇒ 是库。
这条存在的理由**只有**「跨地址空间」。

**依据**:clause 8 允许 *"A revision may add declarations"*。

### 2.3 `openkal.net`(大)—— 与 `openkal.fs` 同构

**依据三条,全部现成**:

- **clause 10**:NIC(openhal,多个)→ 网络栈(openkal,一个),与
  块设备 → `openkal.fs` **同构**。fs 已经在规范里,net 的理由与它逐字相同。
- **clause 3.4**:已经点名 `openkal.net`,并指出它与 fs 的差别正是 **half-closure**。
- **clause 3.4**:拒绝「解析无边界的名字方案」⇒ **DNS 不在里面**。

**面(约六个操作)**:

```
kal_net_connect(endpoint)        → stream      连到一个端点
kal_net_listen(endpoint)         → listener    接受入站
kal_net_accept(listener)         → stream
kal_net_shutdown(stream, dir)                  半关闭 —— clause 3.4 点名的那件事
kal_net_close_listener(listener)
kal_net_props
```

**不在里面**(每一条都是 POSIX 的形状而非原子能力):
地址族与 `sockaddr` 家族 · `setsockopt` 的选项空间 · 非阻塞标志(就绪归端口层) ·
`sendmsg/recvmsg` · **名字解析**。
⇒ endpoint 是**结构化的地址 + 端口**,不是字符串;`getaddrinfo` 是它之上的库。

⚠️⚠️ **两处必须动笔前定,不能边写边定(clause 8:接口发布后不可更改):**

1. **数据报要不要?** 流与数据报是两种资源(clause 6.4 的形状)。
   ⭐ **连锁后果是真的**:若 v1 只做流,DNS 只能走 TCP。这不是细节。
2. **谁来定形?** 按 [[second-instance-exposes-the-interface]],需要**两个形态不同的
   实现**才知道分解对不对 —— 一个宿主内核(openkal-linux over sockets)+ 一个
   BSP over openhal(lwIP)。⚠️ **同一个作者的两份实现等于零证据**
   ([[openkal-portable-program-findings]])。

### 2.4 `openkal.space`(中)—— 地址空间的原子能力

**为什么它是原子的而不是 `fork`**:clause 7.1 拒绝 `fork` 的理由是「复制地址空间**与
执行状态**不能在每个环境上忠实完成」。⭐ 拆开看,不能忠实完成的是**执行状态**那一半
(任意寄存器状态的恢复),而**地址空间**那一半在任何有 MMU 的环境上都是一次操作。

#### 面(七个操作)

```
kal_space_create(struct kal_space* out)                        一个空的空间
kal_space_clone(struct kal_space src, struct kal_space* out)   复制内存与句柄
kal_space_grant_dir(struct kal_space, struct kal_dir,
                    const char* name, kal_uintptr len)         ← kal_fs_preopen 的逆
kal_space_grant_stream(struct kal_space, kal_uintptr stream, int slot)
kal_space_start(struct kal_space, void (*entry)(void*), void* arg,
                void* stack_top, struct kal_task* out)         在其中启动一个上下文
kal_space_destroy(struct kal_space)
kal_space_props
```

⇒ **7 个操作**(含两个 grant),与 `openkal.net` 的 6 个同量级。
§1.3 那条「最大的一次扩张」仍然是算错了。

#### ⭐⭐ 关键:`start` 只收 (entry, arg, stack),**不收寄存器状态**

这是让它能被一份**可移植 C ABI**表达的那一步。

「以任意寄存器状态启动」需要 openkal 认识 `arch::context` —— 而 clause 10 规定
openkal 是 **C ABI**、openarch 是 **concepts**,二者不能互相引用。
Fuchsia 撞的是同一堵墙:`zx_thread_start(thread, entry, stack, arg1, arg2)` 只有
入口和栈,**所以 Fuchsia 上没有真 `fork`**。

⭐ **出路在端口层,不在接口**:`fork` 前 `setjmp`,子空间里从 `entry` 起来之后
`longjmp` 回去。⇒ 子进程只需要从**一个已知函数**起步,而 `jmp_buf` 指向的那段栈
在**被复制的空间里地址相同**,所以跳得回去。UML 一类的实现就是这么做的。

⇒ **接口保持原子且与架构无关,复合发生在端口层。** 这正是「原子能力 + port 组合」
这条原则第一次真正吃重的地方。

#### 各后端的答案(clause 6.1:不提供 = 不导出符号)——**初步网络调研**

| backend | 形状 | 可行性 |
|---|---|---|
| openkal-linux | `clone(CLONE_VM=0)` / `mmap` | ✅ |
| **openkal-windows** | ⭐ `RtlCloneUserProcess`(ntdll)→ `NtCreateUserProcess`;内核侧 `MmInitializeProcessAddressSpace` 带 **COW 标志**,子进程**地址空间完全相同且从同一位置继续** | ⚠️ **形状完全对上**,但**未文档化**;不复制全部句柄,CSRSS 连接需另行处理 |
| **openkal-macos** | `fork()`(BSD)可用;Mach 的 `task_create(parent, inherit_memory, &child)` **形状正是本接口** | ⚠️ `fork` 可用但 Apple 强烈不建议(框架在 fork 后不安全);`task_create` 在现代 XNU 上受 SIP/entitlement 限制 ⇒ **需实测** |
| openkal-opensbi / uefi | — | ❌ 不提供 —— **缺席即答案**,与 `openkal.fs` 在 opensbi 上一样 |

⭐⭐ **两条结论:**

1. **Windows 那条形状比预期准得多** —— `RtlCloneUserProcess` 正是「克隆地址空间 +
   从同一处继续」,连 COW 都在内核里做。⇒ 「Windows 没有 fork 原语」这句常识
   **在 NT 层面是错的**;错的是 Win32 API 面没有暴露它。
2. ⚠️ 但两者都带**未文档化 / 受限**的性质。⭐ 而这**不阻塞定形** ——
   clause 6.1 让「不提供」成为一个合法答案。**缺席即答案这条机制,正是让规范可以
   先定形、实现按各平台可行性推进的原因。**

⚠️ 以上为**初步网络调研,未实测**。`RtlCloneUserProcess` 的未文档化状态对一个必须
长期可用的包是真实风险,落地前要评估。

来源:[huntandhackett/process-cloning](https://github.com/huntandhackett/process-cloning) ·
[Abusing Windows' Implementation of Fork()](https://billdemirkapi.me/abusing-windows-implementation-of-fork-for-stealthy-memory-operations/) ·
[GNU Mach: Task Creation](https://www.gnu.org/software/hurd/gnumach-doc/Task-Creation.html)

#### 三处待定,现给出裁决与理由

**① 继承什么 —— ⭐⭐ 不需要开关,两个入口天然分开**

初稿把它当成「一个开关」,又改成「显式传句柄」,**两次都不对**。正确的形状是:
**`create` 与 `clone` 是两个不同的入口,各自的名字已经说出了它给什么。**

| 需求 | 入口 | 得到什么 |
|---|---|---|
| `fork`(要继承全部) | `kal_space_clone(src, &dst)` | ⭐ 复制**内存与句柄** —— 一个空间**持有**它的句柄,克隆自然带上 |
| 沙箱(要一无所有) | `kal_space_create(&s)` + 显式 `grant` | 只有明确交过去的 |

⇒ **零开关、零歧义,而且每个入口的名字就是它的语义。**

#### 这条缝规范里已经有另一半 —— clause 7.11 的形状

`process.h` 自己写下了这个缺口:

> A stream handle of zero denotes that the program inherits … **which is what an
> environment without a general mechanism for passing handles can always provide.**

而且它已经为此留了属性位:**`KAL_PROCESS_PROP_STREAM_PASSING`**。
⇒ **openkal 早就把「跨 spawn 边界传句柄」建模成一个随实现变化的能力,只是把它限定在了流上。**

⭐⭐ **更准确地说,缺的那一半是 `kal_fs_preopen` 的逆。** 把接收侧与供给侧并排看:

| 接收侧(**今天已有**) | 供给侧 |
|---|---|
| `kal_env_arg` / `arg_count` | spawn 的 `argv` ✔ |
| `kal_env_var` / `var_at` | spawn 的 `envp` ✔ |
| 三个标准流 | `kal_spawn_streams` ✔ |
| **`kal_fs_preopen`** | ⭐ **缺失** |

**恰好一行。** 而 SPEC clause 7.11 正是为这个形状写的:

> An interface that **reports** a property … and offers **no way to set it** is
> incomplete … The absence was not visible in the specification text; it became
> visible when such programs were compiled above an implementation.

⇒ **这不是一个新机制,是一个已有机制缺了逆向的那一半** —— 与 `kal_fs_set_modified`
在 0.5 被补上的理由逐字相同。

#### 形状:grant 属于 `openkal.space`,而不是 `openkal.process`

⚠️ clause 8 规定「可以增加声明,不得改动已有的」,而 `kal_process_spawn` 有十个参数、
`kal_spawn_streams` 的布局按 clause 5.3 不可变 ⇒ **不能往 spawn 上加参数。**

⭐⭐ 而 `openkal.space` **本来就是两段式的**(`create` 然后 `start`),grant 天然落在中间:

```
kal_space_create(&space)
kal_space_grant_dir(space, dir, name, len)        ← kal_fs_preopen 的逆
kal_space_grant_stream(space, stream, slot)       ← kal_spawn_streams 的逐个形式
kal_space_start(space, entry, arg, stack_top, &task)
```

**子进程通过已有的 `kal_fs_preopen` 收到它们 —— 接收侧一行不改。**

⇒ 这一处同时满足了五件事:

| | |
|---|---|
| clause 8 | ✔ 不动任何已有声明,`kal_process_spawn` 原样保留 |
| **spawn 与 space 独立** | ✔ spawn =「环境按名字加载,普通继承」;space =「我自己搭子进程的世界」 |
| 一致性 | ✔ 与 `kal_fs_preopen` 逐字对称,名字都一样 |
| 能力式 | ✔ 无环境权威,交出去的都是明写的 |
| 不提供的实现 | ✔ 整个 `openkal.space` 缺席,clause 6.1 |

#### ⭐ 「受限 fork」这个限制随之消失

初稿说「`kal_file` 传不过去 ⇒ 端口层只能给一个受限的 fork」。
⭐ 用 `clone` 就没有这个问题:**一个空间持有它的句柄,克隆复制它们**,
已打开文件的偏移因此是共享的 —— 正是 POSIX `fork` 的语义。
Linux 的 `fork` 复制 fd 表、能力系统克隆 CSpace,两边都自然。

⚠️ **但这要成为规范的一句话**:「空间持有它的句柄;`clone` 复制它们」。
⭐ 而 Windows 那条调研正好给出了它必须是**属性**的理由 ——
`RtlCloneUserProcess` *"does not necessarily copy all … file handles"*
⇒ 加一位 **`KAL_SPACE_PROP_CLONE_HANDLES`**。

⚠️ **仍未决**:`grant_stream` 的 `slot` 与 `kal_spawn_streams` 的三个字段是否统一编号。
**倾向统一**,否则同一件事有两套编号。

**② 与 `spawn` 的关系 —— 独立,而且理由很硬**

「若是复合,加载器就得从环境搬到端口层」的意思是:今天 `kal_process_spawn` 收一个
**路径**,由**环境**完成打开文件、解析 ELF/PE/Mach-O、映射段、重定位、
构造 argv/env/auxv 栈、跳入口 —— 这一整套就是「加载器」。
若把 spawn 定义成 `space` 原子之上的复合,程序就得自己做这一套。

⭐⭐ **决定性理由:可执行格式是环境的属性。**
openkal-windows 上是 PE,openkal-linux 上是 ELF,macos 上是 Mach-O。
加载器若在端口层,端口层就得认识**所有格式** —— **这正是 openkal 存在要消除的
那个 N×M**。

⇒ **两者独立。`spawn` 保持「环境完成加载」,`space` 只管地址空间。**
这句话要写进规范文本,否则将来两条造进程的路会各长一半。

**③ COW —— 判据是「用户可不可感知」,而它恰好可感知一次**

性能不是语义。⭐ **真正可感知的只有一处:失败在什么时候发生。**

| | clone 时 | 后续写入时 |
|---|---|---|
| 立即复制 | 可能失败(ENOMEM) | 不会 |
| COW + overcommit | 成功 | ⚠️ **可能失败**(Linux OOM) |

⇒ 这是**语义差别**,不是性能差别,而且它正是 clause 3.1 所说的「模拟让调用者
静默地错」那一类。

**裁决**:语义上要求 **「成功即完成」**;`kal_space_props` 出一位说明
**本实现是否可能把失败推迟到后续写入**。⭐ 这正是 clause 6.2 属性字的用法 ——
程序**适应**一个属性,而不是调用它。

⚠️ **诚实**:Linux 默认 overcommit 下保证不了 ⇒ 那一位在 openkal-linux 上会是
「可能推迟」。**这不是缺陷,这正是属性字该说的话。**

#### 它解锁的不只是 `fork`

- 沙箱:在一个新空间里跑不受信任的代码
- ⭐ **一个以 openkal 为 ABI 的操作系统的用户态自己造进程** —— 今天 `spawn` 假定
  环境里有加载器,而一个 OS 的 userland 需要自己加载

### 2.5 SPEC clause 11 增补(零代码)

六条,每条注明是**边界**还是**未决**:

| 条目 | 类别 | 理由 |
|---|---|---|
| 进程复制(`fork`) | **边界(操作)** | `kal_fork` 永不(clause 7.1)。⭐ 但 `openkal.space`(§2.4)让端口层把它组合出来 —— **这一行必须同时说出这一点**,否则一句「不会有 fork」会把地址空间那组能力一起埋掉 |
| 网络 | **未决** | 本版不定义;形状草案见 §2.3 |
| 就绪 / 多路复用 | **边界** | 可由 task + stream 导出 ⇒ 属于端口层 |
| 终端控制 | **未决** | 本版不定义;形状草案见 §2.1 |
| 权限 / 属主 | **边界** | 预设身份模型;Windows 只有只读位 ⇒ 不通用 |
| 符号链接的创建/读取 | **边界** | 文件系统**格式**的属性;`std::filesystem` 本就设计成允许失败 |

⚠️ **「边界」不等于「永远」**:第一行同时是边界与候选,写清楚这一点是本节存在的
理由 —— 一句「不会有 fork」会把地址空间那组能力一起埋掉。

### 2.6 规范包的配套

- `SURFACE.txt` 增新接口的名字(⚠️ 它是 clause 9 的**唯一**依据)
- `tools/check-surface.sh` —— ⚠️ [[link-error-is-the-mechanism-not-the-defect]] 记着:
  不带 `--complete` 时它只查**多出**的名字,所以一个非规范实现能通过。
  **新增接口必须同时让 `--complete` 那一侧变红**才算写完。
- conformance 套件增对应用例

---

### 2.7 ⭐ `openkal-kit` —— 规范之外的组合库(同仓库,独立包)

**动机**:判据二把一批东西判给了「端口层」—— 就绪、进程内流对、`fork` 助手、DNS。
⚠️ 但「端口层」今天只有一个,而且它组合出的是 **POSIX**。
⇒ **一个原生 openkal 应用想同时等两个流,今天没有答案** ——
要么自己写一遍 thread-per-source,要么把整个 musl 拉进来。

#### 定位

| | |
|---|---|
| 位置 | ⭐ **openkal 仓库内,与 `include/`、`src/` 同级的 `kit/`** —— 发现性最好 |
| 形态 | **独立的 `mcpp.toml`**,包名 `mcpplibs/openkal-kit`,模块 `openkal.kit.*` |
| 规范地位 | ⭐ **不进 SPEC.md,非规范** —— 它的价值恰恰在于**可以演进**,而 clause 8 让规范不能 |
| 测试 | ✔ 有自己的测试集,进 openkal 仓库的 CI |
| 启用 | `openkal` 包的一个 feature(`[feature-deps.kit]`),不用的不链接 |

#### ⭐⭐ 它结构上不可能被误认成规范的一部分

clause 10 规定 **openkal 的契约形式是 C ABI**。
⇒ **kit 刻意不是 C ABI** —— 它是 C++ 模块 + `namespace kal::kit`,不导出任何
`kal_` 开头的 C 符号。

这一条同时解决了 `SURFACE.txt` 的约束:*"exports no other name beginning with
`kal_`"* —— C++ 修饰名是 `_ZN3kal3kit…`,**不以 `kal_` 开头** ⇒
`tools/check-surface.sh --complete` 不会把它读成一个不合规的实现。

⭐ **契约形式本身成了「是不是规范」的判据**,而不是靠一句声明。

#### 内容(全部由原子组合,零新接口)

| 模块 | 由什么组合出来 |
|---|---|
| `openkal.kit.wait` | 等 N 个流之一就绪 = `kal_task_wait/wake` + 阻塞读 + 预读缓冲 |
| `openkal.kit.pipe` | 进程内流对 = 环形缓冲 + `wait/wake` |
| `openkal.kit.spawn` | `fork` 助手 = `openkal.space` 的 `clone` + `setjmp/longjmp` |
| `openkal.kit.name` | 端点名字解析(DNS)= `openkal.net` 之上;clause 3.4 明确把它推到接口之外 |

#### 消费者两类

**原生 openkal 应用** · **openkal-musl**(§3.7 的就绪实现直接用 `openkal.kit.wait`)

⭐ **它顺带回答了一个此前没答的问题**:判据二说「可导出的归端口层」,
但没说**归哪个**端口层。答案是:**归一个所有端口层共用的库** ——
否则「同一个组合写第二遍」会在第二个端口层出现时发生。

⚠️ **未决:它与 openkal-musl 的边界。** musl 的 fd 表在 musl 里,而就绪的预读缓冲要挂
在 fd 上 ⇒ **kit 给的是「等一组流」,fd 那一层的适配仍在 musl。** 这条边界要在写之前画清。

### 2.8 ⭐⭐ 深度 review:七处问题,三处是设计错误

判据五条:**对上**(原子 · 可组合 · 最小 · 一致)、**对下**(backend 容易实现)。
逐个接口压测的结果如下。⚠️ 前三条推翻了 §2.1–§2.4 的部分内容。

#### ⓵ ⭐⭐⭐ `kal_space_create` 要删掉 —— 两条独立的理由

**对下:Linux 上几乎无法实现。** `fork` 给的是**克隆**;要造一个**空**的用户地址空间,
只能 spawn 一个 stub 程序再往里写 —— 那需要一个 stub 二进制,是不能接受的形状。

**对上:它逻辑上也没有意义。** `kal_space_start(space, entry, …)` 的 `entry` 是一个
**本空间的**函数指针。在一个**空**空间里,那段代码根本不在里面。
⭐ **两条独立的理由指向同一个裁决 —— 这通常说明裁决是对的。**

⇒ **`openkal.space` 只剩 `clone` / `start` / `destroy` / `props`。**
而沙箱(要一无所有)由 **spawn + grant** 满足,不由 space 满足。

⚠️ **连带结论:`space` 实际上只服务 `fork` 一个用例。** 它值不值四个操作,
应当在定形时重新问一次 —— 本文倾向值得,因为 `fork` 之外还有「OS 用户态自己造进程」。

#### ⓶ ⭐⭐⭐ 三个接口都产生**拥有的流**,而 core 的流是**借来的**

`openkal.stream` 的模型是借用(clause 6.7 / clause 11:*"Every handle in the core
interfaces is borrowed"*),所以它**没有 close**。而:

| 接口 | 产生 | 需要 close 吗 |
|---|---|---|
| `openkal.fs` | `kal_file` | ✔ 已有 `kal_fs_close_file` |
| `kal_process_channel` | 两个流 | ⚠️ **缺** |
| `kal_net_connect` / `accept` | 流 | ⚠️ **缺** |

⚠️⚠️ **channel 缺 close 不是小事**:父进程 spawn 之后不关掉 `child_end`,
读端**永远等不到 EOF** —— 这是管道的经典陷阱,而接口不提供关闭就无法避免。

⭐ **同一个问题第三次出现,说明该收口的是规则而不是补丁。**
clause 11 自己写着:*"Owned handles arrive with `openkal.fs`, and the rules for
their release … are deferred to that interface."* —— **那笔延期到期了。**

**裁决**:沿用 `openkal.fs` 已确立的形状 ——**谁给的谁收**,每个产生拥有句柄的接口
自带它的 close(`kal_process_channel_close` / `kal_net_close`)。
❌ **不**给 core 的 `kal_stream` 加通用 close:那会改变 core 接口的语义,
而 clause 3.2 的理由(core 一旦扩张就要求每个实现)同样适用于给 core 加操作。

⇒ channel 变 2 个操作,net 变 7 个。

#### ⓷ ⭐⭐ `terminal` 的两个 set 无法恢复原状

`set_raw(s, 0)` 恢复到**什么**?一个 TUI 退出时必须把终端还原成**它进来时的样子**,
否则用户的 shell 坏掉。我给的两个 setter **没有保存原状的地方**。

⇒ 改成 clause 7.11 要求的**成对**形状,而且顺带从 2 个操作变回 2 个:

```
kal_terminal_get_mode(stream, kal_uintptr* mode)
kal_terminal_set_mode(stream, kal_uintptr  mode)
kal_terminal_size(stream, &cols, &rows)
kal_terminal_props
/*  位: KAL_TERM_LINE_EDIT  KAL_TERM_ECHO  */
```

⭐ 与 `kal_fs_file_info` / `kal_fs_set_modified` **同构**,而且 save/restore 是
`get` 一次、退出时 `set` 回去 —— 一个 TUI 本来就这么写。

⚠️ **裸机上仍然成立**:UART 没有行编辑 ⇒ `get_mode` 报「都关着」,`set_mode` 是
无事可做即完成。

#### ⓸ ⭐⭐ 「诞生时收到的东西」缺一个名字 —— `kal_grant`

`argv` / `envp` / 三个流 / preopen 是**同一类东西**(openkal.env 的定义就是
*"the parameters a program receives at inception"*),而它们今天在**四个地方各自表达**。

把它具体化成一个对象,§2.4 的 grant 问题与沙箱问题一起解决:

```
kal_grant_create(&g)
kal_grant_add_arg / add_var / add_stream / add_dir
kal_grant_spawn(g, base, path, path_len, &proc)      按名字启动一个程序
kal_grant_start(g, space, entry, arg, stack, &task)  在一个克隆的空间里启动
kal_grant_close(g)
```

⭐⭐ **它同时做到三件事**:
1. `kal_process_spawn` 的**十个参数塌成三个**(base/path/grant)
2. **grant 同时服务 spawn 与 space** —— 沙箱走 `grant_spawn`,fork 走 `clone`+`grant_start`
3. 与 `kal_fs_preopen` / `kal_env_*` 的接收侧**逐字对称**(clause 7.11 的逆)

⚠️ **代价**:`kal_process_spawn` 保留不动(clause 8),于是**有两条 spawn 路**。
⭐ 这次是**有意且一次性**的,规范可以明说哪条是正道 —— 但**这是一个大决定,
必须单独确认**,它触及 `openkal.process` 的地位。

⚠️ **对下检查**:实现要能「暂存一组句柄再一次性交付」——
Linux 用 `posix_spawn_file_actions` 或 fork+dup2 ✔;Windows 用
`PROC_THREAD_ATTRIBUTE_HANDLE_LIST` ✔;裸机不提供 process ⇒ 不适用 ✔。

#### ⓹ ⭐⭐ net 的数据报问题,实质是「DNS 能不能用」

`openkal.kit.name`(DNS)需要 UDP。若 v1 只做流,DNS 只能走 TCP ——
而不少 resolver 不接受 TCP-only。⇒ **「要不要数据报」实际上等于「net 落地之后
有没有人能用域名」。**

⚠️ 加上数据报,net 从 7 个操作变成 ~11 个 —— **这就不小了**。
⇒ ⭐ **这是 net 最大的设计张力,必须专门决策,不能顺带。** 本文不预设答案。

#### ⓺ ⭐ 窗口尺寸变化只能轮询 —— 可接受,但必须写下来

openkal 没有信号 ⇒ 没有 SIGWINCH。TUI 只能在事件循环里轮询 `kal_terminal_size`。
⭐ 在一个本来就有事件循环的程序里这是自然的,**但 TUI 作者会先去找通知机制**
⇒ README 必须直说。

#### ⓻ ⭐ listener 不是流,而 `kit.wait` 只等流 —— **组合路径成立**

一个服务器要同时等 listener 与若干连接。listener 不是 `kal_stream`,
`kit.wait` 表面上等不了它。
⇒ 但 kit 可以为 listener 起一个辅助 task 做阻塞 `accept` ✔。

⭐⭐ **这条值得单记:它是「组合发生在库里」这条路第一次被真实用例压测,而它通过了。**
接口不必为组合的方便而变形。

#### 修正后的规模

| 接口 | 操作数 | 变化 |
|---|---|---|
| `openkal.terminal` | 4 | 形状改(成对),数量不变 |
| `openkal.process` 加法 | **2**(channel + close) | +1 |
| `openkal.space` | **4** | −3(删 `create` 与两个 grant) |
| `openkal.grant` | **7** | ⭐ 新增,但吸收了 space 的 grant 与 spawn 的参数表 |
| `openkal.net` | **7 或 ~11** | +1(close);数据报未决 |

---

### 2.9 ⭐⭐ 完整性验收:面够不够用

**方法**:每条需求走一遍「需求 → 组合路径 → 缺哪个原子」。不看接口漂不漂亮,
只看**组合得出来还是组合不出来**。

#### 结论

> **面基本够用。全部需求里只有一个组合不出来,而它是一个真实的原子缺口:**
> **⭐⭐⭐ 一个阻塞中的读,如何结束。**

#### 缺口的证据链(实测两处头文件)

| 实测 | 结果 |
|---|---|
| `task.h:51` `kal_task_wait(word, expected, **timeout_ns**)` + 属性位 `KAL_TASK_PROP_WAIT_TIMEOUT` | ✅ **主循环可以带超时醒来** |
| `stream.h:26` `kal_stream_read(s, buf, len)` | ⚠️ **没有截止时间,阻塞不可打断** |

⇒ `openkal.kit.wait` 的实现(每源一个辅助 task 做阻塞读)**对短命程序成立**
(TUI 退出时 `kal_exit` 收走一切),**对长跑程序漏 task** ——
每关一个连接漏一个,服务器跑一天漏一万个。

⚠️ 同一个缺口还挡住另外两处:`select`/`poll` 的 **timeout 参数**,
以及「**等多个子进程之一退出**」(`kal_process_wait` 也是不可打断的阻塞)。

#### 按三问判它

| 问 | 答 |
|---|---|
| 可导出? | ❌ **不可** —— 一个已经阻塞在环境里的调用,只有环境能让它返回 |
| 几个? | 一个 ⇒ openkal |
| OS 的形状? | ❌ POSIX 用信号/`pthread_cancel`、Windows 用 `CancelIoEx`、Fuchsia 用 handle close —— **三种形状**,而「等待要有尽头」是它们共同的那个概念 |

⇒ **它通过,而且是本轮唯一一个通过的新原子。**

#### 两个候选形状 —— ⚠️ 必须裁决,本文不预设

**(a) 带截止时间的等待**

```
kal_stream_read_until(stream, buf, len, deadline_ns)   → kal_io_result
kal_net_accept_until (listener, deadline_ns, &stream)
```

| | |
|---|---|
| ✅ 一次解决三件事 | 超时读 · 辅助 task 可退出 · `select` 的 timeout |
| ✅ 一致性 | 与 `kal_task_wait` 的 `timeout_ns` **形状逐字相同**(ns,零=无超时) |
| ✅ 对下容易 | Linux `poll`+`read` · Windows overlapped/`WaitForSingleObject` · 裸机 UART 轮询+timer —— **每个环境都给得出** |
| ⚠️ 代价 | 不是「立刻取消」,辅助 task 最多再等一个 deadline |
| ⚠️ 位置 | `openkal.stream` 在 **core** 里,而 clause 3.2 的理由(core 一旦扩张就要求**每个**实现)同样适用于加操作 ⇒ ⭐ 应当**自成一个可选接口**(clause 6.2:「实现可能没有的 operation 自成接口」),资源是流/listener |

**(b) 关闭唤醒**:在 ⓶ 新增的 `kal_net_close` / `kal_process_channel_close` 的**语义**里
规定「关闭使阻塞在该流上的读返回」。

| | |
|---|---|
| ✅ 不加任何新操作 | 只写一句语义 |
| ⚠️ 只覆盖**拥有的**流 | 借来的 stdin 仍然打不断(可接受:`kal_exit` 收尾) |
| ⚠️ 解决不了 timeout | `select(timeout)` 仍然没有答案 |
| ⚠️⚠️ **对下更难** | Linux 上 `close(fd)` **不保证**唤醒 blocked reader ⇒ 实现得自己配一个内部 eventfd + poll。**接口简单了,实现复杂了** |

⭐ **本文倾向 (a)**:它对下更容易(实现本来就要 poll),而且多解决一件事(timeout)。
但这是一个需要拍板的决定。

#### 其余需求:全部可组合 ✔

| 需求 | 组合路径 | |
|---|---|---|
| `fork` | `space.clone` + `setjmp/longjmp` + `grant` | ✔ |
| 子进程 + 捕获输出 | `channel` + `grant_spawn` | ✔ |
| 进程内 pipe | kit:环形缓冲 + `task.wait/wake` | ✔ |
| socket 服务器 | `net.listen/accept` + `kit.wait` | ✔(需缺口) |
| 终端 UI | `terminal.get/set_mode` + `size` 轮询 | ✔ |
| `std::thread` / `atomic::wait` | `task.start` / `task.wait/wake` | ✔ **正好对上** |
| `std::chrono` / `sleep_for` | `time` | ✔ |
| `std::random_device` | `random` | ✔ |
| `<filesystem>` | `fs`(部分操作允许失败,标准如此设计) | ✔ |
| 时区数据库 | 读 tzdata 文件 ⇒ `fs` | ✔ |
| `dlopen` / 动态链接 | 读文件 `fs` + `openkal.exec` 的可执行内存 + 重定位(库) | ✔ **exec 够用** |
| 等多个子进程之一 | kit + 辅助 task | ✔(需缺口) |
| cwd / `chdir` | 端口层在 preopen 之上模拟(`okm_getcwd.c` 已有) | ✔ |

#### 两条已识别、本轮不必,但其中一条会影响定形

| | 三问 | 处置 |
|---|---|---|
| **共享内存**(两个空间共用一段) | 通过(不可导出 · 一个 · 通用:shm/section/VMO) | ⚠️⚠️ **本轮不做,但 `space` 定形时必须留一句话** —— 将来它一出现,`kal_space_clone` 就要回答「共享的那段怎么办」 |
| **文件锁**(多进程协调) | 通过,但裸机没有 | 记入 clause 11,今天无消费者 |

⚠️ **内存映射文件不是缺口,是性能**:`mmap(fd)` 的用途(动态链接、大文件)
都能用 `read` + `openkal.exec` 组合出来,只是慢。

---

### 2.10 ⭐⭐ 用四问回扫:数据报的结论是反的,而 grant 是我过度设计

**判据(四问,顺序即优先级)**:

1. **是不是最小的原子能力?**
2. **是不是所有内核都有?**
3. **是不是只能由 openkal 给,而不能由已有原子组合出来?**
4. **是不是通用(不与某个 OS 的形状绑定)?**

> 能由原子组合出来的,一律去 `openkal-kit`。只有**必须**的才入规范。
> 且**对上对下双向**检查,并与 openkal 已有的语义/风格一致。

#### ⓵ 数据报:⭐⭐⭐ 我的结论是反的

初稿说「加上数据报 net 从 7 变 11,就不小了」。**用四问过一遍,这个反对理由不成立。**

| 问 | 答 |
|---|---|
| 最小原子? | ✔ `open` / `send_to` / `recv_from` / `close` / `props` —— **5 个** |
| 所有内核都有? | ✔ 而且 ⭐ **比流更普遍** —— 一个 IP 栈实现 UDP 只要几百行,TCP 要几千行 |
| 只能 openkal 给? | ✔ **消息边界、无连接、可能乱序** —— 这些**不可能**由字节流组合出来 |
| 通用? | ✔ |

⭐⭐ **而「变成 11 个操作」这个担心,是因为我把它们当成了一个接口。**
按 clause 6.4(我自己在 §2.3 就引用过它):**流与数据报是两种资源** ——
「定位适用于文件而不适用于连接;半关闭适用于连接而不适用于文件」是同一条推理。

⇒ **它们应当是两个接口:**

```
openkal.net        连接（流）  connect / listen / accept / shutdown / close / props   6
openkal.datagram   数据报      open / send_to / recv_from / close / props             5
```

⭐ **拆开之后,对上对下都更好:**

| | |
|---|---|
| **对下** | 一个裸机 BSP 可以**只提供 datagram**(UDP 好写得多)⇒ clause 6.1 的缺席机制正好用上 |
| **对上** | ⭐ **DNS 不再被「TCP-only」卡住** —— `kit.name` 只依赖 `openkal.datagram` |
| **风格** | 与 `fs` / `process` / `task` 一样**按资源分**,不是按主题分 |

⇒ **§2.8 ⓹ 那条「必须专门决策」的张力消失了。答案是拆,不是选。**

⚠️ **DNS 的 resolver 地址从哪来?** POSIX 读 `/etc/resolv.conf`(ambient 配置)。
⭐ openkal 上应当**由调用方显式传入** —— 这与能力式模型一致(程序不该有 ambient 的
resolver),而且它可组合(从 `kal_env_var` 或一个 preopen 的文件读,由**程序**决定)。
⇒ **不需要新原子。**

#### ⓶ ⭐⭐ 同一把尺子回扫,`kal_grant` 是我过度设计

§2.8 ⓸ 引入 `kal_grant` 对象(7 个操作),理由两条:
**(1)** 把 preopen 交给一个新空间 · **(2)** 让 `kal_process_spawn` 的十个参数塌成三个。

用四问过:

| 部分 | 只能 openkal 给? | 判定 |
|---|---|---|
| `add_dir`(交出一个目录句柄) | ✔ 不可组合 | **必须** |
| `add_stream` | ✔ | **必须**(spawn 已有,形式不同) |
| `add_arg` / `add_var` | ❌ **argv/env 只是字节,spawn 已经收它们** | **便利,不是原子** |
| 「塌掉参数表」 | ❌ 纯便利 | **不是理由** |

⇒ ⭐ **grant 对象把「没办法传的」和「已经有办法传的」放进了同一个对象。**
按「只有必须的才入规范」,它的正当理由只剩 `add_dir` 一条。

⚠️ 而**与 openkal 风格一致**这条也指向同一边:`kal_process_spawn` 本来就是十个参数
的长参数表,**再加一个参数与既有风格一致**;引入一个 grant 对象是**一个新概念**。

⇒ **退回最小形状 —— 一个新声明,一个新参数:**

```c
struct kal_preopen { struct kal_dir dir; const char* name; kal_uintptr len; };

int kal_process_spawn_with(struct kal_dir base, const char* path, kal_uintptr path_len,
                           const char** argv, const kal_uintptr* argv_lens, kal_uintptr argc,
                           const char** envp, const kal_uintptr* envp_lens, kal_uintptr envc,
                           const struct kal_spawn_streams* streams,
                           const struct kal_preopen* grants, kal_uintptr grant_count,
                           struct kal_process* out);
```

⚠️ 重复十个参数**是丑的**,但它是**最小的**:一个声明、零新对象、零新概念,
且 `kal_process_spawn` 原样保留(clause 8)。
⭐ 这是「最小 + 一致」压过「好看 + 便利」的一次,而那正是这四问的排序。

#### ⓷ ⭐⭐⭐ 连带:`openkal.space` 的两个 grant 也不需要了

§2.8 ⓵ 删掉 `kal_space_create` 之后,`space` 只剩 `clone` —— 而
**`clone` 已经复制了全部句柄**(§2.4)。于是:

| 用例 | 走哪条 | 还需要 space 的 grant 吗 |
|---|---|---|
| `fork`(继承全部) | `kal_space_clone` | ❌ clone 全带 |
| 沙箱(只给指定的) | `kal_process_spawn_with` | ❌ 不走 space |

⇒ ⭐ **`kal_space_grant_dir` / `kal_space_grant_stream` 整个删掉。**

#### 修正后的规模(与 §2.8 对照)

| 接口 | §2.8 | **§2.10** | 变化 |
|---|---|---|---|
| `openkal.terminal` | 4 | 4 | — |
| `openkal.process` 加法 | 2 | **3** | +1(`spawn_with`) |
| `openkal.space` | 4 | **4** | 形状变(去 grant,回到 clone/start/destroy/props) |
| `openkal.grant` | 7 | **0** | ⭐ **删除** —— 便利不是原子 |
| `openkal.net`(连接) | 7 或 ~11 | **6** | ⭐ 张力消失 |
| `openkal.datagram` | — | **5** | ⭐ 新增,独立接口 |
| 截止时间(§2.9) | — | **2** | 待裁决 |

⇒ **总计 24 个操作,分在六个可选接口里**,而 §2.8 的版本是 24 个分在五个里 ——
**数量持平,而每个接口都更小、更可独立提供、且没有一个是「便利」。**

⭐⭐ **这一节的价值不在数字,在于它证明了那四问是可执行的**:同一把尺子,
一次推翻了我的一个反对理由(数据报),一次扫掉了我的一处过度设计(grant)。
**判据要能否定作者自己,才算判据。**

---

### 2.11 ⭐⭐ 第二次深度 review:采纳 (a),但理由和代价都要重写

**(a) 带截止时间的等待,已采纳。** 但本轮 review 找到一个**更好的候选**,
算清之后它仍然输 —— 输在哪里,比结论更值得记。

#### ⓵ ⭐⭐⭐ 考虑过而未采纳:就绪通知(`notify`)

与其「让阻塞的读能超时退出」,不如**让它根本不必阻塞**:

```
kal_stream_notify(stream, kal_u32* word)    该流可读时，递增 *word 并 wake 它
```

主循环于是 `kal_task_wait(word, seen, timeout)` —— ⭐ **一个 task 等 N 个源,
辅助 task 整个不需要。**

| | `read_until`(a) | `notify` |
|---|---|---|
| 操作数 | ⚠️ 每个可无限阻塞的操作 +1(见 ⓶) | ⭐ **1 个覆盖全部可等待的东西** |
| 辅助 task | 仍需 N 个(只是能退出了) | ⭐ **不需要** |
| ⭐ 预读缓冲(§3.7) | ⚠️ **仍然需要** | ⭐ **整个消失**(通知不消耗数据) |
| 与既有原子咬合 | 平行的第二套超时 | ⭐⭐ **正好接上 `kal_task_wait/wake`** |
| §2.8 ⓻ 的 listener | 靠 kit 变通 | ⭐ 接口层面解决 |
| 对下:裸机 | 轮询 + timer | ⭐ **中断 → wake,更自然** |
| **对下:Linux** | ⭐ `poll(fd, ms)` + `read`,**十行、无状态、无线程** | ⚠️⚠️ **要一个进程级 epoll + 一个后台线程** |

⇒ ⭐⭐ **它在五项上更好,输在最后一项 —— 而最后一项是 clause 7.1。**

> **clause 7.1(Naturalness)**:实现不应被迫构建一个兼容层。

一个**隐藏的后台轮询线程**,正是「重建一套事件机制」。而 `read_until` 在 Linux 上
就是驱动本来就要做的那件事(`poll` 然后 `read`)。

⇒ **按 clause 7.1 采纳 (a)。** 并按规范自己的体例(**clause 6.3
「Mechanisms considered and not adopted」**),把 `notify` 连同这张表写进规范 ——
⭐ **否则下一个人会重新想到它,并重新算一遍。**

#### ⓶ ⚠️ (a) 的代价比上一版说的大

「哪些操作会无限阻塞」穷举一遍,每个都要一个带截止时间的版本:

| 会无限阻塞 | 需要 |
|---|---|
| `kal_stream_read` | ✔ |
| `kal_stream_write` | ⚠️⚠️ **我上一版漏了** —— 管道满 / TCP 窗口满时会阻塞 |
| `kal_net_accept` | ✔ |
| `kal_datagram_recv_from` | ✔ |
| `kal_process_wait` | ✔ |
| `kal_task_join` | ❌ 可由 `wait/wake` 协调,不需要 |

⇒ **`openkal.deadline` 是 5 个操作 + props = 6**,与 `openkal.net` 同样大。
**这是 (a) 的结构性代价:每新增一个可无限阻塞的操作,这个接口就 +1。**

⚠️ 而「同一件事两个版本」在规范里**有先例**:clause 7.8 让
`kal_fs_open_file`(两个 flag)与 `kal_fs_open`(整个意图一个字)并存,并给了理由。
⇒ 不是新问题,但**这个先例必须被引用**,否则它看起来像随手加的。

#### ⓷ ⭐⭐ `kal_stream_write` 会阻塞 ⇒ `popen` 的经典死锁,§3.3 漏了

kit 的 wait 只处理了**读侧**。而一个子进程往满的管道写、父进程又在等它退出 ⇒ **死锁**。
这是 `popen` 教科书级的陷阱,而 §3.3 的两阶段实现没有提。

⇒ **§3.3 必须补一条**:临时文件阶段天然没有这个问题(文件不会满);
换到 `kal_process_channel` 之后**就有了** ⇒ 那一步必须同时用上
`kal_deadline_write`。⭐ **一个「后来才出现的死锁」比一开始就有的更难查。**

#### ⓸ ⭐⭐ `kal_space_start` 应当返回 `kal_process`,不是 `kal_task`

一个跑在**另一个地址空间**里的执行上下文,`kal_task_join` 能 join 它吗?
跨空间的 task 句柄语义没人定过。

⭐ 而它本来就有名字:**一个在自己地址空间里执行的东西,就是一个进程。**

⇒ `kal_space_start(space, entry, arg, stack_top, struct kal_process* out)`,
之后 `kal_process_wait` / `terminate` / `close` **全部复用**。
**一致性 +1,新概念 −1,操作数不变。**

#### ⓹ 四处小项(写下来免得落掉)

| | 裁决 |
|---|---|
| `struct kal_endpoint` 定义在哪 | `types.h`(或共享头)—— net 与 datagram 共用,且**一个缺席时另一个仍要能用** |
| `kal_net_shutdown` 的方向 | 三个常量:read / write / both |
| `kal_terminal_size` 在非终端流上 | `kal_err_not_supported` —— 与今天 `ioctl` 的 `ENOTTY` 对应 |
| 跨接口引用类型 | ✔ **已有先例**:`kal_fs_stream(kal_file) → kal_uintptr` 已经引用了 stream。**声明在规范包、符号在实现包**,类型可见而符号缺席,正是 openkal 的模型 |

#### 定稿规模

| 接口 | 操作数 | |
|---|---|---|
| `openkal.terminal` | 4 | `get_mode` / `set_mode` / `size` / `props` |
| `openkal.process` 加法 | 3 | `channel` / `channel_close` / `spawn_with` |
| `openkal.space` | 4 | `clone` / `start`(→`kal_process`) / `destroy` / `props` |
| `openkal.net` | 6 | `connect` / `listen` / `accept` / `shutdown` / `close` / `props` |
| `openkal.datagram` | 5 | `open` / `send_to` / `recv_from` / `close` / `props` |
| **`openkal.deadline`** | **6** | `read` / `write` / `accept` / `recv_from` / `wait_process` / `props` |

⇒ **28 个操作,六个可选接口,没有一个是「便利」。**
⚠️ 比 §2.10 多 4 个,全部来自 ⓶ 的穷举 —— **穷举比估计贵,但估计会漏。**

---

### 2.12 头文件草案(六个接口)

⚠️ **草案,不是定稿。** 每份都按现有头文件的约定写:句柄是**单字 struct 传值**、
字符串**永远带长度**、传输返回 `struct kal_io_result`、释放返回 `void`、
只 include 兄弟头。

#### `openkal/timeout.h` —— ⚠️ 名字从 `deadline` 改了,理由见 §2.13 ⓷

```c
/* openkal.timeout --- a bound upon operations that would otherwise wait
 * without end.
 *
 * Every operation here is the operation of the same name in another interface,
 * with one argument added. Clause 7.8 already establishes that a second form of
 * one operation is admissible when the first cannot state the whole of an
 * intent: `kal_fs_open_file' and `kal_fs_open' stand beside each other for that
 * reason, and these stand beside their originals for the same one.
 *
 * ⚠️ THE ARGUMENT IS A DURATION, NOT AN INSTANT, and the name of this interface
 * was changed to say so. `kal_task_wait' already takes `timeout_ns' and already
 * defines zero as no timeout; a second spelling of the same idea would be the
 * one thing this specification most consistently refuses.
 *
 * An expired bound is reported as `kal_err_again' --- "the operation would
 * block" --- which is what an expiry is. The error set is closed (clause 5.2)
 * and required no addition. */
#ifndef OPENKAL_TIMEOUT_H
#define OPENKAL_TIMEOUT_H
#include "types.h"
#include "stream.h"
#include "net.h"
#include "datagram.h"
#include "process.h"

#ifdef __cplusplus
extern "C" {
#endif

struct kal_io_result kal_timeout_read (struct kal_stream, void*,       kal_uintptr len, kal_u64 timeout_ns);
struct kal_io_result kal_timeout_write(struct kal_stream, const void*, kal_uintptr len, kal_u64 timeout_ns);

int kal_timeout_accept(struct kal_net_listener, kal_u64 timeout_ns, struct kal_stream* out);

struct kal_io_result kal_timeout_recv_from(struct kal_datagram, void*, kal_uintptr len,
                                           struct kal_endpoint* from, kal_u64 timeout_ns);

int kal_timeout_wait_process(struct kal_process, kal_u64 timeout_ns,
                             int* status, int* terminated);

/* The smallest bound this implementation distinguishes, in nanoseconds. A
 * board whose only clock ticks at a millisecond reports 1000000; a caller that
 * asks for less is not refused and does not get less. Per implementation, so a
 * word rather than an enquiry (clause 6.2). */
extern const kal_uintptr kal_timeout_granularity_ns;

#ifdef __cplusplus
}
#endif
#endif
```

#### `openkal/terminal.h`

```c
/* openkal.terminal --- what an interactive stream does with what is typed at
 * it. The resource is a stream for which `kal_stream_props' reports
 * KAL_STREAM_PROP_INTERACTIVE; every operation here reports
 * kal_err_not_supported for any other.
 *
 * A separate interface rather than operations upon `openkal.stream', for the
 * reason clause 6.4 gives when it places positioning in `openkal.fs': the
 * behaviour varies between the RESOURCES of the stream interface, and an
 * implementation could neither claim these honestly nor withhold them usefully.
 *
 * ⚠️ THE PAIR IS get/set AND NOT two setters. A program that turns line editing
 * off must be able to put back what was there, and a setter alone gives it
 * nothing to put back --- it would restore a default, and the terminal a user
 * returns to is then not the one they had. Clause 7.11 states the general rule;
 * this is an instance of it. */
#ifndef OPENKAL_TERMINAL_H
#define OPENKAL_TERMINAL_H
#include "types.h"
#include "stream.h"

/* Positions in the mode word. A position that has not been assigned reads as
 * zero, so a program compiled against a later specification behaves correctly
 * against an earlier implementation (clause 6.2). */
#define KAL_TERM_LINE_EDIT ((kal_uintptr)1u << 0)  /* the environment assembles lines */
#define KAL_TERM_ECHO      ((kal_uintptr)1u << 1)  /* the environment shows what is typed */

#ifdef __cplusplus
extern "C" {
#endif

int kal_terminal_get_mode(struct kal_stream, kal_uintptr* mode);
int kal_terminal_set_mode(struct kal_stream, kal_uintptr  mode);

/* The size of the display, in character cells. An environment that does not
 * know --- a serial line has no way to ask --- reports kal_err_not_supported
 * and leaves both outputs untouched.
 *
 * ⚠️ THERE IS NO NOTIFICATION. openkal has no signals, so a program learns of a
 * change by asking again. A program with an event loop already has somewhere to
 * ask from; one without does not need to know. */
int kal_terminal_size(struct kal_stream, kal_uintptr* cols, kal_uintptr* rows);

kal_uintptr kal_terminal_props(struct kal_stream);   /* enquiry: varies per resource */

#ifdef __cplusplus
}
#endif
#endif
```

#### `openkal/net.h` —— 连接

> ⚠️ **本草案已被 §7.5 ⓶ 取代**:交出的是被拥有的 `kal_net_conn`,流经 `kal_net_stream` 从它借。下面保留的是当时提出的形状。


```c
/* openkal.net --- a connection, which is a stream with a peer and a way to be
 * half-closed. Clause 3.4 records why this is not merged with `openkal.fs':
 * positioning applies to a file and not to a connection, half-closure to a
 * connection and not to a file.
 *
 * ⚠️ NAME RESOLUTION IS NOT HERE. Clause 3.4 excludes it in terms: an
 * implementation shall not be required to parse an unbounded set of name
 * schemes. An endpoint is an address and a port; turning "example.com" into one
 * is a library above `openkal.datagram'. */
#ifndef OPENKAL_NET_H
#define OPENKAL_NET_H
#include "types.h"
#include "stream.h"

struct kal_net_listener { kal_uintptr h; };

/* Directions for kal_net_shutdown. */
#define KAL_SHUT_READ  1
#define KAL_SHUT_WRITE 2
#define KAL_SHUT_BOTH  3

/* Positions in kal_net_props. */
#define KAL_NET_PROP_IPV6      ((kal_uintptr)1u << 0)
#define KAL_NET_PROP_HALFCLOSE ((kal_uintptr)1u << 1)

#ifdef __cplusplus
extern "C" {
#endif

int kal_net_connect(const struct kal_endpoint*, struct kal_stream* out);
int kal_net_listen (const struct kal_endpoint*, struct kal_net_listener* out);
int kal_net_accept (struct kal_net_listener,    struct kal_stream* out);

/* Ends transfer in one direction while the other continues. This is the
 * operation that distinguishes a connection from a file, and it is why the two
 * are separate interfaces. */
int kal_net_shutdown(struct kal_stream, int direction);

/* ⚠️ AN OWNED STREAM, UNLIKE THE THREE `openkal.stream' provides. A connection
 * is obtained and must be released; the standard streams are borrowed and are
 * not. Same division `openkal.fs' already draws with kal_fs_close_file. */
void kal_net_close         (struct kal_stream);
void kal_net_close_listener(struct kal_net_listener);

extern const kal_uintptr kal_net_props;

#ifdef __cplusplus
}
#endif
#endif
```

#### `openkal/datagram.h`

```c
/* openkal.datagram --- messages with boundaries, sent without a connection.
 *
 * A SEPARATE INTERFACE FROM `openkal.net', for the reason clause 6.4 gives: a
 * datagram and a connection are two resources, and an operation that some
 * resources of an interface can never satisfy does not belong in it. A message
 * boundary is not a property a byte stream has; ordering is not a property a
 * datagram has.
 *
 * ⭐ AND IT IS THE EASIER HALF TO PROVIDE. A board that carries an IP stack
 * reaches datagrams in a few hundred lines and connections in a few thousand,
 * so an implementation that supplies only this one is ordinary rather than
 * deficient --- clause 6.1 already expresses that by absence. */
#ifndef OPENKAL_DATAGRAM_H
#define OPENKAL_DATAGRAM_H
#include "types.h"

struct kal_datagram { kal_uintptr h; };

#define KAL_DGRAM_PROP_IPV6      ((kal_uintptr)1u << 0)
#define KAL_DGRAM_PROP_BROADCAST ((kal_uintptr)1u << 1)

#ifdef __cplusplus
extern "C" {
#endif

/* A local endpoint of zero port asks the environment to choose one. */
int kal_datagram_open(const struct kal_endpoint* local, struct kal_datagram* out);

/* A message is sent whole or not at all; a partial send is not a result this
 * interface produces. */
struct kal_io_result kal_datagram_send_to(struct kal_datagram, const void*, kal_uintptr len,
                                          const struct kal_endpoint* to);

/* Reports one message and who sent it. A message longer than the buffer is
 * truncated and the excess is lost, which is what the medium does. */
struct kal_io_result kal_datagram_recv_from(struct kal_datagram, void*, kal_uintptr len,
                                            struct kal_endpoint* from);

void kal_datagram_close(struct kal_datagram);

extern const kal_uintptr kal_datagram_props;

#ifdef __cplusplus
}
#endif
#endif
```

#### `openkal/space.h`

> ⚠️ **本草案已被 §7.5 ⓵ 取代**:收敛成 `kal_space_start` 一个操作,`kal_space` 类型不存在。下面保留的是当时提出的形状。


```c
/* openkal.space --- an address space, and a context executing in one.
 *
 * ⚠️ THIS IS NOT `fork'. Clause 7.1 refuses to require the duplication of an
 * address space AND ITS EXECUTION STATE; what is here is the first half alone.
 * A context started in a cloned space begins at a function the caller names,
 * not at the instruction the caller was executing --- which is what lets this
 * be stated in a C application binary interface at all. A library above this
 * interface reaches `fork' by saving its own execution state before the clone
 * and restoring it in the new context; that is composition, and it belongs
 * above this line rather than in it.
 *
 * ⚠️ THERE IS NO `create'. An empty address space contains no code, so the
 * entry function a caller would name is not in it. A program that wants a
 * child with only what it grants uses kal_process_spawn_with. */
#ifndef OPENKAL_SPACE_H
#define OPENKAL_SPACE_H
#include "types.h"
#include "process.h"

struct kal_space { kal_uintptr h; };

/* Positions in kal_space_props. */
/* Whether a clone carries the handles the original holds. An environment whose
 * cloning primitive copies memory and not handles reports zero here, and a
 * library above it cannot reach POSIX fork semantics. Measured: this is the
 * case on at least one environment. */
#define KAL_SPACE_PROP_CLONE_HANDLES ((kal_uintptr)1u << 0)
/* Whether the copy may be completed lazily, so that a store to cloned memory
 * may fail after the clone reported success. A program that cannot tolerate a
 * deferred failure adapts; it cannot ask for the other behaviour. */
#define KAL_SPACE_PROP_DEFERRED_COPY ((kal_uintptr)1u << 1)

#ifdef __cplusplus
extern "C" {
#endif

int kal_space_clone(struct kal_space* out);   /* clones the CALLER's space */

/* Starts a context in the given space. The result is a process: a context with
 * an address space of its own is what that word means, and `kal_process_wait',
 * `kal_process_terminate' and `kal_process_close' therefore apply unchanged. */
int kal_space_start(struct kal_space, void (*entry)(void*), void* arg,
                    void* stack_top, struct kal_process* out);

/* Releases the space. A space whose context is running is not released until
 * that process ends; the two handles are independent and either may be closed
 * first. */
void kal_space_destroy(struct kal_space);

extern const kal_uintptr kal_space_props;

#ifdef __cplusplus
}
#endif
#endif
```

#### `openkal/process.h` 的三处增补

```c
/* --- added --- */

/* One directory a started program shall receive among its preopens. The layout
 * is frozen (clause 5.3). */
struct kal_preopen {
    struct kal_dir dir;
    const char*    name;
    kal_uintptr    len;
};

/* Starts a program that receives exactly the directories named. The program
 * reads them back through kal_fs_preopen, which is the operation this one is
 * the inverse of --- clause 7.11.
 *
 * ⚠️ A SECOND DECLARATION RATHER THAN AN ARGUMENT ADDED TO THE FIRST, because
 * clause 8 forbids altering an existing one. kal_process_spawn remains, and a
 * program that does not grant directories keeps using it. */
int kal_process_spawn_with(struct kal_dir base,
                           const char*  path, kal_uintptr path_len,
                           const char** argv, const kal_uintptr* argv_lens, kal_uintptr argc,
                           const char** envp, const kal_uintptr* envp_lens, kal_uintptr envc,
                           const struct kal_spawn_streams* streams,
                           const struct kal_preopen* grants, kal_uintptr grant_count,
                           struct kal_process* out);

/* A pair of streams of which one end crosses a spawn boundary. The parent
 * holds `mine'; `theirs' is what it places in a kal_spawn_streams.
 *
 * ⚠️ BOTH ENDS ARE OWNED. A parent that does not release `theirs' after the
 * spawn never observes the end of input on `mine' --- the classic deadlock of
 * this arrangement, and the reason an interface that hands out these streams
 * must also take them back. */
int  kal_process_channel(struct kal_stream* mine, struct kal_stream* theirs);
void kal_process_channel_close(struct kal_stream);

/* --- added positions in kal_process_props --- */
#define KAL_PROCESS_PROP_CHANNEL   ((kal_uintptr)1u << 3)
#define KAL_PROCESS_PROP_GRANT_DIR ((kal_uintptr)1u << 4)
```

#### `openkal/types.h` 的两处增补

```c
/* A byte. The three fixed widths above are the ones openkal's OPERATIONS use;
 * an address is the first datum openkal must state byte by byte. */
#if defined(__UINT8_TYPE__)
typedef __UINT8_TYPE__ kal_u8;
#elif defined(_MSC_VER)
typedef unsigned char  kal_u8;
#endif

/* Where a connection or a message goes. The layout is frozen (clause 5.3).
 *
 * ⚠️ HERE AND NOT IN net.h, because `openkal.datagram' uses it and either
 * interface may be provided without the other.
 *
 * ⚠️ BYTES AND A LENGTH, not a tagged union of families. The length is what
 * distinguishes them, and it is a VALUE rather than a layout --- so the set of
 * lengths this specification defines may grow while clause 5.3 holds the
 * structure fixed. That is the same evolution rule clause 6.2 gives a property
 * word, applied to a value instead of to a bit.
 *
 * An implementation refuses a length it does not know (kal_err_invalid) rather
 * than reading it as one it does. Twenty-four bytes is chosen so that an
 * address with a scope identifier fits without a second type. */
struct kal_endpoint {
    kal_u8      addr[24];   /* network order --- the bytes of the address itself */
    kal_uintptr addr_len;   /* 4 = IPv4  16 = IPv6  20 = IPv6 with a scope
                             * identifier; further values are defined by later
                             * revisions and refused, not misread, by an
                             * implementation that does not know them. */
    kal_u32     port;       /* a number, in host order: this interface does not
                             * ask a caller to perform a protocol's byte order
                             * conversion. */
};
```

---

### 2.13 ⭐⭐ 规范 / ABI 级整体 review:六处发现

写草案的过程本身是一次 review。逐条:

#### ⓵ ⭐⭐ 错误集是**封闭**的,而超时需要一个错误 —— 而它已经有了

`types.h` 写着:*"The set is closed: an implementation maps its environment's
error values onto these and **does not extend them**."* ⇒ **不能加 `kal_err_timeout`。**

⭐ 而 `kal_err_again` 的定义正是 *"the operation would block"* —— **一次超时的到期,
说的就是这件事。** ⇒ **复用它,零改动。**

⚠️ 但必须在规范文本里写死:在带超时的操作上,`kal_err_again` **只**表示到期。
否则「到期」与「本来就会阻塞」在一个不阻塞的调用上会同读数。

#### ⓶ ⭐⭐ `types.h` 里**没有字节类型**

现有四个类型是 `kal_uintptr` / `kal_u32` / `kal_u64` / `kal_i64` ——
它们是**操作的**类型。而一个地址必须**逐字节**陈述。
⇒ **加 `kal_u8`**,并在注释里说明它与那三个的区别(见 §2.12)。

⚠️ 用 `unsigned char` 而不加类型也可以,但 types.h 的注释说得很清楚:
「一个决定写在八个地方」正是它存在的理由。**加类型与既有理由一致。**

#### ⓷ ⭐⭐⭐ `deadline` 这个名字与 `kal_task_wait` 不一致 ⇒ 改名 `timeout`

`kal_task_wait(word, expected, **timeout_ns**)` —— **相对时长**,且
**零表示不设超时**。

⚠️ 我原来叫它 `deadline` 并想用**绝对时刻**。⭐ **两处都要改**:
名字改成 `openkal.timeout`,参数改成相对的 `timeout_ns`,零的含义与 `task_wait` 相同。

> **同一个概念在一份 ABI 里必须只有一种拼法。** 这是本规范最一贯的一条,
> 而我差点在它上面留下第二种。

⚠️⚠️ **代价必须写下来**:零被「不设超时」占用了 ⇒ **无法表达「立即返回,不等」**
(即 `poll(timeout=0)` 的非阻塞探测)。
⭐ 这是「逐字一致」与「表达力」的真实冲突,而一致性优先。
调用方要非阻塞探测,传 `1`(纳秒)—— 丑,但**不引入第二种拼法**。
⇒ 这一条应当写进 clause 6.3「考虑过而未采纳」。

#### ⓸ ⭐ `props` 有两种形式,而我差点全用错

现有头文件的分法是**严格的**:

| 形式 | 用在 | 例子 |
|---|---|---|
| **函数**,取资源 | 属性**随资源**变化 | `kal_stream_props(s)` —— 终端与文件答案不同 |
| **`extern const kal_uintptr`** | 属性**随实现**变化 | `kal_task_props` / `kal_fs_props` / `kal_process_props` |

⇒ 六个新接口的归属:

| 接口 | 形式 | 理由 |
|---|---|---|
| `terminal` | **函数** | 一个 pty 能报尺寸,一条串口不能 —— **同一实现两种答案** |
| `net` / `datagram` / `space` | **`extern const`** | IPv6 支持、克隆是否带句柄 —— 是实现的性质 |
| `timeout` | **`extern const`** | 粒度是时钟的性质。⭐ 而且它不是位字段而是一个**数值**(`kal_timeout_granularity_ns`)—— 与其他 props 形状不同,要在规范里说明 |

#### ⓹ ⚠️ `kal_endpoint` 布局冻结 ⇒ 三个字段永远加不进去

clause 5.3:结构体布局不可变。⇒ IPv6 的 **scope id**(链路本地地址需要)、
**flow label**、以及任何非 IP 的地址族,**定稿之后永远加不进去**。

⭐ **我的裁决是不带**,理由写在草案注释里:携带一个协议族的每个字段,
就变成了那个协议族的接口而不是一个地址。
⚠️ **但这是一条不可逆的决定,必须由你确认** —— 链路本地 IPv6(`fe80::/10`)
没有 scope id 是**用不了**的,而嵌入式网络里它并不罕见。

#### ⓺ ⭐ 三处小的但会咬人的

| | |
|---|---|
| `kal_space_clone` 克隆的是**调用者自己的**空间 | 没有第二个空间可克隆(要有就得先有 `create`,而 §2.8 ⓵ 删掉了它)⇒ **签名里没有 src 参数**,草案已改 |
| `space` 与 `process` **两个句柄同时存在** | 谁先释放?⇒ 草案定为**独立,任一先释放皆可**,并写进注释 |
| 32 位目标上 `kal_u64` 参数占两个寄存器 | `kal_timeout_recv_from` 有 5 个参数 ⇒ riscv32 上会溢出到栈。**性能,不是正确性** ⇒ 记录,不改 |

#### 符号规模

SURFACE.txt 从 **~40** 增到 **~68**。`check-surface.sh` 的成本不变(它是逐行比对)。
⚠️ **但每个后端都要重新回答一次「提供还是不提供」** —— 而那正是 clause 6.1
要求的、且今天没有任何机制强制的事。⭐ **`--complete` 那一侧必须在新接口落地时
一起被使用**,否则一个后端「忘了不提供」会以链接期缺席的形式混过去。

---

### 2.13' ⭐⭐⭐ `kal_endpoint` 的可扩展性:四个方案,第四个不需要保留字节

§2.13 ⓹ 把它记成「不可逆,要确认」。**再想一轮,它不必是不可逆的。**

#### ❌ 方案一:保留字节

```c
struct kal_endpoint { kal_u8 addr[16]; kal_uintptr addr_len; kal_u32 port;
                      kal_u32 reserved[N]; /* must be zero */ };
```

| ⚠️ | |
|---|---|
| **「必须为零」没有人保证** | C 不自动清零。一个忘了初始化的调用方,会在 reserved 被启用的**那一天**突然改变行为 |
| **N 取多少是一个猜** | 取少不够,取多每个 endpoint 都在浪费 |
| **它与 openkal 每一条规则相反** | clause 7.8 要求「说出整个意图」;保留字段是「我还没想好」的具体化 |
| 先例是反面的 | `sockaddr_storage` / `OVERLAPPED` 的保留字段被创造性地使用过 |

⇒ **保留字节能工作,但它把一个「以后再说」冻进了 ABI。**

#### ❌ 方案二:不透明字节 + 长度

调用方无法构造一个 endpoint ⇒ 需要构造函数 ⇒ 又回到接口里;且丢掉类型检查。

#### ⚠️ 方案三:版本化的类型

clause 5.3 冻结的是**已有类型的布局**,新增一个类型是允许的(clause 8)。
⇒ 将来出 `kal_endpoint2` + `kal_net_connect2`。
**诚实且有先例**(clause 7.8 让 `kal_fs_open_file` 与 `kal_fs_open` 并存),
⚠️ 但每次演进要把操作复制一遍。**留作最后手段。**

#### ⭐⭐⭐ 方案四:变长地址 —— **可扩展的是取值,不是布局**

关键在于把 scope id 看成**地址的一部分**,而不是 endpoint 的**另一个字段**。

```c
struct kal_endpoint {
    kal_u8      addr[24];   /* 网络序，就是地址本身的字节 */
    kal_uintptr addr_len;   /* 4=IPv4  16=IPv6  20=IPv6+scope  其余由 props 定义 */
    kal_u32     port;       /* ⭐ 主机序数值，见下 */
};
```

⭐⭐ **它不是「保留字节」,区别是决定性的:**

| | 保留字节 | 变长地址 |
|---|---|---|
| 那些字节今天的含义 | ❌ **未定** —— 谁也不知道该填什么 | ✔ **地址的长度就是它的长度**,超出 `addr_len` 的字节不被读 |
| 扩展时改的是 | ⚠️ **布局的解释**(clause 5.3 的边缘) | ✔ **`addr_len` 的取值集合** —— 一个值,不受 clause 5.3 约束 |
| 旧调用方 + 新实现 | ⚠️ 取决于它有没有清零 | ✔ `addr_len=16` 照常工作 |
| 新调用方 + 旧实现 | ⚠️ 静默地错 | ⭐ `addr_len=20` 被拒(`kal_err_invalid`)—— **一个诚实的失败** |

⭐ **而这个形状 openkal 到处都在用**:每一个字符串都是 `const char* + kal_uintptr len`。
**地址是同一形状**,不是一个新概念。

⭐ 并且它与 clause 6.2 的属性字**同构**:位可以增长,未分配的读作零;
这里是**取值**可以增长,不认识的被拒绝。⇒ **同一条演进规则的第二个实例。**

#### 数组开多大

| 用途 | 字节 |
|---|---|
| IPv4 | 4 |
| IPv6 | 16 |
| IPv6 + scope id | **20** |
| 非 IP(Bluetooth BD_ADDR 6 / CAN 4 / mesh 8) | ≤ 8 |

⇒ **24**。`24 + 8 + 4` 对齐后 40 字节 —— 传指针,不进寄存器,代价可忽略。

⚠️ **地址族怎么区分?** 一个 6 字节地址是 Bluetooth 还是别的?
⭐ **今天不需要区分**:clause 10 规定**每个接口一个实现** ⇒ 一个程序里只有一个网络栈,
而**那个栈知道它的地址是什么**;`kal_net_props` 说明它支持哪些长度。
⇒ 一个同时支持 IP 与非 IP 的栈是将来的问题,**而 24 字节的余量让将来有得选**
(例如用 `addr_len` 的高位编码族)。**这正是不必现在就不可逆的意思。**

#### ⭐ 顺带一处必须定死的:字节序

| 字段 | 序 | 理由 |
|---|---|---|
| `addr[]` | **网络序** | 它就是地址本身的字节,不是一个数值 |
| `port` | ⭐ **主机序数值** | 它是一个数,而 openkal 不让调用方做协议的字节序转换 —— 那是实现的事 |

⚠️ **不定死的话每个实现会各猜一次**,而两边猜反了的症状是「连到了错的端口」,
在一个能连通的网络里可能很久才被发现。

#### 裁决

**采纳方案四。** ⇒ §2.13 ⓹ 那条「不可逆,要确认」**降级为一条普通的定形决定** ——
不带 scope id 的是**今天的取值集合**,不是这个类型的能力上限。

⚠️ 仍然不可逆的只剩一条:**`addr[24]` 这个数字**。24 是从上表推的,
若将来出现更长的地址族,只能走方案三。⭐ 但那时的代价是**一个新类型**,
而不是**一个已经发布的类型说不出话**。

---

### 2.14 SPEC 增补文本(clause 6.3 / clause 11)

#### clause 6.3「Mechanisms considered and not adopted」增两条

> **Readiness notification.** An interface reporting that a stream may be read,
> by waking a word as `kal_task_wake` does, was considered as the remedy for a
> context that would otherwise wait without end. It composes better than the
> bound this specification adopted: one operation covers every waitable thing,
> a single context may await many sources, and a library above it needs no
> read-ahead buffer because a notification consumes nothing.
>
> It was not adopted because of what it asks of an implementation. On an
> environment whose readiness is discovered by polling a set of descriptors, an
> implementation would have to maintain that set and a context of its own to
> watch it — a mechanism reconstructed rather than a facility conveyed, which
> clause 7.1 excludes. The bound this specification adopts asks the same
> environment only for what it already does at the point of the call.
>
> **An instant rather than a duration.** `openkal.timeout` states a duration
> because `kal_task_wait` does. An instant would not accumulate drift when a
> caller retries in a loop, and was considered for that reason; it was not
> adopted because it would give one specification two spellings of one idea.
> A caller that requires an instant computes the remaining duration from
> `kal_time_monotonic`, and the cost of doing so falls on the caller that has
> the requirement rather than on every implementation.

#### clause 11「Matters this version does not settle」增六条

> 4. **Networking.** Not defined by this version. `openkal.net` and
>    `openkal.datagram` are anticipated by clause 3.4 and are separate for the
>    reason clause 6.4 gives.
> 5. **Readiness.** Awaiting one of several sources is not an operation of this
>    specification. It is reached above the interface, from `openkal.task` and a
>    bound upon each wait; clause 6.3 records the alternative that was weighed.
> 6. **Terminal control.** Not defined by this version. `KAL_STREAM_PROP_INTERACTIVE`
>    reports that a stream is one; no operation acts upon that fact.
> 7. **Permission and ownership.** Not defined, and not a deferral: a permission
>    presupposes an identity, and the environments this specification targets do
>    not agree that one exists. A C library above openkal reports the absence as
>    the error its own surface defines.
> 8. **Creation and reading of links.** Not defined, and not a deferral, for the
>    reason clause 6.4 gives: whether a filesystem has links is a property of the
>    format rather than of the environment. `KAL_FS_PROP_LINKS` reports it;
>    resolution follows one where the property is claimed.
> 9. **Duplication of the calling image.** `fork` is refused by clause 7.1 and
>    that refusal stands. ⚠️ It is refused as an OPERATION: the atomic
>    capabilities from which a library may compose it — cloning an address space,
>    starting a context in one — are a candidate for a later version and are not
>    excluded here. A sentence reading "openkal will not have fork" would bury
>    them, and this clause exists so that it is not written.

⚠️ **第 9 条的最后一句是本节最重要的一句** —— 它防的是我自己犯过两次的那个错
(§1.2 / §1.3)。

---

## 3. openkal-musl(端口层)—— 七条,是本轮工作量的主体

### 3.1 ⓪ 定位 PC=0(**先做,而且是定位不是修**)

⭐ **排第一不是因为最严重,是因为它让其余每一条的读数都不可信** —— 一个在早期
初始化就 SIGSEGV 的程序,后面 26 条失败都是它的下游。

**已由源码收窄的两条**(不必再查):

- `okm_syscall.c` 的 `default:` 返回 `-ENOSYS`(71 个 `case` + 一个 default)
  ⇒ **不是未知系统调用走空指针**
- 端口层一共只有**两处**弱引用(`grep '__attribute__((weak))' port/src` = 2 行:
  `okm_phdr.c:39` 的 `__ehdr_start`、`okm_syscall.c:31` 的 `kal_random_fill`),
  **两处都判空了** ⇒ 不是 [[link-error-is-the-mechanism-not-the-defect]] 第二形态的复发

**三个仍开着的候选,各配一条决定性检查**:

| 候选 | 检查 |
|---|---|
| `.init_array` 走过头或含 0 项(`okm_start.c:179` 的循环**逐项不判空**) | `readelf -x .init_array <bin>`;临时打印循环的 `a` 范围 |
| 程序自己 `dlsym` 得 0 就调用(静态 musl 的 `dlsym` 恒返回 0) | `nm <bin> \| grep dlsym`;`gdb` 在 `dlsym` 下断 |
| ⭐ **终端初始化**:`ioctl` 只答 `TCGETS`,`TCSETS`/`TIOCGWINSZ` 都 `-ENOTTY` ⇒ TUI 拿到失败后的路径 | **先排除这一条** —— 崩溃位置与它重合 |

⚠️ **判据的单位是一整行输出**:`bt` 空的时候 `info registers rip rsp` 与
`x/8gx $rsp` 不空。⚠️ **不要凭报错跳到修法** ——
[[reasons-written-from-memory-kill-good-fixes]];这里连报错都没有,只有一个空栈。

### 3.2 ① `copy_file_range` —— **只需这一个**

⭐ 实测 libc++ 源码(`libcxx/src/filesystem/operations.cpp`):
`copy_file` 对 `copy_file_range` 的回落名单**含 ENOSYS**(`:314`),
对 `sendfile` 的**只认 `EINVAL`** ⇒ ENOSYS 走不到 `fstream` 回落。

⇒ **实现 `copy_file_range` 一个就够,`sendfile` 是多余的**(实现了 range 就永远到不了
sendfile)。实现成 `kal_stream_read`→`kal_stream_write` 循环。
⚠️ 短写不是成功结果([[openkal-spec-and-linux-impl]]),循环必须写全或报错。

**量级**:~40 行。

### 3.3 ② 替换 `popen.c` —— 替换源码,不重建机制

`okm_spawn.c` 的开篇已经论证过这个动作:

> openkal has neither operation … What openkal has instead is **the composite** …
> so the replacement is a **translation of arguments rather than a reconstruction
> of a mechanism** — and it is shorter than the code it replaces.

`posix_spawn.c` 已经在排除表里(`mcpp.toml:118`),`popen.c` **不在** ⇒ 它还在用
`pipe2` ⇒ ENOSYS。

**两阶段**:

| 阶段 | 实现 | 语义差 |
|---|---|---|
| 现在(`kal_process_channel` 落地前) | `kal_fs_open`(`CREATE\|EXCLUSIVE\|TRUNCATE`)开临时文件当子进程 stdout,`kal_process_wait` 后 seek 回 0 | ⚠️ 不能并发交错;需要可写文件系统 |
| `channel` 落地后 | 直接用 `kal_process_channel` | 语义补齐 |

⚠️⚠️ **这不是 `pipe2`,不要假装是。** 三处语义差必须写进 README。
⭐ **正因为差,才必须实现成 `popen` 而不是实现成 `pipe2`** —— 一个假的 `pipe2` 会让
self-pipe 的调用方**静默地永远等下去**,这正是
[[c-library-configured-by-what-is-beneath]] 里 futex 那条踩过的形状。

ⓘ **musl 的 `system()` 走 `posix_spawn`,所以它今天就能用** —— 「不能 shell out」的
范围比它看起来窄,值得在回复里说清楚。

**量级**:~120 行。

### 3.4 ③ `last_write_time(dir)` —— clause 7.12 的保留名

clause 7.12 的 `"."` 就是为这件事存在的(*"a program holding a directory has no way
to ask an operation about that directory"*)。⇒ `stat`/`statx`/`newfstatat` 对目录路径
应走 `kal_fs_open_dir` + `kal_fs_info(dir, ".")`,而不是按文件路径走 `kal_fs_open`。

**量级**:~20 行。

### 3.5 ④ `umask` 去静默 —— 本轮最硬的一条正确性缺陷

**实测**:`g_umask` 只被 `SYS_umask` 自己读写(`okm_syscall.c:388` 定义、`:937` 使用),
**从不作用于任何创建**;而 `kal_fs_open` 本来就没有 mode 参数。

⇒ `umask(077)` 返回旧值、报成功、**下一次创建不受影响**。这正是 `okm_opt.h` 开篇
明令禁止的那一种:

> ⚠️ THE ONE WAY THIS COULD GO WRONG IS NOT PRESENT: **nothing below reports
> SUCCESS having done nothing.**

⭐ **它比「0600 落成 0666」那条更硬** —— 后者要论证具体值,这条不需要:
**一个有返回值的调用,它的效果不存在。**

**处置**:删掉 `g_umask`,让 `SYS_umask` 落进 `default:` 的 `-ENOSYS`。
⚠️ musl 的 `umask()` 会因此返回 -1/ENOSYS,调用方多数不检查 —— **这是对的**:
它此前得到的成功是假的。README 记一条。

**量级**:~10 行(删)。

### 3.6 ⑤ ⭐⭐ 「永远没有」变链接错误 —— 零设计成本,回报最大

**问题**:今天 `okm_syscall.c` 用同一个 `-ENOSYS` 回答了两件性质不同的事。

| | 例子 | 正确的回答时机 |
|---|---|---|
| **这个后端不提供** | core-only 后端上的 `open` | **运行期 ENOSYS** ✔ `okm_opt.h` 那道缝做对了 |
| **openkal 根本没有这个接口** | `fork` / `socket` / `poll` / `chmod` | ⭐ **链接期** —— clause 6.2 的表就是这么规定的 |

第二类是**永久的、与后端无关的**事实。ENOSYS 把它伪装成了运行期条件,于是
`mcpplibs/tinyhttps`(**mcpp 自己索引里的包**)在 openkal 上**构建成功**,
跑到第 27 个测试才失败。

**做法**:`mcpp.toml` 的 `sources` 增排除项。**机制全部现成**:

- `!` 排除语法已在用(今天已排除 10 个 musl 源)
- cflags 已有 `-ffunction-sections -fdata-sections`,ldflags 已有 `-Wl,--gc-sections`
  (`mcpp.toml:240`)⇒ ⭐ **没被引用的不会失败,被引用的才失败** —— 粒度正好

**候选排除集合**(⚠️ 需一轮闭包实测):

```
!musl/src/network/**          socket 全族 + getaddrinfo
!musl/src/select/**           poll / select / epoll / eventfd / signalfd
!musl/src/process/fork.c      以及 vfork / _Fork
!musl/src/unistd/pipe*.c      pipe / pipe2
!musl/src/stat/chmod.c        chmod / fchmod / fchmodat
!musl/src/unistd/symlink*.c   symlink / symlinkat
```

⚠️ **闭包是这条的唯一风险**:musl 的 `network/` 内部互相引用
(`getaddrinfo` → `socket`),排一个可能牵出一串。**这是实现问题,不是设计问题** ——
判据是「排完之后 `mcpp build` 全绿」。

⚠️ **`timerfd`/`eventfd` 要不要一起排?** 它们在 `select/` 之外。
⭐ 建议**一起排**:一个「创建了却永远不会就绪」的 fd 比 ENOSYS 更坏。

**判据(两向,缺一不可)**:

| 方向 | 断言 |
|---|---|
| 正 | 一个引用 `socket` 的程序**链接失败并指名 `socket`** |
| 反 | 一个**不**引用它的程序**正常链接** —— 否则就是把所有人都打死了 |

### 3.7 ⑥ 就绪 `poll`/`select` —— 端口层设计,不动 spec

**为什么在这里而不在 openkal**(⭐ 更准确地说是在 §2.7 的 `openkal.kit.wait` 里,由所有端口层共用):可由 `openkal.task`(`kal_task_wait/wake`,
`okm_opt.h:135`,端口的 `SYS_futex` 就走它)+ `openkal.stream` 的阻塞读组合出来 ——
每源一个辅助 task 做阻塞读,读到就唤醒主 task。**可导出 ⇒ 是库。**

⚠️ **代价是真的,必须写下来**:「就绪」不等于「已读」,而 `kal_stream` 读了退不回去
⇒ **端口层必须为每个可 poll 的 fd 做预读缓冲**。

- 它已经拥有 fd 表(`okm_desc`,`okm.h`)⇒ 缓冲有地方放
- stdio 本来就在做预读 ⇒ 不外来
- ⚠️ 但 `read` 必须改为**先取缓冲**,这动的是热路径

⚠️ **未测的两条**:
(a) 预读缓冲对 `poll` 语义的覆盖度 —— 能不能撑住 ftxui 的用法;
(b) 一个 `poll` 过、随后被传给别人(dup / 交给子进程)的 fd 怎么办。

**量级**:大,**需要一次设计**,是本批唯一需要设计而非实现的一条。

### 3.8 ⑦ `private_include_dirs`(等 mcpp)

`port/include/features.h:70` 自己写下了第一优解和它为什么做不到:

> ⓘ **THIS IS THE SECOND-BEST REMEDY.** The first would be for a package to
> distinguish the directories it is **built from** from the directories it
> **publishes**. Measured 2026-08-22: **mcpp cannot express it.**

mcpp 侧改动见配套文档 §4。此处只需在 `mcpp.toml` 把三个内部目录移过去:

```toml
include_dirs         = ["port/include", "musl/include"]
private_include_dirs = ["musl/src/include", "musl/src/internal",
                        "musl-generated/internal"]
```

⚠️ **顺序必须与 `include_dirs` 交错保持声明序** —— `features.h` 那段实测说得很清楚:
挪到后面,musl 自己的构建会先找到公共 `<features.h>` 而失败。

⭐ **判据不是「`hidden` 不再泄漏」,是「那三个目录不出现在消费者的命令行上」** ——
前者会随着包侧再打一个补丁而变绿,而缺陷还在。

### 3.9 ⑧ POSIX 验收套件 —— **长期回报最高的一条**

**缺口**:openkal 的 conformance 验的是 **openkal 自己的 `kal_*` 面**;
**没有任何东西验 openkal-musl 重建出来的 POSIX 面。**

⇒ 本轮五条缺陷它抓五条(⓪ / copy_file / popen / last_write_time / umask),
**在用户之前**。

**设计要点**:

| | 要求 |
|---|---|
| 分母 | ⭐ 判据是 `67 passed / 94`,不是「套件跑过了」([[criterion-whose-no-is-also-silence]]) |
| 三态 | 每个用例声明**应当成功** / **应当失败且 errno 是 X** / **不适用于本后端**。⚠️ 只有两态会让「没测成」和「不支持」同读数 |
| 矩阵 | 每 backend × 每 arch,**真跑**(qemu),不是只链接 —— 本轮全部三类问题都在链接之后 |
| 种子 | ⭐ **报告者的 94 个用例**,他已经在 issue 里提出愿意提供 |

⚠️ **它必须能变红。** 一个新增的、故意不被实现的用例必须让套件失败,否则这个套件
就是 [[openkal-spec-and-linux-impl]] 记的那个「第一版 surface 检查器是空转的」。

---

## 4. openkal-llvm-runtime —— `__config_site` 与实际能力对账

### 4.1 实测的三行(§配套文档 §6.7 表 C)

| 开关 | generic | 实际 | 处置 |
|---|---|---|---|
| `_LIBCPP_HAS_RANDOM_DEVICE` | 1 | ✅ 符合 | **0.1.3 已修**(报告者测的是 0.1.1) |
| `_LIBCPP_HAS_FILESYSTEM` | 1 | ⚠️ 部分 | **保持 1** —— `<filesystem>` 每个操作都有 `error_code` 重载,标准把它设计成允许失败;Windows FAT 上 `create_symlink` 同样失败 |
| `_LIBCPP_HAS_TERMINAL` | 1 | ❌ **不符** | ⭐ **改成 0**,直到 `openkal.terminal` 落地 |

⭐ **为什么 FILESYSTEM 留 1 而 TERMINAL 改 0** —— 这不是尺度不一,是标准的态度不同:

> **标准把这件事设计成「可以失败」的 ⇒ 声明有,失败时报错是对的。**
> **标准假定它「一定成功」/ 用开关表达有没有 ⇒ 没有就必须写 0。**

### 4.2 ⭐ 对账要成为一条 CI 判据,而不是一次检查

`__config_site` 是**一份对下层能力的声明**,而今天**没有任何东西核对它**。
三行里有两行曾与实际不符,而它们都是**编译期**写死的 —— 程序因此把 `<filesystem>`
和终端支持整个编进去,再在运行期一条一条撞 ENOSYS。

⇒ 每个开关配一个**最小探针**,与 §3.9 的验收套件同批跑:
`_LIBCPP_HAS_TERMINAL` ⇒ 一个 `tcgetattr`+`tcsetattr` 的探针;
`_LIBCPP_HAS_RANDOM_DEVICE` ⇒ 构造一个 `std::random_device` 并取一个值。

⚠️ 未测:`_LIBCPP_HAS_LOCALIZATION` / `UNICODE` / `WIDE_CHARACTERS` /
`TIME_ZONE_DATABASE` 四行,本轮没有验过。

---

## 5. openkal-linux 与其它 backend

### 5.1 实测:openkal-linux 是**满的**

| backend | abort | stream | memory | env | time | fs | process | task | random | exec |
|---|---|---|---|---|---|---|---|---|---|---|
| **openkal-linux 0.5.4** | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| openkal-macos | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| openkal-windows | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| openkal-opensbi | ✅ | ✅ | ✅ | ✅ | ✅ | — | — | — | — | — |
| openkal-uefi | ✅ | ✅ | ✅ | — | — | — | — | — | — | — |

⭐⭐ **十个接口一个不缺 ⇒ 本轮的缺口不在 backend,在接口集合本身。**
这条读数把「让 openkal-linux 更完整」整条路排除掉了,值得单独记。

### 5.2 新接口落地时,每个 backend 的答案

⚠️ **一个接口新增,五个 backend 都要**明确回答**「提供还是不提供」** ——
clause 6.1 的缺席是靠**不导出符号**表达的,不是靠不写。

| 接口 | linux | macos | windows | opensbi | uefi |
|---|---|---|---|---|---|
| `openkal.terminal` | ✅ termios | ✅ termios | ✅ Console API | ⚠️ UART:关行编辑=无事可做即完成;尺寸=不知道 | 同左 |
| `kal_process_channel` | ✅ pipe2 | ✅ | ✅ CreatePipe | ❌ 无 process ⇒ 整个 `openkal.process` 本就不提供 | ❌ |
| `openkal.net` | ✅ sockets | ✅ | ✅ Winsock | ⚠️ 需 BSP 带 lwIP ⇒ **默认不提供** | ⚠️ UEFI 有 TCP protocol,**未测** |

### 5.3 openkal-linux 的一条待议:`openat(..., 0666)` 硬编码

`src/fs.cpp:127` 硬编码 `0666`,`:236` 硬编码 `0777`。

⚠️ **在 openkal 没有权限模型的前提下,这不是缺陷** —— 实现必须选一个值。
⭐ 真正的缺陷是端口层的 `umask` 静默(§3.5)。

⚠️ **未测**:报告者报的「文件 0600 落成 0777」我复现不出机制 ——
`openat(...,0666)` 给不出 0777。**权限被放宽这件事成立;放宽到哪个值未定。**
⇒ §3.9 的验收套件里应当有一条用例把它量出来。

---

## 6. 分批与依赖

| 批 | 内容 | 仓库 | 前置 |
|---|---|---|---|
| **P0** | ⓪ 定位 PC=0 | musl / linux | — |
| **P0** | ⑤ **「永远没有」变链接错误** | musl(manifest) | — |
| **P0** | ④ `umask` 去静默 | musl | — |
| **P1** | ① `copy_file_range` · ③ `last_write_time(dir)` | musl | ⓪ |
| **P1** | ② `popen`(临时文件阶段) | musl | ⓪ |
| **P1** | `_LIBCPP_HAS_TERMINAL` → 0 | llvm-runtime | — |
| **P1** | clause 11 增补六条 | openkal | — |
| **P2** | ⑧ **POSIX 验收套件** | musl + CI | P0/P1(否则读数全是 ⓪ 的下游) |
| **P2** | ⑥ 就绪 `poll`/`select` | musl | 需设计 |
| **P2** | ⑦ `private_include_dirs` | musl | **mcpp 侧 C** |
| **P3** | `openkal.terminal` + 五个 backend | openkal + 5 | 设计定形 |
| **P3** | `kal_process_channel` + backend | openkal + 3 | 设计定形 |
| **P4** | `openkal.net` + 两个形态不同的实现 | openkal + 2 | ⚠️ 数据报与定形者两处未决 |
| **P4** | `openkal.space` + linux 实现 | openkal + linux | ⭐ **设计与 P3 同批做**,因为它决定 `spawn` 怎么被理解 |
| **P4** | ⭐ **句柄的跨空间传递**(独立立项) | openkal | ⚠️ `space` 只是第一个撞上它的;在它有答案前 `kal_space_start` 只传流 |
| **P2–P3** | ⭐ `openkal-kit`(非规范,openkal 仓库内的独立包,§2.7) | openkal 仓库 | 就绪/流对随 §3.7 一起落;`fork` 助手随 `space` 落 |

⭐ **P0 三条零设计成本,而 ⑤ 的回报最大** —— 它把发现从运行期移到链接期,
让「范围」这件事对每一个用户可见,而不只是对报告者。

⚠️ **P2 的验收套件必须排在 P0/P1 之后** —— 在 ⓪ 修好之前跑它,读数全是 ⓪ 的下游
([[openkal-portable-program-findings]]:一条流水线按顺序失败时,后面每一条缺陷都被
藏住,而且第一条会被当成唯一一条)。

---

## 7. mcpp 侧的配合点

完整清单见配套文档 §6.9/§6.10.3。与本文直接耦合的只有两条:

| # | 内容 | 本文哪里用到 |
|---|---|---|
| **C** | `[build] private_include_dirs` | §3.8 |
| **D** | 目标侧报告在「由图供给的层收窄了上层的面」时说出来 | §3.6 的运行期一侧 —— ⚠️ 引擎不得硬编码 POSIX 名 ⇒ 由包说 |

---

## 7.5 ⭐⭐ 实施回填:写实现改掉了两处接口设计

本节是**落地之后**回填的。§2.12 的头文件草案里有两处,**读的时候没看出问题,写第一份
实现时第一次运行就暴露了**。两处都是**改规范而不是改实现**。

### ⓵ `openkal.space` 从两个操作收敛成一个

草案是 `kal_space_clone` + `kal_space_start`,调用者手里拿着一个 space 句柄。

⚠️ **本规范面向的环境里没有一个把这对当原语。** Linux 的 `clone` 是**一个动作**:
复制地址空间**并且**在副本里开始执行,没有「只做前一半」的形式。要求实现把两者分开,
它只能照样启动一个上下文、把它停在某个等待原语上、再自建一条信令通道告诉它跑什么 ——
clause 7.1 对此的判词是:**是规范的形状有错,不是实现有错。**

⭐ 分开的形式还有一个它答不上来的问题:调用者 clone 之后、start 之前改了自己的内存,
子上下文看见的是哪一份?一个操作没有这个问题。

⇒ `kal_space_start(entry, arg, stack_top, out)`,`kal_space` 类型与
`kal_space_clone` / `kal_space_destroy` 一并删除。被否掉的形式记进 clause 6.3。

### ⓶ `openkal.net` 交出的是**被拥有的连接**,流是从它借的

草案里 `kal_net_connect` 直接交出一个 `kal_stream` 并声明它是被拥有的。

⚠️ **那在 clause 7.2 下无法实现。** 规范要求「实现不得把已释放的句柄当作有效」,而
流句柄就是环境的传输操作所接受的东西 —— 在描述符系统上是一个**关掉就会被复用的数字**,
没有地方放代际。被拥有的句柄放得下,借来的流放不下。

⭐ `openkal.fs` 早就回答过同一个问题:文件被拥有,流经 `kal_fs_stream` 从它借,并随
文件一起释放。`openkal.net` 现在就是这个形状,措辞也照抄 —— 因为是同一个安排。

更直白的症状是第一次跑就出现的:

    FAIL: the bytes read are the bytes written
    FAIL: the peer observes end of input after a half-closure

Linux 实现把连接打进了句柄方案,而 `kal_stream_read` 把那个打包过的字当描述符用。

⇒ 新增 `struct kal_net_conn` 与 `kal_uintptr kal_net_stream(struct kal_net_conn)`;
connect / accept / peer / local / shutdown / close 全部改收连接;
`kal_timeout_accept` 交出的也是连接。

### ⓷ `_LIBCPP_HAS_TERMINAL` 的结论与 §4.1 相反

§4.1 说把它设成 `0`。⭐ **那只是「对一个坏掉的 port 的正确描述」** —— 它门控的是
`isatty`,而 `isatty` 之所以不工作,是端口层用 TCGETS 回答了 musl 用 TIOCGWINSZ 问的
问题。修端口层才是修法,声明本来就该是真的。

⇒ 端口层已修;`__config_site` 的声明保持 `1`,并在 openkal-llvm-runtime 的 CI 里加了
一条**对账**判据:判据是**关系**(pty 与 pipe 必须不同,且与系统 C 库同向),
两个方向都会红 —— 声明 1 而端口答不出、声明 0 而端口其实答得出,都判失败。

---

## 8. 未决与我可能错的地方

1. **§2.3 `openkal.net` 的数据报问题未决**,而它有连锁后果(DNS)。**不能边写边定。**
2. **§3.7 的预读缓冲未测** —— 能否撑住 ftxui 的用法,以及 dup/传递后的 fd 语义。
3. **§3.6 的排除闭包未测** —— musl `network/` 内部互相引用,可能牵出一串。
4. **§5.3 权限放宽到哪个值未定** —— 我复现不出 0777。
5. **§4.2 四行 libc++ 开关未验**(LOCALIZATION / UNICODE / WIDE_CHARACTERS /
   TIME_ZONE_DATABASE)。
6. ⚠️⚠️ **§1.2/§1.3 是我连续说错两次后的更正。** 第一次把 `fork` 写成「永远不可能
   的硬天花板」—— 判据二(能不能由原子能力组合出来)本来就该问到这一步,我没问。
   第二次把它降为「候选但不做」,给的三条理由里**两条是错的**:面的大小算错了
   (5 个操作,比 net 少),而「无 MMU 目标必然缺席」**根本不是反对理由 ——
   它就是 clause 6.1 的机制本身**。
   ⭐ 教训:**「这个目标做不到」在一个可组合的规范里不是排除理由,是缺席的答案。**
7. **§2.4 的三处未决(继承语义 / 与 spawn 的关系 / COW 是否承诺)必须动笔前定** ——
   clause 8 不允许事后改,而其中第一条的两个需求方向相反。
8. **我没有真的跑过一次报告者那样规模的工作区** —— 本文的实测都在等价的小工程上做的。

---

## 9. 相关记忆

[[link-error-is-the-mechanism-not-the-defect]] · [[c-library-configured-by-what-is-beneath]] ·
[[openkal-spec-and-linux-impl]] · [[openkal-portable-program-findings]] ·
[[second-instance-exposes-the-interface]] · [[criterion-whose-no-is-also-silence]] ·
[[reasons-written-from-memory-kill-good-fixes]] · [[build-program-advisory-channel]]
