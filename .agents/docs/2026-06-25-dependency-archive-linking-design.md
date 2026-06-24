# 依赖库归档链接（dependency `lib` → static archive）设计方案

> mcpp 0.0.63 → 0.0.64 — 修复 `mcpp test` 的 `duplicate symbol: main`,并把
> 「依赖库」从「散对象内联」升级为「静态归档按需链接」。
>
> 关联：[2026-06-25-cdb-test-coverage-design.md](2026-06-25-cdb-test-coverage-design.md)
>（同一轮 test 体验修复链的第三环）。
>
> **状态：设计定稿，进入实施。** 实施进度见 §8（动态更新）。

## 1. 问题

含 `[dev-dependencies] gtest` 的项目，若测试文件自带 `int main()`（脚手架默认模板
就是），`mcpp test` 链接失败：

```
ld.lld: error: duplicate symbol: main
>>> obj/test_smoke.o:(main)         ← 测试自己的 main()
>>> obj/gtest_main.o:(.text+0x0)    ← gtest_main.cc 自带的 main()
```

测试二进制同时链接了测试自身的 `main` 与 gtest 的 `gtest_main.o`（也含 `main`）。

需优雅支持的**交叉组合**（用户尽量无感）：

| 用 gtest? | 自带 main? | 期望 |
|---|---|---|
| 否 | 是 | 链接测试自己的 main（今天 fresh 项目即此，正常）|
| 是 | 是（调用 `InitGoogleTest`/`RUN_ALL_TESTS`）| 用测试的 main，**不要** gtest_main |
| 是 | 否（只写 `TEST(...)`）| 用 gtest_main 提供的 main |
| 否 | 否 | 无入口——**清晰报错**，非崩溃 |

## 2. 根因

`compat.gtest` 描述符（`mcpplibs/mcpp-index/pkgs/c/compat.gtest.lua`）**已声明**：

```lua
mcpp = {
    sources = { "*/googletest/src/gtest-all.cc", "*/googletest/src/gtest_main.cc" },
    targets = { ["gtest"] = { kind = "lib" } },   -- 已建模为「库」
}
```

mcpp 的 manifest 解析器也**已支持** `kind="lib"` → `Target::Library`
（`manifest.cppm:637`）。**但 mcpp 的依赖链接模型没有兑现它**：

- `plan.cppm` 的 link-unit 循环只为**根包**的 `[targets.*]` 建 LinkUnit；
- 依赖包（含 dev-dep）的对象通过 `append_package_objects` / compileUnit 循环
  **直接内联**进消费者二进制（`plan.cppm:486–575`）；
- 于是 `gtest-all.o`、`gtest_main.o` 作为**散对象**被无条件灌入测试二进制 →
  `gtest_main.o` 的 `main` 与测试自身 `main` 冲突。

**这不是 gtest 的缺陷,也不是描述符的缺陷,而是 mcpp 核心 link 模型的缺口。**

## 3. 架构决策：修复归属（回答「改 mcpp-index 还是 mcpp」）

| 候选 | 评价 |
|---|---|
| ❌ mcpp 里给 gtest 加特判（跳过 gtest_main.o） | 把第三方库名硬编进构建核心,污染架构,未来每个框架都要再特判。否决。 |
| ❌ 改 gtest 描述符拆出 `gtest_main` 单独目标 | 描述符**已**声明 `kind="lib"`,够用;拆分增加每个库包的作者负担,且仍需 mcpp 支持「按归档链接」。非必要。 |
| ✅ **mcpp 核心:兑现依赖的 `kind="lib"` → 建静态归档 `.a` → 按归档链接** | 通用、由**既有**描述符元数据驱动、零 gtest 特例、未来框架自动适配。**采用。** |

**职责分层(干净):**
- **描述符(mcpp-index)**:声明「我是什么」——`kind="lib"`。gtest 已声明,**不改**。
- **mcpp 核心**:实现「怎么链」——依赖的 lib 包 → 归档 → 按归档链接。**通用 HOW。**

**未来演进**:mcpplibs 生态测试框架 / mcpp 原生测试框架,只要在各自描述符里声明
`kind="lib"`(并可选地提供一个含 `main` 的入口对象),就**自动**获得「带 main 用
自己的、不带 main 用框架的」正确行为,mcpp 无需任何该框架的知识。

## 4. 核心设计：依赖库 → 静态归档 → 条件链接

### 4.1 原理 + 跨链接器现实(归档 / 内联 **混合**)

