---
subject: build-program
status: active
---

# 构建依赖的一条管线：配置与产物两轴、包的实例、库接口、兼容系统与工作空间收尾（#797）

- 日期：2026-10-11。状态：修订 3，待 review。
- 基线：`main` `2bf99ee1`（2026.10.10.2 加 bootstrap 提交）。
- 证据：本地 Linux x86_64，gcc 16.1.0，用 `main` 构建的 mcpp（版本串 2026.10.10.1，本文涉及的代码路径与
  `main` 相同）。复现脚本 [2026-10-11-build-dependencies-one-pipeline-repro.sh](2026-10-11-build-dependencies-one-pipeline-repro.sh)
  只打印观察结果，不做断言；下文 R1–R8、W1–W2 引用其中的用例，§11 的性能基线另行记录了命令。
- 范围：mcpp#797 正文与评论；同一领域读代码与实测发现的缺陷；兼容系统；构建数据库对构建程序的描述；
  工作空间清单的语义。**一个 PR 完成**，不分阶段，不留临时方案。

**修订 3 的变化（review 结论）：**
- 修订 2 的五个决策点全部确认：插件由导入判定；有构建程序的包，其构建依赖的库默认编译（`host-module = false` 退出）；
  构建数据库的新集合命名；`[workspace.dependencies]` 的继承规则；`mcpp pack` 拒绝插件根。
- 构建程序、宿主实例与引擎模块**只**进入 `mcpp emit build-database` 的 S1 文档，这是 mcpp 与 mcppls 生态内部的联动；
  `compile_commands.json`（`mcpp build` 写出的与 `--spec compile-commands` 输出的）完全不变，不为其他 LSP 做适配（§7.2）。
- 新增"同一个库既在 `[dependencies]` 又在 `[build-dependencies]`"的验收矩阵（§3.5），作为本 PR 必须测通的条件。
  实测基线：同一消费方两张表（R2）失败；工作空间中两个成员分别使用（R2b）、经另一个库传递链接加直接构建依赖（R2c）
  今天可以工作，列为不得退化的保护用例。
- 自我 review 的修正见 §18。

**修订 2 的变化（review 结论）：**
- 移除新键 `lib`。SPEC-004 §5.2 规定已发布描述符中的键不得改名，而 `host-module` 已在已发布描述符中；
  它保留原名，含义精确为"构建程序使用这个依赖的库"，并且在 `[build-dependencies]` 中成为默认（§3.1、§3.2）。
- 评估插件生态（插件依赖插件与库、插件包自己的构建与测试）。插件单元仍由导入引擎接口判定；
  "插件单元经 `[dependencies]` 进入目标即拒绝"一条收窄为"进入**目标配置的实例**即拒绝"：配置沿依赖传播，
  插件依赖插件写在 `[dependencies]` 中，在宿主实例里成立；插件库作为根时以构建程序配置规划（§2.4、§4）。
  新增 R7（插件包今天无法单独构建）。
- 兼容系统做成登记表：每条兼容读法有 id、弃用版本、移除下限与规范写法；警告、文档、`mcpp why compat`、
  测试与 CI 都由这张表派生（§6）。移除下限 `2027.1` 已确认。
- 构建数据库与 `compile_commands.json` 描述构建程序、宿主实例与引擎模块，为 mcppls 准备基础设施（§7，R8）。修订 3 收窄为只进 S1 文档。
- 新增工作空间清单语义的完整对照（虚拟根与带根包的根），以及两个新发现：虚拟根旁的 `build.mcpp`
  被静默忽略（W1）、`[workspace.dependencies]` 中的 `features` 被静默丢弃（W2）（§8）。
- 新增性能基线与预算（§11）、可观测性（§12）。
- 宿主图从"一个合成包含多个 `bin`"改为"一个合成工作空间，每个构建程序一个合成成员"，每个程序的依赖与
  链接各自正确，共享包仍只编译一次（§4.3，自我 review 的修正）。
- 程序缓存身份对 path 包改用 stat 摘要加内容回退，避免今天检查 host 模块条目时观察到的 52 ms（§4.4、§11）。

## 0. 结论

1. **两条轴。** section 决定**配置**：`[dependencies]`、`[dev-dependencies]` 为目标配置，`[build-dependencies]`
   为构建程序配置（宿主）。边上的请求决定**产物**：库、`tools`、`artifacts`。
   `host-module` 不再决定放置，只表示"构建程序使用这个库"，并且在 `[build-dependencies]` 中默认成立。
2. **包的实例。** 节点是 `(包, 配置)`。两张表中同名的包得到两个实例；版本统一，特性按配置激活。
3. **构建程序图。** 每个宿主配置一张图，每个构建程序是其中一个合成成员，依赖是本包 `[build-dependencies]`
   中的库。这张图走现有的子构建与工作空间分组规划、由 ninja 构建：依赖按完整的包编译，`build.mcpp`
   本身也在其中编译，因此 BMI 与使用它的代码由同一个 flag 生成器产生。
   `host_module_compile`、`host_module_store`、单独编译接口单元的路径整体删除。
4. **插件。** 导入 `mcpp.core` 的单元是插件单元，只能出现在构建程序配置的实例中。
   插件可以依赖插件与普通库（写在 `[dependencies]`，随配置传播）；插件库作为根时以构建程序配置规划，
   所以 `mcpp build`、`mcpp test` 在插件包里直接可用。
5. **库接口一个定义。** 接口根（`[lib] path`、Form B 新增的 `lib`、约定路径）的 `export import` 闭包，
   加上已启用特性加入的接口单元，就是公开模块；目标、构建程序、`mcpp pack`、校验读同一个函数。
6. **兼容系统。** 一张登记表，按日期版本给出弃用与移除下限（`2027.1`），警告、文档、报告、测试、CI 都由它派生。
7. **可观测性与编辑器支持。** `mcpp emit build-database`（S1）描述构建程序、宿主实例与引擎模块，供 mcppls 使用，`compile_commands.json` 不变；`mcpp why build-programs`、
   `mcpp why compat` 与日志标签让每一步可以解释。
8. **工作空间。** 修复 R4、R5、W1、W2；虚拟根与带根包的根逐表写明语义。

## 1. 问题与证据

### 1.1 复现

| 编号 | 用例 | 观察（mcpp 2026.10.10.1） | 原因 |
|---|---|---|---|
| R1 | 头文件库的模块包装（toml++ 形状），`[build-dependencies]` 中 `host-module = true` | `fatal error: hl/hl.hpp: No such file or directory` | 宿主编译只有编译器、标准、`base` 与已有 BMI（`host_module_compile.cppm:35`、`:545`），不带提供方的编译上下文 |
| R2 | 同一包在两张表中（include 改为相对路径以绕过 R1） | 目标侧 `hl: error: failed to read compiled module` | 两次声明合并为一条边（`graph.cpp:279`），`hostModule` 被并入（`:300`），`reachable(targetEdgesOnly)` 跳过它（`features.cpp:1304`），`sources` 被清空 |
| R3 | `mid` 的 `[build-dependencies]` 为 `hl` 请求特性 `x`，根的 `[dependencies]` 也依赖 `hl` | 目标编译带上 `-DHL_FEATURE_X` | `aggregatedRequest`（`features.cpp:73`）对所有入边取并集，不分配置 |
| R4 | 带 `[package]` 的根，`[workspace.xlings.overrides]`，根包以 path 依赖成员 | `error: 'sub' states [xlings.overrides] … it is a dependency of this build` | `merge_xlings`（`project.cppm:253`）在每个位置复制 overrides，依赖位置随后被 `sources.cpp:334` 拒绝 |
| R5 | 同上，改为根自己的 `[xlings.overrides]`（位置读法） | 普通 `mcpp build` 根包也出现 `manifest/workspace-position` 警告，随后同样失败 | 依赖位置照样走位置读法并发出警告，而根包自己的条目本来就在这次构建的环境中 |
| R6 | path host 模块所 include 的头文件从 42 改为 43 | 构建程序不重编不重跑，产物仍是 42 | 程序缓存身份只哈希接口文件（`build_program.cppm:1269`） |
| R7 | 一个插件包（`import mcpp;`）作为根构建，即 `examples/08-build-rules/rules-embed` 的形状 | `module 'mcpp' imported but not provided`，构建失败 | 根总是以目标配置规划，引擎模块只在构建程序的编译中存在 |
| R8 | `mcpp emit build-database` 一个带 `build.mcpp`、导入 host 模块的工程 | 翻译单元只有 `main.cpp` 与 `std.cc`；`build.mcpp`、host 模块与引擎模块都不出现 | 构建程序的编译在计划之外。本 PR 只改 S1 文档（§7.2） |
| R2b | 工作空间中 m1 的 `[dependencies]` 与 m2 的 `[build-dependencies]` 使用同一个库 | 两者都工作（`ok: m1 42`、`ok: m2 42`） | 两条边属于不同的消费方，没有合并；保护用例 |
| R2c | 根经库 `lib` 传递地链接 `hl`，同时把 `hl` 写在自己的 `[build-dependencies]` 中 | 工作（`ok: 42 42`） | 同上；保护用例 |
| W1 | 虚拟根（没有 `[package]`）旁放一个会失败的 `build.mcpp` | 没有任何输出，构建成功 | 包表在虚拟根上会被报告（"acts on no package"），`build.mcpp` 不在这份清单里 |
| W2 | `[workspace.dependencies] hl = { path = …, features = ["x"] }`，成员 `hl.workspace = true` | 特性 `x` 没有生效，也没有警告 | `merge_workspace_deps`（`project.cppm:53`）只复制 version、path、git |

