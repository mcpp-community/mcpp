# 目标侧来自依赖图之后:七项优化方案

2026-08-24。本文由一次真实使用暴露的十条问题写成 —— 一个 hello-world 工程,
`[dependencies]` 里只加了一行 `openkal-llvm-runtime`,随后在四个目标上连续失败。

**基准版本**:`origin/main` = `2026.8.24.1`(含 `01d6cef`「Resolve the target side
once, per layer, after the dependency graph is known」)。本文所有行号指该版本。

**本文的每一条根因都读过码,每一条性能数字都实测过。** 实测环境:Linux x86_64,
`llvm@22.1.8`,工程 `test3`(1 个 `src/main.cpp`,1 条依赖)。

---

## 0. 一句话论断

`01d6cef` 把「目标侧从哪来」这个问题**收敛到了一处解析**,但它的**五个消费者**
仍然停在旧模型上:

| 消费者 | 仍然假设 | 后果 | 节 |
|---|---|---|---|
| 三元组的命名语义 | `env` 段说明 C 库是谁 | **名字说 `-gnu`,事实是 musl** | §4 |
| 工具链的命名空间 | 目标侧策略可以当编译器族 | `openkal-llvm` 叫你去装一个已装的东西 | §4.4 |
| 工具链轴(选哪个编译器) | 由载荷矩阵决定,图解析之前就知道 | gcc 去编 libc++ 的 std 模块 | §2 |
| 目标词表(哪些三元组存在) | 存在 ⇔ 本机有载荷伺候 | `x86_64-windows-musl` 被拒,而它能跑 | §3 |
| flag 派生(发什么 flag) | 判据是目标格式 | clang-only flag 递给 mingw g++ | §2 |
| 包的 `cfg` 谓词 | 同第一行 | Win32 导入库整组丢失 | §4B |
| 缓存身份 | 键可以含绝对路径与三元组拼写 | 同一份产物存了六份 | §6 |

⚠️ 这不是七个 bug,是**一次模型迁移只做了一半**。逐条修会把同一句话说七遍;
本文按「让每个消费者也去问那一处」来组织。

⭐ **§4 是根**:三元组把「C 库是谁」编进了名字,而 openkal 把它交给了图。
§2、§3、§4B 都是这一条的下游。

⭐⭐ **§12 是这件事的全景**,并且它有一份现成的对照实验:
`mcpp#492` + `xim-pkgindex#677` 正在用**载荷模型**做 openkal 用**图模型**
做过的同一件事 —— 349 行引擎改动 vs 0 行。读 §12 之前先读 §4。

⚠️ 另有两项与 openkal 无关、但被同一次使用暴露的**构建性能缺陷**(§5、§6),
它们的量级比上面五条都大。

---

## 1. 实测数据(先摆事实)

### 1.1 一次全缓存命中的 clean build:1.81 s

`mcpp clean && mcpp build`,逐行打时间戳:

```
     0 ms | Resolving toolchain
     1 ms | Resolved llvm@22.1.8 → …/xim-x-llvm/22.1.8/bin/clang++
    53 ms | [VERBOSE] probe: payload paths: …            ← 编译器探测 50ms
    57 ms | [VERBOSE] index: openkal-llvm-runtime@0.1.1: resolvable locally
   261 ms | build.mcpp compiling                          ← ⚠️ 204ms 完全静默
   311 ms | build.mcpp running
   335 ms | Target x86_64-unknown-linux-gnu
   348 ms | [VERBOSE] scan: scanning module sources
   736 ms | Inferred sources […]                          ← ⚠️ 388ms 扫描+校验+plan
   740 ms | Cached openkal-llvm-runtime v0.1.1 (243 units)
  1075 ms | [VERBOSE] build/stage: compile-commands: 333ms ← ⚠️
  1747 ms | [VERBOSE] build/stage: ninja: 662ms            ← ⚠️
  1833 ms | [VERBOSE] build/stage: loader-tags: 37ms
```

对照:
- no-op build(fast path 命中):**35 ms**
- 冷构建(缓存未命中):**4.9 s 墙钟 / 44 s CPU**
- `--offline` 与联网**逐毫秒相同** ⇒ 稳态无网络开销

### 1.2 构建图的形状

```
$ grep -oE "^build [^:]*: [a-z_]+" build.ninja | awk '{print $NF}' | sort | uniq -c
   1624 stage_file
      1 phony
      1 cxx_scan
      1 cxx_object
      1 cxx_link
      1 cxx_dyndep
```

**1629 条边里 1624 条是 `stage_file`**,而 `stage_file` 的 rule 是:

```ninja
rule stage_file
  command = $mcpp stage $verify --output $out $in
```

即**每拷贝一个文件启动一次 mcpp 进程**。实测单次进程启动 ~2.1 ms
(200 次 `mcpp --version` = 0.428 s)⇒ 1624 × 2.1 ms ≈ **3.4 s CPU**,
并行摊到多核后就是那 662 ms。核心数越少越难看。

### 1.3 `compile_commands.json`

一个只有 **1 个源文件**的工程,产出 **3235 条、8.4 MB**,每次构建全量重写,
耗时 **333 ms**(占墙钟 18%)。3234 条是用户永远不会打开的依赖 TU。

### 1.4 全局缓存的实际状态

```
build-cache/v1   12 GB      pkg 7.5 GB (395 条)   std 3.9 GB (136 条)   tool 48 MB
```

⚠️ **缓存本身是好的**:`mcpp clean` 后重建打印
`Cached openkal-llvm-runtime v0.1.1 (243 units)`,1.02 s 完成。
用户感受到的「每次都重编」是**键在碎**,不是没缓存。

碎在哪(实测,取 `target_triple = x86_64-unknown-linux-gnu` 的 25 条 std 条目
逐字段做直方图):

| 字段 | 不同取值数 | 说明 |
|---|---|---|
| `std_module_source_hash`(内容) | **2** | 真正的身份 |
| `std_module_source`(绝对路径) | **6** | ⚠️ 冗余,且它在碎键 |
| `compiler_version` | 2 | 合理 |

六个路径分别是:本地开发 checkout、git clone、registry 0.1.0、registry 0.1.1、
llvm 载荷 22.1.8、llvm 载荷 20.1.7 —— **其中四个的 std.cppm 内容完全相同**。

同一目标的两套拼写也各自成条目:

```
25 条  x86_64-unknown-linux-gnu        18 条  x86_64-windows-gnu
17 条  x86_64-linux-gnu                 1 条  x86_64-w64-mingw32
                                        1 条  x86_64-windows-musl
```

⭐ `x86_64-windows-musl` 与 `x86_64-windows-gnu` 的两条 std 条目
**除 triple 字段外逐字段相同、各占 32.7 MiB** —— 这是本文 §3 的直接证据:
两个三元组交给编译器的是**同一个 LLVM triple**。

pkg 侧同样:`mcpplibs.xpkg@0.0.57` × 14、`compat.gtest@1.15.2` × 11、
`compat.zlib@1.3.2` × 10、`compat.mbedtls@3.6.1` × 9。

另有 **44 条 `(incomplete)` 0 字节条目**,最老 12 天,从不回收。

---

## 2. 【架构·高】目标侧是四层,不是三层 —— 编译器必须进 `TargetSide`

### 2.1 成因

`src/targetside/model.cppm:224-244` 的 `Inputs` **没有编译器这一项**。
`resolve()` 因此无法表达「这份 C++ 运行时能不能被这个编译器消费」。

`check_layering()`(:309)只守一个方向:

```cpp
if (ts.cxx.origin == Origin::Payload && ts.cAbi.origin != Origin::Payload && …)
```

即「载荷的 C++ 运行时 × 非载荷的 C 库」。用户的情况是
`cxx.origin == Graph`,直接返回 `nullopt`。

而这条约束**在代码里写下来了,没有任何东西执行它**
—— `src/build/prepare.cppm:1522` 的注释原文:

> Measured 2026-08-23: `--target x86_64-windows-gnu` with an explicit `llvm@22.1.8`
> resolved `x86_64-w64-mingw32-g++`, and **gcc cannot compile libc++'s std module**.

⚠️ 这是这个仓库反复付过学费的形状:**结论会被复查,理由不会**;
**写在注释里的约束没有任何东西在执行它**。

近因还有一层,可定位到一行:`src/toolchain/gcc.cppm:159`
`std_module_build_command()` 拼命令时**从不读 `tc.stdModuleFlags` /
`tc.stdModuleTargetFlags`**,只有 `clang.cppm:205/360` 读。于是
`prepare.cppm:5872-5884` 辛苦收集的包侧 `-isystem` / `-idirafter` /
defines / `--target=` 在 GCC 后端上全部落地即丢。用户看到的命令行
一个 `-isystem` 都没有,正是这个。

### 2.2 设计

给 `CapLayer` 加第四项 `Compiler`,并**引入与 `provides` 对称的 `requires`**:

```toml
# openkal-llvm-runtime/mcpp.toml
provides = ["mcpp:c++-abi=libc++"]
requires = ["mcpp:compiler=llvm"]
```

⭐ 判据不变:**mcpp 硬编码层名,永不硬编码实现名**。`llvm` 出现在包的
manifest 里,不出现在引擎里 —— 这正是 `targetside/model.cppm:127-138`
自己立的规矩。`parse_capability` 已经是这个形状,加一个 `else if` 即可。

`Inputs` 加一个 `std::string compilerFamily`;`resolve()` 填 `ts.compiler`;
`check_layering()` 增加一条:图供给的层若声明了 `mcpp:compiler=X` 而当前
编译器族不是 X,拒绝并说明。

### 2.3 实施步骤

1. `targetside/model.cppm`:`CapLayer::Compiler`、`Layer compiler`、
   `Inputs::compilerFamily`、`resolve()` 一分支、`check_layering()` 一条。
2. `manifest/xpkg.cppm`:解析 `requires` 数组,复用 `parse_capability`。
   ⚠️ **未知键必须被忽略而不是让整份 manifest 加载失败** —— 见
   [`#359 provisions-reexport`] 的教训:老客户端遇到新键会整包解析失败。
3. `prepare.cppm:5495-5519`:填 `in.compilerFamily`。
4. `gcc.cppm:159`:消费 `tc.stdModuleFlags`(即使 §2.2 会拒绝这个组合,
   这条对未来「图供给 libstdc++」仍然必要 —— 现在是静默丢弃)。
5. `tests/unit/test_targetside.cpp`:表驱动新增 4 行
   (gcc×图libc++ / clang×图libc++ / gcc×载荷libstdc++ / 图无 requires)。

### 2.4 判据

- `mcpp build`,默认 gcc,依赖含 `openkal-llvm-runtime` ⇒ 在**编译开始之前**
  报出「这份 C++ 运行时需要 llvm 族编译器」,并给出可粘贴的修法。
- 单元测试**不启动任何构建**即可断言该矩阵(这是 `targetside` 模块被拆出来的
  原因:「everything here can be asserted from a table」)。

### 2.5 刻意不做

不在引擎里写 `if (stdlib == "libc++" && compiler == gcc)`。那是把产品名放进引擎,
`fam == "openkal-llvm"` 已经因此被删过一次。

---

## 3. 【架构·高】目标词表拆成两张:能力表 vs 载荷表

### 3.1 成因

`src/toolchain/triple.cppm:171` 的 `kKnownTargets` 同时承担两个含义:

1. 「mcpp 认识这个三元组」
2. 「本机有载荷能产出它」

`prepare.cppm:1405` 的硬拒用的是第 1 个含义,而表的内容是按第 2 个含义填的。
于是 `x86_64-windows-musl` 被拒 —— 尽管:

⭐ **实测:它能跑。** 按报错自己给的逃生口写
`[target.x86_64-windows-musl] toolchain = "llvm@22.1.8"`,mcpp 全部编译通过,
只在链接期缺四个导入库(§4)。补上后:

```
Finished dev in 2.75s
test3.exe: PE32+ executable (console) x86-64, 14 sections
imports: ntdll.dll / api-ms-win-core-synch-l1-2-0.dll / SHELL32.dll / KERNEL32.dll
wine test3.exe → Hello from test3!
```

而且两个三元组交给编译器的是**同一个 LLVM triple**:

```
Target x86_64-windows-gnu  → x86_64-w64-windows-gnu
Target x86_64-windows-musl → x86_64-w64-windows-gnu
```

⚠️ 同一文件里,**这个问题的另一半已经学会了推迟**:`prepare.cppm:1439-1454`
把「本机载荷伺候不了」的拒绝推迟到图已知之后,理由写得很清楚 ——
「a dependency can supply the target's platform interface and C library, and the
dependency graph does not exist yet at this line」。**「未知目标」这条是同一句话,
却还在图之前开火。**

附带缺陷:`did_you_mean`(:518)是纯 Levenshtein。`musl → msvc` 距离 3,
预算 `max(2, 19/4) = 4` ⇒ 建议了**唯一一个 C 库与 ABI 都相反**的目标。

### 3.2 设计

- **能力**:`triple::parse()` 已经是答案。`x86_64-windows-musl` 解析成功。
  解析失败(`x86_64-linuxx-gnu`)仍然立刻拒 —— 打字错误这条守住了。
- **可服务性**:载荷 **或** 图,只有解析之后才知道。
- `kKnownTargets` 退回它真正擅长的两列:**约定 pin** 与 **默认链接方式**。
  没有行 = 没有约定,而这对图供给的目标恰好是正确语义。

`--target <可解析三元组>` 一律放行;把 `unservedTargetDiagnosis` 的
`known` 前置条件去掉,让「无行」也走同一条推迟路径。

⭐ 这一处改动同时消掉:未知目标硬拒、错误的 did-you-mean、以及
「为 windows-musl 加一行」的必要性。并且它泛化 —— `aarch64-linux-musl`、
`riscv64-linux-musl`、`x86_64-macos` 在图能伺候它们的那天自动可达,
不需要再改表。

