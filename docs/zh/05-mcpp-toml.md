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
soname = "libmylib.so.1"  # 可选: ELF/Mach-O ABI 名称,运行时会生成同名 alias
```

`soname` 用于共享库的 ABI 名称,类似 Autotools/CMake 中的
`SOVERSION`/`SONAME`。在 Linux 上,mcpp 会向链接器传递
`-Wl,-soname,<name>`,并在输出目录生成 `<name> -> lib<target>.so` alias,
让下游程序可通过标准 ABI 名称 `DT_NEEDED` 或 `dlopen()` 加载该库。
该字段只对 `kind = "shared"` 有效,值必须是文件名 basename。

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

| 你想要 | 用 |
|---|---|
| 某二进制**自己入口**上的不同宏/标志 | per-target `defines` / `cxxflags`(见上) |
| 两个产品差异在它们**共享**的代码里 | 拆成 [workspace](06-workspace.md) member,各自 `[build]` 标志,共享一个 `lib` |
| **选择**某共享库的变体(如某后端) | 在该库上用 `[features]`(§2.8)——additive,作用到库自己的编译 |
| **整次构建的模式**(sanitizer、契约语义、优化档) | `[profile.<name>]`(§2.9)+ `--profile`;`mcpp test --profile <name>` 同样支持 |

mcpp 刻意不在一次构建里把同一个共享源编译成两份:一个源对应一个对象(模块还对应一个 BMI),
所以"必须穿透共享代码"的差异应放在包/feature 边界,而非单个目标上。

### 2.3 `[build]` — 构建配置

```toml
[build]
sources      = ["src/**/*.cppm", "src/**/*.cpp"]  # 源文件 glob(默认: src/**/*.{cppm,cpp,cc,c,S,s,asm})
include_dirs = ["include", "third_party/include"]  # 头文件搜索路径
include_dirs_after = ["*"]         # 排在系统目录之后搜索的头文件目录(-idirafter)
c_standard   = "c11"              # C 源文件的标准(默认 c11)
cflags       = ["-DFOO=1"]        # 额外 C 编译参数
cxxflags     = ["-DBAR=2"]        # 额外 C++ 编译参数(不要放 -std=...)
ldflags      = ["-lfoo"]          # 额外链接参数
defines      = ["BIZ=1", "QUX"]   # 作用于每个 TU 的预处理宏(脱糖为 -D;会进入模块扫描)
cxx_runtime  = "self-contained"   # C++ 运行时契约(见下节);static_stdlib 是旧拼写
macos_deployment_target = "14.0"   # macOS 产物的最低支持系统版本(仅 macOS 生效)
cache        = "global"           # 依赖的全局构建缓存:global(默认)| local | off(见 §2.10)
```

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

### C++ 运行时契约(`cxx_runtime`)

`cxx_runtime` 声明的是**产物对运行它的机器做出的承诺**。它是**分发**属性而非
构建属性 —— 它描述的是运行期依赖集,而兑现它的 flag 逐平台不同。

```toml
[build]
cxx_runtime = "self-contained"          # 作用于所有目标(默认值)

# 或者按角色分别指定:
[build.cxx_runtime]
default = "self-contained"              # 可执行文件与共享库
tests   = "host-coupled"                # 测试二进制从不离开本机

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
libc++/libc++abi —— 系统 libc++ 会把实际可运行版本钉死在构建机的 OS(老系统
缺新符号,如 `std::print` 的支撑符号),只有静态化才能真正兑现
`macos_deployment_target` 的 floor。Linux/MinGW 上它是 `-static-libstdc++`
(GCC)或整条链的 `-static`(MinGW);Linux 上的 clang/libc++ 工具链则显式链入
libc++.a/libc++abi.a/libunwind.a。更低的 macOS floor(11–13)需自建 libc++
归档(已验证可行,数据级切换,按需提供)。

`static_stdlib` 是旧拼写,仍然有效:`true` 等价于 `self-contained`,`false`
等价于 `host-coupled`。显式写了 `cxx_runtime` 时以后者为准。

**兑现不了的契约会被报出来,绝不静默降级。** 若工具链不带 `libc++.a`,或某个
契约在该平台上没有对应机制(MSVC 运行时的 `self-contained` 需要 `/MT`,mcpp
目前不发射),构建会打印实际退到了哪一档,而不是悄悄交付一个与 manifest 所述
不同的产物。

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
> 把流顶上去,你的代码不需要做任何事。详见 mcpp-community/mcpp#336。

`defines` 接受**裸**宏名(不带 `-D`),把每个条目脱糖为 `-D<x>`,同时作用于 C 和
C++ 编译通道。它覆盖包内每个 TU(含模块接口单元),因此也会进入 P1689 模块扫描
—— 这正是被宏保护的 `import` 能被解析的前提。汇编单元同样能拿到。它是普通的构建
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
gtest   = "1.15.2"              # 精确版本
mbedtls = "3.6.1"
ftxui   = "6.1.9"

# dotted selector: 先匹配 mcpplibs.<path>, 找不到再匹配同级 peer root。
# 例如 imgui.core 会按顺序尝试 mcpplibs.imgui/core, imgui/core。
[dependencies]
capi.lua = "0.0.3"
compat.gtest = "1.15.2"
imgui.core = "0.0.1"
imgui.backend.glfw_opengl3 = "0.0.1"

# 命名空间子表写法
[dependencies.mcpplibs]
cmdline   = "0.0.2"
tinyhttps = "0.2.2"
llmapi    = "0.2.5"

[dependencies.compat]
glfw = "3.4"                    # 显式 namespace, 不走 mcpplibs 优先候选

