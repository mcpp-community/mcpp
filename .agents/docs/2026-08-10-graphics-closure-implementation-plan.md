# 实施计划:图形栈闭合与分发档位

> 设计:`2026-08-10-graphics-closure-and-distribution-tiers-design.md`
> 分支:`feat/graphics-closure-and-distribution-tiers`
> 单 PR、一档全发。基线 `main` `3f237ed`(`2026.8.10.1`)。

---

## 0. 模块划分

三条**协议**各自独占一个 `.cppm`,因为它们每一条都有**两个以上的读写方**,
而"同一决策两处推导"是这个仓库反复付过学费的形状。

| 新模块 | 协议内容 | 谁写 | 谁读 |
|---|---|---|---|
| `src/build/graph_shape.cppm`<br>`mcpp.build.graph_shape` | `build.ninja` 首行 `# mcpp:graph=<shape>` | `ninja_backend` | `execute`(两条快路径) |
| `src/build/loader_contract.cppm`<br>`mcpp.build.loader_contract` | 加载器标签契约:哪种产物要哪个标签、flag 怎么拼、判决怎么表达 | `plan`(链接期)、`pack`(patchelf 期) | `runtime_validation`(rule E) |
| `src/pack/host_requirements.cppm`<br>`mcpp.pack.host_requirements` | `HOST-REQUIREMENTS` 文本格式 + 到 xpkg `[runtime].requirements` 的投影 | `pack` | `publish`(J)、一致性检查 |

平台特化一律留在 `src/platform/`,不外溢:

| 平台模块 | 本轮改动 |
|---|---|
| `src/platform/elf_runtime.cppm` | 新增 `SearchPathTag`(`None`/`Rpath`/`Runpath`/`Both`)与 `searchPathTag` 字段。**今天两个标签被合并进 `runpaths` 就丢了**,rule E 要的正是被丢掉的那一位 |
| `src/platform/runtime_env_contract.cppm` | 补一条:DT_RUNPATH 只对**携带它的对象自己发起**的 `dlopen` 生效;第三方对象代为 `dlopen` 时不生效 |

`loader_contract` **不认识 ELF** —— 它表达"可执行文件要 RPATH、库要 RUNPATH"这条策略,
读标签的动作委托给 `platform::elf`。这样 macOS/Windows 只是"契约不适用",而不是散落 `#ifdef`。

**判断"是不是可执行文件"统一用 `PT_INTERP` 是否存在**(`facts.interp` 非空),
不用 `ET_EXEC` —— PIE 可执行文件是 `ET_DYN`,和共享库同型。xlings 的 `_has_pt_interp` 同判据。

---

## 1. 步骤

每步给:改哪、测什么、**怎么先证伪**。**每一步的测试必须先看到红。**

### 步 1 — A:pin + 版本(无行为变更,先落地,让后面每一步都在新 pin 上验)

- `src/xlings.cppm:47` `kXlingsVersion` → `"2026.8.10.4"`
- `src/version.cppm:34` `MCPP_VERSION` → 下一个补丁号
- `mcpp.toml` 的 `version` 同步(第一组两个文件,同一个 commit)
- **不动** `.xlings.json` 的 bootstrap pin
- 验:`.github/tools/check_version_pins.sh` 绿

### 步 2 — 平台层:把标签这一位保住

`src/platform/elf_runtime.cppm`:

```cpp
enum class SearchPathTag { None, Rpath, Runpath, Both };
// ElfRuntimeFacts 新增:
SearchPathTag searchPathTag = SearchPathTag::None;
```

在已有的 `kDtRpath` / `kDtRunpath` 分支里各置一位;`runpaths` 的既有语义
(两者都在时以 RUNPATH 为准)**不变** —— 那是加载器物理,现有调用方依赖它。

- 单测 `tests/unit/test_elf_runtime.cpp`:三个 fixture(仅 RPATH / 仅 RUNPATH / 两者)
  断言 `searchPathTag` 与 `runpaths` 各自正确。
- **证伪**:两者都在时若只读第一个命中,`Both` 会退化成 `Rpath` —— fixture 必须能抓到。

### 步 3 — B:#405

`src/build/ninja_backend.cppm:1053–1083`,`staged` 非空且 `has_std_artifacts` 时,
把 `std_bmi_dst`(以及 `has_std_compat` 时的 `compat_bmi_dst`)一并放进聚合。

- e2e `tests/e2e/212_cached_dep_std_is_ordered.sh`
  - 依赖选 `mcpplibs.cmdline@0.0.2`(其模块 `import std`),消费方**刻意不写 `import std`**
  - 清该包 pkg 缓存 → 项目 A(miss)必须 OK → 项目 B(hit)必须也 OK
  - **图断言**:`_mcpp_staged_cache : phony` 那一行必须含 `std.gcm`
  - ⚠️ **全程不删任何产物**
- **证伪**:撤掉 push_back,B 必须红

### 步 4 — C:#407

1. `mcpp.build.graph_shape`:`enum class GraphShape { Normal, WithTests }`、
   `header_line(shape)`、`read_shape(ninjaPath)`。**读写同一处拼写。**
2. `BuildPlan` 加 `graphShape`;`prepare_build` 按 `includeDevDeps || !extraTargets.empty()` 置位
3. `emit_ninja_string` 首行写 header
4. `try_fast_build` / `try_fast_run`:`read_shape() != Normal` ⇒ 当 miss
5. 删 `configure.cppm:101` 的调用与 `execute.cppm:288` 的 `forget_build_cache_entry`

