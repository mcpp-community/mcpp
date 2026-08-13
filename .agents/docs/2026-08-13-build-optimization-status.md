# 构建性能优化:综合报告(2026-08-13)

本文报告 L1–L4 四条杠杆的**当前状态**、每条的**依据**、以及一次**被回退的实施**。
架构与方案在 `2026-08-13-build-performance-architecture.md`;这里只讲做到了哪里。

---

## 0. 一句话结论

**目标达成。** 同一份 pinned 源码(@8219584)、同一台机器、`eef8a4c` 上复测:

| | schedule=off | schedule=on | 比值 |
|---|---|---|---|
| **gcc 16.1.0** | 80.58s | **35.13s** | **2.29×** |
| **llvm 22.1.8** | 32.40s | **18.04s** | **1.80×** |

**L1 与 L2 叠加**:gcc/off 80.58s → clang/on **18.04s = 4.47×**,远在 50s 线内。
两者正交 —— L1 换编译器(压常数),L2 改图的形状,所以相乘而不是相加。

拆分调度下 noop **重建 0 个产物**(`.o`/`.gcm`/`.pcm` 一个都没动)。

### 而且是通用的,不是把 mcpp 这一个工程调快

| 工程 | 规模 | schedule=off | schedule=on | 比值 |
|---|---|---|---|---|
| **mcpp** | 138 模块 / 57k 行 | 79.9s | **34.80s** | **2.30×** |
| **xlings** | 110 模块 / 46k 行 | 112.92s | **33.41s** | **3.38×** |

xlings 是**独立作者、独立代码库**的对照(openxlings/xlings @ b1563fe),
效果比开发它的那个工程**更大**。两个工程都 noop 无重建
(mcpp 0.21s;xlings 的 10.77s 全部是依赖解析开销,`.ninja_log` 增量 **0 条边**)。

⚠️ xlings 那一栏有一处不对称:off 那次编译了 `mcpplibs.xpkg`(5 个单元),
on 那次命中了缓存。5 个单元相对 80s 的差值可以忽略,但记在这里而不是抹掉。

⚠️ **修正一次错误归因。** 本文先前写「schedule 基础层导致段错误,已整批回退」。
重新施加后逐条复现:**基础层 rc=0**。那两次崩溃用的二进制**都包含当时未提交的图拆分
发射** —— 崩的是那部分。基础层已恢复,`auto` 为 off、`on` 才启用拆分形状。

---

## 0b. L2 的覆盖面 —— gcc 与 clang 都已落地

两个编译器各支持**其中一种**机制,不可互换,`policy` 决策一次:

| 编译器 | 形状 | 为什么只能是它 |
|---|---|---|
| gcc | `detach-codegen` | 无廉价的 BMI-only 模式(`-fmodule-only` 要花掉整编译的 99%),但 gcc 用 `rename()` 发布 BMI,所以"文件出现"是可靠信号 |
| clang | `two-phase` | 反过来:clang 用 `O_TRUNC` 就地写 BMI(读者会看到半个文件),但它有真正廉价的 BMI-only 调用 |

### clang 这条的两个坑,都不是"接边"的问题

**坑 1:`--precompile` 发出来的不是同一种 BMI。**

    -fmodule-output= … -c                        7.35s   BMI  9,102,984 B  (reduced)
    --precompile                                 1.81s   BMI 18,402,920 B  (FULL)
    --precompile -Xclang -emit-reduced-module-interface
                                                 1.67s   BMI  9,102,968 B  (reduced)
                          (clang 22.1.8,src/build/prepare.cppm)

`--precompile` 单独用又快又对**看起来**成立,实际上它发的是 *full* BMI ——
因为它的产物本来是要喂回去做 codegen 的。把 full BMI 发布给下游不是等价替换:
小模块上体积涨约 16 倍(`mcpp.platform` 19,424 → 313,452 B),而且在 mcpp 自己的
模块图上直接让 clang 22.1.8 编错一个下游 TU:

    error: call to implicitly-deleted default constructor of
           'formatter<basic_string<char>, wchar_t>'

—— 一个**窄**格式串,报错点在 `std` 里面,离真因三个文件远。同一个 TU 对着 reduced BMI
编译通过。所以 reduced 不是优化,是契约:`bmiOnlyFlags` 必须逐字节复现它。

⚠️ 这个坑的判据是**体积**,不是"编过了"。先做出来的版本能跑完 fixture、
noop 干净、增量传播正确,**在 137 个模块的真实工程上才炸**。

**坑 2:object 边只能重编源码,不能读 BMI。**

