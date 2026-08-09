# xlings × mcpp 生态契约收敛与优化设计

> 状态：Review Draft
>
> 日期：2026-08-09
>
> 基线：mcpp <code>main@80291ca01a98</code>；xlings 最新发布 <code>2026.8.9.2</code>；mcpp 发布包当前内置 xlings <code>2026.8.8.1</code>。#397 的跨仓证据快照为 mcpp-index <code>b86fc7c</code>、xim-pkgindex <code>c0aded29</code>；2026-08-09 复核的远端 main 分别为 <code>b86fc7c</code>、<code>e802938</code>，设计不依赖本地旧 checkout。
>
> 范围：设计与迁移方案，不代表本文所列实现已经完成。
>
> 关联问题：[#397（C1–C9 汇总）](https://github.com/mcpp-community/mcpp/issues/397)、[#396](https://github.com/mcpp-community/mcpp/issues/396)、[#392](https://github.com/mcpp-community/mcpp/issues/392)、[#380](https://github.com/mcpp-community/mcpp/issues/380)。

## 1. 结论先行

这些问题不是九个独立 bug，也不是“再补一轮 if”能稳定解决的问题。它们共同暴露了五类边界不清：

1. **身份边界不清**：<code>namespace:name</code> 在 CLI、模板、索引、渲染器之间被拆掉或退化为裸 <code>name</code>。
2. **状态边界不清**：构建缓存只记“输入看起来没变”，却没有声明一次成功必须留下哪些输出与证明。
3. **运行时边界不清**：xlings、SubOS、索引和 mcpp 都能推断或改写运行时环境，却没有统一、可持久化、可验证的运行时契约。
4. **策略边界不清**：用户构建时的物理可运行性、索引仓库的生态闭包策略、GPU/宿主探测混在同一层。
5. **发布边界不清**：GitHub Release 是事件源，AUR/Homebrew 等下游却按“一次事件必成功”实现，缺少最终一致性对账。

建议采用 **分层、类型化、可验证契约**，而不是把 xlings 扩成一个全知守护进程：

- **xlings** 是环境与运行时 substrate：安装、版本选择、payload provenance、SubOS、RuntimeBinding、环境操作语义。
- **xim-pkgindex** 是系统级 payload 与宿主桥接策略：glibc、Mesa、NVIDIA/WSL sentinel、图形运行时闭包。
- **mcpp-index** 是 C/C++ 包、模板和链接元数据：模块、头文件、link plan、GUI 模板与平台依赖声明。
- **mcpp** 是项目规划器与产物验证器：构建图、缓存后置条件、模板事务、实际二进制物理检查、可解释诊断。
- **发布流水线** 是投影对账器：以不可变 Release Manifest 为源，将 AUR 等渠道收敛到期望状态。

推荐方案的核心不是“增加更多配置”，而是减少重复推断：每个事实只有一个所有者，其他组件消费版本化契约。

## 2. 目标与非目标

### 2.1 目标

- 将 C1–C9、#380、#392、#396 收敛到少量可复用的根契约。
- 让 <code>mcpp new</code>、<code>mcpp add</code>、索引和生成后的清单使用同一套包身份。
- 让构建“成功”同时意味着必要产物、元数据和运行时证明完整。
- 让私有 glibc、宿主库、图形运行时的来源和兼容性可计算、可解释、可缓存。
- 支持 Linux、macOS、Windows 的真实平台差异，不制造虚假对称。
- 让发布渠道从一次性推送转成幂等对账，外部服务恢复后可自动收敛。
- 保持无变化构建快速，不在构建热路径启动 xlings 或访问网络。

### 2.2 非目标

- 不在 mcpp 中实现 GPU 驱动管理。
- 不要求每个 GUI 测试都在普通共享 runner 上打开真实窗口。
- 不用 xlings daemon 替代 mcpp 的项目图规划。
- 不把索引闭包策略变成用户本地每次构建的强制联网检查。
- 不以一个跨四仓库的巨型 PR 交付。
- 本文不直接关闭 issues、重跑 AUR 工作流或发布新版本。

## 3. 当前事实与根因

### 3.1 C1–C9 不是同一优先级，但可以归并

| 编号 | 当前现象 | 直接根因 | 应归入的长期契约 | 优先级 |
|---|---|---|---|---|
| C1 | fast path 在 <code>compile_commands.json</code> 等配置产物缺失时仍可直接进入 Ninja，编辑器反复请求；普通 executable 缺失通常仍由 Ninja 重建 | 缓存没有声明配置成功的后置条件 | BuildSnapshot + OutputManifest | P1 |
| C2 | 注释中的 <code>R"(</code>、普通字符串中的注释记号会污染模块扫描状态 | 先做脆弱的行级 strip，再解析 token | 线性词法 masker | P1 |
| C3 | TTY 下 <code>--no-color</code> 无效 | color 状态延迟初始化覆盖显式参数 | 一次初始化的 UiPolicy | P2 |
| C4 | namespaced 包 exports 非 strict 时漏检，strict 时会把合法 exports 误报 missing | qualified owner 与短 manifest name 做字符串比较 | PackageId 类型贯穿 | P1 |
| C5 | Windows 版本探测含 POSIX <code>2>/dev/null</code> | 命令以 shell 字符串表达 | CommandSpec + direct exec | P1 |
| C6 | 部分安装形态下 release 内置 xlings 不进入候选更新链 | 候选只按位置推断，没有 provenance/capability | XlingsCandidate 选择策略 | P1/P2 |
| C7 | xlings 端已按 presence 语义修复，但 mcpp 仍把 <code>op=set</code> 无条件覆盖 | reader 与当前 wire 语义漂移，且缺少显式 replace | EnvOp schema v2 | P1 |
| C8 | 两段无版本安装循环在进入 fetch 前即失败并吞掉错误；当前用户可达影响尚未证明 | 多个安装 owner、错误被忽略 | 单一 solver/installer owner | P2 |
| C9 | 路径、源输出、glob、脚手架、链接、版本、canary、CI、机器输出等 11 个长期项 | 同一事实多处生产、字符串协议与 fail-open | 分别映射到下表所列基础契约 | P1–P3 |

C9 的 11 项应明确归属，而不是留作“杂项”：

| C9 项 | 归属 |
|---|---|
| Windows 路径与 action 规范化（#393） | typed Path + CommandSpec |
| generated source output 被静默排除（#393） | OutputManifest + action postcondition |
| 默认 source glob 漂移（#386） | 单一 SourceSet producer |
| 脚手架安全与卡死（#380） | NameAtom + 单次渲染 + 事务目录 |
| runtime link order / macOS runtime dir（#304） | LinkPlan token + 平台 capability |
| prerelease 依赖解析（#370） | 单一 VersionReq parser |
| cfg version 被忽略（#290） | 封闭条件词汇；未知条件 hard error |
| operator-template canary 只 precompile、不 import（#256） | 真实 importer + crash/unsupported/fail 三态 |
| cppfly resolver 后续候选不可达、canary 可自我跳过（#215） | 遍历全部候选 + fail-closed capability gate |
| fresh install 被缓存掩盖（#259） | 冷 HOME 独立 gate |
| machine output 不一致（#379） | UiPolicy + WireEnvelope |

其中 #386 的“四项 fallback glob 与七项 canonical 默认不一致”是已证实的代码漂移，但正常 xpkg 装载路径已补默认并要求 sources 非空；当前用户可达性尚未建立，应作为 P3 清理而不是阻塞前两阶段。

### 3.2 #380：输入验证只是第一层，真正缺的是脚手架事务

当前 <code>mcpp new</code> 可接受路径穿越、控制字符和会重新引入占位符的名称；builtin renderer 对含 <code>PROJECT</code> 的替换值可静态证明无限循环，package-template renderer 虽推进游标，不会以同一种方式无限循环，但仍会发生占位符相互消费。两条路径都有 I/O 错误被忽略和半成品风险。修复不能只加一个正则：

- 项目逻辑名必须是一个 <code>NameAtom</code>，与目标目录 <code>--dir</code> 分离。
- 新项目默认执行 portable policy：拒绝绝对路径、<code>.</code>/<code>..</code>、任一平台路径分隔符、NUL/控制字符、Windows 保留设备名及尾随点/空格；已有 legacy manifest 继续可读，不借此批量改写用户身份。
- namespace 是结构化字段，不允许塞回项目名或模板字符串。
- 渲染必须单次完成；未知占位符、重复键、非法 TOML/C++ 标识均 hard error。
- 先写同父目录临时目录，验证生成清单与必需文件，再原子 rename。
- 任一步失败清理临时目录；目标已存在默认不覆盖。

### 3.3 #392 与 #396：问题是“实际装载物理”，不是版本号猜测

#392 展示了私有 glibc 与宿主 <code>libtinfo</code>/Mesa 混装时的两个失败方向：

- 私有 loader/libc 较旧，宿主库要求更高 GLIBC symbol version。
- 通过全局 <code>LD_LIBRARY_PATH</code> 暴露私有 glibc，又让外部宿主命令加载到错误 libc/私有符号。

当前 main 已有隔离私有 glibc 环境的局部缓解，因此不能表述成“完全未修”。但仅靠目录过滤仍不能证明最终 ELF 的实际闭包可运行。

另一个独立的不确定性是 post-install 的 sandbox glibc 查找：注释声称选择 newest，代码实际返回 <code>directory_iterator</code> 的第一个命中；该顺序未规定，不能描述为“字典序第一”。目标设计必须按完整 PackageId/version/RuntimeBinding 选择，不从目录枚举顺序推断。

#396 中应保留两条物理不变量，但修正 Rule A 的计算方式：

- **Rule B — 单一 RuntimeBinding**：实际 <code>PT_INTERP</code> 与最终解析到的 libc 必须来自同一个 payload/runtime binding。
- **Rule A — symbol ceiling**：对每一个实际借用的宿主 ELF，对其 <code>.gnu.version_r</code> 所需的最高 GLIBC 版本逐对象检查，不用粗糙的“私有 glibc 版本 ≥ host glibc 版本”替代。

生态策略另行定义：

- **Policy D — 生态闭包**：索引包的运行时依赖应由生态包闭包满足；宿主对象只允许出现在显式、类型化的 <code>host-link sentinel</code> 后。
- D 在索引 CI/安装物化时强制。用户构建的最终目标是对**已证明**的 A/B 冲突 hard fail，并清晰展示 provenance；在精确闭包解析器积累真机证据前，按 observe → warning → strict opt-in → 默认 strict 分阶段，不能拿粗略的 host glibc 代理在第一天全局 hard fail。

这一区分很重要：A/B 是物理事实，D 是仓库治理策略。

### 3.4 模板 namespace：现有语法先天歧义

当前 <code>--template</code> 解析 <code>pkg@ver:template</code>，第一个冒号被当作 template 分隔符。因此 <code>ocornut:imgui</code> 会被理解为 package=<code>ocornut</code>、template=<code>imgui</code>，无法表达规范身份。scaffold resolver 又只尝试空 namespace 与 <code>compat</code>，没有复用依赖路径的 IndexRoute；裸 <code>imgui</code> 会先命中冻结的 <code>mcpplibs:imgui</code>，而不是新的 <code>ocornut:imgui</code>，也不会报告跨 namespace 候选歧义。

同时，模板获取阶段即使从 descriptor 得到 namespace，返回值和渲染变量仍只保留裸 name；依赖注入也用字符串搜索和短名。这会重新引入此前包索引已经修复的同名碰撞。

正确方向是复用 <code>mcpp add</code> 已有身份语法，而不是再发明第三套压缩分隔符：

~~~text
mcpp new app --namespace acme \
  --template ocornut:imgui@1.92.8 \
  --variant glfw-opengl3 \
  --index official

mcpp new app --template mcpplibs:templates@0.0.1
mcpp new --list-templates ocornut:imgui@1.92.8
~~~

- <code>--template</code>：规范 PackageRef，即 <code>namespace:name@version</code>。
- <code>--variant</code>：该 provider 内的模板变体。
- <code>--namespace</code>：新项目自身 namespace。
- <code>--dir</code>：文件系统目的地。
- <code>--index</code>：路由来源；index alias/path/URL 不属于 PackageId。
- 旧 <code>pkg@ver:template</code> 只作为 deprecated ingress，解析后立即规范化并告警。
- 对 <code>pkg:word</code>，新语义“namespace:name”与旧语义“pkg 的 variant”可能同时成立：只在一侧候选唯一时兼容；两侧都存在或无法排除时返回稳定的 ambiguity error，并给出可复制的显式 <code>--template</code>/<code>--variant</code> 写法。
- builtin <code>bin</code> 可保留兼容 alias，但内部也使用明确的 builtin provider identity。

可选的紧凑糖 <code>namespace:name@version#variant</code> 没有冒号歧义，但建议在显式 flags 与 wire model 稳定后再决定；内部始终是 TemplateSelection，不让紧凑语法成为第三种身份。

### 3.5 图形程序：当前已有正确积木，但责任仍有重复

当前生态已经包含：

- mcpp-index 的 <code>ocornut:imgui</code>、<code>compat.glfw</code>、<code>compat.glx-runtime</code>。
- xim-pkgindex 的 <code>xim:graphics</code>、Mesa，以及 NVIDIA/WSL host-link sentinels。
- Linux Mesa 的软件和多类硬件 backend；macOS native frameworks；Windows Win32/system SDK 路径。

问题在于 <code>compat.glx-runtime</code> 仍需要从 SubOS view 选择并 symlink GL 库到自己的 runtime 目录。这是过渡桥，而不是最终模型。最终应让 xlings 持久化可传递 runtime exports，mcpp 直接消费 resolved runtime contract；mcpp-index 只声明依赖，不复制 xlings 的视图布局。

Vulkan 还存在更明显的旁路：<code>compat.vulkan-runtime</code> 仍从 host 的 <code>/lib*</code>/<code>/usr/lib*</code> 收集 ICD 和 transitive DSOs，测试主要覆盖 loader API，没有证明真实 ICD/device、来源或 GLIBC 闭包。GL 的收敛方案必须同时覆盖 Vulkan loader、ICD manifest 与 driver；否则只是把同类风险移到另一个 API。

### 3.6 AUR：外部维护是触发原因，缺少对账才是长期漂移原因

2026-08-09 的只读审计显示：

- GitHub 最新 release 是 [<code>v2026.8.8.4</code>](https://github.com/mcpp-community/mcpp/releases/tag/v2026.8.8.4)，AUR RPC 中 <code>mcpp-bin</code> 与 <code>mcpp-m</code> 均停在 <code>2026.8.1.1-1</code>，相差 20 个正式 release。
- 最后成功的 aur-publish run 是 [<code>30649165652</code>](https://github.com/mcpp-community/mcpp/actions/runs/30649165652)；从 [<code>30718043364</code>](https://github.com/mcpp-community/mcpp/actions/runs/30718043364) 起到 2026-08-08，共核对到 18 个同类失败、4 个因 release gate skipped；最新失败是 [<code>31254088758</code>](https://github.com/mcpp-community/mcpp/actions/runs/31254088758)。
- 失败日志均已完成版本与 sha 刷新，随后在 AUR SSH 阶段收到 <code>The AUR is down due to maintenance</code> 并退出 128。
- 当前 workflow 只有 release 完成事件与手工触发；没有 retry、schedule/reconcile、推送后验证。

还存在一个必须先修的独立 P0：

- <code>scripts/aur/update.sh:65</code> 只匹配单行 <code>sha256sums=('...')</code>。
- <code>scripts/aur/mcpp-m/PKGBUILD:34-35</code> 是两行数组，所以 source checksum 没有被更新。
- workflow 生成的 <code>.SRCINFO</code> 使用新 checksum，而 PKGBUILD 仍保留旧 checksum，形成同一提交内部不一致。
- 当前顺序先推 <code>mcpp-bin</code> 再推 <code>mcpp-m</code>；AUR 恢复后直接重跑可能先产生部分成功，再尝试推送坏的 mcpp-m 元数据。

因此不能把“重跑工作流”作为修复。应先增加本地结构化更新与一致性 gate，再恢复发布。

### 3.7 多视角评估

| 视角 | 当前风险 | 评价 | 目标变化 |
|---|---|---|---|
| 架构 | 高 | xlings、probe、post-install、build plan、run env 多次推导 runtime truth；scaffold 又绕开 canonical resolver | 单一 owner + versioned contract + typed snapshot |
| 稳定性 | 高 | fail-open、忽略 I/O/error_code、配置副作用未入 manifest；外部发布一次失败即永久漂移 | transaction/postcondition/reconciler |
| 兼容性 | 高 | GLIBC 风险取决于实际 DSO；namespace、EnvOp、runtime dirs 在读写端语义漂移 | exact artifact proof + schema negotiation |
| 优雅简洁 | 中高 | CLI 表面不算大，但内部靠短名、目录顺序、字符串命令和目录复制造成隐性复杂度 | 保持少量 CLI，内部用 PackageRef/CommandSpec/LinkIntent |
| 用户体验 | 高 | 成功可能留下半状态；错误只报 GLIBC/包不存在，无法说明来源；机器输出不稳定 | 原子操作、canonical ref、<code>why</code> provenance、稳定 envelope |
| 性能 | 中 | 现有 fast path 很快，但把完整性排除在“命中”之外；runtime/版本探测可能重复 | 本地 hash snapshot、按产物增量验证、无热路径 xlings/network |
| 多平台 | 高 | POSIX shell 片段泄漏到 Windows；Mach-O runtime dirs 与 Linux ELF 语义混用；硬件结论常由 headless/cross build 代替 | LinkIntent 平台 lowering + native cold-home/hardware evidence |

这里的目标不是用“更严格”换“更慢”。类型化契约让昂贵解析只发生在安装、configure 或 changed artifact 上，hot no-op 只校验 fingerprint 与 required outputs。

## 4. 三种架构选择

| 方案 | 架构 | 稳定性 | 兼容性 | 简洁性 | 性能 | 多平台 | 结论 |
|---|---|---|---|---|---|---|---|
| A. 按 issue 打补丁 | 局部修改快，但同一事实继续多处推断 | 短期变绿，回归概率高 | 表面影响小，长期漂移大 | 初看简单，维护复杂 | 可维持当前热路径 | 平台分支继续散落 | 只用于 P0 止血 |
| B. xlings 全知 daemon | 所有解析/探测集中 | daemon 生命周期与状态成为新故障域 | 强耦合 xlings 版本 | 接口表面少，系统更重 | 构建热路径多进程/IPC | Windows/macOS 服务语义复杂 | 不采用 |
| C. 分层 typed contracts | 每个事实单一 owner，边界清楚 | 可校验、可缓存、可回滚 | 支持 schema/legacy ingress | 数据模型略增，重复逻辑显著减少 | 构建只读本地契约 | 原生 provider 表达差异 | **推荐** |

方案 C 可以允许 P0 局部修复先落地，但所有 P0 修复都应朝目标契约收敛，不能制造第二套临时协议。

## 5. 目标架构

~~~mermaid
flowchart LR
    XI[xlings<br/>install / SubOS / RuntimeBinding] --> RC[Runtime & Install Contract]
    XP[xim-pkgindex<br/>system payload / host sentinel] --> XI
    MP[mcpp-index<br/>C++ package / template / LinkPlan] --> MC[mcpp<br/>planner / builder / inspector]
    RC --> MC
    MC --> OM[BuildSnapshot / OutputManifest]
    MC --> ELF[Artifact Physics Verdict]
    GH[Immutable GitHub Release Manifest] --> REC[Channel Reconciler]
    REC --> AUR[AUR mcpp-bin / mcpp-m]
    REC --> OTHER[other package channels]
~~~

### 5.1 单一事实所有者

| 事实 | 唯一 owner | 消费者 |
|---|---|---|
| 包 canonical identity、版本与安装 payload | xlings/libxpkg | mcpp、索引工具、模板 |
| runtime binding、loader/libc、exports、env op | xlings + xim package descriptor | mcpp |
| C/C++ source/module/link/template metadata | mcpp-index | mcpp |
| 项目 build graph 与 required outputs | mcpp | IDE、CI、用户 |
| 实际 ELF 闭包与兼容 verdict | mcpp post-link inspector | 用户、CI、缓存 |
| GitHub release 资产与 checksum | Release Manifest | AUR/Homebrew/其他渠道 |
| AUR 当前版本 | AUR | reconciler，仅作为 observed state |

### 5.2 不可违反的不变量

1. **Identity**：内部永远用结构体，不用拼接字符串承担身份。
2. **Success**：返回成功前，声明的 required outputs 必须存在并通过验证。
3. **Runtime**：loader、libc 与解析到的共享库必须能追溯到 provider。
4. **No hot-path orchestration**：构建热路径不启动 xlings、不访问索引网络。
5. **Atomicity**：cache snapshot、模板目录、投影元数据和渠道更新必须原子提交或明确部分失败。
6. **Fail closed on ambiguity**：同名候选、多 payload、未知 cfg/placeholder 不按目录顺序或短名猜。
7. **Native evidence**：Windows/macOS/aarch64 行为只由对应 runner 证明。

## 6. 核心数据契约

### 6.1 Identity types

建议共享概念定义，语言实现可独立：

~~~text
NameAtom         = one validated identifier atom
NamespacePath    = one or more NameAtom segments
PackageId        = { namespace: NamespacePath, name: NameAtom }
PackageRef       = { id: PackageId, versionReq?: VersionReq }
ResolvedPackageId= { id: PackageId, version, indexRoute, descriptorDigest, payloadDigest }
ProjectIdentity  = { namespace: NamespacePath, name: NameAtom, version }
TemplateSelection= { provider: PackageRef, variant?: NameAtom }
~~~

规则：

- wire/display 规范形式是 <code>namespace:name</code>。
- bare name 只能在候选唯一时作为便捷输入；歧义必须列出候选并失败。
- legacy FQN 只在 ingress 解析一次；之后不再拆字符串。
- map/cache key 使用完整 PackageId。
- qualified display 由类型派生，不在各层自行加前缀。
- index route 和 transport provenance 随 ResolvedPackageId 保留用于诊断/lock，但不混进 PackageId 本身。

### 6.2 xlings Runtime & Install Contract v2

xlings 在安装/物化时写入 versioned、只含相对路径的契约。可以演进现有安装描述文件，也可以使用专用文件；名称不是本文的关键决策，schema 是。

建议字段：

~~~json
{
  "schema": 2,
  "package": {"namespace": "xim", "name": "glibc", "version": "2.44"},
  "payload": {
    "id": "content-addressed-id",
    "digest": "sha256:...",
    "root": "."
  },
  "runtimeBinding": {
    "id": "linux-glibc-2.44-x86_64",
    "platform": "linux",
    "arch": "x86_64",
    "loader": "lib/ld-linux-x86-64.so.2",
    "libc": "lib/libc.so.6",
    "glibcProvidedCeiling": "2.44"
  },
  "exports": {
    "includeDirs": [],
    "libraryDirs": ["lib"],
    "runtimeDirs": ["lib"],
    "crtObjects": []
  },
  "runtimeDependencies": [
    {"namespace": "xim", "name": "ncurses", "version": "..."}
  ],
  "environment": [
    {"name": "PATH", "op": "prepend", "value": "bin"}
  ],
  "capabilities": ["runtime.opengl", "runtime.egl"],
  "provenance": "ecosystem",
  "hostObservation": {
    "glibc": "2.43",
    "fingerprint": "optional-host-fingerprint"
  }
}
~~~

约束：

- 所有路径相对 payload root；移动安装根不使契约失效。
- 契约文件有 content hash；mcpp cache key 使用 hash，不使用 mtime 猜测。
- SubOS 只引用 active provider contract id，不复制或重新解释 payload 目录。
- <code>provenance</code> 是封闭枚举：<code>ecosystem</code>、<code>system-sdk</code>、<code>host-link</code>。
- 复用 xlings 现有 <code>host_glibc</code> 观测并带入诊断/host-link fingerprint；mcpp 不再忽略该字段，但它不能替代逐 DSO 的 symbol requirement 检查。
- 旧 schema 可读时告警并转成内存 v2；多 payload/歧义时 fail closed，不再按字典序挑目录。

### 6.3 EnvOp schema v2

废弃歧义的单一 <code>set</code>：

事实边界：xlings 自 <code>2026.8.8.2</code> 起，POSIX/fish/PowerShell/进程内路径已经统一为“变量存在即不覆盖”，包括显式空字符串；剩余缺陷是 mcpp reader 仍把 Set 解释成 replace。schema v2 是为了把这一区别永久写入协议，而不是声称 xlings writer 仍未修。

| op | 精确定义 |
|---|---|
| <code>default</code> | 变量不存在时设置；变量存在但为空也视为已存在 |
| <code>prepend</code> | 按平台路径分隔符前置，去重并保序 |
| <code>replace</code> | 无条件覆盖，必须显式使用 |

兼容规则：

- schema 1 的 <code>set</code> 按 xlings 历史行为映射为 <code>default</code>。
- xlings writer 与 mcpp reader 同一发布窗口支持 v2。
- POSIX shell、fish、PowerShell、xlings 进程内应用和 mcpp 读取共享同一组 golden fixtures。

### 6.4 BuildSnapshot 与 OutputManifest

一次配置成功写入原子、版本化 snapshot：

~~~text
BuildSnapshot
  schema
  inputFingerprint
  graphFile
  sourceSetDigest
  toolchainContractHash
  runtimeContractHashes[]
  outputManifest
  postLinkVerdicts[]

OutputManifest
  ninjaOwnedArtifacts[]
  metadataProjections[]
  generatedSources[]
  requiredPostconditions[]
~~~

fast path 决策：

- Ninja-owned artifact 缺失：交给 Ninja 依据图重建。
- <code>compile_commands.json</code> 等 metadata projection 缺失：从 output-dir 中 fingerprint 对应的 canonical copy 原子重投影，不必完整 prepare。
- graph/snapshot/runtime contract 缺失或 schema 不支持：完整 prepare。
- 任一 required postcondition 缺失：不得输出 “Finished”。
- <code>compile_commands.json</code> 加入新项目默认 ignore；已有项目即使未 ignore 也应正确修复。

### 6.5 UiPolicy 与 WireEnvelope

进程启动时仅初始化一次：

~~~text
UiPolicy {
  color: Auto | Always | Never,
  quiet: bool,
  output: Human | Wire
}
~~~

- <code>--no-color</code> 直接得到 <code>Never</code>，后续 TTY 探测不能覆盖。
- human/status/progress 写 stderr。
- machine stdout 只允许 versioned envelope；无彩色、无 spinner、无说明文字。
- pseudo-TTY、Windows console、管道和重定向分别测试。

### 6.6 CommandSpec

所有子进程用结构化 argv，不拼 shell：

~~~text
CommandSpec {
  executable,
  argv[],
  cwd,
  envDelta,
  stdinPolicy,
  stdoutPolicy,
  stderrPolicy
}
~~~

<code>2>/dev/null</code>、引号、重定向不再进入参数字符串。POSIX 与 Windows 使用各自 native spawn backend，共享上层语义测试。

## 7. 详细设计

### 7.1 xlings 候选发现与升级

当前 release-bundled xlings 被排除在候选更新链之外。改为显式候选：

1. CLI/config 显式覆盖。
2. mcpp release-bundled sibling。
3. distro/AUR 提供的环境候选。
4. mcpp writable sandbox 已安装候选。
5. system PATH。

每个候选携带：

~~~text
XlingsCandidate {
  path,
  version,
  provenance,
  writable,
  contractCapabilities[],
  digest?
}
~~~

选择原则：

- 先满足 required contract capability/schema。
- 默认不降级。
- 在可信候选中选择最高兼容版本，而不是只按目录位置。
- 需要复制到 sandbox 时原子替换并验证 digest/version。
- <code>mcpp self env</code> 显示候选、被选原因、版本和 provenance。

release pin 同时表达两件事：

- 本次 release 原生 CI 验证过的 exact bundled version。
- 运行时允许的 minimum contract capability/schema。

这样可接受未来兼容版本，又不丢失发布可复现性。

### 7.2 构建完整性、C1 和 generated source

将构建分成三个明确阶段：

1. **Plan**：解析 identity、source set、toolchain/runtime contract，生成 graph 与 snapshot。
2. **Execute**：Ninja/runner 生产 artifacts 和 generated sources。
3. **Verify/Project**：检查 OutputManifest、运行 post-link inspector、原子投影 CDB/机器元数据。

generated action 必须声明 outputs；其输出并入下一轮 SourceSet 或明确标为最终产物。没有声明或声明后缺失均失败，不能静默排除。

默认 source glob 只由一个 SourceSet producer 展开；清单、scanner、Ninja graph 不再各自 glob。

性能要求：

- snapshot 校验按 digest/stat 快速路径完成。
- CDB 重投影是文件复制/rename，不重新求解全部依赖。
- no-change build 目标维持 <code>&lt; 0.5 s</code>，或相对当前基线不回退超过 10%；两者取更严格、但先在 CI 固定硬件建立基线。

### 7.3 C++ scanner

用 O(bytes) streaming lexical masker 代替启发式 strip，状态至少包括：

- Normal
- line comment
- block comment
- string / char / escape
- raw string delimiter / raw body
- line splice 与 CRLF 处理

masker 保留换行与列宽，将注释/字符串内容替换为空白；module/import parser 只读取 masked code。注释只可在 Normal 状态开始。

回归 corpus 必含：

- 行注释和块注释中的 <code>R"(</code>。
- 普通字符串中的 <code>/*</code> 与 <code>//</code>。
- raw string 中的假 <code>import</code>。
- char literal、encoding prefix、自定义 raw delimiter。
- CRLF、行拼接、文件末尾未闭合状态。

scanner 保持热路径无编译器子进程；若未来接 P1689，编译器扫描作为权威慢路径/不确定输入 fallback，不与 masker 产生第三套模块身份。

### 7.4 exports 与 namespace

<code>SourceUnit.owner</code>、manifest owner、dependency owner 全部改为 PackageId。校验使用结构体相等；诊断需要字符串时调用一个 formatter。

这个改动同时解决：

- C4 namespaced exports 被跳过。
- namespace 被重复前缀化。
- 模板依赖注入回退短名。
- cache/graph 在同短名包之间碰撞。

### 7.5 脚手架与模板

#### 7.5.1 CLI

推荐稳定表面：

~~~text
mcpp new <project-name>
  [--namespace <namespace>]
  [--dir <path>]
  [--template <namespace:name@version>]
  [--variant <name>]
~~~

交互输出必须同时展示 provider 的 qualified identity 和 variant；<code>--list-templates</code> 的 wire 模式返回结构化数组。

#### 7.5.2 renderer

RenderVars 至少包含：

~~~text
project.name
project.namespace
project.qualifiedName
project.version
template.provider.namespace
template.provider.name
template.provider.qualifiedName
template.variant
~~~

实现要求：

- parse template 成 token stream，一次替换，不扫描插入值。
- 未知 token hard error；需要 literal token 使用明确 escape。
- 文件路径渲染也走同一验证器，禁止绝对路径、<code>..</code> 和目录逃逸。
- 依赖注入操作 TOML AST，不用字符串查找；写出完整 namespace 分组。
- 模板 descriptor 声明兼容的 mcpp contract/version、必需 capability 与平台。

#### 7.5.3 transaction

流程：

~~~text
parse and validate input
  -> resolve exact template provider
  -> materialize to sibling temp dir
  -> render once
  -> parse generated mcpp.toml
  -> validate required files and no path escape
  -> optional offline configure smoke
  -> atomic rename to destination
~~~

任何错误返回非零并删除 temp；诊断给出字段、非法值和允许形式，不留下半项目。

#### 7.5.4 compatibility

- 旧 <code>pkg@ver:variant</code> 支持两个 release train，仅限能唯一解析的无 namespace provider。
- 第一个 release train warning；第二个可通过 compatibility flag 使用；之后删除。
- 模板索引新增 canonical identity，不原地改变旧模板含义。

### 7.6 Artifact Physics Inspector

当前 pre-link driver <code>-###</code> 检查只看到“编译器计划”，不能证明最终 ELF。新增 post-link inspector：

1. 读取实际 <code>PT_INTERP</code>、<code>DT_NEEDED</code>、RPATH/RUNPATH。
2. 根据 artifact 路径、Runtime Contract 和平台规则递归解析动态闭包。
3. 给每个对象标注 payload/provider/provenance。
4. 从 <code>.gnu.version_r</code> 计算每个 host-borrowed object 的 GLIBC requirements。
5. 验证 Rule B 与 Rule A。
6. 生成 versioned verdict，按 artifact digest + Runtime Contract hash 缓存。

可复用 vendored patchelf 获取 interp/rpath/needed；GLIBC version requirement 建议实现最小只读 ELF parser，避免假设用户安装 readelf，也避免仅用系统 glibc 版本近似。

诊断示例信息应包含：

~~~text
artifact
selected loader and provider
selected libc and provider
offending object
required GLIBC symbol ceiling
provided runtime ceiling
resolution path and remediation candidates
~~~

build fast path 必须把 verdict 当作 required output；artifact 或 Runtime Contract hash 变化后失效。

<code>allow_host_libs</code> 可以调整 Policy D 的告警/允许范围，但不能关闭物理 A/B：配置项不能让不可装载的 ELF 变得可运行。

上线分四步：

1. **observe**：只记录闭包与 provider，对照 linker trace/map、readelf/patchelf fixtures。
2. **warning**：对 proven mismatch 报高质量诊断，但保留显式 strict opt-in。
3. **strict opt-in**：在 mcpp/xim-index native matrix 中积累低误报数据。
4. **default strict**：只对精确解析得到的 mismatch/unresolved required object hard fail；解析器自身无法证明的情况返回“inconclusive”，不得假装兼容，也不得用 <code>our_glibc ≥ host_glibc</code> 代理误拒绝。

### 7.7 GUI / graphics 分层

当前 <code>[runtime]</code> 的 soname、capability、provider 和目录大多是扁平字符串：provider 只记 short name，<code>library_dirs</code> 又同时影响 <code>-L</code> 与 RPATH，capability 还可能同时表示 requires 与 provides。目标模型先拆开“需求”和“已解析实物”：

~~~text
RuntimeRequirement {
  kind: soname | capability | icd_manifest | display | host_service,
  value,
  phase: link | run,
  required,
  target,
  requester: PackageId
}

RuntimeArtifact {
  role: loader | library | driver | manifest | host_bridge,
  relativePath,
  provider: ResolvedPackageId,
  provenance: payload | subos_view | host_link | system_sdk,
  abi,
  requiredGlibcCeiling?,
  digest?,
  hostFingerprint?
}
~~~

链接和运行目录也必须分义：

~~~text
link.libraryDirs
link.transitiveNeededDirs
runtime.rpathDirs
runtime.dlopenDirs
runtime.environment
deploy.files
~~~

runtime 目录不得隐式进入 <code>-L</code>。ELF lowering 分别生成 link search、<code>-rpath-link</code> 和 RPATH/RUNPATH；Mach-O 使用 <code>@rpath</code>/install name；PE 使用 app-local DLL 与 system DLL contract。<code>resolution.json</code> 输出 requirement → canonical provider → artifact → search mechanism → provenance → ABI verdict 的完整链。

#### 7.7.1 层次

| 层 | 职责 | 禁止事项 |
|---|---|---|
| xlings | payload、SubOS view、RuntimeBinding、runtime exports、host sentinel 激活 | 不理解 ImGui/GLFW 项目语义 |
| xim-pkgindex | Mesa/系统运行时闭包、NVIDIA/WSL host-link sentinel、平台系统包 | 不生成 mcpp 工程 |
| mcpp-index | ImGui/GLFW 等 C++ 包、LinkPlan、模板、平台依赖选择 | 不复制/symlink xlings 内部 view 作为长期 ABI |
| mcpp | 根据 capability 规划、链接、artifact physics、why 诊断 | 不探测 GPU 型号或选择驱动 |

#### 7.7.2 平台策略

- **Linux**：<code>xim:graphics</code> 作为图形运行时入口；Mesa 默认闭包；NVIDIA/WSL 通过 sentinel 表达显式 host-link。
- **macOS**：使用系统 SDK/framework capability；不伪装成 Linux Mesa 布局。
- **Windows**：使用 Win32/system SDK 与明确 DLL runtime contract；命令执行和路径由 native backend。

模板声明平台依赖，不在源码中散落“如果 Linux 就手工找 libGL”。

ImGui 包本身也应按 feature 解耦：

- <code>core/headless</code> 不拉窗口系统。
- <code>backend-glfw-opengl3</code> 显式引入 GLFW、OpenGL headers 与 runtime。
- <code>backend-vulkan</code> 显式引入 Vulkan loader + ICD requirement。
- <code>app</code> 可组合默认 backend；<code>docking</code>/<code>viewports</code> 是正交 feature。

当前 <code>ocornut:imgui</code> 的 app facade/后端依赖仍容易让只用 core 的项目拉入 graphics 栈，这一拆分应由 mcpp-index 完成，不放进 xlings。

#### 7.7.3 过渡

<code>compat.glx-runtime</code> 的 symlink bridge 暂时保留：

1. xlings contract v2 能表达 resolved transitive runtime exports。
2. mcpp 能直接消费并生成正确 LinkPlan/runtime dirs。
3. mcpp-index native tests 证明不再需要桥。

然后在一个有 warning 的 release train 中弃用，避免双份 runtime dir 漂移。

#### 7.7.4 测试矩阵

| 层级 | Linux | macOS | Windows |
|---|---|---|---|
| 常规 PR | headless llvmpipe configure/build/run；artifact A/B | native framework build + minimal smoke | Win32 backend build + minimal smoke |
| 原生/计划任务 | X11/Wayland；AMD/Intel/NVIDIA；WSL2 | arm64 + x86_64 native window | x64/arm64 native window |
| side-effect | 安装 graphics 前后无关 ELF 的 interpreter 与 GLIBC ceiling 不变 | SDK selection 不污染全局 env | DLL/path contract 不污染宿主 shell |

Vulkan native gate 至少创建 instance、枚举 physical device，并验证实际加载的 ICD manifest/driver provenance；只测试 loader symbol 不算通过。真实 GPU 行为只能由对应 native runner 证明；Linux headless 通过不外推成所有硬件通过。当前 <code>xim:graphics</code> recipe 明确仅支持 Linux x86_64，所以 aarch64 在支持落地前应返回清晰的 capability unavailable，而不是把 cross-build 或 skipped runtime test 汇总成绿色。

### 7.8 安装 owner 与错误传播

删除 C8 的两段 versionless 预安装循环。唯一流程：

~~~text
parse PackageRef
  -> solve exact graph
  -> install/materialize exact nodes through xlings
  -> verify install contracts
  -> expose graph to planner
~~~

- 任何节点失败立即带完整 PackageId/version/provenance 返回。
- 不允许 <code>catch (...) {}</code> 或忽略 error_code 后继续。
- 离线模式只使用已验证 materialization；缺失时明确报错。

### 7.9 Release Manifest 与 AUR reconciler

#### 7.9.1 Release Manifest

release 成功后发布不可变 manifest：

~~~json
{
  "schema": 1,
  "version": "2026.8.8.4",
  "tag": "v2026.8.8.4",
  "commit": "...",
  "assets": [
    {"name": "...", "sha256": "...", "platform": "...", "arch": "..."}
  ],
  "bundled": {"xlings": "2026.8.8.1"}
}
~~~

AUR 生成器只消费 manifest，不重新从日志/文件名猜版本和资产。

<code>mcpp-m</code> 应消费 release 自己发布且带 sidecar 的版本化 source asset，不再依赖可能重生成的 tag archive；生成器下载实物重算 hash，并与 manifest/sidecar 交叉验证。

#### 7.9.2 先修 P0 生成器

- 不再用行级 sed 更新 Bash array；使用小型、确定性生成器从 model 完整生成 PKGBUILD 与 .SRCINFO。
- 在推送前下载/校验 source archive 与二进制资产。
- 比较 PKGBUILD 解析结果与 .SRCINFO：version、source URL、每个 checksum 必须一致。
- 在 Arch container 中运行 <code>makepkg --printsrcinfo</code> 并与提交文件 diff。
- 在 Arch container 中运行 <code>makepkg --verifysource</code>；PKGBUILD 是源，<code>.SRCINFO</code> 只由 makepkg 生成。
- 分别构建/安装 <code>mcpp-bin</code> 与 <code>mcpp-m</code> 的最小 smoke。
- 所有 package 预检全部通过后才允许任何 push，避免 mcpp-bin 先成功、mcpp-m 后失败。
- dry-run 默认不加载 SSH secret、不 push，只输出 desired/current、生成文件、diff 和验证报告。

#### 7.9.3 对账工作流

触发：

- release workflow 成功事件：低延迟路径。
- schedule：建议每 6 小时。
- workflow_dispatch：恢复/指定版本，但默认仍以 latest stable manifest 为准。

算法：

~~~text
read latest immutable Release Manifest
  -> query AUR RPC and git heads for every managed package
  -> compare desired/current with Arch vercmp; reject implicit downgrade
  -> generate all expected repositories in temp dirs
  -> validate all package postconditions
  -> for each drifting package, push idempotently with bounded retry/backoff
  -> query AUR RPC/git again
  -> succeed only if observed state matches expected state
~~~

行为：

- AUR maintenance、连接超时等 retryable failure 使用指数退避与抖动。
- auth、invalid metadata、checksum mismatch 属于 permanent failure，立即失败并告警。
- 每个 package 的 push 可独立重试，但总体状态明确显示 partial convergence。
- GitHub Release 是 desired state，AUR 是 observed projection；无需再维护一份可漂移的 mutable ledger。
- 错过中间 release 时允许直接收敛 latest stable，不要求重放全部历史版本。
- workflow_run 只负责唤醒；每次都重新读取最新完整、非 draft、非 prerelease manifest，迟到事件不得降级 AUR。
- 已知 AUR package clone 失败时不得自动当作“首次发布”初始化空仓；首次认领必须是独立、显式流程。
- 读取可走 HTTPS，SSH 只用于 push；固定官方 host key，不在发布时盲信动态 <code>ssh-keyscan</code>。
- 使用专用发布身份/Ed25519 key；普通 PR 与不受信任代码永远拿不到 secret。
- AUR git head 是立即验证源，RPC 允许 bounded poll；RPC 延迟不得触发重复提交。
- 使用普通 fast-forward push，禁止 force-push/历史改写。

建议 SLO（待 review）：

- AUR 可用时，release 后 30 分钟内收敛。
- 事件失败后，scheduled backstop 最迟 6 小时再次尝试。
- 连续 24 小时未收敛触发高优先级告警。

<code>mcpp-git</code> 是否发布是独立产品决策；若保留，必须加入 managed package matrix 和相同 postcondition，不能只在仓库里放模板。

## 8. 兼容与迁移策略

### 8.1 双读单写

- xlings/mcpp 在迁移窗内读取 schema 1 和 2，只写 schema 2。
- build cache schema 改变视为 cache miss，不尝试就地猜测迁移。
- 唯一可转换的 legacy identity 在 ingress 转换；歧义立即失败。
- 旧模板语法有明确两个 release train 的弃用窗口。

### 8.2 Fail-open 与 fail-closed 边界

| 情况 | 行为 |
|---|---|
| 缺少可再生成的 CDB projection | 本地重投影 |
| 旧但唯一可转换的 contract | 转换 + warning |
| 多 payload、同短名多候选 | fail closed |
| Rule A/B 已证明的物理冲突 | 经过 observe/warning 迁移后 hard fail；解析不确定单独报告 inconclusive |
| Policy D 仅在本地用户项目违反且 A/B 安全 | 默认清晰 warning；索引 CI hard fail |
| 未知 cfg、placeholder、output | hard fail |
| AUR 外部维护 | retry + scheduled reconcile，不回滚 GitHub release |

### 8.3 可回滚性

- contract/snapshot 都有 schema 与原子文件；回滚二进制时旧 reader 忽略不支持的新文件并重新 materialize。
- symlink bridge 只在新 runtime export 经过至少一个 release train 后移除。
- 渠道 reconciler 生成提交前保存 expected diff；不 force-push AUR 历史。
- 不通过修改全局 host env 回滚 runtime；切换 active RuntimeBinding。

## 9. 分阶段交付

### Phase 0 — 止血，不等待完整架构

1. mcpp：C1 输出完整性检查/CDB repair；C2 masker；C3 color；C4 PackageId 比较；C5 CommandSpec；删除 C8 循环。
2. scaffold：#380 NameAtom、路径约束、单次渲染、临时目录事务。
3. AUR：修复 mcpp-m checksum 生成；增加 PKGBUILD/.SRCINFO 一致性 gate；全部预检后再 push；再人工恢复一次对账。
4. release：先完成 C5/C6 的候选/版本探测闭环，再将 bundled xlings 升到已验证的 2026.8.9.x，并跑冷 HOME 原生 gate；不能只改 pin。

每项独立小 PR，避免 P0 被 contract v2 设计阻塞。

### Phase 1 — Identity 与本地契约

1. PackageId/PackageRef/ProjectIdentity 全链路。
2. 新 <code>--template PackageRef --variant</code> 表面和 legacy ingress。
3. BuildSnapshot/OutputManifest。
4. EnvOp schema v2 golden fixtures。
5. xlings Runtime & Install Contract v2 writer；mcpp 双 reader。
6. XlingsCandidate capability/provenance 选择。

### Phase 2 — Runtime physics 与 graphics

1. post-link ELF parser/verdict 与 <code>mcpp why runtime</code>。
2. xim-pkgindex Policy D CI。
3. mcpp 读取 transitive runtime exports。
4. Linux llvmpipe/native 图形矩阵。
5. 弃用 <code>compat.glx-runtime</code> symlink bridge。

### Phase 3 — 脚手架 UX 与原生 GUI vertical slice

1. TOML AST 依赖注入与模板 contract。
2. ImGui/GLFW canonical template。
3. Linux、macOS、Windows 各自一条从 <code>mcpp new</code> 到真实运行的冷 HOME vertical slice。
4. 机器输出与 IDE 消费统一。

### Phase 4 — 发布渠道最终一致性

1. Release Manifest。
2. AUR event + schedule reconciler、retry 分类、推后验证。
3. 将 Homebrew/其他渠道逐步迁到同一 manifest 模型。
4. 发布 dashboard 展示 GitHub release、索引、AUR 和 bundled xlings 的期望/实际版本。

## 10. 跨仓库 PR 切分建议

| 顺序 | 仓库 | 小 PR 主题 | 依赖 |
|---|---|---|---|
| 0A | mcpp | AUR generator/checksum consistency + dry-run validation | 无 |
| 0B | mcpp | C1/C3/C5 快速修复与回归测试 | 无 |
| 0C | mcpp | scanner masker（C2） | 无 |
| 0D | mcpp | scaffold transaction（#380） | 无 |
| 1A | xlings | install/runtime contract v2 + EnvOp v2 writer | 设计字段冻结 |
| 1B | mcpp | contract v1/v2 reader + candidate selection | 1A fixtures |
| 1C | mcpp/libxpkg consumer | PackageId 全链路与模板 CLI | identity spec |
| 1D | mcpp | BuildSnapshot/OutputManifest | 0B |
| 2A | mcpp | ELF actual-artifact inspector | 1B |
| 2B | xim-pkgindex | Policy D closure CI + sentinel schema | 1A |
| 2C | mcpp-index | graphics runtime exports 消费，移除桥的准备 | 1A/1B/2B |
| 3A | 三平台仓库矩阵 | GUI cold-home vertical slices | 1C/2A/2C |
| 4A | mcpp release | Release Manifest + AUR reconciler | 0A |

协调者负责 schema fixtures、跨仓集成顺序和最终 native gate；各 PR 保持可独立回滚，不改写历史。

## 11. 验收标准

### 11.1 功能与回归

- #380 的 hang、路径逃逸、控制字符、部分目录均有回归测试。
- 同短名不同 namespace 的 package/template 可同时存在，bare ambiguity 明确失败。
- 删除 artifact、CDB、graph、verdict 任一项后，下一次 build 能正确重建/重投影或完整 prepare。
- C2 最小 corpus 及现有真实项目 corpus 零 pass-to-fail。
- <code>--no-color</code> 在 TTY、pipe、Windows console 均无 ANSI。
- EnvOp golden fixtures 在 xlings 与 mcpp 结果字节一致。
- A/B 测试含：同 binding 成功、loader/libc 混源失败、host object ceiling 高于 runtime 失败、低于等于成功。
- 安装 graphics 不改变无关 ELF 的 interpreter/provider/GLIBC ceiling。

### 11.2 用户路径

每个平台都从隔离 HOME/MCPP_HOME/XLINGS_HOME 开始：

~~~text
install released mcpp
  -> verify selected bundled/upgraded xlings
  -> mcpp new namespaced GUI project
  -> resolve/install dependencies
  -> configure/build
  -> run platform-appropriate smoke
  -> delete CDB/artifact and verify repair
  -> inspect mcpp why runtime / wire output
~~~

不得用已缓存开发机状态替代。

### 11.3 性能

- scanner O(bytes)，以大 translation-unit corpus 防止超线性回退。
- no-change build 不启动 xlings、不访问网络。
- artifact physics 仅在新 artifact 或 contract hash 变化时运行。
- 固定硬件建立 median/p95 基线后，hot no-op build 回退不超过 <code>max(5%, 10 ms)</code>；缺 CDB 只允许一次轻量 reconfigure/project。
- 增量链接的 artifact inspector 额外成本目标不超过 <code>max(5%, 50 ms/产物)</code>；hot no-op parse 次数必须为 0。
- contract 解析与 snapshot 校验有单独 benchmark，避免 JSON/磁盘布局成为热路径瓶颈。

### 11.4 多平台与发布

- Linux x86_64/aarch64、macOS arm64/x86_64、Windows x64 至少有原生 cold-home gate。
- 不从 Linux source review 推断 Windows spawn/console 正确。
- release 完成后验证 remote tag/commit、assets、checksums、bundled xlings、索引版本。
- AUR push 后同时验证 AUR git head 与 RPC version；只看到 workflow green 不算渠道收敛。

## 12. 可观测性与用户体验

新增统一解释命令，优先扩展现有 <code>mcpp self</code>/<code>mcpp why</code>，不创建大量顶层命令：

~~~text
mcpp self env
  selected xlings, all candidates, provenance, contract schema

mcpp why package namespace:name
  normalized PackageRef, selected version, source index, dependency path

mcpp why runtime <artifact>
  loader/libc provider, dynamic closure, host-link leaves, A/B verdict

mcpp build --output wire
  versioned event/result envelope only
~~~

human 模式先给解决动作，再给细节。例如 loader/libc 混源时直接指出是哪个对象、来自哪里、需要哪个 runtime contract，而不是只输出 “GLIBC not found”。

## 13. 风险与缓解

| 风险 | 缓解 |
|---|---|
| schema v2 同时改 xlings/mcpp，发布错位 | 双读单写、共享 fixtures、capability negotiation |
| typed identity 改动面大 | 先在 ingress/graph 边界引入，禁止新增裸字符串 key |
| ELF parser 容易遗漏格式 | 最小只读范围、fixture 与 readelf/patchelf 对照、fuzz |
| Policy D 过严伤害用户自定义宿主库 | 只在 index CI hard fail；用户项目区分 A/B 与 D |
| GUI native CI 不稳定 | headless PR gate + 有标签的 native scheduled gate，分别报告 |
| AUR 部分发布 | 全包预检、独立状态、post-push reconcile；不 force-push |
| contract 使 fast path 变慢 | content hash 缓存、只读本地文件、按 artifact digest 复用 verdict |

## 14. Review 需要确认的决策

1. 是否接受 **方案 C：分层 typed contracts**，并明确拒绝 xlings daemon 化？
2. 是否接受模板 CLI 将 provider 与 variant 分离：<code>--template namespace:name@version --variant name</code>？是否还需要后续提供 <code>#variant</code> 紧凑糖？
3. 是否接受项目逻辑 namespace、项目名和输出目录分别由 <code>--namespace</code>、位置参数、<code>--dir</code> 表达？
4. 是否接受 Rule A 按实际 host object 的 GLIBC symbol requirement 计算，而不是比较两端 glibc 发行版本？
5. 是否接受 A/B 在用户构建 hard fail、Policy D 只在索引 CI hard fail的分层？
6. 是否接受 mcpp 不做 GPU 探测，GPU/宿主驱动选择只由 xim sentinel/provider 负责？
7. 是否接受 Runtime & Install Contract v2 使用相对路径、content hash、双读单写迁移？
8. 是否接受 AUR 先修 generator consistency，再启用 6 小时 scheduled reconcile；30 分钟/6 小时/24 小时作为初始 SLO？
9. <code>mcpp-git</code> 是正式维护的第三个 AUR package，还是从自动发布范围明确移除？
10. Phase 0 的顺序是否同意：AUR 元数据安全、C1–C5/C8、#380 事务、bundled xlings 升级并行止血？

## 15. 证据锚点与验证边界

下列行号对应本文基线，后续代码移动时以符号为准：

| 主题 | 当前实现证据 |
|---|---|
| C1 fast path / CDB | <code>src/build/execute.cppm:705-774</code>；<code>src/build/ninja_backend.cppm:1547-1549</code>；<code>src/build/compile_commands.cppm:282-307</code> |
| C2 scanner | <code>src/modgraph/scanner.cppm:137-189,579-590</code> |
| C3 color | <code>src/cli.cppm:97-108</code>；<code>src/ui.cppm:233-240,266-331</code> |
| C4 exports | <code>src/modgraph/scanner.cppm:786-792</code>；<code>src/modgraph/validate.cppm:90-118</code> |
| C5 process | <code>src/fallback/xlings_binary.cppm:156-183</code>；<code>src/platform/process.cppm:191-226,335-350</code> |
| C7 EnvOp | <code>src/xlings/subos_info.cppm:250-300</code>；<code>tests/unit/test_subos_info.cpp:290-310</code> |
| C8 dead loops | <code>src/toolchain/lifecycle.cppm:551-565</code>；<code>src/build/prepare.cppm:1664-1670</code>；<code>src/pm/package_fetcher.cppm:923-936</code> |
| #380 | <code>src/scaffold/create.cppm:183-298</code>；<code>src/scaffold/template.cppm:134-182</code> |
| template identity | <code>src/scaffold/template.cppm:22-47,125-228</code>；<code>src/scaffold/create.cppm:30-141</code> |
| #392 selection | <code>src/toolchain/post_install.cppm:378-388</code>；<code>src/xlings.cppm:35-48</code> |
| #396 pre-link only | <code>src/build/hermetic.cppm:103-211</code>；<code>src/build/ninja_backend.cppm:1575-1582</code> |
| runtime flattening | <code>src/manifest/types.cppm:455-467</code>；<code>src/build/plan.cppm:631-712</code>；<code>src/build/flags.cppm:709-724,860-885</code> |
| AUR trigger/push | <code>.github/workflows/aur-publish.yml:14-102</code> |
| AUR checksum defect | <code>scripts/aur/update.sh:61-66</code>；<code>scripts/aur/mcpp-m/PKGBUILD:32-35</code> |

本次只读交叉审计没有执行会安装 graphics 或创建工程的命令，也没有在 WSL2、AMD/Intel/NVIDIA、macOS MoltenVK、Windows Vulkan 或 Linux aarch64 graphics 上实跑。mcpp 基线已有的 7 个被触发 Actions workflow 全绿，只能证明现有覆盖集；它们没有针对性覆盖上述大部分缺陷，native aarch64 在该提交也未触发。因此本文不建议仅凭现有 CI 关闭 #397/#396/#392/#380。

## 16. 相关既有设计

本文是跨问题的收敛层，不替代以下文档的细节：

- <code>.agents/docs/2026-08-07-xlings-as-runtime-substrate-design.md</code>
- <code>.agents/docs/2026-08-08-payload-version-and-contract-drift-design.md</code>
- <code>.agents/docs/2026-08-08-machine-readable-output-protocol-design.md</code>
- <code>.agents/docs/2026-08-05-build-mcpp-extensibility-architecture.md</code>
- xlings 的 <code>.agents/docs/2026-08-09-ecosystem-closure-design.md</code>

如果本文获批，下一步不是直接开启一个大实现，而是把第 14 节决策写成冻结的 ADR/schema fixtures，再按第 10 节拆分小 PR。
