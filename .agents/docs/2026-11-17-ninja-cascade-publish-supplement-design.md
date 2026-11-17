---
subject: build
status: active
---

# 第二部分之后的补充方案：带修正的 ninja、模块重编译的级联、工作区发布与本轮遗留项

**日期：** 2026-11-17 · **版本：** v4（已确认并实施；实施中测得的结果见第 10 节） · **基线：** mcpp 2026.11.17.1、openkal-platform 0.29.1、mcpp.platforms 0.1.0、
djgpp-platform 0.2.0、setup-mcpp v1.0.1、ninja 1.12.1（xim）· **前文：** [第二部分](2026-11-16-ecosystem-gap-supplement-part2-design.md)
（第 18 节记录 #945 在真实工程上的结果）

## 1. 来源与已定事项

| 来源 | 内容 | 已定 |
|---|---|---|
| #945 | 发布后的真实工程：工作区测试中 clang 读取模块时崩溃、ninja 以非零状态结束而不指名步骤；内容不变的重复运行仍有 136 个包编译 | 用户决定 A：带 ninja-build/ninja#2840 修正的 ninja 作为 xim 包，版本号带区分标记，不作为 `latest` |
| 下一版范围 | 运行器的 `hosts` 参与选择；早期平台选择可用的谓词；引擎内置行的默认 openkal-platform；`--strict` 提示 | 用户同意 |
| #946 | 一次编辑之后模块图的重编译级联：矩阵、原因 R1–R9、可优化项 O1–O8 | mcpp 侧的优化全部实施；GCC 的优化默认开启 |
| #947 | 工作区成员的发布：兄弟版本、归档名、tag、描述符形状、一次发布多个成员 | 实施 |
| 第二部分遗留 | mwrap 模板的引号键；openkal-uefi 与 openkal-dos 的三处文件系统缺口；非 Linux 宿主构建 BSD 行；illumos；外部验证工程的上游反馈 | 本文给出分析与建议，待审阅 |

## 2. #945：带修正的 ninja

### 2.1 依据

ninja 的 `Plan::EdgeFinished` 在一条边完成时可能加载 dyndep 文件；加载会向 `edge->outputs_` 与 `node->out_edges_` 追加元素，
而 `EdgeFinished` 与 `NodeFinished` 正以迭代器遍历这两个容器，迭代器因此失效（ninja-build/ninja#2662，1.12.1 与 1.13.2
均有报告，间歇出现于重建）。修正 ninja-build/ninja#2840（2026-09-26 起未合入）把两处循环改为按下标遍历，新增
`src/indexed_range.h` 与其单元测试，`build.cc` 的改动为 13 行。

mcpp 已为缓存放置单独一遍（第二部分 18 节）。按目标构建（`mcpp test`）与 `-k` 的扫描在主遍中进行；构建程序重跑后各包的
动作节点不再是最新，扫描的 dyndep 文件在主遍的完成回调中加载，即该缺陷的条件。真实工程两次运行的失败都落在导入 GPU
模块且带构建程序的一组：一次为 clang 读取模块时崩溃（导入单元在其 BMI 写完之前启动，与迭代器失效后的调度一致），
一次为 ninja 无输出而结束（崩溃或堆损坏，与原报告的 `free(): invalid pointer` 一致）。

### 2.2 包

| 项 | 决定 |
|---|---|
| 基线 | v1.12.1 加 #2840 的两个提交（回移，冲突在 `CMakeLists.txt`、`configure.py`、`src/build.cc`，均为上下文差异）。不升到 1.13：改动面只含修正 |
| 版本键 | `1.12.1-mcpp.1`。xlings 的比较器（`src/core/semver.cppm`）把 `-` 之后读作预发布：低于 `1.12.1`；`ninja@1.12.1` 为相等约束，要求预发布相同，不选中它；`ninja@1.12.1-mcpp.1` 为精确约束。xim 的 `latest` 仍指 `1.12.1`。已知限制：`>=1.12.1` 不接受它 |
| 与 `revision` 的关系 | xim-pkgindex 的规范规定：同一上游版本下载荷因补丁而变时增加 `revision`，不改版本键。`revision` 会替换所有 xim 用户的 `ninja@1.12.1` 与 `latest`，与用户决定相反；因此本包是另一来源（分叉）的另一个版本键，不是 `1.12.1` 的包装修订。规范（xpackage-v2）补一句写明此区别 |
| 二进制的自述 | `ninja --version` 输出 `1.12.1-mcpp.1`（`ninja.lua` 记录过两个不同的二进制都自称 1.12.1）；`ninja_required_version` 的解析只读前两段，不受影响 |
| 自动更新 | `ninja.lua` 的 `autoupdate` 以 `xlings-res/ninja` 的 release 为来源。合入前确认更新工具不把带预发布标记的 release 提升为 `latest`；不能保证时该 release 标为 pre-release 并在条目中排除 |
| 退出条件 | 上游发布含该修正的版本后，固定版本改为上游版本，分叉归档 |
| 构建来源 | `mcpp-community/ninja`（ninja-build/ninja 的分叉），分支 `mcpp/1.12.1`；CI 产出与现有资产形状相同的四个文件：linux-x86_64（glibc 静态）、linux-aarch64（musl 静态）、macosx-arm64、windows-x86_64；运行 ninja 自己的测试（含 `indexed_range_test`）。`xlings-res/ninja` 无构建配方：构建前核对现有四个资产的链接方式，配方写入分叉的 CI |
| 发布 | `xlings-res/ninja` 的 release `1.12.1-mcpp.1` 与 GitCode 镜像；每个资产附 `.sha256`，索引条目写明每个架构的 sha256；资产集合与 1.12.1 相同（无 macOS x86_64、无 Windows arm64） |
| xim-pkgindex | `pkgs/n/ninja.lua` 在 linux、macosx、windows 下增加 `["1.12.1-mcpp.1"]`，注释写明来源与修正；`latest` 不变 |

