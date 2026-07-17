# 汇编源一等公民 + 通用构建能力(G1–G9)设计方案

> 日期:2026-07-17
> 需求来源:[2026-07-17-mcpp-feature-requests.md](2026-07-17-mcpp-feature-requests.md)(G1–G9 清单)
> 基线:mcpp 0.0.94(HEAD `450c24b`),xlings 已打包 nasm(G7 供给侧已完成)
> 本文所有 file:line 锚点均在 0.0.94 源码上核实。

---

## 0. 结论先行

G1–G9 全部与现有架构**兼容且低摩擦**,没有一条需要推翻既有抽象:

| 项 | 合理性结论 | 实现量级 | 排入 PR |
|---|---|---|---|
| G1 `.S`/`.asm` 原生汇编源 | ✅ 三处扩展名路由 + 一条 ninja 规则,架构零改动 | 中 | **PR-A** |
| G7 nasm 供给 | ✅ xlings 已打包;mcpp 侧只差 `ensure_nasm` 按需触发 | 小 | PR-A |
| G8a glob 不跟目录软链接 | ✅ 已定位:`recursive_directory_iterator` 默认选项 | 小(scanner 已打开) | PR-A |
| G5 mcpp.toml features gate sources | ✅ 纯解析器覆盖缺口,struct 字段已存在 | 小 | **PR-B** |
| G6 generated_files 进 mcpp.toml | ✅ 同上,materialize/指纹链路格式无关已通 | 小 | PR-B |
| G1b cfg 条件化 sources(新识别) | ✅ L1 条件机制的自然延伸,`.asm` 跨架构可用的前提 | 小 | PR-B |
| G4 per-glob flags | ✅ `scan_overrides` 就是现成的 per-glob 模板,per-TU flags 管道端到端已存在 | 中 | PR-B |
| G8b 相对 `-I` 对 `.cppm` 不生效 | ⚠️ 现象待复现定位;修复点在 flags 组装,随 G4 顺手 | 小 | PR-B |
| G3 build.mcpp 环境契约 + cross | ✅ env 注入点唯一(`capture_exec`);cross 跳过是显式 TODO | 中 | **PR-C** |
| G2 依赖包 build.mcpp | ✅ leaf-only 是显式设计注释,插入点明确;共享 registry 只读是主要风险 | 大 | PR-C |
| G9 跨项目产物缓存 | ✅ 与源码构建哲学不矛盾(缓存键含工具链指纹);生态级远期 | 大 | 路线图,不排期 |

**全部工作收敛为一个版本 0.0.95、三个 PR**:PR-A 汇编源(G1+G7+G8a)→ PR-B 声明式清单能力(G5+G6+G1b+G4+G8b)→ PR-C build.mcpp 补全(G3+G2)。版本 bump 与发布只在 PR-C 合入后做一次。G9 只留设计草图。

---

## 1. 架构合理性分析(逐条对照源码)

### 1.1 G1:为什么"一等公民"是对的,以及架构上它意味着什么

mcpp 的源文件处理链是:**glob 发现 → scanner 分类 → plan 对象命名/收集 → ninja 规则选择 → 链接对象收集**。扩展名路由分散在三个独立位置,**没有中心注册表**:

1. scanner 分类:`.c`/`.m` 跳过模块文本扫描 — `src/modgraph/scanner.cppm:264-266`;
2. plan 侧 `is_implementation_source()` 只认 `.cpp/.cc/.cxx/.c/.m` — `src/build/plan.cppm:200-203`;链接对象收集以此为门 — `plan.cppm:599-613, 745-760`;
3. ninja 规则选择 `pick_rule()` — `src/build/ninja_backend.cppm:530-537`(`is_c_source()` 同逻辑在 `compile_commands.cppm:52-54` 还有一份拷贝)。

这直接印证"一等公民 or 静默坏掉"的判断:一个新扩展名如果只改了 glob 而没改 `is_implementation_source()`,它的 `.o` 会**被静默丢出链接线**(`plan.cppm:602/608` 的扩展名门),不报错。所以 G1 的成本恰好就是把这三处 + cdb 一处改齐,并加单测锁住一致性——这也是为什么"让每个包用 build.mcpp 自己编汇编"是更差的路线:build.mcpp 产物走 `mcpp:generated=` 只能是**源文件**,`.o` 级产物没有进入链接线的协议,且 cross 下 build.mcpp 目前整体跳过(`build_program.cppm:388-393`)。

其余支撑事实:

- **`.S`(GAS)几乎免费**:`.c` 走 `c_object` 规则(`$cc $cflags -c`,`ninja_backend.cppm:397-407`),gcc/clang 驱动器按扩展名自动对 `.S` 做 cpp 预处理+汇编。唯一要避免的是把 `-std=c17` 等 C 专属旗标喂给汇编单元(触发驱动器告警),故给汇编单独一条规则、单独一份 flag 组装(见 §2.1)。
- **`.asm`(NASM)绕过 CommandDialect 是正确的**:`CommandDialect`(`src/toolchain/dialect.cppm:22-54`)抽象的是"同一语义、不同拼写"(gnu vs msvc);nasm 只有一种拼写,不进 dialect,自建规则即可。`-f` 格式从 `Triple`(`src/toolchain/triple.cppm:32-60`,已有 `is_pe()`/`family()`)推导,零新概念。
- **增量/并行自动获得**:per-source 增量是 ninja 的职责(mtime + command hash),新源类型有 ninja edge 即得;nasm 自带 `-MD` 依赖输出,`%include` 追踪走 ninja deps 通道,与 `c_object` 同款。
- **指纹**:输出目录指纹的 flags 哈希在 `canonical_compile_flags()`(`src/build/prepare.cppm:182-229`),需追加 asm 维度(见 §2.1.6)。

