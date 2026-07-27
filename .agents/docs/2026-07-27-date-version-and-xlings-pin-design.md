# 日期版本号 + xlings pin 收敛 — 设计

日期：2026-07-27
状态：设计定稿，待实施
关联：#289（沙箱 xlings 永不刷新）、0.0.109 批次（#286/#287/#288）

---

## 1. 目标

两件事，合并为一个 PR：

1. **mcpp 版本号改为日期格式** `YYYY.M.D.N`，与 xlings 生态对齐（xlings 已于本日从 `0.4.70` 迁到 `2026.7.27.0/.1/.2`）。
2. **把所有 xlings pin 升到最新** `2026.7.27.2`，包括 mcpp release 内置的那一份。

第 2 件事是例行操作；第 1 件事**会踩到一处真实的比较逻辑断点**，必须先修，否则版本一改，索引底线契约（E0006）就会静默失效。

---

## 2. 版本号规范

### 2.1 格式

```
YYYY.M.D.N
2026.7.27.1
 │   │ │  └── 当日序号（ordinal）
 │   │ └───── 日，不补零
 │   └─────── 月，不补零
 └─────────── 年
```

不补零是与 xlings 已发布版本一致的写法（`2026.7.27.2`，不是 `2026.07.27.2`）。补零会让两个生态的版本串对不齐，也会让字符串比较的行为在跨月时更难推理。

### 2.2 第 4 段的语义

> **`.0` 保留给正式版本 / 稳定版本。日常迭代默认从 `.1` 开始。**

即：一天之内可以有 `.1`、`.2`、`.3` …… 若干次常规发布；`.0` 这个位置只在该版本被认定为正式 release 或稳定版时才使用。

**本次发布取 `2026.7.27.1`** —— 这是一次常规迭代（版本方案迁移 + pin 升级），不是被指定的稳定版里程碑。若要改为 `.0`，只需改动版本常量本身，实现不受影响。

### 2.3 与旧版本的序关系

旧 `0.0.109` → 新 `2026.7.27.1`。第一段从 `0` 变 `2026`，在任何按段做数值比较的实现里都是单调递增的，不存在回退。这一点在 xlings 侧同样成立（`0.4.70` → `2026.7.27.0`），生态两侧方向一致。

---

## 3. 核心问题：4 段版本在比较时被静默截断

### 3.1 断点

`src/version_req.cppm` 的 `Version` 是**严格三段**：

```cpp
struct Version {
    int major = 0, minor = 0, patch = 0;
    auto operator<=>(const Version&) const = default;
};
```

`parse_version` 的循环上界是 `idx < 3`：

```cpp
int* parts[3] = { &v.major, &v.minor, &v.patch };
int idx = 0;
while (idx < 3 && i <= s.size()) { ... }
```

于是 `parse_version("2026.7.27.1")` 得到 `{2026, 7, 27}` —— **第 4 段被丢弃且不报错**。后果：

```
2026.7.27.0 == 2026.7.27.1 == 2026.7.27.9      // 全部相等
```

### 3.2 这会坏掉什么

**E0006 索引底线契约**（`src/pm/index_contract.cppm:83-86`）：

```cpp
auto need = mcpp::version_req::parse_version(minMcpp);
auto have = mcpp::version_req::parse_version(ownVersion);
if (!need || !have) return std::nullopt;   // 畸形契约永不 brick
if (*have >= *need) return std::nullopt;
```

索引写 `min_mcpp = "2026.7.27.5"`，用户跑 `2026.7.27.1` —— 两者比较相等，`have >= need` 成立，**底线检查放行**。用户拿着不够新的 mcpp 去读新索引，得到的是描述符读取返回空这类难以归因的次生故障（这正是 0.0.109 批次里已经踩过一次的形态）。

这不是理论风险：底线契约存在的唯一理由就是挡住这种情况，而日期版本让它在**同一天内**完全失效。

### 3.3 修法：4 段 + 精确回写

```cpp
struct Version {
    int major = 0, minor = 0, patch = 0, revision = 0;
    int components = 0;      // 源串实际写了几段，仅用于回写，不参与比较
};
```

两条约束，缺一不可：

**(a) 比较只看 4 个数字，不看 `components`。**
不能再用 `= default` 的 `<=>`（它会把 `components` 也纳入比较，导致 `"1.2"` ≠ `"1.2.0"`）。必须显式按 major → minor → patch → revision 比较。

**(b) `str()` 必须精确回写源串的段数。**
这条是**载荷性的**，不是美观问题。`src/pm/resolver.cppm:138`：