### 2.3 mcpp

- `pinned::kNinjaVersion` 改为 `1.12.1-mcpp.1`。已有的 home 在下一次运行时按固定版本安装它（引导时缺失即安装），无需用户操作。
- 选择顺序不变（mcpp#875）：编译器提供者旁的 ninja、home 中的固定版本、PATH。实测 llvm 23.1.3 与 gcc 的包均不带 ninja；
  文档写明：提供者自带 ninja 时使用它，且应为带修正的版本。
- **离线的 home。** 固定版本缺失而无法安装时，使用已安装的 `1.12.1` 并经 `diag::degraded()` 报告，不使构建失败。
- **不带修正的 ninja 的暴露面，与引擎侧的选择（见 2.6）。**
- **ninja 未指名步骤而失败时报告其退出状态。** `failure_diagnostics` 增加一行：`ninja exited with status N`，或
  `ninja was ended by signal S (SIGSEGV)`；Windows 上为 NTSTATUS（`0xC0000005, access violation`）。退出状态由
  `run::execute` 与 `run_ninja_reporting` 交出，结果记录（`result.json`）记入同一值。

### 2.4 第二层：内容不变的重复运行仍在编译

真实工程第二次 `mcpp test --workspace` 仍有 136 个包出现编译行。构建目录中 `build.ninja`（成员程序的规划）与
`test.ninja`（测试规划）并存，共用 `.ninja_log`：同一输出若两份清单给出的命令不同，每一次交替都使其过期。
先以 `MCPP_NINJA_DEBUG=explain` 在该工程上测出 136 个包各自的原因，再在两个修正中选其一，结果记入本文：

1. 两次规划给共有单元的命令相同（差异来自规划的输入，例如 dev 依赖的包含目录进入非测试单元）；
2. 共有单元的命令确有差异时，测试规划使用自己的目录，判定依据为共有单元命令的摘要，而不是清单层面的推断（取代 `#test=` 轴的
   规则推断）。

### 2.5 验证

- ninja：`indexed_range_test` 与 ninja 全部单元测试，四个平台。
- 因果：以 ASan 构建的 1.12.1 在真实工程上运行，取得迭代器失效的报告；同样构建的带修正版本无报告。取不到报告时，
  2.1 的因果保持为"相符"，不写为"已证实"。
- 真实工程：同一构建目录，带修正的 ninja，`mcpp test --workspace` 连续 10 次（基线 1.12.1 为 2 次中 2 次失败）：
  无 clang 读取模块崩溃，无未指名步骤的失败。#945 在此之后关闭。
- e2e：未指名步骤的失败给出退出状态（以 `kill -SEGV` 一个替身 ninja 构造）。上游无最小复现，不写依赖时序的 e2e。

### 2.6 引擎侧是否另设防线

**问题。** 缺陷只在主遍中触发：一条边完成时才加载 dyndep 文件。主遍开始前 dyndep 文件已是最新时，ninja 在依赖扫描阶段
加载它，不经过出错的代码。mcpp 的扫描遍（W2）为此把扫描提前，但有三处不覆盖（均出自 `ninja_backend.cppm` 的注释）：

| 不覆盖的情形 | 原因（现行设计） |
|---|---|
| 按目标构建（`mcpp test`、`--target`） | 扫描遍会扫描目标用不到的单元 |
| `-k` | 扫描遍在第一个失败处结束，且同一失败会报告两次 |
| 所属包带构建前动作（构建程序）的单元——**任何构建方式** | 这些扫描等待动作；提前会使所有编译排在最长的动作之后 |

#945 的两次失败都落在第三种（带构建程序的包），并处于第一种之下。带修正的 ninja 覆盖三种；不带修正的 ninja 只在以下
两处被选中：编译器提供者旁带 ninja（实测 llvm、gcc 的包均不带），或 home 中没有固定版本而取 PATH（引导会安装固定版本，
仅离线且从未安装时出现）。