reduced BMI 不能拿去 codegen,于是 object 边是 `-c <源码>`(不带 `-fmodule-output`,
BMI 归 A 边所有,两条边不能写同一个文件)。代价是**前端跑两遍**;收益是下游只等 A 边。

两条边彼此**独立**(object 边不等 BMI 边),所以 codegen 可以整体落在图的后面。

### dyndep:一个源码两条边,两条都要记录

P1689 只知道 object(`primary-output` 取自被扫描命令的 `-o`),BMI 边没有记录时
ninja 不是警告而是**整图拒绝**:

    ninja: build stopped: 'pcm.cache/mcpp.version_req.pcm' not mentioned in
           its dyndep file 'obj/version_req.cppm.ddi.dd'

—— 报的是边,不是缺失的记录,指向的是无辜的一侧。

解法是 `mcpp dyndep --split-module`:给 BMI **和** primaryOutput 各写一条记录。
不是"把目标改成 BMI" —— 两条边都要解析同一批 import,都需要同一批隐式输入。
**不要去改扫描的 `-o`**:GCC 那边共用 `-o` 与 `-fdeps-target` 已经造成过
"扫描去写还不存在的 `gcm.cache/`" —— **在 mcpp 自己的仓库上不暴露**
(那个目录早被上一次构建建好),换个全新工程立刻失败。

### 实测(pinned 源码 @ 8219584,clang 22.1.8)

| 并发 | schedule=off | schedule=on | 比值 |
|---|---|---|---|
| `-j4` | 56.34s | **37.60s** | 1.50× |
| `-j8` | 34.00s | **25.55s** | 1.33× |
| `-j32` | 32.03s | **17.95s** | **1.78×** |

前端跑两遍要多花 CPU,但**在试过的每个并发档位上都是净赢**,所以没有加核数门槛。

**正确性判据是产物而不是退出码**:两条臂的 **130 个目标文件逐字节相同**。
(BMI 有 101 个不同 —— 两臂在不同的指纹目录下,BMI 里烙了输出目录的绝对路径;
这正是"对照放两个目录会让路径冒充差异"那条,所以 BMI 差异在这里不构成证据。)

---

## 1. 四条杠杆的状态

| | 杠杆 | 状态 | 依据 |
|---|---|---|---|
| **L1** | 按次选择工具链 `--toolchain` | **已实施** | 实测 81.8 → **32.6s**(2.51×) |
| **L2** | 下游在 BMI 可用时即开始 | **已实施**(`bmi_schedule = "on"`,gcc + clang) | gcc 79.9 → **34.8s**(2.30×);clang 32.0 → **17.95s**(1.78×) |
| **L3** | 定义移出接口单元 | **不做** —— 已量出它治的是 L2 同一个病 | 实测:对 mcpp **−6.2%**,对 cmake +92.3% |
| **L4** | 拆 `build.prepare` | **已实施**(架构收益;性能上为零) | 实测:**0**,原因见下 |

### L1:做成了「按次选择」,没有换默认

`mcpp build --toolchain llvm@22.1.8` —— 实测 **81.83s → 32.61s(2.51×)**,已达标。

**换默认**才是那个不能做的动作:它让所有已发布包的指纹失效(全生态一次性重编),
三平台 llvm 版本还不统一(Windows 20.1.7 vs Linux/macOS 22.1.8),
且牵涉 `-static-libstdc++` 与 libc++/libstdc++ 的 ABI 选择 —— 需要协调。
**按次选择不需要任何人配合,收益却是同一个 2.51×。**

⚠️ 它**不改变形状**:clang 下 makespan 32.20s / 关键路径 32.15s = 仍然 **100%**,
并行度 3.90×,与 gcc 完全一致。clang 只是每模块便宜 2.5 倍。**L2 因此仍然必要。**

### L3:明确不进这个 PR

L3 指的是**改 mcpp 自己的 138 个模块**——把定义从接口单元移到实现单元。
它不是构建引擎的能力,而是**被构建工程的写法**。

**决定:待合入的 PR 不动 mcpp 源码的实现风格。** 理由:
本轮的目标是优化 **mcpp 的构建性能**(引擎能力),而"改被测工程的结构来提速"
是另一件事——它对所有用 mcpp 的工程都适用,却要求每个工程改写自己的代码。
把两者混进同一个 PR,会让一次引擎改动挟带一次跨全库的风格变更。

**可以做的**:拉一个临时分支/PR,只为**量出具体收益**(推算是链 74.6s → ~10.4s),
测完即弃,不合入。收益数字回填到本文。

### ⚠️ 更正:L3 是单项收益最大的杠杆,不是"绕行方案"

