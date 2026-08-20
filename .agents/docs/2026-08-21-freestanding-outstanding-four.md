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

### 2.2 若确认缺失:三种修法与取舍

体积是这里的决定性事实。实测 LLVM 22.1.8 的 Linux 载荷:

| | |
|---|---|
| `include/c++/v1` | 16 MB,1678 个文件 |
| `share/libc++` | 624 KB,133 个文件 |
| 两者 xz -9 压缩后 | **0.9 MB** |
| 整个载荷解包后 | 893 MB |

**方案一:把这两个目录加进 Windows 的 LLVM 载荷。**

代价 0.9 MB,占载荷体积的千分之一。⚠️ 关键点是**不改 Windows 宿主的默认
标准库**:那份 clang 仍然为 Windows 宿主的活儿配 MSVC STL,libc++ 的头只是
**多出来的一份资源**,由 `-nostdinc++` 加显式 `-isystem` 的使用者取用。默认
行为一字不变。

**方案二:独立发一个 `xim:libcxx-headers` 包。**

libc++ 的头是与宿主无关的纯文本,单独成包在概念上更干净:交叉编译要的正是
「这些头」,而不是「某个宿主工具链对该用哪套 STL 的意见」。

⚠️ 但它引入一条版本耦合:这些头必须与产生 `std/*.inc` 导出表的那个 libc++
版本一致,而 `std-freestanding` 的导出表正是从载荷里筛出来的。于是需要一条
「headers 版本 == llvm 版本」的约束,而索引今天没有表达这种约束的机制——
只能靠描述符里的注释,而注释维持不住跨包的不变量(这一点本次实施已经吃过
一次亏:诊断里写死的版本字面量)。

**方案三:`std-freestanding` 自带一份。**

不予考虑。重新分发 libc++ 的头意味着一个包要跟踪上游的每一次变更,而它现在
只需要**筛选**;这会把一个选择器变成一个分发者。

### 2.3 ⭐ 结论与理由

**选方案一。** 理由是三条,按分量排序:

1. **0.9 MB 对 893 MB。** 独立成包所解决的问题——体积——在这里不存在。
2. **版本耦合是真实的,而独立成包会把它变成跨包不变量。** 同一个载荷里的
   两个目录天然同版本;两个包则需要一条索引表达不了的约束。
3. **默认行为不变。** 方案一不改 `-stdlib` 的默认值,所以 Windows 宿主的
   普通构建看不出任何差别。

⇒ 若第 2.1 步确认缺失,向 `xlings-res/llvm` 的打包流程提出:Windows(以及
任何缺失的宿主)的载荷补入 `include/c++/v1` 与 `share/libc++`。

⚠️ 该仓库目前只是发布托管仓(只有 README),打包脚本不在其中,所以这一步是
**对外部的请求而不是我们能自己合入的改动**。方案里必须把它当外部依赖排期,
这与 2026-08-05 那次索引 bump PR 的教训一致。

### 2.4 落地后的判据

* `std-freestanding` 的 Windows 行从「测量缺口」翻转为**真实交叉构建**;
* 该行现有的自退役断言会主动失败并提示注释过时——这正是它被那样写的原因;
* README 的宿主表由「Windows 否」改为「是」,并把 2.2 的取舍留在文档里,
  因为「为什么不独立成包」是个会被反复问到的问题。

---

## 3. openarch 的 trap / per-CPU / 屏障 / 时钟

### 3.1 现状

`openarch` 0.2.0 已通过它自己定义的门槛:同一份探针源码在 riscv64 与
aarch64 上运行,输出逐字相同;两条原语(上下文切换、页表项)各有两个后端。

四个尚未开始的接口是**刻意**未开始的,理由写在包的 README 里:它们的形状
取决于前两条原语的结论,而在门槛通过之前写下它们,等于用一个未经验证的抽象
去生成更多代码。

门槛现在通过了,所以这条理由不再成立。

### 3.2 顺序,以及每一步要回答的问题

按「哪一个最可能证伪接口」排序,而不是按完整性:

| 次序 | 接口 | 最可能出问题的地方 |
|---|---|---|
| 1 | `openarch.trap` | 陷入帧的形状。riscv 用 `scause`/`sepc`/`stval` 三个 CSR;aarch64 用 `ESR_EL1`/`ELR_EL1`/`FAR_EL1`,而 `ESR` 把「什么原因」与「哪一类」编码在同一个寄存器的不同位段里。两者对「一次陷入是什么」的切分不同 |
| 2 | `openarch.cpu` | per-CPU 基址。riscv 惯例用 `tp`;aarch64 用 `TPIDR_EL1`。差别不大,但屏障不同:riscv 是 `fence` 的四象限参数,aarch64 是 `dmb`/`dsb`/`isb` 三条不同指令 |
| 3 | `openarch.timer` | riscv 的 `mtime`/`mtimecmp` 是内存映射且**属于平台而非 ISA**(SBI 的 `set_timer` 才是可移植的那层);aarch64 的通用定时器是 CSR。这一条可能证明「时钟不属于 arch 层」 |

⭐ 第 3 条本身就是一个值得先回答的问题:如果时钟在 riscv 上必须经过 SBI,
那它属于 openkal 的实现方而不是 openarch,而这会改变分层。**建议把它作为
一次调研而不是一次实现来排期。**

### 3.3 判据

与已完成的两条原语一致,**每个接口都必须有一份跑在两台机器上的探针**,并且
探针源码只有一份。新增接口时,`examples/switch` 那种「per-machine 只有三十行
console」的形状要保持——差别一旦超出那个规模,就是接口在漏。

### 3.4 不做的事

* 不加第三个架构来「验证」这些接口:门槛的定义是第二台机器,第三台带来的
  信息量远低于它的成本;
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

**阶段 1(便宜且立刻有价值):只做 linux x64。**

`openkal-uefi` 今天在 CI 里用 apt 装 `qemu-system-x86`。先把 linux x64 一个
宿主的载荷做出来,它就能改用生态内的包,而这本身就是一次真实验证——载荷能
不能启动一个 UEFI 镜像,是可以立刻回答的。

⚠️ 但**不要**在只有一个宿主时就把它收录进索引:描述符的收录门槛是五个宿主,
破例一次就等于把门槛降成注释。阶段 1 的产物先放在 CI 里直接用。

**阶段 2:补齐五个宿主,收录 `xim:qemu-x86`。**

**阶段 3:目标表加 `x86_64-none-elf` 行。**

与 `aarch64-none-elf` 同样是零 libc 档(索引里没有 x86 的裸机 C 库,而第一批
消费者——UEFI 应用与 openarch 的第三个后端——都不需要)。

### 4.4 判据

* 阶段 1:`openkal-uefi` 的 CI 不再出现 `apt-get install`,而 OVMF 启动断言
  一字不改地通过;
* 阶段 2:五个资产两端镜像逐字节一致,DT_NEEDED 闭包按 `qemu-arm` 的做法
  实测后决定 `deps` 是否为空;
* 阶段 3:一条 e2e,断言产物而不是退出码——`.text` 的加载地址、零未定义符号、
  以及命令行上的 ISA 参数,与 `tests/e2e/136` 同形。

---

## 5. 排序与建议

| | 项 | 价值 | 代价 | 建议 |
|---|---|---|---|---|
| 1 | Windows libc++(第 2 节) | 高:让一个宿主从「不可用」变成「可用」,且 0.9 MB | 低,但**含外部依赖** | 先做 2.1 的测量,再提请求 |
| 2 | feature 源不一致(第 1 节) | 高:两条命令行为不一致会让每个带 feature 的包踩一次 | 中:需要 glob 展开 + 两族的 e2e | 紧随其后 |
| 3 | openarch 的时钟归属(3.2 第 3 条) | 中:可能改变分层,而**调研比实现便宜得多** | 低 | 作为调研先做 |
| 4 | openarch 的 trap / cpu(第 3 节) | 中 | 中 | 时钟调研之后 |
| 5 | x86_64 裸机(第 4 节) | 中:打开一个新架构,但 UEFI 那条已经能跑 | **高**:五个宿主的 QEMU 构建 | 阶段 1 可以先做 |

⚠️ 三项与外部有关,排期时要当成外部依赖而不是自己能做完的事:Windows 载荷
的打包脚本不在我们的仓库里;`xim:qemu-x86` 需要进 `openxlings/xim-pkgindex`;
QEMU 的五宿主构建若沿用 xPack 的脚本,需要与那个项目协调。