| 选择 | 内容 | 覆盖 | 代价 |
|---|---|---|---|
| **A（建议）** | 不改遍的结构。所运行的 ninja 不是固定版本、且以未指名步骤的失败结束时，诊断加一行：该 ninja 不含 ninja-build/ninja#2662 的修正，固定版本为 `1.12.1-mcpp.1` | 固定版本：全部；其他 ninja：给出原因 | 无 |
| B | 扫描遍扩到按目标构建与 `-k` | 前两种；第三种仍暴露，而 #945 正是第三种 | 单目标构建多一遍扫描；`-k` 的失败报告需去重 |
| C | 另加一遍：先运行全部动作与其扫描 | 三种 | 放弃 W2 测得的并行（编译排在最长动作之后） |

v2 称扩展扫描遍后"与所选的 ninja 无关"，不成立：第三种不在扫描遍之内。建议 A：修正在 ninja 中完成，引擎不为一个已修的
外部缺陷改变遍的结构。

## 3. #946：模块重编译的级联

| 项 | 原因 | 决定 |
|---|---|---|
| O1 | GCC 的 `localtime:` 带宿主时区缩写；`looks_like_stamp` 只认 ` UTC`，非 UTC 宿主上截断不生效，整条链重编译 | **修正。** 时间戳字段按其终止符屏蔽，任意时区缩写；单元测试加非 UTC 的戳；e2e 在 `TZ=Asia/Shanghai` 下 touch 接口单元，只重编译该单元 |
| O2 | GCC 下部分行移位只改 `.gnu.c++.olm`（位置映射）、`.gnu.c++.cfg` 与时间戳，仍重编译整条链 | **实施，带守卫；默认开启以守卫的测量通过为条件。** 见 3.1 |
| O3 | GCC 以 CRC 把导入者的 BMI 绑到被导入的 BMI | 编译器侧；构建工具不能保留旧 BMI（CRC 不符为硬错误）。记录，不实施 |
| O4 | Clang 的 BMI 对非 inline 函数体、注释、私有模块片段敏感，并经贡献 BMI 的哈希传到 A | 编译器侧；`.pcm` 无可按段隔离的结构。记录，不实施 |
| O5 | 级联中重编译而字节不变的对象仍被归档与链接 | **实施。** 对象边与 BMI 边相同：保留旧文件、编译、比较，相同则放回旧文件与其时间，`restat = 1`；归档与链接因此不再运行。比较限于对象大小 ≤ 64 MiB。收益有界：被编辑单元的对象变化时（R8）链接照常进行，省下的是中间库的归档与共享库的链接；先在真实工程上计数四种编辑省下的步骤，省下的归档与链接不足以抵消每次编译的比较时不实施，结果记入本文；Windows 上与 O7 同一限制 |
| O6 | 重编译的原因不报告 | **实施。** BMI 比较的结果写入构建目录（`bmi-changes.tsv`：模块、旧/新摘要、变化的种类）；构建结束时一行：`12 units compiled again: the interface of mlibs.math changed`；仅位置变化时写 `(locations only)`。`--verbose` 下列出每个单元及其原因；不新增选项（`--explain` 已是错误码说明的旧写法） |
| O7 | Windows 上单边的 `cxx_module` 规则不比较 BMI | **先测量后实施。** 在 Windows CI 上测出排除的原因（推测为替换已映射的文件）；可替换时以 `MoveFileEx` 加重试实现同一比较，不可时在规范中写明 Windows 的例外 |
| O8 | 索引包升级时依赖它的缓存包按 Merkle 键全部重建，不按接口截断 | **不实施。** 升级必然重新链接；GCC 重建的 BMI 带新的时间戳；收益低，保守是设计（`cache_key.cppm`） |

### 3.1 O2：GCC 的位置变化不传播

- **适用范围（gcc 16.1.0 直接测量，2026-11-17）。** 行移位按位置分三种：

  | 插入一行的位置 | 变化的段 | O2 |
  |---|---|---|
  | BMI 记录的全部位置之后 | 仅时间戳 | 不需要，现行截断已生效（O1 修正后任意时区） |
  | 全部声明之前，或前置声明之后的非 inline 函数体内（#946 G1 的布局） | `.olm`、`.cfg` | **适用** |
  | 两个带函数体的声明（inline、模板、类）之间 | 其后各实体的段 | 不适用，判为接口变化 |

  声明处即定义的写法（`export int f() { … }`）属第三种，O2 对它无收益。
- **实测的反例。** `export template<class T> constexpr unsigned f(T) { return __builtin_LINE(); }` 之前插入一行：变化的段只有
  `.olm` 与 `.cfg`，而导入者编译出的常量由 3 变为 4。保留旧 BMI 时导入者得到 3，与干净构建不同——这是程序行为，不是
  调试信息。非模板的 inline 函数与类内的静态成员在解析时折叠，所在段随之变化，不受影响；#946 R3 的检查只覆盖了这一种。
