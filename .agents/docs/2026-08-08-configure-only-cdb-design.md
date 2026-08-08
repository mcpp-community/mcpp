# `build --configure-only` 与可靠 CDB 设计

## 1. 背景

普通 `mcpp build` 已在启动 Ninja 前生成 `compile_commands.json`。因此源码存在
语法错误时仍能得到 CDB；真正缺失的是一个只完成项目解析、工具链解析、构建计划
生成和 CDB 发布，而不编译普通翻译单元、不链接最终目标的入口。

PR #372 同时实现了 IDE snapshot、NDJSON 生命周期、内容寻址 reply、发布状态机和
CDB 可靠性。RFC #379 决定拆分这些关注点：本设计只保留只有 mcpp 能可靠完成的
构建配置能力，通用机器协议继续在 RFC 中讨论。

## 2. 目标

新增：

```text
mcpp build --configure-only [现有 build selectors]
```

成功时必须满足：

1. 复用真实 `prepare_build()` 和 `BuildPlan`，不复制编译参数推导。
2. 根 CDB 覆盖所选 package 的普通源码与 `tests/**/*.cpp`。
3. 测试 TU 带有 dev-dependencies 和匹配的 `[build].flags`。
4. 准备标准库 BMI，并只物化已经命中全局缓存的依赖 BMI。
5. 不启动 Ninja，不生成普通对象文件，不链接可执行文件或库。
6. CDB 通过跨平台原子替换发布；失败时旧 CDB 保持不变。
7. 仅当新 CDB 已成功发布时返回 0。

该命令不是只读操作。`prepare_build()` 仍可能执行 `build.mcpp`、安装缺失依赖或
工具链、写 `mcpp.lock`、resolution、缓存及构建目录元数据。IDE 插件必须继续受
workspace trust 约束。

## 3. 非目标

本 PR 不实现：

- JSON 或 NDJSON stdout、wire envelope、protocol version。
- `ide snapshot`、`ide configure` 或 daemon。
- project/configuration/snapshot ID。
- `.mcpp/ide/current.json`、内容寻址 reply、跨文件发布事务或发布锁。
- `invalidatedBy`、manifest metadata 或依赖图。
- 构建未缓存的项目或依赖模块 BMI。
- 新的 toolchain、profile、feature、capability 或 workspace selector 语义。
- `.xlings.json` pin 更新。

## 4. 方案选择

### 方案 A：复用 Ninja backend 的内部 dry-run，推荐

`prepare_build()` 生成包含普通目标和测试目标的计划，再以
`BuildOptions::dryRun=true` 调用现有 Ninja backend。backend 仍生成 `build.ninja`
和 CDB，但在启动 Ninja 前返回。

优点：编译命令只有一个生成路径；现有 selector、工具链和平台逻辑全部复用。
风险：不能调用普通 `run_build_plan()`，否则会错误填充 BMI cache 和 build fast-path
cache。需要独立的 `run_configure_plan()` 收口配置模式。

### 方案 B：在 CLI 直接调用 `emit_compile_commands()`

代码更短，但会绕过 backend 的 manifest、命令长度检查和后续编译命令演进，形成
第二条 CDB 路径。不采用。

### 方案 C：保留 #372 的 `ide configure`

能提供更丰富状态，但需要 NDJSON、snapshot 发布和长期协议兼容，且插件当前只消费
CDB 路径与成功结果。不采用。

## 5. CLI 与执行流

`build` 增加布尔选项 `--configure-only`，其余选项继续由现有 parser 和
`BuildOverrides` 处理：`-p/--package`、`--workspace`、`--profile`、`--target`、
`--features`、`--cap`、`--cache`、`--offline`、`--strict` 和 `--static`。

执行流：

```text
cmd_build
  -> workspace_fanout_members（沿用现有语义）
  -> discover_test_targets（按 member 发现测试）
  -> prepare_build(includeDevDeps = !tests.empty(), extraTargets = tests)
  -> stage_cached_module_prerequisites
  -> run_configure_plan
       -> NinjaBackend::build(dryRun=true, requireCompileDatabase=true)
       -> 生成 build.ninja
       -> 原子发布 compile_commands.json
       -> 在 spawn Ninja 前返回
```

配置模式必须跳过 build fast path。它也不得：

- 调用 `bmi_cache::populate_from()`；
- 写 `target/.build_cache`；
- 输出 `Finished ...` 这种表示已完成编译的状态；
- 把不存在的最终产物报告为 produced artifacts。

workspace fan-out 继续逐 member 调用相同流程。每个 member 的新 CDB 内容与已有根
CDB 合并，fresh entry 优先，最终得到一个覆盖 workspace 的根 CDB。任一 member
失败时沿用现有 continue-on-failure 和首个非零结果规则。

## 6. 测试目标发现

从 `run_tests()` 抽取 `discover_test_targets()`，由 `mcpp test` 和配置模式共同调用。
该函数负责：

