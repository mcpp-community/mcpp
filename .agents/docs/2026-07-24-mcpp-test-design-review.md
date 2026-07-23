# mcpp test 架构评估与设计方案

- 日期：2026-07-24
- 分支：`feat/test-isolation-json`（评估对象 = 本分支 8 个提交 + mcpp test 既有形态）
- 关联：d2mcpp「练习即测试」重设计（d2mcpp 仓 `.agents/docs/2026-07-23-exercises-as-tests-design.md`）
- 本文定位：**设计评审记录 + 后续演进方案**。先从 mcpp 自身架构评估本批次改动与
  `mcpp test`/gtest 的定位关系，再评估对 d2mcpp 的适配性，最后给出路线图。

---

## 1. 结论摘要

1. `mcpp test` 的正确定位是**二进制运行器（runner of binaries）**：发现/构建/运行
   `tests/**/*.cpp`（每文件一个独立二进制）并聚合退出码。它对二进制内部使用什么
   断言框架**零感知**——gtest 是依赖层（dev-dependencies）的方案，与运行器正交。
   本批次改动全部落在运行器层，未破坏该分层。
2. 六项改动中五项与 mcpp 架构自洽且为通用能力；**逐测试隔离的当前实现存在一个
   实质代价：Phase B 跨测试构建串行化**（全绿路径最明显），有明确的改进方案（P1，
   keep-going 预构建 + 存在性归因），语义不变、并行度恢复。
3. gtest 与 mcpp test 不是竞争关系而是两层：文件级过滤（`mcpp test <pattern>`）与
   用例级过滤（`-- --gtest_filter=...` 透传）今天已可组合。不建议把 gtest 感知
   内建进运行器；若要用例级一等公民，正确路径是**可选的测试协议**（P5，v2 提案）。
4. 对 d2mcpp：练习=文件=二进制的模型是**必然选择**（用例级框架无法表达"尚未编译
   通过"这一教学核心状态）；判定三来源分层映射良好；遗留一处发现逻辑重复
   （Provider 目录扫描 vs 运行器 glob），可由 P2（`mcpp test --list`）收敛。

---

## 2. 分层模型：运行器与框架的正交性

```
┌────────────────────────────────────────────────────────┐
│ runner 层(mcpp test)                                    │
│   发现 tests/**/*.cpp → 每文件合成 TestBinary target     │
│   构建(dev-deps 生效) → 运行 → 退出码聚合 → 人读/JSON    │
├────────────────────────────────────────────────────────┤
│ in-binary 层(框架自选,mcpp 零感知)                       │
│   裸 main / gtest(+gtest_main) / catch2 / d2x 练习库 …   │
└────────────────────────────────────────────────────────┘
```

既有事实支撑这一分层已是 mcpp 的实际设计：

- gtest 以 `[dev-dependencies] gtest = "1.15.2"` 进入，`plan.cppm` 的
  dependency-provided-entry 启发（gtest_main 提供 `main`）解决链接问题——框架
  支持发生在**依赖解析层**，不在运行器；
- `mcpp test -- args` 把参数透传给**每个**测试二进制，`-- --gtest_filter=X`
  即用例级过滤，无需运行器懂 gtest；
- 判定协议只有一条：**进程退出码**。gtest 聚合其内部用例到退出码，裸 main 直接
  返回，d2x 练习库经 atexit 收口——三者对运行器等价。

### 与同类工具对照

