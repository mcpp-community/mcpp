---
subject: review
status: active
---

# #674 设计方案评审：`-include unistd.h` 在 Windows + `presents = "posix"` 上的可行性

- 被评审对象：`.agents/docs/2026-09-19-issue-674-cenv-posix-preinclude-design.md`（Path C）
- 上游 issue：mcpp-community/mcpp#674
- 评审依据：`origin/main` 361874df（mcpp 2026.9.18.4）；`mcpplibs/mcpp-index` f3c69ac；`mcpplibs/openkal` 871f8f0
- 评审环境：本机 Linux x86_64，clang 22.1.0（`~/.xlings/subos/current/bin/clang`）
- 日期：2026-09-20

---

## 0. 结论

**不建议实施。** 三层理由，每一层单独成立：

1. **方案修不了它要修的东西。** 18 个 `compat.*` 失败的第一条诊断，在 `mcpp-index` 的
   checked-in 测量数据里全部是「找不到某个 Windows 头」（`windows.h` / `_mingw.h` /
   `windef.h` / `io.h` / `intrin.h` / `mm_malloc.h`），**没有一条**是设计稿 §1.3 所写的
   `'lseek' was not declared in this scope`。这些 `#include` 由 `#if defined(_WIN32)` 选中，
   而强制包含一个 POSIX 头**在构造上**改变不了这个条件，也变不出 `windows.h`。

2. **方案的前提不成立。** issue #674 与设计稿 §1 共用一套因果叙述——「引擎已经做到
   `__unix__` 定义、`_WIN32` 不定义，剩下的是上游代码的假设」——这套叙述与三份可查证
   据矛盾：`x86_64-pc-cygwin` 三元组的实测预定义、`mcpp-index` 里那次测量所钉的图、以及
   mcpp#673 实际做的事。真正的成因是**那次测量的图里根本没有任何包声明 `[c-abi]`**，
   于是 cygwin 化实现从未执行，`_WIN32` 仍然定义着。

3. **即便前提成立，实现也会炸。** `-include` 经由 `cEnvTokens` 广播，会同时灌进
   C++ 模块接口单元（含 `std` 模块）、GAS 汇编单元、以及校验探针；并且被
   `appendUniqueFlags` 的按串去重吃掉第二个 `-include`，把 `sys/stat.h` 变成一个位置参数。
   四处都已在本机实测复现。

**真正的下一步在生态侧而不在引擎侧**：把 `openkal-musl 0.15.0` / `openkal-llvm-runtime 0.11.0`
发出去，`tests/openkal/pins.toml` 的 `runtime` 从 `0.10.0` 抬上去，重跑 30-member。在那之前，
Windows 腿测量的不是 `presents = "posix"`，而是「没有 `presents` 时会怎样」。

---

## 1. 方法与判据

本评审不复述文档，只做三件事：读实现、读被引用的数据、在本机实测。所有实测命令与原始
输出列在 §9，可逐条重跑。

判据的选取遵循一条规则：**每个结论都要落在一个与该结论的「否」能区分开的对象上**。
「设计稿说 X」不是证据；「代码在第 N 行做了 Y」「checked-in 的测量文件里记录了 Z」
「本机这条命令打印出 W」才是。

---

## 2. 致命问题：方案对它要修的失败是正交的

### 2.1 失败诊断的真实分布

`mcpplibs/mcpp-index` 的 `.xpkgindex/openkal-compat.json` 是这次测量唯一 checked-in 的
原始数据（`measured: 2026-09-17`，`pins: mcpp 2026.9.17.3 / runtime 0.10.0 / llvm@22.1.8`）。
`x86_64-windows-gnu` 上 15 runs / 15 fails，15 条首诊断逐条如下：

| member | 第一条诊断 |
| --- | --- |
| archive | `xz .../src/common/sysdefs.h:44:11: fatal error: '_mingw.h' file not found` |
| c-ares | `ares_setup.h:81:12: fatal error: 'windows.h' file not found` |
| capi-lua | `lua .../src/loadlib.c:150:10: fatal error: ...` |
| catch2 | `catch_windows_h_proxy.hpp:24:10: fatal error: 'windows.h' file not found` |
| cli11 | `CLI/impl/Argv_inl.hpp:44:10: fatal error: 'windef.h' file not found` |
| cmp-module | `asio/detail/...` |
| curl | `curl_setup.h:33:10: fatal error: '_mingw.h' file not found` |
| doctest | `doctest.h:3215:10: fatal error: 'windows.h' file not found` |
| eigen | `clang/22/include/mm_malloc.h:43:22: error: ...`（clang 自带头） |
| fmtlib.fmt | `clang/22/include/intrin.h:12:15: fatal error: ...`（clang 自带头） |
| libpng | `pngpriv.h:578:12: fatal error: 'windows.h' file not found` |
| mimalloc | `mimalloc/atomic.h:16:10: fatal error: 'windows.h' file not found` |
| re2 | `util/mutex.h:15:10: fatal error: 'windows.h' file not found` |
| spdlog | `fmt/bundled/format-inl.h:20:12: fatal error: 'io.h' file not found` |
| sqlite3 | `sqlite3.c:29506:10: fatal error: 'windows.h' file not found` |

