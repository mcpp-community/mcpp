# Linux → Windows MinGW 交叉工具链 — 设计方案

> 2026-07-15 · 基于 0.0.91(CommandDialect + MSVC 原生后端 + c++fly 已发布)代码审计
> 姊妹文档:`2026-07-13-toolchain-backend-abstraction-msvc-mingw-design.md`(§4.4/§7 登记本方案为延期项)、
> `2026-06-23-aarch64-musl-gcc-canadian-cross-rebuild.md`(canadian-cross 打包先例)、
> `2026-07-07-hermetic-toolchain-link-model-design.md`(链接模型)、
> 记忆:[[release-publish-pipeline]]、[[toolchain-dialect-and-mingw]]、[[aarch64-musl-static-ninja-closure]]

## 0. 一句话

把 `msvc-mingw-design` §4.4/§7 **登记但延期**的「Linux→Windows 交叉」补上——它验证工具链抽象层的
最后一维:**host ≠ target**(native MinGW 保持 host==target==Windows,交叉强制两者分离)。**后端零新增**
(复用 0.0.89 的 `kGnuDialect` 拼写 + `is_mingw_target` PE 链接模型,二者早已按 target 而非 host 求值),
工作量集中在三处:**A. 解除 host 门**(把 install/select 侧的「MinGW 仅 Windows」假设改成 host+target
组合)、**B. 自包含 tarball 打包**(从源码 canadian-cross 出 GCC-16 mingw-w64 **MSVCRT** 交叉链,xlings-res
双端镜像,与 llvm/musl-gcc 同模式)、**C. Wine 真实验证**(host 跑不了 PE,靠 Wine 端到端断言)。
CRT 选型**跟随 Rust**:Rust Tier-1 `x86_64-pc-windows-gnu` 链 MSVCRT、UCRT 另立 `gnullvm` 目标——
本期取 MSVCRT triple `x86_64-w64-mingw32`(恰与现 `abi.cppm:76` mingw→msvcrt 一致,无需细化),UCRT 登记为
triple 可选 follow。

## 1. 现状评估(审计结论,file:line 落地)

### 1.1 已有的、正确的抽象缝(交叉直接复用,零改)

| 抽象 | 位置 | 对交叉的意义 |
|---|---|---|
| PE 链接模型按 **target** 求值 | linkmodel.cppm:231-236 | `is_msvc_target \|\| is_mingw_target → CLibMode::None`;注释明说「so a future cross-compile resolves by what it builds FOR」——本方案就是那个 future |
| `is_mingw_target(tc)` | model.cppm:107-110 | triple 含 `mingw32` 即 PE 目标,与 host 无关 |
| `kGnuDialect`(命令拼写) | dialect.cppm:72-87 | GCC/Clang/MinGW 共享 gnu 方言;交叉链是 GCC 家族 → 拼写零差异 |
| cfg 谓词 `mingw → windows` | prepare.cppm:69 | `[target.'cfg(windows)'.build]` 对交叉产物正确求值(按解析后 target,非 host) |
| Windows 链接三坑已就位 | flags.cppm:364-400 | `-static` / `-lstdc++exp` / `-static-libstdc++ -static-libgcc`——Windows 分支按 target 命中,交叉原样吃 |
| musl 交叉的 `<triple>-g++` 前端机制 | registry.cppm:99-104,167-173 | 交叉前端解析的现成模板;`x86_64-w64-mingw32-g++` 套同一机制 |
| `BuildOverrides.target_triple` | prepare.cppm:403-411 | `--target x86_64-w64-mingw32` 入口已在 |
| 交叉下跳过 L3 build.mcpp | prepare.cppm:914-919 | host 构建程序不为 target 跑,交叉语义已对 |
| `abi.cppm` mingw → `msvcrt` | abi.cppm:76 | **恰好匹配本期 MSVCRT 选型**——无需 `msvc-mingw-design` §4.2 计划的 ucrt 细化 |

**结论**:抽象层已经把两条正交轴分开了——`CommandDialect` = **命令拼写**(gnu),`is_mingw_target`/linkmodel
= **链接语义**(PE)。交叉恰好落在「gnu 拼写 + PE 语义」交点,而这两点都已按 target 而非 host 决策。所以
mcpp 侧改动很小,真正的工作在打包与验证。