### 1.2 读代码所得

- 同一条轴两种读法：`features.cpp:1304` 读 `hostModule || buildOnly`，`scan.cpp:94` 只读 `buildOnly`。
- docs/05 说"表决定是否进入目标，请求只决定产物"，docs/30 说 `host-module = true` 的依赖写在 `[dependencies]` 里也不进目标；
  代码实现的是后者。`examples/08-build-rules/app` 正是 `[dependencies] … host-module = true` 的写法。
- 导出面两套定义：普通路径以 `sources` 中所有 `export module` 单元为可导入，接口根只用于警告
  （`validate.cppm:120`、`scan.cpp:951`）；宿主路径要求接口根并只取列出的接口单元（`features.cpp:920`）。
  SPEC-008 的公开模块计算在 `scan.cpp:951-970` 内联，`pack/library_pipeline.cppm:244` 另有一份。
- Form B 没有 `lib` 键（`xpkg.cppm:258`）。index 中 7 个用 `generated_files` 合成接口单元的包都无法给出接口根。
- 规则依赖规则的写法（docs/30）让规则的**接口**导入它 `[build-dependencies]` 中的包，
  `examples/08-build-rules/rules-tidy` 即是：这是旧模型下唯一可行的写法。

## 2. 模型

### 2.1 两条轴

| 轴 | 由什么决定 | 取值 | 含义 |
|---|---|---|---|
| **配置** | section | 目标：`[dependencies]`、`[dev-dependencies]`；构建程序：`[build-dependencies]` | 依赖的代码为哪个配置编译、链接进谁 |
| **产物** | 边上的请求 | 库（`host-module` 在构建程序配置中控制它）、`tools = [...]`、`artifacts = [...]` | 要依赖包的哪些产物 |

- 配置沿 `[dependencies]` 边向下传播：一个宿主实例的 `[dependencies]` 也是宿主实例（Cargo、Bazel、Conan 相同）。
- 一个包的 `[build-dependencies]` 只服务**它自己的**构建程序。
- `tools` 与配置无关，总是为构建机器构建（SPEC-004 §10.1）；`artifacts` 以消费方的目标构建（§10.3），只出现在目标配置中。

### 2.2 包的实例

- 节点是 `(包, 配置)`。被目标边到达的包有目标实例，被构建程序配置的库边到达的包有宿主实例，两者可以并存。
- **一个包一个版本。** 主图的解析已经覆盖两种配置（worklist 会走 `buildDependencies`），锁文件不变；宿主图以主图的解析为钉（§4.3）。
- **特性按配置激活**（R3，Cargo resolver v2）。构建环境（`feature-xlings`）取两种配置激活结果的并集：
  规则在消费方构建中运行动作所需的载荷，属于消费方的构建环境（SPEC-004 §4.3"两条轴的并集"）。

### 2.3 构建程序

构建程序是构建程序配置中的一个程序：主文件是 `build.mcpp`（或由规则特性合成的程序），依赖是本包构建程序配置中的库边，
再加上经 `reexport` 传递来的库（provision 图）。它的编译与链接走计划与 ninja；它的**运行**（环境契约、指令协议、
运行缓存、诊断回放）不变。

### 2.4 插件

**判定。** 导入 `mcpp.core`（或永久等价写法 `mcpp`，SPEC-007 R9.1）的单元是**插件单元**。引擎接口只在构建程序配置中存在。

**规则。**

| 编号 | 规则 |
|---|---|
| P1 | 插件单元只能出现在构建程序配置的实例中。一个**目标配置的实例**的编译集合中出现插件单元时，规划期拒绝 |
| P2 | 配置沿 `[dependencies]` 传播。插件 A 用到插件 B 或普通库 C，在 A 的 `[dependencies]` 中声明 B、C；A 被消费方的 `[build-dependencies]` 到达时，A、B、C 都是宿主实例 |
| P3 | 一个包的**库**含有插件单元（在当前启用的特性下）时，它作为构建的根（`mcpp build`、`mcpp test`、`-p` 选中的成员、tools 子构建的根）以构建程序配置规划：宿主工具链，`--target` 指向他处时拒绝并写出原因 |
| P4 | 一个包的**程序目标**（`bin`、`app`）的单元导入引擎接口，而该包不是 P3 的插件库时，规划期拒绝：引擎接口属于构建程序，而 `build.mcpp` 才是构建程序 |
| P5 | `mcpp pack` 拒绝以构建程序配置规划的根：插件以源码分发，宿主 BMI 不可移植 |

**P1 的消息**写出：单元、它所属的包、到达该包的边所在的 section，以及正确的写法。消费方写错 section 时，
指向 `[build-dependencies]`；依赖链中间某个**目标**库错误地依赖了插件时，写出这条链。

**为什么由导入判定，而不是声明（例如 Cargo 的 `[lib] proc-macro = true`）。**

| | 由导入判定（采用） | 包级声明 |
|---|---|---|
| 事实所在 | 单元的导入，一处 | 声明与导入两处，可能不一致 |
| 混合包（`mcpp:plugins`：基础库 `mcpp.plugins` 只导入 `std`，被 `mcpp-embed` 工具使用；插件单元在特性之后） | 按启用的特性自然成立 | 包级声明无法表达；需要逐特性声明 |
| 已发布的插件包 | 不需要改动 | 都要加声明 |
| 与 2026.10.10.1"模块角色取自声明"的原则 | 一致：角色取自单元自己的声明 | 另起一个来源 |
| 根的配置需要在工具链解析之前确定 | 需要一次预扫描（§4.8，§11 有预算） | 加载期即可知 |

预扫描的代价是确定的、可测量的；声明的代价是一个事实有两个来源。因此采用由导入判定。

**生态场景评估。**

| 场景 | 写法 | 结果 |
|---|---|---|
| S1 第三方插件基于 L2 `mcpp:plugins` | 插件的 `[dependencies.mcpp] plugins = { features = ["plugins-core"] }` | 宿主图中两者都是宿主实例 ✔（旧写法 `[build-dependencies] … host-module = true` 按兼容读法成立，带警告） |
| S2 插件用普通库（toml++、fmt） | 插件的 `[dependencies]` | ✔ 今天不可能（R1） |
| S3 插件用另一个插件，后者有 `tools` | 照常 | ✔ `tools` 不受影响 |
| S4 插件包自己构建、测试 | `mcpp build` / `mcpp test` | ✔ P3；今天失败（R7）。插件的单元测试可以写成普通的 `tests/*.cpp`，导入 `mcpp.core` 与测试工具，不必再借消费方的构建程序 |
| S5 库把插件转交给它的消费方 | 库的 `[build-dependencies] p = { reexport = true }` | ✔ provision 传播不变 |
| S6 两个插件依赖同一个库、特性不同 | — | ✔ 同一宿主图内特性合并（与 Cargo 同一配置内的合并一致） |
| S7 两个消费方的标准不同 | — | ✔ 不同宿主配置组，各一张图 |
| S8 混合包 `mcpp:plugins` | 不变 | ✔ 目标侧只用基础库；构建程序配置启用插件特性 |
| S9 插件包自己有 `build.mcpp`，用另一个插件 | 插件的 `[build-dependencies]` | ✔ 宿主图中递归一层 |
| S10 菱形：消费方的两个构建依赖都依赖 `mcpp:plugins` | — | ✔ 宿主图中一个实例 |
| S11 两个插件导出同名模块 | — | 宿主图规划期报模块冲突（现有规则），写出两个包 |
| S12 目标库误用 `[dependencies]` 依赖插件 | — | P1 拒绝，消息写出依赖链 |

### 2.5 库接口（SPEC-008 的延伸）

一个函数 `library_interface(包, 配置, 已启用特性, 模块图)` 给出：

| 量 | 定义 |
|---|---|
| 接口根 | `[lib] path`；Form B 的 `lib`；都没有时为约定路径 `src/<包名末段>.<模块接口扩展名>`（文件存在时）。至多一个 |
| 公开模块 | 接口根声明的模块及其 `export import` 传递闭包，**加上**已启用特性的 `sources` 中声明主模块接口的单元及其转出闭包 |
| 可导入模块 | 该配置编译集合中的全部导出主模块（可导入而不公开的模块产生 W3，SPEC-008 第一阶段） |
| 发布闭包 | 公开模块接口传递导入的全部单元（SPEC-008 I5） |

- 特性一项修订 SPEC-008 I2：特性是否启用由消费方决定，`export import` 无法表达"只在特性启用时转出"，
  因此特性加入的接口单元就是该特性的接口。`mcpp:plugins` 的 `rules-*`、`plugins-core` 正是这种形状。
