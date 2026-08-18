# 裸机 / freestanding 支持 — 实施计划(2026-08-19)

- Date: 2026-08-19
- Status: **实施计划,待 review**
- 上游:`2026-08-19-freestanding-baremetal-design.md`(方案,决策已全部落定见其 §22)
- 证据:`2026-08-18-freestanding-baremetal-analysis.md`(E/X/Y/Z/C/K/KA/TH/M/ISO/AAL/CABI/WS 全部复现命令)
- **范围 = Route B(方案 §21.3)+ `mcpplibs.std.freestanding`**(2026-08-19 追加,见 §1.6)
- D 档(openkal / openhal / openarch)路线图见 §7 —— **规划清楚但不并入本计划的里程碑**。

---

## 0. 拆解原则

1. **每个工作单元 = 一个可独立落地、可独立验收的 PR。** 不做「大 PR 拆成子任务」。
2. **判据是可执行断言,不是「跑通了」。** 每个单元的验收写成命令 + 期望输出。
3. **最大化解耦**:凡是不在关键路径上的,都标为并行线,不排在里程碑里等。
4. ⚠️ **两个前置先行**:`W0`(止血)与 `W12+P1`(CI 能真跑)必须最先落 —— 否则**后面所有里程碑的绿色都不可信**。

---

## 1. 工作单元

### 1.1 关键路径(串行,7 个)

| ID | 内容 | 依赖 | 验收判据(可执行) | 规模 |
|---|---|---|---|---|
| **W0** | **修 E1 静默失败**:门的判据从「triple 认不认识」改成「这次构建为哪个 target 发了 `--target`」(方案 §2.4 三条) | — | `mcpp build --target riscv64-none-elf` 无裸机配置时**指名道姓报错**;⚠️ **不存在「成功且产物是 x86-64」的路径**;回归测试直接断言 `file` 输出 | S |
| **W1** | **triple 收 `os=none`**:`parse()` 的 `none` 仅当整串无其它 OS token 时作 OS 段(`triple.cppm:299`) | W0 | `--target riscv64-none-elf` 解析成功;⚠️ `x86_64-none-linux-gnu` **仍解析为 linux**(正反两侧单测);`family()` 对 none 返空 | S |
| **W2** | **TargetSpec 表 + 单一取值口**:`TargetInfo` 加 `spec` 列;新增 `resolve_target_spec()`;消费方逐个改为从它取 | W1 | 消费方清单(compile/link flags · cfg · 产物命名 · runner · BMI 键 · hermetic 白名单)**逐个核对无第二处推导**;`kKnownTargets` 含 `riscv64-none-elf` 行 | M |
| **W4** | **`CLibMode::Freestanding`**:新态发 `-nostdlib -nostartfiles -static -T`;链接器按**载荷绝对路径**寻址 + 校验 `--version` 含 `LLD`;hermetic 语义反转(`allow_host_libs` 在 `os=none` 下失效) | W2, W3 | 产物 `file` 含 `UCB RISC-V`;`readelf -l` **无 PT_INTERP**;`llvm-nm -u` **空**;`build.ninja` 里 `--target` 出现次数 **> 0** | M |
| **W8** | **provisions 三种**:`LinkerScript`(单值,冲突报错)· `StartupObjects`(**两个具名槽 `prologue`/`epilogue`,槽内数组序**)· `Runner` | W4 | BSP 包提供的 `link.ld` + `crti/crtn` 按 `prologue → 用户对象 → epilogue` 进链接线;⚠️ 两个 BSP 同时提供 = **硬错误** | M |
| **W9** | **`mcpp run` runner**:`[target].runner` argv 模板 + 产物路径追加尾部;BSP 亦可经 provision 提供 | W2, W8 | `mcpp run --target riscv64-none-elf` 在 qemu 里打印出预期串 | S |
| **W11** | **`mcpp test` 裸机**:默认 `batch`(一镜像一次 qemu,semihosting 打用例名)· `isolated` 可选 · **超时后自动重跑一次 isolated 定位** | W9, P1 | 一个含 3 个用例的裸机测试,batch 模式一次 qemu 全过;人为让第 2 个跑飞 ⇒ **超时后能指出是第 2 个** | M |