### 1.2 需要解除的 host 假设(本方案 Part A 的对象)

MinGW 目前是 **Windows 原生**工具链,install/select 侧硬编码了「host 必须是 Windows」:

1. **install 门**:`lifecycle.cppm:186-191` `mingw_wrong_host()` 在非 Windows host 硬失败(:190),
   `385`、`506` 亦引用。交叉要求 Linux host 能装。
2. **索引可见性**:`registry.cppm:270-271` `available_toolchain_indexes` 仅在 Windows host 列 `mingw-gcc`。
   交叉要求 Linux host 列 `mingw-cross-gcc`。
3. **doctor 探测**:`doctor.cppm:132-149` 探 `xim:mingw-gcc`,host 假设需并入组合判断。
4. **注册名与包名**:`registry.cppm:179-182` `mingw → mingw-gcc`(winlibs Windows zip)。交叉是**另一个包**
   (`mingw-cross-gcc`,Linux host 制品),不能复用同一 xim 名(host≠target,tarball 不同)。

这四处的共同根因:**工具链的 host 维与 target 维被 `is_windows()` host constexpr 合并了**。Part A 把判断从
「host==Windows」改为「(host, target) 组合」——native MinGW = (Windows, Windows),交叉 MinGW = (Linux, Windows)。

### 1.3 结论

延续既定原则(2026-05-15 §2.3):**值类型 traits + per-compiler provider + 中心查询点,不引入虚函数**。
交叉不新增后端,只是让既有 gnu 后端在 Linux host 上被允许、被发现、被解析。

## 2. 目标模型:host / target 二维化

### 2.1 核心:host≠target 一等公民

`Toolchain::targetTriple`(model.cppm)已是交叉/MinGW/MSVC 唯一 keying 字段。缺的不是数据结构,是
**决策点仍按 host 短路**。本方案确立不变量:

> 凡涉及「产物形态」的判断(链接模型、CRT、exe 后缀、DLL 部署、PE 校验)一律按 **target triple**;
> 凡涉及「工具能否在此机器运行/安装」的判断(install 门、索引可见性、前端候选)按 **(host, target) 组合**;
> **绝不**用裸 `is_windows()` host constexpr 代表产物形态。

`linkmodel.cppm:236` 已是前者的样板;Part A 把后者从 `mingw_wrong_host()` 的单维 host 检查升级为组合表:

```
支持矩阵(本期)
                 target: windows-msvcrt   linux   macos
host: linux            mingw-cross-gcc ✓   musl/gcc  —
host: windows          mingw-gcc(native) —        —
host: macos            —                  —        clang
```

### 2.2 CRT 选型:跟随 Rust,triple 编码 CRT

Rust 的处理是本期依据:`x86_64-pc-windows-gnu`(Tier-1)链 **MSVCRT**(`msvcrt.dll`),UCRT 未改这个目标而是
**另立** `x86_64-pc-windows-gnullvm`。mingw-w64 自身与 Debian 也用 **triple 编码 CRT**(`x86_64-w64-mingw32` =
msvcrt、`x86_64-w64-mingw32ucrt` = ucrt)。据此:

- **本期 target = MSVCRT triple `x86_64-w64-mingw32`**。理由:① 跟随 Rust Tier-1 的稳定选择;
  ② 恰与现 `abi.cppm:76` mingw→msvcrt 一致,**无需**改 abi;③ Wine 下最稳(`msvcrt.dll` 恒在,UCRT 需
  `ucrtbase.dll` 及一族 api-ms-win-*);④ 与 native winlibs(UCRT)**故意分成两个 triple**,而非二选一——
  用户可各取所需,artifact 不混淆。
- **UCRT 登记为 follow**:triple `x86_64-w64-mingw32ucrt`,Rust `gnullvm` 式独立目标;届时 abi.cppm 细化 ucrt
  (即 `msvc-mingw-design` §4.2 原计划,留给它自己那期)。

**注意与 native MinGW 的 ABI 分叉**:native = UCRT、cross = MSVCRT,所以「交叉构建的产物 ≠ 原生构建的产物」。
这是**有意**的——CRT 进 triple,两者是不同 target,不是同一 target 的两种构建。用户要 UCRT 交叉,等 follow。

### 2.3 明确不做(见 §9)

不引入虚基类后端;不改 native winlibs MinGW(Part B)分毫;不做 i686/arm64;不做 UCRT 交叉(本期);
不动 `mcpp pack` 的 PE 支持。

