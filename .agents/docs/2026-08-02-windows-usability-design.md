# Windows 可用性设计:裸机无感可用 + build.mcpp 全方言 + 测试面补齐

日期:2026-08-02 · 基线:main @ 7f1489d(mcpp 2026.8.1.1)
前置:`.agents/docs/2026-08-01-issue331-windows-msvc-triage.md`(issue #331 逐条核验)

> 本文所有代码坐标均对着 HEAD 核验过。**未在 Windows 实跑**——凡只能由 Windows 侧行为
> 决定的判断,文中标注「未实测」。

---

## 0. 摘要

mcpp 今天在 Windows 上有三个**正交**的故障面。它们互相独立触发、独立修复,但共同构成
「mcpp 在 Windows 上门槛高」这一个用户感受。

| | 故障面 | 触发条件 | 现状 |
|---|---|---|---|
| **F1** | 默认工具链选错 | 机器无 VS / BuildTools | 必挂 + **零诊断** |
| **F2** | 含空格路径被拆开 | 路径含空格(`Program Files`、用户名带空格) | 两处独立漏引号 |
| **F3** | build.mcpp 在 MSVC 上零支持 | `[toolchain] = msvc@system` | 三层全断 |

外加一个跨平台的能力缺口:

| | 缺口 | 现状 |
|---|---|---|
| **F4** | build.mcpp 不能 `import std;` | 只能 `#include`,无 std BMI 通道 |

对应五个组件:

| | 组件 | 主要触及 |
|---|---|---|
| A | Windows 首跑 detection-first + 意图分档回退 | `msvc.cppm` `triple.cppm` `prepare.cppm` |
| B | 含空格路径:两条通道收敛成一处 | `ninja_backend.cppm` `platform/process.cppm` |
| C | build.mcpp 三层(引号 / 方言 / 环境) | `dialect.cppm` `build_program.cppm` |
| D | build.mcpp 支持 `import std;` | `build_program.cppm`(复用 `stdmod.cppm`) |
| E | CI 测试面:三轴 + 自证门 + 新 e2e | `ci-fresh-install.yml` `ci-windows.yml` `tests/e2e/` |

**核心立场:不引入 clang + libc++ 的任何形态。** 裸 Windows 的洞不是「缺工具链」,是
「默认值选错」——零依赖可用的 `x86_64-windows-gnu`(winlibs GCC)早已是 `verified` 层、
早已有 Windows-native CI,只是不是默认。理由见 §7。

---

## 1. 问题模型

### 1.1 F1 — 默认工具链在裸 Windows 上必挂

一般 Windows 电脑**没有** MSVC STL。系统自带的只有 UCRT 运行时 DLL;头文件与导入库来自
Windows SDK,STL 来自 Visual Studio / Build Tools 的 "Desktop development with C++" 负载,
两者都不预装。

而 mcpp 的首跑默认把 macOS 和 Windows 并成了一条分支:

```cpp
// src/toolchain/triple.cppm:148
inline constexpr std::string_view kFirstRunMacWin = "llvm@20.1.7";
```

```cpp
// src/build/prepare.cppm:1209
if constexpr (mcpp::platform::is_macos || mcpp::platform::is_windows) {
    defaultSpec = std::string(pins::kFirstRunMacWin);
}
```

Windows 的 host triple 是 MSVC ABI(`triple.cppm:137`,`t.env = "msvc"`),而 clang 打 MSVC
ABI 时用的是 MSVC STL 而非 libc++:

```cpp
// src/toolchain/clang.cppm:138-140
// Clang targeting MSVC uses MSVC STL, not libc++.
bool msvTarget = is_msvc_target(tc);
tc.stdlibId    = msvTarget ? "msvc-stl" : "libc++";
```

⇒ **裸机器上默认路径必挂**,而且安装那一步是成功的(llvm payload 装得下来),失败发生在
之后的编译期。

**零诊断。** `prepare.cppm:1284` 现有的引导只覆盖 `CompilerId::MSVC && envOverrides.empty()`
——即「检测到 VC tools 但缺 SDK」。clang-targeting-msvc 在完全没有 VS 的机器上不触发任何
检查,用户拿到的是 clang 自己的 `'vector' file not found` 或
`unable to find a Visual Studio installation`,从这里推不出「该换 `--target x86_64-windows-gnu`」。

**可用的路径就在旁边。** `triple.cppm:106` 早已把 `x86_64-windows-gnu` 登记为 `verified`
层、pin `gcc@16.1.0`、默认静态;`registry.cppm:240-242` 在 Windows host 上把它映射到
winlibs 的 `mingw-gcc`;`tests/e2e/97_mingw_toolchain.sh` 每次 Windows CI 都在真跑,并且
断言到「产物在剥离 PATH 的干净目录下能跑」。这条路零 VS 依赖、全静态自包含、`import std`
可用——唯一的问题是它不是默认,而这个事实只写在 `README:311-313` 的脚注里。

**首跑方案单独盖不住的洞。** detection-first 只在「没有任何默认值」时生效。一个用户在无 VS
的机器上跑过一次旧版 mcpp,`llvm@20.1.7` 已被写进 `config.toml` 的 `[toolchain] default`,
从此每次构建都有默认值 ⇒ 首跑分支永不再进 ⇒ **永远撞墙**。存量用户和升级用户全在这个洞里。
所以 A 组件必须包含一个作用在**每次构建**上的修复门,而不只是首跑决策。

### 1.2 F2 — 含空格路径被拆开

同一份 manifest `[build] include_dirs` 经两条通道到达编译命令行,只有一条做了 shell 引号。

**通道一(全局,正确)** —— `flags.cppm:223`/`:241`:

```cpp
includeTokens.push_back(std::string(d.includePrefix) + p.string());
...
include_flags += shell_quote_arg(escape_path(std::filesystem::path(t)));
```

`:219-221` 的注释明写这么做的理由:*"ninja-$-escape and shell-quote per token (#234) so an
include dir whose name contains a space can't silently split into two shell words once ninja
hands the resolved command line to the shell."*

**通道二(per-TU,漏了)** —— `ninja_backend.cppm:108-113`:

```cpp
std::string local_include_flags(const CompileUnit& cu, bool msvcDialect) {
    ...
    for (auto const& inc : cu.localIncludeDirs) {
        flags += " -I";                     // ← 硬编码,未用 d.includePrefix
        flags += escape_flag_path(inc);     // ← 只有 ninja `$` 转义,没有 shell 引号
    }
```

`cu.localIncludeDirs` 的来源同样是 manifest(`plan.cppm:883` →
`local_include_dirs_for_manifest`,`scanner.cppm:999` → `local_include_dirs_for`),所以这是
**同一个语义、两处推导**。ninja 反转义后把裸空格交给 shell,路径当场裂开。

这条**不是 Windows 专属**:Linux 上 `include_dirs = ["/home/my dir/inc"]` 以完全一样的方式
中招。只是 Windows 上 `C:\Program Files\...` 让它变成日常。

同一函数还有第二个小问题:`msvcDialect` 形参只被 `localIncludeDirsAfter` 那一半使用,前一半
硬编码 `-I`。cl.exe 接受 `-I`,所以今天无害,但这是同一处硬编码的另一面。

**第二处** —— `platform/process.cppm:234` 的 `command_from_argv`,被 `:400` 和 `:459` 两个
执行入口使用。Windows 下把 argv 拼成 `cmd.exe /c` 的字符串时 `argv[0]` 未加引号,payload 装
在 `C:\Program Files\...` 或用户名带空格的机器上即断(**未实测**,依据是代码里无引号逻辑)。
这条是 build.mcpp 通道的前置——不修它,§4 的后两层白做。

### 1.3 F3 — build.mcpp 在 MSVC 上零支持,共三层

**第一层:引号。** 同 §1.2 第二处。

**第二层:方言。** `build_program.cppm` 整个文件 `grep -i msvc` 只有注释命中,**零代码分支**。
编译 argv 写死 GNU 驱动语法(`:677-695`):

```cpp
std::vector<std::string> compileArgv = { hostCompiler.string(), std_flag, "-O0" };
...
compileArgv.push_back("-x"); compileArgv.push_back("c++");
...
if (staticHostHelper) compileArgv.push_back("-static");
compileArgv.push_back("-o"); compileArgv.push_back(bin.string());
```

`-O0` / `-x c++` / `-static` / `-o` 一个都不是 cl.exe 的语法。`host_base_flags(tc)`
(`:168-243`)也只有 Clang 和 GCC 两个分支,MSVC 走到底会拼出一串 `-B` / `-L` / `-Wl,`。

指令面同病:`parse_line`(`:132-134`)把 `mcpp:link-lib=foo` 硬拼成 `-lfoo`、
`mcpp:link-search` 硬拼成 `-L`,MSVC 侧应是 `foo.lib` 和 `/LIBPATH:`。

**第三层:环境。** `model.cppm:46` 的 `envOverrides`(MSVC 的 `INCLUDE` / `LIB` / `VSLANG`)
目前**只有** `ninja_backend.cppm:1342`/`:1395` 在消费。`build_program.cppm` 调的是
`capture_exec(compileArgv, {}, compileCwd)` —— 传的是空 env。所以即使方言全翻译对了,
cl.exe 依然找不到 `<cstdio>`。

> **这是 #331 报告没看到的一层。** 报告只说到第一层;修好引号后 MSVC 上的 build.mcpp
> 依然编不过,只是错误从 `'C:\Program' is not recognized` 变成
> `D9002: ignoring unknown option '-O0'` + `LNK1181`,再修好方言又会变成
> `cannot open include file: 'cstdio'`。三层必须一起过。

### 1.4 F4 — build.mcpp 不能 `import std;`

`build_program.cppm` 只认 `import mcpp`:

```cpp
// src/build/build_program.cppm:666
bool usesModule = srcText.find("import mcpp") != std::string::npos;
```

没有任何 std BMI 通道。内置的 `mcpp` 模块(`:247` 的 `kMcppModuleSource`)特意在 global
module fragment 里用 `<cstdio>` / `<cstdlib>` 而**不用** `import std;`,注释写得很直白:
*"a typed API over the stdout wire protocol so build.mcpp can `import mcpp;`
(no `#include`, no `import std;`)"* —— 这是绕开缺口的权宜,不是设计意图。

结果是 build.mcpp 作为「原生 C++ 构建程序」的定位与 mcpp 自身的模块化主张自相矛盾:
mcpp 让用户全项目 `import std;`,却要求构建脚本回退到 `#include`。

---

## 2. 组件 A:首跑 detection-first + 意图分档回退

### 2.1 新谓词:什么叫「这台机器有可用的 MSVC」

`msvc.cppm` 增加:

```cpp
// 两件齐才算可用。
bool has_usable_msvc();   // = find_std_module_source() && find_windows_sdk()
```

两个查找器都是**现成的**(`msvc.cppm:34` / `:102`),不新增任何探测逻辑。

**为什么必须两件齐**:只探 `find_vs_install_path()` 会把「装了 VS 但只勾了 .NET 负载」判为
有 MSVC,然后在编译期才炸——恰好是现在这个 bug 的变种。要求 STL 与 SDK 同时在场,直接堆死
「有 VS 无 C++ 负载」和「有 VC tools 缺 SDK」两种半残状态。

### 2.2 pin 拆分

```cpp
// src/toolchain/triple.cppm:148 —— 拆前
inline constexpr std::string_view kFirstRunMacWin  = "llvm@20.1.7";

// 拆后
inline constexpr std::string_view kFirstRunMac     = "llvm@20.1.7";
inline constexpr std::string_view kFirstRunWinMsvc = "llvm@20.1.7";  // 探到 VS
inline constexpr std::string_view kFirstRunWinGnu  = "gcc@16.1.0";   // 未探到
```

`prepare.cppm:1209` 的 `if constexpr (is_macos || is_windows)` 拆成独立的 macOS / Windows
分支。Windows 分支:

```
has_usable_msvc()
  ├─ true  → defaultSpec = kFirstRunWinMsvc            (现行为完全不变)
  └─ false → defaultSpec = kFirstRunWinGnu
             defaultTarget = "x86_64-windows-gnu"
```

**两个轴都写。** 只写 `default_target` 也能工作——`prepare.cppm:1024` 的词表 pin 约定会把
`tcSpec` 自动带成 `gcc@16.1.0`——但依赖隐式推导会让 `mcpp toolchain list` 与 `config.toml`
的表述对不上。显式写两个轴,`toolchain list` 两个轴都打星,与 e2e 97 已有的断言一致。

`prepare.cppm:1167`(offline / no-auto-install 的硬错误分支)与 `:1224`(First run 的 info
文案)同步分档,否则会向裸 Windows 用户建议一条在他机器上不可用的命令。

### 2.3 意图来源分档

在现有优先级链的四个赋值点各记一个来源标记。**零新配置字段**:

| 赋值点 | 来源 | 语义 |
|---|---|---|
| `prepare.cppm:947` | `ManifestToolchain` | 项目 `mcpp.toml [toolchain]` —— 用户显式 |
| `prepare.cppm:951` | `GlobalDefault` | 全局 `config.toml` —— mcpp 自选居多 |
| `prepare.cppm:1015` | `TargetSection` | `[target.X].toolchain` —— 用户显式 |
| `prepare.cppm:1024` | `TargetPin` | 词表约定 —— mcpp 自选 |
| `prepare.cppm:1274` | `FirstRun` | 本次首跑写入 —— mcpp 自选 |

分档策略:

```
用户显式(ManifestToolchain | TargetSection)
    → 硬失败 + 诊断。不静默推翻用户写死的选择。
      一个真需要 MSVC ABI(要链 vcpkg 预编译 .lib)的项目,
      静默换 ABI 比报错更坏。

mcpp 自选(GlobalDefault | TargetPin | FirstRun)
    → 自动回退到 winlibs,重写全局默认,一行 info。真·无感。
```

### 2.4 存量修复门

位置:`detect()` 之后,即现在 `prepare.cppm:1284` 那个 MSVC-缺-SDK 检查所在处。

**两个检查合成一个判据**,而不是并排两条 —— 否则又是一处「同一决策两处推导」:

```
判据:目标是 MSVC ABI(msvc@system 或 clang→msvc target)且 !has_usable_msvc()
  ├─ 来源 = 用户显式 → 硬失败 + 诊断
  └─ 来源 = mcpp 自选 → 回退 winlibs + 重写全局默认 + info
```

这一门作用在**每次构建**上,因此同时盖住了 §1.1 末尾那个首跑方案盖不住的洞:已经把
`llvm@20.1.7` 写进 `config.toml` 的存量/升级用户,下次构建自动被修好,无需任何手动命令。

现有 `:1284` 的「有 VC tools 但缺 SDK」文案作为诊断分支的一个 case 保留,不丢信息。

### 2.5 offline 例外

`mcpp::platform::env::offline_mode()` / `no_auto_install()` 为真时**不自动安装** winlibs,
只走诊断分支。否则一个关网的 CI 会在毫无预期的情况下去拉 ~200MB payload —— 这正是
`:1150-1183` 那段 offline 硬错误存在的理由,回退路径必须尊重它。

### 2.6 诊断文案

```
llvm on Windows targets the MSVC ABI and needs Visual Studio / Build Tools
(UCRT + Windows SDK + MSVC STL), which was not found on this machine.

  • no Visual Studio? use the self-contained MinGW-w64 toolchain — no VS required:
        mcpp toolchain default gcc@16.1.0 --target x86_64-windows-gnu
  • have Visual Studio? install the "Desktop development with C++" workload
    (it provides the MSVC STL and the Windows SDK), then retry.
```

---

## 3. 组件 B:含空格路径

### 3.1 两条通道收敛成一处

`ninja_backend.cppm:108` 的 `local_include_flags` 改为:

- 用 `d.includePrefix` 取代硬编码的 `" -I"`(与 `flags.cppm:223` 一致);
- 每个 token 走与 `flags.cppm:241` **同一个** quoting helper。

**这是本组件的关键约束**:修法不能是在 `:112` 再抄一遍 `shell_quote_arg(escape_path(...))`,
而必须让两条通道调用同一个函数。否则下次新增第三条通道时会以完全相同的方式再漏一次 ——
本 bug 本身就是「#234 修了通道一、通道二没跟上」的产物。

抽出的 helper 建议落在 `flags.cppm`(通道一现居地,且 `ninja_backend` 已依赖它),签名形如:

```cpp
std::string include_token(const CommandDialect& d,
                          const std::filesystem::path& dir);  // prefix + escape + quote
```

`localIncludeDirsAfter` 那一半的 `-idirafter` / `/I` / NASM `-I` 三态降级逻辑
(`ninja_backend.cppm:126-131` 的注释)保持不变,只在 quoting 上并轨。

### 3.2 `command_from_argv` 的 Windows argv[0]

`platform/process.cppm:234`。Windows 下走 `cmd.exe /c` 时对 `argv[0]` 加引号。

**这条必须在 Windows 上实测**才能定稿:`cmd.exe` 的引号规则与 `CreateProcess` 不同
(`/c "a b" c` 的整体剥壳行为、内嵌引号的处理),从 Linux 侧推断不可靠。实施时先写一个
最小复现(payload 路径含空格 → `capture_exec`),在 CI 上跑通再改。

覆盖它的测试见 §6.3。

---

## 4. 组件 C:build.mcpp 三层

### 4.1 L1 引号

同 §3.2。

### 4.2 L2 方言:扩 `CommandDialect`

`dialect.cppm:22` 的 `CommandDialect` 目前只有编译侧字段,**没有链接侧**。新增四个:

| 字段 | GNU | MSVC | 用处 |
|---|---|---|---|
| `libFlag` | `-l{}` | `{}.lib` | `parse_line` 的 `mcpp:link-lib` |
| `libSearchPrefix` | `-L` | `/LIBPATH:` | `parse_line` 的 `mcpp:link-search` |
| `forceCxxLang` | `-x c++` | `/TP` | `.mcpp` 扩展名对编译器未知 |
| `staticRuntime` | `-static` | `/MT` | `staticHostHelper` |

`libFlag` 用格式串而非前缀,因为 MSVC 是后缀形态(`foo.lib`)而非前缀形态,一个
`std::string_view prefix` 表达不了。

改造两处消费点:

- `build_program.cppm:677-695` 的 `compileArgv` —— `-O0` 走 `d.optPrefix`、`-o` 走
  `d.outputObjPrefix`(或链接输出的对应形态)、`-x c++` 走 `d.forceCxxLang`、`-static` 走
  `d.staticRuntime`;
- `build_program.cppm:132-134` 的 `parse_line` —— `link-lib` / `link-search` 走上表。

`parse_line` 目前不持有 dialect,需要把它传进去(或把翻译推迟到消费点)。**倾向后者**:
`parse_line` 产出中立的结构化字段(`libs` / `libSearchDirs`),翻译发生在拼 argv 的地方 ——
这样指令协议本身保持方言无关,和 `mcpp:` 协议是「声明式」的定位一致。

### 4.3 L3 环境

- `build_program.cppm` 的 `capture_exec(compileArgv, {}, compileCwd)` 改为传
  `tc.envOverrides`(`model.cppm:46`);
- `host_base_flags`(`:168`)加 MSVC 分支,返回空 —— MSVC 不用 `-B`/`-L`/`--sysroot`,
  它的搜索路径**全部**经由 `INCLUDE` / `LIB` 环境变量,这正是 L3 存在的理由。

### 4.4 模块路径的边界

MSVC × `import mcpp;`(以及 MSVC × `import std;`,见 §5)走的是 `.ifc` + `/reference`,与
GCC 的 `gcm.cache` / Clang 的 `-fmodule-file=` 差异较大,是独立的一块工作量。

**本方案明确不做**,但必须**明确报错**而非静默炸:

```
build.mcpp: `import mcpp;` / `import std;` are not yet supported under MSVC.
            Use `#include` in build.mcpp, or build with a GCC/Clang toolchain.
```

两者共用同一个门与同一条诊断,不是两处推导。

---

## 5. 组件 D:build.mcpp 支持 `import std;`

### 5.1 机器全在,只差接线

主构建的 std 模块通道是 `mcpp::toolchain::stdmod::ensure_built`
(声明 `stdmod.cppm:63`,调用点 `prepare.cppm:3785`),返回:

```cpp
struct StdModule {              // stdmod.cppm:46
    std::filesystem::path bmiPath;          // <cacheDir>/gcm.cache/std.gcm
    std::filesystem::path objectPath;       // <cacheDir>/std.o
    std::filesystem::path compatBmiPath;
    std::filesystem::path compatObjectPath;
};
```

按 (工具链 × 标准 × 方言) 三层缓存。build.mcpp 直接复用同一个函数,四步:

1. **检测** —— 沿用 `:666` 同款子串检测,增加 `import std;` / `import std.compat;`;
2. **构建** —— `ensure_built(hostTc, m->package.standard, stdFlagAndDialect, ...)`;
3. **喂入** —— 按方言:
   - GCC → 把 BMI 软链/拷到 `bdir/gcm.cache/std.gcm`。`import mcpp;` 已经是这个模式
     (`:698` 把 `compileCwd` 设成 `bdir` 正是为此),std 只是多一个成员;
   - Clang → `-fmodule-file=std=<path>`,复用 `flags.cppm:358` 的 `stdBmiUsePrefix` 与
     `staged_std_bmi_path`;
4. **链接** —— `stdObjectPath` 加进 `compileArgv`,位置同 `:686` 的 `mcpp.o`。

### 5.2 唯一必须盯死的点:host ≠ target

`ensure_built` 必须喂**宿主**工具链,不能喂 `*tc`。

好消息是这个坑已经被填过:`prepare.cppm:1349` 的 `host_tc_for_build_program()` 在交叉构建
下会单独解析一份宿主工具链,注释明写 *"Deliberately NO target injection: the spec resolves
for the host."*。接线时取它返回的 `htc` 即可。

喂错的后果是产出一个**在宿主上跑不了的 helper**——正是 mingw-cross 那批 host≠target bug
的同款失败模式(std 源探测 / binutils `-B` ×2 / `-lstdc++exp` 门全部错在同一个轴上)。这
一条在实施时应有单测或 e2e 直接锁住,不能只靠 review —— 现成的挂载点是
`tests/e2e/112_build_mcpp_cross.sh`(唯一一个跑交叉 build.mcpp 的 e2e),给它补一条
`import std;` 断言即可,不必新写。

### 5.3 成本

- 本地构建:`hostTc == *tc`,std BMI 与主构建**共享缓存,零额外开销**;
- 交叉构建:多编一份宿主 std —— 合理且不可避免。

### 5.4 标准档位

`import std;` 的可用性受 `importStdMinLevel` 门控(`clang.cppm:151` = 20,MSVC = 23)。
build.mcpp 的 `std_flag` 来自 `m->package.standard`,与主构建同源,所以这个门自动生效,
无需额外逻辑。若项目标准低于门槛,应报与主构建一致的诊断。

### 5.5 顺带解锁(不在本方案内)

内置 `mcpp` 模块(`build_program.cppm:250` 的 `kMcppModuleSource`)可以不再受「不能
`import std;`」的约束。但它现在的 C 级原语实现工作正常且零依赖,**本方案不动它** ——
留作后续,并把 `:247` 的注释更新为陈述现状而非陈述限制。

### 5.6 检测方式的已知弱点

子串检测(`srcText.find`)会命中注释和字符串字面量里的 `import std`。

- **过检**:多编一份 std BMI。本地零成本(共享缓存),交叉构建浪费一次编译。
- **漏检**:非常规写法(如 `import  std;` 双空格)导致编译报错。

两种失败模式都不产生**错误的构建结果**,且与现有 `import mcpp` 检测的行为一致。本方案
沿用,不引入完整扫描器 —— 若将来 build.mcpp 通道接入 P1689 扫描,两者一起升级。

---

## 6. 组件 E:CI 测试面

### 6.1 硬约束

GitHub Actions **没有** Windows 10 / 11 客户端镜像,也**没有**任何不带 VS 的 Windows runner。
可用标签只有:

| 标签 | 实际 | 内核近似 |
|---|---|---|
| `windows-2022` | Windows Server 2022 | ≈ Win10 21H2 |
| `windows-2025` | Windows Server 2025 | ≈ Win11 24H2 |
| `windows-11-arm` | Windows 11 ARM64 | —— |

因此:「win10 / win11」映射为 `windows-2022` / `windows-2025` 两个镜像;「无 MSVC」必须
自己造。

`windows-11-arm` **不纳入**:`aarch64-windows-gnu` 在 `triple.cppm:101` 的 `kKnownTargets`
里根本不存在,winlibs 也无 arm64 payload —— 那是一条独立的移植线,不是测试面问题。

### 6.2 fresh-install 三轴

`ci-fresh-install.yml:383` 的 `windows-fresh` 从 1 个 job 拆成 3 个:

| job | runner | 内容 |
|---|---|---|
| `windows-2022-fresh` | `windows-2022` | 现有全套 |
| `windows-2025-fresh` | `windows-2025` | 现有全套 |
| `windows-nomsvc-fresh` | `windows-2025` + 遮蔽 | 见 §6.3 |

三个 job 共享同一段步骤定义(composite action 或 reusable workflow),避免三处推导。

### 6.3 无 MSVC 面:遮蔽法 + 自证门

**遮蔽**(runner 是一次性的,破坏无所谓):

- 重命名 `C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe`
  (`msvc.cppm:154` 写死的路径);
- 重命名 VS 安装根;
- 清 `VSINSTALLDIR` / `VCINSTALLDIR` / `VCToolsInstallDir` / `VS*COMNTOOLS`
  (`msvc.cppm:168` 的 env 策略)。

**遮蔽法唯一的风险**是漏遮一条探测策略(`msvc.cppm:239` 的三级 fallback:vswhere → env →
已知路径)导致「其实还探得到 MSVC,于是走了老路径而全绿」的假绿。

**自证门直接消掉这个风险** —— job 的第一步:

```
1. mcpp toolchain default msvc    → 必须失败      ← 遮蔽生效的自证
2. mcpp new / build / run         → 必须全绿      ← 无感可用
3. mcpp toolchain list            → 星在 x86_64-windows-gnu
4. 产物拷到干净目录、剥离 PATH 后运行 → 必须成功   ← 自包含
```

第 1 步一旦因为遮蔽不全而**通过**,job 当场红。假绿在结构上不可能。

第 4 步复用 `tests/e2e/97_mingw_toolchain.sh` 已验证的模式
(`PATH="/usr/bin:/c/Windows/System32"` + `objdump -p` 导入表检查)。

### 6.4 新 e2e

现有编号最高 `178_test_observability.sh`,新增从 179 起:

| 编号 | 覆盖 | `# requires:` |
|---|---|---|
| 179 | 含空格路径工程(`.../my dir/proj`,含 `include_dirs` + build.mcpp) | —— (跨平台) |
| 180 | MSVC × build.mcpp(`#include` 形态,三层全过) | `windows msvc` |
| 181 | build.mcpp `import std;`(GCC + Clang) | —— (跨平台) |
| 112(扩) | 已有的 `build_mcpp_cross.sh` 补一条 `import std;` 断言 —— 锁 §5.2 的 host≠target | `mingw-cross` |
| 182 | 裸 Windows 回退 + 意图分档(显式配置须硬失败) | `windows` |

179 与 181 是跨平台的 —— 空格拆分和 `import std;` 都不是 Windows 专属,在 Linux 上跑更快、
反馈更早。

> **提醒**:`# requires:` 必须在脚本第 2 行,否则等同没写(见
> `.agents/docs/2026-07-31-test-observability-*`)。

### 6.5 补现有 CI 的空缺

`ci-windows.yml:267` 的 MSVC 步骤目前跑 `99_msvc_native_build.sh`,其中 `build.mcpp` 出现
**0 次**;整个 Windows e2e 套件(`ci-windows-e2e.yml:70`)跑在 `llvm@20.1.7` 上,十三个涉及
build.mcpp 的 e2e(89 / 92 / 97 / 110 / 111 / 112 / 124 / 125 / 143 / 144 / 145 / 164 / 168)
全在 clang 下、payload 路径不含空格。

> **覆盖缺口是本批次三条 A 类问题的共同成因**:
> `MSVC × {build.mcpp, 含空格路径, cppwinrt, /MT}` 这个笛卡尔积在 CI 里是空的。

MSVC 步骤补 180。

---

## 7. 明确拒绝

### 7.1 Windows 上加 clang + libc++ 并设为默认 —— 拒绝

分两种形态,都不成立:

**(a) libc++ 配 MSVC ABI(`x86_64-pc-windows-msvc` + libc++)**

- LLVM 官方**不发布** Windows 的 libc++ 二进制,上游属 experimental,locale / 线程 / 异常
  几块长期有缺口;
- **ABI 隔离**:libc++ 的 `std::string` / `std::vector` 与 MSVC STL 不兼容。一旦选它,
  vcpkg 的 MSVC 预编译包、任何第三方 `.lib`、系统 SDK 里跨 `std::` 类型的接口全部不能链;
- **而且它并不解决问题**:MSVC ABI 依然需要 Windows SDK 的 `ucrt`/`um`/`shared` 头和
  import lib,裸机器上照样没有。

**(b) llvm-mingw(clang + libc++ + lld + libunwind,`x86_64-windows-gnu`)**

- 技术上可行;
- 但与已有的 winlibs GCC **功能等价** —— 同一个 target、同一个 ABI、同一类产物;
- 加它 = 给同一个 target 加第二套 stdlib 实现,边际收益(clang 诊断、libc++)远小于成本
  (多一条 payload 发布线、多一组 `import std` 路径、多一个 BMI 缓存维度、CI 多一轴)。

**结论:裸 Windows 的洞已被 winlibs GCC 填上,缺的只是默认值。**

### 7.2 其余拒绝项(承自 #331 triage)

| 项 | 理由 |
|---|---|
| `ldflags` 路径启发式 | `join_flags` 注释里记着上次同类事故(0.0.97 的 C3 回归);正解是补 typed 字段 |
| `[build] linkage` 简写 | `docs/05-mcpp-toml.md:554` 已明文(*"`toolchain` / `linkage` are exact-triple only"*);这是文档错误不是功能缺失 |
| mcpp 内置 cppwinrt 生成 | 属 build.mcpp 的职责范围,不是引擎职责 |

---

## 8. 风险与不覆盖

| 风险 | 缓解 |
|---|---|
| 遮蔽清单与 `msvc.cppm` 探测策略漂移 | §6.3 的自证门:漏遮当场红,不会假绿 |
| 自动回退改写全局 config,让 CI 意外切工具链 | §2.5:offline / no-auto-install 下只诊断不安装;且分档只对 mcpp 自选的默认生效 |
| `cmd.exe` 引号规则从 Linux 侧推断不可靠 | §3.2:先写最小复现在 Windows CI 上跑通再定稿 |
| `ensure_built` 喂错工具链产出跑不了的 helper | §5.2:用 `host_tc_for_build_program()`,并由 e2e 112(扩)在交叉场景下锁住 |

**明确不覆盖**:

- 真实客户端 SKU 独有的行为(UAC 提权、Defender 实时扫描干扰、长路径策略)—— GHA 无客户端
  镜像,只能靠自托管 runner,不在本方案内;
- ARM64 Windows(见 §6.1);
- MSVC × 模块化 build.mcpp(见 §4.4,明确报 unsupported)。

---

## 9. 实施顺序

按「改动小 × 收益大」排,前三条各自独立可交付:

| 序 | 内容 | 规模 | 收益 |
|---|---|---|---|
| 1 | 组件 A(首跑分档 + 存量修复门 + 诊断) | 小 | 裸 Windows 从「必挂」变「无感可用」,零新组件 |
| 2 | 组件 B(两条通道收敛 quoting) | 小 | 跨平台;含空格路径不再静默拆分 |
| 3 | 组件 D(build.mcpp `import std;`) | 小 | 跨平台;机器全在,只差接线 |
| 4 | 组件 E 的 CI 三轴 + 自证门 | 中 | 锁住 1 的行为;测试面从 1 个镜像变 3 轴 |
| 5 | 组件 C L1(argv[0] 引号) | 中 | 需 Windows 实测;是 6 的前置 |
| 6 | 组件 C L2+L3(方言 + 环境) | 大 | MSVC 上的 build.mcpp 从零到可用 |
| 7 | 组件 E 的 e2e:新增 179–182 + 扩 112 | 中 | 与 1/2/3/6 各自配套,随对应项落地 |

1–3 可并行,互不触碰同一函数(A 在 `prepare.cppm` 上半段,B 在 `ninja_backend`/`flags`,
D 在 `build_program.cppm` 下半段)。5–6 串行且必须在 Windows 上实测。

---

## 附:代码坐标速查

| 坐标 | 内容 |
|---|---|
| `src/toolchain/triple.cppm:148` | `kFirstRunMacWin` —— 待拆分 |
| `src/toolchain/triple.cppm:106` | `x86_64-windows-gnu` 词表项(verified,pin gcc@16.1.0) |
| `src/toolchain/triple.cppm:137` | Windows host triple `env = "msvc"` |
| `src/toolchain/clang.cppm:138-140` | clang 打 MSVC ABI ⇒ `stdlibId = "msvc-stl"` |
| `src/toolchain/msvc.cppm:34/:102` | `find_std_module_source` / `find_windows_sdk` |
| `src/toolchain/msvc.cppm:154/:168/:239` | vswhere / env / 三级 fallback —— 遮蔽清单来源 |
| `src/toolchain/registry.cppm:240-242` | Windows host → `mingw-gcc`(winlibs) |
| `src/toolchain/dialect.cppm:22` | `CommandDialect` —— 待扩四字段 |
| `src/toolchain/stdmod.cppm:46/:63` | `StdModule` / `ensure_built` |
| `src/toolchain/model.cppm:46` | `envOverrides` |
| `src/build/prepare.cppm:947/951/1015/1024/1274` | tcSpec 优先级链 —— 意图来源标记点 |
| `src/build/prepare.cppm:1167/1209/1224` | offline 硬错误 / 首跑默认 / First run 文案 |
| `src/build/prepare.cppm:1284` | 现有 MSVC-缺-SDK 诊断 —— 与新门合并 |
| `src/build/prepare.cppm:1349` | `host_tc_for_build_program()` —— host≠target 已解 |
| `src/build/prepare.cppm:3785` | `ensure_built` 主构建调用点 |
| `src/build/flags.cppm:223/241` | 通道一:`d.includePrefix` + `shell_quote_arg` |
| `src/build/flags.cppm:358` | `stdBmiUsePrefix` / `staged_std_bmi_path` |
| `src/build/ninja_backend.cppm:108-113` | 通道二:硬编码 `-I`,漏 shell 引号 |
| `src/build/ninja_backend.cppm:1342/1395` | `envOverrides` 唯一消费点 |
| `src/build/build_program.cppm:132-134` | `parse_line` 的 `link-lib` / `link-search` |
| `src/build/build_program.cppm:168-243` | `host_base_flags` —— 只有 Clang/GCC |
| `src/build/build_program.cppm:666` | `usesModule` 子串检测 |
| `src/build/build_program.cppm:677-695` | `compileArgv` —— 写死 GNU 语法 |
| `src/build/build_program.cppm:698` | `compileCwd = bdir`(gcm.cache 定位) |
| `src/platform/process.cppm:234/400/459` | `command_from_argv` 及两个调用点 |
| `.github/workflows/ci-fresh-install.yml:383` | `windows-fresh` —— 待拆三轴 |
| `.github/workflows/ci-windows.yml:261/267` | mingw e2e 步骤 / MSVC 步骤 |
| `tests/e2e/97_mingw_toolchain.sh` | winlibs 自包含的既有验证模式 |
| `tests/e2e/112_build_mcpp_cross.sh` | 唯一的交叉 build.mcpp e2e —— §5.2 的挂载点 |
| `docs/05-mcpp-toml.md:554` | `toolchain` / `linkage` 仅限精确 triple(§7.2) |
