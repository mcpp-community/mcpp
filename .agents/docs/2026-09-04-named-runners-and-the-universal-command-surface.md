# 具名 runner、通用命令面、部分后端,与生态闭环

2026-09-04 · 生态级方案 **v6**(引擎与 openarch 已实施;§11 是实施回填)· 取代
`2026-09-04-commercial-grade-…-plan.md` §2 的槽表设计

前置:[`2026-09-04-commercial-grade-baremetal-embedded-plan.md`](2026-09-04-commercial-grade-baremetal-embedded-plan.md)
· PR #550(Cortex-M 七行,已合)· PR #551(旧设计,**按本文重做**)

---

## 0. 三条约束,和它们推翻的四版设计

> **① 一级命令必须所有场景都用得到。** 不通用的用 `--xxx`。
> **② 默认行为覆盖 80%,同时提供可复杂自定义的选项。**
> **③ 核心只放通用框架;其余走配置驱动与插件化。**

| 版本 | 形态 | 被哪条推翻 |
|---|---|---|
| v1 | `mcpp flash`/`monitor`/`debug` 三个一级命令 | ① 非固件工程里是死命令;嵌入式词汇写进引擎 |
| v2 | `mcpp <name>` 动态分派 | ① 更糟:一级命令面**因工程而异** |
| v3 | `mcpp run --runner flash` | ② **把 80% 的场景做成了 20% 的写法** |
| v4 | + `hardware` feature 重定义**默认** runner | (方向对,但生态零件不全) |
| **v5** | + 生态五零件 + openarch 能力拆分 + **板级包不再拼路径** | —— |

---

## 1. 全局 review:五条发现

本节是本轮最有价值的部分 —— 前四条都是**已有的东西没被用上**,而不是缺东西。

### 1.1 ⭐⭐⭐ 板级包在做引擎已经替它们做了的事

