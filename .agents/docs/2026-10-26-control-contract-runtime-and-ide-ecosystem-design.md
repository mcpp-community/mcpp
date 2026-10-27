---
subject: ecosystem
status: active
---

# 控制契约、并发与性能、单一 C++ 运行时，以及 IDE 生态（生态级，#838、#839）

日期：2026-10-26。分析基线：mcpp `c4f8b011`（2026.10.24.1 之后的 main）；xlings `f437e780`；
mcpp-vscode `432325a`（0.9.0）；mcpp-language-server `0.0.17`；mcpp-plugins `0.19.1`；
mcpp-index（mcpplibs）本机快照。

**修订记录**

- rev 1：由一次讨论收敛而来（问答与结论见附录 A），评审前的完整稿。写作中实测了 O7（E19、E20）：SYCL 单元跨运行时按类型捕获 `sycl::exception`，孤岛不成立，§6.3 增加条件 6，sycl 改为只走 U。
- rev 2（第一轮评审）：
  - O1 采纳 `--deny`。
  - O2：孤岛声明**不放在 `[package]`**。改为复用已有的 `requires` 与依赖的 `visibility` 词汇：`requires` 增加对象形式
    `{ layer = "…", visibility = "private" }`，并增加按目标的写法 `[target.<sel>] requires`（对称于 2026.9.12.3 的 `requires_abi`）。理由见 §6.3。
  - O3：**新建仓库 mcpp-ide，不改 mcpp-vscode 的仓库名**；README 参照 mcppls；插件名沿用 `mcpp-xxx` 模式（VS Code 仍发布为 `mcpp-vscode`，JetBrains 为 `mcpp-clion`）。
  - O4 采纳（webui 只渲染；后端经 xlings 索引分发）。
  - O6 按索引习惯：`gcc.libstdcxx`（对称于 `llvm.libcxx`），版本为"gcc 版本.包修订号"。
  - mcppls：本批不提 PR，改为 issue [Sunrisepeak/mcpp-language-server#71](https://github.com/Sunrisepeak/mcpp-language-server/issues/71)。
  - 新增 §23 多视角自审，自审结论已并入正文（链接产物原子替换、修改段内复核计划、外来运行时按 DT_NEEDED 识别、退出码 5 与 130/143、启动描述只含 mcpp 注入的变量、`--home` 组合规则、各格式下孤岛条件的差异等）。
- rev 3（实测收口与实施调整）：
  - **IDE 一线本批不实施**：mcpp-vscode 不动，计划记录于 [mcpp-vscode#23](https://github.com/mcpp-community/mcpp-vscode/issues/23)（含 O9）；mcppls 记录于 #71。mcpp 的控制契约照常实施（agent、CI 同样需要）。
  - 开放问题全部关闭（§22）：O5、O8、O10 由临时 PR [mcpp#840](https://github.com/mcpp-community/mcpp/pull/840) 实测（run 40221149103、40222782650）；O7 由分析与本机测量关闭（E21）。
  - D22 按平台定形：POSIX 先写临时名再改名；PE 先把旧产物改名挪开再原地链接（E23）。
  - 持久计划（D6）落到已有机制：配置记录 + `build.ninja`，有效性由 `admit_recorded_build`（#757 起的唯一闸门）判断；本批把记录改为每配置一个文件，并让查询读取它（§3.2）。依据：X1 之后 `toolchain list` 64 ms，`emit build-database` 的剩余开销是规划本身（E22）。
  - 锁的范围从"指纹确定"开始：规划的 0–11 阶段（读清单、解析、联网、工具链、扫描）不持锁，收尾阶段与 ninja 持锁（§4.1）。
  - X2 不需要单独的迁移步骤：认领规则在每次投影变更时生效，home 在第一次变更时自行完成迁移（§10.2）。
  - §20 增加任务依赖图。
- rev 4（实施收口，2026-10-26）：以下各项按实现与实测修订正文，正文中与之冲突的旧表述以本条为准。
  - **持久计划**：不新增计划格式。计划仍是配置记录 + `build.ninja` + 配置级编译数据库，有效性只由 `admit_recorded_build` 判断；
    快速路径保留，作为读取记录的一方（它们在接纳记录之前取构建目录锁）。`target/.build_cache` **不拆分**：读改写在
    `target/.build_cache.lock` 下进行并原子替换，格式不变，因此 §17 的"旧格式迁移"一行取消。
  - **查询不读记录**：编译器探测缓存（`<home>/cache/probe`，键为命令与引号内文件的 stat）与提供者握手缓存
    （`<home>/cache/provider-handshake`）之后，`emit build-database` 71 ms、`toolchain list` 44 ms，已达 §21.3 目标（E28）。
  - **编译数据库**：根目录副本改为按需后，完整构建中的 `compile-commands` 阶段为 14 ms（mcpp 自身），不再单独跳过。
  - **锁**：`mcpp test` 持锁直到测试结束（测试在同一构建目录运行刚链接的程序）；`run` 在 exec 前释放。
  - **命令形状**：`mcpp run --print-launch`（`run` 的 `--format` 表示可分发物）；`mcpp pack --list-formats` 只输出 JSON；
    `index list --format json` 给出注册表与工程的 `[indices]`，包目录由 `search --format json` 回答（不带关键字时列出全部）。
  - **pack 不加事件流**：`mcpp pack --message-format json` 已有单文档输出（docs/50），加事件行会破坏按单个 JSON 解析的使用者；
    需要构建事件的调用方先 `mcpp build --message-format json`。事件流不含 `result` 事件，结果即信封。build 与 pack 的
    信封把 mcpp 自身的错误作为诊断携带。
  - **取消**：`cancelled` 记录在修改段开始时预先生成，信号处理器在结束受管进程组之后用 `open`/`write`/`rename` 落盘，
    然后按默认动作结束（POSIX 状态 `128 + N`）。ninja 进程组仍以 SIGKILL 结束（既有理由：ninja 只在等待子进程时检查信号）。
    Windows 的状态是系统的 `STATUS_CONTROL_C_EXIT`，不改写为 130（D23 的 Windows 部分据此修订）。
  - **compiler-runtime 规则不变**：图提供的 libstdc++ 在 clang 下链接为 `-stdlib=libstdc++ -fuse-ld=lld
    --rtlib=compiler-rt --unwindlib=libgcc`，提供包不声明 `compiler-runtime`；§6.4 第二条取消。
  - **构建程序环境**：`MCPP_CXX_STDLIB` 取依赖图的 `c++-abi` 供给（构建程序在目标侧解析之前运行，因此按声明读取），
    新增 `MCPP_CXX_ABI_ROOT`（供给包根目录），规则包据此让设备编译器使用同一个库。
  - **条件 4** 检查孤岛包的 role=Object 动作产出（规则包产出的目标文件），不检查普通编译单元。
  - **拒绝信息不点名实现包**：SPEC-002 §1.1 禁止引擎持有实现名，因此 `requires` 不满足时的补救写为
    `help: depend on a package that declares provides = ["mcpp:c++-abi=libstdc++"]`；`gcc.libstdcxx` 由 docs/22、docs/42 与
    index 的 sycl-runtime 描述给出。
  - **`mcpp.lock` 乐观写入**：不一致时不在进程内重新规划，以 `[lockfile-concurrent]` 失败并给出 `help: run the command again`
    （重跑即重新规划）。写入改为原子替换，符号链接写到其目标，只读文件照旧拒绝。
  - **清单诊断的位置**：清单拒绝按消息点名的 `[表].键` 定位到该值（或表头）所在行；信封诊断从消息开头的位置得到
    `path` 与 `range`（docs/50 §2）。
  - **补救行统一为 `help:`**（原 `hint:`），写入 SPEC-012。
  - **D13 的 index 测量（E27）**：本机 registry 中 `DT_NEEDED` 含 libstdc++ 的共享库属于 dpcpp、glslang、mesa（驱动、
    xatracker）、slang、libllvm、qt；index 描述文件按名链接的只有 `compat.sycl-runtime` 的 `libsycl`（本批声明 `requires`）；
    mesa 的驱动在运行期 dlopen，不在链接行上；其余不被 index 包按名链接。因此 D13 对 index 的新增拒绝只有 llvm 下的
    SYCL，新增警告为 0。
  - **X4（xlings，新增）**：项目 store（mcpp 为工程索引维护的 `.mcpp/.xlings`）的 subos 只有已安装包的 shim，没有 `xlings`
    入口；安装 `gcc.libstdcxx` 时 `xim:gcc` 的 config hook 以 `system.bindir()/xlings script` 重写 specs，报
    `sh: …/subos/_/bin/xlings: not found`。xlings 在为 hook 配置 bin 目录时补上指向 home 入口的链接（与其旁 shim 指向同一
    二进制）；`tests/e2e/project_hook_runs_xlings_test.sh` 在 2026.10.24.1 上失败。
  - **`gcc.libstdcxx` 只有一个版本**：描述文件的依赖边按平台而不按版本，一个描述文件只能跟随一个 GCC 发布，取 mcpp 默认
    gcc 工具链所用的 16.1.0（§12.1 的"按 `xim:gcc` 已有版本各一个"据此修订）。sycl-consumer 与 index 的
    `sycl-runtime` 成员按条件声明依赖：`--no-accel` 的 CPU 变体仍用 libc++；index 成员在 Linux 上陈述 gcc，llvm 组合由新成员
    `libstdcxx-llvm` 覆盖。
  - **mcpp-plugins 的 sycl 规则读环境变量** `MCPP_CXX_ABI_ROOT` 而非 `mcpp::cxx_abi_root()`：后者在旧引擎上不存在，用 gcc 的
    旧引擎用户因此不受影响；整个插件包的引擎下限不因一个规则提高。
  - **计划等价**（§21.1 第 10 条）：不新增计划格式后，等价性即既有快速路径的正确性，由 #544、#716、#755 与 riscv 的 e2e
    覆盖，不另设语料测试。
- rev 5（2026-10-27，[mcpp#845](https://github.com/mcpp-community/mcpp/issues/845)）：增加 §24"程序起点与镜像入口"与 D24，
  在本批 mcpp PR（#842）中实现。原议的 `entry = "<依赖>"` 加 feature 声明的写法不采纳（§24.6）。
- rev 6（2026-10-27）：增加 §25"拆除 `modules/platform`"与 D25，在本批 mcpp PR（#842）中实现，mcpplibs.os 发布 0.2.0。
- rev 7（2026-10-27，§25 实施收口）：§25.3 的去向按实现修订（`mcpp.invocation` 为独立包 `modules/invocation`，锁的
  实现单元在 `src/native/<system>`）；§25.5 第 2 条的命令解释器范围按实现写明（mcpp 以文本组成并记录的命令行同 ninja 规则一样
  由解释器执行）；增加 §25.7（GCC 16 的模块接口约束，实测）与 §25.8（openkal 补丁发布中实测发现的两处行为）。

关联：**mcpp#838**（sycl lane 下 `std::exception` 匹配不到）、**mcpp#839**（同配置并发构建在同一构建目录竞争、双方失败）、
mcpp#596（unwinder 被拆成两半）、mcpp#514（两份 libc++ 头文件）、mcpp#544 / #716 / #755（快速路径的拒绝清单）；
[SPEC-002](../../docs/specs/target-side.md)、[SPEC-003](../../docs/specs/exit-codes.md)、
[SPEC-010](../../docs/specs/home-initialization.md)、[docs/50](../../docs/50-machine-output.md)、
[docs/22](../../docs/22-target-side.md)。

复现：[2026-10-26-control-contract-runtime-and-ide-ecosystem-repro.sh](2026-10-26-control-contract-runtime-and-ide-ecosystem-repro.sh)
（场景 A–I，只用 mcpp home 中已有的载荷；`FRESH=1` 时另建一个全新 home 复现 X2；场景 I 需要 `xim:dpcpp` 7.1.0）。

读者：评审与实现者。涉及六个仓库：mcpp、openxlings/xlings、mcpp-community/mcpp-plugins、
mcpplibs/mcpp-index、Sunrisepeak/mcpp-language-server，以及新建的 mcpp-community/mcpp-ide
（由 mcpp-vscode 演进）。**每个仓库一个 PR，PR 内用 commit 作检查点**（§20）。

**本设计的边界**：

- **mcpp 是基础设施**。只放三类东西：构建语义、对 mcpp 自身行为的标准化契约、通用基础设施原语
  （锁、取消、原子发布、记录）。和构建系统没有直接关系的东西不进 mcpp。判据：**不打开 IDE 的场景
  （脚本、CI、agent）也需要它，才进 mcpp**。优先扩展已有机制，不新增概念。
- **IDE 的通用逻辑在独立的 C++ 仓库 `mcpp-ide`**，用 `mcpp.toml` 构建（形态参照 mcppls），各 IDE 插件只做接入与 UI。
- **mcpp 本身必须很好操作**：终端里的人、只有 shell 的 agent，不经过 IDE 或 MCP，也能完成全部事情。
- 清单优先（`mcpp.toml` 为主，`config.toml` 只给默认值），核心知识判据沿用 2026-10-23 设计的 D18。
- 一次做完，不分期；兼容靠显式的 COMPAT 读法和移除下限，不靠分阶段。

## 0. 结论

| # | 决定 |
|---|---|
| D1 | **调用链模型**：人、agent、IDE、CI 的每条调用链都终止于 mcpp，mcpp 是唯一公共节点。必须在所有链上成立的东西放进 mcpp；只有一条链需要的放在那条链自己的层（§2） |
| D2 | **跨链观察靠 mcpp 落盘的规范化记录**，不靠常驻进程转发。agent 在 shell 里构建，IDE 读记录就能看到（§5.2） |
| D3 | **mcpp-ide 只做 IDE 后端，不提供 MCP**；agent 一律直接调用 mcpp（§14） |
| D4 | **不设单独的 agent 模式**。现有机器协议升格为统一的**控制契约**，IDE 后端与 agent 共用；范围包括非 TTY 下的人类输出（§5） |
| D5 | **构建流水线分三段：规划（只读，可联网，不加锁）→ 修改（加构建目录锁）→ 执行（释放锁后运行程序或测试）**（§3） |
| D6 | **规划的产出持久化为"计划"**：每个配置一份记录（取代共享的 `target/.build_cache`）加 `build.ninja` 与配置级编译数据库；有效性只由 `admit_recorded_build` 判断（#757 起已是唯一闸门）；build、run 与查询（`emit build-database`）在记录有效时不再规划（§3，rev 3） |
| D7 | **锁的粒度最小**：每个构建目录一把，只覆盖修改阶段；读操作永不等锁；`mcpp.lock` 用乐观并发；锁描述符 CLOEXEC，`run` 在 exec 前释放（§4） |
| D8 | **副作用约束是 `--offline` 的推广**，不是新概念：`--deny <effects>`，`--offline` ≡ `--deny network`（§5.5） |
| D9 | **一个进程一套 C++ 运行时（U，默认）**；孤岛（I）只在组件显式声明且引擎能机械验证时成立；宿主决定（H）由 target base 陈述（§6） |
| D10 | **工程层面不新增"标准库"配置项**。`c++-abi` 是目标侧的独立一层，按已有模型由依赖选择；llvm 加 libstdc++ 通过依赖一个 libstdc++ 提供包实现（与 openkal-llvm-runtime 对称）（§6.2） |
| D11 | **`requires` 不满足时拒绝，并给出一行修复**；解析不自动补提供方（§6.3） |
| D12 | **`abi:` 能力并入 `requires = ["mcpp:<层>=<实现>"]`**，旧写法作 COMPAT 读法；发布前清点 index 中把 `c++-abi` 当编译器代名词的用法（§6.7） |
| D13 | **删除 ldflags 中搜 "stdc++" 子串的推断**；未声明的外来运行时按孤岛规则校验：可靠的放行并提示补声明，不可靠的拒绝（§6.6） |
| D14 | **工程根目录的 `compile_commands.json` 默认不写**；`mcpp.toml` 配置输出路径，`config.toml` 可给全局默认；已有副本停止更新，提示一次（§7） |
| D15 | **颜色**：识别 `CLICOLOR_FORCE` / `FORCE_COLOR`；`NO_COLOR` / `MCPP_NO_COLOR` 优先（§5.9） |
| D16 | **性能**：xlings interface 的固定 0.5 s 等待消除；缓存条目在发布时记录大小；编译器探测不经 shell 并缓存；查询命令走持久计划（§8） |
| D17 | **xlings 升级缺陷（X2）**：认领能证明是自己创建的未登记条目，`self init` 完成迁移，报错可操作；mcpp 透传 xlings 的诊断（§9.2、§10.2） |
| D18 | **home 选择规范升为生态级**：xlings 补上 `self init --home` 与持久选择；mcpp 与 xlings 都增加单次调用的全局 `--home`（§9.1、§10.3） |
| D19 | **mcpp-ide 是新仓库**（不改 mcpp-vscode 的仓库名）：C++ 后端（mcpp.toml），stdio 上的 JSON-RPC；共享 spec 生成两端声明；共享 webui（只渲染）；VS Code 适配层仍发布为 `mcpp-vscode`，JetBrains 为 `mcpp-clion`；依赖 mcppls 提供语言功能；README 参照 mcppls（§14） |
| D20 | **孤岛声明复用 `requires` + `visibility`**：`requires = [{ layer = "mcpp:c++-abi=libstdc++", visibility = "private" }]`，可写在 `[target.<sel>]` 上；不新增键名（§6.3） |
| D21 | **libstdc++ 提供包名 `gcc.libstdcxx`**，版本"gcc 版本.修订号"，内容借用 `xim:gcc` 载荷（§12.1） |
| D22 | **链接产物原子发布**：POSIX 先写 `<out>.tmp` 再改名；PE 先把旧产物改名挪开（`<out>.old-<n>`）再原地链接，正在运行的程序保留其文件。执行段因此无需持锁（§4.1、§4.3、E23） |
| D23 | **退出码**：副作用被拒绝为 `5`；被信号取消为 `128 + 信号`（130、143）；写入 SPEC-003（§5.4、§5.5） |
| D24 | **入口分两层**：程序起点（L1，`main` 指向的翻译单元及 PE 上的入口函数）属于程序，可由依赖的 `kind = "entry"` 目标提供（`main = { from = "<依赖>/<入口>" }`）；镜像入口（L0，`start` 符号）属于面向启动者的一层，只在没有 C 运行时的镜像上可写，可由目标、入口目标或板级包陈述，不一致即拒绝（§24） |
| D25 | **mcpp 不保留操作系统层**：`modules/platform` 删除。操作系统能力直接调用 mcpplibs.os，缺少的能力加入 mcpplibs.os（0.2.0）；`platform` 名下属于 mcpp 自身语义的代码（副作用约束、调用环境、运行时搜索与加载器环境、命令行拼写、平台键、Apple SDK、脚手架、终端交互）归入各自所属的 mcpp 模块（§25） |

## 1. 问题与证据

### 1.1 实测（Linux x86_64，mcpp 2026.10.24.1，除注明外均在本机）

| # | 现象 | 数据 |
|---|---|---|
| E1 | **#838 最小复现，无需 sycl**：llvm@22.1.8，`[build] ldflags = ["-L<gcc>/lib64", "-l:libstdc++.so.6"]`（sycl 规则追加的就是这个），测试调用一次 `std::make_exception_ptr`，再 `catch (const std::exception&)` 库里抛出的 `runtime_error` | 输出 `std::exception missed -> (...)`，退出 1。`__cxa_throw`、`__cxa_begin_catch`、`typeinfo for std::exception` 来自 `libc++abi.a`（本地、被 `--exclude-libs` 隐藏）；`__gxx_personality_v0`、`typeinfo for std::runtime_error` 来自 `libstdc++.so.6` |
| E2 | 同上，把 `libc++.a libc++abi.a` 手动移到 `-l:libstdc++.so.6` 之前重链 | 正确捕获，退出 0；对 libstdc++ 的 `GLIBCXX` 导入为 0 |
| E3 | `mcpp build` 产出的 `libxrt.so`（ToolchainCoupled） | NEEDED 顺序 `libstdc++.so.6` 在 `libc++.so.1` 之前；`__cxa_throw`、`__gxx_personality_v0`、typeinfo 绑到 `@CXXABI_1.3` / `@GLIBCXX_3.4`；同时 NEEDED `libunwind.so.1`，进程内两套 unwinder |
| E4 | **#839**：24 个模块的工程，两个冷构建间隔 0.1–0.5 s 启动，5 轮 | **3 轮双方都失败**（`failed to write/read compiled module: No such file or directory`）；2 轮成功但都完整编译了一遍 |
| E5 | dev 与 `--release` 并发 | 都成功（指纹不同，构建目录天然分开） |
| E6 | `mcpp run` 的程序运行中修改源码再构建 | 成功，1.3 s；旧进程输出旧结果，新二进制输出新结果 |
| E7 | 构建过程中执行 `emit build-database` | 成功，未被阻塞 |
| E8 | 命令耗时（hyperfine，热缓存） | `--version` 0.45 ms；`--protocol-version` 1.8 ms；`self env` 2.2 ms；`build`（无改动）49.6 ms；`run`（无改动）52.4 ms；**`emit build-database` 604 ms**；**`toolchain list` 554 ms**；**`cache list` 376 ms** |
| E9 | `emit build-database` / `toolchain list` 的时间分解（strace） | xlings `interface payload_status` 约 590 ms；`interface --version` 6 ms。`payload_status` CPU 只有 40 ms，墙钟 510 ms：心跳线程一次 `nanosleep(0.500 s)`，主线程 futex 等待 0.299 s。查 1 个还是 8 个目标都是 517 ms |
| E10 | `cache list`：5.0 GB、61,265 个文件、580 个条目 | 61,742 次 `stat`、约 39,000 对 mmap/munmap、9,315 次 openat，全是 CPU 开销 |
| E11 | 完整构建的阶段计时（`MCPP_LOG_LEVEL=info`） | 规划 64–84 ms；**`compile-commands` 119–126 ms**；emit-ninja 19–22 ms |
| E12 | X2：全新 home 中先装 gcc 16.1.0，再在 default subos 里切到 15.1.0 再切回 | 正常 |
| E13 | 同一全新 home，只删掉 `subos/default/.xlings.json` 中 `libatomic.so` 的两条登记，链接保留，再切换 | `user-owned or unobserved entry preserved`，与本机旧 home 的错误逐字相同。按文件登记从 xlings 2026.10.9.1（#641）起引入 |
| E14 | clang 22 + libstdc++ 15.1.0 / 16.1.0 | `bits/std.cc` 预编译成功（需要 `-isystem <c++>/backward`）；`import std` 编译、链接、运行（`println`、`format`、`ranges`、异常、`jthread`）；产物只 NEEDED `libstdc++.so.6`、`libgcc_s.so.1` |
| E15 | clang 22 + libstdc++ 16.1.0，`.so` 抛 `runtime_error`，可执行文件 `catch (const std::exception&)` | 跨 `.so` 正确捕获 |
| E16 | `mcpp run` 的退出码 | 程序返回 0/3/255 → 同值；256 → 0；异常 134；segv 139（POSIX 上 mcpp `execve` 替换自身） |
| E17 | 用 clangd 22 检查 gcc lane 的 `import std` | `module 'std' not found`；llvm lane 无错误；C++20 + `#include` 对照组无错误 |
| E18 | 管道 vs 伪终端下的 `Running` | 管道：无颜色码（CLion 按 stderr 涂红）；PTY：`ESC[1mESC[92m`（绿） |
| E19 | 冲突集 ∩ SYCL 单元的未定义符号（dpcpp 7.1.0，`-fsycl -std=c++17 -O2 -fPIC --gcc-install-dir=<gcc 15.1.0>`） | `saxpy.o` 21 个、Tensorvia `runtime` 10 个、`arithmetic` 13 个、`matmul` 13 个，全部是 ABI 层与 `std::exception` / `std::bad_alloc`；布局相关成员 0 个 |
| E20 | 同上单元引用的、冲突集之外的 typeinfo | `saxpy.o`、`arithmetic`、`matmul`：`typeinfo for sycl::_V1::exception`（定义在 `libsycl.so`） |
| E21 | HIP（O7） | 本生态的 HIP 只有 NVIDIA 平台：`mcpp.rules.hip` 用工程自己的 clang 编译 `.hip`（`-x cuda`），AMD 平台被规则拒绝（无 ROCm 载荷）；`libcudart_static.a` 未定义的 C++ ABI 符号只有 `__cxa_atexit`（glibc 提供）。没有第二套 C++ 运行时，HIP 无需声明 |
| E22 | X1 后的查询耗时（本机，xlings 发布构建替换进 home 的符号链接副本） | `xlings interface payload_status` 525 → 23–31 ms；`mcpp toolchain list --format json` 569 → 64 ms；`mcpp emit build-database`（mcpp 仓库自身）912 → 417 ms，剩余为规划时间 |
| E23 | 并发重链与启动（4 个启动线程，25 次重链；mcpp#840） | Linux bfd 原地写：4 896 次 EACCES；gold：589 次 ETXTBSY；macOS ld64 原地写：允许覆盖运行中的程序，出现 ENOEXEC；先写临时名再改名：Linux、macOS 均 0 次错误。Windows：运行中的 exe 不能被替换（错误 5）但可以改名；改名挪开后原地链接，启动只会失败于"文件不存在 / 共享冲突"，不会执行半个文件 |
| E24 | 锁语义（mcpp#840） | Linux、macOS：`O_CLOEXEC` 的 flock 在 exec 时释放，非 CLOEXEC 被程序继承；SIGKILL 释放。Windows：`LockFileEx` 立即失败返回 33；阻塞获取在持有者被终止时立即返回；句柄不被继承 |
| E25 | PE 上的孤岛（O10；MinGW g++ 16.2 libstdc++ DLL + llvm-mingw 20261020 libc++；mcpp#840） | C 接口：正确；C++ 接口按类型捕获：只有 `catch(...)` 接住（条件 6 成立）；我们一侧两个 libc++ 映像 + libstdc++ DLL：跨我们映像按类型捕获正确（条件 1 放宽）；libstdc++ 导入库在 libc++ 之前：exe 从 `libstdc++-6.dll` 导入 `__cxa_throw`、personality、`logic_error`，完全不导入 `libc++.dll`（#838 的 PE 形态，条件 3、5 成立）；在其后：正确 |
| E26 | llvm + libstdc++ 的其他组合（O8；本机与 mcpp#840） | x86_64：`import std.compat`、ASan / UBSan / TSan、跨 `.so` 按类型捕获全部正确；aarch64 Linux：index 没有 llvm 载荷（`E_UNSUPPORTED_TARGET`），组合不存在 |
| E27 | D13 的 index 测量（rev 4） | 本机 registry 中 `DT_NEEDED` 含 libstdc++ 的共享库：dpcpp 13 个、qt 175 个、qt-base 65 个、slang 6 个、glslang 4 个、mesa 3 个（`libgallium`、`libvulkan_radeon`、`libxatracker`）、libllvm 1 个；index 描述文件按名链接的只有 `compat.sycl-runtime`（`libsycl.so`） |
| E28 | 查询耗时（rev 4，本机，xlings 2026.10.26.1） | `mcpp emit build-database` 71 ms；`mcpp toolchain list --format json` 44 ms；`mcpp build`（无改动）50 ms；`cache list` 117 ms（旧条目，迁移后读记录）；完整构建的 `compile-commands` 阶段 14 ms |

### 1.2 缺陷、设计缺口与根因

| 编号 | 问题 | 根因 |
|---|---|---|
| B1 | #839 并发构建冲突 | 工程构建目录是唯一没有跨进程保护的共享可变资源（全局侧：索引刷新非阻塞锁、BMI 缓存按条目锁、工具链按版本目录，已经细粒度） |
| B2 | #838 异常 ABI 被拆成两半 | 链接规则是 `$cxx … $ldflags $unit_ldflags`（`ninja_backend.cppm:2196–2202`），外来运行时在 `$ldflags` 里先于自身运行时归档；更深一层是两套 Itanium 运行时共用 ABI 符号名，多映像时没有可靠的链接写法 |
| B3 | 外来运行时靠字符串推断 | `flags.cppm:1452` 在 ldflags 里搜 `stdc++` 子串；规则包用 `link_lib(":libstdc++.so.6")` 写字符串，绕开了已有的有类型通道（`requires = ["mcpp:c++-abi=…"]`） |
| B4 | 快速路径的正确性靠清单 | `try_fast_build` / `try_fast_run` 各自重放、各自维护拒绝清单，#544、#716、#755 与 riscv 那次都是漏项；查询命令没有快速路径 |
| B5 | 查询慢 | xlings `heartbeat_loop_` 的 `sleep_for(500ms)` 不可中断（`interface.cpp:456`）；stdin 线程 100 ms poll；`cache list` 现算大小；探测经 `sh -c env …` |
| B6 | X2 升级缺陷 | 2026.10.9.1 之前投影的链接只在 home 顶层别名表里，没有迁入 subos 的按文件登记 |
| B7 | IDE 标红（gcc lane） | gcc 的 `.gcm` 不能被 clangd 读；根目录 `compile_commands.json` 让 CLion 以 Compilation Database 接管工程，mcppls 按设计让出 |
| B8 | 外部工具无法强制颜色 | `detect_color` 只看 TTY 与 `NO_COLOR`（`ui.cppm:435`） |
| B9 | 契约缺口 | build/pack 无事件流；无启动描述；无结果记录；取消语义、并发语义未规范；索引与搜索无 JSON；打包格式不可查询 |

## 2. 调用链模型

参与方：人、agent、IDE（适配层 + mcpp-ide 后端）、CI、mcpp；旁及 mcppls 与被构建的程序。

```
A  人 ─────────────────────────────→ mcpp          终端
B  人 → IDE（适配层 → mcpp-ide）───→ mcpp          按钮、运行配置
C  agent ─(shell)──────────────────→ mcpp          今天 agent 的主路径
D  人 ─委托→ agent ────────────────→ mcpp          权限属于人
E  人 → IDE 内的 agent ─(终端)─────→ mcpp
F  CI ─────────────────────────────→ mcpp          非交互、可复现
G  mcpp → build.mcpp、规则包、工具链、程序本身
```

**多重关系**：同一工程同时被多条链触碰，共享 `target/`、`MCPP_HOME`、`mcpp.lock`、`mcpp.toml`；
观察关系有"人看 agent 做了什么""IDE 看终端里构建出了什么""agent 读 IDE 的诊断"。

**放置原则**：

1. 所有链都需要的 → mcpp。
2. 只有一条链需要的 → 那条链自己的层（会话 → mcpp-ide；原生 UI → 适配层）。
3. 直连链 A、C 必须完整；IDE 与 mcpp-ide 永远不是某个能力的前提。
4. 跨链观察靠 mcpp 落盘的记录（D2）；仲裁（并发）只能由 mcpp 做，因为终端里的人不经过任何中间层。

**能力 × 层**（每项的细节在后续各节）：

| 能力 | 需要它的链 | 层 |
|---|---|---|
| 构建、运行、测试、打包语义 | 全部 | mcpp |
| 输出信封、事件流、退出码、副作用表 | B C D F | mcpp |
| 启动描述 | B（调试）C F | mcpp |
| 结果记录落盘 | 跨链观察 | mcpp |
| 构建目录锁、取消、原子发布 | 多链并发 | mcpp |
| 副作用约束 | B（不受信任工作区）C D | mcpp |
| manifest 诊断带位置、索引与搜索 JSON、打包格式查询 | B C | mcpp |
| 非 TTY 的人类输出规范、颜色约定 | A C D | mcpp |
| 会话：监听、缓存、编排、视图模型、多工程 | B | mcpp-ide |
| 原生 UI：运行配置、测试树、调试器、面板 | B | 适配层 |
| agent 使用说明 | C D E | mcpp 文档；针对具体 agent 的封装 → mcpp-ide 仓库 `agents/` |
| 语义查询（符号、引用、验证） | B C | mcppls（已有） |

## 3. mcpp：三段式流水线与持久计划

### 3.1 三段

| 段 | 做什么 | 写入 | 锁 |
|---|---|---|---|
| **规划** | 读 manifest、解析依赖与工具链（可联网）、目标侧、扫描，直到配置指纹确定（准备阶段 0–11） | 不写构建目录 | 无 |
| **修改** | 准备的收尾阶段（13：动作、BMI 缓存、`resolution.json`、`build.ninja`、配置级编译数据库）、运行 ninja、校验产物、写配置记录与结果记录 | 构建目录 | 构建目录锁 |
| **执行** | `run` exec 程序；`test` 运行测试；`pack` 打包（读构建产物，写 `-o` 目标） | 无（pack 写其输出） | 无 |

实现（rev 3）：配置目录由指纹决定，而指纹在扫描阶段末尾才确定（`prepare/scan.cpp`）；所有写构建目录的步骤都在其后的收尾阶段
（`phase13_finish`）与 ninja 中。因此锁在收尾阶段开始时获取、随 `BuildContext` 持有到 ninja 结束，规划不需要改写成"先算后写"。

### 3.2 持久计划

rev 3：计划就是已有的**配置记录**加 `build.ninja` 与配置级编译数据库。记录写于每次完整构建之后；有效性只由
`admit_recorded_build`（`execute.cppm`）判断——#757 起它已是 `try_fast_build`、`try_fast_workspace_build`、`try_fast_run`
共用的唯一闸门：引擎身份、运行时绑定、`build.ninja` 的模式与请求、清单与源码树的时间、路径依赖、载荷目录、按路径命名的工具链、产物快照。
各路径只保留属于自己的判断（工作区选择、要运行的程序、runner、运行层工具）。

本批的改动：

| 改动 | 原因 |
|---|---|
| `target/.build_cache` 改为每配置一个文件 `target/.build_cache.d/<triple>-<fp>`，原子替换 | 共享文件的读改写在不同配置并发构建时互相覆盖（§4） |
| `emit build-database` 在记录有效时直接读取配置级数据库 | E22：X1 之后剩余的 417 ms 是规划本身；读记录不规划 |
| 读取旧的 `target/.build_cache`（COMPAT），首次写入时迁移 | 升级后第一次构建仍走快速路径之外的完整路径一次即可 |

**并发规划**：两个进程可能同时规划同一配置；后到者在修改段拿到锁之后才写，写入内容由同一组输入决定，结果相同。

### 3.3 编译数据库从计划派生

配置级 `compile_commands.json` 是计划的函数：计划不变时不重写（E11：今天每次完整构建 119–126 ms）。

## 4. mcpp：并发（修复 #839）

### 4.1 资源与保护

| 资源 | 方案 |
|---|---|
| 构建目录 `target/<triple>/<fp>/` | 每目录一把锁 `.mcpp-build.lock`，**只在修改段持有**（准备收尾阶段 + ninja；快速路径只在 ninja 期间）。后到者等待，拿到锁后 ninja 基本无事可做，等价于合并两次构建 |
| `run` / `test` 的执行段 | 不持锁。`run` 在 `execve` 前释放；锁描述符 CLOEXEC（POSIX 的 `FileLock` 已用 `O_CLOEXEC`，`fs.cppm:373`），保证程序不继承锁。执行段不持锁之所以安全，靠下一行的原子替换 |
| 链接产物（可执行文件、共享库、测试二进制） | **原子发布**（D22，E23）。POSIX：链接写 `<out>.tmp`，再 `mv -f` 改名（同目录 rename 原子）。PE：链接前把旧产物改名为 `<out>.old-<n>`，再原地链接；运行中的程序保留旧文件，下次链接清理不再被占用的 `.old-*`。由 ninja 链接边的命令实现，与链接器无关 |
| 查询（`emit`、`why`、`cache list`、读记录） | 不加锁；所有记录写临时文件后原子改名 |
| 根目录单文件（根 `compile_commands.json`、`target/.last-result.json`） | 原子替换 |
| `mcpp.lock` | 乐观并发：规划时记哈希，写入前比对；不一致则重新规划一次，仍冲突则报错，点名"并发修改"。**不在网络或解析期间持锁** |
| 全局 `MCPP_HOME` | 沿用现有细粒度模式（索引刷新非阻塞、BMI 缓存按条目、工具链按版本目录）；实现时逐个核对其余写入点 |

### 4.2 锁的实现

- `FileLock` 增加可等待的获取：非阻塞尝试加退避（50 ms 起），可被取消（§5.4）。Windows 用 `LockFileEx`（已有）。
- 锁文件内容写持有者：pid、开始时间、命令行、调用方标签（`MCPP_CLIENT`，见 §5.8）。等待超过 300 ms 时：
  人看到 `Waiting for build lock held by pid 1234 (mcpp build, from mcpp-ide, 8s)`；
  机器读到 `{"kind":"mcpp.event","event":"waiting","holder":{…}}`。
- 进程崩溃时操作系统释放锁，不存在僵死锁。
- 不新增"等待或失败"参数：默认等待；调用方取消即可。IDE 的"保存时构建，新的取代旧的"由 mcpp-ide 取消自己上一次构建实现。

### 4.3 Windows

实测（E23、E24）：正在运行的 exe 与已加载的 DLL 不能被替换（错误 5），但可以改名；改名后的文件在程序退出前不能删除。
"先写临时名再改名"在 Windows 上失败于第二步（替换运行中的文件），因此 PE 采用"先改名挪开、再原地链接"：今天正在运行时重链会以
LNK1104 失败，改后成功；与链接同时发生的启动只会失败于"文件不存在 / 共享冲突"，不会执行半个文件（Windows 不允许执行正被写入的映像）。
`LockFileEx` 的语义与 POSIX flock 一致：立即失败返回 33，持有者终止时等待者立即获得，句柄不被子进程继承。

## 5. mcpp：控制契约

### 5.1 事件流

- `mcpp build` 与 `mcpp pack` 接受 `--message-format json`：stdout 上是 NDJSON，记录 `{"schemaVersion":1,"kind":"mcpp.event","event":…}`，
  `event` ∈ `plan`、`waiting`、`progress`、`diagnostic`、`artifact`、`result`；最后一条是结果信封（`mcpp.build` / 已有的 `mcpp.pack`）。
- `mcpp test` 的测试流早于信封存在，**保持不变**（docs/50 的稳定性承诺）。
- `mcpp run` 的 stdout 属于程序，**不加事件流**。需要结构化构建的调用方先 `mcpp build --message-format json`，再按启动描述（§5.3）执行。

### 5.2 结果记录（D2）

- 修改段结束时写 `target/<triple>/<fp>/result.json`（信封 kind `mcpp.result`）：命令、请求、调用方标签、开始与结束时间、
  状态（`ok` / `failed` / `cancelled`）、诊断（带 path 与 range）、产物（含启动描述）、阶段耗时、配置级编译数据库路径。
- 工程级指针 `target/.last-result.json`：最近一次结果所在的配置。
- 读者：mcpp-ide（监听后更新问题面板）、mcppls（找编译数据库的回退路径，§13）、CI、`mcpp why`。
- 与 `resolution.json` 的关系：后者记"做了什么决定"，结果记录记"结果如何"。两者并列，都在修改段原子写入。

### 5.3 启动描述

每个可运行产物：`argv`（runner 前缀 + 程序 + 参数占位）、`env`（mcpp run 注入的 loader 环境，即今天的 `childEnv`）、`cwd`、
`runner`（名字与来源）。出现在结果记录的产物条目里；单次查询用 `mcpp run --print-launch --format json`（构建但不执行）。
调试器、IDE、CI 按它执行，与 `mcpp run` 的环境逐项一致。`env` 只含 mcpp 注入或改写的变量（loader 路径、runner 需要的变量），不含调用方的其余环境，避免把机密写进结果记录。

### 5.4 取消

- SIGINT / SIGTERM（Windows：Ctrl-C / CTRL_BREAK）：规划段直接退出（没有写入）；修改段转发给 ninja 进程组、等待、
  写结果记录（`cancelled`）、释放锁。退出码按 `128 + 信号`（SIGINT 130、SIGTERM 143；Windows 的 Ctrl-C 同样返回 130），写入 SPEC-003（D23）。
- 构建目录在取消后保持一致（ninja 的日志语义），下一次构建从断点继续。

### 5.5 副作用约束（`--offline` 的推广，D8）

- 全局 `--deny <effect,…>` 与环境变量 `MCPP_DENY_EFFECTS`；词汇沿用 docs/50 §4：`init-mcpp-home`、`write-project`、
  `write-global-cache`、`network`、`exec-build-script`（`read-project` 不可拒绝）。
- `--offline` 保留，等价于 `--deny network`。
- 运行到需要被拒绝的副作用之前停下，诊断写明副作用与原因（例：`network: install xim:gcc@15.1.0`），**退出码 5**（SPEC-003 新增，D23）。只有 shell 的 agent 靠退出码就能区分"被我的约束挡住"与"构建失败"。
- 计划有效时不会运行 `build.mcpp`，因此 `--deny exec-build-script` 下也能构建；约束针对的是本次运行实际要做的事。
  调用方（agent 的宿主、IDE 的不受信任工作区）审批后去掉约束重跑。
- 信封里的 `effects` 继续报告实际发生的副作用。

### 5.6 查询的补全

| 查询 | 形式 |
|---|---|
| manifest 诊断 | 所有 manifest 相关诊断在信封里带 `path` 与 `range`（TOML 语法、未知键、类型、指向某个键的语义拒绝）。不新增命令 |
| 搜索 | `mcpp search --format json`（kind `mcpp.search`） |
| 索引目录 | `mcpp index list --format json`（kind `mcpp.index`：namespace、name、description、licenses、repo、versions、镜像），**一次调用**取代 mcpp-vscode 对 Lua 文本的解析 |
| 打包格式 | `mcpp pack --list-formats --format json`（kind `mcpp.pack.formats`，来自已解析的依赖图） |
| 文件归属 | 已有：`emit build-database` |

### 5.7 非 TTY 的人类输出规范

新规范（`docs/specs/human-output.md`）：

1. 非 TTY 下不输出进度区域；叙述只有动词行（`Compiling <package>`、`Finished`、`Running`），不逐单元输出。
2. 每个错误在有可执行的下一步时，以一行 `help:` 结尾，给出命令或要改的键。
3. 编译器诊断原样完整输出（它就是内容）；mcpp 自己的叙述有行数上限。
4. 动词词汇固定，写入规范。

docs/50 仍声明人类输出没有兼容承诺；本规范约束的是形态，不是逐字内容。

### 5.8 调用方标签

`MCPP_CLIENT=<name>`（IDE、agent 宿主自行设置）：写入锁持有者与结果记录，便于"谁在构建"的回答。只记录，不改变行为。

### 5.9 颜色（D15）

优先级：`MCPP_NO_COLOR=1` / `NO_COLOR` 非空 → 关闭；`CLICOLOR_FORCE` 非 `0` 或 `FORCE_COLOR` 非空 → 开启；否则看 TTY。
Windows 强制开启时同样打开 VT 处理。

### 5.10 agent 使用说明

mcpp 文档新增一章（docs/52）：调用链、非 TTY 输出、副作用与审批、等待锁、结果记录、启动描述、退出码。
面向具体 agent 的封装（如 Claude Code 的 skill）放在 mcpp-ide 仓库 `agents/`。

## 6. mcpp：单一 C++ 运行时（修复 #838）

### 6.1 原理

两套 Itanium C++ ABI 运行时共用同样的 ABI 符号名：`__cxa_*`、`__gxx_personality_v0`、`std::exception` 一族
（不在 libc++ 的 `__1` 内联命名空间里）的类型信息、虚表与成员、`operator new/delete`。同一进程的全局符号空间里，
它们要么像 #838 那样各用一半，要么像 #596 那样互相串用。`std::runtime_error` 等类型在两边的**内存布局不同**
（libstdc++ 是 COW 字符串，libc++ 是 refstring），构造用一边、析构用另一边就是内存错误。
因此**没有任何链接写法能让"多映像 + 两套运行时 + 异常跨映像"可靠**。

### 6.2 模型：沿用目标侧五层（D10）

`c++-abi` 是独立的一层（docs/22）：至少两个可互换实现、可独立替换（E14 实测 clang 可换 libstdc++）、与 `c-abi` 有"为其配置"关系。
工程只用三种方式表达目标侧：triple、`[toolchain]`（只选 `compiler`）、依赖（其余各层）。因此：

- **不在 `[toolchain]` 或 `[build]` 新增标准库键**。前者会把 `compiler` 与 `c++-abi` 合成一层，后者把解析期事实当成编译参数。
- **llvm + libstdc++** = 依赖一个 libstdc++ 提供包（§12.1），与 openkal 的写法对称：

```toml
[dependencies]
gcc.libstdcxx = "16.1.0"      # provides mcpp:c++-abi=libstdc++（D21）

[toolchain]
default = "llvm@22.1.8"       # 只选编译器
```

### 6.3 三种模式

**U：统一（默认）**。一个进程一套运行时，由"每层一个提供方"（已有）决定：载荷、预编译、或依赖图。
包用 `requires = ["mcpp:c++-abi=…"]` 声明需求；不满足时**拒绝并给出一行修复**（D11）：

```
error: compat.sycl-runtime requires c++-abi = libstdc++, but this build's c++-abi is libc++ (llvm@22.1.8 payload)
  help: add a libstdc++ supplier:  gcc.libstdcxx = "16.1.0"  under [target.'cfg(accelerator = "sycl")'.dependencies]
  help: or build with a gcc toolchain:  --toolchain gcc@16.1.0
```

**I：孤岛（显式声明，可验证）**。组件声明自己的 C++ 运行时只在 C 接口之后（D20）：

```toml
[target.'cfg(os = "linux")']
requires = [{ layer = "mcpp:c++-abi=libstdc++", visibility = "private" }]
```

为什么这样写，而不是 `[package]` 下的新键：

- **`[package]` 是包的身份与元数据**；孤岛描述的是包的代码与目标侧 `c++-abi` 层的关系，属于目标侧事实。
- **它随目标变化**：孤岛只在两套运行时可能共处的目标上有意义（ELF 上的 libc++ 与 libstdc++）；在 MSVC 或 macOS 上没有对应物。
  `[package]` 不能按目标条件化，2026.9.15 把 `std-module` 从 `[package]` 挪到 `[build]` 也是同一个原因。
- **词汇已经存在**：`requires` 已经表达"某层必须解析为某实现"；依赖的 `visibility` 已经定义为"用途是否越过本包自身的边界"（docs/05）。
  `private` 的含义恰好是：这个需求只对本包自己的代码成立，不越过本包的边界，不约束映像的 `c++-abi`。字符串形式保持原义（等价于 `visibility = "public"`）。
  行业对照：Conan 2 的 requirement trait `visible=False` 表达同样的"私有需求"。
- **按目标的 `requires` 本身就缺**：index 的 Lua 描述文件可以按平台写 `requires`，TOML 却只有 `[package] requires`。
  `[target.<sel>] requires` 对称于已有的 `[target.<sel>] requires_abi`（2026.9.12.3），合并规则相同：`[package]` 的与所有匹配选择器的取并集。
解析优先级：**能统一就统一**（图的 `c++-abi` 恰为所需实现时，孤岛声明不生效）；否则在下列条件**全部**可验证时成立：

1. **我们这一侧只有一个 C++ 映像**：按根包**声明的全部目标**判定（不按本次请求的目标，保证各命令结论一致）：没有含 C++ 的
   `kind = "shared"` 目标，图中没有含 C++ 的共享依赖。自身运行时静态嵌入并隐藏（已有 `hide_static_cxx_runtime`）。
2. **一套 unwinder**：`--unwindlib=libgcc`（已有）。
3. **链接顺序**：自身运行时先于外来运行时（修 B2，E2 已验证）。
4. **冲突集判据**：链进我们映像的孤岛目标文件（规则包 action 的产出），其未定义符号不得引用"布局相关冲突集"
   （`std::exception` 一族中除 `std::exception` 自身外的构造、析构、`what`，`std::bad_alloc` 等）；ABI 层冲突集
   （`__cxa_*`、personality、`operator new/delete`）必须**整体**绑定到自身运行时。冲突集是一张随运行时版本维护的数据表。
5. **链接后校验**：我们的映像不得以 `GLIBCXX_*` / `CXXABI_*` 版本导入 ABI 层冲突集中的任何符号（E1 的坏产物正有此特征，E2 的好产物为 0）。
6. **不跨运行时按类型捕获**：孤岛目标文件不得引用由外来运行时映像（如 `libsycl.so`）定义的 typeinfo。原因：那个映像用外来运行时的
   `__cxa_throw` 抛出（异常类 `GNUCC++`），我们映像里的 personality 属于自身运行时（libc++abi 只认 `CLNGC++`），会把它当外来异常，
   只有 `catch (...)` 能接住，带类型的 `catch` 失效。

不满足 → 拒绝，给出 U 的两条修复。

**实测（E19、E20）**：冲突集按实际运行时计算（`libstdc++.so.6` 导出名 ∩ `libc++.a`/`libc++abi.a` 定义名，381 个，其中 ABI 层 56 个、
`std` 非 `__1` 名 132 个）。sycl-consumer 的 `saxpy.o` 与 Tensorvia 的 `runtime`、`arithmetic`、`matmul` 三个 `.sycl` 单元（dpcpp 7.1.0，
按 `mcpp.rules.sycl` 的参数编译）引用的冲突集符号只有 ABI 层与布局不变的 `std::exception`、`std::bad_alloc`，**条件 4 成立**；
但 `saxpy.o`、`arithmetic`、`matmul` 都引用 `typeinfo for sycl::_V1::exception`（源码 `catch (const sycl::exception&)`），**条件 6 不成立**。
结论：**SYCL 只能走 U**；孤岛机制保留给 C 接口、内部不让异常外泄的组件（例如只暴露 C API 的预编译 C++ 库）。

**各目标格式下的条件**（E25）：上述 6 条按 ELF 的全局符号作用域推导。PE 按 DLL 导入表绑定，没有全局作用域：
条件 1 放宽为"我们一侧的所有映像共用同一个运行时映像"（多个 libc++ 映像共用 `libc++.dll` 时跨映像按类型捕获正确）；
条件 3、5 照常成立（导入库的顺序决定 ABI 层从哪个 DLL 导入，链接后校验读导入表）；条件 4、6 与格式无关。
Mach-O：Apple 平台只有 libc++，不存在孤岛。MSVC ABI：只有一个 C++ 运行时家族，不存在孤岛。

**H：宿主决定**。Python 扩展、JNI、宿主插件：`c++-abi` 由 target base 陈述（2026-10-23 的目标基座模型，来源 `prebuilt`），包只能服从。

### 6.4 引擎改动

- **图提供的 libstdc++ 在 clang 下的接线**（对称于已有的图提供 libc++）：`-nostdinc++`（已有，`hostflags.cppm:502` 附近）
  加提供包的头文件目录（含 `backward/`）；链接 `-stdlib=libstdc++`；std 模块取提供包的 `[build] std-module`；
  `hostflags` 中硬编码的 `-stdlib=libc++` 改为按已解析的 `c++-abi` 选择。
- **compiler-runtime 层的规则修订**：docs/22 现写"compiler runtime 必须属于编译器自身的家族"。GNU/Linux 发行版的 clang
  默认就是 `--rtlib=libgcc --unwindlib=libgcc`，E14 也按此链接成功。规则改为"必须是编译器支持的实现"
  （clang：compiler-rt 或 libgcc；gcc：libgcc），libstdc++ 提供包同时 `provides mcpp:compiler-runtime=libgcc`。修订 SPEC-002。
- **构建程序环境**暴露已解析的 `c++-abi` 实现与提供方根目录，规则包据此决定 `--gcc-install-dir` 等，使设备半边与宿主半边用同一份 libstdc++。
- **链接顺序**：外来运行时（孤岛声明的）由引擎放在自身运行时之后，不再由规则包写进 `$ldflags`。
- **冲突集检查与链接后校验**并入 `symbol_provision`（已有的同类检查所在）。

### 6.5 解析时序

运行时需求是**静态元数据**（包的 manifest），解析阶段即可知道；规则包运行在解析之后，只负责编译与链接机制。
这回答了"构建扩展系统是否最优"的问题：会影响解析的信息必须静态声明（与 Cargo 的 `links`、Conan 的 settings、
CPS 的 `cpp_runtime_vendor` 一致），构建程序不得用字符串表达它。

### 6.6 删除子串推断（D13）

`flags.cppm:1452` 的 `stdc++` 子串推断删除。替代：识别**未声明的外来运行时**，来源有两处：链接线上直接点名的运行时库（`-l:libstdc++.so.6` 等），以及链接输入中的共享库其 `DT_NEEDED` 含外来运行时的 SONAME（数据表：`libstdc++.so.*`、`libc++.so.*`、`libc++abi.so.*`；只看直接链接输入，不递归）。这类库就是条件 6 所说的"外来运行时映像"，它们定义的 typeinfo 构成条件 6 的检查集合。识别结果视为**未声明的孤岛**，按 §6.3 的 I 条件校验：

- 全部满足 → 构建，并 `warning` 提示规则包作者补声明（COMPAT 读法，移除下限见 §17）。
- 不满足 → 拒绝，给出 U 的修复。今天能正常工作的工程照常构建；今天悄悄出错的工程得到明确的错误。

### 6.7 合并 `abi:` 能力（D12）

- `abi:<dim>=<v>`（`abi.cppm`）与 `requires = ["mcpp:<layer>=<impl>"]` 表达同一事实。本机 index：`abi:cxxstdlib` 0 个包，
  `abi:glibc` 2 个（compat.cpptrace、compat.glfw），`mcpp:c++-abi` 3 处。
- `abi:` 作为 COMPAT 读法映射到 `requires`（`abi:glibc` → `mcpp:c-abi=glibc`，`abi:cxxstdlib=X` → `mcpp:c++-abi=X`，
  `arch`、`os` 映射到目标 cfg 检查），`step13_abi_enforcement` 并入目标侧的 requires 检查。
- **语义漂移**：`mpusz.mp-units` 在 Linux 上声明 `requires mcpp:c++-abi=libc++`，注释的真实原因是 GCC 16.1 的模块缺陷，
  即约束的是编译器。`c++-abi` 可独立选择后，应改为 `requires = ["mcpp:compiler=llvm"]`。发布前清点 index 中全部 `mcpp:c++-abi` 用法（§12）。

## 7. mcpp：工程根目录的 `compile_commands.json`（D14）

- 配置级数据库（`target/<triple>/<fp>/compile_commands.json`）保持，内部产物，按 §3.3 只在计划变化时重写。
- 根目录副本**默认不写**。`mcpp.toml`：

```toml
[build]
compile_commands = "compile_commands.json"   # 相对工程根目录的输出路径；不写则不输出
```

- `~/.mcpp/config.toml` 可给同名键的全局默认（想保留旧行为的人设一次）。
- 一次性导出已有：`mcpp emit build-database --spec compile-commands -o PATH`。
- 迁移：根目录已有副本、且未配置该键时，停止更新，提示一次（`diag::note`，按工程记一次，记录在 `target/` 下），说明如何开启。
- `${mcpp.compile_db}` 替换（`prepare/plan.cpp:1021`）在未配置时指向配置级数据库。

行业对照：CMake `CMAKE_EXPORT_COMPILE_COMMANDS` 默认关闭且写在构建目录；Meson 写在构建目录；xmake 显式命令或插件。

## 8. mcpp：性能（D16）

| 项 | 改动 | 依据 |
|---|---|---|
| 查询命令 | 走持久计划（§3.2） | E8：604 / 554 ms |
| xlings 固定等待 | xlings 侧修复（§10.1），mcpp 提高内置 xlings 下限 | E9 |
| `cache list` | 条目发布时把大小与文件数写进 `entry.json`；`list` 只读 580 个小文件；旧条目缺字段时现算一次并回写（原子替换） | E10 |
| 编译器探测 | 直接 exec（不经 `sh -c env`），结果按（编译器路径、大小、mtime、参数）缓存在 `MCPP_HOME` | E9 |
| 编译数据库 | 计划不变不重写；根目录副本默认关闭 | E11 |
| 锁开销 | 无竞争时一次非阻塞 flock | §4 |

验收数见 §21.3。

## 9. mcpp：其他

### 9.1 单次调用的 `--home`（D18）

全局 `--home <dir>`：等同于只在本进程设置 `MCPP_HOME`，并按 SPEC-010 的规则解析（持久选择、定位器照常读取）。
与 `self init --home` 的组合：前者选择本进程的原始 home，后者是持久选择的目标；两者同时出现且值不同时拒绝（用法错误，退出码 2），值相同时等同于只写一个。
SPEC-010 升为生态级规范，xlings 引用同一份条文（§10.3）。

### 9.2 provisioning 失败时透传诊断（D17）

今天失败时 mcpp 建议 `xlings install …` 手动安装，而这条命令会碰到同一个错误。改为原样透传 xlings 的诊断（含其 `help`），
不再给出照做无效的命令。

## 10. xlings

### 10.1 X1：消除固定等待

`heartbeat_loop_` 改为在条件变量上带超时等待、对 `stop_token` 立即唤醒（`std::condition_variable_any::wait_for` 的 stop_token 重载）；
`stdin_loop_` 的 poll 同样可被唤醒（自管道或 eventfd）。验收：`interface payload_status` ≤ 30 ms（今天 517 ms）。

### 10.2 X2：认领可证明的条目

- **认领规则**：未登记的符号链接，若指向本 home 自己的包存储，且恰为某个已登记包版本中同名的文件，则必为 xlings 所建，补登记后继续。
  普通文件、指向存储之外的链接，仍按"用户文件"保留，安全边界不变。
- 规则在每次投影变更时生效（`prepare_projection_change` 与单项放置共用），变更成功后 subos 的登记记录新状态，home 在第一次变更时自行完成迁移，不需要单独的迁移步骤（rev 3）。
- 报错写明条目、原因、处理方式（例如 `xlings self doctor` 或手工移除）。
- 影响范围：2026.10.9.1 之前初始化、装过 gcc 的 home，切换 gcc 版本时失败（E13）。

### 10.3 X3：home 选择

- `xlings self init --home <dir>` 与持久选择（定位器文件，语义与 SPEC-010 一致）。
- 全局 `--home <dir>`：本次调用有效；导出给子进程的 `XLINGS_HOME` 与之一致。
- shim 的"按自身所在 home 定位"不变；mcpp 调用 xlings 继续经环境变量传 `XLINGS_HOME`。

## 11. mcpp-plugins

- `mcpp.rules.sycl`：删除 `link_lib(":libstdc++.so.6")`；`--gcc-install-dir` 取自构建程序环境中已解析的 libstdc++ 提供方，
  使设备半边与宿主半边用同一份 libstdc++。保留 `-lsycl` 与搜索目录。SYCL 只走 U（§6.3 实测，条件 6）。
- `mcpp.rules.hip`：同样处理；HIP 单元是否捕获运行时库抛出的带类型异常，按 E20 的方法实测后定为 U 或可孤岛（O7）。
- 文件头关于"接缝"的说明改写：异常不能从运行时库跨到我们的映像按类型捕获，因此需要统一运行时。
- fixture：`tests/sycl-consumer` 在 U 下跑两种（gcc；llvm + libstdc++ 提供包）；在 llvm 且无提供包时断言被拒绝且 `help` 给出一行修复；
  新增一个 `kind = "shared"` 的 fixture，断言 U 下跨 `.so` 捕获成功。

## 12. mcpp-index（mcpplibs）

### 12.1 libstdc++ 提供包 `gcc.libstdcxx`（D21）

```toml
provides = ["mcpp:c++-abi=libstdc++", "mcpp:compiler-runtime=libgcc"]
requires = ["mcpp:compiler=llvm", "mcpp:c-abi=glibc"]

[build]
std-module        = "<c++>/bits/std.cc"
std-module-flags  = ["-isystem", "<c++>/backward"]
```

命名按索引习惯：上游项目作命名空间、组件作名字，对称于 `llvm.libcxx`、`llvm.compiler-rt-builtins`。版本为"gcc 版本.包修订号"（`16.1.0.1`、`15.1.0.1`），跟随 `xim:gcc` 已有的版本。
内容借用 `xim:gcc` 载荷（不复制、无源码仓库）：头文件、`bits/std.cc`、`libstdc++.so.6`、`libstdc++.a`、`libsupc++.a`。只提供 `linux`（`requires mcpp:c-abi=glibc`）；musl 目标若需要，另由 `xim:musl-gcc` 以同样方式提供，不在本批。

### 12.2 声明与清点

- `compat.sycl-runtime`：声明 `requires = ["mcpp:c++-abi=libstdc++"]`（U；E20 表明不能孤岛）。HIP 对应包若存在，按 O7 的实测结果声明。
- `mpusz.mp-units`：`requires mcpp:c++-abi=libc++` → `requires mcpp:compiler=llvm`。
- `compat.cpptrace`、`compat.glfw`：`abi:glibc` → `requires mcpp:c-abi=glibc`。
- `compat.mysql-connector-cpp`：保持（Linux 上的预编译静态库确实绑定 libstdc++）。
- 发布前清点全部 `mcpp:c++-abi` 与 `abi:` 用法，逐条判定"约束的是哪一层"。

## 13. mcpp-language-server

**本批不提 PR**：mcpp 的主路径（`emit build-database`）不变，影响有限。已提 issue [Sunrisepeak/mcpp-language-server#71](https://github.com/Sunrisepeak/mcpp-language-server/issues/71)，记录 mcpp 发布后要做的三件事：

- 回退路径：mcpp 无法询问时，今天读根目录残留的 `compile_commands.json`（`src/project/mcpp.cppm:41`）。根目录副本默认关闭后，
  回退改为读 `target/.last-result.json` 指向的配置级数据库。
- 构建后刷新：监听结果记录（§5.2），取代由 mcpp-vscode 转发的刷新。
- CLion 插件：根目录副本不再默认存在，CLion 不再以 Compilation Database 接管 mcpp 工程，mcppls 自然启用；无需改动，补一条 e2e 断言。

## 14. mcpp-ide（新仓库，由 mcpp-vscode 演进）

**rev 3：本批不实施**。mcpp-vscode 不动；本节的计划（仓库、命名、三层边界、能力表、O9）记录于
[mcpp-vscode#23](https://github.com/mcpp-community/mcpp-vscode/issues/23)。本批交付的是 IDE 一线所依赖的 mcpp 契约（§5），
它们同样服务于 agent 与 CI（D1），不依赖 IDE 一线的进度。

**仓库与命名（D19）**：新建 `mcpp-community/mcpp-ide`，**不改 mcpp-vscode 的仓库名**。插件名沿用 `mcpp-xxx` 模式：
VS Code 适配层仍以 `mcpp-vscode`（publisher `mcpp-community`，扩展 ID 不变）发布，Marketplace 上的用户无感升级；
JetBrains 适配层发布为 `mcpp-clion`。mcpp-vscode 仓库在 mcpp-ide 首个发布之后的去向见 O9。

**README 参照 mcppls**：标题与一句话定位；`**English** | 简体中文`；文档、安装、设置、For agents、规范、发布的链接行；
两段说明（问题是什么、mcpp-ide 怎么解决）；演示图；`Features`；`Install`（From an agent / From a release / From source）；
`Documentation`；`Contributing`；`License`。中英双语（`README.md`、`README.zh-CN.md`）。

### 14.1 结构

```
mcpp-ide/
  mcpp.toml        C++ 后端（mcpp 构建），二进制 mcpp-ide，`mcpp-ide serve`：stdio 上的 JSON-RPC
  protocol/        协议 JSON Schema + 来自 mcpp 真实输出的 golden fixture（两端适配层共同验收）
  spec/            命令表、设置注册表（沿用 config-registry.json）、菜单表、i18n → 生成 VS Code package.json 声明部分与 plugin.xml、设置类、Bundle
  webui/           共享面板：依赖图、库、缓存、设置、打包（VS Code webview / JetBrains JCEF 加载同一份）
  vscode/          TS 适配层
  jetbrains/       Kotlin 适配层（CLion 2025.2+）
  agents/          面向具体 agent 的封装（skill 等）
  tools/           生成器、lint
```

### 14.2 三层边界

| 层 | 内容 | 不放 |
|---|---|---|
| mcpp | §5 的契约 | 任何 IDE 概念 |
| mcpp-ide 后端 | 会话与工作区模型（打开的文件属于哪个 mcpp.toml）、监听（结果记录、manifest）、编排（同工程串行、取消、新取代旧、构建后刷新）、视图模型（工程树、缓存、库目录、依赖图投影与折叠）、策略（清理的危险等级与确认）、设置校验与迁移、自检报告、运行时文案 i18n、与 mcpp 的能力协商（`--protocol-version`） | 不解析 mcpp.toml、不做依赖解析，只消费 mcpp 契约（同 mcppls 只读 build-database 的约束） |
| 适配层 | 生成的静态声明；协议到原生 UI 的映射；承载 webui 与消息桥；后端进程管理；找到或安装 mcpp 与后端；工作区信任 | 任何业务判断 |

### 14.3 能力（两端同一张表）

| 能力 | VS Code | JetBrains（CLion） | 依赖的 mcpp 契约 |
|---|---|---|---|
| 运行、构建、测试 | Task（PTY，颜色正常） | "mcpp" 运行配置类型，PTY 控制台，`main()` 旁 ▶ | 事件流、结果记录 |
| 测试树、单测重跑 | Testing API | IntelliJ 测试运行器 | `test --message-format json`（已有） |
| 调试 | DebugConfigurationProvider（cppdbg / CodeLLDB / lldb-dap） | 内置 GDB/LLDB | 启动描述 |
| 诊断 | Diagnostics（不再用正则 problem matcher） | Problems + console filter | 事件流的 diagnostic 记录 |
| manifest 编辑 | 诊断来自 mcpp（删除 TS 的 TOML 校验层） | 同左，解析与高亮由平台 TOML 插件提供 | manifest 诊断带位置 |
| 库浏览 | webui | webui | `index list` / `search` 的 JSON |
| 打包 | webui 打包面板 | 同左 | `pack --list-formats`、`pack --message-format json` |
| 语言功能 | 依赖 mcppls 扩展 | 依赖 mcppls CLion 插件 | — |

### 14.4 分发

后端作为预编译包经 xlings 索引分发（O4），插件不分平台、只出一个包；版本：后端与适配层同仓同步发版，彼此握手校验。

## 15. 诊断分级

| 情形 | 等级 | 码 |
|---|---|---|
| 两个依赖对 `c++-abi` 的需求冲突 | 规划时拒绝（已有） | — |
| `requires mcpp:c++-abi` 不满足 | 规划时拒绝，附两条 `help` | 已有 requires 拒绝 |
| 孤岛声明存在但 I 条件 1–3 不满足 | 规划时拒绝 | 新：`cxx-runtime-island` |
| 孤岛目标文件跨运行时按类型捕获（条件 6） | 构建时错误（目标文件编译后才可检查） | 同 `cxx-runtime-collision` |
| 冲突集判据不满足（孤岛目标文件引用布局相关符号） | 构建时错误（目标文件在规划后才产生） | 新：`cxx-runtime-collision` |
| 链接后校验：映像以 GLIBCXX/CXXABI 导入 ABI 层冲突集 | 构建时错误 | 同上 |
| 未声明的外来运行时，I 条件满足 | `warning`（作者向），提示补声明 | — |
| 未声明的外来运行时，I 条件不满足 | 规划或构建时错误（同上两码） | — |
| `abi:` 旧写法 | `warning`（COMPAT 读法） | — |
| 根目录 `compile_commands.json` 不再更新 | `note`，每工程一次 | — |
| 等待构建锁 | 状态行与事件，不是诊断 | — |
| 副作用被拒绝 | 拒绝，列出副作用与原因；退出码 5 | 新：`effect-denied` |
| `requires` 的 `visibility = "private"` 用在 `c++-abi` 以外的层 | 规划时拒绝（一个映像只能有一个 C 运行时，openkal 边界一节） | 新：`requires-private-layer` |
| `mcpp.lock` 并发修改，重试后仍冲突 | 错误 | 新：`lockfile-concurrent` |
| provisioning 失败 | 错误，透传 xlings 诊断 | — |

## 16. 规范与文档修订

| 文档 | 修订 |
|---|---|
| docs/50 + zh | 事件流、`mcpp.result`、`mcpp.search`、`mcpp.index`、`mcpp.pack.formats`、启动描述、`--deny`、`MCPP_CLIENT` |
| SPEC-003 | 新增退出码 `5`（副作用被约束拒绝）与 `128 + 信号`（130、143）；诊断码 `effect-denied`、`lockfile-concurrent` |
| docs/04、docs/06、docs/22 | `requires` 的对象形式与 `visibility`；`[target.<sel>] requires` |
| SPEC-002 + docs/22 | 孤岛与宿主模式、compiler-runtime 规则修订、图提供的 libstdc++、冲突集 |
| 新 `docs/specs/concurrency.md` | 三段、锁粒度、持有者、等待、原子发布、乐观并发 |
| 新 `docs/specs/human-output.md` | §5.7 |
| SPEC-010 → 生态级 | `--home`；xlings 引用 |
| docs/42（异构构建） | sycl / hip 的运行时与孤岛 |
| docs/04（mcpp.toml） | `[build] compile_commands` |
| 新 docs/52 + zh | 从程序与 agent 驱动 mcpp |
| CHANGELOG | 全部 |

## 17. 兼容

| 项 | 读法 | 移除下限 |
|---|---|---|
| `abi:` 能力 | 映射到 `requires`，`warning` | 两个发布周期之后，且 index 中已无使用 |
| 未声明的外来运行时 | 按孤岛校验，可靠放行 + `warning` | 同上，且 mcpp-plugins 已发布声明版本 |
| 根目录 `compile_commands.json` | 停止更新 + `note` | 不移除（只是默认值变化） |
| `target/.build_cache` 旧格式 | 读取后迁移为每配置文件 | 一个发布周期 |
| `--offline` | 永久保留，等价 `--deny network` | — |
| sycl 在 llvm 下（含 mcpp-plugins 0.19.x 的旧规则） | 今天能构建、但运行时库抛出的 `sycl::exception` 接不住（E20）；新版拒绝并给出一行修复（加 libstdc++ 提供包，或 gcc） | 行为变化写入发布说明；mcpp-plugins 的 fixture 同批更新 |
| 缓存 | `c++-abi` 已是 ABI 维度，切换后重建 | — |
| 外来运行时按 `DT_NEEDED` 识别 | 比子串推断覆盖更多：链接了"内部用 libstdc++、对外只有 C 接口"的预编译库（例如系统 ICU）的 llvm 工程，单映像时得到 `warning`，含 C++ 共享库目标时被拒绝 | 合并前用 llvm 跑一遍 index 冒烟，统计新增的 warning 与拒绝，写入设计修订（§20.3） |
| `requires` 的对象形式、`[target.<sel>] requires` | 旧 mcpp 读到会报未知形式 | 使用它们的 index 描述文件声明 mcpp 版本下限 |

## 18. 行业对照

| 体系 | 做法 | 本设计 |
|---|---|---|
| Conan | `compiler.libcxx` 是全图设置，计入 package_id；纯 C 包去掉该设置；允许按包覆盖，但不校验可靠性 | 全图一个提供方 + 声明 + 链接后校验 |
| CPS（CMake 4.3 正式支持） | `platform.cpp_runtime_vendor`（gnu / llvm / microsoft），有类型的声明 | 对应 `requires mcpp:c++-abi` |
| DPC++ | 预编译 `libsycl` 依赖 libstdc++；libc++ 需自建运行时且为实验性 | U 默认用 libstdc++；I 仅在可验证时 |
| Cargo | `links` 在清单中静态声明"同一原生库全图只出现一次" | 影响解析的事实静态声明 |
| CMake / Meson / xmake | 编译数据库默认不写源码根目录 | D14 |

## 19. 不采纳的方案

| 方案 | 原因 |
|---|---|
| 在 `[toolchain]` 加 `stdlib` 键 | 合并 `compiler` 与 `c++-abi` 两层，违背层的划分条件（E14 证明可独立替换） |
| 解析时自动补 libstdc++ 提供包 | 多一个看不见的输入，可复现性变差；拒绝 + 一行修复已足够 |
| 只修链接顺序 | 只覆盖单映像；多映像与布局冲突仍不可靠 |
| 常驻服务放进 mcpp（`mcpp ide serve`） | 会话状态不属于构建契约；mcpp 保持一次性、无状态 |
| mcpp-ide 提供 MCP | 跨链观察已由结果记录解决，排队由 mcpp 的锁解决；两条 agent 路径会分歧 |
| 每个命令各自的快速路径 | 正确性靠清单，已多次漏项；查询无法受益 |
| 工程级的粗粒度锁 | 不同配置本来互不影响（E5）；锁住整个工程会让 IDE 与终端互相阻塞 |
| 新增"白名单"概念 | `--offline` 已是副作用约束，推广即可 |

## 20. 交付：每仓库一个 PR，commit 作检查点

### 20.1 顺序（发布依赖）

xlings → mcpp（提高内置 xlings 下限）→ mcpp-index（libstdc++ 提供包与 `requires` 新形式需要 mcpp 下限）→ mcpp-plugins。
mcpp-language-server 与 IDE 一线本批不提 PR，跟踪于 mcppls#71、mcpp-vscode#23。

**任务与依赖（rev 3）**

```
X1 xlings 等待 ─┐
X2 xlings 认领 ─┼─→ xlings 2026.10.26.1 发布 ─→ M15 内置 xlings 下限
X3 xlings home ─┘                                     │
M1 FileLock 可等待 ─→ M4 构建目录锁 ─→ M6 结果记录/启动描述 ─→ M13 编译数据库
M3 配置记录每配置一文件 ─┘   │                                   │
D22 链接产物原子发布 ────────┘（与 M4 同批：执行段不持锁的前提） │
M5 取消 ─────────────────────→ M6                                 │
M7 --deny（依赖 M6 的信封与 effects 报告）                        │
M8 查询 JSON、M9 输出与颜色（独立）                               │
M10 图提供的 libstdc++ ─→ M11 运行时模式 U/I/H ─→ M12 abi: 并入 requires
          │                    │
          └─→ index gcc.libstdcxx ─→ plugins sycl 规则 ─→ Tensorvia 冒烟
M14 性能（独立）；M16 文档与规范（最后）；M17 测量（合并前）
```

每个节点一个检查点 commit；每个检查点在本地构建并运行受影响的单元与 e2e。依赖边表示"后者的测试需要前者"：
锁（M4）与原子发布（D22）同批，因为执行段不持锁正是以原子发布为前提；运行时模式（M11）的 e2e 需要图提供的 libstdc++（M10）与 index 的提供包。
mcpp PR 合并前按惯例对 mcpp-plugins 跑 `ci.yml -f mcpp_source_ref=<branch>`，并用 Tensorvia 做外部冒烟（gcc、llvm + libstdc++、sycl）。

### 20.2 openxlings/xlings（一个 PR）

1. `interface`：心跳与 stdin 线程可中断等待；测试断言 `payload_status` 耗时。
2. `xvm/materialize`：可证明条目的认领规则；报错可操作。
3. `self init`：升级时认领迁移；e2e 用"删登记保留链接"的 home 复现 E13。
4. home：`self init --home`、定位器、全局 `--home`；引用生态 home 规范。
5. 文档与 CHANGELOG。

### 20.3 mcpp-community/mcpp（一个 release PR，`release/<version>`）

1. `platform`：`FileLock` 可等待获取（退避、可取消、持有者内容）；`MCPP_CLIENT`。
2. 流水线三段：规划段不写构建目录；修改段集中落盘；记录原子替换。
3. 持久计划：指纹输入表、有效性检查；`.build_cache` 改为每配置文件；删除 `try_fast_build` / `try_fast_run` 与拒绝清单。
4. 构建目录锁：只覆盖修改段；`run` exec 前释放；等待事件；`mcpp.lock` 乐观并发。修 #839（e2e：E4 复现在上个发布失败）。
5. 取消语义；SPEC-003。
6. 事件流（build、pack）、结果记录、`.last-result.json`、启动描述与 `run --print-launch`。
7. `--deny` / `MCPP_DENY_EFFECTS`；`--offline` 改为别名。
8. 查询补全：manifest 诊断位置、`search` / `index list` JSON、`pack --list-formats`。
9. 非 TTY 输出规范、`help:` 行、颜色约定。
10. 目标侧：图提供的 libstdc++ 在 clang 下的接线（头文件、std 模块、`-stdlib`）；compiler-runtime 规则修订；构建程序环境暴露 `c++-abi`。
11. 运行时模式 U / I / H：孤岛声明、I 条件、冲突集数据表、链接顺序、链接后校验；删除子串推断，未声明外来运行时的 COMPAT 校验。修 #838（e2e：E1 在上个发布失败）。
12. `abi:` 并入 `requires`（COMPAT 读法）。
13. 根目录 `compile_commands.json` 默认关闭、`[build] compile_commands`、config 默认值、迁移提示；编译数据库按计划派生。
14. 性能：`cache list` 条目大小、探测直接 exec 与缓存。
15. 全局 `--home`；provisioning 诊断透传；内置 xlings 下限。
16. 规范、双语文档、CHANGELOG、版本号；`check_*` 脚本全过。
17. 合并前测量：用 llvm 对 index 全量冒烟，统计 D13（`DT_NEEDED` 识别）新增的 warning 与拒绝；计划等价性测试的语料结果；写回本设计。

### 20.4 mcpplibs/mcpp-index（一个 PR）

1. libstdc++ 提供包（§12.1）。
2. `compat.sycl-runtime` 声明 `requires mcpp:c++-abi=libstdc++`；HIP 对应包按 O7 实测结果声明。
3. mp-units、cpptrace、glfw 的声明修正；全量清点记录。
4. 一个用 llvm + libstdc++ 的冒烟包测试。
5. 使用 `requires` 新形式或依赖 `gcc.libstdcxx` 的描述文件声明 mcpp 版本下限。

### 20.5 mcpp-community/mcpp-plugins（一个 PR）

1. sycl / hip 规则：删除 `:libstdc++.so.6`，`--gcc-install-dir` 跟随已解析的提供方。
2. fixture：U（gcc；llvm + 提供包）；llvm 无提供包时被拒绝且给出一行修复；`kind = "shared"` 在 U 下跨 `.so` 捕获成功。
3. 文档与版本。

### 20.6 Sunrisepeak/mcpp-language-server（本批不提 PR）

已提 issue [#71](https://github.com/Sunrisepeak/mcpp-language-server/issues/71)：回退路径读 `.last-result.json`；监听结果记录刷新；CLion e2e 断言 mcpp 工程不被 Compilation Database 接管。mcpp 发布后由 mcppls 自行排期。

### 20.7 mcpp-community/mcpp-ide（rev 3：本批不实施，见 mcpp-vscode#23）

1. 仓库骨架、`mcpp.toml`、CI（含两端适配层）；README 与 README.zh-CN 按 mcppls 的结构（§14）。
2. `protocol/`：schema 与 golden fixture（取自 §20.3 的 mcpp 分支）。
3. 后端：会话、监听、编排、视图模型、能力协商。
4. `spec/` 与生成器。
5. `webui/`：依赖图、库、缓存、设置、打包。
6. `vscode/`：由 mcpp-vscode 迁移，改为消费后端；Testing API、调试、诊断；仍以扩展 ID `mcpp-vscode` 发布。
7. `jetbrains/`：运行配置、测试树、调试、console filter、面板；以 `mcpp-clion` 发布。
8. `agents/`：面向具体 agent 的封装。
9. 分发与发布流程。

## 21. 验收

### 21.1 不变量

1. 同配置并发构建：双方都成功，第二个等待后不重复编译（E4 的复现 20 轮全过）。
2. 不同配置并发：互不等待。
3. `mcpp run` 的程序运行期间，同配置构建不被阻塞（锁已在 exec 前释放）。
4. 查询命令在构建进行中不阻塞。
5. 任何组合下进程内只有一套 C++ ABI 运行时被我们的代码使用；不可靠的组合在规划或构建时被拒绝，不存在"能运行但 catch 失败"的产物。
6. 跨 `.so` 的 `catch (const std::exception&)` 在 U 模式下成功（E15）。
7. 计划有效性判断只在一处；删除快速路径后，#544、#716、#755 与 riscv 那次的 e2e 全部通过。
8. 启动描述执行的环境与 `mcpp run` 一致（逐项比较）。
9. 结果记录在构建结束（成功、失败、取消）时总是存在且完整。
10. **计划等价**：对一组工程（mcpp 自身、examples/、mcpp-plugins 的 fixture、index 冒烟包），读取持久计划与重新规划得到的 `plan.json` 逐字节相同；只改动一个指纹输入时计划失效。
11. 链接产物在任何时刻要么是旧文件、要么是完整的新文件（并发构建期间反复 exec 测试二进制不出现 ENOEXEC）。

### 21.2 新 e2e（必须在上一个发布上失败）

并发冷构建；#838 最小复现；孤岛拒绝；llvm + libstdc++ 提供包的 `import std` 与跨 `.so` 捕获；exec 前释放锁；等待事件；
取消与结果记录；`--deny network` 在需要安装时拒绝；根目录 `compile_commands.json` 默认不写与迁移提示；`CLICOLOR_FORCE`；
`run --print-launch`；`index list --format json`；`--home`。

### 21.3 性能（本机同条件，hyperfine）

| 命令 | 今天 | 目标 |
|---|---|---|
| `xlings interface payload_status` | 517 ms | ≤ 30 ms |
| `emit build-database`（计划有效） | 604 ms | ≤ 80 ms |
| `toolchain list --format json` | 554 ms | ≤ 80 ms |
| `cache list --format json`（580 条目） | 376 ms | ≤ 40 ms |
| `build`（无改动） | 49.6 ms | ≤ 55 ms（不回退） |
| 完整构建中的编译数据库 | 119–126 ms | 计划不变时 0 |

## 22. 开放问题

| # | 问题 | 处理 |
|---|---|---|
| O1 | ~~`--deny` 的名字~~ **已定**：`--deny`；与 `MCPP_DENY_EFFECTS` 取并集，manifest 不能放宽 | — |
| O2 | ~~孤岛声明的位置~~ **已定**（D20）：`requires` 对象形式 + `visibility = "private"`，可写在 `[target.<sel>]` | — |
| O3 | ~~仓库~~ **已定**（D19）：新仓库 mcpp-ide，不改 mcpp-vscode 仓库名，插件名沿用 `mcpp-xxx` | — |
| O4 | ~~webui 与分发~~ **已定**：webui 只渲染；后端经 xlings 索引分发 | — |
| O5 | ~~Windows 与 macOS 的锁和运行中程序的替换~~ **已实测**（E23、E24）：PE 先改名挪开再原地链接；锁语义三平台一致 | mcpp#840 |
| O6 | ~~提供包名~~ **已定**（D21）：`gcc.libstdcxx`，按 `xim:gcc` 已有版本各一个 | — |
| O7 | ~~冲突集判据对 SYCL / HIP 是否成立~~ **已关闭**：SYCL 走 U（E19、E20）；HIP 没有第二套运行时（E21） | — |
| O8 | ~~llvm + libstdc++ 的其他组合~~ **已实测**（E26）：x86_64 全部正确；aarch64 无 llvm 载荷 | mcpp#840 |
| O9 | ~~mcpp-vscode 仓库的去向~~ **已定**：本批不动，记录于 mcpp-vscode#23 | — |
| O10 | ~~PE 下孤岛条件的形式~~ **已实测**（E25）：条件 1 放宽为"共用一个运行时映像"，条件 3、5 照常成立（§6.3） | mcpp#840 |

## 23. 多视角自审

每条：发现 → 处理。已并入正文的标注所在章节。

| 视角 | 发现 | 处理 |
|---|---|---|
| 架构 | 三段式与持久计划触及每个命令，是全批最大的风险点 | 在 mcpp PR 中拆成 commit 2、3 两个检查点；验收增加"计划等价"（§21.1 第 10 条）；把 #544、#716、#755、riscv 那次的 e2e 全部保留并在删除快速路径后通过 |
| 架构 | 规划段不持锁，两个进程可能交错写同一配置的 `plan.json` | 修改段拿锁后复核计划指纹，不一致则重规划一次（§3.2） |
| 架构 | 计划若包含模块图，源码改动（新增 import）会让计划过期 | 工程内模块边由 ninja dyndep 在构建时扫描，源码内容不是计划输入；源文件的增删经 glob 目录列表进入指纹（§3.2） |
| 正确性 | 执行段不持锁时，`mcpp test` 可能 exec 一个正被并发构建重写的测试二进制（GNU ld 先删后写） | 链接产物一律"临时名 + 原子改名"，由 ninja 链接边包装实现，与链接器无关（D22，§4.1）；验收第 11 条 |
| 正确性 | 构建被 SIGKILL 时没有结果记录，`.last-result.json` 指向更早的结果 | 结果记录带开始时间与调用方标签；读者把"比当前锁持有者更早"的记录视为陈旧；锁随进程消失，不会僵死 |
| 正确性 | 持锁方卡死时等待者无限等待 | 不加超时参数；等待状态显示持有者与已用时间，调用方可以取消（§4.2）。IDE 由 mcpp-ide 取消自己的旧构建 |
| 语义 | `visibility = "private"` 若用于 `c-abi`，等于在一个映像里放第二个 C 运行时，违背 openkal 边界一节"一个映像一个 C 运行时" | 只接受 `mcpp:c++-abi`；其他层规划时拒绝（`requires-private-layer`，§15） |
| 语义 | compiler-runtime 规则放宽后，`__udivti3` 的那条依据是否失效 | 依据的真实不变量是"一个程序只有一个 compiler-runtime"，由"每层一个提供方"保证；放宽的只是"必须同族"，提供包同时提供 `compiler-runtime=libgcc`，整个程序仍只有一个（§6.4） |
| 语义 | `--deny` 能否被工程放宽 | 不能：约束来自调用方（命令行与环境变量取并集），manifest 没有对应键（§5.5、O1） |
| 体验 | sycl 在 llvm 默认工具链下被拒绝，Tensorvia 这类工程第一次构建就报错 | 拒绝信息给出两条可直接粘贴的修复（加 `gcc.libstdcxx`，或 `--toolchain gcc@16.1.0`）；docs/42 写明原因（E20）；Tensorvia 自己的 manifest 注释已建议 gcc |
| 体验 | 根目录 `compile_commands.json` 默认关闭后，只用 clangd、不用 mcppls 的新用户没有编译数据库 | 接受这个代价（与 CMake、Meson 一致；CLion 接管问题随之消失）；docs/01 入门写明 `[build] compile_commands`；`mcpp new` 模板不默认开启 |
| 体验 | 只有 shell 的 agent 需要区分"被约束挡住"与"构建失败" | 新增退出码 5（D23） |
| 兼容 | `DT_NEEDED` 识别比子串推断覆盖更广，可能新拒绝一些今天能构建的 llvm 工程 | 合并前对 index 全量冒烟测量影响面并写回设计（§17、§20.3 第 17 条）；单映像时只是 `warning` |
| 兼容 | 旧 mcpp 读不懂 `requires` 对象形式与 `gcc.libstdcxx` 的接线 | index 描述文件声明 mcpp 版本下限（§20.4 第 5 条） |
| 兼容 | `target/.build_cache` 格式变化 | 读取旧格式后迁移，一个发布周期后移除读取（§17） |
| 跨平台 | 孤岛的 6 个条件是按 ELF 的全局符号作用域推导的 | Mach-O：Apple 平台只有 libc++，不存在孤岛；PE：按 DLL 导入表绑定，没有全局作用域，条件 1、3 可能可以放宽，条件 4、5、6 仍成立（链接期在同一映像内、按类型捕获跨运行时都与格式无关）。列为 O10，probe PR 实测后写入 SPEC-002 |
| 跨平台 | Windows 上正在运行的 exe 不能被覆盖 | 原子替换在 Windows 上先把旧文件改名挪开（§4.3）；O5 实测 |
| 性能 | 计划有效性检查本身的开销 | 预算：清单哈希 + glob 目录列表 + 载荷目录 `stat`，典型工程 ≤ 10 ms；验收时用 mcpp 仓库自身（大 glob）测一次 |
| 性能 | `DT_NEEDED` 识别要读每个链接输入的 ELF 头 | 结果按（路径、大小、mtime）缓存在计划里 |
| 安全 | 启动描述与结果记录可能把调用方环境（含机密）写进 `target/` | `env` 只含 mcpp 注入或改写的变量（§5.3）；`MCPP_CLIENT` 只做记录，不影响行为 |
| 最小核心 | 逐项核对新增内容是否属于"构建语义、对自身行为的契约、通用基础设施" | 全部满足：锁、取消、原子发布、记录是基础设施；事件流、启动描述、副作用约束、JSON 查询、人类输出规范是对自身行为的契约；运行时模式是构建语义。面向具体 agent 的封装、会话、监听、视图模型都不在 mcpp |
| 最小核心 | 是否新增了概念 | 没有新概念：副作用约束是 `--offline` 的推广；孤岛复用 `requires` 与 `visibility`；结果记录对称于 `resolution.json`；`[target.<sel>] requires` 对称于 `requires_abi` |
| 可测性 | 复现脚本里的实验要变成回归测试 | A（#839）、B（#838）、F（llvm + libstdc++）、G（退出码）、I（冲突集）转为 e2e，且必须在上一个发布上失败（§21.2）；D、E 的性能数转为 §21.3 的验收 |
| 生态 | mcpp-ide 依赖尚未发布的 mcpp 契约 | 发布顺序把 mcpp-ide 放在最后；`protocol/` 的 golden fixture 取自 mcpp 发布分支；后端用 `--protocol-version` 做能力协商，旧 mcpp 上关闭相应功能 |

## 24. 程序起点与镜像入口（mcpp#845）

### 24.1 问题

框架提供程序入口是常见需求：gtest 的 `gtest_main`、SDL3 的 main callbacks、Qt 在 Windows 上的 `qtmain`、
openmui 的播放器。`main` 只接受本包内的文件，框架只能让模板生成一个由应用维护的样板文件。裸机上同一问题
表现为另一种形式：启动代码与入口符号由板级包（BSP）或程序自身提供，链接器从链接脚本的 `ENTRY()` 或默认值
`_start` 得到入口，入口符号缺失时 GNU ld 与 lld 只给出警告并把入口地址设为 0，镜像能链接成功却从错误的地址开始执行。

### 24.2 模型：启动者与两层入口

每个镜像都有一个**启动者**，即把控制权交给镜像的一方：操作系统加载器与 C 运行时、okrun、Android 运行时、
固件或 ROM、引导协议、模拟器。入口由此分为两层：

| 层 | 含义 | 归属 |
|---|---|---|
| L0 镜像入口 | 启动者进入镜像的位置：一个符号（ELF `e_entry`），或由链接脚本放置的向量表 | 面向启动者的一层 |
| L1 程序起点 | 程序代码开始执行的翻译单元；PE 上另有入口函数的选择（`main`、`wmain`、`WinMain`、`wWinMain`） | 程序 |

| 目标 | 启动者 | L0 | L0 的归属 | L1 |
|---|---|---|---|---|
| hosted（Linux、macOS、Windows） | 加载器与 CRT | `_start`、`mainCRTStartup` | 工具链与 C 库 | `main`，PE 上由 `windows_entry` 选择 |
| Android `app` | ART/JNI | 平台约定的导出 | 程序的入口翻译单元 | 无 `main` |
| ok 镜像 | okrun | `ok_main`（格式固定） | 镜像内的 C 库；无 C 库时为程序自身 | `main` |
| 裸机，有 crt0 | ROM、硬件或模拟器 | `_start`、`Reset_Handler` 与向量表 | 板级包（crt0 选择、启动代码、链接脚本） | `main` |
| 裸机，无 C 库（内核、引导程序） | 硬件或引导协议 | `_start` 等 | 程序自身，或板级包 | 可以没有 |

L1 的可选取值只在 PE 上存在，其余各行固定为 `main` 或由平台约定，因此 `windows_entry` 保持现名与现有范围，
不推广为跨平台字段。L0 只在镜像没有 C 运行时的行上由构建陈述：hosted 行上改写它会绕过 CRT 初始化，ok 行上它由
格式固定，Android 行上由平台约定。

### 24.3 清单

所有入口相关的键都按目标寻址（`[targets.<n>]` 及行表 `[target.<sel>.targets.<n>]`），不进入 `[build]`：
`[build]` 是对每个目标生效、按规则合并的包级配置，入口是单个程序的身份，库没有入口。

| 键 | 位置 | 含义 |
|---|---|---|
| `main = "<文件>"` | 程序目标、`kind = "entry"` 目标、行表 | 程序代码开始的翻译单元。有 C 运行时的镜像中它定义 `main`；没有 C 运行时的镜像中它定义 `start` 所指的符号 |
| `main = { from = "<依赖>/<入口>" }` | 程序目标、行表、`[test]` | 程序起点由所列依赖的一个 `kind = "entry"` 目标提供。`<依赖>` 是本清单中依赖的键（`openmui`、`compat.gtest`），或本包自身的名字 |
| `kind = "entry"` | 提供方 | 只作为其他程序起点的翻译单元。可陈述 `main`（路径）、`start`、`windows_entry`、`cflags`、`cxxflags`、`defines`、`required_features` |
| `start = "<符号>"` | 程序目标、入口目标、行表、`[test]` | L0 符号 |
| `mcpp:link-start=<符号>` | 构建程序指令（协议 16），`LinkGlobal` | 板级包陈述的 L0 符号 |

```toml
# 提供方
[targets.player]
kind = "entry"
main = "src/entry/player.cpp"

[target.'cfg(os = "android")'.targets.player]
main = "src/entry/player_android.cpp"

# 使用方
[dependencies]
openmui = "0.1"

[targets.notes]
kind = "app"
main = { from = "openmui/player" }

# 测试
[test]
main = { from = "compat.gtest/main" }

# 无 C 库的内核
[targets.kernel]
kind  = "bin"
main  = "src/start.S"
start = "_start"
```

### 24.4 语义

1. **入口翻译单元在提供方的上下文中编译，链入使用方的程序。** 编译使用提供方的头文件目录、模块接口、标志与
   入口目标自身的 `cflags`、`cxxflags`、`defines`；每个配置编译一次。它不进入提供方的任何产物：入口文件与其他目标的
   `main` 一样从每个映像的对象集合中排除，只链入选择它的程序。
2. **契约只经符号与运行时数据。** 入口翻译单元不导入使用方的模块。供使用方实现的回调须在提供方接口中以
   `extern "C++"` 或 `extern "C"` 声明：附着于命名模块的声明不能在模块之外定义。
3. **按目标选择，不参与特性合一。** 特性在依赖图上取并集，入口在每个目标上互斥；以特性承载入口时，两个使用方
   开启不同的入口特性会使"恰好一个"的规则对其中一方失败。入口目标的 `required_features` 是选择它的前提：未开启时
   拒绝，并给出开启所需特性的依赖写法。
4. **使用方的入口标志不再适用。** `main = { from }` 与目标的 `cflags`、`cxxflags`、`defines` 同时出现时拒绝：
   这些标志的作用域是目标自身的入口翻译单元，而该单元已属于提供方。
5. **`windows_entry` 随入口继承。** 使用方未陈述时取入口目标的值；两者不同时拒绝并指出两处陈述。
   `windows_subsystem` 由使用方陈述。
6. **测试。** `[test] main = { from }` 为每个发现的测试程序提供起点，测试文件作为该程序的普通翻译单元链入；
   `[test] start` 为每个测试镜像陈述 L0。开发依赖中定义 `main` 的对象在测试自身定义 `main` 时不链入的既有规则保留。
7. **`start` 的合法范围。** 只在独立（freestanding）目标上可写；其他行上拒绝，并说明该行 L0 的归属
   （C 运行时、okrun 的 `ok_main`、Android 运行时）。
8. **L0 的唯一性。** 目标（或 `[test]`）、所选入口目标与依赖闭包中的 `mcpp:link-start` 可能各自陈述 `start`；
   陈述一致时取该值，不一致时拒绝并列出各陈述的来源。没有任何陈述时，链接脚本的 `ENTRY()` 或链接器默认值生效。
9. **渲染与校验。** ELF 链接渲染为 `-e <符号>` 与 `--require-defined=<符号>`（经驱动时加 `-Wl,`）：后者使未定义的
   入口符号成为链接错误，而非警告。`start` 优先于链接脚本中的 `ENTRY()`，与命令行优先于脚本的链接器规则一致。
10. **诊断。** 解析错误指出依赖键或入口名并列出候选；链接因入口符号未定义失败时，附注指出该符号的陈述来源。
    独立目标构建结束时在 `Size` 行之后给出 `Entry` 行（目标、符号、来源）。结果记录（docs/50）的产物条目增加
    `entry`：`from`（入口的来源）、`start` 与 `start_from`。

### 24.5 兼容

不使用新键的工程行为不变；`main = "src/start.S"` 的含义不变。`kind = "entry"`、`main` 的表形式、`start` 与
`[test] main` 为新语法，旧引擎按未知值或未知键拒绝；使用它们的包声明引擎下限 2026.10.26.2。`mcpp:link-start`
属于构建程序协议 16，旧引擎按未知指令处理。

### 24.6 不采纳的方案

| 方案 | 不采纳的理由 |
|---|---|
| `entry = "<依赖>"` 加 `[features.<f>] entry = "<文件>"`（#845 原议） | 特性在全图合一，入口按目标互斥（§24.4 第 3 条）；一件事分写两处并依赖"恰好一个已开启特性"的间接规则；同一包的多个目标无法各选入口 |
| 库导出 `main`（`gtest_main` 的做法） | Android 与 PE 的入口形式不同；依赖链接顺序与归档成员抽取；无法给出来源诊断 |
| 由规则包生成入口源文件 | 入口是普通翻译单元，不应依赖构建插件；需要使用方编译期信息的是代码生成问题 |
| 入口写为函数名，由 mcpp 生成 `main` 调用它 | 需要定义入口 ABI（签名、导入、`wmain`/`WinMain` 到 UTF-8 参数的转换、Android 无参数），mcpp 成为代码生成者并承担各平台入口适配 |
| 入口在使用方上下文中编译 | 无法缓存；提供方代码受使用方标志影响，契约不透明 |
| 跨平台的 `entry_function` 字段 | L1 的取值只在 PE 上存在，其余行上只能是 `main` 或被忽略 |

### 24.7 验收

e2e：依赖提供的程序起点（含回调由使用方实现、`windows_entry` 继承与冲突、`required_features` 未开启的拒绝、
未知依赖与非入口目标的拒绝、使用方入口标志的拒绝）；`[test] main = { from }`；本包自身作为提供方；行表替换入口文件；
独立目标上 `start` 的渲染、`--require-defined` 使缺失符号成为错误、三方陈述冲突的拒绝、hosted 行上的拒绝；
`mcpp:link-start` 指令。单元测试：清单解析的接受与拒绝、ELF 渲染。

## 25. 拆除 `modules/platform`

### 25.1 问题

mcpp 2026.10.26.2 把操作系统层换成 mcpplibs.os 时保留了 `modules/platform` 作为外观层：接口不变，内部部分改调
mcpplibs.os。结果是两个操作系统层并存：`modules/platform` 约 6200 行，142 个文件导入它，其中进程启动与有界进程
（约 3000 行）、终端检测、文件系统原语、环境变量读取与 mcpplibs.os 重复；其余部分并非操作系统抽象，而是 mcpp 自身的
语义被放在了 `platform` 名下。

### 25.2 原则

1. 操作系统能力只有一份，在 mcpplibs.os。mcpp 缺少的能力加入 mcpplibs.os，而不是留在 mcpp。
2. 属于 mcpp 语义的代码归入拥有该语义的模块，模块名与命名空间一致。
3. 平台差异以 `if constexpr` 与按系统选择的实现单元表达；原生头文件只出现在必须的最小范围内。

### 25.3 去向

| 现内容 | 去向 |
|---|---|
| 平台常量（`is_windows`、`host_arch`、`exe_suffix`、`name`） | `mcpplibs::os`（`PLATFORM`、`ARCH`、`is_windows`、`exe_suffix`、`name`） |
| 平台键与轴（`PlatformKey`、`HostPlatform`、`TargetPlatform`、`xpkg_platform*`）、宿主的产物命名与能力（`static_lib_ext`、`shared_lib_ext`、`lib_prefix`、`supports_rpath`、`supports_full_static`、`needs_explicit_libcxx`） | `modules/toolchain-model`（`mcpp.toolchain.platform`）：它们是工具链模型的词汇 |
| 环境变量读取 | `mcpplibs::os::env` |
| 环境变量写入（`set`、`unset`、`ScopedEnv`、`with_prefix`、Windows `prepend_path`） | `mcpp.invocation`（独立包 `modules/invocation`，只依赖 mcpplibs.os）：本次调用的环境陈述。mcpp 各包（含 `log`、`manifest`、`toolchain-model`、`buildmcpp`）的读取先查陈述再查 `os::env`，每个子进程的环境由陈述得出；mcpp 不再改写自身的进程环境。xlings 的调用环境同样按次给出，`ScopedInvocationEnv` 删除 |
| 副作用约束（`offline_mode`、`effect_denied`、`note_*`、`no_auto_install`、`writes_global_cache`） | `mcpp.effects`（D8） |
| 加载器环境（`runtime_library_path_key`、`host_tool_runtime_library_path_key`、`strip_private_glibc`、`prepend_path_list`、`build_env_prefix`、`inherited_path`、Linux 的 `build_*ld_library_path_prefix`）、`runtime_search`、`runtime_env_contract` | `src/runtime`（`mcpp.runtime.launch_env`、`mcpp.runtime.search`）；`src/runtime` 中命名空间 `mcpp::platform::{runtime,elf,search}` 改为 `mcpp::runtime::{binding,elf,search}` |
| 进程（`capture`、`capture_exec*`、`run_*`、`stream_exec`）、有界进程、`unixproc`、`winproc` | `mcpplibs::os::process`。mcpp 的组合（调用环境、宿主工具的加载器路径、取消与中断文件、终端模式恢复）在 `mcpp.run`。以 shell 字符串执行的调用改为参数向量；语义上需要 shell 的（`[hooks]` 命令）显式以 `sh -c` 或 `cmd /d /s /c` 启动 |
| 取消（信号处理器中结束进程组并写中断文件） | `mcpplibs::os::interrupt`（请求在等待中被读取）加 `os::process` 的单元：等待以有界的 `wait_for` 轮询中断请求，收到后 `end_unit`、写入中断文件、恢复终端，并以请求结束本程序（`end_as_requested`）。§5 的退出码不变 |
| shell 引号（`quote*`、`paste_arg`、`silent_redirect`、`null_redirect`） | `mcpp.build.command_line`：ninja 规则、钩子命令与供人复制的命令的拼写（`display(argv)`），以及 Windows 命令解释器的参数（`windows_shell_arguments`）。以 shell 字符串启动程序所需的 `windows_command_from_argv`、`windows_wrap_for_cmd_c` 删除 |
| 终端检测（`is_terminal`、尺寸、颜色） | `mcpplibs::os::terminal`；画面输出、按键解码、`StdoutToStderr`、协议输出、Unicode 判断属于界面，归入 `src/ui`（`mcpp.ui.terminal`） |
| 文件系统（`self_exe_path`、`which`、`replace_file`、`append_atomically`、`extended_length`、`fs_caps`、无替换改名、交换、目录链接、`sync_*`） | `mcpplibs::os::fs` 与 `os::env`（`image`、`find_executable`） |
| `FileLock` | `mcpp.home.files`：与早先发布的 mcpp 共用同一把锁（POSIX 上为 `flock`，与 openkal 的记录锁互不排斥），因此是 mcpp 对自身旧版本的兼容契约。按系统选择的实现单元位于 `src/native/posix`、`src/native/windows`（基础 `sources` 排除，由 `[target.<os>.build] sources` 按系统选入）；Windows 上与 `os::fs::lock` 同为 `LockFileEx` |
| 信任检查（`untrusted_because`，目录属主） | `mcpp.home`，基于 `os::fs` 的属主查询 |
| `capacity` | `mcpplibs::os::machine`（处理器、内存）；并行度策略 `recommended_jobs` 归入 `mcpp.build` |
| Windows 事实（`active_code_page`、`long_paths_enabled`、注册表路径、`env_directory`、`writable_fixed_drive`） | `mcpplibs::os::windows`（代码页、长路径）；home 的默认位置选择归入 `mcpp.home` |
| macOS SDK、部署目标 | `src/toolchain`（`mcpp.toolchain.apple_sdk`） |
| `project_name`、`scaffold_fs` | `src/scaffold` |

### 25.4 mcpplibs.os 0.2.0

| 增加 | 原因 |
|---|---|
| `process::exec(options)`：成为该程序；POSIX 上替换映像，其他系统上启动、等待并以其状态结束 | `mcpp run` 的退出码是程序自身的（E16） |
| `process::stdio::output`：标准错误与标准输出共用一个管道；`spawn_options::one_string` | 诊断按出现顺序收集；命令解释器按自身语法读取命令 |
| `fs::sync`、`fs::append`、`fs::extended`、`fs::windows_extended` | 原子发布的持久化；多进程追加；越过 `MAX_PATH` 的写法（规则本身在每个主机上可测） |
| `fs::behaviour`、`fs::measure`、`fs::place`、`fs::exchange` | 文件系统能力的实测与据此选择的无替换发布、交换（原 `fs_caps`） |
| `fs::owner_of`、`fs::writable` | home 的信任检查 |
| `machine::processors()`、`machine::memory_available()` | 并行度（原 `capacity`） |
| `stdio::read_input_for`、`stdio::output_to_error`；`terminal::raw_options`、`terminal::foreground` | 按键输入与协议输出（原终端交互） |
| `interrupt::end_as_output_closed()` | 读端关闭时按系统的方式结束（§25.5 第 4 条） |
| `posix::set_creation_mask` | 系统 home 的文件权限（umask 022） |
| `windows::code_page`、`console_output_code_page`、`long_paths_enabled`、`machine_registry_path`、`writable_fixed_drive` | 控制台输出、长路径声明、Windows SDK 与 home 的默认位置 |

### 25.5 进程、取消与关闭的读端

mcpplibs.os 与 openkal 没有异步信号：结束请求（SIGINT、SIGTERM、SIGHUP，Windows 的控制台事件）被接管后置一个字，
由程序读取（`os::interrupt`）；向读端已关闭的流写入返回 `errc::closed`，不结束程序。mcpp 由此采用以下模型，
取代信号处理器：

1. **一个守望线程。** `main` 开始时由 `mcpp.run` 接管结束请求并启动守望线程，线程阻塞于 `os::interrupt::wait`。
   请求到达时它结束已登记的子进程单元（`end_unit`），以原子替换写入中断文件，执行已登记的清理（恢复终端），
   然后以请求结束本程序（`end_as_requested`）。主线程无需轮询，等待中的子进程由其单元结束，§5.4 的取消记录与退出码
   不变。
2. **子进程。** 由 `mcpp.run` 统一启动：参数向量，`own_unit` 与 `bound_lifetime`，环境由调用覆盖（`mcpp.invocation`）
   得出；捕获、截止时间、空闲时限、逐行回调、停止条件按调用方给出，以 `os::process` 的有界读与有界等待实现。
   命令解释器只用于本身就是命令文本的命令：用户书写的命令（`[hooks]`）、MSVC 的环境脚本，以及 mcpp 以文本组成并
   如此记录的命令行——std 模块的构建命令（记录在其元数据中）、链接标志的 `-###` 检查、P1689 调试扫描、分离代码生成
   所执行的 ninja 规则文本，它们与 ninja 规则同为一行文本。其余调用（编译器探测、git、xlings、tar、strip、patchelf、
   readelf、vswhere、`cl` 横幅）均为参数向量加环境追加。POSIX 解释器为 `sh -c`，Windows 为
   `cmd.exe /d /s /c "<命令>"`，以 `one_string` 原样传递（openkal 0.17.1 `KAL_SPAWN_ONE_STRING`）。
   通用入口为 `run::execute(argv, Options)`：输入（空或继承）、两个输出流的去向（继承、合并读取、只读标准输出、
   标准错误分开保留等）、总时限与空闲时限、逐行回调、停止条件、单元、宿主工具的加载器路径；`run_exec`、
   `capture_exec`、`capture_stdout`、`stream_exec` 等为其常用形式。
3. **`mcpp run`。** `os::process::exec`：POSIX 上替换映像，退出码与信号即程序自身的。
4. **关闭的读端。** mcpp 的输出写入失败于 `errc::closed`（或 `EPIPE`）时，按第 1 条的顺序结束子进程、写中断文件、
   执行清理，然后 `os::interrupt::end_as_output_closed()`：POSIX 上以 SIGPIPE 结束（shell 看到 141），与此前相同；
   Windows 上写入被拒绝而程序继续，与此前相同。
5. **openkal 的配套发布。** openkal 0.17.1（`KAL_SPAWN_ONE_STRING`）、openkal-windows 0.13.1（实现它）、
   openkal-linux 0.18.1 与 openkal-macos 0.15.1（SIGHUP 属于结束请求，以及 §25.8 的两处）；mcpplibs.os 0.2.0
   以它们为下限。四个仓库的补丁在同名分支 `openkal-0.17.1` 上准备，openkal 的一致性检查按分支名取实现。

### 25.6 验收

`modules/platform` 与 `mcpp.platform*` 模块不存在；`mcpp::platform` 命名空间不存在；`src/` 与 `modules/` 中不出现操作系统原生头文件
（只在 `src/native/<system>` 的锁实现单元中），平台差异不以预处理条件表达（`if constexpr` 与按系统选入的实现单元）。全部单元测试与 e2e 通过；取消、退出码、SIGPIPE（941）、并发构建、
`mcpp run` 退出码、Windows 长路径、宿主工具加载器路径的 e2e 不变地通过。

### 25.7 GCC 16 的模块接口约束（实测）

拆除过程中，GCC 16.1 写出的若干编译后接口在下一个导入者处不可读（`failed to read compiled module cluster N: Bad file
data`、`failed to load pendings for 'std::vector'`）。以最小化的导入集合逐一定位，触发条件均为两个接口在同一导入者中
合并时，其中一个接口单元自身携带了 `std` 模板的实例化：

| 定位到的组合 | 处理 |
|---|---|
| `mcpp.build.command_line`（实现写在接口单元内）与 `mcpp.provider.xlings.subos_info` | 实现移入 `command_line.cpp` |
| `mcpp.toolchain.apple_sdk`（接口中的 `deployment_target` 函数体、`std::optional<bool>` 返回值）与 `mcpp.modgraph.graph` | 函数体移入实现单元；`sdk_has_target` 改为三值枚举 `Slice` |
| `mcpp.toolchain.clang`（实现写在接口单元内）与 `mcpp.modgraph.p1689` | 实现移入 `clang.cpp` |
| `mcpp.provider.xlings`（2000 行实现写在接口单元内） | 实现移入 `xlings.cpp` |
| `mcpp.toolchain.platform::is_android_host`（`inline` 且含静态局部 lambda） | 移入实现单元 |

规则：新增或移动的模块，接口单元只含声明与常量，实现放在实现单元；接口不导入只有实现需要的模块（如
`mcpplibs.os.process`）。

### 25.8 openkal 补丁发布中实测发现的行为

1. **解释器程序的名字（openkal-linux 0.18.1）。** openkal-linux 以打开的描述符启动程序（`execveat(fd, "",
   AT_EMPTY_PATH)`），需要解释器的程序（`#!` 脚本）因此得到 `$0 = /dev/fd/<n>`，以 `dirname "$0"` 定位同目录文件的
   脚本找不到它们（mcpp 的单元测试 `NetworkProcess.MetadataAndResourceDeadlinesStopDescendants` 的桩脚本写入
   `/dev/fd/ticks`）。0.18.1 在内核答 ENOENT（需要解释器）时，若绝对名可知且与描述符是同一文件（设备号与 inode 相同），
   以绝对名 `execve`；否则保留描述符方式。测试 `script_sees_its_name`。
2. **其他处理器的映像（openkal-macos 0.15.1）。** EBADARCH 与 EBADMACHO 此前译为输入输出错误，现与 ENOEXEC 同译为
   `kal_err_not_program`；mcpp 的 `runner_lookup` 据此只需识别 ENOEXEC 一种"本机不能装载"。测试
   `foreign_image_not_program`。
## 附录 A：讨论收敛记录（摘要）

| 问题 | 结论 |
|---|---|
| `mcpp run` 的返回值 | 程序自身的退出码（POSIX `execve`），E16 |
| IDE 标红只在 C++23 | `import std` 在 gcc lane 的 BMI 不可被 clangd 读，与退出码无关，E17；CLion 控制台的红色是 stderr 无颜色码，E18 |
| 手动 External Tool 还是插件 | 做 mcpp-ide；语言功能由 mcppls 的插件提供 |
| 合仓与复用 | 共享 spec、webui、协议 fixture；通用逻辑在 C++ 后端 |
| 常驻服务放哪 | 独立工具 mcpp-ide，不进 mcpp |
| agent 模式 | 不单设；统一控制契约；mcpp-ide 不提供 MCP |
| 锁 | 粒度最小；三段式；持久计划 |
| C++ 运行时 | 三种模式；U 默认；按目标侧模型由依赖选择；不新增工程配置项；拒绝 + 一行修复；`abi:` 并入 `requires` |
| 本机无法构建 Tensorvia | xlings 升级缺陷 X2（E12、E13），非 mcpp 缺陷 |
| xlings 的 home | 补齐与 mcpp 一致的机制，规范升为生态级 |
| 根目录编译数据库 | 默认不写，可配置，已有副本停止更新并提示 |