**零条「未声明的标识符」。** 设计稿 §1.3 写的四条形态

```
'lseek' was not declared in this scope   → 缺 <unistd.h>
'fstat' was not declared in this scope   → 缺 <sys/stat.h>
'open'  was not declared in this scope   → 缺 <fcntl.h>
'mmap'  was not declared in this scope   → 缺 <sys/mman.h>
```

在这份数据里一条都不存在。该节以「§1. 实测」为标题呈现，但它不是从这次测量读出来的。

### 2.2 `-include` 与 `#ifdef _WIN32` 是正交的

zlib 的失败信号（设计稿选作代表）来自 `gzguts.h`：

```c
49  #if defined(__TURBOC__) || defined(_MSC_VER) || defined(_WIN32)
50  #  include <io.h>
51  #  include <sys/stat.h>
52  #endif
```

诊断是 `gzguts.h:50:12: fatal error: 'io.h' file not found`。第 50 行**在 `#if` 体内**，
列 12 正是 `<io.h>` 的起始列。因此该诊断只有在第 49 行为真时才可能产生，即
**`_WIN32`（或 `_MSC_VER`/`__TURBOC__`）在那次编译里是定义着的**。

设计稿 §1.1 对同一段代码的解读是「`_WIN32` 未定义，所以走 `#else` 分支，要求 `lseek`
在作用域」。这里有两处与源码不符：这个 `#if` 块**没有 `#else`**（`#else` 在
`gzlib.c` 的另一个 `#if` 里）；而且如果 `_WIN32` 真的未定义，第 50 行不会被预处理，
诊断不会出现在那一行。

更根本的是：**`-include unistd.h` 无法使 `#include <io.h>` 成功**。它既不 undef
`_WIN32`，也不产生 `io.h`。对上表 15 条里的 13 条（全部 `windows.h` / `_mingw.h` /
`windef.h` / `io.h` 类），结论相同。

### 2.3 §1.2 的 glibc/musl 对照表与本机实测相反

设计稿 §1.2 的因果链条是「glibc 的 `<stdio.h>` 传递包含 `<unistd.h>`，musl 不包含，
所以 zlib 在 Linux 上碰巧能编」。本机实测（glibc 2.39，见 §9 M3）：

```
$ printf '#include <stdio.h>\n' | clang -E -xc - | grep -c 'unistd\.h'
0
$ printf '#include <stdio.h>\nlong f(void){ return lseek(0,0,0); }\n' | clang -fsyntax-only -std=c11 -xc -
error: call to undeclared function 'lseek'
```

glibc 的 `<stdio.h>` **不**传递包含 `<unistd.h>`，只包含它之后 `lseek` **不**在作用域。
§1.2 表格的 glibc 行为假，musl 行为真，而整节的论证依赖两行的差。

---

## 3. 前提核查：issue 与设计稿引用的历史都不成立

### 3.1 `_WIN32` 为什么还定义着：那次测量的图里没有 `[c-abi]`

本机实测两个三元组的预定义（§9 M1/M2）：

| 宏 | `--target=x86_64-pc-cygwin` | `--target=x86_64-w64-windows-gnu` |
| --- | --- | --- |
| `_WIN32` / `_WIN64` | 不定义 | **定义** |
| `__MINGW32__` | 不定义 | **定义** |
| `__CYGWIN__` | **定义** | 不定义 |
| `__unix__` / `unix` | **定义** | 不定义 |
| `__SIZEOF_LONG__` | 8 | 4 |
| `__SIZEOF_WCHAR_T__` | 2 | 2 |

即：**cygwin 化实现一旦生效，`_WIN32` 就必然不在**，`gzguts.h:50` 就必然到不了。
既然它到了，那次编译就没有走 cygwin 化实现。

原因写在 `mcpp-index` 自己的 `tests/openkal/pins.toml` 里：

```toml
runtime   = "0.10.0"
...
# `runtime` does NOT move here: openkal-llvm-runtime 0.11.0 (the version that
# actually declares/consumes `[c-abi]`) is not published yet
# ... changes nothing observable today: no package this pin resolves declares `[c-abi]`.
mcpp      = "2026.9.18.3"
```

引擎抬到了 2026.9.18.3，**图没有抬**。`resolvedTargetSide.cAbiDecl` 为空，
`prepare.cppm:10790` 那个 `if` 整块跳过，`cEnvTokens` 为空，命令行逐字节等于该特性
存在之前。所以 Windows 腿测到的是「没有 `presents` 时会怎样」。

