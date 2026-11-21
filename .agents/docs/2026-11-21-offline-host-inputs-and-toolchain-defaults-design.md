---
subject: ecosystem
status: active
---

# 离线、宿主输入与工具链的缺省：按内容取归档的来源、项目的取齐、宿主输入的声明、工具链的解析与锁、缺省版本的规则（mcpp#988、#989、#990）

**日期：** 2026-11-21 · **版本：** v1（待负责人确认第 9 节） · **基线：** mcpp 2026.11.20.1（`722ec1b6`）与 #984 的分支，
xlings 2026.11.17.2，xim-pkgindex `c0023dd` · **规范：** SPEC-004 §2（四层取值）、SPEC-006（工具链管理）、SPEC-002（目标侧五层）、
SPEC-013（平台与层）、home 的初始化（`docs/specs/home-initialization.md`） · **所属：**
[生态的测试与 CI 方案](2026-11-20-test-architecture-upgrade-design.md) 第 15 节；与
[构建程序的输入由观测给出](2026-11-21-build-program-observed-inputs-design.md) 共用其 3.6 的提示机制。

## 1. 范围与原则

本方案处理一台机器得到构建所需之物的方式：联网时从哪里取，离线时从哪里取，哪些可以取自宿主，取什么版本。四条原则：

1. **每个工具只做它自身视角下通用的机制。** xlings 是包与载荷的管理者：取归档、装载荷、管理索引；mcpp 是项目的构建者：
   从清单与锁得出一个项目需要什么。离线、宿主输入与版本的规则落在拥有该事实的一方，另一方只组合它。
2. **默认密闭。** 工具链、基础层与引导工具取自索引中的包；宿主只在数据声明或用户显式开启时使用（AGENTS.md §2）。
3. **每个缺省是数据，且有显式的覆盖**，取值按四层：命令行与环境、工程（`mcpp.toml`）、机器（`config.toml`）、引擎（SPEC-004 §2）。
4. **做得更少从不沉默，正常的路径保持安静。** 提示经由 `mcpp.diag` 的汇点，按子方案一 3.6 的 `always` 与 `once` 两种策略。

## 2. 现状

| 方面 | 现状 | 问题 |
|---|---|---|
| 离线 | 只有 `--offline`、`MCPP_OFFLINE` 与旧的 `MCPP_NO_AUTO_INSTALL`；`[index] auto_refresh = false` 只停止隐式刷新，下载照常 | 没有工程与机器两层（违反原则 3）；没有「联网时取齐、离线时构建」的操作，只能联网完整构建一次 |
| 没有网络的机器 | xlings 的下载器在目标文件已存在且 sha256 相符时不下载；索引可以是本地目录（mcpp 的 `path = "…"`、xlings 的 `file://`）；home 可以整体复制或作为 upstream | 可行的做法都要求用户了解内部布局：归档须放在 xlings 的下载目录并按 URL 推出的文件名命名（GitHub 的标签归档同名为 `v<版本>.tar.gz`）；复制 home 只在相同的绝对路径下可用 |
| 引导工具 | `check_base_init` 无条件要求 home store 中固定版本的 patchelf 与 ninja | 没有覆盖（违反原则 3）；以宿主的工具链与 `glibc@system` 构建时并不需要 patchelf，仍被拒绝 |
| 配套载荷 | 编译器旁的 binutils 与 linux-headers 由 `find_sibling_binary` 与 `find_package_in_xpkgs` 取目录遍历遇到的**第一个**版本 | 选择取决于文件系统的顺序，而非工具链载荷声明的依赖。本机同时有 binutils 2.42 与 2.42.1，构建使用 2.42，gcc 的 recipe 声明的是 2.42.1，而 2.42 没有能看到 subos 库的 `ld` 包装 |
| 配套载荷的版本 | 由 gcc 的 recipe 的依赖固定，mcpp 中没有覆盖 | 违反原则 3 |
| 部分版本 | `gcc@16`：`toolchain install` 取索引最新，`toolchain default` 取已安装的最新，构建取已安装的版本 | 一个请求三个回答；docs/20 陈述的「取最新发布」只对第一处成立 |
| 编译器的版本 | gcc 的版本取横幅中最后一段数字 | Fedora 的 `g++ (GCC) 16.2.1 20261029 (Red Hat 16.2.1-3)` 被读作 `3`（#989） |
| 三元组 | vendor 段只认 `unknown`、`pc`、`w64`、`none` | `x86_64-redhat-linux`、`x86_64-suse-linux` 无法解析，原生 sysroot 的设置不生效，且没有提示 |
| 缺省版本 | 首次运行的缺省是 `triple.cppm` 中的常量 | 不是数据（违反原则 3）；多项落后于上游的正式发布（3.7 的表） |
| 包的版本号 | binutils 的 `2.42.1` 与 2.42 字节相同，是为重跑安装钩子而新增的键 | 上游没有发布 2.42.1，而上游确有 `.1` 修订（2.45.1、2.46.1）；`revision` 机制已经存在 |

