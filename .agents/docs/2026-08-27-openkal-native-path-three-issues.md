# 目标侧被解析出来了,只发给了一个编译单元

2026-08-27 · 三个 issue 的深度分析 + 优化方案(**待 review,尚未实施**)

三个 issue:
[mcpp#514](https://github.com/mcpp-community/mcpp/issues/514) ·
[openkal-linux#13](https://github.com/mcpplibs/openkal-linux/issues/13) ·
[openkal-musl#13](https://github.com/mcpplibs/openkal-musl/issues/13)

前置:[`2026-08-25-the-two-layer-predicate-family.md`](2026-08-25-the-two-layer-predicate-family.md) ·
[`2026-08-26-cross-target-implies-graph.md`](2026-08-26-cross-target-implies-graph.md) ·
[`2026-08-24-target-side-architecture.md`](2026-08-24-target-side-architecture.md)

---

## 0. 一句话

> **mcpp 已经算出了「这个目标的 C++ ABI 头文件集合是哪 18 个目录」,并且把它准确地
> 交给了 `std` 模块那一个编译单元。图里其余每一个单元,只有恰好站在一条依赖边上的
> 才拿得到。**

`prepare.cppm:7207`:

```cpp
// ⚠️ AND THE HEADERS THIS PACKAGE ITSELF IS BUILT AGAINST.
for (auto& d : pkg.publicUsage.includeDirs)
    flags += " -isystem " + mcpp::xlings::shq(d.string());
```

这段注释把道理写得完全正确 —— 「std 模块源码在任何重要的意义上都是这个包的一个翻译
单元,它到达 C 库头文件的方式和其余单元一样」。**它只差一句:其余单元并不都能到达。**

于是 `std` 是 openkal 味的(对),依赖包的 BMI 是载荷味的(错),任何同时导入两者的
TU 在第一个碰到双方都声明了的名字的模板实例化处炸掉 —— 这正是报告者贴的
`reference to 'space' is ambiguous`。

⭐ 这与 2026.8.25.1/.2 修的七条、2026.8.26.1 修的那条是**同一个形状**:一个已经被
正确回答的问题,答案没有送到所有该读它的地方。区别只在这次不是「谓词问错了」,是
**「答案只发给了一个人」**。

⚠️ **但这句话只覆盖 mcpp#514。** 三个 issue 里有一半不是缺陷,而是报告者被路由到了
一条回答另一个问题的路径上 —— **§1.5 先回答「他要什么、为什么会走到这里、是谁的
缺陷」,§6.5 给通用视角的完整性补充。** 只读修复清单会修错。

> ⭐ **只想看结论的读者直接去 §6.10** ——「三侧各做什么」,含判据的最终形态
> (clause 10 基数 + 原子性)、三个接口的形状草案、mcpp 的七条、以及给报告者的
> 三类答复。中间各章是它的依据。
>
> ⭐ **openkal 及其生态仓库的设计/优化方案已独立成篇** ——
> [`2026-08-27-openkal-ecosystem-design-plan.md`](2026-08-27-openkal-ecosystem-design-plan.md)。
> 本篇是三个 issue 的缺陷分析 + **mcpp 引擎侧**的修复;那篇是 **openkal / openkal-musl /
> openkal-llvm-runtime / 各 backend** 的接口判据与分批清单。两篇不重复。

---

## 1. 五条结论

| # | 结论 | 判据强度 |
|---|---|---|
| **A1** | 提供目标侧层的包,其 `publicUsage` 到不了兄弟依赖包的单元 | ⭐ **本机实测**(§2.2) |
| **A2** | **新发现**:编译侧仍在用 `!crossTargetFlag.empty()` 当「系统来自图」,是 #511 修的那条在**下一行**的孪生兄弟;`e2e 295` 只断言 `ldflags`,看不见它 | ⭐⭐ **本机实测**(§2.3) |
| **A3** | 跨 home 发现:`find_sibling_package` 无条件回落 `~/.xlings`;`active_home_xpkgs` 是 `mcpp::home::root()` 的第二份拷贝 | 读源码 + 本机存在性实测(§2.4) |
| **B** | 依赖缓存键描述了**编译器**,没有描述**它被指向的头文件集合**;同一台机器上 `std` 缓存把这件事做对了 | ⭐ **磁盘上的两个 JSON 实测**(§3.1) |
| **C** | openkal-musl 的 `hidden` 泄漏是**引擎缺口**,不是包的缺陷 —— 那份头文件自己写下了第一优解和它为什么做不到 | 读源码,一行(§4) |

openkal-linux#13 不在这张表里:它**不是一条缺陷**,是三类东西被列在了一张表上。见 §5。

---

## 1.1 判据来源标注

本文严格区分四种依据,凡未实测的推断都标了「**未测**」:

| 记号 | 含义 |
|---|---|
| ⭐ 实测 | 我在本机跑出来的,命令与输出都在文中 |
| 读源码 | 从 `origin/main@078631d` 的源码直接读出的事实 |
| 报告方所测 | issue 里贴的、我没有复现的 |
| **未测** | 我的推断 |

实测环境:`origin/main@078631d`(`2026.8.26.2`)自举构建 ·
x86_64-linux-gnu · llvm@22.1.8 · gcc@16.1.0 · `MCPP_HOME=~/.mcpp`。

---

## 1.5 诉求是什么,以及这条路为什么会长成这样

⚠️ **本章先于逐条修复,因为三个 issue 里有一半不是缺陷,而按缺陷去修会修错。**

### 1.5.1 诉求(报告者自己的话,#492 评论)

> 「我这边要同时出 x86_64 和 aarch64 两份静态二进制」
> 「当时满脑子能出静态产物就行」
> 「we're switching our workspace to llvm + musl static builds via this path and
> retiring our `build-static.sh` workaround」

⇒ **诉求是一个链接属性**:双架构、全静态、无 `PT_INTERP` 的 Linux 可执行文件。

应用本身是一个**完全普通的 POSIX C++23 CLI**:7 成员 workspace、~2k 编译单元、
ftxui 终端 UI、tinyhttps 本地回调服务器、mbedtls、要 shell out、94 个用例的测试套。

⚠️ **它不是一个可移植性诉求。** 他没有要求这份源码跑到裸机、UEFI 或 supervisor 上。

### 1.5.2 他被路由到哪一格,以及为什么

`x86_64-linux-musl` 这**一个目标**在实测矩阵里是四格
([`2026-08-26-the-support-matrix-measured.md`](2026-08-26-the-support-matrix-measured.md) §2/§3):

| | **gcc** | **llvm** |
|---|---|---|
| **payload**(kernel-abi = Linux 直连) | ✅ `xim:musl-gcc` 自带 sysroot,libstdc++,**完整 POSIX** | ❌ ① 密闭性拒绝 —— clang 载荷不带 musl |
| **graph**(kernel-abi = openkal) | ❌ ⑤ 没有 `openkal-gcc-runtime` | ✅ libc++,**POSIX 子集** |

⭐⭐ **两个可用格在反对角线上,而编译器那一维他没得选。**
GCC 16 的 modules 有未修的 ICE(#491 正文:BMI 读回期段错误,cc1plus 堆内 tree
指针损坏),clang 22 编同一代码库全绿。选定 clang 之后,`linux-musl` 只剩右下那格。

⇒ **他没有"选择"openkal —— 矩阵里只有那一格是亮的。**

而右下那格的第三维是 `kernel-abi = openkal`,openkal SPEC clause 7.1 明写这一层
**故意不具备 POSIX 的形状**(没有地址空间复制、没有全局路径命名空间、没有描述符),
因为它要能被实现在裸机、UEFI、supervisor 上。

⭐ **一句话:他要的是一个链接属性,拿到的是一层可移植性抽象。**
在 Linux 上,静态 musl 本来自带 fork/pipe/socket/chmod/symlink —— 因为 musl 直接
对 Linux 说话。经由 openkal 之后这些被那一层的设计目标筛掉了,**而这笔交易他没有
参与,也没有任何一处告诉过他。**

### 1.5.3 构建过程从头到尾没有说过这件事

目标侧报告打的是三行,**全对**:

```
kernel-abi   openkal   (openkal-linux@0.5.4, graph)
c-abi        musl      (openkal-musl@0.3.5,  graph)
c++-abi      libc++    (openkal-llvm-runtime@0.1.3, graph)
```

**没有一行说「因此 fork / socket / chmod / symlink 会在运行期报 ENOSYS」。**
他是在 94 个用例跑出 27 个失败的时候知道的 —— 也就是**依赖解析期已知的事实,
被推迟到运行期才显形**。这与 [[recorded-field-with-no-decision-reader]] 是同一族:
问题问对了、答案也对了,只是没送到需要它的那一侧。

⚠️ **也是为什么 #491/#492 的撤回值得回看一眼。** 那个 PR 提的正是「llvm 家族的
payload musl 路线」,即上表左下 → 右上那一格。它被 openkal 路线取代了,而两者
**回答的不是同一个问题**:

| | 回答的问题 |
|---|---|
| openkal 路线 | 一份源码怎么到达**多个环境** |
| #492 路线 | clang 怎么产出**静态 musl 产物** |

第二个问题至今没有第二个答案。

### 1.5.4 那么,是谁的缺陷?—— 四类

| 条目 | 类别 | 归属 |
|---|---|---|
| §2 目标侧 include 集合到不了兄弟依赖包(A1) | **缺陷** | mcpp 引擎。与 openkal 无关,任何 graph 供给的目标侧都中 |
| §2.3 编译侧谓词(A2) | **缺陷** | mcpp 引擎。`mcpp build --target <宿主自己>` 就中,连依赖都不用 |
| §2.4 跨 home 发现(A3) | **缺陷** | mcpp 引擎(密闭性) |
| §3 缓存键缺头文件集合(B) | **缺陷** | mcpp 引擎(正确性:静默错产物) |
| §4 `hidden` 泄漏(C) | **缺陷,但根因在引擎** | 表现在 openkal-musl,成因是 mcpp 不能表达「编译用/发布用」 |
| §5.2 PC=0 跳转 | **缺陷** | openkal-musl 或 openkal-linux(位置未定) |
| §5.3 `copy_file_range`/`popen`/`last_write_time(dir)` | **缺陷** | openkal-musl 端口层:**可表达而未表达** |
| §5.5 权限静默放宽 | **缺陷** | openkal-linux + 端口层;根在 openkal 无权限模型 |
| §5.4/§5.6 `fork`/`pipe2`/socket/symlink | **不是缺陷** | openkal 在做它声明要做的事(clause 7.1) |
| §1.5.2 反对角线为空 | **不是缺陷,也不是边界** | **矩阵空缺** —— 没人做,而不是不能做 |

⭐ **最后一行是本文最重要的一行。** 前面九行加起来修完,报告者仍然在 openkal 上,
仍然没有 `fork`。让他拿到他真正要的东西的,是最后一行。

---

## 2. mcpp#514 §A —— 目标侧的头文件集合只到达一个单元

### 2.1 现状:三条互不相交的路径

一个编译单元的 include 集合今天由三处拼出来,**互不知情**:

| 来源 | 代码 | 到达谁 |
|---|---|---|
| 工具链载荷的头文件 | `hostflags.cppm:199-217` → 全局 `cxxflags` 规则 | 每一个单元 |
| 根清单的 `[build] include_dirs` | `flags.cppm:482` → 全局 `cxxflags` 规则 | 每一个单元(**含依赖包**,见 §2.2 的旁证) |
| 依赖包发布的 `publicUsage` | `prepare.cppm:3490` 定点 → `privateBuild` → `scanner.cppm:1100` → **逐单元** | **只有这条边的消费者** |

openkal 那 18 个目录走第三条。而 `nlohmann.json` 不依赖 `openkal-llvm-runtime` ——
它是图里的兄弟,不是下游。所以它一个都拿不到。

⭐ **这不是传播漏了一条边,是这个事实放错了模型。**
`publicUsage` 描述的是「一个库对它的使用者的要求」,而目标的 C++ ABI 头文件集合是
**整张图的属性** —— `mcpp.targetside` 那个模块的开篇写的就是这句话:

> The fix is not a better guess. It is to resolve once, after the graph is known,
> and to have every consumer read that one value.

目标侧已经 resolve 了(`prepare.cppm:6713`),`std` 模块读了它(间接地,通过
provider 的 `publicUsage`),**编译边没有读**。

### 2.2 ⭐ 实测 ①:20 行复现,不需要 openkal

```
work/
  mcpp.toml            [dependencies] abiprov = { path = "abi" }
                                      mcpplibs.cmdline = "0.0.1"
  src/main.cpp         import abiprov; import mcpplibs.cmdline;
  abi/mcpp.toml        provides = ["mcpp:c++-abi=libc++"]
                       [build] include_dirs = ["abi-include"]
```

`mcpp build` 后读 `compile_commands.json`,问每一行「命令里有没有 `abi-include`」:

```
DEP   cmdline.cppm           abi-include on line: False    ← 兄弟依赖包
DEP   options.cppm           abi-include on line: False
DEP   parse.cppm             abi-include on line: False
ABIPKG abiprov.cppm          abi-include on line: True     ← provider 自己
ROOT  main.cpp               abi-include on line: True     ← 消费者
```

⭐ **一个声明了 `mcpp:c++-abi` 的包,它发布的头文件目录到不了兄弟依赖包。**
报告里的现象在没有 openkal、没有 musl、没有交叉的情况下就成立了。

⚠️ 旁证:同一次实测里,根清单的 `[build] include_dirs = ["hdrs"]` **确实**出现在了
`cmdline.cppm` 的命令行上(它走全局规则)。所以「依赖包看不到根的 include」这个直觉
是错的 —— 看不到的恰恰是走 `publicUsage` 那条路的,也就是目标侧走的那条。

### 2.3 ⭐⭐ 实测 ②(新发现):编译侧的谓词还没修

`hostflags.cppm:199`:

```cpp
const bool graphSuppliesTarget = !tc.crossTargetFlag.empty();

if (bypassCfg && !graphSuppliesTarget) { ...载荷 libc++ 头... }
if (!trustCfg && !graphSuppliesTarget && ...) { ...载荷 glibc / linux-headers 头... }
```

这**正是** [`2026-08-26-cross-target-implies-graph.md`](2026-08-26-cross-target-implies-graph.md)
分析的、`2026.8.26.1`(#511)在链接侧改成 `plan.targetSide.cAbi.prebuilt()` 的那个
谓词。`git log -- src/toolchain/hostflags.cppm` 的最后一次改动是 #486 ——
**编译侧从未跟上**。

同一台机器、同一个编译器、同一个目标,只差写不写 `--target`:

```
$ mcpp build
$ mcpp build --target x86_64-unknown-linux-gnu
```

`build.ninja` 的 `ldflags` 行(#511 修过的那条):**逐 token 相同 ✔**
`cxxflags` 行:显式那条**少了六个 token**:

```
< --no-default-config
< -nostdinc++
< -isystem <store>/xim-x-llvm/22.1.8/include/c++/v1
< -isystem <store>/xim-x-llvm/22.1.8/include/x86_64-unknown-linux-gnu/c++/v1
< -isystem <store>/xim-x-glibc/2.44/include
< -isystem <store>/xim-x-linux-headers/5.11.1/include
```

⭐⭐ **三个后果,一个比一个重:**

1. **头文件来自一个库,目标文件链自另一个库。** 链接线保留了载荷的
   libc++/glibc(#511 修对了),编译线把它们全丢了 —— clang 回落到自己的默认搜索。
   本机有系统头就悄悄编过去,没有就报一个指向载荷的错。
2. ⚠️ **`--no-default-config` 一起消失了。** 于是任何带 `--target` 的构建都从
   「一切显式」切换成「这台机器的 cfg 说了算」,而 `post_install.cppm:266` 自己写着
   那个 cfg 是「per-machine, per-install-path artifact」。
   ⭐ **这解释了报告者的 workaround 为什么有效** ——
   他手写的 `x86_64-unknown-linux-musl-clang++.cfg` 之所以被读到,正是因为
   `--target` 路径把 `--no-default-config` 丢掉了。报告里那句
   「必须叫 `-clang++.cfg` 这个名字」也随之解释:那是 clang 自己的
   `<triple>-<driver>.cfg` 默认查找规则,而不是 mcpp 的机制。
3. **`e2e 295` 看不见它。** 那个测试的不变式写的是「命名宿主自己的目标什么都不改变」,
   而它只取 `grep -m1 '^ldflags'`。同一个不变式在下面一行不成立,而测试不看那一行。

### 2.4 第二个皱褶:跨 home 发现

报告者说「机器上存在别的 mcpp home 时,依赖单元会拿到**它们的**宿主工具链 include
集合」。两处机制:

**(a) `xlings.cppm:935`** —— `find_sibling_package` 找不到时无条件回落
`$HOME/.xlings/data/xpkgs`:

```cpp
// Also check ~/.xlings/data/xpkgs/ (xlings global home) as fallback.
const char* home = std::getenv("HOME");
if (home) { auto xlingsXpkgs = path(home)/".xlings"/"data"/"xpkgs"; ... }
```

`probe.cppm:448` 用它找 `linux-headers`,结果直接进每一条编译命令的 `-isystem`。
⭐ 本机 `~/.xlings/data/xpkgs` **存在**(实测,含 `fromsource-x-*` 等 30+ 包)——
这条回落在这台机器上是活的。

**(b) `xlings.cppm:836`** —— `active_home_xpkgs()` 自己重新推导了一遍 home:

```cpp
if (const char* h = getenv("MCPP_HOME")) home = h;
else if (const char* u = getenv("HOME")) home = path(u)/".mcpp";
```

而 `mcpp.home::root()` 的答案有**三档**(`$MCPP_HOME` → 自包含安装
`<binary-dir>/..` → `$HOME/.mcpp`)。自包含安装下两者给出不同的 home。
⚠️ `home.cppm` 的开篇写的正是这件事:「Every path under the mcpp home must be
derived from here. Before #311 this logic existed in three places … the copies
drifted」。**这是第四份拷贝。**

⚠️ **未测**:报告里「`--target musl` 时那些行仍然出现」这一点,与 §2.3 的实测冲突
(`--target` 非空 ⇒ 载荷行被抑制)。最可能的解释是那次构建走了 **fast path 重放**
—— `.build_cache` 不记录 home,整项目指纹也不记录(`fingerprint.cppm` 11 个字段里没有
任何一个是 home 或载荷路径,且 `normalize_driver_output` 会**故意**把 `/home/` 路径
抹掉),于是换了 home 也命中同一个 `target/<triple>/<fp>/`,ninja 重放的是上一次生成
的、写着另一个 home 绝对路径的 `build.ninja`。这也解释了「选中的味道会在
`resolution.json` 里跨次留存」。**这条要一次实测才能定案**,命令在 §7。

### 2.5 修复方案

#### A1 —— 目标侧的使用要求是全图的,不是一条边的

在 `prepare.cppm` 目标侧解析完成之后(`resolvedTargetSide` 已定,`~6713`),收集
**从图里供给任一 `mcpp:` 层的包**的 `publicUsage`,并入**每一个**包的
`privateBuild`:

```cpp
// 目标侧的头文件集合是这次构建的属性,不是某条依赖边的属性。
// 它已经被算出来了 —— prepare.cppm:7207 把同一个集合交给了 std 模块。
UsageRequirements targetSideUsage;                      // 四个字段:
for (idx : layerProviderPackageIndices)                 //   includeDirs
    merge(targetSideUsage, packages[idx].publicUsage);  //   includeDirsAfter
for (auto& p : packages)                                //   cflags / cxxflags
    appendUnique(p.privateBuild, targetSideUsage);      // 已在的会被去重
```

四条设计判断:

1. **只并入 `privateBuild`,不并入 `publicUsage`。** 它对全图可见,不需要再传播;
   写进 `publicUsage` 会让它进入被打包的库的使用要求,那是另一个层次的承诺。
2. **追加在末尾。** 包自己的头文件仍然先被搜到;目标侧只需要排在**驱动默认目录之前**,
   而驱动的默认目录永远在最后。
3. **provider 自己是 `layerProviderPackageIndices` 的成员**,所以它不需要特例 ——
   `appendUnique` 让「它本来就有」成为无操作。
4. ⭐ **`prepare.cppm:7207` 那一段随之变成这条规则的一个实例。** 不要保留两份:
   std 模块的 flags 应当从同一个 `targetSideUsage` 取,否则「哪些目录是目标侧的」
   这个决定又有了两处推导 —— 这个仓库为这个形状付过四次代价(#233/#240/#242/#344)。

⚠️ **一个必须同时回答的问题**:`layerProviderPackageIndices` 里要不要含
`Origin::Graph` 以外的层?**不要**。载荷/xpkg 供给的层由 `hostflags` 那条路负责,
两条路同时发会让顺序变成两处决定。判据就是 `Layer::fromGraph()`。

#### A2 —— 编译侧读 `targetSide`,和链接侧读的是同一个值

`hostflags.cppm` 拿不到 `plan.targetSide`(它是 `mcpp.toolchain` 层,不能 import
`mcpp.build.plan`)。⇒ 通过 `HostFlagOptions` 传一个 `bool cAbiPrebuilt`,
在 `flags.cppm` 那一处从 `plan.targetSide.cAbi.prebuilt()` 填,和链接侧**同一个表达式**:

```cpp
hopt.cAbiPrebuilt = plan.targetSide.cAbi.prebuilt();   // flags.cppm，与 1245/1484/1547 同源
...
const bool graphSuppliesTarget = !opt.cAbiPrebuilt;    // hostflags.cppm:199
```

⚠️ **不要在 hostflags 里重新推导。** 那正是 #486 的开篇列出的三处分歧的成因。
`hostflags` 的其它调用方(std 模块预编译、build.mcpp 宿主编译)必须一并给出这个值,
否则 `std.pcm` 和导入它的单元会用两套头文件 —— 这就是 e2e 181 抓过的形状。

⚠️ **`--no-default-config` 要单独拉出来。** 它今天被 `bypassCfg && !graphSuppliesTarget`
连坐,而它与「目标侧从哪来」无关:cfg 是 per-machine 的不可复现产物,**任何**
mcpp 构建都不该读它。改成无条件发(clang 且有 cfg 时)。

#### A3 —— home 发现收敛到一个答案

1. `active_home_xpkgs()` 改为 `mcpp::home::root() / "registry" / "data" / "xpkgs"`。
2. `find_sibling_package` 的 `~/.xlings` 回落:**去掉**,或改为只在
   `mcpp::home::root()` 与该路径同源时才走。
   ⚠️ 这条回落写于跨 home 还不是问题的时候;删掉它可能让某些机器上的
   `linux-headers` 探测从「找到别人的」变成「找不到」——
   而 `probe.cppm:453` 的 verbose 分支已经为「找不到」准备好了诊断。
   **这个方向是对的:找不到会说话,找错了不会。**

#### 判据(A)

| # | 判据 | 形态 |
|---|---|---|
| A-1 | §2.2 那个 20 行工程里,`cmdline.cppm` 的命令行含 `abi-include` | 新 e2e,断言 CDB 的行而不是 grep 全文 |
| A-2 | `e2e 295` 同时比对 `^cxxflags` 与 `^ldflags`,两条都是恒等式 | **改现有测试**,一行 |
| A-3 | `--no-default-config` 在两种拼写下都在 `cxxflags` 里 | 并入 A-2 |
| A-4 | 单测:`targetSideUsage` 只收 `fromGraph()` 的层 | `test_targetside.cpp` 的表可以直接扩 |

⚠️ **A-2 是这批里最重要的一条**,因为它是**恒等式**:不需要期望值表,在每台宿主上
都成立,而且它会在下一次有人只修一层时立刻变红。

---

## 3. mcpp#514 §B —— 键描述了编译器,没描述它被指向的头文件

### 3.1 ⭐ 磁盘上的两个 JSON

同一台机器,同一个 `build-cache/v1`:

```
~/.mcpp/build-cache/v1/pkg/mcpplibs/cmdline@0.0.1/<key>/entry.json
  inputs.toolchain = { compiler, compiler_version, driver_identity,
                       target_triple, target_implied_flags,
                       stdlib, stdlib_version }        ← 七个字段，没有一个是头文件集合

~/.mcpp/build-cache/v1/std/<key>/std-module.json
  std_build_commands = [ "... -isystem ... -isystem ... " ]   ← 整条命令行，含头文件集合
```

⭐⭐ **两个缓存,一台机器,两种「输入相同」的定义。** `std` 那个是对的 ——
`stdmod.cppm` 的注释说它「一直就是正确的身份」。依赖那个少一根轴。

而且这根轴是**故意**被抹掉的一半:`normalize_driver_output`(`probe.cppm:151`)
会把 `/home/`、`/tmp/`、`/var/` 开头的路径整段替换掉,**这是对的** —— 它让缓存
跨 home 可共享。错的是**没有任何别的东西**接着说出「跨的这两个 home 里,glibc 是
2.39 还是 2.44、linux-headers 是哪一版、cfg 文件写了什么」。

### 3.2 报告的三个方向,机制各不相同

| 方向 | 报告的现象 | 机制 |
|---|---|---|
| 1 | 装了 cfg workaround 之后,被污染的 BMI 仍然被复用 | ⭐ **成立且已定位**:cfg 文件是 `--target` 路径下每条编译命令的输入(§2.3),而键里没有它。加了 cfg 键不动 ⇒ 命中 ⇒ 从不重编 |
| 3 | 两次**宿主**构建,glibc 头来自不同 home(2.39 vs 2.44),命中同一条目 | ⭐ **成立且已定位**:`driver_identity` 抹掉路径 + 键无 sysroot 轴 ⇒ 同键。混合 BMI 让 clang 前端 SIGSEGV 在 `ASTReader::FindExternalVisibleDeclsByName` —— **反序列化崩溃而不是可读诊断**,因为 BMI 之间没有互校验 |
| 2 | musl 构建填了缓存后,宿主 `mcpp build` 复用了 musl 味的 BMI | ⚠️ **按报告的字面意思不成立**:`target_triple` **在**键里(上面的 JSON 实测),`--target x86_64-linux-musl` 会改写 `tc.targetTriple`(`prepare.cppm:2480`,clang 分支)。⇒ 两次构建的 dep 键必然不同。**要么是另一条路径把它重新调味了,要么这条是从症状推出来的。§7 给了定案命令** |

⚠️ 我把方向 2 单独拎出来说,是因为 **[[recorded-field-with-no-decision-reader]] 的
教训是双向的**:一条被推出来而没被测的因果,会让修复落在一个已经为真的谓词上。
`dep-bmi-cache-cross-version-poisoning` 那次我把根因写错过两次,第二次靠 grep 生成物
才定死。

### 3.3 ⭐ A 修好之后,B 自己消失一半

`cache_key::fill_package_config` 已经把 `pkg.privateBuild.includeDirs` 折进
`includes` 轴(前缀 `priv:`,相对 `<store>` 归一化)。

⇒ **A1 把目标侧目录并进每个包的 `privateBuild` 的那一刻,每个依赖包的键就自动带上了
目标侧的身份。** 不需要为 openkal 这条路加任何新轴。

这也是判断 A1 那个形状对不对的一个独立证据:**一个正确的建模会让下游的键自己变对。**

### 3.4 剩下的一半:载荷侧的头文件集合

A1 覆盖不到「同一个 triple、同一个 clang、不同的 glibc/linux-headers 载荷」。这需要
一根显式的轴。最小形状:

```cpp
// BuildAxes 新增一个字段，A 轴（toolchain）内：
std::string targetHeaderSet;   // 头文件搜索集合的摘要
```

内容取**已经解析好的**那三处,不要重新推导:

- `resolve_clang_driver(tc).compile_tokens(...)`
- `resolve_link_model(tc).compile_tokens(...)`
- clang cfg 文件的**内容摘要**(存在时),因为它是命令的一部分而不在命令里

⭐ **取 token 而不是取路径**:`stdmod` 已经证明了这个取法是对的(它折进整条命令行),
而且 token 里的 `<store>` 路径可以用 `fill_package_config` 已有的归一化函数处理,
保持跨 home 可共享 —— 这正是 §3.1 说「抹路径是对的」的那个性质。

⚠️ **不要 bump `kCacheEpoch`。** 那个常量的注释规定了它的判据:「Bump ONLY when a
change makes previously written entries **unusable**」。加一根轴让旧条目**miss**,
不让它们不可用;而且 `inputs_match`(`bmi_cache.cppm:211`)对
`inputs.toolchain` 是整对象比较,旧条目自然不匹配。`cache gc` 的大小统计也仍然有效。

⚠️ **`std` 缓存不需要改** —— 它已经对了。但它和 dep 缓存现在会因为**不同的理由**
失效,`mcpp cache list` 的输出会短暂地看起来不一致。这是正确的,值得在
`docs/` 里写一句。

#### 判据(B)

| # | 判据 | 形态 |
|---|---|---|
| B-1 | 单测:两个只在 `targetHeaderSet` 上不同的 `BuildAxes` 产生不同的 `key_hex` | `test_cache_key.cpp` 已有表 |
| B-2 | ⭐ e2e:`--cache global` 下,**写一个 cfg 文件**再构建,`build-cache/v1/pkg/...` 下多出一个目录 | ⚠️ **不能写进真载荷**;测试要在临时 `MCPP_HOME` 里做 |
| B-3 | A1 落地后,含 `mcpp:c++-abi` provider 的图与不含的图,同一个依赖包落在两个键上 | e2e,判据是**目录名**不是日志行 |

⚠️ **B-2/B-3 的判据必须是磁盘上的条目,不是 CLI 打的 `Cached` 那一行。**
`dep-build-cache-scoping` 那次,「假状态行骗了三个月」。

---

## 4. openkal-musl#13 —— 包不能区分「我从哪儿编」和「我发布什么」

### 4.1 那份头文件自己写下了答案

`openkal-musl/port/include/features.h:70`:

> ⓘ **THIS IS THE SECOND-BEST REMEDY.** The first would be for a package to
> distinguish the directories it is **built from** from the directories it
> **publishes**. Measured 2026-08-22: **mcpp cannot express it** — publicUsage
> takes privateBuild's include directories entire. Moving the two directories
> into per-glob flags places them AFTER include_dirs on the command line, and
> musl's own build then finds the public `<features.h>` before the internal one
> and fails with `unknown type name hidden`.

引擎侧就是一行,`prepare.cppm:4162`:

```cpp
pkg.publicUsage.includeDirs      = pkg.privateBuild.includeDirs;
pkg.publicUsage.includeDirsAfter = pkg.privateBuild.includeDirsAfter;
```

⇒ **issue 报的是 openkal-musl 的行为,缺陷在 mcpp。** 包侧现在的做法(`hidden` 在
C++ 下给 `extern "C"`、C 下清空;`weak` 清空;`weak_alias` 删除)已经是第二优解,
而且被三位不同的消费者各校准过一次(`restrict` / 链接性 / compiler-rt 的 `weak`)。
**不要动它。**

### 4.2 方案:`[build] private_include_dirs`

```toml
[build]
include_dirs         = ["port/include", "musl/include"]        # 编译 + 发布
private_include_dirs = ["musl/src/include", "musl/src/internal",
                        "musl-generated/internal"]             # 只编译，不发布
```

- 语义:进 `privateBuild.includeDirs`,**不**进 `publicUsage.includeDirs`。
- 顺序:与 `include_dirs` **交错保持声明顺序**,因为 features.h 那段实测说得很清楚
  —— 把这两个目录挪到后面,musl 自己的构建就先找到公共 `<features.h>` 而失败。
  ⭐ 所以这不能实现成「private 的都排在后面」,必须是**同一个有序列表,带一个
  发布位**。
- ⚠️ 这是 `[build]` 的一个新键。按
  [[new-capability-key-floor-measured]]:`mcpp.toml` 里不认识的键会让整份 manifest
  加载失败还是被忽略?**发布前必须实测索引 latest 的行为**,并且 openkal-musl 的
  `min_mcpp` 下限要写下界。

### 4.3 为什么这值得做,而不是「包自己绕过去」

`OKM_MUSL_INTERNAL` 那条(见 [[c-library-configured-by-what-is-beneath]])解决的是
**宏**的泄漏,它成立是因为 `defines` 不传播。**目录**没有对应的机制,所以三个
musl-internal 目录仍然出现在每一个消费者的命令行上 —— `hidden` 只是**已经被发现的
那一个**名字。musl 的 `src/include` 覆盖层里还有 `__syscall`、`__libc`、`a_cas` 等
一批名字,下一个消费者会撞上下一个。

⭐ **判据不是「`hidden` 不再泄漏」,是「那三个目录不出现在消费者的命令行上」。**
前者会随着包侧再打一个补丁而变绿,而缺陷还在。

#### 判据(C)

| # | 判据 |
|---|---|
| C-1 | 单测:声明了 `private_include_dirs` 的包,其 `publicUsage.includeDirs` 不含这些目录,`privateBuild.includeDirs` 含且**顺序与声明一致** |
| C-2 | e2e:消费者的 CDB 行里没有 provider 的私有目录,provider 自己的行里有 |
| C-3 | 生态:openkal-musl 改用新键后,一个把 `hidden` 用作普通标识符的消费者能编过 —— **这条要等 mcpp 发布并进索引** |

---

## 5. openkal-linux#13 —— 三类东西,一张表

report 里那张 ENOSYS 表把三种性质完全不同的东西列在了一起。**逐条实现会做错至少三条。**

### 5.1 分类

| POSIX 面 | 类别 | 依据 |
|---|---|---|
| `copy_file_range` / `sendfile` | **① 端口层可解,不动 spec** | 语义就是「从一个流读、往另一个流写」,`kal_stream_read/write` 齐全 |
| `popen` / 子进程输出捕获 | **① 端口层可解,不动 spec** | 见 §5.3 |
| `last_write_time(dir)` 报 "Is a directory" | **① 端口层缺陷** | `kal_fs_open_dir` + `kal_fs_info` 都在;`.` 是保留名(clause 7.12),目录自己可以被问 |
| `fork` | **② 设计边界,不是缺陷** | SPEC clause 7.1 明写:复制地址空间不能在每个环境上忠实完成 |
| 文件权限位 / `chmod` | **③ 真缺口,而且当前行为是静默放宽** | 见 §5.5 |
| `symlink` / `symlinkat` | **③ 真缺口** | `KAL_FS_PROP_LINKS` 与 `kal_node_link` 都在,**没有任何操作能用它们** |
| socket / bind / listen | **③ 真缺口** | SPEC clause 3.4 已经点名了 `openkal.net`,见 §5.6 |
| **PC=0 的跳转** | **⓪ 唯一无歧义的缺陷** | 见 §5.2 |

### 5.2 ⓪ NULL stub —— 先修这个,因为跳到 0 的程序说不出任何话

⭐ **这条排第一不是因为最严重,是因为它让其余每一条都测不准。**
一个在早期初始化就 SIGSEGV 的程序,后面 26 个失败用例的读数都是它的下游。

我能从源码确定的**收窄**:

- `okm_syscall.c` 的 `default:` 分支返回 `-ENOSYS`(实测 `grep`,68 个
  `case SYS_*` + 一个 default)。⇒ **不是未知系统调用走空指针。**
- 端口层一共只有**两处**弱引用(`grep '__attribute__((weak))' port/src` = 2 行):
  `okm_phdr.c:39` 的 `__ehdr_start` 与 `okm_syscall.c:31` 的 `kal_random_fill`,
  **两处都判空了**。⇒ 不是 [[link-error-is-the-mechanism-not-the-defect]] 第二形态
  的复发。

⇒ 三个仍然开着的候选,每个配一条**决定性**检查:

| 候选 | 决定性检查 |
|---|---|
| `.init_array` 走过了头 / 含 0 项(`okm_start.c:179` 的循环**逐项不判空**) | `readelf -x .init_array <bin>`;并在循环里临时打印 `a` 的范围 |
| 程序自己 `dlsym` 得到 0 就调用(静态 musl 的 `dlsym` 恒返回 0) | `nm <bin> | grep dlsym`;`gdb` 在 `dlsym` 下断 |
| 某个 `kal_*` 被以函数指针形式取址后置零(不是弱引用,是数据) | `gdb`:`x/4gx $sp` 取返回地址 → `info symbol` |

⚠️ **不要凭报错信息跳到修法。** [[reasons-written-from-memory-kill-good-fixes]]、
[[link-error-is-the-mechanism-not-the-defect]] 都是同一天里在同一个仓库上踩的:
「报错信息不是规范」。这里连报错信息都没有 —— 只有一个空栈。

⚠️ **判据的单位是一整行输出。** 空栈 + PC=0 时,`bt` 是空的,但
`info registers rip rsp` 与 `x/8gx $rsp` 不是。

### 5.3 ① 端口层可解的四条 —— 一个都不用动 spec

**(a) `copy_file_range` / `sendfile`。** 在 `okm_syscall.c` 里加两个 case,实现成
`kal_stream_read` → `kal_stream_write` 的循环。⚠️ 短写不是成功结果
([[openkal-spec-and-linux-impl]]),循环必须写全或报错。

**(b) `popen` —— ⭐ 替换源码,不要重建机制。**

这正是 `okm_spawn.c` 开篇已经论证过的动作:

> musl starts a program by duplicating itself … openkal has neither operation …
> What openkal has instead is **the composite** … so the replacement is a
> **translation of arguments rather than a reconstruction of a mechanism** ——
> and it is shorter than the code it replaces.

`posix_spawn.c` 已经在 `mcpp.toml:118` 的排除表里。`popen.c` **不在**,所以它还在
用 `pipe2` ⇒ ENOSYS。而 `kal_process_spawn` 的 `struct kal_spawn_streams`
(`process.h:17`)允许调用方指定三个流 —— **子进程的 stdout 可以是父进程给的任意流**。

⇒ 把 `!musl/src/stdio/popen.c` 加进排除表,写 `port/src/okm_popen.c`:用
`kal_fs_open`(`CREATE|EXCLUSIVE|TRUNCATE`)开一个临时文件当子进程的 stdout,
`kal_process_wait` 之后把它 seek 回 0 交给 `FILE*`。

⚠️⚠️ **这不是 `pipe2`,不要假装是。** 语义差三处,而且都必须写进包的 README:
不能与子进程并发交错;不能用于 self-pipe;`popen(..., "w")` 方向要单独处理。
⭐ **正因为差,才必须实现成 `popen` 而不是实现成 `pipe2`** ——
一个假的 `pipe2` 会让 self-pipe 的调用方**静默地永远等下去**,而这正是
[[c-library-configured-by-what-is-beneath]] 里 futex 那条踩过的:
「⚠️ 答案由被重建的那张面决定」。

ⓘ 顺带:musl 的 `system()` 走的是 `posix_spawn`,所以 **`std::system` 今天就能用**。
报告里「不能 shell out」的范围比它看起来窄,值得在回复里说清楚。

**(c) `last_write_time(dir)`。** clause 7.12 的保留名 `"."` 就是为这件事存在的
(「a program holding a directory has no way to ask an operation about that
directory」)。端口层的 `stat`/`statx` 对目录路径应当走
`kal_fs_open_dir` + `kal_fs_info(dir, ".")`,而不是 `kal_fs_open` 文件路径。

**(d) symlink 的**报告**方式。** 端口层已经在
`okm_syscall.c:688/699` 对 `kal_node_link` 返回 `-ENOSYS`。这是对的,保留;
真正缺的是**创建/读取**,那属于 ③。

### 5.4 ② `fork` 是边界不是缺陷 —— 诊断而不是实现

SPEC clause 7.1 与 `okm_spawn.c` 的开篇都把 `kal_fork` 这个**操作**写死了。

⚠️⚠️ **本节初稿写的是「openkal 不会长出 `fork`,这是通用性的硬天花板」——那句话错了。**
clause 7.1 禁止的是**这个操作**,没有禁止**地址空间的原子能力**(创建地址空间 /
映射内存 / 以给定寄存器状态在其中启动上下文)。有了那三样,端口层**可以**把 `fork`
组合出来 —— Fuchsia 没有 `fork` 却有 `zx_process_create`/`zx_vmar_map`/`zx_thread_start`。

⇒ 正确的说法是:**`fork` 需要一组 openkal 目前没有的原子能力,而那组能力是一个
将来的候选,不是一条永久的拒绝。** 为什么不在本批(三条理由)见
[`2026-08-27-openkal-ecosystem-design-plan.md`](2026-08-27-openkal-ecosystem-design-plan.md) §1.2。

**本节其余结论不变**:今天没有那组能力,而报告者那一处应当改用 `posix_spawn` ——
因为**他那份代码在传统体系里也不可移植**(Windows 没有 `fork`)。

⚠️ 但报告者读到的是 `"fork failed: Function not implemented"` —— 一个不区分
「本实现没有」和「本模型没有」的答案。⇒ 值得做的是**让这条边界可读**:
openkal-musl 的 README 增一节「这个 C 库不提供什么,以及改用什么」,把
`fork`→`posix_spawn`、`pipe2`→`popen`、`socket`→(无)列成一张表。

⭐ **一条不能实现的调用,唯一能改善的是它被理解的速度。**

### 5.5 ③ 权限位 —— 当前行为是**静默放宽**,这是唯一被明令禁止的失败模态

读源码:

- `openkal/include/openkal/fs.h` —— `kal_fs_open` 的 flags 只有 6 位
  (READ/WRITE/CREATE/EXCLUSIVE/TRUNCATE/APPEND),**没有任何权限参数**;
  `kal_node_info` 只有一个 `writable` 布尔。
- `openkal-linux/src/fs.cpp:127` —— `openat(..., f, 0666)` 硬编码;
  `:236` —— `mkdirat(..., 0777)` 硬编码。

⇒ 程序请求 `0600` 拿到 `0666 & ~umask`。⚠️ **这不是「缺一个特性」,这是
`okm.h` 自己划的唯一禁区**:

> ⚠️ THE ONE WAY THIS COULD GO WRONG IS NOT PRESENT: **nothing below reports
> SUCCESS having done nothing.**

一个要求私有的文件被创建成组内/全局可读,而调用方**收到了成功**。密钥、token、
会话文件都走这条路。⭐ **这条的严重性高于表里其它任何一条,而它在报告里排第四。**

**最小的、加性的、clause 允许的一步**(如果一定要动 spec):

- `kal_fs_props` 加一个位 `KAL_FS_PROP_PERMISSIONS`。clause 6.2 明写属性字就是
  为这类问题准备的,而且「a position that has not been assigned reads as zero, so
  that a program compiled against a later specification behaves correctly against
  an earlier implementation」—— **向后兼容是这个机制自带的**,clause 8 也允许
  「A revision may add declarations」。
- **不加操作。** `chmod` 是一个 operation,clause 6.2 说 operation 的缺席应当由
  **独立接口**表达(链接期缺席),而不是塞进 `openkal.fs` —— 因为一个 FAT 分区上的
  实现永远满足不了它,那正是 clause 6.4 禁止的。

⭐ 而在 spec 动之前,**端口层就能把静默去掉**:`O_CREAT|O_EXCL` 且请求模式比
环境能给的更严格时,openkal-musl 至少要能被问出真相。具体形态需要一次设计讨论,
本文不预设。

### 5.6 ③ socket —— spec 已经预留了名字,但现在不做

SPEC clause 3.4 里 `openkal.net` **已经被点名**:

> An earlier draft replaced `openkal.fs` and **`openkal.net`** with a single
> interface … Positioning applies to a file and not to a connection;
> **half-closure applies to a connection and not to a file.**

⇒ 加一个 `openkal.net` 是 spec **预期内**的演进(clause 3.2:新接口一律放在 core
之外;clause 6.5:`openkal.exec` 已经给了「可选接口在依赖解析期决定」的先例)。

**但现在不该做**,三条理由,都来自这个生态自己的记录:

1. [[openkal-portable-program-findings]]:**两个实现意见一致等于零证据** ——
   同一个作者、同一次阅读。一个从**一个 CLI 的需求**设计出来的网络接口,会把那个
   CLI 的形状(一个本地回调服务器)当成接口的形状。
2. [[second-instance-exposes-the-interface]]:第二个实例才暴露接口是否完整。
   `openkal.net` 需要至少两个环境(比如 Linux 与 一个非 POSIX 环境)各写一份,
   才知道 half-closure、地址族、非阻塞该怎么切。
3. clause 3.2 的不对称性:**放错位置的代价不对称**。接口一旦定下就不能改
   (clause 8:「shall not alter existing ones」;clause 5.3:结构布局不可变)。

⇒ 回复 issue 的正确形状是:**说明范围,而不是承诺路线图**。
openkal 的 README/SPEC 值得增一节「本版不提供什么」,把 socket、pipe、fork、
权限位、符号链接创建列进去,并说明每条是「边界」还是「未决」。
⚠️ SPEC clause 11 已经有「Matters this version does not settle」这一节 ——
**这五条就应该进那一节**,那是它们的既有位置,不需要新机制。

---

## 6. 批次与优先级

| 批 | 内容 | 面 | 依赖 |
|---|---|---|---|
| **P0** | **A2** 编译侧谓词 + `--no-default-config` 无条件化 + `e2e 295` 扩到 `cxxflags` | mcpp | 无 |
| **P0** | **⓪** openkal-musl 的 NULL 跳转定位(不是修,是定位) | openkal-musl | 无 |
| **P1** | **A1** 目标侧使用要求全图化 + `prepare.cppm:7207` 收敛为它的实例 | mcpp | A2(否则两处顺序互相干扰) |
| **P1** | **A3** home 发现收敛到 `mcpp::home::root()` | mcpp | 无 |
| **P2** | **B** `targetHeaderSet` 轴(A1 之后剩下的那一半) | mcpp | A1 |
| **P2** | **C** `[build] private_include_dirs` | mcpp | 无 |
| **P2** | **①** copy_file_range/sendfile、popen 替换、`last_write_time(dir)` | openkal-musl | ⓪ |
| **P3** | **②** README 的「不提供什么」表 | openkal-musl / openkal-linux | ① |
| **P3** | **③** 权限位的静默去掉 + SPEC clause 11 增补五条 | openkal | 需要单独讨论 |
| **不做** | `openkal.net`、`fork`、`pipe2` | — | 见 §5.4 / §5.6 |

⚠️ **A2 排在 A1 前面是有理由的**:A1 往命令行上加 `-I`,而 A2 决定载荷的
`-isystem` 在不在。先加 `-I` 会让「载荷的行还在不在」这个问题被新加的目录遮住,
到时候读不出是哪一层在起作用 —— [[second-copy-of-a-decision-written-without-reading-the-first]]:
**一层盖住一层,修完要看下一层读数。**

⚠️ **P0 那条 mcpp 改动会改变每一个带 `--target` 的构建的编译线。** 按
[[everything-except-docs-goes-through-pr]],七个 openkal 仓库的 CI 都要在
`MCPP_SOURCE_REF` 下现场构建验一遍,判据是 `under review:` 那一行,不是「七个全绿」。

---

## 6.5 通用完整性补充 —— 六条,都不专为这个场景

⚠️ 本章刻意**不**从「让报告者的程序跑起来」出发。判据是:**换一个用户、换一个应用,
这六条是否仍然成立。** 逐条给了这个反问的答案。

### G1 —— 一个层只有一个实现,它就还不是层

`mcpp.targetside` 的架构规则写着「mcpp 硬编码层名,永不硬编码实现」,理由是
「生态的组合是 2×N×M 而包是 2+N+M」。可 `kernel-abi` 这一层今天在 Linux 上
**N = 1**:所有 graph 路线都是 openkal。

两个空缺,都在矩阵里已被记录:

1. **`openkal-gcc-runtime`**(填 ⑤)—— 让 gcc 也能走 graph。矩阵文档已记为生态空缺。
2. **`linux-musl` 的 llvm 实现**(填 ①)—— 一个包,`provides` 只写两层:

   ```toml
   provides = ["mcpp:c-abi=musl", "mcpp:c++-abi=libc++"]
   #  kernel-abi 不由它供给 —— 留空即"传统栈上这层没有名字"
   ```

   ⭐ **引擎已经能表达这个组合,不需要 #492 那种 `isLlvmMusl` 分支。**
   `TargetSide` 的注释为这个形状写好了(*"kernelAbi HAS NO NAME ON A TRADITIONAL
   STACK … 这个字段在 picolibc 裸机构建里读 `—`"*),而 `2026.8.25.1` 把
   `system_from_graph()` 拆成可以单独问 `cAbi.prebuilt()`,正是为了让
   「一层来自图、另一层来自载荷」成为可表达的排布。
   ⇒ **这是包的工作,不是引擎的工作。**

   ⚠️ **未测**:需要真跑一次才能确认那个组合的 flag 路径通(尤其 `-B`/startup
   objects 与 `--gcc-toolchain` 的来源)。#492 撤回时留下的设计文档
   `.agents/docs/2026-08-23-llvm-musl-target-design.md` 里有一半答案。

**换个用户还成立吗?** 成立。任何「要 clang + 静态 musl」的项目今天都只有 openkal
一条路,而这个组合与可移植性无关 —— 它是绝大多数「发一个能拷到任何 Linux 上跑的
二进制」的诉求的形状。

### G2 —— 一个层收窄了它上面那层的面,这件事必须在构建期可读

今天:openkal 的缺席在 **链接期**表达(clause 6.1),这对直接调 `kal_*` 的消费者是
对的。但 C 库**转发**它时,链接期缺席被翻译成了**运行期 ENOSYS** ——
这在分层上是正确的([[c-library-configured-by-what-is-beneath]]:一层用"不存在"
表达的缺席,到上一层变成"一个有定义的错误"),**代价是程序在运行期才发现依赖解析期
就已知的事。**

三个形态,按代价排序:

| 形态 | 改动 | 局限 |
|---|---|---|
| openkal-musl 的 README 增「本 C 库在什么配置下不提供什么」表 | 零 | 要人去读 |
| build.mcpp 的 advisory 通道打一行 | 零引擎、零 spec | ⚠️ **缓存命中不重跑** ⇒ 提示会出现一次就消失([[build-program-advisory-channel]]),不能是唯一载体 |
| C 库声明它重建出来的 POSIX 面,mcpp 在目标侧报告里多打一行 | 新词汇 | **建议先不做** —— 等第二个消费者,理由同 §5.6 |

⚠️ **引擎不得硬编码 POSIX 设施名**(架构规则四),所以这条无论如何都必须由包来说。

**换个用户还成立吗?** 成立,而且更普遍:任何 `Origin::Graph` 的层都可能收窄上层的面,
openkal 只是第一个。

### G3 —— 「能构建」不是「能用」,缺的是一整层验收

- openkal 的 conformance 套件验的是 **openkal 自己的 `kal_*` 面**。
- **没有任何东西验 openkal-musl 重建出来的 POSIX 面。**

⇒ 一套「POSIX 验收」套件,跑在 **每个 backend × 每个 arch** 上,会在用户之前抓住本轮
的 ⓪(NULL 跳转)、①(`copy_file_range`/`popen`/`last_write_time`)、
§5.5(权限静默放宽)—— **五条里的五条**。

⭐ **这是六条里回报最高的一条**:它把「67/94」从**用户的读数**变成 **CI 的读数**。

⚠️ 判据要带分母([[criterion-whose-no-is-also-silence]]):
`67 passed / 94` 是读数,「套件跑过了」不是 —— 一个什么都没发现的套件也会报成功。
⚠️ 且必须**真跑**(qemu),不能只链接:本轮全部三类问题都在链接之后。

**换个用户还成立吗?** 成立。这是「第二个实例才暴露接口是否完整」
([[second-instance-exposes-the-interface]])的验收版本。

### G4 —— 引擎表达力的三个缺口

前两个是本文 §2/§4 的 A1 与 C。第三个是新的,而且最深:

**c. 包的构建不能条件于"已解析的目标侧"。**

`openkal-musl/port/src/okm.h` 自己写下了这条:

> ⚠️ THE TARGET IS A PROXY FOR THE IMPLEMENTATION, AND AN IMPERFECT ONE. …
> **mcpp conditions on the target and not on which package satisfies a
> capability; when it can, this block is where that would be read instead.**

于是 `OKM_HAS_FS/PROCESS/TASK` 只能用 `cfg(os = "none")` 当代理 —— 而代理是错的:
一块板**可以**带真正的文件系统。

⭐ **这条可以做,而且比看上去便宜。已核三点:**

1. 目标侧扫描只读 **manifest 的 `provides` + 已激活的 feature + 已解析的 tc**;
   feature 激活在 `prepare.cppm:5603`,工具链在 `:5218` —— 都早于依赖 build.mcpp(`:6308`)。
2. `directives.cppm` 的 **16 行指令表里没有任何一行能改 `provides`**
   ⇒ build.mcpp 不可能改变扫描的输入。
3. ⇒ **把目标侧解析从 `:6713` 前移到 `:6308` 之前是安全的**,之后每个包的
   build.mcpp 都能被告知"我下面是谁"。

⚠️ **未测**:前移会改变 `tsd::Inputs` 里 `tc` 的成熟度(`tc` 在 `:5218`–`:6713` 之间
被读写过 39 处)。要先枚举哪些字段进了 `Inputs`,再判断它们在 `:6308` 时是否已定。

**换个用户还成立吗?** 成立。「C 库要被它下面那层配置」是分层系统的通例,不是 openkal 的特例。

### G5 —— 头文件集合必须进缓存键与指纹

本文 §3。通用形式:**任何"头文件集合不能由 (编译器, 三元组) 推出"的目标** ——
交叉、显式 sysroot、graph 供给 —— 都暴露在这条上。今天只有 `std` 缓存把它做对了。

**换个用户还成立吗?** 成立,且今天就在生效:§3.2 方向 3 是**两次宿主构建**,连交叉都不是。

### G6 —— 「换编译器」可能同时换掉平台接口,诊断要按两根轴说话

矩阵文档的建议 ③ 已经写了一半:「① 拒绝时指出可用的替代编译器,两向判据」。

⚠️ **另一半没写**:在 `linux-musl` 上,从 llvm 换到 gcc 不只换了编译器,还把
`kernel-abi` 从 openkal 换成了 Linux 直连、`c++-abi` 从 libc++ 换成了 libstdc++ ——
**三样东西一起动了**。一条只说「用 gcc 也能构建这个目标」的建议,会让下一个人
在不知情的情况下跨过同一条缝(方向相反)。

判据:该建议必须同时说出**换过去之后那三行目标侧报告长什么样**。

**换个用户还成立吗?** 成立 —— `x86_64-windows-gnu` 在两张表里 c-abi 一个是 `gnu`
一个是 `musl`,矩阵文档已经把那一格标成「一个目标名,两个 C 库」。

### 优先级:哪些解阻塞,哪些是完整性

| | 解报告者的阻塞 | 通用完整性 |
|---|---|---|
| **G1.2**(llvm-musl 包) | ⭐⭐ 直接解掉 openkal-linux#13 的**全部 27 条** —— 因为不再经过 openkal | ⭐⭐ |
| **G3**(POSIX 验收) | 间接:让剩下的缺陷在 CI 而不是在他那里显形 | ⭐⭐⭐ |
| A1/A2/A3/B(§2/§3) | ⭐ 解 mcpp#514 | ⭐⭐⭐ |
| C(§4) | ⭐ 解 openkal-musl#13 | ⭐⭐ |
| ⓪/①(§5.2/§5.3) | ⭐ 若他继续留在 openkal 上 | ⭐⭐ |
| G2/G4/G6 | — | ⭐⭐ |

⚠️ **G1.2 与 §5 不是二选一。** 即使他换到 llvm-musl 包,openkal 路线仍然要修 ——
它服务的是**另一批用户**(裸机、UEFI、跨平台一份源码),而那批用户不会撞到 `fork`。
⭐ 反过来说:**§5 那些条目之所以被当成"openkal 的缺陷",恰恰是因为一个不需要
可移植性的用户被放到了可移植性抽象上。** 把他放回去之后,那些条目回到它们本来的
优先级 —— 除了 ⓪ 和 §5.5,那两条在任何用户身上都是缺陷。

---

## 6.6 什么可以进 openkal —— 四问

判据不是我提的,`okm_syscall.c:836` 解释 `openkal.random` 为什么必须存在时已经写下了:

> ⚠️ **AND IT IS WHY `openkal.random` HAD TO EXIST. Entropy is not derivable from
> the other interfaces** … Neither bypassing the layer nor inventing entropy was
> acceptable, so the layer gained an interface.

把它和 clause 6.4 / 7.1 合起来,是四问:

1. **只有环境能给吗?** 能由已命名的东西(流 / 任务 / 内存 / 进程)组合出来的,是**库**。
2. **在形态不同的环境里都以可辨认的形式存在吗?** 预设了某个 OS 的模型(用户身份、
   全局路径命名空间、地址空间复制)的,是**那个 OS 的形状**。
3. **是不是「某些资源永远满足不了」的操作?**(clause 6.4)
4. **实现它会不会逼某个环境去构建兼容层?**(clause 7.1)

### 按四问过一遍

| 候选 | 判定 | 理由 |
|---|---|---|
| **symlink** | ❌ 不进 | 文件系统**格式**的属性(FAT 没有,NTFS 语义还不同),不是内核能力。`KAL_FS_PROP_LINKS` 那个属性位是对的;加操作正撞 clause 6.4 |
| **permission / chmod** | ❌ 不进 | 预设**身份模型**。裸机/UEFI 没有用户,Windows 是 ACL,WASI 是能力模型 ⇒ 不通用。⭐ **缺陷是「静默」,不是「缺 chmod」**(见 §6.8 ②) |
| **pipe(进程内)** | ❌ 不进 | = ring buffer + `okm_task_wait/wake`,而这两个已经有(`okm_opt.h:135`,端口的 `SYS_futex` 就走它)⇒ **可导出 ⇒ 是库** |
| **pipe(跨到 spawn 的程序)** | ⚠️ 候选,但**不是现在** | 只有环境能做。⭐ 但它不是 `pipe2`,而是 `kal_process_spawn` 缺「给我一个连到子进程 stdout 的流」—— **已有接口的一次加法**(clause 8 允许)。而临时文件已能覆盖绝大多数用法(§5.3b),⇒ 等一个临时文件解决不了的用例 |
| **net** | ✅ 通过,**不是现在** | 网络栈确实不可由已有接口导出;clause 3.4 已点名 `openkal.net`。但 clause 8 定死后不能改 ⇒ 不该由一个程序的需求定形(§5.6) |
| **就绪/多路复用(poll)** | ⚠️ **新候选,见 §6.8 ①** | 这一条比上面任何一条都更接近「只有环境能给」,而且它今天一个都没有 |

⭐ **结论:上一版列的四个可选接口里,三个不该进,第四个不该现在进。**

### 6.6.1 ⭐⭐ 一条更硬的判据,它替换上面四问的**范围结论**

上面四问要人去判断「是不是通用内核能力」—— 而那是一次判断,判断会错。把判断交给
**已经存在的可移植程序**:

> **传统体系下已经可移植的程序用到的东西,openkal 体系必须有。**
>
> 否则 openkal 让这些程序**更不可移植**,而不是更可移植 —— 它们在 Windows/macOS/Linux
> 上靠三条 `#ifdef` 分支就能走,到了 openkal 上没有第四条可写。

这条判据是从 openkal 自己的目的推出来的,不是外加的:openkal 存在是为了让一份源码
到达多台机器。**一个已经到达三台机器的程序,不该在第四台上失去能力。**

| 设施 | Windows | macOS | Linux | 可移植程序已经在用? | ⇒ openkal |
|---|---|---|---|---|---|
| 网络 socket | ✅ Winsock | ✅ | ✅ | ✅ **`mcpplibs/tinyhttps@0.2.8` 就是**(`#ifdef _WIN32` 两支) | ✅ **该有** |
| 就绪等待 | ✅ `WSAPoll` | ✅ | ✅ | ✅ 同上(它用 `poll`) | ✅ **该有** |
| 管道 + 子进程输出 | ✅ `CreatePipe`/`CreateProcess` | ✅ | ✅ | ✅ | ✅ **该有**(spawn 那半个已经有) |
| 终端控制(raw / 尺寸) | ✅ Console API | ✅ termios | ✅ | ✅ **ftxui 就是** | ✅ **该有** |
| **`fork`** | ❌ **没有** | ✅ | ✅ | ❌ 用了它的程序**本来就不可移植** | ❌ **不该有** |
| **mode 位 / `chmod`** | ❌ 只有只读位 | ✅ | ✅ | ❌ | ❌ **不该有** |
| **symlink** | ⚠️ 要权限 | ✅ | ✅ | ⚠️ 标准库把它设计成**允许失败** | ❌ **不该有** |
| `copy_file` | 标准库层面已可移植 | | | ✅ | ❌ 端口层做 |

⚠️ **这张表与 §6.6 四问的结论不同,差在两处,而且是这条判据赢:**

1. 四问把 `net` 判成「通过但等第二个消费者」。这条判据说**它已经有第二个消费者了**
   —— mcpp 自己索引里的 `tinyhttps`,它的两支 `#ifdef` 就是两个独立实现,
   而且不是同一个作者读同一份规范写出来的
   ([[openkal-portable-program-findings]] 那条「两个实现一致等于零证据」在这里**不适用**)。
2. 四问完全没看到**就绪原语**与**终端控制**。这条判据把它们提到与 net 同级,
   而它们比 net 小得多。

⭐ **推论:`__config_site` 那份开关清单本身就是一份现成的核对表**
(`_LIBCPP_HAS_TERMINAL` / `_LIBCPP_HAS_FILESYSTEM` / `_LIBCPP_HAS_THREADS` / …)——
libc++ 已经替我们列出了「一个标准库需要下层提供什么」,而 §6.7 表 C 显示今天有两行
与现实不符。**核对它,比逐条讨论接口更该先做。**

### 6.6.2 ⭐⭐ 「永远不会有」与「这个后端没有」是两件事,今天用同一个答案回答

`okm.h` 论证 ENOSYS 正当性的那段是对的 —— 但它论证的是**第二件事**:

> A C library may [answer unsupported], because POSIX's surface HAS one — ENOSYS
> — and every caller of `open` already handles a failure.

`open` 在一个 core-only 后端上失败,是**接口存在而这个后端不提供** ⇒ ENOSYS 正确,
`okm_opt.h` 那道缝处理得也正确。

但 `fork` / `socket` / `poll` / `chmod` 是**另一件事:openkal 根本没有这个接口,
因此没有任何后端会提供它**。对这一类,ENOSYS 把一个**永久的、与后端无关的**事实
伪装成了一个运行期条件。

⭐ **正确的答案是链接错误**,而这正是 clause 6.2 的表已经规定的:

| 时机 | 机制 | 回答什么 |
|---|---|---|
| 链接 | 未定义符号 | 是否用了它**不提供**的接口 |

⇒ **openkal-musl 应当把这些 POSIX 面从构建里排除掉**,让引用它们的程序链接失败并
被指名。

**机制全部现成,是一次 manifest 编辑,没有新机器:**

- `mcpp.toml` 的 `sources` 已经有 `!` 排除语法,**今天已经排除了 10 个 musl 源**
- cflags 已有 `-ffunction-sections -fdata-sections`,ldflags 已有 `-Wl,--gc-sections`
  (`mcpp.toml:240`)⇒ **没被引用的不会失败,被引用的才失败**,粒度正好

**效果**:`tinyhttps` 在 openkal 上**链接失败并指名 `socket`**,而不是构建成功、
测试跑到第 27 个才知道。⭐ 这把发现从**运行期**移到**链接期**,零设计成本。

⚠️ **未测**:排除集合的**闭包**要跑一轮才能定 —— musl 的 `network/` 内部互相引用
(`getaddrinfo` → `socket`),排一个可能牵出一串。这是实现问题,不是设计问题。

---

## 6.7 能力支持表(实测)

⚠️ 判据全部来自源码与磁盘,不是推的。每张表标了取数方式。

### 表 A —— openkal 接口 × 实现(取自各仓库 `src/` 的文件集合 + `provides`)

| backend | abort | stream | memory | env | time | fs | process | task | random | exec |
|---|---|---|---|---|---|---|---|---|---|---|
| openkal-linux 0.5.4 | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ |
| openkal-macos | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| openkal-windows | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | ✅ | — |
| openkal-opensbi | ✅ | ✅ | ✅ | ✅ | ✅ | — | — | — | — | — |
| openkal-uefi | ✅ | ✅ | ✅ | — | — | — | — | — | — | — |

⭐ **openkal-linux 是完整的 —— 十个接口一个不缺。** 所以本轮的缺口**不在 backend**,
在**接口集合本身**。这条读数很重要:它把「让 openkal-linux 更完整」这条路排除掉了。

### 表 B —— openkal-musl 重建出来的 POSIX 面

判据:`port/src/okm_syscall.c` 的 `case SYS_*` 集合(**71** 个)对
`musl/src/**/*.c` 引用到的 syscall 集合(**289** 个)。⭐ 分母在这里。

| 设施 | 状态 | 判据 |
|---|---|---|
| 文件 I/O、目录、重命名、删除 | ✅ | `open/openat/read/write/lseek/getdents64/mkdirat/renameat2/unlinkat` 均在表 |
| stat / 时间戳 | ✅ | `stat/fstat/newfstatat/statx/utimensat` 在表 |
| 线程、futex、TLS | ✅ | `okm_thread.c` 直接走 `kal_task_*`;`SYS_futex → __okm_futex` |
| 匿名内存 | ✅(受限) | `mmap` 仅 `MAP_ANON\|MAP_PRIVATE` 且 `addr==0 && fd<0`,否则 `-ENOSYS` |
| 时间、睡眠 | ✅ | `clock_gettime/nanosleep/clock_nanosleep` 在表 |
| 起子进程 + 等待 | ✅ | `okm_spawn.c` 替换了 `posix_spawn.c`;`wait4` 在表。⇒ **`system()` 可用** |
| 熵 | ✅ | `SYS_getrandom → kal_random_fill`(弱引用 + 判空) |
| **`fork` / `clone` / `vfork`** | ❌ ENOSYS | 不在表 —— clause 7.1 的**设计边界** |
| **pipe / pipe2 / mkfifo** | ❌ ENOSYS | 不在表 |
| **poll / ppoll / select / pselect6** | ❌ ENOSYS | 不在表 —— 见 §6.7 ① |
| **epoll / eventfd / signalfd / timerfd** | ❌ ENOSYS | 不在表 |
| **socket 全族** | ❌ ENOSYS | 不在表 |
| **chmod / fchmodat / chown** | ❌ ENOSYS | 不在表 |
| **symlink / symlinkat** | ❌ ENOSYS | 不在表;`readlinkat` 在表但对 `kal_node_link` 返回 `-ENOSYS` |
| **copy_file_range / sendfile** | ❌ ENOSYS | 不在表 |
| **`umask`** | ⚠️ **成功且无效** | `g_umask` 只被 `SYS_umask` 自己读写,**从不作用于任何创建**(`okm_syscall.c:388/937`) |
| **终端控制** | ⚠️ 只答"是不是终端" | `ioctl` 仅 `TCGETS` 返回 0;`TCSETS`/`TIOCGWINSZ` → `-ENOTTY` |
| **信号 handler** | ❌ ENOSYS(**诚实**) | `rt_sigaction` 对 `SIG_DFL/SIG_IGN` 返 0,真实 handler 返 `-ENOSYS`(`:969-970`) |
| **uid/gid** | ⚠️ 虚构 | `getuid/geteuid/getgid/getegid` 一律返回 **1000**(`:935`) |

### 表 C —— libc++ 的特性声明 vs 实际(读 `openkal-llvm-runtime/llvm-generated/*/__config_site`)

| 开关 | generic | freestanding | 实际 |
|---|---|---|---|
| `_LIBCPP_HAS_THREADS` | 1 | 1 | ✅ 真的有 |
| `_LIBCPP_HAS_MONOTONIC_CLOCK` | 1 | 1 | ✅ |
| `_LIBCPP_HAS_RANDOM_DEVICE` | **1** | 0 | ✅ **0.1.3 已修**(报告者测的是 0.1.1) |
| `_LIBCPP_HAS_FILESYSTEM` | 1 | 0 | ⚠️ **部分** —— symlink / copy_file / permissions 运行期 ENOSYS |
| `_LIBCPP_HAS_TERMINAL` | 1 | 0 | ⚠️ **不符** —— 见表 B 终端控制 |
| `_LIBCPP_PSTL_BACKEND_STD_THREAD` | 是 | 是 | ⚠️ 线程有,**就绪原语一个没有** |
| `_LIBCPP_HAS_LOCALIZATION` / `UNICODE` / `WIDE_CHARACTERS` / `TIME_ZONE_DATABASE` | 1 | 1 | **未测** |

⭐⭐ **这张表本身就是一个缺口的证据:`__config_site` 是一份对下层能力的声明,而
今天没有任何东西核对它。** 三行里有两行与实际不符,而它们都是**编译期**就写死的
—— 程序因此把 `<filesystem>` 和终端支持整个编了进去,再在运行期一条一条撞 ENOSYS。

⇒ **回答「能力是不是不完整」:是,而且不完整的位置不在 backend(表 A 满),
在接口集合(表 B)与声明和现实的落差(表 C)。**

---

## 6.8 其他问题(报告者未列,本轮实测发现)

**① ⭐⭐ 就绪 / 多路复用原语一个都没有。**
`poll`/`ppoll`/`select`/`pselect6`/`epoll_*`/`eventfd2`/`signalfd4`/`timerfd_*`/`pipe2`
**全部** ENOSYS(逐个查过 `case SYS_*`)。⇒ **任何事件循环都建不起来。**
这比"没有 socket"更根本 —— 一个不联网的 TUI 也要等"输入就绪或超时"。
⭐ 按 §6.6 的四问,这一条最接近「只有环境能给」:等待多个来源之一变为就绪,
不能由 `kal_stream_read`(阻塞、单源)组合出来。**如果 openkal 只增一个接口,
候选应该是它,而不是 net。**
⚠️ **未测**:是否有环境无法提供它(裸机上"就绪"是否有意义)。这决定它是不是通用能力。

**② ⭐⭐ `umask()` 成功且无效。** `g_umask` 只被自己读写,从不作用于任何创建
(`kal_fs_open` 也没有 mode 参数)。⇒ `umask(077)` 返回旧值、报成功、**下一次创建
不受影响**。这正是 `okm_opt.h` 开篇明令禁止的那一种:
*"nothing below reports SUCCESS having done nothing"*。
⭐ **它比 §5.5 那条更硬** —— §5.5 要论证"0600 被放宽"的具体值,这条不需要:
**一个有返回值的调用,它的效果不存在。**

**③ ⭐ 终端控制不存在,而 `KAL_STREAM_PROP_INTERACTIVE` 这个位存在。**
`ioctl` 的注释自己写着:*"Everything a terminal can be asked to do beyond that is
not an operation openkal has."* ⇒ TUI 进不了 raw mode、拿不到窗口尺寸。
⚠️ **未测**:这与 §5.2 的 PC=0 是否相关。但"早期终端 UI 初始化"这个位置与它重合,
**定位 PC=0 时应当先排除这一条**。

**④ `sigaction` 是诚实的**(我一度以为它静默吞掉,读源码后是我错了):真实 handler
返回 `-ENOSYS`。⇒ SIGWINCH / SIGINT 不可用,但程序会被告知。

**⑤ `getuid` 族返回硬编码 1000。** 一个被虚构的身份。⚠️ 与 ② 合起来,任何做
"按权限/属主判断"的代码都在一个虚构的世界里运行,而且**全部报成功**。

---

## 6.9 综合改动清单:改动量、影响面、通用性、风险

⚠️ 改动量是**量级估计**,不是实测行数。影响面按"会不会改变现有构建的命令行/产物"分。

### mcpp 引擎侧

| # | 改动 | 文件 | 量级 | 影响面 | 通用性 | 风险 | 判据 |
|---|---|---|---|---|---|---|---|
| **A2** | 编译侧读 `targetSide`,`--no-default-config` 无条件化 | `hostflags.cppm` + `flags.cppm` + 3 处调用方 | ~40 行 | ⚠️ **改变每一个带 `--target` 的构建的编译线** | ⭐⭐⭐ 与 openkal 无关 | ⚠️ **中** —— 三个调用方必须同批给值,否则 `std.pcm` 与导入者头文件不一致 | e2e 295 扩到 `cxxflags`(恒等式) |
| **A1** | 目标侧使用要求全图化 + `:7207` 收敛 | `prepare.cppm` | ~60 行 | 只影响 graph 供给目标侧的构建 | ⭐⭐⭐ | 低 —— 追加+去重,现有边不动 | §2.2 的 20 行工程 |
| **A3** | home 发现收敛到 `mcpp::home::root()` | `xlings.cppm` | ~15 行 | ⚠️ 可能让某些机器上 `linux-headers` 从"找到别人的"变"找不到" | ⭐⭐⭐ 密闭性 | 低(失败会说话) | §7 ① |
| **B** | `targetHeaderSet` 轴 | `cache_key.cppm` + `prepare.cppm` | ~50 行 | **全部缓存条目 miss 一次**(不失效) | ⭐⭐⭐ | 低 —— 不 bump epoch,`inputs_match` 天然拒旧 | `test_cache_key.cpp` |
| **C** | `[build] private_include_dirs` | `types/toml/prepare` | ~80 行 | 新键,老 mcpp 的行为待测 | ⭐⭐ | ⚠️ **中** —— 见 §4.2 的 floor 问题 | 单测 + e2e |
| **G4c** | 目标侧解析前移到依赖 build.mcpp 之前 | `prepare.cppm` | ~30 行(移动) | 依赖 build.mcpp 的环境多几个变量 | ⭐⭐⭐ | ⚠️ **中高** —— `tc` 在 `:5218–:6713` 间被读写 39 处,前移要逐字段核 | 未定 |

**引擎侧合计**:6 处、~275 行、4 个文件为主。⭐ **其中 A1/A2/A3/B 四条与 openkal
无关**,是任何交叉/graph 目标都吃的正确性问题。

### openkal 生态侧

| # | 改动 | 仓库 | 量级 | 影响面 | 通用性 | 风险 |
|---|---|---|---|---|---|---|
| **⓪** | 定位并修 PC=0 | musl 或 linux | 未知 | 全部程序 | ⭐⭐⭐ | 先定位,**不猜** |
| **①a** | `copy_file_range`/`sendfile` | musl port | ~40 行 | `std::filesystem::copy_file` | ⭐⭐⭐ | 低 |
| **①b** | 替换 `popen.c`(临时文件流) | musl port | ~120 行 | shell out | ⭐⭐ | ⚠️ 语义差三处,必须写进 README |
| **①c** | 进程内 `pipe2`(ring + task_wait) | musl port | ~150 行 | 自管道/线程唤醒 | ⭐⭐ | ⚠️ **与 spawn 的会合是本轮唯一要设计的地方** |
| **①d** | `last_write_time(dir)` 走 `"."` | musl port | ~20 行 | `std::filesystem` | ⭐⭐⭐ | 低 |
| **②** | 去掉 `umask` 的静默 | musl port | ~10 行 | 权限语义 | ⭐⭐⭐ | 低 —— 最小是删掉 `g_umask`,让它也 ENOSYS |
| **C'** | 用 `private_include_dirs` 收掉 3 个内部目录 | musl | ~5 行 | 所有消费者 | ⭐⭐ | 依赖引擎 C |
| **③** | clause 11 增补:net / pipe / 权限 / symlink / 终端 / 就绪 | openkal SPEC | 文档 | — | ⭐⭐⭐ | 零 |
| **④** | POSIX 验收套件 | musl + CI | 大 | — | ⭐⭐⭐ | 零(只增读数) |

⭐ **注意:生态侧没有一条是「加 openkal 接口」。** 按 §6.6 的四问过完之后,
27 条里除 socket 外全部落在端口层。

### 架构与稳定性评估

| 维度 | 评估 |
|---|---|
| **架构方向** | ⭐ 全部改动都在**收敛**方向:A1/A2 把"同一个决定的第二处推导"删掉,A3 把 home 的第四份拷贝删掉,C 让包能说出它一直想说的话。**没有一条是加机制。** |
| **对既有生态的冲击** | A2 最大(改每个 `--target` 构建的编译线)。⚠️ 七个 openkal 仓库必须在 `MCPP_SOURCE_REF` 下现场构建验一遍 |
| **可回退性** | A1/A3/B/C 都是加法或收敛,回退=revert。⚠️ **A2 不是** —— 它会让一些今天"靠 cfg 文件恰好能编"的构建变成显式,**这些构建在 A2 之后可能需要真的声明它们的依赖** |
| **稳定性净收益** | ⭐⭐ B 消除的是**静默错产物**(混合 BMI 让 clang 前端 SIGSEGV);② 消除的是**静默错权限**。这两条的当前状态都是"报成功而结果是错的",属于最坏的一类 |
| **通用性** | 引擎六条里五条与 openkal 无关;生态九条里七条与本报告者的具体应用无关 |

### 能不能解决报告者的问题,以及怎么解决

| 他的 27 条失败 | 修完之后 | 靠哪一条 |
|---|---|---|
| `fork` | ❌ 仍然不行 | **应用改用 `posix_spawn`**;`system()` 今天就能用 |
| 子进程输出捕获 | ✅ | ①b |
| `pipe2`(自管道) | ✅ | ①c |
| socket / 本地 HTTP 回调服务 | ❌ 仍然不行 | 需要 `openkal.net` —— **明确列为不解决** |
| 事件循环(poll) | ❌ 仍然不行 | §6.8 ①,**新候选接口** |
| `chmod` / 权限 | ⚠️ 从"静默错"变成"明确失败" | ② |
| `symlink` | ⚠️ 保持 ENOSYS,但被文档化 | ③ |
| `copy_file` | ✅ | ①a |
| `last_write_time(dir)` | ✅ | ①d |
| 终端 UI 崩溃 | ⚠️ **取决于 ⓪ 的定位结果** | ⓪(+ §6.8 ③) |
| `std::random_device` | ✅ **已解决** | 0.1.3 |

⭐⭐ **结论:走 openkal 路线,他的应用仍然跑不起来 —— 因为 socket 与 poll 两条不在
可修范围内,而它们是那个应用的核心(本地回调服务 + 终端事件循环)。**

⇒ **能解决他问题的只有两条路,而且都不是"修缺陷":**

1. **openkal 增 `net` + 就绪原语两个接口** —— 按 §6.6 的四问,`net` 通过但需要
   第二个消费者定形;就绪原语是**本文新提的候选**,未测其通用性。
2. **§1.5.2 那张矩阵的反对角线** —— 给 llvm 一条 payload musl 路线。他的应用不改一行。

⚠️ **两条都不该由这一个应用的需求来推动决定。** 而本文列的 15 条改动,无论选哪条路
都要做 —— 它们修的是**引擎的正确性**与**端口层已经能表达却没表达的东西**,
与路线选择无关。

---

## 6.10 结论:三侧各做什么

### 6.10.0 判据的最终形态

三条判据在讨论中依次替换,**最后一条是规范自带的**,前两条只作为它的快速筛:

| | 判据 | 地位 |
|---|---|---|
| 四问 | 「是不是通用内核能力」 | ⚠️ 要人判断,**判错过两处**(漏掉就绪与终端;把 net 判成"等第二个消费者") |
| 传统体系已可移植 | 「三个平台上可移植程序已经在用的,openkal 必须有」 | ⭐ 好用的筛,但**会把 `poll` 误判进来**(可移植程序用它是因为 POSIX 给了,不是因为不可约) |
| **clause 10 基数 + 原子性** | 「一个程序里几个实现?」+「能不能由已命名的东西组合出来?」 | ⭐⭐ **规范自带,且两处冲突都由它裁决** |

加上一条形状约束,它决定接口**长什么样**而不只是**有没有**:

> ⭐⭐ **openkal 的接口是面向内核的通用原子能力,不与任何一个内核的形状绑定。
> 组合由上层做。**

这条不是新增的原则 —— clause 3.4 已经在用它:*"The stream is therefore the shared
currency of the specification and **not its common entrance**"*,以及它拒绝那个
「把名字映射到资源」的统一接口时给的三条理由。

### 6.10.1 openkal(spec)—— 三个接口,而且都要按原子性重新定形

⚠️ **每一条的形状都不是它在 POSIX 里的形状。** 这是本节的要点。

#### ① `openkal.net`(大)—— 与 `openkal.fs` 同构,但**不是 BSD socket**

规范里的依据全部现成:
- clause 10:NIC(openhal,多个)→ 网络栈(openkal,一个),与 块设备 → `openkal.fs` **同构**
- clause 3.4:已经点名 `openkal.net`,并指出它与 fs 的差别正是 **half-closure**
- clause 3.4:拒绝解析无边界的名字方案(⇒ **DNS 不在里面**)

⇒ 原子面大约是**六个操作**,而不是 BSD 的三十个:

```
kal_net_connect(endpoint)      → stream        建立到一个端点的连接
kal_net_listen(endpoint)       → listener      接受入站
kal_net_accept(listener)       → stream
kal_net_shutdown(stream, dir)                  半关闭 —— clause 3.4 点名的那件事
kal_net_close_listener(listener)
kal_net_props                                  能力字
```

**不在里面**(每一条都是 POSIX 的形状而不是内核的原子能力):
地址族与 `sockaddr` 家族、`setsockopt` 的选项空间、非阻塞标志(就绪归端口层)、
`sendmsg/recvmsg`、**名字解析**。
⇒ 端点是**结构化的地址+端口**,不是字符串;`getaddrinfo` 是端点之上的库。

⚠️ **两处未决,必须在动笔前定,不能边写边定:**
1. **数据报要不要?** 流与数据报是两种东西(clause 6.4 的形状)。若 v1 只做流,
   则 DNS 只能走 TCP —— 这是一个真实的连锁后果,不是细节。
2. **谁来定形?** clause 8 规定接口一旦发布不可更改。⭐ 按
   [[second-instance-exposes-the-interface]],**需要两个形态不同的实现**
   (一个宿主内核 + 一个 BSP over openhal)才知道分解对不对。

#### ② `openkal.terminal`(小)—— 按 clause 6.4 独立成接口

「raw 模式」**在不同资源上表现不同**(终端 vs 普通文件),这正是 clause 6.4 裁定
`seek` 属于 `openkal.fs` 而非 `openkal.stream` 的那个形状。⇒ 独立接口,资源是一个
交互式流,入口是已有的 `KAL_STREAM_PROP_INTERACTIVE`。

原子面约 **2 操作 + 1 询问**:「不要解释我的输入」(raw)、「不要回显」、「显示多大」。
**不是 `termios`** —— 那是一个带六十个标志位的 POSIX 结构。

裸机上:没有行编辑 ⇒ 关掉它是**无事可做即完成**(`okm_opt.h` 已有这类先例);
尺寸报「不知道」。

#### ③ `openkal.process` 的一次加法(小)—— 跨 spawn 边界的通道

`kal_spawn_streams` 已经收流句柄,缺的是**取得一个跨得过 spawn 边界的流**。

```
kal_process_channel(void) → (parent_stream, child_stream)
```

⭐ **比「给我子进程的 stdout」更原子**:调用方自己决定把 `child_stream` 装到子进程
的哪一个流上,于是 `popen("r")`、`popen("w")`、双向捕获都由上层组合出来 —— 正是
「上层自己组合」。

⚠️ **进程内的**流对不属于这里(可由缓冲 + `kal_task_wait/wake` 导出 ⇒ 是库)。
这条的存在理由**只有**「跨地址空间」。

#### ④ 不进 openkal 的,以及为什么(记录下来,免得再讨论一次)

| | 为什么不进 |
|---|---|
| `fork` | clause 7.1;且 Windows 没有 ⇒ 用它的程序本来就不可移植 |
| `pipe`(进程内) | 可由 `kal_task_wait/wake` + 缓冲导出 ⇒ 库 |
| **就绪 `poll`/`select`** | ⭐ 同上,可由 task+stream 导出。**「传统体系已可移植」那条判据在这里判错了** |
| `chmod` / mode 位 | 预设身份模型;Windows 只有只读位 ⇒ 不通用 |
| `symlink` | 文件系统**格式**的属性;`std::filesystem` 本来就设计成允许失败 |
| DNS / `getaddrinfo` | clause 3.4 明确拒绝「解析无边界的名字方案」⇒ 端点之上的库 |

#### ⑤ 文档(零代码)

clause 11「本版不解决」增补:网络、就绪、终端、权限、符号链接、进程复制 —— 六条,
每条说明是**边界**(fork/权限/symlink)还是**未决**(net/terminal/channel)。

### 6.10.2 openkal 生态(实现层)

| # | 内容 | 仓库 | 量级 |
|---|---|---|---|
| ⓪ | **定位并修 PC=0** | musl / linux | 未知,**先定位** |
| ① | `copy_file_range`(**只需这一个**,见下) | musl port | ~40 行 |
| ② | 替换 `popen.c`(临时文件流;`channel` 落地后改用它) | musl port | ~120 行 |
| ③ | `last_write_time(dir)` 走 clause 7.12 的 `"."` | musl port | ~20 行 |
| ④ | **`umask` 去静默** —— `g_umask` 只被自己读写,删掉它让它也 ENOSYS | musl port | ~10 行 |
| ⑤ | ⭐ **「永远没有」变链接错误**:排除 `net/`、`fork.c`、`pipe*.c` 等 | musl **manifest** | 编辑 + 一轮闭包实测 |
| ⑥ | **就绪 `poll`/`select`**(含预读缓冲)—— 端口层设计,不动 spec | musl port | 大,**需设计** |
| ⑦ | 用 `private_include_dirs` 收掉三个 musl 内部目录 | musl | ~5 行,等 mcpp |
| ⑧ | ⭐ **POSIX 验收套件**(每 backend × 每 arch,真跑) | musl + CI | 大,**回报最高** |
| ⑨ | `__config_site` 与实际能力**对账** | llvm-runtime | 见下 |

**关于 ①**:libc++ 的 `copy_file` 对 `copy_file_range` 的回落名单**含 ENOSYS**
(`operations.cpp:314`),对 `sendfile` 的**只认 EINVAL** ⇒ 实现
`copy_file_range` 一个就够,两个都做是多余的。

**关于 ⑨**:`_LIBCPP_HAS_TERMINAL 1` 与 `_LIBCPP_HAS_FILESYSTEM 1` 今天与实际不符
(§6.7 表 C)。⭐ **在 `openkal.terminal` 落地前,`HAS_TERMINAL` 应当是 0** ——
声称有然后运行期失败,比声称没有更糟。

### 6.10.3 mcpp(引擎)

与 openkal 的路线选择**完全无关**,无论 openkal 怎么长都要做:

| # | 内容 | 量级 | 关键风险 |
|---|---|---|---|
| **A2** | 编译侧读 `targetSide`;`--no-default-config` 无条件化 | ~40 行 | ⚠️ 改每个 `--target` 构建的编译线;三个调用方同批 |
| **A1** | 目标侧使用要求全图化;`:7207` 收敛为它的实例 | ~60 行 | 低 |
| **A3** | home 发现收敛到 `mcpp::home::root()`;删 `~/.xlings` 回落 | ~15 行 | 低 |
| **B** | 缓存键增 `targetHeaderSet` 轴 | ~50 行 | 低,不 bump epoch |
| **C** | `[build] private_include_dirs` | ~80 行 | ⚠️ 新键的 floor 待测 |
| **G4c** | 目标侧解析前移到依赖 build.mcpp 之前 | ~30 行 | ⚠️ `tc` 39 处读写,逐字段核 |
| **D**(新) | ⭐ 目标侧报告增一行:**由图供给的层收窄了上层的面时说出来** | 未定 | 引擎不得硬编码 POSIX 名 ⇒ 由包说 |

### 6.10.4 应当提示报告者做什么

⭐ **给可判定的答复,不是"我们在看"。** 他的 27 条分三类,三类的处置不同:

**第一类 —— 我们修,他不用改代码(5 条)**
`copy_file` · `last_write_time(dir)` · 子进程输出捕获 · `umask` 语义 · `random_device`(**0.1.3 已修**,他测的是 0.1.1)

**第二类 —— 他要改,而且在传统体系里也该改(3 条)**

| 他现在写的 | 改成 | 为什么这不是将就 |
|---|---|---|
| `fork` | `posix_spawn` / `std::system` | ⭐ **Windows 没有 `fork`** —— 这份代码在传统体系里也不可移植 |
| `std::filesystem::create_symlink` 假定成功 | 失败则降级 | Windows 上没有权限时同样失败;标准把它设计成可失败 |
| `permissions()` 假定成功 | 同上 | Windows 只有只读位 |

**第三类 —— 今天没有绕法,要等接口(2 条)**
终端 UI(等 `openkal.terminal` + 端口层就绪)· 本地 HTTP 服务(等 `openkal.net`)

⇒ **在这两条落地前,他的程序在 openkal 上跑不完整。这不是缺陷,是范围。**
他有三个知情选择,而**今天他一个都看不见**:

1. 等第三类接口(时间由 spec 定形决定,不由缺陷修复决定)
2. 把这两处换成 openkal 面上能表达的(不现实)
3. 走 §1.5.2 矩阵的另一格(payload musl),放弃跨平台换取立即可用

⭐ **让这三个选择可见,是 6.10.2 ⑤ 那一条(链接错误)零成本做到的** ——
`tinyhttps` 会在链接期被指名,而不是跑到第 27 个测试。

**他能贡献的两样,他自己已经提过:**
- ⭐ **94 个 POSIX 用例** —— 正是 6.10.2 ⑧ 缺的那一层的现成种子
- 双架构 musl libc++ 制品 —— 若走 §1.5.2 那一格,这是缺的那一块

---

## 7. 定案用的四条命令

三条待测项,每条给出**能给出否定读数**的形态。

**① §2.4 的 fast-path 重放假说**(未测):

```sh
# 同一个工程，两个 home，只看它是否重新 prepare
MCPP_HOME=$A mcpp build --target x86_64-linux-musl
grep -c "$A" target/*/*/build.ninja          # 期望 >0
MCPP_HOME=$B MCPP_VERBOSE=1 mcpp build --target x86_64-linux-musl 2>&1 | grep "scanning module sources"
grep -c "$A" target/*/*/build.ninja          # 若仍 >0 且上一行为空 ⇒ 重放了 $A 的图
```

⚠️ 判据是**两条同时成立**:没有 `scanning module sources`(说明没重新 prepare)
**且** `build.ninja` 里仍是 `$A` 的路径。只看其中一条,两种不同的机制会同读数。

**② §3.2 方向 2**(dep 缓存跨目标复用):

```sh
mcpp build --target x86_64-linux-musl --cache global
mcpp build --cache global
ls -d ~/.mcpp/build-cache/v1/pkg/<ns>/<pkg>@<ver>/*/ | wc -l   # 期望 2；若为 1 ⇒ 报告成立
python3 -c "import json,glob;[print(f, json.load(open(f))['inputs']['toolchain']['target_triple']) for f in glob.glob('...entry.json')]"
```

⚠️ 判据是**条目目录数 + 每个条目里的 `target_triple`**,不是 CLI 的 `Cached` 行。

**③ ⓪ NULL 跳转**:

```sh
gdb --args ./prog
(gdb) run
(gdb) info registers rip rsp
(gdb) x/8gx $rsp            # 返回地址在这里，bt 空的时候它不空
(gdb) info symbol <上面读到的地址>
readelf -x .init_array ./prog | grep -c ' 00000000 00000000'
```

**④ A-2 的恒等式**(应当加进 `e2e 295`):

```sh
diff <(tr ' ' '\n' <implicit.cxx | grep -v '^--target=' | sort) \
     <(tr ' ' '\n' <explicit.cxx | grep -v '^--target=' | sort)
```

---

## 8. 我可能错的地方

按这个仓库的惯例,把不确定的都列出来:

1. **§2.4 的 fast-path 假说未测。** 报告里那条现象与 §2.3 的实测冲突,我给的解释
   是三个候选里最可能的一个,但没有读数。§7 ① 是判据。
2. **§3.2 方向 2 我判它「按字面不成立」,依据是磁盘上的 `entry.json` 含
   `target_triple` + `prepare.cppm:2480` 会改写它。** 如果报告者的宿主构建也带
   `--target`,或者他说的是 `std` 缓存而不是 dep 缓存,结论要改。§7 ② 是判据。
3. **A1 的顺序判断(追加在末尾)是从「驱动默认目录总在最后」推的,未测。**
   openkal 那 18 个目录里若有需要压过包自己头文件的(例如
   `__config_site`),顺序要反过来。**落地前必须在真 openkal 工程上比对
   `-E -v` 的搜索列表**,不能只看编过了。
4. **§5.5 报告的「文件 0600 落成 0777」我复现不出机制** ——
   `openat(...,0666)` 给不出 0777。可能是报告者量的是别的东西(比如
   `permissions()` 失败后回读默认),也可能进程 umask 为 0 且量的是目录。
   **权限被放宽这件事成立;放宽到哪个值未定。**
5. **§4.2 的 `private_include_dirs` 是新 `[build]` 键。**
   按 [[new-capability-key-floor-measured]],发布前必须实测**索引 latest** 的
   mcpp 对不认识的 `[build]` 键是忽略还是硬失败 —— 这两种行为决定了
   openkal-musl 能不能在下一版就用上它。
6. **我没有真的跑过一次 openkal + musl 的七成员工作区。** 本文的 A1/A2 是在
   20 行的等价工程上实测的;它们复现了同一个机制,但没有复现报告者的规模。

---

## 9. 相关记忆

[[recorded-field-with-no-decision-reader]](答案已解析却没接到决定上)·
[[second-copy-of-a-decision-written-without-reading-the-first]](一层盖住一层)·
[[criteria-should-not-be-substring-searches]](判据不该是子串搜索)·
[[dep-build-cache-scoping]](缓存假状态行骗了三个月)·
[[dep-bmi-cache-cross-version-poisoning]](根因写错过两次)·
[[c-library-configured-by-what-is-beneath]](判据用错了「语言」)·
[[link-error-is-the-mechanism-not-the-defect]](链接错误是机制不是缺陷)·
[[openkal-portable-program-findings]](两个实现一致等于零证据)·
[[graph-decides-and-dep-fingerprint-gap]](依赖的 `[build]` 从没进指纹)
