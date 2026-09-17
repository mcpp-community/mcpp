---
subject: triage
status: landed
---

# 运行时绑定方案 v3:让 mcpp 真正安装它所声明的运行时

- 日期:2026-09-17(v3,取代同日的 v1 与 v2)
- 前置分析:`.agents/docs/2026-09-17-issue-660-glibc-line-binding-analysis.md`(其中 §7「使用方即时绕过」的第一条已被本文 §1 的实测推翻,见该文件末尾的更正)
- 范围:mcpp 一个 PR(必需);xim-pkgindex 一个 PR(判据门);xlings **不需要 PR**,只开一个独立 issue
- 状态:待 review

---

## 0. 讨论记录:对 v2 三个问题的回答

### Q1 `xlings subos describe --json` 是做什么的,是否必要?

v2 引入它有两个用途:(a) 把旧 subos 的运行时记录迁移成「版本 + payload 位置」的完整契约;(b) 作为 mcpp 查询这份契约的机器接口。

**结论:不必要,撤回。**

- 机器接口:xlings 已有 NDJSON 的 `xlings interface <capability>`(目前 21 个 capability),mcpp 已经通过它调用 `install_packages`。如果将来确实需要查询,应当新增一个 **interface capability**,而不是新增 CLI 动词;CLI 是面向人的界面。
- 迁移:v3 不需要迁移(见 Q3)。

### Q2 D2(离线 + 从未链接 + 旧契约 → 报错)的影响面

v3 中这条变为:「离线,且 subos 声明的那个运行时版本不在 store 中」时报错;报错会写明需要预先获取的精确坐标 `xim:glibc@<v>`。三个条件必须**同时**成立才会触发:

1. 处于离线状态(`--offline`、`MCPP_OFFLINE`,或网络不可达);
2. 声明的版本目录不存在。现实中只有两种来源:
   - 旧 xlings(< 2026.8.27.2)建的 home,声明的是常量 `glibc@2.44`,store 里却只有 `2.44.x`;
   - CI 缓存恢复了工具链,但缓存里的 glibc 不是当前声明的版本;
3. 视图中没有链接 libc(视图已链接时,身份取自链接,不查 store)。

影响的变化:在 v3 之前,这类 home 靠「store 里恰好只有一个 `2.44.x`」才能通过;一旦出现两个就是 #660。所以 v3 不是把能用的变成不能用,而是把**时而通过、时而失败**变成**确定报错并给出命令**。联网环境(包括普通 CI)不会触发这条:第一次运行就会把声明的版本安装好,之后永久满足条件 2。受影响的只有「从旧 home 或旧缓存直接进入离线运行」的空隔离环境,修复方法是把报错中的坐标加入它们的离线缓存。

### Q3 v2 的「运行时新契约」要解决什么,为什么现在撤回?