### 1.2 前置线(必须早于关键路径的对应节点)

| ID | 内容 | 必须早于 | 验收判据 | 规模 |
|---|---|---|---|---|
| **W3** | ⚠️ **探测代码普查**:所有「编个小程序链一下」的探测(doctor · 工具链能力探测 · `hasImportStd` 探测)在 `os=none` 下会**假失败**(CMake `TRY_COMPILE_TARGET_TYPE` 教训) | **W4** | 逐个列出探测点并标注 `os=none` 下的行为;修掉会假失败的 | S–M |
| **W12** | ⚠️ **e2e harness `# requires-hard:`**:能力缺失时 **FAIL 而非 SKIP**(`# requires:` 保持 SKIP);CI 汇总输出**实际执行条数** | **W4 的验收** | 故意不装 qemu ⇒ 裸机 e2e **job 红**,不是绿着跳过 | S |
| **P1** | **qemu 进 xim-pkgindex**(xlings/mcpp 双生态复用) | **W12 生效** | `mcpp` 侧能经 `[xlings] deps` 装到 `qemu-system-riscv64` | S,非引擎 |

### 1.3 并行线(不在关键路径,任何时候可做)

| ID | 内容 | 依赖 | 验收判据 | 规模 |
|---|---|---|---|---|
| **W5** | **BMI 缓存键 + 指纹**:`TargetSpec` 全字段进指纹;freestanding 进 BMI 键 | W2 | 同源码在 hosted / freestanding 下 BMI **互不复用**;⚠️ 判据取自 ISO 系列:**混用不同 flag 的 BMI 会让编译器崩** | S |
| **W6** | **`import std` 关断**:`os=none` ⇒ `hasImportStd=false` + 说明「为什么」并指向 `mcpplibs.std.freestanding` | W1 | `import std;` 在 `os=none` 下给出 mcpp 自己的诊断,**不是 libstdc++ 内部报错** | S |
| **W7a** | **`cfg(freestanding)` / `cfg(os="none")` 谓词** | W1 | `[target.'cfg(freestanding)'.build]` 在裸机 target 下生效、hosted 下不生效 | S |
| **W13** | **产物形态**:`.elf` + `.bin`(objcopy)+ `.map`(`-Wl,-Map=`)+ **size 摘要**默认;`.hex` 可选 | W4 | 四件产物齐备;构建后打印 text/data/bss | S |
| **W15** | ⚠️ **CDB 合并到 workspace 根**(与裸机无关的已有 gap:实测每成员一份、根上没有) | — | `mcpp build --workspace` 后 workspace 根有一份 CDB,**每个文件带它所属成员 target 的 flags** | S |
| **W16** | **`--target` + `--workspace` 冲突即报错**(成员声明了 `[build] target` 而命令行给了不同的) | — | 混合 workspace 上 `--workspace --target X` **报错说明冲突**,不静默覆盖 | S |
| **W17** | **裸机 `mcpp pack`**:产物集 + sha256 + 构建元数据(target · toolchain · 版本 · BSP 版本) | W13 | 打出的包含元数据,能回答「这台设备上是哪个构建」 | S |

### 1.4 ②工效档(可整体推迟到关键路径跑通之后)

> ⚠️ 方案 §7.5.4 已论证:**兜底是链接期硬错** ⇒ 这一档只影响**错误发生的时机与可读性**,不影响正确性。

| ID | 内容 | 依赖 | 规模 |
|---|---|---|---|
| **W10** | **能力模型扩到包级**:`requires` 从 `featureRequires`(feature 级)扩到包/构建级;resolver 保证「**恰好一个提供者**」;能力集合并(`TargetSpec.envCaps ∪ BSP ∪ 后端 ∪ HAL 提供者`) | — | M |
| **W7b** | **`cfg(capability = "heap")` 谓词** | W10 | S |
| **W14** | **契约层**:`[freestanding]` 段(tier/requires/example)· 一致性示例构建 + 符号审计 · 证据写 xpkg 描述符(**手写字段被覆盖**)· 索引三态展示 | W10 | M–L |

