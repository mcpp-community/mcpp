# Namespace Field Design — mcpp 0.0.6

> 方案设计文档,指导 namespace 字段的实现和生态迁移。

## 1. 动机

mcpp 生态向 C++23 模块化方向发展。包索引中存在两类库:

- **模块化库**(mcpplibs 生态):原生 `export module`,用 `import` 消费
- **非模块化库**(compat):传统 C/C++ 库,通过 Form B 描述文件 + `#include` 消费

需要在 namespace 层面区分这两类,让用户一眼看出某个依赖是否是模块化的,
同时为未来"非模块化库迁移到模块化"提供清晰的升级路径。

## 2. 命名空间划分

| namespace | 含义 | 示例包 |
|---|---|---|
| `mcpplibs` | mcpplibs 生态的模块化 C++23 库 | cmdline, tinyhttps, llmapi, xpkg, templates |
| `mcpplibs.capi` | mcpplibs 的 C API 模块化封装子集 | lua (封装 Lua C API 为 C++23 module) |
| `compat` | 非模块化的第三方 C/C++ 库(兼容性支持,不鼓励直接使用) | gtest, mbedtls, lua(上游 C 库), ftxui |

### 2.1 默认 namespace

由于 xlings 的 `defaultNamespace = repo.name`(硬编码为索引仓库名 `"mcpp-index"`),
我们采用**每个包显式指定 namespace** 的方案,不依赖默认值。

### 2.2 用户 mcpp.toml 写法

```toml
# 模块化库
[dependencies.mcpplibs]
cmdline   = "0.0.2"
tinyhttps = "0.2.2"
llmapi    = "0.2.5"

# C API 封装
[dependencies.mcpplibs.capi]
lua = "0.0.3"

# 非模块化兼容库
[dependencies.compat]
gtest   = "1.15.2"
mbedtls = "3.6.1"
ftxui   = "6.1.9"
lua     = "5.4.7"         # 上游 C 库(和 mcpplibs.capi.lua 是不同的包)
```

### 2.3 迁移路径

当某个 compat 库完成模块化封装后:
1. 在 mcpplibs 或 mcpplibs.capi 下发布新包
2. compat 版本标记 deprecated(保留一段时间)
3. 用户改一行依赖声明即可迁移

## 3. 索引文件布局

### 3.1 描述文件命名

文件名使用 `<namespace>.<name>.lua` 格式:

```
pkgs/
  c/compat.gtest.lua              namespace="compat", name="gtest"
  c/compat.mbedtls.lua            namespace="compat", name="mbedtls"
  c/compat.lua.lua                namespace="compat", name="lua"
  c/compat.ftxui.lua              namespace="compat", name="ftxui"
  m/mcpplibs.cmdline.lua          namespace="mcpplibs", name="cmdline"
  m/mcpplibs.tinyhttps.lua        namespace="mcpplibs", name="tinyhttps"
  m/mcpplibs.llmapi.lua           namespace="mcpplibs", name="llmapi"
  m/mcpplibs.xpkg.lua             namespace="mcpplibs", name="xpkg"
  m/mcpplibs.templates.lua        namespace="mcpplibs", name="templates"
  m/mcpplibs.capi.lua.lua         namespace="mcpplibs.capi", name="lua"
```

### 3.2 描述文件格式

```lua
package = {
    spec      = "1",
    namespace = "compat",          -- 显式 namespace(0.0.6+)
    name      = "gtest",           -- 短名
    ...
}
```

### 3.3 xpkgs 安装目录

```
<namespace>-x-<name>/<version>/

compat-x-gtest/1.15.2/
compat-x-mbedtls/3.6.1/
compat-x-lua/5.4.7/
compat-x-ftxui/6.1.9/
mcpplibs-x-cmdline/0.0.2/
mcpplibs-x-tinyhttps/0.2.2/
mcpplibs.capi-x-lua/0.0.3/
```

## 4. mcpp 实现清单

### 4.1 src/pm/compat.cppm (已完成 PR #23)

- `resolve_package_name(name, ns)` — 显式 ns 优先 > 点号拆分 > 默认
- `qualified_name(ns, short)` — 重建完整名
- `xpkg_dir_name(index, ns, short)` — xpkgs 目录名

### 4.2 src/manifest.cppm (已完成 PR #23)

- `Package.namespace_` 字段
- TOML `[package].namespace` 解析
- `extract_xpkg_namespace()` — 从 xpkg lua 读 namespace

### 4.3 src/pm/package_fetcher.cppm (待更新)

`install_path()` 查找逻辑需要同时支持:
- 新路径: `<namespace>-x-<name>`(如 `compat-x-gtest`)
- 老路径: `<defaultIndex>-x-<qualifiedName>`(如 `mcpp-index-x-gtest`)

### 4.4 src/cli.cppm (已完成 PR #23)

- dep 名称匹配走 compat 模块
- lua namespace 传播到 manifest

## 5. 向后兼容

### 5.1 compat.cppm 的三条规则

1. 有 `namespace` 字段 → 直接用(新路径)
2. `name` 带点号 → 按首个点拆分(老路径,deprecated in 1.0.0)
3. 纯短名 → 走 `install_path` 的 fallback 扫描

### 5.2 install_path 双路查找

```
查 <xpkgs>/<namespace>-x-<name>/<version>/      ← 新路径
查 <xpkgs>/<defaultIndex>-x-<qualifiedName>/<version>/  ← 老路径 fallback
```

先找到哪个用哪个。新安装的包走新路径,老缓存继续能用。

## 6. 弃用时间线

| 版本 | 变化 |
|---|---|
| 0.0.6 | namespace 字段支持 + 双路 install_path |
| 0.1.0 | mcpp-index 全面迁移到显式 namespace |
| 1.0.0 | 移除 name 嵌点的 compat 拆分逻辑 |
