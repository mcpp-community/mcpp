---
subject: design
status: active
---

# MSVC toolset 的选择、#685、#687 与工具链管理规范：总体设计

- 日期：2026-09-24（v2，同日修订；与 v1 的差异见 §12）
- 依据：`2026-09-24-685-687-msvc-stl-and-toolchain-payloads.md`（下称「分析」）。分析里已经论证过的内容，本文只引用。
- 规范：`docs/specs/toolchain-management.md`（SPEC-006，草案 v0.1），与本文同时写成。
- 基线：mcpp `origin/main` b30e70c4
- 状态：待 review。

## 0. 维护者的决定

| 事项 | 决定 |
|---|---|
| 已安装 gcc 13.3.0 / 15.1.0 的用户 | 不主动通知；只在 doctor 里诊断 |
| Windows 默认工具链 | 不变 |
| MSVC 再分发立场 | 保持现状 |
| 生态包的标记 | 用 `xim:` 前缀表示只取生态包 |
| STL 对 clang 的版本下限 | 不做预检查，等编译期的 `STL1000` 报错 |
| `cl.exe` 行的边界变化 | 只要比现在更好，就接受 |
| 载荷规范化 | 先写一份核心 spec，暂不实现；等 LLVM 发新版本、一起加入新工具链时，再实现、验证和调整。配方与构建 CI 放在哪个仓库，以后再定 |

## 1. 摘要

| 部分 | 设计要点 | 仓库 | 时机 |
|---|---|---|---|
| A. MSVC toolset 的选择 | 在 MSVC ABI 目标上，**编译器是工具链，MSVC toolset 是 sysroot**。clang 行用 `[target.x86_64-windows-msvc] sysroot = "msvc@…"` 指定；带版本时先匹配机器上的 toolset，`xim:` 前缀表示只取生态包。mcpp 只选一次，把结果显式传给 clang，同时打印并记录 | mcpp | review 通过后 |
| B. #685 | 计算 deployment target 时不再看宿主；是否适用按目标判定 | mcpp | 先做 |
| C. #687 | 修正 `gcc.lua` 的引号并补测试；mcpp doctor 给出诊断 | xim-pkgindex、mcpp | 先做 |
| D. 工具链管理规范 | 写成 SPEC-006 草案；实现跟随下一批 LLVM 工具链 | mcpp（spec），生态仓库（实现，待定） | spec 现在写，实现以后做 |

## 2. 原则

1. **一个问题只有一个回答者。** 选择在 prepare 阶段做一次，结果存进 `Toolchain`，所有读者读这个结果。
2. **声明压过探测，探测的结果必须看得见。**
3. **目标的属性按目标判定。** 只有在宿主上运行的东西按宿主判定。
4. **每个修复都要有判据：把修复撤回，判据就变红。**

---

## 3. A 部分：MSVC toolset 的选择

### 3.1 语义：编译器是工具链，MSVC 是 sysroot

v1 给 clang 行设计的是一个叫 `msvc` 的新键，读起来分不清这一行到底是 clang 还是 msvc。问题的根源是把两条轴混在了一起：

- **工具链轴**：由谁来编译。可以是 `cl.exe`，也可以是 clang。
- **目标轴**：对着什么编译。三元组 `x86_64-windows-msvc` 的第三段本身就说明了这一点：对着 MSVC 环境编译，包括 STL、CRT、UCRT 和 Win32 SDK。

mcpp 已经有目标轴上现成的写法：`[target.<triple>].sysroot`。SPEC-004 §4.1 把它定义为目标轴的条目，含义是「被编译对着的东西」。LLVM 也用同样的名字称呼这个目录：clang-cl 的 `/winsysroot`，以及 GNU 驱动的 `-Xmicrosoft-windows-sys-root`。所以：

```toml
[toolchain]
windows = "llvm@22.1.8"            # 编译器：clang

[target.x86_64-windows-msvc]
sysroot = "msvc@14.44.35207"       # clang 对着编译的 MSVC 环境
```

