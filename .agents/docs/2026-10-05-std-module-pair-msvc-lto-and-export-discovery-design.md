---
subject: design
status: active
---

# 下一个版本的发布方案：标准库模块、原生 MSVC LTO 与导出发现、共享库的链接配置、Windows 参数引号（#768 后续、#770、#771）

- 日期：2026-10-05。状态：修订 6，按 review 结论（D10a、D10b、D11、O6 通过，其余按建议）实现于 mcpp 2026.10.5.2。本文是**下一个版本的统一发布方案**，所有项目在一个发布 PR 中完成（§14）。
- 基线：`main`  `8d9bde29`（已合入 #769）。
- 证据：
  - 临时探测 PR #772：只保留一个探测 workflow，探测脚本不做断言。
    - run `37290831503`：直接调用 cl / link / lib。
    - run `37291652508`：用当前 commit 构建的 mcpp，在普通环境和 VS 开发者环境各跑一次。
    - run `37297856040`：O6（Windows 代码页）和 O7（工作空间的 profile，Linux）。O7 的结果与本地结果逐条一致，只有图目录的排列顺序不同。
  - Linux 本地对照：mcpp 2026.10.5.1，加上一个修改了链接组的实验构建。
  - 环境：`windows-latest`，cl 19.51.36260 / MSVC 14.51.36231 / LLVM 22.1.8 / SDK 10.0.26100.0；Linux x86_64，gcc 16.1。
- 只对 review 有价值的部分写进正文；完整日志见 #772 的 job summary。

**修订 6：实施记录。** 实现与本文的差异：

| 项 | 本文 | 实现 | 理由 |
| --- | --- | --- | --- |
| D1 | `StdModuleSet` 值类型 | `Toolchain::set_std_modules` / `clear_std_modules` 两个成员函数，六处写入点全部改用它们 | 字段保持原样，避免改动每个读取点；不变式同样由一处保证 |
| D2 | 只覆盖已有 compat 的行 | 同时补上 GCC 行的 `std.compat`（`bits/std.compat.cc`） | 实施中发现 GCC 行的 `import std.compat` 一直失败，而 SPEC-009 §6.2 要求 Default 行通过它 |
| D5 | `std::optional<bool>` | `windowsAutoExport` 加 `windowsAutoExportDeclared` 两个成员 | 仓库约定：导出结构体不用 `std::optional` 数据成员（`Profile` 的注释记录了 clang + MSVC STL 下的编译失败） |
| D5 | 按包降级 | 以 `/GL-` 追加到受影响包的单元 flag | cl.exe 的后置 `/GL-` 覆盖前置 `/GL`，不需要第二套全局 flag |
| D6 | `pack` 静态库不带 LTO | `BuildOverrides::no_lto_in_archives`，由库打包流水线设置；gcc / clang 用 `-fno-lto`，cl.exe 用 `/GL-` | 与 D5 同一机制 |
| D9 | `+ lto (partial)` | `BuildConfig::ltoPartial` | — |
| D7b | 一个版本的 note | `link/root-flags`，只在根包私有 flag 中有搜索路径或库、且计划中有依赖的共享库时给出 | — |
| D11 | `[test] windows_code_page` | 测试程序只接收应用程序清单，名字置于 `res/tests/` 之下 | 测试名是路径，并可能与程序名相同 |

**任务依赖与跨仓库协作。**

```
mcpp（一个发布 PR，release/2026.10.5.2）
  D8 引号 ─┐
  D7 链接组 ┤
  D3 读取器 ┼─→ SPEC / docs / CHANGELOG ─→ 完整 CI ─→ 自审 ─→ squash 合入 main
  D4–D6,D9 ┤
  D1,D2    ┤
  D10,D11  ┘
          ─→ release.yml（四个平台构建 → GitHub Release → mcpp-release.json）
              ─→ xlings-res/mcpp 镜像（GitHub + GitCode；GitCode 不完整时用 gtc 在本地补齐）
              ─→ openxlings/xim-pkgindex 的 bump PR（机器生成）─→ 合入 ─→ 索引 latest 生效
                  ─→ xlings 生态验证（干净的 XLINGS_HOME 中 install mcpp@2026.10.5.2，构建并运行样例）
                  ─→ （可选）.xlings.json 的引导版本
```

- 发布 PR 合入前，main 上排队的 CI 让位于 PR 的 CI；合入后先跑 release，再重新触发 main 的 CI。
- #772（探测 PR）保留到发布完成，用于复测；#773 由本发布覆盖，合入后在其中说明并关闭。

**修订 5 的变化：**
- O6 查明原因（§13）：测试本身依赖代码页；测试程序与 mcpp.exe 运行在不同的代码页。纳入方案（D11）。
- O7 查明并扩展为 “工作空间根与根包的复用”（§12）：
  - 虚拟根上写的 `[profile]` 被静默忽略。
  - profile 取自第一个被选中的成员。
  - 带 `[package]` 的工作空间，根包不继承 `[workspace.*]`。
  - 纳入方案（D10a–D10c）。

**修订 4 的变化：**
- 新增 §1 “诊断分级方针”，并把每一种情形逐条归类为报错、降级报告或提示（§1.3 总表）。
- 多角度自审后的修正：
  - **D5 降级的粒度从 “按目标” 改为 “按包”。** 发现边读取的 `lu.objects` 包含放入该镜像的静态依赖的对象（#646 F1），而且一个源文件只编译一次。
  - **新增：用户在 flag 中手写的 `/GL` 视为显式陈述。** mcpp 不改写透传的 flag，所以这种情形与自动导出冲突时在规划期报错，不降级。
  - **新增：`import std.compat` 而标准库不提供 compat 时，在规划期报错。** 今天这种情形只会在编译期得到 `module not found`。
  - D6 扩展为 “`pack` 产出的静态库不携带任何 LTO 中间表示”，覆盖所有工具链。
  - D7 定稿：图级 flag（ABI 一定是，profile 建议是）加上闭包 flag；消费者包的私有 flag 不再进入依赖的共享库。用一条 note 加 release note 处理这项兼容性变化。
