---
name: mcpp-contributing
description: Use when contributing to the mcpp project — submitting bug fixes, new features, code optimizations, documentation improvements, or any PR. Covers issue creation, branch conventions, build verification, CI requirements, and PR workflow using gh and git.
---

# mcpp 项目开发贡献规范

## Overview

mcpp 项目的贡献流程：先创建 Issue → 切分支 → 实现改动 → 提交 PR → CI 通过 → Review 合入。

- 仓库：https://github.com/mcpp-community/mcpp
- 构建：`mcpp build`（C++23 模块自举）
- 测试：`mcpp test` 覆盖 `tests/**/*.cpp`，`tests/e2e/` 提供真实二进制的端到端脚本
- CI：GitHub Actions，base 为 `main` 的 PR 触发分平台构建、测试与 E2E 检查

## 核心原则

### 禁止直接 push main

**所有改动必须通过 PR 合入 main**，无论改动大小。这包括：
- 代码改动（feat / fix / refactor）
- 文档改动（docs / skills / .agents/）
- 配置改动（mcpp.toml / .xlings.json / CI workflow）
- 版本号 bump（必须通过 PR，不能直接 push）

**唯一例外**：紧急 hotfix 需要 `--admin` 合入，但也必须先创建 PR。

违规示例（不允许）：
```bash
# ✗ 直接 push 到 main
git commit -m "docs: add skill" && git push origin main

# ✗ 绕过分支保护
git push origin feature:main
```

正确做法：
```bash
# ✓ 始终走 PR 流程
git checkout -b docs/add-release-skill
git commit -m "docs: add release skill"
git push -u origin docs/add-release-skill
gh pr create --title "docs: add release skill" --body "..."
# 等 CI 通过后合入
```

## 贡献流程

### 1. 创建 Issue（必须）

所有贡献先创建 Issue，特别是新功能。避免重复工作，留下讨论记录。

**Bug 修复**

```bash
gh issue create \
  --title "fix: 简短描述" \
  --body "## 复现步骤
1. ...

## 期望行为
...

## 实际行为
...

## 环境
- mcpp 版本：\`mcpp --version\`
- OS："
```

**新功能**

```bash
gh issue create \
  --title "feat: 简短描述" \
  --body "## 动机
...

## 设计思路
...

## 涉及模块
..."
```

**代码优化**

```bash
gh issue create \
  --title "refactor: 简短描述" \
  --body "## 当前问题
...

## 优化方案
..."
```

### 2. 创建分支

**必须从最新 main 创建分支，禁止在 main 上直接开发。**

```bash
git checkout main && git pull origin main
git checkout -b <type>/<short-description>
# type: feat / fix / refactor / test / docs / chore
```

分支命名规范：
- `feat/xxx` — 新功能
- `fix/xxx` — Bug 修复
- `refactor/xxx` — 重构
- `test/xxx` — 测试
- `docs/xxx` — 文档
- `chore/xxx` — 版本 bump、配置、依赖更新

### 3. 实现改动

**开发要求**
- 遵循现有代码风格（查看相邻代码）
- 模块导入用 `import std;` 和 `import mcpp.xxx;`
- 只改需要改的，不顺手重构不相关代码
- 平台相关代码放在 `src/platform/` 目录下

**构建验证**

```bash
# 用现有 bootstrap mcpp 自举构建
mcpp build
# 选择刚生成的 target/**/bin/mcpp（Windows 为 mcpp.exe），不要硬编码宿主 triple
<fresh-mcpp-binary> --version
```

**测试**

```bash
# C++ 单元/集成测试：由刚构建的二进制发现 tests/**/*.cpp
<fresh-mcpp-binary> test
# 端到端测试：显式把刚构建的二进制交给脚本
# 路径必须是刚构建产物的绝对路径；Windows 使用 mcpp.exe。
MCPP=<absolute-path-to-fresh-mcpp-or-mcpp.exe> bash tests/e2e/01_help_and_version.sh
MCPP=<absolute-path-to-fresh-mcpp-or-mcpp.exe> bash tests/e2e/<relevant-test>.sh
# 新功能按变更契约补充 focused unit/integration 和/或 E2E 覆盖
```

E2E 并不保证完全离线：部分脚本需要工具链、索引或 capability provider。
按 CI 等价方式设置 `MCPP_HOME`、镜像和其他 capability 后再运行；不要让缓存命中
或空 workspace 选择冒充行为覆盖。

### 4. 提交 PR

**提交信息前缀**：`feat:` / `fix:` / `refactor:` / `test:` / `docs:` / `chore:`

