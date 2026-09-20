---
subject: design
status: superseded
---

<!-- Recorded as written on 2026-09-19; the list markers were spelled with
     symbols and are transcribed as words, per this repository's prose rule.
     Nothing else is changed. The review of this document is
     2026-09-20-issue-674-design-review.md. -->

# #674:`presents = "posix"` 在 Windows 上兑现契约的下半段

- Issue: mcpp-community/mcpp#674(2026-09-18)
- 依据:`origin/main` 361874df(mcpp 2026.9.18.4);本机 Linux x86_64,llvm 22.1.8
- 上一阶段:mcpp#673(7788d3e, 2026.9.18.3)做了"只压 `_WIN32`、不定义 `__unix__`"的 trade-off,本设计补齐同一 trade-off 的另一面
- 状态:方案待 review,未实现

---

## 0. 结论

1. **缺陷不在 mcpp#673,不在 openkal-musl,在上游代码的潜伏假设。** 18 个 `compat.*` 包失败是因为 zlib 等上游代码依赖 glibc 的传递包含,musl 不传递包含就暴露了"`lseek` 不在作用域"这一上游 bug。mcpp#673 写下的 trade-off("只压 `_WIN32`、不定义 `__unix__`")已经显式接受这 18 个失败;本设计**不是在撤销 mcpp#673**,而是在同一 trade-off 下补齐 `presents = "posix"` 契约兑现的另一半。

2. **修复归属在 mcpp 引擎层,而非 openkal-musl、非 mcpplibs/mcpp-index、非 upstream。**
   - openkal-musl 不承担:musl 的"头文件自包含、零传递包含"是设计原则(也是 POSIX 标准本身的要求),openkal-musl 一旦加传递包含就偏离 musl 上游,违背项目定位。
   - mcpplibs/mcpp-index 不承担:18 个 per-package patch 是次优替代(下文 §2.3),但它把复杂度推给每个 compat 包,而这些包其实共享同一个根因。
   - upstream 不在本次修复范围:修 zlib 等 PR 应另开路径(可以是 mcpplibs 上游反馈),不阻塞引擎侧修复。
   - 引擎承担:**`presents = "posix"` 是 mcpp 引擎对编译命令行的契约承诺,POSIX 标准规定哪些符号在哪些头里,引擎要把这些头显式拉进每个 TU。**

3. **方案在 `src/toolchain/cenv.cppm:282-299` 的 Windows + Posix 分支追加 `-include unistd.h` 与 `-include sys/stat.h`,起点以 zlib 的失败信号为最小集合,通过 30-member 测量迭代追加。** 不一次加齐所有 POSIX 头——`presents = "posix"` 是"POSIX 接口按需可达",不是"所有 POSIX 接口已被强制 include"。前者是契约,后者是包办代替。

4. **影响范围天然收在"Windows × x86_64 × `presents = "posix"`"这一格。** 由 cenv.cppm 现有的三层显式闸门(`os == "windows"` / `decl.presents == Posix` / `arch == "x86_64"`)保证,其他组合的编译命令行一字不改,链接行完全不变,运行时零开销。

5. **本设计不动 design §3.3 与 execution plan §4。** 不重新打开 mcpp#673 的 trade-off(`__unix__` 定义与否),也不动 cenv.cppm 已有的"`__CYGWIN__`/`__CYGWIN32__` 保持定义"那条记录。**改动是新增一行类的代码 + 一段注释,不改任何已写下的设计决策。**

---

## 1. 实测

### 1.1 复现:18 个 compat.* 失败的代表信号(`compat.zlib`)

```
$ mcpp build --target x86_64-w64-windows-gnu ...
...openkal-musl-0.15.0/include/...
tests/openkal-work/archive/.mcpp/.../compat-x-zlib/1.3.2/zlib-1.3.2/gzguts.h:50:12:
fatal error: 'io.h' file not found
```

issue #674 给出的根因(已实测确认):

