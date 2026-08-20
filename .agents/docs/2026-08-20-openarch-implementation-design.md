# openarch 实现方案:arch 机制层

**状态**:设计,未实施。本文给出仓库形态、接口划分、里程碑与**可证伪的判据**。

配套文档:

- [`2026-08-19-freestanding-baremetal-design.md`](2026-08-19-freestanding-baremetal-design.md)
  —— §11 提出三层,§11.7 给出 D3 的门。
- [`2026-08-19-freestanding-baremetal-implementation-plan.md`](2026-08-19-freestanding-baremetal-implementation-plan.md)
  —— §7 的 D3 行:交付物、前置、继续判据、停止信号。
- [`2026-08-20-openkal-design.md`](2026-08-20-openkal-design.md)
  —— 契约机制的先例;§21 的模块归属规则在本文沿用。
- [`2026-08-20-freestanding-ecosystem-positioning.md`](2026-08-20-freestanding-ecosystem-positioning.md)
  —— 本层在生态中的位置。

证据来源:本文**没有实测**。openarch 尚未有任何代码,所有关于每个 arch 的机制差异
均来自体系结构手册的公开知识,标注为「设计主张」。⚠️ 这一点很重要:
D3 的性质决定了**只有 A0 探针能产生第一条实测**,在那之前本文全部内容都是待验证的。

---

## 0. 一句话

openarch 是**唯一一个实现者集合有界的层**,因此它的仓库形态、里程碑顺序与判据
都应当与 openkal / openhal 相反:**单仓、先做最硬的、以证伪为目标**。

---

## 1. 与 openkal / openhal 的根本差异

| | openkal | openhal | **openarch** |
|---|---|---|---|
| 实现者集合 | **开放**:任何 OS / 内核 | **开放**:任何芯片 / 板 | ⭐ **有界且小**:x86_64 · aarch64 · riscv64 · 或许 arm32 / loongarch |
| 谁会去实现 | 第三方 | 第三方(D0 的真变量) | **基本只有自己** |
| 契约机制 | C ABI | concept | concept + `inline` / 汇编符号 |
| 成败取决于 | 第三方来不来 | 驱动作者来不来 | ⭐ **抽象碎不碎** |

⇒ 前两层是**生态问题**,openarch 是**技术问题**。生态问题靠等待和推广,技术问题
靠尽早证伪。本文全部设计围绕这一条。

---

## 2. 决策:单仓库

### 2.1 结论与理由

**`mcpplibs/openarch` 一个仓库,内含接口模块与全部 arch 后端。**

| 理由 | 说明 |
|---|---|
| 实现者是自己 | 没有第三方需要独立发版节奏 |
| ⭐ **接口与实现必须共同演化** | D3 的门是「加第二个 arch 时抽象不碎」——**只有加第二个 arch 才知道接口错在哪**。拆仓会把决定这一层生死的那个循环拖慢 |
| 选择机制现成 | `cfg(arch = ...)` 条件源码,引擎零新增轴 |
| 数量有界 | 四到五个 arch,单仓的维护面不会失控 |

### 2.2 ⚠️ 单仓不等于放弃边界

仓内仍**必须**保持模块边界,沿用 openkal §21:

- 接口模块 `openarch.context` / `openarch.trap` / … 由**规范侧**拥有;
- 后端**不导出任何模块**,只提供定义(concept 的 model、以及汇编符号);
- 后端源码按 `cfg(arch = ...)` 条件编入,**不参与模块名**。

⇒ 将来若某个 arch 需要独立发版,拆分是机械操作而不是重构。

### 2.3 ⚠️ 单仓的 CI 代价

接口仓天然 target 中立,而 openarch 的仓 **必须按 arch 做矩阵**:每个 arch 至少
「交叉编译通过 + 在模拟器里跑通探针」。只跑宿主那一个 arch 的 CI **等于没有 CI**,
因为本层的全部风险都在 arch 之间的差异上。

### 2.4 目录形态

```
openarch/
  mcpp.toml
  src/
    context.cppm          接口:concept + 类型            ← 规范侧
    trap.cppm
    addrspace.cppm
    percpu.cppm
    tick.cppm
    barrier.cppm
    boot.cppm
    arch/
      riscv64/ context.S  trap.S  boot.S  addrspace.cpp   ← 后端,cfg 条件编入
      aarch64/ context.S  trap.S  boot.S  addrspace.cpp
      x86_64/  context.S  trap.S  boot.S  addrspace.cpp
  tests/
    probe_context_zero_cost.cpp     ← A0
    probe_addrspace_attrs.cpp       ← A0
```

```toml
[target.'cfg(arch = "riscv64")'.build]
sources = ["src/arch/riscv64/**"]
[target.'cfg(arch = "aarch64")'.build]
sources = ["src/arch/aarch64/**"]
```

---

## 3. 接口划分:只含机制,不含策略

判别式沿用 openkal §2.2 的形式:

> 一个东西若在不同 arch 上**只是实现方式不同**,它是机制;
> 若在不同 arch 上**可以有不同的合理选择**,它是策略,不进本层。

