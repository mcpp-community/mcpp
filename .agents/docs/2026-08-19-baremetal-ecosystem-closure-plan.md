# 裸机 / freestanding — 全生态打通实施计划(第二阶段)

- Date: 2026-08-19
- Status: **实施计划,待 review**
- 上游:`2026-08-19-freestanding-baremetal-implementation-plan.md`(第一阶段;**基础依赖侧已完成**)
- 方案:`2026-08-19-freestanding-baremetal-design.md` · 证据:`2026-08-18-freestanding-baremetal-analysis.md`
- 范围:**把链路从「包装好了」推到「用户能用」** —— 跨 `xim-pkgindex` / `mcpp` / `mcpplibs` 三仓

---

## 0. 现在在哪(实测基线,不是推测)

第一阶段交付的**基础依赖侧**已在已发布索引里,并本地真跑通:

| 已有 | 状态 |
|---|---|
| `xim:qemu-riscv@9.2.4-1` | ✅ 五宿主目标,镜像双端 |
| `xim:picolibc-riscv@1.8.12` | ✅ 两档位 `rv64gc/lp64d` + `rv32imac/ilp32`,含 builtins |
| 裸机内核(C++20 模块 → `ld.lld` → qemu) | ✅ 零未定义符号、无 PT_INTERP、真启动 |
| picolibc 程序(`printf` 浮点 + `malloc`) | ✅ semihosting 真跑 |

**但这一切今天都要手写十几个 flag。** 用户侧一行都还没打通 —— 这就是本计划要消灭的距离。

### 0.1 ⭐ 三条决定本计划形状的实测

1. **接缝已经可表达,不需要新轴。** `build.mcpp` 拿得到 `MCPP_TARGET` / `MCPP_TARGET_OS` /
   `MCPP_TARGET_ARCH` / `MCPP_TARGET_ENV`(`hostprogram.cppm:155-158`),并能发
   `include_dir()` / `link_search()` / `link_lib()` / `cflag()` / `cxxflag()`。
   ⇒ **BSP 包零引擎改动就能把 xlings 的 sysroot 接进编译链接。**

2. **它表达不了的恰好只有两件事** —— 而这两件正是引擎必须补的:
   - **链接线的顺序**:`crt0.o` 必须排在用户对象**之前**,`link_search`/`link_lib` 表达不了序;
   - **运行**:`build.mcpp` 是构建期程序,`mcpp run` 需要的 argv 模板它够不到。

3. **现有 provision 只有三种**(`build/provisions.cppm:79`):`tool` · `host-module` · `dep-dir`。
   ⇒ W8 要加的 `linker-script` / `startup-objects` / `runner` 是**真新语义**,不是重复造轮子。

---

## 1. 「打通」的定义:一条可执行的验收链

⭐ **本计划的唯一总判据 —— 用户写这些,然后跑这三条命令:**

```toml
# mcpp.toml
[package]
name = "hello-mcu"
version = "0.1.0"

[build]
target = "riscv64-none-elf"

[dependencies]
riscv-virt-rt              = "0.1"   # BSP:链接脚本 + 启动 + runner
mcpplibs.std.freestanding  = "0.1"   # std 子集
```

```bash
mcpp build --target riscv64-none-elf
mcpp run   --target riscv64-none-elf     # qemu 里打印出模块的输出
mcpp test  --target riscv64-none-elf     # 3 个用例,一次 qemu 全过
```

⚠️ **判据的严格形式**:上面这份工程里,**用户看不到也不需要知道**下列任何一个词 ——
`picolibc`、`compiler-rt`、`libclang_rt.builtins`、`crt0-semihost.o`、`picolibcpp.ld`、
`-nostdlib`、`-mcmodel=medany`、`-machine virt`、`-bios none`、`-semihosting`。

**它们全部由 BSP 包 + 引擎承担。** 只要用户还得自己写其中任何一个,这条线就没打通。

### 1.1 三个否定判据(防假绿)

