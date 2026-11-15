---
subject: cross-platform
status: active
---

# 生态缺口补充方案：一个 C 程序走遍全部目标所暴露的通用问题

**日期：** 2026-11-15 · **版本：** v9（实施前评审） · **基线：** mcpp 2026.11.15.1、openkal-platform 0.28.0、
djgpp-platform 0.1.0、mcpp:plugins 0.20.0、dirun 0.3.2、xlings v2026.10.26.1

## 1. 来源与方法

验证工程是一个 C89 命令解释器（两个程序，约 74 个源文件，自带 System V 可移植层）。它接入 mcpp 只靠清单、
一个构建程序、一个页面模板与少量 `#if` 守卫，构建 22 个目标；凡有运行环境者，以同一批处理脚本比对输出。运行
环境：本机（qemu-user、wine、node、okrun、DOSBox、QEMU+OVMF、FreeBSD VM、headless Chromium），GitHub 托管机
（Linux x86_64/arm64、macOS arm64/x86_64、Windows x64/arm64），模拟器（FreeBSD aarch64、Android x86_64）。

编号：G 为 2026-11-11 的缺口，N 为本轮，X 为 xlings 与 xim，D 为 DJGPP，R 为运行器的引擎项，W 为 web，A 为
运行器架构的缺口（第 10 节）。任务与检查点用小写的仓库缩写加序号（第 7 节），与问题编号不重名。

## 2. 原则：基础设施与项目的边界

mcpp、openkal、xlings/xim 的包、dirun 与 mcpp:plugins 是通用基础设施。一项修复放在哪一层，按下表判断：

| 层 | 放什么 | 位置 | 例 |
|---|---|---|---|
| 引擎 | 机制：对所有程序、所有行成立的行为 | mcpp、xlings 本体 | 启动器的系统名映射（N20）、构建依赖的版本统一（N29） |
| 数据 | 事实：某个系统、某个行、某个工具的知识 | builtin.toml、平台包 | wine 运行 Windows 程序、Emscripten 的异常参数、哪些主机能执行哪些架构 |
| 实现 | 一个系统上 openkal 接口的实现 | openkal-* | UEFI 的文件系统、macOS 的 `__bzero` |
| 运行器 | 怎样把程序交给一台机器 | dirun 与随包的配置 | 模拟器的启动、文件的交付、状态的取回 |
| 插件 | 一种分发格式的组装与呈现 | mcpp:plugins | web 目录、以格式命名的 runner |
| 包 | 工具与镜像的获取，以及随包的配置 | xim-pkgindex、mcpp-index | OVMF 经 Shell 传参、Android 镜像与其客体配置 |
| 工程 | 一个工程自己的选择 | 清单、构建程序、模板、CI | 宏定义、页面模板、测试脚本、开启哪些运行器 |

基础设施只预留扩展点与配置项，不为某个工程写特例；引擎不出现任何系统名、工具名与运行器名（AGENTS.md §2）；
新增的键沿用已有的名字与形状（“一个概念一种拼写”）；每个缺省值是数据，并有四层覆盖（SPEC-004 §2）。
验证工程中属于工程自身选择的内容（宏、页面模板、测试脚本、对 dirun 的直接调用）留在工程中，不进入本方案。

## 3. 结论

1. 构建路径已经足够：22 个目标全部构建；主流系统与模拟器上的运行与基准一致（含 Windows arm64）；固件与
   openkal DOS 能启动；浏览器版在 Chrome 中运行。
2. 剩余问题多数不需要改 mcpp 或 xlings 本体，而在运行器、平台数据、openkal 实现、dirun、xim 包与插件中修复。
   需要改 mcpp 本体的 11 项在一个 PR 中完成，以 commit 为检查点（第 5 节）。xlings 本体 2 项。
3. 能放在一个 PR 的放在一个 PR；外部前置只约束检查点与合并的时机，不是拆 PR 的理由；只有内容要等
   **本仓库自己的发布**、或仓库本身是发布渠道时才拆开（第 7 节）：xim-pkgindex 3 个，其余每个仓库 1 个，mcpp 也是 1 个。
4. v8 依据源码与实测修正了 v7 的运行器规范：mcpp 已有的进程模型足够，不新增信号转发与宽限期（第 10.4 节）。

## 4. 问题清单

“层”即第 2 节的各行；“文档”指只需写明现有行为。

| ID | 现象 | 根因（已核对） | 层 · 归属 |
|---|---|---|---|
| N18 | `x86_64-macos-musl` 发布构建链接失败：`__bzero` 未定义 | clang 在 x86_64 Darwin 上把清零降为 `__bzero`，openkal 的 C 库不提供 | 实现 · openkal-macos |
| N14 | UEFI 程序收不到参数 | 平台的 UEFI runner 命令缺 `--`；ovmf 配置把程序放作 `BOOTX64.EFI` 由固件直启，没有加载参数。**已实测**：不放 `BOOTX64.EFI` 时 OVMF 进入内置 Shell，执行 `startup.nsh`，参数到达程序，状态经 debug-exit 取回（Shell 有 5 秒倒计时） | 数据 · openkal-platform、ovmf 包 |
| N34 | UEFI 程序读不到任何文件：`cd` 为 `/`，`dir` 为 File Not Found | openkal-uefi 未实现 `fs`（其 README 写明 `fs` 待 SIMPLE_FILE_SYSTEM） | 实现 · openkal-uefi |
| N35 | UEFI 参数含引号时切分错误 | openkal-uefi 按空格切分 load options；由 Shell 启动时，Shell 已给出 `EFI_SHELL_PARAMETERS_PROTOCOL` 的 argv | 实现 · openkal-uefi |
| N15 | UEFI 上 mimalloc 警告无安全随机源 | 未接 EFI_RNG_PROTOCOL | 实现 · openkal-uefi |
| N19 | okrun 打不开含 `//` 的路径 | 打开映像前未规范化 | 实现 · openkal-image |
| N12 | dirun 0.3.2 携带的 okrun 落后，仍有 `../` 错误 | dirun 自带一份 okrun 副本 | 运行器 · dirun |
| N13 | DOS 运行器只取回标准输出 | DOSBox 的 shell 无 `2>`；dirun 只有 `output-file` | 运行器 · dirun、dosbox-staging 包 |
| N25 | `mcpp run` 不能把数据文件交给机器类运行器 | 运行协议（docs/41）已规定：搬动产物的运行器复制 `MCPP_RUNTIME_FILES` 所列文件；dirun 未实现 | 运行器 · dirun |
| N28 | `--systems all` 的包缺 macos-x86_64 的 okrun | 平台 runner 的 `hosts`、`built-as` 未列；受 N18 阻挡 | 数据 · openkal-platform |
| N10 | FreeBSD 上 musl 身份的程序取系统名得 `openkal` | hosted 安排报实现名 | 实现 · openkal-musl / openkal-freebsd |
| N17 | openkal DOS 行上 `if … goto` 结束批处理 | 未定位 | 实现 · openkal-dos（技术实验） |
| N22 | 按 `pack_format()` 切换链接参数不生效 | 第二遍按设计不重链；插件文档未说明 | 插件 · mcpp:plugins |
| N31 | 用户打开 `<out_dir>/<target>-index.html`，页面加载不到脚本 | dist-web 的中间页面写在产物目录旁，名字像成品 | 插件 · mcpp:plugins |
| N32 | 从磁盘打开 web 页面，浏览器拒绝取 `.wasm`；pack 不说明产物如何使用 | 浏览器规则；插件未提示 | 插件 · mcpp:plugins（提示与文档） |
| N36 | `mcpp run --pkg-format web` 拒绝：“produced 3 distributables” | dist-web 把每个复制的文件都声明为终端产物；引擎以“未被其他动作消费的输出”为分发物 | 插件 · mcpp:plugins |
| N21 | profile 的 `ldflags` 排在目标块之前 | 顺序未写入文档 | 文档 · mcpp |
| N27 | Emscripten 的 `tcsetattr` 无原始模式 | Emscripten 的行为 | 文档 · mcpp |
| G6 | `mcpp run --target *-freebsd` 只能拒绝 | xim 已有 freebsd-disk-image 14.5（x86_64）；缺 runner 数据与 aarch64 镜像 | 数据、包 |
| G7 | `mcpp run --target *-android` 只能拒绝 | xim 已有模拟器、platform-tools、API 24 镜像；dirun 不能经命令到达客体 | 运行器 · dirun；包；数据 · builtin.toml |
| X1 | Windows 行在 Linux 上需工程自写 `runner = ["wine"]` | xim 已有 wine 11.0；缺 runner 数据 | 数据 · builtin.toml、openkal-platform |
| X4 | openkal-image 0.6.3 的 release 没有二进制；xim 停在 0.6.2 | 发布流程未附产物 | 包 · openkal-image 发布 |
| D1 | DJGPP 的 GCC 为 12.2，无模块 | 上游无 GCC 14 预编译包（build-djgpp 止于 12.2，build-gcc 只有脚本） | 包 · xim（自建） |
| N20 | 打包的 POSIX 启动器以 `uname -s` 原样拼目录，FreeBSD 上找不到包内 okrun | 未小写 | **引擎** |
| N9 | `artifact_name("exe","{name}")` 各行结果不一（`x.exe.exe`、`x.exe`、DJGPP 两份） | B2 规则未统一应用 | **引擎** |
| N16 | `[resources]` 作用于 UEFI 这一 PE 行 | 判据取文件格式而非执行系统 | **引擎** |
| N29 | 工程要 mcpp:plugins 0.21.0、平台要 0.20.0，结果以误导性错误失败 | 同一包的两个版本请求未识别为冲突 | **引擎** |
| N30 | pack 未指名时取第一个程序；插件不提交的原因不进入错误 | 选取与插件的 `target` 互不知晓 | **引擎** |
| N11 | `--all-programs` 只为入口程序生成启动器 | — | **引擎** |
| N23 | i686 映像的载体不构建；`mcpp run` 在 x86_64 上拒绝 i386 映像 | 载体构建未继承平台声明；`same-arch` 不含“能执行该架构”的主机 | **引擎**（执行关系作为数据） |
| N26 | Emscripten 行上 C++ 异常默认不可捕获 | 行未开启 wasm 异常 | **引擎**（行数据与 std 模块参数） |
| N33 | web profile 加 `-sSINGLE_FILE=1` 后，干净构建的打包失败；旧的 `.wasm` 存在时被当作产物打包 | `<name>.wasm` 被声明为无条件的隐式输出；最终错误只说 “no action claimed” | **引擎** |
| R1 | 首次使用代价大的运行器（VM、模拟器、wine）一旦写成数据，`mcpp run` 将在首次运行时下载 GB 级组件或在家目录留下状态 | 决策：默认关闭，可配置（第 5 节） | **引擎**（一个键） |
| R2 | runner 在数据与构建程序中字段不对称；不在包的 `bin/` 下的程序没有可移植的写法；Windows 上脚本型程序的规则未成文（第 10 节 A1–A5） | 两种表达各自生长 | **引擎**（模型与规范） |
| W1 | `mcpp pack --pkg-format web` 的产物没有“打开即可用”的方式 | 页面须经 HTTP 提供；没有以格式命名的 runner | 插件 · mcpp:plugins |
| X2 | Windows 上 `write_file_atomic` 单次 `MoveFileExW` | openxlings/xlings#715 | **xlings** |
| X5 | Windows 上经 xlings 的 shim 启动程序，参数含 `"` 与 `&` 时被拆开，其后的内容被当作命令执行 | shim 经命令解释器转交参数（实测）；影响所有经 shim 启动的工具，含 mcpp | **xlings** |
| X3 | subos 的 `cc` 与主机库错配（`__isoc23_strtol`、`libc.so`） | 已定位：xim `musl` 配方把 11 个与 glibc 同名的链接文件（`crt1.o`、`libc.so` 等）注册进共享 `lib/`，安装 musl 即改变 `cc` 的链接 | xim-pkgindex#1162（已合并）：共享 `lib/` 只留加载器 `ld-musl-<arch>.so.1`；链接文件入 `usr/lib/musl/`，头文件在 `usr/include/musl/`；修订号 2 修复已有 home |

