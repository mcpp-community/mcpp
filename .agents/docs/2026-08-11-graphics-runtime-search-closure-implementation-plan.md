# 实施计划:运行期搜索闭包 与 binding 降级

> 设计:`2026-08-11-graphics-runtime-search-closure-and-binding-degradation.md`
> 分支:`feat/runtime-search-closure-and-binding-degradation`
> 基线:`main` `1bc6074`(`2026.8.11.1`)。**单 PR、一档全发,版本 `2026.8.11.2`。**
>
> **实施状态:已完成。** 实施过程中改动本计划的四处,均在对应小节以
> **【实施修正】** 标出 —— 每一处都是被**实测**推翻的,不是改主意。

---

## 0. 模块划分:协议独占一个 `.cppm`,平台特化留在 `platform/`

三个及以上的读写方共享同一条规则 ⇒ 独占一个模块。这是本仓库反复付学费后立下的形状
(`loader_contract` / `graph_shape` / `host_requirements` 都是这么来的)。

| 新模块 | 协议内容 | 谁写 | 谁读 |
|---|---|---|---|
| **`src/platform/runtime_search.cppm`**<br>`mcpp.platform.runtime_search` | **运行期搜索路径契约**:一条目录的**来源**、**次序**、**是否机器本地** | `runtime_binding`(组装)、`plan`(链接期) | `elf_runtime`(闭包解析)、`pack`(剥离判定)、`runtime_validation`(记录) |

**为什么放 `platform/` 而不是 `build/`**:它描述的是**加载器怎么搜**(物理),
不是 mcpp 怎么选(策略);而且 `elf_runtime`(platform)必须读它 ——
放 `build/` 会让 `platform → build` 反向依赖。

**它只 `import std`**,不认识 `RuntimeBinding`、不认识 ELF。纯策略,可单测,零耦合。

平台特化各归各位,不外溢:

| 平台位置 | 本轮改动 |
|---|---|
| `src/platform/runtime_binding.cppm` | farm 目录发现(与既有 libc 探测同一次遍历);`declared`/`note` 降级字段 |
| `src/platform/elf_runtime.cppm` | 宿主默认目录按 binding 分档;`Status::Unresolvable` |
| `src/platform/linux/`、`macos`、`windows` | **不动** —— farm 是"有没有 DT_RPATH"的函数,已由 `platform::supports_rpath` 表达 |

---

## 1. M1 — `src/platform/runtime_search.cppm`(新)

```cpp
export module mcpp.platform.runtime_search;
import std;

export namespace mcpp::platform::search {

// 一条运行期搜索目录的来源。次序与"是否可分发"都由它决定,
// 调用方不再各自推导。
enum class Origin {
    Payload,      // 不可变载荷目录(<store>/xim-x-glibc/2.39/lib64)
    Package,      // 包描述符声明的 runtime 目录
    SubosFarm,    // subos 符号链接农场(<subos>/lib)—— 可变
    HostDefault,  // 宿主加载器的内建默认目录 —— 仅非 hermetic binding 适用
};

// 次序 = 不可变性递减。
int rank(Origin);

// 这条目录是否是"这台机器的私有状态"⇒ 不得随产物分发。
bool is_machine_local(Origin);   // Payload / SubosFarm ⇒ true

std::string_view to_string(Origin);

struct Dir { std::filesystem::path path; Origin origin; };

// 唯一一次排序 + 去重。stable,同 rank 内保持插入序。
std::vector<Dir> ordered(std::vector<Dir> dirs);
}
```

**`rank` 的理由写进注释,因为它是本轮唯一的不变式**:

> 载荷目录不可变(装一次不再动),farm 每次 `xlings install` 都重写符号链接。
> 载荷在前 ⇒ libc / libm / libstdc++ 永远从被 pin 的载荷解析,farm 只补没人提供的。
> farm 在前 ⇒ 一次安装能在事后悄悄换掉一个**已经构建好**的产物的 libc。

---

## 2. M2 — `src/platform/runtime_binding.cppm`

### 2.1 结构

```cpp
struct RuntimeBinding {
    …
    bool        declared = false;   // subos 是否自我描述(subos_info 块存在)
    std::string note;               // 降级原因;非空则调用方必须呈现
    std::vector<std::filesystem::path> searchDirs;   // farm 视图目录(可变)
};
```