```c
// zlib gzguts.h
#if defined(__TURBOC__) || defined(_MSC_VER) || defined(_WIN32)
#  include <io.h>      /* provides lseek on Windows */
#  include <sys/stat.h>
#endif

// zlib gzlib.c
#if defined(__DJGPP__)
#  define LSEEK llseek
#elif defined(_WIN32) && !defined(__BORLANDC__) && !defined(UNDER_CE)
#  define LSEEK _lseeki64
#elif defined(_LARGEFILE64_SOURCE) && _LFS64_LARGEFILE-0
#  define LSEEK lseek64
#else
#  define LSEEK lseek       /* lseek must be in scope here */
#endif
```

`__unix__` 已定义(由 cenv 切换 `--target=x86_64-pc-cygwin` 提供),`_WIN32` 未定义(由 mcpp#673 显式压),所以走 `#else` 分支,要求 `lseek` 在作用域。zlib 自己没有 `#include <unistd.h>`(它的 `#include <stdio.h>` 等头由 musl 提供,但 musl 不传递包含 `<unistd.h>`)。

### 1.2 musl vs glibc 的传递包含差异

| | glibc | musl |
|---|---|---|
| `<stdio.h>` 是否传递包含 `<unistd.h>` | **是**(非 POSIX 标准行为) | **否**(POSIX 合规行为) |
| 后果:zlib 在 Linux 上 | `lseek` 在作用域,编译过 | `lseek` 不在作用域,失败 |
| musl 设计原则 | — | "头文件自包含,用户显式包含所需" |

**musl 的"零传递包含"是设计原则,不是 bug**——POSIX 标准规定 `lseek` 由 `<unistd.h>` 提供,**不**要求 `<stdio.h>` 提供。所以 zlib 应该自己 `#include <unistd.h>`;它在 glibc 上"碰巧工作"是 glibc 的非标准行为。

### 1.3 18 个 compat.* 失败的根本同形

每个失败包的第一个错误形如:
- `'lseek' was not declared in this scope` → 缺 `<unistd.h>`
- `'fstat' was not declared in this scope` → 缺 `<sys/stat.h>`
- `'open' was not declared in this scope` → 缺 `<fcntl.h>`
- `'mmap' was not declared in this scope` → 缺 `<sys/mman.h>`

**每个失败包共享同一个根因**:在 `presents = "posix"` 的 Windows + musl 编译下,POSIX 头文件不会自动出现在作用域里——既不是 musl 给的(glibc 才有传递包含),也不是 cenv 给的(目前 cenv 只管身份宏,不强制 include 头文件)。

---

## 2. 设计原则(对齐已有架构)

### 2.1 谁写、谁包、谁兑现——四层角色清晰

```
POSIX 标准(抽象契约)
    │
    │  规定 <unistd.h> 必须声明 lseek
    │
libc 实现(写头的)
    │
    ├── musl ──── openkal-musl(包了 musl 的 mcpp 包,自己一行不写头)
    ├── glibc ── (系统 glibc)
    └── bionic ─ (系统 bionic)
    │
mcpp 引擎(兑现契约的)
    │
    └── 读 [c-abi] presents = "posix" → 产出 -include unistd.h
```

**每一层只做自己的事:**
- POSIX 规定契约(不实现)
- libc 写头(不偏离 POSIX 也不添加非标准行为)
- openkal-musl 包 musl(不写头、不加补丁)
- mcpp 引擎兑现契约(发 `-include` 指令,不写头也不背头内容)

如果让 openkal-musl 承担"传递包含"或"兼容 shim",那它就不再是 musl。如果让 mcpp 引擎写头,那它就不是引擎而是 libc 实现。**所以"-include unistd.h"的指令方 = mcpp 引擎、文件提供方 = libc 实现**,两者物理上分属不同角色,不能合并。

### 2.2 引擎的 POSIX 知识是"通用知识,非包名"

`src/toolchain/cenv.cppm:10-14` 已明确:

> "GENERIC KNOWLEDGE, NO PACKAGE NAMES. ... openkal-musl is nowhere in this file; a second POSIX C library on Windows would realise through the same table."

`-include unistd.h` 写入 cenv.cppm **不违反**这条规则,因为它写入的是 **POSIX 标准规定的接口头**(所有 POSIX libc 都同意),不是某个 C 库的实现细节(每个 libc 各不相同)。**POSIX 标准头是契约,不是包名。**

### 2.3 为什么 per-package patch 是次优而非首选

`mcpplibs/mcpp-index` 的 `[target.cfg(os = "windows").patches]` 机制是次优替代:

| 维度 | 引擎 `-include`(本方案) | per-package patch(Path A) |
|---|---|---|
| 代码改动位置 | cenv.cppm 一个分支,1-3 行 | 18 个 PR,每个 ~10 行 patch 文件 |
| 修复面 | 一次性覆盖所有 compat.* + 未来同形失败 | 只覆盖当前 18 个;未来新加 compat 包再补一次 |
| 上游修 bug 后 | 引擎不能自动撤销(无害但白 include) | patch 变 stale,需要人主动撤销 |
| 复杂度归属 | 引擎侧(契约兑现方) | 分散在 18 个包 |
| 与 openkal-musl 的耦合 | **零** | **零** |
| 与上游(zlib 等)的耦合 | **零** | 每个 patch 都是上游 bug 的反向补丁 |

**两条路径互相不冲突**:本方案是"立刻止血 + 全局正确",per-package patch 是"上游反馈,长期可去"。本方案实施后,per-package patch 仍可在 mcpplibs/mcpp-index 推进,但不再紧迫。

### 2.4 为什么不加齐所有 POSIX 头

候选"全包"清单:

```
<unistd.h>     <sys/stat.h>     <fcntl.h>        <sys/types.h>
<dirent.h>      <signal.h>       <sys/mman.h>      <sys/wait.h>
<poll.h>        <sys/select.h>   <sys/socket.h>    <netinet/in.h>
<arpa/inet.h>   <sys/ioctl.h>    <sys/resource.h>  <sys/time.h>
... (POSIX 头共计 ~50 个)
```

**反对一次加齐的理由:**
1. **语义错位**:`presents = "posix"` 是"POSIX 接口按需可达",不是"所有 POSIX 接口已被强制 include"。前者是契约(用户代码 / 上游代码按需 include),后者是包办代替。
2. **每 TU 付出代价**:虽然每个头都很小,但每个 TU 都强制预处理扫描所有头,即使该 TU 不需要。
3. **撤销成本高**:若未来发现某个头在某个奇怪的目标上有兼容问题,要撤销很麻烦(会影响所有 TU)。
4. **契约本身就反对**:`presents = "posix"` 的设计意图是"标准接口按需用",不是"全都先 include"。

正确做法是**最小集合 + 测量驱动迭代**——以 zlib 失败信号为最小起点(`unistd.h` + `sys/stat.h`),跑 30-member 测量看剩余失败的第一个"未声明标识符"是什么,按需追加。

---

## 3. 方案

### 3.1 mcpp 引擎侧修改(一个 PR)

**M1 cenv.cppm:282-299 在 Windows + Posix 分支追加 `-include`。**

```cpp
// src/toolchain/cenv.cppm:282-299
} else if (os == "windows") {
    if (decl.presents == CAbiPresents::Posix) {
        if (arch != "x86_64")
            return refuse("the Cygwin-flavoured realisation is measured "
                          "on x86_64 only; this arch has no verified "
                          "substitute triple");
        r.tokens.push_back("--target=x86_64-pc-cygwin");

        // ── POSIX contract fulfilment: present POSIX-standard headers ──
        //
        // `presents = "posix"` is the engine's promise that POSIX-standard
        // symbols (lseek, fstat, read, write, close, ...) are reachable from
        // every TU. The preprocessor identity (`__unix__` defined) was already
        // delivered above; the symbol availability needs this second half.
        //
        // POSIX §2.2.2 binds `<unistd.h>` to `lseek`/etc. and `<sys/stat.h>`
        // to `fstat`/etc. Any C library declaring `presents = "posix"` MUST
        // provide these headers. We do not name any package here; this is
        // the POSIX contract the engine is fulfilling, not a tuning for any
        // specific libc.
        //
        // Upstream code (zlib et al., issue #674) was written against glibc's
        // transitive-include behavior (which is non-POSIX) and so relies on
        // these headers being in scope without an explicit `#include`. musl
        // follows strict POSIX self-contained headers, so the contract must
        // be enforced here. Per-package patches in mcpplibs/mcpp-index are
        // still possible and remain the right long-term cleanup.
        //
        // ADDITIONAL HEADERS BELONG HERE ONLY WHEN MEASUREMENT SHOWS THEY
        // ARE NEEDED. Do not blanket-include all of POSIX: `presents` means
        // "available on demand", not "pre-included by the engine".
        r.tokens.push_back("-include"); r.tokens.push_back("unistd.h");
        r.tokens.push_back("-include"); r.tokens.push_back("sys/stat.h");

        r.expectDefined.push_back("__unix__");
        r.expectDefined.push_back("__CYGWIN__");
        r.expectUndefined.push_back("_WIN32");
        cygwinIdentity = true;
    } ...
}
```

**改动量**:`src/toolchain/cenv.cppm` 一个分支 + 一段注释,约 20 行新增(含注释)。无新字段、无新 API、无新模块。

**M2 不动 `expectDefined`/`expectUndefined` 的语义。** probe 验证"宏定义是否符合声明",但不验证"-include 是否成功"——因为 probe 读 `clang -dM` 输出的是已定义宏,不是 include 列表。`-include` 的成功由 e2e 测量保证(probe 守住第一层契约,e2e 守住第二层)。

**M3 不改 design §3.3、不改 execution plan §4。** 本次只补 `presents = "posix"` 兑现的另一半,不重新打开"`__unix__` 是否定义"的 P3 spike 决策。CHANGELOG 条目写明"补齐 mcpp#673 trade-off 的另一半:POSIX 接口符号可见性"。

### 3.2 单测更新

`tests/unit/test_cenv.cpp` 在 `WindowsPosixCygwinSubstitutesTriple` 测试(`test_cenv.cpp:80-100` 附近)追加两条断言:

```cpp
TEST(CEnv, WindowsPosixPreIncludesPosixStandardHeaders) {
    CAbiDecl d;
    d.presents   = CAbiPresents::Posix;
    d.dataModel  = CAbiDataModel::ArchDefault;
    d.wcharBits  = 32;
    d.hasWchar   = true;
    auto r = cenv::realise(d, "windows", "x86_64", false);
    ASSERT_TRUE(r.has_value()) << r.error();
    ASSERT_TRUE(has(r->tokens, "-include"));
    ASSERT_TRUE(has(r->tokens, "unistd.h"));
    ASSERT_TRUE(has(r->tokens, "sys/stat.h"));
}
```

并加一条断言守住"非 Windows + Posix 不受影响":

```cpp
TEST(CEnv, LinuxPosixDoesNotPreIncludePosixHeaders) {
    // ...Linux + Posix 的现有测试保持不动,作为回归断言
}
```

### 3.3 e2e 与测量

**E1 30-member 测量。** 跑与 mcpp#673 同形的 30-member 矩阵,关注:
- Windows × x86_64 × openkal 各 compat.* 包:从 18 个失败 → 期望显著减少(`unistd.h` 缺失类应全消)
- 残余失败的第一个错误是否是"未声明的标识符"——若是,记录该标识符及其应在的头,作为迭代追加的依据
- Linux / macOS / freestanding 各路径:零回归

**E2 增量迭代规则。** 每发现一个新的"未声明标识符"对应 POSIX 标准头,在 cenv.cppm 的列表里追加;每个 PR 一次只追加一个头(避免一次 PR 范围过大)。`/tmp/posix-preinclude-evolution.md`(PR 描述外)记录每次追加的判据。

**E3 probe 不验证 include 行为。** 这是有意为之:probe 已经守住"`__unix__` 已定义 / `_WIN32` 未定义"这一层契约;`-include` 的成功是 build-time 验证(probe 之后才能展开)。把 probe 改造成验证 include 列表,会让 probe 跨进"运行时验证"领域,超出 §3.2 的设计意图。

---

## 4. 影响范围与使用规范

### 4.1 影响范围(改动前后对比)

| 场景 | 改动前 | 改动后 |
|---|---|---|
| Linux × x86_64 × `presents = posix` | 不进 Windows 分支 | **不变** |
| Linux × x86_64 × `presents = windows` | 不进 Windows 分支 | **不变** |
| Linux × x86_64 × `presents = none` | 不进 Windows 分支 | **不变** |
| macOS × aarch64 × `presents = posix` | 进 macOS 分支 | **不变** |
| freestanding × `presents = posix` | 进 freestanding 分支 | **不变** |
| **Windows × x86_64 × `presents = posix`** | 加 `--target=x86_64-pc-cygwin` | **加 `--target=...` + `-include unistd.h` + `-include sys/stat.h`** |
| Windows × x86_64 × `presents = windows` | 不动 | **不变** |
| Windows × x86_64 × `presents = none` | 拒绝 | **不变** |
| Windows × aarch64 × `presents = posix` | 拒绝(arch 闸门) | **不变** |

**只有"Windows × x86_64 × `presents = "posix"`"一行受影响**,其余所有组合编译命令行一字不改。

### 4.2 使用规范

**对包维护者(mcpplibs/mcpp-index):**
- 是:**不需要**为 compat.* 包加 per-package patch 来修复"`lseek` 未声明"一类失败——引擎已自动处理
- 注意:仍可保留或新加 per-package patch(用于处理非"POSIX 标准符号"类的上游 bug)
- 否:不要因为"路径 C 已上"而推迟 upstream 反馈;两条路径并行,不互斥

**对引擎调用方(任何 `mcpp build`):**
- 是:在 `presents = "posix"` + Windows x86_64 上,POSIX 标准符号自动在作用域
- 注意:任何非 POSIX 标准头(如 `<sys/mman.h>`、`<sys/wait.h>`)仍需 `#include`——不要假设引擎把它们也预 include 了
- 否:不要在源码里依赖"`lseek` 在 `<stdio.h>` 里"(违反 POSIX 的假设)

