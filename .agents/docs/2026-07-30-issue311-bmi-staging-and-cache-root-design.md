# BMI staging 原语 + BMI 缓存根收敛 — 设计

日期：2026-07-30
状态：设计定稿，待实施
关联：#311（Windows 上 clangd 导致 mcpp build 假失败）
建议目标版本：**2026.7.30.1**

---

## 1. 摘要

issue #311 报的症状真实：Windows 上 clangd 把 `pcm.cache/std.pcm` 内存映射住，mcpp 的
`powershell Copy-Item -Force` 原地覆写同一文件 → Windows error 1224
（`ERROR_USER_MAPPED_FILE`）→ ninja edge 失败 → `error: build failed`。

但这不是一个 bug，而是**三个互相放大的缺陷**叠在同一条链上：

| 编号 | 缺陷 | 层级 |
|---|---|---|
| **D1** | std BMI staging 用 `Copy-Item -Force` **原地覆写**：非原子、无重试、不判内容等价 | 构建后端 |
| **D2** | `default_cache_root()` 是 home 解析器的第三份拷贝：Windows 无 `USERPROFILE` 分支 ⇒ 缓存根落到 **cwd**，且与 `cfg.bmiCacheDir` 分家 | 路径解析 |
| **D3** | staging edge 无 `restat`⇒ 重新 stage 必级联全量重编；失败诊断无可执行信息 | 构建后端 / 诊断 |

D2 是**触发器**：它让"内容完全相同但 staging edge 变脏"成为常态，从而把 D1 这个平时藏着的
脆弱点顶到用户面前；D3 决定了它爆出来时有多难懂、多贵。

本设计只做 D1/D2/D3 的收敛，**不做**"clang/MSVC 引用即用、彻底取消拷贝"（见 §7，单独批次）。

---

## 2. 现状机制（逐段核过的链条）

### 2.1 mcpp 主动把这个文件交给 clangd

```cpp
// src/build/flags.cppm:357-362
if (!traits.stdBmiUsePrefix.empty() && !plan.stdBmiPath.empty()) {
    std_module_flag = std::string(traits.stdBmiUsePrefix)      // clang: " -fmodule-file=std="
                    + escape_path(staged_std_bmi_path(plan));  // 绝对路径，指向 build dir 内的副本
}
// src/build/flags.cppm:369-382
prebuilt_module_flag = std::string(traits.bmiSearchPrefix)     // " -fprebuilt-module-path="
                     + escape_path(plan.outputDir / traits.bmiDir);
```

这两个 flag 一起进 `compile_commands.json`（`src/build/compile_commands.cppm:127-170`，
`arguments` 数组直接来自同一个 `flags.cxx`）。`flags.cppm:371-381` 的注释写得很清楚：这里
刻意用绝对路径，**就是为了让 clangd 能解析** `import std;`。

于是：clangd 解析任何 `import std;` 的 TU → 前端 ASTReader 加载该 `.pcm` → LLVM MemoryBuffer
对这种体量（本机实测 GCC 侧 `std.gcm` = 31,466,112 字节；clang 侧同量级）走 mmap → Windows
上即 `CreateFileMapping`，文件带 user-mapped section，且 clangd 会把 AST/preamble 缓存住。

### 2.2 mcpp 同时在原地覆写它

```cpp
// src/build/ninja_backend.cppm:411-419
append("rule cp_bmi\n");
if constexpr (mcpp::platform::is_windows) {
    append("  command = powershell -NoProfile -Command \"Copy-Item -Force '$in' -Destination '$out'\"\n");
} else {
    append("  command = mkdir -p $$(dirname $out) && cp -f $in $out\n");
}
append("  description = STAGE $out\n\n");
```

```cpp
// src/build/ninja_backend.cppm:786-812（四条 staging edge）
build pcm.cache/std.pcm        : cp_bmi <cacheRoot>/<fp>/pcm.cache/std.pcm
build obj/std.o                : cp_bmi <cacheRoot>/<fp>/std.o
build pcm.cache/std.compat.pcm : cp_bmi ... | pcm.cache/std.pcm
build obj/std.compat.o         : cp_bmi ...
```

`Copy-Item -Force` 打开目标写入 ⇒ 目标带 mapped section ⇒ 1224。

