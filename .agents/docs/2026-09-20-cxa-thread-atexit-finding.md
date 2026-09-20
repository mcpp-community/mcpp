---
subject: review
status: landed
---

# `__cxa_thread_atexit` 在 openkal-Windows 上:两层都已定位并修复

> **2026-09-21 收尾。** 第二层已定位,修法已实测,发在
> `openkal-llvm-runtime@0.15.0`。下文 §1–§5 保留当时的记录(包括两个被否掉的假设),
> §7 是结论。**§4 的第一个假设当时被判为「否」,而它其实是对的——错的是那次探针的
> 构造,见 §7。**

- 日期：2026-09-20
- 来源：mcpp-index 的 30-member 重测,doctest 与 spdlog 两个成员停在
  `ld.lld: error: undefined symbol: __cxa_thread_atexit`
- 结论：**不要只修第一层。** 只修它会把一个构建期的响亮失败换成一个运行期的静默失败。

---

## 1. 最小复现(五行)

```cpp
#include <cstdio>
struct D { int v; ~D() { std::printf("dtor %d\n", v); } };
thread_local D t{7};
int main() { std::printf("v=%d\n", t.v); return 0; }
```

依赖 `openkal-llvm-runtime = "0.12.0"`。

| 目标 | 读数 |
| --- | --- |
| `x86_64-linux-gnu` | 构建并运行 |
| `x86_64-windows-gnu` | `ld.lld: error: undefined symbol: __cxa_thread_atexit` |

## 2. 第一层:已定位

`llvm/libcxxabi/src/cxa_thread_atexit.cpp:109`

```cpp
#if defined(__linux__) || defined(__Fuchsia__)
extern "C" {
  _LIBCXXABI_FUNC_VIS int __cxa_thread_atexit(Dtor, void*, void*) throw() { ... }
}
#endif
```

**上游只在这两个系统上导出这个符号**,因为在别处别人已经导出了。实测:

```
$ llvm-nm --defined-only .../x86_64-w64-mingw32/lib/libmingw32.a | grep -c __cxa_thread_atexit
1
```

——普通 MinGW 目标由 `libmingw32.a` 提供。openkal 把 C 库连同它的运行时一起换掉,
于是**两边都以为对方会提供**。这与本轮其他几处同形:上游问的是「这是哪个 OS」,
而真正的问题是「这个映像里还有没有第二个 C++ 运行时」。

文件里那段 fallback(`#ifndef HAVE___CXA_THREAD_ATEXIT_IMPL`,把析构挂在一个
`__libcpp_tls_key` 上)是**完整的**,只是被这个守卫挡在导出之外。

## 3. 把守卫放开之后:链接通了,析构不跑

在 `#if` 上加一条本包自己的条件之后:

| | Linux | Windows(wine) |
| --- | --- | --- |
| 链接 | 通过 | **通过**(此前失败) |
| `thread_local` 析构是否运行 | **运行** | **不运行** |

`examples/cxx` 加一条断言(在一个 spawned thread 里构造带析构的 `thread_local`,
join 之后查标志),Linux `ok`、Windows `FAIL`。

## 4. 第二层:两个假设,都被实测否掉

**假设一:PE 上 `thread_local` 走 emutls,它自己的 pthread key 先于 libc++abi 的
key 被析构,于是 `run_dtors` 读到的链表已经空了。**

否。key 析构里读 `thread_local` 在两个目标上都读到正确的值:

```cpp
static thread_local int marker = 0;
static void dtor(void*) { saw = marker; }
// 线程里 marker = 42; pthread_setspecific(k, ...)
```

```
Linux   : key destructor read thread_local as 42 (expect 42)
Windows : key destructor read thread_local as 42 (expect 42)
```

**假设二:`__cxa_thread_atexit_impl` 是弱符号,在 PE 上解析成了非空,于是走了
`if (__cxa_thread_atexit_impl)` 那一支而不是 fallback。**

否。两个目标上都是 null:

```
Linux   : __cxa_thread_atexit_impl = 0   -> fallback branch
Windows : __cxa_thread_atexit_impl = 0   -> fallback branch
```