读法是「编译器 llvm 22，sysroot 为 MSVC 14.44」。这与 Linux 上「编译器 gcc，sysroot 为 glibc」是同一个结构。

`cl.exe` 行没有第二个选择：编译器本身就是 toolset，它的 sysroot 就是它自己。

```toml
[toolchain]
windows = "msvc@14.44.35207"       # 编译器与 sysroot 是同一个 toolset
```

`cl.exe` 行如果另外写了 `sysroot`，并且与编译器不是同一个 toolset，就拒绝构建。

### 3.2 写法

同一套写法用在两个位置：`cl.exe` 行的工具链字符串，和 clang 行的 `sysroot`。

| 写法 | 含义 |
|---|---|
| `msvc@system`；clang 行不写 `sysroot` 时等同于此 | 机器上的默认 toolset，顺序见 §3.4 |
| `msvc@14.44.35207` | 先在机器上找目录名与之完全相同的 toolset；找不到就用生态包，未安装时自动安装 |
| `msvc@14.44` | 按版本分量做前缀匹配。机器上有就取匹配的最高版本，没有就取生态包里匹配的最高版本 |
| `xim:msvc@14.44.35207` | 只取生态包，不看机器 |

规则：

- **前缀按分量匹配。** `14.4` 匹配 `14.4.x`，不匹配 `14.44`。「最高」按数字元组比较，不按字符串比较。今天的 `find_latest_msvc_tools()` 用的是字符串比较。
- **`xim:` 用在 `sysroot` 里**，就是这个键现有的 xpkg 引用语法（`<namespace>:<name>@<version>`）。不带命名空间的 `msvc@…` 是新增的形式，只在 `*-windows-msvc` 行上有效。
- **`xim:` 用在工具链字符串里**，需要解析器支持去掉这个前缀。gcc、llvm 等族没有系统来源，对它们来说 `xim:` 与不带前缀等价。
- **只有 MSVC 有系统来源。** `gcc@system` 继续被拒绝。

「先匹配系统」之所以可行：完整版本号对应微软的同一次构建，`xim:msvc` 解包的是 VS 自己的 channel manifest 里的同一批 vsix，所以两种来源的 toolset 内容按构造相同。这一点尚未逐字节核对，放在 §3.10 的判据里。

### 3.3 来源不同带来的差异

| 差异 | 处理 |
|---|---|
| SDK 跟随来源：生态包用载荷自带的 `windows-sdk`，系统来源用机器扫描的结果 | 沿用 `resolve_sdk_for()`。SDK 版本以 `ucrt@<ver>` 进入缓存键，并在构建开头打印。要连 SDK 一起固定，用 `xim:` |
| 部分版本在不同机器上可能解析到不同的补丁版本 | 打印解析出的完整版本；文档建议 CI 写完整版本，或写 `xim:` |
| `msvc@<版本>` 过去一律用生态包，现在机器上有同版本就用机器的 | 写进迁移说明（§3.8） |
| 机器上匹配到的 toolset 不完整：缺 `lib/x64`；需要 `import std` 时缺 `modules/std.ixx`；`cl.exe` 行缺 `cl.exe` | 不完整的不算匹配，跳过并打印一行说明，然后回落到生态包 |

### 3.4 候选集合与解析算法

**系统候选。** 取以下来源的每个实例：

- `vswhere -all -prerelease -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -format json -utf8`；
- `VSINSTALLDIR` / `VCINSTALLDIR` 指向的实例；
- 没有 vswhere 时，扫描固定路径。

每个实例再展开其下的 `VC/Tools/MSVC/<dir>`，记下实例路径、toolset 版本、产品名，以及它是否为 `VC/Auxiliary/Build/Microsoft.VCToolsVersion.default.txt` 指定的默认版本。加 `-utf8` 是因为用户名里可能有非 ASCII 字符。

**生态包候选。** 已安装的 `xim-x-msvc/<v>`，以及索引里可安装的版本。

