---
subject: platform
status: review
---

# 生态设计：目标集合与腿、打包模型 rev、运行覆盖、不回落、外部构建接入、mcppls 单包与编辑器

日期：2026-10-29（rev 2：2026-10-30）。基线：mcpp `db3b8159`（2026.10.29.1）。前文：
[platforms design](2026-10-28-platforms-design.md)（SPEC-013）、
[part 2](2026-10-28-platforms-part2-packaging-and-distribution-design.md)（SPEC-014）、
[openkal run/test/pack](2026-10-29-openkal-run-test-pack-design.md)（SPEC-002 §6 的执行宿主）。
相关 issue：#849、#851、#852、#857、#860、openxlings/xlings#708。相关规范：SPEC-002、SPEC-007、SPEC-011、SPEC-013、SPEC-014，
mcppls S4；新增 SPEC-015（目标集合与腿）。

**修订记录**

- rev 1：综合 2026-10-29 的讨论。已定：#849 的 APE 实现（openkal-cosmo）独立立项、先做 probe，本批只留位置；运行器的宿主列表由
  平台声明；kit 移出程序包、包内带 openkal kit；VS Code 扩展与 mcpp-vscode 0.9.0 一模一样再微调；目标集合并入本设计。
- rev 2（本稿，评审）：三个视角的自我评审（核心架构与规范、命令行与迁移、生态可行性）逐条处理，见 §14。主要变化：集合的展开
  在 CLI 层、按平台选择后取交集（§3.1）；腿的构建复用工作空间多配置的机制（§3.3）；启动器统一为 POSIX 脚本加引擎的 Windows
  启动程序，取消"单系统本机启动器"与载体的新形式（§4.3）；`runs_on` 重定义（§4.4）；描述符键改为目标表的一列、不改变已有
  行为的部分写明（§5.1）；`exceptions = true` 保留原义（§6）；#857 的方案按 openkal-linux 0.18.2 的实际代码重写，probe 前置
  （§7）；外部构建改为计划阶段的契约文件加扩展 `deps-cmake`（§8）；kit 的离线语义如实写出，S4 修订（§9.2）；VS Code 的"一模
  一样"定义在渲染结果上，HTML 生成移入页面以与 CLion 共用（§10.1）；被替换的条款、下游迁移、完整级联（§12、§13）。

## 0. 摘要

`--target x86_64-openkal` 在 `pack` 里打出全部架构，在 `build`、`run` 里只是 x86_64：同一参数两种语义，一条与多条 `--target` 的结果
不单调。根因有两个：

1. **名字被静默放大或替换。** 架构被平台的 `universal` 放大；不认识的系统回落到宿主的描述符分段（#851）；有歧义的裸名绑到另一个包
   （#852）；分发条目把架构扩到所有系统（#860）。
2. **"在哪里运行"没有一份数据。** 映像架构、运行器所在的系统、载体能构建的行、PATH 回落各处自行推算。

本批的模型：

- **目标集合（SPEC-015）。** `--target` 精确；首段是系统名的写法（`openkal`、`linux-musl`）是集合，在 CLI 层展开为平台目标中
  余下部分相同的全部目标。腿的规划与构建复用工作空间多配置的机制，`build`、`run`、`test`、`pack`、IDE 看到同一组腿。
- **打包（SPEC-014 rev）。** 删除 `universal`：腿怎样合并由目标的种类与平台能力决定；openkal 的包只有一种布局，启动器是 POSIX
  脚本与引擎的 Windows 启动程序；`--pkg-format` 可写列表；`--systems` 接受 `<os>`、`<os>-<arch>` 与 `all`。
- **运行覆盖。** 打包时计算一次 `runs_on`，分发条目、启动器、合并、摘要与 CI 都读它；单文件定义为"最少的载体覆盖腿 × 系统"，
  APE 载体到来时命令不变。
- **不回落。** 名字只解析到它确切指代的东西；显式声明的别名与展开不是回落；改变行为的地方逐条写出。
- **外部构建接入。** 引擎在计划阶段写出目标的编译、链接、宿主三段契约；mcpp-plugins 的 `deps-cmake` 读它；静态库经 `link_flag`
  进入 mcpp 的链接与映像收尾。clangd 因此可以成为 ok 映像，mcppls 收敛为一个包。
- **编辑器。** VS Code 扩展的界面与交互以 mcpp-vscode 0.9.0 为准，一致性定义在渲染结果上；数据经 mcpp-ide 后端的逐视图适配层
  获得；0.9.0 的页面成为 VS Code 与 CLion 共用的组件库。

## 1. 问题