下面那段用**未标定的旧 fixture**得出"L3 只值 +8%",**那个数字不可信** ——
那份 fixture 每个 TU 有 74% 是编译器启动、`weight` 旋钮推不动成本(见 bench/README §1a)。
用标定后的 fixture(`--preset standard`)重测的 2×2:

| | `modules`(定义在接口) | `modules-impl`(定义移到实现单元) |
|---|---|---|
| **schedule=off** | 17.76s | **5.30s** |
| **schedule=on** | 12.45s | **5.00s** |

* **L3 单独:3.35×** · **L2 单独:1.43×** · **L2+L3:3.55×**

**它们叠加,但只叠一点点**,因为**两者治的是同一份浪费、只是从两头下手**:
L3 把 codegen 从接口单元搬走,L2 是不等那份 codegen。做了任何一个,
另一个就没多少可买。而**单项收益 L3 远大于 L2**。

⚠️ 注意 L2 在这里只有 1.43×,而在 mcpp 真实源码上是 2.30× ——
fixture 单元的 codegen/parse 比例与真实模块不同,**不要跨工作负载搬运比值**。

**所以"极致性能"的答案是:引擎侧 L2 + 工程侧 L3,而 L3 是更大的那一半。**
L3 仍然不进这个 PR(它改的是被构建工程的写法),但它的定位从
"给没有 L2 的引擎准备的绕行方案"更正为**最有效的单项优化**,
文档提示应当据此改写。

### 旧的分析(基于未标定 fixture,保留作为对照)

不用拉分支:bench 的 `modules-impl` 变体测的**正是** L3(定义写在接口单元 vs 移到实现
单元),数据已经在 `bench/results/five-way-20260812/` 里。同一 fixture、同一编译器:

| 场景 | mcpp:modules → modules-impl | cmake:modules → modules-impl |
|---|---|---|
| cold (gcc) | 3.53 → 3.25s **(+7.8%)** | 13.05 → 12.80s (+2.0%) |
| cold (clang) | 2.50 → 2.19s **(+12.3%)** | 4.00 → 3.96s (+0.9%) |
| **edit-body (gcc)** | 0.29 → 0.31s **(−6.2%)** | **10.29 → 0.79s (+92.3%)** |
| edit-body (clang) | 0.46 → 0.31s (+32.5%) | 2.62 → 0.42s (+84.0%) |

**看 `edit-body` 那两行。** 把定义移出接口单元,给 cmake 带来 **92%** 的提升,
给 mcpp 带来 **−6%**(即没有)。原因是同一个:改函数体时接口没变,
cmake 按 BMI 的 mtime 级联,mcpp 比 BMI 的内容。**L3 是给没有 L2 的引擎准备的绕行方案。**
引擎做了这件事之后,工程再去重构,在这条轴上什么都买不到。

真正留下来的是 **cold 上 +8%~12%** —— 接口单元变薄,关键链就变短。这是真的,
但它是"顺手的好设计",不是一条值得为性能去改 138 个模块的理由。

**所以 L3 的文档提示是:先要引擎的 L2,再谈重构。** 顺序反了会做很多白工。

### L4:已实施,而且实测收益为零 —— 这一点比数字本身重要

抽出 `mcpp.build.prepare_inputs`(341 行,cfg() 谓词 + 指纹规范化),
`prepare.cppm` 6521 → 6186 行,两个函数 re-export 所以调用方零改动。

**构建时间没有变化**(off 79.23s / on 34.54s)。原因在拆分前就分析出来了,
实测只是确认:**抽出来的东西成了 prepare 的依赖,链只会变长不会变短** ——
`… → prepare_inputs → prepare → …` 仍然串行,prepare 少掉的成本正好由新模块付掉。

要缩短关键路径,抽出的部分必须是 prepare 的**兄弟**(被 prepare 的**导入者**直接用)。
已查清:configure 只用 `BuildContext`,execute 用 `BuildContext` + `prepare_build`,
其余只用 `prepare_build`;而链上是 prepare → execute → configure,
execute 离不开 `prepare_build`,所以抽类型也没人能离开这条链。
真正有效的是拆 `prepare_build` 本身。

而且 **L2 落地后这件事的收益又小了一截**:一个接口现在只阻塞导入者约 22% 的编译,
不是全部。所以这次拆分按**架构**理由留下(6500 行的模块本就该拆),不按性能理由。

### 这条界线

⚠️ **这是本轮最重要的一条界线。** 「优化 mcpp 的构建性能」指的是**通用构建性能**,
不是把 mcpp 这一个工程调快。**通过改被测目标来变快,不能算数** ——
它对别人的工程一点用都没有,而且会让基准失去意义。

