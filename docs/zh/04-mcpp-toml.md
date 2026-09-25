# 04 —— mcpp.toml 工程文件指南

**读者：**编写或阅读一份 manifest 的作者。

**本章回答的那一个问题：**一份 `mcpp.toml` 可以逐字段写下什么。

**不在这里：**本文件里的这些表分属四个主题，但都不归本章拥有——依赖属于
[05](05-dependencies.md)，feature 属于
[06](06-features-and-capabilities.md)，对目标的条件化属于
[22](22-target-side.md)，工程的环境属于
[23](23-the-project-environment.md)。每个主题都在它对应的表所在处点名。

`mcpp.toml` 是 mcpp 构建工具的工程配置文件，类似于 Cargo 的 `Cargo.toml` 或
Node 的 `package.json`。把它放在工程根目录；`mcpp build` 会自动发现并读取它。

## 1. 最小化示例

mcpp 按**约定优于配置**设计——大多数字段都有合理的默认值，因此最简单的
`mcpp.toml` 只需要几行：

### 1.1 可执行程序（最简）

```toml
[package]
name    = "hello"
version = "0.1.0"
```

mcpp 自动推断：
- 源文件：`src/**/*.{cppm,cpp,cc,c,S,s,asm}`
- 入口点：`src/main.cpp` → 产出 `hello` 二进制
- 标准：C++23
- 模块：扫描 `export module ...` 声明并自动构建依赖图

### 1.2 库项目（最简）

```toml
[package]
name    = "mylib"
version = "0.1.0"

[targets.mylib]
kind = "lib"
```

库根约定：主模块接口默认是 `src/mylib.cppm`（包名的最后一段）。

## 2. 完整字段参考

### 2.1 `[package]` —— 包元数据

```toml
[package]
name        = "myapp"              # Package name (required)
version     = "0.1.0"              # Semantic version (required)
standard    = "c++23"              # C++ standard (default c++23; can be set to c++20 / c++26)
description = "My awesome app"     # Description (optional)
license     = "MIT"                # License (optional)
authors     = ["Alice", "Bob"]     # Author list (optional)
repo        = "https://github.com/user/myapp"  # Repository URL (optional)
```

`standard` 是 C++ 语言标准的一等设置。推荐取值：

- `c++23`：默认值，与当前以模块为基础的默认模板相配。
- `c++20`：mcpp 接受的最低档位——具名模块是 C++20 的特性，所以在这套构建
  模型里，低于它就没有意义可言。当外部约束（较旧的内部规定、只支持到
  C++20 的第三方 API）迫使档位下降时使用它。`import std;` 在这一档同样
  可用：它本是 C++23 的**库**特性，但 GCC（≥ 15）、Clang + libc++（≥ 17）
  与 MSVC STL（VS 2022 17.8 起）在 C++20 模式下都同样提供 `std` 模块。
  注意 C++23 的库设施（`std::print`、`std::expected` 等）在这一档不可用，
  `mcpp new` 生成的代码也不例外。
- `c++26`：用于 C++26 的语言特性。
- `c++2a` / `c++2c`：兼容别名，解析后归一化为 `c++20` / `c++26`。
- `gnu++20` / `gnu++23` / `gnu++26`：GNU 方言；这个选择会进入指纹与 std
  BMI 的缓存键。
- `c++latest`：解析为所用工具链支持的最新标准档位。适合本地实验，但不
  推荐用在要求可复现的发布包上。
- `c++fly`：`c++latest` **加上所用工具链能启用的每一项实验性标准特性**
  （语言特性加标准库）。在 GCC ≥ 16 上会打开 C++26 反射（`-freflection`）
  与 contracts；在 Clang/libc++ 上会加上 `-fexperimental-library`；工具链
  不支持的开关会被跳过并打印一份摘要。它刻意依赖具体工具链——这是最
  前沿的试验场模式，绝不用于已发布的包。

两条值得了解的性质：

- **标准是模块图全局的。** 根包的 `standard` 适用于构建中的每一个翻译
  单元，包括依赖——依赖自身的 `standard`，在它作为依赖被构建时不会被
  使用。这不是一种简化：BMI 在不同档位之间不兼容（GCC 会报
  `language dialect differs` 拒绝它），所以一张图在物理上无法同时容纳
  两个档位。
- **档位之间从不共享缓存。** 标准是指纹、`import std` BMI 身份与依赖
  构建缓存键的一部分，所以在 `c++20` 与 `c++23` 之间切换，会让每个档位
  拿到自己独立的 target 目录与自己独立的 std BMI，而不是一次损坏的命中。

若源码在所用工具链未提供 `std` 模块的档位上写了 `import std;`，mcpp 会
在编译之前失败，并同时点出工具链与工程档位两者。

字段值的两种写法都被接受：`standard = "c++26"` 与 `standard = 26`。

当**某个依赖声明的档位高于全图的档位**时，mcpp 会在编译之前说明这一点，
而不是任由构建在那个依赖的源码内部某处失败。见
[workspace §4.2](07-workspace.md)。

`[package.metadata.<tool>]`（mcpp 2026.9.16.1+）是一张引擎保留、但不解读
的表。它是这个包对自身的陈述，供读取它的工具使用——例如某个框架收集
每个库贡献了什么——并通过 `mcpp::graph_file()` 到达根包的构建程序
（[30 —— build.mcpp](30-build-mcpp.md)）。表中的路径由那个读取者相对包的
manifest 目录解析。`[package]` 中 mcpp 不读取的任何其它键都会被报告，与
`[build]` 中的处理一致：一条警告，在 `--strict` 下则是错误
（2026.9.16.1+）。

```toml
[package.metadata.demo]
resources = "res"
```

#### 方言标志与 `import std` BMI

有些标志会改变标准库头文件的声明内容，所以预编译的 `import std` BMI 也
必须用它们一起构建。这正是 `[build] dialect_cxxflags` 的用途：它会应用
于 std BMI 的预构建、模块扫描，**以及**图中的每一个翻译单元，包括依赖。

```toml
[build]
dialect_cxxflags = ["-fno-exceptions"]
```

当 mcpp 在 `cxxflags` 中发现某些标志时（`-freflection`、`-fchar8_t`、
`-D_GLIBCXX_USE_CXX11_ABI=…`），会自动把它们提升进那个通道——一张混用
了这些标志的图本来就是病态的，所以没有哪个依赖能对它们持不同意见。

`-fno-exceptions` 与 `-fno-rtti` **不会**被提升，因为依赖可以合理地持
不同意见：它们移除了依赖可能要用到的语言设施，而消费者无权替依赖做这个
选择。若留在 `cxxflags` 里，它们会到达每一个 TU 却到不了预构建，导致
构建无法成功——mcpp 会在编译之前拒绝，并点名这个键：

```
error: `-fno-exceptions` changes the language dialect, but the `import std` BMI is
       precompiled without it, so every importing translation unit will fail with
       "language dialect differs".
       Declare it as a dialect flag instead:

         [build]
         dialect_cxxflags = ["-fno-exceptions"]
```

这项检查读取的是**生效**标志，所以同一个标志写在
`[profile.<name>] cxxflags` 或某个 `[target.…]` 块里也会触发。当图里
没有任何单元 import `std` 时不会触发，此时该标志只是一个正常生效的按
单元选项。

### 2.2 `[targets.<name>]` —— 构建目标

```toml
# Executable (default; inferred automatically when src/main.cpp exists)
[targets.myapp]
kind = "bin"
main = "src/main.cpp"       # Optional, defaults to src/main.cpp

# Static library
[targets.mylib]
kind = "lib"

# Shared library
[targets.mylib]
kind = "shared"
soname = "libmylib.so.1"  # Optional: Linux/ELF ABI name; an alias of the same name is generated at runtime
```

`soname` 是共享库的 ABI 名，相当于 Autotools/CMake 里的
`SOVERSION`/`SONAME`。在 Linux 上，mcpp 会向链接器传入
`-Wl,-soname,<name>`，并在输出目录生成一个 `<name> -> lib<target>.so`
的别名，使下游程序能通过 `DT_NEEDED` 或 `dlopen()`，按其标准 ABI 名加载
这个库。这个字段只对 `kind = "shared"` 生效，取值必须是一个文件名
（不含路径）。一个未声明 `soname` 的 ELF 共享库，会把自己的输出文件名
记为自己的 SONAME（2026.9.14.2+）——这正是它的消费者已经记录在
`DT_NEEDED` 里的那个名字；bionic 从 API level 23 起要求必须有这个名字。

共享库目标在全部三种二进制格式上都能工作。ELF 得到一个带 `soname` 和
`$ORIGIN` 搜索路径的 `.so`；Mach-O 得到一个 install name 为
`@rpath/<file>` 的 `.dylib`，因此即使被移动位置也仍然有效；PE 同时得到
loader 打开的 `.dll` 与链接器消费的导入库，导出列表从对象文件按 MSVC
ABI 生成（不带 `__declspec(dllexport)` 或 `.def` 时什么都不导出）。见
`tests/e2e/08`、`257` 与 `259`。

#### `kind = "app"` —— 用户启动的那个东西（mcpp 2026.9.12.3+）

```toml
[targets.myapp]
kind = "app"
main = "src/main.cpp"
```

`app` 在每一行上命名的是同一个事实——用户启动的那个程序——而每一行
为它提供自己的文件形式：

| 行 | `app` 的形式 | 文件 |
|---|---|---|
| ELF、PE、Mach-O 各行，`wasm32-emscripten` | 与 `bin` 相同 | `myapp`、`myapp.exe`、`myapp.js` |
| `*-linux-android` | 与 `shared` 相同 | `libmyapp.so` |

在 `*-linux-android` 上，平台把一个应用当作共享库加载进一个 Java 进程
（`System.loadLibrary("myapp")`，manifest 里的 `android:name`）；这一行上
没有可执行文件形式的应用。在其余每一行上，`app` 的链接方式与 `bin`
完全相同，产出的文件与 `bin` 目标的产物逐字节相同。

`main` 在每一行上都保持同一个含义：它命名定义入口点的翻译单元。当
`app` 是一个可执行文件时，那个入口就是 `main` 本身。在 `*-linux-android`
上，这个文件被编译为共享库的一个翻译单元，平台自己的入口
（`ANativeActivity_onCreate`，或它声明的 JNI 导出）是平台自己的契约，
不是 mcpp 分配的名字。`exports`（见上）对 `app` 目标的适用方式与对
`shared` 目标完全相同，`windows_subsystem` / `windows_entry`（见下）
接受 `app` 的方式也与接受 `bin` 完全相同。

在其形式为共享库的那一行上，`mcpp run` 一个 `app` 目标，若不带
`--format` 会拒绝执行，并点名这个旗标与所解析出的图提供的那些格式。
`mcpp pack --format apk` 会把这个库放到闭包已经放置共享对象的位置。见
[10 —— 打包与发布](10-pack-and-release.md)中的 `mcpp run --format`。

早于 2026.9.12.3 的引擎不认识这个取值，会拒绝并点名它认识的三种：

```
targets.myapp.kind must be 'bin', 'lib' or 'shared'; got 'app'
```

#### `exports` —— 产物发布的符号集合（mcpp 2026.9.6.5+）

```toml
[targets.mydriver]
kind    = "shared"
soname  = "libmydriver.so.1"
exports = "abi/mydriver.exports"     # or inline: exports = ["vk_icd*"]
```

**省略这个键会发布一切，而这恰好是两个平台本来就在做的事**——ELF 给
符号默认可见性，PE 自动生成一份列出每个符号的 `.def`。`exports` 收窄
这个范围。

两类工程需要这种收窄。**带稳定 ABI 的运行时**只发布一份经过审查的
集合，其余一概不发布，让不在集合里的东西保留自由变化的空间。**与同类
插件并存加载的插件**不能相撞：一个 Vulkan ICD 是按名字被找到
`vk_icdGetInstanceProcAddr` 的，若它同时导出自己的内部符号，就会与
loader 以及进程中的其它 ICD 相撞。