| 编号 | 观察 | 位置 / 来源 |
|---|---|---|
| P1 | 平台声明 `universal = true` 时，单个 `--target x86_64-openkal` 打出平台全部 `targets`；三元组的架构只决定第一条腿 | `src/pack/pipeline.cppm:410`、`:638` |
| P2 | 一个 `--target` 得到全部腿，两个得到恰好两条 | `src/cli/cmd_publish.cppm:905–915` |
| P3 | `universal` 同时决定"几个架构"与"什么布局"；单架构的跨系统布局、单架构 okb 写不出 | SPEC-014 §3 |
| P4 | 行为由依赖图里平台包的一个字段决定 | openkal-platform `mcpp.toml:32` |
| P5 | 腿的循环散在四处，各自 `prepare_build`：Android 的额外腿、通用包、库包、平台包的 `mcpp test` | `pipeline.cppm:162`、`:651`，`library_pipeline.cppm:174`，`cmd_build.cppm:1297–1320` |
| P6 | 分发条目给通用包写三个系统 × 全部映像架构，而包只带四个 `<os>-<arch>` 的运行器（#860） | `src/pack/distribution.cppm:1537–1560` |
| P7 | 单文件每 `<os>-<arch>` 一个；"一个文件到处运行"（#849）在模型里没有位置 | SPEC-014 §4 |
| P8 | `TargetPlatform::for_os` 对不认识的系统回落到宿主：`emscripten`（#851.1）、`openkal`、`ios`、`none`。Android 解析为 `os = linux`、`env = android`，不在此列。该值还用于 semver 解析、版本条目与 revision、索引刷新 | `platform.cppm:114–119`，`toolchain.cpp:1220`，`graph_load.cpp:320`、`:826`，`xlings.cpp:364` |
| P9 | 两个末段同名的构建依赖：裸名绑到其中一个，每次构建警告（#852） | `src/build/prepare/graph.cpp`（`fillDepDirs`）、`src/build/build_program.cppm` |
| P10 | emcc 链接不带 `-O`；异常只有 JS 模型（#851.2/.3） | 链接参数组装 |
| P11 | Android 应用沙箱（`run-as`）中 mcpp 启动的每个程序 ENOENT，2026.10.26.2 起（#857）。openkal-linux 0.18.2 的启动是 `openat(O_PATH)` 加 `execveat(AT_EMPTY_PATH)`，ENOENT 时已有带 `same_file` 守卫的 `execve` 重试，所以 ENOENT 来自别处；注释所说"首次 ENOSYS 后改用 execve"没有代码实现 | openkal-linux `src/process.cpp:684–712`、`:340–347` |
| P12 | CMake 项目无法为平台目标编译；`deps-cmake` 只传编译器路径，不传平台标志与层的头文件；`ldflags` 不展开 `${mcpp.out_dir}` | mcpp-plugins `deps/cmake.cppm:380–559`，`plan.cpp:1164–1212` |
| P13 | mcppls 每平台一个包：服务器已是一个 okb，但载荷带每系统的原生 clangd 与按宿主选的 kit | mcppls `packaging/README.md`、`src/engine/payload.cpp:118`、`:169–190` |
| P14 | mcpp-vscode 1.0 相对 0.9.0 大幅回退：扩展端 TypeScript 22 147 → 1 932 行，库界面 5 043 → 87 行，缓存界面 2 292 → 72 行，全部面板 583 行、样式 71 行 | mcpp-vscode `src/`、mcpp-ide `webui/` |
| P15 | mcpp-clion 0.1.1：1 处将移除 API（`CPPToolchains.Toolchain.getDebuggerKind()`）、4 处弃用、4 处实验 API；261 上 optional 依赖 `intellij.testRunner.plugin` 不可解析 | Marketplace 校验与本地 verifier 报告 |
| P16 | mcpp-ide 的 CI 打两次包，每次构建 aarch64 腿 | mcpp-ide `.github/workflows/ci.yml:62–64` |
| P17 | mcpp-vscode 1.0.1 在 Marketplace，Open VSX 仍为 0.9.0；仓库未归档 | 两个市场的查询 |

## 2. 原则

1. **写什么做什么。** 参数精确；范围的扩大由用户写出（集合、多个目标、`all`）。
2. **不静默。** 名字解析不到确切的对象就不绑定或拒绝并给出写法；显式声明的别名与展开不是回落；取交集、略去、改变行为都要说出来。
3. **一维一参，默认在 manifest。** 平台声明能力，不声明项目的默认行为。
4. **一个事实一处计算。** "在哪里运行"打包时算一次。
5. **渐进披露。** 零参数做对的事；控制随层级出现；提示教下一层的写法。
6. **核心只放通用契约。** 构建系统、工具与容器格式的细节在插件。
7. **机器输出只增不改。** 单腿、单容器时输出逐字节不变（docs/50 §7）。

## 3. 目标集合与腿（SPEC-015，新）

### 3.1 写法与展开

- **精确目标**：三元组，含义不变。
- **目标集合**：首段属于系统词表（`linux`、`macos`、`windows`、`openkal`、`emscripten`、`ios`、`android`、`none`）的写法。`triple::parse`
  不校验首段（`unknown-linux-gnu` 会解析成架构 `unknown`），所以集合靠首段的词表识别，不靠解析失败。
- **展开在 CLI 层、prepare 之前**：
  1. 按 docs/21 的规则补全环境（`linux` → `linux-gnu`；`windows` 补为 `windows-gnu`，不含 msvc）。
  2. 候选：内置目标表中系统与环境相同、且不是 planned 的行。
  3. 对每个候选按现有规则选平台（`prepare/platform.cpp:144–160`：目标行的平台列、`[target.<sel>] platform`、`MCPP_<T>_PLATFORM`）。
     候选必须选到同一个平台，否则拒绝并写出分歧。
  4. 与该平台的目标取交集：平台行用 `[platform] targets`，hosted 安排用 `[platform.hosted] targets`；无平台的行取候选本身。
  5. 结果为空时拒绝，列出可用的目标。
- 多个 `--target` 取并集、去重、按目标表的顺序。机器输出与摘要只写展开后的精确目标；`Packed`/`Finished` 的摘要列出各腿，展开随
  平台版本变化时用户看得见。
- `[build] target` 只接受精确目标；集合只出现在命令行与 `[pack] targets`。

| 写法 | 展开（当前内置表与 openkal-platform 0.17.3） |
|---|---|
| `openkal` | `x86_64-openkal`、`aarch64-openkal` |
| `linux-musl` | `x86_64-linux-musl`、`aarch64-linux-musl`（planned 的 `riscv64-linux-musl` 不在内） |
| `linux` | `x86_64-linux-gnu`、`aarch64-linux-gnu` |
| `none-elf` | 内置表中全部非 planned 的裸机 elf 行 |

### 3.2 命令的语义

| 命令 | 精确目标 | 目标集合 |
|---|---|---|
| `build` | 构建这条腿 | 构建全部腿，每条腿在 `target/<triple>/` 下 |
| `run` | 本机架构经运行器；另一架构按 SPEC-002 §6 经执行宿主，否则拒绝（2026.10.29.1 的 `runner_reach`） | 选一条能到达的腿，本机架构优先；只构建这一条；写出所选的腿；都不能到达则拒绝 |
| `test` | 同 `run` 的到达规则；到达不了即拒绝（SPEC-013 §10） | 构建全部腿，运行能到达的腿，其余逐条报告"已构建、未运行"并给出原因；**一个测试都没运行时失败** |
| `pack` | 一个包，一条腿 | 按 §4.2 |
| `self env` | 不变 | `mcpp.env` 增加 `legs[]`，每项是一份精确目标的报告；顶层取第一条腿 |
| IDE（`targets/list`） | 每个精确目标一行 | 集合一行，如 `openkal (x86_64, aarch64)` |

平台包里不带 `--target` 的 `mcpp test` 今天就遍历平台的 `targets`（`cmd_build.cppm:1297–1320`），即"集合"语义的一个特例；本设计
把它统一到上表，行为不变。

### 3.3 架构：腿的规划串行、构建并行

- 复用工作空间多配置构建的机制（`cmd_build.cppm` 约 395–430 行）：**逐腿串行规划**（`prepare_build` 一次一腿，`mcpp.lock` 只在
  规划阶段写，避免 SPEC-011 §4 的乐观写入冲突），**再并行构建**（作业数按腿静态分摊 `[build] jobs`，`progress::configurations`、
  `defer_finished` 给出一个 `Finished`）。多把构建目录锁按目标名排序获取。
