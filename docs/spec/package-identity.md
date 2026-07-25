# SPEC-001:包身份、依赖选择器与匹配机制

| 项 | 值 |
|---|---|
| **规范编号** | SPEC-001 |
| **标题** | 包身份(`package.namespace` / `package.name`)、`[dependencies]` 选择器与匹配机制 |
| **状态** | **评审中(Review)** —— 已实现 |
| **版本** | 1.0 |
| **最后修改** | 2026-07-25 |
| **对应实现** | mcpp **0.0.106**(xlings >= 0.4.69) |
| **作者/维护** | mcpp-community |
| **相关设计文档** | `.agents/docs/2026-06-20-package-resolution-architecture.md` §4<br>`.agents/docs/2026-06-26-identity-first-resolution-no-filename.md`<br>`.agents/docs/2026-07-25-issue278-descriptor-name-form-canonicalization-design.md`<br>`.agents/docs/2026-07-25-name-namespace-bidirectional-verification-report.md`<br>`.agents/docs/2026-07-25-name-namespace-canonical-implementation-spec.md` |
| **相关 issue** | [mcpp#278](https://github.com/mcpp-community/mcpp/issues/278)<br>[xlings#381](https://github.com/openxlings/xlings/issues/381) —— 索引键缺命名空间维度(§3.3) |

## 规范用语

本文中的 **必须 / 禁止 / 应当 / 可以** 按 RFC 2119 的含义使用:

- **必须(MUST)/ 禁止(MUST NOT)**:强制要求,违反即为错误,由 lint 或运行期拦截。
- **应当(SHOULD)**:强烈建议;偏离需有充分理由。
- **可以(MAY)**:可选。

## 实现状态标记

本文同时描述**目标规范**与**当前实现**。每条规则标注状态:

| 标记 | 含义 |
|---|---|
| ✅ **已实现** | 0.0.106 的行为与本规范一致 |
| ⚠️ **部分实现** | 已有实现,但语义或覆盖面与本规范有差异(差异已注明) |
| ❌ **未实现** | 本规范要求但尚未支持;当前行为已注明 |

> **本规范已在 mcpp 0.0.106 全部实现。** 索引作者按 §3 书写即可。0.0.105 及更早版本要求的过渡形态(`name` 必须写成 `<namespace>.<name>`)仍被接受为**兼容写法**,见 §8。

---

## 1. 范围与边界

### 1.1 本规范覆盖

- 索引描述符(`.lua`)中 `package.namespace` / `package.name` 的语义与形态
- `mcpp.toml` 中 `[dependencies]`(及 `[dev-dependencies]` / `[build-dependencies]` / `[feature-deps.*]`)的书写文法
- 从用户书写 → 候选身份 → 描述符发现 → 身份校验 → 安装目标的完整匹配机制

### 1.2 边界:mcpp 不改动 xlings 规范

**mcpp 基于 xlings 已定的机制实现,不重新定义它们。** 下列属于 xlings 侧既定事实:

| xlings 既定机制 | 内容 |
|---|---|
| **寻址模型** | `<namespace>:<name>` —— 冒号前比对包声明的 `package.namespace`(**不是**索引名) |
| 索引作用域 | **每个索引一张表**;不同索引之间不共享键空间 |
| xpkg 安装目录 | `{package.namespace}-x-{package.name}` |

> ≤0.4.68 的索引**存储**实现只按裸 `name` 建键、`namespace` 退化为命中后过滤,与上面的寻址模型有落差。该缺口已由 [xlings#381](https://github.com/openxlings/xlings/issues/381) 在 **0.4.69** 修复,索引改按 `(effectiveNamespace, name)` 建键。**mcpp 0.0.106 起要求 xlings >= 0.4.69。**

mcpp 的正确做法是**把描述符里读到的字面值原样使用**,而不是在自己这边重新推导一遍。本规范的多数条款是这句话的展开。

---

## 2. 数据模型:身份是二元组

> **一个包的身份是 `(namespace, name)` 二元组。文件名、安装目录名、用户的书写方式都不是身份,而是身份的派生或输入。**

| 字段 | 角色 | 形态 |
|---|---|---|
| `package.namespace` | 命名空间,**点分层级路径** | 可为空;`compat` / `mcpplibs` / `mcpplibs.capi` / `a.b.c` |
| `package.name` | 包名,**单一原子段** | `zlib` / `asio` / `lua` |

**层级一律属于 `namespace`。** `(mcpplibs.capi, lua)` 是规范形态。

✅ **已实现**:身份二元组模型贯穿 mcpp 全链路(`XpkgIdentity`、`canonical_xpkg_identity`、`xpkg_lua_identity_matches`)。

---

## 3. 描述符侧规范

### 3.1 `package.namespace`

- **可以**为空。空表示该包属于**默认命名空间的公共包**(如 `imgui` / `ffmpeg` / `opencv`),其裸 `name` 即为完整身份。
- 非空时,**必须**是点分路径,每段由字母、数字、`-`、`_` 组成。
- **可以**多级(`mcpplibs.capi`),多级用于表达真实的层级归属。

✅ **已实现**。

### 3.2 `package.name`

> **规范:`name` 必须是单一原子段,禁止包含 `.`。任何层级都必须放进 `namespace`。**

```lua
-- ✅ 规范形态
package = { namespace = "chriskohlhoff", name = "asio" }
package = { namespace = "mcpplibs.capi", name = "lua"  }
package = { namespace = "",              name = "imgui" }

-- ❌ 层级留在 name 里
package = { namespace = "mcpplibs", name = "capi.lua" }
```

✅ **已实现**(0.0.106)。`mcpp xpkg parse` 与安装路径共用同一谓词校验。

**兼容写法**:0.0.105 及更早版本要求的完全限定拼写仍被接受 ——

```lua
package = { namespace = "compat", name = "compat.zlib" }   -- legacy,短名 = "zlib"
```

命名空间前缀会被剥离后再判定,只有**剩余部分**必须是原子段。现网全部已发布描述符都是这个形态,因此**不会**因本规范而失效。

### 3.3 `(namespace, name)` 唯一性

> **同一个索引内,`(namespace, name)` 必须唯一。**

身份是二元组(§2),唯一性因此也按二元组要求 —— `(alpha, asio)` 与 `(beta, asio)` 是两个不同的包,**可以共存于同一个索引**。

这与 xlings 的寻址模型一致:目标写作 `<namespace>:<name>`,冒号前比对包声明的 `package.namespace`,显式写出命名空间即可消歧。

✅ **规范如此**。

✅ **已实现**。xlings **0.4.69** 起索引按 `(effectiveNamespace, name)` 建键([#381](https://github.com/openxlings/xlings/issues/381)),同一索引内两个同短名不同命名空间的包各自可寻址;裸名请求多候选时报 ambiguity 并列出候选。

e2e `163_identity_first_resolution.sh` 端到端锁住:同一 path 索引内 `(alpha, widget)` 与 `(beta, widget)` 各自安装到 `alpha-x-widget` / `beta-x-widget`。

> ≤0.4.68 的环境下该场景不可用(其中一个包不可达)。mcpp 0.0.106 因此要求 xlings >= 0.4.69。

### 3.4 描述符文件名

> **文件名不参与任何解析,可以任意。应当使用 `<name>.lua` 或 `<namespace>.<name>.lua`。**

身份来自文件**内容**声明的 `package.{namespace,name}`,与文件叫什么无关。

✅ **已实现**(0.0.106)。两个半边都已兑现:
- **命中后的校验**:任何候选都必须通过 `xpkg_lua_identity_matches` 复核声明身份(防止 `compat.zlib` 请求被外来的裸 `zlib.lua` 满足)。
- **发现**:推荐文件名作为**快路径**先探测;全部落空时回落到按声明身份扫描 `pkgs/**/*.lua`。因此叫任何名字、放在任何字母目录下的描述符都能被找到,而符合推荐命名的索引**不付出任何扫描开销**。

e2e `163_identity_first_resolution.sh` 锁住:身份为 `(acme, widget)` 的描述符命名为 `pkgs/z/totally-unrelated-name.lua` 仍可解析安装。

推荐文件名(快路径探测顺序,非规范):

| 请求身份 | 探测的文件名(按序) |
|---|---|
| `(mcpplibs, cmdline)` | `cmdline.lua`, `mcpplibs.cmdline.lua`, `compat.cmdline.lua` |
| `(compat, zlib)` | `compat.zlib.lua`, `zlib.lua` |
| `(∅, imgui)` | `imgui.lua`, `compat.imgui.lua` |

---

## 4. 消费侧规范:`[dependencies]` 书写文法

用户写的**不是身份,而是选择器(selector)** —— 它展开为一个**有序候选身份列表**,按序尝试,首个满足者胜出。

### 4.1 四种书写形式

| # | 写法 | 展开为候选(按序) | 语义 |
|---|---|---|---|
| 1 | `[dependencies]`<br>`gtest = "1.15.2"` | ① `(mcpplibs, gtest)`<br>② `(∅, gtest)` | **裸名**。只解析默认命名空间搜索路径(§5.2) |
| 2 | `[dependencies]`<br>`acme.widget = "1.0"` | ① `(mcpplibs.acme, widget)`<br>② `(acme, widget)` | **点式选择器**。先试默认命名空间下的同名子空间,再试同级 peer root |
| 3 | `[dependencies.acme]`<br>`widget = "1.0"` | ① `(acme, widget)` | **命名空间子表**。权威、单候选、无猜测 —— **推荐用于第三方命名空间** |
| 4 | `[dependencies]`<br>`"acme.widget" = "1.0"` | ① `(acme, widget)` | **引号点式键**(legacy)。单候选,等价于 #3 |

✅ **已实现**(`resolve_dependency_selector` / `make_direct_dependency_selector`)。

**#2 与 #3 的区别很重要**:`[dependencies.acme]` 是**显式 TOML 表**,mcpp 视其为命名空间根,直接产出单候选;而 `[dependencies]` 里的点式键是**有序猜测**,会先试 `mcpplibs.` 前缀。

判定「是否命名空间根」的条件:该路径是 TOML **显式表**(`[dependencies.acme]` 被真实书写过),**或**键名恰为默认命名空间 `mcpplibs`。

多级命名空间同理,`ns` 逐层累积:`[dependencies.mcpplibs.capi]` + `lua = "0.0.3"` → 单候选 `(mcpplibs.capi, lua)`。

### 4.2 裸名的解析域是封闭的

> **裸名(#1)只解析三类包:`mcpplibs`(默认命名空间)、`compat`(包装命名空间)、无 `namespace` 声明的上游包。任何声明了第三方命名空间的包,禁止用裸名请求。**

✅ **已实现**(0.0.105)。裸名请求命中一个声明了非空第三方 `namespace` 的描述符时,该候选被拒绝。

**设计理由**:全域按短名搜索会让解析结果取决于「本机装了哪些索引」——
1. 两个命名空间拥有同名包时,胜负由索引顺序决定,而用户 `[indices]` 添加的索引之间**没有全序**;
2. **新增一个索引可能悄悄改变某个既有依赖解析到的包**(供应链隐患);
3. 同一份 `mcpp.toml` 在不同机器上可能解析到不同的包。

依赖解析的**可复现性**优先于书写便捷性。

### 4.3 解析失败时的诊断

候选全部落空时,mcpp **必须**明确失败,并列出尝试过的身份;若该短名存在于其他命名空间,**应当**给出可直接抄写的正确写法。

✅ **已实现**(0.0.105):

```
error: dependency 'asio': no package found under the namespaces mcpp searched
  tried: (mcpplibs, asio), (no namespace, asio)
  a package with this name exists under another namespace:
    chriskohlhoff.asio
  bare names only resolve to the `mcpplibs` / `compat` namespaces. write it out:
    [dependencies]
    "chriskohlhoff.asio" = "1.38.1"
  or:
    [dependencies.chriskohlhoff]
    asio = "1.38.1"
```

该 did-you-mean 扫描**仅**在已失败路径触发,结果**只进错误文案**,禁止回灌解析、lockfile 或安装层 —— 否则就退化成 §4.2 否决的全域模糊匹配。

---

## 5. 匹配机制

### 5.1 完整流程

```
用户书写(selector)
   │  §4.1 展开
   ▼
有序候选身份列表  [(ns₁,n₁), (ns₂,n₂), …]
   │  逐个尝试
   ▼
① 发现:在候选所属索引里定位描述符文件        ← 当前受文件名约束(§3.4)
② 校验:xpkg_lua_identity_matches 复核声明身份  ← §5.2
③ 收敛:INV-RESOLVE 拒绝裸名命中第三方 ns      ← §4.2
④ 回填:用描述符**声明的** namespace 定身份     ← §5.3
   │  首个通过者胜出;全部落空 → §4.3 报错
   ▼
选定身份 (ns, name) → 派生 wire key / store dir(§6)
```

### 5.2 身份校验规则

给定候选 `(ns, shortName)` 与一个描述符,`xpkg_lua_identity_matches` 的判定:

| 候选 `ns` | 判定 |
|---|---|
| 描述符无 `name` | **接受**(无从校验,宽松) |
| `ns` 为空(discovery) | 短名相等即可 |
| `ns == "mcpplibs"`(默认命名空间) | 描述符身份的 ns ∈ **{`mcpplibs`, `compat`}**,或(旧式)无 ns |
| 其他具体 ns | **精确相等** |

✅ **已实现**。第三行即**默认命名空间搜索路径**:裸 `gtest` 能命中 `compat.gtest`,正是这条。

### 5.3 空命名空间的回填(P3)

候选 `(∅, name)` 命中后,**必须**用描述符**声明的** `namespace` 作为最终身份,而不是候选的空值。

若描述符本身未声明 `namespace`(上游裸包如 `opencv`),则**空命名空间就是它的合法身份**,不得强行填充。

✅ **已实现**(0.0.105)。此前空命名空间会流入 lockfile 与安装层。

---

## 6. 派生量

三个派生量**全部**由描述符的字面 `name` 与 `namespace` 决定:

| 派生量 | 规范公式 | 状态 |
|---|---|---|
| xlings 索引键 | `<字面 name>` | ✅ |
| xpkg 安装目录 | `{namespace}-x-{字面 name}` | ✅ |
| 安装 target | `<namespace>:<字面 name>@<版本>` | ✅ |

**注意 target 的冒号前缀是包的命名空间**(xlings 的 *effective namespace*),不是索引名 —— 这正是同一索引内两个同短名包得以各自寻址的原因。无命名空间的上游包用裸字面名寻址,无前缀。

**关键性质**:三者只依赖字面值,**与 `name` 写成短名还是 legacy FQN 无关**。因此实现无需按形态分支,新旧描述符可长期混存。

实测佐证:`{namespace}-x-{字面 name}` 对现网全部 48 个描述符成立 —— `compat` + `compat.zlib` → `compat-x-compat.zlib`;`acme` + `asio` → `acme-x-asio`。两者都是正确的 xlings 行为。

---

## 7. 实现记录(0.0.106)

| # | 项 | 规范条款 | 落地方式 |
|---|---|---|---|
| 7.1 | 身份归一化去掉 split-on-last-dot | §2 | `canonical_xpkg_identity` 直接取声明的两个字段,仅为 legacy FQN 剥前缀。不再从 `name` 反推描述符未声明的命名空间 |
| 7.2 | 安装 target 用字面 `name` | §3.2 §6 | `prepare.cppm` 用 `extract_xpkg_name(luaContent)`(描述符已在作用域,零额外 I/O);读不到时回落旧渲染 |
| 7.3 | store 目录 | §6 | `{ns}-x-{短名}` 与 `{ns}-x-{legacy FQN}` 并列为一等候选,其余降为过渡兼容 |
| 7.4 | 文件名自由 | §3.4 | 推荐文件名为快路径;落空后按声明身份扫描 `pkgs/**/*.lua`。符合推荐命名者零扫描开销 |
| 7.5 | `name` 形态校验语义反转 | §3.2 | `xpkg_name_form_violation` 改判「短名必须原子」;legacy FQN 前缀先剥离后再判 |

**核心变化是三处「减法」**:不再从 `name` 反推命名空间、不再重新渲染 wire key、不再把规范 store 目录埋在猜测候选里。三处都是「值已在手却重新推导一遍」—— 与 #278 的根因同型。

---

## 8. 迁移

### 8.1 兼容矩阵

| | FQN 形态描述符(现网) | 短名形态描述符 |
|---|---|---|
| mcpp ≤ 0.0.105 | ✅ | ❌ `E_NOT_FOUND` |
| 实现 §7.2 之后 | ✅ | ✅ |

**只有一个破损格。** 因此:

1. mcpp 0.0.106 已发布,对两种形态都工作,**描述符可逐包迁移** —— 不是一次性切换。
2. 某个索引开始使用短名形态时,其 `index.toml` 的 `min_mcpp` **必须**同步抬到 **0.0.106**。这是硬性的:更低版本的客户端会静默 `E_NOT_FOUND`,无技术手段绕过。
3. 同一索引内**可以**长期混存两种形态。

### 8.2 索引作者应当怎么写

```lua
package = {
    namespace = "chriskohlhoff",
    name      = "asio",                 -- ✅ 规范形态
}
```

已发布的 legacy 形态(`name = "chriskohlhoff.asio"`)**无需改动即可继续工作**;迁移到短名形态时,把该索引的 `min_mcpp` 抬到 0.0.106。

---

## 9. 端到端示例

### 9.1 第三方命名空间包

**描述符**(`pkgs/c/chriskohlhoff.asio.lua`,文件名仅为推荐):

```lua
package = {
    spec      = "1",
    namespace = "chriskohlhoff",
    name      = "chriskohlhoff.asio",   -- 过渡形态;规范形态为 "asio"
    xpm = { linux = { ["1.38.1"] = { url = "…", sha256 = "…" } } },
}
```

**消费**:

```toml
[dependencies.chriskohlhoff]
asio = "1.38.1"
```

**匹配过程**:

```
选择器 [dependencies.chriskohlhoff] + asio
  → 显式命名空间表 → 单候选 (chriskohlhoff, asio)
  → 发现:探测 pkgs/c/chriskohlhoff.asio.lua           ✓
  → 校验:声明 (chriskohlhoff, chriskohlhoff.asio)
          归一化 → (chriskohlhoff, asio) == 候选        ✓
  → 身份 (chriskohlhoff, asio)
  → wire key   chriskohlhoff.asio       (§7.2 后:asio)
  → target     chriskohlhoff:chriskohlhoff.asio@1.38.1
  → store dir  chriskohlhoff-x-chriskohlhoff.asio  (§7.2 后:chriskohlhoff-x-asio)
```

**等价写法**:`[dependencies]` + `"chriskohlhoff.asio" = "1.38.1"`。

**错误写法**:`[dependencies]` + `asio = "1.38.1"` —— 裸名不解析第三方命名空间(§4.2),报错并给出上述两种正确写法。

### 9.2 裸名经 compat 搜索路径

```toml
[dependencies]
gtest = "1.15.2"
```

```
选择器 gtest → 候选 ① (mcpplibs, gtest)  ② (∅, gtest)
候选①:探测 gtest.lua / mcpplibs.gtest.lua / compat.gtest.lua
       命中 compat.gtest.lua,声明 (compat, compat.gtest) → 归一 (compat, gtest)
       校验:候选 ns 为默认命名空间 → 允许 ns ∈ {mcpplibs, compat} → ✓
身份 (compat, gtest) → store dir compat-x-compat.gtest
```

### 9.3 多级命名空间

```lua
package = { namespace = "mcpplibs.capi", name = "mcpplibs.capi.lua" }  -- 规范形态:name = "lua"
```

```toml
[dependencies.mcpplibs.capi]
lua = "0.0.3"
```

身份 `(mcpplibs.capi, lua)` —— 层级在 `namespace`,`name` 是原子段 `lua`。

---

## 10. 变更记录

| 版本 | 日期 | 变更 |
|---|---|---|
| 0.1 | 2026-07-25 | 首版草案。整合 #278 的双向验证结论:确立「身份 = `(namespace, name)`、层级归 `namespace`、`name` 为原子段」为规范形态,并如实标注 0.0.105 的过渡形态(强制 FQN)与全部待实现项 |
| 1.0 | 2026-07-25 | **mcpp 0.0.106 全部实现**:身份归一化去 split-on-last-dot、target 用字面 `name`、store 目录、文件名自由(快路径+身份扫描)、`name` 形态校验反转。xlings 0.4.69 修好 #381 后 §3.3 的 `(namespace, name)` 唯一自然成立。状态 草案 → 评审中 |
| 0.6 | 2026-07-25 | §3.3 改按 **`(namespace, name)` 唯一**表述(与身份数据模型、xlings 寻址模型一致);单仓同名冲突重新定位为 xlings ≤0.4.68 的**实现缺口**(xlings#381 修复中),不再作为规范约束或索引侧 lint 要求 |
| 0.5 | 2026-07-25 | §3.3 补「跨索引仓同名正常工作」的对照:约束**仅**存在于单个索引仓内(每仓一张独立表),跨仓由聚合产生多候选、显式 ns 即可消歧。同步补进 xlings#381 |
| 0.4 | 2026-07-25 | §3.3 关联上游 issue [xlings#381](https://github.com/openxlings/xlings/issues/381)(已提,含纯 xlings 最小复现) |
| 0.3 | 2026-07-25 | **修正 §1.2 与 §3.3 的定性**:此前把「索引以裸 `name` 为键」表述成 xlings 的既定设计,不准确。xlings 的**寻址模型本就是 `ns:name`**(`catalog.cppm build_match_` 用 `pkg->namespace_` 比对冒号前缀);问题在**存储实现**只按 `name` 建键、命名空间退化为命中后的过滤器。据此把该约束重新定性为 **xlings 的实现缺口(应提上游 issue)**,而非设计约束;修好后约束自动消失 |
| 0.2 | 2026-07-25 | §3.3 补充实证与成因说明:澄清 `(namespace, name)` 唯一才是数据模型的要求,`name` 唯一是 xlings 索引键空间缺少命名空间维度所致的**外部约束**;补同索引同名包的实测(一个装上、另一个 `E_NOT_FOUND`)、与 `name` 形态的关系(FQN 形态顺带规避、短名形态暴露)、以及短期 lint / 长期上游需求的承接方式 |

---

> **本规范已在 mcpp 0.0.106 全部实现**,状态为「评审中」。
> 英文版待补(`docs/spec/` 顶层按仓库惯例为英文,本文档先以中文成稿)。