### 3.3 实施步骤

1. `prepare.cppm:1400-1411`:未知但可解析 ⇒ 不再 `return std::unexpected`,
   改为写入 `unservedTargetDiagnosis`。
2. `prepare.cppm:1459`:去掉 `known &&` 前置条件。
3. `triple.cppm:518` `did_you_mean`:输入可解析且 `env` 是已知拼写时,
   **不建议 `env` 不同的目标**。
4. `mcpp toolchain list`:Targets 表加 STATUS 值 `graph`(见 §7.2)。

### 3.4 判据

- Linux 上 `mcpp build --target x86_64-windows-musl`,工程依赖含 openkal ⇒ 构建成功。
- Linux 上 `mcpp build --target x86_64-macos`,工程**不含** openkal ⇒ 仍然拒绝,
  且报错里出现「nothing in the dependency graph supplies its system side」。
- `mcpp build --target x86_64-linuxx-gnu` ⇒ 立刻拒(解析失败)。

### 3.5 刻意不做

不给 `kKnownTargets` 加 `x86_64-windows-musl` 行。加一行只解决一个三元组,
而问题是这张表被当成了两张表用。

---

## 4. 【架构·最高】三元组与工具链的语义:名字在说谎

用户原话:「工具链和 target 在语义设计方面也有点乱,例如 `-gnu` 但是却是 `-musl`」。

这一条是本文里**最根本**的一条 —— §2、§3 都是它的下游。

### 4.0 现象:名字与事实矛盾,而 mcpp 把两者并排打印出来

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu
       kernel-abi  openkal   (openkal-windows@0.1.3, graph)
       c-abi       musl      (openkal-musl@0.3.3, graph)      ← 名字说 gnu
```

⚠️ **这不是 Windows 特有的。** 同一工程在宿主上:

```
Target x86_64-unknown-linux-gnu
       c-abi       musl      (openkal-musl@0.3.3, graph)      ← 同样说谎
```

### 4.1 成因:三元组用 3 个字段承载 4 条正交的轴

真正正交的是四条:

| 轴 | 取值 | 谁决定 |
|---|---|---|
| 1 机器 | x86_64 / aarch64 / riscv64 | 用户 |
| 2 平台接口 | linux / windows / macos / none | 用户 |
| 3 对象格式 + 调用 ABI | ELF-SysV / PE-GNU / PE-MSVC / Mach-O | 用户 |
| 4 C 库(及其上的 C++ 运行时) | glibc / musl / picolibc / 无 | **载荷 或 图** |

`<arch>-<os>-<env>` 只有三个字段,于是 `env` 一人干两份活:

- Linux 上 `env` 指**第 4 轴**(`gnu` = glibc,`musl` = musl)
- Windows 上 `env` 指**第 3 轴**(`gnu` = PE/GNU ABI,`msvc` = PE/MSVC ABI)

这个重载是从 GNU/LLVM 三元组继承来的,在载荷模型下一直凑合能用 ——
因为第 4 轴由载荷决定,而载荷是按三元组选的,所以名字碰巧总是对的。

⭐ **openkal 把第 4 轴交给了图,重载就此破产**:名字仍然按第 4 轴拼写,
而第 4 轴已经不由名字决定了。

### 4.2 根因可以定位到一行

`src/toolchain/triple.cppm:514`:

```cpp
if (t.os == "linux" && t.env.empty()) t.env = "gnu";   // "x86_64-linux" alias
```

⭐ **「env 未指定」这个状态在解析时就被销毁了。** 用户写 `x86_64-linux`
(意思是「C 库谁供给谁说了算」),`parse()` 把它改写成 `x86_64-linux-gnu`
(意思是「我要 glibc」),然后图供了 musl,报告就只能自相矛盾。

⚠️ 而**这个仓库已经知道正确形状**:`[target.X].sysroot` 就是标准的三态,
理由写在 `triple.cppm:207-217`:

> `nullptr` -> the project said nothing; the target table's column applies
> `"xim:..."` -> the project named a different C library
> `""` -> the project asked for NO C library

`env` 需要的是同一个三态,而 :514 把第一态折叠进了第二态。

旁证:**macOS 已经是对的**。`:513` `if (t.os == "macos") t.env.clear();`
—— macOS 的三元组本来就没有 env 段,而它工作得很好。空 env 是**已被验证
可行的状态**,不是新发明。

### 4.3 设计:三元组是「请求」,`TargetSide` 是「事实」

**规则一:`env` 空 = 未指定,不再自动填充。**

- `x86_64-linux` = 「C 库由载荷或图决定」← 图模式下这是**正确写法**
- `x86_64-linux-gnu` = 「我要求 glibc」
- `x86_64-linux-musl` = 「我要求 musl」
- Windows 同理:`x86_64-windows` = 「ABI 由供给方决定」

⚠️ 兼容性:`x86_64-linux-gnu` 在**没有图供给**时行为必须逐字节不变
(仍然是 glibc 载荷)。这条改的是「空」的含义,不是「非空」的含义。

**规则二:请求与事实冲突 ⇒ 拒绝,而不是并排打印。**

```
error: this build requests the `gnu` C ABI, and its dependency graph supplies `musl`.
         requested   gnu     (from --target x86_64-linux-gnu)
         resolved    musl    (openkal-musl@0.3.3, graph)
       A C library is not interchangeable at this seam: the C++ runtime above it
       was configured against one of them.
       Write `--target x86_64-linux` to let the graph decide, or remove the
       package that supplies the C ABI.
```

⭐ 判据方向与 §3 相反,而这是刻意的:§3 是「不可证伪就放行」(未知目标推迟
到图已知),这里是「已经证伪就拒绝」(名字和事实都已知且矛盾)。

**规则三:报告里标出请求,只在它与事实不同时。**

```
c-abi       musl    (openkal-musl@0.3.3, graph)
```
未指定时如上;若显式请求且一致,加 `, as requested`;不一致则走规则二。

**规则四:`cfg()` 按事实求值,不按名字。**

包想问的是「C 库是不是 musl」,现在只能问 `env == "musl"`,而在图模式下
那个字段已经不回答这个问题。增加 `cfg(c-abi = …)` / `cfg(kernel-abi = …)`
/ `cfg(c++-abi = …)`,由 `TargetSide` 求值。

⚠️ **时序限制,必须显式拒绝而不是留给用户去撞**:`cfg()` 现在在依赖解析
**期间**求值,`TargetSide` 在解析**之后**才有。新三个维度只能用于 `[build]`
段(flags / ldflags / defines),**不能用于 `[dependencies]`**,否则成环。
解析器要为此报一条专门的错。

### 4.4 工具链侧的同一个病:`openkal-llvm` 不是一个编译器族

`src/toolchain/registry.cppm:46`

```cpp
enum class Family { Gcc, Llvm, Msvc, OpenkalLlvm };
```

而 :411 自己说明:

> ⭐ **THE SAME PAYLOAD.** `openkal-llvm` downloads nothing of its own and
> installs nothing of its own — it is **a statement about where the TARGET SIDE
> comes from**, and the compiler is the llvm payload either way.

⇒ 一个**目标侧策略**被塞进了**编译器族**的命名空间。后果直接可见:

```
Available toolchains (run `mcpp toolchain install <family> <version>`):
     openkal-llvm 22.1.8 / 20.1.7          ← 叫你去装一个已经装好的东西
```

`available_toolchain_indexes()`(:623-631)把同一个 llvm 包**列了两次**,
一次记 `Family::Llvm` 一次记 `Family::OpenkalLlvm`;而 `toolchain list` 的
「已装」判定比的是 `f.family == idx.family`(`lifecycle.cppm:632`),
于是永远认为 `openkal-llvm` 没装。

**修法**:

- `Family` 回到三个:`Gcc / Llvm / Msvc`。
- `openkal-llvm` 降级为**纯拼写别名** → `llvm`。
  ⚠️ 已有 e2e `269_openkal_llvm_spelling_still_resolves.sh` 守这条,别删。
- 目标侧从哪来**不再由工具链名字表达**,它由图表达 —— 那本来就是
  `01d6cef` 的结论,只是命名空间没跟上。
- Available 段按**载荷包名**去重,不按 Family。

### 4.5 实施步骤

1. `triple.cppm:514`:删掉自动填充;`Triple::env` 的注释改写为「请求,空 = 未指定」。
   ⚠️ 全仓库搜 `env.empty()` 与 `is_musl()` / `is_windows_gnu()`,
   逐处判断「空」该走哪支 —— 这是本节风险最大的一步。
2. `targetside/model.cppm`:`Inputs` 加 `requestedCAbi`;新增 `check_request()`。
3. `prepare.cppm`:在 `check_layering` 旁边调用 `check_request()`。
4. `manifest/types.cppm`:`cfg()` 语法加三个维度 + `[dependencies]` 里的拒绝。
5. `registry.cppm`:`Family::OpenkalLlvm` 删除,`openkal-llvm` 走别名表。
6. `lifecycle.cppm:623-631`:Available 段按包名去重。

### 4.6 判据

- `mcpp build --target x86_64-linux`,依赖含 openkal ⇒ 成功,
  报告写 `c-abi musl (openkal-musl@…, graph)`,**不出现 `-gnu`**。
- `mcpp build --target x86_64-linux-gnu`,依赖含 openkal ⇒ **拒绝**,
  报错含「requested gnu / resolved musl」。
- `mcpp build --target x86_64-linux-gnu`,**不含** openkal ⇒ 与今天逐字节相同。
- `mcpp toolchain list`:装了 llvm 时,`openkal-llvm` **不出现**在 Available。
- `mcpp toolchain default openkal-llvm@22.1.8` ⇒ 仍然解析成功(别名)。

### 4.7 刻意不做

**不发明新的三元组语法**(不搞 `x86_64-linux+musl` 之类)。三元组是用户会
打字的东西,也是目录名和 `cfg` 的 key;它的价值在于和 LLVM/GNU 的写法互认。
本节只做两件事:**恢复「空」这一态**,以及**把事实从名字里移到 `TargetSide` 里**。

**不改 `llvm_triple()` 的输出。** 交给编译器的仍然是四段 LLVM 拼写;
`x86_64-windows-gnu` 与 `x86_64-windows-musl` 今天就都翻译成
`x86_64-w64-windows-gnu`(实测),这是对的 —— 第 4 轴本来就不该进
编译器的 triple。

---

## 4B. 【跨平台·高】包的 cfg 谓词是上一节的直接受害者

### 4B.1 成因(这是本次最干净的一条)

两份 `build.ninja` 的 `ldflags` 逐 token 对比,差别**只有四个**:

```
windows-gnu :  … -Wl,-e,okw_start -lntdll -lsynchronization -lshell32 -lkernel32 -L…
windows-musl:  … -Wl,-e,okw_start                                                -L…
```

来源是包侧,`openkal-windows/mcpp.toml:41`:

```toml
[target.'cfg(all(windows, env = "gnu"))'.build]
ldflags = ["-lntdll", "-lsynchronization", "-lshell32", "-lkernel32"]
```

包的注释说明了意图:这四个是为了区分「GNU/PE ABI」与「MSVC ABI」
(「the two application binary interfaces this environment has spell a library
differently」)。但它用 `env = "gnu"` 当代理。

⭐ **这四个是 Win32 导入库,是 kernel-abi 的性质,不是 C 库的性质。**
而 mcpp 自己在 `x86_64-windows-gnu` 上打印的就是:

```
kernel-abi  openkal   (openkal-windows@0.1.2, graph)
c-abi       musl      (openkal-musl@0.3.2, graph)
```

**三元组说 gnu,实际 C 库是 musl。三元组在说谎,而包的 cfg 在信它。**

### 4B.2 设计:落在 §4.3 规则四上

包想问的是「C 库是不是 musl」/「对象 ABI 是不是 MSVC」。今天它只有
`env` 一个字段可问,而那个字段在图模式下已经不回答第一个问题。

⇒ 修法就是 §4.3 的规则四:`cfg(c-abi = …)` / `cfg(kernel-abi = …)` /
`cfg(c++-abi = …)`,由 `TargetSide` 求值。

落点是现成的:`cfg()` 的求值输入**已经是「解析后的 target」而不是 host**,
把 `TargetSide` 的三层灌进求值环境即可。

⚠️ 时序限制见 §4.3 规则四:新维度只能用于 `[build]` 段,
`[dependencies]` 里必须显式拒绝而不是静默为假。

⚠️ **`env` 本身仍然保留**,因为第 3 轴(对象格式 + 调用 ABI)确实需要一个
字段,而 `gnu`/`msvc` 在 Windows 上一直就是这个意思。变的是:
**Linux 上的 `env` 不再是 C 库的事实来源** —— 它只是一个请求(§4.3 规则一)。

### 4B.3 立即可做(不依赖引擎)

`openkal-windows` 的 cfg 改成:

```toml
[target.'cfg(all(windows, not(env = "msvc")))'.build]
```

⭐ **已实测**:单 token 改动后 `x86_64-windows-musl` 全绿(§3.1 的产物即出自此)。
`not()` 在当前 cfg 语法里已支持(`docs/05-mcpp-toml.md:849`)。
这是包侧 PR,提到 `mcpplibs/openkal-windows`,不排队等引擎。

### 4B.4 判据

- 引擎侧:`cfg(c-abi = "musl")` 在 `x86_64-windows-gnu`(图供 musl)上求值为真。
- 引擎侧:`cfg(c-abi = …)` 出现在 `[dependencies]` 里 ⇒ 明确报错,不是静默为假。
- 包侧:`--target x86_64-windows-gnu` 与 `--target x86_64-windows-musl`
  产出的 `ldflags` 逐 token 相同。

---

## 5. 【性能·最高】staging:1624 条边 → 每包一条

### 5.1 成因

`stage_file` 成为独立 ninja 边是**正确的**,理由记录在 PR#317:ninja 判脏包含
「输出存在但 `.ninja_log` 无该输出的命令行记录 ⇒ dirty」,把 staging 藏在
`prepare_build` 里拷贝会让**缓存命中也 100% 重编**(那个假 `Cached` 骗了三个月)。

但「必须是一条边」不等于「必须是每文件一条边」。当前形态的代价:

- 1624 次进程启动 × 2.1 ms ≈ **3.4 s CPU**
- 构建图 1629 条边,其中 99.7% 是拷贝
- ninja 阶段 **662 ms**(本例 4 个包 / 243 units;真实工程会线性放大)

### 5.2 设计

**按包分组:一个包一条 `stage_file` 边,多输出。**

```ninja
rule stage_pkg
  command = $mcpp stage --manifest $out.rsp
  rspfile = $out.rsp
  rspfile_content = $in_newline
  restat = 1
