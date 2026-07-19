# 大型源码直编包全平台化:平台三修(#247/#248/#249)+ build.mcpp 构建期生成能力 + 描述符复杂度治理

日期:2026-07-19
状态:设计评审中(未实施)
关联:mcpp#247 mcpp#248 mcpp#249;mcpp-index compat.ffmpeg(17,543 行)/ compat.opencv(2,739 行);
前置设计:`2026-06-30-l3-build-mcpp-implementation-design.md`(L3 协议)、`2026-07-17-asm-sources-and-general-build-capabilities-design.md`(G1–G9,G2/G3 已落地 0.0.95)

---

## 0. 结论先行

三个 issue 不是三件孤立平台 bug,而是同一件事的三个断面:**ffmpeg/opencv 级"全源码直编"包(数千 TU、冻结 configure 快照、consumer-side 合成)把 mcpp 的平台机械层第一次推到了极限**。linux 腿全绿恰恰说明包形态(描述符文法 + build.mcpp 协议)已经够用;macOS/Windows 挂掉的三处全部是 mcpp 核心的平台实现债,不是包的问题:

- **#247**(Windows):driver-style 链接规则不走 rspfile → 2283 个对象内联溢出 CreateProcess 32KiB。
- **#248**(macOS):`capture_exec` 非 Linux 路径用 shell 字符串拼 env,`ENV… cd <cwd> && bin` 里 env 只绑给 `cd` → dep build.mcpp 收不到 G3 契约环境。
- **#249**(macOS):dep `include_dirs` 一律发 `-I`(最高优先级),大小写不敏感文件系统上 ffmpeg 源根的 `VERSION` 顶替 libc++ `<version>`。

三修全部小而清晰(合计 ~150 行改动),建议一个版本(0.0.100)一起出,并**顺手修掉本次调研发现的第四个隐性大坑**:`generated_files` 每次构建无条件重写 → mtime 抖动 → 冻结快照包(config.h 被全部 TU include)**增量构建完全失效,每次 build 都是全量重编**。这个对 ffmpeg 级包的伤害不亚于三个 issue 本身。

描述符复杂度问题(compat.ffmpeg.lua 17.5k 行)的答案不是"把 configure 搬进 build.mcpp 构建期跑"——那会把维护期的确定性换成消费期的风险,违背"依赖图声明式、命令式困在叶子"的 à la carte 纪律。正确方向是**数据与逻辑分家**:

> **17.5k 行不是逻辑复杂度,是冻结数据放错了容器的观感。**
> 描述符保持声明式骨架;冻结快照(config 头 + 源清单)是维护期流水线生成的**数据**,用生成器压缩(glob 压缩 + common/delta 拆分,预计 17.5k → ~6k 行);
> **不可约的动态部分**(stub 合成、二进制嵌入、per-OS 选择)交给 build.mcpp——为此把 build.mcpp 指令面补全(`source=` / `include-dir=` / `include-dir-after=`),让 compat.opencv 已经验证的混合形态成为**标准模板**,可复制到任意 cmake/autoconf 上游。

---

## 1. 三个 issue 的代码级核实(HEAD = 0.0.99)

三个 issue 的根因描述**全部核实成立**,另有若干 issue 未提到的 nuance 影响修法。

### 1.1 #247 Windows driver-style 链接不走 rspfile —— 确认,含两个修法要点

`src/build/ninja_backend.cppm:567-598`:链接/归档/动态库规则按 `separateLinker` 分两支。
`separateLinker` 不是 dialect 字段,是本地推导(`ninja_backend.cppm:364-365`):

```cpp
const bool separateLinker =
    dial.linkStyle == mcpp::toolchain::CommandDialect::LinkStyle::SeparateLinker;
```

- `SeparateLinker` 支(L567-585,MSVC dialect):三条规则都已走 `@$out.rsp`。✅
- driver-style else 支(L586-598):`cxx_link` / `cxx_archive`(gnu dialect `.archiveCmd = "$ar rcs $out $in"`,`dialect.cppm:84-86`)/ `cxx_shared` 全部内联 `$in`,无 rspfile。❌

