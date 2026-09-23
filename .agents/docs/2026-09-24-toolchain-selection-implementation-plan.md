---
subject: plan
status: landed
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

## 4. 执行记录

| 编号 | 结果 |
|---|---|
| T1、T2 | 子代理在 `mcpp-685` 完成，合入主线分支；e2e `746` 与 `test_doctor_fixincludes` 在撤回修复后变红 |
| T3 到 T8 | 主线在 `mcpp-tcsel` 完成，mcpp#688 |
| T9 | openxlings/xim-pkgindex#870 已合入 |
| T10 | mcpp#688 以 b4824697 合入。Windows 上前三轮编译失败（`optional<std::string>` 成员），第四轮编译通过后 `import std` 失败（`std.compat`），原因与修法见设计文档 §11；最终 40 项通过、1 项跳过，失败的两项是 macOS xcode-27，与 main 上同因（mcpp#669）。合入后 main 上同一提交的结果相同 |
| T11 | release run 35924420072 全绿。四个归档与 `.sha256` 在各自出现后由本地 gtc 传到 GitCode，GET 读回 200 且大小一致；与 GitHub release 逐字节比较 8/8 相同，四个归档与 sidecar 哈希一致。索引 bump openxlings/xim-pkgindex#871 的四个哈希与下载的归档逐个对应到平台槽位，CI 17 项通过后以 4440c503 合入；`latest` 恰为三行且都指向 2026.9.24.1；索引指针的 `index_version` 为 4440c50。AUR（`2026.9.24.1-1`）、Homebrew（`version "2026.9.24.1"`）、PyPI（`mcpp-bin 2026.9.24.1`）均已跟上 |
| T12 | SubOS `v924`，`--sandbox`，xlings 与 mcpp 都配 CN 镜像。沙箱里 `xlings update` 第一次即得到 `latest -> 2026.9.24.1`，`xlings install mcpp@2026.9.24.1` 从 CN 镜像安装。同一份脚本（`2026-09-24-toolchain-selection-verify.sh`）对 2026.9.24.1 为 fails=0（21 项通过）；对 2026.9.21.3 为 fails=6，恰为六条 CHANGE 断言（D 一条、E 三条、F 两条），GUARD 在两个版本上都通过。旧版本上的读数：三元组为 `arm64-apple-macos14.0`，改值后构建目录数不变，`foo:gcc@16.1.0` 被接受，`xim:msvc@system` 报「only available on Windows hosts」。MSVC toolset 的选择只能在 Windows 上测，列为 NOT RUN，由 Windows CI 的 e2e 760（VS 2026，MSVC 14.51.36231）与 239 覆盖 |

发布前补做的两次测量：

- **旧引擎读新写法**（设计 §3.8）。用已发布的 2026.9.21.3 在 Windows runner 上构建四份清单：`msvc@system`、`msvc@<toolset>` 让整份清单被拒；`xim:msvc@<toolset>` 被接受而不生效，构建输出却把它列为 c-abi 层。写入 CHANGELOG 与 `docs/20`。
- **沙箱判据的基线**。验证脚本先对 2026.9.21.3 运行：F 段原先断言 `xim:gcc@16.1.0` 能构建，在旧版本上同样通过，因为旧引擎会剥掉任何 `<ns>:` 前缀。F 段改为断言真正变化的两点（其他命名空间被拒；`xim:msvc@system` 作为写法被拒），CHANGELOG 与文档里「`xim:` 对所有族都接受」的表述也改为说明这一点早已成立、变化的是只接受 `xim:`。
- **E 段的判据读错了文件**。它在 `target/` 下找 `compile_commands.json`，而 mcpp 把它写在工程根目录，于是在两个版本上都找不到文件而失败；旧版本上的这个失败曾被读成「CHANGE 段按预期失败」。新版本的运行暴露了它（构建输出自己写着 `→ arm64-apple-macos11.0`）。E 段改为每次 configure 之后立即读工程根目录的文件，并以构建目录数检查指纹；上表 T12 的两组读数都来自修正后的脚本。