- 将 package selector 解析到同一个 member 根目录；
- 展开 `tests/**/*.cpp`；
- 用相对 `tests/` 的无扩展路径生成稳定测试名；
- 检测重复测试名；
- 把匹配的 `[build].flags` 中 defines、cflags、cxxflags 附到合成 target。

`mcpp test --list` 现有的 best-effort 行为必须保留：源码或 manifest 尚未可构建时，
只要能够发现测试文件就继续列出；严格 manifest 与依赖校验仍由后续
`prepare_build()` 完成。

配置模式只有在发现测试 target 时启用 dev-dependencies。没有测试的项目不应仅因
IDE 配置而安装无关开发依赖。

## 7. 模块前置产物

`prepare_build()` 已保证当前工具链需要的标准库模块可用。配置模式还会把计划中
`servedFromCache && providesModule` 的依赖 BMI 从全局缓存物化到计划引用的构建目录。

该步骤只复制现有 BMI，不复制缓存对象文件，也不编译任何缺失模块。物化失败发生在
CDB 发布前，并保留旧 CDB。项目自身模块或未缓存依赖模块保持 pending；用户显式
执行普通 `mcpp build` 后才获得完整模块语义。

## 8. CDB 发布与错误处理

新增平台原语 `platform::fs::replace_file(source, destination, error_code)`：

- POSIX 使用同文件系统 `rename`；
- Windows 使用 `MoveFileExW(..., MOVEFILE_REPLACE_EXISTING)`；
- 不先删除 destination；
- 函数为 `noexcept` 风格，通过 `error_code` 报错。

CDB 写入流程：

1. 生成并解析 JSON，要求顶层为数组。
2. 与现有有效条目合并并删除已不存在源文件的旧条目。
3. 内容未变化时不写文件，避免无意义触发 clangd 重索引。
4. 在同目录完成唯一临时文件写入和 flush。
5. 通过 `replace_file` 原子替换目标。
6. 替换失败时清理临时文件，返回错误，旧文件保持不变。

`write_compile_commands()` 改为返回结构化成功或错误。为保持普通构建兼容性：

- 普通 build/test 遇到 CDB 发布失败时输出 warning，继续真实构建；
- `--configure-only` 设置 `requireCompileDatabase=true`，发布失败直接返回非零。

不加入 `FileLock`。单文件原子替换已保证 clangd 不会读到半截 JSON；并发配置采用
last-writer-wins，但每个可见版本都是完整文件。只有未来重新引入多文件事务时才需要
发布锁。

## 9. 输出契约

本 PR 只提供人类输出和进程退出码，不承诺机器可读 stdout。建议成功信息为：

```text
Configured <package> (<N> compile commands)
```

错误继续通过现有 stderr/UI 通道报告。插件稳定依赖仅有：

- 进程退出码；
- 工程或 workspace 根目录的 `compile_commands.json`。

未来 RFC 可以向同一命令增加 `--format json`，默认人类输出不需要变化。

## 10. 测试策略

### 单元测试

- CDB 合并继续覆盖 fresh-wins、删除失效文件、损坏旧 JSON 回退。
- 原子 writer 拒绝非数组 JSON。
- 内容不变时不改 mtime。
- `replace_file` 成功替换旧文件。
- source 不存在时替换失败且 destination 内容保持不变。
- 测试发现覆盖 nested names、重复名、member scoping 和 `[build].flags`。
- 损坏 manifest 下 `test --list` 仍保持 best-effort inventory。

### E2E

- 语法错误源码：配置成功、CDB 含源码、没有普通对象或最终二进制。
- 测试 TU：CDB 含 `tests/**/*.cpp`，并具有 dev-dependency include/define。
- workspace：默认 virtual workspace fan-out 和 `-p <member>` 均生成正确条目。
- selector：profile、target、features、capability 与普通 build 进入同一 BuildPlan。
- 失败保留：新 CDB 发布失败时旧 CDB 内容不变且命令返回非零。
- Windows：真实 MSVC 配置与 `MoveFileExW` 替换由 Windows CI 覆盖。

## 11. 对核心构建的影响

共享变化限定为三处：

1. 测试发现从 `run_tests()` 抽为共享模块，行为由原有测试锁定。
2. CDB 从截断写改为临时文件加原子替换；普通构建失败策略保持非致命 warning。
3. backend 增加 `requireCompileDatabase` 选项，默认 false；所有现有调用语义不变。

配置模式使用独立执行函数，不写 fast-path cache 或全局 BMI cache，因此不会让后续
真实 build 错误命中不存在的产物。

## 12. 后续工作

以下事项留在 RFC #379，不阻塞本 PR：

1. 未知 format 的统一退出码、stdout 和诊断结构。
2. `--format json` 与旧 `--json` 的兼容规则。
3. `self env` JSON、`xpkg parse` schema version。
4. read-only metadata 与 manifest 合法键/枚举词汇表。
5. resolved metadata、依赖图与 CDB `invalidatedBy`。
6. 至少三个稳定消费者出现后再抽取通用 `mcpp.wire`。
