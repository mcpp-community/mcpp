---
subject: design
status: landed
---

# 一处来源，一种含义：工作空间的传递语义、生成输出、模块角色与首次运行（#786、#785、#778、#790）

- 日期：2026-10-10。状态：修订 6，已实施（mcpp 2026.10.10.1、xlings 2026.10.10.2、xim-pkgindex glibc 2.44.3 r4、mcpp-plugins）。
- 基线：`main` `b1652184`（2026.10.8.1）。
- 证据：本地 Linux x86_64，已发布的 mcpp 2026.10.8.1，gcc 16.1.0 与 llvm 23.1.3。
  复现脚本 [2026-10-10-one-source-one-meaning-repro.sh](2026-10-10-one-source-one-meaning-repro.sh)
  只打印观察结果，不做断言；下文 R1–R6 引用其中的用例。
- 外部 PR #787（#786）与 #779（#778）各自只修复报告中的一个实例，本文不以它们为基础（§15）。

**修订 6 的变化（实施时的结论）：**
- review 结论：Q1 X1–X3 全做；Q2 做 P1；Q3 按依赖关系等 xlings 发布；Q4 D29 用 `env LD_LIBRARY_PATH=… <tool>`；
  Q5 全部发布后由维护者在真机验收（O11）。
- X3 的钩子心跳为 10 s 后首次、之后每 30 s（不是每 10 s）；终端上的解包行同样在 10 s 后首次、之后每 30 s，
  NDJSON 的 `extract` 事件每多读 1% 一次。接口协议升到 1.7。
- P1 只重新打包 glibc（r4，两个架构，已发布到 GitHub 与 GitCode，逐字节核对）。gcc 16.1.0、musl-gcc 15.1.0
  不重新发布：X1 已能处理，新 revision 会让每个已安装的用户重新下载。P2 只检查一次变更新增的载荷 URL。
- 新增 X4（维护者报告）：交互式加入已运行的 SubOS 会话时，命令得到调用者的终端，fish 退出
  （`tcgetpgrp failed`）。改为在沙箱内为它分配 PTY 作为控制终端，调用者终端置 raw 模式并转发字节与窗口尺寸。
- xlings PR 651 另修一处竞态：proxy 会话结束时 supervisor 立即 SIGKILL bwrap，命令的退出码变成 137。
- 兼容模块 `mcpp.pm.compat.workspace_position` 由 `mcpp.manifest` 伞模块导出，而不是 `mcpp.pm.compat` 门面：
  它读 `Manifest`，而 `mcpp.manifest.types` 导入该门面。
- `KeyRegistry`（`mcpp.manifest.key_registry`）推导出：解析器的 `[build]` 与 `[target.<sel>.build]` 已知键、
  `[workspace.target.<sel>.build]` 接受的键、W7 的包表清单；单元测试核对它与 `kWorkspaceBuildKeys`、与分组键。
  `[target.<sel>]` 标量清单与文档中的键类别表仍由各自已有的测试守护。
- D7：构建程序的缓存仍是每个包一条记录；切换配置会重新运行程序，不会重新编译它。
- D28：以 argv 启动的 shell 由 `posix_shell()` 解析；`popen` 与 `std::system` 仍用 C 库的 `/bin/sh`，
  因此 Android 的基线是 10 及以上。
- D23：mcpp 把 xlings 的 `extract`、`hook` 事件显示为 `Installing …` 行，并写入日志；不另加 30 s 心跳。
- D26 以 `degraded`（`build-program/provides`）实现。
- 工作空间层的解析：`[workspace.*]` 下的文档由同一个 `parse_document` 读取，保留原值的行号。
- 测试：e2e 893–901（在 2026.10.8.1 上全部失败）；Windows 探测由单元测试覆盖；D27 的两个 job 在
  `ci-fresh-install.yml`（发布后），发布前在探测 PR #791 上以候选包运行。

**修订 5 的变化（review 结论与探测）：**
- Android 不换工具链：aarch64 仍用 llvm@23.1.3 + glibc，目标是真实跑通（D21 改为只识别、不改默认值）。
- §13 改写为跨仓库修复：xlings 的硬链接回退（X1）、失败分类与止损（X2）、阶段内进度（X3）；载荷重新打包（P1、P2）；
  mcpp 的 D28、D29、D22、D23、D27。
- 探测 round 6、7：Android 14 应用沙箱中，去掉硬链接的 llvm@23.1.3 + glibc 能运行，mcpp 2026.10.8.1 构建 `import std` 成功；
  最新的 xlings 2026.10.10.1 解包同样失败（xlings 自身缺陷）；arm64 runner 没有 KVM。
- 用户的日志证实 Planning 的 35 分钟全部在 xlings 安装 musl-gcc 之中（R4）。
- D30：xlings 固定到包含 X1–X3 的新版本，不是 2026.10.10.1（它没有 X1）。

**修订 4 的变化（review 结论与探测）：**
- D17 改写：工具链只要被明确指定过（清单、`--toolchain`、全局默认值），就永不自动换成 MinGW；
  只有什么都没指定时才可以改用 MinGW（§12.2）。
- §13 增加：LLVM 没有 musl 静态版本；Android 宿主的默认值与以后的静态 LLVM（O9）；Termux 的 CI（D27）。
- 临时探测 PR #791（五轮：run 37974353683、37975117448、37976918574、37977451853、37978204220）测量 O5、O6、O8，结果写入 §5.2、§12.1、§13（修订 5 时又跑了 round 6、7：run 37980130590、37980701891）。
- 探测发现 Android 上另外三个阻碍：mcpp 写死 `/bin/sh`（D28）、工具链的加载器环境泄漏给宿主 shell（D29）、
  termux-exec 的 linker 执行模式拒绝非 PIE 静态程序（O10）。硬链接问题不止 glibc：gcc、musl-gcc 的载荷同样含有。
- 新增 §14：mcpp 依赖的 xlings 升级到最新发布版本（D30）。

**修订 3 的变化（review 结论）：**
- O1：旧位置写法移入 `mcpp.pm.compat` 兼容模块，标注 `COMPAT(workspace-position)` 并给出警告，随兼容模块的退役边界一起移除（§3.6）。
- O2、O3、O4：按建议采纳，转为 D24–D26（§6）。
- O5：显式定义 `__cpp_impl_coroutine` 由用户负责，mcpp 不做保证；删除修订 2 的 G3 探测与失败说明的修改（§9）。
  MSVC 下各模块角色的参数仍需实测，因为 D8 依赖它（O5 收窄）。
- 新增 §11：mcpp 自身与生态（mcpp-plugins、openkal、xim-pkgindex、xlings）的同步更新清单。
- 新增 §12：Windows 上 Visual Studio 或 Windows SDK 不在 `C:` 时的探测与回退。
- 新增 §13：Termux（Android）上首次运行失败与 Planning 长时间无输出。

**修订 2 的变化：**
- 文件改名（原名 `2026-10-10-root-position-action-outputs-module-role-design.md`）。
- §2 起改写工作空间语义：以“前缀即语义”取代修订 1 中“`[target.<t>]` 标量行是环境表、
  `.build` 是包表”的划分。那种划分让同一个 `[target.<t>]` 表头下的两部分传递方式不同，
  是理解负担的来源。
- 键的类别登记表扩展到全部键（D4），整套语义在一个发布中完成，不分步。
- 新增：`--toolchain` 覆盖清单中的工具链时给出警告（§8）；显式启用 `__cpp_impl_coroutine`
  的写法与提示（§9）；`.xlings.json` 的引导版本（§10）。
- review 结论：保留多配置模型（成员可以声明自己的配置）；共享只经 `workspace.` 前缀；
  虚拟根上的包表先给出警告。

**修订 1：** 初稿，提出 A/B/C 三类问题。

## 0. 范围

| 项 | 结论 | 理由 |
| --- | --- | --- |
| #786 中以 `dialect_cxxflags` 定义 `__cpp_impl_coroutine` | 使用问题；由用户负责，见 §9 | clang 23 对 32 位 x86 Microsoft ABI 不支持协程，2026.10.8.1 已给出说明 |
| #784 | 不是设计问题 | 剩余阻塞是 `released ARM64 CN SubOS ecosystem` 中 proot 的 signal 11，单独处理 |
| #756 | 已修复 | 2026.10.2.1（#759），已关闭 |

在范围内的问题，结构都相同：**一个事实有两个来源，或者一个产物有两个写入者**。

| 类 | issue | 一个事实，两个来源 |
| --- | --- | --- |
| A 工作空间的传递 | #786、#785 | 同一张表的传递方式由它“写在哪里”与“它是哪一部分”共同决定；根位置值由三张手抄清单分别决定继承、分组与虚拟根 |
| B action 输出 | #778、R2 | 一个输出有两个写入者（prepare 的占位文件与生成器），新旧由不与它同处的 ninja 日志判断 |
| C 模块角色 | #790、R5 | 一个单元是否产出 BMI，由扩展名与模块声明分别回答，规则选择只读前者 |

## 1. 实测