- 读者全部改用这个函数：目标侧 W3（`scan.cpp`）、构建程序的导入检查、`exports` 断言（`validate.cppm`）、
  `mcpp pack` 的发布闭包（`library_pipeline.cppm`）、`rule_module` 校验、诊断、构建数据库（§8）。

### 2.6 行业对照

| 工具 | 构建期依赖 | 同包两用 | 管线 | 特性/配置隔离 | 插件 |
|---|---|---|---|---|---|
| Cargo | `[build-dependencies]`、proc-macro | 两次编译 | 同一条，换 target | resolver v2 分开 | `[lib] proc-macro = true`，就是 host 库 |
| Bazel | `cfg = "exec"` | 两次配置 | 同一条 | 每配置一套 flag | exec 配置下的依赖 |
| Conan 2 | `tool_requires` / traits | 两个 context、两个节点 | 同一条，两套 profile | 按 context | 不单独建模 |
| vcpkg / Meson / Zig | `host: true` / `native: true` / `b.graph.host` | 各一份 | 同一条 | 按 triplet / machine / target | 不单独建模 |

mcpp 多一条约束：BMI 不能跨编译器、标准或语言选项使用。宿主实例必须与 `build.mcpp` 由同一个工具链、同一个标准、
同一个 flag 生成器编译，这就是 §2.3 让 `build.mcpp` 进入宿主图的原因；也是不采用 #797 A 方案（交给 tools 子构建、
各自为根）的原因：多个宿主库共享一个依赖时，各自为根会编译并链接多份。

## 3. 清单语义与使用场景的变化

### 3.1 `host-module`：不加新键

- **不加 `lib` 键。** SPEC-004 §5.2：已进入已发布描述符的键禁止改名。`host-module` 已在 index 与
  `mcpp:plugins` 的消费方清单中，改名只会给每个用户制造一次迁移和一段两种拼法并存的时期，
  换来的只是名字更贴切。
- **含义精确化：** `host-module = true` 表示"构建程序使用这个依赖的库"：它的公开模块可导入，公开头文件可见，
  对象参与链接。名字只说了模块，是历史原因；SPEC-004 §5.2 要求把这类不一致写成规则并注明来源，本文照做。
- **默认值：** 在 `[build-dependencies]` 中，边没有列出 `tools`、`artifacts` 时，`host-module` 默认为 `true`；
  列出时默认为 `false`，两者都要时写 `host-module = true`（与今天 `mcpp:plugins` 的
  `features = […], host-module = true, tools = ["mcpp-embed"]` 写法相同）。请求声明了 `rule_module` 的特性时，
  隐含 `true`（与今天相同）。`host-module = false` 可以显式退出（只要依赖的目录或载荷时）。
- 在 `[build-dependencies]` 中写 `host-module = true` 而没有 `tools` 是在陈述默认值，**不给诊断**
  （与 `default-features = true` 相同）。

### 3.2 使用场景：之前与之后

| 场景 | 之前 | 之后 |
|---|---|---|
| 构建程序用一个普通库（toml++、nlohmann.json） | 要写 `host-module = true`；头文件库的模块包装编不过（R1）；Form B 包缺接口根直接拒绝 | `[build-dependencies] tomlplusplus = "3.4.0"`，`import tomlplusplus;` 即可 |
| 构建程序用一个 C 库（cJSON）或带非接口源文件的库 | 不可能：只编译接口单元，没有链接 | 完整编译并链接 |
| 构建依赖只用来取工具（protoc） | `{ tools = ["protoc"] }` | 不变，不编译库 |
| 既要工具又要库（`mcpp:plugins` + `mcpp-embed`） | `host-module = true, tools = [...]` | 不变 |
| 规则特性（`features = ["rules-spirv"]`） | 隐含 `host-module` | 不变 |
| 同一个包既链接进目标，又在构建程序中使用 | 两张表各写一次，构建失败（R2） | 两张表各写一次，两个实例，各自正确 |
| `[dependencies] x = { host-module = true }`（`examples/08-build-rules/app`） | 不进目标，只给构建程序 | 行为不变，警告建议移到 `[build-dependencies]`；`2027.1` 起移除这种读法 |
| 插件依赖插件 | 写在插件的 `[build-dependencies]`（`host-module = true`），由接口导入 | 写在插件的 `[dependencies]`；旧写法行为不变并给出警告 |
| 插件包自己 `mcpp build` / `mcpp test` | 失败（R7） | 以构建程序配置构建与测试 |
| 只为取目录而写的 `[build-dependencies] x = "1.0"`（x 是库），且本包有构建程序 | 不编译 | 编译 x 的宿主实例（与 Cargo 相同）；只要目录时写 `host-module = false`。没有构建程序的包不受影响：宿主图只为构建程序存在 |
| 构建依赖请求的特性 | 泄漏进目标实例（R3） | 只作用于宿主实例；依赖这种泄漏的工程需要在目标边上写出特性 |
| Form B 包声明接口根 | 无法声明 | `lib = "mcpp_generated/fmt_module.cppm"` |

### 3.3 Form B 的 `lib` 与 `modules`

- **`lib = "<路径>"`**（新，属于新增而非改名）≡ `[lib] path`。路径规则同 `sources`：相对版本目录，接受 `*/` 包装层。
- **`modules = { ... }`** ≡ `[modules] exports`：对公开模块集合的**断言**。存在时与 §2.5 派生的公开模块（基础部分）逐项比较；
  不一致时本包构建警告、`--strict` 下错误、作为依赖时不报。它不指定接口根：一个事实一个声明位置。
- 更旧的引擎把 `lib` 当作未知键警告并跳过（`xpkg.cppm:238`），目标使用不受影响；index 以 `min_mcpp` 门控即可。

### 3.4 同一个包写在两张表中

两条边、两个实例，**不合并**。同一 section 内的多次声明（无条件与 `[feature-deps.<f>]`、`[target.<sel>.dependencies]`）
照旧合并请求（`graph.cpp:617`）。docs/05 中 "the ordinary declaration wins … requested of the one edge" 一段改写。

### 3.5 同一个库同时用于目标与构建程序：验收矩阵

这是本 PR 必须测通的条件（e2e 903）。库 `hl` 是头文件库加模块包装的形状：接口单元的全局模块片段以
`#include <hl/hl.hpp>` 引入自己的头文件，`[build] include_dirs = ["include"]`，带一个特性 `x`。

| 编号 | 写法 | 期望 | 今天 |
|---|---|---|---|
| M1 | 同一消费方：`[dependencies] hl` 与 `[build-dependencies] hl`（不写 `host-module`） | 目标导入 `hl`、`build.mcpp` 导入 `hl` 都成功；两个实例，目标实例的 BMI 在目标计划中，宿主实例的在宿主图中 | 失败（R1，绕过 R1 后仍失败：R2） |
| M2 | 同 M1，构建依赖显式写 `host-module = true` | 与 M1 相同（陈述默认值，无诊断） | 失败（R1、R2） |
| M3 | 同 M1，两侧特性不同：目标边 `features = []`，构建依赖边 `features = ["x"]` | 宿主实例有 `x`，目标实例没有（`-DHL_X` 只出现在宿主图的编译中） | 目标实例收到 `x`（R3） |
| M4 | 同 M1，`hl` 来自本地 index 的 Form B 并声明 `lib` | 与 M1 相同；版本只解析一次，`mcpp.lock` 中一条 | 失败（无接口根） |
| M5 | 工作空间：m1 的 `[dependencies] hl`，m2 的 `[build-dependencies] hl` | 两者都工作 | 工作（R2b，绕过 R1 时） |
| M6 | 根经库 `lib` 传递地链接 `hl`，并在 `[build-dependencies]` 中直接写 `hl` | 两者都工作 | 工作（R2c，绕过 R1 时） |
| M7 | 一个依赖包 `d` 的 `build.mcpp` 导入 `hl`（`d` 的构建依赖），而根在 `[dependencies]` 中链接 `hl` | 两者都工作，`hl` 的目标实例只编译一次、宿主实例只编译一次 | 依赖的构建程序走同一条 host 模块路径，受 R1 限制 |
| M8 | M1 在交叉构建中（`--target aarch64-linux-gnu`） | 目标实例为 aarch64 编译，宿主实例为宿主编译，构建程序在宿主上运行 | 失败（R1、R2） |
| M9 | M1 的构建数据库 | S1 文档中 `hl`（目标）与 `hl:host`（宿主）两个集合，参数各自与实际编译一致；`compile_commands.json` 与修订前逐字相同（只有目标单元） | 无宿主集合 |
| M10 | M1 之后编辑 `hl/include/hl/hl.hpp` | 目标实例与宿主实例都重编，构建程序重跑，产物带新值 | 构建程序不重跑（R6） |

两个实例各自提供模块 `hl`，但处在两次规划（目标计划与宿主图）中，不构成同名模块冲突；依赖缓存的键因配置
（工具链、profile、特性）不同而不同，两份条目并存。

## 4. 引擎架构

### 4.1 边模型

`DependencyEdge`（`state.cppm:120`）：

- 删除 `hostModule` 字段；`buildOnly` 改为 `configuration`（`Target | BuildProgram`），由 section 决定并沿 `[dependencies]` 继承
  （今天 `buildOnly` 已是继承的：`graph.cpp:1926`）。