```

保住的性质:
- ninja 仍有可比对的命令行记录(rspfile 内容进哈希)⇒ PR#317 的正确性依据不动。
- 粒度仍是「包」—— 缓存键变了就重放该包,这正是缓存条目的粒度,没有损失。
- `restat = 1` 保留(参与 [`issue311-bmi-staging`] 的 mtime 抑制)。

⚠️ **必须用 rspfile**:243 个文件的命令行会撞 Windows 的 `cmd.exe` 8191 上限
(#274 已经踩过)和 Linux 的 `MAX_ARG_STRLEN` 128 KiB(见
[`link-argv-max-arg-strlen`])。

⚠️ **order-only phony 前置不能动**:所有 staged 产物经一个 phony 成为非 staged
边的 `||` 前置,这是为了修「包内模块序丢失」(macOS/Clang 会挂在
`failed to find module file for module 'pkg:part'`,Linux 赢竞态所以本地全绿)。
分组不影响这条,但**改完必须在 macOS 上跑**。

### 5.3 更进一步:零拷贝

同一文件系统上用 hardlink / `FICLONE` reflink 代替字节拷贝,`stage` 的成本从
O(bytes) 降到 O(1)。⚠️ hardlink 会让缓存条目与 build dir 共享 inode ——
必须确认没有任何一步会**原地修改** staged 产物(`strip`、`elfpatch` 都会),
否则会污染缓存。安全形态:只对**不会被后续步骤改写**的产物(BMI)用链接,
对象文件仍拷贝;或统一改成 copy-on-write reflink。

### 5.4 判据

- 同一工程 `mcpp clean && mcpp build`,ninja 阶段耗时下降 ≥ 60%。
- `build.ninja` 边数从 1629 降到 ~10。
- ⚠️ **回归测试必须先 touch 源码或删产物**,否则工程级 fast path 会把改动
  整个遮蔽掉(这条已经骗过两次)。
- macOS + Clang 上跑一遍(模块序竞态只在那里输)。

---

## 6. 【性能·高】缓存身份:去掉路径,归一拼写,回收空壳

### 6.1 成因一:身份键含绝对路径

`src/toolchain/stdmod.cppm:157-172` 的 14 个身份字段里同时有:

```cpp
"std_module_source",        // 绝对路径     ← 冗余
"std_module_source_hash",   // 内容哈希     ← 真身份
```

实测(§1.4):25 条同 triple 的条目里,内容哈希 **2 个取值**,路径 **6 个取值**。
一个 33 MB 的 std 条目,仅仅因为同一份 `std.cppm` 换了个目录就重建一次。

同样的形状也在 `std_build_commands`(:171)里 —— 它内嵌包的每一条绝对
`-I` / `-isystem`。版本变了该失效(对),但**路径变了不该失效**。

### 6.2 成因二:三元组两套拼写各自成条目

```
25 条  x86_64-unknown-linux-gnu   vs   17 条  x86_64-linux-gnu
18 条  x86_64-windows-gnu         vs    1 条  x86_64-w64-mingw32
```

`j["target_triple"] = tc.targetTriple`(:134)存的是**当时手里那个拼写**,
没有先过 `triple::parse(...).str()`。

⭐ 已证:`x86_64-windows-musl` 与 `x86_64-windows-gnu` 的两条 std 条目
除 triple 字段外**逐字段相同**,各占 32.7 MiB。

### 6.3 成因三:44 条 0 字节 `(incomplete)` 条目

写入不是原子的:先建目录,产物落地失败就留下空壳,`gc` 也不清。

### 6.4 设计

1. **身份键去掉 `std_module_source` 与 `std_compat_source`**(路径),
   只保留两个 `_hash`。⚠️ 路径仍写进 metadata **供人阅读**,但不进
   `metadata_matches` 的 14 键列表。
2. **`target_triple` 进键前先归一**:`triple::parse(tc.targetTriple)->str()`。
   不可解析时保留原串(逃生口)。
3. **`std_build_commands` 归一化后再进键**:把已知的不可变前缀
   (`$MCPP_HOME/registry/data/xpkgs/`、`$MCPP_HOME/build-cache/`)替换成
   占位段。⚠️ 这与已有的 `kStdKeyPlaceholder` 自指解法同形,复用它。
4. **原子提交**:落到 `<key>.tmp-<pid>/` 再 `rename`。
   `gc` 无条件清理不含 metadata 的目录。
5. `pkg` 侧同样审一遍 `cache_key.cppm` 的输入里有没有绝对路径与拼写。

### 6.5 判据

- 本机 `mcpp cache list`:std 条目从 136 降到 ~40,`build-cache/v1/std`
  从 3.9 GB 降到 ~1.2 GB。
- 把 `openkal-llvm-runtime` 从 registry 换成同内容的 git clone 再构建
  ⇒ **命中**同一条 std 条目(现在是新建一条)。
- `--target x86_64-linux-gnu` 与 `--target x86_64-unknown-linux-gnu`
  命中同一条条目。
- 构建中途 `kill -9` ⇒ `cache list` 里不出现新的 `(incomplete)`。

### 6.6 刻意不做

不改 pkg 缓存的「每包 Merkle 键 + F 轴递归」策略。那条是对的,理由已经
用三组手工对照证过(BMI 轴取窄=编译器硬报错,`.o` 轴取窄=**静默错对象**
⇒ 必须取保守侧)。本节只清理键里**不该在的输入**,不动键的**语义范围**。

---

## 7. 【易用·高】让工具说出它做了什么决定

### 7.1.1 实测:全局默认确实被词表 pin 覆盖,而绕法要按目标写一遍

核实(2026-08-24):

```
~/.mcpp/config.toml        [toolchain] default = "llvm@22.1.8"
test3/mcpp.toml            [target.x86_64-windows-gnu] toolchain = "llvm@22.1.8"
```

⇒ 用户已经把同一个值**写了两遍**才让 `--target x86_64-windows-gnu` 用上 llvm。
第一遍(全局默认)被词表 pin 忽略,第二遍(`[target.X]`)才被认。

⭐ 这就是这条缺陷的实际代价:**绕法必须按目标重复一次**。四个目标就写四段,
而每一段写的都是「请用我已经设为默认的那个」。

### 7.1 `Resolved` 那行没说自己覆盖了谁

用户跑了 `mcpp toolchain default llvm`,mcpp 回了
`Default set to llvm@22.1.8 (was: gcc@16.1.0)`。随后
`mcpp build --target x86_64-windows-gnu` 打印 `Resolved gcc@16.1.0`,**不解释**。

成因:`prepare.cppm:433-444`

```cpp
GlobalDefault,   // config.toml [toolchain] default — mcpp's own default
inline bool tc_origin_is_user_explicit(TcOrigin o) {
    return o == TcOrigin::ManifestToolchain || o == TcOrigin::TargetSection;
}
```

⚠️ 注释里那句「mcpp's own default」**不成立**:枚举里 `FirstRun` 是单独一项,
所以 `GlobalDefault` 只可能来自用户主动执行的 `mcpp toolchain default`。
2026-08-23 那次修复(`pinWouldOverruleUser`,:1531)修的正是这条,但谓词划窄了。

**修法(两步)**:

- 立即:`tc_origin_is_user_explicit` 纳入 `GlobalDefault`。
- 结构:pin 与目标侧同批推迟 —— 图供给目标侧时,词表行的 pin 指的那份载荷
  提供的头文件和 C 库,这个工程一样都不用,pin 本身就失去意义。
- 无论走哪条,状态行必须写成:
  ```
  Resolved gcc@16.1.0  (target pin for x86_64-windows-gnu, overriding default llvm@22.1.8)
  ```

### 7.2 `mcpp toolchain list` 的三处误导

`src/toolchain/lifecycle.cppm:587-605` 的 Targets 表 = 已装载荷 + 词表,
再经 `host_can_serve`(`registry.cppm:583`,纯载荷谓词)过滤。后果:

| 现象 | 事实 |
|---|---|
| 本机看不到 `aarch64-macos` | mcpp 自己 `5e3e1d9` 的实测表里,Linux 宿主产出过 aarch64 Mach-O |
| `x86_64-windows-gnu` 的 TOOLCHAIN 列写 `gcc 16.1.0` | 恰是这个工程**唯一不能用**的那个 |
| `openkal-llvm 22.1.8` 列在「Available(去装)」 | `registry.cppm:411` 明说它「downloads nothing, installs nothing」,载荷已装 |

**修法**:
- Targets 表加 SOURCE 列:`payload` / `graph`。
- 在**工程上下文**里(cwd 有 `mcpp.toml` 且有依赖)按图重算该表。
  无工程时退回现状并注明「(payload only — run inside a project to see
  what your dependencies add)」。
- `openkal-llvm` 与 `llvm` 共享载荷 ⇒ Available 段按**载荷**去重,不按 Family。

### 7.3 「Resolved 之后 204 ms 静默」

不是把它变快(它是本地解析,`--offline` 逐毫秒相同),是把它**说出来**。
当前 204 ms 依赖解析 + 388 ms 扫描校验 plan = **近 600 ms 无任何输出**,
用户感知为卡顿。加两条状态行:

```
   Resolving dependencies                     ← 覆盖那 204ms
   Scanning module sources (N packages)       ← 覆盖那 388ms
```

真要变快:依赖解析可按 `(manifest hash, index version)` 记忆化。
⚠️ 落点应该是 `mcpp.lock` —— 而它现在**只写不读**,构建路径从不读它,
`mcpp update` 因此是空操作。这一条单独立项,不塞进本文。

### 7.4 `compile_commands.json`:333 ms / 8.4 MB / 3235 条

一个源文件的工程不该产出这个。加 `[build] compile-commands`:

- `"workspace"`(**新默认**):只写工作区成员的 TU。本例 3235 → 1。
- `"all"`:现状,给需要跳进依赖读源码的人。
- `"off"`。

外加:内容不变则不重写(现在每次全量重写)。

---

## 8. 【文档·高】openkal 在 `docs/` 下等于不存在

实测统计(`origin/main`):

| 检索 | 出现次数 |
|---|---|
| `mcpp:kernel-abi` / `mcpp:c-abi` / `mcpp:c++-abi` 在 `docs/` | **0** |
| `openkal` 在 `docs/` | **3**,全在 `13-baremetal.md`,全是讲 `alloc-kal` |
| `examples/` 下 openkal 示例 | **0** |
| `mcpp new` 的 openkal 模板 | **0** |

而 `docs/03-toolchains.md:115` 与 `README.md:380` 仍把 `x86_64-windows-gnu`
描述为「MinGW-w64 / gcc 16」,**一个字都没提**目标侧来自依赖图时所需的编译器族会变。

⇒ 一个用户在 `[dependencies]` 里加一行,就跨进了一套完全不同的解析规则,
而没有任何文档提到这件事发生了。

**要写的三样**(⚠️ 中英双份 + 标题层级是 CI 强制的):

1. `docs/14-openkal.md` / `docs/zh/14-openkal.md`
   - 「目标侧从哪来」:载荷模型 vs 图模型,以及 `Target` 报告怎么读
   - `provides = ["mcpp:<layer>"]` 语法(§2 落地后加 `requires`)
   - 何时需要换编译器族,以及为什么
   - 已知可达的目标矩阵 —— ⚠️ **写实测过的,不写推断的**
2. `examples/06-openkal-cross/`:一份 `mcpp.toml` + 一条命令跨到三个目标,
   README 附实测产出(`file` 输出 + 导入表)。
3. `docs/03-toolchains.md` 与 `README.md` 的目标表加一列「目标侧来源」。

⚠️ 文档里的每一条可粘贴命令都是**承诺**,包括其中的版本号。
这条已经复发过两次(`compat.std-freestanding@0.1.0` 在索引里不存在,
照着粘贴直接失败)。示例里的版本号必须与索引当天的 latest 一致,
并在发布新版时同批更新。

---

## 9. 【稳定性】测试矩阵漏掉的正是默认路径

`aa891f8` 建了 openkal 的 3×3 交叉验证工作流,但它**只跑 openkal-llvm**。

用户走的是默认路径:词表 pin ⇒ `gcc@16.1.0`。**「图目标侧 × gcc」这一格
从来没有被任何测试覆盖过**,而它是零配置用户会走到的第一条路。

⚠️ 同一教训在这个仓库已经犯过两次:**断言要放到矩阵的「行」一级**。

**要加的格**:

| 目标 | 编译器 | 期望 |
|---|---|---|
| `x86_64-linux-gnu` | gcc(默认) | ⇒ §2 的拒绝诊断,**不是**编译器原话 |
| `x86_64-windows-gnu` | gcc(词表 pin) | ⇒ §2 的拒绝诊断 |
| `x86_64-windows-musl` | llvm | ⇒ 构建成功(§3 + §4) |
| `aarch64-macos` | llvm,Linux 宿主 | ⇒ 构建成功且出现在 `toolchain list`(§7.2) |

⚠️ e2e 的 `# requires:` 行必须真的能匹配,否则测试从未跑过而 CI 全绿
—— `65_*` 曾因死 token 一次都没跑。加完新格后逐条确认 skip 计数。

