# PR #455–#459 深度 review:裸机 freestanding 从「能编」到「能用」

**范围**:`mcpp-community/mcpp` #455 · #456 · #457 · #458 · #459,以及同期的
`openxlings/xim-pkgindex` #651/#652/#653(+ 三条版本 bump)、`mcpplibs/mcpp-index`
#219/#220、两个新建仓库 `mcpplibs/riscv-virt-rt` 与 `mcpplibs/std-freestanding`。

**时间**:2026-08-19 一日之内(#455 合入 → #459 合入),发布 `2026.8.19.1` … `2026.8.19.4`。

⚠️ **本文的定位是 review 而不是总结**:凡是「做了什么」都给出可复核的出处(PR 号、
文件、命令),凡是「做错了什么」都写明**发现方式**与**当时为什么没看见**。
一日四个补丁版本本身就是一个信号,第 6 节专门分析它。

---

## 0. 一句话结论

引擎侧的形状是对的,而且**收敛得比计划更小**;真正的成本几乎全在
**「我把属于 target 的东西写成了包的依赖」** 这一类边界判断上 ——
五个已修缺陷里有三个是同一个根因的不同表现。

判据(用**已发布**二进制、从零验证):

```bash
mcpp new blinky --template riscv-virt-rt
cd blinky && mcpp run          # qemu 里打印;manifest 里没有 [target.*] 段
```

```toml
[package] name = "blinky" / version = "0.1.0"
[build]   target = "riscv64-none-elf"
[dependencies] riscv-virt-rt = "0.3.0"
```

---

## 1. 五个 PR 各做了什么

| PR | 规模 | 内容 | 版本 |
|---|---|---|---|
| **#455** | +6355/−18,34 文件 | 裸机 target 成立:`riscv64/32-none-elf` 进目标表、新建 `src/freestanding/`(`target`/`linkline`/`runner`)、`mcpp:link-script=` 指令(protocol 3)、`mcpp::xpkg_dir` | 2026.8.19.1 |
| **#456** | +2/−2 | 发布 | — |
| **#457** | +1388/−49,24 文件 | **runner 归 BSP**(`mcpp:runner=`,新 `Slot::Runner` + `Scope::RunGlobal`,protocol 4)、裸机 `mcpp test`、产物集(`.bin`/`.map`/size 摘要) | 2026.8.19.2 |
| **#458** | +109/−3,5 文件 | 修 `mcpp build` 之后 `mcpp run` 在宿主上直接 exec 裸机 ELF | 2026.8.19.3 |
| **#459** | +753/−45,21 文件 | 异常/RTTI 整图关闭、**目标自带 sysroot 轴**、`toolchain_dir`/`sysroot_dir`、依赖缓存键补 target flags、std 子集打通 | 2026.8.19.4 |

累计(从 #455 之前的 `9c5cc8d` 算起,含同期其它工作):**87 文件 / +10330 / −161**。
其中 `src/freestanding/` 三个模块共 **432 行**,裸机 e2e 四个脚本共 **983 行**,
`test_freestanding.cpp` **25 个单测**。

**指令表 13 → 15 行,protocol 2 → 4。** 两条新指令(`link-script`、`runner`)各自
只占表里一行 —— 这是 #455/#457 最值得记的一点,见 §2.1。

### 生态侧

| 仓库 | 内容 |
|---|---|
| `openxlings/xim-pkgindex` | #651 `qemu-riscv 9.2.4-1`(五个宿主目标)· #653 `picolibc-riscv 1.8.12`(自建,`rv64gc/lp64d` + `rv32imac/ilp32`)· #652 修镜像工具两个缺陷 · #654/#655/#657 版本 bump |
| `mcpplibs/mcpp-index` | #219 收录 `riscv-virt-rt`(已合)· #220 `std-freestanding 0.2.0` + `riscv-virt-rt 0.3.0`(待 2026.8.19.4 生效) |
| `mcpplibs/riscv-virt-rt` | 板级支持包,0.1.0 → **0.3.0**;含 `templates/blinky`(`mcpp new --template`)、examples、仓库 CI(rv64+rv32 真跑 qemu) |
| `mcpplibs/std-freestanding` | freestanding std 子集,0.1.0 → **0.2.0**;`tools/regenerate.sh` 实测生成 |

两个包 GitHub + GitCode 双镜像,tarball **逐字节校验一致**。

---

## 2. 架构评估

### 2.1 ⭐ 最好的一条:能力增长没有换来概念增长

裸机需要三样东西是引擎原本表达不了的:**链接脚本**、**执行方式**、**目标 C 库位置**。
三样落地为:

- `link-script` —— 指令表**一行**(`Scope::LinkGlobal`,`Transform::LinkerScript`);
- `runner` —— 指令表**一行** + 一个新 `Slot` + 一个新 `Scope`;
- 目标 sysroot —— `TargetInfo` **一个字段**,与 `pin` 并列。

⚠️ **计划里原本有更大的方案,是实测把它们砍掉的**:

| 计划写的 | 实测 | 结果 |
|---|---|---|
| W8「带序 startup-objects 具名槽」 | `-lcrt0-semihost` 从**归档**里拉得进来,顺序由链接脚本 section 序决定 | ⛔ 整个槽位机制不需要,真正缺的只有 `-T` |
| §4「裸机 `mcpp test` 要 batch 模式 + 结构化 stdout 协议」 | qemu 冷启动 **12ms**(不是计划写的 0.4s);semihosting **把固件 `main` 返回值原样传给 qemu 退出码** | ⛔ 六个设计决定里四个不必做,R3 缩成「走 `mcpp run` 用的同一个 runner」 |
| W12 `requires-hard` 能力硬门 | 同一个 token 要同时表达「平台本来没有」与「runner 配错了」 | ⛔ 实现后用了一次,macOS e2e **一小时内证伪**,整个机制撤销 |

⇒ **这三条是本轮质量的主要来源**:计划里凡是带具体数字或「A 不成立所以要 B」的句子,
数字与断言本身就是必须先测的探针。

### 2.2 ⭐ 作用域的不对称是刻意的,而且被两侧钉住

`directives.cppm` 里 `link-search`/`link-lib`/`link-script` 是 **LinkGlobal**(到达消费者),
`include-dir`/`cflag`/`cfg` 是 **PackagePrivate**(不到达)。这条供应链纪律让
**引擎完全不需要 sysroot 概念**就能让 BSP 工作:目标头由包私有 include、对外只 export 一个模块。

e2e/131 从**两侧**钉它:目标 C 头**必须**到达消费者,板级包**自己的**头**必须不**到达。

### 2.3 ⚠️ 最大的架构错误:把 target 的属性写成了包的依赖

`riscv-virt-rt` 与 `std-freestanding` 都曾声明
`[xlings] deps = ["xim:picolibc-riscv@1.8.12"]`,后者还声明了 `xim:llvm`。
**这是用户 review 时指出的**,三条都对:不该绑 libc、不该绑架构、不该绑编译器实现。

真因是**引擎的结构性缺口**,不是包写得随意:

| | 编译器 | 目标 libc |
|---|---|---|
| hosted(`x86_64-linux-musl`) | 目标表 `pin` **自动** | musl 在 gcc 载荷里 / glibc 走 `PayloadPaths` —— **自动**,从没人写过 `xim:glibc` |
| freestanding | 目标表 `pin = llvm@22.1.8` **自动** | **一条轴都没有** ⇒ 外溢到每个包 |

⇒ #459 给 `TargetInfo` 加 `sysroot` 轴,复用现有 `[xlings] deps` 物化通道安装,
引擎发 `-isystem <sysroot>/include/<档位>` 与 `-L <sysroot>/lib/<档位>`。
再补两个「问引擎」的接口 `mcpp::toolchain_dir()` / `mcpp::sysroot_dir()`。

**结果**:`std-freestanding` 变成**零依赖**,`riscv-virt-rt` 只剩模拟器(那确实是板级事实)。
分界线也随之清楚了:**位置是目标的事实,选择是板级的事实。**

⚠️ **这个错误能存在两个版本,是因为它在装好的机器上完全不可见** —— 见 §5.2。

### 2.4 单一读取点

`choose_runner(ctx)` 是「产物怎么执行」的**唯一**读取点,`mcpp run` 与 `mcpp test` 共用。
这是本仓库反复付过学费的形状(#233/#240/#242/#344:同一决策两处推导)。
覆盖语义也留了:`[target.X].runner` 覆盖 BSP 供的,调试时换 `-bios` 是正当需求。

---

## 3. 兼容性评估

### 3.1 ⭐ 已发布数据不得让已发布程序失效

索引 `min_mcpp` **没有抬**(memory:`index-floor-must-degrade`)。
新包进索引不会让老客户端拿不到任何包。`riscv-virt-rt` 0.1.0/0.2.0 **留在索引里**,
因为「已经发布过的版本就是可能有人 pin 过的版本」。

### 3.2 ⚠️ 类型化 API 没有语言内的特性探测(实测)

```cpp
if constexpr (requires { mcpp::runner("x"); })   // 名字不存在 ⇒ 硬错误,不是 false
```

`requires` 表达式作用在**不存在的限定名**上是 ill-formed。⇒ 包**无法**优雅降级;
而 wire 协议那条点名 `mcpp self update` 的诊断也够不着(它要求程序**先编译得过**)。

⇒ 补在引擎侧:`build.mcpp` 编译失败且错误含「不是 `mcpp` 的成员」时追加升级提示,
三个前端三种拼写都认。**对以后每一次类型化 API 新增都生效**,不只是 `runner`。

### 3.3 ⚠️ 升级即坏的缓存缺陷(#459 修)

依赖缓存键有 `targetTriple`,**没有 triple 隐含的那组 flag**。而**哪些 flag 由 triple 隐含
是 mcpp 的决定**,会随版本变、triple 字符串不变。

⇒ 加 `-fno-exceptions` 那一刻,所有已建过的裸机工程**复用升级前的 BMI**,硬失败,
而错误只点名一个 `.pcm` 文件:

```
error: exception handling was enabled in precompiled file
'mcpplibs.riscv_virt_rt.pcm' but is currently disabled
```

已加 `targetImpliedFlags` 轴(hosted 为空,不动任何现有键),单测钉「加一个 flag 键必须变」。

### 3.4 ⚠️ 一条指向不存在的包的诊断(自造,已修)

第二阶段的 `import std` 诊断末尾给了可直接粘贴的
`mcpplibs.std.freestanding = "0.1"` —— **这个包当时没发布**。用户粘完下一条命令报
`package not found`。⇒ **诊断里的每条建议都是承诺**;现在包发布了,那一行才放回去。

### 3.5 破坏性变更盘点

| 变更 | 影响面 | 处置 |
|---|---|---|
| `mcpp run` 位置参数改名 `target` → `bin` | **零**(只出现在 `--help`,`cmd_run` 按下标读) | — |
| `mcpp run --target-triple` | 保留为别名(2026.8.19.1 已发布的拼写),e2e/130 两个拼写都钉 | — |
| freestanding 加 `-fno-exceptions -fno-rtti` | 裸机工程全体 | 由 §3.3 的缓存键保证重编;**异常在裸机上本来就不可用**(无 unwinder) |
| 目标自带 sysroot | 老包的 `[xlings] deps` 仍可留(去重),**不破坏** | 新包不必写 |

---

## 4. 简洁与优雅

**好的**:

- `src/freestanding/` 三个模块 **432 行**承载全部裸机知识,且 `linkline` 是**纯字符串构造**
  (不依赖 `CompileFlags`,避免模块环)。
- ISA 表是**数据**:`riscv32` 支持靠表里加一行,引擎与 BSP 零改动 —— 这条被 N3 判据实测过。
- 模板**随包走**:`cmd_new` 的内建注册表刻意冻结在 `bin`,其余是包模板;
  `mcpp new` 把自依赖按**解析到的版本**写进 manifest ⇒ **模板不可能与库脱节**。
  ⚠️ 计划里原本要给 mcpp 加内建 `--template baremetal-riscv`,**是错的形状**。
- std 子集**不手写导出表**:libc++ 自带 110 个 `std/<header>.inc`,子集是机械挑选,
  生成物 217 行全是 `#include`;`tools/regenerate.sh` 每次运行都打印宿主对照组。

**不够好的**:

- ⚠️ `src/build/prepare.cppm` 与 `execute.cppm` 各 +136/+137 行。这两个文件已经很大,
  本轮又往里加了 sysroot 解析、runner 选择、size 报告、快路径前置条件。
  **它们正在变成「什么都知道的地方」**,下一轮应当先拆再加。
- `build.mcpp` 里合成 `__config_site` 是 40 行字符串拼接。可用但不优雅;
  若第二个包也要合成配置,应当抽成 `mcpp::` 的一个接口。

---

## 5. 稳定性评估

### 5.1 已修缺陷清单(全部为本轮自造)

| # | 缺陷 | 发现方式 | ⚠️ 当时为什么没看见 |
|---|---|---|---|
| 1 | `mcpp run <binary>` 被 `--target` 吃掉 | **CI(e2e/73)** | 位置参数上**原本就有注释写着这个碰撞**,我为了「一致性」覆盖了它 |
| 2 | `mcpp build` 后 `mcpp run` 在宿主 exec 裸机 ELF | **发布后**用发布二进制走新用户流程 | 130/131/132 三个 e2e **都恰好先 `run` 后 `build`**,顺序本身是被测对象 |
| 3 | 诊断指向不存在的包 | 自查 | 写诊断时没验证那一行今天能跑通 |
| 4 | 包声明了 target 的 libc/编译器 | **用户 review** | 在已装好的机器上,这条边在与不在**完全一样** |
| 5 | 缓存键缺 target 隐含 flags | 实施 #459 时的升级路径自测 | 只有跨版本才暴露 |

### 5.2 ⚠️ 三条「判据本身是错的」

这是本轮最值得记的一类问题 —— 测试是绿的,但它测的不是那件事。

1. **第一版 build-then-run 回归测试是假绿**:加进 e2e/131 的既有工程后,
   **关掉修复它照样过**。给 `try_fast_run` 的 16 个 `return nullopt` 各打编号探针后定位到:
   `mcpp.toml` 比 `build.ninja` 新,**快路径本来就没被走到**。
   真因:**重建不会重写内容未变的 `build.ninja`** ⇒ 在原地编辑过 manifest 的目录里,
   manifest 永远最新,快路径**永久关闭**。⇒ 这类测试必须**新建干净工程**,并先确认它**变红**。
2. **e2e/131 的私有性断言判据失效**:它用「`#include <stdio.h>` 编不过」证明依赖的
   `include-dir` 不到达消费者。libc 归目标之后 `<stdio.h>` **本来就该编得过**(和宿主一样)。
   **原来那条测的其实是「libc 从哪来」,不是「作用域对不对」。**
3. **「拿走再装回来」**:xim 安装期依赖边在**已装好的机器**上不可验证。
   判据必须是把 store 里的包改名藏起来 → `mcpp add` + `mcpp build` 把它装回来。

### 5.3 ⚠️ `.map` 曾是「写了但没人跟踪」

`-Wl,-Map=` 是链接命令上的 flag,不是独立边,所以很容易只写不声明 ——
删掉 `firmware.map`,ninja 认为 ELF 是新的、什么都不做,map 就永远不回来。
已声明为链接边的 implicit output,e2e/132 用「删掉必须回来」钉住(**这是唯一能在修复前失败的形式**)。

### 5.4 测试面

- 单测:`test_freestanding.cpp` 25 个 + `test_build_directives` 33 个 + cache_key 19 个。
- e2e:130(引擎链)/131(生态链)/132(test + 产物)/133(std 子集),共 983 行。
- CI:新增 `baremetal` job,**两个 home 都装 qemu**(shim 按拥有它的 home 派发),
  并且**断言每条 PASS 行真的出现** —— `run_all.sh` 跳过时退出码是 0。
- 包侧 CI:`riscv-virt-rt` 与 `std-freestanding` 各自仓库 CI 真跑 qemu,rv64+rv32 双档。

---

## 6. ⚠️ 一日四个补丁版本:这个节奏说明什么

`2026.8.19.1` → `.2` → `.3` → `.4`,其中 `.3` 与 `.4` 都是在**发布之后**发现问题才发的。

**不是坏事的部分**:`.3` 的缺陷是靠「用发布二进制从零走一遍新用户流程」发现的 ——
这一步如果省掉,它会留在生态里直到有用户撞上。**这一步的价值被证明了。**

**是问题的部分**:

- `.2` 发布时,`mcpp build && mcpp run` 这条**新用户最可能敲的两条命令**没有被任何测试覆盖。
  四个裸机 e2e 都先 `run`,是巧合而非设计。
- `.4` 的架构问题(§2.3)是**用户 review 发现的**,不是我自查发现的。我在 `.3` 的收尾报告里
  甚至把 std 子集写成「要从头实现、不该在收尾时半做」——**而调研文档 X1–X10 早就把这条路测到了底**。
  ⇒ **写「这条做不了」之前必须回查自己的调研文档。**

**改进项**(下一轮的硬约束):

1. 发布前必须跑一遍**命令序列矩阵**(`build→run`、`run→build`、`build→test`),而不是单条命令。
2. 任何「某某做不了 / 代价很大」的结论,若同一课题有调研文档,**必须引用文档中的具体测量**才能下。
3. 回归测试必须做 **revert-A 探针**(关掉修复看它是否变红),否则不算写完。

---

## 7. 跨平台评估

⭐ **裸机 target 反而是最省跨平台成本的一类**:clang/lld 是 cross-compiler by construction,
**任何能装 llvm 载荷的宿主都能产出 RISC-V 镜像**,不需要 per-host 交叉载荷。
`host_can_serve` 对 freestanding 直接返回 `true`,这条在目标表注释里写明了。

| 轴 | 状态 |
|---|---|
| 宿主 | 引擎侧 21 个 CI job 全绿(linux/macOS/Windows/aarch64/hermetic/mingw-cross) |
| 模拟器 | `xim:qemu-riscv` 覆盖 **五个宿主目标**(linux x64/arm64 · darwin x64/arm64 · win32 x64) |
| 目标 sysroot | `picolibc-riscv` 与宿主无关(目标侧产物),三平台同一份 |
| ⚠️ 缺口 | `qemu-riscv` **win32-arm64 上游无资产** ⇒ 该宿主装不上,行为正确但会失败 |
| ⚠️ 缺口 | 两个包的仓库 CI **只有 ubuntu-24.04**;macOS/Windows 上的裸机链**没有持续验证** |

⚠️ **mcpp-index 的 `tests/examples/` workspace 成员在三个平台上无条件跑,没有能力门**
⇒ 需要模拟器/目标 sysroot 的包**不能加成员**,验证只能放包自己的仓库 CI。
这是一个**已知的覆盖缺口**,不是遗漏。

---

## 8. 仍然开着的

| 项 | 性质 | 备注 |
|---|---|---|
| **T3 档**(`std::format`、标量 `std::sort`、完整 `std::string`) | 真实边界 | libc++ 把这些实体放在编译版库里(标量 `__sort` 是 `extern template`,**无宏可关**)⇒ 需为目标编 `libc++.a`,是新载荷 |
| `mcpp-index` #220 | 待发布生效 | 依赖 2026.8.19.4 |
| 目标表**索引化** | 已定为阶段二 | 把 `pin` + `sysroot` 搬到索引,含老客户端降级路径;阶段一不会让它更难做 |
| 第二块板 / ARM Cortex-M | 未开始 | rv32 已作为「ISA 表是数据」的证据 |
| `prepare.cppm` / `execute.cppm` 体量 | 技术债 | 见 §4 |
| 包侧 CI 单平台 | 覆盖缺口 | 见 §7 |

---

## 9. 分项评分

| 维度 | 评价 | 依据 |
|---|---|---|
| **架构** | 好 | 能力增长未换来概念增长(2 条指令 + 1 个字段);三个计划中的大机制被实测砍掉;单一读取点 |
| **兼容性** | 好,但有一次真实风险 | 索引底线未抬、别名保留、破坏面为零;⚠️ 但 `-fno-exceptions` 差点造成升级即坏,靠缓存键轴补上 |
| **简洁优雅** | 好 | 432 行承载全部裸机知识;ISA 表是数据;模板随包走;子集不手写导出表 |
| **稳定性** | ⚠️ 中 | 五个自造缺陷,两个是**发布后**才发现;三条判据本身是错的 |
| **跨平台** | 好,有已知缺口 | 引擎侧 21 job 全绿、模拟器五宿主;⚠️ 包侧 CI 单平台 |
| **一致性** | 好 | `--target` 在所有子命令上拼写一致(靠位置参数改名换来,而非放弃一致性) |
| **无感升级** | ⚠️ 中 | 缓存键轴与诊断升级提示都是**补出来的**,不是设计时就有的 |

---

## 10. 三条要带进下一轮的

1. ⭐ **计划里的数字就是探针。** 本轮被实测推翻的计划主张:qemu 冷启动(0.4s→12ms)、
   「裸机没有退出码」(semihosting 有)、builtins 缺口触发者(不是 64 位除法,是 128 位移位)、
   「libc++ 头一个都用不了」(103/110)。**四条全是我没测就写下的。**
2. ⭐ **在「已经配置好的机器」上,很多边验不出来。** 目标 libc 的依赖边、xim 安装期依赖边、
   快路径前置条件 —— 三者的共同点是**存在与不存在长得一模一样**。
   判据必须是**拿走**(stash 后重装)或**新建**(干净工程),不能是「在我这儿是好的」。
3. ⚠️ **顺序本身是被测对象。** `mcpp run` 对、`mcpp build && mcpp run` 错,
   而四个裸机 e2e 恰好都先 `run`。**命令序列矩阵要进发布前检查表。**
