# 商业级可用:mcpp × xlings 的裸机与嵌入式总体方案

2026-09-04 · 多仓库总体方案 · **v5:P0 引擎切片 + B/C/D/E 四轴已实施**(PR #550、#551)

前置讨论:
[`2026-08-21-baremetal-ecosystem-assessment.md`](2026-08-21-baremetal-ecosystem-assessment.md)(七角度评估) ·
[`2026-08-20-baremetal-user-facing-scenarios.md`](2026-08-20-baremetal-user-facing-scenarios.md)(场景) ·
[`2026-07-24-embedded-platform-support-design.md`](2026-07-24-embedded-platform-support-design.md)(issue #276) ·
[`2026-08-27-openkal-ecosystem-design-plan.md`](2026-08-27-openkal-ecosystem-design-plan.md)(生态接口判据)

> ⭐ **只想看结论的读者直接去 §8**:十三条决定,每条附判据与它推翻了上一版的哪一句。
> §2 是唯一的新设计,其余各章是数据与归属。

---

## 0. 一句话,与本文的骨架

> **裸机与嵌入式的"极致体验"不是功能的总和,是「一件事只说一次,并且说在唯一知道
> 它的那一层」。**

所以本文不是一张功能清单,而是一张**归属表**。每一条待办先过同一个三问,再决定它
落在哪个仓库。

### 0.1 三侧划分的判据

| 侧 | 它回答的问题 | 判别问法 | 契约形式 |
|---|---|---|---|
| **mcpp 引擎** | 对**所有**工程都一样的决定,或**只有引擎同时知道两边的地址** | 「换一块板、换一个厂商,这条会变吗?」**会变 ⇒ 不在这侧** | 表 + 单一读点 |
| **build.mcpp 插件(包)** | 属于**某块板 / 某个库**的决定,消费者不该重述 | 「谁知道这件事?」**板知道 ⇒ 在这侧** | directive 协议 + `import mcpp` 类型化 API |
| **xlings 生态** | 一个**要被装到机器上的东西** | 「它是个产物,还是个决定?」**产物 ⇒ 这侧** | xim 描述符 + `xpm.<平台>.deps` |

⭐ **这三条不是新发明的,是把仓库里已经在用的三条规则合成一句。**

* `docs/13`:*location is a target fact, selection is a board fact*
* `docs/14`:五层 target side × 四种来源(`payload` / `prebuilt` / `graph` / 缺席)
* openkal SPEC clause 10 的基数(openarch 一个 · openkal 每接口一个 · openhal 多个)

⚠️ **判据的反面同样承重,方案里每一条都要过它**:**一个决定被两侧同时表达,不会在
加进去的那天失败,会在加下一条语义时变成构建失败。** 这个代价这个仓库付过四次
(#233 / #240 / #242 / #344)。凡是本文里出现「引擎也知道一点、包也知道一点」的条目,
都必须重写到只剩一侧。

### 0.2 「商业级可用」的六个轴

不给判据的「商业级」是口号。本文把它拆成六条,每条都能被证伪:

| 轴 | 判据(一句话) | 今天 |
|---|---|---|
| **A 覆盖** | 目标机器能不能编出正确的目标文件 | 裸机四行,**Cortex-M 缺席** |
| **B 可信** | 判据是否产生在能观察到该现象的环境里 | **全部来自 QEMU**,零真机 |
| **C 闭环** | 从 `new` 到烧录、调试、看串口是不是一条命令链 | **烧录/调试/串口三项完全缺席** |
| **D 可复现** | 同一份输入,两次、两地、两台机器得到同一产物 | ⚠️ **`mcpp.lock` 自己的头注释:"does not yet pin future builds"** —— 它是记录不是钉 |
| **E 可交付** | 企业采购会问的:SBOM、许可、离线内网、支持窗口 | **近乎空白**(`sbom`/`spdx` 全仓零命中) |
| **F 可扩展** | 第三方加一块板 / 一个厂商,引擎 diff 为零 | ⭐ **今天最强的一条,方案要保护它** |

---

## 1. 资产负债表(实测,2026-09-04)

### 1.1 已经就位,且比预期多

| | 实测 |
|---|---|
| 引擎已模块化 | `modules/` 九个包(`buildmcpp` `manifest` `toolchain-model` `platform` `dyndep` `libs` `log` `source-kind` `versioning`) |
| 目标侧模型 | `docs/14` 五层 × 四来源;`hosted-standard-library` 之后**裸机可以有完整 C++**(异常、RTTI、`import std`、`filesystem`) |
| 插件面 | 15 条 directive × 7 种 scope(`PackagePrivate` `LinkGlobal` `SourceSet` `RunGlobal` `RerunKey` `Advisory` `GraphNode`)+ 类型化 `import mcpp` + `action{}` 图节点 + `dep_bin` 宿主工具 |
| 模拟器 | ⭐ **`xim:qemu-arm@9.2.4-1` 已带 `qemu-system-arm`**,机器表实测覆盖 M0(`microbit`)/ M3(`mps2-an385`)/ M4(`mps2-an386`,`b-l475e-iot01a`)/ M7(`mps2-an500`)/ M33(`mps2-an505`,双核 `an521`)/ M55(`mps3-an547`)。**已发布、已装机、一次未用** |
| triple 解析 | 已认识 `eabi`/`eabihf` 且带 `envExplicit`;`normalize_arch` 直通,`cfg(arch=…)` 无封闭词表 |
| ISA 表的列 | `extra` 列(x86_64 为 `-mno-red-zone` 加的)**正好是 `-mfloat-abi`/`-mfpu` 的形状** |
| 运行槽 | `Slot::Runner` + `Scope::RunGlobal`:板级包逐 token 拼 argv,工程可覆盖且被报告 |
| 离线 | `--offline` → `MCPP_OFFLINE` |

### 1.2 缺口,按侧分

| 侧 | 缺什么 |
|---|---|
| **引擎** | Cortex-M 目标行族;`flash`/`monitor`/`debug` 三个槽;**runner 的独占语义**(§2.3);**freestanding 链接无 `--gc-sections`**(实测全仓零命中,§3.1.1);lock 的钉住语义;SBOM 输出 |
| **插件(包)** | ARM 板级包零;真机板级包零 |
| **xlings** | ⚠️ **187 个包里 `openocd`/`probe-rs`/`pyocd`/`gdb`/`dfu-util`/`esptool`/`jlink` 零命中** —— 整个真机工具层不存在。(ARM 的 C 库**不走这一侧**,见 §3.1.1) |
| **判据** | 真机 CI 不存在;裸机 + `import std` 在 mcpp 侧只有 unit test,e2e 在生态仓 |
| **文档** | `docs/13` 停在 `2026.8.21.3`,其「异常关闭 / `import std` 不可用」两条已被 `2026.8.28.2` 推翻 |

---

## 2. C 轴是重心 —— 但比 v1 想的小得多

这是全文唯一需要**新设计**的地方。v1 把它写成"三个新槽 + 三种进程语义",自审后
两条判断把它缩小了一半。

### 2.1 ⭐⭐ 真机上的 `mcpp run` / `mcpp test` 今天就不需要引擎改动

`probe-rs run --chip <X> <elf>` 一个进程做完:烧录、复位、接 RTT/半主机、**把固件
经半主机 `SYS_EXIT` 报的退出码作为自己的退出码**。这与 `qemu-system-* -semihosting`
的形状**逐字段相同** —— 都是「一次性、读退出码」。

⇒ 真机板级包只需把 `runner` 槽填成 `probe-rs run --chip STM32L475VGTx {}`,
**`mcpp run` 就在真机上工作**,引擎零 diff;`mcpp test` 也一样,**前提是一次只跑一个**
—— 那个前提今天要工程自己记住,§2.3 把它交给板级包。

⚠️ **这条推翻了 v1 的分批**:v1 把"引擎槽表"放在真机之前。实际上通往真机
`mcpp test` 的关键路径是 **xlings 的 `probe-rs` 包 + 一个真机板级包**,槽表是
**附加**的(`flash` 只烧不跑、`monitor`、`debug`),不在关键路径上。

### 2.2 三个新槽,两种语义 —— 不是三种

v1 给 `debug` 发明了第三种语义 `Paired`(server + 客户端,交互式,tty 透传)。
自审:**那是把 IDE 的活揽进 mcpp**。`probe-rs gdb` / `openocd` 起的是一个 **server**;
连上去的 gdb / DAP 客户端属于 VS Code、CLion、用户的终端 —— mcpp 已经有一条给 IDE
的机器接口(`docs/11`),边界就画在那里。

| 槽 | 语义 | 引擎做的事 |
|---|---|---|
| `run` | `OneShot` | (已有)读退出码 |
| `flash` | `OneShot` | 读退出码;**读回校验是工具的事,退出码承载它** |
| `monitor` | `LongLived` | 起进程、透传 stdio、把 Ctrl-C 交给它、**不把"永不退出"当失败** |
| `debug` | `LongLived` | 同上,并在机器输出通道上发一行 `{ "gdb_port": …, "elf": … }` 供 IDE 连接 |

⇒ **`Semantics ∈ {OneShot, LongLived}`,两个值**。`debug` 在两值语义下与 `monitor`
同形,**移回 P1**;v1 把它推到 P2' 的理由(可能要会话协议)随边界一起消失。

### 2.3 ⚠️ 真机带来一列模拟器从来不需要的:独占

实测 `execute.cppm:2022`:`mcpp test` 用一个 **`workers` 大小的线程池**并发跑测试
二进制。QEMU 可以开 N 个实例;**一块物理板是一把互斥锁** —— 两个 `probe-rs` 抢一个
探针,结果不是失败而是**乱**。

⇒ 板级包发一条 `mcpp:runner-exclusive=1`(`Scope::RunGlobal`,布尔),引擎在运行
阶段把 workers 钳到 1。**板知道自己是独占的,工程不该记得 `-j1`**。这是 x86_64
长出 `extra` 列的同一形状:第一块真板挖出的不是缺陷,是表少一列。

### 2.4 线协议:三行,不是一种新语法

v1 写了 `mcpp:device-action=<slot>:<token>`。自审:那是在 directive 表旁边**发明第二
套语法**。`runner` 当初就是表里**一行**,新槽照做:

```
表:      {"flash",   …, Slot::Flash,   Scope::RunGlobal, …}   语义 OneShot
         {"monitor", …, Slot::Monitor, Scope::RunGlobal, …}   语义 LongLived
         {"debug",   …, Slot::Debug,   Scope::RunGlobal, …}   语义 LongLived
         {"runner-exclusive", …, Scope::RunGlobal, …}         布尔
类型化:  mcpp::flash(tok) · mcpp::monitor(tok) · mcpp::debug(tok) · mcpp::runner_exclusive()
覆盖:    [target.<triple>].flash = [...]   与 runner 同轴、同一句 note
命令:    mcpp flash · mcpp monitor · mcpp debug   同一个读点
```

⚠️ 每加一条 directive 要改的位置是九处(`build-mcpp-extensibility-architecture`
记的),这是已知代价,不是新代价。

### 2.5 三侧分工

| 侧 | 负责 | 例 |
|---|---|---|
| **引擎** | 四行表、语义列、单一读点、覆盖与 note、命令族、产物集(`.elf` `.bin` **`.hex`** `.map`) | |
| **板级包** | 每个槽的 argv 与条件降级 | `probe-rs run --chip STM32L475VGTx {}` · `probe-rs download --chip … --verify {}` · `probe-rs gdb --chip …` |
| **xlings** | 把工具装到机器上 | `xim:probe-rs`(P0')· `xim:openocd`(P2)· `xim:arm-none-eabi-gdb` |

⚠️ **今天第三列一个都没有** ⇒ xlings 侧**先行**,不是并行。

---

## 3. 逐轴方案

### 3.1 A 覆盖:Cortex-M 目标行族

| 侧 | 事项 | 备注 |
|---|---|---|
| 引擎 | `kKnownTargets` + `freestanding::kTable` 加 **7** 行:`thumbv6m-eabi` `thumbv7m-eabi` `thumbv7em-eabi` `thumbv7em-eabihf` `thumbv8m.base-eabi` `thumbv8m.main-eabi` `thumbv8m.main-eabihf`(均 `-none-`) | `lldEmulation` **留空**(clang 有 arm 的 BareMetal 工具链);`mcmodel` **留空**;`-mfloat-abi`/`-mfpu` 走已存在的 `extra` 列;`mabi = "aapcs"` 同 aarch64 行 |
| 引擎 | 表形状:**7 行,不引入 `-mcpu` 第二轴** | 判据在表自己的注释里:「让 `--target <triple>` 单独足够产出正确的目标文件」。`thumbv6m` 与 `thumbv7em` 的目标文件互不兼容 ⇒ 是行不是偏好 |
| 引擎 | ⚠️ **`-mfpu` 的默认值要实测不要类推**:`thumbv7em-none-eabihf` 不给 `-mfpu` 时 clang 选什么,用 `-###` 问机器 | aarch64 行 `lp64`→`aapcs` 的教训:表按类比填,会错在类比最强的那格 |
| 引擎 | `libdir` 与 `sysroot` 列:**7 行全部留空** | 图来源没有多库目录;列只在 sysroot 存在时被读(§3.1.1) |
| 引擎 | ⭐ **freestanding 编译加 `-ffunction-sections -fdata-sections`,链接加 `--gc-sections`** | 实测全仓零命中。依赖的目标文件**无条件进链接**(`docs/13:423`),没有它图来源的 C 库会把整份 picolibc 放进每个 MCU 镜像 |
| 引擎 | 分级:**2 行 `verified`**(`v6m-eabi`、`v7em-eabihf`,各有板)**5 行 `preview`** | 与 aarch64/x86_64 落地时相同 |
| **mcpp-index** | ⭐ **`mcpplibs/picolibc`:源码包**,`provides = ["mcpp:c-abi=picolibc"]`;**不建 `xim:picolibc-arm`** | §3.1.1。上游 zip(实测 API:`picolibc-arm-none-eabi-1.8.12-15.3.rel1.zip`,Arm GNU 15.3/GCC 编)无论如何不可用 —— `docs/14` 规则拒绝 llvm 下的 libgcc;而"自建 7 个多库"是 prebuilt 来源自带的成本 |
| **mcpp-index** | `mcpplibs/compiler-rt-builtins`:源码包,`provides = ["mcpp:compiler-runtime=compiler-rt"]`;`picolibc` 依赖它 | 只 vendor `compiler-rt/lib/builtins`,目标无关。M0 无硬件除法(`__aeabi_idiv`)、软浮点全套、ryu 的 128 位移位全在这里 |

**工作量**:引擎侧 7 行数据 + 两个 flag。C 库是源码包,主要工作是 vendor + 生成配置
(`openkal-musl` 的 `musl-generated/`、`openkal-llvm-runtime` 的 `llvm-generated/` 是现成先例)。

#### 3.1.1 ⭐ 为什么 C 库走 mcpp-index 而不是 xim 载荷(review 第一轮的追问,已决定)

`docs/14` 原文:*"Moving a layer from a prebuilt payload into the dependency graph
removes engine work rather than adding it. This is the mechanism by which one source
reaches several platforms without an engine change."*

v2 初稿为 prebuilt 来源列出的工作,逐条对照:

| prebuilt 来源要做的 | 图来源 |
|---|---|
| 7 个多库各建一次 | **没有多库**:C 库用与程序完全相同的 `compile_flags` 编译,`-mfpu`/浮点 ABI 一致是**按构造**成立的 |
| `libdir` 列与包目录逐字节对上(#481 的形状) | **列为空** |
| builtins 与 C 库同包,否则第一次 printf 挂 | 两个包,`picolibc` 依赖 `compiler-rt-builtins`,由五层规则「配置于其下的层」表达 |
| 五宿主镜像、`.sha256` 侧文件、CDN 传播等待 | 一个源码 tarball,宿主无关 |
| 版本钉在目标表里,**在 lock 之外** | 进 `mcpp.lock`(D 轴) |
| 每个架构一个包(`picolibc-riscv/aarch64/x86` 已是三个) | **一个包服务全部 11 行裸机目标** |

代价与前提:

* ⚠️ **首次构建付编译一次 picolibc 的墙钟**。全局依赖缓存(`docs/05 §2.10`,
  `$MCPP_HOME/build-cache/v1/`,跨工程、按工具链 × profile × 版本键)使它是**每台机器
  每个目标档一次**。第一天的探针里量它。
* ⚠️⚠️ **前提是 `--gc-sections`**。依赖的目标文件无条件进链接(`docs/13:423`),而全仓
  没有任何 `gc-sections`/`function-sections`(实测零命中)。hosted 目标不在乎,QEMU 的
  128 MB RAM 也不在乎;**64 KB flash 的 MCU 放不下整份 picolibc**。这是图来源的 C 库在
  MCU 上暴露的第一条引擎缺口,也是任何商业 MCU 工具链的标配。
  ⚠️ 随之而来的:向量表没有任何引用,`--gc-sections` 会把它删掉 —— 板级链接脚本必须
  `KEEP(*(.vectors))`;判据是镜像能启动,不是链接退 0。
* 板级包不再 `link_lib("c")`,改为**依赖** `picolibc`;链接脚本走 picolibc 的 `build.mcpp`
  发 `link-search`(LinkGlobal)+ 板级 `board.ld` 里 `INCLUDE picolibc.ld` —— picolibc
  上游设计的用法,零新 API。
* `sysroot_dir()` 对图来源返回空;板级包按 `aarch64-virt-rt` 已有的"缺席"分支处理。
* ⚠️ 这与 `docs/13` 记的「板级包不该绑定 libc」不冲突:那条针对的是 `[xlings] deps`
  里**按架构分的 prebuilt** 绑定;源码依赖是架构无关的。

⭐ **判据**(同时是三个既有 prebuilt 载荷要不要迁移的判据):同一份半主机 hello,
riscv64 上用 `mcpplibs/picolibc` 与用 `xim:picolibc-riscv` 各链一次,**`Size text` 相当且
输出相同**。差得多 ⇒ gc-sections 没生效或配置生成错了;相当 ⇒ 三个 prebuilt 包之后
可以各自退休。这也是 mcpp 自己的 e2e 里**第一个"图来源 c-abi 上裸机目标"**的用例
(今天零个,实测 `tests/e2e` 无 `c-abi=`)。

### 3.2 板的选择(v1 的问题 2,已决定)

判据:两块板要在**每一条会影响判据的轴上都不同**,并且其中一块要**同时存在于 QEMU
与真机** —— 那样同一个板级包有两个观察环境,「模拟器 vs 真机」这条差异本身成为可测的。

| | 板 A | 板 B |
|---|---|---|
| 板 | **B-L475E-IOT01A**(ST) | **Raspberry Pi Pico**(RP2040) |
| 核 / 行 | Cortex-M4F · `thumbv7em-none-eabihf` | Cortex-M0+ · `thumbv6m-none-eabi` |
| 浮点 / 除法 | 硬浮点、硬除法 | **软浮点、无除法指令** —— builtins 判据在这块板上 |
| 探针 | 板载 ST-LINK v2-1 | CMSIS-DAP(debugprobe / 第二块 Pico) |
| 启动 | flash 直接执行 | **ROM bootloader + 镜像内 256 字节 boot2**,另有 UF2 拖放 |
| 核数 | 单核 | 双核 |
| QEMU | ⭐ **有**(`b-l475e-iot01a`,QEMU 9.0+ 的 stm32l4x5 模型) | 无 |

⇒ P0(仅 QEMU)用 **`b-l475e-iot01a`(M4F)+ `microbit`(M0,nRF51,QEMU 有)**
两台机器,覆盖两个浮点档;P1'(真机)用 **B-L475E-IOT01A + Pico**。`b-l475e-iot01a-rt`
一个包服务 P0 与 P1' 两行 —— 这是本方案里判据最强的一处。

⚠️ **第一天的探针**:stm32l4x5 的 QEMU 模型较新,先确认
`qemu-system-arm -machine b-l475e-iot01a -semihosting -kernel hello.elf` 能打印。
不能 ⇒ P0 回落到 `mps2-an385`(M3),板 A 的 QEMU 行留到模型修好。

⚠️ **不选 mps2-an385 做主板的理由**:它是 FPGA 原型板,没人拥有;一个只在 QEMU 里
存在的板级包永远得不到真机那一行。

### 3.3 B 可信:真机判据

⭐ **第一条判据不是「测试通过」,是「产物真的进了 flash」**:`probe-rs download --verify`
读回比对,退出码承载。「烧录命令退 0」与「芯片里是这份镜像」是两件事(`.mcpp_ok`
只证进程退 0 的同形)。

⚠️ **HIL 会长成 flaky,而 flaky 判据比没有判据更糟**(它训练所有人重跑)。
⇒ 真机 job 从第一天起分开报告「探针没找到」与「测试失败」,前者不染红 PR。
`probe-rs list` 在跑测试前作分母。

### 3.4 D 可复现 —— 比 v1 写的更严重

`mcpp.lock` 头注释原文:*"It does not yet pin future builds: index dependencies are
re-resolved from their constraints each time."* ⇒ **今天没有任何东西钉住一次解析**;
`--locked` 不是"加一个 flag",是**先让 lock 成为钉,再有 flag 可以校验它**。

| 侧 | 事项 | 判据 |
|---|---|---|
| 引擎 | lock 从记录升级为钉:再次解析**先读 lock**,不符才走约束;`--locked` 时不符即失败 | 改 lock 后 `mcpp build` 用的版本随之变 |
| 引擎 | lock 覆盖**工具链与载荷**(今天 pin 在目标表里,不在 lock 里) | 换机器拿到同一个 llvm |
| 引擎 | `-ffile-prefix-map`,产物不嵌绝对路径与时间戳 | **两台机器两个目录,sha256 相同** |
| xlings | 索引可钉到 commit 而非 `latest` | 「消费者先发布,`latest` 才能动」的解药 |

⚠️ 可复现的判据**必须跨机器**。同机两次相同只证明缓存工作。

### 3.5 E 可交付(v1 的问题 3,已决定:按依赖拆,不按阶段排)

| 依赖 | 事项 | 放哪 |
|---|---|---|
| **零工程** | 支持窗口与破坏性变更通知期(书面) | **P0'**,与 P0 并行 |
| **零工程** | 传递闭包的许可清单(文档 + `mcpp explain` 已有的图) | P0' |
| **一条输出格式** | `mcpp sbom`(SPDX / CycloneDX):图与版本 mcpp 全知道 | P0' |
| **工程** | `--deny-license` 门 | P2 |
| **工程** | 整仓离线快照一条命令(索引 + 载荷 + 包 → 可拷进内网的目录) | P2 |
| **文档** | 回填 `docs/13` 两条失效限制 | **P0**,本周 |

v1 按工程依赖把 E 整个排到 P2。自审:**其中一半没有工程依赖**,排后面只是惯性。

### 3.6 F 可扩展:保护今天最强的一条

⭐ **加一块板 = 加一个包,引擎 diff 为零。** 本方案的判据:P0 与 P1' 的三个板级包
(`b-l475e-iot01a-rt` / `microbit-rt` / `pico-rt`)**每一个**都必须在 mcpp 引擎零 diff
的前提下落地(§2 的四行表除外 —— 那是一次性的,且三个包共用)。

写成 CI 可检查的:板级包仓库的 PR **不得**要求新版 mcpp(除非显式声明);
`if constexpr (requires { mcpp::x(…) })` 对不存在的限定名是**硬错误**,所以降级靠
引擎侧那条「编译失败且错误含『不是 `mcpp` 的成员』则追加升级提示」的补偿,新增的
`flash`/`monitor`/`debug` 也靠它。

---

## 4. 多仓库分工总表

| 仓库 | 角色 | 主要条目 |
|---|---|---|
| **mcpp** | 表、槽、单一读点、命令族、lock 语义、SBOM | 7 行 thumb;freestanding `--gc-sections`;`flash`/`monitor`/`debug`/`runner-exclusive` 四条 directive + 语义列;`.hex`;lock 成钉 + `--locked`;`mcpp sbom`;`docs/13` 回填;裸机 `import std` 的 mcpp 侧 e2e |
| **openxlings/xim-pkgindex** | 工具 | **`probe-rs`**;`arm-none-eabi-gdb`;`openocd`(P2);离线快照 |
| **mcpplibs/mcpp-index** | 登记与模板 + **目标侧源码包** | 三个板级包描述符;**`picolibc`、`compiler-rt-builtins` 两个新源码包**(§3.1.1);模板随包走 |
| **新建 ×3:板级包** | 槽 + 链接脚本 + 启动 | `b-l475e-iot01a-rt`(QEMU + 真机)· `microbit-rt`(QEMU)· `pico-rt`(真机,含 boot2) |
| **mcpplibs/openarch** | 机器机制层 | 第四后端 **aarch32(A/R)先于 Cortex-M**(§5 P3) |
| **openkal ***、`std-freestanding` | 语言与环境层 | 无新结构;随目标增加回归 |
| **文档** | `docs/13` 回填、`docs/18-devices`(新)、支持窗口声明 | |

---

## 5. 分批清单

每批带**判据**,判据不成立则该批不算完成。

### 第一天的探针(任何批开始前)

1. `qemu-system-arm -machine b-l475e-iot01a -semihosting` 能否打印(决定 §3.2 的回落)
2. `clang --target=thumbv7em-none-eabihf -###` 的默认 `-mfpu`(填表)
3. `probe-rs` 发布二进制的 `LD_TRACE_LOADED_OBJECTS=1` 闭包(决定它是不是"自带库载荷")
4. picolibc 从源码经 mcpp 编译一遍的墙钟,以及**第二个工程**是否命中全局缓存(决定首次
   构建体验;判据是第二个工程的 `Cached` 行与墙钟,不是第一个的)

### P0 —— 能编 Cortex-M

1. 引擎:7 行(2 verified + 5 preview);**freestanding `-ffunction-sections -fdata-sections` +
   `--gc-sections`**;`docs/13` 回填;裸机 `import std` 的 mcpp 侧 e2e
2. mcpp-index:`mcpplibs/picolibc` + `mcpplibs/compiler-rt-builtins` 源码包(§3.1.1)
3. 包:`b-l475e-iot01a-rt`、`microbit-rt`
4. **判据**:两个模板 `mcpp new && mcpp run` 在 CI 的 qemu 里打印;断言 PASS 行;
   **microbit 上一次浮点 printf + 一次整数除法**(M4F 那行对 builtins 缺口是假绿);
   **riscv64 上图来源与 prebuilt 的 `Size text` 相当、输出相同**(§3.1.1 的判据)

### P0' —— 与 P0 并行

5. xlings:`xim:probe-rs`(五宿主、双镜像、`.sha256` 侧文件)
6. E 轴零工程项:支持窗口声明、许可清单、`mcpp sbom`
7. **判据**:干净机器 `xlings install probe-rs -y` 后 `probe-rs list` 有输出且闭包不越界

### P1 —— 真机(关键路径:§2.1)

8. 包:`pico-rt`;`b-l475e-iot01a-rt` 加真机 `runner`(`probe-rs run`)
9. 引擎:`runner-exclusive`(§2.3)—— **唯一在真机关键路径上的引擎改动**
10. 自托管 runner + 两块板;探针缺席与测试失败分开报告
11. **判据**:`mcpp test` 在两块板上读到退出码;`probe-rs download --verify` 读回通过

### P1' —— 闭环槽

12. 引擎:`flash` / `monitor` / `debug` 三行 + 语义列;`.hex`
13. 包:三个板级包填三个槽
14. **判据**:`mcpp debug` 发出的 `{gdb_port, elf}` 能被一个 gdb 连上并停在 `main`

### P2 —— 可交付与第二实现

15. lock 成钉 + `--locked`;跨机器 sha256 判据
16. `--deny-license`;离线快照
17. xlings:`xim:openocd` —— **同一板级包契约的第二个实现**,这本身是槽抽象的判据

### P3 —— openarch 第四后端

18. **aarch32(Cortex-A/R 32 位)优先**:14 个 ABI 函数一个不缺(CP15 的
    `TPIDRPRW`/`TPIDRURW` 恰是两个指针槽;真 MMU;`VBAR`;DMB/DSB/ISB),且它是第一台
    32 位机器,会挖出 `arch_pte_make_leaf` 返回 `arch_u64` 这条没被问过的宽度假设
19. **Cortex-M 后端在其后**,因为要先回答**规范问题**:openarch 的可行性闸是「上下文
    切换 + 页表项」两件,M-profile 没有 MMU、没有页表项,只满足一件。14 个函数里
    5 个可写、4 个不可能(`pte_*`)、5 个语义改变(`trap_set_handler`;`percpu`/`tls`
    四个 —— 无 TPIDR 类寄存器,退化成全局变量后「两个槽是不同的」变成恒真)。
    ⇒ 先决定**接口允不允许部分后端**

### P3' —— 嵌入式 Linux(issue #276)

20. `[target.*].sysroot` 接受外部路径(今天走 `parse_xpkg_ref`);pkg-config
21. 硬边界不变:target 侧必须支持 C++23 modules(GCC ≥ 15);方案是 config②
    (mcpp 自带匹配 libc 的交叉编译器 + 只经 sysroot 取库)

---

## 6. 会踩的坑(取自本仓库已付过的代价)

| # | 坑 | 本方案哪一条会踩 |
|---|---|---|
| 1 | **判据必须放在能观察到该现象的环境里,矩阵的每一行是一个独立观察环境** | 真机 job、qemu job:按**行**穷举 |
| 2 | **`# requires:` 的 e2e 在 shard 上可能一次都没跑过**,`run_all.sh` 跳过退 0 | 守卫住在 job 里,断言 PASS 行 |
| 3 | **判据的「否」与「没测成」同读数** | 烧录校验、闭包检查:带**分母**(`probe-rs list`) |
| 4 | **声明 ≠ 安装**,`[xlings] deps` 不是安装触发器 | probe-rs 的安装边写在索引描述符 `xpm.<平台>.deps`;判据「拿走再装回来」 |
| 5 | **带 `build.mcpp` 的包必须 Form A** | 三个板级包 |
| 6 | **索引陈旧有五层**,第五层是 xlings 消费 `artifact:<sha>`;判据是 `Publish Index Artifact` 在那个 commit 上绿 | 发布后等 ~10 分钟再跑依赖它的 CI |
| 7 | **发布形态 ≠ 开发形态**:生态 CI 全用工作树替换 ⇒ 冲突不存在 | 沙箱解析**已发布**包是唯一验发布物的路径 |
| 8 | **消费者先发布,`latest` 才能动** | `picolibc-arm` / 板级包的发布顺序 |
| 9 | **裸名工具经 PATH 解析到属于另一个 home 的 shim** | 所有槽的 argv 绝对路径 |
| 10 | **自带库载荷 + 声明 loader 依赖 ⇒ elfpatch 整条替换 RPATH,装完即坏** | `openocd`(libusb/hidapi/ftdi)是这一类;`probe-rs` 第一天的探针就是为了确认它不是 |
| 11 | ⚠️ **Windows 上 `GenerateConsoleCtrlEvent` 打到整个控制台**,job object 才是机制 | `LongLived` 槽的 Ctrl-C 归属 —— `monitor`/`debug` 在 Windows 上的第一个缺陷会是这个 |
| 12 | **`pipefail` + `grep -q` 把匹配成功读成 141** | 新脚本用 `grep -c` |
| 13 | **依赖缓存曾经"同进程命中、跨进程付全价"**,`Cached` 一行不是判据 | picolibc 的首次构建体验:判据是**第二个工程**的墙钟 |
| 14 | **`--gc-sections` 会删掉没被引用的向量表** | 板级链接脚本 `KEEP(*(.vectors))`;判据是镜像能启动 |

---

## 7. 明确不做的

* **L4 build backend**(mcpp 作 Yocto/Buildroot 后端)—— 2026-07-24 维护者决策,不改
* **消费厂商预编译 C++ 库** —— 「C++ 只源码,二进制只 C-ABI + 封装」
* **mcpp 驱动 gdb / DAP 客户端** —— 那是 IDE 的边界(§2.2)
* **ESP32 / Xtensa** —— 非 LLVM 上游后端
* **Cortex-M 的 openarch 后端在规范决定之前落地**(§5 P3)

---

## 8. 十三条决定(自审十条 + review 第一轮三条)

| # | 决定 | 判据 | 推翻了 v1 的哪一句 |
|---|---|---|---|
| D1 | **真机 `mcpp run`/`mcpp test` 走现有 `runner` 槽,引擎零 diff** | `probe-rs run` = 烧录 + 复位 + 半主机退出码,一个进程,与 qemu `-semihosting` 同形 | "P1 引擎槽表在真机之前" |
| D2 | **语义列两个值 `{OneShot, LongLived}`,不是三个** | `debug` 只起 server,客户端归 IDE(`docs/11` 已有那条边界) | "`debug` 是 `Paired`,可能要会话协议" |
| D3 | **`debug` 回到 P1'**,与 `monitor` 同批 | D2 之后它与 `monitor` 同形 | "`debug` 推到 P2'" |
| D4 | **线协议 = directive 表加四行**,不发明 `device-action=<slot>:<tok>` | `runner` 当初就是一行;第二套语法是第二个解析器 | v1 §2.1 的语法 |
| D5 | **新增 `runner-exclusive`** | 实测 `execute.cppm:2022` 线程池并发跑测试;板是互斥锁 | v1 没有它 —— 第一块真板挖出的那一列 |
| D6 | **`probe-rs` 先于 `openocd`**,后者 P2 作第二实现 | 单二进制、五宿主预编译、内置芯片库、`run` 一步到位;openocd 是"自带库载荷"形状 | "openocd / probe-rs 并列" |
| D7 | **C 库进 mcpp-index 作源码包(`picolibc` + `compiler-rt-builtins`),不建 `xim:picolibc-arm`** | `docs/14`:层从 prebuilt 移进图是**减**引擎工作;多库、`libdir`、五宿主镜像全是 prebuilt 来源自带的成本;上游 zip(GCC 15.3 编)无论如何不可用 | v2 "自建 7 个多库"(review 追问后改判) |
| D8 | **板 = B-L475E-IOT01A + Pico;QEMU 行 = `b-l475e-iot01a` + `microbit`** | 每条轴都不同(核/浮点/探针/启动/核数),且板 A 同时在 QEMU 与真机 | "mps2-an385 开路" |
| D9 | **E 轴按依赖拆**:零工程项进 P0',工程项留 P2 | 支持窗口/许可清单/`sbom` 没有工程前置 | "E 整体在 P2" |
| D10 | **D 轴升级严重度**:lock 先成钉,再有 `--locked` | lock 头注释原文 "does not yet pin future builds" | "缺的是 `--locked` flag" |
| D11 | **freestanding 加 `-ffunction-sections -fdata-sections` + `--gc-sections`**,作 D7 的前提 | 依赖目标文件无条件进链接(`docs/13:423`)+ 全仓无 gc-sections(实测)⇒ 图来源 C 库会撑爆 MCU flash | v1/v2 都没有 —— 图来源在 MCU 上暴露的第一条引擎缺口 |
| D12 | 自托管 runner **归板级包仓**,mcpp 仓只留模拟器行 | 判据跟着包走;`openkal-llvm-runtime` 自跑 riscv64 qemu 的先例 | (v2 §9 问题 1,维护者已定) |
| D13 | **不做 `.uf2`**;`pico-rt` 的 `flash` 走 `probe-rs download` | RP2040 一家的格式 | (v2 §9 问题 2,维护者已定) |

## 9. 待决项

无。v2 §9 的两个问题已定为 D12、D13;D7 经追问改判,前提为 D11。

---

## 10. 实施状态(2026-09-04,PR #550)

### 已落地:P0 的引擎切片

| 条目 | 状态 |
|---|---|
| Cortex-M 目标行 | ✅ **七行**,`kKnownTargets` 与 `freestanding::kTable` 各七行 |
| D11 死代码段消除 | ✅ `-ffunction-sections -fdata-sections` + `--gc-sections`(驱动路径与直接 lld 路径) |
| `docs/13` 回填两条失效限制 | ✅ 中英双份 |
| 判据 | ✅ e2e 332(四行在 QEMU 中启动)+ 三条单测;CI `bare-metal e2e` 作业已绿 |

### ⚠️ 实施推翻的两处方案原文

**① `extra` 列不承载浮点 ABI —— triple 已经承载了它。**
方案 §3.1 写「`-mfloat-abi`/`-mfpu` 走已存在的 `extra` 列」。实测 llvm 22.1.8:
clang 从 `eabi`/`eabihf` 后缀**自行导出** `-mfloat-abi`,无需引擎发任何标志。

⭐ **但同一次实测挖出方案没有预见的一条**:浮点 ABI 只约束浮点值如何跨越函数边界,
**不约束函数内部发什么指令**。`thumbv7em` 架构蕴含 FPv4-SP,于是软浮点 ABI 下
clang 对一次 float 乘法仍发出 `vmul.f32` —— 在没有 FPU 的 Cortex-M4 上于运行期
触发异常,而编译与链接都是干净的。`extra` 列因此确实被用上了,承载的是 `-mfpu=none`,
不是方案写的那两个标志。

**② 七行不是六行。** 方案 §3.1 列了六行(三个 arch × eabi/eabihf 的子集);实际是
七行 —— `thumbv8m.base` 没有 `eabihf` 变体(M23 无 FPU),而 `thumbv7em` 与
`thumbv8m.main` 各有两个。

### ⚠️ 实施中发现的、方案没有的一条纪律

`-mfpu=none` 第一版只加在**实测到非零 FPU 指令数**的行上。那让表记录的是一次
**测量**而不是一条**保证**,并且各行的差异没有任何读者能看出来。对所有行量化的
单元测试报出了它。⇒ **一行陈述它保证的性质,而不是从一个可以改变的默认值继承它。**

### 未实施(按方案原顺序)

`P0` 的 `mcpplibs/picolibc` + `compiler-rt-builtins` 源码包(§3.1.1)· 板级包三个 ·
`P0'` 的 `xim:probe-rs` 与 E 轴零工程项 · `P1`/`P1'` 真机 · `P1'` 槽表 ·
`P2` lock 成钉与 SBOM · `P3` openarch 第四后端 · `P3'` 嵌入式 Linux。

⚠️ **软浮点行在没有 builtins 时链接不了浮点代码**(实测:`undefined symbol:
__aeabi_fmul`),这正是 §3.1.1 把 `compiler-rt-builtins` 与 C 库并列为 P0 的理由。
整数程序不受影响 —— e2e 332 的四行启动用例即为整数程序。

---

## 11. 第二轮实施(2026-09-04,PR #551)

### 11.1 六轴读数的变化

| 轴 | v4 | 现在 |
|---|---|---|
| **A 覆盖** | 🟡 引擎能编七行 | 🟡 不变(C 库源码包与板级包仍未做) |
| **B 可信** | ❌ 零真机 | 🟢 **模拟器与真机成为同一个包的两个 feature**;真机路径可声明、可解析、判据齐备,尚无实机运行记录 |
| **C 闭环** | ❌ 三个槽都没有 | ✅ `flash`/`monitor`/`debug` + `runner-exclusive`,四槽一读点 |
| **D 可复现** | ❌ `--locked` 不存在 | ✅ `--locked`/`--frozen` 断言并点名漂移;关掉快路径以免空转 |
| **E 可交付** | ❌ 全空白 | 🟢 `mcpp sbom`(CycloneDX 1.5)+ `docs/19` 支持窗口;许可闭包门与离线快照仍未做 |
| **F 可扩展** | ✅ | ✅ 未受损:新板 = 新包,引擎 diff 为零 |

### 11.2 ⭐⭐ 方案 §2.2 的判断被实施证实,§2.3 的被加强

* **两值语义是对的。** `debug` 起服务端、客户端归 IDE 这条边界成立,`debug` 与
  `monitor` 在实现里逐字段同形,没有出现方案担心的「会话协议」。
* **`runner-exclusive` 比方案写的更必要。** 方案说它是「第一块真板挖出的一列」;
  实施时发现它还必须**只紧不松** —— 图里任何一个包知道设备是互斥的,它就是互斥
  的,后来的包保持沉默不得放松它。

### 11.3 ⚠️ 实施挖出的、方案没有的两条

1. **一条规则的第二份拷贝。** 依赖提供的 RunGlobal 条目抵达根工程走的是与
   `apply()` **不同**的路径(`prepare.cppm` 的 BFS 之后)。只接了前者时,
   `mcpp flash` 报「没有配置」而 `mcpp run` 找得到同一个构建程序发出的 runner。
   两处现在都遍历槽表。
2. **快路径会让新槽与 `--locked` 双双空转。** `try_fast_run` 直接 exec 缓存产物,
   于是 `mcpp flash` 打印 `Running target/…/bin/p`;`try_fast_build` 跳过解析,
   于是被改坏的锁通过了 `--locked`。两处都按**性质**设闸(槽是不是 run、是不是
   要求断言),不是按旗标。

### 11.4 仍未做

`mcpplibs/picolibc` + `compiler-rt-builtins` 源码包 · 三个板级包 · `xim:probe-rs` ·
真机 CI · 许可闭包门(`--deny-license`)· 离线整仓快照 · openarch 第四后端(P3)。
