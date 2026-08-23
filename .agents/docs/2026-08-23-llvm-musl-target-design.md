# llvm-musl 静态 target 支持

状态：设计（2026-08-23）

## 问题

`mcpp build --target x86_64-linux-musl` 目前把 musl 目标硬绑到 gcc@16.1.0
（`to_xim_package` 的 `Family::Gcc` + musl triple 分支）。GCC 16.1.0 的
modules 实现有未修复的 ICE（BMI 读回期段错误，gdb 现场为编译器堆内
乱码指针；尚未在 gcc bugzilla 立案），而 clang 22.1.8 编同一模块重度
代码库全绿。
llvm 家族缺的是 musl 侧的 payload：clang 自身不携带 musl libc/libc++。

## 前置事实（已在一个 ~240 个模块 TU 的工作区验证）

1. clang 22.1.8 payload 的 `bin/clang.cfg` 强注 host glibc 路径；musl 构建
   必须让驱动回退中性（`--no-default-config`，或等价地清空 cfg 影响）。
2. musl libc++/libc++abi/libunwind 可由 host clang 以
   `LLVM_ENABLE_RUNTIMES` 交叉构建（x86_64 与 aarch64 双份，含
   `import std` 模块源 std.cppm）；crt/libgcc 复用 musl-gcc payload
   （`--gcc-toolchain` 指向它 + `-rtlib=libgcc -unwindlib=libgcc`）。
3. clang 已知 bug 规避在用户代码侧（PCM wchar 误推导），不阻塞工具链。

## 设计

### 1. payload（分发层）

新 xim 包 `llvm-musl`（每 target arch 一份资产）：
- clang 前端（复用 llvm payload 的二进制，不重复分发）
- `<prefix>/musl/<triple>/`：sysroot（musl-gcc payload 借）+ 自建 libc++ 等
  静态库与 std.cppm

`to_xim_package`：`Family::Llvm` + musl triple 时映射到该包，
frontendCandidates 仍是 `clang++`。

### 2. linkmodel（渲染层）

`resolve_link_model` 增加 llvm-musl 分支（在 `clangWithCfg` 分支之前）：
- `mode = Sysroot`，root = payload 的 musl sysroot
- 额外 tokens：`--no-default-config --gcc-toolchain=<musl-gcc>
  -rtlib=libgcc -unwindlib=libgcc -nostdinc++ -isystem <libcxx>/include/c++/v1
  -nostdlib++ -L<libcxx>/lib -lc++ -lc++abi`
- 全静态：`-static`（沿用 `supports_full_static` 的 target 语义）
- crt 查找：musl triple 前缀规则已存在（binutils_tool 的 cross 分支）

### 3. stdmod（模块层）

llvm-musl 的 std.pcm 必须 `--precompile` 自 payload 里的 musl libc++
std.cppm（不能复用 build-cache 的 glibc 版）。在 stdmod 解析处按
`is_musl_target && is_clang` 选源。

### 4. 别名与默认

- `llvm@<v> --target <triple>-linux-musl` 全形态可用
- 不改变 gcc-musl 的现有默认；llvm-musl 是显式选择

## 验收

- 新包安装后 `mcpp build --toolchain llvm@22.1.8 --target
  x86_64-linux-musl` 对该模块重度工作区编译通过
- 产物 `file` 为 statically linked，`ldd` 报 not a dynamic executable
- aarch64 交叉 + qemu 冒烟
- 现有 gcc/llvm host 路径零回归（tests/unit 全绿）

## 开放问题

- `llvm-musl` 包的资产组装脚本放 xim-pkgindex 还是 mcpp 仓（倾向前者，
  遵循 payload 归属 xim 的既有分工）
- clang.cfg 的 `--no-default-config` 是否应做成 Toolchain 结构里的显式
  开关而非渲染层字符串（倾向后者先行，够用）