- 去掉对外部 PR 的依赖项。#773 的对照数据作为证据保留在 §10.5。

---

## 0. 决策一览（待 review）

| # | 问题 | 提议 | 分级（§1） |
| --- | --- | --- | --- |
| D1 | std / std.compat 由谁设置 | `StdModuleSet`，每种标准库一个推导函数（§3） | 无诊断 |
| D2 | std.compat 何时编译 | 按需编译；import 了 compat 但标准库不提供时，规划期报错（§4） | E1 |
| D3 | 导出发现读到非普通 COFF 对象 | `/bigobj` 改为可读；`/GL` 和未知的匿名对象在构建期报错，并给出专门诊断（§6） | E3 |
| D4 | cl.exe 上的 `lto = true` | 兑现：`/GL`、`/LTCG`、`lib /LTCG`（§7） | 无诊断 |
| D5 | 原生 LTO 与自动导出冲突 | 显式 `true` → E1；flag 中手写 `/GL` → E1；没写 → 按包降级（W） | E1 / W |
| D6 | `pack` 产出的静态库与 LTO | 不携带 LTO 中间表示（所有工具链），降级并报告（§9） | W |
| D7 | 依赖拥有的共享库用哪条链接行 | 图级 flag + 自己闭包的 flag，所有计划都一样（§10） | — |
| D7a | profile 的 `ldflags` 是不是图级 | 是（保持非工作空间的现状） | — |
| D7b | 根包私有 flag 不再进入依赖 DLL | 接受（SPEC-004 §9.6）；一个版本内给出 N，并写 release note | N |
| D8 | Windows 参数引号 | 按 argv 规则引号；`WindowsSdkDir` 规范化（§11） | 无诊断 |
| D9 | 构建输出中的 `+ lto` | 只在 LTO 实际生效时显示（§7） | — |
| D10a | 工作空间中 `[profile.*]` 的来源 | 成为根位置键：成员按 profile 名继承工作空间根的表，成员自己声明的同名表优先（§12） | 无诊断，写 release note |
| D10b | 带 `[package]` 的工作空间，根包是否继承 `[workspace.package]` / `[workspace.build]` | 是，与其他成员一样恰好一次，各种选择下一致（§12） | 无诊断，写 release note |
| D10c | 根清单中键的归属 | 写成一张表：哪些是工作空间级、哪些是根包级（§12.4，进入 SPEC-004 §9） | — |
| D11 | Windows 代码页测试失败（O6） | 修正测试；测试程序可以声明代码页（`[test] windows_code_page`），mcpp 自己的测试使用 UTF-8（§13） | — |

---

## 1. 诊断分级方针

### 1.1 依据

- **mcpp 自身的不变式**（`src/diag.cppm` 模块注释）：“Any branch that does less because a condition was not met MUST either return an error or report through `diag::degraded()`.” `degraded` 必须写出 impact；`--strict` 在一处把所有 degraded 提升为错误；`note` 永不被提升。
- **mcpp 的先例：**
  - 互相矛盾的显式陈述要拒绝：`exports` + `windows_auto_export = false`（`plan.cppm`）。
  - 拒绝会破坏昨天还能构建的程序时，报告而不拒绝：Apple SDK libc++（`scan.cpp`）。
  - 无法产出正确产物时报错：`--required` 下的空导出面、`StdModulePrecompile`。
  - 透传的 flag 原样传递，mcpp 不改写（`flags.cppm`：“ldflags pass through verbatim”）。
  - 拒绝必须有 `refusal::Code`，并映射到 SPEC-003 的退出码。
- **行业惯例：**
  - CMake：IPO 被请求而工具链不支持时，CMP0069 的 NEW 行为是报错；它只是不默默忽略。
  - 发行版：不在静态库中分发 LTO 中间表示（例如 Fedora 的 LTO 方针）。
  - MSVC 文档：不建议分发由 `/GL` 对象组成的 `.lib`。

### 1.2 五条规则

| 级别 | 何时使用 | 机制 |
| --- | --- | --- |
| **E1 规划期拒绝** | 用户的**显式**陈述互相矛盾，或者构建图在规划期就能判定无法满足 | `refusal::Code` + 退出码；诊断点名两条陈述，并给出改法 |
| **E3 构建期报错** | 无法产出正确产物，而且只有到构建期才能看到（例如对象的内容） | 构建边失败，给出准确的原因与改法，不能是误导性的通用错误 |
| **W 降级报告** | 一个**隐式默认**无法兑现，但仍能产出正确（只是没那么优化）的产物；拒绝会破坏现有构建 | `diag::degraded`（what + impact + hint），每次运行一条汇总；`--strict` 下为错误 |
| **N 提示** | 修正了一个泄漏或不一致，带来规划期无法判定是否会产生影响的语义变化 | `diag::note`，只在一个版本内给出，只在相关时给出；同时写 release note |
| **无诊断** | 纯修复、纯重构、纯性能 | — |

还有两条约束：
- **绝不改写用户手写的透传 flag。** 手写的 flag 是显式陈述，只能接受它，或者在矛盾时拒绝。
- **绝不声称没有兑现的能力**（D9）。

### 1.3 每种情形的分级总表

