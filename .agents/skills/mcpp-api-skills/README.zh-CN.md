# mcpp-api-skills

[English](README.md) | **简体中文**

面向 mcpp 的 Agent Skills，帮助 AI 编码助手创建工程、编写 `mcpp.toml`、
管理依赖、扩展构建流程，以及从其他工具调用 mcpp。

每个 skill 聚焦一项任务，提供相关语法、示例和常见问题。
完整列表见 [skills/](skills/README.zh-CN.md)，也可以通过
[接口索引](skills/INTERFACES.md) 查找命令、配置项或 API。

## 从这里开始

| 任务 | Skills |
| --- | --- |
| 创建并运行工程 | [入门](skills/basics/README.zh-CN.md) |
| 配置目标、模块或工作空间 | [工程配置](skills/project-config/README.zh-CN.md) |
| 添加代码生成或构建规则 | [构建扩展](skills/extensions/README.zh-CN.md) |
| 从 agent、编辑器或 CI 调用 mcpp | [程序接口](skills/integration/README.zh-CN.md) |

## 使用

让 agent 读取相关的 `SKILL.md`，或将该 skill 的目录复制到 agent 使用的
skills 位置；如果其中有 `references/`，一并保留。

例如：

```text
Read skills/project-config/mcpp-workspaces/SKILL.md and configure this
project as a workspace with a shared library and two applications.
```

## 相关资料

- [mcpp 文档](https://github.com/mcpp-community/mcpp/tree/main/docs)
- [源码基线与验证记录](maintenance/verification.md)
