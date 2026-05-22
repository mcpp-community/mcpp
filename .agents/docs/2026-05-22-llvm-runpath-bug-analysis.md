# Bug 分析：LLVM 共享库 RUNPATH 失效的完整链路

**Date**: 2026-05-22

## 一、mcpp 的隔离环境架构

mcpp 自包含了一个 xlings 环境，XLINGS_HOME 指向 `~/.mcpp/registry/`：

```
~/.mcpp/
  registry/                        ← mcpp 的 XLINGS_HOME
    bin/xlings                     ← vendored xlings 二进制
    .xlings.json
    data/xpkgs/                    ← mcpp 的 xpkg 存储
      xim-x-llvm/20.1.7/
      xim-x-gcc/16.1.0/
      xim-x-glibc/2.39/
      ...
```

理论上 `mcpp toolchain install llvm` 会通过
`XLINGS_HOME=~/.mcpp/registry/ xlings install llvm`
让 xlings 直接安装到 mcpp sandbox 里。

## 二、实际发生了什么

但 xlings 的子进程 **XLINGS_HOME 传播不可靠**（代码注释原文：
"xlings subprocess XLINGS_HOME propagation is unreliable"），导致 xlings 把
LLVM 安装到了 `~/.xlings/`（全局 xlings 路径）而不是 `~/.mcpp/registry/`。

mcpp 有一个 workaround（`src/pm/package_fetcher.cppm:607-639`）：发现 xpkg
不在 sandbox 里时，从 `~/.xlings/data/xpkgs/` **原样拷贝** 到
`~/.mcpp/registry/data/xpkgs/`：

```cpp
// Workaround: xlings may extract large packages (e.g. LLVM) into its
// global data dir instead of the mcpp sandbox, because the extraction
// subprocess doesn't always inherit XLINGS_HOME.
std::filesystem::copy(src, verdir,
    std::filesystem::copy_options::recursive
    | std::filesystem::copy_options::overwrite_existing, ec);
```

## 三、完整安装链路

```
LLVM tarball
  bin/: RUNPATH = $ORIGIN/../lib (相对路径，可移植)
  lib/libc++.so.1: 无 RPATH
    ↓
xlings install llvm (全局 ~/.xlings/ 环境)
  xlings elfpatch: 给所有 ELF 加 RUNPATH → ~/.xlings/...
  xlings __install_linux_cfg(): 生成 clang++.cfg → ~/.xlings/...
    ↓
~/.xlings/data/xpkgs/xim-x-llvm/20.1.7/
  所有 ELF 的 RUNPATH → ~/.xlings/...
  clang++.cfg 路径 → ~/.xlings/...
    ↓
mcpp resolve_xpkg_path(): 发现 sandbox 里没有 → 从 ~/.xlings/ 原样拷贝
    ↓
~/.mcpp/registry/data/xpkgs/xim-x-llvm/20.1.7/
  所有 ELF 的 RUNPATH 仍然 → ~/.xlings/...     ← 问题所在
  clang++.cfg 路径仍然 → ~/.xlings/...
    ↓
mcpp fixup_clang_cfg(): 修正 clang++.cfg 文本中的路径
    ↓
最终状态：
  clang++.cfg  ✅ 正确指向 ~/.mcpp/registry/...
  lib/*.so     ❌ RUNPATH 仍指向 ~/.xlings/...
  bin/*        ❌ RUNPATH 仍指向 ~/.xlings/...（但 mcpp 用 LD_LIBRARY_PATH 兜底）
```

## 四、为什么 GCC 没问题但 LLVM 有问题

### GCC 路径（正确 ✅）
```
copy → patchelf_walk(payload->root, ...) → fixup_gcc_specs()
       ^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^^
       遍历所有 ELF，重写 PT_INTERP + RUNPATH → mcpp registry 路径
```

GCC 安装后调了 `patchelf_walk()` 对整个 payload 做了 ELF 修正
（`src/cli.cppm:3794`），所以 GCC 的 bin/ 和 lib/ 下所有文件的 RUNPATH 都被
正确重写。

### LLVM 路径（Bug ❌）
```
copy → fixup_clang_cfg() ONLY
       ^^^^^^^^^^^^^^^^^
       只修正了 clang++.cfg 文本，未触碰任何 ELF
```

LLVM 安装后只调了 `fixup_clang_cfg()`（`src/cli.cppm:3825`），修正了 cfg 文件中
的文本路径。但所有 ELF 文件（bin/ 和 lib/ 下）的 RUNPATH 仍然指向 xlings 路径。

