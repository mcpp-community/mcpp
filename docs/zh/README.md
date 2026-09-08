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
| 写一个程序 | [01](01-getting-started.md)、[04](04-mcpp-toml.md) §1 | [`01-hello`](../../examples/01-hello/)、[`02-with-deps`](../../examples/02-with-deps/) |
| 写一个供他人 import 的库 | [11](11-publishing-a-library.md)、[06](06-features-and-capabilities.md)、[04](04-mcpp-toml.md) §2.4 | [`04-workspace`](../../examples/04-workspace/)、[`11-features`](../../examples/11-features/) |
| 发布它 | [10](10-pack-and-release.md)、[11](11-publishing-a-library.md)、[12](12-binary-distribution.md) | [`03-pack-static`](../../examples/03-pack-static/)、[`05-lib-distribution`](../../examples/05-lib-distribution/) |
| 为另一台机器构建 | [21](21-the-target-triple.md)、[24](24-openkal-cross.md)、[40](40-baremetal.md) | [`06-openkal-cross`](../../examples/06-openkal-cross/)、`mcpp new … --template riscv-virt-rt` |
| 使用 GPU 或加速器 | [42](42-heterogeneous-builds.md)、[41](41-devices.md) | [`09-heterogeneous`](../../examples/09-heterogeneous/),从 [`boundary/`](../../examples/09-heterogeneous/boundary/) 开始 |
| 增加一条规则、一种语言或一个生成器 | [31](31-authoring-a-rule-package.md)、[30](30-build-mcpp.md) | [`08-build-rules`](../../examples/08-build-rules/)、[`12-a-new-device-language`](../../examples/12-a-new-device-language/) |
| 向索引添加一个包 | [11](11-publishing-a-library.md)、[SPEC-001](../specs/package-identity.md) | [09](09-commands-by-scenario.md) —— 发布相关场景 |
| 为别人打包一个工具、一个驱动或一块板子 | [32](32-authoring-a-payload.md)、[33](33-authoring-an-adapter.md)、[34](34-authoring-a-bsp.md) | `xim-pkgindex` 与 `mcpp-index` 里的描述符 |
| 修改 mcpp 本身 | [90](90-build-from-source.md)、[92](92-release.md)、[51](51-supported-versions.md) | — |

课程也可以以**项目模板**的形式到达:模板由包提供,`mcpp new --template` 实例化
它。今天有文档的两个是 `riscv-virt-rt`(裸机)与 `ocornut.imgui`(图形应用),
用到它的章节会点名。

## 章节

首位数字就是部分,所以编号本身说明一章属于哪里:

| | |
|---|---|
| `0x` | 基础 |
| `1x` | 发布 |
| `2x` | 工具链与目标 |
| `3x` | 扩展 mcpp 与它的生态 |
| `4x` | 设备与加速器 |
| `5x` | 程序可以解析的契约 |
| `9x` | mcpp 自身 |

同一部分内部的排列是阅读顺序,不是字母序。

### 0x —— 基础

- [00 —— mcpp 的运转方式](00-how-mcpp-works.md) —— 其余每章都假定的模型
- [01 —— 快速开始](01-getting-started.md) —— 安装、创建、构建、运行
- [02 —— 场景](02-scenarios.md) —— mcpp 被用来做什么,以及每一类工作会用到它的哪些功能
- [03 —— 示例项目](03-examples.md) —— 哪个示例教什么
- [04 —— mcpp.toml 工程文件指南](04-mcpp-toml.md) —— manifest 可以说什么
- [05 —— 依赖与解析](05-dependencies.md) —— 依赖从哪里来,以及哪个版本胜出
- [06 —— Feature 与能力](06-features-and-capabilities.md) —— 让包的一部分成为可选
- [07 —— 工作空间](07-workspace.md) —— 多个包,一次构建
- [08 —— 测试](08-testing.md) —— 包括在本机跑不了的那些
- [09 —— 按场景选命令](09-commands-by-scenario.md) —— 认识名词之后的查阅入口

### 1x —— 发布

- [10 —— 发布打包](10-pack-and-release.md)
- [11 —— 发布一个库到 mcpp-index](11-publishing-a-library.md)
- [12 —— 分发预编译库](12-binary-distribution.md)

### 2x —— 工具链与目标

- [20 —— 工具链管理](20-toolchains.md)
- [21 —— 目标三元组](21-the-target-triple.md)
- [22 —— 目标侧](22-target-side.md)
- [23 —— 项目环境](23-the-project-environment.md)
- [24 —— 基于 openkal 的交叉构建](24-openkal-cross.md)

### 3x —— 扩展 mcpp 与它的生态

- [30 —— 构建程序:`build.mcpp`](30-build-mcpp.md) —— 工程需要一步 mcpp 没有规则的工作
- [31 —— 编写规则包](31-authoring-a-rule-package.md) —— 把那一步打包给别的工程用
- [32 —— 编写一个载荷](32-authoring-a-payload.md) —— 由 mcpp 安装的工具或预编译库
- [33 —— 编写运行时适配包](33-authoring-an-adapter.md) —— 够到宿主提供的库
- [34 —— 编写板级支持包](34-authoring-a-bsp.md) —— 一块板子,以及抵达它的方式

