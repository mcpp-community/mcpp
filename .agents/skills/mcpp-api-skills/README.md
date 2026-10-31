# mcpp-api-skills

**English** | [简体中文](README.zh-CN.md)

Agent Skills for mcpp. These guides help coding assistants create projects,
write `mcpp.toml`, manage dependencies, extend builds and call mcpp from tools.

Each skill focuses on one task, with the relevant syntax, examples and common
pitfalls. See [skills/](skills/README.md) for the full list, or use the
[interface lookup](skills/INTERFACES.md) to find a command, configuration key or API.

## Getting started

| Task | Skills |
| --- | --- |
| Create and run a project | [Getting started](skills/basics/README.md) |
| Configure targets, modules or a workspace | [Project configuration](skills/project-config/README.md) |
| Add code generation or a build rule | [Build extensions](skills/extensions/README.md) |
| Call mcpp from an agent, editor or CI | [Program interfaces](skills/integration/README.md) |

## Use

Point an agent at the relevant `SKILL.md`, or copy that skill's directory to
the location your agent uses for skills. Include its `references/` directory
when present.

For example:

```text
Read skills/project-config/mcpp-workspaces/SKILL.md and configure this
project as a workspace with a shared library and two applications.
```

## Further reading

- [mcpp documentation](https://github.com/mcpp-community/mcpp/tree/main/docs)
- [Source baseline and verification](maintenance/verification.md)