## 3. Part A — host≠target 一等公民(mcpp 侧改动,小)

| 步 | 内容 | 位置 | 验收 |
|---|---|---|---|
| A1 | install 门二维化:`mingw_wrong_host()` → `mingw_host_target_ok(host, target)`;native mingw-gcc 仍 Windows-only,mingw-cross-gcc 允许 Linux host | lifecycle.cppm:186-191,385,506 | 单测:四象限矩阵;native 路径零行为变化 |
| A2 | registry 新条目 `mingw-cross`:用户名 `mingw-cross`(或 `mcpp toolchain install mingw --target x86_64-w64-mingw32`);xim 名 `mingw-cross-gcc`;前端候选 `{"x86_64-w64-mingw32-g++"}`;`display_label` "mingw-cross {ver}";`matches_default_toolchain` 认 `mingw-cross@<ver>` | registry.cppm:99-109,167-182,234-255 | 单测:名映射;`mcpp toolchain list`(Linux)见 mingw-cross |
| A3 | 索引可见性组合化:`available_toolchain_indexes` 在 **Linux host** 列 mingw-cross-gcc、**Windows host** 列 mingw-gcc | registry.cppm:270-271 | e2e:Linux/Windows 各见对应项 |
| A4 | 检测走既有 GCC enrich:`x86_64-w64-mingw32-g++ -dumpmachine → x86_64-w64-mingw32` → `is_mingw_target` 命中 → gnu 方言 + PE linkmodel,全零改 | detect.cppm / model.cppm(无改预期) | 检测出 `CompilerId::GCC` + mingw triple |
| A5 | 约定解析套 musl 交叉模板:`--target x86_64-w64-mingw32` → 解析 `mingw-cross-gcc@<ver>` → 前端 `x86_64-w64-mingw32-g++`;交叉下 L3 build.mcpp 跳过(已有) | prepare.cppm:634-667,914-919 | e2e:交叉 build 产 `.exe` |
| A6 | archive/binutils 自包含:交叉 tarball 自带 `x86_64-w64-mingw32-ar` 于前端旁(同 musl「self-contained」) | registry.cppm:290-295 | 静态库 e2e |
| A7 | doctor 组合探测:Linux host 探 `xim:mingw-cross-gcc` | doctor.cppm:132-149 | `mcpp doctor` 报交叉链状态 |

flags/link **无新增分支**:Windows 目标分支(flags.cppm:364-400)按 target 命中,`-static`/`-lstdc++exp`/
`-static-libstdc++ -static-libgcc` 原样;交叉 sysroot 由 driver 自带(`<prefix>/x86_64-w64-mingw32/{include,lib}`
编进 GCC,**无需** `--sysroot`)。这是 A 段「小」的根本原因——链接语义早在 0.0.89 就位。

## 4. Part B — 自包含 xlings-res tarball(打包,主要工作量)

### 4.1 定位与选型依据(调研结论)

调研确认:**无现成的 GCC-16 Linux-host 自包含 tarball**——winlibs 只出 Windows-host、Debian/Ubuntu 卡在
GCC 13(无 `import std`)、只有 Arch `mingw-w64-gcc 16.1.0` 是发行版包(非自包含、仅 Arch)。mcpp 全线是
**hermetic 自包含链**(llvm/musl-gcc/winlibs 皆 xlings-res 制品),故走**从源码 canadian-cross 自出 tarball**,
与 [[aarch64-musl-gcc-canadian-cross-rebuild]] 同路子。

### 4.2 构建配方(4-5 阶段 autotools bootstrap)

上游 = **GCC 16.1.0 + mingw-w64-crt(MSVCRT)+ mingw-w64-headers + winpthreads**。选 GCC 16 与全线
gcc@16.1.0 floor 对齐(GCC 15 有模块模板实例化丢失,见 remediation A2)。阶段:

```
1. binutils  --target=x86_64-w64-mingw32                        → 交叉 as/ld/ar
2. mingw-w64-headers  --host=x86_64-w64-mingw32  install         → Windows API 头
3. gcc  all-gcc install-gcc  (core，尚无 libc)                   → 交叉 C/C++ 前端
4. mingw-w64-crt (--with-default-msvcrt=msvcrt) + winpthreads   → MSVCRT CRT + 线程
5. gcc  make / make install  → 建 libstdc++（连带 import std 源 bits/std.cc）
```