| interface | 机制 | 明确不含的策略 | 变异程度 |
|---|---|---|---|
| `boot` | 从固件/前一级交接到 C++:建栈、清 BSS、取 CPU id | 用什么引导协议、设备树怎么解析 | ⚠️ **极高** |
| `context` | 上下文表示 + 切换 | 调度 | ⭐ **高(最硬之一)** |
| `trap` | 向量表安装 + 最小分发 | 每个 cause 做什么 | ⭐ 高 |
| `addrspace` | 页表项构造 + 安装 + TLB 失效 | VMA 管理、缺页策略 | ⭐⭐ **最高(最硬之一)** |
| `percpu` | 每 CPU 基址的读写 | 里面放什么 | 低 |
| `tick` | 设定下一次定时中断、应答 | 时间片长度 | 中 |
| `barrier` | `std::atomic` 覆盖不到的屏障(指令屏障、TLB 屏障) | — | 低 |

### 3.1 `boot` 为什么不可能统一签名

交接状态三者完全不同:riscv64 由 SBI 从 M 态进入,`a0 = hartid`;aarch64 从
EL2/EL1 进入,`x0 = DTB` 指针;x86_64 由 multiboot2(`ebx = info`)或 UEFI
(image handle + system table)进入。

⇒ **本层不统一入口签名,只统一「交接完成后的后置条件」**:栈可用、BSS 已清零、
`percpu::base()` 可用、`cpu_id()` 可读。入口本身是每 arch 一段 `.S`,由**上层**
(内核或 bootloader)决定用哪个引导协议。

---

## 4. ⭐ 两个最硬的原语

D3 的门只认这两个。本文其余部分都可以推迟,这两个不能。

### 4.1 上下文切换

**共通的洞察**:切换发生在一次普通函数调用点上,因此**只需保存 callee-saved 寄存器**。
调用者已经替我们保存了 caller-saved 的部分。这条对三个 arch 都成立,是抽象成立的基础。

| arch | 需保存 |
|---|---|
| riscv64 | `ra` `sp` `s0`–`s11`(13 个整数寄存器);若用浮点则加 `fs0`–`fs11` |
| aarch64 | `x19`–`x30` `sp`;`d8`–`d15` |
| x86_64 (SysV) | `rbx` `rbp` `r12`–`r15` `rsp` |

接口形状:

```cpp
export module openarch.context;

export namespace arch {

// 不透明存储。大小与对齐由后端给出,上层只持有它。
struct context;                      // 后端提供完整定义

// 把一个新上下文初始化成「一旦被切入就从 entry(arg) 开始执行」。
void context_init(context&, void (*entry)(void*), void* arg, void* stack_top);

// 保存当前到 from,恢复 to。返回时,当前上下文是 from。
[[gnu::returns_twice]] void context_switch(context& from, context& to);

}
```

⚠️ **`context_switch` 必须是纯汇编符号,不能是 C++ 函数。** 编译器插入的
prologue/epilogue 会在换栈之后运行,访问的是错误的栈。

⇒ **这意味着本原语的「零成本」不来自 `inline`,而来自「它本来就是一次 call」。**
concept 在这里的作用是给一个汇编符号一个类型化的门面,不是消除调用开销。
本文把这一点写明,因为方案 §16.2 的「显式 `inline` 是首选,LTO 是兜底」在这条上
**不适用**,照搬会得到一个编译不过或运行即崩的设计。

**零成本判据(可证伪)**:

> 用 concept 门面写的切换,与直接 `call` 汇编符号,**LTO 后反汇编逐指令相同**。
> 多出任何一条指令即判定不通过。

### 4.2 页表项与地址空间

变异是全层最大的:

| arch | 级数 | 页大小 | 内存属性机制 |
|---|---|---|---|
| x86_64 | 4 级 / 5 级 | 4K / 2M / 1G | **PAT 索引**(PTE 三位选 8 个 PAT 项) |
| aarch64 | 4 级(granule 相关) | 4K / 16K / 64K | **MAIR 索引**(PTE 三位选 8 个 MAIR 项),另有 TTBR0/TTBR1 分半 |
| riscv64 | Sv39 / Sv48 / Sv57 | 4K / 2M / 1G | ⭐ **PTE 位直给**,无间接表(Svpbmt 才有 PBMT 位) |

⭐ **内存属性是 A 类裂缝(能编但语义不同)的教科书例子**:三者都能表达
「device memory」与「normal cacheable」,但 x86/aarch64 需要**先配置一张间接表**,
riscv 不需要。一个只写 `map(va, pa, Cache::Device)` 的通用内核,在 riscv 上正确,
在 aarch64 上**取决于 MAIR 是否已被正确初始化**——而初始化 MAIR 是策略还是机制,
本身就是要在 A0 里回答的问题。

**判据(可证伪)**:

> 一段**通用**的映射代码 `map(va, pa, Perm::RW | Cache::Device)`,在 riscv64 与
> aarch64 上产生的映射,**用同一段通用测试代码验证语义相同**:写入后读回、
> 且该页在两边都确实是非缓存的(以 arch 特定手段观察)。
>
> 若必须在通用代码里出现 `#if arch` 或必须暴露「先初始化属性表」这一步给上层,
> ⇒ **该抽象碎了**,`addrspace` 应缩小到只做页表**结构**(级数、遍历、失效),
> 把属性移到能力轴。