## 五、为什么运行时才暴露

### 编译阶段：不受影响
clang++ 编译用户代码时，linker flags 来自 clang++.cfg（已被 fixup_clang_cfg
修正）。所以 `-L` 和 `-Wl,-rpath` 正确指向 mcpp registry。**编译出的二进制的
RUNPATH 是正确的。**

### 运行阶段：RUNPATH 不传递

```
hh (RUNPATH → ~/.mcpp/registry/...)                           ← 正确（来自 cfg）
 └→ libc++.so.1        ✅ 通过 hh 的 RUNPATH 找到
     └→ libatomic.so.1  ❌ 用 libc++.so.1 的 RUNPATH 搜索
                            → ~/.xlings/... → 路径不存在 → 失败
```

ELF 规则：**RUNPATH 是非传递的**。loader 搜索 `libatomic.so.1` 时用的是
请求方 `libc++.so.1` 自己的 RUNPATH，不是 `hh` 的 RUNPATH。

### CI 碰巧通过
GitHub Actions ubuntu-24.04 预装了 `libatomic.so.1` 在 `/usr/lib/x86_64-linux-gnu/`。
loader 在 RUNPATH 搜索失败后 fallback 到系统路径，碰巧找到了。

## 六、为什么 bin/ 下工具能跑

mcpp 在运行 clang++ 时会设置 `LD_LIBRARY_PATH`
（`src/platform/linux.cppm:32-48`）：

```cpp
env LD_LIBRARY_PATH='/home/speak/.mcpp/registry/.../lib:...'
    clang++ ...
```

`LD_LIBRARY_PATH` 优先级高于 RUNPATH，所以 clang++ 自身虽然 RUNPATH 指向
xlings 路径，但通过 `LD_LIBRARY_PATH` 还是能找到依赖。

另外，在同一台机器上 xlings 和 mcpp 共存时，`~/.xlings/` 路径实际上也是有效的，
所以 bin/ 下的工具即使 RUNPATH 指向 xlings 路径也能跑。

但**用户编译出的二进制不会被设置 LD_LIBRARY_PATH**——用户运行 `mcpp run` 或
手动执行二进制时，只靠 ELF 内嵌的 RUNPATH + 系统路径。

## 七、Bug 归属

| 层 | 行为 | 正确性 |
|----|------|--------|
| LLVM tarball | bin/ 用 `$ORIGIN/../lib`，lib/ 无 RPATH | ✅ 正确 |
| xlings elfpatch | 给所有 ELF 加绝对 RUNPATH → `~/.xlings/` 路径 | ✅ 对 xlings 正确 |
| xlings XLINGS_HOME 传播 | 子进程可能忽略 XLINGS_HOME | ⚠️ xlings 已知限制 |
| mcpp copy workaround | 从 `~/.xlings/` 原样拷贝到 sandbox | ⚠️ 已知 workaround |
| **mcpp LLVM post-install** | **只修正 cfg，不修正 ELF RUNPATH** | **❌ 遗漏** |
| mcpp GCC post-install | patchelf_walk + specs fixup | ✅ 完整 |

**Bug 属于 mcpp 侧**：mcpp 知道拷贝后的 ELF 路径不对（对 GCC 已有完整修正），
但对 LLVM 遗漏了 `patchelf_walk()` 这一步。

如果 xlings 修复了 XLINGS_HOME 传播问题（直接安装到 mcpp sandbox），
xlings elfpatch 会写入 `~/.mcpp/registry/...` 路径，这个 bug 就不会出现。
但在那之前，mcpp 侧的 post-install fixup 是正确的兜底方案。

## 八、修复

在 LLVM post-install 块中，`fixup_clang_cfg()` 之前加入 `patchelf_walk()`，
但只走 `lib/` 目录（不走 `bin/`）：

- `lib/` 下的 .so：RUNPATH 必须修正（libc++.so.1 → libatomic.so.1 依赖链）
- `bin/` 下的工具：mcpp 通过 LD_LIBRARY_PATH 运行，且同机器上 xlings 路径
  通常有效，暂不修正

注意：严格来说 `bin/` 下的 RUNPATH 也是错的（指向 xlings 路径而非 mcpp
registry），但因为 mcpp 总是通过 LD_LIBRARY_PATH 调用这些工具，所以影响不大。
未来可以考虑也修正 bin/，但需要在 rpath 中加入 zlib/libxml2 等依赖路径。
