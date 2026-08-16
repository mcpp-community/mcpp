# Windows 三条轴:落地报告(mcpp 2026.8.17.1)

> 设计:`2026-08-16-windows-toolchain-three-axes-design.md`(§6.5 记录逐条状态)
> 实现:mcpp #448(单 PR,含设计文档本身)
> 前一轮:`2026-08-16-msvc-ecosystem-final-report.md`

---

## 0. 一句话

上一轮给**编译器**装上了版本轴;这一轮把它编译时用的**头文件**和产物加载的
**运行时**也各自变成了**声明出来、解析一次**的值 —— 并且顺手让 `mcpp pack`
不再需要**运行产物**才能知道产物依赖什么。

---

## 1. 三条轴,分别落在哪

| 轴 | 之前 | 现在 |
|---|---|---|
| **来源** 编译器哪来的 | ✅ 已建模,代价摊在 ~30 处分支 | 解析一次(`Origin`),三处重复各消掉一份 |
| **SDK** 用哪套头/导入库 | ❌ **靠搜**,两种来源同一条链 | **按来源绑定**;受管 toolset 忽略环境并说出来 |
| **运行时分发** 产物带不带 vcruntime | ❌ 对 MSVC **一律拒绝** | PE 上 `toolchain-coupled` 真正成立 |
| **打包**(轴三的下游) | ❌ Windows 硬拒绝;跨不了 OS/架构 | 静态读导入表,任何宿主都能给 PE 打 zip |

### §2 SDK:绑定而不是搜索

`find_windows_sdk()` 过去按 `WindowsSdkDir` → 兄弟 store → 常规路径扫,**两种来源
走同一条链**。于是被 pin 住的 `msvc@<toolset>` 是一个**环境可以悄悄改写**的 pin,
两台机器可以用两套 SDK 编同一份 manifest,而日志里没有一行提到 SDK。这不是假设:
这一轮之前追的那个 LNK1104,就是一个只解包了一半的 payload 因为版本号更高赢了这场
扫描。

```
msvc@<toolset>  →  随该 toolset 装进 store 的 windows-sdk payload。
                   WindowsSdkDir / WindowsSdkVersion 被忽略 —— 并且打印 note。
msvc@system     →  维持今天的搜索链。机器上的东西只能靠找,而在那里
                   "明确声明"应当压过"扫描"。
```

旁边没有 SDK payload 的受管 toolset **仍然**退回机器的 SDK —— 能用 > 失败 ——
并且**说出来**,因为那次构建已经不可复现,而除此之外没有东西会记录它。

### §2.3 `ucrt@<version>`:填一个从字段诞生起就留着的槽

`RuntimeBinding::runtimeId` 的注释从一开始就写着
*"(`glibc@…`, `macos_sdk@…`, `ucrt@…`)"*,而仓库里从来没有一处写过 `ucrt@`。
于是 SDK 版本进不了 `runtimeContractHash`,**两套 SDK 共用一个构建缓存键**。

**它和 `glibc@` 不同构,而且这一点写在会被读到的地方**:

| | `glibc@2.39` | `ucrt@10.0.26100.0` |
|---|---|---|
| 绑的是 | 一个 **payload**,头和 `.so` 都在里面 | — |
| 能真绑上去吗 | ✅ patchelf 让产物真跑在那份上 | ❌ `ucrtbase.dll` 是 OS 组件,换不掉也不该发 |
| 于是标识的含义 | **运行时绑定** | **兼容性下限声明** |

所以它**不**投影进 `libc` —— 否则私有 libc 那套机制会去找一个本就不该存在的
payload。各处 `starts_with("glibc@")` 改成 `runtime_provider()` 分派:另一个
provider 读起来是"这里没有规则",而不是"没有身份"。

**§2.4(manifest 的 SDK 版本键)明确不做** —— 它会和 `_WIN32_WINNT` 并排,看起来
可以互相替代,实际管的是不同的事。

### §3.3 PE 上的 `toolchain-coupled` 现在有意义

原来的拒绝语是 MSVC 运行时"随 OS/redistributable 分发,而不是随工具链"。
对 `ucrtbase.dll` 是对的;对 `vcruntime140.dll` / `msvcp140.dll` 是**错的** ——
它们躺在每个 toolset 的 `VC\Redist\MSVC\…` 里,正是 gcc 和 `libstdc++.so` 的关系。

于是它拿同一个契约。PE 没有 rpath,所以**机制**是拷到产物旁而不是加一条搜索路径 ——
同一个契约,不同的机制,这正是三层模型存在的理由。

`/MT` 仍然是降级,而且是**真正的矛盾**:静态 CRT 根本没有 DLL 可以耦合,消息会说清
是哪一边赢了。DLL 集合来自 `vc_redist_dir()` —— 排除 `debug_nonredist\`(不可再分发)
的**唯一**判据。在这里再写一条按名字的规则,可能和它不一致,而在**这件事**上不一致
是许可问题,不是 bug。

### §4 打包:读导入表,别运行产物

`mcpp pack` 用 `#if defined(_WIN32)` 拒绝 Windows,理由写的是工具是 POSIX-only。
那是症状。闭包来自