### 1.5 载荷 / 生态(非引擎,完全并行)

| ID | 内容 | 依赖 | ⚠️ 判据 | 状态 |
|---|---|---|---|---|
| **P1** | qemu 进 xim-pkgindex | — | `mcpp` 侧能经 `[xlings] deps` 装到 `qemu-system-riscv64` | ✅ **已合入**(见 §1.5.1) |
| **P2** | per-target compiler-rt builtins | — | ⚠️ 判据见 §1.5.2 —— **原判据「含一次 64 位除法」已被实测推翻** | ✅ **已发布**(与 P3 同包,§1.5.3) |
| **P3** | picolibc 载荷 | — | ⚠️ 发行版包**无 rv64gc 变体**(X11)⇒ 自建 | ✅ **已发布**(§1.5.3) |
| **P4** | libc++ 每目标 freestanding `__config_site` | P3 | 17 个抽样头在 `riscv64-none-elf` 下可编 | 未做 |
| **E-BSP** | `riscv-virt-rt` BSP 包 | W8 | 方案 §7.1 的三文件工程端到端通过 | — |
| **E-STD** | `mcpplibs.std.freestanding` | 见 §1.6(**分三阶段,Stage 1 零载荷依赖**) | ⚠️ 热路径薄封装**必须标 `inline`**(方案 §16.2) | — |

#### 1.5.1 P1 已落地(2026-08-19,xim-pkgindex #651 已 squash 合入)

`xim:qemu-riscv@9.2.4-1`,取 **xPack** 官方预编译 —— 唯一为本索引服务的全部五个宿主目标
(linux x64/arm64 · darwin x64/arm64 · win32 x64)从同一次版本化发布出货、且每个资产带
`.sha` sidecar 的上游。⭐ 顺带在 CI 里落了一条 `baremetal riscv smoke`:**用 llvm 载荷把
一个 C++20 模块接口单元编成裸机内核,在这个 qemu 里启动,断言 OpenSBI banner + 内核自
己的输出**。这条流水线就是 M3「能跑」的现成载体。

⚠️ 过程中在**索引基础设施**上撞出两个从没人走过的缺陷,已一并修掉:

1. `arch_alias` + `ci.mirror` 在 **spec 文档写的字段顺序**下把 SHA256 当架构名拼进镜像 URL;
2. 版本 bump 机器人会把 `["latest"]` **往回搬**(实测 `fd 10.4.2→10.4.1`、`qemu-riscv 9.2.4-1→8.2.6-1`)。

⚠️ 还有一条与 **mcpp 自身载荷处理同形**的教训:xlings 的 elfpatch **整条替换 `DT_RPATH`
而不是前置追加**,自带库的载荷(`RPATH=$ORIGIN/../libexec`)一旦声明 loader 依赖就全部
失联 —— **而 install 报成功**,只有真运行才炸。参见记忆里 PR#414 的 `$ORIGIN` 优先级问题:
这是同一条边的另一侧。

#### 1.5.2 ⚠️ 实测推翻 P2 的原判据

原写的是「验收用例必须含一次 64 位除法或软浮点」。**在默认档 `rv64gc/lp64d` 上,64 位除
法根本不需要 builtins** —— `rv64gc` 含 `M` 扩展,有硬件 `divu`;`lp64d` 是硬浮点。实测:

| march/mabi | 缺的 builtins |
|---|---|
| `rv64gc/lp64d`(**默认裸机档**) | **无** |
| `rv64imac/lp64` | `__muldf3` |
| `rv64i/lp64` · `rv32imac/ilp32` | `__muldf3` + `__udivdi3` |

⭐ **但 builtins 在 rv64gc 上仍然是真需要的** —— 触发者不是除法,是 **128 位移位**:
picolibc 的 `printf` 浮点格式化(ryu)引用 `__ashlti3` / `__lshrti3`,rv64 无对应指令。

