# 依赖构建产物的全局缓存收敛 — 设计

日期：2026-07-30
状态：设计定稿，待实施
分支/worktree：`worktree-build-cache-design`
关联：本仓库 `.agents/docs/2026-07-30-issue311-bmi-staging-and-cache-root-design.md`（**前置依赖**）
建议目标版本：阶段 1 → issue311（2026.7.30.1）之后的下一版；阶段 2 紧随

---

## 0. 一句话结论

mcpp **已经有**一套全局依赖 BMI/对象缓存（`$MCPP_HOME/bmi/<fp>/deps/...` + `mcpp cache
list|info|prune|clean`），但它当前**净收益为零**：

1. 缓存键用的是**全工程指纹**，任何工程只要包名/版本/无关依赖/flag 不同就完全不共享；
2. 即使命中，产物被拷进 build dir 后 **ninja 仍然全部重编**（`command line not found in
   log`）——所以 CLI 打印的 `Cached <pkg>` 是**假的**；
3. 顺带一个已经存在但被 (2) 掩盖住的**正确性缺陷**：`--profile` 不进缓存键。一旦修好 (2)，
   `mcpp build --release` 会直接吃到 `-O0 -g` 的依赖对象。

本设计把缓存键从「全工程指纹」收敛为「**每包 Merkle 键**」，把命中路径从「拷贝后祈祷 ninja
不重编」改为「**发 stage 边而不发 compile 边**」，并按硬性顺序先补齐正确性前置。

---

## 1. 现状机制（逐段核过的链条）

### 1.1 缓存已经存在，且已接入构建

| 环节 | 位置 |
|---|---|
| 缓存条目布局与读写 | `src/bmi_cache.cppm`（`CacheKey` / `is_cached` / `stage_into` / `populate_from`） |
| 命中判定 + staging（prepare 阶段） | `src/build/prepare.cppm:3725-3831` |
| 回填（build 成功之后） | `src/build/execute.cppm:300-308`、`:951` |
| std BMI 缓存 | `src/toolchain/stdmod.cppm:189-303`（`ensure_built`，键 = `<cache_root>/<fp>`） |
| 运维命令 | `src/bmi_cache/maintenance.cppm` → `mcpp cache list/info/prune/clean` |
| 全量清理 | `src/build/execute.cppm:1118`（`mcpp clean --bmi-cache`） |

缓存根目录：`mcpp::toolchain::default_cache_root()` = `$MCPP_HOME/bmi`
（该函数自身的三份拷贝问题由 issue311 的 S2 收敛，本设计直接复用其 `mcpp::home::bmi_root()`）。

### 1.2 缓存键 = 全工程指纹

```cpp
// src/build/prepare.cppm:3788-3798
mcpp::bmi_cache::CacheKey key {
    .mcppHome    = (*cfg2)->mcppHome,
    .fingerprint = fp.hex,              // ← 全工程指纹
    .indexName   = depIdent ? depIdent->indexName : (*cfg2)->defaultIndex,
    .packageName = depName,
    .version     = depVer,
    ...
};
```

而 `fp.hex` 的第 7 个字段（`compileFlags`）是
`canonical_compile_flags(*m) + canonical_package_build_metadata(packages)`
（`prepare.cppm:3587-3588`），后者遍历 **`packages` 全表，含 `packages[0]` = root 工程本身**
（`prepare.cppm:1363` / `2216` 建立，`267-334` 序列化 `name`/`version`/flags/include dirs/
generated_files）。

std BMI 同样按 `fp.hex` 分目录（`stdmod.cppm:204`：`cache_root / fingerprint_hex`）。

### 1.3 命中之后 ninja 并不认账

`stage_into`（`bmi_cache.cppm:139-181`）把缓存里的 `.gcm/.pcm` 与 `.o` 拷进
`target/<triple>/<fp>/{gcm.cache,obj}/`，并 `touch_now`。但 ninja 判脏不只看 mtime：
`build.ninja` 里这些文件仍然是 **compile 边的输出**，而 ninja 对「输出存在但 `.ninja_log`
里没有该输出的命令行记录」的判定是 **dirty**。新 build dir 天然没有 `.ninja_log`，于是
全部重编。

---

## 2. 实测证据

全部在本机跑过，命令与输出可复现。

### E1 — 全局缓存命中却 100% 重编（决定性）

```
$ mcpp build                       # cachetest: 依赖 compat.zlib@1.3.2
   Compiling cachetest v0.1.0 (.)
      Cached compat.zlib v1.3.2          ← mcpp 说命中
[3/19] ... gcc ... -c .../zlib-1.3.2/gzclose.c  -o obj/gzclose.o
[4/19] ... gcc ... -c .../zlib-1.3.2/uncompr.c  -o obj/uncompr.o
   ...（16 个 .c 全部重编）
```

ninja 自己的解释：

```
$ ninja -C target/x86_64-linux-gnu/<fp> -d explain -n
ninja explain: command line not found in log for obj/zutil.o
ninja explain: obj/zutil.o is dirty
ninja explain: command line not found in log for obj/uncompr.o
ninja explain: obj/uncompr.o is dirty
   ...
```

⇒ 缓存只在「同一个 build dir 已经有 `.ninja_log`」时"命中"，而那时本地产物本来就在。
**全局缓存的净收益 = 0，只剩拷贝开销与磁盘占用，外加一条假的 `Cached` 状态行。**

