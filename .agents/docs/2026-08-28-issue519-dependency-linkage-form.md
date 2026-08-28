# issue #519:依赖的链接形态 —— 一条不变量,两个高度

2026-08-28 · 架构分析 + 设计方案(**待 review,尚未实施**)

核实基线:mcpp `f2dbeec`,mcpp-index `8f5ad30`(2026-08-27)。

> ⚠️⚠️ **范围约束(实施前定下,不可让步)**:mcpp 侧只做**通用底座**。
> `mcpplibs/mcpp-index#245`(eui-neo 的 Linux SNI 托盘)是这条 issue 在真实世界里的
> 实例,用来**验证**这套机制,**绝不用来驱动设计** —— 引擎里不得出现任何
> glib / gio / zlib / 托盘 的知识,也不得为了让那一个包通过而加特例。
> 判据很简单:把 §4、§2 的规则里所有专有名词划掉,规则本身必须仍然完整。
> 同样地,通用实现只依赖 mcpp + xlings 体系,不依赖宿主。
下文标「实测」的每一条都附了命令与读数;标「读码」的附了文件行号。

---

## 0. 一句话

> **一个链接映像里,一个库只允许有一个提供者、一种形态。**

issue 提的两件事,是这条不变量的两半:

- issue §4.3 的诊断 = **执行**这条不变量;
- issue §4.1/§4.2 的形态轴 = 当映像里确实出现两个提供者时,**唯一能让用户满足它的杠杆**。

所以它们不是「先做哪个」的取舍,而是**一个没有杠杆的不变量只是一堵墙**。
但顺序仍然是诊断在前,因为诊断能独立正确,而形态轴单独落地会把一个
**可检测的缺陷换成一个不可检测的缺陷**(§5.3,这是本文最重要的一条修正)。

而不变量的执行有**两个高度**,因为 mcpp 有两种知识:

| | 知道什么 | 何时 | 覆盖 | 看得见 §2 的复现吗 |
|---|---|---|---|---|
| **D1 声明层** | mcpp 自己决定的东西(包、目标、形态) | plan 期 | 全平台、精确 | **看不见**(vendor 包里的 `libz.so.1` 不是 mcpp 的概念) |
| **D2 测量层** | 链接器实际产出的东西 | 链接后 | 仅 ELF | **看得见**,而且是唯一看得见的 |

「断言 + 核验」是这个仓库的既有形状(`scan_overrides` 断言 + P1689 核验;
pack 的 interface digest)。这里是同一个形状的第三次应用。

---

## 1. issue 的判据核实

### 1.1 成立的

| issue 陈述 | 核实 |
|---|---|
| `kind="lib"` 的对象全量入消费者链接行,无归档懒选 | 读码成立。`plan.cppm:1591` `append_package_objects` / `1782` 循环 |
| `kind="shared"` 在消费者 `bin/` 真建 `.so` | 读码成立。`plan.cppm:1607-1621` |
| 消费者对此没有发言权 | 成立。形态只由被依赖包的 `Target::kind` 决定 |
| `-fPIC` 已经是全局的 | 读码成立。`flags.cppm:444-451`,任一 shared link unit 存在即全局 PIC |
| compile-once ⇒ 同一组对象能同时喂两条路 | 成立,`docs/05` §2.2 明文 |
| `[target.*].linkage` 是另一根轴(libc/CRT) | 成立,且**只在精确 triple 表下解析**(`prepare.cppm:2026` 注释:"There is no `[build].linkage`") |
| ELF 全局扁平命名空间导致劫持,不是 bug | 成立 |
| 没有任何机制阻止或诊断混形态 | 成立 —— 这是真正的缺口 |

### 1.2 ⚠️ 三条需要修正

**(a) `required_features` 门控在依赖包上从来不生效。**

issue §3.3 说「开了 `shared` 之后两个 target 都会出 ⇒ 重复符号」。读码后是另一回事,
而且更糟:

- 目标门控只有一处:`prepare.cppm:5402` 的 `std::erase_if(m->targets, …)`,
  `m` 是**根 manifest**,判据是 `activeRootFeatures`。**依赖包的 `targets` 一个都不过滤。**
- 所以 `zlib-shared = { kind="shared", required_features={"shared"} }` 会**无条件**建出来,
  对每一个消费者都建,不管有没有开那个 feature。
- 而重复符号**不会**发生:`plan.cppm:1475` 一旦把该包记入 `sharedDepPackages`,
  `1663` 与 `1783` 就把该包的**全部**编译单元排除出消费者的链接行。

结论方向一致(这条路不通),但理由完全不同。**这是本次核实新挖到的独立缺陷**:
一个描述符可以用 `required_features` 写出一个自以为可选、实际强制的目标。

**(b) `kind = "lib"` 不是断言,是解析器的默认值。**

issue §4.1 提议「未声明 = 两种形态都支持,声明 = 有约束」。对着装机量核一遍:

| | 数量 | 说明 |
|---|---|---|
| mcpp-index 包总数 | 130 | `pkgs/*/*.lua` |
| 显式 `kind = "shared"` | **12** | 全是 `compat.x11` 家族 + `xcb` + `vulkan` |
| 显式 `kind = "lib"` | **84** | |
| 完全没有 `targets` 块 | 33 | |

而 `xpkg.cppm:1435` 是 `t.kind = Target::Library;  // default`。
也就是说 **84 个包写下 `kind = "lib"` 是在复述默认值,不是在主张约束**;
按 issue 的字面读法,这 84 个包会被永久冻结成 static-only,轴对它们完全失效 ——
而它们正是这根轴要服务的对象。

⚠️ 索引描述符**按版本冻结**,不能要求改 84 个包(记忆:「索引是数据 mcpp 是程序」)。

**幸运的是不对称读法既安全又正确**:

| 描述符说 | 读作 | 为什么安全 |
|---|---|---|
| `kind = "shared"` | **约束:必须 shared** | 12 次全部出于同一个真实理由(会被第三方 `dlopen`,进程里必须唯一) |
| `kind = "lib"` / 缺席 | **无约束** | 它是默认值,117 个包没有选择过它 |

**零描述符改动**。这是整个设计里最省的一步。

**(c) 「改成 shared」不会自动解决 §2 的复现。** 见 §5.3,单独列。

### 1.3 ⭐ 代码里写下了这条不变量,但没有任何东西在执行它

`src/build/distribution.cppm:306-310`:

> An executable's static libstdc++ is already local (ld exports only what a
> loaded object references, and mcpp passes no `-rdynamic`)

括号里的**机制描述是对的**,括号外的**结论是错的**:当确实有一个已加载对象引用了它时,
它就不是 local 的 —— 而这正是 §2 的场景。这条注释同时给出了 §2 缺陷的成因
和一个从未被执行的不变量。(同型记忆:「上层假设了下层从未承诺的性质」、
「写在注释里的约束没有任何东西在执行它」。)

⭐ 好消息是:这句话反过来读,就是 D2 的判据。

---

## 2. D2:判据全部来自测量,不需要任何声明

### 2.1 谓词(⚠️ 已被第二轮 review 修正两次,见 §12.1)

适用条件:产物有 `PT_INTERP`(即它是要被加载器启动的可执行文件),
且链接行里没有 `-rdynamic` / `--export-dynamic` / `--dynamic-list`
(mcpp 从不发这些 —— 全仓 grep 只在注释里出现)。

**第一段(便宜,总是跑)—— 这个映像导出了什么静态定义:**

