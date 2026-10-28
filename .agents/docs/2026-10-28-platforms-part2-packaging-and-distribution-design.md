---
subject: platform
status: landed
---

# 平台 part 2：打包与分发——三维模型、通用包、单文件、okb、体积，以及 openkal 0.17.2 轮

日期：2026-10-28。前文：[platforms design](2026-10-28-platforms-design.md)（SPEC-013，平台机制）。本文是它的补充，覆盖第四、五轮讨论
（2026-10-28）定下的打包与分发、`.okso` 的本机绑定、体积，以及因此新增的 openkal 0.17.2 轮。相关规范：SPEC-007（构建插件）、
SPEC-012（输出）、SPEC-013（平台）、新增 SPEC-014（打包与分发）。

**修订记录**

- rev 1：定案——三维模型与 `--pkg-format`；okb v1 由 openkal-image 定义；体积优化本批实施并实测；`.okso` 本机绑定由
  openkal 体系的构建插件完成；APE 单文件记为后续（mcpp#849）。
- rev 2（本稿）：§14 的四项定案；新增 §15，以 mcppls 与 mcpp-ide 交叉验证 openkal 体系的独有特性。
- 落地（2026-10-29）：mcpp 2026.10.27.1（#847）与 2026.10.28.1（#854）；openkal-platform 0.17.3、openkal-musl 0.22.6、openkal-llvm-runtime 0.16.6、mcpplibs.os 0.2.1、openkal-image 0.1.3（okrun/okld 各系统程序）、mcpp-ide 0.1.0、mcppls 0.0.18；xim-pkgindex 收录 okrun、okld、mcpp-ide 与 mcppls 0.0.18（#1085）。

## 0. 摘要

`mcpp pack` 的选择拆成三个正交维度，各自回答一个问题：**链接**（`--linkage`，程序属性：库在程序里还是在旁边）、**闭包**
（`--mode`，打包属性：运行的机器要提供什么）、**容器**（`--pkg-format`，打包属性：交付成什么），外加**构建腿**（`--target`，可重复）。
一个平台可以声明其目标的包默认是**通用**的：一个包含全部架构，解压即可在任何系统、任何该平台架构上运行。单文件是通用的容器
（原生目标为启动桩加载荷，openkal 为载体从内存加载）；okb 是 openkal 的通用映像包，由 openkal-image 定义格式、由 openkal-platform 的
插件作为包格式提供，引擎不含 openkal 知识。体积在构建、剥离、容器、去重、共享五处优化，本批实测后写入。openkal 侧新增一轮 0.17.2：
okb 与 okrun 的 bundle 和库接口、`.okso` 本机绑定插件、openkal-musl 的 renameat2 修正。

## 1. 问题（讨论中确认）

| 编号 | 观察 |
|---|---|
| Q1 | `--format` 在报告类命令里是输出编码（`self env --format json`），在 `pack`、`run` 里是包格式；同名异义。 |
| Q2 | `--mode static` 把"程序全静态"（链接）与"机器什么都不用提供"（闭包）合在一个词里；openkal 有静态（一个 `.oke`）与共享（`.oke` 加 `.okso`）之分，却被说成"不适用"。 |
| Q3 | openkal 的价值——一份二进制对应"架构"而不是"系统 × 架构"——在包里没有体现：M2 是单架构的目录，M3 是单系统单架构的程序。 |
| Q4 | 没有单文件交付；用户需要自己解压。 |
| Q5 | 打包已近九千行，有包格式提供者（构建插件）的机制，却没有一份规范说明模型、阶段、暂存树与接口。 |
| Q6 | 体积：`--gc-sections` 已有而没有 `-ffunction-sections/-fdata-sections`，容器只有 gzip，资源按腿重复。 |
| Q7 | 一个 C 库要为每个系统各编一份原生库；`.okso` 只能被 ok 映像使用。 |
| Q8 | openkal-musl 的 `renameat2` 忽略 `RENAME_EXCHANGE`，第二个条目被覆盖（数据丢失；mcpplibs.os 0.2.1 的审计发现）。 |

## 2. 模型：三个维度与构建腿