- **守卫。** 模板体内未折叠的位置内建函数在 BMI 中以名字出现（实测：`__builtin_LINE`、`__builtin_source_location`）。新旧
  BMI 任一含位置读取的名字时按 `exact` 比较。**待测量（M1）：** `import std` 之下，模板体内调用
  `std::source_location::current()` 时 BMI 中出现的名字，据此确定名字表；另测合约（`-fcontracts`）的违例位置。
  名字表不能由测量确定为完备时，O2 默认关闭（`"exact"` 为默认），由用户按 profile 开启。
- **判定。** 新旧 BMI 逐段比较：除 `.gnu.c++.olm` 外各段相同，`.gnu.c++.cfg` 在屏蔽随位置变化的字段后相同（字段由实测确定，
  写入 `stage.cppm` 的注释与单元测试），且守卫通过，即判为"仅位置变化"，保留旧 BMI。导入者看到的 BMI 与 CRC 都不变。
- **代价。** 导入者中来自该模块的全部行号滞后，直到它因别的原因重编译：调试信息（含内联与模板代码的行表，影响单步与断点）、
  诊断中的行号、`-fsanitize` 与覆盖率报告中的行号。
- **键。** `bmi_cutoff = "exact" | "ignore-locations"`（值陈述比较时忽略的内容；mcpp 不计算接口摘要，故不用 `interface`），按四层取值：`MCPP_BMI_CUTOFF`、`[profile.<name>]`、`config.toml`、引擎默认。
- **切换即恢复。** 截断方式写入模块编译边的命令。切换到 `exact` 使各接口单元重编译并按 `exact` 比较，曾按位置等价保留的
  BMI 被替换，其导入者随之重编译；仅在打包时改用 `exact` 而不重跑这些边，不能修复此前留下的滞后。
- **分发的产物等于干净构建。** `mcpp pack` 在 profile 带调试信息时使用 `exact`（经上一条生效）；不带调试信息时，守卫通过的
  位置变化不改变对象字节（#946 表 4），无需区别。
- **开销与复用。** 段比较只在现行的（屏蔽时间戳的）字节比较不等之后进行，相等路径无新增开销；实现在现有的
  `mcpp bmi-equal` 步骤与 `restat` 协议之内，不新增边。O5 复用同一步骤（对象无需屏蔽）。
- Clang 不适用（O4）。

### 3.2 测试

- 单元：非 UTC 的 `localtime:`；仅 `.olm` 不同的两份 `.gcm`；`.olm` 与一个函数段同时不同的两份 `.gcm`；仅 `.olm` 不同而含
  `__builtin_LINE` 的两份 `.gcm`（按 `exact`）。
- e2e：#946 的 `cascade-matrix.sh` 的子集进入 e2e（G1 的 touch、comment-line、inline-body，GCC 与 LLVM 各一列，`TZ` 两种）；
  O5 的归档与链接不运行；O6 的原因行与 `--explain`；`bmi_cutoff = "exact"` 恢复整条链重编译；模板体读取行号的反例在默认配置下输出与干净构建相同。
- 规范：SPEC 中 BMI 截断一节写明"接口截断"的定义、GCC 的位置例外与 `exact`。

## 4. #947：工作区成员的发布

| 项 | 决定 |
|---|---|
| (1) 兄弟成员的 `path` 边不写 `version` | 发布时取兄弟成员自己的 `version`，写为 `version = "<它的版本>"`（已定，不写 `=`）（与现行拒绝消息给出的写法相同）；`--strict` 仍拒绝。写入的是已发布清单中的要求，消费者据此在索引中解析兄弟包，因此兄弟包须同批发布（(5) 保证） |
| (2) 归档名与 URL | 带命名空间的包用完整身份：`mlibs.math-0.1.0.tar.gz`；无命名空间的包不变；归档内的顶层目录同名。已发布的描述符写有显式 URL，按 `*/mcpp.toml` 定位清单，不受影响 |
| (3) tag | `mcpp publish --tag <模板>`，模板可含 `{version}`、`{name}`、`{namespace}`，默认 `v{version}`。清单中不设对应的键：现无 `[package.publish]` 表，为一个值新增一张表不相称；出现需求时再定 |
| (4) 描述符形状 | 生成现行形状：文件 `<ns>.<name>.lua`，`namespace` 与短 `name`；`mcpp xpkg parse` 与索引的 lint 接受，旧形状仍可解析 |
| (5) 一次发布多个成员 | `mcpp publish --workspace` 与 `-p <成员>…`：按依赖顺序打包，一个 tag，归档旁写 `SHA256SUMS`，每个成员一个描述符；`--dry-run` 列出全部。tag 模板对每个成员求值：结果相同的成员归入同一 tag，各 tag 一份 `SHA256SUMS`；同一 tag 下归档名相同即拒绝。mcpp 只生成归档、校验和与描述符，不创建 release |