`libraryDirs`(载荷,不可变)与 `searchDirs`(farm,可变)**必须是两个字段** ——
合成一个就把 rank 的信息丢了,而 rank 是本轮的全部。

### 2.2 `resolve_runtime_binding`:矛盾报错,缺席降级

| 情况 | 今天 | 改为 |
|---|---|---|
| 点名的 subos 目录不存在 | error | **error(保留)** —— 用户输入无法被满足 |
| 无 `.xlings.json` / 无 `subos_info` 块 | error | `declared=false` + `note`,**返回可用 binding** |
| `runtime` 字段为空 | error | 同上 |
| `schema > kSupportedSchema` | error | 读懂的字段照用 + `note`(与 `subos_info::read` 对齐) |
| `schema < kSupportedSchema` | error | 照用 |

降级 binding 的内容:`platform`/`arch`/`providerId`/`subosDir`/`selection` 照填,
`runtimeId` 留空,`loader`/`libc` 无值 ⇒ rule A/B 自然落到 `Inconclusive` 并说明
**是因为没有声明**,而不是因为查过了。

### 2.3 farm 发现:与既有 libc 探测同一次遍历

今天 `{subosDir/"lib64", subosDir/"lib"}` 那个循环找的是 `libc.so.6`(找到即 `break`)。
farm 需要的是**目录本身是否存在**,两件事一次走完:

```
for candidate in {lib64, lib}:
    if is_directory(candidate):        searchDirs.push_back(candidate)      // farm
    if is_regular_file(candidate/libc.so.6) and libraryDirs.empty():
        …既有的 canonical → 载荷目录 → libraryDirs / loader…
```

**不新增第二处布局知识。** 只在 `if constexpr (is_linux)` 内。

### 2.4 序列化

- `search_dirs`、`declared`、`note` 进 JSON。
- `searchDirs` + `declared` **进 `canonical_contract`** ⇒ contract hash 变 ⇒
  farm 变化会正确地让快路径与校验缓存失效。**这会让所有既有缓存失效一次,是预期的。**
- `deserialize` 的完整性检查放宽:`declared=false` 的 binding 允许 `runtimeId` 为空
  (今天 `schema==0 || runtimeId.empty()` 直接判 incomplete)。

---

## 3. M3 — `src/platform/elf_runtime.cppm`

### 3.1 搜索顺序按契约,宿主默认目录分档

```cpp
// resolve_needed 内
dirs = requester.runpaths (expand $ORIGIN)
     + additionalSearchDirs
     + binding.libraryDirs                 // Origin::Payload
     + binding.searchDirs                  // Origin::SubosFarm
     + (is_hermetic(binding) ? {} : host_library_dirs());   // ← 分档
```

**`is_hermetic(binding)` = `binding.loader.has_value()`** —— 产物的 `PT_INTERP` 指向私有
加载器时,宿主的内建默认目录**不在它的搜索路径里**。今天无条件加宿主目录,是
`validation: pass` + `cannot open shared object file` 同时成立的直接成因。

非 hermetic(`gcc@system`、macOS、Windows)保持原状:那里宿主目录**确实**是默认值。

### 3.2 第四种判决

```cpp
enum class Status { Pass, ProvenMismatch, Unresolvable, Inconclusive };
```

hermetic binding 下一个解析不到的 `NEEDED` 是**可证的失败**(私有 loader 一定打不开),
把它塞进 `Inconclusive` 是把可证的事说成没查过。

- `elf_runtime.cppm:759` 的 `inconclusive(...)` 在 hermetic 下改走 `unresolvable(...)`,
  非 hermetic 保持 `inconclusive`(宿主可能在 `ld.so.cache` 里有,mcpp 不读 cache)。
- `runtime_validation`:`has_proven_mismatch()` → `has_blocking_failure()`,
  收下 `ProvenMismatch | Unresolvable`;`status_name`/`parse_status` 补 `"unresolvable"`。
- `ninja_backend.cppm:1794` 的门同步。
- `doctor.cppm:279` 的三分支补第四支。

---

## 4. M4 — `src/build/plan.cppm`:farm 进闭包,末位

### 【实施修正 ①】不能塞进 `linkIntent.runtimeSearchDirs`

原计划让 farm 复用那个字段。**不行**:它有三个消费者,其中一个是

```
plan.linkIntent.runtimeSearchDirs → plan.runtimeLibraryDirs → compute_run_env()
                                  → LD_LIBRARY_PATH(`mcpp run` 的子进程环境)
```

