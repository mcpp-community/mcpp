# c++fly:一键启用"最新标准 + 全部实验特性"(语言 + 标准库)设计

> 日期:2026-07-14 · 基线:ce34a70(0.0.90 已含 #210 方言旗标全图化)
> 输入:用户需求——"mcpp.toml 里指定/enable 一项即可启用 C++26 反射等实验特性,跨编译器";
> review 反馈(2026-07-14)——"收敛到:必然当前编译器最新 C++ 标准 + 可开启的所有实验特性,核心是语言特性 + 标准库"
> 前置文档:`2026-07-13-post-089-roadmap-and-std-dialect-flags-design.md`(§1.3/§1.5)、
> `2026-07-13-single-pr-090-implementation-plan.md`(S1)、`2026-06-01-cpp-standard-first-class-design.md`
> 本文是 post-089 路线图 §1.5 预留位的续篇:"`reflection = true` 这类糖等用户需求明确再加"——需求现已明确。
> review 定案(2026-07-14):值命名 = **`c++fly`**(§11-Q1);index 收录策略 = **v1 warn 观察**(§11-Q2);GCC16 契约旗标已实机定案(§11-Q3)。

## 0. 一句话结论

**新增一个版本无关的 `standard` 值 `c++fly`(命名已定案,§11-Q1):**

```toml
[package]
standard = "c++fly"   # 一键:工具链最新 -std= 档位 + 它能开的全部实验特性门
```

语义 = ①resolved 工具链支持的**最新 `-std=` 档位**(GCC16→`-std=c++26`,Clang→`-std=c++2c`,
MSVC→`/std:c++latest`)②该工具链支持的**全部实验性语言特性门**(GCC16→`-freflection` 等)
③**标准库实验门**(libc++→`-fexperimental-library`;libstdc++/MSVC STL 无需)。
判定由工具链层新数据表 `cppfly.cppm` 按〈编译器族 × 版本 × stdlib〉给出,产物汇入
0.0.90 已建成的图全局方言旗标通道——**机制零新管线,新增的只是"意图 → 档位+旗标"这层数据**。
不支持的特性**软跳过**并打印 summary(该值的本义就是"这套工具链能玩的都给我")。

用户体验对比:

```toml
# 0.0.90 现状:自己查档位、自己查旗标拼写,换 Clang/MSVC 即坏
[package]
standard = "c++26"
[build]
dialect_cxxflags = ["-freflection"]

# 本设计:一行,意图完整,跨编译器
[package]
standard = "c++fly"
```

## 1. 现状盘点:0.0.90 已有什么、还缺什么

### 1.1 已有(#210 落地件,全部可复用)

| 件 | 位置 | 作用 |
|---|---|---|
| known-list `is_dialect_flag()` | `src/manifest/types.cppm:453-467` | `-freflection/-fcontracts/-fchar8_t/-D_GLIBCXX_USE_CXX11_ABI=` 识别 |
| 逃生舱 `[build] dialect_cxxflags` | `src/manifest/toml.cppm:648` | 用户显式声明图全局旗标 |
| 合并去重 `dialect_flags(bc)` | `src/manifest/types.cppm:469-479` | 显式 ∪ 自动提升 |
| 图全局注入 | `plan.cppm:328-331` → `flags.cppm:304-310` | 进全局 `$cxxflags`(全图 TU) |
| scan/std-BMI 同步 | `prepare.cppm:2590-2607`(`stdFlagAndDialect`) | 扫描与 std/std.compat 预构建同方言 |
| 指纹自动失效 | std-module.json 哈希完整命令串 | 旗标变更 → BMI 重建,零新缓存逻辑 |
| e2e | `98_reflection_import_std.sh` | gcc16 + `-freflection` + import std 回归 |
| standard 归一化 | `types.cppm:481-539`(`normalize_cpp_standard`) | c++23/26/2c/gnu++/latest 白名单,level 数值化 |
| 各家 `-std=` 拼写 | `dialect.cppm:116-123`(`std_flag_for`) | GNU `-std=<canonical>` / MSVC `/std:c++latest` |

### 1.2 缺口(本设计要补的四件事)

1. **无"最新档位"概念**:`c++latest` 是静态映射(canonical 固定,GNU 侧落到
   `-std=c++26`),不随 resolved 工具链取其真正的最新档;更没有"档位 + 实验门"的捆绑。
2. **无能力/版本校验**:代码里没有任何 `version >= N` 门控(`provider.cppm` 的
   `capabilities_for()` 只按 CompilerId+triple,未用 `Toolchain::version`);`gcc@15` +
   `-freflection` 只能等编译器报未知选项。
3. **无一键全开**:每个特性一个旗标,用户要自己攒清单、自己跟进编译器演进。
4. **拼写族问题悬空**:`dialect_cxxflags` 存 GNU 拼写;MSVC 原生后端(0.0.90 S2-S6)
   落地后同一 manifest 在 cl.exe 上是非法选项。

### 1.3 概念模型:两个轴,一个捆绑值

- `standard` 决定 `-std=` 档位(23/26/latest),是**语言基线**;
- 实验特性是**基线之上的附加门**:同一 `-std=c++26` 下,GCC 16 的反射仍需 `-freflection`
  (libstdc++ `<meta>` 整体被 `__cpp_impl_reflection` 守卫,该宏仅由此旗标定义——
  post-089 文档 §1.1)。
- 初版设计曾把两轴拆开暴露(`standard` + 具名 `std_features` 列表);review 反馈明确
  **场景是"前沿尝鲜"整体模式**——要的是"必然最新档 + 全部能开的门"这一个捆绑值,
  细粒度选择不是当前需求(§4-C 记录取舍,§10 保留延伸位)。

## 2. 外部事实:编译器实验特性支持矩阵(2026-07 快照)

| 特性(提案) | GCC 16.1 | Clang 上游 22/23 | Bloomberg clang-p2996 | MSVC v145 (VS2026 18.6) |
|---|---|---|---|---|
| 最新 `-std=` 档位 | `-std=c++26` | `-std=c++2c` | `-std=c++26` | 仅 `/std:c++latest`(无 `/std:c++26`) |
| 反射 P2996+P3394 | ✅ 需 **`-freflection`**(实验) | ❌ 无 | ✅ `-freflection`/`-freflection-latest`(含 P1306/P3096/P3394/P3491) | ❌ 无,无公开 ETA |
| 契约 P2900R14 | ✅ 实验(**随 `-std=c++26` 默认启用,无需附加旗标**;`-fcontracts` 仅用于在更早标准下强开;evaluation-semantic 由 `-fcontract-evaluation-semantic=` 控制。已实机定案,§11-Q3) | ❌ 无 | — | ❌ 无 |
| 展开语句 P1306 | ✅ 随 `-std=c++26` | Clang 23 partial | 经 `-freflection-latest` | ❌ |
| 标准库实验门 | 无需(libstdc++ 不设门) | libc++ 需 **`-fexperimental-library`** | 同 | 无需(MSVC STL 随 `/std:c++latest`) |

要点:
- 反射已于 2025 Sofia 会议进入 C++26;GCC 16.1(2026-04 发布)是**唯一上游落地**,
  且仍标注 experimental;Clang 上游没有,只有 Bloomberg fork;MSVC 没有。
- 即 **v1 语言特性侧的现实收益面 = GCC ≥ 16**(mcpp 生态已有 gcc16 系工具链包,winlibs
  MinGW 亦然);Clang 侧 v1 收益 = 最新档 + `-fexperimental-library`;Clang/MSVC 缺失的
  特性由 summary 如实呈现,而非编译器原始报错。
- 该矩阵是**易变数据**——这正是它必须住在 mcpp 工具链层数据表里、而不是让每个用户
  manifest 各自硬编码旗标的根本原因(Clang 落地 P2996 后,mcpp 发版加一行,全生态受益)。

来源:gcc.gnu.org/projects/cxx-status.html、gcc-16/changes.html、clang.llvm.org/cxx_status.html、
github.com/bloomberg/clang-p2996、learn.microsoft.com(/std 文档、VS2026 release notes)、
libcxx docs(UsingLibcxx)。

## 3. 先例调研

| 系统 | 机制 | 对本设计的启示 |
|---|---|---|
| MSVC | `/std:c++latest` = "下一标准草案的全部已实现特性 + 部分实验特性"一键聚合 | 与本设计同构的**厂商级先例**:一个档位值捆绑"最新 + 实验" |
| libc++ | `-fexperimental-library` 一个旗标聚合全部不稳定库件 | 库侧聚合开关成立;mcpp 把它并进同一个值 |
| Rust nightly | `#![feature(x)]` 具名注册表 + stable 拒绝 | 具名粒度模型,对应 §10 的延伸位;其"注册表 + 工具链门控"内核被本设计沿用 |
| CMake `import std` | `CMAKE_EXPERIMENTAL_CXX_IMPORT_STD` = 按版本变化的 UUID,升级即失效 | 反面参照:故意摩擦表达"无稳定性承诺"。mcpp 用 summary 警示而非 UUID 摩擦(§5.4) |
| xmake/meson/bazel | 只有 `-std=` 档位设定,无实验特性抽象 | 空白区,mcpp 的差异化点 |

## 4. 方案空间与取舍

### 方案 A:钉版本的伪标准值——`standard = "c++26-fly"`(最初提案)

三个结构性问题:**钉死 26** → 每个标准期一个新值,gnu++26-fly/c++latest-fly 组合爆炸;
GCC 17 若默认启用反射,"26-fly" 对它失义;canonical 白名单每档 ×2。
判定:否决**钉版本**的拼法;其"一键"诉求由方案 D 的版本无关值承接。

### 方案 B:布尔总开关——`[build] experimental = true`

"全部"本就是随工具链漂移的移动目标,这一点方案 D 同样存在(见 §5.5 如何面对);
B 真正的问题是**放错了地方**:它与 `standard` 分居两表、且允许 `standard = "c++23"` +
`experimental = true` 这种自相矛盾的组合(实验特性几乎全部要求最新档)。review 反馈
"必然当前编译器最新 C++ 标准"指明二者不该独立配置。判定:否决独立布尔键。

### 方案 C:具名特性列表——`std_features = ["reflection", ...]`(初版推荐)

粒度最细、显式具名可做硬错诊断,Rust nightly 同构。review 反馈指出其错位:名字
"体现不出是开最新支持的",且细粒度选择不是当前需求——用户场景是**整体尝鲜模式**,
不是逐特性挑选。判定:**不作为 v1 表面**;其内核(注册表 + 版本门控)全部保留为
方案 D 的实现机制;若日后出现"库硬依赖单一特性"的需求,再把列表作为高级表面加回
(§10),届时零机制返工。

### 方案 D(采纳):版本无关的捆绑档位值——`standard = "c++fly"`

一个值 = 最新档 + 全部实验门(语言 + 标准库)。关键设计判断:

1. **版本无关命名**消解了 A 的全部问题:不钉 26,永远指"当前工具链的最前沿",
   特性毕业(变默认)后该值语义自动收窄到"剩余的实验门",名字永不失义;
   白名单只加一个值。
2. **放在 `standard` 键上**消解了 B 的问题:与档位天然捆绑,不存在矛盾组合;
   用户"enable 一个项"的诉求被字面满足。
3. `c++latest` 与它的分工清晰:`latest` = 最新**已定档**(仍是确定性构建),
   `c++fly` = latest **+ 实验门**(明示放弃跨工具链确定性,见 §5.5)。
4. 机制上它只是方案 C 注册表的一个"全选 + 取最新档"查询,实现同一张表。

## 5. 设计(方案 D 详述)

### 5.1 manifest 表面与解析

```toml
[package]
standard = "c++fly"    # 唯一新增;不需要任何其他键
```

- `normalize_cpp_standard`(`types.cppm:481`)增一个值:canonical `"c++fly"`,
  `level = 1000`(> `c++latest` 的 999,语义"latest 之上"),新增
  `CppStandardConfig::experimental = true` 字段。白名单错误消息同步补拼写。
- **不做 gnu 变体**(`gnu++fly`):实验模式下 GNU 扩展无额外意义,YAGNI。
- parse 期无其他新键、无形态校验负担。

### 5.2 工具链层:`src/toolchain/cppfly.cppm`(新模块,方案 C 内核)

模块名对齐 manifest 值(review 拍板):收敛后本模块的单一职责就是"`c++fly` 在各
编译器上的支持情况与映射",以值为名最可检索、职责边界自解释("职责命名、禁口袋名");
若日后加回具名列表表面(§10 延伸位),注册表仍住这里,届时再评估是否更名。
三张数据表 + 一个查询点:

```cpp
// 表 1:族 × 版本 → 最新已支持 -std= 档位(canonical)
struct LatestStdRule  { CompilerId family; int minMajor; std::string_view canonical; };
// GCC≥14→"c++26"、GCC13→"c++23"、Clang≥17→"c++2c"、MSVC→哨兵(std_flag_for 出 /std:c++latest)

// 表 2:实验性语言特性门(版本门控)
struct StdFeatureRule { CompilerId family; int minMajor; std::string_view flags; };
struct StdFeature     { std::string_view name;      // "reflection"
                        std::string_view paper;     // "P2996" —— summary/文档用
                        std::span<const StdFeatureRule> rules; };  // 无该族行 = 不支持

// 表 3:标准库实验门(stdlib 维度,非 compiler 维度)
struct StdlibGateRule { std::string_view stdlibId;  // "libc++"
                        std::string_view flags; };  // "-fexperimental-library"

struct CppFlyResolution {
    std::string              stdCanonical;   // 档位(交给既有 std 通道)
    std::vector<std::string> flags;          // 全部实验门旗标(该族拼写,去重)
    std::vector<EnabledInfo> enabled;        // 名字 + 旗标 —— summary 用
    std::vector<SkipInfo>    skipped;        // 名字 + 原因 —— summary 用
};
CppFlyResolution
cppfly::resolve(const Toolchain& tc);   // 唯一查询点;首个 Toolchain::version 消费者
```

v1 表内容(刻意最小):

| 表 | 内容 |
|---|---|
| LatestStd | GCC≥14→c++26 / GCC13→c++23 / Clang≥17→c++2c / MSVC→latest 哨兵 |
| 语言特性 | `reflection`(P2996:GCC≥16→`-freflection`)、`contracts`(P2900:GCC≥16→**空串**,随 c++26 档位默认启用,§11-Q3 已定案——空串行"支持但无需附加旗标"从第一天就有真实客户) |
| 标准库门 | libc++→`-fexperimental-library`(libstdc++/MSVC STL 无行 = 无需) |

**毕业演化**:某族新版本默认启用某特性后,把该行 `flags` 改为空串(summary 仍列为
enabled,佐证"该值语义自动收窄");Clang 落地 P2996 后加一行。manifest 永不用改。

### 5.3 注入:复用 0.0.90 通道,单点计算、两点消费

toolchain resolve 之后(此时才知道族/版本/stdlib):

```
cfg.experimental?
  → r = cppfly::resolve(tc)
  → std 档位:std_flag_for 按 r.stdCanonical 出旗标(签名扩为接收 Toolchain 或预解析结果;
     顺带审计现状 c++latest 在 GNU 侧的 canonical→旗标路径,见 §11-Q5)
  → effectiveDialect = dialect_flags(bc) ∪ r.flags   // 既有合并函数扩一个入参,去重
```

`effectiveDialect` 在**单一 helper**里计算,喂给现有两个消费点——
`plan.cppm:328-331`(→ `BuildPlan::dialectFlags` → `flags.cppm:304-310` 全局 `$cxxflags`)
和 `prepare.cppm:2590-2607`(`stdFlagAndDialect` → P1689 扫描 + std/std.compat BMI 预构建)。
两点必须同源,否则重演"scan/BMI 少旗标"一类分裂 bug。下游(指纹失效、ninja 重建、
compile_commands.json)全部自动正确,零新管线。

**拼写族问题(§1.2-4)顺带解决**:`cppfly::resolve` 按 resolved 族输出拼写,
注入通道的天然是对的;raw `dialect_cxxflags` 逃生舱维持"用户自负拼写"。

### 5.4 软语义 + summary(该值的契约)

`c++fly` 的本义是"这套工具链能玩的都给我"——**逐特性软判定,不硬错**
(硬错会让该值在 Clang/MSVC 上不可用,违背本义)。作为交换,构建头部**必打印** summary
(非 verbose 也打;呼应 CMake"无稳定性承诺"的意图但不设 UUID 摩擦):

```
c++fly on gcc@16.1.0: -std=c++26; enabled reflection (-freflection), contracts; skipped: (none)
c++fly on llvm@22.1.8: -std=c++2c; enabled experimental-library (-fexperimental-library); skipped reflection (clang: unsupported), contracts (clang: unsupported)
```

`mcpp why toolchain` / `self doctor` 输出追加同一份解析结果(与 0.0.90 §1.3d 的
dialectFlags 输出并列)。生态衔接:summary/doctor 里对 skipped 特性给出可执行 hint
(如 `hint: reflection needs [toolchain] linux = "gcc@16"`)——mcpp 自分发工具链,
这个修复闭环是 CMake/meson 给不了的。

### 5.5 确定性边界与图语义(明示,不遮掩)

- 该值**按设计放弃跨工具链确定性**(同一 manifest 在 gcc16/llvm22 上档位与旗标不同)——
  这正是"尝鲜模式"的定义,由 summary 保证可解释性。适用场景:playground、实验分支、
  跟进标准演进的库的 CI 矩阵;**不适用**:追求可复现分发的包(§11-Q2:index 收录是否
  警告/拒绝该值)。
- 图语义零新规则:`standard` 本就 root 统治全图(`prepare.cppm:172`,依赖 manifest 的
  standard 被 root 覆盖),`c++fly` 继承之;实验旗标走图全局方言通道,
  与档位天然同图一致。依赖自己声明 `c++fly` 而 root 没写 → 与今天任何
  standard 不一致的处理相同(root 赢),不新增分歧面。

### 5.6 缓存与指纹

无新逻辑:档位旗标与实验旗标都进全局 `$cxxflags` 与 std-BMI 命令串,ninja 命令行
变更与 std-module.json 命令哈希已覆盖失效。**注意**:指纹的输入是 resolve **后**的
旗标串(非 canonical 名),所以换工具链/升级 mcpp 表数据 → 命令串变 → 自动重建,
"移动目标"不会产生陈腐缓存。

## 6. 测试

- **单测**(表驱动,仿 `test_toolchain_dialect.cpp`):
  - `cppfly::resolve`:gcc13/15/16 × clang17/22 × msvc,断言档位、enabled/skipped
    切分、旗标拼写;stdlib 维度(libc++ vs libstdc++ vs MSVC STL);
  - `normalize_cpp_standard("c++fly")`:canonical/level/experimental 字段;
    白名单错误消息含新拼写;
  - 与 `dialect_cxxflags` 合并去重(用户手写 `-freflection` + experimental 不重复)。
- **e2e**:
  - `98` 系增变体:manifest 仅 `standard = "c++fly"`,gcc16 上断言全图 TU、
    scan、std-BMI 命令串含 `-std=c++26` 与 `-freflection`,构建运行绿
    (`# requires: gcc`,host gcc ≥ 16 才跑,遵守既有 host-aware 纪律);
  - llvm 工具链 + 同 manifest → 构建照常,断言 `-std=c++2c`、`-fexperimental-library`
    (libc++ 时)与 skipped summary 文案(软路径回归,不依赖 gcc16,CI 可跑);
  - 既有 `standard = "c++23"/"c++26"/"c++latest"` 工程全量回归(字节不变)。

## 7. 工作量分解(单 PR,目标 0.0.91)

| 步 | 内容 | 触点 |
|---|---|---|
| S1 | `cppfly.cppm` 三表 + `cppfly::resolve` + 单测 | `src/toolchain/`(新) |
| S2 | `normalize_cpp_standard` 增值 + `CppStandardConfig::experimental` + `std_flag_for` 版本化(含 c++latest GNU 路径审计 §11-Q5)+ 单测 | `types.cppm`、`dialect.cppm` |
| S3 | prepare 接线:experimental 分支 → resolve → 方言合并 helper(两消费点同源)→ summary/doctor 输出 | `prepare.cppm`、`plan.cppm`、`flags.cppm`(仅合并点) |
| S4 | e2e ×3(§6) | `tests/e2e/` |
| S5 | CHANGELOG、README manifest 一节 | docs |

预估含测试 400–700 行;S1/S2 可并行,S3 依赖两者。

## 8. 兼容与迁移

纯增量:不写 `c++fly` 的 manifest 行为逐字节不变;`c++latest` 语义不动
(最新**已定档**,确定性构建);`dialect_cxxflags` 与 cxxflags known-list 自动提升
继续工作(定位:注册表没有的新旗标 / 单点精细控制的逃生舱,README 如此表述);
`98` 号 e2e 原变体保留(逃生舱回归)。无格式版本升级。xpkg 路径
(`xpkg.cppm:1226` 一带)v1 不加该值(index 包无此需求,见 §11-Q2)。

## 9. 验收标准

1. `standard = "c++fly"` 一行,gcc@16 上:全图 TU、扫描、std BMI 均带
   `-std=c++26 -freflection`(契约随档位默认启用,无附加旗标,§11-Q3),
   反射示例工程构建运行绿,manifest 零手写旗标。
2. 同一 manifest 切 llvm@22:构建照常,档位 `-std=c++2c`,libc++ 下带
   `-fexperimental-library`,summary 列出 skipped reflection/contracts 及 hint。
3. summary 在两种工具链上均打印且内容与实际命令串一致(std-module.json 可核对)。
4. `c++latest`/`c++26`/`c++23` 工程全量 e2e 无回归,行为字节不变。
5. gcc@15 上 experimental:档位 `-std=c++26`,reflection 因版本门控进 skipped
   (而非编译器报未知选项)。

## 10. 非目标(显式出圈)

- **具名特性列表表面**(`std_features = ["reflection"]`,初版方案 C):机制(注册表)
  本 PR 全部就绪,列表表面等"库硬依赖单一特性、需要硬错诊断"的需求出现再加,零返工。
- **`gnu++fly` 变体**(§5.1,YAGNI)。
- **契约评估语义的语义化配置**(GCC 拼写 `-fcontract-evaluation-semantic=`
  `ignored|observed|enforce|quick_enforce`,以 gcc16 man page 为准):属"特性的参数"
  而非"开关",走 `dialect_cxxflags` 直传。
- **clang-p2996 / 未来 llvm-p2996 fork 打包进 mcpp 生态**:机制已就绪(表里加行),
  打包/分发是独立 issue;落地后 Clang 侧 experimental 自动获得反射。
- **把任意依赖 cxxflags 全图化**(0.0.72 包所有制不变,与 post-089 §1.5 一致)。

## 11. 开放问题(review 决策点)

- **Q1 值命名(已定案:`c++fly`)**:用户 review 拍板——短、有项目气质,且
  "fly" 不与任何真实 `-std=` 拼写冲突。落选备选记录:`c++experimental`
  (自文档化但长)、`c++next`、`c++edge`。版本无关性(§4-D 核心论证)与拼写无关。
- **Q2 index 收录策略(已定案:v1 warn 观察)**:发布到 index 的包声明 `c++fly`
  时收录侧输出警告(非确定性语义提示),不拒绝;观察生态用法后再定是否收紧。
  落点在 mcpp-index 收录校验,独立小 PR,不进本 PR 范围。
- **Q3 GCC 16 契约的确切 enable 旗标(已定案,2026-07-14 实机探测)**:本机
  `xim-x-gcc/16.1.0` cc1plus `-fsyntax-only` 探测结论——`-std=c++26` 单独即定义
  `__cpp_contracts` 且 `pre()` 语法可编译(`-std=c++23` 下报语法错,探针有效);
  `-fcontracts` 的作用是在更早标准下强开(`-std=c++23 -fcontracts` 亦定义宏)。
  对照:反射不对称——`-std=c++26` 单独**不**定义 `__cpp_impl_reflection`,必须
  `-freflection`。故表数据:contracts → 空串,reflection → `-freflection`。
  (0.0.90 known-list 里的 `-fcontracts` 保留不动:它仍是合法的图全局方言旗标,
  只是本设计的 experimental 展开不需要发它。)
- **Q4 CI**:硬路径 e2e 需要 host gcc ≥ 16;若 CI 镜像未达,首版仅本地跑
  (`# requires: gcc` 门已有),llvm 软路径 e2e 不受影响。
- **Q5 c++latest GNU 路径审计**:`std_flag_for`(`dialect.cppm:122`)GNU 分支为
  `stdPrefix + canonical`,而 `c++latest` 的 canonical 即 "c++latest"(GCC 不识别
  `-std=c++latest`)——实现 S2 时核查现状调用链是否已在上游换算,若是历史暗雷则
  顺手修复(独立小 commit)。
