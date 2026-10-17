---
subject: design
status: active
---

# 升级的连续性：打包修订不在使用路径上替换载荷、替换失败不拆掉可用状态、home 中的悬空记录自愈、真实的升级 lane

- 日期：2026-10-17。状态：修订 3，已实施（libxpkg #50、xlings #678 与 2026.10.17.1、xim-pkgindex #993、mcpp #815）。
- 起因：用户报告。`xlings self update && xlings update mcpp -y` 之后，`mcpp new hhss && mcpp run` 每次失败：
  `toolchain 'llvm@23.1.3': install of 'xim:llvm@23.1.3' failed … sysroot update refused before metadata changed:
  c++@16.1.0: installed asset registration missing`；`mcpp self doctor` 不报错。
- 基线：`main` `f4816141`（mcpp 2026.10.16.3，内置 xlings 2026.10.16.2）；openxlings/xlings `03a2002c`
  （2026.10.16.2）；openxlings/xim-pkgindex `5a70d12`。
- 证据：本地复现（Linux x86_64）；probe PR #814 两轮运行
  [38989869868](https://github.com/mcpp-community/mcpp/actions/runs/38989869868)（11 个 job）、
  [38990143701](https://github.com/mcpp-community/mcpp/actions/runs/38990143701)（3 个 job），见 §2。
- 范围：每个仓库一个 PR：libxpkg（L1，`floor` 的解析）、xim-pkgindex（I1）、xlings（X1–X4，一次发布）、mcpp。
  发布顺序 libxpkg → xlings → mcpp；xim-pkgindex 的规范与检查与之并行，最后合入。不分阶段，不留临时方案。

## 0. 结论

升级之所以不是无感的，是 2026.10.16.1 把「打包修订是否最新」并进了「载荷是否完整」，而 mcpp 在使用路径
（build/run/test/doctor）上把「不完整」当作「未安装」同步重装。于是 index 中任何一次修订号上调，都会在用户
下一次 `mcpp run` 时变成一次替换：在一致的 home 中是一次多余的下载（gcc 101.7 MB、llvm、glibc 36.7 MB），
在带一条悬空记录的 home 中是每次都失败，而 xlings 保留下来的可用载荷被 mcpp 丢弃。CI 全部从空 home 起步，
装出来的修订号永远等于索引的修订号，这条路径从未被执行。

修复是四个不变量，各落到唯一的位置：

1. **使用不变更**（P1）：使用命令不替换一个可用的载荷。修订号不同是「有更新的打包」，不是「不完整」；
   只有低于索引声明的可用下限（`floor`）时才必须替换。
2. **替换失败不降级**（P3）：替换是事务；失败时旧载荷仍可用就继续用，并如实报告。
3. **一个事务只被自己的问题阻止**（P4）：工作区中引用不存在注册的记录是垃圾，xlings 在每个元数据事务开始时
   以 doctor 的同一个函数规范化并报告。
4. **诊断只读且如实**（P5），**升级由真实的上一发布版验证**（P6）。

## 1. 现象与链条

用户机器上的完整链条（代码位置以基线为准）：

1. mcpp 2026.10.16.3 第一次运行，把内置 xlings 从 2026.10.10.2 换成钉住的 2026.10.16.2。
2. `Fetcher::resolve_xpkg_path`（`src/pm/package_fetcher.cppm`）的 `complete_here()` 问提供者
   `payload_status`。xlings 2026.10.16.1 起，`payload_state_()`（xlings `src/core/xim/commands.cpp`）在
   `payload_revision_verdict(...).stale` 时返回 `complete=false`（xlings #676，即 10-16 设计的 X4）。
   llvm 23.1.3 在索引中为 revision 1（xim-pkgindex #938，2026-10-08），载荷记录为 0。
3. `complete_here()` 失败，mcpp 在 `mcpp run` 内调用安装。xlings 计划为 `reinstall: recipe revision 1,
   installed revision 0`，下载、解包、把旧载荷移到一边。
4. 元数据事务的投影预检 `prepare_projection_change`（xlings `src/core/xvm/materialize.cpp`）对被重新配置的
   target `c++` 收集期望的投影：`c++` 的 active 是 `16.1.0`（gcc 的），版本库里没有这条注册 →
   `collect_claims(..., ClaimSource::Present)` 返回 `installed asset registration missing`。事务拒绝，
   旧载荷放回（`kept the previous payload`）。
5. mcpp 再次 `complete_here()`：仍是 `complete=false`（修订号仍旧）→ 报错。失败后的索引刷新打印
   `Refreshing package index — xim:llvm@23.1.3 not found locally (one-time)`：载荷与描述文件都在本地，
   这句话的理由是错的，而且每过一次 debounce 窗口就出现一次。
6. `mcpp self doctor` 的 runtime capabilities 检查调用 `prepare_build`，又发起一次同样的重装；失败被报告为
   `ok (run inside a package to check its runtime capabilities)`，而用户就在包里。它的 toolchain 一节检查的是
   PATH 上的 `~/.xlings/subos/default/bin/g++`，不是 mcpp 为这个项目解析的 llvm。

升级前为什么没事：v2026.10.11.1 的 `resolve_xpkg_path` 只看 `.mcpp_ok` 标记，不看修订号；xlings 2026.10.10.2 的
`payload_status` 也不看。10-16 设计以「mcpp 不写载荷」取消标记、改由提供者回答完整性（§7.1、X4），
`docs/specs/package-identity.md` §10.3 第 3 条把「修订号与索引条目一致」写进了完整性。该设计的兼容一节写的是
「已安装的 mcpp 行为不变」，实现与之相反。

用户 home 的状态（诊断脚本实测）：版本库在 `.xlings.json`（无 `data/versions.json`）；`xim-gnu-gcc` 只剩
`15.1.0`，`c++` 只剩 `15.1.0 / 22.1.8 / 23.1.3`，工作区却以 `16.1.0` 为 active；工作区有而版本库没有的记录
529 条；指向 `/tmp/claude-1000/fakehome/...` 的条目 1827 个（来源见 §3 F6）。

## 2. 实测（probe #814）

上一发布版 2026.10.11.1（自带 xlings 2026.10.10.2）建立 home，装 llvm@23.1.3（Linux 另装 gcc@16.1.0），
`mcpp new && mcpp run`；换成 2026.10.16.3，在同一 home 中 `mcpp new && mcpp run` 若干次。

| 变体 | 平台 | 升级后第一次 run | 结论 |
|---|---|---|---|
| plain（今天的索引） | x86_64、aarch64、macOS、Windows | rc 0，1 s，无下载 | 修订号一致时升级无感 |
| snapshot（xim-pkgindex `ebf1fbb`，2026-10-07，真实的旧修订载荷） | x86_64 | 旧索引仍在时：rc 0，1–2 s，无替换，**用 revision 0 的 llvm 构建并运行成功**；`index update` 后：llvm 项目下载 glibc 36.7 MB 并替换 llvm（19–24 s），gcc 项目下载 gcc 16.1.0-r1 101.7 MB 与 binutils 2.42.1 | 修订号上调在使用路径上变成替换与下载 |
| stamp（载荷记录的修订号与索引不同） | x86_64、aarch64、macOS、Windows | rc 0，12–30 s，重新下载 llvm（Linux 另有 glibc、gcc） | 机制跨平台 |
| orphan（stamp + gcc 的 `c++@16.1.0` 为 active 且版本库无此注册，即用户 home 的形状） | x86_64、aarch64 | **rc 101，每次**（22–45 s 后失败），与用户报错逐字一致；`mcpp self doctor`（项目内）发起安装且返回 0 | 一条悬空记录使替换永久失败；doctor 改变状态且不报告 |

附带的实测事实：

- llvm revision 0 与 1 的 cfg 唯一差别是 glibc 库目录（`glibc/2.44.3/lib` → `lib64`）；glibc revision 4 的载荷
  同时有 `lib` 与 `lib64 -> lib`，所以 revision 0 的 llvm 在 revision 4 的 glibc 上仍然自洽。头文件搜索列表相同。
- orphan 变体中，gcc 项目的运行替换了 gcc（修订 0 → 1），gcc 的 `config()` 重新注册了 `c++@16.1.0`，悬空记录
  随之消失，此后 llvm 的替换成功。即：**重新配置记录的拥有者能修复它，但 xlings 不会主动这样做**。
- `xlings self doctor` 对 orphan home 报 4 条 binding state 发现（`xvm-active-version-missing` 等），
  其中给出的 `run xlings install c++` 把 target 当成了包名。
- 修订号的波及面（xim-pkgindex `5a70d12`，Linux）：llvm 20.1.7/22.1.8/23.1.3 revision 1（10-08，归档不变，
  cfg 随 glibc 布局）；gcc 16.1.0 revision 1（10-12，`16.1.0-r1` 新归档，去掉硬链接，为 Android/FAT）；
  glibc 2.44.3 revision 4（去掉硬链接，为 Android）；binutils revision 2。每个在这些日期之前装过的 Linux home
  都会在升级后的第一次 run 中替换它们。
- macOS 上 `mcpp self doctor` 的 toolchain 一节报告 `/usr/bin/g++`（Apple clang 17），并因一条 Xcode SDK 的
  警告返回 1。

## 3. 根因

### F1 使用路径替换载荷

完整性（载荷在、有内容、没有失败标记）与时效（安装的修订号是否等于索引当前的修订号）是两个独立的事实，
2026.10.16.1 把后者并进了前者。使用路径只该关心前者：一个完整的载荷，连同它安装时解析出的闭包
（载荷中的 `.xlings-resolution.json`），是自洽的，可以使用。修订号上调的原因多种多样：为 Android 去掉硬链接、
随依赖的布局重新生成 cfg、修复一个所有人都会遇到的缺陷。只有最后一种要求已装的载荷必须替换，而这一点只有
写描述文件的人知道。今天的数据模型无法表达它，于是 xlings 和 mcpp 只能对所有修订号一律替换。

### F2 替换失败丢弃可用载荷

xlings 的替换是事务（`PayloadReplacement`，失败放回旧载荷），但 mcpp 在失败后仍以 `complete` 判断，旧载荷
因修订号旧而被判为不完整，于是报错。提示固定为「retry」，而这次失败是确定性的。失败后的索引刷新沿用
`DescriptorMiss` 的理由（`not found locally (one-time)`），对一个本地存在的描述文件既不真也不是一次性的。

### F3 一个事务被别的包的悬空记录阻止

投影预检对「被重新配置的 target 的期望投影」使用 `ClaimSource::Present`，遇到没有注册的 active 版本就拒绝。
这条记录属于 gcc，与 llvm 的事务无关；它本来就是坏的（`c++` 的 shim 无处可派发），llvm 的替换只是第一个读到它。
xlings 的 doctor 已经认得它（`xvm-active-version-missing`），`--fix` 也有处理悬空引用的代码（`repair_state_`），
但安装路径没有用同一个函数。结果是一个与本次操作无关的旧问题把这次操作永久卡住。

### F4 诊断命令改变状态且掩盖失败

`mcpp self doctor` 的 runtime capabilities 检查调用完整的 `prepare_build`，因此会安装与替换；它把准备失败
一律说成「不在包里」。toolchain 一节用 `mcpp::toolchain::detect()` 检查 PATH 上的编译器，不是 mcpp 为当前
上下文解析的工具链，所以在工具链坏掉时报 `ok`。

### F5 CI 看不见升级

mcpp 的 fresh-install 与 e2e 每次新建 `MCPP_HOME`，从当前索引安装。`916_a_home_from_before_keeps_working`
手写一个只有假 mcpplibs 包的 home，不含工具链、不用上一版的 xlings、不跑 `mcpp new && mcpp run`；10-16 设计
§15 写的「2026.10.11.1 建立的 home」在落地时变成了伪造的 home。xlings 的 `install_revision_test.sh` 是干净 home
中单个 hook-only 包，没有多个包共享一个 target，也没有不一致的记录。

### F6 用户 home 的污染

1827 个条目与 doctor 中数百条悬空链接指向 `/tmp/claude-1000/fakehome/.mcpp/registry/...`，即曾有以假 HOME
运行的测试进程写入了真实的 `~/.mcpp/registry`。来源进程未定位。产品侧的要求由 F3 的自愈覆盖（坏记录不再阻止
操作）；这台机器的 home 另行修复，不属于本设计的代码改动。

## 4. 原则

- **P1 使用不变更。** build、run、test、pack、doctor 等使用命令不替换一个可用的载荷。可用 = 完整，且安装的修订号
  不低于索引条目声明的 `floor`。
- **P2 身份与可用性分离。** 载荷的身份仍是 `(namespace, name, version, revision)`；构建记录的是**实际使用的**
  身份。索引的当前修订号决定新安装得到什么，不决定已安装的是否可用。
- **P3 替换是事务，失败不降级。** 替换只发生在显式的供给命令中，或可用性要求它时；失败后旧载荷仍可用就继续用。
- **P4 版本库是 home 的权威。** 工作区中引用不存在注册的记录是垃圾；任何元数据事务开始时以 doctor 的同一个函数
  规范化，并说出做了什么。一个事务只能被它自己的问题阻止。
- **P5 诊断只读且如实。** doctor 不安装、不替换；它报告 mcpp 实际会用的东西，失败就说失败。
- **P6 升级由真实的上一发布版验证。** 不伪造旧 home。

## 5. 决策

| # | 决策 | 理由 |
|---|---|---|
| D1 | 提供者契约分开报告完整性与时效：`payload_status` 与 `install_targets` 的每个载荷给出 `complete`（只表示完整性）、`revision`（索引当前）、`installed_revision`、`floor`、`usable`（`complete` 且 `installed_revision ≥ floor`；未记录修订号的载荷按可用）、`current`（`installed_revision == revision`）。xlings 以特性 `payload_currency` 宣告；mcpp 的 `unmet()` 要求它 | F1：一个字段承载两件事，调用方就无法区分「旧」与「坏」。判定由提供者给出，mcpp 不复刻版本文法与修订号规则（10-16 设计的端口边界） |
| D2 | 索引的版本条目可声明 `floor`（最低可用修订号，`0 ≤ floor ≤ revision`，缺省 0）。修订号上调时由描述文件作者决定已装的旧修订是否仍可用；现有条目不声明，即全部可用 | 只有作者知道一次修订是「为 Android 去掉硬链接」还是「所有人都会踩的缺陷」。§2 实测本次的 llvm、gcc、glibc 修订对已装载荷都不要求替换 |
| D3 | mcpp 的载荷解析分两种意图：**使用**（build/run/test/pack/doctor、运行库 glibc 的 `ensure_declared_runtime`、宿主工具引导）与**供给**（`mcpp toolchain install`）。使用：`usable` 即用；不可用（缺失、不完整、低于 floor）才安装或替换，并以状态行说明原因（`Updating llvm@23.1.3  packaging revision 0 → 1 (below the index's floor 1)`）。供给：`usable && !current` 时替换为当前修订（`Updating … packaging revision 0 → 1`），`current` 时说已安装 | P1、P3。用户显式要求的才是升级；`mcpp toolchain install <spec>` 本来就是「让这个工具链就绪」的命令，不新增动词 |
| D4 | 库依赖（mcpplibs 等，#524 A）保持「描述文件与载荷同修订」：修订号不同按未安装处理，在构建中替换。它们获得 D5 的失败语义与 X2 的自愈 | 库依赖的描述文件在构建时被读（链接面、flags，#524 的实例），载荷必须与之一致；工具链与运行库的构建输入是载荷本身及其安装时的闭包，不读描述文件。两者语义不同，各自正确 |
| D5 | 替换失败：提供者的替换是事务，原载荷保留。只有供给意图会替换一个仍 usable 的载荷，它失败时报错，并说明仍在使用的修订号；使用意图只替换不 usable 的载荷，失败时没有可退回的载荷，报错。（修订 1 写的「使用意图继续用旧载荷」在使用意图下不会出现：使用意图对 usable 的载荷不发起替换。）提示由错误码决定：确定性的拒绝（`E_INTERNAL`、`E_PERMISSION`、`E_DISK_FULL`、`E_CANCELLED`）不说 retry、不经命令行重试、不刷新索引；只有 `E_NOT_FOUND`（以及没有错误码的旧路径）触发失败后的索引刷新，理由文本为「could not be installed; the index may predate it」 | F2。诊断分级：使用意图下为命令失败并给出原因，供给意图下为命令失败并说明仍在使用的修订号 |
| D6 | 时效的呈现不在构建中：`mcpp index update` 结束时列出非当前的已装载荷，`mcpp toolchain list` 标出 `packaging revision 1 available`（JSON 为 `installedRevision`/`availableRevision`），doctor 同样报告；构建中的自动刷新只在 verbose 日志中记录 | 无感：构建输出不随索引变化而变化；用户在显式改变索引的那一刻、在查看工具链时看到它 |
| D7 | mcpp doctor 只读且如实：runtime capabilities 以不供给的意图准备（解析失败即报告错误与原因）；toolchain 一节报告 mcpp 为当前上下文解析的工具链（项目 → 配置默认），附载荷状态（`usable/current`、两个修订号）与 xlings home 的一致性摘要（悬空记录数，来自 X3）；PATH 上的编译器不再作为「toolchain」报告 | F4、P5 |
| D8 | 构建身份记录实际使用的修订号：运行时契约（`package-identity` §10.2 的 `revision=<n>`）本来就读载荷的安装记录；工具链的驱动身份在修订号大于 0 时追加 `payload-revision: <n>`（同样读安装记录），因而进入指纹与缓存键 | P2：同一项目在两个修订的载荷上构建，产物与缓存不得混用。代价：修订号大于 0 的工具链在升级后重建一次缓存，CHANGELOG 写明 |
| D9 | CI 增加真实升级 lane，§7 | P6、F5 |

开放问题（维护者选择，不是推迟的范围）：

- **O1** 悬空 active 的处理（X2）：推荐「解除该 target 的 active，并报告 `c++: active version 16.1.0 had no
  registration; deactivated (xlings use c++ <version>)`」；另一选项是改选该 target 另一个已注册的版本。推荐前者，
  因为改选会悄悄改变用户 shell 中 `c++` 指向的编译器。
- **O2** `floor` 放在索引（推荐，D2）还是 mcpp 内置表。放在 mcpp 只能表达「mcpp 需要」，不能表达「这个包的这次
  修订修了所有人的缺陷」；前者在需要时也可以由 mcpp 对索引提出。

## 6. 生态的需求

libxpkg（recipe 的解析者，`PlatformResource::revision` 在此）：

- **L1** 版本条目解析 `floor`（非负整数，高于 `revision` 时按 `revision` 读）；`ResolvedResource` 随 `ref` 报告解析到的条目的
  `floor`；版本 0.0.65。

xim-pkgindex：

- **I1** V2 规范为版本条目增加 `floor`；`.github/scripts/check-revision.lua` 校验 `0 ≤ floor ≤ revision`、位置
  （版本项上，不在 alias 与按架构的表内），`floor` 不属于资源。现有条目不需要改动。（修订 1 曾要求「不改
  `revision` 不得提高 `floor`」；实施时删去：作者事后才确认某个已发布修订修复了所有已装载荷的缺陷时，把 `floor`
  提到这个修订是正当的。）

xlings（一次发布，mcpp 的 PR 把内置 xlings 钉到它）：

- **X1** `payload_status`、`install_targets` 按 D1 报告；特性 `payload_currency`；协议版本上调。xlings 的安装计划
  不变：被请求的目标与计划中的依赖都按 `current` 替换。一次安装是供给动作，新载荷是对着索引当前的闭包写成的，
  它的依赖必须同为当前修订（llvm revision 1 的 cfg 指向 glibc revision 4 的 `lib64`，配上 revision 3 的 glibc
  就不自洽）。使用路径不受影响：按 P1，使用意图对一个 usable 的载荷根本不发起安装。（修订 1 曾写「依赖按
  usable 判断」，实施前的审读发现上述不自洽，改为此条。）
- **X2** 元数据事务（`process_xvm_operations_`、`publish_removal_state_`）在本包的注册读取作用域之前，以 doctor 的同一个
  函数（`xvm::plan_scope_reference_repair`）规范化候选工作区：引用不存在注册的 `installed[]` 条目删除，悬空 active 按 O1
  解除；本包自己的注册恢复了的记录不算修复。规范化随事务提交，提交后以 `scope_repaired` 事件报告（mcpp 原样显示）；
  其他 subos 的投影用规范化后的副本计算，不写回。投影预检本身不变：规范化之后，悬空记录不再进入预检。
- **X3** doctor：当前 subos 的 `--fix` 调用 X2 的同一个函数（含 `installed[]`）；`xvm-active-version-missing` 与
  `xvm-subos-active-missing` 的 remedy 不再把 target 当包名，改为 `xlings self doctor --fix` 或 `xlings use <target>
  <version>`（注册已缺失，拥有者无从得知）；`system_status` 输出 `binding_state` 摘要（特性 `binding_state`），供 mcpp
  doctor 读取。
- **X4** 测试：两个包注册同一 target、删除一方的注册后，另一方的安装与替换成功并报告规范化；`payload_status`
  的六个字段与 `floor` 的组合；依赖中的旧修订不被替换。新测试在 2026.10.16.2 上必须失败。

## 7. 测试

mcpp：

- **单元（provider 端口的假实现）**：`(complete, usable, current, 意图)` → 动作的完整矩阵（用、替换、安装、报错、
  降级继续）；替换失败且旧载荷 usable 时使用意图继续、供给意图报错；错误码 → 提示与是否刷新的对应。
- **e2e（本地夹具，离线）**：以本地索引夹具与载荷记录，断言旧修订的工具链被使用、构建无下载、`mcpp toolchain
  list` 标出可用的新修订、`mcpp toolchain install` 替换它；doctor 在项目中不发起任何安装（以提供者调用日志断言）。
- **CI lane `ci-upgrade.yml`**（Linux x86_64、aarch64、macOS、Windows）：上一发布版（从 GitHub release 取，版本由
  `mcpp-release.json` 与本仓库版本推出，每次发布自动前移）建立 home、装工具链、`mcpp new && mcpp run`；换成本 PR
  构建的 mcpp，断言 `mcpp new && mcpp run` 成功、零下载、无替换。Linux x86_64 另跑 snapshot（旧修订载荷）与
  orphan（悬空记录）两个变体：前者断言零下载且 `index update` 报告非当前载荷，后者断言成功并出现规范化报告。
- 新增测试在 2026.10.16.3 上必须失败（snapshot 与 orphan 变体已由 probe 证明会失败）。
- `916` 保留为 index 迁移的 e2e，其注释不再声称覆盖「旧 home 继续可用」；升级由上面的 lane 覆盖。

## 8. 规范与文档

- `docs/specs/package-identity.md`：§10.2 改写 mcpp 侧对修订号的三处使用（工具链与运行库按 usable，库依赖按
  一致，契约记录实际使用的修订号）；§10.3 第 3 条把「修订号与索引条目一致」移出完整性，加入 usable 与 current
  的定义；变更记录与版本。
- `docs/specs/toolchain-management.md`：`mcpp toolchain install` 对已装旧修订的语义；`toolchain list` 的时效列。
- `docs/` 与 `docs/zh/` 中工具链管理与 doctor 的章节：升级、修订号与 `floor` 的用户可见行为。

## 9. 提交顺序（一个 PR 内）

1. 内置 xlings 钉到 X1–X4 的发布；`unmet()` 要求 `payload_currency`；provider 端口的 `PayloadStatus` 增加 D1 字段。
2. 解析意图（D3）与失败语义（D5）：`resolve_xpkg_path` 的意图参数，使用与供给的调用点逐一标明；
   `ensure_declared_runtime` 与宿主工具引导走使用意图；单元测试。
3. D8：工具链指纹与运行时契约取 `installed_revision`。
4. D6：`index update` 的非当前载荷报告、`toolchain list` 的时效列。
5. D7：doctor 只读与如实；读取 X3 的一致性摘要。
6. 失败后刷新的理由与条件（D5）。
7. e2e 与 `ci-upgrade.yml`；规范、双语文档、CHANGELOG。

## 10. 这台机器

用户的 home 需要单独修复，不依赖本设计落地：它的版本库与工作区有 529 条不一致，且有 1827 个条目指向不存在的
假 HOME。probe 实测「重新配置悬空记录的拥有者」能修复 `c++@16.1.0` 这一条；整体上更稳妥的是在备份后重建
`~/.mcpp/registry`。具体操作由维护者决定。

## 11. 实施计划与依赖

```
L1 libxpkg floor ──► X1 payload_status/install_targets 六字段 ──┐
I1 规范与 check-revision（并行）                                ├─► xlings 发布 2026.10.17.1 ──► M1 钉住与 unmet()
X2 事务规范化（plan_scope_reference_repair）──┬─► X4 测试 ──────┘                          │
X3 doctor 共用函数、remedy、system_status 的 binding_state ─┘                              ▼
                                         M2 意图与失败语义（D3/D5）──► M3 D8 ──► M4 D6 ──► M5 D7 ──► M6 测试与 ci-upgrade
                                                                                                   │
                                                                         规范、双语文档、CHANGELOG ◄┘
```

| 任务 | 仓库 | 内容 | 依赖 | 验证 |
|---|---|---|---|---|
| L1 | libxpkg | `floor` 的解析与随 `ref` 解析；夹具与单元测试；0.0.65 | — | `mcpp test` |
| I1 | xim-pkgindex | V2 规范的 `floor`；`check-revision.lua` 的范围与「不改 revision 不得抬 floor」；双语文档 | — | 本地对 `main` 跑检查脚本 |
| X1 | xlings | `payload_state_` 的 `complete` 只表示完整性；六字段；`payload_currency`；协议 1.11 | L1 | 单元与 e2e |
| X2 | xlings | 纯函数 `plan_scope_reference_repair`；`process_xvm_operations_`、`publish_removal_state_` 先规范化候选工作区并报告（`scope_repaired` 数据事件与日志）；其他 subos 的投影用规范化后的副本 | — | 单元、e2e（两包共享 target） |
| X3 | xlings | doctor 的当前 subos 修复调用同一纯函数（含 `installed[]`）；`xvm-active-version-missing` 的 remedy；`system_status` 输出 `binding_state` 摘要，特性 `binding_state` | X2 | 单元 |
| X4 | xlings | e2e：orphan 形状下替换成功并报告规范化；`payload_status` 字段与 `floor` 组合 | X1–X3 | 在 2026.10.16.2 上失败 |
| M1 | mcpp | 内置 xlings 2026.10.17.1；`unmet()` 要求 `payload_currency`；`PayloadStatus` 增字段 | xlings 发布 | 单元 |
| M2 | mcpp | `PayloadIntent{Locate, Use, Provision}`；调用点逐一标明；失败语义按错误码；失败后刷新只对 `E_NOT_FOUND` | M1 | 单元（假提供者） |
| M3 | mcpp | 工具链 `driverIdent` 记录载荷的 `installed_revision`（>0 时） | M2 | 单元 |
| M4 | mcpp | `index update` 结束时报告非当前载荷；`toolchain list` 的时效标注 | M1 | e2e |
| M5 | mcpp | doctor 只读：runtime capabilities 以 Locate 准备；toolchain 一节报告解析到的工具链与载荷状态、home 一致性摘要 | M2 | e2e（调用日志无安装） |
| M6 | mcpp | e2e（离线夹具）；`ci-upgrade.yml`（4 平台 + snapshot/orphan）；916 注释 | M1–M5 | CI；在 2026.10.16.3 上失败 |