**Cargo/Zig 对照**(需求文档已给):Cargo 无原生汇编逼出 cc/nasm-rs crate 生态,构建逻辑碎在几千个 build.rs 里;Zig `addAssemblyFile` 声明式进图。mcpp 的声明式依赖图哲学("依赖图保持声明式、命令式困在叶子",见 2026-06-29-manifest-environment-and-platform-design)与 Zig 同路。

### 1.2 G5/G6:不是新功能,是解析器覆盖缺口

两个描述符语法(mcpp.toml 与 index 描述符)**合成到同一份数据模型** `mcpp.manifest.types`(`src/manifest/manifest.cppm:14-18`)。核对结果:

- `BuildConfig::featureSources`(`types.cppm:132`)与 `BuildConfig::generatedFiles`(`types.cppm:142`)**字段已存在**;
- xpkg 解析器两者都填:feature `sources` 分发在 `xpkg.cppm:976`,`generated_files` 在 `xpkg.cppm:843-863`;
- toml 解析器两者都没有分支:`[features]` 表单只处理 implies/defines/requires/provides(`toml.cppm:149-183`),无 `sources`;全文无 `generated` 引用;
- 消费侧完全格式无关:featureSources 的 drop+add 在 `prepare.cppm:2481-2525`(0.0.94 已修双模式);generatedFiles 的落盘在 `materialize_generated_files()`(`prepare.cppm:278-330`,调用点 `:1561`),内容进包指纹(`prepare.cppm:268-273`),落盘后被正常 glob 匹配——**build 侧零改动**。

Schema 准入(2026-06-04-manifest-schema-ownership,"closed grammar, open vocabulary"):两个字段都是**固定形状的机制 + 开放值域**(feature 名/路径/内容是值不是键),满足 Rule A/B/C,无需新解析语义。结论:G5/G6 就是给 `toml.cppm` 各加一个分支 + 文档,是本清单里性价比最高的两项。

### 1.3 G1b(新识别):cfg 条件化 sources —— `.asm` 跨架构可用的前提

分析 G1 时发现的缺口:x86 汇编源**只能在 x86 目标上进构建**,但目前所有源集机制都不能按 target 条件化——`[target.'cfg(...)'.build]` 只合并 flags(`ConditionalConfig` `types.cppm:245-256`,合并点 `prepare.cppm:714-736` 仅 cflags/cxxflags/ldflags),featureSources 按 feature 不按 target。没有它,含 `.asm` 的包在 aarch64 上要么硬报错要么用户手工开关 feature。

修法是 L1 机制的自然延伸:`ConditionalConfig` 增加 `sources` 字段,`[target.'cfg(arch = "x86_64")'.build] sources = [...]` 求值后 APPEND 进 `bc.sources`(与 flags 同一合并点,同样"按解析后的 target 求值"——这正是 L1 设计当初选对的地方,交叉编译直接正确)。`!` 排除 glob 由 scanner 既有的正负 glob 集处理(`scanner.cppm:420-433`)。归入 PR-B 的"声明式源集三补齐"。

### 1.4 G4:per-glob flags —— 管道已存在,只差接线

三个关键事实说明 G4 是接线不是架构:

1. **per-glob 机制已有模板**:`[scan_overrides."<glob>"]`(解析 `toml.cppm:194-224`,消费 `scanner.cppm:446-480`)就是"glob → 命中源文件 → 附加元数据"的完整先例;
2. **per-TU flags 管道端到端已通**:`SourceUnit.packageCflags/packageCxxflags`(scanner 附加,`scanner.cppm:449-490`)→ `CompileUnit`(`plan.cppm:431-433`)→ ninja edge 变量 `unit_cflags`/`unit_cxxflags`(`ninja_backend.cppm:640-646, 687-693`);
3. **per-TU 增量自动正确**:flags 变化改变 edge command,ninja 按 command hash 只重编命中的 TU。

唯一的设计工作是语义定界(见 §2.3):匹配顺序、多 glob 叠加、与 feature defines/`[target.*]` per-target flags 的组合次序。

### 1.5 G2/G3:build.mcpp 补全 —— 跳过是显式设计债,不是意外

- **G3**:env 注入点唯一且为空——`capture_exec({bin}, {}, root)`(`build_program.cppm:459`),第二参数就是 env map,契约变量从这里注入,一处改。cross 跳过是带注释的显式 TODO(`build_program.cppm:388-393`:"host-toolchain-for-cross is a follow-up")。真正的工作量在**cross 下解析宿主工具链**:当前传入的 `hostCompiler` 在 `--target` 构建里是交叉编译器前端,需要独立解析 host 三元组的默认工具链(`host_triple()` `triple.cppm:118-125` + registry 既有解析路径)。
- **G2**:leaf-only 是设计注释明说的(`prepare.cppm:962-963`:"Leaf-only: it cannot gate the top-level dependency graph"),全树唯一调用点 `prepare.cppm:965`。依赖侧插入点在 per-dep prepare 流(`prepare.cppm:1366-1440` 一带,`materialize_generated_files` 调用点 `:1561` 之侧)。**主要风险已知**:依赖根在 `~/.mcpp/registry` 共享且可能只读(mcpp-index smoke 只读 pack 事故的同款教训),所以 dep 的 build.mcpp 产物目录必须**项目本地化**,不能写进共享包根(见 §2.4.2)。
- **作用域照 Cargo 是对的且现成**:`apply()`(`build_program.cppm:364-371`)写的是"所在 manifest 的 bc"——对 dep manifest 调用即天然作用域正确:cxxflag/cfg 只进该包自身 TU;`link-lib`/`link-search` 写进 dep 的 `bc.ldflags`,而 per-package ldflags 传播到终链的通道已存在(openblas per-OS ldflags 先例)。