⇒ **新判据**:验收用例必须**用 picolibc 的 `printf` 打一个浮点数**(或显式做 128 位移位),
而不是「含一次 64 位除法」。前者在默认档上就能暴露缺口,后者不能。

#### 1.5.3 P2+P3 已发布为一个包(`xim:picolibc-riscv@1.8.12`)

picolibc 1.8.12 + compiler-rt builtins(LLVM 22.1.8),用 **mcpp 自己的 LLVM 载荷**
交叉编两个档位:

| profile | triple | 内容 |
|---|---|---|
| `rv64gc/lp64d` | `riscv64-none-elf` | 139 头 + 37 个 lib 文件 |
| `rv32imac/ilp32` | `riscv32-none-elf` | 同上 |

⭐ **关键架构性质:目标侧产物,与宿主无关** —— 一份 5.4MB 归档服务全部平台/架构,
不做宿主矩阵。已发到 `xlings-res/picolibc-riscv`(GitHub + GitCode,两端哈希一致)。

⚠️ **P2 与 P3 必须同包**:picolibc 的 `printf` 没有 builtins 链不上(§1.5.2)。

**端到端已验**(sysroot 与 qemu 都来自 xlings):C++20 模块 → `printf` 浮点 →
`malloc` → semihosting 跑出 `picolibc-riscv sysroot: 42 / 3.1416 / ok` + `malloc=yes`;
同一模块按 rv32imac/ilp32 链出 32 位软浮点 ELF。

⚠️ **三条与 W4 直接相关的实测**:

1. **E6-1 独立复现**:`-fuse-ld=lld` 解析到了**宿主的 GNU ld**
   (`xim-x-binutils/2.42/bin/ld`,报 `unrecognised emulation mode: elf64lriscv`)。
   **W4 必须用链接器的绝对路径**,这条不是理论风险。
2. ⭐ **llvm 载荷的 `bin/clang.cfg` 无条件注入宿主 glibc 头 + 写死的 x86-64
   `--dynamic-linker` + 宿主架构 `-L`,一条都不按 target 设防**。不加
   `--no-default-config`,宿主 `<limits.h>` 会盖过 freestanding 那份。
   ✅ **mcpp 已经躲开了**(`src/build/flags.cppm:495` 显式 `--no-default-config`
   并自己提供全部内容)—— 这条记下来是为了让 W3/W4 不必再推导一次。
3. ⚠️ **sysroot 的目标头绝不能进 subos sysroot**(会遮住宿主 libc)。消费者自己把
   `--sysroot`/`-isystem` 指到 `<install>/{include,lib}/<march>/<mabi>/`。
   **这就是 W4 该消费它的形状。**

### 1.6 ⭐ `mcpplibs.std.freestanding` —— 提入承诺范围,分三阶段

**为什么必须做**:没有 std 子集,mcpp 的裸机故事就退化成「C with classes」——**证明不了 mcpp 自己的价值**(modules + 现代 C++)。它是这条线唯一能验证「mcpp 裸机 ≠ 退回 C」的东西。

**实测:它可以分三阶段,而且 Stage 1 不依赖任何载荷工作。**

| 阶段 | 交付 | 依赖 | 实测状态 |
|---|---|---|---|
| **S-1 零 libc 档** | 8 个头 + 基础类型:`type_traits` `concepts` `span` `expected` `bit` `utility` `tuple` `charconv` + `size_t/byte/uintN_t` | **仅需 freestanding `__config_site`**(4 个 `#define` 翻转,**包可用 `build.mcpp` 从宿主那份生成**)⚠️ 外加 BSP 提供 `memcpy/memmove/memset/memcmp` | ✅ **模块编译通过、消费者编译通过**;链接只缺 `memmove`(ST1) |
| **S-2 +picolibc 头** | **52 个头的可移植子集**(两家标准库交集,Y4) | P3 的**头文件部分**(不需要 `libc.a`) | ✅ 17/17 抽样、103/110 全量可编(X2/X3) |
| **S-3 +目标版 libc++.a** | `std::format` · 标量 `std::sort` · `std::string` 全功能 | P4 完整 | ⚠️ 需为目标编 libc++ |

