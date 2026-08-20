# 裸机生态的四项未完成事项:方案

2026-08-21。对象是 2026-08-20 那一批实施之后仍然敞开的四件事,记录于
[`2026-08-20-freestanding-ecosystem-positioning.md`](2026-08-20-freestanding-ecosystem-positioning.md)
第 12 节。每一项都已定位到具体位置,本文给出修法、判据与代价。

四项之间没有依赖关系,可以并行,也可以只做其中任意一项。它们的价值差别很
大,第 5 节给出排序与理由。

---

## 0. 一处必须先纠正的记录

前一份文档与若干提交信息里写着「Windows 的 LLVM 载荷不带 libc++ 头,已由
CI 测量」。**该测量不成立**,纠正如下。

std-freestanding 的 Windows 作业里那一步打印的是:

```
payload: /c/Users/runneradmin/.mcpp/registry/data/xpkgs/xim-x-llvm/*/
  (no include/)
  include/c++ : absent
  share/libc++: absent
```

⚠️ 第一行的 glob **没有展开**。那说明该路径下不存在任何目录,也就是说这一步
检查的位置本身就是错的——Windows 上 mcpp 的 home 与我在脚本里拼的
`$HOME/.mcpp/registry` 不是同一处。它测到的是「我猜的路径不对」,而不是
「载荷里没有那些头」。

**已经成立的事实只有一条**:该行的构建死于
`src/std_freestanding.cppm:19:10: fatal error: 'algorithm' file not found`。
这说明 clang 被解析到了,而 libc++ 的头不在 mcpp 交给它的 include 路径上。
至于载荷里究竟有没有那些文件,以及 mcpp 指向了哪里,尚未被观察过。

⇒ 第 2 节的第一步是补上这个测量,而不是直接动载荷。

---

## 1. `mcpp test` 与 `mcpp build` 对 feature 源的判断不一致

> **已修复(mcpp 2026.8.21.1)。** ⚠️ 而修法既不是路线 A 也不是路线 B —— 是第三次
> 尝试的判据,它当初被以一个**错误的理由**放弃了。见 1.3.1。

### 1.1 现象与位置

同一个包、同一份清单,两条命令编译的源文件集合不同:

| 命令 | 未激活 feature 的源 |
|---|---|
| `mcpp build` | 不编译(正确) |
| `mcpp test` | **编译** |

实测于 `mcpplibs/riscv-virt-rt`:`src/kal/**` 只由 `openkal` feature 命名,
`mcpp build --target riscv64-none-elf` 正确略过它,而
`mcpp test --target riscv64-none-elf` 编译它并死于
`'openkal/abort.h' file not found`——那个头由该 feature 的 `[feature-deps]`
带来,feature 没激活自然不在。同一缺陷经 path 依赖传到了
`mcpplibs/std-freestanding`。

位置在 `src/build/prepare.cppm`,feature glob 的处理分两半:

```cpp
if (!bc.featureSources.empty()) {
    if (!includeDevDeps) {          // ← DROP 只在非 test 模式跑
        ...把未激活 feature 的 glob 关掉,并追加 `!` 排除...
    }
    ...ADD 在两种模式下都跑...
}
```

`includeDevDeps` 表示的是「这次构建包含 dev 依赖」,也就是 test 模式。

### 1.2 为什么那个条件当初是对的

引擎里的注释写明了:gtest 把 `gtest_main.cc` 同时列在 `main` feature 与
base `sources` 里,而 dev 依赖那条轨道的「逐测试 main 探测」需要**看得见**
它才能逐个测试地把它剪掉。`!` 排除会让它消失。

所以这不是有人忘了,而是一个真实约束被用一个过宽的条件表达了。

### 1.3 ⚠️ 三次修法,全部被测量证伪

| 尝试 | 结果 |
|---|---|
| DROP 与 `!` 排除在两种模式下都跑 | **弄坏 gtest**:mcpp 自己的单测链接失败,`ld returned 1 exit status` |
| test 模式只删 glob 字符串、不加 `!` 排除 | 无效:`src/kal/**` 从来不在 `bc.sources` 里,删字符串什么都没删,文件仍由 base glob `src/**` 匹配 |
| 按「该 glob 是否也出现在 base `sources`」判别 | 记录为无效,理由是「gtest 的 base 是一条能匹配到那个文件的 glob,不是同一个字符串」。**这条记录是错的** —— 见 1.3.1 |

三次都已撤销。**不留半个修复**:把一个只在部分情形下正确的门放进已发布的
引擎,比一个有文档的缺陷更糟。

