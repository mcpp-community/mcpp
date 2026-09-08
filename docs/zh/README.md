# 用户文档目录

[English](../README.md) | **简体中文**

这棵树是 **mcpp 已实现功能的使用手册**。每一章陈述一个能力做什么、怎么写、
当前边界在哪里。设计理由、被否掉的替代方案、以及尚未发布的规划,**不写在这里**
—— 它们属于设计记录,而设计记录不是用户文档。

## 文档的归属

| 树 | 读者 | 内容 |
|---|---|---|
| `docs/**` | 手上有任务的人 | mcpp 已实现的东西怎么用 |
| [`docs/specs/**`](../specs/README.md) | 对着机制做实现的人:索引作者、下游工具、贡献者 | 语义、约束与匹配规则,每条标注实现状态 |
| `.agents/docs/**` | 做过某次改动的人,以及以后问「为什么是这样」的人 | 推理、实测,以及什么被推翻了 |
| `.agents/skills/**` | 照流程执行的贡献者或 agent | 带判据的步骤 |

章节引用规范以取得精确语义,但**不引用设计记录**:记录描述的是一个时刻,不带
稳定性承诺,因此读者需要的东西一律写在本树或规范里。

## 从这里开始

| 目标 | 阅读 | 运行 |
|---|---|---|
| 写一个程序 | [00](00-getting-started.md)、[02](02-mcpp-toml.md) §1 | [`01-hello`](../../examples/01-hello/)、[`02-with-deps`](../../examples/02-with-deps/) |
| 写一个供他人 import 的库 | [11](11-publishing-a-library.md)、[04](04-features-and-capabilities.md)、[02](02-mcpp-toml.md) §2.4 | [`04-workspace`](../../examples/04-workspace/)、[`11-features`](../../examples/11-features/) |
| 发布它 | [10](10-pack-and-release.md)、[11](11-publishing-a-library.md)、[12](12-binary-distribution.md) | [`03-pack-static`](../../examples/03-pack-static/)、[`05-lib-distribution`](../../examples/05-lib-distribution/) |
| 为另一台机器构建 | [21](21-the-target-triple.md)、[24](24-openkal-cross.md)、[30](30-baremetal.md) | [`06-openkal-cross`](../../examples/06-openkal-cross/)、`mcpp new … --template riscv-virt-rt` |
| 使用 GPU 或加速器 | [32](32-heterogeneous-builds.md)、[31](31-devices.md) | [`09-heterogeneous`](../../examples/09-heterogeneous/),从 [`boundary/`](../../examples/09-heterogeneous/boundary/) 开始 |
| 增加一条规则、一种语言或一个生成器 | [40](40-authoring-a-rule-package.md)、[05](05-build-mcpp.md) | [`08-build-rules`](../../examples/08-build-rules/)、[`12-a-new-device-language`](../../examples/12-a-new-device-language/) |
| 向索引添加一个包 | [11](11-publishing-a-library.md)、[SPEC-001](../specs/package-identity.md) | [06](06-commands-by-scenario.md) —— 发布相关场景 |
| 修改 mcpp 本身 | [90](90-build-from-source.md)、[92](92-release.md)、[51](51-supported-versions.md) | — |

课程也可以以**项目模板**的形式到达:模板由包提供,`mcpp new --template` 实例化
它。今天有文档的两个是 `riscv-virt-rt`(裸机)与 `ocornut.imgui`(图形应用),
用到它的章节会点名。

## 章节

编号说明这一章属于哪一部分:`0x` 使用 mcpp,`1x` 发布构建产物,`2x` 工具链与目标,
`3x` 裸机与设备,`4x` 扩展 mcpp,`5x` 面向机器的契约,`9x` mcpp 自身。同一部分内部
的排列是阅读顺序。

### 0x —— 使用 mcpp

- [00 —— 快速开始](00-getting-started.md)
- [01 —— 示例项目](01-examples.md)
- [02 —— mcpp.toml 工程文件指南](02-mcpp-toml.md)
- [03 —— 工作空间](03-workspace.md)
- [04 —— Feature 与能力](04-features-and-capabilities.md)
- [05 —— 构建程序:`build.mcpp`](05-build-mcpp.md)
- [06 —— 按场景选命令](06-commands-by-scenario.md)

### 1x —— 发布构建产物

- [10 —— 发布打包](10-pack-and-release.md)
- [11 —— 发布一个库到 mcpp-index](11-publishing-a-library.md)
- [12 —— 分发预编译库](12-binary-distribution.md)

### 2x —— 工具链与目标

- [20 —— 工具链管理](20-toolchains.md)
- [21 —— 目标三元组](21-the-target-triple.md)
- [22 —— 目标侧](22-target-side.md)
- [23 —— 项目环境](23-the-project-environment.md)
- [24 —— 基于 openkal 的交叉构建](24-openkal-cross.md)

### 3x —— 裸机、设备与加速器

- [30 —— 裸机与 freestanding 目标](30-baremetal.md)
- [31 —— 抵达一台设备](31-devices.md)
- [32 —— 异构硬件构建](32-heterogeneous-builds.md)

### 4x —— 从外部扩展 mcpp

- [40 —— 编写规则包](40-authoring-a-rule-package.md)

### 5x —— 机器接口与兼容性

- [50 —— 机器可读输出](50-machine-output.md)
- [51 —— 受支持的版本与兼容性](51-supported-versions.md)

### 9x —— 为 mcpp 本身做贡献

- [90 —— 从源码构建与参与贡献](90-build-from-source.md)
- [91 —— 工具链机制内幕](91-toolchain-internals.md)
- [92 —— 发布 mcpp](92-release.md)

## 规范文档

规范性文档 —— 语义、约束与匹配规则,每条规则标注其实现状态。
面向索引作者、贡献者与下游工具。

- [specs/](../specs/README.md) —— 全部规范的索引
  - [SPEC-001 —— 包身份、`[dependencies]` 选择器与匹配](../specs/package-identity.md)
  - [SPEC-002 —— 目标侧模型:保留命名空间、五层、三条规则](../specs/target-side.md)
  - [SPEC-003 —— 退出码契约](../specs/exit-codes.md)
  - [SPEC-004 —— `mcpp.toml` 的平面划分、条件化形状、解析轴与命名规约](../specs/manifest-semantics.md)