## 3. 设计

### 3.1 xlings：按内容取归档的来源

**机制。** 一个归档的身份是索引给出的 sha256。xlings 持有一个有序的**来源**列表；每次需要一个归档时，依次询问各来源，
第一个给出 sha256 相符之文件的来源胜出，文件经校验后进入安装。来源有两类：

- **目录。** 一个本地目录，其中的文件以任意名字、任意层级存放；xlings 对目录建立 sha256 的索引（按路径、大小与修改时间备忘，
  只在三者改变时重新计算）。
- **网络。** 现有的镜像（`GLOBAL`、`CN` 与自适应的选择），位于全部目录之后。离线时这一类被跳过。

现有的「下载目录中已有且 sha256 相符即不下载」成为列表中第一个目录来源（xlings 自己的下载缓存），行为不变。

**配置。** `.xlings.json` 的 `archives`：目录的列表。`xlings config --add-archives <dir>` 与 `--rm-archives <dir>` 修改它。

**导出。** `xlings download <包@版本>… --to <dir> [--with-index]`：把每个包在本宿主上所需的归档（含依赖的闭包），
以及 `--with-index` 时所用索引的快照，写入目录，并写出清单 `bundle.toml`（格式版本、每个归档的包、版本、sha256 与目标、
每个索引的名字与修订）。目录本身即是一个可用的来源。

**导入。** `xlings install --from <dir> <包>…`：以目录为本次的第一个来源，以其中的索引快照为本次的索引。

**离线时缺少归档。** 一次列出本次需要而所有来源都没有的归档（包、版本、期望的 sha256），而不是在第一个处停止。

### 3.2 mcpp：项目的取齐

**`mcpp fetch`** 是构建中「取得所需之物」这一步的命令形式。它与构建共用解析与目标的闭包：依赖（按锁）、工具链与其配套载荷、
目标的基础层、引导工具、构建程序的依赖。构建在缺少时调用同一个函数。

```bash
mcpp fetch [--target <t>]…                 # 联网：把所需之物装入本机的 home
mcpp fetch --to <dir> [--target <t>]…      # 写出一个自描述的目录（bundle）
mcpp fetch --from <dir>                    # 从目录装入本机的 home；之后目录可以移走
```

- **`--to`** 调用 xlings 的 `download --to --with-index` 写出载荷与索引快照（含 mcpp 的各索引），另以 `git bundle` 写出每个 git 依赖在
  锁中的提交，并把这些记入同一份 `bundle.toml`。
- **`--from`** 以目录的索引快照解析，调用 `xlings install --from` 安装，并把 git bundle 导入 `~/.mcpp/git`。
- **稳定性的承诺。** 一个版本的 `--to` 写出的目录，同一版本与更新的版本的 `--from` 都接受；目录内部的布局不是接口。