L2 是通用的:引擎侧的 2.30× / 3.38× 在**两个互不相关的工程**上都成立,
不要求任何人改写自己的代码。L3/L4 只对被改写的那个工程生效。

**所以 L3 和 L4 都不进这个 PR**,它们降级为**文档里的提示**:
想更快的工程可以这么做,收益在临时分支上量、量完即弃,数字回填到本文。

两者仍然不是同一类动作,区别记在下面:

* **L4 是架构改动** —— `build.prepare` 6521 行、16.4s、占关键链 22%,是唯一的真离群点。
  形状比"把大文件拆小"苛刻得多:

  ⚠️ **把一部分抽成 prepare 的依赖,是让链变长而不是变短。**
  `… → 新模块 → prepare → …` 仍然串行,只是多了一跳;prepare 少掉的那点成本
  被新模块自己的成本抵掉。抽出来的东西必须是 prepare 的**兄弟** ——
  被 prepare 的**导入者**直接使用,才能与 prepare 并行编译。

  实际调查(谁 import prepare、用了什么):

      configure.cppm   只用 BuildContext(一个类型)
      execute.cppm     用 BuildContext + prepare_build
      doctor / pack / cli.cmd_build   只用 prepare_build

  所以把 `BuildContext` 抽成叶子模块能让 `configure` 不再依赖 prepare ——
  **但链上是 prepare → execute → configure**,而 `execute` 需要 `prepare_build`,
  configure 仍在 execute 之后。**净收益为零。**

  真正能缩短关键路径的是**拆 `prepare_build` 本身**,让 `execute` 只依赖它的一部分。
  那是对一个 6521 行模块的深度重构,不是一次抽取,需要单独立项 ——
  而且按上面那条界线,它属于**工程侧建议**,不属于本轮的引擎优化。
* **L3 是实现风格改动** —— 把定义从接口单元移到实现单元,要动 138 个模块。
  它对代码的组织方式提出要求,而收益只对改写了的那个工程生效。
  **不做**;要量收益就在临时分支上测,测完即弃。

⚠️ **两者都不得触碰 bench 钉住的那份基准 mcpp 源码**(`bench/projects/mcpp/` 指向的
被测树的快照)。那份源码是**测量基准**:改了它,前后两次测量就不再是同一个工作负载,
所有比值失效。

**L2 与它们的区别**:引擎侧的 2.30× 对**每一个** mcpp 工程都生效,
不要求任何人改写自己的代码;L3/L4 只对被改写的那个工程生效。

### L2 做到哪里

**已实施并验证通过**的部分:

* `src/build/schedule/policy.cppm` —— 纯函数决策表(每个编译器一种机制),
  7 条单测两侧钉死;`requested_switch` 是唯一读开关的地方。
* `src/build/schedule/detach_codegen.cppm` —— gcc 的运行期。
  **实测:阶段一在 2.30s / 16.15s = 14% 返回,目标文件正确,两阶段 rc=0。**
* 可观测性:`# mcpp:graph=normal;schedule=detach-codegen` 写进图头,
  `--verbose` 打印决策理由。
* 失效靠**指纹**而不是守卫:换调度就换构建目录,旧形状的图结构上不可达。

**已全部接上。**(下段保留当时的记录,因为两个失效点值得留证)
历史记录 —— 曾经未接上的:图的拆分发射。它需要按 §2.3/§2.4 把 BMI 边的 `depfile` 接到
P1689 扫描的产出上;第一版发射(未提交)会让 `mcpp build` **段错误(rc=139)**,
而**同一棵树上不含它的二进制 rc=0** —— 这一点是逐条复现出来的,
先前把整批基础层当成元凶是错误归因。

`auto` 现在是 **off**:调度改错是**静默**的(漏掉一条头文件依赖不会报错,
只会不再重编),不该凭一台机器的结果成为默认。

---

## 2. 已经钉死、下次不必重走的四件事

这些是本轮最有价值的产出,全部有实测支撑:

1. **GCC 原子发布 BMI,clang 不是。**
   strace:GCC 写 `<name>.gcm~` 再 `rename()`;clang 以 `O_TRUNC` **直写最终路径**。
   ⇒ 「看文件出现」对 GCC 成立、对 clang **不成立**(会读到写了一半的 `.pcm`)。

