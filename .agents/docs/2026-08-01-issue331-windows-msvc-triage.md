# issue #331 逐条核验 + Windows 无 MSVC 默认工具链分析

日期：2026-08-01 · 基线：main @ 7f1489d（mcpp 2026.8.1.1）
方法：全部结论对着 HEAD 源码核验，逐条给行号。**没有在 Windows 上实跑**——凡是只能由 Windows 侧行为决定的，下面明确标注"未实测"。

---

## 0. 结论速览

| # | 报告的问题 | 真实性 | 根因是否如报告所述 | 归类 |
|---|---|---|---|---|
| 1 | `build.mcpp` 在 Windows+MSVC 完全不可用 | ✅ 真实 | ⚠️ 只说对了第一层，**底下还有一层更硬的** | A：真实且通用 |
| 2 | cppwinrt 投影头目录不在 `INCLUDE` | ✅ 真实 | ✅ 完全准确 | A（加目录）+ B（生成投影头） |
| 3 | 绝对路径 `include_dirs` 跳过 glob；且被空格拆开 | ✅ 两半都真实 | ✅ 前半准确；**后半我定位到了** | A：真实且通用（非 Windows 专属） |
| 4 | `ldflags` 文件输入不能用工程相对路径 | ✅ 真实 | ✅ 准确 | A（补 typed 字段）+ C（"像路径就重解析"的建议不该做） |
| 5 | 文档在 `[build]` 下举例 `linkage` | ✅ 真实 | ✅ 准确，且**还漏了第三处** | A：文档 bug；建议的"实现成简写"不该做 |
| 6 | path 索引注册落在沙盒 xlings 配置里 | ✅ 症状真实 | ❌ **代码不是这么走的** | D：真实症状 + 根因误判 |

分类含义：
- **A** = 真实且通用，mcpp 该修
- **B** = 真实，但不该由 mcpp 承担（属于 `build.mcpp` / 项目侧）
- **C** = 提议本身有害，应拒绝（问题真实，解法不对）
- **D** = 症状真实，但报告给出的机制与代码不符，得重新定位

**没有一条是伪问题。** 六条全部可在源码里指到行。分歧只在"根因"和"该怎么修"。

---

## 1. `build.mcpp` 在 Windows + MSVC 下完全不可用 — 真实，且比报告更严重

### 1a. 报告的那层：`argv[0]` 不加引号（确认）

`src/platform/process.cppm:230-245`：

```cpp
#if defined(_WIN32)
    std::string cmd = argv[0];        // 刻意保留 RAW
#else
    std::string cmd = mcpp::platform::shell::quote(argv[0]);
#endif
```

`src/build/build_program.cppm:699` 正是 `capture_exec(compileArgv, {}, compileCwd)`，Windows 上 `capture_exec` 走 `popen`/`cmd.exe`（`process.cppm:371-378` 明确写了 Windows 保留 `std::system` 路径）。`cl.exe` 路径必含空格 → `'C:\Program' is not recognized`。**完全属实。**