**解析函数 `select_toolset()` 是纯函数。** 输入是候选集合、环境变量快照、写法，以及本次构建的需求（是否要 `cl.exe`、是否要 std 模块）。输出是 `MsvcSelection`（来源、toolset 目录、版本、实例与产品名、SDK、说明文字），或者一个列出全部候选的错误。只有枚举候选这一步依赖 Windows。

**`msvc@system` 按顺序取第一个完整的候选：**

1. `VCToolsInstallDir` 指向的 toolset；
2. `VSINSTALLDIR` / `VCINSTALLDIR` 所指实例的默认 toolset；
3. `PATH` 上 `cl.exe` 所在的 toolset；
4. 带 C++ 组件、且 `installationVersion` 最高的实例的默认 toolset；
5. 固定路径扫描（只在没有 vswhere 时）。

这就是 clang 驱动和 mcpp 两条现有选择链合并后的顺序。

**带版本的写法：**

1. 在系统候选里找完整且匹配的，取最高的；
2. 找不到就在生态包候选里找（`xim:` 写法直接从这一步开始），已安装的优先，其次从索引安装；
3. 仍然没有就报错，列出三类候选。

写了版本时，`VCToolsInstallDir`、`VSINSTALLDIR`、`WindowsSdkDir` 都不参与选择；如果它们本来会选出别的结果，打印一行说明。

**结果只算一次。** 在 prepare 里算出，存进 `Toolchain` 的只读字段：`msvcToolsDir`、`msvcToolsVersion`、`msvcOrigin`、`windowsSdkRoot`、`windowsSdkVersion`，写法与 `appleSdkRoot` 相同。

**原生构建也读得到。** `sysroot` 按实际构建目标去查目标行（`src/build/prepare.cppm:1371-1391` 的 `find_target_entry` / `sysroot_override`，调用处在 `:10682`），所以在 Windows 上直接 `mcpp build` 也会命中 `[target.x86_64-windows-msvc]`。目标行上的 `toolchain` 则只按 `--target` 去查（`:2801`）。

build.mcpp 在 Windows 宿主上的宿主编译，读的是宿主三元组那一行，也就是同一行。

### 3.5 交给编译器

**`cl.exe` 行。** 机制不变，仍由 `build_env_for_cl()` 合成 `INCLUDE` / `LIB`，只是输入改为 `MsvcSelection`。

**clang 行。**

- **注入位置。** `modules/toolchain-model/src/linkmodel.cppm:373` 目前对 MSVC 目标返回空模型，改为返回三组 token，每组都是分开的两个参数：
  - `-Xmicrosoft-visualc-tools-root <toolsDir>`
  - `-Xmicrosoft-windows-sdk-root <sdkRoot>`
  - `-Xmicrosoft-windows-sdk-version <sdkVer>`

  这些 token 经由已有的读者到达各处：
  - `host_compile_tokens`（`src/toolchain/hostflags.cppm:247`），它服务普通编译、std 模块预编译和 build.mcpp 宿主编译；
  - 缓存键（`src/build/cache_key.cppm:499`）；
  - 链接一侧（`hostflags.cppm:581` 的 `host_link_tokens`，以及 `src/build/flags.cppm:1684` 起的 PeLld 分支）。

  MinGW 分支继续返回空模型。
- **scan-deps。** 它读到的是否就是这条编译命令，实现时要核对。
- **`std.ixx`** 从 `<toolsDir>/modules/std.ixx` 取（`src/toolchain/clang.cppm:202-217`）。`stdlibVersion` 改记 toolset 版本（`clang.cppm:189`）。
- **toolset 和 SDK 必须一起传。** 否则 clang 会放弃 `%INCLUDE%`，SDK 退回注册表里的最高版本。
- **链接命令的环境里去掉 `LIB`**，这样就关上了 `lld-link` 的剩余路径（`lld/COFF/Driver.cpp:874`）。
- **路径含空格。** 系统 toolset 一定在 `C:\Program Files\…` 下，token 必须经过现有的转义渲染。

这组参数都是 `/vctoolsdir` 等选项的别名（`Options.td:9513-9521`，llvmorg-22.1.8）。它们的路由效果已经在本机实测过（分析 §3.4）。

### 3.6 记录与可见性

