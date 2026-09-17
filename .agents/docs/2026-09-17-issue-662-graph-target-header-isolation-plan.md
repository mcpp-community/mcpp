---
subject: triage
status: active
---

# #662:目标侧由依赖图提供时，编译器的隐式头文件搜索仍指向宿主

- Issue: mcpp-community/mcpp#662(Sunrisepeak,2026-09-17)
- 依据:`origin/main` 9bc00f80(mcpp 2026.9.17.2);本机 Linux x86_64,已安装 `mingw-w64-x86-64-dev`;llvm 22.1.8
- 状态：方案待 review,未实现

---

## 0. 结论

1. **缺陷在引擎，而且范围比 issue 描述的宽。** 目标侧的 C 库由依赖图提供(`c-abi musl (openkal-musl, graph)`)时,
   mcpp 会把该 C 库的头文件目录以 `-I` 广播给图中每个编译单元，但**没有关掉编译器 driver 自带的系统头文件搜索**。
   链接侧对同一事实早已写成 `-nostdlib`,编译侧缺少对应的一半。受影响的是**所有**文本包含 C 头文件的单元
   (C、C++、汇编、依赖扫描),不只是 compat 包的 C 源文件。
2. **C++ 有一个同形的孪生缺陷。** 同一目标下，clang++ 的默认搜索列表里还有宿主 mingw 的 **libstdc++** 目录
   (`/usr/lib/gcc/x86_64-w64-mingw32/13-win32/include/c++`),而这个图的 C++ 层是 libc++。issue 的最小复现里
   根项目只 `import std`、没有文本包含头文件，所以没有暴露。
3. **issue 中有两处判断与实测不符。**
   - 「C++ 模块单元没有这个问题」:全局 `cxxflags` 同样没有隔离;C++ 单元没出错只是因为没有文本包含 C 头文件。
   - 「宿主没有 mingw 时同一个项目能构建」:去掉宿主头文件后,zlib 的 `gzlib.c` 报
     `fatal error: 'io.h' file not found`。`io.h` 只存在于宿主 mingw 中，所以没有 mingw 的机器上这个组合同样会失败，
     只是报错不同。这是依据头文件位置作出的推断，没有在无 mingw 的机器上单独测量。
4. **修好隔离之后，暴露出一个真实的包级不兼容，它属于生态，不属于引擎。** `x86_64-windows-gnu` 目标预定义 `_WIN32`,
   zlib 把 `_WIN32` 当作「存在 Windows CRT」(`<io.h>`、`_lseeki64`、`_wopen`),而这个图的 C ABI 是 musl。
   一直以来，宿主 mingw 恰好替它补上了 CRT 头文件。
5. **方案分两层，各自单 PR。**
   - mcpp:编译侧按目标侧各层的来源关闭对应的隐式搜索，与链接侧的 `-nostdlib` 由同一个值决定。
   - mcpp-index:compat 包在 `cfg(all(windows, c-abi = "musl"))` 下走 POSIX 分支(先做 zlib,并给出 ABI 判据)。

---

## 1. 实测

### 1.1 复现(mcpp 2026.9.17.2,与 issue 相同的清单)

```
Target x86_64-windows-gnu → x86_64-w64-windows-gnu
       kernel-abi  openkal  (openkal-windows@0.7.4, graph)
       c-abi       musl     (openkal-musl@0.13.5, graph)
/usr/x86_64-w64-mingw32/include/corecrt.h:98:24: error: typedef redefinition ...
.../openkal-musl-0.13.5/musl/include/sys/stat.h:84:5: error: conflicting types for 'chmod'
```

### 1.2 命令行对比(build.ninja)

| 位置 | 与头文件隔离相关的 token |
|---|---|
| 全局 `cflags` | `-std=c11 -O0 -g --target=x86_64-w64-windows-gnu -fdwarf-exceptions -femulated-tls --no-default-config`,**没有隔离** |
| 全局 `cxxflags` | 同上加 `-std=c++23`,**没有隔离** |
| 全局 `ldflags` | `... -nostdlib -static ...`,**链接侧已隔离** |
| openkal-musl 自己的 C 单元 `unit_cflags` | `-ffreestanding -nostdinc ...`,由包自己声明 |
| compat.zlib 的 C 单元 `unit_cflags` | `-D_XOPEN_SOURCE=700 -DOKW_STANDALONE`,**只有宏定义** |
| zlib 单元的 `local_includes` | zlib 自身目录，以及 openkal-llvm-runtime、openkal-musl 的广播目录 |

