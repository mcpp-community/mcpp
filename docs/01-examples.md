# 01 — 示例项目

> 仓库的 [`examples/`](../examples) 目录下提供了一组循序渐进的最小工程,
> 覆盖从单文件 `import std` 到全静态发布包的常见场景。每个示例都可以
> 独立进入并通过 `mcpp build` 完成构建。

## 运行方式

```bash
git clone https://github.com/mcpp-community/mcpp
cd mcpp/examples/01-hello
mcpp build && mcpp run
```

每个示例附带独立的 README,仅说明该示例相对前一个引入的新概念。
安装步骤、工具链初始化等通用内容统一放在
[00 — 快速开始](00-getting-started.md) 中,不再在示例内重复。

## 示例列表

| # | 路径 | 说明 | 涉及的关键概念 |
|---|---|---|---|
| 01 | [`examples/01-hello`](../examples/01-hello/) | 单文件 + `import std` 的最小工程 | `mcpp new` 的默认产物结构 |
| 02 | [`examples/02-with-deps`](../examples/02-with-deps/) | 引入依赖 `mcpplibs.cmdline` 解析命令行参数 | `[dependencies]`、SemVer、`mcpp.lock` |
| 03 | [`examples/03-pack-static`](../examples/03-pack-static/) | 通过 `mcpp pack --mode static` 生成全静态发布包 | `[target.<triple>]` 与 `[pack]` 配置 |

## 推荐阅读顺序

建议按编号依次阅读:

1. **`01-hello`** 展示 mcpp 工程的最小骨架(`mcpp.toml` 与 `src/main.cpp`),
   并演示 `import std` 的基本用法。
2. **`02-with-deps`** 在前一示例基础上引入外部依赖,涵盖锁文件机制
   与模块化包索引的工作方式。
3. **`03-pack-static`** 演示如何将构建产物打包为可独立分发的单文件
   二进制;打包细节可参考 [02 — 发布打包](02-pack-and-release.md)。

## 新增示例

示例工程遵循统一的目录结构:`mcpp.toml` + `src/` + `README.md`。
新增示例时,在 `examples/` 下创建编号目录(如 `04-xxx/`),并在
README 中简要说明该示例演示的概念,然后提交 PR。提交规范见
[04 — 从源码构建 & 参与贡献](04-build-from-source.md)。