这个文件每行列一个符号模式，`#` 起一行注释，`*` 是唯一的通配符。内联
数组说的是同一件事，用于只有两三个入口点、单独开一个文件显得多余的场合。

一句陈述，三种渲染：

| 平台 | 渲染为 |
|---|---|
| ELF | 一份 version script，`-Wl,--version-script=` |
| Mach-O | `-Wl,-exported_symbols_list`（前导下划线由引擎补上） |
| PE | 那份 `.def`，替换自动生成的、导出一切的那一份 |

**它不改变编译期可见性，这是刻意的。** 这种收窄在全部三种格式上都是
链接期属性，所以一个键只有一种效果。`-fvisibility=hidden` 仍可通过
`[build] cxxflags` 使用，以获取它带来的代码生成收益，它是一个独立的
决定，因为它同时改变这个库自己的翻译单元之间彼此可见的方式。

**符号版本化不是这个键管的事。** `foo@@LIB_1.0` 与 `foo@LIB_0.9` 并存，
是一种只有 ELF 才有、无法中立表达的能力；需要它的包自己写 version
script 并通过 `[build] ldflags` 传入，或者自行计算并发出
`mcpp:link-flag=`（docs/07）。

`soname` 在 `kind = "lib"` 上同样有意义——见下文的
[`dependency_linkage`](#dependency_linkage--静态还是动态由消费者决定)，
在那里，一个库采取的形式变成消费者的决定。

#### `windows_subsystem` 与 `windows_entry` —— Windows GUI 可执行文件（mcpp 2026.9.12.2+）

```toml
[targets.myapp]
kind              = "bin"
main              = "src/main.cpp"
windows_subsystem = "windows"   # "console" (default) | "windows"
windows_entry     = "main"      # "main" (default) | "wmain" | "WinMain" | "wWinMain"
```

一个 PE 可执行文件记录一个 subsystem。`"console"` 附带一个控制台，
`"windows"` 产出一个启动时没有控制台的 GUI 程序。`windows_entry` 命名
的是程序自己定义的函数，不是调用它的启动符号，它独立于 subsystem 之外：
一个控制台程序可以定义 `wmain`，一个 GUI 程序也可以保留可移植的
`int main()`。

这两个键之所以是字段而不是链接旗标，是因为正确的旗标取决于 ABI，而一个
旗标说不出它面向哪个 ABI：

| `windows_subsystem` / `windows_entry` | MSVC ABI（cl、clang-cl、目标为 `*-windows-msvc` 的 clang） | GNU ABI（MinGW gcc、目标为 `*-windows-gnu` 的 clang） |
|---|---|---|
| `"console"` / `"main"` | 无 | 无 |
| `"windows"` / `"main"` | `/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup` | `-mwindows` |
| `"windows"` / `"WinMain"` | `/SUBSYSTEM:WINDOWS /ENTRY:WinMainCRTStartup` | `-mwindows` |
| `"windows"` / `"wWinMain"` | `/SUBSYSTEM:WINDOWS /ENTRY:wWinMainCRTStartup` | `-mwindows -municode` |
| `"console"` / `"wmain"` | `/SUBSYSTEM:CONSOLE /ENTRY:wmainCRTStartup` | `-municode` |

在 MSVC ABI 上，只要两个键中任意一个偏离其默认值，两个旗标就都会被
写出，因为链接器在其中一个缺席时会从另一个推断：单独的 GUI subsystem
会选中 `WinMainCRTStartup`，而一个可移植的 `int main()` 满足不了它；
`/ENTRY:main` 会跳过 CRT 初始化，包括静态构造函数。GNU 风格的驱动程序
以 `-Wl,/SUBSYSTEM:...` 的形式接收 MSVC-ABI 旗标。

这两个键只到达声明它们的目标的链接，不会传播出去。第二个可执行文件、
`mcpp test` 的二进制，以及这个包的消费者，都保持控制台 subsystem——这
正是为什么这些旗标不应放进 `[build] ldflags`：那条通道会到达图中的每
一次链接。在 ELF、Mach-O 与 WebAssembly 上，这两个键不渲染出任何东西，
产物与未写它们时逐字节相同，所以一份跨平台 manifest 不需要 `cfg` 块。
一个库目标若声明这两个键中的任意一个都会被拒绝，拒绝信息点名该目标与
该键。

一个构建程序可以为自己包里的某个可执行文件设置同样的字段，用
`mcpp::windows_subsystem("<target>", "windows")` 与
`mcpp::windows_entry("<target>", "wmain")`（[build.mcpp](30-build-mcpp.md)）。

应用程序包（bundle）、应用程序 manifest 与 DPI 感知不属于这两个键管的
范围；它们属于打包格式与 `[resources]`。

#### 按目标的键（per-target keys）

```toml
[targets.server]
kind     = "bin"
main     = "src/server.cpp"
defines  = ["BUILD_SERVER=1", "PORT=8080"]   # -D macros, applied to this target's entry only
cxxflags = ["-Wno-deprecated-declarations"]  # extra C++ flags for this target's entry (no -std=...)
cflags   = ["-DPURE_C"]                       # extra C flags for this target's entry

[targets.gui]
kind = "bin"
main = "src/gui.cpp"
required_features = ["gui"]                   # only built when feature `gui` is active
```

| 键 | 含义 |
|---|---|
| `defines` | 预处理宏（`name` 或 `name=value`）；在 C 与 C++ 两种入口编译上都脱糖为 `-D<x>`。 |
| `cxxflags` / `cflags` | 这个目标专用的额外编译旗标。**不要**把 `-std=...` 写在这里——用 `[package].standard`。 |
| `required_features` | 只有当构建中**每一个**列出的 feature 都被激活时，这个目标才会被产出；否则被静默跳过。它只是一道闸——不会激活 feature（用 `--features` / `[features].default`）。**一个例外，但它不是第二条规则：** 当这个目标作为 host 工具被请求时（`tools = [...]`，§2.14），这个目标就是被**请求**的那一个，于是它的 `required_features` 变成子构建的**输入**。同一个字段、同一个含义——只是解析的方向反过来了。 |
| `windows_subsystem` *（2026.9.12.2+）* | 可执行文件的 PE subsystem：`"console"`（默认）或 `"windows"`，一个启动时没有控制台的 GUI 程序。只到达这个目标的链接，别处不受影响，在非 PE 的目标上不渲染任何东西。见上一节。 |
| `windows_entry` *（2026.9.12.2+）* | 程序定义的入口函数：`"main"`（默认）、`"wmain"`、`"WinMain"` 或 `"wWinMain"`。见上一节。 |
| `windows_code_page` *（2026.9.26.1+）* | 可执行文件在 Windows 上的 ANSI 代码页：`"utf-8"`，以 Windows 10 1903 及以后版本会遵从的应用程序清单嵌入；或 `"legacy"`，即系统代码页。两者都不写的程序目标运行在系统代码页里，作为 host 工具构建的程序（§2.14）除外，它默认为 `"utf-8"`。写在库目标上会被拒绝；在非 PE 目标上不产生任何东西。见 §2.3 的*路径与文本编码*。 |
| `linkage` *（2026.9.15.2+）* | 一个库目标的**默认**链接形态，`"static"` 或 `"shared"`：不写 `linkage` 的消费者得到的形态。与 `kind = "shared"` 不同，它不是约束，因此消费者的显式陈述会被遵从。与 `kind = "shared"` 同写，或写在程序目标上，都会被拒绝。见[`dependency_linkage`](#dependency_linkage--静态还是动态由消费者决定)。 |

> **范围（重要）：** 目标上的 `defines` / `cxxflags` / `cflags` **只**
> 应用于该目标专属的入口源文件（它的 `main`）——绝不应用于共享的
> 模块/实现对象，后者只编译一次，并链接进每一个目标（mcpp 的一次编译
> 模型）。当一个旗标只需要影响单个二进制（或测试）自己的入口时，这两个
> 键是正确的工具——例如某个测试的 `main` 专门触发违规，需要按测试设置
> contract 求值语义（`-fcontract-evaluation-semantic=observe`）；或者
> 只有入口会读到的一个 feature 宏；或者一次局部的警告抑制。如果一个
> 旗标必须到达**共享**代码，就不属于这里——要么拆成一个
> [workspace](07-workspace.md) 成员，要么用 `[features]`；若是整个构建
> 范围的模式，用 `[profile.*]`（`mcpp test --profile <name>` 会在那个
> profile 下构建整份测试镜像，包括被测代码）。
>
> `[targets.<name>]` 下不受支持的键会被报告为警告（`--strict` 下为
> 错误）。

**构建配置该放在哪里**——当不止一个二进制必须有所不同时：

| 目的 | 做法 |
|---|---|
| 二进制**自己入口**上不同的宏/旗标 | 按目标的 `defines` / `cxxflags`（见上） |
| 两个产物在**共享**代码上有差异 | 拆成 [workspace](07-workspace.md) 成员，各自在共享的 `lib` 之上有自己的 `[build]` 旗标 |
| **选择一个变体**的共享库（例如某个后端） | 用该库上的 `[features]`（§2.8）——是增量的，到达库自己的编译 |
| **整个构建范围**的模式（sanitizer、contract 语义、优化档位） | `[profile.<name>]`（§2.9） + `--profile`；`mcpp test --profile <name>` 同样尊重它 |

mcpp 刻意不在一次构建里用两种方式编译同一份共享源码：一个源文件对应
一个对象（模块对应一个 BMI），所以凡是必须到达共享代码的分歧，都属于
包/feature 的边界，不属于单个目标。

### 2.3 `[build]` —— 构建配置

> **每一条 `sources` 匹配到的文件都必须产出一个会被链接的对象。** 一个
> mcpp 无法归类的文件——扩展名既不在内置集合里，也不在
> `module_extensions` 里——会被拒绝，拒绝信息点名文件、扩展名与这个键。
> 它不会被忽略，因为催生这条规则的失败不是「多出一个文件」，而是**被
> 编译了却没有人链接它**：扫描器读到 `export module` 并给这条边一个
> BMI，而分类器却说这个文件没有角色，作者看到的是对一个模块修饰符号的
> `undefined reference`。头文件属于 `include_dirs`；Windows 资源脚本
> 属于 `[resources]`。

> **`sources = []` 与省略 `sources` 不是一回事。** 缺失这个键会选中
> 默认 glob；显式的空列表意味着**什么都不编译**，这正是一个纯头文件
> 分发包需要表达的意思。在 mcpp 2026.8.18.1 之前两者逐字节相同，所以
> 没有拼法能表达「什么都没有」，`src/` 下留下的任何文件都会被卷进来。

> **一条 `sources` 条目可以携带它所面向的加速器**（2026.9.5.2+）：
> `{ glob = "src/kernels/**/*.cu", accel = "cuda12.9+{sm_89}" }`。glob
> 像其它条目一样加入这份列表；约束决定它是否适用于某次给定的构建。它
> 必须至少匹配一个文件（空匹配会被拒绝：那会让这个设备没有东西可编译，
> 却要等到链接阶段才说出来）。在 `--no-accel` 下这条 glob 会被排除，
> 这正是一个工程产出自己 CPU-only 变体的方式。在一个不覆盖该约束的
> `--accel` 下，构建会被拒绝并点名两者（`accel-mismatch`）。生效集合
> 匹配到的设备类文件——CUDA 与 HIP、GLSL 各阶段、HLSL、OpenCL C 与
> Metal，完整列表见[42 —— 异构构建](42-heterogeneous-builds.md)——从不
> 由引擎编译；它们以 `MCPP_DEVICE_SOURCES` 的形式到达构建程序，工程
> import 的规则包把每一个都变成一个 `mcpp::action`。

```toml
[build]
sources      = ["src/**/*.cppm", "src/**/*.cpp"]  # Source globs (default: src/**/*.{cppm,cpp,cc,c,S,s,asm})
module_extensions = [".ixx"]      # Extra extensions used by module INTERFACES (§ below)
build_program_timeout = 1800      # Seconds a build.mcpp may run; 0 = no limit (§ below)
include_dirs = ["include", "third_party/include"]  # 本包的头文件搜索路径（见下文）
include_dirs_after = ["*"]         # Header dirs searched AFTER system dirs (-idirafter)
private_include_dirs = ["vendor/src/include"]  # Of `include_dirs`, the ones a consumer must NOT get
c_standard   = "c11"              # Standard for this package's C sources (default c11; § below)
cflags       = ["-DFOO=1"]        # Extra C compile flags
cxxflags     = ["-DBAR=2"]        # Extra C++ compile flags (do not put -std=... here)
ldflags      = ["-lfoo"]          # Extra link flags
defines      = ["BIZ=1", "QUX"]   # 本包每个编译单元的预处理宏；按宏名构成集合（见下文）
cxx_runtime  = "self-contained"   # C++ runtime contract (§ below); static_stdlib is the old spelling
target       = "x86_64-linux-musl" # Default build target when no --target is passed
                                   # (≙ cargo build.target; e.g. "ship fully-static")
macos_deployment_target = "14.0"   # Minimum supported OS version for macOS artifacts (macOS only)
dependency_linkage = "static"     # How dependencies arrive: static (default) | shared (§ below)
cache        = "global"           # Global dependency cache: global (default) | local | off (§2.10)
jobs         = "auto"             # Concurrent compiles: a positive number, or "auto" (§ below)
bmi_schedule = "auto"             # Module-edge scheduling: auto (= off) | on | off (§ below)
```

#### 编译 flag 的写法 *(mcpp 2026.9.17.1+)*

`cflags`、`cxxflags` 或 `asmflags` 里的一个元素代表一个或多个编译器
参数（「词」）。这套语法在每个宿主上都相同，无论写在哪张表里：
`[build]`、`[targets.<name>]`、`flags` glob 条目、feature、
`[target.<selector>.build]` 小节、xpkg 描述符，以及构建程序的
`mcpp:cflag=` / `mcpp:cxxflag=` 指令。

| 写法 | 编译器收到的词 |
|---|---|
| `"-O2 -g"` | `-O2`、`-g` |
| `"-include config.h"` | `-include`、`config.h` |
| `"'-DNAME=a b'"` 或 `"-I\"my dir\""` | `-DNAME=a b`、`-Imy dir` |
| `"-DNAME=long long"` | `-DNAME=long long`（见下） |
| `"-DNAME=\\\"text\\\""` | `-DNAME="text"`（一个字符串字面量） |
| `"-I/opt/my\\ dir/include"` | `-I/opt/my dir/include` |
| `"-IC:\\sdk\\include"` | `-IC:\sdk\include` |
| `"-DNAME=a$b"` | `-DNAME=a$b` |

以下规则施加于元素的文本上（TOML 或 Lua 已经去掉了它自己的转义之后）：

- 不带引号的空格与制表符分隔词；
- `'...'` 是字面量，直到下一个 `'`；
- `"..."` 是字面量，只有 `\"` 与 `\\` 分别代表 `"` 与 `\`；
- 在引号之外，反斜杠之后紧跟空格、制表符、`"`、`'` 或 `\`，代表那个
  字符本身；其它反斜杠都是字面量；
- 相邻接触的带引号与不带引号的片段合并成一个词；
- `$`、`*`、`;`、`|` 以及其它 shell 操作符没有特殊含义；
- 一个以 `-D` 或 `/D` 开头并含有空格的元素，视为一个词，按字面接受，
  与以往每一个版本相同。

一条 `defines` 条目是一个值，不受这套语法解析：`defines =
["NAME=\"text\""]` 传出单独一个词 `-DNAME="text"`。`ldflags`、
`dialect_cxxflags` 与 `std-module-flags` 不受本节约束。

`compile_commands.json` 与 `mcpp emit build-database` 在 `arguments`
里列出同样的词，可以不经 shell 直接执行。

在一个工程的首次 plan 时，若某个元素的词与 2026.9.17.1 之前的版本在
同一宿主上传出的参数不同，mcpp 会在 `build/flag-words` 下发出警告，并
点名两者。重复同一份 plan 的构建不会重复这条警告。

#### `defines` 与头文件目录的作用范围 *(mcpp 2026.9.25.1+)*

`defines` 是按宏名构成的集合。条目按包接收它们的顺序读取：`[workspace.build]`
（对工作空间成员而言）、包自己的 `[build]`，然后是每个命中的
`[target.<selector>.build]`。同一宏名的后一个条目替换前一个，条目 `!NAME` 移除该
宏名。每个宏名以一个 `-DNAME` 词到达编译器，C 与 C++ 编译单元相同：

```toml
[build]
defines = ["LEVEL=1", "TRACE"]

[target.'cfg(os = "windows")'.build]
defines = ["LEVEL=2", "!TRACE"]       # Windows 上：-DLEVEL=2，且不定义 TRACE
```

`defines` 条目同样替换本包在 `cflags` 或 `cxxflags` 中写下的同名 `-DNAME` 词。
更早的 mcpp 会把 `!NAME` 作为 `-D!NAME` 交给编译器，那是一个错误，因此使用它的包
需要 mcpp 2026.9.25.1 或更新版本。

`include_dirs` 与 `include_dirs_after` 由本包自己的编译单元搜索。本包的消费者也会
收到它们，`private_include_dirs` 中列出的条目除外。本包的依赖永远收不到它们：无论
由哪个工程构建，依赖都只对照自己的头文件目录以及它自己的依赖所公开的目录编译。
依赖若包含某个头文件，必须通过自己的 `include_dirs` 或它的某个依赖找到它。当依赖的
编译报告缺少某个头文件、而该文件存在于消费者的头文件目录中时，mcpp 会在编译器的
报错之后指出那个目录。

#### `c_standard` 作用于声明它的包 *(mcpp 2026.9.26.1+)*

`c_standard` 设定声明它的那个包自己的 C 编译单元所用的 C 标准。没有声明的包
以 `c11` 编译其 C 单元，消费者的值永远不会到达依赖：声明了 `gnu11` 的依赖在
一个声明 `c99` 的工程里仍以 `gnu11` 编译，什么都没声明的依赖在那里以 `c11`
编译。C++ 单元不接收 C 标准。这个值是包构建键的一部分，所以依赖的缓存对象
服务于每一个消费者。

`cl.exe` 以其默认模式编译 C，不从 mcpp 接收 C 标准。当构建中有包声明了 C 标准
时，构建会用一行汇报每个声明未被施加的包。

在 mcpp 2026.9.26.1 之前，根包的值会到达图中每一个 C 单元，而依赖自己的声明
被读取、被哈希进它的缓存键，却没有被施加（mcpp#695）。

#### `dependency_linkage` —— 静态还是动态由消费者决定

```toml
[build]
dependency_linkage = "shared"        # whole-graph default; "static" is the default default

[profile.dev]
dependency_linkage = "shared"        # per profile

[dependencies]
"compat.zlib" = { version = "1.3.2", linkage = "shared" }   # one package
```

在 mcpp 2026.8.28.2 之前，一个依赖恰好只有一种形态，并且由**包作者**
选择：`kind = "lib"` 把它的对象合并进每个消费者的链接，`kind = "shared"`
产出一个真正的共享库。这个决定的归属者选错了。一个库在运行时是否应该
是一个独立文件，是**正在被构建的那个程序**的属性——它怎样被发布、多久
被重新链接一次、进程里是否已经有别的东西提供了这个库。

- **`static`**（默认）——依赖的对象被合并进使用它的镜像。逐字节等同于
  mcpp 一直以来的做法；不写这个键的工程，构建方式与以前完全一样。
- **`shared`**——mcpp 把这个依赖构建成产物旁边的一个共享库并链接到它，
  通过 `$ORIGIN`（ELF）/ `@loader_path`（Mach-O）/ 可执行文件自己所在
  的目录（PE）在构建目录被移动之后仍能重新找到它。

**这不是 `[target.<triple>].linkage`**（§2.7.1）。那个键回答的是一个
听起来相似、但关于 **C 库**的问题（musl 的 `-static` 链接、MSVC
的 `/MT`）。这两者不是独立的，而且方向很重要：一个完全静态的镜像没有
解释器，因此根本无法加载共享对象。在一个 C 库以静态方式链接的目标
上——这是**musl 的默认行为**——`dependency_linkage = "shared"` 会被
拒绝，并说明原因。

**一个包可以声明它必须是某一种形态**，但只能出于真实的理由：

| 包写的内容 | mcpp 的解读 |
|---|---|
| `[targets.<n>] kind = "shared"` | **必须**是共享的——进程里另有别的东西会 `dlopen` 它，所以只能有一份拷贝（X11、Vulkan loader） |
| `[target.<sel>.targets.<n>] kind = "shared"` *（2026.9.14.2+）* | 在选择器匹配的那些行上**必须**是共享的，在别处两种形态都可以（[22 —— 目标侧](22-target-side.md)） |
| `ldflags` 中含 `-L` | **必须**是静态的——包发布了 mcpp 没有编译、也无法放进自己构建的共享对象里的预构建归档文件 |
| 一个已打包的库（`mcpp pack`） | 它实际发布的那几条腿，来自 `[[runtime.artifacts]] role` |
| 其它任何情况 | 两种形态都可以 |

`kind = "lib"` **不是**一种约束：它是默认值，大多数包写它时并没有在做
选择。没有陈述不等于一种陈述。

**一个包也可以声明一个默认值**（2026.9.15.2+），这不是约束：

```toml
[targets.fw]
kind    = "lib"
linkage = "shared"                   # the form a silent consumer receives

[target.'cfg(env = "android")'.targets.fw]
linkage = "shared"                   # the same, on the rows the selector matches
```

- 形态的决定按以下顺序、以最具体的陈述优先：根包在这条依赖边上写的
  `linkage`，根包写出的 `dependency_linkage`（在 `[build]` 或当前生效
  的 profile 里），包自己的 `linkage`，以及 `static`。
- 一处与包默认值不同的显式陈述会被遵从。这不算一种降级，所以
  `--strict` 接受它，并有一行信息（`Linkage`）点名两处陈述。
- 在同一张表里同时写 `kind = "shared"` 与 `linkage` 会被拒绝，因为一个
  约束不会留下默认值可陈述；一行只陈述 `kind` 与 `linkage` 二者之一，
  后一条匹配的陈述会替换前一条，所以一行上的 `linkage = "static"`，
  能把某张无条件的表约束为 `shared` 的包，交还给它的消费者去选择形态。
- 一个目标无法满足的默认值（一个完全静态的镜像、一个 freestanding
  目标）会无声地回落，因为根本没有人要求过它。
- 早于 2026.9.15.2 的引擎会把 `[targets.<n>] linkage` 报告为不受支持
  的键（对依赖是静默的），并把包按静态链接；从 2026.9.14.2 起的引擎会
  拒绝一张没有 `kind` 的行表。依赖这个键的包应声明这个引擎下限。

根工程的构建程序可以通过 `mcpp::dep_linkage("name")`
（[30 —— build.mcpp](30-build-mcpp.md)）读到每个依赖在这次构建中采取的
形态，使生成的 loader 条目或 import 声明遵循同一个决定。

被约束拒绝的一个请求，会按包允许的形态链接，并给出一条点名包的陈述的
警告（`its manifest states [targets.fw] kind = "shared", ...`）；
`--strict` 把这条警告变成错误。`mcpp why deps` 报告每个依赖的形态及其
原因：`default`、`package-default`（2026.9.15.2+）、`requested`、
`package-kind`、`row-kind`、`packaged`、`no-sources`、
`prebuilt-inputs`、`no-loader` 或 `static-libc`（2026.9.14.2+）。

按依赖设置的 `linkage`，**只**在根工程自己的 `[dependencies]` 里被
遵从。图深处的某个包无权决定最终程序如何布局；真正必须是单一共享拷贝
的包，应该在自己的目标上这样声明。

#### 共享库之下的静态包 *(mcpp 2026.9.16.1+)*

一个共享库与它所触达的静态包一起被链接。一次构建里，每一份共享镜像都
有一个**静态闭包**：从它的包出发、不跨越另一个共享包所能到达的所有
静态包。一个只处在**一个**闭包里、根工程自己又碰不到的静态包，会被
链接进那份镜像，而不是进程序本身。

在 2026.9.16.1 之前，这样的包会被链接进程序，库在运行时绑定的是程序里
的那份拷贝。这只在 ELF 上有效，而且只对那一个程序有效：这个库会拒绝
`-Wl,-z,defs`，一个没有链接该包的宿主无法加载它（`undefined symbol`），
Mach-O 与 PE 在链接期就解析每一处引用，而 Android 会在任何能提供该包
的东西之前先加载应用的共享库。

一个被**多份**镜像触达的静态包（两个共享库，或一个共享库加程序）没有
单一的镜像可以栖身：

- 在 Mach-O、PE 与 Android 应用这一行上，构建会在编译之前被拒绝，理由
  是 `static-package-in-two-images`（[50](50-machine-output.md)）；
- 在其它 ELF 行上，包仍像以前一样留在程序里，构建会报告这一点
  （`build/static-placement`），`--strict` 把它变成错误。

这条消息点名这个包、触达它的那些镜像，以及补救办法：给这个包共享的
形态，让每份镜像各加载一份拷贝。

```toml
[dependencies]
x = { path = "../x", linkage = "shared" }   # on the root's edge

# or as the package's own default, in its manifest
[targets.x]
linkage = "shared"
```

一个提供目标层（`provides = ["mcpp:..."]`、一个 C 库或一个 C++ 运行时）
的包不受这条规则约束：它的对象去往何处，由运行时契约决定
（[20](20-toolchains.md)）。

#### library 目标上的 `soname`

`soname`（§2.2）也可以声明在 `kind = "lib"` 上，不只是
`kind = "shared"`。它是一个库被**查找**时使用的名字，也是 mcpp 构建出
的某个包与第三方的同一份库能解析到**同一个文件**而不是两个文件的
唯一途径——如果声明它就意味着这个包不能再作为静态库被消费，包就没法
陈述这一点。

一份在非共享目标上写了 `soname` 的描述符，无法被 2026.8.28.2 之前的
mcpp 发布版读取——整份 manifest 都会加载失败，而不只是这个键。因此把
这样一份描述符发布到索引，要等到那个引擎下限被移动之后。

#### 符号提供者检查

链接之后，mcpp 会检查镜像里的每一个符号是否恰好只有**一个**提供者。
在 ELF 上，可执行文件先被搜索，所以一个静态合并进程序的库，会在它与
旁边加载的共享库共有的每一个符号上获胜——那份共享拷贝从未被真正
调用，那个库内部的代码运行时面对的是一份它没有链接过的构建。对此，
链接器与加载器都没有任何诊断。

这项检查是一次测量，不是一条声明：它读取产出镜像的动态符号表，去掉
属于拷贝重定位的条目，只报告产物自身闭包里**另有**某个库同样定义的
那些符号。一份进程中只有一处拷贝的安排是无声的。三类共享定义会被计入
但不报告（最后两类为 2026.9.16.1+）：vague linkage，加载器按设计统一
处理这类符号（`STB_WEAK`，以及 GCC 用于内联实体静态数据的
`STB_GNU_UNIQUE`）；构建从**一个对象**同时链接进两份镜像的定义，例如
每一个 import `std` 的 C++ 镜像里 `std` 模块的初始化器；以及那同一个
初始化器相对工具链自身 C++ 运行时的情形，GCC 16 起会导出它。任何其它
地方定义的同名同形符号，仍算一处发现。判定结果被记录在
`target/<triple>/<fp>/resolution.json` 的 `runtime.symbol_provision`
下，带计数与分母，CI 无需 `readelf` 即可读取。

它默认是警告，在 `--strict` 下是错误。解决办法按顺序排列，顺序很重要：

1. **让其中一方停止提供它**——通常是某个包自带了图里已经在构建的某个
   库的拷贝。总是正确的做法。
2. **让两者解析到同一个文件**，做法是在那个库的目标上声明它真实的
   `soname`。
3. **`dependency_linkage`** 改变 mcpp 构建的是哪种形态。它会移除
   **这一条**发现，但单独使用它可能留下**两份**已加载的拷贝而不是
   一份：在一张同时摆放 glib（其 `libgio` 需要 `libz.so.1`）与一个
   静态构建的 `compat.zlib` 的图上实测，切换形态后可执行文件的 88 个
   导出符号消失，随后同时加载了 `libzlib.so` 与 `libz.so.1`。只有当
   第（2）条也成立时，它才会把两个提供者统一成一个。

`private_include_dirs` 命名的是 `include_dirs` 里那些止步于本包自身
边界的条目：本包用它们编译，但消费者永远不会得到它们。

几乎每个包发布的都恰好是它自己构建所用的那一整套，这正是为什么很长
一段时间里单靠 `include_dirs` 就够用。二者出现分歧的情形，是一个包
内嵌了带**内部头文件覆盖层**的库。musl 通过 `src/include` 到达自己的
声明，那里的头文件定义了 `hidden`、`weak` 与 `weak_alias`——这些名字
只对 musl 自己的源码有意义。发布那个目录会把这些宏交给每一个消费者，
而一个把 `hidden` 当作普通标识符使用的消费者，会因为一个它无从看见的
原因而无法编译。

```toml
[build]
# The relative ORDER of the two kinds is load-bearing: the internal overlay
# must precede the public headers for this package's own build. That is why
# this is a SUBSET of `include_dirs` rather than a second list — two arrays
# cannot express one order.
include_dirs         = ["port/include", "musl/src/include", "musl/include"]
private_include_dirs = ["musl/src/include"]
```

条目遵循与 `include_dirs` 相同的 `*` glob 约定，并且是在展开**之后**
匹配的——所以一条 glob 可以正好命中它展开出的那些目录。一个不在这个
包的 `include_dirs` 里的条目不会隐藏任何东西，并且会被如实报告，而
不是悄悄放过。

**在较旧的引擎上，这个键会被忽略，而不会致命。** 在 2026.8.26.2 上
实测：在一个依赖的 manifest 里，它被静默接受；在一份根 manifest 里，
它会警告——
`[build] has unsupported key 'private_include_dirs' (ignored)`——
而构建继续进行。所以一个包可以先采用这个键，不必等待
它的消费者升级；那些还在旧引擎上的消费者，只会继续像以前一样拿到那个
目录。唯一的例外，是一份已发布的 `xim` 描述符的 `target_cfg` 块——
那里，一个无法识别的子键是硬错误，会让整份 manifest 加载失败——在
索引下限点名一个认识这个键的引擎之前，不要把这个键放进那里。

`include_dirs_after`（#249）列出在工具链的系统目录**之后**才被搜索的
头文件目录（在 GCC/Clang 上渲染为 `-idirafter`，在 MSVC 方言下渲染为
追加在末尾的 `/I`，在 NASM 汇编单元上渲染为普通的 `-I`——后两者都没有
对应机制，也都没有需要保护的系统头文件链）。当某个目录是一个解出来的
源码压缩包根目录、其中的文件名与标准头文件相撞时，用它代替
`include_dirs`——例如，在大小写不敏感的 macOS 文件系统上，如果把
ffmpeg 的压缩包根目录放上 `-I`，它顶层的 `VERSION` 文件会遮蔽 libc++
的 `<version>`。用 `include_dirs_after`，系统头文件总是获胜，同时这个
包真正的头文件（`<libavutil/frame.h>`）仍然可以被找到。条目支持与
`include_dirs` 相同的 `*` glob 约定，并沿着相同的边向依赖它的包
传播——消费者收到的是「之后」目录，永远不会被升级为 `-I`。

`macos_deployment_target` 设定产物的 Mach-O 头（`LC_BUILD_VERSION
minos`）里记录的最低系统版本，也就是这个二进制能运行的最旧 macOS
版本。优先级遵循生态惯例：`MACOSX_DEPLOYMENT_TARGET` 环境变量（一次
调用的显式覆盖，cargo/rustc、cc 等同样这样处理）> 这个字段（工程默认
值，类似 SwiftPM 的 `platforms:`）> **内置默认值 `14.0`**（rustc
风格——每个目标都有一个基线，而 14.0 正是 LLVM 官方静态库自身的下限）。
这个值进入 BMI 指纹，所以切换目标会自动重建模块缓存。解析与应用都
按 TARGET 判定，不按运行 mcpp 的那台机器判定：`mcpp build --target
aarch64-macos` 在 Linux 或 Windows 上和在 Mac 上一样遵从这个字段
（以及环境变量），非 macOS 目标则永远不会看到它。

### 构建并发（`jobs`）与模块调度（`bmi_schedule`）

```toml
[build]
jobs         = "auto"    # or a positive number; --jobs / MCPP_JOBS override it
bmi_schedule = "off"     # auto (default, = off) | on | off
```

`jobs` 是同时运行多少个编译。`"auto"` 是**相对正在执行构建的这台机器**
解析的，绝不会被冻结进 manifest：它取一颗异构 CPU 的物理核心数（一颗
13900K 是 8 个 P-core + 16 个 E-core，所以它的 32 个线程不是 32 个
等价的工作者），并按空闲内存夹紧这个数字，因为单次模块接口编译峰值
占用 0.5–1.0 GB。一个畸形的取值会**被报告，绝不会被静默当作默认值
处理**——一个悄悄恢复默认值的拼写错误，会让构建比要求的更慢，却没有
任何迹象说明原因。

优先级如下，每一层描述的是不同的东西：

| 层级 | 作用域 |
|---|---|
| `--jobs` / `MCPP_JOBS` | 本次调用 |
| `[build] jobs`（这个键） | 本工程 |
| `~/.mcpp/config.toml` 里的 `[build] default_jobs` | **本机** |
| 缺失，或 `0` | 什么都不说，交给后端自己的默认值 |

三者之中，只有按机器设置的那个键能承载一个机器事实。`--jobs` 必须在
每次调用时重复传入；这个键是按包的，`[workspace.build]` 不会继承它，
所以一个七个成员的 workspace 会把这个数字重复七遍，并把某个开发者
自己的内存上限提交进仓库。`default_jobs = 0` 是 mcpp 写进一份全新
配置文件时的取值，含义是「未设置」。

`default_jobs` **也约束 `mcpp test` 的并发度**，它缺失时的回落值是
整台机器，而不是某个后端的默认值。一次十个并发进程的测试运行，与一次
十个并发的编译，内存形状相同，所以一个按机器设置的数字对两者都适用。
之所以写出这一点，是因为一个键有两种行为，就必须写清楚。

`bmi_schedule` 决定 importer 何时被解除阻塞。

| 取值 | |
|---|---|
| `"auto"` | **默认值，目前意味着关闭** |
| `"on"` | 拆分这条模块边：importer 在 BMI 发布时就开始，而不是等编译器退出 |
| `"off"` | 一个模块对应一条边 |

只接受这三种拼法。`"ON"`、`"true"` 与 `"yes"` 会**带诊断信息被
拒绝**，而不是被悄悄当作关闭——它们也不是无害的笔误：这个值会进入
构建指纹，所以一个被拒绝的拼法，过去会选中一个不同的构建目录（一次
完整重建），而调度本身却什么都没变。

**为什么 `auto` 是关闭的。** 一次模块接口编译里，86% 的时间花在没有
任何 importer 会读取的代码生成上，所以提前发布 BMI 很值——在 mcpp
自身上实测，`cold` 从 86.7s 降到 35.7s，`edit-body` 从 80.9s 降到
29.8s。但一次调度上的错误是**无声地**错的：一条被漏掉的依赖不会让
构建失败，只会让某样东西不再被重建。它保持默认关闭，直到在每个平台的
CI 上都跑通过。

**它帮不上忙的地方。** 在 mcpp 已经跳过级联的地方——`touch-hub`、
`edit-comment`——没有欠下的工作需要移出关键路径，这个键什么都买不到。
见[基准测试](../../README.md#benchmark)。

**机制**因编译器而异，并自动选择：gcc 用 `rename()` 发布它的 BMI，
所以代码生成与之分离，这条边在发布时就返回；clang 得到的是两条普通的
边，因为它以 `O_TRUNC` 把 BMI 写到最终路径，读者可能会看到一个写了
一半的文件。MSVC 被留在一边不动——`/ifcOnly` 的开销与 `.ifc` 的
原子性都未经测量，两者任何一个猜错都是无声的。

### 模块接口扩展名（`module_extensions`）

mcpp 把 `.cppm` 当作一个模块接口单元。C++ 生态还没有在这件事上收敛到
一种拼法——Clang 还认得 `.ccm` 与 `.cxxm`，MSVC 用 `.ixx`——所以一个
接口用了别的扩展名的工程需要声明它：

```toml
[build]
module_extensions = [".ixx", ".ccm"]
```

这份列表是**只增**的：`.cppm` 永远是模块接口，无法被移除。要阻止某个
具体文件被构建，在 `sources` 里用 `!` 排除它；这正是 `sources` 的用途。

声明一个扩展名会同时做三件事，这也是设一个键而不是设几个键的意义
所在：

1. `sources` 的约定默认值随之扩大，使这些文件能被**找到**
   （`src/**/*.ixx` 加入默认 glob）；
2. 这些单元按**模块**规则编译——它们发出 BMI，它们的对象无条件被
   链接；
3. 新鲜度快路径会盯住它们，所以给某个文件加一条 `import` 会让构建图
   失效，而不是静默复用一份陈旧的图。

除了那些已经命名了某种非模块角色的扩展名（`.cpp` `.cc` `.cxx` `.c`
`.m` `.mm` `.h` `.hpp` `.hh` `.hxx` `.S` `.s` `.asm`）之外，任何扩展名
都会被接受；声明其中之一是 manifest 错误而不是警告，因为那会把（比如
说）C 文件路由进 C++ 模块规则，并在一个既不点名文件也不点名这个键的
地方失败。

扩展名是**按字面匹配、不做大小写折叠**的——`.S` 与 `.s` 在这个领域
是两种不同的语言，大小写永远不会被忽略。

mcpp 总是明确告诉编译器某个模块接口单元就是模块接口单元（Clang 上是
`-x c++-module`，GCC 上是 `-x c++`，MSVC 上是 `/interface /TP`），所以
编译器驱动程序从未听说过的扩展名照样能工作。这正是任何扩展名都被
允许的原因：mcpp 不需要编译器认识它。

> **发布注意事项。** 较旧的 mcpp 不认识这个键：它会警告、忽略它，
> 然后把那些文件当作普通翻译单元编译——这是一次错误的构建，而不是
> 一次干净的失败。一个使用了 `module_extensions` 的已发布包，应在其
> 索引描述符里声明一个 mcpp 版本下限。

### 构建程序超时（`build_program_timeout`）

一个 `build.mcpp` 默认获得 **600 秒**，超时后 mcpp 会杀掉它并使构建
失败，同时点名这个包。一个构建程序确实需要更长时间运行的工程（比如
一个大型的代码生成步骤），可以抬高自己的上限：

```toml
[build]
build_program_timeout = 1800   # seconds; 0 = no limit
```

这个值取自**拥有这个 `build.mcpp` 的那个包自己的 manifest**——一个
依赖的生成器，由依赖自己的声明约束，因为知道它要跑多久的是它的作者。
优先级与 `macos_deployment_target` 遵循相同的形状：

```
MCPP_BUILD_PROGRAM_TIMEOUT=<seconds>   (this invocation; highest)
  > [build] build_program_timeout      (that package's manifest)
  > 600                                (built-in default)
```

省略这个键与把它设为 `0` 不是一回事：不设置意味着「用默认上限」，`0`
意味着「完全没有上限」。

这个值刻意**不是**构建指纹的一部分——它不改变图里的任何一条边，把它
折进指纹会意味着抬高超时会重建整个工程，这与抬高超时的人想要的正好
相反。

**编译**阶段不受这个上限约束，只有构建**程序**受约束。这种不对称是
刻意为之，原因见 [30-build-mcpp.md](30-build-mcpp.md)。

### C++ 运行时契约（`cxx_runtime`）

已移至 [20 —— 工具链管理](20-toolchains.md)。

### 路径与文本编码 *(mcpp 2026.9.26.1+)*

mcpp 在所有平台上都以 UTF-8 文本持有每一个路径、参数与文件内容。它为其他
工具写出的每个文件都是 UTF-8：`build.ninja`、`compile_commands.json`，以及
编译边与链接边的响应文件。一个路径只能以它的 UTF-8 拼法进入这些文本。

**Windows。** `mcpp.exe` 在应用程序清单里声明 UTF-8 代码页。因此在
Windows 10 1903 及以后的版本上，无论系统的 ANSI 代码页是什么，它的窄字符串
以及它与 Windows 交换的每个路径都是 UTF-8。mcpp 在构建中运行的程序共用这个
代码页：`build.mcpp` 链接时带同一份清单；作为 host 工具构建的程序（§2.14）
也会得到它，除非其目标写了 `windows_code_page = "legacy"`，或者该包自己嵌入
了清单。工程为自己构建的程序保持系统代码页，除非其目标写了
`windows_code_page = "utf-8"`。

Ninja 1.11 及以后的版本在同样的声明下以 UTF-8 读取 `build.ninja`。当文件中
含有非 ASCII 字节时，mcpp 会询问 Ninja 以哪种编码读取（`ninja -t wincodepage`），
若回答不是 UTF-8，就拒绝这次构建并点名那个 Ninja。`cl.exe`、`link.exe` 与
`lib.exe` 只有在响应文件以字节顺序标记开头时才按 UTF-8 读取它，所以 msvc
方言的响应文件以字节顺序标记开头。

**没有 UTF-8 拼法的路径。** 在 Linux 上，文件名是一串字节，不必是 UTF-8
（macOS 的文件系统以 UTF-8 存储名字）。在早于 1903 的 Windows 宿主上，清单会被忽略，进程运行在系统的 ANSI
代码页里，而它只能拼出 Unicode 的一部分。在这两种情况下，有些路径没有 UTF-8
拼法：

- 路径没有 UTF-8 拼法的工程目录或 mcpp 主目录（`MCPP_HOME`）在构建任何东西
  之前就被拒绝。消息用转义拼出该路径（不是 UTF-8 的字节写作 `\xE9`），并给出
  这台宿主上的原因。
- 工程内部名字没有 UTF-8 拼法的文件或目录会被跳过，跳过信息按目录汇报一次：

  ```text
  warning: '/home/user/pkg/test/data' contains names that have no UTF-8 spelling
    impact: those files take no part in the build
    hint: The name's bytes are not UTF-8, and build.ninja and compile_commands.json hold UTF-8 text. ...
  ```

  报出的路径是最近的、有 UTF-8 拼法的祖先目录。测试数据或文档的名字是无害的；
  源文件需要改名。
- 文本不是 UTF-8 的 `build.mcpp` 指令按其键名被拒绝
  （[30-build-mcpp.md](30-build-mcpp.md)）。

在 mcpp 2026.9.26.1 之前，这样的路径会让构建以 `internal: unhandled
exception: [json.exception.type_error.316]` 失败；在 Windows 上，每个非 ASCII
的工程路径或主目录都会这样失败，包括系统代码页能拼出的名字（mcpp#693）。
跳过 Windows ANSI 代码页之外的名字始于 mcpp#516。

### 2.3.1 `[build] accel` —— 本次构建面向的加速器

```toml
[build]
accel = "cuda12.8+{sm_80,sm_90f} ptx>=90"
```

这次构建面向哪些设备后端与体系结构。可被 `--accel` 为单次构建覆盖，
它与 `[toolchain]` 的关系和 `--target` 相同；`--no-accel` 显式请求不
要任何加速器，这正是从一个同时发布设备构建的包中选出纯 CPU 变体的
方式。

这个值会与所消费的任何预构建产物的 `accel` 字段比较，一次不要求任何
加速器的构建，会被任何产物满足。见
[42 —— 异构构建](42-heterogeneous-builds.md)。

### 2.4 `[lib]` —— 库根模块约定

```toml
[lib]
path = "src/capi/lua.cppm"    # Override the default lib-root location
```

默认约定：`src/<包名的最后一段>.cppm`（例如包名 `mcpplibs.cmdline` →
`src/cmdline.cppm`）。

### 2.5 `[dependencies]`、`[dev-dependencies]`、`[build-dependencies]`

已移至 [05 —— 依赖与解析](05-dependencies.md)。

### 2.7 `[toolchain]` —— 工具链配置

```toml
[toolchain]
default = "gcc@16.1.0"

# Cross-compilation target override
[target.x86_64-linux-musl]
toolchain = "gcc@16.1.0"
linkage   = "static"
```

### 2.7.1 `[target.*]` —— 平台条件依赖与 flag

已移至 [22 —— 目标侧](22-target-side.md)。

### 2.7.3 `min_api_level` —— 产物必须能跑在多老的 OS 上

```toml
[target.aarch64-linux-android]
min_api_level = 24
```

Android 自己的术语是 **API level**，这里指的是其中的最小值——NDK 的
CMake 工具链文档里把 `ANDROID_PLATFORM` 记录为承载的那个量（「应用或
库支持的最低 API level」），对应 Gradle 的 `minSdk`。

**这是一个工程决定，不是工具链的属性。** 一个 NDK 服务于一段范围内的
level，所以命名 `android-ndk@<version>` 并不能钉住其中一个。

**它到达的是编译器，而不是身份。** 规范三元组始终是
`aarch64-linux-android`，它命名输出目录、`cfg(env = "android")` 与
打包后的 ABI 标签；level 被融合进传给编译器的那个三元组：

| | |
|---|---|
| 规范三元组 | `aarch64-linux-android` |
| clang 收到的 | `aarch64-unknown-linux-android24` |
| 构建指纹 | 携带这个 level |

指纹里带上它不是可选的：level 决定哪些 bionic 符号可见，所以两个
level 是两种 ABI，绝不能共享一个构建目录。

不设置是合法的，意味着使用 NDK 自己的默认值，也就是 `clang -target
aarch64-linux-android` 归一化后得到的那个值。

这与 `macos_deployment_target`（见上文）用的是同一套机制，两者各自用
自己平台的说法命名，而不是共用一个抽象。两者回答的是同一个问题：产物
必须能运行的最旧 OS 版本是哪个。

### 2.7.2 裸机（`os = none`）—— freestanding 目标

`riscv64-none-elf` 与 `riscv32-none-elf` 是底下没有操作系统的目标。
它们不需要按宿主区分的交叉工具链：clang 与 lld 本就是交叉编译器，所以
任何能安装 llvm 载荷的宿主都能产出它们。

本节是 manifest 参考。实战示例——脚手架搭建、运行、在目标上测试、
freestanding 标准库子集，以及编写一个板级支持包——都在
[40 —— 裸机与 freestanding 目标](40-baremetal.md)里。

```bash
mcpp build --target riscv64-none-elf
mcpp run   --target riscv64-none-elf     # via [target.<triple>].runner
```

**从一个板子包开始**

下面这些内容几乎不需要手写。一个板级支持包携带了 C 库、启动代码、
内存布局与模拟器，所以到达一个能启动的镜像的最短路径是：

```bash
mcpp new blinky --template riscv-virt-rt
cd blinky && mcpp run
```

生成的 manifest 不命名任何链接脚本、加载地址、libc 或模拟器——它完全
没有 `[target.*]` 小节。本节剩下的部分描述这样一个包供给了什么，这
正是在为一块没有这些的板子编写包时应当参照的内容。

**freestanding 目标上有哪些不同**

| | |
|---|---|
| 链接行 | `-nostdlib -nostartfiles -static`，不含任何 hosted 的东西——没有 crt 文件、没有动态链接器、没有 C++ 运行时。链接器以**绝对路径**寻址（`-fuse-ld=<payload>/bin/ld.lld`），因为 `-fuse-ld=lld` 经 `PATH` 解析，在任何前面装了 binutils 的机器上都会找到 GNU ld。 |
| ISA 旗标 | `-march` / `-mabi` / `-mcmodel` 取自目标表，所以仅凭 `--target <triple>` 就足以产出正确的对象文件。 |
| C 库 | **目标自己的那一份**，由 mcpp 从目标自己的行解析，方式与解析编译器完全相同——一个裸机工程不声明任何 libc，正如一个 hosted 工程不声明 glibc。它的头文件到达每一个翻译单元，它的目录在链接搜索路径上，所以一个板子包用裸名字（`-lc`、`-lcrt0-semihost`）从中选取。**哪些**对象、**哪份**链接脚本，仍是板子自己的决定。 |
| 异常与 RTTI | **关闭**，在包括依赖在内的每一个翻译单元上。这里没有展开器，也没有 `libc++abi`，所以没有任何东西能抛出；否则单是 `std::optional::value()` 就会拉进 `__cxa_throw` 以及另外三个未定义符号。它属于目标而不是工程的 `cxxflags`，因为 BMI 会记录它——一个带异常编译的依赖，无法被一个不带异常的单元 import。 |
| `import std` | **不可用。** `std` 是覆盖整个标准库的一个模块——包括线程、文件系统与 iostreams——所以没有一个子集能在没有操作系统的情况下构建出来。两个普通依赖取代了它：**板子包**包装目标的 C 库，**`std-freestanding`** 携带标准库里不需要操作系统的那些部分（实测为 libc++ 110 个头文件中的 103 个）。 |
| 入口点 | 只要有东西提供了 `crt0`，`int main()` 就能工作——一个板子包通常提供，这样固件的入口点就是一个普通的 `main`，它的返回值经由 semihosting 到达宿主。只有零 libc 的板子才需要一个显式目标，其 `main` 指向携带 `_start` 的文件。 |

**一份最小固件**

```toml
[package]
name    = "fw"
version = "0.1.0"

[build]
ldflags = ["-T", "/abs/path/to/link.ld"]

[targets.firmware]
kind = "bin"
main = "src/start.S"          # the entry lives in assembly, not in main()

[target.riscv64-none-elf]
runner = ["qemu-system-riscv64", "-machine", "virt", "-nographic",
          "-no-reboot", "-bios", "default", "-kernel"]
```

**`runner`——`mcpp run` 如何执行一个这台机器本身跑不了的东西**

一个裸机镜像的 ISA 不对，没有加载器，并期望独占整个地址空间；直接
执行它会得到「Exec format error」。`runner` 是站在它前面的那份 argv
模板。产物路径会被**追加**在后面，或者在模板包含 `{}` 时替换掉它。

mcpp 刻意**不附带任何默认 runner**。用哪个模拟器、哪个机器型号、哪种
固件模式，都是板子自己的事实——同一 ISA 上的两块板子可能需要不同的
argv（OpenSBI 启动用 `-bios default`，picolibc 镜像用 `-bios none
-semihosting`）——一个替某一块板子猜一个值的引擎，就是另一块板子必须
与之对抗的引擎。一个板级支持包通常会提供它。

### 2.7.3 hosted 目标上的 `runner`（2026.9.2.1+）

`[target.<triple>].runner` 适用于每一个精确三元组，不只是裸机。一个
hosted 的交叉产物——在 x86_64 机器上构建的 `aarch64-linux-musl`——在
一些宿主上可执行（注册了 qemu-user 的 binfmt_misc），在另一些宿主上会
被拒绝并报 `Exec format error`，这两者哪个成立是机器的属性，不是
三元组的属性。mcpp 不预测它。它要么通过工程声明的 runner 执行产物，
要么尝试直接执行，并报告内核给出的答案。

```toml
[target.aarch64-linux-musl]
runner = ["qemu-aarch64-static"]
```

对 `mcpp run` 与 `mcpp test` 都适用的规则：

- **一个已声明的 runner 会被使用。** 它的第一个元素由 mcpp 定位：先在
  `[xlings.workspace]`（§2.13）下每个已声明载荷的 `bin/` 目录里找，再
  到 `PATH` 上找。`PATH` 上的一个裸名字会解析到一个 xvm shim，它回答
  的是当前 SubOS，而不是这个包；正是载荷查找，让一个 runner 能够命名
  工程自己声明的程序。
- **一个已声明但找不到或启动不了的 runner 是一个错误**，报告程序、
  搜索过的目录与 errno。这里没有回落到直接执行：用不同的解释器、不同
  的参数去运行这个产物，正是这个键存在要防止的那种失败。
- **没有 runner，内核拒绝这个产物：** `mcpp run` 报告这次拒绝与需要
  写的那个键，退出码 2。`mcpp test` 把每个测试都报告为未运行，理由
  只报一次，退出码 2（§2.7.3.1）。
- **`--no-runner`** 直接执行产物，忽略已声明的 runner。它陈述的是
  关于这台宿主的一个事实——这个三元组在这里是原生的——而 manifest
  没有轴可以承载这一点；一个 runner 为 x86_64 开发者写的工程，在
  aarch64 机器上依然可读。

通过 `[xlings.workspace]` 配置模拟器，是给 CI 作业、或构建在单一宿主
类别上的工程用的形式。索引里的 `qemu-user-aarch64` 只为 x86_64
Linux 构建，而这张表会在每个构建这个工程的宿主上配置，所以这个条目
要按平台分别写（§2.13）：

```toml
[xlings.workspace]
"xim:qemu-user-aarch64" = { linux = "" }   # present on Linux, any version

[target.aarch64-linux-musl]
runner = ["qemu-aarch64-static"]
```

一个宿主装不上的包是一个硬构建错误，所以一个没有按平台区分的条目，
会让这个工程在 macOS 与 Windows 上完全无法构建。而在这个包同样不
存在的 Linux/aarch64 宿主上，要传 `--no-runner`。

#### 2.7.3.1 `mcpp test` 与未运行的测试

一个这台宿主无法执行其产物的测试，既没有通过，也没有失败。`mcpp
test` 把它报告为**未运行**，在原因确定时打印一次，在摘要里重复原因
的第一行，退出码 2：

```
warning: this host cannot execute aarch64-linux-musl artifacts: Exec format error (error 8); declare [target.aarch64-linux-musl].runner, or pass --no-runner on a host that can
smoke ... not run
error: test result: NOT RUN. 0 passed; 0 failed; 1 not run (this host cannot execute aarch64-linux-musl artifacts: Exec format error (error 8); ...); finished in 0.41s (build 0.39s + run 0.00s)
```

退出码 1 保留原有含义——一个测试运行过并失败；0 意味着每个测试都
运行过并通过。`--message-format json` 在每条记录上携带
`"status":"not_run"` 与一个 `reason`，摘要记录上则是 `not_run` /
`not_run_reason`（见[50 —— 机器可读输出](50-machine-output.md)）。

### 2.8 `[features]` —— Feature

已移至 [06 —— Feature 与能力](06-features-and-capabilities.md)，连同
`provides` / `requires` 与 `[feature-deps.<name>]`。

### 2.8.3 `[scan_overrides."<glob>"]` —— 作者断言的扫描结果

默认的模块扫描器是一趟文本层面的处理，它（刻意）拒绝条件预处理块内的
`import` 语句。有些合法的模块单元里确实有这种写法——例如 fmt 官方的
`src/fmt.cc` 把 `import std;` 保护在 `#ifdef FMT_IMPORT_STD` 之内。
当一个文件的 import 集合已知且稳定时，声明它，而不是去扫描它：

```toml
[modules]
sources = ["src/**/*.cppm", "vendor/fmt.cc"]

[scan_overrides."vendor/fmt.cc"]
provides = ["fmt"]      # at most one provided module per unit
imports  = ["std"]
```

被这条 glob 匹配到的文件会跳过文本扫描；声明的单元直接进入模块图。
这条声明会在**每次构建时被审计**：编译器自己对这个文件做的 P1689
扫描（`.ddi` dyndep 输入）会与它比对，任何分歧都会让那条编译边失败，
并同时打印双方的结果——一条陈旧的声明无法悄悄污染这张图。一条不匹配
任何源文件的 override glob 是一个错误。

xpkg 描述符（索引包）里也有同一个键：

```lua
mcpp = {
    sources  = { "*/src/fmt.cc" },
    cxxflags = { "-DFMT_IMPORT_STD" },
    scan_overrides = {
        ["*/src/fmt.cc"] = { provides = { "fmt" }, imports = { "std" } },
    },
}
```

要把这套「plan 与 ddi 对照」的审计扩展到**每一个**模块单元（不只是
override），在生成构建时设置 `MCPP_VERIFY_MODGRAPH=1`。

### 2.8.4 默认扫描器读到的东西

扫描器为每个文件回答两个问题——这个单元提供什么、这个单元需要
什么——没有别的东西决定这两个答案。

**三种模块声明，它们是不同的产生式。**

```cpp
module M;          // implementation unit:      requires M, provides nothing
module M:part;     // implementation partition: provides M:part
module : private;  // private module fragment:  declares neither
```

第三种不是一个名字以冒号开头的分区。它在任何方向上都不贡献边，它之后
的内容仍然属于同一个单元。编译器是否实现了它，答案由编译器给出：
GCC 16.1 报告 `sorry, unimplemented: private module fragment`，mcpp
对此不做任何额外处理。

**一个模块扩展名文件不必提供一个模块。** 一个实现单元是 `.cppm` 里
合法的居民，`module_extensions` 说的是要**扫描**哪些文件，而不是每个
文件**是**什么。编译模式跟随扫描结果：一个提供模块的单元被当作接口
编译，并给它一个写出 BMI 的地方；一个不提供模块的单元被当作普通翻译
单元编译。因此两个编译器对同一个文件收到的是同一条指令——这在 mcpp
2026.9.9.1 之前并非如此：Clang 会从扩展名推断出 `c++-module` 并拒绝
这个文件，而 GCC 则会构建它。

**源码按 UTF-8 读取。** 一个 UTF-8 字节顺序标记会被消费掉，不算作
正文的一部分，这与每个编译器的处理方式相同，也是 MSVC 默认写出的
形式。一个 UTF-16 或 UTF-32 标记会被点名拒绝，而不是被误读。同一条
规则也适用于 `mcpp.toml`。

**一个源码不可能声明出来的名字会被拒绝。** 一个模块身份是一串由点
分隔的标识符，后面可以再跟一个 `:` 与另一串这样的序列。任何其它形式
都会在扫描阶段失败，而不会进入构建图——否则它会变成一个没有任何
东西会报告的 BMI 路径。

### 2.9 `[profile.<name>]` —— 构建档案

```toml
[profile.dist]
opt      = 3              # -O level (a number, or the string "s"/"z")
debug    = false          # -g
lto      = true           # -flto (note: some packaged gcc builds ship without the LTO plugin)
strip    = true           # -s at link time
# passthrough escape hatch (fixed keys, open values):
cflags   = ["-fno-plt"]
cxxflags = ["-fno-plt"]
ldflags  = []
```

- 选择与默认值：一条裸的 `mcpp build` 使用 **`dev`** profile
  （`-O0 -g`）——这是主流约定（参见 Cargo/Meson/CMake/Zig/Bazel）。
  **Release 需要显式选择：**`mcpp build --release`（简写）或
  `--profile release`。`--dev` 是 dev 的显式简写。`mcpp test
  --profile <name>` 同样适用（在该 profile 下构建被测代码及测试
  二进制）。
- **按工程的默认值**——`[build].default-profile = "<name>"`（别名：
  `profile`）在没有传入旗标时设置这个工程自己的默认值。典型用法是一个
  默认就该优化构建的工具或库：`[build] default-profile = "release"`。
  优先级：`--profile`/`--release`/`--dev` 旗标 **>**
  `[build].default-profile` **>** 全局默认 `dev`。（一个默认走 dev
  的工程，在产出可分发物时应传 `--release`。）
- 内置 profile：`release`（-O2）/ `dev`、`debug`（-O0 -g）/ `dist`
  （-O3 + strip；**默认不启用 LTO**）。`[profile.<内置名>]` 可以整体
  覆盖一个内置定义。
- **每个 profile 拥有自己的构建目录。** 解析出的 profile 旋钮参与
  指纹，所以 `target/<triple>/` 下每个 profile 各有一个哈希目录，在
  它们之间切换是增量的，不是一次完整重建。这也意味着磁盘开销随实际
  用到的 profile 数量而增长。

### 2.10 `[build] cache` —— 依赖的全局构建缓存

从索引取得的依赖，其编译产物会跨工程缓存在
`$MCPP_HOME/build-cache/v1/` 下。一个依赖的产物不依赖于谁在消费它，
所以两个工具链、profile 与依赖版本都相同的工程会复用同一个条目。

```toml
[build]
cache = "global"   # "global" (default) | "local" | "off"
```

| 模式 | 读缓存 | 写缓存 | 先清空构建目录 |
|---|---|---|---|
| `global`（默认） | 是 | 是 | 否 |
| `local` | 否 | 否 | 否 |
| `off` | 否 | 否 | 是 |

`local` 让每个依赖都在这个工程自己的 `target/` 里构建——用于在排查
某个问题时排除缓存因素，也用于给 CI 一条不共享的基线。`off` 还会额外
清空这次构建的 `target/<triple>/<fp>/` 以获得一次冷构建；
`--no-cache` 是它的一个已废弃别名。

优先级：`--cache <mode>` **>** `MCPP_BUILD_CACHE` **>**
`[build] cache` **>** `global`。一个无法识别的取值会被报告（`--strict`
下是错误），而不是静默回落到 `global`。

**不会**被缓存的：`path` 与 `git` 依赖，不论层级多深，以及
workspace 成员。它们的源码可以在 `name@version` 不变的情况下改变，
所以没有任何基于那个身份的键能察觉到变化。

查看与回收：

```
mcpp cache dir                      # where the cache lives
mcpp cache list [--json]            # entries, sizes, last use
mcpp cache info <pkg>@<ver>         # one entry, including the key inputs it was built with
mcpp cache verify                   # every entry's file list against the disk
mcpp cache gc --max-size 5GiB       # LRU-collect package entries to a budget
mcpp cache gc --older-than 30d      # ...or by how long since they were last used
mcpp cache clean [--deps|--std|--all|--legacy]
```

磁盘上的条目布局是版本化的。一个改变了它的 mcpp 发布版，会一次性
淘汰每一个旧条目，所以这样一次升级之后的第一次构建，会重建它的依赖
并重新填充缓存——不需要手动清理任何东西。2026.8.3.4 做的正是这件事：
一个条目的对象路径现在相对**包**寻址，而不再相对最先填充这个条目的
那个工程的构建目录。`mcpp cache verify` 还会额外报告任何记录的地址
逃出了自身范围的条目，使这类复发情况可以离线审计。

### 2.11 `[runtime]` —— provider-neutral 运行时契约

```toml
[runtime]
requirements = [
  { kind = "capability", value = "display.present", phase = "run", required = true,
    discovery = "rpath-of-dispatch" },
  { kind = "soname", value = "libwidget.so.1", phase = "link", required = false },
]
provides = ["display.present"]
artifacts = [
  { role = "library", path = "runtime/libwidget.so.1", provenance = "payload", abi = "elf-x86_64", digest = "sha256:...", host_fingerprint = "host-1" },
]

# Platform-neutral LinkIntent. Paths are relative to this package root.
libraries                = ["widget"]
link_library_dirs        = ["lib"]
transitive_needed_dirs   = ["runtime/closure"]
runtime_search_dirs      = ["runtime"]
frameworks               = ["WindowKit"]
deploy_files             = ["bin/widget.dll"]
deploy                   = [ { from = "share/vulkan/icd.d/widget_icd.json", to = "vulkan/icd.d" } ]

# Use an exact canonical identity when multiple providers exist.
[runtime."display.present"]
provider = "acme.widget-runtime@2.0.0"
```

这张表里一个不受支持的键会**被报告并被忽略**，消息会列出它核对过的
那些键。一个 `[runtime.<capability>]` 子表是一处提供者覆盖，不是一个
键，所以不会被扫入。同样的规则适用于 `[target.<predicate>.runtime]`，
它的词汇是 `libraries`、`link_library_dirs` 与 `frameworks`
（mcpp 2026.9.12.3+）（[22 —— 目标侧](22-target-side.md)）。那张表上的
`frameworks` 会追加在顶层列表之后，只在 Mach-O 各行上渲染
`-framework <name>`，在其它行上什么都不渲染——这是一份 manifest 在
某个 framework 只存在于 iOS 而不存在于 macOS（或反过来）时要用的键：

```toml
[runtime]
frameworks = ["Foundation", "CoreGraphics"]

[target.macos.runtime]
frameworks = ["AppKit"]

[target.'cfg(os = "ios")'.runtime]
frameworks = ["UIKit"]
```

`requirements` 记录一个非空的 `kind`/`value`，一个 `link` 或 `run`
阶段，以及这条要求是否强制（`required` 默认 `true`）。

`discovery` 是可选的，说明**加载器如何找到**满足这条要求的东西——
例如 `rpath-of-dispatch`、`json-dir`、`glvnd-dispatch`。它是**被
声明的，从不被推断**：一个能力用哪种机制是提供者的属性，会在 mcpp
不知情的情况下改变，所以 mcpp 只是携带这个值，把一个未声明的情形
报告为 `unknown`，而不是去猜。之所以专门设一个字段，是因为这些机制
彼此不可互换——一种可能是烘焙进某个 dispatch 库里的搜索路径，另一种
可能是一个持有*绝对*路径的 JSON 文件，「把这个目录整体拷过去」能
满足一种，满足不了另一种。`mcpp pack` 把它写进 bundle 的
`HOST-REQUIREMENTS`，`mcpp publish` 把它投影进描述符，两者出自同一份
推导。可选要求仍然保留可见的来历信息，但不会成为硬性的 ABI 或 doctor
输入。一条显式写成相对文件路径的 `libraries` 条目，相对声明它的包根
目录解析；一个裸的逻辑名字仍然是一个按平台拼写的库名。`artifacts`
要求 `role`、`path` 与 `provenance`；`abi`、`digest` 与
`host_fingerprint` 是可选的佐证。是解析器而不是描述符，给每一条要求
打上确切的请求方 PackageId，给每一个产物打上确切的声明提供方
PackageId，包括命名空间、版本与来源/索引出处。因此一份描述符无法
冒充另一个包，`alpha.backend` 永远不会被折叠进 `beta.backend`。

只有 `provides` 会创建一个由描述符拥有的提供者事实。仅仅要求一项
能力，永远不会让请求方成为它自己的提供者。一个显式的
`[runtime.<capability>] provider=` 覆盖接受一个规范的
`namespace.name@version`（或一个无歧义的兼容拼法）；缺失、或短名字
有歧义的提供者是硬错误。已被 xlings SubOS 选定的提供者/产物事实，
优先于描述符的回落值。xlings/xim 拥有图形栈、驱动、ICD、WSL 与宿主
出处的选择权；mcpp 只记录并消费那个通用结果，从不探测 GPU 硬件。

Link intent 把各个发现阶段分开处理：

| 字段 | ELF | Mach-O | PE/Windows |
|---|---|---|---|
| `link_library_dirs` | `-L` | `-L` | `-L` 或 `/LIBPATH:` |
| `transitive_needed_dirs` | `-Wl,-rpath-link` | 无旗标 | 无旗标 |
| `runtime_search_dirs` | 仅 RUNPATH/rpath，从不是 `-L` | 仅 rpath | 无旗标 |
| `frameworks` | 无旗标 | `-framework` | 无旗标 |
| `deploy_files` | 拷贝边 | 拷贝边 | 拷贝到输出旁边；从不是链接器旗标 |
| `deploy` *（2026.9.12.2+）* | 拷贝边，进 `bin/<to>/` | 拷贝边，进 `bin/<to>/` | 拷贝边，进 `bin/<to>/`；从不是链接器旗标 |

`deploy` 把一个文件放进相对可执行文件的某个目录，而 `deploy_files`
表达不了这一点，因为它把每一条都放在可执行文件旁边。一个读取固定
子目录的加载器需要它：macOS 上的 Vulkan loader 从
`<可执行文件目录>/vulkan/icd.d` 读取驱动 manifest。每一条都是恰好
两个字符串组成的表。`from` 相对声明它的包根目录，`to` 相对可执行
文件所在目录，`"."` 意味着那个目录本身。两者在每个宿主上都以 `/`
分隔，都不能是绝对路径、不能命名一个驱动器、不能含有空、`.` 或 `..`
组成部分；不满足的条目会被拒绝，拒绝信息点名它的索引。两个来源指向
同一个目的地会被拒绝并点名那个目的地，而一个文件名出现在两个不同
目录下不算冲突。`deploy` 是一个独立的键，而不是 `deploy_files` 的
表格形式，因为一个早于它出现的描述符读取器，遇到 `deploy_files` 里的
`{` 会无法终止，而它会跳过一个不认识的 `runtime` 键。`mcpp pack` 把
这两个键指向的文件，以打包出的可执行文件为参照，拷贝到同样的相对
路径。

对于一批兼容性字段，`library_dirs` 只映射到运行时搜索，`dlopen_libs`
映射到必需的运行期 soname 要求，`capabilities` 映射到必需的运行期
能力要求。这些遗留字段都不创建提供者。

`target/<triple>/<fp>/resolution.json` schema 2 存储 RuntimeBinding、
规范化后的要求/提供者/产物、LinkIntent、平台发现机制与链接后判定。
`mcpp why runtime` 是对最新存储文件的一个纯粹解读器：它既不会重新
解析 manifest，也不会启动一次图形/硬件探测。当被选中的宿主提供者
自身需要重新诊断时，用 `xlings doctor`。

每个产物还携带一个仅从路径计算出的 `identity` 判定：

| `identity` | 含义 |
|---|---|
| `ok` | 声明的路径（经符号链接）解析到声明的版本 |
| `mismatch` | 它解析到了别处——**绑定已过期**，某次更晚的安装重新指向了它 |
| `missing` | 已声明，但那个路径上什么都没有 |
| `unverified` | 声明时没有给出可供核对的版本 |

这正是 mcpp 已经在对私有 libc 应用的那条规则（`glibc@2.44` 解析到
那一份载荷；过期或缺失是错误，绝不是「随便一个装着的、看起来能用的
版本」）的推广。它不需要知道这个产物是做什么的。`unverified` 刻意
不等于 `ok`：一个已解析、背后却没有产物的提供者尚未被核对过，
`mcpp why runtime` 会说
`(not declared by the environment — nothing to verify)`，
而不是 `(none)`。

能力名使用分层的小写 `domain.sub.role`（例如 `display.present`）与
前缀式的 `abi:<name>`（例如参与工具链 ABI 强制检查的 `abi:glibc`）。

### 2.12 `[package] platforms` —— 平台声明

```toml
[package]
platforms = ["linux", "macos", "windows", "ios", "android", "emscripten"]
```

声明这个包支持的平台（一条 CI 矩阵提示，经 `mcpp why` 展示）。词汇由
mcpp（拥有 target/triple 体系的一方）固定：
`linux | macos | windows | ios | android | emscripten`
（mcpp 2026.9.12.3+；`ios`、`android` 与 `emscripten` 是这套此前只有
`linux | macos | windows` 的词汇新增的成员）；未知取值会产生警告，
`--strict` 下是错误。

一个平台名就是目标三元组的 `os`，除非一个自己命名了平台的 `env`
优先。Android 各行的 `os = "linux"`、`env = "android"`，所以这份
列表里的 `linux` 不覆盖它们——一个服务 Android 的包要额外写出
`android`。Web 这一行保留自己的 `os` 单词 `emscripten`，与
`cfg(...)` 选择器语法用的是同一个词；没有 `web` 这种拼法。

`mcpp pack` 在一个库目标上，会拿这条声明与它实际产出的那些腿核对，
因为这是第一个有证据可供核对的时刻：

| 情形 | 结果 |
|---|---|
| 某条腿为一个这里没列出的平台打了包 | 警告——manifest 否认了一个这个包明显在服务的平台 |
| 一个列出的平台没有对应的腿，**并且这台宿主本可以构建出一份** | 警告——那里的消费者会解析到这个包，却找不到产物 |
| 一个列出的平台没有对应的腿，但这台宿主构建不出那个平台的产物 | **无声** |

第三行正是这项检查之所以可用的原因。正常的发布流程是在 CI 里为每个
平台各跑一次 `mcpp pack`，所以一台 Linux runner 从不会产出一条
macOS 的腿——如果对此发出警告，会在每个跨平台包的每一次运行上触发，
而一条总是触发的警告会掩盖真正要紧的那一条。「这台宿主本可以构建」
指的是与 `--target` 回答的同一个问题（docs/08 §7.4）。

两者都只是警告，从不是错误：覆盖度是发布纪律的一部分，能判断它的人
是在看发布本身，而不是在看这一次构建。

### 2.12b `[package] accelerators` —— 加速器声明

```toml
[package]
accelerators = ["cuda", "rocm"]
```

声明这个包支持的加速器后端。与 `platforms` 对称：一条意图陈述与一条
CI 矩阵提示，经 `mcpp why` 展示，从不是一道闸。

它与产物的 `accel` 字段刻意区分开。声明是手写的，可以是愿景；`accel`
是从产出某个二进制的那次构建里实测得到的，也是消费者被拒绝时所对照
的那个值。见[42 —— 异构构建](42-heterogeneous-builds.md)。

### 2.13 `[xlings]` —— 工程的环境

已移至 [23 —— 工程的环境](23-the-project-environment.md)。

### 2.14 依赖产出的 host 工具

已移至 [30 —— build.mcpp](30-build-mcpp.md)。

### 2.15 `[resources]` —— 编译进产物的元数据与资产（2026.8.7.1+）

一个 exe 图标，以及 Windows 在文件属性对话框里展示的版本元数据，在
`mcpp.toml` 里不过是一条路径，别无其它：

```toml
[resources]
icon = "assets/app.ico"
```

这就是常见情形的全部。`FILEVERSION`、`ProductName`、
`FileDescription`、`CompanyName` 与 `LegalCopyright` 全都从
`[package]` 取默认值，资源脚本由 mcpp 自动生成。

| 键 | 类型 | 含义 |
|---|---|---|
| `icon` | 路径 | 作为应用图标嵌入（资源序号 1） |
| `files` | 路径列表 | 自行编写的 `.rc` 脚本，会被编译并**追踪**为构建输入 |
| `extra-inputs` | 路径列表 | `.rc` 扫描器看不见的输入（见下） |
| `version-info` | 布尔值 | `false` 退出自动生成版本资源 |
| `[resources.version-info]` | 表 | `company`、`product`、`description`、`copyright`、`original-filename`、`internal-name` |

**只有 PE 目标会*编译*它。** 在 Linux 与 macOS 上，这一节*不适用*：
没有资源单元、没有诊断、构建逐字节相同。**不需要**（也不能用）一个
`cfg(windows)` 谓词——写一次，无条件生效即可。

**一个声明了却不存在的文件会让构建失败——在每一个目标上都一样。**
一份资源和一个源文件一样是构建输入；mcpp 不会悄悄发布一个缺了它的
二进制。校验刻意**不**只针对 PE：一个路径是否存在，是工作树的一个
事实，与目标无关，所以 `icon = "assets/app.ico"` 里的一个笔误，会被
Linux 或 macOS 的构建（以及它们的 CI 作业）捕获，而不必等到 Windows
那一份。要省略图标，删掉这一行即可。

**版本字段。** `FILEVERSION` 取 `[package].version` 的四个数字段，
每一段都必须落在 16 位以内；字符串字段按原样保留版本号，所以一个
数字字段容纳不了的形式（`1.0.0-rc1`），仍然会原样出现在属性对话框里。

#### 自写 `.rc`

```toml
[resources]
files = ["res/app.rc"]
```

设置了 `files` 之后，mcpp 停止生成版本资源，资源 ID 空间归工程所有。
若两者都要，连同它一起设 `version-info = true`（注意会相撞：序号 1
上只能有一个 `RT_VERSION`）。

要从生成的脚本出发，而不是从一个空文件开始，把它从构建目录里拷出来
（`target/<triple>/<fp>/res/<target>.mcpp.rc`）并列进 `files`。结果
逐字节相同，所以从生成切换到手写，不会改变实际发布的内容。

脚本可以嵌入应用程序清单（`1 24 "app.manifest"`，类型 24 即
`RT_MANIFEST`）。`windows_code_page = "utf-8"` 会在同一个序号上嵌入另一份，
所以同时声明两者的包会被拒绝，并给出保留其一的两种做法：把 `activeCodePage`
元素加进脚本的清单并设 `windows_code_page = "legacy"`，或者从脚本里去掉清单。
作为 host 工具构建、默认得到 `utf-8` 的程序则保留它自己的清单。

> **`VS_VERSION_INFO` 需要 `<windows.h>`。** 在一份手写脚本里，
> `VS_VERSION_INFO VERSIONINFO` 若没有 `#include <windows.h>`，会把
> 版本资源归档到一个*字符串*名字下，而不是序号 1。每个工具仍会报告
> `Type: VERSIONINFO`，但 `GetFileVersionInfo` 查的是序号，所以
> PowerShell 的 `FileVersionInfo` 会显示每个字段都是空的。要么包含
> `<windows.h>`，要么写 `1 VERSIONINFO`。mcpp 看到这种写法时会警告；
> 它自己生成的脚本用的是字面量 `1`。

#### 被跟踪的输入

mcpp 读取 `.rc` 里带引号的 `#include`，以及资源语句（`ICON`、
`RCDATA`、`MANIFEST`、数字类型 `24` 等）命名的文件，并把它们变成构建输入，
所以修改图标会触发重新链接。相对文件名按脚本自己所在的目录解析，rc.exe、
llvm-rc 与 windres 也在那里查找它。尖括号 include（`<windows.h>`）属于工具链，由
工具链指纹覆盖，而不是这项扫描。

一个经由宏到达的文件名（`1 ICON APP_ICON`）对这项扫描是不可见的。
mcpp 会点名它无法解析的部分，并要求显式声明：

```toml
extra-inputs = ["assets/app.ico"]
```

#### 其余一切：`role = "object"`

对于不是资源脚本的输入——一段用 `objcopy` 嵌入的二进制数据、一份
生成的 `.def`、一个预构建的对象文件——构建程序可以声明一个构建图
节点，把它的输出并入链接：

```cpp
mcpp::action o;
o.id = "blob"; o.role = "object";
o.arg("./mkblob.sh").arg("blob.bin").arg("${mcpp.out_dir}/blob.o")
 .input("blob.bin")
 .output("${mcpp.out_dir}/blob.o")
 .target("myapp")        // omit: every image, test binaries included
 .submit();
```

见 [30 —— build.mcpp](30-build-mcpp.md)。把这样一个文件名写进
`[build].ldflags` 也「能用」，但 ldflags 在链接命令里是一段扁平
字符串：没有任何东西追踪它，修改这个文件会得到
`ninja: no work to do`。

### 2.16 `[hooks]` —— 项目构建生命周期命令

已移至 [09 —— 按场景选命令](09-commands-by-scenario.md)。

### 2.17 `[test]` —— 测试程序的位置

```toml
[test]
discover = ["tests/**/*.cpp"]    # the default
```

| 键 | 类型 | 含义 |
|---|---|---|
| `discover` | glob 数组 | 每个被某条 glob 匹配到的文件都是一个测试程序；以 `!` 开头的 glob 会移除它匹配到的文件；`[]` 不发现任何测试 |

这些 glob 使用与 `[build] sources` 相同的词汇。一个测试的名字，是它
相对第一条匹配到它的 glob 所在的固定目录的路径，去掉扩展名。两个
同名文件会被拒绝，并点名两者。一个不是「非空字符串数组」的取值是
一个错误；`[test]` 里的其它任何键都是警告，`--strict` 下是错误。
测试模型见[08 —— 测试](08-testing.md)。

## 3. 实战示例

其中四个是可运行的工程，而不是片段，工程是更好的答案：它能构建，并且
由 CI 检查。

| 形态 | 运行 |
|---|---|
| 一个 hello world | [`examples/01-hello`](../../examples/01-hello/) |
| 一个带测试的模块库 | [`examples/11-features`](../../examples/11-features/) |
| 一个带依赖的应用 | [`examples/02-with-deps`](../../examples/02-with-deps/) |
| 一次交叉编译的静态发布 | [`examples/03-pack-static`](../../examples/03-pack-static/) |

有两种形态目前还没有示例，以 manifest 的形式留在这里。

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

### 3.5 混合 C / C++23 模块工程

```toml
[package]
name    = "hybrid"
version = "0.1.0"

[build]
include_dirs = ["include"]
c_standard   = "c11"

[dependencies]
lua = "5.4.7"     # Pure C library; mcpp compiles .c files with the C compiler automatically

[targets.hybrid]
kind = "bin"
```

## 4. 约定与默认值速查

| 项 | 默认值 | 说明 |
|---|---|---|
| 源文件 | `src/**/*.{cppm,cpp,cc,c,S,s,asm}` | 自动递归扫描 |
| 入口点 | `src/main.cpp` | 这个文件存在时，会推断出一个 `bin` 目标 |
| 库根 | `src/<包名的最后一段>.cppm` | 用 `[lib].path` 覆盖 |
| C++ 标准 | `c++23` | 用 `[package].standard` 配置；支持 `c++20` / `c++26` / `c++2a` / `c++2c` / `gnu++NN` / `c++latest` / `c++fly`（实验性试验场） |
| C 标准 | `c11` | `.c` 文件自动经由 C 编译器处理 |
| 静态 stdlib | `true` | 可移植二进制 |
| 头文件 | `include/`（若存在） | 自动加入 `-I` |
| 测试 | `tests/**/*.cpp` | 由 `mcpp test` 自动发现；`[test] discover` 替换这个集合 |
| 依赖命名空间 | `mcpplibs`（默认） | 一个裸选择器只匹配这一个精确命名空间 |

### 4.1 旧 `[language]` 兼容层

旧的配置仍然可以被读取：

```toml
[language]
standard = "c++26"
```

新工程应使用 `[package].standard`。若两处都写了，以
`[package].standard` 为准。