| 情形 | 级别 | 理由 |
| --- | --- | --- |
| `windows_auto_export = true`（显式）+ cl.exe 上 LTO 生效，目标是 PE 共享库 | **E1** | 两条显式陈述矛盾；与 `exports` + `false` 同类；CMake 对显式不可满足的请求也报错 |
| 用户在 `cxxflags` 中写了 `/GL`（或 `-GL`），该包的对象进入一个开启自动导出的 DLL | **E1** | 手写 flag 是显式陈述，mcpp 不能去掉它；改法是关闭自动导出或去掉 `/GL` |
| 没写 `windows_auto_export` + cl.exe 上 LTO 生效 | **W** | 隐式默认无法兑现；降级后产物仍然正确；拒绝会破坏今天能构建的项目（今天 LTO 被忽略，构建成功） |
| 发现边读到 `/GL` 对象（来源是构建程序或 `CL` 环境变量，规划期看不到） | **E3** | 读不出符号，就无法产出正确的导出面；静默跳过会得到缺少导出的 DLL |
| 发现边读到未知类别的匿名对象 | **E3** | 不猜格式 |
| 发现边读到 `/bigobj` 对象 | 无（改为支持） | 格式有公开文档，CMake 也支持 |
| `import std.compat`，但所选标准库没有 compat 源 | **E1** | 规划期即可判定；今天要到编译期才得到 `module not found`，说不出原因 |
| `pack` 产出静态库，而 profile 开启了 LTO | **W** | 产物仍然正确（不带 LTO 中间表示）；不分发是行业惯例 |
| 某个行请求了 LTO 却无法兑现（今后出现的任何行） | **W**，并且不显示 `+ lto` | 一致的兜底规则 |
| 根包私有的 `ldflags` 中有搜索路径或库（`-L`、`-l`、`/LIBPATH:`、`*.lib`），而计划中有依赖拥有的共享库 | **N**（一个版本） | 这些 flag 不再进入依赖 DLL；规划期无法判定依赖是否需要它们；其他种类的 flag（rpath、version script、`/SUBSYSTEM`）进入 DLL 本来就是错的，不提示 |
| 引号修复、SDK 根规范化、按需编译 compat、`StdModuleSet` | 无 | 纯修复或重构 |
| 工作空间根上的 `[profile]` 今天被静默忽略（违反 `diag.cppm` 的不变式） | 修复为生效（D10a） | 这是该不变式所说的 “静默少做”，正确的修复是兑现它，而不是加一条警告 |
| 成员自己声明了与工作空间根同名的 profile | 无（文档化的优先级） | 与 `[toolchain]` 等根位置键的规则一致 |

---

## 2. 约束

| 约束 | 出处 |
| --- | --- |
| Default 行验收包含 `import std.compat` | SPEC-009 §6.2 |
| 能力取自载荷，不按版本推断 | SPEC-009 §6.1 |
| profile 的旋钮在每个编译器上兑现 | `flags.cppm` 中 `realised_opt_level` 的注释 |
| 同一份 manifest 在 ELF / Mach-O / PE 上都能构建 | `docs/04-mcpp-toml.md`（`windows_auto_export`） |
| 使用需求只从依赖流向消费者，禁止反向 | SPEC-004 §9.6 |
| ABI 开关到达 “every TU of every package, and the link” | `docs/22-target-side.md` 中的 `abi` 表 |
| 一个源文件在一次构建中只编译成一个对象 | `docs/04-mcpp-toml.md`（选择构建配置放在哪里） |
| 已发布的键不改名 | SPEC-004 §5.2 |
| 构建数据库与实际构建出自同一推导 | SPEC-005 |

---

## 第一部分：标准库模块

## 3. D1：`StdModuleSet`

- **现状**：`stdModuleSource` 和 `stdCompatSource` 是两个独立字段，在 6 处被设置或清空（clang 检测、msvc 检测、`bind_msvc_sysroot`、包提供的标准库、无宿主 std、Apple SDK）。#768 就是其中一处只改了一半。
- **设计**：
  - 新增 `struct StdModuleSet { path std; path compat; }`，不变式是 “compat 非空 ⇒ std 非空，且来自同一个库”。
  - 每种标准库一个推导函数：libc++ 取同目录的 `std.compat.cppm`；MSVC STL 取 `modules/` 下的两个 `.ixx`；包提供的标准库读 manifest 的两个键。
  - 写入点只能整体赋值；清空统一调用 `clear_std_modules`。`hasImportStd` 由 `std` 是否非空推导（第一步可以先加断言）。
- **自审**：纯重构，命令行和缓存键逐字节不变，用现有测试守住。

## 4. D2：std.compat 按需编译，缺失时在规划期报错

- **实测**（run `37291652508`，普通环境，llvm + MSVC STL）：清空 std 缓存后，只 `import std` 的程序构建完，缓存里出现了 `std.compat.pcm` / `std.compat.o`。
- **设计**：
  1. 扫描结果改为 `{ needsStd, needsStdCompat }`，覆盖整个图；判定复用 `imports_module(…, "std.compat")`。
  2. `ensure_built` / `describe_std_module` 增加 `wantCompat`，**只控制是否执行**。元数据仍包含 compat 的推导，否则 “要不要 compat” 的变化会连带重建 std。
  3. 构建数据库与构建程序路径用同一个条件。
  4. **新增 E1**：`needsStdCompat` 为真而 `StdModuleSet.compat` 为空时，在 `step11_std_module_availability_gate` 中拒绝（与 `std` 不可用时同一个代码，或者新增一个兄弟代码），诊断说明所选标准库（`stdlibId`）不提供 `std.compat`。
- **行业惯例**：CMake 和 MSBuild 都把两个模块一起编译。mcpp 偏离这一点的理由是故障隔离，而且 mcpp 本来就扫描源码，判断是否需要 compat 没有额外成本。
- **自审**：
  - 构建程序（`build.mcpp`）和 `mcpp test` 的扫描都要覆盖到。
  - 只 import std 的项目，第一次用到 compat 时只补编 compat，std 不重建。这一点要有测试。

## 5. 模块源语言判定与验收

- `.ixx → -x c++-module` 的判断有两处：`std` 构建命令写在 `#if defined(_WIN32)` 里，compat 构建命令（#769）按扩展名判断。抽出 `module_source_language_flags(path)` 两处共用，去掉对宿主的依赖。
- **验收**：#768 的示例程序在普通环境中输出 `compat=3`（run `37291652508`），#769 得到实测确认。把它加入 Windows e2e，**普通环境和开发者环境各跑一次**（开发者环境依赖 D8），并更新 SPEC-009 §6.2。

---

## 第二部分：COFF 读取器

## 6. D3：按类别处理匿名对象

**实测头部**（run `37290831503`）：

