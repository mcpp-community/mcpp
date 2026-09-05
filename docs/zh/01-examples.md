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
| 08 | [`examples/08-build-rules`](../../examples/08-build-rules/) | 两个规则包,以及同时用到它们的工程 | `host-module = true`、`[build-dependencies]`、`role = "check"` 的 `mcpp::action` |
| 09 | [`examples/09-cuda-kernel`](../../examples/09-cuda-kernel/) | 接缝模块背后的 CUDA kernel,并带 CPU 回退 | `accel`、带约束的 source glob、`role = "object"` 的 `mcpp::action`、`cfg(accelerator = …)` |
| 10 | [`examples/10-vulkan-compute`](../../examples/10-vulkan-compute/) | 同一个计算写成 Vulkan compute shader,在 GPU 上或在 CPU 上 | 来自 `mcpp:plugins` 的 `mcpp.rules.spirv`、`role = "source"` 的 `mcpp::action`、生成的头文件、作为载荷的软件驱动 |
| 11 | [`examples/11-sycl-kernel`](../../examples/11-sycl-kernel/) | 同一个计算写成 SYCL kernel,由第二个编译器编译 | 来自 `mcpp:plugins` 的 `mcpp.rules.sycl`、`.sycl` 设备扩展名、为 device link 串起来的 `mcpp::action`、`compat:sycl-runtime` |
| 12 | [`examples/12-hip-kernel`](../../examples/12-hip-kernel/) | 同一个计算写成 HIP,够到一台 NVIDIA 设备 | 来自 `mcpp:plugins` 的 `mcpp.rules.hip`、HIP 作为 CUDA 运行时之上的一层头文件、两段式的 `accel` |

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
