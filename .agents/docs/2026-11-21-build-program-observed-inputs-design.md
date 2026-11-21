---
subject: build-program
status: active
---

# 构建程序的输入由观测给出：编译读到的文件、三道闸门的同一份记录、头文件的用法提示、提示的统一机制、契约中名字的歧义（mcpp#991、#852）

**日期：** 2026-11-21 · **版本：** v4（v2 增加 3.6 提示的统一机制；v3 增加 3.7 契约中名字的歧义（#852）；v4：旧的回答经过弃用期，被询问时警告。2026-11-21 确定 3.6 的四点：策略只有 `always` 与 `once`，直接的调用全部迁移且检查没有例外，弃用与未知键保持 `always`，#852 本轮按根因修复；此前确定的：本方案并入
[生态的测试与 CI 方案](2026-11-20-test-architecture-upgrade-design.md) 的本轮，见其 5.23，不另立拉取请求；包外读取在本地构建
中警告、在 `mcpp publish` 中拒绝；标准库头文件与本包头文件两种用法在文档中标为不推荐，首次构建给出一次提示） ·
**基线：** mcpp 2026.11.20.1（`722ec1b6`）与 #984 的分支 · **规范：** SPEC-007 R10（`docs/specs/build-plugins.md`）

## 1. 问题

mcpp#991：`build.mcpp` 以引号路径包含本包的头文件，头文件改变之后 `mcpp build`、`mcpp run` 仍使用旧的程序与旧的指令，
没有任何提示。Linux（gcc 16.1.0）与 Windows（`msvc@system`）相同。本地复现（本方案的基线二进制）：

| 步骤 | 期望 | 实际 |
|---|---|---|
| 首次 `mcpp run` | one | one |
| 头文件改为 two | two | one |
| 删除 `target/.build-mcpp` 之后 | two | one |
| build.mcpp 加入 `rerun-if-changed=support/emit.hpp`（其文本因此改变） | two | two |
| 头文件改为 three | three | two |

最后一行说明文档给出的声明方式也不能绕过：声明使程序重新运行，运行的仍是旧的二进制。用户侧唯一有效的绕法是把头文件放进
`[build-dependencies]` 中的 path 包，因为包的树进入身份。用法本身是文档允许的（docs/30：`#include` 照常可用），
缺陷在引擎。

## 2. 现状：三道闸门，各有一个预测的输入模型

一个头文件的改变要先后经过三处判断，三处都看不见它：

| 闸门 | 位置 | 依据 |
|---|---|---|
| ① 工程的快速路径（无事可做的构建实测 6 至 9 ms） | `build_program.cppm` 的 `program_inputs_stale` | 源文件的修改时间；构建程序记录中声明的文件、glob 与环境变量 |
| ② 编译闸门 | `host_graph.cpp` 的 `program_identity`，记在 `target/.build-mcpp/host/<group>/programs.json` | 引擎版本、工具链、标准、程序文本、库闭包中每个包及可原地变化的树的摘要 |
| ③ 运行缓存（home） | 以 ② 的身份为键，另加声明的输入与契约环境 | 同 ② |

② 在规划子构建之前判断，所以子构建的 ninja 记录（程序单元的扫描 depfile 列出了 `support/emit.hpp`）从未被询问；
③ 以同一个身份为键，所以删除 `target/.build-mcpp` 之后重新编译的程序，其运行仍被回放。

这一类缺陷此前逐项修补：快速路径补入 `build.mcpp` 本身；2026.9.5.4 补入声明的文件（`mcpp.tools.embed`）；
#359 与 #765 补入 glob；#748 补入宿主模块提供方的树；#931 补入链接读到的文件。每次补的是某一道闸门上的某一种输入。
结构上的原因是三道闸门各自预测程序的输入，而主构建早已以观测为准：GNU depfile、MSVC 的 `/sourceDependencies`、
单元缓存的已读清单（测试方案 5.3）。

## 3. 决定

### 3.1 输入分三类，各有唯一的来源