**离线的键。** `[build] offline = true`，在 `mcpp.toml` 与 `config.toml` 中同名同义（与 `memory_threshold` 相同的形式），
四层取值：`--offline` 与 `MCPP_OFFLINE` 为覆盖层。离线时网络来源被跳过，任何需要网络的动作被拒绝并说明原因。

**长期的本地来源。** `config.toml` 的 `[xlings] archives = ["<dir>"]`（与 `[xlings] upstream` 并列），环境变量 `MCPP_ARCHIVES`；
mcpp 把它传给 xlings。它是机器的事实：写在 `mcpp.toml` 中时可以使用，并按 3.3 给出一次可移植性的提示。

**与 upstream 的关系。** `[xlings] upstream` 是已安装载荷的来源（按链接发布，不复制）；`archives` 与 bundle 是归档的来源（校验后安装）。
两者并列，各自不变。

### 3.3 宿主输入：声明、身份与提示

**宿主输入的类别。** 每一类都是显式的开启，开启的方式在四层中：

| 类别 | 开启 |
|---|---|
| 宿主的工具链 | `[toolchain] default = { path = … }`、`msvc@system`（行的数据） |
| 宿主的基础层 | `sysroot = "glibc@system"` |
| 宿主的库 | `[build] allow_host_libs = true`、`MCPP_ALLOW_HOST_LIBS` |
| 依赖的工具 | `[tools.overrides]`、`MCPP_TOOL_<PKG>_<TOOL>` |
| **引导工具** | 新增 `[build] allow_host_tools = ["ninja", "patchelf"]`，四层取值；被开启的工具在 `PATH` 中查找 |

**按需检查。** `check_base_init` 改为在用到时检查：patchelf 只在安装需要重定位的载荷与打包时需要，ninja 只在执行构建图时需要。
一个以宿主的工具链与 `glibc@system` 构建的工程不再因缺少 patchelf 被拒绝。

**身份。** 宿主输入的身份（工具链的路径、版本、三元组与驱动的指纹，基础层的 C 库版本，引导工具的路径与版本）进入缓存键，
并写入 `mcpp.lock`（3.4）。宿主升级后身份改变：缓存不混用，下一次构建重新构建并提示一次。

**提示。** 每一类开启在一个工程中提示一次（`once`，键为类别与对象），两行，陈述来源、风险与回到密闭的写法，例如：

```
note: ninja is this machine's (/usr/bin/ninja 1.12.1), not the store's; another machine may build
      differently, and it lacks the correction mcpp pins (ninja#2662). help: docs/25, "Host inputs"
```

- 风险取自数据：引导工具与工具链的行陈述已知的修正与限制，提示读出它们，代码中不写具体的版本。
- 开启写在 `mcpp.toml` 时，提示另说明它只在这类机器上成立，建议写入 `config.toml`。
- 宿主的身份相对锁改变时再提示一次（新的身份即新的键）。

### 3.4 工具链的解析与锁

- **一个解析函数。** 工具链的请求只有一个解析：部分版本（`gcc@16`、`gcc@16.2`）取**本地索引**中满足它的最新正式发布，
  需要时安装；预发布（`17.0.0-<日期>`）只由完整的版本选中（现状不变）。离线且本地索引不可用时取已安装的最新，并以 `degraded` 说明。
  `toolchain install`、`toolchain default` 与构建调用同一个函数。
- **写明的请求进入锁。** 工程写明的工具链请求（含部分版本）解析后写入 `mcpp.lock`：请求、解析出的版本与修订、配套载荷的版本
  （3.5）、宿主输入的身份（3.3）。之后的构建按锁取，`mcpp update` 重新解析。
  旧的锁没有工具链的条目：普通的构建写入一次；`--frozen` 与 `--locked` 之下不写锁，按请求解析并以 `note` 说明锁缺少这一条目，
  不拒绝，使已有的 CI 不因升级而失败。
