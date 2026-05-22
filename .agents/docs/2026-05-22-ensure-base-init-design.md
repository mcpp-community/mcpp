# 设计方案：ensure_base_init_ok + mcpp self init --force

**Date**: 2026-05-22

## 核心思路

```
ensure_base_init_ok()  →  快速检查基础环境是否完整
                           ├─ ok → 继续执行
                           └─ 不完整 → 报错 + 提示 `mcpp self init --force`

mcpp self init --force →  删除 ~/.mcpp/ 下的 bootstrap 状态，重新初始化
```

---

## 方案 A：启动时检查（每次命令）

```
mcpp <任何命令>
  ↓
ensure_base_init_ok()    ← 每次都跑
  ├─ 检查 xlings binary 存在
  ├─ 检查 subos/default/.xlings.json 存在
  ├─ 检查 patchelf binary 存在
  ├─ 检查 ninja binary 存在
  └─ 全部 ok → 继续
     任一缺失 → error + hint: mcpp self init --force

mcpp self init --force
  ↓
rm -rf bootstrap 状态
重新 ensure_init + ensure_patchelf + ensure_ninja
```

**检查实现**：4 次 `stat()` 系统调用

| 项 | 优点 | 缺点 |
|----|------|------|
| 覆盖面 | 所有命令都保护，不会在 build/run 时遇到莫名失败 | 每次命令多 4 次 stat（~0.1ms，实际可忽略） |
| 用户体验 | 第一时间提示问题，不用等到深处才报错 | `mcpp --help` 也会检查，略多余 |
| 实现复杂度 | 简单，一处检查 | — |

---

## 方案 B：仅在需要 bootstrap 工具的命令前检查

```
mcpp build / run / test       → 不检查（这些不直接需要 patchelf/ninja bootstrap）
mcpp toolchain install        → ensure_base_init_ok()
mcpp toolchain default        → 不检查（只改 config）
mcpp self init --force        → 重新初始化
```

等等——`build` 实际上需要 ninja。所以检查点应该是：

```
需要 ninja 的命令（build/run/test）     → 检查 ninja
需要 patchelf 的命令（toolchain install）→ 检查 patchelf + ninja
其他命令（list/default/help/self/...）  → 不检查
```

| 项 | 优点 | 缺点 |
|----|------|------|
| 覆盖面 | 精确覆盖需要的命令 | 需要在多个命令入口加检查 |
| 性能 | help/list/default 等零开销 | build/run 仍需检查（1-2 次 stat） |
| 实现复杂度 | 中等，需要判断每个命令的依赖 | 容易遗漏新命令 |

---

## 方案 C：在 config::load_or_init 中检查（推荐）

`load_or_init()` 是所有命令的入口（除 --help/--version），当前已在此做 bootstrap。
在 bootstrap 之后加一次轻量检查：

```
config::load_or_init()
  ↓
1. 创建目录结构
2. 加载 config.toml
3. 获取 xlings binary
4. ensure_init()
5. ensure_patchelf()
6. ensure_ninja()
7. ensure_base_init_ok()    ← 新增：验证 4-6 的结果
     ├─ 全部 ok → 返回 cfg
     └─ 缺失 → 返回 error("mcpp self init --force")
```

**关键**：ensure_base_init_ok 不做任何修复，只做检查。
修复只在 `mcpp self init --force` 中做。

```cpp
// src/config.cppm — load_or_init() 末尾
std::expected<void, ConfigError>
ensure_base_init_ok(const GlobalConfig& cfg) {
    auto xlEnv = make_xlings_env(cfg);

    struct Check {
        std::string_view name;
        std::filesystem::path path;
    };

    Check checks[] = {
        {"xlings binary",  cfg.xlingsBinary},
        {"sandbox marker", xlEnv.home / "subos" / "default" / ".xlings.json"},
        {"patchelf",       mcpp::xlings::paths::xim_tool(xlEnv, "patchelf",
                               mcpp::xlings::pinned::kPatchelfVersion) / "bin" / "patchelf"},
        {"ninja",          mcpp::xlings::paths::xim_tool(xlEnv, "ninja",
                               mcpp::xlings::pinned::kNinjaVersion) / "bin" / "ninja"},
    };

    for (auto& c : checks) {
        if (!std::filesystem::exists(c.path)) {
            return std::unexpected(ConfigError{std::format(
                "{} not found at '{}'\n"
                "  hint: run `mcpp self init --force` to repair",
                c.name, c.path.string())});
        }
    }
    return {};
}
```

