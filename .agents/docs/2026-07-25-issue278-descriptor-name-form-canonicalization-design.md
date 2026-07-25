# #278 包身份口径收敛 — 索引侧 + 依赖侧完整方案

> Issue: [mcpp-community/mcpp#278](https://github.com/mcpp-community/mcpp/issues/278)
> 相关:mcpplibs/mcpp-index#116(索引侧临时 lint 守卫)、事故现场 mcpp-index run 30121144277
> 前置设计:`2026-06-20-package-resolution-architecture.md` §4、`2026-06-26-identity-first-resolution-no-filename.md`
> 用户文档落点:`docs/05-mcpp-toml.md` §2.5、`docs/zh/05-mcpp-toml.md` §2.5

---

## 1. 结论

包身份在 mcpp 里有**两侧**,两侧今天都不自洽:

- **索引侧(xpkg 描述符)**:身份归一化容忍 `(ns="a", name="b")` 与 `(ns="a", name="a.b")` 等价,而安装目标构造硬假设后者。结果:split 形式的包能过解析、能过身份闸门,却没有任何消费写法能装上。
- **依赖侧(mcpp.toml)**:裸名依赖的候选阶梯 `(mcpplibs, name)` → `(∅, name)` 中,第二档在规范里是"跨命名空间按名发现",在实现里是"再试两个文件名",且落空后**静默回退**到第一档,把 mcpp 自己编造的命名空间当作结论继续跑。

本方案对两侧同时**做收敛**,而不是继续扩大模糊匹配:

- **索引侧** — `package.name` 必须是完全限定名(FQN)。这不是风格约定,是被 xlings 扁平 key 空间**结构性强制**的(§3.1)。
- **依赖侧** — 命名空间缺省时**只**解析 `mcpplibs` / `compat` / 无命名空间的上游包三类;任何第三方命名空间的包必须写完整(`a.b.c` 或 `[dependencies.<ns>]`)。**放弃全域模糊发现**,以依赖解析的稳定性与可复现性换取便捷性。
- **全索引扫描降级为纯诊断** — 只在已经确定失败的路径上跑一次,产出 did-you-mean,**永不参与解析**。既保住"用户知道该怎么写",又完全避开供应链隐患(§5.3)。

---

## 2. 事实核验

issue 的三条根因分析全部属实,已对 HEAD 逐条核对:

| 主张 | 核实位置 | 结论 |
|---|---|---|
| 归一化把 split 与 FQN 视为等价 | `src/manifest/xpkg.cppm:657-664` | ✅ |
| 安装目标假设 `name == ns + "." + short` | `src/build/prepare.cppm:1716-1717`、`:1756` | ✅ 契约写成散文,无断言 |
| `luaContent` 已在作用域内却未使用 | `prepare.cppm:1584` → 目标构造 `:1716`,同一 lambda | ✅ 零额外 I/O 即可校验 |
| `mcpp xpkg parse` 只校验 `name` 缺失 | `src/cli/cmd_xpkg.cppm:80-85` | ✅ |
| `mcpp emit xpkg` 生成 split 雏形 | `src/pm/publisher.cppm:133`,全程不输出 `namespace` | ✅ |

### 2.1 split 形式在真实生态里是 2/62 的异类

对本机完整索引快照(`~/.mcpp/registry/data/*/pkgs/**/*.lua`)全量抽取 `(namespace, name)`。**所有声明了 `namespace` 的描述符中,只有两个不是 FQN 形式:**

```
chriskohlhoff.asio.lua   ns='chriskohlhoff'  name='asio'            ← 事故当事人
tensorvia-cpu.lua        ns='aimol'          name='tensorvia-cpu'   ← 同源,静默破损
```

其余全部 FQN(`compat.*` 全族、`mcpplibs.*` 全族、`fmtlib.fmt`、`nlohmann.json`、`marzer.tomlplusplus`,含嵌套命名空间的 `ns='mcpplibs.capi' name='mcpplibs.capi.lua'`)。无 `namespace` 的 xim 上游包(`opencv` / `musl-gcc` / `linux-headers` …)属另一类,不受本规则约束。

### 2.1b ⚠️ 实施期修正:`package.namespace` 在 xlings 生态里有第二种含义

上面的 62 例只覆盖了 **mcpplibs 索引**。实现 T2 后对**完整 registry**(200+ 描述符,含 `xim-pkgindex`、`xim-pkgindex-scode`、`xim-pkgindex-awesome`)跑谓词,得到 **30 个 split 形式**,而非 2 个。逐个查看后结论明确:

```
ns=config   name=claude-llm / mcpp-vscode-clangd / rustup-mirror …   type=config|bugfix
ns=scode    name=zlib / openssl / ncurses / readline …               type=package|lib
ns=awesome  name=d2x / dragonos / xim …                              type=pkgindex
ns=fromsource name=util-linux                                        type=package
```

**在 xlings-native 索引里,`package.namespace` 是安装目录分类(install-dir category),不是包命名空间。** 索引由 `build_index(repoDir_)` 建、key 是裸 `pkg.name`,消费端 `xlings install zlib` 直接命中 —— **split 形式在那个世界里是正确的**。30 例全部 `mcpp` 段缺失、全部是 xlings 原生类型,mcpp 的依赖解析路径从不经过它们。

**这意味着 INV-NAME 只在 mcpp 语义下成立**,谓词不能无差别地对所有描述符开火。实施结论:

- **默认开启**(mcpp-index 的 CI 正是 `mcpp xpkg parse pkgs/*/*.lua`,免费获得防护);
- 新增 **`--allow-split-name`** 供 xlings-native 树使用;
- 现网**没有任何 xim 仓运行 `mcpp xpkg parse`**(已核 `.github/`),故误报是理论风险而非现实风险。

`index.toml` 曾被考虑作为自动判别器(**只有** mcpp 索引带它,xim 三个索引全无),但对"作者单独 lint 一个文件"的常见流程会静默跳过检查,故改用显式 flag。此判别器留作后续可选增强。

### 2.2 查找侧已兼容 split,只有目标串不兼容 —— 这是 A 方案的陷阱

- `src/pm/compat.cppm:245` `install_dir_candidates` 已产出 `{ns}-x-{shortName}`(= split 描述符的真实 store 目录);
- `src/pm/compat.cppm:190` `xpkg_lua_candidates` 已产出裸 `<shortName>.lua`。

即 split 唯一的硬失败点就是那一条 target 串。但这些候选项全部标注 `COMPAT, remove in 1.0.0` —— **"让 target 改用字面 name"等于把一批已排期删除的兜底提升为永久承重路径**,并让同一个包在 search / store 目录 / 错误文案里永久二义。

### 2.3 同类缺陷的潜伏支路

单测 `XpkgIdentity.DefaultNamespaceBareNameGatedByFlag`(`tests/unit/test_manifest.cpp:1854`)刻意接纳 `package = { name = "cmdline" }`(无 namespace),请求 `(mcpplibs, cmdline)` 在 `allowLegacyBareDefault=true` 下通过。对这种描述符 xlings 的 key 是 `cmdline`,而 mcpp 推导出 `mcpplibs.cmdline` —— **同一 bug 类**。今天没炸只因真实 mcpplibs 描述符全是 FQN 形式。

结论:身份闸门容忍 **三** 种形式(FQN / split / legacy-bare),目标构造器只支持 **一** 种。

### 2.4 规范考古:归一化没失效,是它配套的另一半从未实现

`2026-06-20 §4.6(b)` 的规范表第一行就是 `aimol | tensorvia-cpu | mcpplibs → (aimol, tensorvia-cpu)`,标注 `← the incident package`,并附 `Equivalence (must all resolve identically) … The user's point, encoded.`。**split 不是越界写法,它是规范当初要照顾的那个案例。** 归一化按规范正确工作了 —— 它是 many-to-one 投影,`(a,b)` 与 `(a,a.b)` 都塌到 `(a,b)`,而 `prepare.cppm:1716` 需要逆向还原字面值,原像有两个,它无条件挑了 `a.b`。

`2026-06-26 §4.5` 写了配套解法:

> key the index/cache on canonical `(ns, name)`, so the split form and the FQN form produce **identical keys and identical resolution**. This makes the producer (emit), the catalog (cache), and the consumer (locate) agree on one key.

而该文档 Status 行:Step 0 landed in v0.0.67(candidate selection is now identity-first);**§5 `PackageLocator` / `IdentityIndex` remains follow-up**。§4.5 的 producer(emit 双字段)与 catalog 收口从未落地 —— 本方案 T4 一字不差写在 §4.5 里。

**并且 §4.5 的前提不成立:catalog 不归 mcpp 管。** 见 §3.1。

> 模糊匹配是 mcpp 的内部方言,而 `package.name` 同时是外部系统的主键。在一个别人拿来做精确主键的字段上做归一化,归一化越成功,越掩盖分歧。

### 2.5 依赖侧候选阶梯的两个实现缺口

`resolve_dependency_selector`(`src/pm/dependency_selector.cppm:88-98`)对裸名产出的阶梯与规范一致:

```
cand①  (mcpplibs, name)     ← kDefaultNamespace 优先
cand②  (∅,        name)     ← 规范语义:跨 ns 按名发现
```

消费它的 `selectDependencyCandidate`(`prepare.cppm:1516-1541`)有两处未兑现规范。

**缺口 A — `(∅, name)` 没有扫描能力,只是"再试两个文件名"。**

`2026-06-26 §4.4` 定义该档为 `locateByName(name)` = "match by `name` alone **across the precedence path**",前提是存在由读遍每个描述符声明字段构建的 `IdentityIndex`。**IdentityIndex 从未实现。** 三个 `read_xpkg_lua*` 入口全是"候选文件名探测 + 命中后校验";`package_fetcher.cppm` 的 `directory_iterator` 只遍历**索引目录**(`sorted_index_dirs`),从未遍历 `pkgs/*/*.lua`。

裸 `asio` 的精确 trace(按 `compat.cppm:159-205`):

| 候选 | 实际探测的文件 | 磁盘 |
|---|---|---|
| ① `(mcpplibs, asio)` | `a/asio.lua`、`m/mcpplibs.asio.lua`、`c/compat.asio.lua` | 均不存在 |
| ② `(∅, asio)` | `a/asio.lua`、`c/compat.asio.lua` | 均不存在 |

真实文件 `c/chriskohlhoff.asio.lua` **不在任何一档候选名里**。identity-first 只落地了**验证**半边(拒绝假命中),**发现**半边从未落地;文档标题喊 "Filename Is Not a Key",而文件名今天仍是发现的唯一键。`prepare.cppm:1456-1459` 的注释描述的是**消歧判据**(已兑现),读起来却像承诺了发现不依赖文件名(未兑现)。

**缺口 B — 全落空静默回退 `front()`;discovery 档命中时写回候选的 ns。**

```cpp
auto selected = candidates.front();                    // 预设 (mcpplibs, name)
if (spec.isVersion() && candidates.size() > 1) {
    for (auto& candidate : candidates) {
        auto lua = readStrictLuaForCandidate(candidate);
        if (lua && xpkgLuaMatchesCandidate(candidate, *lua, false)) { selected = candidate; break; }
    }
}
spec.namespace_ = std::move(selected.namespace_);      // ← 候选的 ns,非描述符声明的 ns
```

- **B1** 全部落空**不报错**,把预设 `(mcpplibs, name)` 当结论继续跑,错误推迟到下载/安装阶段,且文本里带着 mcpp 自己编造的 ns。
- **B2** discovery 档命中时写回候选的空 ns,而 `extract_xpkg_namespace(*lua)` 一行即得声明值,被丢弃。规范 §4.4:"resolved to a real `(ns, name)` before anything downstream sees it (P3)."
- 附注:`xpkg_lua_identity_matches` 的 discovery 分支注释写着 "the caller derives the namespace from the descriptor" —— **闸门在散文里把 P3 责任交给调用方,调用方没做**。这是本问题域内第三处"契约只写在注释里、无人执行"(另两处:`prepare.cppm:1714` fqname 契约、`:1456` identity-first 注释)。

**重解 issue 的四行消费端表**(第 4 行归因与 issue 不同):

| 消费端写法 | 真实失败阶段 | 收敛后定性 |
|---|---|---|
| `[dependencies.chriskohlhoff]` + `asio` | 描述符找到,死在 target 串 | ✅ **正确写法** |
| `[dependencies.chriskohlhoff]` + `"chriskohlhoff.asio"` | ns 拼两遍,选择器层失败 | ❌ 重复限定 |
| `[dependencies]` + `"chriskohlhoff.asio"` | cand② `(chriskohlhoff, asio)` 命中,死在 target 串 | ✅ **正确写法** |
| `[dependencies]` + 裸 `asio` | **缺口 A** 两档全 miss → **B1** 静默回退 → 误导性错误。issue 归因为"身份闸门失败",实际更早,在 read 探测阶段 | ❌ **错误写法**,须明确拒绝 |

**三个缺口彼此独立**:即便 A/B 修好、discovery 正确解出 `(chriskohlhoff, asio)`,target 仍是 `chriskohlhoff.asio` 而 xlings key 仍是 `chriskohlhoff-x-asio`;反之只修 target 串,裸名写法仍找不到描述符。

---

## 3. 两条不变式

### 3.1 INV-NAME(索引侧)—— `package.name` 必须是 FQN

> 描述符的 `package.name` 字面值**就是**它的完全限定名。当 `package.namespace` 非空时,`name` 必须以 `namespace + "."` 开头,且余下短名非空。

**这是被结构性强制的,不是风格偏好。** 已对 libxpkg 与 xlings 源码核实:

```cpp
// openxlings/libxpkg/src/xpkg-loader.cppm:587 — 建索引
std::string key = (namespace_.empty() ? "" : namespace_ + "-x-") + pkg.name;
index.entries[key] = std::move(ie);
// openxlings/libxpkg/src/xpkg-index.cppm:37,111 — 查找
auto it = index.entries.find(name);      // 精确字符串相等,零归一化
if (index.entries.count(name)) ...
// openxlings/xlings/src/core/xim/index.cppm:153 — 唯一调用点
auto result = xpkg::build_index(repoDir_);   // namespace_ 缺省 = 空
```

**关键修正**:`build_index` 的唯一调用点**不传 namespace_**,所以 key 就是 `pkg.name` **字面值本身**,连索引前缀都没有。每个索引一份扁平表,查找是精确 `find()`。

于是同一个索引内 **`pkg.name` 是全局唯一键**。mcpplibs 索引里同时住着 `compat.*` 与 `mcpplibs.*` 两族 —— 若 `name` 都写短名,`compat` 的 `zlib` 与 `mcpplibs` 的 `zlib` 会撞成同一个 key。**命名空间的区分度必须由 `pkg.name` 自身携带 —— 这就是 FQN 形式的来源。** mcpp 的 `canonical_xpkg_identity` 对这个 key 空间没有任何管辖权,§4.5 期望的"三方同 key"只能靠约束 wire key 的书写形式达成。

推论:mcpp 从 `(ns, shortName)` 推导目标串在 INV-NAME 下**构造性正确**,`prepare.cppm:1716` 的推导**无需改动**;读取字面 `name` 的价值**只在于校验**。

### 3.2 INV-RESOLVE(依赖侧)—— 缺省命名空间的解析域是封闭的三档

> `[dependencies]` 中不带命名空间的裸名,**只**解析到以下三类,按序:
> ① `mcpplibs`(默认命名空间) ② `compat`(包装命名空间) ③ 无 `namespace` 声明的上游包(xim)
>
> **任何声明了第三方 `namespace` 的包,裸名请求一律不解析** —— 必须写 `a.b.c` 点式选择器或 `[dependencies.<ns>]` 子表。

① 与 ② 今天**已经**由 `xpkg_lua_identity_matches:705-711` 的 `ns == kDefaultNamespace` 分支实现(`id.ns == kDefaultNamespace || id.ns == kCompatNamespace`)。本方案要做的是把 ③ 从"任意 ns 都能命中"收敛为"仅无 ns 声明者",并把落空定性为错误。

**设计理由**:全域按名发现带来的便捷性,代价是三条稳定性损失 ——
1. 两个命名空间同名时的裁决依赖"索引优先级",而该优先级只对 builtin 索引有定义,用户 `[indices]` 添加的索引之间**无全序**;
2. **新增一个索引可以悄悄改变已有裸名依赖的解析目标** —— 供应链意义上的隐患;
3. 解析结果依赖本机索引快照的内容,同一份 `mcpp.toml` 在不同机器上可能解析到不同包。

依赖解析的可复现性优先于书写便捷性,故收敛。

---

## 4. 决策

### 4.1 索引侧:采纳"规范收紧",否决"target 改用字面 name"

| | 让 target 用字面 `name` | **规范收紧 + lint 拦截** |
|---|---|---|
| 修 #278 | ✅ | ✅ |
| 身份多义 | ❌ 永久二义 | ✅ 单一口径 |
| COMPAT 债 | ❌ split 兜底从"1.0.0 删除"变成承重 | ✅ 可按期删除 |
| 生态冲击 | 无 | 需改 2 个描述符 |
| 失败可见性 | 仍无 lint | ✅ 秒级拦截 |
| 与 §3.1 扁平 key 空间 | ❌ 无解(短名必然撞 key) | ✅ 顺应 |

**同时吸收"字面 name 就在手里"这一洞察**,但用途是 **fail-fast 诊断**,不是解析。

**表述澄清**:这**不是**推翻 §4.2 的匹配规范。`canonical_xpkg_identity` / `xpkg_lua_identity_matches` **一个字不改**,§4.6(b) 的等价关系对**已在索引里的**描述符继续成立;改的是**发布侧的规范化约束** —— 进入索引的描述符必须已是规范形式。即 *canonicalize at the boundary, match liberally inside*。

**诚实记录代价**:§4.6(b) 那条等价标注着 "The user's point, encoded",本方案让其中 split 一支在**新发布**时不再被接受。这是对一条已背书规范的定向收窄,不是纯 bug fix。

**否决的第三条路**:在 libxpkg 侧给索引加 alias 表(同时按 `name` 与 FQN 建 key)。能救两种拼写,但把二义性下沉到更难改的上游,需级联发版。

### 4.2 依赖侧:采纳收敛,否决 IdentityIndex 全域发现

否决理由见 §3.2 三条。**但全索引扫描并非全盘不要 —— 它降级为纯诊断能力**(§5.3):只在已确定失败的路径上跑,产出 did-you-mean,不参与解析。这样彻底避开三条稳定性损失,同时保住便捷性中最有价值的部分。

> 扫描只用于 did-you-mean,永不用于解析。

---

## 5. 实施

### 5.1 索引侧 — 一个谓词,三个落点

**核心谓词**(`src/manifest/xpkg.cppm`,与 `canonical_xpkg_identity` 并列导出):

```cpp
// INV-NAME(#278):描述符的 package.name 字面值就是它的 FQN。
// 违反时返回可直接展示的诊断;符合(或无从判定)时返回 nullopt。
//
// 判定只在「声明了非空 namespace」时生效 —— 刻意收窄:
//   • 无 name          → nullopt(沿用身份闸门宽松语义)
//   • 无 namespace     → nullopt(xim 上游裸包合法)
//   • name 以 ns+"." 开头 → nullopt
//   • 否则             → 违规
std::optional<std::string>
xpkg_name_form_violation(std::string_view declaredNs, std::string_view declaredName);

std::optional<std::string>
xpkg_name_form_violation_from_lua(std::string_view luaContent);
```

**为什么必须收窄,而不是"字面 name != 推导 fqname"的通用比较**:后者会误伤今天正常工作的 compat 别名路径 —— 裸写 `gtest = "1.15.2"` 时读到 `compat.gtest.lua`(`ns='compat' name='compat.gtest'`),推导 fqname 是 `gtest`,通用比较会判违规并硬失败,而该路径靠 `prepare.cppm:1760` 的 `compat.<short>@version` 重试是**正常可用**的。收窄后对三类现存路径全部中立:compat 别名(合规)、legacy-bare(跳过)、无名描述符(跳过)。

**一个谓词、两处调用**是架构要点 —— lint 与运行期共用同一份判定,不再各自推导。

- **T1 lint 层**(`src/cli/cmd_xpkg.cppm`,主防线):`mcpp xpkg parse` 在现有 `name` 缺失检查后调用谓词,违规 **exit 1**:

  ```
  error: pkgs/c/chriskohlhoff.asio.lua: package.name must be the fully-qualified
         name when package.namespace is declared
           namespace = "chriskohlhoff"
           name      = "asio"          <-- expected "chriskohlhoff.asio"
         The index is keyed by the literal package.name (flat key space:
         "<index>-x-<package.name>"), so this descriptor parses but can never
         be installed (E_NOT_FOUND). See mcpp#278.
         fix: name = "chriskohlhoff.asio"
  ```

  `--json` 同步输出 `"error"` 字段供索引 CI 机读。索引侧 lint 只需跑 `mcpp xpkg parse`,mcpp-index#116 的手写守卫退为冗余保险。

- **T2 运行期 fail-fast**(`src/build/prepare.cppm`):lint 只保护过索引 CI 的描述符;第三方索引、历史快照、本地 path index 绕不过运行期。在 `loadVersionDep` 构造 target **之前**(`:1716`)插入谓词检查,零额外 I/O(`luaContent` 已在作用域内),把三平台 20~58 分钟后的 `E_NOT_FOUND` 变成秒级自解释失败。
  **放置位置取舍**:放在 install 分支内(而非 `readLuaContent()` 之后),使"本机已装旧快照"的工程不因升级 mcpp 突然 hard fail;但这保留了 issue 点名的**遮蔽陷阱**(本机绿、干净 CI 红),故配套 **T2b**:已解析到安装物、跳过安装的路径上,同一谓词降级为 `ui::warn` 一行。

- **T4 关掉生成源**(`src/pm/publisher.cppm`):`emit_xpkg` 目前只写 `name = manifest.package.name` 且不输出 `namespace`,维护者归档时手补 `namespace` 的那一刻描述符即破损(`tensorvia-cpu` 正是这么来的)。`Manifest::Package::namespace_` **已存在**(`types.cppm:39`)、`[package] namespace` **已解析**(`toml.cppm:180`),故修复是纯输出层:
  - `namespace_` 非空 → 输出 `namespace = "<ns>"`,`name` 写 `<ns>.<name>`(若已带该前缀则不重复拼);
  - `namespace_` 为空 → 保持现有输出,并在生成注释与 stderr 提示:归档进 namespaced 索引前需声明 `[package] namespace`,否则手工补 `namespace` 会产出无法安装的描述符(引 #278);
  - 附带 `mcpp emit xpkg --namespace <ns>` 覆盖开关。

### 5.2 依赖侧 — 收敛 discovery 档 + 落空即报错

- **T9(缺口 B1)** `selectDependencyCandidate` 全候选落空时**不再静默回退** `front()`,改为明确失败。这是本批次对用户体验改善最大的一项。

- **T10(缺口 B2 / P3)** discovery 档命中后,用 `extract_xpkg_namespace(*lua)` 把 `spec.namespace_` 回填为描述符**声明**的 ns。
  **注意语义细节**:对无 `namespace` 声明的 xim 上游包(`opencv` / `musl-gcc`),`extract_xpkg_namespace` 返回空,**空 ns 在此是合法身份**(下游 `fqname == shortName`,与 xlings 无索引前缀的 key 一致),不得强行填充。规范 §4.4 的 "No empty namespace ever reaches …" 建立在 §4.1 index-owned namespace(`xim-pkgindex → xim`)之上,而 §4.1 同样未实现 —— 在它落地前,空 ns 必须保留。

- **T11(收敛 INV-RESOLVE ③)** discovery 档 `(∅, name)` 命中后追加检查:**若描述符声明了非空 `namespace` → 拒绝该候选**,并把它的真实 FQN 记入 did-you-mean 素材。
  **实现放在 `selectDependencyCandidate` 内,不动 `xpkg_lua_identity_matches`** —— 该闸门的 discovery 分支还有第二个消费者(`mcpp new --template <name>`,`src/scaffold/create.cppm:70-71`),模板发现场景保留全域按名语义是合理的。收紧必须 scoped 到依赖解析这一个调用点。

- **⚠️ 破坏性变更**:今天裸写 `tensorvia-cpu = "0.1.1"` **能装上**(2026-06-26 文档原文:"the **bare** form `tensorvia-cpu` resolves, installs as `aimol-x-tensorvia-cpu`, and builds")。T11 之后它将被拒绝,必须改写为 `aimol.tensorvia-cpu` 或 `[dependencies.aimol] tensorvia-cpu`。此变更须进 CHANGELOG 的 breaking 段,并由 §5.3 的 did-you-mean 给出**逐字可抄的迁移提示**。

### 5.3 诊断专用扫描 — did-you-mean(T12)

`selectDependencyCandidate` 全候选落空时(且**仅**在此时),对已配置的索引跑一次 `pkgs/*/*.lua` 扫描,读每个描述符的声明 `(namespace, name)`,收集短名等于请求短名的条目,产出:

```
error: dependency 'asio': no package found in the default namespace search path
  tried:
    (mcpplibs, asio)  ->  pkgs/a/asio.lua, pkgs/m/mcpplibs.asio.lua, pkgs/c/compat.asio.lua
    (∅,        asio)  ->  pkgs/a/asio.lua, pkgs/c/compat.asio.lua
  a package with this name exists under another namespace:
    chriskohlhoff.asio   (pkgs/c/chriskohlhoff.asio.lua)
  bare names only resolve to the `mcpplibs` / `compat` namespaces. write it out:
    [dependencies]
    "chriskohlhoff.asio" = "<version>"
  or:
    [dependencies.chriskohlhoff]
    asio = "<version>"
```

**约束(必须写进实现注释,否则会重新长成 IdentityIndex)**:
1. 扫描**只在失败路径**触发 —— 正常解析零开销;
2. 扫描结果**只进错误文案**,绝不回灌 `spec.namespace_` / lockfile / 安装层;
3. 扫描结果为空时降级为通用提示,不得因扫描失败而改变退出码。

这条约束是 §4.2 决策能够成立的前提:全域信息只用于**告诉用户怎么写**,不用于**替用户决定装什么**。

### 5.4 用户文档(T13)

`docs/05-mcpp-toml.md` §2.5 与 `docs/zh/05-mcpp-toml.md` §2.5 新增"命名空间解析规则"小节,内容见 §8。

---

## 6. 验证

**单测**(`tests/unit/test_manifest.cpp`,续 `XpkgIdentity` 组):

| 用例 | 断言 |
|---|---|
| `SplitFormNameIsAViolation` | `("chriskohlhoff", "asio")` → 有违规,消息含期望名 |
| `FqnFormIsClean` | `("chriskohlhoff", "chriskohlhoff.asio")` → nullopt |
| `NoNamespaceIsClean` | `("", "opencv")` → nullopt(xim 上游裸包) |
| `NoNameIsClean` | `("compat", "")` → nullopt |
| **`CompatAliasIsClean`** | `("compat", "compat.gtest")` → nullopt(**回归锁**:防止 §5.1 收窄退化成通用比较,打断裸 `gtest`) |
| `NestedNamespaceIsClean` | `("mcpplibs.capi", "mcpplibs.capi.lua")` → nullopt |

**依赖侧单测**(新增 `test_dependency_resolution.cpp` 或并入既有):

- 裸名解析到 `mcpplibs` 包 → 成功;裸名解析到 `compat` 包(`gtest`)→ 成功;裸名解析到无 ns 上游包(`opencv`)→ 成功且 `spec.namespace_` 保持空。
- 裸名请求一个声明了第三方 ns 的包 → **失败**,错误含该包 FQN 与两种正确写法。
- 点式 `chriskohlhoff.asio` / 子表 `[dependencies.chriskohlhoff] asio` → 解析到 `(chriskohlhoff, asio)`。
- **P3 锁**:discovery 档命中一个声明了 ns 的描述符时,`spec.namespace_` 绝不为空(该场景在 T11 后应已被拒绝,此测锁住"即便将来放宽也不得漏 ns")。

**e2e**(2 例):
1. 构造 split 形式描述符的本地 path index + 消费工程 → `mcpp build` **秒级**失败,stderr 含期望名与 `#278`;改为 FQN 后同一命令通过。(锁 T1 + T2)
2. 索引含 `thirdparty.foo`,消费端裸写 `foo` → 失败且提示 `"thirdparty.foo"` 与子表两种写法;改写后通过。(锁 T9 + T11 + T12)

**手工验收**:`mcpp xpkg parse` 对现网描述符全量跑一遍。这是"不误伤"的最强证据,须在 PR 描述中贴出。

---

### 6.1 实测结果(0.0.105 实现后)

| 项 | 结果 |
|---|---|
| 单测 `XpkgNameForm` | **8/8 过**(设计写 6 例,实现补了 `PrefixWithoutShortSegmentIsAViolation` 与 `FromLuaReadsBothFields`) |
| 单测总体 | **37/37 测试二进制全绿**,含 `test_manifest` 119 例 |
| e2e 161 `xpkg_name_form` | 过 —— lint 拒绝 + 修法字面量 + `--json` + `--allow-split-name` + 运行期 fail-fast(**断言"未发生 Downloading"**) |
| e2e 162 `bare_name_namespace_scope` | 过 —— 裸名硬失败 + did-you-mean + 两种正确写法均解析 + compat 别名回归锁 |
| **mcpplibs 索引全量** | **49/49 parse OK,0 违规**(上游已先修好两例,故非设计预期的 2) |
| 本地 e2e 全套 | **165 过 / 1 败**,唯一失败是 `22_doctor_cache_publish.sh`,根因本机 `~/.xlings/subos/current/bin/g++` shim 自引用损坏(环境性,属既有基线 13/22/26/54/62) |
| 活体验证 | 裸 `gtest` 正常构建;`"chriskohlhoff.asio"` 真实下载 + 编译通过 |

**实现期新增的两项修正**(设计未预见):

1. **`--allow-split-name`**(§2.1b):全 registry 回归发现 30 个 split,28 个是 xlings 原生索引的合法用法。
2. **lazy git 索引不参与硬失败**:自定义 **git** 索引由 xlings 在安装期惰性 clone,选择期其描述符可能尚未落盘,"找不到"不是结论性的。故 `selectDependencyCandidate` 在候选命名空间路由到非 builtin、非 local 索引时保留历史 fall-through;builtin 与 local path 索引在选择期均可读,继续走严格规则。e2e 42/43/44 覆盖。

---

## 7. 风险与后续

| 项 | 判断 |
|---|---|
| 误伤现有可用路径(索引侧) | 由 §5.1 收窄 + `CompatAliasIsClean` 回归锁覆盖;62 例全量扫描给出期望结果 |
| **裸名第三方包不再解析** | **已知破坏性变更**(§5.2)。现网受影响者仅 `tensorvia-cpu`(T8 一并改索引)。CHANGELOG breaking 段 + did-you-mean 逐字迁移提示 |
| 第三方索引被新 lint 挡住 | 只在描述符声明了 namespace 时生效,且这类描述符今天本就装不上 —— 从"静默不可用"变为"明示不可用",非回归 |
| 已安装旧快照被遮蔽 | 已知并接受(§5.1 T2),由 T2b 的 warn 使其可见 |
| did-you-mean 扫描长成解析路径 | 由 §5.3 三条约束 + 实现注释锁住;e2e 2 不断言任何"扫描后自动解析成功"的行为 |
| legacy-bare 默认命名空间支路(§2.3) | **不在本批次**。现网零实例。后续:索引默认命名空间非空时发 warn,1.0.0 随 COMPAT 兜底清理 |
| §4.1 index-owned namespace 未实现 | 与 T10 的空 ns 语义耦合(§5.2)。仍是 §5 `PackageLocator` 的正式续作 |
| `mcpp search` 展示口径 | INV-NAME 下 FQN 唯一,无需改动 |

---

## 8. 用户文档增补(§2.5 落地内容)

在 `docs/05-mcpp-toml.md` §2.5 与中文版对应位置,于既有示例之后新增 **Namespace resolution rules / 命名空间解析规则**:

1. 每个包的身份是 `(namespace, name)` 二元组;索引描述符的 `package.name` 写**完全限定名**。
2. **裸名依赖只解析三类**:`mcpplibs`(默认)、`compat`(包装)、无命名空间的上游包。
3. **第三方命名空间必须写全** —— 点式 `"org.pkg" = "…"` 或子表 `[dependencies.org] pkg = "…"`。
4. 给出"为什么不做模糊全域匹配"的一句话理由(可复现性 / 新增索引不得改变既有解析结果)。
5. 给出错误示例与其修法(裸 `asio` → `"chriskohlhoff.asio"`)。
6. 给 xpkg 作者一句指引:`package.name` 必须以 `namespace.` 开头,由 `mcpp xpkg parse` 校验。

---

## 9. 任务清单

**索引侧(INV-NAME):**

- [ ] **T1** `xpkg_name_form_violation{,_from_lua}` 谓词 + 6 条单测(`src/manifest/xpkg.cppm`, `tests/unit/test_manifest.cpp`)
- [ ] **T2** `mcpp xpkg parse` 接入谓词,违规 exit 1,`--json` 同步 `error` 字段(`src/cli/cmd_xpkg.cppm`)
- [ ] **T3** `loadVersionDep` install 分支 fail-fast + 已装路径 warn(`src/build/prepare.cppm`)
- [ ] **T4** `emit_xpkg` 输出 `namespace` + FQN `name`,无 namespace 时提示;`--namespace` 开关(`src/pm/publisher.cppm`)

**依赖侧(INV-RESOLVE):**

- [ ] **T9** `selectDependencyCandidate` 全候选落空不再静默回退 `front()`,改为明确失败(`prepare.cppm:1516-1541`)
- [ ] **T10** discovery 档命中后用 `extract_xpkg_namespace(*lua)` 回填真实 ns;无 ns 上游包保持空 ns
- [ ] **T11** discovery 档拒绝声明了非空 `namespace` 的描述符;**scoped 到依赖解析调用点,不动 `xpkg_lua_identity_matches`**(保 `mcpp new --template`)
- [ ] **T12** did-you-mean:仅失败路径触发的全索引扫描,三条约束写进实现注释
- [ ] **T11b** 修订 `2026-06-26 §4.4/§4.6(a)` 对 discovery 档的表述;更新 `prepare.cppm:1456-1459` 注释,消除 "never by filename" 的误读

**验证与文档:**

- [ ] **T5** e2e 1:split 描述符 → 秒级自解释失败;改 FQN → 通过
- [ ] **T5b** e2e 2:裸名请求第三方 ns 包 → 失败 + did-you-mean;改写后通过
- [ ] **T6** 全索引 62 例 `mcpp xpkg parse` 回归,PR 描述贴结果(期望 2 报错)
- [ ] **T13** 用户文档 §2.5 增补(`docs/05-mcpp-toml.md` + `docs/zh/05-mcpp-toml.md`),内容见 §8
- [ ] **T14** CHANGELOG breaking 段:裸名不再解析第三方命名空间包

**合入与生态:**

- [ ] **T7** 单 PR(`Fixes #278`)→ CI 全绿 → squash 合入
- [ ] **T8** mcpp-index:`chriskohlhoff.asio` + `tensorvia-cpu` 改 FQN;lint job 改调 `mcpp xpkg parse`