| # | 不算打通 |
|---|---|
| N1 | 「能构建」但 `mcpp run` 要用户自己拼 qemu 命令 |
| N2 | 「能跑」但依赖用户在 `mcpp.toml` 里手写 `link-search` / `cflag` |
| N3 | 「都能跑」但换一块板(`rv32imac/ilp32`)要改引擎而不是换 BSP 依赖 |

---

## 2. 三仓责任划分与接缝

| 仓库 | 负责 | **绝不负责** |
|---|---|---|
| `openxlings/xim-pkgindex` | 工具链载荷、模拟器、**目标 sysroot**(已完成) | 不认识 target triple、不认识 BSP |
| `mcpp-community/mcpp` | target 模型 · 链接模式 · provision · runner · test harness | ⚠️ **永不认识 picolibc / 某块板 / 某个 qemu 参数** |
| `mcpplibs/*` + `mcpplibs/mcpp-index` | BSP(板级)· std 子集 · 未来的 HAL | 不改引擎 |

### 2.1 ⭐ 接缝的形状(已实测可行)

```
xim:picolibc-riscv          (xlings 包,目标 sysroot)
        ▲
        │ [xlings] deps  +  build.mcpp 读 MCPP_TARGET_* 后
        │ 发 include_dir / link_search / link_lib / cflag
        │
riscv-virt-rt               (mcpp 包:BSP)
        │
        │ 依赖边 + provision(linker-script / startup-objects / runner)
        ▼
   用户工程                  (只写 [dependencies])
```

⚠️ **BSP 是唯一知道「板」的地方**:内存布局、UART 地址、qemu 机器型号、
`<march>/<mabi>` 档位选择,全部在这一层。换板 = 换一个依赖。

### 2.2 ⚠️ 接缝上必须先验证的三件事(Phase 0)

读代码得出的结论这一轮被实测推翻过多次,所以每条都先做探针:

| 探针 | 问题 | 为什么必须先做 |
|---|---|---|
| **Z1** | BSP 的 `build.mcpp` 真能把 sysroot 的 include/lib 接进消费者的编译链接吗? | 若不能,Phase C 整个形状要改 |
| **Z2** | `build.mcpp` 在**交叉 target** 下真跑得起来吗(它是**宿主**程序) | 宿主程序 + 交叉工程是两套 flag,可能撞车 |
| **Z3** | provision 从 BSP 传到用户工程,经过几条边?re-export 需要吗? | 决定 W8 的传播语义 |

⭐ **Z1 与 Z2 在 hosted target 上就能验**(拿一个假 sysroot 目录),不依赖任何引擎改动
⇒ **今天就能开工,且失败代价只有一个探针。**

---

## 3. 工作单元

### 3.1 Phase 0 — 探针先行(3 个,零依赖,最先做)

| ID | 内容 | ⚠️ 验收判据(可执行) | 规模 |
|---|---|---|---|
| **Z1** | BSP-seam 探针:一个 mcpp 包,`build.mcpp` 读 `MCPP_TARGET_ARCH` 后发 `include_dir`/`link_search`/`link_lib`,消费者在 **hosted** 目标上构建 | 消费者的 `compile_commands.json` 里出现该 include dir,且 `build.ninja` 的链接线含该 `-L`/`-l`;⚠️ **换一个 `MCPP_TARGET_ARCH` 值 ⇒ 发出的目录随之改变**(否则是常量不是接缝) | S |
| **Z2** | build.mcpp 在交叉 target 下的行为普查:它用哪套 flag 编译、`MCPP_TARGET_*` 在 `--target riscv64-none-elf` 下取到什么值 | 逐项列出;⚠️ **若 build.mcpp 自身被交叉编译则是硬缺陷**,必须记为 W3 的一条 | S |
| **Z3** | provision 传播探针:`dep-dir` 从间接依赖到用户工程走不走得通 | 三层依赖链上取到 provision;⚠️ 若需 re-export,W8 的三种新 kind 各自的 `reexportOnly` 列就此定死 | S |

### 3.2 Phase A — 引擎:能表达、能构建(承接第一阶段的 W0–W6/W13)

