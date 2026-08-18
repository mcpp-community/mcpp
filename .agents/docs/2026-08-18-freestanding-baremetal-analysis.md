# C++ 裸机 / freestanding:深度调研与 mcpp 路线分析(2026-08-18)

- Date: 2026-08-18
- Status: **调研报告 + 路线分析,待 review**(不是实施方案,不含工期)
- 起因: [issue #403](https://github.com/mcpp-community/mcpp/issues/403) — RFC:mcpp 对裸机/freestanding target 的支持(riscv64-none-elf 类,toy_kernel 用例)。issue 已 CLOSED,维护者标注「不期望近期实现,供后续设计参考」。
- 关联决策: `.agents/docs/2026-07-24-embedded-platform-support-design.md` **决策 #15**(裸机 / freestanding-modules / ESP32:出范围 / 推迟)。本文重新打开该决策所依据的两条前提,并用实测检验它们。
- 关联: #276(hosted 嵌入式 Linux SDK,另一条线)
- 结构:**实测证据(前置)→ 生态对照 → 标准现状 → 问题分解 → 路线 → 设计侧 → 使用侧 → 陷阱 → 待决策点**
- **⇒ 方案已独立成文:`.agents/docs/2026-08-19-freestanding-baremetal-design.md`**(五层架构、契约层「声明/证据/兜底」、使用侧最小案例、分期与验收判据、D1–D16 回执)。**本文是发现,那份是方案。**

---

## 0. TL;DR

七条结论,每条都有第 1 节的实测编号背书。

1. **mcpp 今天对裸机不是「不支持」,是「静默构建成宿主二进制并报告成功」。**(E1)
   `mcpp build --target riscv64-none-elf` 配合错误信息自己推荐的逃生舱 `[target.riscv64-none-elf]`,输出 `Finished dev … in 0.04s`,产物是 `target/x86_64-linux-gnu/…/toy_kernel`,一个 **x86-64 glibc 动态 PIE**。生成的 `build.ninja` 里 `--target` 出现 **0 次**。**这条与路线选择无关,任何路线下都必须先修。**

2. **裸机能力在 mcpp 现有载荷里已经物理存在。**(E2)
   用 mcpp 自带的 `llvm@22.1.8`(clang + `ld.lld` + `llvm-objcopy`),**零新增工具链**,从一个 **C++20 模块接口单元**编出并链成了完整的 RISC-V 64 裸机内核 ELF + 136 字节裸镜像,`llvm-nm -u` 无未定义符号。缺的不是编译器,**缺的全部是 mcpp 的 target / 链接 / 运行模型**。

3. **唯一真实的载荷缺口很小:compiler-rt builtins 只有 x86_64。**(E3)
   `lib/clang/22/lib/` 下只有 `x86_64-unknown-linux-gnu` 一个目录。⚠️ 而且这个缺口**在 hello-kernel 上看不见**——我们的测试内核 0 个未定义符号,只有做 64 位除法/软浮点的真实内核才会在链接期炸。这是一个天然的假绿点。

4. **`import std` 在裸机上不可用,而且是结构性的,不是配置问题。**(E4)
   GCC 16.1 的 `bits/std.cc`(4502 行)里 `HOSTED|freestanding` 命中 **0 次**,开头就是 `#include <bits/stdc++.h>` 外加 `<execution>`、`<strstream>`;libc++ 的 `std.cppm`(280 行)只有 **1** 处配置守卫。WG21 侧 SG14 在 2025-05 才在讨论「`std.freestanding` 值不值得做」(P3693R0),没有标准。**结论:mcpp 只能明确关断 + 给诊断,不能「降级」。**

5. **但「有 std 子集可用」不只是近,是已经跑通了。**(E5 / X1–X10)
   接上真实 picolibc 的头 + 一份 freestanding `__config_site`,libc++ 的 **110 个 `std/*.inc` 对应头里 103 个可编**;⚠️ 做了宿主对照组后确认:**失败的 7 个在 x86_64 宿主上同样失败**(libc++ 尚未实现的 `<generator>`/`<stacktrace>`/`<rcu>` 等)——**freestanding 这一层的编译期损失是 0**。
   进一步:我据此拼出 `mcpplibs.std.freestanding` 模块并**真的把它链进了裸机内核**——`import` 之后用 `std::array` / `std::ranges::sort`(带投影)/ `std::optional` / `std::atomic` / `std::span` / `std::string_view`,**零未定义符号,text 12435 字节**。`std::thread` / `std::mutex` 在这套配置下**干净地不存在**(编译期报错,不是静默桩)。

6. **子集包不用手写导出表,libc++ 已经把它拆好了。**(X3)
   `std.cppm` 的实体是 **110 个 `share/libc++/v1/std/<header>.inc`**,每个就是那个头的 `export namespace std { using std::X; }`。子集模块 = **机械挑选 `.inc`**,导出表由上游维护。`mcpplibs.std.freestanding` 的源文件只有 **208 行**,而且是生成的。

7. **正确的架构切分是「目标规格是数据,板级支持是包,std 子集也是包」。**
   `-march=rv64gc -mabi=lp64d -mcmodel=medany` 是**目标属性**——必须对每个 TU 和每个依赖完全一致,否则 ABI 撕裂。issue #403 里把它们塞进 `[build].cxxflags` 之所以是错的,不只是「宿主解析错」,而是**依赖拿不到它们**。这正是 Rust 用 target-spec JSON 而不是 `Cargo.toml` flags 的原因。而链接脚本 + 启动代码 + 运行命令属于**板子**,应该是索引里的一个普通包(对标 `riscv-rt` / `cortex-m-rt`),不是引擎里的板子数据库。

8. **推荐路线 = B(最小可落地缝),方向留给 C;而 C 里最贵的那块(std 子集)已被 X 系列证明是小的。**
   A=明确不做、B=开 target/链接/runner 三条缝 + BSP 走包、C=全栈裸机发行版。**无论选哪条,第 1 条的静默失败都必须修**——那是当下唯一的「已发布能力可达的错误行为」。

---

## 1. 本机实测:证据先行

环境:Linux x86_64 / mcpp 二进制 `2026.8.15.3`(仓库 HEAD 为 `f0de3ef`)/ 载荷 `llvm@22.1.8`、`gcc@16.1.0` / 宿主另有 Ubuntu `gcc-riscv64-unknown-elf 13.2.0`。复现命令见附录。

### 1.1 E1 ⚠️ 今天的行为不是拒绝,是静默错构

```console
$ mcpp build --target riscv64-none-elf
error: unknown target 'riscv64-none-elf'
       known targets: `mcpp toolchain list`; a custom triple needs an
       explicit [target.riscv64-none-elf] section in mcpp.toml
```

按这条错误信息**自己的指引**加上逃生舱:

```toml
[target.riscv64-none-elf]
toolchain = "llvm@22.1.8"
```

```console
$ mcpp build --target riscv64-none-elf
   Resolving toolchain
    Resolved llvm@22.1.8 → …/xim-x-llvm/22.1.8/bin/clang++
    Finished dev [unoptimized + debuginfo] in 0.04s

$ file target/x86_64-linux-gnu/*/bin/toy_kernel
… ELF 64-bit LSB pie executable, x86-64, …, dynamically linked,
  interpreter …/xim-x-glibc/2.44/lib64/ld-linux-x86-64.so.2 …

$ grep -c -- --target target/x86_64-linux-gnu/*/build.ninja
0
```

**机制**(已核到 file:line,不是推断):

| 位置 | 行为 |
|---|---|
| `src/toolchain/triple.cppm:299` | `if (k == "unknown" \|\| k == "pc" \|\| k == "w64" \|\| k == "none") continue;` —— **`none` 被当作 vendor 段跳过**,与裸机 OS 段直接撞名 |
| `src/toolchain/triple.cppm:320` | 剩下的 `elf` 段不认识 → `parse()` 返回 `nullopt`,即「这根本不是一个 triple」 |
| `src/build/prepare.cppm:1264` | `if (!known && !hasExplicitSection)` 才报错 —— **有 `[target.X]` 段就放行** |
| `src/build/prepare.cppm:1301` | `host_can_serve` 守卫的条件是 `known && …` —— **不认识的 triple 走不到这条守卫** |
| `src/build/prepare.cppm:1458-1462` | 代码注释直书:「Escape-hatch triples outside the language don't parse and **leave the spec on the host target**」 |

⚠️ 最值得注意的是:**同一份文件在 1281–1296 行把这个结果称为「最坏的失败模式」**并为此专门加了 `host_can_serve` 守卫(注释里还留了 `--target x86_64-windows-msvc` 产出 ELF 的实测记录)。**守卫加在了已知 target 那条路径上,而逃生舱把同一扇门重新打开了。** 这与 `.agents/docs/2026-08-08` 那批「修补放在控制流到不了的地方」是同一形状。

对使用者而言这比 issue #403 描述的还糟:#403 报的是 flags 被按宿主解析后**硬失败**(至少有红);逃生舱路径是**全绿 + 错产物 + 零诊断**。

### 1.2 E2 现有载荷已经能产出完整裸机内核

只用 `xim-x-llvm/22.1.8` 里的 `clang++` / `clang` / `ld.lld` / `llvm-objcopy`,**不装任何交叉工具链**:

```
kmain.cppm  (export module kmain;  export extern "C" void kmain())
start.S     (_start:  la sp, stack_top; call kmain; wfi)
link.ld     (. = 0x80200000; .text/.rodata/.data/.bss)
```

```console
$ clang++ --no-default-config --target=riscv64-none-elf -march=rv64gc -mabi=lp64d \
      -mcmodel=medany -ffreestanding -fno-exceptions -fno-rtti -nostdinc++ \
      -std=c++23 --precompile kmain.cppm -o kmain.pcm      # OK
$ clang++ … -c kmain.pcm -o kmain.o                        # OK
$ ld.lld -T link.ld start.o kmain.o -o kernel.elf          # LINK OK

$ file kernel.elf
kernel.elf: ELF 64-bit LSB executable, UCB RISC-V, RVC, double-float ABI,
            statically linked, not stripped
$ llvm-objcopy -O binary kernel.elf kernel.bin && ls -l kernel.bin   # 136 bytes
$ llvm-nm -u kernel.elf                                     # (空:无未定义符号)
```

三条推论:

- **C++20 模块在 freestanding 下完全可用**——两阶段 `--precompile` → `-c` 对裸机 target 一次通过。模块是语言特性,与 libc 无关。
- **clang 天然多目标**,不需要「一个 target 一个 payload」。这与 GCC 路线(单目标,每个 triple 一份交叉链)是根本不同的成本结构,值得在决策 #8(「先 GCC」)旁边记一笔:**裸机这条线上 clang 的优势是结构性的**。
- **`ld.lld` + `llvm-objcopy` 已在载荷里**,链接与镜像生成不需要新工具。

### 1.3 E3 ⚠️ 唯一真实的载荷缺口:builtins 只有 x86_64

```console
$ ls …/xim-x-llvm/22.1.8/lib/clang/22/lib/
x86_64-unknown-linux-gnu
```

`libclang_rt.builtins.a` 只有宿主一份。RISC-V 64 上一旦用到编译器内建运行时(64 位除法、软浮点、`__muldi3` 类),链接期会缺符号。

⚠️ **这个缺口在 hello-kernel 上不可见**:E2 的内核 `llvm-nm -u` 是空的,一路全绿。**若拿 hello-kernel 当 e2e,builtins 这条永远测不到**——与 `.agents/docs/2026-08-12` 记的 bench 假绿、以及 PR#451 「CI 全绿 ≠ 覆盖到了」同族。验收用例必须**故意**含一次 64 位除法或软浮点。

### 1.4 E4 `import std` 是结构性 hosted-only

```console
$ wc -l …/gcc/16.1.0/include/c++/16.1.0/bits/std.cc
4502
$ grep -c "HOSTED\|freestanding\|_GLIBCXX_HOSTED" …/bits/std.cc
0
```

`std.cc` 第 26 行起:`#include <bits/stdc++.h>`,随后**无条件**再拉 `<execution>` 和 `<strstream>`。**整份文件没有任何 freestanding 分区**。

libc++ 侧同理:`share/libc++/v1/std.cppm` 共 280 行,配置守卫仅 `_LIBCPP_HAS_ATOMIC_HEADER` **1** 处。

⇒ 两个实现都把 `std` 模块做成了「全都要」的单体。**没有实现侧的开关可以让 `import std` 在裸机上变小。**

标准侧也没有:见第 3 节。**mcpp 的正确姿势是在 `os=none` 上把 `import std` 判为不可用并给诊断**,而不是尝试降级——降级出来的「少一半的 std」会是比硬失败更坏的东西,而且会污染 BMI 缓存身份(参考 `2026-07-31` C++20 档位设计里「跨档位 BMI 硬拒」的先例)。

### 1.5 E5 头文件子集:失败全在 libc,不在 libc++

libc++ 的每目标配置文件是 `include/<triple>/c++/v1/__config_site`,载荷里**只有** `x86_64-unknown-linux-gnu` 一份。直接对 `riscv64-none-elf` 编 `<type_traits>`:

```
…/__config:646:8: error: "No thread API"
```

⚠️ `-D_LIBCPP_HAS_THREADS=0` **不管用**(该宏来自 `__config_site`,不是命令行可覆盖的)。合成一份 freestanding `__config_site`(把 `THREADS / FILESYSTEM / LOCALIZATION / MONOTONIC_CLOCK` 四个 `#define` 翻成 0)后,共测 17 个头:

| 结果 | 头 |
|---|---|
| **OK(8)** | `<type_traits>` `<concepts>` `<span>` `<expected>` `<bit>` `<utility>` `<tuple>` `<charconv>` |
| **FAIL(9)** | `<array>` `<optional>` `<algorithm>` `<ranges>` `<variant>` `<atomic>` `<format>` `<string_view>` `<coroutine>` |

失败信息全是两条:`__mbstate_t.h:51: "We don't know how to define mbstate_t"` 和 `string.h:98: unknown type name 'size_t'`。

**全部是 libc 缺失,没有一条是 libc++ 自身的 hosted 依赖。** 换言之:**接上 picolibc / newlib 的头文件,这个子集会立刻变宽**(推断,未实测——本机没装 picolibc,`picolibc-riscv64-unknown-elf` 在 Ubuntu universe 里有 1.8.6-2,但装它属于改用户环境,未执行)。

这条对路线的意义:**「裸机上有可用的 C++ 库」不是遥远目标,是一个载荷构建决策**(给目标编一份 `__config_site` + 一个 freestanding libc),不是引擎特性。

### 1.6 E6 ⚠️ 三个会咬人的环境级事实

1. **`ld.lld` 这个名字在本机解析到 GNU ld。**
   ```console
   $ readlink -f $(which ld.lld)
   /home/speak/.xlings/bin/xlings
   $ ld.lld --version
   GNU ld (XPKG: xlings install fromsource:binutils) 2.42
   ```
   后果:`ld.lld -T link.ld …` 报 `cannot represent machine 'riscv'`。**裸机链接必须按载荷路径寻址链接器,不能按名字。** 这与 `.agents/docs/2026-08-11` 的 `$ORIGIN` 优先级、以及「链接期用 A、运行期加载 B」是同一类地址-vs-名字问题。

2. **`-mcmodel=medany` 不是可选项,是 link-or-not。**
   漏掉它,链接直接失败:
   ```
   ld.lld: error: relocation R_RISCV_HI20 out of range: 524800 is not in [-524288, 524287]
   ```
   载入地址 `0x80200000` 超出 medlow 可寻址范围。**这是「目标规格必须是数据」的最硬证据**:它既不是优化选项也不是用户偏好,它由板子的内存布局决定。

3. **libc++ 的 `__config_site` 是按 target 的文件**,不是宏。「加个 `-isystem` 就能用」是错的。

### 1.7 X1–X10 `mcpplibs.std.freestanding` 子集包:从假设到跑通

E5 只证明了「零 libc 下 8 个头可用」。这一轮把真实 picolibc(Ubuntu `picolibc-riscv64-unknown-elf` 1.8.6-2,`apt-get download` + `dpkg -x` 解到临时目录,**未装进系统**)接上,把整条链走完。

**X1 — libc++ 自己已经把 std 模块拆成可裁剪的零件。**
`share/libc++/v1/std.cppm` 的结构是:全局模块片段 `#include <header>` × N → `export module std;` → `#include "std/<header>.inc"` × **110**。每个 `.inc` 就是那个头的 `export namespace std { using std::X; }`(例:`std/span.inc` 导出 `dynamic_extent`、`span`、`ranges::enable_borrowed_range`)。
⇒ **子集模块是机械裁剪,不需要手写也不需要维护导出表。**

**X2/X3 — 接上 picolibc 头之后,编译期损失为零。**

| 配置 | 可编头数 |
|---|---|
| 零 libc(E5) | 8 / 17 抽样 |
| **+ picolibc 头** | **17 / 17 抽样;103 / 110 全量** |

⚠️ **对照组是这条结论的关键**:失败的 7 个(`generator` `hazard_pointer` `rcu` `spanstream` `stacktrace` `stdfloat` `text_encoding`)拿到 **x86_64 宿主 + 完整 libc++/glibc** 上重测,**7 个全部同样失败**——它们是 libc++ 尚未实现的头,与裸机无关。**没有对照组就会把「libc++ 没写」记成「裸机不行」。**

**X4 — include 能过 ≠ 东西真在。** 用真实使用探针(不是 include 探针)复核:

| 设施 | 结果 |
|---|---|
| `std::span` / `array` / `ranges::sort` / `optional` / `expected` / `atomic` / `string_view` / `charconv` / coroutine | **可用** |
| `std::vector` / `std::string`(需堆) | 编译可用(链接期另说,见 X9) |
| `std::mutex` / `std::thread` | **干净地不存在**(`no type named 'mutex' in namespace 'std'`)—— 不是静默桩,是编译期报错 |
| `std::cout` | 声明在但不可用 |

`__config_site` 关掉 THREADS/FILESYSTEM/LOCALIZATION 的效果正是我们想要的:**不该有的东西直接消失,而不是留个跑起来才错的壳。**

**X5/X6 — 包装模块本身成立。** 由 `ok.list` 生成的 `stdfs.cppm`(**208 行**,全部是 `#include`)对 `riscv64-none-elf` 编出 reduced BMI 26.5 MB + object 1.3 KB;消费者 `import mcpplibs.std.freestanding;` 后编译通过。

**X7–X10 — 链接期的真实边界(这才是可用性的分界线)。**

第一次链接只缺 **2 个符号**,收敛速度远超预期:

| 缺失符号 | 性质 | 解法 |
|---|---|---|
| `std::__libcpp_verbose_abort` | `-fno-exceptions` 下所有 `__throw_*` 的落点 | **用户/BSP 提供 ~3 行**(实测:一个 `for(;;) wfi` 即可) |
| `std::__sort<__less<int>&, int*>` | libc++ 把标量类型的 `__sort` 做成了 **`extern template`**,实体在编译版 libc++ 里 | 见下 |
| `memmove`(去掉 `verbose_abort` 后暴露) | 编译器可自行发出的 libc 函数 | picolibc `libc.a` |
| `std::basic_string::__init/append`(用 `std::format` 时暴露) | `basic_string` 的 out-of-line 成员在编译版 libc++ 里 | 需目标版 `libc++.a` |

⚠️ `__algorithm/sort.h:834-847` 的 `extern template` **无宏可关**(逐条核过,外层无守卫)。但它**只覆盖内建标量类型**——对自定义类型/带投影的 `ranges::sort` 完全走头文件。

**最终边界(X10 实测,零未定义符号):**

```
kernel4.elf: ELF 64-bit LSB executable, UCB RISC-V, RVC, statically linked
   text    data     bss     dec
  12435      16    4096   16547
```

内核里真正跑的是:`import mcpplibs.std.freestanding;` + `std::array` + `std::ranges::sort(t, {}, &Task::prio)` + `std::optional` + `std::atomic` + `std::span` + `std::string_view`,链接输入 = `start.o` + `kmain.o` + `stdfs.o` + 3 行 runtime + picolibc `libc.a`。

⇒ **分界线是清楚的**:

| 档位 | 需要什么 | 覆盖 |
|---|---|---|
| **T1 header-only** | picolibc 头 + `__config_site` + ~3 行 hook | array/span/ranges/algorithm(自定义类型)/optional/expected/atomic/string_view/tuple/bit/utility/concepts/type_traits/charconv/coroutine |
| **T2 + libc.a** | 上面 + picolibc `libc.a` | 编译器隐式发出的 `memcpy/memmove/memset/memcmp`;堆(需 BSP 给 `sbrk`) |
| **T3 + 目标版 libc++.a** | 上面 + 为目标编的 libc++ | `std::format`、标量 `std::sort`、`std::string` 全功能 |

**T1+T2 已在本机完全跑通;只有 T3 需要新载荷。**

**X11 ⚠️ picolibc 的发行版包覆盖不到最常见的 RISC-V profile。**
Ubuntu 包的 rv64 multilib 全集:`rv64i/lp64 rv64ia/lp64 rv64iac/lp64 rv64iaf/lp64f rv64iafd/lp64d rv64if/lp64f rv64ifd/lp64d rv64im/lp64 rv64imac/lp64 rv64imafc/lp64f rv64imf/lp64f`——**没有 `rv64imafdc/lp64d`(即 `rv64gc`)**。本轮实测因此退到 `rv64imac/lp64`。⇒ **载荷不能直接复用发行版包**,要么自建 multilib,要么采纳 LLVM Embedded Toolchain(§2.4)。

**X12 附带发现:qemu 退出码通路是现成的。**
picolibc 载荷里带 `crt0-semihost.o` 和 `libsemihost.a` ——`mcpp test` 在 qemu 下拿测试退出码不需要自己造 semihosting。

### 1.8 Y1–Y5 实现中立的适配层 + 机械验证(**修正 §1.7 的一条结论**)

§1.7 结尾写了「这条包路线天然绑定 clang/libc++」。**实测证明这条不成立**,原因有两条,都比原判断重要。

**Y1/Y2 — libstdc++ 有真正的 freestanding 模式,而且比 libc++ 更诚实。**

`bits/c++config.h:1638`:`#define _GLIBCXX_HOSTED __STDC_HOSTED__` —— libstdc++ 的 freestanding **由编译期开关驱动**(`-ffreestanding` 会把 `__STDC_HOSTED__` 置 0),44 个头对它敏感。不需要合成任何配置文件。

`g++ -std=c++23 -ffreestanding` 下的真实使用探针:

| 可用 | `<span> <array> <algorithm>/ranges::sort <optional> <expected> <atomic> <string_view> <coroutine>` |
|---|---|
| **不可用** | `<charconv> <format> <vector> <thread> <iostream>` —— 全部是 **`#error "This header is not available in freestanding mode."`** |

⚠️ **这条对比是反直觉的,而且方向与 §1.7 相反**:

| | libc++ | libstdc++ |
|---|---|---|
| freestanding 开关 | 每目标 `__config_site` **文件**(载荷构建期烙死) | **`-ffreestanding` 编译期开关**,零配置 |
| 不可用设施的表现 | 头**被守卫成近乎空文件** ⇒ 报错发生在「用它的时候」,信息离现场很远 | **点名 `#error`,当场** |
| 全量扫 110 头 | 103 "OK" | 53 OK |
| 但那个数字的含义 | ⚠️ **含约 50 个只是「能 parse」的空壳**(X4 已测 `std::mutex` 根本不存在) | **53 个全部诚实**,其余硬 `#error` |

**libc++ 的 103 不是「更宽」,是「更含糊」。** 把 include 成功率当能力指标会得到完全错误的排序 —— 这正是陷阱 #10。

**Y3 — 一份源码,两个标准库,零 `#if`。**

写一个实现中立的适配层(不 `#include "std/*.inc"`,而是自己写 `export namespace std { using std::span; … }`),同一个 `core.cppm`:

| 编译配置 | 结果 |
|---|---|
| GCC 16.1 + libstdc++ + `-ffreestanding`(宿主 x86_64) | `core_gcc.o` ✅ |
| clang 22 + libc++ + freestanding `__config_site`(`riscv64-none-elf` + picolibc) | `core_clang.pcm` ✅ |

**⇒ 适配层可行,因为导出的名字是标准规定的,不是任何一家的私产。** libc++ 的 `.inc` 只是一份**方便的名字清单**,不是依赖。

**Y4 — 可移植 freestanding 子集 = 52 个头(两家交集)。**

```
algorithm any array atomic bit cassert cctype cfenv cfloat chrono cinttypes clocale
compare concepts coroutine csetjmp csignal cstdarg cstddef cstdint cstdio cstdlib
cstring ctime cuchar cwchar cwctype exception expected functional initializer_list
iterator limits mdspan memory new numbers numeric optional ranges ratio
scoped_allocator source_location span string_view tuple typeindex typeinfo
type_traits utility variant version
```

⚠️ 但「交集」不是唯一正确答案,因为**两家其实在两个不同档位上**:

| 档 | 环境 | 得到什么 |
|---|---|---|
| **T0 `freestanding-core`** | 无 libc / 无堆 | ≈ 上面 52 个头。libstdc++ `-ffreestanding` **零配置**即是;libc++ 需 `__config_site` |
| **T1 `freestanding+heap`** | + picolibc(含 malloc) | **额外拿到 `vector` / `string` / `format` / `charconv`**(X4/X9 实测,libc++ 侧) |
| **T2 `hosted`** | 现状 | 全部 |

libc++ + picolibc 之所以「更宽」,不是 libc++ 更强,是 **picolibc 给了堆**。⇒ **档位是环境的函数,不是标准库实现的函数。** 这条直接决定了第 9 节的生态架构。

**Y5 — 「这个库需要什么环境」可以从产物机械判定,零元数据。**

| 源码用了 | `llvm-nm -u` 里出现 | 判定 |
|---|---|---|
| `new` / `delete` | `_Znwm` `_ZdlPvm` | 需要 **heap** |
| `throw` / `catch` | `__cxa_throw` `__cxa_begin_catch` `__cxa_allocate_exception` `__cxa_end_catch` | 需要 **exceptions** |
| `dynamic_cast` | `__dynamic_cast` `_ZTI1B` | 需要 **RTTI** |
| 纯计算 | **(空)** | freestanding-clean |

`llvm-nm` 已在载荷里。⇒ **不需要生态先标注,就能判定。** 这是第 9 节的技术地基。

### 1.9 T1–T3b 能力是可插拔的符号契约(**修正 §7.5 里一处分类错误**)

§7.5 曾把 `std::thread` 归到 `hosted` 档。**实测证明这是错的** —— 线程和堆都是**可插拔能力**,不是 hosted 专有设施。

**T1/T2 — 两家标准库都把线程做成了可插拔层。**

| 标准库 | 机制 | 证据 |
|---|---|---|
| **libstdc++** | **gthreads**:`gthr.h` / `gthr-default.h` / `gthr-posix.h` / **`gthr-single.h`** 四件齐全;`_GLIBCXX_HAS_GTHREADS` 构建期决定 | 载荷里四个头都在 |
| **libc++** | **`_LIBCPP_HAS_THREAD_API_EXTERNAL`**:线程原语由外部提供 | `__config:622` / `:662` 明确有这条分支 |

⇒ FreeRTOS / Zephyr 接到任一侧,裸机上就有 `std::thread`。正确的声明是 `tier="core", requires=["threads"]`。

**T3b — `heap` 同理,而且是标准写好的扩展点。**

BSP 侧一个 bump 分配器(替换 `operator new/delete` 重载集),**零 libc、零 malloc**:

```
$ ld.lld -T link.ld start.o usevec.o bsp_heap.o -e kmain -o heapdemo.elf
heapdemo.elf: ELF 64-bit LSB executable, UCB RISC-V, statically linked
   text 5911   data 0   bss 12304        ← llvm-nm -u:空
```

内核里跑的是 `std::vector<int>` + `push_back` + `std::span` 求和。C++ 标准 [new.delete] **本来就规定 `operator new/delete` 可被用户替换** —— 这不是绕过标准,是标准的扩展点。

⚠️ 中途暴露的一条实用细节:漏掉 `operator new(size_t, align_val_t)` 时,lld 报的是 **`undefined symbol: operator new(unsigned long, std::align_val_t)`** —— 精确点名缺哪个重载。**能力契约是可枚举的符号集,不是模糊概念。**

**⇒ 由此闭环:能力的「契约」和能力的「判据」是同一组符号。**

```
heap       契约 = operator new/delete 重载集   判据 = 产物有无 _Znwm / _ZdlPvm
exceptions 契约 = __cxa_throw + _Unwind_*      判据 = 产物有无 __cxa_throw
rtti       契约 = __dynamic_cast + typeinfo    判据 = 产物有无 __dynamic_cast
threads    契约 = __gthread_* / __external_*   判据 = 产物有无这些符号
```

所以 Y5 的符号审计**不是启发式规则**,它检查的就是真正的 ABI 契约;裸机链接之所以是硬门,也是因为契约缺实现在 `-nostdlib` 下必然是未定义符号。

⚠️ **推论**:mcpp **不应该**自己定义一套能力接口 —— 契约由 C++ 标准([new.delete])、Itanium C++ ABI(`__cxa_*`)和各标准库(gthreads / `__external_threading`)**早已定义**,再造一套就是第三套 ABI,与两边都不兼容。唯一需要生态出力的是 `threads`:两家接口不同,一个 RTOS 想服务两边要写两个 shim —— **那是包的工作,不是引擎的**。

### 1.10 H1–H3 生态契约的可组合形态(非标准库能力)

§1.9 覆盖的是**语言运行时**能力。裸机上还有一类**没有任何标准契约**的能力:UART / 定时器 / 存储 / 中断。这一轮量的是它们该长什么形状。

**⚠️ H1 — 形态 A(模块内声明、别的包定义)被语言禁止。**

```
消费者要:  _ZN3halW8mcpplibsW3halW7console5writeEPKcm
           → hal::write@mcpplibs.hal.console(char const*, unsigned long)
提供者给:  _ZN3hal5writeEPKcm
           → hal::write(char const*, unsigned long)
ld.lld: error: undefined symbol: hal::write@mcpplibs.hal.console(...)
```

**模块归属被烙进符号名** ⇒ 附着到模块的函数,定义必须来自同一个模块。**跨包提供实现在物理上不可能。**

⇒ 这给 2026-07-24 决策 #5(C-ABI 是唯一稳定的二进制互操作契约)补了一条**更硬的理由**:在 C++20 模块世界里,跨包提供实现**只有 C ABI 一条路**,这不再只是 ABI 稳定性论证,而是语言层面的物理约束。

**H2 — 形态 B(`extern "C"` 缝)可行**:消费者要 `U mcpp_hal_console_write`,提供者在另一个包定义,链接零未定义符号。代价:单一全局符号 ⇒ **不能有两个提供者**。

**H3 — 形态 C(concept 静态注入)可行,且是唯一可组合的**:

```cpp
// 契约包:纯接口,零实现
template <class T> concept Console = requires(T& t, std::span<const char> s) { t.write(s); };
template <class T> concept Clock   = requires(T& t) { { t.ticks() } -> std::same_as<unsigned long>; };

// 驱动包:只依赖 concept,不知道任何板子
template <hal::Console C, hal::Clock K> void banner(C& c, K& k);

// BSP 包:Uart 与 Uart2 同时存在
```

实测:`banner(u, t)` 与 `banner(u2, t)` **两个提供者共存**,同一驱动各实例化一次(零成本静态派发);且同一个 BSP 的 `operator new` 让 `std::vector` 一起工作 —— **生态库与 std 同时点亮**。整体链接零未定义符号,text 9040 字节。

| 形态 | 跨包 | 多提供者共存 | 成本 |
|---|---|---|---|
| A 模块内声明 | ❌ 语言禁止 | — | — |
| B `extern "C"` | ✅ | ❌ | 链接期绑定,不可内联 |
| C concept 注入 | ✅ | ✅ | **零成本,可内联** |

⇒ **生态 HAL 应以 C 为主**;B 只留给天然单例的运行时能力 —— ⚠️ 而那些恰好已被 §1.9 的标准扩展点(`operator new` / gthreads / `__libcpp_verbose_abort`)覆盖,**生态基本不需要自己发明 `extern "C"` 契约**。

### 1.11 K1–K3 KAL(内核/arch 抽象)的两条硬约束

针对「用 concept 表达 KAL,支持所有 CPU、不管有没有 MMU」这个方向做的探测。**两条结果都是限制性的。**

**K1/K2 — ⚠️ concept 检查语法,不检查语义。**

写一个 `kal::AddressSpace` concept(`map` / `translate`),两个实现:

```
RiscvSv39::map(0x8000_0000, 0x9000_0000) → true    真重映射
NoMmu    ::map(0x8000_0000, 0x9000_0000) → false   只能恒等映射
```

**两者都完全满足同一个 concept,编译期无法区分。** 通用内核代码调用 `map()` 做非恒等重映射,在 NoMmu 上静默失败。

⇒ **MMU 的有无不是一个抽象能藏住的差异**:它上面的代码分成两族(有 demand paging / fork / 地址空间隔离 vs 没有),不是一个统一接口。**这是能力轴(`cfg(mmu)`),不是契约。**

**K3 — ⚠️ 「零成本抽象」在跨模块边界上默认不成立。**

同一段 `setup(arch, va)` 通用代码,三种构建方式:

| 构建 | `RiscvSv39::disable()`(应为一条 `csrci`) | 判据 |
|---|---|---|
| **同 TU + `-O2`** | **完全内联**:`csrci` / `csrsi` + 3 条指令 | ✅ 真零成本 |
| **跨模块 + `-O2`** | **真实 `jal` 调用** `_ZN5archsW5archs9RiscvSv397disableEv` | ❌ 抽象有成本 |
| **跨模块 + `-flto`** | `kmain` 内 `jal` = **0**,内联进来的 csr 指令 = **4** | ✅ 恢复 |

⇒ **内核热路径(irq 开关、per-cpu 访问、页表项操作)一旦跨包/跨模块,默认会退化成真实函数调用。** 对内核这是不可接受的 —— `local_irq_disable()` 本该是一条指令。

**⇒ KAL 的构建模型必须把 LTO 当作必需项,不是优化项。** 而 LTO 在内核上有自己的代价(链接脚本 section 放置、inline asm 约束、编译期),这条必须提前进设计,不能等发现热路径慢了再补。

### 1.12 KA1–KA3 KAL 作为 kernel ABI 统一层(与 1.11 是两件事)

§1.11 测的是「抽象内核内部」。这一组测的是**另一种读法**:统一 syscall / kernel ABI,转发到不同内核后端。**结果全部是肯定的。**

契约走 C ABI 缝(H1 已证明跨包只有这一条路):

```cpp
export module openkal.io;
extern "C" long kal_write(int fd, const char* buf, unsigned long n);
export namespace kal { inline long write(int, const char*, unsigned long); }
```

**KA1 —— 同一份 `app.cppm`,零 `#if`,两个内核后端:**

| 后端 | 实现 | 结果 |
|---|---|---|
| hosted x86_64-linux | 转发到真 `::write(2)` | ✅ **实际运行输出 `hello from one source`** |
| riscv64-none-elf 裸机 | 转发到 MMIO UART | ✅ 零未定义符号,**157 字节** |

**KA3 —— 应用对 KAL 的依赖面是可审计的符号集,且两侧完全相同:**

```
裸机构建的 app.o    外部符号:  kal_write
hosted 构建的 app.o 外部符号:  kal_write
```

**KA2 —— 换后端不重编应用**:同一个 `app_b.o` 换掉后端目标文件重链即成。

⇒ 与 K3 合起来给出两层相反的 ABI 结论:

| | AAL | KAL |
|---|---|---|
| 典型操作 | `local_irq_disable()` 本该一条 `csrci` | `write()` 本来就要陷入内核 |
| 一次 call 的相对成本 | **灾难性** | 可忽略 |
| 正确形态 | **concept + LTO**(K3) | **C ABI**(KA2) |

⚠️ 这正是 Linux 的实际做法(`asm/` 的 `static inline` vs 导出的 syscall 入口)—— 不是巧合。

### 1.13 TH1–TH6 线程契约面 + ⚠️ `thread_local` 的静默失败

**TH1/TH2 —— 契约面有多大(决定这条路是一个下午还是一个季度):**

| | 名字数 |
|---|---|
| libc++ `__external_threading` | **36**(~30 函数 + 8 类型):mutex×4 · recursive_mutex×5 · condvar×5 · thread×9 · once×1 · tls×3 |
| libstdc++ gthreads(`gthr-default.h`) | 71 —— **但其中 17 个是 `__gthread_objc_*` 遗留** ⇒ 真实面 ~30,与 libc++ 高度重合 |

libc++ 已有 `__thread/support/{pthread,windows,c11,external}.h` **四个后端** —— 可插拔线程后端这个形状在标准库里已经存在。

**TH4–TH6 —— ⚠️ `thread_local` 在裸机上是一个完整的静默失败:**

```
thread_local int counter;   →  编译 ✅  链接 ✅  零未定义符号 ✅  零诊断 ✅
产生段:.tbss (4 字节)

bump():
    lw  a0, 0x0(tp)          ← 通过 tp 寄存器寻址
    addiw a0, a0, 0x1
    sw  a0, 0x0(tp)

start.S 里设过 tp 吗?  → 没有(只设了 sp)
```

**⇒ 全绿,运行期静默读写垃圾地址。** 与 §1.1 的 E1 同族:**「没有红」才是问题**。

`thread_local` 不属于线程契约(KAL),属于 **AAL(TLS 寄存器约定)+ BSP(`start.S` 设 `tp` + 链接脚本 `.tdata/.tbss`)**。⚠️ 且 libstdc++/libc++ 内部自己用 `thread_local`,不是「用户不写就没事」。

⇒ **裸机验收用例必须含一个 `thread_local`**,否则这条会活很久。

### 1.14 M1–M5 / ISO 系列:现代 C++ 惯用法可行,⚠️ 以及一次不合格对照的教训

**M1–M3 —— 惯用法在裸机上全部跑通,而且零成本是实测的:**

`concept`(能力契约)+ concept 的 `&&`(world = 接口组合)+ `std::expected`(结构化错误,零 libc 可用)+ **deducing this**(后端只写 `write()`,`write_all()` 免费)+ `if constexpr`(可选能力做增强)。

链接零未定义符号,**text 698 字节**;`kmain` 反汇编 **`jal` 条数 = 0** —— 整条 `log → write_all → write → expected` 链全部内联成直线代码(前提是 **LTO**,见 K3)。

**⚠️ M4/M5 的初始结论是错的,原因是一次不合格的对照。**

初稿断言「clang 22 + 模块 + concept 诊断 = ICE」。但那组对照**同时换了两个变量**(模块 vs 头文件、以及 target/标准库配置)。逐个隔离:

| 隔离 | 结果 |
|---|---|
| ISO-1 宿主 + 正常 libc++ + 模块 + 失败 concept | ✅ 诊断完美 |
| ISO-2 加回 `--target=riscv64-none-elf -ffreestanding` | ✅ |
| ISO-3 概念里用 `std::` 类型(import std 子集) | ✅ |
| ISO-4 两个 `import` 同一行 | ✅(clang 接受;GCC 报解析错) |
| ISO-6 `deducing this` + 跨模块继承 | ✅ |
| **真因:一个 BMI 建于 `-O2`,另一个建于 `-O2 -flto`,混用** | ❌ **ICE** |
| **同组 flag 重建全部 BMI** | ✅ **诊断完美** |

flag 一致后的诊断质量远超预期(精确到缺哪个成员):

```
error: static assertion failed: world 缺 threads 能力
note: because 'bsp::BareWorld' does not satisfy 'RtosWorld'
note: because 'w.threads()' would be invalid: no member named 'threads' in 'bsp::BareWorld'
```

**⇒ 两条结论:**

1. **惯用法完全成立,不存在路线阻塞。**
2. ⭐ **「BMI flag 一致性」不是优化问题,是「不一致会让编译器崩」** —— 这抬高了「BMI 缓存键必须含 freestanding 轴」的等级(后果从「结果不对」变成「编译器崩溃栈」)。**而 mcpp 的指纹/缓存键系统正是防这个的;我是手敲命令行绕过了它才踩到。**

⚠️ **方法论教训**:这一轮我又犯了「对照组同时换两个变量」的错(与 `gcc-bmi-body-insensitive` 那次同族)。**是用户的质疑逼出了重测。** 不合格的对照会把一个配置错误读成编译器缺陷,并据此得出「路线被阻塞」的结论。

### 1.15 AAL / CABI 系列:最硬原语的裂缝,与 C ABI 编码的代价

**AAL-1/AAL-2 —— 「AAL 会不会碎」的正面测试。**

两个**真实不同**的 arch 实现(riscv Sv39 风格 PTE 位 `V/R/W/X/U` vs x86-64 风格 `P/RW/US/NX`,Context 布局完全不同)+ 一段只依赖 concept 的通用内核代码:

- **AAL-1 通过**:同一段 `setup_task()` 对两个 arch 各实例化一次,**`call` 条数 = 0**。
- **AAL-2 主动找裂缝,找到三条,而且分成两类:**

| # | 裂缝 | 表现 | 类型 |
|---|---|---|---|
| 1 | execute-only(riscv 支持 X 不带 R,x86-64 经典分页做不到) | **两边都编过**,x86 实现静默给了 R+X | **A 类:类型对、语义错、静默** |
| 2 | 页大小按下标取 `page_sizes[1]` | 编过;riscv/x86 恰好都是 2M,aarch64 16K granule 下是 32M | **A 类** |
| 3 | 通用代码设 syscall 返回值 `c.a0 = v` | `error: no member named 'a0' in 'RvSv39::Context'` | **B 类:编译期硬错** |

⇒ **规则**:A 类**必须提到能力轴**;B 类**承认它不属于通用层**。与 K2(MMU)、实时性是同一条规则的三次应用。

**CABI —— `result<T,E>` 在 C ABI 上的两种编码,代价实测:**

| 编码 | x86-64 | riscv64 |
|---|---|---|
| **2 字结构返回** | **9 条指令** | **10 条指令,2 次内存存取** |
| 出参 + 错误码 | 12 条指令,3 次栈存取 | 12 条指令,4 次内存存取 |

riscv 反汇编显示 err/value 直接走 `a0`/`a1`,`snez`/`and` 就地做分支消除,**不落栈**。

⇒ **两个 arch 上结构返回都更便宜** ⇒ KAL 的 `result` 用 2 字结构返回。⚠️ 限制:`T` 必须 ≤ 一个机器字,否则退化成隐藏指针。

---

## 2. 生态对照:别人怎么解

### 2.1 Rust / Cargo —— 最完整、也最值得抄的对照

Rust 把裸机拆成**四个互不耦合的层**,这正是 mcpp 缺的分层:

| 层 | Rust 的东西 | 承载什么 |
|---|---|---|
| 库分层 | `core` / `alloc` / `std` **在源码层就是三个 crate** | `#![no_std]` 是**减法可证**的:`core` 一定存在 |
| 目标规格 | 内置 target(如 `riscv64imac-unknown-none-elf`)或 **target-spec JSON** | ISA/ABI/code-model/panic-strategy/linker-flavor 都是**数据** |
| 板级支持 | `riscv-rt` / `cortex-m-rt` crate + `memory.x` | 启动代码 + 链接脚本 **随包分发** |
| 运行/调试 | `.cargo/config.toml` 的 `runner = [...]` | `cargo run` → `qemu-system-riscv64 -kernel <artifact>` |

典型 `.cargo/config.toml`:

```toml
[build]
target = "riscv64gc-unknown-none-elf"
rustflags = ["-Clink-arg=-Tsrc/lds/virt.lds"]

[target.riscv64gc-unknown-none-elf]
runner = ["qemu-system-riscv64", "-nographic", "-machine", "virt", "-kernel"]
```

对 mcpp 最重要的三点:

- **`core` 的存在是 `no_std` 好用的全部原因。** C++ 侧没有任何实现提供等价物(E4)。这是 C++ 裸机体验落后 Rust 的**根因**,不是工具链问题。
- **target spec 是文件/数据,不是 flags。** 用户不会在 `Cargo.toml` 里写 `-mcmodel=medany`。
- **`runner` 只有一行。** 但它把「裸机产物不能直接 exec」这件事整个吸收掉了。

另有 `-Z build-std=core,alloc`(从源码为任意目标现编标准库)——C++ 侧的对应物恰好就是「为目标现编一份 libc++ / `__config_site`」,E5 已经证明其可行性形状。

### 2.2 Zig

target 语法直接是 `arch-os-abi`,`os` 的合法取值里就有 **`freestanding`**(`riscv64-freestanding-none`)。Zig 自带多目标 LLVM 后端 + 自己的 libc/compiler-rt,所以「换 target 就换世界」是零成本的。

**对 mcpp 的启发是 triple 语言本身**:mcpp 的 triple 文档(`triple.cppm:6`)明写「canonical 是 `arch-os[-env]`(三段,无 vendor —— Zig 风格)」,**却把 `os` 的取值锁死在 `{linux, macos, windows}`(同文件第 12 行)**。加一个 `none` 是这套语言的自然延伸,不是异物。

### 2.3 CMake / Conan / PlatformIO / Zephyr

- **CMake**:`CMAKE_SYSTEM_NAME=Generic` 表示「目标没有 OS」,并且必须配 `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`,否则 CMake 的工具链探测会去链一个可执行文件而必然失败。⚠️ **这条对 mcpp 直接适用**:mcpp 的 doctor / 工具链探测里任何「编个小程序链一下」的动作,在 `os=none` 下都会假失败。
- **Conan**:2021 年(1.43)才加 `os=baremetal` 设置,并且官方说明是「这只是一个通用命名约定,预期用户在其下自定义子设置来表达具体硬件/板子/系列」。⇒ **连 Conan 也没有把板子建模进核心**,而是留给 profile。这支持本文第 6 条:板级是数据/包,不是引擎。
- **PlatformIO / Zephyr(west)**:走到了另一极端——内建**成千上万的板子定义**。这是「发行版构建器」的形态,与 `2026-07-24` 决策 #1(mcpp 不做发行版构建器)明确冲突,**不应作为 mcpp 的方向**。

### 2.4 LLVM Embedded Toolchain(可直接借用的载荷形态)

ARM 维护的 `LLVM-embedded-toolchain-for-Arm` 打包了 **clang + lld + libc++ + libc++abi + compiler-rt + picolibc(可选 newlib / llvm-libc)**,通过 multilib 按 `--target` + FPU 选项自动选库,并提供 `--config` 配置文件。官方说明「C++ 通过 libc++/libc++abi **部分支持**,无多线程,ABI 不稳定」。

⇒ 这就是 E2+E3+E5 三条实测拼出来的那个东西的**成品形态**:mcpp 若走到载荷层,不必从零设计,**这套组合已被验证**(且 RISC-V 也在其目标内)。

### 2.5 OSDev 的既有实践约束

OSDev Wiki 的裸机基线要求几条硬约束,任何 mcpp 方案都绕不开:

- 必须用**交叉编译器**,不能用宿主编译器(mcpp 侧对应 E1:必须真发 `--target`);
- 链接顺序 **`crti.o` → `crtbegin.o` → 你的 .o → `crtend.o` → `crtn.o`**,顺序错会出「奇怪的 bug」;`crtbegin/crtend` 由编译器提供,`crti/crtn` 要自己写;这两个共同实现 `_init`/`_fini`,**即全局构造函数的调用点**;
- 编译用 `-ffreestanding`,链接用 `-nostdlib` + `-lgcc`。

⇒ **C++ 的全局构造在裸机上是 BSP 的职责**(`_init` / `.init_array` 遍历)。这条把「BSP 必须能贡献链接输入和链接顺序」这个需求钉死了——不是可选糖。

---

## 3. 标准侧现状:freestanding 与 std 模块

- **freestanding 库子集正在扩大**:P1642(把 `[utilities]`/`[ranges]`/`[iterators]` 的大部分加入 freestanding 子集)、P2338(把「无需 OS 调用、无空间开销」的一切加入 C/C++ 共享 freestanding 库,含 `charconv`、`char_traits`)。C++23 起 `<cstdint>` 等已标为 "all freestanding"。
- **但模块与 freestanding 的交叉是空白**:P1212R0(*Modules and Freestanding*, 2018)提出四个选项并倾向「扩库 + 裁语言(去异常/RTTI/TLS/默认堆)」,得到 SG14 支持,**但没有进入标准**。P0829 的 freestanding 提案系列同样未落地为 `std.freestanding` 模块。
- **最新动向**:SG14 在 2025-05 的会议记录(P3693R0)里出现「下次会议是 Embedded,我想知道大家是否认为 `std.freestanding` 模块值得推进」。**这是「还在问值不值得做」的阶段,不是「在做」。**

⇒ **未来 3~5 年内不会有标准化的 freestanding std 模块。** mcpp 若要在裸机上给用户任何 `import`-able 的标准设施,**只能自己做成一个包**(见第 7 节的 `mcpp.core` 设想),不能等标准。

反过来说,这也是 mcpp 唯一可能领先的位置:**mcpp 是 modules-first 的工具,而整个 C++ 生态还没有人发布过「模块化的 freestanding 子集」。**

---

## 4. 问题分解:裸机支持 = 六条正交轴

把 issue #403 的五个痛点重排成正交轴,可以看清哪些是引擎、哪些是生态、哪些根本不是 mcpp 的事。

| # | 轴 | 今天 | 归属 | 规模 |
|---|---|---|---|---|
| 1 | **目标身份**:triple 语言容纳 `os=none` | `none` 被当 vendor 跳过(`triple.cppm:299`),整串不可解析 | **引擎** | 小 |
| 2 | **目标规格**:ISA / ABI / code-model / 载入地址 | 无处安放;塞 `[build].cxxflags` 会漏掉依赖 | **引擎(数据表)** | 中 |
| 3 | **链接模型**:`-nostdlib -nostartfiles -static -T script`,无 loader / 无 rpath / 用 lld | `CLibMode::None` 存在但语义是「什么都没找到,退回驱动默认」——是**退化态不是正向态** | **引擎** | 中 |
| 4 | **板级支持**:`start.S` / `link.ld` / `_init` 顺序 / 复位向量 | 无 | **生态(包)** | 小(机制已有) |
| 5 | **运行/调试**:qemu runner、semihosting 退出码 | 全仓 `runner\|qemu` 有效命中 **1 处**(且无关) | **引擎(一条缝)+ 生态(命令)** | 小 |
| 6 | **标准库**:`import std` 关断 + freestanding 子集 | 无判定、无诊断 | **引擎(诊断)+ 生态(子集包)** | 小 + 大 |

关键观察:**六条里只有 1/2/3 必须动引擎,而且都不大;4/6 的重头在生态。** 这与 E2(载荷已够用)一致——mcpp 缺的是模型,不是能力。

### ⚠️ 4.1 为什么第 2 轴必须是「数据」而不是「flags」

`-march=rv64gc -mabi=lp64d -mcmodel=medany` 三者有一个共同性质:**链接进同一个镜像的每一个目标文件都必须用完全相同的值编译**,否则要么 lld 在链接期报 ABI 不匹配,要么更糟——过了但运行期行为错。

`[build].cxxflags` 的作用域是**本包**。依赖(未来的 BSP 包、驱动库、第三方 freestanding 库)拿不到它们。所以 issue #403 里那份 `mcpp.toml` 即使 mcpp 修好了「宿主解析」问题,**一旦引入第二个包就会碎**。

Rust 把这些放进 target spec(数据),CMake 放进 toolchain file(数据),Conan 放进 profile(数据)。**三家独立收敛到同一个答案,这不是巧合。**

同时这条与仓库既有原则直接对齐:`.agents/docs/2026-08-18-open-items-analysis-and-axis-discipline.md` 的 flag-axis 纪律、以及 `#233/#240/#242/#344` 那批「同一决策两处推导 = 架构债」。**目标规格必须是一张表,被编译 flags、链接 flags、产物命名、runner、`cfg()` 求值共同消费。**

---

## 5. 三条路线

### Route A — 明确不做(维持决策 #15)

- **做什么**:把 `os=none` 加入 triple 语言**仅用于识别并拒绝**,给一条清楚的「mcpp 不支持裸机目标,建议使用 Makefile/CMake」诊断;修掉 E1 的静默失败。
- **代价**:issue #403 的 toy_kernel 继续用 Makefile;C++ 裸机生态与 mcpp 无关。
- **何时选它**:如果判断裸机用户与 mcpp 的核心价值(modules + 包管理 + 现代 C++23)重叠太少。⚠️ 但注意:**决策 #15 的两条理由已被 E2/E4 部分推翻**——「需要 freestanding modules 核心特性」是错的(模块在裸机上直接可用,E2),真正缺的只是 `import std`(E4);「512KB MCU 上 modules 优势无关」对 MCU 成立,**对 RISC-V 内核/SoC 不成立**(toy_kernel 正是后者)。

### Route B — 最小可落地缝(**推荐**)

开三条缝,板级走包:

1. **triple 语言收下 `os=none`**(`env` 取 `elf`,canonical `riscv64-none-elf` —— 与 Rust/LLVM 拼写一致),`family()` 返回空(⇒ `cfg(unix)` / `cfg(windows)` 自动为假,已是现有行为),新增 `cfg(freestanding)` 或 `cfg(os = "none")` 谓词。
2. **`kKnownTargets` 增加目标规格列**(ISA / ABI / code-model / 默认链接器),并让 `[target.<triple>]` 能覆写这些键 —— 闭词表 + 逃生舱,与仓库既有风格一致。
3. **`CLibMode` 增加正向的 `Freestanding` 态**:发 `-ffreestanding -nostdlib -nostartfiles -static -T <script>`,**不发** loader / rpath / crt 前缀;链接器强制走载荷内的 `ld.lld` 绝对路径(E6-1)。hermetic 检查在此模式下**从「报告泄漏」升级为「任何宿主路径即致命」**。
4. **`[target.<triple>].runner`**:`mcpp run` 把产物路径追加到 runner 命令尾部执行(Cargo 同款语义)。
5. **`import std` 在 `os=none` 上关断 + 诊断**。
6. **BSP 走包**:链接脚本、`start.S`、`crti/crtn`、runner 命令由索引里的一个普通包提供(`riscv-virt-rt` 之类)。**需要新增的只有「链接脚本 + 链接顺序」这一类 provision**;工具、host module、依赖目录等 provision 机制(`src/build/provisions.cppm` 的 `kTable`)已经就位,新增一行即可。

- **不做**:不建板子数据库、不打包 GCC 裸机交叉链、不做 freestanding std 子集。
- **产出**:issue #403 的 toy_kernel 能用 `mcpp build/run --target riscv64-none-elf` 全流程跑通,`mcpp.toml` 里没有一条 hack。

### Route C — 全栈裸机(方向,不是下一步)

在 B 之上加:per-target compiler-rt builtins(E3)、freestanding `__config_site` + picolibc 载荷(E5)、**`mcpp.core` 模块化 freestanding 子集**(第 3 节论证的差异化位置)、`mcpp test` 的 qemu + semihosting 退出码、`mcpp debug` 的 gdb stub 接线、BSP 模板与 `mcpp new --template`。

- **这是「C++ 世界的 embedded Rust 生态」**,也是唯一真正做到 issue 里那句「简单容易」的形态。
- **规模与 `2026-07-24` 嵌入式方案相当或更大**,不应与 B 捆绑决策。

**推荐:B。** 理由:E2 证明能力已在,B 的引擎改动被六条轴框得很死;而 A 与 C 都需要先回答「mcpp 要不要这个用户群」这个战略问题,B 不需要——**B 的每一条缝在非裸机场景也都有独立价值**(目标规格表、正向链接模型、runner 都是通用能力)。

---

## 6. 设计侧:引擎要动什么

按「一个决策一处推导」的要求列。以下是**分析结论,不是实施计划**。

### 6.1 一张目标规格表(第 2 轴)

`triple.cppm` 的 `TargetInfo` 今天有 5 列(canonical / tier / note / pin / defaultStatic)。裸机需要它长出规格列:

```
{ "riscv64-none-elf", "planned", "bare", "llvm@22.1.8", true,
  .isa = "rv64gc", .abi = "lp64d", .codeModel = "medany",
  .linker = Linker::Lld, .clib = CLibMode::Freestanding }
```

**消费方必须收敛到这一处**:编译 flags、链接 flags、`cfg()` 上下文、产物命名(`.elf`)、runner 选择。⚠️ 这里最容易犯的错是「编译侧读表、链接侧再推一遍」——那正是 `linkmodel.cppm` 头注释里记载的 #195 原始病灶(四份发散的副本)。

### 6.2 `CLibMode::Freestanding`(第 3 轴)

现有 `CLibMode::None` 的语义是**否定式**的:「什么都没找到 —— 驱动默认(宿主)生效;hermeticity 检查报告漏进来了什么」(`linkmodel.cppm:27-29`)。裸机需要的是**肯定式**的第四态:

| | `None`(今天) | `Freestanding`(新) |
|---|---|---|
| 语义 | 没找到 C 库 | **明确不要** C 库 |
| CRT | 驱动默认 | `-nostartfiles`,CRT 由 BSP 提供 |
| loader / rpath | 驱动默认 | **禁止**(PT_INTERP 必须不存在) |
| 宿主路径泄漏 | 报告 | **致命** |
| 链接脚本 | — | 必需,`-T <path>` |

⚠️ **不要复用 `None`。** `None` 今天的行为(退回宿主驱动默认)恰恰是 E1 的静默失败,把裸机接到它上面等于把新功能建在已知缺陷上。

### 6.3 runner 缝(第 5 轴)

`mcpp run` 今天假定产物可直接 exec(`cmd_build.cppm:153` → `build_run_target`)。最小改动:目标规格/`[target.<triple>]` 提供 runner argv,`run` 在其尾部追加产物路径。`mcpp test` 同理,但**需要一个退出码约定**(qemu semihosting / `sifive_test` 设备)—— 这条属于 C,B 阶段可只做 `run`。

### 6.4 `import std` 的关断(第 6 轴)

现有 `hasImportStd` 门 + import-std 诊断(`prepare.cppm` 附近)已经是正确的挂点。需要的是:`os=none` ⇒ `hasImportStd = false`,且诊断文案要说清**为什么**(「`import std` 需要 hosted 实现;`riscv64-none-elf` 是 freestanding 目标」),并指向替代路径。

⚠️ **不要做部分 std。** 见第 1.4 节。

### 6.5 BSP 的 provision 形状(第 4 轴)

BSP 包需要向消费者贡献:链接脚本路径、启动目标文件、**链接顺序约束**(OSDev 的 `crti → crtbegin → user → crtend → crtn`)、runner 命令。

`src/build/provisions.cppm` 的 `kTable` 设计意图正好是这个:「一个 provision KIND 是 kTable 的一行,每行声明它是否可按裸名寻址、是否只在显式 re-export 的边上传播」。**新增 `LinkerScript` / `StartupObjects` 两行,复用同一套 fixpoint 传播**,不新增传播逻辑。⚠️ 顺序约束是这里唯一的新语义(现有 provision 都是集合,没有序),需要单独想清楚。

---

## 7. 使用侧:「简单容易」长什么样

### 7.1 今天(issue #403 实录)

`mcpp.toml` 里 8 行 hack flags + 一份 Makefile 手写 link/run/debug + `find target -name "*.a" | head -1` 靠运气取产物。**而且按第 1 节,那份 `mcpp.toml` 今天会静默产出宿主二进制。**

### 7.2 Route B 之后

```toml
[package]
name = "toy_kernel"
version = "0.1.0"

[dependencies]
riscv-virt-rt = "1.0"          # BSP:start.S + link.ld + runner,来自索引

[target.riscv64-none-elf]
toolchain = "llvm@22.1.8"
```

```console
$ mcpp build --target riscv64-none-elf
$ mcpp run   --target riscv64-none-elf     # BSP 的 runner → qemu-system-riscv64 -kernel …
```

**用户手写的裸机知识 = 0 行。** ISA/ABI/code-model 来自目标规格表(数据),链接脚本与启动代码来自 BSP 包,运行命令来自 BSP 包。这与 Rust 的 `riscv-rt` + `runner` 组合是同构的。

再加一层糖(仍属 B):

```console
$ mcpp new toy_kernel --template riscv64-virt-kernel
```

### 7.3 `mcpplibs.std.freestanding`:一个开发者可选用的子集包

§1.7 已经把它跑通了,所以这里写的是**实测形态**而不是设想:

```cpp
export module kmain;
import mcpplibs.std.freestanding;          // ← 可选;不 import 就是纯裸机 C++

struct Task { int prio; char id; };

export extern "C" void kmain() {
    std::array<Task, 4> t{{{3,'c'},{1,'a'},{4,'d'},{2,'b'}}};
    std::ranges::sort(t, {}, &Task::prio);
    std::optional<Task> top = t.empty() ? std::optional<Task>{} : std::optional{t.back()};
    std::atomic<int> seen{0};
    std::span<Task> sp{t};
    for (auto const& x : sp) { seen.fetch_add(1); /* MMIO */ }
    // std::thread / std::mutex 在这里根本不存在 —— 编译期就报错,不是运行期惊喜
}
```

关键性质(全部实测):

- **名字就是 `std::`,不是影子命名空间。** `.inc` 用的是 `export namespace std { using std::span; }` —— 再导出的是**同一个实体**,不是副本。所以它与任何 `#include <span>` 的代码 ODR 兼容,也不会造成「两套 span」。这也意味着**用户代码从裸机移回 hosted 时一行不用改**。
- **可选。** 不 import 它,项目就是纯裸机 C++(E2 那个内核就没用它)。
- **源文件是生成的**(208 行,全是 `#include`),导出表由 libc++ 上游维护。
- **`.inc` 清单 = 这个包的全部策略。** 「哪些设施在裸机上算数」这个领域判断落在**包**里,与 `2026-07-24` 决策 #6(绑定质量是封装作者的领域知识,不是 mcpp 机制)一致。

⚠️ 三条诚实的代价:

1. **libc++ 侧要跟随版本**(`__config_site` 每 target 一份,随版本走)。libstdc++ 侧没有这个负担(`-ffreestanding` 即可,Y1)。
2. **T3 档需要为目标编 libc++.a**(`std::format`、标量 `std::sort`)。T1/T2 不需要。
3. ~~绑定 clang/libc++~~ —— **这条已被 Y3 推翻**:同一份实现中立的 `core.cppm`(自己写 `export namespace std { using std::span; … }`,不用 `.inc`)在 **libstdc++ + `-ffreestanding`** 和 **libc++ + `__config_site`** 两边都编过。导出的名字是标准规定的,`.inc` 只是一份方便的清单。**代价变成:适配层要自己维护那张名字表**(约 52 个头的公共子集),换来实现中立。这是划算的交易,也是这个包存在的真正理由 —— **它提供的是「两家都保证的可移植 freestanding 子集」,而不是「libc++ 的转发」。**

---

## 7.5 生态侧:一个库「在裸机上能不能用」怎么表示

这是比引擎改动更长期的一条,必须整体设计,否则会变成一堆各自为政的布尔键。

### 7.5.1 先把问题问对:它不是一个布尔

`freestanding = true` 表达不了任何真实情况。真实的轴是**环境能力集**:

```
heap | exceptions | rtti | threads | hosted-std | float | lockfree-atomics
```

一个库可能要堆但不要异常;可能纯头无所谓;可能只有某个 feature 要堆。**布尔会立刻不够用,然后长出第二个布尔。**

⚠️ **并且这些轴里没有一条是「hosted 专有」的**(§1.9 T1–T3b 实测):`heap` 由 BSP 的 `operator new` 替换即可(零 libc 跑通 `std::vector`),`threads` 由 RTOS 接进 gthreads / `__external_threading` 即可。**能力是可插拔的符号契约,不是「有没有操作系统」的同义词。** 本节初稿把 `std::thread` 归为 `hosted`,那是错的。

而 §1.8-Y4 已经证明**档位是环境的函数,不是标准库的函数**:libc++「更宽」只是因为 picolibc 给了堆。所以正确的建模是「能力集」,`T0/T1/T2` 只是它上面的**命名预设**。

### 7.5.2 三层,各司其职

| 层 | 谁声明 | 内容 | 何时求值 |
|---|---|---|---|
| **目标提供** | 目标规格表(§6.1)+ BSP 包 | `os=none` ⇒ 默认全关;BSP 提供 `sbrk` ⇒ 打开 `heap` | 解析期 |
| **库需要** | 包(可选) | 需要的能力集;可 per-feature | 解析期 |
| **产物验证** | mcpp | 符号审计(Y5) | 编译/链接期 |

### 7.5.3 ⚠️ 关键洞察:验证是免费的,因为裸机链接本身就是验证

X7–X10 实测:裸机链接会**逐条点名**缺什么(`memmove`、`__libcpp_verbose_abort`、`basic_string::__init`)。因为 `-nostdlib` 下**没有 libc/libstdc++ 来静默满足这些符号** —— 而 hosted 链接恰恰会。

⇒ **只要 §6.2 的 `CLibMode::Freestanding` 是真的 `-nostdlib`,「这个库用了它不该用的东西」就是一个硬错误,不需要任何新机制。** Y5 的符号表只是把同一件事**提前**到单个目标文件层面并翻译成人话。

### 7.5.4 ⇒ 因此元数据的职责不是正确性,是把错误提前

这一步决定了整个设计的取舍:

| | 有元数据 | 无元数据 |
|---|---|---|
| 正确性 | ✅ | ✅(链接期硬错) |
| 错误发生在 | **解析期,报「包 foo 需要 heap,目标 riscv64-none-elf 未提供」** | 链接期,报 `undefined symbol: _Znwm` |
| 用户能否行动 | 能(换包/开 BSP heap) | 难(要自己把 mangled 符号翻译回包) |

**元数据是诊断质量,不是正确性依赖。** 这条直接解掉最难的兼容问题(下一节)。

### 7.5.5 ⚠️ 不能要求生态先标注

mcpp-index 里已发布的包不会为了裸机去改 manifest。而且按索引地板那次的教训(`index-floor-must-degrade`):**不认识的键会让旧客户端整份 manifest 加载失败 ⇒ 已发布包永远无法采用新键。**

因为 7.5.4 成立,这个问题消失:

- **未标注 = 未知,不是不支持。** 不可证伪就放行(与 `mcpp add` 存在性门同一判据),让链接期兜底。
- **hosted 目标上完全不引入门**(零行为变化)。
- **验证结果由 `mcpp publish` 计算并写进索引描述符**,不是写进用户的 manifest —— **索引是数据,manifest 是声明**。这与 PR#451 的 `[[runtime.artifacts]] 承载证据` 同形。

### 7.5.6 落到 mcpp 现有词表,新增键尽量为 0

| 需要表达的 | 用现有的什么 | 是否要新键 |
|---|---|---|
| 「这段代码只在有堆时编」 | `[target.'cfg(...)'.build]` 条件通道 + 新增 `cfg(freestanding)` 谓词 | **0**(谓词是词表扩展,不是新段) |
| 「这个 feature 需要堆」 | 已有 feature 系统 | **0** |
| 「这个库需要哪些能力」 | `[runtime] capabilities/provides` 已是 requires/provides 语法;但 `[runtime]` 语义是**运行期宿主能力**,环境契约是**编译/链接期目标属性**,塞进去会是第二次「一个名字量两件事」 | 需要时再开,**且要先证明诊断确实不够** |
| 「验证结果」 | 索引描述符 | **0**(索引侧字段,非 manifest) |

**建议分期**:先做 `cfg(freestanding)` 谓词 + 链接期兜底 + 符号审计诊断(**manifest 零新增键**);**只有当诊断被证明不够好时**,再考虑声明键。这与本仓「先证明需要,再加键」的做法一致 —— 反例是 `cxxRuntimeTests`(#418):一个解析在 nowhere、应用在 nowhere 的键,看起来可用,什么都不做。

### 7.5.7 ⚠️ 符号审计抓不到什么(必须写进文档,否则会被当成完备保证)

1. **inline asm / 裸 syscall / MMIO** —— 不产生任何可疑符号。
2. **纯头库在自己这一侧没有产物**,需求只在**消费者**的目标文件里显形。⇒ 审计必须能在消费者侧跑(这恰好是裸机链接天然发生的地方),不能只在库发布时跑一次。
3. **能力够 ≠ 能用**:一个 T0-clean 的库可能仍然要 `<format>`(T1)。审计答的是「用了什么」,不是「够不够」。
4. **弱符号/COMDAT** 不能与 `U` 混算(与 `origin-precedence` 那次「判据必须是 GLOBAL 计数」同一类错误)。

---

## 8. ⚠️ 陷阱清单

按「会让人误判成功」的危险程度排序。

1. **E1 的静默失败会在任何路线下继续存在,除非专门修。** 它不在 issue #403 的五个痛点里(#403 报的是硬失败),是本次调研新发现的。守卫已存在于相邻路径(`prepare.cppm:1301`),逃生舱绕开了它。
2. **裸机 e2e 会因为缺 qemu 而全体静默跳过。** 本仓 261 个 e2e 用 `# requires:` 门控,现有 token 里 `gcc` 96 次、`elf` 40 次。⚠️ 按 PR#451 的教训(十个新 e2e 因 `# requires: gcc` 在 macOS/Windows **全跳过**,放开后才暴露 Windows/macOS 真实失败),**一个 `# requires: qemu` 就足以让整条裸机线在 CI 上永远绿而从未运行**。裸机验收必须**先**回答「CI 上谁真的跑了它」。
3. **hello-kernel 是 builtins 缺口的假绿**(E3)。验收用例必须含 64 位除法/软浮点。
4. **`ld.lld` 按名字寻址会拿到 GNU ld**(E6-1),而 GNU ld 对 riscv 的报错是 `cannot represent machine 'riscv'` —— 一条**不像是「找错链接器」**的错误信息,极易误判成目标不支持。
5. **`-mcmodel` 漏了会在链接期才炸,且报错是 relocation 越界**(E6-2),同样不像配置缺失。
6. **`[build].cxxflags` 路线在单包工程上能跑通**,引入第二个包才碎(§4.1)。⚠️ 也就是说:**用 toy_kernel 单包验证「flags 方案够用了」会得到错误结论**。
7. **CMake 的 `CMAKE_TRY_COMPILE_TARGET_TYPE` 教训适用于 mcpp**:任何「编个小程序链一下」的探测(doctor / 工具链能力探测 / `hasImportStd` 探测)在 `os=none` 下都会假失败。这条需要在实施前普查。
8. **`import std` 若做成「部分可用」,会污染 BMI 缓存身份。** 参考 C++20 档位设计里「跨档位 BMI 硬拒 ⇒ 零缓存改动」的先例——freestanding 必须是**缓存键的一个轴**,或者干脆硬拒。
9. **决策 #15 的两条理由需要更新**(不是推翻整个决策,是更新它的依据):模块本身在裸机可用(E2),真正缺的只有 `import std`(E4);MCU 论证对 RISC-V SoC/内核不成立。
10. **⚠️「N 个头能 include」是一个几乎无意义的数字,除非配真实使用探针**(X4)。`__config_site` 把 LOCALIZATION/THREADS 关掉后,`<iostream>` 仍然「能 include」——因为它被守卫成了近乎空文件。**统计 include 成功率 = 统计空文件数量。**
11. **⚠️ 没有宿主对照组,会把「libc++ 还没实现」记成「裸机不支持」**(X3)。7 个失败头在 x86_64 + 完整 libc++ 上同样失败。**对照组把「freestanding 的编译期损失」从「7 个头」修正为「0」。**
12. **⚠️ picolibc 的发行版包没有 rv64gc/lp64d 变体**(X11)。用 `rv64imac/lp64` 验证通过**不能**推广到最常见的 RISC-V profile —— 这是一个「换个 `-march` 就没有库」的隐形边界。
13. **⚠️「libc++ 比 libstdc++ 的 freestanding 子集更宽」是错的排序**(Y1/Y2)。103 vs 53 里,libc++ 的一半是**能 parse 的空壳**(`std::mutex` 根本不存在),libstdc++ 是硬 `#error`。**更宽的那个数字来自更含糊的失败方式。** 我自己在 §1.7 就是靠这个数字得出了「绑定 clang/libc++」的错判,Y3 推翻了它。
14. **⚠️ 符号审计不是完备保证**(§7.5.7):inline asm / 裸 syscall 不留痕;纯头库在自己这侧没有产物;弱符号不能与 `U` 混算。**把它当成完备门会比没有门更危险。**

---

## 9. 待 review 的决策点

请逐条给方向,后续才好出实施方案。

| # | 决策点 | 选项 | 本文倾向 |
|---|---|---|---|
| D1 | **E1 的静默失败何时修** | (a) 立即,独立于路线 (b) 随路线一起 | **(a)** —— 这是已发布能力可达的错误行为,与 RFC 结论无关 |
| D2 | **裸机的战略定位** | A 明确不做 / **B 最小缝** / C 全栈 | **B**,C 作为方向留白 |
| D3 | **triple 语言收不收 `os=none`** | 收(canonical `riscv64-none-elf`)/ 不收 | **收**;即使选 A 也需要它来做「识别并拒绝」 |
| D4 | **目标规格放哪** | (a) `kKnownTargets` 加列 (b) 独立 target-spec 文件 (c) 只 `[target.<triple>]` 键 | **(a)+(c)**:闭词表 + 逃生舱,与仓库风格一致;(b) 留给 C |
| D5 | **`CLibMode` 加正向 `Freestanding` 态,还是复用 `None`** | 加 / 复用 | **加**;复用等于把新功能建在 E1 的缺陷上 |
| D6 | **BSP 走包还是走引擎** | 包(provision 加行)/ 引擎板子表 | **包**;与决策 #1(不做发行版构建器)一致,也与 Conan 的收敛一致 |
| D7 | **`import std` 在 `os=none`** | 硬关断 + 诊断 / 尝试子集 | **硬关断**;子集是 `mcpp.core` 包的事(C) |
| D8 | **首个验证目标** | `riscv64-none-elf` @ qemu virt(toy_kernel 现成)/ `armv7m-none-eabi` @ MCU | **riscv64 virt**:有现成用例、有 E2 的实测通路、qemu 可在 CI 装 |
| D9 | **CI 怎么真跑** | qemu 进 CI / 只做编译+链接断言 / 不做 | 需要明确答案,否则触发陷阱 #2 |
| D10 | **compiler-rt builtins 的来源** | 自建 / 采纳 LLVM Embedded Toolchain / 不做(B 阶段) | B 阶段可**不做但要诊断**;C 阶段倾向采纳既有成品(§2.4) |
| D11 | **`mcpplibs.std.freestanding` 做不做、什么时候做** | (a) B 阶段就做(它是纯包,不依赖引擎改动)(b) 归到 C (c) 不做 | **(a) 可行且诱人**:§1.7 已跑通,且它**不需要任何引擎改动**——但它会把裸机线绑到 clang/libc++(见 D12) |
| D12 | **裸机线的编译器**(§1.8 后**已改判**) | clang 优先 / GCC 优先 / **两家都支持,适配层中立** | **两家都支持**。Y3 证明中立适配层可行;Y1 证明 libstdc++ 的 freestanding 是零配置的 `-ffreestanding` 且诊断更好。⚠️ 但 **clang 天然多目标**(E2)在裸机这条线上仍是结构性优势 —— 结论是「载荷优先 clang、库中立」,**不是分叉** |
| D13 | **freestanding libc 的来源** | picolibc(自建 multilib)/ 采纳 LLVM Embedded Toolchain / newlib | **picolibc**;⚠️ 但发行版包**没有 rv64gc 变体**(X11),必须自建或采纳成品 |
| D14 | **`mcpplibs.std.freestanding` 的形态** | 转发 libc++ `.inc` / **自写实现中立的名字表** | **自写中立表**(§7.3 修正条 3):代价是维护约 52 个头的公共子集清单,换来两家标准库通用 + 可移植性成为包的卖点 |
| D15 | **生态如何表示 freestanding 可用性** | 加 manifest 声明键 / **cfg 谓词 + 链接期兜底 + 索引侧验证结果** | **后者**(§7.5):manifest **零新增键**;元数据只负责把错误提前,不负责正确性;⚠️ 不能要求已发布包先标注 |
| D16 | **档位命名** | `T0/T1/T2` / `freestanding-core` + `freestanding+heap` + `hosted` / 直接用能力集 | 倾向**能力集为准、档位只做命名预设**(§7.5.1)—— 布尔和固定档位都会很快不够用 |

---

## 10. 四个视角:同一批证据,谁该做什么

前九节按「证据 → 路线 → 设计」组织。这一节把同一批结论按**角色**重投影 —— 因为四个角色的义务、失败模式和「对我意味着什么」完全不同。

### 10.1 mcpp 引擎/架构侧

**拥有(四件,每件必须「一处推导」):**

| # | 拥有什么 | 落点 | 规模 |
|---|---|---|---|
| 1 | **目标身份 + 目标规格表** | `triple.cppm`:`os` 词表加 `none`;`TargetInfo` 长出 isa/abi/code-model/linker/clib-mode/env-capabilities 列 | 中 |
| 2 | **正向的 `CLibMode::Freestanding`** | `linkmodel.cppm`:`-nostdlib -nostartfiles -static -T`,不发 loader/rpath/crt 前缀 | 中 |
| 3 | **runner 缝** | `mcpp run/test` 不再假定产物可 exec | 小 |
| 4 | **诊断与门** | E1 的静默失败、`import std` 关断、符号审计翻人话 | 小 |

**明确不拥有:**板子数据库(与决策 #1 冲突)、std 子集的内容策略(决策 #6:领域知识归包)、libc 实现、「哪些设施算 freestanding」的判断。

**⚠️ 引擎侧四个架构风险(全部有本仓先例):**

1. **同一决策两处推导** —— 规格表若被编译侧读一遍、链接侧再推一遍,就是 #195 的原始病灶复发(`linkmodel.cppm` 头注释记着那四份发散副本)。
2. **修补放在控制流到不了的地方** —— E1 就是活例:`host_can_serve` 守卫挂在 `known` 分支,逃生舱走 `!known`。**新加的门必须自己回答「逃生舱走不走这里」。**
3. **BMI 缓存键漏掉 freestanding 轴** —— 同一份源码在 hosted / freestanding 下的 BMI 不可互换(cpp20 档位「跨档位 BMI 硬拒」的先例)。
4. **⚠️ 探测类代码在 `os=none` 下假失败** —— CMake 因此才有 `CMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY`。mcpp 里任何「编个小程序链一下」的探测(doctor、工具链能力探测、`hasImportStd` 探测)都要**在实施前普查一遍**,否则裸机目标会在探测阶段就假失败。

**规模判断**:B 路线的引擎改动集中在 `triple.cppm` / `linkmodel.cppm` / `prepare.cppm` 三个**已有结构**里,不是新子系统。

### 10.2 生态侧(载荷 / 索引 / 包)

**要交付四类东西,其中只有第一类是真的新工程量:**

| 类 | 内容 | 状态 |
|---|---|---|
| **载荷** | per-target compiler-rt builtins(E3);picolibc(⚠️ 发行版包无 rv64gc,X11);T3 档的目标版 libc++.a;libc++ 的每目标 `__config_site` | **唯一真缺口**;可采纳 LLVM Embedded Toolchain(§2.4) |
| **BSP 包** | `start.S` + `link.ld` + `crti/crtn` + 默认 `__libcpp_verbose_abort`(3 行)+ runner 命令 | 机制近乎已有;⚠️ 缺「链接**顺序**」这一种 provision |
| **`mcpplibs.std.freestanding`** | 实现中立名字表(约 52 头交集,D14) | §1.7/1.8 已跑通;**长期维护品,不是写一次** |
| **索引侧验证结果** | `mcpp publish` 算能力集写进描述符 | 零 manifest 改动 |

**⚠️ 生态侧头号风险:CI 上没人真跑。** 一个 `# requires: qemu` 就让整条裸机线永远绿(陷阱 #2,PR#451 同款)。**这条要在动手前回答,不是收尾时。**

**⚠️ 第二风险:载荷的 multilib 覆盖是隐形边界。** X11 实测:换个 `-march` 就没有库。载荷的「支持哪些 ISA 组合」必须是**可查询的数据**,不能是「试了才知道」。

### 10.3 应用开发者(写内核/固件的人)

**今天** → issue #403 实录:8 行 hack flags + Makefile 手写 link/run/debug + `find target -name "*.a" | head -1` 靠运气。⚠️ 而且那份 `mcpp.toml` 今天会**静默产出宿主二进制**(E1)。

**B 路线后** → 手写的裸机知识 **0 行**(§7.2)。

**他必须知道的三件事(「简单容易」的真实边界):**

1. **`import std` 没有**;但 `import mcpplibs.std.freestanding` 有,**而且名字就是 `std::`** —— 导出的是同一个实体,不是影子命名空间 ⇒ **代码搬回 hosted 一行不用改**。这是对开发者最重要的承诺:*你写的是 C++,不是裸机 C++ 方言。*
2. **`std::thread` / `std::mutex` 不存在是编译期报错**,不是运行期惊喜(X4/Y2 实测)。libstdc++ 甚至会点名 `#error "This header is not available in freestanding mode."`
3. **堆由 BSP 提供**;没有堆就没有 `vector`/`string`/`format`(T0 vs T1,§1.7)。

**⚠️ 他会踩的两个坑:**
- `-mcmodel` 漏了**在链接期才炸**,报错是 relocation 越界(E6-2),完全不像「配置缺失」。
- **单包跑通不代表加了依赖还能跑** —— ISA/ABI/code-model 若走 `[build].cxxflags`,依赖拿不到(§4.1)。⚠️ 用 toy_kernel 单包验证「flags 够用了」会得到错误结论。

### 10.4 库作者(发包的人)

这个视角最有意思,因为它决定生态能不能长起来 —— 而 C++ 在这里与 Rust 有**结构性差异**。

**⚠️ Z1 实测:纯头库的裸机可用性不是库的属性,是消费者的属性。**

同一个纯头模板库 `tinylib`,两个消费者:

```
useA.cpp (用 Ring<int>) → (空)          ⇒ freestanding-clean
useB.cpp (用 Vec<int>)  → _Znam _ZdaPv  ⇒ 需要 heap
库自身编出的目标文件     → 无实例化,无任何符号可审
```

⇒ **「库 X 支不支持 freestanding」在 C++ 里普遍无法在库级回答。** Rust 的 `#![no_std]` 是 crate 级 + 编译器强制,C++ 侧**做不到等价的静态保证**。这不是 mcpp 的缺陷,是语言的形状,必须诚实写进文档。

**架构后果(强化 D15):**

- 库级标注只能是**发现/诊断层**(「这个库大概率能用」),**不能是门**。
- 真正的判据必须在**消费者的链接**上 —— 而那恰好是免费的(§7.5.3:`-nostdlib` 下没有 libc 来静默满足符号)。
- ⇒ **更不该建 manifest 声明键当门。**

**库作者的四级投入,L0 必须永远可用:**

| 级 | 做什么 | 成本 | 得到什么 |
|---|---|---|---|
| **L0 什么都不做** | — | 0 | 未标注 = **未知,不是不支持**;hosted 用户零影响;裸机用户靠链接期兜底 |
| **L1 跑一次审计** | 让 mcpp 报告用了什么 | 极低 | 常见结果:**发现自己其实已经 clean** |
| **L2 条件化** | `cfg(freestanding)` gate 掉要堆的路径 | 中 | 同一个包同时服务两端 |
| **L3 声明 + CI** | 在裸机目标上跑 CI | 高 | 可信承诺 |

⚠️ **L0 永远可用是硬承诺。** 一旦「不标注就被排除」,生态会立刻分裂成两半 —— 而 mcpp-index 里已发布的包不会为裸机回头改 manifest(索引地板那次的教训)。

**⚠️ 库作者的三个陷阱:**
1. **「我本地审计过了」不等于消费者那边干净** —— 纯头库自身没有产物(Z1)。
2. **能力够 ≠ 能用**:T0-clean 的库仍可能需要 `<format>`(T1)。审计答的是「用了什么」,不是「够不够」。
3. **feature 会改变答案** —— 一个 feature 开了就要堆 ⇒ 能力集必须能 per-feature,否则又是一个「一个名字量两件事」。

### 10.5 一句话对齐

| 角色 | 一句话 |
|---|---|
| 引擎 | **加三条缝(target/link/runner),不加板子数据库;新门必须自己回答「逃生舱走不走这里」。** |
| 生态 | **唯一真缺口是载荷(builtins + libc + T3 libc++);其余是包。⚠️ 先回答 CI 上谁真跑。** |
| 开发者 | **裸机知识手写 0 行;`std::` 还是 `std::`,搬回 hosted 一行不用改。** |
| 库作者 | **什么都不做也不会被排除;C++ 里 freestanding 是消费者属性,库级标注只能是诊断不能是门。** |

---

## 附录 A:复现命令

```bash
# E1 —— 静默错构(在任意空目录)
cat > mcpp.toml <<'EOF'
[package]
name = "toy_kernel"
version = "0.1.0"
[target.riscv64-none-elf]
toolchain = "llvm@22.1.8"
EOF
mkdir -p src && echo 'int main(){return 0;}' > src/main.cpp
mcpp build --target riscv64-none-elf          # → Finished dev … 成功
file target/x86_64-linux-gnu/*/bin/toy_kernel # → x86-64 dynamic PIE
grep -c -- --target target/x86_64-linux-gnu/*/build.ninja   # → 0

# E2 —— 用 mcpp 现有载荷造裸机内核
L=~/.xlings/data/xpkgs/xim-x-llvm/22.1.8
CXX="$L/bin/clang++ --no-default-config --target=riscv64-none-elf -march=rv64gc \
     -mabi=lp64d -mcmodel=medany -ffreestanding -fno-exceptions -fno-rtti \
     -nostdinc++ -std=c++23"
$CXX --precompile kmain.cppm -o kmain.pcm && $CXX -c kmain.pcm -o kmain.o
$L/bin/clang --no-default-config --target=riscv64-none-elf -march=rv64gc \
     -mabi=lp64d -mcmodel=medany -c start.S -o start.o
$L/bin/ld.lld -T link.ld start.o kmain.o -o kernel.elf   # 必须用绝对路径
$L/bin/llvm-objcopy -O binary kernel.elf kernel.bin
$L/bin/llvm-nm -u kernel.elf

# E3 —— builtins 只有宿主
ls $L/lib/clang/22/lib/

# E4 —— std 模块无 freestanding 分区
grep -c "HOSTED\|freestanding" ~/.xlings/data/xpkgs/xim-x-gcc/16.1.0/include/c++/16.1.0/bits/std.cc
grep -c "_LIBCPP_HAS_" $L/share/libc++/v1/std.cppm

# E5 —— 合成 freestanding __config_site 后的头子集
sed -e 's/#define _LIBCPP_HAS_THREADS 1/#define _LIBCPP_HAS_THREADS 0/' \
    -e 's/#define _LIBCPP_HAS_LOCALIZATION 1/#define _LIBCPP_HAS_LOCALIZATION 0/' \
    -e 's/#define _LIBCPP_HAS_FILESYSTEM 1/#define _LIBCPP_HAS_FILESYSTEM 0/' \
    -e 's/#define _LIBCPP_HAS_MONOTONIC_CLOCK 1/#define _LIBCPP_HAS_MONOTONIC_CLOCK 0/' \
    $L/include/x86_64-unknown-linux-gnu/c++/v1/__config_site > fs/c++/v1/__config_site
$CXX -isystem fs/c++/v1 -isystem $L/include/c++/v1 -c <(echo '#include <span>') -o /dev/null

# E6 —— ld.lld 名字陷阱
readlink -f $(which ld.lld); ld.lld --version | head -1

# ── X 系列:std.freestanding 子集包 ────────────────────────────────────────
# picolibc:只下载 + 本地解包,不装进系统
apt-get download picolibc-riscv64-unknown-elf
dpkg -x picolibc-riscv64-unknown-elf_*.deb root
P=$PWD/root/usr/lib/picolibc/riscv64-unknown-elf

CXX="$L/bin/clang++ --no-default-config --target=riscv64-none-elf \
     -march=rv64imac -mabi=lp64 -mcmodel=medany -ffreestanding \
     -fno-exceptions -fno-rtti -std=c++23 -nostdinc++ \
     -isystem fs/c++/v1 -isystem $L/include/c++/v1 -isystem $P/include \
     -I $L/share/libc++/v1"

# X3 —— 110 个 std/*.inc 对应头的可编性 + ⚠️ 宿主对照组
for inc in $L/share/libc++/v1/std/*.inc; do h=$(basename $inc .inc)
  printf '#include <%s>\nint f(){return 0;}\n' "$h" > h.cpp
  $CXX -c h.cpp -o /dev/null 2>/dev/null && echo "$h" >> ok.list || echo "$h" >> fail.list
done                                          # → OK=103 FAIL=7
for h in $(cat fail.list); do                 # ⚠️ 对照组:这 7 个在宿主也全败
  printf '#include <%s>\nint f(){return 0;}\n' "$h" > h.cpp
  $L/bin/clang++ -std=c++23 -stdlib=libc++ -c h.cpp -o /dev/null 2>/dev/null \
    && echo "HOST-OK $h(真·裸机损失)" || echo "HOST-FAIL $h(libc++ 未实现)"
done

# X5 —— 生成子集模块(208 行,全是 #include)
{ echo "module;"; sed 's|.*|#include <&>|' ok.list
  echo "export module mcpplibs.std.freestanding;"
  sed 's|.*|#include "std/&.inc"|' ok.list; } > stdfs.cppm
$CXX -Xclang -emit-reduced-module-interface --precompile stdfs.cppm -o stdfs.pcm
$CXX -c stdfs.cppm -o stdfs.o

# X10 —— 消费 + 链接,零未定义符号(rt.cpp 仅 3 行 __libcpp_verbose_abort)
$CXX -fmodule-file=mcpplibs.std.freestanding=stdfs.pcm -c kmain4.cppm -o kmain4.o
$L/bin/ld.lld -T link.ld start.o kmain4.o stdfs.o rt.o \
    -L $P/lib/rv64imac/lp64 -lc -o kernel4.elf
$L/bin/llvm-nm -u kernel4.elf     # → 空
$L/bin/llvm-size kernel4.elf      # → text 12435

# X11 —— picolibc multilib 覆盖(⚠️ 无 rv64imafdc/lp64d)
(cd $P/lib && for d in rv64*; do for a in $d/*/; do printf "%s " "${a%/}"; done; done)

# ── Y 系列:实现中立适配层 + 机械验证 ──────────────────────────────────────
# Y1 —— libstdc++ 的 freestanding 是编译期开关,不是配置文件
grep -n "_GLIBCXX_HOSTED" $G/include/c++/16.1.0/x86_64-linux-gnu/bits/c++config.h
#   → #define _GLIBCXX_HOSTED __STDC_HOSTED__

# Y2 —— 不可用设施的表现:libstdc++ 点名 #error
echo '#include <vector>' > u.cpp
g++ -std=c++23 -ffreestanding -c u.cpp -o /dev/null
#   → #error "This header is not available in freestanding mode."

# Y3 —— 同一份中立 core.cppm,两个标准库都过
g++      -std=c++23 -fmodules -ffreestanding -fno-exceptions -fno-rtti -c core.cppm -o core_gcc.o
$CXX     --precompile core.cppm -o core_clang.pcm         # riscv64-none-elf + libc++

# Y4 —— 可移植子集 = 两家交集(52)
comm -12 <(sort ok.list) <(sort gcc_ok.list) | wc -l

# Y5 —— 从产物机械判定环境需求(零元数据)
g++ -std=c++23 -c libprobe.cpp -o libprobe.o && $L/bin/llvm-nm -u libprobe.o
#   new/delete → _Znwm _ZdlPvm | throw → __cxa_throw | dynamic_cast → __dynamic_cast
#   纯计算     → (空)

# Z1 —— ⚠️ 纯头库:可用性是消费者的属性,不是库的属性
g++ -std=c++23 -c useA.cpp -o useA.o && $L/bin/llvm-nm -u useA.o | grep -E "_Zn|_Zd"
#   → (空)            useA 只用了 Ring<int>
g++ -std=c++23 -c useB.cpp -o useB.o && $L/bin/llvm-nm -u useB.o | grep -E "_Zn|_Zd"
#   → _Znam _ZdaPv    useB 用了 Vec<int>
#   而库自身:无实例化 ⇒ 无任何符号可审
```

## 附录 B:参考来源

- OSDev Wiki:[Bare Bones](https://wiki.osdev.org/Bare_Bones)、[C++](https://wiki.osdev.org/C%2B%2B)、[Calling Global Constructors](https://wiki.osdev.org/Calling_Global_Constructors)、[Meaty Skeleton](https://wiki.osdev.org/Meaty_Skeleton)、[Expanded Main Page](https://wiki.osdev.org/Expanded_Main_Page)
- Rust:[The Embedonomicon — Creating a custom target](https://docs.rust-embedded.org/embedonomicon/custom-target.html)、[rustc platform support: riscv64im-unknown-none-elf](https://doc.rust-lang.org/nightly/rustc/platform-support/riscv64im-unknown-none-elf.html)
- WG21:[P1642R11 Freestanding Library](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p1642r11.html)、[P2338R4 Freestanding Library: Character primitives and the C library](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2023/p2338r4.html)、[P1212R0 Modules and Freestanding](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p1212r0.html)、[P0829R3 Freestanding Proposal](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2018/p0829r3.html)、[P2465 std / std.compat modules](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2022/p2465r3.pdf)、[P3693R0 SG14 minutes (2025-05)](https://www.open-std.org/jtc1/sc22/wg21/docs/papers/2025/p3693r0.pdf)
- 工具链:[LLVM Embedded Toolchain for Arm](https://github.com/ARM-software/LLVM-embedded-toolchain-for-Arm)、[picolibc](https://github.com/picolibc/picolibc)、[Trying to build libc++ for bare-metal ARM targets (LLVM Discourse)](https://discourse.llvm.org/t/trying-to-build-libc-for-bare-metal-arm-targets/90501)
- 其他构建系统:[cmake-toolchains(7)](https://cmake.org/cmake/help/latest/manual/cmake-toolchains.7.html)、[Conan 1.43 baremetal setting](https://blog.conan.io/2021/12/21/New-conan-release-1-43.html)、[Zig Build Targets](https://ziglang-zig.mintlify.app/build/targets)
