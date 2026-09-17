---
subject: triage
status: landed
---

# Issue #660 分析:`glibc@2.44` 绑定在 2.44.3 发布后解析失败

- mcpp-community/mcpp#660(Sunrisepeak,2026-09-16,open)
- 触发事件:openxlings/xim-pkgindex#852(glibc 2.44.3,`latest` 前移),它是 openxlings/xlings#605 的根治修复
- 分析日期:2026-09-17;本机 x86_64,host glibc 2.39;被测对象是发布物 `xim-x-mcpp/2026.9.15.1`(与使用方 CI 同版本)

## 0. 结论

1. **issue 对机制的描述不成立。** mcpp 并不要求目录名恰好是 `2.44`。从 2026.8.27.x 起,
   `payload_dir_for_version`(`src/xlings/xlings.cppm:1183`)的规则是:有同名目录就用它;
   否则**恰好一个**按分量细化的目录(`2.44.x`)也算答案;**两个及以上细化目录则拒绝**。
   #660 的真实触发条件是:

   > 绑定是一条版本线(`glibc@2.44`),store 里没有 `2.44` 目录,却有 **两个** `2.44.x`
   > (缓存恢复的 `2.44.2` + 新下载的 `2.44.3`),并且 subos 视图里没有 libc,
   > 于是 fixup 走到按版本查目录的那条分支。

   报错信息打印的是 `<root>/2.44`,读起来像「必须精确匹配」,报告人正是据此得出了错误结论。
   实际走到的是「两个细化目录,拒绝选择」这条分支(`post_install.cppm:422-429`)。

2. **「每台干净机器都失败」不成立,「开发机不复现」的解释也不对。** 只有 `2.44.3` 的干净
   store 能正常通过(本地实测 C3)。会不会失败取决于 store 的历史:只要 store 里有上一个修订
   版且没有精确的 `2.44`,就会失败,无论这台机器是 CI 还是开发机。

3. **CI 为什么会得到一条版本线作为绑定。** 使用方 CI 的 mcpp home 实际运行的是
   xlings **2026.8.17.2**(日志:`Note vendored xlings 2026.8.17.2 is older than the pinned
   2026.9.14.1, but no newer source is available (keeping it ...)`)。这个版本低于 mcpp 注释里
   写明的下限 2026.8.27.2;低于下限时,subos 的运行时取编译期常量 `glibc@2.44`。mcpp 把这个下限
   称为「floor, not a preference」,但遇到旧 xlings 时只打印一条 Note 就继续执行。
   新版 xlings 按索引解析,记录的是精确版本(本地实测:`glibc@2.44.3`,`runtime_source: index`)。

4. **#852 本身没有问题,它只是触发者。** 补丁语义正确,验证充分。但每发布一个打包修订版
   (`2.44.N`),凡是绑定为版本线、store 里又留着上一个修订版的环境,都会多出一个细化目录,
   从而踩中 mcpp 的歧义拒绝。2.44.2 发布时没有暴露,是因为当时各处 store 里还有精确的 `2.44`,
   精确匹配优先。

5. **使用方的绕过办法有效,但理由和评论里写的不同。** 在 `[xlings.workspace]` 里钉
   `xim:glibc@2.44` 能过,是因为精确目录优先;与「必须精确匹配」无关。更直接的办法见 §8。

## 1. 三个对象的关系

```
xlings#605  node 的原生模块(sharp → libvips-cpp.so,DT_RUNPATH=$ORIGIN)找不到 libresolv.so.2
  ├─ xim-pkgindex#851  node 配方追加 NEEDED(缓解措施,只覆盖 node)
  └─ xim-pkgindex#852  glibc 2.44.3:ld.so 把自身所在目录当默认库目录(根治);latest → 2.44.3
        │
        └─ mcpp#660  store 中 {2.44.2, 2.44.3} 并存 + 绑定为版本线 glibc@2.44 ⇒ fixup 拒绝
```

## 2. mcpp 侧的机制(代码)

`ensure_post_install_fixup`(`src/toolchain/post_install.cppm`,2026.9.15.1 与 main 相同)
在 Linux 上有两条分支:

| 条件 | 分支 | 查找方式 |
|---|---|---|
| `resolve_runtime_binding` 在 subos 视图 `lib64/`/`lib/` 找到 `libc.so.6` | A | 取视图链接的**物理**目录,`runtimeId` 改写成物理版本(`binding.cppm:414-420`) |
| 视图里没有 libc(`libraryDirs` 为空) | B | `find_sandbox_glibc_lib` → `select_glibc_payload_lib` → `payload_dir_for_version(root, "2.44")` |

分支 B 的解析规则(`xlings.cppm:1183-1223`):

