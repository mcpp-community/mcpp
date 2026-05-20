---
name: mcpp-contributing
description: Use when contributing to the mcpp project — submitting bug fixes, new features, code optimizations, documentation improvements, or any PR. Covers issue creation, branch conventions, build verification, CI requirements, and PR workflow using gh and git.
---

# mcpp 项目开发贡献规范

## Overview

mcpp 项目的贡献流程：先创建 Issue → 切分支 → 实现改动 → 提交 PR → CI 通过 → Review 合入。

- 仓库：https://github.com/mcpp-community/mcpp
- 构建：`mcpp build`（C++23 模块自举）
- 测试：`tests/e2e/` 下的 bash 脚本
- CI：GitHub Actions，base 为 `main` 的 PR 自动触发

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
# 找到 mcpp 二进制
ls target/x86_64-linux-gnu/*/bin/mcpp
# 构建
<mcpp-binary> build
```

**测试**

```bash
bash tests/e2e/01_help_and_version.sh    # 基础测试
bash tests/e2e/<relevant-test>.sh        # 相关测试
# 新功能应创建对应 E2E 测试
```

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
- [ ] mcpp build 通过
- [ ] E2E 测试通过"
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

CI 包含三个平台：
| Workflow | 平台 | 内容 |
|----------|------|------|
| `ci` | Linux x86_64 | 自举构建 + E2E 测试 |
| `ci-macos` | macOS ARM64 | 自举构建 + E2E 测试 |
| `ci-windows` | Windows x86_64 | 自举构建 + E2E 测试 |

**三个平台全部通过才能合入。** 如果某个平台失败：
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

```
src/
├── cli.cppm              ← 命令行入口
├── config.cppm           ← 全局配置
├── manifest.cppm         ← mcpp.toml 解析
├── platform/             ← 平台抽象层（所有平台相关代码）
│   ├── platform.cppm     ← 统一外观模块
│   ├── common.cppm       ← 平台常量与检测
│   ├── process.cppm      ← 进程执行（自动 stdin 保护）
│   ├── fs.cppm           ← 文件系统（exe 路径、文件锁）
│   ├── shell.cppm        ← Shell 引用
│   ├── env.cppm          ← 环境变量
│   ├── terminal.cppm     ← 终端检测
│   ├── macos.cppm        ← macOS 特有
│   ├── linux.cppm        ← Linux 特有
│   └── windows.cppm      ← Windows 特有
├── build/                ← 构建系统（ninja 后端）
├── pm/                   ← 包管理子系统
├── toolchain/            ← 编译器检测管理
├── modgraph/             ← 模块图扫描验证
├── pack/                 ← 打包发布
└── xlings.cppm           ← xlings 抽象层
tests/e2e/                ← E2E 测试脚本
docs/                     ← 用户文档
.agents/docs/             ← 设计文档
.agents/skills/           ← Agent 技能文档
```

## 注意事项

- C++23 模块项目，修改模块时注意 import 依赖顺序
- 平台相关代码统一放 `src/platform/`，不在其他模块中直接使用 `#if defined`
- E2E 测试应独立运行，不依赖网络
- 不确定方向时先在 Issue 讨论再动手
- **永远走 PR 流程，不直接 push main**