| 类 | 内容 | 来源 |
|---|---|---|
| 预测 | 引擎版本、工具链、标准、构建程序实例的特性、契约环境、库闭包中每个包的身份 | 本次解析（现状不变） |
| 编译读取 | 编译程序单元时读到的文件 | 观测：子构建为该单元写下的扫描 depfile（`<obj>/build.mcpp.ddi.dep`，GNU 的第一条记录），MSVC 为同一单元的 `sourceDependencies`；解析用 `mcpp.build.depfile`，与主构建同一个函数 |
| 运行读取 | 程序运行时打开的文件、读取的环境变量 | 声明：`rerun_if_changed`、`rerun_if_changed_glob`、`rerun_if_env_changed`（现状不变） |

运行读取不改为观测：跨宿主没有可靠的手段（strace、ETW、`LD_PRELOAD` 各管一个系统，且对静态二进制无效），文档继续要求声明。

**编译读取的范围。** 记录排除两类文件：不可变的 store 中的文件（工具链、sysroot 与索引包的载荷，已由工具链与包的身份覆盖；
gcc 的 `-MMD` 本来就不列出系统目录中的头文件），以及 build-dependency 的树中的文件（已由该包的树摘要覆盖）。余下的是
程序所在包的树中的文件与两者之外的文件，通常只有几个。

### 3.2 一份记录，三道闸门共用

- **记录。** 一组程序编译之后，引擎读取每个程序单元的编译读取，写入该程序的记录：`programs.json` 的行增加 `reads`，
  每项为路径、大小、修改时间与内容摘要。
- **身份。** 程序的身份 = 预测部分 + 编译读取的路径与摘要。②、③ 都以它为键，因此一个头文件的改变同时使程序重新编译
  与重新运行。
- **快速路径。** `program_inputs_stale` 除构建程序的运行记录外，也读取各组 `programs.json` 的 `reads`；任一项改变即不走快速路径。
- **比较的次序。** 先比较大小与修改时间，二者之一改变时才计算摘要；摘要相同则更新记录中的大小与时间，不判为改变
  （与单元缓存的摘要备忘相同）。声明的文件（`rerun_if_changed`）的比较改为同一次序：现状每次都计算摘要。
- **编译失败。** 编译失败的程序不写记录，下次构建再次编译（现状不变）。

**成本（实测，Linux，本节的复现工程）。** 主构建无事可做时 6 至 9 ms；在子构建的目录中执行一次无事可做的
`mcpp build` 为 110 至 280 ms。因此不采用「每次都规划子构建，由 ninja 判断」：它把每次构建的下限提高十倍以上。
记录的比较只需几次 `stat`。

### 3.3 头文件的用法：文档标为不推荐，首次构建提示一次

构建程序的推荐写法是导入：标准库用 `import std;`，引擎接口用 `import mcpp;`，自己的共用代码放进一个 path 包的模块，
在 `[build-dependencies]` 中声明并导入。以下两种用法可以使用，文档标为不推荐，并给出推荐的写法：

| 用法 | 判定 | 推荐 | 理由 |
|---|---|---|---|
| A. 包含标准库或系统的头文件（`#include <cstdio>`） | 程序文本中尖括号形式的 `#include` | `import std;` | 每次编译重新解析头文件；`std` 模块已按工具链与标准备好，构建程序与主构建共用 |
| B. 包含本包或依赖的头文件（`#include "support/emit.hpp"`） | 程序文本中引号形式的 `#include` | 共用代码做成 path 包的模块，列入 `[build-dependencies]` 并导入 | 有包的身份，可在多个构建程序与包之间共用，BMI 只编译一次 |

判定只读程序自身的文本（`build.mcpp`，注释与字符串中的不算）：在 `modules/manifest/src/mangle.cppm` 中，以
`imported_module_names` 的同一词法新增 `included_headers`，不读它包含的文件，也不依赖
编译读取：gcc 的 `-MMD` 不列出系统目录中的头文件，A 无法从编译读取中得出。A 与 B 是同一种提示的两种内容，一个程序一条。

**提示。** 一个工程第一次构建时，每一种用法给出一条提示（`note`，不是警告），两行，例如：

```
note: build.mcpp of `repro` includes headers (<cstdio>, "support/emit.hpp"); this works, and the
      recommended form imports them: `import std;`, and a module of a path build-dependency (docs/30, "Headers or modules")
```

