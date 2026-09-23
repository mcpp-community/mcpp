# 20 —— 工具链管理

**读者：** 在意「哪个编译器在跑」的人——一条版本下界、第二套工具链，或者一台
不许用自带编译器的机器。

**本章回答的那一个问题：** 这个工程会被哪个编译器构建、它怎样被安装、以及怎样
换成另一个。

**不在这里：** 这套解析是怎么实现的，那是
[91 —— 工具链机制内幕](91-toolchain-internals.md)；以及 target 怎样命名，那是
[21 —— 目标三元组](21-the-target-triple.md)。

> mcpp 维护一个独立的工具链沙盒，与系统 PATH 完全隔离。

## 设计动机

C++23 模块对编译器版本相当敏感，不同版本的 GCC / Clang 在模块语义的处理上
存在明显差异。系统包管理器提供的版本通常滞后，而多版本共存又带来维护负担。
mcpp 把所有工具链装进同一个沙盒目录（`~/.mcpp/registry/data/xpkgs/`），让每个
工程按需选择自己需要的版本，而不触碰系统环境。

## 自动安装

首次运行 `mcpp build` 且尚未配置工具链时，mcpp 会为当前宿主安装并持久化一对
默认值。这个选择是按宿主区分的：

- Linux x86_64 使用面向原生 glibc ABI 的 `gcc@16.1.0`，X11、OpenGL 与系统库
  因此可以直接使用。
- 其他 Linux 架构使用 `gcc@15.1.0-musl`，这是一套自包含的全静态工具链。
- macOS 使用 `llvm@20.1.7`。
- Windows 上存在可用 MSVC 时使用面向 MSVC ABI 的 `llvm@20.1.7`；没有可用
  MSVC 时使用 `gcc@16.1.0`，target 为 `x86_64-windows-gnu`（MinGW-w64，默认
  静态链接）。

在 Linux 宿主上，全静态的 musl 产物始终只差一个参数：
`mcpp build --target x86_64-linux-musl`。

后续构建不会再次触发这个流程。

> [!TIP]
> 在 CI 中，设置 `MCPP_NO_AUTO_INSTALL=1` 可以只关闭工具链自动安装。需要
> 完全离线的命令时，使用 `mcpp --offline` 或 `MCPP_OFFLINE=1`；它们同时也会
> 阻止索引刷新与下载。

## 身份模型：Toolchain × Target

一切命名由两条正交轴构成：

- **toolchain** = `family@version`，`family` ∈ `gcc | llvm | msvc | emsdk |
  android-ndk`——即*由谁编译*
- **target** = 一个三段三元组 `arch-os[-env]`（例如 `x86_64-linux-musl`、
  `x86_64-windows-gnu`、`aarch64-macos`）——即*产出给谁用*

变体（`gnu | musl | msvc`）存在于 target 的 `env` 段里，永远不进工具链名字。
「cross」同样不是一个名字——它只是 `host ≠ target` 这一层关系，原生构建与
交叉构建用的是同一条命令。旧拼写（`musl-gcc`、`gcc@15.1.0-musl`、`mingw`、
`mingw-cross`、`clang`、`x86_64-w64-mingw32`）作为别名被**永久接受**，归一到
这套模型并打印一行 `note:` 提示。

## 手动安装

```bash
mcpp toolchain install gcc 16.1.0           # host target (GNU libc on Linux)
mcpp toolchain install llvm 20.1.7          # LLVM/Clang, default on macOS and Windows with usable MSVC
mcpp toolchain install gcc 16 --target x86_64-linux-musl    # musl target payload
mcpp toolchain install --target x86_64-windows-gnu          # family omitted → the
                                            # target's convention pin (gcc@16.1.0)
```

显式安装主要用于 CI 缓存预热与离线准备——`mcpp build --target <triple>` 会
自动安装该 target 所需的一切。

版本号支持部分匹配：

```bash
mcpp toolchain install gcc 15               # installs the highest 15.x.y version (15.1.0)
mcpp toolchain install gcc@16               # the @ form works too
```

## 切换默认工具链

默认值是一*对*——toolchain 轴 + target 轴（省略 target 即宿主）：

```bash
mcpp toolchain default gcc@16.1.0
mcpp toolchain default gcc 15               # partial version → highest installed match
mcpp toolchain default gcc@16 --target x86_64-linux-musl   # "default to fully-static musl"
```

这一对默认值持久化为 `~/.mcpp/config.toml` 中的
`[toolchain] default = "gcc@16.1.0"` 加上
`default_target = "x86_64-linux-musl"`。（存量配置里 `default =
"gcc@15.1.0-musl"` 这类合并拼写原样可用，不受影响。）

### 一次构建的编译器选定

有五种来源可以给它命名。它们分级排列，而正是这套分级，让工程能够写下的那两条
压过 mcpp 自己保管的一切：

| | 来源 | mcpp 可否改写 |
|---|---|---|
| 1 | `mcpp.toml` 里的 `[target.<triple>] toolchain` | 否 |
| 2 | `mcpp.toml` 里的 `[toolchain] default`（或 `MCPP_TOOLCHAIN`） | 否 |
| 3 | 依赖给出的 `requires = ["mcpp:compiler=<族>"]` | — |
| 4 | target 行自身的 pin（当该行的载荷供给 target 侧时） | 是 |
| 5 | `mcpp toolchain default`，以及 mcpp 首次运行时的默认值 | 是 |

**一个依赖可以要求一个编译器族。** 一份 C++ 运行时是为某一个族 configure
过的，并把这份配置记在自己发布的头文件里，因此提供它的包会说明自己是为哪个
编译器构建的。当这条要求与第 4、5 级——mcpp 自己推导出来的答案——不同时，
mcpp 就为这次构建取用被要求的那个族：

```
$ mcpp build
   Resolving toolchain
    Resolved llvm@22.1.8 → …/xim-x-llvm/22.1.8/bin/clang++
             required by openkal-llvm-runtime@0.1.3 (`requires = ["mcpp:compiler=llvm"]`),
             not your gcc@16.1.0 — this project only
```

**什么都不写。** 不写 `~/.mcpp/config.toml`，也不写工程自己的 `mcpp.toml`。
这条要求是**这次构建**的性质，所以只作用于这次构建；这台机器的默认值对其他
每一个工程都保持原样。版本取自已经装好的那些——走的是与
`mcpp toolchain default <族>` 相同的解析——只有该族一个版本都没装时，才取
生态自己的 pin。这个 pin 取自载荷正是要求所指的那一行：`llvm` 取
`llvm@…` 的 pin，不取 Android NDK 或 emsdk 的——它们是其他载荷里的 llvm 族
编译器；`mcpp:compiler=emsdk` 或 `mcpp:compiler=android-ndk` 取它自己载荷的
pin(mcpp 2026.9.15.2+)。

**工程自己写下的编译器不会被改写。** 在第 1、2 级上，工程已经说明了自己用
什么构建，依赖与之不一致就是一次真实的矛盾：

```
error: `openkal-llvm-runtime@0.1.3` requires the compiler to be `llvm`.
         compiler          gcc            (16.1.0, payload)
         required          llvm           (required by openkal-llvm-runtime@0.1.3)
       This build's compiler is stated in [toolchain] in mcpp.toml, and a compiler
       the project states outranks one its dependencies ask for.
       Change it to `llvm`, or remove it — with nothing stated, mcpp takes the
       compiler the graph requires and changes no configuration to do it.
```

**两个依赖要求不同的族是一个错误，不是一次挑选。** 一次构建只有一个编译器；
按图的遍历顺序来决定，等于让一个作者既写不出来也预测不了的顺序做主，而且会
满足其中一个包，让另一个包在自己的头文件里失败。

## 查看工具链状态

```bash
mcpp toolchain list
```

输出分两块——每条轴各一块：

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

`*` 标记当前的默认对。Targets 块是 target 词汇表的实时视图，共有四种状态：

| 状态 | 含义 | 后续步骤 |
|---|---|---|
| `installed` | 本机已有的载荷就能产出它 | 无 |
| `available` | 本宿主上存在可安装的载荷 | `mcpp toolchain install` |
| `via dependency graph` | 编译器在本机，而该 target 的系统不在，由包供给 | 依赖一个实现该 target 内核接口与 C 库的包 |
| `planned` | 已登记在词汇表中，尚未发布 | — |

**不出现在这个块里的 target，在本机根本构建不了**——而这句话现在比以前更窄。
在 mcpp 2026.8.25.2 之前，这个块只列出载荷能服务的那些，于是「系统来自依赖图」
的 target 会缺席，即便同一台机器能为它产出真实的产物。`x86_64-windows-msvc`
与 `aarch64-macos` 在 Linux 宿主上仍然缺席，这是**对的**：MSVC 与 macOS SDK
是宿主专有的，没有依赖能替代它们。

## Windows PE 之 MinGW-w64(`x86_64-windows-gnu`，无需 Visual Studio)

**没有装 Visual Studio 时，这就是 Windows 上的默认值。** Windows 自带 UCRT
运行时 DLL，但不带 MSVC STL 或 Windows SDK——两者都只随 Visual Studio 的
「Desktop development with C++」负载一起安装。由于 Windows 上的 llvm 打的是
MSVC ABI，两者都需要，所以 mcpp 首次运行时会探测机器上是否有可用的 MSVC
（STL **与** SDK 两者齐备——只具备其一才是真正的陷阱），探测不到就落回这里，
并把这个选择持久化，之后的构建不再重复提示。不需要安装或配置任何东西。