- 新增 `wantsLibrary`：§3.1 的默认规则在清单规范化时求值，兼容读法也在那里完成，图代码看不到 `host-module` 的历史含义。
- `recordDependencyEdge` 的合并键加上 `configuration`；`graph.cpp:279-306` 的跨 section 合并删除（R2）。
- 实例：`reachable` 只看 `configuration`，结果写入 `PackageRoot::targetInstance`；`compilesHere(i)`（`graph.cpp:2031`）
  加上这个条件；`plan.cppm:2288`、`:2631` 与 `scan.cpp:94` 改读 `compilesHere`。**不再修改清单**（删除清空 `sources` 的做法）。

### 4.2 特性按配置激活

`activeFeaturesByPackage` 分为目标与构建程序两张表；`aggregatedRequest` 只汇总同一配置的入边。编译与 `[features]`
的折叠只用目标表；provision 与 `feature-xlings` 用两表的并集；构建程序表交给宿主图作为输入，宿主图自己的激活结果
必须与之相同，不同即为内部错误（带两边的集合写入日志）。

### 4.3 宿主图

**分组键。** 构建程序配置的工具链身份（指纹）与 C++ 标准（所属包的 `cppStandard`）。标准不同即为不同组（e2e 857 判据 B）。

**合成工作空间。** 每组一个内存中的虚拟工作空间（不落盘），每个构建程序一个合成成员：

```toml
# 合成成员 <ns>.<name> 的清单（身份在保留命名空间 `_build` 下：`_build.<ns>.<name>`）
[package]
name     = "<ns>.<name>"
standard = "<组的标准>"
[build]
sources  = []                          # 明确为空：成员的根目录是消费方包的根目录，不能取到它的 src/**
[dependencies]                         # 该包构建程序配置的库边，加上 reexport 传来的；路径锚定到该包
"<ns>".<name> = { version = "=<主图解析的版本>" | path = "…", features = [...], default-features = … }
[targets.build-program]
kind = "bin"
main = "build.mcpp"                    # 或合成的规则程序（写到宿主图工作目录中）
```

用工作空间而不是一个多 `bin` 的包：每个程序的依赖、导入可见性与链接各自正确；共享的包在同一配置组的一次规划中
只出现一次（SPEC-005 R3.3 已有的工作空间分组语义），因此仍只编译一次（e2e 857 判据 A）。

**引擎包。** `mcpp` 与 `mcpp.core` 的接口文本写入按版本区分的只读目录 `$MCPP_HOME/engine/<mcpp 版本>/modules/`
（`mcpp.cppm`、`mcpp_core.cppm`），作为引擎包。宿主图中有单元导入它时，它是该单元所在包的隐式依赖。依赖缓存把它当作
不可变来源，全局条目仍是 `build-cache/v1/pkg/_engine/mcpp-build-module@<版本>/<key>/`，`mcpp cache list/verify/gc`
看到的不变。源文件目录与 BMI 条目分开，构建数据库指向前者（§8）。目标配置中没有引擎包（P1）。

**执行。** 现有子构建入口 `prepare_build(BuildOverrides)`（与 `step6_build_tool`，`features.cpp:1620`，同一个入口）：

| 覆盖项 | 值 |
|---|---|
| `preloaded_workspace` *(新)* | 合成工作空间（根与成员的清单都在内存中） |
| `target_triple` | `""`（宿主） |
| `inherited_build_program_toolchain` *(新)* | 已解析的工具链对象（共享指针），不从字符串重新解析，保证与今天编译构建程序的工具链相同 |
| `resolution_pins` *(新)* | 主图对闭包中每个包的解析（身份 → 版本或来源）。工具链阶段的程序（SPEC-007 R9.9）在主图解析之前运行，没有钉 |
| `features` | §4.2 的构建程序表 |
| profile | 内部 profile：`opt = 0`、`debug = false`，与今天构建程序的编译相同 |
| 链接 | 今天的"静态助手"策略（musl、MinGW，`build_program.cppm:1258`）由合成成员陈述；Windows 的 UTF-8 清单（#693）由合成成员的资源陈述 |
| `work_dir` | `<工作区>/target/.build-mcpp/host/<组键前 12 位>/`（路径短，mcpp#641） |
| `tool_depth` | 加 1，复用嵌套上限与环检测 |

**并发。** 配置组可以在多个线程上构建（`ui.cppm:473`）。宿主图的工作目录按组键命名，用 `platform::fs::FileLock`
（阻塞获取，带等待日志）互斥；两个目标配置组的宿主配置相同时共用一张宿主图，后到的一方等待后 ninja 无事可做（e2e 912）。

**目标（goal）。** 只构建缓存失效的程序（§4.4）。index 包经依赖缓存全局复用；path 包的产物留在工作区（e2e 857 判据 C、D 的性质不变，
位置变为 `target/.build-mcpp/host/…`）。

**宿主实例自己的构建程序**在宿主图的 `prepare_build` 中按同样机制运行（递归一层，§2.4 场景 S9）。

**`.mcpp` 源文件**在计划中按 C++ 翻译单元处理，命令带强制语言参数（GCC/Clang 的 `-x c++`，cl 的 `/TP`），与今天相同（`dial.forceCxxLangArgv`）。

### 4.4 构建程序：编译交给宿主图，运行不变

- `run_build_program_impl` 中的 `compile_binary`（`build_program.cppm:1533` 起：std 模块暂存、`mcpp` 模块、host 模块循环、
  链接策略、响应文件）由"宿主图产出的程序路径"替代。运行、指令解析、运行缓存、`report_stated_diagnostics`、
  `run_answering_requests` 不变。
- **缓存身份**（替代 `compilerIdentity`）：构建程序工具链指纹、标准、引擎版本、`build.mcpp` 的内容摘要，以及闭包中每个包的
  `(身份, 版本, 来源摘要, 构建程序配置的特性)`。index 包的来源摘要是版本加修订；path 包用源树的 **stat 摘要**
  （路径、大小、修改时间），stat 有变化时再算内容摘要并记录，规则与 ninja 的重启判断相同；源树范围与 tools 库键相同
  （不含带自己 `mcpp.toml` 的子目录，SPEC-004 §10.1）。身份在主图解析后即可算出，**不需要规划宿主图**；全部程序新鲜时
  宿主图不规划（R6 由此修复：头文件的改动改变 stat 与内容摘要）。
- 判定为失效时，日志写出**哪一个分量**变了（§12）。
- **预编译线程**（`target_side.cpp:2080-2130`）删除：同一组失效的程序在一次 ninja 中并行编译。
- **编译失败**按成员归属到包：`build.mcpp of <pkg> failed to compile`，附编译器输出、宿主图工作目录与单独复现的命令
  （tools 子构建的 `subContext` 先例）。
- R9.7（未启用特性提供的模块要写出特性）与 e2e 624（缺失模块要写出键）的诊断移到宿主图扫描阶段，措辞不变。

### 4.5 provision

`Kind::HostModule` 改名为 `Kind::HostLibrary`，由 `configuration == BuildProgram && wantsLibrary` 的边产生；
`reexport` 的不动点（`provisions.cppm:142`）不变。`hostModuleProvidersByConsumer`、`hostModulesByConsumer`、
`dormantFeaturesByConsumer` 由每个程序的依赖集（合成成员的输入）替代。

### 4.6 删除

| 删除 | 由谁承担 |
|---|---|
| `src/build/host_module_compile.cppm`（622 行）、`src/build/host_module_store.cppm`（381 行）、`tests/unit/test_host_module_store.cpp` | 宿主图与依赖缓存 |
| `host_module_units`（`features.cpp:920`）及 `step6_host_module_registration` 中的排序、冲突、命名 | 宿主图的模块图与 `library_interface` |
| `buildTimeOnly` 与清空 `sources` | `targetInstance` 与 `compilesHere` |
| 构建程序的预编译线程 | ninja |
| docs/30 "units are otherwise compiled alone, so they import std, mcpp and nothing else" | 不再成立 |

### 4.7 文件布局

`prepare/features.cpp` 现为 2277 行（上限 2500）。宿主图的分组、合成工作空间、身份与子构建放进新文件
`src/build/prepare/host_graph.cpp`。`library_interface` 是新模块 `mcpp.build.library_interface`：它需要模块图，
而 `mcpp.manifest.types` 不能再增加导入（`toml.cppm:5180` 记录的 GCC 16.1 ICE）。兼容登记表在 `mcpp.pm.compat`（§6）。

### 4.8 根的配置（P3）

工具链解析之前，对根（与 `-p` 选中的成员）当前启用特性下的**库**源文件做一次预扫描，只读导入声明（与主扫描同一个文本扫描器）。
预扫描结果按文件保存在 `PrepareState` 中，主扫描直接取用，同一文件不读两次。含插件单元则以构建程序配置规划，
状态行写出 `Configuration build-program (host <三元组>, <工具链>)`，日志写出触发的单元。

## 5. 诊断

按 `src/diag.cppm` 的不变量与已有先例分级。