```
EXPORTED(exe) = { s ∈ .dynsym(exe) : DEFINED
                                   ∧ ¬(s 是 copy relocation) }

s 是 copy relocation  ⇔  ∃ R_<arch>_COPY 重定位,其 r_offset == s.st_value
```

`STT_FUNC` 的已定义条目**永远**属于 `EXPORTED` —— copy relocation 不会是函数。
只有 `STT_OBJECT` 需要走上面那条重定位判定。

**第二段(仅当 `EXPORTED` 非空)—— 有没有第二个提供者:**

```
CONFLICT(exe) = { s ∈ EXPORTED(exe) : ∃ o ∈ closure(exe), o 也 DEFINE 了 s }
```

`CONFLICT` 非空 ⇔ **同一个符号在这个进程里有两份定义,而静态那份赢**。
这正是 §0 的不变量,只是求值在产物上而不是在 plan 上。

⚠️⚠️ **第二段不能省。** 见 §12.2:mcpp 自己的 `kind = "shared"` 机制
**结构性地**产出「exe 导出、`.so` 绑过来」这个形状,而那是单份定义、良性的。
只看第一段会在合法安排上误报。

### 2.2 实测基线

```console
$ readelf --dyn-syms -W <exe> | awk 'NR>3 && $7!="UND" && $8!=""'
$ readelf -r -W <exe> | grep COPY
```

| 产物 | ELF 类型 | 已定义 / 总 | 符号类型 | COPY 重定位 | `EXPORTED` |
|---|---|---|---|---|---|
| `target/x86_64-linux-gnu/*/bin/mcpp`(四个指纹目录读数一致) | **EXEC** | 5 / 217 | 5×`OBJECT` | **4 条**,覆盖 5 个符号(`environ` 与 `__environ` 同址) | **0** |
| `/usr/bin/git` | **DYN**(PIE) | 1 / 257 | `FUNC error` | 0 | **1** —— 遮蔽 glibc 的 `error(3)` |
| `/usr/bin/ls` | **DYN**(PIE) | 7 / 127 | 6×`FUNC` + 1×`OBJECT` | 0 | **7** —— gnulib obstack vs glibc |
| `/usr/bin/bash` | DYN(PIE) | 2339 / 2578 | —— | —— | 前置条件不成立(`-rdynamic`),**正确排除** |

**mcpp 自己产出的映像 `|EXPORTED| = 0`(分母 217)**,诊断在正常项目上静默。
上表同时是这条规则的四点回归集:两个真阳性、一个真阴性、一个正确排除。

⚠️ 表里两处**推翻了第一轮的写法**:

1. **ELF 类型不能当判据**。mcpp 的产物是 `EXEC`,而 `git`/`ls` 是 `DYN`(PIE)。
   全仓 grep 没有任何 `-pie` / `-no-pie` / `-fPIE` ⇒ 这完全取决于载荷工具链的默认值,
   会在我们脚下变。判据是 **`PT_INTERP` 存在**,而
   `ElfRuntimeFacts::interp` 这个字段**已经在记它了**。
2. **copy relocation 的判定必须按地址,不能按名字**。`environ` 是 `__environ` 的
   WEAK 别名,同址但没有以自己的名字出现在重定位表里 ——
   按名字判会在**每一个** mcpp 产物上误报 `environ`。

### 2.3 归因只在失败时付费

`CONFLICT` 的计算本身已经找出了**共享一侧**(closure 里那个也定义了该符号的对象,
经 `elf_runtime::resolve_runtime_closure` 的 `resolvedObjects`)。
**静态一侧**再补一步:mcpp 知道自己把哪些 `.o` 喂进了这条链接边、
每个 `.o` 属于哪个包(`CompileUnit::packageName`),只对冲突的那几个符号名读 `.symtab`。

⭐ 代价的形状:第一段是一次 `.dynsym` 遍历;第二段的输入集**实测是 0–7 个符号**,
乘以一个典型闭包(个位数对象)。清洁构建根本走不到第二段。

### 2.4 落点:`mcpp.build.runtime_validation`

这个模块的开篇就是「validate only freshly linked Linux ELFs」——
按 stat 快照决定是否重跑、判据持久化在 `build.ninja` 旁、doctor 能复读。
D2 的作用域和它**逐字重合**,复用它的快照/缓存/持久化机制,不是新模块。
⚠️ 但**不复用 `RuntimeVerdict::Status`** —— 那个四值枚举的两个 blocking 态回答的是
「这个产物能不能启动」,而符号劫持的产物跑得好好的。理由与严重度见 §11.4。

`elf_runtime` 已经解析动态段、`DT_VERNEED`/`DT_VERDEF`、`PT_INTERP`,并解析出闭包
(`resolvedObjects`)。新增的是 `.dynsym` 遍历(`DT_SYMTAB` + `DT_STRTAB` +
`DT_HASH`/`DT_GNU_HASH` 取表长)与 `.rela.dyn` 的 `R_<arch>_COPY` 扫描。

### 2.5 D2 不覆盖什么

- **PE / Mach-O 不需要它**:PE 按 `(DLL 名, 符号)` 解析,Mach-O 默认 two-level
  namespace。issue §5 的表已经说对了。
- **`.so` 之间的重复**不在 D2 的谓词里(共享库本来就导出一切)。那是闭包里
  **SONAME 撞车**的问题,`resolve_runtime_closure` 已经看得见,属于后续。

---

## 3. D1:声明层,一个映像一种形态

D2 是核验,D1 是断言 —— 它在 plan 期、零 ELF 解析、全平台成立:

> 一个链接映像里,同一个包解析出的形态必须唯一。

⭐ **D1 不是一个独立的检查器,它就是 §4 那个解析器的错误路径。**
`resolve_form()` 是总函数:对一个(包, 映像)只返回一个 `Form`;
两条边请求不同形态时它没有答案可返回,于是报错并指名两条边。
把 D1 写成第二个遍历会立刻变成「同一个决定两处推导」——
这个仓库的债本里已经有 #233/#240/#242/#344 四条同型。

⭐ **形态的解析单位是「映像」而不是「依赖边」。** issue §4.2 按边写 override,
但在 ELF 单一全局命名空间下,同一个库在同一个映像里出现两种形态**就是**这个诊断
要抓的缺陷。所以:边上的 `linkage` 是一个**请求**,映像级必须收敛成一个答案。

---

## 4. 形态轴的设计

### 4.1 三层,照 `mcpp.build.distribution` 的既有骨架

`distribution.cppm` 已经把「Role × Format → Contract → Mechanism」这套分层做对过一次
(总函数、表驱动、每个 effective ≠ requested 的格子都必须写诊断)。
形态轴是同一个骨架的第二次实例,新模块 `mcpp.build.linkage_form`:

| 层 | 名字 | 谁回答 | 来源 |
|---|---|---|---|
| 1 | **Admissible** 允许是什么(集合) | 引擎 | `admissible(pkg, format, libcLinkage)` —— **推导,从不询问**(4.2) |
| 2 | **Request** 想要什么 | 消费者 | `[build]` / `[profile.*]` / 依赖边(**两张表,不是三张** —— §12.4) |
| 3 | **`DepLinkage`** 最终是什么 | 引擎 | `resolve(admissible, request)`,**总函数**,**每映像一次** |

