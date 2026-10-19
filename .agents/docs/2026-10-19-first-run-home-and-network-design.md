---
subject: home
status: active
---

# 首次初始化、home 重定向与网络诊断优化设计

状态：按最新反馈修订；R2、R3、R4、R5、R7 已获用户认可，R1 与 R6 的两阶段优先级及 Windows 排版已明确。设计语义与实现验收分开记录，具体进展见第 12 节。

执行状态：用户已授权完整实现、单功能 PR、全 CI、自我 review 后 squash 合入和发布。以下为实施中的验收账本，不将草稿或发布版探针作为新功能已完成的证据。

| 任务 | 前置依赖 | 完成证据 |
|---|---|---|
| T1 home 两阶段解析与单跳、原子入口更新 | 现有路径/文件替换/锁能力 | 三平台解析与事务单测；真实 CLI 重启与 force 哨兵 |
| T2 只读配置与本地初始化、bootstrap 分离 | T1；provider 配置接口不联网 | 冷 doctor/env/list/config 无下载；缺 provider 继续诊断 |
| T3 init --home、Windows 两项菜单与来源报告 | T1、T2；驱动器/输入终端能力 | Windows 原生终端与非交互对照，Linux/macOS 无菜单 |
| T4 有界资源探测与下载进展/取消 | T2；核实 xlings 现有接口和 low-speed 能力 | 可控端点覆盖 DNS/代理/TLS/停滞/持续进展与进程树清理 |
| T5 文档、规范、升级与版本 | T1—T4 行为稳定 | 中英文规范/手册一致；版本 pin 与兼容检查 |
| T6 单 PR 完整 CI 与综合 review | T1—T5、适用单测/E2E 通过 | 当前 PR head 的实际 CI 结果及 review 记录 |
| T7 squash、release 与 GitCode 补齐 | T6 全部通过 | 合入 SHA、tag、四平台签名校验资源、GitCode GET 核验 |
| T8 xlings 生态真实验证 | T7 资源与索引传播 | 精确版本和 latest 安装、冷/暖/离线使用及各分发验证 |

T1 与 T2 为当前实施入口；T3/T4 共享底层但不互相依赖。版本在发布前按当日空闲序号确定，bootstrap pin 不领先于实际已发布并入索引的版本。尽量在一个 mcpp 功能 PR 中完成；如 provider 缺少必要接口，记录证据后处理上游依赖，不伪造 mcpp 已实现下载层能力。

读者：实现和评审首次运行、home 选择、网络诊断及跨平台 CLI 行为的维护者。

本文回答：如何记住自定义 home、保留已有路径选择和初始化语义，并使首次联网等待可诊断、有界且不破坏离线能力。

相关文档：[home、索引与 store](../../docs/zh/25-home-indices-and-store.md)、[home 解析实现](../../src/home.cppm)、[配置初始化实现](../../src/config.cppm)。

## 1. 范围与证据

代码基线为 main `c0690412e04b59e424c84fe01f171b8e84e4f748`。运行探针使用校验 SHA-256 的 `2026.10.17.1` release，其 tag commit 为 `684a306bced037014da08b038412f57c203d0bc9`。release 与 main 不是同一个 commit；探针不能冒充 main 的 fresh build 验证。

