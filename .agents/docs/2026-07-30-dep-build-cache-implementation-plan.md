# 依赖构建产物的全局缓存收敛 — 实施计划

配套设计：`2026-07-30-dep-build-cache-scoping-design.md`
前置：issue311 / PR#312（`mcpp stage` 原语 + `stage_file` rule + `restat = 1` + `mcpp.home`）
**已合入 main**（`b794856`，2026.7.30.1），故本计划直接复用，不再自建 staging 原语。
建议目标版本：**2026.7.30.2**（常规迭代，`.0` 保留给稳定版）

单 PR 交付。阶段有硬依赖顺序：**A（正确性）→ B（换键）→ C（stage 边）→ D（模式与运维）**。
A 必须先合入的判据见设计 §5：C 让 ninja 第一次真正接受缓存产物，A 未落地时那等于把
"白重编一遍但结果对" 变成 "直接用错对象"。

---

## 落地前已核实的现状事实（避免设计→实现的重映射错误）

| 事实 | 位置 | 对实现的意义 |
|---|---|---|
| `stage_file` rule 已存在，命令 `$mcpp stage $verify --output $out $in`，带 `restat = 1` | `ninja_backend.cppm:456-459` | C 阶段直接复用，`$verify` 用 `size`（依赖缓存条目是 key-scoped，等长即等价，与 std 边同理） |
| `$mcpp` 变量绑定已提到 `if (dyndep)` 之外 | `ninja_backend.cppm:427` | 不需要再动 |
| **restat 与对齐 mtime 互斥**（PR#312 的教训：对齐 mtime 会重新级联） | 同上注释 | `stage_into` 里的 `touch_now` 在 C 阶段要删掉，改由 restat 负责 |
| `DepCacheIdentity` 已有 `{indexName, packageName, version}`，两个 push 点 | `prepare.cppm:1375-1380` / `:2684` / `:2979` | A 阶段加 `sourceKind` 字段即可，**不需要**按路径前缀猜来源 |
| `sourceKind`（`"version"｜"path"｜"git"`）在两个 push 点都在作用域内 | `prepare.cppm:2699` / `:2982` | S9 的判据是现成的 |
| `dependencyEdges`（`consumerPackageIndex → dependencyPackageIndex` + visibility） | `prepare.cppm:2059-2073` | B 阶段 F 轴（Merkle）直接遍历它，不需要新建图 |
| `CompileUnit` 已带 `packageName` + per-package flags/include dirs | `plan.cppm:20-36` | B 阶段按包分组是现成的；C 阶段加标记字段 |
| `report.topoOrder` 已有拓扑序 | `prepare.cppm:3561` | B 阶段自底向上算 Merkle 键一次遍历 |
| `metadata_for()` 已是完整正确的 std 身份（15 字段） | `stdmod.cppm:88-112` | B 阶段只改目录名，不重新定义身份 |
| `mcpp::home::bmi_root()` 已是单一解析器 | `src/home.cppm` | 新缓存根走它，不再碰 `default_cache_root()` 的旧兜底 |
| 版本号两处 | `mcpp.toml:3`、`src/toolchain/fingerprint.cppm:21` | `.github/tools/check_version_pins.sh` 机器校验 |

---

## 阶段 A — 正确性前置

### A1 · profile 进失效轴

`prepare.cppm:canonical_compile_flags()`（:209-265）末尾追加：

```cpp
s += " opt="   ; s += m.buildConfig.optLevel;
s += " debug=" ; s += m.buildConfig.debug ? "1" : "0";
s += " lto="   ; s += m.buildConfig.lto   ? "1" : "0";
s += " strip=" ; s += m.buildConfig.strip ? "1" : "0";
```

调用点在 profile 解析（:776-809）之后，无需移动。

**副作用（写 CHANGELOG）**：三个 profile 从此各占一个 `target/<triple>/<fp>/`，互不覆盖
（cargo 行为）。代价是磁盘 × profile 数——每个 build dir 带一份 staged std BMI
（本机 `std.gcm` = 31,466,112 B）。

### A2 · `.build_cache` 按 profile 区分（C3b，今天在生效的 bug）

`BuildCacheEntry` 加 `std::string profile;`（`execute.cppm:35-58`）。

