# 20 —— 工具链管理

**读者:**在意「哪个编译器在跑」的人 —— 版本下界、第二套工具链,或者一台不许用
自带编译器的机器。

**本章回答的那一个问题:**这个工程会被哪个编译器构建、它怎样被安装、以及怎样换成
另一个。

**不在这里:**这套解析是怎么实现的,那是 [91 —— 工具链机制内幕](91-toolchain-internals.md);
以及目标怎么命名,那是 [21 —— 目标三元组](21-the-target-triple.md)。

> mcpp 维护一个独立的工具链沙盒,与系统 PATH 完全隔离。

## 设计动机

C++23 模块对编译器版本较为敏感,不同版本的 GCC / Clang 在模块语义
处理上存在明显差异。系统包管理器提供的版本通常滞后,且多版本共存
存在维护成本。mcpp 将工具链统一安装在沙盒目录
(`~/.mcpp/registry/data/xpkgs/`)中,允许项目按需选择版本,且不会
影响系统环境。

## 自动安装

首次运行 `mcpp build` 时,若尚未配置工具链,mcpp 会安装并持久化一对与当前
宿主匹配的默认值:

- Linux x86_64 使用面向原生 glibc ABI 的 `gcc@16.1.0`,X11、OpenGL 与系统库
  可直接链接。
- 其他 Linux 架构使用 `gcc@15.1.0-musl`,这是自包含的全静态工具链。
- macOS 使用 `llvm@20.1.7`。
- Windows 存在可用 MSVC 时使用面向 MSVC ABI 的 `llvm@20.1.7`;没有可用 MSVC
  时使用 `gcc@16.1.0` 和 `x86_64-windows-gnu` target(MinGW-w64,默认 static)。

在 Linux 宿主上,全静态 musl 产物始终只差一个参数:
`mcpp build --target x86_64-linux-musl`。

后续构建不再触发该流程。

> [!TIP]
> 在 CI 中可设置 `MCPP_NO_AUTO_INSTALL=1` 只关闭工具链自动安装。需要完整
> 离线时,使用 `mcpp --offline` 或 `MCPP_OFFLINE=1`;它们还会禁止索引刷新和下载。

## 身份模型:Toolchain × Target

一切命名由两条正交轴构成:

- **toolchain** = `family@version`,family ∈ `gcc | llvm | msvc` ——*用谁编*
- **target** = 三段 triple `arch-os[-env]`(如 `x86_64-linux-musl`、
  `x86_64-windows-gnu`、`aarch64-macos`)——*产出给谁*

变体(`gnu | musl | msvc`)在 target 的 `env` 段里,永远不进工具链名字;
"cross" 也不是名字——它只是 `host ≠ target` 这一关系,原生与交叉是同一条
命令。旧拼写(`musl-gcc`、`gcc@15.1.0-musl`、`mingw`、`mingw-cross`、
`clang`、`x86_64-w64-mingw32`)作为别名**永久接受**,归一到该模型并打印
一行 `note:` 提示。

## 手动安装

```bash
mcpp toolchain install gcc 16.1.0           # host target(Linux 上为 GNU libc)
mcpp toolchain install llvm 20.1.7          # LLVM/Clang,macOS 与有可用 MSVC 的 Windows 默认工具链
mcpp toolchain install gcc 16 --target x86_64-linux-musl    # musl target 的链
mcpp toolchain install --target x86_64-windows-gnu          # 省略 family →
                                            # 取该 target 的约定 pin(gcc@16.1.0)
```

显式安装主要用于 CI 缓存预热与离线准备——`mcpp build --target <triple>`
会自动安装该 target 所需的一切。

版本号支持部分匹配:

```bash
mcpp toolchain install gcc 15               # 安装 15.x.y 中的最高版本(15.1.0)
mcpp toolchain install gcc@16               # 同样支持 @ 形式
```

## 切换默认工具链

默认值是一*对*——toolchain 轴 + target 轴(省略 target = host):

```bash
mcpp toolchain default gcc@16.1.0
mcpp toolchain default gcc 15               # 部分版本时,从已安装的版本中选择最高
mcpp toolchain default gcc@16 --target x86_64-linux-musl   # "默认就要全静态 musl"
```

这对默认值持久化为 `~/.mcpp/config.toml` 中的
`[toolchain] default = "gcc@16.1.0"` + `default_target = "x86_64-linux-musl"`。
(存量 config 里 `default = "gcc@15.1.0-musl"` 这类合并拼写原样可用。)

### 谁决定一次构建的编译器

有五种来源会给它命名。它们是分级的,而这套分级正是让工程能写下的那两条压过
mcpp 自己保管的一切的原因:

| | 来源 | mcpp 可否改写 |
|---|---|---|
| 1 | `mcpp.toml` 的 `[target.<triple>] toolchain` | 否 |
| 2 | `mcpp.toml` 的 `[toolchain] default`(或 `MCPP_TOOLCHAIN`) | 否 |
| 3 | 依赖的 `requires = ["mcpp:compiler=<族>"]` | — |
| 4 | 目标行的 pin(当该行的载荷供给目标侧时) | 是 |
| 5 | `mcpp toolchain default`,以及 mcpp 的首次运行默认值 | 是 |

**依赖可以要求一个编译器族。** 一份 C++ 运行时是为某一个族 configure 过的,
并把这份配置记在它所发布的头文件里,因此供给它的包会说明自己是为哪个编译器构建
的。当这条要求与第 4、5 级 —— mcpp 自己推导出来的答案 —— 不同时,mcpp 就为这次
构建取用被要求的那个族:

```
$ mcpp build
   Resolving toolchain
    Resolved llvm@22.1.8 → …/xim-x-llvm/22.1.8/bin/clang++
             required by openkal-llvm-runtime@0.1.3 (`requires = ["mcpp:compiler=llvm"]`),
             not your gcc@16.1.0 — this project only
```

**不写任何东西。** 不写 `~/.mcpp/config.toml`,也不写工程的 `mcpp.toml`。
这条要求是**这次构建**的性质,就只作用于这次构建;这台机器的默认值保持原样,对
其他每一个工程都是。版本取自已经装好的那些 —— 与 `mcpp toolchain default <族>`
走的是同一条解析 —— 只有该族一个都没装时,才取生态自己的 pin。

**工程写下的编译器不会被改写。** 第 1、2 级上工程已经说明了它用什么构建,依赖
与之不一致就是一次真实的矛盾:

```
error: `openkal-llvm-runtime@0.1.3` requires the compiler to be `llvm`.
         compiler          gcc            (16.1.0, payload)
         required          llvm           (required by openkal-llvm-runtime@0.1.3)
       This build's compiler is stated in [toolchain] in mcpp.toml, and a compiler
       the project states outranks one its dependencies ask for.
       Change it to `llvm`, or remove it — with nothing stated, mcpp takes the
       compiler the graph requires and changes no configuration to do it.
```

**两个依赖要求不同的族是错误,不是一次挑选。** 一次构建只有一个编译器;按图的
遍历顺序来定,等于让作者既不书写也无法预测的顺序做决定,并且会满足其中一个包而
让另一个在它自己的头文件里失败。

## 查看工具链状态

```bash
mcpp toolchain list
```

输出分两块——每条轴一块:

```
Toolchains:
  *  gcc 16.1.0              (default)
     gcc 15.1.0
     llvm 22.1.8

Targets:
     TARGET                  NOTE                  TOOLCHAIN         STATUS
     x86_64-linux-gnu        host                  gcc 16.1.0        installed
  *  x86_64-linux-musl       static                gcc 16.1.0        installed
     x86_64-windows-gnu      PE, static, cross     gcc 16.1.0        installed
     aarch64-linux-musl      static, cross         gcc 16.1.0        available
     riscv64-linux-musl      static, cross         —                 planned

Available toolchains (run `mcpp toolchain install <family> <version>`):
     gcc 15.1.0 / 13.3.0 / 11.5.0 / 9.4.0
     llvm 20.1.7
```

`*` 标记当前的默认对。Targets 块是 target 词汇表的实时视图,共四种状态:

| 状态 | 含义 | 下一步做什么 |
|---|---|---|
| `installed` | 本机已有的载荷就能产出它 | 无 |
| `available` | 本宿主存在可装的载荷 | `mcpp toolchain install` |
| `via dependency graph` | 编译器在本机,而目标的系统不在,由包供给 | 依赖一个实现该目标内核接口与 C 库的包 |
| `planned` | 已登记在词表中,尚未发布 | — |

**不在这个块里的 target,在本机根本构建不了**——而这句话现在比以前更窄。
mcpp 2026.8.25.2 之前,这个块只列载荷能服务的那些,于是「系统来自依赖图」的
target 缺席,而同一台机器能为它产出真实的产物。`x86_64-windows-msvc` 与
`aarch64-macos` 在 Linux 宿主上仍然缺席,这是**对的**:MSVC 与 macOS SDK 是宿主
专有的,依赖替代不了。

## Windows PE 之 MinGW-w64(`x86_64-windows-gnu`,无需 Visual Studio)

**没装 Visual Studio 时,这就是 Windows 上的默认值。** Windows 自带的只有
UCRT 运行时 DLL,MSVC STL 与 Windows SDK 都只随 Visual Studio 的
"Desktop development with C++" 负载安装。而 llvm 在 Windows 上打的是 MSVC ABI,
两者都需要,所以 mcpp 首跑时会探测机器上是否有可用的 MSVC(STL **与** SDK
两者齐备 —— 只具备其一才是真正的风险),探测失败则落到这里,并把选择持久化,之后的
构建不再重复提示。无需任何安装或配置。

同一道检查也会修复既有配置:如果 mcpp 早先自己选定的 `[toolchain] default`
在这台机器上已经不可用,它会被就地改写,并打印一行说明。但**用户在
`mcpp.toml` 里显式写下的** `[toolchain]`(或 `[target.X].toolchain`)永远不会
被推翻——一个需要 MSVC ABI 去链接 vcpkg 预编译 `.lib` 的工程,得到的是一条
指明替代方案的错误,而不是被静默换掉 ABI。

mcpp 里 "MinGW" 是一个 **target**,不是工具链名:`x86_64-windows-gnu`
——GCC 产出 Windows PE(GNU CRT)。两种宿主用同一个身份、同一条命令;
由哪个自包含 payload 来承接是自动分流的(Windows 宿主 → winlibs UCRT
构建;Linux 宿主 → 从源码构建的 MSVCRT 交叉链,CI 中经 wine 实测):

```bash
mcpp build --target x86_64-windows-gnu       # Windows 或 Linux 上皆可
mcpp toolchain default gcc@16 --target x86_64-windows-gnu
# 旧拼写仍然接受:mingw@16.1.0、mingw-cross@16.1.0、
# --target x86_64-w64-mingw32
```

它走常规的 GCC 模块管线(`gcm.cache`、经 libstdc++ `bits/std.cc` 的
`import std`)。该 target 默认 linkage 为 **static**——产出的 `.exe`
完全自包含(无需随包分发 `libstdc++-6.dll`,可直接在 wine 下运行);
要退出请写在 target 段上——`linkage` 只认精确 triple(见
[mcpp.toml](03-mcpp-toml.md) §2.7),`[build] linkage` 这个键并不存在,写了会被静默忽略:

```toml
[target.x86_64-windows-gnu]
linkage = "dynamic"
```

manifest 中:

```toml
[toolchain]
windows = "gcc@16"            # Windows 上的 gcc family = MinGW-w64
# 旧值 "mingw@16.1.0" 原样可用
```

产物名跟随 **target**;静态库的命名约定分岔点是 triple 的 *env* 段,而不是 OS:

| Target | `kind = "lib"` 产出 |
|---|---|
| `x86_64-windows-gnu` | `libfoo.a`(GNU 约定) |
| `x86_64-windows-msvc` | `foo.lib`(MSVC 约定) |

2026.8.3.3 之前,Windows 宿主上的 mingw 构建产出的是 `foo.lib` —— 一个 GNU
archive 顶着 MSVC 的名字,MSVC 拿不去用。若有脚本按 `*.lib` 收集产物
`windows-gnu` 的产物,现在要改成 `*.a`。

## Windows 上产出 Linux ELF(`x86_64-linux-musl`,无需 WSL)

上一节的镜像:一台 Windows 机器直接产出**完全静态的 Linux 二进制**,
不需要 WSL、不需要容器,也不往系统里装任何东西。

```bash
mcpp build --target x86_64-linux-musl        # Windows 或 Linux 上皆可
```

两种宿主上这条命令**逐字相同**,因为 "交叉" 在 mcpp 里不是一个名字,
它只是 `host ≠ target` 这个关系。由哪个 payload 承接目标是自动分流的:
Linux x86_64 宿主装原生 `musl-gcc`;Windows 宿主装一条 **canadian-cross**
GCC(以 `x86_64-linux-gnu` 构建 → 运行于 `x86_64-w64-mingw32` → 产出
`x86_64-linux-musl`)。两者都是 GCC 16.1.0,也都带 `bits/std.cc`,
所以 `import std` 在两边行为一致。

产物是没有 `PT_INTERP` 的全静态 ELF —— 不挑发行版、不挑 libc,
这正是 musl 成为第一个被打通的 Linux target 的原因:

```console
$ file mcpp
mcpp: ELF 64-bit LSB executable, x86-64, statically linked, stripped
```

Windows 上**不支持** `x86_64-linux-gnu`:glibc target 还需要 `xim:glibc`
与 `xim:linux-headers` 两个 sysroot payload,而它们只为 Linux 宿主发布。
musl target 自包含,两者都不需要。

Windows 上也**不支持跨 arch**(如 `aarch64-linux-musl`)—— canadian-cross
payload 是按宿主 arch 构建的。**macOS 宿主则完全没有面向 Linux 的 payload**,
任何 Linux target 都不可用。

判据不必靠记:`mcpp toolchain list` 只列出当前宿主真正装得上的 target,
Targets 一栏里没有的,就是这台机器确实服务不了(实现见
`toolchain::host_can_serve`)。

## MSVC(Windows)

一个 MSVC toolset 有两条路径进入构建,由 **spec 的版本轴**决定是哪一条:

| Spec | 来源 | 解析到的编译器 |
|---|---|---|
| `msvc@system`(或裸 `msvc`) | 这台机器自己的 Visual Studio | 这里恰好装了什么就是什么 |
| `msvc@<toolset>`(如 `msvc@14.44.35207`) | mcpp 安装的 xlings payload | 指名的那一个,在每台机器上一致 |

它们不是"二选一",而是回答了不同的问题。`msvc@system` 问的是*"用这位开发者
已经有的东西"*;`msvc@14.44.35207` 问的是*"用恰好这个编译器构建本工程"*。
pinned toolset 之间、以及与系统 Visual Studio 之间都可以共存。

> **`@system` 是 MSVC 独有的拼写。** 没有 `gcc@system`,也没有 `llvm@system`,
> 这是有意的而不是漏了:mcpp 建立在用户态 OS xlings 之上,整条设计就是**把 host
> 依赖降到最低** —— 工具链来自 manifest 点名的 payload,于是每台机器用同一个
> 编译器。Windows 是唯一一处"拒绝使用已装好的东西"代价大于收益的地方:
> Visual Studio 常常已经装了,又不总能重新分发。其它族写 `<family>@system` 会
> 直接报错,并同时给出两种可能的写法。(不带族的
> `[toolchain] … = "system"` —— 即 PATH 上的编译器 —— 是另一套、也是有意保留的
> 逃生口,不受影响。)

### `[toolchain] … = "system"` —— 拒绝

**mcpp 只用它自己管理的工具链构建。** `PATH` 上现成的编译器不受支持,该配置会被**拒绝**,
而不是提示:

```
error: [toolchain] linux = "system" is not supported: mcpp builds only with
       toolchains it manages.
       A compiler taken from PATH cannot be identified or reproduced, so
       `import std` availability, the runtime closure and "the same build on
       another machine" all stop being things mcpp can promise.
       Name one instead — mcpp installs it on first use:

         [toolchain]
         linux = "gcc@16.1.0"

       or set a machine default with `mcpp toolchain default gcc@16.1.0`, and
       see `mcpp toolchain list` for what is available.
```

`msvc@system` 是**唯一的例外**,而且是另一种拼法:它点名的是一个**族**,mcpp 负责定位并识别
其安装 —— 那是唯一一个编译器不能被重新分发的平台。见上一节。

#### 为什么工具链与库得到的答案不同

mcpp 对 host 依赖的规则并不是各条轴统一的,这个分叉是刻意的:

- **mcpp 自身、以及 mcpp 生态发布的一切,都不依赖任何 host。** 工具链与 payload 都经由
  xlings 获得 —— xim 索引或 mcpp-index。这正是构建能跨机器、跨 Linux 发行版复现的原因。
- **工具链属于这份契约,所以它不是工程可以从 host 拿的东西。** mcpp 承诺的每一件事 ——
  `import std` 可用、运行期闭包可计算、同一份构建在同事机器上和 CI 里一致 —— 都是关于
  **一个 mcpp 解析出来、叫得出名字的编译器**的陈述。`PATH` 上的编译器让这些全部无法核验,
  这就是这一条是拒绝的原因。
- **程序链接哪些库,是程序自己的事。** 工程可以链 host 的库,也可以链自己的 `.so`。mcpp 会
  说明这样做的代价,并指出受支持的路径 —— 声明该 provider 让它从 mcpp-index 解析;索引尚未
  收录时,**把包贡献进 mcpp-index** 就是那条路 —— 但只要结果能构建、能运行,就不强行拒绝。
  产物是开发者的,由他保证。

而"证明跑不起来"的构建在两条轴上都仍然是错误:运行期闭包不可满足时会被拒绝,因为产物根本
起不来。见[二进制分发](12-binary-distribution.md)。

### `msvc@system` —— 机器自己的 Visual Studio

mcpp 只负责定位并识别已安装的 Visual Studio / Build Tools,**从不**安装、
升级或卸载它。

```bash
mcpp toolchain default msvc
```

定位顺序:

1. **`VSINSTALLDIR`** —— 由开发者命令提示符或跑过 `vcvarsall` 的 CI 步骤设置。
   这是一个**回答**而不是猜测,所以排在下面几种探测之前。
2. `vswhere.exe`(含 prerelease / Insiders 实例)
3. `VS*COMNTOOLS`
4. 标准的 `Program Files\Microsoft Visual Studio\<year>\<edition>` 路径

随后识别涉及的各个版本,并持久化为稳定 spec `msvc@system`:

```
Detected   msvc 19.44.35211 (VS 2022 BuildTools) (VC tools 14.44.35207)
           cl: C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe
           import std: available (std.ixx)
Default    set to msvc@system (was: llvm@20.1.7)
```

若机器上没有 Visual Studio,mcpp 会报告该情况并给出两条路径:一个可由 mcpp 安装的
pinned toolset,或者 Visual Studio Installer /
`winget install Microsoft.VisualStudio.2022.BuildTools`。

`mcpp toolchain list` 会把检测到的 MSVC 列在单独的 `System:` 分区,
`mcpp self doctor` 在 Windows 上会报告它的状态。manifest 里:

```toml
[toolchain]
windows = "msvc@system"
```

### `msvc@<toolset>` —— mcpp 安装并 pin 的 toolset

```bash
mcpp toolchain list --available msvc     # 可以 pin 哪些
mcpp toolchain install msvc 14.44.35207
```

这在每个方面都和 `gcc@16.1.0` 一样:payload 下载进 mcpp 自己的 store,多个
toolset 共存,`mcpp toolchain remove msvc@<toolset>` 卸载其中一个,manifest 里
点名的那个会在首次构建时自动安装。

```toml
[toolchain]
windows = "msvc@14.44.35207"
```