**v2 想解决的问题**:mcpp 需要知道 subos 的 glibc payload 在哪里,而它是**推导**出来的:要么读视图里的链接,要么按版本号扫描 `xpkgs/xim-x-glibc/`。推导在版本线和多个修订版并存时会失败(#660)。v2 的思路是让 xlings 把 payload 位置直接写进 `subos_info`,mcpp 只读不推导。

**撤回的原因**:继续实测后发现,真正缺的不是「位置信息」,而是 **payload 从来没有被保证安装**:

- xlings 在下限版本之上已经具备全部所需的语义:
  - `self init` 按索引解析,写入具体版本(实测 E2:`glibc@2.44.3`,`runtime_source: index`);
  - 安装时的优先级是「声明 > 激活 > 索引」:不带版本的 `xim:glibc` 或 `glibc@>=2.39` 依赖都会解析到 subos 声明的版本(xlings `xim/commands.cpp`,`subos_version_of_`);
  - store 的布局 `xim-x-glibc/<version>` 与视图链接,本身就是位置信息。
- 缺口在 mcpp:它「安装声明的运行时」的那段代码**从来没有生效过**(§1 F2),payload 只是作为工具链依赖的**副作用**被装进来。工具链由缓存命中时,这个副作用就不会发生。

只要 mcpp 显式安装它声明的运行时,位置就恒等于 `xim-x-glibc/<声明版本>`,不需要新增字段,也不需要迁移。新契约成了冗余的第二份记录,撤回。

---

## 1. 根因(v3 修正版,带新证据)

| # | 事实 | 证据 |
|---|---|---|
| F1 | mcpp 的「确保 sysroot payload」循环传入的是不带版本的 `xim:glibc` / `xim:linux-headers` | `lifecycle.cppm:937`,`prepare.cppm:3720` |
| F2 | `Fetcher::resolve_xpkg_path` 遇到不带版本的目标直接返回错误 `expected <name>@<version>`;调用方只在 debug 级别记录结果并忽略。**因此这段安装从未执行过** | `package_fetcher.cppm:1020-1025`;`mcpp -v` 实测:`installing dep: xim:glibc` 之后没有 fetcher 的 `resolve:` 行,而 `xim:gcc@16.1.0` 有 |
| F3 | 声明的运行时 payload 只能作为工具链依赖被 xlings 装进来。工具链带着 `.mcpp_ok` 命中(缓存恢复、全局拷贝)时,mcpp 不会调用 xlings,运行时也就不会被安装 | 实测 F-new 第 1 步 |
| F4 | 下限以上的 xlings:声明具体、按声明安装。下限以下(2026.8.17.2):声明是常量 `glibc@2.44`,依赖解析到 latest | 实测 E1 / E2;CI 日志 |
| F5 | mcpp 对 xlings 下限只打印 Note。xlings 包形态的 mcpp 自带满足下限的 xlings,但获取逻辑从不使用它 | `fallback/xlings_binary.cppm` |
| F6 | `payload_dir_for_version` 的细化规则,是 mcpp 为下限以下的 xlings 写的兼容层,并且掩盖了 F2 | 该函数头注释;#660 |

**实测矩阵**(发布物 mcpp 2026.9.15.1;每组都是新建 home)

| 组 | xlings | store 预置(模拟缓存) | 视图有无 libc | 结果 |
|---|---|---|---|---|
| E1 | 2026.8.17.2 | glibc 2.44.2 | 安装后链接到 2.44.3 | 绿(分支 A 掩盖了问题) |
| E2 | 2026.9.14.1 | glibc 2.44.2 | 安装后链接到 2.44.3 | 绿 |
| F-new 第 1 步 | 2026.9.14.1 | glibc 2.44.2 + **已有 gcc** | 无 | **红**:`RuntimeBinding glibc@2.44.3 requires payload …/2.44.3` |
| F-new 第 3 步 | 2026.9.14.1 | 同上,再由项目模式安装 glibc(2.44.3) | 链接到 2.44.3 | 绿 |
| 分析报告 C2/C4 | 2026.9.14.1 | `{2.44.2, 2.44.3}`,声明为版本线 | 无 | 红(#660 原文) |

F-new 第 1 步说明:**仅升级 xlings 不能修复**;只要工具链是缓存命中而声明的运行时不在缓存里,新 xlings 也会失败。报错形式与 #660 不同,但是同一个根因。

**一句话根因**:mcpp 声明了运行时,却从不安装它;它依赖一个会被缓存绕过的副作用,再用目录扫描去猜副作用留下了什么。

---

## 2. 原则

1. **声明的东西由声明方保证存在。** mcpp 选定了 subos 的运行时,就负责让 xlings 安装那一个精确版本。
2. **解析交给 xlings,查找只做精确匹配。** 版本语义(声明 > 激活 > 索引)只在 xlings 中存在一份;mcpp 按 `xim-x-glibc/<声明版本>` 精确读取,不做细化匹配,也不扫描。
3. **下限就是下限。** 低于下限的 xlings 没有第 1 条所依赖的语义,因此必须拒绝,而不是提示后继续。
4. **失败不能被吞掉。** 安装失败要作为错误返回,不能只写 debug 日志。

---

## 3. mcpp PR(必需)

### 3.1 按声明安装运行时(修复 F1–F3)

- 删除两处死代码循环(`lifecycle.cppm:937`、`prepare.cppm:3720`)。
- 新增一个函数 `ensure_declared_runtime(cfg, binding)`:
  - 触发条件:仅当 `runtimeId` 不是 hosted 运行时,并且 `xim-x-<name>/<version>` 中没有 loader;
  - 动作:调用 `fetcher.resolve_xpkg_path("xim:" + runtimeId, autoInstall=true)`,例如 `xim:glibc@2.44.3`;这走的是现有的 `xlings interface install_packages` 通道;
  - 失败时返回错误:`declared runtime glibc@2.44.3 is not installed and could not be installed: <原因>`;离线时附加 `pre-fetch xim:glibc@2.44.3`。
- **调用位置**:在两个入口计算 runtime binding 快照的地方(`prepare` 与 `toolchain install`),解析之后发现 payload 缺失时调用一次,然后重新解析。五处 fixup 调用点都消费这份快照,因此不需要各自处理。
- **正常路径零开销**:payload 已存在时不启动任何子进程。
- `xim:linux-headers` 不纳入:它一直由工具链依赖带入(死循环从未安装过它),保持现状就等于不改变行为。已列入 §6 风险。

### 3.2 精确查找,删除兼容层(修复 F6)

- `select_glibc_payload_lib` 改为只认 `root/<version>`(恢复该函数注释中「glibc@2.44 means the `2.44` directory」的原始语义)。
- 删除 `paths::payload_dir_for_version` 的细化分支。`probe.cppm` 的调用点同样改为精确查找,因为 3.1 保证了精确目录存在。
- 更新对应单测:`ARequestResolvesToItsOneRefinement`、`TwoRefinements*` 改为断言精确匹配语义。

### 3.3 xlings 下限真正生效(修复 F4、F5)

- 获取来源的顺序:发布物自带的 `<exe>/../registry/bin/xlings`(自包含 tarball 与 xlings 包两种形态)→ `MCPP_VENDORED_XLINGS` → `which xlings`。「只升不降」规则不变。
- 获取之后仍低于 `kXlingsVersion`:返回错误,并给出获取命令。发布物形态必定带有满足下限的 xlings,因此这只可能发生在开发构建上。
- 不需要提升 pin:2026.9.16.1 已经具备所需语义(declared 优先级从 2026.8.27.5 起就有)。

### 3.4 判据(e2e,全部使用发布形态)

| # | 场景 | 期望 |
|---|---|---|
| 1 | F-new 第 1 步的形态:store 中有 gcc + glibc 2.44.2,新 xlings 声明 2.44.3 | 绿;store 中新增 `2.44.3`;fixup 使用它 |
| 2 | #660 的形态:声明版本线 `glibc@2.44`(旧 manifest 夹具),store `{2.44.2, 2.44.3}`,视图无 libc | 绿;xlings 按声明安装精确的 `2.44` |
| 3 | 与 1 相同,但加 `--offline` | 红;报错中含 `xim:glibc@2.44.3` |
| 4 | PATH 上有 xlings 2026.8.17.2,使用 xlings 包形态的 mcpp | home 中的 xlings 等于 pin |
| 5 | 正常 home 连续构建两次 | 第二次构建不启动 xlings 子进程(trace 断言) |
| 6 | macOS / Windows | 不调用 `ensure_declared_runtime`(trace 断言) |
| 7 | 修复前后各跑一次 1 与 2 | 修复前为红(确认判据真的在检查) |

**代码量**:删除两个死循环、一个细化分支及相关单测;新增一个函数和获取来源顺序的一处改动。净删除。

---

## 4. xim-pkgindex PR(判据门,必需)

运行时包(`exports.runtime` 非空)的 `latest` 发生变化时,CI 执行:

1. 用已发布的 mcpp latest 建 home,安装工具链;
2. **模拟缓存**:只保留 `xpkgs/` 下的工具链与上一个运行时修订版,删除 `subos/`;
3. 发布新修订版(以 `latest` 指向它的索引运行),再次构建并运行一个 C++ 程序。

这个形态正是 #660 和 F-new 第 1 步共同的缺口。门必须覆盖「工具链由缓存命中」这一步,只做全新安装测不到问题。

合入顺序:先把门指向 mcpp 2026.9.15.1,在当前索引上确认为红;mcpp PR 发布后再改为 required。

---

## 5. xlings:不开 PR,只开独立 issue

v3 所依赖的 xlings 语义在下限版本中都已存在。以下是观测到、但**不在 #660 路径上**的问题,各自开 issue:

- `subos runtime glibc@2.44`:写入的声明是 `2.44`,激活的却是 `2.44.3`(分析报告 §5 B0)。推测原因是安装时读取的是**改写前**的声明;未验证。
- 第一次 `install xim:glibc@2.44` 只留下元数据空壳(C1,仅观测到一次)。
- `self init` 声明了运行时但不安装它,而 `subos new` 会安装:同一件事有两个创建入口,行为不一致。v3 的 3.1 已在 mcpp 侧覆盖;xlings 是否统一,由 xlings 自己决定。

---

## 6. 兼容性、无感升级与跨平台

| 情形 | 升级到 v3 的 mcpp 之后 | 可见变化 |
|---|---|---|
| 正常 home(声明版本已安装) | 无任何额外动作 | 无 |
| 旧 home,声明为常量 `glibc@2.44`,store 只有 2.44.x | 安装精确的 2.44,fixup 指向它;运行时契约变化,触发一次重建 | 一次下载(40 MB)加一次重建 |
| CI 缓存只含 `xpkgs`,声明的新修订版不在缓存中 | 安装声明版本 | 与今天的依赖下载量相当 |
| home 中的 xlings 低于下限(xlings 包形态的 mcpp) | 自动换成包内自带的 xlings | 一行 `Updating vendored xlings` |
| 离线,且声明的版本不在 store(Q2) | 报错,给出坐标 | 从时好时坏变为确定报错 |
| macOS / Windows / hosted 运行时 | 不进入 3.1 | 无 |
| Linux aarch64 | subos 不声明 glibc payload(glibc.lua 只提供 x86_64)→ 不进入 3.1 | 无;判据 6 扩展到这个平台 |
| 交叉构建 | 仍由 `needs_linux_sysroot_payloads(target)` 与声明共同决定 | 无 |

**关于「旧 home 会装上 2.44 旧发布物」**:这是 xlings「声明优先」语义的直接结果,v3 刻意不在 mcpp 中改写。用户可以用 `xlings subos runtime glibc@2.44.3` 显式切换。

**风险**

- `linux-headers` 与 glibc 是同一种形态(同样依赖副作用)。目前没有观测到失败,因为它只有一个版本。发布第二个版本前,需要按同样的方式评估。
- 3.2 删除细化匹配后,凡是**没有经过** 3.1 的读者都会立刻暴露问题。需要用 grep 穷举 `xim-x-glibc` 的全部读取点,确认只有 `post_install` 和 `probe` 两处,且都在快照之后。

---

## 7. 发布顺序

```
mcpp PR → release → (bot) 索引 bump
xim-pkgindex 门:先指向旧 mcpp 确认为红 → mcpp 发布后改为 required
使用方:删除 host-glibc feature 与 CI 中的 XLINGS_VERSION(自行清理)
```

两个 PR 互不阻塞;xlings 不在发布链上。

---

## 8. 需要 review 决定的问题

- **D1 3.2 是否立即删除细化匹配,还是保留一个版本作为过渡?** 建议立即删除。保留它就会继续掩盖 3.1 没有覆盖到的路径,正如它过去掩盖了 F2。
- **D2 旧 home 的常量声明 `glibc@2.44`:遵从声明安装 2.44,还是在 mcpp 中把它当作未决定、改用 index?** 建议遵从声明。「未决定」的判断属于 xlings;mcpp 自行重新解释,会重新引入第二个解析者。
- **D3 `linux-headers` 是否一并纳入 3.1?** 建议不纳入。它没有被声明,没有「声明的版本」可以安装;硬写版本只会把副作用换成猜测。

---

## 9. 实施记录(mcpp 2026.9.17.2)

实现与 §3 的差异,以及原因:

- **安装是惰性的。** `ensure_declared_runtime` 不在解析 binding 时执行,而在 `prepare` 中第一次调用需要
  C 运行时的 fixup 之前执行(`provide_runtime_payload`),且只对 `post_install_fixup_kind` 非空的
  工具链执行。否则使用系统编译器的构建也会下载 glibc。`toolchain install` 在解析 binding 后、
  fixup 之前执行同一函数。
- **安装失败不在这一步报错。** 失败写入 verbose 日志;需要 payload 的 fixup 报告缺失,并写出坐标
  `xim:glibc@<v>`。这样使用 musl、PE 或系统工具链的构建在离线时不受影响。
- **xlings 下限没有改为硬错误。** §3.3 的前提是「低于下限的 xlings 缺少 §3.1 依赖的语义」。实现中
  §3.1 按精确坐标安装,不依赖 xlings 的「声明优先」解析,所以下限以下的 xlings 同样得到正确结果。
  硬错误只会让 PATH 上 xlings 较旧、且不使用发布布局的安装方式(例如只依赖系统 xlings 的打包渠道)
  无法运行。保留的改动是把随发布的 xlings 加入获取来源,并排在 PATH 之前。
- **删除 `needs_linux_sysroot_payloads`。** 它唯一的调用点是那两个从未生效的循环。
- **判据。** e2e 737 在已发布的 2026.9.17.1 上为红(报 #660 原文),在本次构建上为绿;e2e 687 的
  C 腿同样在已发布的二进制上为红、在本次构建上为绿。
