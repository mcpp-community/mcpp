# SPEC-006:工具链管理

| 项 | 值 |
|---|---|
| 规范编号 | SPEC-006 |
| 标题 | 工具链管理:身份、来源、选择与载荷契约 |
| 状态 | 草案 v0.7 |
| 最后修改 | 2026-10-09 |
| 对应实现 | 逐条标注;标为「已实现」的条款对应 mcpp >= 2026.9.24.1。标为「未实现」的条款计划与下一批 LLVM 工具链一同落地,届时按实测修订本规范 |
| 相关设计文档 | `.agents/docs/2026-09-24-toolchain-selection-and-payload-trust-design.md`、`.agents/docs/2026-09-24-685-687-msvc-stl-and-toolchain-payloads.md`、`.agents/docs/2026-09-28-ecosystem-design-and-optimisation-plan.md`、`.agents/docs/2026-10-02-pr-ci-acceleration-and-the-toolchain-specification-design.md` |
| 相关 issue | mcpp#685、mcpp#687、mcpp#718、mcpp#755 |
| 使用文档 | [docs/20 - 工具链](../zh/20-toolchains.md)、[docs/32 - 编写载荷](../zh/32-authoring-a-payload.md)、[docs/91 - 工具链内部](../zh/91-toolchain-internals.md) |

本规范定义 mcpp 对工具链的命名、选择和使用方式,以及一个工具链载荷在发布前必须满足的条件。
目标侧的层模型见 [SPEC-002](target-side.md);清单的平面划分与解析轴见 [SPEC-004](manifest-semantics.md)。

用语按 RFC 2119:**必须 / 禁止**(强制)、**应当**(强烈建议)、**可以**(可选)。

---

## 1. 术语

| 术语 | 含义 |
|---|---|
| 工具链 | 执行编译的程序及其随附部分:编译器驱动、编译器运行时(builtins 与展开器)、随编译器发布的 C++ 标准库、汇编器、链接器与归档工具 |
| 载荷 | 由 xlings 从索引安装的预构建目录树,身份为命名空间、名字与版本 |
| 来源 | 工具链或 sysroot 的出处:`managed`(生态包)或 `system`(在本机定位) |
| sysroot | 编译时所针对的目标环境。Linux 目标上是 C 库载荷;MSVC ABI 目标上是 MSVC toolset(STL、vcruntime、CRT)及其 Windows SDK |
| 构建环境 | 产出一个载荷的机器或容器,以及其中的 C 库 |

---

## 2. 身份与写法

### 2.1 基本写法 已实现

工具链**必须**写作 `<族>@<版本>`,族为 `gcc`、`llvm`、`msvc`、`emsdk`、`android-ndk`。
部分版本**必须**解析为匹配的最高版本。

### 2.2 系统来源 已实现

只有 `msvc` 有系统来源,写作 `msvc@system`。其他族写 `@system`,**必须**在读取处被拒绝。
不带族的 `system`(PATH 上的编译器)**必须**被拒绝,拒绝信息给出可用的写法。

### 2.2.1 按路径命名 已实现

工具链**可以**写成一个表,由路径命名本机已有的一份:
`[toolchain] <键> = { path = "<目录>", prefix, sysroot, family, launcher, tools }`,
或 `MCPP_TOOLCHAIN=path:<目录>`。`<目录>/bin` 中的驱动决定族(`<prefix>clang++` 为 llvm,
`<prefix>g++` 为 gcc);其余属性由探测该驱动得到。

这与 §2.2 拒绝 `system` 并不矛盾:`system` 是「`PATH` 上碰巧有什么就用什么」,而本条是
一次被命名、被识别、被记录的选择,与 `msvc@system` 同形。实现**必须**:

- 以与托管载荷相同的 link model、hermetic 检查与 `import std` 能力判定驱动它;
- **禁止**写入该目录树(它不是 mcpp 管理的);
- 把驱动与 `tools` 所列每个程序的身份(路径、大小、修改时间)纳入指纹与快速路径的判定,
  因为这样的工具链可以原地改变;
