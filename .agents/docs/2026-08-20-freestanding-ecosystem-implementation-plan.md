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
| ⭐ `nolibc` 与 `-lc` 并存会**重复定义而失败** | **冷构建成功**,`nm` 只找到一处 `memcpy` —— C 库是**归档**,成员只在符号仍未定义时才拉入,而包的 `.o` 先定义了它 | 危害不是链接失败,而是**静默替换**了 C 库的优化实现。已更正包注释、README、`docs/13` 中英两版 |

| ⭐ UEFI 需要一个不存在的 PE 形态裸机目标 ⇒ 受阻 | `x86_64-windows-gnu` + 三个链接 flag 即产出 `IMAGE_SUBSYSTEM_EFI_APPLICATION`,**零 DLL 依赖**,OVMF 下启动通过 | `openkal-uefi` 0.1.0 已发布;定位文档 §7.4 已更正 |
| `kind = "lib"` 能让依赖以归档参与链接,从而只拉被引用的成员 | 两个变体**逐字节相同**(`text 1027`),`build.ninja` 里零个 `.a` | 拆分 `nolibc` 的编译器档与库档**不做** —— 能让该边界产生收益的机制不存在 |

| ⭐ `[feature-deps]` 可以写 `"0.1.x"`(照抄自 `docs/05` §2.8.2 的示例) | **解析失败**。对照组:`cmdline` 的 `0.0.1` 通过、`0.0` 通过、`0.0.x` 失败 | `std-freestanding` 0.3.1 改用两段前缀;`docs/05` 中英两版的示例本身也已更正 |
| feature 是消费者控制的,不开就不包含 | **中间库能替根工程打开。** 实测:根不请求 `loud`,中间库请求了,根自己的翻译单元里就得到 `LOUD` | 这是 `nolibc` 必须独立成包的决定性证据 —— 做成 feature 的话,一个库能让每个消费者静默换掉 C 库的 `memcpy` |

| `nolibc` 也应做成 `std-freestanding` 的 feature(默认关),与 `alloc-*` 一致 | ⚠️ **我判断它「状态不可达」,而那个判断建立在一次测错的探针上**;正确测量后它**可达,但代价是一个独立项目**。见 §3.1 | 本轮不加;记为后续项 |

| 零 libc 模板发布即可用 | ⚠️ **生成出来就是坏的。** `mcpp new --template` 强制注入自依赖,而板级包自己的模块 `#include <stdio.h>` ⇒ 那个从未被请求的依赖在工程本身之前就编不过 | 引擎加 `[template.inject] self = false`;板级包在零 libc 档改为**不贡献而非报错**,但**照常发 runner** |

⇒ 十五条里有十四条**若不实测就会写进文档**。

⚠️ 最后这条是唯一一次**发布之后**才发现的:0.4.0 的模板在索引里可见、可生成,
而生成出来的工程跑不起来。发现它的方式是**用发布后的包从零走一遍新用户流程** ——
和上一轮 `.3` 那次同一个方法,也同一个教训:**「我这儿能跑」用的从来不是新用户的路径。**

⭐ 上面这条值得单独记:提法本身是对的(默认关的 feature 与主动加依赖等价,且能让包的
表面一致),**我原来反对它的理由也确实站不住**(中间库无论经 feature 还是经直接依赖
都能把它注入全图)。否掉它的是一个**第三方事实**:两个包服务于互斥的安排。

⇒ 判据不是「这个设计好不好」,而是「有没有一个用户能处在需要它的状态」。

⭐ **`.x` 那条还有第三层**:同一个坏形式也在 mcpp **自己的测试套件**里
(`test_manifest.cpp` 的 `zlib = "1.3.x"`)。文档教它、测试用它、生态照抄它 ——
而三处没有一处会去解析它,因为 fixture 只需要**能解析清单**,从不问索引。
⇒ 已在引擎侧补上校验:依赖版本用**已有的** `version_req::parse_req` 检查,
并用单测钉住五种既有可用形式不被收窄。

⚠️ ⭐ **`0.1.x` 那条是本轮最贵的一次,而且它花了三个版本才修对。**