**信任模型必须写明**:G2 意味着构建任意依赖包会执行其 build.mcpp(Cargo build.rs 同模型)。mcpp 生态已接受同级别信任(index 描述符的 `install()` Lua 已在 xim 沙箱执行),但 build.mcpp 是全权 C++,doc 与 changelog 必须显式声明这一点。

### 1.6 G8:两个一致性 bug

- **G8a 已定位**:`expand_glob` 用 `recursive_directory_iterator(root)` 默认构造(`scanner.cppm:210-221`),默认**不跟目录软链接**。修 = 加 `follow_directory_symlink` + `canonical` 去重防环。vendored 大库常用软链目录组织第三方源,POC 实测命中。
- **G8b 待定位**:相对 `-I` 对 `.cppm` 单元不生效。可疑点:模块单元编译的 cwd 与 include 组装(`flags.cppm:310-320` includes 拼接)相对基准不一致。修法方向:include dirs 在 flags 组装时统一绝对化。**先加最小复现 e2e,再修**(systematic-debugging 原则),随 G4 的 flags PR 顺手。

### 1.7 G9:远期,方向正确

缓存键 = 工具链指纹(已有 10 字段 FNV 指纹,`fingerprint.cppm:90-115`)× 源内容哈希 × canonical flags——**指纹体系就是为此铺的路**,与"源码构建"哲学不矛盾(缓存的是"这套工具链+这套源+这套旗标"的确定性产物)。依赖 hash-as-identity(L1b 路线图项)。本文不排期,只锚定:实现时缓存放 `~/.mcpp/cache/`,以 registry 只读共享的同款血统纪律管理。

---

## 2. 设计细节

### 2.1 G1:`.S`/`.s`/`.asm` 原生汇编源(PR-A)

#### 2.1.1 扩展名与路由

| 扩展 | 含义 | 编译器 | ninja 规则 |
|---|---|---|---|
| `.S` / `.s` | GAS 汇编(`.S` 过 cpp 预处理,`.s` 不过;区分交给驱动器) | `$cc`(gcc/clang 驱动) | 新 `asm_object` |
| `.asm` | NASM 汇编 | `nasm` | 新 `nasm_object` |

`.S`/`.s` 同时收:Windows 大小写不敏感文件系统下二者不可区分,一起路由到 cc 驱动最稳(驱动器自己按真实扩展名决定是否预处理)。

**默认 glob 收窄决策**:默认 sources glob(`toml.cppm:878-887`)从 `src/**/*.{cppm,cpp,cc,c}` 扩为 `src/**/*.{cppm,cpp,cc,c,S,s,asm}`。理由:树里有汇编源的项目几乎必然想编它;此前这些文件根本不进构建,行为变化是"新文件入图"而非"旧行为改变";不想要的用 `!` 排除。MASM 语法 `.asm` 在 nasm 下会硬报错(而非静默错编),错误信息给出排除指引(见 2.1.7)。

**改齐五处路由**(§1.1 的三处 + cdb + P1689):

1. `scanner.cppm:264-266`:`.S/.s/.asm` 加入跳过模块扫描分支(同 `.c`);
2. `plan.cppm:200-203`:`is_implementation_source()` 增列三扩展(链接收集 `:599-613/:745-760` 自动跟随);
3. `ninja_backend.cppm:530-537`:`pick_rule()` 路由 `.S/.s`→`asm_object`、`.asm`→`nasm_object`;`is_c_source` 的 P1689 扫描门(`:551-552, :583, :657`)同步扩为 `is_non_scannable_source`;
4. `compile_commands.cppm:52-54`:`.S/.s` 以 cc 命令进 cdb;**`.asm` 不进 cdb**(clangd 等工具不识别 nasm 命令行,进了反而害 LSP);
5. `p1689.cppm:388-389`:备用 scanner 路径同步。

加一个单测锁一致性:枚举全部受支持扩展,断言"scanner 分类 / is_implementation_source / pick_rule"三方对每个扩展的行为组合合法(防止未来再加扩展名漏改)。

#### 2.1.2 ninja 规则

```ninja
rule asm_object
  command = $cc $asmflags -c $in -o $out    # GNU; 按 dialect compileOnly/outputObjPrefix 拼
  deps = gcc
  depfile = $out.d                          # 经 -MD -MF(.S 走 cpp 才有头依赖;.s 无依赖,depfile 为空可接受)

rule nasm_object
  command = $nasm -f $nasmfmt $nasmflags -MD $out.d -MQ $out -o $out $in
  deps = gcc
  depfile = $out.d
```

两条规则都按需 emit(仿 `need_c_rule` 门,`ninja_backend.cppm:278-284`)。`$asmflags`/`$nasmflags` 是全局变量 + `unit_asmflags` edge 变量(G4 到来后 per-glob 附加)。

