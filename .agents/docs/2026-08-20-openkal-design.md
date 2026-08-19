# openkal 设计方案:通用内核 ABI 规范

**状态**:设计草案,**未实施**。对应第一阶段计划 §7 的 **D0/D1**,而 D0 的门不是技术判据 ——
是**「有第三方实现了第三个后端」**。本文的价值在于**不要在 D0 就把路堵死**。

**证据来源**:标注 ⓘ 的是本机实测(载荷版本写在旁边),其余是设计主张。
调研出处见 [`2026-08-18-freestanding-baremetal-analysis.md`](2026-08-18-freestanding-baremetal-analysis.md)。

---

## 1. 定位:一份规范,两个方向

openkal **不是一个库,是一份内核 ABI 规范**。它有两类使用者,方向相反:

```
        应用 / libc / libc++ / Rust / ...
                    │  对上:消费者「可以依赖什么」
        ════════════╪════════════  openkal SPEC
                    │  对下:实现者「必须提供什么」
        Linux / Windows / 你写的内核 / 裸机 BSP
```

⭐ **关键性质:openkal 不知道自己在 libc 之上还是之下。**
hosted 后端把 `kal_stream_write` 转发到 `::write(2)`(在 libc 之上);
裸机后端直接打 MMIO(取代 libc);picolibc 后端把 `FILE::put` 接到它上面(在 libc 之下)。
**三种都成立,而且是同一份契约。**

ⓘ **实测(调研 KA1/KA3)**:同一份 `app.cppm`,零 `#if`,hosted 转 `::write(2)`、
裸机转 MMIO UART;**两侧 `app.o` 的外部符号集完全相同**(都只有 `kal_write`);
裸机侧零未定义符号,157 字节。

### 1.1 这个性质由什么承载

**句柄的不透明性。** 一旦句柄有类型,openkal 就被钉在 libc 的某一侧:

| 句柄形状 | 强迫的位置 | 后果 |
|---|---|---|
| `int fd` | libc 之下(要有 fd 表) | ⛔ Windows 后端要维护 fd→HANDLE 表 = **模拟层** |
| `FILE*` / stream 对象 | libc 之上 | ⛔ 裸机零 libc 档、Windows 都得造 FILE |
| **不透明一个机器字** | **不强迫** | ✅ 每个后端塞自己原生的东西 |

---

## 2. 划分依据:资源种类,不是「点亮标准库」

⚠️ **一条被否掉的划分依据**:按「点亮标准库的哪一块」切接口。
它耦合到 C++、耦合到今天的标准库,而且**依赖方向反了** —— 内核 ABI 应由内核提供什么定义。

而且那正是**同一个错误的第三代**:POSIX 由 C 的 stdio/unistd 塑形,WASIp1 由 POSIX 塑形。

⭐ **划分依据 = 交给你的是哪一种资源。** 语言无关,也是 WASIp2 / seL4 / Fuchsia 收敛到的形状。

「点亮标准库」退回它该在的位置:**D1 的准入判据**(见 §11.2),不是划分依据。

### 2.1 接口清单

| interface | 资源 | core? | 判据:无该设施的后端能实现吗 |
|---|---|---|---|
| **`openkal.abort`** | (终止) | ✅ | `for(;;) wfi` 是一个**实现** |
| **`openkal.stream`** | 一条字节流 | ✅ | null sink 是一个**实现** |
| **`openkal.memory`** | 一块内存区域 | ✅ | 静态 arena 上的 bump allocator 是一个**实现** |
| `openkal.time` | 一个时间源 | ❌ | ⚠️ 不走的计数器**不是**时钟 —— 会让超时静默失效 = **模拟** |
| `openkal.task` | 一个执行上下文 | ❌ | 需要调度 + 上下文切换(那是 openarch) |
| `openkal.fs` | 一个 **descriptor**(自有句柄类型) | ❌ | 需要一个命名权威 |
| `openkal.net` | 一个 **socket**(自有句柄类型) | ❌ | 同上 |
| `openkal.channel` | 一条消息通道 | ❌ | — |

⭐ **`stream` 是共享货币,不是统一入口**:`fs` 的 descriptor 与 `net` 的 socket 各有
自己的句柄类型和自己的操作,但**都能产出 `openkal.stream`**。往哪写这件事对文件 /
socket / UART 是同一套代码;打开它们不是。

### 2.2 ⭐ core 的判别式:实现 vs 模拟

> **假实现会让上层「静默地错」的 ⇒ 模拟;只是「容量小 / 会失败」的 ⇒ 实现。**

`operator new` 失败在任何平台上都是**有定义的结果**,所以静态 arena 是实现。
而一个不前进的时钟会让 `wait_for` 永远返回、熵不随机会让密钥可预测 —— 那是模拟。

⇒ **「这块 MCU 没有堆」是错的命题**:只要有 RAM,堆就是实现出来的;
上层不关心 openkal 底层怎么做到。

### 2.3 ⚠️ 一个被起草后撤回的分解:`openkal.namespace`

草案曾把 `fs` 和 `net` 消掉,换成一个通用的 `openkal.namespace`(名字 → 资源),
理由是「文件 / TCP 连接 / 管道 / 串口给你的都是 stream,只是命名方式不同」。

**重估后撤回。三条攻击全部成立:**

**① 它触犯本文自己的 §5.1 规矩。** 「一个操作若能『存在但永远失败』,说明它被错误地
合并了」—— 而把文件和 socket 都塞进一个 `stream`,`stream` 的 caps 就成了
**互不相干能力的并集**(seek/size/truncate/sync 对上 shutdown/peer/nodelay),
每个后端对其中大多数说 `false`。**正是那个反模式,换了个地方出现。**

**② `namespace` 需要一个 URI 解析器,那是模拟层。** `kal_namespace_open("tcp://…")`
要求**每个后端都能解析 scheme**:只有 UART 的后端也得解析并拒绝 `tcp://`;
合法 scheme 集合无界、不可发现;错误是字符串形状的。
⚠️ **直接违反对下判据**(四后端自然实现、不需模拟层),而且比 POSIX 还差 ——
POSIX 至少 `open()` 与 `socket()+connect()` 是分开的类型化调用。