- 四处腿循环（P5）收拢到这一处：Android 的额外腿、通用包、库包、平台包的 `mcpp test`。`pack` 只暂存构建层给出的产物。
- 单值的地方扩展：`BuildOverrides.target_triple` 改为腿列表（单腿时与今天相同）；快速路径只用于单腿；`.last-result.json` 与结果
  记录的 `configuration` 每腿一份，单腿时文件与内容不变。
- 默认：`[pack] targets` 是打包的默认目标集合；`build`、`run`、`test` 的默认仍是 `[build] target`。`mcpp new` 的 openkal 模板写上
  `[pack] targets = ["openkal"]`。

### 3.4 构建一次，各处验证

- 写：`mcpp test --target openkal --no-run --archive <file>`：构建各腿的测试程序，与清单 `mcpp-tests.json`（每个测试的腿、参数、
  工作目录、期望）一起写成引擎的 tar（核心不认识 okb）。
- 读：`mcpp test --archive <file>`：在另一台机器上用本机的运行器或系统运行归档中本机能到达的测试，不构建；工作目录是项目检出；
  报告与 `mcpp test` 相同；一个测试都没运行时失败。
- 同样适用于原生交叉：Linux 上构建的 `x86_64-windows-gnu` 测试拿到 Windows 上运行。
- mcppls 与 mcpp-ide 的 CI：Linux 上构建一次，macOS 与 Windows 上 `--archive` 运行。

### 3.5 机器输出

- `mcpp.build`：单腿逐字节不变。多腿时顶层字段取第一条腿，`status` 汇总全部腿，新增 `legs[]`（每项一份完整结果记录）；`artifact`
  与 `diagnostic` 事件新增 `triple`。
- `mcpp.pack`：新增 `pkgFormats[]`（`pkgFormat` 保留为第一个容器）、`legs[]`、`runsOn`（驼峰，同 §4.4）；`artifacts[]` 照旧。

## 4. 打包模型（SPEC-014 rev）

### 4.1 维度

| 问题 | 参数 | `[pack]` | 说明 |
|---|---|---|---|
| 为哪些目标 | `--target`（可重复，精确或集合） | `targets` | 不再被平台放大 |
| 运行的机器提供什么 | `--mode` | `default_mode`（新增接受规范名 `system`、`vendored`、`self-contained`、`static`） | 不新增 `mode` 键 |
| 库在程序里还是旁边 | `--linkage` | `linkage` | 不变 |
| 交付成什么 | `--pkg-format`（可写列表） | `format`（字符串或列表） | 见 §4.6 |
| 运行器带上哪些 | `--systems`：`linux`、`linux-aarch64`、`all` 的列表 | `systems` | `<os>` 取腿的全部架构；`all` 取平台运行器能到达的全部；默认本机系统 |

### 4.2 腿的合并

| 目标的种类与平台能力 | 多条腿的结果 |
|---|---|
| `lib` / `shared` | 一个库包（已有，12-binary-distribution） |
| 程序，平台声明 `runner`（openkal） | 一棵树（§4.3），包名 `<name>-<version>-<平台名>`（今天的 `universalName`，如 `openkal`） |
| 程序，Android 行 | 一个 APK（已有） |
| 其他程序 | 每腿一个包，各以三元组命名；结果多于一个时 `-o` 只能是目录 |

- 单腿的包名是三元组，模式后缀规则不变。
- 工作空间多成员打包接受集合：每个成员按上表（解除 `cmd_publish.cppm` 约 841 行的多三元组拒绝）。
- 删除 `--no-universal`、`[pack] universal`、`[platform] universal` 的作用，按 §13.2 的迁移规则处理。

### 4.3 openkal 的包：一种布局，启动器统一

```
<name>-<version>-<openkal | x86_64-openkal>/
  bin/<name>                 POSIX 启动脚本
  bin/<name>.exe             Windows 启动程序（引擎的启动程序）
  lib/<arch>/<name>.oke      每条腿一组映像（含 .okso）
  libexec/okrun/<os>-<arch>/ 仅 self-contained
  share/…                    资源一份
  mcpp-package.json
```

- 布局与腿数、`--systems` 都无关。`.cmd` 不再生成（Ctrl-C 的"终止批处理操作吗"、进程名是 cmd 的问题随之消失）。
- **POSIX 脚本**：取包根，按 `uname -m` 选 `lib/<arch>/`；`uname -s` 为 `MINGW*`/`MSYS*`/`CYGWIN*` 时 `exec "$top/bin/<name>.exe"`
  （Git Bash 优先选完全同名的文件，GitHub Actions 的 Windows `shell: bash` 即此情形）。
- **Windows 启动程序**：引擎的启动程序（单文件所用的同一程序，基于 mcpplibs.os，为 `x86_64-windows-gnu` 构建），读包内
  `mcpp-package.json` 的 `launch`，按本机架构选映像，先找 `libexec/<runner>/windows-<arch>/`，再在 `runs_on.with` 覆盖的范围内找 PATH。
  自身位置取自 `GetModuleFileNameW`，不用 argv[0]；找不到映像时写出期望的路径。核心只知道 `launch`（运行器命令与每架构的入口），
  不知道 okrun。
- **启动器是派生文件**：`--merge` 合并后重新生成启动器与 `mcpp-package.json`，启动器不参与"同一路径两个摘要"的冲突检查。
- **M3（映像嵌入载体）只属于单文件**。单腿 self-contained 包不再是"一个嵌入映像的本机程序"，而是上面的树；需要一个本机程序
  的用户用 `--pkg-format single-file`。2026.10.29.1 rev 2 为 M3 写的 `emit xpkg` 规则改为读 §4.4 的 `runs_on`。
- openkal 的 `--mode static`（今天是静态链接加 M3）：改为 `--linkage static --mode self-contained` 的树，即一个静态 `.oke` 每架构、
  带运行器；单个嵌入文件用 `--linkage static --pkg-format single-file`。
- `[pack] sign` 对 `bin/`、`libexec/` 中的每个程序执行，含启动程序。

### 4.4 运行覆盖（`runs_on`）

打包时计算一次，写入 `mcpp-package.json`（schema 1 增加字段，不升级）：

```json
"runs_on": {
  "self": ["linux-aarch64", "linux-x86_64", "macos-aarch64", "windows-x86_64"],
  "with": { "okrun": ["…"] }
}
```