**POSIX 分支天然免疫**，这也是为什么 CI 全绿：GNU `cp -f` 在目标打不开时会 unlink 重建，
而 POSIX 本身也允许覆写被 mmap 的文件。本机对照实验（scratch 工程，GCC）：

```
$ chmod 444 target/x86_64-linux-gnu/<fp>/gcm.cache/std.gcm
$ touch ~/.mcpp/bmi/<fp>/gcm.cache/std.gcm          # 让缓存侧变新，把 edge 弄脏
$ mcpp build -v
[1/3] mkdir -p ... && cp -f ~/.mcpp/bmi/<fp>/gcm.cache/std.gcm gcm.cache/std.gcm   ← 成功
[2/3] g++ ... -c src/main.cpp -o obj/main.o                                        ← 级联重编
[3/3] g++ obj/main.o obj/std.o -o bin/demo311
$ ls -l target/.../gcm.cache/std.gcm
-rw-rw-r-- ... 31466112                              ← 权限被 unlink-重建抹掉
```

这个实验同时验证了 D3 的级联：staged BMI 是每个 importer 的 implicit input
（`ninja_backend.cppm:964/971`），`cp -f` 不保留 mtime、rule 又没有 `restat = 1`，
所以**只要重新 stage 一次，所有 `import std;` 的 TU 全部重编**——即使字节完全相同。

### 2.3 失败如何呈现给用户

edge 失败 → ninja 非零 → `execute.cppm:437-448`：

- `is_stale_ninja_failure()`（`execute.cppm:207-219`）不匹配 → 不走重生成；
- `mcpp::ui::error("build failed")`；
- `filter_ninja_output()`（`ninja_backend.cppm:323-345`）丢掉 `FAILED: <target>` 行；
  `powershell ...` 不匹配任何编译器前缀（`command_prefixes()` 只收 cxx/cc/ar/scan_deps，
  `ninja_backend.cppm:278-291`；`read_ninja_command_prefixes()` 同，`execute.cppm:181-205`）
  ⇒ **命令行被保留、失败目标被丢掉**。

用户最终看到的就是 issue 里贴的那三行：一句 `error: build failed` + 一条 PowerShell 命令 +
一句 Windows 错误。既看不出这是 BMI staging（不是编译错误），也没有任何"该怎么办"。

### 2.4 报告中不成立的一处

> "编译和链接实际已成功，二进制已生成，只是 post-build copy 失败。"

冷构建下不成立：staged std BMI 是所有 importer 的 implicit input，copy 失败时它们根本不会
跑，这是**真·阻塞失败**。这句话只在增量场景下成立——而制造该场景的正是 D2：

```
换个 cwd 跑 mcpp build
  → default_cache_root() = <新 cwd>/.mcpp-bmi        （D2）
  → 该目录下无 std BMI → 重新 precompile
  → build.ninja 里 staging edge 的 $in 路径变了
  → build dir 仍是 target/<triple>/<fp>（只由 fingerprint 定名，不含缓存根）
  → 于是"其它全 up-to-date，只有这条 copy edge 脏了"
  → Windows 上它失败，而上一次的二进制还躺在盘上
```

report 是 AI 润色的（作者自己标注了），因果叙述串了，但症状与最终状态自洽。

---

## 3. D2：缓存根解析为什么会分家

```cpp
// src/toolchain/stdmod.cppm:179-187 —— 来自 v0.0.1（git log -L179,187 → 92f1335），一字未改
std::filesystem::path default_cache_root() {
    if (auto* e = std::getenv("MCPP_HOME"); e && *e) return path(e) / "bmi";
    if (auto* e = std::getenv("HOME");      e && *e) return path(e) / ".mcpp" / "bmi";
    return std::filesystem::current_path() / ".mcpp-bmi";
}
```

而正式解析器后来补齐了两件这里没有的事（`src/config.cppm:313-353`）：

1. **Windows `USERPROFILE`**（`default_mcpp_home()`，:315-318）；
2. **self-contained 探测**：`<root>/bin/mcpp` 形态即认 `<root>` 为 home，并排除
   `target/` 与 `data/xpkgs/` 两种祖先（`home_dir()`，:329-351）。