**③ 引用的先例是错的。** 草案称「这是 WASIp2 收敛到的形状」。**不是。** WASIp2 是:

```
wasi:io/streams        input-stream / output-stream   ← 共享的传输资源
wasi:filesystem/types  descriptor                     ← 自有资源类型,能产出 stream
wasi:sockets/tcp       tcp-socket                     ← 自有资源类型,能产出 stream
```

它把**资源种类分开**,共享的是 **stream 这个传输类型**。草案把「共享 stream」
误读成了「统一命名」。

⇒ **保留对的那半(流统一了传输),丢掉错的那半(统一命名)。**

| | namespace 草案 | 撤回后 | 单体 fs+net |
|---|---|---|---|
| 实现者 | ⛔ 人人要 URI 解析器 | ✅ 没有就**不提供**该 interface | ✅ 同 |
| 消费者 | ⛔ 错误是字符串;**编译期不知道支不支持** | ⭐ `import openkal.net;` 缺了就**编译期报错** | ✅ 同 |
| 规范负担 | ⛔ 要标准化 **scheme 注册表** = 巨大隐藏面 | ✅ 每 interface 独立版本,面有界 | ⚠️ 接口大但有界 |
| 类型安全 | ⛔ caps 成为不相干能力并集 | ✅ 文件操作在文件句柄上 | ✅ 同 |

⚠️ **划分原则(按资源种类)没错,错的是塌缩过头** —— `descriptor` / `socket` /
`stream` 本来就是三种资源。

**net 不是设备,但网卡是** —— 按基数分(§12):网卡 N 个 → openhal;协议栈 1 个 → openkal。

---

## 3. 核心 ABI 形状

```c
/* openkal.core —— C ABI(H1:跨包提供实现只有这一条路)*/

typedef struct { uintptr_t h; } kal_stream;     /* 不透明,1 个机器字 */

/* 标准流:借用,不拥有 —— 裸机上「关闭控制台」没有意义 */
kal_stream kal_stdout(void);
kal_stream kal_stderr(void);
kal_stream kal_stdin (void);

/* 2 字返回;T ≤ 1 个机器字 */
typedef struct { uintptr_t n; int32_t err; } kal_io_result;

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

四个后端往 `h` 里塞什么 —— **桥接代码全部为 0 行**:

| 后端 | `h` |
|---|---|
| linux | `fd` |
| windows | `HANDLE` |
| bare + picolibc | ⓘ **`FILE*`(就是 `__stdio`)** |
| bare 零 libc | 驱动结构指针 / 小索引 |
| 真内核 | **能力索引** |

### 3.1 为什么是这些形状

| 决定 | 理由 |
|---|---|
| **C ABI** | ⓘ H1 实测:跨包提供实现只有这一条路;且 KAL 一次 call 的成本**可忽略**(本来就要陷入) |
| **2 字结构返回** | ⓘ CABI 实测:两个 arch 上结构返回都更便宜;RISC-V 上就是 `a0/a1`,陷入桩天然能回。⚠️ **`T` 必须 ≤ 一个机器字**,否则退化成隐藏指针 |
| **不透明句柄** | §1.1;且真内核不能假设进程模型 / 全局命名空间,句柄必须是**调用方上下文相关**的 |
| **`enum class` 错误码,不透传 errno** | errno 是 POSIX 的。⚠️ 但**映射 ≠ 模拟**:查表翻译不是模拟层,造 fd 命名空间才是 |
| **自由函数,不要 vtable** | 虚表 = 把结构体布局写进 ABI,而「只增不改」保护不了它(加一个方法就改布局) |
| **没有 `open(path)`** | WASIp1 的教训:照抄 fd + 路径命名空间 ⇒ 每个非 POSIX 宿主都要模拟 preopen / `openat` |

---

## 4. 能力组件化:ADL 探测 + 兜底重载

⚠️ **草案在这里错了一整节。** 它说:

> `requires` 作用在不存在的限定名上是硬错误 ⇒ 「能力 = 符号在不在」这条路 C++ 内测不出来
> ⇒ 声明必须永远齐全,能力要放进一个 `caps` 结构体。

**第一句对,后面全错。** 硬错误只发生在**限定名**上;**非限定名 + ADL** 在模板里是
dependent 的,`requires` 会老实求值为 `false`。

ⓘ **实测(llvm 22.1.8,真 C++20 模块,不是头文件)**:

```cpp
// ⛔ 限定名:名字不存在 ⇒ 硬错误
if constexpr (requires(S s) { kal::seek(s, 0); })