```
LD_TRACE_LOADED_OBJECTS=1 '<binary>'
```

—— 它**要把产物跑起来**,所以既跨不了 OS,也跨不了**架构**。移植 `tar` 没有用,而
2026-05 那份设计提的每一个工具(dumpbin、`ImageNtHeader`、`Compress-Archive`)都会
把障碍**下移一层**,因为它们都只存在于问题已经消失的那个平台上。

- `mcpp.pack.binfmt` —— ELF `DT_NEEDED`(经段表翻译);PE 导入表**和延迟导入表**
  (少一个延迟导入不会在启动时失败,而是在第一次调用它时失败,更难查)
- `mcpp.pack.zip` —— mcpp 自己写压缩包,理由相同:没有哪个 zip 工具在每个宿主上都
  存在。条目 **stored** 不压缩,这是真实代价和诚实的取舍:DEFLATE 编码器是这里唯一
  可能产出**解出来是错的**而不是响亮失败的部分,而 mcpp 没有 zlib 可借。**确定性**:
  不读任何时间戳,所以公布校验和才有意义
- **§4.3 契约终于到达打包这一步**:以前它止步于编译/链接旗标,决定"哪些文件真的跟着
  走"的那一步看不见承诺过什么 —— ELF 上 `ldd` 闭包**碰巧**一致,PE 上没有任何东西一致

**明确没做的一半,连同代价**:ELF 闭包**仍然**运行产物。`ldd` 交回的是**已解析的
路径**,`DT_NEEDED` 只有名字,把名字变成路径要重新实现 loader 的搜索顺序
(`$ORIGIN`、`DT_RPATH` → `LD_LIBRARY_PATH` → `DT_RUNPATH` → `ld.so.cache`、hwcaps)。
在一条**已经正确、有 e2e 覆盖**的路径上重写它,风险大于收益 —— 于是**跨架构的 ELF
打包仍然不支持**,这正是 §4.1 指出的第二个限制。

### §1 收拢来源轴,而不是推广它

- **`gcc@system` / `llvm@system` 在被读到的地方就拒绝**,并同时给出两种可能的本意。
  它们过去能解析,然后在别处以 `xim:gcc@system` → "no such package" 失败,把读者
  引向一个根本不会存在的版本。`msvc@system` 是对**一个平台**的让步,不是别的族缺失
  的能力;不带族的 `system` 逃生口原样保留
- **spec 过去在相隔十几行的地方被解析了两遍**,各自下结论。现在一次,`origin_of()` 分派
- `resolve_managed_msvc()` 取代两份手写的"受管 toolset 在哪、为什么不能用 fetcher 的
  `root`" —— 而理由只写在其中一份里
- `needs_linux_sysroot_payloads()` 取代同一条规则的两种拼写,其中一份的注释声称它们
  互为镜像。**它们不是** —— 少了 PE 那一项。今天不可达,这正是它活下来的原因
- 工具链解析顺序**被写了两遍**,一处说 3 步一处说 4 步,合起来点到 9 个真实输入里的
  5 个,还互相矛盾。现在一张表,按 `TcOrigin` 的枚举名写,不会悄悄失配

---

## 2. 验证

### 2.1 单元测试:31 个新增,两个**故意不能被"CI 绿了"满足**

- `WindowsSdkDirCannotOverrideAPinnedToolsetsSdk` —— 设计 §6 的验收判据写成单元测试。
  把 `WindowsSdkDir` 指到别处,payload 的 SDK 必须仍然赢,**并且** note 必须说出变量
  被忽略了。一个被**静默**忽略的覆盖,和一个从未设置过的覆盖无法区分
- `NothingElseAsksForStagedRuntimeFiles` —— 那个 deploy 标志会到达一个拷贝步骤,所以
  一个多余的 `true` 会在一个根本没有这种东西的平台上往产物目录里放 DLL。扫过
  (format × stdlib × contract × /MT × explicit) 的每一格

### 2.2 e2e 240:**在 Linux 上**给 PE 打包

放在 mingw-cross job 里,因为**在 Windows 上跑它什么也证明不了**。它构建一个真正的
交叉 PE,放一个该 EXE 会导入的 DLL 的替身,打包,然后由 **Python** 独立验证压缩包:

- `msvcrt.dll` 在 —— **正面**那一半。一个什么都没读到的解析器**产不出**它
- `kernel32.dll` 不在 —— **反面**那一半。单独看,一个什么都没读到的解析器也能通过;
  两条合起来才是决定性的
- `--mode system` 不放任何 DLL;`toolchain-coupled` + `--mode system` 被拒绝,消息里
  同时有契约名和出路

### 2.3 本机真实验证

在这台 Linux 上,`mcpp pack --target x86_64-windows-gnu` 产出的 zip 被
`python3 -m zipfile` 和 `unzip -t` 双双接受,内含 exe 和解析出来的 DLL,没有别的。
`--format dir`、`--mode self-contained`、`--mode static --target …` 逐一验过。

### 2.4 CI

