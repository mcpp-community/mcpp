# issue #344：全局 build cache 的对象地址必须与消费方无关

> 状态：**已实施**（2026.8.3.4）。实施中与设计的两处偏差记录在 §8。
> 关联：#233（编译边撞名）、#240（链接输入未跟改名）、#344（cache 布局跟改名而 key 未跟改）
> 涉及：`src/build/plan.cppm`、`src/build/prepare.cppm`、`src/bmi_cache.cppm`、
> `src/bmi_cache/maintenance.cppm`、`src/build/cache_key.cppm`、`src/build/ninja_backend.cppm`

---

## 0. 结论摘要

issue 的归因是对的，但只描述到第一层。代码核实后，这一条实际叠了**四层**问题，
其中两层是 #344 本身，两层是同一根因的其它实例（其中一个是**静默产出错误产物**，
比 #344 的崩溃更危险）：

| 层 | 问题 | 失败模态 |
|---|---|---|
| L0 | cache 条目内部的 `.o` 地址直接复用了消费方 build dir 的相对路径 | ninja graph 阶段崩（#344） |
| L1 | `obj/` 是**全局**命名空间，消歧普查跨越所有包；而 cache 条目是**per-package** 的 | 结构性地生成 L0 这类 bug（#233/#240/#344 同族） |
| L2 | 「命中判据」与「取用地址」是**两处独立推导** —— `is_cached` 校验条目自述的文件表，消费方却按自己算的地址去取 | 任何布局分歧都从「降级为 miss」变成「硬崩」 |
| L3 | 「可缓存性」用**标签**（`sourceKind == "version"`）判定，而不是用磁盘**出处** | `target/.mangled/` 重锚的包被判为可缓存；今天仅靠轴 F 侥幸不出错 |

修复方向：**把「cache 条目内的产物地址」定义为包自身的纯函数，并让它成为唯一真源；
命中判据改为校验「本次实际要取的那批文件」；可缓存性改判出处。**
`--cache local` 是 workaround，不进方案。

---

## 1. 精确机制（代码级）

### 1.1 消歧普查的作用域是全局的

`src/build/plan.cppm:587-607`：

```cpp
std::map<std::string, int> basenameCount;
for (auto idx : topoOrder) {                       // ← 全图所有包的所有 TU
    basenameCount[object_filename_for(graph.units[idx].path, objExt)]++;
    ...
}
for (auto& t : manifest.targets) { ... }           // ← 再叠上 root 的 entry main（#240）
```

`src/build/plan.cppm:651-660`：

```cpp
auto object_for = [&](src, pkg, relPath) {
    const auto fname = object_filename_for(src, objExt);
    if (basenameCount[fname] > 1)                  // ← 判据取自全局普查
        return "obj" / safe_object_prefix(pkg, relPath.parent_path()) / fname;
    return "obj" / fname;                          // ← 否则平铺
};
```

于是：一个**依赖包**的 `.o` 路径，取决于**别的包有没有同名 basename**。
`compat.zlib` 的 `compress.c`：

- 消费方同时拉了 `compat.bzip2`（也有 `compress.c`）→ `obj/compat_zlib/zlib-1.3.2/compress.o`
- 消费方只拉 `compat.zlib` → `obj/compress.o`

### 1.2 这个消费方相关的路径被直接当成条目内部地址

`src/build/prepare.cppm:4317-4328`：

```cpp
auto object_cache_path = [&](const std::filesystem::path& objectPath) {
    ...  objectPath.lexically_relative("obj")      // ← 只是把 "obj/" 前缀剥掉
    return objectPath.filename().generic_string(); // ← 兜底：退化成裸 basename
};
```

`prepare.cppm:4516`（写入侧）与 `prepare.cppm:4530-4531`（读取侧）都用它：

```cpp
arts.objFiles.push_back(object_cache_path(cu.object));                      // populate 用
cu.cachedObject = cached_obj_path(key, object_cache_path(cu.object));       // stage 用
```

而 `cache_key.cppm` 的七轴（A 工具链 / B 语言 / C profile / D 身份 / E 自身配置 /
F 上游 Merkle / G epoch）**刻意不含 root 的身份与 flags** —— 这个前提对
**产物内容**成立（root 的 cflags 确实不下发到依赖 TU），对**产物文件名**不成立。
同一个 key `58d45813f79658a1`，两种布局。

