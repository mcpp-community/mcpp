# 裸机 / freestanding — 第三阶段:从「能跑」到「能用」

- Date: 2026-08-19
- Status: **方案计划,待 review**
- 上游:`2026-08-19-baremetal-ecosystem-closure-plan.md`(第二阶段;§10 记录已落地与三处实测修正)
- 已发布基线:**mcpp 2026.8.19.1** · `xim:qemu-riscv@9.2.4-1` · `xim:picolibc-riscv@1.8.12`
- 范围:第二阶段 §10.4 列出的四个缺口 —— **runner 归属** · **生态发布** · **裸机 test** · **产物形态 + 模板**

---

## 0. 现在在哪(全部已验证,不是推测)

```
$ mcpp run --target-triple riscv64-none-elf        # mcpp 2026.8.19.1,来自已发布索引
BSP-CHAIN-OK 42
float 3.1416
MALLOC-OK
```

| 已经成立 | 证据 |
|---|---|
| 引擎认识 `riscv64-none-elf` / `riscv32-none-elf` | `toolchain list` 显示 `bare, static, cross` |
| 从 C++20 模块构建裸机镜像 | `tests/e2e/130`,CI 每次跑 |
| BSP 供 sysroot + 链接脚本 + 运行时,消费者只声明依赖 | `tests/e2e/131`,CI 每次跑 |
| `mcpp:link-script=` · `mcpp::xpkg_dir()` · `[target.X].runner` | 已发布在 2026.8.19.1 |

**还差四件事,而它们的性质完全不同:** 一件是**真设计题**(§2),一件是**发布工程**(§3),
两件是**已知形状的实现**(§4/§5/§6)。

---

## 1. 判据

沿用第二阶段 §1 的总判据(用户写五行、跑三条命令、看不到 `picolibc`/`crt0`/`-nostdlib`/
`-machine virt` 等词),并把否定判据补到四条:

| # | 不算「能用」 | 现状 |
|---|---|---|
| **N1** | `mcpp run` 要用户自己拼 qemu 命令 | ⛔ **仍然违反** —— `runner` 在消费者 manifest 里 |
| **N2** | 要用户手写 `link-search` / `cflag` | ✅ 已解决(BSP 供) |
| **N3** | 换一块板要改引擎而不是换依赖 | ✅ 结构上成立,但**没有第二块板证过** |
| **N4** | 依赖只能用 `path = "..."`,`mcpp add` 拿不到 | ⛔ **仍然违反** —— 两个包都没进索引 |

⭐ **第三阶段的完成判据 = N1 与 N4 同时消失**,即:

```bash
mcpp new --template baremetal-riscv blinky && cd blinky
mcpp add riscv-virt-rt          # 从已发布索引
mcpp build --target riscv64-none-elf
mcpp run   --target riscv64-none-elf     # runner 由 BSP 供,manifest 里没有它
mcpp test  --target riscv64-none-elf
```

---

## 2. ⭐ R1:runner 归属 BSP —— 本阶段唯一的真设计题

### 2.1 为什么它不能照抄任何现成机制

四条**已实测**的约束,任意一条都能否掉一个方案:

| # | 约束 | 出处 |
|---|---|---|
| C1 | **模拟器必须用绝对路径** | CI 实测:runner 写裸名 `qemu-system-riscv64` 时 `mcpp run` 报 `[error] xlings: 'qemu-system-riscv64' is not installed`,而同一 job 两步之前 `--version` 刚跑通 —— shim 按**拥有它的 home**派发 |
| C2 | 那个绝对路径**机器相关**,静态 manifest 写不出来 | 载荷路径含 home 与版本 |
| C3 | 能算出它的只有 `build.mcpp` | 它是唯一拿得到 `xpkg_dir()` 与 `MCPP_TARGET_*` 的地方 |
| C4 | 但 `build.mcpp` 是**构建期**程序,`mcpp run` 要的是**运行期**值 | 定义 |

⇒ **C2+C3 排除静态 manifest 传播;C4 说明这个值必须被构建期算出、持久化、运行期读回。**

⚠️ 顺带排除一个看起来更简单的方案:让 BSP 在 manifest 里写
`[target.riscv64-none-elf] runner = [...]` 并让它沿依赖边传播。C1/C2 让它写不出可用的值。

### 2.2 设计:`mcpp:runner=` 指令 + 新 Slot