### E2 — 改自己的版本号就换指纹（缓存键过宽）

```
[package] name = "demoa", version = "0.1.0"  → Fingerprint: 3b8a8ae4fc217233
                          version = "0.1.1"  → Fingerprint: 2138d7ce160e1154
             name = "demob"                  → Fingerprint: bdd6c2981d862423
```

⇒ `mcpp version bump` / 改项目名 / 加一个无关依赖 ⇒ std BMI 与**全部**依赖缓存整体失效。
两个工程只要包名不同，即使依赖集与工具链完全一致，也**一条都不共享**。

### E3 — 本机 26 GB 缓存的重复度

```
$ du -sh ~/.mcpp/bmi        →  26G
指纹目录数                   →  1198
```

**std 侧**（按 `std-module.json` 的 15 个身份字段归一后统计）：

```
std-module.json 目录数: 1014,  distinct std 身份: 15,  合计 16.10 GB
    529 份   7.77 GB  gcc@16.1.0 c++23 libstdc++ x86_64-linux-gnu -std=c++23
    243 份   3.81 GB  gcc@16.1.0 c++26 libstdc++ x86_64-linux-gnu -std=c++26
    103 份   3.69 GB  clang@22.1.8 c++26 libc++  x86_64-unknown-linux-gnu -std=c++26
     71 份   0.06 GB  gcc@15.1.0 c++23 ...
   （其余 11 个身份合计 < 0.8 GB）
```

⇒ **1014 份，只有 15 个真身份。16.10 GB → ~0.5 GB（约 97% 可省）。**

**依赖侧**：

```
dep 条目数 3937,  distinct pkg@ver 113,  合计 8.80 GB
按 (工具链身份, pkg@ver) 归一 → 221 条, 0.52 GB   （94.1% 可省）
    162 份  compat.zlib@1.3.2
    107 份  compat.zstd@1.5.7
     73 份  compat.x11@1.8.13   （单包合计 1.486 GB）
     63 份  mcpplibs/ftxui@6.1.9（单包合计 1.490 GB）
```

⇒ 合计 **26 GB → ~1 GB**。

### E4 — 重复的产物在语义上就是同一个（且为什么内容不相同）

同一个 `compat.zlib` 的 `adler32.o` 在两个指纹目录下字节不同（7432 vs 7448）。唯一差异：

```
DW_AT_producer : GNU C11 16.1.0 -mtune=generic -march=x86-64 -g -O0 -std=c11 -fPIC   （两侧完全一致）
DW_AT_name     : /home/speak/.mcpp/registry/data/xpkgs/compat-x-compat.zlib/1.3.2/... （两侧完全一致）
DW_AT_comp_dir : .../scratchpad/be/tests/examples/eui-neo/target/x86_64-linux-gnu/01c3e0484b536e72
DW_AT_comp_dir : .../scratchpad/w133/tests/examples/eui-gui-vk-glfw/target/x86_64-linux-gnu/0659ceea0b4ded58
```

⇒ 语义完全等价，差异 100% 来自**消费者 build dir 的绝对路径**被烙进了 debug info。
两个推论：(a) 内容寻址去重在今天做不了；(b) 缓存复用后 debug 信息指向另一个工程的目录。

### E5 — profile 不进指纹（正确性）

```
$ mcpp build --dev     --print-fingerprint → Fingerprint: bdd6c2981d862423
$ mcpp build --release --print-fingerprint → Fingerprint: bdd6c2981d862423
$ mcpp build --profile dist --print-fingerprint → Fingerprint: bdd6c2981d862423
```

`prepare.cppm:799-802` 把 profile 落到 `buildConfig.optLevel/debug/lto/strip`，而
`canonical_compile_flags`（`prepare.cppm:209-265`）**只序列化 cflags/cxxflags/ldflags，
不序列化这四个字段**。三个 profile 因此共用同一个 build dir 与同一条缓存条目。

今天没出事，靠的是 ninja 的命令哈希在本地重编。**一旦修好 E1，`--release` 就会直接 stage
进 `-O0 -g` 的依赖对象。** 这决定了实施顺序（§5）。

### E6 — 传递 path 依赖被错误地写进全局缓存

`skipCache` 谓词（`prepare.cppm:3774-3785`）只在**root manifest** 的
`dependencies`/`devDependencies` 里查 `isPath()/isGit()`。传递依赖查不到 ⇒
`specIt == end()` ⇒ `skipCache = false` ⇒ 照样缓存。

```
root(rootp) → A{path} → B{path}
$ mcpp build
$ find ~/.mcpp/bmi/<fp>/deps -mindepth 2 -maxdepth 2 -type d
/home/speak/.mcpp/bmi/c9a9bded8f95af7e/deps/mcpplibs/B@0.1.0     ← B 被缓存了
```

且 `indexName` 回落到 `defaultIndex`，本地包被**误挂到 `mcpplibs` 索引名下**。

接着改 `B/src/B.cppm` 的函数体、不动版本号：

```
$ mcpp build --print-fingerprint  → Fingerprint: c9a9bded8f95af7e   （不变）
$ ls -l ~/.mcpp/bmi/c9a9bded8f95af7e/deps/mcpplibs/B@0.1.0/obj/
-rw-rw-r-- 1 speak speak 3128 ... B.m.o                            （旧对象仍在）
```

