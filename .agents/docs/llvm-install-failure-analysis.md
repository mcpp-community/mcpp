# LLVM 工具链安装失败分析

## 现象

`mcpp toolchain install llvm` 依赖包（libxml2, zlib, glibc 等）安装成功，但 LLVM 本体（800MB）缺失：

```
~/.mcpp/registry/data/xpkgs/
├── xim-x-libxml2/     ✓ 安装成功
├── xim-x-zlib/         ✓ 安装成功
├── xim-x-glibc/        ✓ 安装成功
├── xim-x-llvm/         ✗ 不存在
```

## 根因分析

### 问题 1：`</dev/null` 关闭 stdin 可能破坏 xlings 子进程通信

`platform/process.cppm:79-84` 的 `seal_stdin()` 对所有 POSIX 命令追加 `</dev/null`。

这个修复解决了 macOS 首次运行卡住的问题，但副作用是：xlings 内部的子进程（如解压 800MB LLVM 的 tar 进程）可能依赖 stdin 进行进程间通信或信号传递。小包（libxml2 等）不受影响，大包（LLVM）因为解压时间长，子进程链更复杂，可能被 broken stdin 导致静默失败。

### 问题 2：`2>/dev/null` 吞掉所有错误信息

`xlings.cppm:432-434` 构建的命令：

```bash
cd ~/.mcpp && ... xlings interface install_packages --args '...' 2>/dev/null </dev/null
```

stderr 被完全丢弃。如果 xlings 安装 LLVM 时输出了错误信息到 stderr，我们完全看不到。

### 问题 3：NDJSON handler 只处理 download_progress 事件

`xlings.cppm:645-692` 的 `handle_line` 回调：

```cpp
if (kind != "data") return;                    // 忽略非 data 事件
if (ls.find_str("dataKind") != "download_progress") return;  // 只关心下载进度
```

如果 xlings 发出了 error 事件或 log 事件报告安装失败，全部被静默丢弃。

### 问题 4：Windows 有 fallback 但 Linux 没有

`package_fetcher.cppm:608-638` 有一个 Windows-only 的 workaround：

```cpp
#if defined(_WIN32)
    // 如果 verdir 不存在，检查全局 xlings 目录 ~/.xlings/data/xpkgs/ 并复制过来
    if (!std::filesystem::exists(verdir)) {
        // ... copy from ~/.xlings/ to ~/.mcpp/
    }
#endif
```

这个 workaround 处理了 "xlings 把包装到全局目录而非 XLINGS_HOME 指定目录" 的情况。**Linux 没有这个 fallback**。

### 为什么 CI 没有这个问题

CI 设置了 `MCPP_VENDORED_XLINGS="$XLINGS_BIN"`：

```yaml
export MCPP_VENDORED_XLINGS="$XLINGS_BIN"
"$MCPP" build --target x86_64-linux-musl
```

`MCPP_VENDORED_XLINGS` 触发 `make_xlings_env()` 中的特殊路径，使用全局 xlings 二进制。而且 CI 中的工具链安装走的是 xlings 全局 sandbox（因为 MCPP_HOME 显式设置），与用户本地的嵌套沙箱场景完全不同。

实际上 **CI 也没有测试 `mcpp toolchain install llvm` 这个用户流程**——CI 只测试 `mcpp build`（使用预装的工具链）。

## 修复方案

### 修复 1：`install_with_progress()` Linux 路径改为直接命令（对齐 Windows）

Windows 已经用直接 `xlings install ... -y` 命令而非 interface 模式。Linux 也应该如此：

```cpp
int install_with_progress(const Env& env, std::string_view target,
                          const BootstrapProgressCallback& cb)
{
    // 所有平台统一：先用直接命令安装
    auto directCmd = build_command_prefix(env) + std::format(" install {} -y", target);
    int directRc = mcpp::platform::process::run_silent(directCmd);
    if (directRc == 0) return 0;

    // 直接命令失败则 fallback 到 interface 模式（保留进度回调能力）
    // ...
}
```

### 修复 2：Linux 增加与 Windows 相同的 fallback 检查

在 `resolve_xpkg_path()` 中，将 Windows 的全局目录 fallback 扩展到所有平台：

```cpp
// 移除 #if defined(_WIN32)，改为所有平台通用
if (!std::filesystem::exists(verdir)) {
    // 检查全局 xlings 目录
    auto homeDir = std::getenv("HOME");
    if (homeDir) {
        std::filesystem::path globalXpkgs =
            std::filesystem::path(homeDir) / ".xlings" / "data" / "xpkgs";
        auto globalVerdir = globalXpkgs / verdir.filename().parent_path().filename() / verdir.filename();
        if (std::filesystem::exists(globalVerdir)) {
            // 复制或软链接到 sandbox
        }
    }
}
```

### 修复 3：不对 xlings install 命令关闭 stdin

为 `install_with_progress()` 添加不关闭 stdin 的选项，或让直接 install 命令走 `std::system()` 而非 `platform::process`：

```cpp
// 直接命令不通过 platform::process（不追加 </dev/null）
int directRc = std::system(directCmd.c_str());
```

### 修复 4：CI 增加工具链安装测试

在 `ci.yml` 中增加专门测试 `mcpp toolchain install llvm` 的步骤，确保这个用户核心流程被覆盖。

## 推荐实施顺序

1. **修复 1 + 修复 3**：Linux 改用直接命令 + 不关闭 stdin（最可能解决问题）
2. **修复 2**：增加全局目录 fallback（兜底）
3. **修复 4**：增加 CI 测试（防止回归）