| ID | 内容 | 依赖 | ⚠️ 验收判据 | 规模 |
|---|---|---|---|---|
| **W0** | 修 E1 静默失败:门的判据改为「这次构建为哪个 target 发了 `--target`」 | — | 无裸机配置时**指名道姓报错**;**不存在「成功且产物是 x86-64」的路径** | S |
| **W1** | `triple` 收 `os=none` | W0 | `riscv64-none-elf` 解析成功;⚠️ `x86_64-none-linux-gnu` **仍解析为 linux** | S |
| **W2** | `TargetSpec` 表 + **单一取值口** | W1 | 消费方逐个核对**无第二处推导**;`kKnownTargets` 含 `riscv64-none-elf` / `riscv32-none-elf` | M |
| **W3** | 探测代码普查(doctor / 工具链能力 / `hasImportStd`)在 `os=none` 下的行为 | Z2 | 逐点列清单并修掉会假失败的 | S–M |
| **W4** | `CLibMode::Freestanding`:`-nostdlib -nostartfiles -static -T`;⚠️ **链接器按载荷绝对路径寻址 + 校验 `--version` 含 `LLD`** | W2,W3 | 产物 `file` 含 `UCB RISC-V`;`readelf -l` **无 PT_INTERP**;`llvm-nm -u` **空**;⚠️ **验收工程必须用 picolibc `printf` 打一个浮点数**(见 §6-R2) | M |
| **W5** | BMI 缓存键 + 指纹纳入 `TargetSpec` 全字段 | W2 | 同源码 hosted / freestanding 下 BMI **互不复用** | S |
| **W6** | `import std` 关断 + 指向 `mcpplibs.std.freestanding` 的诊断 | W1 | `os=none` 下给出 mcpp 自己的诊断,**不是 libstdc++ 内部报错** | S |
| **W7a** | `cfg(freestanding)` / `cfg(os="none")` 谓词 | W1 | `[target.'cfg(freestanding)'.build]` 在裸机下生效、hosted 下不生效 | S |
| **W13** | 产物形态:`.elf` + `.bin` + `.map` + **size 摘要**默认 | W4 | 四件齐备,构建后打印 text/data/bss | S |

### 3.3 Phase B — 引擎:能跑、能测

| ID | 内容 | 依赖 | ⚠️ 验收判据 | 规模 |
|---|---|---|---|---|
| **W12** | e2e harness `# requires-hard:`:能力缺失 **FAIL 而非 SKIP**;CI 汇总**实际执行条数** | — | 故意不装 qemu ⇒ 裸机 e2e **job 红,不是绿着跳过** | S |
| **W8** | provision 三种新 kind:`linker-script`(单值,冲突报错)· `startup-objects`(**两个具名槽 `prologue`/`epilogue`,槽内数组序**)· `runner` | W4,Z3 | BSP 提供的 `crt0` 按 `prologue → 用户对象 → epilogue` 进链接线;⚠️ **两个 BSP 同时提供 = 硬错误** | M |
| **W9** | `mcpp run` runner:`[target].runner` argv 模板 + 产物路径追加;**BSP 亦可经 provision 提供** | W2,W8 | `mcpp run --target riscv64-none-elf` 在 qemu 里打印出预期串;⚠️ **两种机器形态都要过**(`-bios default -kernel`(OpenSBI)与 `-bios none -semihosting`(picolibc)) | S–M |
| **W11** | `mcpp test` 裸机:默认 `batch`(一镜像一次 qemu,semihosting 打用例名)· `isolated` 可选 · **超时后自动重跑一次 isolated 定位** | W9,W12 | 3 用例 batch 一次全过;人为让第 2 个跑飞 ⇒ **超时后指出是第 2 个** | M |
| **W16** | `--target` + `--workspace` 冲突即报错 | — | 混合 workspace 上**报错说明冲突**,不静默覆盖 | S |
| **W15** | CDB 合并到 workspace 根 | — | 根上一份 CDB,每个文件带所属成员 target 的 flags | S |

### 3.4 Phase C — 生态库(mcpplibs)