⭐ **S-1 的关键性质:零载荷工作。** `__config_site` 是包自己生成的文件,picolibc 也不需要。⇒ **E-STD 可以承诺,而不必同时承诺全部载荷工程。**

⚠️ **`memcpy/memmove/memset/memcmp` 是编译器可自行发出的四个函数** —— 任何裸机工程都要提供,**归 BSP**(`E-BSP`),不归 std 子集包。这条要写进 BSP 契约。

**判据:**

| 阶段 | 验收 |
|---|---|
| S-1 | `import mcpplibs.std.freestanding;` + `std::to_chars` + `std::span` 在 `riscv64-none-elf` 上链接**零未定义符号**(BSP 提供四个 mem 函数) |
| S-2 | 方案 §7.1 的三文件工程跑通(`std::array`/`ranges::sort`/`optional`/`atomic`/`string_view`) |
| S-3 | `std::format_to` + `std::vector` 在裸机跑通 |

⚠️ 三阶段各自**独立可发布**,S-1 就能证明能力。

---

## 2. 依赖拓扑

```
                 ┌──────────────────────────────────────────────┐
前置(最先落)   │  W0 止血        W12 harness ── P1 qemu       │
                 └───────┬──────────────────┬───────────────────┘
                         │                  │(让后面的绿色可信)
关键路径                 ▼                  │
                       W1 triple            │
                         │                  │
                         ▼                  │
                       W2 TargetSpec ◄──────┼──── W3 探测普查(必须早于 W4)
                         │                  │
                         ▼                  │
                       W4 Freestanding 链接 │
                         │                  │
                         ▼                  │
                       W8 provisions        │
                         │                  │
                         ▼                  │
                       W9 runner            │
                         │                  │
                         ▼                  │
                       W11 test ◄───────────┘

并行线(挂在 W1/W2/W4 之后,互不阻塞):
   W5 BMI键(←W2)   W6 import std关断(←W1)   W7a cfg(←W1)   W13 产物形态(←W4)
   W17 pack(←W13)

完全独立(任何时候):
   W15 CDB合并      W16 workspace target冲突      P2 builtins   P3 picolibc

②工效档(整体可推迟):
   W10 能力模型 ──► W7b cfg(capability)
              └──► W14 契约层

生态:
   E-BSP(←W8;并提供 memcpy/memmove/memset/memcmp)
   E-STD S-1(←零载荷,任何时候可做)──► S-2(←P3 头)──► S-3(←P4)
```

**关键路径长度 = 7 个单元**(W0→W1→W2→W4→W8→W9→W11),其余 **13 个可并行**。

---

## 3. 里程碑与验收

| 里程碑 | 含 | ⚠️ 验收判据(必须是这句) |
|---|---|---|
| **M0 止血 + 可信** | W0 · W12 · P1 | ① E1 **不可复现**;② 故意不装 qemu ⇒ 裸机 e2e **job 红而不是绿着跳过** |
| **M1 能表达** | W1 · W2 · W3 | `--target riscv64-none-elf` 解析成功;无裸机配置时**指名道姓报错**;⚠️ 探测点普查清单已产出 |
| **M2 能构建** | W4 · W5 · W6 · W13 | 产物 `file` 含 `UCB RISC-V` · `readelf -l` **无 PT_INTERP** · `llvm-nm -u` **空** · `build.ninja` 中 `--target` **> 0**;⚠️ **验收工程必须含一次 64 位除法**(P2 假绿防线)且**至少有一个依赖**(flags 作用域防线) |
| **M3 能跑** | W8 · W9 · E-BSP | qemu 里打印出预期串;⚠️ 判据是**「裸机 e2e 实际执行条数 > 0」**,不是「全绿」 |
| **M4 能测** | W11 | 3 用例 batch 一次 qemu 全过;人为让第 2 个跑飞 ⇒ **超时后指出是第 2 个** |
| **M5 工效** | W10 · W7b · W14 · W15 · W16 · W17 | 缺能力时**解析期**指名道姓;索引三态可见 |
| **M6 生态 S-1** | E-STD **S-1** | ⭐ `import mcpplibs.std.freestanding` + `std::to_chars` + `std::span` 在裸机链接**零未定义符号** —— **这是「mcpp 裸机 ≠ 退回 C」的最小证明** |
| **M7 生态 S-2/S-3** | P2 · P3 · P4 · E-STD S-2/S-3 | 52 头子集跑通;`std::format_to` + `std::vector` 在裸机跑通 |

