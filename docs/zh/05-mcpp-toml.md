# mcpp.toml 工程文件指南

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
某处失败。见 [workspace §4.2](06-workspace.md)。

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
> 若标志必须穿透**共享**代码,就不该放在这里 —— 改用 [workspace](06-workspace.md) member 或
> `[features]`;若是整次构建的模式,用 `[profile.*]`(`mcpp test --profile <name>` 会让包括被测
> 代码在内的整个测试镜像都在该 profile 下编译)。
>
> `[targets.<name>]` 下的不支持键会产生 warning(`--strict` 下为 error)。

**构建配置该放哪** —— 当多个二进制需要不同配置时:

| 目标 | 使用 |
|---|---|
| 某二进制**自己入口**上的不同宏/标志 | per-target `defines` / `cxxflags`(见上) |
| 两个产品差异在它们**共享**的代码里 | 拆成 [workspace](06-workspace.md) member,各自 `[build]` 标志,共享一个 `lib` |
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

⚠️ **这不是 `[target.<triple>].linkage`**(§2.7.1)。那个键回答的是听起来相同、
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

⚠️ 在非 shared 目标上写 `soname` 的描述符,**无法被 2026.8.28.2 之前的 mcpp 读取**
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
# ⚠️ 两类目录的**相对顺序**是承重的:本包自己构建时,内部覆盖层必须排在公共头之前。
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
[07-build-mcpp.md](07-build-mcpp.md)。

### C++ 运行时契约(`cxx_runtime`)

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

#### 在 MSVC 运行时上

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

> ⚠️ **但它不会让被宏保护的 `import` 变得可用。** mcpp 在编译器看到文件之前先跑
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

### 2.4 `[lib]` — 库根模块约定

```toml
[lib]
path = "src/capi/lua.cppm"    # 覆盖默认的 lib-root 位置
```

