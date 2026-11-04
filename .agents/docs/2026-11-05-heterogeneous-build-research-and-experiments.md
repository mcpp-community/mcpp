---
subject: heterogeneous-build
status: active
---

# 异构构建补充方案的第二轮调研与实验

**读者：** review mcpp 通用机制、平台包及异构插件的维护者。

**本文回答的问题：** 首轮同意的方向中，哪些具体接口和责任可由实际控制收敛。

**范围之外：** 实现补丁、发布操作及未执行的 GPU、跨系统和集群验收。

日期：2026-11-05。相关规范：[构建插件](../../docs/specs/build-plugins.md)、
[平台](../../docs/specs/platforms.md)、[目标侧](../../docs/specs/target-side.md)。
前文：[异构构建语义与插件边界补充方案](2026-11-05-heterogeneous-build-semantics-and-plugin-boundaries-supplement.md)。

状态：首轮 D1–D10 的方向已获用户同意；D10 增加了“可在 build.mcpp 中按需配置”
的要求。本记录给出实验、收敛建议和仍需 review 的具体取舍，不表示实现已落地。

## 1. 基线与证据范围

| 对象 | 身份 | 用途 |
|---|---|---|
| 发布程序 | mcpp 2026.11.3.1，SHA-256 `e1fb3ece147c7511cc447c4b4e2499cc6f51a15c276cf771772f27e4539c1799` | 实际构建、工作空间及协议控制 |
| mcpp 源码 | `e0aee5cb7dfa5b13d1a5248d447947534d3cfa8b` | origin/main；API、解析、兼容比较与作用域 |
| mcpp-plugins 源码 | `86c76ce7dcf148e8333a69fcecf7bd97f076f415` | origin/main；已发布规则与工具声明 |
| DPC++ | 7.1.0，clang 22.1.0，Intel LLVM `880399f47228ac92f403e3b2b865cf1d6f0b1382` | 实际编译、设备链接和镜像提取 |
| CUDA 工具 | cuda-nvcc 12.9.86 | ptxas、设备库及 fatbin |
| 实验系统 | Linux x86_64 | 不代表 macOS/Windows 或其他架构 |

独立的 DPC++ 控制使用系统 GCC 13 头文件与库；mcpp 构建控制使用其解析到的
GCC 16.1.0。两个工具环境分开记录，不把独立驱动实验写成插件集成通过。
原始脚本、输入与日志位于 `/tmp/mcpp-887-research-round2/`；本文保留关键事实及
复现结构，临时文件不作为未来版本的交付保证。

## 2. E1：通用请求解析与集合行为

从准确源码提取未修改的 `AccelSet`、`major_of`、`parse_accel` 和
`accel_arch_level/covers/accepts`，使用系统 GCC 13 以 C++23 独立编译。
再对实际发布程序执行不含 GPU 源码的控制，分清函数行为与真实命令行为。

### 2.1 实际发布程序的结果

| 请求或控制 | 11.3.1 实际结果 | 结论 |
|---|---|---|
| `--accel 'sycl+{sm_89'` | 构建成功 | 缺少右括号仍被接受，需核心结构验证 |
| `--accel 'sycl+{sm_89}garbage'` | 构建成功 | 未消费的尾部文字被忽略，需核心结构验证 |
| `sycl+{spir64,nvptx64-sm_89,nvptx64-sm_90}` | 通用请求可进入构建 | 既有核心形状可承载带归属的混合目标；不证明现有 SYCL 规则已消费 |
| 源约束为 `sycl+{sm_90}`，请求 `sycl+{sm_89},sycl+{sm_90}` | 拒绝，不满足约束 | 比较只取第一个同名成员 |
| 同一源约束，将上述两个成员顺序交换 | 构建成功 | 重复成员导致顺序相关，需明确拒绝或合并策略 |
| 同一源约束，请求 `sycl+{sm_89,sm_90}` | 构建成功 | 单成员目标集合是正常对照 |
| 同一目标集合交换集合内顺序 | 指纹从 `afb8577403ee8e10` 变为 `95ecca2e6fe9ed71` | 有冗余重编；没有证明错误命中 |

