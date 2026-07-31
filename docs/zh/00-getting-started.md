# 00 — 快速开始

> 5 分钟完成 install → new → build → run → pack 全流程。

## 安装

仅需 Linux x86_64 或 macOS ARM64 环境,无需预先安装 GCC、xlings 或其他依赖。
mcpp 在首次运行时会将默认工具链安装至独立沙盒(`~/.mcpp/`)。
Linux 默认使用 musl-gcc,macOS 默认使用 LLVM/Clang。

推荐通过 [xlings](https://xlings.d2learn.org) 进行安装,可与系统
环境保持隔离:

```bash
xlings install mcpp -y
```

或使用一键安装脚本(内置 xlings,统一安装至 `~/.mcpp/`):

```bash
curl -fsSL https://github.com/mcpp-community/mcpp/releases/latest/download/install.sh | bash
```

完整安装说明(包括 xlings 安装命令、Windows 支持等)参见
[README 的"安装"小节](../../README.zh-CN.md#安装)。

安装完成后,启动新的 shell 会话或执行 `source ~/.bashrc`,然后验证:

```bash
mcpp --version
# mcpp 2026.7.29.1
```

> [!TIP]
> 若提示 `command not found`,通常是 `~/.mcpp/bin` 尚未加入当前 shell
> 的 PATH。重启终端,或执行 `source ~/.bashrc`(zsh 对应 `~/.zshrc`,
> fish 使用 `exec fish`)即可生效。也可直接通过绝对路径
> `~/.mcpp/bin/mcpp` 调用。

## 创建项目

```bash
mcpp new hello && cd hello
```

生成的目录结构如下:

```
hello/
├── mcpp.toml         ← 工程描述
└── src/
    └── main.cpp
```

`src/main.cpp` 默认为 C++23 模块化的 hello world:

```cpp
import std;

int main() {
    std::println("Hello from hello!");
    std::println("Built with import std + std::println on modular C++23.");
}
```

## 构建与运行

```bash
mcpp build
# Compiling hello v0.1.0 (.)
# Finished release [optimized] in 1.6s

mcpp run
# Hello from hello!
# Built with import std + std::println on modular C++23.
```

首次构建需下载默认工具链(Linux 为 musl-gcc 15.1,macOS 为 LLVM/Clang 20.1),
期间显示进度与速度。下载完成后,所有 mcpp 项目共用同一份沙盒。

## 增量编译与测试

```bash
mcpp build              # 增量构建
mcpp clean              # 清理 target/
mcpp test               # 编译并运行 tests/**/*.cpp —— 每文件一个独立二进制,
                        # 框架无关(裸 main,或经 [dev-dependencies] 使用 gtest)
mcpp test <pattern>     # 只运行名字包含 <pattern> 的测试
mcpp test --list        # 只枚举测试,不构建
mcpp test --timeout 30  # 单个测试**运行**超过 30s 被终止(默认 300;0 = 不限)
mcpp test --build-timeout 120   # 单次编译/链接超过 120s 被终止(默认关闭)
```

**运行**那一半默认有界 —— 无人值守的 CI 不该被一个挂住的测试吃掉整个 job。两个期限
覆盖的是不同的一半,互不蕴含:`--timeout` 约束测试**进程的运行**,`--build-timeout`
约束**单次 ninja 驱动**(包级构建、批量测试构建、每个测试各自独立计时)。
**链接卡死属于 `--build-timeout`,`--timeout` 设多大都无效。**

`--build-timeout` 默认关闭,这个不对称是**实测**出来的而非风格选择:单个测试跑过 5 分钟
不寻常,而冷依赖构建跑过 15 分钟很平常(mcpp-index 有一个成员要从源码建 OpenCV,
linux 1019s、windows 1289s)。给它一个默认上限会把「慢但正确」的构建判红。构建可以跑多久
是工程自身的性质,所以由工程来说。仅 POSIX 有效 —— Windows 上没有 kill-by-handle 路径,
该值被忽略。

## 添加依赖

在 `mcpp.toml` 中声明依赖:

```toml
[dependencies]
"mcpplibs.cmdline" = "^0.0.1"
```

`mcpp build` 将自动从
[mcpp-index](https://github.com/mcpplibs/mcpp-index) 解析 SemVer
约束、拉取源码并加入编译图。完整示例参见
[01 — 示例项目](01-examples.md) 中的 `02-with-deps`。

## 生成发布包

`mcpp pack` 将构建产物与运行期依赖打包为可独立分发的 tarball:

```bash
mcpp pack                          # 默认 bundle-project,包含项目第三方 .so
mcpp pack --mode static            # 全静态(musl)
mcpp pack --mode bundle-all        # 全自包含,含 libc 与 ld-linux
```

三种模式的差异及产物布局参见 [02 — 发布打包](02-pack-and-release.md)。

## 后续阅读

- [01 — 示例项目](01-examples.md) — 可直接运行的最小工程集合
- [02 — 发布打包](02-pack-and-release.md) — 构建可分发产物
- [03 — 工具链管理](03-toolchains.md) — 切换编译器与多版本管理
- 任意命令的完整选项可通过 `mcpp <cmd> --help` 查阅


## 更多入口

- GUI 起步:`mcpp new myapp --template imgui`(模板随 imgui 库分发、版本自动对齐;
  `mcpp new --list-templates imgui` 查看库提供的全部模板,`--template imgui:docking` 选指定模板)。
- 解释默认决策:`mcpp why [toolchain|runtime|deps]`;主机能力体检:`mcpp self doctor`;
  机器可读解析清单:构建产物 `target/<triple>/<fp>/resolution.json`。