- **没有写明的跟随引擎的缺省。** 工程没有写工具链时，取引擎的缺省（四层中的引擎层），不写入锁。缺省因 mcpp 的升级而改变时，
  一个工程提示一次：

  ```
  note: mcpp 2026.11.21.1 builds with gcc 16.2.0 by default (this project last built with 16.1.0);
        to keep a version, state it: [toolchain] default = "gcc@16.1.0"
  ```

  「上次所用的缺省」由提示机制记住（子方案一 3.6 的 `target/.mcpp-notices` 增加记住的事实）；新工程与 `mcpp clean` 之后没有可比的值，不提示。

### 3.5 配套载荷：由工具链声明，可以覆盖

- **版本取自声明。** binutils、linux-headers 等配套载荷的版本取自工具链载荷声明并由 xlings 解析的依赖（载荷的安装记录），
  不取目录的顺序。找不到声明的版本时是错误，指名缺少的包与版本。
- **binutils 属于工具链。** 它是编译器驱动的汇编器与链接器，GCC 在构建时按它探测汇编器的能力。覆盖写在工具链的陈述中
  （9.4 的表形式，`companions = { binutils = "xim:binutils@<版本>" }`）；版本低于 GCC 载荷构建时所用的 binutils 时拒绝并说明原因。
- **linux-headers 属于目标的 kernel-abi 层。** 覆盖用已有的层词汇：`[target.x86_64-linux-gnu] layers = { kernel-abi = "xim:linux-headers@<版本>" }`。
  头文件决定编译时可见的接口，运行时的最低内核由 C 库决定，docs/22 写明二者的区别。
- 两种覆盖都是索引中的包，版本固定、密闭，因此不属于宿主输入，不提示。

### 3.6 编译器的识别

- **版本由驱动的机器可读查询给出。** gcc 用 `-dumpfullversion`（GCC 7 起），不可用时用 `-dumpversion`；clang 用 `-dumpversion`；
  横幅只用于判定家族与驱动的指纹。Fedora 的分支快照因此是 16.2.1，与 GCC 的编号规则一致（`X.Y.1` 为 `X.Y.0` 之后分支上的开发）。
- **vendor 段是任意的词。** 与 GNU `config.sub` 相同：架构之后、系统段之前的段是 vendor，取任何词；系统段之后的段仍按词表严格判定。
  `x86_64-redhat-linux` 即 `x86_64-linux-gnu`。
- **本地工具链的三元组无法解析时报告**（`degraded`，说明哪些按目标的设置因此没有生效），不静默地以原文继续。

### 3.7 缺省版本的规则

**规则。** mcpp 的每个缺省（工具链、配套载荷、基础层、引导工具）取发布时上游最新的正式发布，经 mcpp 的 CI 与 canary 验证；
更新的版本与实验版本由用户经 xlings 的包显式陈述。缺省是 `builtin.toml` 中的数据；发版的规程检查每个缺省与上游的最新正式发布。

**包的版本号。** xim-pkgindex 中一个包的版本是上游的版本；同一上游版本的重新打包以 `revision` 表示，不新增上游没有的版本号。
已发布的键不改名；`binutils@2.42.1` 保留，此后的版本按本规则。

**本轮的刷新。**

| 缺省 | 现状 | 上游最新的正式发布 | 需要的载荷 |
|---|---|---|---|
| gcc（Linux x86_64、Windows 的 MinGW） | 16.1.0 | 16.2.0 | glibc 的 gcc 已有（x86_64、aarch64）；musl-gcc、mingw-gcc、mingw-cross-gcc 的 16.2.0 待构建；gcc 16.2.0 以新的 binutils 重建一个修订 |
| gcc（其余的 Linux 架构） | 15.1.0-musl | 16.2.0 | musl-gcc 16.2.0 |
| llvm（macOS、Windows、Linux aarch64） | 23.1.3 | 23.1.6 | 各宿主的 llvm 23.1.6 |
| binutils | 2.42（键 2.42.1） | 2.47 | x86_64、aarch64（aarch64 不含硬链接，Android 拒绝 `link(2)`） |
| linux-headers | 5.11.1 | 6.18 longterm 或 7.3 stable（9.5） | 一个 |
| patchelf | 0.18.0 | 0.19.3 | x86_64、aarch64 |
| ninja | 1.12.1-mcpp.1 | 1.13.2 | 1.13.2 含 ninja#2662 的修正时取上游，否则为 1.13.2 的 mcpp 修订 |