⚠️ 枚举**不能**叫 `Form`:`mcpp.build.loader_contract` 已经导出
`enum class Form { Executable, SharedLibrary, NotElf }`,而 `runtime_validation`
同时要读这两个(D2 在那里落地)。两个 `Form` 在同一处相遇,而它们**语义相近但不同** ——
一个是「这个产物是什么」,一个是「依赖以什么形态进来」。叫 `DepLinkage`,
与用户写的键 `dependency_linkage` 同名:一个概念,在 manifest、代码、诊断里是同一个词。
(仓库里已经有 `Form` / `Format` / `Shape` / `Role` / `Contract` / `Binding` 六个近义名,
随手加第七个正是「口袋名」债的起点。)

Mechanism(对象内联 / shared LinkUnit)**不是一层**,它是 emitter 的事,而且两条都已存在。

⚠️ **能力与约束合并成一个集合,是这轮 review 的第一处简化。**
初稿把它们分成两层,照抄了 `distribution.cppm` 的 Contract/Mechanism 分离 ——
但那里分开是因为诊断要区分「你要 X 我给了 Y」和「X 在这个平台没有机制」。
这里两种否定的诊断文本是同一句(「这个包在这个平台只能是 static」),
分成两层只是把一次交集拆成两次判断。

### 4.2 Admissible 的推导,零新 key

⚠️ **初稿的推导规则是错的**,而且索引里有现成的反例。

初稿写「mcpp 编译它的源码 ⇒ static + shared」。但 `compat.openssl` 同时有
`sources = { "mcpp_openssl_anchor.c" }` **和** `ldflags = { "-Llib", "-l:libssl.a", … }`
—— 它是**混合形态**:一个 anchor TU 由 mcpp 编译,真正的实现是包内随附的
`.a` 归档。把它做成 shared,等于把非 PIC 的 openssl 归档塞进 `.so`。

修正后的规则,判据是 `-L` 一个符号:

> **一个包可以是 shared,当且仅当它对链接的全部贡献都是 mcpp 编译出来的对象。**
> `-L` 恰好是「本包携带了 mcpp 没有编译的链接输入」的标记。
> 没有 `-L` 的 `-lm` / `-lpthread` / `-lws2_32` 是**宿主**系统库,`.so` 依赖它们是正当的。

实测(mcpp-index `8f5ad30`,130 个包):

| | 数量 | 是谁 |
|---|---|---|
| 有 `ldflags` | 31 | |
| 其中含 `-L` | **4** | `compat.openssl`、`compat.mysql-connector-cpp`、`compat.openblas`、`compat.vulkan` |
| 其余 27 个的 `-l` | —— | 全部是系统库:`-lm -lpthread -ldl -lrt -latomic -lws2_32 -lbcrypt -ladvapi32 -lgdi32 -liphlpapi -lresolv -lodbc` … |

四个里 `compat.vulkan` 本就是 `kind = "shared"`(约束),不走这条路。
所以这条规则在 130 个包上**恰好挡住三个真正携带预构建归档的包,零误伤**。

⚠️ 判据要落在**解析后**的 ldflags(cfg 合并之后),且要认三种拼写:
`-Llib`、`-L lib` 两 token、`-Wl,-L…`。

完整的推导表:

| 包形态 | Admissible | 判据来源 |
|---|---|---|
| mcpp 编译全部实现,ldflags 无 `-L` | `{static, shared}` | 有 `sources` ∧ 无 `-L` |
| ldflags 含 `-L` | `{static}` | 携带了 mcpp 没编译的链接输入 |
| 显式 `kind = "shared"` | `{shared}` | 约束(§1.2(b),12 个包) |
| **分发包**(`mcpp pack` 产出) | 随包的那些 role | `runtime.artifacts[].role ∈ {static-library, shared-library}` —— **已经在 manifest 里** |
| 目标格式无加载器(freestanding) | `{static}` | `ninja_backend.cppm:2383`:无 loader 就没有 shared 规则 |
| **整链 `-static`(libc 轴)** | `{static}` | ⚠️⚠️ **两根 `linkage` 轴不是独立的** —— 见下 |

⚠️⚠️ **全静态 libc 让 shared 形态在物理上不可能**,这是第二轮才发现的跨轴耦合。
`flags.cppm:815` 是 `full_static = (full_static_ok && linkage == "static") ? " -static" : ""`,
而 `prepare.cppm:2027` 让 **musl 工具链默认就是 `linkage = "static"`**。
一个 `-static` 的可执行文件没有加载器,装不下任何 `.so`。
⇒ 对 musl 用户,`dependency_linkage = "shared"` 的**默认路径**必须被拒绝并说明原因,
而不是产出一个链接不起来的图。

⭐ 这条耦合同时**消掉了一张表**:第一轮为「musl 要 static、gnu 要 shared」
加了 `[target.<triple>].dependency_linkage`,而真正的答案是引擎自己知道
musl 那条腿只能 static ——用户不需要写。见 §12.4。

⚠️ **Format 必须是 `capability()` 的入参**,不只是包 —— 这是 review 的第二处修正。
`distribution.cppm` 的 `default_contract(Role, Format)` 早就是这个签名,初稿漏了。
裸机三元组下 `dependency_linkage = "shared"` 必须被明确拒绝,而不是产出一个没有规则的边。

`pack/prebuilt.cppm:62` 的 `is_library_role` 已经在读这两个 role。
**打包侧的能力集不需要新增任何 key。**

### 4.3 「必须是 shared」/「必须是 static」的读法

按 §1.2(b) 的不对称读法。而「必须 static」**不需要新键**:实测索引里那三个包
(`compat.openssl` / `mysql-connector-cpp` / `openblas`)全都因为携带 `-L` 而由
§4.2 的规则自动落进 `{static}`。**约束是被测出来的,不是被声明的。**

### 4.4 键的完整清单 —— 一个新键名,三张已有的表

`linkage` 已被 libc 轴占用(`[target.<triple>].linkage`,`docs/05` §2.7.1 已发布)。
不动它。

```toml
[build]
dependency_linkage = "static"      # 全图默认(缺省即 static);或 "shared"

[profile.dev]
dependency_linkage = "shared"      # 按 profile 覆盖

[target.x86_64-linux-musl]
dependency_linkage = "static"      # 精确三元组覆盖,与 linkage/cxx_runtime/sysroot 并列

[dependencies]
"compat.zlib" = { version = "1.3.2", linkage = "shared" }   # 单包请求(仅根 manifest)
```

- 全局键叫 `dependency_linkage`:说清了**谁的** linkage,不会被误读成
  「我这个库怎么构建」(那是 `[targets.*].kind`)。长名与
  `macos_deployment_target` / `build_program_timeout` / `module_extensions` 同风格。
- 边上仍叫 `linkage`:在 `[dependencies]` 表里没有歧义,且是
  Zig / Conan / vcpkg 的共同词汇。

⭐ **两个入口存在的理由不同,这是它们都该在的原因**:
`[build]` / `[profile.*]` 服务**开发回路**(shared 依赖让重链接变快),
边上的 `linkage` 服务**冲突消解**(D2 报了,我要把这一个包换个形态)。
一个不是另一个的语法糖。

⭐ **两张表都是这类标量的既有归属,不是新机制:**