- 序列化：P3 格式加一行 `profile=<name>`；**旧条目缺该字段 ⇒ 读成空 ⇒ 与任何 resolved
  profile 都不匹配 ⇒ 当 miss**（自愈，不需要迁移代码）。
- `write_build_cache` 的去重键：`(targetTriple, profile)`（:143-145）。
- `try_fast_build` / `try_fast_run` 需要知道"本次请求的 resolved profile"。但 fast path 的
  存在意义就是**不跑 prepare_build**，而 profile 解析在 prepare_build 里。
  ⇒ 解析规则本身是纯的（`overrides.profile > [build].default-profile > "dev"`，
  `prepare.cppm:789-791`），把它抽成一个 `mcpp::build::resolve_profile_name(
  const Manifest&, std::string_view override)` 供两侧共用。fast path 只需 load manifest
  （它已经 load 了，用于 staleness 检查）。
- 顺手修 `ui::finished("release", ...)`（`execute.cppm:346`）硬编码 → 传 resolved profile。
  它在 dev 构建下也打 `Finished release [optimized]`，是 E8 里最误导人的一行。

### A3 · fingerprint-changed 警告降噪

`execute.cppm:310-324`。条目里现在有 profile 了，所以能判断"fp 变化是否伴随 profile 变化"：
profile 也变了 ⇒ 不打这条（这是预期的、用户自己要求的），只在 profile 相同而 fp 变时打。

### A4 · 本地来源包排除出缓存（S9）

1. `DepCacheIdentity` 加 `std::string sourceKind;`，两个 push 点（:2684 用字面
   `"version"`，:2979 用作用域内的 `sourceKind`）填上。
2. 缓存循环（:3758-3830）的 `skipCache` 谓词整体替换：

```cpp
// 旧：查 root manifest 的 dependencies/devDependencies（传递依赖查不到 ⇒ 误缓存）
// 新：唯一判据 —— 该包必须来自 index（不可变的 xpkgs store）
if (!depIdent || depIdent->sourceKind != "version") continue;
```

   这同时干掉 `indexName` 回落到 `defaultIndex` 的误挂（E6：本地包 `B` 被挂到
   `mcpplibs` 名下）——`depIdent` 为空时直接不缓存，回落分支消失。
3. 判据方向说明写进代码注释：与 `mcpp add` 存在性门的"不可证伪就放行"**相反**，
   缓存要"不能证明来自不可变 store 就不缓存"。缓存的错误代价是静默错产物。

### A5 · 条目自描述 + 命中逐字段校验（S2）

`bmi_cache.cppm`：

- `CacheKey` 加 `std::string keyHex;`（B 阶段填 Merkle 键）与 `nlohmann::json inputs;`。
- 新 `entry.json`（取代 `manifest.txt` 作为 sentinel，`manifest.txt` 保留写出以便人读）：

```json
{ "schema": 1, "epoch": 1, "key": "<key16>",
  "inputs": { ...键的全部输入... },
  "bmi": ["A.gcm"], "obj": ["a.o"],
  "created": "<iso8601>", "accessed": "<iso8601>" }
```

- `is_cached(key)` ⇒ `entry.json` 存在 ∧ `schema/epoch` 匹配 ∧ **`inputs` 逐字段等于本次
  算出的 inputs** ∧ 清单文件全部存在。字段不等 = miss，**永不"信任 hash 相等"**
  （照抄 `stdmod.cppm::metadata_matches` 的模式）。
- 写序：产物 → `entry.json.tmp` → rename（sentinel 最后）。沿用已有的
  `FileLock::try_acquire`。
- 命中时更新 `accessed`（只重写 `entry.json`，**不动产物 mtime**）——这是 D 阶段
  `cache gc` 能做真 LRU 的前提。

### A6 · 阶段 A 的测试

单测（`tests/unit/test_bmi_cache.cpp` 扩写，`tests/unit/test_fingerprint.cpp` 扩写）：

