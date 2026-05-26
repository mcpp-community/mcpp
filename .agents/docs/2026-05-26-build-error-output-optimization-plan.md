# mcpp build 报错输出优化方案

**Date**: 2026-05-26
**Status**: Proposed

## 1. 目标

当用户代码有编译错误时，默认 `mcpp build` 输出应聚焦在用户真正需要修复的
编译器诊断上：

- 不显示 ninja 进度信息，如 `[1/9] OBJ obj/main.o`
- 不把 `env LD_LIBRARY_PATH=...` 这种运行时环境前缀暴露在失败命令行里
- 不重复打印同一段 ninja / compiler 输出
- `--verbose` 仍保留足够的构建细节，便于定位 mcpp 自身问题

## 2. 真实复现

复现目录：

```bash
cd /home/speak/test/mcpp/helloworld
mcpp build
```

样例里 `src/main.cpp` 当前有明确语法错误：

```cpp
int main(int argc, char* argv[]) {
    hw::
    return 0;
}
```

当前普通 `mcpp build` 的关键输出：

```text
ninja: Entering directory `/home/speak/test/mcpp/helloworld/target/...'
[1/2] OBJ obj/main.o
FAILED: obj/main.o
env LD_LIBRARY_PATH='/home/speak/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/lib:...'${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH} /home/speak/.mcpp/.../clang++ ...
/home/speak/test/mcpp/helloworld/src/main.cpp:8:5: error: expected unqualified-id
    8 |     return 0;
      |     ^
1 error generated.
ninja: build stopped: subcommand failed.
error: auto-installing default toolchain gcc@15.1.0-musl failed: ...
```

为单独观察完整 backend 失败路径，使用已有 `/home/speak/.mcpp` 配置运行：

```bash
MCPP_HOME=/home/speak/.mcpp mcpp build --no-cache
```

可以稳定看到同一段 ninja 输出被打印两次：第一次由 backend 直接 `fputs`，
第二次被塞入 `BuildError.message` 后由 CLI 的 `ui::error()` 再打印。

## 3. 当前代码路径

### 3.1 ninja 进度和失败包装

`src/build/ninja_backend.cppm`

- `NinjaBackend::build()` 生成 `build.ninja`，然后执行
  `ninja -C <outputDir> 2>&1`。
- 非 verbose 模式也会在失败时输出完整 ninja 捕获文本：

```cpp
if (opts.verbose || !ok) {
    std::fputs(out.c_str(), stdout);
}
```

- 失败时又把同一份 `out` 放入 `BuildError.message`：

```cpp
return std::unexpected(BuildError{
    std::format("ninja failed (exit non-zero):\n{}", out),
    plan.outputDir / "build.ninja"});
```

`src/cli.cppm`

- `run_build_plan()` 收到失败后再次打印：

```cpp
mcpp::ui::error(r.error().message);
```

这就是重复输出的直接原因。

### 3.2 fast-path 失败会先打印再回退

`src/cli.cppm::try_fast_build()` 在 build cache 命中时直接运行旧
`build.ninja`。如果 ninja 非零退出，它会先打印输出，再返回 `nullopt`：

```cpp
if (!ok) {
    if (!verbose) std::fputs(out.c_str(), stdout);
    return std::nullopt;
}
```

随后 `cmd_build()` 进入完整 `prepare_build()`。在当前复现环境中，PATH 下的
mcpp 使用的 `MCPP_HOME` 没有默认 toolchain，所以用户代码编译错误后又追加了
一次默认 toolchain 自动安装失败提示，导致错误主题混杂。

### 3.3 `LD_LIBRARY_PATH` 进入失败命令行

`src/toolchain/probe.cppm`

- `detect()` 填充 `Toolchain::compilerRuntimeDirs`
- `compiler_env_prefix()` 把 runtime dirs 转成 shell 前缀

```cpp
return mcpp::platform::linux_::build_ld_library_path_prefix(dirs);
```

`src/platform/linux.cppm`

```cpp
return std::format("env LD_LIBRARY_PATH={}${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}} ",
                   mcpp::platform::shell::quote(joined));
```

`src/build/flags.cppm`

```cpp
f.toolEnv = mcpp::toolchain::compiler_env_prefix(plan.toolchain);
```

`src/build/ninja_backend.cppm`

```ninja
toolenv = env LD_LIBRARY_PATH='...'${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}
rule cxx_object
  command = $toolenv $cxx $cxxflags -c $in -o $out
