# 13 — 裸机与 freestanding 目标

本文说明 mcpp 如何为没有操作系统的目标构建、运行与测试软件,以及板级支持包如何
提供引擎刻意不去掌握的那部分目标事实。

相关文档:[05 — mcpp.toml 清单指南](05-mcpp-toml.md) §2.7.2 是本文使用的
`[target.<triple>]` 各键的参考;[07 — build.mcpp 构建程序](07-build-mcpp.md)
是板级支持包所用指令协议的参考;[08 — 工具链内部机制](08-toolchain-internals.md)
描述了本文所偏离的宿主链接模型。

## 概述

freestanding 目标是 `os` 字段为 `none` 的目标。`src/toolchain/triple.cppm`
的目标表中有三个:

| Triple | 档位 | C 库 |
|---|---|---|
| `riscv64-none-elf` | verified | `xim:picolibc-riscv` |
| `riscv32-none-elf` | verified | `xim:picolibc-riscv` |
| `aarch64-none-elf` | preview | 无 —— 零 libc 档 |

`verified` 意味着该行的镜像被构建**并被运行**过。`preview` 意味着它构建得出、
也被观察到能运行,但尚未纳入引擎自己的模拟器作业。

⚠️ 第三行的 C 库列为空,这是声明而非遗漏:包索引里不存在 aarch64 的 picolibc
构建,而这一行的第一个消费者 —— 机器机制层 `openarch` —— 一个 C 库符号都不引用。
空列在这里的含义与清单里 `[target.<triple>].sysroot = ""` 完全一致,因此面向它的
工程无需声明即处在零 libc 档。想要 C 库的工程自行声明一个,而那也正是它换用另一份
C 库的做法。

这类目标不需要逐宿主的交叉工具链。clang 与 lld 在构造上就是交叉编译器 ——
一个二进制发射它构建时支持的全部目标 —— 因此目标表在每个宿主上都钉
`llvm@22.1.8`,任何能安装 LLVM 载荷的机器都能为这三个中的任何一个产出镜像。

裸机构建需要的三样东西并不是 ISA 的属性,mcpp 也不试图推导它们:选哪个启动对象
与哪些库、哪份链接脚本描述这台机器的内存、以及如何执行产出的镜像。这三样随
**板级支持包**一起提供,而板级支持包是一个普通依赖。由此得到的结果是:换板子是
一次依赖变更,而不是一次构建系统变更。

## 从零到一个可运行的镜像

两条命令即可产出一个会启动的镜像,其中链接脚本、加载地址与模拟器命令行都不必手写:

```bash
mcpp new blinky --template riscv-virt-rt
cd blinky
mcpp run
```

实测输出:

```
   Resolving toolchain
    Resolved llvm@22.1.8 → riscv64-none-elf → @mcpp/registry/data/xpkgs/xim-x-llvm/22.1.8/bin/clang++
    Resolved host toolchain for build.mcpp: clang 22.1.8 (x86_64-unknown-linux-gnu)
  build.mcpp compiling
  build.mcpp running
    Inferred sources [src/**/*.{cppm,cpp,cc,c,S,s,asm}]
    Inferred target blinky (bin from src/main.cpp)
   Compiling blinky v0.1.0 (.)
      Cached riscv-virt-rt v0.3.0 (1 unit)
    Finished dev [unoptimized + debuginfo] in 0.05s
        Size blinky  text 8572  data 80  bss 5668  total 14320
     Running `…/xim-x-qemu-riscv/9.2.4-1/bin/qemu-system-riscv64 … target/riscv64-none-elf/…/bin/blinky`

hello from blinky
float 3.1416
heap ok
```

`Size` 行在每次 freestanding 链接后打印。容量是裸机目标上的支配性约束,而链接完成
的那一刻引擎已经知道这个数;不打印就意味着每个工程都要另跑一次 `size`。在宿主目标
上以及工具缺失时它保持静默 —— 一行信息性输出没有让构建失败的资格。

### 生成的工程

整份清单只有四处声明:

```toml
[package]
name    = "blinky"
version = "0.1.0"

[build]
target = "riscv64-none-elf"

[dependencies]
riscv-virt-rt = "0.3.0"
```

其中没有 `[target.*]` 段,没有链接脚本路径,没有加载地址,没有 `-nostdlib`,
没有 `-march`/`-mabi`/`-mcmodel`,没有 crt0,没有 C 库名字,也没有模拟器命令行。
ISA 参数来自引擎的目标表;其余来自 `[dependencies]` 中列出的板级支持包。

生成的 `src/main.cpp` 是一个普通的 `main`:

```cpp
import mcpplibs.riscv_virt_rt;

extern "C" int main() {
    board::println("hello from blinky");
    board::printf("float %.4f\n", 3.14159);
    void* p = board::alloc(64);
    board::println(p ? "heap ok" : "heap FAILED");
    board::release(p);
    return p ? 0 : 1;
}
```

不需要 `_start`,也不需要汇编入口,因为板级支持包选取了 picolibc 的 semihosting
`crt0` —— C 运行时在 `main` 之前就已就绪,返回值经 semihosting 回到宿主。只有完全
没有 C 库的板子才需要显式入口,做法是让 `main` 指向携带 `_start` 的那份源文件。

## freestanding 目标带来的变化

| 方面 | 在 freestanding 目标上的行为 |
|---|---|
| 链接行 | 从零构造而非在原有基础上追加:`-nostdlib -nostartfiles -static`,没有 crt 文件,没有动态链接器,没有 C++ 运行时。在宿主链接行后追加 `-nostdlib` 将依赖驱动以正确顺序丢弃先前的 flag。 |
| 链接器选择 | `ld.lld` 按**绝对路径**寻址,由驱动自身所在目录推导。`-fuse-ld=lld` 按名字解析,在任何 binutils 位于 `PATH` 更靠前位置的机器上都会找到 GNU ld,随后以 `unrecognised emulation mode: elf64lriscv` 失败。 |
| ISA 参数 | `-march`、`-mabi` 与 `-mcmodel` 来自 `src/freestanding/target.cppm` 中每个目标一行的表,因此仅凭 `--target <triple>` 就足以产出正确的目标文件。 |
| C 库 | 属于**目标**,由 mcpp 从目标自己的表行解析,与解析编译器的方式完全一致。裸机工程不声明 libc,正如宿主工程不声明 glibc。引擎把 sysroot 的库目录放入链接搜索路径,板级支持包因此可以按裸名从中选取(`-lc`、`-lcrt0-semihost`)。 |
| 异常与 RTTI | 在图中每个翻译单元上都关闭,包括依赖的翻译单元。没有 unwinder 也没有 `libc++abi`,因此任何东西都无法抛出;仅 `std::optional::value()` 一处就会引用 `__cxa_throw` 以及另外三个未定义符号。该设定属于目标而非工程的 `cxxflags`,因为 BMI 会记录它,而带异常编出的依赖无法被不带异常的单元导入。 |
| `import std` | 不可用,并且在配置期以诊断拒绝,而不是在链接期失败。 |
| 入口点 | 只要有东西提供 `crt0`,`int main()` 就可用。板级支持包通常提供它。 |
| 默认链接方式 | 静态,而且这不是偏好:没有加载器,因此没有别的选项。 |

一个完全没有依赖的工程仍然构建得出来,这正是「仅凭 ISA 表行就已足够」的证据:

```
        Size norunner  text 12  data 0  bss 0  total 12
```

## 分层:引擎、目标与板级支持包

职责划分只有一句话:**位置是目标的事实,选择是板级的事实。**

| 层 | 拥有的内容 | 例子 |
|---|---|---|
| 引擎 | ISA 档位、freestanding 链接行、产物集、以及「产物如何执行」的单一读取点 | `-march=rv64gc -mabi=lp64d -mcmodel=medany -ffreestanding` |
| 目标 | 用哪个编译器与哪份 C 库,两者都从目标的表行解析并按需安装 | `pin = llvm@22.1.8`、`sysroot = xim:picolibc-riscv@1.8.12` |
| 板级支持包 | 选哪个启动对象与哪些库、哪份链接脚本、哪条模拟器命令行 | `-lcrt0-semihost`、`picolibcpp.ld`、`qemu-system-riscv64 -machine virt …` |