测试：e2e 以三个成员（两个共用短名 `core`）的工作区覆盖 (1)–(5)；文档 docs/11（中英）。

## 5. 下一版的其他项（已同意）

| 项 | 内容 |
|---|---|
| 运行器的 `hosts` | 当前宿主不在运行器的 `hosts` 中时，不采用该运行器：行是本机能直接执行的系统（例如 Windows 宿主上的 `x86_64-windows-musl`）则直接运行；否则拒绝为不可达，消息列出 `hosts`。"能直接执行"取自行与宿主的数据（系统相同且架构可达），即行不带运行器时的现行判定，引擎不写系统名。现状：2026.11.17.1 只按架构判定可达，Windows 宿主上要求启用 Wine（外部验证工程的 Windows 作业因此失败）。e2e 覆盖两种宿主 |
| 早期平台选择的谓词 | SPEC-013 §3.1 写明：平台选定之前可用的是三元组的字面成分（`arch`、`os`、`env`）与族；`c-abi` 等由平台陈述的谓词在此时不成立。`platform` 写在含此类谓词的选择器下时给出警告，并建议 `env = "musl"` 等写法 |
| 内置行的默认 openkal-platform | 引擎内置表的默认改为 0.29.1（生成器读取的固定版本） |
| `--strict` 的提示 | 示例写出被拒绝的键本身，而不是固定的 `mcpplibs.cmdline = "0.0.2"` |

## 6. 第二部分的遗留项

### 6.1 mwrap 模板的引号键

onnxruntime-m 1.25.1、opencv-m 与 ffmpeg-m 的模板写 `"{{self.name}}" = …`，渲染为带引号的点分键（自 mcpp 2026.11.8.1 弃用，
2027.2 移除）。建议：三个包各发包装修订（`1.25.1.1` 等，沿用 recastnavigation 1.6.0.2 的修订形式），模板写
`{{template.package.selector}} = "{{template.package.version}}"`；mcpp-index 的条目与消费者测试指向修订。检查脚本中
"引号键只允许该弃用警告"的例外在三个修订发布后不再触发，保留到 2027.2 作为对旧模板的保护，之后删除。

### 6.2 openkal-uefi 与 openkal-dos 的文件系统缺口

| 缺口 | 位置 | 建议的修正 |
|---|---|---|
| UEFI：改名被拒（`EFI_ACCESS_DENIED`） | openkal-uefi 以 `SetInfo` 的新绝对名改名；OVMF 的 FAT 驱动只接受同一目录内的名字 | 同目录改名写文件名本身；跨目录改名以复制加删除实现，并陈述其非原子 |
| UEFI：列目录的名字不带扩展名（`f1.`） | openkal-uefi 的目录项转换 | 修正名字的转换（`EFI_FILE_INFO.FileName` 的完整 UTF-16 名）；测试含扩展名与长名 |
| DOS：子目录的列举为空 | openkal-dos 的 `findfirst`/`findnext` 路径模式 | 子目录以 `<dir>\*.*` 列举并跳过 `.`、`..`；测试嵌套目录 |

上表"位置"一列是由现象推得的假设，未在实现仓库中复现；openkal-dos 的列举已使用 `*.*`（`src/fs.cpp`），该项的原因尚未确定。
每项先在所属仓库以测试复现，再定修正。

建议本轮即在 openkal-uefi 与 openkal-dos 各开一个 issue（speak-agent）并修正，随 openkal-platform 的版本组补丁发布；
外部验证工程随后在这两行上运行完整脚本。按"环境收敛到 openkal"的约定，实验性部分标注而不推迟。

### 6.3 非 Linux 宿主构建 BSD 行；illumos

原则：交叉构建由行决定，不由宿主决定。测得的五处宿主约定进入行，均属引擎：

| 宿主 | 现象 | 修正 |
|---|---|---|
| macOS | Apple SDK 的头文件进入 FreeBSD 行的 std 模块编译 | 目标系统不是宿主系统时不传 `-isysroot`/`SDKROOT` |
| macOS | libc++ 层按宿主的 ctype 编译（NetBSD，`_CTYPE_BL`） | `llvm.libcxx` 的配置取目标 sysroot，宿主宏不进入 |
| macOS | Mach-O 链接器收到 ELF 选项（OpenBSD，`--disable-new-dtags`） | 链接器的风格由行的对象格式决定（ELF → `ld.lld`） |
| Windows | sysroot 的头文件打不开（NetBSD） | 解压时的符号链接在 Windows 上落为文件或以 junction 实现；测量后定 |
| Windows | crt 对象找不到，程序名带 `.exe`（FreeBSD） | crt 的查找与可执行文件后缀取自行（`exe-suffix`），不取自宿主 |