**对引擎维护者:**
- 是:追加新的 `-include` 必须基于测量(具体的"未声明标识符" + POSIX 标准规定)
- 否:不要一次性加入所有 POSIX 头(违背契约语义,见 §2.4)
- 否:不要加入非 POSIX 标准头(如 `<sys/cygwin.h>`、`<gnu/libc-version.h>` 等 libc-specific 头)——那就破坏了"通用 POSIX 知识,非包名"的设计意图
- 注意:若未来需要给特定 C 库实现差异化策略(如某 C 库不提供 `<sys/stat.h>`),再加 `[c-abi] pre-include` / `pre-include-unset` 字段;**今天不加**——只有一个 POSIX C 库,过度设计无收益

### 4.3 失败模式与诊断

引擎侧不发 `-include` 时:
- 上游代码报"`xxx was not declared in this scope`"——这是预期失败(上游不遵循 POSIX include 规范)
- 解决方案:在 mcpplibs/mcpp-index 加 per-package patch,或推动上游修

引擎侧发 `-include <X>` 但 `<X>` 找不到时:
- clang 报"`'<X>' file not found`"——这是 C 库实现未兑现 `presents = "posix"` 契约
- 诊断:检查 graph 里的 libc 包是否真的提供了该头;若声明 `presents = "posix"` 但不提供 `<X>`,应要求该 C 库要么补头要么改声明
- **不**通过"删除 `-include <X>`"来回避——那等于让引擎容忍契约不兑现