已解决、仅留档：G1、G2、G3、G8。

## 5. 需要改 mcpp 本体

| ID | 修改 | 机制与数据的分界 |
|---|---|---|
| N20 | 启动器把 `uname -s` 小写后映射包内目录；未知系统列出包内系统后退出，不退到 PATH | 机制 |
| N9 | 统一应用 B2：平台后缀只在平台自身步骤读取时保留；行定义的普通后缀让位于工程命名；DJGPP 只产一份 | 机制 |
| N16 | `[resources]` 适用于执行系统为 Windows 的行 | 判据读行数据的 `execution` |
| N29 | 同一构建中同一包的多个版本请求：可统一时取满足各方的版本，否则报冲突并列出请求者 | 机制 |
| N30 | 插件声明它服务的程序时，pack 据此选取；插件不提交的原因并入引擎错误 | 机制；形状见 5.2 |
| N11 | `--all-programs` 为每个程序生成启动器 | 机制 |
| N23 | 载体构建继承工程的平台声明；“哪个主机能执行哪些架构”写入 builtin.toml | 关系是数据，键为 `<os>-<arch>`；形状见 5.2 |
| N26 | wasm32-emscripten 行含 C++ 时启用 wasm 异常，编译、链接与 std 模块一致 | 参数写在行数据中；不新增开关，工程以已有的 flags 覆盖 |
| N33 | 行数据把 `.wasm` 声明为**可选**的同名产物：链接前清除旧文件，存在则暂存；插件不提交时，暂存阶段的警告成为错误的原因 | 同名产物表是数据；形状见 5.2 |
| R1 | runner 的 `enabled`（5.1） | 一个键，四层覆盖 |
| R2 | 运行器的统一模型与规范（第 10 节） | 字段与名字沿用已有的；旧调用保持有效 |

### 5.1 R1：`enabled`

- **键。** runner 表（默认与具名）增加 `enabled`，布尔，缺省为真。为假的 runner 存在但不被使用：`mcpp run` 与
  `mcpp test` 同样拒绝，说明所需的包与开启的写法，reason token 为 `runner-not-enabled`，退出状态属 docs/50
  已有的“拒绝”一类。
- **它属于哪条陈述。** `enabled` 与 `requires` 一样是“这条命令”的性质：一层替换了 `cmd`，就是一条新的陈述，
  `enabled` 回到缺省（SPEC-013 §10 对 `requires` 的现有规则）。所以已经写了 `runner = ["wine", "{}"]` 的工程
  升级后行为不变。只调整不替换时，覆盖层写不带 `cmd` 的表：`runner = { enabled = true }`（现有读取器要求
  `cmd` 非空，覆盖层放宽为可省；提供者与平台仍必须给出 `cmd`）。
- **四层覆盖（SPEC-004 §2）。** 引擎缺省为真；提供者或平台的数据标 `enabled = false`；本机 `config.toml`、
  工程 `mcpp.toml` 以 `[target.<sel>] runner = { enabled = true }` 开启；一次性的覆盖用环境变量
  `MCPP_TARGET_<TRIPLE>_RUNNER_ENABLED`，与已有的 `MCPP_TARGET_<TRIPLE>_RUNNER`、`_SYSROOT`、`_RUN_UNDER` 同一
  构词，不新增命令行开关。
- **显式指名即同意。** `mcpp run --runner <name>` 选中一个未开启的具名 runner 时，本次运行视为开启。
  `--pkg-format <f>` 找到的同名 runner 不算指名。
- **何时标为关闭（写入 SPEC-016，给数据作者的判据）。** 首次使用要取得整机镜像或模拟器，或在构建树与包仓库
  之外留下状态。按此：VM、Android 模拟器关闭；wine 关闭（在家目录建立 prefix）；qemu-user、okrun、node 不变。

```
error: x86_64-freebsd runs through a runner that is not enabled.
  it needs: xim:dirun@0.4.0, xim:freebsd-disk-image@14.5 (installed on first use)
  enable:   [target.x86_64-freebsd] runner = { enabled = true }   # mcpp.toml, or config.toml for this machine
  once:     MCPP_TARGET_X86_64_FREEBSD_RUNNER_ENABLED=true mcpp run --target x86_64-freebsd
```

### 5.2 三项契约的形状（实现前在各自的检查点中定稿）

```toml
# N23  builtin.toml：只写已实测的关系；引擎查表，不含任何架构名
[executes]
"linux-x86_64"   = ["i686"]        # 已由 CI 验证
"windows-x86_64" = ["i686"]        # 实测后加入

# N33  行数据：与主产物同名的附带产物；optional 的缺席不是错误
[[row.companions]]
suffix   = ".wasm"
optional = true
```

```cpp
// N30  插件（构建程序）声明它为哪个程序组装分发物；pack 未指名时取它
mcpp::pack_program("app");         // 一条指令，与 runner()、deploy() 同类；插件以它转达自己的 `target` 选项
```

键名在检查点中对照相邻数据定稿；这里固定的是形状：关系按 `<os>-<arch>`，附带产物是行数据的一张表，插件的
声明是构建程序的一条指令。

### 5.3 检查点

一个 PR（m），每个 commit 是一个检查点：只做一件事，带自己的测试、规范条款、中英文文档与 CHANGELOG 条目，
CI 在每个检查点上通过。合并保留 commit，不压缩，评审逐个检查点进行。

| 检查点 | 内容 |
|---|---|
| m1 构建依赖 | N29 |
| m2 打包 | N20、N11、N30 |
| m3 命名与资源 | N9、N16 |
| m4 行数据 | N23、N26、N33 |
| m5 运行器的启用 | R1；Windows 行在 Linux 上的 wine runner 数据（X1，`enabled = false`） |
| m6 运行器模型 | R2 的字段对齐、`runner_spec`、构建程序中的 `{pkg:}`、数据中的 `longlived` |
| m7 运行器的程序规则 | R2 的查找与 Windows 脚本规则；docs/30 与 docs/41 的矛盾；N21、N27 文档 |
| m8 Android runner 数据 | Android 行提供者条目的 `runner`（dirun 与镜像包，`enabled = false`）；前置：xb 已合并。实现：x86_64 一行（`same-arch`、`exclusive`，提供者 runner 增加 `exclusive` 键）；模拟器包只服务 linux-x86_64，aarch64 行不陈述 runner |

m1–m7 没有外部前置，立即开工，顺序按价值：N29 与 N20 先。m8 是最后一个检查点，前置在外部（dirun 0.4.0 与
API 34 镜像已在 xim）。它不拆成另一个 PR：外部前置不妨碍 m1–m7 的开发、评审与 CI，只决定 PR 何时合并；mcpp
的 PR 里也没有任何内容要等 mcpp 自己发布。代价是 m1–m7 的修复随 PR 一起等到 xb 合并后发布，关键路径因此
经过 mcpp（7.2）。若外部链条因 mcpp 之外的原因停滞，交由评审者决定是继续等待还是先合并 m1–m7，不自行拆分。