| 用例 | 观察（2026.10.8.1） | 期望 |
| --- | --- | --- |
| R1a 虚拟工作空间根写 `[target.'cfg(os = "linux")'.build] dialect_cxxflags = ["-DDIAL=1"]`，`-p app` | `DIAL=0`，0 条警告 | 有明确的写法使其生效；当前写法应有诊断 |
| R1b 同样的行写在成员中，`-p app` | `DIAL=0` | 2 |
| R1c 同一个成员作为独立包 | `DIAL=2` | 2 |
| R2 生成器输出取决于 `mcpp::profile()`；依次 `build`、`build --release`、`build` | 第三次的 dev 产物打印 `GEN=release`；只有一个 `target/.build-mcpp/out/gen.cpp` | `GEN=dev` |
| R3 #778 的工作空间（Linux）：首次构建后移走 `app/target`，保留工作空间的 `target` | 第二次构建失败：`undefined reference to resource()`，`generated.cpp` 为 0 字节 | 生成器重新运行 |
| R4 `src/api_impl.cpp` 写 `module repro:api_impl;`，llvm 23.1.3 | 每次构建都重编该单元并重新链接；`pcm.cache/` 中没有 `repro-api_impl.pcm` | 第二次构建无工作 |
| R5 `src/r5.cpp` 写 `export module r5;`，llvm 23.1.3 | `fatal error: module 'r5' not found` | 构建成功 |
| R6 R4 中由接口单元 `import :api_impl;` | `fatal error: module 'repro:api_impl' not found` | 构建成功 |
| R4–R6 改用 gcc 16.1.0 | 全部成功，第二次构建 0 步 | — |

## 2. 概念

文档与代码中的 “root” 指四种不同的东西。本文起分开使用：

| 术语 | 含义 |
| --- | --- |
| **工作空间根** | 含 `[workspace]` 的清单所在的目录与清单本身 |
| **根包** | 工作空间根同时含 `[package]` 时的那个包；它是成员 `"."`（2026.10.5.2 起） |
| **构建的配置** | 一次构建中全图共有的值：工具链、目标、标准、方言、链接方式、profile、索引……SPEC-004 §9 第 10 条所说的“根位置”指它 |
| **计划根** | 实现概念：由被选中成员合成的虚拟根；不出现在用户语义中 |

每个键有两个互相独立的属性：

- **作用域**，是键自身的属性：
  - P 包私有：`cxxflags`、`sources`、`defines`、`private_include_dirs`……只作用于本包的单元；
  - U 使用需求：`include_dirs`、`ldflags`、库……从依赖流向消费者；
  - C 配置：`standard`、`dialect_cxxflags`、`linkage`、`cxx_runtime`、`target`、`toolchain`、
    `profile`、`indices`、`[target.<sel>]` 的标量行、`.abi`……从构建的配置流向全图；
  - M 元数据：`version`、`license`……不进入命令。
- **来源**，是表的属性：命令行；本包的表；工作空间的共享表；引擎默认值。

条件（`[target.<sel>]`）是第三个维度：它是作用在值上的谓词，**既不改变作用域，也不改变来源**。

## 3. A 类：工作空间的传递

### 3.1 现状

工作空间根的清单今天按**位置**传递一部分表，按**前缀**传递另一部分表：

| 写在工作空间根 | 今天的传递 |
| --- | --- |
| `[workspace.package]`、`[workspace.build]` | 前缀：每个成员继承 |
| `[workspace.dependencies]` | 前缀：成员以 `x.workspace = true` 逐项选择 |
| `[toolchain]`、`[indices]`、`[profile.<name>]` | 位置：成员作为构建的根时继承，成员自己的声明优先 |
| `[target.<t>]` 的标量行（`toolchain`、`linkage`、`cxx_runtime`、`sysroot`、`min_api_level`、`runner(s)`） | 位置：同上，按 triple 整条替换 |
| `[xlings.workspace]` 与 `[target.<sel>.xlings.workspace]` | 位置：每个成员逐行继承 |
| `[target.<sel>.build]`、`[target.<sel>.abi]`、`[target.<sel>.runtime]` | **不传递**（R1a，没有诊断） |
| `[build]`、`[targets]`、`[dependencies]`、`[features]`…… | 不传递；属于根包，虚拟根上静默丢弃 |

于是同一个表头 `[target.<t>]` 下，标量行会传递，`.build` 不会；同一个 `[toolchain]`，
在根包上既是根包自己的声明，也是给成员的默认值。读者必须记住一张按位置、按子表区分的例外表，
#785 已经指出这是理解负担。实现层面，继承（`inherit_workspace_root_position`，
`src/project.cppm:282`；`inherit_workspace_xlings`，`:222`；`inherit_workspace_build`）、
分组（`root_position_key`，`:743`）与计划根（`virtual_workspace_root`，`:813`）是几张必须保持一致的手抄清单。
条件 dialect 与条件 abi 在其中全部缺失，结果就是 R1a、R1b（#786）。

### 3.2 语义设计：前缀即语义

**规则 W1。一份清单中，`workspace.` 之外的每张表只说本包：P、U、M 键作用于本包，C 键是
“本包作为构建的根时”的配置。`workspace.` 之内的每张表只说成员。任何表都不按位置传递。**

**规则 W2。可共享的表 `X` 都有镜像 `[workspace.X]`，键集合、子表结构与条件选择器与 `X` 相同，
去掉不可共享的键。**

| 本包写法 | 工作空间共享写法 | 状态 |
| --- | --- | --- |
| `[package]` 中可共享的元数据与 `standard` | `[workspace.package]` | 已有 |
| `[build]` | `[workspace.build]` | 已有（`kWorkspaceBuildKeys`） |
| `[dependencies]` 的条目 | `[workspace.dependencies]` + `x.workspace = true` | 已有，仍逐项选择加入 |
| `[toolchain]` | `[workspace.toolchain]` | 新增 |
| `[indices]` | `[workspace.indices]` | 新增 |
| `[profile.<name>]` | `[workspace.profile.<name>]` | 新增 |
| `[target.<sel>]` 的标量行、`.build`、`.abi`、`.runtime`、`.xlings.workspace` | `[workspace.target.<sel>]` 及同名子表 | 新增 |
| `[xlings.workspace]` | `[workspace.xlings.workspace]` | 新增（O2） |
| `[targets]`、`[features]`、`[resources]`、`[test]`、`[target.<sel>.targets]`、`[target.<sel>.dependencies]`、`requires_abi` | — | 不可共享：它们描述一个具体的包 |

**规则 W3。合并规则只由键决定，不由表决定。** 成员的值 = 合并（工作空间层，成员层）：

| 键的形态 | 规则 |
| --- | --- |
| 标量 | 成员**声明了**该键时成员优先，否则取工作空间的值（声明与否被记录，不由值推断） |
| 向量 | 追加，工作空间在前 |
| `defines` | 按宏名构成集合；成员同名条目替换，`!NAME` 删除 |
| 命名表（`profile.<name>`、`target.<sel>`） | **逐键**合并，规则同上；不再整表替换 |
| 相对路径 | 以写下它的清单所在目录为基准（#224） |

条件行逐行继承：工作空间的条件行排在成员的条件行之前，成员内部仍按 SPEC-004 §3.1.1 的选择器特异性排序。

**规则 W4。构建的配置**按以下顺序决定，先出现者优先：
1. 命令行（`--target`、`--toolchain`、`--profile`……）；
2. 被选中成员合并后的 C 键（W3 的结果），条件行以该构建的目标求值。

同一张图中的成员，求值后的配置必须相等；不相等的分到不同的图（多配置模型保留，review 结论 1）。
依赖包中的 C 键不生效，也不诊断（SPEC-004 §9 第 10 条，不变）。

**规则 W5。成员在每个位置得到的继承值都相同，而且只得到一次**（2026.9.25.1 起的不变式）：
作为被选中的成员、作为另一个成员的 `path` 依赖、作为通过 `git` 取得的工作空间的成员、作为宿主工具子构建的根。

**规则 W6。成员发布时，继承来的值写进它自己的表**：`[workspace.target.<sel>.build]` 写成
`[target.<sel>.build]`，`[workspace.toolchain]` 写成 `[toolchain]`，依此类推。
已发布的包因此自成一体（SPEC-004 §9 第 7 条）。这样，同一个包作为宿主工具（`tools =`）
从索引取得并成为子构建的根时，它的配置与在工作空间中构建时相同。

**规则 W7。** 一份没有 `[package]` 的工作空间根，若写了包表（`[build]`、`[targets]`、
`[target.<sel>.build]` 中的包键等），给出警告：这些表在这里不作用于任何包；写到成员中，
或写成 `[workspace...]` 来共享。`--strict` 下为错误。

**例子**（#786 的意图，按新规则）：

```toml
# 工作空间根：对所有成员，在 i686-windows-msvc 上
[workspace.target.i686-windows-msvc.build]
dialect_cxxflags = ["-DARCH_COMPAT=1"]

# 或只对一个成员：写在成员自己的清单中（今天的写法，修复后在工作空间中同样生效）
[target.i686-windows-msvc.build]
dialect_cxxflags = ["-DARCH_COMPAT=1"]
```

用户只需记住一句话：**要共享，就加 `workspace.`；不加，就只说本包。**

### 3.3 与 #785 B 的关系

#785 B 反对把 `[target.<triple>]` 改名为 `workspace.<triple>`。它的三条理由在本设计下都不成立：

