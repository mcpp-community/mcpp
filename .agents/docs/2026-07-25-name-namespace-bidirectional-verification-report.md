# `package.name` / `package.namespace` 双向验证报告

> 触发:mcpp-index PR#117 的文档把「`name` 必须写成 FQN」写成了**设计规则**,评审指出这只是兼容写法,层级应当全在 `namespace`、`name` 只留短名。
> 目的:用当前源码 + 实机双向验证,判定哪一方成立、代价在哪、以及 PR#117 该怎么写。
> 基线:mcpp `main` @ `cee8130`(0.0.105),xlings 0.4.68 / libxpkg 当前 HEAD。

---

## 0. 结论先行

| 命题 | 判定 |
|---|---|
| 「层级属于 `namespace`,`name` 概念上是短名」 | ✅ **成立**,且与 mcpp §4.2 一致 |
| 「`name` 必须写 FQN」是 mcpp 的**设计规则** | ❌ **不成立** —— 这是我写错了 |
| 「`name` 必须写 FQN」是**当前实现的编码约束** | ✅ 成立,但根因是**一行可改的代码**,不是架构 |
| 我此前「根因在 libxpkg 的 key 空间」的判断 | ❌ **错误**,已实机证伪(见 §3) |
| 短名形式需要改 libxpkg / xlings | ❌ **不需要**,mcpp 侧一行即可 |

**修正后的根因**:`prepare.cppm:1861` 把已经读到手的字面 `name` **丢弃**,改用 `ns + "." + short` 重新渲染一遍去当 wire key。改成直接用字面 `name`,短名形式立即可用,且**现网 FQN 形式继续可用**。

---

## 1. 方向一:从源码看 mcpp 如何处理 name/namespace

### 1.1 读取 —— mcpp 完全自理,不依赖 xlings

| 环节 | 位置 | 是否依赖 xlings |
|---|---|---|
| 定位描述符文件 | `package_fetcher.cppm` `read_xpkg_lua*` | ❌ 自己扫盘 |
| 解析 `namespace` | `xpkg.cppm:670` `extract_xpkg_namespace` | ❌ 自己解析 |
| 解析 `name` | `xpkg.cppm:676` `extract_xpkg_name` | ❌ 自己解析 |
| 归一化为身份 | `xpkg.cppm` `canonical_xpkg_identity` | ❌ 自己归一 |
| 身份校验 | `xpkg_lua_identity_matches` | ❌ 自己校验 |

**评审意见在这一点上完全正确**:name/namespace 的处理是 mcpp 的自有机制,xlings 只被用于**取值/装包**。

### 1.2 身份归一化对两种写法**已经等价**

```cpp
// xpkg.cppm — canonical_xpkg_identity
std::string prefix = ns + ".";
fqn = name.starts_with(prefix) ? name : ns + "." + name;   // 两种写法在此合流
auto pos = fqn.rfind('.');
return XpkgIdentity{ fqn.substr(0, pos), fqn.substr(pos + 1) };
```

`(acme, asio)` 与 `(acme, acme.asio)` 归一化到**同一身份** `(acme, asio)`。身份层对写法**无偏好**。

### 1.3 唯一的偏好点:目标构造丢弃了字面值

```cpp
// prepare.cppm:1861 —— 唯一把写法变成"有对错"的一行
auto fqname = ns.empty() ? shortName : std::format("{}.{}", ns, shortName);
...
// :1906 / :1911
auto target = std::format("{}@{}", fqname, version);
target = std::format("{}:{}@{}", idxSpec->name, fqname, version);
```

而 `luaContent`(含字面 `name`)**就在同一作用域**——`:1781` 与 `:1865` 的 INV-NAME 检查用的正是它。**信息在手,只是没用。**

### 1.4 store 目录:mcpp 自己猜,且**两种形式都已覆盖**

