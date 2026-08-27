# #516 解决方案:glob walk 在 Windows 上撞到 ANSI 代码页拼不出的目录名就崩

日期:2026-08-27 · 基线:`e187d3f`(origin/main,2026.8.27.1)· 发布版本:**2026.8.27.2** · 状态:**已决策,已实现**

> **决策记录(2026-08-27,来自 review)**
> 1. 口径 **B** —— 跳过要报出来,不静默(§2)
> 2. **P0 + P1 同一个 PR** 全部实现,不拆
> 3. §6.2 的独立 issue **保留**(P2 清单 + 项目根含非 ACP 的静默空构建)
> 4. **不做 httplib 端到端**,单元测试覆盖即可(§5.4 相应作废)
>
> 实现过程中有三处偏离本文初稿,都是测量推翻的,记在 §10。

> Issue: mcpp-community/mcpp#516 · 复现:mcpplibs/mcpp-index PR #260
> 完整取证过程见 openxlings/xlings 仓 `.agents/docs/2026-08-27-mcpp516-windows-acp-path-analysis.md`
> xlings 侧的同类隐患(与本 issue 无关,独立修):openxlings/xlings#571

---

## 0. 三行结论

1. **不是解压问题。** 解压是成功的,文件以正确的 UTF-16 名字落盘;
   抛异常的是 **mcpp 自己**,在 `src/modgraph/scanner.cppm:238` 的 `dir.filename().string()`。
2. **这是 #230 的同一处漏网。** #231 的 "never-throw on unnarrowable names" 加固了
   `path_matches_glob` / `rewrite_rel_copy` / `local_include_dirs_for` 三处,
   **漏掉了同一个 walk 循环里比它们早一行执行的 `is_excluded_walk_dir`**。
   #516 看到的这条整洁报错,正是 #231 顺手加的顶层 catch 在正常工作。
3. **爆炸半径不是一个包。** mcpp-index 128 个 recipe 里 **101 个(79%)**含以 `*` 开头的 glob,
   即"从解压根无界递归遍历"。任何上游 tarball 哪天多一个非 ASCII 文件名,当天 Windows 就红。

---

## 1. 缺陷

### 1.1 抛出点

```cpp
// src/modgraph/scanner.cppm:236-239
bool is_excluded_walk_dir(const std::filesystem::path& dir,
                          const std::filesystem::path& root) {
    auto name = dir.filename().string();                                 // ← 抛
    if (name == ".mcpp" || name == ".git" || name == "target") return true;
```

MSVC STL 的 `path::string()` → `__std_fs_convert_wide_to_narrow(ACP, …)`;
对非 UTF-8 代码页它传 `lpUsedDefaultChar`,一旦用了替换字符就返回
`__std_win_error::_No_unicode_translation`(Win32 1113),
`_Throw_system_error_from_std_win_error` 抛 `std::system_error`。
`system_error(ec)` 不带 what_arg 时 `what()` 就是 `ec.message()`,即

> `No mapping for the Unicode character exists in the target multi-byte code page.`

与 issue 里的报错**逐字一致**。

该函数是 walk 循环体的**第一行**(`scanner.cppm:388` 和 `:483` 两处调用),
**每个目录条目调用一次** —— 所以它比后面所有已加固的站点都先执行,
加固后面三处对目录名根本无效。

### 1.2 怎么走到那棵树的

`compat.httplib.lua`(Form B)带 `include_dirs = { "*" }`,
经 `manifest/xpkg.cppm:1242` 落到 `buildConfig.includeDirs`,
由 `scanner.cppm:788` `local_include_dirs_for()` → `expand_dir_glob(verRoot, "*")`:

```
"*"  含通配符 → 不走字面量快路径
glob_literal_prefix("*") → 通配符在第 0 位 → 前缀为空
start = root → recursive_directory_iterator 遍历整棵 cpp-httplib 源码树
        → test/www/日本語Dir/ 必然被访问
```

`scanner.cppm:796` 自己的注释把 `"*"` 称作
"the `*` extracted-tarball-root glob convention" —— 这是**约定**,不是 httplib 的怪癖。

