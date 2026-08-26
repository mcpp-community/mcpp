# 声明了却没被兑现:第八条,以及文档与词表的一次实测对账

2026-08-26 · 修复 + 优化方案(待 review,尚未实施)

前置:[`2026-08-25-the-two-layer-predicate-family.md`](2026-08-25-the-two-layer-predicate-family.md)
—— 那七条的分析与收尾。本文是第八条,以及由它牵出的一轮文档对账。

每一条都给出实测依据。凡未实测的推断,明确标注。

---

## 0. 一句话

> **词表声明了一样东西,而没有任何一处让它存在;缺席被静默跳过,失败在一百行后
> 以另一个名字出现。**

这与前七条是同一族的另一端:前七条是**判据问错了问题**,这一条是**答案从没被
兑现**。

---

## 1. 第八条:目标行的 sysroot 从不被安装(issue #510)

### 1.1 实测

干净环境——`xlings subos new` + `--sandbox`,空 home,mcpp 的 registry 从零开始:

```
  Target riscv64-none-elf
         kernel-abi  —
         c-abi       picolibc-riscv (xim:picolibc-riscv@1.8.12, prebuilt)
         c++-abi     —
error: build failed
  src/main.cpp:1:10: fatal error: 'stdio.h' file not found
```

⚠️ **报告点名了这个目标的 C 库,而构建找不到它的头。** 同一条命令在装过 picolibc
的机器上正常。

### 1.2 断在哪

同一行目标表声明两样东西,落地方式不同:

```
{ "riscv64-none-elf", "verified", "bare", "llvm@22.1.8", "xim:picolibc-riscv@1.8.12", true }
                                          ↑ 工具链 pin      ↑ sysroot
```

| 字段 | 落地 | 结果 |
|---|---|---|
| `pin` | `resolve_xpkg_path(pkg.target(), autoInstall=true, …)` | 不在就**装** |
| `sysroot` | `penv.deps` → `ensure_project_index_dir` → `seed_xlings_json` | 只**写进 `.xlings.json`** |

然后在 `prepare.cppm:2282` 用它:

```cpp
if (auto dir = xpkg_payload(xl, ref)) {   // 纯查询,不在则 nullopt
    …  targetSysrootInclude = inc;        // 整块被跳过
    …  targetSysrootLib     = lib;
}                                          // 没有 else,没有诊断
```

于是编译命令里既没有 `-isystem <picolibc>/include/<profile>` 也没有对应的 `-L`。

⚠️ **那处代码的注释描述的是一个不发生的安装:**

> The row in kKnownTargets names it, exactly as it names the toolchain pin, and
> it is installed through the same channel a project's `[xlings] deps` use.

`[xlings] deps` 是**声明通道**,不是安装触发器。

### 1.3 为什么至今没人发现

mcpp 自己的裸机 CI 手工把它装上,注释还写明了原因:

```yaml
# The target sysroot, into the home MCPP uses. …
# installed into the ambient xlings home instead, the test would SKIP
"$XLINGS_BIN" install xim:picolibc-riscv -y
test -d "${MCPP_HOME:-$HOME/.mcpp}/registry/data/xpkgs/xim-x-picolibc-riscv"
```

⭐ **每一条裸机 e2e 都跑在一台缺陷已被抹平的机器上。** 开发机同理:做过一次裸机
就永远装着了。这与第七条(首次运行汇入)处境相同——**开发机永远不是首次运行**。

### 1.4 修法

与第七条同形:**不加条件,让它汇入那条已经会做这件事的路径**。

```cpp
// 现在
if (auto dir = mcpp::xlings::paths::xpkg_payload(xl, ref)) { … }

// 改为(与同一行的工具链 pin 同一个通道)
mcpp::fetcher::Fetcher f(**cfg3);
mcpp::fetcher::InstallProgressHandler progress;
if (auto p = f.resolve_xpkg_path(want_sysroot, /*autoInstall=*/true, &progress)) { … }
```

一行上的两个字段本该由同一个机制兑现。

⚠️ **离线与 `MCPP_NO_AUTO_INSTALL` 的行为由 `Fetcher` 统一决定**,不在这里再判一次
—— 那正好是本族缺陷的成因(同一个问题两处回答)。实施前须核实 `Fetcher` 确实在
这两种模式下拒绝而非静默跳过。**未实测。**

### 1.5 判据:删掉补偿

⭐⭐ **删掉裸机 CI 里那两行手工安装,删掉它就是测试。**

这条判据的好处是它**不是新写的断言**(新断言自己可能就有毛病——本次会话里我新写
的六条 e2e 有四条的「否」与「没测成」同读数),而是**移除一处补偿**:

- 补偿在,缺陷看不见;
- 补偿一去,修好之前 `130`–`133` 四条裸机 e2e 必红;
- 修好之后它们照常绿。

两个方向都能验,而且不需要任何新的判据代码。

---

## 2. 文档与词表:一次实测对账

⚠️ **先说结论:被怀疑的那三条 README 命令全部成立。** 我把仓库里所有文档中的
`--target` 拼写抽出来逐个真跑过,而不是按印象判断。