---

## 10. 优先级与依赖

```
第 0 批 —— 立即、无依赖、可单独提 PR
  §4B.3 openkal-windows 的 cfg       (包侧 PR,已实测,一个 token)
  §4.4  Family 去掉 OpenkalLlvm       (纯命名,别名表兜底,e2e 269 已在守)
  §7.1  GlobalDefault 纳入 explicit + 状态行说出覆盖
  §7.4  compile_commands 默认收窄     (333ms + 8.4MB,改动面小)

第 1 批 —— 性能,与语义解耦,可并行
  §5    staging 按包分组             ← 收益最大(1624 → ~10 条边)
  §6    缓存身份去路径 / 归一拼写 / 原子提交
  §7.3  两条状态行(补上 600ms 的静默)

第 2 批 —— 语义地基(⭐ 这是根,后面都依赖它)
  §4    env 恢复三态 + 请求/事实分离 + check_request
  §4B.2 cfg 的 c-abi / kernel-abi / c++-abi 维度

第 3 批 —— 建立在 §4 之上
  §2    编译器进 TargetSide + requires 语法
  §3    词表拆两张(用 §2 的诊断兜底)
  §9    测试矩阵补格

第 4 批 —— 随 §2/§3/§4 落地后一起
  §8    文档 + example + 目标表(⚠️ 写实测过的矩阵,不写推断的)

单独立项、不在本文
  §7.3 尾   mcpp.lock 实际参与解析     (它现在只写不读,`mcpp update` 是空操作)
  §5.3     零拷贝 staging           (要先确认没有原地改写 staged 产物的步骤)
```

⚠️ **§4 排在 §2/§3 之前而不是之后**,尽管 §2/§3 的症状更痛。理由:
§2 的「编译器能不能消费这层」和 §3 的「这个三元组存不存在」都要引用
「这个目标的 C 库到底是谁」,而在 §4 落地之前,那个问题的答案有两个来源
(名字 和 `TargetSide`)且它们会矛盾。**先修根,否则 §2/§3 会各自实现一遍
「以哪个为准」,而这正是 `01d6cef` 刚刚合并掉的那种三处推导。**

---

## 11. 本文刻意没有断言的事

- **没有断言 §5 分组后具体快多少。** 1624 × 2.1 ms 是进程启动的下界,
  实际收益取决于文件系统与核数,要改完再测。
- **没有断言 §6 之后缓存降到多大。** 「136 → ~40」是按内容哈希的取值数
  推的,归一化命令串后可能还有别的轴在碎。
- **没有断言 gcc 加上 `-isystem` 之后就能编 libc++ 的 std 模块。**
  §2.3 步骤 4 修的是「静默丢弃包侧 flag」这个独立缺陷;
  gcc 能否消费 libc++ 的模块是另一个问题,而 §2 的方案是**在此之前就拒绝**,
  正因为答案不确定。⚠️ 若有人要去证明它可行,那是一次实测,不是一次推理。

- **没有断言 §4.1(删掉 `env` 自动填充)的改动面有多大。**
  这是本文风险最高的一步:`env.empty()` 今天意味着「macOS」或「刚解析完」,
  改完之后还意味着「用户没指定」。全仓库每一处读 `env` 的地方都要重新判一次。
  ⚠️ 实施前必须先做一次纯统计:`grep -rn "\.env\b\|is_musl()\|is_windows_gnu()"`,
  把命中点逐个分类,**再**决定这一步值不值得。若命中点超过 ~30 处,
  应改为「保留自动填充,但另存一个 `envExplicit` 布尔」的窄改法 ——
  语义等价,改动面小一个量级。

*(原本这里留了一条「不确定用户为什么现在选中了 llvm」—— 已核实,见 §7.1.1,
现在是事实而不是猜测。)*

---

## 12. 【架构·全景】目标与目标侧作为数据 —— 一份现成的对照实验

本节回答一个直接的问题:**既然 mcpp 已经有了构建期的 target / runtime / ABI
选择,那 libc、编译器运行时、BSP、乃至 target 本身,是不是都可以做成依赖,
而不必每次修改 mcpp?**

答案是 **四层里三层今天就已经可以了,而且有活证据**;剩下的两处耦合比想象中窄。
更有意思的是,**同一时刻正好有一份对照实验在跑**。

### 12.1 对照实验:两个 PR 在做同一件事

| | `mcpp#492` + `xim-pkgindex#677` | openkal |
|---|---|---|
| 要供给什么 | 为 musl 配置过的 libc++/libc++abi/libunwind + `std.cppm` | **同一类东西** |
| 怎么表达 | `kKnownTargets` 新增一列 `llvmSysroot` | 包里一行 `provides = ["mcpp:c++-abi=libc++"]` |
| 引擎改动 | **349 行**,横跨 `triple` / `prepare` / `flags` / `stdmod` | **0 行** |
| 加一个新目标要 | 改 mcpp → 发版 → 等用户升级 | 发一个包 |
| 谁能加 | 只有 mcpp 维护者 | 任何人 |

⚠️ 这不是说 #492 做错了 —— 在它被写下的时候,那是**当时唯一走得通的路**(§12.3)。
它的价值恰恰在于:它把「不走图模型的代价」量化成了一个可以读的 diff。

### 12.2 逐条对照:#492 的七件事,图模型已经做了六件

| # | #492 做的事 | 图模型 | 依据 |
|---|---|---|---|
| 1 | `--no-default-config` 中和 clang.cfg 的宿主绑定 | ✅ 已发 | std 缓存 metadata 实测可见该 flag |
| 2 | `--target=<triple>` | ✅ | `crossTargetFlag` |
| 3 | `-nostdinc++ -isystem <libc++ include>` | ✅ | `pkg.publicUsage.includeDirs`(`prepare.cppm:5872`) |
| 4 | `-nostdlib++ -L… -lc++ -lc++abi` | ✅ | 包自己的 `[build] ldflags` |
| 5 | `std.cppm` 取自包而非载荷 | ✅ | `[package] std-module`(`prepare.cppm:5773`) |
| 6 | 覆盖 `tc->targetTriple`,让输出目录/缓存键/flag 层一致 | ✅ | 图路径同样覆盖 |
| 7 | `--gcc-toolchain=<musl-gcc payload>` 借 crt/libgcc/libc | ❌ | **唯一真正新的机制** |

⭐ 第 7 条之所以需要,是因为在 #492 的方案里 **libc 是一份预编译载荷**;
在 openkal 里 libc 是**图里的一个包**(`openkal-musl`),它自带 start files,
所以没有「向另一份载荷借 crt」这个问题。

⇒ **#492 的六分之七是在为载荷世界重新实现图路径已经做过的事。**

### 12.3 ⚠️ 一次实验推翻了本文先前的判断:预编译包**今天就能**认领目标侧

本文的初稿在这里写着「二进制分发的 mcpp 包无法声明目标侧能力」,理由是
`mcpp pack` 不发 `provides`。**那条结论是错的**,而推翻它的是一次实验。

#### 实验

手写一个**无源码**的包 —— 没有一行可编译的代码,只有预编译资产:

```toml
[package]
namespace = "probe"
name      = "libcxx-prebuilt"
version   = "0.1.0"
provides  = ["hosted-standard-library", "mcpp:c++-abi=libc++"]
std-module        = "modules/std.cppm"
std-compat-module = "modules/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc++", "-isystem", "include-config"]

[build]
sources      = []                                # ⭐ 显式空 = 什么都不编
include_dirs = ["include-config", "include"]
ldflags      = ["-L", "lib", "-lc++", "-lc++abi"]
```

资产就是 llvm 载荷里现成的 libc++(头、静态库、`std.cppm` 与它的 90 个片段),
一个 `path` 依赖引进来。**实测结果:**

```
      Target x86_64-unknown-linux-gnu
             kernel-abi  linux (payload)
             c-abi       gnu (payload)
             c++         libc++         (libcxx-prebuilt@0.1.0, graph)   ← ⭐
   Compiling libcxx-prebuilt (path)
    Finished dev in 0.40s
     Running `target/x86_64-linux-gnu/…/bin/app`

prebuilt package supplied the c++ layer
```

⭐ **一个手写的、零源码的预编译包认领了 `c++-abi` 层、供给了 std 模块、
把自己的 `std-module-flags` 和 `include_dirs` 送上了命令行 —— 引擎零改动。**

#### 所以真正的缺口是什么

`sources = []` 是**已支持的显式拼写**,`types.cppm:374-390` 的注释写得很清楚:
「A binary distribution package needs that spelling」。`provides` / `std-module`
是 `[package]` 级的键,与有没有源码无关。**模型完全能表达。**

缺的只有一件事:`src/pack/manifest_emit.cppm` **从零重建 manifest**,
发的十余个键里不含 `provides` / `std-module` / `std-compat-module` /
`std-module-flags`。

⇒ 准确的说法是:**`mcpp pack` 这条自动生产线会把能力声明丢掉,
但手写 manifest 不受影响。** 这是**工效缺陷,不是能力缺陷**,
优先级应从「阻塞」降到「补齐」。

⚠️ 顺带发现(这条仍然成立):`manifest_emit.cppm:151-158` 的
`cfg_predicate_for(triple)` 按 `arch/os/env` 三段生成谓词,
**把 `env` 段烙进了 mcpp 产出的每一个二进制包**。§4 的问题因此不止影响
手写的包,它影响 `mcpp pack` 的全部产物。

#### 顺带复现:§2 的缺陷不是 openkal 特有的

同一个 probe 包 `--target x86_64-linux-musl`:musl 行的 pin `gcc@16.1.0`
盖掉 llvm 默认,然后 `gcc.cppm:159` 把包侧 flag **全部丢弃** ——

```
x86_64-linux-musl-g++ -std=c++23 -fmodules -O2 --sysroot=… -c …/modules/std.cppm
                                        ↑ 没有 -isystem、没有 --no-default-config、没有 -nostdinc++
error: __config: No such file or directory
```

⭐ 一个与 openkal 毫无关系的包,走到了**逐字相同**的失败。§2 和 §7.1
因此各多一条独立证据。

### 12.4 ⚠️ #492 引入的是第四条并行判据

`src/targetside/model.cppm` 开篇列出了它存在的理由 —— 三条互相矛盾的判据:

```
prepare  openkalTargetSide   工具链族名是 "openkal-llvm"
flags    graphTargetSide     targetCxxRuntime && !crossTargetFlag.empty()
dist     graphCxxRuntime     targetCxxRuntime
```

`#492` 的 `isLlvmMusl` 是**第四条**,而且又一次在三个文件里各推一遍
(`flags.cppm` 三处 `if`、`prepare.cppm` 一处、`stdmod.cppm` 一处)。它的定义是:

```cpp
is_clang(tc) && is_musl_target(tc) && !tc.targetSysrootInclude.empty()
```

⭐ 这句话说的正是 **`TargetSide::cxx.origin != Payload`** —— 一个 `01d6cef`
刚刚建立起来的、有单一真源的事实,被用三个代理变量重新拼了出来。

⚠️ 这不是批评作者:`TargetSide` 只解析**图**供给的层,而 #492 供给的是**载荷**,
`Origin::Xpkg` 那一支今天没有任何消费者去问。**缺口在 `TargetSide` 的覆盖面,
不在 #492 的写法。** 正确修法是让 `Origin::Xpkg` 也成为一等来源,
于是 `isLlvmMusl` 消失,变成 `ts.cxx.origin != Origin::Payload`。

### 12.5 直接回答:哪几层已经可以是依赖

| 层 | 能否作为依赖 | 证据 / 阻塞点 |
|---|---|---|
| 普通库 | ✅ | 一直如此 |
| **BSP / 平台接口**(kernel-abi) | ✅ **0 引擎改动** | `openkal-linux` / `-windows` / `-macos` / `-opensbi` |
| **C 库**(c-abi) | ✅ **0 引擎改动** | `openkal-musl` |
| **编译器运行时**(c++-abi) | ✅ **0 引擎改动** | `openkal-llvm-runtime`(libc++ + libc++abi + libunwind + compiler-rt builtins) |
| 同上,但**以预编译资产分发** | ✅ **实测,0 引擎改动** | §12.3 的 `libcxx-prebuilt` 实验 |
| 同上,但由 **`mcpp pack` 自动产出** | ⚠️ | pack 不生成那四个键(工效,非能力) |
| **target 的「组合」**(已有 token 的新排列) | ⚠️ 假耦合 | §12.6(a),被 `kKnownTargets` 当门挡着 |
| **target 的「词汇」**(全新 os/env token) | ❌ | §12.6(b),`parse()` 闭集 |
| **编译器族** | ❌ | `Family` 枚举 —— 而这条**应当保持关闭**(§12.5.1) |

⇒ 你的判断成立,而且**比预想的还成立**:连预编译资产都不需要引擎改动。
openkal 不是「绕过构建体系」,它是**唯一一个把构建体系当接口用**的生态,
而那个接口是 `01d6cef` 建的。

### 12.5.1 判据:「组合」该是数据,「词汇」该在引擎

三个 ❌ 不该用同一个答案,而区分它们的判据 `targetside/model.cppm:135-138`
已经写下来了,只是当时只用在「层名 vs 实现名」一个维度上:

> Layer names may be hardcoded because the layers are fixed by the C and C++
> build model and do not grow. Implementations may not, because growing is
> precisely what they do: **the ecosystem's combinations are 2×N×M while its
> packages are 2+N+M.**

把这句话推广开,三个问题各自的答案就出来了:

