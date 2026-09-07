# 01 — 示例项目

> 仓库的 [`examples/`](../../examples) 目录下提供了一组循序渐进的最小工程,
> 覆盖从单文件 `import std` 到全静态发布包的常见场景。每个示例都可以
> 独立进入并通过 `mcpp build` 完成构建。

## 运行方式

```bash
git clone https://github.com/mcpp-community/mcpp
cd mcpp/examples/01-hello
mcpp build && mcpp run
```

每个示例附带独立的 README,仅说明该示例相对前一个引入的新概念。
安装步骤、工具链初始化等通用内容统一放在
[00 — 快速开始](00-getting-started.md) 中,不再在示例内重复。

## 示例列表

| # | 路径 | 说明 | 涉及的关键概念 |
|---|---|---|---|
| 01 | [`examples/01-hello`](../../examples/01-hello/) | 单文件 + `import std` 的最小工程 | 最小工程形态(`mcpp new` 还会生成 `tests/test_smoke.cpp`) |
| 02 | [`examples/02-with-deps`](../../examples/02-with-deps/) | 引入依赖 `mcpplibs.cmdline` 解析命令行参数 | `[dependencies]`、SemVer、`mcpp.lock` |
| 03 | [`examples/03-pack-static`](../../examples/03-pack-static/) | 通过 `mcpp pack --mode static` 生成全静态发布包 | `[target.<triple>]` 与 `[pack]` 配置 |
| 04 | [`examples/04-workspace`](../../examples/04-workspace/) | 多包工作空间:两个库 + 一个应用,共享一个命名空间 | `[workspace]`、path 依赖、`mcpp build --workspace` |
| 05 | [`examples/05-lib-distribution`](../../examples/05-lib-distribution/) | 一个预建库和它的消费者,只有放在一起才有意义 | 对库做 `mcpp pack`、同一份源码同时给出 C 头文件与 C++ 模块、distribution 包 |
| 06 | [`examples/06-openkal-cross`](../../examples/06-openkal-cross/) | 同一个程序问每台机器它是什么,从任意宿主构建到四个目标 | `--target`、openkal、不改源码的交叉编译 |
| 07 | [`examples/07-project-subos`](../../examples/07-project-subos/) | 构建程序在工程声明的环境里找工具,而不是问机器上恰好有什么 | `[xlings] subos`、`[xlings.workspace]`、构建程序的 `PATH` 来自工程声明的那个环境 |
| 08 | [`examples/08-build-rules`](../../examples/08-build-rules/) | 两个规则包,以及同时用到它们的工程 | `host-module = true`、`[build-dependencies]`、`role = "check"` 的 `mcpp::action` |
| 09 | [`examples/09-heterogeneous`](../../examples/09-heterogeneous/) | 同一个计算在设备上跑,写成多种编程模型,每种都带 CPU 回退;外加一个同时携带多个后端的产物 | `accel`、带约束的 source glob、接缝模块、来自 `mcpp:plugins` 的规则包、`cfg(accelerator = …)` |
| 09a | [`…/cuda`](../../examples/09-heterogeneous/cuda/) | 接缝模块背后的 CUDA kernel | `mcpp.rules.cuda`、`role = "object"` 的 `mcpp::action`、把驱动陈述为 fact 与 floor |
| 09b | [`…/vulkan`](../../examples/09-heterogeneous/vulkan/) | 同一个计算写成 Vulkan compute shader,在 GPU 上或在 CPU 上 | `mcpp.rules.spirv`、`role = "source"` 的 `mcpp::action`、生成的头文件、作为载荷的软件驱动 |
| 09c | [`…/sycl`](../../examples/09-heterogeneous/sycl/) | 同一个计算写成 SYCL kernel,由第二个编译器编译 | `mcpp.rules.sycl`、`.sycl` 设备扩展名、为 device link 串起来的 `mcpp::action`、`compat:sycl-runtime` |
| 09d | [`…/hip`](../../examples/09-heterogeneous/hip/) | 同一个计算写成 HIP,够到一台 NVIDIA 设备 | `mcpp.rules.hip`、HIP 作为 CUDA 运行时之上的一层头文件、两段式的 `accel` |
| 09e | [`…/multi-backend`](../../examples/09-heterogeneous/multi-backend/) | 多个后端进**同一个产物**,运行期选择 —— 这是库的形态,不是程序的形态 | `accel` 作为集合、`cfg(accelerator = "none")` 及其否定、分发链、C 岛边界之上的模块接缝 |
| 09f | [`…/cann`](../../examples/09-heterogeneous/cann/) | 同一道接缝背后的 Ascend C kernel。**目前还构建不了** —— README 里点明了缺的两块 | `.asc` 设备扩展名、CANN 本来就有的 `op_kernel`/`op_host` 岛、回退用 `accelerator = "none"` |

## 推荐阅读顺序

建议按编号依次阅读:

1. **`01-hello`** 展示 mcpp 工程的最小骨架(`mcpp.toml` 与 `src/main.cpp`),
   并演示 `import std` 的基本用法。当前 `mcpp new` 脚手架还会生成
   `tests/test_smoke.cpp`。
2. **`02-with-deps`** 在前一示例基础上引入外部依赖,涵盖锁文件机制
   与模块化包索引的工作方式。
3. **`03-pack-static`** 演示如何将构建产物打包为可独立分发的单文件
   二进制;打包细节可参考 [02 — 发布打包](02-pack-and-release.md)。

## 新增示例

示例工程遵循统一的目录结构:`mcpp.toml` + `src/` + `README.md`。
新增示例时,在 `examples/` 下创建编号目录(如 `04-xxx/`),并在
README 中简要说明该示例演示的概念,然后提交 PR。提交规范见
[04 — 从源码构建 & 参与贡献](04-build-from-source.md)。
