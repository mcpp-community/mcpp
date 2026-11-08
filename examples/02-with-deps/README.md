# 02 — with-deps

演示：从 [mcpp-index](https://github.com/mcpp-community/mcpp-index)
拉一个依赖（`mcpplibs.cmdline`）并使用它解析命令行。

```bash
cd 02-with-deps
mcpp build                 # 第一次会自动拉依赖
mcpp run -- --name "C++23 modules"
# Hello, C++23 modules!
```

`mcpp.toml` 以身份键 `mcpplibs.cmdline`（命名空间 `mcpplibs`，包 `cmdline`）
和精确版本 `0.0.2` 声明依赖；解析结果写入 `mcpp.lock`。版本范围（`^0.0.2`、
`>=0.0.1, <0.1.0`）的写法见 docs/05-dependencies.md。
