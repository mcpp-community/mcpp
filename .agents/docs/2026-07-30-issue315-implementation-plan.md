# 索引刷新策略收敛 — 实施计划

日期：2026-07-30
设计：`.agents/docs/2026-07-30-issue315-index-refresh-policy-design.md`
关联：#315
目标版本：**2026.7.30.3**（基线 `d79ab00` / 2026.7.30.2）
形态：**单 PR**，内部按 A–F 六段推进，每段自带验收。
状态：**已完成**（PR #320）。实施中偏离计划的地方记在 §G。

---

## 0. 基线核对（已完成）

| 事实 | 位置（@ d79ab00） |
|---|---|
| TTL 站点 | `src/build/prepare.cppm:1390-1433` |
| 每个依赖已自带**有序候选** | `DependencySpec.candidates`（`src/pm/dep_spec.cppm:46`）—— 与 `mcpp add` 用的是同一份 |
| 存在性查找 | `mcpp::pm::lookup_descriptor(route, candidates)`（`src/pm/index_route.cppm:150`） |
| route 工厂 | `prepare.cppm:1548`（`index_route(cfg)`）、`commands.cppm:151-152` |
| 版本可满足性 | `mcpp::pm::resolve_semver(ns, short, req, route, platform)`（`src/pm/resolver.cppm:96`），失败即不可满足 |
| 索引目录/marker/age | `xlings.cppm:381-490`（**匿名命名空间，未导出**） |
| 索引状态（已导出） | `default_index_status` / `official_index_status` → `IndexStatus{dir,present,fresh,ageSeconds}` |
| 索引内容 rev | `<indexDir>/.xlings-index-version`（实测 mcpplibs=`8d67478`，xim-pkgindex=`ebf4020`）—— **mcpp 零引用** |
| 跨平台锁原语 | `mcpp::platform::fs::FileLock::try_acquire(dir)`（`src/platform/fs.cppm:60`，`bmi_cache.cppm:241` 已在用） |
| 索引数据根（已导出） | `mcpp::xlings::paths::index_data(env)` |

---

## A. 探针（先跑，不写代码）

- **P-1 `.xlings-index-version` 更新语义**：clean-room `MCPP_HOME`，走一次真实 `xlings update`，
  比对刷新前后该文件的值与格式（是否 7 位短 sha、有无尾随空白、artifact/git 两通道是否都写）。
  **降级预案**：若某通道不写 → `index_revision()` 返回 nullopt，advisory 只显示 age，**gate 不受影响**。
- **P-2 INV-3 误报实测**：对一个同时含 mcpplibs 依赖与 xim 依赖的工程，打印每个依赖的
  `RefreshDecision`，确认 xim 落 `SuppressedInconclusive` 而非 `DescriptorMiss`。
  P-2 依赖 A 段代码，故实际在 B 段末尾以单测 + 一次真实工程 `-v` 运行完成。

**验收**：P-1 结论写回设计文档 §10；P-2 由单测 + e2e #6 承接。

---

## B. 策略模块（新增 `src/pm/index_refresh.cppm`）

**导出面**

```
enum class RefreshReason { None, IndexAbsent, DescriptorMiss, VersionMiss,
                           SuppressedDebounce, SuppressedOffline, SuppressedInconclusive };
struct RefreshPolicy   { bool offline; std::int64_t debounceSeconds = 120; };
struct RefreshDecision { bool shouldRefresh; RefreshReason reason; std::string subject; };

RefreshDecision decide_for_dependency(const IndexRoute&, const DependencySpec&,
                                      const xlings::Env&, const platform::PlatformKey&,
                                      const RefreshPolicy&);
std::expected<void,std::string> apply(const RefreshDecision&, const xlings::Env&, bool quiet);
RefreshPolicy policy_from_env(const config::GlobalConfig&, bool offlineFlag);
std::string_view reason_text(RefreshReason);
```

**判据顺序（短路，单测逐行锁）**