这些均为同一普通 C++ 程序上的构建控制，不需要 GPU。前两项不是特定 SYCL
参数的值域问题，而是核心未完整验证通用结构。重复成员的最终政策尚需 review，
但不能继续无诊断地让成员次序决定是否满足同一集合约束。

### 2.2 推荐收敛

核心首先完整验证请求结构、配对括号、空元素及未消费尾部，并给出位置与请求来源。
L2 读取核心已接受的通用形状，L3 检查模型的目标值域；未知厂商目标不由核心枚举。
暂不新增公开的结构化 API，以现有 `mcpp::accel()` 为传递通道。

推荐每种实现只出现一个成员，重复实现直接拒绝并提示合并目标集合；集合内相同
目标可去重，等价集合规范化，命令 argv 保留原有顺序语义。规范化进入新版本的
身份规则，说明一次缓存迁移，不静默改写用户 manifest 或锁记录。

SYCL 推荐评审的规范表达为：

```toml
[build]
accel = "sycl+{spir64,nvptx64-sm_89,nvptx64-sm_90}"
```

该表达基于已有通用结构，**尚不是已发布 SYCL 规则的使用承诺**。不写目标集合的
`sycl` 保留既有缺省 SPIR-V 意义；显式集合只生成所列形式，不偷偷追加回退。
`sm_89` 可作为仅指 NVIDIA 的兼容简写评估，诊断与报告优先显示有域的目标名。
其他后端的版本修饰保留已有含义，不在本轮统一解释为编译器包版本。

## 3. E2：可选 action 子进度

### 3.1 当前 API 与机制

`src/build/hostprogram.cppm::action` 具有 `description`、`env`、`cwd`、输入输出和
角色，**没有子进度配置字段**。核心通过 `MCPP_ACTION_STARTS` 记录 check/prepare
开始；`__action` 会隔离父构建的 Ninja 进度环境，子构建不能直接报告到父开始文件。