| 维度 | 选项 | 归属 | 原生目标 | openkal 目标 |
|---|---|---|---|---|
| 链接 `--linkage` | `static` / `shared` | 程序属性；对应 `[build] dependency_linkage`，打包时按它重建 | Linux 上 `static` 还选 `<arch>-linux-musl` 行（全静态，今天的 `--mode static` 行为） | `static`：一个 `.oke`，无 `.okso`；`shared`：`.oke` 加 `.okso` |
| 闭包 `--mode` | `system` / `vendored` / `self-contained` | 打包属性：机器提供什么 | 系统库 / 加非系统库 / 加 C、C++ 运行时 | 机器有加载器与 `.okso` / 带 `.okso` / 再带加载器（载体） |
| 容器 `--pkg-format` | `dir` `tar` `zip` `single-file` 与提供者的格式（`okb`、`appimage`、`msi`…） | 打包属性：交付成什么 | `single-file`：启动桩加压缩载荷 | `single-file`：载体从内存加载；`okb`：通用映像包 |
| 腿 `--target`（可重复） | 三元组 | 一次打包覆盖哪些目标 | 每条一份产物，或合并为一棵树（已有：APK 的 universal） | 平台声明 `universal` 时默认覆盖平台的全部 `targets` |

- `--mode static` 保留为简写：`--linkage static --mode self-contained`。对原生目标与今天的行为相同；对 openkal 目标是"一个静态 `.oke` 加载体"。
- 组合由引擎在打包开始前检查，不可能的组合拒绝并给出出路（例如 `--mode system --pkg-format single-file`：单文件必须自带它所运行的
  东西）。
- **`--pkg-format`**：`pack` 与 `run` 的包格式选项改名为 `--pkg-format`；旧的 `--format` 继续接受并提示是旧写法。全局用法规则：构建类命令
  （build、test、run、pack）的机器输出是 `--message-format`；报告类命令的 `--format` 是输出编码；包格式只叫 `--pkg-format`。
- `[pack]` 增加 `linkage`、`format`（包格式的项目默认）、`compress`；`default_mode` 照旧。

## 3. 通用包（openkal 的默认）

平台在 `[platform]` 中写 `universal = true`（openkal-platform 写），其任一目标的 `mcpp pack` 默认覆盖平台的全部 `targets`；`--no-universal`
只打所选目标。布局（`dir`、`tar`、`zip` 相同）：

```
hello-0.1.0-openkal/              包名带平台名，不带系统与架构
  bin/hello                       POSIX 启动器
  bin/hello.cmd                   Windows 启动器
  lib/x86_64/hello.oke  …okso     每个架构一组映像
  lib/aarch64/hello.oke …okso
  libexec/okrun/<os>-<arch>/okrun[.exe]    仅 --mode self-contained
  share/…                         资源只放一份
  mcpp-package.json               SPEC-014 的包元数据
```

- **启动器由引擎生成**，内容只来自平台的 `runner` 与腿：按 `uname -m` / `%PROCESSOR_ARCHITECTURE%` 选架构目录（映射是引擎的通用知识：
  `x86_64|amd64|AMD64 → x86_64`，`aarch64|arm64|ARM64 → aarch64`），按 `runner.cmd` 起动；`self-contained` 时先找包内
  `libexec/okrun/<os>-<arch>/`，再找 PATH。
- **self-contained 的多系统加载器**：`--systems linux,macos,windows`（默认本机系统）为每个系统 × 架构交叉构建载体的加载器程序。缺少某个
  系统的 SDK 载荷时拒绝并点名；CI 矩阵可各自产出后用 `mcpp pack --merge a.zip b.zip …` 按 `mcpp-package.json` 合并为一棵树（腿与系统取并集，
  同名映像的摘要必须相同）。
- 结果：同一个解压目录在任何系统、任何平台架构上可运行（vendored 需要机器上有 okrun；self-contained 什么都不需要）。

## 4. 单文件（通用）

| 组合 | 产物 | 运行 |
|---|---|---|
| 原生 `vendored`/`self-contained` + `single-file` | 引擎的启动桩 + zstd 压缩的暂存树 | 首次运行把载荷解到用户缓存（按载荷摘要寻址，原子改名），之后直接 exec；缓存根：`$XDG_CACHE_HOME`/`~/.cache`、`~/Library/Caches`、`%LOCALAPPDATA%` |
| 原生 `static` + `single-file`（Linux） | 静态启动桩 + 静态程序 | 同上，零依赖 |
| openkal `self-contained` + `single-file` | 平台的载体 + 本架构的映像组（okb 切片） | 载体经 openkal-run 的 bundle 接口从内存加载，不写盘；Linux 上载体静态，零依赖 |
| openkal `vendored` + `okb` | 通用映像包（§5） | `okrun app.okb`，或 xlings 的 shim；任何系统、任何架构 |