| 编译选项 | 前 4 字节 | Version | ClassID（偏移 12） |
| --- | --- | --- | --- |
| 无 | `64 86 …` | — | — |
| `/bigobj` | `00 00 ff ff` | 2 | `c7a1bad1 eeba a94b af20 faf66aa4dcb8`（LLVM 的 `BigObjMagic`） |
| `/GL` | `00 00 ff ff` | 1 | `38feb30c a5d9 ab4d ac9b d6b6222653c2`（LLVM 的 `ClGlObjMagic`） |
| `/GL /bigobj` | 与 `/GL` 相同 | 1 | 与 `/GL` 相同 |

另外：
- `dumpbin /symbols` 对 `/GL` 对象只给出 `ANONYMOUS OBJECT`。
- 短导入对象是 Version 0（来自 PE 规范，没有实测）。
- mcpp 今天把 `/GL` 对象报成 “this is a /bigobj object”（实测）。

**设计：**
- 判别：`Sig1 = 0 && Sig2 = 0xFFFF` 时，先看 Version：0 是导入对象；≥ 1 时再看 ClassID：bigobj / `/GL` / 未知。
- `/bigobj`：实现读取（56 字节的头、32 位的节数和符号数、20 字节的 `IMAGE_SYMBOL_EX`），与 CMake 的 `bindexplib.cxx` 能力对齐。
- `/GL`、未知类别、导入对象：E3，各自给出准确的诊断。`/GL` 的诊断给出两条改法：`windows_auto_export = false` + `__declspec(dllexport)`（实测今天就能用，见 §8），或者去掉 `/GL`。

**自审（稳定性）：**
- 读取器的错误会**静默**产出错误的导出面，这是最难发现的一类问题。所以要求：
  - 测试数据使用 CI 中由 cl.exe 实际产出的 bigobj / `/GL` 对象字节（从 #772 的 job 中提取，作为 unit 测试的数据文件）。
  - 加一个 Windows e2e：对同一个库分别用 bigobj 和普通 COFF 构建，比较两者的 `.def`，必须一致。

---

## 第三部分：原生 MSVC LTO

## 7. D4 / D9：cl.exe 上兑现 `lto = true`

**实测的现状**：`[profile.release] lto = true` 时输出 `Finished release [optimized + lto]`，但 `build.ninja` 中没有 `/GL` 也没有 `/LTCG`。

**工具行为实测**（run `37290831503`）：

| 情形 | 结果 |
| --- | --- |
| `/GL` 对象，链接时不加 `/LTCG` | 成功，link.exe 提示 “restarting link with /LTCG” |
| `lib` 归档 `/GL` 对象，不加或加 `/LTCG` | 都成功，都没有输出 |
| `/LTCG /DEBUG` | 成功，没有警告 |
| `/LTCG /INCREMENTAL` | LNK4075 警告 |
| `/GL` 加 `/Zi /FS` | 成功 |
| `/GL` DLL + `__declspec(dllexport)` | 导出正确 |
| `import std`：std 与消费者的 `/GL` 取三种组合 | 都能链接并运行 |

**设计：**
- 项目 TU 编译加 `/GL`；exe / DLL 链接加 `/LTCG`；`lib` 加 `/LTCG`（与 CMake 的 `ARCHIVE_CREATE_IPO` 一致）。
- 不加 `/INCREMENTAL:NO`（实测不需要）。
- std 模块的对象不加 `/GL`（实测三种组合都可以），std 缓存因此不区分 LTO。
- D9：`profile_descriptor` 只在 LTO 实际生效时显示 `+ lto`；降级时（§8）显示 `+ lto (partial)`，详情在 W 报告中。
- 缓存：`lto` 已经在 profile 指纹中，cl 版本已经在键中（#746）。

**自审（兼容性）**：已经写了 `lto = true` 的 MSVC 项目，链接会变慢、产物会改变。这是用户声明的意图，release note 要写明。

## 8. D5：与 `windows_auto_export` 的交互

**降级的单位是包，不是目标（修订 4 的更正）：**
- 发现边读取的是 `lu.objects`。对于依赖拥有的 DLL，它包含拥有者包的对象，以及放入该镜像的静态依赖包的对象（`staticsByImagePackage`，#646 F1）。根包的共享库同理。
- mcpp 中一个源文件只编译成一个对象。所以降级必须作用于 “对象进入某个开启自动导出的 DLL 的那些包”。这些包的对象如果也链接进 exe，exe 中的这部分代码同样不做 LTO，是可接受的代价，W 报告中要写出来。

**实测支撑**（run `37290831503`）：DLL 自己的对象是普通 COFF，加一个 `/GL` 静态库（`lib /LTCG`），再加 `.def`，`link /DLL /LTCG`：导出正确。不加 `/LTCG` 时 link 自动重启，同样正确。

**`windowsAutoExport` 改为 `std::optional<bool>`**：
- 两个解析器（`toml.cppm`、`xpkg.cppm`）只在显式书写时赋值；读取时 `value_or(true)`。
- `pack` 的清单生成（`manifest_emit`）必须保留 “没写” 与 “显式 `true`” 的区别。

**规则**（cl.exe 行、LTO 生效、PE 共享库；根包、成员、依赖都适用）：

| 情形 | 级别 | 行为 |
| --- | --- | --- |
| 显式 `true` | E1 | 拒绝；改法：写 `false` 并标注导出，或者关闭 LTO |
| 用户 flag 中有 `/GL`，并且该包的对象进入开启自动导出的 DLL | E1 | 拒绝；改法：写 `false`，或者去掉 `/GL` |
| 没写 | W | 上述包的对象不加 `/GL`；DLL 仍加 `/LTCG`；每次运行一条汇总，列出受影响的 DLL 和包，以及两条改法 |
| 显式 `false` | — | 完整的 `/GL` + `/LTCG`。实测：显式 `false` + `__declspec(dllexport)` + `/GL` 在今天就能构建，`dumpbin /exports` 正确 |

不适用：LLVM LTO（#763）、clang 的 MSVC-ABI 行、exe、静态库、ELF、Mach-O、MinGW。

**为什么默认值不改**：
- 改成全局 `false`，依赖自动导出的现有 DLL 会得到空的导入库。
- 改成 “仅在 LTO 下为 `false`”，同一份 manifest 在 debug 和 release 下的 DLL 接口不同。

## 9. D6：`pack` 产出的静态库不携带 LTO 中间表示

