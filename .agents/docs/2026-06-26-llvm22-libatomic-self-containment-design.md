# LLVM libatomic 自包含缺口分析与修复设计(libatomic.so.1 cannot open)

Date: 2026-06-26

## Summary

升级到 `llvm@22.1.8` 后,`mcpp run` 编译通过但**运行期崩溃**:

```
error while loading shared libraries: libatomic.so.1: cannot open shared object file
```

切回 `llvm@20.1.7` 一切正常。根因不在 mcpp 的构建/RUNPATH 逻辑,而是 **llvm 22.1.8 的 "slim self-contained" 重打包(xim-pkgindex #317)把 `libatomic.so.1` 从包里删掉了**,而该包的 `libc++.so.1` 仍带着一个对 `libatomic.so.1` 的依赖。配合排查还暴露出第二个**早已存在(20/22 皆有)**的问题:真正使用 16 字节原子的用户程序因链接行缺 `-latomic` 而链接失败。

本文记录完整证据、根因、以及落地修复方案。结论:

- **bug #1(22 回归,全崩)**:任何产物(连 hello world)运行期挂 libatomic。
- **bug #2(20+22 旧问题,小众)**:真用 `std::atomic<16字节>` 的程序链接报 `undefined __atomic_*_16`。

**采纳的修复:① + ③(本仓 + 打包),② 反馈上游 LLVM。**

- **①(mcpp)**:链接行追加 `-latomic -Wl,--as-needed`,修 bug #2。
- **③(llvm 打包)**:把 `libatomic.so.1` 带回包内 `lib/x86_64-unknown-linux-gnu/`(对齐 20.1.7),是 bug #1 与 bug #2 的共同基石。
- **②(上游)**:libc++.so.1 的"幽灵依赖"(`-latomic` 未配 `--as-needed`)反馈给上游 LLVM/libc++ 打包,本仓不做 patchelf 摘除。

## 复现与现场

项目为 `mcpp new` 模板(`import std; std::println(...)`)。

```
mcpp toolchain default llvm 22.1.8 && mcpp run
  Compiling ... Finished ... Running ...
  <bin>: error while loading shared libraries: libatomic.so.1: cannot open shared object file

mcpp toolchain default llvm 20  && mcpp run
  Hello from llvm-test!        # 正常
```

## 根因调查(证据)

### 1. 产物本身不直接依赖 libatomic —— 缺的是传递依赖

20 与 22 的最终产物 `DT_NEEDED` **完全一致**,都没有直接 `libatomic`:

```
NEEDED libc++.so.1  libc++abi.so.1  libunwind.so.1  libm.so.6  libc.so.6
RUNPATH <llvm>/lib/x86_64-unknown-linux-gnu : <llvm>/lib : <glibc>/lib64
```

真正带 `NEEDED libatomic.so.1` 的是 `libc++.so.1`(20、22 都带)。

### 2. 差异只在打包:20 自带 libatomic,22 整包没有

```
xim-x-llvm/20.1.7/lib/x86_64-unknown-linux-gnu/libatomic.so.1  → 存在(软链 → .so.1.2.0)
xim-x-llvm/22.1.8/...                                            → find libatomic* 为空
```

libc++.so.1 的 RUNPATH 第一项正是 `<llvm>/lib/x86_64-unknown-linux-gnu`。20 在该目录里有 libatomic → 解析成功;22 该目录里没有 → 失败。

### 3. 为什么不回退系统 libatomic —— 自包含 loader

产物 PT_INTERP = 沙箱 `xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2`。该 loader 默认搜索/ld.so.cache 不含宿主 `/lib/x86_64-linux-gnu`;且可执行文件的 `DT_RUNPATH` **非透传**(不用于解析 libc++ 的子依赖)。这正是 mcpp/xlings"不依赖 host"的设计 —— 因此宿主即便有 libatomic 也不该、也不会被用上。

### 4. RUNPATH 是 mcpp 自己设的(不是上游/xlings)

`src/toolchain/lifecycle.cppm:376-419` 在 llvm 包 post-install 阶段,用 patchelf 对 `lib/` 下所有 .so `--set-rpath` 为
`<llvm>/lib/x86_64-unknown-linux-gnu : <llvm>/lib : <glibc>/lib64`(`post_install.cppm` 的 `patchelf_walk`)。其代码注释明确写着这么设就是为了让
"libatomic.so.1 这种传递依赖被找到 —— 它住在同一个 xpkg 里"。

即 **mcpp 显式假设 libatomic 打在包内**,RUNPATH 已正确指向该目录;22.1.8 slim 删掉文件 = 直接违背 mcpp 代码写明的契约。RUNPATH 没错,错在文件缺失。

### 5. 反转:libc++ 的 libatomic 依赖是"幽灵依赖"(over-link)

`libc++.so.1`(20、22 都一样)带 `NEEDED libatomic.so.1`,但对 libatomic 的**未定义符号数 = 0**(libc++abi、libunwind 同为 0):

```
readelf --dyn-syms libc++.so.1 | grep UND | grep atomic   → 空
```

即构建 libc++ 时挂了 `-latomic` 但**没配 `--as-needed`**,留下一个没人引用的 `NEEDED`。ELF loader 对每个 `DT_NEEDED` 仍做存在性加载(不管用不用),所以文件缺失即启动失败。

### libatomic 的归属

`libatomic` 不是 LLVM 的,也不是 glibc 的,而是 **GCC 运行时库**(`__atomic_*` 是编译器无关 ABI;GNU 平台由 GCC 独家提供,LLVM/compiler-rt 不发布独立 libatomic)。实测:`xim-x-glibc/2.39` 无 libatomic;`xim-x-gcc/16.1.0/lib64`、`xim-x-gcc-runtime` 有。20.1.7 包内那份就是从 GCC 拷进去的(软链 → `libatomic.so.1.2.0`,与 gcc 同版本)。包内 `libunwind`/`libc++abi` 是 LLVM 自家的,唯 libatomic 借 GCC。

## 本地验证矩阵(已实测)

两个 mcpp 项目钉 `llvm@22.1.8`:`normal`(import std)、`atomic`(`std::atomic<alignas(16) Big>`,强制 `__atomic_*_16`):

| | normal(import std) | atomic(16 字节,真用 libatomic) |
|---|---|---|
| 出厂基线 | ❌ 运行期 `libatomic cannot open` | ❌ **链接**就挂 `undefined __atomic_load_16/store_16/compare_exchange_16` |
| 仅摘 libc++ 幽灵依赖(②) | ✅ 正常 | ❌ 仍链接失败(无 libatomic 可链) |
| 仅带回 libatomic(③) | ✅ 正常 | ❌ 仍链接失败(mcpp 链接行无 `-latomic`,库在场也白搭) |
| 手动 `clang++ -L<llvmlib> -latomic` | — | ✅ 链接+运行 `a=1 b=2 ok=1 lockfree=0` |
| 对照:同一 atomic 程序在 llvm 20.1.7(无 -latomic) | — | ❌ **同样**链接失败 → bug #2 非 22 回归 |

结论:
- bug #1 由 ③(带回库)单独即可修好(20.1.7 即此形态);② 是上游正确做法,可选清洁。
- bug #2 需要 ①(链接行 `-latomic`)+ ③(库在场)同时满足;`lockfree=0` 证实 16 字节原子按 ABI 走 libatomic 锁实现。

## 为什么会有这两个 bug(统一视角)

二者是"libatomic 链接处理不一致"同一根源的两面,核心是没人一致地用 `-latomic -Wl,--as-needed`:

| 在哪链接 | 当前做法 | 症状 |
|---|---|---|
| 上游构建 libc++ | 加了 `-latomic`,**未配** `--as-needed` | 没用却留 NEEDED → bug #1(幽灵依赖) |
| mcpp 链接用户程序 | **根本没加** `-latomic` | 真用却没链上 → bug #2(undefined) |

`--as-needed` 恰好提供"没用就丢、真用就留"的条件行为。把它补在这两处,加上 libatomic 始终在场(③),两个症状都消失。

16 字节原子为何必须 out-of-line:x86-64 上 ≤8 字节原子映射单条 `lock` 指令(内联);16 字节需 `cmpxchg16b`(基线不保证、且纯读 load 会写内存破坏 const 语义),故编译器保守发 `__atomic_*_16` libcall,由 libatomic 锁实现 —— 这是 ABI 规范,非 bug。

## 修复设计

### ①(本仓 mcpp):链接行追加 `-latomic --as-needed`

- **落点**:`src/build/flags.cppm` 的 LLVM/clang 分支(约 193-195,设 `-stdlib=libc++ -fuse-ld=lld --rtlib=compiler-rt --unwindlib=libunwind` 处),把原子库追加进 ldflags(`f.ld`)。
- **链接规则**:`ninja_backend.cppm:375` 为 `command = $cxx $in -o $out $ldflags $unit_ldflags`。`$in`(目标文件)在 `$ldflags` 之前,满足 `--as-needed` 的顺序要求(库须出现在引用它的目标文件之后)。
- **建议片段**(放在 ldflags 末尾):

  ```
  -Wl,--push-state,--as-needed -latomic -Wl,--pop-state
  ```

  `--push-state/--pop-state` 隔离 `--as-needed`,不污染其它库的链接语义。
- **效果**:用户程序真用原子 → 自动链上并保留 NEEDED;没用 → `--as-needed` 自动丢弃,产物零额外依赖。
- **前提**:链接器要能找到 libatomic → 依赖 ③ 把库放进 `lib/x86_64-unknown-linux-gnu`(已在 -L/RUNPATH 内,与 libc++ 同目录)。
- **范围**:本设计聚焦 LLVM 工具链。GCC 工具链同样有 bug #2(其 libatomic 已在 gcc lib 目录 + RUNPATH 内),建议 ① 做成对 gcc/llvm 通用;最低限度先覆盖 LLVM。

### ③(llvm 打包):把 libatomic 带回包内

- **落点**:llvm 子打包脚本 `.agents/tools/build-llvm-subpkg.sh`(及 `.agents/skills/llvm-subpackaging/`)。
- **做法**:打包时从 GCC 运行时拷 `libatomic.so.1.2.0` 进 `lib/x86_64-unknown-linux-gnu/`,并建 `libatomic.so.1 → libatomic.so.1.2.0`、`libatomic.so → libatomic.so.1` 软链,对齐 20.1.7 的布局。
- **效果**:libc++ 的(幽灵)NEEDED 可解析(修 bug #1);为 bug #2 提供可链接/可加载的库;且全程不依赖 host。
- **代价**:每个 llvm 包多约 187KB 一个小库,换取"任何场景不比 20 倒退"。
- **重发**:更新 asset → XLINGS_RES → 用户 `mcpp toolchain remove llvm 22.1.8 && reinstall`。

### ②(反馈上游,本仓不做):libc++ 幽灵依赖

- **现象**:上游(或 xlings 重打包所基于的)libc++.so.1 以 `-latomic` 链接但未配 `--as-needed`,产生 0 引用的 `NEEDED libatomic.so.1`。
- **上游正确修法**:构建 libc++ 时用 `-latomic -Wl,--as-needed`,或对发布产物 `patchelf --remove-needed libatomic.so.1 libc++.so.1`。
- **本仓决策**:不在 mcpp post-install 摘除(避免对上游产物做隐式改写),改为向上游 LLVM/打包反馈。在 ③ 已带回 libatomic 的前提下,该幽灵依赖不影响正确性,仅是可加载性上的轻微噪音。

### 方案组合判定

- 最小可用集 = **① + ③**(等于"20.1.7 形态" + "mcpp 自动补 -latomic"),两个 bug 全好。
- ②(摘幽灵)为上游清洁项,做了更干净(普通进程根本不加载 libatomic),不做也不影响正确性。

## 验收标准

- `mcpp run`(import std hello,toolchain=llvm@22.1.8)正常输出,产物 `ldd`/`readelf -d` 无未解析依赖。
- 16 字节原子程序 `mcpp build` 链接通过、运行正确(`is_lock_free()==false` 属预期),产物 `NEEDED libatomic.so.1` 且可经 RUNPATH 解析。
- 不使用原子的程序产物**不含** `NEEDED libatomic`(验证 `--as-needed` 生效)。
- 全程不依赖宿主:在无系统 libatomic 的环境(容器/最小化系统)上述两类程序均可运行。

## 关联文档

- `2026-05-22-llvm-runpath-bug-analysis.md` / `2026-05-22-fix-llvm-shared-lib-runpath.md`(同一 RUNPATH/post-install 区域)
- `2026-05-13-llvm-clang-toolchain-support-design.md`(LLVM 工具链抽象)
