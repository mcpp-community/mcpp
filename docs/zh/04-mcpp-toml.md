# 04 —— mcpp.toml 工程文件指南

**读者:**正在写或正在读一份 manifest 的作者。

**本章回答的那一个问题:**一份 `mcpp.toml` 可以说什么,逐字段地。

**不在这里:**四个主题的表虽然写在这个文件里,但本章不拥有它们 —— 依赖是
[05](05-dependencies.md),feature 是 [06](06-features-and-capabilities.md),
以目标为条件是 [22](22-target-side.md),工程的环境是
[23](23-the-project-environment.md)。每一处都在它的表本该出现的位置点名。

`mcpp.toml` 是 mcpp 构建工具的项目配置文件,类似 Cargo 的 `Cargo.toml` 或 Node 的 `package.json`。放在项目根目录下,`mcpp build` 会自动发现并读取它。

## 1. 最小化示例

mcpp 的设计原则是 **约定优于配置** —— 大多数字段都有合理默认值,最简单的 `mcpp.toml` 只需几行:

### 1.1 可执行程序（最简）

```toml
[package]
name    = "hello"
version = "0.1.0"
```

mcpp 自动推断:
- 源文件: `src/**/*.{cppm,cpp,cc,c,S,s,asm}`
- 入口: `src/main.cpp` → 生成 `hello` 二进制
- 标准: C++23
- 模块: 扫描 `export module ...` 声明自动建立依赖图

### 1.2 库项目（最简）

```toml
[package]
name    = "mylib"
version = "0.1.0"

[targets.mylib]
kind = "lib"
```

lib-root 约定:主模块接口默认在 `src/mylib.cppm`(包名的最后一段)。

## 2. 完整字段参考

### 2.1 `[package]` — 包元数据

```toml
[package]
name        = "myapp"              # 包名(必填)
version     = "0.1.0"              # 语义化版本(必填)
standard    = "c++23"              # C++ 标准(默认 c++23; 可设 c++20 / c++26)
description = "My awesome app"     # 简介(可选)
license     = "MIT"                # 许可证(可选)
authors     = ["Alice", "Bob"]     # 作者列表(可选)
repo        = "https://github.com/user/myapp"  # 仓库地址(可选)
```

`standard` 是 C++ 语言标准的一等配置。推荐值:

- `c++23`：默认值，适合当前模块化默认模板。
- `c++20`：mcpp 接受的最低档位——命名模块本身是 C++20 特性，再往下这套构建模型就不存在了。当外部约束(公司内规、只到 C++20 的第三方 API)必须压低档位时使用。**`import std;` 在这一档依然可用**：它虽然是 C++23 的*库*特性，但 GCC(≥ 15)、Clang + libc++(≥ 17)与 MSVC STL(VS 2022 17.8 起)都在 C++20 模式下提供 `std` 模块。代价是 C++23 库设施(`std::print`、`std::expected` 等)不可用——包括 `mcpp new` 生成的模板代码。
- `c++26`：需要 C++26 语言特性时使用。
- `c++2a` / `c++2c`：兼容别名，解析后分别归一为 `c++20` / `c++26`。
- `gnu++20` / `gnu++23` / `gnu++26`：需要 GNU dialect 时使用，会进入 fingerprint 和 std BMI cache key。
- `c++latest`：跟随当前 mcpp 支持的最新标准，适合本地试验，不推荐要求可复现的发布包使用。
- `c++fly`：`c++latest` **再加上该工具链能开启的全部实验性标准特性**(语言 + 标准库)。GCC ≥ 16 上会打开 C++26 反射(`-freflection`)与契约；Clang/libc++ 上追加 `-fexperimental-library`；不支持的门会跳过并打印 summary。刻意是工具链相关的——最前沿的试验场模式，永远不要用于发布包。

两条需要知道的性质：

- **标准是模块图全局的。** 根包的 `standard` 作用于本次构建的每一个 TU，依赖也不例外——
  依赖自己 manifest 里的 `standard` 在它作为依赖被构建时不生效。这不是简化：BMI 跨档位
  不兼容(GCC 直接报 `language dialect differs`)，同一张图物理上不可能存在两个档位。
- **档位之间从不共用缓存。** 标准同时进入 fingerprint、`import std` 的 BMI 身份和依赖构建
  缓存键，所以在 `c++20` 与 `c++23` 之间切换只会各自拿到独立的产物目录和独立的 std BMI，
  不会出现错误命中。

如果源码在某个档位上 `import std;` 而解析出的工具链在该档位不提供 `std` 模块，
mcpp 会在编译前失败，并同时报出工具链与工程档位。

值的两种拼法都接受:`standard = "c++26"` 与 `standard = 26`。

当**依赖声明的档位高于当前图**时,mcpp 会在编译前说出来,而不是让它在那个依赖的源码里
某处失败。见 [workspace §4.2](07-workspace.md)。

#### 方言标志与 `import std` BMI

有些标志会改变标准库头文件**声明出什么**,因此预编译的 `import std` BMI 也必须带着它们一起
构建。这就是 `[build] dialect_cxxflags` 的用途:它会被施加到 std BMI 预编译、模块扫描
**以及**图中每一个 TU(依赖也包括在内)。

```toml
[build]
dialect_cxxflags = ["-fno-exceptions"]
```

其中少数几个标志,mcpp 在 `cxxflags` 里发现时会自动提升进这条通道
(`-freflection`、`-fchar8_t`、`-D_GLIBCXX_USE_CXX11_ABI=…`)—— 混用这些标志的图本来就是
病态的,任何依赖都不可能对它们持有另一种自洽的意见。

`-fno-exceptions` 与 `-fno-rtti` **不会**被自动提升,因为依赖可以合法地不同意:它们移除的是
依赖可能正在使用的语言设施,而消费者无权替它做这个决定。留在 `cxxflags` 里,它们会到达每一个
TU 却到不了预编译,于是构建不可能成功 —— mcpp 在编译前就拒绝,并指出该用哪个键:

```
error: `-fno-exceptions` changes the language dialect, but the `import std` BMI is
       precompiled without it, so every importing translation unit will fail with
       "language dialect differs".
       Declare it as a dialect flag instead:

         [build]
         dialect_cxxflags = ["-fno-exceptions"]
```

这项检查读的是**生效后的**标志集合,所以同一个标志写在 `[profile.<name>] cxxflags` 或
`[target.…]` 块里同样会被抓到。而当图中根本没有 `import std` 时它不触发 —— 那里它就是一个
正常工作的按 TU 选项。

### 2.2 `[targets.<name>]` — 构建目标

```toml
# 可执行程序(默认,有 src/main.cpp 时自动推断)
[targets.myapp]
kind = "bin"
main = "src/main.cpp"       # 可选,默认 src/main.cpp

# 静态库
[targets.mylib]
kind = "lib"

# 共享库
[targets.mylib]
kind = "shared"
soname = "libmylib.so.1"  # 可选: Linux/ELF ABI 名称,运行时会生成同名 alias
```

`soname` 用于共享库的 ABI 名称,类似 Autotools/CMake 中的
`SOVERSION`/`SONAME`。在 Linux 上,mcpp 会向链接器传递
`-Wl,-soname,<name>`,并在输出目录生成 `<name> -> lib<target>.so` alias,
让下游程序可通过标准 ABI 名称 `DT_NEEDED` 或 `dlopen()` 加载该库。
该字段只对 `kind = "shared"` 有效,值必须是文件名 basename。

共享库目标在三种二进制格式上都可用。ELF 产出带 `soname` 的 `.so` 与 `$ORIGIN`
搜索路径;Mach-O 产出 install name 为 `@rpath/<file>` 的 `.dylib`,因此移动后
仍能被找到;PE 同时产出加载器打开的 `.dll` 和链接器消费的 import library,并在
MSVC ABI 上从对象生成导出表(该 ABI 没有 `__declspec(dllexport)` 或 `.def` 时
不导出任何符号)。参见 `tests/e2e/08`、`257`、`259`。