关键旋钮:`--with-default-msvcrt=msvcrt`(本期 MSVCRT)、`--enable-threads=posix`(或 `mcf`)。crosstool-ng
的 `scripts/build/libc/mingw-w64.sh` 可自动化;本仓沿用 aarch64-musl 的手写脚本先落地、后收编亦可。产物
重命名为 xlings-res 约定 `mingw-cross-gcc-<ver>-linux-x86_64.tar.*`。

### 4.3 发布门禁 — `import std` 是唯一未证链路(make-or-break,先做原型)

调研的核心风险:libstdc++ 自 GCC 15 起 target-independent 地装 `bits/std.cc`,mingw 交叉**预期**能用,
但**无人实证** `import std` 走 `x86_64-w64-mingw32` target。**打包前必须先跑通这一条端到端**,再谈发布:

```sh
# 原型 gate（在候选 tarball 上验证，通不过就不发布）
x86_64-w64-mingw32-g++ -std=c++23 -fmodules \
  -c <prefix>/x86_64-w64-mingw32/include/c++/16/bits/std.cc \
  -o std.o                                   # ① std 模块 BMI 可出？
# ② trivial `import std;` 程序 → -static 链接 → 出 app.exe
# ③ wine ./app.exe 打印预期 → 运行期 import std 真通
```

**发布门断言**(打包脚本硬检查,不是假设):
- tarball 内 `x86_64-w64-mingw32/include/c++/16/bits/std.cc` **存在且非空**(Ubuntu/Homebrew 有过空 modules 文件的坑);
- `libstdc++.modules.json` 路径可解析;
- `libstdc++exp.a` 在(`<print>`/`<stacktrace>` 的 `-lstdc++exp` 依赖它);
- 前端旁有 `x86_64-w64-mingw32-{ar,ld,as}`(自包含)。

任一不满足 → 不发布,回构建配方。**这一段是全方案的真正风险所在**——A/C 都是机械工作,B 的 import std
可行性是决定「做不做得成」的那颗石头。

### 4.4 xlings-res 双端镜像 + xim-pkgindex(与 llvm/musl-gcc 同)

工具链制品**不随 mcpp release 走**,是独立 xlings-res 制品仓 + 索引条目,版本跟随上游 GCC/mingw(同 llvm 模式,
见 [[release-publish-pipeline]] 第 4b 段解耦):

1. **建仓/传资产**:`gh release create <ver> --repo xlings-res/mingw-cross-gcc <tarball>` +
   `gtc release create/upload xlings-res/mingw-cross-gcc --tag <ver>`(GitCode 同名仓)。
2. **双端逐资产 GET 核验**:`curl -sL -o /dev/null -w '%{http_code} %{size_download}'` 要 `200`+真实字节数,
   **两端都验、必须 GET**(HEAD 会骗人;见 [[release-publish-pipeline]] 的「列出但下载 404」「gtc 假错」「api. 主机 404」三坑)。
3. **索引条目**:xim-pkgindex 新增 `pkgs/m/mingw-cross-gcc.lua`——**linux 块**(host=linux),`["<ver>"] = "XLINGS_RES"`
   + `["latest"] = { ref = "<ver>" }` + 每平台 sha256,格式抄 mcpp.lua 的 checked XLINGS_RES 条目。走 PR +
   `gh pr merge --squash --admin`;合入后 `publish-artifact.yml` 自动重打包索引(内容哈希名)。
4. **端到端验证**:`xlings update && xlings install mingw-cross-gcc@<ver>`(或 `mcpp toolchain install mingw-cross`)→
   前端可 `-dumpmachine`。

## 5. Part C — Wine 真实验证(host 跑不了 PE)

### 5.1 e2e 脚本

新增 `102_mingw_cross_wine.sh`(编号接现有 101;`# requires: mingw-cross wine` 于第 2 行,run_all.sh 约定):