- e2e `213_build_after_test_is_not_the_test_graph.sh`:
  `build → test → build`,第三次之后 `bin/<target>` 必须存在且是**这次**产的;
  把 `tests/*.cpp` 写坏后 `mcpp build` 必须仍绿。**不删产物。**
- 既有 `211_configure_only_cdb.sh` 必须保持绿(它断言同一条可观察行为)
- **顺序**:先加 header 检查 → 跑 211 绿 → 再删 `forget_build_cache_entry`。两步不合并。
- **证伪**:去掉 header 检查,213 必须红

### 步 5 — D:DT_RPATH 契约

`mcpp.build.loader_contract`:

```cpp
enum class ArtifactForm { Executable, SharedLibrary, StaticArchive, NotElf };
enum class RequiredTag  { Rpath, Runpath, NotApplicable };
RequiredTag required_tag(ArtifactForm);              // 策略,平台无关
std::optional<std::string> link_flag(RequiredTag);   // "-Wl,--disable-new-dtags"
struct TagVerdict { enum class Status { Ok, Violation, NotApplicable }; … };
TagVerdict check(const mcpp::platform::elf::ElfRuntimeFacts&, ArtifactForm);
```

- **链接期**:`plan.cppm` 按 link unit 的 kind 追加 `link_flag(...)`,
  与 `shared_library_link_flags`(`:391`)同层;**必须排在所有 `--enable-new-dtags` 之后**
- **校验期**:`runtime_validation.cppm` 加 rule E,**warn-first**
- e2e `214_executable_carries_dt_rpath.sh`:
  - gcc 与 clang 各建 bin + shared lib
  - **测试自身先断言默认值是 RUNPATH**(不带 flag 编一个),否则将来链接器默认一变它静默空转
  - bin 必须 `Rpath`,lib 必须 `Runpath`
- **证伪**:去掉 flag,bin 那条必须红

### 步 6 — E:dev 档身份一致

`src/build/runtime_validation.cppm`(或近邻)实现原生求解:

```
dispatch(libGLX.so.0)→ DT_RPATH → vendor 目录 → 解符号链接 → 真实载荷路径
                                                → 与声明 provider 身份比对
```

- 三值 `OK` / `MISMATCH` / `NOT_DECLARED`,由 `mcpp why runtime` 呈现
- `.wiring` **只做增强**:存在则附加显示 `state`,并标注按本产物标签重新求值;
  缺失 / 未知 `state` ⇒ `unverified`;未知 key ⇒ 忽略
- 单测:构造 fixture 目录树(符号链接指向 `0.1.1`、声明 `0.1.2`)⇒ 必须 `MISMATCH`
- **证伪**:把符号链接改成指向 `0.1.2` ⇒ 必须 `OK`;删掉 `.wiring` ⇒ **主判据不变**

### 步 7 — F:pack 档

- **F1** `pack.cppm:350` `set_runpath`:可执行文件加 `--force-rpath`(走 `loader_contract`)
- **F2** 对 `toBundle` 里每个 ELF 同样重写为 `$ORIGIN`(库保持 RUNPATH)
- **F3/F4** `mcpp.pack.host_requirements`:从 plan 的 `RuntimeRequirement` 生成清单
- e2e `215_pack_has_no_build_machine_paths.sh`:
  遍历 bundle 内**每个** ELF,原生解析,**不得出现 xlings store 前缀**;
  可执行 `Rpath`、库 `Runpath`;有能力需求时 `HOST-REQUIREMENTS` 非空
- **证伪**:去掉 F2,必须红

### 步 8 — G:自带 libc 的档拒绝能力型需求

- `pack`/`plan` 期:`static` 与 `self-contained` + 存在 `phase=="run"` 的能力型
  `RuntimeRequirement` ⇒ 失败,错误里说**哪一条能力 / 为什么不相容 / 改用 `vendored`**
- e2e `216_selfcontained_refuses_host_capability.sh`:两档都必须红
- **证伪**:换 `--mode vendored` ⇒ 两条都绿

### 步 9 — J:xpkg-app 投影

- `publish` 的 `[runtime].requirements` 由 `host_requirements` **同一个函数**产出
- 一致性检查(单测):同一 plan 下两种投影逐条相等
- **证伪**:改掉一处 ⇒ 检查红

### 步 10 — 文档

- `docs/` 里 pack / publish / runtime 三处随改动更新
- 设计文档标记实施状态
- `CHANGELOG.md`

### 步 11 — 本地验证

`mcpp build` → `mcpp test`(unit)→ e2e 全量。记录真实输出,不概括。

### 步 12–15 — PR / CI / 合入 / 发版 / index / 生态验收

按 `.agents/skills/mcpp-release/SKILL.md`。

---

## 2. 顺序

```
步1(pin/版本)
  └→ 步2(平台层标签位)──→ 步5(D)──→ 步7(F1/F2)
  └→ 步3(B #405)                     └→ 步8(G)
  └→ 步4(C #407)                     └→ 步9(J)
                        步6(E)独立(只需步2)
```

步 3 / 步 4 与其余互不相干,可先落地先验证。

---

## 3. 风险与守则

- **每个 e2e 先红后绿**,红的输出要贴进 PR 描述。
- **`212` / `213` 全程不删产物** —— 删了未修的二进制也会绿。
- **`214` 先断言默认标签是 RUNPATH** —— 否则将来默认一变它静默空转。
- **步 4 的两小步不合并** —— 先加检查、211 绿,再删旧机制。
- **rule E warn-first** —— 结论只来自一台 NVIDIA/X11/x86_64 机器。
- 跨仓阻塞项:xlings E2b 若无声明式退出即落地(设计 §1.3.1),**与本 PR 无耦合**,单独跟。