- 启动桩的源码随引擎提供（同 engine package 的机制），打包时为目标构建，基于 mcpplibs.os；它只做"校验、解压、exec"。
- 载体从内存加载需要 openkal-run 的 bundle 接口（§8）；载体的入口源码是平台的（`carrier.main`），引擎只把字节放进节、给出
  `mcpp_carried_*` 符号。
- 一个文件在所有系统、所有架构上运行（APE，Cosmopolitan）不在本批：需要基于 Cosmopolitan 的 openkal 实现；记于 mcpp#849。

## 5. okb v1（openkal-image 定义）

格式规范写在 openkal-image 的 `docs/okb.md`，是 okrun 的输入格式，不进入 openkal 核心规范（SPEC 12），因此不引起实现与 musl 的级联。

- **容器**：标准 ZIP（允许 zip64），任何 zip 工具可查看。
- **映像**：以 stored 存放，数据起点按 64 KiB 对齐（与映像的 `max-page-size=65536` 一致，用额外字段补齐，同 zipalign），okrun 直接 mmap 原文件
  的段，不复制、可跨进程共享页。
- **v1 只含映像与清单**。程序在运行时读取的数据文件随通用包交付；映像从 okb 内读取资源需要加载器提供的文件视图，属于格式的后续版本，
  v1 的清单以 `okb` 版本号为将来留出位置。
- **清单 `okb.json`**：`okb`（版本 1）、`name`、`version`、`entry`、每个架构的目录与入口映像、每个条目的 sha256。
- **解析顺序**：本架构目录 → 包内 `lib/` → okrun 的搜索路径（含 xlings 的包存储，§7）。
- **信任**：每个映像各自 `okld sign`；清单可签名，签名覆盖全部条目摘要；okrun 的 `require_signature` 对整个 okb 生效。
- **工具**：`okld bundle <out.okb> …` 写一个 okb（格式的拥有者提供写入器），`okld bundle check` 校验对齐、摘要与签名。
- **由平台作为包格式提供**：openkal-platform 的插件（`plugin = "build.mcpp"`，平台阶段）以已有的包格式机制（SPEC-007 `pack-format`、
  `role = "artifact"` 的 action，第二遍读暂存树）声明 `okb`，动作调用 `okld bundle`。引擎不认识 okb；平台阶段的指令表因此加入
  `pack-format` 与 artifact action。

## 6. SPEC-014：打包与分发

新规范，本批写定，`pack` 的实现与之对齐。

1. **模型**：§2 的三个维度与腿；组合的合法性表；`--mode static` 的定义。
2. **管线**：构建各腿 → 按 `mode` 收集闭包 → 生成暂存树 → 变换（strip、relocate、平台的 `seal`、签名）→ 写容器 → 产物、校验和、
   `mcpp.pack` 信封。`--merge` 是"读入若干暂存树 → 合并 → 写容器"。
3. **暂存树与元数据**：布局（§3）；`mcpp-package.json` 的 schema（名称、版本、平台、腿、系统、每个文件的角色与摘要、启动方式）。这是
   包格式提供者读的输入，也是 `--merge` 与 `mcpp emit xpkg` 读的输入。
4. **接口**（面向构建插件、框架与系统）：
   - 包格式提供者：构建插件或平台插件以 `pack-format` 声明、以 artifact action 生成（已有，规范化输入输出契约：暂存树、
     `mcpp-package.json`、`MCPP_PACK_*` 环境，产物路径与种类）。
   - 启动器与载体：引擎的通用启动桩；平台的 `carrier`。
   - 签名器：`[pack] sign = [argv]`（`{}` 为文件）或插件；签名在写容器之前、对暂存树内的程序与映像。
   - 分发描述：`mcpp emit xpkg` 由 `mcpp-package.json` 生成索引条目——每个系统 × 架构一项，或"一个通用资产对应所有系统"一项。
5. **机器输出**：`mcpp.pack` 信封增加 `linkage`、`mode`、`pkgFormat`、`legs`、`systems`，`artifacts[]` 带 sha256。
6. **全局用法规则**：§2 的 `--format`、`--pkg-format`、`--message-format` 分工。