| 情形 | 级别 | 消息要点 |
|---|---|---|
| P1：目标配置的实例中出现插件单元 | 规划期拒绝 | 单元、包、边所在的 section 与依赖链，以及 `[build-dependencies]` 写法 |
| P3：插件库作为根且 `--target` 指向他处 | 规划期拒绝 | 插件在构建机器上运行，`--target` 不适用；写出触发的单元 |
| P4：程序目标导入引擎接口 | 规划期拒绝 | 引擎接口属于构建程序，指向 `build.mcpp` |
| P5：`mcpp pack` 构建程序配置的根 | 拒绝 | 插件以源码分发 |
| 构建程序导入的模块没有依赖提供 | 规划期拒绝（今天已是） | 候选的 `[build-dependencies]` 条目；未启用特性时写出特性（R9.7） |
| 构建程序导入仅作为他人前提到达的模块 | 规划期拒绝（今天已是） | 措辞不变 |
| 构建程序导入可导入但不公开的模块 | 警告 `build/interface`（SPEC-008 W3，今天只对目标） | 与目标侧相同 |
| 显式声明的接口根不存在 | 本包构建：错误；作为依赖：警告 | 只对能修改它的包报错 |
| `exports` / Form B `modules` 与派生公开集合不一致 | 本包构建：警告，`--strict` 下错误；作为依赖：不报 | 列出多出与缺少的名字 |
| `rule_module` 不是该特性的接口单元之一 | 加载期拒绝 | 列出该特性实际提供的模块 |
| 兼容读法 | 警告 `manifest/deprecated`（只对能修改的清单；`--strict` 下错误） | 由登记表生成：读法、规范写法、弃用版本、移除下限（§6） |
| 宿主图中某个程序编译失败 | 构建期错误 | 包、`build.mcpp`、编译器输出、工作目录、复现命令 |
| W1：虚拟根旁的 `build.mcpp` | 警告，`--strict` 下错误（与虚拟根上的包表相同） | 它不属于任何包；写进某个成员 |
| W2：`[workspace.dependencies]` 中的边请求键（§8.2） | 警告，`--strict` 下错误 | 该键属于每个成员自己的边 |

"A package offering build rules must have a lib root" 这条错误消失（接口根不再是宿主消费的前提）。

## 6. 兼容系统

### 6.1 原则（写入 docs/51 §Deprecation）

- 被弃用的写法或读法照常工作，每次使用给出警告，写出规范写法、弃用版本与移除下限。
- 移除下限是一个 CalVer 月份；从弃用它的发布起至少两个完整日历月；只在下限及以后的发布中移除。
- docs/51 "`mcpp.toml` keys: Additive" 的例外只有登记表中的条目。
- 警告只对能修改该清单的包给出（根、工作空间成员、工作空间内的 path 依赖）；依赖包中的用法由 `mcpp why compat` 报告，
  index 由 `mcpp xpkg parse --format json` 的 `compat` 字段在 CI 中检查。

### 6.2 登记表

`mcpp.pm.compat` 中一张表，每条一行：

| 字段 | 例 |
|---|---|
| `id` | `workspace-position` |
| `since` | `2026.10.10.1` |
| `floor` | `2027.1` |
| `what` | `[toolchain] on a workspace root reaching members by position` |
| `canonical` | `[workspace.toolchain]` |
| `domain` | `manifest/workspace-position` |
| `docs` | `docs/07-workspace.md#…` |

本 PR 登记的条目：

| id | 读法 | 规范写法 |
|---|---|---|
| `workspace-position` | 工作空间根自己的 `[toolchain]`、`[indices]`、`[profile.*]`、`[target.*]` 标量、`[xlings]` 按位置到达成员 | `[workspace.X]` |
| `host-module-in-dependencies` | `[dependencies] x = { host-module = true }`，以及请求规则特性的 `[dependencies]` 边（Form B `deps` 同） | `[build-dependencies]` |
| `plugin-imports-from-build-dependencies` | 插件的库单元导入本包 `[build-dependencies]` 中 `host-module = true` 的条目 | 写进本包的 `[dependencies]` |
| `dotted-package-name` | `name = "ns.name"`、`"ns.name" = "…"`（已有的旧兼容，纳入登记表） | `namespace = …` |

### 6.3 由登记表派生

| 派生物 | 内容 |
|---|---|
| 警告文本 | "`<what>` is deprecated since mcpp `<since>` and is removed from mcpp `<floor>`. Write `<canonical>`." |
| `mcpp why compat`（新话题，`--format json` 为 `mcpp.why.compat`） | 本工程及其依赖用到的每条兼容读法、出现位置、`since`/`floor` |
| docs/51 的弃用表 | `.github/tools/check_compat_registry.py` 核对 docs/51 中英文表格与登记表一致 |
| 代码标记 | 每个 `COMPAT(<id>)` 注释都必须对应一条登记，每条登记至少有一个标记（同一个 CI 工具检查） |
| 强制删除 | 单元测试：`MCPP_VERSION` 达到某条的 `floor` 时失败，写出该 id 与它的标记位置 |
| 新条目的约束 | 同一单元测试检查 `floor` 至少比 `since` 晚两个完整日历月 |

内部的 "COMPAT, remove in 1.0.0" 注释（resolver、package_fetcher、legacy_dirs、compat 的回退候选）不是对外接口，
改为 `COMPAT(<主题>)` 内部标签，不进入登记表、不承诺版本（登记表只登记用户写得出来的东西）。CHANGELOG 不改。

## 7. 引擎模块与构建数据库（mcppls 的基础设施）

### 7.1 引擎模块的位置

- `$MCPP_HOME/engine/<mcpp 版本>/modules/{mcpp.cppm, mcpp_core.cppm}`：只读，首次需要时写入（原子重命名），内容只取决于 mcpp 版本。
- `mcpp env`（`mcpp.env`）新增 `engineModules` 字段，编辑器与 mcppls 可以直接读到。
- `mcpp cache gc` 回收不再被任何已安装 mcpp 版本使用的目录（与 `_engine` 缓存条目同一规则）。

### 7.2 SPEC-005 的变化（v1.8，只增不改，只涉及 S1 文档）

`build.mcpp` 是 mcpp 生态专属的文件，引擎模块是 mcpp 自己的模块，消费它们的是 mcppls。因此新内容**只**进入
`mcpp emit build-database` 的 S1 文档；`mcpp build` 写出的 `compile_commands.json` 与
`mcpp emit build-database --spec compile-commands` 都不变（R4.1 不变），不为其他 LSP 做适配。
插件库作为根时（P3）根计划本身就是构建程序配置，它自己的单元照常出现在两种文档中；引擎包的单元只出现在 S1 的
`mcpp:core` 集合中。

| 规则 | 内容 |
|---|---|
| 新集合 | 每个构建程序一个集合 `<包>:build`（单元为 `build.mcpp` 或合成程序）；每个宿主实例一个集合 `<包>:host`；引擎包为 `mcpp:core`；宿主的标准库模块与目标的不同时为 `mcpp:std:host` |
| 名字的兼容 | 已有集合的名字不变：构建程序配置**不**计为 R3.3 的"多个配置"，因此不触发 `<配置>/` 前缀；新集合以保留后缀区分（与 `<包>:test`、`mcpp:std` 的先例相同） |
| `ide.configuration` | 新集合为 `build-program` |
| `ide.kind` | `<包>:build` 为 `executable`，`<包>:host`、`mcpp:core` 为 `library` |
| `visible-sets` | 新集合只互相可见（宿主图是另一次规划，R3.4 的理由同样成立） |
| 单元的 `arguments` | 取自宿主图计划中该单元的编译记录，与实际编译逐字相同（R3.7 的同一规则，记录来源是宿主图而不是根计划），`.mcpp` 单元带强制语言参数 |
| `provides` | `mcpp:core` 的单元提供 `mcpp` 与 `mcpp.core` |
| `watch` | 增加每个构建程序依赖的源码包的源文件 glob（宿主实例）与引擎模块目录 |
| `--spec compile-commands` 与 `compile_commands.json` | 不包含这些单元，与修订前逐字相同 |
| 不编译（R2.2） | 不变：宿主图在 emit 中只被规划；构建程序照常运行（R2.5），需要编译时与今天一样编译。宿主图在 `mcpp build` 时把自己的描述写到工作目录，emit 在宿主图新鲜时直接读取，不重新规划 |

### 7.3 给 mcppls 仓库的适配 issue（实现完成、发布之后提出）

要点：新集合与命名规则；`ide.configuration = build-program`；`.mcpp` 单元按 C++ 处理（参数中已有强制语言）；
引擎模块的只读源码位置（`mcpp env` 的 `engineModules`）及其 `provides`；`<包>:build` 的可见集合；`watch` 新增项；
插件库作为根时整个文档只有构建程序配置（P3）。附一个示例工程与 `mcpp emit build-database` 的输出。

## 8. 工作空间

### 8.1 一张对照表：虚拟根与带根包的根