- **构建开头打印一行**，例如：`MSVC  14.44.35207 (system: Visual Studio 2022 Community) · Windows SDK 10.0.26100.0 (machine)`。
- **`resolution.json`** 增加两个字段：
  - `msvc_toolset`：`version`、`origin`、`root`、`product`；
  - `windows_sdk`：`version`、`root`、`origin`。

  runtime identity 为 `msvc-stl@<v>` 加 `ucrt@<sdk>`。
- **目标侧报告。** c++ 层报告为 `msvc-stl`，c-abi 层报告为 `ucrt`，来源都是这个 sysroot。目前 sysroot 只接到 c-abi 层（`modules/manifest/src/targetside_model.cppm:716-718`），在 MSVC 目标上要同时接到 c++ 层。
- **`mcpp toolchain list`** 在 Windows 上列出机器上的 toolset。
- **兼容性检查。** 不做 STL 下限的预检查，由编译期的 `STL1000` 报错。

### 3.7 诊断

```
error: msvc@14.38 matches no toolset on this machine or in the index
  on this machine:   14.44.35207  Visual Studio 2022 Community (default)
                     14.51.36014  Visual Studio 18 Insiders (default)
  installed by mcpp: 14.52.36629
  in the index:      14.44.35207, 14.52.36629
```

```
note: sysroot msvc@14.44.35207 is pinned; VCToolsInstallDir (14.38.33130) is ignored
```

其他情形：

- 写的是 `cl` 的版本号（例如 `19.44`）：沿用 `cl_version_spelling_hint()`。
- `cl.exe` 行同时写了 `toolchain = "msvc@A"` 和 `sysroot = "msvc@B"`，A 与 B 不同：拒绝构建。

### 3.8 兼容与迁移

- **旧引擎读到 `sysroot = "msvc@…"`**（不带命名空间）时，按现有解析规则会整份清单报 `is not an xpkg reference`（`modules/manifest/src/toml.cppm:3229-3237`）。失败是响亮的，但报错文字会把人引向 xpkg 语法。
- **旧引擎读到 `sysroot = "xim:msvc@…"`** 时会接受。它接下来做什么没有测过，必须用已发布的上一版引擎实测：是忽略，还是把它当成 C 库去安装。
  - 实现时的代码阅读结论（2026.9.21.3 起这段代码没有变过）：旧引擎把这个值当作 xpkg 引用，经 `targetSysroot` 放进项目环境的依赖里安装（`src/build/prepare.cppm` 中 `materializeRootRuntime` 那一段），并把它报告为 xpkg 来源的 c-abi 层；编译仍由 clang 自行探测，不使用它。也就是说，旧引擎会多下载一份 toolset，结果与不写这个键相同，不会报错。这一点只能在 Windows 上实测，没有做。
- **最低版本只能写在文档里。** 项目清单里没有「最低引擎版本」这个键（未找到），所以在 `docs/20` 里写明新写法需要的 mcpp 版本。
- **行为变化。** `msvc@system` 除 §3.9 的边界情形外结果不变。`msvc@<版本>` 在装有同版本 VS 的机器上改用系统的那一份，这一条写进 `docs/20-toolchains.md` 和 CHANGELOG。

### 3.9 与今天不同的边界情形

维护者的决定是「更好就接受」。每一条都说明了为什么更好：

| 情形 | 今天 | 设计之后 | 更好的原因 |
|---|---|---|---|
| `cl.exe` 行，实例里另装了一个比默认更新的 side-by-side toolset | 取目录名最大的 | 取 `default.txt` 指定的 | 与 VS 自己的 vcvars / MSBuild 默认一致；更新的 side-by-side 往往是预览版 |
| `cl.exe` 行，在 `vcvarsall -vcvars_ver=X` 的环境里 | 忽略 `VCToolsInstallDir` | 使用 X | 声明压过探测，与 `VSINSTALLDIR` 优先的既有决定（xrgui#3）同一原则 |
| clang 行，`std.ixx` 与头文件来自不同 toolset | 可能发生 | 不会发生 | 两者一致是正确性要求 |
| 最新的实例没有 C++ 组件 | clang 找不到标准头 | 跳过该实例 | 今天这种情况必然失败 |
| 系统 SDK 里版本最高的不完整 | clang 仍取它 | 取最高的完整版本 | 避免 LNK1104 这类问题（三轴设计里记录过） |
| 版本比较 | 字符串比较 | 数字元组比较 | `14.9` 与 `14.10` 这类情形用字符串比较会排错 |

