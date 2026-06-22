# aarch64 glibc-world / LLVM 工具链构建计划(deferred)

> 状态:**计划,暂不实施**。日期:2026-06-23。
> 前置:musl-static 路线已闭环(见 [2026-06-22-aarch64-linux-ecosystem-support-analysis.md](2026-06-22-aarch64-linux-ecosystem-support-analysis.md))。
> 决策:优先把 **mcpp + musl @ aarch64** 整套做完;glibc-world / LLVM 原生 ABI 路线**留作独立 initiative**,本文档梳理依赖链与构建策略备用。

---

## 1. 为什么需要(动机)

musl-static 路线已能让 aarch64 上 `install mcpp → build/run import std → self-host`。但 musl-static **无法链接 glibc 世界**(X11/GL/系统 .so),所以:
- GUI / 原生 ABI 包(imgui+GLFW、依赖系统 glibc 库的包)在 aarch64 上**开箱不可用**;
- `mcpp build`(无 `--target`)的 glibc 默认 `gcc@16.1.0` 在 aarch64 上仍会 404(#148 已改为非 x86 默认 musl 规避)。

glibc-world 构建 = 为 aarch64 补齐原生 glibc gcc 工具链 + LLVM,启用原生 ABI / 动态链接 / GUI。

---

## 2. 两条工具链链路

### 2.1 GCC-glibc 链(主链)

依赖拓扑(安装顺序),全部 **XLINGS_RES 预编译资产,当前仅 x86_64**:

```
linux-headers@5.11.1 (scode 源码构建)
  → glibc@2.39 ──┐
binutils@2.42 ───┤
gcc-runtime@15.1.0 (运行库:libstdc++/libgcc_s/libatomic/libasan)
gcc-specs-config@0.0.1 (安装期 specs 重写,config-only 空包)
  └────────────→ gcc@16.1.0
```

`gcc@16.1.0` 的 `deps`(`pkgs/g/gcc.lua`):`xim:glibc@2.39`、`xim:binutils@2.42`、`xim:linux-headers@5.11.1`、`xim:gcc-specs-config@0.0.1`。

### 2.2 LLVM 链(可选,`import std` 更顺)

- `llvm@20.1.7`(`pkgs/l/llvm.lua`,XLINGS_RES;`archs` 已含 `arm64` 但仅元数据)
- `llvm-tools@20.1.7`(`pkgs/l/llvm-tools.lua`,**显式 URL** 写死 `linux-x86_64`)
- **最省事**:直接镜像 LLVM 官方 `LLVM-20.1.7-Linux-ARM64.tar.xz` 到 `xlings-res/llvm`,带 libc++ + `std.cppm`/`-print-library-module-manifest-path` 则 aarch64 `import std` 开箱可用(§8 待确认项)。

---

## 3. 资产产出方式与 aarch64 构建策略

**现状**:glibc/gcc/binutils/gcc-runtime 在索引里只有 `XLINGS_RES` 引用,**构建脚本不在 xim-pkgindex 内**(x86_64 资产由外部流程产出,疑似 crosstool-ng / 原生构建)。

**aarch64 构建策略(择一)**:
- **(推荐)原生构建**:在 `ubuntu-24.04-arm` runner 上原生 build gcc-16 + glibc-2.39 + binutils-2.42,产出 native aarch64 工具链(run-on-aarch64, target-aarch64)。每轮编译 ~1–2h。
- **Canadian-cross**:x86_64 上交叉构建 run-on-aarch64 的 gcc,复杂度高。
- **复用上游/发行版**:从 aarch64 发行版或 Bootlin/kernel.org 预编译 native 工具链**重打包**成 xim 资产布局(最快但需精确对齐 sysroot/elfpatch 期望)。

**资产命名**:`<pkg>-<ver>-linux-aarch64.tar.gz`(Linux=`aarch64`;见前文档 §1.4),内层目录同名,布局对齐 x86_64。GLOBAL=github.com/xlings-res、CN=gitcode(`gtc release upload --tag <ver> xlings-res/<pkg> <files>`)。

---

## 4. 各 recipe 必改硬编码(§4 D-1 细化)

引擎按 OS 解析 deps、配方拿不到真实 arch(xpkg 沙箱 `os.arch` 恒 stub `'arm64'`),所以 loader/triple 改动必须保证 **x86_64 与 aarch64 两套都对**;凡是写死 x86_64 的都要 arch 派生或拆分:

| recipe | 硬编码点 | aarch64 目标 |
|---|---|---|
| `g/glibc.lua` | `exports.runtime.loader = "lib64/ld-linux-x86-64.so.2"`、`abi = "linux-x86_64-glibc"`、库清单 `ld-linux-x86-64.so.2` | `lib/ld-linux-aarch64.so.1`(注意 **不在 lib64**)、`linux-aarch64-glibc` |
| `g/gcc.lua` | `__config_linux` 写死 `ld-linux-x86-64.so.2`;`linux_programs`/`compiler_entry` 的 `x86_64-linux-gnu-*` | arch 派生 loader + `aarch64-linux-gnu-*` |
| `g/gcc-specs-config.lua` | `old_dynamic_linker` 表 + `:87 multi-arch? TODO` | 加 aarch64 loader 项 |
| `g/gcc-runtime.lua` | 内层目录 `gcc-runtime-<ver>-linux-x86_64/`、`abi=linux-x86_64-glibc` | arch 化 |
| `l/llvm.lua` | 内层目录 + triple `x86_64-unknown-linux-gnu` + 链接器 | aarch64 |
| `l/llvm-tools.lua` | 显式 URL 表写死 `linux-x86_64` | 加 aarch64 URL |
| 各包 | `archs = {"x86_64"}` | 加 `"aarch64"` |

**关键陷阱**:glibc 的 aarch64 loader 是 `/lib/ld-linux-aarch64.so.1`(x86_64 是 `/lib64/ld-linux-x86-64.so.2`)—— 路径与目录都不同,`elfpatch` INTERP 必须随 arch 切换,否则打错。

---

## 5. mcpp 本体配套(glibc 路线启用后)

- `mcpp.toml`:可新增 `[target.aarch64-linux-gnu]`(glibc 原生 ABI target);
- `src/build/prepare.cppm`:非 x86 默认现为 musl(#148),glibc 资产就绪后可让 aarch64 也支持 glibc 默认或显式 target;
- `src/platform/linux.cppm:85`、`src/build/flags.cppm:336`、`src/pack/pack.cppm:648` 等 loader/triple 硬编码(见前文档 §4 阶段⑤)需 arch 派生(部分已随 musl 路线处理,glibc 路线需复核)。

---

## 6. 验收

- aarch64 上 `mcpp build --target aarch64-linux-gnu`(glibc)产出动态链接、可跑;
- 一个依赖系统 glibc 库的 GUI 包(如 imgui+GLFW)在 aarch64 上 build/run;
- `import std` 经 LLVM aarch64 路线开箱可用。

---

## 7. 待确认

- [ ] LLVM 官方 `Linux-ARM64` 是否带 libc++ `std.cppm` + `-print-library-module-manifest-path`。
- [ ] `ubuntu-24.04-arm` runner 配额是否够跑 ~1–2h 的 gcc/glibc 原生构建。
- [ ] `scode:linux-headers` 是否已有 aarch64 内核头(`asm/`)。
- [ ] gcc-16.1.0 + glibc-2.39 aarch64 原生构建的可重复脚本(crosstool-ng config 或 build.sh)归档何处。
