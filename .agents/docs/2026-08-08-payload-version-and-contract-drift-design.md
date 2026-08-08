# 载荷版本与契约漂移:四个缺陷,一条线

**日期**:2026-08-08
**性质**:设计提案,待 review。本文不改代码。
**触发**:2026.8.8.1 发布当天暴露的四个缺陷,其中三个是我在这一轮自己引入或自己漏掉的。

**关联**:
- `.agents/docs/2026-08-07-xlings-as-runtime-substrate-design.md`(本轮的上游设计)
- mcpp#352 / #375、mcpp-index#179(已 revert,PR#180)、mcpp#377
- xlings `.agents/docs/2026-08-06-subos-architecture-proposal.md` 的 R1–R7

---

## 0. TL;DR

四个缺陷,**同一条线**:

> **mcpp 对 xlings 的每一项事实,都是「自己猜一个」而不是「读一个」;而它猜的那个,和它烙进产物的那个,是两个独立的答案。**

| # | 缺陷 | 谁引入 | 现状 |
|---|---|---|---|
| **D1** | 载荷探测取**目录序第一个**(注释说是最高,代码没排序),与 gcc 烙定的不是同一份 | 早已存在,本轮触发 | **main 曾变红,已 revert** |
| **D2** | subos_info **wire format 读错**,功能完全不工作 | 本轮我引入 | PR#377 已修 |
| **D3** | 升级后**复用升级前缓存**,修复静默不生效 | 本轮我引入 | PR#377 已修 |
| **D4** | 沙箱 xlings **永不升级**,且 mcpp 打印偏差却不作为 | 早已存在 | **未修** |

D1 和 D4 是同一件事的两面:**mcpp 把一个可变的外部世界当成了装机时的快照**。D2 和 D3 是另一件事的两面:**我用与实现同源的理解去写测试,于是测试证明的是「我和我自己一致」。**

---

## 1. D1:载荷探测与 gcc 烙定的版本是两个独立答案

### 1.1 实测