| 用例 | 断言 |
|---|---|
| profile 进指纹 | 同一 manifest，optLevel/debug 不同 ⇒ `canonical_compile_flags` 串不同 ⇒ fp 不同 |
| `resolve_profile_name` 优先级 | override > default-profile > "dev" |
| `.build_cache` 往返 | 写 (triple="", profile="dev") 与 (triple="", profile="release") ⇒ **两条并存** |
| 旧格式条目 | 无 `profile=` 行 ⇒ 读出空 profile ⇒ 任何 resolved profile 都 miss |
| `entry.json` 校验 | inputs 改一个字段 ⇒ `is_cached` 为 false |
| `accessed` 更新 | `stage_into` 后 `accessed` 变新、产物 mtime 不变 |

e2e（新文件 `tests/e2e/1NN_build_cache_correctness.sh`）：

- **负例**：`mcpp build --dev` → `mcpp build --release`，断言 release 构建里依赖**有**编译边。
- **负例（E8 回归闸）**：`mcpp build --release` → 裸 `mcpp build`，断言产物是 dev
  （`readelf --debug-dump=info` 有 `-O0`／`DW_AT_producer` 含 `-g`），且**不是** 0.00s 假成功。
- **负例（E6 回归闸）**：root → A{path} → B{path}，构建后断言 `mcpp cache list` 里**没有** B。

---

## 阶段 B — 换键

### B1 · 新模块 `src/build/cache_key.cppm`

```cpp
export module mcpp.build.cache_key;
import std; import mcpp.libs.json; import mcpp.toolchain.detect;
import mcpp.toolchain.fingerprint;   // hash_string

export namespace mcpp::build::cache_key {
inline constexpr int kCacheEpoch = 1;   // bump ONLY on cache-incompatible changes

struct PackageKeyInputs {              // 逐轴，A..G（设计 §4 S1）
    // A 工具链 / B 语言方言 / C profile / D 包身份 / E 该包自身配置 / F 上游键 / G epoch
    ...
};
nlohmann::json to_json(const PackageKeyInputs&);   // 进 entry.json，供逐字段校验
std::string hex16(const PackageKeyInputs&);        // 目录名
}
```

`hex16` 复用 `mcpp::toolchain::hash_string`（fnv1a-64 → 16 hex），与既有指纹同族。

**依赖图无环性自查（改前必做）**：`mcpp.build.cache_key` 只 import `std` /
`mcpp.libs.json` / `mcpp.toolchain.{detect,fingerprint}`，均是叶或已在 `mcpp.build.plan`
的闭包内 ⇒ `mcpp.build.prepare → mcpp.build.cache_key` 不成环。

### B2 · 在 prepare 里自底向上算全图键

位置：缓存循环之前（当前 :3725 之前），拓扑序遍历 `report.topoOrder`／`packages`：

```
for pkgIndex in 自底向上顺序:
    inputs = 收集 A..E 轴(pkgIndex)
    inputs.upstream = sorted( key(dep) for dep in dependencyEdges[pkgIndex] )   // F 轴
    key[pkgIndex] = hex16(inputs)
```

- A/B/C 轴对全图相同，一次算出复用。
- E 轴取 `packages[pkgIndex].manifest.buildConfig` + `privateBuild/publicUsage` 的
  include dirs。**include dirs 必须相对 xpkgs store 根归一**（它们是
  `<mcppHome>/registry/data/xpkgs/...` 的绝对路径，不归一则 `MCPP_HOME` 一换全 miss）。
- `sources` 清单取相对包根、排序后的相对路径（`CompileUnit::source` 按 `packageName`
  分组即可）。
- F 轴：`dependencyEdges` 里 `consumerPackageIndex == pkgIndex` 的所有边的
  `dependencyPackageIndex` 的键，排序后拼接。**环**：modgraph 已拒环（`validate`），
  但仍加一个访问集断言，成环时 `return std::unexpected(...)` 而不是无限递归。

### B3 · 缓存布局换到 `cache/v1/`

`bmi_cache.cppm::CacheKey::dir()`：

```cpp
- return mcppHome / "bmi" / fingerprint / "deps" / indexName / "<pkg>@<ver>";
+ return mcppHome / "cache" / "v1" / "pkg" / indexName / "<pkg>@<ver>" / keyHex;
```

`fingerprint` 字段删除。旧 `$MCPP_HOME/bmi/<fp>/deps/` **不读不写不删**，由 D 阶段的
`doctor` 提示 + `cache clean --legacy` 处理。

### B4 · std BMI 换键（S3）

`stdmod.cppm`：