**Android 的 runner 数据为什么在 builtin.toml。** 运行器陈述的最低一级是提供者条目的 `runner`（SPEC-016 §6）；
Android 行由内置提供者（NDK）服务，没有平台包，所以它的 runner 写在该条目上，与现有的 qemu-user 条目同一
形式（`cmd`、`hosts`、`requires`）。这不是硬编码：引擎不认识 dirun 与 Android，条目只说“用哪个包里的哪个
程序”；客体的知识（启动参数、推送与取回的命令）全部在镜像包自带的 dirun 配置中，镜像升级不需要改 mcpp，
只有所需的最低版本变化时才改这一行数据。

## 6. 需要改 xlings 本体

- X2：替换在共享冲突上有界重试；可用时以 POSIX 语义重命名；自身读者以共享删除打开。修复后关闭 #715。
- X5：shim 直接创建进程并按 Windows 的参数规则转交 argv，不经命令解释器；加含 `"`、`&`、`%`、`^` 的回归测试。
- X3：先在干净环境复现；若属 xim 包数据，转为包修复（xb）。

## 7. 实施：PR 划分与任务依赖图

规则：

1. 每个仓库尽量一个 PR，以 commit 为检查点；每个检查点自带测试且 CI 通过。外部前置（别的仓库的发布）只
   约束某个检查点何时完成、PR 何时合并，不拆 PR。拆开只有两种情形：内容要等**本仓库自己的发布**；或仓库
   是发布渠道（索引），无前置的内容是别人的前置，不能等到最后。
2. 所有 PR 立即开工。依赖只约束**某个检查点**或**合并**，不约束开工。
3. 跨仓库的契约先写设计记录、评审后实现：dirun 客体配置的格式（dirun 与镜像包之间），R2 的规范条款与
   5.2 的三项形状（mcpp 与插件、平台、数据之间）。
4. 引擎的新接口由使用方以 `requires` 表达式探测（docs/30），旧引擎上退回现有行为。平台包用了新键时声明
   引擎下限（`[package] mcpp`），旧引擎自动停在上一版本。
5. 每次发布后按发布规范补齐 GitCode 资源并更新索引；mcpp 发布后不升自身固定版本，mcpp-index 的 CI 指向最新版本。

### 7.1 任务

| PR | 仓库 | 检查点 | 验收 | 产出 |
|---|---|---|---|---|
| k1 | openkal-macos | N18 `__bzero`；CI 加 `x86_64-macos-musl --release` | 发布构建通过 | 补丁版本 |
| k2 | openkal-uefi | N35 Shell 参数协议；N15 EFI_RNG_PROTOCOL；N34 `fs`（SIMPLE_FILE_SYSTEM，根为程序所在卷） | UEFI 上读写文件；带引号参数正确；无告警 | 次版本 |
| k3 | openkal 的 hosted 层所在仓库 | N10 hosted 安排报宿主系统 | FreeBSD 上报 FreeBSD | 补丁版本 |
| k4 | openkal-image | N19 路径规范化；X4 发布附全部产物，含 freebsd x86_64/aarch64 的 okrun | `//`、`../` 路径可运行；release 有产物 | 补丁版本 |
| k5 | openkal-dos | N17 最小复现 | — | issue |
| m | mcpp | m1–m8（5.3） | 第 9 节对应格；`mcpp run --target *-android` | 一个版本 |
| p | mcpp:plugins | p1 N36（页面一步以复制的文件为输入，页面是唯一的分发物）；p2 N31（中间页面移入隐藏目录）、N32（pack 结束时打印 web 目录与打开方式）；p3 N22 文档与诊断；p4 W1 `web` runner（10.6） | 产物目录外没有可误开的页面；`mcpp run --pkg-format web` 打开页面 | 补丁版本 |
| xl | xlings | X2；X5；X3 定位 | #715 关闭；特殊字符的参数完整到达 | 版本 |
| d | dirun | d1 设计记录；d2 N25 `MCPP_RUNTIME_FILES`；d3 N13 `stderr-file`；d4 N12 发布包携带的 okrun 取自平台版本组（固定 openkal-platform 0.28.1，CI 以含 `//`、`..` 的路径启动发布包）；d5 经命令到达的客体 | 各配置的测试；Android 配置在模拟器中运行 | 0.4.0 |
| xa | xim-pkgindex | ovmf 配置改为 Shell + `startup.nsh`（N14，dirun 0.3.2 已支持，已实测）；freebsd-disk-image 的 aarch64 镜像与配置（G6）；`freebsd-vm` 退役 | 经各包运行 | 无前置，先合并 |
| xb | xim-pkgindex | okrun、dirun 新版本；dosbox-staging 的 `stderr-file`（N13）；android-system-image API 34 x86_64/arm64，包内带 dirun 配置（G7） | 经各包运行 | k4、d 发布后 |
| xc | xim-pkgindex | DJGPP GCC 14 由脚本自建（D1） | 模块工程在 DOS 行构建 | 独立：构建时间长，失败不影响 xa、xb |
| op0 | openkal-platform | 版本组补丁 0.28.1：openkal-image 0.6.4（k4）、openkal-macos 0.23.4（k1）、openkal-musl 0.29.7（k3）；不含新引擎所需的内容 | family-release 工作流；各行构建 | 0.28.1 |
| op | openkal-platform | op1 N14：runner 命令补 `--`，并固定新的 ovmf 包版本；op2 移出本轮（13.1：对齐会使 openkal 构建失败）；op3 各实现的新版本；op4 N28；op5 FreeBSD 行与 windows-musl 行的 runner（`enabled = false`），并声明引擎下限 | 第 9 节对应格 | 0.29.0 |
| ix | mcpp-index | 每次发布一个索引 PR（发布流程本身如此） | 索引 CI | 随发布 |
| v | 验证工程的 CI（fork） | v1 探针、定时与 `workflow_dispatch` 输入；v2 逐格把探针改为断言 | 第 9 节 | — |

dirun 的 d1 把客体配置分成两个互不相干的部分，三种客体是它们的组合，而不是三种方式：

| | 生命周期（可缺省） | 传输：控制台、文件交付、状态取回 |
|---|---|---|
| 磁盘镜像的 VM（DOS、UEFI、FreeBSD） | 启动 QEMU 或 DOSBox，结束后关机 | 串口、镜像内的文件、debug-exit 或输出文件 |
| Android | 启动模拟器，等待就绪 | 以命令表达：`adb push`、`adb shell` |
| 已在运行的远程主机 | 无 | 以命令表达：`scp`、`ssh` |

### 7.2 依赖图

箭头表示“前者发布后，后者的该检查点才能完成”。没有箭头的检查点立即可做。

```
k1、k3、k4 发布 ──> op0 0.28.1 ──> d4 ──> d 发布 0.4.0 ──┐
k4 okrun 发布 ─────────────────────────────────────────────┴──> xb ──┬──> m8 ──> m 合并、发布 ──> op5 `enabled` 生效
                                                        └──────────────────────────> op5 FreeBSD 行 runner
xa 合并 ───────────────> op1 固定新的 ovmf 版本（与 `--` 成对）；op5 freebsd aarch64 镜像
k1 发布 ───────────────> op4 N28（macos-x86_64 的 okrun 为发布构建）
k1、k2、k3 发布 ───────> op3 各实现的新版本
p 发布 ────────────────> （op2 移出本轮，13.1）

每次发布 ──────────────> ix ──> v2 对应格由探针改为断言

立即可做、无前置：k1–k5、m1–m7、p1–p4、xl、d1–d3、d5、xa、xc、v1
```

关键路径：k4 → op0 → d（0.4.0）→ xb → m8 → m 发布 → op → ix → v2。op0 与 op 分为两个 PR，理由属于第 7 节的拆分条件：op 的内容依赖 mcpp 新版本的发布，op0 的内容只依赖已发布的成员，且版本组的补丁本是平台既定的发布方式（一个成员的修正即该成员的发布加版本组的一个补丁）。图中没有环：dirun 与 xb 都不需要新的 mcpp。
op 是汇合点，最后发布。m1–m7 在 xb 之前完成评审，xb 合并后只剩 m8 一个检查点。

### 7.3 不推迟的做法

- **W1（p4）立即做**：引擎尚无 R2 时，插件以已有接口提供 runner——`mcpp::toolchain_dir()` 定位 emsdk，
  `runner_longlived("web")`，参数含 `--hostname localhost`；引擎提供 `runner_spec` 后（`requires` 表达式探测），
  改用 `{pkg:emsdk}`。N29 发布前，工程把 mcpp:plugins 与平台对齐，不阻挡 W1。
- **op1 与 xa 成对**：新的 ovmf 配置在旧的平台命令下仍然启动程序（只是没有参数，与现状相同），所以 xa 可以
  先合并；op1 固定新的 ovmf 版本并补 `--`，两者一起生效。
- **m 不空等**：m1–m7 逐个检查点评审并通过 CI；p4（W1）、op 的其余检查点都不等 m 发布（7.1、规则 4）。
- **op 在分支上先验收**：op 发布前，验证工程的 CI 以分支依赖运行对应格（v1 的探针），不等发布。

## 8. 验证方式

先本地实测；本机缺资源的（Windows arm64、Android 模拟器的长时运行、FreeBSD aarch64）用验证工程 fork 上的
临时 draft PR 运行 CI，结果记入本文后关闭。本轮已实测：