```
install mingw-cross → mcpp new → build --target x86_64-w64-mingw32
  → 断言产出 target/x86_64-w64-mingw32/**/app.exe
  → objdump/x86_64-w64-mingw32-objdump -p app.exe：导入表无 libstdc++-6.dll / libgcc_s / libwinpthread
    （证 -static 生效；静态是 Wine 干净跑的前提，免拷 DLL 进 WINEPREFIX）
  → WINEPREFIX=<隔离目录> wine ./app.exe：断言 stdout
覆盖：hello → 多模块 → import std（运行期）→ -lstdc++exp（<print>/<stacktrace> 运行期）→ 静态库 → 增量(dyndep)
```

### 5.2 run_all.sh 能力探测

`run_all.sh` 的 `# requires:` 机制(:114-121)需加两个 cap 探针:
- `mingw-cross` — xim 是否装了 `x86_64-w64-mingw32-g++`(不吃 PATH,须显式用装的那份);
- `wine` — `command -v wine`(64-bit-capable;现代 wine 统一,不再单独 `wine64`)。

### 5.3 CI

- **落 `cross-build-test.yml`**(已有交叉语义,musl aarch64 在此)或 `ci-linux-e2e.yml` 新步骤
  "Toolchain: MinGW cross → build → wine run"。**隔离 `WINEPREFIX`**(临时目录,用后即删),`-static` 免 DLL 部署。
- **runner 自带 mingw 干扰**:Ubuntu runner 的 apt `g++-mingw-w64`(GCC 13)在 PATH,测试必须显式用 xim 装的
  GCC-16 那份,不吃 PATH(教训同 run_all.sh:70 native mingw 注释)。
- 迭代走「临时分支只留相关 CI」省时模式(0.0.88 已验证)。

### 5.4 Wine 运行坑(调研)

- **必须静态链接**:动态 mingw exe 在 Wine 下缺 `libstdc++-6.dll`/`libgcc_s_seh-1.dll`/`libwinpthread-1.dll` 会挂,
  除非拷进 WINEPREFIX;`-static` 是干净路径(本期链接默认已 `-static`,见 flags.cppm:386-393)。
- **MSVCRT 选型正好利于 Wine**:`msvcrt.dll` 恒在 Wine 中,免 UCRT 的 `ucrtbase.dll` 依赖(§2.2 选型的附带收益)。

## 6. 开发规范流程(brainstorm → 交付)

沿用 mcpp 既定研发流程,本方案的特殊约束标 **★**:

1. **设计**:本文档(brainstorm 产出)→ 需要时 writing-plans 出实施计划。
2. **★ 原型先行**:**先跑 §4.3 的 import std gate**,再动 mcpp 侧代码。这是唯一未证链路,失败则整个方案回炉;
   不能等 A/C 写完才在打包期开盲盒。
3. **TDD + 零 diff 门**:Part A 对 native MinGW / 现有工具链**零行为变化**——验收沿用 0.0.89/0.0.90 的
   `build.ninja` 逐字节比对门(仅允许注释/变量重排白名单差异)。抽象错了立刻显形。
4. **模块命名**:新模块/函数按职责命名,禁口袋名(ops/manager/utils),见 [[naming-no-grab-bag-modules]]。
5. **两条独立发布轨**(见 §7):toolchain 制品轨 与 mcpp feature 轨,分开评审、分开发布。
6. **门最后删**:若 docs/03-toolchains.md 有「cross not supported」注记,**最后一个 commit** 撤它——门在,
   中间态可安全合入 main;门删,即宣布支持(同 msvc-mingw-design §5.6 模式)。

## 7. PR / 版本(两条独立轨)

### 7.1 轨一:toolchain 制品(`mingw-cross-gcc`)—— 不碰 mcpp 版本

独立 xlings-res 制品仓 + 索引条目,**版本跟随上游 GCC/mingw**,不 bump mcpp(同 llvm/musl-gcc)。流程 =
§4.4:建仓 → 双端镜像 → GET 核验 → xim-pkgindex PR(pkgs/m/mingw-cross-gcc.lua)→ `--squash --admin` →
publish-artifact.yml 自动发索引 → `xlings install` 验证。**先于**轨二可用(mcpp 侧代码要能解析到这个包)。

### 7.2 轨二:mcpp feature PR(Part A/C 代码)—— 带 mcpp 版本 bump

Part A(host 二维化)+ Part C(e2e/CI)是一个带版本号的 mcpp feature PR:

- **版本号两处同步(同一 commit)**:`mcpp.toml` `version` + `src/toolchain/fingerprint.cppm` `MCPP_VERSION`
  (不一致 release smoke 失败;见 skill `.agents/skills/mcpp-release`)。版本可放「最后一个 PR」随合入带上。