1. `spec.isPath() || spec.isGit()` → `None`
2. `policy.offline` → `SuppressedOffline`
3. 无候选路由到**内置 registry**（`find_for_ns` 为 null 或 `is_builtin`）→ `None`
   （项目 `path`/自定义 git 索引：刷全局索引对它无用，保持现状语义）
4. `lookup_descriptor` 未命中且 `!conclusive` → `SuppressedInconclusive`（**INV-3**）
5. 默认索引 `pkgs/` 不存在 → `IndexAbsent` → 刷
6. 未命中 → `DescriptorMiss` → 刷
7. 命中 + `is_version_constraint(spec.version)` + `resolve_semver` 失败 → `VersionMiss` → 刷
8. 上面判刷但 `0 <= age < debounceSeconds` → `SuppressedDebounce`

**依赖方向**：`index_refresh` → {`index_route`, `resolver`, `dep_spec`, `xlings`, `config`,
`platform.fs`, `ui`, `log`}。`resolver` 已 import `index_route`，无环。

**验收**：`tests/unit/test_pm_index_refresh.cpp` 覆盖 8 条判据 + 精确版本不判 VersionMiss。

---

## C. xlings 侧最小增补（`src/xlings.cppm`）

1. `IndexStatus` 增字段 `std::optional<std::string> rev`（读 `.xlings-index-version`，trim 空白；
   缺失/空 → nullopt）。→ `mcpp index status` 与 advisory 免费获得。
2. 新导出 `std::optional<std::string> index_revision(const std::filesystem::path& indexDir)`。
3. **S1**：`is_index_dir_fresh` 中 `age < 0` → 判 **stale**（未来时间戳不得等于永远新鲜）。
4. **S3**：`update_index` 内部用 `FileLock::try_acquire(paths::index_data(env))` 包裹；
   拿不到锁 → 记 verbose 日志并**返回 0（跳过，不报错、不阻塞）**。
5. **S4**：`mark_known_indexes_refreshed` 去掉 `projectDir` 早退，改为项目模式下也打标
   （仅供 advisory）。

**验收**：`tests/unit/test_xlings.cpp` 增负 age、rev 读取三态（缺失/空/带换行）。

---

## D. 五站点收敛

| 站点 | 动作 |
|---|---|
| `prepare.cppm:1390-1433` | 整段替换：遍历 `m->dependencies` 调 `decide_for_dependency`；命中即 `apply` 一次（进程内 `bool` once），reason 进 `-v` 日志 |
| `commands.cppm:167`（add） | 改为 `decide_*` + `apply`，删除本地 TTL 判断 |
| `xlings.cppm:1400`（xim 门） | 保留判据，`kJustRefreshedSeconds` 改由 `RefreshPolicy::debounceSeconds` 提供默认值，注释指向策略模块 |
| `package_fetcher.cppm:1060` | 保留，输出走统一句式 |
| `index_management.cppm:29`（search） | 保留 TTL，**加注释说明这是唯一允许时间驱动的站点** |

**验收**：`grep -n "is_index_fresh" src/` 只剩 search 一处 + xlings 内部实现。

---

## E. D6 + 表达面

1. **`mcpp update` 不再空转**（`commands.cppm:403-430`）：先强制刷索引（不看 TTL/debounce，
   `offline` 下拒绝并说明），再清 lock 条目，输出 `mcpplibs 8d67478 → a1b2c3d` 或 `already at …`。
2. **`--offline`**：`build` / `run` / `test` / `update` 注册；语义 = 本次调用不发起任何网络
   （索引 + 包 + 工具链自动安装）。
3. **`MCPP_OFFLINE=1`**：等价 env；`MCPP_NO_AUTO_INSTALL` 保留为兼容别名（等价于 offline 的
   工具链子集），help/docs 标注 deprecated。
4. **`[index] auto_refresh = true|false`**：`config.cppm` 读取，默认 true。
5. **可解释输出**：刷新一行说明主语与原因；`-v` 打印跳过原因与索引 rev/age；
   offline+miss 的结构化错误含 rev + age + `mcpp index update`。