#### 2.1.3 flags 组装(`flags.cppm` 新增 `f.as` / `f.nasm`)

- `f.as`(喂 `$cc` 的汇编单元):includes(`-I`,`.S` 的 cpp 要用)+ defines + 目标选择/sysroot/`-B` 等 toolchain 定位旗标(**必须继承**,hermetic link model 的教训:宿主污染假绿)——但**不含** `-std=`、`-O`、方言旗标。PIC:不注入(汇编的 PIC 是源码自身的事,注 `-fPIC` 对 `.s` 无意义)。
- `f.nasm`:`-D<define>`(defines 直通,`-D` 拼写同 GNU)+ `-I<dir>/`(**注意 nasm 的 `-I` 需以路径分隔符结尾**,组装时补齐)+ debug(profile 带调试时 ELF 上加 `-g -F dwarf`,PE/Mach-O 上省略——nasm 各格式调试支持不齐)。
- MSVC 工具链(family=Msvc)遇 `.S/.s`:**硬报错**"GAS assembly is not supported by the MSVC toolchain"(cl 没有 GAS 通道;MASM 支持是显式 non-goal)。遇 `.asm`:正常走 nasm(nasm 产 COFF,与 cl 对象互链没问题)。

#### 2.1.4 NASM `-f` 从 Triple 推导

新增 `triple.cppm` 方法(与 `is_pe()`/`family()` 并列):

```cpp
// Triple::nasm_format(): x86 目标 → nasm 输出格式;非 x86 → nullopt(调用方硬报错)
// linux/musl: x86_64→elf64, x86→elf32
// windows(gnu|msvc): x86_64→win64, x86→win32
// macos: x86_64→macho64
```

aarch64 目标遇到 `.asm` 单元 → 硬报错,错误信息指引用 `[target.'cfg(arch = "x86_64")'.build] sources` 门控(G1b,PR-B 落地;PR-A 单独在 main 上的窗口期内文案先指引 `!` 排除,同版本发布无用户可见断层)。

#### 2.1.5 nasm 获取(G7 mcpp 侧收口)

仿 `ensure_ninja`(`xlings.cppm:266/:1209`)加 `ensure_nasm(env, quiet, cb)`:

- **惰性触发**:仅当 scan 后计划里存在 ≥1 个 `.asm` 单元才调用(在 plan→backend 之间,拿到单元列表后);纯 C/C++ 项目零感知;
- 解析顺序:PATH 里已有 `nasm` 且 `nasm -v` ≥ 2.16 → 直接用;否则 `install_direct(env, "xim:nasm")` 走 xlings(已打包);装完仍不可用或版本过低 → **硬失败**,不降级不静默跳(用户既定原则);
- `nasm` 绝对路径进 ninja 变量 `$nasm`。

#### 2.1.6 指纹与增量

- per-source 增量:ninja edge + depfile 自动获得,无 mcpp 侧工作;NASM 用自带 `-MD/-MQ` 走 ninja deps 通道(`%include` 追踪);
- 输出目录指纹(**实施时修订**):PR-A **不**把 nasm 版本折入目录指纹——默认 glob 扩展后人人的 sources 配置都含 `.asm` 模式,"静态含 .asm 就解析 nasm"会迫使每个项目都跑 ensure_nasm,违背惰性原则。替代保证:nasm 绝对路径进 ninja `$nasm` 变量 → 换 nasm(路径变)即 command hash 变即重编;asm 相关全局旗标(`f.as`/`f.nasm`)全部派生自已在指纹里的输入(profile/includes/toolchain)。同路径原地升级 nasm 不触发重编,与既有 c_object 无 depfile 的容忍度同级,接受;用户侧 `asmflags` 键落地时(PR-B per-glob)再进 `canonical_compile_flags`;
- `mcpp test` 路径:0.0.94 修复已保证 featureSources 双模式一致,汇编源走同一 sources 通道,无需特判——e2e 里 build/test 双路径都锁(feature 诊断双路径对比的教训)。

#### 2.1.7 错误面

- MASM 语法喂 nasm:nasm 自身报错,mcpp 包一层提示:"if this is MASM/GAS syntax, exclude it via `!` glob or rename; mcpp routes `.asm` to NASM";
- 非 x86 目标遇 `.asm`:见 2.1.4;
- nasm 缺失/版本过低:硬失败 + `xlings install nasm` 指引。

#### 2.1.8 G8a 顺手修(scanner 已打开)

`expand_glob`/`expand_dir_glob`(`scanner.cppm:210-247`)改用 `directory_options::follow_directory_symlink`,以 `fs::canonical` 结果集去重防环;新增含软链目录的单测 + e2e 断言。

### 2.2 声明式源集三补齐:G5 + G6 + G1b(PR-B)

#### 2.2.1 G5:`[features]` 的 `sources`

```toml
[features]
# 表单形式增加 sources 键,与 xpkg 语义逐字对齐(xpkg.cppm:976)
jpeg = { sources = ["src/codecs/jpeg/**"], defines = ["HAS_JPEG"], deps = [] }
```

- 解析:`toml.cppm:149-183` 表单分支增 `sources` → `m.buildConfig.featureSources[fname]`(struct 已有,`types.cppm:132`);
- 消费:零改动(`prepare.cppm:2481-2525` 的 drop+add 格式无关,0.0.94 已修双模式);
- 单测:toml 解析进 featureSources;**再加一条 toml/xpkg 对称性单测**:同一逻辑内容分别用两种语法写,断言合成的 `Manifest` 相等(把"两语法一模型"从惯例升级为被测契约,防止下次再长出不对称)。