openkal 系列的包自己带着 `-nostdinc`,所以它们不受影响;受影响的是**不知道自己在为 openkal 构建**的普通包。

### 1.3 判据:clang 的头文件搜索列表

```
$ clang -xc -v -fsyntax-only --target=x86_64-w64-windows-gnu --no-default-config /dev/null
 <llvm>/lib/clang/22/include
 /usr/x86_64-w64-mingw32/include                       <- 宿主

$ ... -nostdlibinc
 <llvm>/lib/clang/22/include                           <- 只剩编译器自带头文件

$ clang++ -xc++ -v ...
 /usr/lib/gcc/x86_64-w64-mingw32/13-win32/include/c++  <- 宿主 libstdc++(孪生缺陷)
 /usr/lib/gcc/x86_64-w64-mingw32/13-win32/include/c++/x86_64-w64-mingw32
 /usr/lib/gcc/x86_64-w64-mingw32/13-win32/include/c++/backward
 <llvm>/lib/clang/22/include
 /usr/x86_64-w64-mingw32/include
```

判据是**搜索列表本身**,不是构建是否成功，也不是 `-I` 有没有出现(同一形状见记忆
`an-implicit-include-search-is-not-on-the-command-line`)。

### 1.4 逐单元验证(用 `ninja -t commands` 取出原命令，只增加 token)

| 单元 | 原命令 | + `-nostdlibinc` | + `-nostdlibinc -U_WIN32 -include mcpp_zlib_config.h` |
|---|---|---|---|
| gzlib.c | 8 个错误(宿主 CRT 与 musl 冲突) | 1 个错误:`'io.h' file not found` | 通过 |
| gzread / gzwrite / zutil / deflate | 失败 | — | 通过 |

---

## 2. 设计原则(与已有架构对齐)

- **一层由谁提供，就由谁提供它的头文件和库。** 目标侧模型已经按层记录来源(`targetside_model`:
  `cAbi.fromGraph()`、`cxx.fromGraph()`、`cAbi.prebuilt()`)。链接侧据此替换了 `-nostdlib`,`hostflags.cppm`
  据此撤掉了载荷的 `-isystem`。编译侧还差「撤掉 driver 的隐式搜索」这一步。三者读同一个值，不另作推导
  (`hostflags.cppm` 的注释已经把「READ, NOT DERIVED」写成规则)。
- **引擎做通用的事，包做自己的事。** 「图提供的层不应被宿主补齐」对所有目标、所有包成立，放在引擎;
  「zlib 在 musl 上应走 POSIX 分支」只对 zlib 成立，放在 mcpp-index 的描述符里，使用已有的
  `target_cfg = { ["cfg(...)"] = ... }` 机制(`modules/manifest/src/xpkg.cppm`),引擎不感知任何包名。
- **同一个 token 必须出现在编译器实际收到的每条命令里。** C、C++、汇编、`clang-scan-deps`、std 模块预编译
  都读同一组 flags;`compile_commands.json` 与 `mcpp emit build-database` 自然随之变化(SPEC-005 R3.7)。

---

## 3. 方案

### 3.1 mcpp(一个 PR)

**M1 编译侧按层关闭隐式搜索。** 位置在 `src/toolchain/hostflags.cppm`,即生成 `cflags`/`cxxflags`/`asmflags`
公共 token 的同一处，与 `graphSuppliesTarget`、`cxxFromPayload` 的判断相邻:

| 条件(读 `HostFlagOptions`) | 追加的 token(clang 家族) | 作用于 |
|---|---|---|
| `!cAbiPrebuilt`(C 库由图提供) | `-nostdlibinc` | C、C++、汇编、扫描、std 模块 |
| `cxxFromGraph`(C++ 层由图提供) | `-nostdinc++` | C++、扫描、std 模块 |

