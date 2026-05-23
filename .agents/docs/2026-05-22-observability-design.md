# mcpp 可观察性设计方案

**Date**: 2026-05-22
**Status**: Proposed

## 一、现状分析

### 1.1 用户输出层（mcpp::ui）

`src/ui.cppm` 提供 Rust/Cargo 风格的终端输出：

| 函数 | 输出目标 | 受 -q 影响 | 颜色 | 用途 |
|------|---------|-----------|------|------|
| `status(verb, msg)` | stdout | 是 | 亮绿 | 编译进度 "Compiling foo v0.1.0" |
| `info(verb, msg)` | stdout | 是 | 亮青 | "Downloading", "Updating" |
| `finished(profile, elapsed)` | stdout | 是 | 亮绿 | "Finished release in 2.3s" |
| `warning(msg)` | stderr | 否 | 黄色 | 警告 |
| `error(msg)` | stderr | 否 | 亮红 | 错误 |
| `diagnostic(d)` | stderr | 否 | 多色 | Rust 风格多行诊断 |
| `plain(msg)` | stdout | 是 | 无 | 普通文本 |
| `ProgressBar` | stdout | 是 | 青色 | 下载进度条 |

**开关**：
- `--quiet` / `-q`：抑制 stdout 输出，不影响 stderr
- `--no-color` / `MCPP_NO_COLOR=1` / `NO_COLOR`：禁用颜色
- `--verbose` / `-v`：仅控制 ninja 构建输出详细程度

### 1.2 文件日志层（mcpp::log，刚创建）

`src/log.cppm` 提供简单的文件写入：

- **开关**：`MCPP_LOG_LEVEL=debug|info|warn|error`（默认 off）
- **输出**：`~/.mcpp/log/debug.log`
- **格式**：`2026-05-22 08:46:45.353 [DEBUG] tag: message`
- **线程安全**：mutex 保护

### 1.3 xlings 事件流

mcpp 通过 NDJSON interface 与 xlings 交互，xlings 发出 5 种事件：

| 事件类型 | 是否展示给用户 | 说明 |
|---------|-------------|------|
| ProgressEvent | 部分（进度条） | 下载/解压进度 |
| DataEvent (download_progress) | 是（ProgressBar） | 文件级下载进度 |
| DataEvent (其他) | 否 | install_plan, styled_list 等 |
| LogEvent | **否** | xlings 内部日志，完全丢弃 |
| ErrorEvent | **否** | xlings 错误详情，未展示 |

### 1.4 当前缺陷

1. **--verbose 覆盖不全**：只控制 ninja 构建，不控制包安装、工具链操作
2. **xlings 内部信息丢失**：LogEvent/ErrorEvent 被丢弃，排查问题时无信息
3. **文件日志无持久配置**：只能通过环境变量开启，重启后需重新设置
4. **无结构化诊断**：工具链安装失败时只输出一行 error，无上下文
5. **日志无轮转**：debug.log 会无限增长

## 二、设计目标

1. **零配置可用**：默认行为不变（简洁的 cargo 风格输出）
2. **按需深入**：`--verbose` 显示更多过程信息，`MCPP_LOG_LEVEL` 写文件日志
3. **问题可追溯**：文件日志记录完整的命令、环境、结果，便于事后分析
4. **统一 API**：所有模块通过 `mcpp::log` 写日志，通过 `mcpp::ui` 面向用户

## 三、分层架构

```
┌──────────────────────────────────────────────────┐
│                  用户终端                         │
│  stdout: status/info/finished/progress           │
│  stderr: warning/error/diagnostic                │
└────────────────────┬─────────────────────────────┘
                     │ mcpp::ui（已有）
┌────────────────────┴─────────────────────────────┐
│              verbose 层（新增）                    │
│  --verbose 时额外输出到 stderr:                   │
│    [VERBOSE] fetcher: cmd = cd '...' && env ...  │
│    [VERBOSE] toolchain: installing dep xim:glibc │
│    [VERBOSE] xlings: LogEvent level=info ...     │
└────────────────────┬─────────────────────────────┘
                     │ mcpp::log（增强）
┌────────────────────┴─────────────────────────────┐
│              文件日志层                            │
│  ~/.mcpp/log/mcpp.log                            │
│  格式: TIMESTAMP [LEVEL] tag: message            │
│  级别: MCPP_LOG_LEVEL 或 config.toml [log]       │
│  轮转: 按大小（默认 10MB，保留 3 份）              │
└──────────────────────────────────────────────────┘
```

## 四、详细设计

### 4.1 mcpp::log 模块增强

**文件**：`src/log.cppm`