**报告没提但同样成立**：这不是 `build.mcpp` 专属。`command_from_argv` 是 Windows 上**所有** `capture_exec`/`run_exec` 的公共入口，调用点包括 `execute.cppm:504/716/792/1270`（跑测试、跑 `mcpp run`）、`xlings.cppm:1356`（探 ninja 版本）。今天没炸只是因为 mcpp 自己装的 payload 路径（`C:\Users\<u>\.mcpp\...`）不含空格。**用户名带空格的 Windows 账户，或把 MCPP_HOME 放在 `C:\Program Files\` 下，同样会炸。**

### 1b. 报告没看到的那层：`build.mcpp` 的编译命令行是**写死的 GNU 方言**

`build_program.cppm:678-695`：

```cpp
std::vector<std::string> compileArgv = { hostCompiler.string(), std_flag, "-O0" };
...
compileArgv.push_back("-x"); compileArgv.push_back("c++");
...
if (staticHostHelper) compileArgv.push_back("-static");
compileArgv.push_back("-o"); compileArgv.push_back(bin.string());
```

整个 `build_program.cppm` 里 `grep -i msvc` 只有三条**注释**命中，零条代码分支。`-O0` / `-x c++` / `-o` / `-static` 全是 GCC/Clang 驱动语法，`cl.exe` 一个都不认。`host_base_flags(tc)`（`:168-243`）也只有 Clang 和 GCC 两个分支，MSVC 走到最后会拼出一串 `-B` / `-L` / `-Wl,`。

**所以：把 #1a 的引号修好，Windows+MSVC 的 `build.mcpp` 依然编不过**，只是错误从 `'C:\Program' is not recognized` 变成一串 `cl : Command line warning D9002: ignoring unknown option '-O0'` + `LNK1181`。报告里"顺带"提的 `mcpp:link-lib` 被硬拼成 `"-l" + val`（`:132`）是同一个病的第三个症状，不是独立问题。

**真正的修法**是让 `build.mcpp` 通道走已有的 `mcpp.toolchain.dialect`（`CommandDialect`，0.0.89 就建好了，`flags.cppm` / `ninja_backend.cppm` 都在用），而不是在 `build_program.cppm` 里补 MSVC 特判：

- 编译 argv：`std_flag` / 优化档 / 输入语言 / 输出路径 四个位置改由 dialect 拼；
- 指令面：`link-lib` / `link-search` 落到 dialect 的库/搜索路径拼写（`/link foo.lib` vs `-lfoo`）；
- `-static` → MSVC 侧是 `/MT`，落在 `staticHostHelper` 那个单一决策点里。

### 1c. 为什么 CI 没抓到

`.github/workflows/ci-windows-e2e.yml:70` — 整个 Windows e2e 套件跑在 `llvm@20.1.7` 上。MSVC 只有一个专属脚本 `tests/e2e/99_msvc_native_build.sh`，而它里面 `build.mcpp` 出现 **0 次**。十个 `build.mcpp` e2e（89/92/110/111/125/144/145/164/168 + 97）全在 clang 下跑，clang 的 payload 路径不含空格。

> **覆盖缺口（本次三条 A 类问题的共同成因）**：`MSVC × {build.mcpp, 含空格路径, cppwinrt, /MT}` 这个笛卡尔积在 CI 里是空的。

**归类：A（真实且通用）。** 而且 1a 的影响面超出 MSVC，是全 Windows 的路径假设 bug。

---

## 2. C++/WinRT 投影头目录不在 `INCLUDE` — 真实，但要拆成两半

`src/toolchain/msvc.cppm:455-461` 一字不差：

```cpp
env.push_back({"INCLUDE", join({
    tools / "include",
    sdk.root / "Include" / sdk.version / "ucrt",
    sdk.root / "Include" / sdk.version / "um",
    sdk.root / "Include" / sdk.version / "shared",
    sdk.root / "Include" / sdk.version / "winrt",
})});
```

报告对 `winrt` vs `cppwinrt` 的区分是对的：前者是 ABI 头（`windows.foundation.h`，C 风格 IUnknown 接口），后者是 C++/WinRT 投影（`winrt/Windows.Foundation.h`）。

**两半要分开判：**

- **`cppwinrt` 目录存在时加进 `INCLUDE`** —— 这是 A 类。一行的事，语义等价于 `vcvarsall` 本来就做的（MSBuild 的 C++/WinRT targets 会加这个目录）。mcpp 声称"合成 INCLUDE/LIB，不走 vcvarsall"（`docs/03-toolchains.md:183`），那合成就该完整。**建议按报告做，`exists()` 判断后追加。**

- **目录不存在时现场跑 `cppwinrt.exe -in local -out <dir>` 生成** —— 这是 **B 类，不该由 mcpp 做**。理由：
  1. 这是**代码生成**，和 `xcb` 的 `c_client.py`、`nasm` 汇编、`rc.exe` 资源编译同类，mcpp 对这类东西的既定答案就是 `build.mcpp`（`docs/07-build-mcpp.md`），不是往工具链探测里塞 SDK 工具调用；
  2. 生成参数（`-in local` vs `-in <winmd>` vs `-in 10.0.26100.0`、`-out`、`-optimize`、`-overwrite`）是项目决策，不是工具链属性；
  3. 一旦 mcpp 内置这个，等于把 `cppwinrt.exe` 的 CLI 契约焊进 mcpp 的发布周期。

  报告自己的绕行方式（prebuild 里探测+生成）就是正确形态 —— 它今天必须写成外部 `prebuild.sh`，**只是因为 #1**。#1 修好，这一半自动回到 `build.mcpp` 里，不需要 mcpp 做任何事。

**归类：A（加目录，一行）+ B（生成投影头，属 `build.mcpp`）。**

---

## 3. 绝对路径 `include_dirs` — 两半都真实，**后半我定位到了**

### 3a. 跳过 glob 展开（确认）

`src/build/plan.cppm:261-270`：

```cpp
if (inc.is_absolute()) return { inc };   // 绝对路径原样返回
const auto glob = inc.generic_string();
auto expanded = mcpp::modgraph::expand_dir_glob(root, glob);
```

`expand_dir_glob(root, glob)` 的签名是 root-relative 的，所以早退是**实现便利**留下的口子，不是设计。属实。

### 3b. "被拆成三个参数落到源文件位置" —— 报告说没定位到，**在这里**

同一份数据走**两条通道**，一条加引号一条不加：

| 通道 | 位置 | 处理 |
|---|---|---|
| 全局 `include_flags`（ninja 变量） | `src/build/flags.cppm:239-242` | `shell_quote_arg(escape_path(t))` ✅ 有引号 |
| per-TU `$local_includes` | `src/build/ninja_backend.cppm:108-132` | `escape_flag_path(inc)` ❌ **只做 ninja `$` 转义，无 shell 引号** |

```cpp
// ninja_backend.cppm:108
std::string local_include_flags(const CompileUnit& cu, bool msvcDialect) {
    for (auto const& inc : cu.localIncludeDirs) {
        flags += " -I";
        flags += escape_flag_path(inc);      // ← 只转义 ' ' '$' ':' 给 ninja
    }
```

`escape_flag_path`（`:85-99`）把空格转成 `$ ` 是**给 ninja 解析器看的**；ninja 反转义后交给 shell / rspfile 的就是裸空格。而 `[build] include_dirs` 确实会流进这条通道 —— `plan.cppm:883`：

```cpp
main_cu.localIncludeDirs = local_include_dirs_for_manifest(projectRoot, manifest);
```
（`local_include_dirs_for_manifest` 读的正是 `manifest.buildConfig.includeDirs`；另一条 `plan.cppm:878` 走 `packages[0].privateBuild.includeDirs`，同样进这个函数。）

于是 `C:/Program Files (x86)/Windows Kits/10/Include/*/cppwinrt` →
`-IC:/Program` + `Files` + `(x86)/Windows` + `Kits/10/Include/*/cppwinrt`
—— 第一段被 `cl.exe` 当 include 目录吃掉，**剩下三段正是报告里那三行 `C1083: Cannot open source file`**。完全对上。

报告怀疑 `flags.cppm:238-242` 应该覆盖到 —— 那是**另一条通道**，所以对不上号。这正是"同一决策两处推导"的老毛病（见 `.agents/docs` 里 #242 / C3 两次同类事故）。

**并且这不是 Windows/MSVC 专属**：Linux 上 `include_dirs = ["/home/my dir/inc"]` 会以完全一样的方式裂开。

**修法**：`local_include_flags` 对每个 token 走和 `flags.cppm:241` 同一个 `shell_quote_arg(escape_flag_path(...))`；顺手把写死的 `-I` 换成 `d.includePrefix`（现在靠 `cl.exe` 恰好接受 `-I` 兜着）。3a 单独修：`expand_dir_glob` 支持绝对 glob，或对绝对路径按其 root 拆出 base 再展开。

**归类：A（真实且通用，跨平台）。** 两半都该修，3b 优先级更高（3a 只是没展开，3b 是**静默把 include 路径变成源文件**）。

---

## 4. `ldflags` 的文件输入 — 问题真实，**但报告的两个建议里只有一个能收**

事实核对：`src/build/flags.cppm:108-124` 的 `normalize_ldflag` 只重写两种前缀：

```cpp
if (flag.starts_with("-L") ...)            → 按 root 绝对化
if (flag.starts_with("-Wl,-rpath,") ...)   → 按 root 绝对化
return flag;                               // 其余原样
```

ninja 的 cwd 是 `target/<triple>/<hash>/`（`flags.cppm:216`、`:372` 两处注释确认），所以 `ldflags = ["gen/app.res"]` 必然 `LNK1181`。属实，而且**跨平台通用**：Linux 上 `ldflags = ["libfoo.a"]` 同样解析不到。

报告指出的不一致也属实：`docs/07-build-mcpp.md:49` 写着 `link-search` 的相对路径按工程根解析，代码 `build_program.cppm:131-132` 里 `abs_against_root` 确实这么做了；而 manifest 侧 `[build] ldflags` 没有任何说明。

**两个建议分开判：**

- ❌ **"让 `ldflags` 里长得像路径的条目按工程根解析"—— 不该做（C 类）。** 这是在 flag 字符串上做形状猜测，mcpp 已经在这条路上摔过一次：`join_flags`（`ninja_backend.cppm:134-166`）那一大段注释记的就是 "`shell_quote_arg` 假设每个 flag 元素 = 一个 argv token" 这个猜测怎么把 `-include foo.h` 和 `-Wl,-rpath,'$$ORIGIN'` 一起搞坏的。`ldflags` 是**逃生舱**，它的契约就是"原样进链接行"；给它加启发式重写，等于让 `-Wl,--version-script=foo.map`、`@rsp`、`-l:libfoo.a` 这些形状各自去猜。

- ✅ **"给 `[build]` 补 typed 字段"—— 该做（A 类）。** `link_inputs`（文件输入，按 package root 解析，进 `implicitInputs` 让 ninja 能跟踪重建）+ `link_search`（目录，和 `mcpp:link-search` 同语义）。typed 通道不需要猜形状，还顺带解决 ninja 依赖跟踪 —— 现在 `ldflags = ["../../../gen/app.res"]` 就算路径蒙对了，**改了 `.res` 也不会触发重链**。

  短期无成本的止血：在 `docs/05-mcpp-toml.md` 的 `ldflags` 处写清"原样传递、相对路径相对于 `target/<triple>/<hash>/`"，并给出 `link-search`/`-L` 的正确写法。

**归类：A（补 typed 字段 + 补文档）+ C（路径启发式该拒绝）。**

---

## 5. 文档里的 `[build] linkage` — 真实，**且比报告说的多一处**

代码事实：`linkage` 只在 `[target.<triple>]` 里解析（`src/manifest/toml.cppm:995-1001`），`[build]` 的白名单（`toml.cppm:907`）确实没有它，所以静默忽略 + warning。而 `buildConfig.linkage` 这个**字段本身存在**，由 `prepare.cppm:1016`（`[target]` 覆盖）、`:1026-1029`（target 默认 static / `--static`）、`:1304`（musl）写入，最后在 `flags.cppm:336` 决定 `/MT` vs `/MD`：

```cpp
msvc_base += (plan.manifest.buildConfig.linkage == "static") ? " /MT" : " /MD";
```

报告列的两处文档 bug 属实（`docs/03-toolchains.md:129` 和 `:183`），`docs/05-mcpp-toml.md:484` 的 `[target.*]` 归属是对的。

**报告漏了第三处**：`src/build/prepare.cppm:1303` 的注释也写着

```cpp
// out via [build].linkage / [target.<triple>].linkage.
```

—— 又一次"契约只写在注释里，且注释是错的"。

**报告用的绕行比必要的重**：`[target.'cfg(windows)'.build] cflags/cxxflags = ["/MT"]` 是能用，但既然 `x86_64-windows-msvc` 是精确 triple，直接写

```toml
[target.x86_64-windows-msvc]
linkage = "static"
```

就走了正规通道（也可以 `mcpp build --static`）。文档误导让人多绕了一圈，这本身就是这条 issue 的代价证据。

**"或者在 `[build]` 下把它实现成 `[target.*]` 的简写"—— 建议不该收。** `[build]` 是 target-agnostic 的（一份配置服务所有 target），`linkage` 是 target 属性 —— `docs/05-mcpp-toml.md:554` 已经把这条写成明文规则（"`toolchain` / `linkage` are exact-triple only"），`toml.cppm:1048/1066` 也按这条规则在报错。加简写会让 `[build] linkage = "static"` 在交叉编译时含义不明。

**归类：A（改三处文档 + 那条注释）。建议的实现部分拒绝。**

---

## 6. path 索引注册 —— 症状真实，**但报告的根因和代码对不上**

报告的机制描述是：

> mcpp 把 `[indices]` 物化进 `<project>/.mcpp/.xlings.json`，但真正解析包的那个沙盒 xlings 以 `$MCPP_HOME/registry` 为 home，它读的是 `~/.mcpp/registry/.xlings.json`，不看项目那份。

**代码不是这么走的。** 对非 builtin 索引，mcpp 走的是项目作用域：

```cpp
// src/build/prepare.cppm:1813
const bool useProjectEnv = idxSpec && !idxSpec->is_builtin();   // path 索引 → true
// :2027
auto projEnv = mcpp::config::make_project_xlings_env(**cfg, *root);
auto r = mcpp::xlings::call(projEnv, "install_packages", argsJson, &progress);
```

```cpp
// src/config.cppm:134-137
make_project_xlings_env(cfg, projectDir)
    → { cfg.xlingsBinary, cfg.xlingsHome(), projectDir / ".mcpp" };
                                            ^^^^^^^^^^^^^^^^^^^ XLINGS_PROJECT_DIR
```

`xlings.cppm:832-838` 在项目模式下**显式设置** `XLINGS_PROJECT_DIR`（全局模式才 `env -u`）。另外 `config.cppm:771-776` 的 `exposeLocalIndex` 还把本地索引 symlink/copy 进 `.mcpp/data/<name>` 和 `.mcpp/.xlings/data/<name>` 两处。

所以：**管道是通的、也确实被调用了**（`prepare.cppm:1457` 在依赖解析前调 `ensure_project_index_dir`）。报告观察到的 `searched repos: [xim, mcpplibs]` 说明 **xlings 侧没有把项目 repo 并进搜索集**，断点在 mcpp↔xlings 契约，不在 mcpp 的"注册落点"。

**最可能的实际断点（未实测，需要 Windows/CI 复现确认）**：`config.cppm:704` 对 path 索引写的是

```cpp
customRepos.push_back({ name, source.generic_string(), "", "" });
//                            ^^^^^^^^^^^^^^^^^^^^^^ 一个文件系统路径，写进了 `url` 字段
```

`seed_xlings_json`（`xlings.cppm:1198-1210`）把它当 `"url"` 发出去。xlings 若把 `index_repos[].url` 当 git remote 处理，`D:/a/SpinningMomo/SpinningMomo/mcpp` 这种值就可能被静默丢弃 —— 症状恰好就是"repo 列表里没有 `sm`"。

**顺带发现的一个真 bug（同族，未构成本次故障）**：`xlings.cppm:1104-1118`，`install_with_progress` 的 POSIX NDJSON 兜底分支把命令行**写死**成

```cpp
"cd {} && env -u XLINGS_PROJECT_DIR XLINGS_HOME={} {} interface install_packages ..."
```

—— 硬 unset `XLINGS_PROJECT_DIR`，无视传进来的 `env.projectDir`；而同一函数的**直接路径**用的是 `build_command_prefix(env)`（会正确设置）。同一个函数两条路径对 project scope 的处理相反。今天没炸是因为依赖安装走的是 `xlings::call` 而不是 `install_with_progress`，但只要有人把项目作用域的安装接到这个函数上就会静默降级到全局。

**为什么 e2e 没抓到**：`tests/e2e/52_local_path_namespaced_index.sh:41-47` **预先手工创建了已安装产物**：

```bash
mkdir -p "$TMP/project/app/.mcpp/.xlings/data/xpkgs/compat.cfg/1.0.0/src"
```

于是 `findCompleteInstalled()` 直接命中，**xlings 安装这一腿从来没被执行过**。`42_custom_local_index.sh` / `169_semver_project_index.sh` 同族。这就是"本地绿、CI 红"的结构性来源 —— 和报告观察到的现象同构，只是发生在测试里。

**报告的建议（"至少让诊断能对上"）完全正确，且比它自己以为的更有价值**：mcpp 的诊断（`prepare.cppm:2088-2094`）已经会按 `useProjectEnv` 去读对应的 `.xlings.json` 并列出 repo，但 xlings 的子进程错误文本（`searched repos: [...]`）是另一套，两者拼在一起才误导。修法应该是：install 失败时把 **mcpp 认为生效的 scope + XLINGS_PROJECT_DIR + 项目 `.xlings.json` 的 repo 列表 + xlings 自报的 repo 列表**四项并排打出来，差异一眼可见。

**归类：D（症状真实，根因误判）。** mcpp 侧确定该做的三件：
1. e2e 补一条**真的走安装**的 path 索引用例（不预置 xpkgs 目录）；
2. 诊断并排输出（上面那四项）；
3. `install_with_progress` 的 POSIX 兜底改用 `build_command_prefix(env)`，消掉同函数两条路径的分歧。

path 索引在 xlings 侧到底怎么被接受（`url` 里放路径是否合法、要不要新的 `path` 字段），需要开一条 openxlings/xlings 的 issue 定契约 —— 这条**不是 mcpp 单方面能修的**。

---

## 7. Windows 没有 MSVC 时的默认工具链

### 7.1 一般电脑默认有 MSVC STL 吗？—— **没有**

- Windows 10/11 自带的是 **UCRT 运行时 DLL**（`ucrtbase.dll`，OS 组件）。
- **头文件和导入库**（`ucrt/*.h`、`libucrt.lib`）来自 **Windows SDK**，不预装。
- **MSVC STL**（`<vector>` 等）只随 **Visual Studio / Build Tools** 的 "Desktop development with C++" 负载安装，不预装。
- 结论：裸装 Windows 上，**编译期一无所有**；只有运行期 CRT。

### 7.2 mcpp 今天在裸 Windows 上会怎样

```cpp
// src/toolchain/triple.cppm:148
inline constexpr std::string_view kFirstRunMacWin = "llvm@20.1.7";
```

首次 `mcpp build`（`prepare.cppm:1207-1222`）在 Windows 上自动装 `llvm@20.1.7`。而：

```cpp
// src/toolchain/clang.cppm:137-140
// Clang targeting MSVC uses MSVC STL, not libc++.
bool msvTarget = is_msvc_target(tc);
tc.stdlibId = msvTarget ? "msvc-stl" : "libc++";
```

README:311-313 也白纸黑字写了：

> On Windows, llvm requires an existing **MSVC BuildTools or Visual Studio** (UCRT, Windows SDK, MSVC STL). The MinGW route (`--target x86_64-windows-gnu`) needs no [Visual Studio].

**所以：默认路径在裸 Windows 上必挂，而能用的路径（`x86_64-windows-gnu`，winlibs GCC，完全自包含）就在旁边，只是不是默认。**

更糟的是**没有诊断**。`prepare.cppm:1284-1295` 只对 `CompilerId::MSVC && envOverrides.empty()`（检测到 VC tools 但缺 SDK）给了引导文案。clang-targeting-msvc 在完全没有 VS 的机器上，mcpp 不作任何检查 —— 用户拿到的是 clang 自己的 `'vector' file not found` 或 `unable to find a Visual Studio installation`，从这里推不出"该换 `--target x86_64-windows-gnu`"。

### 7.3 该不该加 clang + libc++ 并设为 Windows 默认？—— **不该**

分两种 "clang + libc++ on Windows"：

**(a) libc++ 配 MSVC ABI（`x86_64-pc-windows-msvc` + libc++）**
- LLVM 官方**不发布** Windows 的 libc++ 二进制；这个组合在上游是 experimental，要自己 build，且 locale / 线程 / 异常几块长期有缺口。
- 更硬的问题是 **ABI 隔离**：libc++ 的 `std::string`/`std::vector` 和 MSVC STL 不兼容，一旦选它，vcpkg 的 MSVC 预编译包、任何第三方 `.lib`、系统 SDK 里跨 `std::` 类型的接口全部不能链。
- 而且它**并不解决问题**：MSVC ABI 依然需要 Windows SDK 的 `ucrt`/`um`/`shared` 头和 import lib，裸机器上照样没有。
- **判定：不真实可行的方案。**

**(b) llvm-mingw（clang + libc++ + lld + libunwind，`x86_64-windows-gnu`）**
- 这个是**真的能用**、真的自包含。
- 但它和 mcpp 已有的 **winlibs GCC（`x86_64-windows-gnu`，libstdc++ + `import std`，全静态自包含）功能等价** —— 同一个 target、同一个 ABI、同一类产物。
- 加它 = 给同一个 target 加第二套 stdlib 实现，换来的边际收益（clang 诊断、libc++）远小于成本（多一条 payload 发布线、多一组 `import std` 路径、多一个 BMI 缓存维度、CI 多一轴）。
- **判定：真实但优先级极低，不该为了"裸 Windows 能构建"而做 —— 那个洞已经被 winlibs GCC 填上了。**

### 7.4 真正的问题是**默认值选错了**，不是缺工具链

Windows 上正确的首次运行策略应该是 **detection-first**（mcpp 在 `msvc@system` 上已经用过这个先例，`lifecycle.cppm:629-651`）：

```
mcpp build（Windows，无配置）
  ├─ 探测到 VS/BuildTools + Windows SDK ？
  │    是 → msvc@system（或 llvm@20.1.7，二选一按策略定）
  │    否 → gcc@16 + --target x86_64-windows-gnu（winlibs，自包含，零 VS 依赖）
  └─ 无论哪条，打印一行说明选了什么、以及怎么切换
```

这样：
- 有 VS 的机器行为**不变**（今天的 `llvm@20.1.7` 或 msvc）；
- 裸机器**从"必挂且看不懂"变成"直接能用"**；
- 不需要任何新工具链、新 payload、新 CI 轴 —— 全部零件（winlibs payload、target 解析、`kFirstRunMacWin` 常量、msvc 探测）都已存在。

**配套的最小诊断**（独立于上面，单独也有价值）：在 `prepare.cppm` 已有的 MSVC-缺-SDK 检查旁边，补一条 —— clang 且 `is_msvc_target(tc)` 且探不到 MSVC STL/SDK 时，直接失败并给出

```
llvm on Windows targets the MSVC ABI and needs Visual Studio / Build Tools
(UCRT + Windows SDK + MSVC STL), which was not found.

  • install:  winget install Microsoft.VisualStudio.2022.BuildTools
  • or switch to the self-contained MinGW route (no Visual Studio needed):
      mcpp toolchain default gcc@16 --target x86_64-windows-gnu
```

**归类：A（真实且通用）—— 但落点是默认选择策略 + 诊断，不是新工具链。**

---

## 8. 建议的落地顺序

按 (影响面 × 修复成本) 排：

| 序 | 动作 | 成本 | 说明 |
|---|---|---|---|
| 1 | `local_include_flags` 补 shell 引号 + 用 `d.includePrefix`（#3b） | 极小 | 静默把 include 路径变成源文件，跨平台，含空格路径必中 |
| 2 | Windows 首跑 detection-first + clang-无-MSVC 诊断（#7） | 小 | 裸 Windows 从"必挂"变"能用"，零新组件 |
| 3 | 文档三处 + `prepare.cppm:1303` 注释改掉 `[build] linkage`（#5） | 极小 | 纯文档，但直接造成了报告人的错误绕行 |
| 4 | `msvc.cppm` INCLUDE 加 `cppwinrt`（存在时）（#2a） | 极小 | 一行；C++/WinRT 项目开箱可用 |
| 5 | `command_from_argv` 的 Windows `argv[0]` 引号（#1a） | 中 | 需在 Windows 上实测 `cmd.exe /c` 的引号行为；影响面超出 MSVC |
| 6 | `build.mcpp` 编译/指令面走 `CommandDialect`（#1b） | 大 | 没有这个，#1a 修了 MSVC 上的 `build.mcpp` 依然不可用 |
| 7 | `[build] link_inputs` / `link_search` typed 字段（#4） | 中 | 顺带修好重链跟踪；同时补 `ldflags` 的 cwd 说明 |
| 8 | path 索引：真安装 e2e + 并排诊断 + `install_with_progress` 兜底修正（#6） | 中 | 契约那半要开 openxlings/xlings issue |
| 9 | 绝对路径 `include_dirs` 的 glob 展开（#3a） | 小 | #3b 修完后这个只是"没展开"，不再是错误落点 |
| — | ~~`[build] linkage` 实现成简写~~ | — | **拒绝**：违反 `[build]` target-agnostic 规则 |
| — | ~~`ldflags` 里"像路径"的条目按 root 重解析~~ | — | **拒绝**：flag 字符串形状猜测，`join_flags` 注释里记着上次教训 |
| — | ~~mcpp 内置 `cppwinrt.exe` 代码生成~~ | — | **拒绝**：属 `build.mcpp`，#1 修好即可 |
| — | ~~Windows 加 clang + libc++ 并设为默认~~ | — | **拒绝**：MSVC-ABI 版不可行；mingw 版与已有 winlibs GCC 重复 |

**其中 #1b 和 #7 是本次真正的架构性发现**：
- `build.mcpp` 整条通道从未接入 0.0.89 就建好的 `CommandDialect`，MSVC 支持从来就是**零**，只是被 #1a 的引号错误挡在前面看不见；
- Windows 的默认工具链和 Windows 上唯一零依赖可用的工具链，**是两个不同的东西**，而这个事实只写在 README 的脚注里。

**共同成因**：CI 的 Windows 面全跑在 `llvm@20.1.7` 上（`ci-windows-e2e.yml:70`），MSVC 只有一个不含 `build.mcpp` 的脚本。补 `MSVC × build.mcpp` 和 `含空格路径 × include_dirs` 两条 e2e，是让这批问题不复发的最小闸门。
