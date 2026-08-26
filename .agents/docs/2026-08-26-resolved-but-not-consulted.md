# 已经解析出的答案,没有被用来做决定

2026-08-26 · 两条用户报告的缺陷 + 两条连带发现 + 优化方案(待 review,尚未实施)

前置:[`2026-08-26-cross-target-implies-graph.md`](2026-08-26-cross-target-implies-graph.md) ·
[`2026-08-25-the-two-layer-predicate-family.md`](2026-08-25-the-two-layer-predicate-family.md)

---

## 0. 一句话

> **两处拒绝,都发生在 mcpp 已经知道正确答案之后。**

一处知道图要求 `llvm` 而手上正装着 `llvm@22.1.8`,却让用户去改全局默认;
一处知道用户没有写 C 库这一段,却拿自己填进去的那一段去查表并宣布不支持。

⭐ 这不是 2026.8.25.x 那个「谓词回答了更窄的问题」的家族。**这次谓词问对了,
答案也算对了,只是那个答案没有参与决定。** 前者是判据错,后者是判据没接线。

---

## 1. 判据

四条,全部在 `mcpp 2026.8.26.1` 实测,机器接口取值。

### 1.1 图声明了编译器,而 mcpp 让用户自己去设

```
$ cat mcpp.toml
  [dependencies]
  openkal-llvm-runtime = "0.1.3"

$ mcpp build                                   # 全局默认 gcc@16.1.0
  build.mcpp compiling
  build.mcpp running
  error: `openkal-llvm-runtime@0.1.3` requires the compiler to be `llvm`.
           compiler          gcc            (16.1.0, payload)
           required          llvm           (required by openkal-llvm-runtime@0.1.3)
         Select that compiler — yours outranks mcpp's own default:
             mcpp toolchain default llvm
```

同一台机器,同一个工程,只把编译器换成图要的那个:

```
$ MCPP_TOOLCHAIN=llvm@22.1.8 mcpp build
      Cached openkal-llvm-runtime v0.1.3 (244 units)
    Finished dev [unoptimized + debuginfo] in 1.02s
```

⚠️ **`llvm@22.1.8` 本来就装着。** 这次拒绝没有换来任何信息:它要求的东西已经
在本机,版本也已经确定,mcpp 只是没有去拿。付出的代价是让用户改一处**全局**状
态(`mcpp toolchain default llvm`)去满足**一个**工程的一条依赖。

### 1.2 短拼写在一个架构上通,在另一个架构上不通

```
$ mcpp build --target aarch64-linux
  error: target 'aarch64-linux-gnu' is registered but not yet supported (planned)

$ mcpp build --target aarch64-linux-musl
    Finished dev [unoptimized + debuginfo] in 0.99s
```

而同一个短拼写在 x86_64 上是**文档教的写法** ——
`examples/06-openkal-cross/src/main.cpp:4`:

```
//     mcpp build --target x86_64-linux          Linux,   any host
//     mcpp build --target aarch64-macos         macOS,   any host
//     mcpp build --target x86_64-windows        Windows, any host
```

这个示例的主题是「一份源码,三台机器」。缺的第四行正是 `aarch64-linux`,而它
是四个里唯一写不出来的。

机器接口:

| 请求 | `data.status` | `data.reason` |
|---|---|---|
| `x86_64-linux` | `ok` | `none` |
| `x86_64-windows` | `ok` | `none` |
| `aarch64-macos` | `ok` | `none` |
| `aarch64-linux` | `refused` | `tier-planned` |

### 1.3 `unknown target` 说的是假话

```
$ mcpp why toolchain --target riscv64-linux --format json | jq -r '.diagnostics[].message'
  unknown target 'riscv64-linux'
         known targets: `mcpp toolchain list`; a custom triple needs an
         explicit [target.riscv64-linux] section in mcpp.toml
```

`riscv64-linux-musl` **就在** `kKnownTargets` 里(tier `planned`)。这个 arch+os
组合是登记过的,消息说它未知。

