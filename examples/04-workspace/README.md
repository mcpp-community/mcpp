# 04 — workspace

多包工作空间示例：两个库 + 一个应用，共享 namespace。

```
04-workspace/
├── mcpp.toml              # [workspace] 声明 members
├── libs/
│   ├── core/              # 基础库 → export module demo.core
│   └── greeter/           # 依赖 core → export module demo.greeter
└── apps/
    └── hello/             # 依赖 greeter → 可执行文件
```

## 构建 & 运行

```bash
cd 04-workspace

# 从 workspace 根目录构建（自动选择 hello 作为构建目标）
mcpp build
mcpp run

# 指定构建某个 member
mcpp build -p hello
```

## 要点

- 根 `mcpp.toml` 只有 `[workspace]`，没有 `[package]`（虚拟工作空间）
- member 之间用 `path = "..."` 声明依赖，与普通项目完全一致
- 所有 member 共享同一个 namespace（`demo`），module 名为 `demo.core`、`demo.greeter`
- C++ module 的 `export` / `import` 控制接口可见性，构建工具不做额外限制
