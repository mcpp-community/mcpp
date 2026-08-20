# freestanding 生态实施计划与依赖图

**状态**:实施中。本文记录任务分解、依赖关系、每步的可证伪判据,以及**实施过程中
被实测推翻的设计主张**。

上游文档:

- [`2026-08-20-freestanding-ecosystem-positioning.md`](2026-08-20-freestanding-ecosystem-positioning.md)
  —— 五项决策及其依据。
- [`2026-08-20-openarch-implementation-design.md`](2026-08-20-openarch-implementation-design.md)
  —— openarch 的仓库形态与 A0 门。

证据来源分两类:标注为「实测」的结论在本机验证过,环境为 mcpp(由本仓库
`docs/baremetal-chapter` 分支构建)· `x86_64-linux-gnu` 宿主 · 目标
`riscv64-none-elf` / `riscv32-none-elf` · `xim:llvm 22.1.8` ·
`xim:picolibc-riscv 1.8.12` · `xim:qemu-riscv 9.2.4-1`;其余为设计主张。

---

## 0. 八维度评估

| 维度 | 评估 | 依据 |
|---|---|---|
| **架构** | 三条新能力各自落在既有机制上,**引擎未新增任何轴**:sysroot 与 `toolchain` 同轴,三个查询与 `sysroot_dir()` 同形,分配器走既有的 feature + capability | §2.1、§2.2 |
| **稳定性** | 每项都做了 **revert-A 探针**(关掉实现看判据是否变红);两处「同一决策两处推导」在加新能力时被就地收敛 | §3 |
| **优雅简洁** | 新增用户可见概念:**一个清单键 + 三个查询 + 一个 feature 名**。三个 provider 包各 30–80 行 | §2.3 |
| **用户体验** | 裸机 `std::vector` 从「自己写 12 个重载」变成一个 feature;失败路径全部在**解析期**报错并点名包 | §2.4 |
| **兼容性** | 索引 `min_mcpp` 未动;旧版本包全部保留;`sysroot` 缺席与空串可区分,故老工程行为不变 | §4 |
| **跨平台** | 引擎改动与宿主无关(纯解析与字符串);⚠️ 生态验证只在 Linux 上做过 | §7 |
| **一致性** | 分配器沿用 `backend-openblas` 已文档化的形状;裸机后端的选择键改为与其它板级事实同源 | §2.2 |
| **无感升级** | 新键在旧引擎上是一条 warning 而非错误;⚠️ 但 `feature-deps` 需 mcpp ≥ 2026.8.6.2,故 `std-freestanding` 0.3.0 对更旧客户端不可用(0.2.0 保留) | §4.2 |

---

## 1. 任务依赖图

```
引擎(单 PR,合入 mcpp-community/mcpp#466)
  T2 [target.X].sysroot + 零 libc 档 ────┐
  T3 三个目标查询 ──────────────────────┤
  T4 operator new 具名诊断 ─────────────┼──> T13 版本 bump + CI + 合入 + 发布
  T5 诊断承诺 e2e 守卫 ─────────────────┘         │
                                                  │
生态(五个仓库,并行)                              │
  T7 std-freestanding-alloc-kal ──┐               │
  T7'std-freestanding-alloc-libc ─┼──> T6 std-freestanding 0.3.0 ──┐
  T8 std-freestanding-nolibc ─────┘                                 │
  T9 openkal 裸机后端 ────────────┐                                 │
                                  ├──> T10 riscv-virt-rt 0.4.0 ─────┤
  T3(引擎)────────────────────┘                                 │
                                                                    ├──> T14 生态闭环验证
  索引 mcpplibs/mcpp-index#226 ─────────────────────────────────────┘
```

**关键路径**:T3 → T10 → 索引 → T14。板级包用不了未发布的查询接口,所以引擎必须先发布。