### 1.3 异常一路无人接手

`expand_dir_glob` → `local_include_dirs_for` → scanner → prepare → `cli::run`
全链路无 catch,最终由 `src/main.cpp:36` 的顶层 catch 接住并 exit 70。

### 1.4 归属反证(值得记住的一条)

有人会怀疑是 xlings 解压时按 ANSI 写坏了名字。**恰恰相反**:
`ERROR_NO_UNICODE_TRANSLATION` 的前提是**宽名里存在 ACP 拼不出的字符**。
若盘上是 mojibake(UTF-8 字节被 CP1252 逐字节打散),那些字符**逐个都在 CP1252 里**,
mcpp 反而不会抛。**mcpp 抛了,正好证明 xlings 写对了。**

---

## 2. 必须先定的口径(A / B)

收敛成唯一入口这件事,**必然要一次性定下"转不出来时怎么办"**,否则每个调用方还是各答各的。

**现状**(#231 留下的):`path_matches_glob` 的 try/catch 返回 `false`
→ 一个 CJK 命名的源文件**既不报错也不编译,它就是不存在**。

| | 行为 | 代价 |
|---|---|---|
| **A** | 保持静默丢弃 | 用户加了个 `模块.cppm`,构建"成功"、符号找不到、零提示 |
| **B** | 丢弃并 `ui::warn` 一次,指名 | 要给这条 warning 找一个不刷屏的去重点 |

**推荐 B。** 理由不是洁癖:这条路径上"没发生"和"成功了"输出完全一样,
而 #230 → #516 之间隔了整整一个大版本才被再次发现,正是因为中间那段时间它只是**静默少编译**。

**本方案按 B 写。** 若定 A,§4.2 的 `emit_unnarrowable_warning()` 整块删掉即可,其余不变 ——
**P0 与口径无关**,可以先合。

---

## 3. P0 — 修掉抛出点

### 3.1 补丁

`src/modgraph/scanner.cppm:236-245`:

```cpp
bool is_excluded_walk_dir(const std::filesystem::path& dir,
                          const std::filesystem::path& root) {
    // 按 path 比较,不要窄化。
    //
    // `filename().string()` 在 MSVC 上走宽→ANSI 转换,对当前代码页拼不出的名字
    // 抛 std::system_error(#230 是同一个转换,#516 是同一个转换的另一处站点)。
    // 而这个函数是 walk 循环体的第一行 —— 每个目录条目都经过它,所以它比
    // path_matches_glob 的 try/catch 更早执行,加固那里对目录名无效。
    //
    // 三个字面量都是 ASCII,转成 native(Windows 上是宽)无损;
    // path::operator== 比较的是 native 串,大小写敏感,行为与原来的窄串比较逐字一致。
    // 用静态常量而非每次构造临时 path,保住 #225 的 per-entry 开销预算。
    static const std::filesystem::path kMcpp{".mcpp"};
    static const std::filesystem::path kGit{".git"};
    static const std::filesystem::path kTarget{"target"};
    const auto name = dir.filename();
    if (name == kMcpp || name == kGit || name == kTarget) return true;

    auto const& submodules = submodule_paths(root);
    if (submodules.empty()) return false;
    std::error_code ec;
    auto c = std::filesystem::canonical(dir, ec);
    return submodules.contains(ec ? dir : c);
}
```

### 3.2 为什么这一处就够(下游已核过)

修好之后,那条 walk 上**不再有**会碰到坏名字的窄化点:

| 后续步骤 | 是否窄化 |
|---|---|
| `it.depth()` / `chain.resize` | 否 |
| `fs::canonical(e.path(), eec)` | `error_code` 重载,不抛 |
| `std::find(chain…)` / `std::set<path>` | `path` 比较,原生串 |
| `path_matches_glob` | **已加固**(`glob.cppm:47-57`)→ 返回 `false` |
| `local_include_dirs_for` 收集结果 | 坏目录已被上一行判为不匹配,不在结果里 |

即:坏名字被**挡在窄化世界之外**,后面拿不到它。

### 3.3 修好之后的行为

`日本語Dir` **不会**成为 include dir(`path_matches_glob` 判不匹配)。
这是**可接受的既有语义**:一个 ACP 拼不出的名字,也没法写进 glob 或编译命令行。
对 cpp-httplib 而言那是 `test/www/` 下的测试数据,不影响头文件消费。

---

## 4. P1 — 收敛(真正的修复)

P0 只是第四个点补丁。再打点补丁,下次会有第五个站点。

### 4.1 两条规则,不是一条

- **规则 1:只与 ASCII 字面量比较的谓词,根本不该窄化** —— 按 `path` 比。
  (`is_excluded_walk_dir` 属于这条,所以 P0 里没用到 `try_narrow`。)
- **规则 2:必须产出窄串的站点,走唯一入口。**

### 4.2 唯一入口

放进 `mcpp.modgraph.glob`(已有的"唯一 glob 匹配器"模块,`glob.cppm` 开头那段
"两个匹配器'应该'一致是这个代码库反复付账的形状,所以只有一个" 的理由同样适用):

```cpp
// 走查得到的路径 → 窄串。当前 ACP 拼不出该名字时返回 nullopt,**永不抛**。
//
// 任何来自 directory_iterator 的 path 都必须经过这里,不得直接 .string()。
// 非 Windows 上这是一次拷贝,永远有值。
std::optional<std::string> try_narrow(const std::filesystem::path& p) {
    try { return p.generic_string(); }
    catch (const std::exception&) { return std::nullopt; }
}
```

口径 B 的 warning(定 A 则删掉此块):

```cpp
// 每个进程每个"不可拼写的父目录"只报一次,避免一棵树刷屏。
void emit_unnarrowable_warning(const std::filesystem::path& p);
```

warning 文案必须**只用可拼写的部分**定位,否则打印它本身又会抛:

```
warning: skipped a path whose name cannot be represented in the active code page (1252)
         under: <p.parent_path() 的可拼写前缀>
         hint:  这些文件不会参与构建。`chcp` 改的是控制台代码页,不是进程 ACP。
```

> 拼装这条消息时**不要**去窄化那个坏名字。向上找到第一个 `try_narrow` 成功的祖先,
> 报到那一级为止。这条不是洁癖 —— 是本类缺陷最容易复发的地方
> (诊断代码自己碰同一个转换)。

### 4.3 审计范围(逐个追过输入来源)

初稿我按文件名 grep 出一张表就交了,那不是测量。逐个追输入来源后砍掉两个误报:

| 站点 | 输入来源 | 结论 |
|---|---|---|
| `modgraph/scanner.cppm:238` | 任意包/项目树的目录名 | **P0,确认触发** |
| `pack/digest.cppm:48` | `pack/prebuilt.cppm:144` 的递归走查 | **真** —— 打包路径不经 glob 过滤 |
| `scaffold/template.cppm:265` | 模板目录名 | **真**,仅当第三方模板目录含非 ACP 名 |
| ~~`build/resources.cppm:66`~~ | `RcTool::path.filename()` | **误报**:那是 `windres`/`rc.exe`/`llvm-rc`,payload 相对定位,全 ASCII |
| ~~`modgraph/p1689.cppm:339`~~ | `source.filename()` | **误报**:CJK 文件名到不了这里(`path_matches_glob` 已先判不匹配) |

`p1689` 那处值得单说,因为它顺带答了一个会被问到的问题:
**"用户自己的 CJK 源文件今天会怎样?"** —— 不是崩,是**静默不编译**。
真正能让 p1689 抛的是同一行的 `source.string()`(**整条绝对路径**,第 341 行):
当**项目根目录自身**含非 ACP 字符时(英文版 Windows + 中文项目路径)。
但那种情况下 `path_matches_glob` 会先把所有源文件判为不匹配,
构建会先以"没有源文件"失败 —— 又一个"没发生和成功了输出一样"。
**这一条不在本 PR 范围**,建议单独立 issue(见 §6.2)。

### 4.4 不变式与 CI 门

写进 `AGENTS.md`:

> **任何来自 `directory_iterator` 的 `path` 都不得直接 `.string()` / `.generic_string()`。**
> 只与 ASCII 字面量比较时按 `path` 比;必须产出窄串时走 `mcpp::modgraph::try_narrow()`。

CI 门(只覆盖真正会走查任意树的目录,并**明说它的边界**):

```bash
# tools/check-narrow-conversions.sh —— 只管 src/modgraph/,因为只有这里走查任意树。
# 它挡的是"新写的代码直接窄化",挡不住"经由别处传进来的 path"——
# 后者靠 §4.2 的唯一入口约定,不靠这个 grep。
if grep -rnE '\.(filename|stem|extension)\(\)\.(generic_)?string\(\)' src/modgraph/ \
   | grep -v 'NARROW-OK:'; then
    echo "FAIL: 直接窄化走查路径。用 try_narrow(),或加 // NARROW-OK: <理由>"; exit 1
fi
```

> 这个门**不是**判据本身,只是让复发变贵一点。判据是 §5 的 Windows 测试。
> 别把它写进验收标准里当"已覆盖"用。

---

## 5. 回归测试(这一节比补丁重要)

### 5.1 一个必须先说清的前提

**在 Linux/macOS 上写的任何测试都无法证伪这个缺陷。**
那两个平台上 `path::string()` 只是拷贝 native 串,不做编码转换,任何文件名都"能表示"。
所以:**测试必须跑在 Windows CI 上,且 runner 的 ACP ≠ 65001。**

`ci-windows.yml` 的 `build + test + package (windows x64, self-host)` job 会跑
`mcpp test`,`tests/unit/test_modgraph.cpp` 在其中 —— 这就是落点。
`windows-latest` 默认 ACP=1252,满足。

**但不能默认它永远满足**:哪天 runner 镜像默认 UTF-8 ACP,这个 case 会**静默变成永远绿的装饰品**。
所以测试自己必须探测,并在探测不到条件时 `GTEST_SKIP()` 且**说明原因**。

### 5.2 名字选择(一个容易踩的坑)

用 `日本語Dir` 忠实于 issue,但它在 **CP932(日)/CP936(简中)/CP950(繁中)上是可拼写的** ——
在这些机器上测试会 skip,等于本地没有覆盖。

**天城文(Devanagari)不在任何 Windows ANSI 代码页里**,因此在**所有**非 UTF-8 ACP 下都不可拼写。
两个都造:天城文保证覆盖,日文保证与 issue 同形。

用 `\uXXXX` 转义而非直接写字面量 —— 不依赖源文件编码设置(MSVC 需要 `/utf-8`,clang 默认 UTF-8;
这个测试不该因为哪天换了编译器就变味)。

### 5.3 测试代码

追加到 `tests/unit/test_modgraph.cpp`:

```cpp
namespace {
// 当前 ACP 能否拼出这个宽名?能 → 说明 ACP 是 UTF-8(或该名字恰好在表内),
// 本用例的前提不成立。用 path 自己的转换来问,问的就是被测代码走的那条路。
bool acp_can_spell(const std::wstring& w) {
    try { (void)std::filesystem::path(w).string(); return true; }
    catch (const std::exception&) { return false; }
}
} // namespace

// #516 / #230:走查任意包树时,一个当前 ANSI 代码页拼不出的目录名
// 曾让 is_excluded_walk_dir 的 .filename().string() 抛 std::system_error,
// 一路逃到 main() 变成 `error: internal: unhandled exception:` + exit 70。
//
// 断言的是「不抛」且「walk 没被截断」,不是「能匹配到那个名字」——
// 一个 ACP 拼不出的名字也没法写进 glob 或编译命令,判它不匹配是既有语义。
TEST(Scanner, GlobWalkSurvivesUnnarrowableNames) {
    // 用 \u 转义,不写字面量:这个测试不该因为哪天换了编译器或改了源文件编码
    // 设置(MSVC 要 /utf-8,clang 默认 UTF-8)就悄悄变味。
    const std::wstring devanagari = L"\u0915\u0916\u0917Dir";  // कखगDir：不在任何 ANSI 代码页
    const std::wstring japanese   = L"\u65e5\u672c\u8a9eDir";  // 日本語Dir：与 #516 同形

    if (acp_can_spell(devanagari)) {
        GTEST_SKIP() << "active code page can spell any name (UTF-8 ACP or non-Windows); "
                        "this defect is unreachable here";
    }

    auto dir = make_tempdir("mcpp-scanner-acp");
    std::filesystem::create_directories(dir / std::filesystem::path(devanagari));
    std::filesystem::create_directories(dir / std::filesystem::path(japanese));
    // 好邻居:证明 walk 没有在坏条目处提前结束。
    write(dir / "zzz_ascii" / "x.h", "#pragma once\n");

    // include-dir 通道(#516 的实际路径:include_dirs = { "*" })
    std::vector<std::filesystem::path> dirs;
    ASSERT_NO_THROW({ dirs = expand_dir_glob(dir, "*"); });
    EXPECT_NE(std::find(dirs.begin(), dirs.end(), dir / "zzz_ascii"), dirs.end())
        << "walk 在不可拼写的条目处被截断了";

    // 文件通道(installedLayoutMatchesIndex 走的那条:前缀为空 → 全树遍历)
    std::vector<std::filesystem::path> files;
    ASSERT_NO_THROW({ files = expand_glob(dir, "**/*.h"); });
    EXPECT_NE(std::find(files.begin(), files.end(), dir / "zzz_ascii" / "x.h"), files.end());

    std::filesystem::remove_all(dir);
}
```

### 5.4 端到端那一层

单测锁住机制,但锁不住"真实上游 tarball + `include_dirs = {"*"}` 约定"这个组合。
唯一能覆盖它的是 **mcpp-index 的 windows 矩阵跑 httplib 三个 example**(PR #260 本身)。
建议:#260 合入后,把 `httplib` / `httplib-tls` / `httplib-zstd` 留在 windows 矩阵里,
**不要**以"windows 上不消费该 feature"为由把它们门控掉 —— 那会把这个覆盖点关掉。

---

## 6. 不做什么

### 6.1 不在 mcpp 侧"宽容处理"文件名

不要试图把不可拼写的名字转义/哈希后当成可用路径。它们最终要交给
编译器驱动、ninja、link 命令行,而那些都是**独立进程、按各自的 ACP 解释字节**。
在 mcpp 内部造一个"看起来能用"的名字,只会把失败推到更远、更难归因的地方。

### 6.2 P2(嵌 `activeCodePage=UTF-8` 清单)不进本 PR

给 Windows 可执行文件嵌带 `<activeCodePage>UTF-8</activeCodePage>` 的清单,
`__std_fs_code_page()` 返回 CP_UTF8,`path::string()` 从此返回 UTF-8 且**永不抛**,
上面所有站点一次性全对。要求 Windows 10 1903+(CI runner 满足)。

**它不危险**:今天能跑通的路径全是 ASCII,而 ASCII 在 UTF-8 和 CP1252/936 下**字节完全相同** ——
build.ninja、CDB、命令行一个字节不变。

**真正的未知是"够不够"**:mcpp 写出 build.ninja 之后,是**另一个进程**
(xlings store 里的 `ninja.exe`,`config.cppm:192`)按**它自己的** ACP 去读。
所以嵌清单只解决 mcpp 这一半;非 ASCII 路径能否端到端跑通,取决于 ninja 和编译器驱动那一侧。

**建议独立立 issue,验收写成"mcpp 不再抛这个异常",不要写成"支持 CJK 路径"。**
后者要单独测量,不能顺带宣称。

同一个 issue 里一并收 §4.3 提到的"项目根目录含非 ACP 字符 → 先以『没有源文件』失败"。

---

## 7. 落地顺序与验收

| 步 | 内容 | 验收 |
|---|---|---|
| 1 | P0 补丁(§3.1) | `ci-windows` 绿 |
| 2 | §5.3 单测 | **打补丁前必须红**,补丁后绿。CI 日志里能看到它**没有** skip |
| 3 | P1 收敛(§4.2/4.3)+ `AGENTS.md` 不变式 + CI 门 | 门在故意加一行裸 `.filename().string()` 时会红 |
| 4 | 真实复现验证 | mcpp-index PR #260 的 windows 矩阵三个 httplib 用例转绿 |

**第 2 步的"打补丁前必须红"不能省。** 一个从来没红过的测试不证明任何事
—— 先在打补丁前跑一次 `ci-windows`(或 workflow_dispatch),把那条
`error: internal: unhandled exception: No mapping…` 留在日志里,再合补丁。

**第 4 步要注意归因**:mcpp-index 的 CI 会随索引发布漂移。
验证时**重跑同一个未变更的 run id**,不要用一次新的 push 去比对 ——
否则"绿了"可能是索引变了,不是补丁起作用了。

### 不算验收的东西

- Linux/macOS 全绿:**与本缺陷无关**,那两个平台上它不可能发生。
- CI 门通过:它只挡新写的直接窄化,不是判据(§4.4)。
- 单测 skip 掉也算绿:**必须检查它真的跑了**。

---

## 8. 风险与回滚

- **P0 行为变化**:`path::operator==` 大小写敏感,与原来的 `std::string` 比较**逐字一致**;
  三个字面量全 ASCII,native 转换无损。**没有行为变化**,只是不再抛。
- **性能**:静态 `path` 常量 + 三次 native 串比较,比原来"每条目一次 `std::string` 构造 + 三次比较"
  **更便宜**。#225 的 per-entry 预算不受影响。
- **回滚**:P0 是单函数改动,回滚即恢复原状(重新可崩)。
  P1 若引入问题,可先只回滚 §4.2 的 warning(口径 B → A),保留唯一入口。

---

## 9. 多角度拆分、依赖关系与跨仓协作

### 9.1 各角度落在哪一处改动上

| 角度 | 这次的具体决定 |
|---|---|
| **架构** | `src/modgraph/` 与 `src/manifest/` 是 leaf 层 —— **全仓 31 个模块 import `mcpp.ui`,这两层一个都没有**,`mcpp.diag` 亦然。所以 glob 层**记录**(`note_unnarrowable_path`),CLI 层**排空上报**。不为了少写十行而捅穿这条边界。 |
| **稳定性** | 抛出点消失,而不是被 catch 住。`try_narrow` 本身 `noexcept` 语义(内部 try),调用方拿 `nullopt`。 |
| **优雅简洁** | 不是第四个 try/catch,而是**按用途分三档的一条规则**(§4.1)。`is_excluded_walk_dir` 的正解是**根本不窄化**,比加保护更短也更快。 |
| **用户体验** | 口径 B:跳过按目录报告一次,走 `diag::degraded` 并给出 `impact`。`hint` 里点名 `chcp` 改的是控制台代码页 —— 那是用户第一个会试的东西。 |
| **兼容性** | `path::operator==` 与原来的窄串比较**逐字同解**;`interface_set_digest` 改用 `u8string()` 对纯 ASCII 名字**字节不变**,已发布包摘要不变。 |
| **跨平台** | 非 Windows 上 `try_narrow` 永不失败 → 一条记录都不会产生 → 行为与今天完全一致。`digest` 那处顺带修掉了一个**真实的跨平台不一致**(Linux 打包 / Windows 校验对非 ASCII 名字给出不同摘要)。 |
| **一致性** | 窄化只有一个答案者;报告只有一个 sink(`mcpp.diag`,它本就是全仓唯一的用户可见告警通道);排空只有一个点(`cli::run` 的 scope guard,覆盖全部五条 return)。 |
| **无感升级** | 没有配置项、没有新 flag、没有行为开关。ASCII 项目(几乎全部)的输出一个字节都不变。 |

### 9.2 任务依赖

```
T1 try_narrow + 记录/排空 API (glob.cppm)
 ├─→ T2 P0: is_excluded_walk_dir 停止窄化      ← #516 的实际崩溃点
 ├─→ T3 其余站点归并 (path_matches_glob / scan_file 诊断 / digest / template)
 ├─→ T4 单元测试 (依赖 T1 的 API 才能断言"记录到了")
 └─→ T5 CI 门 (依赖 T2/T3 才可能绿)
T6 文档与规范 (docs/05 EN+zh 双份 —— check_docs_style 强制标题结构对齐)
T7 版本号 2026.8.27.1 → 2026.8.27.2 + CHANGELOG
T8 xlings pin 2026.8.17.2 → 2026.8.27.4   ← 跨仓,见 9.3;放在最后一个 commit
T2..T8 → T9 CI 全绿 → T10 自我 review → T11 release → T12 生态验证
```

T1 是唯一的串行瓶颈;T2/T3/T4/T6/T7 之后可并行。

### 9.3 跨仓协作边界

| 仓 | 这次做什么 | 依赖 |
|---|---|---|
| `mcpp-community/mcpp` | 全部代码改动 + 发布 2026.8.27.2 | — |
| `openxlings/xlings` | **不改**。#516 不是 xlings 的缺陷(§1.4)。它自己的同类隐患另立 **openxlings/xlings#571** | 独立 |
| `mcpplibs/mcpp-index` | **不改**。按 review 决定不做 httplib 端到端(§5.4 作废),PR #260 在 mcpp 2026.8.27.2 发布后自然转绿 | 依赖 mcpp 发布 |

**唯一一条真正的跨仓阻塞**:`kXlingsVersion` → `2026.8.27.4` 要求该版本**已发布**,
否则 CI 会去下载一个不存在的 `xlings-2026.8.27.4-<platform>` tarball。
`.github/tools/check_version_pins.sh` 会强制 `.github/` 下全部 pin 点与常量一致,
所以这是一次**原子的多点改动**,放在最后一个 commit,发布确认之后再推。

### 9.4 "打补丁前必须红"怎么落地

分两个 commit 推,第一个**故意不含 P0**:

- `ci-windows` 应当在 `Scanner.GlobWalkSurvivesNamesTheCodePageCannotSpell` 失败
  → 证明这个测试**能**失败(而不是恰好通过);
- `ci-linux` 应当在 `check_narrow_conversions.sh` 失败并点名 `scanner.cppm` 那一行
  → 证明这道门**能**抓到真实缺陷,而不只是抓到我构造的负例。

第二个 commit 打上 P0,两条腿转绿。一次推送拿到两份证据。

---

## 10. 实现推翻了初稿的三处

1. **`build/resources.cppm` 与 `modgraph/p1689.cppm` 是误报。** 初稿那张审计表是按文件名
   grep 出来的,不算测量。逐个追输入来源后:前者narrow 的是 `windres`/`rc.exe` 的文件名
   (全 ASCII),后者拿到的路径必然已过 glob 过滤。**真正漏网的是 `pack/digest.cppm`**
   —— 它吃的是对已发布包的**未经过滤**的 `recursive_directory_iterator`,初稿没提。
2. **CI 门的作用域必须收窄。** 第一版覆盖 `src/modgraph src/manifest src/pack src/scaffold`,
   一开就是 **22 个命中,其中约 20 个是假阳性** —— `src/pack` 窄化的是 mcpp 自己产生的
   名字(staging root、built binary、strip artifact),`src/manifest` 只窄化 `.extension()`。
   一道有二十个假阳性的门,一个月内必然被绕过,而那条豁免会变成"曾经有过规则"的唯一记录。
   收窄到 `src/modgraph src/scaffold`,并在脚本头写明**通过 ≠ 已审计**。
3. **口径 B 的实现不能用 `mcpp.log`。** 它的默认 level 是 `off`,`log::warn` 默认什么都不打印
   —— 那样口径 B 会退化成口径 A,而且看起来像做了。正确通道是 `mcpp.diag`,
   它的模块注释本就写着"`log::debug` 和 `log::verbose` 不算用户可见"。

### 一个已知的、没有测试覆盖的环节

`cli.cppm` 里那个 scope guard 的**接线**只由阅读保证:记录逻辑有单测
(`Glob.UnnarrowablePathsDedupToTheirSpellableAncestor`),Windows 用例断言了"确实记录到了",
但"CLI 真的把它打出来了"没有自动化验证 —— 那需要一个 Windows e2e,而本次 review
明确把范围限定在单元测试。写在这里,而不是假装它被覆盖了。
