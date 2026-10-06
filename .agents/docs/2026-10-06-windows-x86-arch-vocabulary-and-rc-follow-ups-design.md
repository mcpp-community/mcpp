---
subject: design
status: active
---

# 2026.10.5.3 发布方案：32 位 x86 的架构词汇、资源编译器的识别与增量（#776 后续）

- 日期：2026-10-06。状态：修订 2，review 通过（O1–O3 按建议），实现于 mcpp 2026.10.5.3。本文是 **2026.10.5.3 的统一发布方案**，所有项目放在一个发布 PR 中完成（§7）。
- 基线：`main` @ `a0c40ec3`（已合入 #776）。
- 来源：`.agents/reviews/2026-10-06-review-776.md` 的问题 1–5。
- 证据：
  - Linux 本地实测，工具为 LLVM 22.1.8 的 clang，以及 NDK 30.0.16248370 自带的 llvm-windres（§2.1）。
  - #776 的 CI run `37358775307`：Windows e2e 2/3 中 E2E 889 通过，用时 14.52s。
- 不需要探测 PR。本文所有待测结论都可以在 Linux 本地测出，或者由发布 PR 自身的 Windows CI（E2E 889 的扩展）直接验证。

**修订 2：实施记录。** 实现与本文的差异：

| 项 | 本文 | 实现 | 理由 |
| --- | --- | --- | --- |
| D2 | `is_x86_32()` 含 `x86` | 只含 `i386`–`i686` | `parse` 已把 `x86` 写成 `i686`，集合里再列它就是第二个回答 |
| D2 | `msvc.cppm` 两个函数改收 `Triple` | 签名不变，内部经 `msvc_arch_dir` 询问 `Triple::msvc_arch` | 调用处与单测都传 GNU 架构名；改签名不改变回答 |
| D2 | 6 处 | 另加 `mcpp.platform` 的 `host_arch`：32 位宿主由 `"x86"` 改为 `"i686"` | 它绕过 `parse` 直接成为 `host_triple()` 的架构段；mcpp 不发布 32 位宿主，无可见变化 |
| D3 | `enum class Flavour` | `RcTool::llvm`（bool）与导出的 `is_llvm_windres(path)` | `style` 已经区分 msvc 与 gnu，第三个取值只在 gnu 内部有意义；bool 不重复 `style` |
| D5 | 不改 specs | SPEC-004 升到 1.13：§4.6 陈述架构段的等同拼写 | §4.6 已规定 `[target.X]` 的查找与拼写无关，`x86` 改变了这条规则的内容 |

---

## 0. 决策一览（待 review）

| # | 问题 | 提议 | 分级 |
| --- | --- | --- | --- |
| D1 | `x86-*` 写法在 clang 和 llvm-windres 上都不可用 | `triple::parse` 把 `x86` 归一为 `i686`，与 `amd64 → x86_64`、`arm64 → aarch64` 同一机制（§2） | 无诊断 |
| D2 | 32 位 x86 的判断散落 6 处，范围互不一致 | `Triple` 上新增 `is_x86_32()` 和 `msvc_arch()`，6 处全部改用它们（§3） | 无诊断 |
| D3 | 按文件名判断是不是 llvm-windres | `find_rc_tool` 在发现工具时记下 `flavour`，判断时解析软链接；使用处不再猜（§4） | 无诊断 |
| D4 | `compile_utf8_manifest` 的增量判断不包含命令行 | 命令行写入同目录的 stamp，命令变化就重新生成（§5） | 无诊断 |
| D5 | #776 的行为没有写进文档，也没有 CHANGELOG | `docs/04` 的 `[resources]` 一节和 `docs/21` 的 arch 段补写，中英双语；CHANGELOG 在 5.3 中补记 #776（§6） | — |
| — | review 问题 5（E2E 脚本权限是 644） | **不处理**：`main` 上 584 个 e2e 文件里有 73 个是 644，`run_all.sh` 通过 bash 调用，仓库并没有“必须可执行”的约定 | — |

待定问题：O1–O3（§8）。

---

## 1. 诊断分级

本次四项都是**纠正 mcpp 内部给出的错误回答**。用户写下的输入在修复前后的含义不变，只是从“得到错误产物”或“在后续阶段失败”变成正确产物，因此都不新增诊断。