且 `cfg.bmiCacheDir = cfg.mcppHome / "bmi"`（:510）**已经是**这个缓存的规范位置。
同一逻辑在仓库里一共三份：`config.cppm:313`、`stdmod.cppm:179`、`prepare.cppm:2826`
（git dep 缓存的内联 lambda）。

### 3.1 后果

| 场景 | dep BMI 缓存（`cfg.bmiCacheDir`） | std BMI 缓存（`default_cache_root()`） |
|---|---|---|
| `MCPP_HOME` 已设 | `$MCPP_HOME/bmi` | `$MCPP_HOME/bmi` ✅ |
| Linux + `xlings install`（xpkgs 形态） | `~/.mcpp/bmi` | `~/.mcpp/bmi` ✅ |
| release tarball 解包（`<root>/bin/mcpp`） | `<root>/bmi` | `~/.mcpp/bmi` ❌ 分家 |
| **Windows PowerShell/cmd（无 `HOME`）** | `%USERPROFILE%\.mcpp\bmi` | **`<cwd>\.mcpp-bmi`** ❌ |

issue 日志里出现 `.mcpp-bmi/<hash>/pcm.cache/std.pcm` 本身就是这个第三兜底分支的铁证——
纯靠想象编报告的人更可能写成 `~/.mcpp/bmi`。

派生问题：

- **cwd 相关**：从子目录跑 = 另一个缓存根 = 重编一次 std（几十 MB、十几秒）；
- **工程被污染**：`mcpp new` 生成的 `.gitignore` 只有 `target/`
  （`src/scaffold/create.cppm:284-287`），所以 `.mcpp-bmi/` 会以 untracked 状态躺在用户
  工程里。mcpp 自己的仓库靠 `.gitignore` 里手写的 `/.mcpp-bmi/` 掩盖了这个papercut；
- **清理/诊断指错地方**：`mcpp clean --bmi-cache`（`execute.cppm:1130`）、
  `doctor`（`doctor.cppm:155/192`）、`bmi_cache/maintenance.cppm:53/167` 全用
  `default_cache_root()`，而 `config.cppm:219-220` 的重置路径删的是 `cfg.bmiCacheDir`
  ——两边可能根本不是同一个目录。

### 3.2 为什么名字是 `.mcpp-bmi` 而不是 `.mcpp/bmi`

没有设计意图，就是 v0.0.1 时随手写的平级名字。工程内 `.mcpp/` 目录**早已是既有约定**
（per-project xlings sandbox，`config.cppm:122/128`），`config.cppm:321` 的同类兜底也写的是
`current_path() / ".mcpp"`。所以工程内兜底的正确形态是 `<projectRoot>/.mcpp/bmi`。

---

## 4. 设计

### S1 — `mcpp stage`：一个可判等、可重试、原子的 staging 原语

新增内部子命令（形态对齐既有的 `mcpp dyndep`：`cli.cppm:496-511` +
`cli/cmd_build.cppm:187`，同样"由 ninja 调用、不进 help 主屏"）：

```
mcpp stage --output <dst> <src>
```

语义表（**顺序即优先级**）：

| 步 | 条件 | 行为 |
|---|---|---|
| 1 | `src` 不存在 | 报错退出 1（这是真 bug，不容忍） |
| 2 | `dst` 存在 且 `size(dst) == size(src)` 且**内容逐字节相等**(默认;`--verify size` 时只比 size) | **一个字节都不写,也不动任何时间戳**;退出 0 |
| 3 | 否则 | 写 `dst.tmp.<pid>`（同目录）→ `rename` 覆盖 `dst` |
| 4 | 步 3 失败 | 退化为原地覆写（`copy_file` overwrite）——赢下"可写但不可删"的持有者形态 |
| 5 | 步 3+4 都失败 | 退避重试 3 次（100/300/900 ms）后仍失败 → 结构化诊断 + 退出 1 |

关键判据（**为什么步 2 是安全的，而不是"偷懒跳过"**）：

> build dir 与缓存目录用的是**同一个 fingerprint**（本机可见：
> `target/x86_64-linux-gnu/284e9f...` ↔ `~/.mcpp/bmi/284e9f...`，均来自
> `prepare.cppm:3612` 传入的 `fp.hex`）。fingerprint 已覆盖编译器身份/版本/target
> triple/stdlib/std 源哈希/标准与方言 flag（`stdmod.cppm:88-112` 的 metadata 同源）。
> 同 fp ⇒ staged BMI 与缓存 BMI 语义等价 ⇒ 已存在即正确。