- 如果 `root/2.44` 是目录,直接返回它(精确匹配优先)
- 否则逐项扫描:分量数更多且前缀分量相等的目录算作细化;**第二个细化目录出现时返回 nullopt**
- 单测已经钉住了这条规则:`TwoRefinementsAreRefusedRatherThanChosenBetween`、
  `PayloadDirForVersion.TwoRefinementsAreNotAnAnswer`(`tests/unit/test_post_install.cpp:268,302`)

还有两处与此相关的缺陷:

- **报错文本有误导性**:`post_install.cppm:426-429` 打印的是 `glibcRoot / version`,既没有说明
  这是「歧义」,也没有列出候选目录。#660 的全部误读都源于这一行。
- **过时的注释**:`post_install.cppm:379-381` 仍写着 "glibc@2.44 means the `2.44` directory
  and no other",与下面的实现矛盾。
- **第三个按声明版本拼目录的调用点**:`binding.cppm:264` 的 `exact_declared_glibc_payload`
  只认精确目录,没有使用共享的解析器。CI 日志里每次构建都出现的
  `runtime binding glibc@2.44 has no loader path or library directory yet ... a second build
  resolves it` 就来自这里。对版本线绑定来说,「第二次构建会解决」永远不会发生。
  `payload_dir_for_version` 的头注释写着「它有两个调用者,而它们曾分别出过错」,
  这里是尚未收拢的第三个。

## 3. 为什么 CI 得到的是版本线绑定,而且视图里没有 libc

**绑定。** 使用方的 `install-mcpp.sh` 用 `xlings install mcpp@2026.9.15.1 -g` 安装 mcpp。
xlings 包形态的 mcpp 不使用自包含 home,因此 home 是 `~/.mcpp`。
`acquire_xlings_binary`(`src/fallback/xlings_binary.cppm`)的升级来源只有两个:
`MCPP_VENDORED_XLINGS` 与 `which xlings`。CI 系统上的 xlings 是 `v2026.8.17.2`,
所以 home 一直停留在 2026.8.17.2。然而同一个 mcpp 包里自带
`registry/bin/xlings`,版本是 **2026.9.14.1**(本地实测),升级逻辑没有把它算作来源。
2026.8.17.2 的 `manifest.cppm:55` 是 `DEFAULT_RUNTIME = "glibc@2.44"` 常量。

报告人在评论里说「用 xlings 2026.9.16.1 新建 subos 仍然记录 `glibc@2.44`」。这是另一条路径:
在隔离的 `XLINGS_HOME` 里没有同步索引,新版 xlings 回落到 `DEFAULT_RUNTIME_FALLBACK = "glibc@2.44"`
(此时 `runtime_source` 为 `fallback`,但评论贴出的 JSON 里没有这个字段)。
两条路径结果相同,原因不同。

**视图。** 使用方的 `actions/cache` 只缓存 `~/.mcpp/registry/data/xpkgs`,不包含 xlings 的
版本数据库和 `registry/subos`。每次运行时,xlings 看不到恢复出来的 payload 已经安装,
于是重新下载(16:01 那次绿色运行的日志里,在同一个缓存 key 下仍然出现了 `Downloading xim:glibc@2.44.2`)。
至于视图为什么一直没有 libc,我的推断是:`mcpp test` 的 `[xlings.workspace]` 按项目模式供给,
glibc 被链接进了项目的 subos,而没有进 registry 的 default 视图。这一点**没有单独测量**;
但日志里的 `no loader path or library directory yet` 证明 fixup 当时确实走的是分支 B。

## 4. CI 证据:同一个缓存 key,只有索引变了,结果由绿转红