另有一条已确认为**正常**的:`pthread_key_create` 的析构在 Windows 上**会**在线程
结束时运行(`pthread tsd dtor ran=1`)。所以不是 TSD 机制本身。

## 5. 为什么本轮不发这个补丁

| | 现状 | 只修第一层 |
| --- | --- | --- |
| 失败在哪 | **链接期** | 运行期 |
| 调用方能否看见 | **能,链接器点名符号** | **不能,注册成功而析构不发生** |

第二种正是 openkal-musl 的 `[c-abi-absent]` 里叫作 `accepted-no-effect` 的那个形状,
也是 SPEC §6.1 把「运行期报告不支持」称为缺陷的理由。**一个响亮的构建期失败,
比一个静默的运行期失败好。**

补丁已撤回。第一层的定位、第二层的两个否定结果,以及最小复现,都在上面——下一个
接手的人不必从 30 个成员的诊断重新走一遍。

## 6. 下一步的判据

1. 在 Windows 上确认 `run_dtors` **是否被调用**(在 fallback 里打一行,重建运行时)。
   - 被调用而链表为空 ⇒ 注册那一侧的问题
   - 没被调用 ⇒ `__libcpp_tls_create` / key 注册那一侧的问题
2. 无论结论如何,修法必须让「析构会跑」与「链接会过」同时成立,或者两者都不成立。


---

## 7. 第二层:已定位(2026-09-21)

### 读数

按 §6 写下的第一条判据做——在 fallback 的 `run_dtors` 里打一行,并同时打印
`&dtors`:

```
[probe] DtorsManager ctor: creating key
[probe] registered dtor, dtors=0x7ffffe994680, key=0x2, &dtors=0x7ffffe9946a8
[prog]  in thread, v=7
[probe] run_dtors called, dtors=0, alive=0, &dtors=0x7ffffe9946c8
[prog]  after join, ran=0 (expect 7)
[probe] run_dtors called, dtors=0, alive=0, &dtors=0x7ffffe994708
```

`run_dtors` **被调用了**——§6 的第二支排除。而 **`&dtors` 三次都不同**,在同一个线程里。

### 真因

`__thread DtorList* dtors` 在本包为 PE 采用的 `-femulated-tls` 下由 emutls 提供。emutls
把每线程的块挂在它**自己的**一个 pthread key 后面,而那个 key 的析构已经先释放了本线程
的块;之后每次读都新分配一个**清零**的块,所以地址每次都不一样。`run_dtors` 走的是空链表。

### §4 假设一其实是对的,错的是那次探针的构造

当时写的是:「PE 上 `thread_local` 走 emutls,它自己的 pthread key 先于 libc++abi 的 key
被析构,于是 `run_dtors` 读到的链表已经空了」——**这就是真因**。

那次探针之所以读到 42,是因为 musl **按 key 的创建顺序**逐个调析构,而探针自己
`pthread_key_create` 在第一次访问 `thread_local` **之前**,于是 emutls 的 key 排在它后面、
析构也在它之后。真实情形里 libc++abi 的 `dtors_key`(实测 `key=0x2`)排在 emutls 之后。

**一个探针报不出它被构造成不会发生的那个顺序。** 判据落在了一个正确的谓词上,而对象的
构造恰好排除了被测的那个条件——这与 [[a-check-that-picks-its-object-by-convention]] 同族。

### 修法

链表存进 **key 自己的值**。key 的析构函数本来就被交给这个值,而任何别的 key 的拆除都碰
不到它。`dtors_alive` 随之不需要:值非空就是「链表在」。零新机制。

### 判据(两条,缺一不可)

`examples/cxx`,两个目标:

```
ok: a thread_local is constructed in a spawned thread
ok: and its destructor runs when that thread ends
```

`x86_64-linux-gnu` 与 `x86_64-windows-gnu`(wine)均 `failures: 0`。只断言「链接通过」
或只断言「构造发生」的判据会同时放过两层——这正是 §5 决定不发第一层补丁的那个理由,
现在它变成了判据本身的形状。