#### 2.2.2 G6:`[generated_files]`

```toml
[generated_files]
"src/wrap/module.cppm" = """
module;
#include <vendored.h>
export module vendored;
...
"""
```

- 解析:`toml.cppm` 新节,镜像 `xpkg.cppm:843-863` → `m.buildConfig.generatedFiles`;路径合法性(相对、不越根)由消费端已有校验兜底(`prepare.cppm:283-296`),解析端重复校验一次给出更好的报错位置;
- 消费/指纹:零改动(`materialize_generated_files` + `genfile:` 指纹折入 `prepare.cppm:268-273`);
- 注意调用点:`materialize` 目前只对**已解析依赖**调用(`prepare.cppm:1561`);根项目自身的 generatedFiles 需要补一个根侧调用(scan 之前)——小改,单测锁住。

#### 2.2.3 G1b:`[target.'cfg(...)'.build] sources`

- `ConditionalConfig`(`types.cppm:245-256`)增 `sources` 字段;解析 `toml.cppm:747-781` 增分支;合并点 `prepare.cppm:714-736` APPEND 进 `bc.sources`(在 build.mcpp 与 scan 之前,顺序天然正确);
- 求值语义沿用 L1:**按解析后的 target**(交叉编译正确性);
- xpkg 侧:index 描述符同样需要表达(ffmpeg/x264 按 arch 选汇编目录),`xpkg.cppm` 增 `target_cfg` 键解析进同一 `conditionalConfigs`。**描述符语法新增 → 走 index_contract 的 min-version 机制**(`pm/index_contract.cppm:108`):使用该键的描述符声明 `min_mcpp = "0.0.95"`,旧 mcpp 拿到新描述符时按既有 floor 逻辑处理,不静默错编。

#### 2.2.4 文档

`docs/05-mcpp-toml.md` 增补三节;schema-ownership 文档的准入三问在 PR 描述里逐条回答(三项都是"机制固定、值域开放",通过)。

### 2.3 G4:per-glob flags(PR-B)

#### 2.3.1 语法与语义

```toml
[build.flags."src/simd/**/*.avx2.cpp"]
cxxflags = ["-mavx2"]

[build.flags."third_party/**"]
cflags   = ["-w"]
cxxflags = ["-w"]

[build.flags."src/x86/**/*.asm"]
asmflags = ["-DPREFIX"]          # 喂 nasm 或 as,按该文件的路由决定
```

- **键集**:`cflags` / `cxxflags` / `asmflags` / `defines`(defines 双拼写展开,同 `mcpp:cfg=` 的处理);不含 `ldflags`(链接无 per-TU 概念);
- **匹配语义**:对每个已发现源文件,**按声明顺序**遍历所有 glob,命中即 APPEND(后声明的排后,自然覆盖 GCC 系"后旗标胜"语义);glob 匹配复用 `path_matches_glob`(`scanner.cppm:95-140`),相对包根;
- **组合次序**(最终 per-TU 命令行):全局 flags(profile/toolchain)→ 包级 flags(descriptor/manifest)→ feature defines → per-glob(最特异,排最后);
- **零命中告警**:一个 glob 若未命中任何源文件,warn(拼错 glob 的静默失效是这类机制的经典坑);
- **作用域**:mcpp.toml 与 xpkg 描述符都支持(OpenCV SIMD dispatch 在描述符里写);per-glob flags 是**私有构建旗标**,不进 usage requirements,不传播消费者。

#### 2.3.2 实现落点

- struct:`BuildConfig` 增 `std::vector<std::pair<std::string, PerGlobFlags>> globFlags`(**vector 不是 map——顺序即语义**);
- 解析:`toml.cppm` 仿 `scan_overrides`(`:194-224`);`xpkg.cppm` 对称分支;
- 消费:scanner 附加 per-unit flags 处(`scanner.cppm:449-490`),命中即 append 到 `SourceUnit.packageCflags/packageCxxflags`(既有管道直达 ninja `unit_*flags`);汇编单元新增 `packageAsmflags` 一路(随 G1 的 `unit_asmflags`);
- 指纹:`canonical_compile_flags()` 折入 globFlags 全序列(glob 串 + 旗标),保证输出目录一致性;per-TU 增量由 ninja command hash 自动正确。

#### 2.3.3 G8b 顺手修

先写最小复现 e2e(`.cppm` 单元 + 相对 `include_dirs`),定位后大概率修在 flags 组装处统一 `fs::absolute`(以包根为基准)。若复现发现根因在别处(如 BMI 二次编译的 cwd),按实际根因单独小 PR,不硬塞。

### 2.4 G3 + G2:build.mcpp 补全(PR-C,单 PR 内两阶段)

**实现顺序刻意:先契约(G3)后扩围(G2)**——依赖包的 build.mcpp 没有 TARGET/features 上下文几乎不可用,契约先行才不用发两版协议。同一 PR 内按此顺序分两组 commit 实现(squash 后对外是一个变更),G3 部分完成即先跑全量回归再叠 G2。

#### 2.4.1 G3:环境契约 + cross 运行(第一阶段,root-only)

**注入变量**(`capture_exec` 第二参数,`build_program.cppm:459` 一处改):