**mcpp self init --force 实现**：

```cpp
int cmd_self_init(const ParsedArgs& parsed) {
    bool force = parsed.is_flag_set("force");

    if (force) {
        // 删除 bootstrap 状态（不删整个 ~/.mcpp/，保留 config.toml 和 toolchain）
        auto xlHome = cfg->xlingsHome();
        for (auto dir : {"subos", "bin"}) {
            std::filesystem::remove_all(xlHome / dir, ec);
        }
        // 删除 bootstrap 工具的 xpkg
        auto xpkgs = xlHome / "data" / "xpkgs";
        for (auto prefix : {"xim-x-patchelf", "xim-x-ninja"}) {
            std::filesystem::remove_all(xpkgs / prefix, ec);
        }
    }

    // 重新 bootstrap（复用 load_or_init 的 bootstrap 逻辑）
    ensure_init(xlEnv, false);
    ensure_patchelf(xlEnv, false, nullptr);
    ensure_ninja(xlEnv, false, nullptr);

    // 验证
    auto check = ensure_base_init_ok(*cfg);
    if (!check) {
        ui::error("init failed: " + check.error().message);
        return 1;
    }
    ui::status("Ready", "sandbox initialized");
    return 0;
}
```

| 项 | 优点 | 缺点 |
|----|------|------|
| 覆盖面 | 所有走 load_or_init 的命令都保护 | --help/--version 不走 load_or_init，不检查（合理） |
| 性能 | 4 次 stat，~0.1ms | 可忽略 |
| 实现复杂度 | 低，一处检查一处修复 | — |
| 用户体验 | 失败时明确提示修复命令 | — |
| --force 语义 | 清除 bootstrap 状态重来，不删 config/toolchain | 用户不需要重装工具链 |

---

## 方案 D：惰性检查 + 自动修复（不推荐）

```
ensure_base_init_ok()
  ├─ 缺失 → 自动尝试修复（不提示用户）
  └─ 修复失败 → 才报错
```

| 项 | 优点 | 缺点 |
|----|------|------|
| 用户体验 | 用户无感，自动恢复 | 自动修复可能在网络差时卡住 |
| 可预测性 | — | 用户不知道发生了什么，行为不透明 |
| 性能 | 正常路径无额外开销 | 异常路径可能阻塞很久（下载 patchelf/ninja） |

---

## 对比总结

| 维度 | A：每次检查 | B：按需检查 | C：load_or_init 检查 | D：自动修复 |
|------|-----------|-----------|-------------------|-----------|
| 性能（正常） | 4 stat | 1-2 stat | 4 stat | 0 |
| 性能（异常） | 0（只报错） | 0 | 0 | 阻塞（下载） |
| 覆盖面 | 全部 | 部分 | 全部（除 help） | 全部 |
| 实现复杂度 | 低 | 中 | **低** | 高 |
| 用户体验 | 明确提示 | 明确提示 | **明确提示** | 不透明 |
| 维护成本 | 低 | 需跟踪新命令 | **低** | 高 |

**推荐方案 C**：在 `load_or_init()` 末尾加 `ensure_base_init_ok()`，失败时提示 `mcpp self init --force`。

理由：
1. 4 次 stat 开销可忽略（< 0.1ms）
2. 一处检查一处修复，代码简单
3. 不自动修复（透明、可预测）
4. `--force` 只清 bootstrap 状态，不删 config 和已安装工具链
