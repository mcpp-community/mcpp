# 工具链架构 review:两种来源、选择与切换、构建与分发(2026-08-16)

> 配套 `2026-08-16-msvc-as-a-managed-toolchain.md`(设计)与
> `2026-08-16-msvc-ecosystem-cross-repo-plan.md`(跨仓库落地)。
> 那两份讲**已经做了什么**;这份讲**现在的形状哪里还不对,以及怎么更优雅**。
>
> 依据是把 `msvc@<toolset>` 真正跑通的这一轮:九层缺陷、五次索引发布窗口、
> 三处"不可能失败的测试"。下面每一条都指得出具体的 `file:line`,
> 不是风格偏好。

---

## 0. 结论先行

已经做对的那件事,是**把"获取"和"解析"拆成两条正交的轴**:

| | 获取(哪来的) | 解析(怎么找到 cl.exe) |
|---|---|---|
| `msvc@system` | 探测这台机器 | `installation_from_tools_dir()` |
| `msvc@<toolset>` | xim 装的 payload | **同一个** `installation_from_tools_dir()` |

受管 toolset 因此不是第二条代码路径,长不出自己的 bug。**这个模型是对的,
下面所有建议都是把它贯彻得更彻底,没有一条要推翻它。**

问题集中在一句话:**这条轴只对 MSVC 存在,而且只贯彻了一半。**

---

## 1. 一个平台特例,摊在 26 处 ⭐

### ⚠️ 本节初稿是错的,先说清楚

初稿把 `is_system_toolchain()` 里的 `Family::Msvc` 读成"不对称,应该推广到所有族",
提出支持 `gcc@system`。**方向反了,而且那个东西根本不存在。**

xlings 是**用户态 OS**,mcpp 建立在它之上,整条设计就是**把 host 依赖降到最低**:
工具链来自 payload,这才使得"manifest 写 14.44.35207"在**每台**机器上成立,
而不是只在恰好装了它的那台上成立。`msvc@system` 是 **Windows 的让步**
(Visual Studio 常常已经装了、又不总能重新分发),不是一种可推广的能力。

