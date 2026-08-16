# 03 — 工具链管理

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

`*` 标记当前的默认对。Targets 块是 target 词汇表的实时视图:`installed`
为已装的链,`available` 为本宿主可安装的 target,`planned` 为已登记但尚未
发布的 target。

## Windows PE 之 MinGW-w64(`x86_64-windows-gnu`,无需 Visual Studio)

**没装 Visual Studio 时,这就是 Windows 上的默认值。** Windows 自带的只有
UCRT 运行时 DLL,MSVC STL 与 Windows SDK 都只随 Visual Studio 的
"Desktop development with C++" 负载安装。而 llvm 在 Windows 上打的是 MSVC ABI,
两者都需要,所以 mcpp 首跑时会探测机器上是否有可用的 MSVC(STL **与** SDK
两件齐——只有一半才是真正的坑),探不到就落到这里,并把选择持久化,之后的
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
[mcpp.toml](05-mcpp-toml.md) §2.7),`[build] linkage` 这个键并不存在,写了会被静默忽略:

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
archive 顶着 MSVC 的名字,MSVC 拿不去用。如果你有脚本按 `*.lib` 去捞
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

| Spec | 来源 | 你拿到的是哪个编译器 |
|---|---|---|
| `msvc@system`(或裸 `msvc`) | 这台机器自己的 Visual Studio | 这里恰好装了什么就是什么 |
| `msvc@<toolset>`(如 `msvc@14.44.35207`) | mcpp 安装的 xlings payload | 你点名的那一个,在每台机器上都一样 |

它们不是"二选一",而是回答了不同的问题。`msvc@system` 问的是*"用这位开发者
已经有的东西"*;`msvc@14.44.35207` 问的是*"用恰好这个编译器构建本工程"*。
pinned toolset 之间、以及与系统 Visual Studio 之间都可以共存。

> **`@system` 是 MSVC 独有的拼写。** 没有 `gcc@system`,也没有 `llvm@system`,
> 这是有意的而不是漏了:mcpp 建立在用户态 OS xlings 之上,整条设计就是**把 host
> 依赖降到最低** —— 工具链来自 manifest 点名的 payload,于是每台机器用同一个
> 编译器。Windows 是唯一一处"拒绝使用已装好的东西"代价大于收益的地方:
> Visual Studio 常常已经装了,又不总能重新分发。其它族写 `<family>@system` 会
> 直接报错,并同时给出你可能想要的两种写法。(不带族的
> `[toolchain] … = "system"` —— 即 PATH 上的编译器 —— 是另一套、也是有意保留的
> 逃生口,不受影响。)

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

若机器上没有 Visual Studio,mcpp 会说出来并同时给出两条路:一个它可以替你装的
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
产物走 —— 见 `docs/zh/05-mcpp-toml.md` 的 `cxx_runtime = "toolchain-coupled"`。

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