这不是新发明的判据，**dep BMI 那条路径已经在这么做**了：

```cpp
// src/bmi_cache.cppm:57-60
// Copy missing cached files into projectTarget/{bmiDirName,obj}. Existing
// project outputs are left untouched: BMIs may differ byte-for-byte between
// equivalent builds, and overwriting them would dirty downstream modules.
```

也就是说：**std staging 是全仓库唯一强制覆写的那一处**，这个不对称本身就是缺陷。
S1 把它拉回一致。

**内容校验是默认,不是开关**(这一条被 Windows CI 纠正过一次)。初稿让 size 相等即视为已
staged,理由是 fp 判据;但同一条 rule 还搬 **DLL**(`runtimeDeployFiles`、以及 Windows 上的
`runtime_alias`),而 PE 的节对齐让「真的重建了、大小却一模一样」十分常见 —— size-only 会把
一个过期的 DLL 留在 build dir 里。逐字节比较则**无条件正确**:内容相同就是不需要写。

成本可接受,因为这个判断只在 ninja 已经认定 edge 脏了才会跑(罕见),此时多读两遍 31 MB 换
的是正确性。`--verify size` / `MCPP_STAGE_VERIFY=size` 保留给「调用方确知源是 fp 作用域」的
快路径。选逐字节比较而不是 hash:同样的 I/O 成本,但没有碰撞面,且能提前退出。

**但 verify 档位是 per-edge 的,不是全局一档**(这一条也是 Windows CI 逼出来的)。判等要读目标
就必须**打开**它,而持有者可以连读都不给:e2e 171 的 PowerShell 用 `FileShare.None` 映射,
`same_content` 连 open 都失败 → 判不出等价 → 去写 → `ERROR_SHARING_VIOLATION(32)` → 构建挂。
而 **size 来自目录元数据,不需要 open**,所以:

| edge | verify | 理由 |
|---|---|---|
| std BMI / std.o / std.compat.* | `--verify size` | fp 作用域 ⇒ 等长即等价;且能在「连读都被拒」的持有下照样跳过 |
| Windows DLL 部署 / `runtime_alias` | 内容(默认) | 源不是 fp 作用域,PE 节对齐让等长-不同内容很常见 |

也就是说 #311 那条真正的路径(clangd 映射 std BMI)现在**连排他锁都能扛**,而可能过期的
DLL 仍然逐字节把关。ninja 里用一个 per-edge 变量 `$verify` 承载(注意 ninja 会裁掉变量值的
尾部空白,所以空格必须留在 rule 的 command 串里)。

**为什么步 2 连时间戳都不碰**（这一条实测修正过一次）：

| 做法 | 下游是否被级联 | 之后 edge 状态 |
|---|---|---|
| 跳过 + 不动任何时间戳 | **否** ✅ | 一次 stage 后即干净（下次 `no work to do`）✅ |
| 跳过 + mtime := `src` 的 mtime | **是** ← 实测重现：`main.cpp` 被重编 | 干净 |
| 跳过 + mtime := now | 是 | 干净 |

ninja 的 restat 语义是"**mtime 未被命令改变**的输出视为从未需要构建"。只要我们动了 mtime
（哪怕只是对齐到 src），ninja 就认为输出被更新了，级联照旧发生。**任何形式的 mtime 触碰都是
错的。**

第一行的两个好处是实测确认的（demo 工程，`touch` 缓存侧 BMI 后连续两次构建）：

```
$ touch ~/.mcpp/bmi/<fp>/gcm.cache/std.gcm && mcpp build -v
[1/3] mcpp stage --output gcm.cache/std.gcm ~/.mcpp/bmi/<fp>/gcm.cache/std.gcm
    Finished release [optimized] in 0.00s        ← main.cpp 没有被重编
$ mcpp build -v
ninja: no work to do.                            ← edge 也没有一直脏着
```

第二次就干净，是因为 restat 分支下 ninja 会把 build log 里该输出的记录推进到输入的时间，
所以"不动 mtime"既不级联、也不会每次构建都重跑——比设计初稿预期的更好。

失败诊断文案（步 5）——必须点名"是什么文件、谁可能持有、怎么办"：