```cpp
- sm.cacheDir = cache_root / std::string(fingerprint_hex);
+ sm.cacheDir = cache_root / "std" / std_identity_key(tc, cpp_standard, cpp_standard_flag,
+                                                     macos_deployment_target);
```

`ensure_built` 的 `fingerprint_hex` 形参删除（唯一调用点 `prepare.cppm:3612`）。

⚠️ **自指陷阱（必须处理）**：`metadata["std_build_commands"]` 里含 `sm.cacheDir` 的绝对
路径，而 cacheDir 又由 metadata 算出。做法：先用占位 `<CACHEDIR>` 生成一份"规范化
metadata"算键，再用真 cacheDir 生成落盘的 metadata。落盘的 `metadata_matches` 逻辑不变。

⚠️ `cache_root` 从 `default_cache_root()`（= `home::bmi_root()` = `<home>/bmi`）改为
`<home>/cache/v1`，需同步 `maintenance.cppm` / `doctor.cppm` / `clean --bmi-cache` 三处
读点。

### B5 · epoch 取代 MCPP_VERSION（S5）

`fingerprint.cppm` 的 `fp.parts[7] = MCPP_VERSION` **保持不变**（build dir 命名 + 本地增量
归它管，保守）。缓存键的 G 轴用 `kCacheEpoch`，写进 `entry.json`；读到不同 epoch 当 miss。

### B6 · 阶段 B 的测试

单测（新 `tests/unit/test_cache_key.cpp`）：

| 用例 | 断言 |
|---|---|
| root 身份不影响依赖键 | 只改 root 包名/版本 ⇒ 依赖包的键**不变** |
| root flags 不影响依赖键 | 只改 root `[build] cxxflags` ⇒ 依赖包的键**不变** |
| 无关兄弟依赖不影响 | 图里新增一个与之无边的包 ⇒ 原包的键**不变** |
| profile 影响 | optLevel 变 ⇒ 键变（C 轴） |
| 该包自身 flags 影响 | 该包 cflags 变 ⇒ 键变（E 轴） |
| 上游变化级联 | 上游包的 flags 变 ⇒ 下游包的键**也变**（F 轴 Merkle） |
| include dirs 归一 | 换 `MCPP_HOME` 前缀 ⇒ 键不变 |
| std 身份键 | 同 (compiler, version, triple, stdlib, std_flag, 源 hash) ⇒ 同键；任一变 ⇒ 键变；`<CACHEDIR>` 占位化生效（两个不同 cacheDir 得同键） |

---

## 阶段 C — 命中的依赖发 stage 边

### C1 · plan 侧标记

`plan.cppm::CompileUnit` 加：

```cpp
bool                  servedFromCache = false;
std::filesystem::path cachedObject;   // 缓存内绝对路径
std::filesystem::path cachedBmi;      // 缓存内绝对路径（有模块时）
```

prepare 的缓存循环命中时，给该包的所有 CU 打上标记并填缓存路径（不再调用
`stage_into` 做进程内拷贝——staging 交给 ninja 边）。

### C2 · ninja 侧分流

`ninja_backend.cppm`：

- P1689 scan／dyndep 的 CU 列表（:850-905 收集 `ddi_paths`）**过滤掉** `servedFromCache`；
- compile 边（:913-1010 两条分支，dyndep / 非 dyndep）**跳过** `servedFromCache`；
- 改发（每产物一条）：
  ```
  build gcm.cache/<mod>.gcm : stage_file <cachedBmi>
    verify = --verify size
  build obj/<rel>.o         : stage_file <cachedObject>
    verify = --verify size
  ```
  `--verify size` 的判据与 std 边同源：条目目录名就是覆盖了工具链/方言/profile/包配置/
  上游闭包的键，同键 ⇒ 等长即等价（PR#312 注释里已把这条判据写清）。
- 消费者 TU 的 implicit input、链接行的对象列表、`bin/` 部署边**都不变**（产物落在原位置）。
- `plan.compileUnits` 保留这些 CU ⇒ `compile_commands.cppm:127-170` 仍为依赖发条目，
  clangd/IDE 不退化。

### C3 · `stage_into` 与 `populate_from` 的调整