2. **两种机制互补,不是二选一。**
   clang 有便宜的 BMI-only 调用(实测 `src/build/prepare.cppm`:**1.67s vs 7.35s**);
   GCC 没有(`-fmodule-only` 要 **99%** 的时间 —— 它不跳过后端,只是不写目标文件),
   但 GCC 原子发布 BMI 而 clang 不。**装反是静默的。**

   ⚠️ 早先这一条写的是「`--precompile` 0.78s / `-c` 自 pcm 0.70s,总 CPU 只多 9.6%」。
   **那条路走不通**:`--precompile` 发的是 *full* BMI(见 §0b 坑 1)。真实数字是
   1.67s + 7.31s ≈ **多 22% CPU**,object 边重编源码而不是读 BMI。

3. **depfile 在 BMI 之后写出。**
   实测:depfile 16.39s,BMI 2.36s,整条编译 16.55s。
   ⇒ 拆分后的 BMI 边**不能**用编译器自己的 depfile(边结束时它还不存在);
   挂到对象边则头文件变更时 `bmi-await` 立刻返回、**什么都不重编**。
   两种朴素挂法都会**静默丢掉头文件跟踪**。

4. **P1689 扫描的 depfile 是可用的替代来源。**
   实测对照:它相对编译的 depfile **只缺 `.gcm`**(那是 dyndep 在管的),
   **头文件全覆盖**;且 `cxx_scan` **没有**声明 `depfile`,ninja 不会消费掉它。
   ⇒ 这条路是通的,只是还没接上。

---

## 3. 顺带修掉的:`mcpp test` 热跑 189.7s → 2.15s(88×)

不属于构建引擎,但属于同一个问题的同一种病 ——「每次都重做一件上一次已经做完的事」。

用户报「每次运行单元测试都很慢,是不是没有并行测试功能」。**先量,分解就把前提否掉了**:

    finished in 189.74s (build 187.70s + run 1.78s)

83 个测试的**运行阶段只有 1.78s**。「没有并行执行」属实,但它不是慢的原因。

给 `NinjaBackend::build` 加了分段计时(`-v` → `build/stage:`,留在代码里),一次热跑:

    loader-tags        total=158701ms  calls=85     <-- 98%
    ninja              total=   575ms  calls=85
    compile-commands   total=   443ms  calls=85
    runtime-validate   total=   160ms  calls=85
    emit-ninja         total=    88ms  calls=85

没有这个分解只能猜 —— 我先后猜过 emit_ninja / compile_commands / hermetic,全错
(三者合计 < 15ms)。

三处修复:

| # | 改动 | 收益 |
|---|---|---|
| 1 | rule E(`check_and_record_loader_tags`)只解析 stat 变了的产物;没变的从 `resolution.json` 把判定**读回来** | 189.7s → 5.3s |
| 2 | `-k 0` 批量构建成功 ⇒ 跳过每个测试的复驱动(~39ms × 83) | 5.3s → 3.9s |
| 3 | 并行跑测试(>1 个时捕获输出、整块打印;=1 个时保持前台流式) | 3.9s → **2.15s** |

⚠️ **1 的回归测试形状**:便宜的写法(跳过没变的就完事)会让记录缩水成「这次重链了
什么」,于是「记录为空」和「全部合规」长得一模一样 —— 正是 rule E 存在的理由。
**纯 noop 抓不到它**(noop 时什么都不重写,坏记录也完好);要**只碰一个目标的源码**
(一个重链、一个不重链)。e2e 214 按这个形状写,并按错误实现验过先红。

⚠️ **3 为什么不是无脑并行**:多个测试直接流式输出会逐行交错,失败的断言变得无法归属 ——
而归属正是那个循环存在的理由。所以 >1 时捕获、结束时整块打印;=1 时保持流式,因为
那是调试场景,挂住的时候尤其需要实时输出。

---

## 4. 本 PR 当前包含什么

引擎侧:

* **L1** `--toolchain SPEC` / `MCPP_TOOLCHAIN`:按次选工具链,不动 manifest、不动指纹。
* **L2** `bmi_schedule = "on"` / `MCPP_BMI_SCHEDULE`:gcc `detach-codegen` + clang `two-phase`,
  决策集中在 `src/build/schedule/policy.cppm`,运行期在 `src/build/schedule/`。
  默认 `auto` = off。
* **L4** 抽出 `src/build/prepare_inputs.cppm`(架构收益,性能为零 —— 见 §1)。
* `--jobs N|auto`。
* **BMI 等价性判断改用 `mcpp bmi-equal`**,替代永远不可能成功的 `cmp -s`
  (GCC 把时间戳写进 BMI 内容)。真实工程实测:
  `touch-hub` **84.53s → 0.44s**(对 cmake **192×**,对上一版 mcpp **174×**)。
  ⚠️ `edit-body` 无提升(18.29s vs 18.30s)且**这是对的** ——
  改函数体确实改变 BMI,级联是必需的。这一行是区分
  「避免不必要的工作」与「避免工作」的对照。