### 4x —— 设备与加速器

- [40 —— 裸机与 freestanding 目标](40-baremetal.md)
- [41 —— 抵达一台设备](41-devices.md)
- [42 —— 异构硬件构建](42-heterogeneous-builds.md)

### 5x —— 面向程序的契约

- [50 —— 机器可读输出](50-machine-output.md)
- [51 —— 受支持的版本与兼容性](51-supported-versions.md)

### 9x —— mcpp 自身

- [90 —— 从源码构建与参与贡献](90-build-from-source.md)
- [91 —— 工具链机制内幕](91-toolchain-internals.md)
- [92 —— 发布 mcpp](92-release.md)

## 反查

上面的章节表是**阅读顺序**。这里是另一种索引:从读者眼前的一个记号,查到拥有它的
那一章。

**manifest 的表与键**

| | 章节 | | 章节 |
|---|---|---|---|
| `[package]`、`[targets.<n>]`、`[build]`、`[lib]` | [04](04-mcpp-toml.md) | `[profile.<n>]`、`[resources]`、`[runtime]` | [04](04-mcpp-toml.md) |
| `[dependencies]`、`[dev-dependencies]`、`[build-dependencies]` | [05](05-dependencies.md) | `scan_overrides`、`module_extensions` | [04](04-mcpp-toml.md) |
| `[features]`、`[feature-deps.<f>]`、`provides` / `requires` | [06](06-features-and-capabilities.md) | `[workspace]` | [07](07-workspace.md) |
| `[toolchain]`、`cxx_runtime` | [20](20-toolchains.md) | `[target.<sel>]`、`cfg(…)` | [22](22-target-side.md) |
| `[xlings]`、`[xlings.workspace]`、`[feature-xlings.<f>]` | [23](23-the-project-environment.md) | `[pack]` | [10](10-pack-and-release.md) |
| `[build] accel`、`[package] accelerators`、`device_extensions` | [42](42-heterogeneous-builds.md) | `[hooks]` | [09](09-commands-by-scenario.md) |
| `[package] platforms`、`[build] cache` | [04](04-mcpp-toml.md) | `[targets.<name>]`、`[profile.<name>]` | [04](04-mcpp-toml.md) |
| `runner`、`[target.<t>.runners]` | [41](41-devices.md) | `rule_module` | [31](31-authoring-a-rule-package.md) |

**命令**

| | 章节 | | 章节 |
|---|---|---|---|
| `build`、`run` | [01](01-getting-started.md) | `test` | [08](08-testing.md) |
| `new`、`new --template` | [01](01-getting-started.md) | `add`、`update`、`why` | [05](05-dependencies.md) |
| `pack` | [10](10-pack-and-release.md) | `publish`、`emit xpkg`、`xpkg parse` | [11](11-publishing-a-library.md) |
| `toolchain` | [20](20-toolchains.md) | `clean`、`cache`、`index`、`self …` | [09](09-commands-by-scenario.md) |

**概念**

| | 章节 | | 章节 |
|---|---|---|---|
| 五个名词;一次构建做了什么 | [00](00-how-mcpp-works.md) | `import std`、模块接口、BMI | [00](00-how-mcpp-works.md)、[20](20-toolchains.md) |
| `mcpp::action`、构建程序 | [30](30-build-mcpp.md) | 规则包、`MCPP_EXPORT_C` | [31](31-authoring-a-rule-package.md) |
| 目标三元组、支持矩阵 | [21](21-the-target-triple.md) | runner、具名 runner | [41](41-devices.md) |
| 岛、接缝、`accel` | [42](42-heterogeneous-builds.md) | 描述符、索引 | [11](11-publishing-a-library.md) |
| ABI tag、预建产物 | [12](12-binary-distribution.md) | 退出码、JSON 输出 | [50](50-machine-output.md) |
| `xim:` 载荷、`[xlings.workspace]` | [32](32-authoring-a-payload.md)、[23](23-the-project-environment.md) | `compat:` 适配包、`runtime.library_dirs` | [33](33-authoring-an-adapter.md) |

## 规范文档

规范性文档 —— 语义、约束与匹配规则,每条规则标注其实现状态。
面向索引作者、贡献者与下游工具。

- [specs/](../specs/README.md) —— 全部规范的索引
  - [SPEC-001 —— 包身份、`[dependencies]` 选择器与匹配](../specs/package-identity.md)
  - [SPEC-002 —— 目标侧模型:保留命名空间、五层、三条规则](../specs/target-side.md)
  - [SPEC-003 —— 退出码契约](../specs/exit-codes.md)
  - [SPEC-004 —— `mcpp.toml` 的平面划分、条件化形状、解析轴与命名规约](../specs/manifest-semantics.md)