## 7. 分发：经 xlings 不改 xlings

xim-pkgindex 的包描述可以表达 openkal 程序：

- 一个资产（通用包或 okb）对应 linux、macosx、windows 三个块，URL 相同。
- `deps` 依赖 okrun（openkal-tools）；xvm 注册时把程序命令配成 `okrun <安装目录>/<name>.okb`（或通用包内的启动器）。
- 共享的 `.okso` 可作为独立的 xlings 包分发（每个架构一份，三系统通用），okrun 的搜索路径包含 xlings 的包存储，多个程序共用一份。

本批在 xim-pkgindex 的 PR 中加入一个 openkal 程序的示例与规则说明（同一 PR 还有 mcpp-ide 包）。

## 8. `.okso` 的本机绑定：openkal 体系的构建插件

- **openkal-run 增加库接口**：`okrun_open_library(path | bytes)`、`okrun_symbol(lib, name)`、`okrun_close(lib)`，以及 §4、§5 用到的 bundle 接口
  （一组内存中的映像按名字相互解析）。
- **插件 `mcpplibs.openkal-bind`**（openkal-image 仓库，与 openkal-run 同版本发布；模块 `mcpp.mcpplibs.openkal_bind`，按 SPEC-007 R9.6）：一个
  以 `.okso` 加 C 头文件发布的库，在非 openkal 目标上以构建依赖引入它；库自己的 build.mcpp 调用插件，插件用工具链的 clang 读出头文件中
  的函数原型（AST JSON），生成绑定源码——每个函数一个同名本机导出，首次调用时经 openkal-run 加载 `.okso`，经带 `sysv_abi` 的函数指针转发
  （调用约定由编译器转换）——作为本包的源文件编译。`.okso` 与 openkal-run 经 `[runtime] deploy` 放到程序旁边。消费方照常依赖并
  `#include`，无感。
- **边界由插件检查并拒绝**：只有 C 接口；边界类型不得含 `long`、`unsigned long`、`wchar_t`、`long double`（LP64 与 LLP64 不同）；资源归属
  在一侧（接口成对创建、销毁）；回调参数由插件生成反向 thunk，原型必须完整；异常与 C++ 类型不跨边界。
- mcpp 引擎不需要新机制；若实现中发现缺口（例如部署 `.okso` 的条件化），在 #847 中补齐。

## 9. 体积

本批全部实施，实测见本节末表（hello 映像；mcppls 服务器与 mcpp-ide 后端的数字随其 PR 写入 §15）。

| 处 | 做法 |
|---|---|
| 构建 | openkal-platform 增加 `-ffunction-sections -fdata-sections`（`--gc-sections` 已有，回收粒度因此到函数与数据）与 `--icf=safe`；`release` 配置可选 `-Oz`；LTO 已有 |
| 剥离 | 默认 strip；`--debug-symbols DIR` 输出分离的调试信息（已有） |
| 容器 | `--compress zstd\|gzip\|xz\|none[:level]`，`tar` 默认仍为 gzip（兼容），`.tar.zst` 可选；单文件载荷用 zstd；okb 内映像不压缩（为 mmap） |
| 去重 | 通用包与 okb 中资源只放一份，只有映像按架构；分发矩阵从"系统 × 架构"降为"架构" |
| 共享 | 共享 `.okso` 作为独立 xlings 包，一个架构装一次，多个程序共用（§7） |

**实测（2026-10-28，x86_64 Linux 主机，`import std` 的 hello，release 构建，已 strip）。**

| 项 | 0.17.1 平台 | 0.17.2 平台 | 变化 |
|---|---|---|---|
| x86_64 映像 `hello.oke` | 1 149 944 B | 1 098 872 B | −4.4%（`-ffunction-sections -fdata-sections`、`--icf=safe`） |
| 同上，`[profile.release] opt = "z"` | — | 998 928 B | 再 −9.1% |

| 容器（通用包：x86_64 与 aarch64 两组映像） | 大小 | 相对 `.tar.gz` |
|---|---|---|
| `tar`（gzip，缺省） | 787 266 B | — |
| `zip`（deflate） | 787 669 B | +0.05% |
| `tar --compress zstd`（19 级） | 594 457 B | −24.5% |
| `tar --compress xz`（6 级） | 532 196 B | −32.4% |
| `okb`（映像 stored、64 KiB 对齐，为原地映射） | 2 475 329 B | 未压缩：分发时由外层容器或传输压缩 |
| 单文件（openkal，Linux 静态载体加 x86_64 映像） | 1 164 664 B | 一个可执行文件 |