⇒ 同样被 E1 掩盖着。**修好 E1 之后这是静默错误产物。**

本机真实缓存里已经躺着这类条目：`mcpp cache list` 输出中含 `mcpplibs/B@0.1.0`
（正是上面这个实验造出来的）以及一批 `mcpplibs/<name>@0.1.0` 形态的条目 —— `@0.1.0` +
挂在 `mcpplibs` 名下，与本节的误判路径吻合。

### E7 — 用户 flag 到底影响不影响依赖（跨工程共享成立的前提）

```
[build] cxxflags = ["-DROOT_ONLY_FLAG=1"]
        cflags   = ["-DROOT_C_FLAG=1"]
```

```
root TU : g++ ... -std=c++23 -fmodules -O0 -g --sysroot=... -B... -DROOT_ONLY_FLAG=1 -c src/main.cpp
dep  TU : gcc ... -std=c11         -O0 -g --sysroot=... -B... -D_GNU_SOURCE -include mcpp_zlib_config.h -c .../adler32.c
```

⇒ **root 的 `[build]` cflags/cxxflags 不下发到依赖。** 依赖只吃：自己的
`buildConfig`（含 feature 折叠出来的 `-DMCPP_FEATURE_*` 与 per-glob flags）、
工程级 profile（`-O0 -g`）、工具链级 flag（`--sysroot`/`-B`/standard）。

这正是「不同工程引用同一个 index 包，产物应当一致」成立的机制依据。**会**影响依赖产物的轴
只有：工具链身份、target triple、C++/C standard 与方言（含 `c++fly`）、macOS deployment
target、**profile**、该包自身激活的 features。这些全部必须进键，且都是**应该**影响的。

### E8 — profile 不进指纹的**第二个**受害者：fast path 直接交付错 profile 的产物

不只是缓存条目串号。`.build_cache` 的条目**只按 `targetTriple` 去重**
（`execute.cppm:143-145`：`erase_if(e.targetTriple == targetTriple)`），profile 不在键里；
而 fast path 的准入条件里 `ov.profile.empty()`（`cmd_build.cppm:81-83`）只挡显式
`--profile/--dev/--release`，**挡不住裸 `mcpp build`**。于是：

```
$ mcpp build                       # 默认 dev
   obj/main.o → DW_AT_producer: GNU C++23 16.1.0 ... -g -O0 -std=c++23 -fmodules   ✔ dev

$ mcpp build --release             # 同一个目录 9900f2252da562b2（fp 不变）
   obj/main.o → 无 debug info                                                       ✔ release

$ mcpp build                       # 应当回到 dev
    Finished release [optimized] in 0.00s
   obj/main.o → 无 debug info                                                       ✘ 仍是 release
$ grep -oE '\-O[0-9s]' build.ninja  →  -O2                                          ✘ build.ninja 里就是 -O2
```

⇒ **今天 `mcpp build --release` 之后，裸 `mcpp build` 会 0.00s "成功"并交付 release 产物。**
这是一个独立于缓存的现存 bug，与 C3 同根（profile 不是失效轴），修 C3 时必须一起修，否则
把 fp 分家只是把"同一个目录里内容是错的"换成"指向另一个 profile 的目录"。

### E9 — GCC 把被导入模块的 BMI CRC 烙进导入者的 BMI（决定 §4 S1 的 F 轴形态）

手工三组对照（`g++ 16.1.0 -std=c++23 -fmodules`，`A` 导入 `B`，只重编 `B`、保留旧 `A.gcm`）：

| 改动 | 结果 |
|---|---|
| 只改 `bfn()` **函数体**（接口逐字节不变） | 旧 `A.gcm` + 新 `B.gcm` **编译通过** |
| 改 `bfn()` 返回类型 + 新增导出类型（**接口**变） | **硬失败**（下方输出） |
| `-DWIDE` 改变导出结构体布局（**ABI** 变，源码字节不变） | **硬失败**（同下） |

```
B: error: module 'B' CRC mismatch
B: error: failed to read compiled module: Bad file data
A: error: failed to read compiled module: Bad import dependency
A: fatal error: returning to the gate for a mechanical issue
```

两个推论：

1. `A` 的 BMI 与它当初读到的那一份 `B` 的 BMI 是**硬绑定**的 —— 所以 `A` 的缓存键必须包含
   「`B` 的 BMI 身份」，而不是「`B` 的 public 接口的某个枚举摘要」。
2. **失败模态不对称**：BMI 轴上键取窄了 ⇒ GCC CRC 硬报错（吵、但不出错产物）；
   `.o` 轴上键取窄了 ⇒ **静默错对象**（无任何校验）。设计必须按后者取保守侧。

### E10 — 顺带发现的两处小问题

- `mcpp cache prune --older-than` 用 `last_write_time(entry.dir)`，而 `stage_into` 命中时
  **不更新缓存目录的时间**（只写消费者 build dir）。所以它是 "last **populated**"，不是 LRU：
  一个天天命中的热包会被当成冷条目剪掉。
- `cache_clean()`（`maintenance.cppm:167-175`）第一行 `remove_all(bmi / "deps")` 指向一个
  不存在的路径（deps 在 `bmi/<fp>/deps`），是死代码；真正生效的是后面的循环。

---

## 3. 缺陷编号汇总

