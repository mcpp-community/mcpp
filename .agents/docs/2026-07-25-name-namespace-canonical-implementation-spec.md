# mcpp `name` / `namespace` 规范实现(定稿)

> 依据:`2026-07-25-name-namespace-bidirectional-verification-report.md` 的双向验证 + 评审给定的规格
> 基线:mcpp `main` @ 0.0.105
> 状态:**待实现**。本文定的是目标实现,不是现状描述。

---

## 0. 边界约束(先于一切)

**mcpp 不改动 xlings 的任何规范,只基于 xlings 已定的机制去实现。**

xlings 侧属于**既定事实**,mcpp 只能遵守,不能重新定义:

| xlings 既定机制 | 内容 |
|---|---|
| 索引键 | `entries[package.name]`,以**字面 `name`** 为键,精确匹配 |
| xpkg 目录 | `{namespace}-x-{name}` —— `name` 是什么就拼什么 |

**由此推出 mcpp 唯一正确的做法:把描述符里读到的字面值原样使用,不要在 mcpp 侧重新推导一遍。** 本文全部改动都是这一句话的展开。

> **勘误**:先前曾把「FQN 形式产出 `acme-x-acme.asio`」当作"不符合 `{ns}-x-{name}` 规范"、并当成短名为正解的佐证 —— **该推论是错的**。`{ns}-x-{name}` 对两种形式都成立(`name="acme.asio"` → `acme-x-acme.asio`;`name="asio"` → `acme-x-asio`),它是确定的 xlings 机制,不偏袒任何一种写法。短名为规范形态的依据是数据模型本身(层级归 `namespace`),不是目录名。

---

## 1. 数据模型

包的身份是二元组,**两个字段都直接取自描述符声明,不做任何重新渲染**:

| 字段 | 含义 | 形态 |
|---|---|---|
| `package.namespace` | 命名空间,**点分层级路径** | 可空;`compat` / `mcpplibs.capi` / `a.b.c` |
| `package.name` | 包名,**单一原子段** | **不含点**;`zlib` / `asio` / `lua` |

> **层级一律放 `namespace`,`name` 只留最后一段。**
> `(mcpplibs.capi, lua)` 是规范形态;`namespace="mcpplibs", name="capi.lua"` 不是。

### 1.1 三个派生量,全部源自**字面 `name`**

| 派生量 | 公式 | 实测依据 |
|---|---|---|
| xlings 索引 key | `<字面 name>` | libxpkg `entries[pkg.name]`,精确匹配 |
| store 目录 | `{namespace}-x-{字面 name}` | 全量实测 48/48 相符(FQN 与短名两种形式) |
| 安装 target | `<indexName>:<字面 name>@<version>` | 实验证实短名/FQN 均可解析 |

**关键性质:三者都只依赖字面 `name`,与「短名形式 / 遗留 FQN 形式」无关。** 因此实现里**不需要按形式分支** —— 这是本方案能同时兼容新旧描述符的根本原因。

### 1.2 文件名不参与任何解析

描述符文件名**自由**。推荐(非强制)`<name>.lua` 或 `<namespace>.<name>.lua`。

这是 `2026-06-26 identity-first` 的原意("Filename Is Not a Key")的完整落实 —— 此前只兑现了「命中后按声明身份复核」,**发现仍受候选文件名约束**,本方案一并补齐(§2.5)。

---

## 2. mcpp 侧实现

### 2.1 身份归一化 —— 去掉 split-on-last-dot

```cpp
// 现状(xpkg.cppm canonical_xpkg_identity):把 name 拼成 FQN 再按最后一个点切开
fqn = name.starts_with(ns + ".") ? name : ns + "." + name;
auto pos = fqn.rfind('.');
return { fqn.substr(0, pos), fqn.substr(pos + 1) };     // ← 会把 ns 从 name 里"猜"出来

// 目标:namespace 就是 namespace,name 就是 name;只为遗留 FQN 形式剥前缀
std::string short_ = name.starts_with(ns + ".") ? name.substr(ns.size() + 1) : name;
return XpkgIdentity{ ns, short_ };
```

差别在于**不再从 `name` 反推命名空间**。`namespace="a", name="a.b.c"` 这类自相矛盾的写法,现状会静默解析成 `(a.b, c)`(一个描述符从未声明的命名空间),新实现直接由 §2.4 的校验拒绝。

### 2.2 安装 target —— 用字面 `name`,不再重新渲染

```cpp
// prepare.cppm:1861 现状 —— 丢弃已读到的字面值,重新渲染
auto fqname = ns.empty() ? shortName : std::format("{}.{}", ns, shortName);

// 目标:字面 name 就是 wire key(luaContent 已在同一作用域)
auto wireName = luaContent ? mcpp::manifest::extract_xpkg_name(*luaContent) : std::string{};
if (wireName.empty()) wireName = ns.empty() ? shortName          // 兜底:读不到描述符时
                                            : std::format("{}.{}", ns, shortName);
```

**已实验验证**(报告 §2.1):改此一行后短名形式立即可装(`acme-x-asio`),且现网 FQN 形式继续可装(`acme-x-acme.asio`),mcpp 自身依赖无回归。