- **一个工程只提示一次。** 两种用法登记为 `once` 策略的提示（3.6），键为（用法，程序所在的包）。之后的构建不再提示，
  直到 `mcpp clean` 删除 `target/`；出现新的用法，或另一个程序出现同一用法时，只提示新出现的。
- **合并。** 一次构建中多个程序有同一用法时合并为一条，列出各包名（工作区的成员，3.6）。
- **范围。** 只提示工程自己能修改的程序：根包、工作区成员与 path 依赖。索引包与 git 依赖的构建程序不提示。
- **机器输出。** 代码为 `build-program-includes-header`；`--quiet` 不显示（3.6）。
- 提示不改变构建的结果，不进入任何键。

### 3.4 读取包外的文件：本地警告，发布拒绝

C. 编译读取中既不在程序所在包的树内、也不在 build-dependency 的树或 store 中的文件（绝对路径、`../shared/x.hpp`），
有两个实际的后果：构建依赖这台机器的文件，违反默认密闭（AGENTS.md §2）；发布的归档由 `git archive` 取包的根目录，
这个文件不在其中，消费方取到的 `build.mcpp` 无法编译。

- **本地构建。** 给出警告，说明影响与办法（把文件移入本包，或做成 path 包），代码 `build-program-reads-outside`。
  登记为 `once` 策略（3.6），键为（程序所在的包，文件），`mcpp clean` 之后重新警告。它不改变这次构建的结果，
  影响在别处（其他机器、消费方），所以不属于 `always`。索引包与 git 依赖的程序
  同样警告，办法改为向包的维护者报告。
- **发布。** `mcpp publish`（它在打包前调用 `prepare_build`，记录因此是当前的）拒绝发布：编译读取中有文件在包的根目录之外，
  或不在 `HEAD` 的已跟踪文件中（归档不含它）。拒绝说明原因与办法，原因标记为 `build-program-unpublished-input`，
  进入 `check_reason_tokens.sh` 核对的列表与面的账本。

### 3.5 构建程序导入的模块与库

- **保留。** 程序只能导入 `std`、`std.compat`、`mcpp`、`mcpp.core`，以及 `[build-dependencies]` 导出的模块与它们以
  `reexport = true` 转交的模块；只经由另一个 build-dependency 的依赖到达的模块被拒绝（`host-module-missing`，现状）。
- **新增的要求即 3.4。** 程序的读取限于它的包、它的 build-dependency 与 store。这是对读取的要求，不是对写法的要求，
  因此同时覆盖头文件、`#embed` 与绝对路径的包含。
- **不限制。** 库自带构建程序（规则包依赖它，嵌套由 `tool_depth` 处理）；库的闭包大小（成本，`mcpp why build-programs`
  已给出）。
- **宿主的可移植性，在实施中核对。** build-dependency 的闭包只支持某些宿主系统时，宿主图的解析是否与目标图一样按宿主
  拒绝并给出原因；不成立时在本轮补上同一检查。

### 3.6 提示的统一机制：一个汇点、一张登记表、两种策略

**现状。** 告诉用户一件事的方式有六种，三种各自实现了「只说一次」：

| 方式 | 位置 | 策略 |
|---|---|---|
| `mcpp.diag`（`warning`、`degraded`、`note`） | `src/diag.cppm`，89 处调用 | 一个进程内按全文去重；保留机器可读的记录；`--strict` 把 `degraded` 升为错误 |
| 直接调用 `ui::warning`、`ui::note` | 31 个文件中约 100 处；另有 4 处直接写 stderr | 无：不去重，不进入机器输出，`--strict` 看不到 |
| `print_hint_once` | `src/toolchain/compat.cppm` | 自己的静态集合，一个进程一次 |
| `Said` | `src/config.cppm` | 自己的静态集合与互斥量，一个进程一次，直接写 stderr |
| 边的提示（`.mcpp-advice`） | `src/build/advice.cppm` | 边运行时才报告，一个进程一次 |
| 构建程序的 `mcpp::warning` | 构建程序的记录 | 每次缓存命中都回放（docs/30：只说一次会被读成「问题已解决」） |

弃用登记表（`modules/manifest/src/compat/registry.cppm`）是这里已有的范式：一张表陈述每一行，警告的文字、
`mcpp why compat`、docs/51 的一致性检查与 `--strict` 的拒绝都由行导出。缺的是同一形式的「提示」：3.3 要求的
「一个工程只说一次」没有可复用的机制，已有的三处「一个进程一次」各自实现。

