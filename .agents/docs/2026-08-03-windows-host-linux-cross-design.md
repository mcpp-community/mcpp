# Windows → Linux 交叉工具链(路径 A:canadian-cross payload)— 设计方案

> 2026-08-03 · 基于 f694e13(2026.8.3.1)代码审计
> 姊妹文档:`2026-07-15-mingw-linux-cross-windows-design.md`(**本方案的镜像**,Linux→Windows,已发布)、
> `2026-06-23-aarch64-musl-gcc-canadian-cross-rebuild.md`(canadian-cross 打包先例)、
> `2026-07-15-toolchain-target-naming-unification-design.md`(工具链×目标二轴命名)、
> `2026-07-07-hermetic-toolchain-link-model-design.md`(链接模型)
> 记忆:[[release-publish-pipeline]]、[[toolchain-target-naming-unification]]、[[mingw-linux-cross-windows]]、[[aarch64-musl-static-ninja-closure]]

## 0. 一句话

补上 `host ≠ target` 的**最后一个象限**:Windows 主机产 Linux ELF。后端零新增(GNU 方言 + ELF 链接模型早已按
target 求值),工作量集中在四处:**A. 解除三处 host 门**、**B. 修两个 host 常量污染 target 决策的真 bug**、
**C. canadian-cross 打包**(build=linux-gnu / host=x86_64-w64-mingw32 / target=x86_64-linux-musl)、
**D. CI 验证**(Windows runner 产 ELF 自己跑不了,需跨 job 回传 —— wine 那一维的镜像难题)。

选 musl 而非 glibc 作为首个 Linux target,理由与现有交叉行一致:**自包含**。musl 交叉链自带 libc + libstdc++ +
binutils,不需要 `xim:glibc` / `xim:linux-headers` 这两个 sysroot payload —— 而它们在 Windows 主机上根本没有发布。
`lifecycle.cppm:561` 的依赖门**已经**写好了这个豁免,零改即正确(见 §1.1)。

---

## 1. 现状评估(审计结论,file:line 落地)

> 方法论警告:`mingw-linux-cross-windows-design.md` 里点名的 `mingw_wrong_host()` 在 HEAD 上**已不存在**
> (那期做完即删)。本节所有坐标以 f694e13 为准;实施前请重新 grep 一遍,别信旧文档的符号名。
> —— 参见 [[v0099-feature-forwarding-238-230]] 同款教训。

### 1.1 已就位的抽象缝(交叉直接复用,**零改**)

| 抽象 | 位置 | 对本方案的意义 |
|---|---|---|
| 链接模型按 **target** 求值 | `linkmodel.cppm:281-286` | PE target 早退,ELF 路径按 target 解析;注释明说「so a future cross-compile resolves by what it builds FOR」 |
| **交叉工具链的 `bits/std.cc` 定位已实现** | `gcc.cppm:75-90` | 显式支持 `<prefix>/<triple>/include/c++/<ver>/bits/std.cc` —— **正是 musl 交叉链布局**,`import std` 开箱可用 |
| sysroot payload 依赖门按 target 豁免 | `lifecycle.cppm:561-562` | `!target.is_musl() && !target.is_pe() && !is_windows && !is_macos` → musl target **且** Windows host,双重豁免,不会去找不存在的 `xim:glibc` |
| `<triple>-g++` 交叉前端解析 | `registry.cppm:227-228` | 交叉前端的现成机制(但 Windows 上缺 `.exe`,见 §1.3) |
| `kGnuDialect` 命令拼写 | `dialect.cppm` | 交叉链是 GCC 家族,拼写零差异 |
| `BuildOverrides.target_triple` 入口 | `prepare.cppm:403-411` | `--target x86_64-linux-musl` 入口已在 |
| 交叉下跳过 L3 `build.mcpp` | `prepare.cppm:914-919` | host 构建程序不为 target 跑,语义已对 |
| 首次运行默认值不受影响 | `prepare.cppm:1416-1421` | Windows host 走 `kFirstRunWinMsvc` / windows-gnu,本方案纯 opt-in |
| 目标词汇表已含该 triple | `triple.cppm:104` | `x86_64-linux-musl` 已是 `verified` 行,**无需新增 target 身份** |

**结论**:mcpp 引擎侧改动极小。真正的工作在打包与验证 —— 与镜像方案(Linux→Windows)的结论一致。

### 1.2 需要解除的 host 门(Part A 的对象)

三处,全是 `is_linux` 硬编码:

1. **可安装性门** — `lifecycle.cppm:371-374`
   ```cpp
   if (t.os == "linux")
       return mcpp::platform::is_linux && (t.is_musl() || t.arch == hostT.arch);
   ```
   Linux 目标一律要求 Linux 主机 → `toolchain list` 的 Targets 块在 Windows 上**不列** linux 行。

2. **payload 选择** — `registry.cppm:225-227`
   ```cpp
   bool native = mcpp::platform::is_linux && t.arch == mcpp::platform::host_arch;
   pkg.ximName = native ? "musl-gcc" : t.str() + "-gcc";
   ```
   Windows host 上 `native` 恒 false → 已经会去要 `x86_64-linux-musl-gcc` 这个包名。**逻辑碰巧是对的**,缺的是包
   (§3)与 `.exe` 前端(§1.3)。

3. **Available 索引 host-split** — `registry.cppm:308-313`
   仅在 Windows host 列 `mingw-gcc`、Linux host 列 `mingw-cross-gcc`。需要在 Windows 分支补列新包。