- **`self`**：凭包内携带的东西与该系统自身的库即可运行（原生包需要的系统库照旧由 `HOST-REQUIREMENTS` 列出）。
- **`with.<runner>`**：机器装有该运行器时可运行。

| 包 | `self` | `with.<runner>` |
|---|---|---|
| openkal `self-contained` | 腿的架构 × 构建出的运行器的 `<os>-<arch>`，取交集 | 腿的架构 × 平台 `runner.hosts`，减去 `self` |
| openkal `vendored` / `system` | 空 | 腿的架构 × 平台 `runner.hosts` |
| 原生单腿（任何模式） | 三元组的 `<os>-<arch>` | 空 |
| 单文件 | 每个文件各自的 `<os>-<arch>` | 空 |
| WebAssembly、Android、iOS、裸机 | 空（`emit xpkg` 照旧拒绝） | 空 |

- 平台声明运行器的宿主：`runner = { cmd = ["okrun", "{}"], host = "same-arch", hosts = [...] }`，即平台钉住的 openkal-image 构建的
  行。`host = "any"` 时 `hosts` 列出运行器程序存在的 `<os>-<arch>`，覆盖与腿的架构无关。
- `self` 与 `with` 都为空时拒绝打包。交集略去的组合在摘要中写出：`Runs on: linux-x86_64, … / not covered: x86_64-macos (no runner
  row)`。
- 已有的 `systems` 字段保留、含义不变（携带的运行器，现写为 `<os>-<arch>`），它是输入的记录，`runs_on` 是由它与腿、平台算出的答案。
- 读者：`emit xpkg`（每个系统块只写 `self ∪ with` 覆盖到的架构，`macos` 写 `macosx`；只在 `with` 中时 `deps = { "xim:okrun" }`；
  Windows 登记 `<name>.exe`）、启动器在覆盖外的错误文字、`--merge`（并集；旧包没有 `runs_on` 时由 `systems`、`legs`、平台算出）、
  `Packed` 摘要、`mcpp.pack` 的 `runsOn`、各项目 CI 的断言。
- 修复 #860；mcpp-ide 0.1.1 的索引条目重新生成。

### 4.5 单文件：最少的载体覆盖腿 × 系统

- 平台的 `carrier` 接受列表，每项增加 `hosts`（今天的单表仍接受，等于一项，`hosts` 同 `runner.hosts`）。执行宿主的运行器程序
  （SPEC-013 §10）取 `hosts` 覆盖"本机系统 × 目标架构"的那一项。
- `--pkg-format single-file`：选出覆盖"腿 × `--systems`"的最少载体文件；结果多于一个时 `-o` 只能是目录。
- **#849 的占位条款**：载体项可声明非按行构建的模型（`build = "<model>"`）；APE 载体的模型随 openkal-cosmo 定义，届时 `--systems all`
  得到一个文件，命令不变。openkal-cosmo 独立立项，先做 probe（MS ABI 与 SysV/AAPCS 的转换、Cosmopolitan 在 Windows 上的进程、
  终端、网络、各系统对 APE 的签名与放行）。

### 4.6 容器与列表

- okb 是平台插件提供的容器，含哪些架构由腿决定：`--target aarch64-openkal --pkg-format okb` 得单架构 okb。命名由插件定
  （`<name>-<version>.okb`，不变）。
- `--pkg-format tar,okb`：一次构建、一棵暂存树、依次写出各容器。规则：
  - 列表只能含读同一棵暂存树的容器（`tar`、`zip` 与插件格式）；`dir`、`single-file` 不进列表（拒绝）。
  - 列表时 `-o` 只能是目录（以 `/` 结尾或已存在的目录）；`--compress` 只作用于引擎容器。
  - `--mode self-contained` 而列表只有 okb：拒绝（okb v1 只含映像，运行器会被丢掉），提示加上 `tar`。
  - `mcpp run --pkg-format` 不接受列表。

### 4.7 拒绝表的变化

| 变化 | 行 |
|---|---|
| 删除 | 多个 `--target` 不在通用包里；平台没有 `runner` 不能打通用包 |
| 改写 | `--systems` 只在 `self-contained` 或单文件时有意义，其他组合拒绝；`[pack] systems` 不搭时提示后忽略（同 `[pack] compress` 的先例） |
| 新增 | 集合展开为空或候选选到不同平台；结果多于一个文件而 `-o` 是文件；列表中有 `dir`/`single-file`；self-contained 只配 okb；`runs_on` 为空 |

### 4.8 渐进披露

| 层 | 写法 | 得到 |
|---|---|---|
| L0 | `mcpp pack` | `[pack] targets`（未写则 `[build] target`）的 vendored tar |
| L1 | `--target openkal`、`--target x86_64-openkal`、`--pkg-format single-file\|okb` | 目标与形态 |
| L2 | `--mode`、`--systems`、`--linkage` | 闭包、运行器、链接 |
| L3（多在 manifest） | `--compress`、`sign`、`--merge`、`strip`、`debug_symbols`、`[pack.bundle-project]` | 细节 |

- `mcpp pack --help` 按 L0–L2 / L3 分组。
- 提示：单腿 openkal 打包时，`Packed` 后写"只含 x86_64；`--target openkal` 或 `[pack] targets = ["openkal"]` 打 openkal-platform 的全部架构"。

## 5. 不回落

### 5.1 描述符键

- 描述符键成为**目标表的一列**（沿用目标表已有的平台列与 `kPlatformNames` 词表），每行声明它读哪些键、按什么顺序；宿主不再参与。
  平台行由平台的 `[platform] descriptor_keys` 覆盖。
- 各行的取值与行为变化：

| 行 | 键（顺序） | 与今天相比 |
|---|---|---|
| Linux（含 android、musl） | `linux` | 不变 |
| macOS | `macosx` | 不变 |
| Windows | `windows` | 不变 |
| `wasm32-emscripten` | `emscripten`, `linux` | Windows、macOS 宿主上不再读宿主段；Linux 宿主上行为不变，taskflow 的 `-pthread` 由描述符增加 `emscripten` 段解决（mcpp-index：taskflow、freetype） |
| `<arch>-openkal` | 由 openkal-platform 声明，预计 `openkal`, `linux` | 先实测：compat 包的 openkal 构建依赖 `linux` 段的程度；Linux 与 macOS 宿主上各构建一次比较映像字节（同时检验与 #850 的关系） |
| iOS | `ios`, `macosx` | 不再读宿主段 |
| 裸机 `none` | `none` | 缺段时拒绝并写出包与目标 |