D1 例外的地方在于它改变了一个**写法的规范形式**：`x86-windows-msvc` 的规范形式变成 `i686-windows-msvc`。先例是 `amd64`、`arm64`，它们一直被静默归一（`modules/toolchain-model/src/triple.cppm:1267`），没有 note。本方案沿用这一先例（见 O1）。

## 2. D1：`x86` 归一为 `i686`

### 2.1 现象（实测）

```
$ clang --target=x86-pc-windows-msvc -c t.c
error: unknown target triple 'x86-pc-windows-msvc19.33.0'
$ llvm-windres --target=x86-pc-windows-msvc -O coff -o t.o t.rc
error: unknown target triple 'x86-pc-windows-msvc19.33.0'
llvm-rc: Preprocessing failed.
```

同一份输入写成 `i686-pc-windows-msvc` 或 `i386-pc-windows-msvc`，两个工具都接受，生成的 machine 是 `0x014c`。

### 2.2 原因

- `normalize_arch` 只归一了 `arm64` 和 `amd64`，`x86` 原样保留。
- `Triple::llvm_triple()` 直接拼成 `x86-pc-windows-msvc`。LLVM 的 `Triple` 不认识 `x86` 这个架构名，它是 MSVC 的写法，不是 GNU 的写法。
- 结果是 `[target.x86-windows-msvc]` 这一行在编译阶段就失败。#776 让 GNU windres 能把 `x86` 映射到 `pe-i386`，但这条路径实际上走不到。

### 2.3 方案

在 `normalize_arch` 中加一行：`if (a == "x86") return "i686";`。

- `[target.X]` 的查找本来就与写法无关（`src/build/prepare/toolchain.cpp:494-503`，先 parse 再比较 `str()`），所以写 `[target.x86-windows-msvc]` 的项目会自动匹配到 `--target i686-windows-msvc`，反过来也一样。
- `i386`、`i486`、`i586` **保持原样**。它们对 clang 来说是不同的基线 CPU，不是同义词。`x86` 只是 MSVC 对“32 位 x86”的总称，Windows 上它的实际基线就是 i686。`msvc.cppm` 的 `triple_for_arch("x86")` 也已经把它映射成 `i686-pc-windows-msvc`，本方案与它一致。
- 归一之后，下游代码里 `starts_with("x86-")` 这样的分支都不会再命中。这些分支在 D2 中一并移除。

### 2.4 测试

- 单测（`modules/toolchain-model/tests/test_triple_vocabulary.cpp`）：
  - `parse("x86-windows-msvc")->str() == "i686-windows-msvc"`
  - `llvm_triple() == "i686-pc-windows-msvc"`
  - `i386-windows-msvc` 不被归一
- 单测（`tests/unit/test_build_resources.cpp`）：`coff_target_flag(llvm-windres, "x86-windows-msvc") == "--target=i686-pc-windows-msvc"`
- E2E 889 扩展：增加一组 `[target.x86-windows-msvc]` + `--target x86-windows-msvc`，构建 C++ exe 加资源，检查 PE machine 为 332 并运行。在 2026.10.5.2 上，这一组会在第一个编译命令处失败（`unknown target triple`），满足“新 E2E 必须在上一版本上失败”。

## 3. D2：32 位 x86 判断收敛到 `Triple`

### 3.1 现状：6 处判断，4 种范围

| 位置 | 判断方式 | 覆盖 `x86` | `i386` | `i486`/`i586` | `i686` |
| --- | --- | --- | --- | --- | --- |
| `triple.cppm:385` `nasm_format` | `arch ==` | ✅ | ✅ | ✅ | ✅ |
| `resources.cppm:171` `coff_target_flag` | `arch ==` | ✅ | ✅ | ✅ | ✅ |
| `toolchain_env.cpp:315` `msvc_arch_of` | 字符串前缀 | ✅ | ✅ | ❌ → `x64` | ✅ |
| `pe_exports.cppm:255` | 字符串前缀 | ✅ | ✅ | ❌ | ✅ |
| `msvc.cppm:1455` cl 版本探测 | `archGnu ==` | ✅ | ❌ → `x64` | ❌ → `x64` | ✅ |
| `msvc.cppm:1616` redist 目录 | `archGnu ==` | ✅ | ❌ → `x64` | ❌ → `x64` | ✅ |