---

## 5. 里程碑:先做最硬的

⚠️ **顺序与常规相反,而且这是刻意的。** 常规做法是先做容易的(`percpu`、`barrier`)
把仓库跑起来。在本层这是错的:容易的部分**全部做完也不能证明这一层成立**,
而一旦最硬的碎了,前面做的全部是沉没成本。

方案 §7.1 的停止信号已经写明:「⚠️ 碎在这里,后面全是幻觉」。

| 阶段 | 交付 | ⭐ 继续的判据 | ⚠️ 停止信号 |
|---|---|---|---|
| **A0 探针** | **只做 §4 两条**,riscv64 + aarch64,**不建完整仓库结构**,不做其余五个 interface | ① 上下文切换的反汇编逐指令判据通过;② 通用 `map()` 的语义判据通过 | 任一不通过 ⇒ **停**。缩小 openarch 范围至「结构而非属性」后重新评估,或整层放弃 |
| **A1 骨架** | 仓库形态(§2.4)· 接口模块 · `cfg` 条件源码 · **CI 按 arch 矩阵** | 两个 arch 都能交叉编译并在模拟器里跑通 A0 的两个探针 | CI 只能跑宿主 arch ⇒ 先解决 CI,不要继续加接口 |
| **A2 中变异** | `boot` · `trap` · `percpu` | trap 的 cause **不被统一成一个 enum**,而是 arch 特定类型 + 少数共通谓词 | 若为统一 cause 而丢失信息 ⇒ 退回 arch 特定 |
| **A3 低变异** | `tick` · `barrier` | tick 的**频率来源**被明确放在能力轴而非接口里 | — |
| **A4 第三个 arch** | x86_64 | ⭐ **接口零改动**地接纳 x86_64 | 若必须改接口 ⇒ 前两个 arch 把接口**过拟合**了,回到 A0 的判据重做 |

### 5.1 为什么第三个 arch 是独立的一关

两个 arch 可以偶然相似。riscv64 与 aarch64 都是 RISC、都有独立的页表根寄存器、
都用 load/store 架构。**x86_64 是最不像的那一个**(段、PAT、IDT 的 16 字节项、
`swapgs`),因此它才是接口是否被过拟合的检验。

⇒ A4 不是「再加一个 arch」,它是**A0 判据的第二次执行**。

---

## 6. 与其它层的接线

| | 关系 |
|---|---|
| **openkal** | 无直接依赖。openkal 是**内核对上**的 ABI,openarch 是**内核对下**的机制。一个内核同时用两者:用 openarch 建起地址空间与陷入,用 openkal 向应用提供服务 |
| **openhal** | 无直接依赖。⚠️ 边界已定:**寄存器 / ISR / DMA 进不了 openhal**,它们属本层与驱动层 |
| **`std-freestanding`** | 单向:openarch 可以用它的 tier-0 部分(`array` `span` `optional` `bit`),实测这些只需 `memmove` + `strlen` 两个纯函数 |
| **引擎** | ⭐ **零关系。** 引擎永不认识 openarch,后端选择是 `cfg(arch = ...)` 条件源码 |

---

## 7. ⚠️ 当前边界

| 边界 | 状态 |
|---|---|
| 本文全部 arch 差异描述 | **来自手册的公开知识,未实测**。第一条实测只能由 A0 产生 |
| 「只保存 callee-saved」 | 设计主张。中断上下文(与函数调用点不同)需要保存全部寄存器,本文**未覆盖**该情形 |
| 反汇编逐指令判据 | 形状已定,**工具链侧如何自动化未设计** |
| `aarch64` / `x86_64` 的模拟器与目标行 | 均不存在。A1 的 CI 需要 `qemu-system-aarch64`,而 xim 索引里**只有 `qemu-riscv`** |
| 中断上下文、SMP 启动、IPI | **本文未涉及**,应在 A2 之后单独评估 |
| 浮点 / 向量上下文 | 未涉及。它会让 `context` 的大小成为 cfg 的函数 |

---

## 8. 决策回执

| | 决策 | 一句话 |
|---|---|---|
| **1** | 单仓库 | 实现者是自己,且**接口与实现必须共同演化** |
| **2** | 仓内保持模块边界 | 后端不导出模块,将来拆分是机械操作 |
| **3** | CI 必须按 arch 矩阵 | 只跑宿主 arch 等于没有 CI |
| **4** | ⭐ **先做最硬的两条** | 容易的部分做完也不能证明这一层成立 |
| **5** | `context_switch` 是汇编符号 | 零成本不来自 `inline`,照搬 §16.2 会得到崩溃的设计 |
| **6** | 内存属性是 A 类裂缝 | 碎了就把 `addrspace` 缩到「结构而非属性」 |
| **7** | x86_64 是第二道门 | 它是过拟合检验,不是「再加一个 arch」 |