| 表 | 为什么是它 |
|---|---|
| `[build]` | 加进 `toml.cppm:1190` 的 `kKnownBuildKeys`。⚠️ 该处注释记着:消息里的列表**就是**同一个列表(曾经手抄第三份并漂移过),所以只需改一处 |
| `[profile.*]` | ⭐ profile 的四个旋钮已经在 cache key 轴 C 里(`cache_key.cppm` `BuildAxes`),形态跟着它走,**缓存键问题自动关闭** |
| ~~`[target.<triple>]`~~ | ⚠️ **第二轮撤回**。第一轮按「与 `linkage`/`cxxRuntime`/`sysroot` 对称」加的,但它服务的唯一场景(musl 静态 / gnu 动态)已被 §4.2 的跨轴耦合自动答掉。我一边用「未出现的需求不入 schema」否掉白名单键,一边为对称性加了它 —— 见 §12.4 |

⚠️ 无论哪张表,这个键都**不能**走 `cfg(...)` 通道:`types.cppm:159-165`
明文说标量需要 last-wins,而条件通道只做 append,
「conditioning a scalar is a different operation」。

#### 除此之外不需要任何键 —— 逐条对照 `docs/05` 附录 A

附录 A 的准则:**「A key that duplicates an answer another section already gives is not admitted.」**

| 候选键 | 判决 | 理由 |
|---|---|---|
| 包侧「我能是 shared」 | **不加** | §4.2 从 `sources` + `-L` 测得,零误伤(130 个包实测) |
| 包侧「我必须 static」 | **不加** | 同上,三个包自动落进 `{static}` |
| 包侧「我必须 shared」 | **不加** | `kind = "shared"` 已经说了(12 个包) |
| 分发包的能力集 | **不加** | `runtime.artifacts[].role` 已经说了。附录 A 点名库分发「added **zero** manifest keys」是范例 |
| D2 的严重度开关 | **不加** | 复用已有的 `--strict`(schema 警告已是这个约定) |
| 有意为之的符号插桩白名单(jemalloc 替换 `malloc`) | **暂不加** | 附录 A:未出现的需求不入 schema。默认是警告不是错误,用户可忽略;真出现了再按「①领域中立 ②1:1 desugar」审 |
| `pack --linkage` | **不是 manifest 键** | CLI 选项,批 F |
| `soname` | **不是新键** | 已存在,批 B1 只是放开它的校验(⚠️ 但有发布顺序约束,见 §11.3) |

### 4.5 默认值 = `static` ⇒ 迁移判据是「零 diff」

今天的行为等价于「约束即形态」,而 117/130 个包的约束是 static。
默认 `static` ⇒ **每一个既有工程的 `build.ninja` 逐字节不变**。
这是可机器核验的迁移判据(归一化 diff build.ninja,`.agents/docs` 里
`issue311` 那份用过同一方法)。

---

## 5. 构建期视角:必须一起改的三处

### 5.1 ⚠️ 这根轴把一个潜在缺陷变成可达的

`-fPIC` 今天由 `flags.cppm:444` **扫描 `plan.linkUnits`** 得出,
而 `cache_key.cppm` 的 A/B/C/D/E/F 六轴里**没有它**(E 取的是 manifest 的 cflags,
不是计算出来的 flags)。

今天这是良性的:一个包的形态固定,PIC 的差异只造成「多编了 PIC」或
「静态对象进了 exe」,都不出错。

**加上这根轴之后就出错**:工程 A(无 shared)缓存了非 PIC 的 `compat.zlib`,
工程 B 请求 `linkage = "shared"` 命中同一条目 ⇒ 非 PIC 对象进 `.so` ⇒
`relocation R_X86_64_32S … can not be used when making a shared object`。

**修法同时消掉一处重复推导**:PIC 由 `resolve_form()` 的结果一次决定,
落进 `BuildAxes`,`flags.cppm` 读它而不是再扫一遍 plan。
(记忆:「同一个决定两处推导」是隐性架构债。)

### 5.2 工程指纹

`dependency_linkage` 改变每一个对象与产物 ⇒ 必须进
`FingerprintInputs::compileFlags`(决定 `target/<triple>/<hash>/` 的那个),
否则切换开关会复用上一次的构建目录。与 5.1 是两个不同的哈希,都要覆盖。

### 5.3 ⚠️⚠️ 合成的 shared 形态没有 ABI 名 —— 轴单独落地会让 §2 变得不可检测

这是本文最重要的一条,它决定了交付顺序。

`compat.zlib` 没有 `soname`(它是 `kind="lib"`)。请求 shared 之后,mcpp 会按
**target 名**产出 `libzlib.so`。而 `libgio-2.0.so.0` 的 `DT_NEEDED` 写的是
`libz.so.1`。于是:

| 状态 | 进程里的 zlib | 有诊断吗 |
|---|---|---|
| 今天(混形态) | 一份(静态那份劫持了 gio) | **D2 能报** |
| 只加轴,请求 shared | **两份**(`libzlib.so` + vendor 的 `libz.so.1`) | **没有任何诊断**(两边都是 .so,exe 的 dynsym 干净) |
| 轴 + 正确 soname | 一份 | 干净 |

⭐ 所以顺序是被推导出来的,不是偏好:
**D2 → `soname` 对任意 library target 生效 → 形态轴。**

`soname` 今天不只是「没被读」——`types.cppm:1033` 的
`validate_target_soname` **主动拒绝**它出现在非 shared 目标上
(`soname is only valid for shared targets`)。批 B1 因此是三小处:
放开这个校验、`plan.cppm:427` 的别名生成、`ninja_backend.cppm:266`
的 `shared_soname_flag`(它已经在 `lu.soname` 为空时正确地不发标志)。

而 `soname` 是索引描述符里**唯一**必须新增的字段,且只有真正要参与
第三方 `DT_NEEDED` 的包才需要写。

⚠️ 「正确的 soname 就一定赢」这一步**不能推理,要测**:vendor 包的
`runtime_search_dirs` 经 `plan.cppm:669-676` 也变成 rpath 条目,
`$ORIGIN` 与它谁在前由 `link_line::UnitTail` 的槽位顺序决定。
这正是 **#304** 的形状(路径遮蔽),两个 issue 在这里合流。

### 5.4 静态形态仍然是「对象内联」,不是归档

`plan.cppm:1491-1498` 记录了一次已被回退的尝试:把 `kind="lib"` 依赖做成 `.a`,
在 Windows/MSVC lld-link 上不可行(不会为入口点拉归档成员 → LNK1561;
归档改变了传递符号解析顺序 → libarchive→lzma LNK2019)。

⚠️ 于是 issue §2 的「放大器」一节里那条建议(改成归档懒选可缩小可劫持面)
是**被历史否决过的**。可劫持面确实会变小,但那不是这根轴该付的代价。
D2 的谓词与内联/归档无关,不受影响。

---

## 6. 打包分发视角

### 6.1 ⚠️ `-L … -l<name>` 是形态盲的 ⇒ 双形态包必须分目录

`pack/manifest_emit.cppm:241` 发的是:

```toml
ldflags = ["-Llib/<triple>", "-l<name>"]
```

同一个目录里同时存在 `lib<n>.a` 与 `lib<n>.so` 时,**ld 优先取 `.so`** ——
形态就变成了 ld 搜索规则的副产物,而不是一个决定。所以双形态包必须:

- 分目录(`lib/<triple>/static/` 与 `lib/<triple>/shared/`),或
- 用 `-l:lib<n>.a` 的显式拼写。

前者更干净,且与既有的 per-triple 目录结构同型。

### 6.2 老客户端只读 ldflags ⇒ 双形态包的「默认形态」是一个发布决定

`manifest_emit.cppm:252-255` 的注释(作者实测):
「An older mcpp reads only the ldflags above and silently ignores this block」。