- 依据：MSVC 的 `/GL` 文档；发行版不在静态库中分发 LTO 中间表示（例如 Fedora 的 LTO 方针）。GCC 默认的 slim LTO 对象，换一个编译器版本就无法使用。
- 设计：`mcpp pack` 产出静态库时，对这些对象不启用 LTO（cl.exe 不加 `/GL`，gcc / clang 不加 `-flto`），报告一条 W。exe 和 DLL 不受影响。
- 自审：今天 gcc / clang 的 `pack` 遇到 `lto = true` 时会产出带中间表示的 `.a`（`src/pack` 中没有 LTO 处理），所以这对它们也是一项行为变化。实现时先核实 `pack` 实际使用的 profile。

---

## 第四部分：共享库的链接配置（#771）

## 10. D7：依赖拥有的共享库 = 图级 flag + 自己闭包的 flag

### 10.1 判定

真实缺陷，不是使用问题。用户用的是有文档的 `build.mcpp` API；期望的行为就是工作空间设计 §15 对成员程序的承诺。

### 10.2 复现

- **Windows**（run `37291652508`，cl.exe 行，`link_lib("ws2_32")`）：
  - 成员 DLL 的链接行缺少 `ws2_32.lib`，却带着**另一个成员**的 `/IGNORE:4099`。
  - 结果 `LNK2019 __imp_WSAGetLastError`。`-p player` 同样失败。
  - 同一个成员的 exe 链接成功。
- **Linux**（本地）：成员的 `.so` 缺少自己的 `build.mcpp` flag（`-lm`、标记），却带着别的成员的标记。
- **非工作空间对照**：依赖的 `.so` 走汇集行，自己的 flag 不缺，但还带着根包和兄弟依赖的 flag。

### 10.3 成因

- 依赖拥有的共享库链接单元（`plan.cppm` 的 `sharedDepTargets` 循环）没有设置 `linkGroup`，所以用的是计划的全局链接行。
- 在工作空间中，成员 `build.mcpp` 的输出不进入全局行，而成员 manifest 的 `ldflags` 会被汇集进全局行。

### 10.4 设计

1. **把图级 flag 单独记录。** 新增 `graphLdflags`，在写入 `state.m` 的同时记录：
   - ABI 渲染的 `-pthread` / `-fexceptions`：docs/22 的契约要求它们到达链接，一定是图级。
   - profile 的 `ldflags`（D7a，建议算图级）：链接选项多是整件产物的属性（加固、链接器选择、sanitizer 运行库），而且这样保持非工作空间的现状。

   根包自己的 `[build] ldflags` 和根包 `build.mcpp` 的输出是根包**私有**的，不进入 `graphLdflags`。
2. **一个 helper**：`link_group_for(closure, productDir, linkOnly)`：
   - `ldflags = graphLdflags + 闭包内各包的 linkUsage.ldflags`（拥有者在前，其余按发现顺序）。
   - `derive_runtime(闭包)`。
   - 闭包：从拥有者出发，沿依赖边走，跳过 `artifacts` 边和 `buildTimeOnly` 包。
3. **三处共用这个 helper**：
   - 依赖拥有的共享库（所有计划；同一个包的多个共享库目标共用一个组）。
   - 成员程序：今天从 `packages[0].linkUsage.ldflags` 起步，改为从 `graphLdflags` 起步。这样带 `[package]` 的工作空间根，其私有 flag 也不再泄漏给成员。
   - artifact 程序。
4. **刷新链接快照**：构建程序运行后，对**所有**依赖重新计算 `linkUsage.ldflags`（今天只在工作空间中做），搜索路径按声明它的包解析。
5. 根包自己的 exe 和共享库仍然使用全局行（汇集），不变。

### 10.5 对照数据（来自 #773 的实验构建，Linux，非工作空间）

这些数据用来定出 §10.4 的边界：

| 依赖 `.so` 的链接行 | main | “只取闭包，不取根” 的实现 |
| --- | --- | --- |
| 根包私有标记 | 有（泄漏） | 无 |
| profile 的 `-z relro,-z now` | 有 | 无 → 所以需要 D7a |
| ABI 的 `-pthread` | 有 | 无 → 违反 docs/22，所以需要 `graphLdflags` |
| 根包 `-L<dir>`，依赖只写 `-lfoo` | 构建成功 | `ld: cannot find -lfoo` → 这就是 D7b 的兼容性变化 |

### 10.6 D7b：根包私有 flag 不再进入依赖 DLL

- **接受这项变化**：
  - SPEC-004 §9.6 禁止需求从消费者流向依赖。
  - 而且泄漏本身会**静默**产出错误的产物：根包的 `-Wl,--version-script`、`/DEF:`、`/SUBSYSTEM`、`/ENTRY` 进入依赖 DLL 后，导出面或入口会是错的。一次带提示的链接失败，好过一个静默错误的产物。
- **N**（只在一个版本内）：根包私有 `ldflags` 中有搜索路径或库（`-L`、`-l`、`/LIBPATH:`、`*.lib`），并且计划中有依赖拥有的共享库时，给出一条 note，点名这些 flag 和受影响的共享库，说明依赖应当自己声明（`[build] ldflags` 或 `mcpp::link_search` / `link_lib`）。
- release note 写明迁移方法。

### 10.7 测试

- unit：
  - 依赖 DLL 的组包含：拥有者和闭包的 flag、`graphLdflags`（ABI、profile）。
  - 不包含：根包的私有 flag、兄弟成员或兄弟依赖的 flag。
  - 工作空间和普通计划都测，ELF 和 PE 都测，一个包有两个共享库目标的情形也测。
- e2e：
  - 在产物上断言：ELF 用 `readelf -d` 读 RUNPATH 标记，Mach-O 用 `otool -l`，Windows 用 `ws2_32` 做行为测试（修复前 LNK2019）。
  - 全量构建和 `-p` 各跑一次。
  - N 只在出现搜索路径或库时给出。

---

## 第五部分：Windows 参数引号

## 11. D8：以反斜杠结尾的参数

