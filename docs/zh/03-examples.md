# 03 —— 示例项目

**读者:**在挑一个起点,或者在找一个与自己形状相近的工程的人。

**本章回答的那一个问题:**哪个示例教什么,以及它们以什么顺序相互叠加。

**不在这里:**任何一个示例的内容 —— 每个示例自带 README,只解释它新增的部分。
在此之前:[01 —— 快速开始](01-getting-started.md)。在此之后:
[04 —— mcpp.toml 工程文件指南](04-mcpp-toml.md)。

[`examples/`](../../examples) 目录是一套课程。每个工程都可以单独跑起来,而且每个
工程都教**一件更早的示例没有教过的事**。本章说明那件事是什么,于是你可以按自己
需要的深度进入,而不必从头读起。

## 运行方式

```bash
git clone https://github.com/mcpp-community/mcpp
cd mcpp/examples/01-hello
mcpp build && mcpp run
```

每个示例自带 README,只解释它新增的部分。安装与工具链初始化在
[01 —— 快速开始](01-getting-started.md),不在示例里重复。

## 课程

### A —— 工程的形状

| 示例 | 首次引入的内容 |
|---|---|
| [`01-hello`](../../examples/01-hello/) | 一个包、`import std`、`mcpp build` 与 `mcpp run` |
| [`02-with-deps`](../../examples/02-with-deps/) | `[dependencies]`、锁文件、`mcpp add` |
| [`04-workspace`](../../examples/04-workspace/) | `[workspace]`、path 依赖、`mcpp build --workspace` |
| [`11-features`](../../examples/11-features/) | **声明** feature 而不是消费它,`[feature-deps]`、`[dev-dependencies]`、`[profile.<name>]`、`mcpp::has_feature` |

### B —— 发布

| 示例 | 首次引入的内容 |
|---|---|
| [`03-pack-static`](../../examples/03-pack-static/) | `mcpp pack --mode static`、`[target.<triple>]`、`[pack]` |
| [`05-lib-distribution`](../../examples/05-lib-distribution/) | 一个库的接口与它的预编译二进制;从同一份源产出 C 头文件与 C++ 模块 |

### C —— 环境

| 示例 | 首次引入的内容 |
|---|---|
| [`07-project-subos`](../../examples/07-project-subos/) | `[xlings]`、`[xlings.workspace]`,以及 `PATH` 来自工程声明环境的构建程序 |

### D —— 目标

| 示例 | 首次引入的内容 |
|---|---|
| [`06-openkal-cross`](../../examples/06-openkal-cross/) | `--target`,同一份源在任意宿主上为四台机器构建 |

裸机由**模板**而不是本目录里的一个工程来教 —— 见下面的*以模板形式到达的课程*。

### E —— 设备与图形

[`09-heterogeneous`](../../examples/09-heterogeneous/) 按顺序读。它的 README 是
地图;下表是每个子示例新增的部分。

| 示例 | 首次引入的内容 |
|---|---|
| [`…/boundary`](../../examples/09-heterogeneous/boundary/) | 单独的岛边界:消费者 import 一个生成的模块,工程里没有接缝也没有头文件。不需要设备 |
| [`…/cuda`](../../examples/09-heterogeneous/cuda/) | 设备编译器、生成边界之上的接缝、把驱动陈述为 fact 与 floor |
| [`…/vulkan`](../../examples/09-heterogeneous/vulkan/) | 一个 compute shader,其 SPIR-V 载荷以模块到达 |
| [`…/sycl`](../../examples/09-heterogeneous/sycl/) | 第二个编译器,自带它自己的标准库 |
| [`…/hip`](../../examples/09-heterogeneous/hip/) | 手写的边界 —— 与 `boundary/` 和 `cuda/` 的对照 |
| [`…/cann`](../../examples/09-heterogeneous/cann/) | NVIDIA 与 Khronos 谱系之外的厂商 |
| [`…/multi-backend`](../../examples/09-heterogeneous/multi-backend/) | 多个后端进同一个产物,运行期选择 |
| [`10-graphics/offscreen`](../../examples/10-graphics/offscreen/) | 结果是像素的渲染管线,并与软件光栅器逐像素比对 |

### F —— 为生态编写扩展

| 示例 | 首次引入的内容 |
|---|---|
| [`08-build-rules`](../../examples/08-build-rules/) | 两个规则包与同时使用它们的工程;`host-module = true`、`role = "check"` 的 `mcpp::action` |
| [`12-a-new-device-language`](../../examples/12-a-new-device-language/) | `device_extensions` 与 `rule_module`:规则包教会 mcpp 一门引擎从未听说过的语言 |

[31 —— 编写规则包](31-authoring-a-rule-package.md) 是这两个示例所演示内容的参考。

## 以模板形式到达的课程

一个包可以提供 `templates/<name>/`,由 `mcpp new --template` 实例化。那是与本目录
和章节并列的第三个教学面;当被教的东西属于某个包而不属于 mcpp 时,课程就落在那里。

| 模板 | 课程 | 章节 |
|---|---|---|
| `riscv-virt-rt` | 一个裸机工程、它的板级支持与它的 runner | [40](40-baremetal.md) |
| `riscv-virt-rt:nolibc` | 同上,但没有 C 库 | [40](40-baremetal.md) |
| `ocornut.imgui` | 一个带窗口与渲染栈的图形应用 | [20](20-toolchains.md) |

```bash
mcpp new blinky --template riscv-virt-rt
```

## 新增一个示例

一个示例目录是 `mcpp.toml` + `src/` + `README.md`,编号接在最后一个之后。什么时候
值得新增一个示例:当一个能力**改变工程的形状** —— 它包含的文件、它声明的 manifest、
或者作者敲的命令。如果一个能力只是既有示例工程里的一行,它属于对应章节里的一个
代码块;如果它只经由命令到达,它属于
[09 —— 按场景选命令](09-commands-by-scenario.md)。

README 要写明这个示例第一个教什么,以及判断它是否成立的判据。贡献流程见
[90 —— 从源码构建 & 参与贡献](90-build-from-source.md)。