| 假设 | 结果 |
|---|---|
| OVMF 无 `BOOTX64.EFI` 时进入 Shell 并带参数运行（N14） | 成立：`startup.nsh` 执行，参数到达，状态经 debug-exit 返回 |
| UEFI 上以 test.bat 验收 | 不成立：openkal-uefi 无 `fs`（新增 N34），验收改为先比对 `/c` 字符串的输出 |
| web 页面的可用性 | 从磁盘打开时取不到 `.wasm`；改用 `-sSINGLE_FILE=1` 被 N33 阻挡；工程侧的页面改为说明原因与打开方式（工程内修复） |
| DJGPP GCC 14 有现成二进制 | 不成立：须自建（D1） |
| okrun 新版有 FreeBSD 二进制 | 不成立：0.6.3 无任何产物（X4） |
| 工程构建程序（插件在其中）能提供具名长驻 runner | 成立：`--list-runners` 显示 build-program、long-lived |
| emrun 服务 web 目录并打开浏览器 | 成立：`.wasm` 以 `application/wasm` 返回；指定浏览器打开页面。默认监听 `0.0.0.0` |
| Windows arm64 批处理超时是生态问题 | 不成立：工程自身的缺陷。批处理以文本模式打开，`CALL` 以 `ftell`/`fseek` 记返回点；Windows C 运行库在文本模式下对只有 LF 的文件 `ftell` 计数错误，`EXIT /B` 回到 `CALL` 行本身。x64 未暴露，因为那里检出的是 CR LF。工程内修复（二进制模式打开），CI 改为两种行尾都运行；修复后全部作业通过 |
| `mcpp run` 在 POSIX 上怎样启动 runner | 读源码（`src/run.cppm`）：mcpp 以 runner 替换自身，runner 拥有终端、信号与退出状态；Windows 上 runner 在与 mcpp 绑定的单元中。v7 的“转发信号、宽限期、报告已结束”与此不符，撤回 |
| emrun 被 Ctrl+C 结束时的状态 | 实测：SIGINT 后退出 0 |
| 旧引擎读到 runner 表中不认识的键 | 读源码（`platform_toml.cppm`）：警告并忽略，runner 照常使用。因此带 `enabled = false` 的平台版本须声明引擎下限（`[package] mcpp`，旧引擎自动停在上一版本，docs/35） |
| 程序查找是否已按 PATHEXT | 读源码（mcpplibs.os `find_executable`）：裸名字在 Windows 上逐个补 PATHEXT 的后缀，且不找无后缀的同名文件；带目录的名字原样使用，不补后缀 |
| mcpp 2026.11.15.1 在 Windows 上启动批处理 runner | 临时 draft PR 实测（windows-2022）：裸名字 `x` 找到 `x.bat`，不取同目录无后缀的脚本；带空格的参数正确到达，退出状态原样返回。带目录而无后缀的名字报 “executable format error”，退出 126。参数含 `"` 时其后的 `&` 被命令解释器当作命令分隔执行，`%VAR%` 被展开，`^` 被吞掉。不经 runner 直接启动的程序参数完整 |
| xlings 在 Windows 上的 shim 怎样转交参数 | 同一次实测：经 shim 启动 mcpp 时，参数含 `"` 与 `&` 即被拆开并执行其后的命令；直接启动 mcpp 本体则不会（新增 X5） |

## 9. 验收矩阵（验证工程的 CI）

以同一批处理脚本的输出比对为准。CI 每一格先作为探针（写入摘要，不使作业失败），修复发布后改为断言；接受
`workflow_dispatch` 的 mcpp 与平台版本输入，并每周定时运行。开启机器类运行器（R1）、传入测试脚本是工程的
选择，写在工程的 CI 中。

| 目标 | 现状（2026-11-15） | 完成后 | 依赖 |
|---|---|---|---|
| Linux x86_64/arm64（gnu、musl）、i686 | 运行 | 运行 | — |
| Windows x64（gnu、musl）、arm64 | 原生机运行（arm64 于修复后经 CI 确认，LF 与 CR LF 两种行尾） | 另加：Linux 上 `mcpp run` 经 wine | X1、R1 |
| macOS arm64 | 运行 | 运行 | — |
| macOS x86_64（musl） | 调试版运行 | 发布版运行 | N18 |
| FreeBSD x86_64/aarch64 | vmactions 运行 | 另加：`mcpp run` 经 freebsd-disk-image；包的启动器直接启动 | G6、R1、N20 |
| Android x86_64/arm64 | 模拟器 action 运行（x86_64） | `mcpp run` 经 dirun 与镜像包 | G7、R1 |
| WebAssembly（node、浏览器） | 运行；pty 交互；Chrome；Pages | 另加：`-sSINGLE_FILE=1` 的页面可从磁盘打开；C++ 异常可捕获 | N33、N26 |
| `mcpp run --pkg-format web` | 拒绝（N36） | 在 Linux、macOS、Windows 上：服务页面（CI 中以 `--no-browser` 的覆盖运行），取回页面与 `.wasm`，中断后结束、无残留进程 | N36、W1、R2 |
| openkal 映像 x86_64/aarch64 | 运行 | 另加：`//`、`../` 路径 | N19、N12 |
| openkal 映像 i386 | 构建 | x86_64 机上运行 | N23 |
| UEFI x86_64 | 启动 | `/c` 字符串带参数运行并比对；之后 test.bat | N14、N35；N34、N25 |
| DOS（DJGPP） | 直接调 dirun 运行 | `mcpp run` 运行，标准错误可见 | N25、N13 |
| DOS（openkal，实验） | 启动 | N17 修复后运行 | N17 |
| 打包 | `--systems all` 的包在 Linux、FreeBSD（直接调 okrun）运行 | 每个程序有启动器；启动器在四种宿主上运行；含 macos-x86_64 的 okrun | N20、N11、N28 |
| 构建依赖 | 须与平台对齐 mcpp:plugins 版本 | 可写较新版本，或得到明确的冲突报告 | N29 |

## 10. 运行器架构：评估、统一模型与规范

### 10.1 两种 runner 的区别：dirun 与 dist-web 的 `web`

在 mcpp 的模型里两者都只是 runner：一条 argv 加几项性质，引擎不区分种类。区别全在性质上：

| | dirun | `web`（dist-web 提供，调用 emrun） |
|---|---|---|
| 面对的对象 | 构建产物：一个程序 | 分发物：web 目录（交给 runner 的是页面） |
| 做的事 | 启动整机或模拟器，或到达一台已有的机器；把程序与文件交付进客体，运行，取回输出与状态 | 在本机起 HTTP 服务，打开浏览器，服务到 Ctrl+C |
| 结束 | 自然结束，退出码即结论（`mcpp run`、`mcpp test` 都可用） | 长驻（`longlived`），由操作者结束；状态不是结论 |
| 客体知识在哪里 | 镜像包的配置（`dirun.toml`），数据 | Emscripten 的 emrun；页面由工程模板决定 |
| 由谁声明 | 平台或内置行的提供者数据 | 插件在构建程序中，以格式命名 |
| 入口 | `mcpp run`（默认 runner） | `mcpp run --pkg-format web`（按格式找同名 runner） |
| 首次使用的代价 | 镜像、模拟器，GB 级，默认关闭（R1） | 无：emrun 在已安装的 emsdk 中 |

两者不合并：dirun 的职责是“把程序交给另一台机器”，web runner 的职责是“把分发物呈现给人”。浏览器中的
自动化测试（无头、取回输出与状态）需要程序以 `--emrun` 链接、加载即运行，属工程选择。

### 10.2 现状评估（2026.11.15.1，源码与实测）

已经通用、本方案不改的部分：

- 引擎不认识任何 runner 名字；`flash`、`serve`、`web` 对它都一样（docs/41）。
- 覆盖层次：提供者条目 → 平台 → 依赖的构建程序 → 本机 `config.toml` → 工程 `[target.<sel>] runner` /
  `runners` → `MCPP_TARGET_<TRIPLE>_RUNNER` → `--runner`（SPEC-013 §10、SPEC-016 §6）。
- `mcpp run --pkg-format <f>` 按格式找同名 runner；插件作为包，经构建程序即可提供 runner（已实测）。
- **进程模型**（`src/run.cppm`）：POSIX 上 mcpp 以 runner 替换自身，runner 拥有终端、信号与退出状态，如同 shell
  启动它；其他系统上 runner 在与 mcpp 绑定的单元中，mcpp 以它的状态结束，结束请求到来时单元一并结束。
  无法启动为 127；被信号结束为 128 加信号号。
- **查找**：程序名先在 `requires` 的包、再在图中各包声明的 payload 的 `bin/`、最后在 PATH 中查找
  （SPEC-013 §10、docs/41）；Windows 上裸名字按 PATHEXT 补后缀，不找无后缀的同名文件，不找当前目录
  （mcpplibs.os）。
- 运行协议：`{}` 为产物；`MCPP_RUNTIME_FILES` 列出随产物的文件；`exclusive`、`host`。

缺口：

