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

## 4. 能力组件化:三种机制,不能混用

⚠️ **先说一条会砍掉最直觉做法的实测**(mcpp 2026.8.19.4 上验的):

```cpp
if constexpr (requires { kal::seek(s, 0); })   // ⚠️ 名字不存在 ⇒ 硬错误,不是 false
```

`requires` 作用在**不存在的限定名**上是 ill-formed。
⇒ **「能力 = 符号在不在」这条路,C++ 语言内测不出来。**
**声明必须永远齐全,能力必须是另一个可命名的东西。**

| | 例子 | 机制 | 失败时机 |
|---|---|---|---|
| **① 接口在不在** | 有没有 `openkal.task` | **模块导入** | 编译期,点名模块 |
| **② 接口内的操作在不在** | 有 `write` 没 `seek` | **constexpr caps** | 编译期,`static_assert` 文案 |
| **③ 语义能力** | 抢占式 vs 协作式调度 | **`cfg()` 轴** | 依赖解析期 |

⚠️ **③ 绝不能做成 concept**:ⓘ K1/K2 实测 `RiscvSv39` 与 `NoMmu` **同时满足**同一个
`AddressSpace` concept,通用代码在 NoMmu 上**静默失败**。**concept 检查语法,不检查语义。**

⭐ **但 openkal core 里一条 ③ 都没有** —— 见 §4.3。K1/K2 那个例子是 **openarch** 的
`AddressSpace`,不是 openkal 的。

### 4.1 ①:让模块解析本身成为能力检查

```
openkal.stream          ← 接口:extern "C" 声明 + concepts + 类型化封装
openkal.stream.caps     ← 由「后端」提供
```

```cpp
export module openkal.stream;
import openkal.stream.caps;   // 没有后端 ⇒ 编译期找不到模块,点名它
```

⇒ **「没有实现者」不是链接器吐未定义符号,而是编译器说模块不存在。**

⚠️ **接法要注意方向。** 草案曾写「接口包 `reexport` 后端的 provisions」——
**错的**:ⓘ `reexport` 是**向下游**传播(`grpc` 把 protoc 透给它的用户),
而这里需要的是接口拿到**消费者所选后端**提供的东西,方向相反,`reexport` 表达不了。

正确接法是**反过来**,而且正好是 `reexport` 的本意:

```toml
# 后端包 openkal-uart 的 manifest
[dependencies]
openkal-stream = { version = "0.1", reexport = true }   # 把接口透给我的消费者
```

```toml
# 消费者:只写后端,按 target 选;接口随之而来
[target.'cfg(os = "linux")'.dependencies]
openkal-linux = "0.1"
[target.'cfg(os = "none")'.dependencies]
openkal-uart  = "0.1"
```

源码 `import openkal.stream;` 两个 target 一字不改。
⇒ **零新增引擎轴,且用的是已有机制的本意。**

⚠️ 两个后端同时进图会造成 `openkal.stream.caps` 模块重复定义 —— 基数为 1(§12)
使这成为用户错误,mcpp 会报模块冲突。

### 4.2 ②:能力是**值**

```cpp
export module openkal.stream.caps;
export namespace kal::stream_caps {
struct caps {
    static constexpr bool sequential = true;
    static constexpr bool seek       = false;   // 这个后端没有
    static constexpr bool vectored   = true;
    static constexpr bool nonblock   = false;
};
}
```

```cpp
if constexpr (kal::stream_caps::caps::seek) { kal::seek(s, off); }

static_assert(kal::stream_caps::caps::seek,
    "this backend has no seekable streams; openkal.fs hands out "
    "descriptors that do");
```

组合 = **concept over caps**,不是 concept over 符号:

```cpp
template <class C> concept Sequential = C::sequential;
template <class C> concept Seekable   = Sequential<C> && C::seek;
```

### 4.3 ③:⚠️ openkal core 用不到它 —— 一条被撤回的引擎改动

草案曾要求扩 `cfg()` 文法以支持 `cfg(mmu)` 这类能力谓词。**重估后撤回。**

那条结论的出处是 K1/K2,而 K1/K2 测的是 **openarch 的 `AddressSpace`** —— 草案把它
搬进了 openkal。逐个接口检查 openkal 有没有「存在但语义不同」的能力:

| interface | 有 ③ 类语义轴吗 |
|---|---|
| `abort` | 无 |
| `stream` | seek / nonblock / vectored 都是**操作**(②类) |
| `memory` | 静态 arena vs 按需分页 = **容量**不是能力;分配失败到处都有定义 |
| `time` | monotonic vs wall 是**两种资源**,不是一个资源的两种语义 |
| `task` | ⚠️ 抢占 vs 协作**确实是** —— 但那是 D1 以后的事 |

⇒ **core(abort + stream + memory)一条语义轴都不需要,①② 足够。**

而且 **triple 本身已经承载了大部分**:`riscv64-none-elf` 与 `riscv64-linux-gnu` 的
区别里就包含了 MMU 用不用。今天已有的文法足以选后端。