### 1.3.1 ⚠️ 第三次尝试的判据是对的,被放弃的理由是错的

去读索引真正携带的描述符 `pkgs/c/compat.gtest.lua`:

```lua
sources  = { "*/googletest/src/gtest-all.cc",
             "*/googletest/src/gtest_main.cc" },   -- 第 71–73 行
features = { ["main"] = { sources = { "*/googletest/src/gtest_main.cc" } } },  -- 第 90 行
```

两处**逐字节相同**。所以「字符串相等判别不到」这句话与事实不符,而当初把它写进
文档时没有去读那个文件 —— 判据被一个凭印象写下的理由否掉了。

真正让第三次尝试失效的是**判据被施加的时机**:membership 是拿 `bc.sources` 去比
的,而 `drop()` 已经先一步把那条 glob 从里面删掉了,于是这个测试只可能为假。

⭐ **修法**:在 `drop()` 之前把 base 的 glob 集合快照下来,并且只在 test 模式使用
它 ——

* glob 同时出现在 base `sources` 与 feature 里 ⇒ 这是一个**门**(gtest 那一族),
  test 模式下保持可见,逐测试 main 探测仍然剪得掉它;
* glob 只出现在 feature 里 ⇒ 这是一个**提供者**(riscv-virt-rt 那一族),
  test 模式下也要 `!` 排除。

⚠️ 而 `!` 排除才是整个机制,删 glob 字符串不是:`src/kal/**` 从来就不在
`bc.sources` 里(该包根本没声明 `sources`),它的文件由推导出的 `src/**` 匹配。

### 1.4 正确的修法

三次失败指向同一个结论:**判据不是「这条 glob 字符串在不在 base 里」,而是
「base 的任何一条 glob 是否匹配这条 feature glob 所匹配的文件」**。前者是
字符串比较,后者需要 glob 展开。

两条可行路线:

**路线 A(引擎内展开)。** 在 feature 门这一处把两侧都展开成文件集合,按
文件而不是按 glob 决定:

```
featureFiles = expand(feature glob)
baseFiles    = expand(base globs, 不含 feature glob)
对每个 f ∈ featureFiles:
    若 feature 未激活:
        若 f ∈ baseFiles → 保留(gtest 那一族:包无条件提供、feature 只是门)
        否则             → 排除(riscv-virt-rt 那一族:包只经 feature 提供)
```

代价:这一处目前只做字符串操作,引入文件系统展开会把它与扫描阶段耦合起来,
并且要考虑展开的缓存与顺序稳定性。

**路线 B(让包表达意图)。** 在 manifest 里区分两种 feature 源:

```toml
[features]
main    = { sources = ["src/gtest_main.cc"], gate-only = true }   # 包无条件提供,feature 只是门
openkal = { sources = ["src/kal/**"] }                             # 只经 feature 提供
```

代价:新增一个清单键,而这正是本次实施一直在避免的事(见定位文档第 7 节
「优雅」那一行)。但它把一个引擎猜不出来的区别交给知道答案的人来写。

⭐ **建议先做路线 A**,因为路线 B 要求每个既有包回头补一个键,而在补之前
行为不变——也就是说 B 不修复任何现存的包,A 修复。若 A 的展开代价被证明
过高,再退回 B。

### 1.5 判据

必须同时满足,**任缺其一即视为未修复**:

1. ✅ `mcpplibs/riscv-virt-rt` 不带 feature 时 `mcpp test` 不再编译
   `src/kal/**`,两条命令都以 0 退出(此前 test 死于
   `'openkal/abort.h' file not found`);
2. ✅ mcpp 自己的 92 个单测仍然链接并通过(gtest 那一族);
3. ✅ `tests/e2e/138_feature_sources_gate_vs_provider.sh`,两个最小包分别表达
   上面两族,不依赖任何生态包。

⚠️ 第 3 条是重点。这个缺陷之所以活到今天,是因为**没有任何测试同时覆盖两族**;
只修不测,下一次同样的改动会再次在两者之间摇摆。

⭐ 那条 e2e 已按「回归测试必须先失败」验证过:拿修复前的引擎跑它,它以正是本节
开头那句症状失败;拿修复后的引擎跑它,它通过。

### 1.6 副作用

修好之后,`mcpplibs/riscv-virt-rt` CI 里那句
`mcpp test --features openkal` 的 `--features` 可以去掉,但**不建议去掉**:
一个包测自己发布的 feature 本来就是对的,与这个缺陷无关。

---

## 2. Windows 的 LLVM 载荷与 libc++ 头

### 2.1 先补测量