- `stage_into` 只在 `--cache=global` 且 **C 阶段之外的调用者**（无）使用 ⇒ 删除进程内
  staging，改为返回清单供 C1 填路径。函数改名 `resolve_cached(key)`，语义变成"读清单
  + 校验，不落盘"。
- `populate_from` 保留：未命中的包在 build 成功后回填。删掉 `touch_now`
  （restat 与对齐 mtime 互斥，PR#312 的教训）。

### C4 · 状态行说真话

`cachedDepLabels` 带上省下的 unit 数：`Cached compat.zlib v1.3.2 (16 units)`。
假状态行能骗人三个月，带计数就骗不了。

### C5 · 阶段 C 的测试

单测（`tests/unit/test_ninja_backend.cpp` 扩写）：

| 用例 | 断言 |
|---|---|
| 命中包不发 compile 边 | `servedFromCache` 的 CU ⇒ `build.ninja` 里无其 compile 命令，有 `stage_file` 边 |
| 命中包不发 scan 边 | 无其 `.ddi` |
| `verify = --verify size` | stage 边带该绑定 |
| 未命中包不变 | 无标记 ⇒ 文本与今天逐字节一致（归一化 diff） |

e2e（新文件 `tests/e2e/1NN_build_cache_crossproject.sh`）——**本设计的核心断言**：

1. clean-room `MCPP_HOME`；工程 P1（含一个 index 依赖）构建 → 条目落地；
2. 工程 P2（**不同包名、不同版本**、同一依赖）构建 ⇒ 依赖的 compile 边数 **= 0**，
   `stage_file` 边数 > 0。断言方式：读 `build.ninja`／`ninja -d explain`
   的结构化输出，**不 grep 全日志**；
3. P1 `mcpp version bump` 后重建 ⇒ std 与依赖**均命中**；
4. 依赖版本改变 ⇒ 键变 ⇒ 重编（正例，防"永不重建"）。

---

## 阶段 D — 模式与运维

### D1 · `--cache=global|local|off`（S6）

- `cli.cppm`：`build` 加 `--cache <MODE>`；`--no-cache` 保留为 deprecated 别名并修正 help
  文案（当前写的是 "Force-clear target/ before building"，名不副实）。
- `run`/`test` 也加（今天它们连 `--no-cache` 都没有）。
- 优先级：CLI > `MCPP_BUILD_CACHE` > `[build] cache` > `global`。
- `manifest.cppm`：`[build] cache` 字段 + 未知值的 warning（`--strict` ⇒ error）。
- `BuildOverrides` 加 `std::string cache_mode;`；`prepare_build` 里 global 之外**不读**
  缓存，`run_build_plan` 里 global 之外**不写**；`off` 额外 `remove_all(outputDir)`。

| 模式 | 读 | 写 | 先清 target/ |
|---|---|---|---|
| `global`（默认） | 是 | 是 | 否 |
| `local` | 否 | 否 | 否 |
| `off` | 否 | 否 | 是 |

### D2 · `mcpp cache` 补齐（S7）

`maintenance.cppm` 走新布局 `cache/v1/`，并新增：

| 命令 | 实现要点 |
|---|---|
| `cache dir` | 打印 `home::root() / "cache" / "v1"`。今天 `cache*`/`doctor`/`clean --bmi-cache` 与 config 重置路径可能不是同一个目录，这条让它永远可验证 |
| `cache gc [--max-size N{MiB,GiB}] [--older-than N{s,m,h,d}]` | 按 `entry.json.accessed` 真 LRU（A5 已保证它被更新）。`--max-size` 从最旧开始删到达标 |
| `cache clean [--deps\|--std\|--all\|--legacy]` | 现在只删 deps 且第一行 `remove_all(bmi/"deps")` 是死代码（deps 在 `bmi/<fp>/deps`）。`--legacy` 删旧 `<home>/bmi/` |
| `cache list --json` | 结构化输出；`list` 增列 `key16` |
| `cache verify` | 逐条目校验 `entry.json` ↔ 磁盘清单，报孤儿/残缺 |

`clean --bmi-cache` 文案改为指向 `cache clean --all`，行为保持。
`doctor`：发现旧 `<home>/bmi/` 时报一行大小 + `mcpp cache clean --legacy`（不自动删）。

### D3 · 阶段 D 的测试