后四处都是潜在缺陷。例如 `pe_exports` 把 i586 当成非 i386，导出名的前导下划线就会处理错；`msvc.cppm` 会给 `i386-windows-msvc` 选中 x64 的 cl 和 redist 目录。这些 target 很少见，所以一直没有暴露，但它们和 #775 是同一类问题：**目标架构的同一个问题，在不同的地方得到了不同的回答。**

### 3.2 方案

在 `Triple` 上新增两个成员，作为唯一的回答来源：

```cpp
bool is_x86_32() const;          // i386 / i486 / i586 / i686（x86 已由 parse 归一）
std::string_view msvc_arch() const;  // "x86" | "x64" | "arm64" | ""（非 Windows 或未知时为空）
```

6 处全部改用这两个成员。其中 `msvc_arch_of` 和 `pe_exports` 拿到的是三元组字符串，先 `parse` 再提问，不再比较前缀。`msvc.cppm` 的两个函数接收的是 `archGnu` 字符串，改为接收 `const Triple&`，或者在调用处 parse。具体选哪种，以改动面最小为准，实施时记录在修订中。

### 3.3 测试

在 `test_triple_vocabulary.cpp` 中用一张表覆盖 `is_x86_32` 和 `msvc_arch`：x86 的 5 种写法、`x86_64`/`amd64`、`aarch64`/`arm64`、一个非 Windows 三元组。

## 4. D3：资源编译器的 flavour 在发现时确定

### 4.1 现状

`coff_target_flag` 用 `tool.name().find("llvm-windres")` 判断该传 triple 还是 BFD 名。两种工具的接受情况：

| 工具 | triple | BFD 名（`pe-i386` 等） |
| --- | --- | --- |
| llvm-windres | ✅（并决定预处理用哪个 triple） | ✅（预处理退回 mingw triple） |
| GNU windres | ❌ | ✅ |

所以把 llvm-windres 误判成 GNU windres，功能上不会出错。唯一的差别在 msvc-env target 上：预处理宏会按 mingw 来定义，而不是按 msvc。llvm-mingw 在 Linux/macOS 上把 `<triple>-windres` 做成指向 `llvm-windres` 的软链接，这种情况会被误判。

### 4.2 方案

- `RcTool` 增加 `enum class Flavour { Msvc, Binutils, Llvm }`，在 `find_rc_tool` 中一次确定。
  - msvc 风格（`rc` / `llvm-rc`）记为 `Msvc`。
  - gnu 风格中，文件名含 `llvm-windres` 的，记为 `Llvm`。
  - 否则用 `weakly_canonical` 解析软链接，**解析后**的文件名含 `llvm-windres` 或 `llvm-rc` 的，也记为 `Llvm`。
  - 其余记为 `Binutils`。
- 现有的 `style` 字段（`"gnu"`/`"msvc"`）保留不动。它决定的是 ninja 规则的写法，读取它的地方有好几处，本方案不改这些读取点。
- `coff_target_flag` 只读 `flavour`。

不起进程做 `--version` 探测，理由见 O2。

### 4.3 测试

单测（Linux/macOS 上运行，Windows 上跳过，因为没有软链接权限的保证）：
- 在临时目录里放一个 `llvm-windres` 文件，再建一个 `i686-w64-mingw32-windres` 软链接指向它。
- 断言 `find_rc_tool` 返回 `Flavour::Llvm`，`coff_target_flag` 返回 triple。
- 断言一个普通文件 `windres` 返回 `Binutils`。

## 5. D4：UTF-8 manifest 的增量判断包含命令行

### 5.1 现状

`resources.cppm:337`：`if (!changed && is_regular_file(out)) return out;`。这里只比较 manifest 和 `.rc` 的文本，命令行变了（换了工具路径、加了 `--target`、升级了 mcpp）不会重新生成。build program 是 host 架构，旧产物恰好仍然正确，所以目前没有实际后果。但这违反了构建图其他部分都遵守的约定：命令是边的一部分（ninja 对 `rc_object` 边就是这样处理的）。

### 5.2 方案