| 版本 | 写法 | 结果 | 击败它的弱判据 |
|---|---|---|---|
| 0.3.0 | `"0.1.x"` | `E_NOT_FOUND`,点名的是**包**(而包存在) | 「本地测试全过」—— 全用 path 依赖 |
| 0.3.1 | `"0.1"` | 解析通过,随后 `install path missing after fetch` | 「没有 `E_NOT_FOUND`」 |
| 0.3.2 | `"^0.1.0"` | ✅ 构建通过 | —— |

⭐ **三个判据强度递增,而前两个各放行了一种不可用的写法:**

    本地能编过  <  没有 E_NOT_FOUND  <  装得上  <  对着真实索引构建成功

只有最后一个能定论。**path 依赖根本不查索引**,所以开发期的「全部通过」证明的是别的
东西 —— 而这条链上每一环我都在当时认为已经验证过了。

⚠️ 最后一条是本轮唯一一次**已经写进了已发布文本才被推翻**的:它进了包的 mcpp.toml、
README、索引描述符与 `docs/13`,之后才被实测否掉。推理链本身没错(依赖确实以 `.o`
参与链接),错在**没有把它与「C 库是归档」这一半合起来看**。
「结构上可能 ≠ 运行时确实」这一条,这次是在我自己身上生效的。

### 3.1 ⭐ 零 libc 档上的标准库子集:一次测了四遍才对

问题是「`std-freestanding` 能不能在零 libc 档上用」。前三次测量全部无效,而**每一次
我都以为已经有答案了**:

| 次 | 做法 | 为什么无效 |
|---|---|---|
| 1 | 手搓 `clang++ -nostdinc++` 编各个头 | 回落到**宿主 glibc** 的头 |
| 2 | 加 `-nostdlibinc` | 载荷的 `clang.cfg` **无条件注入** `-isystem <宿主 glibc>`,仍然穿透 |
| 3 | 由此得出「32/103」 | 该数字量的是载荷配置,不是零 libc |
| 4 | ⭐ **用 mcpp 本身构建**(它发 `--no-default-config`) | 有效 |

⇒ **判据必须走被测系统本身。** 手搓的命令行不是 mcpp 的编译行,而两者的差别恰好
就是这个问题的答案所在。

正确测量的结果:

- 零 libc 档上,**编译器自带的** freestanding C 头都在(`stdint.h` `stddef.h`
  `stdarg.h` `limits.h` `float.h`);缺的只是真正的 libc 头(`string.h` `stdio.h`)。
- 21 个 tier-0 头里 **15 个直接可编**(`array` `span` `expected` `bit` `charconv`
  `concepts` `type_traits` `tuple` `utility` `compare` `limits` `numbers` 等)。
- 余下 6 个的真因是 **libc++ 自带 C 头的包装头**(如 `include/c++/v1/string.h`),
  它靠 `#include_next` 到 C 库那份去取 `size_t`;没有 C 库时断链。
- 加一份最小 `string.h` shim(⚠️ 必须放在 libc++ 之后,否则 `#include_next` 跳过它)
  解锁 `optional` 与 `coroutine`;`string_view` 还差 `mbstate_t`(`<wchar.h>`),
  `atomic` 还差 `time_t`(经 `<chrono>` 到 `<time.h>`)。

⇒ **结论:可达,但它是一个独立项目而不是一个 feature。** 让 `std-freestanding` 在零
libc 档可用,需要 `std-freestanding-nolibc` 从「五个函数」长成「五个函数 + libc++
真正需要的那一小组 C 头」。⚠️ 一份写错的 `mbstate_t` 不会报错,只会让类型静默不匹配 ——
这类东西不能顺手做。

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
| **openkal 后端覆盖面** | ⭐ **已扩到四个**:linux / macos / windows / musl 之外,新增 `openkal-opensbi`(RISC-V 可移植)与 `openkal-uefi`(固件),加上板级包内的裸机后端 | —— |
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
| `std-freestanding-nolibc` 与 `-lc` 共存 | ⚠️ **已实测,而且推翻了设计主张** —— 见 §3 末行 |
| openkal 裸机后端 | 实现 core 三件;`kal_stream_read` 走 semihosting,**未实测**输入路径 |
| 零 libc 模板的 rv32 档 | **已实测**:355 字节,在 `qemu-system-riscv32` 中打印并停机。⚠️ 需手改 `[target.<triple>]` 段的键 —— 该段按精确三元组匹配,而模板只能写出一个 |
