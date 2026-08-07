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
| **S2** | **链接契约** | C++ 运行时契约在**构建期**可声明,C 运行时契约**只有打包期**有 | 补 `c_runtime` 轴,与 `cxx_runtime` 同词汇、同 total function |
| **S3** | **环境契约** | mcpp 产物拿不到 subos 的 env,图形栈靠 mcpp 代码硬扛 | 消费 `subos_info.envs`;图形栈以**依赖**而非代码到达 |

**关键结论:P0 需要 xlings 零改动。** `subos_info` 已经存在,subos view 已经填充好(`subos/default/lib/ld-linux-x86-64.so.2` 实测在位)。唯一需要 xlings 配合的是**把 installer 已经算出来、当前只活在进程里的 `resolved_deps`/`deps_exports` 持久化**——那是 P1。

**多维评估见 §11**(实现代价 / 用户 / 稳定性 / 跨平台 / 简洁 / 兼容性,每项带实测数字)。三条要点:

- **零 BMI/对象缓存失效** —— fingerprint 是 compile-side,契约只改链接命令
- **收益 ≈ 全在 Linux**,macOS 上这条轴退化成单值(有 `cxx_runtime` 在 MSVC 上同样退化的先例可援)
- **评估过程改掉了初稿的两处**:①`subos-coupled` 不该是第四个契约值(它是 Mechanism 不是 Contract);②视图寻址从「建议新默认」下调为「P2 且带两个前置条件」——因为 `doctor.cppm` 已经记录了视图会悬空,而爆炸半径是全体产物

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

**mcpp#375 就是同一句话在 libc 轴上的复述。** 用户想要的是意图(「这个产物要能在没装 mcpp 的机器上跑」),而在 `mcpp build` 期能写的只有机制(`--static`,对 glibc 无解)。

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

### S2 — 链接契约(`c_runtime`)

补上缺失的 Contract 层,**照抄 `cxx_runtime` 的三层形状与词汇**,不发明新概念。

```toml
[build]
cxx_runtime = "self-contained"     # 已有
c_runtime   = "host-coupled"       # 新增(这里写的是「要能分发」)
```

**词汇是三个,和 `cxx_runtime` 逐字相同**——这一点在评估阶段被修正过一次,理由见下面的框:

| Contract | PT_INTERP | RUNPATH | 语义 | 目标场景 |
|---|---|---|---|---|
| `self-contained` | 无(静态) | 无 | 不依赖外部 libc | musl 全静态 |
| `toolchain-coupled`(默认) | 见下:**载荷寻址** 或 **视图寻址** | 同左 | 只在装了这套工具链的机器上可跑 | 开发循环、生态内分发 |
| `host-coupled` | `/lib64/ld-linux-*.so.N`(LSB) | `$ORIGIN/../lib` | manylinux 模型:宿主 glibc ≥ 构建期版本即可跑 | **#375 要的那个** |

> **⚠️ 修正(评估阶段发现)**:本文初稿把「视图寻址」写成第四个契约值 `subos-coupled`。**那是错的**,两条理由:
> 1. `cxx_runtime` 只有三个值,加第四个立刻破坏「同词汇」这个本设计唯一的优雅性论据;而且第四个值在 macOS/Windows 上无对应物。
> 2. 它根本不是一个**契约**——契约描述「对运行它的机器承诺什么」,而载荷寻址与视图寻址对目标机的承诺**完全相同**(「这台机器装了这套工具链」)。它们的差别是**寻址方式**,属于 Mechanism 层。
>
> 所以:`toolchain-coupled` 保持为一个契约值,其 Mechanism 有两种实现——

| Mechanism(`toolchain-coupled` 内部) | PT_INTERP / RUNPATH | 抗载荷升级 | 爆炸半径 |
|---|---|---|---|
| **载荷寻址**(今天) | `…/xpkgs/xim-x-glibc/2.39/lib64/…` | ❌ 载荷被 GC 即失效 | 只影响针对该版本构建的那批 |
| **视图寻址** | `<subos>/lib/…` | ✅ 视图跟随活动版本 | ⚠️ **一个符号链接悬空 = 所有产物同时坏** |

这把「要不要换默认」从一个**契约层的评审**降格成一个**机制层的开关**,评审面和风险都小一圈。选哪个见 §5.3。

Mechanism 层同样是 **total function**:每个格子都有答案,兑现不了的格子返回 `degraded` + 非空 `diagnostic`,调用方必须上抛。这条是 `distribution.cppm` 已经立下的规矩,直接沿用——**「静默无操作」是这个模块存在的意义所要杜绝的那一个结果**。