### 1.3 命中判据看的是错的那张表

`src/bmi_cache.cppm:202-217`：

```cpp
bool is_cached(const CacheKey& key) {
    ...
    auto arts = artifacts_from(*j);                // ← 条目自述的文件表（第一个消费者写的）
    for (auto& o : arts.objFiles)
        if (!exists(cached_obj_path(key, o))) return false;
    return true;
}
```

注意 `prepare.cppm` 在调用 `is_cached` **之前**（4499-4518 行）就已经算好了
**本次真正要取的那批 `arts`**，然后在 4520 行调用 `is_cached(key)` —— 把它扔了。
所以：

- 方向 A（archive 先跑）：条目记录 14 平 + 1 嵌套，全部存在 ⇒ `is_cached` 返回 true；
  libpng 却按自己的 15 个平地址去 stage ⇒ `obj/compress.o` 不存在。
- 方向 B 完全对称。

**谁第二个跑谁挂**，且必然挂在 ninja **graph 加载**阶段：stage 边形如

```ninja
build obj/compress.o : stage_file <cache>/…/obj/compress.o
```

输入是绝对路径、图内无规则生成、磁盘上又不存在 ⇒
`missing and no known rule to make it`。
`stage.cppm:185` 那条 `staging source does not exist` 的运行期守卫**永远到不了**
（ninja 在跑任何命令之前就已经拒绝了图）。这解释了为什么错误信息如此难懂，
也决定了**修复只能落在 plan 阶段，不能落在 stage 阶段**。

### 1.4 与既有契约自相矛盾

`src/bmi_cache/maintenance.cppm:531`（`mcpp cache verify` 的输出文案）写着：

> "They are treated as misses and rebuilt"

这正是应有的契约：**不完整的条目降级为 miss**。
但 `is_cached` 只对「条目缺自己记录的文件」执行这个契约，
对「条目的布局 ≠ 本次要的布局」不执行 —— 契约声明了，但没有被完整实现。

---

## 2. 根因分层与判据

### L1 —— 作用域不匹配（这才是生成 bug 的那台机器）

一句话：**`.o` 的名字活在「整个 build dir」这个命名空间里，而 cache 条目活在「单个包」这个命名空间里。**
用一个作用域的标识符去做另一个作用域的地址，两者的等价关系不同 —— 必然出事。

#233 / #240 / #344 是同一台机器吐出的三个产物：

- #233：全局普查产生的前缀本身不够唯一 → ninja「multiple rules generate」
- #240：普查漏了 entry main → 链接输入与编译边分叉
- #344：普查结果随消费方变化 → cache 条目布局随消费方变化

**只要「布局由一次全局普查决定」这个机制还在，这一族就会有第四个。**
所以正确的修法不是再补一处同步，而是**拆掉普查对依赖包的管辖权**。

### L2 —— 同一决策两处推导

「这个条目里 `compress.o` 叫什么」这件事，今天有两个独立推导：

1. 条目自己的 `entry.json:obj[]`（第一个 populate 者写的）
2. 本次构建的 `object_cache_path(cu.object)`

`is_cached` 只查 (1)，stage 边只用 (2)，两者从不比对。
这是本仓库反复出现的那个隐性架构债形态（见 #239/#240、#315、#336 的复盘）：
**同一决策两处推导，加新语义时会变成构建失败。**

### L3 —— 可缓存性用标签而非出处

`prepare.cppm:4469-4474` 的注释把规则写得很清楚：

> anything it cannot prove came from the immutable xpkgs store stays out

但代码实现的是一个**更弱的代理**：`depIdent->sourceKind != "version"`。
反例就在同一个文件里 —— 多版本共存（mangling）路径，`prepare.cppm:3120-3171`：

```cpp
auto stageBase = *root / "target" / ".mangled" / consumerManifest.package.name;
...
packages[item.consumerDepIndex + 1].root = consumerStage;   // ← root 重锚到 target/ 下
...
dep_cache_identities.push_back({ ..., .sourceKind = "version" });  // ← 标签仍是 version
```

