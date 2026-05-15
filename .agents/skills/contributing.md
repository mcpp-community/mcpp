# mcpp 项目开发贡献指南

本文档面向 AI Agent，说明如何参与 mcpp 项目的开发贡献，包括 Bug 修复、新功能、代码优化、文档改进等。

## 项目概况

- 仓库：https://github.com/mcpp-community/mcpp
- 语言：C++23（纯模块化，43+ 个模块，零 .h 头文件）
- 构建：`mcpp build`（mcpp 用自己构建自己）
- 测试：E2E 测试在 `tests/e2e/` 目录下
- 设计文档：`.agents/docs/` 目录
- CI：GitHub Actions，基于 `main` 分支的 PR 自动触发

## 贡献流程

### 1. 先创建 Issue

**所有贡献都应该先创建 Issue**，特别是新功能。这是为了：
- 与维护者对齐需求和设计方向
- 避免重复工作
- 留下讨论记录

**Bug 修复**

```bash
gh issue create \
  --title "fix: 简短描述问题" \
  --body "## 复现步骤
1. ...
2. ...

## 期望行为
...

## 实际行为
...

## 环境
- mcpp 版本：
- 操作系统：
- 编译器："
```

**新功能请求**

```bash
gh issue create \
  --title "feat: 简短描述功能" \
  --body "## 动机
为什么需要这个功能？

## 设计思路
大致方案描述。

## 涉及的文件/模块
预估改动范围。"
```

**代码优化 / 重构**

```bash
gh issue create \
  --title "refactor: 简短描述优化" \
  --body "## 当前问题
...

## 优化方案
...

## 风险评估
是否影响现有功能？"
```

### 2. 实现改动

**分支规范**

```bash
git checkout main
git pull origin main
git checkout -b <type>/<short-description>
# type: feat / fix / refactor / test / docs
```

**开发要求**

- 代码风格：遵循现有代码模式（查看相邻代码的格式、命名、错误处理方式）
- 模块导入：使用 `import std;` 和 `import mcpp.xxx;`
- 不要引入不必要的改动：只改需要改的，不顺手重构不相关代码

**构建验证**

```bash
# 找到 mcpp 二进制
ls target/x86_64-linux-gnu/*/bin/mcpp

# 构建
<mcpp-binary> build

# 如果修改了构建相关代码，确认自举仍能通过
```

**测试**

```bash
# 运行基础测试
bash tests/e2e/01_help_and_version.sh

# 运行与改动相关的 E2E 测试
bash tests/e2e/<relevant-test>.sh

# 如果添加新功能，应创建对应的 E2E 测试
# 测试脚本放在 tests/e2e/ 下，命名格式：<序号>_<描述>.sh
```

### 3. 提交 PR

**提交信息规范**

```
<type>: <简短描述>

type 取值：
  feat:     新功能
  fix:      Bug 修复
  refactor: 代码重构（不改变功能）
  test:     添加/修改测试
  docs:     文档改动
  ci:       CI/CD 相关
```

**创建 PR**

```bash
git push -u origin <branch-name>

gh pr create \
  --title "<type>: 简短描述" \
  --body "## Summary
- 改动点 1
- 改动点 2

Closes #<issue-number>

## Test plan
- [ ] mcpp build 编译通过
- [ ] 相关 E2E 测试通过
- [ ] 新增 E2E 测试（如适用）"
```

### 4. CI 必须通过

**PR 的 CI 检查是硬性要求**。CI 不通过的 PR 不会被合入。

- CI workflow 在 `.github/workflows/ci.yml`
- 只有 base 为 `main` 的 PR 会触发 CI
- CI 内容：mcpp 自举构建 + E2E 测试套件
- 如果 CI 失败，检查 `gh run view <run-id> --log-failed` 定位问题

```bash
# 查看 PR 的 CI 状态
gh pr checks <pr-number>

# 查看失败日志
gh run view <run-id> --log-failed
```

### 5. Review & 合入

- 维护者会 review PR 并给出反馈
- 根据反馈修改后再次 push（CI 会重新运行）
- 合入方式：Squash merge

## 常用工具

| 工具 | 用途 |
|---|---|
| `gh` | GitHub CLI — 创建 issue/PR、查看 CI、管理仓库 |
| `git` | 版本控制 |
| `mcpp build` | 编译项目 |
| `mcpp run` | 构建并运行 |
| `mcpp test` | 运行测试 |
| `bash tests/e2e/*.sh` | 运行 E2E 测试 |

## 项目目录结构

```
src/
├── cli.cppm                 ← 命令行入口（最大文件）
├── config.cppm              ← 全局配置
├── manifest.cppm            ← mcpp.toml 解析
├── build/                   ← 构建系统（ninja 后端、编译标志）
├── pm/                      ← 包管理子系统
├── toolchain/               ← 编译器检测与管理
├── modgraph/                ← 模块图扫描与验证
├── pack/                    ← 打包发布
├── xlings.cppm              ← xlings 抽象层
└── ui.cppm                  ← 输出样式

tests/e2e/                   ← E2E 测试脚本
docs/                        ← 用户文档
.agents/docs/                ← 设计文档
.agents/skills/              ← Agent 技能文档
```

## 注意事项

- mcpp 是 C++23 模块项目，修改模块时注意 import 依赖顺序
- 不要修改 `.agents/docs/` 下的设计文档（除非是专门的文档 PR）
- E2E 测试应该能独立运行，不依赖网络（使用本地 path index 等方式）
- 如果不确定设计方向，先在 Issue 里讨论再动手