而 farm 进 `LD_LIBRARY_PATH` 正是设计 §6「不做什么」第三条禁掉的东西 ——
它会污染 `mcpp run` 拉起的每一个子进程,包括宿主二进制(实测会让 `xdg-open` /
`notify-send` 死于 `__pointer_chk_guard`)。

**改为 `BuildPlan` 上的独立字段** `runtimeSearch`(`vector<search::Dir>`,
全部四种 origin 的有序记录),farm 的**唯一**消费者是 `flags.cppm` 渲染的
`-Wl,-rpath` 尾巴。**per-object 可达,绝不 per-process。**

### 【实施修正 ②】载荷目录不能只读 `linkRuntimeDirs`

`plan.toolchain.linkRuntimeDirs` **只有 clang 会填**(`clang.cppm:142`)。
GCC 的载荷 `-rpath` 来自**链接模型**(`lm.libDirs`)。第一版记录出来只有一条
farm,而产物 DT_RPATH 有三条 —— 记录与产物不一致,正是这份设计要消灭的形状。

改为向**发出它们的同一个函数**要:`resolve_link_model(plan.toolchain).libDirs`
(纯函数,可在 plan 层调用),再叠 `linkRuntimeDirs`,顺序与 `flags.cppm` 的拼接一致。

### 【实施修正 ③】装配点在 `merge_runtime_binding_contract`,不在 `build_plan`

`plan.runtimeBinding` 在 `prepare.cppm:5313` 才被赋值,晚于 `make_plan` 返回。
装配放进 `merge_runtime_binding_contract`(紧随其后调用),那里三个输入齐全。

**次序天然正确,不需要额外机制**(已核 `flags.cppm:975-978`):

```
f.ld = full_static + link_toolchain_flags + b_flag + runtime_dirs
     + link_intent_ld + atomic_ld + payload_ld + user_ldflags + link_extra
                ↑ 载荷 -L/-rpath        ↑ linkIntent(farm 在其末尾)
```

且 `runtimeSearchDirs` 的既有语义正是我们要的(`flags.cppm:804-806` 原文):
*"contributes RUNPATH only; it must never become a link-time `-L` path"* ——
链接期已由 `--sysroot` 覆盖,这里只补运行期,还省下链接行长度。

### 4.1 两条护栏

| 护栏 | 判据 |
|---|---|
| **交叉目标** | `targetTriple` 非空且(`os != "linux"` 或 `arch != binding.arch`)⇒ 不发 |
| **非 ELF** | `elfTarget == false` ⇒ 不发(与 `loader_tag_flag` 同一个判据,复用) |

---

## 5. M5 — pack:`215` 扩面(剥离不需要写代码)

### 【实施修正 ④】5.1 的前提是错的 —— `system` 档早就剥干净了

原计划断言「`Mode::None` 是唯一不重写 rpath 的档」。**实测推翻**:

```console
$ mcpp pack --mode system && tar -xzf …-system.tar.gz
$ readelf -d bin/glprobe | grep RPATH
 (RPATH)  Library rpath: []                       ← 已清空
$ readelf -p .interp bin/glprobe
  /lib64/ld-linux-x86-64.so.2                     ← 已改回平台标准解释器
$ grep -rl "$HOME/.mcpp" <bundle>/                 ← 无命中
```

`pack.cppm:718` 对 `Mode::None` 把每个依赖都标 skip ⇒ `toBundle` 空 ⇒
`rpath = ""` ⇒ `set_search_path` 整体清空。**这一项从"实现"变成"补测试"。**

「从 farm 解析到的 `NEEDED` 升级为 host requirement」也**不做**:
`HOST-REQUIREMENTS` 存在的理由是记录**产物本身看不出来**的东西(经 dlopen 链到达
的驱动),而 `NEEDED` 本来就写在产物里 —— 再抄一遍是冗余,还会污染
`mcpp publish` 对 `[runtime].requirements` 的投影。

### 5.2 `215` 的两处扩面(设计 §2.6 核出来的)

```bash
STORE="$MCPP_HOME/registry/data/xpkgs"        # ← 今天只到这里
MACHINE_LOCAL="$MCPP_HOME"                    # ← farm 在 registry/subos/…,不在 store 下
```

并补 `--mode system` 的用例 —— 今天 215 只跑默认 `vendored` 档。

