# C++20 作为一等 `standard` 档位:`import std` 全平台可用性设计

> 日期:2026-07-31 · 基线:`7af26ba`(2026.7.30.3 已发布)· 目标版本:2026.7.31.1
> 输入:用户需求——"给 mcpp 增加 C++20 的支持,C++23 依然作为默认;核心是**目前支持的平台和工具链都可用**"
> 外部依据:[gcc PR106852](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=106852)、
> [microsoft/STL#3945](https://github.com/microsoft/STL/issues/3945)(已 fixed,PR #3977,2023-08-31 合入)、
> [libc++ Modules](https://libcxx.llvm.org/Modules.html)
> 前置文档:`2026-06-01-cpp-standard-first-class-design.md`(standard 一等化)、
> `2026-07-14-std-features-experimental-gate-design.md`(c++fly / `cppfly.cppm` 数据表范式)
> 本文所有工具链结论均由**本机实机探针**得出(§2.2),不是文档推断。
> **review 定案(2026-07-31)**:MSVC 侧取 20(带版本阈值,§2.3);不加 `mcpp new --standard`、
> 模板保持 C++23(§4.6);不引入"是否显式声明"位(§9-Q3)。

## 0. 一句话结论

**在 `normalize_cpp_standard` 的白名单里增加 `c++20` / `c++2a` / `gnu++20`(level = 20),
默认值仍是 `c++23`;并把 `import std` 的可用性从今天的布尔 `Toolchain::hasImportStd`
升级为"最低档位" `Toolchain::importStdMinLevel`,由各 provider 自己填。**

```toml
[package]
standard = "c++20"     # 新增:仍然可以 import std;(GCC≥15 / Clang≥17+libc++ 实测通过)
```

机制上**没有新管线**:标准值已经是图全局单值,已经进指纹、进 std BMI 身份、进依赖构建
缓存键;`-std=` 拼写已经走 `dialect.cppm::std_flag_for`(MSVC 侧 `level <= 20 → /std:c++20`
这一行**从写下那天起就是不可达代码**,本设计第一次让它可达)。真正要新增的只有两样:

1. 解析层三个新的别名行(白名单 + level 数值);
2. **能力门**:`import std` 不是所有工具链在所有档位都能提供 —— 今天的门只问
   "有没有 std 模块源",C++20 落地后必须问"**这个工具链在这个档位上能不能建 std 模块**"
   (三家现代实现的答案都是"能",唯一的例外是 STL#3977 解禁之前的老 MSVC,见 §2.3),
   否则那批用户拿到的是 `std.ixx` 内部错误而不是可行动诊断。

## 1. 为什么是 C++20,以及为什么默认仍是 C++23

### 1.1 语言学依据(用户输入的核心论点)

`import std;` 是 C++23 的库特性,但**命名模块本身是 C++20 语言特性**。三家实现都已经
(或已同意)在 C++20 模式下提供 `std` 模块:

| 实现 | 依据 | 状态 |
|---|---|---|
| libstdc++ / GCC | PR106852(std 模块实现);`bits/std.cc` **无任何 `__cplusplus` 版本守卫**(实机核对) | C++20 下可编译(§2.2 实测) |
| libc++ / Clang | [libc++ Modules 文档](https://libcxx.llvm.org/Modules.html):std 模块支持 C++20/23/26,要求 Clang 17+ | C++20 下可编译(§2.2 实测) |
| MSVC STL | STL#3945 "Supporting `import std;` in C++20" → **PR #3977 已合入**(2023-08-31),原话:"blocking 是纯策略选择,没有技术原因" | STL 侧已解禁,**但官方文档仍写 `/std:c++latest`**(§2.3) |

结论:C++20 + `import std` 在 mcpp 支持的 GNU 侧工具链上是**已验证可用**的组合,
不是理论推演。

### 1.2 为什么地板是 C++20,不再往下

命名模块 = C++20 特性。mcpp 的整个构建模型(模块图、BMI、dyndep、`import std`)在
C++17 及以下**不存在**。所以 `c++20` 是这个白名单的天然下界,`c++17` 不在本设计范围
(§8 非目标)。

### 1.3 为什么默认仍是 C++23

- `mcpp new` 生成的模板用 `std::println`(`src/scaffold/create.cppm:258-261`),这是 C++23 库特性;
- 生态(mcpp-index)包描述符默认 `standard = "c++23"`(`src/manifest/xpkg.cppm:1070`);
- C++23 是"import std 一定在标准里"的最低档位,C++20 下的 std 模块是**实现扩展**;
- 改默认值 = 全生态指纹翻车 + 一次全量重编。

**C++20 是给"必须跟老约束共处"的工程(嵌入式 SDK、公司内规、第三方 API 只到 C++20)
的逃生舱,不是新推荐值。**

## 2. 现状盘点与实机证据

### 2.1 触点清单(改动面就是这张表)

| 触点 | 位置 | C++20 落地后 |
|---|---|---|
| 白名单 + level 数值化 | `src/manifest/types.cppm:635-681` `normalize_cpp_standard` | **改**:加 `c++20`/`c++2a`/`gnu++20`(level 20) |
| 错误文案 | `src/manifest/types.cppm:679-681` | **改**:列出新值 |
| canonical 回写 | `src/manifest/toml.cppm:211-214`、`src/manifest/xpkg.cppm:1720-1725` | 不改(已把 `package.standard` 归一为 canonical → 指纹天然免别名抖动) |
| 各家 `-std=` 拼写 | `src/toolchain/dialect.cppm:116-123` | 不改。`level <= 20 → /std:c++20` 这行**首次可达** |
| c++fly / c++latest 解析 | `src/toolchain/cppfly.cppm:140-200` | 不改(999/1000 档位与 20 无交集) |
| 图全局标准 | `src/build/plan.cppm:388-396`、`src/build/prepare.cppm:3665-3690` | 不改(单值来自根 manifest) |
| 全 TU 注入 | `src/build/flags.cppm:383-390` | 不改 |
| p1689 扫描 | `src/modgraph/p1689.cppm:343` | 不改(接收 `stdFlagAndDialect`;`-std=c++23` 只是兜底);C++20 下扫描实测正常 |
| std BMI 身份 | `src/toolchain/stdmod.cppm:114-138` `metadata_for` | 不改(`cpp_standard` + `std_flag` 已在身份里) |
| 依赖构建缓存键 | `cache_key::BuildAxes.cppStandard/cppStandardFlag` | 不改 |
| 指纹 | `src/build/prepare.cppm:3745` | 不改 |
| **import std 能力门** | `src/build/prepare.cppm:3735-3740` | **改**:布尔 → 档位比较(§4.2) |
| build.mcpp helper | `src/build/build_program.cppm:647` | **改(顺带缺陷)**:`"-std=" + canonical` 对 `c++fly`/`c++latest` 拼出非法旗标(§4.5) |
| 文档 | `docs/05-mcpp-toml.md:44-58`、`docs/zh/05-mcpp-toml.md:44-56`、两处能力表 | **改**(ZH 侧还缺 `c++fly` 条目) |

### 2.2 实机证据矩阵(2026-07-31,本机)

方法:**回放 mcpp 自己写下的真实 std 模块构建命令**(`~/.mcpp/build-cache/v1/std/*/std-module.json`
里的 `std_build_commands`),只把 `-std=c++23` 替换成 `-std=c++20`,再用同一套旗标编译
一个 `import std;` 的消费者 TU 并链接、运行。这样验证的是 mcpp 真正会发出的命令行
(含 `--sysroot` / `-B` / `-isystem` / hermetic link model),不是"裸 g++ 能不能过"。

| 工具链 | target triple | std 模块 @ c++20 | 消费者编译+链接 | 运行 |
|---|---|---|---|---|
| gcc 16.1.0 | `x86_64-linux-gnu` | ✅ | ✅ | ✅ `hi 1` |
| gcc 15.1.0 | `x86_64-linux-gnu` | ✅ | ✅ | ✅ `hi 1` |
| gcc 16.1.0 (musl) | `x86_64-linux-musl` | ✅ | ✅ | ✅(`-static` 本机跑通) |
| gcc 15.1.0 (musl) | `x86_64-linux-musl` | ✅ | ✅ | ✅(`-static` 本机跑通) |
| gcc 16.1.0 (mingw cross) | `x86_64-w64-mingw32` | ✅ | ✅ | ✅(`-static` + wine 跑通) |
| clang 22.1.8 + libc++ | `x86_64-unknown-linux-gnu` | ✅(含 `std.compat`) | ✅ | ✅ `hi 1` |
| MSVC(`cl 19.51.36252`,VS 18 / MSVC 14.51) | `x86_64-windows-msvc` | ✅ **Windows CI 实测**(PR#325,`177_cpp20_msvc.sh`) | ✅ | ✅ `cl20 3` |
| clang + MSVC STL | Windows | 未单独验证 → 保持 23(`tc.version` 是 clang 的,答不了 cl 的问题) | — | — |
| llvm 20.1.7(本机 payload) | — | 不适用:该 payload 只有 `bin/ lib/`,**无 `share/libc++/v1/std.cppm`** → `hasImportStd=false`,与档位无关 |

附带抓到的两条硬事实:

1. **跨档位 BMI 绝对不可复用**——用 C++23 建的 `std.gcm` 去喂 C++20 的 TU:

   ```
   std: error: language dialect differs 'C++20', expected 'C++23'
   std: error: failed to read compiled module: Bad file data
   ```

   这正好**反证了缓存设计是对的**:`cpp_standard` + `std_flag` 早已在 std 身份键和
   `BuildAxes` 里,c++20 与 c++23 天然落到不同目录,不需要为本特性动任何缓存代码。

2. **实验门旗标与档位强耦合**:回放 `c++fly` 那份缓存条目时,`-freflection` 直接报
   `'-freflection' only supported with '-std=c++26' or '-std=gnu++26'`。这不影响 c++20
   (c++fly 是独立值),但它明确了一条不变量:**任何"档位 × 方言旗标"的组合都必须同源计算**
   ——今天由 `cppfly::effective_dialect_flags` 单点保证,C++20 不得绕开它。

复现脚本(留档):见 §6 "验证与复现"。

### 2.3 MSVC:唯一未实机验证的一格(review 已定案取 20)

- STL 侧:#3945 已 fixed(PR #3977 合入,2023-08-31),即 `std.ixx` 在 C++20 模式下不再被 STL 主动拦;
- 但 Microsoft Learn 的 [import STL named module 教程](https://learn.microsoft.com/en-us/cpp/cpp/tutorial-import-stl-named-module)
  至今仍写 **`/std:c++latest` 是 `import std;` 的必要条件**,且未标注最低 VS 版本 —— 文档滞后于实现;
- mcpp 的 MSVC 支持是 detection-first 的 `msvc@system`(`src/toolchain/msvc.cppm`),
  编译器与 STL 版本由用户机器决定,mcpp 无法保证用户装的是解禁后的 STL。

**review 定案(2026-07-31):取 20**,与 GNU 侧一致 —— C++20 是用户主动压低档位的知情选择,
不为"可能存在的老 STL"给全体 MSVC 用户加一道拒绝。

但"取 20"不等于"无条件 20":PR #3977 之前的 STL(VS 2022 17.8 / `cl` 19.38 之前)确实会拦,
那些机器上 `import std;` + `/std:c++20` 会落到 `std.ixx` 内部报错。所以这一行写成
**带版本阈值的数据行**(与 `cppfly::kLatestStd` 的 `minMajor` 同范式):

| MSVC 版本 | `importStdMinLevel` |
|---|---|
| `cl` ≥ 19.38(VS 2022 17.8,STL#3977 首个可能的发布档) | **20** |
| 更老 | 23(落到可行动诊断,而不是 `std.ixx` 内部错误) |

这样做的额外好处:**门不会变成不可达代码**。若无条件取 20,则全部 provider 都是 20、
白名单下界也是 20,`level < importStdMinLevel` 永远为假 —— 又是一处"写下即不可达"的分支
(`dialect.cppm:119` 就这样躺了一年)。版本阈值让它既可达、又可被单测覆盖。

⚠️ 19.38 这个阈值来自 PR 合入时间推算,**必须由 Windows CI 的实测作业确认**(§5.3);
若实测显示更早/更晚,改的仍然是这一行数据。

## 3. 概念模型:两个正交轴

```
       语言档位轴 (level)                      能力轴 (per toolchain)
  20 ── 23 ── 26 ── latest(999) ── fly(1000)   hasImportStd + importStdMinLevel
   │     │                                            │
   └─────┴── 用户在 mcpp.toml 声明 ────────────────────┴── provider 探测时填
                          │
                          └──→ 门:needsStdModule && level < importStdMinLevel → 可行动错误
```

今天的门是**一维**的(`hasImportStd` 布尔),因为最低档位就是 23,永远满足。
放开 C++20 后门必须变成**二维**:现代工具链全部答 20(门恒真、零影响),
只有 STL#3977 之前的老 MSVC 会命中 23,拿到可行动诊断而不是 `std.ixx` 内部错误。

## 4. 设计

### 4.1 解析层:三个新白名单条目

`src/manifest/types.cppm::normalize_cpp_standard`:

```cpp
if (s == "c++20" || s == "c++2a") {
    out.canonical = "c++20"; out.flag = "-std=c++20"; out.level = 20; out.gnuDialect = false;
    return out;
}
if (s == "gnu++20" || s == "gnu++2a") {
    out.canonical = "gnu++20"; out.flag = "-std=gnu++20"; out.level = 20; out.gnuDialect = true;
    return out;
}
```

错误文案同步:`expected c++20, c++23, c++26, c++2c, gnu++20, gnu++23, gnu++26, c++latest, or c++fly`。

注意 `out.flag` 只是 GNU 侧静态兜底;真正下发的拼写永远来自
`cppfly::std_flag(tc, canonical, level)` → `dialect_for(tc)` → `std_flag_for`,
所以 MSVC 自动得到 `/std:c++20`(`dialect.cppm:119`,首次可达)。

### 4.2 能力层:`hasImportStd` 布尔 → 档位

`src/toolchain/model.cppm`,紧挨 `hasImportStd`:

```cpp
bool hasImportStd     = false;
// 该工具链能在多低的 -std= 档位上构建 std 模块。0 = 未知(按 hasImportStd 走旧语义)。
int  importStdMinLevel = 0;
```

**知识留在各 provider 里**(与 `hasImportStd` 同一处赋值,不新建中心表——中心表会
和 provider 的探测结果两处推导同一决策,这正是历史上反复踩的坑):

| provider | 赋值点 | 值 | 依据 |
|---|---|---|---|
| GCC | `src/toolchain/gcc.cppm:132-134` | **20** | §2.2 实测 gcc 15/16 × glibc/musl/mingw 全通过 |
| Clang + libc++ | `src/toolchain/clang.cppm:145-147` | **20** | §2.2 实测 clang 22.1.8;libc++ 文档声明 C++20 起支持 |
| Clang + MSVC STL | `src/toolchain/clang.cppm:154-156` | **23** | 用的是 MSVC 的 `std.ixx`(STL 的策略说了算),但此处 `tc.version` 是 **clang 的版本**,答不了 cl banner 的问题 ⇒ 保持严格 |
| MSVC | `src/toolchain/msvc.cppm:548-552` | **20**(`cl` ≥ 19.38)/ **23**(更老) | §2.3,review 定案取 20 + 版本阈值 |

MSVC 那行需要 major.minor 两段(`19.38`),而 `cppfly::compiler_major` 只取前导整数
(MSVC 一律得 19)。**不要**去改 `compiler_major` 的语义(它有别的消费者),在 `msvc.cppm`
里加一个本地的 `toolset_at_least(tc, 19, 38)` 即可 —— 版本比较的知识本来就属于该 provider。

门(`src/build/prepare.cppm:3735-3740`,原地扩写):

```cpp
if (needsStdModule && !tc->hasImportStd) { /* 原文案不变 */ }
if (needsStdModule && tc->importStdMinLevel > 0
    && m->cppStandard.level < tc->importStdMinLevel) {
    return std::unexpected(std::format(
        "source imports std but toolchain '{}' provides the std module only from {} up "
        "(project [package].standard = \"{}\"); raise the standard or drop `import std;`",
        tc->label(), level_name(tc->importStdMinLevel), m->package.standard));
}
```

`level_name(20|23|26)` 是本地小函数,别造第二张表。

### 4.3 图语义:根 manifest 单值,依赖侧的诚实说明

今天的事实(已核实,`plan.cppm:388` 是唯一读取点):**依赖包 manifest 里的
`[package].standard` 在构建时被完全忽略**,全图用根包的档位。这是 BMI 兼容性
强制的(§2.2 证据 1:跨档位 BMI 直接拒读,同图不可能存在两个档位)。

C++20 让这条一直存在的语义**第一次变得可见**:根设 c++20,而某个依赖用了
`std::print` / `std::expected`,报错会出现在依赖的 TU 里。

评估过的"依赖最低档位检查"方案及其陷阱:

> 想法:依赖声明 `standard = "c++23"` 而根解析出 c++20 ⇒ 提前报错。
> **陷阱:默认值与显式声明不可区分**——`xpkg.cppm:1070` 和 toml 解析都在缺省时
> 写死 `"c++23"`,所以这条检查会把**整个 mcpp-index 生态**判成"要求 C++23",
> 直接封死 c++20 的所有实用场景。

⇒ **v1 不做 floor 检查**。要做必须先加"是否显式声明"位(`std::optional` 或
`standardDeclared` bool),那是独立一步(§9-Q3)。v1 只做**诊断增强**:当编译失败发生在
依赖包的 TU 且根档位 < 23 时,在错误尾部追加一行提示"根工程 standard = c++20,
依赖 `<name>` 可能需要更高档位"。

### 4.4 缓存、指纹、跨档位隔离(零改动,附证明)

| 层 | 是否含标准 | 结论 |
|---|---|---|
| 指纹 → `target/<triple>-<fp>` | `fpi.cppStandard = m->package.standard`(**已归一为 canonical**,`toml.cppm:214`) | c++20 与 c++23 落不同产物目录 |
| std BMI 身份 | `metadata_for` 的 `cpp_standard` + `std_flag` + 完整命令串 | 天然分桶,且命中校验会拒绝错档位 |
| 依赖构建缓存 | `BuildAxes.cppStandard` + `cppStandardFlag` | 同上 |
| GCC/Clang 自身 | `language dialect differs` 硬拒(§2.2) | **最后一道保险**:即便键写错也不会静默错编 |

**本特性不新增任何缓存代码。** 这是设计能收敛到"三行白名单 + 一个门"的根本原因。

### 4.5 顺带缺陷:`build.mcpp` 的 `-std=` 拼写绕过了方言层

`src/build/build_program.cppm:647`:

```cpp
std::string std_flag = "-std=" + std::string(cppStandard.empty() ? "c++23" : cppStandard);
```

传入的是 `m->cppStandard.canonical`(`prepare.cppm:3606`)。当工程是 `standard = "c++fly"`
或 `"c++latest"` 时,这里拼出的是 **`-std=c++fly` / `-std=c++latest`** —— GNU 驱动直接
拒绝的非法拼写。这正是 `cppfly.cppm` 头注释里记的 §11-Q5 老坑,在 build.mcpp 路径上
从未修好(因为没人用 c++fly 跑过 build.mcpp)。

C++20 本身不触发这个 bug(`-std=c++20` 合法),但本设计要求**同一决策只有一处推导**:

```cpp
// host 工具链决定拼写(build.mcpp 用宿主编译器编译并运行)
std::string std_flag = mcpp::toolchain::cppfly::std_flag(hostTc, canonical, level);
```

⇒ 接口从 `std::string_view cppStandard` 改为传 `const CppStandardConfig&`(canonical + level)。
调用点两处:`prepare.cppm:3479`(依赖)、`prepare.cppm:3606`(根)。

### 4.6 scaffold:零改动(review 定案)

`mcpp new` 生成的 `main.cpp` 用 `std::println`(C++23)。**模板与 `mcpp new` 一律不动,
也不加 `--standard` 选项。**

定案理由:默认档位是 c++23,用户把它改成 c++20 这个动作本身就说明**知道自己在放弃哪些特性**
——C++20 支持的定位是"给特殊约束下的用户多一个选择",不是新的推荐路径。所有模板、脚手架、
文档示例继续以 C++23 为准。代价(改档位后模板里的 `std::println` 需要自己换成 `std::cout`)
已写进两份文档的 `c++20` 条目。

### 4.7 c++fly / c++latest 不受影响

`cppfly::std_flag` 只在 `level >= 999` 时重解析;`kLatestStd` 表的下界是 c++23。
C++20 与实验门的组合(`-freflection` 要求 c++26,§2.2 证据 2)天然不可达:
c++fly 自己解析档位,不接受外部档位。**不需要为 c++20 增加任何 c++fly 侧的守卫。**

## 5. 测试计划

### 5.1 单元测试

| 文件 | 用例 |
|---|---|
| `tests/unit/test_manifest.cpp` | `c++20`/`c++2a` → canonical `c++20`,level 20,`gnuDialect=false`;`gnu++20`/`gnu++2a` → `gnu++20`,level 20,`gnuDialect=true`;`c++19` 仍报错且文案含 `c++20` |
| `tests/unit/test_toolchain_dialect.cpp` | `std_flag_for(msvc, "c++20", 20) == "/std:c++20"`(**首次覆盖这条分支**);`std_flag_for(gnu, "gnu++20", 20) == "-std=gnu++20"` |
| `tests/unit/test_cppfly.cpp` | `cppfly::std_flag(gcc16, "c++20", 20) == "-std=c++20"`(证明低档位不被 latest 逻辑劫持) |
| `tests/unit/test_cache_key.cpp` | `cppStandardFlag = "-std=c++20"` 与 c++23 键不同 |
| `tests/unit/test_fingerprint.cpp` | 同上,指纹分桶 |
| 新增门测试 | `importStdMinLevel=23` 且 level=20 且 needsStdModule → 错误文案含 "provides the std module only from" |

### 5.2 e2e(编号接 175+)

| 脚本 | 内容 | 平台 |
|---|---|---|
| `175_cpp20_import_std.sh` | `standard = "c++20"` 的工程 + `import std;` + 自有模块:`mcpp build` 成功、`mcpp run` 输出正确;断言 `compile_commands.json` 里出现 `-std=c++20` 且**不出现** `-std=c++23` | Linux/macOS(GNU 侧) |
| `176_cpp20_std_bmi_isolation.sh` | 同一工程先 c++23 后 c++20 构建,断言两个 std 缓存目录并存、两个 target 目录并存、两次都成功(锁住 §2.2 证据 1) | Linux |
| `177_cpp20_msvc.sh` | MSVC 上 `standard = "c++20"`:纯 C++20 工程构建成功;`import std;` 工程在 `cl` ≥ 19.38 的 runner 上**构建并运行成功**(定案取 20 的实测锁);`compile_commands.json` 里出现 `/std:c++20` | Windows(CI) |
| 单测(非 e2e) | 老 MSVC 分支不可能在 CI 上有真机 → 用构造的 `Toolchain{compiler=MSVC, version="19.37"}` 单测 `importStdMinLevel==23` 与门的错误文案 | 全平台 |

CI 矩阵:`ci-linux-e2e.yml` / `ci-macos-e2e.yml` / `ci-windows-e2e.yml` 已按目录跑
`run_all.sh`,新增脚本自动进矩阵。musl / mingw 交叉档位由 `cross-build-test.yml`
覆盖一条 c++20 冒烟即可(§2.2 已实机跑通,CI 只做回归)。

### 5.3 MSVC 的实测义务(定案取 20 的前提)—— ✅ 已兑现

**结论(PR#325,2026-07-31):Windows CI 上 `177_cpp20_msvc.sh` 通过。**
runner 的 `cl 19.51.36252`(VS 18 Enterprise / MSVC 14.51.36231)用 `/std:c++20`
真实编译并运行了 `import std;`,`compile_commands.json` 里是 `/std:c++20` 而非
`/std:c++latest`。取 20 的前提成立;19.38 阈值仍只保护更老的 STL。


取 20 是**乐观决策**,所以它必须由实测背书,而不是由文档背书 —— 恰恰因为
微软自己的文档还写着 `/std:c++latest`(#3945 已 fixed 但 Learn 未更新)。

1. Windows CI 的 `177_cpp20_msvc.sh` 是硬断言(不是 observe-only):MSVC + `/std:c++20` +
   `import std;` 必须真的构建并运行成功。它挂 = 定案的前提不成立,回退到 23。
2. 该作业需打印 runner 的 `cl` 版本(`cl /Bv` 或 detect 的 `tc.version`),把
   "在哪个版本上验证过 20" 变成日志里的事实,而不是文档里的推算。
3. 19.38 阈值若被实测证伪(更早或更晚),改的仍然只有 `msvc.cppm` 里那一行。

## 6. 工作量分解(单 PR,目标 2026.7.31.1)

| # | 改动 | 文件 | 规模 |
|---|---|---|---|
| 1 | 白名单 + 文案 | `src/manifest/types.cppm` | ~20 行 |
| 2 | `importStdMinLevel` 字段 | `src/toolchain/model.cppm` | 2 行 |
| 3 | 四个 provider 赋值 + `toolset_at_least(19,38)` | `gcc.cppm` / `clang.cppm`(×2)/ `msvc.cppm` | ~15 行 |
| 4 | 二维能力门 + 文案 | `src/build/prepare.cppm:3735` | ~12 行 |
| 5 | build.mcpp `-std=` 走方言层(顺带缺陷) | `build_program.cppm` + 两处调用点 | ~15 行 |
| 6 | 单测 6 组 | `tests/unit/*` | ~80 行 |
| 7 | e2e 3 个 | `tests/e2e/175-177_*` | ~150 行 |
| 8 | 文档 EN/ZH | `docs/05-mcpp-toml.md`、`docs/zh/05-mcpp-toml.md` | ~30 行 |

**不需要**:新模块、新缓存字段、新 CLI、schema 迁移、生态侧改动。

### 验证与复现(本设计所用探针)

```bash
# 1) 取 mcpp 自己写下的真实 std 模块命令,替换档位后回放
python3 - <<'PY'
import json, os, subprocess
CACHE = os.path.expanduser("~/.mcpp/build-cache/v1/std")
for d in os.listdir(CACHE):
    m = json.load(open(f"{CACHE}/{d}/std-module.json"))
    out = f"/tmp/replay-{d}-c++20"; os.makedirs(out + "/pcm.cache", exist_ok=True)
    for c in m["std_build_commands"]:
        c = c.replace(f"{CACHE}/{d}", out).replace(f'-std={m["cpp_standard"]}', "-std=c++20")
        print(d, m["compiler"], m["compiler_version"],
              subprocess.run(c, shell=True, capture_output=True, text=True).returncode)
PY
# 2) 跨档位 BMI 不可复用(负向证据)
#    在 c++20 建好的 gcm.cache 目录里用 -std=c++23 编译 import std 的 TU:
#    → "std: error: language dialect differs 'C++20', expected 'C++23'"
```

## 7. 验收标准

1. `standard = "c++20"` 的工程,在 gcc 15/16(glibc、musl、mingw 交叉)与 clang+libc++ 上
   `mcpp build` / `mcpp run` / `mcpp test` 全绿,且源码可以 `import std;`;
2. `compile_commands.json` / `build.ninja` 里出现且仅出现 `-std=c++20`;
3. c++20 与 c++23 的 std BMI、target 目录、依赖构建缓存三者互不污染(e2e 176 锁死);
4. MSVC 上:纯 C++20 工程可构建;`cl` ≥ 19.38 时 `import std;` + `/std:c++20` **构建并运行成功**
   (Windows CI 硬断言);更老的 `cl` 得到**可行动错误**而非 `std.ixx` 内部错误;
5. 默认值仍是 c++23:未声明 `standard` 的工程指纹与本 PR 前**逐字节一致**(无全生态重编);
6. `c++fly` / `c++latest` 行为零变化(现有 e2e 100/101 不动即绿);
7. 两份文档(EN/ZH)同步,ZH 补齐 `c++fly` 条目。

## 8. 非目标(显式出圈)

- **不支持 C++17 及以下**:命名模块是 C++20 特性,mcpp 的构建模型在其下不成立;
- **不降低工具链下限**:仍需 GCC ≥ 15 / Clang ≥ 17(+libc++)/ MSVC(带 std 模块的 VS 2022);
  "支持 C++20"指的是**语言档位**,不是"老编译器能用了";
- **不做同图多档位**:跨档位 BMI 硬不兼容(§2.2),图全局单值是物理约束不是偷懒;
- **不改默认值、不改默认模板**(§4.6 的 `--standard` 是可选项);
- **不做依赖最低档位 floor 检查**(§4.3 的默认值不可区分陷阱)。

## 9. Review 定案(2026-07-31)

**Q1 MSVC 的 `importStdMinLevel`:取 20。** 与 GNU 侧一致 —— 压低档位是用户的知情选择,
不为"可能存在的老 STL"给全体 MSVC 用户加拒绝。落地形态是**带版本阈值的一行数据**
(`cl` ≥ 19.38 → 20,更老 → 23,§2.3),这样既尊重"取 20",又让门保持可达可测,
并且老 VS 用户拿到的是可行动诊断而不是 `std.ixx` 内部错误。阈值待 Windows CI 实测确认。

**Q2 不加 `mcpp new --standard`,模板不动。** 默认 c++23;用户主动改成 c++20 即表示知道
自己放弃了哪些特性。C++20 的定位是"给特殊约束的用户多一个选择",模板与文档示例继续推荐 C++23。

**Q3 不引入"standard 是否显式声明"位。** 今天没有消费者,加了就是无人验证的字段。
留待真正要做依赖 floor 检查的那一步再加,届时必须:字段放 `Manifest::package`
(`std::optional<std::string>`),**`toml.cppm:194` 与 `xpkg.cppm:1070` 两条解析路径同时填**,
否则又是"同一决策两处推导"。本条保留在此文档,作为那一步的前置约束。

**Q4 收 `c++2a` / `gnu++2a` 别名**(与既有 `c++2b` / `c++2c` 对称,零成本)。