与此同时，`mcpp-index` 的 draft 提交 700f7de 已经**先行撤掉**了包侧的适配。撤掉之前，
`pkgs/c/compat.zlib.lua` 里是：

```lua
target_cfg = {
    [<windows>] = { cflags = { "-U_WIN32", "-include", "unistd.h" } },
},
```

撤掉之后只剩 `cflags = { "-include", "mcpp_zlib_config.h" }`，而该生成头是
`#if !defined(_WIN32) → #define Z_HAVE_UNISTD_H 1`。于是在「包侧适配已撤、引擎侧实现
未生效」的这个窗口里，zlib 在 Windows 上同时失去 `-U_WIN32` 与 `Z_HAVE_UNISTD_H`，
恰好落回 `#include <io.h>`。**这是一次发布链顺序造成的窗口，不是 trade-off 剩下的残差。**

### 3.2 mcpp#673 做的不是「只压 `_WIN32`、不定义 `__unix__`」

设计稿 §0.2 与 issue 的 Background 都把 mcpp#673 描述成这个 trade-off 的落地。核对
CHANGELOG `[2026.9.18.3]` 与 7788d3e6 的改动面，#673 实际做了两件事：

1. `cenv::realise` 对 `wchar = 32` **无条件**补 `-fno-short-wchar`（含 freestanding）；
2. `cenv_probe::verify` 新增 `hostStripMacros` 参数，Windows 宿主下传
   `-U_WIN32 -U_WIN64 -U__MINGW32__ -U__MINGW64__`。

两件事都与「是否定义 `__unix__`」无关。而且引擎**从来没有**选择「不定义 `__unix__`」：
`cenv.cppm` 的 Windows + Posix 分支写的是

```cpp
r.expectDefined.push_back("__unix__");
r.expectDefined.push_back("__CYGWIN__");
r.expectUndefined.push_back("_WIN32");
```

`__unix__` 由 cygwin 三元组提供并被探针要求为**已定义**。设计稿与 issue 描述的那个
trade-off 在代码里不存在。

**附带发现（真实缺陷，与本方案无关，建议另开 issue）**：`hostStripMacros` 只进探针
命令（`prepare.cppm:10923-10931`），**从不进 `p.privateBuild.cflags`**。也就是说在
Windows 宿主上，如果 `--target=` 替换确实会漏出宿主的 `_WIN32`（#673 的 CHANGELOG 记录
freestanding 上观察到了），那么探针读到的是剥离后的状态，而真实编译行读到的是未剥离的
状态——**判据施加在了与被测对象不同的命令行上**。issue #674 把 hostStripMacros 列为
「broadcast on the compile line」的一项，与实现不符。这一条在 Linux 宿主上测不出来
（`mcpp::platform::is_windows` 为假，剥离集合为空），而 30-member 的 Windows 腿正是在
Linux 宿主上跑的（`pins.toml` 的 `[runners] x86_64-windows-gnu = ["wine"]`）。

### 3.3 「P3 spike 测过两种呈现并选了一种」没有发生

issue 的 Historical context 引用 `openkal/.agents/docs/2026-09-18-c-environment-execution-plan.md` §4：

> 「`__unix__` 带来新的失败超过收益：改为只不定义 `_WIN32`。」

该句所在的小节标题是 **「## 4. 风险与退出」**，与它并列的是「Cygwin 目标的 TLS、异常展开、
链接驱动不可用：停在 P4」。这是**计划写在实验之前的退出条件**，不是实验结论。

实验结论的位置是 record 的「兼容性测量」，其正文是：

> **兼容性测量。**（待填：新描述文件登记后，与 0.13 基线 Linux 27/3、Windows 15/15 的
> 对比。……）

即**两种呈现的 A/B 从未完成**。design §11 的 review 决定第 4 条「是否定义 `__unix__` 由
实验数据决定（§8 第 4 步）」同样悬着。把一条未执行的退出条件读成已完成的测量结论，
会让后续所有「在这个 trade-off 下补另一半」的论证失去依据。

### 3.4 数目不对

issue 写「Windows side went from `0/30` (0.13) to `12/30`」。checked-in 的 0.13 基线是
**15 runs / 15 fails**（record §4 亦写「Windows 15/15」），不是 0/30。

issue 列出的 18 个名字 = 基线 15 个失败 + `gzip-hpp`、`tinyhttps`、`zlib`。这三个在基线里
**都是 `runs`**。按 issue 自己的清单算，这次变化是**净失去 3 个**，而不是「净得到 12 个」。
失去的恰好是包侧适配被 700f7de 撤掉、而引擎侧替代未生效的那几个（zlib 直接，
gzip-hpp 经由 zlib，tinyhttps 在同一提交里换到 0.3.1）。