- 用 `-nostdlibinc` 而不是 `-nostdinc`:前者保留编译器自带头文件(`stddef.h`、`stdarg.h`、intrinsics),
  只去掉系统和 C 库目录。编译器自带头文件属于编译器层，不属于 C 库层;openkal 包需要更强隔离时，仍由包自己声明 `-nostdinc`。
- 修正现有条件:`-nostdinc++` 目前只在 `!graphSuppliesTarget && !cxxFromPayload` 时追加，恰好在 openkal
  这种两层都来自图的情形下不追加。1.3 节显示这正是 libstdc++ 泄漏的来源。条件改为只看 `cxxFromGraph`。
- GCC 家族没有 `-nostdlibinc`。等价形式是 `-nostdinc` 加上 `-isystem <gcc -print-file-name=include>`
  与 `include-fixed`。这个差异应当放进工具链模型(`modules/toolchain-model`),由模型提供
  「关闭系统 C 库搜索」的 token,hostflags 不按编译器名分支。实现前需要先确认当前是否存在「图提供 C 库 + GCC」的
  目标行：如果不存在，模型对 GCC 返回拒绝并给出原因，而不是静默不隔离。
- 不改变 C 库由载荷提供(`cAbiPrebuilt` 为真)的情形。宿主 `/usr/include` 补齐载荷 glibc 的问题是另一个已记录的
  缺陷形状，不并入本 PR。

**M2 诊断。** 隔离之后，原先靠宿主补齐的包会报 `'<header>' file not found`,而这个报错不会说明原因。
当一次构建失败、目标侧 `c-abi` 来自图、且编译器输出含 `file not found` 时，追加一行说明:

```
note: this target's C library is musl (openkal-musl@0.13.5, from the dependency graph); the host's
      headers are not searched. A package that needs '<io.h>' has to adapt to this C library,
      for example with target_cfg = { ["cfg(c-abi = \"musl\")"] = ... }.
```

实现复用已有的构建失败提示通道(与 `BuildProgramCompatHint` 同类),只匹配这一种形状，不改写编译器输出。

**M3 测试。**
- 单测(`test_hostflags*`):选项矩阵 `cAbiPrebuilt × cxxFromGraph × {clang, gcc}` 下的 token 集合，
  以及「载荷提供两层时 token 不变」这一回归守卫。
- e2e(新增):openkal 的 `x86_64-windows-gnu` 构建。对每个 C 和 C++ 单元，从 `mcpp emit build-database`
  取出实际命令，附加 `-v -fsyntax-only` 执行，断言搜索列表中只有 store 路径和编译器 resource 目录。
  **这个判据只在宿主存在 mingw 头文件时有区分力**,所以:
  - 用一个 capability(例如 `mingw-host-headers`,探测 `/usr/x86_64-w64-mingw32/include`)作为前置条件;
  - 在 `ci-linux-e2e` 的一个 shard 中安装 `mingw-w64`,保证这个 capability 在 CI 上确实成立(`# requires:`
    只在本机跳过而 CI 从不具备，这个坑记录在 `requires-gcc-is-a-linux-only-gate`);
  - 修复前后各跑一次，确认修复前为红。
- 构建数据库:SPEC-005 R3.7 已要求列出编译器实际收到的词,e2e 736 覆盖了渲染规则;本 PR 只需确认新 token 出现在
  数据库条目中。

**M4 文档。** `docs/22-target-side.md`(中英)在「Adaptation To The Resolved Target Side」一节陈述:
一层由图提供时，编译器不再搜索该层的宿主位置;需要适配的包用层谓词适配。CHANGELOG 写明升级影响。

**兼容性与升级。**
- 只影响目标侧由图提供的构建(openkal 各目标、图提供 libc++ 的 iOS 行)。原生构建和载荷提供 C 库的构建，命令行逐字节不变
  (单测守卫)。
- 行为变化：原先依靠宿主头文件才能编译的包会确定地失败。这些构建原本就只在装了对应宿主包的机器上成功，而且
  产物混用了两个 C 库的声明。M2 的说明使这个失败可以诊断。
- flags 变化使这类构建的指纹变化，升级后全量重建一次。