```bash
git push -u origin <branch>
gh pr create \
  --title "<type>: 简短描述" \
  --body "## Summary
- 改动点

Closes #<issue>

## Test plan
- [ ] 文档-only：示例与链接已按当前实现复核，无运行时行为变更
- [ ] 涉及行为或测试文档时：`mcpp test`（unit/integration）通过
- [ ] 涉及行为或测试文档时：相关 E2E 脚本使用 fresh `MCPP` 通过"
```

**PR 要求**：
- title 用英文，body 中英文均可
- 关联 Issue（`Closes #N`）
- 包含 test plan
- 一个 PR 只做一件事，不混入无关改动

### 5. CI 必须通过

CI 不通过的 PR 不会被合入。

```bash
gh pr checks <pr-number>           # 查看状态
gh run view <run-id> --log-failed  # 查看失败日志
```

CI 由分平台的基础构建/单元集成检查与独立 E2E 检查组成：
| Workflow | 平台 | 内容 |
|----------|------|------|
| `ci-linux` / `ci-linux-e2e` | Linux x86_64 | 自举构建、unit/integration / 分片 E2E |
| `ci-macos` / `ci-macos-e2e` | macOS ARM64 | 自举构建、unit/integration / E2E |
| `ci-windows` / `ci-windows-e2e` | Windows x86_64 | 自举构建、toolchain 回归 / E2E |
| `cross-build-test` | Linux/Windows cross targets | 交叉构建、产物运行与 MinGW/Wine 检查 |
| `ci-aarch64-fresh-install` | Linux ARM64 native | path-filtered fresh install、原生自举与 musl `build.mcpp` host-helper 回归 |

**以 PR 实际 required checks 为准，所有未跳过的 required checks 必须通过。** 如果某个平台失败：
1. 下载日志分析原因
2. 修复后 push 到同一分支，CI 自动重跑
3. 如果是 flaky test，在 PR 中说明

### 6. Review & 合入

维护者 review → 反馈修改 → CI 重跑 → Merge（保留 commit 历史）。

合入方式：
- 默认使用 **Merge commit**（保留完整历史）
- 单 commit 的 PR 也可用 **Squash merge**

## Agent 开发规范

Agent（Claude Code 等）在执行任务时，**同样必须遵守 PR 流程**：

### Agent 必须做的
- 从最新 main 切新分支
- 所有改动通过 PR 提交
- 等 CI 通过后再请求合入
- 合入前先确认 PR 无冲突

### Agent 禁止做的
- 直接 push 到 main（即使有 admin 权限）
- 绕过 CI 检查合入
- 在已合入的分支上继续开发（应切新分支）
- 一个 PR 混入不相关的改动

### Agent 的典型工作流

```bash
# 1. 从最新 main 切分支
git checkout main && git pull origin main
git checkout -b <type>/<description>

# 2. 实现改动
# ... edit files ...

# 3. 提交并推送
git add <files>
git commit -m "<type>: <description>"
git push -u origin <type>/<description>

# 4. 创建 PR
gh pr create --title "<type>: <description>" --body "..."

# 5. 等 CI 通过
# 每 60s 检查一次，失败则分析修复
gh run list --branch <branch> --limit 3

# 6. CI 全部通过后，请求用户 review 合入
# 或者用户授权后：
gh pr merge <pr-number> --merge
```

## 项目结构

**`modules/` 放会被链进二进制的独立包，`src/` 放还没分出去的骨架，缩小 `src/`
就是方向。** 单位是**子系统**不是文件——一文件一包只会把目录列表写成 N 份清单。

```
modules/                   ← 独立包，各带 mcpp.toml，由 path 引用，依赖显式声明
├── libs/                  ← json + toml：文本格式解析器（vendored 与自写）
├── log/                   ← 分级日志
├── versioning/            ← mcpp.version + mcpp.version_req
├── source-kind/           ← 源文件角色表
├── dyndep/                ← ninja dyndep 发射
├── platform/              ← 平台抽象层（所有平台相关代码）
├── manifest/              ← manifest 模型、TOML/xpkg 解析 + 它们所用的词汇
├── toolchain-model/       ← 工具链「是什么」：triple / model / dialect /
│                            cppfly / fingerprint / linkmodel
└── buildmcpp/             ← build.mcpp 契约：协议、指令表、provision、tool store
src/
├── cli.cppm              ← 命令行入口
├── config.cppm           ← 全局配置
├── build/                ← 构建系统（ninja 后端、prepare/plan/execute）
├── pm/                   ← 包管理子系统
├── toolchain/            ← 工具链「在哪」：探测、registry、gcc/clang/msvc/llvm
├── modgraph/             ← 模块图扫描验证
├── pack/                 ← 打包发布
├── runtime/              ← 运行时契约（binding、ELF 事实）
└── xlings/               ← xlings 集成
tests/unit/                ← 跨层单测（`mcpp test`）
modules/<x>/tests/         ← 子系统单测（`mcpp test -p <x>`，CI 逐个跑）
tests/e2e/                 ← E2E 测试脚本 (`MCPP=...` + `run_all.sh`)
docs/                     ← 用户文档
.agents/docs/             ← 设计文档
.agents/skills/           ← Agent 技能文档
```

