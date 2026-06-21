# mcpp 后续修复:统一汇总 + 方案设计 + PR 拆分

> 状态:设计 / 待实施
> 前置:`mcpp-index#43`(xcb 配方)与 `mcpp#137`(InstallStash 回滚)**已合入**。
> 范围:仅 **mcpp 仓库**的遗留项(xim-res 打包类 T-a/k/l 见 §6,不在此)。
> 关联:根因分析见 `2026-06-21-xcb-and-install-integrity-cross-repo-fix.md`。

本次把今天讨论中**未做的 mcpp 相关项**全部汇总,逐项给出根因(带 `file:line`)、方案、风险、测试,最后给 **PR 拆分建议**。

---

## 1. 待办项总览(mcpp 仓库)

| ID | 项 | 子系统 | 风险 |
|---|---|---|---|
| T-b | bootstrap/装包进度反馈(现 `>/dev/null` 静默,假死在 "Bootstrap ninja") | 首跑/xlings | 低 |
| T-c | 首个 install 前显式 `Fetching package index…` | 首跑/xlings | 低 |
| T-d | 镜像默认 `CN` 首跳慢/不稳;联网无超时/重试 | 首跑/xlings | 中 |
| T-e | `.mcpp_ok` 写入前校验产物清单;新装禁用 `looks_complete_legacy` 打标记 | 安装完整性 | 中 |
| T-f | `mcpp self doctor` 扫工具链 RUNPATH / subos 悬空软链 | 安装完整性 | 低 |
| T-g | ABI 报错可操作化(指出工具链来源,给对的命令) | 工具链解析 | 中 |
| T-h | `toolchain default`/`list *` 与构建实际解析不一致 | 工具链解析 | 中 |
| T-i | 模块扫描器把字符串字面量里的 `import` 当真(imgui 误报) | 构建图 | 低 |
| T-j | registry 依赖被重装后 build.ninja 陈旧 → `missing and no known rule` | 构建图/缓存 | 中高 |

---

## 2. 逐项方案(含代码定位)

### T-b/T-c/T-d — 首跑 bootstrap UX

**根因**
- `install_with_progress()`(`src/xlings.cppm:870-899`)**优先走直装路径** `xlings install … >/dev/null 2>&1`(`silent_redirect`,`src/platform/shell.cppm:30`),成功即返回;能渲染进度的 NDJSON 回调路径只是**失败兜底**,正常路径永不触发 → 全程无输出、`std::system` 阻塞、屏幕定格在最后印出的 "Bootstrap ninja"。
- 镜像默认 `CN`:`seed_xlings_json(..., mirror="CN")`(`src/xlings.cppm:236-238`,带 `TODO(mirror-default)` 注释 `:223-235`)。mcpp 这层**无重试/超时**,联网延迟方差直接变"假死"时长。

**方案**
- **T-b**:交互式 bootstrap/toolchain 安装**以 NDJSON 进度路径为主**(`install_with_progress` 调换顺序:先 NDJSON 拿 `download_progress` 回调,直装退为兜底);退一步至少在 `std::system` 阻塞期间打 **spinner + elapsed 计时器**。
- **T-c**:首个真正 `xlings install` 前显式 `print_status("Fetching", "package index (one-time)")`(`ensure_init`/`ensure_ninja` 附近,`src/xlings.cppm:1026-1108`)。
- **T-d**:`seed_xlings_json` 默认镜像改为**自动探测**(按 `LANG`/对 github vs ghproxy 一次 tight-timeout HEAD 探测,见现有 TODO 的 (b));并给联网步骤加超时+有限重试。

**风险**:低(T-b/c 纯输出)~ 中(T-d 改默认镜像行为,需保留 `mcpp self config --mirror` 覆盖)。
**测试**:首跑录屏/日志快照;`--mirror CN/GLOBAL` 两路冒烟;离线/慢网模拟下不再无限静默。

---

### T-e/T-f — 安装完整性加固(#137 的延续 P2-b/P2-c)