理想模型:静态归档 `.a` 的成员对象**仅在其符号当前未定义时**才被拉入,于是单个
`libgtest.a = { gtest-all.o, gtest_main.o }` 似乎能同时处理「带/不带 main」。
**ELF(ld.lld)与 Mach-O(ld64)确实如此**:不带 main 的测试,`main` 被 CRT 引用
为未定义 → 链接器从归档拉 `gtest_main.o` 当入口。

**但 Windows 上 mcpp 走 MSVC 模式(clang-cl + lld-link)**,实测:
```
LINK : fatal error LNK1561: entry point must be defined
```
**MSVC lld-link 不会仅为「确定入口点」而从归档惰性拉取成员** → 不带 main 的测试拿不到
`gtest_main.o` → LNK1561。而 `--start-lib/--end-lib` 又不被 Mach-O lld 支持,
不能作为统一替代。

**结论:按「消费者是否自带 main」选择链接方式(per-consumer,全平台正确):**

| 测试自带 main? | 依赖链接方式 | 理由 |
|---|---|---|
| 是 | **归档** `lib<pkg>.a`(排在对象后) | 链接器不拉 `gtest_main.o`(main 已定义)→ 无 `duplicate main`;入口由测试提供,MSVC 也 OK |
| 否 | **直接内联**依赖的非模块对象 | `gtest_main.o` 作为普通对象直接提供入口 → **任何**链接器(含 MSVC)都 OK;测试无 main 故无冲突 |

判据 = **扫描消费者入口源是否定义 `int main`/`auto main`**(空白不敏感、跳过注释行;
启发式,最坏只是选错链接方式而非出错;探测不到时默认按「无 main」内联=改动前行为)。
**通用**:无需识别「哪个依赖对象提供 main」,只看消费者自己——对任何 `kind="lib"`
依赖、任何未来测试框架都成立。

### 4.2 BuildPlan 变更（`plan.cppm`）
对每个 `kind="lib"` 的**依赖**包(非根包),由其**非模块**实现对象(`.cc/.cpp/.c`)
合成一个 `StaticLibrary` LinkUnit → 产 `lib<pkg>.a`:

- 新增 LinkUnit:`{ kind=StaticLibrary, output="bin/<prefix><mangled-pkg><static_lib_ext>",
  objects=<dep 的非模块对象> }`。**命名平台感知**(`libfoo.a` / `foo.lib`,复用
  `platform::lib_prefix`+`static_lib_ext`,镜像 `target_output`)——硬编码 `.a`
  会断 Windows。
- 消费者(Binary/TestBinary/SharedLibrary)按 §4.1 表二选一:
  - **自带 main** → 不内联该 dep 的非模块对象,把 `.a` 路径追加到**链接命令、对象
    之后**(经 `$in`,既上命令行又被 ninja 依赖追踪);
  - **不带 main** → 直接内联该 dep 的非模块对象(走原内联路径)。
- **模块对象(`.cppm` 的 `.o`)永远直接内联**:它们承载模块全局初始化、且从不提供
  `main`,放进归档可能因「无未定义符号引用」被丢弃 → 破坏全局初始化。故模块对象
  不归档(mcpplibs.cmdline 等纯模块依赖 → 无非模块对象 → 无归档 → **行为不变**)。

### 4.3 链接顺序与 ninja（`ninja_backend.cppm`）
- 当前链接行:`build <out> : cxx_link <objects> | <implicitInputs>`。`implicitInputs`
  在 `|` 之后是 ninja 的**order-only/隐式依赖,不进命令行**。归档要真正参与链接,
  其路径必须进**命令行、且在对象之后**。
- 方案:LinkUnit 新增 `std::vector<std::filesystem::path> archiveInputs`;ninja 发射
  时把它们拼在 `objects`(及 std.o)**之后**、作为命令行实参,同时也加入 `| implicit`
  做重建追踪。

### 4.4 复用既有基建
mcpp 已有 `cxx_archive` 规则(`ninja_backend.cppm:378`)、`archive_tool`(ar/llvm-ar,
`flags.cppm:253`)、`StaticLibrary` LinkUnit。本设计复用之,无需新工具。

## 5. 不破坏既有行为(回归边界)
- **纯模块依赖**(mcpplibs.cmdline,`.cppm`)→ 无非模块对象 → 不生成归档 →
  链接与今天**逐字节一致**。