- 独立工程不受影响：它的 `[toolchain]`、`[target.<t>]` 仍写在原处，含义不变（W1）。
- 作为宿主工具构建的成员仍用自己的行（W5）；发布后由 W6 写出，行为不变。
- “永久双读”只发生在工作空间根，而工作空间根本身不会被发布；成员发布时继承值已写出（W6）。
  兼容期之后旧写法即可移除（§3.6）。

#785 A 的 `[workspace.target.<sel>.build]` 是 W2 的一行；#785 C 即 W7。

### 3.4 对比

| 维度 | Cargo | Meson | Bazel | CMake | mcpp（本设计） |
| --- | --- | --- | --- | --- | --- |
| 配置归谁 | `[profile]`、`[patch]`、`resolver` 只在工作空间根生效，成员的被忽略并警告 | `add_global_arguments` 只能在主项目调用 | 属于本次构建（命令行、platform），不属于包 | 一个构建树一个配置 | 属于本次构建；被选中的成员可以声明，多配置分图 |
| 成员能否有不同配置 | 稳定版不能（`per-package-target` 仍 unstable） | 子项目选项可覆盖 | 能（配置 transition） | 不能 | 能（W4） |
| 共享方式 | 命名表 `[workspace.*]`，逐键选择加入 | 主项目 `default_options` | 不继承 | 目录作用域隐式继承 | 命名表 `[workspace.*]`，默认继承（W2） |
| 虚拟根写包表 | 报错 | — | — | — | 警告，`--strict` 为错误（W7） |
| 条件 | 包级 `cfg()` 依赖；flags 放在环境层 `.cargo/config.toml` | 构建文件中的条件判断 | `select()`，只作用于值 | 生成器表达式 | 只作用于值，不改变作用域与来源 |

结论：
- 配置不属于包，这一点各家一致；
- mcpp 允许成员有不同配置，这是 C++ 工作空间（固件、宿主工具、测试并存）的真实需要，代价是 W4 中的“按求值后的值分组”；
- 共享经由命名表，与 Cargo 的 `[workspace.*]`、pnpm 的 catalog、Gradle 弃用 `allprojects{}` 的方向一致；
  按位置隐式传递（CMake 目录作用域）是被淘汰的做法。

（其他工具的条目来自已有了解，写入规范引用前需逐条核对原文。）

### 3.5 技术架构

1. **键的类别登记表 `KeyRegistry`（`modules/manifest`）。** 每个可写的键一行：
   - 表路径与键名；
   - 作用域（P/U/C/M）；
   - 合并形态（标量 / 向量 / 宏集合 / 命名表）；
   - 是否可共享（W2）；
   - 是否允许出现在条件行；
   - 发布时是否写出（W6）；
   - 字段访问器（沿用 `kWorkspaceBuildKeys` 的成员指针 variant）。

   `kWorkspaceBuildKeys` 是它的一个子集，并入其中。

   下列逻辑全部由登记表推导，不再各写一份：
   - 解析器的已知键检查与错误文本；
   - `[workspace.*]` 镜像表的解析；
   - 继承；
   - 根位置值；
   - 发布时写回；
   - W7 的“包表”判定；
   - 文档中的键类别表（新增 `check_manifest_key_classes.py`，比对登记表与 docs/04、docs/07 的表）。
2. **分层的声明。** 清单读入后保存两层：本包层与工作空间层（来自 `[workspace.*]`，旧位置写法也在此层，见 §3.6）。
   条件行是 `ConditionalRow { predicate; 若干 (键, 值) }`，键带着登记表中的类别。
   `ConditionalConfig` 中包所有的部分与根位置的部分由类型分开（`RootPositionRow`）。
3. **一个合并函数。** `inherit(member, workspaceLayer, registry)` 对不带条件的键与条件行使用同一套规则（W3），
   取代 `inherit_workspace_root_position`、`inherit_workspace_xlings` 与 `inherit_workspace_build` 中的手写逻辑。
   每个继承来的值记录来源（文件、表、键、选择器），沿用 `inheritedFromWorkspace`（WS3），供诊断与 `mcpp why` 使用。
4. **`RootPosition`。** `root_position(member, cfgContext)` 从合并后的成员导出全部 C 键，条件行已求值。
   - 分组键 = 它的规范序列化；
   - 计划根应用第一个成员的 `RootPosition`，并携带其原始条件行，供 prepare 以同一上下文求值；
   - 新的 C 键只需在登记表中标为 C，不必改动这些函数。
5. **按有效值分组。** prepare 的上下文是 `cfgpred::context_for(目标 triple)`（`toolchain.cpp:1056`）；
   目标 triple 取 `--target`，否则取 `[build] target`（`toolchain.cpp:966`），否则取宿主。
   这三项在 CLI 中已知，不需要解析工具链。`workspace_groups`（`src/cli/selection.cppm:213`）
   调用同一个函数，因此分组与 prepare 的求值在构造上一致。

### 3.6 兼容与迁移

- 工作空间根上的旧位置写法（`[toolchain]`、`[indices]`、`[profile.<name>]`、`[target.<t>]` 标量行、
  `[xlings.workspace]` 及其条件行）**行为不变**，仍被读入工作空间层，仍按原规则（按 triple 或按名字整表替换）合并；
  每张表给出一次警告 `manifest/workspace-position`，并写出对应的新写法。
- 警告只在旧规则**真正起作用**时给出：
  - 虚拟根上的这几张表只有传给成员这一种含义，总是警告；
  - 带 `[package]` 的根上，它们首先是根包自己的声明（W1）；只有当某个成员作为构建的根、
    并且确实经由旧规则得到了值时才警告，警告指出该成员与该表。只构建根包时不警告。
- **旧写法的读取全部放在兼容模块中。** 新增 `modules/manifest/src/compat/workspace_position.cppm`
  （`mcpp.pm.compat.workspace_position`），由 `mcpp.pm.compat` 门面导出：
  - 它是唯一知道旧写法的地方：把工作空间根上的这几张表转换为工作空间层，并记录“来自旧位置写法”；
    继承、分组与 `KeyRegistry` 只看到工作空间层，不知道旧写法的存在；
  - 每个调用点与转换处标注 `// COMPAT(workspace-position): <移除条件>`，移除时用 grep 即可找全；
  - `compat.cppm` 顶部的 DEPRECATION SCHEDULE 表增加一行（旧写法 → `[workspace.X]`），与已有的两行同样在 1.0.0 移除；
  - 警告由该模块在转换时发出，措辞列出旧表、新表与迁移示例。
- 同一张表同时以旧写法与 `[workspace.X]` 写出时报错：同一个事实不能有两种说法。
- 带 `[package]` 的工作空间根迁移后，根包自己的 `[toolchain]` 等只作用于根包；根包作为成员同样接收 `[workspace.*]`。
- 旧写法移除：随兼容模块在 1.0.0 移除（DEPRECATION SCHEDULE）。
- **旧版 mcpp 会静默忽略新写法。** 实测 2026.10.8.1 读到 `[workspace.toolchain]` 时不给任何提示，照常构建
  （于是用的是另一个工具链）。因此：
  - 使用新写法的工作空间需要用 `[workspace.package] mcpp = ">=<本版本>"` 声明最低版本，由旧版已有的引擎版本检查拒绝构建；
  - 本版本读到新写法、而工作空间没有声明这一下限时，给出一条 note，建议补上；
  - 本版本起，`[workspace]` 下的未知表报错。今天未知表被静默忽略，这与“传播表中被静默丢弃的键一律拒绝”
    （docs/07 §4.1）矛盾，属于同一类缺陷。
- 行为变化：
  - 成员曾被静默丢弃的条件 dialect 与 abi 开始生效（std 缓存键与 fingerprint 改变，相关包重编一次）；
  - 条件值求值后不同的成员分到不同的图；只在其他目标上不同的成员仍在同一张图中；
  - 虚拟根上无效的包表开始警告。

### 3.7 诊断分级

| 情形 | 等级 |
| --- | --- |
| 虚拟根上的包表（W7） | `warning manifest/schema`；`--strict` 为错误 |
| 工作空间根上的旧位置写法 | `warning manifest/workspace-position`，附新写法 |
| 同一表的旧写法与 `[workspace.X]` 并存 | 解析错误 |
| `[workspace.X]` 中不可共享的键（如 `allow_host_libs`、`[workspace.target.<sel>.targets]`） | 解析错误，沿用 `[workspace.build]` 的措辞 |
| `[workspace]` 下的未知表 | 解析错误（今天静默忽略） |
| 使用新写法但未声明 `mcpp` 下限 | note，建议声明下限 |
| 继承与分组结果改变 | 无诊断；CHANGELOG 记录 |

## 4. B 类：action 输出的所有权

### 4.1 现状与根因

- prepare 为 Source action 尚不存在的 TU 输出写占位文件（`directives.cppm:1488`），供 prepare 期扫描读取；生成器在构建时覆盖它。
- `mcpp::out_dir()` 固定为 `<包>/target/.build-mcpp/out`，与配置无关（`build_program.cppm:753`）；
  工作空间成员的 out_dir 在成员目录下（`target_side.cpp:2011`），计划的 ninja 日志在工作空间根下。

根因有两条：
1. **一个输出有两个写入者。** 占位文件比 action 的输入新；只要某份 ninja 日志里有这条 action 的记录，
   ninja 就把占位文件当作最新的生成结果（R3，#778）。#534 是同一结构的前一个实例。