而且这条拒绝没有记号:

```
"reason": "other"
```

`refusal::Code` 有十二个具名值,`unknown target` 这条路径一个都没记,于是
`--format json` 把一个有名字的拒绝报成 `other`。

### 1.4 `why toolchain` 的两个字段互相矛盾

同一份文档,同一次调用:

```json
"cLibrary": { "mode": "payload-first", "origin": "payload",
              "path": ".../xim-x-glibc/2.44/lib64" },
"layers":   [ { "layer": "c-abi", "interface": "musl",
                "impl": "openkal-musl@0.3.5", "origin": "graph" } ]
```

产物是哪一个,由产物回答:

```
$ file target/x86_64-linux-gnu/*/bin/test4
  ELF 64-bit LSB executable, x86-64, statically linked
$ readelf -l … | grep -i interpreter     →  (空)
$ readelf -d … | grep -i needed          →  (空)
$ nm -C … | grep -c openkal              →  11
```

静态、无解释器、无 `NEEDED`。glibc 不在产物里。`cLibrary` 描述的是 payload 的
链接搜索模型,`layers` 描述的是产物,两者在图供给 C 库时分叉,而 JSON 没有任何
字段说明哪一个管用。

---

## 2. 缺陷一:`requires` 被检查,但从未被采纳

### 2.1 位置

`src/build/prepare.cppm` 已经有一个「图存在之后再定工具链」的接缝,它的标题就
是这么写的:

```cpp
// ─── The toolchain, resolved now that the graph exists ──────────────────
                                                          // 5055
for (auto const& pkg : packages)
    for (auto const& entry : pkg.manifest.provides)       // 5074
        …  graphSuppliesSystem = true;
…
if (auto r = resolve_target_toolchain(); !r) …            // 5165
```

这个循环扫 `provides`。**它不扫 `requires`。** 对称的那一半在一千行之后:

```cpp
for (auto const& entry : pkg.manifest.requires_)          // 6153
    requirements.push_back(…);
…
if (auto why = tsd::check_requirements(resolvedTargetSide, requirements)) {
    refusal::record(refusal::Code::LayerRequirement);     // 6290
    return std::unexpected(*why);
}
```

同一个 `packages` 容器,同一种 manifest 字段,一个用来**决定**,一个只用来
**核对**。

### 2.2 为什么这一处是可以修的

`resolve_target_toolchain()` 在 5165 首次被调用。此前没有任何 `tc` 消费者:
依赖的 `build.mcpp` 在 5863 编译,根工程的在 6476。⭐ **在接缝里改 `tcSpec`
不需要重新规划,也不会浪费任何已完成的工作。**

对照 6321 那段注释所说的、在 6328 处**做不到**的事:

> `tc` has been read and mutated at 39 sites between its resolution and this
> point — re-resolving it here would redo all of them out of order.

那是 6328。5101 不是。同一份注释还写下了正确的方向:

> The structural fix is to defer the pin the way the target side itself was
> deferred — resolve it after the graph, where the question it answers has an
> answer.

pin 已经这样延后了(`targetPinCandidate`,5101–5108)。**`requires` 是同一个决
定的第三个输入,只是还没接进来。**

### 2.3 谁的决定可以被改写

`TcOrigin` 已经把这个问题回答完了(`prepare.cppm:454`):

```cpp
export inline bool tc_origin_is_user_explicit(TcOrigin o) {
    return o == TcOrigin::ManifestToolchain || o == TcOrigin::TargetSection;
}
```

`GlobalDefault` 不在其中,并且那段 ⚠️ 注释逐字说明了为什么:target row 的 pin
必须能压过全局默认,因为 pin 说的是「谁供给这个目标的 C 库」。

⚠️ **1.1 里那次拒绝,发生在 `GlobalDefault` 之上** —— 用户跑过
`mcpp toolchain default gcc`,而 `mcpp toolchain default llvm` 打印的
`(was: gcc@16.1.0)` 就是它的收据。**mcpp 拒绝改写一个它自己的谓词判定为可改写
的值。**