在动任何东西之前,先回答两个尚未被观察过的问题:

1. Windows 的 LLVM 载荷里到底有没有 `include/c++/v1` 与 `share/libc++/v1`?
2. mcpp 在 Windows 上把 `toolchain_dir()` 指向了哪里?

判据不能再用猜出来的 home 路径。正确的问法是问 mcpp 自己:

```yaml
- name: What this payload carries, asked rather than guessed
  run: |
    mcpp self env                      # 打印 mcpp 的 home 与工具链路径
    tc="$(mcpp self env | sed -n 's/^toolchain: *//p')"
    ls "$tc/include/c++/v1" 2>/dev/null | head -5 || echo "include/c++/v1 absent"
    ls "$tc/share/libc++/v1" 2>/dev/null | head -5 || echo "share/libc++/v1 absent"
```

⚠️ 若 `mcpp self env` 不输出工具链目录,则先补这一条输出——一个构建程序能
问到的东西,诊断也应当能问到。

### 2.2 ⚠️ 一处需要收回的论证

本文第一版在这里写道:独立成包会引入「索引今天没有表达机制」的跨包版本约束,
所以应当把头加进现有载荷。

**那个前提是错的。** 一条带版本要求的依赖边**就是**那个机制,而
`std-freestanding` 自己已经在用它——`[feature-deps.nolibc]` 里写着
`std-freestanding-nolibc = "^0.2.0"`。我把「描述符里的注释」与「依赖边」混成
了一件事,而这两者恰恰是本次实施反复区分过的东西:诊断里写死版本字面量那次,
教训正是注释维持不住跨仓库不变量,而依赖边可以。

收回该论证之后,取舍要重新做,而重新做的结果与原结论相反。

### 2.3 重新提问:freestanding 的支持应当与标准库无关

标准把 freestanding 子集定义成一张表,**任何**符合的实现都提供它。因此一个
名为「C++ 标准库的 freestanding 子集」的包,其**接口**理应与具体实现无关,
而今天它不是:它合成 libc++ 的 `__config_site`、读 libc++ 的头。

把这件事拆成三层,归属就清楚了:

| 层 | 内容 | 与实现有关吗 |
|---|---|---|
| **接口** | 包含哪些标准头、导出哪些标准名 | **无关**,标准定义 |
| **配置** | 如何让一个实现进入 freestanding 状态 | **有关,但很小**:libc++ 合成 `__config_site`;libstdc++ 一个 `-D_GLIBCXX_HOSTED=0`(实测 29/34);MSVC STL 的头建立在 Windows CRT 上,对非 Windows 目标不成立 |
| **头从哪来** | 那些文件本身 | **与宿主无关**,但今天从宿主的编译器载荷里借 |

⭐ **第三层才是 Windows 问题的所在,而它与「用哪个标准库」无关。**

在 Windows 宿主上交叉编译到 `riscv64-none-elf` 时,宿主的标准库是什么不重要:
MSVC STL 对 riscv 裸机不可用,不是因为它是 MSVC 的,而是因为它下面需要
Windows CRT 而那个目标上没有。**能用的只能是头与宿主无关的实现**——libc++ 或
libstdc++——而两者在那份载荷里都不在。

### 2.4 ⭐ 结论:目标的 C++ 标准库应当像目标的 C 库一样解析

本次实施已经把同一个教训学过一遍。板级包曾经硬编码 `picolibc`,修法不是让
板级包更聪明,而是把 C 库变成**目标的属性**:

```toml
[target.riscv64-none-elf]
sysroot = "xim:picolibc-riscv@1.8.12"     # 目标的 C 库,与宿主无关
```

`std-freestanding` 今天从 `mcpp::toolchain_dir()` 借 C++ 标准库的头,是**同一类
错误**:它把「目标需要一份 C++ 标准库」写成了「宿主的编译器恰好带着一份」。

⇒ 修法与当年一致:**让它成为可解析的东西,而不是借来的东西。** 三步,每步
可独立落地:

**步骤 1:`std-freestanding` 增加「头从哪来」的解析顺序。**

```
1. 工具链载荷带了就用          ← 今天 Linux 与 macOS 的路径,不变
2. 否则用声明的包
```

第 2 条是一条普通依赖边,版本约束由它承载:

```toml
[dependencies]
libcxx-headers = "22.1.8"     # 与产出导出表的那个 libc++ 同版本
```

⚠️ 只有第 1 条落空时才走第 2 条,所以 Linux 与 macOS 的解析路径一字不变——
这是「无感升级」那一栏的要求。

**步骤 2:新建 `xim:libcxx-headers`。**