同一道检查也会修复既有配置：如果 mcpp 早先自己选定的 `[toolchain] default`
在这台机器上已经不可用了，它会被就地改写，并打印一行说明。但用户在
`mcpp.toml` 里显式写下的 `[toolchain]`（或 `[target.X].toolchain`）永远不会
被推翻——一个需要 MSVC ABI 去链接 vcpkg 预编译 `.lib` 文件的工程，得到的是
一条指明替代方案的错误，而不是被静默换掉 ABI。

mcpp 里的「MinGW」是一个 **target**，不是工具链名：`x86_64-windows-gnu`——
GCC 产出带 GNU CRT 的 Windows PE。两种宿主用同一个身份、同一条命令；由哪个
自包含载荷来承接，是自动分流的（Windows 宿主 → winlibs UCRT 构建；Linux
宿主 → 从源码构建的 MSVCRT 交叉工具链，在 CI 中经 wine 实测）：

```bash
mcpp build --target x86_64-windows-gnu       # from Windows OR Linux
mcpp toolchain default gcc@16 --target x86_64-windows-gnu
# legacy spellings still accepted: mingw@16.1.0, mingw-cross@16.1.0,
# --target x86_64-w64-mingw32
```

它走常规的 GCC 模块管线（`gcm.cache`、经由 libstdc++ 的 `bits/std.cc` 实现
`import std`）。该 target 的默认 linkage 是**静态**——产出的 `.exe` 完全
自包含（不需要随包分发 `libstdc++-6.dll`，可以直接在 wine 下运行）。要退出这
个默认值，写在 target 段上——`linkage` 只认精确三元组（见
[mcpp.toml](04-mcpp-toml.md) §2.7），`[build] linkage` 这个键并不存在，写了
会被静默忽略：

```toml
[target.x86_64-windows-gnu]
linkage = "dynamic"
```

manifest 中：

```toml
[toolchain]
windows = "gcc@16"            # gcc family on Windows = MinGW-w64
# legacy value "mingw@16.1.0" keeps working
```

产物名跟随 **target**，而静态库的命名约定分岔点是三元组的 *env* 段，不是
OS：

| Target | `kind = "lib"` 产出 |
|---|---|
| `x86_64-windows-gnu` | `libfoo.a`（GNU 约定） |
| `x86_64-windows-msvc` | `foo.lib`（MSVC 约定） |

2026.8.3.3 之前，Windows 宿主上的 mingw 构建产出的是 `foo.lib`——一个 GNU
归档顶着 MSVC 的名字，MSVC 拿不去用。若有脚本按 `*.lib` 收集
`windows-gnu` 构建的产物，现在要改成 `*.a`。

## Windows 上产出 Linux ELF(`x86_64-linux-musl`，无需 WSL)

上一节的镜像：一台 Windows 机器直接产出**完全静态的 Linux 二进制**，不需要
WSL、不需要容器，也不往系统里装任何东西。

```bash
mcpp build --target x86_64-linux-musl        # from Windows OR Linux
```

两种宿主上这条命令**逐字相同**，因为「cross」在 mcpp 里不是一个名字，它只是
`host ≠ target` 这一层关系。由哪个载荷承接 target 是自动分流的：Linux x86_64
宿主安装原生的 `musl-gcc`；Windows 宿主安装一条 **canadian-cross** GCC（以
`x86_64-linux-gnu` 构建 → 运行于 `x86_64-w64-mingw32` → 产出
`x86_64-linux-musl`）。两者都是 GCC 16.1.0，也都带 `bits/std.cc`，所以
`import std` 在两边行为一致。

产物是没有 `PT_INTERP` 的全静态 ELF——不挑发行版、不挑 libc，这正是 musl
成为第一个被打通的 Linux target 的原因：

```console
$ file mcpp
mcpp: ELF 64-bit LSB executable, x86-64, statically linked, stripped
```

Windows 上**不支持** `x86_64-linux-gnu`：glibc target 还需要 `xim:glibc` 与
`xim:linux-headers` 两个 sysroot 载荷，而它们只为 Linux 宿主发布。musl
target 自包含，两者都不需要。

Windows 上也**不支持跨架构**（例如 `aarch64-linux-musl`）——canadian-cross
载荷是按宿主架构构建的。**一台 macOS 宿主完全没有面向 Linux 的载荷**，任何
Linux target 从那里都够不到。

这一切不必靠记忆：`mcpp toolchain list` 只列出当前宿主真正能安装的
target，Targets 一栏里没有的，就是这台机器确实服务不了（实现见
`toolchain::host_can_serve`）。

## MSVC(Windows)

一个 MSVC toolset 有两条路径进入构建，由 **spec 的版本轴**决定走哪一条：

| Spec | 来源 | 解析出的编译器 |
|---|---|---|
| `msvc@system`（或裸 `msvc`） | 这台机器自己的 Visual Studio | 这台机器的默认 toolset（顺序见下文） |
| `msvc@<toolset>`（如 `msvc@14.44.35207`） | 已安装的同版本 toolset，没有时为 mcpp 安装的 xlings 载荷（2026.9.24.1+） | 指名的那一个 |
| `xim:msvc@<toolset>` | mcpp 安装的 xlings 载荷（2026.9.24.1+） | 指名的那一个，连同随它安装的 SDK |

它们不是二选一，而是回答不同的问题。`msvc@system` 问的是「用这位开发者已经
有的东西」；`msvc@14.44.35207` 问的是「用恰好这个编译器构建本工程」。pinned
toolset 之间、以及与系统 Visual Studio 之间都可以共存。

> **`@system` 是 MSVC 独有的拼写。** 没有 `gcc@system`，也没有
> `llvm@system`，这是有意为之，不是遗漏：mcpp 建立在用户态 OS xlings 之上，
> 整套设计都在把宿主依赖压到最低——工具链来自 manifest 点名的载荷，于是每台
> 机器都用同一个编译器。Windows 是唯一一处「拒绝使用已经装好的东西」代价大于
> 收益的地方：Visual Studio 常常已经装了，又不总能重新分发。写
> `<family>@system` 给其他任何一个族都会直接报错，并同时给出两种可能的意图。
> （不带族的 `[toolchain] … = "system"`——即 PATH 上的编译器——是另一条、
> 同样有意保留的逃生口，不受影响。）

### `[toolchain] … = "system"` —— 拒绝

**mcpp 只用它自己管理的工具链构建。** 取自 `PATH` 的编译器不受支持，这项配置
会被拒绝，而不是被提示：

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

`msvc@system` 是**唯一的例外**，而且是另一种拼法：它点名的是一个**族**，
mcpp 负责定位并识别其安装——那是唯一一个编译器无法被重新分发的平台。见上一节。

#### 工具链与库答案不同的原因

mcpp 对宿主依赖的规则，并不是各条轴统一的，这个分叉是刻意的：

- **mcpp 自身、以及 mcpp 生态发布的一切，都不依赖任何宿主。** 工具链与载荷
  都经由 xlings 获得——xim 索引或 mcpp-index。这正是构建能跨机器、跨 Linux
  发行版复现的原因。
- **工具链属于这份契约，所以它不是工程可以从宿主拿的东西。** mcpp 承诺的
  每一件事——`import std` 可用、运行期闭包可计算、同一份构建在同事机器上和
  CI 里一致——都是关于一个 mcpp 解析出来、叫得出名字的编译器的陈述。取自
  `PATH` 的编译器让这一切都无法核验，这正是这一项会被拒绝的原因。
- **程序链接哪些库，是程序自己的事。** 一个工程可以链接宿主的库，也可以链接
  自己的 `.so`。mcpp 会说明这样做的代价，并指出受支持的路径——声明该
  provider，让它从 mcpp-index 解析；索引尚未收录时，把包贡献进
  mcpp-index 就是那条路——但只要结果能构建、能运行，mcpp 就不强行拒绝。
  产物属于开发者，由他保证。

而「已证明跑不起来」的构建，在两条轴上都仍然是错误：一个满足不了的运行期
闭包会被拒绝，因为产物根本起不来。见
[二进制分发](12-binary-distribution.md)。

### `msvc@system` —— 机器自己的 Visual Studio

mcpp 只负责定位并识别已安装的 Visual Studio / Build Tools，**从不**安装、
升级或卸载它。

```bash
mcpp toolchain default msvc
```

mcpp 按下面的顺序取第一个完整的 toolset（2026.9.24.1+）：

1. **`VCToolsInstallDir`** 指向的 toolset——由开发者命令提示符设置，包括用
   `vcvarsall … -vcvars_ver=<toolset>` 打开的那种；
2. **`VSINSTALLDIR`**（或 `VCINSTALLDIR`）指向的实例的默认 toolset；
3. `PATH` 上第一个 `cl.exe` 所在的 toolset；
4. `vswhere.exe` 报告的、装有 C++ 工具的最新 Visual Studio 实例的默认
   toolset（含 prerelease / Insiders 实例）；