| 写在工作空间根清单中的东西 | 虚拟根（没有 `[package]`） | 带根包的根 |
|---|---|---|
| `[workspace]`、`[workspace.X]` | 给每个成员（带根包时，根包也是成员 `"."`） | 同左 |
| `[package]`、`[build]`、`[targets]`、`[dependencies]`、`[dev-dependencies]`、`[build-dependencies]`、`[features]`、`[resources]`、`[test]`、`[hooks]`、`[runtime]` | 不作用于任何包：警告，`--strict` 拒绝（已实现） | 根包自己的 |
| `build.mcpp` | **今天静默忽略（W1）→ 改为与上一行相同：警告，`--strict` 拒绝** | 根包自己的构建程序，只作用于根包（实测：成员的编译不带它的指令） |
| `[toolchain]`、`[indices]`、`[profile.*]`、`[target.*]` 标量、`[xlings]` | 兼容读法 `workspace-position`：按位置给成员，每次使用警告；同一张表两种写法时拒绝 | 根包自己的；兼容读法只在成员确实收到值时警告 |
| `[workspace.dependencies]` | 成员以 `x.workspace = true` 选用 | 同左；根包也可以选用（实测可用） |
| `[xlings.overrides]` 与 `[workspace.xlings.overrides]` | 只在根位置给（**修复 R4**） | 同左 |
| 宿主图的工作目录 | `<工作空间根>/target/.build-mcpp/host/` | 同左 |

### 8.2 修复

| 编号 | 修复 |
|---|---|
| R4 | `merge_xlings` 拆开：`deps`/`workspace` 条目照旧在每个位置继承（W5）；`overrides` 移到 `merge_layer_root_position`/`inherit_workspace_root_position`。工作空间层与位置读法同样处理 |
| R5 | 依赖位置经位置读法收到 `[xlings]` 条目时，只在**这次构建的根不属于该工作空间**时警告（例如外部工程依赖 git 托管工作空间的成员）；否则构建的根已经把这些条目带进图的环境（`step6_xlings_workspace_from_graph` 收集图中所有包），收到与否不改变构建。判据：`state.runtimeWorkspaceRoot` 是否等于成员所在工作空间的根 |
| W1 | 虚拟根旁的 `build.mcpp` 与包表同等对待 |
| W2 | `[workspace.dependencies]` 的继承复制 `features`（工作空间的在前，成员的追加，与 Cargo 相同）与 `default-features`（成员写了则以成员为准）；其余边请求键（`host-module`、`tools`、`artifacts`、`reexport`、`visibility`）属于每个成员自己的边——同一条目可以被 `[dependencies]` 与 `[build-dependencies]` 继承，含义不同——写在 `[workspace.dependencies]` 中给出警告，`--strict` 拒绝。今天这些键被静默忽略，所以只警告、不拒绝（docs/51：不得静默忽略，也不得让能构建的清单停止构建） |
| 警告示例 | 位置警告的示例按成员实际收到的部分写（overrides 写 `[workspace.xlings.overrides]`，条目写 `[workspace.xlings.workspace]`） |
| 版本文本 | 由登记表生成（§6） |

## 9. 兼容性

| 今天能构建的清单 | 之后 |
|---|---|
| `[build-dependencies] x = { host-module = true }`（规则包、`mcpp:plugins`） | 照常构建，无诊断（陈述默认值）；宿主实例按完整的包编译，`include_dirs` 等生效 |
| `[dependencies] x = { host-module = true }` 或请求规则特性 | 照常构建，不进目标，警告 `host-module-in-dependencies` |
| 插件接口导入 `[build-dependencies]` 中的包（`rules-tidy`） | 照常构建，警告 `plugin-imports-from-build-dependencies` |
| Form B 包没有接口根，从 `build.mcpp` 导入 | 今天失败；之后可导入，本包构建时有 W1（SPEC-008） |
| index 的 toml++（Form A，相对路径 include） | 照常构建 |
| 没有构建程序的工程 | 不规划宿主图，路径与今天相同 |
| 只 `#include`、不导入模块的 `build.mcpp` | 在宿主图中编译（一个没有依赖的成员）；引号 include 按源文件目录解析，结果不变 |

写进 CHANGELOG 的行为变化：宿主实例编译依赖的**全部**源（今天"只取列出的 sources"是为了避免把普通库的单元单独编译，单独编译已不存在）；
目标实例不再收到只由构建依赖请求的特性（R3）；有构建程序的包，其 `[build-dependencies]` 中的库默认被编译（`host-module = false` 退出）。

## 10. 跨平台

| 平台 | 要点 |
|---|---|
| Linux glibc | 程序的 RPATH 由计划的链接规则给出（e2e 901 的工具库路径规则），与今天 `default-v2` 等价，实测（O2） |
| Linux musl、MinGW | "静态助手"策略由合成成员陈述 |
| Windows MSVC / clang-cl | UTF-8 清单由合成成员的资源陈述；工作目录短；`/reference` 由计划的 BmiTraits 生成；`.mcpp` 带 `/TP` |
| macOS | 部署目标由计划的 `min_platform_version` 给出，与今天的判断一致 |
| 交叉构建 | 宿主图 `target_triple = ""`，用构建程序的工具链；引擎包的缓存条目按宿主工具链区分 |
| Termux / Android 宿主 | 与 tools 子构建同一条路径（`posix_shell()`），实测（O4） |

## 11. 性能

### 11.1 基线（本地，mcpp 2026.10.10.1，3 个成员各有导入 `mcpp` 与一个 path host 模块的 `build.mcpp`）

| 情形 | 时间 | 说明 |
|---|---|---|
| 冷构建 `--workspace`（全局缓存中已有引擎模块） | 1.56 s | 构建程序预编译线程 + host 模块存储 |
| no-op `--workspace` | 0.05 s | 整个构建的快速路径，不进入 prepare |
| 编辑 `m1/build.mcpp` 中的值 | 1.24 s | 其中 host 模块条目检查 52 ms、`build.mcpp` 编译约 500 ms、主 ninja 625 ms |
| 很小的工程 no-op / `--configure-only` | 0.05 s / 0.04 s | 进程启动加规划的量级 |

### 11.2 预算与手段

| 路径 | 预算 | 手段 |
|---|---|---|
| 整个构建的 no-op | 不变 | 快速路径不受影响（它在 prepare 之前） |
| prepare 内、全部程序新鲜 | 程序身份计算 ≤ 10 ms（3 个程序） | stat 摘要；不规划宿主图（§4.4） |
| 一个程序失效 | ≤ 今天（1.24 s） | 宿主图规划 ≤ 50 ms（子构建，引擎包与 index 包命中依赖缓存）；ninja 只编译该程序 |
| 冷构建 | ≤ 今天（1.56 s） | 同组程序一次 ninja 并行；共享包编译一次 |
| 插件根的预扫描 | ≤ 5 ms / 100 个文件 | 只读导入，结果交给主扫描复用 |
| 宿主图锁等待 | 记录等待时长 | 日志 `host-graph: waiting for <dir>` |

这些预算在探测 PR 中按 #748 的工作负载在三个平台上实测（O1），达不到时在实现前回到设计。

## 12. 可观测性

| 层面 | 内容 |
|---|---|
| 日志标签（`mcpp.log`，`--verbose` 与 `~/.mcpp/log/mcpp.log`） | `edges`：每条边的配置、产物与来源 section；`features`：每个实例两张表的激活结果；`host-graph`：分组、合成成员、钉、工作目录、锁、规划与 ninja 的 `ScopedTimer`；`build-program`：每个程序的身份分量、**失效原因（哪个分量变了）**、运行；`interface`：每个包的接口根、公开模块与来源；`compat`：每次兼容读法的使用 |
| 记录文件 | 宿主图工作目录中的 `host-graph.json`：组键、成员、每个程序的依赖与身份分量、上次构建时间；与 `resolution.json` 并列 |
| 命令 | `mcpp why build-programs`（`mcpp.why.build-programs`）：每个程序所在的组、依赖、实例、是否新鲜及原因；`mcpp why deps` 写出每条边的配置与每个包的实例；`mcpp why compat`（§6.3） |
| 进度 | 宿主图作为一个分组（docs/50 "The group record"）：`Compiling build programs (host <三元组>, <工具链>, <标准>): m1, m2, m3` |
| 失败 | 宿主图的失败带工作目录与复现命令；`MCPP_TOOL_BUILD_VERBOSE` 同样作用于宿主图的内层输出 |

## 13. 测试

### 13.1 新增 e2e（编号接在 901 之后；每个都必须在 2026.10.10.2 上失败）

