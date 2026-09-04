# 具名 runner、通用命令面、部分后端,与生态闭环

2026-09-04 · 生态级方案 **v8:四条已定,批次已排**(引擎与 openarch 已实施;§11 是实施回填)· 取代
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

⚠️ **而 §12 的工具分档不是第四处** —— 它对齐的是 mcpp 已有的**依赖种类**词汇
(`dependencies` / `build-dependencies` / `dev-dependencies`),不是能力机制。复用既有
词汇同样是好事,但把四条并列成「同一条纪律」是修辞上的合并,不准确。**两个既有机制,
各用其所。**

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

### 4.1 ⭐ `[xlings.workspace]` 是闭环的接线点

```toml
# cortex-m-rt/mcpp.toml
[xlings.workspace]
"xim:qemu-arm" = "9.2.4-1"
"xim:probe-rs" = "0.24.0"
```

⚠️ **本文 v5 在这里写错了两处,已更正:**

| v5 写的 | 实际(2026.9.3 起) |
|---|---|
| `[xlings] deps = [...]` | **`deps` 已从清单里退休**,`[xlings.workspace]` 是作者写的那一张表。`deps` 仍被接受(根清单里报错、依赖清单里只提示),但不是该写的拼法 |
| 「声明 ≠ 安装,真正触发安装的是 `xpm.<平台>.deps`」 | **对根工程已经不对了。** `prepare.cppm:3246` 的原话是 *"the contract being added is 'what you declared gets installed'"* —— 根/workspace 清单声明的东西由 mcpp 自动装 |

### 4.1.1 ⚠️⚠️ 但由此浮出一个真正的设计问题:自动安装只覆盖根

`prepare.cppm:3150` — `runtimeOwnerManifest = wsManifest ? *wsManifest : *m`,
即 **workspace 或根工程的清单,永远不是依赖的**。于是:

| | 查找(`xlingsDepBinDirs`) | 安装(provisioning) |
|---|---|---|
| 根工程声明 | ✅ | ✅ 自动装 |
| **依赖(板级包)声明** | ✅ **本轮刚修** | ❌ **不装** |

⇒ 「板级包知道环境、消费者什么都不声明」这个故事,**查找那一半通了,安装那一半没通**。
干净机器上仍要靠**索引描述符的 `xpm.<平台>.deps`** 把工具随包装上。

⭐ **而这个不对称可能是对的,不是缺陷。** 两者的性质不同:

* **查找只读机器**。让它跨图是安全的 —— 依赖说「我要 qemu」,mcpp 去看看装没装。
* **安装写机器**。让它跨图是一次**权限升级**:一个传递依赖可以让 mcpp 往你机器上
  装任意包。

⇒ 于是分工可以是:**清单里的声明是未经审查的通道,只用于查找;索引描述符的
`xpm.<平台>.deps` 是发布时被审查过的通道,才有资格触发安装。** 板级包两处都写,
而那不是重复 —— 它们回答的是两个不同的问题(「去哪找」与「谁有权装」)。

⚠️ **这一条我没有把握,列为待定** —— 见 §12。

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

---

## 12. 决定:全图安装 + 工具按用途分档

### 12.1 ⭐ 我的「权限升级」反对是错的

我曾主张「让依赖的声明触发安装是一次权限升级」。**那个权限今天已经存在** ——
索引描述符的 `xpm.<平台>.deps` 就在做这件事:你依赖的包现在就能往你机器上装工具。

⇒ 全图安装**不增加任何权力**,它去掉的是一处重复拼写。我把它跟一个「描述符通道
不存在」的世界比了,而那个世界不存在。**选 B。**

### 12.2 ⚠️ 但 B 单独做会装得更多,而工具本来就不必全装

一个板级包声明 `qemu-arm`(跑要用)与 `probe-rs`(上真板要用)。今天:

* 只想 `mcpp build` 的消费者 —— **两个都装**,一个都用不上;
* 用模拟器的消费者 —— probe-rs 白装;
* 而包依赖早就分了 `[dependencies]` / `[build-dependencies]` /
  `[dev-dependencies]`,工具却没有这个轴。