修正后 mcpp-platforms 的 CI 加入 macOS 与 Windows 宿主。illumos 维持排除：clang 的 Solaris 驱动要求 GCC 的 crt 与 Solaris 布局的
sysroot，需单独的平台包与驱动参数，另立条目。

### 6.4 外部验证工程

验证只在其 fork 上进行：新 mcpp 发布后 fork 的全部作业通过，UEFI 与 DOS 两行运行完整脚本。不向上游反馈（已定）；上游不受
任何影响。

## 7. 仓库与 PR（每个仓库一个 PR）

| 顺序 | 仓库 | 内容 |
|---|---|---|
| 1 | mcpp-community/ninja（新分叉） | `mcpp/1.12.1` 分支、CI、release `v1.12.1-mcpp.1` |
| 2 | xlings-res/ninja + GitCode | 资产 `1.12.1-mcpp.1` |
| 3 | xim-pkgindex | ninja `1.12.1-mcpp.1`（非 latest） |
| 4 | mcpp | 2.3、2.4、第 3、4、5 节；规范与文档（中英）；带版本号发布 |
| 5 | openkal-uefi、openkal-dos、openkal-platform | 6.2 与版本组补丁（另含：openkal-platform 的 CI 加入 Windows 宿主运行 `x86_64-windows-musl`） |
| 6 | onnxruntime-m、opencv-m、ffmpeg-m、mcpp-index | 6.1；mcpp-index 的 CI 固定版本随 mcpp 发布 |
| 7 | mcpp-platforms | 6.3 修正发布后 CI 加入 macOS、Windows 宿主 |
| 8 | 外部验证工程（仅 fork） | 新 mcpp 下全部作业通过（6.4） |

一轮完成，每个仓库一个 PR（已定）；提交是检查点：每个提交单独可构建、通过单元测试与它涉及的 e2e，可单独回退。
mcpp 的提交顺序（前面的解除外部阻塞，后面的各自独立）：

| # | 提交 | 先决 |
|---|---|---|
| 1 | 固定 ninja `1.12.1-mcpp.1`、离线回退、退出状态与 2.6-A 的诊断 | 顺序 1–3 已发布 |
| 2 | O1 | — |
| 3 | 运行器的 `hosts`；早期选择的警告；内置默认 0.29.1；`--strict` 提示 | — |
| 4 | 2.4：测量结果与所选修正 | 真实工程的 explain |
| 5 | O6 | — |
| 6 | O2（含守卫） | M1 |
| 7 | O5、O7 | 各自的测量；不达标则本提交只含规范中的陈述 |
| 8 | #947 (1)–(5) | — |
| 9 | 6.3 的五项，每项一个提交 | macOS、Windows 宿主的 CI |
| 10 | 规范、文档（中英）、CHANGELOG、版本号 | — |

完整 CI 在提交 3、8、10 之后各运行一次；某项的测量不通过时回退该提交，不阻塞发布。openkal 各仓库、三个包装仓库、
mcpp-index、mcpp-platforms、fork 各一个 PR。

## 8. 待审阅的决定点

已定（2026-11-17）：ninja 的基线、版本键与分叉；O2 以守卫的测量为条件；O5 先计数；兄弟版本写 `"<版本>"`；每个仓库一个
PR、提交为检查点、一轮完成；6.2 与 6.3 进入本轮；外部验证工程只在 fork。

待定：

1. **2.6：** A、B、C 之一（建议 A）。
2. **`bmi_cutoff` 的值名：** `"exact" | "ignore-locations"`（建议），或 `"exact" | "interface"`。
3. **O6 不新增选项**（汇总一行、文件、`--verbose`），或另设 `--rebuild-reasons`。
4. **#947 (3) 只设 `--tag`**，清单中不设键。
5. **2.4：** 测量后在两个修正中选择，结果先报告再实施。

## 9. 修订依据（v1→v2→v3）