// ✅ 非限定 + ADL:名字不存在 ⇒ false
template <class S> concept Seekable = requires(S s, long o) { seek(s, o); };
```

⇒ **能力不需要另一个可命名的东西。后端的模块接口本身就是能力声明。**

### 4.1 三件事同时成立(ⓘ 全部实测)

```cpp
export module openkal.stream;
export namespace kal {

struct stream    { unsigned long h; };
struct io_result { unsigned long n; int e; };

// ⭐ 兜底重载的返回类型与真实现不同 —— 这是让「探测」与「兜底」共存的关键
struct unsupported_t {};
template <class> inline constexpr bool always_false = false;

template <class S>
unsupported_t seek(S, long) {
    static_assert(always_false<S>,
        "this openkal backend provides no seekable streams. "
        "openkal.fs hands out descriptors that do.");
    return {};
}

// 探测:要求返回真类型,兜底自动落选
template <class S> concept HasSeek =
    requires(S s, long o) { requires __is_same(decltype(seek(s, o)), io_result); };

}
```

后端只需要**声明并定义真实现**,不需要任何配置:

```cpp
export module openkal.uart;
export import openkal.stream;
export namespace kal { io_result seek(stream, long) { /* … */ } }
```

| 场景 | 结果 | ⓘ |
|---|---|---|
| 有后端,调用 `seek(s, 0)` | ✅ 编过,真实现赢重载 | 实测 |
| 有后端,`HasSeek<stream>` | ✅ **真** | 实测 |
| 无后端,`HasSeek<stream>` | ✅ **假**(不是硬错误)⇒ `if constexpr` 可优雅降级 | 实测 |
| 无后端,**强行调用** | ⭐ **编译期报错,文案是规范自己写的那句** | 实测 |

⚠️ **这里有一个必须踩过才知道的坑**:如果兜底重载的返回类型**和真实现一样**,
`HasSeek` 在没有后端时也是**真** —— 因为 ADL 找到了兜底,而 `requires` 不实例化函数体,
`static_assert` 不会触发。**两个机制互相干扰,靠返回类型区分才能共存。**
ⓘ 我第一版就是这么写的,测出来是真才发现。

### 4.2 ⇒ 草案里三样东西被删掉了

| 删掉的 | 为什么不再需要 |
|---|---|
| `caps` 结构体 | 后端的模块接口就是声明 |
| `openkal.stream.caps` 模块 | 同上 |
| `capabilities.toml` | ⭐ **生态整洁性**:mcpp 的一切都在 `mcpp.toml` 里,不该为这个引入第二份配置 |

**后端选择仍然只是条件依赖**(§4.3),整条链路**零新增配置、零新增引擎轴**。

### 4.3 ⭐ 模块名的归属:后端提供「接口名」

这是全套设计里最容易写错的一处,而且草案写错过 —— 它让应用 `import openkal.uart;`,
**那等于把源码钉死在后端上**,正好推翻 openkal 的立身之本。

⚠️ 但它掩盖了一个真实约束。ⓘ **三条实测(mcpp 2026.8.19.4 / gcc 16.1.0)**:

| | 结果 |
|---|---|
| 传递依赖的模块能不能 import | ✅ **能** —— app 只依赖 backend,可以 `import` iface 的模块 |
| **ADL 能不能到达未 import 的模块** | ⛔ **不能** —— `error: 'seek' was not declared in this scope` |
| ⇒ 所以后端的声明**必须在应用 import 的那个模块里** | — |

⭐ **解法:`openkal.stream` 这个模块名由「后端」提供,接口包用另一个名字。**

```
openkal.decl.stream   ← 接口包:类型、兜底重载、concepts
openkal.stream       ← 由「后端」提供:export import openkal.decl.stream; + 真实现
```

```cpp
// 接口包
export module openkal.decl.stream;
export namespace kal { struct stream{…}; struct io_result{…};
                       template <class S> unsupported_t seek(S, long) {…}
                       template <class S> concept HasSeek = …; }

// 后端包 —— 它提供「接口名」
export module openkal.stream;
export import openkal.decl.stream;
export namespace kal { io_result seek(stream, long) { … } }
```

```cpp
// 应用:后端在源码里无名
import openkal.stream;
int main() {
    kal::stream s{1};
    static_assert(kal::HasSeek<kal::stream>);
    return seek(s, 0).n;                        // ADL 找到后端的实现
}
```

ⓘ **端到端跑通**(app 只写 `import openkal.stream;`,concept 为真,返回后端的值)。

⚠️ ⚠️ **一个必须踩过才知道的坑**:接口模块**不能**叫 `openkal.stream.decl`。
ⓘ 实测直接 ninja 自环:

```
ninja: error: dependency cycle: gcm.cache/openkal.stream.gcm -> gcm.cache/openkal.stream.gcm
```

模块图把点号读成了层级关系。⇒ **ABI 模块名不能是接口名的点号延伸**,
`openkal.decl.stream` 可以,`openkal.stream.decl` 不行。

### 4.4 后端怎么被选中:只用 `mcpp.toml`

```toml
# 后端包 openkal-uart
[dependencies]
openkal = { version = "0.1" }          # 它 export import 的那个
```

```toml
# 消费者:只写后端,按 target 选
[target.'cfg(os = "linux")'.dependencies]
openkal-linux = "0.1"
[target.'cfg(os = "none")'.dependencies]
openkal-uart  = "0.1"
```

源码 `import openkal.stream;` 两个 target 一字不改 —— ⓘ 这正是 KA1 测到的
「同一份 `app.cppm`,零 `#if`,两个后端」。

⚠️ 两个后端同时进图 ⇒ 两个包都导出 `openkal.stream` ⇒ 模块名冲突。
基数为 1(§12)使这成为用户错误,而且是**编译期**被发现的。

#### ⭐ 消费者要声明几个依赖:两个

```toml
[dependencies]
openkal = "0.1"                # ① 契约:openkal 本身就是那份规范

[target.'cfg(os = "linux")'.dependencies]
openkal-linux = "0.1"          # ② 实现:可替换的那一半
[target.'cfg(os = "none")'.dependencies]
openkal-uart  = "0.1"
```

技术上 ① **可以省略**(后端已经把它拉进来了,模块也 `export import` 了)。
**但不该省**,理由只有一条,而且是硬的:

> ⭐ **① 是应用真正耦合的东西,而且它让「契约版本不匹配」变成解析期错误。**

应用写 `openkal = "0.2"`、后端只支持 `"0.1"` ⇒ **依赖解析当场失败**;
省掉 ① 的话,同一个问题要等到**编译期**才以一堆签名不匹配的形式冒出来。

而且这与生态里已被验证的形状一致:Rust 的 `embedded-hal`(trait 包)+ 板级包,
应用同时依赖两者;`log` + `env_logger` 也是同一个形状。

| | 只写后端 | ⭐ 契约 + 后端 |
|---|---|---|
| 行数 | 1 | 2 |
| 契约版本由谁定 | ⚠️ 后端 | **应用** |
| 版本不匹配何时暴露 | ⚠️ 编译期,一堆签名错误 | **解析期,一条消息** |
| 换后端要改几行 | 1 | 1(① 不动) |

⚠️ ① 看起来「声明了却没 import」—— 那是表象:应用 `import openkal.stream;` 时,
后端 `export import openkal.decl.stream;` 把它带了进来。**①的作用是钉版本,不是给 import 用。**

#### ⚠️ 命名:丑的那一半要落在实现者身上

**openkal 本身就是接口/ABI,包名不该再带 `-abi`。**

| 谁写 | 名字 | 谁看得到 |
|---|---|---|
| 应用 | `import openkal.stream;` · `openkal = "0.1"` | **所有人** —— 干净 |
| 后端作者 | `export import openkal.decl.stream;` | **只有实现者** |

⇒ 两个模块名是语言逼出来的(§4.3),但**限定词只出现在实现者那一侧**。