```cpp
export namespace mcpp::log {

enum class Level { off, error, warn, info, debug };

struct Config {
    Level       level       = Level::off;
    std::size_t maxFileSize = 10 * 1024 * 1024;  // 10MB
    int         maxFiles    = 3;                   // 保留 3 份
    std::filesystem::path logDir;                  // ~/.mcpp/log/
};

// 初始化：优先 MCPP_LOG_LEVEL 环境变量，其次 config.toml [log] 配置
void init(const Config& cfg);

// 核心 API（不变）
void debug(std::string_view tag, std::string_view message);
void info (std::string_view tag, std::string_view message);
void warn (std::string_view tag, std::string_view message);
void error(std::string_view tag, std::string_view message);

// verbose 输出：同时写文件日志 + stderr（仅 --verbose 时）
void set_verbose(bool v);
void verbose(std::string_view tag, std::string_view message);

// 当前级别查询（用于条件构造昂贵的日志消息）
bool is_enabled(Level l);
bool is_verbose();

} // namespace mcpp::log
```

**verbose() 行为**：
- 始终写入文件日志（level >= info 时）
- 当 `--verbose` 开启时，额外输出到 stderr：`[VERBOSE] tag: message`
- 用灰色/暗色显示，与正常 ui 输出区分

**日志轮转**：
- 写入前检查文件大小
- 超过 maxFileSize 时：`mcpp.log` → `mcpp.log.1` → `mcpp.log.2`（最多 maxFiles 份）
- 简单实现，不需要复杂的日志框架

### 4.2 config.toml `[log]` 配置

```toml
[log]
# 日志级别: "off" | "error" | "warn" | "info" | "debug"
# 环境变量 MCPP_LOG_LEVEL 优先于此配置
level = "off"

# 单个日志文件最大大小（字节），超过后轮转
max_file_size = 10485760    # 10MB

# 保留的历史日志文件数量
max_files = 3
```

**优先级**：`MCPP_LOG_LEVEL` 环境变量 > `config.toml [log].level` > 默认 off

### 4.3 --verbose 全局开关扩展

**当前**：`--verbose` 仅传给 ninja `-v`

**扩展后**：`--verbose` 同时：
1. 设置 `mcpp::log::set_verbose(true)`
2. 在 stderr 显示 verbose 输出
3. 如果文件日志未开启，自动提升到 info 级别

**影响的子系统**：

| 子系统 | verbose 输出内容 |
|--------|-----------------|
| 工具链安装 | 安装目标、xlingsHome、xlingsBinary、依赖安装结果 |
| 包安装 | resolve_xpkg_path 的查找路径、copy workaround 是否触发 |
| xlings 调用 | 完整命令字符串、exitCode、失败详情 |
| xlings 事件 | 转发 LogEvent（level=info/warn/error）|
| 构建 | ninja -v（已有） |
| 工具链探测 | sysroot 路径、payload paths、编译器版本 |
| post-install fixup | patchelf_walk 路径、fixup_clang_cfg 重写内容 |

### 4.4 xlings 事件转发

**当前**：CliInstallProgress 只处理 DataEvent (download_progress)

**增强**：在 EventHandler 中转发 LogEvent 和 ErrorEvent

```cpp
// src/cli.cppm — CliInstallProgress 增强
void on_log(const mcpp::xlings::LogEvent& e) override {
    // 始终写入文件日志
    if (e.level == "error")
        mcpp::log::error("xlings", e.message);
    else if (e.level == "warn")
        mcpp::log::warn("xlings", e.message);
    else
        mcpp::log::info("xlings", e.message);

    // verbose 模式下同时输出到 stderr
    mcpp::log::verbose("xlings", std::format("[{}] {}", e.level, e.message));
}

void on_error(const mcpp::xlings::ErrorEvent& e) override {
    mcpp::log::error("xlings", std::format("{}: {} (hint: {})",
        e.code, e.message, e.hint));
    // 错误始终显示给用户
    mcpp::ui::error(std::format("xlings: {}", e.message));
    if (!e.hint.empty())
        mcpp::ui::plain(std::format("  hint: {}", e.hint));
}
```

### 4.5 关键路径的日志埋点

以下位置需要添加 `mcpp::log::verbose()` / `mcpp::log::debug()` 调用：

#### 工具链安装（src/cli.cppm）
```
verbose: "install: target={} ximName={}"
verbose: "  xlingsHome={} xlingsBinary={}"
verbose: "  installing dep: {}"
verbose: "  dep {} result: {}"
verbose: "  installing main: {}"
verbose: "  post-install fixup: patchelf_walk on {}"
verbose: "  post-install fixup: fixup_clang_cfg"
debug:   "  patchelf_walk: patching {}"
debug:   "  fixup_clang_cfg: rewriting line '{}' → '{}'"
```