| 变量 | 值 | Cargo 对应 |
|---|---|---|
| `MCPP_TARGET` | 解析后的 canonical 三元组(`x86_64-windows-gnu`) | `TARGET` |
| `MCPP_HOST` | `host_triple().str()` | `HOST` |
| `MCPP_PROFILE` | 生效 profile 名(dev/release/…) | `PROFILE` |
| `MCPP_OUT_DIR` | `target/.build-mcpp/out/`(mcpp 预创建;`generated=` 推荐落这里) | `OUT_DIR` |
| `MCPP_MANIFEST_DIR` | 包根(= cwd,显式给一份) | `CARGO_MANIFEST_DIR` |
| `MCPP_FEATURE_<SANITIZED>` | `1`,每个活跃 feature 一个 | `CARGO_FEATURE_*` |
| `MCPP_FEATURES` | 活跃 feature 逗号列表(遍历友好) | — |

- **缓存正确性**:注入的契约值整体折入 cache key(`write_cache`/`cache_fresh`,`build_program.cppm:284-360` 增一行 `ctx <hash>` 记录)——target/profile/features 变了必须重跑,不能依赖用户自觉 `rerun-if-env-changed`;
- **时序修正**:build.mcpp 调用点(`prepare.cppm:965`)目前在 feature 激活(`:2419` 一带)**之前**——注入 features 要求把 root 的 feature 闭包计算前移或拆出(激活闭包本身无 IO,纯 `featuresMap` 图运算,可提前);这是 G3 里最需要小心的重排,单测锁"闭包提前算与原位算结果一致";
- **cross 下运行**:删掉 `:388-393` 的跳过;host 编译器改为**按 `host_triple()` 独立解析**的宿主默认工具链(不能用 cross 前端);`host_base_flags(tc)` 喂宿主 sysroot(既有函数,热机/CI 裸沙箱两态都过的那套);typed `import mcpp;` 模块(`kMcppModuleSource`)同样用宿主链编;
- typed 库(`build_program.cppm:201-214`)同步长出读取接口:`mcpp::target()`/`host()`/`profile()`/`out_dir()`/`has_feature("x")`(C 级原语实现,免 import std,协议 1:1 原则不变);
- e2e:cross(mingw-cross capability)下 build.mcpp 真跑 + `MCPP_TARGET` 断言;features 变更触发重跑断言。

#### 2.4.2 G2:依赖包 build.mcpp(第二阶段)

- **执行点**:per-dep prepare 流,`materialize_generated_files`(`prepare.cppm:1561`)之侧、该 dep glob 展开之前;dep 的 feature 集在 Stage 2a(`activateFeatures`,`prepare.cppm:1845-1860`)已可得;
- **作用域(照 Cargo,现成)**:对 dep 的 manifest 调 `apply()` 即正确——`cxxflag/cflag/cfg` 只进该 dep 自身 TU;`link-lib/link-search` 进 dep 的 `bc.ldflags`,沿既有 per-package ldflags 通道到终链;**只请求叶子边、不能加依赖**的既有纪律对 dep 同样成立(dep 图此刻已定);
- **产物目录项目本地化(关键)**:dep 根在 `~/.mcpp/registry` 共享、可能只读、**绝不可写**。dep 的 bin/cache/OUT_DIR 全部落 `<project>/target/.build-mcpp/deps/<name>@<ver>-<fp>/`;cwd 仍为 dep 根(读源用),`MCPP_OUT_DIR` 指项目本地;`generated=` 路径解析:相对路径**以 OUT_DIR 为基**(root 的 build.mcpp 保持相对包根不变,dep 语义文档写清),落进 `bc.sources` 的是绝对路径(既有通道支持,root 侧 `:371` 先例);
- **触发条件**:dep 根存在 `build.mcpp` 即运行(Cargo 模型,不搞 opt-in 开关);描述符可用 `min_mcpp = "0.0.95"` 声明依赖此能力;
- **信任声明**:changelog/docs 显式写明"构建依赖 = 执行其 build.mcpp"(与 build.rs 同模型);
- **缓存**:per-dep cache 文件在项目本地目录,key 含 dep 指纹 + 契约值,registry 共享包升级(指纹变)自动失效;
- e2e:双包 fixture(dep 带 build.mcpp 产 generated 源 + link-search),断言 flags 不泄漏到 root TU、link 指令达终链、registry 目录零写入(`find -newer` 断言)。

### 2.5 G9:路线图锚点(不排期)

缓存键 = 工具链 10 字段指纹 × per-source 内容哈希 × canonical flags × feature 集;位置 `~/.mcpp/cache/cas/`;命中单位是对象文件与 BMI。前置:hash-as-identity(L1b)、BMI 跨项目可重定位性核实。在 G1–G2 全落地后单独立设计文档。

---

## 3. 任务拆分 / PR / 版本号