| 视角 | v1 的问题 | 依据 | 修订 |
|---|---|---|---|
| 正确性 | O2 称"读取行号的函数体所在段随之变化，不会误判" | gcc 16.1.0：模板体内的 `__builtin_LINE()` 只改 `.olm`、`.cfg`，导入者的常量 3→4 | 3.1：守卫、M1、默认以测量为条件 |
| 正确性 | O2 的收益范围未陈述 | 三种位置的段比较 | 3.1 的表 |
| 正确性 | `pack` 改用 `exact` 即等于干净构建 | 此前保留的 BMI 不因切换而失效 | 3.1：截断方式进入命令 |
| 一致性 | 版本键与索引规范的 `revision` 规则冲突而未说明 | xpackage-v2："a published version key is never renamed" | 2.2 |
| 稳定性 | 只修 ninja；提供者旁与 PATH 上的 ninja 仍有缺陷 | 选择顺序（#875）；扫描遍的限定条件 | 2.3：扫描遍覆盖按目标构建 |
| 稳定性 | 间歇缺陷以两次运行验证；因果为推断 | 基线 2/2 失败 | 2.5：ASan 取证、10 次 |
| 兼容 | 自动更新可能把新 release 提升为 `latest`；离线的 home | `ninja.lua` 的 `autoupdate` | 2.2、2.3 |
| 体验 | `--explain` 与既有写法冲突 | `cli.cppm` | `--rebuild-reasons` |
| 架构 | `bmi_cutoff` 缺机器层；"直接运行"的判定未说明取自数据 | AGENTS 第 2 节 | 3.1、第 5 节 |
| 完整性 | #947 (5) 成员版本不同时"一个 tag"无定义 | — | 第 4 节 |
| 事实 | 6.2 的原因写成已确定 | openkal-dos 已用 `*.*` | 6.2 标为假设，先复现 |
| 范围 | O5 的收益未评估；一个 PR 二十余项 | #946 R8 | O5 先计数；第 7 节分两轮 |
| 正确性（v3） | v2 称扩展扫描遍后"与所选的 ninja 无关" | 带动作的包的扫描不在扫描遍内（`ninja_backend.cppm`），#945 的失败正在其中 | 2.6：三种选择，建议 A |
| 语义（v3） | `"interface"` 暗示接口摘要 | mcpp 不计算接口摘要（#946 2.2） | 值名 `ignore-locations` |
| 简洁（v3） | O6 新增选项；`[package.publish]` 新增一张表 | 现有 `--verbose`；现无该表 | 不新增 |
| 性能、复用（v3） | O2、O5 的开销与落点未陈述 | `mcpp bmi-equal` 与 `restat` 已存在 | 3.1 |
| 范围（v3） | 两轮 | 用户决定：单 PR、提交为检查点 | 第 7 节 |
| 跨平台（v3） | 分叉 CI 的四个资产如何与现有资产同形状未陈述 | `xlings-res/ninja` 只有 README，无构建配方；现有 linux-x86_64 为静态 ELF | 2.2：构建前以 `file` 与大小核对现有四个资产，配方写入分叉的 CI |

## 10. 实施记录（v4，2026-11-17）

实施与方案不同之处，以及实施中测得的事实。

### 10.1 ninja

- **补丁为 #2840 的第一个提交。** 该提交把两处循环改为按下标遍历（`build.cc` 8 行增、6 行删），在 v1.12.1 上无冲突；第二个
  提交只把同样的循环改写为一个范围辅助类，不改变行为，未取。分叉 `mcpp-community/ninja`，分支 `mcpp/1.12.1`，CI 在四个宿主上
  运行 ninja 自己的测试并产出与 1.12.1 同形状的资产（实测现有资产：x86_64 为静态 glibc，aarch64 为静态 musl，macOS 为两种架构
  合一的 Mach-O，Windows 为 MSVC）。
- **更新工具排除预发布。** `xpkgindex-update` 跳过标为 draft 或 prerelease 的 release，`xlings-res/ninja` 的该 release 标为
  prerelease。索引的校验要求新版本写明每个架构的 `url` 与 `sha256`，条目按此写出。
- **"已有的 home 下次运行时安装固定版本"在 2026.11.17.1 上不成立。** `ensure_ninja` 在任一版本的 ninja 存在时即返回；`ninja_program_for`
  取目录遍历给出的第一个版本，顺序未定义。实施：引导按固定版本判断，缺失时安装；构建在同一存放处优先取固定版本。索引尚未列出
  固定版本时（镜像滞后、缓存的索引）新 home 安装 1.12.1 并说明其缺少的修正；已有的 home 保留原有版本，一天之后再试。

### 10.2 `mcpp.bmi`

截断的实现从 `src/build/stage.cppm` 移出，成为独立的包 `modules/bmi`：

| 模块 | 内容 |
|---|---|
| `mcpp.bmi` | 与编译器无关：两个 BMI 的差别（按格式分派，未知格式按字节）、一条边的提问 `keeps_previous`、构建目录的取值与记录、运行结束时的说明 |
| `mcpp.bmi.gcc` | GCC 的 `.gcm`：两个时间戳、行映射段与配置段的两个校验和 |
| `mcpp.bmi.difference` | 两者共用的词汇（相同、仅位置、变化） |

新增一种格式的读者即新增一个模块与分派中的一行。三种调度（未拆分的规则、拆分 BMI 的两种）经同一提问比较，因此取值不写入
命令，而写入构建目录的 `bmi-cutoff`；切换到 `exact` 时移除 `bmi-kept.list` 所列的 BMI，其边与导入者随之运行。v3 的"取值写入
编译命令"由此取代：不必使每个接口单元重新编译。

- **M1 通过。** `import std` 之下模板体内的 `std::source_location::current()` 在 BMI 中留下 `__builtin_source_location`；守卫的
  名字表为 `__builtin_LINE` 与 `__builtin_source_location`。以 `-fcontracts` 编译的图按确切内容比较。默认值为 `ignore-locations`。