### 2.1 `examples/06-openkal-cross/README.md` 实测

```
  x86_64-linux           → x86_64-unknown-linux-gnu     ✅
  aarch64-macos          → arm64-apple-macos14.0        ✅
  x86_64-windows-gnu     → x86_64-w64-windows-gnu       ✅
```

短名(`x86_64-linux`)与 GNU 拼写(`x86_64-w64-mingw32`)都被接受并归一化。README
无需改动。

### 2.2 全仓库拼写清单

从 `docs/`、`examples/*/README.md`、`.agents/docs/` 抽出的全部 `--target` 值,逐个
真跑:

| 拼写 | 结果 |
|---|---|
| `x86_64-linux` / `x86_64-windows` / `riscv64-none` | ✅ 短名,归一化到全名 |
| `x86_64-w64-mingw32` | ✅ GNU 拼写,归一化到 `x86_64-windows-gnu` |
| `x86_64-pc-windows-musl` | ✅ 归一化到 `x86_64-windows-musl`(随后按宿主拒绝,见 §3) |
| `x86_64-linuxx-gnu` | ❌ 拼写错误,诊断给了建议 |
| `x86_64-linux-mus` | ❌ 拼写错误,诊断给了建议 |
| `aarch64-macos-musl` | ❌ 不存在的组合 |

后三条出现在文档里 —— **须确认它们是「反例示范」还是笔误**。若是示范,加一句说明
它们是有意写错的;若是笔误,改掉。**尚未逐处定位。**

### 2.3 已随 2026.8.25.2 更新的文档

- `docs/03-toolchains.md` + `docs/zh/` —— Targets 块的**四种状态**表,以及
  「不在这个块里的 target 在本机根本构建不了」这句话的收窄。
- `docs/07-build-mcpp.md` + zh —— `[xlings].subos` 决定 `build.mcpp` 的 `PATH`。
- `docs/17-the-project-environment.md` + zh —— 新章节。
- `CHANGELOG.md` —— 九条。

### 2.4 待补的文档

| 文档 | 要补什么 | 依据 |
|---|---|---|
| `docs/13-baremetal.md` | 裸机目标的 C 库**由目标行提供**,以及它是否需要预先安装 | 第八条修好后行为改变 |
| `docs/03-toolchains.md` | 「请求的目标与解析出的目标必须同一个 OS」这条拒绝,以及它的两条出路 | 2026.8.25.2 新增 |
| `docs/16-the-target-triple.md` | 短名与 GNU 拼写都被接受并归一化(§2.2 实测) | 词表行为,文档未述 |
| `examples/06-openkal-cross/README.md` | 无需改动 | §2.1 实测 |

---

## 3. 优化项(不是缺陷)

### 3.1 「不能构建」的拒绝没告诉人下一步

无依赖的工程请求 `x86_64-windows-musl`:

```
error: target 'x86_64-windows-musl' cannot be built on this host.
```

而 `toolchain list` 对同一个目标说:

```
x86_64-windows-musl   PE, static, cross   llvm 22.1.8   via dependency graph
```

两句话自洽——列表说「需要依赖图」,而这个工程没有图。但**拒绝那句没有把列表已经
知道的事说出来**:加一个依赖就能构建。

建议:该拒绝在「本宿主可由图服务」时,附一句指向 `toolchain list` 的同一措辞。
⚠️ 判据要两向:**不可由图服务的目标(MSVC、macOS SDK)不得出现这句话**,否则就是
把一条走不通的路指给人。

### 3.2 `xpkg_payload` 的所有调用点

第八条只是其中一处。`xpkg_payload` 是纯查询,而它的每个调用点都要回答「不在时怎么
办」。**建议逐个过一遍**:

```sh
grep -rn "xpkg_payload" src/ --include=*.cppm
```

已知:`prepare.cppm` 的 `fillXpkgDirs`(`[xlings] deps` → `MCPP_XPKG_*_DIR`)对缺席
的答案是「空字符串」,并且**那是有意的**——包声明了工具而没装,构建程序应当看到
空值并自行决定。第八条不同:那不是包声明的,是**词表声明的**,而词表的另一个字段
会自动安装。

⚠️ 我没有逐个核对其余调用点。**未实测。**

---

## 4. 建议的执行顺序

1. **第八条**:改 `prepare.cppm:2282` 走 `autoInstall`;删裸机 CI 那两行;先用**未修
   的二进制**确认四条裸机 e2e 会红(证明判据有效),再用修好的确认转绿。
2. **§3.1 的拒绝措辞**:与 §1 同一个 PR,它们改的是同一类体验。
3. **§2.2 的三处错误拼写**:定位后决定改还是加说明。
4. **§2.4 的三处文档补写**,与 1 同一个 PR(行为变了,文档同步)。
5. **§3.2 的调用点普查**:单独一轮,产出可能是「其余都对」——那也是结论。
6. 回填上一篇 §7 第 5 条:**#486 触碰的每个判据逐个过一遍**。八条里七条出自那次
   改动,值得确认没有第九条。
