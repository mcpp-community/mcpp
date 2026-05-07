# 02 — with-deps

演示：从 [mcpp-index](https://github.com/mcpp-community/mcpp-index)
拉一个依赖（`mcpplibs.cmdline`）并使用它解析命令行。

```bash
cd 02-with-deps
mcpp build                 # 第一次会自动拉依赖
mcpp run -- --name "C++23 modules"
# Hello, C++23 modules!
```

`mcpp.toml` 里的 SemVer 约束（`^0.0.1`）告诉 mcpp 自动选最高匹配版本。
本地结果会写到 `mcpp.lock` 锁版本。