**决定。** 用户可见的提示只有一个汇点 `mcpp.diag`；提示的种类与策略是数据；「说过」的状态由汇点统一保存。

- **汇点。** 所有用户可见的警告、降级与提示经由 `mcpp.diag`。`ui::warning` 与 `ui::note` 只供 `mcpp.diag` 渲染使用；
  上表第二至四行的调用全部改为经由汇点，各自的静态集合删除。边的提示与构建程序的 `mcpp::warning` 是传送通道，保留，
  读到的每一条交给汇点。`.github/tools/check_diag_sink.sh` 检查 `src/`、`modules/` 中 `mcpp.diag` 与 `mcpp.ui` 之外
  不再有直接的调用。
- **登记表。** 新包 `modules/notice` 持有提示种类的表，与弃用登记表同一形式。一行陈述：代码（kebab-case，稳定）、
  严重程度（`note`、`warning`、`degraded`）、策略、领域、文档的位置。只有策略不是缺省的种类需要登记；没有登记的提示
  按缺省策略处理，与今天相同。
- **两种策略。**
  - `always`（缺省）：每次得出该事实时都说，一个进程内按全文去重（现状）。用于这次构建的结果因它而不同的事实
    （`degraded`、构建程序的 `mcpp::warning`、弃用的读法、未知的键）。AGENTS.md §2「做得更少从不沉默」要求这一类不被抑制。
  - `once`：一个工程中每个键只说一次。键由种类与对象（包、文件、成员）组成；对象改变即是新的键，因此新出现的
    事实仍会被说。「说过」指在终端上显示过：`--quiet` 隐去的与只进入 `--format json` 的不算。说过的键记在
    `target/.mcpp-notices`，每行一个键；`mcpp clean` 删除整个 `target/` 时一并删除，`mcpp clean --stale` 不删除它。
    不在工程中的命令（`mcpp toolchain install` 等）没有工程，`once` 退化为 `always`。用于不改变结果的建议，3.3 的两种用法即属此类。
- **防止刷屏。**
  - 一次运行中同一种类的多个对象合并为一条，列出对象；超过五个时列出前五个并写明其余的数目。
  - `note` 至多两行：第一行陈述事实，第二行陈述写法与文档的位置（`help:`）。
  - `--quiet` 不显示 `note`；`warning` 与 `degraded` 照常显示。
  - `--verbose` 也显示被 `once` 抑制的提示，标为 `(noted before)`，使作者可以再次看到它们。
- **机器输出。** `--format json` 的诊断包含每一条提示，显示与否都包含，附 `code` 与 `shown`；机器的读者不受显示策略影响。
- **面的账本。** 登记的代码作为 `notice <code>` 进入 `mcpp emit surface`，由测试方案 5.16 的账本覆盖。
- **并发。** 工程没有覆盖整个 `target/` 的锁（锁在各配置目录与构建程序的组目录上）。写入 `target/.mcpp-notices` 为读出、
  求并集、写入临时文件后改名；两个进程同时写入时可能丢失对方的键，其后果只是该提示再出现一次，因此不另设锁。
- **位置。** 登记表、策略的判定、键的保存与合并的格式在 `modules/notice` 中，不依赖 `src/`，单元测试在该包的
  `tests/` 中；`src/diag.cppm` 调用它并负责渲染。


### 3.7 契约中的名字：歧义按变量名判定（mcpp#852）

**现状。** #852 所报告的情形（`openmui.rules` 与 `gongfu.rules` 的清单都写 `rules`）已在 2026.10.30.1（#862）解决：
两者共有的拼写不属于任何一个包，`MCPP_DEP_AMBIGUOUS_RULES` 列出限定名，`dep_dir("rules")` 结束程序并说出它们；
用例 980（`tests/e2e/deps/graph.cpp`）覆盖它，议题没有关闭。以 host-module 的 build-dependency 复现同一情形，同样不再警告。

根本的原因仍在：歧义按**拼写**判定，而契约发布的是由拼写**有损地**变换得到的变量名（字母转大写，其余字符转 `_`）。
`foo-bar`、`foo_bar` 与 `foo.bar` 是三个拼写、一个变量。本地复现（两个 path 依赖 `foo-bar` 与 `foo_bar`）：