更能说明问题的是:两种来源产生**逐字相同**的输出。

```
$ MCPP_TOOLCHAIN=gcc@16.1.0 mcpp build      # ManifestToolchain,用户写下的
  error: `openkal-llvm-runtime@0.1.3` requires the compiler to be `llvm`.
```

前者应当拒绝(用户写下了 gcc),后者不应当(那是 mcpp 记住的一个默认值)。
`TcOrigin` 分出的这个差别,在用户能看到的任何地方都不存在。

### 2.3.1 ⭐ 建议文本本身就是这个缺陷的指纹

`src/targetside/model.cppm:600–609`,拒绝时给出的全部补救办法:

```cpp
"       Select that compiler — yours outranks mcpp's own default:\n"
"           mcpp toolchain default {}\n"          // ← 全局,影响本机每一个工程
"       or, for one target only:\n"
"           [target.<triple>]\n"
"           toolchain = \"{}\""                    // ← 写进工程 manifest
```

两条**都是持久状态变更**,而触发它们的是一次构建里的一条依赖。第一条尤其错:
**一个工程的依赖要 llvm,代价是这台机器上每一个工程的默认编译器都变了。**

⚠️ 一个引擎在能自己做决定时却建议用户去改全局配置,通常说明这个决定被放在了拿
不到答案的位置上。这里正是如此——建议写于 2.2 所示的那一千行之后,而答案在一千
行之前就齐了。

### 2.4 版本从哪来

`requires = ["mcpp:compiler=llvm"]` 只给族,不给版本。这个问题**已经有答案**:
`mcpp toolchain default llvm` 就只给了族,而它解析出了 `llvm@22.1.8`。
`src/toolchain/lifecycle.cppm:1074`:

```cpp
if (auto picked = resolve_version_match(
        pkg.ximVersion, list_installed_versions(pkgsDir, pkg.ximName)))
```

⭐ **复用它,不要新写一条。** 一个裸族名在 mcpp 里只应该有一种解析方式;写第二
条就是把同一个决定推导两遍,而那是隐性架构债——两处推导今天一致,加新语义时会
变成构建失败。

---

## 3. 缺陷二:tier 闸问的是补全后的身份,而不是请求

### 3.1 补全是身份操作,不是请求操作

`src/toolchain/triple.cppm:642`:

```cpp
if (t.os == "linux" && t.env.empty()) t.env = "gnu";
```

紧挨着的注释把这件事说得很清楚:

> `x86_64-linux` is the canonical identity `x86_64-linux-gnu` … **but it is NOT
> the request** `x86_64-linux-gnu`, which names a C library.

`envExplicit` 这个字段就是为了记住这个区别而存在的,`prepare.cppm:1631/1640` 两
处都在用它:请求的 C 库只在 `envExplicit` 时被记下,报告名在 `!envExplicit` 时
去掉那一段。

⚠️ **而 tier 闸(`prepare.cppm:1550`)和 unknown 闸(1540)都不看 `envExplicit`。**

```cpp
const triple::TargetInfo* known = parsed ? triple::find_known_target(*parsed) : nullptr;
…
if (known && known->tier == "planned" && !hasToolchainOverride) { … }
```

`*parsed` 是补全后的身份。于是:

| 用户问的 | 引擎答的 |
|---|---|
| aarch64 的 Linux 支持吗 | aarch64-linux-**gnu** 支持吗 |
| riscv64 的 Linux 支持吗 | riscv64-linux-**gnu** 支持吗(此行不存在) |

第一行答「planned」,第二行答「unknown」。两个答案都对——对的是它们各自被问的
那个问题,而那不是用户问的问题。

### 3.2 补全没有看词表

补全是词法的:linux→gnu、windows→gnu、freestanding→elf。它在决定时不知道词表里
有什么。把词表按 (arch, os) 分组之后,唯一的分歧集中在两组:

| arch+os | 词表里的行 | 词法默认 | 结论 |
|---|---|---|---|
| x86_64 + linux | gnu `verified` / musl `verified` | gnu | 命中,不变 |
| x86_64 + windows | gnu `verified` / musl `preview` / msvc `verified` | gnu | 命中,不变 |
| freestanding | 全部 `elf` | elf | 命中,不变 |
| **aarch64 + linux** | musl `verified` / gnu `planned` | gnu | ⚠️ 落到 planned |
| **riscv64 + linux** | musl `planned` | gnu | ⚠️ 落到不存在的行 |

⭐ **改动面就是这两格。** 其余每一格的词法默认恰好是一个受支持的行,补全结果不
变。

### 3.3 `parse()` 不动

`tests/unit/test_toolchain_triple.cpp:67`:

```cpp
EXPECT_EQ(parse("x86_64-linux")->str(), "x86_64-linux-gnu");
```

`parse()` 必须保持**词法、全量、与宿主无关**。triple.cppm 的注释为最后一条给了
理由:从宿主的 env 填会让同一条命令在不同机器上得到不同的输出目录和缓存键,
「a target's identity may not depend on where it was built」。

因此补全要作为**请求站点的一个独立步骤**,而不是改 `parse()`。词表是编译期数
据,在每台宿主上相同,所以这个步骤仍然与宿主无关。

---

## 4. 优化方案

### 4.1 A —— 图声明的编译器参与工具链决定

改动落在 `prepare.cppm:5071–5108` 这一个块内。

1. 在既有的 `packages` 循环里同时扫 `requires_`,取 `mcpp:compiler=<family>`。
2. 出现两个不同的 family ⇒ 拒绝,逐字对齐既有的
   「ONE SUPPLIER PER LAYER, AND TWO IS AN ERROR RATHER THAN A PICK」
   (`prepare.cppm:6056`)那条规则的措辞与形状。
3. 恰好一个,且与当前 `tcSpec` 的 family 不同:
   - `tc_origin_is_user_explicit(tcOrigin)` ⇒ **不改**,走今天的
     `LayerRequirement` 拒绝。用户写下的东西不被改写。
   - 否则 ⇒ `tcSpec = <family>`(裸族名,由 4.5 的既有路径定版本),
     `tcOrigin = TcOrigin::GraphRequirement`。
4. 与 pin 的关系**不需要新的优先级**:图要求编译器的场景里,若图同时供给系统,
   `graphSuppliesSystem` 已经取消了 pin;若图不供给系统,那正是
   `ConventionUnreplaced`(5142)要拒绝的局面,拒绝先发生。

新增 `TcOrigin::GraphRequirement`,并且:

- `tc_origin_is_user_explicit()` 不含它(它是 mcpp 的推导,不是用户的话)。
- `tc_origin_name()` 给它一句话。
- 状态行按 `pinReplacedDefault` 的先例说明缘由:

```
    Resolved llvm@22.1.8 → …/xim-x-llvm/22.1.8/bin/clang++
             required by openkal-llvm-runtime@0.1.3 (`requires = ["mcpp:compiler=llvm"]`),
             replacing your default gcc@16.1.0 for this project
```

- `mcpp why toolchain --format json` 在 `data` 下给出该来源,使
  「为什么是 llvm」可被机器回答。

⭐ **`check_requirements` 的建议文本一并改写(§2.3.1)。** 拒绝在方案 A 下只剩
一种局面——工程自己写下了相反的编译器——而那种局面里**全局默认与本次构建无
关**,建议 `mcpp toolchain default llvm` 是答非所问:改了它,这条拒绝照旧。剩下
的正确补救只有两条,都在工程内:

```
       This project states its own compiler, and that outranks the graph:
           [toolchain]  default = "gcc@16.1.0"     ← 改成 "llvm",或删掉这一行
       Removing it lets mcpp take the compiler the graph asks for.
```

