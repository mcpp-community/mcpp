# xlings 作为 mcpp 的运行时底座:运行时身份、链接契约与环境契约

**日期**:2026-08-07
**性质**:设计提案,待 review 后再实施。本文不改任何代码。
**触发**:
- mcpp#375(产物烙私有 glibc PT_INTERP ⇒ 不可分发;系统 loader 包装 ⇒ `/proc/self/exe` 失效)
- mcpp#352(GLFW/OpenGL 静默 exit 255:沙箱 glibc 2.39 加载不了宿主 Mesa)
- xlings 侧新能力已落地:glibc 2.44、22 包 hermetic 图形栈(`xim:graphics`)、subos 自描述(`subos_info`)

**关联(xlings 仓)**:
- `.agents/docs/2026-08-05-ecosystem-three-tier-and-composable-distro.md` —— 三分层定位,**开放问题 K:mcpp 消费 subos 还是消费 platform 描述**。本文回答它。
- `.agents/docs/2026-08-06-subos-architecture-proposal.md` —— 七条规则 R1–R7,本文全程引用
- `.agents/docs/2026-08-05-userspace-distro-hermetic-strategy.md` —— 运行时边界

**关联(mcpp 仓)**:
- `.agents/docs/2026-08-02-issue336-pr142-analysis.md` —— `cxx_runtime` 契约层。**本文是它在 libc 轴上的同族补全。**
- `.agents/docs/2026-07-07-hermetic-toolchain-link-model-design.md` —— `linkmodel.cppm` 的由来

---

## 0. TL;DR

**一句话根因:mcpp 把 xlings 当成「一套需要自己反推的目录约定」,而不是「一个可以查询的运行时」。**

后果是可预测的:xlings 每前进一步,在 mcpp 侧都落成**一次改代码**,而不是**一次改数据**。glibc 2.44 如此,图形栈如此,下一个能力也会如此。

三条缝,按依赖顺序:

| | 缝 | 现状 | 目标 |
|---|---|---|---|
| **S1** | **运行时身份** | 从目录布局反推「哪个 libc」,且**无版本** | 读 subos 的 `subos_info.runtime`(`glibc@2.39`),成为一等轴 |
| **S2** | **分发路径** | 三条 hermetic 路径都存在,但用户找不到;其中一条自己是坏的 | 修坏的那条 + 让三条可发现。**不加「链宿主 libc」的开关** |
| **S3** | **环境契约** | mcpp 产物拿不到 subos 的 env,图形栈靠 mcpp 代码硬扛 | 消费 `subos_info.envs`;图形栈以**依赖**而非代码到达 |

**关键结论:P0 需要 xlings 零改动。** `subos_info` 已经存在,subos view 已经填充好(`subos/default/lib/ld-linux-x86-64.so.2` 实测在位)。唯一需要 xlings 配合的是**把 installer 已经算出来、当前只活在进程里的 `resolved_deps`/`deps_exports` 持久化**——那是 P1。

**多维评估见 §11**(实现代价 / 用户 / 稳定性 / 跨平台 / 简洁 / 兼容性,每项带实测数字)。三条要点:

- **本文被推翻过一整节**(§3-S2):初稿要补一条 `c_runtime` 轴,其 `host-coupled` = 链宿主 libc。已实现、全绿、然后整条撤销。理由与教训写在那一节,**比结论更值得读**
- **零 BMI/对象缓存失效** —— fingerprint 是 compile-side,分发相关的改动都只碰链接与打包
- **收益 ≈ 全在 Linux**;#352 的图形栈迁移是其中收益最大、代价最小的一条

---

## 1. 实测现状(不是推断)

本节每一条都在本机跑过,命令附在后面,便于 review 时复核。

### 1.1 产物里烙进了四个独立的版本 pin

```
$ mcpp new hello && cd hello && mcpp build
$ file target/x86_64-linux-gnu/*/bin/hello
… interpreter /home/speak/.mcpp/registry/data/xpkgs/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
$ readelf -d target/x86_64-linux-gnu/*/bin/hello | grep RUNPATH
  RUNPATH  [ …/xim-x-llvm/22.1.8/lib/x86_64-unknown-linux-gnu
           : …/xim-x-llvm/22.1.8/lib
           : …/xim-x-gcc-runtime/15.1.0/lib64
           : …/xim-x-glibc/2.39/lib64 ]
```

一个 hello world 产物同时钉死了:**home 绝对路径**、**glibc 2.39**、**llvm 22.1.8**、**gcc-runtime 15.1.0**。

这正是 xlings `libs/graphics.lua:47-54` 刚刚明文废弃的反模式:

> `${pkgdir}` 是声明包自己的载荷,它**钉死一个版本目录**:升级 mesa 会让消费者记录的 env 指向旧的那个。subos 视图才是稳定的间接层——就是 `/run/opengl-driver` 在 NixOS 扮演的角色。

mcpp 烙的就是 `${pkgdir}` 等价物。

### 1.2 稳定间接层已经存在,而且已经填充好

```
$ ls -l ~/.mcpp/registry/subos/default/lib/
ld-linux-x86-64.so.2 -> …/xim-x-glibc/2.39/lib64/ld-linux-x86-64.so.2
crt1.o crti.o crtn.o  -> …/xim-x-glibc/2.39/lib64/…
libc.so.6 libc.so libm.so.6 …  -> …/xim-x-glibc/2.39/lib64/…
libc++.so.1 libc++abi.so.1     -> …/xim-x-llvm/22.1.8/lib/…
libatomic.so.1 libasan.so.8    -> …/xim-x-gcc/16.1.0/lib64/…
```

loader、CRT、libc、C++ 运行时**全部在视图里**,按活动版本指向载荷。mcpp 一个都没用。

> ⚠️ 注意布局差异:载荷里 loader 在 `lib64/`,视图里在 `lib/`。mcpp 现有的 `{lib64, lib}` 顺序探测(`probe.cppm:346-349`、`linkmodel.cppm:249-258`)**碰巧两边都能命中**——因为它回退到 `lib`。但「碰巧能命中」正是本文反复要指出的那类脆弱:两个布局不同的树共用一份约定探测,靠的是回退顺序恰好合适。视图新增一个 `lib64` 就会翻。这条要在 P0 验证门里钉住(V5)。

