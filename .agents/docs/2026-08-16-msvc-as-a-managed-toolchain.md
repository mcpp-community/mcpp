# MSVC 纳入 mcpp 工具链体系 —— 设计 + 验证方案(2026-08-16)

> **状态:已实现(2026.8.16.1)。** §6 的六个决策点全部有了答案,记在 §7;
> 其中**一条原方案的判断在实现时被推翻**,已在原处标注而不是悄悄改掉。
> 落地结果与实测见 §7,跨仓库的任务拆分与依赖见 §8。

**三条缺陷全部在 HEAD 上核实过源码,不是照抄 issue;
上游依赖(xlings 包)已经就位并在 Windows CI 上装通。**

对应 issue: #432。触发来源: [Sunrisepeak/xrgui#3](https://github.com/Sunrisepeak/xrgui/pull/3)
(用 mcpp 给 XRGUI 加 Windows 构建,已全绿——但代价是一个 workaround)。

| # | 缺陷 | 性质 | 改动面 | 是否阻塞 xrgui |
|---|---|---|---|---|
| A | `msvc` 是唯一不可声明版本的工具链 | 设计缺口 | 中 | **是** |
| B | `find_windows_sdk()` 写死两个绝对路径 | 可移植性 | 小 | 是(payload 化 SDK 后) |
| C | `cxx_runtime` 与 `linkage` 对静态 CRT 说法不一 | 接口一致性 | 小 | 否 |

A 与 B 合起来才有意义:A 让 mcpp 能拿到指定的 toolset,B 让它能拿到配套的 SDK。
C 独立,可并行。

---

## 0. TL;DR(给 review 的一页纸)

- **MSVC 是 mcpp 工具链体系里唯一的例外**:gcc / llvm 由 mcpp 自己装、按声明解析;
  只有 MSVC 走 `msvc@system` 去**探测宿主**,而那个探测**无法被调用方覆盖**。
- 后果不是「不够优雅」,是**同一份源码在两台机器上会被不同编译器编译,且不会报错**——
  xrgui#3 实测:同一轮 CI 里 mcpp 用 14.51、xmake 用 14.52,直到 14.51 ICE 才暴露。
- **上游已经准备好了**:`xim:msvc` / `xim:windows-sdk` / `xim:curl` 已发布,
  payload 布局刻意做成 mcpp 现状即可识别的形状,且 `xim:msvc@14.52.36629`
  正是 xrgui 需要的那个 toolset。
- 提案:**`msvc@<toolset>` 与 `gcc@<ver>` 同构**——非 system spec 走 `to_xim_package()`
  安装并解析;`msvc@system` 保留原义(用宿主已装的 VS)。
- **验证不靠新写的测试,靠 xrgui**:它是这套东西的真实用例,且已经绿了——
  验收标准是**删掉 workaround 之后仍然绿**,这是一个不能自证的判据。

---

## 1. 核实

### A. `msvc` 无法被指定 —— `src/toolchain/msvc.cppm`

发现顺序:

```
1. vswhere -latest -products * -requires ...VC.Tools.x86.x64
2. VSINSTALLDIR / VS*COMNTOOLS
3. Program Files\Microsoft Visual Studio\<year>\<edition>
```

两个事实叠加成缺口:

1. 第 1 步**没有 `-prerelease`**,所以 Insider 实例对它完全不可见;
2. 第 1 步一旦成功,第 2 步**不会执行**——而那是调用方唯一能控制的入口。

`find_vs_via_env()` 的判据本身很宽松,只要求目录存在:

```cpp
if (auto* dir = std::getenv("VSINSTALLDIR"); dir && *dir) {
    std::filesystem::path p{dir};
    if (std::filesystem::exists(p / "VC" / "Tools" / "MSVC"))
        return p;
}
```