⚠️ 这一条不是文案润色。今天那两条建议里,**唯一能解决问题的那条会改变本机每一
个工程**,而它被排在第一位;方案 A 落地后它连问题都解决不了。建议文本与引擎行
为一起变,否则会留下一条指向不存在的机制的指引。

⚠️ **族没装时不新增行为。** 落到既有的 payload 安装路径;`MCPP_NO_AUTO_INSTALL`
下走既有的拒绝并带上安装写法。这一条不引入新的网络效应,只是把选择权从用户手里
移到了图上。

### 4.1.1 ⭐⭐ 这次选择不写任何东西,而且这是位置带来的,不是额外加的开关

`resolve_target_toolchain` 在整个 `prepare.cppm` 里**只有两个调用点**:

```
2255:  return resolve_target_toolchain();      // 它自己的一次性递归
5165:  if (auto r = resolve_target_toolchain(); !r)
```

也就是说,它整个函数体——包括那条**首次运行安装并持久化**的分支——都在图之后
才执行。分支链(1850 / 2010 / 2012 / **2061**)的最后一格是首次运行,它的进入条
件是 `!tcSpec.has_value()`;而全部三处 `write_default_toolchain`
(2157 / 2206 / 2546)都在这一格里面。

于是把图的要求写进 `tcSpec` 的时机(5101)**早于首次运行分支被求值**:

| | 今天 | 方案 A |
|---|---|---|
| 已有 gcc 默认的机器 | 拒绝,要求改全局默认 | 装/用 llvm,`config.toml` 不动 |
| **一台什么都没装的机器** | 装 gcc → 持久化 gcc → 再拒绝 | `tcSpec` 已是 `llvm` ⇒ **首次运行分支根本不进** ⇒ 直接装 llvm,**什么都不写** |

⭐ **「不改全局配置」不是给方案 A 加的一条约束,而是把决定放对位置后的自然结
果。** 不需要新的开关,也没有需要有人记得不去碰的写入点——那些写入点位于一条
不再进入的分支上。

⚠️ 相应地,**图的要求不得反过来喂给持久化路径**。它是这一次构建的性质,不是这
台机器的性质;把它写回 `~/.mcpp/config.toml` 会让下一个不含该依赖的工程继承一个
没人要求过的编译器。E10 是执行这条承诺的那个判据。

### 4.2 B —— 补全参照词表

新增 `triple::resolve_request(Triple&)`(或等价的自由函数),仅在
`prepare.cppm` 的请求站点、且 `!envExplicit` 时调用,位置在
`[target.X]` 段查找之前(1521)——查找键必须是解析后的身份。

规则,按序:

1. 词法默认命中一个 tier ≠ `planned` 的行 ⇒ 用它。(x86_64-linux → gnu)
2. 否则,同 (arch, os) 下恰好一个 tier ≠ `planned` 的行 ⇒ 用它。
   (aarch64-linux → **musl**)
3. 否则(该组为空,或全 `planned`)⇒ 保留词法默认,让 1540/1550 的既有诊断照旧
   触发,但用**该组存在的行**改写消息(见 4.3)。
4. 多个受支持的兄弟行且词法默认不在其中 ⇒ 拒绝并列出候选。今天词表里没有这一
   格;规则先写下,免得第一次出现时靠猜。

⭐ **规则 1 让这件事自己退休。** `aarch64-linux-gnu` 一旦升到 `verified`,规则 1
先命中,补全自动回到 gnu,不需要有人记得回来删规则 2。

⚠️ **短拼写不承担「早期加入」。** 若工程写了
`[target.aarch64-linux-gnu] toolchain = …` 想提前用 planned 行,它要写全三段。
一个拼写回答一个问题;让短拼写既表示「给我受支持的那个」又表示「并且照顾我的提
前加入段」,是让它同时回答两个。

### 4.3 C —— 两条诊断说真话,并且留下记号

- `unknown target` 在 (arch, os) 组非空时不再使用。`riscv64-linux` 应得到
  `planned` 那条消息,主语是 `riscv64-linux-musl`。