**一条新 directive,与 `link-script` 同族**(第二阶段已经证明这条路走得通):

```cpp
// BSP 的 build.mcpp
const char* qemu = mcpp::xpkg_dir("xim", "qemu-riscv");   // 已有接口
mcpp::runner(std::format("{}/bin/qemu-system-riscv64", qemu).c_str());
mcpp::runner("-machine"); mcpp::runner("virt");
mcpp::runner("-nographic"); mcpp::runner("-no-reboot");
mcpp::runner("-semihosting"); mcpp::runner("-bios"); mcpp::runner("none");
mcpp::runner("-kernel");
```

| 决定 | 取值 | 理由 |
|---|---|---|
| 编码 | **一行一个 argv token,按发射顺序** | argv 是有序列表,而 directive 是「一行一值」。JSON 数组(像 `action`)要引入转义规则,而这里没有任何一处需要嵌套 |
| Slot | **新 `Slot::Runner`** | 它既不是编译输入也不是链接输入;塞进 `LdFlags` 会让它进链接线 |
| Scope | **新 `Scope::RunGlobal`** | 语义与 `LinkGlobal` 平行(到达消费者),但落到运行配置而非链接线。⚠️ **必须是新的 Scope 值**,否则 `link-script` 与它会共享传播路径与冲突规则 |
| 持久化 | 随 directive 记录进构建缓存 | 已有机制,`mcpp run` 的快路径已经读它 |
| 冲突 | **两个依赖都供 = 硬错误**,消费者自己写的 `[target.X].runner` **覆盖**依赖供的 | 两个 BSP 同时供是配置错误;消费者显式覆盖是正当的(调试时换 `-bios default`) |

### 2.3 ⚠️ 这一条要改的地方(每处都要有测试)

1. `directives.cppm` — 新 Slot + 新 Scope + 一行表项 + `Transform::Verbatim`
2. `hostprogram.cppm` — `mcpp::runner(tok)` 接口 + protocol 4
3. 传播 — `Scope::RunGlobal` 的 fixpoint(与 LinkGlobal 同形,但**独立的冲突检查**)
4. `execute.cppm` — `build_run_target` 先读依赖供的,消费者 override 优先
5. `test_targets.cppm` / 测试执行 — **同一个 runner**(见 §4)

⭐ **判据(两侧)**:
- BSP 供 runner ⇒ 消费者 manifest 里**没有 `[target.*]` 段**也能 `mcpp run`;
- 两个依赖都供 ⇒ **硬错误并同时点名两个包**;
- 消费者写了自己的 ⇒ 用消费者的,且**说明它覆盖了谁**。

---

## 3. R2:两个生态包 + 索引收录

### 3.1 要建的仓库