> **架构债 —— 本期收敛(已拍板)**:门 1(可安装性)与门 2(payload 选择)是**同一决策的两处推导** ——
> 「这个 host 能不能服务这个 target」。历史上这个模式每次都在加新语义时变成构建失败
> (见 [[object-path-disambiguation-239-240]])。
>
> 本期把判据提成 `registry.cppm` 里的单一谓词:
> ```cpp
> // registry.cppm — 唯一真源:这个 host 能否服务这个 target
> bool host_can_serve(const triple::Triple& target);
> ```
> `lifecycle.cppm` 的 `installable_here` 改为直接调用它,自身不再做任何 `is_linux` 判断。
> **这是本方案唯一的重构项**,收益是下一个 target(aarch64-linux-musl、riscv64)只改一处。

### 1.3 两个真 bug(host 常量污染 target 决策,Part B)

这两条都不是「缺功能」,是**现存缺陷**,只是今天没有 Windows→Linux 这条路径把它们暴露出来。

**B1 — 交叉前端在 Windows 上永远找不到**(`registry.cppm:228`)

```cpp
pkg.frontendCandidates = { t.str() + "-g++", "g++" };
```

`toolchain_frontend()`(`registry.cppm:264-271`)用 `std::filesystem::exists(binDir / cand)` 逐个试。Windows 上
真实文件是 `x86_64-linux-musl-g++.exe`,`exists("...-g++")` 返回 **false** → 前端解析失败 → 安装成功但工具链不可用。

mingw 分支早就踩过并修了(`registry.cppm:242`:`{"g++.exe", "g++"}`),musl 分支没跟上。修法同款:候选表按 host
补 `.exe` 变体,**`.exe` 在前**。

**B2 — `-static` 在 Windows host 上不会发**(`platform/common.cppm:110` → `flags.cppm:539`)

```cpp
// platform/common.cppm:110
constexpr bool supports_full_static = is_linux;  // macOS/Windows cannot

// flags.cppm:539
std::string full_static = (mcpp::platform::supports_full_static && f.linkage == "static") ? " -static" : "";
```

这是个 **host 常量**,注释说的是「macOS/Windows 主机的产物不能全静态」—— 语义正确,但被 `flags.cppm` 拿来决定
**target** 的链接方式。`x86_64-linux-musl` 的 `defaultStatic = true`(`triple.cppm:104`),在 Windows host 上
`supports_full_static` 为 false → **`-static` 不发** → musl 目标的核心卖点(全静态 ELF)直接丢失。

PE target 不受影响,因为它的 `-static` 走 contract table 的 `unit_ldflags(dist::Format::Pe)` 另一条路
(`flags.cppm:707-708`)—— 也就是说这条常量今天**只服务 ELF**,把它按 host 求值从来就是错的。

修法:改为按 target 求值。语义应是「**目标平台**能否全静态链接」,即 `target.os == "linux"`(macOS 目标因
libSystem 仍为 false)。注意这会同时影响 `build_program.cppm:474` 的 `muslStaticHelper` —— 那里是 **host** 语义
(build.mcpp helper 跑在主机上),**不能一起改**,必须两处分开。参见 [[build-mcpp-helper-self-containment]]:
`-static` 绝不进 base,helper 的静态性是逐平台机制。

> **交付形态(已拍板):B1/B2 与主体合并为单 PR。**
>
> 代价是 [[soname-alias-explicit-ninja-goals]] 记着的那条:混在大改动里的跨平台修复,失败时无法归因。
> 单 PR 下用**提交分层**保住归因能力,这是硬要求不是建议:
>
> | commit | 内容 |
> |---|---|
> | 1 | **先加 B2 的回归断言**(§6.2 的 `grep -q "statically linked"`)—— 此时 CI 必红,红即证明断言有效 |
> | 2 | 修 B2(`supports_full_static` 按 target 求值)—— CI 转绿 |
> | 3 | 修 B1(前端 `.exe` 候选) |
> | 4 | `host_can_serve` 谓词收敛 + 三处门 |
> | 5 | doctor + 文档 + CI job |
>
> commit 1 单独存在是关键 —— 它把「测试真的会失败」变成可查的历史记录,而不是口头保证。
> 参见 [[explicit-ninja-goals-two-regressions]]:没先验证会红的测试,等于没有测试。

---

## 2. 身份与命名

遵循 `toolchain-target-naming-unification` 的二轴模型 —— **不新增任何用户可见身份**:

- **toolchain 轴**:`gcc@16.1.0`(与现有 pin 一致)
- **target 轴**:`x86_64-linux-musl`(`triple.cppm:104` 已是 verified 行)
- 用户命令与 Linux 主机上**逐字相同**:
  ```
  mcpp build --target x86_64-linux-musl
  mcpp toolchain default gcc@16 --target x86_64-linux-musl
  ```

「Windows-hosted」不是名字,是 `host ≠ target` 关系的一个取值 —— 与 mingw 那期的结论同构。

**xim 包名**:`x86_64-linux-musl-gcc`,由 `registry.cppm:227` 的 `t.str() + "-gcc"` 自动导出,**无需改代码**。

**包形态:一包双 host asset(已确证 xlings 支持,定稿)**

原本的未决项是「xlings 的 `xpm` 是否允许同一个包同时挂 linux 与 windows 段」。**已在本机索引上实测确证**:

```bash
$ cd ~/.xlings/data/xim-pkgindex/pkgs && \
  for f in $(grep -rl "windows = {" .); do grep -q "linux = {" "$f" && echo "BOTH: $f"; done
BOTH: n/ninja.lua      BOTH: c/cmake.lua     BOTH: x/xmake.lua
BOTH: g/git.lua        BOTH: r/rust.lua      BOTH: x/xvm.lua        ... (20+ 个)
```

`ninja` / `cmake` / `git` / `rust` 全是一包双 host。**取一包**:

| 方案 | 结论 |
|---|---|
| **一包双 host asset** ✅ | 单个 `x86_64-linux-musl-gcc`,`xpm` 下 `linux` 与 `windows` 两段,XLINGS_RES 按 host 选 asset。包名与 `registry.cppm:227` 的 `t.str()+"-gcc"` 天然一致 → **零代码改动** |
| ~~两包 host-split~~ | 已排除。需在 `registry.cppm` 加 host-split 分支,多一处推导 —— 与 §1.2 的收敛方向相反 |

`install()` / `config()` 的 Lua 需按 host 分叉枚举 bin(`ls -1` vs `dir /b`),这在描述符内部解决,不外溢到 mcpp。

Asset 命名遵循 xlings-res 约定:

```
x86_64-linux-musl-gcc-16.1.0-linux-x86_64.tar.gz     (已有,如未发布则本期一并发)
x86_64-linux-musl-gcc-16.1.0-windows-x86_64.zip      (本期新增)
└── x86_64-linux-musl-gcc-16.1.0-windows-x86_64/
    ├── bin/       (PE: x86_64-linux-musl-g++.exe, -gcc.exe, -ar.exe, ...)   ← host 侧
    ├── include/   (musl headers;include/c++/16.1.0/bits/std.cc)            ← target 侧
    ├── lib/       (musl libc.a、libstdc++.a、lib/gcc/x86_64-linux-musl/…)   ← target 侧
    └── libexec/gcc/x86_64-linux-musl/16.1.0/  (PE: cc1.exe, cc1plus.exe)   ← host 侧
```

> 注意这是 **NATIVE 布局**(target 库在 prefix 根),不是交叉工具链惯常的 `<prefix>/<triple>/` 布局 ——
> musl-cross-make 的 canadian 分支设 `SYSROOT = /` 所致(§3.3 待解点 2)。对 mcpp 无影响甚至更好:
> `bits/std.cc` 落在 `gcc.cppm:71` 的第一候选路径上。

---

## 3. 工具链构建(canadian-cross)

### 3.1 三元组

```
build  = x86_64-linux-gnu        (CI runner / 本机)
host   = x86_64-w64-mingw32      (产出的编译器跑在 Windows)
target = x86_64-linux-musl       (产出的编译器为 Linux 生成代码)
```

三者互不相同 = 教科书定义的 canadian cross。

### 3.2 自举闭环:host 编译器 mcpp 自己已经有了

canadian cross 需要一个 `--host` 编译器(能产 Windows PE 的 GCC)。**mcpp 已发布的 `mingw-cross-gcc@16.1.0`
正是它** —— Linux 主机上的 `x86_64-w64-mingw32-g++`,GCC 16.1.0,自带 mingw-w64 CRT。

这是个漂亮的自举:上一期的产物直接成为这一期的构建输入,不引入任何新的外部依赖。

### 3.3 构建路线:musl-cross-make + `HOST=`(定稿)

**取 musl-cross-make 的 canadian 路径。机制已在源码上确证,不再是「无先例的赌」。**