* `mcpp test` 的三处提速(§3)。

其余是 bench 套件、规范与数据(见 `bench/README.md`、`bench/results/`)。

## 5. CI 与合入

* `curl: (52) Empty reply from server` 那一类红是引导下载失败,12 秒内即挂、与代码
  无关。判据:失败 job 的日志里没有任何测试名,只有 curl 的退出码。已由
  `.github/tools/fetch_release.sh`(`--retry-all-errors` + 归档校验)根治。
* ⚠️ **更正:此前这里写的是「反复出现的红**全部**是下载失败」,那是错的。**
  bench 那条线上真正的问题是**没有红** —— 见 §7。把所有说不清的红都归给网络,
  正是让那件事多存活了几周的原因。
* **未合入**,按要求。

## 7. bench 套件审计:一整条绿色的 CI 什么都没有测

用户报「bench 卡住而且没有进度」。查下去发现卡住只是最表层的症状。

**`bench (macos/clang/fixture)` 报成功,实际是 6 ok / 48 failed / 18 unavailable**;
唯一过的 6 个格子全是 cmake 的 `headers` 变体。三个 xlings job 报成功,**一个测量
都没有**。这个状态持续了数周。

### 六个互相独立的真因

| # | 缺陷 | 为什么没被发现 |
|---|---|---|
| 1 | 每个引擎拿到的编译器不同 —— CI 用 `command -v g++` = runner 的 gcc 13.3.0,而 mcpp 用自己 registry 的 16.1.0 | cmake 配不出 modules、xmake 把 gcc 编崩,都被记成对引擎的「真实发现」 |
| 2 | 构建工具版本随 runner 漂移(镜像自带 cmake 3.31.6,没有 4.0 的 `import std` 键) | 同上 |
| 3 | 被测工程运行时从默认分支 clone —— `--hub src/xlings.cppm` 早就不存在 | 每个格子报 `skipped`,harness 退出 0 |
| 4 | `--hub`/`--body` 按 harness 的 cwd 解析,不是按工程目录 | 只有「测你正站着的树」时才对,即 mcpp 测自己 |
| 5 | harness **永远返回 0** | 「测到了东西」这件事从来没有被断言过 |
| 6 | xmake 的 `--buildir` 相对 `-P` 解析,`clean()` 删的是另一个目录 | `cold 0.60s` 状态 `ok`、带样本 —— **每一个 xmake 真实工程 cold 数字都是假的** |

### 一条贯穿的形状

**每一个都是「失败看起来像成功」,而不是「失败没被处理」。** 套件的协议不变量 1
写着「失败不得看起来像测量」,但它只覆盖了单个 cell 的 `status` 字段 —— 没覆盖
退出码、没覆盖「量到的是不是真的那件事」。

现在补上的断言,按发现顺序:

* `failed` 或「一个 ok 都没有」⇒ 非零退出;已知缺口写进 `allow_failed` 且必须带
  `KNOWN GAP` 说明(守卫检查)。
* `cold` 必须大于同引擎同 variant 的 `2 × noop` —— 否则它没重建。**不是性能阈值**,
  是内部一致性。
* `hub`/`body` 必须在钉住的树里真实存在(靠子模块才可检查)。
* 工具版本必须是精确版本;`reference_mcpp` 必须等于 `.xlings.json` 的 bootstrap pin。
* 扰动**形态**写进 note —— `edit-comment` 在有函数体的单元里插注释(行号全移、BMI
  真的变了、级联是对的)和在没有函数体的单元末尾追加(什么都没动)是两个不同的
  问题,而套件用一个名字同时回答了它们。

### 可观测性(用户最初报的那件事)

* 进度实时打到 stderr 并逐行 flush;
* 每条 configure/build 有超时(默认 1800s),超时 kill 并报 `TIMED OUT after Ns`;
* 失败时直接打出子进程日志尾部;
* 子进程日志改为**追加**、由 runner 每个 cell 清空一次 —— 此前计时构建那一行
  `build ok, spent 0.111s` 会把前面 configure 的输出整个擦掉,这正是 xmake 那条
  0.60s 一开始无法诊断的原因。

### 这批数字里最该记住的一条

`edit-comment` 在 mcpp 自己的工程上是 **199×**,在 xlings 上是 **1.00×**。
不是优化时灵时不灵 —— 是 mcpp 的 hub 恰好没有函数体。**mcpp 测自己永远看不到
这件事**,这就是独立控制目标存在的全部理由。