```
error: cannot stage BMI into the build directory
  file:  <dst>
  from:  <src>
  os error: 1224 (the requested operation cannot be performed on a file with
            a user-mapped section open)
hint: another process has this file memory-mapped or open. The usual holder is
      clangd (mcpp writes this exact path into compile_commands.json so clangd
      can resolve `import std;`), an editor/IDE indexer, or antivirus.
      Close the editor or restart clangd, then re-run `mcpp build`.
```

### S2 — 一个 home 解析器：新叶模块 `mcpp.home`

新建 `src/home.cppm`（`export module mcpp.home;`），只依赖 `std` + `mcpp.platform`：

```cpp
namespace mcpp::home {
std::filesystem::path root();       // = 现 config.cppm:324 home_dir() 逐字搬迁
std::filesystem::path bmi_root();   // = root() / "bmi"
}
```

- `config.cppm` 的 `home_dir()`/`default_mcpp_home()` 删除，`cfg.mcppHome = mcpp::home::root()`，
  `cfg.bmiCacheDir = mcpp::home::bmi_root()`；
- `stdmod.cppm::default_cache_root()` 改为 `return mcpp::home::bmi_root();`
  （工程内兜底随之变成 `<cwd>/.mcpp/bmi`，与 `config.cppm:321` 同形）；
- `prepare.cppm:2826-2832` 的内联 lambda 换成 `mcpp::home::root()`。

模块图无环：`mcpp.config` 的 import 闭包是 `{libs.toml, pm.index_spec, xlings→{pm.compat,
platform, log}, platform, log, fallback.{xlings_binary, config_migration, install_integrity}}`，
其中没有任何 `mcpp.toolchain.*`；反向新增 `mcpp.toolchain.stdmod → mcpp.home` 也不成环
（`mcpp.home` 是叶）。

命名说明：不叫 `mcpp.paths` / `mcpp.utils`——按仓库约定（口袋模块名禁用），这个模块只有
一个职责：解析 MCPP_HOME 及其子目录。

### S3 — `restat = 1` + rule 更名

staging rule 加 `restat = 1`。这是 S1 步 2"不写字节"能真正省掉级联的前提：ninja 在 edge
跑完后复核输出，mtime 未变则把下游标记为干净。

顺手把 rule 名从 `cp_bmi` 改成 `stage_file`：它早就不只搬 BMI 了——Windows 运行期 DLL
部署复用的是同一条 rule（`ninja_backend.cppm:1077-1083`）。**DLL 部署是同一类失败的第二个
受害者**（往正在运行的进程/调试器已加载的 DLL 上 `Copy-Item -Force` 同样失败），S1 一并治好。

还有**第三个**:`rule runtime_alias`(`ninja_backend.cppm:758-764`)在 Windows 上也是一条
`Copy-Item -Force` —— PE 没有 soname 符号链接,别名就是刚建出来的 DLL 的副本,同一危险类。
Windows 分支一并改走 `$mcpp stage`(+ `restat`),**POSIX 分支保持 `ln -s` 不变**:那里符号
链接是语义而不只是写法(改成拷贝会改变打包产物)。这一处是 Windows CI 抓出来的 —— 初稿的
「`Copy-Item` 归零」自查项在本地(Linux)恒为真,永远不会失败。

另有一处必改：`mcpp = <exe>` 这个 ninja 变量目前只在 `if (dyndep)` 里绑定
（`ninja_backend.cppm:403-409`）。新 rule 在**所有**配置下都要用它，必须把绑定提到该
条件之外，否则 GCC 非 dyndep 路径会生成 `$mcpp` 为空的命令。

### S4 — 诊断收敛

1. `command_prefixes()`（`ninja_backend.cppm:278-291`）加入 `mcpp_exe_path()`，
   `read_ninja_command_prefixes()`（`execute.cppm:181-205`）的白名单 key 加 `"mcpp"`：
   这样过滤器会吃掉**被回显的命令行**、保留 mcpp 自己的诊断正文——正好是我们想要的。
2. `filter_ninja_output()` 对 `FAILED:` 不再一刀切丢弃：保留目标名（改成一行
   `failed: <target>`）。当前把"哪个输出失败了"整行丢掉，是 #311 里那段输出读不懂的
   直接原因之一。