### 1.3 mcpp 的 sandbox subos 不描述自己

```
$ python3 -c "import json;d=json.load(open('$HOME/.mcpp/registry/subos/default/.xlings.json'));print(list(d.keys()), len(d['workspace']))"
['workspace'] 356
```

356 个版本条目,**零运行时身份、零环境声明**。没有 `subos_info` 块。

而 xlings `src/core/subos/manifest.cppm:16-20` 的模块注释,点名了这件事:

> 一个程序需要三样:bootstrap(PT_INTERP + CRT + libc)、discovery(PATH + RPATH)、configuration(env vars)。xlings 有前两样——glibc + elfpatch、xvm + shims——第三样什么都没有。**That is the gap behind mcpp-community/mcpp#352**:一个 GLFW 二进制链接得好好的,exit 255,因为没人告诉它 GL 驱动在哪。

xlings 已经把这个缺口补上了(`subos_info`,schema 1)。**mcpp 的 subos 没有拿到它。**

### 1.4 xlings 的权威答案只活在进程里

`src/core/xim/installer.cppm:2394-2450` 已经为**每一个** runtime 依赖计算并记录了完整档案(遵守 R1「权威记录必须是全量的」),包括 `install_dir`、`libdirs`(`{lib64, lib}` 约定**只在这里**应用一次)、`loader`、`abi`。

但落到磁盘上的只有:

```
$ cat ~/.mcpp/registry/data/xpkgs/xim-x-glibc/2.39/.xpkg-install.json
{ "os": "linux", "version": "2.39", "xlings_version": "2026.8.2.1" }
```

三个字段。`exports` 一个都没落盘。

这解释了 mcpp `linkmodel.cppm:182-188` 当年为什么拒绝它:

> 第三个来源——installer 写的持久化 `.xpkg-exports.json` 声明元数据——评估后移除了:它唯一的消费者只有这个 resolver,而上面两个来源已经覆盖了每一个真实载荷。

**那个判断在当时是对的,今天不再对**。理由见 §2.2。

### 1.5 图形栈:mcpp 侧仍是宿主借用

mcpp-index `pkgs/c/compat.glx-runtime.lua` 至今把宿主 `/usr/lib/x86_64-linux-gnu/libGL.so.1` 等 symlink 进 `mcpp_generated/glx_runtime/lib`,再经 `[runtime] library_dirs` 进 RUNPATH(`flags.cppm:698-706`)。

这就是 #352 的直接成因:宿主 Mesa 要 `GLIBC_2.43`,载荷 glibc 是 2.39。而 hermetic 策略已经把这个文件点名为**要淘汰的样本**。

同时,xlings 侧的替代品**已经可用**:`xlings install graphics` —— 22 包 hermetic 栈 + NVIDIA/WSL2 哨兵,一条命令五种宿主形态,无条件分支。

### 1.6 `/proc/self/exe` 陷阱不是用户的 workaround —— 是 mcpp 自己在生产它

这是本次调研里最出乎意料的一条,值得单独列。

#375 描述的第三条症状(用 `ld.so --library-path` 启动后 `/proc/self/exe` 指向 loader)读起来像是用户自己想出来的绕法。**但 mcpp 的 `self-contained` 打包模式生产的就是这个 wrapper**,`docs/02-pack-and-release.md:120-127` 原样写着:

```sh
exec "$here/lib/ld-linux-x86-64.so.2" --library-path "$here/lib" "$here/bin/myapp" "$@"
```

文档给的理由是对的(ELF 规范禁止 `PT_INTERP` 用 `$ORIGIN`),但**后果没有写**:凡是用 `mcpp pack --mode self-contained` 分发的程序,`/proc/self/exe` 全部指向 loader,`/proc/self/cmdline` 全部混入 `--library-path`。所有「在 exe 旁边找资源」的逻辑静默失效——字体、assets、随包分发的辅助二进制。

而 mcpp 自己就依赖这个机制:`src/platform/fs.cppm:106` 读 `/proc/self/exe` 定位自身。

全仓 grep `proc/self/exe` 只有那一处实现,**文档里零处提及这个陷阱**。所以 #375 请求的两条文档补充里,(a)「私有 glibc 对分发的影响」其实已经写了(`02-pack-and-release.md:3-6` 开篇就是),(b)「系统 loader 包装后的 `/proc/self/exe` 陷阱」**确实没有,而且比提问者以为的更严重**——它不是一条使用建议,是一个在售模式的已知缺陷。

---

## 2. 根因

### 2.1 一个问题,mcpp 侧有 N 个回答者

「C 运行时在哪、loader 是哪个、哪些目录进 RUNPATH」这一个问题,mcpp 侧今天有六个独立推导:

| # | 位置 | 怎么答的 |
|---|---|---|
| 1 | `probe.cppm:333-349` | `find_sibling_tool(compilerBin,"glibc")` 爬父目录 → `lib64` 不存在则 `lib` |
| 2 | `linkmodel.cppm:194-243` | 五行 arch→loader 文件名硬表,不中则 glob `ld-*.so*` |
| 3 | `linkmodel.cppm:215-221` | `distro_loader_path`:x86_64 → `/lib64/`,其余 → `/lib/` |
| 4 | `post_install.cppm:162-200` | 扫 clang cfg 文本找 `/ld-linux-` 反推烙定的 loader |
| 5 | `pack.cppm:650-653` | 从 `ldd` 输出取 loader soname,再拼 `/lib64/` 或 `/lib/` |
| 6 | `probe.cppm:190-223` | `lib` / `lib64` / `lib/<triple>` 三种约定拼 runtime 目录 |

这**正是** xlings 侧 `2026-08-06-subos-architecture-proposal.md` §1 诊断出的 P1「一个问题有多个回答者」,只是发生在河的下游。而 xlings 已经从自己那边删到了一个(R2:约定只在写端应用,读端永远不猜)——mcpp 侧还有六个。

判据(R3):**修复如果是「增加一条路径」而不是「移除一条」,它是 workaround。** 下面的设计要能删掉其中的 4–5 条,否则不合格。

### 2.2 为什么「拒绝声明元数据」的判断需要翻转

`linkmodel.cppm` 当年的理由是「唯一的消费者只有这个 resolver」。今天的消费者清单是:

| 消费者 | 需要什么 | 约定能答吗 |
|---|---|---|
| 链接模型 | loader、libdirs、CRT 目录 | 能(勉强) |
| **运行时身份**(新) | glibc **版本**、ABI family | **不能**——目录名不是契约 |
| **subos 视图寻址**(新) | 视图里的 loader 路径(在 `lib/`,载荷在 `lib64/`) | **靠回退顺序碰巧能**(§1.2)——即两个不同布局共用一份猜测 |
| **图形/env 契约**(新) | 哪些 env、哪些值 | **不能** |
| `mcpp pack` | 目标机 loader | 能(硬表) |
| `mcpp doctor` | 悬空链接、版本偏斜 | 部分 |

从一个消费者变成六个,而新增的三个**约定原理上答不了**。R2 的判据在这里是决定性的:一句「没有就自己猜」等于授权每个读端各自实现一份猜测,而读端会随时间增加——mcpp 侧已经从 1 增加到 6。

### 2.3 缺的那一层:libc 轴的契约层在错误的生命周期上

这是最关键的一条,而且 mcpp 自己的代码已经把它写出来了。

`src/build/distribution.cppm:73-77`:

> **NOTE ON SCOPE**:契约管的是 C++ 运行时(stdlib + 它的 ABI 与 unwinder)。**libc 轴是分开的,归 `linkage`/`--static` 管**,部署地板是第三条轴。

三层模型 `Role → Contract → Mechanism` 在 C++ 轴上是完整的:

```
Role      Distributable | Test | Intermediate         ← 内在,用户写不了
Contract  SelfContained | ToolchainCoupled | HostCoupled   ← [build] cxx_runtime
Mechanism (contract × stdlib × 二进制格式) → 链接 flags     ← total function
```

**libc 轴上只有 Mechanism 那一层,而且分散在两个生命周期**:构建期 `--static`,打包期 `--mode`。用户在构建期能说的只有机制,说不了意图(「这个产物要能在没装 mcpp 的机器上跑」)。

而 #336 的分析文档写清了这个形状为什么必然出错:

> 那个 bool 拼的是**机制**(「静态链接 stdlib」),它膨胀成三种不同的平台含义——包括在 Linux/libc++ 上静默无操作:声称 static、产出 toolchain-coupled。契约拼的是**意图**。

**这个观察是对的,但从它推出的结论曾经是错的。** 初稿由此推出「那就补一条 `c_runtime` 轴」——而这条轴唯一的新能力是把产物链到宿主 libc,即策略上被禁的那一侧。**一个真实的架构不对称,不构成做一件被禁的事的理由。** 正确的读法见 §3-S2:用户要的是「能分发」,而 hermetic 的分发路径已经有三条。

这一段保留,是因为那个不对称本身仍然真实,只是它的正确用途是**解释为什么 `mcpp pack` 的默认模式是宿主耦合的**(§3-S2 末尾那笔记账),而不是给 build 期再开一个口子。

**准确说,不是「没有机制」——是机制在错误的生命周期上。** `mcpp pack` 有一个成熟的两轴模型(`docs/02-pack-and-release.md:8-37`:target(libc)× mode(bundling depth),四个模式 `system` / `vendored` / `self-contained` / `static`),`vendored` 默认就会把 PT_INTERP 重指到 `/lib64/ld-linux-*.so.2`(`pack.cppm:649-658`)。所以 **#375 的第 1、2 条症状今天有受支持的答案**。

真正的不对称是这个:

| 轴 | 契约在哪一期可声明 | 怎么实现 |
|---|---|---|
| C++ 运行时 | **构建期**,`[build] cxx_runtime` | 链接时给对 flags |
| C 运行时 / loader | **只有打包期**,`mcpp pack --mode` | 链接后用 patchelf 改写 |

同一类决策(「产物对运行它的机器承诺什么」),一条在构建期声明、一条只能在打包期补救。后果有三个,都是实测的:

1. `mcpp build` 的产物看起来像交付物、其实不是,而**这件事只有读到 pack 文档才知道**——#375 的提问者没走到那一步,直接去套了 `ld.so` 包装
2. `mcpp run` 跑的永远不是将要分发的那个配置,所以分发问题只能在 pack 之后才暴露
3. patchelf 改写是**事后**的:它改得了 PT_INTERP 与 RUNPATH,改不了「链接时就该按目标契约选 RUNPATH 形状」这件事,于是 `self-contained` 只能退回 wrapper 脚本——**即 §1.6 那个缺陷的来源**

> **推论(值得单独强调)**:`/proc/self/exe` 那条症状**不是私有 glibc 的后果,是 wrapper 的后果**;而 wrapper 是「契约无法在链接期表达」的后果。只要 PT_INTERP 指向一个在目标机上真实存在的路径,就不需要 wrapper,`/proc/self/exe` 自然正确。**修契约,第三条症状自己消失——不需要为它单独设计任何东西。**

### 2.4 R6:mcpp 两个用途都绑了载荷,而只有一个该绑

xlings 侧 R6:**内部消费者绑定 payload,不绑定视图。** 三层的消费者不同:

| 层 | 谁该消费 | mcpp 今天 |
|---|---|---|
| payload `data/xpkgs/<pkg>/<ver>` | xlings/libxpkg 自身;**构建期**的 flags | ✅ 用了(正确) |
| subos 视图 `subos/<n>/{bin,lib,usr}` | 用户,以及**用户运行的程序** | ❌ 没用 |
| 每个产物的 RPATH/INTERP | 动态加载器 | ❌ **烙的是 payload** |

mcpp 在**构建期**绑载荷是对的(版本精确、fingerprint 稳定、R6 合规)。
mcpp 在**运行期**绑载荷是错的——那一层的正确锚点是视图。

**一句话:mcpp 用一个地址服务了两个生命周期不同的用途。** 这是所有三个症状的公共上游。

---

## 3. 设计

### S1 — 运行时身份(RuntimeBinding)

**mcpp 停止反推「哪个 libc」,改为读。**

新增一条一等轴 `RuntimeBinding`,词汇与 xlings **逐字一致**(`glibc@2.39` / `musl@1.2.5` / `macos_sdk@14.0` / `ucrt@…`),而不是新造一套。

解析优先级(**每一级都必须显式,不允许「缺省即约定」**——A2):