| | 性质 | 增长 | 谁赋予语义 | 结论 |
|---|---|---|---|---|
| 哪个 libc 配哪个 kernel 配哪个运行时配哪个 arch | **组合** | 乘法 | 包自己 | **必须是数据** ✅ 已是 |
| 有哪些层(kernel-abi/c-abi/c++-abi) | 词汇 | 不增长 | 引擎 | 保持硬编码 ✅ |
| 有哪些 os / env token | 词汇 | 加法,一年一两个 | **引擎**(`is_pe()`、`cfg(windows)`、库命名、strip 适用性…十余处) | 保持闭集 |
| 有哪些编译器族 | 词汇 | 加法,极慢 | **引擎**(flag 拼写、模块模型、BMI 格式、cfg 文件) | 保持闭集 |

⭐ 结论:**`Family` 是闭集这件事没有错,错的是有人往里面塞了一个不是编译器的东西**
(`OpenkalLlvm` = 目标侧策略)。删掉它(§4.4),闭集就是对的。

同理 `parse()` 的 token 表:开放它不是「让 target 成为数据」,而是
**把语义责任推给包作者** —— `os` 字段今天被十余处代码赋予行为,
一个包定义的 opaque `os` 无法回答其中任何一处。

### 12.6 target 要成为数据,耦合点只有两处(而且不是同一处)

「加一个 target 要改 mcpp」听起来像一件事,读码后是两件,难度差一个量级:

**(a) `kKnownTargets`(`triple.cppm:171`)—— 假的耦合。**
这张表现在同时当**门**和**约定表**用。§3 已经论证:它只该是后者。
表里没有行 = 没有约定,而这对图供给的目标恰好正确。
⭐ **实测:`x86_64-windows-musl` 用 `[target.X]` 逃生口全程走通并在 wine 下运行**
—— 也就是说,这一层的「必须改 mcpp」**今天就已经不成立了**,只是默认路径被门挡着。

**(b) `parse()` 的 os/env token 表(`triple.cppm:474-509`)—— 真的耦合。**

```cpp
if (k == "linux")  { t.os = "linux"; … }
if (k == "windows"){ t.os = "windows"; … }
…
// Unrecognized segment (androideabi, wasi, …): not in mcpp's target
// language — treat as unparseable rather than guessing.
return std::nullopt;
```

`arch` 是 `tok[0]`,自由透传;**`os` 和 `env` 是闭集**。所以:

- `riscv64-linux-musl`、`aarch64-windows-gnu` 这类**已有 token 的新组合**
  —— (a) 落地后即可用,**不需要动 mcpp**。
- `x86_64-haiku`、`wasm32-wasi`、`aarch64-linux-ohos` 这类**新 token**
  —— 必须动 `parse()`。⚠️ HarmonyOS 那次适配踩的正是这一处
  (而且发现「`ohos` 是 env 不是 os」)。

**建议的形状**:未知 token **不再让整份三元组解析失败**,而是原样保留为
`opaque` 段,并交给图去认领 —— 与 §3 的判据一致(「不可证伪就放行」)。
若没有任何包 `provides` 这个目标,就在图已知之后拒绝,报错里说得出
「没有任何依赖实现这个平台」。

⚠️ 这条比 (a) 风险高:`os` 字段今天被 `cfg(windows)`、`is_pe()`、
`is_freestanding()`、库命名规则、strip 适用性等**十几处**读。opaque 段进来后
每一处都要有确定答案。**建议排在 §4 之后单独立项,不与 §3 同批。**

### 12.7 「规范 target」放哪里:SPEC-002

`docs/specs/` 已有 SPEC-001(包身份),README 明确了规范文档的性质:
「定义机制的语义与约束」「同时描述目标规范与当前实现,每条规则都标注实现状态」。

⇒ **SPEC-002「目标身份与目标侧能力模型」**,内容:

1. **三元组的语法与语义** —— 四条轴、三个字段、`env` 是请求不是事实(§4)。
   每条标 ✅ / ⚠️ / ❌,因为其中一半今天还不成立。
2. **`mcpp:` 保留命名空间** —— `provides` / `requires` 的语法、当前的三个层名、
   「引擎硬编码层名、永不硬编码实现名」这条约束本身。
3. **目标侧的四种来源** —— `Payload` / `Xpkg` / `Graph` / `None`,以及分层规则
   (下层被谁配置过、上层能被谁消费)。
4. **一个包要供给某一层,必须发什么** —— 含 §12.3 的二进制分发通路。
5. **兼容性条款** —— ⚠️ 老客户端遇到未知 `mcpp:` 层名/未知键**必须降级而不是
   整份 manifest 加载失败**。这条已经付过学费(#359),规范里必须写死。

### 12.8 落地顺序(与 §10 合并)

```
不需要排队 —— 今天就能做,零引擎改动
  §12.3  llvm-musl-libcxx 改为手写 manifest 的 mcpp 预编译包
         ← 实验已证形状可行;#492 的三处引擎改动因此可以撤掉

第 0 批 追加
  §12.3尾 mcpp pack 补发 provides / std-module / std-compat-module / std-module-flags
         ← 工效而非能力(手写不受影响),但自动生产线不该丢语义
  §4.4    Family 删掉 OpenkalLlvm(§12.5.1:闭集是对的,内容错了)

第 1 批 追加
  §12.4  TargetSide 覆盖 Origin::Xpkg,消掉第四条并行判据
         ← 做完之后 #492 的 isLlvmMusl 变成 ts.cxx.origin != ts.cAbi.origin
  §12.9-2 「clang 交叉到载荷供给 libc 的宿主目标」的 C 运行时接线
         ← #492 唯一不可约的部分,换成通用判据后落在这里

第 3 批 追加(§3 之后)
  §12.6(a) 已由 §3 覆盖,无额外工作

明确不做
  §12.6(b) parse() 开放 os/env token  ← 见 §12.5.1:那是把语义责任推给包作者
  编译器族开放                        ← 同上

单独立项
  §12.7    SPEC-002
```

### 12.9 给 #492 的建议(如果它还没合)

不建议直接否掉 —— 它解决的是一个真实且当下的阻塞(GCC 16 modules ICE)。
但 §12.3 的实验说明它的**不可约核心比 349 行小得多**。

#### 实测:把 §12.3 的 probe 包交叉到 musl,看还剩什么

`[target.x86_64-linux-musl] toolchain = "llvm@22.1.8"` + 那个手写预编译包:

| 阶段 | 结果 |
|---|---|
| 目标侧解析 | ✅ `c++ libc++ (libcxx-prebuilt@0.1.0, graph)` |
| **std 模块** | ✅ **编过了**,命令行带 `--target=x86_64-unknown-linux-musl`、`--no-default-config`、`-nostdinc++`、包的 `-isystem` |
| 产物 | ✅ `std.o` + `std.compat.o` + `pcm.cache` 落盘 |
| **链接** | ❌ mcpp 自己的 hermetic 检查拦下:`crt1.o / crti.o / crtbeginT.o` 解析到了**宿主** |

⭐ 也就是说 #492 的七件事里,**第 1–6 条图路径确实全都做到了**(不是推测,
是这次实测走过去的),而第 7 条被 mcpp 自己的完整性检查精确地指了出来。

⇒ **#492 的不可约核心只有一件事:把 clang 指向目标的 C 运行时**
(crt / libgcc / libc,来自另一份载荷)。manifest 无法可移植地命名另一份载荷的前缀,
所以这件事必须留在引擎。

#### 建议

1. **`llvm-musl-libcxx` 改为 mcpp 包**(预编译资产 + `provides` + `std-module`),
   **不需要等 pack 补键,手写 manifest 今天就能发**。
   ⇒ #492 的 `triple` 新列、`prepare` 的解析块、`stdmod` 的分支**全部不需要**。
2. **保留并推广第 7 条**,但换判据。现在是 `is_clang && is_musl_target`,
   应该是**「C 库来自载荷,而 C++ 运行时不来自同一份载荷」** ——
   即 `ts.cAbi.origin == Payload/Xpkg && ts.cxx.origin != ts.cAbi.origin`。
   这不是 musl 特有的形状,是「clang 交叉到任何载荷供给 libc 的宿主目标」。
   ⚠️ 落点应是 §12.4 说的 `Origin::Xpkg` 一等化,而不是第四条并行判据。
3. **保留** `toolchainFromFlag`,但**与 §7.1 合并成一次修改**:#492 只覆盖了
   `MCPP_TOOLCHAIN` 与命令行 flag,仍未覆盖 `mcpp toolchain default` 写下的
   全局默认(§7.1.1 实测:用户被迫把同一个值写了两遍)。
4. ⭐ **顺带**:§12.3 尾的复现说明,#492 的 musl 目标在**默认路径**(词表 pin =
   gcc)上会撞进 §2 的缺陷。#492 合入后应补一格 e2e:
   `--target x86_64-linux-musl` 不带任何 toolchain 覆盖,断言得到的是
   **mcpp 的诊断而不是编译器原话**。

⚠️ **仍未实测的部分**:本节没有跑过 #492 的分支本身,也没有构造一份真正
为 musl 配置过的 libc++(§12.3 的 probe 用的是宿主 libc++,所以它的 std 模块
虽然编过了,链接必然失败)。上面第 1 条建议若要执行,**先把
`llvm-musl-libcxx` 按 mcpp 包形态发一份试装** —— 那是一次实验,不是一次推理。

---

## 13. 目标侧包的分发形态与身份

§12 回答了「能不能作为依赖」。本节回答两个紧接着的问题:
**这些包该以什么形态分发**,以及**怎么知道是谁在供给我的 libc**。

### 13.1 目标侧包应当是**源码包**,预构建是退步

⚠️ **本节初稿写反了,主张目标侧包应当预构建。以下是修正后的结论。**

现状是:**编译器相关的东西被迫走预构建载荷**(`xim:llvm-musl-libcxx` 是
一份 xim 资产 + 引擎硬编码一列),而 openkal 走的是图里的源码包。
正确的方向是后者,理由有五条,其中第三条是决定性的。

**1. 可维护性。** 源码包可以被任何人 fork、打补丁、发版本。预构建资产要求
发布者为每个 (目标 × 架构) 构建、托管、sha256 固定 —— `xim-pkgindex#677`
为此写了 122 行 Lua,资产还挂在贡献者 fork 的 release 上等待迁移。

**2. 正确性由构造保证。** 源码包用**你的**编译器、对着**你的**图里的 C 库、
按**你的** flag 编译。预制归档是对着别的东西编的,匹配与否只能靠信任 ——
而这正是本仓库已经记录过的失败模态:`__config_site` 描述的是一份配置,
混用不会在选择处失败,会在头文件深处失败。

**3. ⭐ 引擎自己的判据指向源码包,而预构建把你推回乘法侧。**
`targetside/model.cppm:135-138`:

> the ecosystem's **combinations are 2×N×M** while its **packages are 2+N+M**

预构建的资产数 = 目标 × 编译器版本 × C 库 —— 那是 N×M 那一侧。
源码包是 1。**这条判据是这套设计的地基,它不支持预构建。**

**4. 冷构建的代价被高估了 —— 实测。**

```
第一次(冷)        4.9 s 墙钟 / 44 s CPU / 243 units
mcpp clean 后      Cached openkal-llvm-runtime v0.1.1 (243 units)   1.02 s
全新工程、不同包名、不同版本号、从未构建过
                   Cached openkal-llvm-runtime v0.1.1 (243 units)   1.655 s
```

⭐ 第三行是关键:每包 Merkle 键**跨工程命中**。所以冷构建是
**每台机器、每 (包版本 × 目标) 一次**,不是每工程一次,更不是每次构建一次。
交叉四个目标 ≈ 3 分钟 CPU,一次。这不构成采用预构建的理由。

**5. 冷构建若真成为问题,正确答案是共享构建缓存,不是预构建包。**
远程/共享 cache 保留源码包的全部性质(可维护、由构造保证正确、包数是加法),
同时消掉重算。预构建包用**放弃前三条**来换第四条,而第四条已经很便宜了。

#### ⭐ 这也正好解释了 349 行 vs 0 行

`openkal` 零引擎改动而 `#492` 要 349 行,差别不在实现质量,在**边界画在哪**:

| | libc 在哪 | 引擎要做什么 |
|---|---|---|
| openkal | **图里**(`openkal-musl`,源码包) | 什么都不用做 —— 两层都在图里,关系由图表达 |
| #492 | **载荷里**(musl-gcc payload) | 必须去接线「载荷 A 的 libc」与「载荷 B 的 libc++」 |

⇒ **一旦把 libc 也拿进图,那条不可约的引擎改动就消失了。**
§12.9 实测剩下的唯一一件事(把 clang 指向目标的 C 运行时)之所以存在,
正是因为 #492 把 libc 留在了载荷里。

#### ⚠️ 但预构建这条路仍需堵一个坑(因为 `pack` 已经能走)

即使不推荐,`mcpp pack` today 已经能产出多目标包,而
`manifest_emit.cppm:151-158`:

```cpp
p += std::format(", env = \"{}\"", seg[2]);
```

为 `x86_64-windows-gnu` 打的腿生成
`cfg(all(arch="x86_64", os="windows", env="gnu"))` ——
**与 `openkal-windows/mcpp.toml:41` 手写的那条逐字同形**,
于是在 `x86_64-windows-musl` 上重演 §4B 的失败。

⇒ §4 落地时,`cfg_predicate_for` 必须一起改。这不是为预构建服务,
是因为**每一个 `mcpp pack` 产出的多目标库都带着这个缺陷**,与目标侧无关。

### 13.2 ⚠️ 两个包同时供给一层时,图遍历顺序里第一个静默胜出

`prepare.cppm:5465-5493` 的 `provider_of`:

```cpp
for (auto const& pkg : packages) {
    for (auto const& entry : pkg.manifest.provides) {
        …
        if (!found || (found->interfaceName.empty() && !p.interfaceName.empty()))
            found = p;
    }
}
```

