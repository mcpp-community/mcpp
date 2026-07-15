# 03 — 工具链管理

> mcpp 维护一个独立的工具链沙盒,与系统 PATH 完全隔离。

## 设计动机

C++23 模块对编译器版本较为敏感,不同版本的 GCC / Clang 在模块语义
处理上存在明显差异。系统包管理器提供的版本通常滞后,且多版本共存
存在维护成本。mcpp 将工具链统一安装在沙盒目录
(`~/.mcpp/registry/data/xpkgs/`)中,允许项目按需选择版本,且不会
影响系统环境。

## 自动安装

首次运行 `mcpp build` 时,若尚未配置工具链,mcpp 会自动安装当前平台
的默认工具链并将其设为全局默认:

```
First run no toolchain configured — installing gcc@15.1.0-musl (musl, static) as default
Downloading xim:musl-gcc@15.1.0 [====>      ] 312 MB / 808 MB  3.7 MB/s
Default set to gcc@15.1.0-musl
```

首跑默认是 host-aware 的:Linux x86_64 → `gcc@16.1.0`(glibc——平台原生
ABI,X11/GL/系统库开箱即链);其他 Linux arch(aarch64 等)→
`gcc@15.1.0-musl`(自包含,全静态);macOS 与 Windows → `llvm@20.1.7`。
任何 Linux 宿主上,全静态 musl 产物始终只差一个参数:
`mcpp build --target x86_64-linux-musl`。

后续构建不再触发该流程。

> [!TIP]
> 在 CI 或离线环境中,可通过设置 `MCPP_NO_AUTO_INSTALL=1` 关闭自动
> 安装行为。此时若未安装工具链,`mcpp build` 将直接报错而不会发起
> 网络请求。

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
mcpp toolchain install llvm 20.1.7          # LLVM/Clang,macOS/Windows 默认工具链
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
`[build] linkage = "dynamic"` 可退出。

manifest 中:

```toml
[toolchain]
windows = "gcc@16"            # Windows 上的 gcc family = MinGW-w64
# 旧值 "mingw@16.1.0" 原样可用
```

## 项目级版本锁定

若项目需固定特定版本而不依赖全局默认,可在项目的 `mcpp.toml` 中声明:

```toml
[toolchain]
default = "gcc@16.1.0"

# 也可按平台分发
[toolchain]
linux = "gcc@15.1.0-musl"
macos = "llvm@20"
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
| `MCPP_NO_COLOR=1` / `NO_COLOR=1` | 禁用彩色输出 |
| `MCPP_LOG=trace\|debug\|info\|warn\|error` | 日志级别 |

未显式设置 `MCPP_HOME` 时,mcpp 将基于二进制所在目录的上一级路径
自动定位沙盒位置(release tarball 解压至 `~/.mcpp/` 后,`~/.mcpp/`
即为 home),因此 release 版本无需任何环境变量配置即可运行。


## ABI 能力强制

依赖可声明 `abi:<name>` 能力(如 `compat.glfw` 声明 `abi:glibc`)。解析出的
工具链 ABI 不满足任一依赖的 abi 要求时,构建会**尽早失败**并给出修复建议
(例如 musl-static 工具链遇到 abi:glibc 依赖),取代深层的链接/头文件报错。
查看:`mcpp why toolchain`。
