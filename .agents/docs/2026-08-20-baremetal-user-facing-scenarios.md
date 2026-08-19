# 裸机:用户面能感受到的变化(场景 + 伪代码)

配套 [PR #455–#459 review](2026-08-20-pr455-459-freestanding-review.md)。
本文只讲**用户敲什么、看到什么、不用写什么**;机制在 review 里。

以下命令与输出都是**实测**的(mcpp 2026.8.19.4 + `riscv-virt-rt 0.3.0`),
不是设想的接口。

---

## 场景 0:先看「不用写什么」

这是本轮最大的用户面变化。裸机工程的**整个** manifest:

```toml
[package]
name    = "blinky"
version = "0.1.0"

[build]
target = "riscv64-none-elf"

[dependencies]
riscv-virt-rt = "0.3.0"
```

⭐ 里面**没有**:链接脚本路径 · 加载地址 · `-nostdlib` · `-march`/`-mabi`/`-mcmodel` ·
crt0 · libc 名字 · 模拟器命令行 · `[target.*]` 段 —— **一个都没有**。

对照:同样的工程在裸机 C/C++ 的常规做法里,通常需要一份 `link.ld`、一份 `start.S`、
一段 Makefile 里的 `qemu-system-riscv64 -machine virt …`,以及一个手工维护的 sysroot 路径。

---

## 场景 1:从零到一个会跑的固件

```bash
mcpp new blinky --template riscv-virt-rt
cd blinky
mcpp run
```

实际输出:

```
 Downloading mcpplibs.riscv-virt-rt v0.3.0
   Compiling blinky v0.1.0 (.)
    Finished dev [unoptimized + debuginfo] in 0.08s
        Size blinky  text 8844  data 80  bss 5668  total 14592
     Running `…/qemu-system-riscv64 … target/riscv64-none-elf/…/bin/blinky`

hello from blinky
float 3.1416
heap ok
```

生成的 `src/main.cpp` 是**普通的 `int main()`**:

```cpp
import mcpplibs.riscv_virt_rt;

extern "C" int main() {
    board::println("hello from blinky");
    board::printf("float %.4f\n", 3.14159);      // 浮点 printf 也能用
    void* p = board::alloc(64);                   // 堆也在
    board::println(p ? "heap ok" : "heap FAILED");
    board::release(p);
    return p ? 0 : 1;                             // 返回值经 semihosting 回到宿主
}
```

⚠️ **不需要 `_start`,不需要汇编入口**。板级包带了 picolibc 的 semihosting `crt0`,
所以 C 运行时在 `main` 之前就已经起来了。只有**零 libc** 的板子才需要自己写入口。

---

## 场景 2:在目标上跑测试

```bash
mcpp test
```

```
   Compiling boots (test)
     Running bin/boots
boots: console
boots ... ok (0.04s)

 test result ok. 1 passed; 0 failed; finished in 0.34s
```

⭐ **每个 `tests/*.cpp` 是一个独立镜像,在模拟器里跑,退出码即判据** ——
和宿主上的 `mcpp test` 完全一样的心智模型。

失败会**点名**:

```
ok_one ... ok
ok_two ... ok
deliberate_fail ... FAIL (exit 1, 0.02s)
error: test result: FAILED. 2 passed; 1 failed
```

⚠️ 这条不是设计出来的,是**实测**出来的:semihosting 把固件 `main` 的返回值原样传给
qemu 退出码(`return 7` → qemu 退出 7)。原计划要为裸机造一套结构化 stdout 协议,不需要。

---

## 场景 3:换 ISA 宽度 —— 改一个 flag

```bash
mcpp build --target riscv32-none-elf
mcpp run   --target riscv32-none-elf
```

**源码一个字不改,板级包一个字不改。**

```
        Size blinky  text 10680  data 44  bss 5400  total 16124
hello from blinky
float 3.1416
heap ok
```

同一个板级包用一份描述服务两个宽度:它从 `MCPP_TARGET_ARCH` 选档位,
而 ISA 参数(`-march`/`-mabi`/`-mcmodel`)来自引擎的目标表 —— **是数据不是代码**。

---

## 场景 4:烧到真硬件要的东西

```bash
mcpp build
```

```
        Size blinky  text 8844  data 80  bss 5668  total 14592
```

产物旁边直接就有:

```
target/riscv64-none-elf/<fp>/bin/blinky        # ELF(调试 / qemu -kernel)
target/riscv64-none-elf/<fp>/bin/blinky.bin    # 裸二进制(烧录器只吃这个)
target/riscv64-none-elf/<fp>/bin/blinky.map    # 链接映射
```

⭐ **size 摘要是每次构建都打印的**,因为裸机的核心约束是**容量** ——
不打印等于让用户自己去查一个每次都想知道的数。

`.map` 是「为什么这段没进来 / 为什么这段这么大」唯一能回答的东西。

---

## 场景 5:在已有工程里加板级支持

```bash
mcpp add riscv-virt-rt@0.3.0
```

```toml
[build]
target = "riscv64-none-elf"

[dependencies]
riscv-virt-rt = "0.3.0"
```

⭐ **模拟器和目标 C 库会被自动装上** —— 用户不需要事先 `xlings install` 任何东西。
(实测判据:把 store 里的 picolibc 藏起来,`mcpp add` + `mcpp build` 把它装了回来。)

---

## 场景 6:用标准库的可移植子集

```toml
[dependencies]
riscv-virt-rt    = "0.3.0"
std-freestanding = "0.2.0"
```

```cpp
import mcpplibs.riscv_virt_rt;
import mcpplibs.std.freestanding;      // 不是 `import std;`

struct Task { int prio; const char* name; };

extern "C" int main() {
    std::array<Task, 4> t{{ {3,"c"}, {1,"a"}, {4,"d"}, {2,"b"} }};
    std::ranges::sort(t, {}, &Task::prio);        // 带投影,裸机上跑
    std::optional<int> o = 41;
    std::atomic<int>   a{0};
    a.fetch_add(o.value() + 1);
    std::span<Task>       s{t};
    std::string_view      sv{"ok"};
    board::printf("atomic %d\n", a.load());
    return 0;
}
```

实测输出 `abcd` / `atomic 42` / `span 4`。

**可用**:`array` `span` `optional` `expected` `atomic` `string_view` `ranges` `algorithm`
`bit` `charconv` `concepts` `type_traits` `tuple` `utility` coroutines …
(libc++ 110 个头里的 **103** 个)

**干净地不可用**(编译期报错,不是跑起来才错):

```cpp
std::mutex m;      // error: no type named 'mutex' in namespace 'std'
```

⚠️ **需要目标版 `libc++.a` 才有的**(今天会在**链接期**失败并点名符号):
`std::format` · 内建标量类型的 `std::sort` · 完整的 `std::string`。

### 如果直接写 `import std;` 会怎样

```
error: `import std;` is not available on 'riscv64-none-elf' — a freestanding
       target has no hosted standard library.
       `std` is one module over the entire library (threads, filesystem,
       iostreams included), so there is no subset of it to build without an OS.
       Use the freestanding subset instead — an ordinary dependency carrying
       the parts of the library that need no OS (array, span, optional, atomic,
       string_view, ranges, expected, charconv, coroutines):

           [dependencies]
           std-freestanding = "0.2.0"

       then `import mcpplibs.std.freestanding;` in place of `import std;`.
```

⭐ **诊断给的那一行是能直接粘贴并跑通的** —— 这是一条硬规矩:
诊断里的每条建议都是承诺(本轮曾违反过一次,见 review §3.4)。

---

## 场景 7:调试时换掉板级包给的 runner

板级包供 runner 是常态,但工程可以覆盖:

```toml
[target.riscv64-none-elf]
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
          "-bios", "default",          # 换成 OpenSBI 启动
          "-s", "-S",                  # 挂 gdb,停在第一条指令
          "-kernel"]
```

```
note: [target.riscv64-none-elf].runner overrides the runner a dependency supplied
```

⭐ 工程写的**赢过**依赖供的,而且 mcpp 会**说出来** —— 覆盖生效时不沉默。

---

## 场景 8:给一块新板子写 BSP(伪代码)

这是生态作者面。**整个 `build.mcpp` 大约 30 行**:

```cpp
import mcpp;
import std;

int main() {
    const bool rv32 = std::string_view{mcpp::target_arch() ?: ""} == "riscv32";

    // 1. 从目标的 C 库里「选」—— 裸名即可,搜索路径引擎已经给了
    mcpp::link_lib("crt0-semihost");      // 换 UART 板就换成别的 crt0
    mcpp::link_lib("c");
    mcpp::link_lib("semihost");
    mcpp::link_lib(rv32 ? "clang_rt.builtins-riscv32"
                        : "clang_rt.builtins-riscv64");

    // 2. 这块板子的内存布局
    if (const char* sr = mcpp::sysroot_dir(); sr && *sr)
        mcpp::link_script(std::format("{}/lib/{}/picolibcpp.ld", sr,
            rv32 ? "rv32imac/ilp32" : "rv64gc/lp64d").c_str());

    // 3. 怎么把镜像跑起来
    if (const char* q = mcpp::xpkg_dir("xim", "qemu-riscv"); q && *q) {
        mcpp::runner(std::format("{}/bin/qemu-system-{}", q,
                                 rv32 ? "riscv32" : "riscv64").c_str());
        for (auto a : {"-machine","virt","-nographic","-no-reboot",
                       "-semihosting","-bios","none","-kernel"})
            mcpp::runner(a);
    }
    return 0;
}
```

⚠️ **注意它不做什么**:不找 libc、不声明 libc、不知道 libc 叫什么。
它只知道**要哪个 crt0、要哪份链接脚本、怎么起模拟器** ——
**位置是目标的事实,选择是板级的事实。**

换一块同 ISA 的板子 = 换这三段里的具体取值,**引擎零改动**。

---

## 场景 9:老版本 mcpp 上会看到什么

```
error: 'runner' is not a member of 'mcpp'
       The `mcpp` build module this engine bundles does not have that name.
       Either the package was written for a newer mcpp (try `mcpp self update`;
       this is mcpp 2026.8.19.1), or the name is misspelled — the compiler
       cannot tell the two apart, because the module is generated by whichever
       mcpp is running.
```

⚠️ 这条提示是**补出来的**,因为包侧**无法**优雅降级:
`if constexpr (requires { mcpp::runner("x"); })` 在名字不存在时是**硬错误**,不是 `false`
—— 语言内没有特性探测这条路。所以引擎在编译失败时把「可能是引擎旧了」说出来。

---

## 一句话:用户感受到的四件事

| | 之前 | 现在 |
|---|---|---|
| **起步** | 自己攒 linker script + start.S + qemu 命令行 | `mcpp new --template riscv-virt-rt` → `mcpp run` |
| **测试** | 裸机基本不测,或自造一套协议 | `mcpp test`,退出码即判据,失败点名 |
| **换宽度** | 改一堆 flag 和路径 | `--target riscv32-none-elf`,源码零改 |
| **烧录** | 自己 objcopy、自己看 size | `.bin`/`.map` 自动产出,size 每次构建都打印 |

---

## 已知边界(不要许诺给用户)

| 边界 | 表现 |
|---|---|
| `std::format` / 标量 `std::sort` / 完整 `std::string` | **链接期**失败并点名符号 —— 需要为目标编 `libc++.a`(未发布) |
| 异常与 RTTI | 整图关闭(裸机没有 unwinder);`try/catch` 编译期就不可用 |
| 第二块板 / ARM Cortex-M | 未做;rv32 只是「ISA 表是数据」的证据 |
| Windows arm64 宿主 | 装不上 `qemu-riscv`(上游无该资产),行为正确但会失败 |
| 目标 C 库不可按工程覆盖 | `[target.X]` 有 `toolchain`/`linkage`/`runner`/`cxx_runtime`,**没有 `sysroot`** —— 想换 newlib today 做不到 |