**一个版本 0.0.95,三个 PR,合并顺序 A → B → C**。仓库既有的"一版本一 PR"惯例这里放宽为"一版本一批 PR":PR-A/PR-B 合入 main 但**不 bump 版本、不发布**(有先例:`ce34a70`/`70967c7` 等 main 上无版本提交;0.0.90 也是 #210+#213 两 PR 成一版);**只有 PR-C 携带版本 bump**(mcpp.toml + `fingerprint.cppm:21` 同提交),其合入即触发 0.0.95 发布链。squash 标题沿用 `feat(scope): 中文标题 (#PR)` / 末 PR 加 `(0.0.95)`(参照 `6d8083a`/`116f532`)。PR 号以开 PR 时实际为准(当前序列 ~#219 起)。

| PR | squash 标题草案 | 内容 | 规模 |
|---|---|---|---|
| **PR-A** | `feat(build): 汇编源一等公民 — .S/.s/.asm 进 sources,NASM 按目标推导 -f (#NNN)` | G1 全部(§2.1)+ G7 收口(`ensure_nasm`)+ G8a 软链 glob | 中:5 处路由 + 2 条 ninja 规则 + flags/指纹 + ensure_nasm |
| **PR-B** | `feat(manifest): 声明式清单能力 — features.sources / generated_files / cfg 条件 sources / per-glob flags (#NNN)` | G5 + G6 + G1b(§2.2)+ G4(§2.3)+ G8b(定位后);含 xpkg 侧 target_cfg/globFlags 与 toml↔xpkg 对称性单测 | 中:双解析器 + 一处合并点 + scanner 接线 + 指纹 |
| **PR-C** | `feat(build): build.mcpp 补全 — 环境契约 + cross 下运行 + 依赖包执行 (0.0.95) (#NNN)` | G3 + G2(§2.4,PR 内先契约后扩围两阶段)+ **版本 bump 0.0.95** + changelog(汇总 A/B/C 全部能力) | 大:env 注入 + 缓存 key + feature 闭包前移 + 宿主链解析 + dep 流插入 + 目录纪律 |

**PR 间依赖**:A 与 B 代码不相交、可并行开发(唯一接缝:A 的非 x86 `.asm` 报错文案引用 B 的 cfg-sources 语法,同版本发布无断层;B 的 `asmflags` per-glob 键消费 A 的 `unit_asmflags` 通道——B 后合即可,若 B 先行则该键留 TODO 随 A 激活)。C 独立于 A/B,但排最后合,因为它带版本 bump 且是回归面最大的一个:合 C 前 main 上已有 A/B 的全部新 e2e 作回归底座。

每个 PR 的 DoD(统一):

1. 单测过(`mcpp test`,tests/unit 自动发现,当前 35 文件基线);
2. e2e 全绿(`tests/e2e/run_all.sh`,本机基线 94 过/5 环境败;新 e2e 见 §4.3);
3. `docs/05-mcpp-toml.md` 同步(PR-B;PR-A/C 补 docs 对应章节);
4. 三平台 CI 过(linux/macos/windows + e2e 工作流)。

PR-C 额外 DoD:`mcpp.toml version` 与 `MCPP_VERSION` 同提交 bump 到 0.0.95(release.yml 冒烟断言二者一致,`release.yml:198/:537`);changelog 汇总三个 PR 的全部用户可见变化(含 G2 信任模型声明、默认 glob 扩展声明)。

---

## 4. 开发流程

### 4.1 单 PR 循环(A/B/C 各一轮)

1. `main` 拉 feature 分支(`feat/asm-sources`、`feat/declarative-manifest`、`feat/build-mcpp-completion`);A/B 可并行分支,C 待 A/B 合入后从 main 拉;
2. **TDD**:先写失败单测/e2e,再实现(superpowers:test-driven-development);G8b 这类未定位 bug 先写复现再修(systematic-debugging);
3. 本地验证:`mcpp build && mcpp test` 自托管 + `tests/e2e/run_all.sh`;**feature/源集类改动必须 build/test 双路径对比**(0.0.94 事故的直接教训);e2e 断言必须 host-aware(0.0.74 的教训:win/mac 自托管 e2e 跑 Linux 断言会挂);
4. docs 同步;仅 PR-C:版本 bump 两处同步(§3 PR-C DoD);
5. 开 PR(`gh pr create`),CI 全绿后 **squash 合入**,标题按 §3 草案;
6. PR-C 合入后走 §5 发布链(一次);发布验证闭环后 workspace bootstrap pin bump(`ci: workspace mcpp bootstrap pin -> 0.0.95 (released + indexed)` 惯例提交)。

### 4.2 单测清单(按 PR)

- **PR-A**:扩展名路由一致性锁(§2.1.1 末);`Triple::nasm_format` 全目标矩阵;`asm_object`/`nasm_object` 规则 emit(test_ninja_backend);`f.as`/`f.nasm` 组装(test_build_flags,含 nasm `-I` 尾分隔符);默认 glob 扩展(test_manifest);软链 glob(test_modgraph);
- **PR-B**:toml featureSources/generatedFiles/cfg-sources 解析;**toml↔xpkg 对称性测试**;根侧 generatedFiles materialize;globFlags 顺序语义/零命中告警/指纹折入;scanner 命中附加;
- **PR-C**:契约 env 注入与 cache key;feature 闭包前移等价性;cross 宿主链解析;dep 作用域(flags 不泄漏)/OUT_DIR 定位/registry 零写入。

### 4.3 e2e 新增(编号接现有 ~103;104–105 随 PR-A,106–109 随 PR-B,110–111 随 PR-C)

| 编号 | 名称 | requires | 断言要点 |
|---|---|---|---|
| 104 | `104_asm_sources_gas.sh` | gcc | `.S` 进构建、对象入链、增量(touch 后仅重编该 TU)、build/test 双路径 |
| 105 | `105_asm_sources_nasm.sh` | gcc **nasm** | `.asm` 编译、`-f` 推导(host 断言)、`%include` 增量、缺 nasm 硬失败文案 |
| 106 | `106_feature_gated_sources_toml.sh` | gcc | mcpp.toml featureSources 开/关 × build/test 四象限 |
| 107 | `107_generated_files_toml.sh` | gcc | 落盘、指纹变更触发重建、越根路径报错 |
| 108 | `108_cfg_conditional_sources.sh` | gcc | host-aware:按 host arch 断言源集差异 |
| 109 | `109_per_glob_flags.sh` | gcc | 命中 TU 得旗标(`-DXX` 探针)、未命中不受影响、顺序覆盖、零命中告警 |
| 110 | `110_build_mcpp_env_contract.sh` | gcc | 契约变量值、features 改变触发重跑、cross(`# requires: mingw-cross` 变体)下真跑 |
| 111 | `111_dep_build_mcpp.sh` | gcc | §2.4.2 的三断言(作用域/终链/registry 零写入) |

`run_all.sh` capability 自动探测(`:41-112`)增 `nasm`(`command -v nasm`,装了 xlings nasm 的 CI runner 应可得;探不到则 105 skip 不 fail)。CI e2e runner 预装 nasm 进 workflow(linux 必装,mac/win 有 xlings 包则装)。

### 4.4 mcpp-index 侧联动验证

**发布前预验证(不等 release)**:PR-A 合入 main 后即可用 main 自托管构建的 mcpp 二进制在 mcpp-index 本地跑 nasm POC(最小 `.asm` 包或 F1+ ffmpeg 汇编档雏形)——把描述符/推导问题在发版前暴露,这是三 PR 合一版相对多版最大的红利:发布时生态用例已实跑过。

**0.0.95 发布后一次性解锁**(全部 `min_mcpp = "0.0.95"`):

- nasm POC 包正式进 index,CI `mcpp test --workspace` 锁;F1+ ffmpeg 汇编加速档开工;
- 把现有靠 xpkg featureSources 的包(compat.gtest/eigen/spdlog/cjson)形态在 mcpp.toml 侧各写一个对称 fixture 进 mcpp-index tests;OpenCV imgcodecs 形态(feature=源集+define)开工;
- OpenCV SIMD dispatch(P5)解阻塞;
- 评估把复杂 install() Lua 逻辑迁移为包内 build.mcpp 的机会(长期让 index 描述符更薄)。

---

## 5. 发布与 xlings 生态打通(PR-C 合入后一次)

既有自动化管线(release.yml + publish-ecosystem)已覆盖,流程照抄,坑位提示来自 0.0.91–0.0.94 实操:

1. **tag 触发 release.yml**:四平台 job(linux musl 静态 / aarch64 cross / macos / windows)自托管构建 + pack + 冒烟(冒烟断言 `--version` == mcpp.toml,DoD-3 的保险);`mcpp publish --dry-run` 产源码 tarball + xpkg.lua;
2. **publish-ecosystem job**(全平台 needs):
   - `mirror_res.sh mcpp <ver>` → xlings-res 双端(github + gitcode);**已知坑**:gtc 大文件致 30min timeout 复发 → 本地 `gtc` 补传大件,`gitcode.com` 真 GET + `cmp` 核验,然后 rerun --failed(幂等续传);本机跑 gtc 需 `/usr/bin/python3`;两端**永不删除在服资产**;
   - `bump_index.sh mcpp <ver>` → 对 openxlings/xim-pkgindex 开 bump PR;**索引 sha256 必须独立核验**(下载资产本地算,不信 CI 日志);合入后注意 refresh marker 假新鲜问题(必要时手动 `xlings update`);
3. **fresh-install 验证**:`ci-fresh-install`(workflow_run 自动触发)+ aarch64 变体;0.0.95 需额外人工验证一次 `xlings install nasm` 在**双镜像**(github/gitcode)都可达(G7 供给侧收口的最后一环);
4. **bootstrap pin bump**:`ci: workspace mcpp bootstrap pin -> 0.0.95 (released + indexed)` 提交进 main;
5. mcpp-index 联动项(§4.4"发布后"清单)随后开工。

合并节奏建议:A → B 之间不必等待;**C 合入前**至少完成一轮 §4.4 的发布前预验证(main 二进制真跑 nasm POC),因为 C 一合就是发布——三层新机制的问题要在 main 上各自消化,不要堆到 release 冒烟才暴露。

---

## 6. 风险与非目标

**风险**

| 风险 | 缓解 |
|---|---|
| 默认 glob 扩展把从未编译的 `.asm` 意外拉进构建 | changelog 显式声明;MASM 报错带排除指引;`!` glob 逃生门 |
| G3 的 feature 闭包前移引入激活语义回归 | 等价性单测 + 0.0.94 双路径 e2e 全量回归 |
| G2 打开依赖任意代码执行面 | 文档显式声明信任模型(= build.rs);registry 零写入 e2e 锁 |
| nasm 在 CI 三平台可得性不齐 | capability 探测 + skip;linux CI 必装;硬失败文案指引 xlings |
| xpkg 新键(target_cfg/globFlags)被旧 mcpp 消费 | 描述符 `min_mcpp` floor(index_contract 既有机制) |

**非目标(本轮明确不做)**

- MASM(`ml64`)支持——`.asm` 恒路由 NASM;
- `asmflags` 进 `[build]` 顶层(per-glob 已覆盖需求,顶层键等真实需求出现再加);
- build.mcpp 产 `.o` 直接入链的协议(G1 已消解该需求);
- G9 实现(单独设计文档)。