### 3.10 测试与判据

**单测（在 Linux 上，被测对象是 `select_toolset()` 纯函数）。** 在临时目录里造出以下环境：两个实例、side-by-side toolset、`default.txt`、一个不完整的 toolset、一个生态包 store，以及几份环境变量快照。覆盖的用例：

- 完整版本命中系统；
- 前缀按分量匹配（`14.4` 不匹配 `14.44`）；
- 回落到生态包；`xim:` 跳过系统候选；
- 找不到时报错并列出三类候选；
- 写了版本时忽略环境变量并打印说明；
- `msvc@system` 按五步顺序选择；
- 不完整的候选被跳过；
- `cl.exe` 行的 `toolchain` 与 `sysroot` 冲突时拒绝。

反向：去掉「先匹配系统」这一步，「完整版本命中系统」的用例必须变红。

**Windows e2e。** 用 runner 自带的 VS，再装上 `xim:msvc` 14.44.35207 和 14.52.36629：

1. `sysroot = "msvc@14.52.36629"`（runner 上没有这个版本）：`-v` 只显示生态包路径，`origin = managed`。
2. 写 runner 上 VS 的完整版本：用系统路径，不触发安装，`origin = system`。同时对比这一份和生态包里同版本的 `include/yvals_core.h`、`lib/x64/msvcprt.lib` 的哈希。
3. `sysroot = "xim:msvc@<runner 上已有的版本>"`：走生态包，`origin = managed`。
4. 在 `vcvarsall -vcvars_ver=<另一个版本>` 的环境里重跑第 1、2 条：结果不变，并打印说明。
5. 不写 `sysroot`：`std.ixx` 与头文件出自同一个 toolset 目录。
6. 路径含空格：第 2 条本身就覆盖了。

**旧引擎：** 用已发布的上一版 mcpp 分别跑两种写法，记录它的实际行为（§3.8）。

### 3.11 代码改动点

| 位置 | 改动 |
|---|---|
| `src/toolchain/msvc.cppm:329-468` | 新增候选枚举和 `select_toolset()`；`find_vs_install_path()` 这条链改为调用它 |
| `src/toolchain/registry.cppm:519-570`（`parse_toolchain_spec`） | 支持 `xim:` 前缀 |
| `src/toolchain/registry.cppm:1192-1200` | 带版本的 msvc 写法，来源由解析结果决定，不再单凭拼写判定 |
| `src/build/prepare.cppm:3520-3528` | 带版本的 msvc 写法先走 `select_toolset()` |
| `modules/manifest/src/toml.cppm:3229-3237` | `*-windows-msvc` 行的 `sysroot` 接受 `msvc@…` |
| `modules/manifest/src/targetside_model.cppm:716` | MSVC 目标上 sysroot 同时供给 c-abi 与 c++ 两层 |
| `modules/toolchain-model/src/model.cppm:234` 附近 | 在 `Toolchain` 上新增只读字段 |
| `modules/toolchain-model/src/linkmodel.cppm:373` | MSVC 目标返回 `-Xmicrosoft-*` token |
| `src/toolchain/clang.cppm:186-217` | `std.ixx` 与 `stdlibVersion` 取自 `MsvcSelection` |
| `src/build/prepare.cppm:14967` | `resolution.json` 新增字段 |
| `src/toolchain/lifecycle.cppm` | `toolchain list` 列出系统 toolset |
| 文档 | `docs/20-toolchains.md`、`docs/22-target-side.md`（`sysroot` 一节）、`docs/04-mcpp-toml.md`，以及 `docs/zh/` 对应译文；SPEC-006 的实现状态 |