1. `--runtime <binding>` CLI
2. `[target.<triple>].runtime` / `[build].runtime`
3. **活动 subos 的 `subos_info.runtime`** ← 新的权威源
4. 载荷实测(仅当 subos 无 `subos_info`,即老 subos 的降级路径,且**必须打一行可见提示**)

配套:`abi.cppm:71-106` 的 `libc` 维度从**无版本**(`"glibc"`)升级为**带版本**(`glibc@2.39`),`family_of` 的映射与 xlings `manifest.cppm:81-92` 对齐——但**只在一处推导**。

> **这条同时解掉一个隐性重复账**:mcpp `abi_profile()` 从 target triple 推 `*-linux-gnu → glibc`,xlings `family_of()` 从 runtime 推 `glibc@2.39 → linux-x86_64-glibc`。同一个概念两处推导,而 mcpp 那份没有版本——所以 mcpp 今天**表达不了**「这个依赖需要 glibc ≥ 2.39」。多 glibc 一到,它就是下一个 bug 源。

**删除**:`probe.cppm` 的 glibc 目录爬升与 `{lib64,lib}` 约定(回答者 #1)。

### S2 — 分发路径:修好坏的那条,让三条可发现

> **⚠️ 这一节被整节推翻过一次,推翻的理由比结论更重要。**
>
> 初稿在这里提出补一条 `[build] c_runtime` 轴,三值与 `cxx_runtime` 相同,其中 `host-coupled` = 把产物链到**宿主的 libc**。它已经实现并通过了全部测试,然后被整条撤销(commit 已 reset)。
>
> **撤销理由一:那是照着 issue 提的实现方法做,不是解决 issue 的问题。** #375 的标题写着「让要分发的应用链到系统 libc」——那是提报者**已经想好的解法**。他的**问题**是「产物没法分发」。照着解法做,等于把提报者的方案当成需求。
>
> **撤销理由二:它穿越了这个生态存在的意义所在的那条边界。** hermetic 策略明文列出禁止穿越项,第一条就是「任何 `/usr/lib*` `/lib*` 下的 `.so`(含 libc)」。xlings 是用户态发行版;一个伸手去 `/lib64` 取 libc 的产物已经不在这个发行版里了。**mcpp 能不用 host 就不用 host。**
>
> **撤销理由三:去掉 `host-coupled` 之后它不剩任何新能力。** `self-contained` 已经由 `--target x86_64-linux-musl` 表达,`toolchain-coupled` 是现状。也就是说这条轴的**全部增量就是那个不该有的值**。
>
> 保留这段记录,是因为「同一个东西以后还会被再提一次」——下次有人拿 #375 说「加个链系统 libc 的开关吧」,这里有现成的答案。

**用户的真问题是「产物没法分发」,而它有三条 hermetic 答案,全都已经存在**:

| 路径 | 命令 | libc 从哪来 | 适用 |
|---|---|---|---|
| **A. 生态闭环** | `mcpp emit xpkg` → `xlings install` | 目标机自己的 xlings 载荷,**elfpatch 在装机期重指 PT_INTERP/RUNPATH** | 目标机在生态内 |
| **B. 静态单文件** | `--target x86_64-linux-musl` | 自带,静态链接 | 任何 Linux,无任何运行期依赖 |
| **C. 自带运行时** | `mcpp pack --mode self-contained` | 自带这套工具链的 glibc + loader | 任何 Linux,含比构建机更老的 |

**A 是这个生态真正的答案**,而且它解释了一件容易被误读的事:产物里烙的那个 `PT_INTERP` 指向构建机路径**并不构成分发障碍**——走 A 时目标机的 xlings 会重写它。#375 观察到的「目标机上路径不存在所以起不来」,前提是**绕开生态直接拷贝二进制**。

所以缺口不在机制,在**可发现性**,外加 **C 这条路自己是坏的**(§1.6)。

**要做的两件事,都不新增穿越宿主的能力:**

1. **修 C**(§1.6 已实施):`self-contained` 的 wrapper 打坏 `/proc/self/exe`。这是三条 hermetic 路径里唯一一条自己有缺陷的,而它恰好是「目标机没有 xlings 又不想静态链接」时的那条。
2. **让三条可发现**:`mcpp pack` 与 `mcpp build` 的文档把这三条并列写清楚,`self-contained` 在 glibc 上不可行时的诊断**逐条列出这三条**,而不是只说「不行」。一个只说「不行」的诊断会把用户推向他自己能想到的办法——而那个办法通常就是宿主 libc。

**明确不做**:不新增任何让 `mcpp build` 产出链宿主 libc 的开关。已有的那个决定只有一个入口(`[build] allow_host_libs` / `MCPP_ALLOW_HOST_LIBS`),再开第二个就是「同一决策两处推导」,而且这一次推导出来的是策略上被禁的那一侧。

> **顺带记一笔既有账**(不在本轮改):`mcpp pack --mode vendored` 是 pack 的**默认**模式,而它把 PT_INTERP 重指到 `/lib64/ld-linux-*.so.2`——即默认打包路径本身就是宿主耦合的。这与 hermetic 策略不一致,但改默认会破坏既有用户,需要单独评估。先记在这里。

### S3 — 环境契约(subos 一等公民)

**mcpp 产物需要的第三样东西(configuration)由 subos 提供,mcpp 消费它,而不是自己实现。**

三件事:

1. **写**:mcpp 初始化 sandbox 时确保 subos 有 `subos_info` 块(今天没有,见 §1.3)。不自己造 schema——调 `xlings self init` 的对应路径,或按 schema 1 写。**权威源是 xlings,mcpp 只保证它存在。**

2. **读并应用**:`mcpp run` / `mcpp test` 把 `subos_info.envs` 的**解析结果**(`${subosdir}` 展开后)应用到子进程,而不是今天只设 `LD_LIBRARY_PATH`(`execute.cppm:285`)。
   `LIBGL_DRIVERS_PATH` / `__EGL_VENDOR_LIBRARY_DIRS` / `XDG_DATA_DIRS` 由此**自动**到位——mcpp 侧一行图形相关的代码都不写。

3. **烙进分发物**:`mcpp pack` 把同一份解析结果写进 launcher —— **仅 `toolchain-coupled`**(目标机上有这个 subos);`host-coupled` 不写,因为目标机没有 subos,那里的图形栈是宿主的事。

**图形栈以依赖到达,不以代码到达**:

- mcpp-index 的 `compat.glx-runtime` **废弃**,改为依赖 `xim:graphics`
- 触及 GL 的包(`compat.glfw` / `compat.opengl` / `compat.imgui` / …)声明能力需求,由 capability → xlings 包的解析落到 `xim:graphics`
- 这是 §4 那条一般原则的第一个实例:**xlings 加 Vulkan loader / 换驱动桥 / 支持新宿主形态,mcpp 侧零改动**

---

## 4. 「未来 xlings 升级、mcpp 自动适配」的一般原则

用户的要求里最重要的一条不是修 #375,是「**以后 xlings 环境升级,mcpp 能很简单地动态适配**」。上面三条缝各自解一个症状,而让它们不再复发的是下面这条规则:

> **凡是 xlings 拥有的事实,必须经由恰好一条数据通道到达 mcpp;mcpp 侧对该事实的再推导数必须为 0。**

三条通道,按**生命周期**划分(不是按内容划分——这是关键,内容会变,生命周期不会):

| 通道 | 载体 | 生命周期 | mcpp 何时读 | 承载什么 |
|---|---|---|---|---|
| **C1 载荷事实** | `<payload>/.xpkg-install.json`(需扩展) | 不可变,随版本目录 | 构建期(prepare) | loader、libdirs、abi、CRT 目录 |
| **C2 subos 事实** | `<subos>/.xlings.json` 的 `subos_info` | 可变,随 `xlings use` | 构建期取身份 + 运行期取 env | runtime 身份、env 声明 |
| **C3 宿主能力** | xlings 依赖图里的哨兵包 | 装机期探测 | **不读** | NVIDIA / WSL2 / 无 GPU |

**C3 的正确做法是「什么都不做」**,这条值得单独说:`xim:graphics` 的哨兵机制(`pkgs/g/graphics.lua`)——每个哨兵探测一个自己不拥有的宿主侧半边,不在就「链接了零个东西然后成功返回」——意味着**「这台机器没有那个」是一个正常返回值,不是一个分支**。所以 mcpp 侧永远不需要写 `if (nvidia)`。任何时候如果 mcpp 里出现了探测宿主 GPU/图形能力的代码,它就是这条原则的违反。

**版本偏斜必须可检测,不能静默**:C1/C2 各带 `schema_version`。mcpp 读到不认识的高版本 → 明确降级 + 一行提示;读到缺失 → 走降级路径 + 一行提示。**沉默成功是 xlings 侧诊断出的横切属性(「『没发生』和『成功了』输出相同」),mcpp 侧不要复制它。**

> 这里有一条来自 xlings 侧的硬教训需要照抄(`libs/sysroot.lua:38-47`):**「版本地板写成数据」永远到不了需要被告知的那些客户端**——能读这个字段的客户端恰恰是不需要被告知的那些。所以 mcpp 读 C1/C2 时,能力缺失的处置必须是**运行时可见的一行输出**,不能是一个只有新版本才会检查的字段。

---

## 5. 方案对比

### 5.1 mcpp 怎么拿到 xlings 的事实(§4 C1 的实现形态)

| | A. 直接读 xlings 的磁盘文件 | B. 调 xlings CLI 查询 | **C. xlings 落一份版本化契约文件,mcpp 读** |
|---|---|---|---|
| `subos_info` | ✅ 已存在,可直接读 | 需要新子命令 | ✅ 已存在 |
| 载荷 `exports` | ❌ **没落盘** | 需要新子命令 | 需 xlings 小改(值已算出) |
| 离线 | ✅ | ⚠️ 取决于实现 | ✅ |
| 热路径开销 | 零 | ❌ 每次 prepare 起子进程 | 零 |
| 版本偏斜 | mcpp 复制 schema,易漂 | ❌ **旧 xlings 没有新子命令**,而沙箱 xlings 出了名地不刷新 | schema_version 显式 |
| 违反哪条规则 | 部分违反 R2(读端仍需知道布局) | —— | 无 |

**推荐 C,以 A 为降级路径,B 永不进热路径。**

理由:写端(installer)**已经**把值算全了(`installer.cppm:2394` 的注释明说「Recorded for EVERY runtime dep, declared exports or not」,正是 R1),持久化几乎零成本,而它一次性删掉 mcpp 侧四个再推导。B 在这个生态里有一个具体的、已被记录的失败模式——沙箱里的 xlings 版本长期落后于 pin,一个「新子命令」在最需要它的机器上恰好不存在。

**分期上这个选择是无痛的**:`subos_info`(C2)已经在盘上,所以 **S1 与 S3 的 P0 完全不依赖 xlings 改动**;只有 C1 的 `exports` 持久化需要跨仓协作,排 P1。

### 5.2 为什么不补 `c_runtime` 这条轴

这一节记录一个**被否掉的方案**,因为它已经实现过一遍,而且看起来很有说服力。

| | A. 补 `c_runtime`(含 `host-coupled`) | **B. 不补,修好并指明三条 hermetic 路径** |
|---|---|---|
| 解决「产物没法分发」 | ✅ | ✅ |
| 需要新概念 | 一个 manifest 键 + 一张机制表 + 一条跨层枚举镜像 | 零 |
| 是否新增穿越宿主的能力 | **是** —— 这正是它的全部增量 | 否 |
| 与 hermetic 策略 | **冲突**(禁止穿越第一条就是 `/lib*` 下的 libc) | 一致 |
| 「可不可以用宿主 libc」的回答者数 | **2**(`allow_host_libs` + 新键) | 1 |

**选 B。** A 有三个独立的致命处,任何一个都够:

1. **它是照着 issue 的解法做,不是解 issue 的问题**(§3-S2 撤销理由一)
2. **它穿越了这个生态存在的意义所在的边界**(理由二)
3. **去掉那个值之后它不剩任何能力**(理由三)—— 也就是说 A 列那些 ✅ 全都不是 A 独有的

值得单独记下:A **已经通过了全部测试** —— 12 个契约表单测、7 个渲染单测、一条覆盖五个断言的 e2e(含「默认不变」与「拒绝要出声」)。**测试全绿不能告诉你这个功能不该存在。**

### 5.3 `toolchain-coupled` 用载荷寻址还是视图寻址

> **本节的推荐在评估阶段被下调过。** 初稿推荐视图寻址;查到 `doctor.cppm:369-387` 之后改为「默认不换,除非配套做完两件事」。理由如下。

**新证据**:mcpp 自己的 doctor **已经在检查 subos 视图悬空**,并且写下了成因:

> Dangling symlinks under `registry/subos/default/lib` — these point into xim payload lib dirs; **a removed package leaves them broken**。

也就是说「视图会悬空」不是假想风险,是 mcpp 已经观测到并专门写了检查的现象。而 PT_INTERP 指向不存在的路径时,`exec` 报的是 **`No such file or directory`——指着一个明明存在的文件**,是 Linux 上最经典的假线索之一。

**公平地算完四格**(不能只讲对自己有利的那格):

| 场景 | 载荷寻址(今天) | 视图寻址 |
|---|---|---|
| 载荷升级 2.39→2.44,**保留** 2.39 | ✅ | ✅(glibc 向后兼容) |
| 载荷升级 **+ GC 掉** 2.39 | ❌ 该批坏 | ✅ |
| 载荷被删(无升级) | ❌ 该批坏 | ❌ **全部坏** |
| home 迁移 | ❌ | ❌(链接仍是绝对路径) |

**视图寻址只在第 2 格严格更好,在第 3 格的爆炸半径更差,其余两格相同。** 一格换一格,不是压倒性的。

| | **A. 维持载荷寻址** | B. 换视图寻址 | C. 按 role 分(bin 视图 / test 载荷) |
|---|---|---|---|
| 抗「升级+GC」 | ❌ | ✅ | ✅ |
| 悬空爆炸半径 | 小(按版本分批) | **大(全体)** | 大 |
| e2e 冲击 | 零 | ~4 个断言 PT_INTERP 的用例 | 同 B |
| 可重现性 | ✅ 完全钉死 | ⚠️ 产物行为随 subos 变,**而 fingerprint 覆盖不到**(它是 compile-side) | ⚠️ 同 B |
| 同一轴上的默认值个数 | 1 | 1 | **2** |

**推荐 A(维持现状),除非同时做完这两件事**——做完之后再切 B:

1. **doctor 能自动修复视图悬空**,而不只是报告(今天只 `warn`)。R3 判据:报告不是修复。
2. **exec 失败有人话诊断**:mcpp 在 `run`/`test` 里检测到 PT_INTERP 不可达时,直接说「你的 subos 视图坏了,跑 `mcpp doctor --fix`」,而不是让用户去解读 `No such file or directory`。

**不推荐 C**:同一条轴上两个默认值,而「同一决策两处推导」在这个代码库里已经反复付过学费(#233/#240/#344/#336)。

**这条不再是「最需要拍板」的一条**——降格成机制层开关之后,它可以在 P2 独立评审,不阻塞 P0/P1。

---

## 6. 分阶段落地

### P0 — 零 xlings 改动

按「收益 ÷ 代价」排序,前两条可以立刻做且互不依赖:

1. **文档 + `self-contained` 缺陷**(§1.6):在 `docs/02-pack-and-release.md` 写明 `/proc/self/exe` / `/proc/self/cmdline` 陷阱,并按 §3-S2 的 (i)/(ii) 择一修掉 wrapper。**这是 #375 唯一一条纯粹的既有缺陷**,不依赖本文任何架构改动。
2. **mcpp-index**:`compat.glx-runtime` → `xim:graphics` → **#352 关闭**
3. **S3-读**:`mcpp run` / `mcpp test` 消费 `subos_info.envs`;初始化时确保 subos 有 `subos_info`(§1.3 的缺口)。第 2 条要在真实 GPU 上验成(V4),依赖这一条。
4. **S2-可发现性**:`self-contained` 在 glibc 上不可行时的诊断逐条列出三条 hermetic 路径;`docs/02-pack-and-release.md` 与 `05-mcpp-toml.md` 并列写清 A/B/C。**不新增任何 build 期链宿主 libc 的开关。**

### P1 — 跨仓契约

5. **C1**:xlings 把 `resolved_deps`/`deps_exports` 持久化(schema 版本化)
6. **S1**:`RuntimeBinding` 成为一等轴;`abi.cppm` 的 libc 维度带上版本
7. **删除**:回答者 #1 #2 #4 #6(R3 判据:必须是**删掉**,不是**再加一条**)

### P2 — 生态 / 需先满足前置条件

8. **前置**:doctor 自动修复视图悬空 + exec 失败人话诊断(§5.3 的两个前提)
9. 之后才评审:`toolchain-coupled` 是否切到视图寻址
10. 多 glibc / platform 成员身份 —— 与 xlings 侧 platform manifest 联动(xlings 开放问题 B)

---

## 7. 跨仓契约:需要 xlings 侧配合的**只有一件事**

把 `installer.cppm` 已经算出来的记录落盘。建议形态(字段名沿用 xlings 内部已有的命名,避免第三套词汇):

```jsonc
// <payload>/.xpkg-install.json —— 扩展,不是新文件
{
  "schema_version": 2,
  "os": "linux", "version": "2.39", "xlings_version": "2026.8.7.1",
  "exports": {                       // 本包声明的(self_exports)
    "loader":  "lib64/ld-linux-x86-64.so.2",
    "abi":     "linux-x86_64-glibc",
    "libdirs": ["lib64"]
  },
  "resolved_deps": {                 // R1:每一个 runtime dep 都记,不只声明了的
    "xim:linux-headers@5.11.1": {
      "install_dir": "…", "libdirs": ["…"], "source": "plan-exact"
    }
  }
}
```

三条要求:

- **相对路径**(相对载荷根),不是绝对路径——绝对路径会把 home 位置烙进一个可被复制/硬链接的文件,而 mcpp 侧已经为这类问题付过学费
- `schema_version` 必须有,且 mcpp 读到未知高版本要**降级 + 出声**
- **R1 全量**:每一个 runtime dep 都记,包括什么都没声明的那些。缺省即约定是这一族缺陷的共同上游(A2)

**注意这不是新设计**——`.xpkg-install.json` 已存在、值已算出、字段名已定。只是把一个被丢弃的计算结果写下来。

---

## 8. 明确不做

- **不接管 subos 状态**。mcpp 读 subos、保证 `subos_info` 存在,**不管理** subos 生命周期、不实现 `subos use`、不写 `workspace` 版本 DB。xlings 侧已经把「recipe 里塞 build 逻辑、mcpp 反过来管 subos 状态」点名为层次未分开的症状。
- **不在 mcpp 里探测宿主图形能力**。哨兵机制在 xlings 侧,见 §4 C3。mcpp 里出现 `if (nvidia)` 即为违规。
- **不追 glibc 版本**。mcpp 不实现「选最新可用 libc」——那会让两台同命令的机器产出不同结果(xlings 侧对 `DEFAULT_RUNTIME` 用常量而非查表,理由相同)。
- **不做 platform manifest**。那是 xlings 的开放问题 B,mcpp 侧只需**消费**一个 runtime 身份;platform 成员身份排 P2。
- **不为 `/proc/self/exe` 单独设计任何东西**。见 §2.3 推论——它是 workaround 的后果,契约修好后自动消失。
- **不动 BMI/fingerprint 的缓存身份**,除非 P2 改默认契约时另行评估。契约变化会改链接命令,**不改编译命令**,这是分期能这么切的原因。

---

## 9. 开放问题

- **Q1**(已在评估阶段自我下调,**不再阻塞 P0/P1**)§5.3 载荷寻址 vs 视图寻址:现推荐维持载荷寻址,先把 doctor 自动修复 + exec 失败人话诊断做完,再在 P2 独立评审是否切换。
- **Q2**(新)`mcpp pack --mode vendored` 是 pack 的**默认**,而它把 PT_INTERP 重指到 `/lib64/ld-linux-*.so.2` —— 默认打包路径本身就是宿主耦合的,与 hermetic 策略不一致。改默认会破坏既有用户,需单独评估:改默认、打告警、还是维持并在文档里说清。
- **Q3** 多 subos:mcpp 有 home 级 sandbox 和项目级 subos(实测 `<proj>/.xlings/subos/_/`)。S3 该读**哪一个**的 `subos_info`?倾向「构建时活动的那个」,但需要确认它在 CI 与本机的一致性。(视图寻址一旦在 P2 启用,同一个问题会变成「烙哪一个」,风险更高——这是又一条把它排到 P2 的理由。)
- **Q4** 交叉编译时 `subos_info.runtime` 描述的是宿主 subos,目标 runtime 从哪来?可能需要 `[target.<triple>].runtime` 强制显式(而不是有一个缺省)。
- **Q5**(回答 xlings 开放问题 K)本文的答案是「**消费 subos 的身份,而不是在 subos 里跑**」——mcpp 读 `subos_info.runtime` 作为契约输入,构建仍在 mcpp 自己的 hermetic 环境里。是否与 xlings 侧的设想一致,需要跨仓确认。
- **Q6**(需你拍板)§3-S2 的 `self-contained` wrapper:选 (i) 安装期改写 PT_INTERP(消灭问题,但 tarball 不再解开即跑),还是 (ii) 保留 wrapper + `MCPP_BUNDLE_DIR` + 文档(不破坏现有契约,但救不了第三方库)?这条**独立于**本文其余部分,可以先决先做。

---

## 10. 验证判据(每条可执行,先红后绿)

| # | 判据 | 怎么测 |
|---|---|---|
| V1 | 三条 hermetic 路径各自在**无 mcpp** 的机器上跑通 | 干净容器里分别验:A `xlings install` 后跑;B musl 产物原样拷进去跑;C `pack --mode self-contained` 解开就跑。**必须是真实干净容器** —— 在 mcpp 沙箱里跑测不出东西,那里载荷路径恰好总是存在 |
| V2 | **每一个** pack 模式产出的程序,`/proc/self/exe` 都指向自身 | 产物内打印 `readlink("/proc/self/exe")` 并断言 == 自身路径,**四个模式全跑**。今天 `self-contained` 必红(§1.6),这条就是它的先红后绿 |
| V3 | (仅当 P2 切视图寻址)产物在载荷升级+GC 后仍可运行 | 装 glibc@2.44、`xlings use`、**删掉 2.39**,不重新构建直接跑旧产物 |
| V4 | GL 程序在 hermetic 图形栈下起窗口 | `xlings install graphics` 后跑 GLFW e2e;**断言渲染器不是 llvmpipe**——「跑起来了」是假绿,#352 的教训是要问「谁答的」 |
| V5 | mcpp 侧 loader/libdir 推导点数量单调下降 | 对 §2.1 六个位置做静态计数,CI 守住上界 |
| V6 | subos 无 `subos_info` 时**出声降级** | 删掉块,断言 stderr 有一行,且构建仍成功 |
| V7 | 产物不引用任何 `/usr/lib*` `/lib*` 下的 `.so` | 对 `mcpp build` 产物做 `ldd` 闭包扫描,断言无宿主路径命中(`allow_host_libs` 显式开启时豁免)|

> **两条来自本仓历史的验证陷阱,必须避开**:
> - **CI 全绿不等于覆盖**(#346:18 个 job 全绿也没测到大链接)。V1/V3 必须在**真实的干净容器**里跑,不能只在 CI 的 mcpp 沙箱里跑——那里 PT_INTERP 恰好总是存在。
> - **「有输出」不是判据**(#352 的 exit 255 无输出;`VS_VERSION_INFO` 全文搜索是假判据)。V4 必须断言**渲染器身份**,不是断言窗口出现。

---

## 11. 多维评估

本节的每个数字都在本机量过。**结论先行:P0 的三条(文档/wrapper、图形栈、S3-读)是低风险高收益,可以直接做;S2 中等;S1 是唯一有实质回归风险的一条,排 P1 是对的;P2 目前不该做。**

### 11.1 实现代价(量化)

| 项 | 数字 | 依据 |
|---|---|---|
| ~~`c_runtime` 实现面~~ | ~~9 文件~~ | **已撤销**(§5.2)。实测过:9 文件 / `flags.cppm` 约 30 处 / 12+7 单测 / 1 条 e2e 全绿 —— 记在这里是为了说明「代价可控」从来不是做不做的判据 |
| **BMI / 对象缓存失效** | **零** | fingerprint 是 **compile-side 10 字段**(`fingerprint.cppm:3-8`),不含链接侧;`target/<fp>/` 目录名不变,只重链 |
| e2e 需改 | ~4 个(203 个中) | 断言 PT_INTERP 的:`30_pack_modes` `86_llvm_hermetic_link` `28_target_static` `168_build_mcpp_musl_host_static` |
| e2e 耦合载荷路径 | 58 处 / 26 文件 | 大多是工具链解析,不是 PT_INTERP;S1 落地时才受影响 |
| mcpp-index 破坏面 | **3 个包 / 81** | `compat.glfw` `compat.glx-headers` `compat.vulkan-runtime` |

**「零缓存失效」是这套方案最大的成本优势**,和 #336 当年便宜的原因相同:契约只改链接命令。

**净增 / 净删**:新增 1 个 manifest 键 + 1 张 mechanism 表 + 1 个 RuntimeBinding 类型 + 2 条读文件路径;删除 §2.1 六个回答者里的 4–5 个,外加整个 `compat.glx-runtime`(约 130 行宿主探测 Lua)。满足 R3。

### 11.2 用户使用层

| 用户 | 影响 |
|---|---|
| **普通用户**(build/run/test) | P0 全部**零感知**;S3 让 GL 程序从「exit 255 无输出」变成能跑,是纯增益 |
| **分发者** | `pack --mode self-contained` 从「静默打坏 exe 相对资源解析」变成可用(§1.6),这是三条 hermetic 路径里唯一一条自己有缺陷的;另外两条(A 生态闭环 / B musl 静态)本来就能用,缺的是文档 |
| **库作者** | `compat.glx-runtime` 废弃是破坏性变更,需过渡期(保留空壳 provider 一个版本) |

**新增认知负担:零。** 不加键、不加轴、不加词汇。用户模型仍是「target × pack mode」,只是三条分发路径终于被并列写出来了。这是选 B 而非 A 的直接收益之一。

### 11.3 稳定性

**新增失败模式三个,两个可控、一个未受控:**

| 失败模式 | 处置 | 评价 |
|---|---|---|
| 读 `subos_info` 失败 / 缺失 | 出声降级(V6) | 可控 |
| xlings schema 演进 | `schema_version` 显式 + 降级出声 | 可控 |
| **视图寻址下产物行为随 subos 变化** | **fingerprint 覆盖不到**(compile-side) | **未受控** —— 这是把它降级到 P2 并加两个前置条件的直接原因(§5.3) |

**P0/P1 本身不引入新的运行期失败模式**:契约层只是把已有机制(pack 的 patchelf)提前到链接期,产出的 PT_INTERP 形状是 `30_pack_modes.sh` 今天已经在断言的那些。

### 11.4 跨平台(最弱的一维,必须直说)

| 平台 | 分发路径可用性 | S1 runtime 身份 | S3 env / 图形 |
|---|---|---|---|
| Linux / glibc | **3** | `glibc@X`,有意义 | ✅ 全部收益 |
| Linux / musl | 1(恒 self-contained) | `musl@X` | 部分 |
| **macOS** | **1** —— libSystem 恒为宿主(`platform/common.cppm:110` `supports_full_static = is_linux`) | 近乎常量 | **无**(无 mesa/glvnd) |
| **Windows** | 2(静/动 CRT),且与 `linkage` 共用 `-static` 拼写 | 近乎常量 | **无** |

**收益 ≈ 全在 Linux。** macOS 上这条轴是退化的——只有一个合法值。

macOS/Windows 上没有 mesa/glvnd,所以 S3 的图形收益是 Linux-only;而分发路径 A/B/C 里,B(musl 静态)本身就是 Linux 概念。**这是诚实的不对称,不是退化** —— 因为本轮不新增任何全平台的轴,也就不存在「为一个平台的问题给三个平台加概念」。

若 review 认为 Linux-only 收益不值一条全局轴,替代是放进 `[target.'cfg(linux)']`。**我不推荐**——那会引入第二套作用域规则,而 `cxx_runtime` 已经确立了「全局轴 + 平台退化」的先例。

### 11.5 简洁优雅

**正面**:零新词汇(三值抄 `cxx_runtime`,runtime binding 抄 xlings);三层模型已存在,新轴是**填满一个已知空格**而非加一个维度;§4 的三通道按**生命周期**而非内容划分,所以 xlings 加新能力时通道数不变。

**负面(必须记账)**:
- 概念总数确实上升,§11.2 已列。
- **初稿的 `subos-coupled` 第四值破坏了对称性**——这是评估阶段发现并已修正的一处真实设计缺陷(§3-S2 的框)。修正之后词汇仍是三个,且「视图 vs 载荷」回到它本来的层(Mechanism)。**如果没做这次评估,这条会带着一个多余的契约值进入实施。**

### 11.6 兼容性

| 面 | 影响 | 风险 |
|---|---|---|
| 已构建产物 | 不受影响(载荷目录仍在) | 无 |
| `[pack].default_mode` / tarball 后缀 frozen wire format | **不动** | 无 |
| 老 subos 无 `subos_info` | 降级 + 提示 | 低 |
| 老 xlings 无持久化 exports | 走 §5.1 的 A 降级路径;P1 才依赖 | 低 |
| `compat.glx-runtime` 下游 | 3 个包 | **低于预期**(初判以为是索引级风险,实测是叶子簇) |
| **新 mcpp + 老 xlings** | 必须验 | **中** —— 已知「沙箱 xlings 长期落后于 pin」,这一格最容易假绿 |

### 11.7 总评

| 维度 | P0(wrapper 修复 + 图形栈 + S3-读) | S2(分发路径可发现性) | S1(RuntimeBinding) | ~~c_runtime~~ / P2 视图寻址 |
|---|---|---|---|---|
| 实现代价 | 低 | 中 | 中高(动 `abi.cppm`,参与依赖解析) | 低 |
| 用户收益 | **高**(#352 从不可用变可用) | 中高 | 低(内部收敛) | 低 |
| 稳定性风险 | 低 | 低 | **中**(ABI 维度回归) | **高**(未受控) |
| 跨平台价值 | Linux-only | Linux 高 / Win 中 / mac 退化 | 全平台(收敛推导) | Linux-only |
| 兼容性风险 | 低(3 个包) | 低 | 中 | 中 |
| **建议** | **直接做** | **做** | **P1,单独评审** | **暂不做** |