### 3.5 Path A 引用的机制不存在

issue 的 Path A 写「mcpplibs/mcpp-index handles each via the
`[target.cfg(os = "windows").patches]` mechanism (one patch per package, ~10-line patch file)」。

mcpp 的清单里**没有 `patches` 键**（`src/manifest/`、`docs/*.md` 全仓无此机制）。
mcpp-index 里真实存在并且已经在用的是 `target_cfg` 下的 `cflags` / `defines` /
`generated_files`——即 §3.1 引的那种形状。设计稿 §2.3 的对照表按「18 个 PR × ~10 行
patch 文件」给 Path A 估成本，这个成本估计的基础是一个不存在的机制。

---

## 4. 即便前提成立，实现也会炸

下列四条各自独立，均在本机实测复现（§9）。

### 4.1 C++ 模块接口单元，包括 `std` 模块

`cEnvTokens` 进的是 `p.privateBuild.cxxflags`（`prepare.cppm:11164`），并且在
`prepare.cppm:12380-12381` 单独再进一次 `std` 模块的编译行。`docs/22-target-side.md`
明写这条广播覆盖「C, C++ and assembly compiles, the dependency scan, and the `std`
module precompile alike」。

模块接口单元必须以 `module;` 或 `export module` 开头。`-include` 把头文件的内容放在
主文件之前，于是：

```
$ clang++ -std=c++23 -include unistd.h --precompile m.cppm -o m.pcm
m.cppm:1:8: error: module declaration must occur at the start of the translation unit
```

带全局模块片段的形状（正是 libc++ `std.cppm` 的形状：注释、`module;`、`#include`、
`export module std;`）同样失败：

```
std_shape.cppm:4:1: error: 'module;' introducing a global module fragment can appear only
                           at the start of the translation unit
std_shape.cppm:8:8: error: module declaration must occur at the start of the translation unit
```

同一份文件去掉 `-include` 即通过。**后果：`import std` 在 Windows × `presents = "posix"`
上直接不可用，图中任何 `.cppm` 亦然。** 这一格恰好是 openkal C++ 侧的旗舰路径
（`openkal-llvm-runtime` + `examples/cxx`）。

设计稿 §4.1 的影响面表格把这一格写成「加 `--target=...` + `-include unistd.h` +
`-include sys/stat.h`」，没有区分该格里的 `.c`、`.cpp`、`.cppm`、`.S` 四类单元。

### 4.2 `appendUniqueFlags` 的按串去重

广播走的是 `prepare.cppm:6380-6391`：

```cpp
for (auto const& f : additions) {
    if (std::find(flags.begin(), flags.end(), f) != flags.end()) continue;
    flags.push_back(f);
}
```

按**整串**判重。设计稿产出的 tokens 序列是
`[..., "-include", "unistd.h", "-include", "sys/stat.h", ...]`，第二个 `"-include"`
与第一个相等，被跳过。落到命令行上是：

```
--target=x86_64-pc-cygwin -include unistd.h sys/stat.h
```

`sys/stat.h` 成为一个位置参数：

```
$ clang -c -include unistd.h sys/stat.h t.c -o t.o
clang: error: no such file or directory: 'sys/stat.h'
```

如果某个包自己的 `cflags` 里已有裸 `-include`（`compat.zlib` 正是
`cflags = { "-include", "mcpp_zlib_config.h" }`），广播的 `-include` 会被**整个**去掉，
两个头文件名都变成位置参数。

单字符令牌形式可以规避（`-includeunistd.h` 与 `--include=unistd.h` 本机均被接受），
但设计稿没有采用，也没有记录这个约束。

### 4.3 汇编广播：GAS 敌对令牌

`prepare.cppm:11166` 把同一批 tokens 送进 `p.privateBuild.asmflags`，并且该处的注释
写下了一条规则：

> Every token in `cEnvTokens`/`cEnvBuiltinsTokens` was checked against clang's GAS
> (`-x assembler-with-cpp`) front end before this was written ... **if a future token
> IS GAS-hostile, `cenv::realise` is where to split it, not this broadcast.**

`-include unistd.h` 正是 GAS 敌对的：

```
$ clang -c -include unistd.h a.S -o a2.o
/usr/include/unistd.h:27:1: error: invalid instruction mnemonic '__begin_decls'
/usr/include/x86_64-linux-gnu/bits/types.h:31:18: error: unexpected token in argument list
...
```

musl 的 `unistd.h` 同样没有 `__ASSEMBLER__` 守卫。设计稿新增了一个满足该规则触发条件的
令牌，但没有按规则在 `realise` 里拆分——这是对被修改文件自己写下的约束的直接违反。
本轮生态里 `.S` 不是边缘情况：openkal-musl 的 `okm_setjmp.S`、vendored libunwind 的
`assembly.h` 都在这条通道上，record §5 里「`[c-abi]` 的实现不作用于汇编源」正是上一轮
专门补的缺陷。