**Nuance A(修法落点)**:Windows 托管工具链是 clang-MSVC-ABI,但 `dialect_for`(`dialect.cppm:111-114`)只对 `CompilerId::MSVC`(原生 cl.exe)选 msvc dialect;Clang 落 `kGnuDialect` → driver-style。也就是说**今天 Windows 上唯一可达的 dialect 恰是永远拿不到 rspfile 的那支**。修必须落在 driver-style 分支,不能指望"换 dialect"。

**Nuance B(现成工具)**:`mcpp::platform::is_windows` constexpr(`common.cppm:46-60`)已存在,且 ninja_backend.cppm:601 已在用 `if constexpr` 同款写法;无需新基建。

**Nuance C(边界)**:编译规则(`cxx_module`/`cxx_object`/`cc`/asm/nasm/`cxx_scan`)全部无 rspfile——单 TU 命令行长度有界,不受此 bug 影响,**不必扩改**。

**修法**(与 issue 建议一致,细化两点):

1. driver-style 分支在 `if constexpr (mcpp::platform::is_windows)` 下发 rspfile 三规则(clang++/gcc/GNU 及 llvm-ar 均支持 `@file`);POSIX 分支保持内联(ARG_MAX 足够,零行为变化)。
2. 三处 rspfile 样板已在 SeparateLinker 支重复过一轮,此次再加三条会有六份;顺手抽一个 `emit_rule(name, cmd, useRsp)` 小 helper 消重(纯重构,规则文本逐字节不变)。归档命令沿用 dialect `.archiveCmd` 的 `$ar`,rsp 变体在 helper 内统一改写 `$in → @$out.rsp`,不必给 dialect 加 `archiveRspCmd` 字段(YAGNI:两个 dialect 的 archive 文法都以 `$in` 结尾)。

**验证**:e2e 新增"千对象链接"合成用例(脚本生成 ~1100 个 1 行 .c,`kind=lib` + bin 消费),Windows CI 腿必须过;Linux/macOS 断言 ninja 文件不含 rspfile(防误扩)。

### 1.2 #248 macOS dep build.mcpp 丢 G3 契约环境 —— 确认,建议直接 launcher-unify

`src/platform/process.cppm`:

- Linux 支(L337 起):`posix_spawn` + `merged_environ(extraEnv)` + `addchdir_np(cwd)`。✅
- macOS/else 支(L378-383):拼 shell 字符串,`capture_with_env`(L218-233)把 env 前缀到**最前**,得到 `MCPP_*=… cd <cwd> && <bin> 2>&1` —— POSIX 语义下赋值只绑 `cd`,真正的 bin 一个变量都收不到。❌

**全仓调用点排查**(本次调研新增的关键事实):`extraEnv` 与 `cwd` **同时非空**的调用点全仓只有一处——`build/build_program.cppm:598`(build.mcpp 运行);其余 8 处(编译探针、execute.cppm 的 run_exec 系、ninja 调用)要么 env 空要么 cwd 空,前缀恰好绑对。所以此 bug 精确地只炸 macOS dep/root build.mcpp,与 issue 的现象面完全吻合。

**修法:采纳 issue 的首选方案(launcher-unify),不做 shell 字符串补丁**:

1. `run_exec`(L308)与 `capture_exec`(L337)的 `#if defined(__linux__)` 扩为 `#if defined(__linux__) || defined(__APPLE__)`。
2. `posix_spawn_file_actions_addchdir_np`:macOS 10.15+ 可用,mcpp 的 macOS floor 是 14.0,无需运行时探测。
3. **可移植性唯一坑**:`extern char **environ` 在 macOS 上仅对可执行文件可直接链接,dylib 需 `_NSGetEnviron()`(`<crt_externs.h>`)。mcpp 是可执行文件,现状可用;但 `merged_environ` 里加 `#if defined(__APPLE__)` 走 `*_NSGetEnviron()` 是零成本的 portable-correct 写法,防未来 process 模块进库。
4. 附带收益:macOS 从此消掉 shell 拼接的 quoting/注入面(0.0.97 的 C3 过度引号回归就是这条路径的血债),`TODO(launcher-unify)`(L301-303)兑现一半(Windows 仍走 `_putenv_s`+继承,行为正确,不动)。