| run | 时间 | 缓存 key | 供给时下载的 glibc | 结果 |
|---|---|---|---|---|
| 35119235698 | 16:01 | `…-9ebd58…` | `xim:glibc@2.44.2` | 绿 |
| (xim-pkgindex#852 合入,17:13) | | | | |
| 35138626160 | 19:07 | `…-9ebd58…`(同一个) | `xim:glibc@2.44.3` | 红,27 秒后报 #660 错误 |
| 35147931933 | 20:41 | `…-eaa867…` | 恢复后打印:store 里只有 `2.44.2`;随后下载 `2.44.3` | 未钉 glibc 的 job 全红;钉了 `xim:glibc@2.44` 的 job 绿 |

同一个缓存在 16:01 能通过、在 19:07 失败,说明 store 里没有精确的 `2.44`
(如果有,精确匹配优先,两次都会通过);而 20:41 的打印直接显示 store 里是 `2.44.2`。
「缓存 `2.44.2` + 新下载 `2.44.3`」正是歧义的构成。

## 5. 本地复现(A/B,发布物 mcpp 2026.9.15.1)

临时 `MCPP_HOME`,`MCPP_VENDORED_XLINGS=<包>/registry/bin/xlings`(2026.9.14.1),
每一步都执行 `mcpp toolchain install gcc@16.1.0`,触发 fixup。

| 步骤 | 绑定 | 视图里有 libc | store | 结果 |
|---|---|---|---|---|
| A | `glibc@2.44.3`(index) | 有 | 2.44.3 | 通过 |
| B1 | `glibc@2.44`(`xlings subos runtime`) | 有(链到 2.44.3) | 2.44.3 | 通过(分支 A) |
| B2 | 同上 | 有 | 2.44.2, 2.44.3 | 通过(分支 A,不看 store) |
| C2 | `glibc@2.44` | **无** | 2.44.2, 2.44.3 | **失败,与 #660 报错逐字相同** |
| C3 | `glibc@2.44` | 无 | 2.44.3 | 通过(唯一细化) |
| C4 | `glibc@2.44` | 无 | 2.44.2, 2.44.3 | **失败**(恢复 2.44.2 后再次复现) |
| C1 | `glibc@2.44` | 无 | 2.44 空壳, 2.44.2, 2.44.3 | 失败:`payload '…/2.44' is stale/incomplete` |

C2/C3/C4 是判据:只改 store 里是否存在第二个细化目录,结果随之翻转。

B 系列还显示了一个附带现象:`xlings subos runtime glibc@2.44` 改写了声明,但视图仍然链接到
2.44.3,因此 mcpp 在分支 A 中把物理身份当作 `glibc@2.44.3`。声明与视图不一致,
xlings 对此只给出一条 warn。

C1 中的 `2.44` 是第一次 `xlings install xim:glibc@2.44` 产生的空壳:目录里只有 `.xpkg.lua`
和 `.xpkg-install.json`,没有 payload。在同一个 home 里第二次安装就得到了完整 payload,
**原因未查明,也没有复现出第二次**。报告人第一条评论里看到的「钉 2.44 后 stale/incomplete」
可能就是这个现象,而不是他后来撤回时给出的 `lib64` 符号链接解释。

## 6. xim-pkgindex#852 审查

**正确性。** 补丁在 `_dl_init_paths` 的系统目录初始化之后,只替换 `dirs[0]` 的 `dirname`/`dirnamelen`,
并同步更新 `max_dirnamelen`(`open_path` 按它分配 hwcaps 子目录的缓冲区)。它不改 cache、
不改其余编译期目录,也不改 secure 模式下 `is_trusted_path_normalize` 使用的 `SYSTEM_DIRS`。
两种进入方式分别处理:PT_INTERP 取 `l_libname->name`,直接调用取 `/proc/self/exe`。
行为断言放在 `build-glibc.sh` 里,覆盖了三件事:只有 RUNPATH 的程序能启动,`--list` 从加载器目录解析,
宿主独有的 `libz.so.1` 仍不可达。补丁不生效时断言会失败,PR 里有验证记录。
这是合适的判据,因为 `strings` 看不出差异。

**「目录里只有 glibc 自己的对象」的前提。** 这个前提要求 PT_INTERP 指向 payload 目录,而不是
subos 视图(视图里汇集了所有包的库)。本机实测:xlings 的 node/python 和 mcpp 的 gcc,PT_INTERP
都是 `xpkgs/xim-x-glibc/<ver>/lib{,64}/ld-linux-x86-64.so.2`,前提成立。如果将来有程序把 PT_INTERP
指向视图,这个补丁就会把整个视图变成默认目录。这个约束目前只写在补丁注释里,没有任何检查在执行它。

**残余风险(低)。** 实现依赖 rtld 内部的初始化顺序:`_dl_init_paths` 执行时
`_dl_rtld_map.l_name` 是否已经设置,决定了走哪条分支。升级 glibc 时,build 脚本里的行为断言会拦住回归。

**跨仓影响(#660 的来源)。** PR 正文说「已有的 subos 仍绑定原来的 glibc」,只考虑了 xlings 侧。
mcpp 的细化规则把「同一条版本线下出现新修订版」变成了一个会导致失败的事件。
这个组合的判据应当是「先 A 后 B」:在已经装有 2.44.2 的 store 上安装 2.44.3,再做一次 mcpp 工具链解析。
这项测试没有做。

**附带发现:2.44 发布物带着构建机的临时 RUNPATH。** 索引里 `2.44` 的归档
(sha256 `0105292f…`,与索引一致)中有 15 个 ELF(`bin/iconv`、`locale`、`localedef`、`getent`、
`makedb`、`sbin/zic`、`iconvconfig`、`libexec/getconf/*` 等)的 RUNPATH 是
`/tmp/claude-1000/-home-speak-workspace-github-openxlings-xlings/…/scratchpad/realhome/…`,
安装后也没有被改写。`2.44.2` 和 `2.44.3` 中为 0 个。影响很小,但 `2.44` 恰好是 xlings 的
`DEFAULT_RUNTIME_FALLBACK`。如果多用户主机上有人抢先创建了这个 `/tmp` 路径,就能向这些工具注入库。

## 7. 修复建议

### mcpp(#660 的归属仓)

1. **让安装与绑定出自同一个答案。** `lifecycle.cppm:937` 与 `prepare` 的首次安装都用不带版本的
   `xim:glibc`(即 latest),而 fixup 读的是 subos 的声明。两处回答的是同一个问题。
   应当按绑定的版本安装(`xim:glibc@<binding 的版本>`);如果绑定来源是 `fallback`
   或旧 xlings 写入的常量,就先用索引重新绑定(`xlings subos runtime <index 解析值>`),再安装。
   **不建议**把歧义规则改成「取最高版本」,那只是换一种方式猜。
2. **把 xlings 下限变成真正的下限。** 在 `candidate_source_version` 中加入可执行文件旁的
   `../registry/bin/xlings`(xlings 包形态的 mcpp 自带这个文件,版本正好等于 pin)。仍然低于下限时,
   至少要对依赖下限的操作(运行时绑定)给出错误或明确警告,而不是一条 Note。
3. **报错写出事实。** 区分「没有候选」和「有 N 个候选,拒绝选择」,并列出候选目录名;
   同时更新 `post_install.cppm:379` 的过时注释。
4. **收拢第三个调用点。** 让 `exact_declared_glibc_payload`(`binding.cppm:264`)走
   `payload_dir_for_version`,并审视「a second build resolves it」这句提示对版本线绑定是否成立。
5. **判据。** 加一条 e2e:store 预置 `2.44.2`,绑定 `glibc@2.44`,视图无 libc;安装 `2.44.3` 后解析工具链。
   修复前应为红,修复后为绿。已有单测只覆盖纯函数,覆盖不到「安装与绑定不一致」这个组合。

### xlings

1. 回落绑定(`runtime_source: fallback`)在索引第一次可用时应自动重新解析,或者由 `self doctor` 报告;
   否则它会持续充当一条版本线。
2. `subos runtime <binding>` 改写声明后,应重新链接视图,或者拒绝在视图与声明不一致的状态下完成
   (§5 B 系列)。
3. 查明 C1 中 `install xim:glibc@2.44` 只留下元数据空壳的原因(观测到一次)。

### xim-pkgindex

1. 在 `glibc.lua` 的修订版策略注释中写明:发布新修订版会让绑定为版本线的 store 出现第二个细化目录;
   在 mcpp 的修复发布之前,这是一个跨仓事件。
2. 考虑是否需要处理 `2.44` 归档里泄漏的临时 RUNPATH(条目只追加不修改,因此只能在注释里说明,
   或者让 xlings 回落常量不再指向它)。

### 使用方的即时绕过

以下任一即可,不需要 feature 开关:

- 让 home 里的 xlings 达到 pin:CI 的 `XLINGS_VERSION` 升到 ≥ 2026.9.14.1,或者设置
  `MCPP_VENDORED_XLINGS=$(dirname $(readlink -f $(which mcpp)))/../registry/bin/xlings`。
  然后确认 `registry/subos/default/.xlings.json` 里是 `runtime_source: index`,且 runtime 是精确版本。
- 或者让缓存里不再同时存在两个修订版:修改缓存 key 的前缀使旧缓存失效,或者在恢复后删除
  `xim-x-glibc/2.44.2`。

评论里的第二个诉求(`cfg(host_os)`)本身是一个合理的需求,应当单独开 issue 评估。
但 #660 不应以它为前提:工具链的 glibc 属于 mcpp 的契约,项目不应该需要去声明它。

## 更正(2026-09-17,后续实测)

§0 第 3 点与 §7「使用方的即时绕过」中「让 home 里的 xlings 达到 pin 即可」**不成立**。

- 在满足下限的 xlings(2026.9.14.1)下,如果工具链由缓存命中而声明的运行时版本不在缓存中,fixup 同样失败(`RuntimeBinding glibc@2.44.3 requires payload …/2.44.3`)。
- 更深一层的原因:mcpp 在 `lifecycle.cppm:937` / `prepare.cppm:3720` 安装 `xim:glibc` 的循环从未生效。`resolve_xpkg_path` 拒绝不带版本的目标,而调用方忽略了这个错误。
- 修订后的根因与方案见 `2026-09-17-runtime-binding-multi-repo-plan.md`(v3)。

有效的即时绕过只剩一条:让缓存中不同时存在两个修订版,或让缓存包含当前声明的精确版本。
