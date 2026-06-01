# CI 工具链缓存优化分析

> 日期: 2026-06-01
> 状态: draft
> 范围: Linux CI/e2e 中工具链 registry 与 xpkg payload 的重复下载、临时 `MCPP_HOME` 隔离策略，以及后续可扩展的工具链缓存架构。

## 0. 结论

当前 CI 慢点不是单纯的 actions/cache 命中率问题。Linux workflow 已经缓存了 `~/.mcpp` 和 `~/.xlings`，但部分 e2e 脚本为了隔离配置会创建新的 `MCPP_HOME`。如果这些临时 home 没有继承已安装的 xpkg payload，测试内部的 `mcpp build` / first-run auto-install 就会把同一份大工具链下载到临时目录，测试结束后又被删除。

最典型的触发链:

```text
ci-linux
  -> persistent ~/.mcpp 已有/将安装 gcc、musl-gcc
  -> tests/e2e/run_all.sh
  -> 29_toolchain_partial_versions.sh 创建 $TMP/h2
  -> first-run auto-install gcc@15.1.0-musl 到 $TMP/h2
  -> trap 删除 $TMP/h2
  -> 31_transitive_deps.sh 再创建 $TMP/mcpp-home
  -> 再次安装/下载 gcc@15.1.0-musl
  -> 后续 "Toolchain: musl-gcc" step 在 persistent ~/.mcpp 里还可能再安装一次
```

合理修法不是取消测试隔离，而是把隔离拆成两层:

1. 配置状态隔离: 每个测试仍可拥有自己的 `config.toml`、lock/cache、project state。
2. 工具链 payload 复用: 大体积、只读的 `registry/data/xpkgs` 从 persistent sandbox 或 xlings cache 继承。

## 1. 现状证据

### 1.1 workflow 已经缓存 persistent sandbox

- `.github/workflows/ci-linux.yml`: `Cache mcpp sandbox` 缓存 `~/.mcpp`，用于保留 musl-gcc、binutils、glibc、linux-headers、patchelf、ninja 等 payload。
- 同一个 workflow 也缓存 `~/.xlings`，用于保留 xlings 自己安装的包。
- E2E step 会设置 `MCPP_HOME=/home/runner/.mcpp`，并设置 `MCPP_E2E_TOOLCHAIN_MIRROR=GLOBAL`。

所以 CI 的正确方向应该是让临时 home 继承这两处 cache，而不是在临时 home 冷启动。

### 1.2 e2e 的临时 home 有两种语义

有些脚本创建临时 `MCPP_HOME` 是为了验证“空配置”或“fresh sandbox”行为，例如:

- `14_toolchain_fallback.sh`: 验证无 toolchain 且 `MCPP_NO_AUTO_INSTALL=1` 时的硬错误。
- `26_toolchain_management.sh`: 显式验证 `toolchain install/list/default/remove`。
- `29_toolchain_partial_versions.sh`: 第一段验证 partial version/default 解析，第二段验证 first-run auto-install。

这类测试不能直接复制全局 `config.toml`，否则会掩盖被测行为。但它们通常可以继承 `registry/data/xpkgs`，因为 payload 是否已经存在不应改变“配置为空时会设置默认 toolchain”的语义。

另一些脚本创建临时 `MCPP_HOME` 只是为了隔离 BMI、git/cache 或测试产物，例如:

- `31_transitive_deps.sh`: 目标是验证传递依赖 include_dirs，不是验证工具链安装。
- LLVM/import std/BMI cache 类测试: 目标是编译行为或 cache 行为，不是下载行为。

这类测试应该默认继承 payload 和必要配置。

### 1.3 `_inherit_toolchain.sh` 的模型不完整

旧 helper 只优先继承 `$HOME/.mcpp/registry/data/xpkgs`，但 `run_all.sh` 的能力检测同时承认:

- `$HOME/.xlings/data/xpkgs/xim-x-musl-gcc/...`
- `$MCPP_HOME/registry/data/xpkgs/xim-x-musl-gcc/...`

这会导致能力检测认为 musl 可用，但临时 `MCPP_HOME` helper 没有把 `.xlings` payload 继承进去，脚本实际构建时仍可能走下载路径。

### 1.4 慢点掩盖了 31 的真实功能失败

Linux PR CI 的失败链显示 `31_transitive_deps.sh` 先在临时 home 下载了 `xim:musl-gcc@15.1.0` 的 808 MB payload，然后才失败在:

```text
child/ch/src/ch.cppm:2:10: fatal error: gc/gc.h: No such file or directory
```

这说明 CI 慢不是唯一问题。即使下载复用做好，31 仍会失败，因为依赖 include_dirs 的传播模型也不完整: 依赖解析只把 dep 的 include_dirs 追加到 root manifest，而没有追加到实际发起依赖的 consumer package。`top -> ch -> gc` 里真正需要 `<gc/gc.h>` 的是 `ch`，不是 root `top`。

## 2. 本 PR 的优化策略

### 2.1 payload 继承从“整目录 symlink”改为“逐 package merge”

`tests/e2e/_inherit_toolchain.sh` 现在会从以下来源继承 xpkg payload:

- `$HOME/.mcpp/registry/data/xpkgs`
- `$HOME/.xlings/data/xpkgs`
- Windows/Git Bash 下的 `$USERPROFILE/.xlings/data/xpkgs`

目标目录是临时 home 的:

```text
$MCPP_HOME/registry/data/xpkgs
```