- 这一列同时供给 semver 解析、版本条目与 revision、索引刷新（P8 的读者），所以"读哪一段"在整个准备过程中一致。
- 宿主工具（`xim:` 的工具链、构建程序）仍按宿主键安装，这是宿主的事实，不是回落。
- 旧 mcpp 读到 `descriptor_keys` 时警告并忽略（SPEC-002 §5.1）。

### 5.2 末段同名的构建依赖（#852）

- 两个可见依赖末段相同、全名不同：裸名不绑定，不再每次构建警告。
- mcpp 发布歧义的候选（`MCPP_DEP_AMBIGUOUS_<TAIL>=openmui.rules,gongfu.rules`）；mcpp-plugins 的 `dep_dir("rules")` 读到它时失败并
  写出两个全名。

### 5.3 分发条目

即 §4.4（#860）。

## 6. Emscripten（#851.2、#851.3）

- profile 的优化级别进入 emcc 的**链接**（wasm-opt 在链接时运行）。issue 实测：hello world 的 wasm 367 420 → 312 657 B，JS 65 920 →
  13 137 B。
- `[target.<sel>.abi] exceptions` 增加取值 `"wasm"`（`-fwasm-exceptions`）与 `"js"`；`true` 保留原义（JS 模型，2026.9.12.3 起，
  docs/22:1281），并在 `mcpp why compat` 中建议 `"wasm"`。异常模型进入指纹；构建报告写出实际的模型。wasm 异常的运行环境要求
  （Safari ≥ 15.2、Node ≥ 17）写入文档。`mcpp new` 的 emscripten 模板写 `"wasm"`。
- 栈与堆：文档给出 `ldflags = ["-sALLOW_MEMORY_GROWTH=1", "-sSTACK_SIZE=1048576"]` 的配方，不增加核心键。

## 7. 程序的启动（#857）

openkal-linux 0.18.2 的启动（`src/process.cpp:684–712`）：`openat(base, name, O_PATH | O_CLOEXEC)`，再
`execveat(exe, "", AT_EMPTY_PATH)`；ENOENT 且绝对名可得（`named`，来自 `/proc/self/fd` 的 readlink）且与 `exe` 是同一文件
（`same_file`）时改用 `execve(绝对名)`；仍 ENOENT 时清除 CLOEXEC 再试；ENOSYS 且 `named` 时 `execve`。所以 run-as 下的 ENOENT 只能
来自：`openat` 本身、`named` 或 `same_file` 为假（readlink 或 `newfstatat` 被拒）、或 `execve` 本身。

- **probe 前置且是决定性的**：模拟器 job 中，一个静态程序在 `run-as com.termux` 下走真实路径的每一步（`openat O_PATH`、readlink
  `/proc/self/fd/N`、`newfstatat`、`execveat AT_EMPTY_PATH`、`execve`），记录每步的 errno 与 `logcat -b events` 的 avc。修复按测得的那
  一步设计。
- **不变的约束**：任何改用 `execve` 的重试都保留 `same_file` 守卫（`execve` 按名重新解析，rename、符号链接、命名空间、
  `(deleted)` 后缀都可能启动另一个文件）；守卫不成立时报告 `execveat` 的 errno。
- **绝对名的来源**：调用方以路径给出程序时，绝对名由调用方的路径直接得到，不经 `/proc` 的 readlink（预计这正是 run-as 下 `named`
  为假的原因，待 probe 确认）。
- **管道协议**：子进程由 `clone(SIGCHLD)` 创建，不共享地址空间，无法设置父进程的状态。管道改为带标签的消息：子进程在改用
  `execve` 之前写"已改用"，失败时写 errno；父进程据此记住此后直接用 `execve`。`:347` 的注释改为与实现一致。
- 级联见 §12。

## 8. 外部构建系统接入

### 8.1 核心：计划阶段写出契约文件

- 引擎在计划阶段为每条腿写出 `${mcpp.contract}`（与 `${mcpp.compile_db}` 同一机制，供 SPEC-007 动作读取），三段：
  - **编译**：每种语言的编译器程序、参数、包含目录（含目标侧包与层的头文件）、宏。
  - **链接**：链接驱动、参数、目标侧的库（供 CMake 的 try-compile；或由插件设 `CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY`）。
  - **宿主**：宿主图的编译器与参数，供需要宿主工具的项目（LLVM 的 `llvm-tblgen`、`clang-tblgen`）。
- 动作在构建中读这个文件，不嵌套调用 mcpp（`self env --target` 会走完整的 `prepare_build`、取构建目录锁，嵌套会与父构建争锁）。
- 核心不认识任何构建系统。

### 8.2 插件：扩展 mcpp-plugins 的 `deps-cmake`

- `deps-cmake`（`deps/cmake.cppm:380–559`）读契约生成 toolchain 文件：平台标志与层的头文件（今天只传编译器路径）、链接段、宿主段
  （宿主工具先为宿主构建）；缓存键 `toolset_identity` 加入契约的摘要。
- 产出的静态库在 `build.mcpp` 中经 `mcpp::link_flag` 给出（先例：`deps/deps.cppm:156–180`；`ldflags` 不展开 `${mcpp.out_dir}`）；一个
  `bin` 目标链接它们，mcpp 完成链接与平台的收尾（`okld note`、`okld check`、封装）。
- openkal-musl 的 `features.h` 在 C++ 模式下对 `restrict` 的处理需要 `-Drestrict=restrict` 才能编过 LLVM（手工实验所得）：在
  openkal-musl 修复，进入 §12 的级联。

## 9. mcppls：一个包服务全部系统与架构

### 9.1 组成

| 部分 | 现在（每平台一个载荷） | 本设计（一个包） |
|---|---|---|
| 服务器 | `mcppls.okb`（已统一） | 不变 |
| okrun | 本系统的一个 | `runner.hosts` 的全部 |
| clangd | 每系统原生（Ubuntu 20.04 + Clang 12、macOS 14、Windows + MSVC） | `clangd.okb`：两个架构的映像（§8 构建；§9.4 的 probe 决定） |
| kit | 按宿主选一个 | 包内 openkal kit；目标 kit 按需（§9.2） |

### 9.2 kit 按目标（S4 修订）

- kit 回答"用户的代码按什么目标编译"。服务器改为多 kit、按项目的目标选择（今天启动时只选一个，`payload.cpp:169–190`；载荷与两个
  编辑器端都拒绝非宿主平台的载荷，`payload.cpp:118`、`payload.ts:137`、`ServerLaunch.kt:67`）。