```
warning: build.mcpp: dependency name collides on MCPP_DEP_FOO_BAR_DIR (kept '.../foo-bar', ignored '.../foo_bar') — rename one dependency to disambiguate
warning: build.mcpp: dependency name collides on MCPP_DEP_MCPPLIBS_FOO_BAR_DIR (kept '.../foo-bar', ignored '.../foo_bar') — ...
```

每次构建都重复，并且 `dep_dir("foo_bar")` 得到 `foo-bar` 的目录，是一个错误的回答。同一缺陷出现在四个通道中：

| 通道 | 变量 | 现状 |
|---|---|---|
| 依赖的目录 | `MCPP_DEP_<NAME>_DIR` | 保留第一个，每次构建警告 |
| 依赖的链接形式 | `MCPP_DEP_<NAME>_LINKAGE` | 同上 |
| 依赖的工具 | `MCPP_DEP_<PKG>_BIN_<TOOL>` | 同上 |
| 声明的载荷 | `MCPP_XPKG_<NAME>_<SUFFIX>`（不带命名空间的拼写） | 两个命名空间声明同一名字时回答先声明的那个，不提示（docs/30 写明） |

**决定。** 一条规则：**多个提供者对应的一个已发布变量不属于其中任何一个。**

- **判定的单位是变量。** 引擎先把每个提供者的每个拼写变换为变量，再按变量分组；一个变量有多于一个提供者时，
  该变量不发布，改为发布 `MCPP_DEP_AMBIGUOUS_<KEY>`（载荷为 `MCPP_XPKG_AMBIGUOUS_<KEY>`），值为可用的精确拼写，
  以逗号分隔。四个通道调用同一个函数，取代 `fillDepDirs` 中按拼写的分组与 `contract_env` 中的三处「保留第一个」。
- **读取者。** `dep_dir` 已按变量读取歧义的记录，不需改动；`dep_linkage`、`dep_bin`、`xpkg_dir` 以同一形式读取，
  遇到歧义时结束程序并说出可用的拼写（与 `dep_dir` 相同，写在标准输出，由引擎在程序非零退出时显示）。
- **构建时不提示。** 一个有歧义的名字只有被询问时才产生错误的结果，被询问时读取者拒绝；因此删除每次构建重复的
  两种 `collides on` 警告。`contract_env` 在写出变量时核对没有重复，重复即引擎的缺陷，以错误报告。
- **没有任何拼写可以区分时。** `foo-bar` 与 `foo_bar` 的每个拼写（包括限定名 `mcpplibs.foo-bar` 与 `mcpplibs.foo_bar`）
  都是同一个变量，构建程序无法通过契约找到其中任何一个。读取者的拒绝说明这两个名字只在变量不能表示的字符上不同，
  办法是改名其中一个包（清单没有给依赖取别名的键）。Cargo 也把 `-` 与 `_` 视为同一个包名。

**未采纳。**
- **单射的变换**（例如以 `__` 表示 `.`）：改变已发布的变量名，违反 AGENTS.md §3「已发布的键不改名」。
- **另一个按精确拼写索引的文件通道**：同一事实的第二个通道，类型化接口与直接读取环境变量的程序会得到不同的回答。

**旧的回答经过弃用期，不立即拒绝。** 今天回答了的名字（变换后冲突的依赖名回答保留下来的第一个；不带命名空间的载荷名
被两个命名空间声明时回答先声明的）在一个弃用期内照旧回答，并在被询问时警告，指出统一规则下的写法；弃用期结束后按上面的
规则拒绝。今天已经拒绝的（#862 的共有末段）不变。

- **登记。** 弃用登记表（`modules/manifest/src/compat/registry.cppm`）增加一行 `ambiguous-contract-name`：`since` 为
  2026.11.21.1，`floor` 按 docs/51 的规则取，`canonical` 为「以精确拼写询问（`xpkg_dir("<ns>", "<name>")`、限定名），
  没有可区分的拼写时改名其中一个包」。警告的文字、`mcpp why compat` 的记录、docs/51 的表与 `--strict` 的拒绝都由这一行导出，
  与其余的弃用读法相同。