上一版曾把这条标为「风险最高」,依据是上游 [issue #55](https://github.com/richfelker/musl-cross-make/issues/55)
提过 mingw host 却无解关闭。**读了本机 payload 里的实际源码后,这个判断被推翻**:

**证据 1 — `HOST` 是一等公民,`NATIVE` 只是它的语法糖**(`litecross/Makefile:87-89`):

```make
ifneq ($(NATIVE),)
HOST:=$(TARGET)          # NATIVE=1 ⟺ HOST=TARGET,仅此而已
endif
```

**证据 2 — canadian 是内建代码路径,直接把 `--build`/`--host` 喂给 configure**(`litecross/Makefile:97-125`):

```make
ifeq ($(HOST),)          # 普通交叉
    SYSROOT = /$(TARGET)
    FULL_BINUTILS_CONFIG += --build=$(BUILD) --host=$(BUILD)
    ...
else                     # ← canadian / native 共用这一支,HOST 可以是任意 triple
    SYSROOT = /
    FULL_BINUTILS_CONFIG += --build=$(BUILD) --host=$(HOST)
    FULL_GCC_CONFIG      += --build=$(BUILD) --host=$(HOST)
    MUSL_VARS =
endif
```

**证据 3 — 用户 `HOST=` 不会被默认值吃掉**(`Makefile:32-36`):`HOST = $(if $(NATIVE),$(TARGET))` 之后第 36 行
才 `-include config.mak`,GNU make 后赋值胜出;`BUILD_DIR`/`OUTPUT` 是递归展开变量,使用时才求值。

**这三条合起来说明:`HOST=x86_64-w64-mingw32` 走的是与 aarch64 那期 `NATIVE=1` 完全相同的代码路径**
(`aarch64-musl-gcc-canadian-cross-rebuild.md` 已实战跑通)。issue #55 的失败因此可以准确归因为
**配置问题而非机制缺失**(2019 年、ppc64 target、提问后无跟进),不构成对本方案的否证。

**证据 4 — canadian 模式下 target 库本就不重建**:上一版路线 3 的「关键洞察」(target 侧产物可复用)
其实是 musl-cross-make canadian 模式的**既有行为** —— `SYSROOT = /` 且 `MUSL_VARS = ` 清空,它不编 musl。
换言之路线 1 已经把路线 3 的优点包含在内,路线 3 没有独立存在价值。

#### 为什么它长期最合适

| 维度 | musl-cross-make(选中) | crosstool-NG | 手工三段 |
|---|---|---|---|
| 与生态一致 | ✅ `xim:musl-cross-make` 已在索引,所有现有 musl payload 都出自它 | ❌ 引入第二套构建体系 | ❌ 无体系 |
| 与既有先例同路径 | ✅ 与 aarch64 `NATIVE=1` 同一分支 | ❌ | ❌ |
| 扩展到下一个 target | ✅ 改 `TARGET =` 一行(aarch64 / riscv64) | ⚠️ 每个组合重写配置 | ❌ 每次手工重来 |
| GCC 版本升级 | ✅ 改 `GCC_VER =` 一行 | ⚠️ 重新 menuconfig | ❌ 全程手工 |
| 可脚本化进 CI | ✅ config.mak + `make` | ⚠️ 可行但重 | ❌ 不可重复 |

手工三段(旧路线 3)**短期最可控但长期最差** —— 不可重复、每次升 GCC 都要手工拼装、无法自动化,
会变成一个只有作者能重建的黑盒。既然选长期最合适,就不能选它。

#### config.mak(实测可用)

```make
GCC_VER    = 16.1.0
BINUTILS_VER = 2.44
MUSL_VER   = 1.2.5
GMP_VER = 6.3.0
MPC_VER = 1.3.1
MPFR_VER = 4.2.2
LINUX_VER = headers-4.19.88-2

TARGET     = x86_64-linux-musl
HOST       = x86_64-w64-mingw32          # ← canadian:产出的编译器跑在 Windows

COMMON_CONFIG += CFLAGS="-g0 -Os" CXXFLAGS="-g0 -Os -fno-declone-ctor-dtor" LDFLAGS="-s"
#                                                    ^^^^^^^^^^^^^^^^^^^^^^ 硬约束,见下
```

#### ⚠️ `-Os` 在 mingw host 上必须配 `-fno-declone-ctor-dtor`(实测,踩过)

**这是本期最贵的一个坑 —— 症状离根因隔了三层,值得单独记。**

症状:GCC 编到最后链接 `gcov-dump.exe` 时炸,报 `std::string` 的**移动构造函数** undefined:

```
ld: libcommon.a(paths-output.o): undefined reference to
    `std::__cxx11::basic_string<...>::basic_string(std::__cxx11::basic_string<...>&&)'
```

第一反应「mingw-cross-gcc payload 的 libstdc++ 坏了」——**错的**。`nm` 显示符号在
`libstdc++.a` 里明明是 `T`(defined)。真相在 **mangled name**,demangle 后完全看不出来:

| 侧 | mangled | 含义 |
|---|---|---|
| 引用 | `…basic_string…C**4**EOS4_` | C4 = 统一构造函数(未 clone) |
| 定义 | `…C**1**EOS4_` / `…C**2**EOS4_` | C1/C2 = complete / base |

`-Os` **自动开启** `-fdeclone-ctor-dtor`,令调用方引用 C4;而 libstdc++.a 只显式实例化了
C1/C2。ELF 平台用符号别名把 C4 接到 C1/C2 上,**COFF 没有别名机制** —— 所以这个失配
只在 mingw 上暴露,且只打在 `#include <string>` 且移动构造 `std::string` 的 host TU 上。

四行最小复现(任何人都能在 30 秒内自证):

```
$ x86_64-w64-mingw32-g++      mv.cc   # LINK OK
$ x86_64-w64-mingw32-g++ -O2  mv.cc   # LINK OK
$ x86_64-w64-mingw32-g++ -Os  mv.cc   # LINK FAIL   ← -Os 是唯一变量
$ x86_64-w64-mingw32-g++ -Os -fno-declone-ctor-dtor mv.cc   # LINK OK
```

> **为什么 aarch64 那期没踩到**:`aarch64-musl-gcc-canadian-cross-rebuild.md` 用的同样是
> `CFLAGS="-g0 -Os"`,但那次 HOST 是 **aarch64-linux-musl(ELF)**,符号别名把问题吃掉了。
> 「同一份 config 换个 HOST 就炸」正是本条必须写进文档的原因。
>
> `CXXFLAGS` 只作用于 host 侧(target 库走 `CXXFLAGS_FOR_TARGET`),因此这个 flag
> **不影响产物 ABI**,可以放心加。

#### ⚠️ 必须 `--disable-libstdcxx-pch`(实测,踩过)

过了上一个坑之后,构建会在 **target libstdc++** 再炸一次:

```
libstdc++-v3/include/atomic:259:7: error: 'constexpr' constructor does not have
  empty body [-Wtemplate-body]
make: *** [x86_64-linux-musl/bits/stdc++.h.gch/O2ggnu++0x.gch] Error 1
```

libstdc++ 会为 `-std=gnu++0x`(C++11)额外生成一份预编译头。GCC 16 的 `<atomic>` 里
那个 `constexpr` 构造函数带非空 body(C++14 起才合法),上游用
`#pragma GCC diagnostic ignored "-Wc++14-extensions"` 抑制过 —— 但 **GCC 15 新增的
`-Wtemplate-body`** 让诊断在模板*定义*处就触发,pragma 的作用域没覆盖到,于是变成硬 error。

**定位方法值得记**:不要去猜,直接看已发布的 `xim:musl-gcc@16.1.0` payload——

```bash
$ find <payload> -name '*.gch' | wc -l
0
```

**零个 PCH**。已发布的 payload 本来就是 `--disable-libstdcxx-pch` 构建的。所以这不是
"canadian 特有的新问题",而是**两个 payload 的构建配置必须对齐**。加上它,与生态既有产物一致。

mcpp 走 modules,PCH 对它零价值,没有任何损失。

前置:`x86_64-w64-mingw32-g++` 必须在 PATH —— 即 §3.2 的自举闭环,来自 mcpp 已发布的 `mingw-cross-gcc@16.1.0`。

#### 待解点 —— 实施中已全部解答(2026-08-03 实测)

原先登记的两个不确定项,在实际跑构建时都有了确定答案:

**1. host 侧 gmp/mpfr/mpc/isl —— 不是问题,GCC in-tree 构建自动处理。**
`litecross/Makefile:160-190` 把这些库 symlink 进 GCC 源码树(`src_gcc/{gmp,mpc,mpfr,isl}`),
走 GCC 的 in-tree 依赖构建 —— 因此它们**天然继承 `--host=`**,不需要任何 `--with-gmp=` 干预。
这很可能就是 issue #55 提问者卡住的地方(他大概率试图分别预编译这些库)。

**2. `SYSROOT = /` 只影响 C 库,C++ 头仍走交叉布局 —— 别被 `-print-sysroot` 骗了。**

canadian 分支设 `SYSROOT = /`,于是 **musl libc / kernel headers** 装进 prefix 根
(`<prefix>/{include,lib}`),`-print-sysroot` 也确实回答 `<prefix>/`。**但 C++ 头不在那里**:
GCC 作为交叉编译器,`GPLUSPLUS_INCLUDE_DIR` 始终是 `<prefix>/<target-triple>/include/c++/<ver>`。

实测踩过:按 `-print-sysroot` 的字面意思把 libstdc++ 放进 `<prefix>/include/c++/`,编译报
`fatal error: iostream: No such file or directory`,而文件明明在。真相在 `-v` 的
`ignoring nonexistent directory` 列表里:

```
ignoring nonexistent directory ".../x86_64-linux-musl/include/c++/16.1.0"
                                  ^^^^^^^^^^^^^^^^^^ 它要的是这里
```

最终布局(**两种布局混用**,这正是反直觉之处):

```
output-x86_64-w64-mingw32/
├── bin/                       (PE: x86_64-linux-musl-g++.exe …)      ← host
├── libexec/gcc/…/cc1plus.exe  (PE)                                    ← host
├── include/                   (musl + kernel headers)                 ← target, sysroot=/
├── lib/                       (musl libc.a, crt*.o)                   ← target, sysroot=/
├── lib/gcc/x86_64-linux-musl/16.1.0/  (libgcc.a, crtbegin.o …)        ← target
└── x86_64-linux-musl/                                                 ← target, 交叉布局
    ├── include/c++/16.1.0/bits/std.cc
    └── lib/libstdc++.a
```

**对 mcpp 无影响**:`bits/std.cc` 落在 `<prefix>/<triple>/include/c++/<ver>/`,正是
`gcc.cppm:75-90` 已实现的交叉候选。§1.1 的原判断成立,`import std` 零改可用。

#### 实施前置(实测确认,必须两者同时在 PATH)

canadian 构建需要**两个**现成工具链,mcpp 生态里都已发布:

| 工具链 | 作用 | 来源 |
|---|---|---|
| `x86_64-w64-mingw32-*` | 编 **host 侧**(driver、cc1plus、binutils → PE) | `xim:mingw-cross-gcc@16.1.0` |
| `x86_64-linux-musl-*` | 编 **target 侧**(libgcc、libstdc++、musl) | `xim:musl-gcc@16.1.0` |

**target 编译器的版本必须与 `GCC_VER` 一致**(本期同为 16.1.0)。若用 15.1.0 去编 16.1.0 的
libstdc++,module 相关产物会与 driver 不匹配 —— 这是最隐蔽的一类失败,不会在构建期报错。

> **make 时间戳陷阱**(实测踩到):新增 `hashes/<tarball>.sha1` 必须**早于**已下载的 tarball。
> `Makefile:86` 的规则是 `$(SOURCES)/%: hashes/%.sha1`,hash 文件更新则视 tarball 过期 → 重新下载
> → 撞上 ftpmirror 502 直接失败。先下载后写 hash 的话,补一次 `touch sources/<tarball>` 即可。

**回退顺序**:若上述两点在合理时间内解不掉,回退 crosstool-NG canadian sample
(`ct-ng x86_64-w64-mingw32,x86_64-linux-musl`,需先建 host 工具链);手工三段仅作最后兜底,
且**一旦动用必须在文档里标注为技术债**,后续补回 musl-cross-make 路径。

### 3.4 spike 的成功判据

#### ⚠️ target libstdc++ 不要重建 —— 从同版本 payload 复用(实测,撞了四次)

`make install` 会一路撞在 **target libstdc++** 上,连撞四种不同的错:

| # | 症状 | 处理 |
|---|---|---|
| 1 | `stdc++.h.gch` — `constexpr` ctor 非空 body | `--disable-libstdcxx-pch` |
| 2 | `futex.lo` / `future.lo` — 同上,`-std=gnu++11` | `CXXFLAGS_FOR_TARGET += -Wno-template-body` |
| 3 | `string-inst.cc` — duplicate explicit instantiation | `-fpermissive` |
| 4 | `format.lo` — instantiating erroneous template | 还有下一个… |

打到第四个就该停下来问:**为什么要用已发布的 GCC 16.1.0 去重新编译 GCC 16.1.0 自己的 libstdc++?**

canadian 构建的职责是产出 **host 侧二进制**。target 库是纯 target 代码,**与谁编译它无关**;
而 `xim:musl-gcc@16.1.0` payload 里那一份是同版本(实测 `BASE-VER` 与 `DATESTAMP` 都是
16.1.0 / 20260430)、经完整 CI 验证的权威副本。canadian 模式设 `SYSROOT = /` 本就假设 target
树已存在 —— 复用它才是**顺着设计走**,不是绕过困难。

于是构建收敛为三步(实测可复现):

```bash
# 1) host 侧:driver / cc1plus / binutils → PE
cd <build>/x86_64-w64-mingw32/x86_64-linux-musl/obj_gcc
make DESTDIR=$OUT install-gcc            # bin/*.exe, libexec/**/cc1plus.exe
make DESTDIR=$OUT install-target-libgcc  # lib/gcc/<triple>/16.1.0/{libgcc.a,crt*.o}
# musl libc 已由前面的 `make install` 装进 $OUT/{include,lib}

# 2) target 侧 C++:从权威 payload 复用(注意是交叉布局,见上文待解点 2)
cp -a $PAYLOAD/x86_64-linux-musl/include $OUT/x86_64-linux-musl/
cp -a $PAYLOAD/x86_64-linux-musl/lib/.   $OUT/x86_64-linux-musl/lib/

# 3) DLL 部署,见下
```

#### ⚠️ `libwinpthread-1.dll` 要部署到**每个**含 .exe 的目录

PE 没有 RPATH,DLL 只从 exe 同目录 / PATH 解析。产出的 host 二进制唯一的非系统依赖就是它
(其余 `KERNEL32/ADVAPI32/msvcrt/WS2_32` 都是 Windows 自带)。漏了就是启动即
`STATUS_DLL_NOT_FOUND`,而且**不止 `bin/`**:

```
bin/                                    x86_64-linux-musl-g++.exe …
libexec/gcc/x86_64-linux-musl/16.1.0/   cc1.exe, cc1plus.exe
x86_64-linux-musl/bin/                  as.exe, ld.exe      ← 最容易漏的一个
```

实测就是漏了第三个:g++ 能跑、`--version` 正常,一编译就在汇编阶段炸 `as.exe`。

```bash
for d in $(find "$OUT" -name "*.exe" -printf "%h\n" | sort -u); do cp -n "$DLL" "$d/"; done
```

> **更优但更贵的替代**:给 host 侧加 `LDFLAGS=-static` 彻底消除 DLL 依赖(winlibs 就这么做)。
> 需要重新 configure + 重链接,本期未做;登记为 follow-up。

---

> **P0 已完成(2026-08-03),六条全绿。** 实测记录见下。验证在本机用 **wine** 驱动 PE 编译器
> 完成 —— 不需要 Windows 机器就能跑完判据 1–6,这条对后续维护很省事。
>
> ```
> 判据 1  bin/x86_64-linux-musl-g++.exe: PE32+ executable (console) x86-64, for MS Windows  ✅
> 判据 2  wine …g++.exe --version → x86_64-linux-musl-g++.exe (GCC) 16.1.0                  ✅
> 判据 3  x86_64-linux-musl/include/c++/16.1.0/bits/std.cc 存在                              ✅
> 判据 4  hello → ELF 64-bit LSB executable, x86-64, statically linked                       ✅
> 判据 5  本机(真 Linux)执行 → "windows builds linux";readelf 无 PT_INTERP               ✅
> 判据 6  import std 编译 + 链接 + 执行 → "import std works"                                ✅
> ```

必须**全部**满足才算 P0 出口 —— 逐条断言,不要只看「编译通过」:

1. `file bin/x86_64-linux-musl-g++.exe` → `PE32+ executable ... x86-64`
2. Windows(或 wine)上 `x86_64-linux-musl-g++.exe -v` 正常输出,`--version` 报 16.1.0
3. **`x86_64-linux-musl/include/c++/16.1.0/bits/std.cc` 存在** —— 没有它 `import std` 全盘不可用,
   这是 mcpp 的硬 floor,不是可选项
4. `hello.cpp` → ELF:`file a.out` 报 `ELF 64-bit LSB executable, x86-64, statically linked`
5. **该 ELF 在真 Linux 上能跑**(不是「链接成功」——见 [[e2e-inherit-toolchain-corrupts-real-payloads]]:
   产物存在 ≠ 产物正确)
6. `import std` 的 hello 能编能跑(判据 3 的运行期闭环)

---

## 4. 分发

### 4.1 xlings-res 镜像

与所有工具链 payload 同模式:GLOBAL → github,CN → gitcode 双端镜像。

**必须 strip**:[[gitcode-mirror-upload-shaping]] 记着上一次的教训 —— 未 strip 的载荷 34.81MB → strip 后 4.62MB,
而 `file.gitcode.com` **入境逐连接限速 ~12KB/s**,不 strip 上传必挂。canadian cross 产物同样要 strip
(`COMMON_CONFIG += CFLAGS="-g0 -Os"` 已在 §3.3 的 config 里)。

**symlink 处理**(本机实测,风险已量化):

```
xim-x-musl-gcc/15.1.0/   symlinks: 14   files: 2369
```

14 个,全是 `.so` 别名(`libstdc++.so` → `.so.6`、`ld-musl-x86_64.so.1`)和 `x86_64-linux-musl-cc`。
Windows 无 Developer Mode 的普通用户**无权创建 symlink**,而 mcpp 安装以普通用户身份运行。

修法:打包时 `tar --dereference`(或 zip 默认行为即存副本)。静态链接场景下 `.so` 别名本就用不到,
`-cc` 别名可留实体副本。**14 个文件的代价换掉一整类 Windows 权限故障,直接做,不要留 symlink**。

> 这条是 §1 之外唯一的 Windows 特有约束,且已从「可能是 blocker」实测降级为「打包参数一行」。

### 4.2 xim-pkgindex 描述符

新建 `pkgs/x/x86_64-linux-musl-gcc.lua`。骨架取自 `mingw-cross-gcc.lua`(cross 语义)+ `mingw-gcc.lua`
(Windows host 的 `dir /b` 枚举)+ `musl-gcc.lua`(一包多 asset):

```lua
package = {
    spec = "1",
    name = "x86_64-linux-musl-gcc",
    description = "GCC cross toolchain targeting x86_64-linux-musl (musl, static)",
    -- ...
    xpm = {
        -- Linux host: 非 x86_64 主机交叉编 x86_64(x86_64 主机走 native `musl-gcc`)
        linux   = { ["latest"] = { ref = "16.1.0" },
                    ["16.1.0"] = { url = "XLINGS_RES", sha256 = { x86_64 = "..." } } },
        -- Windows host: 本期新增的 canadian-cross 产物
        windows = { ["latest"] = { ref = "16.1.0" },
                    ["16.1.0"] = { url = "XLINGS_RES", sha256 = { x86_64 = "..." } } },
    },
}
```

`install()` / `config()` 需按 host 分叉枚举 bin(`ls -1` / `dir /b`),并在 Windows 分支跳过 `.dll`
(`mingw-gcc.lua:is_registerable_bin` 现成)。

**无 `deps`**:musl 交叉链自包含。特别注意 **不要**照抄 `musl-gcc.lua` 的 `deps = {"xim:patchelf@0.18.0"}` ——
那是给 Linux-hosted ELF 前端做 PT_INTERP 重定位的,PE 前端没有 interp,patchelf 在 Windows 上也不存在。

### 4.3 发布顺序

遵循 [[release-publish-pipeline]],且**索引先于代码**:

1. spike 出工具链 → strip → 打包(§4.1)
2. 上传 xlings-res 双端(github + gitcode),**真 GET + sha256 独立核验**
3. 提交 xim-pkgindex 描述符 → 等索引传播(CDN 滞后可达 ~40min,见 [[release-bootstrap-pin-two-groups]])
4. `xlings install x86_64-linux-musl-gcc@16.1.0` 在**真 Windows** 上验证可装可跑
5. 才开始合 mcpp 侧 PR

顺序反了会让 mcpp 的 CI 去装不存在的包,全线假红。

---

## 5. mcpp 侧改动清单

| # | 文件:行 | 改动 | 类型 |
|---|---|---|---|
| 1 | `platform/common.cppm:110` + `build/flags.cppm:539` | `supports_full_static` 由 host 常量改为 target 谓词;**`build_program.cppm:474` 保持 host 语义不动** | **bug 修复 B2** |
| 2 | `toolchain/registry.cppm:228` | musl 前端候选补 `.exe` 变体(`.exe` 在前) | **bug 修复 B1** |
| 3 | `toolchain/registry.cppm` | 新增 `host_can_serve(const Triple&)` 单一谓词 | 重构(收敛两处推导) |
| 4 | `toolchain/lifecycle.cppm:371-374` | `installable_here` 改为调用 #3 | 门 |
| 5 | `toolchain/registry.cppm:308-313` | Windows 分支补列 `x86_64-linux-musl-gcc` | 门 |
| 6 | `toolchain/registry.cppm:334-350` | `archive_tool` 的 musl 分支在 Windows 上试 `<triple>-ar.exe` | 适配 |
| 7 | `doctor.cppm:134-148` | Windows 分支补一行 linux-musl 交叉链探测(仿 mingw 那段) | 可观测性 |
| 8 | `docs/03-toolchains.md` | Targets 表补 Windows host 行;新增一节说明该组合 | 文档 |

**零改确认**(审计过,勿动):`linkmodel.cppm`、`gcc.cppm`(`bits/std.cc` 定位)、`dialect.cppm`、
`triple.cppm`(target 词汇表)、`lifecycle.cppm:561`(sysroot 依赖门)、`prepare.cppm`(首运行默认)。

---

## 6. CI 验证

### 6.1 核心难题:Windows runner 产出的 ELF,自己跑不了

这是 wine 那一维的**镜像问题**,但更麻烦:Linux 上有 wine 能跑 PE,Windows 上跑 ELF 没有对等物。

三个选项:

| 方案 | 说明 | 取舍 |
|---|---|---|
| **跨 job artifact 回传**(推荐) | Windows job 交叉构建 → `upload-artifact` → Linux job `download-artifact` 并真实执行 | 真实执行,无模拟层;代价是两个 job + 一次 artifact 往返 |
| Windows runner 内 WSL | `wsl bash -c ./a.out` | GitHub windows runner 的 WSL 可用性/成本不稳定 |
| 只做静态断言 | `file` 报 ELF + `readelf` 检查 | **不够** —— 见 [[e2e-inherit-toolchain-corrupts-real-payloads]],链接成功 ≠ 能跑 |

取**跨 job artifact 回传**。产物是全静态 musl ELF(无 `PT_INTERP`),任何 Linux 上都能直接执行,回传后
`ubuntu-24.04` 裸跑即可,不需要 qemu。

### 6.2 新 job 骨架

在 `cross-build-test.yml` 新增(结构照抄 `mingw-cross-wine` job,`:194-289`):

```yaml
  windows-host-linux-cross:
    name: windows→linux cross (build on windows, run on linux)
    runs-on: windows-2022
    steps:
      - Bootstrap mcpp via xlings（Windows 版,仿 ci-windows.yml）
      - mcpp toolchain install gcc 16.1.0 --target x86_64-linux-musl
      - mcpp build --target x86_64-linux-musl      # 被测:mcpp 自身
      - upload-artifact: target/**/mcpp

  windows-host-linux-cross-run:
    needs: windows-host-linux-cross
    runs-on: ubuntu-24.04
    steps:
      - download-artifact
      - file ./mcpp | grep -q "statically linked"   # 断言 B2 已修
      - chmod +x ./mcpp && ./mcpp --version         # 真实执行
```

`file ... | grep -q "statically linked"` 这一行就是 **B2 的回归测试** —— B2 未修时它必红。

> **测试自伤坑**:`! cmd | grep` 在 errexit 下被豁免,永不失败(见 [[build-mcpp-helper-self-containment]])。
> 断言一律写成正向 `grep -q`,不要写 `! ... | grep`。

### 6.3 矩阵文档同步

`cross-build-test.yml` 头部的注释矩阵是「mcpp 支持哪些交叉组合」的**单一真源**,必须同步加行:

```
#   x86_64-linux-musl     | gcc@16.1.0 (canadian)           | windows→linux | linux job
```

并把 "Planned cross rows" 里的 clang cross 那条**保留**(本方案不碰 clang 轴,见 §7)。

---

## 6.5 B3 —— 同一类问题的第三例(本期发现,**未修**,登记 follow-up)

CI 首次真正跑通交叉构建时暴露的:产物叫 `mcpp.exe`,而它是个 **ELF**。

`plan.cppm:200-213` 的 `target_output()`:

```cpp
return std::filesystem::path("bin") /
       std::format("{}{}", t.name, mcpp::platform::exe_suffix);   // ← host 常量
```

`exe_suffix` / `lib_prefix` / `static_lib_ext` / `shared_lib_ext` **四个都是 host 常量**
(`platform/common.cppm:18-29`),却用来命名 **target** 产物。与 B2 完全同构,而且**对称地错**:

| 方向 | 现状 | 应当 |
|---|---|---|
| Windows → linux-musl | `mcpp.exe`(却是 ELF) | `mcpp` |
| Linux → windows-gnu | `mcpp`(却是 PE) | `mcpp.exe` |

**为什么本期不修**:改产物命名是行为变更,会同时动到 `tests/e2e/102_mingw_cross_wine.sh`、
release 打包路径和任何用户脚本;而两个方向的现状都**不致命**(扩展名在 Linux 上无意义,
Windows 命令行也能跑无扩展名的 PE)。把一个有回归面的重命名塞进本 PR,违背了 §1.3 定下的
「单 PR 但提交分层、失败可归因」的初衷。

**修的时候要一起改的四个常量**,并且要注意 `runtime_aliases_for_target()`(`plan.cppm:215`)
依赖 `target_output()` 的结果去比对 soname —— 见 [[soname-alias-explicit-ninja-goals]],
那条边曾经因为类似改动漏生成过。

---

## 7. 边界:本方案不做什么

- **不碰 clang cross 轴**。`cross-build-test.yml:38-40` 记着 "mcpp does not yet inject `-target <triple>` +
  a cross sysroot"。让一个 clang 覆盖所有 target 长期更优(不必每个 triple 造一套编译器),但那是
  **另一根轴**:mcpp 今天**从不发 `-target` 给编译器**,`targetTriple` 只用于选 payload / 选目录 / 算缓存键。
  改它要 touch flags + linkmodel + stdmod + probe,比本方案大一个量级。**本期不动,登记为 follow。**
- **不做 Windows → linux-gnu**。glibc 目标依赖 `xim:glibc` / `xim:linux-headers` 两个 sysroot payload,
  它们没有 Windows 分发。musl 静态覆盖了「在 Windows 上产可分发 Linux 二进制」的主要诉求。
- **不做 Windows → aarch64-linux-musl**。同一机制的第二个 triple,等 x86_64 跑通后按 §5#3 的单一谓词加一行即可。
- **不改首次运行默认值**。Windows 主机的默认仍是 MSVC / windows-gnu,本方案纯 opt-in。
  参见 [[issue331-windows-msvc-triage]]:裸 Windows 的问题是**默认值选错**,不是缺工具链 —— 别再往默认路径加分支。

---

## 8. 分期

三期严格串行 —— **顺序反了会让 CI 去装不存在的包,全线假红**(§4.3)。

| 期 | 位置 | 内容 | 出口判据 |
|---|---|---|---|
| **P0** | mcpp 仓库外 | §3.3 spike:musl-cross-make `HOST=x86_64-w64-mingw32` | §3.4 六条判据全绿 |
| **P1** | xlings-res + xim-pkgindex | §4 strip → 打包(去 symlink)→ 双端镜像 → 描述符 | 真 Windows 上 `xlings install x86_64-linux-musl-gcc@16.1.0` 可装可跑 |
| **P2** | mcpp **单 PR** | §5 全部 8 项改动 + §6 CI 双 job,按 §1.3 的五段 commit 分层 | Windows 上 `mcpp toolchain list` 列出 linux-musl 行;交叉产物在 Linux job 上**真实执行**通过 |

**P0 是唯一的高不确定性环节**,且完全在 mcpp 代码之外 —— 它失败不会污染仓库。

> **不要因为 B1/B2「今天就已存在」而提前单独修**。它们缺少能证伪的测试环境:在没有 Windows→Linux 这条路径
> 之前,B2 的回归断言无处可挂,改了也只是「看起来对」。等 P0/P1 就位,断言才有落点 —— 这正是
> §1.3 commit 1 先行的意义。

---

## 9. 已决事项(2026-08-03 review 拍板)

| # | 问题 | 决定 | 依据 |
|---|---|---|---|
| 1 | 构建路线 | **musl-cross-make + `HOST=`**(选长期最合适,非最省事) | §3.3:`litecross/Makefile:87-125` 三条源码证据;与生态同工具、扩展/升级各改一行 |
| 2 | `host_can_serve` 谓词收敛 | **本期做** | §1.2:避免下一个 target 再踩两处推导 |
| 3 | B1/B2 交付形态 | **与主体合并为单 PR** | §1.3:用五段 commit 分层保住归因能力,commit 1 先证明断言会红 |
| 4 | 包形态 | **一包双 host asset** | §2:本机索引实测 20+ 包(ninja/cmake/git/rust)已用双段,xlings 原生支持 |

决定 1 的连带效果:原「路线 3 手工三段」被**取消**而非降级为并行选项 —— 其核心优点(target 库复用)
已被证实是 musl-cross-make canadian 模式的既有行为(`SYSROOT = /` + `MUSL_VARS = ` 清空),
不再有独立价值。它只保留为最后兜底,且动用即记技术债。