```

ninja 在命令失败时会打印失败 command，因此这个 `toolenv` 会直接暴露给用户。

## 4. 优化方案

### 4.1 默认使用安静的 ninja 输出

默认构建命令改为安静模式：

```bash
ninja --quiet -C <outputDir>
```

作用：

- 隐藏 `ninja: Entering directory ...`
- 隐藏 `[x/y] DESCRIPTION ...`
- 保留失败时的必要 stderr/stdout

已在复现目录手动验证：`ninja --quiet -C ...` 不再显示 `[x/y]` 进度，但仍会
显示失败 command 和 compiler diagnostic。因此它只能解决第一类噪声，不能单独
解决 `LD_LIBRARY_PATH` 和重复输出。

`--verbose` 模式保持当前行为：不加 `--quiet`，继续追加 `-v`。

### 4.2 用 ScopedEnv / ScopedRun 代替 `toolenv` 命令前缀

把运行时库路径从 `build.ninja` 规则里移出，改为启动 ninja 子进程时注入环境。

建议新增跨平台 API：

```cpp
namespace mcpp::platform::env {

struct ScopedEnv {
    ScopedEnv(std::string key, std::optional<std::string> value);
    ~ScopedEnv();
};

std::string path_list_separator();
std::string runtime_library_path_key();
std::string prepend_path_list(std::string_view key,
                              std::span<const std::filesystem::path> dirs);

} // namespace mcpp::platform::env
```

平台策略：

| 平台 | 变量 | 说明 |
|------|------|------|
| Linux | `LD_LIBRARY_PATH` | 当前实际需要，替代 `toolenv` |
| macOS | 空 | 不设置 `DYLD_LIBRARY_PATH`；它会影响 ninja 自身和系统 framework 的 dyld 解析，依赖 toolchain/rpath |
| Windows | `PATH` | 如果私有工具链 DLL 需要搜索路径，按 `;` prepend；当前可先空实现 |

`NinjaBackend::build()` 中：

1. 从 `plan.toolchain.compilerRuntimeDirs` 计算 scoped env
2. 在 `process::capture(ninjaCmd)` 前创建 `ScopedEnv`
3. `build.ninja` 不再写 `toolenv` 变量
4. 所有规则从：

```ninja
command = $toolenv $cxx $cxxflags -c $in -o $out
```

改为：

```ninja
command = $cxx $cxxflags -c $in -o $out
```

这样失败时 ninja 最多打印 compiler 命令，不会再出现
`env LD_LIBRARY_PATH=...`。同时 `clang-scan-deps`、`clang++`、`llvm-ar` 等
ninja 子命令都会继承同一份环境，不需要每条 rule 单独拼 shell 前缀。

后续如果要进一步消除全局环境 mutation，可把 `ScopedEnv` 升级为
`process::run({ .argv, .cwd, .env })`，POSIX 走 `fork/execve`，Windows 走
`CreateProcessW` environment block。但第一阶段用 RAII env 已能满足当前目标，
改动面更小。

### 4.3 明确输出所有权，避免重复打印

需要规定：ninja 输出只能由一个层级负责展示。

推荐方案：

```cpp
struct BuildError {
    std::string summary;          // "build failed"
    std::string diagnosticOutput; // 过滤后的 compiler/ninja 输出
    std::optional<std::filesystem::path> where;
};
```

调用关系：

1. `NinjaBackend::build()` 只捕获并返回，不直接 `fputs`
2. `run_build_plan()` 负责打印一次
3. 默认模式打印：

```text
error: build failed
<filtered compiler diagnostics>
```

4. verbose 模式打印完整 ninja 输出，但仍只打印一次

如果暂时不想扩展 `BuildError`，最小改法是：

- `NinjaBackend::build()` 失败时不再 `fputs(out)`
- `BuildError.message` 保留一份输出

但这个最小改法仍会把整段输出挂在 `error:` 后面，可读性不如结构化字段。

### 4.4 过滤默认模式下的 ninja 包装行

`--quiet` 解决进度行，但失败输出仍会包含：

```text
FAILED: obj/main.o
<compiler command>
ninja: build stopped: subcommand failed.
```

建议增加 `filter_ninja_output(out, flags, mode)`：

默认模式移除：

- `ninja: Entering directory ...`
- `ninja: build stopped: subcommand failed.`
- `FAILED: ...`
- 已知工具命令行：以当前 `cxxBinary`、`ccBinary`、`arBinary`、`scanDepsPath`
  开头的行
- 旧 build.ninja 中残留的 `env LD_LIBRARY_PATH=... <tool>` 命令行
- ninja 进度行：`^\[[0-9]+/[0-9]+\] `

默认模式保留：

- compiler warning/error diagnostic
- 源码路径、行列号和 caret
- linker diagnostic
- mcpp 自己的 dyndep / manifest diagnostic

`--verbose` 模式不过滤。

目标默认输出示例：

```text
   Compiling helloworld v0.1.0 (.)