### S5 — 兜底路径的可见性（低成本 papercut）

1. `mcpp new` 的 `.gitignore` 模板（`scaffold/create.cppm:284-287`）加 `.mcpp/`
   ——它同时也是 per-project xlings sandbox 的目录，本来就该忽略；
2. `doctor` 若在 cwd/工程根发现遗留 `.mcpp-bmi/`，打一行提示"legacy BMI cache, safe to
   delete"（不自动删）。

---

## 5. 不采纳 / 需要纠正的方案

| 方案 | 判定 | 理由 |
|---|---|---|
| issue 建议 1：copy 前比内容 | **采纳**（= S1 步 2） | 方向正确；实现上先比 size，hash 作为可选 |
| issue 建议 2：copy 失败降级为 warning | **拒绝** | staged BMI 缺失/过期时，后续要么报莫名的 `module 'std' not found`，要么拿旧 BMI 配新 `std.o` 链接。这是把硬失败换成静默错误。只有"已验证等价"时跳过才安全，而那时它是 no-op success，不是 warning |
| issue 建议 3：`Move-Item` + rename 原子替换 | **部分采纳，但不是对症药** | Windows 上删除/改名带 mapped section 的文件同样返回 1224（rename-over 需要目标可删）。它只能挡杀软扫描造成的瞬时 sharing violation(32)。S1 把它放在步 3（先试），并保留步 4/5 |
| 内容寻址的 staged 文件名（`std-<hash>.pcm`，永不覆写） | 不采纳 | build dir 已按 fingerprint 隔离，同 fp 下"存在即正确"已足够；再加一层命名只会让产物堆积与 flag 生成复杂化 |
| 完全改为 prepare 阶段进程内 staging、取消 ninja edge | 不采纳（本批次） | 更干净，但会丢掉"staged 文件被手动删除时 ninja 自动补齐"这条鲁棒性，且 `mcpp run/test` 的 cached-ninja 快路径不经 prepare。留作 §7 的一部分 |

---

## 6. 风险与验证

| 风险 | 缓解 |
|---|---|
| size 相等但内容不同(S1 步 2 误跳过) | **默认逐字节比较**,单测双向锁住(默认必拷贝 / `--verify size` 才跳过) |
| `restat = 1` 引入意外的"永不重建" | 单测锁 rule 文本；e2e 正例：改 std 源/换 toolchain ⇒ fp 变 ⇒ 新 build dir，不受影响 |
| 收敛缓存根导致老用户一次性重编 std | 只影响 self-contained 与 Windows 两种形态；一次 10–60 s；CHANGELOG 写明，doctor 提示遗留目录 |
| `mcpp` 变量提取出 `if (dyndep)` 后 ninja 文本变化 | `tests/unit/test_ninja_backend.cpp` 断言两种配置（dyndep on/off）下都绑定 |
| Windows 行为无法在 Linux 验证 | e2e 用 `MemoryMappedFile::CreateFromFile` 在 Windows CI 上**不依赖 clangd**地精确复现（见实施计划 P5） |

必须新增的验证（详见实施计划）：

- **反级联**（Linux 可跑）：`touch` 缓存侧 BMI → `mcpp build -v` 中**不得**出现 `main.cpp` 的
  编译行。这条正是今天实测会失败的行为。
- **锁定目标**（Windows only）：PowerShell 映射住 staged BMI → `mcpp build` 必须**成功**
  （走 S1 步 2）；内容确实不同时必须失败并带 hint 文案。

---

## 7. 后续批次（不在本设计范围）

**零拷贝：clang/MSVC 直接引用缓存里的 BMI。**
`-fmodule-file=std=<path>` / `/reference std=<path>` 接受任意绝对路径，`std.o` 也可以直接
进链接行。真正需要文件落在 build dir 的只有 **GCC**（`import std;` 默认按 cwd 相对的
`gcm.cache/std.gcm` 查找，除非改用 `-fmodule-mapper`）。收益：每个 build dir 少几十 MB
I/O，这类锁冲突结构性消失。代价：build dir 不再自包含（缓存被 GC/`clean --bmi-cache`
时会踩空），且要重新审计 `std.compat` 的 BMI 内记录路径。值得做，但要单独一批 + 四平台
e2e，不能和本次修复混在一个 PR 里。
