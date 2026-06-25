# gtest_main 冲突修复:src 轨 feature 控制 + dev 轨 main 检测 + `mcpp add --dev`

> 关联 issue: [#168](https://github.com/mcpp-community/mcpp/issues/168)
> （`mcpp add gtest` → `[dependencies]` → `mcpp build` 报 `LNK2005: main already defined`）
>
> 前序:[2026-06-25-dependency-archive-linking-design.md](2026-06-25-dependency-archive-linking-design.md)
> （0.0.64 的 **dev 轨** 实现 + 为何放弃静态归档）
>
> 跨仓库:**mcpp**(核心)+ **mcpp-index**(`compat.gtest.lua`)。
>
> **状态:设计定稿。dev 轨已实现(0.0.64);src 轨 + `mcpp add --dev` 待实现。**

## 1. 问题与根因

`gtest` 把入口拆成两块:框架 `gtest-all.cc` 与**可选入口** `gtest_main.cc`(后者自带
`int main(){ InitGoogleTest; RUN_ALL_TESTS; }`)。冲突是普适的:**任何"自带 main 的
二进制"一旦链入 `gtest_main.o` 就 `duplicate symbol: main`**。

#168 触发路径:`mcpp add gtest` 写进 **`[dependencies]`**(常规依赖,`commands.cppm:82`)
→ `mcpp build` 把 gtest 全部对象(含 `gtest_main.o`)内联进应用 `hello.exe` → 与
`src/main.cpp` 的 `main` 撞车(Windows `LNK2005`)。

## 2. 最终设计:按依赖所在表分两轨

| 轨 | 场景 | gtest_main 策略 | 机制 | 状态 |
|---|---|---|---|---|
| **A. src / 常规依赖**(`[dependencies]` + `mcpp build`)| 应用/库代码 | 默认**不含**;`features=["main"]` 才含 | **feature 控制(声明式,不猜)** | **待做** |
| **B. dev 依赖**(`[dev-dependencies]` + `mcpp test`)| 测试 | 测试**自带 main → 不链**;否则链 | **判 `int main` 检测**(`source_defines_main`,限 dev-dep 作用域)| **已实现 0.0.64** |

两轨正交互补:src 轨干净声明式、默认安全(#168 默认消失);dev 轨维持 `mcpp test`
"一个 gtest 依赖 + 带/不带 main 测试混用"全自动跑通。

### 2.1 为什么 src 轨用 feature「不猜」、dev 轨用「检测」

- **src 轨不能猜**:常规依赖可能是任意大型 C 库;文本级 `main` 检测**看不懂预处理器
  守卫**——libarchive 的 `archive_blake2s_ref.c` 里 `#if defined(BLAKE2S_SELFTEST)
  int main(void)` 是死代码,但文本扫描会误判 → 误排除 → `undefined reference`。
  (0.0.64 正因此把检测**限定到 dev-dep**。)所以 src 轨必须**声明式 feature**,零误判。
- **dev 轨可以检测**:它只扫**用户自己的测试文件**(普通代码,无预处理器守卫 main,
  可靠),并让 `mcpp test` 对"写/不写 main"都无感;且**已上线验证**。

### 2.2 为什么现有 feature/target 机制不够,src 轨需要新增一环

mcpp 现有 features 能力边界(实证):
- `featuresMap`:feature → **隐含 feature**(`manifest.cppm:245`);
- `required_features`:**target 级**门控 —— `std::erase_if(m->targets, …)`
  (`prepare.cppm:2180`),只决定"是否发射某个 link unit",且作用于**根包** targets;
- 依赖级 feature **选择**(`dep_spec.features`)。

**缺的那一环**:依赖的 **sources 是包级全局**(`BuildConfig.sources`,`Target` 无
`sources` 字段),依赖链接是**按包名内联全部实现对象**(`plan.cppm` 消费者内联循环),
**不经过 target**。所以:
> 即便给 gtest 写 `gtest_main` target + `required_features=["main"]`,删 target ≠ 它
> 名下的源不编译——源在"源→CompileUnit→按包名内联"这条**不经 target** 的路上,照样
> 被编译并内联。`required_features` 门控的是 **link unit**,门不住 **依赖源**。

→ src 轨 feature 控制需要把作用点从「删 target」**下沉到「挑 CompileUnit / 挑内联
对象」**:即 **「已激活 feature ↔ 依赖源的纳入/排除」**。这是真正的新增点,也是旧
mcpp 没有、需考虑兼容的根源。

## 3. src 轨实现

### 3.1 mcpp-index(`compat.gtest.lua`)
```lua
mcpp = {
    language     = "c++23",
    sources      = { "*/googletest/src/gtest-all.cc" },     -- 默认仅框架
    include_dirs = { "*/googletest/include", "*/googletest" },
    targets      = { ["gtest"] = { kind = "lib" } },
    -- 新增:feature 门控的源组。激活 "main" 时才纳入 gtest_main.cc。
    features = {
        ["main"] = { sources = { "*/googletest/src/gtest_main.cc" } },
    },
}
```
- 默认 `gtest = "1.15.2"` → 不含 gtest_main → 任何二进制都不撞 main → **#168 默认消失**;
- `gtest = { version="1.15.2", features=["main"] }` → 纳入 gtest_main(进阶 opt-in)。

### 3.2 mcpp 核心:依赖的「feature 门控源组」——是现有 feature 系统**沿源轴扩一格**

定位:**同一套 feature,不是并行机制。** mcpp 现有 feature 系统:
- 数据 `featuresMap : map<string, vector<string>>`,值=**隐含的别的 feature 名**;
- 作用:① feature → 隐含 feature;② `required_features` 门控 **target(link unit)**;
- 激活:根 `--features`/`[features].default`,依赖 `dep={features=[...]}`(`prepare.cppm:2083`)。

本提案**复用同一 feature 名 + 同一激活机制**,只新增**第三种作用**:
- ③ feature → **纳入哪些源(CompileUnit)** —— 现有 feature 的"值"是 feature 名列表,
  没有"源";所以 feature 声明的 schema 要从"值=feature 名列表"扩成"值=可带 `sources`
  的表"。作用维度从 **target 级**下沉到 **源级**(正因依赖按包内联、源不归 target,
  target 级门控够不到源)。

且当前 `synthesize_from_xpkg_lua`(合成依赖 manifest)**根本没读 `mcpp.features`**
(只读 sources/targets/deps 等)。故本提案两件事:
1. 让 synthesize **开始解析** 依赖描述符的 `mcpp.features.<name>.sources`;
2. 合成时把**已激活 feature**对应的源组**并入** `buildConfig.sources`;**被某 feature
   列出的源**默认**排除**(不并入 base 构建),仅该 feature 激活时纳入。

- 复用现有依赖级 feature 激活集合(`prepare.cppm:2083`)。
- **不改 dev 轨、不改链接器、不引入 gtest 特例**——任何库都能用此通用能力。

## 4. `mcpp add --dev`(补齐缺失能力)
现状:`cmd_add`(`cli.cppm:255`)只写 `[dependencies]`,**无 `--dev`**。
- 新增 `mcpp add --dev <pkg>` → 写入 `[dev-dependencies]`(含命名空间子表,与现有
  `[dependencies.<ns>]` 对称);`mcpp remove` 对称。
- 引导:测试框架(如 gtest)**建议**放 dev(走 dev 轨,体验最佳)。**非强制**——因为
  src 轨已让常规依赖 gtest 默认安全。

## 5. 各组合最终行为(验收矩阵)

| 场景 | gtest 位置 | 写 main? | 预期 |
|---|---|---|---|
| 应用 `mcpp build`(#168）| `[dependencies]` 默认 | 是 | 只框架,**不撞 main** ✓ |
| 应用本身用 gtest、要框架 main | `[dependencies]` `features=["main"]` | 否 | 链 gtest_main,跑通 ✓ |
| 测试 `mcpp test`,只写 TEST 宏 | `[dev-dependencies]` | 否 | 检测无 main → 链 gtest_main ✓(0.0.64)|
| 测试 `mcpp test`,自带 main | `[dev-dependencies]` | 是 | 检测有 main → 不链 gtest_main ✓(0.0.64)|
| 常规库(libarchive)blake2 守卫 main | `[dependencies]` | (死码)| src 轨不猜、dev 检测不扫常规库 → 不误判 ✓ |

## 6. 兼容与发布顺序(实现前须定)

`compat.gtest` 现含 gtest_main;src 轨改成"默认不含"会影响**旧 mcpp**:
- 旧 mcpp 不认 `features.*.sources`;若描述符把 gtest_main.cc 从基础 sources 移走,
  旧 mcpp 下 gtest 将**永远无 main**(连 dev 轨无-main 测试也缺入口)。
- 候选过渡:
  1. **先发认识"feature 门控源组"的新 mcpp,再切描述符**(旧版本仍在野,但 0.0.x
     快速迭代、bootstrap pin 驱动升级,可接受);
  2. 描述符**版本化/兼容写法**,旧 mcpp 维持现状(含 main)、新 mcpp 读新字段;
  3. 确认旧 mcpp 对未知键**非 --strict 下是 warning 非 error**(已见 schema warning
     机制,`manifest.cppm:268`),据此选最小破坏方案。
- **实现前敲定** 1/2/3。dev 轨(0.0.64)不依赖描述符改动,始终工作。

## 7. 实施计划(有序 + 可并行)

前置:**dev 轨已完成(0.0.64)**,本轮不动。

依赖关系小结:
- **W1 `mcpp add --dev`** 与 **W2 feature→源核心**:**两者完全独立**(都在 mcpp,互不
  依赖)→ **可并行**(两个 PR / 两个 agent)。
- **W3 `compat.gtest.lua`**(mcpp-index):语义依赖 W2,但因兼容①(gtest_main.cc 同时
  在 `sources` 和 `features.main.sources`)→ **旧 mcpp 不回归**,故描述符**可提前/并行
  编写与合入**;只是"#168 真正被修好"要等 W2 的 mcpp 发布并被使用。
- **W4 端到端 e2e**(#168 哨兵)需 W2 + W3 都在。
- 各自的**单测随代码 TDD**(与 W1/W2 并行)。

### Phase 0 — 并行实现两个独立 mcpp 核心改动
- [ ] **W1 `mcpp add --dev`**(`cli.cppm` 加旗标 + `commands.cppm` 写 `[dev-dependencies]`,
  命名空间子表对称;`mcpp remove` 对称)+ 单测。**独立 PR,可随时合/发。**
- [ ] **W2 feature→源核心**(`manifest.cppm` 的 `synthesize_from_xpkg_lua` 解析
  `mcpp.features.*.sources`;合成时按已激活 feature 并入、未激活的"feature 列出源"
  默认排除)+ 单测。**独立 PR。**
- *(W1 ∥ W2)*

### Phase 1 — 描述符(可与 Phase 0 并行编写;合入安全)
- [ ] **W3 `compat.gtest.lua`**(mcpp-index):按兼容①——`gtest_main.cc` 同时列在
  `sources` 与 `features.main.sources`。旧 mcpp 只认 sources(照旧含 main、不回归),
  新 mcpp(W2)把它默认排除、`features=["main"]` 时纳入。

### Phase 2 — 端到端验证(需 W2 + W3)
- [ ] **W4 e2e**:`mcpp add gtest && mcpp build`(应用自带 main)→ 成功(#168 哨兵);
  `gtest={features=["main"]}` → 链 gtest_main 成功;`mcpp add --dev gtest && mcpp test`
  带/不带 main 两类全绿(dev 轨已有 e2e 78 覆盖)。

### Phase 3 — 发布闭环(顺序敏感)
- [ ] **W5**:mcpp bump + release + 镜像 xlings-res(gh/gtc)+ xim-pkgindex + 索引发布;
  **mcpp-index 改了 → 重发 mcpp-index 索引产物**。
  - 顺序:**先发含 W1+W2 的 mcpp**,再合/发 W3 描述符(兼容①下即便顺序反了也不回归,
    但先 mcpp 能让 #168 立刻被修)。

> 关键路径:**W2 → W3 → W4 → W5**;**W1 全程旁路可并行**。

## 8. 决策备注

1. **两轨并存是刻意的**:src 要干净声明式(feature,不猜),dev 维持现状(检测,已实现、
   省事)。src 不猜是因为常规库的预处理器守卫 main 会让文本检测误判(blake2 实锤)。
2. **feature 是整库/整源组粒度**,不在链接器层写 gtest 特判;gtest 只是在描述符里
   填了 `main` feature 的数据 → 未来任何测试框架同样适配,mcpp 零框架知识。
3. **默认不含 main**:依赖不应劫持消费者入口;"含 main"会把 #168 设成默认崩溃。
4. **放弃过的路**:静态归档(Windows/MSVC `LNK1561`/`LNK2019` 不可行,见前序文档);
   src 轨"扫所有依赖判 main"(blake2 误判)。