### 4.4 校验探针：没有头文件搜索路径

探针命令（`cenv_probe.cppm`）是

```
<clang> <hostStripMacros> <argv> -x c++ -E -dM -
```

`argv` 里有 `crossTargetFlag` 与 `cEnvTokens`，**没有任何 `-I`、没有 `--sysroot`、
没有 `-nostdlibinc`**——注释明写这是「the IDENTITY-AFFECTING SUBSET」，因为 include path
不改变预定义宏。`-include` 破坏这个不变量：它让探针的成败取决于头文件能否被找到。

本机（Linux 宿主）实测：

```
$ clang --target=x86_64-pc-cygwin -include unistd.h -include sys/stat.h -x c++ -E -dM - </dev/null
rc=0
$ clang --target=x86_64-pc-cygwin -include unistd.h -x c++ -E - </dev/null | grep unistd
# 1 "/usr/include/unistd.h" 1 3 4
```

探针成功了，但读的是**宿主 glibc 的 `/usr/include/unistd.h`**——而真实编译行带
`-nostdlibinc`，读的是图里 musl 的头。探针与被测对象从此看的不是同一份头文件。

去掉宿主头（模拟没有 `/usr/include` 的宿主）：

```
$ clang -nostdlibinc --target=x86_64-pc-cygwin -include unistd.h -x c++ -E -dM - </dev/null
1 error generated.  rc=1
```

`verify()` 对 `rc != 0` 返回 refusal，`prepare.cppm` 把它变成构建失败：
「the [c-abi] verification probe could not be compiled」。也就是说在找不到 `unistd.h` 的
宿主上，这个方案**把整条 Windows × posix 路径变成硬拒绝**。

另有一处不对称：探针 argv 逐个 push `cEnvTokens`（`prepare.cppm:10923`，无去重），
真实编译行经 `appendUniqueFlags`（有去重）。加入 `-include` 之后，**两者不再是同一条
命令行**，而探针的全部价值建立在它们相同之上。

设计稿 §3.2 的 M2 与 §3.3 的 E3 写「probe 不验证 include 行为，这是有意为之」——
但问题不是探针要不要验证 `-include`，而是 `-include` 会不会让探针本身跑不起来。这一点
未被讨论。

### 4.5 指纹与输出目录

`prepare.cppm:12641` 把每个 `cEnvTokens` 令牌折进 `fpi.compileFlags`。新增两对令牌会
移动 Windows × posix 下**每一个**工程的输出目录，全量重建一次。这不是阻断性问题，
但 record §5 刚记录过一次同形事故（「c-abi 指纹无条件扫描平台环境的包 ⇒ 每个使用
openkal 的工程指纹都变」），设计稿的影响面表格没有提到这一行。

### 4.6 设计稿自带的单测会绿，而且什么都没测到

§3.2 提议的断言是

```cpp
auto r = cenv::realise(d, "windows", "x86_64", false);
ASSERT_TRUE(has(r->tokens, "-include"));
ASSERT_TRUE(has(r->tokens, "unistd.h"));
ASSERT_TRUE(has(r->tokens, "sys/stat.h"));
```

它断言的是 `realise` 返回的 vector 的成员资格。§4.2 的去重、§4.1 的模块单元、§4.3 的
汇编单元、§4.4 的探针，四处失败全部发生在这个 vector **之后**。该测试在四处缺陷全部
存在时依然全绿。这正是「判据通过了但什么都没测到」的形状：谓词对、对象错。

能区分的判据只有一条：**一个带 `.cppm`、`.S` 与 `import std` 的 e2e 工程，在
Windows × `presents = "posix"` 上真的构建一次。** 设计稿 §3.3 的 E1 把这件事交给
30-member 测量，而 30-member 里没有一个成员用 C++20 模块（`compat.*` 全是传统 C/C++ 库），
所以即使跑了也测不到 §4.1。

---

## 5. 规范符合性

### 5.1 `presents` 的定义

`docs/22-target-side.md:323` 的四键表：

| Key | Values | Answers |
| --- | --- | --- |
| `presents` | `posix` / `windows` / `none` | **which environment-identity macros source sees** (`__unix__` vs `_WIN32` vs neither) |

openkal 设计 §3.2 的取值表同样：

| 值 | 含义 |
| --- | --- |
| `posix` | 定义 `__unix__`，不定义 `_WIN32` 与 `__MINGW32__` |

两处都把 `presents` 定义为**身份宏**，且两处都紧接着写「三个键分属两类事实，互不推导」。

设计稿 §0.2 与 §2.1 把它重述为：

> 「`presents = "posix"` 是 mcpp 引擎对编译命令行的契约承诺，POSIX 标准规定哪些符号在
> 哪些头里，引擎要把这些头显式拉进每个 TU。」