- **包内 openkal kit**：libc++ 头文件与 `std`、`std.compat` 模块源码、openkal-musl 头文件，每架构一组生成头文件
  （`include/<arch>-openkal/`），生成 `libc++.modules.json`。版本校验改为与 kit 自己的 libc++ 对齐（openkal kit 的 libc++ 是
  23.1.3 的补丁版，clangd 是 23.1.0；今天的校验要求与 clangd 版本相等，`payload.cpp:171`）。
- **目标 kit**（`x86_64-linux-gnu`、`x86_64-w64-mingw32`、`arm64-apple-darwin`，macOS 仍要求本机 SDK）是 xlings 的
  `mcppls-kit-<target>`，经同意后台安装；同时作为 release 资产，离线可手动安装。新增 kit 的同意与安装流程（今天的同意只覆盖构建
  工具下载，`workspace.cpp:4003–4013`、`downloadPrompt.ts`），VS Code 与 CLion 两端都有。
- **离线语义（行为变化，如实写出）**：没有编译器、离线、尚未装宿主 kit 时，宿主目标的文件以 openkal kit 的语义打开：`import std`
  与标准库可用，宿主的系统头文件（`windows.h`、Linux 内核头）与 `_WIN32` 等宿主宏不可用；状态栏写出缺什么与安装命令。在线时
  第一次需要即安装宿主 kit，之后与今天相同。mcpp 项目由 mcpp 提供工具链，不受影响。
- 要改的文件：mcppls `src/engine/payload.cpp`、`modules/pack/*`、`tools/devtools/src/{packaging,check,release,kit}.cpp`、CI 的 payload
  action、`editors/vscode/src/payload.ts`、CLion 端 `ServerLaunch.kt`；S4 规范。

### 9.3 包与分发

| 产物 | 现在 | 本设计 |
|---|---|---|
| vsix | 4 个（target platform） | 1 个 |
| CLion 插件 | 4 个（`com.intellij.modules.os.*`/`arch.*`） | 1 个；mcpp-ide `mide-plugins/clion/build.gradle.kts:36–53` 不再按宿主下载 |
| 载荷 | 4 个 | 1 个 |
| xlings | 每系统一项 | 一个资产三系统，可装范围取自 `runs_on` |
| 构建 | 4 种主机与工具链 | 一台 Linux；测试在四个系统上（§3.4 的 `--archive`） |

- 覆盖随 okrun 扩大：musl 系统、Alpine、Termux（#857 之后）可用，clangd 不再要求 glibc 2.31；Intel Mac、Windows arm64 在平台加入
  对应运行器行后可用。
- 体积（待实测）：vsix 约 50–60 MB（0.0.19 每平台 30–40 MB），前提是 clangd 映像的压缩率与原生相近；okb 以 stored 存放，安装后
  含另一架构的 clangd（约 75 MB）。

### 9.4 clangd 映像的 probe（进行中）

mcppls-clangd 的 draft PR（分支 `probe/clangd-openkal-image`），LLVM 源码经 xlings-res 的 GitCode 镜像获取。测量：编译错误清单、
四个系统上 `clangd --check`、Windows 的路径风格（openkal 在 Windows 上给出原生名字；LLVM 的 `Style::native` 在编译期决定，需要
补丁改为运行时读 `kal_env_system`）、与原生 clangd 的耗时与内存（musl 分配器的风险，必要时链接 mimalloc 或 scudo）、映像与 okb
大小。结论写入本节。

## 10. 编辑器

### 10.1 VS Code：与 mcpp-vscode 0.9.0 一模一样，再微调

**一致性定义在用户看得到的结果上**：命令、菜单、快捷键、设置、树视图、状态栏、通知与对话、webview 渲染出的 DOM 与样式、交互
流程，与 0.9.0 相同。代码组织可以不同。

- **0.9.0 没有可替换的数据接缝**：`views/models.ts` 自己读 `mcpp.toml`、`mcpp.lock`；直接 spawn 10 处分布在 7 个文件，11 个文件直接读
  文件系统。所以做法是**逐视图的数据适配层**：每个视图的视图模型由适配层从后端 RPC 得到，视图模型的形状与 0.9.0 相同。
- **后端补齐的 RPC**：项目模型（0.9 的 `models.ts` 所读）、`xpkg` 详情（0.9 的 `xpkg parse`）、`cache info`/`cache dir`、库索引定位、
  添加依赖。
- **留在扩展端的**：VS Code 原生能力（Task 的 `ProcessExecution`、`problemMatchers`、`withProgress`、文件监视）与 mcppls 扩展的
  bridge（模块图沿用 0.9 的 mcppls `query module` 数据源，保证图相同）。
- **HTML 生成移入页面**：0.9.0 的 HTML 写在扩展端的 TS 字符串里（如 `cachePanelHtml.ts`）。生成函数原样搬进 webview 的脚本包，扩展端
  只发视图模型；渲染结果不变，并使 CLion 能运行同一份页面（§10.2）。
- 1.0 新增而 0.9.0 没有的能力（调试、Task、Testing、打包、目标选择）按 0.9.0 的风格重做后加入。
- **一致性的门**（CI 中）：
  1. **manifest**：0.9.0 `package.json` 的 `contributes` 是新版**生成后** manifest 的子集，id、标题、图标、`when`、默认值相同
     （mcpp-ide 的 manifest 由 `npm run gen` 生成，比较生成结果）。
  2. **渲染**：同一视图模型夹具下，0.9.0 与新版各 webview 的 DOM 快照相同；原生视图（树、状态栏）在 xvfb 下截图比对。需要新建这一
     基础设施（0.9.0 与 mcpp-ide 都没有）。
  3. **流程**：按 0.9.0 的功能清单重写针对后端的 e2e（0.9.0 自己只有 7 个对 `fake-mcpp.js` 的冒烟测试）。
- 通过三道门后进入微调与优化。

### 10.2 共用组件库

0.9.0 的页面（搬入页面的 HTML 生成函数、CSS、交互脚本）成为 VS Code 与 CLion 共用的 `webui/`：
- 宿主桥：页面通过一个桥对象通信，VS Code 上是 `acquireVsCodeApi`，CLion（JCEF）上由插件注入同名对象。
- 主题：页面使用 `--vscode-*` 变量（0.9.0 有 248 处）；CLion 插件把 IntelliJ 主题映射为同名变量注入。
- 于是 CLion 的面板与 VS Code 0.9.0 的面板一致，页面代码不分叉。