#### ⚠️ 后端拥有应用可见的名字 ⇒ 碎片化风险,靠规范 + conformance 机械地封住

这是这套形状唯一的真代价:`openkal.stream` 由后端提供,理论上后端可以往里塞
非标准的东西,而应用**察觉不到自己用了厂商扩展**。

⭐ 但**后端能塞的东西是有界的**,而且边界是语言给的:

| 后端**不能** | 因为 |
|---|---|
| 重定义 `stream` / `io_result` / `unsupported_t` | ⓘ **实测被编译器拒**:`redeclaring 'struct kal::io_result@openkal.decl.stream' in module 'openkal.stream' conflicts with import` |
| 改 concepts 的语义 | 同上 |
| 改兜底重载 | 同上 |
| **只能**:加 spec 列出的那些函数的实现,**或额外的重载** | ← 唯一的自由度 |

⇒ **规范只需要封住最后一行**,而它是**静态可查**的:

> **SPEC**:`openkal.stream` 导出的名字集合**必须等于** spec 列表 ∩ 该后端实现的能力。
> 厂商扩展必须放在**另一个模块名**里(`vendor.foo.stream`),应用要用就得显式 import 它 ——
> **于是「我用了扩展」在源码里是可见的。**
>
> **conformance**:dump 后端模块的导出名集合,与 spec 列表**逐条 diff**。多一个名字 = 不通过。

⚠️ 还要 diff **签名**,不只是名字:后端把 `seek(stream, long)` 写成
`seek(stream, unsigned long)` 时 ADL 仍会经隐式转换选中它,而语义可能不同。

⭐ 这条比 §5 的其它防御更强,因为它**不是行为测试** —— 名字集合是制品的静态属性。

### 4.5 接口层面的缺失(①)

一整个 interface 不存在时,`import openkal.task;` **编译期就找不到模块**。
这一条不变,而且和 §4.1 是同一套失败语义:**编译期,点名,不是运行期返回值。**

---

## 5. ⭐「声称 ≠ 事实」问题:新机制消掉了大半

草案担心的是:后端写 `caps::seek = true` 而 `seek()` 永远失败。
**§4 换成 ADL 之后,结构性的谎话已经说不出来了:**

> **你不能「声称有 seek」而不真的声明一个返回 `io_result` 的 `seek`。**
> 而声明了不定义,是链接错误。

⇒ **声称与实现是同一个制品**,不再需要「同源生成」那套纪律,也不需要 `capabilities.toml`。

### 5.1 ① 让「不支持」在类型系统里**不可表达**(仍然最强)

剩下的问题是**分类错误**,不是撒谎。判据不变:

> **一个操作若能「存在但永远失败」,通常说明它被错误地合并了。**

ⓘ MMU 就是这个的实例:`NoMmu::map()` 对非恒等映射返回 `false` —— 那**不是撒谎,
是这个抽象本来就不该把两族东西装进一个 concept**。当时的结论是把它挪到 `cfg` 轴。

推广:socket 不能 seek,但那**不是后端撒谎,是名词错了** ——
所以 `fs` 的 descriptor 与 `net` 的 socket 各持自己的句柄类型(§2.3),
`kal::seek(socket, 0)` 因为**没有那个重载**而编译失败,不是因为返回错误。

### 5.2 ② 双向 conformance:`false` 也要验

> 后端若不提供 `seek`,**必须真的不导出这个符号** —— `nm` 可查。

⭐ 价值在于**它不是行为测试,是对制品的静态检查**:又快又不可能漏测。
在新机制下这条更强了:导出了符号 ⇒ ADL 就会找到 ⇒ 探测为真 ⇒ 与声称自动一致。
**这条变成了「验证机制本身没被绕过」,而不是「验证后端没撒谎」。**

### 5.3 ③ 过程兜底

conformance 结果进索引元数据:没过 seek 那组的后端,描述符里不允许声称。
这是唯一能约束「实现者根本不跑 conformance」的东西。

### 5.4 ⚠️ 诚实的残余风险

**以上都不证明行为。** 后端可以导出 `seek`、通过 happy path、在某个输入上错。
这是**每一份规范都有的残余风险**(POSIX 也一样),答案只能是 conformance 的覆盖度,
不存在语言层的解法。

⇒ 但和草案相比,**残余从「结构 + 行为」缩小到只剩「行为」** —— 这正是把机制从
「声明一个 bool」换成「声明一个函数」买到的东西。

---

## 6. 实现者视角(对下)

### 6.1 要交付什么

| | |
|---|---|
| 一组 `extern "C"` 定义 | §3 的清单,按你实现的 interface |
| 一个导出真实现的模块 | ⭐ **它就是能力声明**,不需要额外的 caps(§4) |
| conformance 通过记录 | 双向:声称有的能用,没提供的**符号不存在** |

### 6.2 最小实现:一个裸机 UART 后端

```cpp
// openkal-uart/src/stream.cpp
extern "C" {
kal_stream kal_stdout(void) { return kal_stream{1}; }   // h = 一个小索引
kal_stream kal_stderr(void) { return kal_stream{1}; }
kal_stream kal_stdin (void) { return kal_stream{0}; }

kal_io_result kal_stream_write(kal_stream s, const void* buf, uintptr_t n) {
    if (s.h != 1) return kal_io_result{0, KAL_EBADF};
    auto* p = static_cast<const unsigned char*>(buf);
    for (uintptr_t i = 0; i < n; ++i)
        *reinterpret_cast<volatile unsigned char*>(0x10000000) = p[i];
    return kal_io_result{n, 0};
}
}
```

⚠️ **没有第二份配置。** 这个后端不提供 `seek`,做法就是**不声明它** ——
`mcpp.toml` 里只有普通的包信息,`seek.cpp` 不存在,符号也不存在,
而消费者侧 `kal::HasSeek<kal::stream>` 因此为假(§4.1)。

### 6.3 后端怎么被选中

**条件依赖,零新增引擎轴**:

```toml
[target.'cfg(all(arch = "riscv64", os = "none"))'.dependencies]
openkal-uart = "0.1"

[target.'cfg(os = "linux")'.dependencies]
openkal-linux = "0.1"
```