#### 包获取（src/pm/package_fetcher.cppm）
```
verbose: "resolve: target={} verdir={}"
verbose: "  verdir exists={}"
verbose: "  copy workaround: src={} → dst={}"
verbose: "  xlings install exitCode={}"
verbose: "  after install: verdir exists={}"
debug:   "call: cmd={}"
debug:   "call: result exitCode={}"
```

#### xlings 调用（src/xlings.cppm）
```
# xlings.cppm 是 LEAF 模块，不能 import mcpp.log
# 方案：在 call site 添加日志，不修改 xlings.cppm 本身
# 或：给 xlings::call() 添加可选的 log callback
```

#### 工具链探测（src/toolchain/probe.cppm）
```
verbose: "probe_sysroot: strategy={} result={}"
verbose: "probe_payload_paths: glibcLib={} linuxInclude={}"
verbose: "discover_link_runtime_dirs: {}"
```

#### 构建（src/build/）
```
verbose: "build plan: {} sources, {} modules"
verbose: "fingerprint: {}"
debug:   "ninja cmd: {}"
```

### 4.6 环境变量汇总

| 环境变量 | 用途 | 默认值 |
|---------|------|--------|
| `MCPP_LOG_LEVEL` | 文件日志级别 | off |
| `MCPP_NO_COLOR` | 禁用颜色 | 未设置（自动检测 TTY） |
| `NO_COLOR` | 标准禁色协议 | 未设置 |
| `MCPP_HOME` | mcpp 主目录 | ~/.mcpp |

### 4.7 CLI 开关汇总

| 开关 | 作用 |
|------|------|
| `--verbose` / `-v` | 在 stderr 显示详细过程信息 + ninja -v |
| `--quiet` / `-q` | 抑制 stdout 的 status/info/progress |
| `--no-color` | 禁用所有颜色输出 |

`--verbose` 和 `--quiet` 互斥。如果同时指定，`--quiet` 优先（抑制 stdout，verbose 仍输出到 stderr）。

## 五、日志文件格式

**路径**：`~/.mcpp/log/mcpp.log`

**格式**：
```
2026-05-22 08:46:45.353 [DEBUG] fetcher: resolve_xpkg_path: target='xim:llvm@20.1.7' autoInstall=true
2026-05-22 08:46:45.353 [DEBUG] fetcher:   xlingsHome = /home/speak/.mcpp/registry
2026-05-22 08:46:45.353 [DEBUG] fetcher:   expected verdir = /home/speak/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7
2026-05-22 08:46:45.353 [DEBUG] fetcher:   verdir exists = false
2026-05-22 08:46:45.353 [INFO ] fetcher:   copy workaround triggered: src=/home/speak/.xlings/data/xpkgs/xim-x-llvm/20.1.7
2026-05-22 08:46:46.506 [INFO ] fetcher:   copy result: Success
2026-05-22 08:46:46.600 [INFO ] toolchain: post-install: patchelf_walk on lib/
2026-05-22 08:46:47.100 [INFO ] toolchain: post-install: fixup_clang_cfg
```

**tag 约定**：

| tag | 来源 |
|-----|------|
| `config` | 配置加载 |
| `toolchain` | 工具链安装/探测 |
| `fetcher` | 包获取/安装 |
| `xlings` | xlings 事件转发 |
| `build` | 构建过程 |
| `probe` | 工具链探测 |
| `pack` | 打包（mcpp pack） |

## 六、实施计划

### Phase 1：完善 log 模块基础（当前 PR）
1. 增加日志轮转
2. 增加 `verbose()` / `set_verbose()` / `is_verbose()`
3. config.toml `[log]` 配置支持
4. `--verbose` 全局开关调用 `set_verbose(true)`

### Phase 2：关键路径埋点
1. 工具链安装全流程（cli.cppm）
2. 包获取/copy workaround（package_fetcher.cppm）
3. xlings 调用命令和结果（package_fetcher.cppm call site）
4. xlings LogEvent/ErrorEvent 转发

### Phase 3：扩展覆盖
1. 工具链探测（probe.cppm）
2. 构建过程（build/）
3. sysroot 完整性检查
4. post-install fixup 详情

## 七、不做的事情

- **不引入第三方日志框架**：mcpp 追求零依赖，自带的简单 log 模块足够
- **不做远程日志/遥测**：mcpp 是本地工具
- **不修改 xlings.cppm**：它是 LEAF 模块，日志在 call site 添加
- **不做日志级别运行时切换**：重启生效即可
- **不做 JSON 格式日志**：纯文本足够，保持简单