设置 `NINJA_STATUS` 只改变子工具输出，不解决外层 Ninja 缓冲。Ninja 的 console
pool 可以直连标准流，但深度为一，并改变并发输出行为，因此不把所有长 action
切入该 pool 作为默认解决方案。[Ninja 官方手册](https://ninja-build.org/manual.html#_the_literal_console_literal_pool)

### 3.2 实验结构与结果

真实外层 Ninja → 发布版 `mcpp __action` → 研究用 adapter → 真实内层 Ninja。
内层串行执行三个各等待 0.4 秒的命令；adapter 同时转发普通日志，并原子替换各
action 自己的 JSON 快照。独立观察器读取快照，模拟未来核心读取通道。

| 控制 | 首个快照 | 首次外层日志 | 结果 |
|---|---|---|---|
| 普通模式 | 0.469 s | 1.347 s | action 成功、stamp 存在 |
| verbose | 0.487 s | 1.373 s | verbose 未解除外层缓冲 |
| 两个 action 并行 | 两者约 0.478 s | 1.350 s | 各自快照推进，均成功 |
| 子计数 3/3 后 adapter 返回失败 | 仍可读到 3/3 | 约 1.379 s | 外层失败，stamp 不存在 |
| 关闭报告 | 无快照 | 1.328 s | 构建仍成功 |
| 首个快照后向进程组发送 SIGINT | 0.476 s | 0.479 s | 外层退出 2，stamp 不存在 |

这些结果验证 Linux 上可绕过日志缓冲的传输形态，**没有修改 mcpp 的进度 UI**，
没有验证 Windows 的替换、取消和打开文件行为，也不证明 GPU 编译持续活跃。
另一个快照校验原型拒绝残缺 JSON、过期序号、负数、布尔计数、越界计数和超长数据；
一万次更新只保留最后一份状态。它同样不是当前核心实现。

### 3.3 R1：配置与运行时报告的最小接口

推荐新增一个可选字段，沿用 action 现有字段风格，默认关闭：

```cpp
// 拟议接口；11.3.1 没有 progress 字段。
mcpp::action a;
a.id = "component-build";
a.role = mcpp::roles::prepare;
a.description = "Build the external component";
a.progress = true;
// 声明 inputs、outputs、output_dir 和 argv，随后 submit。
```

`build.mcpp` 只声明该 action 愿意提供进度，不在规划期施工，也不静态声明总任务数。
工具本身或插件 adapter 在执行期报告。核心不解析 LLVM、CMake 或 SDK 的业务日志。

| 层 | 提议职责 |
|---|---|
| 核心 | 为被观察的运行分配独立目录与 action 文件，注入预留环境变量，读取、关联和呈现状态 |
| 插件通用库 | 提供不依赖具体工具的快照写入辅助；没有通道时直接无操作 |
| 工具适配器 | 解释自己的阶段、已完成项及已知总量，作为该 action 的单一报告者 |
| 用户 | 在 build.mcpp 或规则 options 中按需启用；普通无报告 action 继续有效 |

候选运行变量名为 `MCPP_ACTION_PROGRESS_FILE`；文件由引擎分配，不由工程固定路径。
一份快照示意为：

```json
{"v":1,"seq":12,"phase":"compile","done":80,"total":200}
```

核心确定 action 身份；快照不提交任意 action id，不声明成功，不改变图或产物。
阶段名由报告者定义。未知总量省略 `total`；阶段变化可重新计数，`seq` 在该次运行内
递增。快照按同目录临时文件替换，每 action 单写者；旧运行路径不复用。

建议初始边界为每份最多 8 KiB，读取在解析前截断并拒绝超限输入，只保留活动 action
的最新状态，写入限频至每秒五次。
TTY 按 UI 节奏采样；非 TTY 合并输出阶段变化及间隔状态；JSON 使用有版本的事件，
不把普通子 stdout 直接混入 NDJSON。残缺或不可读的进度丢弃并限次诊断，命令和
产物的失败仍正常报告。关闭显示且没有机器观察者时不需要分配实时通道。

普通日志和快照彼此独立。action 仅在进程成功及既有产物后置条件通过后完成；计数
到 100%、心跳和“阶段完成”都没有该权限。取消后停止接收本次状态，保留既有取消
和 stamp 规则；遥测文件不进入安装包、发布包或构建输入。

可选报告适用于各 action 角色。source/object 的真实输出不能借用 `--stamp` 传给
包装器，否则成功后写 stamp 会覆盖源码或对象。观测身份与 stamp 分开，已有真实
产物由原命令产生。发布版的无 stamp 包装控制已实际生成 ELF 对象，字节与直接编译
一致；新增观测身份、开始记录和 UI 归属仍需在核心实现，不能只复用 check/prepare。

**兼容性边界：** 旧 action 不设置字段时继续省略它；新核心读取旧 payload 时缺省
关闭。新 API 需相应核心版本；插件若兼容旧核心，只能通过依赖式模板探测可选字段
并降级报告，或明确提高最低版本，不能在普通运行时 `if` 中访问不存在的成员。
已用未修改的旧 action 结构与研究用新结构独立编译依赖式 `requires` 探测：旧结构
返回不支持，新结构成功启用。该控制没有为旧核心增加实际进度能力。
实时快照和随机路径不进身份；修改 build.mcpp 启停该字段仍可能按现有规则重规划或
重建，不承诺零失效，也不因遥测而重新设计全部缓存。

## 4. E3：provider 绑定与互斥的作用域

### 4.1 真实工作空间反例

虚拟工作空间包含 app-a、app-b。app-a 只依赖 provider-a，app-b 只依赖 provider-b。
两 provider 都提供 `probe-interface`，均声明它为 exclusive，各自定义同名入口，
分别返回一和二。两个应用分别断言对应返回值。

| 控制 | 结果 |
|---|---|
| 单独构建并运行 app-a | 成功 |
| 单独构建并运行 app-b | 成功 |
| 同一工作空间 `build --workspace` | 被全局 exclusive 检查拒绝 |
| 去掉 exclusive，构建工作空间并直接运行两个产物 | 均成功；生成图确认每个程序只链接自己的 provider 对象 |
| 一个应用同时依赖两个 exclusive provider | 正确拒绝，保留为负向控制 |
| 去掉 exclusive，但两个应用各自的默认 feature requires 同一 capability | 各自构建通过，工作空间构建被全局多 provider 歧义检查拒绝 |
| 共享 helper 声明 requirement，两个应用分别在根供给 provider | 各自构建通过，工作空间仍因候选被全局混合而拒绝 |

按 `step6_capability_binding` 的实现，候选和互斥声明按准备图累计，requirement 还
按 capability 名全局去重。实际不同链接闭包的程序因此被当作同一组合。这是已复现
的 mcpp 通用作用域问题，不是 GPU 插件特例，也不由取消 exclusive 声明解决。

### 4.2 R2：两个检查范围

推荐 requirement 按**最终消费上下文中对要求者可见的已声明候选**选 provider，
不能从同工作空间其他独立程序的闭包借候选。候选包括应用在本消费图中供给的
provider，不限于要求者自身的子依赖：共享 helper 声明需求、应用分别供给的控制
实际通过，不能因修正全局作用域而破坏这种注入。
exclusive 则按**实际组合的链接闭包**检查，防止同一程序或共享产物组合冲突。
独立程序、独立工具图不能因一次批量构建就相互排斥。

绑定记录至少区分消费上下文、要求者和 capability，不再按 capability 名全局去重。
同一 helper 可在两个应用中绑定不同 provider；若答案影响编译输入，则分别形成
相应变体身份，若只影响最终链接则不无理由复制无关编译。该身份与闭包变化仍需
实现验证，不能仅把全局检查搬到另一处。

暂不新增用户可选的“全局/局部互斥模式”。已有包声明继续表达同一接口的互斥，
核心修正检查上下文。pin 在其适用上下文内选择候选，不能凭 pin 删除已经链接的
另一个定义。需要检查静态库、共享库、测试程序及显式运行依赖；未声明的 dlopen
不由构建图推断。错误诊断展示要求者或链接产物及两条依赖路径。

## 5. E4：实际多目标、跨 TU 和跨静态库

### 5.1 真实设备代码的控制

`kernel.cpp` 提交一个 SYCL single_task，调用由另一个 TU 定义的
`SYCL_EXTERNAL int add_seven(int)`；设备结果预期为 12。
实际驱动采用明确的 target triple 与逐目标后端参数：

```text
-fsycl-targets=nvptx64-nvidia-cuda
-Xsycl-target-backend=nvptx64-nvidia-cuda --cuda-gpu-arch=sm_89
-Xsycl-target-backend=nvptx64-nvidia-cuda --cuda-gpu-arch=sm_90
```

混合形式使用 `-fsycl-targets=spir64,nvptx64-nvidia-cuda`，仅请求 sm_89。
这与[驱动手册](https://intel.github.io/llvm/UsersManual.html)的目标和后端参数分工一致；
这里的最终判断来自锁定驱动的实际编译与产物，不只来自文档或 `-###`。

| 场景 | 实际结果 | 覆盖层级 |
|---|---|---|
| 两 TU、sm_89 与 sm_90 | 成功，约 6.68 s；从最终程序提取四份 NVPTX 镜像 | 实际编译、设备链接、最终产物 |
| 两 TU、SPIR-V 与 sm_89 | 成功，约 6.46 s；提取一份 SPIR64 与两份 NVPTX 镜像 | 实际编译、设备链接、最终产物 |
| 移除设备函数定义 TU | ptxas 拒绝未解析的 `_Z9add_seveni` | 负向设备链接 |
| 单目标 sm_89，将 util.o 放入 libdevice.a | 编译、归档与最终设备链接成功，提取两份镜像 | 单目标跨静态库 |
| sm_89 与 sm_90，二者都封装在 libdevice.a | 独立驱动链接报同一设备符号重复定义 | 多目标与归档的组合未通过 |
| NVIDIA 目标别名，无 save-temps | 实际编译通过 | 不把别名整体归为不可用 |
| 目标别名与 `-save-temps=obj` 组合 | 报 `OpenMP target is invalid`；显式 triple 同类控制通过 | 该驱动组合限制，非核心证明 |

CUDA ELF 的 `e_machine=190`，两组 flags 分别为 `0x590559`、`0x5a055a`；对应
镜像字节可在最终程序找到，另以 clang-offload-extract 验证。混合程序中提取的
SPIR64 镜像具有 SPIR-V 魔数。不能用四份镜像推断四种硬件目标：多个设备模块可以
属于同一目标，目标覆盖需按实际元数据及内容检查。

独立驱动成功时还有 libdl、libpthread、librt 搜索路径警告；它不是发行环境验收。
使用 `ONEAPI_DEVICE_SELECTOR=cuda:gpu` 尝试两个程序，均返回
`CUDA_ERROR_SYSTEM_DRIVER_MISMATCH`（803），未执行到数值断言。因此没有 GPU
执行通过、CPU 回退通过或端到端插件通过的声明。

### 5.2 R3：规则的阶段与组合验收

推荐编译和设备链接从同一个已解析目标集合派生。用户的有域目标由插件映射到
明确 triple 和后端参数；驱动别名属于插件内部策略，不要求核心理解它们。

多个 TU 和单目标归档均能真实完成，不足以宣布“多目标 × 归档”也能组合。
该失败发生在独立 SDK 调用中，优先在工具链及插件阶段设计中调查；正确调用或
逐目标分区方案尚未验证前，不宣布上游 bug 已定根，也不增加 mcpp 专属链接模式。
跨包还需证明产物和使用需求正确传播，本轮没有完成该集成。

规则不能只让单目标 fixture 通过。验收增加目标集合、TU 数量、归档边界的交叉控制；
可用组合实际检查镜像，不可用组合应给出明确工具链和阶段诊断，不能静默少生成目标。

## 6. E5：组件边界与平台通用性

普通 C++ 组件放在 `component/`，主图源码列表只含 `src/main.cpp`。build.mcpp
为两个 TU 分别声明 object action，显式命名工具、源码、对象及 depfile；组件入口为
C ABI，内部一个 TU 调用另一个 TU。所有对象由同一个主程序消费。

首次运行输出 11；无变化构建约 0.029 s；只修改组件头文件后输出 21；移除内部函数
定义则在链接阶段明确失败。说明现有 object actions 和 depfile 可承载这一 CPU
组件形态，无需核心接管另一编译器的源码。它不覆盖 C++ 模块跨编译器、OpenMP
offload、stdpar、GPU 运行或完整外部工程输入扫描。

另实际运行现有 e2e 952，通过非 openkal 平台的编译参数、名称、seal、runner、
配置覆盖、关闭平台及错误平台包控制。程序重新链接后只 seal 一次。脚本有一条
命令替换忽略 NUL 的 shell 警告；用例仍通过，不隐去该限制。

还构造了三个单程序打包元数据夹具，均声明 `runs_on.self=[linux-x86_64]`。
native 与 openkal 的 emitter 控制通过，wasm leg 加同一声明被 `program_host` 拒绝。
这是使用原生占位程序的**合成元数据实验**，只能验证名字相关分类，不是一个真实
wasm carrier 的生成和执行证据。

**R4 推荐：** 平台默认升级继续按前文 PL1–PL9 验收；launcher 自举组合独立复核。
载体宿主分类优先评估已有 `runs_on` 与角色能否表达，暂不增加第二套宿主字段。
没有真实非 openkal carrier 交付场景前，不把上述夹具拒绝扩大为整个打包系统错误。

## 7. E6：兼容性与身份的收敛边界

精确源码比较器实际接受了 `custom1.0+{90a}` 对 `custom1.0+{90b}`，以及同一 opaque
目标上的 custom 1.0 对 1.9；重复同名成员只比较第一项。这确认数字和版本关系
应用到整个开放词汇，并不只应用到 CUDA。

但现有公开文档已描述同主版本和数字架构比较，不能把这一既有契约全部称为新发现
的 bug。新 SYCL 目标采用 `nvptx64-sm_89` 等 opaque 名字时，现有架构比较走精确
相等路径；不需要为了 #887 先增加兼容性表达式语言。

**R5 推荐：** 本轮修结构验证、重复成员和作用域缺陷；保留旧描述符的既有匹配契约。
新模型优先使用有域的 opaque 目标，不把新数字规则继续引入核心。新的通用覆盖
元数据和版本化匹配协议，等两个实际模型证明精确路径不足时再独立设计。现阶段
不新增 `accel_match`、覆盖 DSL 或需要运行插件的预构建筛选。

安装来源方面，继续采用用户已认可的不可变发行包与可变 path 边界。进度不要求扫描
所有安装源码；安装后污染的校验和恢复作为独立维护能力，不伪装成已有自动保证。
目标集合换序造成的新指纹单列为规范化优化；不能用减少冗余重编的工作替代正确性
修复，也不在没有验证的情况下重用旧 BMI。

## 8. 后续验收与推进依赖

| 编号 | 待实现或待验证的断言 | 责任 |
|---|---|---|
| V1 | 缺括号、尾部垃圾、空元素在核心入口明确拒绝 | mcpp |
| V2 | 重复实现政策确定；交换成员顺序不再无诊断地改变覆盖结论 | mcpp，L2 共享读取控制 |
| V3 | 独立程序在工作空间可分别绑定，保留共享库需求由应用供给；有效输入变化不误复用 | mcpp |
| V4 | 同一组合闭包内的 exclusive 冲突继续拒绝，pin 不绕过 | mcpp |
| V5 | 各角色可选进度覆盖并发、关闭、失败、取消及机器输出；不覆盖真实输出 | mcpp 与插件通用辅助 |
| V6 | macOS/Windows 的快照替换、并发读取及取消控制 | 核心平台层及 CI |
| V7 | 多目标与跨静态库的组合形成正确设备链接与镜像 | 工具链与规则插件 |
| V8 | 实际跨包设备产物、使用需求、失效和 ABI 闭环 | 插件与包；有契约反例再扩核心 |
| V9 | GPU 设备及多节点实际执行，报告确切实现和设备 | 应用、运行时和环境 |

确定性修复可并行：核心 V1–V2 与插件读取修复先行；V3–V4 独立修作用域；可选
进度 V5–V6 独立开发；默认平台补丁继续走 PL 矩阵。插件混合目标支持与工具请求
闭环后，推进 V7–V9。新匹配协议和额外核心构建模式不作为这些工作的前置。

on-request 继续复用现有协议。SYCL 的 CUDA 载荷声明应由“sycl 与 cuda 同时选中”
改为 SYCL 规则可见的 on-request 声明，再由自己的 NVIDIA 目标请求；不新增厂商 cfg。
本轮重跑 e2e 874 的范围是“不请求不安装、请求到达安装器、未声明请求拒绝及规划
延后”，不能据此宣布实际 SDK 的首次下载与完整集成已经通过。

## 9. 第二遍自审的结论

| 视角 | 修订或保留的边界 | 结论 |
|---|---|---|
| 用户表达 | 有域目标只写一次；混合实现与混合设备分开 | 提案可 review，语法未宣布发布 |
| 核心职责 | 结构验证和闭包作用域为通用问题，SDK 链接流程不进核心 | 已按实际证据重新分类 |
| 接口复杂度 | 进度只增加一个可选声明，不新增静态任务总数、核心工具解析或运行策略 | 推荐接受 R1，生产实现尚缺 |
| 进度可信度 | 3/3 后失败控制无 stamp；心跳和快照无成功权限 | Linux 原型通过，跨系统待 V6 |
| action 角色 | 真实输出不能被当作 stamp；无 stamp 包装与直接生成的 ELF 字节相同 | 观测与产物分开，新增归属仍待实现 |
| 性能与缓存 | 实时文件不进身份；修改构建程序仍可能重建 | 未承诺零开销或零失效 |
| 并发与取消 | 文件按 action 隔离，Linux 进程组取消不产 stamp | 原型通过，不外推全部进程生命周期 |
| 程序组合 | 只看要求者自身子依赖会破坏应用供给；补充 helper 控制后改为消费上下文 | 反例成立，保留注入语义，闭包及身份修正仍需实现 |
| 工具链兼容 | 别名本身成功，失败限定为与 save-temps 的组合 | 修正过宽归因 |
| 设备链接 | 多 TU、多目标分别成功，不自动推出归档交叉组合成功 | 保留实际失败与 V7 |
| 产物核验 | 从最终程序提取镜像；镜像数量不等于目标数量 | 未以命令文字替代产物 |
| 运行环境 | 驱动 mismatch 导致运行失败，未冒充 CPU 或 GPU 验证 | GPU 数值验收未通过 |
| 平台抽象 | 真实非 openkal 平台通过；carrier 夹具仍是合成输入 | 不夸大真实载体覆盖 |
| 历史兼容 | 当前数字规则已有公开契约，不直接全部移除 | 后续迁移独立设计 |
| 发布范围 | 原 #885 验收与新设计分开，已同意方向不等于实现完成 | 保留 review 与交付边界 |

## 10. 实验记录入口

以下均位于 `/tmp/mcpp-887-research-round2/`：

| 记录 | 主要证据 |
|---|---|
| `exact-accel.cpp`、`exact-accel-results.json` | 未修改核心函数及实际输出 |
| `build_probe.py`、`build-results.json` | 发布版请求、工作空间与组件控制 |
| `grammar-duplicate-controls.json`、`set-order-control.json` | 次序相关覆盖与集合指纹 |
| `exclusive-controls.json`、`capability-requirement-controls.json`、`root-supplied-provider-controls.json` | 独立程序、同程序负向控制、requirement 反例及应用供给 |
| `progress_probe.py`、`progress-results.json`、`progress-validation-results.json` | 普通/verbose/并行/失败/关闭/取消及有界快照原型 |
| `progress-api-compat.cpp`、`progress-api-compat-results.json` | 旧 action 与研究用新结构的可选字段探测 |
| `nonstamped-wrapper-results.json` | 无 stamp 包装保持真实对象字节 |
| `sycl_compile_probe.py`、`sycl-real/results.json` | 真实 TU、目标组合与负向设备链接 |
| `sycl-real/artifact-inspection.json`、`sycl-real/extraction-results.json` | 镜像字节、ELF 和最终程序提取 |
| `sycl-real/runtime-alias-results.json`、`sycl-real/alias-save-temps-results.json` | 运行失败与别名对照 |
| `sycl_archive_probe.py`、`sycl-real/archive-results.json`、`sycl-real/archive-single-results.json` | 归档组合的正负控制 |
| `platform952.log`、`on-request874.log`、`carrier-metadata-results.json` | 平台、现有工具请求及合成元数据控制 |

后续代码提交应将确认的最小反例转为仓库内回归测试；研究脚本不是已发布测试套件。