5. 没有 `vswhere.exe` 时，标准的
   `Program Files\Microsoft Visual Studio\<year>\<edition>` 路径。

实例的默认 toolset 是它的
`VC\Auxiliary\Build\Microsoft.VCToolsVersion.default.txt` 所指的那一个。
一个 toolset 具备 `include\`、`lib\x64\`，对 cl.exe 构建还要具备 `cl.exe`，
才算完整。

随后识别涉及的各个版本，并持久化为稳定的 spec `msvc@system`：

```
Detected   msvc 19.44.35211 (VS 2022 BuildTools) (VC tools 14.44.35207)
           cl: C:\Program Files\Microsoft Visual Studio\2022\BuildTools\VC\Tools\MSVC\14.44.35207\bin\Hostx64\x64\cl.exe
           import std: available (std.ixx)
Default    set to msvc@system (was: llvm@20.1.7)
```

如果机器上没有 Visual Studio，mcpp 会报告这一情况，并给出两条路径：一个
mcpp 可以安装的 pinned toolset，或者 Visual Studio Installer /
`winget install Microsoft.VisualStudio.2022.BuildTools`。

`mcpp toolchain list` 会把检测到的 MSVC 列在单独的 `System:` 分区，
`mcpp self doctor` 在 Windows 上会报告它的状态。manifest 里：

```toml
[toolchain]
windows = "msvc@system"
```

### `msvc@<toolset>` —— mcpp 安装并 pin 的 toolset

```bash
mcpp toolchain list --available msvc     # what can be pinned
mcpp toolchain install msvc 14.44.35207
```

pin 的 toolset 如果这台机器已经装有，就直接使用已安装的那一份（2026.9.24.1+）：
mcpp 在所有 Visual Studio 实例中查找该版本的完整 toolset，只有都没有时才安装
载荷。部分版本（`msvc@14.44`）取匹配的最高版本。环境变量（`VCToolsInstallDir`、
`VSINSTALLDIR`）不参与 pin 的选择；如果某个变量本来会选出别的 toolset，会打印
一行 `note:`。`mcpp toolchain list` 在 `installed toolsets` 下列出这台机器的
toolset。

`xim:msvc@<toolset>` 只要载荷，不看机器上有什么。载荷下载进 mcpp 自己的
store，多个 toolset 可以共存，`mcpp toolchain remove msvc@<toolset>` 卸载其中
一个，manifest 里点名的那个会在首次构建时自动安装。

```toml
[toolchain]
windows = "msvc@14.44.35207"       # an installed 14.44.35207 first, else the payload
# windows = "xim:msvc@14.44.35207" # the payload only
```

所有工具链族都接受 `xim:`；gcc 与 llvm 的工具链总是来自载荷，所以
`xim:gcc@16.1.0` 与 `gcc@16.1.0` 是同一个工具链。它是工具链写法唯一接受的
命名空间：早先的版本会静默剥掉任何 `<ns>:` 前缀，现在其他命名空间被拒绝。

**这里的版本是 toolset 目录名**（`14.44.35207`——即 `VC\Tools\MSVC\` 下的
目录名，也是 `-vcvars_ver` 接受的值），**不是** cl banner 的版本
(`19.44.35211`)，也不是产品年份。机器上不需要预装任何东西：载荷带来编译器、
STL，以及通过它的 `xim:windows-sdk` 依赖带来的 ucrt/um 头文件与导入库。

> **已变更：** `msvc@19.44` 过去的含义是「用系统 MSVC，并校验它的 banner 以
> 19.44 开头」——这一条只有 `mcpp toolchain default` 会检查，构建则静默
> 忽略。现在版本轴在所有地方都指向一个 toolset。写成 `19.x` 会得到一条同时
> 给出两种替代写法的错误——`msvc@system`，或者那台机器实际拥有的 toolset
> 版本。

### 原生 cl.exe 构建

自 0.0.90 起，两种来源在这方面都可用：mcpp 从 VC tools + Windows SDK 合成
INCLUDE/LIB 环境（不经过 `vcvarsall`），把 `std.ixx`/`std.compat.ixx`
staging 成 `.ifc` BMI，用 `/interface /TP /ifcOutput` 编译 `.cppm` 模块
单元，用 `/scanDependencies` 扫描依赖，并通过 response file 调用
`link.exe`/`lib.exe` 完成链接。

**Windows SDK 跟随来源走**，因为两种来源回答的是不同的问题，SDK 的选定
也必须如此：

| 来源 | SDK 的选定方式 |
|---|---|
| 载荷 toolset | **随该 toolset 一起装进 mcpp store 的** `xim:windows-sdk` 载荷。环境里的 `WindowsSdkDir` / `WindowsSdkVersion` 会被**忽略**，mcpp 会打印一行 `note:` 说明这一点。 |
| 已安装的 toolset（`msvc@system`，或在机器上找到的 `msvc@<toolset>`） | 先看 **`WindowsSdkDir`**（与 `WindowsSdkVersion`），没有声明时再看 `C:\Program Files (x86)\Windows Kits\10`。 |

这种不对称正是要点所在。pin 一个 toolset，是在承诺「两台机器用同一套头文件
编译同一份源码」；一个能被环境悄悄改写的变量，会把这条承诺降格成一种偏好。
反过来，机器自己的 SDK 只能靠查找才能确定，而在那里，一个明确的声明应当压过
一次扫描——与 `VSINSTALLDIR` 压过 `vswhere` 是同一条优先级。

如果一个 pinned toolset 旁边没有 SDK 载荷（比如一次较老的安装），mcpp 会
退回使用机器上的 SDK，而不是直接失败——并且会说出来，因为那次构建已经不可
复现，而除此之外没有任何东西会记录这件事。

一个根目录只有**两半都在**才算一个 SDK——`Include\<v>\ucrt\corecrt.h`
**且** `Lib\<v>\um\<arch>\kernel32.lib`。只有头文件、没有导入库的根目录会被
跳过而不是被选中，于是一个只解包了一半的载荷不会压过机器上完整的 SDK，也就
不会在构建的最后一刻变成 `LNK1104: cannot open file 'kernel32.lib'`。

解析出的 SDK 版本是这次构建**运行时身份**的一部分（`ucrt@10.0.26100.0`），
因而也进入了给构建缓存做 key 的那个指纹：换 SDK 就换缓存键，和换编译器一样。
它是一条**兼容性下限声明**，而不是 Linux 上 `glibc@2.39` 那种载荷绑定——
`ucrtbase.dll` 是 Windows 组件，mcpp 既不分发也不替换它。

**CRT 模型。** 默认是 `/MD`(host-coupled)；下面两者中任意一个都会选中
`/MT`：

```toml
[target.x86_64-windows-msvc]
linkage     = "static"           # the libc axis — TARGET section, or `--static`

[build]
cxx_runtime = "self-contained"   # the C++ runtime axis
```

注意这两个键分别属于哪个段：`linkage` 只认精确三元组，而且**没有
`[build] linkage` 这个键**——写了会得到一条「unsupported key (ignored)」
警告，并且不会切到静态 CRT。（本页在前几节已经明确说过这一点，而这里却给出
了一个错误的写法。）在 MSVC ABI 上，这两者是同一个物理开关——`/MT`
从同一个库里链接 C 与 C++ 运行时——所以两种拼写选中的是同一件事，含义相同。
这是一个**整个工程**级的属性：一个工程只构建一份 `std` 模块，cl 会把
`_MSVC_MT`/`_MSVC_MD` 烘进这份模块，所以一个按角色给出的覆盖
（`cxx_runtime = { tests = … }`）会被拒绝，并给出一条说明，而不是在 ucrt
头文件内部产生一次模块不匹配。

### MSVC ABI 上的 clang：toolset 就是 sysroot

编译器是 clang 时（`windows = "llvm@<version>"`，装有 Visual Studio 的机器上的
默认值），MSVC toolset 是 clang 编译时所针对的东西：它的 STL、CRT，以及随它
而来的 Windows SDK。目标行用 `sysroot` 指定它，写法相同（2026.9.24.1+）：

```toml
[toolchain]
windows = "llvm@22.1.8"

[target.x86_64-windows-msvc]
sysroot = "msvc@14.44.35207"     # or "msvc@system" (the default), or "xim:msvc@14.44.35207"
```

mcpp 按上面的规则解析一次 toolset 与它的 SDK，并在每一次编译、链接和 `std`
模块预编译中以 `-Xmicrosoft-visualc-tools-root`、`-Xmicrosoft-windows-sdk-root`
与 `-Xmicrosoft-windows-sdk-version` 传给 clang；`std.ixx` 也取自同一个 toolset。
构建会打印这次选择：

```
    Resolved sysroot msvc@system → MSVC 14.44.35207 (system: Visual Studio Community 2022) · Windows SDK 10.0.26100.0