### 10.3 CLion 的 API

- 替换 `isTrusted`、`startSession`、`runContentDescriptor`、`getDebuggerKind()`：替代 API 必须同时存在于 261（since-build）与 263，以两版
  jar 核对后定。
- `plugin.xml`：261 上 test runner 属于平台核心，optional 依赖只在其存在的版本声明，注释中的"2025.2"改正。

### 10.4 其他

- 目标选择器显示集合的一行（§3.2）。
- Open VSX 发布 1.0.x 之后的版本；mcpp-vscode 仓库在 Open VSX 同步后归档。

## 11. 测试

- SPEC-015：集合识别（首段词表；`unknown-linux-gnu` 仍是精确目标）、补全与展开（每行、planned 排除、空集、平台分歧、去重与顺序）；
  `build`/`run`/`test` 对集合的行为，含执行宿主（qemu-aarch64）、"已构建、未运行"、一个都没运行时失败；规划串行与 `mcpp.lock`；
  `--archive` 的写与读（CI 矩阵）；机器输出单腿逐字节不变（对照 2026.10.29.1 的输出）。
- 打包：§13.1 与 §13.2 每一行；`tar,okb` 一次构建（构建计数断言）；启动脚本在 Git Bash 下转到 `.exe`；`.exe` 启动程序经符号链接
  与改名后仍找到映像；`--merge` 重新生成启动器；`runs_on` 每种包与"未覆盖"一行；`emit xpkg` 按 `runs_on`（#860 的回归测试）；
  迁移规则的每条提示与拒绝。
- 不回落：描述符键表的每一行；wasm 构建在三种宿主上读同一段；openkal 映像在 Linux 与 macOS 宿主上字节相同；#852 的两个 rules。
- #857：probe 的逐步记录；Termux 应用沙箱 job 恢复通过。
- 外部构建：一个 CMake 静态库经 `deps-cmake` 与契约链接成 ok 映像并在三系统运行；宿主工具的 CMake 项目。
- 编辑器：§10.1 的三道门。

## 12. 交付（一轮）与级联

精确钉版本使一个修复逐层传递。顺序：

| 步 | 仓库 | 内容 |
|---|---|---|
| 0 | openkal-linux（probe） | #857 的逐步 probe（§7），决定修复 |
| 1 | openkal-linux | #857 的修复、管道协议 |
| 2 | openkal-musl | 钉 openkal-linux（`mcpp.toml:181`）；`restrict` 修复（§8.2） |
| 3 | openkal-llvm-runtime | 钉 openkal-musl |
| 4 | xim-pkgindex | 以上的索引条目（openkal-image 与 mcpplibs.os 的 CI 之前） |
| 5 | openkal-image | ld、run、tools 同一版本，钉 openkal-linux（`run/mcpp.toml:18`）；okrun 与载体重建 |
| 6 | openkal-platform | `[platform.versions]` 与层；`runner.hosts`、`carrier` 列表与 `hosts`、`descriptor_keys`（实测后）；`mcpp = ">=<本轮 mcpp>"` |
| 7 | mcpplibs.os | 钉新版本组 |
| 8 | mcpp | SPEC-015、SPEC-014 rev、§5、§6、§8.1；同时接受平台的新旧两种写法；`triple.cppm:909` 钉 openkal-platform 新版 |
| 9 | mcpp-plugins | `deps-cmake` 读契约；`dep_dir` 的歧义失败 |
| 10 | mcppls-clangd | clangd 映像（§9.4 决定）、路径风格补丁、宿主 tblgen |
| 11 | mcppls | 单包、S4 修订、kit 流程、CI 改用 `--archive` |
| 12 | mcpp-ide | VS Code（§10.1）、组件库、CLion API、CI 迁移（§13.1） |
| 13 | xim-pkgindex | mcpp-ide、mcppls、okrun 的条目重新生成 |
| 14 | mcpp-vscode | Open VSX 同步后归档 |

- **平台与 mcpp 的相互依赖**：平台的新键需要能读它们的 mcpp，而 mcpp 钉住平台。解法：本轮中平台的 CI 使用本轮 mcpp 发布分支构建
  的 mcpp（probe PR 的做法）；mcpp 同时接受新旧两种写法；两者在同一轮发布，mcpp 的发布钉住新平台。旧 mcpp 读到列表形式的
  `carrier` 会报错（`platform_toml.cppm:460–463`，不是"未知键警告"），所以平台的 `mcpp` 下限必须提高。
- #857 的 probe 在关键路径最前；其余在它的 CI 等待期间并行。

## 13. 迁移

### 13.1 下游