2. **输出与判断它新旧的日志不在一起。** 多个配置共写一个文件，各自的日志都认为它是最新的（R2）；
   成员目录与工作空间日志可以分别被清除（R3）。即使没有占位文件，R2 仍然成立。

### 4.2 设计

**原则：action 的输出只有一个写入者，即 action 本身；它与判断它新旧的 ninja 日志处于同一配置目录下。**

- **单一写入者（D6）。** prepare 不再写任何 action 输出。生成的 TU 在 prepare 期以“声明单元”进入扫描：
  - 路径与语言来自输出路径和扩展名分类；provides/imports 来自 action 的声明（`.provides()` / `.imports()`）；
  - 不读文件，构造方式与 `scan_overrides` 分支相同（`scanner.cppm:1222`）；
  - 声明了 `provides` 的单元标为 `scanOverridden`，构建时由编译器的 P1689 扫描审计（`ninja_backend.cppm:2800`）；
  - 未声明 `provides` 的单元不作断言，它的 imports 由构建期 dyndep 扫描发现，与今天的空占位文件效果相同。
- **按配置的 out_dir（D7）。** `out_dir` 改为 `<计划根目录>/target/.build-mcpp/out/<配置标识>/<包>`：
  - 计划根目录：独立包为包目录；工作空间计划为工作空间根（成员也是）；
  - 配置标识：构建程序运行前已知、且会改变程序环境的值的摘要（O3）；
  - 构建程序的二进制与缓存仍放在不分配置的位置，不重复编译。

Cargo 的 `OUT_DIR`（`target/<profile>/build/<pkg>-<hash>/out`）、Bazel 的 `bazel-out/<配置>/` 与“每个输出只有一个生成 action”，
都是同一原则；CMake 多配置生成器下不带 `$<CONFIG>` 的 custom command 输出会在配置间共享，就是 R2 的问题。

### 4.3 兼容与诊断

- `mcpp::out_dir()`、`${mcpp.out_dir}`、`MCPP_OUT_DIR` 的值改变。按接口使用的构建程序不受影响；
  硬编码 `target/.build-mcpp/out` 的项目需要改用接口（#778 的复现本身就是硬编码）。CHANGELOG 与 docs/30 写明。
- 构建程序之外的脚本（例如测试脚本）需要知道某个包的生成目录时，从 `resolution.json` 读取：
  每个包新增 `outDir` 字段。这是给外部读取者的稳定出口，不再让它们猜路径（mcpp-plugins 的测试即属此类，§11）。
- 旧位置不再写入也不再读取，`mcpp clean` 删除；旧版本遗留的零字节占位文件因此失效。
- configure-only 之后，生成的 TU 在第一次构建前不在磁盘上，`compile_commands.json` 中的条目暂时指向不存在的文件。
- 无新诊断：声明不符时的构建期失败沿用现有审计报错。O4 的过渡期用 `degraded`。

## 5. C 类：模块角色

### 5.1 现状与根因

- `pick_rule`（`ninja_backend.cppm:2532`）按扩展名分类选择规则：`.cpp` → `cxx_object`，不写 BMI。
- 扫描器读模块声明，得到 `provides`；edge 据此声明 BMI 输出（`ninja_backend.cppm:2983`）。
- `cxx_module` 规则内部，`module_lang` 与 `module_output` 已按扫描结果逐 edge 设置（`module_edge_vars`，`:2520`）。

同一问题已经修过三次，每次一处：#272、未知扩展名被拒绝（`scanner.cppm:762`，原注释写着 “Two answers to 'is this a module interface' and only one of them read”）、`module_edge_vars`。
#790 是剩下的一处。根因是扩展名同时回答“什么语言”和“什么模块角色”。GCC 按内容判断，所以 R4–R6 在 GCC 上成功。

### 5.2 设计（D8）

**原则：扩展名决定语言，模块声明决定模块角色。**

- 每个 C++ 单元的模块角色取自扫描得到的 `ModuleDeclaration`（`CompileUnit::declaration` 已存在）。
- 规则选择、`bmi_out`、`module_lang`/`module_output`、`--expect-provides`、打包时的接口发布判断只读这一个字段。
  `pick_rule`：语言为 C++ 且角色产出 BMI（`Interface`、`InterfacePartition`、`ImplementationPartition`，
  或 `Unknown` 且声明了 provides）时用 `cxx_module`。
- `module_extensions` 收窄为“这些扩展名是 C++ 模块源文件，默认走模块规则”；它不再是一个 `.cpp` 成为模块单元的前提。
- 编译器参数：Clang 为 `-x c++-module` 与 `-fmodule-output=`；GCC 不变；原生 cl.exe 按下表（O5 实测，探测 PR #791，run 37974353683）：

  | 角色 | `.cpp` | `.cppm`（cl 不认识的扩展名） | `.ixx` |
  | --- | --- | --- | --- |
  | 主接口 / 接口分区 | `/interface`（不加则 C3378） | `/interface /TP`（不加则文件被忽略，D9024） | 不需要选项 |
  | 实现分区 | `/internalPartition`（不加 C7621；`/interface` 则 C3474） | `/internalPartition /TP`（`/interface /TP` 则 C3474） | — |
  | 实现单元（`module m;`） | 不需要选项 | 未测 | — |

  今天 mcpp 对所有产出 BMI 的单元使用 `/interface /TP`（`module_edge_vars`），所以原生 cl.exe 下 `.cppm` 中的实现分区今天就失败（C3474）。
  D8 让模块角色决定这一参数，同时修复这一缺陷。
- 若 O5 测得某编译器无法以该扩展名编译某种角色，则在计划阶段拒绝，点名文件、角色与改名建议（CMake 对未放进 `CXX_MODULES` file set 的模块单元同样报错）。

## 6. 决定

| # | 决定 | 节 |
| --- | --- | --- |
| D1 | 前缀即语义：`workspace.` 之外只说本包，`workspace.` 之内只说成员；不按位置传递（W1） | §3.2 |
| D2 | 可共享的表都有 `[workspace.X]` 镜像，含 `toolchain`、`indices`、`profile`、`target.<sel>` 及其子表（W2） | §3.2 |
| D3 | 合并规则只由键决定；命名表逐键合并；条件行逐行继承（W3） | §3.2 |
| D4 | `KeyRegistry` 覆盖全部键；分层声明；一个合并函数；`RootPosition`；文档一致性检查 | §3.5 |
| D5 | 分组以 `context_for(目标 triple)` 求值，与 prepare 用同一个函数（W4） | §3.5 |
| D6 | prepare 不写 action 输出；生成的 TU 以声明单元进入扫描 | §4.2 |
| D7 | `out_dir` 按计划根目录与配置标识分开；`resolution.json` 记录每个包的 `outDir` | §4.2、§4.3 |
| D8 | 模块角色只取自模块声明 | §5.2 |
| D9 | 虚拟根上的包表警告（W7）；旧位置写法在兼容模块中读取并警告 | §3.6、§3.7 |
| D10 | 发布时写出继承值，含条件行（W6） | §3.2 |
| D11 | `--toolchain` 覆盖清单声明的工具链时警告 | §8 |
| D12 | 用户定义 `__cpp_impl_coroutine` 由用户负责；mcpp 只保证 `dialect_cxxflags` 一致送达 | §9 |
| D13 | `.xlings.json` 的引导版本更新到 2026.10.8.1 | §10 |
| D14 | 修正 SPEC-004、SPEC build-plugins、docs/04、07、20、30 及中文版 | §16 |
| D15 | Windows SDK 与 vswhere 的探测加入注册表与 `%ProgramFiles%`，不再写死 `C:` | §12 |
| D16 | MSVC 探测返回结构化结果，诊断说出缺的是哪一半、查过哪里 | §12 |
| D17 | 工具链被明确指定过就永不自动换成 MinGW；什么都没指定时才改用 MinGW，并说清楚缺的是什么 | §12 |
| D18 | 显式声明的工具链永不自动切换；MSVC ABI 工具链缺 STL 或 SDK 时在计划阶段拒绝 | §12 |
| D19 | （P1、P2）glibc 2.44.3 r4 等载荷去掉硬链接，索引 CI 拒绝含硬链接的载荷 | §13.4 |
| D20 | （xlings X1）解包时硬链接创建失败则复制目标文件 | §13.4 |
| D21 | 识别 Android 宿主，用于 shell 解析与诊断；**不改**默认工具链（aarch64 仍为 llvm@23.1.3 + glibc） | §13.4 |
| D22 | 安装失败的提示按 xlings 的错误类别给出，不再一律提示“检查网络” | §13.4 |
| D23 | Planning 中显示 xlings 的阶段与进度；verbose 下每 30 s 报告仍在运行的子进程 | §13.4 |
| D24 | `[xlings.workspace]` 的共享写法为 `[workspace.xlings.workspace]`（原 O2） | §3.2 |
| D25 | 配置标识取目标 triple、profile、工具链身份与该包启用的 features（原 O3） | §4.2 |
| D26 | 生成模块接口的 action 必须声明 `.provides()`；发现未声明的写法时给一个版本的 `degraded` 过渡期（原 O4） | §4.2 |
| D27 | Termux 的 CI：termux-docker（aarch64）与 Android 模拟器沙箱（x86_64）两个 job | §13.4 |
| D28 | 子进程使用的 shell 不再写死 `/bin/sh`：依次取 `/bin/sh`、`/system/bin/sh`（Android）、`$PREFIX/bin/sh` | §13.4 |
| D29 | 工具链私有库的加载器环境只给工具链自己的进程，不给 ninja 与它的 `/bin/sh` | §13.4 |
| D30 | mcpp 依赖的 xlings 升级到包含 X1–X3 的新版本，同步所有固定点 | §14 |
| D31 | （xlings X2）解包失败按 errno 分类；失败后不再继续注册程序、不报告 installed | §13.4 |
| D32 | （xlings X3）解包阶段按进度发事件，安装钩子每 10 s 心跳 | §13.4 |