```

并记录在 `resolution.json` 里（`msvc_toolset`、`windows_sdk`）。toolset 目录与
SDK 版本都进入构建缓存键，SDK 版本也是这一行的运行时身份（`ucrt@<version>`），
与 cl.exe 行相同。在 cl.exe 行上，指向与编译器不同 toolset 的 `sysroot` 会被
拒绝。

**更早的引擎**（在 Windows runner 上用 2026.9.21.3 实测）对 `msvc@system` 与
`msvc@<toolset>` 拒绝整份清单，报「is not an xpkg reference」。它们接受
`xim:msvc@<toolset>`，但不按它行事：什么都不安装，clang 针对机器上的 toolset
编译，构建输出却把这个值列为 c-abi 层。依赖所写 toolset 的项目应把 mcpp 固定在
2026.9.24.1 或更高，例如写在 `.xlings.json` 的 workspace pin 里。

## SDK 工具链（`emsdk`、`android-ndk`）

五种工具链拼法里，有两种命名的是一个 **SDK** 而不是一个裸编译器：`emsdk`
与 `android-ndk`。它们的编译器*就是* clang——所以它们不是一个独立的编译器
族，mcpp 也不假装它们是——但那份归档自带 sysroot、自带 C 库，并且这两者都
自带一份生成好的 `std` 模块面。本节讲的就是这个差别。

### 默认无需声明

一个 target 行命名了它自己的载荷，而那个 pin 就是默认值。下面两者都不需要
在 `mcpp.toml` 里写一行：

```bash
mcpp build --target wasm32-emscripten     # resolves emsdk@6.0.9
mcpp build --target aarch64-linux-android # resolves android-ndk@30.0.16248370
```

载荷在某个 target 第一次需要它时**按需安装**，和一个 gcc 或 llvm 载荷完全
一样。`mcpp toolchain list` 会在那一行旁边显示这个 pin，而构建会报出是哪份
归档回答的：

```
Resolved emsdk@6.0.9 → wasm32-emscripten → …/xim-x-emsdk/6.0.9/emscripten/em++
Resolved android-ndk@30.0.16248370 → aarch64-linux-android → …/prebuilt/linux-x86_64/bin/clang++
```

### 也可以显式声明

普通的按 target 键照常可用，而点名该行自己的载荷总是被接受：

```toml
[target.aarch64-linux-android]
toolchain = "android-ndk@30.0.16248370"

[target.wasm32-emscripten]
toolchain = "emsdk@6.0.9"
```

用它可以把版本跨机器钉住，或者选用比该行约定更新的载荷。**版本是自由的**——
索引里发布过的都能解析——所以一个工程正是靠这一点走在默认值之前，或者留在
默认值之后。

### 不可覆盖的部分，以及原因

对这两行，那个 pin 是一个**能力**而不是一个约定：它不是 mcpp 在几个都能服务
该 target 的载荷之间的偏好，而是唯一能服务它的东西。所以载荷的**名字**是
固定的，版本是开放的：

```toml
[target.aarch64-linux-android]
toolchain = "llvm@22.1.8"        # refused
```

```
error: target 'aarch64-linux-android' cannot be emitted by 'llvm@22.1.8'.
       An Android target needs bionic, not just an aarch64 or x86_64 back end:
       its headers, its per-API-level stubs and its loader path are inside the
       NDK, and no package adds them to another compiler.