- **现象**（run `37291652508`）：在 VS 开发者环境中（`WindowsSdkDir=…\10\`），llvm + MSVC 行的 std 模块预编译失败（`no such file or directory: 'Files\Microsoft'` 等）。同一个 job 在普通环境中，两个项目都成功。
- **成因**：
  - SDK 根原样带着末尾的 `\`，渲染成 `-Xmicrosoft-windows-sdk-root "…\10\"`。
  - `quote_windows`（`modules/platform/src/shell.cppm`）只转义 `"`。按 Windows 的 argv 规则，`\"` 是字面引号，这个参数的引号不会闭合。
  - `CreateProcess` 的命令行（`process.cppm`）也用这个函数，所以任何以 `\` 结尾的参数都会触发。
- **判定**：真实缺陷，影响面大：在开发者命令行中，llvm 行的 `import std` 完全不可用。现有 Windows CI 不在开发者环境中运行 llvm 行，所以没有发现。
- **设计**：
  1. `quote_windows` 实现标准的 argv 引号算法：一串反斜杠后面跟 `"` 或位于末尾时，把它们加倍。只有今天本来就是坏的参数，输出才会改变。
  2. `WindowsSdkDir` 读入时去掉末尾分隔符，这样两种环境的 SDK 身份和缓存键一致，可以共享缓存。
  3. 测试：unit 测试做往返校验（按 CommandLineToArgvW 规则解回，必须等于原参数），覆盖末尾 `\`、`\"`、`\\\"`、空格；Windows e2e 显式设置带末尾 `\` 的 `WindowsSdkDir`，构建 `import std` 和 `import std.compat`。

---

## 第六部分：工作空间根与根包的复用（O7）

## 12. D10：profile、`[workspace.*]` 与根包

### 12.1 实测（Linux；本地 2026.10.5.1 与 CI run `37297856040` 中从源码构建的 main 结果一致）

每个成员的 `[build]` 带自己的标记；`[profile.release]` 设置 `opt = 3`，并在 cxxflags 和 ldflags 中带标记；用 `--release` 构建，检查每条 ninja 边。

| 场景 | 命令 | 观察 |
| --- | --- | --- |
| W1 虚拟根，`[profile.release]` 写在根上，成员没有 profile | `build`、`-p app` | **根上的 profile 完全没有生效**：仍是 `-O2`，没有任何 profile 标记，也**没有任何诊断** |
| W2 虚拟根有 profile，成员 app 也有自己的 | `build` | 分成两个图：app 的图用 app 的 profile（`-O3`），lib 单独一个图（`-O2`）；**lib 被编译两遍**；根上的 profile 依然没有生效 |
| W2 | `-p lib` | `-O2`，没有 profile |
| W3 带 `[package]` 的根：同一个文件里有 `[package] app` + `[workspace]` + `[profile.release]` + 自己的 `[build]` | `build`（构建根包） | 根文件的 profile 生效（`-O3`），lib 也是 `-O3`；**app 没有得到 `[workspace.build]`**，lib 得到了 |
| W3 | `--workspace` | 分成两个图：app 的图 `-O3`，lib 的图 `-O2`，**同一个命令下 lib 被编译成两种样子** |
| W3 | `-p lib` | `-O2`：根文件的 profile 对成员不生效 |
| 各场景 | — | lib 的 DLL 带着 app 的私有 `ldflags`，属于 #771 同类的泄漏，由 D7 处理 |

### 12.2 成因

- 工作空间计划的根是一个合成的虚拟根（`project.cppm` 的 `virtual_workspace_root`），它的 `profiles = first.profiles`，也就是**取第一个被选中成员的 profile**。工作空间根自己的 `[profile]` 不在其中。
- profile 不在根位置继承的范围内：`inherit_workspace_root_position` 只继承 `[toolchain]`、`[target.*]` 和 `[indices]`。
- profile 是 `root_position_key` 的一部分，所以 profile 不同的成员会被分进不同的图。
- 根包作为成员 `"."` 时，`load_member_manifest(".")` 直接返回工作空间清单本身，**不执行** `inherit_workspace_package` / `inherit_workspace_build`。直接构建根包时也不执行。两种路径的结果一致，但都没有继承。

### 12.3 判定

- W1 违反 `diag.cppm` 的不变式：用户写的配置被静默忽略。这是缺陷。
- W3 中 “同一个文件里的 `[profile]` 是否生效取决于选择了哪个成员”，`--workspace` 下同一个库编译成两种样子，与工作空间设计 §5.4 的目标（成员共享配置时构建为一个图）相违背。
- 行业对照：Cargo 的 profile 只在工作空间根上声明，成员的 profile 被忽略并给出警告；根包本身就是工作空间成员。

### 12.4 设计

**D10a：`[profile.*]` 加入根位置键。**
- `inherit_workspace_root_position` 增加 profile：成员按 profile **名**继承工作空间根上的 `[profile.<n>]`；成员自己声明的同名 profile 整体优先。这与 SPEC-004 §9.10 对 `[toolchain]` 等键的规则一致；比 Cargo（忽略成员的 profile）更保守，现有成员的 profile 继续有效。
- 由此：
  - W1：根上的 profile 生效。
  - W3 `--workspace`：lib 继承根文件的 profile，与 app 的根位置键相同，合并为一个图，lib 只编译一次。
  - W3 `-p lib`：同样 `-O3`。
- 与 D7a 的关系：图级 flag 中的 profile `ldflags` 取自继承之后的 profile。

**D10b：根包与其他成员一样继承 `[workspace.package]` / `[workspace.build]`。**
- 根包是成员 `"."`（`manifest.cpp` 中的注释：“A rooted workspace's own package is a member like any other”）。`load_member_manifest(".")` 和直接构建根包的加载路径都调用同一个继承函数，恰好一次；不调用根位置继承，因为根包与工作空间就是同一个文件。
- 这样根包在每种选择下得到相同的命令，也满足 SPEC-004 §9 第 1 条 “恰好一次” 的要求。第 1 条列出的位置要补上 “带 `[package]` 的工作空间根自己”。
- 向量的顺序遵循已有规则：`[workspace.build]` 在前，根包自己的 `[build]` 在后。