⭐ **撤回后,本方案变成零引擎改动。**

## 5. ⭐ caps 撒谎问题:四层防御,按强度排

**问题**:后端可以写 `caps::seek = true` 然后 `seek()` 永远失败。
这是 K1/K2 的问题换了一层出现 —— caps 检查的是**有没有这个字段**,不是**它说的是不是真的**。

### 5.1 ① 让「不支持」在类型系统里**不可表达**(最强)

如果一个操作可以「存在但永远失败」,**通常说明它被错误地合并了**。

ⓘ MMU 就是这个的实例:`NoMmu::map()` 对非恒等映射返回 `false` —— 那**不是撒谎,
是这个抽象本来就不该把两族东西装进一个 concept**。当时的结论是把它挪到 `cfg` 轴。

推广到 `seek`:socket 不能 seek,但那**不是后端撒谎,是名词错了**。

```cpp
// 不是:caps::seek = false
// 而是:句柄类型里根本没有那个能力位
using console = kal::stream_of<kal::cap::write>;                   // seek 不在
using file    = kal::stream_of<kal::cap::read, kal::cap::write,
                               kal::cap::seek>;
kal::seek(c, 0);   // ⚠️ 编译错误:重载要求 seek 位
```

⇒ **能撒的谎少了一整类**,因为「`caps::x = true` 而 `x()` 无意义」在类型层面构造不出来。

### 5.2 ② caps 与实现**同源生成**

不要让后端手写 caps。**一张表同时产出 caps 模块和源码选择**:

```
backend/capabilities.toml        ← 唯一来源
   ├─→ mcpp:generated=  →  openkal.stream.caps
   └─→ 源码选择         →  seek.cpp 编不编进去
```

⇒ **撒谎要改表,而改表就把实现一起删了。**
`mcpp:generated=` 与源码选择**都是今天已有的指令**,不需要新机制。

### 5.3 ③ 双向 conformance:`false` 也要验

> `caps::seek == false` 的后端,**必须不导出 seek 符号** —— `nm` 可查。

⭐ 价值在于**它不是行为测试,是对制品的静态检查**:又快又不可能漏测。
两侧都钉,caps 才从「声称」变成「事实」。

### 5.4 ④ 过程兜底

conformance 结果进索引元数据:没过 seek 那组的后端,描述符里不允许声称。
这是唯一能约束「实现者根本不跑 conformance」的东西。

### 5.5 ⚠️ 诚实的残余风险

**这四条都不证明行为。** 后端可以导出符号、通过 happy path、在某个输入上错。
这是**每一份规范都有的残余风险**(POSIX 也一样),答案只能是 conformance 的覆盖度,
不存在语言层的解法。

⇒ **顺序很重要**:先靠 ① 让错误分类不可表达,再靠 ② 让撒谎自毁,③④ 只是兜底。
一上来就写更严的 conformance,是在给一个**本可以消除的问题**加检查。

---

## 6. 实现者视角(对下)

### 6.1 要交付什么

| | |
|---|---|
| 一组 `extern "C"` 定义 | §3 的清单,按你实现的 interface |
| 一个 `<interface>.caps` 模块 | ⚠️ **生成的**,不是手写的(§5.2) |
| conformance 通过记录 | 双向:声称有的能用,声称没有的**符号不存在** |

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

```toml
# capabilities.toml —— caps 与源码选择的唯一来源
[stream]
sequential = true
seek       = false      # ⇒ seek.cpp 不编进去,符号也不存在
vectored   = false
```

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
import openkal.stream;

int main() {
    kal::write(kal::stdout(), "hello\n");
    if constexpr (kal::stream_caps::caps::vectored) { /* 用 writev 形状 */ }
}
```

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
| **`constexpr` caps 描述符** | 能力是**值**不是符号 —— 绕开「`requires` 测不了缺失名字」 | — |
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
| ~~`requires` 探测符号~~ | — | ⛔ ⓘ **实测:硬错误,不是 `false`** |

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
| **「接口包 `reexport` 后端」** | 以为 mcpp 的承载件现成 | ⓘ `reexport` 是**向下游**传播,方向相反。正确接法是**后端 reexport 接口**(§4.1),恰好是该机制的本意 |

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

⭐ **草案的错误全部是「一个漂亮的统一,把两件本来不同的事合并了」**:

- `openkal.namespace` 合并了「命名」与「资源种类」
- `cfg(mmu)` 把 openarch 的结论搬进 openkal
- 「两个堆」是没有合并该合并的(分配器)

而 §5.1 给出的判据 —— **「一个操作若能『存在但永远失败』,说明它被错误地合并了」** ——
本来就能抓住前两条。

⚠️ **我只用它去检查别人的后端,没有用它检查自己的分解。**
⇒ 判据要对**自己的设计**先跑一遍,再拿去当准入门槛。