| ID | 内容 | 依赖 | ⚠️ 验收判据 | 规模 |
|---|---|---|---|---|
| **E-BSP** | `mcpplibs/riscv-virt-rt` — qemu `virt` 的 BSP:`[xlings] deps = ["xim:picolibc-riscv"]`,`build.mcpp` 按 `MCPP_TARGET_ARCH` 选档位并接 sysroot;提供 `linker-script` / `startup-objects` / `runner` | Z1,W8,W9 | §1 的三条命令在**只声明这一个依赖**时跑通;⚠️ **换成 rv32 档只改 BSP 的一个值,引擎零改动**(N3) | M |
| **E-STD-1** | `mcpplibs/std-freestanding` **S-1 零 libc 档**:8 个头 + 基础类型 | — | `import mcpplibs.std.freestanding;` + `std::to_chars` + `std::span` 在裸机链接**零未定义符号** | M |
| **E-STD-2** | **S-2**:52 头可移植子集(接 picolibc 头) | E-STD-1,E-BSP | 方案 §7.1 的三文件工程跑通(`array`/`ranges::sort`/`optional`/`atomic`/`string_view`) | M |
| **E-IDX** | 三个包进 `mcpplibs/mcpp-index`(`pkgs/<字母>/<name>.lua`) | E-BSP,E-STD-1 | `mcpp add riscv-virt-rt` 从**已发布索引**解析成功 | S |

⚠️ **E-STD-1 的依赖关系已被第一阶段改变**:原计划说它需要「BSP 提供 `memcpy/memmove/memset/memcmp`」——
**有了 picolibc 之后这四个来自 `libc.a`**。零 libc 档只在**不依赖 BSP**时才需要自带,
⇒ S-1 的定位从「最小可发布」变成「**不想要 libc 的人的选项**」,而主线是 S-2。

### 3.5 Phase D — 使用侧闭环

| ID | 内容 | 依赖 | ⚠️ 验收判据 | 规模 |
|---|---|---|---|---|
| **T1** | `mcpp new --template baremetal-riscv` | E-IDX,W9 | 生成的工程**开箱即过** §1 三条命令,manifest 不超过 §1 那五行依赖 | S |
| **D1** | 文档:裸机上手页 + BSP 作者指南(怎么写一个新板) | E-BSP | 照文档能从零写出第二块板的 BSP(⚠️ 判据是**别人写得出来**,不是文档存在) | M |
| **D2** | 诊断文案:缺 BSP / 缺 runner / 缺能力时**指名道姓**并给出下一步命令 | W6,W8 | 三种缺失各有一条测试钉住文案含**具体包名** | S |

### 3.6 Phase E — 三仓联防(⭐ 本计划最容易被跳过、也最要命的一档)

⚠️ **链路跨三仓,而三个仓库的 CI 今天互相看不见。** 任何一侧改动都能悄悄打断它。

| ID | 内容 | ⚠️ 验收判据 | 规模 |
|---|---|---|---|
| **X1** | **唯一权威夹具**:`baremetal-closure` —— §1 的工程 + 三条命令 + 断言,放在一个所有仓都能取的地方 | 夹具本身是一个仓库/目录,**没有第二份拷贝** | S |
| **X2** | 三仓 CI 各自跑 X1:`xim-pkgindex`(改包时)· `mcpp`(改引擎时)· `mcpplibs`(改库时) | 三条流水线都**真跑过一次并红过一次**(人为破坏各自那一侧) | M |
| **X3** | 版本兼容探针:mcpp × sysroot × BSP 的矩阵 | ⚠️ 至少验证 **llvm 大版本跨越**(22 编的 builtins 给 23 用)与 **picolibc 头 × libc++ 子集**两条 | S–M |

⭐ **X2 的判据是「红过一次」而不是「绿了」** —— 这一轮已经三次遇到「CI 全绿但那条路径从没执行」。

---

## 4. 依赖拓扑