- **根包自身**对象 → 不归档(它就是要被链接的主体)。
- **shared 依赖**(SharedLibrary)→ 走既有 `append_direct_shared_deps`,不变。
- 仅「依赖包含有非模块实现对象且 `kind=lib`」(gtest、未来 C/C++ 库)行为改变:
  由内联改为归档链接——这正是修复点。

## 6. 边界用例
- **无 main 且无框架的测试**:链接报 undefined `main`。mcpp 捕获 lld/ld 的该错误,
  转成可读提示:`test '<name>' 无入口:请写 int main(),或依赖一个提供 main 的测试
  框架(如 gtest,不写 main 时由 gtest_main 提供)`。(P2,可后续增强;P0 至少不崩。)
- **多个 lib 依赖**:每个一个 `.a`,按依赖顺序排在对象之后;若库间有相互依赖,
  保持拓扑序(已有 `directPackageDeps` 拓扑信息可复用)。
- **静态库目标的根包**(`mcpp build` 产 `.a`)→ 不受影响(那是根包 target,非依赖)。

## 7. 验证策略(TDD)
- **单元**(`tests/unit/test_ninja_backend.cpp` / 新增):给定一个 `kind=lib` 依赖 +
  含非模块对象的 plan,断言:(a)生成 `StaticLibrary` LinkUnit;(b)消费者链接行含
  该 `.a` 且**位于对象之后**;(c)纯模块依赖**不**生成归档。
- **e2e**(新增 `78_test_main_combinations.sh`,三平台):一个含 `gtest` dev-dep 的
  项目,覆盖四种交叉组合各一个 `tests/*.cpp`,断言 `mcpp test` 全绿:
  1. 自带 main + 用 gtest;2. 不带 main + 用 gtest(TEST 宏);3. 自带 main + 不用
  gtest;4.(可选)无 main 无框架 → 期望清晰错误。
- **回归**:既有 15/16/17(test pass/fail/no-tests)、18(devdeps isolation)、
  31(transitive deps)、07/08(static/shared lib)必须仍绿。

## 8. 实施计划(动态更新)

- [x] **P1 plan 模型**:`LinkUnit` 加 `archiveInputs`;为 `kind=lib` 依赖合成
  `StaticLibrary` LinkUnit(`lib<pkg>.a`);消费者改为引用 `.a` 而非内联其非模块对象。
  (`plan.cppm`:staticDep 检测 + 消费者排除内联 + `archiveInputs` 注入)
- [x] **P2 ninja 发射**:链接行把 `archiveInputs` 经 `$in` 排在对象之后(既上命令行又
  被 ninja 依赖追踪)。(`ninja_backend.cppm`)
- [x] **P3 单元测试**:`NinjaBackend.ArchiveInputsLinkedAfterObjects`(归档在对象后);
  全量 24 单测绿。
- [x] **P4 e2e**:`78_test_main_combinations.sh` 四组合 `mcpp test` 全绿 + 断言生成
  `cxx_archive`。本机验证:3 passed;`build libcompat_gtest.a : cxx_archive
  obj/gtest_main.o obj/gtest-all.o`,测试链接行 `…objects… libcompat_gtest.a`。
- [x] **P5 回归**:24 单测 + e2e 15/16/17/18/31/07/08 全绿。
- [x] **P6 版本 + 文档**:bump 0.0.63→0.0.64;CHANGELOG;本文件勾选。
- [ ] **P7 发布闭环**:PR → CI 全平台 → squash --admin 合入 → tag v0.0.64 →
  release → 镜像 xlings-res(gh+gtc,4 平台)→ xim-pkgindex mcpp.lua bump(PR)→
  索引产物自动发布 → `xlings install mcpp@0.0.64` 验证 → bootstrap pin bump。
  （mcpp-index/gtest 描述符**不改** → 无需重发 mcpp-index。）

## 9. 决策备注
1. **由既有元数据驱动,而非新增特例**:`kind="lib"` 早在描述符里;本设计只是让
   mcpp 兑现它。符合「约定优于配置 / 用户无感」。
2. **单归档 + 链接器语义** 已覆盖全部 main 交叉组合,无需拆 `gtest_main`、无需让
   用户选链接哪个目标——最大化「无感」。
3. **模块对象不归档** 是关键安全边界:避免全局初始化被归档式丢弃,且让纯模块依赖
   零回归。
4. **面向未来框架**:任何测试框架在其描述符声明 `kind="lib"` 即自动获得正确入口
   语义;mcpp 永不需要认识具体框架。
