---
subject: toolchain
status: active
---

# LLVM 23.1.3 Part 2：任务依赖与生态交付记录

本记录落实 [Part 2 方案](2026-10-08-llvm-2313-linux-aarch64-ecosystem-part2-design.md)。
状态只根据代码、构建进程、CI、发布资源和真实消费结果更新。未执行的门不计为完成。
维护者已授权实现、每仓库集中 PR、CI 修复、发布、CN 镜像补传、SubOS 实测及已完成 issue 的关闭。

## 1. 架构边界与任务依赖

```mermaid
flowchart TD
    A[当前事实与问题复现] --> B[原生 ARM64 配套资源构建]
    A --> C[xlings 元数据架构上下文修复]
    A --> D[mcpp 引擎与 review 缺口修正]
    B --> E[LLVM ARM64 carve 与准入]
    C --> F[xlings 三平台 CI与发布]
    F --> G[索引客户端与架构资源接线]
    E --> G
    G --> H[索引双镜像和原生消费 CI]
    H --> I[mcpp 全矩阵与 openkal 联测]
    D --> I
    I --> J[生态自审与合入]
    J --> K[mcpp 发布与索引传播]
    K --> L[CN SubOS与生态闭环审计]
```

LLVM 与 glibc 是原生 aarch64 的新默认组合；x86_64 保持 GCC，显式旧配置保留。
资源选择按客户端进程 ABI，不能以硬件架构替代；这保持 Rosetta、WOW64 和 Linux emulation 的一致性。
同一 glibc loader 与核心库来源绑定是稳定性门。libc++ 与 libstdc++ 的 C++ ABI 不混用。

## 2. 仓库交付与责任

| 任务 | 仓库与跟踪 | 依赖 | 验收依据 | 当前状态 |
|---|---|---|---|---|
| T1 review 修正与引擎默认 | [mcpp #784](https://github.com/mcpp-community/mcpp/issues/784)，沿用 #781 | T4 公开资源后完整 CI | 冷 home、默认双轴、GNU native、自举、modules、Windows 诊断 | 实现中 |
| T2 ARM64 配套与 carve | [xim-pkgindex #937](https://github.com/openxlings/xim-pkgindex/issues/937) | 原生构建机 | 架构、来源摘要、loader/runtime、编译运行 | 原生构建已启动 |
| T3 配方元数据上下文 | [xlings #646](https://github.com/openxlings/xlings/issues/646) | 现有 libxpkg LoaderContext | metadata 与 install 相同架构；三平台回归 | 已复现并实现，构建中 |
| T4 索引接线与镜像 | [xim-pkgindex #938](https://github.com/openxlings/xim-pkgindex/pull/938) | T2、T3 发布 | 每架构哈希、旧版本拒绝、双镜像 GET、消费门 | draft |
| T5 原生与生态消费 | mcpp、xlings、mcpp-index、openkal | T1、T4 | 原生 ARM64、CN SubOS sandbox、真实 build/test/run/pack | 待资源与客户端就绪 |
| T6 自审与发布 | mcpp、xlings、资源与索引 | T1–T5 | 最终头 CI、发版产物、指针传播、消费审计 | 待前置门 |

单个仓库的实现集中在一个 PR：mcpp 沿用 #781，索引使用 #938；xlings 因实测发现客户端缺陷而新增必要 PR。
发布版本同时纳入相应实现 PR。发布后生成的资源索引更新依赖已发布摘要，若不能纳入尚未合入的索引 PR，
使用必要的自动索引收尾 PR；bootstrap pin 也只在资源已公开且索引传播后前移。该依赖不能通过提前填写未来版本规避。

## 3. 多角度验收

| 角度 | 约束 | 可验证证据 |
|---|---|---|
| 架构 | runtime、target、client ABI 分别命名 | ELF、LoaderContext、GNU/musl target 与解析报告 |
| 稳定性 | loader 与 libc 同源、私有共享库闭包 | NEEDED、INTERP、RUNPATH、运行与 relocation 探针 |
| 简洁性 | 使用现有 host row、两包 carve、统一 recipe loader | 无新 toolchain 语法、无第二套资源解析器 |
| 用户体验 | 新安装不要求手工装配依赖 | new/build/run 冷安装及诊断 |
| 兼容性 | 旧配置与历史版本保持事实边界 | 旧 LLVM 在 ARM64 不误报可装，旧 x86 recipe 不回归 |
| 跨平台 | 只扩展已发布架构，不扩大 GNU cross 承诺 | 各宿主矩阵与具名 refusal |
| 一致性 | 元数据读取与 install 使用同一上下文 | live recipe、catalog、安装对照测试 |
| 升级 | 保留声明，说明双轴差异，不悄悄改 ABI | fresh 与 retained defaultTarget、prebuilt/BMI 失效检查 |
| 覆盖 | capability、分片和缓存前提必须可观察 | 按镜像完整分片，890 冷安装，candidate 精确钉 |
| 生态 | 索引与发布版客户端实际协作 | CN sandbox、mcpp-index、openkal 运行与 pack |

## 4. 已取得的证据

2026-10-08：发布版 xlings 2026.10.4.1 在隔离 XLINGS_HOME 中通过 add-xpkg 和 info
读取 `description = os.arch()` 的 live recipe，输出 `recipe architecture: unknown`。
代码核查确认 catalog 元数据读取省略 LoaderContext，install 则传入平台与进程架构。
T3 将两者统一；这项必要客户端修复应在新 ARM64 loader metadata 公开前发布。

索引原生资源构建：[run 37684733504](https://github.com/openxlings/xim-pkgindex/actions/runs/37684733504)，
ubuntu-24.04-arm，生成 UAPI、zlib、libxml2、gcc-runtime、glibc 和 LLVM 双包。
这是运行中的构建，尚无资源通过或发布结论。

本地测试按变更契约聚焦执行，避免用重复测试占据资源制作与集成时间。
最终闭环审计仍须逐项覆盖方案 G0–G9；局部通过不能替代整个生态已可用。