| 编号 | 缺口 | 影响 |
|---|---|---|
| A1 | 同一概念两种表达，字段不对称：数据有 `hosts`、`requires`、`{pkg:}`、`built-as`，构建程序没有；构建程序有 `longlived`，数据没有 | 插件不能声明依赖与适用主机；数据不能声明长驻 |
| A2 | 不在包的 `bin/` 下的程序（emrun 在 `emscripten/` 下）在构建程序中只能写绝对路径；docs/30 写“必须绝对路径”，docs/41 写“写名字，不写路径”，两者矛盾 | 路径在构建时固化；文档相互抵触 |
| A3 | Windows 上带目录的名字不补后缀；批处理型程序怎样启动、参数怎样转义没有成文的规则 | 实测：`../tools/x` 不能启动；批处理 runner 的参数含 `"`、`&`、`%`、`^` 时被命令解释器改写或当作命令执行（CVE-2024-24576 一类） |
| A4 | `{}` 指向目录型分发物时交什么未规定（N36 是其症状） | 插件各自猜 |
| A5 | 是否默认启用（R1）没有键 | 首次使用代价大的 runner 不能写成数据 |

### 10.3 统一模型（R2）

一个 runner 是一组字段，数据、构建程序与工程用同样的名字：

| 字段 | 含义 | 数据（toml） | 构建程序 |
|---|---|---|---|
| 名字 | 缺省为默认 runner；否则为具名 | `runner` / `runners.<name>` | `runner(tok)` / `runner(name, tok)` |
| `cmd` | argv；`{}` 产物或分发物，`{sysroot}`，`{pkg:<name>}` | 已有 | 补：模板在构建程序中同样可用 |
| `longlived` | 无自然结束；状态不是结论 | 补 | 已有 `runner_longlived` |
| `exclusive` | 运行不可重叠 | 已有 | 已有 `run_exclusive` |
| `host` | `same-arch` 或 `any` | 已有 | 已有 `run_host` |
| `hosts` | runner 存在于哪些主机（`<os>-<arch>`） | 已有 | 补 |
| `requires` | 所需的包，首次运行时安装，构建不安装 | 已有 | 补 |
| `enabled` | 是否启用（R1） | 补 | 补 |

默认 runner 与具名 runner 是同一种表：`runner = { ... }` 与 `runners.<name> = { ... }` 形状相同，数组形式是
只写 `cmd` 的简写。`cmd`、`requires`、`enabled` 是一条陈述，随 `cmd` 一起被替换；`host`、`exclusive` 各自独立
替换（现有规则）。

构建程序一侧用一个类型与表一一对应，写法与已有的 `mcpp::action` 相同（字段、链式添加、`submit()`）；旧的
逐词调用保持有效：

```cpp
mcpp::runner_spec web;
web.name      = "web";                       // 空：默认 runner
web.longlived = true;
web.arg("{pkg:emsdk}/emscripten/emrun").arg("{}")
   .require("xim:emsdk");
web.submit();
```

| 表的键 | `runner_spec` | 说明 |
|---|---|---|
| `cmd` | `.arg(tok)` | 与 `action.arg()` 相同 |
| `longlived`、`exclusive`、`enabled` | 同名 `bool` 字段 | `enabled` 缺省为真 |
| `host` | 同名字段（`"same-arch"`、`"any"`） | 已发布的键，不改名 |
| `hosts` | `.hosts(h)` | 每次添加一项，与键同名 |
| `requires` | `.require(pkg)` | `requires` 是 C++ 的关键字，不能作成员名；取最近的拼写 |

命名的规则：方法与表的键同名；键是 C++ 关键字时取最近的拼写。类型名取 `runner_spec`：`mcpp::runner` 已是
函数名；`spec` 是“声明式描述”的通行叫法（Kubernetes 的 `PodSpec`）。数据中的键写作 `longlived`，沿用已发布的
`runner_longlived`，不另拼 `long_lived`。

### 10.4 规范（R2）

1. **查找。** 沿用现有顺序（`requires` 的包 → 图中 payload 的 `bin/` → PATH）。新增两点：`{pkg:<name>}` 在构建
   程序中可用，用于不在 `bin/` 下的程序；Windows 上带目录而无 PATHEXT 后缀的名字同样补后缀，规则与裸名字
   相同（不取无后缀的同名文件）。任何情况下不查找当前目录。
2. **执行。** argv 不经 shell。Windows 上的批处理（`.bat`、`.cmd`）由命令解释器启动，这是系统的规定；引擎按命令解释器的规则转义每个参数（引号、`%`、`^`、`&` 等），不能安全转义的参数拒绝执行并说明原因（Rust 标准库对 CVE-2024-24576 的处理）。实测现状不转义。
3. **占位符。** 只替换已知的占位符（`{}`、`{sysroot}`、`{pkg:<name>}`），可以出现在一个词的中间
   （`{}/index.html`、`{pkg:x}/bin/y`），其余花括号原样保留。没有 `{}` 时产物附在末尾（已有规则）。同一名字的
   runner 只能用一种方式声明：`runner_spec` 与逐词调用混用于同一名字时报错。
4. **进程与状态。** 沿用现有模型（10.2），不新增信号转发与宽限期。runner 的退出状态就是 `mcpp run` 的状态。
   `longlived` 的含义是“状态不是结论”：`mcpp test` 不使用长驻 runner，机器输出标明 `long-lived`，由读者决定
   如何对待它的状态。实测 emrun 被 Ctrl+C 结束时退出 0。
5. **分发物。** `{}` 为格式唯一的终端产物；目录型分发物的入口由提供者以 action 的输入输出表达（N36）。
6. **文件。** 搬动产物的 runner 复制 `MCPP_RUNTIME_FILES` 所列文件（已有条款，dirun 实现 N25）。
7. **主机名。** `hosts`、执行关系（N23）与打包启动器（N20）都用 `<os>-<arch>`，系统名取自系统表。

验证：第 1、2 条在 Linux、macOS、Windows 上各有 e2e；`mcpp run --pkg-format web` 一格在三种系统上验证长驻
runner 的启动与结束（第 9 节）。

### 10.5 与行业习惯的对照

| mcpp | 同类工具 | 判断 |
|---|---|---|
| `runner`、`MCPP_TARGET_<TRIPLE>_RUNNER` | Cargo `[target.<triple>] runner`、`CARGO_TARGET_<TRIPLE>_RUNNER`；CMake `CROSSCOMPILING_EMULATOR`；Meson `exe_wrapper`；Bazel `--run_under` | 与 Cargo 同名同形，保留 |
| mcpp 以 runner 替换自身 | `cargo run` 在 Unix 上同样以 exec 交出终端 | 保留：状态、信号与 shell 启动时一致 |
| 具名 runner（`flash`、`web`） | npm scripts、just 的 recipe | “以格式命名、`--pkg-format` 自动找到”是 mcpp 独有的 |
| `exclusive` | Bazel `tags = ["exclusive"]` | 同名同义 |
| `enabled` | Gradle 任务的 `enabled` | 采用 |
| `MCPP_TARGET_<TRIPLE>_RUNNER_ENABLED` | Cargo 以 `CARGO_TARGET_<TRIPLE>_<KEY>` 覆盖任一键 | 同一构词 |
| `requires` | mcpp 各处已用 | 同一概念同一拼写 |
| `{}` | `find -exec {}`、`xargs -I{}` | 沿用 |
| 127、128 加信号号 | POSIX shell 的约定 | 已有 |

已知的不理想，按“已发布的键不改名，历史不一致写成规则”处理：`host`（与产物架构的关系）与 `hosts`（runner
存在于哪些主机）只差一个字母，文档并列说明；`cmd` 只在表中出现，构建程序中对应 `.arg()`。

### 10.6 插件接口（dist-web）

`mcpp::dist::web::options` 增加 `runner`（缺省开启，提供名为 `web` 的长驻 runner）。runner 只监听本机
（`--hostname localhost`）：emrun 默认监听 `0.0.0.0`，开发服务器不应默认暴露给局域网。浏览器、端口等是本机
偏好，不进插件选项，用户以已有的覆盖层次替换整条命令（`config.toml` 的
`[target.wasm32-emscripten.runners] web = ["{pkg:emsdk}/emscripten/emrun", "--port", "8080", "{}"]`）。
其他 `dist-*` 成员按同一形状提供以格式命名的 runner，由各自决定，不在本方案范围。

不做：让 profile 携带 target 与 pkg-format 以缩短命令。命令由 README 给出完整形式。

## 11. 决策记录

| 序 | 决策 |
|---|---|
| 1 | 第 5 节本体项作为 mcpp 下一版本范围；N9 统一应用 B2；N16 判据改为执行系统；N26 含 C++ 时默认 wasm 异常；N21、N22 只做文档与诊断；以验证工程作金丝雀，定时加按需；32 位新行维持现状 |
| 2 | xim-pkgindex 的生态工具一并补全；验证工程 CI 的全矩阵为整体验收 |
| 3 | Android 运行器在 dirun 中，形式为“经命令到达的客体”；首次使用代价大的运行器默认关闭、提示完善、可在 mcpp.toml 配置（R1）；这类运行器这一轮只保证 Linux 主机（程序规则与 `web` runner 覆盖三种系统）；无前置的先做 |
| 4 | 基础设施只提供机制、数据与扩展点；工程特有的配置留在工程（第 2 节） |
| 5 | W1：dist-web 提供 `web` runner（emrun），只监听本机；不让 profile 携带 target 与格式；运行器的统一模型、插件接口与规范一并设计（R2） |
| 6 | R2 的构建程序接口用一个类型（`runner_spec`）与表一一对应 |
| 7 | 能放在一个 PR 的放在一个 PR，以 commit 为检查点；不能的拆开；任务按依赖图并行，尽量不推迟 |
| 8 | 需要验证的假设用临时 draft PR 实测（本轮：Windows 上的批处理 runner，结果见第 8 节，PR 已关闭） |
| 9 | 撤回此前两项：长驻 runner 结束时由 mcpp 报告“已结束”并退出 0；固定 10 秒宽限期。原因：mcpp 在 POSIX 上以 runner 替换自身，没有可以转发、等待或报告的 mcpp；改为 10.4 第 4 条 |
| 10 | R1 的一次性覆盖用环境变量 `MCPP_TARGET_<TRIPLE>_RUNNER_ENABLED` |
| 11 | Android 的 runner 数据留在 builtin.toml 的提供者条目，是 mcpp 同一个 PR 的最后一个检查点（m8）。mcpp 侧统一为单 PR：只有依赖 mcpp 自己发布的内容才拆；不阻塞开发的外部依赖不拆 |
| 12 | mcpp#932、#936、#938 并入本 PR（m9），按设计规范取最佳方案，不取 issue 的临时建议（13.4）；N39 待评审 |
| 13 | mcpp#942、#941 并入本 PR：`[target.'<sel>'.test] discover` 与 `[test] features`，复用条件输入与 dev-dependency 的解析，不改 mtest（13.8） |