- **触发 release**:`gh workflow run release.yml --ref main`(或 push tag `v<ver>`)。
- **publish-ecosystem 自动**(0.0.88 起):tag 触发后自动镜像 mcpp 四平台产物到 xlings-res/mcpp 双端 +
  向 xim-pkgindex 开 bump PR。**剩余手动**:① 双端 GET 核验;② `gh pr merge <n> --repo openxlings/xim-pkgindex
  --squash --admin`;③ `xlings install mcpp@<ver>` 验证;④ bump 本仓 `.xlings.json` bootstrap pin 直推 main。

**合入顺序**:轨一(制品可下)→ 轨二(mcpp 认它)。轨二的 e2e `# requires: mingw-cross` 依赖轨一已发布。

## 8. xlings 打通 + 真实验证(端到端闭环清单)

一条命令都不能少的验收链(全绿才算「支持」):

```
① 原型 gate（§4.3）        import std 走 mingw 交叉 target 真通（先决,失败即止）
② xlings 装制品            xlings install mingw-cross-gcc@<ver> → -dumpmachine=x86_64-w64-mingw32
③ mcpp 解析                mcpp toolchain install mingw-cross → mcpp toolchain list（Linux）见之
④ 交叉构建                 mcpp new demo && mcpp build --target x86_64-w64-mingw32 → app.exe
⑤ PE 校验                  objdump -p app.exe 导入表无 libstdc++/libgcc/winpthread DLL（-static 生效）
⑥ Wine 运行               WINEPREFIX=<tmp> wine ./app.exe → stdout 断言（运行期 import std）
⑦ C++23 库特性            <print>/<stacktrace> 程序 → -lstdc++exp → wine 跑通
⑧ CI 常态化               cross-build-test.yml 每次跑 ②–⑦（隔离 WINEPREFIX,显式用 xim 的 GCC-16）
⑨ 生态闭环                xim-pkgindex 索引发布 + bootstrap 用户 `xlings install` 复现全链
```

**真实验证 ≠ 单测**:§4.3 明确了 import std / libstdc++exp 的功能性「靠测不靠文档」——④–⑦ 的 Wine 运行断言
是本方案能否宣布「支持」的唯一凭据,不能以「构建成功 + PE 有效」代替运行期验证(那会重蹈 [[issue195-hermetic-link-model]]
的「宿主污染假绿」)。

## 9. 非目标(本期明确不做)

- **UCRT 交叉**:登记为 triple 可选 follow(`x86_64-w64-mingw32ucrt`,Rust `gnullvm` 式独立目标;届时
  abi.cppm 细化 ucrt,即 msvc-mingw-design §4.2 原计划)。
- **i686 / arm64 mingw 交叉**:仅 x86_64;其余登记后续(机制同,triple 换)。
- **llvm-mingw(Clang/libc++)**:mstorsjo/llvm-mingw 是 Linux-host 现成 tarball,但 Clang+libc++(另一条
  `import std` 路,非 `bits/std.cc`)——仅当 GCC mingw std 模块被证坏时的 fallback,不是本期后端。
- **native winlibs MinGW(Part B)不动**:它是 (Windows,Windows,UCRT),本方案是 (Linux,Windows,MSVCRT),
  两条独立 target,互不干扰。
- **`mcpp pack` 的 PE 支持**:pack/pipeline.cppm 是 ELF/patchelf 专属,独立议题。
- **MSYS2/Cygwin 环境适配**;**Wine 之外的运行方式**(真机 Windows CI 是 native MinGW 的事)。

## 10. 排期与依赖

```
★ 原型 gate（§4.3,import std 走 mingw 交叉）—— 先决,通不过则全案回炉
│
├─→ 轨一 B:mingw-cross-gcc 制品（canadian-cross 构建 + xlings-res 双端 + 索引 PR）
│      └ 独立版本,先于轨二可用
│
└─→ 轨二 A+C:mcpp feature PR
       ├ A(host 二维化,对 native 零 diff:A1-A7)
       └ C(Wine e2e + CI + 删门)
       └ 硬依赖轨一制品已发布(e2e `# requires: mingw-cross`)