消费方包的 `root` 被重锚到 `<project>/target/.mangled/<pkg>/__self__`，
**源码被 `rewrite_module_decls` 改写过**（import 名被重命名），
但它的 `sourceKind` 仍是 `"version"`，`localTaint` 也不会点亮它
（被 mangle 的次要包同样是 `"version"`）。
即：一个源码已被改写、根目录位于可变的 `target/` 下的包，**被判定为可缓存**。

今天没出错，是因为轴 F 恰好救了场 —— 次要包的 D 轴 `packageName` 变成了 mangled 名，
消费方的 `upstreamKeys` 因此不同，key 分裂。
**这是一个「侥幸成立、且只由一条轴支撑」的不变量**，而它守护的失败模态是
「静默取到用错模块名编出来的 `.o`」—— 比 #344 的崩溃更难诊断。
`cache_key.cppm:41` 自己写过这个不对称性：

> too narrow a key on the object axis is a silently wrong `.o` — objects carry no such self-check.

---

## 3. 方案

七项，R1–R4 是必须的一组（缺任何一项都留下漏洞），R5–R7 是配套。

### R1（必须）· 依赖包的 obj 命名空间按包切分，且**无条件镜像**

**改 `plan.cppm` 的 `object_for`：依赖包的对象一律落在自己的子树下，且不看普查。**

```
root 包（永不入 cache，保持现状）：
    obj/<name>.o                                    // 无冲突
    obj/<root-slug>/<mirrored-relDir>/<name>.o      // 冲突时（现有行为）

依赖包（可入 cache）：
    obj/<pkg-slug>/<mirrored-relDir>/<name>.o       // 无条件，不做普查
```

- `<pkg-slug>` = `sanitize(qualified package name)`（现有 `sanitize`，`.`/`/` → `_`）
- `<mirrored-relDir>` = 现有 `safe_object_prefix` 的 relDir 部分（去掉 pkg 前缀）

三条性质，逐条对应上面三层：

1. **跨包撞名结构性消失**（不同目录），普查对依赖包不再有管辖权 → L1 关闭。
2. 包内撞名仍由包内普查处理 —— 但普查范围是包自身的 TU 集合，
   而「哪些 TU 属于这个包」已经完全被 key 的 E 轴（`sources` + `features` +
   `generatedFiles` + `sourceGlobs`）覆盖，所以**是包的纯函数**。
   本方案取消这层普查（无条件镜像），普查只留给 root。
3. 条目内部地址 = build 路径剥掉 `obj/<pkg-slug>/` = `<mirrored-relDir>/<name>.o`，
   **是包的纯函数**，与消费方无关 → L0 关闭。

**为什么无条件、不保留「无冲突则平铺」的优化**：那个条件判断就是生成 #233/#240/#344
的那台机器。保留它就是保留状态；而它省下的只是路径长度。

**已知代价与缓解**：依赖对象路径变长（`obj/adler32.o` → `obj/compat_zlib/zlib-1.3.2/adler32.o`，
约 +25 字符）。Windows `MAX_PATH`=260 是唯一实际风险，且 build dir 本身已是
`target/<triple>/<fp>/obj/...`。实施时**必须**用 mcpp-index 里路径最深的成员
（ffmpeg / opencv-module）在 Windows 上实测。若触顶，退路是把 `<pkg-slug>` 换成
`<pkg-slug 的 8 位 hash>`（仍是包的纯函数，性质不变），**不要退回条件化**。

### R2（必须）· 条目内地址收敛为唯一真源

`CompileUnit` 新增一个字段，由 `plan.cppm` 与 `object_for` **同一处**算出：

```cpp
struct CompileUnit {
    std::filesystem::path object;            // build dir 相对路径（既有）
    std::filesystem::path packageObjectRel;  // cache 条目内相对地址；空 = 本单元不可缓存
    ...
};
```

随之：

- **删除** `prepare.cppm:4317-4328` 的 `object_cache_path` lambda —— 它是第二处推导。
- populate 侧（`prepare.cppm:4516`）与 stage 侧（`prepare.cppm:4530`）都改用
  `cu.packageObjectRel`。
- `bmi_cache::populate_from` 的 `projectObj / o` 需要从 `obj/<pkg-slug>/` 起算，
  因此 `CacheKey` 增加 `objSubdir`（或直接传绝对源路径），
  不再假设「条目内地址 == build dir 内 `obj/` 下的地址」——
  **这个假设正是 #344**，必须显式打破而不是巧合地维持。

