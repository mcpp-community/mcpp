# compile_commands.json 测试覆盖缺失：分析报告与设计方案

> mcpp 0.0.62/0.0.63 — tests/ 目录无代码提示根因分析 + 修复方案
>
> 关联：[2026-05-12-compile-commands-design.md](2026-05-12-compile-commands-design.md)（cdb 初始设计）
>
> **状态：已定稿并实现**（采用「合并保留」方案，见 §6 / §8 决策备注）。

## 1. 现象

在一个含 `[dev-dependencies] gtest` 的项目里（如 `helloworld`），编辑
`tests/test_smoke.cpp` 时 clangd **无任何代码提示**：`gtest::InitGoogleTest()`、
`import mcpplibs.cmdline`、`import std;` 全部报红、无补全、无跳转。

用户初判：「是不是生成的 `compile_commands.json` 没有包含 dev-dependencies 下的库？」

## 2. 结论（TL;DR）

`compile_commands.json` 只反映**当前这一次构建命令的 `BuildPlan`**。

- `mcpp build` / `mcpp run` 的 plan **不含** `tests/**/*.cpp`，也**不解析
  dev-dependencies**。
- 只有 `mcpp test` 的 plan 才把 tests + dev-deps 纳入。
- 两个命令写**同一个** `<projectRoot>/compile_commands.json`，**后写覆盖前写**。

日常「编辑 → `mcpp build`」循环里，cdb 几乎总是被 `mcpp build` 写成「无 tests」
版本 → clangd 找不到测试文件的编译条目 → 退化为无标志猜测 → tests/ 全无提示。

用户的判断**基本正确**：缺的直接原因是 cdb 里没有该测试文件的条目；而这个条目
恰恰是 gtest 等 dev-dependency `-I` 头目录的**唯一载体**——「缺测试文件」与
「缺 dev-deps」是同一个根因。

## 3. 复现证据

在 `/home/speak/test/mcpp/helloworld` 实测，cdb 中出现的 `file`：

| 命令 | cdb 中的文件 |
|---|---|
| `mcpp build` | 3 个依赖 cppm + `src/main.cpp`（**无 tests**）|
| `mcpp test` | 上述 + `tests/test_smoke.cpp` + `gtest-all.cc` / `gtest_main.cc`（**完整**）|
| `mcpp build`（改 `src/main.cpp` 后真实重建）| 又回退成**无 tests** |

并确认：`mcpp test` 写出的 `test_smoke.cpp` 条目**带全了**
`-I.../gtest/include`、`-std=c++23`、`std.pcm`、`-fprebuilt-module-path` ——
只要这条条目在，补全就完全正常。

一个反直觉但已查清的细节：连续多次 `mcpp build`（全缓存、0.00s）**不会**回退
cdb —— 因为 P0 fast-path（`try_fast_build`，`execute.cppm:269`）在 `build.ninja`
新鲜时**整体跳过后端**，根本不重写 cdb。但只要发生一次真实重建，`mcpp build`
就用无 tests 的 plan 覆盖它。这正是「有时有提示、改完代码后又没了」的来源。

## 4. 根因链（代码定位）

1. `src/build/compile_commands.cppm` — cdb 完全由
   `for (auto& cu : plan.compileUnits)` 生成，**等于当次 plan 的镜像**。
2. `src/build/ninja_backend.cppm:693` — `backend->build()` 内部调用
   `write_compile_commands(plan, flags)`，写到 `plan.projectRoot/compile_commands.json`。
3. `src/build/execute.cppm:377` — `mcpp build` 走
   `prepare_build(includeDevDeps=false, extraTargets={})` → plan 无 tests、无 dev-deps。
4. `src/build/execute.cppm:456` — `mcpp test` 走
   `prepare_build(includeDevDeps=true, testTargets=...)` → plan 含 tests + dev-deps。
5. 两条路径写**同一个文件** → 互相覆盖；`mcpp build` 频率远高 → cdb 长期处于
   「无 tests」态。
6. `write_compile_commands` 现为**全量覆盖**（仅做 content 相等短路），不保留既有条目。

## 5. 两条关键架构约束（决定了方案走向）

实现过程中发现两点，直接否决了「让 `mcpp build` 主动生成测试 cdb」的朴素思路：

### 5.1 `inTestMode` 互斥（`plan.cppm:460`）
存在 `TestBinary` 目标时，整张 plan **互斥地只构建测试二进制**，跳过常规
`Binary`/`Library`——否则把 gtest 的 `main`（`gtest_main.cc`）拉进项目常规 bin
会 `multiple definition of 'main'`。即「一张 plan 同时构建普通 bin + 可链接的
测试单元」在当前架构下不成立。

### 5.2 offline-first 是本仓库的硬架构约束
`prepare_build(includeDevDeps=true)` 注释明确「dev-deps are also **fetched** +
scanned」。让 `mcpp build` 为了 IDE 去解析 dev-deps，会在 gtest 未安装时**触发
下载**，**违背 offline-first**（仓库多处 offline-first 注释），并坑了「只想
build、不想 test」的用户——既改了 build-only 行为，又凭空拉包。

### 5.3 一个使纯方案可行又使其无必要的事实：build/test 共用输出目录
`fingerprint.dependencyLockHash = ""`（M2 未实现，`prepare.cppm:2234`），且
`compileFlags` 不含 dev-deps → **dev-deps 不影响 fingerprint** → `mcpp build`
与 `mcpp test` 必然落在**同一个** `target/<triple>/<fp>/` 目录。