| 编号 | 缺陷 | 层级 | 证据 |
|---|---|---|---|
| **C1** | 命中的依赖产物 ninja 一律重编（`command line not found in log`），全局缓存净收益为零，且 UI 谎报 `Cached` | 构建后端 | E1 |
| **C2** | 缓存键 = 全工程指纹（含 root 包名/版本、全图 flags）⇒ 跨工程零共享、改自己版本号即全失效 | 键 | E2 E3 |
| **C3** | profile（`-O`/`-g`/lto/strip）不进指纹 ⇒ dev/release/dist 共用同一个 build dir 与同一条缓存条目 | 正确性 | E5 |
| **C3b** | `.build_cache` 条目只按 `targetTriple` 去重，fast path 的准入只挡显式 `--profile/--dev/--release` ⇒ `--release` 之后裸 `mcpp build` 0.00s "成功"并交付 release 产物。**这是今天就在生效的 bug**，与 C3 同根 | 正确性（独立于缓存） | E8 |
| **C4** | 传递 path/git 依赖被写进全局缓存，且源码变更不改键 | 正确性 | E6 |
| **C5** | 缓存条目无自描述元数据（只有 `manifest.txt` 文件清单），`is_cached` 是"存在即命中"，无法校验、无法审计 | 键 | 代码 |
| **C6** | 依赖对象里烙进消费者 build dir 路径（`DW_AT_comp_dir`）⇒ 无法内容寻址去重、debug 信息指向别的工程 | 可复现性 | E4 |
| **C7** | 整个 `MCPP_VERSION` 进指纹 ⇒ 每次 mcpp 发版把全部缓存作废（含纯 C 目标文件） | 键 | 代码 + E3 的 1198 目录 |
| **C8** | `cache prune` 是 last-populated 而非 LRU；无容量上限；`cache clean` 有一行死代码；无 `cache dir` | 运维 | E8 |

C3/C4 与 C1 的关系是硬约束：**C1 修好之前它们是无害的浪费，修好之后是静默错误产物。**

---

## 4. 设计

### S1 — `PackageBuildKey`：每包 Merkle 键（治 C2 / C3 / C5）

新建叶模块 `src/build/cache_key.cppm`（`export module mcpp.build.cache_key;`），从
`BuildPlan` + `PackageRoot` 计算每个包**自己**的键。`CompileUnit` 已经带
`packageName` 与 per-package 的 `packageCflags/packageCxxflags/localIncludeDirs`
（`plan.cppm:20-36`），**每包粒度的数据已经在 plan 里了**，不需要新的解析。

键的输入，按轴分组（序列化格式固定、有序、带字段前缀，与 `canonical_*` 现有风格一致）：

| 轴 | 字段 |
|---|---|
| A 工具链 | `compiler_id`, `compiler_version`, `driver_identity`, `target_triple`, `stdlib_id`, `stdlib_version` |
| B 语言/方言 | `cpp_standard`, `std_flag`, `dialect_flags`（含 `c++fly` 解析结果）, `c_standard`, `macos_deployment_target` |
| C profile ← **C3 的修复** | `opt_level`, `debug`, `lto`, `strip` |
| D 包身份 | `index_name`, `package_fqn`, `package_version` |
| E 该包自身的有效构建配置 | `sorted(active_features)`, `cflags`, `cxxflags`, `asmflags`, `ldflags`, `globFlags`（有序全量，同 `prepare.cppm:301-307`）, `defines`, `generated_files(path=content)`, `include_dirs`（**相对 xpkgs store 根归一后**）, `sources`（相对包根、有序） |
| F 上游接口闭包 | `sorted( 每个直接依赖 → 它的 PackageBuildKey )`，递归 ⇒ Merkle |
| G BMI 纪元 | `mcpp_bmi_epoch`（见 S5，**不是**完整 `MCPP_VERSION`） |

**不含**：root 包名/版本、root 的 `[build]` flags（E7 已证不下发）、图里无关的兄弟依赖、
消费者的 build dir 路径。

**F 轴为什么必须是递归键，而不是「枚举上游的 public include dirs + interface defines」：**

1. **BMI 是硬绑定的，不是"接口等价即可"。** E9 实测：GCC 把被导入模块 BMI 的 CRC 写进导入者
   的 BMI，接口或 ABI 一变就 `module 'B' CRC mismatch` + `Bad import dependency`。所以
   `A` 的键需要的是「`B` 的 BMI 身份」本身，而 `B` 的 BMI 身份 = `B` 的完整键。
2. **可枚举清单必然漏项。** 要枚举的是 `B` 的 public include dirs 的**内容**、interface
   defines、`generated_files`、以及 `B` **re-export** 出去的传递模块接口 —— 最后这项从 `B`
   的 manifest 里根本看不出来（它是 `R` 的接口，经 `B` 转出）。少一项 = 命中一条 BMI
   加载不了的条目。
3. **失败模态不对称（E9 的第二个推论）**：BMI 轴取窄 ⇒ CRC 硬报错（吵）；`.o` 轴取窄 ⇒
   静默错对象（哑，`.o` 没有任何自校验）。设计必须按后者取保守侧。