## 7. 待定

- ~~O5：原生 cl.exe 对各模块角色的参数。~~ 已测（§5.2 的表）：实现分区必须用 `/internalPartition`。
- ~~O6：Windows SDK 不在常规路径时的实际行为。~~ 已测（§12.1）。
- ~~O7：Termux 上 Planning 35 分钟。~~ 用户日志证实在 xlings 安装 musl-gcc 之中（§13.1，R4）。
- ~~O8：Android 上 llvm + glibc 是否可用。~~ x86_64 已在 Android 14 应用沙箱中端到端验证（§13.3）。
- ~~O9：musl 静态 LLVM。~~ 不需要（review 结论）。
- ~~O10：termux-exec 的 linker 执行模式。~~ 未能稳定复现，撤销（§13.5）。
- **O11：aarch64 真机验证。** GitHub 没有可运行 aarch64 Android 模拟器的 runner。X1、D29 完成后，用 PR 构建的 mcpp 在 aarch64 Termux 真机上
  完成 `mcpp new hello && mcpp run`（llvm@23.1.3 默认），作为发布前的最后一步。

## 8. `--toolchain` 覆盖清单中的工具链（D11）

**现状。** `--toolchain` 经 `MCPP_TOOLCHAIN` 进入 prepare，优先于清单的 `[toolchain]`（`toolchain.cpp:339`），
也优先于匹配的 `[target.<t>].toolchain`（`step1_apply_target_section`，`:451`）。覆盖是静默的。
`--toolchain` 由 CLI 写入 `MCPP_TOOLCHAIN`，用户直接设置该环境变量与写 `--toolchain` 等价，提示中写作 `--toolchain (MCPP_TOOLCHAIN)`。

**设计。** 当命令行给出的工具链与该构建的配置中由清单声明的工具链（本包或工作空间层，`[toolchain]` 或匹配的 `[target.<t>].toolchain`）**不同**时，给出警告：

```
warning: --toolchain llvm@23.1.3 replaces gcc@16.1.0 declared at [toolchain].default (mcpp.toml)
```

- 相同时不提示；清单没有声明时不提示；
- 宿主工具子构建（`tcFromConsumer`）与依赖包不提示；
- 用 `diag::warning`，不是 `degraded`：用户的选择被执行了，什么也没少做，因此 `--strict` 不因此失败；
- 工作空间多配置时，每个（声明，覆盖值）组合每个进程只说一次（diag 已去重）；
- 来源一列沿用 `tcSpecSource` 的措辞，指向写下它的文件与键。

## 9. 用户定义 `__cpp_impl_coroutine`（D12）

**立场：这是用户自己的声明，mcpp 不做保证，也不特别处理。** 编译器已声明 32 位 x86 Microsoft ABI 上的协程不受支持；
用户定义该宏，相当于替编译器打开一个它声明不支持的功能，结果由用户负责。

mcpp 只负责一件事，而这件事属于 A 类修复：**用户写在 `dialect_cxxflags` 中的宏，一致地到达 std/std.compat 预编译、扫描与每个 C++ 单元。**
§3 实施后，下面两种写法都满足这一点：

```toml
# 一个成员（或独立包）
[target.i686-windows-msvc.build]
dialect_cxxflags = ["-D__cpp_impl_coroutine=201902L"]

# 工作空间的所有成员
[workspace.target.i686-windows-msvc.build]
dialect_cxxflags = ["-D__cpp_impl_coroutine=201902L"]
```

- 写在 `cxxflags` 中时，std 模块的预编译看不到这个宏，仍会在 `<generator>` 中失败。这是 `dialect_cxxflags` 的通用规则，不是协程的特例。
- 不探测、不验证、不增加提示；`msvc_coroutines` 的失败说明不变（它只在 std 的 `<generator>` 失败或协程用法失败时出现，
  用户定义宏之后，前一种失败不再发生）。
- docs/20 该节保留“不推荐”，补一句：“mcpp 不对此做保证；若坚持，宏必须写在 `dialect_cxxflags` 中，原因见 docs/04 §2.1。”

## 10. `.xlings.json` 的引导版本（D13）

仓库的 `.xlings.json` 固定 `"mcpp": "2026.9.24.1"`，它是自举构建使用的 mcpp，落后当前版本十余个发布。按 `check_version_pins.sh` 第 (b) 条，引导版本必须是已发布、且索引已提供的版本；
2026.10.8.1 满足这一条件。发布 PR 中把它改为 `2026.10.8.1`，并运行 `check_version_pins.sh`。

验证（Linux x86_64，干净的 worktree，`b1652184`，删除 `.xlings.json`，默认 gcc@16.1.0）：

| 引导版本 | 结果 |
| --- | --- |
| 2026.9.24.1 | 构建成功（102 s），但给出 `warning: [test] has unsupported key 'windows_code_page' (ignored)` |
| 2026.10.8.1 | 构建成功（1 m 42 s），无警告 |

在这台 Linux 主机上，旧引导版本“无法构建”没有复现。但它会忽略它不认识的键，得到的不是这份清单描述的构建：
上表中 Windows 的测试代码页（2026.10.5.2）被丢弃；macOS、Windows 与 aarch64 上默认的 `llvm@23.1.3` 以及
2026.10.8.1 的工具链修复也不在旧版本中。引导版本必须理解清单中的每个键，所以更新它；
未在 Windows、macOS、aarch64 上实测旧版本的具体失败，发布 PR 的 CI 会覆盖这几个平台。

**对 mcpp 自身迁移的约束。** 引导版本 2026.10.8.1 会静默忽略 `[workspace.toolchain]` 等新写法（§3.6）。
因此 mcpp 自己的 `mcpp.toml` 在本版本中**不能**改用新写法，否则自举构建会悄悄用错工具链；它保留旧写法（自举时有兼容警告），
在下一个版本把引导版本提升到本版本之后再迁移（§11）。

## 11. 同步更新：mcpp 自身与生态

| 仓库 | 受影响的部分 | 动作 | 时机 |
| --- | --- | --- | --- |
| mcpp：`mcpp.toml` | 带 `[package]` 的工作空间根，根上有 `[toolchain]`、`[target.aarch64-linux-gnu]`、`[target.x86_64-linux-musl]`、`[target.aarch64-linux-musl]` | 本版本保留旧写法：`mcpp build` 只以根包为根，不触发警告；`mcpp test -p <模块>` 这类以成员为根的命令会警告。下一个版本迁移到 `[workspace.toolchain]`、`[workspace.target.*]` | 下一个版本（§10 的约束） |
| mcpp：e2e | 10 个脚本在工作空间根上写了旧位置的表 | 保留 2 个作为兼容覆盖（断言警告与行为不变），其余迁移到新写法 | 本版本 |
| mcpp：docs、specs、示例 | docs/04、07、20、30 与中文版，SPEC-004、build-plugins | 改写（§16） | 本版本 |
| mcpp-community/mcpp-plugins | `tests/metal-consumer/check-metal.sh`、`tests/apk-consumer/check-apk-features.sh`、`tests/apk-consumer-libraries/check-apk-libraries.sh` 与 `ci.yml` 硬编码 `target/.build-mcpp/out`（D7）；`rules/qt.cppm` 的 source action 只生成非模块 TU，不受 D26 影响 | 测试改为从 `resolution.json` 的 `outDir` 读取 | 与本版本同时发布 |
| openxlings/libxpkg | `build.mcpp` 使用 `mcpp::out_dir()` 接口 | 无需修改；以发布前的 CI 验证 | 本版本 |
| mcpplibs/openkal 系列（openkal、-musl、-linux、-llvm-runtime、-emscripten、-opensbi、-macos、-windows、-uefi、std-freestanding-alloc-kal） | 都不是工作空间；独立包的 `[toolchain]`、`[target.<sel>.build]` 含义不变（W1） | 无需修改；以 mcpp 的 openkal workflow 验证 D6–D8 | 本版本 |
| openxlings/xim-pkgindex、xlings-res | glibc 2.44.3-r3（每个架构 256 个硬链接）、gcc 16.1.0（8）、musl-gcc 15.1.0（13）含硬链接 | P1：重新打包（glibc r4 优先，它在 llvm 默认路径上）；P2：索引 CI 拒绝含硬链接的载荷 | mcpp 发布之前 |
| openxlings/xlings | 解包没有硬链接回退（2026.10.10.1 实测）；`LocalWriteFailure` 一律报成磁盘满；失败后继续产生连带错误；阶段内无进度 | X1–X3（§13.4）；发布新版本；mcpp 固定到它（D30） | mcpp 发布之前 |

## 12. Windows：Visual Studio 或 Windows SDK 不在 `C:`