#### `exports` —— 产物发布的符号集合(mcpp 2026.9.6.5+)

```toml
[targets.mydriver]
kind    = "shared"
soname  = "libmydriver.so.1"
exports = "abi/mydriver.exports"     # 或内联:exports = ["vk_icd*"]
```

**不写这个键就发布全部,而那正是两个平台今天的默认**——ELF 给符号默认可见性,PE 会
自动生成列出全部符号的 `.def`。`exports` 把它收窄。

两类工程需要收窄。**有稳定 ABI 的运行时**只发布一份经过评审的集合,不在集合里的东西
才保持可改。**与同类并存的插件**不能撞名:Vulkan loader 按名字找
`vk_icdGetInstanceProcAddr`,一个把内部符号也导出的 ICD 会与 loader 以及同进程内另一个
ICD 相撞。

文件一行一条符号模式,`#` 起注释,`*` 是唯一的通配符。内联数组说的是同一件事,用于
只有两三个入口、单开一个文件反而是仪式的场合。

一句话,三种渲染:

| 平台 | 渲染为 |
|---|---|
| ELF | version script,`-Wl,--version-script=` |
| Mach-O | `-Wl,-exported_symbols_list`(前导下划线由引擎补) |
| PE | `.def`,取代自动生成的全导出版本 |

**它不改变编译期可见性,这是有意的。** 三种格式上收窄都是链接期属性,所以一个键只有
一个效果。`-fvisibility=hidden` 仍可经 `[build] cxxflags` 使用以取得代码生成上的收益,
而它是一个**单独**的决定,因为它同时改变本库各翻译单元之间如何看见彼此。

**符号版本化不是这个键。** `foo@@LIB_1.0` 与 `foo@LIB_0.9` 并存是 ELF 独有的能力,
无法中立表达;需要它的包自己写 version script 经 `[build] ldflags` 传入,或者算出来后
用 `mcpp:link-flag=` 发出(docs/07)。

`soname` 对 `kind = "lib"` 同样有意义 —— 见下文的 `dependency_linkage`,
库以何种形态出现是**消费者**的决定。

#### 按目标的键(per-target keys)

```toml
[targets.server]
kind     = "bin"
main     = "src/server.cpp"
defines  = ["BUILD_SERVER=1", "PORT=8080"]   # -D 宏,只作用于该目标的入口
cxxflags = ["-Wno-deprecated-declarations"]  # 该目标入口的额外 C++ 标志(不要放 -std=...)
cflags   = ["-DPURE_C"]                       # 该目标入口的额外 C 标志

[targets.gui]
kind = "bin"
main = "src/gui.cpp"
required_features = ["gui"]                   # 仅当 feature `gui` 激活时才构建
```

| 键 | 含义 |
|---|---|
| `defines` | 预处理宏(`name` 或 `name=value`),脱糖为 `-D<x>`,作用于该目标入口的 C 与 C++ 编译。 |
| `cxxflags` / `cflags` | 该目标的额外编译标志。**不要**放 `-std=...`——用 `[package].standard`。 |
| `required_features` | 仅当列出的 feature **全部**激活时才生成该目标,否则静默跳过。只是门禁——不激活 feature(用 `--features` / `[features].default`)。 |

> **作用域(重要):** 目标上的 `defines` / `cxxflags` / `cflags` **只作用于该目标独占的入口源**
> (它的 `main`)——**绝不**作用于共享的模块/实现对象(那些只编译一次、被每个目标链接,即 mcpp 的
> compile-once 模型)。当标志只需影响某个二进制(或测试)**自己的入口**时,这正是合适的工具 ——
> 例如某个测试的 `main` 里触发契约违规、需要按测试设置契约求值语义
> (`-fcontract-evaluation-semantic=observe`),或入口独享的 feature 宏、局部告警抑制。
> 若标志必须穿透**共享**代码,就不该放在这里 —— 改用 [workspace](07-workspace.md) member 或
> `[features]`;若是整次构建的模式,用 `[profile.*]`(`mcpp test --profile <name>` 会让包括被测
> 代码在内的整个测试镜像都在该 profile 下编译)。
>
> `[targets.<name>]` 下的不支持键会产生 warning(`--strict` 下为 error)。

**构建配置该放哪** —— 当多个二进制需要不同配置时:

| 目标 | 使用 |
|---|---|
| 某二进制**自己入口**上的不同宏/标志 | per-target `defines` / `cxxflags`(见上) |
| 两个产品差异在它们**共享**的代码里 | 拆成 [workspace](07-workspace.md) member,各自 `[build]` 标志,共享一个 `lib` |
| **选择**某共享库的变体(如某后端) | 在该库上用 `[features]`(§2.8)——additive,作用到库自己的编译 |
| **整次构建的模式**(sanitizer、契约语义、优化档) | `[profile.<name>]`(§2.9)+ `--profile`;`mcpp test --profile <name>` 同样支持 |

mcpp 刻意不在一次构建里把同一个共享源编译成两份:一个源对应一个对象(模块还对应一个 BMI),
所以"必须穿透共享代码"的差异应放在包/feature 边界,而非单个目标上。

### 2.3 `[build]` — 构建配置

> **`sources` 匹配到的每一项都必须产出一个会被链接的对象。** mcpp 放不下的文件 ——
> 扩展名既不在内建表也不在 `module_extensions` 里 —— 会被拒绝,并点名文件、
> 扩展名与该写的键。**不是忽略**:催生这条规则的失败不是「多编了一个文件」,
> 而是**编了却没人链** —— 扫描器读到 `export module` 就给那条边挂了 BMI,
> 而分类器说这个文件没有角色,作者看到的是一条模块修饰过的 `undefined reference`。
> 头文件应放进 `include_dirs`,Windows 资源脚本放进 `[resources]`。

> **`sources = []` 与不写 `sources` 不是一回事。** 不写这条键选择默认 glob;
> 显式的空列表意味着**什么都不编** —— 那正是一个纯头文件的分发包需要表达的。
> 在 mcpp 2026.8.18.1 之前两者逐字节等价,于是「什么都不编」无从表达,
> `src/` 下剩下的任何文件都会被扫进来。


> **`sources` 的条目可以带上它所面向的加速器**(2026.9.5.2+):
> `{ glob = "src/kernels/**/*.cu", accel = "cuda12.9+{sm_89}" }`。glob 与其它条目一样
> 进入列表;约束决定它是否适用于某一次构建。它必须至少匹配一个文件(空匹配会被拒绝:
> 那会让这个设备无东西可编,而只在链接时才说话)。`--no-accel` 下该 glob 被排除,
> 一个工程由此产出它的 CPU-only 变体。`--accel` 未覆盖该约束时构建被拒并给出两侧
> (`accel-mismatch`)。有效集合匹配到的设备类源文件 —— CUDA 与 HIP、GLSL 各 stage、
> HLSL、OpenCL C 与 Metal,完整清单见 [42 — 异构硬件构建](42-heterogeneous-builds.md) —— 引擎
> 从不编译;它们以 `MCPP_DEVICE_SOURCES` 到达构建程序,由工程引入的规则包把每一个
> 变成一条 `mcpp::action`。