```

每轮沿用 0.0.88 流程:临时分支精简 CI 迭代 → 带版本号真 PR(轨二)→ 全 CI → 合入 → release →
生态闭环(publish-ecosystem 自动镜像 mcpp + 索引 PR;制品仓是额外手动 gh+gtc 双端 + GET 核验)。

## 11. 实测验证结果(2026-07-15,全链闭环)

本方案不止纸面——已从源码建出 GCC-16.1.0 mingw-w64 MSVCRT 交叉链、发布到生态、端到端跑通。

### 11.1 制品与发布(轨一,已执行)

- **构建**:from-source canadian-cross(binutils 2.44 + GCC 16.1.0 + mingw-w64 v12.0.0 CRT,
  `--with-default-msvcrt=msvcrt`)。三坑已解:① PATH `$PREFIX/bin:/usr/bin:…`(target 工具优先、
  裸 gcc 是真 host);② 勿全局 export CC(`--host` 的 CRT/libstdc++ 须用 target gcc,否则
  `_mingw_mac.h no`);③ `MAKEINFO=true`(texinfo 缺时 gccint.info 段错)。
- **瘦身**:597MB→**104MB**(cc1plus/cc1/lto1/lto-dump 各 ~400MB 带调试符 → `strip --strip-unneeded`;
  删 share/{info,man,doc}、删 lto-dump)。strip 后 import std + wine 仍通。
- **发布**:`xlings-res/mingw-cross-gcc` GitHub + GitCode 双端(gh 需先 push README 建默认分支;
  gtc 需 `repo push` 建 main;gtc 是 python 脚本、须 `/usr/bin/python3` 绕开坏 shim);两端 ranged
  GET 206 核验;`xim-pkgindex` PR #389 合入 → 索引重发 `xim-index-a220160`。

### 11.2 make-or-break gate 通过(§4.3)

`x86_64-w64-mingw32-g++ -std=c++23 -fmodules bits/std.cc` 出 std.gcm → `import std;` 程序
`-static -lstdc++exp` 链成 PE32+(仅 KERNEL32+msvcrt 导入)→ **wine 跑出** `std::println/vector/
ranges::sort/print`。`bits/std.cc` 113KB、`libstdc++exp.a` 就位。

### 11.3 全链闭环(经真发布的生态)

`mcpp toolchain install mingw-cross 16.1.0`(从 gitcode xlings-res 下 104MB、校验 sha256、装好)
→ `mcpp build --target x86_64-w64-mingw32`(mcpp 自身管线:xim 解析工具链→std BMI staging→
多模块→PE 链)→ `crosswin.exe`(PE32+,仅 KERNEL32+msvcrt)→ wine 打印 import std + 多模块输出。
e2e `102_mingw_cross_wine.sh` 过、单测 33/33。

### 11.4 真实验证抓到的 4 个 host≠target bug(纸面/单测抓不到,已全修)

「真实验证 ≠ 单测」(§8)的活教材——这 4 个只有真跑 mcpp 全链才暴露:

| # | 位置 | 症状 | 修 |
|---|---|---|---|
| 1 | gcc.cppm `find_std_module_source` | 交叉的 `bits/std.cc` 在 `<prefix>/<triple>/include/c++/<ver>/`,只找 host `<prefix>/include/c++/`→"provides no std module source" | 按前端名剥 `-g++` 得 triple 补查 target 子目录 |
| 2 | gcc.cppm `std_module_build_command` | `!is_musl` 才跳外部 -B → 交叉误接 Linux binutils,Linux `as` 编 PE/SEH asm(`.def`/`.seh_proc`)报错 | `!is_musl && !is_mingw`(自包含) |
| 3 | flags.cppm 主链 -B | 同因 | 加 `!isMingwTc` |
| 4 | flags.cppm PE link 分支 | 整段(`-static`/`-lstdc++exp`)在 `if constexpr(is_windows)` **host** 门内→Linux 交叉永不进→`std::print` 的 `__open_terminal` 未定义 | 提为运行期 `if(isMingwTc)` **按 target** 求值、host 无关(early return);native Windows mingw 亦走此支(统一) |

Bug 4 正是本文档反复强调的 host≠target 违例——PE 产物形态判断必须按 target 而非 host constexpr。
Part A 表(§3)相应扩展:除 registry/lifecycle/prepare,还须改 gcc.cppm 探测/预编译 + flags.cppm
link 分支(host≠target 化)。