追踪：[issue #822](https://github.com/mcpp-community/mcpp/issues/822)。测试载体：[临时 draft PR #823](https://github.com/mcpp-community/mcpp/pull/823)，仅包含探针与执行它们的工作流，不含产品实现，不计划合入。[最终 Actions 运行](https://github.com/mcpp-community/mcpp/actions/runs/39184402622)，测试 commit 为 `a736c64ca94fc013aed39ffb10d8346c569c45ad`。

本设计覆盖普通用户 home 的持久选择、Windows 首次目录选择、初始化阶段、网络检查、下载停滞处理与 doctor。`--system` 不采用用户重定向文件；实际 home 继续由系统配置决定，`--system self init --home` 拒绝，避免更改用户默认位置。

CI benchmark 与 #816 是同轮调查发现的独立缺陷，见第 10 节；不混入 home 与网络功能实现。

### 1.1 源码事实

| 位置 | 当前行为 | 影响 |
|---|---|---|
| `src/home.cppm::root` | 环境变量、自包含目录、用户目录回退 | 尚无 locator 与 `self init --home` |
| `src/cli/cmd_self.cppm::env_data_readonly` | JSON env 只读解析路径 | 已有可复用的只读方向；human env 仍初始化 |
| `src/config.cppm::load_or_init` | 读取配置时同时初始化 sandbox、安装 ninja；ELF 平台还安装 patchelf | 首次查询、修改配置与诊断可进入联网流程 |
| `write_default_xlings_json` | 新配置默认 `auto`，已有配置保留原 mirror | 自动选择已有入口；不能把改默认值当成完整修复 |
| `src/doctor.cppm::doctor_report` | 初始 `load_or_init` 可联网；后续构建规划已有离线约束 | 后续只读规划不能阻止入口 bootstrap |
| `src/doctor.cppm::self_config` | 先 `load_or_init`，再更新已有 mirror | 用户切源前可能被旧源初始化阻塞 |
| `src/provider/xlings/xlings.cppm::install_with_progress` | 直接安装优先，输出丢弃，外层总上限 3 小时；之后再走接口回退 | 看不到等待原因；切换安装接口不等于切换源 |
| 同文件接口安装 | 300 秒输出静默限制，接口会发 heartbeat | heartbeat 不能证明下载字节仍在进展 |
| 同文件 `ensure_init` | sandbox 初始化子进程总上限 600 秒 | 名义本地操作也可能因首次索引获取等待；不能只在 ninja 下载前探测 |
| `src/doctor.cppm::self_init` | 裸 `--force` 清理 `registry/cache/build-cache/bmi` 后初始化 | 保持现有破坏性语义，新增路径选择不能意外继承删除 |
| `install.sh` | 使用 `MCPP_PREFIX`；若干请求缺少连接或无进展上限 | 安装脚本也有等待缺口；现有入口不能静默改名 |

### 1.2 运行结果

本地 Linux 探针已完成：显式 home、自包含 home、dev 布局回退及 JSON env 不写入通过；离线 doctor 0.615 秒完成并到达后续 build policy 检查；离线 force init 返回 1，但四类目录中的测试哨兵均被删除，其他目录保留。故初始化失败并不撤销已有 force 清理。

注入 `127.0.0.1:9` 的 HTTP/HTTPS/ALL_PROXY 后，冷 doctor 和冷 `self config --mirror GLOBAL` 均进入 bootstrap，并在外部观察上限 12 秒时仍未退出。前者种下 `auto`，后者种下 `GLOBAL`。这说明卡住与 mirror 已经正确写入可以同时发生。

最终 Actions 的三平台 baseline 和模块对照共四个 job 均通过。绿色表示现状断言成立且观测成功，不表示下表中的联网等待或模块错误已经修复：

| runner | home 优先级、只读 JSON env 与 force 哨兵 | 离线 doctor | 冷 doctor + 拒绝代理 | 冷 config GLOBAL + 拒绝代理 |
|---|---|---|---|---|
| ubuntu-24.04 | 通过 | rc 0，0.565 秒 | 超过 12.002 秒，测试器终止 | 超过 12.002 秒，测试器终止 |
| windows-2025 | 通过 | rc 0，1.094 秒 | 超过 12.047 秒，测试器终止 | 超过 12.032 秒，测试器终止 |
| macos-15 | 通过 | rc 0，0.722 秒 | 超过 12.014 秒，测试器终止 | rc 0，约 12.08 秒；bootstrap ninja 失败后仍写入配置 |

外部观察上限设为 12 秒，表中记录实际 wall time，包含轮询和清理开销；macOS config 在等待器观察到自然退出时略超过该值，不能据此声称产品有严格 12 秒期限。

Windows 两个冷命令日志均停在 sandbox `Initialize`；Linux 已进入 patchelf 安装；macOS doctor 已继续到 package home 检查。相同注入下的等待点不同，不能只修某一个下载函数便宣称闭合。

Windows runner 的 D 盘类型为本地固定盘，临时目录写入成功。这只验证检测事实的一个正例，不代表目标 `D:\mcpp` 已验证，也不代表原生菜单已验证。新 locator、Windows 菜单和 5 秒探测尚无实现，不属于此次现状探针已通过的功能。

首轮模块对照 job 失败于测试夹具：setup 导出的 MCPP 为 workspace shim，在临时项目目录中报告 mcpp 未安装；三种模式均未进入真实编译。测试改用固定发布版真实二进制后重跑成功，最终对照见第 10.2 节；[首轮](https://github.com/mcpp-community/mcpp/actions/runs/39184064081) 不能作为 #816 复现证据。完整 CI 也被测试文件触发；本次不合入，仅取专用探针结果，因此取消该临时 PR 的重复全量 CI，不将取消计作通过。

故障注入只覆盖拒绝代理连接，不等价于完整断网，也不覆盖 DNS 阻塞、TLS、重定向后停滞、丢包和真实 CN 用户线路。离线探针检查后续诊断和未创建 bootstrap/index 状态，并未进行操作系统级网络流量审计。

## 2. 原始 home、真实 home 与单跳入口

### 2.1 优先级与兼容边界

解析分为两步，不能把两步合成一张优先级列表。

第一步选择原始 home（入口位置），沿用现有顺序：非空 `MCPP_HOME` → 可识别的自包含安装目录 → 用户目录下的 `.mcpp`。`MCPP_HOME` 在这一步优先级最高，`--home` 不改变原始 home 的选择。

第二步选择真实 home（本次实际存放配置、工具链与缓存的位置）：`self init --home <path>` 的显式目标 → 原始 home 下 `home.toml` 的直接目标 → 原始 home 本身。

默认情况下没有重定向文件，也没有 `--home`，原始 home 与真实 home 是同一个目录。`MCPP_HOME` 不因为引入真实 home 概念而自动变成另外一个路径，不新增第二个真实 home 环境变量。

入口文件为 `<原始 home>/home.toml`。只有原始 home 采用默认用户目录时，它才是 `~/.mcpp/home.toml`；显式 `MCPP_HOME` 与自包含安装不会读取另一套用户默认入口。只有在自己的原始 home 中显式保存重定向后，它们才使用另一真实目录。这修正前版“永远写 ~/.mcpp，再提示被环境变量遮蔽”的方案，避免保存了一个日常命令实际不用的设置。

| 使用条件 | 原始 home | 无重定向时真实 home | 显式保存到 E 后 |
|---|---|---|---|
| 未设置环境变量、非自包含 | `~/.mcpp` | `~/.mcpp` | `~/.mcpp/home.toml → E` |
| `MCPP_HOME=D` | D | D | `D/home.toml → E`，环境变量仍为 D |
| 自包含安装在 P，未设置环境变量 | P | P | `P/home.toml → E`，程序仍位于 P |

`--home` 仅为 `self init` 的选项，本轮不扩展为全局选项。本次目标通过统一 home 上下文传递；不持久写环境变量，也不在进程中把 `MCPP_HOME` 改成真实 home 再次解析，以免下一次读取误把目标当入口、出现第二跳。自包含判断的 `target` 和 `data/xpkgs` 排除规则保留。

用户目录继续采用当前平台规则：Windows 优先 `USERPROFILE`，然后 `HOME`；POSIX 使用 `HOME`。用户目录无法解析且没有其他原始 home 来源时，不写永久 locator，显式报错；保留现有无 locator 情况的工作目录回退行为，以免顺手更改其他路径契约。

### 2.2 文件格式与单跳

例如用户入口文件为：

```toml
# C:/Users/alice/.mcpp/home.toml
home = "D:/mcpp"
previous_home = "C:/Users/alice/.mcpp"
```

`home` 为必填绝对路径。`previous_home` 为可选诊断元数据，只保存上一次持久默认位置，不是另一个跳转入口，也不是待自动清理列表。记录它的原因是 D→E 切换后，仅扫描原始 home 无法报告 D 中保留的数据。用户已认可 R3；该字段不扩展为多条历史记录。

解析器只读原始 home 下的入口文件一次。目标目录中的同名文件不参与解析；本次显式 `--home` 目标中的 locator 也不解释。显式环境变量和自包含目录如果被选为原始 home，则只解释该目录自己的入口。不展开 TOML 中的 shell 变量，不执行命令。命令行相对路径在入口处转为绝对路径后保存；输出路径以平台可读形式呈现，文件内容与机器输出使用正确编码。

不存在、不可读、格式错误或不是目录的保存目标：普通命令报带入口路径和目标路径的错误，禁止回退后创建另一套 home。未被选中的原始 home 中的 locator 不读取，其损坏不影响当前来源。`self init --force --home P` 可在确认原始 home 后修复损坏的 locator，不要求先成功解析旧目标；无法确认旧位置时不编造 `previous_home`。

切回原始 home 本身时移除 locator，而不是写出指向自己的 locator。实现按路径身份识别同一路径，涵盖 Windows 大小写、分隔符、junction 与 POSIX symlink 别名。相同目标视为幂等；拒绝把原始 home 重定向到自己的子目录或祖先目录，避免入口与真实布局相互包含；不以字符串前缀判断。

原始 home 已经有旧数据时保留数据。新用户采用用户默认入口并选择外置 home 后，入口目录只有 locator；已有用户或自包含安装可能同时保留旧配置、registry、缓存和程序文件，不能自动清空。

## 3. 初始化、切换与 force

### 3.1 命令矩阵

| 命令与状态 | 结果 | 删除行为 |
|---|---|---|
| `self init`，已有当前 home | 幂等检查、补全当前 home | 无 |
| `self init --home P`，无已初始化 home | 本地初始化 P，保存用户默认，按需 bootstrap | 无 |
| `self init --home P`，与当前/已存默认为同一路径 | 幂等采用 P，必要时保存指向 | 无 |
| `self init --home P`，已有不同的初始化 home | 退出 1，给出当前位置、目标及 `--force --home` 提示 | 无 |
| `self init --force --home P` | 覆盖保存的默认指向，采用或补全 P | 不删除旧 home，也不清空 P |
| 裸 `self init --force` | 保留现有重建语义，作用于本次解析出的 home | 清理现有四类目录 |
| `--offline self init --home P` | 本地配置与默认指向正常建立，不探测、不下载 | 无 |

存在空目录或 locator 不等于已经初始化。现有 home 的识别依据可读取的 mcpp 配置和可识别的 mcpp 布局；布局不完整也属于已建立的 home，应提示修复而非假装首次运行。不能通过联网判定是否已有 home。

检查既有 home 时，同时区分原始 home、未应用 `--home` 前的真实位置与已保存目标。显式环境变量的值、自包含程序的位置不被改写；本次明确的 `--home` 操作在该原始 home 下保存默认真实位置。

目标已有合法 mcpp home 时验证后复用；目标是不可写目录、非法路径或与既有布局不兼容时拒绝。`--force --home` 不授权清空不兼容目标。与当前目标相同的 `--force --home` 仍为非破坏性路径选择，不转为裸 force 的删除逻辑。

### 3.2 持久写入时机

状态转换为：解析与冲突检查 → 建立目标本地配置 → 原子保存默认位置 → 按需网络/下载 → 校验。

路径或本地配置验证失败时不改 locator；网络失败或 Ctrl+C 发生在保存后时，保留已保存目标和可恢复的本地状态。报告 bootstrap 未完成，下一次继续使用新目标。离线缺少必需工具时退出 1，并明确区分“默认位置已保存”和“初始化未完成”。

locator 同目录临时写入，再原子替换；使用原始 home 处的锁串行化多个进程的默认位置更新，替换前重读并检查冲突。需要保存或覆盖入口时，先验证原始 home 的入口可写性，再建立新目标，避免在不可写自包含目录下初始化到最后才发现无法保存。保存本身失败时仍保留旧文件；新目标已建立的本地文件不自动回滚删除，并明确报告未切换。已有入口的普通读取不要求原始 home 可写、不创建锁文件、不联网；不修改 shell profile、用户环境变量或系统配置。

本轮不新增 `self reset`，不删除旧 home，不进行数据迁移。

### 3.3 只在相关操作输出提示

新提示限定在：发生选择/覆盖的 `self init`、用户主动查看的 `self config`、`self doctor`；`self env` 的路径来源是其报告字段。普通 build/run/search/help/version 不重复输出 home 重定向提示。

裸 force 仅在当前 home 来自 locator 时补充真实清理目标和入口保留说明；未经过重定向的裸 force 保持当前输出与行为。`--home` 保存成功时显示原始 home、真实 home 和入口位置；不再沿用前版“保存后被同一个 MCPP_HOME 遮蔽”的提示，因为该环境变量决定的正是本次入口。相关说明只在设置或主动诊断时显示，其他命令不反复提示。

```text
        Home D:\mcpp
      Origin C:\Users\alice\.mcpp\home.toml
     Warning --force rebuilds the registry and clears initialization caches in D:\mcpp
             The home locator and previous home data are retained.
```

`self config` 和 `self doctor` 显示原始 home、真实 home、来源、locator 与保留的旧数据。两者相同时无需重复两行相同路径；重定向时明确区分。只有经检查仍存在可识别旧数据时显示 retained；不存在时不宣称有残留。若旧 home 被显式列为 upstream，显示“作为上游复用”，不显示“未使用”。不扫描磁盘寻找未记录的历史 home，不把保留数据算作健康错误。

## 4. Windows 首次交互

该交互选择仅用于 Windows。Linux 和 macOS 不显示目录选择菜单，沿用原始 home 或显式 `--home`。Windows 上也只有显式 `self init`、未提供 `--home`、没有已初始化当前 home、没有已保存 locator、无显式 `MCPP_HOME`、不在自包含模式，且 stdin 与提示输出终端均可交互时才显示菜单。普通命令的懒初始化不弹菜单，保存过但不完整的初始化继续使用已选目标。

检测 D 为存在且可写的本地固定磁盘。使用平台驱动器类型 API，排除网络盘、可移动盘、光驱与未就绪盘；可写性用可清理的临时目录/文件验证，不能只看权限位。选择后仍验证 `D:\mcpp` 本身；D 根可写不代表既有 `D:\mcpp` 可用。路径由重解析点导向网络位置时不将其标为本地固定盘推荐目标。

符合条件时显示以下静态布局。`Choose` 与 `Select` 的首字母左对齐，标题与选项之间留一空行；选项保持内容列缩进。状态动词仍沿用现有宽度；提示、诊断输出到 stderr。该示例为目标输出，需在 PowerShell、cmd、Windows Terminal 和可识别的 Git Bash 终端验证。

```text
        Home No existing home found
       Drive D:\ is a writable local fixed drive

      Choose the mcpp home:

             1) D:\mcpp                  [default]
             2) C:\Users\alice\.mcpp

      Select [1/2, Enter=1]:
```

只接受去掉首尾空白后的 `1`、`2` 或空行。空行选 1；EOF 和 Ctrl+C 取消，不等同空行，不保存 locator。其他输入一次报错退出 2，不循环、不接受路径输入：

```text
       Error Invalid selection; expected 1 or 2
        Hint For a custom path, run: mcpp self init --home <path>
```

D 不合适时不显示菜单，使用现有用户目录默认。无终端或输入重定向时同样使用既有默认，不读输入、不自动选择 D。Git Bash 终端识别必须使用既有平台抽象并验证 mintty 场景；无法可靠识别时采用非交互路径，不能挂起。其他平台同一命令、同一 home/force/离线语义，只有 D 盘建议属于 Windows。

离线模式仍允许目录选择，选择后网络阶段明确 skipped。显式 `--home` 不产生任何菜单。

## 5. 初始化阶段与只读配置

将 `load_or_init` 的职责拆成读取/解析配置、本地创建布局、按能力 bootstrap。调用者显式选择允许的副作用，不能再由通用配置读取自动安装工具。

| 阶段 | 工作 | 网络规则 |
|---|---|---|
| Home | 解析来源，检查路径、冲突与交互选择 | 无 |
| Configure | 创建必要本地配置，原子保存默认 | 无 |
| Provider | 找到或从安装包/现有本地来源取得 xlings | 不为诊断临时下载 provider |
| Resolve | 识别本地工具、完整 payload 和可复用 upstream | 不以外网探测阻断本地复用 |
| Network | 仅对确实需要下载的资源做有界检查 | 离线跳过；资源已齐全则不主动探测 |
| Fetch/Install | 刷新必要索引、安装缺少的初始化工具 | 按资源回退、报告进展；支持取消 |
| Verify | 报告 ready 或具体缺失项 | 不偷偷重试下载 |

同一条命令内的资源需求可能逐步获知，不为提前显示检查而强迫先刷新索引。下一资源需要不同主机时，可复用新鲜结果或在该下载前增量探测。

当前 `xlings self init` 不能因为名称中有 init 就被当作已确认无网络的本地阶段。网络检查必须位于第一次可能获取索引的调用之前；优先由 provider 将纯布局创建与索引获取分开。在上游尚未分开前，把这个初始化调用整体视为潜在网络步骤，放在检查之后并受取消/超时约束，不能只拦住后面的工具安装。

`self env`、`self config`、`self doctor`、`toolchain list` 不自动 bootstrap；doctor 缺 provider 时报告缺失，其余本地检查继续。显式 `self init` 和真正需要工具的 build/run/toolchain install 才按能力获取。`self config --mirror` 应通过 provider 的无网络配置接口更新；若现接口会隐式初始化，需要修改提供者接口，不在 mcpp 中另造一套 provider 文件编辑规则。

## 6. 网络检查、选源与下载边界

### 6.1 端点与结果

网络模块报告“此资源端点可用”，不声称“互联网正常”。按实际候选索引/产物端点进行小请求，覆盖允许的重定向；不通过 ping、首页可达或地理位置推断下载可用。避免只用 HEAD 而误判拒绝 HEAD 的镜像；小 GET 也要限制读取量并可取消，不能下载完整工具链用于测试。

`auto` 下并行检查 GLOBAL/CN 候选，优先使用可服务该资源的候选；探测不能保证随后下载成功，实际下载仍需回退。已有显式 CN/GLOBAL 和自定义索引保留提供者当前策略，不静默改成 auto；支持 `self config --mirror auto` 恢复自动策略。显式 region 的失败回退沿用提供者契约，本轮不擅自改为“禁止其他域名”或“总访问其他域名”。

自定义/企业索引只检测自己的候选，不为了检查外网去访问无关公共站点。代理、证书及 TLS 验证沿用真实下载的设置，不关闭证书验证。输出是否使用代理及失败阶段，不输出带凭据 URL、代理秘密或完整环境变量。

结构化结果包含资源类别、region、脱敏 host、DNS/connect/TLS/HTTP/timeout/cancelled 分类、耗时和检测时间。失败不修改永久离线状态；正结果按资源与代理配置短期复用，不能让一个 GitHub 首页结果代表所有 release CDN。建议进程内正缓存 5 分钟、失败缓存 15 秒；本轮不新增持久网络缓存。

### 6.2 候选时间预算（review R5）

| 工作 | 候选默认 | 上限含义 |
|---|---|---|
| 初始化探测或 doctor 网络段 | 总计 5 秒，包含全部候选与重定向 | 共享 deadline，并行任务到期取消；不是每个端点各等 5 秒 |
| 探测单次连接 | 最多 3 秒，且不超过剩余总预算 | 需要覆盖 DNS 与连接阶段的实际等待 |
| 实际下载连接 | 8 秒/候选，受该操作预算约束 | 失败后尝试其他合法源 |
| 实际下载无有效字节进展 | 30 秒 | heartbeat、日志或 spinner 不刷新此计时 |
| 小型索引刷新 | 沿用当前总上限 120 秒 | 重试、退避、切源共享总预算 |
| 大型安装安全总上限 | 保留现有 3 小时作为最后保护 | 有效下载可持续；无法替代连接与无进展上限 |

探测 deadline 包含启动、DNS、代理、TLS 与重定向。只在等待线程外层返回而留下阻塞 DNS/子进程不满足有界要求。取消使用平台进程树/取消原语，并验证 Windows 无遗留 curl/git/xlings。

下载 byte progress 与接口 heartbeat 的区分需要 xlings 暴露。mcpp 不另造完整下载器；职责为调用策略、总预算、阶段输出、提供者错误映射和父进程兜底。如果 provider 缺少接口，先在 xlings 落地能力并更新最小兼容版本，再接入。不能把“传入一个 idle timeout”写成字节停滞检测已经完成。

直接安装路径不能继续丢弃决定网络行为的输出后只显示计时。优先统一到稳定的结构化进展接口；若大型包的直接路径仍需保留，也须获得字节/阶段/失败信息，不能无限等待后用另一安装接口重做同一失败下载。

### 6.3 输出与离线

拟议输出沿用现有动词列，不增加独立横幅或每条命令的网络公告：

```text
        Home D:\mcpp
   Configure Mirror policy: auto
    Provider xlings 2026.x.x (local)
     Network Checking GLOBAL / CN endpoints...
     Network GLOBAL timed out; CN reachable (186 ms)
      Source CN selected for package index
    Fetching Package index...
  Installing ninja...
       Ready D:\mcpp
```

“Source”是此资源的实际选择。下载中发生切源时报告对应原因和下一候选；初始探测失败但真实请求成功时显示真实结果。不把 CN 不可用归因为整机无网。

离线模式优先于探测、刷新、下载和 provider 自动联网。初始化、设置 locator 和本地复用仍可执行：

```text
        Home D:\mcpp
   Configure Local configuration ready
     Default Saved in C:\Users\alice\.mcpp\home.toml
     Network Skipped (offline mode)
     Missing ninja
       Error Initialization incomplete; the default home was saved
```

工具已齐全时离线 init 成功；缺少必需资源时退出 1。不因为探测失败自动切为离线模式，不从联网失败推断本地资源不可用。

Unix 安装脚本的连接、停滞、校验文件请求也补齐边界；保留 `MCPP_PREFIX`。程序安装位置、xlings 自身 home 与 mcpp 数据 home 分开说明，设置 mcpp locator 不移动前两者。安装器获取脚本本身的网络问题属于其上游调用，不宣称 mcpp 初始化检查可以覆盖。

## 7. doctor 行为

doctor 先做本地检查，不调用安装或修复流程；`--offline` 完全跳过网络段。普通 doctor 默认做一次共享 5 秒预算的资源端点诊断，网络失败作为 warning，其余本地结果照常完成。已存在的本地错误继续影响退出状态，不能被网络 warning 覆盖。

5 秒约束专指网络段，不是 doctor 总耗时保证。provider 查询、编译器信息、项目只读规划也须有各自的子进程约束；本次本地 0.615 秒不能推广为所有环境都在一秒内完成。

home 来源、locator、旧数据保留、代理是否启用和检测端点归入诊断结果。新增结构化字段若进入现有机器协议，按对应协议演进规则处理，不私自改变已发布 envelope。

## 8. 实现顺序与修改边界

1. 先使配置读取、mirror 修改和诊断不执行 bootstrap，补充缺 provider 的只读降级。
2. 为 home 建立两阶段、带来源的解析结果、原始 home 下的 locator、单跳规则、冲突保护和原子更新；普通命令不增加提示。
3. 接入 `self init --home`，区分裸 force 与显式目标 force；验证环境变量、自包含与 `--system` 边界。
4. 在 `modules/platform` 增加/复用驱动器和输入终端能力，接入两项菜单；CLI 层组织布局，不在 home leaf 层读取交互输入。
5. 先完善 xlings 的阶段、下载进展、连接/停滞取消能力，再接入初始化 preflight 与 doctor 网络段；保留现有索引与大包预算含义。
6. 用户文档与 help 在功能落地时同步中英文更新；此前仅保留本设计记录。

### 8.1 架构约束

一次调用只有一个已解析的 home 上下文，包含 `originalHome`、原始来源、`effectiveHome`、真实来源和 locator 路径。先完成 CLI 参数解析，再完成 home 选择，之后配置、provider、缓存、日志和 force 清理共享该结果。查询模式与修改模式复用路径解析，分别决定是否允许写入或联网。

`home::root()` 对既有数据消费者继续表示真实使用的 home。原始 home 只服务入口读写、来源报告和旧数据说明；不允许 toolchain、缓存各自重新解析环境变量。程序随附资源从实际安装位置获取，其安装位置不因数据 home 重定向而改变。provider 仅接收已经确定的真实 registry 路径，不再次解释 locator。

路径选择与本地 locator 读写归 home 子系统；平台层只提供文件系统、驱动器、终端与取消能力；CLI 层组织菜单和提示；provider 负责下载/选源事实，mcpp 组织阶段和预算。原始/真实两个概念是同一次解析的输入与输出，不是两套相互同步的全局配置。

## 9. 验证矩阵与通过条件

临时 PR 的现状探针只回答现状问题。后续实现不能以该 PR 绿色作为功能验收。

| 领域 | 需要验证的行为 | 环境/证据 |
|---|---|---|
| 路径兼容 | 原始 home 的 env、自包含、dev/xpkgs 排除、fallback 均保持；无重定向时原始=真实；无全局新提示 | Linux/macOS/Windows，解析单测与真实 CLI |
| locator | 重启仍生效；只跳一次；只读所选原始 home 的入口；坏目标拒绝；显式 force-home 可修复坏入口 | 三平台隔离入口；验证 env/portable 均不读另一套 ~/.mcpp 入口 |
| 路径身份 | 相对路径、空格、Unicode、大小写别名、junction/symlink、切回入口 | Windows 非 UTF-8 ACP 路径也要覆盖；失败有明确错误 |
| 切换事务 | 同路径幂等；不同路径需 force；两边 sentinel 保留；写入失败保留旧 locator；并发切换无半文件 | 可控故障注入与真实文件系统 |
| force | 裸 force 与当前清理范围一致；重定向时只清理解析目标；显式 `--force --home` 不删除 | 三平台目录 sentinel，不用生产 home |
| 菜单 | 1/2/空行/非法输入/EOF/Ctrl+C；无终端不等待；无 D/非固定/不可写 D 不推荐；Linux/macOS 永不显示 | Windows 原生控制台/ConPTY 与 Git Bash；D 探测以可注入平台事实补全反例 |
| 输出 | 相同动词列、详情对齐；窄终端长路径不破坏结构；相关操作才输出提示 | 真实终端/PTY transcript，不以管道输入冒充交互验证 |
| 离线 | CLI flag 和 env 两入口；无 preflight/refresh/download；本地复用不依赖外网 | 控制网络调用记录，必要时操作系统级拦截；冷/暖 home 对照 |
| 故障网络 | DNS、代理拒绝、TLS、403/404、重定向、握手后停滞、持续小流量、CN/GLOBAL 部分故障 | 可控本地 HTTP/TLS/代理端点与少量真实资源检查 |
| deadline | 并行候选共享预算；heartbeat 不掩盖停滞；重试不重置总预算；取消无孤儿 | 三平台测 elapsed 和子进程存活；不只断言返回码 |
| doctor/config | 冷 home 无 bootstrap；provider 缺失仍报告本地结果；offline 跳过；网络失败有界 | 真正隔离 home、完整日志和后续检查已执行证据 |
| 大包 | 有进展的慢下载不被 5 秒探测预算截断；停滞及时切源/失败 | 字节进展事实；有限大小的可控流模拟，真实大包抽验 |

网络时间测试允许平台调度余量，但余量不能成为隐藏的第二个长超时；建议探测 5 秒加最多 2 秒终止清理余量。Windows D 盘存在的一条正例不能代替无 D/不可写等反例。跨平台当前只有发布版探针，不声称 PowerShell 交互已验证。

## 10. 同轮独立缺陷

### 10.1 macOS benchmark 比例判断

[原 CI job](https://github.com/mcpp-community/mcpp/actions/runs/39129581254/job/117682786534) 报告 116 passed、1 failed、171 skipped，唯一失败为 `230_bench_harness.sh`。测得 cold 2.70 秒、noop 1.36 秒；`bench/src/main.cpp` 因 `cold < 2 * noop` 将其判为 clean 未生效。cold 日志显示执行构建，但仅凭该日志也不替代产物级清理验证。

设计方向：用产物删除与重新生成、实际编译事件作为测试断言；耗时比例作为可疑提示，不能单凭比例报确定的清理失败。不以调低阈值或增大 fixture 作为根治。修复独立提交，不属于本设计功能 PR。

### 10.2 #816 的头文件传递 import

[#816](https://github.com/mcpp-community/mcpp/issues/816) 的关键形状是 `main.cpp → #include helper.h → import mylib → import dependency`。`plan.cppm` 已沿 `cu.imports` 构建传递闭包；需要验证源扫描未看到头文件 import 时，编译器 P1689/dyndep 后发现的模块如何获得正确 map。

临时 PR 在 macOS 用同一 local path dependency 比较直接 import、源码中传递 import、头文件中传递 import，保存构建日志、CDB 和 build.ninja。它不依赖 Godot/Quill 网络下载，但仍只是一个工具链版本的诊断，不替代原包的最终回归。

[最终模块对照 job](https://github.com/mcpp-community/mcpp/actions/runs/39184402622/job/117843665715) 使用 LLVM 23.1.3 与 release mcpp 2026.10.17.1，得到：

| main 的形状 | build 结果 | CDB 中 main 的模块映射 |
|---|---|---|
| `import depmod` | rc 0，1.785 秒 | 有 `.modmap` |
| `import mylib`，mylib 再 import depmod | rc 0，1.408 秒 | 有 `.modmap` |
| `#include helper.h`，头文件 import mylib | rc 1，1.240 秒 | 无 `.modmap`，只有 flat prebuilt module path |

第三项原始错误为 `helper.h:2:1: error: failed to find module file for module 'depmod'`。这将问题收窄至头文件引入 import 时的映射覆盖；源码中明确 import 的传递闭包在本对照中成立。该失败是被观察的产品缺陷，模块 job 的绿色仅保证直接 import 对照成功并完成证据采集，不把第三项当成产品通过。

实现方向：使实际编译读取的模块映射与编译器发现的依赖一致，同时维持包可见性、BMI 构建顺序、缓存 staging 和 workspace 稳定性。不通过扁平复制/符号链接依赖 BMI 掩盖缺失映射，也不无条件将所有包模块暴露给所有编译单元。

## 11. 决策状态与本轮 review 要点

| 编号 | 状态 | 方案及需要核对的细节 |
|---|---|---|
| R1 | 按最新反馈修订 | 原始 home 先按 env/portable/fallback 选定；真实 home 再按 --home/原始入口/原始目录选定。`MCPP_HOME=D` 后显式选择 E，写 D/home.toml；不写另一套 ~/.mcpp，不改环境变量。只跳一级。 |
| R2 | 用户已认可 | 裸 force 保持破坏性；显式 --force --home 即使同路径也不清空数据。仅 locator 来源的裸 force 增加目标提示；本轮不增 reset。 |
| R3 | 用户已认可 | 一个 locator 文件，可附 previous_home；只保存上一持久默认，旧目录存在才报告，不扫描历史、不自动清理。 |
| R4 | 用户已认可 | 本地验证成功即保存默认，先于联网。下载失败/离线缺工具时默认仍已切换，命令返回 1；保存失败则旧指向保留，并说明目标本地文件可能已建立。 |
| R5 | 用户已认可 | 探测共享 5 秒；下载连接 8 秒、无字节进展 30 秒。doctor 网络失败只警告；大包保留 3 小时最终保护；provider 须暴露真实进展。 |
| R6 | 按最新反馈修订 | 仅 Windows 首次显式 init 有两项菜单；Choose 与 Select 左对齐，标题与选项隔一空行；空行选 D。非法输入退出 2，EOF/Ctrl+C 取消；其他平台无菜单。 |
| R7 | 用户已认可 | human env/config/doctor/list 不自动安装；相关现有 E2E 按新契约更新；离线跳过探测和下载。 |

本轮额外列出的边界：保存或覆盖入口时原始 home 不可写则显式报错，不偷偷换入口，普通读取不要求可写；坏 locator 可通过 `--force --home` 修复；切回原始目录删除 locator；原始与目标不能互相包含。最后一项是避免目录归属歧义的保守约束，需 review 是否接受，暂不承诺支持嵌套 home。

## 12. 文档复核

2026-10-19 完成结果回填后对前版进行了第二遍成文复核。以下保留当时结论；本轮两阶段模型修订以第 12.1 节为准。

| 维度 | 结论与位置 |
|---|---|
| 读者、归属与梯度 | 通过；开头明确实现/review 读者，提案只写在设计树，链接现有参考与实现 |
| 渐进性与直观 | 通过；第 1 节先给事实和实测，再给第 2—7 节行为；可粘贴的新命令及输出已由状态行统一标为设计 |
| 覆盖度与可查阅性 | 通过；第 3 节有命令矩阵，第 9 节列出反例、平台与证据，第 11 节逐项给出 review 决策 |
| 断言与证据 | 通过；release/main SHA 分开；第 1.2 节说明故障注入范围；第 10.2 节区分 job 绿色与被观察的产品失败 |
| 信息密度与易读 | 通过；表格比较语义、时间预算与验证范围，说明段落解释因果；Windows 输出集中在第 4 节 |
| 推荐路径与增量状态 | 通过；固定入口文件和单跳为唯一推荐方案，无其他并列持久化机制；未实现内容不写入用户手册 |
| 旧行为与提示范围 | 通过；第 2.1、3.1、3.3 节保留 env/portable/force，限定提示，不向普通命令增加提醒 |
| 当前边界 | 通过；第 1.2、9 节明确新功能未验收、原生菜单未测试、故障注入不覆盖真实 CN 网络 |

`bash .github/tools/check_docs_style.sh` 通过；该脚本作用域主要为用户文档，不能代替上表对本设计的人工复核。设计文件链接和代码块结构另行核对。尚未实现产品功能，因此不运行或声称完成其功能回归。

### 12.1 本轮多视角自我 review

本轮按更新后的整篇成文重新进行自我 review；已有发布版探针只作为现状证据。

| 视角 | 发现与修订 | 复核结论 |
|---|---|---|
| 模型一致性 | 前版混排 --home 与 MCPP_HOME，导致保存位置被当前 env 遮蔽；第 2 节改为原始/真实两阶段，入口随原始 home | 已消除前版矛盾；R1 的具体例子供本轮核对 |
| 默认与兼容 | 未设置重定向时两者相同；env 与 portable 不读取另一用户入口，已有排除规则保留 | 设计成立；需后续三平台回归验证 |
| 架构与简洁性 | 防止将 MCPP_HOME 改写为真实路径后再读目标 locator；第 8.1 节规定单一上下文、单次解析，数据消费者只用真实 home | 只增加入口选择职责，不建立两套运行配置 |
| Windows UX | Choose 与 Select 对齐并增加标题空行；菜单严格限 Windows，已保存但未完成初始化不再弹窗 | 文案与状态规则一致；原生终端布局仍待实现验证 |
| Linux/macOS UX | 不引入目录菜单；显式 --home 与错误/离线语义和 Windows 一致 | 不把 Windows 盘符推荐推广为跨平台默认行为 |
| 文件事务与失败恢复 | 补充入口写操作的可写性预检、普通读取不要求写权限、保存失败不删目标、坏 locator 的显式修复、切回入口的删除语义 | 失败不会悄悄启用另一个 home；嵌套目录约束列为边界 review |
| 自包含与资源归属 | 原始入口可保留程序与旧数据；重定向仅改变真实数据位置，程序随附资源按安装位置查找 | 不把自包含二进制移走，也不把其安装资源误认成新 home 已下载内容 |
| 网络与离线 | 入口与真实位置解析不触发探测；首次可能联网的 provider init 也受阶段与预算约束；heartbeat 不算下载进展 | 已认可预算保留；provider 能力仍为实施前置依赖 |
| 输出与可诊断性 | 修正旧的遮蔽提示；仅 init 设置操作和主动 config/doctor 报告双路径，普通命令不重复 | 与用户限定的提示范围一致 |
| 验证与证据 | 第 9 节新增原始/真实矩阵、坏入口修复和非 Windows 无菜单；历史 Actions 只保留现状事实 | 没有宣称修订后的 locator 或交互已通过 CI |

待实现验证与待 review 的语义边界分开列出；本轮为设计修订，无产品代码变更，无需重跑原样的现状探针。

### 12.2 实施检查点：home 与只读配置

已在功能分支开始 T1—T3：两阶段 home 解析、单跳 TOML 入口、显式 force-home 非删除语义、Windows 两项菜单、初始化策略拆分、provider 内的本地 mirror 更新，以及相关来源报告。T4 网络检查与 provider 下载边界尚未完成，不能将此检查点视为最终实现。

本地 GCC 16.1 自举构建成功；聚焦 `mcpp --offline test home` 完成，13 个用例通过（`/tmp/mcpp-first-run-home-tests.log`），覆盖既有路径与缓存规则、单跳、坏入口修复、强制切换数据保留、返回原始位置、嵌套目录拒绝及只读配置不写入。真实 CLI 首轮发现 TOML 字符串重复加引号，已修正；对外 CLI 必须重新链接后再验收，不以单测通过替代实际二进制读回。

当前 provider 边界、模块 wiring、文件长度与版本 pin 检查通过。尚未运行当前功能 head 的跨平台 CI，也未验证 Windows 原生交互。xlings（provider）的连接超时仍为 30 秒硬编码，已有 low-speed 看门狗；T4 要求补齐可配置连接预算和结构化探测，不用旁路下载器冒充 provider 已支持。

本检查点重新链接后的真实 CLI 验证已通过：`919_a_home_selection_is_remembered_once.sh`；既有 `38_self_config_mirror.sh`、`46_self_config_mirror_no_bootstrap.sh`、`604_offline_skips_sandbox_bootstrap.sh` 按无隐式 bootstrap 的契约更新后通过。它们只证明 Linux 当前基础行为，不替代 Windows 原生交互、网络 T4 或最终全 CI。


### 12.3 实施检查点：有界网络诊断与平台选择

mcpp 功能草稿为 [PR #824](https://github.com/mcpp-community/mcpp/pull/824)，提供者前置能力为
[xlings PR #684](https://github.com/openxlings/xlings/pull/684)。后者新增本地端点发现、
复用下载传输层的资源探测、可缩短的连接/读取预算、实际来源和候选尝试信息。
mcpp 通过 provider port 获取结构化结果，端点发现与并发子进程共享 5 秒期限。
诊断不建立第二套下载器，不根据一次探测永久改写 mirror 配置。

本地 Linux 已有以下证据：

| 验证 | 结果 | 边界 |
|---|---|---|
| provider 首轮网络用例 | 5 个通过，随后补实际候选尝试用例后 6 个通过 | 真实 loopback GET/404、拒绝代理、无凭据输出、字节进展与总预算；不是完整公网故障矩阵 |
| 旧 provider 对照 | 3 个资源诊断断言因缺少新 capability 失败 | 旧版 2026.10.18.1；证明新断言有区分度 |
| mcpp 网络与进程用例 | 5 个通过；连续 heartbeat 输出下仍在约 225 ms 被测试期限停止 | POSIX 探测 fixture；输出期间取消的进程用例待 Windows CI 复核 |
| 显式迁移与 home E2E | 10、11、39、151、681、687、846、916、919 全部通过 | Linux 实际重新链接的 CLI；保留文件状态断言 |
| 文档与层次检查 | 文档样式、干净 worktree 文档结构、provider 边界、模块 wiring 通过 | 原生 Windows 交互仍需实际 runner 证据 |

对下载链的补充 review 发现：socket timeout 不能覆盖所有 DNS/响应头等待，interface heartbeat
也不能代表字节进展。因此父进程增加进展期限轮询，由平台进程封装在持续输出时仍检查并清理
整个子进程树。只增加字节数或真正切换候选才能更新进展时间；完成后的解压/配置阶段不沿用
活动下载的 idle 计时。该层补充不替代 provider 的来源回退与 socket 预算。

本轮新增的可移植平台选择采用 `if constexpr (mcpp::platform::is_windows)`。
stdin 终端检测由 facade 分派到对应平台绑定；条件编译仅留在系统头和 syscall 绑定边界。
Windows 原生控制台检查通过独立 console 输入/屏幕缓冲驱动真实 CLI，检查 Choose/Select
同列、标题空行、非法输入退出、选择 2 与 Enter 默认选择 D。该脚本仅用于可丢弃的 hosted
Windows runner；已有 D:\mcpp 时拒绝测试，禁止清理未知 home。

基础 CI 已暴露两类需要修正的既有断言：路径规范化后的 /private/var 与 /var（Windows 同类
拼写差异），以及把 self env 当作初始化入口的 E2E。前者改用目录身份比较，并由原始解析来源
直接报告 user/portable；后者改为显式初始化或本地配置修改。最终 head 的全 CI 尚未完成，
provider 尚未发布和更新 floor，当前不能宣称 T4—T8 完成，未合入也未发布 mcpp。


### 12.4 补充 review：首次请求、只读入口与安装器

安装接口的总保护不能只依赖第一个 download_progress 事件：首次索引获取可能发生在它之前。
统一的安装期限状态同时覆盖结构化安装入口与 bootstrap 入口：前置资源阶段 120 秒；活动下载
按字节与候选变化判断 30 秒无进展；进入解压、hook 或配置后交还本地阶段与最终进程期限。
超时保留先前已报告的本地错误，网络超时作为 E_NETWORK 返回，禁止再通过另一个前端重复下载。

端点查询前先在同一个 5 秒预算内读取 provider handshake。旧 provider 不支持诊断时停止在握手，
避免未知 capability 被其通用命令入口当成写操作。真实 CLI 联调也暴露 provider 自身会为新查询
创建 operation journal；前置 PR 增加只读登记，并跳过这些 interface 读取的旧 home 采用和 profile
升级，加入目录与文件内容不变的对照。此补丁仍待最终 CI，不把此前 6 个网络用例当成新断言已通过。

独立 install.sh 继续使用其原有 curl 传输，补齐两侧镜像、版本元数据和校验文件的等待保护。
连接 8 秒；元数据总上限 120 秒；压缩包最终上限 3 小时。curl 的最小非零 low-speed 阈值为
1 B/s，窗口 30 秒，因此安装器的低速保护与 mcpp/provider 的严格字节 idle 判断并不等同；
不能用 curl 参数测试替代 provider 连续字节进展的验收。