```toml
[build]
sources      = ["src/**/*.cppm", "src/**/*.cpp"]  # 源文件 glob(默认: src/**/*.{cppm,cpp,cc,c,S,s,asm})
module_extensions = [".ixx"]      # 模块**接口**额外使用的扩展名(见下节)
build_program_timeout = 1800      # build.mcpp 的运行上限(秒);0 = 不限(见下节)
include_dirs = ["include", "third_party/include"]  # 头文件搜索路径
include_dirs_after = ["*"]         # 排在系统目录之后搜索的头文件目录(-idirafter)
private_include_dirs = ["vendor/src/include"]  # `include_dirs` 中不发布给消费者的那些
c_standard   = "c11"              # C 源文件的标准(默认 c11)
cflags       = ["-DFOO=1"]        # 额外 C 编译参数
cxxflags     = ["-DBAR=2"]        # 额外 C++ 编译参数(不要放 -std=...)
ldflags      = ["-lfoo"]          # 额外链接参数
defines      = ["BIZ=1", "QUX"]   # 作用于每个 TU 的预处理宏(脱糖为 -D;会进入模块扫描)
cxx_runtime  = "self-contained"   # C++ 运行时契约(见下节);static_stdlib 是旧拼写
macos_deployment_target = "14.0"   # macOS 产物的最低支持系统版本(仅 macOS 生效)
dependency_linkage = "static"     # 依赖以何种形态进入:static(默认)| shared(见下文)
cache        = "global"           # 依赖的全局构建缓存:global(默认)| local | off(见 §2.10)
jobs         = "auto"             # 并发编译数:正整数,或 "auto"(见下节)
bmi_schedule = "auto"             # 模块边调度:auto(= 关)| on | off(见下节)
```

#### `dependency_linkage` —— 静态还是动态由消费者决定

```toml
[build]
dependency_linkage = "shared"        # 全图默认;缺省即 "static"

[profile.dev]
dependency_linkage = "shared"        # 按 profile 覆盖

[dependencies]
"compat.zlib" = { version = "1.3.2", linkage = "shared" }   # 单个包
```

在 mcpp 2026.8.28.2 之前,一个依赖只有一种形态,而且由**包作者**定死:
`kind = "lib"` 把它的对象并进每个消费者的链接,`kind = "shared"` 产出真正的
共享库。这个决定放错了位置。一个库在运行期该不该是独立文件,是**被构建的那个
程序**的性质 —— 它怎么分发、多久重链一次、进程里是不是已经有人提供了这个库。

- **`static`**(默认)—— 依赖的对象并进使用它的映像。与 mcpp 一直以来的行为
  逐字节相同;不写这个键的工程构建结果不变。
- **`shared`** —— mcpp 把依赖构建成产物旁边的共享库并链接它,由 `$ORIGIN`
  (ELF)/ `@loader_path`(Mach-O)/ 可执行文件自身目录(PE)保证构建目录
  移动后仍能找到它。

**这不是 `[target.<triple>].linkage`**(§2.7.1)。那个键回答的是听起来相同、
实则关于 **C 库**的问题(musl 的 `-static`、MSVC 的 `/MT`)。两者并不独立,而且
方向很重要:整链静态的映像没有解释器,根本装不下任何共享对象。因此在 C 库静态
链接的目标上 —— 这是 **musl 的默认** —— `dependency_linkage = "shared"` 会被
拒绝,并说明原因。

**包可以声明它必须是某一种形态**,而且只在确有理由时:

| 包写了 | mcpp 读作 |
|---|---|
| `[targets.<n>] kind = "shared"` | *必须* shared —— 进程里会有别人 `dlopen` 它,因此只能有一份(X11、Vulkan loader) |
| `ldflags` 里含 `-L` | *必须* static —— 包携带了 mcpp 没有编译的预构建归档,放不进 mcpp 自己构建的共享对象 |
| 分发包(`mcpp pack`) | 它实际随包的那些腿,取自 `[[runtime.artifacts]] role` |
| 其他 | 两种形态都可以 |

`kind = "lib"` **不是**约束:它是默认值,大多数包写下它并没有做任何选择。
**没有陈述不等于一条陈述。**

依赖边上的 `linkage` 只在**根工程**的 `[dependencies]` 里生效。依赖图深处的包
无权决定最终程序的布局;真正必须只有一份共享副本的包,应当在自己的 target 上
声明。

#### library 目标上的 `soname`

`soname`(§2.2)在 `kind = "lib"` 上同样可以声明。它是一个库被**找到**时用的
名字,也是 mcpp 构建的那份与第三方携带的同一个库能解析到**同一个文件**的唯一
途径 —— 而如果声明它就意味着这个包不能再作为静态库被消费,包就无法陈述这件事。

在非 shared 目标上写 `soname` 的描述符,**无法被 2026.8.28.2 之前的 mcpp 读取**
—— 失败的是整份 manifest,不只是这个键。因此把它发布进索引要等下限抬上去。

#### 符号提供者检查

链接之后,mcpp 会问:映像里的每个符号是不是**恰好有一个**提供者。在 ELF 上
可执行文件排在最前,因此被静态并进程序的库,会在它与旁边加载的共享库共有的
每个符号上获胜 —— 共享的那份永远不会被调用,而那个库里的代码跑在一份它并非
针对其链接的构建上。链接器和加载器都不会为此报任何一句话。

这项检查是**测量**而不是声明:读产物的动态符号表,去掉 copy relocation,只报告
产物自身闭包里**也**有定义的那些。进程里只有一份副本的安排保持静默。判定记录在
`target/<triple>/<fp>/resolution.json` 的 `runtime.symbol_provision` 下,带计数
与分母,CI 不需要 `readelf` 就能读。

默认是警告,`--strict` 下升级为错误。三条出路**有次序**,而次序是要紧的:

1. **让其中一方不再提供这个库** —— 通常是那个携带了依赖图已经在构建的库的副本
   的包。永远正确。
2. **让两者解析到同一个文件**:在库的 target 上声明它真正的 `soname`。
3. **`dependency_linkage`** 改变 mcpp 构建的形态。它会消掉**这一条**报告,但单
   独用可能把一份变成**两份**:实测在一个暂存了 glib(其 `libgio` 需要
   `libz.so.1`)、同时静态构建 `compat.zlib` 的图上,切换形态让可执行文件的 88
   个导出符号归零,然后 `libzlib.so` 与 `libz.so.1` **两个都被加载**。只有在
   (2) 同时成立时它才真的把两个提供者合成一个。

`private_include_dirs` 指出 **`include_dirs` 中**在本包边界处停住的那些条目:
本包用它们编译,消费者永远收不到。

绝大多数包发布的就是它编译时用的那一套,所以长期以来只有 `include_dirs` 就够了。
两者不同的形状只有一种 —— 一个包**内嵌了带内部头覆盖层的库**。musl 通过
`src/include` 到达它自己的声明,而那些头定义了 `hidden`、`weak`、`weak_alias`,
这些名字只对 musl 自己的源码有意义。把那个目录发布出去,等于把这些宏交给每一个
消费者;而一个把 `hidden` 当普通标识符用的消费者会编不过,且看不出原因。

```toml
[build]
# 两类目录的**相对顺序**是承重的:本包自己构建时,内部覆盖层必须排在公共头之前。
# 这正是它被设计成 `include_dirs` 的**子集**而不是第二个列表的原因 ——
# 两个数组表达不了一个顺序。
include_dirs         = ["port/include", "musl/src/include", "musl/include"]
private_include_dirs = ["musl/src/include"]
```

条目支持与 `include_dirs` 相同的 `*` glob 约定,并在**展开之后**比对 ——
所以一个 glob 可以恰好指名它展开出的那些目录。若某条目不在本包的 `include_dirs`
里,它什么也没扣下,mcpp 会把这件事说出来而不是让它悄悄通过。

**旧引擎会忽略这个键,而不会因此失败。** 在 2026.8.26.2 上实测:出现在依赖的清单里
时被静默接受;出现在根清单里时给一条警告 —— `[build] has unsupported key
'private_include_dirs' (ignored)` —— 构建照常继续。所以一个包可以先用上这个键,
不必等消费者升级;还在旧引擎上的消费者只是像以前一样继续收到那个目录。**唯一不成立
的地方**是已发布的 `xim` 描述符的 `target_cfg` 块:那里不认识的子键是硬错误,会让
整份清单加载失败 —— 在索引下限指向认识它的引擎之前,不要把这个键写进那里。