⇒ 一个双形态包,`ldflags` 那一行指向哪个形态,就是**所有老客户端拿到的形态**。
建议固定为 static(与今天的默认一致),新客户端通过中立通道
(`[target.'…'.runtime] link_library_dirs/libraries`)选另一个。

### 6.3 `mcpp pack` 今天是整包单形态

`LibraryPackPlan::targetShared` 是**整个包一个 bool**,legs 是按 **triple** 分的
(胖包),不是按形态分的。要支持双形态,leg 的维度要从 `triple` 变成
`(triple, form)`。这是打包侧唯一的结构性改动。

`LibraryLeg::shared` 已经是 per-leg 的,所以改动集中在
「怎么产生 legs」而不是「怎么描述 legs」。

### 6.4 请求一个没随包的形态 ⇒ 复用 `check_prebuilt` 的拒绝路径

`pack/prebuilt.cppm` 的 ABI-tag 拒绝已经建立了「说清楚包里有什么、你要的是什么」
的诊断形状。形态不匹配是同一类,复用它,不要新写一条。

### 6.5 索引描述符:零必需新键

- Admissible 从 `sources` + `-L` + `runtime.artifacts[].role` 推(全部已有);
- 「必须 shared」从 `kind = "shared"` 推(已有,12 个包);
- 「必须 static」从 `-L` 推(已有,实测三个包,零误伤);
- 唯一会写进描述符的 `soname` 是**可选**的,只有要参与第三方 `DT_NEEDED` 的包才写。

⚠️⚠️ 但 `soname` 不是「老客户端忽略它」——它会让老 mcpp **加载 manifest 直接失败**。
见 §11.3,那是本设计里唯一一条有发布顺序约束的改动。

---

## 7. 分批

| 批 | 内容 | 独立价值 | 依赖 |
|---|---|---|---|
| **A** | **D2**:两段式谓词(§2.1,⚠️ 已被 §12.1/§12.2 修正两次)+ 归因 + 落进 `runtime_validation`(⚠️ 独立报告项,**不复用** `RuntimeVerdict::Status`,见 §11.4) | 把一个 latent ODR 问题变成可见的构建诊断。**issue 自己也建议先做这条** | 无 |
| **B1** | 引擎放开 `soname` 的非 shared 校验(`types.cppm:1033`) | 让「一个 SONAME 一个提供者」可表达 | 无 |
| **B2** | 索引描述符**写入** `soname` | 让 §5.3 的第三行成立 | ⚠️⚠️ B1 **已发布**且索引 `latest` 的 mcpp 下限跨过它(§11.3) |
| **C** | **D1 = `resolve_form()` 的错误路径** + `mcpp.build.linkage_form` 三层模型(Admissible 推导 + 默认 static) | 零行为改变(默认 static ⇒ build.ninja 零 diff),但把决定收敛到一处 | 无 |
| **D** | PIC 进 `BuildAxes` + 工程指纹;`flags.cppm` 改为读它 | 消掉一处重复推导 | C |
| **E** | `dependency_linkage`(`[build]` + `[profile.*]`)+ 边上 `linkage`(**仅根 manifest**,§11.3) | 轴可用 | C、D |
| **F** | `mcpp pack` 的 `(triple, form)` legs + 分目录 | 分发侧可用 | E |
| **G** | 依赖包的 `required_features` 门控(§1.2(a) 的独立缺陷) | 独立 | 无 |
| **H** | 文档:`docs/05` §2.2 「shared 仅 Linux/ELF」已过时(§11.5);`linkage` 两根轴的辨析框 | 独立 | 无 |

A、B1、G、H 四批互不依赖,可并行开 PR。B2 是**发布顺序**问题而不是实现问题。

---

## 8. 判据(测试)

⚠️ 按记忆里那两条:判据要带分母;判据的「否」不能与「没测成」同读数。

| # | 判据 | 形式 |
|---|---|---|
| 1 | 干净项目 `\|HIJACK\| = 0`,**并打印分母**(`0 of 217 dynamic symbols`) | e2e,`# requires: elf` |
| 2 | 复现 §2 的场景 ⇒ D2 **命中,且诊断里同时出现两个包名和两种形态** | e2e。⚠️ 判据是整行输出,不是子串 |
| 3 | 用户自己写了 `-rdynamic` ⇒ D2 **静默**(前置条件不成立) | 单测足够 |
| 4 | 默认 `static` 下,一组既有工程的归一化 `build.ninja` **逐字节不变** | 单测 + e2e |
| 5 | 同一映像两条边请求同一个包的不同形态 ⇒ **报错并指名两条边** | 单测(纯 `resolve_form` 表) |
| 6 | 请求 shared 的包,其对象的 `-fPIC` 必须来自 `BuildAxes` ⇒ **改 `dependency_linkage` 必须改缓存键** | 单测比对两个 key_hex |
| 7 | 分发包缺少被请求的形态 ⇒ 拒绝,**并列出包里实际有的 role** | e2e |
| 8 | 依赖包的 `required_features` 未满足 ⇒ 该 target **不出现在 plan 里** | 单测 |
| 9 | 一个 `ldflags` 含 `-L` 的包被请求 shared ⇒ **拒绝,并复述那条 `-L`** | 单测(`compat.openssl` 形状的 fixture) |
| 10 | 裸机三元组下请求 shared ⇒ **拒绝**,不是产出一条没有规则的边 | 单测(`resolve_form` 的 Format 入参) |
| 11 | 非根 manifest 的依赖边写了 `linkage` ⇒ **忽略并警告**,不改变形态 | 单测 |
| 12 | `dependency_linkage = "shared"` 的工程 `mcpp pack` ⇒ 产出的 bundle 里**有那些 `.so`,且 exe 能在解包后启动** | e2e |

⚠️ 判据 1、2、7、12 要落在 e2e,而 shard 上的 `# requires:` 会静默跳过
(记忆:`e2e-requires-llvm-never-runs-on-shards`)。守卫要写在 job 里。

---

## 9. 明确不做

- **不改 `[target.<triple>].linkage`**(libc 轴)。名字撞车用前缀解决,不动已发布语义。
- **不做全闭包符号求交**。§2.1 的谓词只需要 exe 一侧,代价是一次 `.dynsym` 遍历。
- **不把静态形态改成归档**。历史已否决(§5.4)。
- **不做同一映像内同一个库的两种形态并存**。那是被诊断的缺陷本身,不是特性。
- **PE / Mach-O 不实现 D2**。它们结构上没有这个问题。D1 仍然全平台生效。
- **不动索引的 84 个 `kind = "lib"`**。
- **不加符号插桩白名单键**。默认警告即可忽略,按附录 A 等真实需求出现(§4.4)。
- **不认非根 manifest 的边上 `linkage`**(§11.3)。
- **批 F(一个包同时发两种形态)暂缓**,直到有实证需求(§11.2)。

---

## 10. 与既有 issue 的关系

- **#304**(`runtime.library_dirs` 落在链接行造成路径遮蔽):§5.3 的最后一步与它合流 ——
  「正确的 soname 是否真的赢」取决于 rpath 槽位顺序。两个 issue 应该一起测。
- **#493**(`[system_deps]` 经 pkg-config 引入宿主库):宿主库的形态完全不由 mcpp 决定,
  D1 对它一无所知。**D2 是它唯一的判据**,而且不需要为它写任何新代码 ——
  这是「测量而非声明」这个选择最大的一笔回报。

---

## 11. 自我 review:五个视角