- **只在被询问时警告。** 弃用期内引擎照旧发布旧的变量，并同时发布 `*_AMBIGUOUS_<KEY>`。读取者（`dep_dir`、`dep_linkage`、
  `dep_bin`、`xpkg_dir`）见到二者时返回旧的回答，并写出指令 `mcpp:deprecated=ambiguous-contract-name<TAB><名字><TAB><可用的拼写>`；
  引擎把它记为该包构建程序的一次弃用读法（`record_use`），以弃用的警告显示。没有询问这个名字的程序不受影响，构建时也不再有
  每次重复的 `collides on`。指令随引擎接口的协议版本加一。
- **策略。** 弃用的警告是 `always`（3.6）：它改变这次构建的结果（回答的是两个提供者中的一个），并在 `floor` 变为错误。
- **直接读取环境变量的程序。** 不经类型化接口的程序得不到警告；弃用期内它读到的值与今天相同，弃用期结束后变量不再发布。
  docs/30 写明这一点，并建议使用类型化接口。
- **`floor` 之后。** 旧的变量不再发布，读取者按本节的规则拒绝；登记表的行按 docs/51 删除，单元测试 `test_pm_compat` 在
  `MCPP_VERSION` 到达 `floor` 时指出它。

**兼容。** 原来唯一的变量不变。发生冲突的变量与不带命名空间的载荷名在弃用期内回答不变，被询问时警告；docs/30 的两处陈述
改为弃用期内的行为与之后的规则，CHANGELOG 记在 Deprecated。

## 4. 宿主、跨平台、依赖与升级

- **宿主。** 构建程序总以宿主工具链编译；交叉构建多一份 `std` 模块；宿主平台为 openkal 时程序在宿主编译、经 `okrun`
  运行。编译读取随宿主的那次编译，与目标无关。
- **跨平台。** gcc 与 clang 用扫描 depfile，MSVC 用 `sourceDependencies`；`mcpp.build.depfile` 已处理前缀的改写与 Windows
  的大小写折叠。路径按 AGENTS.md §4 比较 `path`、摘要取 `u8string()`，不经 `.string()`。
- **工作区。** 一组中有多个程序时编译读取按程序记录；成员共用的 `tools/common.hpp` 是常见写法，修复后被跟踪，
  并属于 3.3 的 B。
- **升级。** 旧的 `programs.json` 没有 `reads`，视为不当前，每个工程重新编译并运行一次；home 中旧身份的运行缓存不再命中，
  由 `mcpp cache gc` 按未使用的天数回收。没有清单键与命令行的改变。
- **一致性。** 与主构建、单元缓存使用同一个 depfile 解析函数、同一种摘要备忘、同一条「能观测的以观测为准」。

## 5. 规范与文档

- **SPEC-007 R10.3** 改为：程序的身份覆盖预测部分与编译读取；编译读取取自子构建对程序单元的依赖记录，不含 store 与
  build-dependency 的树中的文件；运行缓存以身份为键；所有程序的预测部分与编译读取都未变时不规划构建程序图。
  新增 R10.8（用法的提示，3.3）、R10.9（包外读取，3.4）、R10.10（契约中名字的歧义按变量判定，3.7）。状态标为已实现，
  版本 2026.11.21.1。
- **docs/30（英、中）。** 「Incremental: declared inputs」改为两段：编译读到的文件由引擎观测，运行时读取的文件与变量仍需声明；
  新增「Headers or modules」一节：两种不推荐的用法、推荐的写法与首次构建的提示；「`import std;`」一节指向它。
- **docs/50（英、中）。** 新的诊断代码与原因标记。
- **docs/30** 的「A name two dependencies answer to is no package's」改为按变量陈述，并写明只能改名的情形；`xpkg_dir` 一节中
  「回答先声明的」改为拒绝。
- **CHANGELOG** 2026.11.21.1 的 Fixed（#991；#852 的按变量判定）、Added（提示的机制、用法的提示、包外读取的警告与发布的拒绝）
  与 Deprecated（`ambiguous-contract-name`：冲突的依赖名与不带命名空间的载荷名的旧回答，被询问时警告）。

## 6. 测试