### 2.3 store 目录 —— 收敛为单一公式

```cpp
// compat.cppm install_dir_candidates 现状:6 个猜测候选,{ns}-x-{short} 被标为 COMPAT 兜底
// 目标:{namespace}-x-{字面 name} 单一公式(全量实测 48/48 命中),其余降为过渡期兼容项
```

### 2.4 `name` 形态校验 —— INV-NAME 语义反转

| | 现状(0.0.105) | 目标 |
|---|---|---|
| 规则 | `namespace` 非空时 `name` **必须**是 `<ns>.<short>` | `name` **不得含点**;层级归 `namespace` |
| 违规示例 | `ns="chriskohlhoff", name="asio"` | `ns="mcpplibs", name="capi.lua"` |
| 落点 | `xpkg_name_form_violation` + `mcpp xpkg parse` + 安装路径 | 同左 |

过渡期:遗留 FQN 形式(`name` 恰以 `namespace + "."` 开头)**降级为 warning**,不阻断;其他含点写法直接报错。

### 2.5 描述符发现 —— IdentityIndex(文件名自由的必要前提)

文件名一旦自由,「候选文件名探测」就不再可靠:`read_xpkg_lua*` 三个入口目前只探测 `compat::xpkg_lua_candidates` 生成的固定名单,叫别的名字的描述符**根本看不见**。

因此必须落地 `2026-06-26 §5` 一直推迟的 `IdentityIndex`:扫描 `pkgs/*/*.lua`,读每个描述符声明的 `(namespace, name)` 建表,文件名仅作可选加速提示。

> **与 INV-RESOLVE 不冲突。** 被否决的是「裸名跨命名空间发现」(解析结果取决于装了哪些索引);IdentityIndex 是「按**已知的精确身份**查表」,确定性不受影响。消费端解析规则(裸名只解析 `mcpplibs` / `compat` / 无 ns 上游)**保持不变**。

---

## 3. 遗留约束:同索引内 `name` 必须唯一(索引侧承担)

xlings 的索引是**每索引一张扁平表、以字面 `name` 为键**(§0 的既定机制)。短名形式下,同一索引里 `compat`/`zlib` 与 `mcpplibs`/`zlib` 会落到同一个键 `zlib` 上而**互相覆盖** —— 现状靠 FQN 写法天然错开。

按 §0,这**不是 mcpp 能解决的问题**:改键空间等于改 xlings 规范,越界。因此约束由**索引侧承担**:

> **同一索引内,`package.name` 的字面值必须唯一。**

由 mcpp-index 的 lint 强制(遍历 `pkgs/*/*.lua`,`name` 字面值不得重复)。

**现网检查:mcpplibs 索引 48 个描述符,短名化后无冲突**(`compat.*` 与 `mcpplibs.*` 之间无同短名者),故该约束立即可满足,不需要为它改任何包名。

若将来某个索引确实需要容纳同短名的两个包,那是 xlings 键空间的能力缺口,应作为**上游需求**提给 xlings,而不是在 mcpp 侧绕开。

---

## 4. 兼容矩阵与迁移

| | 现网描述符(FQN) | 新描述符(短名) |
|---|---|---|
| 现有 mcpp ≤0.0.105 | ✅ | ❌ `E_NOT_FOUND` |
| 本方案实现后 | ✅ | ✅ |

**只有一个破损格**,含义:

1. **先发 mcpp**(对两种写法都工作),**再按节奏迁描述符** —— 不是一次性切换。
2. 描述符改短名时,该索引的 `index.toml` `min_mcpp` **必须**同步抬到含本实现的版本(硬性,无技术手段绕过)。
3. 迁移可**逐包**进行,索引内可长期混存两种形式。

---

## 5. 实施清单

**mcpp(一次发版):**

- [ ] `canonical_xpkg_identity` 去 split-on-last-dot(§2.1)
- [ ] `prepare.cppm` target 用字面 `name`(§2.2)
- [ ] `install_dir_candidates` 收敛为 `{ns}-x-{字面 name}`(§2.3)
- [ ] `xpkg_name_form_violation` 语义反转 + `mcpp xpkg parse` 同步(§2.4)
- [ ] `IdentityIndex` 落地,`read_xpkg_lua*` 改走它(§2.5)
- [ ] e2e 161 断言反转;新增「短名描述符可安装」「任意文件名可发现」正向 e2e
- [ ] `docs/05-mcpp-toml.md` §2.5 xpkg 作者段改写

**mcpp-index(mcpp 发布后,可逐包):**

- [ ] 新增 lint:同索引内 `name` 字面值唯一(§3)
- [ ] 描述符逐步短名化;`min_mcpp` 抬版
- [ ] 现有「文件名必须规范」的 lint **不要加**(文件名自由)

---

## 附:本方案相对现状的三处"减法"

值得强调的是,这不是加复杂度,而是**去掉三处多余的推导**:

1. 不再从 `name` 反推命名空间(split-on-last-dot)
2. 不再重新渲染 wire key(直接用字面值)
3. 不再猜 store 目录(6 个候选 → 1 个公式)

三处都是「mcpp 手里已有准确值、却选择重新推导一遍」—— 与 #278 的根因同型。
