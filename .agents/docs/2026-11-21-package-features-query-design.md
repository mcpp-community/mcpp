---
subject: ecosystem
status: active
---

# 包的特性作为机器可读的回答：一个目录项，两种来源，三种状态（mcpp#992）

**日期：** 2026-11-21 · **版本：** v1 · **基线：** mcpp 2026.11.21.1 的分支（#984） · **规范：** SPEC-001 §3.5（描述符的两种形式）、
SPEC-004（清单的键）、SPEC-010 §5（只读查询不安装）、docs/50（`kindVersion` 内只增加字段） · **所属：**
[生态的测试与 CI 方案](2026-11-20-test-architecture-upgrade-design.md) 第 15 节的子方案三。

## 1. 问题

mcpp-ide 只显示 mcpp 陈述的事实，不读索引的描述符（mcpp-ide#16、#17：`features = [...]` 中的补全、悬停、未知名字的诊断、
包详情中的勾选）。mcpp 的机器输出不含包的特性：`mcpp search --format json` 只有 `description`、`id`、`index`、`licenses`、
`name`、`namespace`、`repo`、`versions`；`mcpp xpkg parse` 与 `mcpp why` 也没有。

## 2. 现状的测量

- **一个模型。** `mcpp.toml`（`toml.cppm`）与描述符的内联表（`xpkg.cppm` 的 `synthesize_from_xpkg_lua`）解析到同一个
  `Manifest`：`featuresMap`（特性 → implies，`default` 项为缺省集合）、`featureForwards`（`<依赖>/<特性>`）、
  `featureRequires`、`featureProvides`。不需要新的读取。
- **两种来源。** 本机 mcpplibs 索引的 342 个描述符中，197 个为内联形式，特性在索引中（30 个写有特性）；81 个为指针形式、
  66 个未写 `mcpp` 字段（按约定为指针形式），共 147 个，特性在载荷的 `mcpp.toml` 中。内联表对描述符的所有版本共用；
  指针形式的每个版本各有自己的清单。
- **读取路径已经收敛。** `mcpp.pm.descriptor` 的 `descriptor_payload_present` 判断载荷是否在，`descriptor_manifest` 给出
  内联表或载荷中清单的位置（mcpp#960）。构建在读取指针形式的清单时另做一步：归档中的 workspace 成员接受其 workspace
  的继承（`graph_load` 的 `loadFrom`）。
- **没有描述。** 两种来源都没有特性的 `description`；表形式的键检查（#540）把它报为未知键。

## 3. 决定

### 3.1 一个目录项

`mcpp.manifest` 增加 `feature_catalog(const Manifest&)`：每个特性一项，`name`、`default`（在缺省集合中）、`implies`
（本包的特性）、`enables`（`<依赖>/<特性>`）、`description`（可空），按名字排序；`default` 本身不是一项。所有输出由它给出，
JSON 的形状只在一处写出。

### 3.2 一条读取路径

`mcpp.pm.package_manifest` 的 `installed_manifest` 给出索引包某个已安装版本的清单：`descriptor_manifest` 之后，指针形式的
清单按构建的方式读取，含归档中 workspace 的继承与描述符的命名空间。构建（`graph_load`）与查询都调用它，二者对同一个包不会
给出不同的特性。`workspace_listing` 与 `inherit_as_workspace_member` 由 prepare 移入该模块；错误是 `ManifestError`
（`at` 为读不出的清单），由调用方以自己的主语写出，构建的消息不变。

### 3.3 三种状态

一个包的一个版本的回答：

```json
"features": {
  "version": "0.0.5",
  "state": "declared",
  "from": "payload",
  "list": [ { "name": "gui", "default": true, "implies": ["render"], "enables": ["imgui/docking"],
              "description": "..." } ]
}
```

| `state` | 含义 | `list` |
|---|---|---|
| `declared` | 回答取自索引（内联形式）或已安装的载荷中的清单 | 特性的列表，没有特性时为 `[]` |
| `not-installed` | 指针形式，该版本的载荷不在 store 中 | `null` |
| `unreadable` | 载荷在，清单读不出；另附一条诊断 | `null` |

`[]` 只表示「声明了零个特性」。未知的回答为 `null`：只读列表的客户端若把未安装读成没有特性，会把每个合法的名字诊断为
未知。`from` 为 `index` 或 `payload`。

不是 mcpp 包的条目不输出 `features`。判据取自已有的两个事实：描述符写有 `mcpp` 字段（任一形式）的是 mcpp 包；不写该字段的
描述符，在提供方的官方索引（`official_index_status` 的目录）中是提供方自身的载荷，在其他索引中按 SPEC-001 §3.5 的约定为
指针形式。本机的目录中，官方索引的 342 个 `xim` 条目与 `luban` 等命名空间的条目不输出；mcpplibs 索引的 66 个未写字段的
描述符按指针形式回答。

### 3.4 不安装

查询不下载、不安装（SPEC-010 §5）：已安装就读取，未安装就以 `not-installed` 回答。取得载荷的是构建与 `mcpp fetch`。

### 3.5 两个输出

- **`mcpp search --format json`**：每个包的 `features`，为最新版本（`versions` 的第一项）的回答。一个包的查询即精确名字的
  search。`mcpp.search` 的 `kindVersion` 保持 1（只增加字段）。
- **`mcpp why features [--format json]`**（`mcpp.why.features`）：本工程解析出的每个包，`package`、`root`（是否为根）、
  `manifest_dir`，以及 `features` 的 `list` 与 `active`（本次构建激活的特性）。规划时每个包都已在 home 中，列表总是已知的。
  数据记录在 `resolution.json` 的 `graph` 中（`features` 为激活的特性，与构建程序读取的图文档同名同形；`feature_list`
  为目录项），命令读取该记录，与 `mcpp why deps` 读取同一记录相同。构建程序的图文档不加目录项：它是构建程序重跑键的一部分，
  描述的改动不应使构建程序重跑。不给话题的 `mcpp why` 不含该话题，与 `flags`、`platform` 相同。

`--protocol-version` 的 `features` 增加 `package-features`。

### 3.6 描述

表形式的特性增加 `description`（字符串），与 `[package] description` 同名同义；描述符的内联表的特性项同样接受。
短形式（`x = ["a"]`）没有描述。键检查的已知键随之增加。

## 4. 不做的

- 不在发布时把特性抄进指针形式的描述符：两份事实会分离，描述符也不按版本区分。
- 不在查询中取得载荷。

## 5. 验证

| # | 判据 | 层 | 用例 |
|---|---|---|---|
| Q1 | `feature_catalog` 的缺省（含闭包）、implies、enables、description 与排序 | 单元 | `FeatureCatalog/*`（modules/manifest） |
| Q2 | `description` 在两种来源中被读取且为字符串，表形式不再报未知键；两种来源对同一陈述给出同一目录 | 单元 | `FeatureCatalog/*` |
| Q3 | 三种状态：内联形式为 `declared`/`index`；指针形式已安装为 `declared`/`payload`（含归档中 workspace 的继承），未安装为 `not-installed`，清单读不出为 `unreadable`，后两者 `list` 为 `null` | 单元、e2e | `PackageFeatures/*`；`index::features/992 …never installed`（search 前后各一次，中间以 `mcpp fetch` 安装） |
| Q4 | `mcpp why features`：路径依赖的 `list`、`active` 与描述，JSON 与文本 | e2e | `index::features/992 why features…` |
| Q5 | `--protocol-version` 列出 `package-features` 与 `mcpp.why.features` | e2e | `cli::golden/1123` |