| 编号 | 判据 |
|---|---|
| 902 | 头文件库的模块包装（path、本地 index 的 Form A、Form B + `lib`）可以从 `build.mcpp` 导入，GMF 用 `<…>`（R1）；不写 `host-module` 也可以 |
| 903 | §3.5 的验收矩阵 M1–M10（M5、M6 在今天通过，作为不得退化的保护；其余在 2026.10.10.2 上失败） |
| 904 | 只由构建依赖请求的特性不进目标实例，宿主实例拿到它（R3） |
| 905 | 宿主库带非接口源文件、C 源文件和自己的 `[dependencies]`，全部链接进构建程序（#797 Q4、Q5） |
| 906 | 编辑 path 宿主库所 include 的头文件，程序重编重跑；不编辑时 `--verbose` 显示宿主图未被规划（R6 与预算） |
| 907 | Form B `lib`；`modules` 断言不一致时的诊断与 `--strict`；`lib` 指向不存在的文件：本包构建报错，作为依赖时只警告 |
| 908 | `host-module` 默认值：只要 `tools` 时不编译库；加 `host-module = true` 两者都有；规则特性隐含；`host-module = false` 退出 |
| 909 | 兼容登记表：每条读法行为不变并给出带 `since`/`2027.1` 的警告；index 包不警告；`mcpp why compat` 列出它们；`--strict` 下为错误 |
| 910 | 插件：P1（`[dependencies]` 到插件被拒绝并写出链）、P3（插件包 `mcpp build`、`mcpp test` 成功，`--target` 他处被拒绝）、P4、P5；插件依赖插件与普通库（S1、S2） |
| 911 | 工作空间：R4、R5、W1、W2 |
| 912 | 两个配置组宿主配置相同时，宿主库只编译一次（锁与共享） |
| 913 | S1 文档：`<包>:build`、`<包>:host`、`mcpp:core` 集合存在，参数与宿主图的实际编译逐字相同，已有集合名字不变，`watch` 含新项；`compile_commands.json` 与 `--spec compile-commands` 的输出与修订前逐字相同；`mcpp env` 的 `engineModules` 指向存在的只读源码 |

### 13.2 单元测试

`library_interface`（接口根、约定、特性接口、转出闭包、断言比较）；边模型（配置继承、`wantsLibrary` 默认规则）；
程序身份（每个分量的变化都改变身份，stat 摘要回退到内容摘要）；兼容登记表（下限强制、`floor - since` 下限、
警告文本）；插件预扫描（只读导入、结果复用）。

### 13.3 已有测试

189、309–312、610、622、624、630、633、798、815、819、831 **不改断言**通过（行为兼容）。857 判据 C/D 与 859 断言的是
`host-modules/` 存储布局，改为断言同样的性质（编译一次、path 包不进全局缓存、引擎模块进全局缓存、输入一致时复用），
位置改为宿主图与依赖缓存。894 的警告文本改为登记表生成的文本。

### 13.4 本轮已做的测量

R1–R8、R2b、R2c、W1–W2 在复现脚本中（2026-10-11 全量运行一次，结果与 §1.1 一致）；§11.1 的基线；`examples/08-build-rules/app`（`[dependencies] … host-module = true`
与规则依赖规则）今天可以构建（1.25 s），它是兼容读法必须保持的用例。

## 14. 文档与规范

- **SPEC-004**：新增 §12"依赖的库与配置"（两条轴、实例、一个包一个版本、按配置激活特性、`host-module` 的精确含义与默认值、
  §5.2 所要求的历史注记）；§9 第 13 条的移除下限；`[workspace.dependencies]` 的继承键（W2）；变更记录。
- **SPEC-005**（构建数据库）v1.8：§7.2，只改 §3（S1 文档）与 §6（`watch`），§4 不变。
- **SPEC-007**（构建插件）：插件单元的定义与 P1–P5；构建程序在宿主图中编译；引擎模块的位置；变更记录。
- **SPEC-008**（库接口）0.2：I1 扩展到配置；I2 加入特性接口；Form B `lib`；`exports` 为断言；§4 诊断表；第二阶段的条件不变。
- **docs（中英）**：04 §2.4 `[lib]`；05 `[build-dependencies]`（两条轴、`host-module` 含义与默认、两表同名）；
  30 构建程序与构建依赖的库（删除 "compiled alone"）；31 插件写作（依赖写在 `[dependencies]`、插件包的构建与测试、`rule_module`）；
  07 工作空间（§8 的对照表）；50 新的 `why` 话题与分组；51 弃用策略与登记表；03 示例；23 `mcpp env` 的 `engineModules`。
- **CHANGELOG**：Added、Changed、Fixed（R1–R8、W1、W2）、Removed、Deprecated（登记表条目）。

## 15. 生态同步

| 仓库 | 内容 | 时机 |
|---|---|---|
| mcpp（本 PR） | `examples/08-build-rules`、`09-heterogeneous`、`10-graphics`、`12-a-new-device-language` 改为规范写法；`rules-tidy` 的依赖移到 `[dependencies]` | 本 PR |
| mcpp-index | 6 个 Form B 包加 `lib`；`marzer.tomlplusplus` 回到 Form B 加 `lib`，include 改回 `<toml++/toml.hpp>`，按 revision 递增；`min_mcpp` 门控；CI 读取 `mcpp xpkg parse --format json` 的 `compat` 字段 | mcpp 发布之后 |
| mcpp-plugins | 消费方测试中多余的 `host-module = true` 可保留（陈述默认值）；fixture 测试可以改写为插件包自己的 `mcpp test`（S4）；README；`mcpp = ">=<本发布>"` | mcpp 发布之后 |
| mcppls | 适配 issue（§7.3） | mcpp 发布之后 |

## 16. 决策

| 编号 | 决策 |
|---|---|
| D1 | 配置由 section 决定，产物由边上的请求决定 |
| D2 | 不加新键；`host-module` 保留原名，含义为"构建程序使用这个库"，在 `[build-dependencies]` 中没有 `tools`/`artifacts` 时默认成立 |
| D3 | 节点为 `(包, 配置)`；两表同名为两个实例；版本统一；特性按配置激活；环境取并集 |
| D4 | 宿主图：按（工具链身份、标准）分组，合成工作空间、每个构建程序一个合成成员，经子构建规划与构建，以主图解析为钉 |
| D5 | 引擎模块是按版本存放的只读引擎包，只在构建程序配置中存在；全局缓存条目位置不变 |
| D6 | 宿主图 profile 为 `opt = 0`、`debug = false`，与今天相同；不新增用户可配置项 |
| D7 | 宿主图工作目录按组键命名并加锁；index 包经依赖缓存全局复用，path 包留在工作区 |
| D8 | 程序缓存身份覆盖整个闭包；path 包用 stat 摘要加内容回退；全部新鲜时不规划宿主图 |
| D9 | `library_interface` 是库接口的唯一定义 |
| D10 | Form B `lib` ≡ `[lib] path`；`exports`/`modules` 是断言 |
| D11 | 特性加入的接口单元是该特性的公开模块；`rule_module` 必须是其中之一（SPEC-008 I2 修订） |
| D12 | 接口根不是宿主消费的前提；显式声明而不存在：本包报错，依赖警告 |
| D13 | 构建程序只能导入本包构建依赖（及 reexport 传来的）的模块；目标侧导入传递依赖模块的现状不变（改变它会让能构建的清单停止构建） |
| D14 | 删除 `host_module_compile`、`host_module_store`、`host_module_units`、清空 sources、预编译线程 |
| D15 | 插件单元由导入引擎接口判定；P1–P5 |
| D16 | 兼容登记表与 §6.3 的派生物；移除下限 `2027.1` |
| D17 | 只有 S1 文档按 §7.2 描述构建程序、宿主实例与引擎模块，名字只增不改；`compile_commands.json` 不变 |
| D18 | 工作空间：§8.2 的六项修复 |
| D19 | 工具链阶段的构建程序（R9.9）用同一套宿主图，bootstrap 工具链，没有钉 |
| D20 | 性能预算（§11.2）与可观测性（§12）是实现的验收条件 |

## 17. 待测量（探测 PR）

| 编号 | 问题 | 判据 |
|---|---|---|
| O1 | #748 工作负载在 Linux、macOS、Windows 上的冷构建、程序失效、全部新鲜、no-op | §11.2 的预算 |
| O2 | Linux glibc 程序的 RPATH 与 `default-v2` 运行一致；musl、MinGW 静态链接 | 三平台 e2e 通过 |
| O3 | Windows MSVC 与 clang-cl：UTF-8 清单、`/reference`、`/TP`、路径长度 | Windows runner 上 902–913 与已有测试通过 |
| O4 | macOS 部署目标；Termux 宿主 | 对应 runner 与 Android 探测通过 |
| O5 | 交叉构建（`aarch64-linux-gnu`、`wasm32-emscripten`）的宿主图与引擎包缓存键 | 程序可运行，缓存条目按宿主工具链区分 |
| O6 | 子构建规划宿主图的实际耗时（`ScopedTimer`） | ≤ 50 ms |

## 18. 自我 review

### 18.1 修订 2 → 3

| 问题 | 修正 |
|---|---|
| 修订 2 把新集合也放进 `compile_commands.json`，会改变所有读者看到的内容，而这些单元只对 mcppls 有意义 | 只进 S1（§7.2），e2e 913 断言 `compile_commands.json` 逐字不变 |
| S1 中新单元的 `arguments` 原写作"与 `compile_commands.json` 的条目同一条记录"（R3.7），而新单元不再进入后者 | 改为取自宿主图计划的编译记录 |
| 插件根（P3）的引擎包单元在 `compile_commands.json` 中是否出现没有说明 | 引擎包单元只在 S1 的 `mcpp:core` 中出现，插件根自己的单元照常 |
| "同一个库两侧使用"只有一条测试（903 的 R2），没有覆盖特性不同、Form B、依赖的构建程序、交叉构建、头文件改动 | §3.5 的矩阵 M1–M10，M5、M6 是今天能工作的保护用例 |
| 目标与宿主两个实例都提供模块 `hl`，是否触发同名模块冲突没有说明 | 两次规划各自一张模块图，不冲突；依赖缓存的两份条目并存（§3.5 末段） |