### 3.8 提示与拒绝

| 情形 | 级别、策略 | 代码或原因标记 |
|---|---|---|
| 离线时所有来源都没有所需的归档 | 错误；一次列出全部，并给出在联网的机器上执行的 `mcpp fetch --to` | `artifact-unavailable-offline` |
| bundle 的索引快照中没有锁所记的版本 | 错误；列出这些版本 | `bundle-older-than-lock` |
| bundle 不含本宿主或所选目标的载荷 | `warning`，`always` | `bundle-lacks-target` |
| 未开启离线而网络不可用，使用了本地的来源 | 沿用现有的「刷新失败、使用本地」的警告，写明来源 | — |
| 宿主输入的开启（3.3） | `note`，`once` | `host-input` |
| 宿主输入的身份相对锁改变 | `note`，`once`（新的身份即新的键） | `host-input-changed` |
| 引擎的缺省工具链改变（3.4） | `note`，`once` | `default-toolchain-changed` |
| 机器的路径写在 `mcpp.toml` 中（`archives`、宿主输入） | `note`，`once` | `machine-path-in-manifest` |
| 离线且本地索引不可用，部分版本取已安装的最新 | `degraded`，`always` | — |
| 本地工具链的三元组无法解析 | `degraded`，`always` | — |
| 覆盖的 binutils 低于 GCC 载荷构建时所用 | 错误 | `companion-too-old` |
| `fetch --to`、`--from` 完成 | 一行汇总：包的数目、大小、目标 | — |

## 4. 各仓库的范围与次序

| 仓库 | 范围 | 依赖 |
|---|---|---|
| xlings | 3.1：来源列表、目录来源的 sha256 索引、`archives` 配置、`download --to --with-index`、`install --from`、离线时一次列出缺少的归档；文档；发版 | — |
| xlings-res（载荷的构建） | 3.7 表中待构建的载荷 | — |
| xim-pkgindex | 3.7 的版本：binutils 2.47、linux-headers、patchelf 0.19.3、gcc 16.2.0 的重建修订与各家族的 16.2.0、llvm 23.1.6、ninja；gcc 的依赖改到这些版本；包版本号的规则写入其贡献说明 | xlings-res；xlings 的新版（recipe 若使用新的字段） |
| mcpp（#984） | 3.2 至 3.6、3.8；`builtin.toml` 的缺省（3.7）；mcpp 所捆绑与所固定的 xlings 版本 | xlings 的发版；xim-pkgindex 合入并发布 |
| mcpp-index | 无改动 | — |

xlings 与 xlings-res 的工作可以并行；xim-pkgindex 在载荷发布之后合入；mcpp 的缺省在 xim-pkgindex 发布之后切换，其余各项与它们并行实施。

## 5. 规范与文档

- **SPEC-004 §2** 的键：`[build] offline`、`[build] allow_host_tools`、`[xlings] archives`（机器层）。
- **SPEC-006** 工具链管理：一个解析函数；写明的请求进入锁；配套载荷取自声明；覆盖；缺省是数据及其规则；编译器版本的查询。
- **SPEC-002 与 docs/21** 三元组的 vendor 段。
- **home 的初始化** 的规范：按需检查引导工具；来源与 bundle。
- **使用文档（英、中）**：docs/05（离线与 `mcpp fetch`）、docs/20（解析、锁、配套载荷、覆盖）、docs/21（vendor 段）、docs/22（kernel-abi 的覆盖与运行时内核）、
  docs/25（来源、bundle、宿主输入）、docs/51（缺省版本的规则与本轮的版本）、docs/90（离线构建 mcpp 自身）、docs/50（新的代码与原因标记）。