| 编号 | 断言 | 位置 |
|---|---|---|
| T1 | #991 的复现：头文件改变后程序重新编译并重新运行，输出新值；删除 `target/.build-mcpp` 之后同样 | `tests/e2e/build-program/` 的新用例（gcc） |
| T2 | 只改变一个与程序无关的源文件时，程序既不编译也不运行（`--verbose` 的 `cached`） | 同上 |
| T3 | 快速路径：头文件改变后的第一次构建不报告 `Finished ... 0.00s` | 同上 |
| T4 | 提示：第一次构建出现一次，第二次构建没有，`mcpp clean` 之后再出现；工作区中两个成员合并为一条 | 同上 |
| T5 | 包外读取：本地构建警告一次；`mcpp publish --dry-run` 拒绝并给出原因标记；文件未被 git 跟踪时同样拒绝 | `tests/e2e/pack/` 或 `publish` 的用例 |
| T6 | 旧格式的 `programs.json`（无 `reads`）：重新编译一次，之后当前 | `tests/e2e/build-program/` |
| T7 | MSVC：T1 与 T2 | `tests/e2e/windows/` 的新用例 |
| T8 | clang（macOS）：T1 | `tests/e2e/toolchain/` 或 macOS 的作业已运行的程序 |
| U1 | 编译读取的分类：store、build-dependency 的树、本包、包外；Windows 的大小写与前缀 | `tests/unit/` |
| U2 | 提示的策略：`always` 与 `once` 的判定，键的合并写入（两个写者），合并的格式（五个以上的对象），`--quiet`、`--verbose` | `modules/notice/tests/` |
| T9 | 提示的机器输出：被抑制的提示在 `--format json` 中 `shown` 为假；`check_diag_sink.sh` 在本仓库通过 | `tests/e2e/cli/` 与 CI 的门 |
| T10 | 名字的歧义（3.7）：依赖 `foo-bar` 与 `foo_bar` 构建时没有 `collides on`；弃用期内 `dep_dir("foo_bar")` 返回旧的回答并给出弃用的警告，`mcpp why compat` 列出它，`--strict` 拒绝；用例 980 的断言保持；`dep_bin` 与 `xpkg_dir` 的同一情形 | `tests/e2e/deps/graph.cpp` |
| U3 | 分组函数：按变量分组、精确拼写的列表、四个通道的变量形式；载荷的不带命名空间的拼写被两个命名空间声明 | `tests/unit/` |

面的账本（测试方案 5.16）中新的诊断代码与原因标记由 T4、T5 覆盖。迁移到汇点的约 100 处提示保留原有的文字，已有的 e2e 断言
继续成立；文字因去重而少出现的地方由完整的 e2e 运行核对。

## 7. 实施的任务与依赖

| 编号 | 任务 | 位置 | 依赖 |
|---|---|---|---|
| P0 | 提示的机制：`modules/notice`（登记表、策略、键的保存、合并的格式），`mcpp.diag` 接入，按 AGENTS.md §4 的四处接入 | `modules/notice`、`src/diag.cppm` | — |
| P0b | 迁移：直接的 `ui::warning`、`ui::note` 与 stderr 的写入改为经由汇点；删除 `print_hint_once` 与 `Said` 的静态集合；`check_diag_sink.sh` 进入 CI 的门 | 第 3.6 节表中的 31 个文件；`.github/tools/` | P0 |
| P1 | SPEC-007 R10.3、R10.8 至 R10.10 | `docs/specs/build-plugins.md` | — |
| P2 | 编译读取：从子构建取程序单元的读取并分类，写入 `programs.json`，进入身份 | `src/build/prepare/host_graph.cpp`；分类函数进入 `modules/` 中与构建程序记录同一个包 | P1 |
| P3 | 快速路径与运行记录读取同一份 `reads`；声明的文件先比较大小与时间 | `src/build/build_program.cppm` | P2 |
| P4 | 提示（3.3）与包外警告（3.4），登记为 `once` | `src/build/prepare/host_graph.cpp`、`modules/notice` 的登记表 | P0、P2 |
| P5 | 发布的拒绝与原因标记 | `src/cli/cmd_publish.cppm`、`src/build/refusal.cppm` | P2 |
| P5b | 名字的歧义按变量判定：一个分组函数供四个通道使用；`dep_linkage`、`dep_bin`、`xpkg_dir` 读取歧义的记录；删除两种 `collides on` 警告，重复的变量为引擎错误（3.7） | `src/build/prepare/graph_provisioning.cpp`、`sources.cpp`、`src/build/build_program.cppm`、`src/build/hostprogram.cppm` | — |
| P6 | 测试 T1 至 T10、U1 至 U3 | 见第 6 节 | P0 至 P5b |
| P7 | docs/30、docs/50（英、中），CHANGELOG；mcpp-contributing 技能：用户可见的提示经由 `mcpp.diag`，策略不是缺省的提示先登记 | — | P0 至 P5 |