### 3.12 不做

- SDK 版本键；
- 改变没有 VS 的机器上的默认工具链；
- Linux 宿主交叉编译到 `x86_64-windows-msvc`；
- llvm × toolset 的默认配对数据；
- STL 下限预检查。

---

## 4. B 部分：#685

- **解析与宿主无关。** `modules/platform/src/macos/macos.cppm:105-116` 去掉 `#if defined(__APPLE__)`，优先级为 env > manifest > 14.0。
- **各读者按目标判定：**
  - 三元组：`prepare.cppm:1933`；
  - 指纹：`prepare_inputs.cppm:536` 改为读 `min_platform_version()` 的结果；
  - 平台事实：`prepare.cppm:8707`；
  - `-mmacosx-version-min`：`hostflags.cppm:513`；
  - build.mcpp 宿主编译（`build_program.cppm:1205`）保持按宿主判定。
- **判据：**
  - 单测：解析结果与宿主无关；
  - Linux 宿主上的纯计划测试（`--target aarch64-macos`，版本 `11.0`）：`build.ninja` 里出现 `arm64-apple-macos11.0`；改值后指纹变化；env 优先于 manifest；
  - 反向：macOS 宿主构建 `x86_64-linux-musl`，命令里不出现 `-mmacosx-version-min`。

## 5. C 部分：#687

**xim-pkgindex：**

- `pkgs/g/gcc.lua:230-232` 改为只有一层引号，且不带 `|| true`：

  ```lua
  local out = os.iorun(string.format("grep -rlF %s %s",
                                     __shq(banner), __shq(root)))
  ```

  `os.iorun` 的两个实现都经过 `/bin/sh`，零命中时也都不报错。executor 实现会在命令末尾追加 ` 2>/dev/null > "<tmp>"`，这个重定向只作用于 `|| true` 里的 `true`，grep 的输出就不会被捕获（实测）。
- 每次都写一行日志，包含搜索的根目录和删除的数量，数量为零时也写。
- 修正注释里与实现不符的两处。
- `tests/g/test_gcc.py` 断言：安装 13.3.0 或 15.1.0 之后，`include-fixed` 下没有带横幅的文件。撤回修正后这条断言要变红。
- 顺带核对 11.5.0 和 9.4.0。

**mcpp：**

- 只在 `mcpp self doctor` 里加一项检查：已安装 gcc 的 `include-fixed` 里有带 fixincludes 横幅的文件时，列出文件、横幅里的源路径，以及重装命令（`mcpp index update`，然后 remove + install）。
- 判据：用 15.1.0 载荷的副本触发，用 16.1.0 不触发。

## 6. D 部分：工具链管理规范

写成 `docs/specs/toolchain-management.md`（SPEC-006，草案 v0.1）。每一条都标注了实现状态。内容：

1. 身份与写法，包括 `xim:` 前缀和 MSVC 的系统来源；
2. 来源与选择：一次选择、声明优先、结果可见、MSVC ABI 目标的 sysroot、目标属性按目标判定；
3. 载荷契约：可重定位、不含构建环境的 C 库内容、安装时的改写限于清单、完整性、描述文件的来源字段、修订与资产名；
4. 构建：配方入库、固定的构建环境、可复现等级；
5. 验收：载荷 lint、兼容矩阵、准入门、已安装载荷的诊断；
6. 发布顺序。

spec 以已有机制为基础，而不是另起一套：

- `docs/32` 的描述符规则；
- `docs/91` §4 的安装后修正管线与 `.mcpp-fixup.json`；
- `.mcpp-toolchain.json`；
- xim-pkgindex 的准入脚本 `verify-toolchain.sh`；
- CI 工作流 `toolchain-consumer-smoke.yml`。

spec 也写明了现有的准入门为什么没能拦住 #687：

- 准入门不在 CI 里运行，默认使用 glibc 2.39，测试程序也不含线程头；
- smoke 工作流只测描述符改动后解析出的那一个版本（latest）。