## 路径窄化不变式（走查得到的 path 不得直接 `.string()`）

Windows 上 `std::filesystem::path::string()` 会把 native（宽）名经**进程 ANSI 代码页**
转换，遇到该代码页拼不出的字符就抛 `std::system_error`。**非 Windows 上同一个调用只是
一次拷贝，永不失败**——所以这个隐患在 Linux/macOS 上（包括它们的测试里）完全不可见。

它已经付过两次代价，每次戴着不同的面具：#230 抛出后逃到 `std::terminate`，git-bash
显示为**裸 exit 127**（看起来像"命令找不到"）；#516 逃到 `main()` 的 catch，显示为
`internal: unhandled exception`（看起来像下载器的**解压/编码缺陷**）。#231 加固了三处
调用点，漏掉了同一个 walk 循环里**早一行**执行的第四处。

规则（按用途选，不是三选一的风格问题）：

| 用途 | 写法 |
|---|---|
| 与 ASCII 字面量比较 | **按 `path` 比**，根本不窄化 |
| 需要稳定身份（hash / key / digest） | `p.u8string()` —— 各平台都是 UTF-8，不碰代码页 |
| 需要交给编译器 / ninja / CDB | `mcpp::modgraph::try_narrow(p)`，并处理 `nullopt` |

`try_narrow` 返回 `nullopt` 表示"这个文件没法出现在任何交给工具链的字符串里"。
**跳过它，并且必须报出来**——`mcpp.diag` 的批次不变式对此已有规定：因为前提不满足而
少做事，必须走 `diag::degraded()` 并给出 `impact`。静默丢弃是这类缺陷藏身的地方。

`src/modgraph/` 与 `src/manifest/` 是 leaf 层（全仓没有一条到 `mcpp.ui` / `mcpp.diag`
的 import 边），所以它们**记录**（`note_unnarrowable_path`），由 CLI 层排空上报。

`.github/tools/check_narrow_conversions.sh` 是硬门，但它只扫 `src/modgraph`、
`src/scaffold`——**通过不等于已审计**。确有把握的站点用 `// NARROW-OK: <理由>` 标注，
理由必须写出"为什么这个输入不可能带这种名字"。

**测试只有跑在 Windows CI 上才有意义**，且必须自己检查 `GetACP()`：runner 镜像哪天默认
UTF-8 ACP（65001），这类用例会静默变成永远绿的装饰品。参见
`tests/unit/test_modgraph.cpp` 的 `Scanner.GlobWalkSurvivesNamesTheCodePageCannotSpell`。

## 注意事项

- C++23 模块项目，修改模块时注意 import 依赖顺序
- 平台相关代码统一放 `modules/platform/`，不在其他模块中直接使用 `#if defined`
- **新增 `modules/` 包要改四处**（包自己的 mcpp.toml、根的
  `[dependencies.mcpp]`、根的 `[workspace] members`、两份 xmake 源文件清单），
  其中三处的遗漏都在**很远的地方**才失败——最坏的一处只在没有 mcpp 的 macOS
  自举机器上。`.github/tools/check_modules_wiring.sh` 守住它，会在 CI 里跑
- **import 要指向类型的提供者，不是它的某个消费者。** 一条为拿 `Toolchain` 而
  `import mcpp.toolchain.detect` 的边（`model` 才是定义处，`detect` 只是转发）
  把整个 build.mcpp 契约压在了包管理器之上。一行 import 就是「能不能成为独立
  模块」的全部距离
- **子系统的单测放 `modules/<x>/tests/`**，跨层的放 `tests/unit/`。前者构建在
  「只有它自己和它声明的依赖」这个配置里——那是根构建从不产生的配置，也是唯一
  能抓到「悄悄依赖了未声明之物」的地方。
  ⚠️ `mcpp test -p <x>` 对没有测试的成员**退 0**
- E2E 测试应声明所需 capability，并使用隔离的 `MCPP_HOME`；需要网络/索引的脚本
  不得被描述为完全离线
- 不确定方向时先在 Issue 讨论再动手
- **永远走 PR 流程，不直接 push main**