⇒ **没有冲突检测。** 两个包都声明 `mcpp:c-abi`,`packages` 的遍历顺序决定谁赢,
而那个顺序既不是用户写的,也不是用户能预测的。第二个包被静默忽略,
它的 `[build]` 段却仍然参与构建 —— 一个 libc 的头文件配另一个 libc 的实现。

⚠️ 这一层的失败模态特别糟:C 库/内核接口/C++ 运行时**不是可叠加的贡献**,
是**互斥的选择**。选错不会链接失败,会得到一个能跑、偶尔崩的程序。

#### 现成的正确形状:Cargo 的 `links` 键

Cargo 对「我代表这个原生库」有完全同构的约束:**全图至多一个包可以声明
某个 `links` 值**,否则解析期直接报错并指出冲突双方。

mcpp 需要同一条不变量,而且更强 —— 三个层各自至多一个供给者:

```
error: two packages supply the C ABI, and it is a choice rather than a contribution.
         mcpplibs/openkal-musl@0.3.3     (a dependency of openkal-llvm-runtime)
         acme/tinylibc@0.2.0             (a direct dependency)
       A build has exactly one C library. Remove one, or pin the layer with
       `[target.<triple>] c-abi = "…"`.
```

⭐ 判据方向与 §3 相反且刻意如此:§3 是「不可证伪就放行」,
这里是「两个都摆在眼前且互斥,必须拒绝」。

### 13.3 「官方包加命名空间前缀」——⚠️ 该做成显示,不该做成准入

#### 实测:报告显示的是一个不唯一的标识符

`prepare.cppm:5478`:

```cpp
p.name = pkg.manifest.package.name;
```

⇒ 报告打的是 `openkal-windows@0.1.3`,**不含 namespace**。
而 SPEC-001 明确:`name` 是**单一原子段**,层级由 `namespace` 承载。
所以今天的情况是:**一个能改写整个构建的特权角色,正在用一个不保证唯一的
标识符显示自己。** 任何人都可以发一个 `name = "openkal-windows"`。

**修法(零成本)**:`Provider::id()` 打全限定名。

```
kernel-abi  openkal   (mcpplibs/openkal-windows@0.1.3, graph)
c-abi       musl      (mcpplibs/openkal-musl@0.3.3, graph)
c++         libc++    (acme/experimental-libcxx@0.0.1, graph)      ← 一眼看出
```

⇒ 「官方 vs 第三方」这个需求,**在显示层就解决了**,不需要任何权限机制。

#### 为什么不该做成准入门槛

把 `provides = ["mcpp:…"]` 限制到 `mcpplibs:` 之类的命名空间,代价是:

1. **它把守门人从 mcpp 挪到 mcpplibs,瓶颈没消失,只是换了个仓库。**
   而本文 §12 全部的出发点就是「不必每次修改 mcpp」。
2. **它违反 `targetside/model.cppm:127-138` 自己立的规矩** ——
   「硬编码层名,永不硬编码实现」。按所有者建白名单,就是按所有者硬编码实现。
3. **它挡不住真正的风险。** 一个恶意包不需要 `provides` 就能破坏构建
   (`[build] cxxflags` 就够了)。特权不在这个键上,在「能进依赖图」这件事上。

#### 那什么才是真正缺的:知情

真正的风险不是「谁能供给」,是**一个传递依赖可以在 root 工程不知情的情况下
换掉它的 libc**。这与 Cargo 对 build script 的处理是同一类问题。

建议(按强度递增,选一):

- **A(推荐)**:报告打全限定名 + 标注**直接还是传递**:
  `c-abi musl (acme/tinylibc@0.2.0, graph — via openkal-llvm-runtime)`。
- **B**:传递依赖认领层时打一条 `warning`,直接依赖不打。
- **C**:root 工程可显式确认 `[target-side] allow = ["acme/tinylibc"]`,
  未列出即拒绝。⚠️ C 的代价是每个用户都要写一遍,而绝大多数人只想用默认组合。

⭐ 建议 A + §13.2 的唯一性约束。两者合起来覆盖了「语义清晰」的全部需求,
而**不引入任何准入门槛**。

### 13.4 按你给的三条判据核一遍

| | 功能实现 | 语义清晰 | 生态兼容性 |
|---|---|---|---|
| **目标侧走源码包**(推荐) | ✅ openkal 已经是,零引擎改动 | ⭐ 正确性由构造保证,不靠信任预制件的匹配 | ✅ 包数是加法(2+N+M),不随目标矩阵爆炸 |
| 目标侧走预构建 | ✅ 形状也可行(§12.3) | ⚠️ `__config_site` 类的错配只在头文件深处暴露 | ❌ 资产数 = 目标 × 编译器版本 × C 库,回到乘法侧 |
| 冷构建成本 | 实测 44 s CPU,**跨工程命中**后为零 | — | 若真成问题 ⇒ **共享构建缓存**,而不是预构建包 |
| 层供给唯一性(§13.2) | 解析期一次检查 | ⭐ 把「静默选一个」变成「明确拒绝」 | ⚠️ 可能打破现存的图 —— 上线前先扫索引 |
| 全限定名显示(§13.3) | 一行改动 | ⭐ 特权角色用唯一标识符显示 | ✅ 纯输出变化 |
| 命名空间准入门槛 | — | — | ❌ **不做**:把瓶颈换个仓库,违反引擎自己的规矩,且挡不住真风险 |
| `pack` 的 `cfg_predicate_for` 用 env 段 | — | ⚠️ 与 §4B 同形的缺陷 | ❌ **每个多目标预构建库都带着它**,随 §4 一起修 |

⚠️ **唯一性约束的兼容性风险是本节唯一的真风险**:如果索引里已经存在
两个包在同一层声明能力(比如新旧两代 openkal 实现),打开检查会让现存工程
构建失败。**上线前必须先扫一遍索引**,而不是直接合。

---

## 14. 定稿:四层四来源模型

本节是前面所有章节的收敛。判据是一句可验收的话:

> ⭐ **#492 的开发者只维护一个类似 `openkal-llvm-runtime` 的包,就能满足需求。**

三个维度分别定下来,然后用这句话验收。

---

### 14.1 架构设计

#### 模型:四层 × 四来源

**四个层**(引擎硬编码,因为它们由 C/C++ 构建模型固定、不增长):

```
compiler     谁在编译              ← 今天不在 TargetSide 里(§2)
kernel-abi   平台接口
c-abi        C 库
c++-abi      C++ 运行时
```

**四种来源**(引擎硬编码,`targetside::Origin` 已经是这四个):

```
Payload   编译器载荷自带
Xpkg      被点名的一份预制载荷(target 表 sysroot 列 / [target.X].sysroot)
Graph     依赖图里的包
None      没有(零 libc 档、裸机无 kernel)
```

**实现名永不硬编码。** `openkal` / `musl` / `libc++` / `picolibc` 出现在包的
manifest 与索引里,不出现在引擎任何一行代码里。

#### 三条规则

**规则一 —— 每层恰好一个供给者。**
C 库、内核接口、C++ 运行时不是可叠加的贡献,是互斥的选择。两个供给者是错误
(§13.2 实测:今天是图遍历顺序里第一个静默胜出)。

**规则二 —— 上层必须为下层配置过。**
`check_layering` 已经守住一半(载荷 C++ 运行时 × 非载荷 C 库),
另一半(图供 C++ 运行时 × 编译器族)缺失(§2)。

**规则三 ⭐ —— 引擎只在「跨来源」时接线。**

| 组合 | 谁表达关系 | 引擎改动 |
|---|---|---|
| 两层都在 `Graph` | 包之间用普通依赖表达 | **0 行** ← openkal |
| 两层都在 `Payload` | 载荷自洽 | **0 行** ← gcc-musl |
| 一层 `Payload`/`Xpkg`,一层 `Graph` | **只有引擎知道两边的地址** | 必须接线 ← #492 |

⇒ **#492 需要的不是一个新分支,是规则三第三种情形被实现。**

#### 实测:模型已经完备,缺的只是消费

```toml
[target.x86_64-linux-musl]
toolchain = "llvm@22.1.8"
sysroot   = "xim:musl-gcc@16.1.0"     # 已存在的键
```

```
c-abi       musl-gcc       (xim:musl-gcc@16.1.0, prebuilt)   ⭐ Origin::Xpkg,模型认了
c++         libc++         (libcxx-prebuilt@0.1.0, graph)    ⭐ Origin::Graph,模型认了
```

而生成的链接行:

```
ldflags = -static --target=x86_64-unknown-linux-musl --no-default-config
          -fuse-ld=lld -L<llvm 载荷>/lib/... -Wl,-rpath,...
                                  ↑ 没有 --sysroot,没有 --gcc-toolchain
```

⇒ hermetic 检查开火:`crt1.o / crti.o / crtbeginT.o` 解析到宿主。

**诊断:`sysrootXpkg` 被解析、被报告,却没有被渲染。** 原因是
`prepare.cppm` 里把 xpkg 物化(`xpkg_payload()` → `targetSysroot*`)那一段
**限定在 `is_freestanding()`**,而 `flags.cppm` 只在 freestanding 分支消费它。

#### 架构结论(三条,按重要性)

**A1. 删掉 `llvmSysroot` 列。** `sysroot` 列的注释自己写着:

> The TARGET's C library, resolved at compile time exactly the way `pin`
> resolves the compiler. … A hosted target gets its libc automatically —
> `x86_64-linux-musl` carries musl inside its **gcc payload**

⭐ 「hosted 目标自动拿到 libc」这条假设**只在编译器是 gcc 时成立**。
换成 clang 就不成立 —— 而那正是 #492 的场景。列的**含义**已经覆盖了它,
只有**消费范围**没有。#492 加第二列,是把一个作用域问题当成了缺列问题。

**A2. 把 sysroot 列的物化与渲染从 freestanding 解绑。** 一处 `if` 的作用域,
外加 clang 的 `--gcc-toolchain` 渲染。⚠️ 后者需要一次实测确认(下方 §14.4)。

**A3. `Family` 保持闭集,删掉 `OpenkalLlvm`。**
「目标侧从哪来」由 `TargetSide` 回答,不由工具链名字回答(§4.4、§12.5.1)。

---

### 14.2 语义一致性

五条,每条一句话,每条都有一个当前违反它的实测。

| # | 语义 | 当前违反 |
|---|---|---|
| S1 | **三元组是请求,`TargetSide` 是事实** | `x86_64-windows-gnu` 报告 `c-abi musl`(§4) |
| S2 | **`cfg()` 按事实求值,不按名字** | `openkal-windows` 的 `env="gnu"` 丢掉四个导入库(§4B) |
| S3 | **每层恰好一个供给者** | 冲突时图遍历顺序第一个静默胜出(§13.2) |
| S4 | **特权角色用唯一标识符显示** | 报告只打 `name`,不打 `namespace`(§13.3) |
| S5 | **`provides` / `requires` 对称** | `requires` 不存在;编译器要求无法声明(§2) |

**S1 的推论**:请求与事实矛盾 ⇒ **拒绝**,不并排打印。
`env` 空 = 未指定(恢复 `triple.cppm:514` 抹掉的那一态)。

**S3 的形状**:与 Cargo 的 `links` 键同构 —— 全图至多一个包声明某个值,
冲突即解析期报错并指出双方。

**S4 的修法**:`Provider::id()` 打全限定名。⭐ 「官方 vs 第三方」是**显示问题**,
不是权限问题;命名空间准入门槛明确不做(§13.3)。

**S5 的语法**:

```toml
provides = ["mcpp:c++-abi=libc++"]     # 我供给哪层
requires = ["mcpp:compiler=llvm"]      # 我需要哪层由谁供给
```