6. `mcpp index status` 增 rev 列。

**优先级**：flag > env > config。

---

## F. 测试 + 版本 + 交付

- 新 e2e `tests/e2e/173_index_refresh_policy.sh`，9 个场景（设计 §8）。
  **反向断言写法**：`out=$(...); echo "$out" | grep -q X && { echo FAIL; exit 1; }`
  （`! cmd | grep` 在 errexit 下被豁免，永不失败）。
- 全量本地回归：`mcpp test`（单测）+ 受影响 e2e（09/14/44/105/169 等涉及索引/离线的用例）。
- 版本 `2026.7.30.3`：只改 `mcpp.toml` + `src/toolchain/fingerprint.cppm`；
  **bootstrap pin（`.xlings.json` / `ci-fresh-install.yml MCPP_PIN`）不动**。
  `bash .github/tools/check_version_pins.sh` 必须过。
- 单 PR → CI 全绿 → bypass squash 合入。
- 合入后：release 四平台 → 镜像 xlings-res（gh+gtc）→ xim-pkgindex PR → `xlings install mcpp`
  真装验证 → bootstrap pin bump（独立 commit）。

---

## G. 实施中相对本计划的偏离（都是实现时才看清的约束）

| 计划 | 实际 | 原因 |
|---|---|---|
| `--offline` 走 `BuildOverrides` 透传 | CLI 全局 flag → **写 `MCPP_OFFLINE` 环境变量**，各处按需读 | 消费者横跨 pm（索引/包）与 build（工具链），透传要改三个子系统的签名；env 又恰好让 `--offline` 与 `MCPP_OFFLINE=1` 成为字面上的同一个开关 |
| `offline_mode()` 放 `mcpp.pm.index_refresh` | 放 **`mcpp.platform.env`**（叶子模块） | `mcpp.pm.index_route` 已 import `mcpp.fetcher`，pm 里的 helper 会让 fetcher 反向依赖 pm ⇒ **模块环** |
| 判据顺序：offline 最先短路 | **本地分析在前，opt-out 在后** | 先短路会把「解析得好好的依赖」也报成 `offline mode`，而那恰恰是 offline 用户要确认的事。多出的成本只是本地文件读 |
| `decide_for_miss(policy, subject)` | 加 `env` 参数，**同样吃去抖** | 连着两次 `mcpp add` 打错字不该换来两次多仓库同步，理由与构建路径完全一致 |
| `mcpp update` 无条件强制刷新 | 工程里**没有走共享 registry 的依赖时跳过** | 纯 path/git 依赖的工程刷全局索引毫无意义；顺带让 e2e 23/24 不必平白联网 |
| `index_revision` 直接定义在实现区 | 拆成匿名命名空间里的 `read_index_revision` + 外层转发 | 实现区那一大段在**匿名命名空间**内，直接定义会得到内部链接 ⇒ 导出声明未定义（link 期才暴露） |
| P1 单列一期 | **全部并入 P0** | advisory、`index status` rev 列、S4 打标、docs 都只有几十行，分期反而让「变懒」上线时缺少配套的可观测性 |
| 计划未列 | 追加 `mcpp why deps` 的 `package index: <rev> (<age>)` 行 | 刷新变懒后，「为什么解析到这个版本」的答案常常是「因为它是你本地索引已知的最新版」 |

## 风险与回退

| 风险 | 处置 |
|---|---|
| INV-3 接错 → 每次构建都刷 | 单测 + e2e #6 双闸；P-2 真实工程实测 |
| `.xlings-index-version` 语义不符 | gate 不依赖 rev，最坏只损失 advisory 精度 |
| S3 锁在 Windows 行为不一致 | 走 `platform/fs.cppm` 封装；拿不到锁=跳过而非阻塞 |
| 语义变化引发「拉不到新版本」误报 | `mcpp update` 变成真入口 + advisory 提示 + docs 明写 |