内容是 `include/c++/v1` 与 `share/libc++`,实测 xz 压缩后 **0.9 MB**。它与
宿主无关,因此**一个包服务全部五个宿主**,而「加进载荷」要给每个缺失的宿主
各加一次。

**步骤 3:实现层的选择成为可声明的。**

接口层不变,配置层按检测到的实现分支:

```
有 include/c++/v1        → libc++    :合成 __config_site
有 bits/c++config.h      → libstdc++ :-D_GLIBCXX_HOSTED=0
两者皆无                  → 具名诊断
```

⚠️ libstdc++ 那条实测只到 29/34(缺的五个是 GCC 16 未实现的 C++26 新增项),
所以它是一条**真实但更窄**的路径,应当照实说明而不是宣称等价。

### 2.5 两条路的对比

| | 加进 Windows 载荷 | 独立成包 |
|---|---|---|
| 体积 | 每个缺失宿主 +0.9 MB | 一个包服务全部宿主 |
| 版本约束 | 天然同版本 | **由依赖边承载**(收回的那条论证以为不行) |
| 谁能改 | ⚠️ 打包脚本**不在我们仓库**,是外部依赖 | 我们自己能做完 |
| 概念 | 「宿主编译器顺带带一份」 | **「目标需要一份」**,与 C 库同构 |
| 对 libstdc++ 路径 | 无帮助 | 同一套机制可以再加一个头包 |

⭐ **改选独立成包。** 决定性的不是体积,而是后两行:它是我们能自己做完的,
并且它把这件事放回了本次实施已经证明正确的那个位置——**目标需要什么,由目标
解析,而不由宿主的编译器顺带提供。**

⚠️ 「加进载荷」仍值得作为**补充**提给上游:一份带 libc++ 的 Windows 载荷会让
第 1 条解析路径直接命中,连依赖边都不需要。但它不该是主路径,因为它把一个
我们能解决的问题变成了一个要等别人的问题。

### 2.6 判据

* 步骤 1 落地后,Linux 与 macOS 的构建**一字不变**;
* Windows 行从「测量缺口」翻转为真实交叉构建,其自退役断言主动失败;
* **两侧判据**:把工具链载荷里的 `include/c++/v1` 藏起来,构建仍应通过(走
  第 2 条);把包也拿掉,应给出具名诊断而不是 `'algorithm' file not found`。

⚠️ 第三条是重点。这个能力若只在 Windows 上被用到,就只有一台机器测过;
「拿走再装回来」是唯一能在任何机器上验证它的判据,与 picolibc 那条安装边的
教训一致。

---

## 3. openarch:三台机器,一个包两个门面,后端由 feature 选择

> **本节已由 0.4.0 落地。** 下面记录做成了什么、以及两处本方案原先判断错了的地方。

### 3.1 已完成(0.4.0)

四个接口 —— contexts、页表项、trap、per-CPU 与屏障 —— 覆盖**三个**指令集:
riscv64、aarch64、x86_64。一份探针源码在三台机器上构建并运行,输出逐字节相同。

⭐ **第三台机器是把门槛变成证据的那一步,而本方案原先低估了它。** §4 把 x86_64
当作「模拟器载荷问题」,并把它排在最后;实际上 riscv64 与 aarch64 都是弱内存序、
定长指令的 load/store RISC 机器,**一个同时适配两者的接口可能是因为它对,也可能
是因为它们像**,而在这两台机器上再怎么测也分不开这两种情况。x86_64 两样都不是:
变长指令;total store order(四条屏障里三条不需要任何指令);中断机制是 256 个门
的表;控制台由 `out` 到达,没有任何指针能命名它。

它挖出了三条,而每一条都是两台 RISC 机器合起来也看不见的:

1. **`MAIR_EL1` 那个决定不再是 aarch64 的例外。** 两台机器时是一比一,「本层拥有
   属性寄存器」还可以被称作 aarch64 的权宜。x86_64 的 `PWT`/`PCD`/`PAT` 三个分散
   的位同样构成 `IA32_PAT` 的索引 —— 现在是二比一,方向反了过来。
   ⚠️ **而且它的规则更严。** 未编程的 `MAIR_EL1` 字段读作最严格的类型,过早的
   aarch64 映射只是慢而正确;`IA32_PAT` 的复位值在索引 1 上是 **write-through**,
   过早的设备映射是被缓存的 —— 写在程序没有选择的时刻到达设备,不触发任何异常。