默认约定:`src/<包名最后一段>.cppm`(如包名 `mcpplibs.cmdline` → `src/cmdline.cppm`）。

### 2.5 `[dependencies]` — 运行时依赖

```toml
# 默认包空间(mcpplibs)下的包
[dependencies]
cmdline   = "0.0.2"              # 精确版本
templates = "0.0.1"

# dotted selector 是单一精确身份:最后一段是包名,之前所有段都是 namespace。
compat.gtest = "1.15.2"
imgui.core = "0.0.1"
imgui.backend.glfw_opengl3 = "0.0.1"
mcpplibs.capi.lua = "0.0.3"
```

```toml
# 命名空间子表写法
[dependencies.mcpplibs]
cmdline   = "0.0.2"
tinyhttps = "0.2.2"
llmapi    = "0.2.5"

[dependencies.compat]
glfw = "3.4"                    # 显式 namespace,无回退搜索
```

```toml
# 路径依赖(本地开发)
[dependencies]
mylib = { path = "../mylib" }
```

```toml
# Git 依赖 —— tag / branch / rev 三选一
[dependencies]
mylib = { git = "https://github.com/user/mylib.git", tag = "v1.0.0" }
applib = { git = "https://github.com/user/applib.git", branch = "develop" }
```

```toml
# 长式 dep spec:features 与 backend 旋钮
[dependencies]
imgui = { version = "0.0.3", features = ["docking"] }   # 请求该依赖的 feature
widget = { version = "1.0", backend = "glfw_opengl3" }  # 糖:= features=["backend-glfw_opengl3"]
```

`backend = "<impl>"` 是**通用约定糖**:1:1 脱糖为请求该依赖的 `backend-<impl>`
feature(库若支持该旋钮,应在自己的 `[features]` 中声明 `backend-*` 系列)。
若目标包声明了 `[features]` 但不含所请求的 feature(含 backend 脱糖结果),
默认给出 warning,`mcpp build --strict` 下报错。

**Git 依赖与 `mcpp.lock`**:`tag` 和 `rev` 本身就指向历史中的固定点,而 `branch`
是会动的。首次构建把分支解析成一个 commit 并写进 `mcpp.lock`,此后每次构建都重建
**那个** commit —— lock 是权威而不是缓存提示,所以删掉 `~/.mcpp/git` 或换一台机器
都不会静默切换到更新的分支头。需要新的分支头时,必须显式指定:

```bash
mcpp update mylib     # 丢掉记录的 commit,下次构建重新解析
mcpp update           # 同上,对所有依赖
```

既然记录的 commit 已经足以决定构建什么,那么在 `~/.mcpp/git` 里已有克隆的情况下,
重新构建完全不发网络请求,`--offline` 下照常工作。只有两件事需要网络:解析一个在
lock 里没有 commit 的分支,以及克隆一个尚未缓存的 commit。`git =` 若指向本地目录
(或 `file://` URL),这两件事都不需要网络,因此离线下也绝不会被拒绝。

**SemVer 约束**:

```toml
[dependencies]
foo = "^1.2.3"      # >= 1.2.3, < 2.0.0 (caret,默认)
bar = "~1.2.3"      # >= 1.2.3, < 1.3.0 (tilde)
baz = "=1.2.3"      # 精确匹配
qux = ">=1.0, <2.0" # 范围组合
```

#### 命名空间解析规则

每个包的身份是**命名空间 + 名字**二元组。每个 selector 都只规范化成一个身份:

- `cmdline` → `(mcpplibs, cmdline)`;省略 namespace 只表示默认 `mcpplibs`。
- `compat.gtest` → `(compat, gtest)`。
- `mcpplibs.capi.lua` → `(mcpplibs.capi, lua)`。

不存在有序回退或按短名的全索引模糊搜索:

```toml
# ✅ 正确 —— 点式选择器
[dependencies]
chriskohlhoff.asio = "1.38.1"

# ✅ 正确 —— 命名空间子表(同一组织有多个包时更推荐)
[dependencies.chriskohlhoff]
asio = "1.38.1"

# ❌ 错误 —— 裸名永远到不了 chriskohlhoff 命名空间
[dependencies]
asio = "1.38.1"
```

第三种写法会明确报错,指出实际尝试的 `(mcpplibs, asio)`;若该短名存在于别处,错误信息会给出可直接复制的显式 selector。

##### 裸名过渡期(`2026.8.10.1` 起,`2026.9` 移除)

索引里已发布的 `compat.*` 包与既有 manifest **全部**写成裸名(`gtest = "1.15.2"`)。
升级后直接失败,等于让一次程序发布把**已经发布、且无法追溯修改**的数据作废,
所以有一个版本的过渡期:裸名在 `mcpplibs` 未命中时仍可到达 `compat.<name>`,
不声明 namespace 的 descriptor 也仍可被裸名解析。

但它不再静默:

```
warning: dependency 'gtest' resolved to 'compat.gtest' through the deprecated
bare-name search; namespace omission means `mcpplibs` only. Write the exact
package:
    [dependencies.compat]
    gtest = "1.15.2"
  (or run `mcpp add compat.gtest@1.15.2`). This fallback is removed in 2026.9.
```

写进 `mcpp.lock`、install 与 cache 的是**规范身份**;歧义拼写只存在于工程的
`mcpp.toml` 中,直到被改写 —— `mcpp add gtest@1.15.2` 会完成这次改写。

过渡期**不适用于**写明 namespace 的 selector(`mcpplibs.gtest` 未命中就是未命中),
裸名也**仍然**到不了第三方 namespace。

**为什么只允许一个身份?** 因为依赖解析必须可复现。候选搜索会让同短名包受索引状态影响,新增索引还可能悄悄重定向既有依赖。

**给 xpkg 作者:** 索引描述符里,身份是 `(package.namespace, package.name)` 二元组。命名空间是点分路径,**`name` 是单一原子段**:

```lua
package = {
    namespace = "chriskohlhoff",
    name      = "asio",                 -- 单一段;不是 "chriskohlhoff.asio"
}

package = {
    namespace = "mcpplibs.capi",        -- 层级放这里
    name      = "lua",
}
```

文件名只是提示 —— 描述符按声明的身份被发现,所以 `pkgs/c/chriskohlhoff.asio.lua` 与 `pkgs/z/anything.lua` 解析结果完全相同。推荐 `<name>.lua` 或 `<namespace>.<name>.lua`(命中 mcpp 的快路径),但不强制。

旧的完全限定拼写(`name = "chriskohlhoff.asio"`)仍被接受,已发布的描述符无需改动。`mcpp xpkg parse` 会校验该规则,请在索引 CI 里跑它。描述符身份需要 mcpp >= 0.0.106,精确 selector 需要 mcpp >= 2026.8.10.1,两者使用 xlings >= 0.4.69;规范全文见 `docs/spec/package-identity.md`。

`mcpp new --template` 刻意复用同一身份模型，而不是另造包文法:
`[ns.]name[@version][:tname]`。其中裸名同样只表示 `mcpplibs`，version 与模板名可分别
省略。省略 `tname` 时选择唯一显式 default；若未写 `default = true` 且只有一个模板，
该单模板自动成为默认。多个未标默认的模板会报错，绝不按目录顺序选择。规范表见
`docs/spec/package-identity.md` §4.4。
#### mcpp 何时刷新包索引

`mcpp build` / `run` / `test` **只在依赖无法用本地索引解析时**刷新包索引,绝不会
因为"时间到了"就刷。具体地说,只有三种情况会触发:本地根本没有索引、依赖的描述符
不在其中、或 SemVer 约束在本地已知版本里无解。只要所有依赖都能在本地解析出来,
无论本地索引多旧,构建都不会发起任何网络请求。

由此带来的一个需要知道的语义:`^1.2` 这类约束是对**本地索引已知的版本**求解的。
若上游在上次刷新之后发布了 `1.3.0`,需要主动获取:

```bash
mcpp index update     # 同步索引
mcpp update           # 同步索引,并重新解析依赖
mcpp index status     # 看本地现状:状态、年龄、修订号
```

三个开关,优先级从高到低:

| 开关 | 作用 |
|---|---|
| `--offline`(任意命令) | 完全不碰网络——不刷索引、不下载、不自动装工具链,也不发 `git ls-remote`/`clone`。已安装的东西照常构建,包括 commit 已在 `mcpp.lock`、克隆已在缓存里的 git 依赖 |
| `MCPP_OFFLINE=1` | 同上,作用于整个 shell 会话或 CI job |
| `~/.mcpp/config.toml` 里 `[index] auto_refresh = false` | 永不自动刷新索引,但下载仍然可用 |

`MCPP_NO_AUTO_INSTALL=1` 作为 `--offline` 的旧式窄化拼写仍然有效(它只管工具链的
自动安装)。

任意命令加 `-v` 可以看到每个依赖的判定结果与原因。

### 2.6 `[dev-dependencies]` —— 测试依赖

```toml
[dev-dependencies.compat]
gtest = "1.15.2"
```

`mcpp build` 忽略这些依赖;`mcpp test` 解析并使用它们。`mcpp test` 自动发现
`tests/**/*.cpp` 并把它们编译为测试二进制。运行器与框架无关:每个文件是一个独立的
二进制,以退出码判定 —— 裸 `main`、gtest(经 `[dev-dependencies]` + `gtest_main`)
或任何其他框架的行为完全一致,`-- args` 会转发给每个测试二进制
(例如 `-- --gtest_filter=...`)。注意:合成的测试目标名可能包含 `/`
(`tests/00-a/0.cpp` → `00-a/0`),这与 `[targets.*]` 名不同 —— 两个命名空间是
刻意分开的(测试目标从不进入 manifest,也不参与发布)。测试按其相对 `tests/` 的
路径命名(`tests/00-a/0.cpp` → `00-a/0`),每个测试独立编译(一个测试写坏只让它
自己失败;包或依赖损坏则报告为构建错误),`mcpp test <pattern>` 与
`--message-format json` 分别提供过滤与机器可读输出。

### 2.6.1 `[build-dependencies]` —— 构建期依赖(mcpp 2026.8.29.1+)

```toml
[build-dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

段与边上的请求回答的是**两个不同的问题**,把它们混为一谈是建模上的错误,不是写法之争。

- **段**回答:这个包本身进不进目标。`[dependencies]` 进;`[build-dependencies]` 永不进,
  只能经由它到达的东西也一样。
- **边上的请求**回答:要它的哪一种构建期产物。`tools = [...]` 要一个宿主可执行文件,
  `host-module = true` 要一个构建程序可以 import 的模块。

一个包可以同时在两个轴上取值,而 protobuf 正是证明这两个轴必须分开的例子:工程既链接
`libprotobuf`,构建期又需要 `protoc`。它只写一次,写在 `[dependencies]` 里:

```toml
[dependencies]
protobuf = { version = "35.1", tools = ["protoc"] }
```

`[build-dependencies]` 用于第一个轴无法表达的那种组合 —— 某个包的库不得进入目标,
而它的工具或规则仍然需要。同一个包同时出现在两张表里不是错误:普通声明胜出,因为
一行 `[build-dependencies]` 不应该悄悄拿掉目标真正需要的库。

与 `[dev-dependencies]` 不同,这些依赖**会**被传递遍历:一个构建期依赖自己的依赖正是
让它能工作的东西,并且继承它「只服务构建」的性质。

feature 可以为构建期请求划定范围而无需第二个声明处 —— `[feature-deps.<name>]` 可以给
一条已经无条件声明的依赖追加 `tools`,所以「按需才要」不需要另开一张表。

> 这个段很早就能被解析,而直到 2026.8.29.1 之前没有任何做决定的代码读它:写下它得到的是
> 一份能加载的清单、零诊断、零效果。

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

用 `[target.<sel>]` 表把依赖与构建 flag 限定到某个平台。选择器 `<sel>` 有三种形式:

| 选择器 | 含义 | 示例 |
|---|---|---|
| **裸 OS 别名** | 单个 OS / 族 —— 简洁且常用的形式 | `[target.windows]`、`[target.unix]` |
| **`cfg(...)` 谓词** | 复合条件(arch / env / 组合子) | `[target.'cfg(all(linux, not(arch = "aarch64")))']` |
| **精确三元组** | 某个具体目标(同时承载 `toolchain` / `linkage`) | `[target.x86_64-linux-musl]` |

一个选择器可以承载平台条件的**依赖**与**构建 flag**:

```toml
# 简洁的裸别名形式 —— 仅在 Windows 上拉取并链接 OpenBLAS。
[target.windows.dependencies.compat]
openblas = "0.3.33"
[target.windows.build]
ldflags = ["-Llib", "-llibopenblas"]

# cfg(...) 用于复合谓词(文法:all/any/not 作用于 os/arch/family/env,
# 以及裸别名 windows/unix/linux/macos)。
[target.'cfg(all(linux, not(arch = "aarch64")))'.build]
cxxflags = ["-march=x86-64-v2"]
```

`[target.windows]` 与 `[target.'cfg(windows)']` 完全等价 —— 裸别名
`windows` / `linux` / `macos` / `unix` 都不是合法的目标三元组,因此不存在歧义。
单个 OS/族用裸形式,arch/env 条件与组合子用 `cfg(...)`。

- **可用键**:`dependencies` / `dev-dependencies` / `build-dependencies` /
  `feature-deps.<feature>`(mcpp 2026.8.6.2+ —— 见 §2.14;feature 本身无条件注册,
  只有它的依赖集合受限定),以及带 `cflags` / `cxxflags` / `ldflags` / `sources`
  的 `build`(mcpp 0.0.95+ —— 条件源码 glob,例如把 `src/x86/**/*.asm` 收在
  `cfg(arch = "x86_64")` 之后;`!` 排除 glob 在此同样有效),再加 `flags` 与
  `include_dirs` / `include_dirs_after`(mcpp 0.0.102+)。
- **`build` 接受的恰好是*可叠加的构建输入*集合** —— 那些以追加方式合并、
  并在谓词求值之后被消费的东西。`linkage`、`target` 与档案开关刻意不在其中:
  它们是**目标选择的输入**(用一个针对 `target` 求值的谓词去条件化 `target`
  是循环的),或者需要覆盖而非追加的语义。
- **按解析后的目标求值** —— 交叉构建取 `--target` 三元组,否则取宿主。因此原生
  Linux 构建**根本不会下载** `[target.windows]` 依赖。
- **优先级**:精确三元组表胜过 `cfg`/别名表;多个命中的谓词表,其 flag 按序拼接。
  条件项追加在无条件 `[build]` 项**之后**,因此在 GNU「最后一个 flag 生效」的
  规则下,条件规则会覆盖更宽的无条件规则。这正是让按 OS **移除**成为可表达的原因:

  ```toml
  [build]
  flags = [{ glob = "third_party/zlib/**", defines = ["HAVE_UNISTD_H=1"] }]

  # clang-MSVC 没有 <unistd.h>:撤销基础 define,加上 windows 的那个。
  [target.'cfg(windows)'.build]
  flags = [{ glob = "third_party/zlib/**",
             defines = ["NO_FSEEKO"], cflags = ["-UHAVE_UNISTD_H"] }]
  ```

- **未命中当前目标的条件 `flags` 条目根本不存在**,因此它不会产生
  「glob 未匹配到任何源文件」的警告。于是一份 manifest 可以同时携带三个 OS 的
  flag 表,而不会在另外两个上制造噪声 —— 与未启用 feature 的条目根本不存在是
  同一个道理。**无条件**表里的零命中 glob 仍然告警,因为那里它是真实缺陷。
- **`toolchain` / `linkage` / `sysroot` 仅限精确三元组** —— 它们描述某一个具体的交叉目标,
  因此写在 `[target.<triple>]` 下(见上),而不是裸别名或 `cfg(...)` 下。

#### `sysroot` —— 目标的 C 库

`sysroot`(mcpp 2026.8.20.2+)覆盖目标表为某个三元组绑定的 C 库,与 `toolchain`
覆盖编译器 pin 同轴:一个指名目标所解析的编译器,另一个指名它的 C 库,两者在工程
有理由与之分歧之前都只由引擎决定。

```toml
[target.riscv64-none-elf]
sysroot = "xim:newlib-riscv@4.4"     # a different C library
```

```toml
[target.riscv64-none-elf]
sysroot = ""                          # no C library at all
```

⚠️ **键缺席与键为空是两个不同的答案。** 缺席继承目标表的 C 库。存在且为空是
**零 libc 档**:不解析任何 C 库,不加入头文件与库目录,链接行上只有工程与其依赖
提供的内容,`#include <stdio.h>` 不再解析。内核与 bootloader 要的正是这一档,而把
两种情形合并会让这类工程静默地把目标的 C 库拿回去。

取值是 xpkg 引用或空字符串;裸名在解析清单时即被拒绝,因为接受它会导致什么都不安装,
然后在很晚的时候以「缺少 libc」失败。

构建程序可以询问解析到的是哪份 C 库:`mcpp::target_libc()` 返回其包名,
`mcpp::target_libc_profile()` 返回目标 ISA 档位对应的子目录。零 libc 档上两者均为空。
参见[13 —— 裸机与 freestanding 目标](13-baremetal.md)。

### 2.7.2 裸机(`os = none`)—— freestanding target

`riscv64-none-elf` 与 `riscv32-none-elf` 是底下没有操作系统的 target。它们不需要
逐宿主的交叉工具链:clang 与 lld 天生是交叉编译器,任何能装 llvm 载荷的宿主都能
产出它们。

本节是清单参考。示例部分 —— 生成工程、运行、在目标上测试、freestanding 标准库
子集,以及编写板级支持包 —— 在
[13 — 裸机与 freestanding 目标](13-baremetal.md)。

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

### 2.8 `[features]` —— Feature(Cargo 风格,可加性)

#### 表形式 —— 让 feature 贡献的不止是隐含 feature

`[features]` 的条目除了写成数组,还可写成**表**,从而让该 feature 在隐含 feature
之外,携带包自有的预处理 `defines`、feature 门控的源 glob(`sources`,mcpp
0.0.95+——列出的 glob 离开默认构建,仅当 feature 激活时才编译,与 index 描述符的
`features.<f>.sources` 完全对等;这正是 vendored 大库最高频的形态:*feature =
一组源文件 + 一个 define*)、feature 门控的 per-glob 编译旗标(`flags`,mcpp
0.0.101+),以及 capability 的 `requires` / `provides`(见 §2.8.1):

```toml
[features]
default    = []
# 数组简写:仅隐含 feature。
docking    = ["extra"]
extra      = []
# 表形式:激活时贡献一个包自有的宏。
mpl2only   = { defines = ["EIGEN_MPL2_ONLY"] }
# 表形式:宏 + 一个隐含 feature。
fast_math  = { defines = ["APP_FAST=1"], implies = ["extra"] }
# 表形式:feature 门控源 + 与其同居的 per-glob 旗标。
simd       = { sources = ["src/simd/**"], flags = [
                 { glob = "src/simd/**/*.avx2.cpp", cxxflags = ["-mavx2"] } ] }
```

- `defines` 为**裸**宏名(不带 `-D`);feature 激活时每个脱糖为 `-D<x>`,加到该包
  自己的编译上——与 `[targets.*] defines` 完全一致。按约定仅限包**自有**的带命名
  空间宏:feature **不**注入自由的包级 `cflags`/`ldflags`,否则会破坏加性的 feature
  并集模型。链接旗标来自 provider 依赖(§2.8.1),而非 feature。
- 每个激活的 feature 仍会得到自动的 `-DMCPP_FEATURE_<NAME>`,`defines` 与之叠加。
- `flags`(mcpp 0.0.101+)与 `[build].flags`(§2.3)共用同一有序 inline-table 数组
  文法(`glob` 必填,加 `cflags`/`cxxflags`/`asmflags`/`defines`;与
  `[[build.flags]]` 一样也接受 `[[features.<name>.flags]]` 拼写)。feature 激活时
  条目追加在 base `[build].flags` **之后**(feature 按名
  序),"last flag wins" 使 feature 规则可覆盖更宽的 base 规则;未激活时条目根本
  不存在(不会有死 glob 告警)。这让 feature 的组内专属旗标与其 `sources` 同居,
  而不必写成 base 规则、在 feature-off 构建里留下必死的 glob。与 `defines` 不同,
  feature `flags` 是**私有 per-TU 构建旗标**——永不传播给消费者(与 `[build].flags`
  同契约),因此不破坏加性模型:glob 限定作用面、顺序确定、无跨包效应。

### 2.8.1 `provides` / `requires` —— 能力(后端选择)

**capability(能力)** 是一个共享的抽象名字(如 `blas`)。包可以 *provide*(提供)
一种能力;feature 可以 *require*(需要)一种能力而非点名某个具体包,解析器会从依赖
图中绑定**恰好一个** provider。这样就能在多个可互换后端(OpenBLAS / MKL / …)中选其
一,而不必把选择写死进库里。

```toml
# provider 包为任何 require 它的依赖方满足某能力。
[package]
name     = "compat.openblas"
version  = "0.3.0"
provides = ["blas", "lapack"]
```

```toml
# 消费方经由自己的某个 feature 来 require 这个抽象能力。
[features]
use_blas = { defines = ["EIGEN_USE_BLAS"], requires = ["blas"] }

# 图中有 >1 个 provider 时,选其一(否则构建报错并列出候选)。
[capabilities]
blas = "compat.openblas"     # 等价于:mcpp build --cap blas=compat.openblas

[dependencies]
compat.openblas = "0.3.0"    # provider 必须是图中真实存在的依赖
```

保留前缀 `mcpp:` 命名本引擎解析的目标侧层,这些名字对照一个闭集校验。
包级 `requires` 数组承载对称的陈述 —— 某个目标侧层必须解析为什么,
本包才可用。

```toml
[package]
name     = "acme.llvm-runtime"
version  = "0.1.0"
provides = ["mcpp:compiler-runtime=compiler-rt", "mcpp:c++-abi=libc++"]
requires = ["mcpp:compiler=llvm"]
```

作为标准库的包在 `[build]` 下陈述它的 `std` 模块源,
其所需的 flag 在那里与任何其它构建输入一样可条件化。

```toml
[build]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc++"]

[target.'cfg(c-abi = "musl")'.build]
std-module-flags = ["-D_GNU_SOURCE"]
```

五个层、约束它们的规则与相应诊断,见 [14 - 目标侧](14-target-side.md)。

绑定是**确定性**的:

| 图中某被需要能力的 provider 数量 | 结果 |
|---|---|
| 恰好一个 | 自动绑定(无需配置) |
| `[capabilities]` pin / `--cap` 指定了一个 | 以 pin 为准 |
| 零个 | **报错**:没有包提供 `<cap>` |
| 两个及以上且未 pin | **报错**并列出候选——绝不静默猜测 |

被绑定 provider 的链接/头文件旗标经由常规依赖机制流到消费方;capability 层是那道
*选择与校验* 步骤,把"静默选错后端 / 缺后端"变成构建期的显式报错。

**绑定选中的是 provider,它不裁剪链接行。** 依赖包的目标文件一律进入消费方的链接,
与它的能力是否被绑定无关。实测:两个包都提供同一能力且都定义 `cap_probe`,未 pin 时
解析按上表报错;按提示用 `[capabilities]` pin 其中一个之后,构建走到链接器才失败——

```
ld: obj/mcpplibs_pa/src/impl.o: in function `cap_probe':
    multiple definition of `cap_probe'; obj/mcpplibs_pb/src/impl.o: first defined here
```

这一点对**多个 provider 定义同一批符号**的能力有影响 —— 全程序单例(例如
`operator new`),或名字集合固定的 C 接口。对这类能力,图中出现两个 provider 是**待修的
缺陷**而非可 pin 的歧义:pin 会把一个点名两个候选的报错,换成一个点名 mangled 符号的报错。
可互换的**库**(各 BLAS 实现导出不同的符号集合,按链接选其一)不受此影响。

### 2.8.2 `[feature-deps.<name>]` —— 由 feature 拉取的依赖

在 `[feature-deps.<name>]` 下声明的依赖是**可选的**:仅当该 feature 激活时(根 `--features`,
或某依赖 spec 的 `features = [...]`)才会被解析。`[dependencies]` 中的依赖始终被解析;
可选性由声明的*位置*表达,而非某个标志位。

```toml
[features]
use_blas         = { defines = ["EIGEN_USE_BLAS"], requires = ["blas"] }
backend-openblas = { implies = ["use_blas"] }

# 仅当 `backend-openblas` 激活时才拉取。每个条目都是完整的依赖 spec
#(version/path/git + 其自身的 features)。
[feature-deps.backend-openblas]
compat.openblas = "0.3"
```

⚠️ **写 `"^0.3.0"`,而不是 `"0.3.x"` 或 `"0.3"`。** 以索引中确定存在的包作对照,
判据取**构建成功**:

| 写法 | 结果 |
|---|---|
| `cmdline = "0.0.1"` | 构建通过 |
| `cmdline = "^0.0.1"` | 构建通过 |
| `cmdline = "0.0"` | 解析通过,随后 `install path missing after fetch` |
| `cmdline = "0.0.x"` | `E_NOT_FOUND`,点名的是包 —— 而该包存在 |

⭐ 这三种结果值得分开,因为两个更弱的判据各自会放行一种不可用的写法:
「没有 `E_NOT_FOUND`」放行两段前缀,「解析通过」同样放行它。**只有对着真实索引构建
一次**才能定论。

⚠️ 这一点在此处比在 `[dependencies]` 中更要紧:**实现取不回来的 feature 等于不存在的
feature**,而开发期使用 **path** 依赖的工程根本不查索引 —— 该失败只在发布之后才出现,
而且是出现在别人身上。

该机制与能力(§2.8.1)组合:单个 `backend-openblas` feature 既**拉取** provider
(`compat.openblas`,其 `provides = ["blas"]`),又**开启**消费方开关
(`implies = ["use_blas"]`,其 `requires = ["blas"]`)。当图中只有一个 provider 时,
能力自动绑定——消费方只需写 `features = ["backend-openblas"]`。

在索引包的 Lua 描述符中,等价写法为内联形式:

```lua
features = {
    use_blas         = { defines = { "EIGEN_USE_BLAS" }, requires = { "blas" } },
    ["backend-openblas"] = {
        implies = { "use_blas" },
        deps    = { ["compat.openblas"] = "0.3.x" },
    },
}
```

#### 保持可替换的默认实现

同样这三件东西,也覆盖"库希望**提供**一份实现但不**强加**一份"的情形 —— 全程序单例,
例如 `operator new`、日志 sink、panic handler:

```toml
[features]
default   = []
# 消费方开关:"我用到了本库中需要分配器的那部分"。
alloc     = { requires = ["freestanding-allocator"] }
# 内置默认:激活它就够了。
alloc-kal = { implies = ["alloc"] }

# 仅在 `alloc-kal` 激活时解析,因此库本体不携带对该实现的依赖。
[feature-deps.alloc-kal]
std-freestanding-alloc-kal = "0.1.x"
```

三种用法各一行:

| 消费方需要 | 清单里怎么写 |
|---|---|
| 不用会分配的那部分 | `std-freestanding = "0.2.0"` —— 分配器不进图 |
| 默认实现 | `features = ["alloc-kal"]` —— 实现随之进图,**无需知道其包名** |
| 自己的或第三方的 | `features = ["alloc"]` 加一个 `provides = ["freestanding-allocator"]` 的包 |

有两条性质使该形状优于无条件随包提供实现。随包提供实现的库替程序做了本属程序的决定,
而且**撤销不掉**:feature 是**加性**的,消费方没有把某个默认**关掉**的手段。以及,由于
依赖包的目标文件无条件参与链接(§2.8.1),随包的默认加上程序自备的那份是**重复定义**
而非替换 —— 让 C++ 标准库能提供可替换 `operator new` 的那套归档语义,对包依赖并不适用。
把实现放在开关之后,意味着两者**从不共存**。

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

### 2.13 `[xlings]` — 构建环境

```toml
[xlings]
deps  = ["make@4.4", "cmake@3.28", "python@3.13"]   # 要供给的 host 构建工具
subos = "dev"                                        # 命名的项目级沙箱

[xlings.workspace]                                   # 固定工具版本([toolchain] 的通用形式)
clang = "20.1.7"

[xlings.envs]                                        # 应用到工具环境的环境变量
OPENBLAS_NUM_THREADS = "1"
```

声明项目的**构建环境**,经 xlings(mcpp 的底座)供给。子段名与 xlings 自身的
`.xlings.json` schema **1:1** 对齐,因此 mcpp 把它们**原样**物化进
`<项目>/.mcpp/.xlings.json`(无翻译层):`deps`(host 构建工具)、`[xlings.workspace]`
(工具→版本固定)、`subos`(命名沙箱)、`[xlings.envs]`(环境变量)。用它声明构建所需的
host 工具(`make`/`cmake`/`protoc`…)、按项目固定工具版本、或设构建期环境变量——无需手改
`.xlings.json`。`[toolchain]`(§2.7)仍是编译器的便捷简写;`[xlings.workspace]` 是其通用形式。

`subos` 选择根项目用于 build/run 的**本地开发 OS 环境**。未声明该键时固定使用 mcpp 已初始化、
经 release 验证的 `McppDefault`;`subos = "default"` 则仍是显式的
`NamedSubos("default")`。没有 CLI/环境变量 override,也不会隐式跟随 xlings active/current。

在 Linux 上,所选环境同时固定 loader/libc contract,所以 `el8`、`trixie` 可在同一机器共存,
并进入不同构建指纹。workspace 整体构建时由 workspace root 覆盖 member 声明;member/依赖中的
SubOS 不传递——库只有作为独立 root 开发时才使用自己的声明,作为别人的源码依赖时使用消费者
root 的环境。指定的命名 SubOS 不存在、缺少或使用不兼容 runtime contract 都会直接报错,不会
回退 default/active/编译器烙入状态。参见 docs/08-toolchain-internals.md §2.1。

### 2.14 依赖产出的 host 工具(mcpp 2026.8.5.1+)

一个包能构建出消费者在**构建期**需要的二进制 —— `protoc`、`grpc_cpp_plugin`、
`flatc`、`moc`、转译器。在依赖上声明:

```toml
[dependencies]
protobuf = { version = "35.1",   tools = ["protoc"] }
grpc     = { version = "1.83.0", tools = ["grpc_cpp_plugin"] }
```

每个名字必须是该包的一个 `kind = "bin"` target。mcpp 会**为构建机器**构建它,
并把绝对路径以 `MCPP_DEP_<PKG>_BIN_<TOOL>` 交给 `build.mcpp` —— 用
`mcpp::dep_bin("protobuf", "protoc")` 读取(见 [07 — build.mcpp](07-build-mcpp.md))。

四条值得知道的性质:

- **永远是 host 二进制。** 即使 `mcpp build --target <triple>`,工具依然为**本机**
  构建 —— 代码生成器必须在这里跑。它是一次独立的、面向 host 的子构建:工具包
  自己的 `[toolchain]`、自己的依赖解析生效,不需要与当前构建一致。安全的原因是
  可执行文件与工程代码**零 ABI 接触**。
- **单一版本轴。** 工具的版本**就是**依赖的版本,所以「protoc 与其运行时不匹配」
  这种情况**不可表达**。(把工具单独打包正是会出这个问题,而且它在**运行期**才咬人,
  不是编译期。)
- **默认关闭。** 没人要就什么都不构建,成本由消费者付。包用 `[features]` +
  `required_features` 给昂贵的部分加门(protobuf 的 `protoc` 需要 libprotoc 的
  ~157 个额外 TU,只用运行时的人绝不该编译它)。
- **全局缓存**,按 包版本 × host 工具链 × feature × 自身依赖闭包 键控 —— 每台机器
  构建一次,而不是每个工程一次。

#### `[tools.overrides]` —— 使用已有的二进制

```toml
[tools.overrides]
"compat.protobuf:protoc" = "/usr/bin/protoc"
```

或者不改 manifest(CI、发行版打包):

```bash
MCPP_TOOL_PROTOBUF_PROTOC=/usr/bin/protoc mcpp build
```

命中 override 会**完全跳过构建**。每个同类系统都提供这条逃生舱(vcpkg 的
`VCPKG_HOST_TRIPLET`、CMake 的 `LLVM_NATIVE_TOOL_DIR`、Qt 的 `QT_HOST_PATH`),
理由一样:一个在本机构建不出来的工具**不能是死路**。它**刻意不进** cache key ——
逃生舱不是可复现输入。

#### `host-module = true` —— 可复用的构建规则以包分发

一条规则(比如「对这些 `.proto` 跑 protoc」)应该**写一次**,而不是复制进每个
消费者的 `build.mcpp`。把它做成普通的 mcpp 库包再 import:

```toml
[dependencies]
protobufgen = { version = "0.1.0", host-module = true }
```

```cpp
// build.mcpp
import mcpp;
import protobufgen;
int main() { return protobufgen::generate({"schema"}) ? 0 : 1; }
```

mcpp 会把该包的 lib 根模块**为 host 编译,且与 `build.mcpp` 在同一条命令里** ——
这正是 BMI 能用的前提:一个模块接口只对「在 standard / dialect / 编译器身份上与
它一致」的编译可导入。

于是规则**有版本、能测试、能通过既有的包管理器分发**,而且是用 **C++** 写的
—— 不引入第二门语言,这正是 `build.mcpp` 存在的理由。

**模块名是规则源码自己声明的那个**(mcpp 2026.8.29.1+)。`export module
acme.rules.protobuf;` 就以 `acme.rules.protobuf` 被 import,与包叫什么无关。
模块名是作者定义的 API,不镜像包身份 —— 普通库包一直遵循的就是这条规则。

2026.8.29.1 之前 host 模块这条路径注册的是裸 `package.name`,于是声明名与包名
分叉的规则包在 GCC 上能构建、在 Clang 与 MSVC 上失败:GCC 的 BMI 隐式落在
`gcm.cache` 且按**声明名**索引,而另外两者拿到的是显式的 `<name>=<bmi>` 映射。
因此包名不再承担任何 C++ 命名约束,`grpc-rules` 重新是合法包名。

**两个规则不得声明同一个模块名。** `import` 寻址的是模块,所以两个这样的包对编译器
不可区分,而它们的 BMI 与对象文件同名 —— 后者覆盖前者,存活的那个对象被送进链接两次。
mcpp 拒绝这种情形,并点名两个包与各自的 interface 路径。检查的范围是一次 `build.mcpp`
能看见的那些规则,不是索引级的全局唯一性 —— `path` 依赖与私有 registry 本来就绕得开。

**`mcpp.` 前缀保留给由 mcpp 项目维护的规则。** 不在 `mcpp` 命名空间下的包声明该前缀
的模块名时给出一条同时点名两者的警告,构建继续。之所以是警告:引擎判定不了谁是官方,
`path` 依赖、私有镜像与内部 fork 都合法,而且从这里看都一样。

lib 根必须在 `src/<name>.cppm`(或 `[lib] path` 指向的位置);缺失时报
*"host module 'x': no interface unit at …"*。

*仅构建期:* `host-module = true` 的依赖**不会**被编进、也不会被链进本工程的 target,
它所依赖的东西也不会。它只在 `build.mcpp` 期间运行,别处都不出现。(2026.8.5.2 之前
它还会被当作普通库再编一遍,这正是规则里 `import mcpp;` 失败的原因:在那第二次编译里
内置模块并不存在。2026.8.29.1 之前被排除的只有规则本身,它自己的 `[dependencies]`
仍会被编译并链进消费者的二进制,而规则却 import 不到它们。)

#### 依赖另一个规则的规则(mcpp 2026.8.29.1+)

规则在自己的 `[build-dependencies]` 里声明所需之物,并可以 import 其中标了
`host-module = true` 的条目:

```toml
# 写在规则包自己的清单里
[build-dependencies]
globbing = { path = "../globbing", host-module = true }
```

```cpp
// 规则自己的接口
export module tidyrule;
import std;
import mcpp;
import globbing;
```

mcpp 先编译内层规则,同一条命令、同一套 flag,因此 BMI 的一致性仍是结构性事实而不是
需要事后校验的性质。

消费者**不可以** import `globbing`:构建期的 provision 只在 `reexport = true` 的边上
再跨一跳,而 mcpp 自己执行这条规则,不交给编译器 —— 在 GCC 上那个 import 会成功,
然后在别人的机器上失败。

*限制:* 每个 host 模块只有一个接口单元。带实现单元或多个模块的库还不能作为规则的
构建期依赖。

#### `reexport = true` —— 由库替用户拉起整条工具链(2026.8.6.2+)

上面这些都由**使用工具的人**声明。当知识本来属于库时,这个位置就错了:gRPC
的代码生成需要 protobuf 的 `protoc`,而 gRPC 包的任何使用者都不应该知道这件事。

`reexport = true` 把一条边上的构建期提供物 —— 它的 `tools`、它的
`host-module`、以及该依赖的目录 —— 交给**本包自己的消费者**:

```toml
# 写在 grpc 包自己的 manifest 里
[feature-deps.codegen]
"compat.protobuf" = { version = "35.1",   tools = ["protoc"],          reexport = true }
grpc-plugin       = { version = "1.83.0", tools = ["grpc_cpp_plugin"], reexport = true }
grpcgen           = { version = "1.83.0", host-module = true,          reexport = true }
```

于是使用者只写一行,再 import 那个规则:

```toml
[dependencies]
grpc = { version = "1.83.0", features = ["codegen"] }
```

```cpp
// build.mcpp
import mcpp;
import grpcgen;
int main() { return grpcgen::generate_all() ? 0 : 1; }
```

- **默认关闭,并且刻意不复用边上的 `visibility`。** `visibility` 本身默认就是
  `"public"`,复用该可见性意味着任意深度的依赖都能静默地向构建程序的工具
  命名空间里塞东西。「把某样东西交给消费者」是一条供应链主张,必须写下来。
- **一次声明只走一跳。** 被再导出的提供物到达声明它的那个包的消费者;要继续
  往上走,下一个包必须自己也写 `reexport`。每个包只决定**它**交出什么。
- **feature 可以往一条已经声明过的依赖上追加请求。** gRPC 无条件依赖 protobuf,
  而它的 `codegen` feature 往同一条边加 `tools = ["protoc"], reexport = true`。
  `tools` 与 `features` 取并集,`host-module` 与 `reexport` 取或;`version` /
  `path` / `git` 不合并 —— feature 仍然无法静默覆盖无条件条目的身份。
- **传播的是可见性,不是执行。** `dep_bin()` 只返回路径,跑不跑仍由消费者的
  `build.mcpp` 决定;谁构建了这个工具、tool store 怎么做键,都不改变。
- **裸名由阶梯决定,而不是靠运气。** 一旦两个库都能再导出,它们可能同时提供
  尾名 `protobuf`。全限定的 `MCPP_DEP_<NS>_<NAME>_BIN_<TOOL>` 总是发布;裸名
  依次绑定到 `mcpplibs.<x>`、`compat.<x>`、无命名空间的 `<x>`,最后才是「剩下
  的唯一候选」——存在争用时 mcpp 会说出来,而不是默默选一个。

##### 旧版 mcpp 读到用了这些键的 manifest

不认识的依赖键会被**记为降级**并忽略(mcpp 2026.8.6.2+),因此一份为更新的
mcpp 写的包仍然能加载,这个读取器认识的部分照常生效。在那之前它是**整份加载
失败**且报错误导,这正是「已发布的包永远无法采用新键」的原因——与索引下限确立
的是同一条性质:**数据不得决定程序是否可用**。

因此,一个**依赖** `reexport` 才有那套人机工程的包,仍然需要足够新的客户端;
变化在于该包的其余部分在旧客户端上不再一起失效。

##### 按平台裁剪提供物

一个包可能只在部分平台声明 `bin` 目标。既然现在是**库**决定请求什么,无条件的
请求就会把「不支持的平台」变成用户改不掉的硬错。用条件段裁剪:

```toml
[target.'cfg(not(windows))'.feature-deps.codegen]
"compat.protobuf" = { version = "35.1", tools = ["protoc"], reexport = true }
```

`[target.<sel>.feature-deps.<feature>]`(2026.8.6.2+)与 `[target.<sel>]` 下的
其余依赖表(`dependencies` / `dev-dependencies` / `build-dependencies`)遵循同
一套谓词规则,针对**解析后的 target** 求值。**feature 本身在所有平台都注册**
—— 只有它拉进来的东西是条件性的 —— 因此在没有任何谓词匹配的平台上请求它,不是
「未知 feature」错误。

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

见 [07 — build.mcpp](07-build-mcpp.md)。把这类文件写进 `[build].ldflags` 也「能用」,
但 ldflags 是链接命令里的一串字符:没有任何东西跟踪它,改了它得到的是
`ninja: no work to do`。

### 2.16 `[hooks]` —— 项目构建生命周期命令(实验性)

> **实验性。** Hook 目前**不能**决定一次构建成功与否。所有 Hook 失败都以
> **warning** 报出,`mcpp build` 保留它自己挣来的结果;`side_effect = true`
> 会被报错拒绝,而不是被采纳。这个键保留在 schema 里,这样今天写下的 manifest
> 在该功能转正时无需改动。另有两条限制是永久的、不是临时的:**只有根项目的 Hook
> 会执行**,而且**只有 `mcpp build` 会执行它们**。

Hook 是 `mcpp build` **在一段区间内持有**的命令,事件名就是那段区间:

```toml
[hooks]
build_start = "echo build started"
build_failed = "notify-send 'build failed'"
build_finished = "notify-send 'build finished'"

# 可选;以下是默认值。
timeout_seconds = 10
enabled = true
side_effect = false           # 实验期内 `true` 会被拒绝
```

| 键 | 类型 | 默认值 | 它命名的区间 |
|---|---|---:|---|
| `build_start` | 命令 | — | 项目准备完成后开启,命令退出时闭合 |
| `build_finished` | 命令 | — | 构建成功后开启,命令退出时闭合 |
| `build_failed` | 命令 | — | 构建失败后开启,命令退出时闭合 |
| `during_build` | 命令 | — | 构建开始前开启,构建结束后闭合 |
| `timeout_seconds` | 整数,1–86400 | `10` | 单次运行的时限 |
| `enabled` | 布尔 | `true` | 是否启用本表中的全部命令 |
| `side_effect` | 布尔 | `false` | Hook 失败是否让本次构建失败。**保留键**——实验期内只接受 `false` |

前三个区间是**自闭合**的——命令退出,区间就结束。"同步"在这里不是一种单独的模式,
它就是自闭合区间的样子。`during_build` 是唯一由别的东西闭合的区间,而那两个只对其中
一种形状有意义的键,是从这一点推出来的,不是额外规定的例外。

命令写成字符串;需要选项时写成表:

| 表内键 | 适用于 | 含义 |
|---|---|---|
| `cmd` | 所有事件 | 命令本身,必填 |
| `timeout_seconds` | 自闭合事件 | 覆盖本表默认值 |
| `loop` | `during_build` | 命令在区间闭合前退出时重新启动 |

`loop` 写在自闭合事件上、`timeout_seconds` 写在 `during_build` 上,都是**错误**而不是
被忽略的键:自闭合区间随命令退出而结束,没有东西可重启;而 `during_build` 已经由构建
定界。一个被接受却什么都不做的键,读起来就是"这功能坏了"。

命令通过宿主 Shell(`/bin/sh` 或 `cmd.exe`)执行,工作目录是**项目根目录**——不是敲
`mcpp build` 的那个目录,所以 Hook 里的相对路径在哪儿发起构建都指同一处。自闭合命令
的标准输入、输出和错误沿用普通终端行为。没有配置的事件直接跳过。

生命周期为:

```text
during_build 开启
build_start
    ├─ 构建成功 → during_build 闭合 → build_finished
    └─ 构建失败 → during_build 闭合 → build_failed
```

`during_build` 在终止 Hook **之前**闭合,因此两条命令不会重叠执行。

`build_failed` 与 `build_finished` 互斥,而且两者都只在 `build_start` 已经执行之后
才可达。项目**准备**阶段就失败的情况——manifest 非法、依赖无法解析、没有可用工具链
——一个 Hook 都不触发:此时构建尚未开始,而 Hook 程序本身可能正是准备阶段要装的东西。

命令无法启动、返回非零或超过时限均视为 Hook 失败。`during_build` 还多一种:开了 `loop`
的命令**起不来**——连续五次在一秒内以非零状态结束——就不再重启,并被报出来。(很快就
成功结束的命令,正是 `loop` 被要求重复的那件事,不算失败。)以上每一种都以 **warning**
报出,构建保留它自己挣来的结果——`[hooks]` 还在实验期,它没有投票权。Hook 自身失败不会
再触发另一个 Hook。

改变这一点的正是 `side_effect = true`,而今天写它是一个错误:

```text
error: mcpp.toml: error: [hooks].side_effect = true is not available yet:
[hooks] is experimental and cannot decide whether a build succeeded. …
```

是拒绝而不是悄悄降级,因为两种沉默的做法都更糟:采纳它等于让一个实验性功能对每一次
构建都有否决权;忽略它则让项目以为自己的构建被通知程序把着关,而实际上没有。功能转正
后,`true` 的含义是"Hook 失败让构建失败"——而构建自身失败时仍保留它自己的退出码,所以
`mcpp build` 不会把一次编译错误报成通知程序的问题。

关于 `during_build` 有两件事值得单独知道:

- **它的输出被丢弃**,因为它与构建并发写出,否则会插进某条编译诊断的中间。要看它的
  输出就跑 `mcpp build --verbose`。
- **停止的单位是进程树,不是进程。** `player & wait` 让播放器成为 mcpp 所启动那条命令
  的孙子进程,只停掉后者会让音频设备在构建结束后仍被占着。mcpp 把命令放进它自己的
  进程组(Windows 上是 job object)并停止整组,构建被 Ctrl-C 打断时也一样。

作用范围:

- 只有 `mcpp build` 执行 Hook。`mcpp run`、`mcpp test` 和
  `mcpp build --configure-only` 同样会构建,但有意不执行。
- Hook 属于**被构建的那个包**。workspace 展开时就是逐个成员:各自的 `[hooks]`、
  各自的构建、各自的根目录。**虚拟** workspace 根(只有 `[workspace]` 没有
  `[package]`)不构建任何东西,写在那里的 `[hooks]` 永不触发。
- 依赖的 `[hooks]` **一律跳过**,只有根项目的会执行。mcpp 解析的每一份 manifest 都
  带着这一节,依赖的也带,而没有任何东西去读它——这正是"装一个包"不会变成"在我下次
  构建时跑包作者的 Shell 命令"的原因。这是设计的性质,不是一个等着被打开的默认值。
- 声明了生效的 Hook 就等于让项目放弃空转快路径,因为 `build_start` 规定在准备阶段之后
  执行。对已经是最新状态的带 Hook 项目,`mcpp build` 的代价是一次准备,而不是毫秒级。

`[hooks]` 里、以及某个事件表里不认识的**键**都是 warning(`--strict` 下为错误),所以为更新版 mcpp 写的
manifest 在这一版仍能加载;不认识的**值**——`cmd` 缺失或不是字符串、`timeout_seconds` 不在
1–86400 之间、键写给了错误的区间——是 manifest 错误。

> **Hook 是代码,而 `mcpp.toml` 是仓库的一部分。** 构建一个刚克隆下来的项目,会以
> 执行 `mcpp build` 的那个账户的权限,运行它 `[hooks]` 里写的任何东西。这与
> `build.mcpp`([07 — build.mcpp](07-build-mcpp.md))已经要求的信任是同一份;
> `[hooks]` 扩大的是它的范围,而不是引入了一份新的信任。

Hook 程序可以作为普通 xlings 依赖安装。例如,音频通知程序可以把音频内置进自己的
可执行文件,无需让 mcpp 处理媒体资源:

```toml
[hooks]
build_finished = "mcpp-hooks-audioplayer niulai-mm"
build_failed = "mcpp-hooks-audioplayer niulai-niulai"
side_effect = false

[xlings]
deps = ["xim:mcpp-hooks-audioplayer@0.0.1"]
```

根据构建成功或失败播放不同提示音。`side_effect = false` 写出来而不是靠默认值:它是
这份 manifest 自己就想要的值——缺个音频设备不该让构建失败——所以等这个键有了不止
一个可接受的值之后,它仍然会这么写。

## 附录 A. Schema 所有权原则(新字段准入标准)

> **语法封闭,词汇开放**:谁拥有解析语义谁定义键;谁拥有领域知识谁定义值。

- mcpp 只定义**机制**(features 并集/闭包、capability require/provide/override、
  profile→编译器旗标、platform→triple),键与形状固定;feature 名、能力名、
  后端名等**领域词汇只出现在值里**,不进 mcpp 代码。
- **不支持包自定义 toml 键**:键合法性不得依赖"先解析目标包",否则 manifest
  失去静态可解析性(lockfile/LSP/审计的前提)。包的扩展点 = 固定机制内的开放值域。
- 包级旋钮统一收敛进 features;糖键(如 `backend=`)进入核心语法须满足:
  ① 领域中立(跨生态通用模式)② 1:1 脱糖、零新增解析语义。
- 字段归属总表与定型决策见
  `.agents/docs/2026-06-04-manifest-schema-ownership.md`。

## 3. 实战示例

### 3.1 简单 Hello World

```toml
[package]
name    = "hello"
version = "0.1.0"
```

```cpp
// src/main.cpp
import std;
int main() { std::println("Hello, mcpp!"); }
```

```bash
mcpp build && mcpp run
```

### 3.2 模块化库 + 测试

```toml
[package]
name    = "mymath"
version = "1.0.0"

[targets.mymath]
kind = "lib"

[dev-dependencies.compat]
gtest = "1.15.2"
```

```cpp
// src/mymath.cppm
export module mymath;
export int add(int a, int b) { return a + b; }
```

```cpp
// tests/test_add.cpp
#include <gtest/gtest.h>
import mymath;
TEST(Math, Add) { EXPECT_EQ(add(1, 2), 3); }
```

```bash
mcpp build   # 编译库
mcpp test    # 编译 + 跑测试
```

### 3.3 依赖其他包的应用

```toml
[package]
name    = "myapp"
version = "0.1.0"

[dependencies]
ftxui = "6.1.9"

[dependencies.mcpplibs]
cmdline = "0.0.2"
llmapi  = "0.2.5"
```

mcpp 自动:
1. 从 mcpp-index 下载源码 tarball
2. 按 `[build].include_dirs` 传播头文件路径
3. 传递依赖自动入图(llmapi → tinyhttps → mbedtls 全自动)

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

### 3.6 跨编译静态发布

```toml
[package]
name    = "mytool"
version = "1.0.0"

[toolchain]
default = "gcc@16.1.0"

[target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
linkage   = "static"
```

```bash
mcpp build --target x86_64-linux-musl
# → 产出完全静态链接的二进制,可直接 scp 到任意 Linux x86_64 机器运行
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