# 路径依赖(本地开发)
[dependencies]
mylib = { path = "../mylib" }

# Git 依赖 —— tag / branch / rev 三选一
[dependencies]
mylib = { git = "https://github.com/user/mylib.git", tag = "v1.0.0" }
applib = { git = "https://github.com/user/applib.git", branch = "develop" }

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
都不会悄悄把你挪到更新的分支头上。要新的分支头,得显式要:

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

每个包的身份是**命名空间 + 名字**二元组。依赖 key 的写法决定 mcpp 到哪些命名空间里找。

**裸名只在三个地方解析**,按序:

| # | 命名空间 | 示例 |
|---|---|---|
| 1 | `mcpplibs` — 默认命名空间 | `cmdline = "0.0.2"` |
| 2 | `compat` — 第三方 C/C++ 库的包装命名空间 | `gtest = "1.15.2"` → `compat.gtest` |
| 3 | 完全没有声明命名空间的上游包 | `opencv = "4.10.0"` |

**其他命名空间一律必须写全。** 不存在按短名的全索引模糊搜索:

```toml
# ✅ 正确 —— 点式选择器
[dependencies]
"chriskohlhoff.asio" = "1.38.1"

# ✅ 正确 —— 命名空间子表(同一组织有多个包时更推荐)
[dependencies.chriskohlhoff]
asio = "1.38.1"

# ❌ 错误 —— 裸名永远到不了 chriskohlhoff 命名空间
[dependencies]
asio = "1.38.1"
```

第三种写法会明确报错,并列出搜索过的命名空间;若该短名的包存在于别处,错误信息会直接给出应当改写成的那一行。

**为什么不让裸名跨所有命名空间去找?** 因为依赖解析必须可复现。全域短名搜索意味着:(a) 两个命名空间拥有同名包时,胜负由索引顺序决定;(b) **新增一个索引可能悄悄改变某个既有依赖解析到的包**。要求写出命名空间,才能让同一份 `mcpp.toml` 在每台机器上解析到相同的包。

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

旧的完全限定拼写(`name = "chriskohlhoff.asio"`)仍被接受,已发布的描述符无需改动。`mcpp xpkg parse` 会校验该规则,请在索引 CI 里跑它。需要 mcpp >= 0.0.106 与 xlings >= 0.4.69;规范全文见 `docs/spec/package-identity.md`。
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

#### mcpp 何时刷新包索引

`mcpp build` / `run` / `test` **只在依赖无法用本地索引解析时**刷新包索引,绝不会
因为"时间到了"就刷。具体地说,只有三种情况会触发:本地根本没有索引、依赖的描述符
不在其中、或 SemVer 约束在本地已知版本里无解。只要所有依赖都能在本地解析出来,
无论本地索引多旧,构建都不会发起任何网络请求。

由此带来的一个需要知道的语义:`^1.2` 这类约束是对**本地索引已知的版本**求解的。
如果上游在你上次刷新之后发布了 `1.3.0`,你需要主动去取:

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

绑定是**确定性**的:

| 图中某被需要能力的 provider 数量 | 结果 |
|---|---|
| 恰好一个 | 自动绑定(无需配置) |
| `[capabilities]` pin / `--cap` 指定了一个 | 以 pin 为准 |
| 零个 | **报错**:没有包提供 `<cap>` |
| 两个及以上且未 pin | **报错**并列出候选——绝不静默猜测 |

被绑定 provider 的链接/头文件旗标经由常规依赖机制流到消费方;capability 层是那道
*选择与校验* 步骤,把"静默选错后端 / 缺后端"变成构建期的显式报错。

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
compat.openblas = "0.3.x"
```

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

### 2.11 `[runtime]` — 主机运行时能力

```toml
[runtime]
library_dirs = ["vendor/lib"]            # 烤进产物 RUNPATH 的目录(相对包根)
dlopen_libs  = ["libGL.so.1"]            # 运行期 dlopen 的 soname(doctor 校验)
capabilities = ["opengl.glx.driver"]     # 需要的主机能力(开放命名空间)
provides     = ["opengl.glx.driver"]     # 显式声明本包兑现的能力(强 provider)

# 显式 provider 覆盖(三档旋钮的"显式"档)
[runtime."opengl.glx.driver"]
provider = "compat.glx-runtime"
```

- **provider 选择**:声明 `provides` 的包(强)优先于仅在 `capabilities` 列出
  能力的包(弱,向后兼容);`[runtime.<cap>] provider=` 显式覆盖最优先,
  指向依赖图中不存在的 provider 时给出 warning。
- 解析结果可经 `mcpp why runtime`、`mcpp self doctor` 与构建产物
  `target/<triple>/<fp>/resolution.json` 查看(默认不是魔法)。
- 能力命名约定:分层小写 `domain.sub.role`(如 `opengl.glx.driver`、
  `x11.display`)与前缀类 `abi:<name>`(如 `abi:glibc`,参与工具链 ABI 强制)。

### 2.12 `[package] platforms` — 平台声明

```toml
[package]
platforms = ["linux", "macos", "windows"]
```

声明包支持的平台(CI 矩阵提示,经 `mcpp why` 展示)。词表由 mcpp 固定
(它拥有 target/triple 体系):`linux | macos | windows`;未知值 warning,
`--strict` 下报错。

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

[dev-dependencies]
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
toolchain = "gcc@15.1.0-musl"
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
| 依赖命名空间 | `mcpp`（默认) | 平铺写法走默认 ns |

### 4.1 旧 `[language]` 兼容层

旧配置仍可读取:

```toml
[language]
standard = "c++26"
```

新项目请使用 `[package].standard`。如果两个位置都出现，`[package].standard` 是权威配置。