| 仓库 | 内容 | 依赖 |
|---|---|---|
| `mcpplibs/riscv-virt-rt` | qemu `virt` 的 BSP。`[xlings] deps = ["xim:picolibc-riscv@1.8.12", "xim:qemu-riscv@9.2.4-1"]`;`build.mcpp` 按 `MCPP_TARGET_ARCH` 选档位,发 include-dir(私有)+ link-search/link-lib/link-script/**runner** | mcpp ≥ 2026.8.19.1(+ §2 后的版本) |
| `mcpplibs/std-freestanding` | `mcpplibs.std.freestanding` —— 私有 include picolibc 头,导出可移植 std 子集模块 | 同上 |

⚠️ **`riscv-virt-rt` 的代码在 `tests/e2e/131` 里已经逐行验证过**,建仓是把它从 heredoc 搬进仓库并补 README/CI/examples,不是重新设计。

### 3.2 ⚠️ 兼容底线:一个需要新 mcpp 的包,怎么进一个服务老客户端的索引

**已核准的事实:mcpp 只有索引级 `index.toml [index].min_mcpp`,没有包级的 mcpp 版本要求。**

| 方案 | 后果 |
|---|---|
| 抬高索引 `min_mcpp` | ⛔ **绝对不行** —— 索引是数据、mcpp 是程序,**发布数据不得让程序失效**;抬高会让所有老客户端拿不到**任何**包 |
| 什么都不加 | 老客户端能解析、能下载,构建到 `build.mcpp` 那一步才失败 —— 但失败**是可行动的**:2026.8.19.1 起诊断会说「要么这个包写给更新的 mcpp(试 `mcpp self update`),要么指令拼错了」 |
| 加**包级** `requires-mcpp` 键 | 早失败、消息更准,但 ⚠️ **要先探针**:老客户端遇到未知的**顶层 manifest 键**是警告还是硬失败?若是硬失败,已发布包永远无法采用新键(与 provisions 的 reexport 键同一形状) |

⭐ **决定:先按「什么都不加」发布,并把包级 `requires-mcpp` 作为一个独立探针(P-COMPAT)排在后面。**
理由:诊断已经可行动,而一个会让老客户端硬失败的新键代价高得多 —— 且这个代价**尚未测过**。

#### ⚠️ 实施时探到的:上表第 2 行「诊断可行动」对**类型化 API 是假的**

那条可行动的诊断(`protocol_error()`,点名 `mcpp self update`)只对**wire 键**生效,
而它要求程序**先编译得过**。BSP 用的是 `mcpp::runner(...)`,老 mcpp 上根本编不过,
拿到的是:

```
error: 'runner' is not a member of 'mcpp'
```

—— 读起来像**包作者拼错了**,而不是**读者的引擎旧了**。

我先试了在包里做优雅降级,**实测证伪**:

```cpp
if constexpr (requires { mcpp::runner("qemu"); })   // ✗ 名字不存在 ⇒ 硬错误
```

`requires` 表达式作用在**不存在的限定名**上是 ill-formed,**不是求值为 `false`**。
⇒ **语言内没有特性探测**,包侧无路可走。

⭐ **所以补在引擎侧**:`build.mcpp` 编译失败且错误里出现「不是 `mcpp` 的成员」时,
追加一段点名 `mcpp self update` 并报出当前版本的提示(三个前端三种写法都认)。
**这对以后每一次类型化 API 新增都生效**,不只是 `runner`。
判据:`mentions_missing_mcpp_api` 单测钉三种拼写 + 三条不该误报的普通错误。

### 3.3 收录进 `mcpplibs/mcpp-index`

`pkgs/r/riscv-virt-rt.lua` 与 `pkgs/m/mcpplibs.std.freestanding.lua`。

⭐ **判据是 `mcpp add` 而不是「文件进了仓库」**:

```bash
mcpp new probe && cd probe
mcpp add riscv-virt-rt              # 从已发布索引解析、下载、写进 mcpp.toml
mcpp build --target riscv64-none-elf
```

⚠️ **发布顺序有依赖**:`riscv-virt-rt` 的 runner 需要 §2 落地。⇒ 先发**不带 runner** 的
版本(0.1.0,消费者仍写 `[target.X].runner`),§2 落地后发 0.2.0 把 runner 收进 BSP。
**两次发布都要留在索引里**,这样 0.1.0 的用户不会被新 mcpp 的要求卡住。

---

## 4. R3:裸机 `mcpp test`(W11)

### 4.1 已核准的机制事实

- `mcpp test` 从 `tests/**/*.cpp` 发现用例,每个编成一个**独立可执行**,用
  `run_exec_deadline` 跑,退出码即判据。
- 裸机上这两条都不成立:没有进程退出码回到宿主,而且**每个用例一个镜像**意味着
  N 次 qemu 冷启动。

### 4.2 设计

| 决定 | 取值 | 理由 |
|---|---|---|
| 默认模式 | **`batch`**:所有用例编进**一个**镜像,semihosting 打用例名与结果,固件自行 poweroff | qemu `virt` 冷启动实测 ~0.4s;30 个用例 isolated 就是 12s 纯开销 |
| 结果通道 | **stdout 上的结构化行**(`mcpp-test: <name> <ok|fail>`),`mcpp` 侧解析 | 没有退出码可用;semihosting 的 stdout 是唯一可靠通道 |
| 收尾 | 固件写 syscon `0x5555` 关机 | 已验证:否则 qemu 永远不退,只能靠超时 |
| 超时 | 整镜像一个 deadline;**超时后自动用 `isolated` 重跑一次** | batch 下「跑飞」只知道**没跑完**,不知道**是谁**;重跑一次才定位得到 |
| `isolated` | 可选开关,一个用例一个镜像 | 定位用,不做默认 |
| 执行 | **走 §2 的同一个 runner** | ⚠️ 两条路径(run / test)各自推导 runner 就是「同一决策两处推导」 |

### 4.3 ⚠️ 判据

- 3 个用例 batch 一次 qemu 全过;
- **人为让第 2 个死循环 ⇒ 超时后 mcpp 指出是第 2 个**(不是「有用例超时」);
- ⚠️ **一个用例失败不能让整批静默变绿** —— 断言 `mcpp test` 的退出码非零且列出失败者。

---

## 5. R4:产物形态(W13)

| 产物 | 怎么来 | 为什么需要 |
|---|---|---|
| `.elf` | 已有 | 调试、qemu `-kernel` |
| `.bin` | `llvm-objcopy -O binary` | 真硬件烧录只吃裸二进制 |
| `.map` | `-Wl,-Map=<out>.map` | 裸机上「为什么这段没进来」只有 map 能答 |
| **size 摘要** | `llvm-size` → 构建后打印 text/data/bss | ⭐ 裸机的核心约束是**容量**;不打印等于让用户自己去查 |

⚠️ **实现要点**:`.bin` 与 `.map` 是**额外的 ninja 边**,不是链接命令的副产物 ——
`.map` 可以挂在链接行上,`.bin` 必须是一条以 `.elf` 为输入的新边,否则增量构建不会重生成。

⭐ **判据**:改一行源码 → `.bin` 的 mtime 与内容都变;`.map` 里能找到该符号。

---

## 6. R5:`mcpp new --template baremetal-riscv`(T1)

生成的工程**必须开箱通过 §1 的三条命令**,且 manifest 不超过:

```toml
[package]
name    = "blinky"
version = "0.1.0"

