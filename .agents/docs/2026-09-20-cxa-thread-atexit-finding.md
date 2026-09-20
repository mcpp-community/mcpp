---
subject: review
status: active
---

# `__cxa_thread_atexit` 在 openkal-Windows 上:定位到一层,第二层未定位

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

第二种正是 openkal-musl 的 `[c-abi.absent]` 里叫作 `accepted-no-effect` 的那个形状,
也是 SPEC §6.1 把「运行期报告不支持」称为缺陷的理由。**一个响亮的构建期失败,
比一个静默的运行期失败好。**

补丁已撤回。第一层的定位、第二层的两个否定结果,以及最小复现,都在上面——下一个
接手的人不必从 30 个成员的诊断重新走一遍。

## 6. 下一步的判据

1. 在 Windows 上确认 `run_dtors` **是否被调用**(在 fallback 里打一行,重建运行时)。
   - 被调用而链表为空 ⇒ 注册那一侧的问题
   - 没被调用 ⇒ `__libcpp_tls_create` / key 注册那一侧的问题
2. 无论结论如何,修法必须让「析构会跑」与「链接会过」同时成立,或者两者都不成立。
