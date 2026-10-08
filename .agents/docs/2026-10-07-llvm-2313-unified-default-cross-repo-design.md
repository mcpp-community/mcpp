---
subject: toolchain
status: active
---

# LLVM 23.1.3 全平台统一默认:跨仓库联动方案(mcpp × xim-pkgindex)

本记录设计一次跨两个仓库的联动:mcpp 的默认工具线(LLVM 线)整体从
`llvm@20.1.7` / `llvm@22.1.8` 移动到 `llvm@23.1.3`,macOS arm64 与
Windows(MSVC 可用)两个宿主的**宿主默认工具链**提到 `llvm@23.1.3`;
Linux 宿主默认保留 `gcc@16.1.0`(评审裁决,见 D1)。同时这一次移动解决
mcpp#669(macOS 27 SDK 的 `arm64e.x1` 使 `ld64.lld` 22.1.8 无法链接)。

读者:准备执行这次移动的两个人(仓库各一),以及以后问「为什么默认是
llvm 23.1.3、为什么这么移动」的人。SPEC-009(`docs/specs/toolchain-maintenance.md`)
给出的是**规则**;本记录给出的是**把规则落到这两个仓库的具体步骤、
决策点与风险**。步骤编号与 SPEC-009 §10 的十步一一对应。

证据标注:标「已核实」的条目给出可复查的出处(issue 正文、CI 日志、
GitHub PR/Release 页);标「未验证」的条目必须在对应的门或验收步骤里
实测后才可当作事实。

---

## 1. 为什么现在移动

三条独立的理由,汇在同一次移动上:

1. **macOS 27 的阻塞(mcpp#669,已核实)。** macOS 27 SDK 的 `.tbd` 文件
   列出 `arm64e.x1` 架构,LLVM 22.1.8 的 `ld64.lld` 无法解析
   (`could not load TAPI file ... malformed file`),两条 `xcode-27` CI 腿
   (ci-macos.yml、ci-macos-e2e.yml)按 §8.2 挂 `known_red: '#669'` 保持红色。
   上游修复 [llvm/llvm-project#222721] 于 2026-09-11 合入 main;
   backport [llvm/llvm-project#224185] 经 ABI 安全化(枚举值追加在尾部而非
   中间插入,nico 认可)后由 tru 于 **2026-09-29 手动合入 release/23.x,
   commit `ee66426`**(已核实:PR #224185 页面)。
2. **23.1.3 是第一个携带该修复的点发布(已核实)。** 23.1.0(2026-08-25)与
   23.1.1(2026-09-08)早于修复合入;23.1.2 明确不含(rust-lang 侧的更新说明
   引用「23.1.2 lacks it」);23.1.3 的发布资产已在 llvm-project Releases 页
   出现(含 `LLVM-23.1.3-Linux-ARM64.tar.xz`)。选 23.1.3 而非 23.1.1/23.1.2
   不是偏好,是修复落点决定的。
3. **SPEC-009 §4.1 的方向。** 同一宿主上同一族应当解析到同一发布;当前
   LLVM 族在 macOS 宿主默认 20.1.7、Windows(MSVC)宿主默认 20.1.7、
   17 个目标行钉 22.1.8,三处不一致且都没有记录理由。统一到 23.1.3 一次
   消除全部三处偏离。

评审裁决(2026-10-07):Linux 宿主默认**保留** `gcc@16.1.0`,不做家族切换;
macOS 与 Windows 的宿主默认提到 `llvm@23.1.3`。理由与记录见 §4 的 D1。

---

## 2. 两仓库的现状(2026-10-07,main)

### 2.1 mcpp 侧的版本钉(全部要动的位置)

引擎内的两张表(`modules/toolchain-model/src/triple.cppm`;线表 TS-3 尚未
落地,§10.6 的「移动线表」在今天的结构里等于同时改这两张表):

| 位置 | 当前值 | 移动后 |
|---|---|---|
| `pins::kFirstRunMac`(:1067) | `llvm@20.1.7` | `llvm@23.1.3` |
| `pins::kFirstRunWinMsvc`(:1071) | `llvm@20.1.7` | `llvm@23.1.3` |
| `pins::kFirstRunWinGnu`(:1076) | `gcc@16.1.0` | **不动**(须与 x86_64-windows-gnu 行相等,test_windows_defaults.cpp 强制) |
| `pins::kFirstRunLinuxX86_64`(:1078) | `gcc@16.1.0` | `llvm@23.1.3`(D1) |
| `pins::kFirstRunLinuxOther`(:1079) | `gcc@15.1.0-musl` | `llvm@23.1.3` 或保留(D2) |
| `pins::kSuggest*`(:1081-1083) | `llvm 20.1.7` 等 | 同步为 `llvm 23.1.3` |
| `kKnownTargets` 17 行 llvm 行 | `llvm@22.1.8` | `llvm@23.1.3` |

17 个 llvm 行(`x86_64-windows-musl`:535;bare:riscv64/32-none-elf :549-550、
aarch64/x86_64-none-elf :572/:593、thumbv6m-8m 系 :630-636、armv7a 系 :653-654;
ios:aarch64-ios :788、aarch64/x86_64-ios-sim :819-820;verified 9 行、preview 8 行)。

gcc 系目标行**不动**(D3):`x86_64-linux-musl`(:472)、
`aarch64-linux-musl`(:473)、`x86_64-windows-gnu`(:474)均为 `gcc@16.1.0`;
它们是 C 库绑定的交叉行,不是宿主默认。release.yml 的全静态发布路径走
`--target x86_64-linux-musl`,依赖这两行保持 gcc。

引擎外的读者与被检查副本(单 PR 同步,§10.6;数字来自逐文件盘点):

| 类 | 位置与规模 |
|---|---|
| 自举清单 | `mcpp.toml:49-52`:`default = "gcc@16.1.0"` / `macos = "llvm@22.1.8"` / `windows = "llvm@20.1.7"` → 统一为 `llvm@23.1.3`(D1 成立时 default 也改;否则 macos/windows 改、default 保留为已记录偏离) |
| 工作流 | 9 文件 21 处 `llvm@`,另有裸拼写 `install llvm 22.1.8/20.1.7`(ci-linux.yml:143、ci-fresh-install.yml:229、ci-linux-e2e.yml:70,176、openkal-cross.yml:212,505)、路径引用 `xim-x-llvm/22.1.8`(openkal-cross.yml:229,316)、`llvm-tools@22.1.8`(ci-linux.yml:154)、prewarm 列表(ci.yml:155) |
| action | `.github/actions/setup-macos-llvm/action.yml:75`(`xlings install llvm -y \|\| xlings install llvm@20.1.7 -y`) |
| CI 工具 | 7 文件 13 处:check_function_sizes.sh(5)、check_unicode_paths.sh:65,100(fixture 清单)、build_examples.sh:33、check_matrix_reasons.sh:11、check_version_pins.sh:49、check_workflow_assertions.py:9(注释) |
| e2e | 38 文件 77 处字面量;helper `tests/e2e/_llvm_env.sh` 已有 `MCPP_E2E_LLVM_VERSION`(缺省取最新已装版本)机制,见 D5 |
| 矩阵 | `tests/matrix/expected.tsv`:109 个数据行写死 `llvm@22.1.8`(比较用 mode/host/target/compiler/status/reason 六列) |
| 文档 | 17 文件 102 处(llvm@20.1.7 18 处、llvm@22.1.8 84 处);默认值钉点:docs/01-getting-started.md:46-47(及 zh :44-45)、docs/20-toolchains.md:29-30(及 zh :30-31)、docs/21-the-target-triple 22 处;README 平台表 |
| 文档检查 | `.github/tools/check_default_toolchain_docs.py:42-73` 的期望短语表按 `pins::host_default_toolchain` 生成,移动后随引擎自动要求新短语——期望表本身无需手改,但四份文档必须同 PR 改(C2 在每个宿主 CI 行上强制) |
| 示例 | 6 文件 16 处,全部 `llvm@22.1.8` |

### 2.2 xim-pkgindex 侧的现状

`pkgs/l/llvm.lua`(`xpm` 三平台,`latest` 三平台均为 `{ ref = "22.1.8" }`):

| 平台 | 版本 | 资产来源 | 备注 |
|---|---|---|---|
| linux | 20.1.7 / 22.1.8 | `"XLINGS_RES"` 哨兵 | deps:`xim:glibc@>=2.39`、`xim:linux-headers@5.11.1`、`xim:zlib@1.3.1`、`xim:libxml2@2.13.5`、`xim:gcc-runtime@15.1.0`(clang-22 动态链 libstdc++.so.6;clang-20 静态,dep 是否仍需要须对 clang-23 实测 ldd);载荷新目录名 `llvm-<ver>-linux-x86_64` |
| macosx | 20.1.7 / 22.1.8 | 显式 GLOBAL/CN URL,**sha256 = nil** | slim 自包含子包,由 `build-llvm-subpkg.sh --pkg llvm` 从上游全量 carve;cfg 走 `xcrun --show-sdk-path`(#858)与 `-fuse-ld=lld`(不用 Apple ld) |
| windows | 20.1.7 / 22.1.8 | `"XLINGS_RES"` 哨兵 | core-only(无 libc++,MSVC ABI 用 MSVC STL);资产 `llvm-<ver>-windows-x86_64` |

配套:`pkgs/l/llvm-tools.lua`(linux/macosx-arm64/windows 三平台,latest 22.1.8);
`pkgs/l/llvm-dev.lua`(**latest = 20.1.7.1**,源码构建 X86;AMDGPU;SPIRV,
因 GCC 16.1 对 `AMDGPUAsmParser.cpp` 的 ICE 而用 GCC 15.1.0 构建)。

发布机制(已核实,来自 `.agents/skills/llvm-subpackaging/SKILL.md` 与
`references/publish-resources.md`):

- 上游没有分包;xlings-res 的 `llvm` 与 `llvm-tools` 都是从上游全量包
  carve 的,工具是 `.agents/tools/build-llvm-subpkg.sh`(manifest 驱动,
  macOS 自包含校验内嵌 Mach-O `LC_LOAD_DYLIB` 读取器;不 strip)。
- 双镜像:GLOBAL `github.com/xlings-res/llvm`、CN `gitcode.com/xlings-res/llvm`,
  tag = 版本号;GLOBAL 可 `gh release upload --clobber`,**GitCode 资产不可
  替换不可删除**(SPEC-006 §4.6 的根),新资产名直接 `gtc release upload`。
- 资产命名 `<pkg>-<ver>-<platform>-<arch>.<ext>`;格式 mac=`tar.xz`、
  win=`tar.xz`+`zip`、linux=`tar.gz`+`tar.xz`。
- 验收:`.agents/tools/verify-toolchain.sh`(INTERP 无关的解包-编译-运行,
  exit code 0/1/2/3 契约见 `.agents/tools/README.md`);索引 CI 有
  ci-xpkg-test.yml、toolchain-consumer-smoke.yml、consumer-through-index-override.yml。
- llvm 不参与 `url_template` 自动更新(version-check.py 的 opt-in 契约),
  版本条目由手工维护;`check-revision.lua` 比对 revision。
- 已知缺口:macosx 条目 `sha256 = nil`(xim-pkgindex#27「自动填写资源哈希值」
  仍 open)。SPEC-006 §4.6 要求版本内容由 sha256 固定——本批新条目必须带
  sha256,不顺手回填旧条目(旧条目回填属 §5.3 修订,另行处理)。

---

## 3. 上游事实清单(执行前逐条复核)

| # | 事实 | 证据 | 状态 |
|---|---|---|---|
| U1 | arm64e.x1 修复在 release/23.x,commit `ee66426`,2026-09-29 合入 | llvm-project PR #224185 | 已核实 |
| U2 | 23.1.3 是第一个携带 U1 的点发布;23.1.2 不含 | rust-lang 更新说明引用;PR 页 | 已核实 |
| U3 | 23.1.3 全量发布资产存在(Linux ARM64 已见;mac/win 资产名以 Releases 页为准) | llvm-project Releases | 部分核实——执行 Phase 0 时逐平台确认下载链接与 sha256 |
| U4 | clang 23.1.0 的 MSVC STL `std` 模块 `align_val_t` 歧义缺陷在 23.1.1 修复 | mcpp#640,llvm-project#218152 | 已核实;23.1.3 ⊇ 23.1.1,Phase 2 在 Windows 行实测确认 |
| U5 | 22.x 的 `release/22.x` 分支已关闭,不会再有携带修复的 22.x 点发布 | mcpp#669 正文 | 已核实 |
| U6 | LLVM 22→23 是主版本跳变,包 ABI 标签随之变化 | `clang22-libcxx23` → `clang23-libcxx23`(src/pack/abi_tag.cppm 按主版本生成) | 已核实(机制);带旧标签的预制产物被 prebuilt.cppm 拒绝,属预期行为 |
| U7 | GCC 16.1.0 构建 LLVM 23 源码是否仍在 AMDGPUAsmParser 上 ICE | — | 未验证;只影响 llvm-dev(D4),不影响默认线 |

---

## 4. 决策点(review 时请逐条表态)

### D1 Linux 宿主默认:gcc → llvm 家族切换 —— 裁决:保留 gcc

评审裁决(2026-10-07):**Linux 宿主默认保留 `gcc@16.1.0`**,不做家族切换。
记录如下,供线表注释与文档改写使用:

- 理由(SPEC-009 §4.1 要求的「为什么这一行与族的移动不同步」):Linux 的
  宿主默认族是 gcc,面向原生 glibc ABI,系统库(X11、OpenGL)直接可用;
  LLVM 线的本次移动只覆盖 macOS 与 Windows 宿主默认和 17 个 llvm 目标行。
  这是平台设计决定,不是落后于族的偏离,因此没有退出条件。
- 附带收益:mcpp#666(clang 构建的 mcpp SIGSEGV)不再处于自举路径上,
  G2 的 Linux 腿继续以 gcc 构建,Linux 无新风险。
- `kFirstRunLinuxOther = gcc@15.1.0-musl` 同样保留。它的偏离理由本批补记
  (非 x86_64 Linux 宿主没有受管 glibc gcc 载荷,全静态 musl 是唯一自包含
  选择),消除 SPEC-009 §4.1「没有记录理由」的既有缺口之一。

### D2 Linux 非 x86_64 宿主 —— 由 D1 裁决消解

Linux 全部宿主默认保留 gcc,本批**不新增** linux-arm64 llvm 载荷
(llvm.lua 维持 linux-x86_64 资产;上游 `LLVM-23.1.3-Linux-ARM64.tar.xz`
存在但本批不用)。llvm 目标行(bare-metal、ios 等)在 aarch64 Linux 宿主
上的可用性与今天相同,无回归。

### D3 gcc 系目标行不动

`x86_64-linux-musl`、`aarch64-linux-musl`、`x86_64-windows-gnu` 三行保持
`gcc@16.1.0`;Windows 无 MSVC 的回退默认(`kFirstRunWinGnu`)随之不动,
`test_windows_defaults.cpp` 的一致性断言继续成立。统一仅指 LLVM 线与
宿主默认;静态 musl 发布路径(release.yml)不受影响。

### D4 llvm-dev 与 llvm-tools 是否同批 —— 裁决:llvm-tools 同批,llvm-dev 以后再做

- `llvm-tools@23.1.3`:同批加行(carve 机械,三平台),latest 随 §10.7 移。
- `llvm-dev@23.1.3`:**本批不做**(评审裁决 2026-10-07),在索引中标记为
  后续工作。依据:22.1.8 批次从未产出 llvm-dev(latest 至今 20.1.7.1);
  它是 `status = "dev"` 的 mesa 构建期输入包,与默认工具链移动无依赖,
  资产保留即可用;且 SPIRV-LLVM-Translator 尚未发布 v23.1.3 tag(最新
  v23.1.2),现在构建还要先裁决翻译器的版本配对。跟进事项记为 follow-up:
  按 `.agents/tools/graphics/build-llvm-dev.sh` 配方构建(需 subos
  `gfxbuild` + gcc 15.1.0,磁盘 ≥ 25GB)。llvm-dev 的 latest 留在
  20.1.7.1,不倒退。

### D5 e2e 字面量的处理

77 处 e2e 字面量,两条路:

- 最小改动:全部字面量 22.1.8→23.1.3、20.1.7→23.1.3。
- 借机迁移:`_llvm_env.sh` 的 `MCPP_E2E_LLVM_VERSION` 机制(缺省取最新
  已装版本)是 C3 的方向——fixture 的 `[toolchain]` 行改为跟随已装版本。

推荐:本批对**fixture 里的默认值形态**(`[toolchain] macos/windows = ...`)
迁移到 helper 或删除(它们本想表达「用 llvm 行」,字面量反而让每次移动
都要动 38 个文件);对**明确测试 Supported 版本行为**的少数用例保留
22.1.8 字面量并加注释「Supported 线,故意不随默认移动」。这把下一次移动
的读者面缩到接近零,是 C3 的第一笔本金。

### D6 macosx 新条目的 sha256

23.1.3 的 macosx 条目**必须**带 sha256(与 GLOBAL/CN/carve 产物三方一致,
SKILL 第 4 节的既有要求);不回填旧条目。

### D7 已记录默认值的使用者通知(SPEC-009 §11.2 未实现)

移动后,已由旧 mcpp 首次运行写入 `[toolchain] default = llvm@20.1.7` 的
机器**不会**自动迁移(§11.1:记录不移动;§11.2 的通知机制不存在)。本批
不实现 §11.2(范围控制),在 mcpp 发布说明中写明:
`mcpp toolchain default llvm@23` 一条命令移动,`mcpp toolchain default --keep`
保留(后者尚不存在,发布说明只写前者)。§11.2 与 `--keep` 留给线表落地批次。

### D8 移动前是否先落地 TS-3 单线表

TS-3(一张线表 + C1-C4 检查)未实现;§10 前言明说没有线表时 10.6 要同时改
两张表及其全部副本。先落地线表再移动,可以把读者面一次性收缩,但把一次
大改动变成两次。推荐:**先移动、后线表**——移动后全部 llvm 值相同,
线表抽取的 diff 更接近纯重构;本 PR 顺带做 D5 的 e2e 迁移,已是 C3 的
最大头。

---

## 5. 执行计划(与 SPEC-009 §10 的映射)

### Phase 0 — 上游与载荷(SPEC-009 §10.1、§10.2)

xlings-res / xim-pkgindex 侧,一个工作分支:

1. 从 llvm-project Releases 下载 23.1.3 三平台全量包(Linux-X64、
   macOS-ARM64、Windows x86_64-msvc),记录每个的 sha256
   (下载可走代理加速;大文件在 CN 网络外取)。
2. `build-llvm-subpkg.sh --pkg llvm` 逐平台 carve:
   `macosx-arm64`(tar.xz)、`windows-x86_64`(tar.xz+zip)、
   `linux-x86_64`(tar.gz+tar.xz)。
   自包含校验(macos LC_LOAD_DYLIB;linux ldd 无 libLLVM.so;win 无
   LLVM-C.dll import)必须 0 失败;不 strip。
3. `build-llvm-subpkg.sh --pkg tools` 三平台 carve `llvm-tools`。
4. Linux 载荷跑 `.agents/tools/verify-toolchain.sh`(exit 0);
   实测 clang-23 的 `ldd` 是否仍需 libstdc++.so.6,决定 `gcc-runtime`
   dep 去留(llvm.lua 的 dep 注释是按 clang-22 写的,不要沿用结论)。
5. §10.3 / §9.1 双镜像:GLOBAL `gh release upload 23.1.3 ...`、
   CN `gtc release upload`(GitCode 不可覆盖,资产名一次写对:
   `llvm-23.1.3-macosx-arm64.tar.xz` 等);发布后从 GLOBAL、CN、carve
   产物三方 sha256 比对,并对两镜像各做一次 GET(状态码 200 + 字节数 +
   sha256)记录到 PR 描述。tag `23.1.3` 先建,注意既有教训:GLOBAL 的
   tag 归档名须带正确的归档扩展名 basename。

### Phase 1 — 索引加行,latest 不动(SPEC-009 §10.4)

xim-pkgindex 一个 PR:

- `pkgs/l/llvm.lua`:三平台各加 `"23.1.3"` 条目(macosx 显式 URL+sha256,
  D6;linux/win 资产就绪后 `"XLINGS_RES"`),`latest` 保持 `{ ref = "22.1.8" }`
  (§10.4:latest 不变);linux deps 按 Phase 0 第 4 步的实测修订。
- `pkgs/l/llvm-tools.lua`:三平台各加 23.1.3(latest 不动)。
- CI 绿:ci-xpkg-test、toolchain-consumer-smoke(显式 `llvm@23.1.3`
  安装走一遍三平台)、check-revision。
- PR 描述附 Phase 0 的镜像验证记录。

### Phase 2 — 门(SPEC-009 §10.5,G1-G7)

门工作流(C7)不存在,本批以一个 mcpp 仓库的**比较分支 + 专用工作流**
执行门,结果贴进线表 PR 描述:

- 输入:同一 mcpp 提交;每宿主同一 runner 镜像、同一 job 内安装
  D = `llvm@22.1.8`(mac/win 为 20.1.7,即各宿主现 Default)与
  R = `llvm@23.1.3`(索引行已在,显式版本安装,latest 未动即可装)。
- 宿主腿:linux-x86_64、macos-15、**macos-xcode-27**(R 腿预期由红转绿,
  这是 #669 的验收)、windows-msvc。
- G1:e2e 套件以 D 与 R 各跑一遍;R 不新增失败、不新增跳过。
- G2:以 R 构建 mcpp 自身并跑套件——本批的 G2 腿是 **macOS 与 Windows**
  (自举清单 `mcpp.toml` 的 `macos`/`windows` 移到 23.1.3);Linux 的
  自举继续走 gcc@16.1.0,不在 R/D 比较之内(D1 裁决)。
- G3:验收程序(`<memory>`/`<mutex>`/`<thread>`、`import std`、
  `import std.compat`)在 R 上构建运行(e2e 886/888 覆盖的行)。
- G4:§7 复现集在 R 上重跑,已知条目逐一记录结果:
  #256(clang 20/22 BMI 毒化)、#666、macOS 27 SDK 的 INFINITY/NAN
  (hostflags.cppm 的绕行在 23.1.3 + 27 SDK 上是否仍需要)、
  clang 22 的两阶段精简接口绕行(ninja_backend.cppm;**即使 23 已修复,
  绕行也不拆**——§7.3 要求所有 Supported 发布越过修复后才移除,22.1.8
  移动后成为 Supported,仍被覆盖)。
- G5:模块图(每单元提供/需要的模块)在 R 与 D 下逐单元一致。
- G6:mcpp 与 bench/ 冷暖构建时长、BMI 体积,R ≤ D 的 110%(三次中位数)。
- G7:Windows 腿 e2e 881(导出发现读 R 的文本 IR)以 R 通过。

未过的门:发布留在 Available,记录原因;macOS 或 Windows 的 G2 不过时,
对应宿主的默认移动单独回退,不牵连另一宿主。

### Phase 3 — mcpp 单 PR 移动(SPEC-009 §10.6)

一个 PR 同时改(缺一即违反 §3.3 的「既不读也不查的字面量是缺陷」):

1. `triple.cppm`:`kFirstRunMac`、`kFirstRunWinMsvc`、`kSuggestLlvm` 与
   17 个 llvm 行 → 23.1.3;`kFirstRunLinuxX86_64`、`kFirstRunWinGnu` 不动,
   并按 D1 为 `kFirstRunLinuxX86_64`(平台设计:面向 glibc ABI 的 gcc)
   与 `kFirstRunLinuxOther`(无受管 glibc gcc 载荷,全静态 musl)补记
   §4.1 要求的理由注释,消除「落后未记录理由」的既有缺口。
2. `mcpp.toml`(D1)。
3. 文档:docs/01、docs/20、docs/21、docs/zh/ 三对、README 平台表、
   其余 §2.1 表列出的 17 文件;docs/20 的 Linux 理由句按 D1 实测改写。
4. `.github/tools/check_default_toolchain_docs.py` 期望短语若为硬编码
   表则同步(设计上它从引擎推导,确认后可能零改动——C2 的四条陈述由
   CI 在每个宿主行上强制)。
5. 工作流 9 文件、action、CI 工具 7 文件;**保留且注释标注**一条
   22.1.8 腿作为 Supported 线的冒烟(TS-2 的 Supported 义务)。
6. e2e 按 D5;`expected.tsv` 109 个 llvm 行 → 23.1.3;examples 6 文件。
7. xcode-27 两腿:R 绿后**移除 `known_red: '#669'`**——§8.2 要求 issue
   关闭时腿离开已知红列表,顺序是:腿先绿、known_red 移除、然后关闭
   issue(#669 的关闭条件两条:Xlings 发布携带 backport 的 macOS arm64
   LLVM + 两腿绿,此时均已成立)。

### Phase 4 — mcpp 发版 → latest → 层级平移(SPEC-009 §10.7、§10.8、§10.9)

1. 按 mcpp-release 技能发版(版本号走当时的主版本序列;ABI 标签与缓存
   键已随版本进入指纹,§6.3 无需换纪元)。
2. 发版后 xim-pkgindex 一个 bump PR:`llvm.lua` / `llvm-tools.lua`
   三平台 `latest = { ref = "23.1.3" }`(参照既有 `bump(mcpp): track ...`
   的 PR 形状);22.1.8 条目**保留**——它成为 Supported(§10.8),
   Available 的旧版本不动。
3. 关闭 mcpp#669(若 Phase 3 未关)。
4. SPEC-009 的实现状态由后续规范版本修订(§4.1 的三处偏离消除、
   §6.2 的 macOS 第三项验收成立、§12 的 macos/windows 偏离消除);
   本记录不改规范。

撤销(§10.9):revert Phase 3 的 mcpp PR 即回滚;载荷与索引行保留,
latest 不受影响。若 revert 发生在 latest 移动之后,再一个 bump PR 把
latest 指回 22.1.8。

---

## 6. 风险登记

| 风险 | 影响 | 缓解 |
|---|---|---|
| #256(BMI 毒化)在 23.1.3 仍在 | e2e 某些模块图形态失败;绕行继续存在 | G4 重跑记录;绕行按 §7.3 保留(22.1.8 仍是 Supported) |
| LLVM 22→23 的其他行为变化(诊断文案、文本 IR 形状、模块 BMI 布局) | G1/G5/G7 出红 | 门逐项暴露;G7 专门覆盖文本 IR 的非稳定性 |
| GitCode 资产不可替换 | 资产名/内容写错后无法原地修复 | 上传前三方 sha256;命名按 SKILL 模板;错了换 `-r1` 修订名(§5.3) |
| linux llvm 23.1.3 仍动态依赖 libstdc++ 而 dep 没配 | 干净机器上 clang 无法启动(bare-metal 等目标行的 Linux 宿主装机) | Phase 0 第 4 步实测 ldd;dep 缺失时安装即失败(llvm.lua 的 cfg 拒绝宿主回退) |
| `install llvm`(裸拼写)在 latest 移动后取到 23.1.3 | 移动前若有工作流依赖 latest=22.1.8 的隐式行为 | 顺序保证:latest 在 mcpp 发版后才动(§10.7);Phase 3 起所有工作流显式版本 |
| macosx 条目 sha256 缺失的既有先例被沿用 | §4.6/§9.1 的保证落空 | D6:新条目必须带;评审点 |

---

## 7. 验收标准(全部满足才算本次联动完成)

1. 两镜像上 `llvm@23.1.3`(以及 llvm-tools)GET 200、字节数与 sha256
   三方一致,记录在案。
2. `xlings install llvm@23.1.3` 在三平台成功;Linux 上
   verify-toolchain.sh exit 0;consumer smoke 绿。
3. mcpp 门 G1-G7 结果成文;G2 在 macOS 与 Windows 腿以 23.1.3 通过
   (Linux 自举继续走 gcc,不在比较之内)。
4. mcpp 线表 PR 合入后:各宿主 CI 绿,含 xcode-27 两腿;
   `mcpp self env --format json` 的 `defaultToolchain` 在 linux-x86_64、
   macos-arm64、windows(msvc 可用)上均为 `llvm@23.1.3`;
   check_default_toolchain_docs.py(C2)绿。
5. e2e 881(G7)在 Windows 以 23.1.3 绿。
6. #669 关闭;`known_red` 列表不再含它。
7. mcpp 发版;xim-pkgindex latest bump PR 合入;22.1.8 条目保留为
   Supported。

## 8. 实施状态(2026-10-07 追加)

已实施。xim-pkgindex 侧:PR #936(llvm/llvm-tools 23.1.3 三平台加行 + carve 配方修复)CI 全绿后合入;双镜像 GET + sha256 验证 10/10 与 carve 产物一致;`latest` 未动,待 mcpp 发版后移动(§10.7)。mcpp 侧:线表与全部读者移动(本 PR)。

实施中的实测发现,超出本记录 §2-§4 的盘点:上游 Linux 归档的 libc++ 系共享库不带 RUNPATH,而 DT_RUNPATH 不传递——`libc++.so.1` 的 `NEEDED libatomic.so.1` 只能靠宿主 loader 缓存或 xlings 装后改写解析;已发布的 22.1.8 资产在干净机器上跑 `verify-toolchain.sh` 的 import std 门即失败,23.1.3 资产在 carve 配方加入 `$ORIGIN` RUNPATH 后过门。llvm.lua 的 macosx 条目历史上 `sha256 = nil`,新条目按 D6 填实。llvm-dev 后置(D4 裁决)。known-red 腿共四处(ci-macos、ci-macos-e2e 与 ci-fresh-install 的 macos-fresh、macos-brew-fresh,后两处在初版盘点之外),全部随本次移动转为普通腿;`tests/scripts/test_check_workflow_assertions.py` 的已知红腿下限断言(≥4)相应改为 0。

## 9. 后续批次的实施(2026-10-08 追加)

**#669 关闭**:xlings#645 合入后,xcode-27 两腿重跑转绿(run 37622254100),按 issue 既述条件关闭。

**#782 修复(本 PR 第二段)**:openkal macos 腿的重跑暴露了守卫的真实形状——载荷已装(23.1.3 经 autoInstall 成功),但同样的拒绝仍出现,证明过度的不只 install 跳过,还有把「诊断已置」当「不可构建」的整条短路。修复:工具链解析照常安装(声明的/`--target` 指定的工具链,graph 供应系统侧时正是需要它的形态);安装失败且诊断已置时,诊断在那里释放(一条因一个话)。e2e 890 钉住窄契约:不可服务目标上声明工具链仍安装、拒绝等 graph 说话(mac/win 宿主腿跑,Linux vacuous 跳过)。

**e2e 工具链版本的统一抽象层(评审要求,架构落地)**:`tests/e2e/_toolchain_env.sh` 是测试学到一个工具链版本的**唯一地点**——三步解析(显式覆盖 `MCPP_E2E_<族>_VERSION` → 注册表里最新已装载荷 → 文件底部的回退常量),变量 `<族>_VERSION` 与 `<族>_ROOT`(llvm/gcc/musl-gcc/mingw-cross 四族)。`_llvm_env.sh` 变为它的别名垫片。本次迁移 llvm 族 38 个文件;移动一条线现在改这个文件底部的常量即可。gcc 系字面量的清扫留给下次触及那些测试的 PR(文件头注释已写明迁移方法)。

**CI 终态(run 37666251572,commit 8b3580dd)**:51/52 绿。openkal 三平台腿全绿——#782 的修复(用户声明照常安装/引擎选择跳过+释放)经 CI 完整验证,#782 关闭。唯一红:bare-Windows 腿,根因是 GitHub 当日把 `windows-latest` 底镜像从 `win25-vs2026/20260925.250` 滚动切换到 `win22/20261004.326.1`,新镜像上 mingw-gcc 16.1.0(sha256 钉未变、归档重下核对含 `cc1plus.exe`)的 `cc1plus` 无法执行——第三方基础设施回归,issue #783 跟踪,不阻塞本 PR。

**`llvm@latest` 的写法(评审问询,未在本批)**:实测 `llvm@latest` 不被接受——mcpp 钉精确发布(SPEC-006 §2.1、SPEC-009 §3.4「索引的 latest 不是默认值」,可复现构建的根基)。「跟最新」的诉求可以由写法糖满足:解析层把 `@latest` 翻译为该族已安装/索引的最高版本并在解析行陈述翻译结果(翻译发生在缓存键之前,报告显示精确版本)。这是语义扩张,立项后单独做;若做,SPEC-006 §2.1 需要相应修订。

---

## 变更记录

| 日期 | 变更 |
|---|---|
| 2026-10-07 | 初版:上游事实(U1-U7)、两仓库现状盘点、八个决策点(D1-D8)、五阶段执行计划与风险登记。 |
| 2026-10-07 | 评审裁决入档:D1 Linux 宿主默认保留 `gcc@16.1.0`(平台设计,不记为偏离,`kFirstRunLinuxOther` 的理由本批补记);D2 随之消解,不新增 linux-arm64 载荷;D4 llvm-dev 不在本批、标记后续做(22.1.8 无 llvm-dev 先例,SPIRV-LLVM-Translator 无 v23.1.3 tag),llvm-tools 仍同批。执行计划、风险与验收同步收敛到三平台。 |
