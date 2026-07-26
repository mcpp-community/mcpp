# 裸名依赖的 wire address 收敛 — Design（mcpp 0.0.109）

> 触发事件：mcpp-index PR #120（SPEC-001 短名迁移，2026-07-25T20:10:37Z 合入）后，
> mcpp main 的 7 个 workflow 红了 6 个。诊断报告见本文 §1，那不是新引入的回归。

## 1. 事实与证据

### 1.1 时间线

| 时间 (UTC) | 事件 | 结果 |
|---|---|---|
| 2026-07-25 18:43 / 18:58 | PR #282 的 CI（**与 main 完全相同的 mcpp 代码**） | ✅ 全绿 |
| **2026-07-25 20:10:37** | **mcpp-index #120 合入**（48 个描述符 FQN → 短名，floor → 0.0.108） | — |
| 2026-07-25 20:18:22 | #282 squash 进 mcpp main，push 触发 CI | ❌ 7 个 workflow 红 6 个 |

mcpp 侧 diff 只有 `.xlings.json` 一行。**代码没变，索引变了。**

### 1.2 决定性对照（`integration: mcpp builds & runs xlings`，同一次运行、同一进程）

`MCPP_VERBOSE=1` 抓到的真实 xlings 调用：

```
targets:["compat:zlib@1.3.2"]        → ✅
targets:["compat:lz4@1.10.0"]        → ✅
targets:["compat:xz@5.8.3"]          → ✅
targets:["compat:zstd@1.5.7"]        → ✅
targets:["compat:libarchive@3.8.7"]  → ✅     ← 依赖写的是限定名 compat.xxx
targets:["mcpplibs:ftxui@6.1.9"]     → ❌
targets:["compat.ftxui@6.1.9"]       → ❌     ← 依赖写的是裸名 ftxui
```

**限定名走 `compat:<short>` 全通；裸名走 `mcpplibs:<short>` 全挂。** 正确地址是
`compat:ftxui@6.1.9`，两条尝试都没打中。

### 1.3 根因

裸名 `gtest = "1.15.2"` 解析为 `ns=kDefaultNamespace("mcpplibs")` + `short=gtest`
（`src/pm/dep_spec.cppm:58`）。

**第一步：身份门刻意放宽，接受 compat 描述符。**
`src/manifest/xpkg.cppm` `xpkg_lua_identity_matches`：

```cpp
if (ns == kDefaultNamespace) {
    return id.ns == kDefaultNamespace
        || id.ns == kCompatNamespace     // ← compat.gtest.lua 从这里被接受
        || (allowLegacyBareDefault && id.ns.empty());
}
```

**第二步：拼 wire target 时，name 取自描述符，namespace 取自请求。**
`src/build/prepare.cppm:1864`：

```cpp
auto wireName = luaContent ? extract_xpkg_name(*luaContent) : "";   // "gtest"   ← 描述符
auto target   = std::format("{}:{}@{}", ns, wireName, version);     // ns="mcpplibs" ← 请求
```

身份门认下的是 `(compat, gtest)`，发出去的却是 `mcpplibs:gtest`。
**同一次解析，身份有两个来源。**

**第三步：兜底写死迁移前的字面名。**

```cpp
auto compatTarget = std::format("compat.{}@{}", shortName, version);
```

### 1.4 为什么迁移前是绿的

迁移前 `extract_xpkg_name` 返回 `"compat.gtest"` → 首选 target `mcpplibs:compat.gtest`
（同样是错的，同样挂）→ 兜底 `compat.gtest@1.15.2` **恰好命中旧字面名，救了回来**。

**主路径一直是坏的，一直靠那条写死的 legacy 兜底在扛。** #120 抽掉了兜底命中的
字面名，主路径的 bug 才现形。这是存量缺陷到期，不是新回归。

### 1.5 改动面（对现网索引 48 个描述符逐个核对）

| 裸名请求能解析到的描述符 | 数量 | 修复后 |
|---|---|---|
| `namespace = "mcpplibs"`（cmdline / tinyhttps / opencv / imgui / ffmpeg / llmapi / templates / xpkg） | 8 | **无变化**（wireNs == 请求 ns） |
| `namespace = "compat"` | 34 | `mcpplibs:X` → `compat:X`，**这 34 个当前 100% 是坏的** |
| `namespace` 为空的 legacy 描述符 | **0** | 边界情况在真实索引里不存在 |
| 其他 ns（chriskohlhoff / fmtlib / marzer / nlohmann / aimol / mcpplibs.capi） | 6 | 身份门本就拒绝裸名请求，够不到 |

