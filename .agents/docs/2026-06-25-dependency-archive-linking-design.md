# 依赖入口对象的条件链接（fix `mcpp test` duplicate `main`）设计方案

> mcpp 0.0.63 → 0.0.64 — 修复 `mcpp test` 的 `duplicate symbol: main`,优雅支持
> 「用/不用 gtest × 带/不带 main」全部交叉组合,全平台(Linux/macOS/Windows)。
>
> 关联：[2026-06-25-cdb-test-coverage-design.md](2026-06-25-cdb-test-coverage-design.md)
>（同一轮 test 体验修复链的第三环）。
>
> **状态：已实现并全平台 CI 通过。** 实施进度见 §8。
>
> **重要演进**:最初设计为「依赖 `kind="lib"` → 静态归档 `.a` 按需链接」(标题旧名),
> 但该方案在 **Windows/MSVC lld-link 上两处致命**(见 §3.5 / §9),最终改为**保持依赖
> 内联、仅条件性排除依赖自身的 main 对象**——等效、最小爆炸半径、全平台可行。

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
| ❌ mcpp 里给 gtest 加特判（跳过 gtest_main.o） | 把第三方库名硬编进构建核心,污染架构。否决。 |
| ❌ 依赖 `kind="lib"` → 静态归档 `.a` 按需链接(最初采用) | 理念干净,但 **Windows/MSVC lld-link 两处致命**(§3.5);**否决**。 |
| ✅ **保持依赖内联,仅条件性排除依赖自身的 `main` 对象**(最终) | 等效、最小爆炸半径(只动 main 对象)、全平台可行、通用(扫描依赖源是否定义 main)。**采用。** |

### 3.5 为何放弃静态归档(Windows/MSVC 两处致命)
最初实现「依赖 lib → `.a` 归档 → 按需链接」,Linux/macOS/aarch64 全绿,但 Windows CI
连续失败,逐层揭开:
1. **`LNK1561: entry point must be defined`** —— Windows 上 mcpp 走 **MSVC 模式
   (lld-link)**,它**不会仅为确定入口点而从归档惰性拉取成员**。不带 main 的测试
   (用 gtest TEST 宏 + gtest_main)拿不到 `gtest_main.o` 的 `main` → 失败。
   (`--start-lib/--end-lib` 又不被 Mach-O lld 支持,不能统一替代。)
2. **`LNK2019: unresolved external __imp_lzma_*`(构建 xlings)** —— 把**常规** lib
   依赖(libarchive)也归档后,MSVC 链接器对「归档→另一归档(lzma)」的**传递符号
   解析顺序**处理不同 → 一片未解析外部符号。

两者证明:**静态归档在 MSVC 上不可行**(既不能供入口,又破坏传递链接)。故回退到内联。

## 4. 核心设计：依赖入口对象的条件链接

**保持所有依赖对象内联(沿用既有链接模型,xlings/libarchive/lzma 等逐字节不变),
仅对「依赖自身定义 `main` 的对象」(如 gtest 的 `gtest_main.o`)做条件处理:**

| 消费者自带 main? | 依赖的 main 对象(gtest_main.o)| 其余依赖对象 | 结果 |
|---|---|---|---|
| 是 | **排除**(不链接) | 内联 | 入口=消费者自己;无 `duplicate main` ✓ |
| 否 | **内联**(直接链接,提供入口) | 内联 | 入口=gtest_main;全平台(含 MSVC)OK ✓ |

- 「依赖的 main 对象」= 扫描每个**依赖**(非根包、非 shared)实现源,
  `source_defines_main` 为真者(gtest_main.cc 有 main;gtest-all.cc / libarchive /
  lzma 没有)。**一次性预扫描**存入 `depEntryMainSources`,消费者循环 O(1) 查表。
- 「消费者自带 main」= `source_defines_main(entryMain)`(对测试即测试文件本身)。
- **直接链接对象**(非归档)→ 不依赖任何链接器的归档拉取语义 → **Linux/macOS/Windows
  一致**。
- 仅排除「依赖的 main 对象」→ 其余链接**与改动前完全一致**,零回归(尤其 xlings)。

### 4.1 `source_defines_main` 的健壮性(关键)
判据是「源是否定义 `int main(`/`auto main(`」,**必须先剥离注释 + 字符串 + 字符 +
raw-string 字面量再匹配**——否则测试夹具里的 `"int main(){...}"` 字符串会假阳性。
`test_modgraph.cpp` 正是此坑:它在双引号字符串里嵌了 `int main()`,逐行启发式误判它
「自带 main」→ 早期归档版把 no-main 测试错配 → MSVC LNK1561。现用字符状态机剥离后
再匹配,导出 + 8 个单测守卫(`test_main_detection.cpp`)。