推论：测试文件补全本就**必须**先跑一次 `mcpp test`/install——`#include
<gtest/gtest.h>` 需要 gtest **已安装**（装包只能由 test/install 做），
`import std` / `import mcpplibs.cmdline` 需要 BMI **已构建**。三者只有 `mcpp test`
能一次备齐，且它们都落在共享目录里。所以「无网络、没跑过 test 就白嫖测试补全」
物理上不可能——这恰恰说明把「拉依赖」塞进 `mcpp build` 既不对也没必要。

## 6. 方案：offline-first 的 cdb「合并保留」（已实现）

**核心**：`mcpp test` 已经能写出完整且正确的 cdb（测试条目 flag 有效、其引用的
BMI 因共用目录而真实存在）。bug 的本质是 **`mcpp build` 会摧毁它**。因此最小且
合理的修复是：让 `mcpp build` **停止摧毁**，而不是让它去重新生成。

`write_compile_commands` 从「全量覆盖」改为「**合并**」：

- 当前 plan 覆盖的文件 → 用本次条目（权威、最新 flag）；
- 当前 plan **未**覆盖、但**文件仍存在于磁盘**的旧条目（即上次 `mcpp test`
  写入的 `tests/*.cpp`、gtest 源）→ **保留**；
- 文件已不存在的旧条目 → **剪除**（cdb 不积累死引用）。
- 保留 fresh 顺序，旧条目追加其后（确定性、最小 churn）；内容不变则不重写
  （不触发 clangd 重索引）。
- 旧 cdb 解析失败 → 回退为纯 fresh（永不因坏文件破坏生成）。

`mcpp build` 自身**零改动**：不解析、不下载任何 dev-deps，不动构建图。对
build-only 用户行为与今天**完全一致**；只是不再把 `mcpp test` 的成果擦掉。

### 6.1 实现位置
- `src/build/compile_commands.cppm`
  - 新增纯函数 `merge_compile_commands(fresh, existing, fileExists)`：可注入
    `fileExists` 谓词，无需真实 FS，完全可单测；用 nlohmann 非抛出解析。
  - `write_compile_commands`：读现有 cdb → `merge_compile_commands(...)` →
    内容变化才写。
- 其余文件（`plan.cppm` / `prepare.cppm` / `ninja_backend.cppm` /
  `execute.cppm`）**均无改动**。构建图 100% 不变 → 跨平台零回归。

### 6.2 测试
- 单测 `tests/unit/test_compile_commands.cpp`（TDD，先 RED 后 GREEN）：
  保留未覆盖条目 / 剪除已删文件 / fresh 胜出且不重复 / 坏 JSON 回退。
- e2e `tests/e2e/77_cdb_preserves_test_entries.sh`：`mcpp test` 后**真实重建**
  `mcpp build`（改 main.cpp 破 fast-path），断言测试条目存活 + src 仍在 +
  删除测试文件后条目被剪除。无 `requires:`，三平台均跑（对标 15/16/17）。

### 6.3 用户使用语义
- 新建/已有项目：跑一次 `mcpp test`（你本来就要跑来测试，且它会装 gtest +
  构建 BMI）→ cdb 完整、tests/ 有补全；
- 此后任意 `mcpp build`（含真实重建）**保留**该补全，不再回退；
- 只想 build 的用户：完全不受影响，`mcpp build` 不拉任何 test 依赖。

## 7. 被否决的备选方案

| 方案 | 否决理由 |
|---|---|
| **纯方案 B**：`mcpp build` 主动解析+拉 dev-deps 写测试条目 | 违背 offline-first；改 build-only 行为；凭空拉包；且因 §5.3 本就需先 `mcpp test`，收益为零。用户明确反对。 |
| **plan 携带 ideOnly 测试单元 + ninja default 排除** | 受 §5.1 `inTestMode` 互斥牵连，需深改 `make_plan`/解析器（2400 行），破坏构建图的风险高，违背「不破坏架构合理性」。 |
| **双趟 `prepare_build`（build 后再算一遍测试 plan 喂 cdb）** | 仍要解析 dev-deps（offline-first 问题不变）；且 build 规划成本翻倍。 |
| **独立 `mcpp cdb` 命令** | 多一个命令面；仍需 dev-deps 已装；合并方案已用更小代价覆盖同一目标。 |

## 8. 决策备注（为什么最终选「合并保留」）

1. **尊重既有架构而非对抗它**：offline-first（§5.2）与 `inTestMode` 互斥
   （§5.1）是仓库既定架构。合并方案不触碰二者；其余方案都要么违背 offline-first，
   要么深改构建核心。用户指令「方案 B 必须综合考虑架构，不要只为完成目标破坏
   合理性」——遵循该指令的正解就是**放弃纯 B 的实现形态、保留 B 的目标**
   （cdb 覆盖完整编译面），用 offline-first 的合并达成。
2. **诚实的投影语义**：合并后的 cdb 仍只由**真实 plan**构成（本次 build plan ∪
   上次 test plan 的存活条目），不凭空捏造条目，不指向不存在的 BMI（§5.3 保证
   存活条目的 BMI 真实在共享目录里）。
3. **「先跑一次 test」不是缺陷而是内在事实**（§5.3）：任何方案都绕不开它，所以
   合并方案相比纯 B **无实际功能损失**，却换来 offline-first + build-only 零影响 +
   构建图零风险 + 极小改动面。
4. **可逆与可演进**：若将来 M2 lockfile 让 build/test 目录分叉（§5.3 前提失效），
   届时存活条目的 BMI 路径可能失效；那是 follow-up，应在 lockfile 设计里保证
   「非测试构建的 fingerprint 不被 dev-deps 扰动」，与本方案正交。