这是把一个已发布字段的语义**扩宽**到原文没有的范围。`presents` 回答「源码走哪条分支」，
不回答「哪些符号已在作用域」。设计稿 §2.4 自己也写下了正确的那句——「`presents = "posix"`
是『POSIX 接口按需可达』，不是『所有 POSIX 接口已被强制 include』」——但 §3.1 的实现
正是后者的一个子集。同一份文档里两个互相否定的语义定义。

一个字段的语义扩宽，代价不在实现，而在它给下一个读者的授权：此后任何「某个 POSIX 符号
在某个包里不可见」都成了引擎的义务，而 §2.4 想守住的那条线没有任何机制在执行它。

### 5.2 openkal 设计已经为这类失败指定了答案

§5「平台相关代码」是 §2.1 那 13 条 `windows.h` 类失败的 designated 归属：

- §5.3 包内 feature：`tinyhttps` 就是设计稿自己举的例（`posix-socket` / `winsock`），
  而 `tinyhttps 0.3.1` 在 700f7de 里正是「selects POSIX sockets on Windows with musl」；
- §5.3 独立 shim 包：多个包需要同一段平台代码时；
- §6 / §7：`platform` 标签与 `platform-dependencies = "refuse"`，让「这个包需要平台接口」
  成为**被测量、被展示**的事实，而不是一次失败。

§9「备选与不做」里与本方案最近的一条：

| 备选 | 结论 |
| --- | --- |
| 在 musl 上仿真 `windows.h` | **不做**。永远做不完，且是 SPEC §3.1 拒绝的「模拟」 |
| 维持现状，逐包按 `c-abi` 适配 | **保留为退路** |

引擎侧无条件预置 POSIX 头，与「在 musl 上仿真 windows.h」的性质同源：都是由中间层替
上游代码决定它该看见什么，都没有终点判据（设计稿 §3.3 的 E2「每发现一个新的未声明
标识符就追加一个头」把这一点写成了流程）。

### 5.3 「通用 POSIX 知识，非包名」这条辩护为何不足

设计稿 §2.2 用 `cenv.cppm:10-14` 的「GENERIC KNOWLEDGE, NO PACKAGE NAMES」为 `-include`
辩护：POSIX 标准头是契约不是包名。

这条规则约束的是**不得按 C 库的身份分支**，它不授权「凡 POSIX 规定的都可以由引擎替
每个 TU 决定」。同一段注释紧接着写的是「a second POSIX C library on Windows would
realise through the same table」——它要求的是**可替换性**，而 §4.4 表明加入 `-include`
之后，realisation 对「这个 C 库是否在这个搜索路径里提供了这个头」产生了依赖，恰好削弱
了可替换性。

### 5.4 包私有作用域是一条被明写依赖的性质

`compat.zlib.lua` 现在的注释里有一段值得完整引用：

> This `cflags` is package-private: it reaches zlib's own translation units
> (gzlib.c, zutil.c, ...) but not a consumer compiling zlib.h ... So the library
> computes z_off_t with Z_HAVE_UNISTD_H set (= off_t) while an openkal-Windows
> consumer ... computes it with Z_HAVE_UNISTD_H unset (= long long) ...

即索引维护者**明确依赖**「包侧 `cflags` 不外溢到消费者」这条性质，并围绕它写了运行期
断言。引擎广播没有这条性质（它进每个包的 `privateBuild`）。把强制包含从包侧提升到
引擎侧，等于把一个被明写依赖的作用域边界抹掉；即便对 `unistd.h` 这一个头恰好无害，
§3.3 的 E2 迭代规则会持续往这条通道里加头。

---

## 6. 稳定性与兼容性影响面

| 维度 | 设计稿的判断 | 核查结果 |
| --- | --- | --- |
| 影响面收在 Windows × x86_64 × posix 一格 | 是（§4.1 表） | **成立，但该格内部未分层**：`.c` / `.cpp` / `.cppm` / `.S` / 探针 五类对象行为不同，其中三类会失败 |
| 链接行不变、运行时零开销 | 是 | 成立 |
| 其他组合命令行一字不改 | 是 | 成立 |
| 不改任何已写下的设计决策 | 是（§0.5） | **不成立**：扩宽了 `presents` 的语义（§5.1），违反了 `prepare.cppm:11148` 写下的 GAS 拆分规则（§4.3） |
| 改动量「一行类 + 注释」 | 是（§3.1） | **低估**：要正确落地必须同时改 `Realisation` 的字段划分、asm 广播、探针 argv 构造、去重逻辑四处 |
| 上游修好后「无害但白 include」 | 是（§2.3 表） | 对 `.c` 成立；对 `.cppm` / `.S` 不是白 include 而是硬失败 |