- 把它们记在该次构建的产物旁,使快速路径在其中之一改变后让行;
- 在构建输出、`resolution.json` 与 `mcpp why toolchain` 中陈述其来源(§3.3)。

`[toolchain] bootstrap = "<族>@<版本>"` 命名编译并运行构建程序的工具链;未写时按 §3 的
规则决定。`[toolchain] <键> = { configure = "build.mcpp" }` 把构建工具链的选择交给根构建
程序的工具链阶段(SPEC-007 R9.9)。

**状态:已实现**(mcpp 2026.10.1.3,mcpp#755)。

### 2.3 生态包前缀 已实现

`xim:<族>@<版本>` 表示只取生态包。对没有系统来源的族,它与不带前缀的写法等价,规范写法去掉前缀;
对 msvc,前缀改变含义,规范写法保留前缀。`xim:msvc@system` 与 `xim:` 之外的命名空间**必须**被拒绝。

### 2.4 带版本的 msvc 写法 已实现

`msvc@<版本>` **必须**先在本机已安装的 toolset 中匹配,匹配不到再取生态包;规则见 §3.4。
`mcpp toolchain default msvc@<版本>` 在本机已有该 toolset 时**必须**直接接受,不要求先安装生态包。

### 2.5 版本匹配 部分实现

前缀**必须**按版本分量匹配:`14.4` 匹配 `14.4.x`,不匹配 `14.44`。「最高」**必须**按数字元组比较。

当前:本机 MSVC toolset 的匹配与比较按本条实现;其他族的前缀匹配方式未逐一核对。

---

## 3. 来源与选择

### 3.1 一次选择 已实现

一次构建中,工具链及其 sysroot **必须**只解析一次;编译、依赖扫描、标准库模块、链接与缓存键**必须**读取同一个结果。
clang 以 MSVC ABI 为目标时,所选 toolset 与 SDK 以 `-Xmicrosoft-visualc-tools-root`、`-Xmicrosoft-windows-sdk-root`、
`-Xmicrosoft-windows-sdk-version` 显式交给编译器驱动,`std.ixx` 取自同一个 toolset。

### 3.2 声明优先于探测 部分实现

项目写明的版本**必须**优先于环境变量与本机扫描;被忽略的环境变量**必须**以一行说明报告。

| 条目 | 状态 |
|---|---|
| 受管 MSVC 忽略 `WindowsSdkDir` 并报告 | 已实现 |
| `VSINSTALLDIR` 优先于 vswhere | 已实现 |
| 写明版本时忽略 `VCToolsInstallDir` 与 `VSINSTALLDIR`,并报告 | 已实现 |

### 3.3 结果可见 已实现

凡由探测得到的选择,其来源、版本与 SDK **必须**打印在构建输出中,写入 `resolution.json`,并进入缓存键。

不是生态缺省的来源(按路径命名的工具链、被覆盖的载荷、使用者指定的工具、宿主上找到的
程序)**必须**各自以一行陈述,写出它是什么、从哪里来、以及陈述它的位置;`Finished` 行
**必须**汇总它们;`resolution.json` 的 `sources` 记录每一条。全部来源都是生态缺省时,输出
**必须**与没有本机制时相同。`--managed-only`(或 `MCPP_MANAGED_ONLY`)**必须**拒绝任何
非缺省来源,并逐条点名(mcpp#755)。
MSVC ABI 目标上:SDK 以 `ucrt@<版本>` 进入运行时身份;clang 行的 toolset 与 SDK 写入 `resolution.json` 的
`msvc_toolset` 与 `windows_sdk`,toolset 目录与 SDK 版本进入缓存键,`stdlibVersion` 记为 toolset 的版本。

### 3.4 MSVC 的候选与顺序 已实现

本机候选**必须**覆盖全部 VS 实例(含预发布版)及每个实例下的全部 toolset。

`msvc@system` **必须**按以下顺序取第一个完整的候选:

1. `VCToolsInstallDir` 所指的 toolset;
2. `VSINSTALLDIR` 或 `VCINSTALLDIR` 所指实例的默认 toolset;
3. `PATH` 上 `cl.exe` 所在的 toolset;
4. 带 C++ 组件、且安装版本最高的实例的默认 toolset;
5. 没有 vswhere 时,扫描固定路径得到的候选。

带版本的写法**必须**依次尝试本机候选、已安装的生态包、索引;三者都没有时,**必须**报错并列出三类候选。

「完整」指本次构建所需的文件都在:库目录;需要 `import std` 时的 `modules/std.ixx`;`cl.exe` 行的 `cl.exe`。
不完整的候选**必须**被跳过,并报告。

`cl.exe` 行与 clang 行使用同一个选择。

### 3.5 MSVC ABI 目标的 sysroot 已实现

在 `*-windows-msvc` 目标上,编译器是工具链,MSVC toolset 是 sysroot。

- `[target.<三元组>].sysroot` 在这些行上**可以**写作 `msvc@system`、`msvc@<版本>` 或 `xim:msvc@<版本>`,含义同 §2。
- 未写时**必须**等同于 `msvc@system`。
- 编译器为 `cl.exe` 时,sysroot **必须**是该编译器所属的 toolset;另写一个不同的 sysroot,**必须**被拒绝。
- SDK **必须**跟随 toolset 的来源:生态包 toolset 用随其安装的 `windows-sdk` 载荷,本机 toolset 用本机扫描的结果。
- 目标侧报告([SPEC-002](target-side.md))中,c-abi 与 c++ 两层记为预制来源;具体的 toolset 与 SDK 记入 `resolution.json`(§3.3)。
- 该值**禁止**作为 C 库包进入项目环境的安装集合。

### 3.6 目标属性按目标判定 已实现

描述产物的属性(最低系统版本、三元组中的版本段)**必须**按目标判定,与宿主无关;
只有在宿主上执行的编译(build.mcpp)按宿主判定。macOS 的 deployment target 在任何宿主上都按目标解析与施加。

### 3.7 MSVC ABI 目标的 CRT 模型 已实现

在 `*-windows-msvc` 目标上,CRT 模型(静态或动态)是目标 ABI 的属性,而非某一个编译器的属性:
`cl.exe` 与以该 ABI 为目标的 clang 行**必须**接收同一个模型,分别以各自驱动的拼写(`/MT`/`/MD`,
`-fms-runtime-lib=static`/`=dll`)发给编译单元、`std`/`std.compat` BMI 与链接命令。

- 未声明的契约在该 ABI 上,对每个角色都**必须**解析为 `toolchain-coupled`:动态 CRT,并将所选
  toolset 自带的 `vcruntime140.dll`/`msvcp140.dll` 等文件置于产物旁。
- `self-contained`,或 `linkage = "static"`,**必须**解析为静态 CRT。
- `host-coupled` **必须**解析为动态 CRT,且不放置文件。
- 所选 toolset 不带 `VC\Redist\MSVC\<版本>\<架构>\Microsoft.VC*.CRT` 目录时,未声明的契约**必须**
  静默解析为 `host-coupled`;显式声明的 `toolchain-coupled` **必须**被拒绝,并指出缺失的目录——
  这是行的一个属性,不因某一次构建而降级。
- `[build] cxxflags` 或 `dialect_cxxflags` 中出现的自由拼写 CRT 词(`/MT`、`/MD`、`-fms-runtime-lib=*`
  等)与已解析的模型一致时**应当**被警告为冗余;不一致时**必须**被拒绝,消息**必须**指出该词、
  所在的键与该词对应的值。依赖包的 `[build] cxxflags` 同样检查,因为它们作用于该包自己的单元:
  不一致时**必须**被拒绝并指出该包;一致时不警告,因为替代它的键 `cxx_runtime` 只属于根。
  调试 CRT 词(`/MTd`、`/MDd`、`-fms-runtime-lib=*_dbg`)**必须**被拒绝:模型不表达调试 CRT,
  标准库模块与链接使用发布版 CRT。

#### 3.7.1 程序旁的文件由一个解析器决定 已实现

PE 程序旁的每个名字,其字节来自哪里,**必须**由一个解析器回答(`mcpp.build.runtime_placement`)。
构建的放置边、链接后的 `place-dlls` 边、`mcpp run`/`mcpp test` 携带的文件与 `mcpp pack` 都读取
这一个答案,**禁止**各自再决定。

- **候选分三类。** 声明:`[runtime] deploy_files` 与插件的 `deploy`(SPEC-007 R4.2)。工具链:
  所选 toolset 的 `Microsoft.VC*.CRT` 目录中的文件。推导:在运行时搜索目录中找到的 DLL。
  声明优先于工具链,工具链优先于推导。
- **MSVC C++ 运行时是一个带版本的集合。** 同一集合的文件**禁止**跨版本混用,集合整体选择:
  默认取 toolset 的集合;某个推导目录中的集合完整(含 toolset 集合的每一个名字)且其
  `VERSIONINFO` 文件版本严格更新时,取该集合,并说明一次。集合的版本取其成员中最旧的一个。
- **契约决定种类,而不只是次序。** host-coupled 下,**禁止**从任何来源放置运行时文件:声明的
  运行时文件**必须**在编译前被拒绝(`crt-declared-under-host-coupled`),推导目录中的运行时文件
  被丢弃并说明一次。self-contained 下程序不导入运行时;依赖带来运行时的名字时,按上一条的集合
  规则放置。
- **声明的运行时文件优先于集合选择,并与 toolset 的运行时版本比较。** 更旧时**必须**给出警告,
  点名两个版本。在这一下限的读数被证实可靠之前,它是警告而不是拒绝:可执行映像的
  `MajorLinkerVersion.MinorLinkerVersion` 由 lld-link 写为 14.0,不能回答「哪个 toolset 构建了
  这个映像」。
- **读不出的版本不作决定。** 某个候选的 `VERSIONINFO` 读不出时,按种类次序决定,并说明一次;
  **禁止**把缺失的版本当作更旧或更新比较。
- **依赖目录携带运行时文件是打包缺陷。** 它**必须**被说明一次,**禁止**成为静默的来源
  (xim-pkgindex 的配方规则见其文档)。
- **规划之后才出现的名字用同一规则。** `prepare` 在构建中填充的目录里的运行时名字,由链接后
  的放置以同一个解析器决定;`place-dlls` 以 `--crt <规则>` 与 `--toolset-crt <目录>` 接收规划
  采用的规则。`mcpp pack` 在不携带运行时的模式下把运行时的名字当作宿主提供,不论哪个目录提供
  了副本。
- **构建期的工具与程序使用同一个运行时。** 为 MSVC ABI 目标运行的每个 action,`PATH` 的首位
  **必须**是 toolset 的运行时目录。系统目录在加载器的搜索中先于 `PATH`,所以装有 VC++
  redistributable 的机器使用系统的运行时;`PATH` 在系统没有时提供它。
- **决定被记录。** `resolution.json` 的 `runtime.placement`(每个目的地、来源与种类)、
  `runtime.crt_set`(规则、来源种类与版本)与 `runtime.placement_notes`。

MinGW 的运行时(`libstdc++-6.dll`、`libgcc_s_seh-1.dll`、`libwinpthread-1.dll`)按种类次序决定,
没有版本规则:这些 DLL 不携带可靠的 `VERSIONINFO`,也没有工具链候选为它们放置。

---

### 3.8 系统头文件不取自宿主 已实现

使用受管 C 库的构建**禁止**隐式搜索宿主的系统头文件目录。clang 在受管 glibc 上**必须**携带 `-nostdlibinc`;
GCC 以 xlings subos 为 `--sysroot`,没有可用 subos 而退回载荷布局时,**必须**以 `-isysroot <C 库载荷>`
取代编译器构建时记录的 sysroot。需要宿主头文件的项目**必须**在自己的清单中写出该目录(例如 `cflags`/`cxxflags`
中的 `-idirafter /usr/include`);`allow_host_libs` 只放开链接,不改变头文件搜索。

当前:clang 一侧由 `ToolchainLinkModel::compile_tokens` 与安装后重新生成的驱动配置文件给出;GCC 的退回路径由同一函数给出
`-isysroot`(mcpp 2026.10.8.1)。e2e 891 覆盖 clang 一侧的未找到、显式写出与依赖图提示的边界。

## 4. 载荷契约

### 4.1 可重定位 部分实现

载荷在使用时**禁止**依赖构建环境中的路径。以下文件含构建环境的路径,但不参与编译与链接,**可以**豁免:
libtool 的 `.la`;gcc 的 `plugin/include/configargs.h` 与 `install-tools/mkheaders.conf`。

### 4.2 不含构建环境的 C 库内容 部分实现

gcc 载荷的 `lib/gcc/<三元组>/<版本>/include-fixed/` **禁止**含带 fixincludes 横幅(`auto-edited by fixincludes`)的头文件。
这类头文件是构建环境 C 库的冻结副本,在搜索顺序上排在构建所用的 C 库之前。gcc 自己生成、不带横幅的头文件不受此限。

当前:gcc 13.3.0、15.1.0 与 11.5.0 的 x86_64-linux-gnu 发布归档含这类文件(mcpp#687)。索引中的 gcc 配方在安装时删除它们(openxlings/xim-pkgindex#870 之后生效);
在此之前安装的载荷由 `mcpp self doctor` 报告(§6.4)。故标为部分实现:安装后的载荷满足本条,发布归档不满足。

### 4.3 安装时的改写 部分实现

| 要求 | 状态 |
|---|---|
| 安装后的改写**必须**限于一张列出的清单:ELF 的 `PT_INTERP` 与 `RUNPATH`、clang 的 `.cfg` | 部分实现:改写集中在单一入口,清单未写成规范 |
| 改写**必须**由标记文件 `.mcpp-fixup.json` 记录 | 已实现 |
| 改写**必须**经副本与原子重命名完成 | 部分实现:mcpp 的安装后修正管线如此;xlings 安装时所做的改写未核对 |
| 清单之外的文件**必须**与发布归档一致 | 未实现:没有检查 |
| gcc 的 `specs` **禁止**在安装时改写 | 已实现 |

### 4.4 完整性 部分实现

载荷**必须**包含其声明的能力所需的全部文件,例如 llvm 载荷的 `share/libc++/v1/std.cppm`,以及 libc++ 运行期依赖的 `libatomic.so.1`。

当前:准入脚本检查 llvm 载荷的这几项,但它不在 CI 中运行。

### 4.5 描述文件 部分实现

载荷根目录的 `.mcpp-toolchain.json` 已支持 `schema`、`frontend`、`platform_floor`、`std_module_defines`、`runner`(已实现)。

它**应当**另外记录来源(未实现):配方所在仓库与提交、编译器的配置行、上游源码的 sha256、构建所用的 C 库及其版本。

### 4.6 修订与资产名 已实现

一个版本在索引中的内容由 sha256 固定。内容有任何变化,**必须**使用新的资产名,因为 GitCode 上的发布资产既不能替换,也不能删除。

---

## 5. 构建

### 5.1 配方入库 部分实现

每个发布的载荷**必须**能由一份检入仓库的配方重新构建。

当前:llvm 子包、musl、glibc 等由 xim-pkgindex 中的构建脚本产出;gcc 由 `fromsource` 配方产出。已发布的三个 gcc 载荷出自两个不同的构建环境,载荷本身不记录构建环境。配方与构建 CI 所在的仓库待定。

### 5.2 构建环境 未实现

构建**必须**在固定的容器中进行;构建 sysroot **必须**取自索引发布的 C 库载荷,**禁止**取自某台机器的 subos。

### 5.3 可复现等级 部分实现

| 等级 | 含义 | 要求 |
|---|---|---|
| 可重建 | 由配方与固定输入重新得到功能等价的载荷 | **必须** |
| 可验收 | 由第 6 节的程序判定合格与否 | **必须** |
| 逐字节可复现 | 固定时间戳、路径前缀映射与归档顺序 | **可以** |

---

## 6. 验收

### 6.1 载荷 lint 未实现

**必须**有一个程序对一个载荷目录执行 §4.1 至 §4.4 的全部检查,并在发布前运行。
每一项检查**必须**有反向测试:把对应的缺陷放回载荷,该项检查必须失败。

### 6.2 兼容矩阵 部分实现

载荷发布或 C 库绑定变化时,**必须**编译一个矩阵:一维是该目标行在索引中可解析的全部编译器版本,另一维是全部 C 库版本。
格子**必须**由索引枚举得到,**禁止**使用手写清单。
每格至少编译一个使用 `<memory>`、`<mutex>`、`<thread>` 的程序,以及一个 `import std` 的程序。
在 MSVC ABI 目标上,矩阵的另一维是 toolset 版本。

当前:描述符变化时,CI 只安装不带版本号时解析出的那一个版本(即 `latest`),并编译一个 `import std` 程序。gcc 描述符的改动因此只在 16.1.0 上验证过,13.3.0 与 15.1.0 未被覆盖。

### 6.3 准入门 部分实现

xim-pkgindex 的准入脚本 `verify-toolchain.sh` 对一个载荷归档做一次真实的编译、链接与运行,并对 llvm 载荷检查完整性与 CRT 的解析位置(已实现)。

它**必须**在 CI 中运行(未实现),并**必须**覆盖 §6.2 的程序(未实现)。当前它默认使用 glibc 2.39,测试程序不含线程头文件。

### 6.4 已安装载荷的诊断 部分实现

`mcpp self doctor` **必须**对已安装的载荷执行 §6.1 中不依赖归档的检查;发现问题时,**必须**给出重装命令。

当前:doctor 执行 §4.2 的检查(gcc 载荷 `include-fixed/` 中带 fixincludes 横幅的文件);§4.1、§4.3、§4.4 的检查未实现。

---

## 7. 发布顺序 未实现

移动一个默认版本或一个 C 库绑定的顺序,由 [SPEC-009](toolchain-maintenance.md) §10 规定;§6.2 的矩阵是该顺序中的一道要求。
本节不再自有条款,实现状态同 SPEC-009 §10:未实现。

---

## 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| v0.1 | 2026-09-24 | 初版草案:身份与写法、来源与选择(含 MSVC ABI 目标的 sysroot)、载荷契约、构建、验收、发布顺序 |
| v0.2 | 2026-09-24 | 随 mcpp 2026.9.24.1 更新实现状态:§2.3、§2.4、§3.1 至 §3.6 已实现;§4.2、§6.4 部分实现;§2.2 更正:不带族的 `system` 被拒绝 |
| v0.3 | 2026-09-28 | 随 mcpp 2026.9.28.1:新增 §3.7,MSVC ABI 的 CRT 模型是目标 ABI 的性质,cl 与 clang++ 同样收到,默认 `toolchain-coupled`(mcpp#718)。 |
| v0.5 | 2026-10-01 | 随 mcpp 2026.10.1.3(mcpp#755):新增 §2.2.1,工具链可由路径命名,并说明 `bootstrap` 与 `configure = "build.mcpp"`;§3.3 增加非缺省来源的陈述、汇总、记录与 `--managed-only`。 |
| v0.4 | 2026-09-28 | 随 mcpp 2026.9.28.2:新增 §3.7.1,程序旁的文件由一个解析器决定;MSVC C++ 运行时是一个带版本的集合;契约决定种类;声明的运行时文件与 toolset 的版本比较;读不出的版本不作决定;action 的 `PATH` 首位是 toolset 的运行时目录(2026-09-28 设计 WS1)。 |
| v0.6 | 2026-10-02 | §7 移入 SPEC-009(工具链的支持与维护):移动默认版本或 C 库绑定的顺序、门与撤销由 SPEC-009 §10 规定,本节改为引用。 |
| v0.7 | 2026-10-09 | 随 mcpp 2026.10.8.1:新增 §3.8,使用受管 C 库的构建不隐式搜索宿主的系统头文件;需要时由项目显式写出。 |