⇒ **B 与分档要一起做。合起来之后,常见情形装得比今天更少。**

### 12.3 形状:一张表不变,条目可以是标量或表

2026-09-03 那份文档为「`workspace` 是**唯一**一张表」辩护过,不应推翻。所以分档
写在**条目**上,而不是新开几张表 —— 这与 mcpp 自己的依赖拼法同形
(`dep = "1.0"` 或 `dep = { version = "1.0", features = [...] }`)。

```toml
[xlings.workspace]
"xim:qemu-arm" = "9.2.4-1"                              # 80%:不写就是今天的行为
"xim:codegen"  = { version = "1.0", when = "build" }    # 20%
"xim:probe-rs" = { version = "0.24.0", when = "run" }
```

| `when` | 什么时候装 | 传播到消费者 |
|---|---|---|
| (不写) | **与今天完全一样**:构建时就装 | 是 |
| `build` | `mcpp build` 起 | 是 |
| `run` | 只在 `mcpp run` / `mcpp test` 时 | 是 |
| `dev` | 只在**声明它的那个包自己**被开发/测试时 | **否** |

⭐ **不写 `when` 保持今天的行为,所以没有迁移。** 分档是把范围**收窄**的可选动作,
不是必须回答的新问题 —— 这正是「默认覆盖 80%、其余可配置」。

### 12.4 feature 门控:沿用已有的 `[feature-deps]` 形状

真板工具只在 `hardware` feature 下才需要。包依赖已经有这个机制,工具沿用同一个
拼法而不是发明新键:

```toml
[feature-xlings.hardware]
"xim:probe-rs" = "0.24.0"
```

⇒ 用模拟器的人**永远不下载 probe-rs**。

### 12.5 合起来的效果

| 情形 | 今天 | B + 分档 |
|---|---|---|
| 消费者 `mcpp build`(板级包依赖) | 描述符把 qemu + probe-rs 都装上 | **一个都不装** |
| 消费者 `mcpp run`(默认 feature) | 同上 | 只装 qemu |
| 消费者 `mcpp run`(`hardware`) | 同上 | 只装 probe-rs |
| 板级包作者写几处 | 清单 + 描述符**两处** | **一处** |

### 12.6 ⚠️ 实施时会撞到的三条

1. **安装的 stamp 按「列表」计**(`prepare.cppm` 的注释说的)。分档后同一个工程会
   在不同命令下要求不同的子集,stamp 必须按 **(子集, 用途)** 计,否则
   `mcpp build` 之后的 `mcpp run` 会认为「装过了」而跳过 run 档。
2. **交叉目标的 sysroot 是 mcpp 自己追加进 deps 列表的**,不是作者声明的 —— 它按
   性质属于 `build`,且必须不受 `when` 影响(现有注释已说明它不该被 provisioning
   改变行为)。
3. **`xlingsDepBinDirs`(查找)必须包含 run 档**,否则 runner 找不到刚装的工具。
   本轮已把查找扩到全图,分档时要确认两者的集合定义一致。

### 12.7 与描述符的关系

`xpm.<平台>.deps` 仍然存在,但**不再是板级包作者必须记得的第二处**:清单声明即安装。
描述符保留给「这个 xim 包自身的安装期依赖」这一层,那是 xim 的事,不是 mcpp 工程的事。

---

## 13. 仍待定

1. `when` 的取值是否要第四个(例如 `test` 与 `run` 分开)。我倾向不要 —— `mcpp test`
   要跑产物,与 `run` 是同一个需求。
2. `dev` 档是否值得做。它是唯一一个**不传播**的档,语义最重而用例最少;可以先只做
   `build`/`run`,`dev` 留到有人要。
3. `[feature-xlings.<feature>]` 这个拼法要不要与 `[feature-deps]` 完全对齐(后者的
   键是包名,这里的键是 xim 地址)。

---

## 14. 五个问题的解法

整体审查提出的五条,逐条给形状。⭐ 其中第一条的解法把一条「发现」变成了**接口的一次
最小补全**,而不是一个待议事项。

### 14.1 ⭐⭐ openarch 缺的不是第五个组,是 trap 组里缺一个动作

