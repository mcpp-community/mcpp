# Windows 工具链的三条轴:来源、SDK、运行时(2026-08-16)

> 取代 `2026-08-16-toolchain-architecture-review.md` 的 §1 / §2 / §3 / §3d。
> 那份是**发现**(问题在哪、file:line),这份是**方案**(怎么改、怎么算通过)。
> review 的 §3b / §3c / §4 / §5 / §6 / §7 已落地,不在此文范围。
>
> 起因是 review 之后的一轮追问,它推翻了我原方案里的两处形状错误 ——
> 两处都记在 §0.2,因为**错的那版看起来同样合理**,不写下来会被重新提出来。

---

## 0. 先把三样东西分开

### 0.1 今天被混在一起的三层

Windows 上一次构建牵涉三个**互相独立**的问题,而现在它们纠缠在一起:

| 轴 | 问的是什么 | 今天在哪 | 状态 |
|---|---|---|---|
| **来源** | 编译器**哪来的**:机器上的 VS,还是 xim 装的 payload | `is_system_toolchain()` `registry.cppm:428` | ✅ 已建模,但代价摊在 26 处 |
| **SDK** | 用**哪套**头/导入库/工具 | `find_windows_sdk()` `msvc.cppm:628` | ❌ **靠搜**,没有身份 |
| **运行时分发** | 产物**带不带** `vcruntime140.dll` | `distribution.cppm:432` | ❌ 对 MSVC **一律拒绝** |

**判据:三个问题的答案可以两两独立取值。** 受管 toolset 可以配系统 SDK(今天 xrgui CI 就是),
系统 VS 也带自己的 Redist。任何把它们绑成一个开关的设计都是错的。

### 0.2 两处已被推翻的形状(不要重新提出来)

**(a) 不要把 `@system` 推广到所有族。**

初稿把 `is_system_toolchain()` 里的 `Family::Msvc` 读成"不对称,应该补齐",提议
支持 `gcc@system`。**方向反了**:xlings 是用户态 OS,mcpp 建立在它之上,整条设计
就是**把 host 依赖降到最低**。`msvc@system` 是 **Windows 的让步**
(Visual Studio 常常已经装了、又不总能重新分发),不是一种缺失的通用能力。

顺带记一笔:真正提供 host 依赖的那一处,拼写是**不带族的 `system`**
(`prepare.cppm` 里 `*tcSpec == "system"` 那条 `// Explicit user opt-in to system
PATH compiler — kept as escape hatch`)。别再把它当成"缺失的能力"。

**(b) 不要新加 `windows_sdk = "..."` 这个 manifest 键。**

理由有两层:

1. **模型里已经有位置了。** `runtime_binding.cppm:26` 的 `runtimeId` 注释原文就是
   *"Provider-native runtime identity (`glibc@...`, `macos_sdk@...`, `ucrt@...`)"*
   —— `ucrt@...` **早就写着**,只是全仓库没有一处填它,而消费者硬编码 `glibc@` 前缀
   (`runtime_validation.cppm:387`)。
2. **它和 `_WIN32_WINNT` 会变成两个看起来能互相替代、实际管不同事的旋钮**,见 §2.4。

---

## 1. 轴一:来源 —— 收拢特例,不是推广它

### 1.1 问题

`is_system_toolchain()` 的判据本身没问题。问题是**这个平台特例的代价被摊开了**:
lifecycle.cppm **13 处**、prepare.cppm **13 处** msvc 专有分支,而 gcc / llvm / mingw
在 lifecycle.cppm 里**一处都没有**(mingw 更是零分支 —— 它整个被表达成一个
*target*,`x86_64-windows-gnu`,那才是这套设计想要的形状)。

另外三处是**同一条规则的重复拼写**,与特例本身无关:

1. `xim_tool()` + `installation_at()` + 错误信息 —— prepare 与 lifecycle 几乎逐字重复
2. sysroot 依赖规则两份且**不等价**(一份只看 musl,一份 musl + PE + 宿主),
   而注释声称它们互相对应
3. 解析链的注释说 4 步,实际有 **9 个输入**

### 1.2 方案

把"来源"变成一个**解析一次**的值,而不是每个子命令自己再问一遍:

```cpp
enum class Origin { Managed, SystemMsvc };   // 只有两种,而且只会有两种
```

- `prepare` / `lifecycle` 各在入口解析一次,之后按它分发
- `parse_toolchain_spec` 对**非 msvc 的 `@system`** 显式报错并指出替代写法
  (今天是"未实现",于是失败发生在别处、消息说的是别的事)