递归的真实代价**不是**「上游换 profile 级联下游」—— profile 是 C 轴，它本来就在**每一个**
包的键里，有没有 F 轴都会让全图 key 变。递归的真实代价是：**上游的私有输入变化会级联下游**
（例如 `B` 给自己加一个 `-DB_INTERNAL_LOG=1`，或改一个不提供模块的私有源文件，`B` 的接口
逐字节没变，但 `A` 的键跟着变、`A` 白重编一次）。

而这个代价在真正吃缓存的人群里 ≈ 0：**index 包的描述符按版本冻结**，`B` 的 buildConfig 不
可能不 bump 版本就变（仓库里 #253 的注释依赖的正是这条不变量），所以 `B` 的键只能因
「版本 bump」或「全图轴（工具链/standard/profile）」而变 —— 而版本 bump 本来就该让 `A`
失效（`A` 要链接 `B` 的对象）。唯一能不改版本就动 `B` 私有 flag 的是 path/git 包，而它们
已被 S9 整体排除出缓存。

⇒ 递归是唯一可靠的形态，且在目标人群里几乎不付代价。

拓扑序已由 `report.topoOrder`（`prepare.cppm:3561`）给出，自底向上一次遍历即可算完全图键。

### S2 — 缓存布局与自描述条目（治 C5）

> **实施时的修正**：根目录不是 `$MCPP_HOME/cache/v1` 而是
> **`$MCPP_HOME/build-cache/v1`** —— `cache` 这个名字已归 `GlobalConfig::metaCacheDir`
> （索引元数据）所有，而它的 reset 路径 `remove_all` 整个目录。其余布局如下不变。
> 完整偏差清单见实施计划的「实施结果与计划的偏差」。

```
$MCPP_HOME/build-cache/v1/
  pkg/<index>/<pkg>@<ver>/<key16>/
      entry.json          ← 自描述：键的全部输入 + 文件清单 + created/accessed
      bmi/<module>.{gcm,pcm}
      obj/<rel>.o
  std/<stdkey16>/
      std-module.json     （沿用现有 14+1 字段元数据）
      {gcm,pcm}.cache/std*.{gcm,pcm}
      std*.o
```

`entry.json` 是把 std 侧已经做对的事（`stdmod.cppm:88-145` 的 `metadata_for` +
`metadata_matches`）搬到依赖侧：**命中判定从「manifest.txt 存在且文件都在」升级为
「键匹配 ∧ `entry.json` 的输入逐字段等于本次算出的输入 ∧ 清单文件齐全」**。

`v1/` 前缀 + 新目录名意味着老的 `$MCPP_HOME/bmi/<fp>/` 一次性弃用。**不自动删**：
`mcpp doctor` 报一行 "legacy BMI cache at ~/.mcpp/bmi (26.0 GiB), safe to delete:
`mcpp cache clean --legacy`"。理由：E1 证明这 26 GB 从来没产生过收益，没有任何保留价值，
但删 26 GB 必须是用户的显式动作。

### S3 — std BMI 换键（治 C2 的 std 侧，收益最大、改动最小）

`stdmod.cppm:204`：

```cpp
- sm.cacheDir = cache_root / std::string(fingerprint_hex);
+ sm.cacheDir = cache_root / "std" / std_identity_key(metadata);
```

`metadata_for(...)` 已经是**正确且完整**的 std 身份（compiler/version/driver_identity/
triple/stdlib/std_flag/源文件 hash/构建命令），`metadata_matches` 已经在按它校验。今天唯一
的错误就是**目录名用的不是它**。

**实现陷阱（必须处理）**：`metadata["std_build_commands"]` 里含 `sm.cacheDir` 的绝对路径，
而 `cacheDir` 又要由这个 metadata 算出来 ⇒ 自指。算键前必须把 cacheDir 出现处替换成占位符
`<CACHEDIR>`（§2 的 E3 统计脚本正是这么归一才得出「15 个身份」的）。`ensure_built` 的签名
里 `fingerprint_hex` 参数随之删除 —— 它是唯一的调用点（`prepare.cppm:3612`）。

预期效果：1014 目录 / 16.10 GB → 15 条 / ~0.5 GB。`mcpp version bump` 不再触发 std 重编
（当前约 10–60 s + 31 MB 写盘）。

### S4 — 命中的依赖发 stage 边，而不是 compile 边（治 C1）

`CompileUnit` 加一个字段：

```cpp
bool servedFromCache = false;      // 命中全局缓存，产物由 stage 边提供
std::filesystem::path cachedObject;   // 缓存内绝对路径
std::filesystem::path cachedBmi;      // 缓存内绝对路径（有模块时）
```

`ninja_backend` 对 `servedFromCache` 的 CU：

- **不发** compile 边（`ninja_backend.cppm:913-1010`）；
- **不发** P1689 scan / dyndep 边（`:850-905`）—— 依赖的模块不需要重新扫，BMI 已在缓存里；
- **改发** 每产物一条 `stage_file` 边，`$in` = 缓存内绝对路径，`$out` = build dir 内既有位置：
  ```
  build gcm.cache/<mod>.gcm : stage_file <cache>/bmi/<mod>.gcm
  build obj/<rel>.o         : stage_file <cache>/obj/<rel>.o
  ```

这样 build dir 的**产物位置、链接行、消费者 TU 的 implicit input 全部不变**，改变的只是
「这些文件由谁产生」。ninja 拿到 stage 边的命令行记录，判脏语义从此自洽。