⚠️ **一个真实的顺序约束**:`riscv-virt-rt` 0.4.0 的 `build.mcpp` 调用
`mcpp::target_builtins_lib()`,而该名字只存在于本轮的 mcpp。用更旧的 mcpp 消费
0.4.0 会得到「`target_builtins_lib` 不是 `mcpp` 的成员」并附带升级提示 —— 这条提示
正是为这类情形补的,见定位文档 §3.1。0.3.0 保留在索引中,旧客户端仍可用。

---

## 2. 已完成项与其判据

### 2.1 T2 —— `[target.X].sysroot` 与零 libc 档

⭐ **一个旋钮解锁三条线**:内核要一份 C 库都不要、厂商 SDK 上的工程要换成 newlib、
`std-freestanding` 坐到 openkal 的 C 库上。

判据(两侧都钉,实测):

| | 结果 |
|---|---|
| 不覆盖 | picolibc 头可用,`text 12` |
| `sysroot = ""` | `fatal error: 'stdio.h' file not found` |
| 零 libc 自包含镜像 | **`text 108`,qemu 里打印 `zero-libc ok`** |
| 裸名 `"newlib"` | 解析期拒绝并说明期望形式 |

⭐ **revert-A 探针**:注释掉 `effective_sysroot` 的覆盖分支后,e2e/134 精确地死在
第 4 步那条承重断言。

### 2.2 T3 —— 三个目标查询

⚠️ 它解除的耦合**在任何 manifest 里都看不见**:`riscv-virt-rt` 自 #459 起既不声明
LLVM 也不声明 picolibc,却依然服务不了第二种工具链或第二份 C 库。

判据**不是**「能编过」(只有一种取值时永远成立),而是:

- 板级包代码里 `clang_rt` 与 `rv64gc/lp64d` 两个字面量**归零**(实测:仅存于解释性注释);
- rv32 / rv64 双档仍通过(实测:两档 `vector 5 last=16`)。

### 2.3 T6–T10 —— 分配器与零 libc 层

形状沿用 `docs/05` §2.8.2 已文档化的 `backend-openblas`:feature 作开关,
capability 作仲裁,实现在别的包。

实测的四条:

| | 结果 |
|---|---|
| `features = ["alloc-libc"]` | `vector 5 last=16`,**用户零 `operator new`** |
| `features = ["alloc-kal"]` + 板级 openkal 后端 | `kal vector 6 last=15`,全链路 `vector → operator new → kal_alloc → picolibc` |
| 只开 `alloc`,图中无 provider | 解析期:`no package provides capability 'freestanding-allocator'` |
| 两个 provider 同图 | 解析期:点名 `[std-freestanding-alloc-kal, std-freestanding-alloc-libc]` |

### 2.4 零 libc 内核模板

`mcpp new <name> --template riscv-virt-rt:nolibc` —— 实测 **369 字节**,无 C 库、
无启动对象、无板级包,在 qemu 中打印并停机。

---

## 3. ⚠️ 实施中被实测推翻或修正的设计主张

这一节是本文最有价值的部分:计划里凡是带具体断言的句子,实测都可能推翻它。