---

## 6. M6 — `XLINGS_SUBOS_LD_PATHS=0`:声明式退出

mcpp 在**驱动 ninja 之前**把它设进自己的进程环境(子进程继承 ⇒ 覆盖 ninja / 驱动 / ld),
**不进 ninja 命令行**(链接行有 128KiB 上限)。

- 今天 = 无操作(xlings 还没读它);
- xlings E2b 落地当天自动生效,mcpp 的 DT_RPATH 仍只含 mcpp 决定的内容;
- 键名与语义在 `runtime_search.cppm` 里以常量声明一次,**不散落**。

---

## 7. M7 — 可观测性

| 载体 | 补什么 |
|---|---|
| `resolution.json` | `runtime_search` 数组:`[{path, origin}]`,**保序** |
| `mcpp why runtime` | `search:` 行按 origin 展开;binding 未声明时打印 `note` |
| `mcpp doctor` | `declared=false` 作为 **info** 呈现并说明影响范围 |

---

## 8. 测试

### 8.1 单测(`tests/unit/`)

| 文件 | 断言 |
|---|---|
| `test_runtime_search.cpp`(新) | `rank` 次序;`ordered` 去重且 stable;`is_machine_local` 逐值 |
| `test_subos_info.cpp`(补) | schema 高于支持值时**不失败**,填 note |
| `test_runtime_contract.cpp`(补) | 降级 binding 的 serialize↔deserialize 往返;contract hash 含 searchDirs |

### 8.2 e2e

| # | 文件 | 断言 | 防空转 |
|---|---|---|---|
| T1 | `219_runtime_search_farm_is_last.sh` | 可执行文件 `DT_RPATH` **最后一项**是 binding 的 farm | 读**生成物** |
| T2 | 同上 | `libc.so.6` 解析到载荷目录,不是 farm | 次序反了它先红 |
| T3 | `220_farm_only_needed_runs.sh` | 一个只有 farm 提供的 `NEEDED` 的产物 **rc=0 真的跑起来** | **唯一能戳破假绿的断言**;库从 farm∖载荷 差集里取,差集空则 **skip 并打印原因** |
| T4 | 同上 | 谁都提供不了的 `NEEDED` ⇒ 构建**变红**并指名 | 防止状态枚举没接到失败门 |
| T5 | `215`(扩) | `--mode system` 产物不含任何 `$MCPP_HOME` 路径 | 前缀扩到整个 home |
| T6 | `221_subos_without_info_still_builds.sh` | `subos_info` 缺失的 fixture 能 `mcpp build` | **不要求任何图形能力**,Windows/macOS 都要真跑到 |

**`# requires:` 只用 `run_all.sh` 真授予的能力**;T6 **不得**带 `elf`/`gcc`,否则它在
Windows 上被跳过,而 Windows 正是它要防的回归。

---

## 9. 文档

| 文件 | 改什么 |
|---|---|
| `docs/08-toolchain-internals.md` | 新增"运行期搜索闭包"一节:四种 origin、次序与理由 |
| `docs/02-pack-and-release.md` | `system` 档会剥机器本地路径并升为 host requirement |
| `docs/11-machine-output.md` | `resolution.json` 的 `runtime_search` 字段 |
| `docs/zh/` 对应件 | 同步 |
| 设计文档 | 顶部标注实施状态与 PR 号 |

---

## 10. 版本与 pin

- `mcpp.toml` `[package].version` + `src/version.cppm` `MCPP_VERSION` → **`2026.8.11.2`**(同一 commit)
- `src/xlings.cppm` `kXlingsVersion` → **最新 xlings**(实施时以 `xlings --version` / 索引为准)
- `.xlings.json` 的 bootstrap pin **本 PR 不动**(发布并进索引后才前移)
- `bash .github/tools/check_version_pins.sh` 必须过

---

## 11. 实施顺序

**唯一不可交换:M3 在 M4 之前。** 先加路径再补判据 = 在不会响的报警器上加功能。

```
M1 契约模块 → M2 binding(降级+farm) → M3 闭包判据 → M4 链接期 → M5 pack → M6 退出声明 → M7 观测 → 测试 → 文档 → 版本
```

M2 的降级半边(§2.2)与图形无关,是正在阻塞 Windows 用户的回归 —— 它在同一个 PR 里,
但**提交上独立成一个 commit**,以便必要时单独 cherry-pick。