| 用例 | 断言 |
|---|---|
| 模式优先级 | CLI > env > manifest > 默认（单测直接测解析函数） |
| `local` 不写缓存 | clean-room 下 `--cache=local` 构建后 `cache list` 为空 |
| `off` 清 target | 构建产物先存在，`--cache=off` 后 build dir 被重建 |
| `--no-cache` 别名 | 与 `--cache=off` 行为一致 |
| `gc --max-size` | 造 3 条不同 `accessed` 的条目 ⇒ 删到达标，保留最新 |
| `cache verify` | 手动删一个产物 ⇒ 报残缺并非零退出 |

---

## 阶段 E — 收尾

1. **版本号**：`mcpp.toml:3` + `src/toolchain/fingerprint.cppm:21` → `2026.7.30.2`；
   跑 `bash .github/tools/check_version_pins.sh`。
   （bootstrap pin `.xlings.json` / `MCPP_PIN` **不动** —— 它是自举起点，不随发布走。）
2. **CHANGELOG.md**：三段——(a) 全局缓存此前净收益为零（附 ninja `-d explain` 证据）；
   (b) profile 现在是失效轴，`target/` 下每 profile 一个目录、磁盘上升；
   (c) 旧 `<home>/bmi/` 一次性弃用，`mcpp cache clean --legacy` 可回收（本机 26 GB）。