**验证**:单测——`capture_exec` 带 `extraEnv`+`cwd` 双非空,断言子进程 `getenv` 可见且 cwd 正确(Linux/macOS 同一断言);e2e——带 build.mcpp 读 `MCPP_OUT_DIR` 的 dep fixture,macOS CI 腿必过(现有 e2e 恰好全部是"不读契约变量"的 build.mcpp,静默容忍了缺失——这本身是测试覆盖教训:**契约变量必须有断言型消费者**)。

### 1.3 #249 `-I` 源根顶替系统头 —— 确认,`include_dirs_after` 是正解,另发现一处不一致

- dep `include_dirs` → `local_include_flags`(`ninja_backend.cppm:90-97`)一律发 `-I`,置于 `$local_includes`,排在 `$cxxflags`(内含工具链 `-isystem`/`-idirafter` 系统头)之前;且 gcc/clang 语义下 `-I` 恒先于一切 system 链搜索。**没有任何降级通道**:`kKnownXpkgKeys`(`manifest/xpkg.cppm:119-124`)只有 `include_dirs`,无 after/system 变体。
- 机制先例已在仓内:`linkmodel.cppm:70-73` 对 payload 头按模式选 `-isystem`/`-idirafter`(GCC 用 `-idirafter` 保 `#include_next` 链),`build_program.cppm:177/200` 同款。此次是把该能力**开放给描述符**。

**为什么选 `-idirafter` 不选 `-isystem`**:`-isystem` 目录仍排在默认系统目录**之前**——`VERSION` 照样顶替 libc++ `<version>`,只是降了警告等级;`-idirafter` 是唯一保证"排在全部系统头之后"的选项,而 `<libavutil/frame.h>` 这类非系统头名不受影响。issue 判断正确。

**修法**:

1. 数据模型:`BuildConfig::includeDirsAfter`(`types.cppm`,紧邻 `includeDirs`);
2. 文法:xpkg `include_dirs_after`(`manifest/xpkg.cppm`,进 `kKnownXpkgKeys` + 别名表)+ mcpp.toml `[build] include_dirs_after`(`toml.cppm`),per-OS 段自动继承(additive overlay 免费获得);
3. 展开:与 `include_dirs` 同走 `expand_dir_glob`(`prepare.cppm:1916` 一带),支持 `*` 源根约定;
4. 传播:沿 Public/Interface 边与 `includeDirs` 同规则传播,**保持 after 属性不衰减**(消费者收到的仍是 `-idirafter`);
5. 发射:`ninja_backend.cppm` `local_include_flags` 旁增 after 变体,附加在 `$local_includes` 尾部(位置无所谓,语义由 flag 本身保证);
6. 生态:compat.ffmpeg 生成器把 `"*"`、`"*/libavcodec"` 等**源根条目**挪到 `include_dirs_after`,`mcpp_generated/*`(自有生成头,需要顶替源内同名头)留在 `include_dirs`。opencv 同步。

