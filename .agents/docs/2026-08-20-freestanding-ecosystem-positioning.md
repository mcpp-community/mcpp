# 裸机 / 嵌入式 / 内核方向的生态定位与缺口(设计方案)

**状态**:设计,未实施。本文不新增里程碑,它给出五项决策的形状与判据,
以及下一个真实案例的选择依据。

配套文档:

- [`2026-08-19-freestanding-baremetal-design.md`](2026-08-19-freestanding-baremetal-design.md)
  —— 裸机方向的原始方案,§11 提出 openkal / openhal / openarch 三层。
- [`2026-08-19-freestanding-baremetal-implementation-plan.md`](2026-08-19-freestanding-baremetal-implementation-plan.md)
  —— §7 是 D 档四阶段路线图与每阶段的继续/停止判据。
- [`2026-08-20-openkal-design.md`](2026-08-20-openkal-design.md)
  —— 内核 ABI 规范的推导,§6.2 给出裸机 UART 后端。
- [`2026-08-20-pr455-459-freestanding-review.md`](2026-08-20-pr455-459-freestanding-review.md)
  —— 引擎侧五个 PR 的 review。
- `docs/13-baremetal.md`(PR#466)—— 面向用户的章节。

证据来源分两类:标注为「实测」的结论在本机验证过,环境为 mcpp 2026.8.20.1
(由本仓库构建)· `x86_64-linux-gnu` · `xim:llvm 22.1.8` · `xim:picolibc-riscv 1.8.12` ·
`xim:qemu-riscv 9.2.4-1` · `mcpplibs:riscv-virt-rt 0.3.0` · `mcpplibs:std-freestanding 0.2.0`,
日期 2026-08-20;其余为设计主张,或转引自上列文档并注明出处。

---

## 0. 多维度评估摘要

| 维度 | 当前状态 | 依据 |
|---|---|---|
| **构建轴** | 完整。ISA 档位、目标 libc、启动对象、链接脚本、执行方式各有归属,且各只有一个读取点 | §2.1 |
| **设备轴** | 空。无 PAC、无 HAL、无中断模型、无设备树 | §2.2 |
| **可移植性表达** | ⚠️ 缺口。库无法声明自己 freestanding-safe,消费者靠链接失败发现 | §4 |
| **目标可扩展性** | ⚠️ 缺口。加一个目标是引擎代码变更,生态作者做不到 | §5 |
| **OS 服务抽象** | 已实现(openkal 0.5,八个接口),裸机后端未实现 | §6 |
| **设备服务抽象** | 已设计未实现(openhal,D2),停止判据已写明 | §2.2、§10 |
| **内核方向** | 构建侧可用;⚠️ 零 libc 档缺失,x86_64 裸机目标缺失 | §7 |
| **案例覆盖** | ⚠️ 一台几乎没有外设的虚拟机。分层的证据强度是所有结论里最弱的一环 | §8 |

---

## 1. 现状核定:`riscv-virt-rt` 是什么

### 1.1 实测构成

实测:`src/riscv_virt_rt.cppm` 共 **46 行**,导出
`board::print/println/printf/alloc/release/copy/fill/poweroff`。除 `poweroff` 外
全部是 picolibc 的直接转发。`poweroff` 是**全包唯一一次寄存器访问**,向 `virt`
的 syscon `0x100000` 写 `0x5555`。

包真正承重的部分在 `build.mcpp`(约 30 行),而其中三件事都是**选择**而非实现:
选 `crt0-semihost` 等四个库、指名 picolibc 的 `picolibcpp.ld`、拼 qemu 的 argv。

### 1.2 它在分层中的位置

它**不是 HAL**:没有 GPIO、定时器、中断、DMA,没有任何外设类型。
它也**不是 PAC**:没有寄存器定义。

它是一个板级支持包,粒度是**每台机器**(QEMU `virt`),不是每个 ISA。它同时服务
rv64 与 rv32,因为 `virt` 两个宽度都存在且 picolibc 两个 multilib 档位都有。

### 1.3 与 Rust `-rt` 约定的对应

名字沿用 `cortex-m-rt` / `riscv-rt` 的 `-rt` 约定,但**比它们薄一层**:

| | Rust `riscv-rt` | `riscv-virt-rt` |
|---|---|---|
| 复位向量 | 自己实现 | picolibc 的 `crt0-semihost` |
| `.bss` / `.data` 初始化 | 自己实现 | 同上 |
| 链接脚本 | 自带 `link.x` | 指名 picolibc 的 `picolibcpp.ld` |
| 入口宏 | `#[entry]` | 普通 `int main()` |

⇒ 它的贡献是**接线**而不是实现。这在 `virt` 上是优点(代码少、不重复造轮子),
但也意味着**它没有验证过「板级包能否实现启动逻辑」这件事**。

---

## 2. 分层与归属

### 2.1 现有各层与归属

| 层 | 归属 | 载体 | 状态 |
|---|---|---|---|
| ISA 档位(`-march`/`-mabi`/`-mcmodel`) | 引擎 | `src/freestanding/target.cppm`,每目标一行 | ✅ 2 行 |
| 编译器 + 目标 libc | 目标表行 | `triple.cppm` 的 `pin` + `sysroot` | ✅ |
| 启动对象 / 链接脚本 / 执行方式 | 板级支持包 | `build.mcpp` 三条指令 | ✅ |
| 可移植语言设施 | 普通依赖 | `std-freestanding` | ✅ 103/110 头(实测) |
| OS 服务抽象 | 普通依赖 | `openkal` + 后端 | ✅ 规范与两个后端;⚠️ 无裸机后端 |
| 设备服务抽象 | 普通依赖 | `openhal` | ⚠️ 已设计未实现(D2) |
| arch 机制 | 普通依赖 | `openarch` | ⚠️ 已设计未实现(D3) |

### 2.2 构建轴完整,设备轴为空

`riscv-virt-rt` 能到达「hello world + 堆 + 退出码」,是因为 **semihosting 提供了
一个长得像宿主的控制台**。而 semihosting 是调试器功能:`qemu -semihosting` 与
JTAG 探针提供它,量产板子不提供。

⇒ **该路径不能原样迁到真实硬件。** 真实板子上 `board::println` 需要一个
picolibc 后端,而该包不提供。

### 2.3 ⚠️ 引擎永不认识 KAL / HAL / ARCH

沿用方案 §11.6 / §14.0 的结论,本文不修改它:引擎只有 L1–L3 三条缝,
openkal / openhal / openarch 都是**包**。后端选择是**条件依赖,零新增轴**
(openkal 设计 §6.3 已按此实现)。

本文的五项决策全部遵守该约束:§4 复用既有的 `provides`,§5 只动目标表的**输入
来源**而不动其读取点,§6/§7 是包与载荷,§8 是案例选择。

---

## 3. 对比 Rust `no_std`:五处结构差异

### 3.1 ⭐ `no_std` 是 crate 属性,freestanding 是 target 属性

Rust 中一个库写 `#![no_std]`,**同一份 crate** 宿主与 MCU 都能编。
mcpp 中 freestanding 住在目标表,引擎对**图中每个 TU**强制
`-ffreestanding -nostdinc++ -fno-exceptions -fno-rtti`。

⇒ 包**无法声明**自己 freestanding-safe,索引也无法据此筛选。这是 §4 要解决的。

### 3.2 `core` 不需要 libc,`std-freestanding` 需要

`core` 自包含,`alloc` 需显式 `#[global_allocator]`。
`std-freestanding` 是 libc++ 的头压在目标 C 库之上;`malloc` 由 picolibc 提供,
**今天没有办法说「不要堆」**。这是 §7 零 libc 档的动机之一。

### 3.3 目标数量与可扩展性

rustc 内置约 40 个裸机目标,mcpp 有 2 个。数量差距本身不重要——重要的是
**rustc 的自定义目标 JSON 早于 tier-3 目标很多年**,生态先跑起来才谈内置。
mcpp 今天没有等价物。这是 §5。

### 3.4 分层深度

Rust:`riscv`(ISA)→ `riscv-rt`(运行时)→ PAC(每芯片,SVD 生成)→
HAL(`embedded-hal` trait)→ BSP(每板)。共五层。

mcpp 把这些压进一个板级包。对 `virt` 够用,**撑不过同一厂商的第二颗芯片**——
PAC / HAL 的切分正是让 50 块板共用一份 HAL 的东西。

### 3.5 网络效应在 trait 不在目标数

一个驱动 crate 写给 `embedded-hal::spi::SpiDevice`,在每颗芯片上都能用。
openhal 的 D2 判据(**同一个驱动包既跑裸机 MCU 又跑 Linux**)正是对标这一形状,
且其停止信号写得很清楚:**驱动作者不来就停**。

---

## 4. 决策 A:freestanding 成为包能力,且由证据推导

### 4.1 包含关系成立于源码层

「没有 OS 能用,有 OS 更能用」成立,但要说准:它成立在**源码层**而非产物层。
freestanding 构建被强制四个 flag,所以「能 freestanding 编」等价于
「这份源码不用 OS、不用异常、不用 RTTI、不用 hosted std」。这样的源码在有 OS 时
当然也能编——但会被**按 hosted 重新编一遍**,不是复用同一份产物。

### 4.2 机制已存在

`provides` 已经是包级键,不需要新增任何段:

```toml
[package]
provides = ["freestanding"]
```

与 `blas` / `lapack` 同一套词汇(`docs/05` §2.8.1)。

### 4.3 ⚠️ 声称不足以成立,判据必须是构建

openkal 设计 §5 为此把整套能力位机制删掉,理由是
「无法在不定义的前提下声称」。同一条纪律必须套在这里,否则重演
`caps::seek = true` 而 `seek()` 永远失败。

本处的判据近乎免费:

> ⭐ **包能为 `riscv64-none-elf` 编过,它就是 freestanding-safe,by construction。**
> 因为引擎已经把那四个 flag 强制到全图,没有第二件事需要查。

⇒ 形状是**证据推导而非作者声称**:包 CI 里加一条 freestanding 构建,索引记录
该事实。这与 PR#451 的 `[[runtime.artifacts]]` 承载证据同形。

### 4.4 消费端收益

| | 今天 | 决策 A 之后 |
|---|---|---|
| 裸机工程 `mcpp add` 一个 hosted-only 库 | 解析成功、构建到链接期报一批未定义符号 | 解析期即可警告或拒绝 |
| 索引检索「哪些库能裸机用」 | 不可能 | 可枚举 |
| 库作者知道自己是否还兼容 | 不知道 | CI 变红 |

### 4.5 今天的兜底

实测:`cfg(os = "none")` 与 `cfg(arch = "riscv64")` 都能匹配,库因此可以按
freestanding 走另一套源码或 flag:

```toml
[target.'cfg(os = "none")'.build]
defines = ["PROBE_OS_NONE=1"]
```

```
cfg(os = "none") matched
cfg(arch = "riscv64") matched
```

⚠️ 但那是逐包自觉,**索引检索不到**,因此不能替代决策 A。

---

## 5. 决策 B:目标表的三步可扩展路径

### 5.1 三条路径

| | 形态 | 代价 | 兼容风险 |
|---|---|---|---|
| **(a) 工程内定义目标** | `[target.<triple>]` 今天只能**覆盖**已知 triple 的旋钮,扩成能**定义**未知 triple(march / mabi / mcmodel / sysroot / pin) | 最小 | **零**:工程本地,老 mcpp 不认识该 triple 时干净失败 |
| **(b) 目标定义包** | 目标行随包走,板级支持包自带它的 target | 中 | 低 |
| **(c) 索引化** | 已定为阶段二 | 大 | ⚠️ 必须降级 |

### 5.2 推荐顺序:(a) → (b) → (c)

理由是**前一条产出后一条所需的证据**。(a) 立即解除生态作者的阻塞,并且在设计
(c) 的索引 schema 之前,先积累「真实目标行长什么样」的样本。Rust 的自定义目标
JSON 就是这个位置。

⚠️ (c) 必须遵守既有结论:**索引是数据,mcpp 是程序,发布数据不得让程序失效**。
新目标行在老客户端上必须降级为「不认识这个 triple」,而不是让整份索引加载失败。

### 5.3 ⚠️ 单一读取点不可破

`src/freestanding/target.cppm` 的注释写明它是**单一读取点**,存在理由是不让同一
决策在 N 处推导(#233 / #240 / #242 / #344 的教训)。

⇒ 清单定义的目标必须**喂进同一张表**,由 `resolve()` 统一读出,不得开第二条解析
路径。决策 B 改的是表的**输入来源**,不是它的**读取方式**。

---

## 6. 决策 C:`std-freestanding` 与 openkal 互补,不替代

### 6.1 两个不同的问题

| 问题 | 归属 | 类比 |
|---|---|---|
| 语言设施(容器、算法、`optional`、`span`) | `std-freestanding` | Rust `core` |
| OS 服务(读写、时钟、内存、进程) | `openkal` | Rust `std` 减去 `core`;或 WASI |

### 6.2 openkal 解决的那个

库写 `kal::` 而不是 libc / POSIX,后端由条件依赖选中。转引 openkal 设计 §1:
裸机侧后端直接写 MMIO,两侧 `app.o` 的外部符号集合完全相同,裸机侧**无未定义符号,
体积 157 字节**(该数字转引自设计文档,本文未复测)。

一整个 interface 不存在时,`import openkal.task;` 在**编译期**即找不到模块;
未提供的符号确实不导出,该性质用 `nm` 静态可查。

### 6.3 ⚠️ openkal 解决不了的那个

`std::format`、内建标量类型的 `std::sort`、完整的 `std::string` 在 libc++ 的
**编译版库**里(标量 `__sort` 的实例化是 `extern template`,没有可关闭它们的宏)。
这要的是**为目标编出的 `libc++.a`**,是载荷问题而不是抽象层问题。

⇒ openkal 不会让这三样出现,`docs/13` 的「当前边界」那一行不因 openkal 而改变。

### 6.4 裸机后端的形状

openkal 设计 §6.2 已给出最小裸机 UART 后端,约 15 行,选中方式是条件依赖:

```toml
[target.'cfg(all(arch = "riscv64", os = "none"))'.dependencies]
openkal-uart = "0.1"
```

⇒ **这是 D0 的「两个后端(linux / bare)」里缺的那一个**,也是 §7 零 libc 档的
天然第一个消费者。

---

## 7. 决策 D:零 libc 档与内核开发者支持

### 7.1 今天已经能用的

freestanding 目标 · 自带链接脚本(`link-script`)· 自定义 runner ·
`.bin` / `.map` / size 摘要 · `mcpp test` 在模拟器里读退出码。

⇒ 写内核最繁琐的构建部分基本齐备。实测:一个零依赖的 freestanding 工程可以构建,
产物 `text 12 data 0 bss 0 total 12`——这证明仅凭 ISA 表行就足以产出正确目标文件。

### 7.2 缺口

| 缺口 | 为何挡路 |
|---|---|
| **零 libc 档** | 目标行今天硬绑 `sysroot = xim:picolibc-riscv@1.8.12`。内核作者要的是**什么都不要**。需要 `sysroot` 可为空这一档,以及「`main` 指向携带 `_start` 的文件」这条路径的真实示例 |
| **`[target.X].sysroot` 覆盖** | 今天工程换不掉 libc 实现(`docs/13` 已记为边界)。零 libc 档与换 newlib 是同一个旋钮的两种取值 |
| **`x86_64-none-elf` 目标行** | 内核开发的主要战场之一,今天一行都没有 |
| **openarch(D3)** | Context / Trap / AddressSpace / PerCpu。⚠️ 其停止判据最硬:上下文切换或页表项抽象一碎,**后面全是幻觉** |

### 7.3 ⭐ 最小示例不是一个内核

给内核开发者的最小示例应当是**一个零 libc 的 `riscv64-none-elf` 工程**:
自带 `link.ld`、自带 `_start`、自己写 UART、`main` 指向汇编,启动到打印一行并停机。

它同时是三样东西:

1. 内核作者的起点;
2. 零 libc 档(`sysroot` 为空)的判据——**没有它,那一档没有任何测试**;
3. openkal 裸机后端(§6.2 那 15 行)的宿主。

⚠️ 判据必须是「拿走」而不是「在我这儿是好的」:把 picolibc 从解析中排除后仍能
构建并运行,才证明零 libc 档真的成立。

---

## 8. 决策 E:下一个真实案例

### 8.1 ⭐ 实测:载荷已注册的目标

`xim:llvm 22.1.8` 的 `clang -print-targets`:

**已注册**:`aarch64` · `arm` · `thumb` · `avr` · `msp430` · `riscv32` · `riscv64` ·
`loongarch32/64` · `mips` · `ppc` · `sparc` · `systemz` · `wasm32/64` · `x86` ·
`x86-64` · `xcore` 等。

**未注册**:`xtensa`。

⇒ 两条结论:

- **Cortex-M 不需要新的工具链载荷**,`arm` / `thumb` 已在其中。
- **ESP32 / S2 / S3(Xtensa)在钉住的载荷上不成立**,除非另做一份 Espressif 分支的
  LLVM 载荷。

⚠️ **但目标 C 库要新建。** 实测 xim 索引 `pkgs/` 下与裸机 C 库相关的包只有
**`picolibc-riscv.lua` 一个**,没有 arm 版 picolibc,没有 newlib,没有任何
`arm-none-eabi` 包。

⇒ Cortex-M 一步的真实成本是**四件**而不是三件:

| | 内容 | 性质 |
|---|---|---|
| 1 | `xim:picolibc-arm` 载荷 | ⚠️ **新建**。picolibc 上游支持 ARM,且 `picolibc-riscv` 是自建的(见 review §1 生态侧),因此这是**重复一次已走通的流程**,不是未知领域 |
| 2 | 目标表加行(`thumbv7em-none-eabi` 一类) | 一行 |
| 3 | 板级支持包 | 与 `riscv-virt-rt` 同形 |
| 4 | `xim:qemu-arm` 载荷 | ⚠️ **新建**。实测 xim 索引下只有 `qemu-riscv.lua` 一个 qemu 包,没有 `qemu-arm`。上游同为 xPack(`qemu-arm-xpack` 与 `qemu-riscv-xpack` 同一发布方),因此描述符可照 `qemu-riscv.lua` 改写 |

⇒ 修正判断:Cortex-M 一步**不是「加一行」**,而是**两个新载荷 + 一行 + 一个包**。
两个载荷都是已走通流程的重复,风险低但工作量真实存在。这一点必须在排期前说清楚,
否则会重演「计划里凡是带具体数字的句子都是必须先测的探针」那条教训。

### 8.2 ESP32 分型号

| 型号 | ISA | 结论 |
|---|---|---|
| ESP32 / S2 / S3 | Xtensa | ❌ 载荷未注册该后端 |
| ESP32-C3 / C6 | RISC-V rv32imc | ⚠️ ISA 上成立,但一次带进三个新问题 |

C3 的三个新问题:ROM bootloader 的镜像头(esptool 格式)· 没有 semihosting 需自行
初始化 UART · 外设实际依赖 ESP-IDF(FreeRTOS + CMake + Kconfig 一整套)。

⇒ **它测的是厂商 SDK 集成,不是裸机模型。**

### 8.3 推荐顺序

| 步 | 案例 | 它验证什么 | 为什么是这个位置 |
|---|---|---|---|
| **1** | Cortex-M + QEMU(`mps2-an385` 或 `lm3s6965evb`) | 「ISA 表是数据」在**真正不同的架构**上是否成立 | 不需硬件,CI 保持封闭;载荷已有 `arm`/`thumb` |
| **2** | RP2040(树莓派 Pico) | 真烧录 + 真 UART 控制台(无 semihosting) | 4 美元、开源 SDK 成熟、UF2 烧录路径清楚 |
| **3** | ESP32-C3(可选) | 厂商 SDK 集成 | 三个新问题应当逐个引入 |

### 8.4 ⚠️ 为什么顺序不能反

先做真实硬件会把「新 ISA」与「没有 semihosting」两个变量混在一起,失败时分不清
是哪一个。这与本轮已经付过学费的形状一致:**判据一次只应引入一个变量。**

### 8.5 ⭐ 真正的收益

真实案例会**逼出 PAC / HAL / BSP 的切分**,把「我们的分层是对的」从断言变成被检验
过的结论。

今天这套分层只在**一台几乎没有外设的虚拟机**上验证过,这是本文全部结论里证据最弱
的一环。

---

## 9. 板级粒度

**裸机不存在「通用 x86_64 / arm64」。** x86_64 裸机意味着在写 bootloader 或内核
(UEFI / multiboot);arm64 裸机永远在某颗具体 SoC 上。

| 可跨板共享 | 不可跨板共享 |
|---|---|
| ISA 级运行时、目标 libc、`std-freestanding`、openkal 契约 | 内存映射、外设、启动协议、链接脚本 |

⇒ 正确粒度是三段:**每 ISA 一份薄 `-rt` + 每芯片家族一份 HAL + 每板一份 BSP**。

`riscv-virt-rt` 现在把三段熔在一起,因为 `virt` 是虚拟机、几乎没有外设(§1.2)。
第二块板会强制这次切分,而这正是 §8 的目的。

---

## 10. 实施顺序与判据

⚠️ 本节不排期。顺序依据是「解除阻塞的程度 × 判据的可检验性」。

| 序 | 决策 | 判据(达成即继续) | 停止信号 |
|---|---|---|---|
| 1 | **A** freestanding 包能力 | 一个既有库为 `riscv64-none-elf` 构建通过并被索引记录;裸机工程 `mcpp add` 一个 hosted-only 库时在**解析期**被告知 | —— (代价极低,无停止条件) |
| 2 | **B(a)** 工程内定义目标 | 一个未进引擎表的 triple 仅凭 `mcpp.toml` 即可构建;`resolve()` 仍是唯一读取点 | 若发现必须开第二条解析路径 ⇒ 停,回到 §5.3 重设计 |
| 3 | **D** 零 libc 档 + 最小示例 | 把 picolibc 排除后仍能构建并在 qemu 打印一行 | —— |
| 4 | **E 步 1** Cortex-M QEMU | 两个新载荷(`picolibc-arm` / `qemu-arm`)+ 目标表一行 + 一个板级包,源码零改地跑通;⭐ **引擎零改动** | 若引擎必须改 ⇒ 「ISA 表是数据」不成立,回炉 |
| 5 | **C** openkal 裸机后端 | D0 的「两个后端」补齐;conformance 双向通过 | —— |
| 6 | **E 步 2** RP2040 | 真烧录 + 真 UART | —— |
| 7 | **B(b)/(c)** 目标定义包 / 索引化 | 老客户端降级正确 | —— |

⚠️ **openhal(D2)与 openarch(D3)不进本表。** 它们的门在 D 档路线图里,
且 D2 的停止信号是「驱动作者不来就停」——那不是技术判据,不能靠实施推进。

---

## 11. 当前边界

| 边界 | 状态 |
|---|---|
| 本文全部分层结论 | 只在 QEMU `virt` 一台虚拟机上验证过 |
| openkal 裸机后端的 157 字节 | 转引自 openkal 设计 §6.2,**本文未复测** |
| Cortex-M 可行性 | 依据是载荷注册了 `arm`/`thumb`,**未实际构建过任何 Cortex-M 产物** |
| `picolibc-arm` / `qemu-arm` | 实测 xim 索引中**均不存在**,两者都要新建;上游可用性(picolibc 的 ARM 支持、xPack 的 `qemu-arm-xpack`)**未逐一核实** |
| RP2040 / ESP32-C3 | 均未尝试 |
| `provides = ["freestanding"]` 的索引侧表示 | 形状已定(证据推导),**schema 未设计** |
| macOS / Windows 宿主上的裸机链 | 无持续验证(`docs/13` 已记) |

---

## 12. 决策回执

| | 决策 | 一句话 |
|---|---|---|
| **A** | freestanding 成为包能力 | 复用 `provides`,但**由构建证据推导而非作者声称** |
| **B** | 目标表可扩展 | **先工程内定义,再随包走,最后索引化**;单一读取点不可破 |
| **C** | `std-freestanding` 与 openkal 互补 | openkal 解决 OS 服务可移植;**解决不了目标版 `libc++.a`** |
| **D** | 零 libc 档 | 最小示例不是一个内核,是**一个零 libc 的启动工程**——它同时是该档的唯一判据 |
| **E** | 下一个案例 | **Cortex-M + QEMU 先行**(编译器载荷已有 `arm`/`thumb`,但 **C 库与模拟器两个载荷都要新建**);ESP32 分型号,Xtensa 不成立 |

⚠️ 五项决策全部不要求引擎认识 KAL / HAL / ARCH,也全部不新增引擎轴——
A 复用 `provides`,B 只改目标表的输入来源,C/D 是包与载荷,E 是案例选择。