**实测(xrgui#3)**:GitHub `windows-2025-vs2026` 镜像上同时存在

```
C:\Program Files\Microsoft Visual Studio\18\Enterprise\VC\Tools\MSVC\14.51.36231   ← 预装
C:\VS2026Insider\VC\Tools\MSVC\14.52.36629                                          ← 装的
```

同一轮 CI 里 mcpp 选 14.51、xmake 用 14.52。而 **14.51 编不了那个代码库**:

```
mo_yanxi_utility/src/utility/math/basic/vector2.ixx(67):
  fatal error C1001: Internal compiler error.
  (compiler file '...\CxxFE\sl\p1\c\template.cpp', line 26415)
  note: IFC import detected.
```

**导出完整 vcvars 环境无效**,也是实测的:多花约 7 分钟装 Insider,构建日志里的 cl
一个字都没变。最终只能在 CI 里**把 `vswhere.exe` 挪开**,逼 mcpp 落到第 2 步。
那个 workaround 现在还在 xrgui 的 workflow 里,是这份方案要消掉的东西。

### B. Windows SDK 路径写死 —— 同文件

```cpp
for (const char* base : {"C:\\Program Files (x86)\\Windows Kits\\10",
                         "C:\\Program Files\\Windows Kits\\10"}) {
```

无 `WindowsSdkDir` 覆盖、无注册表回退。toolset 那边没有对应问题:
`find_vs_via_env()` 只要求 `<VSINSTALLDIR>/VC/Tools/MSVC` 存在。

### C. 静态 CRT 有两个入口 —— 且注释互相矛盾

| 键 | MSVC 上的行为 |
|---|---|
| `[build] linkage = "static"` | **真的发 `/MT`** |
| `[build] cxx_runtime = "self-contained"` | **未实现**,warn 一次后退回 host-coupled |

- `src/build/flags.cppm:605` — *"/MD default, **/MT under static linkage**"*,且确实发了
- `src/build/distribution.cppm:202` — *"Under the MSVC runtime mcpp never made that promise
  — **there is no /MT emission at all**"*

`linkage` 那条实现得很扎实:`flags.cppm:611`(项目 TU)与 `prepare.cppm:4980`(std 模块)
共用同一个 `msvc_crt_flag()`——正是 #422 的修法,两边不可能再分叉。问题只在**入口有两个**,
而名字更像干这事的那个是无效的那个。

---

## 2. 上游已经就位(不需要 mcpp 侧配合的部分)

xim-pkgindex 现已发布三个包(#626 / #627 / #628 均已合入,Windows CI 实测装/卸载通过):

| 包 | 版本 | 说明 |
|---|---|---|
| `xim:msvc` | `14.44.35207`(latest)、**`14.52.36629`** | payload 包,多版本共存 |
| `xim:windows-sdk` | `10.0.26100` | ucrt/um/shared 头 + um 库 + rc/mt |
| `xim:curl` | `8.21.0` | 下载器本身也在生态内 |

**payload 布局是刻意照着 mcpp 现状做的**:

```
<store>/xpkgs/xim-x-msvc/14.52.36629/
└── VC/Tools/MSVC/14.52.36629/{bin/Hostx64/x64/cl.exe, include/, lib/x64/, modules/std.ixx}

<store>/xpkgs/xim-x-windows-sdk/10.0.26100/
├── Include/10.0.26100.0/{ucrt,um,shared}/
└── Lib/10.0.26100.0/{ucrt,um}/x64/
```

即 **toolset 那一半 mcpp 今天就能识别**(把 `VSINSTALLDIR` 指过去即可,前提是缺陷 A 修掉);
**SDK 那一半够不到**,因为缺陷 B。

---

## 3. 方案

### 3.1 A —— 让 `msvc@<toolset>` 与 `gcc@<ver>` 同构

现状(`src/toolchain/lifecycle.cppm:503`):

```cpp
// msvc@system: mcpp never installs MSVC — report what's there, or
// print installation guidance.
if (mcpp::toolchain::is_system_toolchain(*spec)) { ... return 1; }
```

`is_system_toolchain()` 对**所有** msvc spec 为真,所以 `msvc@14.52.36629` 也走这条死路。

**提案:按 spec 是否带具体版本分流**

| spec | 语义 | 解析 |
|---|---|---|
| `msvc@system` | 用宿主已装的 VS(**保留现状,含探测顺序**) | `detect_installation()` |
| `msvc@<toolset>` | 由 mcpp 安装并使用指定 toolset | `to_xim_package()` → `xim:msvc@<toolset>` |

改动集中在三处:

1. `is_system_toolchain()` 只在 version 为 `system`/空时为真;
2. `to_xim_package()` 增加 msvc 行(`xim:msvc`,版本原样);
3. 解析已安装 payload 时,`VC/Tools/MSVC/<version>` 直接由包版本拼出——
   **不再探测**,因为版本是声明的。

> **为什么这比"给 vswhere 加 `-prerelease`"更值得做**:后者只是让探测更可能猜对。
> 而问题的本质是「这份构建用了哪个编译器」不该由机器状态回答。xlings V2 spec 的
> R3 把这条写成了规则:*"If a fix ADDS a path rather than REMOVING one, it is a
> workaround. Making two independent answers more likely to agree is not a fix."*
>
> 不过 `-prerelease` 仍建议顺手加上:它让 `msvc@system` 在装了 Insider 的机器上
> 至少能看见它。这是**兼容改进**,不是缺陷 A 的答案。

**兼容性**:`msvc@system` 行为不变,现有工程零影响。新增的是一条以前会报错的路径。

### 3.2 B —— `find_windows_sdk()` 认 `WindowsSdkDir`

顺序改为:

```
1. WindowsSdkDir + WindowsSdkVersion   (vcvars 本来就导出这两个)
2. 已解析 msvc 包的 windows-sdk 依赖   (走 3.1 的路径时)
3. 现有的两个绝对路径                   (回退)
```

第 2 条是 3.1 的自然延伸:`xim:msvc` 声明了 `deps = { "xim:windows-sdk@10.0.26100" }`,
所以走包路径时 SDK 的位置是**已知的**,不必再探测。

### 3.3 C —— 消掉第二个入口

二选一:

- **(推荐)** `cxx_runtime = "self-contained"` 在 MSVC 上直接映射到静态 CRT,
  与 `linkage = "static"` 走同一个 `msvc_crt_flag()`;
- 或保持未实现,但把诊断改成明确指向 `linkage`。

无论哪个,`distribution.cppm:202` 那句 *"there is no /MT emission at all"* 都要改——
它与 `flags.cppm` 的实现直接矛盾。

---

## 4. 验证方案 —— 用 xrgui,不用新造的测试

xrgui#3 是这套东西的**真实用例**,而且**它已经是绿的**。这一点很重要:
验证不是"让它变绿",而是**在删掉 workaround 之后它是否仍然绿**——
一个不能靠调整测试来自我满足的判据。

### 4.1 基线(今天的状态)

```
.github/workflows/mcpp-windows.yml
  ├─ 装 VS 2026 Insider 到 C:\VS2026Insider        ~7 min
  ├─ 直接从该路径导出 vcvars(不经 vswhere)
  ├─ 把 vswhere.exe 挪开          ← workaround,本方案要消掉的
  └─ 断言 mcpp 解析到 14.52,否则立刻失败
```

两条腿(mcpp / xmake)全绿,`xrgui_tests` 54 项通过。

### 4.2 验收步骤

| 阶段 | 动作 | 通过标准 |
|---|---|---|
| V0 | 不改 xrgui,先在一台装了 VS 的机器上 `mcpp toolchain install msvc 14.52.36629` | payload 落在 `xpkgs/xim-x-msvc/14.52.36629`,`mcpp why toolchain` 报告它 —— **且宿主的 VS 仍在**(证明不是靠探测碰巧对) |
| V1 | xrgui 的 `mcpp.toml` 改 `[toolchain] windows = "msvc@14.52.36629"` | `mcpp why toolchain` 报告 14.52.36629 |
| V2 | **删掉 workflow 里「挪开 vswhere.exe」那一步** | 仍解析到 14.52.36629 —— 这是缺陷 A 修复的**唯一有效证据** |
| V3 | 删掉 Insider 安装与 vcvars 导出两步 | mcpp 自己装 toolset + SDK;`mcpp build --features tests` 通过,`xrgui_tests` 仍 54/54 |
| V4 | 对比 xmake 腿 | 两条腿仍然只差构建系统 —— 但**注意**:xmake 仍需 Insider 安装,V3 不能删它需要的那部分 |

**V2 是关键**:workaround 还在的时候,缺陷 A 是否修好无法区分——
`VSINSTALLDIR` 被采纳和 vswhere 找不到东西,现象一样。

### 4.3 反向验证(容易被忽略)

- **`msvc@system` 未回归**:在同一台机器上把 spec 换回 `msvc@system`,
  应仍解析到宿主的 VS(而不是刚装的 payload)。两条路径必须互不污染。
- **版本切换真的生效**:`xlings use msvc 14.44.35207` 后重新构建,
  `cl` 报告的版本随之改变。多版本不是摆设。
- **SDK 来自包而非宿主**:装完后临时重命名 `C:\Program Files (x86)\Windows Kits`,
  构建应仍然成功(缺陷 B 修好的判据)。这条在 CI 上做比在本机安全。

### 4.4 已知不被覆盖的部分

- **`14.52` 的安装路径 CI 没跑过**。xim-pkgindex 的 `windows-test` 装的是 `latest`(14.44)。
  两者差异只在 payload URL 与目录版本,后者已逐个从真实 payload 读出核对
  (14.44 的三个 payload 版本 35228/35220/35226 都解到 `35207`;14.52 的 payload
  与目录一致,都是 `36629`),但**没有实际装过一次**。V0 会第一次覆盖它。
- **Insiders payload 会轮换** —— 但这条正在被消掉,见 §4.5。

### 4.5 镜像:把"地址会失效"变成"地址锁定"

原本这份方案把 Insiders 的轮换列为已知风险:14.52.36629 下架时 URL 404。
**这一点可以直接消掉,而不是接受**——review 反馈给的方向是镜像到 gitcode。

关键在于:**镜像的是同一份字节,所以 sha256 不变**。于是 payload 条目从

```lua
{ name = "...", sha256 = "...", url = "https://download.visualstudio.microsoft.com/..." }
```

变成一个**来源列表**,校验规则完全不动:

```lua
{ name = "...", sha256 = "...",
  urls = { "https://gitcode.com/xlings-res/msvc/.../<name>",      -- 镜像优先
           "https://download.visualstudio.microsoft.com/..." } }  -- 官方回退
```

`fetch_verified()` 依次尝试,**无论从哪个来源取到,都按同一个 sha256 校验**。
所以镜像不是新的信任源——它只是同一份字节的第二个地址。取不到就换下一个,
两个都失败才报错。

这比 `XLINGS_RES` 的 res 形状更合适:后者的自动 URL 约定是
`{name}-{version}-{os}-{arch}.{ext}`,而 MSVC 一个版本是**一组**文件、各自带着
微软自己的文件名(SDK 那边光 CAB 就 15 个)。而 payload 表本来就是 recipe 自己解析的,
加一个来源列表不需要框架配合。

**要镜像的量**(已实测):

| 内容 | 文件数 | 体积 |
|---|---|---|
| `msvc@14.44.35207` | 4 个 vsix | 83.5 MB |
| `msvc@14.52.36629` | 4 个 vsix | 102.4 MB |
| `windows-sdk@10.0.26100` | 4 MSI + 15 CAB | 139.1 MB |
| 合计 | 27 | **325 MB** |

**边界要说清楚**:把微软的编译器二进制放到第三方主机,与"安装时从微软 CDN 下载"
是两件不同的事——前者是再分发。这是维护者的决定,不是技术选择;本文只描述机制。
上传本身需要 gitcode 凭据,不在本方案的执行范围内。

**落地拆两步**,因为它们的风险完全不同:

1. **recipe 侧支持多来源**(纯代码,可先合):`urls` 列表 + 依次回退,
   单来源时行为与今天完全一致 → 零风险,且为镜像就绪;
2. **实际上传 + 填入镜像 URL**:需要凭据与上面那条边界的决定。

第 1 步做完之后,即使一个镜像都还没有,这套东西也不会变差;而一旦 14.52 真的下架,
补一行 URL 就能恢复,不必重新找一个还活着的 toolset 版本再验证一遍。

---

## 5. 落地顺序

```
C(独立,小)  ──────────────────────────────┐
                                          ├─→ 发布 ─→ xrgui V1..V4
A(核心) ─→ B(依赖 A 的包路径) ─→ V0 ─────┘
```

- C 可以随时单独落,不阻塞任何人;
- A 不修,B 修了也没用(SDK 找得到,编译器还是错的);
- V0 应在 A+B 发布**之前**用本地构建跑一次,否则 xrgui 那边的红会同时有两个可能来源。

---

## 6. 待 review 决策点(已全部拍板,答案见 §7.1)

1. **`msvc@<toolset>` 的语义**是否如 3.1 所提(按版本分流,`@system` 保留原义)?
2. `-prerelease` 是否顺手加上(改善 `msvc@system`,不替代 A)?
3. C 选哪一个:让 `cxx_runtime` 生效,还是把诊断指向 `linkage`?
4. B 的第 2 条(从已解析的包依赖里取 SDK)是否值得做,还是只做 `WindowsSdkDir` 就够?
5. 验证方案里 4.3 的三条反向验证是否都要进 CI —— 尤其"重命名 Windows Kits"那条,
   它最有说服力,也最容易在别的 job 上产生副作用。
6. **镜像(§4.5)**:recipe 侧的多来源支持可以先合(零风险);实际镜像涉及再分发,
   需要维护者拍板,且上传需要 gitcode 凭据。是否现在就做第 1 步?

---

## 7. 落地结果(2026.8.16.1)

### 7.1 六个决策点的答案

| # | 决定 | 落地方式 |
|---|---|---|
| 1 | **按版本轴分流,`@system` 完全保留** | `is_system_toolchain()` 加版本判据;`msvc@system` 的语义与探测链一字未改(只有顺序修了,见 2) |
| 2 | **加** | vswhere 加 `-prerelease`,但顺序也变了 —— 见 §7.2 那条被推翻的判断 |
| 3 | **让 `cxx_runtime` 生效** | 两个键都走 `msvc_wants_static_crt()`;`distribution.cppm:202` 那句自相矛盾的注释删掉 |
| 4 | **做,但不是「从包依赖里取」** | 改成从**编译器自己的路径**反推 store —— 见 §7.2 |
| 5 | **不进 CI** | 见 §7.3 |
| 6 | **两步一起做了** | recipe 侧多来源 + 27 个 payload 实际镜像完成并逐个校验(xim-pkgindex#629) |

### 7.2 两条实现时改掉的判断

**① 缺陷 B 的第 2 条来源不是「已解析的包依赖」,而是编译器自己的位置。**

原方案说:走包路径时 `xim:msvc` 声明了 `deps = { "xim:windows-sdk@10.0.26100" }`,
所以 SDK 的位置是已知的。能做,但它要求 **mcpp 侧知道那个依赖的名字和版本** ——
把 SDK 版本写进 mcpp,而它本该只是包的事。

实际做法:`sibling_sdk_roots(clPath)` 用已有的 `xpkgs_from_compiler()` 从 cl.exe
的路径反推出它所在的 store,再取 `xim-x-windows-sdk/*`。**编译器自己的路径就说明了
它来自哪个 store**,SDK 是它在那里的邻居。零配置、零版本硬编码,而且对
`msvc@system` 自动返回空(系统 cl 不在任何 store 里)。

**② `VSINSTALLDIR` 与 vswhere 的顺序,原方案说得不够。**

原方案把 `-prerelease` 列为「兼容改进,不是缺陷 A 的答案」,这是对的;
但它没说**顺序本身就是缺陷**。vswhere 在几乎每台开发机上都返回点什么,所以
`VSINSTALLDIR` 事实上不可达 —— 这正是 xrgui 那个 workaround 存在的原因,而
workaround 是要被删掉的。所以顺序改成:

```
VSINSTALLDIR → vswhere(-prerelease) → VS*COMNTOOLS → 绝对路径
```

`VS*COMNTOOLS` 留在 vswhere **之后**,这一点是新的:它们是机器全局的残留
(2017 的 `VS150COMNTOOLS` 不该压过当前安装),而 `VSINSTALLDIR` 是有人为这个
shell 设的。原来的 `find_vs_via_env()` 把两者混在一起,提前它就会连带提前残留。

### 7.3 4.3 的三条反向验证:两条进了 e2e,一条没进

| 反向验证 | 结论 |
|---|---|
| `msvc@system` 未回归 | **进了** —— `239_msvc_managed_toolset.sh` 第 3 步:同一台机器、同一个项目,spec 换回 `msvc@system` 必须解析到系统的 cl。没有这条,「受管能用」与「受管把一切换掉了」这两种结果长得一模一样。 |
| 版本切换真的生效 | **进了(以更强的形式)** —— 单测 `ResolvesTheDeclaredToolsetAndNotItsNeighbour`:两个 toolset 都在,要**老的**那个。「取最新」的实现会在这里失败,而在真机上它可能碰巧对。 |
| 重命名 `Windows Kits` | **没进**。它确实最有说服力,但它会让同一个 runner 上**其它 job** 的 MSVC 构建随机失败,而 CI 上的 job 隔离不到目录改名这一层。改由单测覆盖:`ExtraRootsCoverTheManagedPayload` 在没有任何 env、没有任何 Windows Kits 的 Linux 上跑 —— 这比重命名更彻底,因为那台机器上**本来就没有** SDK 可以被找到。 |

### 7.4 单测第一次能在 Windows 之外跑

改动前,msvc 的发现逻辑**一行都无法在 Linux 上测试**:每个入口都从探测机器开始,
而探测在 `#if defined(_WIN32)` 里。

`installation_at()` 接受目录而不是探测机器,`find_windows_sdk()` 接受 root 列表,
两者都不再被平台宏包住 —— 于是 fixture 目录树就能驱动真实代码路径。新增 6 个
单测在 Linux CI 上跑,其中三条是这次改动的核心判据:

```
MsvcManaged.ResolvesTheDeclaredToolsetAndNotItsNeighbour   两个都在,要老的
MsvcManaged.AbsentToolsetIsNulloptNotASubstitute           缺了就是缺了,不替换
MsvcSdk.DeclaredRootOutranksTheManagedOne                  声明压过探测
```

`test_toolchain_msvc` 25 项全绿,全套单测 83/83。

---

## 8. 跨仓库任务拆分与依赖

四个仓库,三条可并行的链。边是**真实依赖**,不是先后偏好:

```
xim-pkgindex #629  ──────────────┐  (包必须先发布,mcpp 才装得到)
  多来源 urls + 27 payload 镜像    │
  windows-sdk 导出 WindowsSdkDir   │
                                  ▼
mcpp #4xx (2026.8.16.1) ────→ 发布 ────→ xrgui:删 workaround + 改 spec
  A 受管 toolset                            (V2 是唯一不能自证的证据)
  B SDK 搜索顺序
  C CRT 双入口         ← 与 A/B 无依赖,可并行
  D 发现顺序           ← 与 A 同文件,顺序上一起改
```

**为什么 C 和 D 仍然放在同一个 PR 里**:C 与 A/B 确实无依赖,但它改的是同一个
「MSVC 在 mcpp 里到底怎么被描述」的问题,分开发会让 CHANGELOG 读者以为是两件事;
D 与 A 改同一个函数,分开发第二个 PR 必然要重写第一个 PR 刚写的注释。

**唯一的硬依赖**是 xim-pkgindex → mcpp:`mcpp toolchain install msvc 14.44.35207`
装的就是那个包。所以 #629 必须先合并并发布,`239_msvc_managed_toolset.sh` 才
可能通过 —— 在此之前它会走 SKIP 分支(索引取不到时干净跳过),而不是红。