2. **`pc` 在每台机器上并不指同一件事。** 两台 RISC 机器都报告出错指令的地址;
   x86_64 把异常分为 *fault*(如此)与 *trap*(报告**下一条**的地址),而 `int3`
   ——`instr_len` 正是为跨过它而存在的断点 —— 是 trap。后端做归一化,于是
   `f->pc += f->instr_len` 在三台机器上都恢复到同一处。
3. **接口的一条承诺在这台机器的页表项里无法表达。** riscv 用 `U` 限定 `X`,
   aarch64 有独立的 `PXN`/`UXN`;x86_64 只有一个覆盖全部特权级的 `NX`,该规则改由
   `CR4.SMEP` 提供。

⭐ **0.3.x 的第三条发现仍然成立:`trap_frame` 必须带 `instr_len`。** 实测 rv64gc:

```
cause:3 desc=breakpoint          epc:0x800001f2
cause:2 desc=illegal_instruction epc:0x800001f6   ← 无限重复
```

`0x800001f2` 不是四字节对齐 —— C 扩展是 `rv64gc` 的一部分,汇编器发的是两字节的
`c.ebreak`。aarch64 只有一种指令宽度,永远暴露不了这一条。

### 3.2 目录树:混合式的根,以及它顺带修好的一条

```
openarch/
├── mcpp.toml              [package] openarch  兼  [workspace]
├── src/                   C++ 门面 —— 模块 mcpplibs.openarch 再导出四个
├── tests/                 两个门面必须一致的地方
├── abi/                   契约 —— 只有头文件,不依赖任何东西
│   ├── mcpp.toml          openarch-abi
│   └── include/
│       ├── mcpplibs/openarch.h   C 门面,全部
│       └── openarch/
│           ├── types.h    宽度,写一次、断言一次
│           ├── abi.h      后端要实现的东西
│           └── pte_encode.h   纯编码器,宿主可调用
├── backends/              每个指令集一个包,都 provides "openarch-backend"
│   ├── riscv64/  aarch64/  x86_64/
└── examples/switch/       一份探针源码
```

⚠️ **本方案原先写的是「根是 `[workspace]`,接口在 `spec/` 成员里」,那个形状是错
的,而错在一条它自己没有预见的地方。** portability 作业在仓库根跑
`mcpp build --target riscv64-none-elf`,虚拟 workspace 会**对所有成员扇出** ——
于是 aarch64 汇编被喂给 riscv 汇编器:

```
unrecognized instruction mnemonic, did you mean: sra, srl?
```

混合式的根(同时是 `[package]` 与 `[workspace]`)修好了它:根现在是接口包,构建
它只拉入该 target 的后端。这个形状同时也是消费者只写一行依赖的原因 —— 虚拟
workspace 会让 `openarch = "0.4.0"` 不得不指名成员目录。

### 3.2.1 两个门面

消费者写一行依赖,然后二选一:

```c
#include <mcpplibs/openarch.h>   /* C,以及想要 C 名字的 C++ */
```
```cpp
import mcpplibs.openarch;        // 四个模块,再导出
```

⭐ 两者是**一个库的两种拼写**,不是两份互相对齐的声明:模块的 `trap_frame`
**就是** `::arch_trap_frame`(`using`,不是同形体),枚举由契约的枚举量*定义而来*
—— `illegal = ARCH_TRAP_ILLEGAL`。`tests/faces.cpp` 检查的是**推导**而不是一致性,
后者是更弱的东西:「两边都是 2」今天成立、明天可能不成立,唯一维持它的是有人同时
改两处。

### 3.2.2 后端由 feature 选择

三个曾由一个机制回答的问题被分开了:

| 消费者要什么 | 清单里写什么 |
|---|---|
| 本 target 的后端 | `openarch = "0.4.0"` |
| 指定某一个 | `default-features = false, features = ["backend-riscv64"]` |
| **自己实现** | `default-features = false, features = ["backend-external"]` + 一个 `provides = ["openarch-backend"]` 的包 |

`backend-external` 不指名任何包,而是 *require 能力*;图里没有提供者时构建在
configure 阶段停下并说明,而不是在链接期报出一个改过名的符号。这与
`std-freestanding` 的分配器同形,于是生态里「一个可被替换的默认实现」只有一种
写法而不是两种。

⚠️ **`backend-auto` 刻意不 require 该能力,而第一版让它 require 了。** feature 是
可加的,而 `requires` 是无条件的 —— 哪怕满足它的 `feature-deps` 是 target 条件化
的。于是本包自己的宿主测试无法构建:

```
error: no package provides capability 'openarch-backend' required by 'openarch'
```

宿主目标没有后端是**关于目标的事实**,不是消费者能处理的错误。