我原先把它记成「可能需要第五个接口组,待议」。审查后这个判断太松:**抢占是 MCU 上用
openarch 的主要理由**,表达不了它,openarch-on-Cortex-M 就只有一半用处。

看四台机器要做的事:

| | 抢占时改什么 |
|---|---|
| Cortex-M | 换 `PSP`,以 `EXC_RETURN` 返回 |
| riscv64 | 改 `sepc`,换陷入桩将要恢复的寄存器区 |
| aarch64 | 改 `ELR_EL1` 与 `SP_EL0` |
| x86_64 | 改中断帧的 `RIP`/`RSP` |

**同一个动作,四种写法** —— 这正是这一层存在的理由。而 openarch 的 trap 组今天只能
**观察**陷入(`set_handler`)和**屏蔽**中断,不能**作用于**陷入。

⇒ 解法是一个函数,不是一个组:

```c
/* 协作式:立刻切换,别人切回来时返回。            (已有) */
void arch_context_switch(void* from, void* to);

/* 抢占式:让"这一次陷入"返回到 `to` 而不是被中断的那个上下文;
   被中断的存进 `from`。它正常返回给处理程序,切换在异常退出时发生。 */
void arch_trap_switch(arch_trap_frame* f, void* from, void* to);
```

⭐ **两者形状相同,差别只在"什么时候生效"。** 前者是「现在切」,后者是「返回时切」。

能力名 `openarch:preemption`,由后端声明。Cortex-M **能**实现它 —— `examples/preempt`
里那段手写的 PendSV 汇编就是它的实现。⇒ **那段汇编从示例移进后端**,示例退回成
四十行纯调度策略、零汇编,这正是这一层该有的样子。

⚠️ 仍然要**不止一台机器**才能定稿(riscv64/aarch64 各写一次才知道签名对不对),
所以顺序是:先在 Cortex-M 与 aarch32 上各实现一次,再冻结签名。

### 14.2 ⭐ 生态闭环不是两批,是一个包多一个 feature

我原先把「零 libc 档」与「带 libc 档」排成前后两批,等于把一个**现在就能交**的东西
压在一个大工程后面。

而 mcpp 早就有这个轴:`sysroot = ""` 是零 libc 档(`docs/13` 的原词)。所以:

```toml
# cortex-m-rt
[features]
default  = ["emulator"]
emulator = {}
hardware = {}
libc     = {}          # ← 不选就是零 libc 档

[feature-deps.libc]
picolibc = "1.8.12"
```

⇒ **`cortex-m-rt` 现在就能发**(零 libc,自带启动与向量表 —— `examples/preempt`
已证明这条路通);`picolibc` 落地后,用户加一个 feature,包加一条 `feature-deps`。

⭐ **与模拟器/真机是同一个机制的第二次使用。** 不是新概念。

### 14.3 发现性:放进已有的两个地方,不新增噪声

「引擎不认识任何名字」的代价是新用户不知道 `--runner` 存在。三处补,零新概念:

1. **`mcpp explain`** —— 它已经是「告诉我解析出了什么」,加一节「本工程提供的 runner」
   是零新概念。`--list-runners` 与它共用一个读点。
2. **`mcpp run --help`** 一行:*"see --list-runners for what this project supplies"*。
3. **模板的 README** —— 模板随包走,板级包最清楚自己有什么。

⚠️ **不做**「首次运行时提示一次」:它需要一个 stamp,而这个仓库记过
「提示出现一次就消失」的教训 —— 缓存命中时不重跑,最需要它的那次反而不打印。

### 14.4 ⭐⭐ 工具分档的判据:测「要装什么」,不测「装成了没有」

我原先说这块判据最薄弱,因为验证需要真实 xim 安装,而 e2e 里没有干净环境。

**那是把判据施加在了错误的对象上。** 被断言的是**mcpp 请求安装的集合**,不是安装
成功。而请求是可观察的:`MCPP_NO_AUTO_INSTALL`(或 `--offline`)下,provisioning
不装并**点名它本来要装什么**(`prepare.cppm:3339`)。

⇒ 判据:

```
MCPP_NO_AUTO_INSTALL=1 mcpp build    的输出里 不得 出现 run 档的工具
MCPP_NO_AUTO_INSTALL=1 mcpp run      的输出里 必须 出现 run 档的工具
```

**一次下载都不需要,而且判据带分母**(两条命令的差集正是被测的性质)。

### 14.5 顺序:aarch32 独立成批,并说明它为什么仍值得

我做的顺序与自己的建议相反(先 Cortex-M 后 aarch32),理由成立 —— Cortex-M 才有真实
应用。但 aarch32 那条发现**还没拿到**:它是第一台 **32 位**机器,会挖出
`arch_pte_make_leaf` 返回 `arch_u64` 这条从没被问过的宽度假设。

⇒ 它不因为被推后而失去价值,而且 §14.1 的 `arch_trap_switch` **恰好需要它** ——
签名要在两台不同机器上各实现一次才能冻结。**两件事合成一批。**

---

## 15. 已定的四条(2026-09-04 review)

| # | 决定 | 出处 |
|---|---|---|
| 1 | **`arch_trap_switch`** —— openarch 缺的不是第五个组,是 trap 组缺一个动作 | §14.1 |
| 2 | **`cortex-m-rt` 用 `libc` feature 分档**,零 libc 档现在就能发;**picolibc 并行开工**,不排在它后面 | §14.2 |
| 3 | **分档判据用 `MCPP_NO_AUTO_INSTALL`** —— 测「要装什么」,零下载 | §14.4 |
| 4 | **批 1" 的反向发布顺序** —— 板级包必须等引擎发布 | §15.1 |

⭐ 并且:**批 1 的引擎接口(`--runner`、`[target.X.runners]`、`mcpp:runner-named=`、
`mcpp::runner(name, tok)`)一经发布即成兼容契约。** 本轮之后改它们要付迁移代价,
所以形状在批 1 合入前定稿 —— 已定稿。

---

## 16. 批次(每仓一个 PR,并行的用同一列)

```
       ┌─ 批 1  mcpp #551 ──────────────────────────────────┐  36/36 绿
       │    具名 runner · emit sbom · --locked · 裸名跨全图  │  ← 解锁其余一切
       └────────────────────┬───────────────────────────────┘
                            │ 必须先发布 2026.9.4.2
        ┌───────────────────┼───────────────────┬────────────────────┐
        ▼                   ▼                   ▼                    ▼
   批 1' openarch      批 1" 两个板级包     批 2 mcpp+既有包      批 5 mcpp-index
   部分后端+preempt    裸名简化            [xlings] 工具分档      picolibc +
   (已提交未推)        ⚠️ 等引擎发布        (when/feature-xlings)  builtins
        │                                                            │
        ▼                                                            │
   批 4 openarch                          批 3 新仓 ◄────────────────┘
   arch_trap_switch + aarch32             cortex-m-rt
   两机各实现一次后冻结签名                零 libc 档先发,
        │                                  picolibc 到位后加 libc feature
        ▼
   批 6 xim-pkgindex  probe-rs → 真机路径可落地
```

### 16.1 ⚠️ 两条发布顺序,方向相反

1. **批 1"(板级包)必须等引擎发布。** 裸名写法依赖批 1 的「查找跨全图」;先发板级包
   会让老引擎上那两个包直接坏掉。—— 「消费者先发布」的**镜像**。
2. **批 5(picolibc)必须先于批 3 引用它的那一版发布。** `cortex-m-rt` 要写
   `[feature-deps.libc] picolibc = "…"`,而按版本引用一个未发布的包解析不到。
   —— 这是「消费者先发布」的**正例**。

⇒ 所以批 3 分两版:**`0.1.0` 零 libc 档,不等任何人**;`0.2.0` 加 `libc` feature,
等批 5。⭐ 这正是 §14.2 那个 feature 分档买到的东西:**近档不被远档挡住。**

### 16.2 并行关系

* 批 2、批 5 与批 1'/1" **互不依赖**,可同时进行。
* 批 4 依赖批 1'(要有 Cortex-M 后端才能在上面实现 `arch_trap_switch`)。
* 批 6 独立,任何时候都能做;它只在批 3 的 `hardware` feature 要真跑时才成为阻塞。
