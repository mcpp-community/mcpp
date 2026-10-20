# 依赖 image 的资源归属、模块扫描边界与锁文件稳定写入

状态：待评审；本文件提出实现方案，不表示运行时修改已经完成。

日期：2026-10-21。分析基线：`cdc8daa7c3166188a28a338ce1cb66b3e39f0719`。

关联：[mcpp#826](https://github.com/mcpp-community/mcpp/issues/826)、
[mcpp#816](https://github.com/mcpp-community/mcpp/issues/816)、
[mcpp#807](https://github.com/mcpp-community/mcpp/issues/807)。

读者：评审和实现这批修复的维护者。本文裁定现有契约的缺陷、合理优化与能力扩展的边界，
给出实现接点和验收条件。实现采用单一 PR，以连续 commit 作为 checkpoint；
不引入统一三类子系统的新框架。

## 1. 裁定依据与结论

裁定顺序是现行规范、用户文档、设计记录、实现与实测。实现中的过滤条件不是行为正确的
充分依据；合法 C++ 代码也不是默认扫描器承诺支持其全部发现方式的充分依据。
设计记录中未落地的扩展不作为当前能力承诺。

| 项目 | 分类 | 本次决定 |
| --- | --- | --- |
| #826：依赖自己的 EXE、DLL 丢失资源 | 现有资源所有权契约的实现缺陷 | 修复，资源按 image 的所属包规划 |
| #816：include 携带的 import 未进入文本扫描图 | 默认文本扫描的设计边界，不是本例所声称的通用传递映射缺陷 | 不实现自动兼容；本 PR 补齐边界说明和受支持路径验证，合入后按 not planned 关闭 |
| #807：emit 把自己写出的 host 锁文件列入 watch | SPEC-005 R6.4 的契约缺陷 | 修复 watch 的输入与输出边界 |
| 内容相同的 mcpp.lock 仍被覆盖写入 | 公共写入层缺少幂等性 | 补充 write-if-changed，普通 build 与 host 子构建共用 |

依据：

- [SPEC-005 §2、§6](../../docs/specs/build-database.md)：emit 不写工程目录，
  不写入 watch 列出的文件；inputs-fingerprint 描述输入内容。
- [SPEC-004](../../docs/specs/manifest-semantics.md)：声明按键的作用域生效，
  包的资源描述该包的产物。
- [资源设计 A3、A5](2026-08-07-windows-resources-and-version-identity-design.md)：
  资源进入所属包的 PE image，依赖资源不传播给消费者。
- [扫描器设计 §2、§3-pre、§4](2026-07-08-scanner-backend-abstraction-design.md)：
  默认保持文本扫描；发现不足时允许声明并由编译器审计；不做部分预处理求值。
- [BMI 设计 B1 与被排除方案](2026-10-01-pack-drive-and-selection-independent-compile-design.md)：
  BMI 按提供包放置，模块按导入关系解析，避免目录搜索选中错误提供者。
- [scan_overrides 用法（§2.8.3）](../../docs/zh/04-mcpp-toml.md)：
  已有声明入口，不新增 manifest 键。

## 2. 证据与验证边界

源码检查基于上述提交。运行探针使用已安装的 mcpp `2026.10.19.1` 与 LLVM `23.1.3`，
Linux x86_64；没有用该提交重新编译 mcpp，因此源码判断与发布二进制结果分开记录。

| 项目 | 已验证内容 | 尚未验证内容 |
| --- | --- | --- |
| #826 | windows_resources.cpp 只收集根包或 selectedMember，且跳过 artifactOf 非空的 link unit | 本次没有在 Windows 提取修复前后的 PE 资源 |
| #816 | 本地 path 模块依赖下，头文件内 import 失败，直接 import 成功；为源文件声明 imports 后成功 | 未验证所有编译器及可变宏配置 |
| #807 | emit 的 watch 包含合成 host 的 mcpp.lock；公共 writer 无条件截断写入 | 本次 path 和 Git 简化探针没有生成 host 锁文件，未完成同内容 mtime 变化的运行复现 |

#816 的编译器 .ddi 正确记录 `mylib`，但失败 TU 的编译参数没有 modmap。
规划器看不到 include 内的 import，导致已有的传递闭包计算没有起点；不是所有传递导入都失效。

## 3. R：按 image 所属包规划 Windows 资源

### 3.1 保持的语义

每个实际参与源码构建的包，其资源只进入它自己拥有的程序或共享库。
依赖包是否被命令行直接选择，不影响它自己的 image 是否兑现自己的资源声明。
artifactOf 表示交付关系，不表示该 image 不需要资源。

资源不沿依赖边传播。不把静态依赖的 manifest、图标或版本块合并到消费者。
不修改已交付的预编译 EXE、DLL。非 PE 目标仍不编译资源；声明文件的存在性校验
保持现有跨目标规则。测试 image 保持现有规则：只接收显式请求的应用 manifest，
不接收普通图标与版本资源。

### 3.2 实现接点

1. 在 `src/build/prepare/windows_resources.cpp` 中按包身份收集 subject：保留根包与
   selected member 的现有校验，再纳入本计划实际源码构建且拥有程序或共享库 link unit 的依赖包。
   按限定包名去重，不遍历未参与本计划的 workspace 成员，不重新解析其他闭包。
   subject 及声明文件校验不按 PE 设门；只有资源编译按 PE 设门，避免同一声明的有效性依赖宿主或目标。
2. 使用 `LinkUnit::package` 与 subject 的限定包名精确匹配。普通构建的根包也有明确身份，
   不再以空 owner 表示“拥有所有 image”。移除以 shared target 名字猜测所属包的分支。
3. artifact image 通过同一归属判断进入资源流程，移除无条件排除 artifactOf 的判断。
   已有 artifact 部署步骤复制最终链接产物，不增加部署后嵌入步骤。
4. 资源相对路径、include 搜索与合成元数据全部读取所属包。资源输出按包隔离；
   同包多个 image 可复用作者脚本编译结果，合成 OriginalFilename 等信息仍按 image 生成。
5. image 的 target 属性也从同一所属包解析，避免同名 target 导致 code page、long paths
   或 manifest 冲突检查使用另一个包的声明。
   冲突检查只考虑本计划实际生成的相关 image，不让未启用 target 的属性错误拒绝当前构建。
6. 保持资源工具惰性解析、缺工具硬错误、资源输入跟踪和 no-image 诊断语义。
   依赖的 image 已存在时不得误报该依赖“没有 image”。

不引入新的 owner 字段；`LinkUnit::package` 已提供所需身份。
输出命名的改动限定在解决包间冲突所需范围，不附带全局目录重排。

### 3.3 验收

- 直接构建与 artifact 构建均能从 updater.exe 提取其声明的 manifest。
- 普通依赖 DLL 携带自己的资源，消费者 EXE 保留自己的资源。
- 不同包同名 shared target 的资源、manifest 与合成元数据不串用。
- `.rc`、`.manifest`、icon 或 extra-inputs 改变后，相关资源与 image 正确增量重建。
- 冷构建、依赖对象缓存命中以及 `-p`/`--workspace` 切换均保持归属正确。
- 静态依赖不传播资源；非 PE 和测试 image 的现有规则不回归。

在资源 E2E 与 workspace 资源测试基础上增加依赖 image 场景，检查 Ninja 链接输入和
最终 PE 内容。Windows MSVC ABI 行与 MinGW 行分别验证，不用“构建退出 0”代替资源检查。

## 4. M：#816 的处理边界

### 4.1 不以探针可通过作为支持承诺

本次裁定：不实现 #816 请求的自动兼容，不保留待定的运行时修复任务。
默认文本扫描不展开 include，这与既有确定性和成本边界一致。issue 的复现把直接 import
放在头文件中，超出该默认发现机制的范围；不是规划器对已知导入边遗漏传递遍历。
本 PR 明确该边界并验证既有传递映射契约，合入后将 #816 按 not planned 关闭。

此前探针通过直接 import 或 scan_overrides 构建成功，只证明当前实现存在这两条可执行路径。
后者不自动成为 #816 的推荐 workaround，也不据此增加兼容测试来长期固定这项用法。
scan_overrides 保持原有用途与声明审计契约，不承担绕过架构边界的专用修复责任。

控制用例为：源文件中存在文本可见的直接 import，提供者与依赖关系符合公开接口及闭包规则，
规划器应传递解析其可达模块。源码已有闭包遍历，同一探针把 import 放回源文件后构建通过，
两者共同支持本次裁定。因此本方案没有模块映射运行时改动，也不列出“以后决定”的子任务。

### 4.2 本批交付与排除事项

本 PR 的 #816 交付固定为两项：

1. 在默认扫描器的中英用户文档中明确：不展开 include；需要自动发现的 import 写在被扫描的
   源文件中。不新增头文件 import 兼容教程，不把 scan_overrides 描述成该 issue 的专用补丁。
2. 增加或复用文本可见 import 的传递映射控制测试，覆盖“消费 TU → 本包模块 → 依赖模块”，
   检查模块映射与实际编译结果。按已有编译器 CI 行验证，确认该契约没有平台特例。

本 PR 不改模块扫描和审计的运行时接受规则，不实施错误字符串识别或额外诊断补偿。
测试或实现若与上述证据出现矛盾，视为当前 PR 验收失败，先解决矛盾再合入，
不得用“后续处理 #816”通过验收。

保留现有 scan_overrides 强制审计和可选全量审计行为，不默认收紧全部 TU 的接受规则，
也不把 P1689 调试入口提升为跨编译器生产配置。PR 描述记录本次“不实现”的依据与交付结果。
不使用 `Closes #816` 自动将其标成 completed；合入后显式以 not planned 原因关闭，
这属于本次 PR 的完成清单，不是新的评估任务。

### 4.3 不采纳的方案

| 方案 | 排除依据 |
| --- | --- |
| 所有依赖 BMI 扁平化或增加平铺软链接 | 破坏按提供者隔离，同名模块冲突 |
| 给所有依赖目录追加 BMI 搜索路径 | 旧 BMI 可能回答错误提供者，已有设计明确排除 |
| 所有 TU 注入全图或整包闭包的 modmap | 引入不可达模块，破坏按 TU 导入关系及选择稳定性 |
| 默认扫描递归展开 include 或部分求值预处理条件 | 改变文本扫描的成本与确定性边界 |
| dyndep 发现额外 import 后直接修补规划图 | 引入规划后的第二条建图通道，扩大调度、缓存和数据库一致性负担，本方案不采用 |

不将 #816 泛化为“传递模块不可用”，也不以某个平台偶然能从 BMI 内记录的路径找到依赖
作为跨平台兼容承诺。关闭理由是本次已明确的设计边界，不是声称 issue 中的原始写法已被修好。

## 5. L：公共锁文件内容相同则不写

### 5.1 历史与语义

已完成的稳定性修复包括 [#369](https://github.com/mcpp-community/mcpp/pull/369) 的
build/test 内容一致、[#676](https://github.com/mcpp-community/mcpp/pull/676) 的跨平台哈希、
[#743](https://github.com/mcpp-community/mcpp/pull/743) 的包身份去重。
这些修复保证相同语义不会产生不必要的内容差异，没有保证文件不被重写。

`src/pm/lock_io.cppm::write` 仍直接打开 ofstream。其覆盖写入实现自 2026 年 5 月迁入
该模块后保留。本地可见历史中没有公共 writer 曾实现 write-if-changed 后又回退的证据。
既有 E2E 844 的 cmp 只能证明内容相同，不能证明 mtime 未变化。

本次只改变落盘行为。是否重新解析依赖、版本选择、部分 workspace 合并、devOnly 排除、
`--locked` 断言及锁文件权威范围均保持原有语义；不增加“完全冻结依赖”的承诺。

### 5.2 写入流程

在公共 `mcpp::pm::write` 实现，`mcpp.lockfile` shim 和全部调用者共享，不只改 prepare 调用点。

1. 将 Lockfile 序列化一次，形成待发布的完整字节。
2. 二进制读取已有文件；字节相同则直接返回成功，不创建临时文件，不改 mtime。
3. 比较时仅将 CRLF 视为 LF 等价的行结束形式；相同内容的旧 CRLF 文件保留原字节与 mtime。
   不解析 TOML 后比较，不忽略字段、注释或顺序，不清理其它空白。该规则对所有平台相同，
   避免为了新增优化先制造一次无意义改写。
4. 文件不存在则创建；内容不同则沿用现有写入方式，检查打开、写入和关闭结果，返回路径明确的错误。
   继续采用现有文本输出约定，不在本批强制统一已有文件的物理行结束符。
5. 已有文件读取失败不能被当作“内容相同”。区分不存在与其他 I/O 错误；返回带路径的错误。
   writer 的失败不得在 prepare 中被 `(void)` 静默丢弃，相关调用者按现有诊断契约处理。

本批撤回原方案附带的临时文件原子替换：该改动不是同内容不写的必要条件，还涉及符号链接、
文件权限和 Windows 占用语义。保留现有文件对象行为，避免为一个幂等性优化增加平台依赖或文件框架。
现有非原子更新与多进程并发合并的局限不在本批扩大为新保证。
错误传播只覆盖现有 writer 的调用者，不重构包管理命令的事务模型。

### 5.3 验收

- 首次创建和内容更新输出正确；同内容再次写入保留原字节与 mtime，LF/CRLF 均覆盖。
- 测试先将文件时间设为可区分的旧值，再写同内容，避免文件系统时间粒度造成假通过。
- I/O 失败可见；同内容分支不要求写权限，不因只读但可读的已正确锁文件而失败。
- 已有符号链接、权限与路径编码的行为不因优化改变；Windows 由真实平台测试验证。
- 普通 build 以及强制进入规划路径的第二次 build 均保留未变化锁文件的 mtime。
  不能用整个构建的快速返回代替 writer 验证。
- 原有 build/test 内容稳定、跨平台摘要、workspace 去重与 `--locked` 测试继续通过。

writer 单测覆盖相同字节和错误路径；扩展 E2E 844 或增加专门 E2E，验证真实规划调用。
跨平台运行包括 Windows，覆盖行结束符、只读文件与非 ASCII 路径差异。

## 6. W：emit 的 watch 只报告外部输入

### 6.1 边界修复

`src/build/build_database.cppm::render` 当前对每个 member 无条件 watch
`ctx.projectRoot / "mcpp.lock"`。buildPrograms member 的 projectRoot 是合成 host 目录，
该锁文件由同一次规划产生，因此违反 SPEC-005 R6.4。

按来源而非路径字符串过滤：真实工程／workspace 锁文件继续监视；合成 host 锁文件
不作为外部输入。不要仅使用“路径在 MCPP_HOME 下”作为过滤条件，真实可编辑依赖也可能
位于该目录中。

同一 host 图的合成清单也是输出，随此次变更审计其 watch 来源并排除合成项。
保持真实源码包 mcpp.toml、build.mcpp、声明输入与源文件 glob 的监视。
引用真实的 path build dependency 时，该依赖的可编辑输入仍需出现在 watch 中。
稳定存储与 Git 检出继续遵守 SPEC-005 R6.2，不因 host 图加入而扩大监视范围。

过滤规则同时约束 inputs-fingerprint 的输入集合，不仅从最终 watch 数组删除字符串。
生成目录可以被 emit 写入，工程目录仍不能写入；不得借此绕过根锁只读规则。

### 6.2 与 write-if-changed 的关系

L 与 W 各有独立责任。L 稳定同内容写入；W 保证“输出不进入输入集合”。
只做 L 时，解析变化仍会写入被监视的输出，不满足 R6.4；只做 W 时，普通 build 仍可能
无意义更新锁文件时间戳。语言服务器按内容去重可以防御重复事件，但不替代引擎契约修复。

### 6.3 验收

1. 使用版本型 build dependency，确认 host 锁文件真实存在且非空，再连续执行 emit。
   path-only 夹具和被 host 图转换为 path 的 Git 夹具不足以覆盖本缺陷。
2. watch 与 fingerprint 输入不包含合成 host 锁文件或合成清单，仍包含真实项目和
   可编辑 build dependency 的输入。
3. 同输入重复 emit 的 fingerprint 稳定，工程锁文件内容与 mtime 不变。
4. 修改真实依赖声明、build.mcpp 或声明输入后，相关 fingerprint 与模型按实际变化更新。
5. 版本约束改变导致合成 host 锁变化时，它仍不进入 watch；不产生自触发反馈环。

扩展 build-database 渲染测试与 E2E 688/913。现有 913 的 path dependency 不能作为
“同内容 host 锁文件不重写”的证明。测试文件名按仓库编号约定选择，不预占 issue 编号。

## 7. 单 PR 与 commit checkpoint

用户明确要求本方案通过单 PR 实现，本节取代按问题分 PR 的原安排。
同一分支上按以下顺序形成可审查的 commit；各 commit 连同对应测试构成 checkpoint，
不先提交一个明知不能构建的实现，再在后续 commit 补齐必要接线。

| Checkpoint | commit 内容 | 主要范围 | 完成条件 |
| --- | --- | --- | --- |
| C0 | 评审后的设计基线 | 本文件 | 裁定、范围和验收明确 |
| C1 | L：锁文件幂等写入 | lock_io、现有 writer 调用者、单测与规划 E2E | 同内容不写，变化正确写入，错误可见 |
| C2 | W：build-database 输入边界 | render 的输入来源、版本型 host 夹具 | 满足 R6.4，真实输入变化仍可观察 |
| C3 | R：资源归属 | subject 与 image 匹配、PE 与跨目标 E2E | 依赖 image 兑现自身资源且不传播 |
| C4 | M：边界裁定和整体收尾 | 可见 import 控制用例、默认扫描边界的双语说明、规范复核 | 无兼容补丁，受支持路径不回归，三个 issue 的关闭结论明确 |

单 PR 覆盖三个 issue 的最终处置：#807、#826 修复完成；#816 明确不实现自动兼容。
合入后的完成清单：

- #807、#826：在修复及验收完成后以 completed 关闭，可由 PR 关闭关键字完成。
- #816：显式以 not planned 关闭，命令为
  `gh issue close 816 --repo mcpp-community/mcpp --reason "not planned"`，不附加评论。
- 检查三个 issue 均已关闭，且关闭原因与本设计一致；只合入 PR、留下 #816 未处置不算完成。

上述关闭动作在未来实际实现、验证和 PR 合入后执行，本次设计更新不提前关闭 issue。
checkpoint 用于分支内审查、定位和必要时回退；按后续用户授权，最终以 squash 合入单 PR。
checkpoint commit 保留在 PR 历史中。commit message 与 PR 标题按项目规范使用英文。

实现前在本分支同步最新 main，不丢弃已评审设计。各 checkpoint 使用 fresh binary 验证其相关行为；
最终提交再完成组合验证和 PR required checks。资源修复覆盖 Windows MSVC ABI 与 MinGW 行，
锁文件与 emit 覆盖 Linux、macOS、Windows；工具链缺失明确记录，不以跳过作为通过。
2026-10-21 实施授权：完成单 PR 实现、测试、CI、自审、squash 合入、release 和 xlings 生态验证。
不在 issue 下发表评论；#816 合入后按 not planned 关闭。release 资产生成后即可本地 gtc 补传，
不等待远程 mirror leg；校验下载字节与摘要后再推进索引与真实安装。

用户文档只陈述已经实现的能力与限制，英文和简体中文同步；若实现改变 SPEC 的保证或
输入定义，同 PR 更新对应规范。设计决策保留在本文件，落地后按文档规范追加状态记录。

## 8. 多视角自审与修订结果

| 视角 | 原方案风险 | 修订后的约束与判据 |
| --- | --- | --- |
| 规范符合性 | 把 #816 探针成功用法当成应支持能力，或一直保留待定结论 | #816 明确不实现自动兼容；本 PR 完成边界说明和控制测试，合入后按 not planned 关闭 |
| 架构简洁 | 引入新的 owner、文件发布框架或扫描补偿通道 | 复用 LinkUnit::package、公共 writer 与 render 输入收集；不加 manifest 键和配置开关 |
| 全局一致性 | 资源补丁只修 artifact，或 watch 只删最终字符串 | 所有本计划源码构建 image 按所属包处理；watch 与 fingerprint 共用输入来源判断 |
| 跨平台 | PE gate 提前导致非 PE 不校验依赖声明；Windows 换行引发首次重写 | subject/校验与资源编译分离；LF/CRLF 仅作行结束等价，已有相同文件不重写 |
| 稳定性 | 原子替换扩大到权限、符号链接与占用语义；默认全量审计改变接受范围 | 删除非必要原子发布，保持文件对象行为；不默认收紧扫描审计 |
| 兼容性 | 以兼容头文件 import 为目标固定 workaround，或为所有 TU 注入模块 | 不承诺该用法；不修补规划图、不增加搜索目录、不改变 BMI 放置和模块可见性 |
| 缓存与增量 | 只验证热构建或整个 build 快速返回 | 同时验证冷构建、强制规划、对象缓存命中、成员选择切换与真实资源重建 |
| 可观测性 | 版本型 host 锁不存在也宣布复现；写入错误仍被忽略 | fixture 先断言非空锁存在；writer 调用错误可见；所有结论标注平台与证据范围 |
| 变更控制 | 分 PR 与用户交付要求冲突 | 单 PR，C0—C4 连续 commit checkpoint，保留审查与回退边界 |

全局组合验收至少包括：build → emit → build，build → test → build，以及 `-p` →
`--workspace` → `-p`。仅由选择或命令顺序改变造成的锁内容抖动、资源串用和模块参数污染均不接受。
这些序列分别验证其既有契约，不要求不同命令输出的整个数据库或测试依赖模型完全相同。

资源缓存特别核对：资源计划仍在已有 P13 路径完成，不能因 C++ 对象命中缓存漏掉资源边；
只读锁特别核对：相同内容不需写权限，实际内容变化仍按原写入语义报告失败。
watch 特别核对：不以 buildPrograms 一刀切丢掉 host 图引用的真实可编辑包输入。

文档属于待评审设计，以上是源码与设计层面的自审结论；跨平台运行验收留待实现阶段完成，
不把本次文档检查表述为运行时验证通过。

## 9. 评审决策点

- 接受 R：资源归属使用现有 package 身份，覆盖依赖自己的 EXE/DLL，不传播静态依赖资源。
- 接受 M：#816 不实现自动兼容；本 PR 完成双语边界说明与控制测试，合入后按 not planned 关闭。
  不将 override 成功探针升级为推荐 workaround 或支持承诺，不保留未定实现任务。
- 接受 L：公共 writer 同内容不写，LF/CRLF 等价比较不改已有字节；保留现有更新方式与文件对象语义。
- 接受 W：生成的 host 锁文件与清单不作为 watch 输入，保留可编辑 build dependency 的真实输入。
- 接受单 PR、连续 commit checkpoint，最终 squash 合入；全量审计默认开启、自动预处理扫描、
  原子发布与并发锁文件事务均不纳入本批。

## 10. 实施任务依赖与发布验收

依赖关系：C0 → C1 → C2；C0 → C3；C0 → C4。所有实现与双语文档完成后，
执行组合测试、跨平台 CI 与综合自审，再 squash 合入。版本 bump 与 CHANGELOG 在同 PR 完成，
bootstrap pin 保持已发布值；发布后更新不是发布前 CI 的前提。

发布依赖：合入且 CI 通过 → tag/release canaries → 四平台资产 → GitHub/GitCode 摘要验证
→ 生态索引 PR 及传播 → 精确版本与 latest 的真实 xlings 安装和运行验证。
资产可用后本地 gtc 上传与其余 release job 可并行；release CI 最终仍需全部成功。
真实验证包括普通构建、重复规划锁文件 mtime、build-database 输入边界和可见 import 的模块链。
最后核对 AUR、Homebrew、Nix、PyPI 与各宿主安装检查中实际启用的发布 job，记录可核验结果。