```cpp
return parsed[*idx].str();
```

依赖解析返回的版本串是**从 `Version` 重建的**，不是原始字符串 —— 它会流向 lock 文件、wire 地址、安装目标。所以：

| 源串 | 解析 | `str()` 必须给出 |
|---|---|---|
| `1.15.2` | {1,15,2,0} c=3 | `1.15.2` ← 不能是 `1.15.2.0`，否则所有现存依赖的寻址全变 |
| `2026.7.27.1` | {2026,7,27,1} c=4 | `2026.7.27.1` |
| `2026.8.1.0` | {2026,8,1,0} c=4 | `2026.8.1.0` ← **不能塌成 `2026.8.1`** |

第三行正是 `.0` 正式版约定的直接后果。若只用「第 4 段非 0 才打印」这种简化规则，`.0` 版本会被回写成三段，与索引里的字面 key 对不上。`components` 字段就是为这一行存在的。

### 3.4 连带需要照顾的点

`matches()` 里 Caret / Tilde 构造上界时是逐字段改写 `c.v`，新增字段后必须一并清零：

```cpp
case Op::Caret:  upper = c.v; ++upper.major; upper.minor = upper.patch = upper.revision = 0;
case Op::Tilde:  upper = c.v; ++upper.minor; upper.patch = upper.revision = 0;
```

漏掉 `revision = 0` 会让 `^2026.7.27.3` 的上界变成 `2027.0.0.3`，把 `2027.0.0.0` ~ `2027.0.0.2` 错误排除。

对既有三段依赖，新增的第 4 段恒为 0，`^`/`~`/`=` 的语义**逐字节不变**。

---

## 4. xlings 侧：为什么不需要改 xlings 也能跑通

需要先确认一件事 —— xlings 自己的 `semver::parse`（`src/core/semver.cppm`）对 4 段版本是**直接拒绝**的：

```cpp
// Trailing content after 3 components is invalid
if (pos < numpart.size()) return std::nullopt;
```

那 mcpp 发成 `2026.7.27.1` 还装得上吗？**装得上**，因为解析路径根本不走 semver：

`src/core/xim/catalog.cppm::select_version_` 的顺序是

1. **先在版本表里做字面 key 精确匹配** —— `versions.find("2026.7.27.1")` 命中 `mcpp.lua` 里的字面键，直接返回；
2. 没有 hint 时读 `latest = { ref = "..." }`，同样是字面串；
3. 只有 hint 无法字面命中时，才落到 `semver::select_best`。

而 `xlings.lua` 里已经躺着 `["2026.7.27.2"]` / `["2026.7.27.0"]` 且 `latest.ref = "2026.7.27.2"` —— 这条路径**在生产里已经被 xlings 自己验证过了**。

### 已知的 xlings 侧边界（记录，不在本次范围）

| 行为 | 状况 |
|---|---|
| `xlings install mcpp@2026.7.27.1` | ✅ 字面 key 精确匹配 |
| `xlings install mcpp`（latest） | ✅ 读 `latest.ref` |
| `sort_desc` 排序 | ⚠️ 4 段解析失败 → 回退**字典序**。日期 > 旧 semver 成立；同日 `<10` 次发布也成立；**第 10 次发布起 `.10` 会排在 `.9` 之下** |
| `xlings install mcpp@2026.7`（前缀） | ❌ 前缀范围匹配对 4 段版本无效 |
| `quick_install.sh` 镜像择优 | ⚠️ `sort -t. -k1,1n -k2,2n -k3,3n` **只排前三段**。仅在**未**显式指定版本时执行 —— 本仓所有 bootstrap 都显式传 `v<ver>`，故不触发 |

后三条对 mcpp 的实际发布节奏无影响（同日 10 次发布不现实、前缀装 mcpp 无人使用、bootstrap 一律显式 pin），但值得作为 upstream issue 记在 xlings 侧，因为**它们同样作用于 xlings 自己的版本**。

已核实（不是推断）：`v2026.7.27.2` 在 `openxlings/xlings` 与 `d2learn/xlings` 两个 org 上都存在，四平台资产齐全、命名与 `0.4.69` 完全同构，直连 GET 返回 200。release.yml 里那份 aarch64 xlings 是 `if curl` 保护的 —— 资产缺失只会**静默降级**成不打包，所以必须先验证再升。

---

## 5. 版本号与 pin 的完整落点

这是本设计的另一半价值：把散落的落点一次性列全。当前 `src/xlings.cppm` 那句「keep in lock-step with release.yml / cross-build-test.yml / ci-linux-e2e.yml」的注释**已经是不全的** —— 它漏掉了两个 composite action，而那两个正是 0.0.109 批次里让 CI 沙箱烂在 0.4.30 的元凶。