**只有当前全坏的 34 个会变。没坏的一个都不动。**

### 1.6 第二个独立失效面：e2e 163 的 fixture 是对上游描述符做 sed

`tests/e2e/163_identity_first_resolution.sh:42` 把注册表里真实的
`chriskohlhoff.asio.lua` sed 成测试 fixture：

```bash
sed -e "s/name        = \"chriskohlhoff.asio\"/name        = \"$4\"/"
```

#120 把 asio 也迁了（`name = "chriskohlhoff.asio"` → `"asio"`），**第二条 sed 不再匹配**，
fixture 身份变成 `(acme, asio)`，而 app 请求 `acme:widget`：

```
error: dependency 'acme.widget': not found in local index at '.../idx1'
```

一个看起来 hermetic（`mktemp -d`、无网络）的测试，实际把上游描述符的**逐字文本**
编进了断言。linux shard2 / windows shard2 / macOS 三处同时挂。

### 1.7 第三个（潜伏）：bootstrap 版本漂移 vs 索引 floor

```
error: index requires mcpp >= 0.0.108 but this is mcpp 0.0.107 [E0006]   # linux
error: index requires mcpp >= 0.0.108 but this is mcpp 0.0.102 [E0006]   # windows
```

`.github/actions/bootstrap-mcpp/action.yml` 跑的是 **`xlings install mcpp -y`——没有版本
pin**。`.xlings.json` 里的 pin 只进了 cache key；而 `restore-keys` 前缀匹配会把旧 cache
捞回来，`install` 幂等地保留缓存里的任意旧版本。

当前被热 cache 掩盖（`mcpplibs.cmdline` 已缓存，不需要读索引）。**冷 cache 一来就是硬失败。**

### 1.8 索引侧的流程盲区（为什么 #120 自己 8 个 check 全绿）

mcpp-index 每个 example 用的都是限定名 + path 索引：

```toml
[indices]
compat = { path = "../../.." }
[dependencies.compat]
gtest = { version = "1.15.2", features = ["main"] }
```

`[dependencies.compat]` 走 `compat:<short>`，正是**唯一还能通的那条路**；而所有真实
消费者（含 mcpp 自己的 `mcpp.toml`）写的是裸名。**验证矩阵与消费矩阵不同构** →
结构性假绿。（与 `xcb-link-regression` / `spec001` 两次是同一类。）

---

## 2. 设计

### 2.1 原则

> **身份门认下谁，就用谁的两半。**

`read_xpkg_lua*` 家族已经用 `xpkg_lua_identity_matches` 把一个描述符**认定为**这次请求
的提供者。既然认定了，wire address 的 namespace 和 name 就都必须来自它——不能一半取
描述符、一半取请求。

这与 SPEC-001 §6 一致：wire key 是字面 `package.name`，地址是
`<effectiveNamespace>:<literal name>`。

### 2.2 新增纯函数（可单测）

`prepare.cppm` 里那段逻辑很难单测。把它抽成 `src/manifest/xpkg.cppm` 的纯函数——
`canonical_xpkg_identity_from_lua` 就在隔壁，身份门也在那里：

```cpp
struct XpkgWireAddress {
    std::string ns;       // effective namespace（"" = 无命名空间包）
    std::string name;     // 字面 package.name（SPEC-001 §6 的 wire key）
    std::string target;   // "<ns>:<name>" 或 "<name>"
};

XpkgWireAddress xpkg_wire_address(std::string_view luaContent,   // 可为空
                                  std::string_view requestNs,
                                  std::string_view shortName);
```

语义：

| luaContent | ns | name |
|---|---|---|
| 有，且声明了 namespace | 描述符的 effective ns | 字面 `package.name` |
| 有，但没声明 namespace | `requestNs`（= `indexDefaultNs` 语义） | 字面 `package.name` |
| 无 / 无 name | `requestNs` | `requestNs.empty() ? shortName : "<requestNs>.<shortName>"`（**保持历史推导不变**） |

最后一行是关键：描述符读不到时**不改变任何现有行为**，失败点留在原地。

### 2.3 兜底链加宽

