# macOS LLVM 默认工具链跨平台适配方案

Date: 2026-05-16

## 目标

mcpp 在 macOS 上默认使用 LLVM/Clang 作为工具链，支持 C++23 `import std`，
与 Linux 上的 GCC 默认体验对等。

## 设计

### 当前工具链解析优先级

```
0. --target / --static CLI 覆盖 → [target.<triple>] 查找
1. 项目 mcpp.toml [toolchain].<platform> 或 .default
2. 全局 ~/.mcpp/config.toml [toolchain].default
3. First-run auto-install（当前硬编码 gcc@15.1.0-musl）
```

### 改动方案

**核心改动**：第 3 步的 first-run auto-install 改为平台感知：

```cpp
#if defined(__APPLE__)
    std::string defaultSpec = "llvm@20.1.7";
#else
    std::string defaultSpec = "gcc@15.1.0-musl";
#endif
```

**影响**：
- macOS 用户首次运行 `mcpp build` 时，自动安装 LLVM 20.1.7 而非 GCC
- Linux 用户行为不变（仍然默认 GCC musl 静态链接）
- 已配置 `[toolchain]` 或全局 default 的用户不受影响（优先级 1/2 覆盖）

### 其他适配点

1. **First-run UI 消息**：macOS 上显示 "installing llvm@20.1.7 as default" 而非 musl
2. **`mcpp new` 模板**：可选在生成的 mcpp.toml 中加入 `[toolchain] macos = "llvm@20.1.7"`
3. **错误消息**：MCPP_NO_AUTO_INSTALL 的提示信息也需平台感知

## CI 验证计划

在 macos-15 runner 上验证完整链路：
1. xlings bootstrap ✅（已验证可用）
2. xlings install llvm → 安装 LLVM 到 ~/.xlings
3. 使用 LLVM clang++ 编译 `import std` 项目
4. 验证 mcpp 的探测逻辑（target triple, sysroot, libc++ module 路径）