**这里的版本是 toolset 目录名**(`14.44.35207` —— 即 `VC\Tools\MSVC\` 下的
目录名、也是 `-vcvars_ver` 接受的值),**不是** cl banner 版本
(`19.44.35211`),也不是产品年份。机器上不需要预装任何东西:payload 带来编译器、
STL,以及通过它的 `xim:windows-sdk` 依赖带来 ucrt/um 的头文件与导入库。

> **已变更:** `msvc@19.44` 过去的含义是"用系统 MSVC,并校验其 banner 以 19.44
> 开头" —— 这条只有 `mcpp toolchain default` 会查,构建则静默忽略。现在版本轴
> 在所有地方都指 toolset。写成 `19.x` 会得到一条同时给出两种替代写法的错误
> —— `msvc@system`,或那台机器实际拥有的 toolset 版本。

### 原生 cl.exe 构建

自 0.0.90 起两条来源都可用:mcpp 从 VC tools + Windows SDK 合成 INCLUDE/LIB
环境(不经 `vcvarsall`),把 `std.ixx`/`std.compat.ixx` staging 成 `.ifc` BMI,
用 `/interface /TP /ifcOutput` 编译 `.cppm` 模块单元,用 `/scanDependencies`
扫描,并通过 response file 调 `link.exe`/`lib.exe` 链接。

**Windows SDK 跟着来源走**,因为两条来源回答的是不同的问题,SDK 也必须如此:

| 来源 | SDK 怎么选 |
|---|---|
| `msvc@<toolset>` | **随该 toolset 一起装进 mcpp store 的** `xim:windows-sdk` payload。环境里的 `WindowsSdkDir` / `WindowsSdkVersion` 会被**忽略**,并且 mcpp 会打印一行 `note:` 说明。 |
| `msvc@system` | 先 **`WindowsSdkDir`**(+ `WindowsSdkVersion`),再 `C:\Program Files (x86)\Windows Kits\10`。 |

这种不对称正是要点。pin 一个 toolset 是在承诺"两台机器用同一套头文件编同一份
源码";一个能悄悄改写它的环境变量会把这条承诺降格成偏好。反过来,机器自己的
SDK 只能靠找,而在那里"明确声明"应当压过"扫描" —— 和 `VSINSTALLDIR` 压过
`vswhere` 是同一条优先级。

如果一个 pinned toolset 旁边没有 SDK payload(比如较老的安装),mcpp 会退回用
机器上的 SDK 而不是失败 —— 并且会说出来,因为那次构建已经不可复现,而除此之外
没有任何东西会记录这件事。

一个根目录只有**两半都在**才算 SDK —— `Include\<v>\ucrt\corecrt.h` **且**
`Lib\<v>\um\<arch>\kernel32.lib`。只有头文件、没有导入库的根会被跳过而不是
被选中,于是一个只解包了一半的 payload 不会压过机器上完整的 SDK,也就不会在
构建的最后一刻变成 `LNK1104: cannot open file 'kernel32.lib'`。

解析出的 SDK 版本是这次构建的**运行时身份**(`ucrt@10.0.26100.0`)的一部分,
因而也进入了给构建缓存做 key 的那个指纹:换 SDK 就换缓存键,和换编译器一样。
它是一条**兼容性下限声明**,而不是 Linux 上 `glibc@2.39` 那种 payload 绑定 ——
`ucrtbase.dll` 是 Windows 组件,mcpp 既不分发也不替换它。

**CRT 模型。** 默认 `/MD`(host-coupled);下面两者任一都会选 `/MT`:

```toml
[target.x86_64-windows-msvc]
linkage     = "static"           # libc 那根轴 —— TARGET 段,或 `--static`

[build]
cxx_runtime = "self-contained"   # C++ 运行时那根轴
```

注意这两个键分别属于哪个段:`linkage` 只认精确 triple,**没有 `[build] linkage`
这个键** —— 写了会得到一条 "unsupported key (ignored)" 警告,而且不会切到静态
CRT。

toolset 自带的那份可再分发 CRT(`vcruntime140.dll` / `msvcp140.dll`)可以跟着
产物走 —— 见 `docs/zh/03-mcpp-toml.md` 的 `cxx_runtime = "toolchain-coupled"`。

## 项目级版本锁定

若项目需固定特定版本而不依赖全局默认,可在项目的 `mcpp.toml` 中声明:

```toml
[toolchain]
default = "gcc@16.1.0"
linux   = "gcc@16.1.0"
macos   = "llvm@20.1.7"
```

项目级声明优先于全局默认配置。

## Target 与交叉构建

```bash
mcpp build --target x86_64-linux-musl        # 全静态 ELF
mcpp build --target aarch64-linux-musl       # 跨 arch(x86_64 上出 aarch64)
mcpp build --target x86_64-windows-gnu       # Linux 上出 Windows PE
```

`--target` 会对已知 target 词汇表做校验(README 平台表与之同源):打错
字是**硬错误并附建议**(`did you mean 'x86_64-linux-musl'?`)——绝不会
静默退回宿主构建。词汇表之外的自定义 triple,在 `mcpp.toml` 中显式声明
`[target.<triple>]` 节即可放行(逃生舱)。

每个已知 target 自带约定:pin 的工具链(按需自动安装)与默认 linkage
(`*-linux-musl` 与 `x86_64-windows-gnu` 默认 static)。显式的
`[target.<triple>]` 节可同时覆盖两者:

```toml
[target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
linkage   = "static"
```

### 约定可以被推翻,能力不行

**有宿主**那一行的 pin 回答的是*哪个载荷供给这个目标的 C 库*,所以一个自己供给
C 库的工程可以写任何编译器 —— 整个 openkal 生态就建立在这道口子上。不能做的是
写另一个编译器而什么都不供给:

```
$ mcpp build --target x86_64-linux-musl        # [toolchain] default = "llvm@…"
error: target 'x86_64-linux-musl' takes its C library from the 'gcc@16.1.0'
       payload, and 'llvm@22.1.8' has none here.
```

有两类行回答的是另一个问题,它们的 pin 根本不可被推翻:

| 行 | 为什么 |
|---|---|
| 所有 `*-none-elf` | 不存在按宿主分的交叉载荷;clang 与 lld 按构造就是交叉编译器,gcc 不是 |
| `x86_64-windows-musl` | 没有任何 gcc 载荷发得出 PE + musl —— mingw 载荷发的是 PE + MinGW CRT,那是隔壁 `-gnu` 那一行 |

```
$ mcpp build --target riscv64-none-elf         # [toolchain] default = "gcc@…"
error: target 'riscv64-none-elf' cannot be emitted by 'gcc@16.1.0'.
```

**两处拒绝都发生在做出决定的地方**,而不是留给编译器。2026.8.26.1 之前,前者
会跑完整个构建然后死在链接上,报 `crtbeginT.o (bare name)`;后者给出
`g++: error: unrecognized argument in option '-mabi=lp64d'` —— 一条关于选项的
消息,而决定在一百行之前。

要给这些结果分类的程序读 `mcpp why toolchain --format json` 的 `data.reason`
(`convention-unreplaced` / `capability-pin`),而不是那句话 ——
见[第 11 章](50-machine-output.md)。

项目还可以声明自己的*默认*构建 target——"本项目发布全静态"这类语义
就该放在这里(全静态是产物属性,不是编译器家族属性):

```toml
[build]
target = "x86_64-linux-musl"                 # ≙ cargo 的 build.target
```

配合 `mcpp pack --mode static` 即可产出全静态发布包,完整示例参见
[`examples/03-pack-static`](../../examples/03-pack-static/)。

## 卸载

```bash
mcpp toolchain remove gcc@16.1.0
```

## 重置沙盒

```bash
rm -rf ~/.mcpp                              # 删除整个沙盒
mcpp build                                  # 后续构建将再次触发首次安装
```

## 环境变量

mcpp 的运行行为可通过以下环境变量调整:

| 变量 | 用途 |
|---|---|
| `MCPP_HOME` | 覆盖沙盒位置(默认 `~/.mcpp/`),绝对路径优先级最高 |
| `MCPP_NO_AUTO_INSTALL=1` | 禁用工具链自动安装,适用于 CI 与离线环境 |
| `MCPP_OFFLINE=1` | 完全不访问网络,等价于全局 `--offline` |
| `MCPP_NO_COLOR=1` / `NO_COLOR=1` | 禁用彩色输出 |
| `MCPP_LOG_LEVEL=debug\|info\|warn\|error\|off` | 日志级别 |

未显式设置 `MCPP_HOME` 时,mcpp 将基于二进制所在目录的上一级路径
自动定位沙盒位置(release tarball 解压至 `~/.mcpp/` 后,`~/.mcpp/`
即为 home),因此 release 版本无需任何环境变量配置即可运行。


## ABI 能力强制

依赖可声明 `abi:<name>` 能力(如 `compat.glfw` 声明 `abi:glibc`)。解析出的
工具链 ABI 不满足任一依赖的 abi 要求时,构建会**尽早失败**并给出修复建议
(例如 musl-static 工具链遇到 abi:glibc 依赖),取代深层的链接/头文件报错。
查看:`mcpp why toolchain`。

## 已知工具链风险:模块接口中的运算符模板(Clang 20+)

一个导出**替换性运算符模板**的模块,在 Clang 20 或 22 下会毒化所有导入者
中该运算符的名字查找:任何 `import` 了这个模块、并用到该运算符的 TU
——**无论作用在什么类型上**——都会让前端崩溃(SIGSEGV)。GCC 16 与
Clang 18 不受影响,所以这是 Clang 18 到 20 之间的一处回归。

它正好打在 module-package 这个模式上。包装一个运算符是 `static inline`
模板的上游头文件,再用一个恒真约束镜像它们的签名(跨 TU 包含关系的标准配方),
恰恰就是踩中它的写法。

**经验判据:**每个模板形参都应由**第一个**函数实参定死。破坏这一点的形状就是有毒的:

```cpp
// 有毒 —— `n` 与 `l` 不由第 1 个实参决定
template<typename T, int m, int n, int l>
Matx<T, m, n> operator*(const Matx<T, m, l>& a, const Matx<T, l, n>& b);

// 有毒 —— 第二个 typename 只出现在第 2 个实参里
template<typename T1, typename T2, int n>
Vec<T1, n>& operator+=(Vec<T1, n>& a, const Vec<T2, n>& b);

// 没问题 —— 每个形参都由第 1 个实参定死
template<typename T, int m, int n>
Matx<T, m, n> operator+(const Matx<T, m, n>& a, const Matx<T, m, n>& b);
```

崩溃是**按名字**触发的:一处被毒化的 `operator*` 声明,会让每个导入者里的
每一个 `x * y` 都崩,哪怕类型完全无关。函数体本身无关紧要。

**绕法**是整体推导操作数类型再加约束,而不是在形参列表里把它们拆开。
这样保持调用兼容,跨 TU 语义也仍然成立 —— 上游那个精确匹配的
`static inline` 更特化,在那边照样胜出:

```cpp
template<typename MA, typename MB>
    requires pick<typename MA::value_type>
          && __is_same(MA, typename MA::mat_type)
          && __is_same(MB, Matx<typename MB::value_type,
                               (int)MA::rows, (int)MA::cols>)
inline MA& operator+=(MA& a, const MB& b);
```

跟踪于 [mcpp#256](https://github.com/mcpp-community/mcpp/issues/256)。
`tests/e2e/150_clang_module_operator_template.sh` 是一只跑在内置 LLVM
工具链上的金丝雀 —— 未来某次 Clang 升级修好(或再次弄坏)这一点时,
它会显式暴露出来,而不是悄悄改变包能表达的东西。

## C++ 运行时契约(`cxx_runtime`)

`cxx_runtime` 声明的是**产物对运行它的机器做出的承诺**。它是**分发**属性而非
构建属性 —— 它描述的是运行期依赖集,而兑现它的 flag 逐平台不同。

> **不含 C++ 的目标没有 C++ 运行时契约需要兑现。** mcpp 用 C 驱动链接它,
> 并且完全不发 C++ 运行时相关的 flag,因此一个纯 C 的共享库不会平白拿到
> `libstdc++` / `libc++` 依赖。目标里只要有一个 C++ 翻译单元,整个目标就回到
> C++ 驱动。这一判定由源码推导,没有对应的配置键。

```toml
[build]
cxx_runtime = "self-contained"          # 作用于所有目标(默认值)

# 或者按角色分别指定:
[build.cxx_runtime]
default = "self-contained"              # 可执行文件
tests   = "host-coupled"                # 测试二进制从不离开本机
shared  = "self-contained"              # 共享库(见下 —— 默认值随目标格式而变)

# 或者按目标三元组 —— 与 `linkage` 并列,因为它们是同一根轴:
[target.x86_64-linux-gnu]
cxx_runtime = "host-coupled"            # 例如这次构建是为发行版打包
```

| 取值 | 产物运行时需要 | 典型场景 |
|---|---|---|
| `self-contained`(默认) | 自身之外不需要任何 C++ 运行时 | 分发二进制 |
| `toolchain-coupled` | mcpp 装的那份工具链的 C++ 运行时 | 本地迭代 |
| `host-coupled` | 驱动默认解析到的那份(通常是系统运行时) | 发行版打包;必须与宿主共用同一份运行时的 `dlopen` 插件 |

**默认即自包含(portable by default)**:macOS 上这会静态链入 LLVM 自带的
libc++/libc++abi —— 系统 libc++ 会把实际可运行版本固定在构建机的 OS(老系统
缺新符号,如 `std::print` 的支撑符号),只有静态化才能真正兑现
`macos_deployment_target` 的 floor。Linux/MinGW 上它是 `-static-libstdc++`
(GCC)或整条链的 `-static`(MinGW);Linux 上的 clang/libc++ 工具链则显式链入
libc++.a/libc++abi.a/libunwind.a。更低的 macOS floor(11–13)需自建 libc++
归档(已验证可行,数据级切换,按需提供)。

**共享库是唯一一个默认值随目标格式变化的角色**,因为危害本身随格式变化。
`.so`/`.dylib`/`.dll` 不是一个小号可执行文件 —— 它被加载**进**一个已经有
C++ 运行时的进程。

| 目标格式 | `kind = "shared"` 的默认契约 | 原因 |
|---|---|---|
| ELF(Linux 等) | `toolchain-coupled` | ELF 只有一个全局符号命名空间,先加载的定义胜出。静态内嵌了 libstdc++ 的 `.so` 会把它**导出**,链接该库的可执行文件于是把自己的 `std::` 引用绑到那里 —— 它自己的 `self-contained` 契约静默变成空操作,它的 C++ 运行时变成"碰巧加载的那一份该库"。 |
| Mach-O | `self-contained` | 那里的机制本来就是 `-load_hidden`(hidden 可见性),dyld 不会归一这些符号;而且 macOS 上根本没有 toolchain-coupled 这一档(见下文注)。 |
| PE(Windows) | `self-contained` | PE 没有全局符号命名空间 —— 导入按 DLL 逐个按名解析,一个 DLL 的私有运行时不可能被别人捡走。 |

在 ELF 上显式写 `shared = "self-contained"` 是支持的,而且就是字面意思:库会内嵌
运行时。此时 mcpp 会额外发 `-Wl,--exclude-libs`(针对标准库归档),让内嵌的那份
留在库的动态符号表之外,链接它的任何东西都捡不走。本工程代码产生的模板实例化
(`std::string` 之类的 weak/COMDAT 符号)仍然会导出 —— 那是 C++ ABI 的预期行为,
不是这里要防的泄漏。

工程级的 `cxx_runtime = "…"`(或 `static_stdlib = false`)同样作用于共享库:
有人写下了整个工程的承诺。只有在**没人写**的时候,随格式变化的默认值才生效。

`static_stdlib` 是旧拼写,仍然有效:`true` 等价于 `self-contained`,`false`
等价于 `host-coupled`。显式写了 `cxx_runtime` 时以后者为准。

**兑现不了的契约会被报出来,绝不静默降级。** 若工具链不带 `libc++.a`,或某个
契约在该平台上没有对应机制,构建会打印实际退到了哪一档,而不是悄悄交付一个与
manifest 所述不同的产物。

### 在 MSVC 运行时上

这里的机制就是 CRT 模型,而它是**整个工程级**的开关:cl 会把 `_MSVC_MT` /
`_MSVC_MD` 烘进工程唯一的那份 `std` 模块,所以与工程不一致的按角色契约无法兑现,
会被报出来而不是被忽略。

| 取值 | 在 MSVC 上是什么 |
|---|---|
| `self-contained` | `/MT`,静态 CRT。`linkage = "static"` 从 libc 那根轴选中的是同一件事。 |
| `host-coupled`(`/MD` 下的默认) | 由目标机器提供 `vcruntime140.dll` / `msvcp140.dll` —— 即那台机器装了 Visual Studio 或 redistributable。 |
| `toolchain-coupled` | toolset **自带**的那份 DLL 跟着产物走。 |

`toolchain-coupled` 值得说清楚,因为直觉上的理解是错的。`ucrtbase.dll` **是**
Windows 组件(Win10 起),mcpp 从不分发它;而 `vcruntime140.dll` /
`msvcp140.dll` **不是**:每个 MSVC toolset 都在
`VC\Redist\MSVC\<version>\<arch>\` 下带着它们,和 gcc payload 带着
`libstdc++.so` 是同一件事。在这个契约下 mcpp 会把它们放到产物旁边 —— 这正是让
默认的 `/MD` 产物能在"只装了 pinned toolset、根本没有 Visual Studio"的机器上跑
起来的原因。

调试版 CRT(`debug_nonredist\` 下的 `vcruntime140d.dll` 等)永远不会被放进去:
它不可再分发。

> **从 2026.8.15 或更早版本升上来?** 这条键在 MSVC ABI 上曾经是**空操作** ——
> 它会报 `not implemented for the MSVC runtime yet`,写什么都退回 `/MD`。
> 自 2026.8.16 起它真的生效,于是一份从那个年代带着
> `cxx_runtime = "self-contained"` 的 manifest **会在升级时换掉 CRT 模型**:
> 从 `/MD` 变成 `/MT`。它不是同一个模型的更严格版本,而且因为这个值一直是合法的,
> 切换是**静默**的。若工程是在这条键尚未生效时写下它的,应重新确认所需的取值。

把它和 `/MT` 一起用是**矛盾**而不是缺功能 —— 静态 CRT 根本没有 DLL 可以耦合 ——
所以会被报出来并落到 `self-contained`。另一半由 `mcpp pack` 兜底:什么都不打包的
模式(`--mode system`、`--mode static`)兑现不了 `toolchain-coupled`,会直接拒绝。

**边界。** 该契约只管 C++ 运行时。静态 **libc** 是另一根轴(`linkage = "static"`
/ `--static`,如 musl 目标),部署下限是第三根轴(`macos_deployment_target`)。
另外,`host-coupled` 只承诺 mcpp 不做任何"把 C++ 运行时打进产物"的动作,它不会
去掉链接因其它原因已经携带的工具链 rpath —— 所以在 ELF 上这类产物仍可能优先
找到工具链的库。

> **macOS + `self-contained` 与静态初始化次序。** Mach-O 没有按优先级排序的
> 初始化段,而 libc++ 的 `<iostream>` 也不像 libstdc++ / MSVC STL 那样自带
> `ios_base::Init` 守卫 —— 于是从 `libc++.a` 里拉出来的流初始化器本来会排在
> 程序自己的全局构造函数**之后**:一个在构造函数里碰 `std::cout` 的全局对象会
> 读到尚未构造的流,进程启动即崩。mcpp 会把一个极小的生成对象排在链接最前面
> 把流顶上去,调用方代码无需改动。详见 mcpp-community/mcpp#336。

`defines` 接受**裸**宏名(不带 `-D`),把每个条目脱糖为 `-D<x>`,同时作用于 C 和
C++ 编译通道。它覆盖包内每个 TU(含模块接口单元),因此也会进入**编译器自己的**
P1689 模块扫描。

> **但它不会让被宏保护的 `import` 变得可用。** mcpp 在编译器看到文件之前先跑
> 自己的词法预扫描,而那个扫描器对**任何** `#if` / `#ifdef` 块内的 `import`
> 一律拒绝,不求值条件:
>
> ```
> error: import statement inside conditional preprocessor block (forbidden in M1)
> ```
>
> 所以即使 `FOO` 写在 `defines` 里,`#ifdef FOO` / `import bar;` 仍然会失败。
> 替代写法是把条件放在全局模块片段的 `#include` 上。见
> mcpp-community/mcpp#421。

汇编单元同样能拿到。它是普通的构建
输入,所以 `[target.'cfg(...)'.build]` 也能承载它:

```toml
[build]
defines = ["APP_NAME=\"demo\""]

[target.'cfg(windows)'.build]
defines = ["USE_WIN32", "WINVER=0x0A00"]
```

选择合适的轴:

| 想让宏作用于… | 用 |
|---|---|
| 本包的每个 TU | `[build].defines`(本节) |
| 仅某个二进制自己的入口源 | `[targets.<name>].defines` |
| 指定的一批文件 | `[build].flags` 配 `glob` + `defines` |
| 本包每个 TU **以及**消费者的 TU | `[features.<name>].defines`(接口贡献) |

`[build].defines` 是包私有的:不会传播给消费者。

`[build]` 下不支持的键会作为警告报出(`--strict` 下为错误),而不是被静默忽略。

C++ 标准不要通过 `build.cxxflags = ["-std=..."]` 配置。请使用:

```toml
[package]
standard = "c++26"
```

mcpp 会把同一个标准用于普通 C++ 编译、模块扫描、`compile_commands.json` 和 `import std` 的标准库 BMI 构建。

**glob 排除**(`!` 前缀,mcpp 0.0.4+):

```toml
[build]
sources = [
    "src/**/*.cpp",
    "!src/**/*_test.cpp",       # 排除测试文件
    "!src/**/*_fuzzer.cpp",     # 排除 fuzzer
]
```

**per-glob 旗标**(mcpp 0.0.95+):`[build] flags` 是**有序**的内联表数组,把额外
编译旗标只附加到 glob 命中的源文件——SIMD 多档 dispatch TU 与三方代码告警隔离的
正解:

```toml
[build]
flags = [
  { glob = "third_party/**",         cflags = ["-w"], cxxflags = ["-w"] },
  { glob = "src/simd/**/*.avx2.cpp", cxxflags = ["-mavx2"], defines = ["HAVE_AVX2"] },
  { glob = "src/x86/**/*.asm",       asmflags = ["-DPREFIX"] },
]
```

每条目键:`glob`(相对包根,必填)+ `cflags` / `cxxflags` / `asmflags` /
`defines`(没有 `ldflags`——链接没有 per-TU 作用域)。声明顺序即应用顺序:靠后
条目的旗标排在命令行更后,配合 GNU "后旗标胜",窄 glob 放在宽 glob 之后即可覆盖。
所有命中条目都生效;这些是私有构建旗标,不会传播给消费者。glob 零命中会打印
warning(打错的 glob 不允许静默无效)。

**生成文件**(mcpp 0.0.95+):`[generated_files]` 把相对路径映射到文件内容(支持
TOML 多行字符串)。条目在源 glob 展开之前写入工程树——与 index 描述符合成模块
包装文件是同一机制——内容进指纹,改内容即重建:

```toml
[generated_files]
"src/gen/wrap.cppm" = """
module;
#include <vendored.h>
export module wrap;
"""
```

路径必须留在工程根之内(`..` / 绝对路径是解析错误)。

**汇编源**(mcpp 0.0.95+):`.S`/`.s`(GAS——由 C 驱动器预处理,覆盖 ARM 与
AT&T 语法 x86)和 `.asm`(NASM——Intel 语法 x86)是一等源文件:默认 glob 收录、
进指纹、增量并行构建、像任何对象一样链接。NASM 的输出格式由目标三元组推导
(`elf64`/`win64`/`macho64`/...——交叉构建零特判);`nasm` 仅在存在 `.asm` 单元时
惰性解析:先 `PATH`,再 mcpp 沙箱,再 `xlings install nasm`;找不到 ≥2.16 的
nasm 则**硬失败**(汇编绝不静默跳过)。限制:`.asm` 仅限 x86 目标(其他目标硬
报错——用条件 sources 门控)、MSVC 工具链不支持 `.S`、`.asm` 即 NASM 语法
(MASM 源请用 `!` 排除)。