实现（lint、矩阵、配方）跟随下一批 LLVM 工具链一起做，到时候按 spec 实现、验证，并据实测调整 spec。

## 7. 实施顺序

1. xim-pkgindex：修 `gcc.lua` 并补测试（C 部分）。
2. mcpp：#685（B 部分）。
3. mcpp：#687 的 doctor 诊断（C 部分）。
4. mcpp：MSVC toolset 选择（A 部分），在本设计 review 通过后开始，需要 Windows CI。
5. 生态：SPEC-006 的实现，跟随下一批 LLVM 工具链。

除纯文档外，所有改动都走 PR；合入后以 `origin/main` HEAD 上那次 run 为准。

## 8. 风险

| 风险 | 缓解 |
|---|---|
| 系统优先让不同机器用到不同的 SDK | 打印并记录 `ucrt@<ver>`；需要固定时用 `xim:` |
| 旧引擎对 `sysroot = "xim:msvc@…"` 的行为未知 | 发布前用上一版引擎实测（§3.10） |
| vswhere 的 JSON 输出有变化 | 解析失败时回落到 `-latest` 链并打印说明 |
| 系统 toolset 与生态包内容相同这一点未逐字节验证 | e2e 第 2 条核对 |
| `linkmodel` 的 PE 分支变成非空，影响 MinGW | MinGW 继续返回空模型，单测断言 |

## 9. 自审

我对 v1 做了自审，找到的问题和处理如下：

| # | v1 的问题 | 处理 |
|---|---|---|
| 1 | clang 行的 `msvc` 新键把编译器轴和目标轴混在一起，读起来分不清是 clang 还是 msvc | 改用目标轴已有的 `sysroot`（§3.1） |
| 2 | v1 设想新键「与 `toolchain` 一样」，但目标行上的 `toolchain` 只在交叉构建时读取（`prepare.cppm:2801`），照这个样子做，Windows 上直接 `mcpp build` 时就读不到 | `sysroot` 按实际构建目标去查，原生构建也生效（§3.4 末尾） |
| 3 | v1 说旧引擎对新键「只警告」 | 对 `sysroot = "msvc@…"`，旧引擎会拒绝整份清单；`xim:` 形式的行为未知，列为发布前的实测项（§3.8） |
| 4 | v1 的 `gcc.lua` 修正示例带 `|| true`，在 executor 实现下捕获为空 | 去掉 `|| true`，附实测（§5） |
| 5 | v1 说默认结果不变，但系统 SDK 的选择规则与 clang 不完全相同 | 加入边界表（§3.9） |
| 6 | v1 断言 scan-deps 会拿到这些 token，没有核对 | 改为实现时核对（§3.5） |
| 7 | v1 没有说明 build.mcpp 宿主编译怎样拿到同一个选择 | 宿主三元组那一行就是同一行（§3.4） |
| 8 | v1 的 D 部分没有读 xim-pkgindex 里已有的准入门和 smoke 工作流，等于把同一个决定又写了一遍 | spec 以它们为基础，并说明它们为什么没拦住 #687（§6） |
| 9 | v1 的前缀匹配和「最高」没有定义比较方式 | 按分量前缀匹配，按数字元组比较（§3.2） |
| 10 | v1 的 vswhere 调用没有处理编码 | 加 `-utf8`（§3.4） |
| 11 | v1 没有说明目标侧报告里 sysroot 供给哪几层 | MSVC 目标上同时供给 c-abi 与 c++（§3.6） |
| 12 | spec 初稿写的是「`ucrt@` 身份已实现」，没有区分行 | 改为 `cl.exe` 行已实现、clang 行未实现：e2e 241 为此专门钉了 `msvc@system` |
| 13 | spec 初稿把「副本加原子重命名」标为已实现 | 这只对 mcpp 的修正管线成立，xlings 安装时的改写没有核对过，改为部分实现 |
| 14 | spec 初稿说 smoke 工作流只测「一个版本」，没有说明是哪一个 | 读了原文：不带版本号安装，也就是 `latest`（16.1.0）。加清理逻辑的 PR（#563）正是在 16.1.0 上变绿的，而 16.1.0 本来就没有冻结文件，清理代码生效与否都是绿，这条绿什么也没有测到 |