配套消掉上面三处重复:提 `resolve_managed_msvc(env, pkg)`;合并 sysroot 谓词;
把解析顺序变成一张**数据表**,让注释无处可撒谎。

### 1.3 边界

- **裸 `gcc` 的语义不能变**(那会改现有 manifest 的行为)。只有显式 `@system` 是新语义
- 裸 `system`(不带族)那条逃生口**保留**,它是有意的

---

## 2. 轴二:SDK —— 绑定,而不是搜索

### 2.1 问题

编译器拿到了版本轴,**SDK 没有**。`find_windows_sdk()` 按顺序扫:
`WindowsSdkDir` → 兄弟 xpkgs → 写死的 `C:\Program Files (x86)\Windows Kits\10`。

但对 `msvc@<toolset>`,SDK 是 recipe 里**声明过的依赖**
(`xim:windows-sdk@10.0.26100`),位置是确定的。把确定的东西拿去搜,三个后果:

1. **环境能悄悄改写钉住的选择** —— 一个游离的 vcvars 设了 `WindowsSdkDir`,
   manifest 钉的 SDK 就失效了
2. **今天那个 LNK1104 就是搜出来的** —— 半装的 payload 因为版本号更高,
   排在机器自己那套完整 SDK 前面,每个 TU 都编过,最后炸在链接,
   **日志里没有一行提到 SDK**
3. **两台机器仍可能用不同 SDK 编同一份源码** —— 正是版本轴当初要解决的那个问题,
   只是换到了下一层

### 2.2 方案:按来源分流

```
msvc@<toolset>  →  SDK 从同一条 xim 解析拿(和编译器同一个机制)
msvc@system     →  维持现在的搜索链(机器上的东西只能靠找)
```

受管那条不再"搜到就用",而是**问 toolset 声明了哪个 SDK 依赖**。搜索链降级为
`msvc@system` 专用。

**`WindowsSdkDir` 的地位要一起想清楚。** 它是"有人明确指定",按"声明压过探测"
应当优先;但对受管 toolset 它又会破坏可复现性。建议:

- `msvc@system`:`WindowsSdkDir` 优先(维持今天)
- `msvc@<toolset>`:**忽略它,但要说出来** —— 一行 `note:`,别静默

理由是这条轴的全部意义就是"声明什么就用什么",而静默覆盖会让它失效且不可见。

### 2.3 运行时身份:填那个留了很久的槽

`runtimeId = "ucrt@10.0.26100"`,从 toolset 的 SDK 依赖投影而来,进
`runtimeContractHash`。

**但它和 glibc 不完全同构,这个差别必须写进代码注释**:

| | Linux | Windows |
|---|---|---|
| `glibc@2.39` 绑的是 | 一个 **payload**:头 + `.so` 都在里面 | — |
| 能不能真绑上去 | ✅ patchelf 让产物**真的跑在那份 glibc 上** | ❌ `ucrtbase.dll` 在系统里,**换不掉也不该换** |
| 于是标识的含义 | **运行时绑定** | **兼容性下限声明** |

我们的 `windows-sdk` 包**只装了 ucrt 的一半**(头 + 导入库),
**没有** `Universal CRT Redistributable`,因为 Win10 起 `ucrtbase.dll` 是系统组件、
应用不该自带。这是**有意的**,要写进注释,否则下一个人会以为能像 glibc 那样
把 ucrt 打进产物。

配套:`runtime_validation.cppm:387` 与 `post_install.cppm` 里硬编码的 `glibc@` 前缀
要从"只认 glibc"改成"按 provider 分派",否则填了也没人认。

### 2.4 **不要**加 manifest 的 SDK 版本键

一般 Windows 开发者怎么处理 SDK:

| 做法 | 普遍程度 | 后果 |
|---|---|---|
| VS 装一个"最新",`<WindowsTargetPlatformVersion>10.0</...>`(字面意思就是"本机最新") | 绝大多数 | **同一份源码在两台机器上链到不同 SDK,且无提示** |
| 钉死 `10.0.22621.0` | 有纪律的团队 | 声明路线 |
| xwin / msvc-wine 自己下 payload | Rust 圈 / 交叉编译 | 本质就是我们做的事 |

**默认(跟着 toolset 依赖走)已经强过主流做法** —— 它至少保证每台机器一致,
而主流做法连这一点都不保证。

差别到底在哪,分两半:

| | 影响 | 为什么 |
|---|---|---|
| **ucrt 那一半** | **很小** | Win10 起 `ucrtbase.dll` 是系统组件且向后兼容 |
| **um(Win32 API)那一半** | **会咬人** | 新 API 只存在于新 SDK 的头/导入库 |

两种失败难度天差地别:

- 链到**老** SDK 用了新 API → **编译期**失败,一眼看出 ✅
- 链到**新** SDK 但跑在老 Windows → **运行期**失败(加载失败 / `GetProcAddress` 返回 null)❌

**而真正声明"我承诺跑在哪个 Windows"的是 `_WIN32_WINNT` / `WINVER` 宏,不是 SDK 版本。**
SDK 版本决定的是"我能看见哪些 API"。加一个 `windows_sdk =` 键会让用户以为它就是
兼容性下限旋钮 —— **两个旋钮看起来能互相替代,实际管不同事**,这是比不做更糟的结果。

结论:**这一条不做**。真有少数场景要钉,先把 `_WIN32_WINNT` 的表达方式想清楚,
再决定要不要第二个旋钮。

---

## 3. 轴三:运行时分发 —— `toolchain-coupled` 对 MSVC 是成立的

### 3.1 问题

`distribution.cppm:432` 说:

> `cxx_runtime = "toolchain-coupled"` has no meaning for the MSVC runtime
> (it ships with the OS/redistributable, **not with the toolchain**)

这句话对 `msvc@system` 是对的,对 `msvc@<toolset>` 是**错的** —— 受管 payload 里就有
`VC/Redist/MSVC/<ver>/<arch>/Microsoft.VC143.CRT/{vcruntime140,msvcp140,...}.dll`,
而且 VS 自身安装也带同一个目录,所以**两条来源都能支持**。

于是分发矩阵上有个洞:

| | 自包含 | 宿主耦合 | 工具链耦合 |
|---|---|---|---|
| gcc / libstdc++ | 静态链接 | 用系统 libstdc++ | **拷 libstdc++ 到产物旁** ✅ |
| MSVC | `/MT` ✅ | `/MD` ✅ | **拒绝** ❌ |

### 3.2 已落地的一半

`vc_redist_dir()` 已经把 toolset 自带的那份 CRT 放进 `linkRuntimeDirs`,
Windows 上它就是 PATH —— 所以 **`mcpp run` 已经能在只有受管 toolset 的干净机器上
跑起来**(在此之前默认 `/MD` 产物会以"找不到 vcruntime140.dll"失败,
而 CI 看不到,因为 runner 上装着 VS)。

### 3.3 还差的一半

**把那些 DLL 拷到产物旁**,即 PE 上真正实现 `toolchain-coupled` —— 和 gcc 拷
libstdc++ 是同一件事、同一个语义。

⚠️ **只取 `<arch>/Microsoft.VC*.CRT/`,不能取 `debug_nonredist/`** ——
后者(`vcruntime140d.dll` 等)**不可再分发**。这条要有测试钉住,
`vc_redist_dir()` 已经这么做了,拷贝那一步要沿用同一个判据。

### 3.4 三层的最终形状

```
ucrt        →  SDK 给导入库,运行时由 OS 提供      →  下限声明(runtimeId)
vcruntime   →  toolset 自带 Redist,可随产物走     →  cxx_runtime(§3)
um/shared   →  SDK 给,纯链接期                    →  无运行期对应物
```

---

## 4. 打包:读二进制,别运行它

### 4.1 问题比"没做 Windows"更深

`pack.cppm:323` 的 `ldd_parse` 求依赖闭包的方式是:

```
LD_TRACE_LOADED_OBJECTS=1 '<binary>'
```

**它要把目标二进制跑起来。** 所以这不是"缺一个 PE 分支":

- **跨不了 OS**(Linux 上跑不了 PE)
- **跨不了架构**(x86_64 上跑不了 aarch64 产物,即便同是 Linux)

`pack.cppm:638` 那条 `#if defined(_WIN32)` 的拒绝只是结果,不是原因。

### 4.2 分层

| 层 | 职责 | 平台相关? |
|---|---|---|
| **契约** `distribution.cppm` | 决定**该带什么** | ❌ **已存在** |
| **闭包** | 静态读导入表:ELF `DT_NEEDED` / PE 导入表 / Mach-O `LC_LOAD_DYLIB` | ✅ 纯解析 |
| **系统库判据** | ELF 现有那张表 / PE 的 kernel32、user32、ucrtbase… / Mach-O `/usr/lib`、`/System` | ✅ |
| **重定位** | ELF: patchelf `$ORIGIN/../lib` / **PE: 无操作**(DLL 放 exe 旁就是规则) / Mach-O: `install_name_tool` + `@loader_path` | ✅ |
| **打包** | tar.gz / zip | ✅ |

**换成静态解析之后,跨 OS 打包不是额外功能,是自然结果** —— 没有任何一步需要执行产物。