```
Phase 0(今天可开,零依赖)
   Z1 seam探针 ── Z2 build.mcpp交叉行为 ── Z3 provision传播
      │              │                        │
      │              ▼                        │
      │           W3 探测普查                 │
      │              │                        │
引擎  ▼              ▼                        ▼
   W0 ─► W1 ─► W2 ─► W4 ─► W8 ─► W9 ─► W11
              │      │      ▲             ▲
              │      │      │             │
              ├─► W5 │      │             W12(必须早于 W11 的验收)
              ├─► W6 │      │
              ├─► W7a│      │
              └──────┴─► W13│
                            │
生态                        │
   E-STD-1 ────────────────┐│
   E-BSP ◄──── Z1 ─────────┴┴──► E-STD-2 ──► E-IDX
      │                                        │
使用侧                                         ▼
                                            T1 ─► D1 ─► D2
联防
   X1 夹具 ──► X2 三仓 CI ──► X3 兼容矩阵
```

**关键路径 = `Z1 → W0 → W1 → W2 → W4 → W8 → W9 → E-BSP → T1`(9 个)。**
其余 **15 个可并行**,其中 **7 个(Z1/Z2/Z3/W0/W12/W15/W16/E-STD-1)零依赖,今天就能开**。

---

## 5. 里程碑与验收

| 里程碑 | 含 | ⚠️ 验收判据(必须是这句) |
|---|---|---|
| **M-0 接缝可信** | Z1 · Z2 · Z3 · W0 · W12 | ① Z1 里**改 target 值 ⇒ 发出的目录随之改变**;② 故意不装 qemu ⇒ 裸机 e2e **job 红** |
| **M-1 能表达** | W1 · W2 · W3 | `--target riscv64-none-elf` 解析成功;无配置时**指名道姓报错**;探测点清单已产出 |
| **M-2 能构建** | W4 · W5 · W6 · W7a · W13 | `file` 含 `UCB RISC-V` · 无 PT_INTERP · `llvm-nm -u` 空;⚠️ **验收工程用 picolibc `printf` 打浮点**且**至少一个依赖** |
| **M-3 能跑** | W8 · W9 · E-BSP | `mcpp run --target riscv64-none-elf` 打印出预期串;⚠️ 判据是**裸机 e2e 实际执行条数 > 0** |
| **M-4 能测** | W11 | 3 用例 batch 一次全过;第 2 个跑飞 ⇒ **超时后指出是第 2 个** |
| **M-5 生态可取** | E-STD-2 · E-IDX | `mcpp add riscv-virt-rt` 从**已发布索引**解析;三文件工程跑通 |
| **M-6 ⭐ 打通** | T1 · D1 · D2 | ⭐ **§1 那五行依赖 + 三条命令,在一台干净机器上从零跑通,用户没见过 §1 列出的任何一个词** |
| **M-7 不再退化** | X1 · X2 · X3 | 三仓 CI 各自**红过一次**;兼容矩阵两条已验 |

---

## 6. ⚠️ 风险(每条都有实测出处)

| # | 风险 | 出处 | 防线 |
|---|---|---|---|
| **R1** | **CI 绿着但那条路径从没执行** | 本轮三次 | W12 前置;判据是**执行条数 > 0**;X2 判据是**红过一次** |
| **R2** | **builtins 缺口的假绿**:`rv64gc` 有硬件 `divu`,64 位除法**碰不到** builtins | 第一阶段实测 | M-2 验收工程**必须 `printf` 一个浮点数**(那是唯一会引 `__ashlti3`/`__lshrti3` 的日常操作) |
| **R3** | **`ld.lld` 按名字解析到宿主 GNU ld** | 第一阶段独立复现(`unrecognised emulation mode: elf64lriscv`) | W4 强制**绝对路径** + `--version` 校验 |
| **R4** | **默认 cfg 毒化交叉编译**:llvm 载荷的 `clang.cfg` 无条件注入宿主 glibc 头 + 写死 x86-64 动态链接器 | 第一阶段实测 | ✅ **mcpp 已经躲开**(`flags.cppm:495`)—— 但 **W3 必须逐点确认探测路径也躲开了** |
| **R5** | **单包验证会得出错误结论**(flags 作用域,加第二个包才碎) | 方案 §4.1 | M-2 验收工程**至少一个依赖** |
| **R6** | **BMI flag 不一致 ⇒ 编译器崩**(不只是结果不对) | ISO 系列 | W5 与 W4 同期 |
| **R7** | **`thread_local` 静默写坏内存**(编过链过零诊断) | TH6 | 裸机验收用例**必须含一个 `thread_local`** |
| **R8** | **带序 provision 被顺手做成集合** | — | W8 判据显式检查链接线**顺序** |
| **R9** | **新门被逃生舱绕开**(E1 的原始形状) | E1 | 每个新门自问「**逃生舱走不走这里**」并写测试 |
| **R10** | **读代码下结论**(本轮又被实测推翻多次) | 全程 | 每个单元判据是**命令 + 期望输出** |
| **R11** | ⚠️ **三仓改动互不可见** —— 任一侧都能悄悄打断链路 | 结构性 | Phase E 是**必需配套**,不是可选项 |
| **R12** | **`build.mcpp` 是宿主程序,工程是交叉的** —— 两套 flag 可能撞车 | 未验证 | **Z2 先探**;若 build.mcpp 被交叉编译则是硬缺陷 |
| **R13** | ⚠️ **文档写了但别人写不出第二块板** | — | D1 的判据是**别人真写出来**,不是文档存在 |