**默认值:`toolchain-coupled` 不变**(= 今天的行为),所以引入这条轴**本身不改变任何现有产物**。视图寻址是 `toolchain-coupled` 内部的机制开关,排 P2,判据见 §5.3。

一个无论何时切换都必须成立的前提:**仅当 `family_of(subos_info.runtime)` 与目标 triple 的 `{os, arch, libc}` 相符时才允许视图寻址**;交叉编译(目标 aarch64、视图是 x86_64)必须落回载荷寻址。这是一个带判据的派生选择,不是静默默认值。

**与 `mcpp pack` 两轴模型的关系(重要:不能变成第三个回答者)**

`pack` 的两轴是 **target(libc)× mode(bundling depth)**。新的 `c_runtime` **不是第三条轴**,它是 pack 的 mode 轴一直在隐式表达、却只能在打包期表达的那条**契约**。对应关系必须是一个函数,不是一张需要人对齐的表:

| `c_runtime` 契约 | pack mode | 关系 |
|---|---|---|
| `self-contained` | `static` | 契约决定 mode 的合法集合 |
| `host-coupled` | `system` / `vendored` | 二者差别是**打包深度**(带多少第三方 `.so`),不是契约——契约都是「宿主提供 libc + loader」 |
| `toolchain-coupled` | (不可分发) | pack 时必须报错或提升契约,**不能静默产出一个跑不起来的 tarball** |

也就是说:**mode 继续管「带多少东西」,contract 管「对目标机承诺什么」**。今天 mode 同时承担了两件事,这是它必须在打包期才能决定的原因。拆开之后,`mcpp pack` 的 mode 语义不变、别名不变、tarball 后缀这个 frozen wire format 不变。

**`self-contained` 的 wrapper 要一并处理**(§1.6):ELF 禁止 `PT_INTERP` 用 `$ORIGIN` 是硬约束,所以「不用 wrapper」需要另一条路。两个候选,留给 review:

- **(i) 安装期改写**:解包时把 PT_INTERP 改成解包目录的绝对路径(conda / AppImage 系的做法)。mcpp 已经有 patchelf 管线,`/proc/self/exe` 完全正确。代价:多一个安装步骤,tarball 不再是「解开就能跑」。
- **(ii) 保留 wrapper + 显式补偿**:wrapper 里导出 `MCPP_BUNDLE_DIR`,文档写明陷阱,并提供一个「先看 `MCPP_BUNDLE_DIR` 再退回 `/proc/self/exe`」的推荐解析顺序。代价:需要应用配合,救不了第三方库(GUI 框架的字体解析)。

倾向 (i)——它把问题真正消灭而不是转嫁给应用,符合 R3(删掉一个回答者,而不是再加一条路径)。但它改变了 `self-contained` 的用户契约,需要你拍板。

**删除/收敛**:回答者 #2 #3 #5 收敛为一处「按 contract 求 PT_INTERP」的 total function;`pack.cppm` 不再自己拼 `/lib64/`,改为向该函数请求。

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

### 5.2 `c_runtime` 该不该是新轴(而不是复用已有开关)

| | A. 复用 `--static` / `linkage` | B. 让 `cxx_runtime` 一并管 libc | D. 维持现状(只有 pack `--mode`) | **C. 新增同族 `c_runtime` 轴** |
|---|---|---|---|---|
| 能表达 `host-coupled` glibc 吗 | ❌ glibc 静态链接不可行 | —— | ✅ 打包期能 | ✅ 构建期就能 |
| `mcpp build` / `mcpp run` 能跑将要分发的配置吗 | ❌ | —— | ❌ **只能 pack 后才知道** | ✅ |
| 与 #336 的结论一致吗 | ❌ 正是 #336 判定为错的形状(机制冒充意图) | ❌ `distribution.cppm:73` 写明了两轴分离的理由 | ⚠️ 同一类决策两个生命周期两套词汇 | ✅ |
| 新概念数 | 0 | 0 | 0 | 0(词汇、三层、total function 全部照抄) |
| 代价 | —— | —— | **零**(什么都不做) | 一个新 manifest 键 + 一张 mechanism 表 |

**推荐 C。**