结论：分发矩阵从"系统 × 架构"降为"架构"后，再以 zstd/xz 压缩容器得到最大收益；映像本身的收益来自段级回收与 `-Oz`。
okb 不压缩是格式的选择（原地加载），其传输体积由外层（xlings 下载的压缩包或 HTTP 压缩）决定。

## 10. openkal 0.17.2 轮（本批最后）

| 包 | 版本 | 内容 |
|---|---|---|
| openkal-image（openkal-ld、openkal-run、openkal-tools，同版本） | 0.1.2 | okb v1（`docs/okb.md`）、`okld bundle`/`bundle check`、okrun 读 okb、bundle 接口、库接口；新包 `openkal-bind`（同仓同版本） |
| openkal-musl | 0.22.4 | `renameat2` 的 `RENAME_EXCHANGE` 等标志按语义实现或拒绝（Q8）；钉住 openkal-ld 0.1.2 |
| openkal-llvm-runtime | 0.16.4 | 钉住 musl 0.22.4 |
| openkal-platform | 0.17.2 | 版本组前移；体积标志；`universal = true`；okb 包格式插件；载体入口改用 bundle 接口 |
| openkal 与三个实现 | 不变 | — |

顺序：openkal-image → openkal-musl → openkal-llvm-runtime → openkal-platform → 索引 → mcpp 目标表钉住 0.17.2（#847 的最终 commit）。
openkal-platform 0.17.1 先行发布，用于 #847 开发期的 CI 与索引；0.17.2 是本批的最终钉住。os 0.2.1 的钉住不涉及 musl，dev 依赖的
llvm-runtime 随平台迁移移除；mcppls 的平台迁移后不再自己声明运行时。

## 11. 交付（在 part 1 §13 的基础上追加）

| 仓库 | PR | 追加内容 |
|---|---|---|
| mcpp | #847 | 三维模型、`--pkg-format`（旧写法提示）、通用包与启动器、`--systems`、`--merge`、单文件启动桩、通用载体字节嵌入、签名接口、压缩、`mcpp-package.json`、平台阶段的包格式指令、SPEC-014、docs/10 重写（中英）、e2e |
| mcpplibs/openkal-image | 新 PR（0.1.2） | §5、§8 |
| mcpplibs/openkal-musl | 新 PR（0.22.4） | Q8 |
| mcpplibs/openkal-llvm-runtime | 新 PR（0.16.4） | 钉住 |
| mcpplibs/openkal-platform | 首个 PR 发 0.17.1；第二个 PR 发 0.17.2 | §3 的 `universal`、§5 的插件、§9 的标志 |
| mcpplibs/mcpp-index | 同 part 1 的 PR | 0.17.2 轮条目 |
| openxlings/xim-pkgindex | 同 part 1 的 PR | §7 的示例与规则 |

openkal-platform 有两个 PR：0.17.1 是平台机制的首发（#847 开发期需要它进入索引），0.17.2 随新一轮；二者内容不同，各自对应一个
发布，不合并。

## 12. 测试

e2e：三维组合与拒绝；`--pkg-format` 与旧写法；通用包两架构布局与启动器（本机架构运行；另一架构在 qemu-user 可用时运行）；
`--merge`；原生单文件（vendored 与 static）首次解压与复用；openkal 单文件从内存运行（不写盘：缓存目录无新文件）；okb 的生成、
`okld bundle check`、`okrun app.okb`；签名步骤；`mcpp emit xpkg` 的通用资产条目；体积标志生效（`--gc-sections` 后未引用函数消失、
`--icf` 合并）。openkal-image：okb 读写、对齐、摘要、签名的单元测试；绑定插件对一个 C 库（含拒绝的类型）的 e2e，三系统 CI。

## 13. 自审

1. **一致性**：三个维度各回答一个问题，旧选项保留为简写或旧写法，用户的已有命令不变；`--pkg-format` 消除同名异义。
2. **引擎边界**：okb、载体入口、绑定插件都在 openkal 体系；引擎只提供通用机制（启动桩、字节嵌入、包格式提供者、启动器生成）。
3. **跨平台**：通用包与 okb 让 openkal 的产物数等于架构数；self-contained 的多系统加载器在一台机器上可产出（SDK 缺失时拒绝），或由
   `--merge` 合并 CI 矩阵的结果。