### 12.1 现状（代码）

- “MSVC 可用”定义为 STL 与 SDK 都找到（`has_usable_msvc`，`msvc.cppm:1345`）。
  - Visual Studio：`VSINSTALLDIR`、vswhere、`VS*COMNTOOLS`、常规路径。vswhere 的路径写死为
    `C:\Program Files (x86)\Microsoft Visual Studio\Installer\vswhere.exe`（`msvc.cppm:1061`）。
    Visual Studio 装在 `D:` 时，安装器与 vswhere 仍在系统盘的 Program Files (x86) 下，因此通常能找到；
    Windows 本身不在 `C:` 时找不到。
  - Windows SDK（`find_windows_sdk`，`msvc.cppm:1227`）：`WindowsSdkDir` 环境变量、受管载荷、两个写死的路径
    `C:\Program Files (x86)\Windows Kits\10` 与 `C:\Program Files\Windows Kits\10`。**不查注册表。**
    SDK 装在 `D:` 且不在开发者命令行中时，找不到。
- **首次运行**（没有任何工具链声明、没有 `--target`、没有全局默认值）且“MSVC 不可用”时，目标改为
  `x86_64-windows-gnu`，工具链改为 MinGW，提示 “no Visual Studio found”，并**写入全局默认工具链与默认目标**
  （`toolchain.cpp:404`、`:1680`）。于是：
  - Visual Studio 已安装、只是 SDK 不在 `C:` 时，提示的原因是错的；
  - 之后用户修好 SDK，这台机器的默认值仍是 MinGW。
- **显式声明了工具链**（`[toolchain]`、`[target.<t>].toolchain`、`--toolchain`、全局默认值）时，**不会**自动切换：
  回退条件要求 `tcSpec` 为空。缺 SDK 时在计划阶段拒绝（`msvc_unavailable_guidance`，`options.cpp:40`），
  但措辞不准：除了 cl.exe 一行，其余情况一律说 “MSVC STL + Windows SDK — neither was found”。

**实测（探测 PR #791，run 37974353683，`windows-latest`，mcpp 2026.10.8.1）。** 把 `C:\Program Files (x86)\Windows Kits\10`
移到 `C:\MovedKits\10`，并按安装器的做法改写注册表（`Installed Roots\KitsRoot10`、`Microsoft SDKs\Windows\v10.0\InstallationFolder`，
含 WOW6432Node），模拟装在其他盘的 SDK：

| 场景 | 结果 |
| --- | --- |
| B0 对照：SDK 在原处，`[toolchain] windows = "llvm@23.1.3"` | 成功（`msvc@system → MSVC 14.51.36231 · Windows SDK 10.0.26100.0`） |
| S1 什么都没指定（新的 MCPP_HOME） | 改用 `gcc@16.1.0 → x86_64-windows-gnu`，提示 “no Visual Studio found”（实际有 Visual Studio Enterprise 2026），这条提示还打印了两次；写入全局默认工具链与默认目标；`plan 1m14s` |
| S2 `[toolchain] windows = "llvm@23.1.3"` | **不切换**；计划阶段拒绝（exit 2）。上一行已解析出 `msvc@system → MSVC 14.51.36231`，错误却说 “MSVC STL + Windows SDK — neither was found”，并建议改用 MinGW 默认值 |
| S3 `--toolchain llvm@23.1.3` | 同 S2 |

结论：
- 明确指定时不切换、缺东西时拒绝，这两点今天已经成立；
- 问题在探测（不查注册表）与措辞（把“只缺 SDK”说成“都没找到”，把“有 Visual Studio”说成“没有”）；
- 修订 3 中“缺 SDK 时静默继续”的判断是读代码得出的，实测不成立，已删除。
- **mcpp 不会自动下载 MSVC。** 受管的 `msvc@<toolset>`（`xim:msvc` 加上与之同装的 `windows-sdk`）只在用户声明时使用。

### 12.2 设计

- **D15 探测位置。** SDK 的查找顺序：`WindowsSdkDir` → 受管载荷 → 注册表
  `HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots` 的 `KitsRoot10`（含 WOW6432Node 视图，这是 SDK 安装器记录安装位置的地方）
  → 由 `%ProgramFiles(x86)%`、`%ProgramFiles%` 得到的常规路径。vswhere 的路径同样由 `%ProgramFiles(x86)%` 得到。
  `mcpp doctor`、`mcpp self env` 与构建使用同一个函数。
- **D16 结构化结果。** 探测返回 `{Visual Studio 实例, toolset/STL, SDK, 每一项查过的位置}`。
  诊断说出缺的是哪一半，例如 “Visual Studio 2022 found at D:\VS\2022, but no Windows SDK: searched …”，
  不再在找到 Visual Studio 时说 “no Visual Studio found”。
- **D17 只有“什么都没指定”时才可以改用 MinGW。**
  - **明确指定**的含义：本包或工作空间层的 `[toolchain]`、匹配的 `[target.<t>].toolchain`、`--toolchain`（`MCPP_TOOLCHAIN`），
    以及全局默认值（`mcpp toolchain default` 写下的值，包括以前的首次运行写下的值）。
    任何一项存在，mcpp 就使用它，**永不**自动换成 MinGW；要换，只能由用户改 `--toolchain` 或改清单（或改全局默认值）。
    MSVC ABI 的工具链缺 STL 或 SDK 时按 D18 拒绝。
  - **什么都没指定**：可以改用 MinGW。无论是完全没有 MSVC，还是只缺一半（有 Visual Studio 但缺 SDK，或反过来），都改用 MinGW，
    写入全局默认值（与现行首次运行相同，之后的构建保持稳定，不会因为某天补齐了 SDK 而悄悄换 ABI）。
    提示必须说清楚：找到了什么、缺了什么、查过哪里，以及补齐之后如何改用 MSVC
    （`mcpp toolchain default llvm@<版本>`）。不再在找到 Visual Studio 时说 “no Visual Studio found”。
- **D18 明确指定的工具链缺东西时拒绝。** 今天的行为（S2、S3）就是如此，把它写成不变式并加测试；
  拒绝信息改用 D16 的结构化结果，说清楚缺的是哪一半。错误列出查过的位置与选项：安装 SDK 组件；设置 `WindowsSdkDir`；
  `mcpp toolchain install msvc`（受管，自带 SDK）；或由用户自己改用 MinGW（`--toolchain` 或改清单）。
- **不自动下载 MSVC。** 受管 MSVC 体积以 GB 计，且是否使用它是用户的选择；它只作为错误信息中的一个选项出现。

## 13. Termux（Android）：跨仓库的修复（mcpp、xlings、xim-pkgindex/xlings-res）

**目标（review 结论）：** Android 上不换工具链。aarch64 默认仍是 `llvm@23.1.3` 加受管 glibc（与 GNU/Linux aarch64 相同），
x86_64 仍按 Linux 的规则；要做的是让它在 Termux 中真实跑通。

### 13.1 用户提供的证据

- **第一次运行（2026.10.8.1，llvm@23.1.3 + glibc）：** `extract failed for glibc: write_header(…/libexec/getconf/POSIX_V6_LP64_OFF64): Can't create`，
  随后 llvm 的安装钩子因缺 glibc 拒绝，提示 “check free space and permissions”“check network and retry”。
- **第二次运行（更早的 mcpp，默认 musl-gcc@15.1.0）：** `~/.mcpp/log/mcpp.log` 的最后一行是 `05:06:56` 的
  `xlings: interface install_packages exec: … xim:musl-gcc@15.1.0`，此后没有任何记录；对应的 `mcpp-xlings-*.stderr` 为空。
  初始化共 171 s（索引 130.8 s、patchelf 22.7 s、ninja 17.7 s），下载 79.2 MB 用时 178.7 s；之后 30 分钟以上全部在 xlings 的这次安装中，
  mcpp 只显示 `Planning · 35:22`。

### 13.2 根因（全部经探测 PR #791 实测）

| # | 根因 | 所在 | 证据 |
| --- | --- | --- | --- |
| R1 | **载荷含硬链接，Android 应用沙箱禁止创建硬链接，而 xlings 解包没有回退** | xlings `src/core/xim/extract.cpp`（`archive_write_header` 失败即返回 `LocalWriteFailure`）；载荷 | 模拟器（API 34，Enforcing，`runas_app`）中 `ln` → `Permission denied`；mcpp 2026.10.8.1 自带的 xlings 与**最新的 xlings 2026.10.10.1** 都报 `extract failed for glibc … Can't create`（round 7）；硬链接条目数：glibc 2.44.3-r3 两个架构各 256（253 个 `share/zoneinfo`、3 个 getconf），gcc 16.1.0 为 8，musl-gcc 15.1.0 为 13，llvm 23.1.3、gcc-runtime、libxml2、zlib、linux-headers 为 0 |
| R2 | **工具链私有库的加载器环境泄漏给 ninja 调用的宿主 shell** | mcpp（ninja 子进程的环境） | termux-docker aarch64：`CANNOT LINK EXECUTABLE "/bin/sh": …/xim-x-llvm/23.1.3/lib/aarch64-unknown-linux-gnu/libc++.so is too small`，mcpp 构建失败；同一环境中只把 `LD_LIBRARY_PATH` 加在 clang 命令上则正常（P3/P4）。x86_64 模拟器上宿主 shell 未受影响（P3 通过） |
| R3 | **mcpp 写死 `/bin/sh`** | mcpp `modules/platform/src/process.cppm` | termux-docker 镜像没有 `/bin`，mcpp 启动的每个子进程 0.2 s 内失败；Android 10 及以上真机有 `/bin -> /system/bin`，不受影响 |
| R4 | **xlings 安装在解包与钩子阶段内部没有进度** | xlings（`Extracting` 只在阶段开始时报告一次）；mcpp（只显示 `Planning`） | 用户第二次运行：30 分钟以上没有任何输出 |
| R5 | **失败信息误导** | xlings `installer.cpp:785` 把所有 `LocalWriteFailure` 报成 `E_DISK_FULL` 并提示 “check free space and permissions”；解包失败后仍继续，产生 “installed but registered none of the programs” 的连带错误；mcpp 一律提示 “check network and retry” | 用户报错与模拟器复现一致 |

