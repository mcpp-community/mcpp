# openkal 设计方案:通用内核 ABI 规范

**状态**:0.4 已实施并发布。规范与接口模块位于
[`mcpplibs/openkal`](https://github.com/mcpplibs/openkal)(0.4.0),Linux 实现位于
[`mcpplibs/openkal-linux`](https://github.com/mcpplibs/openkal-linux)(0.4.0),
macOS 实现位于 [`mcpplibs/openkal-macos`](https://github.com/mcpplibs/openkal-macos)(0.2.0),
其上的 C 库位于 [`mcpplibs/openkal-libc`](https://github.com/mcpplibs/openkal-libc)(0.2.0)。

配套文档:

- [`2026-08-20-openkal-implementation-plan.md`](2026-08-20-openkal-implementation-plan.md)
  —— 0.1/0.2 的实施计划与结果。
- [`2026-08-20-openkal-completeness-plan.md`](2026-08-20-openkal-completeness-plan.md)
  —— 0.3 扩展至八个接口的计划与判据。
- [`2026-08-20-openkal-portable-program-findings.md`](2026-08-20-openkal-portable-program-findings.md)
  —— 0.4 的两条规范条款,及发现它们的可移植程序。

本文记录推导过程,包含被撤回的方案及其理由;规范正文(`openkal/SPEC.md`)只记录结论。
两者冲突时以规范正文为准。

证据来源分两类:标注为「实测」的结论在本机或持续集成上验证过,载荷版本随文注明;
其余为设计主张。调研出处见
[`2026-08-18-freestanding-baremetal-analysis.md`](2026-08-18-freestanding-baremetal-analysis.md)。

---

## 0. 多维度评估摘要

下表按八个维度给出当前状态。各维度的详细论证分散在正文对应章节。

| 维度 | 状态 | 依据 |
|---|---|---|
| **架构** | 接口按资源种类划分,而非按标准库的哪一部分得以启用划分;规范包拥有全部模块,实现不导出模块,因而实现无法扩展接口 | §2、§21 |
| **稳定性** | 结构体布局在 0.1 冻结;演进规则允许新增声明、禁止修改既有声明;0.4 新增两条行为规则而未改动任何声明,导出符号集合不变 | §16.3、§22 |
| **优雅简洁** | 0.2 起不存在可选操作,因而不存在表达可选性的机制;两套曾被构建的机制(能力位结构体、ADL 兜底重载)均已删除,理由见 §15 | §4、§15 |
| **用户体验** | 消费者 import 接口、不命名实现;更换实现是 manifest 中的一行;缺少实现时链接期报错并点名未定义的函数 | §7、§21.3 |
| **兼容性** | 消费者声明两条依赖,其中契约那条把版本不匹配从编译期的一批签名错误转为解析期的一条消息 | §4.4 |
| **跨平台** | 句柄为不透明的一个机器字,因而实现可位于 C 库之上、之下或不依赖 C 库;两个操作系统上的实现记录了四处分歧 | §1.1、§23 |
| **一致性** | 采用生态既有词汇:能力缺失在编译期表达,后端选择是条件依赖;引擎未新增任何轴,openkal 的实现未要求修改 mcpp | §14、§24 |
| **无感升级** | 0.1 引入两个包、不修改任何既有包;0.4 未改动声明,符合 0.3 的实现同样满足 0.4 的导出要求 | §22 |

---

## 1. 定位:一份规范,两个方向

openkal 是一份内核 ABI 规范,而非一个库。它有两类使用者,方向相反:

```
        应用 / libc / libc++ / Rust / ...
                    │  对上:消费者可以依赖什么
        ════════════╪════════════  openkal SPEC
                    │  对下:实现者必须提供什么
        Linux / Windows / 自行编写的内核 / 裸机 BSP
```

其关键性质是:openkal 不确定自身位于 C 库之上还是之下。宿主后端把
`kal_stream_write` 转发到 `::write(2)`,此时它位于 C 库之上;裸机后端直接写 MMIO,
此时它取代 C 库;picolibc 后端把 `FILE::put` 接到它上面,此时它位于 C 库之下。
三种配置都成立,且是同一份契约。

实测(调研 KA1/KA3):同一份 `app.cppm`,不含任何 `#if`,宿主侧转 `::write(2)`、
裸机侧转 MMIO UART;两侧 `app.o` 的外部符号集合完全相同,均只有 `kal_write`;
裸机侧无未定义符号,体积 157 字节。

### 1.1 该性质由什么承载

由句柄的不透明性承载。句柄一旦具有类型,openkal 就被固定在 C 库的某一侧:

| 句柄形状 | 强制的位置 | 后果 |
|---|---|---|
| `int fd` | C 库之下,须存在 fd 表 | Windows 实现须维护 fd→HANDLE 表,构成模拟层 |
| `FILE*` 或流对象 | C 库之上 | 裸机零 C 库配置与 Windows 均须构造 FILE |
| 不透明的一个机器字 | 不强制 | 每个实现存放其原生表示 |

---

## 2. 划分依据:资源种类

一条被否决的划分依据是:按标准库的哪一部分得以启用来切分接口。该依据耦合到 C++、
耦合到当前的标准库,且依赖方向相反 —— 内核 ABI 应当由内核提供什么来定义。

该错误此前出现过两代:POSIX 由 C 的 stdio 与 unistd 塑形,WASIp1 由 POSIX 塑形。

采用的划分依据是「交付的是哪一种资源」。该依据与语言无关,也是 WASIp2、seL4 与
Fuchsia 收敛到的形状。

「标准库的哪一部分得以启用」退回其应有的位置:D1 阶段的准入判据(见 §13.1),
而非划分依据。

### 2.1 接口清单

0.3 将接口从三个扩展至八个。下表中的判据一列回答:不具备该设施的实现能否提供该接口。

| interface | 资源 | core | 判据 |
|---|---|---|---|
| `openkal.abort` | 终止 | 是 | `for(;;) wfi` 是一个实现 |
| `openkal.stream` | 一条字节流 | 是 | 空接收端是一个实现 |
| `openkal.memory` | 一块内存区域 | 是 | 静态区域上的递增分配器是一个实现 |
| `openkal.env` | 环境提供的参数与变量 | 否 | 空向量是一个实现 |
| `openkal.time` | 一个时间源 | 否 | 不前进的计数器不是时钟,会使超时静默失效,构成模拟 |
| `openkal.fs` | 一个 descriptor,自有句柄类型 | 否 | 需要一组预置目录 |
| `openkal.process` | 一个被启动的程序 | 否 | 需要启动程序的能力 |
| `openkal.task` | 一个执行上下文 | 否 | 需要调度与上下文切换,属 openarch |
| `openkal.net` | 一个 socket,自有句柄类型 | 保留 | 需要协议栈 |
| `openkal.channel` | 一条消息通道 | 保留 | — |

`stream` 是共享的传输类型,而非统一入口:`fs` 的 descriptor 与 `net` 的 socket 各有
自己的句柄类型与操作,但都能产出 `openkal.stream`。向何处写入这件事,对文件、socket
与 UART 是同一套代码;打开它们不是。

### 2.2 core 的判别式:实现与模拟

判别式如下:

> 若一个虚假的实现会使上层静默地产生错误结果,则它是模拟;若它只是容量有限或会失败,
> 则它是实现。

`operator new` 失败在任何平台上都是有定义的结果,因此静态区域上的分配器是实现。
不前进的时钟会使 `wait_for` 立即返回,不随机的熵源会使密钥可预测,二者均是模拟。

由此,「这块 MCU 没有堆」是一个错误的命题:只要有 RAM,堆就可以被实现出来;
上层不关心 openkal 底层如何做到。

### 2.3 一个被起草后撤回的分解:`openkal.namespace`

草案曾以一个通用的 `openkal.namespace`(名字映射到资源)取代 `fs` 与 `net`,
理由是文件、TCP 连接、管道与串口交付的都是流,区别仅在命名方式。

重估后撤回,三条反对意见全部成立。

第一,它违反本文 §5.1 自己给出的判据。该判据为「一个操作若能存在但永远失败,
说明它被错误地合并了」。把文件与 socket 都归入一个 `stream`,则 `stream` 的能力集合
成为互不相干能力的并集(seek、size、truncate、sync 对 shutdown、peer、nodelay),
每个实现对其中大多数返回否定。这正是该判据所禁止的形状,只是换了位置出现。

第二,`namespace` 需要一个 URI 解析器,而解析器是模拟层。
`kal_namespace_open("tcp://…")` 要求每个实现都能解析 scheme:只提供 UART 的实现
也必须解析并拒绝 `tcp://`;合法 scheme 的集合无界且不可发现;错误以字符串形式表达。
这直接违反对下判据(四个实现均能自然实现、不需模拟层),且劣于 POSIX ——
POSIX 至少把 `open()` 与 `socket()+connect()` 分为两个类型化调用。

第三,所引先例有误。草案称该形状是 WASIp2 收敛到的形状,事实并非如此。WASIp2 的形状是:

```
wasi:io/streams        input-stream / output-stream   共享的传输资源
wasi:filesystem/types  descriptor                     自有资源类型,能产出 stream
wasi:sockets/tcp       tcp-socket                     自有资源类型,能产出 stream
```

它把资源种类分开,共享的是 stream 这一传输类型。草案把「共享 stream」误读为「统一命名」。

结论是保留正确的一半(流统一了传输),舍弃错误的一半(统一命名)。

| | namespace 草案 | 撤回后 | 单体 fs+net |
|---|---|---|---|
| 实现者 | 每个实现都需要 URI 解析器 | 不具备则不提供该 interface | 同左 |
| 消费者 | 错误是字符串;编译期无法得知是否支持 | `import openkal.net;` 缺失则编译期报错 | 同左 |
| 规范负担 | 需标准化 scheme 注册表,隐含接口面规模不受控 | 每个 interface 独立版本,接口面有界 | 接口大但有界 |
| 类型安全 | 能力集合成为不相干能力的并集 | 文件操作作用于文件句柄 | 同左 |

划分原则(按资源种类)本身没有错误,错误在于塌缩过度 —— descriptor、socket 与
stream 本就是三种资源。

网络本身不是设备,而网卡是。按基数划分(§12):网卡有 N 个,归 openhal;
协议栈有 1 个,归 openkal。

---

## 3. 核心 ABI 形状

```c
/* openkal —— C ABI(H1:跨包提供实现只有这一条路径)*/

typedef struct { uintptr_t h; } kal_stream;     /* 不透明,一个机器字 */

/* 标准流:借用而非拥有 —— 在裸机上关闭控制台没有意义 */
kal_stream kal_stdout(void);
kal_stream kal_stderr(void);
kal_stream kal_stdin (void);

/* 两字返回;标量部分须不超过一个机器字 */
typedef struct { uintptr_t n; int32_t e; } kal_io_result;

kal_io_result kal_stream_write(kal_stream, const void* buf, uintptr_t len);
kal_io_result kal_stream_read (kal_stream, void*       buf, uintptr_t len);
int32_t       kal_stream_flush(kal_stream);

/* memory */
void* kal_alloc  (uintptr_t size, uintptr_t align);
void  kal_free   (void* p, uintptr_t size, uintptr_t align);

/* abort */
_Noreturn void kal_abort(const char* msg, uintptr_t len);
_Noreturn void kal_exit (int32_t code);
```

四类实现在 `h` 中存放的内容如下,桥接代码均为零行:

| 实现 | `h` |
|---|---|
| linux | `fd` |
| windows | `HANDLE` |
| 裸机 + picolibc | 实测为 `FILE*`,即 `__stdio` |
| 裸机零 C 库 | 驱动结构指针或小索引 |
| 真内核 | 能力索引 |

### 3.1 形状的选择理由

| 决定 | 理由 |
|---|---|
| C ABI | 实测(H1):跨包提供实现只有这一条路径;且 KAL 一次调用的成本可忽略,因为调用本身就要穿越陷入边界 |
| 两字结构返回 | 实测(CABI):两个体系结构上结构返回都更便宜;RISC-V 上即 `a0/a1`,陷入桩天然可返回。标量部分必须不超过一个机器字,否则退化为隐藏指针 |
| 不透明句柄 | 见 §1.1;且真内核不能假设进程模型与全局命名空间,句柄必须与调用方上下文相关 |
| 封闭错误枚举,不透传 errno | errno 属于 POSIX。映射不等于模拟:查表翻译不是模拟层,构造 fd 命名空间才是 |
| 自由函数而非虚表 | 虚表把结构体布局写入 ABI,而「只增不改」的演进规则保护不了它 —— 增加一个方法即改变布局 |
| core 中没有 `open(path)` | WASIp1 的教训:照搬 fd 加路径命名空间,导致每个非 POSIX 宿主都要模拟 preopen 与 `openat` |

---

## 4. 可选能力的机制:两次构建,两次删除

本节记录一段完整的推导,其结论是删除该机制。保留记录的理由是:两个候选方案各自的
约束是实测得到的,而下一次真正出现可选操作时需要这些约束。

### 4.1 草案的第一处错误及其纠正

草案曾写道:

> `requires` 作用于不存在的限定名是硬错误,因此「能力等于符号是否存在」这条路径
> 在 C++ 内无法测出,声明必须永远齐全,能力须放入一个 `caps` 结构体。

第一句成立,其后的推论不成立。硬错误只发生在限定名上;非限定名经 ADL 在模板中是
待决的,`requires` 会求值为 `false`。

实测(llvm 22.1.8,真 C++20 模块,非头文件):

```cpp
// 限定名:名字不存在时是硬错误
if constexpr (requires(S s) { kal::seek(s, 0); })

// 非限定名经 ADL:名字不存在时求值为 false
template <class S> concept Seekable = requires(S s, long o) { seek(s, o); };
```

由此,能力不需要另一个可命名的实体,实现的模块接口本身即是能力声明。

### 4.2 兜底重载机制,及其只有实测才能发现的约束

```cpp
export module openkal.stream;
export namespace kal {

struct stream    { unsigned long h; };
struct io_result { unsigned long n; int e; };

// 兜底重载的返回类型必须与真实现不同
struct unsupported_t {};
template <class> inline constexpr bool always_false = false;

template <class S>
unsupported_t seek(S, long) {
    static_assert(always_false<S>,
        "this openkal backend provides no seekable streams. "
        "openkal.fs hands out descriptors that do.");
    return {};
}

template <class S> concept HasSeek =
    requires(S s, long o) { requires __is_same(decltype(seek(s, o)), io_result); };

}
```

四种情形均经实测:

| 情形 | 结果 |
|---|---|
| 有实现,调用 `seek(s, 0)` | 编译通过,真实现在重载决议中被选中 |
| 有实现,求值 `HasSeek<stream>` | 为真 |
| 无实现,求值 `HasSeek<stream>` | 为假,非硬错误,因而 `if constexpr` 可降级 |
| 无实现,强制调用 | 编译期报错,文案由规范给出 |

其中一条约束只有实测才能发现:若兜底重载的返回类型与真实现相同,则 `HasSeek` 在
无实现时同样为真 —— ADL 找到了兜底重载,而 `requires` 不实例化函数体,
`static_assert` 因而不触发。两个机制相互干扰,只有依靠返回类型区分才能共存。
本文第一版即如此编写,测得为真后才发现。

### 4.3 模块名归属:一条已被撤回的安排

0.1 采用的安排是:接口包提供 `openkal.decl.<interface>`,实现提供
`openkal.<interface>` 并 `export import` 前者。该安排的唯一用途是使可选能力可经 ADL
探测,而 ADL 要求实现的声明对消费者可见。

该安排在 0.2 被撤回,理由见 §21。此处保留其中三条实测,因为它们是语言事实,
与该安排是否被采纳无关(mcpp 2026.8.19.4 / gcc 16.1.0):

| 事实 | 结果 |
|---|---|
| 传递依赖的模块能否 import | 能。应用只依赖实现,可以 import 接口包的模块 |
| ADL 能否到达未 import 的模块 | 不能。报 `error: 'seek' was not declared in this scope` |
| 接口模块能否命名为 `openkal.stream.decl` | 不能。ninja 报依赖自环 |

第三条的完整报错为:

```
ninja: error: dependency cycle: gcm.cache/openkal.stream.gcm -> gcm.cache/openkal.stream.gcm
```

模块图把点号读作层级关系。因此,若将来需要一个与接口名并列的模块名,它不能是接口名的
点号延伸:`openkal.decl.stream` 可行,`openkal.stream.decl` 不可行。

### 4.4 实现如何被选中:只使用 `mcpp.toml`

消费者声明两条依赖:

```toml
[dependencies]
openkal = "0.4.0"                # 契约

[target.'cfg(os = "linux")'.dependencies]
openkal-linux = "0.4.0"          # 实现,可替换的一半

[target.'cfg(os = "macos")'.dependencies]
openkal-macos = "0.2.0"
```

源码写 `import openkal.stream;`,两个目标之间一字不改。这与 KA1 实测的
「同一份 `app.cppm`、无 `#if`、两个实现」是同一形状。

契约那条依赖在技术上可以省略,但不应省略,理由只有一条:

> 它是应用真正耦合的对象,并且它把契约版本不匹配变成解析期错误。

应用声明 `openkal = "0.4"` 而实现只支持 `"0.3"` 时,依赖解析当场失败;省略该条时,
同一问题要等到编译期才以一批签名不匹配的形式出现。

该形状与生态中已被验证的形状一致:Rust 的 `embedded-hal`(trait 包)加板级包,
应用同时依赖两者;`log` 加 `env_logger` 也是同一形状。

| | 只写实现 | 契约加实现 |
|---|---|---|
| 行数 | 1 | 2 |
| 契约版本由谁决定 | 实现 | 应用 |
| 版本不匹配何时暴露 | 编译期,一批签名错误 | 解析期,一条消息 |
| 更换实现须改几行 | 1 | 1,契约那条不动 |

包名不应带 `-abi` 后缀:openkal 本身就是接口与 ABI。

### 4.5 接口层面的缺失

一整个 interface 不存在时,`import openkal.task;` 在编译期即找不到模块。
该性质在 0.2 的分层下依然成立,因为模块由规范包提供。

---

## 5. 声称与事实的分离

草案担心的情形是:实现声明 `caps::seek = true` 而 `seek()` 永远失败。
在 ADL 机制下,结构性的不实声明已不可能作出:

> 无法在不声明一个返回 `io_result` 的 `seek` 的前提下声称具备 seek 能力,
> 而声明后不定义是链接错误。

在 0.2 删除该机制后,该问题以更彻底的方式消失:不存在可选操作,因而不存在可声称的能力。

### 5.1 使「不支持」在类型系统中不可表达

剩余的问题是分类错误而非不实声明。判据不变:

> 一个操作若能存在但永远失败,通常说明它被错误地合并了。

MMU 是该判据的一个实例:`NoMmu::map()` 对非恒等映射返回否定,这不是不实声明,
而是该抽象本不应把两族对象装入一个 concept。当时的结论是把它移到 `cfg` 轴。

推广之:socket 不能 seek,但这不是实现的不实声明,而是名词错误。因此 `fs` 的
descriptor 与 `net` 的 socket 各持自己的句柄类型(§2.3),`kal::seek(socket, 0)`
因不存在该重载而编译失败,而非因返回错误而失败。

### 5.2 双向 conformance

> 实现若不提供某操作,必须确实不导出该符号 —— 该性质可用 `nm` 检查。

其价值在于它不是行为测试,而是对制品的静态检查:既快且不可能漏测。
0.4 的实现中,该检查以 `--complete` 模式运行,即实现声称提供全部接口时,
未导出的名字同样构成失败。两个实现均导出全部 47 个名字。

### 5.3 残余风险

以上均不证明行为。实现可以导出某操作、通过顺利路径、在某个输入上出错。
这是每一份规范都具有的残余风险,POSIX 亦然,答案只能是 conformance 的覆盖度,
不存在语言层的解法。

0.4 的经验给出了该风险的一个具体形态:一个测试若不观察目标,就无法检测目标。
实现的套件启动 `/bin/true` 并读取其状态,而 `/bin/true` 忽略参数,因此无论参数向量
完整到达还是被移位一位,状态都相同。详见 §23.1。

---

## 6. 实现者视角

### 6.1 交付内容

| | |
|---|---|
| 一组 `extern "C"` 定义 | 按所实现的 interface,覆盖 §3 与 §22 的清单 |
| conformance 通过记录 | 双向:提供的操作可用,未提供的符号不存在 |
| 导出模块 | 无。规范包拥有全部模块,实现不导出任何模块(§21) |

### 6.2 最小实现:一个裸机 UART 后端

```cpp
// openkal-uart/src/stream.cpp
extern "C" {
kal_stream kal_stdout(void) { return kal_stream{1}; }   // h 是一个小索引
kal_stream kal_stderr(void) { return kal_stream{1}; }
kal_stream kal_stdin (void) { return kal_stream{0}; }

kal_io_result kal_stream_write(kal_stream s, const void* buf, uintptr_t n) {
    if (s.h != 1) return kal_io_result{0, kal_err_invalid};
    auto* p = static_cast<const unsigned char*>(buf);
    for (uintptr_t i = 0; i < n; ++i)
        *reinterpret_cast<volatile unsigned char*>(0x10000000) = p[i];
    return kal_io_result{n, kal_ok};
}
}
```

不存在第二份配置文件。该实现不提供 `openkal.fs`,做法是不提供其定义;
`mcpp.toml` 中只有普通的包信息。

### 6.3 实现如何被选中

条件依赖,引擎无新增轴:

```toml
[target.'cfg(all(arch = "riscv64", os = "none"))'.dependencies]
openkal-uart = "0.1"

[target.'cfg(os = "linux")'.dependencies]
openkal-linux = "0.4.0"
```

### 6.4 真内核实现者的额外约束

1. 不能假设进程模型、用户内核分界或全局命名空间 —— 单地址空间内核中没有进程。
2. 句柄必须与调用方上下文相关,不能是全局整数表的下标。
3. 返回形状必须能穿越陷入边界 —— 两字结构在 RISC-V 上即 `a0/a1`。

三条约束都指向同一结论:不透明的一个机器字的句柄。

---

## 7. 上层视角

### 7.1 应用直接使用

```cpp
import openkal.stream;        // 只写接口名;提供定义的是实现

int main() {
    kal::write(kal::out(), "hello\n");
}
```

实测(KA2):更换实现不需重新编译应用 —— 同一个 `app.o` 更换实现的目标文件重新链接即可。

### 7.2 类型化封装:C ABI 之上的零成本层

```cpp
export namespace kal {

inline io_result write(stream s, std::span<const std::byte> b) {
    return kal_stream_write(s, b.data(), b.size());
}

inline io_result write(stream s, std::string_view sv) {
    return write(s, std::as_bytes(std::span{sv.data(), sv.size()}));
}

}
```

`std::span`、`std::byte` 与 `std::string_view` 均在 freestanding 可用的 103 个头文件内
(实测,见 freestanding review §4),因此该层在裸机上可以存在。

---

## 8. 对接下层硬件:BSP 接线

openhal 的设备实例可以成为 openkal 的实现,这是接线而非层级:

```cpp
// BSP:把这块板的 openhal Serial 接成 openkal 的标准输出
import openhal.serial;

namespace {
    auto& uart = board::uart0();          // openhal 实例,concept,零成本
}

extern "C" kal_io_result kal_stream_write(kal_stream s, const void* buf, uintptr_t n) {
    if (s.h != 1) return { 0, kal_err_invalid };
    uart.write({static_cast<const std::byte*>(buf), n});   // 内联
    return { n, kal_ok };
}
```

成本上需注意:openhal 是 concept,`uart.write` 可内联;openkal 是 C ABI,
`kal_stream_write` 是一次真实调用。这正是两者契约形态不同的原因(§12)。

---

## 9. 对接 C 库:picolibc

实测(picolibc 1.8.12,rv64gc/lp64d),`libsemihost.a` 的 `common_iob.c.o` 含:

```
d __stdio                    FILE 实体
R stdout / stdin / stderr    三者均指向它,同址
U sys_semihost_putc          未定义
U sys_semihost_getc          未定义
```

而 `libc.a` 并不定义 `stdout` —— picolibc 早已把「C 库」与「控制台位于何处」分开。

因此移植到 openkal 只需一个小目标文件:

```cpp
// openkal-picolibc/src/console.cpp
#include <stdio.h>
import openkal.stream;

static int kal_putc(char c, FILE*) {
    return kal_stream_write(kal_stdout(), &c, 1).e == kal_ok ? c : EOF;
}
static FILE __kal_stdio = FDEV_SETUP_STREAM(kal_putc, nullptr, nullptr,
                                            _FDEV_SETUP_WRITE);
extern "C" FILE* const stdout = &__kal_stdio;
extern "C" FILE* const stderr = &__kal_stdio;
```

成本论据:`struct __file` 中的 `put` 本来就是函数指针
(`int (*put)(char, struct __file*)`),接到 openkal 上不新增任何一层间接。
由此 `printf` 与 `kal_stream_write` 汇入同一底层,不产生第二条 I/O 路径。

反向同样成立:宿主实现的 `kal_stream_write` 可以就是 `fwrite(buf, 1, n, stdout)`。
同一份契约,两个方向。

---

## 10. 对接 libc++ 与 gcc 工具链

### 10.1 libc++:36 个名字

实测:llvm 22.1.8 的 `__thread/support/` 有四个后端 —— `pthread.h`、`windows.h`、
`c11.h`、`external.h`。可插拔线程后端这一形状在标准库中已经存在。

置 `_LIBCPP_HAS_THREAD_API_EXTERNAL = 1` 后须提供 `<__external_threading>`,
其接口面为 36 个名字(自 `pthread.h` 计数,与调研一致):

```
mutex×4            __libcpp_mutex_t / _lock / _trylock / _unlock / _destroy
recursive_mutex×5  __libcpp_recursive_mutex_t / _init / _lock / _trylock / _unlock / _destroy
condvar×5          __libcpp_condvar_t / _signal / _broadcast / _wait / _timedwait / _destroy
thread×9           __libcpp_thread_t / _id / _create / _join / _detach / _yield / _sleep_for …
once×1             __libcpp_execute_once 与 __libcpp_exec_once_flag
tls×3              __libcpp_tls_key / _create / _get / _set
```

0.3 定义 `openkal.task` 时采取的形状与此不同,且这一差异是刻意的。openkal 只声明
「使一个上下文在某个字上挂起」的原语,互斥量与条件变量是其上的构造。
`openkal-libc` 用 `kal_task_wait` 与 `kal_task_wake` 构造了一个三态互斥量,
证明该分解可行(§23.2)。因此对接 libc++ 时,36 个名字中的互斥量与条件变量部分
由适配层构造,而非由 openkal 逐一声明。

这一取舍的理由是:互斥量的数量在一个程序中不确定,而挂起原语只有一个。
按 §12 的基数论据,数量不确定的对象不属于 C ABI 契约。

### 10.2 gcc 与 libstdc++:gthreads

实测(gcc 16.1.0):`bits/gthr-default.h` 中 `__gthread_*` 共 71 个名字,
其中 20 个是 `__gthread_objc_*` 遗留,真实接口面为 51,与 libc++ 的 36 高度重合。

更重要的先例是 gcc 自带的 `gthr-single.h`:「没有线程」是一个编译期后端选择,
而非运行期 `ENOSYS`。这正是本方案主张的形状,而 gcc 已如此实践三十年。

因此 openkal 的 gcc 侧对接是提供一份 `gthr-openkal.h`,与 `gthr-posix.h` 平级。

### 10.3 `operator new` 与 `__libcpp_verbose_abort`

二者是标准扩展点,不需要发明契约:

```cpp
void* operator new(std::size_t n) {
    if (void* p = kal_alloc(n, __STDCPP_DEFAULT_NEW_ALIGNMENT__)) return p;
    kal_abort("operator new failed", 20);
}
_LIBCPP_BEGIN_NAMESPACE_STD
[[noreturn]] void __libcpp_verbose_abort(const char* f, ...) _NOEXCEPT {
    kal_abort(f, __builtin_strlen(f));
}
_LIBCPP_END_NAMESPACE_STD
```

实测得到的一条约束:`__libcpp_verbose_abort` 必须通过 libc++ 自身的头文件声明 ——
真实符号位于 ABI 内联命名空间 `std::__1::` 中,手写 `namespace std { }` 能编译通过、
无法链接,且报错文字与未定义时完全相同。

### 10.4 openkal 不认领 `thread_local`

实测得到的静默失败:

```
thread_local int counter;  →  编译通过、链接通过、无未定义符号、无诊断
                           →  运行期通过未设置的 tp 读写无效地址
```

`thread_local` 不属于线程契约,属于 openarch(TLS 寄存器约定)与 BSP
(`start.S` 设置 `tp`、链接脚本提供 `.tdata`/`.tbss`)。
而 `__external_threading` 中的 tls 三项是 `pthread_key` 那种动态 TLS,并非同一事物。

规范必须明确该边界,否则「我实现了 openkal 的线程」会被读作「`thread_local` 可用了」。
且 libc++ 与 libstdc++ 自身内部就使用 `thread_local`,并非用户不书写即可回避。

---

## 11. C++ 特性与其所解决的问题

| 特性 | 解决的问题 | 限制 |
|---|---|---|
| modules | 模块找不到即构成能力检查,把链接期错误提前到编译期 | 需要 mcpp 的 `reexport` 与 provisions 承载 |
| ADL 与 `requires` | 非限定名在模板中是待决的,缺失时求值为 `false` 而非硬错误 | 限定名仍是硬错误;二者的差别是该机制成立的全部基础。0.2 起未被使用 |
| 兜底重载与 `static_assert` | 无实现时直接调用即编译期报错,文案由规范给出 | 兜底的返回类型必须与真实现不同,否则探测恒为真。0.2 起未被使用 |
| `if constexpr` | 消费者按能力降级,未选中的分支不实例化 | — |
| concepts | 组合能力;openhal 的多提供者共存 | 只检查语法不检查语义(K1/K2) |
| 非类型模板参数 | 使「不支持」在类型系统中不可表达(§5.1) | — |
| `enum` | 封闭错误集合,不透传 errno | — |
| `[[nodiscard]]` | `io_result` 不能被静默丢弃 | — |
| `std::span` / `std::byte` / `string_view` | C ABI 之上的类型化层 | 三者均在 freestanding 可用的 103 个头文件内 |
| `[[noreturn]]` | 把 `kal_abort` 的控制流事实纳入类型 | — |
| virtual 与虚表 | 不采用 | 把结构体布局写入 ABI,「只增不改」保护不了 |
| exceptions 与 RTTI | 不采用 | 裸机整图关闭(mcpp 2026.8.19.4 起) |
| 以 `requires` 探测限定名 | 不采用 | 实测为硬错误 |
| `caps` 结构体与 `capabilities.toml` | 不采用 | 被 ADL 机制取代,而后者又在 0.2 被删除;第二份配置文件违背生态整洁性 |

---

## 12. 与 openhal 及 openarch 的关系:契约形态由基数决定

| | 一个程序中的实现数 | 契约 | 理由 |
|---|---|---|---|
| openarch | 1,一颗 CPU | concept 且必须 LTO | 实测 K3:`local_irq_disable()` 本应是一条 `csrci`;跨模块 `-O2` 退化为真实 `jal`,`-flto` 后恢复 |
| openkal | 1,一个内核 | C ABI | 一次调用成本可忽略,因为本就要陷入;H1 |
| openhal | N,每条总线与设备各一 | concept | 实测 H3:多提供者共存且零成本可内联 |

openarch 与 openhal 都采用 concept,但理由相反:前者因调用成本不可接受,
后者因需要多实现共存。openkal 两种压力都不存在,因而采用最简单也最稳定的 C ABI。

`openkal.stream` 与 `openhal.Console` 的边界:

- openkal 的流是程序的标准流:数量为一、由环境给出、借用而非拥有。
- openhal 的 `Console` 与 `Serial` 是被打开的设备:数量为多、被拥有、有类型。

二者是两个名词,而非同一事物的两层。自然的组合方式见 §8。

---

## 13. 判据与门

### 13.1 双向准入判据

| 方向 | 判据 | 计量方式 |
|---|---|---|
| 对下 | 四个实现均能自然实现,不存在模拟层 | 写出四个实现的 `kal_stream_write`,计量「仅为迁就形状而存在」的行数。任何实现若需要表、注册中心或路径解析器,则形状有误 |
| 对上 | C 库、libc++ 与 gcc 的既有插点可直接落上,不存在适配层 | `stdout->put`、`operator new`、`__libcpp_verbose_abort`、36 个 `__libcpp_*`、`gthr-*.h`,同样计量桥接行数 |

对上判据不满足意味着资源分解切分有误,应当返回修改分解,而非修改边界以迁就标准库。

### 13.2 D0 至 D3 的门

| 阶段 | 继续的判据 | 停止的信号 |
|---|---|---|
| D0 | 有第三方实现了第三个后端 —— 这是该方向能否成立的真实变量 | 半年内无人实现,则停在 D0,作为 mcpp 内部设施 |
| D1 | 三个官方实现全部通过 conformance;Windows 实现不经 MSVC CRT 的 POSIX 兼容层 | Windows 实现无法在不模拟的前提下完成,说明 SPEC 形状有误,需返工 |
| D2(openhal) | 同一个驱动包既运行于裸机 MCU 又运行于 Linux | 驱动作者不参与则停止;不影响 openkal |
| D3(openarch) | 两个最难抽象的原语(上下文切换、页表项)不碎裂 | 碎裂则停止;此处碎裂后,其后的结论均不可信 |

0.4 的进展不移动 D0 的门。两个官方实现均由同一作者编写,因而它们之间的一致
不构成第三方证据(§23.1)。

---

## 14. 明确不做的事项

| 事项 | 理由 |
|---|---|
| core 中提供 `open(path)` | WASIp1 的教训;打开文件属于 `openkal.fs`(自有句柄类型),不在 core,也不经一个通用命名接口(§2.3) |
| 运行期 `ENOSYS` | 部分性以「链接哪些 interface」表达;gcc 的 `gthr-single.h` 已是三十年的先例 |
| 透传 errno | 采用封闭枚举;映射不是模拟 |
| 虚接口与虚表 | ABI 脆弱,与「只增不改」冲突 |
| 保证 `thread_local` | 属于 openarch 与 BSP(§10.4) |
| 引擎认识 openkal | 计划 §7.2 第一行:引擎永不认识这三层;实现的选择是条件依赖,引擎无新增轴 |

---

## 15. 撤回记录

草案与 0.1 中被撤回的四条,连同它们当时看似成立的理由:

| 撤回的事项 | 当时的理由 | 错误所在 |
|---|---|---|
| `openkal.namespace` 取代 fs 与 net | 文件、socket 与 UART 交付的都是流,区别仅在命名方式 | 一、违反本文 §5.1 的判据,能力集合成为不相干能力的并集;二、URI 解析器即是模拟层,违反对下判据;三、所引 WASIp2 先例是误读,它分开资源种类,只共享 stream 类型 |
| 扩展 `cfg()` 文法以支持 `cfg(mmu)` | K1/K2 的结论是「MMU 是能力轴而非契约」 | 该结论关于 openarch;openkal core 逐个接口检视,没有一条语义轴需要它,且 triple 本身已承载大部分 |
| `caps` 结构体与 `capabilities.toml` | 认为 `requires` 无法测出缺失的名字,因而能力必须放入另一个可命名的实体 | 该性质只对限定名成立。非限定名经 ADL 在模板中是待决的,缺失时求值为 `false`。实现的模块接口本身即是能力声明,两样事物整体删除,亦不需要第二份配置 |
| 应用 `import openkal.uart;` | 希望接口随实现而来 | 把源码固定在特定实现上,推翻 openkal 的基本性质。且它掩盖了真实约束:ADL 到不了未 import 的模块 |

四条的共同形状:都是一个看似整齐的统一,而该统一把两件本来不同的事合并了。
这与 §5.1 给出的判据是同一条 —— 只是该节的判据此前被用于检查他人的实现,
而未被用于检查自身的分解。

---

## 16. 综合 review 发现的开放问题

以下问题在 0.1 至 0.4 的过程中逐条得到规范表态,状态列注明其去向。

### 16.1 两个分配器

实测:picolibc `libc.a` 的 `vfprintf.c.o` 引用 `free`,即 printf 与分配器是耦合的。
而固件中已存在一整套 `malloc`、`free`、`__malloc_sbrk_aligned` 与 `__fallback_sbrk`。

若 `operator new` 走 `kal_alloc`(openkal 的区域分配器)而 `printf` 走 picolibc 的
`malloc`,则同一块 RAM 上存在两个分配器,且都试图扩展 sbrk。

规范条款:

> 实现所处的环境若已存在 C 库分配器,`kal_alloc` 必须实现在它之上,而非与它并列。

| 配置 | 做法 | 结论 |
|---|---|---|
| picolibc | `kal_alloc` 转 `malloc`,openkal 位于 C 库之上 | 一个分配器 |
| 零 C 库 | `kal_alloc` 使用自带区域;不存在 C 库 malloc | 一个分配器 |
| 自带区域且 C 库 malloc 同时存在 | — | 禁止 |

该条部分可 conformance 化:检查固件中 sbrk 的消费者是否唯一。

### 16.2 短写

写参考实现时立即撞上:`::write(2)` 可以短写。规范须选择一侧。

| 选择 | 后果 |
|---|---|
| 写全或报错(已采纳) | 循环位于实现内部,只写一次 |
| 允许短写 | 每个调用方都要自行编写循环,这正是 POSIX 使大量程序出错之处 |

已写入 SPEC 条款 7.4:`kal_stream_write` 写全或报告阻止它的条件,部分传输不是成功的结果;
`kal_stream_read` 报告实际传输的字节数,可少于请求数,零字节加 `kal_ok` 表示输入结束。
与部分写入不同,部分读取携带调用方需要的信息。

### 16.3 错误集合的封闭性与结构体布局

POSIX 有约 130 个 errno,一个约 15 项的封闭集合必然丢失信息。
已采纳的方案是封闭集合,细节通道只用于日志、不进入控制流。

C ABI 没有版本,而结构体布局会被永久冻结。「每个 interface 独立版本、只增不改」
保护得了函数(新增函数是可加的),保护不了结构体:`kal_io_result` 的布局一旦发布
即不能变动。已采纳的方案是明确声明这些布局永久冻结,写入 SPEC 条款 5.3。
符号版本后缀作为备选记录于 SPEC 的未决事项。

### 16.4 拥有与借用

标准流是借用的,不配 `close`;`openkal.fs` 的 descriptor 是拥有的,必须关闭。
C ABI 中没有 RAII,类型化 C++ 层可以封装 RAII,但那不是规范的一部分,
Rust 与 C 的消费者拿不到。规范因此明确列出哪些句柄是拥有的,并规定重复关闭的行为。

### 16.5 线程安全

同一个 `kal_stream` 被两个上下文同时写入的行为,在 0.3 定义 `openkal.task` 后
立即成为必须回答的问题。SPEC 条款 6.5 给出回答。

### 16.6 sized-free 的方向性成本

`kal_free(p, size, align)` 对区域分配器友好,但方向不对称:

- 宿主实现 `kal_free` 转 `free(p)`,丢弃 size,无成本。
- 反向:一个建立在 `kal_alloc` 之上的 C 库 `malloc` 必须自行保存 size,每次分配多一个字。

与 §16.1 的规则合并考虑,反向配置本就应当避免,因而成本可控。该取舍已写明。

---

## 17. 第一轮 review 的元结论

三条撤回(§15)与六条开放问题(§16)中,存在一个共同形状。

草案的错误分为两族。

第一族:一个看似整齐的统一,把两件本来不同的事合并了。

- `openkal.namespace` 合并了「命名」与「资源种类」。
- `cfg(mmu)` 把 openarch 的结论搬入 openkal。
- 「两个分配器」是未合并本应合并的对象。

而 §5.1 给出的判据 ——「一个操作若能存在但永远失败,说明它被错误地合并了」——
本可抓住前两条。该判据此前只被用于检查他人的实现,未被用于检查自身的分解。

第二族:测量了一种写法,并把结论推广到全部写法。

`caps` 结构体与 `capabilities.toml` 那一整套建立在一次实测之上:
「`requires { mcpp::runner("x") }` 是硬错误」。该实测本身没有错误,
错误在于结论的量词 —— 所写的是「C++ 语言内无法测出」,而真实情况是
「限定名无法测出」。非限定名经 ADL 一直可行。

代价是一整节设计与一份多余的配置文件格式,而验证它只需三行代码。

两族合并后的规律是:一条实测能否定一个做法,但否定不了一整类做法。
在写下「X 做不到」之前,应当先问「所测的是 X,还是 X 的某一种写法」。

---

## 18. 第二轮 review 新增的两条

### 18.1 基数是「每个 interface 一个实现」

草案 §12 称 openkal 的基数为 1,其含糊之处在于「1 个什么」。澄清如下:

```toml
[target.'cfg(os = "none")'.dependencies]
openkal-uart  = "0.1"      # 提供 openkal.stream 的定义
openkal-arena = "0.1"      # 提供 openkal.memory 的定义
```

一个程序可以从不同提供者取得不同的 interface,这是 §2 资源分解的自然结果。
冲突只发生在同一个 interface 有两个提供者时,那是重复符号定义,在链接期报错。

因此「基数 1」应当读作:每个 interface 的实现是一个。契约形态(C ABI)的论据不变。

### 18.2 兜底重载的适用范围

```cpp
template <class S> unsupported_t seek(S, long) {
    static_assert(always_false<S>, "…no seekable streams…");
}
```

`S` 无约束,因此 `kal` 中任何类型调用 `seek` 都落到此处。
`seek(some_socket, 0)` 会得到「no seekable streams」,名词错误。

修法是把兜底约束到它应当负责的类型:

```cpp
template <class S> requires std::same_as<S, stream>
unsupported_t seek(S, long) { … }
```

该条虽小,但它是 §5.1 判据的又一次应用:兜底重载的适用范围也是一种分类,
分类过宽则诊断指向错误的位置。

---

## 19. 官方参考实现:`openkal-linux`

它有两个身份:一个当天可用的实现,以及其他实现者可以照抄的样板。
它不移动 D0 的门(门是「有第三方实现了第三个后端」),但它使门变得够得着 ——
在没有可照抄的样板时,第三方须同时推测形状并编写实现。

### 19.1 参考实现验证了 fs 与 stream 的分解

编写 Linux 实现时会立即撞上一件事:

> Linux 上「能否 seek」是每个句柄的属性,而非实现的属性。
> `lseek(fd)` 对普通文件成功,对管道返回 `ESPIPE`。

因此若 `openkal.stream` 具有 `seek`,Linux 实现无法诚实回答:声称具备则对管道永远失败
(正是 §5.1 的「存在但永远失败」反模式);声称不具备则文件无法使用。

而 §2.3 的分解使该问题不存在:seek 属于 `openkal.fs` 的 descriptor 类型,
`openkal.stream` 根本没有它。参考实现独立地证实了那次撤回是正确的。

这是编写一份完整实现的最大价值:它是发现分解错误的唯一方法,且比 conformance 更早。

### 19.2 样板所教授的模式

| 模式 | 其他实现者应当照抄的内容 |
|---|---|
| EINTR 循环 | 每个实现都要处理自身平台的中断语义 |
| `kal_alloc` 转 `malloc` | §16.1:存在 C 库分配器时建立在它之上 |
| `kal_from_errno` 是一张表 | 映射不等于模拟(§3.1) |
| 不提供某能力时不声明 | 能力缺失即不提供定义,而非提供定义后返回错误 |
| `_exit` 而非 `exit` | `exit` 会执行 atexit 与静态析构,而 `kal_exit` 的契约是立刻结束。此类语义细节须向 SPEC 对齐 |

---

## 20. 0.3:从三个接口扩展到八个

0.3 的接口集合由资源种类推导而来,而非由某个程序的调用清单推导而来。
该方向由明确要求确定:设计通用内核 ABI,并以 gcc 工具链作为验证,而非以 gcc
的调用清单定义 openkal。

三条推导及其证据:

**`openkal.fs` 全程相对于一个目录,不存在全局路径命名空间。** 全局命名空间在
基于能力的内核上不存在,该类实现将不得不构造一个。环境提供一组预置目录,
程序在其上操作。证据是一个大型可移植程序的形状:名字解析是 C 库的工作,
而 `openkal-libc` 的 `okc::resolve` 以最长前缀匹配实现了它(§23.2)。

**`openkal.process` 启动一个程序而非复制调用方。** 复制调用方无法在每个环境上
忠实完成。旁证是:一个大型可移植程序启动子进程,而不调用任何复制类操作。

**`openkal.task` 暴露「使一个上下文在某个字上挂起」的原语。** 互斥量与条件变量是
其上的构造,这一点在任何实现它们的 C 库中都可观察到。

`openkal.time` 的能力字是 0.3 引入的机制,其必要性由第二个实现证实:
macOS 的单调时钟在系统挂起期间继续前进,Linux 的停止(§23.1)。
若不存在能力字,该差异只能由消费者自行推测。

---

## 21. 0.1 的分层被推翻:实现不应拥有应用 import 的名字

0.1 让实现提供 `openkal.stream`,规范包提供 `openkal.decl.stream` 并由实现再导出。
review 指出这与「openkal 是接口」矛盾 —— 应用 import 的那个名字落在了规范管不到的一方。

### 21.1 原有理由不成立的原因

该安排的唯一用途是使可选能力可经 ADL 探测,而 ADL 要求实现的声明对消费者可见。
而 0.1 没有定义任何可选能力 —— `write_vectored` 是为演示机制而放入的。

因此该机制服务的是一个当时不存在的需求,却付出了「实现拥有接口名」这一真实代价。

### 21.2 0.2 的分层

```
应用 ──import──► openkal ◄──import── 实现
  │                                    │
  └──────────── 链接 ─────────────────┘
```

- 规范包提供全部模块。
- 实现不导出任何模块,只贡献定义;它 import 与消费者相同的接口,因为它要定义自己
  所声明的内容。
- 因此实现无法扩展接口。这不是一条需要执行的规则,而是该安排的推论。

实测:实现零模块的工程编译运行通过;缺少实现时报
`undefined reference to kal_stream_write`,位于链接期,可读。

### 21.3 代价与去向

| | 0.1 | 0.2 及其后 |
|---|---|---|
| 缺少实现 | 编译期,点名模块 | 链接期,点名函数 |
| 可选能力探测 | ADL,编译期 `if constexpr` | 无,因为不存在可选能力 |
| 实现能否扩展接口 | 能新增重载,靠 conformance 封住 | 不能,由安排本身排除 |
| 碎片化风险 | 存在,需导出名集合比对兜底 | 不存在 |

可选能力的机制推迟到真正出现时再定,SPEC 6.3 记录了两个候选与各自的约束
(以及三条实测:限定名与非限定名的差别、ADL 到不了未 import 的模块、
点号延伸导致模块自环)。

### 21.4 元结论

这是同一形状的第三次出现:为一个尚不存在的需求设计机制,并为它付出真实代价。

前两次是 `openkal.namespace`(为统一命名而合并两种资源)与 `cfg(mmu)`
(把 openarch 的结论搬入 openkal)。判据应当是「今天是否存在该需求」,
而非「将来是否会有」—— 将来的需求可以用「推迟决定并记录约束」来承接,
其代价远低于提前实现一个错误的机制。

---

## 22. 0.4:规范此前未表态的两点

0.4 的两条条款由一个跨八个接口的可移植程序发现。二者均未改动任何声明,
因而导出符号集合不变,符合 0.3 的实现同样满足 0.4 的导出要求。
详细记录见
[`2026-08-20-openkal-portable-program-findings.md`](2026-08-20-openkal-portable-program-findings.md)。

### 22.1 条款 7.6:参数向量

`kal_process_spawn` 接受一个路径与一个参数向量。0.3 未说明该向量是否包含被启动程序
自身的名字。两个实现都在向量前面插入了路径,因而调用方的第 0 项落到了第 1 位。

采纳的规则是:向量是完整的,且被原样传递,其第 0 项是被启动程序观察到的自身名字。
三条理由,按分量排序:

1. 两侧必须一致。被启动的程序通过 `kal_env_arg(0)` 读取自身名字,而该函数确实包含它。
   调用方若未提供,则无法预测程序将读到什么。
2. 程序观察到的自身名字,在每个具有参数向量的环境上都是可观察行为,该选择属于调用方。
3. 相邻接口均如此:`posix_spawn`、`fdio_spawn`,以及 `CreateProcess` 的惯用形式。
   偏离者需要理由,而此处没有。

### 22.2 条款 7.7:缺席是一种答复

`kal_fs_info` 作用于不存在的名字时,0.3 声明了 `kal_node_absent` 但未说明何时使用它。

采纳的规则是:查询成功并报告缺席,而打开同一名字报告 `kal_err_not_found`。
查询与访问是两种操作:一个询问名字指向什么的调用方,在被告知它不指向任何东西时
已经得到了答复。这与 `openkal.env` 在「变量缺席」与「变量值为空」之间所作的区分
是同一条,理由也相同。

---

## 23. 第二个实现与其上的 C 库所提供的证据

### 23.1 两个实现之间的一致不构成证据

`openkal-macos` 记录了四处与 Linux 实现的分歧,每一处都是某个接口本可假设某种机制
的位置:单调时钟在系统挂起期间继续前进而非停止;名字比较不区分大小写;
spawn 没有设置工作目录的属性;不存在程序可用的挂起原语。
这四处证明能力字与相应的接口形状是必要的。

但两个实现在参数向量一事上作出了相同的选择,而该选择是错误的。它们一致的原因是
同一作者、同一次阅读。同一作者的第二个实现,其证据力低于他人的第二个实现;
这是这两个实现之间所能建立的上限。

由此推出一条对 conformance 的要求:一个测试若不观察目标,就无法检测目标。
实现的套件启动 `/bin/true` 并读取状态,而 `/bin/true` 忽略参数,因此参数向量
完整到达与被移位一位产生相同的状态。0.4 新增的断言启动一个 shell:
`sh -c <script>` 在未给出后续参数时从自身的第 0 项取 `$0`,因而脚本观察到调用方
选择的名字。该断言在改变行为之前,已被确认对旧行为失败。

### 23.2 `openkal-libc` 检验了规范所作的声明

规范声明:把一个 C 库移植到 openkal 一次,其上的软件即可运行于每个 openkal 实现。
`openkal-libc` 检验该声明,方式是执行规范刻意置于自身之外的两项适配:

1. 把一个全局名字解析到环境提供的目录上(`okc::resolve`,最长前缀匹配)。
2. 从挂起原语构造同步对象(`okc::mutex`,三态)。

其上的一个普通程序按全局路径读取文件、查询环境变量、测量时间间隔并启动另一个程序,
而其自身不包含上述任何一项。该程序的输出与系统 `wc` 逐字段一致。

`openkal-libc` 的三个测试套件最初全部链接失败,报出十六个未定义操作。
该失败正是 SPEC 条款 4.2 所描述的诊断,这是该条款首次被一个非为检验它而构造的场景检验。

---

## 24. 一致性:与 mcpp 生态的关系

openkal 的实现未要求修改 mcpp。所使用的机制全部是生态既有的:

| 需求 | 所用机制 | 是否新增引擎轴 |
|---|---|---|
| 按平台选择实现 | `[target.'cfg(os = "…")'.dependencies]` | 否 |
| 库自身不链接实现,而其测试需要 | `[target.'cfg(…)'.dev-dependencies]` | 否 |
| 契约版本不匹配在解析期暴露 | 普通的版本依赖 | 否 |
| 能力缺失在编译期表达 | 模块不存在 | 否 |
| 导出面的静态检查 | `nm` 加一份名字清单 | 否 |

不引入第二份配置文件是一条明确约束:mcpp 生态的全部声明位于 `mcpp.toml` 中,
`capabilities.toml` 因违背该约束而被删除(§15)。

---

## 25. 现状小结

| 维度 | 状态 |
|---|---|
| 位置无关(C 库之上或之下) | 由不透明句柄承载,三种实现实测 |
| 划分依据 | 资源种类;曾塌缩过度(namespace),已撤回 |
| core 边界 | abort、stream、memory;判别式是「实现与模拟」 |
| 接口集合 | 八个接口,0.3 由资源种类推导;net 与 channel 保留 |
| 能力表达 | 不存在可选操作,因而不存在表达可选性的机制;属性差异由能力字承载 |
| 模块名归属 | 规范包拥有全部模块,实现不导出模块 |
| 依赖形状 | 契约与实现两条,契约那条用于固定版本 |
| 引擎改动 | 零 |
| 对下与对上判据 | 双向且可计量,以桥接行数计 |
| 实现数量 | 两个操作系统实现,一个其上的 C 库;两者由同一作者编写 |
| 最大风险 | 不是技术性的,而是 D0 的门:是否有第三方实现第三个后端。参考实现降低门槛,不移动门 |