---

## 7. 版本兼容与无感升级(X3 展开)

| 轴 | 问题 | 现状 | 探针 |
|---|---|---|---|
| **llvm 大版本** | sysroot 的 builtins 用 22.1.8 编,给 23 的 clang 用行不行? | builtins 是 ABI 稳定的运行时函数,**理论上行** | ⚠️ 理论不算 —— X3 必须真用 23 链一次 |
| **picolibc 头 × libc++ 子集** | libc++ 的 freestanding 头依赖 libc 头的哪些部分? | S-2 已验 52 头交集 | 换 picolibc 小版本后重跑 |
| **BSP × 引擎** | 新增 provision kind 后旧 BSP 还能用吗? | 未发生 | ⚠️ **provision 表加行必须向后兼容**:旧包不声明新 kind ⇒ 沿用默认,不是失败 |
| **sysroot 升级** | picolibc 1.9 出来后怎么升? | 需**重新构建**(包刻意不设 `ci.update`) | 脚本已在 `.agents/tools/`,判据是同输入同字节 |

⚠️ **一条硬约束**:`xim:picolibc-riscv` 的版本号是 picolibc 的,但内含 builtins 的 llvm 版本
**只写在 `BUILDINFO` 里**。⇒ X3 必须能回答「这台机器上的 sysroot 是哪个 llvm 编的」,
否则 llvm 升级后的问题无法定位。

---

## 8. 不在本计划内

| 项 | 去向 |
|---|---|
| **P4** libc++ 每目标 freestanding `__config_site` | E-STD-2 内部按需;不单独立项 |
| **E-STD S-3**(`format` / `sort` / `string` 全功能) | 需为目标编 libc++;M-6 之后再评估 |
| **ARM / 其它 arch** | ⚠️ 判据是 **N3**:换 arch 应当只是加一个 BSP + 一个 sysroot 包;**若需改引擎,说明 W2 的 TargetSpec 抽象错了** |
| **D 档**(openkal / openhal / openarch) | 路线图在第一阶段计划 §7,**不并入本计划里程碑** |
| **烧录 / OTA** | 设备侧的事,mcpp 不碰 |

---

## 9. 为什么这样排

1. **Phase 0 先行**,是因为这一轮「读代码下结论」被实测推翻了不止一次,而接缝是本计划里
   **唯一还没有任何实测支撑**的部分。三个探针加起来不到一天,买的是整个 Phase C 的形状。
2. **E-STD-1 从「最小证明」降级为「可选档」**:第一阶段把 picolibc 做出来之后,
   `memcpy` 那四个函数来自 `libc.a`,S-1 不再是通往 S-2 的必经之路。**主线是 S-2。**
3. **Phase E 与 Phase A/B/C 并行而不是排在最后**:X1 的夹具就是 M-2/M-3/M-4 的验收载体,
   先有夹具,后面每个里程碑都直接用它,而不是各写一份。
4. ⭐ **M-6 的判据是「用户没见过那些词」** —— 这是唯一能把「能跑」和「打通」区分开的判据。
   任何以「我们能构建裸机产物了」结束的版本,都还停在 M-2。