⚠️ 本节记录的是**这轮 review 推翻或补上的东西**,不是复述前文。
已经写回正文的四条(§4.1 三层、§4.2 的 `-L` 规则、§4.2 的 Format 入参、§3 的 D1 归位)
在这里只列结论。

### 11.1 架构

| # | 发现 | 处置 |
|---|---|---|
| 1 | ⭐ **D1 不该是第二个遍历**,它是 `resolve_form()` 没有答案时的错误路径 | 已写回 §3 |
| 2 | ⭐ **五层过度分解**:Capability 与 Constraint 的两种否定诊断文本相同,合成一个 `Admissible` 集合 | 已写回 §4.1 |
| 3 | ⚠️ **Format 必须是 `capability()` 的入参**,`distribution.cppm` 的 `default_contract(Role, Format)` 早就是这个签名 | 已写回 §4.2 |
| 4 | ⚠️ **Form 必须挂在 `PackageRoot` 上,不能让 `make_plan` 再推一次**。今天 `plan.cppm:1474` 直接读 `t.kind` 来决定要不要建 shared 边;如果解析在 `prepare_build`、消费在 `make_plan` 而中间不传值,就又是一次「同一决定两处推导」 | 批 C 的实现约束 |
| 5 | ⚠️ **shared 依赖会改变 `mcpp pack` 的闭包**。`pipeline.cppm:171` 已经把第三方 `.so` 的搜索目录喂给 pack,但那两个通道(`plan.runtimeLibraryDirs` / `linkIntent.runtimeSearchDirs`)装的是**依赖包声明的目录**,而合成的 `bin/libzlib.so` 是**本次构建的 LinkUnit 输出**,不在其中。⚠️ 它大概率经 `$ORIGIN` 的 DT_NEEDED 闭包被捞到 —— 但这是推理不是测量,判据 12 就是为它写的 | **批 E** 前必须实测(shared 依赖在 E 落地时就存在,不等 F) |

### 11.2 简洁

- **净新增概念:两个**(`Admissible` / `Form`)。D1、D2、PIC 轴、soname 全都挂在已有结构上:
  `runtime_validation` 的快照机制、`BuildAxes` 的轴 C、`TargetEntry` 的标量位、
  `check_prebuilt` 的拒绝路径、`kKnownBuildKeys` 的一处列表。
- **净新增 manifest 键:一个名字**(`dependency_linkage`),外加依赖边上的 `linkage`。
  索引描述符侧**零必需新键**。
- ⭐ 最省的一步仍然是 §1.2(b) 的不对称读法:它把「要改 84 个已发布描述符」
  变成「一行代码都不用改」。
- 反向自查:有没有哪一块是可以删掉而设计仍然成立的?**有一块** ——
  §6.3 的 `(triple, form)` legs。它只服务「一个包同时发两种形态」,
  而这个需求今天没有实证。⚠️ 批 F 应该等到有人真的要它,
  按附录 A 的「未出现的需求不入 schema」同理处置。

### 11.3 兼容 —— 这轮最大的一处发现

⚠️⚠️ **`soname` 写进索引描述符会让所有老 mcpp 硬失败,不是被忽略。**

`types.cppm:1030` 的 `validate_target_soname` 在 `kind != SharedLibrary` 时返回错误,
而两个解析器都把它变成**加载失败**:

```
src/manifest/toml.cppm:580     return std::unexpected(error(origin, *msg));
src/manifest/xpkg.cppm:1483    return std::unexpected(ManifestError{*msg, …});
```

这与记忆里那条实测同型(`provides` 硬失败 / `requires` 静默忽略)。
⇒ **批 B 必须拆成 B1(引擎)/ B2(描述符),B2 的前置判据是索引 `latest` 的
mcpp 下限跨过 B1 的发布**,而不是「B1 合入了」。这是一条发布顺序,不是实现顺序。

其余兼容项:

| 项 | 结论 | 判据 |
|---|---|---|
| `[build] dependency_linkage` 出现在依赖的 mcpp.toml,被老 mcpp 读到 | ✅ 安全 | `toml.cppm:1219` 走 `schemaWarnings`,不是错误 |
| 边上的 `linkage` 出现在**非根** manifest | ⚠️ **必须只认根 manifest** | 一个中层包强推全图形态是供应链属性;`reexport` 的注释已经为同一形状写下过判决(「visibility 默认 public,riding it 会让任意深度的依赖悄悄改变消费者」)。而真正需要 shared 的中层包,应该在**自己的 target 上写约束**,不是在边上写请求 —— 请求通道只需要根级 |
| 默认值 | ✅ `static` ⇒ 既有工程 `build.ninja` 零 diff(判据 4) |
| 索引 84 个 `kind = "lib"` | ✅ 不动 |

### 11.4 易用性

| # | 发现 | 处置 |
|---|---|---|
| 1 | ⚠️ **D2 不能复用 `RuntimeVerdict::Status`**。`ProvenMismatch` 字面上就是「两个运行期载荷被混在一起」,但它 `blocking()`;而 `/usr/bin/git`、`/usr/bin/ls` 形状的映像在真实世界大量存在(实测),第一天就 block 会把绿的构建变红。而且那个四值枚举的注释本身就在警告「把 Unresolvable 折进 Inconclusive」这类混淆 | **独立报告项**;默认警告,`--strict` 升级为错误 |
| 2 | ⚠️ **两个 `linkage` 的辨析必须进文档**。同一份 manifest 里 `[target.<t>].linkage`(libc)与 `[build] dependency_linkage`(依赖)并存,是可预见的踩坑点 | 批 H:`docs/05` §2.7.1 加辨析框;`mcpp doctor` 一并打印两根轴的解析值 |
| 3 | 诊断的可操作性:issue §4.3 的文本给了两条出路。**要补第三条**(声明 soname 让两者统一),否则 vendor 包场景下用户无路可走 | 批 A 的文案 |
| 4 | 有意插桩(jemalloc 替换 `malloc`)会误报 | 默认是警告 ⇒ 可忽略。不加白名单键(§4.4) |

### 11.5 跨平台

| # | 发现 | 处置 |
|---|---|---|
| 1 | ⚠️ **`docs/05` §2.2(:100-104)已经过时**:它写「shared 仅支持 Linux/ELF,macOS/Windows 会在 plan 前被拒」。而 `tests/e2e/257_shared_library_pe.sh` 的头注明确写着「Until now this was Linux-only… this test is a NEW CAPABILITY」,`259_shared_library_macho.sh` 同理 | 批 H,独立的文档缺陷 |
| 2 | ⚠️ **MinGW 的 `-Wl,-Bdynamic` 位置敏感**:`manifest_emit.cppm` 记着实测 —— mcpp 给 PE 可执行文件 `-static`,ld 进入 static-only 模式会拒绝导入库,标志必须**紧挨**它启用的那个 `-l`。合成的 shared 依赖在 MinGW 上会撞上同一条 | 批 E,复用 `link_line` 的槽位而不是再拼一次 |
| 3 | **MSVC ABI**:DLL 不 `dllexport` 就什么都不导出,mcpp 已有 `coff_exports` 自动生成 `.def`(e2e 258)。把一个从没打算做 DLL 的 C 库合成成 shared,会导出全部符号 —— 能用,但体积和链接时间要在文档里说清 | 批 E 的文档 |
| 4 | **freestanding**:`ninja_backend.cppm:2383` —— 没有 loader 就没有 shared 规则。见 §4.2 的 Format 入参与判据 10 | 已写回 |
| 5 | **D2 仅 ELF**,与 `runtime_validation` 的既有作用域一致;PE/Mach-O 结构上没有这个缺陷 | 已在 §2.5 |