**根因**
- 标记写入只证"进程退出/布局像"非"产物正确":`mark_install_complete` 在 `inst->exitCode==0 && verdir 存在`(`src/pm/package_fetcher.cppm:740-743`)或 copy 兜底的 `looks_complete_legacy` 布局启发式(`:754-758`)时就写。
- doctor 现有 `doctor_report()`(`src/doctor.cppm:46+`,有 `ok/warn/err` 助手)未覆盖工具链 RUNPATH / subos 悬空软链(zlib 被删后 `subos/default/lib/libz.so.1` 悬空就是没人体检到)。

**方案**
- **T-e(a)**:配方可声明 `verify = { "include/xcb/xproto.h", ... }`(产物相对 install dir 的必存路径)。解析挂在 `src/manifest.cppm`(现有 `sources`@1851 / `generated_files`@1882 / `targets`@1903 处加 `verify` key);`mark_install_complete` 前逐一 `exists` 校验,缺则判失败、清理、不写标记。
- **T-e(b)**:**新装路径禁用 `looks_complete_legacy` 打标记**(`package_fetcher.cppm:754-758` / `779-784` 的 copy 兜底分支)——布局启发式只应用于"老包一次性收编",不能作为刚装完的完整性判定。
- **T-f**:`doctor_report()` 加一节:遍历已安装工具链二进制,`readelf -d` 取 RUNPATH,校验每个目录存在;扫 `subos/default/lib` 软链是否悬空;发现即 `warn` + 可操作提示(指向缺失的 `xim-x-*` 包)。