`packageObjectRel` 的推导规则（含逃逸情形）：

1. `relPath` 在包根内 → `safe_object_prefix(relDir) / object_filename_for(src)`
2. `relPath` 逃出包根（build.mcpp 的 `OUT_DIR` 生成源，注释见 `plan.cppm:613-624`）
   → 先按 **xpkgs store root** 相对化，沿用 `fill_package_config` 已有的
   `<store>/…` 手法，映射成 `__store/<safe components>/<name>.o`
3. 两者都不成立 → `packageObjectRel` 置空 ⇒ 整个包**不可缓存**（见 R5 的全有全无）

这条明确删掉了现有的 `return objectPath.filename()` 兜底 ——
那个兜底会把两个不同源文件静默映射到同一条目地址，是一个未被触发的
「静默错误 `.o`」通道。

### R3（必须）· 命中判据校验「本次要取的那批」，不匹配一律降级为 miss

```cpp
// bmi_cache.cppm
bool is_cached(const CacheKey& key, const DepArtifacts& requested);
```

语义：

- `entry.json` 存在、schema 匹配、`key` 匹配、`inputs` 逐字段匹配（既有）
- **且** `requested` 的每一项都在条目的记录表中，**且**在磁盘上存在
- 任一不成立 ⇒ **miss**（走 populate），**永不**成为构建失败

这一条是**稳定性护栏**，不是 #344 的修法本身：它把「条目布局与本次期望分歧」
这一整类未知问题，从「ninja graph 阶段的天书错误」永久降级成「多编一次」。
R1+R2 之后这个分歧不应再出现，正因为如此，**一旦出现就必须有个诊断出口**：

```
mcpp 侧（verbose 或 warn，一行）：
  warning: build cache entry for compat.zlib@1.3.2 [58d4…] does not contain the
           artifacts this build needs (2 of 15 missing, e.g. `compress.o`);
           treating as a miss. Run `mcpp cache verify` for details.
```

没有这行，一个系统性的分歧会表现为「cache 永远 100% 不命中」而无任何信号 ——
这正是 v2026.7.30.2 之前那个假 `Cached` 骗了三个月的镜像失败模态。

同时把 `mcpp cache verify` 的检查扩展一档：**报告条目内地址是否符合规范形态**
（R1 定义的 `<mirrored-relDir>/<name>.o`），使 L2 的一致性可离线审计。

### R4（必须）· `kCacheEpoch` 1 → 2

`cache_key.cppm:67` 的注释已经把判据写死了：

> Bump ONLY when a change makes previously written entries unusable
> (the serialized input shape, **the artifact layout**, or the staging contract).

R1 改的正是 artifact layout。不 bump 的话，旧条目会被新代码当成候选，
虽有 R3 兜底降级为 miss，但会在同一目录里叠加两套布局的文件，
让 `gc` 的体积统计与 `verify` 的输出都失真。bump epoch 是这里唯一干净的做法。

### R5（必须）· 可缓存性改判**出处**，并且全有全无

两处收紧，落在 `prepare.cppm:4452-4476`：

**(a) 出处判据**。把

```cpp
if (!depIdent || depIdent->sourceKind != "version") continue;
```

改成「标签 **且** 磁盘出处」：

```cpp
if (!depIdent || depIdent->sourceKind != "version") continue;
if (!is_under(packages[i].root, storeRoot)) continue;   // 新增
```

这直接实现了 4469-4473 行注释里已经写下的规则，并结构性地排除
`target/.mangled/**` 重锚包（L3）。今天靠轴 F 侥幸成立的那个不变量，
从此有第二道、且是**按定义**成立的防线。

**(b) 全有全无**。若该包任一 `CompileUnit::packageObjectRel` 为空，
整个包退出缓存（既不读也不写）。理由：部分 stage 会让一个包的产物一半来自
cache、一半来自本次编译 —— 这是 `.o`/BMI 混龄，恰是 GCC 把 BMI 的 CRC
烙进导入者时最难诊断的那种失败。

### R6（应做）· 不变量测试：地址对图的其余部分免疫

单元测试（`tests/unit/`），直接钉住 L1：

> 构造两个 plan：图 X 只含包 P；图 Y 含包 P + 包 Q，且 Q 有与 P 同名的 basename。
> 断言 P 的每个 `CompileUnit::packageObjectRel` 在 X 与 Y 中**逐字节相同**。