兜底只在首选 target 失败后触发。当前只试 `compat.<short>`（迁移前字面名）。
新索引下该字面名已不存在，因此补上 SPEC-001 拼法：

1. `compat:<shortName>@<ver>` — SPEC-001 短名形态（**新增**）
2. `compat.<shortName>@<ver>` — legacy 字面名（保留）

两条都只在与首选 target 不同时才发。这让「描述符读不到」的路径对新旧两种索引都成立。

### 2.4 不做的事

- **不改 `xpkg_lua_identity_matches` 的放宽规则。** 裸名能解析到 compat 是有意设计
  （`gtest = "1.15.2"` 必须能用），本次只修地址推导。
- **不动 xlings 任何规范。**
- **不处理 `imgui`/`ffmpeg` 在 mcpplibs 与 compat 下各有一份描述符、靠候选文件名顺序
  消歧这件事。** 那是独立的存量歧义（与 #278 同类），修复保持现有结果不变
  （`imgui.lua` 先于 `compat.imgui.lua`，mcpplibs 胜出），另开 issue。

---

## 3. 验证策略

### 3.1 单测（`tests/unit/test_manifest.cpp`）

针对 `xpkg_wire_address` 的纯函数矩阵——覆盖 §1.5 表格的每一行：

- compat 描述符 + 裸名请求 → `compat:gtest`（**当前 bug 的直接锁定**）
- mcpplibs 描述符 + 裸名请求 → `mcpplibs:cmdline`（不变）
- legacy FQN 描述符（`ns=compat, name=compat.zlib`）+ 裸名请求 → `compat:compat.zlib`
- 限定请求（`ns=compat`）+ compat 描述符 → `compat:gtest`（不变）
- 嵌套 ns（`ns=mcpplibs.capi, name=lua`）→ `mcpplibs.capi:lua`（不变）
- 无 luaContent → 历史推导 `mcpplibs:mcpplibs.gtest`（不变）
- 描述符无 namespace 声明 → 回落 requestNs

### 3.2 e2e（新增 `165_bare_name_cross_namespace_wire_address.sh`）

**必须 hermetic 地复现生产 bug**——这是本次最重要的一条，因为索引侧的假绿正是
"验证矩阵与消费矩阵不同构"造成的。

用 `[indices] mcpplibs = { path = ... }` 把默认命名空间路由到本地 fixture 索引，
索引里放一个 `namespace = "compat", name = "widget"` 的描述符，app 写**裸名**依赖：

```toml
[indices]
mcpplibs = { path = "../idx" }
[dependencies]
widget = "1.38.1"
```

- 修复前：target = `mcpplibs:widget` → 装不上
- 修复后：target = `compat:widget` → 装上，落在 `compat-x-widget`

断言 store 目录 `compat-x-widget` 存在——直接锁住 wire namespace 取值。

### 3.3 e2e 163 去耦合

sed pattern 改为不依赖旧拼写：

```bash
sed -e "s/^\( *namespace *= *\)\"[^\"]*\"/\1\"$3\"/" \
    -e "s/^\( *name *= *\)\"[^\"]*\"/\1\"$4\"/"
```

（更彻底是 fixture 完全自造，但那会丢掉「用真实描述符跑通整条 install 路径」这个
本来有价值的性质，故只做 pattern 去耦合。）

### 3.4 bootstrap pin

`xlings install mcpp@<pin> -y`，pin 从 `.xlings.json` 读，装完断言
`mcpp --version` 与 pin 相等——版本漂移立刻可见，而不是等冷 cache 才炸。

---

## 4. 交付

- 单 PR，版本 **0.0.109**（`mcpp.toml` + `src/toolchain/fingerprint.cppm` 两处同步）。
- CI 全绿（7 workflow）→ bypass squash 合入。
- release 0.0.109 → 镜像 xlings-res 双端 → xim-pkgindex → 真装验证 → bootstrap pin bump。

## 5. 后续（不在本 PR）

- mcpp-index 补一个「裸名 + 远端 git 索引」的 example，堵住 §1.8 的盲区。
- mcpp-index 清 `chriskohlhoff.asio.lua:41-48` 的陈旧注释（还写着 `name` MUST be FQN，
  第 49 行已是 `name = "asio"`）。
- `imgui`/`ffmpeg` 双描述符靠文件名顺序消歧 → 独立 issue。