逐 package merge 比整目录 symlink 更稳，因为 CI 可能同时存在两套 payload 来源: mcpp 自己安装的 toolchain 在 `~/.mcpp`，xlings bootstrap 安装的包在 `~/.xlings`。逐项链接可以把两边合并进临时 home，而不会因为先 symlink 了一个根目录导致另一个来源无法补充。

### 2.2 first-run 测试只继承 payload，不继承配置

`29_toolchain_partial_versions.sh` 的 second home 继续保持:

```text
无 config/default state
无 inherited subos
```

但会继承 xpkg payload。这样仍能验证 first-run auto-install 的用户语义:

- 生成项目没有 `[toolchain]`。
- 第一次 `mcpp build` 会出现 First run。
- 默认选择 `gcc@15.1.0-musl`。
- default 会被持久化，第二次 build 不再打印 First run。

区别是 install 阶段可以发现 payload 已存在，不再把大归档下载到临时目录。

### 2.3 传递依赖测试不再承担工具链冷启动

`31_transitive_deps.sh` 的目标是验证:

- top 只声明 child。
- child 自己声明 grandchild。
- grandchild 的 `[build].include_dirs` 能传到 child 编译命令。

这个测试不应该下载工具链。现在它会继承 payload-only，并在没有可复用 musl xpkg payload 时直接 skip。musl 的安装/构建路径由 workflow 的专门 toolchain step 覆盖。

### 2.4 Linux CI 预热一次 musl-gcc

Linux e2e step 在运行 `tests/e2e/run_all.sh` 前执行:

```bash
"$MCPP" toolchain install gcc 15.1.0-musl
```

这有两个作用:

1. 让 29/31 这类临时 home 测试通过 helper 复用 persistent payload。
2. 让后续 `"Toolchain: musl-gcc — build mcpp (--target)"` 复用同一份安装。

冷 cache 时最多下载一次 musl-gcc；热 cache 时这个命令应快速命中本地 payload。

### 2.5 include_dirs 按 dependency edge 传播

`src/cli.cppm` 的依赖解析现在把 include_dirs 当作 edge 属性处理:

```text
consumer package -> dependency package
```

每个 unique dependency 仍只解析/扫描一次，但每个 consumer 都会获得该 dependency 的 public include dirs。这样:

- root 直接依赖 header-providing package 时，root compile units 能看到 headers。
- child 依赖 grandchild 时，child compile units 能看到 grandchild headers。
- 同一个 dependency 被多个 package 复用时，每条边都能得到 include dirs，而不会因为 resolved map 命中就跳过传播。

这比“全部追加到 root 全局 flags”更接近 package-owned build metadata 的长期方向，也避免传递依赖的 header 只在 root 上可见、在真正 consumer 上不可见。

## 3. 通用架构建议

### 3.1 把 e2e home 分成三种模式

建议后续显式化 e2e helper API:

```bash
source tests/e2e/_home.sh payload-only
source tests/e2e/_home.sh payload-and-config
source tests/e2e/_home.sh empty
```

语义:

| 模式 | 继承 xpkgs | 继承 config | 用途 |
|---|---:|---:|---|
| `payload-only` | 是 | 否 | first-run、空配置、install/default 语义测试 |
| `payload-and-config` | 是 | 是 | 普通编译、BMI、dependency、import std 测试 |
| `empty` | 否 | 否 | 专门验证冷启动、错误提示、install 下载路径 |

这样每个测试脚本不用手写 `MCPP_INHERIT_CONFIG=0 MCPP_INHERIT_SUBOS=0`，也能避免未来新增测试重新引入冷下载。

### 3.2 把“下载路径测试”集中到少数专门 job

大体积工具链下载只应该出现在这些地方:

1. `26_toolchain_management.sh`: CLI install/list/default/remove。
2. Linux workflow 的 toolchain matrix: GCC、musl-gcc、LLVM。
3. fresh-install workflow: 验证发布包在空环境中的安装体验。

其他 e2e 默认应复用 payload。这样失败定位也更清楚:

- 下载失败: 看 toolchain/fresh-install job。
- build/module/dependency 失败: 看 e2e。

### 3.3 cache key 和 install marker 要区分 payload 与配置

长期建议把工具链 install 状态拆开:

```text
registry/data/xpkgs/<pkg>/<version>/      # payload, content-addressable-ish
registry/toolchains/<name>@<version>.json # mcpp view: compiler path, target, stdlib, source payload
config.toml                              # user default and mirror
```

临时 home 可以安全 symlink/copy payload，但不必继承 default toolchain。`toolchain install` 应该在 payload 已存在时只补 mcpp 的 toolchain metadata，不重新下载。

### 3.4 CI 可观测性

建议后续给 e2e runner 增加轻量统计:

```text
downloads_before=<count>
downloads_after=<count>
toolchain_install_seconds=<duration>
```

可以先用日志 grep 实现:

- `Downloading xim:`
- `Downloading compat.`
- `Installing ...`

目标不是精确计费，而是让 PR 上能直接看到“这次 e2e 是否触发了工具链冷下载”。

## 4. 验证计划

本 PR 应至少验证:

1. `bash -n tests/e2e/_inherit_toolchain.sh tests/e2e/29_toolchain_partial_versions.sh tests/e2e/31_transitive_deps.sh`
2. `29_toolchain_partial_versions.sh` 日志不再在临时 home 冷下载 musl-gcc。
3. `31_transitive_deps.sh` 在可复用 musl payload 存在时通过；不存在时 skip，而不是下载。
4. Linux CI e2e 和后续 musl target step 都通过。
