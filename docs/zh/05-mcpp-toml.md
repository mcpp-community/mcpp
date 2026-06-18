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
- 源文件: `src/**/*.{cppm,cpp,cc,c}`
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
standard    = "c++23"              # C++ 标准(默认 c++23; 可设 c++26)
description = "My awesome app"     # 简介(可选)
license     = "MIT"                # 许可证(可选)
authors     = ["Alice", "Bob"]     # 作者列表(可选)
repo        = "https://github.com/user/myapp"  # 仓库地址(可选)
```

`standard` 是 C++ 语言标准的一等配置。推荐值:

- `c++23`：默认值，适合当前模块化默认模板。
- `c++26`：需要 C++26 语言特性时使用。
- `c++2c`：兼容别名，解析后归一为 `c++26`。
- `gnu++23` / `gnu++26`：需要 GNU dialect 时使用，会进入 fingerprint 和 std BMI cache key。
- `c++latest`：跟随当前 mcpp 支持的最新标准，适合本地试验，不推荐要求可复现的发布包使用。

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
sources      = ["src/**/*.cppm", "src/**/*.cpp"]  # 源文件 glob(默认: src/**/*.{cppm,cpp,cc,c})
include_dirs = ["include", "third_party/include"]  # 头文件搜索路径
c_standard   = "c11"              # C 源文件的标准(默认 c11)
cflags       = ["-DFOO=1"]        # 额外 C 编译参数
cxxflags     = ["-DBAR=2"]        # 额外 C++ 编译参数(不要放 -std=...)
ldflags      = ["-lfoo"]          # 额外链接参数
static_stdlib = true               # 静态链接 libstdc++(默认 true)
macos_deployment_target = "14.0"   # macOS 产物的最低支持系统版本(仅 macOS 生效)
```

`macos_deployment_target` 设定产物 Mach-O 头里的最低系统版本
(`LC_BUILD_VERSION minos`),即二进制能运行的最老 macOS。优先级与各生态
惯例一致:环境变量 `MACOSX_DEPLOYMENT_TARGET`(单次调用的显式覆盖,
cargo/rustc、cc 等同样尊重该变量)> 本字段(项目默认,类似 SwiftPM 的
`platforms:`)> **内建默认 `14.0`**(rustc 风格——每个 target 都有基线,
14.0 即 LLVM 官方静态库自身的下限)。该值会进入 BMI 指纹——切换 target
会自动重建模块缓存。

**默认即静态运行时(portable by default)**:`static_stdlib = true`
(默认)时,macOS 链接会静态链入 LLVM 自带的 libc++/libc++abi ——
系统 libc++ 会把实际可运行版本钉死在构建机的 OS(老系统缺新符号,
如 `std::print` 的支撑符号),静态化才能真正兑现 floor。因此默认构建的
产物在任何 macOS ≥ 14 上开箱即用。设 `static_stdlib = false` 退回动态
系统 libc++(产物只保证在构建机同版本及以上运行)。更低 floor(11–13)
需自建 libc++ 归档(已验证可行,数据级切换,按需提供)。

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

# Git 依赖
[dependencies]
mylib = { git = "https://github.com/user/mylib.git", tag = "v1.0.0" }

# 长式 dep spec:features 与 backend 旋钮
[dependencies]
imgui = { version = "0.0.3", features = ["docking"] }   # 请求该依赖的 feature
widget = { version = "1.0", backend = "glfw_opengl3" }  # 糖:= features=["backend-glfw_opengl3"]
```

`backend = "<impl>"` 是**通用约定糖**:1:1 脱糖为请求该依赖的 `backend-<impl>`
feature(库若支持该旋钮,应在自己的 `[features]` 中声明 `backend-*` 系列)。
若目标包声明了 `[features]` 但不含所请求的 feature(含 backend 脱糖结果),
默认给出 warning,`mcpp build --strict` 下报错。

**SemVer 约束**:

```toml
[dependencies]
foo = "^1.2.3"      # >= 1.2.3, < 2.0.0 (caret,默认)
bar = "~1.2.3"      # >= 1.2.3, < 1.3.0 (tilde)
baz = "=1.2.3"      # 精确匹配
qux = ">=1.0, <2.0" # 范围组合
```

### 2.6 `[dev-dependencies]` — 测试依赖

```toml
[dev-dependencies]
gtest = "1.15.2"
```

`mcpp build` 忽略这些;`mcpp test` 解析并使用。`mcpp test` 会自动发现 `tests/**/*.cpp` 并编译为测试二进制。

### 2.7 `[toolchain]` — 工具链配置

```toml
[toolchain]
default = "gcc@16.1.0"

# 跨编译目标覆盖
[target.x86_64-linux-musl]
toolchain = "gcc@15.1.0-musl"
linkage   = "static"
```

### 2.8 `[features]` — 特性(Cargo 式,加性)

```toml
[features]
default = ["base"]        # 默认激活集
base    = []
docking = ["extra"]       # 激活 docking 时隐含激活 extra(传递闭包)
extra   = []
```

- 激活来源:包自身 `default` 集 ∪ 显式请求(根包 `mcpp build --features a,b`;
  依赖经长式 dep spec `features = [...]` / `backend = "..."` 糖)。
- 每个激活的 feature 在该包的编译中得到宏 `-DMCPP_FEATURE_<NAME>`
  (名字转大写,非字母数字转 `_`,如 `backend-a` → `MCPP_FEATURE_BACKEND_A`)。
- **strict 校验**:目标包声明了 `[features]` 表时,请求未声明的 feature 给出
  warning;`--strict` 下报错。未声明 `[features]` 的包接受任意请求(纯宏用法)。

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

- 选择:`mcpp build --profile <name>`(以及 `mcpp test --profile <name>`,会让被测代码与测试二进制
  都在该 profile 下编译),默认 `release`。
- 内置档案:`release`(-O2)/ `dev`、`debug`(-O0 -g)/ `dist`(-O3 + strip;
  **不默认开 lto**)。`[profile.<内置名>]` 可整体覆盖内置定义。

### 2.10 `[runtime]` — 主机运行时能力

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

### 2.11 `[package] platforms` — 平台声明

```toml
[package]
platforms = ["linux", "macos", "windows"]
```

声明包支持的平台(CI 矩阵提示,经 `mcpp why` 展示)。词表由 mcpp 固定
(它拥有 target/triple 体系):`linux | macos | windows`;未知值 warning,
`--strict` 下报错。

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
| 源文件 | `src/**/*.{cppm,cpp,cc,c}` | 自动递归扫描 |
| 入口 | `src/main.cpp` | 有这个文件就推断为 `bin` 目标 |
| 库根 | `src/<pkg-tail>.cppm` | 可用 `[lib].path` 覆盖 |
| C++ 标准 | `c++23` | 用 `[package].standard` 配置; 支持 `c++26` / `c++2c` |
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