**可回退性**：作为一次发布，它会移动全部 Windows × posix 工程的输出目录（§4.5）；撤销
时再移动一次。两次移动都发生在用户侧。

---

## 7. 生态发展视角：真正的下一步

按依赖顺序，与本方案无关的三步先做：

1. **完成发布链。** `openkal-musl 0.15.0` 与 `openkal-llvm-runtime 0.11.0` merge + tag +
   GitCode 镜像 + xim-pkgindex 登记（openkal 自审 §11 已把这两条列为「用户拍板」的
   未校验项，record §4 的沙箱 B–E 四段 NOT-RUN 也是同一个根因）。
2. **抬 `tests/openkal/pins.toml` 的 `runtime`。** 在 `runtime = "0.10.0"` 下，
   Windows 腿测的不是 `presents = "posix"`。这一步之前的任何 Windows 数字都不能用来
   给引擎决策。
3. **重跑 30-member，并且真的做那次 A/B。** design §11 决定 4 与 record §4 的「（待填）」
   指的是同一件事：`__unix__` 定义与否的两种呈现各跑一次。这是 #674 讨论的四条路径
   （尤其 B/D）唯一的判据来源，今天它还不存在。

第 3 步跑完之后，残余失败按 openkal 设计 §5 逐包归位：

- 需要 `windows.h` 一族的（c-ares、catch2、cli11、doctest、libpng、mimalloc、re2、
  sqlite3、spdlog、archive/xz、curl）→ §5.3 的包内 feature 或图提供的平台 SDK 依赖，
  并在索引里落 `platform` 标签。这**不是失败**，是 §7 标签体系要表达的事实；
- clang 自带头的两个（eigen 的 `mm_malloc.h`、fmt 的 `intrin.h`）→ 已由
  `builtins = "iso"` 与 `-nostdlibinc`（#662/#664）覆盖，重测确认即可；
- 真正「缺 POSIX 头」的（如果重测后还剩）→ 包侧 `target_cfg.cflags` 的 `-include`，
  即 zlib 已经用过并且已经因为不再需要而撤掉的那个机制。

**给 #674 的回复建议**：Path A 是正确的归属，但成本估计应更正（机制是 `target_cfg`
不是 `patches`，而且多数包属于 §5 的 `platform` 标签而非需要 patch）；Path B/D 需要
先补那次从未做过的 A/B；Path C 因 §2 与 §4 不建议。issue 正文里的三处事实（0/30 基线、
hostStripMacros 在编译行上、P3 spike 已测两种呈现）建议一并更正，否则它们会作为前提
被下一份设计继承——这次已经发生了一遍。

---

## 8. 如果仍要在引擎侧做强制包含，唯一可能安全的形状

这一节不是推荐，是在用户拍板要做的情况下把约束列全。

1. **不走 `Realisation::tokens`。** 新增独立字段（如 `preIncludeTokens`），因为它必须
   从三个消费点里被排除：探针 argv（`prepare.cppm:10923`）、asm 广播
   （`prepare.cppm:11166`）、以及任何模块接口单元的编译行。
2. **模块接口单元必须排除。** 这需要在 `build_program` / `flags` 层按单元类型分流，
   而不是在包级 `cxxflags` 上。今天没有这个分流点，新增它是本方案的主要工作量。
   `std` 模块（`prepare.cppm:12380`）要单独排除。
3. **单令牌拼法。** `-includeunistd.h` 或 `--include=unistd.h`，规避
   `appendUniqueFlags` 的按串去重；同时在 `appendUniqueFlags` 上加一条「带参令牌不参与
   去重」的规则，否则下一个同形令牌会再犯一次。
4. **绝对路径而非裸名。** 裸名依赖搜索路径，在 `-nostdlibinc` 之下由图提供的 C 库满足，
   在探针里由宿主满足——两者不同（§4.4）。若保留裸名，必须同时保证探针不带这些令牌。
5. **判据必须是一个 e2e 工程**，含 `.cppm`、`.S`、`import std`，在
   Windows × `presents = "posix"` 上构建并运行；单测断言 vector 成员资格不能作为判据
   （§4.6）。
6. **终点规则要可执行。** §3.3 的 E2「按测量迭代追加」没有终止条件，也没有拒绝条件。
   若无法写出「什么时候不再追加」的判据，这条规则保护不了 §2.4 想守的那条线。

---

## 9. 复现

全部命令在本机（Linux x86_64，`~/.xlings/subos/current/bin/clang`，clang 22.1.0）执行。