## 12. 被否定的方案与自我评审

### 12.1 被否定的方案

| 方案 | 否定的原因 |
|---|---|
| 引擎为数据文件新增通道（N25） | 运行协议已有 `MCPP_RUNTIME_FILES`；缺的是 dirun 的实现 |
| dirun 增加一种 “android 方式” | 是为一个系统写的特例；改为生命周期与传输两部分的组合 |
| 把“x86_64 可执行 i386”写进引擎；或只按架构写成数据 | 关系随系统而变（macOS 不成立）；写成按 `<os>-<arch>` 的数据 |
| N26 增加一个可关闭的开关 | 参数是行数据，工程以已有 flags 覆盖 |
| dirun 自带同版本的 okrun | 改为声明对 okrun 包的依赖，副本与版本漂移一起消失 |
| web runner 并入 dirun | 职责不同：交给另一台机器，与呈现给人 |
| 浏览器、端口作为插件选项 | 属本机偏好，走已有的覆盖层次 |
| `runner_spec` 用指定初始化；方法名 `.requires()` | 前者与 `mcpp::action` 的写法不同；后者是关键字，不能编译 |
| `enabled` 像 `host`、`exclusive` 那样被只写 argv 的覆盖继承 | 已写 `runner = ["wine", "{}"]` 的工程升级后会被拒绝 |
| 为默认 runner 的一次性开启新增命令行开关 | 已有 `MCPP_TARGET_<TRIPLE>_<KEY>` 的构词，不增加词汇 |
| Android 的 runner 数据随镜像包提供 | 运行器陈述没有“包”这一级；内置行的最低一级是提供者条目，与 qemu-user 相同 |
| mcpp 转发信号、设宽限期、为长驻 runner 改写退出状态（v7） | 与现有进程模型冲突，且会让交互式程序读不到终端；现有模型已满足需要 |
| 平台包直接发布带 `enabled = false` 的 runner，由旧引擎自行处理 | 旧引擎警告后照常使用该 runner，会在首次运行时下载镜像；改为声明引擎下限 |
| xim-pkgindex、mcpp-index 各一个 PR | 索引是发布渠道：无前置的内容不应等关键路径末端的内容 |
| Android 的 runner 数据拆成 mcpp 的第二个 PR（v8） | 外部前置不妨碍其余检查点的开发与评审，只决定合并时机；拆 PR 只为“等本仓库自己的发布” |
| Windows arm64 的超时归为生态问题 | 逐命令定位后是工程自身的缺陷（文本模式下的 `ftell`），在工程内修复 |

### 12.2 自我评审（v9）

| 角度 | 检查 | 结论 |
|---|---|---|
| 架构 | 引擎是否出现系统名、工具名、运行器名 | 没有：wine、dirun、emrun、Android 都只在数据、包或插件中 |
| 架构 | 新增的引擎概念 | 四个：`enabled`、`runner_spec`（已有字段的另一种写法）、执行关系表、可选的附带产物；各有数据形状（5.1、5.2、10.3） |
| 规范 | 每个缺省值是否是数据并有四层覆盖 | `enabled` 四层齐全；其余新键是行数据或提供者数据，沿用现有层次 |
| 规范 | “少做不沉默” | 未开启的 runner 拒绝并给出原因、所需的包、两种开启写法与 reason token |
| 通用 | 模型能否覆盖已知的各类 runner | 用户态模拟、解释器、VM 与模拟器、设备、远程主机、呈现、调试服务器：都只用这组字段 |
| 复用 | 是否重复已有机制 | v7 重复了进程模型与 PATHEXT 查找；v8 改为引用现有实现，只补缺口 |
| 语义 | 名字是否一词一义 | 任务编号与问题编号不再重名；`requires`/`.require` 的差异写成规则 |
| 兼容 | 升级后现有工程的行为 | 不变：自写 runner 的工程不受 `enabled` 影响；旧引擎停在旧的平台版本；N9、N16、N33 的产物变化写入 CHANGELOG |
| 简洁 | 文档 | 第 5 节与任务表合并；修改历史压缩为 12.1 |
| 依赖 | 是否有可以提前的任务被后序任务拖住 | xa 与 xb 拆开；op 是汇合点，分支上先验收。mcpp 单 PR 的代价是 m1–m7 的发布等 xb：已写明，并给出停滞时的处理 |
| 依赖 | 单 PR 后是否成环 | 没有：d、xb 不需要新的 mcpp；依赖 m 发布的只有 op5 与 p4 的接口切换，后者经探测自动启用 |
| 未决 | 尚无实测支持的条款 | 10.4 第 2 条的转义算法、`{pkg:}` 在 Windows 上补后缀：实现时在 Windows 的 e2e 中验证；长驻 runner 在 Windows 上被 Ctrl+C 结束后有无残留进程：CI 无法发出控制台事件，m7 中以库的单元测试覆盖 |

## 13. 实施中的新发现（待评审）

### 13.1 N37：构建依赖与目标图共用一次版本解析

**现象（实测，2026-11-15）。** 工程以 `[build-dependencies.mcpp] plugins = "0.21.0"` 构建 `x86_64-openkal`。
平台 openkal-platform 0.28.0 的插件请求 `plugins 0.20.0`。N29 的拒绝给出这两个请求者；工程以
`override = true` 选定 0.21.0 后，构建仍然失败：`mcpplibs.openkal` 由 `mcpplibs.os@0.4.0` 请求 0.24.0，由
`openkal-musl@0.29.6` 请求 0.25.3。plugins 0.21.0 起 `plugins-core` 依赖 mcpplibs.os，而 os 0.4.x 固定在
openkal 0.24。

**原因。** 构建程序的依赖（主机图，mcpp#797）与目标的依赖在同一次解析中按 `(namespace, name)` 合并。
主机的子构建从这次解析固定版本（`host_graph.cpp`），因此主机侧的 openkal 与目标侧的 openkal 被当作一个包。
两者编译给不同的机器，本不需要一致。

**影响。**
- op2（平台的插件依赖对齐到 0.22.0）会使所有 openkal 构建失败。本轮 op 保持平台的插件依赖为 0.20.0
  （其 `plugins-core` 不依赖 os），op2 移出本轮，等 13.1 定稿。
- 工程同时使用 plugins ≥ 0.21 与 openkal 目标时，只能把插件依赖写在只匹配其他目标的
  `[target.<sel>.build-dependencies]` 下。

**建议的设计。** 只经构建依赖到达的包（`buildOnly`）以独立的身份解析：`ResolvedKey` 增加一个“图”维度，
与已有的 `variant` 维度同一方式（两个补丁集已是两个身份）。主机图内部仍按 N29 统一或拒绝；主机图与目标图之间
不合并、不冲突。主机的子构建取主机身份的版本。锁文件为主机身份另记一节。这是引擎的解析模型变化，需要
评审后实现，不在本轮的 mcpp PR 中。

### 13.2 N38：内置 profile 的部分覆盖整体替换（待评审）

**现象（实测，2026-11-16，2026.11.15.1 与本分支相同）。** 工程只写 `[profile.dev] ldflags = [...]`，构建报告
`dev [optimized]`，编译取 `-O2`、无 `-g`：内置 dev 的 `opt = 0`、`debug = true` 不再生效。

**现状。** docs/04 §2.9 写明 `[profile.<built-in name>]` 整体替换内置定义，行为与文档一致。Cargo 的 `[profile.dev]`
按键覆盖，未写的键取内置值；只为加一个链接标志而写 profile 的工程因此失去调试构建，且只能从状态行察觉。

**建议的设计。** 内置名字的 profile 按键覆盖：未写的键取内置值，与 Cargo 一致；自定义 profile 维持现状（未写的键取
引擎缺省）。这是已发布行为的改变：依赖“整体替换”的工程需补写键。升级时对写了内置名字、且未写 `opt` 或 `debug` 的
profile 给出一次提示。需要评审后实现，不在本轮。

### 13.3 本轮已实现的范围变化

- **N29（m1）** 实现为：构建依赖的两个请求无法统一时拒绝，列出两个请求者、合并失败的原因与 `override` 写法。
  此前的路径尝试重命名模块，在没有源文件的数据包（平台）上以无关的消息失败。
- **N30 的插件侧**（dist-web 调用 `mcpp::pack_program`）需要协议 19，随 mcpp 发布后的插件版本采用；
  插件以 `mcpp::warning` 陈述不提交的原因，这一部分不需要新引擎，在 p 中完成。