- **技能**：mcpp-release 增加缺省与上游最新正式发布的核对；xlings 的文档陈述来源与 bundle。
- **CHANGELOG** 各仓库各自的条目。

## 6. 测试

| 编号 | 断言 | 位置 |
|---|---|---|
| X1 | 目录来源按 sha256 命中，文件名任意；sha256 不符的文件不被采用；来源的次序；目录的索引在文件改变时更新 | xlings 的测试 |
| X2 | `download --to --with-index` 写出的目录可被 `install --from` 在无网络的命名空间中使用；缺少时一次列出全部 | xlings 的测试（`unshare -n`） |
| F1 | `mcpp fetch --to` 后在无网络的命名空间中 `mcpp fetch --from` 与 `mcpp build --offline` 成功；含 git 依赖与构建程序的依赖 | mcpp e2e |
| F2 | 离线时缺少归档：一次列出全部并给出命令；bundle 比锁旧时的错误 | mcpp e2e |
| F3 | mcpp 自身：干净的 home 先 `fetch --to`，再在无网络的命名空间中构建 | CI 的作业 |
| H1 | `allow_host_tools` 使用 `PATH` 中的 ninja 并提示一次；第二次构建不提示；`mcpp clean` 之后再提示 | mcpp e2e |
| H2 | 以宿主的工具链与 `glibc@system` 构建时，store 中没有 patchelf 不被拒绝 | mcpp e2e |
| H3 | 宿主输入的身份进入锁；身份改变时重新构建并提示一次 | mcpp e2e |
| R1 | `gcc@16` 在三处得到同一个回答（索引的最新）；写入锁；`mcpp update` 推进 | mcpp e2e 与单元测试 |
| R2 | 没有写工具链的工程在缺省改变后提示一次 | mcpp e2e |
| C1 | 配套载荷取声明的版本：store 中有多个版本时结果与目录顺序无关 | mcpp 单元测试与 e2e |
| C2 | binutils 与 kernel-abi 的覆盖生效；过旧的 binutils 被拒绝 | mcpp e2e |
| V1 | `-dumpfullversion` 的版本；Fedora 横幅的回归；vendor 段为任意词的三元组 | mcpp 单元测试（`modules/toolchain-model/tests`） |
| D1 | 缺省取自 `builtin.toml`；发版规程的核对脚本列出落后于上游的缺省 | mcpp 单元测试与工具的测试 |
| P1 | 新的代码与原因标记由面的账本覆盖（测试方案 5.16） | 面的账本 |

## 7. 未采纳

- **vendor（把依赖的源码复制进仓库）。** path 依赖与 upstream 已覆盖其用途；复制违背「依赖而不复制」。
- **每个工程一个 home。** home 是机器的 store，为多个工程共用；工程的需要由键与锁表达。
- **整体复制 home。** 载荷重定位后带绝对路径，只在相同路径下可用；体积为全部而非所需。
- **由 mcpp 把归档放进 xlings 的下载目录。** 依赖 xlings 不承诺的内部布局，且只对 mcpp 有用。
- **按文件名的本地镜像。** 标签归档的文件名在包之间重复；sha256 是索引已经给出的身份。
- **缺省自动跟随索引的最新。** 用户的缺省会随索引的更新而改变，且未经 mcpp 的 CI 验证。
- **binutils 作为独立的层。** 它属于工具链，GCC 的能力在构建时依它而定；五层是封闭的集合（SPEC-002）。
- **按时间或次数限制提示。** 见子方案一第 8 节。

## 8. 自评