**直接复用 issue311 的 `mcpp stage` 原语 + `restat = 1` + rule 更名 `cp_bmi → stage_file`**：
它已经定义了「size/hash 相等则一个字节都不写、尽力对齐 mtime、原子 rename、退避重试、
结构化诊断」。有了 `restat = 1`，第二次起 stage 边是 no-op 且**不级联下游重编**。这就是把
本设计排在 issue311 之后的原因 —— 两者需要的是同一个原语。

`plan.compileUnits` **保留** 这些 CU（只是打了标记），因此
`compile_commands.json`（`compile_commands.cppm:127-170`）仍然为依赖发条目，clangd/IDE
不退化。CDB 生成器忽略 `servedFromCache`。

`ctx.cachedDepLabels` 的 `Cached` 状态行从此变成真话；并且应该改成打印**省下的 TU 数**，
例如 `Cached compat.zlib v1.3.2 (16 units)` —— 假状态行能骗人三个月，带计数就骗不了。

### S5 — 收窄失效轴（治 C7）

`fingerprint.cppm` 的 `fp.parts[7] = MCPP_VERSION` 保持不变（build dir 命名与本地增量归它
管，宁可保守）。但**缓存键的 G 轴换成 `mcpp_bmi_epoch`**：一个手动递增的整型常量，只在
BMI 编码/staging 语义/键计算方式发生不兼容变化时 +1，与发版号解耦。

```cpp
// src/build/cache_key.cppm
inline constexpr int kCacheEpoch = 1;   // bump ONLY on cache-incompatible changes
```

同时进 `check_version_pins.sh` 的机器校验清单？**不进** —— 它不是 pin，不存在跨文件不变量，
只需在 `entry.json` 里记下 epoch，读到不同 epoch 的条目当 miss 处理即可（自愈）。

### S6 — 三种构建模式（用户面）

一个正交开关承载三态：

```
mcpp build                    # = --cache=global（默认）
mcpp build --cache=local      # 依赖全部在本工程 target/ 内编，不读不写全局缓存
mcpp build --cache=off        # 清空 target/ 全量重编，且不读不写全局缓存
mcpp build --no-cache         # deprecated alias of --cache=off（help 文案修正）
```

优先级：CLI `--cache` > 环境变量 `MCPP_BUILD_CACHE` > `[build] cache = "global|local|off"`
> 默认 `global`。`mcpp run` / `mcpp test` 透传同一开关（`run`/`test` 目前连 `--no-cache`
都没有 —— 顺手补齐，`cmd_build.cppm:43` 是唯一读取点）。

语义表：

| 模式 | 读全局缓存 | 写全局缓存 | 先清 `target/` |
|---|---|---|---|
| `global`（默认） | 是 | 是 | 否 |
| `local` | 否 | 否 | 否 |
| `off` | 否 | 否 | **是** |

`local` 存在的意义：排障时把「是不是缓存的问题」一次性排除掉，以及给 CI 一个可复现的
无共享状态基线。`off` 保留今天 `--no-cache` 的全部行为。

### S7 — 运维命令补齐（治 C8）

`mcpp cache` 已经是正确的归属（不需要放到 `mcpp self` 下）。新增/修正：

| 命令 | 说明 |
|---|---|
| `mcpp cache dir` | 打印缓存根绝对路径。今天 `cache *`/`doctor`/`clean --bmi-cache` 用 `default_cache_root()` 而 `config.cppm` 的重置路径用 `cfg.bmiCacheDir`，两者可能不是同一个目录（issue311 D2）；有了这条命令，"到底在哪"永远可验证 |
| `mcpp cache gc [--max-size <N>{MiB,GiB}] [--older-than <N>{s,m,h,d}]` | 真 LRU。前提：**`stage_into` 命中时必须 touch 条目**（写 `entry.json` 的 `accessed` 字段，不动产物 mtime）。默认上限建议 `[cache] max_size`（config），未设则不设限只做 `--older-than` |
| `mcpp cache clean [--deps\|--std\|--all\|--legacy]` | 今天只删 deps 且第一行是死代码。`--legacy` 删旧的 `$MCPP_HOME/bmi/` |
| `mcpp cache list --json` | 让 CI / 工具可消费；顺手把 `list` 的 `key16` 与 `entry.json` 摘要打出来 |
| `mcpp cache verify` | 逐条目校验 `entry.json` ↔ 磁盘清单，报孤儿/残缺条目（承接 `.mcpp_ok` 内容盲区那类问题） |

`mcpp clean --bmi-cache` 的文案改为指向 `mcpp cache clean --all`，行为保持。

### S8 — 依赖产物路径归一（治 C6，可与 S1–S7 并行）

给**依赖的** compile 边加：

```
-ffile-prefix-map=<consumerBuildDir>=/mcpp/build
-ffile-prefix-map=<xpkgsStoreRoot>=/mcpp/store
```

（GCC ≥ 8 / Clang ≥ 10 支持 `-ffile-prefix-map`；MSVC 无对应项 ⇒ 该轴在 MSVC 上跳过，
不作为硬要求。）

收益：依赖对象变得与消费者工程无关 ⇒ (a) debug 信息不再指向别人的工程目录；(b) 为后续
内容寻址去重/硬链接共享打开门。**不作为 C1/C2 的前置**，因为键已经保证语义等价。

### S9 — 排除本地来源的包（治 C4）

