---
subject: plan
status: active
---

# 工具链选择与载荷可信度：实施计划

- 日期：2026-09-24
- 设计：`2026-09-24-toolchain-selection-and-payload-trust-design.md`（v2）
- 规范：`docs/specs/toolchain-management.md`（SPEC-006）
- 维护者对设计 §10 的答复：
  - `sysroot` 语义接受；
  - `xim:` 前缀对所有工具链族都接受；
  - SPEC-006 保持一份，不拆分。

## 1. 约束

| 角度 | 约束 |
|---|---|
| 架构 | MSVC 的选择只在 prepare 里做一次，结果存进 `Toolchain`，所有读者都读它。选择逻辑是纯函数，只有枚举候选依赖 Windows。deployment target 的解析函数以目标三元组为参数，调用方无法再按宿主判定 |
| 稳定性 | 不写版本时，默认结果与今天一致，例外只有设计 §3.9 列出的几种（每一种都更好）。任何回落都打印一行说明 |
| 简洁 | 不新增清单键；clang 行复用 `[target.<triple>].sysroot`。三组 `-Xmicrosoft-*` 参数由 `LinkModel` 这一个地方产生 |
| 用户体验 | 构建开头打印一行，说明用的是哪个 toolset、来自哪里、SDK 版本。找不到时列出三类候选。写了版本而环境变量被忽略时，打印说明 |
| 兼容性 | `msvc@system`、`msvc@<版本>`、`gcc@…`、`llvm@…` 这些写法都继续有效。`msvc@<版本>` 在机器上有同版本时改用系统的那一份，这一点写进 CHANGELOG。缓存键的变化只影响 Windows clang 行 |
| 跨平台 | 选择逻辑、写法解析、清单解析的单测都在 Linux 上运行；Windows 行为由 Windows CI 的 e2e 覆盖。#685 的修正对 macOS 宿主保持原有输出 |
| 一致性 | `cl.exe` 行与 clang 行共用同一个选择函数。「最高」统一按数字元组比较 |
| 无感升级 | 已有项目不需要改清单。新写法在旧引擎上是响亮的失败，不会被静默忽略；文档写明所需的最低版本 |
| 测试覆盖 | 每个判据都配反向用例（撤回修复就变红）。纯函数单测覆盖边界情形；e2e 覆盖真实二进制 |

## 2. 任务

| 编号 | 仓库 | 内容 | 主要文件 | 依赖 |
|---|---|---|---|---|
| T1 | mcpp | #685：deployment target 与宿主无关，是否适用按目标判定 | `modules/platform/src/macos/macos.cppm`、`src/build/prepare.cppm`（deployment target 相关位置）、`src/build/prepare_inputs.cppm`、`src/toolchain/hostflags.cppm`（`-mmacosx-version-min`）、`src/toolchain/stdmod.cppm`、`src/build/flags.cppm`、`src/build/build_program.cppm`、对应单测 | 无 |
| T2 | mcpp | #687 的 doctor 诊断：已安装 gcc 的 `include-fixed` 里带横幅的文件 | `src/doctor.cppm`、单测 | 无 |
| T3 | mcpp | 工具链写法的 `xim:` 前缀（所有族） | `src/toolchain/registry.cppm`、`src/toolchain/compat.cppm`、单测 | 无 |
| T4 | mcpp | MSVC 候选枚举与纯函数 `select_toolset()`；`cl.exe` 行的 `msvc@system` 与带版本的写法改用它 | `src/toolchain/msvc.cppm`、`src/build/prepare.cppm`（工具链解析段）、单测 | T3 |
| T5 | mcpp | clang 行的 MSVC sysroot：清单解析、在 prepare 中解析、`Toolchain` 字段、`-Xmicrosoft-*` token、`std.ixx`、`stdlibVersion`、链接环境、`resolution.json`、构建开头那一行 | `modules/manifest/src/toml.cppm`、`modules/toolchain-model/src/model.cppm`、`linkmodel.cppm`、`src/toolchain/hostflags.cppm`（链接 token）、`src/toolchain/clang.cppm`、`src/build/flags.cppm`（链接一侧与含空格路径的转义）、`src/build/cache_key.cppm`、`src/build/prepare.cppm` | T4 |
| T6 | mcpp | `mcpp toolchain list` 在 Windows 上列出机器上的 toolset | `src/toolchain/lifecycle.cppm` | T4 |
| T7 | mcpp | Windows e2e：多版本、`xim:`、环境变量被忽略、`std.ixx` 与头文件同源 | `tests/e2e/`、`.github/workflows/ci-windows*.yml` | T5 |
| T8 | mcpp | 文档：`docs/20`、`docs/22`、`docs/04` 及 `docs/zh/` 对应译文；SPEC-006 的实现状态；CHANGELOG；版本号 | 文档与版本文件 | T1 到 T7 |
| T9 | xim-pkgindex | `gcc.lua` 清理代码的引号修正与日志；`tests/g/test_gcc.py` 断言 | `pkgs/g/gcc.lua`、`tests/g/test_gcc.py` | 无 |
| T10 | — | 两个 PR 的 CI 全绿后合入：xim-pkgindex 先合，mcpp 后合 | — | T1 到 T9 |
| T11 | mcpp、xlings-res、xim-pkgindex | 发布：打 tag、四平台构建、本地 gtc 补 GitCode、核验镜像、合入 bot 的索引 PR、在 PR 里前移 bootstrap pin | — | T10 |
| T12 | — | 生态验证：在 xlings subos 沙箱里（mcpp 与 xlings 都配 CN 镜像）装已发布的 mcpp，逐项验证 #685、#687 以及 mcpp-index 的若干包 | — | T11 |

并行方式：

- T1 与 T2 交给一个子代理，在 worktree `mcpp-685` 里做；
- T9 交给另一个子代理，在 xim-pkgindex 的 worktree 里做；
- T3 到 T8 由主线在 `mcpp-tcsel` 里做；
- T1、T2 做完后合入主线分支，形成一个 PR。

文件归属按上表划分。`src/build/prepare.cppm` 由两边在互不重叠的区段修改：T1 只动 deployment target 相关的函数与调用处，T4、T5 只动工具链解析段与 `resolution.json`。

## 3. 判据

| 编号 | 判据 | 反向 |
|---|---|---|
| T1 | 在 Linux 宿主上，`aarch64-macos` 目标的 `build.ninja` 带有 manifest 里写的版本；改值后指纹变化；env 优先于 manifest | macOS 宿主构建非 Apple 目标时不出现 `-mmacosx-version-min` |
| T2 | 15.1.0 载荷副本触发诊断 | 16.1.0 不触发 |
| T3 | `xim:gcc@16.1.0` 与 `gcc@16.1.0` 解析结果相同；`xim:msvc@system` 被拒绝 | — |
| T4 | 纯函数单测覆盖设计 §3.10 列出的全部用例 | 去掉「先匹配系统」后，对应用例变红 |
| T5 | 单测：MSVC 目标的 link model 产出三组 token，并进入缓存键；Windows CI 构建一个 `import std` 程序 | MinGW 目标的 token 为空 |
| T7 | Windows e2e 的五条 | — |
| T9 | 安装后 `include-fixed` 里没有带横幅的文件 | 撤回修正后断言变红 |
| T12 | 沙箱里所有检查通过，并把没有运行的检查单独列出 | — |