- **`.gnu.c++.cfg` 中随位置变化的是第 0–3 字节（该段的校验和）与第 8–11 字节（模块的校验和）**，其余字节相同时才判为仅位置。
- **取值的层次没有机器层。** profile 的键（`ndebug`、`trim_paths`）在 `config.toml` 中均无对应；`bmi_cutoff` 与之一致：环境、
  profile、引擎默认。
- **O5 不实施。** 被编辑单元的对象在带调试信息的 profile 下随行号变化；不带调试信息时只有不改变代码的编辑（注释）使对象不变。
  O1、O2 之后 GCC 下这类编辑不再到达导入者，剩余的收益是一次归档与链接，代价是每次编译多一次对象的保留与比较。
- **O7 未在 Windows 上测量。** 未拆分的规则在 Windows 上不比较 BMI，规范（SPEC-018 §1）写明这一例外；每条编译命令已经由
  mcpp 的执行器启动，比较可以移入该执行器而不依赖 shell，这是它的落点。

### 10.3 文件系统缺口的原因

v3 把三处缺口的原因标为假设。复现后两处的原因都不在所猜的位置，第三处是同一原因：

| 现象 | 原因 | 修正 |
|---|---|---|
| UEFI：列目录给出 `f2.`，改名被固件拒绝 | 仅发布档。openkal-uefi 的源码以 `x86_64-unknown-uefi` 编译，`wchar_t` 为 16 位；优化器把测量 UTF-16 名字长度的循环替换为 `wcslen`，链接到 C 库中 32 位 `wchar_t` 的 `wcslen`，长度有误。列目录按该长度转换，改名把该长度的名字写入 `EFI_FILE_INFO` | openkal-uefi 0.11.2：`-fno-builtin`（openkal-windows 已因同一原因如此）；C 库示例在两种 profile 下运行 |
| DOS：子目录的列举为空 | openkal-musl 的 `chdir` 不释放此前工作目录的句柄。i686 Linux 上一个 20 行的脚本留下 20 个未关闭的目录句柄；openkal-dos 有 32 个目录，其后 `opendir` 失败 | openkal-musl 0.29.9 |
| DOS：`opendir("/okd")`、`chdir("/")` 失败 | 提供的目录都以卷命名，没有名为 `/` 的目录 | openkal-musl 0.29.9：以分隔符开头的名字是当前目录所在卷的根 |

openkal-platform 0.29.2 采用这两个版本；UEFI 与 DOS 的检查程序创建目录、写入、改名、列举并删除文件。

### 10.4 #947

`mcpp publish` 没有 `--strict`，(1) 中"`--strict` 仍拒绝"不适用；兄弟成员的清单没有版本时仍拒绝。成员的 tag 不同时各有
`SHA256SUMS-<tag>`。描述符的文件名与索引一致：`mcpplibs` 中为 `<name>.lua`，其他命名空间为 `<namespace>.<name>.lua`。

### 10.5 非 Linux 宿主构建 BSD 行（6.3）：原因重新归属，未在本轮完成

逐条读出 mcpp-platforms 0.1.0 在 macOS 与 Windows 宿主上的失败记录后，五处的归属与 6.3 的表不同：

| 宿主 | 记录 | 归属 |
|---|---|---|
| Windows，NetBSD | `sys/cdefs.h:76: cannot open file …/machine/cdefs.h` | sysroot 中的 `machine` 是符号链接；归档在 Windows 上解包后它不是目录。属于包的解包（xlings），不是引擎 |
| Windows，FreeBSD | `ld.lld: unable to find library -lgcc`、`-lgcc_s` | FreeBSD 的 `libgcc.a` 是指向 `libcompiler_rt.a` 的符号链接，`libgcc_s.so` 同理：与上一条同一原因。6.3 所记的"crt 找不到、程序名带 `.exe`"不是这条记录的内容 |
| macOS，OpenBSD | `ld64.lld: unknown argument '--disable-new-dtags'` | 引擎：链接经由 Mach-O 的链接器 |
| macOS，NetBSD | libc++ 的 `__locale`：`_CTYPE_BL` 未声明 | 引擎或 `llvm.libcxx` 的构建：C++ 层按宿主的配置编译 |
| macOS，FreeBSD | std 模块的预编译读到 Apple SDK 的头文件 | 引擎：SDK 进入了非 Apple 的行 |

Windows 的两条是一处修正，位置在解包（符号链接在 Windows 上以复制落地），需要 xlings 的一次发布；macOS 的三条在引擎，每条都需要
在 macOS 宿主上逐次测量。两者都不能在没有该宿主的机器上完成，本轮没有做；mcpp-platforms 的 CI 仍只含 Linux 宿主，其文档
写明宿主的范围。这是本方案中唯一没有落地的已定条目。