`compat.glx-runtime` 加了一条 `xim:graphics` 依赖(为修 #352)。`mesa` 声明:

```lua
"xim:glibc@>=2.38",   -- 下界,不是钉死
```

解析器装了 **glibc 2.44**,与既有的 2.39 并存。然后:

```
.../xim-x-glibc/2.39/lib64/libc.so.6: version `GLIBC_2.42' not found
                                      (required by <测试二进制>)
```

**在 `asio-module`、`core` 上炸 —— 与图形毫无关系的包。**

### 1.2 机制

`probe.cppm` 调 `find_sibling_tool(compilerBin, "glibc")`。那个函数的注释说它取最高版本 —— **代码没有排序**:

```cpp
// Return the first (highest) version dir that exists.
for (auto& v : std::filesystem::directory_iterator(root, ec)) {
    if (v.is_directory(ec)) return v.path();
}
```

`directory_iterator` 是无序的,所以真实行为是**取 readdir 碰巧给出的第一个**。本机实测顺序是 `2.44, 2.39`,于是选中 2.44 —— 但**换台机器、装个别的包、甚至删一个目录,结果都可能变**。这是本文档记录的第三处「注释描述了代码没有的行为」(另两处见 §3.3)。

于是:

| | 谁决定 | 结果 |
|---|---|---|
| **编译/链接** 对着哪份 glibc | mcpp 的探测(readdir 第一个) | **2.44**(本机;换台机可能不同) |
| **运行时** PT_INTERP / specs | gcc 载荷装机时 elfpatch 烙定 | **2.39** |

两个独立的答案,**在只装一份 glibc 时恒相同**,所以从未被迫达成一致 —— 这正是 xlings 侧 R1/R2 诊断出的 P1「一个问题多个回答者」,原样出现在 mcpp。

### 1.2b 为什么「新 glibc 向后兼容」救不了这一格

glibc 的兼容性是**单向**的:对 2.39 构建的产物在 2.44 上跑 ✅;对 2.44 构建的在 2.39 上跑 ❌。本次报错正是第二种 —— 加载了 2.39 的 `libc.so.6`,而二进制要 `GLIBC_2.42` 的符号。**问题不是运行时太老,是编译期跑到前面去了而运行期没跟上。**

### 1.2c 参照系:发行版、Nix、和 mcpp 现在在哪

| | 发行版(Arch / Fedora) | Nix / xlings | **mcpp 今天** |
|---|---|---|---|
| 同时几份 glibc | **1** | 多份并存 | 多份并存 |
| 路径 | **无版本** `/lib64/ld-linux-x86-64.so.2` | 带版本 `…/2.44/lib64/…` | 带版本 |
| 升级 | **就地替换同一个文件** | 在旁边装一份新的 | 在旁边装一份新的 |
| 编译期与运行期的一致性 | **构造上不可能分歧** | 每个消费者冻结自己的选择 | **两个独立决定者** ❌ |

滚动发行版之所以能一直换 glibc 而不炸,是因为**编译侧和运行侧是同一个文件**;新 glibc 保留全部旧符号版本,所以既有二进制照跑。

**Arch 上唯一会出这个错的场景,与本次同型:部分升级**(`pacman -Sy pkg` 不带 `-u`)—— 装了一个对着更新 glibc 构建的包,而 glibc 没跟着升。Arch 明文禁止部分升级,理由一模一样。

**结论:mcpp 用了 Nix 的布局(多版本并存 + 版本化路径),但没有兑现那个布局的前提 —— 每个消费者的选择必须只有一个决定者。** 这不是「要不要支持多 glibc」的问题,而是既有布局的前提没被满足。

### 1.2d xlings 早就预见了,而一条下界绕开了它

`glibc.lua` 明写 `latest` 故意停在 2.39:

> Moving `latest` to 2.44 would be safe for the same reason — but it would also **silently change which glibc every existing home resolves to**, and that is a decision to make deliberately rather than as a side effect of adding a version.

而 `mesa` 的 `xim:glibc@>=2.38` **绕开了 `latest` 指针**,把 2.44 直接拉了进来。一个刻意做出的「不动 latest」的决定,被一条依赖的下界从侧面推翻。

**这一条值得同步给 xlings**:范围解析取最高满足者,与 `latest` 别名承载的「这个 home 该用哪一份」是两个不同的意图,而前者会静默压过后者。是否该让范围解析在 `latest` 满足区间时优先取 `latest`,是 xlings 侧的开放问题。

### 1.3 判据:它不是「装了新 glibc」的问题

**任何**让 home 里多出一份更高版本 glibc 的操作都会触发它 —— 一条新依赖、一次 `xlings install`、一个新包。**这个雷一直在,只是这次我踩了。**

### 1.4 方案

| | A. 探测改为「与 gcc 烙定的那份一致」 | B. 探测取最高但校验一致性,不一致就报错 | C. 维持现状 |
|---|---|---|---|
| 编译/运行是否可能分裂 | **不可能** | 可能,但会被拦下 | 会,且静默 |
| 需要什么输入 | gcc 烙定的 glibc 版本 | 同左 | — |
| 多 glibc 场景 | 天然正确(跟工具链走) | 需要人工介入 | 坏 |
| 结果是否确定 | ✅ | ✅ | ❌ **随目录序变** |

**推荐 A,B 作为过渡期的护栏。**

「gcc 烙定的是哪份」不需要猜:`post_install.cppm` 已经有 `extract baked loader from clang cfg` 的能力(扫 `/ld-linux-`),而 gcc 侧的 specs 里也写着。**更好的做法是让 xlings 落盘**(见 §5),但即使不落盘,从 gcc 自己的产物反推也比「取最高」正确 —— 因为它反推的正是运行期会用的那一份。

**必须同时做的一件事**:`mcpp doctor` 增加一条 —— *home 里有多份 glibc 且编译期选中的不是 gcc 烙定的那份*。这是 D1 唯一的可观测入口;没有它,下一次触发同样只会表现为一个与 glibc 无关的包炸掉。

---

## 2. D4:沙箱 xlings 永不升级,而 mcpp 知道却不说

### 2.1 实测

```
$ mcpp self env
xlings binary = /home/speak/.mcpp/registry/bin/xlings     ← 2026.8.2.1
xlings pinned = 2026.8.6.3
```

**mcpp 把两个数字并排打印出来,不作任何评价。**

`fallback/xlings_binary.cppm:22`:

```cpp
if (std::filesystem::exists(destBin)) return destBin;   // 无任何版本检查
```

### 2.2 两种布局,只有一种会更新

| 布局 | xlings 从哪来 | 升级 mcpp 时 |
|---|---|---|
| **tarball 相对**(`MCPP_HOME=<解包目录>/registry`) | release 自带的 `registry/bin/xlings` | ✅ 随之更新 |
| **默认 home**(`MCPP_HOME=~/.mcpp`) | 首次 `self init` 时 acquire 的那份 | ❌ **永不更新** |

CI 走第一种(所以 CI 一直是新的),开发机与普通用户走第二种。**「我记得已经修过」和「实测是旧的」都对,只是说的不是同一种布局。**

### 2.3 后果:整条修复链断在这里

`#352` 需要四环:mcpp 读 `subos_info` → index 拉 `xim:graphics` → **graphics 把声明写进 subos** → subos 有 `subos_info`。第三环需要沙箱 xlings ≥ 2026.8.5.1;`graphics.lua` 显式探测 `type(subos.env) ~= "function"`,**老客户端上声明被静默丢弃**。

实测本机沙箱 xlings 2026.8.2.1 的二进制里,`subos_info` 这个字符串出现 **0 次**。

### 2.4 方案

| | A. 版本低于 pin 就自动替换 | B. doctor 报告 + 一条显式命令 | C. 每次启动检查 |
|---|---|---|---|
| 无感 | ✅ | ❌ 用户要动手 | ✅ |
| 风险 | 换掉用户正在用的 xlings | 低 | 每次 exec 多一次版本读 |
| 与「mcpp 不接管 subos 状态」的边界 | ⚠️ 擦边 | 一致 | ⚠️ |

**推荐 A + B**:acquire 时比对版本,低于 pin 则替换(这是 mcpp 自己 vendored 的那份,不是用户的系统 xlings,替换它不越界);同时 doctor 把偏差列为一条 finding。

**A 的判据必须是「低于 pin」而不是「不等于 pin」** —— 用户手动放了一份更新的进去,不该被降级。

> **⚠️ 这条要小心**:替换沙箱 xlings 会改变依赖解析的行为。参考 `index-floor-must-degrade` 的教训——**发布数据不得让程序失效**。所以替换后必须能回退,且 doctor 要能说出「现在用的是哪一份、为什么」。

---

## 3. D2/D3:测试证明的是「我和我自己一致」

### 3.1 D2 —— wire format 读错,功能完全不工作

盘上真实的 `envs` 是**以 binding 为键的对象**;我的 reader 期望**数组**。`is_array()` 为假 ⇒ 循环一次不执行 ⇒ 对着真实 subos **一个变量都不应用**。

**10 个单测 + 1 条 e2e 一路全绿**,因为每一份 fixture 都是我按同一个臆想手写的。

### 3.2 根因不是笔误,是方法

> **用与实现同源的理解去造 fixture,抓不出对 wire format 的误解。只有取自写入方的 fixture 能抓。**

已加 `RealXlingsCapture`(真实输出逐字副本),parser 改为从 xlings 的 reader **誊写**而非建模 —— 这顺带抓到第二处分歧:xlings 丢弃 `op` 不是 `set`/`prepend` 的声明,我原来全收。

### 3.3 D3 —— 自证断言,以及注释撒的谎

我原本的 e2e 只跑一次 `mcpp run`,PASS —— 而它存在的目的是覆盖**缓存快路径**,那次根本没走。加上「这次必须走快路径,否则报错」之后,立刻暴露快路径丢环境变量,**同一条断言接着又抓到第二个**(缓存行读写顺序不匹配)。

更难看的是:reader 的注释**声称**它把旧缓存当 miss —— 那个检查从来没写过。**注释描述了代码没有的行为,比漏掉更糟:它让下一个读代码的人不去查。**

### 3.4 提议:三条可执行规则

- **R-A 跨仓 wire format 的 fixture 必须取自写入方**,不得手写。形式:golden 文件 + 注明来源与采集时间。
- **R-B 一条测试如果存在的目的是覆盖某条路径,必须先断言「这次确实走了那条路径」。** 否则它可能一直 PASS 而零覆盖。
- **R-C 注释若描述一条行为,该行为必须有对应断言。** 「reader 把旧缓存当 miss」这句话本身就该是一个测试名。

---

## 4. 四个缺陷的公共上游

```
D1 载荷版本  ── 猜(取最高)      ─┐
D4 xlings    ── 快照(装机时那份) ─┼─→ 把可变的外部世界当成不变的
D2 wire fmt  ── 猜(臆想格式)     ─┤
D3 缓存契约  ── 猜(注释当实现)   ─┘
```

**D1/D4 是「外部世界会变,而我记的是装机那一刻」;D2/D3 是「外部契约有确定形状,而我记的是我以为的形状」。**

两者的解药是同一句话:**凡是别人拥有的事实,读它,并且用它自己的产物来验证你读对了。**

---

## 5. 与上游设计的关系:这轮验证了什么、推翻了什么

`2026-08-07-xlings-as-runtime-substrate-design.md` 的核心论点是「mcpp 把 xlings 当目录约定而不是可查询的运行时」。**这一轮四个缺陷全部是那个论点的实例**,而且其中两个是我一边写着那份设计一边犯的 —— 这本身是最强的证据。

它也**修正**了那份设计的一处轻描淡写:§7 把「xlings 落盘 exports」排在 P1,理由是「A 降级路径足以工作」。**D1 证明那不成立** —— 没有权威落盘,mcpp 只能靠目录扫描猜一个,而猜出来的那个与 gcc 烙定的那个是两个独立答案。**`exports` 落盘应升到 P0**,且内容要包含「gcc 这份载荷烙定的 glibc 是哪一份」。

---

## 6. 分期

**P0(main 已红过,先止血)**
1. ✅ revert mcpp-index#179(PR#180)
2. ✅ PR#377:wire format + 旧缓存(D2/D3)
3. **D1 护栏**:`mcpp doctor` 报告「多份 glibc 且编译期选中的不是 gcc 烙定的那份」

**P1(让 #352 真正到达用户)**
4. **D4**:acquire 时版本比对 + 替换,doctor 报告偏差
5. **D1 正解**:载荷探测改为跟随 gcc 烙定的版本
6. 之后才重新落 mcpp-index 的图形栈迁移

**P2**
7. xlings 侧落盘 `exports`(含 gcc↔glibc 的绑定),mcpp 改读它
8. R-A/R-B/R-C 写进贡献规范

---

## 7. 明确不做

- **不在 recipe 里回避 D1**(比如把 mesa 的 glibc 下界改成钉死)。那是把引擎缺陷摊给包作者,而且下一个用下界的包会再犯 —— 与 `aarch64-native-gcc-payload-is-musl` 那次「归因成包缺 arch 轴是错的」同型。
- **不让 mcpp 接管 subos 生命周期**。D4 的方案只替换 mcpp 自己 vendored 的那份二进制,不碰用户的系统 xlings、不管理 subos。
- **不追 glibc 版本**。D1 的正解是「跟随工具链烙定的那一份」,不是「总用最新」。

---

## 8. 开放问题

- **Q1** D1 的「gcc 烙定的是哪份 glibc」:从 gcc 产物反推(specs / PT_INTERP),还是等 xlings 落盘?前者能立刻做,后者更正确 —— 是否两者都要,反推作为降级?
- **Q2** D4 的替换时机:`self init` 时?每次 acquire 时?还是只在 doctor --fix?自动替换与「用户手动放了一份新的」如何共存(判据「低于 pin 才换」是否足够)?
- **Q3** 多 glibc 是 xlings 明确要支持的方向(`subos new --runtime glibc@2.44`)。D1 修好之后,mcpp 对「一个 home 里多份 glibc」的正式立场是什么 —— 跟随工具链,还是跟随 subos 的 `runtime`?后者更符合上游设计的 S1,但需要 D4 先修。
- **Q4** 本轮 revert 掉的图形栈迁移,重新落地时如何验证「装这个栈不会影响 home 的其余部分」?**这一条是这次事故真正缺失的那个测试。** 一个可执行形状:装栈前后各跑一次与图形无关的成员(`asio-module`、`core`),断言产物的 `PT_INTERP` 与 `readelf -V` 的 glibc 符号版本上界不变。
- **Q5**(xlings 侧,§1.2d)范围解析取最高满足者,压过了 `latest` 别名承载的「这个 home 该用哪一份」。是否该让 `latest` 在满足区间时优先?这决定了「加一个新版本进索引」是不是一个安全操作。

---

## 9. 实施回写(2026-08-08,PR #378)

方案落地过程中,有三条是设计当时没看见的,其中第一条**修正了本方案自己的一个假设**。

### 9.1 兼容回落读的是「我刚删掉的机制的产物」

§3.5 写的兼容路径是:subos 不自述时,读工具链自身烙入的值 —— gcc 的 `specs`、
clang 的 `.cfg`。这在**已有机器**上成立,因为那些文件是**旧 fixup 写出来的**,还留在
盘上。

在**全新安装**的机器上不成立:mcpp 已经不写它们了,所以它们不存在。链条是
`无 binding → 无 payloadPaths → 无 --dynamic-linker → 产物拿宿主 loader`,hermetic
检查如实报出 `/lib64/ld-linux-x86-64.so.2 (outside the sandbox)`。

**每台开发机都还留着那些文件,所以本机验证永远是绿的,CI 才红。** 这条应当写进判据:

> 删掉一个机制时,先列出谁在读它的产物。兼容回落尤其危险 —— 它读的往往正是旧机制
> 留下的东西。

补的第三个来源是**编译器自身的 `PT_INTERP`**:由 patchelf 走查写入(该机制仍在运行),
指向的正是这套工具链对齐的那个 glibc payload,与 specs 曾经持有的是同一个事实,但
**不依赖 mcpp 写过任何东西**。自己读 ELF 头而不用 `patchelf --print-interpreter`——
本函数在 prepare 期运行,那里不保证 patchelf 已解析。

验证方式是把 `specs` 文件真的挪开再跑,而不是读代码。

### 9.2 归属谓词加在了一个到不了的函数里

§3.5 的 D5 把 `remap_xlings_baked_sysroot` 的判据从「存在」改为「归属」。但调用方
`probe_sysroot` 的**第一步**是「存在且可用 ⇒ 直接 return」,只有在路径**不存在**时才
往下走到 remap —— 而该函数存在的全部意义,正是处理「存在、可用、却属于别人」。

本机实测:这个仓库的 gcc 报的 sysroot 指向 `xim-pkgindex-fromsource` 下的
`.xlings/subos/default`,每次构建都在吃另一个仓库的头文件。修法是把归属问在可用性
**之前**;「外来但可用」保留为最后兜底,因为没有 registry subos 可映射的机器上,取
「什么都不要」会直接坏掉。

### 9.3 生成的 spec 放在了缓存能删的目录里

`--no-cache` 会 `remove_all(outputDir)`,而 spec 是 prepare 期写进那个目录的 ⇒ **每次
冷构建 + gcc 必挂**。修法不是调顺序,而是把「ninja 跑之前该文件必须存在」当作**不变量
在消费点持有** —— 这样也扛得住用户手动 `rm -rf target`。

值得记的是它**怎么被发现的**:e2e 201 是专为这套机制写的测试,但它只跑暖构建,而暖
构建下文件从上一次留着。**是全量本地 e2e 扫出来的,不是定向子集。**

### 9.4 链接行去重

C-runtime 的那组 flags 同时经 `link_toolchain_flags` 与 `payload_ld` 发出,整组在链接行
上出现两次。对正确性无害,但链接行有 128KiB 硬上限(`MAX_ARG_STRLEN`),真实
workspace 已经用掉 43%。`payload_ld` 现在只服务 clang-with-cfg 的 PayloadFirst,其余组合
本就已经有了。

### 9.5 一条跨平台低级错误

`rel.native().rfind("..", 0)` 在 Windows 上**编译不过**(`native()` 是 `wstring`),所有
Windows job 直接挂;而且它在能编译的地方语义也错 —— 名叫 `..cache` 的目录并不是逃逸。
包含关系是**路径分量**的问题,`path_is_under` 按分量问。

### 9.6 「拒绝」的边界:一个 payload 是答案,两个才是问题

§3.2 把判据写成「没有权威就拒绝走 payload-first」。落地后发现这条**收得过紧**。

所有兼容来源(gcc 的 `specs`、clang 的 `.cfg`、编译器自身的 `PT_INTERP`)都是**某个更早
的机制写出来的东西**,而一台机器可以合法地一个都没有。这时按 §3.2 的字面判据就该拒绝,
产物于是拿宿主 loader —— 比「猜错版本」更糟,因为它连沙箱都出去了。

真正的轴不是「mcpp 能不能去看 payload 目录」,而是**有没有得选**:

- **恰好一个 glibc payload** ⇒ 没有选择可言。它是这套工具链产出的任何产物唯一可能绑定的
  运行时,拒绝等于拒答一个只有一个答案的问题。
- **两个或以上** ⇒ 沉默。这正是事故本身的形状,必须由 subos 来定。

这与被移除的旧规则的区别是决定性的:旧规则在**有得选**的时候按 `readdir` 顺序选,而且
一直到装进第二个 payload 之前都看起来是对的。

另外,记录下来的 loader 路径可能指向 subos **视图**(`<home>/subos/default/lib/ld-linux-…`)
而不是 payload —— 视图路径里根本没有版本段。解析前先 canonical 化(R6:产物绑 payload,
不绑可变视图)。

验证方式:把 gcc 的 `specs` 挪开**并且**把 gcc 自身的 `PT_INTERP` 改指宿主 loader,即三条
兼容来源全部失效,binding 仍解析得出,产物仍拿 payload loader。

### 9.7 兼容来源是**记录**,不是权威

§9.6 补上单例回落后 CI 依然红,日志直说了原因:

```
probe: runtime 'glibc@2.39' is not installed in this home
```

那台机器上只有一个 glibc payload:**2.44**;而兼容记录烙的是 **2.39**。

三条兼容来源(`specs`、`.cfg`、`PT_INTERP`)都是**过去某个时刻的记录** —— 装机时写的、
fixup 时写的 —— 而 payload 被替换(升级、回收)时**没有任何东西回头去改它们**。§3.2 的
「精确匹配、绝不回落」于是如实地拒绝了 2.39,结果是没有 payloadPaths、没有
`--dynamic-linker`、产物拿宿主 loader。

**判据要补一句:一条记录只有在它指的东西还在,才算数。**

现在每条记录都对「实际安装了哪些 payload」核对一遍:

- 记录指的 payload **还在** ⇒ 采信(即使装了多个,它就是产物会加载的那个)
- 记录指的 payload **不在了** ⇒ 跳过,继续往下(单例回落往往能答)
- 什么都答不上来 ⇒ 沉默

这条与 §9.6 是同一件事的两面:**§9.6 说「没得选就不算猜」,§9.7 说「化石不算权威」。**
两条都是在收窄「拒绝」的适用范围 —— 拒绝该留给**真正有歧义**的情况。

### 9.8 musl 目标不碰 glibc payload

`sysroot_mode` 里补 loader / lib 目录时漏了 musl 护栏。musl 的 sysroot 是自包含的,而
同时被探测到的 glibc payload 属于**宿主**工具链 —— 把它放进链接路径后,ld 会把 glibc 的
静态 `libc.a` 拉进一次 musl 链接:

```
undefined reference to `_DYNAMIC'      (dl-reloc-static-pie.o)
hidden symbol `_DYNAMIC' isn't defined
```

同一函数里紧接着的 header 分支**本来就有** `is_musl_target` 判断 —— 那句话对库和 loader
一样成立,只是当时没写上。由 e2e 103 抓到,同样是全量跑扫出来的。
