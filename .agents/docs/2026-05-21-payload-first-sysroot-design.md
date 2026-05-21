# 设计方案：Payload-first 工具链环境管理

## 背景

mcpp 当前通过 `--sysroot` 指向 xlings subos 来为工具链提供 C 库头文件和内核头文件。
这导致了对 xlings 内部结构（subos）的隐式依赖，且 subos 的完整性不可控。

## 设计原则

**mcpp 只把 xlings 当包索引 + 资源下载工具。所有编译环境由 mcpp 从 xpkgs payload
细粒度组装，不依赖 xlings 的 subos 或其他内部功能。**

## 当前问题

xlings 中相关的 xpkgs 包：

| 包名 | 内容 | 路径 |
|------|------|------|
| `xim-x-glibc/2.39` | glibc 头文件 + 运行时库 | `include/` (130 头文件) + `lib64/` (libc.so, crt*.o, ld-linux) |
| `scode-x-linux-headers/5.11.1` | Linux 内核头文件 | `include/linux/`, `include/asm/`, `include/asm-generic/` |
| `xim-x-gcc/16.1.0` | GCC 编译器 | C++ 标准库头文件 + 编译器内建头文件 + 运行时库 |
| `xim-x-llvm/20.1.7` | LLVM/Clang 编译器 | libc++ 头文件 + clang 内建头文件 + 运行时库 |

这些包已经在 xpkgs 中了，但 mcpp 没有直接利用它们的路径，而是依赖 subos
（subos 实际上是 xlings 从这些包 + 宿主系统组装的一个合并目录）。

## 目标设计

### 核心思路

mcpp 在 detect 阶段从工具链 binary 位置推导出 sibling xpkgs（glibc、linux-headers），
在 flags 阶段直接用这些 payload 路径组装编译和链接参数，不使用 `--sysroot`。

### 路径推导链

```
compiler binary (e.g. ~/.mcpp/registry/data/xpkgs/xim-x-gcc/16.1.0/bin/g++)
    │
    ├── xpkgs_from_compiler() → ~/.mcpp/registry/data/xpkgs/
    │
    ├── find_sibling_tool("glibc")
    │   → ~/.mcpp/registry/data/xpkgs/xim-x-glibc/2.39/
    │   ├── include/        → glibc 头文件 (-isystem)
    │   └── lib64/          → 运行时库 (-L, -rpath, -B for crt*.o)
    │
    └── find_sibling_tool("linux-headers")  [优先 scode-x-linux-headers]
        → ~/.mcpp/registry/data/xpkgs/scode-x-linux-headers/5.11.1/
        └── include/        → linux/, asm/, asm-generic/ (-isystem)
```

### 环境检查

mcpp 在 detect 阶段主动检查工具链环境完整性，而不是被动等到编译报错。

```cpp
struct SysrootPaths {
    std::filesystem::path glibcInclude;     // glibc headers
    std::filesystem::path linuxInclude;     // linux kernel headers
    std::filesystem::path glibcLib;         // glibc runtime (lib64/)
    std::filesystem::path dynamicLinker;    // ld-linux-x86-64.so.2
};

// detect 阶段调用：
std::expected<SysrootPaths, DetectError>
probe_sysroot_paths(const std::filesystem::path& compilerBin);
```

检查项：

| 检查 | 验证文件 | 失败提示 |
|------|---------|---------|
| glibc 头文件 | `glibc/include/features.h` | `glibc xpkg not found; run: mcpp toolchain install gcc` |
| linux 内核头文件 | `linux-headers/include/linux/limits.h` | `linux-headers package missing; will use host /usr/include as fallback` |
| glibc 运行时 | `glibc/lib64/libc.so.6` | `glibc runtime not found` |
| 动态链接器 | `glibc/lib64/ld-linux-x86-64.so.2` | `dynamic linker not found` |

### 编译 flags 组装

#### GCC（当前用 --sysroot，改为 -isystem + -L）

**修改前**：
```
g++ -std=c++23 --sysroot=<subos> ...
```

**修改后**：
```
g++ -std=c++23 \
    -isystem <glibc>/include \
    -isystem <linux-headers>/include \
    -L<glibc>/lib64 \
    ...
```

注意：GCC 使用相对路径自动找到自己的 C++ 标准库头文件和编译器内建头文件，
不需要额外的 `-isystem`。只需提供 glibc 和 linux-headers 的路径。

#### Clang（当前依赖 cfg，改为 --no-default-config + 显式路径）

**修改前**：
```
# cfg 中的路径（指向 ~/.xlings/，可能 stale）
clang++ --sysroot=<xlings-subos> -isystem <xlings-llvm>/include/c++/v1 ...
```

**修改后**：
```
clang++ --no-default-config \
    -stdlib=libc++ \
    -isystem <llvm>/include/c++/v1 \
    -isystem <llvm>/include/<triple>/c++/v1 \
    -isystem <glibc>/include \
    -isystem <linux-headers>/include \
    -fuse-ld=lld \
    --rtlib=compiler-rt \
    --unwindlib=libunwind \
    -L<llvm>/lib/<triple> \
    -L<glibc>/lib64 \
    ...
```

这里 mcpp 显式提供了 cfg 中所有必要的 flags，不再依赖 cfg 文件。
Linux 和 macOS 走同一套逻辑（macOS 用 xcrun SDK 替代 glibc/linux-headers）。