[build]
target = "riscv64-none-elf"

[dependencies]
riscv-virt-rt = "0.2"
```

⚠️ **两个实测约束会体现在模板里**:

1. **裸机固件可以有 `main`** —— 只要 BSP 带了 picolibc 的 `crt0`,`main` 就是普通的
   `main`。**只有零 libc 档才需要 `[targets.X] main = "src/start.S"`**。⇒ 模板走 BSP 路线,
   用户看到的就是 `int main()`。
2. 模板要**同时**给一个 `tests/` 用例,否则 `mcpp test` 在新工程上无事可做,W11 的价值
   在用户第一次接触时就是隐形的。

---

## 7. 依赖拓扑 · 里程碑 · 并行度

```
R1 runner 归属 BSP ──┬──► R2b riscv-virt-rt 0.2(收 runner)
   (引擎 + 协议 4)   │
                     └──► R3 裸机 mcpp test(共用同一个 runner)
                                    │
R2a 两个仓库 + 索引收录 ────────────┼──► R5 模板(需要 add 得到 + runner 由 BSP 供)
   (0.1.0,不含 runner)             │
                                    │
R4 产物形态 ────────────────────────┘  (完全独立,任何时候可做)
```

| 里程碑 | 含 | ⚠️ 验收判据(必须是这句) |
|---|---|---|
| **M-3′ 生态可取** | R2a | `mcpp add riscv-virt-rt` 从**已发布索引**成功,且构建跑通 |
| **M-3″ N1 消失** | R1 · R2b | ⭐ 消费者 manifest **没有 `[target.*]` 段**,`mcpp run` 仍在 qemu 里跑出预期串 |
| **M-4 能测** | R3 | 3 用例 batch 一次全过;第 2 个跑飞 ⇒ **超时后指出是第 2 个**;1 个失败 ⇒ **退出码非零** |
| **M-5 能烧** | R4 | 改一行源码 ⇒ `.bin` 内容变;构建后打印 text/data/bss |
| **M-6 ⭐ 能用** | R5 | ⭐ **干净机器上 `mcpp new --template baremetal-riscv` → 三条命令全过,用户没见过 §1 列出的任何一个词** |

**并行度**:R4 零依赖;R2a 只依赖已发布的 mcpp(今天就能开);R1 是唯一的串行头。

---

## 8. ⚠️ 风险(每条有出处)

| # | 风险 | 出处 | 防线 |
|---|---|---|---|
| **R-1** | **runner 用裸名 ⇒ 换台机器就不工作** | CI 实测(shim 按 owner home 派发) | BSP 必须发**绝对路径**;e2e 断言 runner 首 token 是绝对路径 |
| **R-2** | **新 Scope 与 LinkGlobal 共用传播路径** ⇒ 冲突规则互相污染 | 设计 | `Scope::RunGlobal` 必须是**独立值**,并有自己的冲突测试 |
| **R-3** | **run 与 test 各自推导 runner** | 本仓反复付过的形状(#233/#240/#242/#344) | 一个读点,两个调用方 |
| **R-4** | **batch 模式下一个用例失败被读成整批成功** | 假绿家族 | 判据是**退出码非零 + 列出失败者**,不是「有输出」 |
| **R-5** | **`.bin` 不是新 ninja 边 ⇒ 增量构建拿到陈旧二进制** | 增量语义 | 判据是**改一行源码后 `.bin` 内容变** |
| **R-6** | **抬高索引 `min_mcpp` 把老客户端变砖** | 记忆:PR#349 | ⛔ 绝不抬高;包级要求走 P-COMPAT 探针 |
| **R-7** | **未知顶层 manifest 键让老客户端硬失败** ⇒ 已发布包永远无法采用新键 | provisions reexport 同形 | P-COMPAT **先探再定**,不先加键 |
| **R-8** | **模板生成的工程在干净机器上装不齐依赖** | 首次体验 | M-6 判据明写「干净机器」 |
| **R-9** | **只有一块板 ⇒ N3 从未被证过** | 现状 | R2b 之后加**第二块板**(rv32 档)作为 N3 的证据 |
| **R-10** | **读代码下结论** | 本轮被实测推翻五次以上 | 每个单元判据都是命令 + 期望输出 |

---

## 9. 兼容性与无感升级

| 轴 | 问题 | 处置 |
|---|---|---|
| **protocol 3 → 4** | 加 `mcpp:runner=` 要不要抬 protocol? | ⭐ **要**。抬了之后,老 mcpp 遇到它会说「要么这个包写给更新的 mcpp,要么拼错了」(2026.8.19.1 起的措辞),而不是静默丢弃 |
| **BSP 0.1.0 → 0.2.0** | 0.2.0 需要更新的 mcpp | **两个版本都留在索引里**;0.1.0 的用户不受影响 |
| **索引底线** | 是否抬 `min_mcpp` | ⛔ 不抬(R-6) |
| **`[target.X].runner` 的去留** | BSP 供了之后它还留着吗 | **留着,并且是覆盖语义** —— 调试时换一个 `-bios` 是正当需求,而删掉一个已发布的键是破坏性变更 |

---

## 10. 不在本阶段

| 项 | 去向 |
|---|---|
| **第二块板 / ARM Cortex-M** | R-9 要求 rv32 作为 N3 的证据;真正的 ARM 支持另立 |
| **E-STD S-3**(`format` / `sort` / `string` 全功能) | 需为目标编 libc++ |
| **烧录 / OTA** | 设备侧的事,mcpp 不碰 |
| **D 档**(openkal / openhal / openarch) | 路线图在第一阶段计划 §7 |

---

## 11. 实施记录:两条计划主张被同一轮实测推翻

§4 的裸机 `mcpp test` 设计建立在两个**我没测就写下的数字/断言**上。两个都是错的。

| 计划写的 | 实测 | 后果 |
|---|---|---|
| 「qemu 冷启动 ~0.4s ⇒ 30 用例 isolated 要 12s」⇒ **默认 batch** | **12ms**(5 次连跑 63ms) ⇒ 30 用例 **0.36s** | ⛔ **batch 的全部理由消失**;连带「超时后重跑一次 isolated 定位」也不需要 —— isolated 本来就指名道姓 |
| 「裸机没有退出码回宿主 ⇒ 结果要走结构化 stdout 通道」 | **semihosting 把固件 `main` 的返回值原样传给 qemu 退出码**(`return 7` → 退出码 7) | ⛔ **不需要新通道**;「退出码即判据」在裸机上原样成立 |

⇒ **R3 从一个子系统缩成一件事**:让测试二进制**走 `mcpp run` 用的同一个 runner**。
实测结果:

```
ok_one ... ok (0.02s)
ok_two ... ok (0.02s)
deliberate_fail ... FAIL (exit 1, 0.02s)
error: test result: FAILED. 2 passed; 1 failed
```

三条判据(全过 · 失败被点名 · 退出码非零)一次全中,而 §4.2 表里的六个设计决定
**有四个根本不需要做**。

⚠️ **教训与本轮其它几次同形**:计划里凡是带具体数字或「A 不成立所以要 B」的句子,
数字与断言本身就是**必须先测的探针**。这两条的代价是我差点实现一整个 batch 子系统。

### 11.1 R1/R4 的实施结论

- **R1 按设计落地**(新 `Slot::Runner` + 新 `Scope::RunGlobal` + 一行表项),三侧已验:
  BSP 供 ⇒ 消费者无 `[target.*]` 也能跑 · 两个依赖都供 ⇒ **硬错误并点名两个** ·
  消费者写了 ⇒ 以它为准并说明覆盖了谁。
- **R4 按设计落地**,包括计划里点名的 R-5:`.bin` 是以 `.elf` 为输入的**独立 ninja 边**,
  改一行源码后**内容**变(不是只有 mtime 变)。
- ⚠️ **一次为了「一致性」而制造的回归,被 CI 抓回来。** `mcpp run` 用
  `--target-triple` 而其余子命令用 `--target`,看起来像我造成的不一致,于是我给
  `run` 也加了 `--target`。**parser 把选项和位置参数按同一个词索引**,于是:

  ```
  $ mcpp run q
  error: unknown target 'q'          ← q 是二进制名,被当成了目标三元组
  ```

  `e2e/73` 立刻红。而位置参数上**原本就有一条注释写着这个碰撞** —— 我为了对称把它
  覆盖了。

  但**第一版修法(`run` 退回只认 `--target-triple`)也不对** —— 它把「不能坏」买回来了,
  代价是留下一条真实的不一致。**真因不是这个词有两个轴,而是位置参数的名字选错了**:
  `ParsedArgs::value()` 在选项未设置时会**按同名回落到位置参数**,而这个位置参数的名字
  `cmd_run` 从来不读(它按下标取 `positional(0)`),只出现在 `--help` 里。
  ⇒ **把位置参数改名 `bin`**(在 `--help` 里本来就更准),`--target` 就自由了。
  `--target-triple` 作为 2026.8.19.1 已发布的拼写保留为别名。
  **一致性是判据之一,它排在「不能坏」后面 —— 但排在后面不等于要放弃。**

### 11.2 R2/R5 的实施结论:两条计划主张又被推翻

#### ⭐ R5:`mcpp new --template baremetal-riscv` 不该由 mcpp 提供

§6 写的是给 mcpp 加一个内建模板。**读代码发现这条路是被刻意关掉的**:

```cpp
// src/cli/cmd_new.cppm
//   builtin registry (frozen: bin; gui = transitional alias), else a
//   package template: [ns.]pkg | [ns.]pkg:tmpl | [ns.]pkg@ver:tmpl.
```

⇒ **模板随提供它的包走**(封闭语法 / 开放词汇),而且 `mcpp new` 会把自依赖按
**它解析到的版本**写进生成的 manifest —— 模板因此**不可能与库脱节**。

落地形态因此变成:`riscv-virt-rt` 里加 `templates/blinky/`,用户敲

```bash
mcpp new blinky --template riscv-virt-rt
```

**mcpp 侧零改动**。这比原计划好在:模板与板级包同一版本、同一仓库、同一次发布。

#### ⚠️ R2:包的 `[xlings] deps` **不会**到达消费者 —— 少了这条边包是废的

`mcpp` 只为**根工程**(或 workspace)物化 `[xlings]`(`prepare.cppm` 的
`runtimeOwnerManifest`)。BSP 自己声明的 `xim:picolibc-riscv` / `xim:qemu-riscv`
在被当依赖用时**一个都不装** —— 用户 `mcpp add` 之后拿到的是一个没有 libc 也没有
模拟器的板级包,直到 `build.mcpp` 才报出来。

解法**不在 mcpp 里**:xim 的描述符本来就有安装期依赖边,写在 `xpm.<平台>.deps`
(`libpng.lua` 早就这么用)。⇒ 描述符里加

```lua
deps = { "xim:picolibc-riscv@1.8.12", "xim:qemu-riscv@9.2.4-1" },
```

**实测判据(必须是「拿走再装回来」,不能只看装好的机器)**:把 store 里的
`xim-x-picolibc-riscv` 改名藏起来 → `mcpp add` + `mcpp build` **把它装了回来并链接成功**;
不加这条边则停在 `build.mcpp` 说「declared in [xlings].deps but is not installed」。

#### ⚠️ Form A vs Form B:选错的话包会「解析成功、编译成功、链接到空气」

`riscv-virt-rt` 带 `build.mcpp`,而 mcpp 在**包根**找它。Form B(内联 `mcpp = {...}`)
把包根留在解包目录,tarball 的 `riscv-virt-rt-<v>/` 包裹层由每条 glob 的 `*/` 吸收
⇒ **`build.mcpp` 落在下一层,找不到**。Form A(`mcpp = "*/mcpp.toml"`)把包根移进包裹层。
⇒ **凡是带 `build.mcpp` 的包必须用 Form A。**

#### 兼容性:类型化 API 没有语言内的特性探测(见 §3.2 的补记)

包侧无解 ⇒ 补在引擎侧的编译失败诊断上,对**以后每一次**类型化 API 新增都生效。

### 11.3 ⚠️ 我在第二阶段留下了一条**指向不存在的包**的诊断

W6 的 `import std` 诊断里写着可以直接粘贴的:

```toml
[dependencies]
mcpplibs.std.freestanding = "0.1"
```

**这个包没有发布,而且本阶段也不该顺手发**(E-STD-1/E-STD-2 各自是 M 规模:
`-nostdinc++` 之下 libc++ 的头一个都用不了,子集要自己实现;而且往 `namespace std`
里加声明本身就是另一个问题)。

⇒ 用户照着诊断粘完,下一条命令报「package not found」。**一条修不好问题的诊断,
比一条只解释不给命令的诊断更糟** —— 它让读者接下来几分钟在怀疑自己的索引坏了。

已改成指向**今天确实存在**的东西(板级包导出的模块,点名 `mcpplibs.riscv_virt_rt`),
并把子集包描述成一个**形态**而不是一行可粘贴的依赖,末尾明说「还没有这样的包」。
**E-STD 仍然开着;它发布时要把这条诊断改回具体的一行。**

⚠️ 一般化:**诊断里的每一条建议都是一个承诺**,而承诺是要被兑现的。写「加这一行」
之前必须先确认那一行今天能跑通 —— 这条和 [[issue427-absence-treated-as-contradiction]]
里那条错误建议是同一形状。

### 11.4 ⚠️ 2026.8.19.2 发出去就带着一个 bug:`mcpp build` 之后 `mcpp run` 直接执行裸机 ELF

**发布后**用发布的二进制做真实验证时才发现 —— 这正是「本地真实验证」这一步存在的理由。

```
$ mcpp new blinky --template riscv-virt-rt && cd blinky
$ mcpp run                     # ✅ 走 qemu,打印正常
$ mcpp build && mcpp run       # ❌ Running `target/riscv64-none-elf/…/bin/blinky`
                               #    没有模拟器、没有输出、exit=1
