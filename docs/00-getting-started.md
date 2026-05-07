# 00 — 快速开始

> 5 分钟完成 install → new → build → run → pack 全流程。

## 安装

仅需 Linux x86_64 环境,无需预先安装 GCC、xlings 或其他依赖。
mcpp 在首次运行时会将 musl-gcc 工具链安装至独立沙盒(`~/.mcpp/`)。

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
[README 的"安装"小节](../README.md#安装)。

安装完成后,启动新的 shell 会话或执行 `source ~/.bashrc`,然后验证:

```bash
mcpp --version
# mcpp 0.0.1
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

首次构建需下载默认工具链(musl-gcc 15.1,约 800 MB),期间显示进度
与速度。下载完成后,所有 mcpp 项目共用同一份沙盒。

## 增量编译与测试

```bash
mcpp build              # 增量构建
mcpp clean              # 清理 target/
mcpp test               # 编译并运行 tests/**/*.cpp(gtest 风格)
```

## 添加依赖

在 `mcpp.toml` 中声明依赖:

```toml
[dependencies]
"mcpplibs.cmdline" = "^0.0.1"
```

`mcpp build` 将自动从
[mcpp-index](https://github.com/mcpp-community/mcpp-index) 解析 SemVer
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