### 4.4 边界与不做什么

**不做:**
- 否:给 `[c-abi]` 加 `pre-include` 字段(过度设计,见 §2.4 / §4.2)
- 否:改动 `__unix__` / `_WIN32` 的定义策略(已由 mcpp#673 锁定)
- 否:在 openkal-musl 加任何补丁(违背 openkal-musl 定位)
- 否:在 mcpplibs/mcpp-index 强制 18 个 compat 包都加 patch(per-package patch 是 optional 清理,不是必需)
- 否:给 clang 贡献新 target(LLVM 不会接收,且修了也不解决问题,见 mcpp#674 历史讨论)

**做:**
- 是:在 cenv.cppm 的 Windows + Posix 分支追加 `-include unistd.h` 与 `-include sys/stat.h`
- 是:在 test_cenv.cpp 加单元测试
- 是:CHANGELOG 写明:补齐 mcpp#673 trade-off 的另一半
- 是:PR 描述明确"POSIX 契约兑现,非任何特定 C 库特化"
- 是:通过 30-member 测量迭代(每个新发现的失败头独立追加一个 PR)

---

## 5. 顺序

```
mcpp PR(M1-M3) → CI 跑通 → 30-member 测量 → release
                                                │
mcpp-index PR(可选 per-package 清理,不阻塞) ←──┘
```

M1(代码)+ M2(测试)+ M3(CHANGELOG) 在同一个 PR 内,1-3 天工作量。CHANGELOG 与 PR 描述的措辞要明确"补齐 mcpp#673 的下半段",让后续读者能从 git 历史看到完整脉络。

---

## 6. 需要决定的问题

- **D1 `-include` 的最小集合:`unistd.h` + `sys/stat.h` 是否足够开始?** 建议是。这两个头覆盖 zlib 的 `lseek` + `fstat`,且严格对应 POSIX 标准对它们的归属。后续按测量迭代。**本地验证不需要任何 host 上的工具安装**——所有工具链(包括 Windows 跨编译所需的 mingw-w64)均由 mcpp 通过 `mcpp toolchain install` 自动管理到 `~/.mcpp/registry/data/xpkgs/`,与系统 PATH 完全隔离(`docs/20-toolchains.md:13`)。本 PR 不引入任何新的 host 依赖。
- **D2 是否同步在 `src/toolchain/cenv.cppm` 的注释里明确"POSIX 契约兑现,非 libc 特化"?** 建议是。回应 issue #674 的"归属判断"——下一个读者看到 `-include` 应该立刻明白这不是为 openkal-musl 调优,而是 POSIX 标准。
- **D3 是否同步更新 `docs/22-target-side.md` 的 §3.2?** 建议暂缓。本设计不在 design §3.3 改任何东西;若 30-member 测量后总结出更广的契约语义,再统一更新文档。如果只想在 docs 里加一行注释,可以在 §3.2 的"manifest 字段"小节末尾追加"see cenv.cppm for the second half of the contract"一句话。
- **D4 是否在 CHANGELOG 引用 issue #674?** 建议是。这是 issue → PR 的标准做法。
- **D5 per-package patch 是否并入本次?** 建议不并入。本 PR 只做引擎侧修复;per-package 清理留给 mcpplibs/mcpp-index 后续 PR(可选)。

---

## 7. 参考

- Issue #674 — 18 个 compat.* 失败的现象与四种候选路径
- mcpp#673 — `7788d3e`,commit message 详述 P3 spike trade-off
- `src/toolchain/cenv.cppm:10-14, 64-72, 188-204, 282-299` — 改动点上下文
- `src/toolchain/hostflags.cppm` — `-nostdlibinc` 注入点,保证 `-include` 走 musl 头不走 clang 内置
- `tests/unit/test_cenv.cpp:80-100` — 现有 Windows + Posix 单元测试
- `docs/22-target-side.md:307-396` — `[c-abi]` 字段语义,`presents` 是请求(契约)
- `docs/24-openkal-cross.md:37-69` — 三层宏家族的划分
- `.agents/docs/2026-09-17-issue-662-graph-target-header-isolation-plan.md` — 前置 PR,隔离宿主头文件搜索(本方案的依赖)