### Toolchain 数据模型扩展

```cpp
struct Toolchain {
    // ... 现有字段 ...

    // 替代 sysroot 的细粒度路径
    struct PayloadPaths {
        std::filesystem::path glibcInclude;     // glibc headers
        std::filesystem::path glibcLib;          // glibc lib64
        std::filesystem::path linuxInclude;      // linux kernel headers
        std::filesystem::path dynamicLinker;     // ld-linux path
    };
    std::optional<PayloadPaths> payloadPaths;    // 非空 = 使用 payload 模式
};
```

当 `payloadPaths` 有值时，flags 组装使用细粒度路径；
当 `payloadPaths` 为空时（系统编译器、用户自定义），回退到 `sysroot` 或不传。

### GCC specs fixup 的演进

当前 `fixup_gcc_specs()` 重写 specs 中的 dynamic-linker 和 rpath 路径，
指向 glibc xpkg 的 `lib64/`。这已经是 payload-first 的做法。

未来可以考虑在 specs fixup 中同时重写 sysroot：
```
# specs 中添加:
*sysroot:
<glibc-xpkg-root>
```
这样 GCC 自身就知道正确的 sysroot，不需要命令行 `--sysroot`。
但这需要更深入理解 specs 格式，作为 Phase 3 推进。

### Clang cfg fixup（新增）

类似 `fixup_gcc_specs()`，在 toolchain install 时重写 `clang++.cfg`：

```cpp
void fixup_clang_cfg(const std::filesystem::path& payloadRoot,
                     const PayloadPaths& paths) {
    auto cfgPath = payloadRoot / "bin" / "clang++.cfg";
    // 重写:
    //   --sysroot=<old> → 删除（由 mcpp 显式传递）
    //   -isystem <old>  → -isystem <llvm>/include/c++/v1
    //   -L<old>         → -L<llvm>/lib/<triple>
    //   -Wl,--dynamic-linker=<old> → -Wl,--dynamic-linker=<glibc>/lib64/ld-linux
    //   -Wl,-rpath,<old> → -Wl,-rpath,<glibc>/lib64
}
```

这样即使不用 `--no-default-config`，cfg 中的路径也是自洽的。

## 实施路线

### Phase 1（当前 PR #62 已完成）

- 删除 M5.5 subos 覆盖
- `probe_sysroot` 增加 cfg 解析 + GCC stale path remap
- macOS Clang 保持 `--no-default-config` + xcrun

**效果**：修复了 bug，但 GCC sysroot 仍依赖 registry subos，Clang 仍依赖 cfg 中的
xlings 路径。

### Phase 2：Payload-first 路径探测

- Toolchain 模型增加 `PayloadPaths` 字段
- `probe_sysroot_paths()` 通过 `find_sibling_tool()` 定位 glibc 和 linux-headers xpkgs
- 环境检查：detect 阶段验证关键头文件和库文件存在
- 缺失 linux-headers 时给出明确提示，或自动触发安装

**改动文件**：
- `src/toolchain/model.cppm` — PayloadPaths 结构
- `src/toolchain/probe.cppm` — probe_sysroot_paths() 实现
- `src/toolchain/detect.cppm` — 调用 probe_sysroot_paths

### Phase 3：Payload-first flags 组装

- `flags.cppm` 使用 PayloadPaths 组装 `-isystem` / `-L` / `-B` 替代 `--sysroot`
- `stdmod.cppm` 同步使用 PayloadPaths
- GCC 和 Clang 走统一的 flags 组装逻辑
- 去掉对 cfg 文件的运行时依赖

**改动文件**：
- `src/build/flags.cppm` — 核心 flags 组装
- `src/toolchain/stdmod.cppm` — std module 预编译
- `src/toolchain/clang.cppm` — clang std module build commands

### Phase 4：cfg/specs fixup 统一

- toolchain install 时对 Clang cfg 做 fixup（类似 `fixup_gcc_specs`）
- 使 payload 完全自洽，不依赖 xlings 原始路径
- 去掉 `--no-default-config`（cfg 本身已正确）

**改动文件**：
- `src/cli.cppm` — fixup_clang_cfg() + install 流程集成

### Phase 5（可选）：依赖声明

在 xpkgs 包描述中声明 sysroot 依赖关系：
```toml
[toolchain.gcc]
requires = ["glibc", "linux-headers", "binutils"]
```
mcpp 在 toolchain install 时自动检查和安装这些依赖包。

## 设计对比

| 维度 | 当前（subos） | 目标（payload-first） |
|------|-------------|---------------------|
| sysroot 来源 | xlings subos（合并目录） | glibc + linux-headers xpkgs |
| 路径控制 | 被动依赖 xlings 初始化 | mcpp 主动探测和组装 |
| 环境检查 | 无（编译时才报错） | detect 阶段验证 |
| cfg/specs | cfg stale + specs fixup | 两者都 fixup，路径自洽 |
| 跨平台 | macOS/Linux/Windows 各自特殊处理 | 统一的 PayloadPaths 抽象 |
| 错误提示 | `linux/limits.h not found` | `linux-headers package missing` |
| xlings 依赖 | subos + xpkgs + cfg 路径 | 只依赖 xpkgs 文件 |