## 10. 待 review 的决策点

1. **`sysroot` 的语义**（§3.1）：在 MSVC ABI 目标上，编译器是工具链，MSVC toolset 是 sysroot。clang 行写 `sysroot = "msvc@…"`，`cl.exe` 行不写。
2. **`xim:` 前缀接受的范围**：对所有工具链族都接受（没有系统来源的族等价于不写），还是只接受 msvc。建议全部接受，因为这是生态包的统一标记。
3. **SPEC-006 的范围与粒度**：选择规则和载荷契约放在同一份 spec 里，还是拆成两份。

## 11. 实现记录（2026-09-24）

实现时与本文前面各节不同的地方，以及原因：

| 位置 | 设计 | 实现 | 原因 |
|---|---|---|---|
| §3.5 链接环境 | 去掉 `LIB` | 不去掉 | `envOverrides` 作用于整个 ninja 进程；用户可能用 `LIB` 提供自己的库目录，清掉它有兼容性风险。显式 `-libpath` 排在前面，剩余路径只在显式路径缺库时才会用到 |
| §3.4 写了版本时的 SDK | `WindowsSdkDir` 不参与 | 系统来源的 toolset 仍按机器扫描选 SDK，`WindowsSdkDir` 优先 | 沿用三轴设计对 `msvc@system` 的决定：机器上的东西只能靠查找，声明压过扫描。版本进入 `ucrt@` 身份和缓存键，所以结果可见 |
| §3.6 目标侧报告 | c++ 与 c-abi 两层报告来源为 sysroot | 两层仍记为预制来源（SPEC-002 不变），所选 toolset 与 SDK 写入 `resolution.json` | 不改 SPEC-002 的层语义；`resolution.json` 已经给出同样的信息 |
| §3.7 cl.exe 行的冲突 | 拒绝 | 已实现：cl.exe 行的 `sysroot` 若指向另一个 toolset，报错并说明两种做法 | — |
| 新增 | — | `mcpp toolchain default msvc@<toolset>` 在机器已有该 toolset 时直接写入默认值，不要求先装载荷 | 与构建对同一写法的理解保持一致 |
| 新增 | — | `flags.cppm` 的路径转义：含空白的路径加引号，其余路径的写法逐字节不变 | 系统 toolset 总在 `C:\Program Files` 下；此前经这条通道的路径都不含空格 |
| 新增 | — | std 模块命令的路径转义：Windows 用平台引号，POSIX 保持单引号 | Windows 经 cmd.exe 执行这条命令，cmd.exe 不认单引号 |
| 新增 | — | cl.exe 行不再从别的 toolset 借用 `std.ixx` | 同一个「两个选择器」问题在 cl.exe 行上的另一处 |
| e2e 239 | — | 改用 `xim:msvc@<toolset>` | 裸写法现在会先选 runner 上的同版本 toolset，测试就不再测载荷 |
| 新增 | — | `CompileUnit::providesModule` 从 `std::optional<std::string>` 改为 `std::string`（空串表示不是模块接口） | 第一轮 Windows CI 在 clang + MSVC STL 下报 `_SMF_control` 无匹配构造函数，报在 `plan.cppm` 的 `CompileUnit` 上；这是已知的那类问题：模块接口一有扰动就可能触发，删掉这个成员类型才能根除 |

Linux 上的零差异检查：用已发布的 2026.9.21.3 与本分支的二进制，在同一目录分别构建 `examples/01-hello` 与 `examples/04-workspace`，四份 `build.ninja` 除 mcpp 自身路径那一行外逐字节相同，指纹也相同。

## 12. 修订记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v1 | 2026-09-24 | 初版：`msvc` 新键；系统优先；D 部分只列路线 |
| v2 | 2026-09-24 | 按维护者决定改为 `sysroot` 语义，接受 `xim:`，取消 STL 下限预检查，C 部分只做 doctor，D 部分改为 SPEC-006 草案；并入 §9 的自审结果 |