**跨平台。**
- Windows 宿主上的 openkal 目标同样读这组 flags;MSVC 不参与图提供 C 库的目标。
- macOS 宿主上的 iOS 行(图提供 libc++、SDK 提供 C 库)只受 `-nostdinc++` 条件修正的影响，而那一路原本已经发出
  `-nostdinc++`,需要单测确认 token 不重复。
- 裸机目标(`riscv64-none-elf` 等)的 clang driver 没有系统目录,`-nostdlibinc` 不改变搜索结果;
  需要确认已有裸机 e2e 保持为绿。

### 3.2 mcpp-index(一个 PR)

**I1 compat.zlib 在图提供 musl 的 Windows 目标上走 POSIX 分支。**

```lua
target_cfg = {
    ["cfg(all(windows, c-abi = \"musl\"))"] = {
        cflags = { "-U_WIN32", "-include", "mcpp_zlib_config.h" },
    },
},
```

同时把 `mcpp_zlib_config.h` 中的 `#if !defined(_WIN32)` 保持原样:`-U_WIN32` 之后它会定义 `Z_HAVE_UNISTD_H`。

**ABI 判据(必须有，否则只能证明「编译通过」)。** `-U_WIN32` 只作用于 zlib 自己的单元，消费者看到的
`zconf.h` 仍带着 `_WIN32`。实测读 `zconf.h`,两种视角下的差异有三处:
1. `z_off_t`:zlib 一侧是 `off_t`(经 `Z_HAVE_UNISTD_H`),消费者一侧是 `long long`。
   在 musl 的 x86_64-windows 配置下 `off_t` 也是 `long long`,但这需要测量，不能靠推断;
2. `gzopen_w`:消费者看得到声明，但 zlib 对象中没有定义(`WIDECHAR` 未定义)。调用它会在链接期失败，不会静默出错;
3. `WIN32` 宏：只影响 16 位路径，对 64 位目标没有作用。

判据:mcpp-index 的 zlib 测试成员在该目标下断言 `(zlibCompileFlags() >> 6) & 3` 对应的 `z_off_t` 字节数，
等于消费者一侧的 `sizeof(z_off_t)`。这个值由 zlib 对象自己报告，是跨越两种视角的唯一直接证据。

**I2 其他 compat 包。** issue 的实际目标 `compat:libarchive` 的 Windows 分支远多于 zlib,需要单独评估，不在本 PR 中
承诺。I1 给出的是模式，并在 `docs/repository-and-schema.md` 记录:在 `c-abi = "musl"` 的 Windows 目标上，
`_WIN32` 不代表 Windows CRT。

**I3 CI。** 如果 mcpp-index 的矩阵中没有 openkal Windows 目标，zlib 成员增加一条交叉构建腿。它依赖 mcpp M1 发布,
因此 pin 先移动，再启用这条腿。

---

## 4. 顺序

```
mcpp PR(M1-M4)→ 发布 → 索引 bump
mcpp-index PR(I1-I3,pin 到新版)→ 全量验证
沙箱:xlings subos --sandbox + CN mirror,宿主装有 mingw 与没有 mingw 两种形态，对照上一版
```

I1 本身不依赖 M1:在未修复的 mcpp 上，宿主有 mingw 时依然会因为冲突而失败(冲突来自隐式搜索，与 `_WIN32` 无关)。
所以两个 PR 的判据只能在 M1 发布之后一起成立。

---

## 5. 需要决定的问题

- **D1 隔离 token 放在全局 flags,还是只加给非 openkal 包的单元?** 建议放在全局。openkal 包自带的 `-nostdinc`
  更强，两者叠加没有副作用;按包区分会引入「这个包是不是 openkal」的判断，而引擎不应该感知包名。
- **D2 `-nostdlibinc` 还是 `-nostdinc`?** 建议 `-nostdlibinc`。编译器自带头文件属于编译器层;musl 自带
  `stddef.h` 等，在 `-I` 顺序上本来就排在 resource 目录之前。
- **D3 M2 的诊断提示是否纳入同一个 PR?** 建议纳入。没有它，升级后出现的 `file not found` 读起来像 mcpp 回归。
- **D4 GCC 家族：实现等价隔离，还是明确拒绝?** 取决于是否存在「图提供 C 库 + GCC」的目标行，实现前先穷举
  目标行表。