`skipCache` 谓词从「查 root manifest 的 dependencies」改为**按 `PackageRoot` 的来源判定**：

> 包根路径不在 registry/xpkgs store 之下（`cfg.mcppHome/registry/data/xpkgs/...`）的一律
> 不缓存 —— 不管它是 root 的直接 path 依赖、传递 path 依赖、workspace 成员，还是 git 依赖
> 的 checkout。

判据方向与 `mcpp add` 存在性门的教训相反：那里是「不可证伪就放行」，这里是
「**不能证明它来自不可变的 store 就不缓存**」。缓存的错误代价是静默错产物，必须取保守侧。

顺带修 `indexName` 的回落：`depIdent` 缺失时不要回落到 `defaultIndex`（E6 里把本地包 `B`
误挂到 `mcpplibs` 名下），本地包在 S9 之后根本不会进缓存，回落分支应该改成"进不了缓存"的
显式路径。

---

## 5. 实施顺序（硬约束）

```
issue311（2026.7.30.1，已设计）
  └─ mcpp stage 原语 + restat=1 + rule 更名 + mcpp.home 收敛
       ↓ 复用

阶段 1 · 正确性前置 —— 必须先于阶段 2 合入
  P1  profile 进失效轴：本阶段用**最小改法**——在 canonical_compile_flags 里补序列化
      optLevel/debug/lto/strip（4 行），让现有 fp.hex 立刻区分 dev/release/dist。
      阶段 2 把它作为 S1 的 C 轴迁到每包键上；两处不是重复实现，是同一语义的迁移。
      副作用（可接受、须写 CHANGELOG）：三个 profile 从此各占一个 target/<triple>/<fp>/
      目录，互不覆盖 —— 这本来就是 cargo 的行为。代价是磁盘 ×profile 数（每个 build dir
      带一份 staged std BMI，本机 std.gcm = 31,466,112 字节）。
  P2  C3b：.build_cache 条目按 (targetTriple, **resolved profile**) 去重，profile 不匹配
      即当 miss 走 prepare_build。**只做 P1 不做 P2 是不够的**：fp 分家只会把"同一个目录
      里内容是错的"换成"fast path 指向另一个 profile 的目录"，E8 的洞照旧。
      顺手修 run_build_plan 里硬编码的 ui::finished("release", ...)——它在 dev 构建下
      也打印 "Finished release [optimized]"，是 E8 里最误导人的一行。
  P3  fingerprint changed (X → Y), full rebuild 警告（execute.cppm:310-324）会在每次切
      profile 时触发。改成能区分"仅 profile 变"的文案，否则它从有用信号退化成噪音。
  S9  本地来源包（含传递 path/git、workspace 成员）排除出缓存 + indexName 回落修正
  S2  entry.json 自描述 + 命中时逐字段校验（取代"存在即命中"）
  e2e 负例：dev 构建后 --release，必须重编而不是复用；改 path dep 源码不改版本号，必须重编

阶段 2 · 性能
  S1        缓存键：全工程指纹 → 每包 Merkle 键（新模块 mcpp.build.cache_key）
  S3        std BMI 键：fp.hex → std-module.json 身份键（注意 <CACHEDIR> 占位化）
  S4        命中的依赖发 stage 边、不发 compile/scan 边（复用 mcpp stage + restat）
  S5        缓存失效轴：MCPP_VERSION → kCacheEpoch
  S6        --cache=global|local|off + [build] cache + MCPP_BUILD_CACHE
  e2e 正例：两个不同包名的工程依赖同一个 index 包 ⇒ 第二个工程 0 条依赖编译边
  e2e 正例：mcpp version bump ⇒ std 与依赖均不重编

阶段 3 · 后续（不在本设计的实施范围内，只定方向）
  S7        cache dir / gc / clean --std|--all|--legacy / list --json / verify
  S8        -ffile-prefix-map 归一
  零拷贝    直接 -fmodule-file= / 绝对路径 .o 进链接行，取消 staging
            （与 issue311 §7 合并，GCC 的 gcm.cache 相对查找需要 -fmodule-mapper）
```

**为什么阶段 1 不能与阶段 2 并肩或落后**：阶段 2 的 S4 让 ninja 第一次真正接受缓存产物。
在 C3/C4 未修的前提下，这等于把「白白重编一遍（结果正确）」变成「直接用错对象（结果错误）」。
E5/E6 两条实测就是这两颗雷的引信。

---

## 6. 不采纳 / 需要纠正的方案