error: build failed
/home/speak/test/mcpp/helloworld/src/main.cpp:8:5: error: expected unqualified-id
    8 |     return 0;
      |     ^
1 error generated.
```

### 4.5 修正 fast-path 失败语义

`try_fast_build()` 当前在失败时“先打印失败，再回退完整构建”。这会导致：

- 同一编译错误可能先由 fast-path 打印一次，再由完整构建打印一次
- 旧 `build.ninja` 的 toolchain 和当前 config 不一致时，用户会看到两个不同主题的错误

建议把返回值从 `std::optional<int>` 改成 tri-state：

```cpp
enum class FastBuildKind { NotApplicable, Success, BuildFailed, StaleOrInvalid };

struct FastBuildResult {
    FastBuildKind kind;
    int exitCode = 0;
    std::string output;
};
```

策略：

- cache 不存在、fingerprint 不匹配、`build.ninja` 缺失：`NotApplicable`
- ninja 成功：`Success`
- ninja 报 `loading build.ninja` / `unknown target` / manifest 结构明显不匹配：
  `StaleOrInvalid`，静默回到完整 prepare
- 普通 compile/link 失败：`BuildFailed`，直接按同一套过滤和单次打印逻辑返回，
  不再触发自动安装默认 toolchain

这样用户代码错误不会被后续 toolchain resolve/install 错误污染。

## 5. 实施顺序

1. **先去重输出**
   - 调整 `NinjaBackend::build()` 和 `run_build_plan()` 的打印职责
   - 增加 syntax-error e2e，断言同一 compiler error 只出现一次

2. **移动 toolenv 到 scoped env**
   - 增加 `platform::env::ScopedEnv`
   - `emit_ninja_string()` 删除 `toolenv` 变量和 `$toolenv` 前缀
   - `NinjaBackend::build()` 和 `try_fast_build()` 启动 ninja 前设置运行时库路径
   - 断言生成的 `build.ninja` 不包含 `LD_LIBRARY_PATH`

3. **安静化 ninja 默认输出**
   - 非 verbose 加 `--quiet`
   - 增加 `filter_ninja_output()`
   - 断言默认失败输出不包含 `[1/`、`FAILED:`、`ninja: build stopped`

4. **修正 fast-path 失败**
   - 将 `try_fast_build()` 改为 tri-state
   - 编译失败直接返回失败，不进入完整 prepare
   - stale/invalid ninja 才允许静默回退

## 6. 验证要求

新增或调整 E2E：

```bash
bash tests/e2e/05_errors.sh
```

新增专门用例，例如 `tests/e2e/48_build_error_output.sh`：

- 创建项目并写入语法错误
- 运行 `mcpp build`
- 断言输出包含源码诊断：`src/main.cpp:*: error:`
- 断言输出不包含：
  - `LD_LIBRARY_PATH=`
  - `[1/`
  - `FAILED:`
  - `ninja: Entering directory`
  - `ninja: build stopped`
- 断言 `expected unqualified-id` 只出现一次
- 运行 `mcpp build --verbose`，断言 verbose 仍保留 ninja/command 细节
- 检查生成的 `build.ninja` 不包含 `toolenv` 和 `LD_LIBRARY_PATH`

相关单元测试：

- `filter_ninja_output()`：覆盖 progress、FAILED、command、compiler diagnostic 混合文本
- `ScopedEnv`：覆盖变量不存在、变量已存在、prepend 后析构恢复

本地验证命令：

```bash
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 bash tests/e2e/05_errors.sh
PYTEST_DISABLE_PLUGIN_AUTOLOAD=1 bash tests/e2e/48_build_error_output.sh
mcpp build
```

## 7. 风险和边界

- `ScopedEnv` 会短暂修改当前进程环境。mcpp CLI 当前是单构建流程，风险可控；
  如果未来 backend 并发运行多个 ninja，应升级为 `ProcessOptions.env`。
- 过滤 command line 不能误删 compiler diagnostic。规则应只删除“已知工具路径开头”
  的整行，不做宽泛字符串匹配。
- `--verbose` 必须不丢信息，否则会降低 mcpp 自身问题的可诊断性。
- fast-path 回退判断要保守：无法确认是 stale/invalid 时，按普通构建失败处理并
  打印一次，避免再次污染错误输出。