### 4.2 通用性 / 面向未来
不识别「哪个依赖、哪个对象」是 gtest_main——只看「依赖对象是否自带 main」+「消费者
是否自带 main」。任何未来测试框架(mcpplibs 生态 / mcpp 原生)其 main-提供对象都被
同样处理,mcpp **零框架知识、零特例**。描述符层(mcpp-index gtest)**无需改动**。

### 4.3 实现位置(`plan.cppm`,`make_plan`)
- 预扫描:`depEntryMainSources` = 所有依赖(非根、非 shared)实现源中
  `source_defines_main` 为真者。
- 每个消费者:`entryDefinesMain = source_defines_main(entryMain)`。
- 内联循环新增一行:`if (entryDefinesMain && depEntryMainSources.contains(cu.source)) continue;`
  ——自带 main 的消费者跳过依赖的 main 对象;其余一切照旧。
- `source_defines_main` 导出供单测。**无新 LinkUnit、无 ninja 改动**(归档相关代码
  及 `archiveInputs` 字段已全部移除)→ 后端/链接行与改动前一致。

## 5. 不破坏既有行为(回归边界)
- 仅当「消费者自带 main」**且**「某依赖对象自身定义 main」时,该对象被排除——这是
  唯一的行为变化点。
- 所有其余链接(纯模块依赖、shared 依赖、根包、常规 C/C++ lib 如 libarchive/lzma、
  无 main 的测试)**逐字节不变** → xlings 等复杂工程零回归(这正是放弃归档换来的)。

## 6. 边界用例
- **无 main 且无框架的测试**:无入口 → 链接器报 undefined `main`(真实用户错误);
  `mcpp test` 已透出诊断(本轮新增,见 cdb 修复链)。
- **自带 main 且不用 gtest(但 gtest 是 dev-dep)**:依赖对象不被引用 → 链接器本就不
  纳入(内联对象未引用即不产生符号需求);gtest_main 对象被显式排除 → 无冲突。
- **多个依赖各自提供 main**:自带 main 消费者全部排除;无 main 消费者会拉多个 main →
  duplicate(罕见,清晰报错)。

## 7. 验证策略(TDD)
- **单元** `test_main_detection.cpp`:`source_defines_main` 对真实 main / 带参 main /
  `auto main` 判真;对字符串字面量、raw-string、注释里的 `int main()` 判假;对
  `mainHelper` 判假。(8 例)
- **e2e** `78_test_main_combinations.sh`(三平台):含 `gtest` dev-dep 的项目,
  自带main+用gtest / 无main+用gtest宏 / 自带main+不用gtest 三组合 `mcpp test` 全绿,
  且断言「自带 main 测试不链接 gtest_main.o、无 main 测试链接之」。
- **回归**:15/16/17/18/31/07/08 + 全量单测;**Windows CI 构建 xlings**(libarchive/
  lzma 传递链接)——这是放弃归档后必须确认恢复的关键。

## 8. 实施计划

- [x] **P1 plan 模型**:`source_defines_main`(剥离注释/字符串/raw-string)+ 导出;
  `depEntryMainSources` 预扫描;消费者按「自带 main」排除依赖 main 对象。
  (归档方案 staticDep/archiveInputs 已回退移除。)
- [x] **P2 后端**:无改动(回退归档发射);`mcpp test` 透出 `diagnosticOutput`(可见性)。
- [x] **P3 单元测试**:`MainDetection`(8 例)+ 全量 25 单测绿。
- [x] **P4 e2e**:`78_test_main_combinations.sh` 三组合全绿 + 链接行断言。
- [x] **P5 回归**:25 单测 + e2e 全套绿;**全平台 CI 全绿**(Linux/Windows/aarch64/
  macOS/e2e)——含 Windows 构建 xlings(libarchive/lzma)、Windows e2e 78 三组合。
  e2e 78 断言历经 `.exe` 后缀 + 反斜杠路径两次跨平台适配。
- [x] **P6 版本 + 文档**:bump 0.0.63→0.0.64;CHANGELOG;本文件。
- [x] **历程**:归档 → MSVC LNK1561/LNK2019(§3.5)→ 回退内联+条件排除。
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