`mcpp.build.runner_lookup`(#544,2026-09-02)的模块注释原文:

> *"That is what lets a runner name a program the project declared under
> `[xlings] deps` **without writing the payload's home-and-version path into the
> manifest**"*

它按顺序搜索:**已声明 `[xlings] deps` 的载荷 `bin/` → PATH**,并且在哪儿都找不到
时给出点名搜索过哪些路径的错误。

而两个现有板级包**都还在用 #544 之前的写法**:

```cpp
// riscv-virt-rt / aarch64-virt-rt 今天
if (const char* qemu = mcpp::xpkg_dir("xim", "qemu-arm"); qemu && *qemu) {
    mcpp::runner(std::format("{}/bin/qemu-system-aarch64", qemu).c_str());
    …
} else {
    mcpp::warning("qemu-arm is not installed, so `mcpp run` has no runner…");
}
```

⇒ **正确写法是一行,并且更健壮:**

```cpp
mcpp::runner("qemu-system-aarch64");     // 引擎去找;找不到时它自己会说清楚
```

⚠️⚠️ **连带结论:`mcpp::warning()` 兜底在这个用例上不再需要。** 那条 advisory
通道是我在 2026-08-21 的评估里列为「最高优先级引擎缺口」并在 2026.8.21.2 落地的,
用来补救「`xpkg_dir` 返回空 ⇒ 静默不配 runner ⇒ `mcpp run` 报一句在此处不对的
建议」。**#544 用另一条路解决了同一个问题,而没有人把两者连起来。**
(`warning` 本身仍有别的正当用途,不撤。)

⭐ 这条对本轮的直接影响:`cortex-m-rt` 的 `build.mcpp` 会明显更短,且**没有那个
「声明≠安装」的失败模式**。两个既有板级包应当同步简化。

### 1.2 ⭐⭐ 烧录就是运行(80/20)

真板上「跑起来」= 烧进去 + 复位 + 接输出 + 取回退出码,`probe-rs run` 一条命令
就是这四件事。⇒ **`hardware` feature 重定义的是默认 runner**,不是新增具名的。

```bash
mcpp run     # 模拟器上跑    ← 默认 feature
mcpp run     # 真板上跑      ← hardware feature,命令一个字不改
```

具名 runner 只服务 20% 的例外:只烧不跑、看串口、调试服务端、擦片。

### 1.3 ⭐⭐ openarch 的「规范决定」不需要,机制已在

Cortex-M 没有 MMU、没有页表项,只满足可行性闸(上下文切换 + 页表项)的一半。
我原以为要先做一个规范决定。**实测:openarch 早就用能力绑定后端**
(`provides = ["openarch-backend"]` / `requires = […]`)⇒ **把能力拆细即可**:

```toml
有 MMU 的后端: provides = ["openarch-backend", "openarch:address-space", "openarch:percpu-register"]
Cortex-M:      provides = ["openarch-backend"]
```

需要地址空间的内核在**解析期**得到点名的话,而不是链接期一堆
`undefined reference to arch_pte_*`。**加法,不破坏既有消费者。**

### 1.4 ⭐ 同一个机制,现在用在三处

| 用处 | 声明者 | 引擎知道 |
|---|---|---|
| 目标侧五层(`docs/14`) | `provides = ["mcpp:c-abi=musl"]` | 层名,不知实现 |
| openarch 后端 | `provides = ["openarch-backend", …]` | 有后端这件事 |
| **具名 runner** | `mcpp:runner-named=flash:…` | 有具名 runner 这件事,**不知名字** |

**三处共用一个机制,不是三个机制。** 这正是「核心只放通用框架」。

### 1.5 ⚠️ 唯一的新风险:首次构建的墙钟

C 库改为**源码包**后,干净机器上第一次 `mcpp run` 要编一遍 picolibc。全局依赖缓存
(`docs/05 §2.10`,跨工程)使它是**每台机器每个目标档一次**,但第一次仍是第一次。

⚠️ **这与「默认覆盖 80%」直接冲突,必须实测。** 判据写在 §9;若 >60s,就要给
`mcpp new` 的模板加一句「首次构建会编译 C 库,约 N 秒」的状态行,而不是让人干等。

---

## 2. 唯一的概念:runner 有名字,默认的那个没有

```
包(build.mcpp)   mcpp::runner("qemu-system-arm")   默认 —— 覆盖 80%,写裸名
                  mcpp::runner("flash", tok)        例外 —— 覆盖 20%
                  mcpp::runner_longlived("monitor")
                  mcpp::run_exclusive()             ← 由 runner_exclusive 改名

线协议            mcpp:runner=<token>
                  mcpp:runner-named=<name>:<token>
                  mcpp:runner-longlived=<name>
                  mcpp:run-exclusive=1

工程              [target.X] runner = [...]
                  [target.X.runners] flash = [...]

用户              mcpp run / mcpp run --runner flash / mcpp run --list-runners
```

⚠️ **`longLived` 是声明的,不是从名字推的**:`openocd -c "program … exit"` 会终止、
`openocd -c "init"` 不会,拼写到最后一个参数为止都一样。从名字推只对引擎认识的名字
有效,而引擎不认识任何名字。

⭐ **`run-exclusive` 是改过的名字**(原 `runner-exclusive`)。它说的是「这个目标的
运行不能重叠」—— 对一块板、一个探针、一张 GPU、一个 license 受限的工具同样成立,
而 "device" 把它读窄了。

---

## 3. 一级命令面:全部通用,本轮零新增

```
new  build  run  test  clean  add  remove  update  search
publish  pack  emit  toolchain  cache  index  self
```

| 能力 | 归属 |
|---|---|
| 抵达产物的例外方式 | `mcpp run --runner <name>` |
| 物料清单 | `mcpp emit sbom`(`emit` 已是「生成描述本工程的文档」) |
| 可复现断言 | `--locked` / `--frozen`(与 `--offline` 同一条侧信道) |

---

## 4. 生态闭环:五个零件

判据是一句话:**干净机器上,`mcpp new blinky --template cortex-m-rt && mcpp run`
打印出东西。** 今天缺三件。

| # | 零件 | 仓 | 状态 | 缺了会怎样 |
|---|---|---|---|---|
| 1 | 编译器(llvm 载荷) | 目标表 | ✅ | —— |
| 2 | 模拟器 `xim:qemu-arm` | xim-pkgindex | ✅ 已发布**零消费者** | —— |
| 3 | **C 库 `mcpplibs/picolibc`(源码包)** | mcpp-index | ❌ | `'stdio.h' not found` |
| 4 | **`mcpplibs/compiler-rt-builtins`(源码包)** | mcpp-index | ❌ | 软浮点行 `undefined __aeabi_fmul` |
| 5 | **板级 `mcpplibs/cortex-m-rt` + 模板** | 新建仓 | ❌ | 用户自己写链接脚本与向量表 |
| 6 | 真机工具 `xim:probe-rs` | xim-pkgindex | ❌ | `hardware` feature 无从落地 |

### 4.1 ⭐ `[xlings]` 字段是闭环的接线点

```toml
# cortex-m-rt/mcpp.toml
[xlings]
deps = ["xim:qemu-arm@9.2.4-1", "xim:probe-rs@0.24.0"]
```

⚠️ **声明 ≠ 安装。** `[xlings] deps` 让 `runner_lookup` 知道去哪个载荷的 `bin/`
里找;**真正触发安装的是索引描述符的 `xpm.<平台>.deps`**。两处都要写,判据是
「把 store 里的包改名藏起来,再 `mcpp add` + `mcpp run`,它被装了回来」。

### 4.2 为什么 C 库是源码包而不是 xim 预编译

| prebuilt 要做的 | 源码包 |
|---|---|
| 7 个多库各建一次 | **没有多库** —— 用与程序完全相同的 `compile_flags` 编,ABI 一致按构造成立 |
| `libdir` 列与包目录逐字节对上(#481 的形状) | 列为空 |
| builtins 必须同包否则第一次 printf 挂 | 两个包,由依赖边表达 |
| 五宿主镜像、`.sha256`、CDN 等待 | 一个源码 tarball,宿主无关 |
| 版本钉在目标表里,**在 lock 之外** | 进 `mcpp.lock` |
| 每架构一个包(现已三个) | **一个包服务全部 11 行裸机目标** |

⚠️ 代价见 §1.5(首次墙钟),前提是 `--gc-sections`(已随 #550 落地)。

---

## 5. openarch:部分后端 + 真实应用

| 顺序 | 后端 | 为什么 | 真实应用 |
|---|---|---|---|
| 1 | 能力拆分 | 见 §1.3;加法,不破坏 | 现有三后端补声明 |
| 2 | **aarch32**(Cortex-A/R 32 位) | 14 个函数**一个不缺**(CP15 的 `TPIDRPRW`/`TPIDRURW` 恰是两个指针槽、真 MMU、`VBAR`、DMB/DSB/ISB);第一台 **32 位**机器,挖出 `arch_pte_make_leaf` 返回 `arch_u64` 的宽度假设 | `examples/switch` 扩到四机同源 |
| 3 | **Cortex-M**(部分后端) | 挖出「每台机器都有地址空间」这条从没被问过的基数假设 | ⭐ **抢占式任务切换器**,`examples/preempt` |

⚠️ **Cortex-M 的真实应用不是跑通探针,是一个能抢占的调度器。** 只做
`arch_context_switch` 往返的探针,与一个被 SysTick 打断、在 PendSV 里换栈再恢复的
调度器,考的不是同一件事 —— 后者才会暴露 `arch_trap_*` 在 M-profile 上「向量表是
按异常号索引的数组,不是单一入口」的语义变化。

---

## 6. 与主流对比

| | 抵达产物的模型 | 弱点 |
|---|---|---|
| **Cargo** | `cargo run` + `.cargo/config.toml` 的 `runner` | 每目标**只有一个**;由**用户**配置而非包提供 |
| **PlatformIO** | `pio run -t upload/monitor` | 动作词汇由平台固定 |
| **west (Zephyr)** | `west flash` + `runners.yaml` | **两套 CLI**;runner 是 Zephyr 专用 Python |
| **CMake** | 自定义 target | 无「抵达产物」概念、无发现机制,每工程重造 |
| **npm** | `npm run <script>` | 无产物/目标/交叉编译概念 |
| **mcpp v5** | `mcpp run`(80%)+ `--runner`(20%),**由包提供** | 首次构建墙钟(§1.5) |

⭐ Cargo 是最近的祖先,mcpp 超过它两点:**默认 + 任意具名**;**由板级包提供,用户
什么都不配**。
⭐⭐ 相对全部五者独有的:**嵌入式开发者用的命令与桌面开发者完全相同**。

---

## 7. 场景

| 领域 | 默认 runner | 具名(20%) | 用户敲 |
|---|---|---|---|
| 桌面 | 直接执行 | — | `mcpp run` |
| MCU 模拟器 | `qemu-system-arm` | — | `mcpp run` |
| MCU 真板 | `probe-rs run` | `flash` `monitor` `debug` `erase` | `mcpp run` |
| 嵌入式 Linux | `ssh … ./app` | `deploy` `logs` | `mcpp run` |
| Web / wasm | `wasmtime` | `serve` | `mcpp run` |
| 移动端 | 模拟器启动 | `install` `logcat` | `mcpp run` |
| HPC | 本地跑一份 | `submit` | `mcpp run` |

```bash
mcpp new blinky --template cortex-m-rt && cd blinky
mcpp run                                    # 模拟器,零配置              ← 80%
# 板子到了,改一行 features = ["hardware"]
mcpp run                                    # 真板,命令没变              ← 80%
mcpp test                                   # 板上跑,自动一次一个
mcpp run --list-runners                     # 这块板还能做什么
mcpp run --runner monitor                   # 看串口                      ← 20%
mcpp emit sbom -o sbom.json ; mcpp build --locked
```

---

## 8. 跨仓库依赖与发布顺序

⚠️ **消费者先发布,索引 `latest` 才能动**(否则干净环境全红而开发机全绿)。

```
xim-pkgindex:  probe-rs ──────────────┐
mcpp:          引擎(具名 runner)──┐   │
mcpp-index:    compiler-rt-builtins │  │
                     ↓              │  │
               picolibc ────────────┤  │
                     ↓              │  │
新建仓:        cortex-m-rt ─────────┴──┘   (依赖 picolibc + 声明两个 xim 工具)
                     ↓
openarch:      能力拆分 → aarch32 → cortex-m + examples/preempt
```

发布次序:`probe-rs` / `builtins` → `picolibc` → 引擎 → `cortex-m-rt` → openarch。

---

## 9. 本轮范围与判据

| # | 事项 | 判据 |
|---|---|---|
| 1 | 引擎:具名 runner、`--runner`/`--list-runners`、`run-exclusive` 改名 | e2e:默认+具名+缺席拒绝+一名一提供者 |
| 2 | `mcpp emit sbom`;`--locked` 保持 | 已有 e2e 333 改写 |
| 3 | `mcpplibs/compiler-rt-builtins` + `mcpplibs/picolibc` 源码包 | riscv64 上与 `xim:picolibc-riscv` 的 `Size text` 相当且输出相同 |
| 4 | `mcpplibs/cortex-m-rt` + 模板,**裸名 runner** | 干净沙箱 `mcpp new && mcpp run` 打印 |
| 5 | `xim:probe-rs` | 干净机器装完 `probe-rs list` 有输出,闭包不越界 |
| 6 | openarch 能力拆分 + aarch32 + Cortex-M + `examples/preempt` | 四机同源;抢占调度器在 mps2-an385 上换栈成功 |
| 7 | 简化 `riscv-virt-rt`/`aarch64-virt-rt` 为裸名 | 两包 CI 仍绿,`build.mcpp` 变短 |
| 8 | `docs/18`、`docs/19`(中英)+ 方案回填 | —— |
| ⚠️ | **首次构建墙钟实测** | §1.5;>60s 则加状态行 |

**后续轮次(本轮不做,已记):** 真机 CI(自托管 runner + 两块板)· `--deny-license`
许可闭包门 · 离线整仓快照 · `xim:openocd`(槽抽象的第二实现)。

---

## 10. 仍待定

1. `mcpp run` 在真板上会写 flash —— 需要 `Flashing … then running` 的状态行,不能
   静默写片。(已定:**要**这行)
2. `examples/preempt` 放 openarch 仓(与 `examples/switch` 并列,判据同一处)。(已定)
3. 既有两个板级包的简化是否本轮做 —— 我建议**做**,它同时验证 §1.1 的结论。

---

## 11. 实施回填(2026-09-04)

### 11.1 已落地

| # | 事项 | 仓 | 判据 |
|---|---|---|---|
| 1 | 具名 runner、`--runner`/`--list-runners`、`run-exclusive` 改名 | mcpp | e2e 333(十条)✅ |
| 2 | `mcpp emit sbom`;`--locked` | mcpp | 同上 ✅ |
| 3 | **依赖声明的工具可按裸名解析** | mcpp | e2e 334 ✅ |
| 4 | 两个既有板级包简化为裸名 | riscv-virt-rt / aarch64-virt-rt | 各自 example 启动并打印 ✅ |
| 5 | openarch 能力拆分 | openarch | 三后端补声明 ✅ |
| 6 | **openarch Cortex-M 部分后端** | openarch | 链接并启动 ✅ |
| 7 | **`examples/preempt` 抢占式调度器** | openarch | 两任务互相观测到被抢占 ✅ |

### 11.2 ⭐⭐⭐ 实施挖出的第一条:`xlingsDepBinDirs` 只看根清单

§1.1 断言「板级包应当写裸名」。实测**它不工作** —— `runner_lookup` 搜索的目录
只从 `runtimeOwnerManifest.xlings.deps` 收集,即**根工程**的声明。于是裸名在
「消费者自己声明了工具」时有效,在「板级包声明了工具」时失败。

**这正好是反的。** 板级包恰恰是那个知道「哪个模拟器/探针能抵达这台机器」的东西;
要求消费者也声明一遍,正是板级包存在所要消除的重复。

⇒ 收集范围扩到图中每一个包(根优先)。这条是 §1.1 那个简化能否成立的**前提**,
而方案里没有它 —— 因为在写方案时,那个失败模式还没有被触发过。

### 11.3 ⭐⭐ 实施挖出的第二条:openarch 缺一条抢占原语

`examples/preempt` 的第一版从 PendSV 里调 `arch_context_switch`。它**编译、链接、
启动、并报告两个任务谁也没观测到对方** —— 没有任何东西失败,只有那条计数器断言
分辨出了差别。

原因是结构性的:异常处理程序运行时,硬件已经把半个寄存器文件压到**被中断任务的
栈**上,而处理程序跑在 MSP 上。交换「调用者的被调用者保存寄存器」交换的是**处理
程序自己的**状态。

⚠️ **openarch 有「切到另一个已保存的上下文」,没有「切换这次陷入将要返回到的上下
文」。** 每个架构都需要后者才能抢占,而每个架构的写法都不同(riscv64 改 `sepc`、
aarch64 改 `ELR_EL1`、x86_64 改中断帧)—— 这正是这一层存在所要抽象的东西的形状。

⭐ **`examples/switch` 不可能发现这条:它从不进入陷入。** 这是「真实应用而不是探针」
这个要求兑现出的价值。

已记在 `examples/preempt/README.md`;是否把它命名为第五个接口组,应当在**不止一台
机器**在视野内时单独决定,不在本轮。

### 11.4 本轮未做(顺延)

`mcpplibs/picolibc` 与 `compiler-rt-builtins` 源码包 · `mcpplibs/cortex-m-rt`
板级包 · `xim:probe-rs` · openarch aarch32 后端 · 真机 CI · `--deny-license` ·
离线整仓快照。

⚠️ 其中 **picolibc 源码包 + cortex-m-rt** 是「干净机器上
`mcpp new --template cortex-m-rt && mcpp run` 打印出东西」这条闭环判据的最后两块;
`examples/preempt` 之所以能在没有它们的情况下跑通,是因为它是**零 libc** 的
(`sysroot = ""`),自带启动与向量表。