R2 的位置说明它与载荷无关：任何把工具链私有库放进环境的宿主都会受影响，Android 的 bionic shell 只是最先暴露出来。

### 13.3 修复后能否真实跑通：已验证的部分

| 环境 | 做法 | 结果 |
| --- | --- | --- |
| Android 14 应用沙箱，x86_64 模拟器（round 7） | 在 runner 上按设备的绝对路径预装 llvm@23.1.3 + glibc，去掉硬链接后打包推入沙箱（即 R1 修复后的状态） | clang 23.1.3 运行；编译并运行多线程 C++ 程序（`sum=6`）；**mcpp 2026.10.8.1 构建 `import std` 程序成功（2.08 s）**；runner 上构建的程序在沙箱中运行 |
| termux-docker aarch64（bionic 用户态，round 6） | mcpp 首次运行安装 llvm@23.1.3 + glibc（容器允许硬链接） | clang 运行，编译并运行多线程程序；mcpp 构建因 R2 失败；把加载器环境只给工具后正常 |
| aarch64 真机 Android | GitHub 的 arm64 runner 没有 KVM（`/dev/kvm` 不存在），不能运行 aarch64 模拟器 | 未验证；由 O11 在设备上完成 |

结论：R1、R2 修复后，llvm@23.1.3 + glibc 在 Android 上可以真实工作；x86_64 已在真实沙箱中端到端验证，aarch64 由组件级证据支持，需真机确认。

### 13.4 设计

**xlings（openxlings/xlings）**

- **X1（D20）硬链接回退。** `extract.cpp` 在把条目交给 `archive_write_header` 之前处理硬链接条目：
  先尝试 `create_hard_link(目标, 路径)`；若失败且 errno 为 `EPERM`、`EACCES`、`EXDEV`、`EMLINK` 或 `ENOTSUP`，
  则复制已解出的目标文件（保留权限与修改时间），跳过该条目的数据，并在该归档内只记录一次 verbose 说明。
  其他条目仍走 libarchive。tar 规定硬链接的目标出现在前，所以复制时目标已在磁盘上；目标不存在时按归档损坏报错。
  测试：含硬链接条目的归档，在注入“创建硬链接失败”的情况下解出的内容与权限和正常解包一致。
- **X2（D31）解包失败的分类与止损。**
  - `LocalWriteFailure` 按 errno 细分：`ENOSPC`/`EDQUOT` 才是 `E_DISK_FULL`；权限类给出路径与原因；
  - 一个包解包失败后，不再继续“注册程序”等后续步骤，也不报告 `installed`，只报告这一条失败。
- **X3（D32）阶段内进度。** `Extracting` 阶段按条目数或字节数定期发 `progress` 事件；安装钩子运行期间每 10 s 发一次心跳
  （阶段名、包名、已用时间）。接口格式不变，只是事件更密。
- 发布一个包含 X1–X3 的 xlings 版本；mcpp 固定到它（D30，§14）。

**xim-pkgindex / xlings-res**

- **P1（D19）重新打包。** glibc 2.44.3（x86_64、aarch64）发布 r4，打包时解引用硬链接（`tar --hard-dereference`）；
  gcc 16.1.0、musl-gcc 15.1.0/16.1.0 同样处理。zoneinfo 中被解引用的文件都很小，体积增加可忽略（实测前后数据待重新打包时记录）。
- **P2 载荷检查。** 索引 CI 拒绝含硬链接条目的载荷，新载荷不再引入。
- 有了 X1，P1 不是必需的；它让旧版 xlings 用户（未升级者）也能在 Android 上安装，并去掉对 X1 的依赖。见决策 Q2。

**mcpp**

- **D29 加载器环境只给工具。** 工具链私有库目录不再放进 ninja 进程的环境；需要它的工具在命令中以 `env LD_LIBRARY_PATH=… <tool>`
  前缀获得（或由 RPATH 解决，二者择一，以实现时的改动面为准）。宿主 shell 与其他命令看不到它。
- **D28 不写死 `/bin/sh`。** 一处解析：`/bin/sh`，否则 `/system/bin/sh`，否则 `$PREFIX/bin/sh`。ninja 自身写死 `/bin/sh`，
  因此 Android 的支持基线是 Android 10 及以上（有 `/bin -> /system/bin`），写入 docs/20。
- **D22 按失败类别提示。** 读取 xlings 返回的错误码（X2 之后可区分下载、解包、权限、磁盘、钩子），给出对应提示，不再一律 “check network and retry”。
- **D23 Planning 不再沉默。** 把 xlings 的阶段与进度（X3）显示在进度行（例如 `Installing musl-gcc@15.1.0: extracting 41%`）；
  verbose 下记录每个阶段的起止；子进程超过 30 s 无事件时，verbose 每 30 s 输出 “still running”。
- **D21（改）识别 Android 宿主，但不改默认工具链。** 判断依据：`ANDROID_ROOT` 与 `ANDROID_DATA`，或 `/system/bin/linker64` 存在。
  用于 D28 的 shell 解析、`mcpp self env` 的报告与诊断措辞。删除 `toolchain.cpp:1555` 关于 “aarch64 → musl … ideal for Termux” 的过时注释。
- **D27 Termux CI。** `ci-fresh-install.yml` 增加两个 job，安装已发布的 mcpp（以及 PR 构建的 mcpp）完成 `mcpp new hello`、`mcpp run`：
  - termux-docker aarch64（先建 `/bin -> /system/bin`）：覆盖 aarch64 的默认工具链与 R2；
  - Android 14 x86_64 模拟器 + Termux APK + `run-as`：覆盖应用沙箱（R1），约 15 分钟。

### 13.5 其他结论

- **LLVM 没有 musl 静态版本**（xim-pkgindex 的 llvm 在 Linux 上全部依赖 glibc 与 gcc-runtime）。按 review 结论不需要它，原 O9 撤销。
- **termux-exec 的 linker 执行模式（原 O10）**：round 4 中一次调用报 `has unexpected e_type: 2`；round 6、7 在同一模拟器、termux-exec 2.3.0 下，
  默认模式与关闭该模式都能运行 mcpp；用户真机也能运行。不作为阻碍，撤销 O10。

## 14. mcpp 依赖的 xlings 版本（D30）

**现状。** mcpp 固定 xlings 2026.10.8.1，固定点：
- `src/xlings/xlings.cppm:117` 的 `kXlingsVersion`（`config.cppm` 的 `kXlingsPinnedVersion` 引用它；发布包内置的 xlings 与 `mcpp self env` 的 “xlings pinned” 都取自这里）；
- `.github/workflows/release.yml` 的 `XLINGS_VERSION`（4 处）与 aarch64 资产名（`:396–407`）；
- `.github/workflows/ci-fresh-install.yml:155` 的 `quick_install.sh … v2026.10.8.1`。

xlings 已发布 2026.10.9.1、2026.10.9.2、2026.10.10.1。其间的改动：SubOS 架构（#641）、xvm 对载荷未提供资产的处理（#649，2026.10.9.2）、
Luban OS（#650，2026.10.10.1）。

**本地兼容性冒烟测试**（Linux x86_64）：mcpp 2026.10.8.1 的发布包中替换为 xlings 2026.10.10.1，新的 MCPP_HOME，
`gcc@16.1.0` 的 `import std` hello：初始化、patchelf/ninja 引导、glibc 与 gcc 安装、构建全部成功（1 m 26 s，主要为下载）。

**设计。**
- 本版本把 xlings 升级到**包含 X1–X3 的新版本**（2026.10.10.1 之后的下一个发布）。2026.10.10.1 不够：round 7 在 Android 沙箱中实测，
  它解包 glibc 时与 2026.10.8.1 一样失败。
- 若 X1–X3 赶不上 mcpp 的发布，先固定 2026.10.10.1（Linux x86_64 冒烟测试已通过），并依赖 P1（glibc r4）让 Android 的默认路径可以安装；
  这条路线的取舍见决策 Q2。
- 所有固定点一次改齐；`check_version_pins.sh` 已扫描 `.github/` 中的 xlings 固定点，补充对 `kXlingsVersion` 的比对，
  让源码与 workflow 中的版本不能不一致。
- `xlings.cppm` 中按版本记录“为什么需要这个版本”的注释段落增加一条，写明新版本带来的、mcpp 依赖的行为（X1 的解包回退、X2 的错误码、X3 的进度事件，以及 #641 之后 SubOS 的变化中 mcpp 用到的部分）。
- 验证：发布 PR 的完整 CI（四个平台、openkal、交叉构建），加上 D27 的两个 Termux job。