`include_dirs_after`(#249)列出**排在工具链系统目录之后**搜索的头文件目录
(GCC/Clang 发射为 `-idirafter`;MSVC 方言退化为排在末尾的 `/I`,NASM 汇编
单元退化为普通 `-I`——两者都没有对应 flag,也都没有需要保护的系统头搜索链)。当目录是解压后的源码 tarball 根目录、且其中的文件名会与标准头冲突时,
用它代替 `include_dirs` —— 例如 ffmpeg 根目录的 `VERSION` 文件在大小写不敏感
的 macOS 文件系统上会把 libc++ 的 `<version>` 遮蔽(若该根目录挂在 `-I` 上)。
使用 `include_dirs_after` 时系统头永远优先,而包自己的真实头文件
(`<libavutil/frame.h>`)仍能找到。条目支持与 `include_dirs` 相同的 `*` glob
约定,并沿相同的依赖边传播给消费者 —— 消费者收到的仍是 after 目录,
永远不会被升级为 `-I`。

`macos_deployment_target` 设定产物 Mach-O 头里的最低系统版本
(`LC_BUILD_VERSION minos`),即二进制能运行的最老 macOS。优先级与各生态
惯例一致:环境变量 `MACOSX_DEPLOYMENT_TARGET`(单次调用的显式覆盖,
cargo/rustc、cc 等同样尊重该变量)> 本字段(项目默认,类似 SwiftPM 的
`platforms:`)> **内建默认 `14.0`**(rustc 风格——每个 target 都有基线,
14.0 即 LLVM 官方静态库自身的下限)。该值会进入 BMI 指纹——切换 target
会自动重建模块缓存。

### 构建并发(`jobs`)与模块调度(`bmi_schedule`)

```toml
[build]
jobs         = "auto"    # 或正整数;--jobs / MCPP_JOBS 覆盖它
bmi_schedule = "off"     # auto(默认,= 关)| on | off
```

`jobs` 是同时跑几个编译。`"auto"` **在构建这台机器上现算**,绝不冻进 manifest:
异构 CPU 上取物理核数(13900K 是 8 P-core + 16 E-core,它的 32 个线程不是 32 个
等价的工人),再按可用内存夹一次 —— 单个模块接口编译峰值 0.5–1.0 GB。
优先级:`--jobs` / `MCPP_JOBS` > 这个键 > 后端自己的默认值。写错的值会被
**明确报出来,绝不静默当成默认值** —— 一个悄悄退回默认的拼写错误,表现是
「构建莫名其妙比我要求的慢」。

`bmi_schedule` 决定**导入方什么时候被解锁**。

| 值 | |
|---|---|
| `"auto"` | **默认值,而它目前等于「关」** |
| `"on"` | 拆开模块边:BMI 一发布导入方就能开始,而不是等编译器退出 |
| `"off"` | 每个模块一条边 |

只认这三种拼写。`"ON"`、`"true"`、`"yes"` 会被**拒绝并给出诊断**,而不是悄悄
当成关 —— 而且它们不是无害的笔误:这个值会进构建指纹,所以一个被拒的拼写
以前会选到**另一个构建目录**(即一次全量重建),同时对调度没有任何影响。

**`auto` 为何等于关闭。** 模块接口编译中约 86% 是任何导入方都不会读取的代码生成,
因此提前发布 BMI 收益显著 —— 在 mcpp 自身上实测:`cold` 86.7s → 35.7s、
`edit-body` 80.9s → 29.8s。但调度错误的表现是静默失效:缺少一条依赖不会使构建
报错,只会使某个目标不再重建。因此在所有平台完成 CI 验证前,该键保持 opt-in。

**该键无效的场景。** mcpp 本来就跳过级联的地方(`touch-hub`、`edit-comment`)
没有可以移出关键路径的必需工作,该键不产生收益。见
[性能对比](../../README.zh-CN.md#性能对比)。

**实现方式**按编译器确定,无需用户选择:gcc 用 `rename()` 发布 BMI,所以代码
生成被分离出去、边在发布时就返回;clang 换成两条普通边 —— 它把 BMI 直接
`O_TRUNC` 写到最终路径,读的人可能看到写了一半的文件。MSVC 不动:`/ifcOnly`
的代价和 `.ifc` 是否原子发布都没测过,而这两件事猜错都是无声的。

### 模块接口扩展名(`module_extensions`)

mcpp 把 `.cppm` 视为模块接口单元。C++ 生态并没有收敛到一种拼法 —— Clang 还认
`.ccm` 和 `.cxxm`,MSVC 用 `.ixx` —— 所以接口用别的扩展名的工程自己声明:

```toml
[build]
module_extensions = [".ixx", ".ccm"]
```

这个列表是**追加**的:`.cppm` 永远是模块接口,不能删。要让某个文件不参与构建,
用 `sources` 的 `!` 前缀 —— 那才是 `sources` 的职责。

声明一个扩展名会同时做三件事,这正是「一个键而不是几个键」的理由:

1. `sources` 的约定默认值跟着变宽,文件才**能被找到**(`src/**/*.ixx` 自动进入默认 glob);
2. 这些单元用**模块**规则编译 —— 产出 BMI,其 `.o` 无条件进入链接;
3. 新鲜度快路径会扫描它们,所以给其中一个加 `import` 会让构建图作废,
   而不是静默复用一张过期的图。

**任何扩展名都接受**,唯独拒绝那些已经代表其他角色的
(`.cpp` `.cc` `.cxx` `.c` `.m` `.mm` `.h` `.hpp` `.hh` `.hxx` `.S` `.s` `.asm`)——
这是 manifest **错误**而不是警告,因为它会把(比如)C 文件送进 C++ 模块规则,
最终失败在一个既不提文件也不提这个键的地方。

扩展名**按字面匹配,不做大小写折叠** —— 在这个领域里 `.S` 和 `.s` 是两种不同的语言,
所以大小写从不被忽略。

mcpp 每次都会**显式告诉编译器**这个单元是模块接口(Clang 用 `-x c++-module`,
GCC 用 `-x c++`,MSVC 用 `/interface /TP`),所以即使编译器驱动从没听说过这个扩展名
也能工作。这也是为什么任何扩展名都被允许:mcpp 不需要编译器认识它。

> **发布须知**:旧版 mcpp 不认识这个键 —— 它会警告、忽略,然后把那些文件当作普通
> 翻译单元编译,得到一个**错误的构建**而不是一次干净的失败。发布一个用了
> `module_extensions` 的包,请在它的索引描述符里声明 mcpp 版本下限。

### 构建程序超时(`build_program_timeout`)

`build.mcpp` 默认有 **600 秒**,超时后 mcpp 杀掉它并让构建失败、点名是哪个包。
构建程序确实需要跑更久的工程(大规模代码生成)自己抬高上限:

```toml
[build]
build_program_timeout = 1800   # 秒;0 = 不限
```

这个值读的是**拥有该 `build.mcpp` 的那个包**的 manifest —— 依赖的生成器由依赖自己的
声明来限制,因为只有它的作者知道要跑多久。优先级与 `macos_deployment_target` 同构:

```
MCPP_BUILD_PROGRAM_TIMEOUT=<秒>   本次调用(最高)
  > [build] build_program_timeout  该包自己的 manifest
  > 600                            内置默认
```

**不写这个键**与**写 `0`** 不是一回事:不写表示「用默认上限」,`0` 表示「完全不设上限」。

这个值刻意**不进构建指纹** —— 它不改变图里的任何一条边,而把它折进指纹会让
「抬高超时」触发全量重建,这恰好与抬高超时的人想要的相反。

只有构建**程序**受限,**编译**不受限。原因见
[30-build-mcpp.md](30-build-mcpp.md)。
### C++ 运行时契约(`cxx_runtime`)

已移入 [20 —— 工具链管理](20-toolchains.md)。


### 宿主代码页之外的文件名

glob 是窄字符串,编译命令和 `build.ninja` 也是。在 Windows 上这些字符串由进程的
**ANSI 代码页**产生,因此一个名字在该代码页里无法拼写的文件,既匹配不了 glob,也
写不进编译命令或构建文件。

这类条目会被跳过,并按目录报告一次:

```text
warning: 'C:/.../pkg/test/www' contains names this system's active code page cannot represent
  impact: those files take no part in the build
  hint: Windows only: this is the process ANSI code page, which `chcp` does not change. ...
```

报告里给的是**最近一个代码页拼得出的祖先目录**,用通用(`/`)写法。拼不出的那个名字本身
永远不会被打印:渲染它会抛出这条消息正在报告的同一个异常。

`chcp` 改的是**控制台**代码页,对此无效。若这些名字只是测试数据或文档,跳过是无害
的——上游 tarball 里带一个日文夹具目录,在 en-US 宿主上照样构建。源文件则不然:需要
改名,或换一台代码页覆盖得了的机器。

Linux 与 macOS 不做这种转换,因此那里不会跳过任何名字。一个包在一边能构建、在另一
边报 `internal: unhandled exception` 并指向代码页,就是 mcpp#516。

### 2.3.1 `[build] accel` — 本次构建面向的加速器

```toml
[build]
accel = "cuda12.8+{sm_80,sm_90f} ptx>=90"
```

本次构建为哪些设备后端与架构编译。单次构建可用 `--accel` 覆盖 ——
这与 `--target` 对 `[toolchain]` 的关系相同;`--no-accel` 是显式请求「不要加速器」,
也就是在一个同时发布了设备构建的包中选中 CPU-only 变体的方式。

该取值会与构建所消费的任何预建产物的 `accel` 字段比较,而请求为空的构建被任何产物满足。
见 [42 — 异构硬件构建](42-heterogeneous-builds.md)。

### 2.4 `[lib]` — 库根模块约定

```toml
[lib]
path = "src/capi/lua.cppm"    # 覆盖默认的 lib-root 位置
```

默认约定:`src/<包名最后一段>.cppm`(如包名 `mcpplibs.cmdline` → `src/cmdline.cppm`）。
### 2.5 `[dependencies]`、`[dev-dependencies]`、`[build-dependencies]`

已移入 [05 —— 依赖与解析](05-dependencies.md)。

### 2.7 `[toolchain]` —— 工具链配置

```toml
[toolchain]
default = "gcc@16.1.0"

# 交叉编译目标覆盖
[target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
linkage   = "static"
```
### 2.7.1 `[target.*]` —— 平台条件依赖与 flag

已移入 [22 —— 目标侧](22-target-side.md)。


### 2.7.2 裸机(`os = none`)—— freestanding target

`riscv64-none-elf` 与 `riscv32-none-elf` 是底下没有操作系统的 target。它们不需要
逐宿主的交叉工具链:clang 与 lld 天生是交叉编译器,任何能装 llvm 载荷的宿主都能
产出它们。

本节是清单参考。示例部分 —— 生成工程、运行、在目标上测试、freestanding 标准库
子集,以及编写板级支持包 —— 在
[40 — 裸机与 freestanding 目标](40-baremetal.md)。

```bash
mcpp build --target riscv64-none-elf
mcpp run   --target riscv64-none-elf     # 经 [target.<triple>].runner
```

**从板级支持包起步**

下面这些几乎都不需要手写。板级支持包(BSP)自带 C 库、启动代码、内存布局和模拟器,
所以跑起一个镜像的最短路径是:

```bash
mcpp new blinky --template riscv-virt-rt
cd blinky && mcpp run
```

生成的 manifest 里没有链接脚本、没有加载地址、没有 libc、没有模拟器 —— 连
`[target.*]` 段都没有。本节余下的内容讲的是**这样一个包提供了什么**,也就是要给
一块还没有 BSP 的板子写一个时该照着做什么。

**freestanding target 上有什么不同**

| | |
|---|---|
| 链接线 | `-nostdlib -nostartfiles -static`,且不带任何 hosted 的东西 —— 没有 crt 文件、没有动态链接器、没有 C++ 运行时。链接器用**绝对路径**寻址(`-fuse-ld=<载荷>/bin/ld.lld`),因为 `-fuse-ld=lld` 走 `PATH` 解析,在任何 binutils 排前面的机器上都会找到 GNU ld。 |
| ISA flag | `-march` / `-mabi` / `-mcmodel` 来自 target 表,所以只写 `--target <triple>` 就足以产出正确的目标文件。 |
| C 库 | **属于 target**,由 mcpp 从目标自己那一行解析,和解析编译器同理 —— 裸机工程不声明 libc,正如宿主工程不声明 glibc。它的头进入每一个翻译单元,它的目录进入链接搜索路径,所以板级包用**裸名**选库(`-lc`、`-lcrt0-semihost`)。**选哪个**启动对象、**用哪份**链接脚本仍然是板级决定。 |
| 异常与 RTTI | **关闭**,作用于每一个翻译单元,依赖的也不例外。没有 unwinder、没有 `libc++abi`,谁都抛不了;否则光是 `std::optional::value()` 就会拉进 `__cxa_throw` 等四个未定义符号。它属于 **target** 而不是工程的 `cxxflags`,因为 **BMI 会记录这个配置** —— 带异常编出来的依赖,不带异常的单元 import 不进来。 |
| `import std` | **不可用。** `std` 是覆盖整个库的一个模块 —— 线程、文件系统、iostreams 全在内 —— 没有 OS 就没有它的子集可编。取代它的是两个普通依赖:**板级包**包住目标的 C 库,**`std-freestanding`** 提供标准库里不需要 OS 的那部分(实测 libc++ 110 个头里的 103 个)。 |
| 入口点 | **只要有人提供 `crt0`,`int main()` 就能用** —— 板级支持包通常就提供它,于是固件的入口就是普通的 `main`,它的返回值经 semihosting 传回宿主。**只有零 libc 的板子**才需要显式声明 target 并把 `main` 指向携带 `_start` 的那个文件。 |

**一个最小固件**

```toml
[package]
name    = "fw"
version = "0.1.0"

[build]
ldflags = ["-T", "/abs/path/to/link.ld"]

[targets.firmware]
kind = "bin"
main = "src/start.S"          # 入口在汇编里,不在 main()

[target.riscv64-none-elf]
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
          "-no-reboot", "-bios", "default", "-kernel"]
```

**`runner` —— `mcpp run` 如何执行本机跑不了的东西**

裸机镜像的 ISA 不对、没有 loader、且期望独占整个地址空间;直接 exec 它得到的是
"Exec format error"。`runner` 就是挡在它前面的 argv 模板。产物路径会被**追加**,
或者在模板含 `{}` 时替换进去。

mcpp **刻意不提供默认 runner**。用哪个模拟器、哪个机器型号、哪种固件模式都是板级
事实 —— 同一 ISA 的两块板需要不同 argv(OpenSBI 启动用 `-bios default`,picolibc
镜像用 `-bios none -semihosting`)—— 引擎一旦猜一个,另一块板就得跟它打架。板级
支持包通常会提供它。

### 2.7.3 hosted 目标上的 `runner`(2026.9.2.1+)

`[target.<triple>].runner` 对每一个精确三元组生效,不限于裸机。一个 hosted 交叉产物
—— 在 x86_64 机器上构建的 `aarch64-linux-musl` —— 有的宿主能直接执行(binfmt_misc
注册了 qemu-user),有的宿主以 `Exec format error` 拒绝;属于哪一种是机器的性质,不是
三元组的性质。mcpp 不预测它:要么通过工程声明的 runner 执行产物,要么尝试直接执行并
报告内核的回答。

```toml
[target.aarch64-linux-musl]
runner = ["qemu-aarch64-static"]
```

规则对 `mcpp run` 与 `mcpp test` 相同:

- **声明了 runner 就使用它。** 其第一个元素由 mcpp 定位:先在 `[xlings.workspace]`(§2.13)
  声明的每个载荷的 `bin/` 目录里找,再找 `PATH`。`PATH` 上的裸名会命中 xvm shim,而
  shim 按当前 SubOS 而非按包作答;先查载荷,runner 才能直接写工程声明过的程序名。
- **声明的 runner 找不到或启动不了是错误**,错误里带程序名、搜索过的目录和 errno。
  不回落到直接执行:让产物在另一个解释器下带着另一组参数运行,正是这个键要防止的
  失败。
- **没有 runner 且内核拒绝产物:** `mcpp run` 报告拒绝原因与应当写的键,退出码 2。
  `mcpp test` 把每个测试报告为未运行,原因只打印一次,退出码 2(§2.7.3.1)。
- **`--no-runner`** 直接执行产物并忽略声明的 runner。它陈述的是关于本机的事实 ——
  这个三元组在本机是原生的 —— 清单没有承载它的轴;为 x86_64 开发者写的 runner 在
  aarch64 机器上仍可用。

通过 `[xlings.workspace]` 装模拟器是 CI 任务或单一宿主类别工程的形态。索引里的
`qemu-user-aarch64` 只为 x86_64 Linux 构建,而这张表在每台构建本工程的宿主上都会
provisioning,所以条目按平台写(§2.13):

```toml
[xlings.workspace]
"xim:qemu-user-aarch64" = { linux = "" }   # Linux 上存在即可,版本不限

[target.aarch64-linux-musl]
runner = ["qemu-aarch64-static"]
```

宿主装不了的包是硬构建错误,所以不带平台形式的条目会让工程在 macOS 与 Windows 上
无法构建。同样没有这个包的 Linux/aarch64 宿主传 `--no-runner`。

#### 2.7.3.1 `mcpp test` 与未运行的测试

产物在本机无法执行的测试既没有通过也没有失败。`mcpp test` 把它报告为**未运行**,
在确立原因时打印一次,在汇总里重复原因的第一行,退出码 2:

```
warning: this host cannot execute aarch64-linux-musl artifacts: Exec format error (error 8); declare [target.aarch64-linux-musl].runner, or pass --no-runner on a host that can
smoke ... not run
error: test result: NOT RUN. 0 passed; 0 failed; 1 not run (this host cannot execute aarch64-linux-musl artifacts: Exec format error (error 8); ...); finished in 0.41s (build 0.39s + run 0.00s)
```

退出码 1 含义不变 —— 有测试运行并失败;0 表示每个测试都运行并通过。
`--message-format json` 在每条记录上带 `"status":"not_run"` 与 `reason`,在汇总记录上
带 `not_run` / `not_run_reason`(见 [50 —— 机器可读输出](50-machine-output.md))。
### 2.8 `[features]` —— Feature

已移入 [06 —— Feature 与能力](06-features-and-capabilities.md),
连同 `provides` / `requires` 与 `[feature-deps.<name>]`。


### 2.8.3 `[scan_overrides."<glob>"]` —— 作者断言的扫描结果

默认的模块扫描器是文本级的一遍扫描,它(刻意地)拒绝条件预处理块内部的 `import`
语句。有些合法的模块单元带着这种写法 —— 例如 fmt 官方的 `src/fmt.cc` 把
`import std;` 收在 `#ifdef FMT_IMPORT_STD` 之后。当该文件的 import 集合已知且稳定时,
用声明取代扫描:

```toml
[modules]
sources = ["src/**/*.cppm", "vendor/fmt.cc"]

[scan_overrides."vendor/fmt.cc"]
provides = ["fmt"]      # 每个单元至多提供一个模块
imports  = ["std"]
```

被 glob 命中的文件跳过文本扫描,声明的单元直接进入模块图。该声明**每次构建都被审计**:
编译器自己对该文件的 P1689 扫描结果(`.ddi` dyndep 输入)会与之比对,任何分歧都会让
那条编译边失败并打印双方 —— 陈旧的声明无法静默污染模块图。未命中任何源文件的
override glob 是错误。

同一个键在 xpkg 描述符(索引包)中同样存在:

```lua
mcpp = {
    sources  = { "*/src/fmt.cc" },
    cxxflags = { "-DFMT_IMPORT_STD" },
    scan_overrides = {
        ["*/src/fmt.cc"] = { provides = { "fmt" }, imports = { "std" } },
    },
}
```

要把 plan 与 ddi 的比对审计扩展到**每一个**模块单元(而不只是 override),
在生成构建时设置 `MCPP_VERIFY_MODGRAPH=1`。

### 2.9 `[profile.<name>]` — 构建档案

```toml
[profile.dist]
opt      = 3              # -O 级别(数字或 "s"/"z" 字符串)
debug    = false          # -g
lto      = true           # -flto(注意:部分打包 gcc 未启用 LTO 插件)
strip    = true           # 链接期 -s
# passthrough 逃生口(固定键、开放值):
cflags   = ["-fno-plt"]
cxxflags = ["-fno-plt"]
ldflags  = []
```

- 选择与默认:裸 `mcpp build` 走 **`dev`** 档(`-O0 -g`)——主流惯例(参照
  Cargo/Meson/CMake/Zig/Bazel)。**release 为 opt-in:** `mcpp build --release`(短写)或
  `--profile release`;`--dev` 是 dev 的显式短写。`mcpp test --profile <name>` 同理
  (被测代码与测试二进制都在该 profile 下编译)。
- **项目级默认** —— `[build].default-profile = "<name>"`(别名 `profile`)设置该项目在不带
  flag 时的默认。典型用途是"以发布优化为常态"的工具/库:`[build] default-profile = "release"`。
  优先级:`--profile`/`--release`/`--dev` flag **>** `[build].default-profile` **>** 全局 `dev`。
  (默认 dev 的项目在产出可分发物时应显式 `--release`。)
- 内置档案:`release`(-O2)/ `dev`、`debug`(-O0 -g)/ `dist`(-O3 + strip;
  **不默认开 lto**)。`[profile.<内置名>]` 可整体覆盖内置定义。
- **每个 profile 各占一个构建目录。** 解析后的 profile 开关参与指纹,所以
  `target/<triple>/` 下每个 profile 一个哈希目录,来回切换是增量而不是全量重编;
  代价是磁盘占用随实际使用的 profile 数量增长。

### 2.10 `[build] cache` — 依赖的全局构建缓存

从索引获取的依赖,其编译产物按包缓存在 `$MCPP_HOME/build-cache/v1/` 下,跨工程共享。
依赖的产物与"谁在消费它"无关,所以工具链、profile、依赖版本相同的两个工程复用同一条目。

```toml
[build]
cache = "global"   # "global"(默认)| "local" | "off"
```

| 模式 | 读缓存 | 写缓存 | 先清构建目录 |
|---|---|---|---|
| `global`(默认) | 是 | 是 | 否 |
| `local` | 否 | 否 | 否 |
| `off` | 否 | 否 | 是 |

`local` 把所有依赖都编在本工程 `target/` 内 —— 排障时一次性排除"是不是缓存的问题",
也给 CI 一个无共享的可复现基线。`off` 额外清掉本次的 `target/<triple>/<fp>/` 做冷构建;
`--no-cache` 是它的兼容别名。

优先级:`--cache <mode>` **>** `MCPP_BUILD_CACHE` **>** `[build] cache` **>** `global`。
无法识别的值会被报出来(`--strict` 下为错误),而不是静默回落到 `global`。

**不进缓存的**:`path` 与 `git` 依赖(任意深度)以及 workspace 成员。它们的源码可以在
`name@version` 不变的情况下改变,任何基于该身份的键都看不见这种变化。

查看与回收:

```
mcpp cache dir                      # 缓存在哪
mcpp cache list [--json]            # 条目、体积、最后使用时间
mcpp cache info <pkg>@<ver>         # 单条目详情,含它是用什么键输入编出来的
mcpp cache verify                   # 逐条目校验清单与磁盘
mcpp cache gc --max-size 5GiB       # 按 LRU 收到容量预算内
mcpp cache gc --older-than 30d      # 或按"多久没用过"回收
mcpp cache clean [--deps|--std|--all|--legacy]
```

条目的磁盘布局是带版本的。改动布局的 mcpp 版本会**一次性作废全部旧条目**,
所以升级后的第一次构建会重编依赖并重新填充 —— 不需要手工清理。
2026.8.3.4 就是这样一次:条目里对象的地址现在相对**包**自身,
而不再相对"最先填充这个条目的那个工程"的构建目录。
`mcpp cache verify` 另外会报告任何逃出条目的记录地址,
使这条不变量可以离线审计。

### 2.11 `[runtime]` — provider-neutral 运行时契约

```toml
[runtime]
requirements = [
  { kind = "capability", value = "display.present", phase = "run", required = true },
  { kind = "soname", value = "libwidget.so.1", phase = "link", required = false },
]
provides = ["display.present"]
artifacts = [
  { role = "library", path = "runtime/libwidget.so.1", provenance = "payload", abi = "elf-x86_64", digest = "sha256:...", host_fingerprint = "host-1" },
]

# 平台无关 LinkIntent;路径相对本包根目录。
libraries                = ["widget"]
link_library_dirs        = ["lib"]
transitive_needed_dirs   = ["runtime/closure"]
runtime_search_dirs      = ["runtime"]
frameworks               = ["WindowKit"]
deploy_files             = ["bin/widget.dll"]

# 多 provider 时使用精确 canonical identity。
[runtime."display.present"]
provider = "acme.widget-runtime@2.0.0"
```

`requirements` 记录非空 `kind`/`value`、`link` 或 `run` 阶段,以及是否强制
(`required` 默认 `true`)。`artifacts` 必须含 `role`、`path`、`provenance`;
可选 requirement 仍保留为 provenance,但不会进入硬 ABI/doctor 输入。
`libraries` 中显式的相对文件路径按声明包根目录解析;裸逻辑名仍按目标平台拼成库名。
`abi`、`digest`、`host_fingerprint` 是可选证据。requester/provider 身份不由描述符
填写:resolver 会用含 namespace、version、source/index provenance 的精确 PackageId
给 requirement 和 artifact 盖章。因此描述符不能冒充别的包,
`alpha.backend` 也不会与 `beta.backend` 混同。

只有 `provides` 会创建描述符侧 provider fact;需要某能力绝不会让 requester 自动
成为 provider。显式 `[runtime.<capability>] provider=` 接受 canonical
`namespace.name@version`(或唯一无歧义的兼容拼写);不存在或同短名歧义都会 hard error。
xlings SubOS 已选择的 provider/artifact fact 排在描述符 fallback 前。图形栈、driver、
ICD、WSL 与 host provenance 选择由 xlings/xim 负责;mcpp 只记录、消费通用结果,
不探测 GPU 硬件。

LinkIntent 把不同发现阶段分开:

| 字段 | ELF | Mach-O | PE/Windows |
|---|---|---|---|
| `link_library_dirs` | `-L` | `-L` | `-L` 或 `/LIBPATH:` |
| `transitive_needed_dirs` | `-Wl,-rpath-link` | 无 flag | 无 flag |
| `runtime_search_dirs` | 只进 RUNPATH/rpath,绝不进 `-L` | 只进 rpath | 无 flag |
| `frameworks` | 无 flag | `-framework` | 无 flag |
| `deploy_files` | copy edge | copy edge | 复制到产物旁,绝不成为 linker flag |

一个兼容发布周期内仍读取旧字段:`library_dirs` 只映射到运行期搜索;
`dlopen_libs` 映射为必需的 run-phase soname requirement;`capabilities` 映射为必需的
run-phase capability requirement。这些旧字段都不会创建 provider。

`target/<triple>/<fp>/resolution.json` schema 2 持久化 RuntimeBinding、canonical
requirements/providers/artifacts、LinkIntent、平台搜索机制与链接后 verdict。
`mcpp why runtime` 只是最新存储文件的纯解释器:不重新解析 manifest,也不启动图形/
硬件 probe。需要重新诊断所选 host provider 时使用 `xlings doctor`。

每个 artifact 还带一个仅由路径算出的 `identity` 判定:

| `identity` | 含义 |
|---|---|
| `ok` | 声明的路径(穿过符号链接后)落在声明的那个版本里 |
| `mismatch` | 它解析到了别处 —— **该 binding 已陈旧**,后来的某次安装把它重新指向了别的地方 |
| `missing` | 声明了,但那个路径上什么都没有 |
| `unverified` | 声明时没有可供比对的版本 |

这就是 mcpp 早已施加于私有 libc 的那条规则的推广(`glibc@2.44` 解析到那一份载荷;
陈旧或缺失是错误,而绝不是「已安装版本里哪个看起来能用就用哪个」)。它不需要知道
该 artifact 做什么。`unverified` **有意**不等于 `ok`:一个解析到了却没有 artifact
在其背后的 provider 并未被核验过,因此 `mcpp why runtime` 打印
`(not declared by the environment — nothing to verify)` 而不是 `(none)`。

能力名使用分层小写 `domain.sub.role`(如 `display.present`)和前缀类
`abi:<name>`(如 `abi:glibc`,参与工具链 ABI 强制)。

### 2.12 `[package] platforms` — 平台声明

```toml
[package]
platforms = ["linux", "macos", "windows"]
```

声明包支持的平台(CI 矩阵提示,经 `mcpp why` 展示)。词表由 mcpp 固定
(它拥有 target/triple 体系):`linux | macos | windows`;未知值 warning,
`--strict` 下报错。

对库目标执行 `mcpp pack` 时,会拿这条声明与**实际产出的腿**核对 —— 那是第一个
有证据可核的时刻:

| 情况 | 结果 |
|---|---|
| 某条腿的平台不在此列 | warning —— manifest 否认了一个包明明能服务的平台 |
| 声明了某平台却没有对应的腿,**且本宿主本来就能构建它** | warning —— 该平台的消费者会解析到这个包却找不到产物 |
| 声明了某平台却没有对应的腿,而本宿主根本构建不了它 | **不说话** |

第三行才是这个检查可用的原因。正常的发布流程是 CI 上每平台各跑一次
`mcpp pack`,于是 Linux runner 永远不会产出 macOS 腿 —— 为此告警会在每个跨平台
包的每一次运行中触发,而**永远触发的告警会把真正该看的那条盖掉**。「本宿主能不能
构建」与 `--target` 回答的是同一个问题(docs/08 §7.4)。

两者都只是 warning,绝不报错:覆盖度属于发布纪律,而能作判断的人看的是发布,
不是这一次构建。

### 2.12b `[package] accelerators` — 加速器声明

```toml
[package]
accelerators = ["cuda", "rocm"]
```

声明该包支持的加速器后端。与 `platforms` 同形:一个意图声明与 CI 矩阵提示,
由 `mcpp why` 展示,**不是门**。

与产物的 `accel` 字段刻意不同。声明由人手写、可以是期望值;`accel` 是从产生该二进制的
那次构建测量出来的,并且是消费者被拒绝时所依据的东西。见
[42 — 异构硬件构建](42-heterogeneous-builds.md)。
### 2.13 `[xlings]` —— 工程的环境

已移入 [23 —— 项目环境](23-the-project-environment.md)。

### 2.14 依赖产出的 host 工具

已移入 [30 —— build.mcpp](30-build-mcpp.md)。


### 2.15 `[resources]` —— 编译进产物的元数据与资产(2026.8.7.1+)

exe 图标,以及 Windows 在文件「属性」里显示的版本信息,就是 `mcpp.toml` 里的一个路径:

```toml
[resources]
icon = "assets/app.ico"
```

常见场景到此为止。`FILEVERSION`、`ProductName`、`FileDescription`、`CompanyName`、
`LegalCopyright` 全部从 `[package]` 取默认值,资源脚本由 mcpp 生成。

| 键 | 类型 | 含义 |
|---|---|---|
| `icon` | 路径 | 作为应用图标嵌入(资源序号 1) |
| `files` | 路径列表 | 工程自带的 `.rc` 脚本,mcpp 编译并**跟踪**为构建输入 |
| `extra-inputs` | 路径列表 | `.rc` 扫描器看不见的输入(见下) |
| `version-info` | 布尔 | `false` 表示不要生成版本资源 |
| `[resources.version-info]` | 表 | `company`、`product`、`description`、`copyright`、`original-filename`、`internal-name` |

**只有 PE 目标会*编译*这一节。** 在 Linux/macOS 上它**不适用**:不产资源单元、
不出诊断、构建逐字节不变。**无需**(也不能)加 `cfg(windows)` 谓词 ——
无条件写一次即可。

**声明了却不存在的文件会让构建失败 —— 在每个目标上都是。** 资源和源码一样是
构建输入;mcpp 不会悄悄产出一个缺了它的二进制。校验刻意**不**按 PE 设门:
路径是否存在是关于工作树的事实,不是关于目标的事实,所以 `icon = "assets/app.ico"`
里的拼写错误由 Linux/macOS 构建(以及对应的 CI job)当场抓住,而不是等
Windows 那条。不想要图标,把那一行删掉。

**版本字段。** `FILEVERSION` 取 `[package].version` 的四段数值,每段必须放得进
16 位;字符串字段保留版本原文,所以数值字段装不下的形态(`1.0.0-rc1`)在属性
对话框里照样看得到。

#### 自写 `.rc`

```toml
[resources]
files = ["res/app.rc"]
```

写了 `files`,mcpp 就不再生成版本资源 —— 资源 ID 空间由工程自行支配。两者都需要时同时写
`version-info = true`(注意冲突:序号 1 的 `RT_VERSION` 只能有一个)。

想从生成的脚本起步而不是从空文件起步:把它从构建目录里拷出来
(`target/<triple>/<fp>/res/<target>.mcpp.rc`)填进 `files`。结果**字节相同**,
所以从「生成」走到「手写」不会改变产物。

> **`VS_VERSION_INFO` 需要 `<windows.h>`。** 手写脚本里如果写
> `VS_VERSION_INFO VERSIONINFO` 而没有 `#include <windows.h>`,版本资源会被存成
> **字符串名**而不是序号 1。所有工具依然报告 `Type: VERSIONINFO`,但
> `GetFileVersionInfo` 查的是序号,于是 PowerShell 的 `FileVersionInfo` 里每个字段
> 都是空的。要么 include `<windows.h>`,要么直接写 `1 VERSIONINFO`。mcpp 见到这个
> 形状会警告;它自己生成的脚本用的是字面 `1`。

#### 被跟踪的输入

mcpp 会读 `.rc`,把引号形式的 `#include` 和资源语句(`ICON`、`RCDATA`、
`MANIFEST` …)点名的文件都变成构建输入,所以改图标会重链。尖括号形式
(`<windows.h>`)属于工具链,由工具链 fingerprint 覆盖。

通过宏间接引用的文件名(`1 ICON APP_ICON`)扫描看不见。mcpp 会**指名**它没能解析
的东西,并要求显式声明:

```toml
extra-inputs = ["assets/app.ico"]
```

#### 其余一切:`role = "object"`

不是资源脚本的输入 —— `objcopy` 嵌入的 blob、生成的 `.def`、预编译对象 ——
可以由构建程序声明一个产出接到链接的图节点:

```cpp
mcpp::action o;
o.id = "blob"; o.role = "object";
o.arg("./mkblob.sh").arg("blob.bin").arg("${mcpp.out_dir}/blob.o")
 .input("blob.bin")
 .output("${mcpp.out_dir}/blob.o")
 .target("myapp")        // 省略:接到每个镜像,含测试二进制
 .submit();
```

见 [30 — build.mcpp](30-build-mcpp.md)。把这类文件写进 `[build].ldflags` 也「能用」,
但 ldflags 是链接命令里的一串字符:没有任何东西跟踪它,改了它得到的是
`ninja: no work to do`。
### 2.16 `[hooks]` —— 项目构建生命周期命令

已移入 [09 —— 按场景选命令](09-commands-by-scenario.md)。


## 3. 实战示例

其中四个是**可运行的工程**而不是片段,而工程是更好的答案:它能构建,而且由 CI 检查。

| 形态 | 运行 |
|---|---|
| 一个 hello world | [`examples/01-hello`](../../examples/01-hello/) |
| 带测试的模块化库 | [`examples/11-features`](../../examples/11-features/) |
| 带依赖的应用 | [`examples/02-with-deps`](../../examples/02-with-deps/) |
| 交叉编译的静态发布 | [`examples/03-pack-static`](../../examples/03-pack-static/) |

还有两种形态暂时没有对应示例,以 manifest 的形式留在这里。

### 3.4 纯 C 库

```toml
[package]
name    = "myc"
version = "0.1.0"

[build]
c_standard   = "c99"
include_dirs = ["include"]
sources      = ["src/**/*.c"]

[targets.myc]
kind = "lib"
```

### 3.5 混合 C / C++23 模块项目

```toml
[package]
name    = "hybrid"
version = "0.1.0"

[build]
include_dirs = ["include"]
c_standard   = "c11"

[dependencies]
lua = "5.4.7"     # 纯 C 库,mcpp 自动用 C 编译器编译 .c 文件

[targets.hybrid]
kind = "bin"
```


## 4. 约定与默认值速查

| 项目 | 默认值 | 说明 |
|---|---|---|
| 源文件 | `src/**/*.{cppm,cpp,cc,c,S,s,asm}` | 自动递归扫描 |
| 入口 | `src/main.cpp` | 有这个文件就推断为 `bin` 目标 |
| 库根 | `src/<pkg-tail>.cppm` | 可用 `[lib].path` 覆盖 |
| C++ 标准 | `c++23` | 用 `[package].standard` 配置; 支持 `c++20` / `c++26` / `c++2a` / `c++2c` / `gnu++NN` / `c++latest` / `c++fly`(实验试验场) |
| C 标准 | `c11` | `.c` 文件自动走 C 编译器 |
| 静态 stdlib | `true` | 便携二进制 |
| 头文件 | `include/`(如果存在） | 自动加到 `-I` |
| 测试 | `tests/**/*.cpp` | `mcpp test` 自动发现 |
| 依赖命名空间 | `mcpplibs`(默认) | 裸 selector 只表示该精确 ns |

### 4.1 旧 `[language]` 兼容层

旧配置仍可读取:

```toml
[language]
standard = "c++26"
```

新项目请使用 `[package].standard`。如果两个位置都出现，`[package].standard` 是权威配置。