4. **稳定性与安全**：okb 映像对齐后 mmap，对齐不足或摘要不符时拒绝而非退回复制；单文件解压先校验载荷摘要，解到同目录的临时名再原子改名，
   按摘要寻址，不复用不完整的缓存，缓存目录只对当前用户可写。
5. **用户体验**：默认即通用；解压即用；单文件免解压；xlings 一条命令安装。
6. **可观察性**：`mcpp-package.json` 与 `mcpp.pack` 信封记录腿、系统、每个文件的摘要。
7. **规模与风险**：本批再增一轮 openkal 发布与 SPEC-014。缓解：okb 不进入核心规范，级联止于 image、musl、llvm-runtime、platform；
   openkal-platform 0.17.1 先行使 #847 的 CI 不等待新一轮。
8. **未纳入**：APE 单文件（mcpp#849）。

## 14. 定案

1. `[platform] universal` 声明其目标默认打通用包（openkal-platform 写 `true`），`--no-universal` 只打所选目标。
2. okb 由 openkal-platform 的插件作为包格式提供，引擎不认识 okb；平台阶段的指令表加入 `pack-format` 与 artifact action。
3. `tar` 默认仍为 gzip，zstd 经 `--compress zstd` 或 `[pack] compress` 选择。
4. openkal-platform 0.17.1 先行发布，0.17.2 为本批最终钉住。

## 15. 交叉验证：mcppls 与 mcpp-ide 用上 openkal 的独有特性

两个 IDE 一线的程序是本批特性的第一批真实用户：它们的服务器与后端本来就以 ok 映像分发，因此逐项采用本批的机制，
既是功能交付，也是对平台、打包与 okb 的交叉验证。二者的 PR 在 mcpp 发布与 openkal 0.17.2 轮之后完成这部分，判据写入各自 CI。

| 特性 | mcppls（#72） | mcpp-ide（0.1.0） | 验证的是 |
|---|---|---|---|
| 零声明的 `--target <arch>-openkal`（SPEC-013） | 服务器删去运行时、工具、ld 的声明 | 后端同样零声明 | 平台的层、工具、链接、封装、运行器 |
| 通用包（`universal`） | 一份 `mcppls-<v>-openkal.zip` 代替每系统 × 架构的服务器包 | 一份后端包 | 布局、启动器、`--systems` 的多系统加载器、`--merge` |
| okb | 编辑器扩展（VS Code vsix、CLion 插件、Zed）只带一个 `mcppls.okb` 与各系统 × 架构的 okrun，一个 vsix 服务全部平台 | vsix 与 mcpp-clion 的 zip 同样只带一个 `mcpp-ide.okb`，各平台一个包而不是按平台分包 | okb v1 的生成、`okld bundle check`、okrun 读 okb、mmap 与签名 |
| 单文件（openkal） | 命令行用户的 `mcppls` 单文件（每系统 × 架构，免安装） | 无（由扩展启动） | 载体从内存加载、bundle 接口 |
| xlings 单资产分发 | xim-pkgindex 的 mcppls 包改为一个资产对应三系统，依赖 okrun | 新增 mcpp-ide 包同法 | §7 的写法 |
| 宿主图的平台（SPEC-013 §6） | CI 一条以 `--host-platform openkal-platform@…` 构建（devtools 的构建程序作为映像运行） | CI 同一条（`spec/` 生成器作为映像运行） | 构建程序、其库作为映像，自举边界 |
| 体积（§9） | 服务器映像的实测基线与优化后数字写入本方案 §9 | 后端同 | 体积标志、zstd、去重 |
| 运行时版本组 | 以 openkal-platform 0.17.2 的版本组构建 | 同 | §8.1 |

判据：两个 CI 各有一条"扩展包内只有一个 okb、每个系统 × 架构的 okrun 都能启动它"的检查；vsix 与 CLion 插件 zip 的体积在本方案
记录前后对比。交付给维护者上传的产物目录（part 1 §13.3）中的 vsix 与 zip 即为采用 okb 后的版本。

### 15.1 实施中的修正与实测