先拼好 argv，再把它逐行写入 `<stem>.cmd`，沿用同一个 `write_if_changed`。三个文件中任何一个变化都会触发重新生成。只多写一个小文件，不增加进程。

### 5.3 测试

单测（非 Windows）：
- 在临时目录里放一个假的 `windres` shell 脚本，它把收到的参数写进输出文件。toolchain 的 `binaryPath` 指向同一个目录。
- 连续两次调用，第二次输出不变。
- 把 `targetTriple` 从 `x86_64-windows-gnu` 改成 `i686-windows-gnu` 后再调用，输出内容变为带 `pe-i386` 的版本。

## 6. D5：文档与 CHANGELOG

- `docs/04-mcpp-toml.md` 与 `docs/zh/04-mcpp-toml.md` 的 `[resources]` 一节，补写：gnu 风格的资源编译器按目标架构生成 COFF 对象（llvm-windres 收到三元组，GNU windres 收到 BFD 名）；rc.exe / llvm-rc 生成的 `.res` 与架构无关。
- `docs/21-the-target-triple.md` 与 `docs/zh/21-…` 的 Segments 表下，补写 arch 的接受写法：`arm64 → aarch64`、`amd64 → x86_64`、`x86 → i686`。`i386`/`i486`/`i586` 保持各自的含义。
- `CHANGELOG.md` 新增 `## [2026.10.5.3]`：
  - Fixed：#775/#776 的 driver link 与 COFF 资源；`x86-*` 写法（D1）；i386–i586 在 MSVC 目录和导出名上的判断（D2）。
  - Changed：资源编译器识别（D3）；manifest 增量（D4）。
- 本次不改 specs：目标三元组的规范不在 `docs/specs/` 中，`[resources]` 的语义也没有变化。

## 7. 发布 PR 与任务依赖

```
mcpp（一个发布 PR，release/2026.10.5.3，基于 main a0c40ec3）
  D1 归一 ──→ D2 Triple 成员（依赖 D1：x86 不再出现）
  D3 flavour ┐
  D4 stamp   ┼─→ docs 04/21（中英）+ CHANGELOG ─→ 版本号 bump ─→ 完整 CI ─→ 自审 ─→ squash 合入 main
  E2E 889 扩展┘
          ─→ release.yml ─→ xlings-res 镜像（GitHub + GitCode，必要时 gtc 补齐）
              ─→ xim-pkgindex bump PR ─→ 合入 ─→ 干净 XLINGS_HOME 安装验证
```

- 版本号：`mcpp.toml` 和 `modules/versioning/src/version.cppm` 改为 `2026.10.5.3`。
- 合入前检查：`check_docs_style.sh`、`check_docs_structure.sh`、`check_modules_wiring.sh`、`check_narrow_conversions.sh`、`check_version_pins.sh`、`check_file_lengths.sh`、`check_workflow_assertions.py`、`git diff --check`、`gen_agents_index.py`。
- E2E 889 的 x86 写法组在 2026.10.5.2 上必须失败：由 Windows CI 跑一次 `MCPP=<2026.10.5.2>` 对照。如果 CI 上不方便，就按 #776 作者的做法在 PR 描述中给出失败输出。

## 8. 待定问题

| # | 问题 | 建议 |
| --- | --- | --- |
| O1 | D1 是静默归一，还是在 parse 时拒绝 `x86` 并提示改用 `i686`？ | **静默归一**。先例是 `amd64`/`arm64`；`x86` 是 MSVC 用户最自然的写法，而且含义唯一。 |
| O2 | D3 要不要对名字不明确的 windres 起进程跑 `--version` 来判断 flavour？（Windows 上的 llvm-mingw 是复制出来的 exe，不是软链接） | **不探测**。误判的后果只是预处理 triple 退回 mingw，而且只在“msvc-env target 恰好找到一个改了名的 llvm-windres”时出现；每次 prepare 多一个进程不划算。 |
| O3 | 既然 E2E 889 已经在 CI 上跑通 `i686-windows-msvc`，要不要把它登记进 known-target 表（tier `preview`）？ | **不纳入 5.3**。登记会带来 target-matrix 的扫描行、覆盖门禁、README 和 docs/21 的表格，范围超出本次修复；另开 issue。目前仍可以通过 `[target.i686-windows-msvc]` 段使用。 |