| 维度 | mcpp test | cargo test | ctest | gtest(单跑) |
|---|---|---|---|---|
| 测试单元 | 文件=二进制 | 集成测试同（tests/*.rs=crate）；单测经 libtest 协议到用例级 | 注册的命令 | 用例（in-binary） |
| 编译失败语义 | **该测试 fail，其余照跑**（本批次） | 整体构建失败（不隔离） | 构建在外部，坏目标不阻塞其他命令 | n/a |
| 过滤 | 文件名子串（本批次） | 用例名子串 | -R 正则 | --gtest_filter |
| 机器可读 | NDJSON（本批次） | --message-format json | --output-json | --gtest_output=json |
| 判定 | 退出码 | libtest 协议/退出码 | 退出码/正则 | 内部统计 |

结论：mcpp test 的文件级模型与 ctest 同族、比 cargo 的集成测试**多出编译隔离**
（cargo 的 tests/*.rs 一个编译失败同样拖死整轮）。用例级是 cargo 单测/gtest 的
领地，mcpp 通过透传组合而非内建。

---

## 3. 本批次改动逐项评估

### 3.1 tests 相对路径命名（4b09984）✓

- 动机：子目录布局下 stem 撞名硬错误；名字应与文件系统布局同构。
- 一致性：合成 target 专属规则，平铺布局名字不变，无兼容破坏。
- **已知豁免需要记录**：合成测试名可含 `/`（`00-a/0`），而 `[targets.*]` 用户
  target 名文法不允许——两套命名空间事实上分离（合成 target 不进 manifest、不参与
  publish/xpkg）。属可接受的不一致，但应在 docs 里写明，防止未来有人"统一"它。

### 3.2 `BuildOptions::ninjaTargets`（bce3e7d）✓

- 后端接口的最小扩展（显式 goal 集合，空=全量），`producedArtifacts` 同步收窄。
- 通用性无争议；风险点是调用方误传非 output 路径 → ninja "unknown target" 报错，
  错误信息可读，可接受。

### 3.3 逐测试编译隔离（ed557df + Phase A 目标集修正）——语义 ✓，实现有代价

语义评估：
- 包级失败（lib/deps/共享模块）≠ 测试失败的区分是**正确且必要**的：52 个红测试
  误导用户"你全错了"，而真相是基础设施坏了。Phase A 以"全部非测试入口的编译单元 +
  非测试 link 产物"为 goal 集合，覆盖了 test 模式下 lib target 被跳过、共享破坏
  藏在模块对象里的边角（e2e 119 第二场景验证）。
- 归因精确：每测试独立 ninja goal，诊断天然按测试分组，扫描期错误（dyndep）也
  落在对应测试上。

实现代价（本文最重要的自我批评）：
- **Phase B 对每个测试串行调用 `backend->build()`**。旧实现一次 ninja 调用并行
  编译全部测试 TU；现在 N 个测试 = N 次 ninja 进程 + N 次 build.ninja/
  compile_commands 重写 + N 次（缓存命中的）hermetic 检查，且**测试之间的编译
  不再并行**。全绿路径（CI、正常项目）代价最大；d2mcpp 的 49 题答案全量验证
  实测 ~29s（含运行），可用但不优。
- 运行阶段本就串行（与旧行为一致），不算回归，但同样是并行机会。

**改进方案（P1，语义不变）**：三段式
1. Phase A 不变；
2. Phase B-1：**单次** ninja 调用携带全部（过滤后）测试 goal + `-k 0`
   （keep going）——恢复全并行；
3. Phase B-2：逐测试按**产物存在性**判定编译成败；对失败者**逐个**重跑其 goal
   （必然快速失败）以取得干净的每测试诊断，随后照常运行成功者。
   失败是少数路径，逐个重试的代价与收益成正比。

### 3.4 过滤器（16da216）✓

- 过滤发生在**构建/运行阶段而非计划阶段**是关键决策：计划恒含全部测试，
  `compile_commands.json` 保持完整（clangd 依赖）。正确。
- 子串语义与 cargo 对齐；`…/1` 匹配 `…/10` 的邻居效应已在下游（d2mcpp Provider
  逐行精确匹配）消化。可选增强：`--exact`。

### 3.5 `--message-format json`（f2c7830）✓，schema 需小幅演进

- 流式逐测试记录 + 包级错误记录 + summary，stdout 纯协议流：与 `-q`/`ui::set_quiet`
  的既有静音机制正交组合，实现干净。
- 缺口：per-test `duration_ms`（进度类前端需要）；无 `--list`（见 P2）；
  `signal` 从退出码反推（128+n 启发）而非 wait status 直读，Windows 语义空缺。

### 3.6 输出交错与横幅（fix 提交）✓

`Compiling → 结果 → 该测试诊断`的连续块 + 删除 Phase A 后误导性的
`Finished test` 横幅——cargo 形态，修复了"成功横幅紧邻全红"的自相矛盾。

### 3.7 平台/清单层修复 ✓

- `merged_environ` 剥离 `xim-x-glibc` loader 条目：只针对私有 payload、用户条目
  保留、显式 override 优先——外科式，配合 musl 静态发行从两端封死嵌套段错误。
- `[build].flags` glob 覆盖测试 TU + 死 glob 警告改查磁盘：glob 指名文件，
  是否被扫描为 source 与之正交；警告语义从"没命中扫描源"修正为"磁盘上无此文件"，
  是普适改进而非特例。

---

## 4. mcpp test 与 gtest：定位与演进

### 4.1 现状判断

- 运行器不感知框架是**优点**：gtest/catch2/doctest/裸 main/d2x 库在退出码协议下
  等价，mcpp 无需 per-framework 适配矩阵。
- `docs/00-getting-started.md` 的 "(gtest style)" 措辞**具有误导性**——它描述的是
  "常配合 gtest 使用"，读起来却像"运行器实现了 gtest 语义"。应改为
  "one binary per file; bring your own framework (gtest via dev-dependencies)"。

### 4.2 不建议：运行器内建 gtest 感知

内建意味着：解析 gtest 输出/JSON、维护版本兼容、并对 catch2/doctest 重复同样工作。
分层被打穿，收益仅是省去一次 `--` 透传。否决。

### 4.3 建议（P5，v2 提案）：可选的用例级测试协议

参照 cargo/libtest 的"运行器↔测试二进制"契约，定义 mcpp 自己的**可选**协议：

```
运行器设 MCPP_TEST_PROTOCOL=1 后：
  <bin> --mcpp-list           → NDJSON: {"case":"Math.Add"} …
  <bin> --mcpp-run <case>     → 运行单用例,退出码判定
  （或单进程模式：<bin> --mcpp-json → 逐用例 NDJSON 结果流）
不支持协议的二进制（裸 main）：探测失败即回退到整二进制模式——零破坏。
```

- gtest 侧一个 ~50 行 adapter（translate 到 `--gtest_list_tests`/`--gtest_filter`/
  `--gtest_output`）即可接入；d2x 练习库天然可实现。
- 运行器获得：用例级过滤/并行/JSON，而仍不认识任何具体框架。
- 明确 **YAGNI 边界**：在出现第二个真实需求方之前不实现，本文仅锁定方向，防止
  未来用"内建 gtest"这类打穿分层的方案填这个空。

---

## 5. 对 d2mcpp 的适配性评估

1. **文件=二进制模型是教学的必然选择**：练习的核心状态是"尚未编译通过"，用例级
   框架（gtest 等）以"可编译"为前提，无法表达这一状态；编译错误本身是教学主通道
   （`D2X_YOUR_ANSWER` 指着要填的位置）。文件级隔离恰好把"一个未完成练习"约束为
   "一个红测试"。
2. **判定三来源分层映射良好**：mcpp JSON（编译/退出码事实）× d2x 库侧信道
   （断言语义/路障）× Provider（合并成协议 verdict）——运行器不需要为 d2mcpp
   增加任何专有语义，验证了 §2 的分层。
3. **发现逻辑存在重复**：Provider 自行扫描 `src/*/tests/` 推导 id/order，运行器
   另有一份 `tests/**` glob。两者约定同构但真相源有二。P2 的
   `mcpp test --list --message-format json`（输出测试名+主文件路径）可让 Provider
   的枚举改为消费运行器输出，回到单一真相源。
4. **性能画像**：checker 单题路径（filter + 缓存）最优（~0.2s 级）；e2e 全量
   路径承受 §3.3 的串行代价（49 题 ~29s），P1 落地后预期显著下降。
5. **已消化的风险**：子串过滤邻居效应（Provider 精确匹配）；`signal` 启发式
   （练习判定不依赖 signal 字段）。

---

## 6. 路线图（按优先级）

| # | 事项 | 层 | 验收标准 |
|---|---|---|---|
| P1 | Phase B 改 keep-going 预构建 + 存在性归因 + 失败者逐个取诊断 | runner | e2e 152–157 不变全绿；多测试全绿工程 `mcpp test` 构建墙钟时间恢复到与旧单次构建同量级 |
| P2 | `mcpp test --list [--message-format json]` | runner | 输出测试名+主文件；d2mcpp Provider 枚举可切换为消费该输出 |
| P3 | JSON 记录增加 `duration_ms`；`signal` 改从 wait status 直读（POSIX） | runner | schema 向后兼容（只增字段） |
| P4 | 每测试超时 `--timeout <s>`（挂死测试 → run_fail + 标注） | runner | 睡死测试不拖垮整轮 |
| P5 | 用例级测试协议（§4.3） | 协议 | 出现第二个需求方后再立项 |
| P6 | docs 措辞修正："(gtest style)" → framework-agnostic 表述；记录 §3.1 命名豁免 | docs | — |

## 7. 风险与未决

- **Windows 全链路未实测**：`--quiet`（sandbox ninja 补丁旗标）、路径分隔、
  `_pclose` 退出码语义——发版前需要一轮 Windows CI。
- P1 的存在性归因前提是"goal 失败 ⇒ 产物不存在"：ninja 对失败目标不落盘产物，
  但**残留的旧产物**会造成误判——实现时须以"本轮 mtime 是否推进/restat 状态"或
  预删除产物为判据，不能只看 exists()。
- 协议字段一旦被 d2mcpp 之外的消费者依赖，schema 演进需按"只增不改"纪律执行。