- **op0** 见 7.1：版本组补丁 0.28.1 先于 op 发布，dirun 0.4.0 经它取得 okrun 0.6.4。
- **N27** 实测结论与原记录不同：node 下 Emscripten 对终端的同步读取以 `EIO` 失败（是否调用 `tcsetattr` 相同），
  管道与文件可读；文档按实测写出（docs/24）。
- **op（openkal-platform 0.29.0）** 实现的范围：
  - op1：UEFI runner 补 `--`，固定 ovmf 9.2.4-2 与 dirun 0.4.0。
  - op3：openkal-uefi 0.11.0。openkal-musl 0.29.7 请求 `^0.10.0`，因此先发 openkal-musl 0.29.8（`>=0.10.0, <0.12.0`）。
  - op4：okrun runner 的宿主加入 `macos-x86_64` 与 `linux-i686`（后者 `built-as = i686-linux-musl`），`--systems all`
    的包实测含两者。
  - op5：按 R1 的规则，虚拟机与机器模拟器的 runner 一律 `enabled = false`，包括 UEFI（QEMU 与 OVMF）与 DOS（DOSBox），
    不只 FreeBSD 与 windows-musl。hosted 行的 runner 以完整的 `[[platform.provider]]` 陈述：`hosted = true`，
    `base = "none"`，toolchain 与 layers 取 `[platform.hosted]` 的值。实测三行的构建图与不加条目时逐字节相同。只写 runner
    的条目会取 ok 映像的约定，所以不能只写 runner。
  - 本项留作评审（N40）：`[platform.hosted]` 若能按行陈述 runner，即可去掉 layers 的重复。这需要改 SPEC-013 §1 的键表，
    不在本轮。
- **N17（k5）** 定位与原记录不同：批处理静默结束不是 `if … goto` 所致，而是之后的目录列举超出 openkal-dos 的 64 KiB
  首个栈，栈溢出不产生故障而挂起，未写出的输出随之丢失。最小复现（8 KiB 栈帧递归 8 层即挂起）见 mcpplibs/openkal-dos#3，
  修复属 openkal-dos，本轮不实现。
- **G7（m8、xb）** 实测：API 34 镜像的 system.img 是 GPT 磁盘（vbmeta 与动态分区 super），qemu-user 的根仍只属 API 24
  arm64-v8a；模拟器 37.1.11 以 `-sysdir` 启动时需要一个含 `platform-tools` 的 SDK 根，否则在读取 `-sysdir` 后以 SIGSEGV
  结束，启动脚本给出空的 SDK 根。x86_64 镜像经已安装的配置与 dirun 0.4.0 运行动态程序，19 秒得到状态，参数与标准错误完整，
  运行后不留模拟器与设备目录。
- **N21** 实测顺序为 `[build]`、profile、`[target.<sel>.build]`，`cflags`、`cxxflags`、`ldflags` 相同（docs/04）。
- **R2 的 Windows 脚本规则（与 10.4 第 2 条的差异，2026-11-16 评审确认）** 实现为拒绝含命令解释器语法的参数（系统表
  `command-scripts` 指出哪些后缀经解释器启动），不做转义。转义要求引擎以 `one_string` 自行构造
  `cmd.exe /d /s /c` 的整条命令行（Rust 的做法），其正确性只能在 Windows 上实测；拒绝的集合包含可以安全转义的字符
  （引号内的 `&`、`|`），比 10.4 更严，但满足“不能安全传递的参数不执行”。评审确认以拒绝为本轮的规则，不补做转义。

### 13.4 追加：mcpp#932、#936、#938（2026-11-16）

三个问题都是通用缺陷，与具体工程无关，都在本 PR 实现。下面逐条写出：哪些部分排除了，采用的设计，以及没有采用 issue
原建议的理由。

**#938 按确切路径列出的源文件不存在时静默跳过。**
- 设计：`sources` 中不含模式字符（`*?[{`）的条目指名一个文件；文件不存在时，扫描以错误拒绝，并指出陈述它的表。
  - 表按 `[build]`、`[target.'<sel>'.build]`、`[features.<f>]` 的顺序查找，取自条件合并前的清单
    （`beforeConditionalMerge`）。
  - 扫描器导出 `unnamed_sources(root, manifest)`，只陈述条目，不决定严重性。
  - 严重性由准备阶段第 11 步按包所在的树决定，复用 `state.editablePackage`（与依赖 lint 的划线相同）：
    可编辑的树（根、workspace 成员、`path` 包）报错；取入存储的包报警告。
  - 理由：已发布包的 tarball 不可更改，使用者无从修复。若新引擎拒绝它，此前能构建的发布就会被拒绝。
    警告仍然说明随后导入失败的原因。
- 不在检查范围内：
  - 构建会写出的文件，即 action 的输出与声明的单元；
  - 绝对路径条目，它们来自构建程序；
  - 匹配为空的模式，因为一个目标上的选择可以为空。

**#932 `mcpp test --guide` 在 workspace 根先规划并构建全部成员。**
- 原因：guide 读取的测试框架是包清单中的声明数据，此前却从测试计划中读取，计划需要工具链、构建程序、扫描和构建锁。
- 设计：
  - 新增 `resolve_graph`，执行与 `prepare_build` 相同的阶段（清单与 workspace、工具链请求、图解析），在图之后停止。
  - 框架的提取抽成 `collect_test_frameworks`，由计划与 guide 共用。
  - guide 在任何位置都不规划、不构建，也不取构建锁：单个包、单个成员、workspace 根都一样。
  - 选中多个成员时，按配置组解析，合并各组的框架。
- 未采用 issue 的“工作区根打印通用指南”：成员的框架可以从解析图中得到，通用指南会丢掉这一信息。

**#936 workspace 测试中，后一个成员的构建重编了已是最新的依赖模块（Windows 上写入失败）。**
- 根因（在 Linux 上复现）：成员的程序（L5，`MCPP_BIN_<NAME>`）由每个成员各自规划。
  - 该规划只按这一个成员解析共享包的 feature，而测试规划按组内成员的合并结果解析。
  - 两个规划写入同一目录的同一路径，所以一个未改动的模块在一次运行中编译 3 次，每次再运行时又编译 3 次。
  - Windows 不允许覆盖已被映射的文件，这次重写因此失败。
- 设计：组内成员的程序作为一次规划，与 `mcpp build --workspace` 相同，所以共享包的配置与测试构建一致。
  - 只有构建失败时，才对单个成员重新规划，以指出是谁的程序失败。
  - 实测：首次运行编译 1 次，再次运行 0 次。
- 未采用 issue 的“写临时文件再改名”：
  - 输出由编译器写出，不经过 mcpp；
  - Windows 上改名覆盖一个被映射的文件同样失败；
  - 根因是不必要的重写，消除重写后，这条路径不再出现。

### 13.5 N39：同一包的不同配置共用输出路径（待评审）

#936 修复后，仍有两种情形使同一个包在一个输出目录中以不同配置交替编译：
- 测试的 dev-dependency 给共享包加了 feature，此时程序规划与测试规划不同；
- `mcpp build -p a` 与 `mcpp build --workspace` 交替执行。

在 Linux 上，这两种情形只是重编；在 Windows 上，如果另一个进程映射着旧的 BMI，写入会失败。

建议的设计：包的对象与 BMI 路径带上该包的配置身份（feature、标志的摘要），使不同配置并存、互不覆盖，配合
`mcpp clean --stale` 回收。代价是磁盘占用，这与 #874 的“每个配置一个目录”相互权衡，需要评审后实现，不在本轮。

### 13.6 链接没有并发上限（CI 中定位，已修复）

- 现象：本 PR 的 `unit-linux-archive`（构建全部测试并归档）在每个提交上都失败。runner 被关闭，或与服务端失去通信，没有上传日志。main 上同一作业通过。
  - 先前误判为基础设施故障。按“红色作业先读日志”的规则比对 main 的历史后，确认失败与本 PR 有关。
- 根因：GCC 且 `bmi_schedule = "on"` 时，ninja 的 `-j` 是编译上限的 6 倍，世代的信号量只约束编译器，链接不受约束。
  - 本 PR 改动了几乎所有模块都导入的 `toolchain-model` 构建程序，因此 26 个测试程序在同一时刻就绪并同时链接。每个 `ld` 占 1–1.8 GB，16 GB 的 runner 内存耗尽。
  - main 上的增量运行没有重链，所以没有暴露。
- 设计：链接取同一信号量的许可（`$mcpp __compiler-cap --sem .mcpp-sched --output $out -- <linker>`），编译与链接合计不超过本机的并发数。
  - 图中仍不写数字，与作业数无关（e2e 861 的规则）；归档只复制成员，不取许可。
  - 许可包住整个发布：被持有的命令结束时，产物已经就位。
    - 世代的事务以命令结束时的输出身份判定提交；若许可只包住链接器，其后的 `mv` 改变了输出，恢复时该输出被判为未提交而删除（实测：构建程序链接后消失）。
    - 因此 POSIX 的临时名发布由 shell 序列（`t=… && … && mv`）改为一个命令：`mcpp link-output --temporary` 以 `<dir>/.<file>` 链接（保留扩展名，N9），成功后改名覆盖产物，失败时删除临时文件。
    - 至此每个主机的链接都经 `link-output` 发布（Windows `--aside`，伴随文件 `--companions`，POSIX `--temporary`）；原地链接的目标不经发布。
  - 未采用 ninja `pool`：pool 的深度要写进 `build.ninja`，图就随作业数变化。
