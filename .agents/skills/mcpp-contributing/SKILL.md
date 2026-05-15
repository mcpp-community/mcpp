---
name: mcpp-contributing
description: Use when contributing to the mcpp project — submitting bug fixes, new features, code optimizations, documentation improvements, or any PR. Covers issue creation, branch conventions, build verification, CI requirements, and PR workflow using gh and git.
---

# mcpp 项目开发贡献

## Overview

mcpp 项目的贡献流程：先创建 Issue → 实现改动 → 提交 PR → CI 通过 → Review 合入。

- 仓库：https://github.com/mcpp-community/mcpp
- 构建：`mcpp build`（C++23 模块自举）
- 测试：`tests/e2e/` 下的 bash 脚本
- CI：GitHub Actions，base 为 `main` 的 PR 自动触发

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

### 2. 实现改动

**分支**

```bash
git checkout main && git pull origin main
git checkout -b <type>/<short-description>
# type: feat / fix / refactor / test / docs
```

**开发要求**
- 遵循现有代码风格（查看相邻代码）
- 模块导入用 `import std;` 和 `import mcpp.xxx;`
- 只改需要改的，不顺手重构不相关代码

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

### 3. 提交 PR

**提交信息**：`feat:` / `fix:` / `refactor:` / `test:` / `docs:` 前缀

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

### 4. CI 必须通过

CI 不通过的 PR 不会被合入。

```bash
gh pr checks <pr-number>           # 查看状态
gh run view <run-id> --log-failed  # 查看失败日志
```

CI 内容：mcpp 自举构建 + E2E 测试。只有 base 为 `main` 的 PR 触发。

### 5. Review & 合入

维护者 review → 反馈修改 → CI 重跑 → Squash merge。

## 项目结构

```
src/
├── cli.cppm              ← 命令行入口
├── config.cppm           ← 全局配置
├── manifest.cppm         ← mcpp.toml 解析
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
- E2E 测试应独立运行，不依赖网络
- 不确定方向时先在 Issue 讨论再动手