3. **docs/**：`docs/26-bmi-cache.md` 同步新布局与键定义（`bmi_cache.cppm` 头注释指向它）。
4. **自举验证**：`mcpp build`（release profile）+ `mcpp test` 全绿；
   用**新构建出的** `mcpp` 二进制跑一遍 e2e，而不是 bootstrap 的那份。
5. **PR**：单 PR，标题带版本与 issue 号；CI 全绿后 squash 合入。
6. **xlings 全生态验证**：release → 镜像 xlings-res（gh + gtc 双端）→ xim-pkgindex PR →
   clean-room `XLINGS_HOME` 真装 → 跑 e2e → bump bootstrap pin。

---

## 实施结果与计划的偏差（实现后回填）

实现于 2026.7.30.2。以下是与本计划不同的地方，以及原因。

| 项 | 计划 | 实际 | 原因 |
|---|---|---|---|
| 缓存根 | `$MCPP_HOME/cache/v1` | **`$MCPP_HOME/build-cache/v1`** | `cache` 这个名字已归 `GlobalConfig::metaCacheDir`（索引元数据）所有，而 `config.cppm:222` 的 reset 路径 `remove_all` 整个目录。编译产物塞进别人会清空的目录里，会以「构建缓存有时会自己空掉」的形态浮现 |
| `CacheKey` 字段 | `mcppHome` + 内部拼 `cache/v1` | **`cacheRoot`（调用方传 `mcpp::home::cache_root()`）** | 布局根必须只有一份定义；在 `bmi_cache.cppm` 里再写一遍 `"build-cache"/"v1"` 正是注释管不住的跨文件不变量 |
| `stage_into` | 改名 `resolve_cached`，返回清单 | 同（且 `touch_now` 删除） | 计划一致。restat 与对齐 mtime 互斥（PR#312 的教训） |
| epoch 覆盖 std | G 轴含 `kCacheEpoch` | **std 键不含 epoch** | std 键放在 `stdmod.cppm`（与 `metadata_for` 同处，内聚更好），不引入 `stdmod → build.cache_key` 的跨层依赖。std 的格式兼容由 `build-cache/v1` 路径段承载 —— 一份 std BMI 的有效性取决于工具链，不取决于 mcpp 的缓存簿记 |
| A2 的范围 | `.build_cache` 加 profile | **另加 P2/P3 两项** | 光加字段不够：fast path 的准入只挡显式 `--profile`，挡不住裸 `mcpp build`，所以必须同时让 fast path 自己解析 profile（新 `resolve_profile_name`）。另修 `ui::finished` 的硬编码 `"release" [optimized]` |
| `finished()` 签名 | 不在计划内 | 加可选 `descriptor` 形参 | `[optimized]` 也是硬编码的；描述符由真正解析了 profile 开关的调用方给出，fast path 不给（它没解析过），所以没有调用方需要编造 |
| e2e 文件 | `1NN_*` 两个 | **172 / 173 / 174 三个** | 模式与运维命令的面足够大，值得独立文件；另改写了既有的 19（升级为传递 path 依赖的负例）与 49（改读 `entry.json` + 断言无 compile 边） |
| `--cache=off` 的语义 | 「清 target/」 | **清本次的 `target/<triple>/<fp>/`** | 这本来就是 `--no-cache` 的既有行为；旧 help 文案「Force-clear target/」两处不准（不是整个 target/，也与缓存无关），一并改正 |
| S8 `-ffile-prefix-map` | 阶段 3 | **未实施** | 它的收益是为内容寻址去重铺路，而本批不做内容寻址；且改变依赖对象的 debug 路径会让 gdb 需要 `set substitute-path`，是可感知的调试体验回退。留作独立批次 |
| 零拷贝 | 阶段 3 | **未实施** | 与 issue311 §7 合并，需要 GCC 的 `-fmodule-mapper` 与四平台 e2e |

**未实施项的处置**：S8 与零拷贝均记在设计文档 §7 的后续批次里，不在本批 CHANGELOG 中承诺。

### 顺带发现、本批**未**处理的一个同类隐患（记录待办）

fast path 重放 `build.ninja` 的前提是「本次请求会生成同一张图」。本批把 **profile** 与
**cache mode** 两个轴记进了 `.build_cache` 并要求匹配，但**图形状还受若干环境变量影响，
它们都不在任何记录里**：

| 输入 | fast path 是否能发现变化 |
|---|---|
| `--profile` / `--dev` / `--release` | ✅ 记录 + 匹配（本批） |
| `--cache` / `[build] cache` / `MCPP_BUILD_CACHE` | ✅ 记录 + 匹配（本批） |
| `--target` / `-p` / `--features` / `--cap` / `--static` / `--strict` | ✅ 显式 flag 直接绕过 fast path |
| 改 `mcpp.toml` | ✅ mtime 比 `build.ninja` 新 |
| `MACOSX_DEPLOYMENT_TARGET` | ❌ 进 fingerprint，但 fast path 只校验「记录的 fp 与目录名自洽」，不重算 fp |
| `MCPP_VERIFY_MODGRAPH` / `MCPP_SCANNER` | ❌ 改变 scan/dyndep 边的形状 |

这三个都是**本批之前就存在**的（fast path 从来没有校验过它们），且都需要「重算一次 fp 才能
比对」——那与 fast path 存在的意义（不跑 prepare_build）直接冲突。正解是给 fast path 一个
**廉价的图身份**：把这些输入（以及 profile/cache mode）折进一个短哈希写进 `.build_cache`，
匹配即重放。那是一次独立的收敛，不该塞进本批。

判据同本批：`.build_cache` 的条目必须自证「我是在什么条件下生成的」，否则「条件变了」与
「条件没变」在读取端不可区分 —— 这正是 profile 那个 0.00s 假成功的成因。

## 已知坑（来自本仓库既有教训，实现时逐条对照）

| 坑 | 规避 |
|---|---|
| 指纹目录随版本变，`ls target/<triple>/ \| head -1` 会自查到旧二进制 | 测试里用 `mcpp build --print-fingerprint` 拿 fp，或对目录按 mtime 排序 |
| `! cmd \| grep` 在 `errexit` 下被豁免、永不失败 | 负例断言不要写成这种形状；用显式 `if grep -q ...; then exit 1; fi` |
| e2e `_inherit_toolchain.sh` 写穿符号链接会损坏真实 payload | 新 e2e 不 `ln -sf` 真实 payload；只用 clean-room `MCPP_HOME` + 工程内 `path` 索引 |
| 本机 `~/.mcpp` 有 26 GB / 1198 个旧指纹目录 | 所有新 e2e 用 clean-room `MCPP_HOME`，否则计时与断言全被污染 |
| 断言读全日志 grep 易假绿 | 读 `build.ninja` / `resolution.json` / `cache list --json` 等结构化载荷 |
| ninja `restat` 与对齐 mtime 互斥 | `populate_from`/`resolve_cached` 都不要 touch 产物 mtime |
| 描述符 Lua 里的跨包字面地址会随布局变化失效 | 本次不动包布局，但 `check_cross_package_refs.lua` 仍要过 |