- 消息里引用的三元组必须是**用户写下的那个**,或明确写成「你写的 X,它指的是
  Y」。今天 `--target aarch64-linux` 的报错主语是 `aarch64-linux-gnu`,用户没有
  打过这个字符串。
- `unknown target` 路径补 `refusal::record`。新增 `Code::UnknownTarget`,
  `--format json` 的 `reason` 从 `other` 变成具名值。

### 4.4 D —— `why toolchain` 的 C 库只有一个答案

`cLibrary` 与 `layers[].c-abi` 在图供给 C 库时说的是两件事。两条路可选,倾向
第二条:

1. 图供给 c-abi 时,`cLibrary.mode` 置为一个表示「不适用」的值。
2. **保留两者并改名**,让字段名说清它们各自回答什么:
   `payloadLinkModel`(payload 的链接搜索模型)与 `layers[].c-abi`(产物里的
   C 库)。二者本就不是同一个问题,今天的字段名让它们看起来是。

⚠️ 这一条与 A/B 无依赖,可以单独走。它是**机器接口的自洽性**问题:两个字段在
同一份文档里对同一个事实给出不同答案,而消费方无从判断该信哪个。

---

## 5. 验收体系

判据全部走 `--format json`,不做字符串搜索。

| # | 判据 | 方式 |
|---|---|---|
| E1 | 图要 llvm、全局默认 gcc、llvm 已装 ⇒ `status=ok`,`compiler.family=clang` | `why toolchain --format json` |
| E2a | 同上,但工程写了 `[toolchain] default = "gcc@16.1.0"` ⇒ `refused` / `layer-requirement` | 同上 |
| E2b | 同上,但工程写了 `[target.<triple>] toolchain = "gcc@16.1.0"` ⇒ `refused` / `layer-requirement` | 同上 |
| **E10** | **E1 前后 `~/.mcpp/config.toml` 的 sha256 相同** | `sha256sum` 对照 |
| **E11** | 无任何工具链的机器上跑 E1 ⇒ 只装 llvm,不出现 `First run … installing gcc` | 构建日志 + `config.toml` 仍无 `default` |
| E3 | E1 的状态行含图的选择缘由,且 `data` 里该来源可读 | 同上 |
| E4 | 两个包要求不同 family ⇒ `refused`,消息同时点名两个包 | 同上 |
| E5 | `--target aarch64-linux` ⇒ `status=ok`,产物目录 `target/aarch64-linux-musl/` | `why` + 目录存在 |
| E6 | `--target x86_64-linux` ⇒ 产物目录仍是 `x86_64-linux-gnu/` (不回归) | 同上 |
| E7 | `--target riscv64-linux` ⇒ `reason=tier-planned`,消息主语含 `riscv64-linux-musl` | `why toolchain --format json` |
| E8 | `parse("x86_64-linux")->str() == "x86_64-linux-gnu"` 仍然成立 | 既有单测,不改 |
| E9 | 图供给 c-abi 时,C 库在 JSON 里只有一个答案 | `why` + `readelf -d` 对照 |

⚠️ **E5/E6 必须成对。** 只测 aarch64 会让「把 linux 的默认整个换成 musl」这种
过头的实现看起来是对的。E6 是那条对照。

⚠️ **E2a/E2b 必须成对存在。** A 的全部风险在于它改写了谁的决定;没有这两条,
「不改写用户写下的东西」这条承诺就没有任何东西在执行它。两条分别覆盖工程级和
目标级两种写法——只测一种,另一种的豁免会静默生效。

⚠️ **E10 是一条「什么都没发生」的判据,因此必须落到 sha256 而不是落到构建成
功。** 构建成功与配置被改写可以同时为真,而那正是这次要消除的行为。

⚠️ **E11 需要一台没有工具链的机器,本机永远看不见。** 它属于 CI 的 fresh-install
轴(bootstrap 之后、任何 `toolchain install` 之前)。判据是**日志里不出现
`installing gcc`** ——「装了 llvm」是恒真的,两条路径都会装 llvm,区别只在有没有
先装一个没人要的 gcc。