**风险**:中(T-e 触及安装标记,与 #137 同区,需回归既有安装/copy-fallback 路径);低(T-f 只读体检)。
**测试**:gtest——配方声明 verify 但产物缺失→install 判失败、不写 `.mcpp_ok`;copy-fallback 不再凭布局打标记;doctor 能报出被删的 `xim-x-zlib`。复用 #137 的 `tests/unit/test_install_integrity.cpp`。

---

### T-g/T-h — 工具链解析透明化 & ABI 可操作报错(同根,合一)

**根因(关键:多源真相)**
构建期解析顺序(`src/build/prepare.cppm`,后者覆盖前者):
1. `:487` 项目 `mcpp.toml [toolchain]`(`manifest.cppm:91 for_platform`)——**最先,直接 shadow 全局**;
2. `:490-491` 全局 `config.toml [toolchain] default`(`config.cppm:501`);
3. `:505` / `:516-520` `[target.<triple>]` 覆盖 + `*-musl` 约定**硬编码** `gcc@15.1.0-musl`;
4. `:595-596,648-651` 内建默认 `gcc@16.1.0`(仅首跑分支,且会持久化)。

而 `mcpp toolchain default <spec>`(`src/toolchain/lifecycle.cppm:420,430`)和 `list` 的 `*` 标记(`lifecycle.cppm:180-181,195`,`matches_default_toolchain` `registry.cppm:190-199`)**只读/只写全局 config.toml**,完全不看项目 `[toolchain]`/`[target.*]`。
→ 于是 `default` 报 `gcc@16.1.0`、`list *` 跟全局,而 `run` 实际可能走项目/target 覆盖的 `gcc@15.1.0-musl`,**三者各执一词**。

ABI 报错(`prepare.cppm:2453-2458`)已建议 `mcpp toolchain default <glibc>`,但当 musl 来自 `[target.*]`/`*-musl` 约定时,**这条命令改不了结果**(target 覆盖优先级更高)——所以是"误导性建议"。

**方案**
- **T-h**:抽出**单一"有效工具链解析"函数**(输入:cfg + manifest + 可选 target),让 `toolchain list` 的 `*`、`mcpp self env`、build 三处**共用**它;`list`/`env` 显示**有效解析结果**并标注来源(project `[toolchain]` / `[target.x]` / `*-musl` 约定 / 全局 default)。
- **T-g**:ABI 报错里**点名工具链来源**(复用上面解析函数返回的 source),只有当来源是"全局 default"时才建议 `mcpp toolchain default …`;来源是 `[target.*]`/约定时,提示改 target 覆盖或换 `--target`。

**风险**:中(改解析展示,核心解析逻辑不动,只是统一读取)。
**测试**:构造带 `[toolchain]` + `[target.*]` 的项目,断言 `list *`/`env`/build 三者一致;ABI 报错文案按来源分支。

---

### T-i — 模块扫描器误报(imgui)

**根因**:默认正则扫描器 `scan_file`(`src/modgraph/scanner.cppm:202-332`)是**逐行裸文本扫描**,只剥行注释(`strip_line_comment` `:151-155,226`),**不识别字符串/原始字符串/块注释**。`create.cppm:221-224` 的 `R"GUI(… import imgui.core; import imgui.app; …)GUI"` 模板文本里的 `import` 在 `:287-323` 被当成真 import → `resolve_graph` 在 `:425-429` 报 "module 'imgui.core' imported but not provided"(打印于 `prepare.cppm:2169-2171`)。(`MCPP_SCANNER=p1689` 的编译器路径 `p1689.cppm:318-396` 不受骗,但非默认。)

**方案**:在 `scan_file` 读取循环(`:224-227`)加**跨行词法状态**:`in_raw`(+活动 `R"delim(`/`)delim"` 定界)、块注释 `/* */`、普通字符串 `"…"`,被其吞掉的行不进入 `:287` 的 import 匹配。状态需跨 `getline` 持有(类比现有 `if_depth` `:221`)。

**风险**:**低**,高度隔离、易测;只影响扫描,不改产物。**快赢项**。
**测试**:新 gtest——含 `R"(import foo.bar;)"` 原始字符串 + `/* import x; */` 块注释的 `.cppm` 不产生 require/warning;真实顶层 `import` 仍被识别。

---

### T-j — registry 依赖变动后 build.ninja 陈旧

**根因**:指纹 `compute_fingerprint`(`src/toolchain/fingerprint.cppm:90-115`)的依赖项里 **`dependencyLockHash` 被硬编码为 `""`**(`prepare.cppm:2200`,注 "M2");`canonical_package_build_metadata`(`prepare.cppm:98-143`)只折入依赖的**清单声明身份**(`name@version`、flags、include、generated 内容),**不哈希 registry 里依赖的实际磁盘状态**。于是依赖被重装但**版本串不变** → `fp.hex` 不变 → `outputDir` 不变 → 复用旧 `build.ninja`。fast-path `try_fast_build`(`src/build/execute.cppm:262-362`)只 stat `projectRoot/src/` 与 `mcpp.toml`(`:305-321`),**不 stat 依赖目录**;且 ninja 的 `missing and no known rule` 错误**不被** `is_stale_ninja_failure`(`:154-159`)识别 → 不自动重生 → 硬失败(需手动 `mcpp clean`)。

**方案(两处互补)**
- **主**:`prepare.cppm:2200` 用**真实哈希**替代空串——对每个已解析依赖的 registry 安装状态(install root 下源文件 mtime/内容,via `fingerprint.cppm:36/82 hash_file`)求哈希,喂入 `dependencyLockHash`。依赖重装即改 `fp.hex` → 干净重生到新目录。
- **兜底**:把 `"missing and no known rule to make"` 加入 `is_stale_ninja_failure`(`execute.cppm:154-159`),让陈旧图触发一次性重生而非硬失败;可选再让 `try_fast_build` 的新鲜度扫描覆盖依赖根目录(`:309-321`)。

**风险**:**中高**——动指纹会影响缓存命中率与重建行为,需防"过度失效"(每次重装全量重编)。建议主方案用**内容哈希而非纯 mtime**,保证同内容重装不变指纹。
**测试**:e2e——装好→构建→原地重装同版本依赖(改内容)→ 构建应重生而非报 missing;同内容重装→指纹不变、命中缓存。

---

## 3. PR 拆分建议

**结论:拆成 5 个 PR,不要并成一个。** 五项分属 5 个互不重叠的子系统、文件不交叉、风险档次差异大;并成一个会让 review 困难、把无关风险耦合在一起。

| PR | 标题 | 含 | 主要文件 | 风险 | 建议次序 |
|---|---|---|---|---|---|
| **PR-1** | scanner: 跳过字符串字面量 | T-i | `src/modgraph/scanner.cppm` | 低 | **1(快赢)** |
| **PR-2** | install-integrity: 产物校验 + 禁用 legacy 标记 + doctor 体检 | T-e, T-f | `package_fetcher.cppm` `install_integrity.cppm` `manifest.cppm` `doctor.cppm` | 中 | 2(接 #137) |
| **PR-3** | toolchain: 有效解析统一 + ABI 可操作报错 | T-g, T-h | `prepare.cppm` `toolchain/lifecycle.cppm` `toolchain/registry.cppm` | 中 | 3 |
| **PR-4** | first-run UX: 进度可见 + 索引提示 + 镜像/超时 | T-b, T-c, T-d | `xlings.cppm` `config.cppm` | 低-中 | 4 |
| **PR-5** | build-graph: 依赖指纹 + stale-ninja 自愈 | T-j | `prepare.cppm` `execute.cppm` `fingerprint.cppm` | 中高 | **5(最后,需缓存回归)** |

**分组依据**
- 子系统内聚:每个 PR 只动一个子系统,文件集基本不交叉(唯一轻微交叉:PR-3 与 PR-5 都碰 `prepare.cppm`,但区域不同——`:487-520/2453` vs `:2200`——可顺序合入避免冲突)。
- 风险隔离:PR-1(纯隔离)、PR-5(动缓存指纹)放两端;PR-2 紧接 #137 同区一起 review。
- 独立可合:5 个互不依赖,任意顺序都能单独合;每个都随**下个 mcpp release**生效(都是核心二进制改动)。

**可选合并**:若想减数量,PR-1+PR-5 可并为"build-graph 正确性"一个 PR(都是今天暴露的 build.ninja/模块图问题),但风险档次差异大(T-i 极安全、T-j 需缓存测试),**建议仍分开**。

---

## 4. 生效方式

全部 5 个 PR 都是 **mcpp 核心二进制改动** → 生效路径同 #137:

```
合入 mcpp/main → cut release → 镜像 xlings-res(gh+gtc)
→ 更新 xim-pkgindex + bump bootstrap pin → 用户 `xlings install`/升级
```

即**需发新 mcpp 版本**,用户升级后随二进制生效。无一可走"改索引即生效"的免发版路径(那只属于配方类,如 #43)。

---

## 5. 不在本设计内(xim-res 打包类,另行处理)

- **T-a** strip 工具链(cc1/cc1plus 未 strip,490MB→~150MB,首跑 ~3× 提速)——xim-res 重新打包。
- **T-k** 去重 bundled gcc tarball(同包 10 份 = 3.4GB)——缓存清理/打包。
- **T-l** `.tar.gz`→`zstd` 并行解压——xim-res 打包。

生效方式:在 xim-res 重新打包工具链 → 用户重装/新装工具链时生效(非 mcpp 二进制)。

---

## 6. 进度

- [x] **PR-1 scanner 跳字符串(T-i)** — 实现 + 2 项 gtest 回归 + 实测 create.cppm 不再误报;开 PR
- [ ] PR-2 install-integrity 加固(T-e, T-f)
- [ ] PR-3 toolchain 解析统一 + ABI 报错(T-g, T-h)
- [ ] PR-4 first-run UX(T-b, T-c, T-d)
- [ ] PR-5 build-graph 失效(T-j)+ 版本 bump → 0.0.58

> 次序:PR-1(快赢)先;PR-2/PR-3/PR-4 文件不交叉 → 并行实现;PR-5 最后(碰 prepare.cppm 不同区域 + 带版本 bump)。