### 6.4 ⚠️ 真内核实现者的额外约束

1. **不能假设进程模型 / 用户内核分界 / 全局命名空间** —— 单地址空间内核里没有「进程」
2. **句柄必须是调用方上下文相关的**,不能是全局整数表下标
3. **返回形状要能过陷入边界** —— 2 字结构在 RISC-V 上就是 `a0/a1`

⇒ 三条**全部指向同一个结论**:不透明一个机器字的句柄。

---

## 7. 上层视角(对上)

### 7.1 应用直接用

```cpp
import openkal.stream;        // ⭐ 只写接口名;提供它的是后端(§4.3)

int main() {
    kal::write(kal::stdout(), "hello\n");

    // 有就用,没有就走别的路 —— 探测是 ADL,不是查表
    if constexpr (kal::HasVectored<kal::stream>) { /* writev 形状 */ }
    else                                         { /* 逐段写 */ }

    // 要求必须有:文案是规范自己写的
    static_assert(kal::HasSeek<kal::stream>,
                  "this program needs seekable streams");
}
```

⭐ **而且不写 `if constexpr` 也不会错**:直接调 `kal::seek(s, 0)` 在没有后端支持时
就是**编译期报错并给出规范的原话**(§4.1 实测)—— 这是 §7 里问「能不能内置」的答案:
**能,而且是默认行为,不需要消费者做任何事。**

ⓘ **KA2 实测:换后端不重编应用** —— 同一个 `app.o` 换掉后端目标文件重链即成。

### 7.2 类型化封装(C ABI 之上的零成本层)

```cpp
export module openkal.stream;
import openkal.stream.caps;

export namespace kal {

enum class error : std::int32_t { ok = 0, badf, again, io, nospace, /*…*/ };

struct [[nodiscard]] io_result {
    std::uintptr_t n;
    error          e;
    constexpr explicit operator bool() const { return e == error::ok; }
};

inline io_result write(stream s, std::span<const std::byte> b) {
    auto r = kal_stream_write(s, b.data(), b.size());
    return { r.n, static_cast<error>(r.err) };
}

// 便利重载:字符串
inline io_result write(stream s, std::string_view sv) {
    return write(s, std::as_bytes(std::span{sv.data(), sv.size()}));
}

}
```

⚠️ `std::span` / `std::byte` / `std::string_view` **全都在 freestanding 可用的 103 个头里**
(ⓘ 实测,见 review §4)—— 这一层在裸机上是可以存在的。

---

## 8. 对接下层硬件:BSP 接线

⭐ **openhal 的设备实例可以成为 openkal 的后端** —— 这是接线,不是层级:

```cpp
// BSP:把这块板的 openhal Serial 接成 openkal 的 stdout
import openhal.serial;

namespace {
    auto& uart = board::uart0();          // openhal 实例(concept,零成本)
}

extern "C" kal_io_result kal_stream_write(kal_stream s, const void* buf, uintptr_t n) {
    if (s.h != 1) return { 0, KAL_EBADF };
    uart.write({static_cast<const std::byte*>(buf), n});   // 内联进来
    return { n, 0 };
}
```

⚠️ **注意成本**:openhal 是 concept,`uart.write` **可内联**;openkal 是 C ABI,
`kal_stream_write` 是一次真调用。这正是两者契约形态不同的原因(§10)。

---

## 9. 对接 libc:picolibc(⚠️ 缝是实测出来的)

ⓘ **实测(picolibc 1.8.12,rv64gc/lp64d)** —— `libsemihost.a` 的 `common_iob.c.o`:

```
d __stdio                    ← FILE 实体
R stdout / stdin / stderr    ← 三个都指向它(同址)
U sys_semihost_putc          ← 未定义
U sys_semihost_getc          ← 未定义
```

而 **`libc.a` 根本不定义 `stdout`** —— picolibc 早就把「C 库」和「控制台在哪」分开了。

⇒ 移植到 openkal = **一个小 .o**:

```cpp
// openkal-picolibc/src/console.cpp
#include <stdio.h>
import openkal.stream;

static int kal_putc(char c, FILE*) {
    auto b = static_cast<std::byte>(c);
    return kal::write(kal::stdout(), {&b, 1}) ? c : EOF;
}
static FILE __kal_stdio = FDEV_SETUP_STREAM(kal_putc, nullptr, nullptr,
                                            _FDEV_SETUP_WRITE);
extern "C" FILE* const stdout = &__kal_stdio;
extern "C" FILE* const stderr = &__kal_stdio;
```

⭐ **成本论据**:`struct __file` 里 `put` **本来就是函数指针**
(`int (*put)(char, struct __file*)`)。接到 `kal` 上**不新增任何一层间接**。

⇒ **`printf` 和 `kal::write` 汇到同一个底,不产生第二条 I/O 路径。**

⚠️ 反向也成立(openkal 在 libc **之上**):hosted 后端的 `kal_stream_write` 可以就是
`fwrite(buf, 1, n, stdout)`。**同一份契约,两个方向。**

---

## 10. 对接 libc++ / gcc 工具链

### 10.1 libc++:36 个名字,ⓘ **实测清单**

ⓘ llvm 22.1.8 的 `__thread/support/` 有 **四个后端**:`pthread.h` `windows.h` `c11.h`
**`external.h`** —— 可插拔线程后端这个形状**在标准库里已经存在**。

`_LIBCPP_HAS_THREAD_API_EXTERNAL = 1` 后,要提供 `<__external_threading>`,
面是 ⓘ **36 个名字**(从 `pthread.h` 数出来的,与调研一致):

```
mutex×4            __libcpp_mutex_t / _lock / _trylock / _unlock / _destroy
recursive_mutex×5  __libcpp_recursive_mutex_t / _init / _lock / _trylock / _unlock / _destroy
condvar×5          __libcpp_condvar_t / _signal / _broadcast / _wait / _timedwait / _destroy
thread×9           __libcpp_thread_t / _id / _create / _join / _detach / _yield / _sleep_for …
once×1             __libcpp_execute_once (+ __libcpp_exec_once_flag)
tls×3              __libcpp_tls_key / _create / _get / _set
```

映射到 `openkal.task` 应当是**一一对应**,不需要适配层:

```cpp
// openkal 侧 <__external_threading> 的实现骨架
using __libcpp_mutex_t = kal_mutex;                       // 不透明,1 字
inline int __libcpp_mutex_lock(__libcpp_mutex_t* m) {
    return static_cast<int>(kal_mutex_lock(*m));          // 直调,零桥接
}
```

⚠️ **这听起来像「又在照抄」,区别是实质的**:`__external_threading` 本来就是给
**非 POSIX 系统**用的插点,它已经过了「非 POSIX 宿主能不能实现」这道筛;POSIX 没有。
**照抄一个可移植性插点 ≠ 照抄一个 OS。**

### 10.2 gcc / libstdc++:gthreads

ⓘ **实测(gcc 16.1.0)**:`bits/gthr-default.h` 里 `__gthread_*` 共 **71** 个名字,
其中 **20** 个是 `__gthread_objc_*` 遗留 ⇒ **真实面 51**,与 libc++ 的 36 高度重合。

⭐ ⓘ 更重要的先例:gcc 自带 **`gthr-single.h`** —— **「没有线程」是一个编译期后端选择**,
不是运行期 `ENOSYS`。**这正是本方案 §4 主张的形状,而且 gcc 已经这么做了三十年。**

⇒ openkal 的 gcc 侧对接 = 提供一份 `gthr-openkal.h`,和 `gthr-posix.h` 平级。

### 10.3 `operator new` 与 `__libcpp_verbose_abort`

这两个是**标准扩展点**,不需要发明契约:

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

ⓘ ⚠️ **`__libcpp_verbose_abort` 必须通过 libc++ 自己的头声明** —— 真符号在 ABI 内联
命名空间 `std::__1::` 里,手写 `namespace std { }` **编得过、链不上、报错一字不变**
(本轮实测踩过)。

### 10.4 ⚠️ openkal 明确**不认领** `thread_local`

ⓘ 实测的静默失败:

```
thread_local int counter;  →  编译 ✅ 链接 ✅ 零未定义符号 ✅ 零诊断 ✅
                           →  运行期通过未设置的 tp 读写垃圾地址
```

`thread_local` **不属于线程契约**,属于 **openarch(TLS 寄存器约定)+ BSP
(`start.S` 设 `tp` + 链接脚本 `.tdata/.tbss`)**。
而 `__external_threading` 里的 `tls×3` 是 **pthread_key 那种动态 TLS**,不是它。

⇒ **规范里必须写死这条边**,否则「我实现了 openkal 的线程」会被读成「`thread_local` 能用了」。
⚠️ 且 libc++/libstdc++ **自己内部就用 `thread_local`**,不是「用户不写就没事」。

---

## 11. C++ 特性 → 解决什么问题

| 特性 | 解决什么 | ⚠️ 限制 |
|---|---|---|
| **modules** | 接口/caps 分离;**模块找不到 = 能力检查**,把链接期错误提前到编译期 | 需要 mcpp 的 `reexport`/provisions 承载 |
| ⭐ **ADL + `requires`** | ⓘ **非限定名**在模板里是 dependent 的 ⇒ 缺失时求值为 `false` 而不是硬错误。**能力探测不需要额外的数据结构,后端的模块接口就是声明** | ⚠️ **限定名**(`kal::seek`)仍是硬错误 —— 两者的差别是这套设计成立的全部基础 |
| ⭐ **兜底重载 + `static_assert`** | 没有后端时直接调用 ⇒ **编译期报错,文案是规范写的** | ⚠️ 兜底的**返回类型必须与真实现不同**,否则探测恒为真(ⓘ 实测踩过) |
| **`if constexpr`** | 消费者按能力降级,未选中的分支**不实例化** | — |
| **concepts** | 组合 caps;openhal 的多提供者共存 | ⚠️ ⓘ **只检查语法不检查语义**(K1/K2),③ 类能力不能用它 |
| **非类型模板参数(能力位)** | 让「不支持」**在类型系统里不可表达**(§5.1) | — |
| **`static_assert` + 文案** | 把缺能力变成**人能读**的编译错,而不是链接器的符号名 | — |
| **`enum class`** | 封闭错误集合,不透传 errno | — |
| **`[[nodiscard]]`** | `io_result` 不能被静默丢弃 | — |
| **`std::span` / `std::byte` / `string_view`** | C ABI 之上的类型化层 | ⓘ 三者都在 freestanding 可用的 103 头里 |
| **`_Noreturn` / `[[noreturn]]`** | `kal_abort` 的控制流事实进类型 | — |
| ~~virtual / vtable~~ | — | ⛔ 把结构体布局写进 ABI,「只增不改」保护不了 |
| ~~exceptions / RTTI~~ | — | ⛔ 裸机整图关闭(mcpp 2026.8.19.4 起) |
| ~~`requires` 探测**限定名**~~ | — | ⛔ ⓘ 实测:硬错误。**改用非限定 + ADL** |
| ~~`caps` 结构体 / `capabilities.toml`~~ | — | ⛔ 被 ADL 机制整个取代;且第二份配置文件违背生态整洁性 |

---

## 12. 与 openhal / openarch 的关系:**契约形态由基数决定**

| | 一个程序里几个实现 | 契约 | 为什么 |
|---|---|---|---|
| **openarch** | **1**(一颗 CPU) | concept **+ LTO 必需** | ⓘ K3:`local_irq_disable()` 本该一条 `csrci`;跨模块 `-O2` 退化成真 `jal`,`-flto` 才恢复 |
| **openkal** | **1**(一个内核) | **C ABI** | ⓘ 一次 call 可忽略(本来就陷入);H1 |
| **openhal** | **N**(每条总线/设备) | **concept** | ⓘ H3:多提供者共存 + 零成本可内联 |

⭐ **openarch 与 openhal 都是 concept,但理由相反**:一个因为调用成本灾难性,
一个因为要多实现共存。openkal 两种压力都没有 ⇒ 用最简单也最稳定的 C ABI。

**openkal.stream 与 openhal.Console 的边界**:

- openkal 的流 = **程序的标准流**:一个、环境给的、借用的 ——「我的诊断往哪去」
- openhal 的 `Console`/`Serial` = **你打开的一个设备**:多个、拥有的、有类型