```cpp
// compat.cppm install_dir_candidates
candidates.push_back(std::format("{}-x-{}", ns, fqname));      // <ns>-x-<ns>.<short>
...
candidates.push_back(std::format("{}-x-{}", ns, shortName));   // <ns>-x-<short>  ← 短名形式
```

短名形式的 store 目录**早已在候选表里**(现标注为 COMPAT 兜底)。

---

## 2. 方向二:实机验证(真实包 `asio` 1.38.1,path index,mcpp 0.0.104 避开 INV-NAME 干扰)

固定 `namespace = "acme"`,只改 `name` 字段:

| # | `name` | mcpp 发出的 target | 结果 | store 目录 |
|---|---|---|---|---|
| 1 | `acme.asio` | `acme:acme.asio@1.38.1` | ✅ 装上并编译 | `acme-x-acme.asio` |
| 2 | `asio` | `acme:acme.asio@1.38.1` | ❌ `E_NOT_FOUND` | — |

第 2 行即 **#278 事故的最小复现**:索引 key 是 `asio`,mcpp 却要 `acme.asio`。

### 2.1 关键实验:改一行后重测

实验补丁(**未合入**,已复原):目标构造改用描述符字面 `name`。

```cpp
if (luaContent) {
    auto lit = mcpp::manifest::extract_xpkg_name(*luaContent);
    if (!lit.empty()) fqname = lit;
}
```

| # | `name` | 结果 | store 目录 |
|---|---|---|---|
| 3 | `asio`(短名) | ✅ **装上并编译** | `acme-x-asio` |
| 4 | `acme.asio`(FQN) | ✅ 装上并编译 | `acme-x-acme.asio` |
| 5 | mcpp 自身依赖(真实生态) | ✅ 无回归 | — |

**两种写法同时可用**,因为字面 `name` **恒等于**索引 key。下游(payload 定位、编译)零改动即работ——`install_dir_candidates` 已含短名候选。

---

## 3. 证伪:我此前「根因在 libxpkg」的判断是错的

我曾主张「libxpkg 的 key 空间没有 namespace 维度,所以 `name` 必须自带区分度」。**实验 3 直接证伪**:key 空间没变、libxpkg 一行没动,短名形式照样装上。

正确表述:key 空间确实是扁平的、以 `pkg.name` 字面值为键 —— 但这**不构成对 `name` 写法的约束**,因为 mcpp 完全可以把字面值原样送过去。真正的约束是 mcpp 自己**重新渲染**了一个可能不同的字符串。

**撞键顾虑同样不成立**:`compat` 的 `zlib` 与 `mcpplibs` 的 `zlib` 若都写 `name = "zlib"`,确实会在**同一索引内**撞键 —— 但这是**索引内命名唯一性**问题,可由 lint 保证(同一索引内 `name` 唯一),不必让每个包都背 FQN。跨索引不撞,因为每个索引一张表。

---

## 4. 兼容矩阵

| | 现网描述符(FQN) | 新描述符(短名) |
|---|---|---|
| 现有 mcpp(≤0.0.105) | ✅ | ❌ `E_NOT_FOUND` |
| 改后 mcpp(用字面 name) | ✅ | ✅ |

**只有一个破损格**:旧 mcpp + 新描述符。含义:

- 迁移**不是**一次性切换。改后的 mcpp 对两种写法都工作,所以**先发 mcpp、再按节奏迁描述符**。
- 描述符一旦改短名,消费它的最低 mcpp 版本就抬高了 → 迁移时 `index.toml` 的 `min_mcpp` **必须**同步抬到含该修复的版本(这次是硬性的,不像 0.0.105 那次可选)。

---

## 5. 对 PR#117 的处置建议

PR#117 现有文档把**编码约束**写成了**设计规则**,方向与上述结论相反,**不应按现状合入**。三选一:

**A. 拆分(推荐)** —— PR#117 只保留无争议部分:去重(字节级重复的 `capi.lua.lua`)、规范路径(`tensorvia-cpu.lua` → `aimol.tensorvia-cpu.lua`)、规则 2(短名原子)、规则 3(规范路径)、CI 升 0.0.105。**删掉全部关于 `name` 形式的表述与规则 1 lint**,留给迁移 PR。

**B. 并入迁移** —— PR#117 改造成迁移的一部分:等 mcpp 侧修复发布后,49 个描述符全改短名、lint 规则 1 反转、`min_mcpp` 抬版。

**C. 维持现状** —— 只在文档里把 FQN 标注为「当前编码约束、非设计理想」。**不推荐**:等于把一条要回滚的约定写进 49 个包的规范。

---

## 6. 若走迁移,工作项清单

**mcpp 侧(一次发版,例如 0.0.106):**

- [ ] `prepare.cppm` 目标构造改用描述符字面 `name`(字面为空时回落到现有渲染)
- [ ] `compat.cppm` `install_dir_candidates` 把 `{ns}-x-{short}` 提为**首选**,`{ns}-x-{fqname}` 降为兼容项
- [ ] **INV-NAME 谓词的语义反转**:从「必须是 FQN」改为「`name` 不得含点(层级归 `namespace`)」;`xpkg_name_form_violation` 及 `mcpp xpkg parse` 同步
- [ ] e2e 161 断言反转;新增「短名描述符可安装」正向 e2e
- [ ] 文档 `docs/05-mcpp-toml.md` §2.5 的 xpkg 作者段改写

**mcpp-index 侧(mcpp 发布后):**

- [ ] 49 个描述符 `name` 去掉 namespace 前缀
- [ ] `check_package_name.lua` 规则 1 反转
- [ ] `check_package_filename.lua` 的规范路径公式随之调整(FQN 文件名 → ?需重新定义)
- [ ] `index.toml` `min_mcpp` / `latest_mcpp` 抬到 0.0.106
- [ ] 一次全量 `mcpp test --workspace` 三平台验证

**未决问题(需拍板):**

1. **规范文件名怎么定?** 现行公式对非默认命名空间用 `<FQN>.lua`。若 `name` 变短名,是继续用 `<ns>.<short>.lua`(与 `name` 不再字面相等)还是改 `<short>.lua`(跨命名空间会重名)?
2. **同索引内 `name` 唯一性**由谁保证?建议新增 lint:同一索引内 `name` 字面值不得重复(替代 FQN 天然提供的唯一性)。
3. **是否需要过渡期双读?** 即改后的 mcpp 已同时支持两种写法,故不需要;但若希望旧 mcpp 也能读新描述符,只能靠 `min_mcpp` 挡住,无技术手段。

---

## 附:我为什么把 PR#117 写成那样

按时间顺序,证据链是这样的,错在**没有做本报告 §2.1 这一步实验**:

1. #278 事故现场:`name` 从 FQN 改成短名 → 三平台 `E_NOT_FOUND`。→ 得出「短名不可用」。**正确但不完整**——只证明了"当前 mcpp 下不可用",没证明"必然不可用"。
2. 读 libxpkg:`entries[pkg.name]` 精确匹配、`build_index` 不传 namespace。→ 得出「key 空间无 namespace 维度,`name` 必须自带区分度」。**这一步是错的**:key 空间的形状不约束 `name` 的写法,只要 mcpp 把字面值原样送过去即可。
3. 全索引扫描:46/49 用 FQN,2 个短名的都坏了。→ 强化了「FQN 是约定」。**幸存者偏差**:它们坏是因为 mcpp 的渲染,不是因为写法本身。
4. 于是把「FQN」当作规则写进 lint 与文档。

**根本失误**:我把「当前实现的行为」当成了「架构的约束」,而没有去问"这一行改掉会怎样"。评审的两次纠正——先是「a.b.c 应在 namespace」,再是「mcpp 不依赖 xlings 的 name 机制」——正是指向这个缺口。§2.1 的实验本应在写 lint **之前**做。