| 计划/直觉写的 | 实测 | 处置 |
|---|---|---|
| `std-freestanding` 需要堆,「没有办法说不要堆」 | tier-0 **完全不碰堆**;`vector` 编得过链不上;补 12 个重载即通 | 定位文档 §3.2 改写为分配中立 |
| 「`std-freestanding` 应基于 openkal」 | tier-0 全部未定义符号只有 `memmove` + `strlen`;耦合在**头文件**而非服务 | 接触面收窄到 tier-1 分配器一处 |
| 默认实现随包提供、程序可覆盖 | **依赖以裸 `.o` 参与链接,整份 build.ninja 零个 `.a`** ⇒ 归档语义不适用,两份定义是重复而非替换 | 改为「开关控制是否存在」,两份定义从不共存 |
| `[capabilities]` 可以消歧两个 provider | **消歧不裁剪链接行**,pin 之后死于 `multiple definition` | 单例符号类能力:两个 provider 是**待修的缺陷**;已写入 `docs/05` §2.8.1 |
| openkal 设计 §6.3 用 ISA 级 cfg 选裸机后端 | UART 基址是**板级**事实;换板后能编能链能跑**却不打印** | 后端改由板级包提供,选择键是那条板级依赖 |
| 模板用 `[build] ldflags = ["-T", "link.ld"]` | 链接器工作目录是构建目录 ⇒ `cannot find linker script link.ld` | 改用 `mcpp:link-script=`,它按包根解析 |
| `alignof(__max_align_t)` 可用 | 该标识符来自 `<stddef.h>`,零 libc 包没有 | 改用编译器预定义的 `__BIGGEST_ALIGNMENT__` |
| Cortex-M「只缺一行目标表」 | xim 索引里裸机 C 库只有 `picolibc-riscv`,qemu 只有 `qemu-riscv` | 真实成本是**两个新载荷** + 一行 + 一个包 |

⇒ 八条里有七条**若不实测就会写进文档**。

---

## 4. 兼容性与无感升级

### 4.1 不破坏既有工程

| 变更 | 影响面 |
|---|---|
| `[target.X].sysroot` 新键 | 缺席时行为与从前逐字节相同(`effective_sysroot` 返回目标表行) |
| 三个查询 | 纯新增;旧 `build.mcpp` 不调用即不受影响 |
| `operator new` 诊断 | 仅在构建**已经失败**时追加,且需链接器点名该符号 |
| 索引 | `min_mcpp` 未动;旧版本包全部保留 |

### 4.2 ⚠️ 一处真实的版本下限

`std-freestanding` 0.3.0 使用 `[feature-deps]`,需 mcpp ≥ 2026.8.6.2;
`riscv-virt-rt` 0.4.0 使用本轮的三个查询。两者的旧版本(0.2.0 / 0.3.0)**都保留在
索引里**,因此旧客户端解析既有的 pin 不受影响,只有主动请求新版本才会遇到下限。

这与「索引是数据、mcpp 是程序,发布数据不得让程序失效」一致:失效的是**新数据对
旧程序**,而旧程序请求旧数据仍然成立。

---

## 5. 未完成项及其阻塞原因

⚠️ 本节如实记录,不以「已规划」代替「已实现」。

| 项 | 状态 | 阻塞 |
|---|---|---|
| **openarch A0 的第二个 arch** | 受阻 | A0 的门要求**两个真实不同的 arch**。aarch64 缺目标表行与模拟器(xim 只有 `qemu-riscv`)。⭐ 只做一个 arch **不能**证明抽象不碎 —— 这正是该门存在的理由,伪造它比不做更糟 |
| **openarch 的 addrspace 判据** | 受阻 | 内存属性的语义判据需要在两个 arch 上真跑 |
| **Cortex-M(决策 E 步 1)** | 未开始 | 需新建 `xim:picolibc-arm` 与 `xim:qemu-arm` 两个载荷 |
| **`provides = ["freestanding"]` 的索引侧表示(决策 A)** | 形状已定,schema 未设计 | 需要索引 schema 变更,应单独立项 |
| **目标表工程内定义(决策 B(a))** | 未开始 | 与决策 A 同批评估更合适 |

---

## 6. 当前边界

| 边界 | 状态 |
|---|---|
| 生态侧验证 | **仅 Linux**。macOS / Windows 宿主上的裸机链无持续验证 |
| 两个 alloc provider 的互斥 | 由解析器保证,**已实测**;但未验证第三方 provider 的情形 |
| `std-freestanding-nolibc` 与 `-lc` 共存 | 设计上不可共存,**未实测**其失败信息是否可读 |
| openkal 裸机后端 | 实现 core 三件;`kal_stream_read` 走 semihosting,**未实测**输入路径 |
| 零 libc 模板的 rv32 档 | **未实测**(模板只写了 rv64 的 `[target.*]` 段) |