---

## 12. 第二轮自我 review:六个视角

⚠️ 本轮的规则:**不复述 §11**。只记这一轮**推翻或补上**的东西。
本轮推翻了三条我自己在第一轮写下的判断,其中一条会让整条诊断在合法安排上误报。

### 12.1 ⚠️⚠️ D2 的谓词错了两次(架构 / 正确性)

**(1) 「无版本 ⇒ 劫持」是一条依赖 libc 的启发式,不是判据。**

第一轮用「已定义 dynsym 是否带 `@GLIBC_x`」区分劫持与 copy relocation。
但**符号版本是 glibc 的特性,musl 完全没有**。mcpp 的 musl 目标是一等公民 ——
只是它默认 `-static`(§4.2)所以恰好没有 `.dynsym`,判据才没当场露馅。
**「恰好不出错」不是正确。**

替换成一条不依赖 libc 也不依赖架构的:

| 符号类型 | 判定 |
|---|---|
| `STT_FUNC` 已定义 | **一定**是导出 —— copy relocation 永远不会是函数 |
| `STT_OBJECT` 已定义 | 查 `.rela.dyn` 里有没有 `R_<arch>_COPY` 的 `r_offset == st_value` |

⚠️ **必须按地址查,不能按名字。** 实测:`environ` 是 `__environ` 的 WEAK 别名,
两者同址,但重定位表里只有 `__environ` 那一条。按名字查会在**每一个** mcpp 产物上
误报 `environ`。

四个产物的回归读数在 §2.2,新规则全部判对(mcpp 0 / git 1 / ls 7 / bash 排除)。

**(2) ⚠️⚠️ 只看 exe 一侧会在 mcpp 自己的合法安排上误报。** 见 §12.2。

### 12.2 ⚠️⚠️ mcpp 的 `kind = "shared"` 机制结构性地产出同一个形状(架构)

读码:一个 shared 依赖的链接单元只拿**它自己**的对象
(`plan.cppm:1618` `append_package_objects(lu, dep.packageName)`),
它的 shared 依赖走 `append_direct_shared_deps` —— 而它的 **static 依赖什么都不走**。
那些对象进了**消费者的 exe**(它们不在 `sharedDepPackages` 里)。
`ld -shared` 默认允许未定义符号,于是 `.so` 链接通过,运行期绑到 exe。

**这正是 §2 的形状,而且是 mcpp 自己排的。**

⭐ 但它是**良性的**,而且是 load-bearing:进程里只有**一份**定义。
换成「把 static 依赖也塞进 `.so`」反而会变成两份、两套状态。

⇒ 真正的判据不是「exe 导出了东西」,而是「exe 导出的东西**还有第二个提供者**」。
§2.1 因此改成两段式。⭐⭐ 而这一改让 D2 与 §0 的不变量**逐字对齐**:
一个库,一个提供者。第一轮的谓词只是它的一个必要条件。

⚠️ 今天的索引躲过这一劫是**人口的巧合**,不是设计的性质:
12 个 shared 包唯一的 static 依赖是 `compat.xorgproto` / `compat.xtrans`,
而这两个的 `sources` 是 `mcpp_generated/xorgproto_empty.c` —— **空 TU**。
换任何一个真有代码的 static 依赖进来,形状立刻实体化。

⭐ 附带的好处:**D2 的 e2e fixture 不需要手工组装 glib 包**(issue §7 那个)。
三个 mcpp 包就能造出来:一个 `kind="shared"` 的 A、一个真有符号的 static B、
一个也定义了 B 那些符号的 `kind="shared"` C。

### 12.3 ⚠️⚠️ 两根 `linkage` 轴不是独立的(跨平台 / 架构)

第一轮把「名字撞车」当成纯粹的命名问题。它不是。

`flags.cppm:815` 全静态 libc 发 `-static`,而 `prepare.cppm:2027`
**让 musl 工具链默认就是 `linkage = "static"`**。一个 `-static` 可执行文件
没有加载器,**装不下任何 `.so`** ⇒ 在 mcpp 最常见的 musl 路径上,
`dependency_linkage = "shared"` 物理不可能。

⇒ `admissible()` 的入参从 `(pkg, format)` 变成 `(pkg, format, libcLinkage)`。
已写回 §4.1 / §4.2 与判据 13。

### 12.4 ⚠️ 我对自己的准则不一致(简洁)

同一节里我用「未出现的需求不入 schema」否掉了插桩白名单键,
**又为对称性加了 `[target.<triple>].dependency_linkage`** —— 它没有实证需求,
只有「和 `linkage`/`cxxRuntime`/`sysroot` 并排好看」这个理由。
而它想服务的场景(musl 静态 / gnu 动态)被 §12.3 的耦合自动答掉了。

**撤回。用户可写的入口从三个降到两个 + 一个边上的键。**

### 12.5 ⚠️ 命名:`Form` 已经被占用(优雅 / 架构)

`mcpp.build.loader_contract` 导出 `enum class Form { Executable, SharedLibrary, NotElf }`,
而 `runtime_validation.cppm:622` 正在用它 —— 那也正是 D2 要落地的地方。
两个 `Form` 会在同一个翻译单元里相遇,而它们**语义相近但不同**。

改叫 **`DepLinkage`**,与用户写的 `dependency_linkage` 同名:
**一个概念,在 manifest、代码、诊断里是同一个词。**

⭐ 仓库里已经有 `Form` / `Format` / `Shape` / `Role` / `Contract` / `Binding`
六个近义名。随手加第七个正是「口袋名」债的起点 —— 这次不是模块名,是类型名,
但同一条规则适用:**按职责命名,不按形状命名。**

### 12.6 ⚠️ `PT_INTERP`,不是 ELF 类型(跨平台)

实测:mcpp 的产物是 `ET_EXEC`,而 `/usr/bin/git`、`/usr/bin/ls` 是 `ET_DYN`(PIE)。
全仓 grep **没有任何 `-pie` / `-no-pie` / `-fPIE`** ⇒ 这个属性完全来自载荷工具链的默认值,
会在我们脚下变,而且变的方向是「判据静默失效」——
`ET_EXEC` 的检查在 PIE 世界里读数永远是「没有可执行文件要查」。

判据是 **`PT_INTERP` 存在**,而 `ElfRuntimeFacts::interp` **已经在记它**。
(记忆:「判据写了、绿了、却从没跑到」——这是它的第 n 次。)

### 12.7 易用性:诊断必须区分两种「导出」

有了两段式谓词,消息也分两层,而且**第一层默认不打印**:

| 情形 | 说什么 |
|---|---|
| `EXPORTED` 非空、`CONFLICT` 空 | **什么都不说**。那是 §12.2 的合法安排 |
| `CONFLICT` 非空 | 报告:符号(截断 + 总数)、静态提供者(包)、共享提供者(文件 + 包)、**三条出路**(设 `dependency_linkage`、让其中一方不再提供、声明 soname 让两者统一) |

⭐ 「第一层默认不打印」是这轮易用性上最重要的一条:
第一轮的设计会在每一个用了 `compat.x11` 且有真实静态依赖的项目上刷警告,
而用户对此**无事可做** —— 那正好是 `distribution.cppm` 的
`explicitRequest` 注释警告过的东西:诊断是为**被打破的承诺**准备的。