**D10c：根清单中键的归属，写成一张表**（进入 SPEC-004 §9 和 docs/07）：

| 键 | 归属 | 对根包的作用 | 对其他成员的作用 |
| --- | --- | --- | --- |
| `[workspace]`、`[workspace.dependencies]` | 工作空间 | 通过 `x.workspace = true` 显式引用 | 同左 |
| `[workspace.package]`、`[workspace.build]` | 工作空间 | 继承（D10b） | 继承 |
| `[toolchain]`、`[target.*]`、`[indices]`、`[profile.*]` | 根位置 | 直接生效 | 成员作为构建的根时继承；成员自己声明的优先（D10a） |
| `[package]`、`[build]`、`[dependencies]`、`[targets]`、`[features]`、`[resources]`、`[test]` | 根包 | 只作用于根包 | 不作用于成员 |

- 根包的 `[build] ldflags` 是根包私有的。在 D7 中，它们进入根包自己镜像的链接组，不进入 `graphLdflags`，也不进入成员或依赖的共享库。这一点同样适用于带 `[package]` 的工作空间。

### 12.5 测试

- 把 #772 中的 `ws_profile.sh` 场景改写成 e2e，断言修复后的预期：
  - W1：根上的 profile 生效。
  - W2：成员的 profile 优先。
  - W3：`--workspace` 下只有一个图，lib 只编译一次；`-p lib` 下 `-O3`；根包得到 `[workspace.build]`。
  - 各场景中，lib 的 DLL 不带 app 的私有 flag。
- 断言每个包在 `build`、`--workspace`、`-p` 下的编译命令完全一致（工作空间设计 §5.3 的 “同一个单元在每种选择中都用同一条命令编译”）。

## 第七部分：Windows 代码页（O6）

## 13. D11：`Glob.EscapedSpellingIsUtf8WhateverTheName`

### 13.1 实测（run `37297856040`，`windows-latest`）

| 项 | 结果 |
| --- | --- |
| runner 的 ANSI 代码页 | 1252 |
| CI 上用当前 main 运行 `mcpp test test_modgraph` | 该测试**通过** |
| 同一段转换，在不声明代码页的程序中（`GetACP() = 1252`） | `std::filesystem::path("caf\xE9")` 成功 |
| 同一段转换，在 `windows_code_page = "utf-8"` 的程序中（`GetACP() = 65001`） | **抛出** `No mapping for the Unicode character exists in the target multi-byte code page.`，与报告中的异常相同 |
| `MultiByteToWideChar(…, MB_ERR_INVALID_CHARS, "caf\xE9")` | cp 1252 成功；cp 936（GBK）失败；cp 65001 失败 |

### 13.2 成因

- 测试的最后一行在**所有平台**上执行：`escaped_spelling(std::filesystem::path("caf\xE9"))`。
  - 在 Windows 上，`path` 用进程的 ANSI 代码页把窄字符串转为 UTF-16。
  - `\xE9` 只在 1252 这类单字节代码页中可以转换；在 936（中文系统）和 65001（UTF-8）中都会抛异常。
  - 这一行想测的是 POSIX 的情形（名字的字节不是 UTF-8），只应该放在 POSIX 分支。
- **为什么 CI 一直发现不了**：mcpp.exe 声明了 UTF-8 代码页（#693，`docs/res/mcpp.rc`），而 `mcpp test` 构建的测试程序运行在系统代码页中（CI 上是 1252；`windows_code_page` 只能写在 `[targets]` 上，发现得到的测试没有地方写）。所以：
  - 测试没有在产品的运行环境中执行。
  - 测试结果取决于运行它的机器的区域设置。
- **与之前的修复的关系**：#231、#517、#698 修的是产品代码中的窄化（UTF-16 → 窄字符串）站点，并加了 `check_narrow_conversions.sh` 这道关卡。本问题出在 #698 加入的测试本身，以及测试程序的代码页，不在那些修复的覆盖范围内。产品本身（UTF-8 进程）不会构造这样的路径，没有发现产品缺陷。

### 13.3 设计

1. **修正测试**：Windows 分支只用 UTF-16 输入（孤立代理项、`é`）；窄字符串的非 UTF-8 字节只放在 POSIX 分支。
2. **测试程序可以声明代码页**：
   - 新增 `[test] windows_code_page = "utf-8" | "legacy"`，作用于所有发现得到的测试程序。语义与 `[targets.<n>].windows_code_page` 相同（SPEC-004 §5.3 的平台前缀规约，非 PE 时不产生任何东西）。
   - 默认仍是 `legacy`（程序的编码由程序自己决定，不改变现有项目）。
   - mcpp 自己的 `mcpp.toml` 写 `"utf-8"`，使单元测试与 mcpp.exe 运行在同一个代码页中，结果不再取决于机器的区域设置。
3. **防止再次发生**：在 Windows CI 中，单元测试本来就在 UTF-8 中运行（第 2 条），这就是对 65001 的覆盖。936 等多字节代码页由第 1 条的测试修正覆盖；不为它单独增加 runner。
4. **附带审计（低优先级，可以不进入本次发布）**：
   - 在 UTF-8 进程中，用**非 UTF-8 的外部文本**构造 `path`（例如工具以 OEM 或 ANSI 代码页输出的路径）同样会抛异常。
   - 建议参照 `check_narrow_conversions.sh` 审计这类 “加宽” 站点，并按需要增加关卡。

## 14. 发布方案：一个发布 PR

每个提交都可以单独构建、单独测试、单独 revert；前面的提交不依赖后面的决策。