### 3.2.3 类型集中到一处

`openarch/types.h` 定义 `arch_u32`/`arch_u64`/`arch_uptr` 并**断言它们的宽度**。
此前每处用点各自拼出 `unsigned long long`,顶上一段注释解释为什么不是
`unsigned long` —— 一条被描述而从未被检查的规则。它唯一一次被违反(`1UL << 53`)
是靠运气发现的:那个移位恰好在 `constexpr` 里,编译器被迫求值。

⚠️ `arch_uptr` **不**断言为八字节。页表项在每台机器上都是 64 位(包括 32 位机器),
指针不是,而 `riscv32-none-elf` 是本仓库打算到达的目标。断言指针是八字节会在今天
测过的每台机器上通过,而那正是 openkal 在 `fs.h` 里犯过的错。

### 3.2.4 ⚠️ CI 从 0.3.0 起一直是红的,而我此前报告过它是绿的

两处,都是我写的断言把「意图」和「它实际匹配的模式」搞混了:

1. **「探针不按架构分支」这条太宽。** 它 grep `__riscv|__aarch64__`,而探针**必须**
   在恰好一处指名架构 —— 陷入指令,`ebreak` / `brk #0` / `int3` 是同一个想法的三种
   拼写,没有可移植的第四种。trap 接口在 0.3.0 落地时这条断言就开始失败,按它自己
   的字面是对的、按它的意图是错的,而它一直红到 0.3.1 因为没有人去读那些 run。
   收窄为:**一个**条件块,块内除指令外别无他物。
2. portability 作业的扇出问题,见 §3.2。

### 3.3 仍未做:时钟,以及它是否属于这一层

⭐ 建议**先作为调研而不是实现**排期。

riscv 的 `mtime`/`mtimecmp` 是内存映射的,且属于**平台而非 ISA**;可移植的那
一层是 SBI 的 `set_timer`。aarch64 的通用定时器是 CSR。若 riscv 上可移植的
时钟必须经过 SBI,那它属于 **openkal 的实现方**而不是 openarch,而这会改变
分层。

调研的判据:在 `-bios none`(M 模式)与 `-bios default`(S 模式 + OpenSBI)
两种启动方式下各写一个最小时钟探针,看 M 模式那份能否不经 SBI 完成,以及
S 模式那份是否只能经 SBI。两个答案决定接口归属,而调研比实现便宜得多。

### 3.4 不做的事

* 不加第三个架构来「验证」这些接口:门槛的定义是第二台机器;
* 不实现页表**走查**:构造表项是机制,决定表项放在哪里是策略。

---

## 4. x86_64 裸机目标

> **本节的阶段 4 已由 mcpp 2026.8.21.1 提前落地,而落地过程推翻了本节的一个前提。**
> 阶段 1–3(`xim:qemu-x86` 的构建与收录)仍然未做,分析依旧成立。

### 4.0 ⚠️ 「不是代码,是模拟器载荷」这句话是错的

本节原先断言目标行本身没有工作量,只差一个模拟器。实测下来目标行需要**引擎
代码**,而原因是 clang 的属性、不是指令集的属性。

clang 由 triple 选工具链。它为 arm / aarch64 / riscv 备有 *BareMetal* 工具链,
直接以 `ld.lld` 链接;**它没有 x86_64 的**,于是裸 x86_64 triple 的每一种写法都
落到通用 GCC 工具链上 —— 而后者的链接器是**宿主的 `g++`**:

```
g++: error: unrecognized command-line option '-fuse-ld=/…/llvm/22.1.8/bin/ld.lld'
```

对 `x86_64-none-elf`、`x86_64-unknown-none-elf`、`x86_64-unknown-none`、
`x86_64-elf`、`x86_64-none-none`、`x86_64-unknown-unknown` 逐一实测,结果一致;
`-fuse-ld=lld` / `--ld-path=` / `--gcc-toolchain=` / `-B` 逐一实测,均不改变结果。
唯一能改变它的是把 `linux` 放进 OS 位 —— 那会给一次裸机链接带来**八条宿主 `-L`**。

两种结果都不可接受:经宿主 `g++` 会让这一行只在 Linux 宿主上成立(而 macOS 与
Windows 宿主根本没有能产 ELF 的 `g++`);宿主搜索路径出现在 freestanding 链接
上,正是引擎要守住的封闭性。

**解法**:ISA 档表新增 `lldEmulation` 列;置位时引擎直接用 `ld.lld` 驱动链接。
标志的词汇随工具一起改变 —— `-Map=` 而非 `-Wl,-Map=`。⚠️ 仅属于驱动的标志是
**丢弃**而非翻译,而第一次尝试漏了两个:

```
ld.lld: error: unknown argument '-nostdlib++'
ld.lld: error: unknown argument '-Wl,--disable-new-dtags'
```

⚠️ riscv 与 aarch64 两行该列**留空**。它们的驱动本就到得了 lld,为了让三行看起来
一致而改动一条可用的链接,正是引入回归的方式。

第二列 `extra` 承载 `-mno-red-zone`,也不是偏好:System V 的 128 字节红区在有 OS
的机器上安全,是因为内核为中断切了栈;裸机上处理器把中断帧压进红区,被中断的叶
函数恢复后局部变量已被覆盖 —— 不触发异常、没有诊断,而且只在中断恰好落在叶函数
内部时发生。

### 4.0.1 ⭐ 探针能跑起来,靠的是 multiboot 的 a.out kludge

QEMU 的 multiboot 装载器**只接受 32 位 ELF**:

```
qemu-system-x86_64: Cannot load x86-64 image, give a 32bit one.
```

而 ELF 的 class 是整个文件的属性,x86-64 代码产不出 ELF32。走的是 multiboot 的
另一条路 —— flag 位 16 的 a.out kludge,头里自带装载地址,装载器根本不解析 ELF。

⭐ 让这条路的算术成立的是 `SIZEOF_HEADERS`:装载器算的起始文件偏移是
`header_addr - load_addr`,所以这个差必须等于 multiboot 头在文件里的真实偏移。
把镜像起点写成 `0x100000 + SIZEOF_HEADERS`,ELF 头恰好占满 `load_addr` 与
`header_addr` 之间的字节,偏移与地址保持同余、链接器不插填充,差值**按构造**就是
偏移。按平常写法(`. = 0x100000`)`.multiboot` 落在文件偏移 0x1000 而地址
0x100000,`load_addr` 就得是 0xFF000 —— 落在写入会被丢弃的 legacy BIOS 窗口里。

### 4.1 阶段 1–3 卡在哪里

不是代码,是模拟器载荷。`qemu-riscv` 描述符里写明了本索引的收录门槛:为它
服务的五个宿主目标(linux x64/arm64、darwin x64/arm64、win32 x64)从**同一个
版本化发布**提供预编译件,且每个资产带侧文件校验。

xPack 一共只发布两个 QEMU 包(arm 与 riscv),没有 x86。qemu.org 只出 Windows
安装器,macOS 与 Linux 由发行版包服务。因此没有上游过得了这条线。

### 4.2 从源码构建:可行,代价明确

QEMU 可以从源码构建,而且**只构建需要的目标**能把代价压下来:

```
./configure --target-list=x86_64-softmmu --disable-docs --disable-guest-agent \
            --disable-tools --disable-vnc --disable-sdl --disable-gtk
```

单目标构建远小于全量。需要的依赖是 meson、ninja、glib、pixman、zlib。

⚠️ **难点不在构建,在五个宿主目标。**

| 宿主 | 途径 | 难度 |
|---|---|---|
| linux x64 | GitHub 托管 runner | 低 |
| linux arm64 | GitHub 现已提供 arm64 runner | 低 |
| darwin arm64 | macOS runner | 中(依赖走 Homebrew,需处理 `@rpath` 与最低版本) |
| darwin x64 | macOS runner(x64 镜像) | 中,同上 |
| win32 x64 | MSYS2 / mingw | **高**:QEMU 在 Windows 上的依赖链与 DLL 布局是这条路上最麻烦的一段 |

⭐ 更省的一条路:**照 xPack 自己的方式做。** `xpack-dev-tools` 的构建脚本是
公开的,且已经解决了上面五个宿主的全部问题——它只是没有为 x86 建一个包。
沿用它们的构建容器与脚本、把 `--target-list` 换成 `x86_64-softmmu`,比从零
搭五条流水线现实得多。

### 4.3 阶段划分

**阶段 1:只做 linux x64,先用起来。**

`openkal-uefi` 今天在 CI 里用 apt 装 `qemu-system-x86`。先把 linux x64 一个
宿主的载荷构建出来并直接在 CI 里用,它本身就是一次真实验证——载荷能不能启动
一个 UEFI 镜像,是可以立刻回答的。

⚠️ **不要**在只有一个宿主时就收录进索引:描述符的收录门槛是五个宿主,破例
一次就等于把门槛降成注释。

**阶段 2:在一个临时 PR 的 CI 上,用 Actions 构建其余四个宿主。**