| 位置 | 今天 | 本设计 |
|---|---|---|
| mcpp-ide `ci.yml:62–64` | 两条 pack | `mcpp pack mcpp-ide --target openkal --mode self-contained --systems all --pkg-format tar,okb -o target/dist/` |
| mcpp-ide `ci.yml:67`、`release.yml:38–39` | 依赖 `mcpp-ide-$version.okb` | 名字不变 |
| mcpp-ide `ci.yml:70–75` | 读 `systems` | 改读 `runs_on.self`（`systems` 仍在，现写为 `<os>-<arch>`） |
| mcpp-ide `ci.yml:285`、README（中英） | `mcpp-ide.cmd` | `mcpp-ide.exe` |
| mcppls `ci.yml:304` | macOS 行 `--target x86_64-openkal --systems macos` | `--target openkal --systems macos` |
| mcppls `ci.yml:315`、`:430–435` | 读 `systems` | 改读 `runs_on` |
| mcppls `ci.yml:328` | `mcppls.cmd` | `mcppls.exe` |
| mcppls `ci.yml:345` | `--target x86_64-openkal --pkg-format okb` | `--target openkal --pkg-format okb` |
| xim-pkgindex | Windows 别名 `.cmd` | `.exe`（重新生成） |
| mcpp 文档 | docs/10（81、116–121、206–207、734–817、900–905、935）、docs/35、docs/24、docs/specs/packaging.md、docs/specs/platforms.md、docs/zh/*、docs/50:733 | 按本设计改写 |
| mcpp 测试 | e2e 942、951、955、956、961，`test_pack_distribution.cpp` | 按本设计改写 |

### 13.2 旧写法

| 写法 | 处理 |
|---|---|
| `--no-universal` + 精确目标 | 接受，提示布局的变化（M3 改为树；需要一个本机程序用 `--pkg-format single-file`） |
| `--no-universal` + 集合 | 拒绝（两者矛盾） |
| `[pack] universal = true` | 拒绝，给出 `[pack] targets = ["openkal"]` |
| `[pack] universal = false` | 提示后忽略 |
| `[platform] universal` | 提示后忽略 |
| 单个 `--target <arch>-openkal` | 打这一条腿，提示集合写法（§4.8） |

以上都登记进 `mcpp why compat`。

### 13.3 被替换的条款

| 规范 | 条款 | 变化 |
|---|---|---|
| SPEC-013 | §1.1 `universal` | 删除作用（§13.2） |
| SPEC-013 | §1.1 `carrier`（"三键必须给出"） | 接受列表与 `hosts`；载体项可声明构建模型（§4.5） |
| SPEC-013 | §10 执行宿主的运行器程序、§15.4 `fields.carrier` | 按 `hosts` 选择载体项 |
| SPEC-013 | §11 载体的三种形式 | "嵌入一个映像"只属于单文件；判据 13（e2e 961）改写 |
| SPEC-013 | §16 平台包的 `mcpp test` | 归入 SPEC-015 §3.2 |
| SPEC-014 | §1.4、§1.5 的相关行 | §4.2、§4.7 |
| SPEC-014 | §2.1、§2.2（通用包） | §4.2、§4.3 |
| SPEC-014 | §4.1（ok 映像单腿用 `bin/`）、§4.2 命名、§4.3 启动器与 PATH | §4.2、§4.3、§4.4 |
| SPEC-014 | §4.5 `systems`、§5.2、§8（M3 的 `emit xpkg` 规则）、§9 | §4.4 |
| SPEC-002 | §6 执行宿主 | 不变，被 SPEC-015 §3.2 引用 |

## 14. 自我评审的处理

三个视角（核心架构与规范、命令行与迁移、生态可行性）的意见与处理：

| 意见 | 处理 |
|---|---|
| 集合的展开源循环依赖，`linux-musl` 的来源有歧义，`parse` 不校验首段 | §3.1：首段词表识别、CLI 层展开、按候选选平台后取交集 |
| P8 的事实：Android 不回落；`targetPlatform` 还供 semver、版本条目、索引刷新 | P8 改正；§5.1 改为目标表的一列，逐行写出行为变化 |
| 没写被替换的条款；旧 mcpp 读列表 `carrier` 报错 | §13.3；§12 的下限与相互依赖 |
| 腿下沉与 `mcpp.lock` 并发；四处腿循环；单值字段 | §3.3：规划串行、构建并行，四处收拢，单值扩展 |
| "单系统本机启动器"与 `<os>-<arch>` 冲突；合并时启动器摘要冲突；`.cmd`/`.exe` 不一致 | §4.3：统一为 POSIX 脚本 + 引擎的 `.exe`，启动器是派生文件；取消载体的新形式 |
| `runs_on` 的 `carried` 定义与原生 `system` 不一致、与 `systems` 重复、`host = "any"`、PATH 回落 | §4.4：`self`/`with` 重定义、`systems` 保留为输入、`any` 的含义、PATH 只在 `with` 内 |
| 静默的不兼容改动；下游断点没列全 | §4.8 提示、§13.1、§13.2 |
| 机器输出违反 docs/50 §7 | §2 原则 7、§3.5 |
| 静默的交集与丢弃 | §4.4 的"未覆盖"、§4.6 的拒绝、§4.7 |
| `--pkg-format` 列表与 `-o`、`--compress`、`dir`、`single-file`、`run` 的交互 | §4.6 |
| Git Bash 选中 POSIX 脚本 | §4.3 |
| `test --archive/--from` 命名、okb、假绿 | §3.4：`--no-run --archive` 写、`--archive` 读、引擎 tar、无测试运行即失败 |
| `exceptions = true` 改义的风险 | §6：保留原义，新增取值 |
| `[pack] mode`、`[build] target` 的集合、`--systems` 的粒度、`run` 的选腿、工作空间、库目标 | §4.1、§3.1、§3.2、§4.2 |
| #857：代码已有带守卫的 ENOENT 重试；`clone` 不共享状态；`execve` 不等价 | §7 重写：probe 前置并决定修复、守卫保留、绝对名来源、带标签的管道协议 |
| `self env` 会构建、取锁；`ldflags` 不展开输出目录；缺链接与宿主契约；已有 `deps-cmake` | §8 重写 |
| 0.9.0 没有数据接缝；webview 是 VS Code 专用；三道门不现实 | §10.1、§10.2 重写：一致性定义在结果上、逐视图适配层、HTML 生成移入页面、门的重定义 |
| kit 的"默认行为不变"不成立；按目标选 kit 是新代码 | §9.2：如实写出离线语义；S4 修订与要改的文件 |
| 级联缺环节；平台与 mcpp 相互依赖 | §12 |

## 15. 评审项

1. 目标集合：首段系统词表识别、CLI 层展开、按平台取交集；`[build] target` 只收精确目标（§3.1）。
2. 腿的规划串行、构建并行，复用工作空间多配置机制（§3.3）。
3. `test` 对集合：一个测试都没运行即失败；`--no-run --archive` 写、`--archive` 读（§3.2、§3.4）。
4. 机器输出：单腿逐字节不变，多腿加 `legs[]`、事件加 `triple`、`pkgFormats[]`（§3.5）。
5. 删除 `universal` 与旧写法的处理表（§4.2、§13.2）。
6. openkal 唯一布局：POSIX 脚本 + 引擎的 `.exe`，取消 `.cmd`；M3 只属于单文件；openkal `--mode static` 的新含义（§4.3）。
7. `runs_on` 的 `self`/`with` 与 `runner.hosts`（§4.4）。
8. `carrier` 列表与 `hosts`、构建模型的占位（§4.5）。
9. `--pkg-format` 列表的规则（§4.6）。
10. 描述符键表的每一行，尤其 emscripten 的 `emscripten, linux` 与 openkal 先实测（§5.1）。
11. `exceptions` 保留 `true` 的原义、新增 `"wasm"`（§6）。
12. #857：probe 决定修复，守卫保留，带标签的管道协议（§7）。
13. 契约文件三段 + 扩展 `deps-cmake`（§8）。
14. kit 的离线语义（§9.2）。
15. VS Code 的一致性定义、逐视图适配层、HTML 生成移入页面、三道门（§10.1、§10.2）。
16. 交付顺序与平台、mcpp 同轮发布（§12）。