### 4.3 还有一处断链

**`pack` 从不读 Contract。** `MechanismInput.msvcStaticCrt` 只影响编译链接**旗标**;
`pack` 决定真拷哪些文件,却不认识 `cxx_runtime`。所以那个契约在打包这一步
**没有执行者** —— ELF 上靠 `ldd` 闭包歪打正着,PE 上完全没有。

把第一层接到第二、四层,才是"一套系统"而不是"两套"。矛盾时要报出来
(`pack` 已有"带自己 libc 的包不能消费宿主库"这类拒绝的先例,照着长)。

---

## 5. 兼容性与迁移

| 改动 | 是否改用户可见格式 | 迁移 |
|---|---|---|
| §1 Origin 收拢 | ❌ 纯内部 | 无 |
| §1 拒绝 `gcc@system` | ⚠️ 之前"未实现"、现在显式报错 | 消息要给替代写法 |
| §2 SDK 绑定 | ❌ 行为变更,非格式 | 受管 toolset 上 `WindowsSdkDir` 从"生效"变"忽略+提示" |
| §2 `ucrt@` 身份 | ⚠️ 进 `runtimeContractHash` → **缓存键变** | 一次性全量重建,和别的 contract 变更同类 |
| §3 PE toolchain-coupled | ❌ 新增能力 | 之前是 degraded,现在能满足 |
| §4 PE pack | ❌ 新增能力 | 之前是硬错误 |

**唯一需要留意的是 §2 的 hash 变更** —— 它会让 Windows 上已有的构建缓存失效一次。

---

## 6. 验收判据(每条怎么算通过)

**没有一条可以靠"CI 绿了"算过。** 这一轮十一层缺陷,每一层出现时 CI 都是绿的。

| # | 判据 | 为什么它不能被自我满足 |
|---|---|---|
| §1 | 26 处分支的数量**下降**,且 `gcc@system` 报错里出现替代写法 | 数量可数;消息内容有断言 |
| §2 | 受管 toolset 下,**设一个指向别处的 `WindowsSdkDir`,构建仍用 payload 的 SDK**,且打印了 note | 环境能覆盖=没绑定,这条直接证伪 |
| §2 | `runtimeContractHash` 在 Windows 上随 SDK 版本变化 | 不变就是没进 hash |
| §3 | **在一台没有 VS、没装过 redist 的干净 Windows 上**,`/MD` 产物拷到别处能跑起来 | CI runner 有 VS,**必须用干净机器或容器**,否则这条测不出东西 |
| §3 | 产物旁**没有** `*d.dll` | debug CRT 不可再分发 |
| §4 | **在 Linux 上**为 Windows 产物打出 zip,内含正确 DLL 闭包 | 跨 OS 是这条的全部意义;同 OS 打包证明不了 |
| §4 | `cxx_runtime` 与 `--mode` 矛盾时**报出来** | 静默通过=契约无执行者 |

> §3 和 §4 的判据都要求**比 CI 环境更贫瘠的机器**。这一轮反复出现的形状是
> "验收环境比目标环境富裕",这两条是直接针对它写的。

---

## 6.5 落地状态(2026-08-17,mcpp 2026.8.17.1)

> 落地过程中撞到的、不在计划里的三件事(clang 在两个目标上同时出问题、一处文档
> 自相矛盾、`--mode static` 覆盖用户 target),连同验证结果与遗留账,记在
> `2026-08-17-windows-three-axes-final-report.md`。

**§1 / §2 / §3 / §4 全部实现,§2.4 明确不做。** 逐条对应:

| 条目 | 状态 | 落点 |
|---|---|---|
| §1 Origin 建模 | ✅ | `enum class Origin`(`model.cppm`)—— 放在数据模型里,让 spec 侧(`registry`)与已定位编译器侧(`msvc`)指的是**同一根轴** |
| §1 拒绝 `gcc@system` | ✅ | `parse_toolchain_spec`,错误里同时给出 pin 与 PATH 逃生口两种写法 |
| §1 解析一次 | ✅ | `prepare` 原先把同一个字符串**解析两遍**、各自下结论;现在一次,`origin_of()` 分发 |
| §1 消重复:`resolve_managed_msvc` | ✅ | `registry.cppm`。"payload 在哪、为什么不能用 fetcher 的 `root`"两份手写实现,而理由只写在其中一份里 |
| §1 消重复:sysroot 谓词 | ✅ | `needs_linux_sysroot_payloads()`。两份**不等价**,而其中一份的注释声称它们互为镜像 —— 少的是 PE 那一项 |
| §1 解析链注释 | ✅ | 一张按 `TcOrigin` 枚举名写的表。原先两处、分别声称 3 步和 4 步,合起来点到 9 个输入里的 5 个,还互相矛盾 |
| §2.2 SDK 按来源分流 | ✅ | `msvc::resolve_sdk_for()`;受管来源忽略 `WindowsSdkDir`/`WindowsSdkVersion` 并打印 `note:` |
| §2.2 无 SDK payload 时 | ✅ | 退回机器 SDK 并**说出来**(可用 > 失败,但不可复现这件事必须留痕) |
| §2.3 `ucrt@` 身份 | ✅ | `bind_windows_ucrt()`,进 `runtimeContractHash`;`runtime_provider()` 取代各处 `starts_with("glibc@")` |
| §2.4 manifest SDK 键 | ⛔ **不做** | 理由见该节 |
| §3.3 PE `toolchain-coupled` | ✅ | `dist::Mechanism::deployToolchainRuntime` → `CompileFlags::toolchainRuntimeDeploy` → ninja `stage_file` 边 |
| §3.3 排除 debug CRT | ✅ | 判据只有一处(`vc_redist_dir()`),拷贝那步复用它而不是另立一条按名字的规则 |
| §4 静态读闭包 | ✅ | `mcpp.pack.binfmt`:ELF `DT_NEEDED`、PE 导入表 **+ 延迟导入表** |
| §4 跨 OS 打包 | ✅ | Linux 上给 Windows 产物打 zip;`mcpp.pack.zip` 自己写压缩包(没有哪个 zip 工具在每个宿主上都存在) |
| §4.3 `pack` 读 Contract | ✅ | `cxx_runtime` 决定 toolchain runtime 目录进不进搜索集;与 `--mode` 矛盾时拒绝 |

**两处对方案本身的修正**,来自实现过程:

1. **`dist::Format` 改为先看 target triple、再退回宿主。** 原先除 MinGW 外一律
   问宿主,于是"Windows 上的契约"在 Linux runner 上**根本无法断言** —— 而这轮
   大部分 review 就发生在 Linux runner 上。只**新增**答案:说不出 OS 的 triple
   仍走原来那条推导,现有构建一个都不变。
2. **`force_bundle` 在 PE 上也要能覆盖系统表。** ELF 上一直如此;PE 上第一版把
   系统排除写成了无条件的,那会让一个明确写下来的决定变成装饰。

**§4 未做的一半,以及理由。** ELF 闭包**仍然**通过运行产物取得
(`LD_TRACE_LOADED_OBJECTS`),`binfmt` 的 `DT_NEEDED` 读取只用于识别与诊断。
原因不是懒:`ldd` 交回的是**已解析的路径**,而 `DT_NEEDED` 只有名字,把名字变
成路径要重新实现 loader 的搜索规则(`DT_RPATH` → `LD_LIBRARY_PATH` →
`DT_RUNPATH` → `ld.so.cache` → 默认目录,加上 `$ORIGIN` 展开与 hwcaps 子目录)。
在一条**已经正确、且被 e2e 覆盖**的路径上重写这个,风险远大于收益。
**代价要说清楚:跨架构的 ELF 打包(x86_64 上给 aarch64 产物打包)仍然不支持**,
而这正是 §4.1 指出的第二个限制。PE 那条没有既有实现,所以它从一开始就是静态的。

---

## 7. 落地顺序

| # | 项 | 规模 | 依赖 |
|---|---|---|---|
| 1 | §3.3 PE `toolchain-coupled`(拷 DLL) | 中 | 无。`vc_redist_dir()` 已在 |
| 2 | §2.2 SDK 按来源分流 | 中 | 无 |
| 3 | §2.3 `ucrt@` 身份 + 解开 `glibc@` 硬编码 | 中 | 建议紧跟 2 |
| 4 | §1 Origin 收拢 + 消三处重复 | 大 | 建议在 2/3 之后 —— 都动同一片代码 |
| 5 | §4 PE pack | 大 | 排在 1 之后:先解决"能跑",再解决"能分发" |

**§2.4(manifest SDK 版本键)不做**,理由见该节。

前三项互不依赖,可并行。§1 和 §5 各自单独一轮。

---

## 8. 一句话

这三条轴今天被搅在一起,是因为**只有编译器那一条被显式建模过**。
SDK 靠搜、运行时靠拒绝、打包靠一个要跑起来才work的原语 ——
每一处都在"环境恰好合适"时正常工作,而在贫瘠环境里失败,
**且失败信息不指向真正的原因**。

把三条轴各自变成一个**声明出来的、解析一次的值**,是这份方案唯一在做的事。