文件的行数与函数的长度按 AGENTS.md §4：`host_graph.cpp` 现为 1,200 余行，P2 与 P4 的新代码以函数加入，单个函数不超过约 400 行。

## 8. 未采纳

- **每次规划子构建，由 ninja 判断。** 正确，但无事可做的构建从 6 ms 升到 110 ms 以上（3.2 的测量）。
- **把程序所在包的整棵树并入身份。** 任何源文件的改变都使程序重新运行，运行缓存失去作用；代码生成类的程序代价最大。
- **以二进制的内容为运行缓存的键。** MSVC 的二进制默认含时间戳与 PDB 的 GUID，每次重新编译都会改变；它只在重新编译之后
  才有区别，而重新编译已由编译读取决定，是同一信息的第二个来源。
- **只对引号包含给出警告（#991 的第二个建议）。** 包含是文档允许的用法；修复之后它能正确工作，警告会把正确的用法报告成问题。
- **每次构建都提示 3.3 的用法。** 重复的提示使真正的警告被忽略；一个工程提示一次已足够让作者知道。
- **按时间或次数限制提示（「每天一次」「最多三次」）。** 状态与时间相关，同一工程在两台机器上表现不同；`once` 的键只取决于事实。
- **把「说过」记在 home 中。** home 为多个工程共用，一个工程的提示会因另一个工程而消失；`mcpp clean` 也无法重置。
- **`once` 用于 `degraded`。** 降级改变这次构建的结果，抑制它即违反 AGENTS.md §2。
- **观测运行时的读取。** 见 3.1。

## 9. 自评

- **架构。** 三道闸门的输入由一份记录给出，以后新增的编译输入（例如 `#embed`）不需要再补任何一道闸门。预测部分仍有
  一个模型，但它只含观测不到的事实。
- **稳定性。** 观测的边界与 ninja 相同：depfile 不记录查找失败（包含路径中新出现一个同名文件遮蔽旧文件、`__has_include`
  所查的文件后来出现），`-MMD` 不列出系统目录中的头文件（`gcc@system` 之下 `/usr/include` 的改变不被观测，与主构建相同）。
  这些边界在 docs/30 中写明。
- **前提，在实施中核对。** MSVC 的程序单元在子构建中写出 `sourceDependencies`，且其路径可以由 `mcpp.build.depfile` 读出；
  不成立时由 P2 为程序单元加上该选项。快速路径读取各组 `programs.json` 之后，复现工程无事可做的构建的用时（现为 6 至 9 ms）
  不增加 1 ms 以上；测量记录在测试方案的实施记录中。
- **简洁。** 不新增清单键与命令行选项；新增一个记录字段、一个提示的包（取代三处各自的实现）、两个诊断代码与一个原因标记。
- **一致性。** 提示与弃用读法同一范式（一张表，行导出一切）；所有提示经由一个汇点，`--strict`、机器输出与去重对每一条都成立。
  契约的四个通道以同一条规则处理名字，判定的单位就是发布的单位（变量）。
- **用户体验。** 修复是无感的；提示一个工程一次、两行、给出写法；发布的拒绝发生在消费方受影响之前。
- **兼容性。** 旧记录使每个工程重新编译一次；已发布的包不受影响，除非它读取包外的文件，此时它对消费方本来就是坏的。
- **跨平台。** gcc、clang、MSVC 三种依赖记录都由已有的解析函数处理，测试覆盖 Linux、macOS 与 Windows。
- **测试覆盖。** 第 6 节的 T1 至 T10 与 U1 至 U3；面的账本覆盖新的代码。