这条断言就是「条目地址是包的纯函数」的机器化表述。任何未来往 `object_for`
里加入图级状态的改动都会立刻挂在这里 —— 这正是 #233/#240/#344 三次都缺的那道闸。

### R7（应做）· e2e 复现 A/B 双向

新增 `tests/e2e/1xx_build_cache_object_layout.sh`，形状照抄
`172_build_cache_cross_project.sh`（离线 path index + `fresh-sandbox`）：

- 本地索引提供 `lib-a` 与 `lib-b`，**两者各有一个同名源文件** `compress.c`
- 工程 `both` 依赖 a+b；工程 `only-a` 只依赖 a
- **方向 A**：先 `both` 后 `only-a`；**方向 B**：清 cache，先 `only-a` 后 `both`
- 两个方向都必须构建成功，且第二个工程的 `build.ninja` 里
  **`lib-a` 的源文件零 compile 边**（沿用 172 的判据：从 `build.ninja` 读，不从状态行读）

「零 compile 边」这条不能省 —— 只断言「构建成功」会被 R3 的 miss 降级悄悄满足，
测试就变成了假绿。

---

## 4. 不采纳的方案

| 方案 | 不采纳的理由 |
|---|---|
| 把「本次消歧结果」并入 cache key | issue 自己已经指出：按消费方分裂条目，与跨工程共享的设计目标直接冲突。26GB/1198 目录那次的教训就是键里混进了消费方。 |
| 只在 stage 阶段回退（源不存在则改用 compile 边） | ninja 在 **graph 加载**阶段就已失败，任何运行期回退都到不了（§1.3）。必须落在 plan 阶段 = R3。 |
| 条目里同时存两套布局 | 把 L2 的「两处推导」升级成「两处存储」。体积翻倍，且第三种布局出现时同样失效。 |
| 只做 R3（把崩溃降级为 miss）不做 R1 | 症状消失，但 zlib/ffmpeg 这类高扇入包在混合工程里将**永远不命中**，cache 收益归零而无任何信号 —— 与 `--cache local` 等价，只是更隐蔽。 |
| 只做 R1 不做 R3 | 修掉了今天这一个实例，留着「布局分歧 ⇒ graph 崩」这条通道给下一个实例。#233→#240→#344 已经证明会有下一个。 |

---

## 5. 影响面与迁移

- **旧条目**：epoch bump 后自然失效，被 `mcpp cache gc` 回收。用户侧表现为一次全量重建，
  无需任何手工步骤，也不需要提示用户清 cache。
- **build dir 布局变化**：依赖对象路径变化 ⇒ 首次构建全量重编（`target/` 内），
  与 epoch bump 的影响重合，不额外增加成本。
- **受影响的下游读者**：`compile_commands.json`、`distribution.cppm`、
  `.ddi` 放置（`ninja_backend.cppm:1026` 明确「跟随对象路径」）、链接输入
  —— 全部派生自 `cu.object` 这一真源，随之自动跟随；**不得**新增任何一处独立推导。
- **CI**：mcpp-index 全量 workspace（linux/macos/windows 三 leg，47 成员）是本条的
  最终验收面 —— 修复前 13/11/8 失败，修复后须回到 `all 47 member(s) passed`，
  且**必须核对第二个成员的 `build.ninja` 确有 stage 边**（否则可能是「全都变 miss」的假绿）。

---

## 6. 实施顺序

1. R4（epoch bump）—— 先行，使中间态不会读到旧布局条目
2. R1 + R2 —— 一起改，`plan.cppm` 与 `prepare.cppm` 同一次提交（真源迁移不可拆）
3. R3 —— `is_cached` 签名变更 + 诊断 + `cache verify` 扩展
4. R5 —— 出处判据 + 全有全无
5. R6 + R7 —— 测试；R6 必须在 R1 之前写好并**看到它在 main 上失败**
6. mcpp-index 侧：pin 到预发布版本，跑全量 workspace 三 leg

第 5 步的「先看到它在 main 上失败」不是形式主义：本仓库有过多次
「测试写完就是绿的、其实根本没覆盖到」的记录（#230 的真凶早已被修好、
#332 的「扫缓存比对字节等价」是假验证）。R6 必须先红后绿。