- e2e 编号从 `299` 起(现有最大 `298`)。
- E5/E6 在 `tests/matrix/scan.sh` 里有天然位置:短拼写是**请求**这一列的一个新
  取值,`expected.tsv` 增两格,四台构建机各一次。
- ⚠️ E5 的产物目录判据要落到**目录名**,不要落到构建成功。补全错到 gnu 而工具链
  仍然能出 aarch64 产物的世界里,只看「构建成功」是恒绿的。

---

## 6. 共同形状,以及为什么值得单列

2026.8.25.x 修的那七条是**谓词回答了比它被问的更窄的问题**。这两条不是:

| | 8.25.x 家族 | 本文两条 |
|---|---|---|
| 谓词 | 问错了 | 问对了 |
| 答案 | 错的 | 对的 |
| 缺陷位置 | 判据本身 | 答案没有接到决定上 |

- `requires` 被完整解析、完整核对,只是没有参与选择。
- `envExplicit` 被完整记录、在报告里被完整使用,只是没有参与查表。

⭐ **两者都是「多存了一个字段而没有多接一根线」。** 这种缺陷不会在读判据时被发
现,因为判据是对的;它只在用户问「你既然已经知道了,为什么还要我说一遍」时暴
露。1.1 的用户原话就是这句。

⚠️ 由此得到一条可复用的检查:**新增一个记录性字段时,列出它的读者。** 只有一个
读者(报告)而没有决策读者,通常意味着这根线没接完。`envExplicit` 今天的读者是
两处报告和零处决定。

---

## 7. 不做什么

- **不改 `parse()` 的填充。** 身份必须全量、词法、与宿主无关(§3.3)。
- **补全不看 `host_can_serve`,不看图。** 目标身份不得依赖构建它的机器。词表是
  编译期数据,这是补全能参照的唯一一张表。
- **不让图要求压过用户写下的工具链。** `[toolchain]` / `[target.X]` /
  `MCPP_TOOLCHAIN` 保持今天的拒绝(E2a/E2b)。这是**唯一**保留拒绝的局面。
- **不写任何持久状态。** 不改 `~/.mcpp/config.toml`,不改工程的 `mcpp.toml`。
  图的要求是这一次构建的性质,不是这台机器的性质,也不是这个工程的声明。
  §4.1.1:这不是一条需要有人遵守的约束,而是决定放对位置后的结果——那些写入点
  位于一条不再进入的分支上。
- **不建议用户去改全局默认。** 拒绝仅剩的那种局面里全局默认与本次构建无关,
  建议它是答非所问(§4.1 末)。
- **不给短拼写加「提前加入」语义**(§4.2)。
- **aarch64 的其余目标仍然延缓**,见
  [`2026-08-26-aarch64-linux-ecosystem-closure.md`](2026-08-26-aarch64-linux-ecosystem-closure.md)。
  §4.2 规则 1 保证 `aarch64-linux-gnu` 升级后补全自动跟随。

---

## 8. 顺序与代价

| 项 | 依赖 | 触及 | 风险 |
|---|---|---|---|
| C(诊断说真话 + 记号) | 无 | `prepare.cppm` 1540/1550、`refusal.cppm` | 低,纯消息与记号 |
| B(补全参照词表) | C 先落更好读 | `triple.cppm` 新函数、`prepare.cppm` 请求站点 | 低,改动面两格(§3.2) |
| D(JSON C 库自洽) | 无 | `doctor.cppm` | 低,但改字段名是接口变更,须进 `kindVersion` 讨论 |
| A(图声明的编译器) | 无 | `prepare.cppm` 5071–5108、`TcOrigin`、状态行、`why` | 中,它改写决定 —— E2 是闸 |

C、B、D 相互独立;A 独立于三者。四项可并行,合入顺序按上表从上到下最易复查。