**顺手修(本次调研发现的不一致)**:主工程直连路径 `plan.cppm:242-251` 的 `local_include_dirs_for_manifest` 是裸 `root / inc` 拼接,**不走 glob 展开**——与 dep 路径(`prepare.cppm:1916` 走 `expand_dir_glob`)行为分叉。统一到 `expand_dir_glob`,消掉"同一个键两条求值路径"(0.0.98 #242 的教训在 include 轴上的翻版)。

**兼容性注意**:老版本 mcpp 遇到未知键 `include_dirs_after` 会**静默跳过**(`xpkgUnknownKeys` 仅在 `mcpp xpkg parse` 时告警)——即老 mcpp 装新描述符会退化为"缺了这些 include"而非报错。发描述符时用 xpm 版本条目区隔(新形态只发给 >= 0.0.100 的索引轨道),或接受"老版本 macOS 上本来就是坏的"这一现实(#249 场景下降级无损失:目录仍可经 `-I` 补发一份到老轨道)。

### 1.4 第四坑(issue 之外,本次调研发现):`generated_files` 每次构建无条件重写

`materialize_generated_files`(`prepare.cppm:311-361`):每次 prepare 对每个条目无条件 `create_directories` + `ofstream` 写入(L342-354),**无内容比对、无 mtime 保护**。root 每次 build 都走(L1119-1123),dep 在 `loadVersionDep` 每次都走(L1823-1826)。

ninja 是 mtime 驱动的。冻结快照形态下 `mcpp_generated/config.h` 被**全部 2283 个 TU** include(depfile 追踪)——每次 `mcpp build` 重写它 → mtime 变 → **全量重编,增量构建对 ffmpeg 级包完全失效**。讽刺的是 build.mcpp 的缓存设计文档明确写了防的就是这个("regeneration … would otherwise bump mtimes and force spurious rebuilds"),`generated_files` 这条老通道没享受同款纪律。

**修法**(10 行级):写前读旧文件比内容,逐字节相同则跳过写。正确性零风险(内容变化本来就折入指纹,`prepare.cppm:301-306`);收益是冻结快照形态的增量构建从"全废"变"正常"。**此修与 #247/#248/#249 同批出**,否则 ffmpeg 包即使全平台绿了,开发体验也是每次全量重编的假绿。

---

## 2. compat.ffmpeg.lua 为什么 17,543 行 —— 复杂度解剖

生成流水线(mcpp-index `tools/compat-ffmpeg/`,commit 6e844cf):`fetch_upstream.sh`(拉官方 tarball)→ `gen_config.sh`(**维护期跑一次** hermetic `./configure --disable-autodetect`,`make -n` 干跑得到精确源清单)→ `gen_descriptor.py`(吐 lua)。可字节级复现。行数构成:

| 段 | 行数 | 本质 |
|---|---|---|
| 骨架(spec/xpm/顶层 build 配置) | ~350 | 真正的"描述符" |
| linux `sources`(2283 TU 逐条列出) | ~2,300 | **数据**:configure 的 CONFIG_* 门控决定哪些文件编,glob 表达不了 |
| linux `generated_files`(config.h/config.asm/*_list.c 等 15 个文件内联) | ~8,100 | **数据**:configure 输出的冻结快照 |
| macosx `sources`(与 linux 重叠 ~90%) | ~2,100 | **重复数据** |
| macosx `generated_files` | ~5,000 | 数据(per-OS 确实不同,但结构同源) |

即:**~98% 是维护期流水线生成的冻结数据,~2% 是人写的声明**。Windows 腿加入后将再涨 ~7k 行。对照 compat.opencv(2,739 行)已用三招压缩:

1. **glob 压缩**:385 个源文件 → 19 个 dir/brace glob(`{a,b}` 与 `!` 排除,scanner 均已支持,`scanner.cppm:344-398/763-789`);
2. **build.mcpp 合成**:字体嵌入、OpenCL kernel 转码、jpeg12/16 重编 stub——不可约的动态部分收进 ~270 行 C++;
3. **数据文件中转**:`generated_files` 只放小的 `tu_manifest.txt`(数据),build.mcpp 读它展开成 stub(逻辑)——**数据与逻辑分家的雏形**。

ffmpeg 没法把 sources 全 glob 化(configure 门控是文件粒度),但 common/delta 拆分与目录级 glob 收缩空间很大(见 §4 P2)。

### 2.1 方向裁决:复杂度应该住在哪里?

| 方案 | 形态 | 判定 |
|---|---|---|
| A. 全冻结(ffmpeg 现状) | 快照+清单全部内联 | 确定性/可审计最优,但体积随 OS×包数线性爆炸,且掩盖了"哪些是人的决策" |
| B. build.mcpp 构建期跑 configure/等价物 | 描述符极小,构建期算一切 | **否决**。把维护期确定性换成消费期风险:等于每个消费者机器上重跑 autoconf 探测(哪怕用 C++ 重写),破坏 hermetic("--disable-autodetect 后快照可复现"正是这套模式的立身之本)、破坏零 shell、破坏"叶子命令式"纪律,且 per-包重写 configure 是不可维护的天量 |
| **C. 数据/逻辑分家(opencv 形态标准化,推荐)** | 描述符=骨架;冻结快照=压缩后的数据段;build.mcpp=只做合成(读数据、写产物、发指令) | 保留 A 的确定性,吸收 B 的表达力,体积可控(ffmpeg 预估 17.5k → ~6k) |

**原则句**:冻结快照是**维护期决策的存档**,必须留在描述符(或其数据段)里可审计;build.mcpp 只承担**从存档到产物的机械展开**,不承担决策。configure 永远不进消费者机器。

---

## 3. build.mcpp 构建期生成:现有能力与缺口(HEAD 核实)

指令协议现状(`build_program.cppm:104-123`,typed `import mcpp;` 1:1 镜像):

| 能力 | 现状 | 对大型包的影响 |
|---|---|---|
| 追加 c/cxx/ld flags、`-D` | ✅ `cxxflag/cflag/cfg/link-lib/link-search` | 够用 |
| 声明生成**源文件**入编译集 | ✅ `generated=`(要求运行后文件存在,`:617-623`;进 `bc.sources`+`modules.sources`) | 够用(opencv stub 即此) |
| **选择既有源文件**(非生成) | ⚠️ 灰色可行:`generated=<绝对路径>` 恰好能过存在性检查,scanner 对绝对字面量直取(`scanner.cppm:776-783`)——**未文档化、语义错位** | 需要正名 |
| 追加 include 目录 | ❌ 无指令;只能裸发 `cxxflag=-I…`(不规范化、c/cxx 要各发一遍、无 after 变体) | opencv 已被迫用裸 flag(`:412-413`) |
| per-glob/per-file flags | ❌ 无指令 | 实际不缺:描述符 `flags` 的 `**/…` glob 对 OUT_DIR 产物也能命中(leading-`**` 匹配绝对路径),opencv `**/tu/jpeg12/**` 即此用法——**文档化即可,不加指令** |
| `.o` 直入链接 | ❌(设计文档显式非目标) | 维持:汇编已是一等源,无需求 |
| root 拿 `MCPP_DEP_*_DIR` | ❌(root build.mcpp 跑在依赖解析前,`prepare.cppm:2991-2993` 已记 follow-up) | 消费侧合成(如 ffmpeg-m 读 compat.ffmpeg 源树)需要 |
| 缓存 | ✅ 契约 env 整体折入 ctxHash,声明式 I/O 重跑门 | 设计良好,新指令按既有 `d <kind>` 记录扩展即可 |

### 3.1 P1:指令面补全(小步、正名、不扩权)

新增三条指令 + 一处时序修正,全部落 `build_program.cppm` 的 `parse_line`/`apply` + typed 库同步:

1. **`mcpp:source=<path>`** —— 把"选择既有源入编译集"正名。相对路径:root 相对包根、dep 相对 `MCPP_MANIFEST_DIR`;绝对路径直取。与 `generated=` 的差异仅在语义申明(不承诺"本程序写出的");落点同款(`bc.sources`+`modules.sources`,存在性检查同款)。`generated=` 的绝对路径灰色用法保持兼容并在文档里指向 `source=`。
2. **`mcpp:include-dir=<path>` / `mcpp:include-dir-after=<path>`** —— 路径按上述规则解析规范化,分别进 `buildConfig.includeDirs` / `includeDirsAfter`(与 §1.3 的新字段共用通道,一次实现两处受益)。**作用域维持 Cargo 纪律:私有**(只进本包 TU),不做 usage requirement 传播——需要传播的 include 属于描述符声明面,不属于构建期程序(防"构建期程序悄悄改公共接口"这一供应链面)。
3. **`MCPP_TARGET_OS` / `MCPP_TARGET_ARCH` / `MCPP_TARGET_ENV`**(契约 env 便利拆分,Cargo `CARGO_CFG_TARGET_*` 对应)—— 三元组解析在 mcpp 侧做一次,免每个 build.mcpp 手撕字符串(ffmpeg/opencv 的 per-OS 选择逻辑直接受益)。
4. **root `MCPP_DEP_*_DIR`**:把 root build.mcpp 的运行点从依赖解析前(`prepare.cppm:1180`)移到 dep 解析完成后、modgraph scan 前(dep build.mcpp 循环 `prepare.cppm:2953` 同一带)。风险点单测锁死:root build.mcpp 的 `generated=` 输出仍须先于 scan 注册(现有顺序保证不变量:materialize → build.mcpp → scan)。

不做的(显式非目标):per-glob flags 指令(现有描述符 glob 通道已覆盖)、`.o` 注入、任何"加依赖"能力(叶子纪律不破)、`install()` 替代(那是 xim 层)。

### 3.2 兼容与信任面

- 新指令老版本 mcpp 收到会打 "ignoring unknown directive" 警告并继续(`:121` 前向兼容已设计好)——包侧用 `min_mcpp`/xpm 版本轨道声明下限。
- 信任模型不变(build.rs 等价:构建即执行包的 build.mcpp);新指令没有扩大能力面(include-dir 私有、source 仅指向已下载 payload 内文件)。

---

## 4. 描述符瘦身:P2(mcpp-index 侧,mcpp 零改动或近零改动)

**目标:把 opencv 三招沉淀为可复制的生成器框架,ffmpeg 描述符 17.5k → ~6k 行,新包(SDL/curl/sqlite/…)按模板一周内成型。**

1. **`tools/compat-gen/` 通用框架**(把 `tools/compat-ffmpeg/gen_descriptor.py` 泛化):
   输入 = per-OS 构建快照(CI matrix 各腿跑维护期 configure/cmake 一次,上传 artifact)+ 包插件(~100 行:声明 GEN_FILES、flags 规则、glob 压缩策略);
   输出 = 单一 lua 描述符 + 字节级复现校验(现有 "byte-identical regeneration" 纪律固化为 CI check)。
2. **common/delta 拆分**(纯生成器逻辑,mcpp 免改):per-OS 段是 additive overlay(`xpkg.cppm:801-805`),生成器把 sources 交集提升到顶层、per-OS 只留差集——ffmpeg linux/macosx 源清单重叠 ~90%,仅此一项省 ~2,000 行,加 Windows 腿的边际成本从 ~7k 降到 ~1k。
   ⚠️ 边界:`generated_files` 是 map-insert(先到先得),**同 key 不能 per-OS 覆盖顶层**——config 快照必须整文件留在 per-OS 段(现状即正确,不要试图 delta 化配置文件,得不偿失)。
3. **glob 收缩**:`make -n` 清单先按目录聚类,整目录全选的收成 `dir/*.c`,零散的保留逐条 + `!` 排除;opencv 已验证(385→19)。
4. **数据/逻辑分家模板**:大数据表(如 stub 清单)走 `generated_files` 落成 `.txt` 数据文件,build.mcpp 读取展开——描述符里数据是数据、程序是程序,review 面清晰。
5. **per-OS 段跨 OS 校验**(本次调研发现的盲区):非宿主 OS 段被 skip-table 跳过(`xpkg.cppm:1394-1399`),windows 段的 typo 在 linux CI 上不可见——`mcpp xpkg parse` 增加 `--all-os` 模式逐 OS 各 parse 一遍,mcpp-index CI 接入。此为 mcpp 侧小改(parse 命令层,不动构建)。

**显式否决的备选**:快照外置为独立下载资产(第二条 url/sha256 腿)——多一个供应链验证面、破坏索引单文件原子性、离线优先受损;17.5k 行文本在索引 tarball 里压缩后不足 100KB,体积不是真问题,**review 信噪比**才是,而那由 common/delta+glob 收缩解决。

---

## 5. 实施切分与版本

| 批次 | 内容 | 落点 | 规模 |
|---|---|---|---|
| **P0(0.0.100,单 PR)** | #247 rspfile(含 emit_rule 消重)· #248 launcher-unify(`\|\| __APPLE__` + `_NSGetEnviron`)· #249 `include_dirs_after` 全链(types/xpkg/toml/expand/propagate/ninja)+ plan.cppm include glob 统一 · §1.4 generated_files 内容比对跳写 | mcpp | ~150 行核心 + 测试 |
| **P1(0.0.101)** | `mcpp:source=` · `mcpp:include-dir[/-after]=` · `MCPP_TARGET_OS/ARCH/ENV` · root `MCPP_DEP_*`(时序后移) · feature 未知子键告警(`xpkg.cppm:1183-1187` 现静默吞) | mcpp | 中 |
| **P2(随 P0/P1 各自发布后)** | compat-gen 框架 · ffmpeg/opencv 描述符再生成(common/delta+glob 收缩+include_dirs_after 迁移)· `xpkg parse --all-os` + CI 接入 · windows 腿(ffmpeg PR#89 系列收口) | mcpp-index(+mcpp parse 层小改) | 生成器工程 |
| P3(挂账不排期) | feature 门控 include_dirs/generated_files · sources 外置清单键(`sources_manifest =`)· 描述符规模守卫(parse 内存/条目数上限告警) | mcpp | 待需求实证 |

P0 内四项互不纠缠但共享同一验收场景(ffmpeg/opencv 全平台),按 0.0.97 的经验单 PR 逐簇 commit;**e2e 必须含全平台冒烟**(#247 只在 Windows 现形、#248/#249 只在 macOS 现形、§1.4 只在二次构建现形——单平台单次构建的 CI 全绿恰是这批 bug 能活到今天的原因)。

验收基线(全部来自现成复现件):
- Windows:mcpp-index spike 分支 multi-platform compat.ffmpeg `mcpp test` 链接过(#247);
- macOS:spike PR#90 compat.opencvmac build.mcpp 拿到 `MCPP_OUT_DIR`(#248);PR#91 `mcpp test -p ffmpeg-module`(`import ffmpeg.av`)过(#249);
- 全平台:ffmpeg 包二次 `mcpp build` 零重编(§1.4)。

---

## 6. 风险与开放问题

1. **#248 改的是进程启动原语**,macOS 全部 `capture_exec`/`run_exec` 调用点(含编译探针、ninja 调起)行为从 shell 换 posix_spawn——理论上更严格(不再有 shell 展开)。需全量 e2e 在 macOS 过一遍;若有调用点暗依赖 shell 语义(如命令里带重定向字符串),会在此暴露。现调研未发现此类调用点(`2>&1` 由 capture 侧 pipe 取代),列为 PR 内验证项。
2. **include_dirs_after 传播语义**:传播链(Public/Interface)上 after 属性保持,但消费者自己的 `-I` 仍在 dep 的 `-idirafter` 之前——符合预期(消费者自有头最优先)。开放问题:是否给 `include_dirs` 整体加"默认 `-isystem` 化"的远期开关(压制 dep 头的告警噪声)——与 #249 正交,挂 P3。
3. **root build.mcpp 时序后移**(P1-4)是 P1 里唯一动既有不变量的项:root 的 `generated=` 源此前在 dep 解析前就已注册,后移不影响(scan 在更后),但 root build.mcpp 发出的 `cxxflag` 此前理论上可影响 dep 的…… 不能——dep 编译用 dep 自己的 buildConfig,root flags 不下渗(Cargo 纪律),确认无耦合。单测锁:后移前后 root+dep 双 build.mcpp fixture 的 ninja 产物逐字节一致。
4. **老 mcpp × 新描述符**的静默降级(§1.3 兼容性注意):接受现实 + xpm 轨道区隔,不做描述符侧 shim。