| 方案 | 判定 | 理由 |
|---|---|---|
| 给 staging 输出加 `restat = 1` 就能让 ninja 跳过 | **不成立** | `restat` 只在**有** log entry 时改写 mtime 判定。E1 的 ninja 解释是 `command line not found in log`，即根本没有 entry ⇒ 直接 dirty。必须让缓存产物成为**某条边的输出**（S4） |
| `generator = 1` 标记依赖的 compile 边（ninja 对 generator 边跳过命令哈希检查） | **拒绝** | 会把「命令行变了要重编」这条规则整体关掉，源码/flag 改动也不再重编。用错误换性能 |
| 缓存内容寻址（`obj/<sha256>.o`），天然去重 | **本批不采纳** | E4 证明今天同语义对象字节不同（`DW_AT_comp_dir`）。要先做 S8 才有意义；且键已经保证语义等价，内容寻址只多省磁盘不多省时间 |
| 把 `dependencyLockHash`（`fingerprint.cppm` 第 9 字段，今天恒为空）填上，让指纹更精确 | **方向相反** | 那会让缓存键更宽（锁文件里任何一个无关依赖变化都失效）。本设计要的是**更窄且更准**的每包键。第 9 字段留给 build dir 命名用 |
| 依赖用硬链接而非拷贝 stage 进 build dir | **不采纳** | build dir 里的文件一旦被判脏重写就会**写穿**到缓存（本仓库已有 `_inherit_toolchain.sh` 写穿符号链接损坏真实工具链的先例）。reflink（`FICLONE`）可以，但只在 btrfs/xfs 且同设备 ⇒ 作为 S4 里的可选优化，默认走 issue311 `mcpp stage` 的拷贝 |
| 把缓存清理放到 `mcpp self` 下 | **拒绝** | `mcpp cache` 已经存在且是正确归属（`cli.cppm:391-410`）。`self` 是 mcpp 自身的安装/升级面 |
| 直接删掉旧的 `$MCPP_HOME/bmi/`（26 GB） | **不自动删** | 内容确实无价值（E1），但删 26 GB 必须是显式动作：doctor 提示 + `mcpp cache clean --legacy` |

---

## 7. 风险与验证

| 风险 | 缓解 |
|---|---|
| Merkle 键漏掉某个真实影响产物的轴 ⇒ 复用错对象 | 键的输入全量写进 `entry.json`，命中时**逐字段比对**（不只比 hash）。字段不匹配 = miss + 重建，永不"信任 hash 相等"。这是 std 侧 `metadata_matches` 已验证过的模式 |
| 跳过 scan/dyndep 边后，消费者 TU 的 dyndep 找不到依赖模块的 BMI | BMI 由 stage 边落在原位置（`gcm.cache/<mod>.gcm`），消费者的 implicit input 不变。e2e 必须覆盖「消费者 `import` 一个命中缓存的模块依赖」这条正例 —— 这是 S4 最可能出问题的地方 |
| `restat = 1` 引入「永不重建」 | 单测锁 `build.ninja` 文本；e2e 正例：改依赖版本 ⇒ 键变 ⇒ 新条目 ⇒ 重建 |
| 阶段 1 落地后老缓存全 miss，用户体感"变慢了一次" | 新布局 `cache/v1/` 与旧 `bmi/<fp>/` 无关，本来就是全 miss；CHANGELOG 写明一次性重建，doctor 提示旧目录可删 |
| workspace 场景：成员之间互为 path 依赖 | S9 把它们全部排除出缓存（正确：成员源码随时可改）。`--workspace` / `-p <member>` 的 e2e 必须断言成员产物**不**进全局缓存 |
| MSVC 无 `-ffile-prefix-map` | S8 在 MSVC 上跳过；不影响 S1–S7 |
| 多进程并发写同一条目 | 沿用 `populate_from` 已有的 `FileLock::try_acquire`（`bmi_cache.cppm:192`，拿不到锁即视为成功让给对方）。新增 `entry.json` 同样最后写、temp+rename 作 sentinel |

必须新增的验证（e2e）：

- **跨工程共享（正例，本设计的核心断言）**：两个不同包名/不同版本的工程依赖同一个 index 包，
  第二个工程构建时依赖的 compile 边数 = 0。用结构化载荷断言（读 `build.ninja` 或
  `-d explain`），**不要 grep 全日志**。
- **版本 bump 不失效（正例）**：`mcpp version bump` 后 std BMI 与依赖条目均命中。
- **profile 隔离（负例，阶段 1 必须先绿）**：`--dev` 构建后 `--release`，依赖必须重编。
- **path dep 源码变更（负例，阶段 1 必须先绿）**：改传递 path 依赖的源码不改版本号，
  必须重编，且该包**不出现**在 `mcpp cache list` 里。
- **假 `Cached` 回归锁**：状态行打印的 unit 数与实际跳过的边数一致。

本地复现注意（已知坑，沿用既有教训）：e2e 必须统一 `MCPP_HOME`；测本设计时**不要**用本机
`~/.mcpp`（26 GB 旧缓存 + 1198 个指纹目录会干扰计时与断言），用 clean-room `MCPP_HOME`；
`! cmd | grep` 在 `errexit` 下被豁免、永不失败，负例断言不要写成这种形状。

---

## 8. 预期收益

| 维度 | 现状 | 本设计后 |
|---|---|---|
| 全局缓存对构建时间的贡献 | **0**（E1：命中也全编） | 依赖 TU 从"每工程编一次"降为"每 (工具链×profile×features) 编一次" |
| 本机缓存体积 | 26 GB / 1198 指纹目录 | ~1 GB（std 16.10→~0.5 GB，deps 8.80→~0.5 GB） |
| `mcpp version bump` 之后 | std 重编（10–60 s）+ 全部依赖重编 | 全部命中 |
| 新工程首次构建（依赖已被别的工程编过） | 全量编依赖 | 只 stage（数十 MB 拷贝，`restat` 后二次为 no-op） |
| `--release` 复用 `--dev` 的依赖对象 | 今天靠 ninja 侥幸不出错；S4 之后会出错 | 键隔离，不可能发生 |
| 缓存可审计性 | 只有文件清单 | `entry.json` 自描述 + `cache verify` + `cache dir` |