```sh
# M1/M2 —— 两个三元组的预定义
clang --target=x86_64-pc-cygwin        -x c -E -dM - </dev/null \
  | grep -E '^#define (_WIN32|_WIN64|__CYGWIN__|__unix__|__SIZEOF_LONG__|__SIZEOF_WCHAR_T__) '
clang --target=x86_64-w64-windows-gnu  -x c -E -dM - </dev/null \
  | grep -E '^#define (_WIN32|_WIN64|__MINGW32__|__SIZEOF_LONG__) '
# cygwin : __CYGWIN__ __unix__ unix __SIZEOF_LONG__ 8 __SIZEOF_WCHAR_T__ 2   （无 _WIN32）
# mingw  : _WIN32 _WIN64 __MINGW32__ __SIZEOF_LONG__ 4

# M3 —— glibc 的 stdio.h 不传递包含 unistd.h
printf '#include <stdio.h>\n' > s.c
clang -E s.c | grep -c 'unistd\.h'                      # 0
printf '#include <stdio.h>\nlong f(void){return lseek(0,0,0);}\n' > s2.c
clang -fsyntax-only -std=c11 s2.c                       # error: call to undeclared function 'lseek'

# M4 —— 模块接口单元
printf 'export module demo;\nexport int f(){return 42;}\n' > m.cppm
clang++ -std=c++23 -include unistd.h --precompile m.cppm -o m.pcm
# error: module declaration must occur at the start of the translation unit

# M5 —— libc++ std.cppm 的形状（注释 + module; + #include + export module）
printf '// gen\n\nmodule;\n\n#include <cstddef>\n\nexport module stdlike;\n' > std_shape.cppm
clang++ -std=c++23                  --precompile std_shape.cppm -o /dev/null   # OK
clang++ -std=c++23 -include unistd.h --precompile std_shape.cppm -o /dev/null
# error: 'module;' introducing a global module fragment can appear only at the start ...

# M6 —— GAS
printf '.text\n.globl foo\nfoo:\n  ret\n' > a.S
clang -c a.S -o a.o                                     # OK
clang -c -include unistd.h a.S -o a2.o
# /usr/include/unistd.h:27:1: error: invalid instruction mnemonic '__begin_decls'

# M7 —— appendUniqueFlags 去重之后的命令行
printf 'int main(){return 0;}\n' > t.c
clang -c -include unistd.h sys/stat.h t.c -o t.o
# clang: error: no such file or directory: 'sys/stat.h'

# M8 —— 探针形状
clang --target=x86_64-pc-cygwin -include unistd.h -x c++ -E - </dev/null | grep -m1 'unistd.h"'
# 读到 /usr/include/unistd.h —— 宿主 glibc 头
clang -nostdlibinc --target=x86_64-pc-cygwin -include unistd.h -x c++ -E -dM - </dev/null
# 1 error generated.  rc=1

# M9 —— 单令牌拼法可用
clang -c -includeunistd.h  t.c -o t.o                   # 接受
clang -c --include=unistd.h t.c -o t.o                  # 接受
```

数据来源：

```sh
cd ~/workspace/github/mcpplibs/mcpp-index
python3 - <<'PY'
import json
d = json.load(open('.xpkgindex/openkal-compat.json'))
print(d['pins'], d['measured'])
for n, i in sorted(d['members'].items()):
    t = i['targets'].get('x86_64-windows-gnu', {})
    if t.get('status') != 'runs':
        print(n, t.get('status'), t.get('diagnostic', '')[:120])
PY
sed -n '1,35p' tests/openkal/pins.toml
git show 41ad30a:pkgs/c/compat.zlib.lua | grep -n 'target_cfg' -A 4
```

---

## 10. 参考

- `src/toolchain/cenv.cppm:188-300` —— 映射表与 Windows + Posix 分支
- `src/toolchain/cenv_probe.cppm:144-250` —— 探针命令的构造
- `src/build/prepare.cppm:6380-6391` —— `appendUniqueFlags`（按串去重）
- `src/build/prepare.cppm:10790-10955` —— realise、宿主剥离、探针调用
- `src/build/prepare.cppm:11140-11170` —— 三通道广播与 GAS 规则
- `src/build/prepare.cppm:12372-12382` —— `std` 模块的编译行
- `src/build/prepare.cppm:12630-12645` —— 指纹
- `docs/22-target-side.md:295-430` —— `[c-abi]` 四键与映射表
- `mcpplibs/openkal/.agents/docs/2026-09-18-openkal-c-environment-and-personalities-design.md` §3.2 / §5 / §9 / §11
- `mcpplibs/openkal/.agents/docs/2026-09-18-c-environment-execution-plan.md` §4（标题为「风险与退出」）
- `mcpplibs/openkal/.agents/docs/2026-09-18-c-environment-record.md` §4（「兼容性测量」为「待填」）
- `mcpplibs/mcpp-index/.xpkgindex/openkal-compat.json` —— 2026-09-17 基线
- `mcpplibs/mcpp-index/tests/openkal/pins.toml` —— `runtime = "0.10.0"`
- `mcpplibs/mcpp-index` 700f7de —— 撤回 `-U_WIN32 -include unistd.h`