### 12.8 优雅:这个设计的「一句话」现在能自我证明

> 不变量:**一个库,一个提供者,一种形态。**
>
> - **D1** = 在 plan 上求值:`|DepLinkage(pkg, image)| = 1`
> - **D2** = 在产物上求值:`EXPORTED(exe) ∩ ⋃ defines(closure) = ∅`
>
> **同一句话,两个论域。**

第一轮的 D2 不满足这个形式(它只看一侧),所以两者只是「都有用」;
修正后它们是**同一条不变量的两次求值**。这才是这套设计该有的样子。

另一条可检查的优雅性判据 —— **输入里有几个是新的**:

| `resolve()` 的输入 | 来源 |
|---|---|
| `sources` | 已有 |
| `ldflags` 里的 `-L` | 已有 |
| `targets.*.kind` | 已有 |
| `runtime.artifacts[].role` | 已有 |
| 目标格式 | 已有 |
| libc `linkage` | 已有 |
| **请求** | **1 个新键** |

**六个已有 + 一个新的。** 这根轴不是往 manifest 里加信息,
它是**问一个从来没人被问过的问题**。

### 12.9 本轮新增的判据

| # | 判据 | 形式 |
|---|---|---|
| 13 | musl / 整链 `-static` 下请求 shared ⇒ **拒绝并说明是 libc 轴导致的**,不是产出一个链不起来的图 | 单测 |
| 14 | `environ` **不得**出现在任何 `EXPORTED` 里(WEAK 别名同址) | 单测,读真实产物 |
| 15 | PIE 产物(`ET_DYN` + `PT_INTERP`)与 `ET_EXEC` 产物 **走同一条路径** | 单测两个 fixture |
| 16 | `kind="shared"` A → static B(**B 有真实符号**),`CONFLICT` 空 ⇒ **静默** | e2e,§12.2 的合法安排 |
| 17 | 同上再加一个也定义 B 符号的 C ⇒ **报告,并同时指名 B 与 C** | e2e |
| 18 | 静态链接产物(无 `.dynsym`)⇒ 诊断**跳过而不是报 0**,两者读数必须可分 | 单测(记忆:「判据的否与没测成同读数」) |

### 12.10 仍然没有答案的两个

1. ⚠️ **`mcpp pack` 能不能捞到合成的 `bin/lib*.so`**(§11.1 第 5 条)——
   仍是推理。判据 12 覆盖它,但批 E 之前无法实测。
2. ⚠️ **「正确的 soname 就一定赢」**(§5.3 末)—— 取决于 `$ORIGIN` 与依赖包
   `runtime_search_dirs` 在 `link_line::UnitTail` 里的相对次序,与 **#304** 合流。
   两条都**不要写进设计当结论**,它们是待测项。

---

## 13. 真实验证:用生态自己的 glib 复现 issue §2

⚠️ 本节是**实测**,不是设计。用的是 `mcpplibs/mcpp-index#245`(eui-neo 的 Linux SNI
托盘)的需求场景 —— 但**不依赖宿主**:暂存的 glib 及其整条闭包全部取自 xim
(`xim:glib@2.80.0` / `zlib` / `pcre2` / `libselinux` / `util-linux` / `libffi`),
引擎里没有任何 glib / zlib / 托盘的知识。

### 13.1 场景

`libgio-2.0.so.0` 的 `DT_NEEDED` 里有 `libz.so.1`(实测,不是构造出来的),
而索引里 `compat.zlib` 是 `kind = "lib"`。一个同时依赖两者的工程 = issue §2。

### 13.2 读数

```console
$ mcpp build
warning: trayapp: 88 symbols in this image are also provided by a library it loads.
    adler32()  adler32_combine()  ...  and 82 more
  Also provided by:
    …/xpkgs/xim-x-zlib/1.3.1/lib/libz.so.1.3.1
```

| | issue §2 报的 | 本次实测 |
|---|---|---|
| exe 里 DEFINED 的 zlib 符号 | 86 | **88** |
| `LD_DEBUG=bindings` 中 libgio 绑到 exe | 12 次 | 同形状,**含 `inflateGetHeader' [ZLIB_1.2.2]`** —— 带版本的引用绑到无版本的定义 |
| 有没有任何工具说话 | **没有** | **mcpp 在构建期指名了两个提供者** |

⭐ issue 里那条「同一情形在链接期(跨 `.so` 时)是硬错误,在这里静默通过」
现在有了构建期的声音。

### 13.3 ⚠️⚠️ 出路的次序被这次实测改掉了

按 `linkage = "shared"` 走一遍:

| | 之前 | 之后 |
|---|---|---|
| exe 导出的 zlib 符号 | 88 | **0** |
| 诊断 | 报 | 静默 |
| **进程里加载的 zlib** | **1 份** | ⚠️ **2 份**(`libzlib.so` + `libz.so.1`) |

`readelf -d bin/libzlib.so | grep SONAME` → **没有**。这正是 §5.3 推导出的结果,
现在是实测:**这根轴单独用,会把一个可检测的缺陷换成一个不可检测的缺陷。**

⇒ 诊断文案里三条出路的**次序**因此改了:第一条必须是永远正确的那条
(让一方不再提供),形态切换排最后并写清它的前提。单测断言的是**次序**,
不是三个子串都在。

### 13.4 这次验证证明了什么

1. 引擎在**真实 ELF 闭包**上工作(glib/gio + zlib/pcre2/selinux/mount/ffi),
   不只是在人造 fixture 上;
2. 两段式谓词在真实图上**没有误报**(vendor 包自身的 `.so` 之间不报);
3. `dependency_linkage` 的逃生口在真实图上**确实生效**(88 → 0);
4. ⚠️ 而它的**局限也是真的**,并因此改了文案 —— 这是本次验证最有价值的产出。

### 13.5 ⚠️⚠️ 第三处:打出来的包起不来

§11.1 第 5 条写的是「大概率经 `$ORIGIN` 的闭包被捞到 —— 但这是推理不是测量」。
判据 12 就是为它写的,而**推理错了**:

```console
$ mcpp pack && tar xzf … && ./app
error while loading shared libraries: libcore.so
```

`pack` 的闭包来自**运行产物**(`LD_TRACE_LOADED_OBJECTS`),而它运行的是
**暂存目录里的副本** —— 那个副本旁边的 `bin/` 是空的,`$ORIGIN` 解析不到,
库于是从不出现在闭包里,也就从不被打包。构建、打包、上传全程无话。

⚠️ **不是这根轴引入的**:在 mcpp 2026.8.26.1 上用作者声明的 `kind = "shared"`
依赖复现,同样起不来。但轴把它从「12 个自称 shared 的包」变成「任何一个包」
都可达 —— 与 PIC 进缓存键完全同型,所以同一个 PR 修掉。

### 13.6 三条被实测推翻的推理,一次会话

| # | 我写的 | 实测 |
|---|---|---|
| §11.1-5 | 「大概率被 `$ORIGIN` 闭包捞到」 | ⚠️ 没有。包解开就起不来 |
| §12.7 文案 | 三条出路,`dependency_linkage` 排第一 | ⚠️ 它会把 1 份变 2 份;次序要倒过来 |
| §12.2 | (推导)shared→static 是良性的 | ✅ 实测确认,且**第一轮的谓词会误报它** |

⭐ 前两条都是**推理写在文档里、判据写在测试里、然后测试推翻了推理**。
第三条是推理被证实 —— 但只有把它写成判据才发现第一轮的谓词错了。