| # | 提交 | 内容 | 新的诊断 |
| --- | --- | --- | --- |
| 1 | Windows 参数引号 | D8 + unit / e2e | — |
| 2 | 链接组 | D7 / D7a / D7b：`graphLdflags`、`link_group_for` 三处共用、快照刷新 | N |
| 3 | COFF 读取器 | D3：bigobj 读取、`/GL` 和未知类别报错，测试数据用 CI 中实际产出的对象 | E3 |
| 4 | 构建输出 | D9：`+ lto` 只在生效时显示 | — |
| 5 | SPEC 修订 | SPEC-004：`lto` 的语义（各行如何兑现，不能兑现时 W）、`windows_auto_export` 三态、链接 flag 的作用域（图级 / 私有 / 闭包）；SPEC-003：新增的拒绝代码 | — |
| 6 | 原生 MSVC LTO | D4、D5（E1 / W，按包降级）、D6（所有工具链） | E1、W |
| 7 | 标准库模块 | D1、D2（按需编译 + E1）、§5 的语言判定统一 | E1 |
| 8 | 工作空间与根包 | D10a / D10b / D10c：profile 成为根位置键、根包继承、键归属表进入 SPEC-004 §9 和 docs/07；`ws_profile` e2e | — |
| 9 | 代码页测试 | D11：修正测试、`[test] windows_code_page`、mcpp 自己的测试使用 UTF-8 | — |
| 10 | 验收与文档 | Windows `import std.compat` e2e（两种环境）、SPEC-009 §6.2、中英文 docs、CHANGELOG | — |

**CI 覆盖：**

| 内容 | 覆盖它的 job |
| --- | --- |
| D8 | Windows e2e，在一个作业中设置 `WindowsSdkDir`，或者新增一个开发者环境的作业 |
| D7 | Linux / macOS / Windows e2e（在产物上断言） |
| D3、D4、D5 | Windows e2e，cl.exe 行（`ci-windows-e2e`） |
| D2、§5 | Windows e2e，llvm + MSVC 行 |
| D10 | Linux / macOS / Windows e2e（在边和产物上断言） |
| D11 | Windows 单元测试（UTF-8 进程中运行） |

#772 中的探测脚本保留到发布 PR 合入，期间可以随时复测。

## 15. 兼容性与风险

| 变化 | 影响面 | 处理 |
| --- | --- | --- |
| 引号修复 | 只有今天本来就是坏的参数 | 无 |
| SDK 根规范化 | 开发者环境中的 std 缓存重建一次 | release note |
| 根包私有 flag 不再进入依赖 DLL | 依赖这一泄漏的项目会链接失败 | N（一个版本）+ release note |
| 依赖 DLL 得到自己 `build.mcpp` 的 flag | 修复 | — |
| cl.exe 上 LTO 真正生效 | 写了 `lto = true` 的 MSVC 项目 | release note |
| 显式 `true` 或手写 `/GL` 与自动导出冲突 | 很少 | E1 |
| `pack` 的静态库不带 LTO 中间表示 | 开了 LTO 并打包静态库的项目 | W |
| import 了 compat 但标准库不提供 | 今天就失败，只是诊断变好 | E1 |
| bigobj 可以参与发现 | 以前失败的项目 | 无 |
| 工作空间根上的 `[profile]` 开始生效 | 在虚拟根上写了 profile 的工作空间（今天被忽略） | release note |
| 带 `[package]` 的工作空间根，profile 对所有成员生效、合并为一个图 | 这类工作空间的成员 | release note（结果是少编译一遍） |
| 根包继承 `[workspace.build]` / `[workspace.package]` | 带 `[package]` 的工作空间的根包 | release note |
| `[test] windows_code_page` | 新增的键，默认不变 | 无 |

**风险与对策：**
- COFF 读取器出错会静默得到错误的导出面 → 用真实对象字节做测试，再用 bigobj 与 COFF 的 `.def` 对比。
- 链接组改变了所有依赖共享库的链接行 → unit 测试做正向和反向断言，e2e 在产物上断言。
- 一个发布 PR 体量大 → 按提交可以 bisect，并跑完整 CI。

## 16. 开放问题

- **已关闭**：
  - **O1**：没有 clang-cl 行。
  - **O2**：std 模块对象不加 `/GL`。
  - **O5**：所有计划使用同一规则（§10）。
  - **O6**：查明原因并纳入方案（§13，D11）。可以登记一个 issue 追踪，由发布 PR 关闭。
  - **O7**：查明原因并扩展为 §12（D10a–D10c）。
- **仍然开放**：
  - **O3**：LLVM bitcode 静态库的分发，由 D6 统一处理；如果需要例外，再单独讨论。
  - **O4**：W 报告每次运行汇总一条（已经纳入设计，请确认）。


## 17. 发布说明草稿

- **修复**：
  - 在 Visual Studio 开发者命令行中使用 llvm 工具链（MSVC ABI）时，`import std` 编译失败。原因是以 `\` 结尾的参数被错误引用。
  - 依赖和工作空间成员的共享库，会用上它自己的包（包括 `build.mcpp`）及其依赖声明的链接库和选项（#771）。ABI 和 profile 的链接选项仍然到达所有镜像。
  - `import std.compat` 在所选标准库不提供它时，会在构建开始前给出明确的错误。
  - MSVC 的 `/bigobj` 对象可以参与自动导出发现；`/GL` 对象得到准确的诊断。
  - 工作空间根上写的 `[profile.*]` 现在会生效（以前被静默忽略）；成员自己声明的同名 profile 优先。在带 `[package]` 的工作空间中，根文件的 profile 对所有成员一致生效，不再因选择不同而把同一个库编译成两种样子。
  - mcpp 的单元测试在 Windows 上与 mcpp.exe 运行在同一个代码页（UTF-8）中，不再依赖机器的区域设置。
- **变化**：
  - 带 `[package]` 的工作空间，根包与其他成员一样继承 `[workspace.package]` 和 `[workspace.build]`。
  - 新增 `[test] windows_code_page`。
  - 根包自己的 `[build] ldflags` 不再进入依赖的共享库。依赖需要的搜索路径和库，应当由依赖自己声明。
  - cl.exe 上的 `lto = true` 现在会真正启用 LTO（`/GL` + `/LTCG`）。
  - 没写 `windows_auto_export` 的 DLL 在 LTO 下降级（报告）；显式 `true` 或手写 `/GL` 与自动导出同时出现时报错。
  - `mcpp pack` 产出的静态库不携带 LTO 中间表示。
  - `import std.compat` 只在被使用时编译。
- **构建输出**：`+ lto` 只在 LTO 实际生效时显示。
