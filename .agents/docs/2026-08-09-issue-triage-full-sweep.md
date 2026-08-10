# mcpp Open Issue 全量深度核验报告

> 盘点基线:mcpp `main@80291ca`(v2026.8.8.4)· xlings `openxlings/xlings@2913a09`(2026.8.9.2)
> xim-pkgindex `@c0aded29` · mcpp-index `@b86fc7c` · mcpp-vscode `@56594db`
> 日期:2026-08-09 · 方法:26 个 open issue,23 条逐条落到当前代码核验 + 对「可关闭」结论做双路对抗验证

---

## 0. 一句话结论

**23 条全部保持 open,本轮 0 关闭。** 两条曾被判为「可关闭」的(#313、#177)在对抗验证中被 2:0 推翻——推翻的理由不是「结论不够保守」,而是**拟发的关闭留言里有会被读者当场证伪的错误**(见 §3)。

真正的收获不在关闭数,而在:**核验过程中撞出 30 条 issue 一个字没提的缺陷,其中 6 条比它们所属的 issue 本身更该先修**,包括一条今天就在破坏已发布的 mcpp-vscode 扩展的 10 行缺陷(§5.1)。

---

## 1. 判定总表

| # | 判定 | 置信 | 优先级 | 工作量 | 一句话 |
|---|---|---|---|---|---|
| [#396](https://github.com/mcpp-community/mcpp/issues/396) | VALID_OPEN | high | P2 | L | form-X 二进制的链接期物理检查(xlings 闭环执行点 3),mcpp 侧零实现 |
| [#393](https://github.com/mcpp-community/mcpp/issues/393) | VALID_OPEN | high | P3 | M | `${mcpp.*}` 展开拼写没有单一规则,词表内部就自相矛盾 |
| [#392](https://github.com/mcpp-community/mcpp/issues/392) | PARTIAL | high | P1 | M | 上游已修默认 runtime,但 mcpp 的 xlings pin 没跟;fixup 仍按目录序选 glibc |
| [#386](https://github.com/mcpp-community/mcpp/issues/386) | VALID_OPEN | high | P2 | M | 维护者承诺的「`sources = []` 真正生效」一行未落地 |
| [#382](https://github.com/mcpp-community/mcpp/issues/382) | PARTIAL | high | P2 | M | 包侧已修;mcpp 把 subos 的 `op="set"` 读成无条件覆盖,与 xlings 契约相反 |
| [#380](https://github.com/mcpp-community/mcpp/issues/380) | VALID_OPEN | high | P2 | M | `mcpp new` 名字契约缺失:死循环 / 路径逃逸 / 假成功,四段代码原封不动 |
| [#379](https://github.com/mcpp-community/mcpp/issues/379) | PARTIAL | high | P2 | L | 契约层已由 PR #385 冻结为 wire v1;阶段 3b/3c 与 7 条纪律项未动 |
| [#374](https://github.com/mcpp-community/mcpp/issues/374) | VALID_OPEN | high | P2 | S | 把命名**约定**当成**要求**,非模块 lib 被误报缺模块根 |
| [#373](https://github.com/mcpp-community/mcpp/issues/373) | VALID_OPEN | high | **P1** | M | 扫描器不剥块注释——但真正的坑是它连**顺序**都是错的(§5.2) |
| [#371](https://github.com/mcpp-community/mcpp/issues/371) | PARTIAL | high | **P1** | M | 前提被证伪(CDB 早就在 spawn ninja 前写);真需求收窄到「不付全量构建」 |
| [#370](https://github.com/mcpp-community/mcpp/issues/370) | VALID_OPEN | high | P2 | S | 「范围永远够不着」被劈成两条路径,只有一条带修法提示 |
| [#313](https://github.com/mcpp-community/mcpp/issues/313) | PARTIAL | high | P3 | S | 7 条建议:3 条已交付、1 条重复于 #144、3 条无落地物 |
| [#304](https://github.com/mcpp-community/mcpp/issues/304) | VALID_OPEN | high | P2 | M | `[runtime] library_dirs` 进 `-L`;正解是第三条路(`-rpath-link`),不是 issue 给的二选一 |
| [#293](https://github.com/mcpp-community/mcpp/issues/293) | PARTIAL | high | **P1** | M | `ln -sf` 原样还在;mcpp 自己的改写器已围栏,xlings 侧写穿仍无遮挡 |
| [#290](https://github.com/mcpp-community/mcpp/issues/290) | VALID_OPEN | high | P2 | M | xpkg 描述符缺版本条件轴;但成本被 issue 高估一个数量级 |
| [#289](https://github.com/mcpp-community/mcpp/issues/289) | PARTIAL | high | P2 | M | 三条主张全被 PR #378 推翻;残留两洞,其一是 Windows 上整套机制空转 |
| [#284](https://github.com/mcpp-community/mcpp/issues/284) | VALID_OPEN | high | P2 | S | `toolchain remove` 少一个位置参数——而且比 issue 以为的还多缺 partial 解析 |
| [#283](https://github.com/mcpp-community/mcpp/issues/283) | VALID_OPEN | high | P2 | M | target pin 静默压过显式默认;更硬的一半是连 `[toolchain]` 也保不住 |
| [#276](https://github.com/mcpp-community/mcpp/issues/276) | VALID_OPEN | high | P2 | XL | 嵌入式 SDK RFC:不缺机制,缺输入通道;最小切入点是一个 `sysroot` 字段 |
| [#259](https://github.com/mcpp-community/mcpp/issues/259) | VALID_OPEN | high | P2 | M | 正文归因已被自己的评论推翻;mcpp 侧三层兜底全是空转,一行未改 |
| [#256](https://github.com/mcpp-community/mcpp/issues/256) | PARTIAL | high | P2 | S | 文档扎实;canary 在三处偏离目的,且守的不是默认工具链版本 |
| [#215](https://github.com/mcpp-community/mcpp/issues/215) | UPSTREAM_BLOCKED | high | P3 | S | 触发条件未满足(实机核过);但「谁告诉你该动了」没有机制 |
| [#177](https://github.com/mcpp-community/mcpp/issues/177)+[#144](https://github.com/mcpp-community/mcpp/issues/144) | PARTIAL | high | P2 | L | 同一缺口的两个面:mcpp 没有「申报外部已存在之物」的入口 |

**保留不动**(用户指定):[#43](https://github.com/mcpp-community/mcpp/issues/43)(NULL,历史盘点存档)、[#260](https://github.com/mcpp-community/mcpp/issues/260)(项目留言板)。

分布:`VALID_OPEN` 15 · `PARTIAL` 7 · `UPSTREAM_BLOCKED` 1 · `FIXED/OBSOLETE/INVALID` **0**。

---

## 2. 横向发现:五个反复出现的形状

这轮 23 条里,同一批形状出现了不止一次。它们比单条 issue 更值得记。

### 2.1 「同一决策多处推导」——本轮出现 7 次

| 决策 | 推导点数 | 后果 |
|---|---|---|
| 「用户没写 sources」 | 5 处(`toml.cppm:226/229/1475`、`prepare.cppm:3209`、`xpkg.cppm:1864`) | #386 修一处不够;且 `prepare.cppm:3210` 的默认 glob 已与 `toml.cppm:1476` 漂移(少了 `.S/.s/.asm`) |
| 「双写法 + partial 版本」 | 3 层 × 3 命令,remove 那份只推导了一半 | #284 |
| 限定包名 | 3 处算法不一致(`scanner.cppm:789`、`plan.cppm:264`、#374 将新增的第三处) | `namespace` + `name` 组合下 `plan.cppm:1054` 静默失配 |
| 目标路径 `current_path() / name` | 2 处(`create.cppm:183`、`:217`) | #380 |
| `${mcpp.*}` 展开拼写 | 词表内部两种(`.string()` vs `.generic_string()`) | #393 |
| glibc 载荷选择 | 构建路径已收敛到权威制,fixup 路径仍按目录序 | #392 / #396 |
| 「默认工具链是什么」 | `prepare.cppm` 与 `lifecycle.cppm` 两套算法 | #283 |

**判据**:新增一条语义时如果要改 ≥3 处,那不是工作量问题,是这个决策没有单一真源。

### 2.2 「修补放在控制流到不了的地方」——本轮出现 5 次

- **#289**:版本探针把 `2>/dev/null` 硬编码进一条经 `cmd.exe /c` 执行的命令(`xlings_binary.cppm:160`)⇒ Windows 上返回空串 ⇒ 两个消费点都退化成「读不出来,不判断」。而 issue 点名的受害平台**正是 Windows**。仓库里已有 `mcpp::platform::null_redirect`(`common.cppm:37-41`)就是为这件事准备的。
- **#256**:canary 的 control 步骤产出 `ctl.pcm` 之后再没引用过它;注释声称它守「导入侧必定能编过」,实际一个断言都没有。实测该断言今天在 22.1.8 上成立——可写、为真、就是没写。
- **#256**:`EXPECTED=unknown ⇒ exit 0`。索引一旦把 llvm `latest` 推到 23,canary 在最该响的时候静音。
- **#293/#259**:`{"xim:glibc","xim:linux-headers"}` 预装循环传的是无版本的 `"xim:glibc"`,被 `package_fetcher.cppm:932` 的 `<name>@<version>` 门当场拒掉——**自引入起一次都没装成过任何东西**,一处 `(void)` 丢弃、一处只进 `log::debug`。
- **#313**:`--no-color` 在 TTY 下是空操作(§5.3)。

### 2.3 「断言镜像了实现,而不是校验实现」

- **#215**:`test_cppfly.cpp:80` 断的是 mcpp 自己那张表的输出,输入还是手搓的 `Toolchain` 结构体,连编译器进程都不启动;`101_cppfly_llvm_soft.sh:49` grep 的是 mcpp 自己打印的 summary,而那行内容正由那张表决定。**上游 clang 明天落地反射,CI 一根红线都不会亮。**
- **#374**:`conventional lib root` 这条 warning 全仓零测试覆盖;7 个 `TEST(Validate,…)` 里只有一个传了非空 `projectRoot`,on-disk 检查在其余单测里被 `validate.cppm:139` 整段跳过。
- **#313**:`01_help_and_version.sh` 走管道,`detect_color()` 天然返回 false,所以「无色」这个结果永远对、原因永远测不出来。

### 2.4 「包侧绕过 ≠ 引擎缺陷消失」

三条 issue 的症状在生态里已经看不见了,但缺陷一动没动:

- **#304**:`compat.vulkan-runtime.lua:126-171` 已经把「只采集带版本号的 soname」固化成注释。下一个写 symlink farm 的人会原样踩进去,而报错里没有任何线索指向 `runtime.library_dirs`。
- **#396**:唯一挡住 libc 跟着 `[runtime] library_dirs` 进消费者 RUNPATH 的,是 `compat.glx-runtime.lua:203-213` 里一段 Lua 黑名单,注释自己写着「the failure it prevents has no diagnostic of its own」。**引擎级守卫缺位,被一个包用 Lua 补上了。**
- **#382**:`#565` 改的是配方 `config()` 的行为,只在配方重新执行时生效;已经写进某个 subos `.xlings.json` 的 `GALLIUM_DRIVER=d3d12` 声明不会因 `xlings update` 消失。在那之前,mcpp 侧的无条件 `set` 意味着这些用户**连 `export GALLIUM_DRIVER=llvmpipe` 自救都做不到**。

### 2.5 「issue 的归因经常是错的,包括维护者自己写的」

本轮有 **8 条** issue 的核心事实主张被推翻或修正:

| issue | 被推翻的主张 | 实际 |
|---|---|---|
| #289 | 「pin 只有一个消费点(打印)」「获取点按存在性早退」 | PR #378 已全面重写,现有 5 个消费点 |
| #371 | 「CDB 只在构建成功后才写」 | `ninja_backend.cppm:1549` 写 CDB,`:1664` 才 spawn ninja |
| #259 | 「llvm xim 包漏声明 glibc 依赖」 | `llvm.lua:23-40` 一直声明着(作者自己在评论里已retract) |
| #393 | 「`make_preferred` 归一化替换值即可」 | `outputDir.string()` 本来就是纯原生,按此修法写完测试会「过」而 bug 不动 |
| #380 | 「包模板渲染器有同样的死循环风险」 | `template.cppm:139` 是 `pos += to.size()`,不会重扫 |
| #284 | 「partial 版本在 remove 里已经能解析」 | `resolve_version_match` 在 remove 全函数体零调用 |
| #290 | 「版本维度在解析时未知,需要延迟合并」 | `packageVersion` 早就是 `synthesize_from_xpkg_lua` 的形参 |
| #396 | 「规则 A 可以从第一天硬失败」 | `our_glibc >= host_glibc` 是上界代理,会误杀能跑的二进制(实测:host=2.43 上用 2.39 载荷链 `libz.so.1`(只要 GLIBC_2.14)完全正常) |

**这直接决定了「已修复 ⇒ 关闭」这个动作的成本**:issue 正文的行号平均漂移了两到三个版本,而其中相当一部分主张在写下时就不准确。

---

## 3. 关闭判断:为什么本轮 0 关闭

流程上,每条判为「可关闭」的结论都会派两个独立反驳者:一个查代码事实,一个站在原报告人视角问「这样关掉他会不会觉得没解决」。两条候选各被 2:0 推翻。

### 3.1 #313(7 条产品建议合集)

- **落地为真的 3 条**已复核:Homebrew tap(`homebrew-mcpp` 活跃,formula 已跟到 `2026.8.8.4`,`bin.mkpath` 首装 ENOENT 已修)、模板机制(`--template` + e2e + docs)、短命令(仅 xlings 侧)。
- **推翻理由 1**:关闭留言给的两条「现在就能用」的方案**在报告人自己的 Mac 上都跑不通**。
  - 短命令(建议 4)是 xim 包 `mcpp-short-cmd`,靠 `xvm.add()` 注册,**必须先装 xlings**。而 brew formula 的 `def install` 只写 `bin/mcpp`;install.sh 与 AUR 同样只把 `$PREFIX/bin` 进 PATH,xlings 躺在 `$PREFIX/registry/bin/`。四条安装通道里三条拿不到这个能力,报告人恰好在其中一条上。(顺带:`install.sh:13` 的自述 `$PREFIX/bin/{mcpp,xlings}` 是过时的。)
  - 「复用本机 LLVM」(建议 2)的逃生门 `[toolchain] default = "system"`,在没有 `$CXX` 时兜底到**字面量 `g++`**(`probe.cppm:249-254`)。macOS 上 `which g++` = Apple Clang;Homebrew 的 llvm 是 keg-only 且 bin 下没有 `g++` 这个名字。而 mcpp 默认模板用 `import std`,Apple CLT 给不出可用的 std 模块清单。**这条路在报告人平台上大概率直接失败。**
  - 而且这条逃生门**从未被验证过能构建成功**——`14_toolchain_fallback.sh:39-40` 自己写着「we don't verify success, only that the hard-error path doesn't fire」。
- **推翻理由 2**:三项「归位」承诺全无落地物。help 着色的小 issue 还没开;#260 是留言板不是特性追踪器;承诺的 cmake Agent Skill 在 `.agents/skills/` 里不存在(只有 `mcpp-usage` / `mcpp-contributing` / `mcpp-release`)。**先关合集再承诺开新的,顺序反了。**

**关闭前置条件**:(a) help 着色单开并落地;(b) 建议 7 要么落地 Skill 要么明确写 WONTFIX 并指向 cmake2mcpp;(c) 建议 2 明确 dedupe 到 #144 并在 #144 里写清逃生门的真实兜底;(d) 建议 5 若折进 #260,#260 里得留下记录。

### 3.2 #177(支持 cmake/make/xmake 项目)

- **推翻理由 1**:复现步骤今天原样失败(实测,mcpp 2026.8.8.4):`error: path dependency 'cjson' (at '...') has no mcpp.toml`。git 与 path 两种形态走同一分支(`prepare.cppm:3861-3866`),`git log -S"has no mcpp.toml"` 找不到任何把这条硬拒改成设计决策的 commit。
- **推翻理由 2**:「维护者已明确否决」是误读。2026-06-27 08:18 否决的是「把构建工具融入 mcpp」;11:02 报告人主动把提案收窄成「只在没有 mcpp.toml 时代跑一次上游构建工具」;11:19 维护者对收窄后提案的回复是「可能要看能不能导出标准布局的目录结构」+ xmake `package/manager` 链接——**这是可行性探讨,不是否决**。
- **推翻理由 3(最要命)**:拟发留言里有三处会被当场证伪的错误,其中一条是「`[runtime] library_dirs` 烤的是 RUNPATH,不是 `-L`」——而**同一 tracker 里 #304 是 OPEN 的,标题就是它进 `-L`**。贴出去等于和一条 open issue 打架。
- **推翻理由 4**:「给它写 12 行描述符」差一个数量级。同段引用的两个先例是 `compat.mysql-connector-cpp.lua`(328 行)与 `compat.openssl.lua`(466 行);最简单的 `compat.cjson.lua` 也有 64 行,还要用户自算 sha256、写双镜像 URL、写三套 OS 的 xpm 表。而 `docs/10-publishing-a-library.md` 里 `compat.` / `Form B` / `xpm` 命中数 **0**——让用户去写一个 mcpp 自己不文档化的 Lua schema。

**结论**:#177 要关只能以 **WONTFIX** 的名义关(它是 `enhancement` 标签),不能包装成「能力已经有了,只是文档没跟上」;且必须先补一章「接入非 mcpp 的 C/C++ 库」的文档。#144 应改标题重新立题为「外部/自定义工具链声明」,与 #276 归到同一条 roadmap。

---

## 4. 逐条分析

> 格式:**根因**(可能与 issue 归因不同)→ **当前代码**(file:line,全部为本轮实读)→ **修法** → **处置**

### #396 · form-X 二进制的链接期物理检查 · VALID_OPEN · P2/L

**根因**:不是「mcpp 有个 bug」,而是「mcpp 处在唯一能看见这件事的位置上,却什么都不看」。mcpp 把每个用户二进制放进形态 X(自带 loader + 自带 libc,且我们的 `ld.so` 编译进去的 cache 路径在任何机器上都不存在 ⇒ 无宿主回退)。链接期是唯一一个 PT_INTERP、RUNPATH、DT_NEEDED 闭包三者同时在场的时刻。

**当前代码**:唯一的物理检查是 `hermetic.cppm:103 verify_hermetic_link`(由 `ninja_backend.cppm:1579` 在跑 ninja **之前**调用)。它 `-###` 干跑,只看 CRT 目标文件(`:187`)和最后一个 `--dynamic-linker`(`:189`);允许前缀是**整个 xpkgs 根**(`:118`),所以 2.39 的 loader 配 2.44 的 libc 原样通过。不看 `-rpath`、不看 `-l/-L`、不读产物 ELF。`src/` 下 `getconf|host_glibc|GLIBC_2` 零命中。

**两条对 issue 原设计的修正(决定实施顺序)**:

1. **规则 A 不能用 `our_glibc >= host_glibc` 硬失败**——那是上界代理不是物理。实测(host glibc 2.39):`libz.so.1` 最高只要 `GLIBC_2.14`、`libtinfo.so.6` 要 `2.33`、`libexpat.so.1` 要 `2.38`。在 host=2.43 的机器上用 2.39 载荷链 `libz.so.1`,按 issue 字面规则会被硬拒,而那个二进制跑得好好的。精确判据是 `our_glibc >= max(闭包里每个宿主对象 .gnu.version_r 的最高 GLIBC_x.y)`,可精确算出、永不误杀。
2. **不要把规则 A 建在 `subos_info.host_glibc` 上**——今天一台机器上都没有这个键。xlings 侧确实实现了(`manifest.cppm:87/398/442`、`platform.cppm:195-212`),但 (a) mcpp 内嵌 pin 是 `2026.8.8.1` < 2026.8.9.1,沙箱 xlings 根本不会写;(b) 即使 bump,xlings **不回填**(`subos.cppm:161` 块有效即原样返回)。本机实测 `~/.mcpp/registry/subos/*/.xlings.json` 与 `~/.xlings/subos/*/.xlings.json` **全部无此键**。写了就是死代码。

**规则 B 在今天是构造性成立的**:`linkmodel.cppm:346-357` 的 `crtDir`/`libDirs`/`loader` 同取一个 `pp.glibcLib`,而 `pp` 由唯一权威解析(`probe.cppm:399` 明确拒绝按目录序猜 libc)。能打破它的三条都在 flags 层之外:用户 `ldflags` 自带 `-rpath`、依赖包的 `[runtime] library_dirs` 直接进 RUNPATH(`flags.cppm:719-722`)、以及 `post_install.cppm:380-388` 仍按目录序选 glibc 写进 clang.cfg。所以规则 B 值得作为**回归护栏**,但不是今天正在流血的伤口。

**修法**:阶段 0(读 `host_glibc` + mcpp 自己的 live probe,绝对路径 `/usr/bin/getconf`)→ 阶段 1(新模块 `src/build/elf_physics.cppm`,规则 B,用 patchelf 而非 readelf——`xim-x-gcc` 载荷**没有** readelf,`doctor.cppm:365` 那句「always present in our sandbox」不成立)→ 阶段 2(规则 A 精确式)→ 阶段 3(硬失败开关)。**不要复用 `[build] allow_host_libs`**:那个键名比它管的范围(CRT + loader 两项)大得多,绑在一起会把两件事混为一谈。

---

### #393 · `${mcpp.*}` 混合分隔符 · VALID_OPEN · P3/M

**根因**:引擎变量的展开拼写没有单一规则。`prepare.cppm:5045-5047` 三个 token 用 `.string()`(原生),`prepare.cppm:5063` 的 `${mcpp.target_file:}` 用 `.generic_string()`。**同一个封闭词表两种拼写。**

**issue 的建议是空操作**:`ctx.plan.outputDir.string()` 本来就是纯原生(`prepare.cppm:209` 全程 `operator/`),`make_preferred` 什么都不改。按此修法写完测试会「过」,bug 一动不动。

**真正活着的泄漏通道是 `a.command`**,不是 issue 猜的 file 字段:outputs/inputs/objects 三条路都被 `escape_ninja_path` 的 `generic_string()`(`ninja_backend.cppm:76`)压平,CDB 被 #391 的 `native_string` 兜住;而 command token 在 `ninja_backend.cppm:1391` 逐字输出。文档旗舰示例 `docs/05-mcpp-toml.md:1252` 的 `.arg("${mcpp.out_dir}/blob.o")` 就是这条路。

**同一形状在 env 通道完整复制了一份**:`build_program.cppm:230-231` 把 `MCPP_OUT_DIR`/`MCPP_MANIFEST_DIR` 设为 `.string()`,文档和 6 个 e2e(188/124/144/187/193/194)都写 `std::string(mcpp::out_dir()) + "/gen.cpp"`。`directives.cppm:668-670` 只绝对化 inputs/outputs,`a.command` 从不过那一层。**一个从不写 `${mcpp.…}` 的 build.mcpp,照样会在 Windows 上把混合路径塞进命令行。**

**修法**:统一到 **generic**(`/`)——三条现有先例:`${mcpp.target_file:}` 本来就是、ninja 节点名一律 generic(`ninja_backend.cppm:65-77` 已论证过)、`escape_flag_path` 同理由。改 `prepare.cppm:5045-5047` 三行为 `.generic_string()`,`build_program.cppm:230-231` 同改。**不要对 `a.command` 的整个 token 做 path 归一化**——token 可能是 `--cpp_out=${mcpp.out_dir}/gen` 这种 flag。同时把 `substitute` 从 `prepare_build` 体内的 lambda 提成可单测的具名函数(它是 lambda 正是这类缺陷能潜伏三个月的直接原因),不变量锁「四个 token 展开成同一拼写」而不是「out_dir 别混」。

---

### #392 · WSL2 私有 glibc 2.39 撑不起系统库 · PARTIAL · P1/M

**根因分两层,issue 把它们混在一起了**:

**(A) 报告人机器上「产物绑 2.39」的直接原因不是 clang.cfg**,而是他的 `~/.xlings/subos/default/.xlings.json` 里 `subos_info.runtime` 本来就写着 `glibc@2.39`(来自 xlings ≤2026.8.8.x 的 `DEFAULT_RUNTIME`)。上游 2026.8.9.1 已把默认与 `latest` 改成 2.44 并在 commit message 里点名本 issue;**但 mcpp 的 `kXlingsVersion` 仍 pin 在 `2026.8.8.1`,这个修复还没到达 mcpp 用户。**

**(B) mcpp 自己的两处真缺陷**:

1. **fixup 仍在猜 glibc**——`post_install.cppm:380-388 find_sandbox_glibc_lib` 注释写「the newest installed version」,实现是 `directory_iterator` 取第一个带 loader 的目录,**没有任何排序**。它决定写进 `clang.cfg` 的 `-B/-L/--dynamic-linker/-rpath`(`:314-321`)。构建路径已在 PR #378 改成权威制(`probe.cppm:399` 拒绝按目录序猜),cfg 这条没跟上——**「mcpp build 出来的产物」和「人直接敲 clang++ 出来的产物」可以指向不同的 glibc**。本机正是这个状态(xpkgs 下 2.39 与 2.44 并存)。
   > 报告人观察到的「把 2.39 改名 2.39.off 仍被选中」正是目录序而非语义序的直接证据——`"2.39.off" < "2.44"`。
2. `mcpp run` 把 toolchain 库路径塞进子进程 `LD_LIBRARY_PATH`,打死应用内所有 bash 调用(xdg-open / notify-send / gio trash)。

**修法**:【1】抬 `src/xlings.cppm:47` 的 pin 到 ≥2026.8.9.1(`check_version_pins.sh` 会自动校验其余 pin 点,别手维护列表)。验证判据是「新建 subos 的 `.xlings.json` 里 `runtime == glibc@2.44`」,**不是「命令退 0」**;且 xlings 明确只影响新建 subos,老 subos 不迁移。【2】`ensure_post_install_fixup` 加 `runtimeBinding` 形参,binding 非空 ⇒ 直接定位;为空 ⇒ **按语义化版本取最新**(复用 `lifecycle.cppm:213` 的 `version_greater`),并把 `:378` 那句撒谎的注释改成实际行为。

---

### #386 · `sources = []` 被静默忽略 · VALID_OPEN · P2/M

**根因**:数据模型里缺一位「是否声明过」。`types.cppm:170` 的 `sources` 是裸 `std::vector<std::string>`,消费方只能拿 `.empty()` 当「用户没写」的代理——而 `sources = []` 恰好也是空。`libs/toml.cppm:480-490` 的 `get_string_array` 对 `[]` 返回 engaged 的空 vector(不是 `nullopt`),信息在这一层就丢了。同一 `.empty()` 代理在 **5 处**各推导一遍。

叠加第二层:「自动扫描」是两条互不知情的通道——源 glob 走 manifest 的 `sources`,目标推导走 `toml.cppm:1501/1506` 对文件系统的直接探测,推导出的 entry 又在 `plan.cppm:1197-1198` 被合成为绕过 scanner 的编译单元。**关掉源 glob 也关不掉目标推导。**

第三层(用户真正卡住的地方):「可被依赖」与「不认领脚下文件」今天是互斥的。有 `[package]` 才能被依赖,有 `[package]` 就必然认领 `src/**`。

**今天就能用、且比维护者给的方案 A 更贴合用户需求的形状**(实测跑通,应先回帖):

```toml
[package]
name = 'spdlog-workspace'
version = '0.1.0'

[build]
sources = ["!src/**"]          # 未文档化的偏方,但今天就生效

[dependencies.spdlog]
path = 'cmake2mcpp_generated/spdlog'

[workspace]
members = ['cmake2mcpp_generated/spdlog']
```

根建出空产物、成员正常编译、别的项目依赖它也通过。**唯一前提**:根 `src/` 下不能有 `main.cpp`、不能有任何 `.cppm`(否则 `toml.cppm:1499` 的目标推导照样开火)。spdlog 恰好满足。
> 维护者的方案 A(纯虚拟工作区)满足「能构建」但不满足用户明确提出的「能被其他项目依赖」——用户的反驳是对的,实测:`error: dependency 'mylib' resolved to package '' (mismatch with declared name 'mylib')`。

**修法(窄规则,零回归)**:在 `Manifest`(不是 `BuildInputs`,避免波及 `:195` 的 `merge_inputs` 追加语义)加 `bool sourcesDeclared`;`toml.cppm:1475` 的 `.empty()` 改 `!m.sourcesDeclared`;`toml.cppm:1499` 的目标推导加同一道门;`xpkg.cppm:1864` 的 Form B 也要区分「没声明」(仍报错)与「声明为空」(放行)。顺带收敛 `prepare.cppm:3209` 那份已漂移的默认 glob(4 项 vs 7 项,**走多版本 mangling staging 的包今天就会漏掉汇编源**)。

---

### #382 · subos `op="set"` 的语义 · PARTIAL · P2/M

**主症状(包侧)已修**(xim-pkgindex #563 mesa 25.0.7.2 带 d3d12+iris、#565 驱动缺失时不强制)。**残留在 mcpp 侧**:

**根因**:`op="set"` 在 xlings 的语义是**声明默认值**(set-if-unset),mcpp 读成了**命令**(无条件覆盖)。而且这个误读被 `c1e9360` 用一条断言 + 一大段注释固化下来,理由是「op 词汇表是 xlings 的契约,消费方不得擅改」。**这个理由本身是对的,错的是它对契约的认定**——xlings 的四个后端(`xlings/src/core/subos.cppm:1012/1120/1143/1166`)恰恰全是条件赋值:

| 后端 | 代码 |
|---|---|
| POSIX | `: "${VAR:=value}"; export VAR;` |
| fish | `if not set -q VAR; set -gx VAR …; end` |
| pwsh | `if (-not $env:VAR) { … }` |
| in-process | `else if (existing.empty()) { set_env_variable(...) }` |

所以现在的代码不是「守住了契约」,而是**以守契约之名违反了契约**:同一个 subos,`xlings subos use` 进去用户的 export 保留,`mcpp run` 进去被覆盖——正是那段注释自己声称要避免的分歧。

**代码里为「不修」给的两条理由,一条错、一条反过来支持修**:(a)「subos 需要 `set` 表达用户不得覆盖」在当前生态**零实例**——整个 xim-pkgindex 里 `op="set"` 只有 `wsl-gl-host-link.lua:298` 一处,其余全是 `prepend`,而唯一那处恰恰是用户需要能覆盖的那个;(b)「消费方不得给 op 加第二种含义」是对的,但它指向的是**现在**的代码。

**修法**:改 `subos_info.cppm::resolve_env`(`:250-299`)为 xlings 的两段式:先纯 manifest 折叠(`set` 存在则 `set` 赢、prepends 全丢;否则 prepends 按 provider 逆序去重拼接),再对着 ambient 应用(`prepend` 接在前;`set` **ambient 存在就整个让位**)。判据用「**有没有**」而不是「**空不空**」——两个消费点的 lambda 正好是 `getenv` 的 `optional`(`execute.cppm:369-372/891-894`),对齐 xlings 2026.8.8.2+ 的 `env_is_set`。**`73ab179` 那版用的是 `amb && !amb->empty()`,不要照抄**(那对应 xlings 2026.8.6.1 的旧语义)。

> 这个误读来回了三次:`73ab179`(两半都修)→ `1cc1052`(撤回 set)→ `c1e9360` 合入(只剩 prepend),而推翻撤回理由的第 3 条评论比 `c1e9360` 晚 33 分钟。**修的时候把 xlings 那四个行号写进注释和测试**——这是唯一能阻止第四次的东西,光写「set 是默认值」没用,上一版就是这么写的。

---

### #380 · `mcpp new` 名字契约缺失 · VALID_OPEN · P2/M

**根因**:同一个字符串被两个互不相识的消费者直接吃掉,中间没有任何归一化或校验层。`cmd_new.cppm:24` 拿到 `positional(0)` 后除了 `name.empty()` 一个字符都不看,就同时交给文件系统路径(`current_path() / name`,`create.cppm:183` 与 `:217` 两处推导)和模板数据(`create.cppm:266-269` 手写 find/replace)。

死循环是**独立的实现缺陷**:那段循环每轮 `find` 都从位置 0 重新开始,替换值含 needle 时不是幂等而是自我再生。**仓库里已经有一个写对了的替换器**(`template.cppm:139` 的 `pos += to.size()`),builtin 骨架却手抄了一个写错的版本。

**issue 有一条主张不成立**:包模板渲染器**不会**挂死。但它有另一个真 bug——`template.cppm:142-144` 是三遍独立全文扫描,后一遍会命中前一遍刚插进去的内容;实测 `projectName = "{{self.name}}"` 时 `name = "{{project.name}}"` 渲染成 `name = "imgui"`(静默错值)。

**修法**:在 `mcpp.scaffold` 导出单一校验器 `validate_project_name`,在 `cmd_new.cppm:28` 之后、第 40 行分流之前**唯一一次**调用(这样 builtin 与包模板天然共用同一契约)。正向文法 `[A-Za-z_][A-Za-z0-9_-]*`,错误消息里把文法和被拒字符都打出来。**含 `.` 直接拒**并引用 `docs/spec/package-identity.md §3.2`——实测 `mcpp new foo.bar` 今天 rc=0 且 `mcpp build` 成功,即 mcpp 会给用户生成一个**本地能编、一发布就不合规**的 manifest,这比死循环更可能被真实用户撞到。删掉手抄的错替换器,builtin 改用 `template.cppm` 的那份(并顺手修串扰)。四处 `std::ofstream` 全部检查流状态。

> 零测试覆盖:`tests/unit` 下无任何 scaffold 测试,`02_new_build_run.sh:11` 只跑 `mcpp new hello`。**这些不是回归,是从来没被测过。**

---

### #379 · 机器可读输出协议 RFC · PARTIAL · P2/L

**已落地**(PR #385 / `55a39d9` / 2026.8.8.4):阶段 0(stdout 归属)、阶段 1(`src/wire.cppm` 信封 + `--protocol-version`)、阶段 2(`--format` 归一)、阶段 3a(`self env` / `xpkg parse` / `cache list` 三个 kind)。

**三处提案被有意改掉,而且每一处都改对了**:

1. 未知 format 从「stdout + envelope」改成 **stderr + rc 2 + stdout 一字不写**。理由(`wire.cppm:251-254`):「一个还不知道会被给什么格式的请求,不该往协议拥有的通道里写东西;客户端在 stdout 上找不到 JSON 就已经得到答案了」。客户端判据相应改为**正向识别**(解析 stdout,要求 `schemaVersion` + `kind`)——因为 `--protocol-version` 在所有早于它的 mcpp 上**恰恰就是**「spawn 一个可能失败的命令」那种情况(实测:打印 `Error: unknown option` 到 **stdout** 且 rc=1)。
2. `destructive` 布尔 → 命名 **effects 集合**(`InitMcppHome/ReadProject/WriteProject/WriteGlobalCache/Network/ExecBuildScript`)+ 放进 `--protocol-version` 的静态表。bool 既做不了执行前 gate,也分不开四种边界。
3. `--json` **不做拼写归一**,永久保留 legacy 裸 payload——拼写兼容 ≠ payload 兼容。

**缺口不是惯性,是实施计划自己划的范围线**(`2026-08-08-wire-protocol-implementation-plan.md:19-21` 明写阶段 3b/3c 不在本次范围)。剩下 7 条,建议各开独立 issue、全部立项后再关 #379:

1. `self env --format json` 补齐字段(W4-Step2 **计划内漏做**:`env_data_readonly()` 没有 index repos / default toolchain,而 vscode 侧正是基于「覆盖全部需求」才同意删掉自己的 home 推算逻辑)。约束:不能改用 `config::load_or_init`(会创建 6 项)。
2. `mcpp build --configure-only [--format json]` + `invalidatedBy`。**前置**:必须先做第 5 条,否则 xlings 的索引刷新输出会污染这条命令的 stdout。
3. `mcpp metadata [--resolved] --format json`。
4. **CDB 原子写**(`compile_commands.cppm:306` 仍是 `std::ofstream os(path); os << content;`,非原子、不检查失败)。
5. stdout 归属收敛(`xlings.cppm:414-421` 的 `print_status`、`package_fetcher.cppm:1036-1037` 硬编码的 `/*quiet=*/false`)。判据不能写「e2e 绿」——现有 e2e 用 `_inherit_toolchain.sh` 预置工具链,索引永远命中,**结构性覆盖不到这条路径**。
6. 每个 kind 的 `data` golden 测试(`tests/fixtures/wire/` 是**空目录**;今天把 `mcppHome` 改名不会让任何测试变红,而 `docs/11-machine-output.md:196-204` 已把它写成公开承诺——这正是 RFC §4 拿 xlings 20/20 空 outputSchema 当反例要防的东西)。
7. manifest supported-key 词汇表出口。

**关键窗口**:mcpp-vscode 侧全仓 grep `--format json` / `--protocol-version` / `xpkg parse` / `self env` **零命中**。wire v1 还没有真实客户端压过,**现在改字段是零成本**。

---

### #374 · 非模块 lib 误报缺模块根 · VALID_OPEN · P2/S

**根因**:`has_lib_target` 与「有模块根」被当成了同一件事,而它们正交——`kind="lib"` 回答**产物形态**,`src/<name>.cppm` 回答**接口形态**。`docs/05-mcpp-toml.md:369` 自己写的是 "Default convention",代码执行的是义务。这条判据引入时(`1ffd275`, 2026-05-09)mcpp 还没有 compat/Form-B 包,所有 lib 都是模块库,前提当时恰好为真;compat 生态进来后前提塌了,判据没跟着走。

**当前代码**:`validate.cppm:133-156`,行号未漂移。

**影响面比 issue 写的更宽,但触发条件更窄**:更宽——`xpkg.cppm:1870-1878` 在 `targets` 为空时自动补 Library target,所以**所有** Form-B compat 包都命中(不只显式写 `kind="lib"` 的 protobuf);更窄——普通消费者构建**不会**看到它(唯一调用点 `prepare.cppm:4896` 只验主 manifest,已实测),真正的泄漏口是 host 工具子构建(`prepare.cppm:4441/:4471`)与包自身当主工程构建(mcpp-index CI)。**写 PR 描述时别沿用「每个消费者每次构建」的说法。**

**修法有一个必踩的陷阱**:新谓词必须用 **限定名** 比对。`scanner.cppm:789-791` 给 `u.packageName` 填的是 `namespace.name`;照抄 `u.packageName == manifest.package.name` 会对所有 namespaced 包判成「没有模块接口」,把 warning 全局静默——症状变成「issue 修好了」,实际是又踩了同一个坑。**这个坑今天已经吞掉了另一条检查**(§5.4)。

---

### #373 · 扫描器不剥块注释 · VALID_OPEN · **P1**/M

**根因不是「少了一个 `strip_block_comments`」**,而是扫描器用「一行一行、一个 pass 一种语法」去近似 C++ 的词法状态机。raw-string / 块注释 / 行注释 / 普通字符串字面量是**同一层的互斥词法状态**,谁先出现谁生效;拆成串联的独立 pass,每加一种就必然引入一批新误判。

**「仅告警」这个定级是错的,实测两种硬后果**:

- 把旧实现整段用 `/* */` 注释掉、里面留着 `export module X;`(而 `X` 真由同包另一文件提供)⇒ `mcpp build` **直接拒绝启动**:`error: scanner errors: … module 'mylib' already provided by …`(`scanner.cppm:938-946`)。触发它的是「注释掉一段旧代码」这种最日常的动作。
- 块注释里孤零零一句 `export module future;` ⇒ 构建成功,但 build.ninja 里该 TU 变成 `build obj/main.o | gcm.cache/future.gcm : cxx_object`,声明了一个永不产出的 BMI。实测 `ninja -n` 仍要重跑 OBJ+LINK(对照组是 `no work to do`)——**该 TU 及其下游永久增量失效**,而 mcpp 层的工程级 fast path 会把这个症状盖住。

**修复必须连带解决三件事**(单独修块注释会更糟):

1. **顺序已经是错的,而且是假阴性**:`scanner.cppm:588-589` 先 `strip_raw_strings` 后 `strip_line_comment`。只要 `//` 注释里出现一个未闭合的 `R"(`(写文档解释 raw-string、贴示例都会),`in_raw` 悬挂到文件末尾,**后面所有 `import` / `export module` 全部消失**。实测两例:真 `export module mylib;` 被吞 ⇒ 消费者收到 `module 'mylib' imported but not provided`(一个明明存在的模块);`import totally.real.module;` 被吞 ⇒ 一路到 g++ 才报 `failed to read compiled module`。
2. **普通 `"..."` 字面量里的 `/*` 是必踩的地雷**:mcpp 自己的树里就有——`distribution.cppm:417` 的 `"/* Generated by mcpp. …"`,配对 `" */\n"` 在 `:426`。一个不认识字符串字面量的块注释 pass 会吞掉 417-426。安全阀:**普通字符串状态必须行内即抛、绝不跨行**(它本来就不能无转义跨行)。
3. `scan_overrides` 本该是包侧逃生口,但被 `toml.cppm:422-425` 的「既不 provides 也不 imports 就报错」挡住(mcpp-index `compat.abseil.lua:114-132` 已把这件事写成 `-- KNOWN WARNING` 注释,结论与本 issue 一致)。**允许 `scan_overrides` 显式声明空结果**是一个低成本的独立改进,能给所有第三方源码包一个立刻可用的出口。

**修法**:把 `:137-189` 两个 helper 折叠成一个单遍词法状态机 `strip_noncode(line, in_raw, raw_close, in_block)`,状态优先级严格照 C++ 词法自左向右(in_raw > in_block > 代码态;代码态里 `//` 立即返回,**绝不能让本行后面的 `R"` 再改 `in_raw`**)。**方法论提示**:所有变体都能在 `tests/unit` 层用 `scan_file()` 直接断言,不需要真编译——先把 9 条单测写进去看红成什么样(至少两条今天就红),再动 helper。

**当前可用的绕过(可回复报告者)**:`MCPP_SCANNER=p1689`(`prepare.cppm:4876-4886`)切到 g++ 驱动的 P1689 扫描,实测告警消失。但它是实验开关,代码注释指向的 `docs/27` **在 docs/ 下并不存在**(docs/ 只到 `11-machine-output.md`)——这个悬空引用也该顺手清掉。

---

### #371 · IDE 模型与预构建 CDB · PARTIAL · **P1**/M

**issue 把三件事捆成一个 RFC,而这三件事后来各自走散了**:

1. **一个不成立的前提**——「CDB 只在构建成功后才写」。实际 `ninja_backend.cppm:1549` 写 CDB,`:1664` 才 spawn ninja,自 #24 起就是。#379 §1.6 后来实测推翻了它。
2. **一个真实但被误述的成本问题**——不是「编不过就拿不到 CDB」,而是「为了拿 CDB 要付一次完整的依赖构建 + 编译 + 链接」,以及「prepare 阶段失败就一份 CDB 都没有」。**这才是唯一还站得住的核心诉求。**
3. **一个协议层**——已在 #379/PR #385 以 `src/wire.cppm` 落地,只是没姓 `ide`、没用 NDJSON 事件流。

更深一层:**CDB 是 `NinjaBackend::build()` 的副产品,而不是一个具名 configure 阶段的产出**。生命周期被绑在「跑一次完整构建」上,才同时长出三个症状。

**修法(裁到一条主线,按性价比排序)**:

- **第 0 步(P0,~10 行,应立刻单独发)**:修 fast path 不重建 CDB(§5.1)。
- 第 1 步:`write_compile_commands` 改成可失败 + 原子写(与 #379 第 4 条同一件事)。
- 第 2 步:`--configure-only`。**`BuildOptions::dryRun` 是死代码**(`backend.cppm:14` 声明、`ninja_backend.cppm:1567` 读取——写完 CDB 就 return——全仓无任何写入方)。也就是说执行路径已经存在且在正确位置,**缺的只是一根从 CLI 到它的线**。这一点在 #379 的实施计划里没被指出,会让人高估阶段 3b 的工作量。
- 第 3 步:CDB 覆盖 `tests/**` + dev-dependency 上下文。
- **不做**:`mcpp ide` 命名空间、NDJSON 事件流、独立 ID 体系——已被 wire v1 覆盖或不需要。

---

### #370 · 全预发布键缺精确 pin 提示 · VALID_OPEN · P2/S

**根因**:`pin_hint` 的触发条件绑在一个**语法事实**(「这个键 `parse_version` 解不动」),而真正该触发它的是**语义事实**(「任何范围都够不到任何候选」)。预发布键语法合法且可排序 ⇒ 落进 `literals` 分支;同时 SemVer 的预发布可见性规则(`version_req.cppm:313-318/341`)让不含预发布的范围对它恒不可见。**错误信息的分支结构照抄了解析器的分类,而不是用户的实际处境。**

**当前代码**:`resolver.cppm:187-193` 三分类;`:202-207` 全仓唯一一份提示,文案还写死「These keys are not ordered versions」;`:230-239` 的 `best.empty()` 分支只在 `!unorderable.empty()` 时附加提示。

**三个包的键均已复核**:`compat.glad` = `0.0.0-651a425`、`compat.tray` = `0.0.0-8dd1358`、`compat.re2` = `2022-04-01`(issue 写的预发布标识 `04.01` 是笔误,实际是单个标识符 `04-01`——照抄进测试断言会对不上)。

**修法**:新增一条**并列**的 `prerelease_pin_hint`,**不要复用 `pin_hint` 的文案**(「不是有序版本」对预发布是错的:它们恰恰有序,只是被可见性规则挡住)。同时把 `unorderable` 那条的 `keys.front()` 改成取最高可用键——两条腿可以同时成立。

**顺手修上游入口**:`mcpp add <pkg>@'*'` 会毫无提示地写进一个永不可解析的依赖(`commands.cppm:55` 让 `warn_unpublished_version` 对任何范围直接早退)。实测 `mcpp add 'acme:glad@*'` 输出 `Adding acme:glad v*` 并写入 manifest,要到下一条 `mcpp build` 才炸。**用户最可能的操作序列正是这个**,加同一条告警成本近乎为零。

---

### #313 · 7 条产品建议合集 · PARTIAL · P3/S

逐条状态:

| # | 建议 | 状态 |
|---|---|---|
| 1 | Homebrew 分发 | ✅ 已交付(`homebrew-publish.yml` / `f4a7568` / tap 跟到 2026.8.8.4) |
| 2 | 复用本机已装 LLVM/GCC | ❌ 只有未文档化的逃生门,无自动识别 → 应 dedupe 到 #144 |
| 3 | help 一级标题着色 | ❌ 未做(S 级,唯一便宜且无争议的一条) |
| 4 | 命令别名 | ⚠️ 仅 xlings 用户可用;**brew / install.sh / AUR 三条通道拿不到** |
| 5 | #260 做成命令 | ❌ 未做,且 #260 是留言板不是特性追踪器 |
| 6 | 模板项目 | ✅ `--template` / `--list-templates` + e2e + docs(imgui-m 0.0.6 带 `templates/{docking,window}`) |
| 7 | CMake ↔ mcpp 互转译 | ❌ 承诺的 Agent Skill 不存在 |

**技术根因只有建议 2 有**:mcpp 的工具链身份是 `(family ∈ gcc|llvm|msvc, version, target)` 两轴闭词表(`compat.cppm:110-151 normalize_spec`),「系统已装工具链」被塞成 family 上的特例(`registry.cppm:369-371` 的 `is_system_toolchain` 只认 MSVC)。**「本机已有一份可用编译器」在当前模型里没有可命名的身份。**

处置见 §3.1。

---

### #304 · `runtime.library_dirs` 落到链接行 · VALID_OPEN · P2/M

**根因**:一个键被同时用来表达两件不同的事,而只有一件写进了名字和文档。`flags.cppm:720-723` 在发 `-Wl,-rpath,` 的同时顺手发了 `-L`。三者本来可分:

| flag | 作用 | 谁需要 |
|---|---|---|
| `-Wl,-rpath,<dir>` | 运行期 | ✅ 唯一被需要的那一半 |
| `-Wl,-rpath-link,<dir>` | 解析直接链接的 `.so` 的传递 DT_NEEDED | ✅ 链接期唯一正当需求 |
| `-L<dir>` | 暴露给显式 `-lfoo` | ❌ 无人需要,全部危害来源 |

**实测三方对照**(binutils 2.42 与 ld.lld 22.1.8 结果一致):只给 `-rpath-link` ⇒ 传递依赖解析成功;只给 `-rpath-link` 而写 `-lbar` ⇒ `cannot find -lbar`(即 rpath-link **不**参与 `-l` 解析);只给 `-rpath` ⇒ 传递依赖找不到。

**所以「保留链接期能力」和「不遮蔽别人的库」不是取舍,是一行 flag 拼错了。** issue 给的两个选项之外存在第三个更好的答案。

**比 issue 说的更严重**:`flags.cppm:884-885` 的拼接顺序把 `runtime_dirs` 放在 `user_ldflags` **之前**,所以即使用户在 `[build] ldflags` 里显式写 `-Lvendor`,runtime 目录仍然赢。**连用户自己指定的路径也遮蔽。**

**修法**:`flags.cppm:720-723` 的 dep 目录那段把 `-L` 换成 `-rpath-link`;`:712-715` 的 `plan.toolchain.linkRuntimeDirs` **保持 `-L` 不动**(那是工具链私有 libc/libgcc 目录,`-lc`/`-lm`/`-latomic` 真的靠它解析)。**两段必须区别对待,别一把改。**

---

### #293 · e2e 写穿符号链接损坏真实工具链 · PARTIAL · **P1**/M

**根因**:测试沙箱的边界是「名字」而不是「物理位置」。所有下游代码拿到的都是**拼写上落在沙箱里、物理上落在用户家目录**的路径,它们各自都没有理由怀疑这一点。这是**乘法成本**的缺陷:每新增一个会写 payload 的动作,就多一条写穿路径。

**没变的部分(缺陷主体)**:`tests/e2e/_inherit_toolchain.sh:22-37`,`:33` 仍是 `ln -sf`;整个文件自 `75cf6c0`(2026-06-01)未动过。**48 个 e2e 脚本 source 它。**

**已经关掉的部分(mcpp 自己的改写器)**:`post_install.cppm:26-59` 的 `containment_root`/`escapes_containment`、`:95-102` 的 patchelf 逐文件围栏、`:278-284` 的 cfg 围栏、`:485-497` 的入口所有权守卫(来自 `dea5f1f`, PR #275)。`fixup_gcc_specs` 已整个删除。

**时序更正(影响优先级排序)**:#273 的围栏(2026-07-24)**早于** #293 提交(2026-07-27)。所以 issue 里那次 24 个可执行文件被改 PT_INTERP 的事故,要么用的是围栏之前编出来的 mcpp,要么根本不是 mcpp 的 `patchelf_walk` 干的(**更可能是 xim 的 elfpatch**)。**mcpp 侧再加围栏收益有限;真正能挡住的是改 helper + 加完整性校验。**

**修法**:

- **A**(必做,S):`link_xpkg_payloads()` 从「整包软链」改成「硬链接农场(`cp -al`)+ 真拷可变小集(`bin/`、`libexec/`、`specs`、`*.cfg`)」。硬链接对 mcpp 自己的 patchelf **已经安全**(`post_install.cppm:113-147` 已改成 copy+rename 会断链)——**这是本次核验里最有价值的一个变化,它让 A 方案从不可行变成可行**。`|| cp -r` 降级分支必须保留。
- **B**(必做,S):`run_all.sh` 跑套件前后各采一次 payload 指纹(只看 PT_INTERP + RUNPATH)并 diff,失败时指名哪个 payload。**损坏是静默的、跨运行的**,这是它能长期存在的原因。
- **C**:注意别把 `install_integrity.cppm:189-195` 的 `looks_complete_legacy()` 一起改掉——`doctor.cppm:613` 的 `clean_all_incomplete` 在符号链接沙箱里本该是灾难,是这层保护让那 42 个无 marker 目录幸免。

---

### #290 · xpkg 描述符缺版本条件轴 · VALID_OPEN · P2/M

**根因**:条件化构建输入有两条通道,求值时机决定各自能表达什么——平台轴是**解析前的文本拼接**(`xpkg.cppm:1143-1148`),target triple 轴是**解析后的结构化合并**(`prepare.cppm:462-472`)。版本轴两条都没接上。**但这不是「时机上做不到」**:`packageVersion` 早就是 `synthesize_from_xpkg_lua` 的形参(`xpkg.cppm:1130`),构建路径上永远是 `resolve_semver` 之后的具体值。根因是当年加平台块时只把 os 接进了拼接点,没有把它一般化。

**issue 把成本估高了一个数量级**:不需要 Manifest 暂存、不需要新的延迟合并阶段、连 `append(BuildInputs&,…)` 都用不上——**十几行文本拼接就够**。真正的成本在 lint 闭包(`--all-versions`)和索引 floor 抬升的发布流程上,issue 一个字没提。

**举例已作废,建议换掉**:`ggml-org.llamacpp` 现在是单版本 Form A,历史上从没有过 mcpp 块。换成 `compat.catch2`(2.13.10 vs 3.15.2,两套完全不同的源码布局,注释自己写着「THIS DISJOINTNESS IS THE LOAD-BEARING PREMISE」)+ mcpp-index#187 那个 `undefined symbol: Catch::Session::Session()` 的真实故障。

**受众可以收窄**:自己能控制打包的库(tinyhttps 9 版、xpkg 15 版、imgui 6 版)走 Form A,把 mcpp.toml 塞进每个 tag 的 tarball,版本差异天然随源码树走。真正卡住的是 `compat.*`——sha256 pin 的原始上游 tarball,塞不进任何东西。**本 issue 的受众是「compat 层的多版本包」这一个明确子集(今天 8 个描述符)。**

**修法**:`kKnownXpkgKeys` 加 `"version"`,在平台拼接**之后**追加版本拼接(标量键靠后写覆盖,版本应赢过平台),解析循环加 skip 分支。**匹配语义必须以字面键相等为主形态**——索引里活着的键包括 `b10069`、`1.92.8-docking`、`2026.08.08`、`latest`,`version_req::matches` 对它们一律失败。

---

### #289 · 沙箱 xlings 永不刷新 · PARTIAL · P2/M

**issue 的三条核心主张全部被 PR #378(`fdad165`)推翻**:`acquire_xlings_binary` 现在接收 pin、读实际版本、严格落后才替换,且替换前先给候选源定价(避免用系统的 0.4.51 覆盖 2026.8.2.1);`doctor.cppm:405-418` 加了同一项检查;pin 漂移由 `.github/tools/check_version_pins.sh` 在 CI 强制(本地实跑 `OK: xlings pins all at 2026.8.8.1`, exit 0)。

> 讽刺的是,**issue 里最有价值的那半条(「Adjacent, same shape」的 pin 一致性)反而先被修好了**,而且长成了比原提议更强的东西:除了 xlings pin,还管住 mcpp 自身版本的四处一致性,并把「bootstrap pin 不得超前于在建版本」写成可执行判据。

**残留两洞,都落在「修补放在控制流到不了的地方」这个形状上**:

1. **Windows 上整套机制空转**:`xlings_binary.cppm:160` 把 `2>/dev/null` 硬编码进一条经 `cmd.exe /c` 执行的命令 ⇒ 命令不执行 ⇒ 返回空串 ⇒ acquire 退回旧的 early return、doctor 退化成一条 warn。**issue 点名的受害平台恰恰是修复不生效的平台。** 仓库里已有 `mcpp::platform::null_redirect` 就是为这件事准备的。
2. **自带副本这条最可靠的来源没接进来**:候选源只有 `MCPP_VENDORED_XLINGS` 和 `which xlings`(`:214-223`)。而正是「机器上的 xlings 很旧」这个前提,决定了 `which xlings` 在最需要自愈的场景里也是旧的 ⇒ 打一行 Note 就放弃,尽管一份与 pin 逐字一致的 xlings 就躺在 `<install>/registry/bin/xlings`(实测 2026.8.8.4 包内为 2026.8.8.1)。

**副作用**:`doctor.cppm:411-417` 的「It is replaced automatically on the next `mcpp self init`」在上面那个分支里是错的。仓库里遗留的 `tests/e2e/doctor.log` 就是现成复现——同一次运行第 16 行说「no newer source is available (keeping it)」、第 25 行说「会自动替换」。

**守卫必须在本平台真跑一次探针**,而不是再写一遍字符串比较(后者在 Linux 上照样绿)。

---

### #284 · `toolchain remove` 拒绝空格分隔写法 · VALID_OPEN · P2/S

**根因**:同一个「双写法 + partial 版本」的用户契约在三个子命令里各自独立推导了一遍,remove 那份只推导了一半——CLI 声明层(`cli.cppm:421-425` 只一个位置参数)、路由层(`cmd_toolchain.cppm:48` 只传 `positional(0)`,而 `positional(1)` **里其实已经有值**)、领域层(`lifecycle.cppm:703-705` 签名少一个参数,且**全函数体零调用** `resolve_version_match`)。

**issue 有一条归因是错的,会带偏修复**:「since partial versions already resolve here」不成立。实测两个 15.x payload 都在场时 `remove gcc@15` 直接 `not installed`。**按 issue 字面只做「加位置参数 + 抄 default 的 help 文案」,会得到一个文案承诺 partial、行为不支持 partial 的命令,比现状更糟。** 修复必须包含 partial 解析。

---

### #283 · target pin 静默压过显式默认 · VALID_OPEN · P2/M

**根因不是「判据写窄了」**,而是全局 config 的 `[toolchain] default` 是一个**双写键**:用户(`mcpp toolchain default`)和 mcpp 自己(首次运行持久化,`prepare.cppm:1697/1717/1835`)写的是同一个格子,**配置层没有 provenance 字段能把二者分开**。#332 为了保住「mcpp 可以修正自己写下的旧默认」,只能把 `GlobalDefault` 判为非用户显式(`prepare.cppm:651-653`),并用 `test_windows_defaults.cpp:75` 钉死。`prepare.cppm:1769-1774` 的注释已经亲口承认过这个缺口。

**更硬的一半(issue 没提)**:在 `mcpp.toml` 里写死 `[toolchain] linux = "gcc@15.1.0"`,然后 `mcpp build --target x86_64-linux-musl` —— `tcOrigin = ManifestToolchain`(用户显式)但 `targetFromGlobalDefault = false`(target 是命令行给的)⇒ `pinWouldOverruleUser = false` ⇒ `prepare.cppm:1441` 照样覆盖。**连写进 mcpp.toml 的显式工具链都保不住**,只要 target 是显式给的。issue 的 workaround(改用 `[target.X]`)还能救,但用户完全没有理由预期 `[toolchain]` 会输给一个 pin。

**修法(必须先补 provenance,不能直接让全局默认与 `[target.X]` 同权)**:`GlobalConfig` 加 `defaultOrigin`(缺省 `"auto"`,老配置向后兼容),`lifecycle.cppm:687` 传 `"user"`、三处首次运行传 `"auto"`;`TcOrigin` 新增 `UserGlobalDefault` 并算作用户显式。**同时补一条诊断**:pin 真的覆盖了用户设置时必须说话,而不是静默。

---

### #276 · 嵌入式 Linux SDK 集成 RFC · VALID_OPEN · P2/XL

**根因**:不是缺机制,是**缺输入通道**——而通道之所以缺,是因为 mcpp 的工具链模型只承认一个权威:**编译器二进制自己**。`detect()`(`detect.cppm:37-125`)把 triple(`:81`)、sysroot(`:112`)、std 模块源(`:105-109`)全部从同一个二进制推导。这在「mcpp 自带整套工具链」的世界里正确且优雅,但嵌入式场景要求解耦「用什么编译器生成机器码」(应来自 mcpp)和「产物将运行在哪个世界里」(必须来自设备 rootfs)。

**地基已经全在**:`linkmodel.cppm:29-30` 的 `CLibMode::Sysroot`、`:81-83` 编译侧 `--sysroot=`、`:106-109` 链接侧 `--sysroot=` + `--dynamic-linker=` + `-L/-rpath`、`:405-408` 的 `else if (!tc.sysroot.empty()) sysroot_mode(...)`;`hermetic.cppm:130` 外部 sysroot 自动放行(**不需要 `allow_host_libs`**)。**只要 `tc.sysroot` 有值,整条链路自动就绪。**

**最小切入点 = P0「sysroot 缝」,一个字段贯穿三个文件**:

1. `TargetSection` 加 `std::filesystem::path sysroot`,`toml.cppm:1155-1160` 的 `[target.<triple>]` 循环加一行解析,并在 `:1010` 的合法键白名单登记(否则未知键让整份 manifest 报错)。
2. `--sysroot <path>` 写进 `BuildOverrides`,供 CI / Buildroot 的 `.mk` 直接注入。
3. **唯一写入点必须放在 `detect` 之后**(`prepare.cppm:1729` 与 `:1841` 两次 detect 之后)——`detect.cppm:112` 会无条件覆盖它。
4. **⚠️ fingerprint 必须同时加轴,不加就是回归**。

**今天就能试的路径(issue 与评论都没提)**:`[toolchain] = "system"` + `$CXX` 已经能注入外部交叉编译器,并连带采纳它自己的 sysroot(`prepare.cppm:1556-1558` → `probe.cppm:249-253` → `probe.cppm:330-336`「foreign but usable sysroot beats no sysroot」→ `linkmodel.cppm:405-408` → `hermetic.cppm:112-116` 整体早退)。**厂商 SDK 的 gcc ≥ 15(带 `bits/std.cc`)今天就能试。**

---

### #259 · 裸机 `toolchain install llvm` 后 clang exec 127 · VALID_OPEN · P2/M

**三层根因,和 issue 正文的归因完全不同**:

- **表层(索引侧,已排除)**:`llvm.lua:23-40` 一直声明着 `xim:glibc`,现在还是 `>=2.39` 并新增 `xim:gcc-runtime@15.1.0`。「包漏声明」不成立(作者自己已 retract)。
- **中层(xlings 侧,仍在)**:`installer.cppm` 里 dep 节点的两类失败(`:2266-2269` load_package 失败、`:2313-2316` 下载资源解析失败)是 `log::warn` + `continue`,不进 `plannedDownloads`、不 emit `InstallPhase::Failed`;`commands.cppm:645` 的退出码只看 `failedCount` ⇒ **依赖被丢掉、主包照装、elfpatch 照样把不存在的 glibc loader 烙进 PT_INTERP、退出码 0**。
- **深层(mcpp 侧,这才是它该留在 mcpp 仓的理由)**:mcpp 对「刚装完的工具链能不能跑」一个断言都没有,而它自己写的三层兜底全是空转:
  1. `lifecycle.cppm:559-564` / `prepare.cppm:1668-1670` 传无版本的 `"xim:glibc"`,被 `package_fetcher.cppm:932` 的 `<name>@<version>` 门当场拒掉,**自引入至今一次都没装成过任何东西**,返回值一处 `(void)` 一处只 `log::debug`;
  2. `post_install.cppm:441` 找不到 glibc 时静默跳过 patchelf(gcc 分支 `:422-427` 会 warn,**llvm 分支连 warn 都没有**);
  3. `lifecycle.cppm:576-582` 安装后只 `exists(bin)`,不 exec。

**修法**:F1 删掉两个空转循环(评论已证明 mcpp 没有绕过 xlings 的依赖解析,即使能跑也是重复劳动;而在 mcpp 里钉死 glibc 版本常量会立刻和上游打架——`latest` 正在 2.39→2.44 动);F2 在 `lifecycle.cppm:582` 之后、打印 `Installed` 之前对 `bin` 跑一次 `detect()`,失败时用 `read_elf_interp(bin)`(`post_install.cppm:534`)读 PT_INTERP 给出可执行的诊断,而不是把 `probe.cppm:107` 的裸文案透出去。

**CI 缓存复刻了 issue 正文点名的那个坑**:`ci-linux-e2e.yml:104-166` 的 `hermetic` job 环境是对的(`debian:stable-slim`,显式断言 `! command -v gcc`),但它 `actions/cache` 了 `~/.mcpp` 且带 `restore-keys: mcpp-hermetic-`。**一旦某次跑绿并存了缓存,之后每次 llvm 都是「已经装好」的,冷装路径再也不被覆盖**——和「本机永远注意不到,因为之前装 gcc 已经 park 了 glibc」是同一失效模式,只是从开发机搬到了 CI。

---

### #256 · clang 20/22 模块 BMI 毒化名字查找 · PARTIAL · P2/S

**归因正确,mcpp 不背这个锅。** ask 1(文档)扎实:`docs/03-toolchains.md:348-399` 的 "Known Toolchain Hazard" 段含毒性形状、安全形状、workaround、回链与 canary 指路,中文镜像在 `docs/zh/03-toolchains.md:335`。

**ask 3(canary)存在但有三个洞**,而且都在「修补放在控制流到不了的地方」上:

1. **control 步骤只 precompile 不 import**(`150_...sh:92-95`)。`ctl.pcm` 产出后再没被引用,而注释明确声称它守「导入侧必定能编过」。实测该断言今天在 22.1.8 上 rc=0——**可写、为真、就是没写**。
2. **ACTUAL 仅由退出码推导**:上游把 SIGSEGV 换成一条正当诊断时,canary 会继续全绿。应改成崩溃指纹(`PLEASE submit a bug report` / `Stack dump`),非零退出但无指纹应当报第三态并 FAIL。
3. **未登记的大版本 `exit 0`**:索引一旦把 llvm `latest` 推到 23,canary 在 200 个测试的输出流里以 OK 通过。**ask 3 的原话是「a future clang bump that fixes (or re-breaks) this is visible」,这条路径恰好把它变成不可见。**

**再叠加**:`ci-linux-e2e.yml:90/93` 只装 gcc,缓存血统里唯一的 llvm 是旧版——**这个 canary 在 CI 里守的不是默认工具链版本**。

**ask 2(上游 LLVM 报告)至今没有任何记录。** 建议:mcpp 侧三处小改(A/B/C)可以一个 PR 收掉;上游追踪转成 issue 正文的一行链接,不必因此保持 open——但**在 canary 修好之前不要关**,否则「未来 clang 修好了」这件事没有任何人会知道。

---

### #215 · cppfly 加 Clang 反射行 · UPSTREAM_BLOCKED · P3/S

**触发条件未满足,实机核过**:`xim-x-llvm/22.1.8/bin/clang++ -freflection -std=c++2c -fsyntax-only` → `unknown argument: '-freflection'`。表 `cppfly.cppm:94-99` 只有 GCC 16 一行,未动过。

**真正的问题不是「该不该加行」,而是「没有任何机制会告诉你表已经过期了」**:今天所有声称「clang 没有反射」的测试断的都是 mcpp 自己表的输出——`test_cppfly.cpp:80` 的输入是手搓的 `Toolchain` 结构体(连编译器进程都不启动),`101_cppfly_llvm_soft.sh:49` grep 的是 mcpp 自己打印的 summary(而那行内容正由那张表决定)。**两条断言都是同义反复。**

**修法**:(A) 把 issue 正文的触发条件改成机器可判的谓词(「最新 llvm payload 的 clang++ 在 `-std=c++2c` 下定义 `__cpp_impl_reflection`」);(B) 加 `102_cppfly_reflection_canary.sh`,**反转断言极性**——探到就 FAIL 并指出该改哪张表。

**加行之前必须先修一处**:`cppfly.cppm:161-164` 的 gate 循环只比 `rule.family` 就 `break`(版本判断在循环外),而 `latest_std_canonical`(`:142-148`)是 `continue` + 循环内比较、注释还写着「rows per family ordered newest-first」。**给 Clang 加第二行(上游拼写 vs fork 拼写)的那一刻,第二行会静默不可达**,而单测因为只有单行 fixture 也照样绿。

**同一形状的当下真风险**:`100_cppfly_reflection.sh:16-19` 用「最新已装 gcc payload 是否接受 `-freflection`」做前置探测,不通过就静默 `SKIP-INLINE`。**GCC 17 若把 `-freflection` 改名或默认开启,唯一那条硬路径反射 e2e 会静默停跑。**

---

### #177 + #144 · 外部构建系统 / 本机工具链 · PARTIAL · P2/L

**共同缺口**:mcpp 只有「自己下载并完全掌控」这一种资源获取模式,**凡是「外部已经存在的东西」都缺少一个申报入口**。

- 工具链轴(#144):`Family` 是封闭 enum(`registry.cppm:30`),`is_system_toolchain` 只认 MSVC,manifest `[toolchain]` 只收 `family@version` 不收路径。唯一外部入口是那个没写进文档、也进不了全局默认的 `"system"` 字面量。
- 库/sysroot 轴(#177 + #276):`CLibMode::Sysroot` 机制已在,但来源写死在 `probe_sysroot`;没有 pkg-config;git/path 依赖必须自带 mcpp.toml。

**#177 今天的三条可用路径**:`[build] include_dirs` + `ldflags`(**注意:没有 `library_dirs`/`link_libs` 键**)、`build.mcpp`(可 `std::system()` 起 cmake/make/xmake,再用 `mcpp:include-dir=`/`link-search=`/`link-lib=` 喂回构建图,配 `rerun-if-changed` 做增量——**这就是报告人要的形状,已实测端到端跑通**)、compat 描述符 + 项目本地索引 `[indices]`。

**但这三条都不构成关闭理由**(见 §3.2):字面复现今天原样失败;「维护者已否决」是误读;`docs/10-publishing-a-library.md` 里 `compat.`/`Form B`/`xpm` 命中数为 0。

**处置**:#177 → 先补一章「接入非 mcpp 的 C/C++ 库」的文档(写清三条路线的选择判据),再以 WONTFIX 关闭字面需求;#144 → 改标题为「外部/自定义工具链声明」,与 #276 归入同一 roadmap,P0 是把 `"system"` 逃生阀从「事实存在」变成「产品特性」(文档化 + `mcpp toolchain default system` 放行 + 外部工具链身份进 fingerprint)。

---

## 5. 顺带发现:issue 一个字没提的 30 条

按「是否比所属 issue 更该先修」排序。**前 6 条建议脱离原 issue 单独立项。**

### 5.1 🔴 P0 · fast path 命中时 CDB 永不重建 —— 今天就在破坏 mcpp-vscode

`execute.cppm:705` 的 `try_fast_build` 新鲜度门只比对 `build.ninja` 的 mtime 与 mcpp.toml / 源码(`:757-764`),**不检查任何产物是否存在**;命中后 `:768-774` 直接 `run_ninja_fast` + `return 0`,`NinjaBackend::build()` 整个不执行,`write_compile_commands` 一次都不跑。

```
$ mcpp new demo3 && cd demo3 && mcpp build      # 成功,CDB 生成
$ rm compile_commands.json
$ mcpp build ; echo $?                          # "Finished dev in 0.00s" ; 0
$ ls compile_commands.json                      # No such file  ← 且此后每次都如此
```

**触发条件很日常**:`mcpp new` 写的 `.gitignore` 只有 `target/` 和 `.mcpp/`,`compile_commands.json` 未被忽略 ⇒ `git clean -fd` 删掉它、保留 `target/` ⇒ 下一次 `mcpp build` 命中 fast path ⇒ **CDB 再也回不来**。

而 mcpp-vscode(`src/extension.ts:105,128-136`)整个流程是「CDB 不存在 → 提示 Run mcpp build → 用户点 Build → 再检查」。**这个组合让扩展陷入死循环:构建每次都成功,提示每次都还在。**

**修法**:`execute.cppm:764` 那串 freshness 检查后加一条产物存在性检查。同一形状在 `try_fast_run`(`:788` 起)也在。**~10 行。**

### 5.2 🔴 P1 · 扫描器的 strip 顺序本身就是一个正在跑的假阴性

见 #373 §。`//` 注释里一个未闭合的 `R"(` 会让 `in_raw` 悬挂到文件末尾,**后面所有 `import`/`export module` 全部消失**。写文档解释 raw-string、贴一段带 raw-string 的示例都会触发。**这比 #373 报的块注释问题严重:那个是假阳性(多报),这个是假阴性(漏报)。**

### 5.3 🟠 P1 · `--no-color` 在 TTY 下是空操作

- `cli.cppm:107`:`--no-color` → `ui::disable_color()`(argv 预扫描,早于任何输出)
- `ui.cppm:239`:`disable_color() { g_color = false; }` —— **只改 `g_color`,不置 `g_inited`**
- `ui.cppm:233-237`:`init() { if (g_inited) return; g_color = detect_color(); g_inited = true; }`
- 每个输出入口都先调 `init()`

时序:`--no-color` 置 false → 第一条 `ui::status()` 调 `init()` 发现 `g_inited` 仍 false → 重新 `detect_color()` → **在 TTY 上又变回 true**。

**没人发现的原因**:CI 和所有 e2e 都通过管道捕获,`detect_color()` 在非 TTY 下本来就返回 false —— **「无色」这个结果是对的,但原因是错的**。`MCPP_NO_COLOR` / `NO_COLOR` 走 `detect_color()` 内部,不受影响;只有 `--no-color` 这一条路径坏了。修法一行:`{ g_color = false; g_inited = true; }`。

### 5.4 🟠 P1 · `[modules].exports` 检查对任何 namespaced 包是静默空操作

`validate.cppm:98` 比的是 `u.packageName == manifest.package.name`,而 `scanner.cppm:789-791` 填的是**限定名**。于是 `namespace="acme" / name="lib1"` 时 `"acme.lib1" != "lib1"`,`actual` 永远是空集,**整个 exports 校验(含 `strict`)一条都不触发**。

实测:`[package] name="lib1", namespace="acme"` + `exports = ["totally.not.this"]` + `export module acme.lib1;` → `mcpp build` **通过,零诊断**;删掉 `namespace` 那行,同一工程立刻报 `module 'acme.lib1' is exported by code but not listed in [modules].exports`。

成因可追:`scanner.cppm:786-788` 的注释说这个限定名是为了让「模块名必须以包名为前缀」的检查工作,而那条检查在 `0b8b81b` 里被删了,留下的 `:98` 就悬空了。**修 #374 之前必须先看懂这条**,否则会原样再踩一次。

### 5.5 🟠 P1 · 两个「看起来在做事」的空转循环

`lifecycle.cppm:559-564` 与 `prepare.cppm:1668-1670` 的 `{"xim:glibc","xim:linux-headers"}` 预装循环传的是无版本 spec,被 `package_fetcher.cppm:930-935` 一进门就拒。**自引入至今一次都没装成过任何东西**,一处 `(void)` 丢弃、一处只 `log::debug`。glibc 实际是靠 gcc 的 xim 依赖顺带装上的,所以症状被遮住了。

### 5.6 🟠 P1 · Windows 上 xlings 版本探针整套空转

见 #289 §。`2>/dev/null` 经 `cmd.exe /c` 不成立 ⇒ 返回空串 ⇒ 两个消费点都退化。仓库里已有 `mcpp::platform::null_redirect` 就是为这件事准备的。

### 5.7 其余 24 条(按 issue 归属)

| 归属 | 发现 | 级别 |
|---|---|---|
| #393 | `MCPP_OUT_DIR`/`MCPP_MANIFEST_DIR` 用 `.string()`,`a.command` 从不过归一化层 ⇒ **不写 `${mcpp.…}` 的 build.mcpp 照样在 Windows 上产出混合路径** | P2 |
| #393 | 含 `${mcpp.` 的 `role="source"` 输出被 `prepare.cppm:2969` 整体排除在源集外 ⇒ **产物永远不会被编译,零诊断**,而 `docs/07-build-mcpp.md:222-224` 承诺「畸形 action 是硬错误,绝不静默跳过」 | P2 |
| #386 | `prepare.cppm:3210` 的默认 glob(4 项)已与 `toml.cppm:1476`(7 项)漂移 ⇒ **走多版本 mangling staging 的包今天就漏掉汇编源** | P2 |
| #380 | `template.cppm:142-144` 三遍独立全文扫描 ⇒ 占位符串扰,静默错值 | P2 |
| #380 | `template.cppm:156` 的 `recursive_directory_iterator(…, ec)` 接住 `ec` 从不判读 ⇒ 模板目录读不了时零迭代、返回成功、打印 "Created" | P2 |
| #380 | `mcpp new foo.bar` rc=0 且能构建,但违反 `docs/spec/package-identity.md §3.2` ⇒ 生成**本地能编、一发布就不合规**的 manifest | P2 |
| #304 | `flags.cppm:884-885` 把 `runtime_dirs` 拼在 `user_ldflags` **之前** ⇒ 用户显式 `-Lvendor` 也被压过 | P2 |
| #304 | macOS 上 `[runtime] library_dirs` **完全是死的**(`flags.cppm:873-874` 既不发 `-L` 也不发 `-rpath`),而 `execute.cppm:1370-1377` 还在警告「dependencies must be reachable through the binary's rpath」——mcpp 自己从没写过那个 rpath | P2 |
| #304 | 同一个键在四个后端有四种链接行含义(ELF `-L`+`-rpath` / MSVC `/LIBPATH:` / clang-MSVC-ABI 与 MinGW 什么都不发 / macOS 什么都不发),文档只有一句「进 RUNPATH」 | P3 |
| #370 | `mcpp add <pkg>@'*'` 静默写进永不可解析的依赖 | P2 |
| #370 | `best.empty()` 的消息完全丢掉 `aliases` ⇒ 「只有 alias + 预发布键」的包不会被告知 `= "latest"` 可行 | P3 |
| #374 | 限定包名三处独立推导且算法不一致 ⇒ `namespace="acme"`+`name="acme.lib1"` 时 `plan.cppm:1054` 静默失配(gtest_main.cc 这类 entry object 该丢不丢) | P2 |
| #290 | `target_cfg` 里写 `cfg(version=…)` 能过解析但 `match_kv` 不认 ⇒ **永远不生效且不报错** | P2 |
| #290 | mcpp-index CI 只跑 `mcpp xpkg parse` 不带 `--all-os` ⇒ windows 段的 typo 在索引 CI 上隐形 | P3 |
| #289 | `load_or_init()` 现在每条命令都 spawn 一次 `xlings --version`(实测 134ms,无缓存)⇒ 对 `self env` 这类瞬时命令是纯加价 | P3 |
| #289 | `doctor.cppm:411-417` 的补救文案在「无更新源」分支里是错的(`tests/e2e/doctor.log` 第 16 vs 25 行自相矛盾) | P3 |
| #293 | 别把 `install_integrity.cppm:189-195` 的 `looks_complete_legacy()` 一起改掉——`doctor.cppm:613` 的 `clean_all_incomplete` 在符号链接沙箱里靠它才没酿成灾难 | ⚠️ |
| #396 | `xim-x-gcc` 载荷**没有** readelf,`doctor.cppm:365` 的「always present in our sandbox」不成立;patchelf 才是两边都保证有的 | P3 |
| #396 | `[build] allow_host_libs` 的名字比它管的范围(CRT+loader 两项)大得多,做 #396 时**别复用这个键** | ⚠️ |
| #379 | `xpkg parse --format json` 失败契约自相矛盾:坏 descriptor 时 stdout 是合法信封但错误在 `data.error` 里是人类文本、`diagnostics` 为 `[]`;另外三个失败分支 stdout **一字不写** ⇒ 客户端把「descriptor 写错」误判成「mcpp 太老」 | P2 |
| #379 | 静态效应表与实际信封对不上(`self env` 声明 `["init-mcpp-home"]`,实际信封 `[]`、创建项数 0) | P3 |
| #379 | 第四种机器输出拼写:`mcpp test --message-format json`,不带信封、不走 wire,而契约文档一个字没提——**它恰恰是 CI 最可能消费的出口** | P2 |
| #379 | `--protocol-version` 是裸 argv 扫描(`cli.cppm:629-635`),`mcpp new --template --protocol-version` 会被劫持 | P3 |
| #379 | 未知命令时 `print_usage()` 进 **stdout**(rc=127),与协议 stdout 归属冲突 | P3 |
| #215 | `cppfly.cppm:161-164` 的 gate 循环不支持同族多行(`break` 而非 `continue`),与 `latest_std_canonical` 语义不一致 ⇒ **加 Clang 第二行的那一刻它会静默不可达** | ⚠️ |
| #215 | `100_cppfly_reflection.sh:16-19` 是会自我关闭的守卫:GCC 17 改名或默认开启 `-freflection` ⇒ **唯一那条硬路径反射 e2e 静默停跑** | P2 |
| #256 | `ci-linux-e2e.yml` 只装 gcc,canary 守的不是默认工具链版本 | P2 |
| #259 | `ci-linux-e2e.yml:104-166` 的 hermetic job 缓存 `~/.mcpp` 且带 `restore-keys` ⇒ **冷装路径跑绿一次后再也不被覆盖** | P2 |
| #373 | `prepare.cppm:4876-4886` 的注释指向 `docs/27`,**docs/ 下不存在**(只到 `11-machine-output.md`) | P3 |
| #373 | `scan_overrides` 被 `toml.cppm:422-425` 的「既不 provides 也不 imports 就报错」挡住,包侧没有逃生口 | P2 |

---

## 6. 建议执行顺序

### 批次 A · 独立小修(总计约 60 行,可一个 PR 收掉)

1. fast path 产物存在性检查(§5.1)—— **10 行,今天就在破坏已发布扩展**
2. `disable_color()` 置 `g_inited`(§5.3)—— 1 行 + 一个直连 `ui::` 的单测(**不能靠现有 e2e,无色也过 = 假绿**)
3. `xlings_binary.cppm:160` 换 `null_redirect`(§5.6)—— 1 行 + 一个**在本平台真跑探针**的单测
4. 删掉两个空转的 sysroot 预装循环(§5.5)
5. `toolchain remove` 补位置参数 + partial 解析(#284)
6. `resolver.cppm` 加 `prerelease_pin_hint` + `mcpp add` 侧同一告警(#370)
7. `validate.cppm` 加「一个模块接口都没有 ⇒ 不告警」谓词(#374)—— **必须先修 §5.4 的限定名比对**

### 批次 B · 需要设计的单点

8. **scanner 单遍词法状态机**(#373 + §5.2)—— 先写 9 条单测看它们红成什么样,再动 helper
9. `sourcesDeclared` 一位 + 两条自动扫描一起关(#386)—— 顺带收敛 `prepare.cppm:3210` 的漂移
10. `subos_info::resolve_env` 两段式重写(#382)—— **把 xlings 那四个行号写进注释和测试**
11. `flags.cppm:720-723` 的 `-L` → `-rpath-link`(#304)—— `linkRuntimeDirs` 那段保持不动
12. `mcpp new` 单一校验器(#380)
13. `e2e/_inherit_toolchain.sh` 改硬链接农场 + `run_all.sh` 加 payload 指纹校验(#293)

### 批次 C · 需要新字段/新契约

14. xlings pin → ≥2026.8.9.1 + fixup 接权威(#392)—— 验证判据是新 subos 的 `runtime`,**不是命令退 0**
15. `[toolchain] default` 加 provenance(#283)
16. `[target.X].sysroot` 字段 + `--sysroot`(#276 的 P0 切入点)—— **fingerprint 必须同时加轴**
17. xpkg 描述符版本条件块(#290)—— 十几行拼接,成本在 lint 闭包和发布流程
18. wire 阶段 3b/3c(#379 / #371)—— `dryRun` 执行路径已存在,缺的只是从 CLI 到它的一根线

### 批次 D · 关闭前置动作

19. #313:help 着色单开并落地 → 建议 7 落 Skill 或明确 WONTFIX → 建议 2 dedupe 到 #144 → 再关
20. #177:补「接入非 mcpp 的 C/C++ 库」文档章 → 以 WONTFIX 关字面需求;#144 改标题重新立题
21. #256:canary 三处修好 → 上游追踪转成正文链接 → 可关
22. #379:7 条各开独立 issue → 全部立项后关,或就地降为「wire v1 已冻结」跟踪 issue

---

## 7. 方法论与证据边界

**做了什么**:26 个 open issue,除去用户指定保留的 #43 / #260,其余 23 条(#177 与 #144 合并分析)各派一个 agent 对照当前代码逐条核验;对判为「可关闭」的结论各派两个独立反驳者(一个查代码事实、一个站原报告人视角),**不确定时默认推翻**。累计 27 个 agent、1594 次工具调用。

**证据规矩**:每条事实主张必须落到当前 file:line 并引用真实代码;issue 引的行号平均漂移两到三个版本,一律重新定位。「已修复」必须能指出具体 commit / 现有代码,并说明「按 issue 的复现步骤现在会发生什么」。多条结论有实机复现(#380 的死循环与路径逃逸、#284 的能力矩阵、#304 的 `-rpath-link` 三方对照、#370 的 local index 复现、#371 的 CDB 消失、#215 的 `-freflection` 探测、#256 的 clang 22.1.8 SIGSEGV、#396 的 `.gnu.version_r` 读取)。

**全程只读**:所有临时工程写在 scratchpad,mcpp 仓库 `git status --porcelain` 全程为空。

**未做/边界**:
- 部分复现用的是本机已装的 mcpp 2026.8.8.2 而非 main 2026.8.8.4;每处都用 `git log -S` / blame 确认了涉及代码在两版之间未变动,结论对 main 成立。
- #290 的 `check_llamacpp_snapshot.py` 主张未核(未 checkout `mcpplibs/llama.cpp-m`),标 UNVERIFIABLE。
- #382 的 GPU 加速路径(mesa 25.0.7.2 的 d3d12/iris)本机无 WSL2 无 Intel GPU,payload 里有、依赖闭包干净但**从未被执行过**——只能由报告者验证。但这不构成保持 open 的理由。
- 所有 xlings 侧结论基于 `openxlings/xlings@2913a09` 本地检出,未跑 xlings 自身的测试套件。

---

*本报告由 mcpp 全量 issue 核验流程生成 · 基线 `main@80291ca` / v2026.8.8.4 · 2026-08-09*