---

## 4. ⚠️ 实施期风险(每条都有实测出处)

| # | 风险 | 防线 |
|---|---|---|
| 1 | **CI 绿着但从没跑过** | M0 的 W12 前置;判据是**执行条数 > 0** |
| 2 | **hello-kernel 是 builtins 缺口的假绿**(E3:测试内核 `llvm-nm -u` 为空,一路全绿) | M2 验收工程**必须含 64 位除法/软浮点** |
| 3 | **单包验证会得出错误结论**(§4.1:flags 作用域,加第二个包才碎) | M2 验收工程**至少一个依赖** |
| 4 | **探测代码在 `os=none` 下假失败** | W3 必须早于 W4 |
| 5 | **BMI flag 不一致 ⇒ 编译器崩**(ISO 系列,不只是结果不对) | W5 与 W4 同期 |
| 6 | **新门被逃生舱绕开**(E1 的原始形状:守卫挂在 `known` 分支) | 每个新门自问「**逃生舱走不走这里**」并写测试 |
| 7 | **`ld.lld` 按名字解析到 GNU ld**(E6-1) | W4 强制绝对路径 + `--version` 校验 |
| 8 | **`thread_local` 静默写坏内存**(TH6:编过链过零诊断) | ⚠️ 裸机验收用例**必须含一个 `thread_local`** |
| 9 | **带序 provision 被顺手做成集合** | W8 判据显式检查链接线顺序 |
| 10 | **读代码下结论**(本轮三次被实测推翻:ISO 对照 / E1 / WS 多 target) | 每个单元的判据都是**命令 + 期望输出**,不是「看代码应该对」 |

---

## 5. 解耦说明:哪些可以完全独立开工

**零依赖,今天就能开:**
- `W0` 止血 · `W12` harness · `P1` qemu · `W15` CDB 合并 · `W16` workspace 冲突 · `W10` 能力模型 · `P2` builtins · `P3` picolibc

**⇒ 8 个单元可以在关键路径开始前或并行进行。**

**关键路径上只有 7 个**,且每个都是「在已有结构上加列/加态/加行」,不是新子系统:
- `triple.cppm` 加列 · `linkmodel.cppm` 加态 · `provisions.cppm` 加行 · `cmd_build` 加 runner

**⚠️ 唯一真正的新语义**:`W8` 的 `StartupObjects` 两个具名槽(现有 provision 全是无序集合)。

---

## 6. 不在本计划内

| 项 | 去向 |
|---|---|
| C 档载荷(P2/P3/P4) | **按需**;⚠️ 但 `E-STD` **已提入承诺范围**(§1.6),其 S-1 阶段零载荷依赖 |
| D 档(openkal / openhal / openarch) | **不并入本计划里程碑**;⭐ **路线图见 §7**(分四阶段 + 每阶段的继续/停止判据) |
| 烧录 / OTA | ⚠️ **设备侧的事,mcpp 不碰**(方案 §21.1) |
| AAL/KAL/HAL 接口内容 · 后端实现 · 后端选择策略 | ⚠️ 生态侧;**引擎永不认识这三层**(方案 §11.6 / §14.0) |

---

## 7. D 档路线图:openkal / openhal / openarch

> ⚠️ **不并入 §3 的里程碑。** 这里规划清楚是为了让「什么时候该继续、什么时候该停」有明确判据 —— 而不是为了排期。
>
> **顺序依据**(方案 §12.4):按「门槛 × 先例厚度」排 —— openkal 最先(门槛最低、先例最厚、且是 std 子集的天然后端),openarch 最后(风险最高、两个硬原语未验证)。