---

## 8. 实施记录（2026.8.3.4）

落点：`plan.cppm`（R1/R2）、`prepare.cppm`（R2/R3/R5）、`bmi_cache.cppm`（R2/R3）、
`bmi_cache/maintenance.cppm`（R3 审计）、`cache_key.cppm`（R4）、
`tests/unit/test_object_address.cpp`（R6）、`tests/e2e/184_build_cache_object_layout.sh`（R7）。

与设计稿的三处偏差，都是实施时被现实证伪的假设：

**① 「store root」不是一个目录，是一组。** 设计稿假设可缓存性判据可以写成
「包根在 `<xlingsHome>/data/xpkgs` 之下」。实际上自定义 git 索引会把 payload 装进
**项目本地**的数据根（`config::project_xlings_data_roots`:
`<project>/.mcpp/data` 与 `<project>/.mcpp/.xlings/data`）—— `tests/e2e/172` 正是
这个形态。按单一 storeRoot 判定会把 172 里的依赖判成不可缓存，
表现为「cold build did not populate a cache entry」。
所以 R5(a) 与 plan 的 `__store` 锚点都接受一个 `storeRoots` 列表。

> 这也是一条方法论证据:**先写测试再看它红，能同时验证「测试有效」和「判据正确」**。
> 172 的红是判据过窄的第一手证据，如果只跑新增的 184，这个收窄会一路带到 CI。

**②「包根在 store 之下」必须按**字面路径**判定，不能用 `std::filesystem::relative`。**
`relative()` 对两侧都跑 `weakly_canonical`，**会解析符号链接**。而「store 里的条目是指向另一个
store 的符号链接」是常态 —— `tests/e2e/_inherit_toolchain.sh` 就是这么把开发机的工具链
借给隔离 MCPP_HOME 的，CI 缓存复用热 payload 树也是同一手法。一旦 canonical 化，
`<home>/registry/data/xpkgs/<pkg>` 就不再「位于 store 之下」，**该包静默退出缓存**。

这条被 e2e `40_llvm_bmi_cache` 抓到：第二次构建打印 `Compiling` 而不是 `Cached`。
**症状是「cache 永远不命中」而非报错** —— 正是 R3 那条 warning 想要暴露、
而这里恰好覆盖不到的形态(判据过窄时根本进不了 cache 那段代码，无从报告)。
收敛为 `plan.cppm::path_is_under_any`，字面判定为主 + canonical 重试兜住路径拼写差异
(Windows 上 `HOME` 与 `USERPROFILE`、盘符大小写)，两者任一为真即可 ——
**拼写不同最坏退化成慢，不会退化成错**。可缓存性门与 `__store` 锚点共用它。

**③ R6 的「先红」不能靠 checkout main 得到**，因为断言引用的 `packageObjectRel`
字段在 main 上不存在，测试根本编译不过 —— 那是编译失败，不是断言失败，证明不了任何事。
实际做法是在新代码上**临时把 `object_for` 的依赖分支换回全局普查**，
确认两条断言精确地红（`RootObjectsStayFlatAndUncacheable` 保持绿），再还原。
R7 则可以用真正的 pre-fix 二进制跑，并复现出与 issue 完全一致的报错文本。

验证状态：unit 54/54 通过；e2e 172 / 174 / 184 通过；184 在 pre-fix 二进制上
按双方向各自复现 `missing and no known rule to make it`。

## 7. 遗留 / 后续

- **多版本共存 × 对象子树**：两个版本的同一包若同时在图中，`<pkg-slug>` 相同。
  今天 mangling 会把次要包的 `package.name` 改成 mangled 名，因而 slug 天然分开；
  但这依赖 mangling 路径，不是结构保证。若将来出现「不 mangle 的多版本共存」，
  slug 需要带版本。R6 的不变量测试无法覆盖此情形，**单独记一条**。
- **L3 的彻底收敛**：`.mangled` 重锚让「包的 root 是不是不可变 payload」这件事
  在 `prepare.cppm` 里有两处认知（`sourceKind` 标签与实际路径）。R5(a) 加的是防线，
  真正的收敛是让重锚同时更新 `dep_cache_identities` 的 sourceKind。
  可与后续的 mangling 递归化改造一并做。