被纠正之后我又去给自己发明的拼写写了个"显式拒绝",那是**从自己的错误里长出来的
新范围**,已经作废(mcpp#441 已关闭)。而且那条拒绝消息还说错了一件事 ——
它写"只有 `msvc@system` 存在",可 `prepare.cppm:1376` 有一条**故意保留**的裸
`system` 逃生口:

```cpp
} else if (tcSpec.has_value() && *tcSpec == "system") {
    // Explicit user opt-in to system PATH compiler — kept as escape hatch.
```

**真正提供 host 依赖的那一处,拼写是不带族的 `system`。** 记在这里,
免得下一份 review 再把它当成"缺失的能力"。

### 剩下的、与那个错误无关的部分

`registry.cppm:428` 的判据本身没问题。问题是**这个平台特例的代价被摊开了**:
lifecycle.cppm **13 处**、prepare.cppm **13 处** msvc 专有分支,
而 gcc / llvm / mingw 在 lifecycle.cppm 里**一处都没有**
(mingw 更是零分支 —— 它整个被表达成一个 *target*,`x86_64-windows-gnu`,
那才是这套设计想要的形状)。

其中由 `is_system_toolchain` 直接触发的四处:

| 位置 | 内容 |
|---|---|
| `lifecycle.cppm:637` | `toolchain install` 拒绝安装 system |
| `lifecycle.cppm:848` | `toolchain default` 持久化 `"msvc@system"` **并清掉 `default_target`**(别的族没有这一步) |
| `lifecycle.cppm:931` | `toolchain remove` 拒绝 |
| `prepare.cppm:1260` | 构建期短路整条 xim 路径 |

`prepare.cppm:1257` 那个变量叫 `tcSpecIsMsvc`,但它其实是 `is_system_toolchain`
的结果 —— 名字说的是"族",值说的是"来源"。

另外三处是同一条规则的**重复拼写**,和特例本身无关,纯粹是复制:

1. `xim_tool()` + `installation_at()` + 错误信息 —— `prepare.cppm:1322-1341`
   与 `lifecycle.cppm:730-755` 几乎逐字重复 → 提一个
   `resolve_managed_msvc(env, pkg)`。第三份出现的时候就是下一个 #436。
2. sysroot 依赖规则两份且**不等价**(`prepare.cppm:1471-1480` 只看 musl,
   vs `lifecycle.cppm:694-702` musl + PE + 宿主)→ 合成一个谓词。
   `lifecycle.cppm:692` 的注释声称它们互相对应。
3. `prepare.cppm:1002-1005` 的注释说解析是 4 步,实际链路有 **9 个输入**。
   注释比代码老,而这段代码是**决定用哪个编译器**的。

### 建议:收拢,不推广

把"来源"变成一个显式的、**解析一次**的东西,而不是每个子命令自己再问一遍:

```cpp
enum class Origin { Managed, SystemMsvc };   // 只有两种,而且只会有两种
```

`prepare` / `lifecycle` 各解析一次,之后按它分发。特例仍然是特例,
但只在**一处**被认出来。

## 2. SDK 是"依赖",却被当成"搜索路径" ⭐ 高优先级

### 现状

编译器拿到了版本轴,**SDK 没有**。

`msvc.cppm:589` 的 `find_windows_sdk()` 按顺序扫:
`WindowsSdkDir` → 兄弟 xpkgs → 写死的 `C:\Program Files (x86)\Windows Kits\10`。

但对 `msvc@<toolset>` 来说,SDK 是 recipe 里**声明过的依赖**
(`xim:windows-sdk@10.0.26100`),位置是确定的。确定的东西被拿去搜,
就会出现今天这两个后果:

1. **半装的 payload 因为版本号更高,排在机器自己那套完整 SDK 前面**,
   每个 TU 都编过,最后炸在 `LNK1104: cannot open file 'kernel32.lib'`,
   而日志里没有一行提到 SDK。(已修:`pick()` 现在两半都要,
   但那是"别选错",不是"直接知道选哪个"。)
2. **manifest 无法钉 SDK 版本**。可以写 `msvc@14.44.35207`,
   不能写"配 10.0.26100 那套 SDK"。两台机器仍可能用不同 SDK 编同一份源码 ——
   正是版本轴当初要解决的那个问题,只是换到了下一层。

### 建议

对受管 toolset,**绑定**而不是搜索:

```
msvc@<toolset>  →  SDK 从同一条 xim 解析拿(和编译器同一个机制)
msvc@system     →  维持现在的搜索链(机器上的东西只能靠找)
```

再给 manifest 一个可选的显式轴:

```toml
[toolchain]
windows = "msvc@14.44.35207"
windows_sdk = "10.0.26100"     # 可选;省略则用 toolset 声明的那个依赖
```

**收益**:两条来源在**两半**上都对称(编译器 + SDK),而不是只在编译器这一半;
"声明什么就用什么"这句承诺覆盖整个工具链而不是它的一部分。

---

## 3. `cxx_runtime = "toolchain-coupled"` 对 MSVC 被拒,但工具链**确实带着**运行时 ⭐ 高优先级

### 现状

`distribution.cppm:432`:

> `cxx_runtime = "toolchain-coupled"` has no meaning for the MSVC runtime
> (it ships with the OS/redistributable, **not with the toolchain**)

这句话对 `msvc@system` 是对的,对 `msvc@<toolset>` 是**错的**。
受管 payload 里就有:

```
VC/Redist/MSVC/14.44.35112/x64/Microsoft.VC143.CRT/
    vcruntime140.dll   msvcp140.dll   msvcp140_2.dll   vcruntime140_threads.dll
```

recipe 的 payload 集里明确包含 `Microsoft.VC.<ver>.CRT.Redist.X64.base.vsix`
(`pkgs/m/msvc.lua:134`)—— **我们下载了它,然后一个字节都没用。**
而 VS 自身安装也带同样的目录,所以两条来源都能支持。

于是分发矩阵上出现一个洞:

| | 自包含 | 宿主耦合 | 工具链耦合 |
|---|---|---|---|
| gcc / libstdc++ | 静态链接 | 用系统 libstdc++ | **拷 libstdc++ 到产物旁** ✅ |
| MSVC | `/MT` ✅ | `/MD` ✅ | **拒绝** ❌ |

### 建议

PE 上实现 toolchain-coupled:把 `Microsoft.VC*.CRT/*.dll` 拷到产物旁边 ——
和 gcc 拷 libstdc++ 是同一件事、同一个语义。

**收益**:`/MD` 构建**可分发**,不再要求目标机装 VC++ 运行时;
三行分发契约在两个 ABI 上一致;已经付过的下载有了用处。

### 而且这不只是"少个功能",是 `mcpp run` 的一个潜在失败

默认是 `/MD`,产物链的是 `vcruntime140.dll` / `msvcp140.dll`。
这两个**不是** Windows 自带的(自带的是 `ucrtbase.dll`,Win10 起进系统),
它们来自 VC++ 可再发行包。

而全仓库搜不到一处 `Redist` / `vcruntime140` / `msvcp140`:

```
$ grep -rn "Redist\|vcruntime140\|msvcp140" src/
（无匹配）
```

也就是说:一台**只装了受管 toolset、没装过 VS 也没装过 redist** 的干净 Windows,
`mcpp build` 会成功,`mcpp run` 会以「找不到 vcruntime140.dll」失败。
今天没暴露,是因为 CI runner 上装着 Visual Studio ——
**又一次「验收环境比目标环境富裕」**,和 §5 是同一个形状。

`runtimeLibraryDirs`(Windows 上就是 PATH,`env.cppm:163`)这条通道是现成的,
缺的只是把受管 payload 的 `VC/Redist/.../Microsoft.VC143.CRT` 放进去。
所以这一条同时修两件事:**运行**(把 DLL 放上 PATH)和
**分发**(把 DLL 拷到产物旁)。

**注意**:debug 的 `vcruntime140d.dll` 在 `debug_nonredist/` 下,
**不可再分发** —— 实现时必须只取 `x64/Microsoft.VC143.CRT/`,不能取
`debug_nonredist/`。这一条要写进测试。

---

## 3b. `mcpp doctor` 里还留着 #436 修掉的那个 bug —— 现成的第四份拷贝 🔴 立刻可修

`doctor.cppm:398`:

```cpp
auto bin = mcpp::toolchain::toolchain_frontend(
    vEntry.path() / "bin", mcpp::toolchain::to_xim_package(s));
if (bin.empty()) continue;          // ← 装好的 msvc toolset 在这里被丢掉
```

这就是 #436 修的那一条:cl.exe 在 `VC/Tools/MSVC/<ver>/bin/Host<h>/<arch>/`,
深四层,`bin/` 下什么都没有 → `continue`。#436 把
`toolchain list` 换成了 `payload_frontend(root, pkg, family)`,
**但 doctor 没跟着换。**

后果:同一台机器上 `mcpp toolchain list` 看得见的 msvc toolset,
`mcpp doctor` 看不见 —— 两个都"成功",而且不一致。

**修法**:`doctor.cppm:398` 改用 `payload_frontend`。一行。
这也是 §1 那句"布局规则被抄了第 N 份"的活证据:#436 消掉了第三份,
第四份一直在。

---

## 3c. `has_usable_msvc()` 把"这台机器有 VS"当成了"这里能用 MSVC" ⭐ 中高优先级

`msvc.cppm:660` 只探测机器:

```cpp
return find_std_module_source().has_value() && find_windows_sdk().has_value();
```

但它被用在三个对**受管 toolset 同样适用**的决策上:

| 位置 | 决策 |
|---|---|
| `prepare.cppm:1097` | 首次运行时要不要把 Windows 用户导向 mingw |
| `prepare.cppm:1389` | 离线错误文案 |
| `prepare.cppm:1558` | MSVC ABI 修复门 |

于是:**一台钉了 `msvc@14.44.35207`、但没装 Visual Studio 的机器,
这个判据回答 `false`** —— 明明工具链就在 payload 里躺着。

这和 §1 是同一个病:**判据问的是"族/机器",而答案取决于"来源"。**

**建议**:拆成两个问题 —— `system_msvc_is_usable()`(现在这个)和
`msvc_is_available(spec)`(system 就探测,受管就看 payload 在不在)。
调用点各自选一个,而不是共用一个含糊的。

---

## 3d. Windows 上的"打包分发"是空的 ⭐ 高优先级(范围最大)

`pack.cppm:637`:

```cpp
#if defined(_WIN32)
    // `mcpp pack` is not yet supported on Windows.
```

整个 `mcpp pack` 是 ELF 专用的:`LD_TRACE_LOADED_OBJECTS` / `ldd` /
`patchelf` / `tar`。Windows 上直接返回错误,让人去用 CI 的 zip。

而且**两套系统并不通话**:

- `distribution.cppm` 决定的是**契约**(自包含 / 宿主耦合 / 工具链耦合),
  只影响编译链接**旗标**;
- `pack.cppm` 决定的是**真的拷哪些文件**,它**从不读 Contract**,
  也不认识工具链。

所以今天 `cxx_runtime` 这个契约在打包这一步是**没有执行者**的 ——
ELF 上靠 `ldd` 闭包歪打正着,PE 上则完全没有这一步。

**建议**(按性价比排序):

1. **先让 §3 的 toolchain-coupled 落地**(把 VC CRT DLL 拷到产物旁)。
   这一步不需要 `pack` —— 它属于构建产物布局,而且顺手修掉 §3 那个
   `mcpp run` 在干净机器上的失败。
2. 再做 PE 版 `pack`:DLL 闭包用 `dumpbin /dependents` 或读 PE 导入表
   (后者无外部依赖,更符合这套代码库的口味),`is_system_lib()` 换成
   Windows 的系统 DLL 白名单(kernel32/user32/ucrtbase/…),打 zip 而不是 tar。
   已有设计稿:`.agents/docs/2026-05-19-pack-windows-design.md`。
3. 让 `pack` **读 Contract**:`--mode` 与 `cxx_runtime` 现在是两套词汇,
   讲的是同一件事。至少要在两者矛盾时报出来
   (`pack.cppm:192-229` 已经有这类拒绝的先例,可以照着长)。

---

## 3e. 主构建和 host helper 用不同的链接策略(#437) 🔴 已修

同一个工具链、同一个环境,两条链接路径给出不同结果:

- **主构建**在 macOS 上刻意 `-fuse-ld=lld`(`flags.cppm` 的 macOS 分支,
  注释原文:*"Xcode 15.4's ld aborting at launch ... when its libc++
  resolution was diverted"*);
- **host helper**(`build.mcpp`)走 `hostflags.cppm` 的 trust-cfg 分支,
  **返回空 link token**,于是用 Xcode 的 `/usr/bin/ld` —— 那个 ld 自己链 libc++,
  跑在 payload 的 `DYLD_*` 里,dyld 把它的 libc++ 解析到 payload 那份,
  缺 `__ZdaPv`,**还没开始链接就 abort**。

> **mcpp 用 llvm 能把整个工程构建出来,却构建不了自己的 `build.mcpp`** ——
> 这不是 macOS 环境问题,是两条路径没对齐。主构建早就绕过了这个坑,
> host helper 这条没跟上。

修法:trust-cfg 分支在 macOS 上也追加 `-fuse-ld=lld`。cfg 选的是 runtime,
**从来没选过链接器**。

**它和这份 review 其余部分是同一个形状**:一条规则有两处实现,其中一处知道
真相、另一处不知道 —— 和 §1 那三处重复拼写、§3b 的 doctor 第四份拷贝完全同类。
根因记在 `2026-08-13-build-optimization-status.md` §9a-3。

---

## 4. `installed()` 的语义:必须是"这份 recipe 产出的状态" ⭐ 跨仓库规则

这一轮九层缺陷里,真正致命的几层全是这一条。

### 两条子规则

**(a) 它必须是覆盖,不是抽样。** 每条断言对应一个"只有它才提供"的 payload。
`windows-sdk` 现在是这样做的:`gdi32.lib` 只在 Desktop Libs
(它的 365 个库和 Store Apps Libs 的 116 个**完全不相交**),所以哪个 payload
没到,报错就点名哪一个。

**(b) 它必须能表达"不该在什么"。** 版本号不会因为 recipe 改了就变,
所以 `installed()` 是**唯一**能把老机器拉回来的东西。#637 让 recipe 不再安装
`vctip.exe`,但已经装了的机器一点没变 —— 直到 #639 让 `installed()`
检查它**不在**。

> 新增文件靠断言"它在"就能发现,**删掉的文件必须显式说"它不该在"**。

### 建议

1. 写进 `.agents/skills/xpkg-creater/SKILL.md`(规则 + 这两个反例)。
2. 加一条 CI lint:`install()` 落 N 个 payload 的 recipe,
   `required_files()` 至少要提到 N 个互不相同的路径。
   便宜,而且正好挡住"覆盖退化成抽样"。

---

## 5. 不运行被测物的验收步骤,验的是解压 ⭐ 中优先级

索引侧 windows-test 做的是「装 → 检查 → 卸」,**全程不编译**。
而 `vctip.exe` 是 `cl.exe` **运行时**才被拉起的 —— 不跑编译器就没有进程,
卸载自然成功。于是 msvc.lua 连续几个 PR 的 windows-test 全绿,
包括「post-uninstall checks」,而卸载对任何真正构建过东西的人都是坏的。

### 建议

工具链类包的 windows-test 增加一步:**编译并链接一个真程序**,再卸载。
至少把包自己 `programs` 里声明的东西各跑一次。
代价是几十秒,挡住的是"装得上但用不了"这整类缺陷 —— 这一轮里它出现了四次。

---

## 6. 卸载的健壮性只做在 mcpp 一侧 ⭐ 中优先级

`remove_payload_tree()`(`lifecycle.cppm:224`)现在会:清只读位 → 有上限重试
→ **改名挪走** → 下次生命周期命令清扫。

但 `xlings remove msvc` 有**一模一样**的问题:Windows 不让删含打开文件的目录,
而 `/Zi` 构建留下的 `mspdbsrv.exe` 就住在 payload 里。

### 建议

把 park-and-sweep 下沉到 xlings 的卸载路径。谁装的谁卸,这个能力不该只有
mcpp 有 —— 否则每个消费者都要自己重写一遍,而写错的方式很多
(我第一版就是"靠试删来找占用者",诊断变成了第二次破坏)。

---

## 7. 索引发布窗口没有可观测的"就绪"信号 ⭐ 中优先级

今天撞了**五次**。「合入 → 索引 → 装得到」之间有个肉眼不可见的窗口,
而窗口期内的失败信息是 `not found` —— 和"这个包根本不存在"一模一样。

最难受的是第 3 次和第 5 次:它们让**已经修好**的缺陷看起来像没修好,
只有对着时间戳才分得清。

### 建议

索引发布一个**修订号**,消费者能等到 `>= rev`。
退一步:至少让"刚发布、还没到"与"没有这个包"在报错里能区分开。
`xlings` 已经有 `run \`xlings update\` if the package was just published`
这句提示 —— 方向是对的,只是 CI 里没有人能照做。

---

## 8. 已经做对、不要动的部分

写下来是为了避免后续 review 把它们"顺手改掉":

- **两条正交的轴**(§0)。这是整个设计的承重墙。
- **`msvc_wants_static_crt()` 单一推导**(`dialect.cppm:134`)。
  三处调用(`flags.cppm:613`、`flags.cppm:851`、`prepare.cppm:5023`)
  传的都是**manifest 原始标量**,所以编译器收到的 `/MT` 和分发表说的
  自包含不可能不一致。**不要**改成读解析后的 contract —— 那会让每个
  Windows 构建都翻成 `/MT`。
- **payload 多地址 + 单一 sha256**。镜像不是第二个信任根,是同一份字节的
  第二个地址。
- **能力驱动的 e2e 拆分**(`ci-windows-msvc-xlings.yml`)。用能力而不是文件清单
  分流,新测试靠声明 `# requires: xlings-msvc` 加入,没有第二份清单会走样。

---

## 9. 建议的落地顺序

| # | 项 | 规模 | 影响面 | 依赖 |
|---|---|---|---|---|
| 0 | **§3b doctor 用 `payload_frontend`** | **一行** | mcpp | 无。现成的 bug,先修 |
| 1 | §4 `installed()` 规则 + lint | 小 | xim-pkgindex | 无。最便宜,挡住的最多 |
| 2 | §5 索引 CI 真编译一次 | 小 | xim-pkgindex | 无 |
| 3 | §3 MSVC toolchain-coupled(含 `mcpp run` 的 PATH) | 中 | mcpp | 无。DLL 已在 payload 里 |
| 4 | §3c 拆开 `has_usable_msvc` | 小 | mcpp | 无 |
| 5 | §2 SDK 绑定 + 版本轴 | 中 | mcpp | 无 |
| 6 | §6 park-and-sweep 下沉 | 小 | xlings | 无 |
| 7 | §1 收拢 Origin + 消三处重复 | 大 | mcpp | 建议在 §2/§3c 之后 —— 都动 spec 层 |
| 8 | §3d PE 版 `pack` | 大 | mcpp | 排在 §3 之后:§3 先解决"能跑" |
| 9 | §7 索引修订号 | 大 | xlings + 索引 | 收益最分散 |

第 0–4 项互不依赖,可以并行,合计不大。§1 和 §3d 是两块真正的工作量,
建议各自单独一轮 —— 尤其 §1 动的是 spec 层,改完之后其余几项的 diff 会更小。

⚠️ §1 初稿的 `gcc@system` 提案已整段作废(mcpp#441 已关闭)——
那是我读错了设计意图凭空造出来的东西。xlings 是用户态 OS,
host 依赖是要**降到最低**的,不是要补齐的能力。

**如果只做三件**:§3b(一行)、§4(规则,挡住的最多)、§3(让干净 Windows
上 `mcpp run` 真的能跑)。

---

## 10. 一句话

这一轮九层缺陷,没有一层是"写错了"。全都是**验收判据比"能用"弱**:
`installed()` 只查 cl.exe、只查目录;`find_windows_sdk()` 只查头文件;
索引 CI 从不编译;e2e 只能 pass 或 skip。

把判据改成"能用"之后,它们一层层自己冒出来了。
上面七条建议,本质上是同一句话在七个位置的应用。