- **运行器在任何机器上以平台的托管安排构建。** 每个系统的运行器与载体在其行（`<arch>-linux-musl`、`<arch>-macos`、
  `<arch>-windows-gnu`）上以 openkal-platform 的托管安排构建，Linux 上一次打包即得四个 okrun（Linux 两个架构、macOS
  arm64、Windows x64），Windows 的 okrun 不再带 VCRUNTIME140.dll。mcpp-ide 因此去掉了各系统分别打包与 `--merge` 的作业；
  mcppls 保留 Linux 与 macOS 两份的合并，以继续交叉验证 `--merge`。
- **Windows 主机上的映像字节不同（mcpp#850，上游）。** clang 在 Windows 主机上拼接包含路径时用 `\`，`-ffile-reproducible`
  对 POSIX 目标不改；libc++abi 的 `__FILE__` 因此不同，合并正确地拒绝。Linux 与 macOS 构建的映像逐字节相同，通用包在二者
  之一上打包。
- **macOS 托管安排的参数（openkal-musl 0.22.6）。** Mach-O 的构造函数顺序取决于链接载入对象的顺序；musl 的构造函数早于
  openkal-macos 记录参数时，`main` 得到 argc 0。mcppls 的 CI 测得；0.17.3 轮修复，平台的 hosted 检查断言参数。
- **mcppls 的 xlings 包仍按系统。** 其载荷带每个系统的 clangd 与语义套件，载荷中的服务器为同一个 okb 与该系统的 okrun；
  "一个资产对应三系统"适用于 mcpp-ide 的后端包与 okrun 之外的纯映像程序。

**mcpp-ide 实测（CI run 40640226458，openkal-platform 0.17.3）。**

| 产物 | 采用 okb 之前 | 0.17.2 平台 | 0.17.3 平台（运行器以托管安排构建） |
|---|---|---|---|
| vsix | 4 686 866 B | 2 965 783 B | 3 217 042 B |
| CLion 插件 zip | 4 605 498 B | 2 924 975 B | 3 172 459 B |
| `mcpp-ide.okb`（两个架构） | — | 5 731 095 B | 5 731 231 B |
| okrun linux-x86_64 / linux-aarch64 | — | 682 944 / 655 096 B | 683 024 / 655 128 B |
| okrun macos-aarch64 | — | 1 398 448 B | 1 237 392 B |
| okrun windows-x86_64 | — | 462 336 B + VCRUNTIME140.dll | 828 416 B |

扩展包由"每平台一个包"变为"一个包服务所有平台"；0.17.3 的增长来自 Windows 的 okrun 改为静态的托管安排程序。

mcpp-ide 0.1.0 发布（release run 40723046724，mcpp 2026.10.27.1）：vsix 3 233 617 B，CLion 插件 zip 3 185 790 B，
`mcpp-ide.okb` 5 775 551 B，通用包 `mcpp-ide-0.1.0-openkal.tar.gz`（自包含，四个系统的 okrun）3 022 509 B；okrun 与上表相同。

**mcppls 0.0.18 实测（CI run 40716668192，mcpp 2026.10.27.1，openkal-platform 0.17.3）。**

| 产物 | 字节 |
|---|---|
| `mcppls-0.0.18-openkal.zip`（两个映像、启动器、四个系统的 okrun） | 6 902 710 |
| `mcppls-0.0.18.okb` | 15 359 571 |
| 单文件 linux-x86_64 / linux-aarch64 | 8 374 688 / 8 217 000 |
| 单文件 macos-aarch64 / windows-x86_64.exe | 8 849 424 / 8 519 680 |

| 平台 | 载荷（发布的树） | vsix | 0.0.17 载荷 / vsix |
|---|---|---|---|
| linux-x64 | 31 023 759 | 33 490 334 | 28.1 MB / 31.1 MB |
| linux-arm64 | 29 913 830 | 32 697 810 | 26.6 MB / 30.1 MB |
| darwin-arm64 | 27 704 509 | 29 797 390 | 24.6 MB / 27.1 MB |
| win32-x64 | 37 110 754 | 39 470 558 | 34.4 MB / 37.5 MB |

载荷的树取自前一次运行（40670552556）的同一资产。mcppls 的载荷仍按系统（每个系统的 clangd 与语义套件），其中的服务器为
同一个 okb 与该系统的 okrun。载荷较 0.0.17 大约 3 MB：其中的服务器由该系统的一个程序变为两个架构映像的 okb
（15 359 571 B）与该系统的 okrun。
