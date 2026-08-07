# 两处「模型比生态少一层」:Windows 资源输入 与 版本身份

> 状态：**已实施（2026.8.7.1）**。剩余待决的 4/5/6 按文档推荐执行，可回退，见 §D。
> 实施中发现的三条见 §E。
> 关联：[#365](https://github.com/mcpp-community/mcpp/issues/365)（Windows 资源编译）、
> [#363](https://github.com/mcpp-community/mcpp/issues/363)（版本模型）
> 涉及：`src/build/{plan,prepare,ninja_backend,flags}.cppm`、
> `src/manifest/{types,toml}.cppm`、`src/toolchain/{model,dialect,llvm,gcc,msvc}.cppm`、
> `src/version_req.cppm`、`src/pm/{resolver,lock_io}.cppm`、`src/build/execute.cppm`

---

## 0. 为什么写在一份文档里

两个 issue 领域无关，但失效形状同一条：**生态已经产出的东西，mcpp 的模型表达不了，于是走到一条「不报错但结果是错的」路径上。**

| | 生态产出 | mcpp 的模型 | 用户实际遭遇 |
|---|---|---|---|
| #365 | Windows 应用带 `.rc`（图标、VERSIONINFO） | 链接期输入只有「对象文件」和「不透明 ldflags 字符串」 | 只能把预编译 `.res` 塞进 ldflags；改图标后 `ninja: no work to do` |
| #363 | 上游发 `b10069`、`1.92.8-docking` | 版本 = 4 个整数，字面键被丢弃 | `^1.92.8` 在两个不同 tarball 之间任选一个；lock 记的是约束本身 |

两条都不是「少个 feature」，是**表达力缺口导致的静默错误**。本仓库反复付过这个学费（假 `Cached` 骗了三个月、`.mcpp_ok` 只证进程退 0、索引下限把旧客户端变砖），处理原则一致：**要么给出正确答案，要么给出指名的错误，不要第三种。**

除此之外两者有一处真实交汇（不是硬凑）：`version_req::Version` 同时是 #363 的排序依据 和 #365 生成 `FILEVERSION` 所需的 4 元组来源。#363 把「字面 / 序」分开之后，#365 正好取它的数值侧。**Part A 与 Part B 可各自独立发布**，只要 A 落在 B 之后（或 A 自己解析一次版本号，成本一行）。

---

# Part A — #365：Windows 资源编译

## A1. 关键发现：VERSIONINFO 读不到，不是 llvm-rc 的 bug

issue 结尾要求作者注意「llvm-rc 生成的 VERSIONINFO 结构不被 GetFileVersionInfo 解析」，并建议 mcpp 侧做专门处理或换编译器。**这条归因是错的，实测已证伪。**

用 issue 里那份 `.rc`（`VS_VERSION_INFO VERSIONINFO`，未 include 任何头），以及把首 token 换成字面 `1` 的版本，分别用 `llvm-rc 22.1.8` 编译，比对 `.res` 的资源头：

```
# VS_VERSION_INFO VERSIONINFO  →  508 字节
ff ff 10 00  56 00 53 00 5f 00 56 00 45 00 52 00 ...
^type=0xFFFF,16 ^name = UTF-16 字符串 "VS_VERSION_INFO"

# 1 VERSIONINFO                →  480 字节
ff ff 10 00  ff ff 01 00
^type=0xFFFF,16 ^name = 序号 1
```

`VS_VERSION_INFO` 是 `verrsrc.h` 里的宏（`#define VS_VERSION_INFO 1`），随 `windows.h` 引入。**没有 include 它时，rc 语法允许标识符出现在资源名位置，于是它被当成资源名字符串**——资源类型仍是 RT_VERSION(16)，所以 `llvm-readobj --coff-resources` 照样显示 `Type: VERSIONINFO`；但 `GetFileVersionInfo` 查的是 `MAKEINTRESOURCE(VS_VERSION_INFO)` 即**序号 1**，查不到，所有字段返回空。报告者观测到的每一条都对上了。

两种修法都实测通过，产出与 `1 VERSIONINFO` **字节相同**（480 字节，`ff ff 01 00`）：

```
llvm-rc -D VS_VERSION_INFO=1 /fo out.res in.rc      # 命令行定义
# 或在 .rc 顶部写 #define VS_VERSION_INFO 1 / #include <windows.h>
```

同时实测清了 llvm-rc 的能力边界（`llvm-rc /?`）：**默认就做预处理**（有 `/no-preprocess` 才关），支持 `/I` 加 include 路径、`/D` 定义宏；**没有任何 depfile 选项**。所以「llvm-rc 不认常量」也不是 llvm-rc 的缺陷，是 `.rc` 里没有 include，预处理器无从展开。

**对设计的三条影响**

1. mcpp 不需要绕开任何工具 bug。合成 `.rc` 时写字面 `1 VERSIONINFO`，构造性正确。
2. 用户自写 `.rc` 要能 `#include <windows.h>`，所以 mcpp **必须把目标的 SDK / mingw include 目录喂给 rc 工具**（`/I`，MSVC 走 `INCLUDE` 环境变量——`Toolchain::envOverrides` 已经在填它）。
3. **不注入 `-DVS_VERSION_INFO=1`。** 那是拿 mcpp 去局部模仿 Windows SDK：报告者接下来还缺 `VOS_NT_WINDOWS32`、`VFT_APP`……每补一个就多一处与真 SDK 漂移的定义。正解是让真的 `windows.h` 可达。作为补偿，见 A5 的 lint。

## A2. 现状：三条通道，没有一条能表达「被跟踪的链接期输入」

| 通道 | 现状 | 为什么不行 |
|---|---|---|
| `[build].ldflags` | `ninja_backend.cppm:825` 把 `$ldflags` 整串拼进 link 命令 | ldflags 是不透明字符串，**没有任何一处从里面析出文件路径当 implicit input**。`.res` 改了 ninja 不知道 → issue 报的 `no work to do` |
| `[build].sources` | `.rc` 不在 `is_implementation_source` / `is_c_source` / nasm / gas 任一分派里（`plan.cppm:299`、`ninja_backend.cppm:244`） | 会被当成 C++ 翻译单元进模块图与编译集 |
| `build.mcpp` 的 `action{}` | `BuildAction::Role` 三个值：`Source`（产出进**编译**集）、`Check`（产出是 stamp）、`Artifact`（**输入是 link 产物**） | 三条接线分别接在「编译输入 / 无 / 链接输出」上。**「链接输入」这个接线点在角色表里不存在。** |

第三行是架构级的：`types.cppm:219` 明说「role 不是三种机制，是同一条边的三种接线」。资源编译正好证明这张表**缺一格**——link 边是有输入有输出的节点，而没有任何角色能把产物接到它的输入上。同类需求还有：`objcopy` 嵌入二进制 blob、`.def`/`.exp`、外部生成的 `.o`、linker script。

> 换句话说：#365 不修，用户就只能继续用 ldflags 塞路径——而 ldflags 塞路径**正是 issue 抱怨的那个不被跟踪的东西**。表达力缺口和它逼出的坏 workaround 是同一件事。

## A3. 设计：三层，每层是下一层的默认值

分层控制的判据来自 grpcgen 那批的教训：**断崖本身就是设计缺陷**——两个旋钮之后只能手写六十行绕开规则，而那六十行会与规则悄悄漂移。所以三层必须能连续下降，L0 的产物就是 L1 的输入。

### L0：声明式（覆盖 90% 场景，零 `.rc` 编写）

```toml
[resources]
icon = "assets/app.ico"

[resources.version-info]        # 全部可选，默认从 [package] 取
company     = "…"               # 默认 authors[0]
product     = "…"               # 默认 package.name
description = "…"               # 默认 package.description
copyright   = "…"               # 默认 "© " + authors[0] + "，" + license
```

`[package]` 已有 `name / version / description / license / authors / repo`（`types.cppm:37`），足够生成一份合法 VERSIONINFO。`FILEVERSION` / `PRODUCTVERSION` 取 `version` 的 4 段数值（`0.2.0` → `0,2,0,0`；`2026.8.6.3` → `2026,8,6,3`），每段 clamp 到 u16 并在越界时报错而不是截断；`StringFileInfo` 里的 `FileVersion` / `ProductVersion` 写**版本字面串**（这样 `1.0.0-rc1` 这类预发布信息不丢——见 Part B）。

mcpp 把合成的 `.rc` 写到 `target/<triple>/<fp>/resources/<pkg>.rc`，编译，产物挂到该包每个 PE link unit 的输入上。

### L1：自写 `.rc`

```toml
[resources]
files = ["res/app.rc"]
extra-inputs = ["res/dialogs.h"]   # 兜底：扫描没认出来的输入
```

mcpp 编译并**跟踪**它：`.rc` 自身 + 从 `.rc` 文本扫出的 `#include "…"` 与资源语句里的引号文件名（`ICON "x.ico"`、`24 MANIFEST "app.manifest"`、`RCDATA`、`BITMAP` …）都进 implicit inputs。llvm-rc/windres 都不给 depfile（A1 实测），所以这里只能扫，因此配一条 `extra-inputs` 显式兜底——与 `[modules].scan_overrides` 同一个「发现不够时改成声明」的先例。

**合成与自写的交互，一张 3 行表**（不做文本解析猜测，规则显式）：

| `version-info` | `files` | 行为 |
|---|---|---|
| 未写 | 空 | 合成（L0） |
| 未写 | 非空 | **不合成**——用户接管了资源 ID 空间 |
| `= true` | 非空 | 合成；ID 冲突由用户负责（文档写明 RT_VERSION 只能有一个 id 1） |

`version-info = false` 恒不合成。

**L0 → L1 没有断崖**：合成的 `.rc` 落在 `target/` 下一个稳定路径，用户 `cp` 进自己的树、填进 `files`，得到**字节相同**的资源。这条要作为判据机器化验证（A-判据 6），否则「每层是下一层的默认值」只是文档里的一句话。

### L2：通用原语 —— `action` 的第四个 role

给 `BuildAction::Role` 补上 `Object`：**产出接到 link 边的输入上**。角色表变成完整的四格：

```
Source   → 编译输入
Check    → 无（stamp）
Object   → 链接输入      ← 新增
Artifact → 链接输出之后
```

这不是为资源加的钩子（钩子数是乘法成本，`build-mcpp-extensibility-architecture` 那批已经定过调）——L0/L1 走的是引擎自己的 rc 边，不经过 action。`Object` 是**补齐角色表**，顺带让 ldflags 塞路径这类 workaround 全类退休。

**已决：本批做。** 不做的话「不被跟踪的 ldflags 路径」仍是唯一出路，等于留着这个 issue 的成因。

`Object` 的产物接到**哪个** link unit：`Artifact` 是靠 `${mcpp.target_file:NAME}` 出现在 inputs 里反推的，`Object` 反不了（它的产物在 link 之前，没有 link 产物可引用）。定为给 `BuildAction` 加一个可选 `targets = [...]`——空 = 声明它的那个包的所有 link unit，与 L0/L1 的默认作用域一致。`targets` 里出现未知目标名时报错并列出本次构建的目标，复用 `${mcpp.target_file:}` 已有的那条诊断（`prepare.cppm:4941`）。

## A4. rc 工具解析：dialect × payload 的实测矩阵

**绝不走 PATH。** `~/.mcpp/registry/subos/default/bin/x86_64-w64-mingw32-windres` 现在是个指向 `bin/xlings` 的符号链接——xlings 的裸名 shim 机制，最后注册者抢名（已经弄坏过一次真实工具链：`gcc --version` 全对而产物是 ARM）。工具必须**相对编译器二进制所在 payload 解析**，先例是 `clang::find_scan_deps(tc)`（`prepare.cppm:4958`）。

| dialect / 目标 | rc 工具 | 产物 | 链接器接受 |
|---|---|---|---|
| MSVC（`rc.exe` / `link.exe`） | `rc.exe`（SDK；include 走 `envOverrides` 里的 `INCLUDE`） | `.res` | link.exe 直接吃 `.res` |
| clang + lld-link（Windows 原生） | `llvm-rc`（payload 自带） | `.res` | lld-link 直接吃 `.res` |
| GNU / mingw（`x86_64-w64-mingw32-g++` + ld） | `windres -O coff` | COFF `.o` | ld 吃对象；**ld(bfd) 不吃 `.res`** |

**实测到的 payload 缺口**：Linux 上的 `xim-x-llvm/22.1.8/bin` 只有 `llvm-rc` 和 `llvm-readobj`，**没有 `llvm-windres`、没有 `cvtres`**（Windows 的 20.1.7 payload 才有 `llvm-windres.exe`）。所以：

- Linux → Windows 走 **mingw 交叉链**（mcpp 现在的交叉路径）：`x86_64-w64-mingw32-windres` 在 mingw payload 里，没问题。
- Linux → Windows 走 **clang + lld** ：只有 `llvm-rc` 能产 `.res`。**`ld.lld` 的 mingw 模式是否接受 `.res` 需实测**（VERIFY-A3）。接受则这条路直接通；不接受则必须从 mingw payload 借 `windres`，此时要给出指名的错误。

解析时机与失败策略照抄 nasm（`prepare.cppm:4964-5031`）：**惰性**——只有 plan 里真的有资源单元才解析；**硬失败**——找不到工具就报错并指名工具与 dialect，绝不静默跳过（掉一个 `.o` 会以「图标没了」或几层之外的 undefined 现形）。

## A5. 增量、隔离、与那条 lint

- **构建图**：新增 `plan.resourceUnits`（源 `.rc`、输出、implicit inputs），backend 出一条 `rc_object` 规则，输出 append 到对应 `LinkUnit::objects`。**不进 `CompileUnit`**——那会把 `.rc` 拖进模块图、topo 序、`compile_commands.json`（clangd 会当场噎住）和缓存键。
- **与依赖缓存零交互**：资源只由「拥有 link unit 的那个包」声明，root 包永不进全局缓存。依赖声明的资源**不传播**（一个依赖的 VERSIONINFO 和消费者的会打架）；非 root 包声明了资源但自己不产 PE link unit → 警告并忽略。
- **非 PE 目标**：整节**不适用**（不是降级）。不需要 `cfg(windows)`——而且**不能**走那条通道：`[target.'cfg(…)'.build]` 的 `kKnownConditionalBuildKeys`（`toml.cppm:1174`）是封闭表，且「条件通道只载 BuildInputs，一条轴一套作用域规则」是明文设计立场，图标/版本元数据不是 build input，塞进去会被 schemaWarnings 拒掉。用户无条件写一次即可。
- **可见性补偿**（§D-3 的义务）：PE 目标下 `[resources]` 生效时打一行 status（`Embedding   app.ico + version info`）；非 PE 目标不打也不警告。节名里既然看不出平台，就得让「它这次生效了」出现在输出里——顺带让判据 A2 的增量行为可观测。
  > 这里**偏离 issue 的第 3 条要求**。issue 要「资源文件缺失时跳过，不应导致构建/打包失败」。拆成两件事：跨平台不炸由「非 PE 目标不适用」解决；而**声明了却不存在的文件是硬错误**——与 `main = "…"` 必须匹配恰好一个文件同一条规则。静默跳过一个声明过的输入，等于让「图标为什么没了」变成不可归因的问题，正是这个 issue 的起点。
- **lint（补偿 A1 第 3 条）**：自写 `.rc` 里出现 `VERSIONINFO`，其名字 token 既不是字面 `1`、文件里又没有 `#include` 也没有 `#define VS_VERSION_INFO` 时，给一条 warning 并附确切修法。这是启发式（用户可能在别处定义了宏），所以只 warn；它把本 issue 里那个**完全无法自行诊断**的失效变成一行可读的话。

## A6. 明确不做

- **`subsystem` / `entry`**：报告者也在用 `-Wl,-subsystem:windows -Wl,-entry:mainCRTStartup`。那是链接模式，不是资源，属于另一条轴（`[target.<triple>].linkage` 那一层）。本设计不动它，现有 ldflags 写法继续有效——但它在**同一个「Windows GUI 应用」的用户故事**里，值得单独 triage。
- **macOS bundle / Info.plist / `.desktop`**：`[resources]` 的语义定为「编译进产物的元数据与资产」，将来这些扩展**同一节**而不是新开 `[macos]`；本批只实现 PE 消费者。
- **对话框、字符串表等资源类型**：mcpp 不解析 `.rc` 语义，L1 原样交给 rc 工具。

## A7. 判据（可机器验证）

1. **A1**：Windows 目标 + `[resources] icon=…` 产出的 exe，其 RT_VERSION 资源名是**序号 1**（按字节验，不是「存在一个版本资源」），且 `FileVersionInfo::GetVersionInfo` 的 ProductName / FileVersion 非空。
2. **A2**：动 `.ico`、动 `.rc`、动 `[package].description` 三者任一 → ninja 重链；都不动 → no-op。（原症状是 `no work to do`。）
3. **A3**：同一份 manifest 在 Linux/macOS 构建**逐字节不变**，零 warning。
4. **A4**：`.res`/`.o` 不出现在 `compile_commands.json`，不出现在模块图。
5. **A5**：rc 工具不可用 → 错误消息含工具名与 dialect；e2e 用「把 payload 里的 windres 改名」构造。
6. **A6（无断崖）**：把合成的 `.rc` 复制进源码树并填入 `files`，产出资源**字节相同**。
7. **A7**：`.rc` 里 `#include` 的头改动 → 重编（验证扫描确实进了 implicit inputs）。

## A8. 待实测（VERIFY）

- **VERIFY-A1**：mingw payload 的 `windres -O coff` 产物在 mcpp 的交叉链里能被 ld 正常收进 `.rsrc`（本机当前没装 mingw 交叉 payload）。
- **VERIFY-A2**：`rc.exe` 在 mcpp 的 MSVC 路径下能通过 `envOverrides.INCLUDE` 找到 `windows.h`。
- **VERIFY-A3**：`ld.lld` 的 mingw 模式是否接受 `.res` 输入（决定 clang+lld 交叉是否需要借 windres）。
- **VERIFY-A4**：CI 覆盖。Windows job 必须真的跑 `FileVersionInfo` 读取——「exe 里有 VERSIONINFO 资源」这个断言在本 issue 的失效场景下**恒为真**，是假绿（与 E0006 那次「断言出现某错误 = 假绿」同一形状）。

---

# Part B — #363：版本身份

## B1. 关键发现：解析器已经读到了字面键，然后把它扔了

`resolver.cppm:127-154`：

```cpp
auto rawVersions = mcpp::manifest::list_xpkg_versions(*luaContent, platform);  // 字面键，全在手里
std::vector<vr::Version> parsed;
for (auto& s : rawVersions) {
    auto v = vr::parse_version(s);
    if (!v) continue;                 // ← 不可排序的键静默消失，且此后下标不再对齐
    parsed.push_back(*v);
}
auto idx = vr::choose(*req, parsed);
return parsed[*idx].str();             // ← 从 4 个整数重新渲染出一个地址
```

`list_xpkg_versions` 返回的是描述符里的**字面 key**（`xpkg.cppm:880`，就地取引号内文本）。也就是说：**正确答案一直在 `rawVersions[*idx]` 里，代码却从 `parsed` 里倒推了一个。** `str()` 只能渲染 3–4 段数字，于是任何非纯数字键在范围路径上**永远寻址不到**——不是「不支持」，是「先丢掉再重造」。

这解释了 issue 的全部三条现象，包括为什么精确路径完全正常：精确路径（`is_version_constraint` 为 false）根本不经过这段代码，字面串直接进 wire 地址。

`continue` 那行还额外制造了**下标错位**：`parsed` 与 `rawVersions` 长度不同，而 `choose` 返回的是 `parsed` 的下标。今天没暴露只因为返回值不再用 `rawVersions`。修的时候必须成对保存，不能只把 `parsed[*idx].str()` 换成 `rawVersions[*idx]`。

**血缘**：`str()` 头上那段注释明写「load-bearing：pm/resolver 把它当解析结果返回，流向 lock 与 wire 地址——必须复现索引的字面版本键」。这个约束**已经被写下来了，但只用日期版本的 `.0` 尾段验证过**；本 issue 是同一约束在预发布/非 semver 上的第二次违约。真正的修法不是再加一条注释，而是**让「重新渲染出地址」这件事在类型上不可表达**。

## B2. 三个症状，两个根因

issue 列了三条，我把归因拆开：

| 现象 | 根因 |
|---|---|
| ① 精确键（含 `b10069`、`1.92.8-docking`）全通 | — 无需改动，与设计一致 |
| ② `^b10069` 不可能 | 版本模型只有「4 个整数」一种形态，没有「不可排序但可寻址」这一类 |
| ③ `^1.92.8` 看不见 `1.92.8-docking` 的区别 | 截断丢语义（parse）+ 从数值重造地址（B1） |
| ④ lock 记的是约束本身 | **另一个根因**：解析结果没有回流；见 B4 |

②③ 是同一个根因的两面：**版本 = 字面身份 + 可选的序**，而现在只建模了序。④ 与它们无关，是消费者读错了输入。

## B3. 设计：字面是身份，数值只是序

```cpp
// 一个索引版本键。literal 是身份（wire 地址、store 目录、lock）；
// order 是可选的排序能力：nullopt = 不可排序，只能精确匹配。
struct VersionKey {
    std::string          literal;
    std::optional<Order> order;
};
struct Order {                       // 现在的 Version + 预发布
    int major, minor, patch, revision;
    std::vector<PreId>   prerelease;  // 空 = 正式版；正式版 > 任何预发布
    // build metadata（'+' 之后）不参与序，但**在 literal 里**，所以不影响身份
};
```

- `resolve_semver` 返回 `keys[*idx].literal`。**`str()` 不再出现在任何寻址路径上**，降级为纯展示，并在注释里改掉「load-bearing」的说法——把约束从「需要遵守」变成「无处可犯」。
- **不可排序的键（`b10069`）**成为一等公民的一类：`order == nullopt`，只参与 `=` / 裸字面（`is_version_constraint` 为 false 的那条路），范围与 `*` 跳过它。这把 issue 建议 2 的「明确而不是靠恰好走了另一条代码路径」落成类型。
- **预发布序**按 semver：`1.92.8-docking < 1.92.8`；预发布标识符点分段比较，数字段按数值、数字段 < 字母数字段。加上 mcpp 的第四段：序为 `major, minor, patch, revision, 然后 prerelease`（`1.2.3-rc < 1.2.3 < 1.2.3.1`，自洽）。
- **范围对预发布的可见性**采用 npm/Cargo 已被接受的规则：**带预发布的候选只有在约束里存在一个「同 (major,minor,patch,revision) 且自身带预发布」的比较子时才可入选。** 一条规则同时修两件事：`^1.92.8` 不再看得见 `1.92.8-docking`（issue 的核心诉求），且 `^1.2.3` 的上界不再漏进 `2.0.0-alpha`（今天 `v < upper` 会漏，是同族的既存缺陷）。
- **序相等但字面不同**（只差 build metadata，如 `1.0.0+a` 与 `1.0.0+b`）：见下面 B3.5——这个情形在真实索引里存在，但它的真身不是「平局」。**列入待决策 4。**

## B3.5 关键发现：真实索引里的「平局」是 alias，而 mcpp 不认识 alias

扫了本机 `xim-pkgindex` 全部 161 个描述符，非纯数字版本键只有两个包，而**两个都命中本 issue 的形状**：

```lua
-- pkgs/j/jdk-temurin.lua，三个平台表都一样
["latest"]   = { ref = "25.0.4+7" },
["25.0.4"]   = { ref = "25.0.4+7" },
["25.0.4+7"] = { url = …, sha256 = … },     -- 唯一的真条目

-- pkgs/c/cc-connect.lua
["1.3.2"], ["1.3.3-beta.1"]
```

`jdk-temurin` 同时是「build metadata 平局」和「不可排序键被静默跳过」的实例，而**两者都是假的**：`25.0.4` 与 `latest` 都是 `{ ref = ... }` 指针，指向同一个 `25.0.4+7`。

**`list_xpkg_versions` 把每个引号键都当成一个版本，包括别名**（它就地收集平台表里的所有 key，`xpkg.cppm:955`；全仓无一处读 `ref`）。于是范围解析的候选集里，三个「版本」有两个是指向第三个的指针。

这一条把待决策 4 的问法改掉了：**先排除纯 alias 条目，再谈平局政策**——否则是在给一个不存在的问题定规则，而且定出来的规则会在一个完全正常的索引写法上开火（对 `jdk-temurin` 报「两个版本序相等，无法取舍」，而两个答案是同一个 payload）。

`cc-connect` 则是另一件事的实例：今天 `^1.3` 会解析到 `1.3.3-beta.1`（截断成 `1.3.3` → 最高），改后按 npm 预发布可见性规则解析到 `1.3.2`。**方向是对的**（范围不该悄悄给你 beta），但这是一处真实的、生态可见的行为变化，必须写进发布说明。

**诊断**（issue 建议 2 的另一半）。今天两个方向都不好：

- 约束侧 `^b10069` → `invalid version constraint '^b10069': version: not a number ('b10069')`。技术上不错，但没说**为什么不可能**。
- 键侧才是真隐患：索引里只有 `b10069`，用户写 `*` 或 `^0.1` → 静默 `continue` → **`no valid versions in index`**。这句话在**索引里明明有一个完全可用的版本**时把责任推给了索引。

新形状：范围没匹配到任何东西、而存在不可排序的键时，错误必须指名它们并给出可执行修法——「`ggml-org:llamacpp` 的版本键（`b10069`, `b10121`）不是可排序的版本号，范围约束无法表达它们；请精确 pin：`llamacpp = "b10069"`」。

## B4. lock：数据结构已经存在，两个消费者读错了输入

`prepare.cppm:1964-1977` 里已经有：

```cpp
struct ResolvedRecord {
    std::string version;       // 解析后的具体版本
    std::string constraint;    // 作者写的原始约束
    std::string requestedBy;
    std::string source;        // "version" | "path" | "git"
    ...
};
std::map<ResolvedKey, ResolvedRecord> resolved;    // 覆盖整张图，含传递依赖
```

**lock 需要的每一个字段都在里面，覆盖范围也对。** 但两个消费者都没读它：

| 消费者 | 现在读什么 | 后果 |
|---|---|---|
| lock 写入 `prepare.cppm:5363` | `m->dependencies`（root 直接依赖，`spec.version` = 未解析的约束） | lock 记 `^1.92.8`；**传递依赖完全不在 lock 里** |
| `Compiling` 行 `execute.cppm:323-327` | 同上 | 打印 `compat.imgui v^1.92.8` |

`resolveSemver` 改的是 worklist 里的**副本**（`auto& spec = item.spec;`）。旁边 `prepare.cppm:3277-3283` 已经有一处专门往 `m->dependencies` 回写 `namespace_ / shortName / candidates` 的代码——**`version` 只是没被列进去**。这是本仓库反复遇到的「同一决策两处推导」的镜像：结果算出来了，消费者读的是输入。

**正解不是补第三处回写**（那会再造一个推导点），而是让 lock 写入和 `Compiling` 行**都读 `resolved`**。顺带解决三件事：lock 记真实版本、lock 覆盖传递依赖、控制台输出与 lock 同源不可能不一致。

**但还有一层更深的问题**：今天 lock 只在一处被读回，且只读 git（`prepare.cppm:1089-1092` → `parse_git_source`）。**index 依赖的 lock 条目从不参与解析。** 也就是说 lock 对 index 依赖是装饰性的——只写真实版本会让它**看起来**权威而实际仍不 pin，这比现在更容易误导（假 `Cached` 的教训）。

两条路：

- **B4-a（本批）**：写真相 + 覆盖传递依赖 + 文件头注释明说「本文件记录本次解析结果，尚不 pin 后续构建」。诚实、零行为变更、零生态风险。
- **B4-b（单独一批）**：让 lock 权威，并配 `mcpp update`。这是正确终局，但它**改变解析行为**（有 lock 时不再自动吃索引新版本），需要独立的迁移与生态验证。

**已决：本批只做 B4-a，lock 不权威。** B4-b 单开一批。

这条决定带一个**不可省略的义务**：既然 lock 写的是真实版本却仍不参与解析，文件头必须自己说出来。写死在 `serialize()` 的头注释里，而不是只写进 docs——`# Auto-generated by mcpp. Do not edit by hand.` 后面加一行「记录本次解析结果；尚不锁定后续构建（索引出现更高版本时会重新解析）」。理由与假 `Cached` 那次相同：一个看起来权威而实际不 pin 的产物，比一个明显不完整的产物更危险。判据 B6 之外加一条 **B8：lock 头部含该声明**（e2e grep，改回权威时这条断言会红，正好提醒同批删掉它）。

`LockedPackage` 的 `requested` 字段（作者写的约束）**推迟到 B4-b**：它唯一的用途是「manifest 改了要重解析」，而本批不读 lock，现在加就是加一个没有消费者的字段。**待决策 2 撤销。**

## B5. 兼容性

爆炸半径小得反常，值得点明：**`mcpp.version_req` 全仓只有两个消费者**——`pm/resolver.cppm` 与 `pm/index_contract.cppm`（`prepare.cppm` 只 import 未用）。

| 面 | 影响 | 处置 |
|---|---|---|
| 四段日期版本 `2026.8.6.3` | 无预发布、无 build metadata → 新解析器逐位同旧 | 判据 B4：对现有全部 `min_mcpp` 值做全表比对 |
| E0006 索引下限（`index_contract.cppm:125`） | 只用 `>=` 比 `Order` | 不变；malformed → `nullopt` 的「永不砖」性质保留 |
| 已发布索引里的既有键 | `^1.92.8` 今天**恰好**选中非 docking，改后**确定性地**选中它 —— 结果不变、原因变对 | 但若某包**只有**预发布键而消费者写了范围，改后会从「能解析」变成「报错」 |
| `mcpp add` / `index_refresh` | 都走 `is_version_constraint`（纯语法谓词），裸 `1.92.8-docking` 仍判为精确 | 不变 |

最后一行是唯一的真实生态风险，必须**在发版前用真实 mcpp-index 全表扫**：找出所有「版本键全是预发布」的包。（这条与「发版前必须本地跑真实 mcpp-index workspace」是同一条纪律；CI 全绿证明不了。）

**全表扫已做一次**（本机 `xim-pkgindex`，161 个描述符）：非纯数字键只有 2 个包，无「只有预发布键」的包。两个包的具体影响见 B3.5——`cc-connect` 的 `^1.3` 会从 `1.3.3-beta.1` 改到 `1.3.2`（方向正确，需写进发布说明），`jdk-temurin` 取决于 alias 是否排除。**这次扫的是本机快照，发版前要对当时的索引重扫一遍。**

**VERIFY-B1**：把范围解析结果从 alias 键（`25.0.4`）改成真条目（`25.0.4+7`）之后，store verdir 与 wire 地址行为是否变化——xlings 是否在建 verdir 之前解析 `ref`。若会产生第二个 verdir 或改变寻址，则退回「平局时择字面较大者 + 一次 warning」，把 alias 建模单开一批。

## B6. 判据（可机器验证）

1. **B1**：对索引里每个键 `k`，`resolve_semver("=" + k)` 必须原样返回 `k`。这条把「不再重新渲染地址」变成可穷举的属性测试。
2. **B2**：`1.92.8` 与 `1.92.8-docking` 在任何约束下不可互相替代；`^1.92.8` → `1.92.8`；只有 `^1.92.8-a` 这类自带预发布的约束才可能选到 `1.92.8-docking`。
3. **B3**：`^1.2.3` 不匹配 `2.0.0-alpha`（今天会匹配）。
4. **B4**：`b10069` 只有精确可达；范围约束下的错误消息**指名该键并给出精确 pin 的修法**，且不含 "no valid versions in index"。
5. **B5**：`2026.8.6.3` 与所有现存 `min_mcpp` 的比较结果逐位不变。
6. **B6**：`mcpp.lock` 的 `version` 恒为可寻址字面版本；连续两次 `mcpp build` 之间 lock 字节不变（幂等）。
7. **B7**：lock 含传递依赖；`Compiling` 行的版本与 lock 一致（同一数据源，构造性保证）。

---

# C. 实施顺序（各切片可独立发布）

| 步 | 内容 | 依赖 | 风险 |
|---|---|---|---|
| B-1 | `VersionKey`（字面+可选序）、预发布序、npm 预发布可见性规则、`resolve_semver` 返回字面键、不可排序键的指名错误 | — | 低（两个消费者） |
| B-2 | lock 与 `Compiling` 行改读 `resolved`；覆盖传递依赖；文件头诚实声明 | — | 低（无行为变更） |
| A-1 | `[resources]` 解析、`plan.resourceUnits`、`rc_object` 规则、rc 工具惰性解析（payload 相对，硬失败）、L1 自写 `.rc` + 输入扫描 | — | 中（新构建边） |
| A-2 | L0 合成 VERSIONINFO + icon；`FILEVERSION` 取 4 元组 | B-1 更佳（否则自己解析一次版本号） | 低 |
| A-3 | VERSIONINFO 名 lint | A-1 | 低 |
| A-4 | `Role::Object` + `targets`（已决：本批做） | A-1 | 中（角色表扩容） |
| — | **B-3（不在本批）** lock 权威 + `mcpp update` + `requested` 字段 | B-2 | 中（改解析行为） |

先 B 后 A：B-1 给 A-2 提供版本 4 元组，且 B 的爆炸半径最小、能先独立验证。

---

# D. 决策记录

## 已决

1. **lock 本批不权威。** 只写真相（B-2），并在 `serialize()` 头部声明它尚不 pin（判据 B8）。`mcpp update` 与 `requested` 字段一起放到 B-3，不在本批。
2. **`Role::Object` 本批做**，用可选 `targets = [...]`（空 = 声明包的全部 link unit）指定接哪条 link 边。
3. **节名 `[resources]`，平台不进拼写。** 平台作用域是「只有 PE 目标消费」，但不写进节名：图标作为**概念**不是 Windows 专有的，只有文件格式与嵌入机制是；将来 macOS `.icns` / Linux `.desktop` 扩**同一节**（必要时 `icon` 升级成 per-platform 映射），保持一条轴，而不是按 OS 切成 `[windows]` / `[macos]` 三份并让 `icon` 这类共性键重复三次。
   这条决定带一个**义务**：manifest 里既然看不出「只对 Windows 生效」，就得让它在别处可见。PE 目标下 `[resources]` 生效时打一行 status（`Embedding   app.ico + version info`）——非 PE 目标**不打也不警告**（是「不适用」不是「降级」，每次 Linux 构建警告一次是噪音）。这条同时让判据 A2 的增量行为可观测。
   **per-target 延后**（落点是 `kKnownTargetKeys`，老 mcpp 只 warn 不 fail）。

## 待 review

4. **平局政策——但问法要先改（见 B3.5）。** 拆成三小条：
   - **4a（推荐做）** 识别纯 alias 条目（`{ ref = "…" }` 无 payload），**从范围候选里排除**；精确寻址不变（`= latest` / `= 25.0.4` 照旧走别名）。不做这条，平局政策就是在给一个不存在的问题定规则，而且会在 `jdk-temurin` 这种完全正常的索引写法上开火。
   - **4b（推荐）** 排除之后剩下的真平局（两个都是真条目、只差 build metadata）→ **硬错，指名两个键并给出「精确 pin 其一」的修法**。理由：此时两个键是两个不同 tarball、不同 sha256，mcpp 无从知道要哪个；「择字面较大者」是把猜测包装成确定性，正是本 issue 抱怨的「取舍由一个看不见差别的序决定」。#349「数据不得让程序失效」的反向担忧在这里是有界的——它只影响那一个包的范围解析（不是索引级不可用），错误里就写着修法，且 4a 之后它在惯用写法上不可能触发。
   - **4c** 若 VERIFY-B1 发现排除 alias 会改变 store verdir 或寻址行为，则 4a/4b 一起退回「择字面较大者 + 一次 warning」，alias 建模单开一批。
5. **`.rc` 输入跟踪：扫描 + 指名缺口 + `extra-inputs` 兜底（推荐），还是纯显式声明？**
   一份 `.rc` 的输入只有两类，分别处置：
   - **`#include`**：只跟踪**引号形式**（`"dialogs.h"`，项目自有）；**尖括号形式不跟踪**（`<windows.h>` 属工具链，随 payload 不变，且已被工具链 fingerprint 计入构建目录）。这条规则一下去掉了「无法枚举 windows.h 传递闭包」这个担忧。
   - **资源语句里的数据文件**（`1 ICON "app.ico"`、`24 MANIFEST "app.manifest"`、`RCDATA` / `BITMAP` / `CURSOR` / `FONT` / `TYPELIB`）：扫引号字符串。
   唯一的真缺口是**宏间接**（`1 ICON APP_ICON`，文件名藏在 `#define` 后面）。处置：扫描遇到「操作数不是字符串字面量」或「引号 include 在搜索路径上找不到」时**警告并指名**，指向 `extra-inputs`——「限定了覆盖范围就要说出漏了什么」。
   为什么不选纯显式：漏掉一项时 mcpp **无从警告**（它不知道自己漏了什么），而漏掉的后果是 exe 里留着旧资源直到有人碰一下 `.rc`——与本 issue 报的失效同一类。
   为什么不选「自己预处理 + 拿编译器 depfile」（最精确）：只有 `llvm-rc` 有干净的 `/no-preprocess` 入口，`windres` 与 `rc.exe` 都没有「吃已预处理输入」的干净模式 ⇒ 工具矩阵从 3×1 变 3×2。精确度换来的是每条 dialect 两套管线。
   附注：**L0 完全不需要扫描**（合成的 `.rc` 里 icon 路径是声明来的，精确），所以这套启发式只作用于自写 `.rc`——而自写 `.rc` 的人正是能写 `extra-inputs` 的人。
6. **偏离 issue #365 第 3 条**（「资源文件缺失时跳过，不应导致构建/打包失败」）。issue 这一条把两个担忧捆在一起，拆开之后一个消失、一个反转：
   - **担忧一「Windows-only 声明会弄坏我的 Linux/macOS 构建」** → 已由「非 PE 目标整节不适用」解决。不是跳过，是没有消费者：零 warning、逐字节不变。issue 想要的跨平台安全**已经拿到了**。
   - **担忧二「资产文件不在（新克隆没拉 LFS / CI 没有这个文件 / 设计还没交图）不该让构建失败」** → 这里偏离：**声明了却不存在 = 硬错误**。四条理由：
     ① 一致性——mcpp 里每个「声明过的输入」都是这个规则：`main = "…"` 必须匹配恰好一个文件、`scan_overrides` 的每个 glob 必须匹配 ≥1 个文件、nasm 缺失是硬错误且注释明写「never a silent skip，掉一个 `.o` 会在几层之外以 undefined 现形」。
     ② 它会把本 issue 的失效模式**制度化**——报告者的全部抱怨就是「静默不生效」；把「文件缺失 → 跳过」写成设计，等于让静默不生效成为规定行为。
     ③ 最疼的是发布构建：一个路径拼错或资产没提交，产出的是**没有图标、没有版本信息的正式二进制，而且什么都没说**。发现时间点通常是有人下载之后，归因成本极高。
     ④ 「图标是可选资产」说的是**功能**可选，不是**声明**可选。不要图标已经可表达：把那一行删掉。
   - 若仍要字面满足 issue：`icon = { path = "…", optional = true }`，走既有 `diag::degraded` 通道（报告一次、`--strict` 拒绝）——**显式选择降级**而不是默认静默。推荐先不做（YAGNI）：不声明、或用 `action{role=object}` 生成，两条退路已经在。

---

# E. 实施回执（2026.8.7.1）

## E.1 写代码才撞出来的三条

1. **⚠️ 非 ASCII 元数据会让 rc 编译器直接拒绝整个脚本。** 设计里完全没有这一格。用合成器的输出跑真 `llvm-rc` 时立刻炸：

   ```
   llvm-rc: Error in VERSIONINFO statement (ID 1):
   Non-ASCII 8-bit codepoint (—) can't be interpreted in the current codepage
   ```

   触发它的是**mcpp 自己生成的**默认 copyright 里的一个 em dash。而 `[package]` 的 description/authors 是用户文本,中文项目必然命中 ⇒ **必须给 rc 工具传 UTF-8 codepage**(`/C 65001` / `--codepage=65001`),否则一个中文描述的项目根本构建不了。同时把生成文本本身收敛成纯 ASCII(单测断言),这样生成物不依赖那个 flag 是否传对——两道,因为它们防的是两件事。

   这条是「只写文档不写示例会漏掉」的又一次:设计里推演到了「VERSIONINFO 的名字必须是序号 1」,推不到「编码」。

2. **五段版本键在真实索引里存在**,而不是假想。`jdk-corretto` 发 `25.0.4.7.1`(`<feature>.<interim>.<update>.<build>.<revision>`),四段截断让 `25.0.4.7.1` 与将来的 `25.0.4.7.2` 比较相等。数值段因此改成任意长度而不是加到第五段——**固定长度这件事本身**是缺陷,加一段只是把下一次推迟。

3. **`.res` 的资源头偏移是 40 不是 32。** 设计文档里我写了「first eight bytes of the resource header」,实际是:32 字节全零头 → dataSize+headerSize(8 字节) → type+name。判据写成字节断言时必须核对偏移,否则断言恒假/恒真。已在代码注释与 e2e 里更正。

4. **⚠️ `.rc` 是「mtime 扫描看不见、但改了它图就该长得不一样」的第三个实例**——由 Windows CI 抓到,本机与设计都没预见。

   一次只改 `res/app.rc` 的构建报 `Finished dev in 0.15s`:工程级 fast path 短路了整个 prepare。`sources_newer_than` 只扫 `src/**/*` 的 C++ 扩展名,`.rc` 既不在 `src/` 下、也不是那些扩展名。

   **这不只是「警告没打」**:`.rc` 的 implicit input 集合来自**扫描它**,而扫描在 prepare 里。用户往脚本里加一行 `#include "ids.h"`,那个头文件永远不会被跟踪——ninja 用的是上一次算出来的输入表。

   同一个函数里已经为这件事写过两遍理由(`build.mcpp` 一次、#359 的 glob 输入一次),措辞都是「一种 mtime 扫描看不见的输入,而它改变了图应该长什么样」。**这是同一形状的第三次**,而前两次的注释没能让第三次被预见到——因为它们是**举例**,不是**判据**。真正该问的是:「这次新增的输入,改了它以后 prepare 的产出会不会变?会,就必须进这个扫描。」

   处置:只扫 `files`。`icon` 与 `extra-inputs` 已经是 ninja 的 implicit input,改它们不改变图的形状,为一次改图标强制走完整 prepare 买不到任何东西。

5. **我自己写了两条假绿断言。** 图标断言原本搜 4 字节(`00ff00ff`),在 MB 级二进制里撞上是常事——Linux 上通过很可能就是撞上了,而它同时**掩盖了第 4 条**(Windows 上 b3 本该在这里暴露 fast path 问题)。改成 4 像素 icon 的 16 字节高熵标记,并加一条「旧标记必须消失」。
   教训与「断言『出现 E0006』是假绿」同族:**断言必须能失败**,而「短模式在大文件里出现」这种断言几乎不可能失败。

## E.2 与设计的偏差

- **`peUnits` 为空时整节跳过并警告**(设计没提)。一个只产静态库的包声明了 `[resources]`,原设计会去解析 rc 工具并硬失败——为一次没有消费者的编译要求一个工具。
- **同名 `.rc` 冲突显式报错**。两个不同目录下的 `app.rc` 会写同一个产物,原本会变成 ninja 的 "multiple rules generate",报在离原因很远的地方。
- **`[resources]` 的路径解析用 `lexically_normal` 而非 `weakly_canonical`**。canonical 会解析符号链接,把与用户所写不同的路径烙进生成的脚本(与 #344 让缓存锚点保持字面判定同一条理由)。
- **版本号无数值形式时报 degraded**。`synthesize_rc` 是纯函数发不出诊断,所以由 prepare 侧再解析一次并报告:FILEVERSION 会是 `0,0,0,0` 而字符串字段保留真版本,不说的话属性对话框与 `[package].version` 不一致且无从解释。

## E.3 验证到什么程度

| 判据 | 状态 |
|---|---|
| A1 序号 1 / 元数据可读 | ✅ 本机实测(`.res` 字节 + `llvm-readobj` 双重断言),e2e 197/198 |
| A2 增量 | ✅ 改 icon、改 `[package].description` 都到达 exe;无变更为 no-op |
| A3 非 PE 不适用 | ✅ 198 里用同一份 manifest 构建 host,零警告、无 res 单元 |
| A4 不进 compile_commands / 模块图 | ✅ 结构性(独立 `ResourceUnit`,不入 `CompileUnit`) |
| A5 工具缺失硬失败 | ⚠️ 代码路径有,e2e 未构造(要改 payload 目录) |
| A6 L0→L1 字节相同 | ✅ 198 实测 `cmp` 通过 |
| A7 `.rc` 的 include 被跟踪 | ✅ 单测覆盖扫描;e2e 覆盖 icon/metadata 两类输入 |
| B1 返回值恒为字面键 | ✅ e2e 196 用四种真实索引形状 |
| B2 docking 不可互相替代 | ✅ 单测 + 196 |
| B3 `^1.2.3` 不匹配 `2.0.0-alpha` | ✅ 单测 |
| B4 不可排序键指名错误 | ✅ 单测 + 196 |
| B5 E0006 逐位不变 | ✅ 单测(含 `2026.8.3.3` 对现役下限) |
| B6/B7 lock 真实 + 幂等 + 传递 | ✅ 196;169 也加了断言 |
| B8 lock 头部声明 | ✅ 196 断言(改成权威时会红) |

**A5 是唯一没有 e2e 的判据**:构造它要临时改动 payload 目录,而 payload 是共享的只读树,e2e 写它会污染其他测试(#293 的教训:一个测试的失败源于上一次运行)。

## E.4 本批 CI 覆盖的真实缺口

`cross-build-test.yml` 的 `mingw-cross-wine` 是**唯一**有 MinGW 交叉链的 job,而它**按文件名逐个调用 e2e**(不跑 `run_all.sh`)⇒ 新增的 198 必须显式加进 workflow,否则 GNU/windres 这一半在 CI 里一次都不会跑。已加。Windows 原生那一半走 `ci-windows-e2e` 的整套 `run_all.sh`,`# requires: windows` 自动生效。

---

# F. 实施后 review 轮(同一 PR,合入前)

对已实施版本做了一次架构 / 稳定性 / 一致性 / 多平台的深度 review,并对每条可疑点做真机复现而不是代码推理。**四条实测确认的缺陷 + 三条口径不一致**,全部在本 PR 内修掉。下面记的是**判据**,不是清单。

## F.1 「同一决策两处推导」又出现了一次,而且两处已经不一致

`resources.cppm:202` 用 `find_first_of(";:")` 切工具链 PATH 覆盖,`prepare.cppm:5323` 切 INCLUDE 用 `find(';')`——**同一份数据、同一个 PR、两种切法**。

`;:` 那一种是错的:msvc 分支只在 Windows 上走,PATH 覆盖由 `msvc.cppm:492` 用 `;` 拼接真实 Windows 路径,而**盘符冒号就在下标 1**。`C:\Windows Kits\…` 被切成 `C` 加一段当前盘相对路径 —— 同盘侥幸命中、跨盘必然找不到。放大它的是:这条 PATH 遍历是 msvc 下的**主路径**,不是兜底(`rc.exe` 属于 Windows SDK,从不在 `cl.exe` 旁边,`probe_dir(compilerDir)` 只答得出 llvm-rc)。

⇒ 收敛成一个 `rsrc::split_env_list`,两个调用点共用。**判据:一个 PR 内出现两处相同解析,先合并再讨论哪种对。**

## F.2 「模型少一层」在本 PR 自己身上复发:`Role::Object` 的默认目标集漏了测试二进制

真机复现(Linux/clang22,库代码调用 object 提供的符号):

```
mcpp build → rc=0     mcpp run → rc=0
mcpp test  → ld.lld: error: undefined symbol: blob_value
```

`image` 只认 `Binary | SharedLibrary`,而 `TestBinary` 是第三种 kind。**这正是 #365 那条毛病的同构体**:表格少一格,后果是用户在图外找路。

关键在于**它没有可用的逃生口**:显式 `.target("t_core")` 确实能修(实测通过),但测试链接单元是从 `tests/*.cpp` **发现**出来的,名字不在 `mcpp.toml` 里;而且只写测试目标时 `mcpp build` 直接硬失败(实测:`names unknown target(s): t_core / targets in this build: [objrole]`)。⇒ **目标名字空间是 mode-dependent 的**,这一层设计没有承认。

⇒ 默认集合加入 `TestBinary`。`[resources]` 的 `peUnits` 反向决策(排除测试二进制)保持不变,并在两边互相写明理由:**图标属于「要发布的东西」,符号属于「要链接的东西」**。

## F.3 F.2 与「未知 target 报错」是耦合的,不能分开修

未知名检查挂在 `if (!attached && ...)` 上,于是**只要有一个名字命中,其余拼错的就永不上报**。实测 `.target("objrole").target("no_such_target_TYPO")` → `rc=0`,日志零提及 —— 与 `types.cppm` 和 docs 明写的契约相反。

⚠️ **但先修这条会当场废掉 F.2 的唯一逃生口**:`.target("app").target("t_core")` 之所以能在 `mcpp build` 下不报错,靠的正是这个漏洞。⇒ **正确顺序是先给 F.2 一个不依赖具体测试名的表达方式(默认集合),再把检查收紧到 per-target。** 单独修任何一条都会把用户推进另一个坑。

## F.4 lock 从「读输入」改成「读输出」,顺带把命令也读进去了

真机复现(同一工程交替执行):

```
mcpp build → 1 条    mcpp test → 2 条(多了 dev-dep)   mcpp build → 又变回 1 条
```

`resolved` 在 `includeDevDeps` 时含 dev-deps,而旧代码遍历 `m->dependencies`(dev-deps 是另一张表)**结构性地写不进去**。196 只断言了 build→build 幂等,抓不到。

⇒ `WorkItem`/`ResolvedRecord` 增加 `devOnly` 并沿依赖边传播(**多消费者取 AND**:非 dev 的消费者一出现就清掉),lock 跳过 `devOnly`。**判据:lock 是 manifest 的函数,不是命令的函数。** 196 补 build→test→build 三段 `cmp`,并断言 dev-dep 确实被解析过(否则断言是空转)。

## F.5 「不适用」不该覆盖到校验

`[resources]` 的「声明了必须存在」整块包在 `if (trip.is_pe())` 里。实测:Linux 上 `icon = "assets/DOES_NOT_EXIST.ico"` + `files = ["res/nope.rc"]` → `rc=0`,日志里 `resource` 出现 **0 次**。

⇒ Linux/macOS 的开发机与 CI **结构性地**抓不到打错的资源路径,只有 Windows job 会红 —— 这正是这条硬错误要消灭的「太晚才知道」。**路径存不存在是关于工作树的事实,不是关于目标的事实。** 校验提到 `is_pe()` 之前,编译留在后面。新增 `199_resources_validation.sh`(无 `requires`,每个 shard 都跑)守这条。

## F.6 两条 warning 应当是 degraded

`diag.cppm` 的规矩:`degraded` 带 impact 且 `--strict` 会失败;`warning` 是作者笔误 / schema 漂移。按这条口径,`resources/versioninfo`(你的 VERSIONINFO Windows 读不到)和 `resources/no-image`(声明了却没有任何东西嵌)都是「你要了 X,得到的是零」,应当是 degraded —— 尤其前者正是本 feature 要消灭的静默失效,`--strict` 抓不到它说不过去。`role="object"` 无消费者是同一形状,新增 `action/no-target`,与 `resources/no-image` 同口径。

## F.7 优雅性

- `} else {` 之后约 150 行**完全不缩进**、靠底部 `}   // peUnits non-empty` 收尾 —— 在一个对形式如此讲究的仓里是「这块该抽出去」的直接信号。改成带早返回的 `plan_resources` lambda。
- `ResourceUnit::packageName` 写入后全仓无人读取,删除。
- `LinkUnit::objects` 注释写「relative to plan.outputDir」而 `Role::Object` 往里塞绝对路径 —— 有理由(ninja 按字面串识别节点),但字段契约本身要改口径,否则下一个人会「顺手规范化」。
- `resolve_semver` 的字面短路对**任意**约束生效而不只 `=` 前缀。今天安全,靠的是三个调用方都先过 `is_version_constraint` 门——但**那个判据没有写进这个函数**。裸 `1.2.3` 在本语法里是 caret,哪天直达此处,caret 会静默变成精确 pin。收紧成必须 `=` 前缀。
- `try_merge_semver` 在两个约束相同时不再拼成 `=X,=X`(防御性:prepare 只在两个**已解析版本**不同时才走到这里,而相同约束解析结果相同 —— 但它挡的失败是静默且彻底的)。

## F.8 测试自身的假绿

- `_windows_resources_body.sh` 里 `.res` 的序号字节判据包在 `case *.res` 中,而 windres 产 `.o` ⇒ **GNU 那一半的核心断言可能一次都不跑**;后备的 readobj 检查也是「grep 不到就跳过」。补一条方言无关的判据(字符串名资源会把 UTF-16 名字写进资源目录,序号名不会),并在 A3-lint 段加**正向反证**:故意构造坏脚本,断言它确实产生了那个 UTF-16 名字 —— 否则「名字不存在」可能因任何理由通过。
- 改写 `res/app.rc` 那一步前缺 `sleep 1`,而该段**唯一**的重跑 prepare 触发源就是 `.rc` 的 mtime。其余每处 mtime 敏感改动都有。

## F.9 方法论

**每一条都先真机复现再下判断,没有一条是纯代码推理。** 复现同时充当「断言能失败」的证明:五条新断言各自对应一段用**修复前**二进制跑出来的、看得见的错误输出。这比事后再造一个反向用例更便宜也更可信。

## F.10 CI 抓到的第三条:判据选了一个那条 job 拿不到的工具

改完 F.8 的判据后本机 198 通过,CI 的 `mingw-cross` job 却红在:

```
FAIL: llvm-readobj not found under /home/runner/.mcpp
```

那条 job 只装 mingw 工具链,**sandbox 里根本没有 LLVM 载荷**——而本机什么都装着,所以本机永远看不见。

**硬失败本身是对的**(静默跳过正是 #365 的出厂方式),错的是**判据依赖了一个不属于这条路径的工具**。正解:`windres -J coff -O rc` 把编译好的资源**反读成 rc 源码**,名字直接可见,而且它在 GNU 这一支**必然存在**——它就是产出这个文件的工具。实测 `.o` 与链接后的 `.exe` 都能读:

```
1 VERSIONINFO                   ← Windows 找得到
"VS_VERSION_INFO" VERSIONINFO   ← 找不到
```

rc 工具从 `build.ninja` 的 `rc =` 绑定里取而不走 PATH——mcpp 本来就是 payload 相对解析的(裸 `windres` 在 PATH 上是 xlings shim),问 PATH 会用**另一个**工具去检查这个产物。msvc 那一支保留 llvm-readobj:能走到那一支的配置里,LLVM 载荷就是默认工具链本身。

**判据:一条断言要用的工具,必须来自它所断言的那条路径本身。** 「本机装得全」是最容易把环境假设藏起来的地方——三次改判据里,前两次都是本机绿、别处红。

## F.11 第四条:`*windres*` 这个名字什么都不告诉你 + msvc 那一支其实零 CI 覆盖

F.10 改成 windres 反读后,mingw job 绿了,**Windows job 红了**:

```
FAIL: windres could not read back .../res/resapp.mcpp.o
```

两件事同时暴露:

1. **`llvm-windres` 不能反读。** 它只是 `llvm-rc` 的单向包装,没有 `-J coff`。而它和 GNU binutils 的 `windres` **都匹配 `*windres*`** ⇒ 按工具名分派是错的。修法:**两条路线依次 TRY**(反读 → llvm-readobj),只有两条都不可用才硬失败。
2. **native Windows 上默认工具链走的是 GNU 支,不是 msvc 支。** 证据在失败信息里:target 目录是 `x86_64-windows-msvc`,而产物是 **`.o` 不是 `.res`** ⇒ `dialect_for(tc).id != "msvc"`,`find_rc_tool` 取了 GNU 分支并选中 `llvm-windres.exe`,lld-link 照单全收。

**⚠️ 推论:`.res` 那一支(rc.exe / llvm-rc + 偏移-40 字节判据)在两个 CI job 里都不执行。** mingw job 是 GNU,Windows job 也是 GNU。也就是说:

- e2e 里 `case "$RES_ART" in *.res)` 是**死分支**——这也正是为什么「方言无关的那条判据」不是锦上添花而是唯一的那条。
- **F.1 修的那个 PATH 切分缺陷(msvc 下找不到 `rc.exe`)没有任何 CI 覆盖**,只有单测 `EnvListSplitsOnSemicolonsOnly` 守着字符串切分本身。要真正覆盖它需要一个 `# requires: msvc` 的资源 e2e(`msvc@system` 工具链),本批不做,**明确记为缺口**。

**判据:「本机/某个 job 绿」不等于「这条分支跑过」。判断一条 fork 是否被覆盖,要看产物形态(`.res` vs `.o`),不能看 job 名字里有没有 windows。**