## 6. 下一步(按顺序)

1. **`auto` 是否翻成 on**:这是发布决策不是技术缺口 —— 指纹里带了 schedule,
   翻默认会让所有已发布包全量重建一次。等三平台 CI 见过 `on` 之后再单独立项。
2. **msvc**:`/ifcOnly` 的代价与 `.ifc` 是否原子发布都还没测。猜错是静默的
   (半个 BMI 不是诊断,是编错),所以保持 `None`。
3. **L3 作为书写约定**:优先施加于链上那 19 个模块(见 §1),不回改存量。
4. **换默认工具链**单独立项(生态决策,见 §L1)。


## 8. ⚠️ L2 有一个未修好的正确性缺陷 —— 现阶段不应推荐开启

`bmi_schedule = "on"` 在**增量重建**上会让导入者撞到不存在的 BMI:

    failed: gcm.cache/fx.unit_1.gcm
    fx.unit_0: error: failed to read compiled module: No such file or directory
    fx.unit_0: note: imports must be built before being imported

**复现**(生成的 fixture,modules variant,四个场景稳定失败):

    bench --engines 'mcpp[schedule=on]=<mcpp>' --variants modules \
          --scenarios touch-hub,touch-leaf,edit-body,edit-comment \
          --preset standard --runs 2 --compiler payload:gcc

### 已经确定的

* **不是编译器之间的竞态**:`-j1` 一样复现。
* **窗口是设计带来的、而且很大**:phase 1 在 spawn 编译器**之前**就把旧 BMI
  rename 进 `.bak`,直到编译器发布新的为止,这个模块在磁盘上**没有 BMI**。
  实测一次增量重建中该文件消失约 **208ms**(2ms 采样 × 104 次命中)。
* **失败模态是安全的那一种**:该路径下 BMI 是**缺失**而不是**陈旧**,所以永远
  是响亮的失败,不会产出一个「成功但错误」的构建。这一点是量出来的,不是希望。
* 真实工程(mcpp 自己、xlings 两种风格)上没有复现 —— 只有 fixture 的紧密
  unit_0→unit_1 链会撞上。**这就是合成 fixture 的价值**,我此前把它当成
  「不如真实工程可信的那一半」,是错的。

### 已经修掉但**不是**本缺陷成因的

`compile_release_at_bmi` 的 `read_rc` 分支返回成功却从不 `settle_bmi`(见
`8c9f239`)。它确实是个真缺陷 —— 上一份 BMI 一直停在 `.bak`,而且那个单元的
**等价性检查从未运行**,也就是说级联抑制对最便宜的那些单元是静默关闭的。修完
之后 `.bak` 残留归零,**但四个场景照样失败**。

### 还没搞清楚的

在 `-j1`、且 dyndep 明确写着 `unit_1.gcm: dyndep | unit_0.gcm` 的情况下,导入者
为什么仍然会在那 208ms 的窗口里被 ninja 调度。下一步应当是 `ninja -d explain`
配合边级时间线,而不是继续静态推理 —— 这一条我已经猜错过一次。

### 因此

* **`auto` 绝不能翻成 on**,直到这条修好;
* 所有已发布的 `bmi_schedule` 数字都是在缺陷存在时取的,README 里已标注不可引用;
* 修法方向:要么让 BMI 在整个重建期间保持可读(先编译到临时路径、成功后再原子
  替换,而不是先把旧的挪走),要么让导入者的边真正等到 BMI **重新发布**之后。
  前者更像是对的 —— 「先移走再重建」本身就在制造一个不存在的中间态。


## 8b. 第四次尝试之后:把 L2 从 CI 里撤出,并说明现状

**已修好的两件事(独立成立,与下面那条无关):**

* `compile_release_at_bmi` 的 `read_rc` 分支返回成功却不 `settle_bmi` ——
  上一份 BMI 停在 `.bak`,而且那个单元的**等价性检查从未运行**,级联抑制对最便宜
  的单元静默关闭。
* 编译失败时不再把单元留在「完全没有 BMI」的状态。

**「导入者读不到 BMI」已经修好。** 原设计在 spawn 编译器**之前**就把旧 BMI
`rename` 走,于是模块在磁盘上有约 208ms 没有 BMI(实测)。改成**复制**一份到
`.bak`、原件留在原地,并用「文件身份(size+mtime)发生变化」而不是「文件存在」
来判断发布。`failed to read compiled module` 不再出现。