⭐ 这一步的价值在于**先把五条流水线跑通,再谈收录**。GitHub 托管的 runner
覆盖 linux x64/arm64、macOS arm64/x64 与 windows x64,恰好就是本索引服务的
五个宿主——所以这五条腿可以在同一个 PR 的矩阵里一次跑出来,产物作为 artifact
留存。

顺序上这比先建仓、先写描述符要省:构建失败在这里只是一个红叉,而不是一个
已发布却装不上的包。⚠️ 而且它把「Windows 那条最难的腿」的风险前置——那一腿
若过不去,五宿主的门槛就不成立,后面的收录也就不该开始。

沿用 `xpack-dev-tools` 公开的构建脚本,只把 `--target-list` 换成
`x86_64-softmmu`,比从零搭五条流水线现实得多:那些脚本已经解决了这五个宿主的
依赖与打包问题,缺的只是没有为 x86 建过包。

**阶段 3:五条腿都绿之后,再补进 xlings 生态。**

镜像到 `xlings-res/qemu-x86` 双端,写 `xim:qemu-x86` 描述符,进
`openxlings/xim-pkgindex`。DT_NEEDED 闭包按 `qemu-arm` 的做法**实测**后决定
`deps` 是否为空,而不是从兄弟描述符抄结论。

**阶段 4:目标表加 `x86_64-none-elf` 行。✅ 已完成(mcpp 2026.8.21.1)。**

与 `aarch64-none-elf` 同样是零 libc 档:索引里没有 x86 的裸机 C 库,而第一批
消费者 —— UEFI 应用与 openarch 的第三个后端 —— 都不需要。

⚠️ 这一阶段本来排在最后,理由是「等模拟器」。实际顺序反了过来:目标行先落地,
openarch 的第三个后端因此写得出来,而**第三台机器正是把门槛从「适配」变成
「抽象」的那一步**(见 §3.1)。模拟器仍然缺,CI 的那一行用 apt 装并注明了原因。

### 4.4 判据

* 阶段 1:`openkal-uefi` 的 CI 不再出现 `apt-get install`,而 OVMF 启动断言
  一字不改地通过;
* 阶段 2:五个资产两端镜像逐字节一致,DT_NEEDED 闭包按 `qemu-arm` 的做法
  实测后决定 `deps` 是否为空;
* 阶段 3:一条 e2e,断言产物而不是退出码——`.text` 的加载地址、零未定义符号、
  以及命令行上的 ISA 参数,与 `tests/e2e/136` 同形。

---

## 5. 排序与建议

| | 项 | 状态 | 建议 |
|---|---|---|---|
| 1 | Windows libc++(第 2 节) | 未做。⭐ 结论已从「加进载荷」**改为独立成包**,理由见 2.5 | 先做 2.1 的测量,再按三步落地。**我们自己能做完** |
| 2 | feature 源不一致(第 1 节) | ✅ **已修复**(2026.8.21.1)。⚠️ 修法就是第三次尝试的判据 —— 它当初被一个**没有去读文件就写下的理由**否掉了 | — |
| 3 | openarch(第 3 节) | ✅ **0.4.0 已完成**:混合式的根、两个门面、feature 选后端、三个指令集 | — |
| 4 | openarch 的时钟归属(3.3) | 未做 | ⭐ 先作为**调研**:两种启动方式各一个最小探针,答案决定接口归属 |
| 5 | x86_64 裸机(第 4 节) | ✅ **阶段 4 已完成**(目标行 + 引擎的直连链接);阶段 1–3 未做 | `xim:qemu-x86` 仍缺。阶段 1 先做 linux x64 供 openkal-uefi 用;阶段 2 用**临时 PR 的 CI 矩阵**跑通五条腿再谈收录 |

⚠️ **本方案排序里有一处判断错了,值得记下来。** 第 5 项原先排在最后,理由是它
「卡在模拟器载荷」。实际做下来,它是**唯一一项改变了对已完成工作之信心**的:
第三台机器挖出了三条两台 RISC 机器合起来也看不见的东西(§3.1)。一个「被外部
依赖卡住」的条目和一个「价值低」的条目在列表上看起来一样,而它们不是一回事。

⚠️ 排期时要当外部依赖的只剩两处,比本文第一版少了一处:`xim:qemu-x86` 需要
进 `openxlings/xim-pkgindex`;QEMU 的五宿主构建若沿用 xPack 的脚本,需要与那个
项目协调。

⭐ **Windows libc++ 从外部依赖变成了我们能自己做完的事**,这正是 2.2 收回那条
论证之后取舍改变的直接结果:独立成包不需要动别人的打包脚本。