```

**根因**:`try_fast_run` 直接 exec 缓存里的产物,它的注释写着
「`mcpp run` never takes a --target flag」所以只匹配 `targetTriple == ""` 的条目。
调用点确实挡住了 `--target` 旗标 —— 但**工程可以把目标写在 manifest 里**
(`[build] target`),而这个拼写**从来没进过缓存键**:交叉构建写下的条目 key 是 `""`,
读回来就被当成宿主构建。

⇒ 修法是把这个函数的**前置条件**写成代码:它 exec 产物本身,所以只在产物属于**本机**时
才成立。声明了默认目标就一律退回完整 prepare(runner 正是在那里解析的)。

#### ⚠️ 第一版回归测试是假绿 —— 而且我差点就信了

把 build-then-run 加进 e2e 131 的既有工程后,**关掉修复它照样通过**。
探针(给 `try_fast_run` 的 16 个 `return nullopt` 各打一个编号)指出是 **BAIL 11**:
`mcpp.toml` 比 `build.ninja` 新,快路径**本来就没被走到**。

原因很反直觉:**重建并不会重写内容未变的 `build.ninja`**,所以在一个「原地编辑过
manifest」的目录里,manifest 永远是最新的那个文件,快路径**永久关闭**。
⇒ 测试必须**新建一个干净工程**。改完后先确认它在关掉修复时**变红**,再确认修复后变绿。

**教训**:`mcpp run` 单独跑是对的,`mcpp build && mcpp run` 才错 —— **顺序本身就是被测
对象**。而 130/131/132 三个测试都恰好先 `run`,所以谁也看不见。