**是两个名词,不是一个东西的两层。** 自然组合见 §8。

---

## 13. 判据与门

### 13.1 双向准入判据(⚠️ 文档今天只有对下那一半)

| 方向 | 判据 | 怎么数 |
|---|---|---|
| **对下** | 四个后端自然实现,**没有模拟层** | 写出四个后端的 `kal_stream_write`,数「只为迁就形状而存在」的行数 —— 任何后端需要**表/注册中心/路径解析器**,形状就错了 |
| **对上** | libc / libc++ / gcc 的**既有插点**直接落上,**没有适配层** | `stdout->put` · `operator new` · `__libcpp_verbose_abort` · 36 个 `__libcpp_*` · `gthr-*.h` —— 同样数桥接行数 |

⇒ **对上判据不满足 ⇒ 说明资源分解切错了,回去改分解** —— 而不是改边界迁就标准库。

### 13.2 D0–D3 的门(来自第一阶段计划 §7,不改)

| 阶段 | ⭐ 继续的判据 | ⚠️ 停止的信号 |
|---|---|---|
| **D0** | **有第三方实现了第三个后端** —— 这才是 D 能否成立的真变量 | 半年内无人实现 ⇒ 停在 D0,当 mcpp 内部设施 |
| **D1** | 三个官方后端全过 conformance;⚠️ windows 后端**不经 MSVC CRT 的 POSIX 兼容层** | windows 做不出来而不模拟 ⇒ **SPEC 形状错了**,回炉 |
| **D2**(openhal) | 同一个驱动包既跑裸机 MCU 又跑 Linux | 驱动作者不来 ⇒ 停;不影响 openkal |
| **D3**(openarch) | ⚠️ 两个最硬原语不碎(上下文切换 / 页表项) | 碎掉 ⇒ **停**;⚠️ 碎在这里后面全是幻觉 |

---

## 14. 明确不做的

| | 为什么 |
|---|---|
| **`open(path)` 进 core** | WASIp1 的教训;开文件在 `openkal.fs`(自有句柄类型),不在 core,也不经一个通用命名接口(§2.3) |
| **运行期 `ENOSYS`** | 部分性用「链接哪些 interface」表达;⭐ gcc 的 `gthr-single.h` 已是三十年的先例 |
| **errno 透传** | 封闭 `enum class`;映射不是模拟 |
| **虚接口 / vtable** | ABI 脆弱,与「只增不改」冲突 |
| **`thread_local` 的保证** | 属于 openarch + BSP(§10.4) |
| **引擎认识 openkal** | ⚠️ 计划 §7.2 第一行:**引擎永不认识这三层**;后端选择 = 条件依赖,零新增轴 |
| **现在就实现** | D0 的门是「第三方来了没有」,不是「能不能写出来」 |

---

## 15. 撤回记录

⚠️ 草案里被 review 推翻的两条,连同它们**为什么当时看起来对**:

| 撤回的 | 当时的理由 | 为什么错 |
|---|---|---|
| **`openkal.namespace`**(取代 fs/net) | 「文件 / socket / UART 给你的都是 stream,只是命名方式不同」 | ① 触犯本文自己的 §5.1 规矩(caps 成为不相干能力并集);② URI 解析器**就是**模拟层,违反对下判据;③ 引用的 WASIp2 先例是**误读**(它分开资源种类,只共享 stream 类型) |
| **扩 `cfg()` 文法支持 `cfg(mmu)`** | K1/K2 说「MMU 是能力轴不是契约」 | 那条结论是关于 **openarch** 的;openkal core 逐个接口查下来**一条 ③ 类语义轴都没有**,而且 triple 本身已承载大部分 |
| **`caps` 结构体 + `capabilities.toml`** | 以为「`requires` 测不了缺失的名字」⇒ 能力必须放进另一个可命名的东西 | ⓘ **只对限定名成立**。非限定 + ADL 在模板里是 dependent 的,缺失时求值为 `false`。⇒ 后端的模块接口本身就是能力声明,**两样东西整个删掉**,也不需要第二份配置 |
| **应用 `import openkal.uart;`** | 想让「接口随后端而来」 | ⛔ **把源码钉死在后端上**,推翻 openkal 的立身之本。ⓘ 而且掩盖了真约束:**ADL 到不了未 import 的模块**(实测)。正解是**后端提供接口名**(§4.3) |

⭐ 三条的共同形状:**都是「我有一个漂亮的统一」,而漂亮的统一把两件本来不同的事合并了。**
这与 §5.1 给出的判据是同一条 —— 只是那一节我用它去检查别人的后端,没有用它检查自己的分解。

---

## 16. 综合 review 发现的开放问题

撤回两条之后重新通读,又找出六条 —— 都是**规范必须表态、但草案没表态**的。

### 16.1 ⚠️ 两个堆(ⓘ 实测,不是理论)

ⓘ picolibc `libc.a` 的 `vfprintf.c.o` **引用 `free`** —— **printf 与分配器是耦合的**。
而固件里已有一整套 `malloc`/`free`/`__malloc_sbrk_aligned`/`__fallback_sbrk`。

⇒ 若 `operator new` 走 `kal_alloc`(openkal 的 arena)而 `printf` 走 picolibc 的
`malloc`,**同一块 RAM 上会有两个分配器**,而且都想长 sbrk。

**规范必须写死一条**:

> **后端上若已存在 libc 分配器,`kal_alloc` 必须实现在它之上,而不是与它并列。**

三种配置:

| 后端 | 做法 | |
|---|---|---|
| picolibc | `kal_alloc` → `malloc`(openkal 在 libc **之上**) | ✅ 一个堆 |
| 零 libc | `kal_alloc` → 自带 arena;没有 libc malloc | ✅ 一个堆 |
| ⛔ 自带 arena **且** libc malloc 也在 | — | **禁止** |

⚠️ 这条部分可 conformance 化:检查固件里 `sbrk` 的消费者是不是只有一个。

### 16.2 ⚠️ 错误集合的封闭性

草案说「封闭 `enum class`,不透传 errno」。但 POSIX 有约 130 个 errno,
一个 ~15 项的封闭集合**必然丢信息**。规范要表态:

- 丢掉细节(简单,但诊断质量下降),还是
- 留一个 `other` + **后端私有的细节通道**(`kal_last_error_detail()`),
  ⚠️ 但那是全局状态,和 errno 一样的毛病

**倾向**:封闭集合 + 细节通道**只用于日志**,不进控制流。需要写进 SPEC。

### 16.3 ⚠️ C ABI 没有版本,而结构体布局会被永久冻结

「每个 interface 独立版本 + 只增不改」保护得了**函数**(加新函数是可加的),
保护不了**结构体**:`kal_io_result` 的布局一旦发布就永远不能动。

选项:符号带版本后缀(`kal_stream_write_v1`,难看但诚实),或**明确声明这些布局永久冻结**。
草案默认了后者却没写出来。

### 16.4 ⚠️ 拥有 vs 借用,C ABI 强制不了

标准流是**借用**(不配 `close`),`openkal.fs` 的 descriptor 是**拥有**(必须 close)。
C ABI 里没有 RAII,谁来保证?

⇒ 类型化 C++ 层可以包 RAII,但**那不是规范的一部分**,Rust/C 消费者拿不到。
规范至少要把「哪些句柄是拥有的」写清楚,并规定重复 close 的行为。

### 16.5 ⚠️ 线程安全未规定

同一个 `kal_stream` 被两个 task 同时 `write`,是什么行为?
POSIX 至少规定了 `PIPE_BUF` 以内的原子性。**草案一个字没提。**
这条在有 `openkal.task` 之后立刻变成必须回答的。

### 16.6 ⚠️ sized-free 的方向性成本

`kal_free(p, size, align)` 对 arena 友好(Rust 的做法),但:

- hosted 后端 `kal_free` → `free(p)`,**丢掉 size**:无成本 ✅
- 反向:一个建在 `kal_alloc` 之上的 libc `malloc` **必须自己存 size** ⇒ 每次分配多一个字

⚠️ 与 §16.1 的规则合看,反向配置本来就该避免,所以成本可控 —— 但要写明。

---

## 17. 这一轮 review 的元结论

三条撤回(§15)+ 六条开放问题(§16)里,有一个共同形状值得单独记:

草案的错误分两族。

**族一:一个漂亮的统一,把两件本来不同的事合并了。**

- `openkal.namespace` 合并了「命名」与「资源种类」
- `cfg(mmu)` 把 openarch 的结论搬进 openkal
- 「两个堆」是**没有**合并该合并的(分配器)

而 §5.1 给出的判据 —— **「一个操作若能『存在但永远失败』,说明它被错误地合并了」** ——
本来就能抓住前两条。⚠️ **我只用它去检查别人的后端,没有用它检查自己的分解。**

**族二:测了一种写法,把结论推广到了全部。**

⚠️ `caps` 结构体 + `capabilities.toml` 那整套,建立在**一次实测**上:
「`requires { mcpp::runner("x") }` 是硬错误」。那次实测本身没错,
错在**结论的量词** —— 我写的是「C++ 语言内测不出来」,而真实情况是
**「限定名测不出来」**。非限定 + ADL 一直是可以的。

⇒ 代价是**一整节设计 + 一份多余的配置文件格式**,而验证它只需要三行代码。

⭐ **两族合起来的规律**:一条实测能否定一个做法,**但否定不了一整类做法** ——
写下「X 做不到」之前,要先问「我测的是 X,还是 X 的某一种写法」。

---

## 18. 第二轮综合 review 新增的两条

### 18.1 ⭐ 基数是「每个 interface 一个实现」,不是「每个程序一个后端」

草案 §12 说 openkal 的基数是 1,含糊在于**1 个什么**。澄清:

```toml
[target.'cfg(os = "none")'.dependencies]
openkal-uart  = "0.1"      # 提供 openkal.stream
openkal-arena = "0.1"      # 提供 openkal.memory
```

**一个程序可以从不同提供者拿不同的 interface** —— 这是好事,而且是 §2 资源分解的
自然结果。冲突只发生在**同一个 interface 有两个提供者**时(两个包都导出
`openkal.stream`),那是模块名冲突,**编译期报错**。

⇒ 「基数 1」应当读作:**每个 interface 的实现是 1 个**。契约形态(C ABI)的论据不变。

### 18.2 ⚠️ 兜底重载太贪心,文案会张冠李戴

```cpp
template <class S> unsupported_t seek(S, long) {
    static_assert(always_false<S>, "…no seekable streams…");
}
```

`S` 无约束 ⇒ `kal` 里**任何**类型调 `seek` 都落到这里。
`seek(some_socket, 0)` 会得到「no seekable **streams**」—— 名词错了。

**修法**:兜底要约束到它该管的类型,或者文案改成不带具体名词的。

```cpp
template <class S> requires std::same_as<S, stream>
unsupported_t seek(S, long) { … }
```

⚠️ 这条小,但它是 §5.1 那条判据的又一次应用:**兜底重载的适用范围也是一种「分类」**,
分类过宽,诊断就会指向错误的地方。

---

## 19. 设计现状小结(第二轮 review 后)

| 维度 | 状态 |
|---|---|
| **位置无关**(上/下 libc) | ✅ 由不透明句柄承载,ⓘ 三种后端实测 |
| **划分依据** | ✅ 资源种类;⚠️ 曾塌缩过头(namespace),已撤回 |
| **core 边界** | ✅ abort + stream + memory,判别式是「实现 vs 模拟」 |
| **能力探测** | ✅ ADL + 兜底重载,ⓘ 真模块上三行为同时成立;**零额外配置** |
| **模块名归属** | ✅ 后端提供接口名(语言逼出来的),⚠️ 碎片化靠 spec + 名字集合 diff 封住(静态可查) |
| **依赖形状** | ✅ 契约 + 后端两条,契约那条用来**钉版本** |
| **引擎改动** | ✅ **零** |
| **对下/对上判据** | ✅ 双向且可数(数桥接行数) |
| ⚠️ **开放问题** | §16 六条 + §18 两条,**全部需要 SPEC 表态** |
| ⚠️ **最大风险** | 不是技术 —— 是 **D0 的门:有没有第三方来实现第三个后端** |