⚠️ 老引擎遇到未知 `mcpp:` 层名或未知键**必须降级,不得让整份 manifest
加载失败**(#359 已付过学费)。这条进 SPEC-002。

---

### 14.3 用户侧使用

#### 零配置到交叉,四条命令

```bash
mcpp build                                  # 宿主,载荷自洽
mcpp toolchain default llvm                 # 换编译器 —— ⚠️ 今天会被词表 pin 盖掉(§7.1)
mcpp build --target x86_64-linux-musl       # 交叉
mcpp build --target x86_64-linux            # ⭐ S1 之后:C 库谁供给谁说了算
```

加一层 = 加一条依赖:

```toml
[dependencies]
llvm-musl-runtime = "0.1.0"
```

#### 报告显示四层,带全限定名

```
   Compiling app v0.1.0 (.)
      Target x86_64-linux-musl → x86_64-unknown-linux-musl
             compiler    llvm@22.1.8                            (payload)
             kernel-abi  linux                                  (payload)
             c-abi       musl        (xim:musl-gcc@16.1.0,      prebuilt)
             c++         libc++      (mcpplibs/llvm-musl-runtime@0.1.0, graph)
```

⭐ 三处变化:**加 compiler 行**(S5)、**全限定名**(S4)、
**来源词与层对齐**(payload / prebuilt / graph / —)。

#### 四条诊断,替换今天的编译器原话

```
error: this C++ runtime requires an llvm-family compiler.
         c++       libc++  (mcpplibs/llvm-musl-runtime@0.1.0, graph)
         compiler  gcc@16.1.0  (target pin for x86_64-linux-musl)
       The target row's pin is mcpp's own default and yours outranks it:
           mcpp toolchain default llvm@22.1.8
       or pin it for this target only:
           [target.x86_64-linux-musl]
           toolchain = "llvm@22.1.8"
```

```
error: two packages supply the C ABI, and it is a choice rather than a contribution.
         mcpplibs/openkal-musl@0.3.3   (via mcpplibs/openkal-llvm-runtime)
         acme/tinylibc@0.2.0           (a direct dependency)
```

```
error: this build requests the `gnu` C ABI, and its graph supplies `musl`.
       Write `--target x86_64-linux` to let the graph decide.
```

```
error: nothing supplies this target's C library.
         c-abi   —
       The compiler payload does not carry one for x86_64-linux-musl, and no
       dependency provides `mcpp:c-abi`. Name a prebuilt one:
           [target.x86_64-linux-musl]
           sysroot = "xim:musl-gcc@16.1.0"
       or depend on a package that implements it.
```

⚠️ 每条可粘贴的行都是**承诺**,含其中的版本号。示例里的版本必须与索引当天的
latest 一致,并在发布新版时同批更新(已复发过两次)。

---

### 14.4 #492 的验收清单

| # | 开发者需要什么 | 现状 | 谁来做 |
|---|---|---|---|
| 1 | 包能声明 `mcpp:c++-abi` | ✅ **已实测** | — |
| 2 | 包能供 `std.cppm` + 自己的 flags | ✅ **已实测** | — |
| 3 | std 模块能带 `--target=` 交叉编译 | ✅ **已实测**(产出 `std.o`+`std.compat.o`) | — |
| 4 | `[target.X].sysroot` 能在 hosted 目标上被**解析** | ✅ **已实测**(报告显示 `prebuilt`) | — |
| 5 | 同一份 sysroot 被**渲染**到编译/链接行 | ❌ | **A2 —— 唯一有分量的一条** |
| 6 | `--toolchain llvm` 不被词表 pin 盖掉 | ❌ | §7.1(#492 已修一半,漏了全局默认) |
| 7 | 图供 libc++ × gcc ⇒ 拒绝而不是乱编 | ❌ | §2 |
| 8 | 冲突与身份可见 | ❌ | S3 / S4 |

⭐ **1–4 今天就成立。引擎侧真正要做的是 5,其余三条是诊断质量。**
而第 5 条是**已有列的解绑**,不是新机制 —— 与 #492 的 349 行相比,
它同时服务 freestanding、hosted-musl 和未来任何「编译器不带目标 libc」的组合。

#### 做完之后,#492 的开发者做什么

发一个包,形如:

```toml
[package]
namespace = "<his-ns>"
name      = "llvm-musl-runtime"
version   = "0.1.0"
provides  = ["mcpp:c++-abi=libc++"]
requires  = ["mcpp:compiler=llvm"]
std-module        = "llvm-generated/std.cppm"
std-compat-module = "llvm-generated/std.compat.cppm"
std-module-flags  = ["--no-default-config", "-nostdinc", "-nostdinc++", "-D_GNU_SOURCE"]

[build]
sources      = [ ... libc++ / libc++abi / libunwind 的源码 ... ]
include_dirs = [ ... ]
```

**引擎一行不改,索引一行不加。** 使用者写两行:

```toml
[dependencies]
llvm-musl-runtime = "0.1.0"

[target.x86_64-linux-musl]
toolchain = "llvm@22.1.8"
sysroot   = "xim:musl-gcc@16.1.0"
```

⚠️ 最后两行将来可以由 target 表的列提供默认(那时使用者只写第一行),
但**列是便利,不是机制** —— 机制是 `[target.X]` 那两个键,它们今天就存在。

#### ⚠️ A2 需要先做的一次实测

clang 找 crt/libgcc 靠 `--gcc-toolchain`,而 picolibc 那类是纯 sysroot
(只有 include/lib)。**提案**:sysroot 来源是 `Xpkg` 且编译器是 clang 时,
无条件同时给 `--sysroot=<root>` 与 `--gcc-toolchain=<root>` ——
若载荷不是 gcc 形状,clang 在那里找不到 GCC 安装,退回 `--sysroot`,
即 picolibc 今天已经在走的路。

⚠️ **这是设计假设,不是实测结论。** 落地前必须先验证两件事:
(a) 给 picolibc 的 sysroot 额外加 `--gcc-toolchain` 是否零影响(裸机四个目标全跑);
(b) 给 musl-gcc 载荷加上后 crt/libgcc 是否真的解析到载荷内(hermetic 检查转绿)。
若 (a) 不成立,退化方案是给 sysroot 列加一个形状标记 —— 但那是数据,仍不是分支。

---

### 14.5 落地顺序(替换 §10 与 §12.8)

```
第 0 批 · 低风险 · 各自可独立提 PR
  S4     Provider::id() 打全限定名                    一行,纯输出
  A3     Family 删 OpenkalLlvm(别名兜底)             e2e 269 已在守
  §7.1   GlobalDefault 纳入 user-explicit + 状态行说明覆盖
  §4B.3  openkal-windows 的 cfg 改 not(env="msvc")    包侧 PR,已实测
  §7.4   compile_commands 默认收窄                    333ms + 8.4MB

第 1 批 · 性能 · 与语义解耦
  §5     staging 按包分组(1624 → ~10 条边)          ← 收益最大
  §6     缓存身份去路径 / 归一拼写 / 原子提交
  §7.3   补上那 600ms 静默的两条状态行

第 2 批 · ⭐ 解锁 #492
  A2     sysroot 列的物化与渲染从 freestanding 解绑    ← 先做 §14.4 的实测
  S3     每层唯一供给者                              ← 先扫索引确认无现存冲突
  §2     compiler 进 TargetSide + requires 语法

第 3 批 · 语义地基
  S1     env 三态 + 请求/事实分离                     ⚠️ 改动面最大,先做统计
  S2     cfg 的 c-abi / kernel-abi / c++-abi 维度
  §13.1尾 pack 的 cfg_predicate_for 一起改
  §3     词表不再是门(依赖 §2 的诊断兜底)
  A1     删 llvmSysroot(此时 #492 的其余部分可撤)

第 4 批
  §8     文档 + example + 目标表
  SPEC-002
  §9     测试矩阵补格

明确不做
  parse() 开放 os/env token        把语义责任推给包作者(§12.5.1)
  Family 开放                      同上
  命名空间准入门槛                  把瓶颈换个仓库(§13.3)
  目标侧包预构建                    违反 2+N+M 判据(§13.1)
```

⭐ **第 2 批做完,验收判据即成立** —— #492 的开发者只维护一个包。
第 3 批是让这件事在语义上也说得通;第 0/1 批与它正交,可以并行。

---

## 15. `sysroot = "xim:musl-gcc@16.1.0"` 为什么读起来是错的

§14.3 的示例里有一行:

```toml
[target.x86_64-linux-musl]
toolchain = "llvm@22.1.8"
sysroot   = "xim:musl-gcc@16.1.0"
```

**llvm 的构建里出现一个 gcc 包 —— 这行读起来是错的,而它确实是错的。**
三层混乱叠在一起,每一层都可定位。

### 15.1 报告自己就是证据

同一个字段位置,两条来源给出性质不同的东西(⭐ 两行都是实测):

```
c-abi   musl-gcc   (xim:musl-gcc@16.1.0, prebuilt)     ← 载荷来源:包名
c-abi   musl       (openkal-musl@0.3.3, graph)         ← 图来源:接口名
```

`targetside/model.cppm` 逐行对照:

```cpp
// 图来源 —— 包自己声明的
ts.cAbi = { Origin::Graph, in.cAbi->display_interface(), in.cAbi->id(), false };
//                         └─ provides = ["mcpp:c-abi=musl"] 里的 "musl"

// 载荷来源 —— 从包名切出来的
ts.cAbi = { Origin::Xpkg, xpkg_interface(in.sysrootXpkg), in.sysrootXpkg, false };
//                        └─ "xim:musl-gcc@16.1.0" → 砍掉 ns 和版本 → "musl-gcc"
```

⭐ **`Graph` 来源有「接口 / 实现」分离,`Xpkg` 来源没有。**
`xpkg_interface()` 的注释写着「the interface a reader wants to see is the name」——
它把**包的名字**和**接口的名字**当成了同一个东西,而这正是
`Layer` 结构体开篇花二十行论证过的、必须分开的两样:

> `openkal` is an interface; `openkal-macos`, `openkal-windows` … are
> implementations of it. Collapsing them would hide the fact that one source
> reaches four machines because four packages answer to one name.

⚠️ 同一个缺陷在裸机行上也成立(读码推得,未实测):
`xim:picolibc-riscv@1.8.12` → 接口名会显示成 `picolibc-riscv`,
而接口是 `picolibc`,`-riscv` 是打包细节。

### 15.2 三层混乱,逐层拆开

**(1) 键名是 flag 名,不是角色名。**
`sysroot` 是 `--sysroot` 这个编译器选项的名字。而这个字段的**文档含义**
(`triple.cppm:101`)是「**The TARGET's C library**」。角色叫 `c-abi`,
机制才叫 sysroot。用机制命名角色,读者只能靠猜。

**(2) 值点名一个包,而不是一种能力。**
用户想说的是「这个目标的 C 库是 musl」。`xim:musl-gcc@16.1.0` 说的是
「那个碰巧装着我要的东西的包」。`musl-gcc` 这个载荷里有 gcc、musl、libgcc、
libstdc++ 四样,而这里只想要中间两样 —— 名字里却是第一样。

**(3) 版本是错的那个东西的版本。**
`@16.1.0` 是 **GCC** 的版本号。musl 自己的版本(1.2.x)在这行里根本不可见,
也无法指定。⇒ 「我要 musl 1.2.5」在这套语法里说不出来。

### 15.3 修法:让载荷也声明,而不是被猜

⭐ **根本修法只有一条:`Xpkg` 来源使用与 `Graph` 来源相同的能力声明。**

xim 描述符增加同一套 `mcpp:` 能力键:

```lua
-- xim-pkgindex/pkgs/m/musl-gcc.lua
provides = { "mcpp:c-abi=musl" }          -- 我供给哪一层,接口叫什么
provides_version = { ["mcpp:c-abi"] = "1.2.5" }   -- 那一层的真实版本
```

于是:

- `xpkg_interface()` 这个**猜测函数被删掉**,两条通路收敛到一处;
- 报告变成 `c-abi musl (xim:musl-gcc@16.1.0, prebuilt)` —— 接口正确,
  实现地址仍然可见,这正是 `Layer` 结构体想要的两个字段;
- 「哪个包供给 musl」变成**可查询的数据**,而不是使用者要记住的知识。

### 15.4 用户侧的最终形态:那一行根本不该出现

分离之后,`[target.X]` 的键按**角色**命名,值按**接口**书写:

```toml
[target.x86_64-linux-musl]
toolchain = "llvm@22.1.8"
c-abi     = "musl"                          # 说的是「要 musl」
```

⚠️ 三态必须保留(与 `[target.X].sysroot` 今天的三态一一对应,
理由见 `triple.cppm:207-217`):

```toml
# 键缺席        → 目标表的列说了算
c-abi = "musl"  # → 我要 musl,由索引解析成载荷
c-abi = "none"  # → 零 libc 档(今天的 sysroot = "")
c-abi = { prebuilt = "xim:musl-gcc@16.1.0" }   # → 逃生口:直接点名
```

⭐ **而绝大多数情况下这一行也不该出现** —— 目标表的行本来就该给出默认:

```toml
[target.x86_64-linux-musl]
toolchain = "llvm@22.1.8"
```

`xim:musl-gcc` 这个字符串留在 mcpp 自己的表里,那是 mcpp 的事,不是使用者的事。
使用者只在**想换掉默认**时才写 `c-abi`。

⚠️ 兼容性:`sysroot` 作为**已废弃别名**保留,语义不变。裸机生态已经有清单在用
`sysroot = "xim:picolibc-riscv@1.8.12"` 与 `sysroot = ""`,不得失效。

### 15.5 这条与 §14 的关系

§14 说「模型完备,缺的只是消费」—— 那是就**机制**而言。
本节说的是:**机制完备不等于语义清晰**。`Origin::Xpkg` 这一支能跑通,
但它在两个地方比 `Origin::Graph` 弱一级:

| | `Graph` | `Xpkg`(今天) | 修法 |
|---|---|---|---|
| 接口名 | 包声明 | **从包名猜** | §15.3 |
| 层的版本 | 包的版本即该层版本 | **是载体的版本** | §15.3 |
| 用户如何指定 | 一条普通依赖 | **点名一个载荷地址** | §15.4 |

⇒ 这三条进 §14.5 的**第 2 批**(与 A2 同批):A2 让 `Xpkg` 能被渲染,
§15 让它读起来是对的。**只做 A2 会得到一个能用但语义混乱的机制** ——
而语义混乱正是本文开头那十条问题的共同来源。

### 15.6 落地顺序增补

```
第 2 批 · 解锁 #492(增补)
  A2      sysroot 列的物化与渲染从 freestanding 解绑
  §15.3   xim 描述符声明 mcpp: 能力;删掉 xpkg_interface() 的猜测
  §15.4   [target.X] 的键改为 c-abi(sysroot 降级为兼容别名)
  S3      每层唯一供给者
  §2      compiler 进 TargetSide + requires 语法
```

⚠️ §15.3 需要 xim 侧配合(描述符新增键),排期上要与 `xim-pkgindex` 协调;
而 §15.4 是纯 mcpp 侧,可以先行 —— 先行时 `c-abi = "musl"` 暂时只接受
`{ prebuilt = "…" }` 形式,接口名解析等 §15.3 到位。

---

## 16. ⚠️ 撤回 A2:`--gcc-toolchain` 的借用是错的

§14.4 把「把 clang 指向目标的 C 运行时」定为 #492 唯一不可约的引擎改动,
并按 #492 的写法采用了 `--gcc-toolchain=<musl-gcc 载荷> -rtlib=libgcc
-unwindlib=libgcc`。**这个方案作废。** 它为了少改一点东西,把架构和语义弄乱了。

### 16.1 借的到底是什么:编译器运行时

实测 `openkal-llvm-runtime` 编了什么,按子系统:

```
compiler-rt   498 个对象    ← 68%
libcxx        159
libcxxabi      51
libunwind      21
```

⭐ **这个包最大的一块是 compiler-rt,而它声明的能力只有 `mcpp:c++-abi=libc++`。**
compiler-rt builtins 是**C 程序**就需要的东西(`__udivti3`、`__muloti4`…),
与 C++ 毫无关系。

而 `#492` 借的正是这一块的 GCC 版本:

| | 编译器运行时(builtins + unwinder) |
|---|---|
| openkal | LLVM 的 compiler-rt + libunwind —— **自己编,498 + 21 个对象** |
| #492 | **GCC 的 libgcc / libgcc_eh** —— 从 musl-gcc 载荷借 |

⇒ 「llvm 怎么和 musl-gcc 混到一起」的准确答案:
**#492 让 clang 从 GCC 拿走它自己的编译器运行时。**

### 16.2 代码自己就反对这件事

`src/toolchain/linkmodel.cppm:188-196`:

```cpp
static constexpr std::string_view kLinkDriverFlags =
    " -stdlib=libc++ -fuse-ld=lld --rtlib=compiler-rt --unwindlib=libunwind";

// The same selection for a link that has no C++ in it (mcpp#426). Only
// `-stdlib=` comes off: **the compiler runtime and the unwinder are just as
// much a C decision**, and dropping them would make a C link resolve
// __udivti3 differently from every other link in the same build.
static constexpr std::string_view kLinkDriverFlagsC =
    " -fuse-ld=lld --rtlib=compiler-rt --unwindlib=libunwind";
```

⭐ mcpp#426 已经把「编译器运行时是一条独立的、全构建必须一致的轴」写下来了 ——
理由就是「否则一次 C 链接解析 `__udivti3` 的方式会和同一次构建里其它链接不同」。

**而 A2 允许某一个目标把这条轴静默翻成 libgcc,正是那条注释在防的事。**

⚠️ 并且这条轴今天有**三条互不相干的通道**:

```
引擎     linkmodel.cppm 的字符串常量     --rtlib=compiler-rt --unwindlib=libunwind
#492     它自己分支里的字符串            -rtlib=libgcc -unwindlib=libgcc
openkal  一个包里的 498 个对象           声明成 mcpp:c++-abi=libc++(名不副实)
```

「一个事实三条通道」正是 `targetside` 模块被创建出来消掉的形状。

### 16.3 模型缺第五层

四层里没有编译器运行时的位置,所以它只能藏在 flag 字符串里,
于是「llvm 配 libgcc」这种不协调**只有人眼能看出来,机器看不出来**。

**五层模型**:

```
compiler      谁在编译             llvm / gcc / msvc
compiler-rt   编译器自己的运行时     compiler-rt+libunwind / libgcc / MSVC 的
kernel-abi    平台接口             linux / windows / openkal
c-abi         C 库                 glibc / musl / picolibc / 无
c++-abi       C++ 库               libc++ / libstdc++ / MSVC STL
```

⚠️ **compiler-rt 必须与 c++-abi 分开,而这已经被实测过一次**:
`targetside/model.cppm` 开篇记录的那个缺陷,就是「一个纯 C 程序交叉到 macOS,
图里没有 C++ 运行时」。那个程序**仍然需要 builtins**。
把 builtins 塞在 `c++-abi` 名下,等于说一个 C 程序不需要 `__udivti3`。

加上这一层之后:

```
compiler      llvm@22.1.8   (payload)
compiler-rt   libgcc        (xim:musl-gcc@16.1.0, prebuilt)    ← ⚠️ 一眼看出不对
c-abi         musl          (xim:musl-gcc@16.1.0, prebuilt)
c++-abi       libc++        (…/llvm-musl-runtime@0.1.0, graph)
```

**规则二(上层必须为下层配置过)可以直接拒绝这一格** ——
不协调从「靠人眼」变成「机器可检查」。

### 16.4 替代方案:只读 musl 自己的文件,不需要重新打包

实测 `xim-x-musl-gcc/16.1.0` 的布局:

```
x86_64-linux-musl/                      ← 纯 musl sysroot,零 GCC
├── include/                               musl 的头
└── lib/crt1.o  crti.o  crtn.o          ⭐ 这三个是 musl 自己的
lib/gcc/x86_64-linux-musl/16.1.0/
├── libgcc.a                            ← GCC 的,在 sysroot 之外
└── libgcc_eh.a
```

⭐ **musl 的 sysroot 子目录里一行 GCC 都没有。** `--gcc-toolchain=<载荷根>`
是刻意伸到那个子目录**之外**去拿 libgcc 的。

⇒ 正确的接线:

```
--sysroot=<载荷>/x86_64-linux-musl        只读 musl 的头、libc.a、crt1/crti/crtn
--rtlib=compiler-rt --unwindlib=libunwind  编译器运行时来自图(或 llvm 载荷)
                                           ⇒ 消费的每一个文件都属于它声称的那一层
```

**没有 `--gcc-toolchain`,没有 `-rtlib=libgcc`,不需要重新打包 xim。**

配合 §15.3(xim 描述符声明自己供给哪层、在哪个子目录),
`xim:musl-gcc` 这个名字也不再出现在使用者面前:

```lua
-- xim-pkgindex/pkgs/m/musl-gcc.lua
provides       = { "mcpp:c-abi=musl" }
provides_path  = { ["mcpp:c-abi"] = "${arch}-linux-musl" }   -- 层在载荷里的位置
provides_version = { ["mcpp:c-abi"] = "1.2.5" }
```

⇒ 报告写 `c-abi musl (xim:musl-gcc@16.1.0, prebuilt)` —— 诚实:
musl,由那个包供给。载荷叫什么名字是打包的事,不是语义的事。

### 16.5 修订后的 #492 验收清单

| # | 需要什么 | 现状 | 谁做 |
|---|---|---|---|
| 1–4 | 包声明能力 / 供 std 模块 / 交叉编译 / xpkg 被解析 | ✅ **已实测** | — |
| 5 | **单一角色的 sysroot 被渲染**(只 `--sysroot`,不 `--gcc-toolchain`) | ❌ | 引擎 |
| 5b | xim 描述符声明层与子目录 | ❌ | xim-pkgindex(§15.3) |
| 6 | `compiler-rt` 成为第五层并被检查 | ❌ | 引擎(§16.3) |
| 7 | `--toolchain` / 全局默认不被词表 pin 盖掉 | ❌ | §7.1 |
| 8 | 图供 libc++ × gcc ⇒ 拒绝 | ❌ | §2 |
| 9 | 冲突与身份可见 | ❌ | S3 / S4 |

而**开发者维护的那个包**要供给两层,不是一层:

```toml
provides = ["mcpp:compiler-rt=compiler-rt", "mcpp:c++-abi=libc++"]
requires = ["mcpp:compiler=llvm"]
```

⭐ 这正是 `openkal-llvm-runtime` 今天已经在**做**的事(498 + 21 + 159 + 51),
只是它今天只**说**了其中一半。

### 16.6 ⚠️ 唯一需要先实测的一件事

去掉 `--gcc-toolchain` 之后,clang 静态链接 musl 是否还需要
`crtbegin.o` / `crtend.o`。本机检查:llvm 载荷里**没有** `clang_rt.crt*.o`。
两种可能:

- clang 在 musl/静态下根本不发它们(musl 用 `.init_array`,不需要)⇒ 直接可行;
- 需要,则由图里的运行时包供给(compiler-rt 本来就能构建它们)⇒ 包多编两个文件。

⚠️ **这是本节唯一的未知数,必须先测再动手。** 测法:手工拼一条
`clang --target=x86_64-unknown-linux-musl --sysroot=<载荷>/x86_64-linux-musl
--rtlib=compiler-rt --unwindlib=libunwind -static` 的链接,看能否产出可运行的 ELF。

### 16.7 对 §14.5 落地顺序的修订

```
第 2 批 · 解锁 #492(修订)
  §16.6   先实测:去掉 --gcc-toolchain 后 musl 静态链接是否成立
  §16.3   compiler-rt 成为第五层(模型 + 报告 + 规则二检查)
  A2'     单一角色 sysroot 的渲染(⚠️ 不是 A2:不发 --gcc-toolchain,不改 --rtlib)
  §15.3   xim 描述符声明层 / 子目录 / 层版本
  §15.4   [target.X] 的键改为 c-abi
  S3      每层唯一供给者
  §2      compiler 进 TargetSide + requires

明确不做(增补)
  --gcc-toolchain 借用 / -rtlib=libgcc
        ← 让一次构建里的 __udivti3 与其它链接不一致(mcpp#426 已论证);
          为省一次打包/一层建模而牺牲语义,是本文反对的那类捷径
```

---

## 17. 实测回填 —— 落地之后,本文有哪几处是错的

本节写于 2026.8.24.3 发布之后。只记录**推翻或修正了上文判断**的部分;
按计划做成了的事不在此列,因为它们已经在代码和 docs/14、docs/15 里。

### 17.1 §11「§4.1 改动面有多大」—— 我给出的窄改法阈值定错了方向

上文要求实施前先做纯统计,并说「命中点超过 ~30 处,应改为保留自动填充
另存 `envExplicit` 布尔」。实测命中 **22 处,其中 10 处集中在 `triple.cppm`**。

按本文的规则,22 < 30,应当走「删掉自动填充」的宽改法。**而正确的做法
仍然是窄改法**,理由与命中点数量无关:`x86_64-linux-gnu` 是
`mcpp toolchain list` 打印的东西,因此也是人们照抄的东西,删掉填充会让
这个拼写变成一个**没有 C 库的三元组**。

⭐ 判据从来不是「要改多少处」,而是「哪一个是身份、哪一个是请求」。
两者都要留:规范形式当身份(输出目录、缓存键、`cfg()` 的主语),
`envExplicit` 记住请求。一个用改动面大小来选设计的规则,选出的是省事的
那个,不是对的那个。

### 17.2 §11「gcc 能否消费 libc++ 的 std 模块」—— 仍然没测,而这是对的

本文说「若有人要去证明它可行,那是一次实测,不是一次推理」。落地时没有
去做这次实测,而是让 `openkal-llvm-runtime` 声明
`requires = ["mcpp:compiler=llvm"]`,在编译任何东西之前拒绝这个组合。

保留这条记录是因为**「不去回答一个问题」也是一种设计决定**,而它需要一个
理由:能否让 gcc 编 libc++ 的模块,答案无论是什么都不会改变这个包的正确
配置,所以那次实测的成本不换来任何决策。

### 17.3 §12.3「二进制分发不能声明 provides」—— 已被实验否掉

上文断言二进制分发形态无法声明层能力,并据此推导出一整套需要引擎改动的
方案。**这是错的。** 手写一个 `sources = []` + `provides` + `std-module`
的无源码包,零引擎改动即可占住 c++-abi 层。§12.3 已在本文修正。

⚠️ 这一条与 17.1 是同一种错误:**用「做不到」当推理的前提,而没有去试**。
[[reasons-written-from-memory-kill-good-fixes]]

### 17.4 新层名的发布门槛 —— 本文完全没有涉及,而它决定了能不能发

上文把 `mcpp:compiler-runtime` 当作一个模型问题,没有问「一个已发布的
构建工具读到这个键会怎样」。实测,一个依赖声明一个键:

| 键 | 2026.8.19.4 | 2026.8.24.1 | 2026.8.24.3 |
|---|---|---|---|
| `requires = ["mcpp:compiler=llvm"]` | exit 0 未校验 | exit 0 未校验 | exit 0 解析 |
| `provides = ["mcpp:compiler-runtime=…"]` | exit 0 **未校验** | **exit 2 拒绝** | exit 0 解析 |

⭐ **两个 exit 0 不是同一种成功。** 19.4 早于层词表,整个数组被当作不认识
的键忽略 —— 它既不拒绝也不读。一个 pin 在那里的 CI 会在一份中心主张从未
被检查过的清单上报绿。因此生态包声明新层时,`MCPP_VERSION` 必须同一改动
里上移,理由不是兼容性而是**让断言有人执行**。

拒绝窗口只有 2026.8.24.1 一个版本。判据是**索引服务哪个版本**,不是本包
声明的 floor,也不是「新 mcpp 支持了」。

### 17.5 一次撤回:把规范的机制当成了缺陷

落地过程中,示例程序在 `riscv64-none-elf` 上链接失败:

```
ld.lld: error: undefined symbol: kal_fs_props
>>> referenced by fs.cppm:136
```

据此给 `openkal-opensbi` 补了 `kal_fs_props = 0`,发布 0.1.3 并合入索引。
**全错。** openkal SPEC 6.1:「实现不提供的接口,作为链接期定义是缺席的,
使用它的消费者链接失败。」6.2 的表把时机分开 —— 链接器回答「是否用了它
不提供的接口」,能力字回答「在**它提供的接口内**它如何表现」。

给一个没有任何操作的接口定义能力字,是回答第二个问题而跳过第一个。
已发 0.1.4 撤回,索引里 0.1.3 的条目整个删除。真正错的是**程序**:
一份可移植源码可以问「这个文件系统怎样比较名字」,不可以问「这台机器
有没有文件系统」。示例因此从「一份源码四台机器」改为三台宿主目标。

⚠️ 与 17.1、17.3 同族,而这次更贵:**报错信息告诉你什么坏了,它不是规范。**

### 17.6 落地后的实测数字

| 项 | 值 |
|---|---|
| 单元测试 | 93 通过 / 0 失败 |
| e2e(本机) | 289 通过 / 25 失败,零新增失败,修好 1 条既有失败 |
| `prepare.cppm` 实质改动 | +153 / −25(其余 745 行是 lambda 缩进) |
| 五层中由图供给 | 4 层(仅 compiler 来自载荷) |
| 沙箱行为验证 | 13 断言全通过,mcpp 2026.8.24.3 自索引装出 |