中间那一行正是让包不必指名 C 库的原因。两个生态包早先都声明过
`[xlings] deps = ["xim:picolibc-riscv@1.8.12"]`,这把包绑死在一份 libc、一种架构
与一种编译器实现上。该声明已不再需要,目标的 sysroot 列取代了它。

同一 ISA 上的第二块板子,是把最后一行里的三个取值换掉。它不需要引擎作任何改动。

## 示例

本节每一段实录都在 `x86_64-linux-gnu` 上用 mcpp 2026.8.20.1 实测得到,
参见[验证范围](#验证范围)。

### 切换 ISA 宽度

同一份源码与同一个板级支持包同时服务两种宽度:

```bash
mcpp run --target riscv32-none-elf
```

```
        Size blinky  text 10412  data 48  bss 5400  total 15860
hello from blinky
float 3.1416
heap ok
```

工程的源码与板级支持包都未改动。板级支持包从 `MCPP_TARGET_ARCH` 选取档位,而 ISA
参数来自引擎的表 —— 那是数据而不是代码,再支持一种宽度是加一行。

### freestanding 标准库子集

`import std` 是覆盖整个库的一个模块,线程、文件系统与 iostreams 全在其中,因此在
没有操作系统的前提下不存在它的子集可编。承载其中不需要 OS 的那部分的是一个普通依赖:

```toml
[dependencies]
riscv-virt-rt    = "0.3.0"
std-freestanding = "0.2.0"
```

```cpp
import mcpplibs.riscv_virt_rt;
import mcpplibs.std.freestanding;      // not `import std;`

struct Task { int prio; const char* name; };

extern "C" int main() {
    std::array<Task, 4> tasks{{ {3,"c"}, {1,"a"}, {4,"d"}, {2,"b"} }};
    std::ranges::sort(tasks, {}, &Task::prio);
    for (const auto& t : tasks) board::printf("%s", t.name);
    board::println("");

    std::optional<int> o = 41;
    std::atomic<int>   a{0};
    a.fetch_add(o.value() + 1);
    board::printf("atomic %d\n", a.load());

    std::span<Task>  s{tasks};
    std::string_view sv{"ok"};
    board::printf("span %zu %s\n", s.size(), sv.data());
    return 0;
}
```

实测输出与体积:

```
        Size blinky  text 19564  data 72  bss 5632  total 25268

abcd
atomic 42
span 4 ok
```

该子集覆盖 LLVM 22.1.8 载荷所带 110 个 `std/*.inc` 头中的 103 个 —— 2026-08-20 在
两侧目录分别计数得到 —— 且它由机械挑选生成,而不是手写的导出表。被略去的 7 个,
按包一侧的说明在宿主 `x86_64` 上同样失败;该说明未在此重新实测。可用的实体包括
`array`、`span`、`optional`、
`expected`、`atomic`、`string_view`、`ranges`、`algorithm`、`bit`、`charconv`、
`concepts`、`type_traits`、`tuple`、`utility` 以及协程。

子集排除掉的部分在编译期被排除,而不是运行期:

```cpp
std::mutex m;
```

```
error: no type named 'mutex' in namespace 'std'
```

### 子集中会分配的那半边

子集不改变什么能编。它的全部头文件都是无条件包含的,`std::vector` 今天就编得过;
失败发生在**链接**,因为 freestanding 目标没有编译版 `libc++`,也就没有
`operator new`:

```
ld.lld: error: undefined symbol: operator new(unsigned long)
>>> referenced by allocate.h:58
```

| 不需要分配器 | 需要分配器 |
|---|---|
| `array` `span` `optional` `expected` `atomic` `string_view` `ranges` `algorithm` `bit` `charconv` `tuple` | `vector` `string` `deque` `list` `map` `set` `unordered_*` `function` `any` `make_unique`,以及默认的协程帧 |

分配器随一个 feature 到来:

```toml
[dependencies]
riscv-virt-rt    = "0.4.0"
std-freestanding = { version = "0.3.0", features = ["alloc-libc"] }
```

```
        Size blinky  text 13764  data 80  bss 5668  total 19512

vector 5 last=16
```

`alloc-libc` 转发到目标的 C 库,目标有 C 库时它是更短的路径。`alloc-kal` 转发到
openkal,适用于同一份源码还要为「环境不是 C 库」的目标构建的工程;裸机上 openkal
后端由板级支持包提供,因为控制台与堆区都是板级事实:

```toml
riscv-virt-rt    = { version = "0.4.0", features = ["openkal"] }
std-freestanding = { version = "0.3.0", features = ["alloc-kal"] }
```

`operator new` 是全程序单例,因此实现是独立的包,而选择属于程序。feature 以能力的
形式声明这条要求,实现方提供该能力,解析器绑定恰好一个。两种失败因此都在图解析时
报告,点名的是包而不是 mangled 符号:

```
error: no package provides capability 'freestanding-allocator' required by 'std-freestanding'

error: capability 'freestanding-allocator' has multiple providers in the graph:
       [std-freestanding-alloc-kal, std-freestanding-alloc-libc]
```

### 没有 C 库的目标

`[target.<triple>].sysroot` 覆盖目标表所绑定的 C 库,与 `toolchain` 覆盖编译器 pin
同轴。空字符串表示完全不要 C 库:

```toml
[target.riscv64-none-elf]
sysroot = ""
```

有了这一行,C 头文件离开编译行,C 库离开链接行。`#include <stdio.h>` 不再解析,
镜像里只剩工程与其依赖放进去的内容。实测:一个自带入口点与链接脚本的自包含镜像
链接后为 **108 字节**,并能启动。

键缺席与键为空是两个不同的答案。缺席继承目标表的 C 库,存在且为空则拒绝它。内核与
bootloader 要的是后者,而

```bash
mcpp new mykernel --template riscv-virt-rt:nolibc
```

生成的工程已处于该安排 —— 一个入口点、一份内存映射、一个设备,实测 369 字节。

freestanding 翻译单元仍会用到的 C 函数中,有四个是义务而非便利:`memcpy`、
`memmove`、`memset` 与 `memcmp` 必须存在,因为编译器把结构体赋值与数组初始化下降到
它们之上。`std-freestanding-nolibc` 提供这四个与 `strlen`。

子集与这一档是**组合**关系而非互斥。`std-freestanding` 带 `features = ["nolibc"]`
时在完全没有 C 库的情况下编译,实测其 **103 个头中的 94 个**可用。障碍从来不是子集
需要一个 C 库:libc++ 为 C 头文件提供了包装头 —— `string.h` 及其同类 —— 它们通过
`#include_next` 续到真正的头去取 `size_t`、`mbstate_t`、`time_t` 与 `EOF`。没有
C 库时这条链无处可续,包装头死在缺**类型**而不是缺头文件上,这正是从报错看不出成因的
原因。四个很小的头把链续上,而该 feature 负责把它们引进来。

板级支持包同样可以服务这一档,理由与"为什么要有板级包"是同一条:一台机器的 UART 在哪、
RAM 从哪开始、哪个模拟器启动它,没有一条是 C 库事实:

```toml
[dependencies]
riscv-virt-rt = { version = "0.5.0", features = ["nolibc"] }
```

⚠️ `std-freestanding-nolibc` 正是该 feature 解析到的包,而**直接**把它与 C 库并用时
是**静默**失败而非响亮失败。C 库以归档形式
发布,归档成员只在符号仍未定义时才被拉入;而依赖包的目标文件无条件进入链接。于是该包
先定义了 `memcpy`,C 库的成员从不被拉入,构建**成功** —— 程序拿到的是逐字节实现而不是
C 库经过优化的那份,且没有任何提示。实测(picolibc 在场):冷构建链接通过,`nm` 只找到
一处定义。

### 在目标上运行测试

`mcpp test` 为每个 `tests/*.cpp` 构建一个独立镜像,在板级支持包提供的模拟器里运行,
并以退出码为判据。semihosting 把固件 `main` 的返回值传递到模拟器的退出码,因此其
心智模型与宿主上的测试运行完全一致。

```bash
mcpp test
```

```
   Compiling boots (test)
     Running bin/boots
boots: console
boots ... ok (0.02s)

 test result ok. 1 passed; 0 failed; finished in 0.58s (build 0.05s + run 0.02s)
```

失败的用例会被点名,命令的退出状态非零:

```
deliberate_fail ... FAIL (exit 1, 0.02s)
about to fail
boots ... ok (0.02s)
boots: console

error: test result: FAILED. 1 passed; 1 failed; finished in 0.24s (build 0.04s + run 0.02s)

failures:
    deliberate_fail
```

### 烧录所需的产物集

freestanding 链接产出三个文件而不是一个:

```
target/riscv64-none-elf/<fingerprint>/bin/blinky        91640 bytes   ELF, for a debugger or `qemu -kernel`
target/riscv64-none-elf/<fingerprint>/bin/blinky.bin     8664 bytes   flat image, what a flasher accepts
target/riscv64-none-elf/<fingerprint>/bin/blinky.map    253369 bytes  link map
```

裸二进制由一条 `objcopy -O binary` 边产出,该工具与驱动取自同一份载荷。映射文件是
链接边的隐式输出而非一个孤立的 `-Wl,-Map=` flag,因此删掉它会被重新生成;它是唯一
能回答「某个段为何在此处」以及「某段内容为何被或未被从归档中拉入」的产物。

### 覆盖板级支持包提供的 runner

板级支持包通常提供 runner。工程可以覆盖它,这是通常的优先级 —— 工程作者写下的
胜过依赖提供的:

```toml
[target.riscv64-none-elf]
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
          "-no-reboot", "-bios", "default",
          "-s", "-S",                    # wait for a debugger on the first instruction
          "-kernel"]
```

覆盖生效时会被报告,而不是静默应用:

```
        note [target.riscv64-none-elf].runner overrides the runner a dependency supplied
```

产物路径追加在模板末尾,或者在模板包含 `{}` 时替换该记号。追加是常见形态,因为
`-kernel <image>` 处在命令行末尾。

mcpp 不附带任何默认 runner。用哪个模拟器、哪种机器模型、哪种固件模式都是板级事实
—— 同一 ISA 上的两块板子需要不同的 argv,OpenSBI 启动用 `-bios default`,picolibc
镜像用 `-bios none -semihosting` —— 因此一个猜中其中之一的引擎,会被另一块板子反复
对抗。

## 诊断

### freestanding 目标上的 `import std`

```
error: `import std;` is not available on 'riscv64-none-elf' — a freestanding target has no hosted standard library.
       `std` is one module over the entire library (threads, filesystem, iostreams
       included), so there is no subset of it to build without an OS underneath.
       Use the freestanding subset instead — an ordinary dependency carrying
       the parts of the library that need no OS (array, span, optional, atomic,
       string_view, ranges, expected, charconv, coroutines):

           [dependencies]
           std-freestanding = "0.2.0"

       then `import mcpplibs.std.freestanding;` in place of `import std;`.
       The target's C library itself comes from the BOARD package (riscv-virt-rt
       exports `mcpplibs.riscv_virt_rt`).
```

该消息在配置期发出。若改为报告缺少 `std` 模块源,会把读者引向排查一个损坏的载荷,
而工具链其实什么都不缺。

### 缺少 runner

freestanding 产物无法在构建机器上执行:ISA 不对,没有加载器,而且它预期独占整个
地址空间。当没有配置 runner 时,`mcpp run` 会报告「构建成功」与「无法执行」之间的
这段落差:

```
error: no runner is configured for 'riscv64-none-elf' — a freestanding artifact cannot execute on this machine.
       Declare how to run it:

           [target.riscv64-none-elf]
           runner = ["qemu-system-riscv64", "-machine", "virt",
                     "-nographic", "-no-reboot", "-bios", "default", "-kernel"]

       The artifact path is appended, or substituted for `{}` if the template contains it.
       A board-support package normally supplies this so you do not have to.
```

## 编写板级支持包

板级支持包是一个普通的 mcpp 包。它在 `[xlings] deps` 下声明所需的模拟器,为消费者
导出一个 C++ 模块,并从 `build.mcpp` 发出它的板级事实。

### 板级支持包发出的指令

| 指令 | 作用 |
|---|---|
| `mcpp:link-lib=<name>` | 向消费者的链接行加入 `-l<name>`。裸名即可:目标 sysroot 的库目录已在搜索路径上。 |
| `mcpp:link-search=<dir>` | 加入 `-L<dir>`,用于包自身携带的库。 |
| `mcpp:link-script=<abs path>` | 加入 `-T <path>`。相对路径按包根解析,因此属于目标 C 库的脚本必须按绝对路径指名。 |
| `mcpp:runner=<token>` | 向运行模板追加一个 argv 记号。argv 是有序列表,模板由重复构建 —— 一个记号一行指令。 |
| `mcpp:include-dir=<dir>` | 加入一个**仅对本包生效**的头文件目录。 |

两个「向引擎提问」的接口提供了板级支持包不应硬编码的路径:`mcpp::sysroot_dir()`
返回目标 C 库的根,`mcpp::xpkg_dir(ns, name)` 返回一个已安装载荷的目录。
`mcpp::target_arch()` 报告正在构建的架构。

### 指令作用域与到达消费者的内容

作用域是刻意不对称的,而这份不对称正是指令层完全不需要 sysroot 概念的原因:

| 作用域 | 指令 | 到达消费者 |
|---|---|---|
| `LinkGlobal` | `link-lib`、`link-search`、`link-script` | 是 |
| `RunGlobal` | `runner` | 是 |
| `PackagePrivate` | `include-dir`、`include-dir-after` | 否 |

因此板级支持包以私有方式包含目标的 C 头文件,并把希望被看见的部分导出为一个 C++
模块。消费者导入该模块,而不继承一条头文件搜索路径。

### 一份完整的 build.mcpp

下面是 QEMU RISC-V `virt` 机器的板级支持包,去掉注释后的全文。这就是该板子构建
逻辑的全部:

```cpp
import mcpp;
import std;

int main() {
    const std::string arch = mcpp::target_arch() ? mcpp::target_arch() : "";
    const bool rv32 = (arch == "riscv32");

    // Selected out of the target's C library, by bare name.
    mcpp::link_lib("crt0-semihost");
    mcpp::link_lib("c");
    mcpp::link_lib("semihost");
    mcpp::link_lib(std::format("clang_rt.builtins-{}",
                               rv32 ? "riscv32" : "riscv64").c_str());

    // This machine's memory layout, asked for rather than declared.
    if (const char* sysroot = mcpp::sysroot_dir(); sysroot && *sysroot)
        mcpp::link_script(std::format("{}/lib/{}/picolibcpp.ld", sysroot,
                                      rv32 ? "rv32imac/ilp32" : "rv64gc/lp64d").c_str());

    // How to run an image. The emulator is named by absolute path, because a
    // bare name resolves through PATH to a shim that dispatches against its
    // own owner home rather than the home this build uses.
    if (const char* qemu = mcpp::xpkg_dir("xim", "qemu-riscv"); qemu && *qemu) {
        mcpp::runner(std::format("{}/bin/qemu-system-{}", qemu,
                                 rv32 ? "riscv32" : "riscv64").c_str());
        for (auto a : {"-machine", "virt", "-nographic", "-no-reboot",
                       "-semihosting", "-bios", "none", "-kernel"})
            mcpp::runner(a);
    }

    mcpp::rerun_if_env_changed("MCPP_TARGET_ARCH");
    return 0;
}
```

该包的清单只声明模拟器,别无其他:

```toml
[xlings]
deps = ["xim:qemu-riscv@9.2.4-1"]
```

在这块板子上链接 `clang_rt.builtins` 不是可选项。picolibc 通过 ryu 格式化浮点值,
而 ryu 需要 128 位移位,rv64 没有对应指令;缺少 builtins 时链接会在 `__ashlti3`
与 `__lshrti3` 上失败。一个「64 位除法是否可用」的检查到不了这条判据,因为 rv64gc
有硬件 `divu`。

包一侧无法探测出当前 mcpp 是否比它所面向的版本更旧:
`if constexpr (requires { mcpp::runner("x"); })` 作用在未知的限定名上是硬错误而不是
`false`,语言在此不提供特性探测。引擎为此作了补偿:当 `build.mcpp` 编译失败且错误
指出某个名字不是 `mcpp` 的成员时,追加一条升级提示。

## 验证范围

本文的命令与输出于 2026-08-20 实测,环境如下:

| 组件 | 版本 |
|---|---|
| mcpp | 2026.8.20.1,由本仓库构建 |
| 宿主 | `x86_64-linux-gnu` |
| 工具链 | `xim:llvm` 22.1.8 |
| 目标 C 库 | `xim:picolibc-riscv` 1.8.12 |
| 模拟器 | `xim:qemu-riscv` 9.2.4-1 |
| 板级支持包 | `mcpplibs:riscv-virt-rt` 0.3.0 |
| 标准库子集 | `mcpplibs:std-freestanding` 0.2.0 |

体积随 mcpp 版本变化:同一个工程在 2026.8.19.4 下实测为 `text 8844`,在 2026.8.20.1
下为 `text 8572`。因此上文的数字表示量级,而不是固定值。

裸机链路只在 Linux 上有持续验证。引擎侧 CI 有一个 `baremetal` job,覆盖四个端到端
脚本;两个生态包在各自仓库中以 QEMU 真跑 RISC-V 64 与 32 —— 全部在 `ubuntu-24.04`
上。macOS 与 Windows 宿主预期可用,依据是载荷为交叉编译器且 `xim:qemu-riscv` 发布了
五个宿主目标的资产,但该预期**没有**测试覆盖。

## 当前边界

| 边界 | 观察到的行为 |
|---|---|
| `std::format`、内建标量类型上的 `std::sort`、以及完整的 `std::string` | 在**链接**期失败并点名未定义符号。libc++ 把这些实体放在编译版库中 —— 标量 `__sort` 的实例化是 `extern template`,没有可用于关闭它们的宏 —— 因此需要为目标编出的 `libc++.a`。该载荷尚未发布。 |
| 异常与 RTTI | 在整张图上关闭。`try`/`catch` 在编译期即不可用。一块随包提供目标版 `libc++abi` 与 unwinder 的板子有重新开启它们的正当理由;那也正是这一项应当成为一个清单键的时刻。 |
| 板子覆盖面 | 只有一个板级家族。`riscv32-none-elf` 证明的是 ISA 表为数据,而不是已移植第二台机器。ARM Cortex-M 尚未尝试。 |
| 替换 C 库 | 自 2026.8.20.2 起可经 `[target.<triple>].sysroot` 表达,而**仅空值一侧经过验证**(零 libc 档)。指向另一份 C 库同样被接受并经同一通道安装,但生态中没有第二份裸机 C 库,该路径未经测试。 |
| `win32-arm64` 上的 `qemu-riscv` | 上游包未为该宿主发布资产,因此在其上安装会失败。该失败是正确的而非静默的,但该宿主无法运行裸机镜像。 |
| 生态侧 CI 广度 | 两个生态包各自的 CI 只跑 `ubuntu-24.04`。mcpp-index 的 `tests/examples/` workspace 成员在三个平台上无条件运行且没有能力门,因此需要模拟器与目标 sysroot 的包无法加入其中。这是一个已知的覆盖缺口。 |