### 7.1 四个阶段与门

| 阶段 | 交付 | 前置 | ⭐ 继续的判据(gate) | ⚠️ 停止的信号 |
|---|---|---|---|---|
| **D0 探针** | `openkal` 最小 world(`io/streams` + `clocks/monotonic` + `memory/alloc`)· **两个后端**(linux / bare)· conformance 骨架 | Route B 的 M3 | ⭐ **有第三方实现了第三个后端** —— 这才是 D 能否成立的真变量 | 半年内无人实现第三个后端 ⇒ **停在 D0,当作 mcpp 内部设施用** |
| **D1 openkal SPEC v0.1** | 接口清单冻结 · **C ABI 映射规范**(方案 §13.2)· **三个官方后端**(linux / **windows** / bare)· conformance suite 正式版 | D0 通过 | 三个官方后端**全部过 conformance**;⚠️ windows 后端**不经 MSVC CRT 的 POSIX 兼容层** | windows 后端做不出来而不模拟 ⇒ **SPEC 形状错了**(方案 §11.2.12),回炉重设计 |
| **D2 openhal** | 设备服务接口(Console/Serial/Spi/I2c/Gpio/Pwm/Adc/Delay)· `openhal-linux` 官方实现 | D1 通过 | **同一个驱动包既跑裸机 MCU 又跑 Linux**(对标 `linux-embedded-hal` 已证明的形状) | 驱动作者不来 ⇒ 停;openhal 不影响 openkal |
| **D3 openarch** | arch 机制接口(Context/Trap/AddressSpace/Atomics/PerCpu/Tick/Boot)· 两个 arch 实现 | D2 通过 **且**有真实内核项目 | ⚠️ **两个最硬原语不碎**:一段通用内核代码在两个真实不同 arch 上**既类型正确又语义正确,且 LTO 后无额外指令**(方案 §11.7) | 上下文切换或页表项抽象碎掉 ⇒ **停**;⚠️ 碎在这里,后面全是幻觉 |

### 7.2 每阶段的已知约束(全部来自实测)

| 阶段 | 必须遵守 |
|---|---|
| 全部 | **引擎永不认识这三层**(方案 §11.6 / §14.0);后端选择 = **条件依赖**,零新增轴 |
| D0/D1 | 契约 = **C ABI**(H1:跨包提供实现只有这一条路);`result<T,E>` = **2 字结构返回**(CABI 实测更便宜);⚠️ **`T` ≤ 一个机器字** |
| D1 | ⚠️ **不要模仿 POSIX** —— WASIp1 的教训;准入判据是「四后端自然实现,不需模拟层」;**每个 interface 独立版本 + 只增不改** |
| D2 | 契约 = **concept**(H3:多提供者共存、零成本);⚠️ **寄存器/ISR/DMA 进不来**;时序/实时性是**能力**不是可藏的差异 |
| D3 | 契约 = **concept + `inline`**(§16.2:显式 `inline` 是首选,LTO 是兜底);⚠️ **只含机制不含策略**;A 类裂缝(能编但语义不同)**必须提到能力轴** |

### 7.3 ⚠️ 贯穿 D 档的三条纪律

1. **缺能力 = 编译期不存在**,不是运行期 `ENOSYS`(X4 实测:`no type named 'mutex'`)。禁止静默降级。
2. **conformance suite 是分仓的必需配套**,不是可选项 —— 没有它,分仓会退化成「N 个各自解释接口的实现」(方案 §18.3)。
3. **官方维护 linux/windows/bare 三个后端不是为了服务用户,是为了让准入判据可执行**(方案 §18.4)。

### 7.4 与 Route B 的关系

- **D 完全依赖 B;B 不依赖 D** ⇒ 先做 B **零损失**。
- ⭐ **D0 的最小 world 与 `E-STD` 天生互补**:`mcpplibs.std.freestanding` 是 openkal 的**第一个真实消费者**,给了 D0 一个立刻可验证的验收标准 —— *同一份 std 子集,在 hosted 与裸机两个后端下行为一致*。