```

这次拒绝与代码生成无关。一个普通 clang 发 aarch64 ELF 完全没问题；它拿不出
来的是**体系**。在声明处就说出来，比解析出 llvm 再在构建深处失败要好——而
后者正是这道闸存在之前会发生的事：先是 `'__config' file not found`，然后是
bionic 自己头文件里的 `Unversioned target triples are not supported!`，两句
都没有点名那个服务不了这一行的工具链。

`wasm32-emscripten` 按同一条规则、用它自己的句子拒绝：除了 Emscripten，没有
东西能发 WebAssembly。

### 属于工程的那一半

工具链属于 SDK；**部署下限**属于工程，它按平台各自拥有自己的键——见
[04 —— mcpp.toml](04-mcpp-toml.md) §2.7.3：

```toml
[target.aarch64-linux-android]
min_api_level = 24               # Android
```

```toml
[package]
macos_deployment_target = "14.0" # Apple
```

一个 NDK 服务一段 API level 的区间，所以这个级别是工程自己的决定，点名
`android-ndk@<version>` 并不会钉住其中任何一个。不写这个键时，mcpp 读 NDK
自己在 `meta/platforms.json` 里声明的下限。

### 产物的运行方式

模拟器和真机都不属于工具链这根轴。wasm 模块需要的是解释器而不是模拟器，由
载荷说出它是谁：`xim:emsdk` 在自身旁边写出 `.mcpp-toolchain.json`，其中的
`runner` 键就是它所依赖的 `xim:node` 载荷里的 `node`。项目与依赖都没有声明
runner 时，`mcpp run` 与 `mcpp test` 使用这个程序，因此不涉及 PATH 上的
`node`。载荷的 runner 是一个程序，产物路径追加在其后；它或者是载荷内的相对
路径，或者是持有该载荷的包存储之内的绝对路径，存储之外的路径一律忽略。在
配方写出这个键之前安装的载荷没有描述文件，其产物仍按自身的
`#!/usr/bin/env node` 这一行运行，和之前一样。要让这样的载荷获得描述文件，
先刷新索引再重装：`mcpp index update`，然后 `mcpp toolchain remove <spec>`
与 `mcpp toolchain install <spec>`。仅仅重装会再次按同一份配方安装，因为
索引只在解析未命中时刷新，而不因配方变化而刷新；这一点在 2026.9.12.2 的
沙箱验证中实测过。在此之前，失败信息来自解释器自身
(`/usr/bin/env: 'node': No such file or directory`)，mcpp 尚未对它作出解释
(#621)。

对于产物在别处运行的 target，`runner` 键是一个 argv 前缀，而那个会话属于
一个**包**，不属于引擎：

```toml
[target.x86_64-linux-android]
runner = ["adb-run"]             # a program from xim:android-platform-tools

[target.aarch64-ios-sim]
runner = ["simctl-run"]          # a program from xim:apple-simulator-tools
```

runner 是一个 argv 前缀，而一次**会话**不是。在一台 iOS 模拟器上运行一个
程序意味着：挑一台设备、若未启动则启动它、等待启动完成、spawn，并把程序自己
的退出状态返回回来。清单里的一行没有开始也没有结束，这正是那部分工作住在
一个包里的原因。

## Apple 的 SDK 是被定位的，不是被安装的

三条 iOS 行——`aarch64-ios`、`aarch64-ios-sim` 与 `x86_64-ios-sim`——是一个
平台可以取的另一种形状，值得与上面两个 SDK 工具链放在一起读，因为它们回答的
是同一个问题，只是答法不同。

**编译器是我们的；只有 SDK 是 Apple 的。** 任何足够新的 clang 都能为一个
iOS 部署目标产出 arm64 Mach-O，所以这三行钉的是 `llvm@22.1.8`——那个普通
载荷，和 `aarch64-macos` 用的是同一个。无法打包的是 iPhoneOS 与
iPhoneSimulator 的 SDK：它在 Xcode 里，而且不可再分发。所以 mcpp **定位**
它，通过 `xcrun --sdk <name> --show-sdk-path`，与它一直以来定位 macOS SDK
的方式完全相同。

这也是这三行不带 `sysroot` 条目的原因。那一列命名的是一个包，而一个被定位的
目录不是包。

```bash
mcpp build --target aarch64-ios        # resolves llvm@22.1.8 + the iPhoneOS SDK
mcpp build --target aarch64-ios-sim    # resolves llvm@22.1.8 + the Simulator SDK
```

## mcpp 保留的宿主面，以及每一项的理由

这个引擎围绕的那条规矩是：**一次构建可复现，当且仅当造出它的工具来自下一台
机器也能取得的地方。** 因此每一个参与构建的工具都来自依赖图或 xlings，而
下表是**不**来自那里的全部——每一条都带着它不属于生态的理由。

| 项 | 到达方式 | 不在生态里的理由 |
|---|---|---|
| **xlings 自己** | 随 mcpp 自己的载荷捆绑（`[xlings] binary = "bundled"`，默认值）。`"system"` 才会去查 PATH。 | 自举：总得有东西去取第一个包。默认是捆绑的那份，所以走宿主是用户主动做出的一个选择，不是一次兜底。 |
| **一个 Apple SDK** | 经 `xcrun` 定位，从不安装 | 不可再分发。没有东西可以打包，而拒绝会在解析任何载荷之前就点名它。 |
| **iOS 模拟器运行时** | `simctl`，经由 `xim:apple-simulator-tools` | 同上。 |
| **一个汇编器（`nasm`）** | 先取被钉住的 `xim:nasm`；只有那条路服务不了时才取宿主的，且**在构建报告里点名**用到的是哪一个 | 一台离线、但本来就装了可用汇编器的机器仍然能构建。它此前是反过来的——见下文。 |
| **PATH 上的 C++ 编译器（`$CXX`，否则 `g++`）** | 只有 `mcpp doctor` | 那个命令的职责就是报告宿主的状况。构建这条路上，在每一个到得了这个探针的分支上都从解析出的载荷设定编译器，载荷解析不了时**拒绝**，而不是落到 PATH。 |
| **一个命令解释器（`/bin/sh`，Windows 上是 `cmd.exe`）** | `[hooks]` 走 `run_shell_deadline`，xlings CLI 走 `run_streaming_bounded`，还有一个分离式的代码生成命令 | 一条 hook 是**用户自己**用 shell 语法写下的那一行。自带一个 shell 会改变那一行被解读所用的语言，所以这里依赖的不是一个能打包的工具，而是宿主对那一行含义的约定。 |
| **MSVC 工具集与 Windows SDK** | 用户点名的 `msvc@system`；在机器上找到的 pin 版本 `msvc@<toolset>`；或者一个受管工具集旁边没有 SDK 载荷时 | 不可再分发，与 Apple SDK 同一类。受管工具集**绑定**自己的 SDK，即使 `WindowsSdkDir` 被设置也不理会——一个环境能覆盖的 pin 就不是 pin；回落到机器自己那份能用，但不可复现，因此会带一句说明（`SdkChoice::note`，调用方必须把它呈现出来）。 |

这张表意在穷举，而它是**推导出来的**，不是靠记忆写出来的。四次扫描 `src/`
与 `modules/` 就能重新得到它。每一处 `fs::which` 调用——恰好五处，每一处都
对应上面的一行：`toolchain/probe` 取 `$CXX`，`config` 与
`fallback/xlings_binary` 里的两处取 xlings，`xlings/xlings` 取汇编器。每一个
以 `/usr`、`/bin`、`/opt`、`/etc` 为根的字符串字面量。每一处 `cmd.exe` 与
`COMSPEC` 的使用。以及每一个读取宿主工具链所设变量的
`getenv`——`CXX`、`SDKROOT`、`WindowsSdkDir`、`WindowsSdkVersion`、
`VSINSTALLDIR`、`MACOSX_DEPLOYMENT_TARGET`（其余 `getenv` 读的是 mcpp 自己
的 `MCPP_*` 旋钮与 `HOME`，不指向任何宿主工具）。这张表的第一版只做了第一次
扫描，因此缺了最后两行；把扫描方式写在这里，是为了让下一个读者去核这张表，
而不是去信它。

其中两条扫描结果值得点名，因为它们指向相反的方向。`mcpp.toolchain.registry`
会**拒绝**一个 `frontend` 写着 `/usr/bin/g++` 或者用 `../` 爬出去的载荷
描述符——而且是按**字符串**校验，不经 `std::filesystem::path`，因为
`path("/usr/bin/g++").is_absolute()` 在 Windows 上为假，那道闸恰恰在那台
宿主上没有看到问题（2026-09-11 实测）。而 `src/runtime/elf` 里写下
`/usr/lib` 与 `/usr/lib64`，只是为了**建模**运行期加载器将要搜索的地方，
mcpp 不从那里读任何东西。

其余的一切都来自依赖图或 xlings，包括最常被误认为是宿主的那几个：编译器与
链接器（一个载荷）、C 库与 C++ 运行时（几个包）、`ninja` 与
`patchelf`(xlings)，以及 `ar` / `strip` / `objcopy`（从解析出的工具链自己的
目录派生，**从来不是一个裸名**——`mcpp.toolchain.registry::binutils_tool`
要么返回一个存在的绝对路径，要么什么都不返回）。

**汇编器是唯一指错了方向的一个，值得记录下来，而不是悄悄掉个头
(2026.9.20.1)。** `find_usable_nasm` 此前先问 PATH，再看沙箱。装了汇编器的
机器用它自己的那一份，没装的机器则下载被钉住的那一份。三台机器可能从同一棵
源码树产出三份不同的目标文件，而两次构建都没有一行说出它用的是哪一个
汇编器。现在这个顺序反过来了；当服务这次构建的是宿主那一份时，构建会说出来：

```
warning: the assembler for this build is the host's ('/usr/bin/nasm'), not the one this engine pins
  impact: two machines can assemble the same source with different assemblers, and the build records only this line
  hint: run `xlings install nasm` so the pinned copy is used
```

**一个宿主工具到达构建，本身不是缺陷；一个宿主工具静默地到达构建才是。** 这
正是上面是一张表而不是一条禁令的原因：每一条都到得了，每一条都有自己的理由，
而且每一条都会在被用到的地方说出来。

### 这条路新增的宿主面，具名且有界

两项，都只在 macOS 上，都落在「一个只存在于它自己那个操作系统上的专有运行时」
这一类里：

| 项 | 经由 | 被允许的依据 |
|---|---|---|
| iPhoneOS / iPhoneSimulator SDK | `xcrun` | 不可再分发；没有东西可以打包 |
| 模拟器运行时 | `simctl`，经由 `xim:apple-simulator-tools` | 同上 |

其余一切都来自生态：编译器、C++ 运行时、链接器与打包。这两项都不是通过一次
兜底到达的——每一项都是被有意请求的，而它的缺失会是一次点名它的拒绝：

```
error: target aarch64-ios needs the iphoneos SDK, which this machine does not provide.
       It is not redistributable, so mcpp LOCATES it rather than installing it:
       `xcrun --sdk iphoneos --show-sdk-path` must answer, which needs Xcode on
       macOS (not the Command Line Tools alone -- those ship the macOS SDK only).
       Check `xcode-select -p`, and note that the compiler is not what is
       missing: these rows pin `xim:llvm`, which every other Apple row also uses.
```

这次拒绝发生在**任何载荷被解析之前**。一个 Apple SDK 不是依赖能供给的东西，
所以没有任何后续步骤能学到会改变这个答案的信息——而一台没有 Xcode 的机器，
不应该先下载一个编译器，然后才被告知缺的不是编译器。

### 这些行上的 C++ 运行时与编译器运行时是包

这份载荷的静态 libc++ 是一个 macOS 目标文件，ld64 拒绝把它链进一次 iOS
链接；载荷的资源目录里只有 `libclang_rt.osx.a`，没有 `ios` 或 `iossim` 的
归档。因此这两层都来自依赖图，与裸机行上的 C 库和 builtins 是同一做法：

```toml
[target.'cfg(os = "ios")'.dependencies]
llvm.libcxx               = "22.1.8.2"   # libc++ and libc++abi as source, with the std module
llvm.compiler-rt-builtins = "22.1.8.5"   # __isPlatformVersionAtLeast and the generic routines
```

一个 framework 声明这两行一次，每个应用都继承它们。报告把两层都记为图里的，
链接行带 `-nostdlib++`，产物的加载命令里不含 `libc++.1.dylib`：一个翻译单元
编译所依据的头文件、它导入的模块，与最终链接的目标文件，按构造是同一个发布
版本。

不声明第一行时，运行时就是 SDK 的 libc++。不导入 `std` 的程序因此也会取
SDK 的头文件（`-nostdinc++ -isystem <sdk>/usr/include/c++/v1`）并链接
`-lc++`。导入 `std` 的程序仍然用载荷的模块与头文件，配 SDK 的 dylib，和此
版本之前每一次 iOS 构建的做法相同；这是 libc++ 的两个不同发布版本，prepare
会报告一次这个搭配并点名上面两行，因为它只能在较新头文件里的内联路径没有
引用旧 dylib 缺失的导出时才能链接成功。不声明第二行时，prepare 会报告一次
「载荷没有这个平台的编译器运行时」；从未触及可用性检查的程序仍照常链接。

### 部署目标

`[build] ios_deployment_target` 与 `macos_deployment_target` 并列，而两者
分成两个键只有一个理由：`"14.0"` 是一个 macOS 版本号，对一个 iOS SDK 毫无
意义。任何一个给定的 target 上，两者只有一个能成立，所以它们共用构建指纹里
的同一个槽位。

```toml
[build]
ios_deployment_target = "18.0"
```

它由**有效三元组**承载，别处都不承载：

```
Target aarch64-ios     → arm64-apple-ios18.0
Target aarch64-ios-sim → arm64-apple-ios18.0-simulator
```

这是 Apple 自己的拼法。不会发出 `-miphoneos-version-min` 标志：三元组已经
完全决定了平台与最低版本，一个标志只会成为第二个回答三元组已经回答过的问题
的地方。不写这个键是合法的，含义是取定位到的 SDK 自身的版本：mcpp 用
`xcrun --sdk <name> --show-sdk-version` 读出它，并写进同一个槽位。clang 对
一个不带版本的 iOS 三元组给出的默认值，比机器上任何 SDK 都旧（它拒绝了
libc++abi 用到的线程局部存储；在 Xcode 16.4 上实测过），所以三元组在驱动
看到它之前就已经带上了版本。Android 的差别在另一个方向：bionic 会直接拒绝
一个不带版本的三元组。

### 模拟器的形状与它的边界

模拟器是两行，而不是设备行上的一个标志。一次模拟器构建有它自己的 SDK，产出
的对象命名它自己的平台（`LC_BUILD_VERSION` 报告 `IOSSIMULATOR` 而不是
`IOS`），并取一个不同的部署目标段。两个架构都存在，是因为模拟器跑的是
**宿主**的架构：一台 Apple 芯片的机器需要 `aarch64-ios-sim`，一台 Intel 的
需要 `x86_64-ios-sim`。

设备行的 `runner` 保持未设置。没有开发者自己拥有的签名，一个产物无法在一台
iOS 设备上运行，而那不是一个构建工具能供给的东西。

## 项目级版本锁定

若工程需要锁定某个特定版本，而不依赖全局默认值，可在工程自己的 `mcpp.toml`
中声明：

```toml
[toolchain]
default = "gcc@16.1.0"
linux   = "gcc@16.1.0"
macos   = "llvm@20.1.7"
```

工程级声明优先于全局默认配置。

## Target 与交叉构建

```bash
mcpp build --target x86_64-linux-musl        # fully static ELF
mcpp build --target aarch64-linux-musl       # cross-arch (aarch64 on x86_64)
mcpp build --target x86_64-windows-gnu       # Windows PE from Linux
```

`--target` 会对照已知的 target 词汇表做校验（README 的平台表与之同源）：
拼错是**硬错误并附带建议**(`did you mean 'x86_64-linux-musl'?`)——绝不会
静默退回到宿主构建。词汇表之外的自定义三元组，在 `mcpp.toml` 中显式声明
`[target.<triple>]` 段即可放行。

每个已知 target 自带一个约定：被钉住的工具链（按需自动安装）与默认
linkage（`*-linux-musl` 与 `x86_64-windows-gnu` 默认静态）。一个显式的
`[target.<triple>]` 段可以同时覆盖两者：

```toml
[target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
linkage   = "static"
```

### 约定可以被推翻，能力不行

**有宿主**的那一行，它的 pin 回答的是「哪个载荷供给这个 target 的 C
库」，所以一个自己供给 C 库的工程可以写任何编译器——整个 openkal 生态就建立
在这道口子上。它不能做的是写一个不同的编译器、却什么都不供给：

```
$ mcpp build --target x86_64-linux-musl        # [toolchain] default = "llvm@…"
error: target 'x86_64-linux-musl' takes its C library from the 'gcc@16.1.0'
       payload, and 'llvm@22.1.8' has none here.
```

有两类行回答的是另一个问题，它们的 pin 根本不可能被推翻：

| 行 | 原因 |
|---|---|
| 所有 `*-none-elf` | 不存在按宿主分的交叉载荷；clang 与 lld 按构造就是交叉编译器，gcc 不是 |
| `x86_64-windows-musl` | 没有任何 gcc 载荷能发出 PE + musl C 库的组合——mingw 载荷发出的是 PE + MinGW CRT，那是隔壁 `-gnu` 那一行 |

```
$ mcpp build --target riscv64-none-elf         # [toolchain] default = "gcc@…"
error: target 'riscv64-none-elf' cannot be emitted by 'gcc@16.1.0'.
```

**两处拒绝都发生在做出决定的地方**，而不是留给编译器。在 2026.8.26.1
之前，前者会跑完整个构建，然后死在链接上，报 `crtbeginT.o (bare name)`；
后者则给出 `g++: error: unrecognized argument in option '-mabi=lp64d'`——
一条关于选项的消息，而决定在一百行之前就已经做出。

给这些结果分类的程序，读的是 `mcpp why toolchain --format json` 里的
`data.reason`(`convention-unreplaced` / `capability-pin`)，而不是那句话
本身——见[第 50 章](50-machine-output.md)。

一个工程还可以声明自己的*默认* build target——「本工程发布全静态产物」这
类语义就该放在这里（全静态是产物的属性，不是编译器族的属性）：

```toml
[build]
target = "x86_64-linux-musl"                 # ≙ cargo's build.target
```

配合 `mcpp pack --mode static`，即可产出一个全静态的发布包；完整示例参见
[`examples/03-pack-static`](../../examples/03-pack-static/)。

## 卸载

```bash
mcpp toolchain remove gcc@16.1.0
```

## 重置沙盒

```bash
rm -rf ~/.mcpp                              # remove the entire sandbox
mcpp build                                  # the next build triggers first-run installation again
```

## 环境变量

mcpp 的运行行为可以通过下列环境变量调整：

| 变量 | 用途 |
|---|---|
| `MCPP_HOME` | 覆盖沙盒位置（默认 `~/.mcpp/`）；绝对路径优先级最高 |
| `MCPP_NO_AUTO_INSTALL=1` | 禁用工具链自动安装，适用于 CI 与离线环境 |
| `MCPP_OFFLINE=1` | 完全不访问网络，等价于全局 `--offline` |
| `MCPP_NO_COLOR=1` / `NO_COLOR=1` | 禁用彩色输出 |
| `MCPP_LOG_LEVEL=debug\|info\|warn\|error\|off` | 日志级别 |

未显式设置 `MCPP_HOME` 时，mcpp 会基于二进制所在目录的上一级路径自动定位
沙盒（一份 release tarball 解压到 `~/.mcpp/` 之后，`~/.mcpp/` 就是 home），
所以 release 版本不需要任何环境变量配置即可运行。

## ABI 能力强制

一个依赖可以声明 `abi:<name>` 能力（例如 `compat.glfw` 声明
`abi:glibc`）。当解析出的工具链的 ABI 不满足某个依赖的 abi 要求时，构建会
**尽早失败**并给出修复建议（例如一个 musl-static 工具链遇到一个
abi：glibc 依赖），取代更深层的链接或头文件报错。查看方式：
`mcpp why toolchain`。

## 已知工具链风险：模块接口中的运算符模板（Clang 20+）

一个导出**替换性运算符模板**的模块，在 Clang 20 或 22 下会毒化所有导入者
中该运算符的名字查找：任何 `import` 了这个模块、并用到该运算符的翻译
单元——**无论作用在什么类型上**——都会让前端崩溃（SIGSEGV）。GCC 16 与
Clang 18 不受影响，所以这是 Clang 18 到 20 之间的一处回归。

它正好打在 module-package 这个模式上。包装一个运算符是 `static inline`
模板的上游头文件，再用一个恒真约束镜像它们的签名（这是跨翻译单元包含关系的
标准配方），恰恰就是踩中它的写法。

**经验判据：** 每个模板形参都应由**第一个**函数实参定死。破坏这一点的形状
就是有毒的：

```cpp
// Poisonous — `n` and `l` are not determined by argument 1
template<typename T, int m, int n, int l>
Matx<T, m, n> operator*(const Matx<T, m, l>& a, const Matx<T, l, n>& b);

// Poisonous — second typename appears only in argument 2
template<typename T1, typename T2, int n>
Vec<T1, n>& operator+=(Vec<T1, n>& a, const Vec<T2, n>& b);

// Fine — every parameter is pinned by argument 1
template<typename T, int m, int n>
Matx<T, m, n> operator+(const Matx<T, m, n>& a, const Matx<T, m, n>& b);
```

崩溃是**按名字**触发的：一处被毒化的 `operator*` 声明，会让每个导入者里
每一个 `x * y` 都崩溃，即使类型完全无关。函数体本身无关紧要。

**绕过方法**是整体推导操作数类型再加约束，而不是在形参列表里把它们拆开。
这样保持调用兼容，跨翻译单元的语义也仍然成立——上游那个精确匹配的
`static inline` 更特化，在那边照样胜出：

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
工具链上的金丝雀——未来某次 Clang 升级修好（或者再次弄坏）这一点时，它会
显式暴露出来，而不是悄悄改变包能表达的东西。

## 已知工具链风险：宽的可平凡比较类型上的 `std::find`(clang + MSVC STL 14.51)

编译器是 clang、标准库是 MSVC STL 14.51（Visual Studio 18）时，对宽度超过
八字节的可平凡复制类型调用 `std::find` 的翻译单元，会在标准库内部编译失败：

```text
xutility:320:23: error: static assertion failed: unexpected size
xutility:6542:49: note: in instantiation of function template specialization
  'std::_Find_vectorized<const T, T>' requested here
```

这正是 mcpp 在 Windows 上默认工具链的形态（clang 面向
`x86_64-pc-windows-msvc`），所以即使 mcpp 和程序本身都没有错误，这个失败也
会经由 `mcpp build` 出现。MSVC STL 通过一个只在 clang 下生效、且没有尺寸
上限的 trait，把这个类型纳入了向量化路径，而它分派到的函数只实现了 1、2、
4、8 字节的元素；MSVC 自己的前端从不走这条路径。同一份源码在 `windows-2022`
所带的 MSVC STL 14.3x 上可以编译。

缺陷在上游，跟踪于
[microsoft/STL#6294](https://github.com/microsoft/STL/issues/6294)。
下游实测过两种绕过方式：

- 为元素类型写一个用户定义的 `operator==`，而不是让编译器默认生成。该类型
  因此不再是可平凡相等比较的，STL 就会走标量路径。
- 在 MSVC STL 早于 14.51 的镜像上构建，例如 `windows-2022`。

跟踪于 [mcpp#609](https://github.com/mcpp-community/mcpp/issues/609)。

## C++ 运行时契约（`cxx_runtime`）

`cxx_runtime` 声明的是产物对运行它的那台机器做出的承诺。它是**分发**属性，
不是构建属性——它描述的是运行期依赖集，而兑现它的旗标逐平台不同。

> **不含 C++ 的 target 没有 C++ 运行时契约需要兑现。** mcpp 用 C 驱动链接
> 这样的 target，并完全不发出 C++ 运行时相关的旗标，因此一个纯 C 的共享库
> 不会平白拿到它用不上的 `libstdc++`/`libc++` 依赖。这个 target 里只要有
> 一个 C++ 翻译单元，整个 target 就回到 C++ 驱动。这一判定由源码推导得出，
> 没有对应的配置键。

```toml
[build]
cxx_runtime = "self-contained"          # applies to every target (the default)

# or, per role:
[build.cxx_runtime]
default = "self-contained"              # executables
tests   = "host-coupled"                # test binaries never leave this machine
shared  = "self-contained"              # shared libraries (see below — the
                                        # default differs by target format)

# or, per target triple — beside `linkage`, which is the same axis:
[target.x86_64-linux-gnu]
cxx_runtime = "host-coupled"            # e.g. this build is for a distro package
```

| 取值 | 产物在运行期的需求 | 典型用途 |
|---|---|---|
| `self-contained`（默认） | 自身之外不需要任何 C++ 运行时 | 分发一个二进制 |
| `toolchain-coupled` | mcpp 安装的那套工具链自带的 C++ 运行时 | 本地迭代 |
| `host-coupled` | 驱动默认解析到的那一份（即系统运行时） | 发行版打包；必须与宿主共用同一份运行时的 `dlopen` 插件 |

**默认即自包含（portable by default）**：在 macOS 上，这会静态链接 LLVM
自带的 libc++/libc++abi——否则系统的 libc++ 会把可运行版本钉死在构建机的
操作系统上（老系统缺少更新的符号，例如 `std::print` 背后的支撑符号），而
只有静态链接才能真正兑现 `macos_deployment_target` 的下限。在
Linux/MinGW 上，它是 `-static-libstdc++`（GCC）或者整条链接的
`-static`(MinGW)；在 Linux 的 clang/libc++ 工具链上，它显式链接
libc++.a/libc++abi.a/libunwind.a。更低的 macOS 下限（11–13）需要一份自建
的 libc++ 归档（已验证可行，是一次数据级的切换，可按需提供）。

**共享库是唯一一个默认值取决于 target 格式的角色**，因为它面对的危害本身就
随格式变化。一个 `.so`/`.dylib`/`.dll` 不是一个小号可执行文件——它被加载
**进**一个已经拥有 C++ 运行时的进程。

| target | `kind = "shared"` 的默认值 | 原因 |
|---|---|---|
| ELF（Linux 等） | `toolchain-coupled` | ELF 只有一个全局符号命名空间，先加载的定义胜出。一个静态内嵌了 libstdc++ 的 `.so` 会把它**导出**，链接该库的可执行文件于是把自己的 `std::` 引用绑定到那里——它自己的 `self-contained` 契约会静默变成空操作，它的 C++ 运行时变成碰巧加载到的那一份该库。 |
| Mach-O | `self-contained` | 那里的机制本来就是 `-load_hidden`，即隐藏可见性，dyld 因此从不归一这些符号；而且 macOS 上根本没有 toolchain-coupled 这一档（见下文注记）。 |
| PE(Windows) | `self-contained` | PE 没有全局符号命名空间——导入按 DLL 逐个按名解析，一个 DLL 的私有运行时不可能被别的东西捡走。 |

在 ELF 上显式写 `shared = "self-contained"` 是支持的，而且就是字面意思：
库会内嵌运行时。此时 mcpp 会额外为标准库归档传递
`-Wl,--exclude-libs`，让内嵌的那份留在库的动态符号表之外，链接它的任何
东西都捡不走。本工程代码产生的模板实例化（`std::string` 之类的
weak/COMDAT 符号）仍然会导出——那是 C++ ABI 的预期行为，不是这里要防的
泄漏。

一个工程级的 `cxx_runtime = "…"`（或 `static_stdlib = false`）同样作用于
共享库：有人已经说明了整个工程的承诺。只有在没人说明的情况下，随格式变化的
默认值才会生效。

**加载 C++ 共享库的程序**(mcpp 2026.9.16.1+)。在 ELF 上，上面两个默认值
会在同一个进程里发生冲突：一个 self-contained 的程序加载
toolchain-coupled 的 C++ 共享库，进程里因此同时存在一份静态 C++ 运行时和
一份共享的。可执行文件导出该库引用的运行时符号，库把其中一部分绑定到程序的
那份、其余留给自己，两半对共享状态的认识因而不一致；实测中，用
`llvm@22.1.8` 构建的这类程序，会在库第一次格式化字符串时以
`std::bad_cast` 中止，用 `gcc@16.1.0` 构建则能跑，但有 900 个
libstdc++ 符号被抢占。因此在 ELF 上，一个没有人为它声明契约的程序或测试，
如果直接或经由另一个共享库加载了本次构建产出的 C++ 共享库，就会取用该
共享库的契约。进程本来就通过库自己的 `NEEDED` 条目需要那份运行时，所以
没有任何部署会因此多出一项要求；`resolution.json` 会在
`runtime.cxx_runtime_by_role` 下记录最终生效的契约。

已声明的契约从不会被改动。一个**声明了** `self-contained` 的程序或测试，
若加载了耦合到共享运行时的 C++ 共享库，会在编译前被拒绝（reason
`program-cxx-runtime-split`，见[50](50-machine-output.md)）；出路是删掉
这条声明，或者用 `cxx_runtime = { shared = "self-contained" }` 给共享库
一份私有副本，让每份运行时都留在各自的映像里。

**在 Mach-O 上，每个映像各自携带自己的运行时**(mcpp
2026.9.16.1+)。那里每个角色的默认值都是 self-contained，每个映像以隐藏
可见性内嵌载荷的 `libc++.a`，于是标准库类的类型信息在每个映像里各有一份，
而 libc++ 按地址比较它。在 macos-15 上实测：按默认值，dylib 里抛出的
`std::runtime_error` 不会被程序里的 `catch (const std::runtime_error&)`
捕获，两个 `std::error_code` 的 category 比较也不相等；当所有角色都写
`cxx_runtime = "host-coupled"` 时，两者都成立。默认值不变——它是今天每个
macOS 构建的形态——但当一次构建的程序加载了本次构建产出的 C++ dylib 时，
会被告知一次（`build/cxx-runtime-identity`）：当对象以异常的形式、或以
按身份比较的 libc++ 值的形式跨越这条边界时，应当为整个进程声明同一份
运行时。

**依赖的共享库之上，C++ 运行时是一个包时**(mcpp
2026.9.15.2+）。当依赖图中有一个包提供 C++ 层（`llvm.libcxx`，见
[22](22-target-side.md)）时，它的对象会被链进程序，并且以隐藏可见性编译，
于是一个构建为 C++ 共享库的依赖无法解析到程序里的那一份。mcpp 会在编译前
拒绝这样的构建（reason `shared-library-cxx-runtime`，见
[50](50-machine-output.md)），并给出两条出路：在依赖的边上写
`linkage = "static"` 把它静态链接——只有当依赖自己的 manifest 没有把它
约束成共享形态时，才会给出这一条；或者为共享库声明一份私有副本：

```toml
[build]
cxx_runtime = { shared = "self-contained" }
```

在这条声明下，每个这样的共享库都会自己链接运行时包的对象。于是每个映像各自
持有运行时类的类型信息：一个标准库类的异常在共享库里抛出时，程序里的该类不
会捕获它，而共享库自己定义的类则会。默认值从不会选中这份私有副本，只有这条
声明会。在 Mach-O 上，当图中一份构建的 C++ 运行时是一个包时，每个单元都以
隐藏可见性编译，使得运行时的实例化永远不会与系统的 libc++ 合并；那里的共享
库因此只导出源码中标注了 `[[gnu::visibility("default")]]` 的声明，别无
其他。

`static_stdlib` 是较旧的拼写，仍然有效：`true` 等价于
`self-contained`，`false` 等价于 `host-coupled`。显式写了 `cxx_runtime`
时，以它为准。

**兑现不了的契约会被报出来，绝不会被静默降级。** 如果工具链不带
`libc++.a`，或者某个契约在该平台上没有对应的机制，构建会打印它实际退到了
哪一档，而不是悄悄交付一个与 manifest 所述不同的产物。

### 在 MSVC 运行时上

这里的机制就是 CRT 模型，而它是一个**整个工程**级的开关：cl 会把
`_MSVC_MT`/`_MSVC_MD` 烘进一个工程唯一构建的那份 `std` 模块，所以一个与
工程不一致的按角色契约无法被兑现，会被报出来，而不是被忽略。

| 取值 | 在 MSVC 上的含义 |
|---|---|
| `self-contained` | `/MT`——静态 CRT。`linkage = "static"` 从 libc 那根轴选中的是同一件事。 |
| `host-coupled`（`/MD` 下的默认值） | 由目标机器提供 `vcruntime140.dll` / `msvcp140.dll`——即那台机器装了 Visual Studio 或对应的 redistributable。 |
| `toolchain-coupled` | toolset **自带**的那份 DLL 跟着产物一起走。 |

`toolchain-coupled` 值得说清楚，因为直觉上的理解是错的。`ucrtbase.dll`
**是**一个 Windows 组件（Windows 10 起），mcpp 从不分发它；而
`vcruntime140.dll` 与 `msvcp140.dll` **不是**：每个 MSVC toolset 都在
`VC\Redist\MSVC\<version>\<arch>\` 下带着它们，和一个 gcc 载荷带着
`libstdc++.so` 是同一件事。在这份契约下，mcpp 会把它们放到产物旁边——这
正是让一次默认的 `/MD` 构建，能在一台只装了被钉住的 toolset、完全没有
Visual Studio 的机器上运行起来的原因。

调试版 CRT（`debug_nonredist\` 下的 `vcruntime140d.dll` 等）永远不会被
放进去：它不可再分发。

> **从 2026.8.15 或更早版本升级时的变化。** 这个键在 MSVC ABI 上曾经是
> **空操作**——它会报 `not implemented for the MSVC runtime yet`，写任何
> 值都会退回 `/MD`。自 2026.8.16 起它真的会生效，于是一份从那个年代带着
> `cxx_runtime = "self-contained"` 的 manifest，**会在升级时换掉 CRT
> 模型**：从 `/MD` 变成 `/MT`。它不是同一个模型的更严格版本，而且因为
> 这个值一直是合法的，这次切换是**静默**的。如果一个工程是在这个键尚未
> 生效时写下它的，应当重新确认所需的取值。

把它和 `/MT` 一起写是一处**矛盾**，而不是缺功能——一份静态 CRT 根本没有
DLL 可以耦合——所以它会被报出来，并落回 `self-contained`。另一半由
`mcpp pack` 兜底：一个什么都不打包的模式（`--mode system`、
`--mode static`）兑现不了 `toolchain-coupled`，会直接拒绝。

**MSVC ABI 上的 clang**（`x86_64-windows-msvc` 的 `llvm` 行，记录自 mcpp
2026.9.16.1 起）。上表描述的是 `cl.exe`，mcpp 只向它传递 CRT 模型。MSVC
ABI 上的 clang 使用 GNU 方言，收不到任何模型，它的驱动链接静态 CRT
(`-defaultlib:libcmt`)：这一行构建出的程序不会导入
`vcruntime140.dll`、`msvcp140.dll` 或 `api-ms-win-crt-*`，每个 DLL 各自
带着自己的 CRT。因此这一行无论 `cxx_runtime` 写什么都是
`self-contained`，`resolution.json` 如实记录这一点，显式写
`host-coupled` 或 `toolchain-coupled` 会打印这一行兑现不了它。需要在
MSVC ABI 上使用动态 CRT 的工程，应当用 `msvc@system` 构建。

**边界。** 该契约只管辖 C++ 运行时。静态 **libc** 是另一根轴
(`linkage = "static"` / `--static`，例如一个 musl target)，部署下限是
第三根轴——Apple target 用 `[package]` 里的
`macos_deployment_target`，Android 用 `[target.<triple>]` 下的
`min_api_level`。两者都喂给 `llvm_triple()` 的同一个参数、占据构建指纹
里的同一个槽位，因为一个 target 要么是 Apple 的要么是 Android 的，而两者
回答的是同一个问题。另外，`host-coupled` 只意味着 mcpp 不做任何「把 C++
运行时打进产物」的动作；它不会去掉链接因其他原因已经携带的工具链
rpath——所以在 ELF 上，这类产物仍可能优先找到工具链自己的库。

> **macOS + `self-contained` 与静态初始化次序。** Mach-O 没有按优先级
> 排序的初始化段，而 libc++ 的 `<iostream>` 也不像 libstdc++ 与 MSVC
> STL 那样自带 `ios_base::Init` 守卫——于是从 `libc++.a` 里拉出来的流
> 初始化器，本来会排在程序自己的全局构造函数**之后**运行：一个在构造函数
> 里碰了 `std::cout` 的全局对象，会读到一个尚未构造的流，并在进程启动时
> 崩溃。mcpp 会把一个极小的生成对象排在链接最前面，把流的初始化顶上去；
> 调用方代码不需要做任何事。详见 mcpp-community/mcpp#336。

`defines` 接受**裸**的宏名（不带 `-D`），把每个条目脱糖为
`-D<x>`，同时作用于 C 与 C++ 两条编译通道。它覆盖包内每一个翻译
单元——包括模块接口单元——因此也会进入编译器自己的 P1689 模块扫描。

> **但它不会让被宏保护的 `import` 变得可用。** mcpp 会在编译器看到文件
> 之前先跑自己的词法预扫描，而那个扫描器对**任何** `#if` / `#ifdef`
> 块内的 `import` 一律拒绝，不会对条件求值：
>
> ```
> error: import statement inside conditional preprocessor block (forbidden in M1)
> ```
>
> 所以即使 `FOO` 写在 `defines` 里，`#ifdef FOO` / `import bar;` 这一对
> 仍然会失败。替代写法是把条件放在全局模块片段里的一个 `#include` 上。
> 跟踪于 mcpp-community/mcpp#421。汇编单元同样会拿到它。它是一个普通的
构建输入，所以 `[target.'cfg(...)'.build]` 也能承载它：

```toml
[build]
defines = ["APP_NAME=\"demo\""]

[target.'cfg(windows)'.build]
defines = ["USE_WIN32", "WINVER=0x0A00"]
```

选择合适的轴：

| 想让宏作用于…… | 用 |
|---|---|
| 本包每一个翻译单元 | `[build].defines`（本节） |
| 只作用于某个二进制自己的入口源文件 | `[targets.<name>].defines` |
| 一批指定的文件 | `[build].flags` 配 `glob` + `defines` |
| 本包每一个翻译单元**以及**消费方每一个翻译单元 | `[features.<name>].defines`（接口层的贡献） |

`[build].defines` 是包私有的：不会传播给消费方。

`[build]` 下不受支持的键会作为警告报出（`--strict` 下为错误），而不是被
静默忽略。

不要通过 `build.cxxflags = ["-std=..."]` 配置 C++ 标准。请改用：

```toml
[package]
standard = "c++26"
```

mcpp 会把同一个标准应用到普通 C++ 编译、模块扫描、
`compile_commands.json`，以及 `import std` 所需的标准库 BMI 构建。

**glob 排除**(`!` 前缀，mcpp 0.0.4+)：

```toml
[build]
sources = [
    "src/**/*.cpp",
    "!src/**/*_test.cpp",       # Exclude test files
    "!src/**/*_fuzzer.cpp",     # Exclude fuzzers
]
```

**per-glob 旗标**(mcpp 0.0.95+)：`[build] flags` 是一个**有序**的内联
表数组，把额外的编译旗标只附加到一个 glob 命中的那些源文件上——这是
SIMD dispatch 翻译单元与三方代码告警隔离的正解：

```toml
[build]
flags = [
  { glob = "third_party/**",            cflags = ["-w"], cxxflags = ["-w"] },
  { glob = "src/simd/**/*.avx2.cpp",    cxxflags = ["-mavx2"], defines = ["HAVE_AVX2"] },
  { glob = "src/x86/**/*.asm",          asmflags = ["-DPREFIX"] },
]
```

每条目的键：`glob`（相对包根，必填），加上 `cflags` / `cxxflags` /
`asmflags` / `defines`（没有 `ldflags`——链接没有按翻译单元的作用域）。
声明顺序即应用顺序：靠后的条目，它的旗标排在命令行更后面，配合 GNU「后
旗标胜」的规则，一个窄 glob 放在一个宽 glob 之后就能覆盖它。所有命中的
条目都会生效。这些是私有构建旗标——它们不会传播给消费方。一个零命中的
glob 会打印警告（一个拼错的 glob 不允许静默地什么都不做）。

**生成文件**(mcpp 0.0.95+)：`[generated_files]` 把一个相对路径映射到
文件内容（支持 TOML 多行字符串）。这些条目会在源码 glob 展开之前写入
工程树——与索引描述符合成模块包装文件用的是同一套机制——内容会进入
指纹，所以编辑它会触发重建：

```toml
[generated_files]
"src/gen/wrap.cppm" = """
module;
#include <vendored.h>
export module wrap;
"""
```

路径必须留在工程根之内（`..` 或绝对路径是解析错误）。

**汇编源文件**(mcpp 0.0.95+)：`.S`/`.s`（GAS——由 C 驱动预处理，覆盖
ARM 与 AT&T 语法的 x86）和 `.asm`（NASM——Intel 语法的 x86）是一等公民的
源文件：默认被 glob 收录、进入指纹、增量并行构建，并像任何其他对象一样
被链接。NASM 的输出格式由 target 三元组推导得出
（`elf64`/`win64`/`macho64`/...——交叉构建天然可用），而 `nasm` 本身只有
在存在 `.asm` 单元时才会被惰性解析：先看 `PATH`，再看 mcpp 沙盒，再
`xlings install nasm`；如果哪一处都得不到 ≥2.16 的 nasm，构建会**硬
失败**（汇编绝不会被静默跳过）。限制：`.asm` 只支持 x86 target（其他
target 上是硬错误——需要用条件 sources 把这些文件挡在外面），
`.S` 在 MSVC 工具链上不可用，而 `.asm` 就是指 NASM 语法（MASM 源码应当
用 `!` 排除）。

## 当前边界

- **一台 macOS 宿主完全没有面向 Linux 的载荷**，任何 Linux target 从那里
  都够不到。这是一条发布边界，不是引擎边界。
- manifest 里显式写的 `[toolchain]`，或者 `[target.X].toolchain`，
  **永远不会被推翻**。一个工程钉住了一个编译器，随后遇到这个钉子服务不了
  的 target 时，会被拒绝，而不是被悄悄换成另一个编译器。