- **架构。** xlings 增加一个来源列表与一对导出导入，mcpp 增加 `fetch` 与三个键；没有一方依赖另一方的内部布局。`fetch` 与构建共用解析，
  离线只是来源的不同。工具链的解析由三处合为一处，配套载荷由目录顺序改为声明。
- **稳定性与可复现。** 离线取得的字节与联网相同（sha256）；锁记录工具链与宿主的身份；配套载荷的选择与机器无关。
- **简洁。** 用户的操作是 `mcpp fetch --to`、`mcpp fetch --from` 与一个 `offline` 键；不需要了解 store、下载目录与归档的文件名。
- **用户体验。** 缺少的一次列全并给出命令；宿主输入与缺省的改变各说一次；正常的离线构建没有额外的输出。
- **兼容与无感升级。** 联网的用户行为不变；`--offline`、`MCPP_OFFLINE` 照旧；锁增加的工具链条目在旧锁缺少时写入一次；缺省的改变有提示，
  需要旧版本的工程写明它。
- **跨平台。** 来源按内容，不受文件名的大小写与非法字符影响；`--target` 可以重复，一个 bundle 可含多个目标；Windows 与 macOS 走同一条路径。
- **一致性。** 来源与 upstream 并列；三个键遵循四层；提示经由一个汇点；版本号遵循上游。
- **测试覆盖。** 第 6 节在 xlings 与 mcpp 两侧覆盖每一项机制，离线的断言在无网络的命名空间中成立；面的账本覆盖新的代码。
- **代价。** 3.7 的刷新需要构建多个工具链载荷（gcc 的三个家族、llvm 的四个宿主），是本方案中耗时最长的部分。

## 9. 待确认

9.1 **范围。** xlings 的 PR 与发版、xlings-res 的载荷、xim-pkgindex 的 PR 与 mcpp 的改动都属于本轮（第 4 节）。

9.2 **命令与键的名字。** `mcpp fetch [--to|--from]`；`[build] offline`；`[build] allow_host_tools`；`[xlings] archives` 与 `MCPP_ARCHIVES`；
xlings 的 `archives`、`download --to --with-index`、`install --from`。

9.3 **bundle 的内容。** 载荷、依赖的源码归档、索引的快照、git 依赖的 `git bundle` 与 `bundle.toml`，即一个目录是完整的离线输入。

9.4 **binutils 的覆盖的写法。** 工具链的陈述现有字符串形式（`"gcc@16.2.0"`）与表形式（`{ path = … }`、`{ configure = … }`），
`[toolchain]` 的其余键与子表是系统的名字（`linux`、`windows`）。候选：
- (a) 表形式增加包的一种：`default = { package = "gcc@16.2.0", companions = { binutils = "xim:binutils@2.47" } }`，`[toolchain.linux]` 等处同样可写；
- (b) 与工具链分开的表 `[toolchain.companions]`：与 `[toolchain.<系统>]` 共用一个命名空间，读者无法区分子表与系统名，不采用；
- (c) 不在 mcpp 中提供，以 xlings 包的修订发布配套的组合（以新的 binutils 发布 gcc 的新修订）：版本仍是数据，但用户不能在工程中覆盖，违反原则 3。
推荐 (a)：与 `{ path = …, tools = … }` 同为工具链陈述的表形式，`companions` 的值是包引用，与 `layers` 的值同一种写法。

9.5 **linux-headers 的缺省。** 6.18 longterm（推荐：与工具链的刷新节奏一致，头文件的接口在长期版本内稳定）或 7.3 stable（最新的接口）。

9.6 **锁中的工具链条目。** 只记写明的请求；没有写明的跟随引擎的缺省并提示（3.4）。

9.7 **本轮刷新的版本表（3.7）。** 包括 llvm 23.1.6 与 ninja 在内；ninja 取上游还是 mcpp 修订，由 #2662 的修正是否在 1.13.2 中决定，实施中核对。