### 18.2 修订 1 → 2

| 问题 | 修正 |
|---|---|
| 新键 `lib` 违反 SPEC-004 §5.2（已发布的键不改名），并让每个用户迁移一次 | 撤回，`host-module` 精确化（D2） |
| "插件经 `[dependencies]` 进入目标即拒绝"会挡住插件依赖插件，也没有回答插件包自己怎么构建（R7） | 改为配置传播加 P1–P5 |
| 一个合成包含多个 `bin`：所有程序共享依赖、链接与可见性，一个程序的依赖会出现在另一个程序的链接中 | 合成工作空间，每个程序一个成员 |
| 程序身份对 path 包的源树摘要每次都读内容（今天检查一次 host 模块条目要 52 ms） | stat 摘要加内容回退 |
| 构建程序配置的集合若计为"多个配置"，会给已有集合加前缀，破坏现有的构建数据库读者 | 新集合用保留后缀，不计为配置（§7.2） |
| 合成成员的根目录是消费方包的根目录，默认的 `sources` 推断会取到消费方的 `src/**` | 合成成员明确 `sources = []` |
| `FileLock::try_acquire` 只尝试一次 | 宿主图需要阻塞获取并记录等待（§4.3） |
| 插件根的预扫描与主扫描重复读文件 | 结果交给主扫描复用（§4.8） |
| 兼容读法警告若对 index 包给出，用户无法处理 | 只对能修改的清单警告，依赖中的用法由 `mcpp why compat` 报告 |
| `examples/08-build-rules/app` 用的是要弃用的写法 | 列入兼容测试（909）与示例更新（§15） |

## 19. PR 组织

一个 PR，分支 `release/<下一版本>`，按依赖顺序提交（每个提交都能编译，测试在最后一个提交全部通过）：

1. 兼容登记表、派生的警告、`mcpp why compat`、CI 工具与单元测试；docs/51 策略；`workspace-position` 迁入登记表。
2. `library_interface` 模块与单元测试；`scan.cpp`、`validate.cppm`、`library_pipeline.cppm` 改用它；Form B `lib`；`exports` 断言。
3. 清单规范化：`host-module` 的默认值与兼容读法；`[workspace.dependencies]` 的继承键（W2）。
4. 边模型：`configuration`、`wantsLibrary`、实例谓词、按配置激活特性；删除清空 sources。
5. 引擎包与 `mcpp env` 字段；宿主图（`prepare/host_graph.cpp`）；构建程序的编译改走宿主图；程序身份。
6. 插件规则 P1–P5 与根的预扫描。
7. 删除 `host_module_compile`、`host_module_store`、预编译线程；诊断迁移。
8. S1 文档的新集合（SPEC-005 v1.8）；`compile_commands.json` 不变的回归断言。
9. 工作空间：R4、R5、W1、警告示例。
10. 可观测性：日志标签、`host-graph.json`、`mcpp why build-programs`、进度分组。
11. e2e 902–913、已有测试的更新、examples。
12. 规范（SPEC-004、005、007、008）、docs 中英文、CHANGELOG、版本号。

在 review 结论与探测 PR（O1–O6）完成之前，不开始实现。

## 20. 实施计划与任务依赖（goal 启动时写入）

版本：`2026.10.11.1`（发布日期变化时在最后一个提交中改）。分支 `release/2026.10.11.1`，mcpp 一个 PR；
mcpp-index、mcpp-plugins 各一个 PR（发布之后）；mcppls 一个 issue。

| 任务 | 内容 | 依赖 | 验证 |
|---|---|---|---|
| T1 | 兼容登记表（`mcpp.pm.compat`）、警告生成、`mcpp why compat`、下限强制的单元测试、`check_compat_registry.py`；`workspace-position` 迁入 | — | 单元测试；894 的新文本 |
| T2 | 工作空间：R4、R5、W1、W2、警告示例 | T1 | e2e 911 |
| T3 | 清单：`host-module` 的三态（未写/真/假）与默认值；`[dependencies]` 中的兼容读法移到 `buildDependencies`；Form B `lib`；Form B `deps` 同规则 | T1 | 单元测试（toml、xpkg） |
| T4 | 边模型：`configuration`、`wantsLibrary`、`targetInstance`、`compilesHere`；特性按配置；删除跨 section 合并与清空 sources | T3 | e2e 903（M1–M3 的目标侧）、904、310 |
| T5 | `library_interface` 模块及其读者；Form B `lib`、`exports` 断言、`rule_module` 校验 | — | 单元测试；e2e 907 |
| T6 | 引擎包：`$MCPP_HOME/engine/<版本>/modules/`；`mcpp env` 的 `engineModules` | — | e2e 913（部分） |
| T7 | 宿主图：合成工作空间（写到 `target/.build-mcpp/host/<键>/`，可在该目录直接复现）、子构建、钉、工具链继承、内部 profile、锁、`.mcpp` 源；构建程序的编译改走宿主图；程序身份；删除 `host_module_compile`、`host_module_store`、预编译线程 | T4、T5、T6 | e2e 902、903、905、906、912；已有 189、309–312、610、622、624、630、633、798、815、819、831、857、859 |
| T8 | 插件 P1–P5 与根的预扫描 | T7 | e2e 910 |
| T9 | S1 文档的新集合 | T7 | e2e 913 |
| T10 | 可观测性：日志标签、`host-graph.json`、`mcpp why build-programs`、`why deps` 的配置、进度分组 | T7 | e2e 906 的 `--verbose` 断言 |
| T11 | e2e 902–913、单元测试、已有测试的更新 | 各自的任务 | 本地全量 e2e；CI 三平台 |
| T12 | 规范（SPEC-004、005、007、008）、docs 中英文、CHANGELOG、版本号 | 全部 | `check_docs_*`、`check_*` |
| T13 | examples（08、09、10、12）改为规范写法 | T3 | `build_examples.sh` |
| T14 | CI 全绿、生态级自我 review、合入、发布、GitCode、xim-pkgindex、clean-room 验证、mcpp-index 与 mcpp-plugins PR、mcppls issue | 全部 | — |

关键路径是 T3 → T4 → T7 → T8/T9/T10 → T11。T1、T2、T5、T6 与关键路径并行。
宿主图的合成工作空间**写到磁盘**（修订 3 写作"不落盘"）：它由引擎生成，落盘之后可以在该目录直接运行
`mcpp build` 复现宿主图的构建，这是可观测性要求的一部分；内容不变时不重写，ninja 保持增量。

## 21. 实施记录（实现中确定的细节）

| 项 | 结论 |
|---|---|
| 宿主图目录的自描述 | 目录中写 `.mcpp-build-programs`（`toolchain=`、`profile=`、`pin=`、`index-store=`），`prepare_build` 在其所在目录读取它，使 `cd <dir> && mcpp build` 与子构建得到相同的覆盖项 |
| 宿主图的 `[xlings.overrides]` | 构建根生效的 overrides 写入合成工作空间的 `[workspace.xlings.overrides]`（相对路径改为绝对路径），子构建以同一来源声明供给宿主图中的包所声明的载荷（e2e 873） |
| 项目级 index 存储 | 自定义 index 的包装在发起构建的项目的 `.mcpp/` 中；子构建以 `BuildOverrides::project_index_root` 指向它，不在宿主图目录重新安装（e2e 858 E） |
| `plugin-imports-from-build-dependencies` | 判据是两包的源码（插件库 import 的模块由 `[build-dependencies]` 中 `host-module = true` 的包声明），在 phase 6 的 `step6_compat_library_edges` 中改写为普通边并写入清单的 `[dependencies]`，使链接闭包包含它；与读取它的图无关 |
| 可编辑的包 | 兼容读法的警告给"不在包存储（index 存储、`$MCPP_HOME`）中的包"，`path` 依赖无论位置都算可编辑（与 Cargo 对 lint 的划分一致） |
| 失败报告的确定性 | 一次构建的全部程序先在宿主图中一次编译；失败时各程序按运行顺序单独请求自己的二进制，报告的是运行顺序中第一个失败的程序（e2e 860 C） |
| 虚拟根的 overrides | `virtual_workspace_root` 原本没有从首个成员复制 `xlings.overrides`，`[workspace.xlings.overrides]` 在不带 `[package]` 的根上不生效；本 PR 修正（e2e 911 V） |
| 工作空间根的 schema 警告 | 不带 `[package]` 的根在成员构建时其 `schemaWarnings`（含层表与 W2 的请求键）此前不报告；`report_workspace_root` 报告它们 |
| 文件长度 | `graph.cpp` 的查询闭包拆到 `graph_closures.cpp`（P4b 的阶段边界） |
| 本机限制 | 代理会话运行在 xlings 沙箱 subos 中，`xlings self init` 被拒绝，依赖全新 `MCPP_HOME` 的 e2e（618、622、625、627–630、657、669、686、737、779、790、884 等）在本机与基线二进制同样失败；它们由 CI 验证 |