### 5.1 mcpp 自身版本（4 处）

| 位置 | 形态 |
|---|---|
| `mcpp.toml` `[package].version` | release.yml 由它 awk 出 tag |
| `src/toolchain/fingerprint.cppm` `MCPP_VERSION` | `--version` 输出 + BMI 指纹 + E0006 比较 |
| `.xlings.json` `workspace.mcpp` | CI bootstrap 装哪个 mcpp |
| `.github/workflows/ci-fresh-install.yml` `MCPP_PIN` | 全新安装验证 |

### 5.2 xlings pin（6 个文件）

| 文件 | 数量 |
|---|---|
| `src/xlings.cppm` `pinned::kXlingsVersion` | 1（`mcpp self env` 打印） |
| `.github/actions/bootstrap-mcpp/action.yml` | default + cache key/lineage |
| `.github/actions/setup-macos-llvm/action.yml` | 同上 |
| `.github/workflows/bootstrap-macos.yml` | `XLINGS_VERSION` |
| `.github/workflows/ci-linux-e2e.yml` | `quick_install.sh -s v<ver>` |
| `.github/workflows/cross-build-test.yml` | `XLINGS_VERSION` ×2 |
| `.github/workflows/release.yml` | `XLINGS_VERSION`（**这一份决定 release 内置的 xlings**） |

「mcpp 依赖的内部 xlings」即 release.yml 打进 tarball 的 `<install>/registry/bin/xlings`，由 release.yml 的 `XLINGS_VERSION` 决定，`kXlingsVersion` 只是把它打印出来。两者必须一致，但**当前没有任何机制保证**。

下载 URL 用的是 `d2learn/xlings`，发布在 `openxlings/xlings`。已核实两个 org 的 `v2026.7.27.2` 与 `v0.4.69` tag 均存在，升级无风险。

### 5.3 格式敏感的脚本

`.github/tools/install_pinned_mcpp.sh` 两处：

```bash
grep -oE '[0-9]+\.[0-9]+\.[0-9]+' | head -1
```

对 `2026.7.27.1` 只截出 `2026.7.27`。pin 与实测值都被同样截断，于是**校验会通过，但比的是三段** —— 同日任意序号都算匹配，这个 pin 校验就退化成了摆设。必须改为接受 3 或 4 段并精确比较。

---

## 6. 结构性修复：漂移守卫

上面 5.1 / 5.2 一共 10 个落点，靠注释和人工同步。0.0.109 批次已经证明这条约定会失效，而失效方式是**静默的**：CI 沙箱在 0.4.30 上跑了很久没人发现，直到索引做了短名迁移才炸出来。

因此本次一并加一个检查，把「约定」变成「机制」：

- 所有 xlings pin 必须等于 `pinned::kXlingsVersion`
- `mcpp.toml` == `MCPP_VERSION` == `.xlings.json` == `MCPP_PIN`

任一不一致 → CI 失败并指出具体文件。这直接对应 #289 里记的「Adjacent, same shape」那条。

守卫本身用纯文本提取实现，不依赖 mcpp 二进制 —— 它必须能在 mcpp 构建失败时依然跑得起来，否则在最需要它的时候恰好不可用。

---

## 7. 不做的事

- **不改 xlings 的 semver 解析。** 4 段拒绝解析是 xlings 侧的问题，且已由「字面 key 优先」的解析顺序绕开。要改应当在 xlings 仓库单独提 issue/PR，混进本 PR 会让两个生态的发布互相阻塞。
- **不动 #289 的沙箱 xlings 刷新功能。** 本次只把 pin 升到最新；运行时检查与同步是独立特性。
- **不改索引侧 `mcpp.lua`。** 新版本条目由发布流程的 `res_versioned` 机制追加。

---

## 8. 验收口径

1. `parse_version("2026.7.27.1") != parse_version("2026.7.27.2")`，且 `<` 成立 —— 单元测试锁住。
2. `str()` 对 `1.15.2` / `2026.7.27.1` / `2026.8.1.0` 三种输入精确回写 —— 单元测试锁住。
3. E0006 在同日不同序号之间能正确触发 —— 单元测试锁住。
4. 漂移守卫在故意改坏一个 pin 时失败 —— 负向验证，不能只看它在正常状态下通过。
5. 全平台 CI 绿；发布后真装 `mcpp@2026.7.27.1` 并跑通一次依赖解析。