- **B** 看似更省,但 `distribution.cppm` 已经把两轴合并的后果写清楚了:一个 `static_stdlib` bool 膨胀成三种平台含义,并在 Linux/libc++ 上静默无操作。C++ 与 C 的运行时是两条独立可组合的轴(自带 libc++ + 用宿主 glibc 是完全合法的一组),合并会立刻产生表达不了的格子。
- **D 是需要认真对待的对照组**——它代价为零,而且今天确实能把产物分发出去。**不选它的理由只有一条,但这条足够**:「产物对运行它的机器承诺什么」这一个决策,在 C++ 轴上是构建期属性、在 C 轴上是打包期属性,两套词汇。这个代码库里「同一决策两处推导」的账已经反复付过(#233/#240/#344 是同一台机器,#336 本身就是第五次),而它的表现形式一贯是:**加新语义时变成构建失败,或者更糟——静默产出错的东西**。§1.6 的 wrapper 缺陷就是这笔账已经开始收利息的证据。

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
4. **S2**:落 `c_runtime` 契约层 + Mechanism total function;`host-coupled` 在**构建期**打通 → **#375 的架构诉求关闭**(第 1、2 条症状今天已可用 `mcpp pack` 解决,见 §2.3;这里补的是「在链接期声明意图」的能力)

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
- **Q2** `host-coupled` 时依赖包的 `.so` 怎么走?`$ORIGIN/../lib` + 打包时收集(= 今天 `mcpp pack` 的 `BundleProject`),还是要求依赖也 `host-coupled`?倾向前者,但要确认与 `cxx_runtime` 的组合矩阵每格都有答案。
- **Q3** 多 subos:mcpp 有 home 级 sandbox 和项目级 subos(实测 `<proj>/.xlings/subos/_/`)。S3 该读**哪一个**的 `subos_info`?倾向「构建时活动的那个」,但需要确认它在 CI 与本机的一致性。(视图寻址一旦在 P2 启用,同一个问题会变成「烙哪一个」,风险更高——这是又一条把它排到 P2 的理由。)
- **Q4** 交叉编译时 `subos_info.runtime` 描述的是宿主 subos,目标 runtime 从哪来?可能需要 `[target.<triple>].runtime` 强制显式(而不是有一个缺省)。
- **Q5**(回答 xlings 开放问题 K)本文的答案是「**消费 subos 的身份,而不是在 subos 里跑**」——mcpp 读 `subos_info.runtime` 作为契约输入,构建仍在 mcpp 自己的 hermetic 环境里。是否与 xlings 侧的设想一致,需要跨仓确认。
- **Q6**(需你拍板)§3-S2 的 `self-contained` wrapper:选 (i) 安装期改写 PT_INTERP(消灭问题,但 tarball 不再解开即跑),还是 (ii) 保留 wrapper + `MCPP_BUNDLE_DIR` + 文档(不破坏现有契约,但救不了第三方库)?这条**独立于**本文其余部分,可以先决先做。

---

## 10. 验证判据(每条可执行,先红后绿)

| # | 判据 | 怎么测 |
|---|---|---|
| V1 | `mcpp build`(不经 pack)产出的 `host-coupled` 产物在**无 mcpp** 的机器上直接跑通 | 容器里只装 glibc ≥ 2.39,把 `target/**/bin/hello` **原样**拷进去执行。**不经 pack 是判据的一部分**——经 pack 今天就能过,测不出新东西 |
| V2 | **每一个** pack 模式产出的程序,`/proc/self/exe` 都指向自身 | 产物内打印 `readlink("/proc/self/exe")` 并断言 == 自身路径,**四个模式全跑**。今天 `self-contained` 必红(§1.6),这条就是它的先红后绿 |
| V3 | (仅当 P2 切视图寻址)产物在载荷升级+GC 后仍可运行 | 装 glibc@2.44、`xlings use`、**删掉 2.39**,不重新构建直接跑旧产物 |
| V4 | GL 程序在 hermetic 图形栈下起窗口 | `xlings install graphics` 后跑 GLFW e2e;**断言渲染器不是 llvmpipe**——「跑起来了」是假绿,#352 的教训是要问「谁答的」 |
| V5 | mcpp 侧 loader/libdir 推导点数量单调下降 | 对 §2.1 六个位置做静态计数,CI 守住上界 |
| V6 | subos 无 `subos_info` 时**出声降级** | 删掉块,断言 stderr 有一行,且构建仍成功 |
| V7 | `c_runtime × cxx_runtime × 格式` 矩阵无静默格 | 单测遍历全矩阵,断言每格要么有 flags 要么 `degraded` + 非空 diagnostic |

> **两条来自本仓历史的验证陷阱,必须避开**:
> - **CI 全绿不等于覆盖**(#346:18 个 job 全绿也没测到大链接)。V1/V3 必须在**真实的干净容器**里跑,不能只在 CI 的 mcpp 沙箱里跑——那里 PT_INTERP 恰好总是存在。
> - **「有输出」不是判据**(#352 的 exit 255 无输出;`VS_VERSION_INFO` 全文搜索是假判据)。V4 必须断言**渲染器身份**,不是断言窗口出现。

---

## 11. 多维评估

本节的每个数字都在本机量过。**结论先行:P0 的三条(文档/wrapper、图形栈、S3-读)是低风险高收益,可以直接做;S2 中等;S1 是唯一有实质回归风险的一条,排 P1 是对的;P2 目前不该做。**

### 11.1 实现代价(量化)

| 项 | 数字 | 依据 |
|---|---|---|
| `c_runtime` 实现面 | ~9 个文件,`flags.cppm` 约 30 处 | 以同构的 `cxx_runtime` 实测面积为代理 |
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
| **分发者** | 收益最大:`c_runtime = "host-coupled"` 让 `mcpp build` 直接产出可分发物,`mcpp run` 跑的就是将要分发的配置。**但对已经知道 `mcpp pack` 的人,增量只是「早一步发现问题」** |
| **库作者** | `compat.glx-runtime` 废弃是破坏性变更,需过渡期(保留空壳 provider 一个版本) |

**新增的认知负担是真实的**:用户模型从「target × pack mode」变成「+ `c_runtime` + `cxx_runtime`」。缓解只有一条——三值词汇完全相同,学一次用两处;且 `c_runtime → 合法 pack mode` 是函数关系而非需要人工对齐的表(§3-S2)。

### 11.3 稳定性

**新增失败模式三个,两个可控、一个未受控:**

| 失败模式 | 处置 | 评价 |
|---|---|---|
| 读 `subos_info` 失败 / 缺失 | 出声降级(V6) | 可控 |
| xlings schema 演进 | `schema_version` 显式 + 降级出声 | 可控 |
| **视图寻址下产物行为随 subos 变化** | **fingerprint 覆盖不到**(compile-side) | **未受控** —— 这是把它降级到 P2 并加两个前置条件的直接原因(§5.3) |

**P0/P1 本身不引入新的运行期失败模式**:契约层只是把已有机制(pack 的 patchelf)提前到链接期,产出的 PT_INTERP 形状是 `30_pack_modes.sh` 今天已经在断言的那些。

### 11.4 跨平台(最弱的一维,必须直说)

| 平台 | `c_runtime` 有几个有意义的值 | S1 runtime 身份 | S3 env / 图形 |
|---|---|---|---|
| Linux / glibc | **3** | `glibc@X`,有意义 | ✅ 全部收益 |
| Linux / musl | 1(恒 self-contained) | `musl@X` | 部分 |
| **macOS** | **1** —— libSystem 恒为宿主(`platform/common.cppm:110` `supports_full_static = is_linux`) | 近乎常量 | **无**(无 mesa/glvnd) |
| **Windows** | 2(静/动 CRT),且与 `linkage` 共用 `-static` 拼写 | 近乎常量 | **无** |

**收益 ≈ 全在 Linux。** macOS 上这条轴是退化的——只有一个合法值。

**但这是「诚实的退化」而非「破坏」,且有直接先例**:`cxx_runtime` 在 Windows/MSVC 上同样退化,`distribution.cppm` 用 `explicitRequest` 处理——**默认不吭声,显式写了才告诉你没实现**。`c_runtime` 照抄即可,不需要新机制。

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

| 维度 | P0(文档/wrapper + 图形栈 + S3-读) | S2(`c_runtime`) | S1(RuntimeBinding) | P2(视图寻址) |
|---|---|---|---|---|
| 实现代价 | 低 | 中 | 中高(动 `abi.cppm`,参与依赖解析) | 低 |
| 用户收益 | **高**(#352 从不可用变可用) | 中高 | 低(内部收敛) | 低 |
| 稳定性风险 | 低 | 低 | **中**(ABI 维度回归) | **高**(未受控) |
| 跨平台价值 | Linux-only | Linux 高 / Win 中 / mac 退化 | 全平台(收敛推导) | Linux-only |
| 兼容性风险 | 低(3 个包) | 低 | 中 | 中 |
| **建议** | **直接做** | **做** | **P1,单独评审** | **暂不做** |