⚠️ 中间踩的两个坑,都值得记住:
* `std::filesystem::file_size(p, ec)` 失败时返回 `(uintmax_t)-1`。我在检查 `ec`
  **之前**就把它写进结构体,于是「文件不存在」与默认构造的哨兵不相等 —— phase 1
  第一次轮询就认为「变了」,在编译器产出任何东西之前返回,所有 object 边报
  `no compiler was started … phase 1 did not run`。
* `copy_file` 给副本盖的是**拷贝时刻**的 mtime。而 `settle_bmi` 恢复这份副本正是
  为了让 mtime **不前进**、让 ninja 的 restat 掐断级联。不显式把原 mtime 带过去,
  恢复反而把 mtime 推前 —— `touch-hub` 变成 12.61s(冷构建 12.37s),
  **六个格子全报 `ok`**。状态列抓不到这个,只有数字能。

**仍然没修好的:** `touch-leaf` / `edit-body` 现在挂在**链接**上 ——
`undefined reference to unit_19_value@fx.unit_19()`。方向应当是:BMI 的 restat
抑制不能连带抑制**这个单元自己的 object 边** —— 它的源码确实变了,object 确实
必须重建。BMI 不变(导入者不必重建)与 object 必须重建,是两件事。

**因此 CI 里暂时不跑 `+schedule=on` 这条臂。** 它是 opt-in、默认关闭,用户拿到的
东西不受影响;但 CI 不应该去测一个构建不起来的配置。放回去是一行,门槛是 §8 的
复现全绿。

**这条 bug 我连错四次**(误诊 settle_bmi、哨兵不匹配、mtime 没带过去、以及现在的
object 边)。记在这里是因为下一个人应该从「object 边与 BMI 边的 restat 语义不同」
开始,而不是从头再猜一遍。


## 9. bench 跑起来之后暴露的两个**与 bench 无关**的既有缺陷

这两个都不是这次改动引入的 —— 是矩阵此前根本没在测,所以从来没人看见。

### 9a. macOS 上 mcpp 编不了依赖的 `build.mcpp` 助手

`bench (macos/clang/xlings-2026.8.13.1)`:

    error: dependency 'xpkg': build.mcpp failed to compile (exit 1):
    dyld[21445]: Symbol not found: __ZdaPv
    clang++: error: unable to execute command: Abort trap: 6

`__ZdaPv` 是 `operator delete[](void*)`。助手链接过了,运行期找不到 libc++。
与 [[build-mcpp-helper-self-containment]] 同一类问题(glibc 靠 rpath、musl 与 PE
才需 `-static`),但 macOS 这条此前没有覆盖。

**影响面比 bench 大**:任何在 macOS 上依赖带 `build.mcpp` 的包的工程都会踩到。

### 9a-2. 同一个机制,更大的面:macOS 上**每个**子进程都被污染

不只是 `build.mcpp` 助手。bench 的 macOS 格子里 cmake / bazel / 参照 mcpp
**三个引擎一起**挂在同一处:

    dyld: Symbol not found: __ZdaPv
      Referenced from: …/XcodeDefault.xctoolchain/usr/bin/ld     ← Apple 自己的链接器
      Expected in:     …/registry/…/lib/libc++.1.0.dylib

**Apple 的 `ld` 自己就链 libc++**,而 registry 的那份 libc++ 进了动态加载器的搜索
路径,于是 `ld` 还没开始链接就 abort。判据:**被测的 mcpp 那条臂在同一次运行里
全绿** —— 三个引擎同样地挂、一个不挂,说明问题在环境而不在任何一个引擎。

⚠️ **把 macOS 上的 payload libc++ flag 全部去掉之后,它照样发生。** 所以污染不是
经由链接 flag 进来的,而是经由 `DYLD_*`(很可能是 mcpp/xlings 为了让自己的载荷
二进制跑起来而设的),然后被所有子进程继承。

macOS 的 bench 格子因此进 `excluded`,原因写在 matrix.json 里。**没有继续猜** ——
本机是 Linux,复现不了,而这条已经让我烧掉好几轮 CI。

### 9b. mbedtls 的 registry 源码包缺 `framework/` 子模块

    mbedtls-3.6.1/CMakeLists.txt:304
      framework/CMakeLists.txt not found.
      Run `git submodule update --init` from the source tree.

挡住的是 xlings cmake arm 的最后四分之一(ftxui / libarchive / lua 三个已经接好、
能用)。注意 **mcpp 自己构建 mbedtls 不会踩到**,说明 mcpp 走的根本不是 mbedtls
的 CMake 路径。

解法有三条,都需要决策而不是我单方面选:vendor 那个不大的 `framework` 目录、
改走 mbedtls 的 Makefile、或者让 registry 把完整树打进包里。