mcpp #448:**19 项全绿**(Linux / macOS ARM64 / Windows × build+unit+e2e+toolchains、
hermetic 容器、四条 cross-build、xlings 集成)。

---

## 3. 路上撞到的三件事(都不在计划里)

### 3.1 clang 在**两个目标上同时**出问题

同一份新代码,gcc 到处都绿,而 clang:

| | 现象 |
|---|---|
| Windows | clang 20.1.7(MSVC ABI)**编译** mcpp.pack 时段错误,0xC0000005,无诊断,五个 job 同时红 |
| macOS ARM64 | `test_pack_binfmt` 在**运行**时 SIGSEGV |

**解析器不是问题,而且这是量出来的**:同一份代码在 clang 22.1.8 + libc++ 下
ASan+UBSan 干净,在 x86_64 Linux 上**作为 clang 模块**编译并运行正确(-O0/-O2)。

处理方式沿用本仓库已有的判例(`hostflags.cppm` 的开头注释:往一个模块的匿名命名空间里
加一个**没被使用**的函数,会让**相邻**函数被误编译;结论是"机制未知,复现稳定,便宜的
反应是别往那个命名空间里加东西")。于是移除形状、保留行为:跨模块的作用域枚举做导出
结构体的默认成员、ranges 投影取导入类型的成员、跨模块边界的 `std::span`、模块 purview
里的函数模板 —— 全部换成更简单的等价写法。**哪一个是原因并未确定,注释里就是这么写的**,
而不是编造一个结论。

macOS 那一半后来被**拆分测试**定位到了:崩在 `ThePeFixtureItselfIsWellFormed` ——
**完全不碰任何模块**的测试夹具代码。把夹具改写成最朴素的形式(不用 span 参数、不用捕获
可变字符串的 lambda)之后消失。夹具现在带一个 `MCPP_TEST_TRACE=1` 才开的分步 trace:
如果它再来,一轮 CI 就能定位,而不是这次的四轮。

### 3.2 一处文档自相矛盾

`docs/03-toolchains.md` 的 MinGW 段说 `[build] linkage` 这个键不存在、会被静默忽略;
两百行之后的 MSVC 段**恰好**把它当成选 `/MT` 的写法展示。是照着文档写了一遍、看着 mcpp
打印 `unsupported key 'linkage' (ignored)` 才发现的 —— 而跟着这一页做的用户,得到的也是
这个,只是没人告诉他为什么什么都没变。两个语种都已修正。

### 3.3 `--mode static` 会覆盖用户点名的 target

强制 musl 的那次重新 prepare 忽略了 `opts.targetTriple`,于是
`--mode static --target x86_64-windows-gnu` 悄悄变成一次 **Linux** 构建。在 PE 打包
还不存在时这是看不见的 —— 根本没有 Windows 包可以让人发现它缺了 —— 而现在它是个错误答案。

---

## 4. 留下的账(都写清楚了,没有藏)

| # | 事项 | 为什么这次不做 |
|---|---|---|
| 1 | **macOS 上的 `mcpp pack` 会执行用户的产物** | `ldd_parse` 在 macOS 上等于直接跑二进制(`LD_TRACE_LOADED_OBJECTS` 在那里不是环境变量)。两个 pack e2e 都 `requires: pack patchelf elf`,即 **macOS 上完全没有覆盖**,改一条没有测试的路径无法验证。`binfmt` 已经识别 Mach-O,补 `LC_LOAD_DYLIB` 读取是自然的下一步 |
| 2 | **跨架构 ELF 打包**仍不支持 | 见 §4「明确没做的一半」 |
| 3 | `[pack] include` / `exclude` 被解析、存进 Plan,**从未被消费** | 早于本轮;文档把它们当作已有功能。属于 pack 的另一条轴,不在三条轴的范围内 |
| 4 | Windows CI 仍用 `7z a -tzip` 手工打包 release | 那是一个自带 `registry/` 的定制布局,不是 pack 的输出。现在 `mcpp pack` 能产 zip 了,迁移是可能的,但那是发布流程的改动 |
| 5 | Windows 宿主 → Linux target 时 `dist::Format` 仍解析为 PE | 早于本轮。本轮的 triple 判据**只新增答案**(说不出 OS 的 triple 走原推导),刻意没有动这一条:它会改变一个正在通过的 CI job 的旗标,而对这三条轴没有好处 |

---

## 5. 迁移(用户视角)

| 改动 | 用户可见格式 | 迁移 |
|---|---|---|
| `ucrt@` 进 `runtimeContractHash` | ⚠️ 缓存键变 | **Windows 构建缓存重建一次**,和任何 contract 变更同类 |
| 受管 toolset 绑定 SDK | 行为,非格式 | pinned toolset 上 `WindowsSdkDir` 从"生效"变成"忽略 + 报告" |
| 拒绝 `gcc@system` | ⚠️ 之前"未实现",现在显式错误 | 消息给出两种替代写法 |
| PE `toolchain-coupled` / PE pack | 新增能力 | 之前是 degraded / 硬错误 |
| `--mode static --target <pe>` | ⚠️ 之前静默变成 Linux 构建 | 现在按 target 走 |

其余全部是内部改动。