- CI：测试由固定的 bootstrap（2026.11.13.1）构建，它没有这项修复，而发布后不提升自身固定版本。
  - `ci-unit.yml` 按 bootstrap 的版本判断：早于 2026.11.16.1 时以 `MCPP_BMI_SCHEDULE=off` 构建归档，此时 ninja 的作业数（核数 + 2）约束链接；固定版本前移后自动恢复分离调度。
- 测试：单元 `NinjaBackend/LinksHoldAPermitOfTheScheduleBound`；e2e 938（重链不暴露半个文件）、861（图与作业数无关）。
  - 实测（从 main 的测试构建恢复，`MCPP_JOBS=4`）：bootstrap 同时运行的 `ld` 最多 7 个，超过上限 4，用时 539 s；本分支最多 4 个，用时 360 s。

### 13.7 mcpp 捆绑的 xlings 前移到 2026.11.16.1（本 PR 实现）

- X2（xlings#715）与 X5 在 xlings 2026.11.16.1 中修复；mcpp 捆绑并在 CI 中使用的 xlings 原为 2026.10.26.1。
- 实测：本 PR 的完整 CI 中，Windows 的 e2e `sources/100` 失败，原因正是 #715：`Failed to write file: ...\registry\subos\default\.xlings.json`。
- 设计：`pinned::kXlingsVersion` 与 `.github` 中的固定版本一并前移到 2026.11.16.1（`check_version_pins.sh` 校验一致），mcpp 的用户因此得到 X2 与 X5 的修复。
- 风险评估：两个版本之间只有三个提交（xlings#716、#717、#718），后两个只改发布流程，mcpp 调用的接口约定不变。

### 13.8 mcpp#942 与 #941：目标的测试，以及测试构建打开的 feature（本 PR 实现）

两个问题同源：测试的配置无法表达，只能为每种组合另建一个包（一个 179 个成员的 workspace 中占相当比例）。两者都是通用缺陷，用 mcpp 已有的两种机制补齐，不新增概念，也不改 mtest。

**#942 `[target.'<sel>'.test] discover`。**
- 现状：源文件（`[target.'<sel>'.build] sources`）、dev-dependency（`[target.'<sel>'.dev-dependencies]`）与单个 target（`[target.'<sel>'.targets.<t>]`）都可按目标变化，测试程序集合不能；dev-dependency 已可在 Web 上去掉，引用它的测试文件仍在每个目标上编译。
- 规范依据：条件节只贡献构建输入；测试文件是测试构建的输入，所以条件 `test` 表只承载 `discover`；`programs`、`split`、`link`、`store`、`main`、`start` 决定形态，不进入条件表（其它键为警告）。
- 语义：谓词与目标相符的表，其 glob 追加到 `[test] discover`，整体求值；`!` 从整个集合中移除，与条件 `sources` 相同。两种写法都成立：在 Web 上移除（`!tests/native/**`），或只在原生目标上增加（`tests/native/*.cpp`）。
- 目标：`--target`，否则包的 `[build] target`，否则本机，与 `workspace_groups` 的规则相同（W4）。
- 测试在依赖解析之前发现，目标侧层（`kernel-abi`、`c-abi` 等）尚无值；这类谓词被拒绝，而不是按假处理把测试静默去掉。
- 修正 issue 的示例：`programs` 已是粒度键（`"file"`/`"directory"`/`"package"`），不能复用，键为 `discover`。
- 不采用 C++ 的 `#if __EMSCRIPTEN__`：测试会在 Web 上编成空程序并“通过”（虚绿）；导入一个在该目标上被去掉的 dev-dependency 的模块也无提供者。
- 实现：`ConditionalConfig::testDiscover`；`conditional_test_globs` 位于 `mcpp.build.test_targets` 的实现单元中，用 prepare 的谓词求值器（`cfgpred`）。
  - 求值器不进入接口单元：接口单元导入 `prepare_inputs` 时，GCC 16.1 在导入 `mcpp.cli` 的 `main.cpp` 处内部错误（实测）；放入实现单元后，导入者读到的接口不变。

**#941 `[test] features`。**
- 语义：测试是其包的消费者，`[test] features` 是这个消费者的 feature 请求，在测试构建中解析，与 dev-dependency 相同，不进入产物，其它消费者看不到。
- 规则：
  - 未声明的 feature 在加载时拒绝（与 `--features` 相同；该表只在测试本包时读取，没有兼容性顾虑）。
  - 与 `--features` 相加；只在测试规划（`includeDevDeps`）中生效；`mcpp build` 与测试运行的程序（L5，`MCPP_BIN_<NAME>`）保持默认 feature。
- 单个包：在 prepare 的第 0 步之后并入被测包的请求，所有读取根请求的地方一致看到；记录的图写明它们。
  - “未改动的工程”快速路径按请求加 `[test] features` 比对，保持可用（e2e 1099 B 断言第二次运行不重新规划）。
- workspace：在该成员自己的边上请求。另一个声明同名 feature 的成员不受影响（e2e 1100：成员 b 的同名 feature 源文件不能编译，`mcpp test --workspace` 通过）。
  - 组内其它成员的测试经由依赖看到被打开 feature 的包，与 dev-dependency 的 feature 在测试构建中统一的现行行为一致。
- 与 docs/08“被测代码即发布代码、共用一个构建目录”的关系：feature 请求记录在图上，不移动构建目录（`request_tag`），所以先后构建 `mcpp build` 与 `mcpp test` 会重编 feature 改变的部分，与交替使用 `--features` 相同。消除这种重编的是 N39（配置身份进入输出路径），仍待评审。
- 未做的门控：只在某 feature 下才有意义的测试，若以后需要，可由 feature 自己声明（`[features.<f>] tests`，与 `[features.<f>] sources` 对称），而不放进 `cfg(...)`。cfg 在 feature 解析之前求值，Cargo 同样不支持在 target 表中以 feature 为谓词。本轮的 `[test] features` 总是打开所列 feature，不需要门控。

**与 mtest 的关系。**
- 分工：mcpp 决定构建什么（程序与翻译单元，按目标与 feature，编译期）；mtest 决定运行什么（用例，按机器能力，运行期：`requires:` 标签、带原因的跳过、`resource{"machine"}`）。
- 两个问题都在编译期，运行期跳过无法替代：依赖不能为该目标构建。协议（SPEC-017）不变。
- 运行存储也不需要“排除”状态：某目标上被排除的测试只是不在该次运行的列表中，按名称的测量历史、`--shard`、`--failed` 都按列表工作。
- 可选的后续联动（不在本轮）：把构建事实（目标的 cfg 值、激活的 feature）交给测试进程，使用例可声明 `requires:os=…` / `requires:feature=…`。用于“处处能编译、只在某配置下有意义”的用例；目前 feature 已能生成宏，暂无需求。

**测试。**
- 单元：`Manifest/TestFeaturesNameDeclaredFeatures`、`Manifest/AConditionalTestTableCarriesDiscoverAlone`。
- e2e 1098：Web 上移除、原生上增加、层谓词拒绝、其它键警告。
- e2e 1099：无 feature 时失败；有 feature 时通过，第二次运行走记录；`mcpp build` 的图不含 feature 源文件；未声明的 feature 被拒绝。
- e2e 1100：workspace 中成员各自的边。

**13.7 补记：冷缓存暴露的测试隔离缺陷（已修复）。**
- 现象：xlings 版本进入 CI 沙箱缓存键，前移后 runner 的 mcpp home 为冷缓存（无 llvm；main 的 push 运行由 build 作业 `prewarm` 安装后保存，PR 不保存）。e2e `llvm/36` 在 CI 中三次失败，均在同一分片：glibc 的配置拒绝 `registry/subos/default/lib/Scrt1.o`（“user-owned or unobserved entry”）。本地均通过（开发者 home 已有 llvm）。
- 根因：`cli/commands` 的 231 在 Linux 上以用户 home 执行 `mcpp build --toolchain llvm@23.1.3`，home 没有 llvm 时就向用户 home 安装 llvm（253 MB）。同分片并行的 `llvm/36` 以运行时的载荷存在判断门控（`needs_llvm_payload`），在安装进行中看到 `bin/clang++` 而运行；它继承的 home 中 llvm 不完整，于是自行安装 llvm 与 glibc，而 glibc 的物化遇到继承来的 subos 链接（指向用户 home 的载荷，不在用例的记录中），按 xlings 的规则拒绝。证据：同一程序中 `llvm/37` 的能力探测与 `llvm/101` 的检查都报告 llvm 不存在（mtest 以种子打乱顺序，二者先于 36 运行）。
- 设计：用例不向共享 home 安装工具链，只测量 home 已有的（与 671 相同的写法：home 有 llvm 时测量，否则打印 `NOT MEASURED`）。
- 随附的隔离修正：`inherited` home 原先把用户的 `registry/subos` 整个链接进用例 home，用例的安装因此写入用户的 subos。现在用例的 subos 是副本，链接保持为链接（`cp -a`，`fs::copy_options::copy_symlinks`），与 `_inherit_toolchain.sh` 对包目录已有的处理理由相同。subos 是链接的集合（开发者 home 上约 1600 个），复制的代价与链接相当。它不是 `llvm/36` 的原因：只改它之后仍然失败。
- 同一运行中的另一失败 `signals/340`：support 的 `processes()` 读 `/proc/<pid>/cmdline` 时进程恰好结束，读返回 ESRCH，libstdc++ 的 `basic_filebuf::underflow` 抛出异常。读取期间结束的进程按不存在处理。
