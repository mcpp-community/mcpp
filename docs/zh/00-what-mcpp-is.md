# 00 —— mcpp 是什么

**读者:**任何人,在读别的之前。

**本章回答的那一个问题:**mcpp 是什么、它为一个工程做了什么、以及试一下的代价有
多大。

**不在这里:**这些东西内部怎么运转,以及每一个字段和旗标。本章以一段**真的跑过**的
会话结束;它之后的章节才是参考。

## 五样东西合在一个程序里

```
mcpp = 通用构建系统
     + 构建插件
     + 包管理
     + 工具链管理
     + 环境与运行时(xlings)
```

多数 C++ 工程要把这五样从不同工具里拼起来,而新人一个下午就消耗在拼缝上:构建文件
假定机器上有某个编译器,包管理器假定一份不是它写的构建文件,而环境是 README 里的
一段话。

mcpp 是一个程序,所以没有缝需要拼。

对于已经有工具做这些事的读者,各部分大致落在这里:

| 部分 | 在 mcpp 里是 | 大致相当于谁的活 |
|---|---|---|
| 通用构建系统 | `mcpp.toml`、模块图、ninja 后端 | CMake、Meson |
| 构建插件 | `build.mcpp`、规则包 | xmake rules |
| 包管理 | `[dependencies]`、`mcpp.lock`、索引 | Conan、vcpkg |
| 工具链管理 | 编译器作为被安装并钉住的载荷 | 手工装 GCC / LLVM / MSVC,或 Rust 里 rustup 的角色 |
| 环境与运行时 | `[xlings]`、载荷、运行期搜索路径 | Nix、conda |

**这张表是给各部分定位,不是宣称等价。** 上面每一个工具在它自己的领域里做的都比 mcpp
多,需要那种深度的工程应当去用它。这一行说的只是「这个部分对应哪件熟悉的活」,好让
本套文档其余部分有地方可挂。

## 核心保证

> **拿到任何一个 mcpp 工程,`mcpp build` 就能构建** —— 不需要自己装编译器、配环境,
> 也不需要去找依赖的库。

全部主张就是这一句,下面的内容是把它**演示出来**,而不是把它重复一遍。

两条边界写在这里,好让这句话可以被信任:面向设备的工程第一次仍会下载那个设备的
工具包;而这台机器服务不了的目标会被**点名拒绝**,不会被错误地构建出来。

## 第一段会话,从头到尾

跑在一台唯一的 C++ 编译器是 GCC 13 的机器上 —— 那个编译器编不了 `import std`。

```console
$ mcpp new hello
Created bin package 'hello' at /tmp/zero-demo/hello
Next: cd hello && mcpp build && mcpp run  (or `mcpp test`)
```

四个文件,manifest 五行:

```
hello/
├── mcpp.toml
├── src/main.cpp
├── tests/test_smoke.cpp
└── .gitignore
```

```toml
[package]
name        = "hello"
version     = "0.1.0"
description = "A modular C++23 package"
license     = "Apache-2.0"
```

**没有声明编译器、没有声明语言档位、没有声明任何依赖**,而源码用的是这台机器自己的
编译器不具备的特性:

```cpp
import std;

int main() {
    std::println("Hello from hello!");
}
```

```console
$ mcpp run
    Inferred target hello (bin from src/main.cpp)
   Compiling hello v0.1.0 (.)
    Finished dev [unoptimized + debuginfo] in 0.64s
     Running `target/x86_64-linux-gnu/0946988e9e4b52ba/bin/hello`

Hello from hello!
Built with import std + std::println on modular C++23.
```

墙钟 1.25 秒,含首次运行。

干这件事的编译器不是机器上的那一个:

```console
$ g++ --version
g++ (Ubuntu 13.3.0-6ubuntu2~24.04.1) 13.3.0

$ mcpp self env
default toolchain   = gcc@16.1.0
```

mcpp 装了 GCC 16 并用了它。宿主上没有任何东西被改动,而克隆这个工程的同事拿到的是
**同一个编译器**,不是他的发行版恰好带的那个。

**加一个依赖是一行,不需要别的步骤:**

```toml
[dependencies]
"mcpplibs.cmdline" = "^0.0.1"
```

`mcpp build` 会解析它、取回它、构建它、链接它。

## mcpp 面向什么

mcpp 围绕 **C++20/23 模块与最新语言特性**建立,它维护的生态由此而来,而不是来自一个
笼统的雄心:

| | |
|---|---|
| 模块化 C++ | `import std` 零配置、模块扫描、跨工程 BMI 缓存 |
| 嵌入式与裸机 | freestanding 目标、板级支持包、从源码到一个跑起来的镜像只要一条命令 |
| 异构计算与 GPU | CUDA、HIP、SYCL、Vulkan/SPIR-V 与 Ascend C,每一条都是规则包而不是引擎特性 |
| 图形 | 着色器作为构建的一部分被编译,并以模块到达 |
| 内核与底层 | 零 libc 档、显式的链接模型、没有隐藏的宿主依赖 |

这些一样都不需要的工程,照样得到上面那条保证;需要其中之一的工程,不必离开这个工具
去得到它。

## 接下来去哪

| | |
|---|---|
| 把一个程序跑到屏幕上 | [01 —— 快速开始](01-getting-started.md) |
| 判断 mcpp 适不适合手头的工作 | [02 —— 场景](02-scenarios.md) |
| 读一个形状相近的工程 | [03 —— 示例项目](03-examples.md) |
