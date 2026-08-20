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
| 按「该 glob 是否也出现在 base `sources`」判别 | 无效:gtest 的 base 是一条**能匹配到**那个文件的 glob,不是同一个字符串;字符串相等判别不到 |

三次都已撤销。**不留半个修复**:把一个只在部分情形下正确的门放进已发布的
引擎,比一个有文档的缺陷更糟。

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

1. `mcpplibs/riscv-virt-rt` 不带 feature 时 `mcpp test` 与 `mcpp build`
   编出的目标文件集合相同;
2. mcpp 自己的 `mcpp test unit/test_manifest` 仍然链接并通过(gtest 那一族);
3. 两条断言进 e2e,用两个最小包分别表达上面两族,而不是依赖生态包。

⚠️ 第 3 条是重点。这个缺陷之所以活到今天,是因为**没有任何测试同时覆盖两族**;
只修不测,下一次同样的改动会再次在两者之间摇摆。

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

## 3. openarch:trap 与 cpu 已实现,目录树重整为三层

### 3.1 已完成(0.3.1)

`openarch.trap` 与 `openarch.cpu` 两个接口连同两个后端已实现,探针仍是一份
源码、在两台机器上输出逐字相同。

⭐ **门槛产出的第三条发现:`trap_frame` 必须带 `instr_len`。** 第一版没有它,
理由是「两台机器的陷入指令都是四字节」。实测 rv64gc:

```
cause:3 desc=breakpoint          epc:0x800001f2
cause:2 desc=illegal_instruction epc:0x800001f6   ← 无限重复
```

`0x800001f2` 不是四字节对齐——C 扩展是 `rv64gc` 的一部分,汇编器发的是两字节
的 `c.ebreak`,handler 按 4 推进就落进下一条指令中间。aarch64 只有一种指令
宽度,永远暴露不了这一条。后端知道答案而调用方推导不出来,这正是「该放进
frame」的定义。

其余两处结构性差异:

* **陷入的形状**。riscv 一个入口加一个原因寄存器;aarch64 十六个槽,而「哪个
  槽跑了」这半信息在槽跳走之后任何寄存器里都读不到。本层因此接管 aarch64 的
  向量表,理由与接管 `MAIR_EL1` 一致。⚠️ 每个槽必须在**不破坏被陷入上下文**
  的前提下记下自己的编号——`mov x9, #N` 会毁掉 x9,而那是被中断代码的寄存器。
* **屏障**。riscv 一条 `fence`、操作数是两个集合的叉积;aarch64 三条含义不同
  的指令。接口暴露的是两者都能回答的四个问题,而 `complete` 单列是因为 `dmb`
  与 `dsb` 的区别在写设备寄存器时是正确性问题。

### 3.2 目录树:接口层、ABI 与后端在树上分开

```
openarch/
├── mcpp.toml              [workspace]
├── abi/                   契约 —— 只有头文件,不依赖任何东西
│   ├── mcpp.toml          openarch-abi
│   └── include/openarch/
│       ├── abi.h          后端要实现的东西
│       └── pte_encode.h   纯编码器,宿主可调用
├── spec/                  契约之上的 C++ 模块
│   ├── mcpp.toml          openarch          → 依赖 openarch-abi
│   ├── src/               context/trap/cpu/pte.cppm
│   └── tests/             编码器的宿主断言
├── backends/              每个指令集一个包
│   ├── riscv64/mcpp.toml  openarch-riscv64  → 依赖 openarch-abi
│   └── aarch64/mcpp.toml  openarch-aarch64  → 依赖 openarch-abi
└── examples/switch/       一份探针源码,在每台机器上运行
```

⭐ **`spec/` 拥有全部模块、没有一条指令;`backends/` 拥有指令、不导出模块。**
两条都由 CI 断言,而不是交给目录名——0.3.1 之前后端在 `src/arch/<arch>/`、
与规范同包,分层是一个由路径撑着的约定。

**拆分逼出了一个接口变化,而那个变化本身是对的。** 模块实现单元必须与它实现
的模块同包,所以在 `pte`/`trap`/`cpu` 的边界变成 C ABI 之前,拆包不可能。收益
不是整洁:边界是 C ABI 的规范可以由**不是 C++ 模块**的东西实现——一份汇编、
一个厂商的二进制,或者同一指令集在另一个特权级上的第二个后端。riscv 正需要
最后这一种。

⚠️ **`abi/` 单独成包是依赖图逼出来的。** 第一版把头放在 `spec/`、后端依赖
`spec/`,而 `spec/` 用 cfg 拉后端以免消费者写自己的架构。mcpp 拒绝:

```
error: dependency cycle through package 'openarch'
       while computing its build-cache key
```

放弃自动选后端会让每个消费者写下架构;把头复制进每个后端会造出两份必须一致
而没有机制保证一致的文件。契约不属于任何一侧。

**消费者写的东西一字未变**:`openarch = "0.3.1"`,后端由目标解析。

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

### 4.1 卡在哪里

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

**阶段 4:目标表加 `x86_64-none-elf` 行。**

与 `aarch64-none-elf` 同样是零 libc 档:索引里没有 x86 的裸机 C 库,而第一批
消费者——UEFI 应用与 openarch 的第三个后端——都不需要。

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
| 2 | feature 源不一致(第 1 节) | 未做,三次修法被证伪 | 需要 glob 展开 + 两族的 e2e |
| 3 | openarch 的 trap / cpu(第 3 节) | ✅ **0.3.1 已完成**,目录树重整为 abi/spec/backends 三层 | — |
| 4 | openarch 的时钟归属(3.3) | 未做 | ⭐ 先作为**调研**:两种启动方式各一个最小探针,答案决定接口归属 |
| 5 | x86_64 裸机(第 4 节) | 未做 | 阶段 1 先做 linux x64 供 openkal-uefi 用;阶段 2 用**临时 PR 的 CI 矩阵**跑通五条腿再谈收录 |

⚠️ 排期时要当外部依赖的只剩两处,比本文第一版少了一处:`xim:qemu-x86` 需要
进 `openxlings/xim-pkgindex`;QEMU 的五宿主构建若沿用 xPack 的脚本,需要与那个
项目协调。

⭐ **Windows libc++ 从外部依赖变成了我们能自己做完的事**,这正是 2.2 收回那条
论证之后取舍改变的直接结果:独立成包不需要动别人的打包脚本。
