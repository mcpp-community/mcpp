# xlings#381 索引键补齐命名空间维度 — 设计/优化方案

> Issue: [openxlings/xlings#381](https://github.com/openxlings/xlings/issues/381)
> 基线:xlings 0.4.68 / libxpkg 当前 HEAD
> 影响仓:`openxlings/xlings`(catalog 层)、`mcpplibs/libxpkg`(index 层)
> 关联:mcpp SPEC-001 §3.3(`docs/spec/package-identity.md`)

---

## 1. 一句话结论

`IndexEntry` **不携带 `namespace`** —— 这是本 bug 的结构性成因,也是任何修法的第一块砖。补上它之后,**修复几乎不需要新机制**:xlings 早已有「多候选 + ambiguous 报错」的完整机器,只是被 `build_match_` 每仓只产一个 match 的瓶颈卡住了。

推荐 **方案 B:`entries` 改为 `name → vector<IndexEntry>`**,把仓内同名包变成候选项,交给既有聚合层处理。

---

## 2. 根因:两处缺陷,一处是数据丢失、一处是传导瓶颈

### 2.1 数据丢失 —— 建索引期静默覆盖

```cpp
// libxpkg/src/xpkg-loader.cppm:587
std::string key = (namespace_.empty() ? "" : namespace_ + "-x-") + pkg.name;
index.entries[key] = std::move(ie);      // ← 同名直接覆盖,无任何提示
```

`namespace_` 是**函数参数**(唯一调用点 `xlings/src/core/xim/index.cppm:153` 不传值),不是 `pkg.namespace_`。故键 = 裸 `pkg.name`,同名包互相覆盖。

### 2.2 关键结构事实 —— `IndexEntry` 里没有 namespace

```cpp
// libxpkg/src/xpkg.cppm:94
struct IndexEntry {
    std::string name;          // "vscode@1.85.0"
    std::string version;
    std::filesystem::path path;
    PackageType type;
    std::string description;
    bool installed;
    std::string ref;           // alias target
};                             // ← 没有 namespace
```

所以查询路径必须**重新加载并解析 `.lua`** 才知道命名空间:

```cpp
// xlings/src/core/xim/catalog.cppm:285-300
if (auto* entry = state.index.find_entry(resolved)) { matched = entry->name; }
auto pkg = state.index.load_package(*matched);           // ← 到这一步才拿到 namespace
auto ns  = pkg->namespace_.empty() ? state.spec.defaultNamespace : pkg->namespace_;
if (parsed.explicitNamespace && parsed.namespaceName != ns) return {};   // 命中后过滤
```

**这既是本 bug 的成因,也是一个顺带的性能问题**(每次查询都读盘解析 Lua)。补 `namespace` 字段后两者一并解决。

### 2.3 传导瓶颈 —— 每仓只产一个 match

```cpp
// catalog.cppm:328
std::vector<PackageMatch> collect_matches_(...) {
    for (auto& repo : repos) {
        auto match = build_match_(repo, parsed, platform);   // ← 单数
        ...
    }
}
```

**xlings 已经具备处理多候选的全部机器**:`primaryMatches` / `subMatches` 分层、`dedupe_matches_`、`format_ambiguous_candidates`(`catalog.cppm:65`)、显式 ns 过滤。跨仓同名之所以工作正常,正是因为走了这条路。仓**内**同名只是从未有机会进入这条路。

> **设计要点:修复不是加机制,是把仓内候选接进已有机制。**

---

## 3. 方案对比

| | 方案 | 键形态 | 评价 |
|---|---|---|---|
| A | 复合键 `(namespace, name)` | `map<pair<string,string>, IndexEntry>` 或拼接字符串 | ❌ 拼接需分隔符,而 `-x-` 已被 `merge()` 用作 sub-index 前缀语义,复用会混淆;无 ns 查询退化为全表扫描;`search`/日志里的键不再是人类可读的包名 |
| **B** | **`name → vector<IndexEntry>`** | `unordered_map<string, vector<IndexEntry>>` | ✅ **推荐**。键仍是包名(`search`、`match_version` 前缀扫描、日志全部不变);无 ns 查询天然拿到全部候选;有 ns 查询在向量内过滤 |
| C | 仅冲突时把 ns 折进键 | 混合 | ❌ 非一致表示,查找要分两种情况,复杂度最高 |

### 3.1 为什么 B 的改动面最小

现有 API 与其在 B 下的对应:

| 现有 | B 下 | 改动 |
|---|---|---|
| `find_entry(name) → IndexEntry*` | `find_entries(name) → span<const IndexEntry>` | 调用点由「取一个」改为「遍历」 |
| `resolve(name)`(alias 解引用) | 同名多条时按 ns 过滤后解引用 | 需要 ns 参数(可选) |
| `match_version(name)`(`name@ver` 前缀扫描) | 逻辑不变,键仍是 `name`/`name@ver` | **无改动** |
| `search(query)` | 遍历 vector | 小改 |
| `merge(base, overlay, ns)` | 按 `(ns,name)` 去重而非覆盖 | 小改 |
| `mark_installed(name)` | 需要 ns 定位具体条目 | 需要 ns 参数 |

---

## 4. 推荐实现(方案 B)

### 4.1 第一步:`IndexEntry` 携带 namespace

```cpp
struct IndexEntry {
    std::string namespace_;    // ← 新增,建索引期即填,查询无需再解析 .lua
    std::string name;
    …
};
```

`build_index` 里 `load_package(entry.path)` 已经拿到完整 `Package`,直接填 `ie.namespace_ = pkg.namespace_;` 即可,零额外开销。

**这一步单独就有价值**:`build_match_` 不必再 `load_package` 才能判断命名空间,把「每次查询读盘解析 Lua」降为「仅在确定要用该包时才解析」。

### 4.2 第二步:`entries` 改为多值

```cpp
struct PackageIndex {
    std::unordered_map<std::string, std::vector<IndexEntry>> entries;
    …
};

// build_index:同 (ns,name) 才算重复,否则并列
auto& bucket = index.entries[pkg.name];
auto dup = std::ranges::find_if(bucket,
    [&](const IndexEntry& e){ return e.namespace_ == pkg.namespace_; });
if (dup != bucket.end()) {
    log::warn("duplicate package identity ({}, {}) in {} — {} overrides {}",
              pkg.namespace_, pkg.name, repo_dir.string(),
              entry.path.string(), dup->path.string());
    *dup = std::move(ie);          // 真重复才覆盖,且**出声**
} else {
    bucket.push_back(std::move(ie));
}
```

> **注意这里同时消灭了「静默」**:真正的 `(ns,name)` 重复现在会告警。原 issue 里那条「最小改进:建索引期冲突告警」被自然吸收。

### 4.3 第三步:`build_match_` 产出多个 match

```cpp
// 由 build_match_ → build_matches_,返回 vector
std::vector<PackageMatch> build_matches_(RepoState& state,
                                         const ParsedTarget_& parsed, …) {
    std::vector<PackageMatch> out;
    for (auto& entry : state.index.find_entries(resolved)) {
        auto ns = entry.namespace_.empty() ? state.spec.defaultNamespace
                                           : entry.namespace_;
        if (parsed.explicitNamespace && parsed.namespaceName != ns) continue;  // ← 过滤前置
        …                       // 版本选择、storeRoot、installed 判定不变
        out.push_back(std::move(match));
    }
    return out;
}
```

`collect_matches_` 由 `push_back(match)` 改为 `append_range(matches)`。**下游一行不用改** —— `dedupe_matches_`、`primaryMatches`/`subMatches` 分层、`format_ambiguous_candidates` 全部照常工作。

于是:

- `alpha:demo` / `beta:demo`(仓内同名)→ 各自精确命中 ✅
- `demo`(不指定 ns,仓内两个同名)→ 走已有 ambiguous 报错,列出两个候选 ✅ —— **与今天跨仓同名的体验完全一致**

### 4.4 第四步:缓存格式

`.xlings-index-cache.json` 的每条 entry 增加 `namespace` 字段,并 **bump `CACHE_FORMAT_VERSION`**。旧缓存因版本不匹配自动失效重建,无需迁移代码。

---

## 5. 兼容性与风险

| 项 | 判断 |
|---|---|
| **索引数据格式** | 无变化 —— 描述符 `.lua` 一个字不用改 |
| **现网索引** | 无同 `name` 冲突(FQN 写法天然错开),修复后行为**完全不变** |
| **缓存** | 版本 bump → 首次运行重建一次,秒级 |
| **公开 API** | `libxpkg` 的 `find_entry`/`resolve`/`mark_installed` 签名有变;消费方仅 xlings 自身,可同批次改 |
| **`merge()` 的 `-x-` 前缀语义** | **保持不动**。它表达的是 sub-index 归属,与包命名空间是两件事,本方案不触碰 |
| **行为变化** | 仅在「仓内同名」这一此前不可用的场景;其余路径逐字节等价 |

**最大风险点**:`match_version` 的 `name@version` 前缀扫描现在要在 vector 上做。需确认版本化条目(`vscode@1.85.0` 作为**键**)与命名空间维度不打架 —— 建议保持「版本化键仍是独立 bucket」,即 `entries["vscode@1.85.0"]` 与 `entries["vscode"]` 各自成桶,与今天一致。

---

## 6. 分阶段落地

**Phase 1(独立可发,低风险)**
- `IndexEntry` 加 `namespace_`,`build_index` 填值;缓存字段 + 版本 bump
- `build_match_` 改用 `entry.namespace_` 而非 `load_package()` 取 ns → **顺带优化查询性能**
- `build_index` 对真 `(ns,name)` 重复告警
- 此阶段**行为不变**,纯结构准备 + 一个新告警

**Phase 2(修复本 issue)**
- `entries` 改多值;`find_entries`;`build_matches_`;`collect_matches_` 接入
- 回归:issue 里的最小复现应产出「两个都可见 / 显式 ns 各自命中 / 裸名报 ambiguous 列两候选」

**Phase 3(可选清理)**
- 审视 `merge()` 的 `-x-` 前缀是否仍必要(多值表后,sub-index 归属或可改由 `IndexEntry` 字段表达,而非污染键)

---

## 7. 验收标准

以 issue 的最小复现为准(纯 xlings,隔离 `XLINGS_HOME`):

```
$ xlings search demo
  ◆ alpha:demo   alpha's demo package
  ◆ beta:demo    beta's demo package        # ← 修复前缺失

$ xlings info alpha:demo   → ✅
$ xlings info beta:demo    → ✅              # ← 修复前 not found

$ xlings install demo
[error] package 'demo' is ambiguous, candidates:
1. alpha:demo@1.0.0   from repo 'demoidx'
2. beta:demo@1.0.0    from repo 'demoidx'    # ← 与跨仓同名体验一致
```

外加:跨仓同名(`xim:mcpp` / `local:mcpp`)行为**不得**变化;现网索引全量 `xlings search` / `install` 无回归。

---

## 8. 对 mcpp 的影响

**mcpp 侧不需要任何改动。** 按 SPEC-001 §1.2 的边界,mcpp 基于 xlings 机制实现、不重定义它们。本修复落地后:

- SPEC-001 §3.3 的「`(namespace, name)` 唯一」自然成立,其中的「≤0.4.68 已知缺口」小节可删除;
- mcpp-index **无需**任何 lint 规避;
- mcpp 若将来迁到短名形态(`name = "zlib"`),同一索引内 `compat`/`mcpplibs` 同短名不再有障碍 —— 本修复是那次迁移的**前置条件之一**。