## 15. 外部 PR 的处置

- #787：修复了“成员中的条件 dialect”（W4 的一个实例），未覆盖条件 abi、工作空间层、按值分组；按原始谓词分组会过度拆分。
- #779：修复了 R3，未覆盖 R2；仍保留两个写入者。
- 两者的分析并入本文；发布 PR 合入后在两个 PR 中说明、致谢并关闭。

## 16. 测试、规范与文档

**新增 e2e**（从 893 开始编号，每个都必须在 2026.10.8.1 上失败）：

| 测试 | 覆盖 |
| --- | --- |
| 893 前缀即语义 | `[workspace.toolchain]`、`[workspace.profile]`、`[workspace.target.<sel>]` 的标量行与 `.build` 到达每个成员与根包、不到达取来的依赖；命名表逐键合并；成员的同名 define 优先；两种写法并存时报错；`[workspace]` 下的未知表报错；W7 警告 |
| 894 旧位置写法的兼容 | 旧写法的行为与 2026.10.8.1 相同；虚拟根总是警告；带 `[package]` 的根只在成员经由旧规则取值时警告，只构建根包时不警告 |
| 895 配置中的条件值 | R1b；只在其他目标上不同的两个成员在同一张图中；条件 dialect 进入 std 缓存键 |
| 896 action 输出只有一个写入者 | R3；configure-only 后不留文件；已有输出（含合法的空文件）不被改动 |
| 897 每个配置有自己的生成输出 | R2；`resolution.json` 的 `outDir` |
| 898 `.cpp` 中的模块单元 | R4 第二次构建无工作；R5、R6 在 Clang 与 GCC 上都成功 |
| 899 `--toolchain` 覆盖提示 | 不同时警告并指向文件与键；相同、未声明、宿主工具时不提示；`--strict` 不失败 |
| 900 发布写出继承值 | `mcpp publish --dry-run` 的清单含 `[target.<sel>.build]` 与 `[toolchain]` 的继承值 |
| 901 加载器环境不进入宿主 shell | 一个 source action 经 `/bin/sh` 打印 `LD_LIBRARY_PATH`，其中不含工具链私有库目录（D29） |

Windows 探测（D15–D18）与 Android 识别（D21）改动的是系统环境，不适合写成 e2e：
- 单元测试：注册表与环境变量的读取通过注入的读取函数测试；各种“缺一半”的组合对应的选择与诊断；Android 判断；shell 的解析顺序（D28）。
- CI：D27 的两个 Termux job。
- O6 的探测 PR 提供真实环境下的证据。

**单元测试**（A–C 类）：`KeyRegistry` 的完整性（每个解析器认识的键都有一行）；合并规则；`RootPosition` 的序列化与求值；
兼容模块的转换；声明单元的构造。

**规范与文档**：
- SPEC-004（`docs/specs/manifest-semantics.md`）：§2 术语（构建的配置）；§9 第 7 条（发布写出条件行）、第 8 条（xlings）、
  第 10 条（W1–W4）；新增 W7 与旧写法迁移。
- `docs/specs/build-plugins.md`：单一写入者；`out_dir` 按配置；`resolution.json` 的 `outDir`。
- docs/04、docs/07（§4 改写为“前缀即语义”，删除 “There is no `[workspace.target.<triple>]`” 与按位置继承的表述）、
  docs/20（§9；Windows SDK 探测与首次运行；Android 默认值）、docs/30（占位文件与 `out_dir`），以及对应的 `docs/zh/`。

## 17. 自审记录

修订 2 写成后逐条核对，改动如下：

| 发现 | 处理 |
| --- | --- |
| 初稿假定旧版 mcpp 会拒绝未知的 `[workspace.*]` 表；实测 2026.10.8.1 静默忽略 `[workspace.toolchain]` | §3.6 改为依赖 `mcpp` 下限，加 note；本版本起未知表报错 |
| 初稿称 `cxxflags` 中的宏会被方言检查拒绝；该检查只针对特定方言参数 | §9 改为如实描述后果：std 预编译看不到宏 |
| 初稿称“只在其他平台不同的成员不再被拆分”，但今天它们本来就没有被拆分（条件行完全被忽略） | §3.6 改为描述新规则下的结果 |
| `--toolchain` 实际经由 `MCPP_TOOLCHAIN` 进入 | §8 写明两者等价及提示措辞 |
| 命名表改为逐键合并会改变 `[profile]`、`[target.<t>]` 的现有整表替换语义 | 只用于新写法；旧位置写法在兼容期内保持整表替换（§3.6） |

修订 3 的自审：

| 发现 | 处理 |
| --- | --- |
| 引导版本 2026.10.8.1 静默忽略新写法，mcpp 自己的 `mcpp.toml` 若在本版本迁移，自举构建会用错工具链 | mcpp 自身推迟到下一个版本迁移（§10、§11） |
| openkal 系列都不是工作空间 | 不需要迁移，只做验证（§11） |
| 带 `[package]` 的根上，旧位置的表首先是根包自己的声明；无条件警告会让 mcpp 自己的每次构建都报警告 | 警告只在成员经由旧规则取值时给出（§3.6） |
| mcpp-plugins 的测试硬编码生成目录 | 新增 `resolution.json` 的 `outDir` 作为稳定出口（D7） |
| Windows：显式声明时不会切换（回退条件要求 `tcSpec` 为空），但缺 SDK 时静默继续 | D18 改为在计划阶段拒绝 |
| Termux 第一次失败的硬链接原因只在 x86_64 r1 载荷上核对过 | §13.1 写明 aarch64 r3 需在重新打包时核对 |
| Termux 第二次运行用的很可能不是 2026.10.8.1 | 列为 O7，结论不基于推测 |

修订 4 的自审：

| 发现 | 处理 |
| --- | --- |
| 修订 3 依据读代码称“明确指定的工具链缺 SDK 时静默继续”；实测为计划阶段拒绝 | §12.1 改为实测结果；D18 改为“保持并写成不变式，修正措辞” |
| 修订 3 以为换成 musl gcc（D21）即可规避 Android 的问题；实测 musl-gcc 载荷同样含硬链接 | §13.2 的 R1：硬链接是所有受管工具链的共同问题（修订 5 起不再换工具链） |
| termux-docker 镜像没有 `/bin`，与 Android 10+ 真机不同 | CI 中先建 `/bin -> /system/bin`；同时修 mcpp 写死 `/bin/sh`（D28） |
| 模拟器会话 A 中静态程序无法执行，但用户真机能运行 mcpp | 不据此下结论，列为 O10 |
| cl.exe 表中“`.cppm` 实现单元用 `/TP`”未测 | 表中标为“未测” |
| xlings 2026.10.10.1 的兼容性只在 Linux x86_64 上冒烟测试过 | D30 以发布 PR 的完整 CI 验证 |

修订 5 的自审：

| 发现 | 处理 |
| --- | --- |
| 修订 4 以“换成 musl gcc”规避 Android 问题，与 review 结论（保持 llvm 23）不符，且 musl-gcc 载荷同样含硬链接 | D21 改为只识别不改默认；§13 改为修复根因 |
| “直接用最新的 xlings”是否够用，修订 4 只是推测 | round 7 实测 2026.10.10.1 同样失败，并在 xlings 源码中定位（`extract.cpp`、`installer.cpp:785`） |
| R2 只在 aarch64 termux-docker 中出现，x86_64 模拟器的宿主 shell 未受影响 | 两处都写明；修法相同，不依赖于哪种 shell 会失败 |
| x86_64 的端到端验证用了预装、去硬链接的载荷，而不是 xlings 实际安装 | §13.3 写明这是“R1 修复后的状态”；X1 发布后由 D27 的模拟器 job 以真实安装复验 |
| aarch64 没有真实 Android 环境的端到端证据 | O11：发布前在真机上验证 |

尚未消除的风险：
- `KeyRegistry` 覆盖全部键，是本版本改动最大的部分；完整性由单元测试保证（每个解析器认识的键都必须有一行）。
- X1–X3、P1、P2 在其他仓库中完成，需要在 mcpp 发布前合入并发布。
- aarch64 真机只能在设备上验证（O11）。

## 18. 实施

跨仓库的顺序（前一步完成才能做后一步的，用箭头标出）：

```
xlings：X1、X2、X3 → 发布 xlings ─┐
xim-pkgindex / xlings-res：P1（glibc r4 优先）、P2 ─┤
                                                  ├→ mcpp 发布 PR（release/<version>）→ 发布 → 生态验证
mcpp-plugins：测试改读 resolution.json 的 outDir ──┘                        （含 O11 真机验证）
```

mcpp 发布 PR 内部顺序：

1. D13、D30（引导版本与 xlings 版本；D30 等 xlings 发布）；
2. D4 的登记表与分层、兼容模块（其余 A 类工作的基础）；
3. D1–D3、D5、D9、D10、D24；
4. D8；
5. D6、D7、D25、D26；
6. D11、D15–D18；D21–D23、D28、D29；D27 的两个 CI job；
7. 规范与文档。

探测 PR #791 保留到本版本发布，用于复测 X1 之后的真实安装与 D27 的 job 原型。O11 在发布前完成。
