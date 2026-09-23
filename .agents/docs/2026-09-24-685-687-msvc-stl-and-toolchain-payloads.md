---
subject: triage
status: landed
---

# #685、#687 与 Windows clang 的 MSVC STL：三个问题的归属，以及工具链载荷的规范化

- 日期：2026-09-24
- 对象：mcpp-community/mcpp#685、#687；Windows 上 `llvm` 工具链的 STL 来源；工具链载荷的生产与验收
- 基线：mcpp `origin/main` b30e70c4（`src/`、`modules/` 与 d1f1c98f 相同）；openxlings/xim-pkgindex `origin/main` 2646a16；openxlings/libxpkg `origin/main` 385a1e9；d2learn/xim-pkgindex-fromsource `origin/main` bec5139
- 测量环境：本机 x86_64 Linux；已安装载荷 `xim-x-gcc` 13.3.0 / 15.1.0 / 16.1.0、`xim-x-llvm` 20.1.7 / 22.1.8、`xim-x-glibc` 2.44
- 本文只做分析，不改代码。文中标注「测量」的结论在本机复现过；标注「代码阅读」的结论来自源码，没有运行；标注「未核实」的是推断

---

## 0. 结论

| 问题 | 性质 | 归属 | 修法要点 |
|---|---|---|---|
| #685 | 引擎缺陷：「产物是不是 macOS 二进制」本应按目标判定，现在按宿主判定 | mcpp | 解析函数去掉 `#if defined(__APPLE__)`，是否适用由各读者按目标决定。共有六个读者，其中 build.mcpp 的宿主编译应继续按宿主判定 |
| #687 | 载荷缺陷，而且已有的修复从未生效。xim-pkgindex 在 2026-08-08 已给 `gcc.lua` 加上 `__prune_stale_fixincludes()`，但它的命令把单引号嵌在单引号里，被 shell 拆开后变成对当前工作目录的 `grep -r`，碰不到载荷 | xim-pkgindex 修配方；mcpp 对已安装载荷给出诊断 | 修正引号，让零命中在日志里可见；补安装后断言测试；已安装的用户需要重装；长期在构建端清空 include-fixed |
| Windows clang 的 STL | 设计缺口。受管载荷 `xim:msvc` / `xim:windows-sdk` 已经存在，但只有 `cl.exe` 使用；`llvm` 这一行由 clang 自己探测机器上的 Visual Studio | mcpp，加上索引里的配对数据 | 不单独分发 STL，分发单位保持「toolset + SDK」。用 `-Xmicrosoft-visualc-tools-root` / `-Xmicrosoft-windows-sdk-root` 把受管 toolset 绑定给 clang，这组参数本机已测可用 |
| 工具链管理规范化 | 需要做，但对象是载荷的生产与验收，不是引擎 | 生态为主（配方仓库、索引 CI）；mcpp 提供描述文件和 doctor 检查 | 载荷契约、配方即代码并记录来源、载荷 lint、工具链 × C 库兼容矩阵、安装后不就地改写 |

提问中的三个问题，直接回答如下。

1. **Windows 上的 clang 只依赖 MSVC 的 STL 吗？** 不是。在 MSVC ABI 下，clang 从 VS toolset 取 STL 头、vcruntime，以及 CRT 的导入库和静态库；从 Windows SDK 取 UCRT、Win32 头和导入库。编译器、`lld-link`、`llvm-rc` 和 compiler-rt 来自 llvm 载荷。完整分解见 §3.2。
2. **要不要通过 xlings 分发 MSVC STL，并做成可配置？** xlings 已经在分发：`xim:msvc` 有 14.44.35207 和 14.52.36629 两个版本，`xim:windows-sdk` 有 10.0.26100，都有 GitCode 镜像。缺的是 clang 这一行没有接上。建议接上，但不单独拆出 STL，因为 STL 头、vcruntime 和 CRT 库必须来自同一个 toolset。配置沿用已有的 `msvc@<toolset>` 拼写，不新增 SDK 版本键。这与 2026-08-16 三轴设计 §2.4 的决定一致。机器上装了多个 MSVC 时，今天有两个互相独立的选择器，所用版本也就不固定。§3.6 说明怎样收拢成一次选择，以及三种指定方式。
3. **工具链管理要不要规范化、程序化、可复现？** 需要。#687 就是例证：这类缺陷的修复已经写进了配方，但没有任何程序验证它生效。已发布的三个 gcc 载荷来自两套构建环境，载荷本身也不记录是由哪一版配方构建的。§4 给出具体做法。

---

## 1. #685：deployment target 按宿主判定

### 1.1 核实（代码阅读）

issue 列出的三处根因全部成立。

- `modules/platform/src/macos/macos.cppm:105-116`：`deployment_target()` 在 `#if defined(__APPLE__)` 之外直接返回空串，manifest 里的值被丢弃。
- `src/build/prepare.cppm:1933-1935`：`min_platform_version()` 对 macOS 目标调用上面的函数，得到空串；随后 `modules/toolchain-model/src/triple.cppm:160` 回落到写死的 `"14.0"`。
- `src/build/prepare_inputs.cppm:536`：只有在 `if constexpr (mcpp::platform::is_macos)` 成立时，也就是宿主是 macOS 时，这个值才折入指纹。

### 1.2 issue 没有列出的读者

`deployment_target()` 的每个读者都要单独判断：它问的是目标，还是宿主。

| 读者 | 位置 | 问的是 | Linux 宿主交叉到 macOS 时的现状 |
|---|---|---|---|
| 有效三元组 | `prepare.cppm:1878-1936` → `triple.cppm:160` | 目标 | 恒为 14.0（issue 已述） |
| 项目指纹 | `prepare_inputs.cppm:536` | 目标 | 不折入，改了值也不重建（issue 已述） |
| 平台事实 `macos.deployment-target` | `prepare.cppm:8707-8732` | 目标 | 值为空，所以不写入事实表；包里声明的 `macos.deployment-target >= X` 在这种构建里不会被检查（代码阅读） |
| 编译命令里的 `-mmacosx-version-min` | `hostflags.cppm:513`、`flags.cppm:541/727/1795` | 目标 | Linux 宿主上不发这个参数，版本由三元组承载，结果正确 |
| std 模块预编译 | `stdmod.cppm:258-307` | 目标 | 同上 |
| build.mcpp 的宿主编译 | `build_program.cppm:1205-1208` | **宿主** | 正确，因为 build.mcpp 就在宿主上运行 |

反方向也按宿主判定（代码阅读，外加测量）。macOS 宿主交叉到非 Apple 目标（`x86_64-linux-musl`、wasm、Android）时，`hostflags.cppm:513` 的条件 `mcpp::platform::is_macos && opt.appleSdkRoot.empty()` 成立，编译命令会带上 `-mmacosx-version-min=14.0`，项目指纹也会折入这个值。本机用 clang 22.1.8 对三个非 Apple 目标测过，都是 `warning: argument unused during compilation`，退出码 0。所以这一半的后果只是一条警告（在 `-Werror` 下会变成错误）和不必要的重建，产物不受影响。

### 1.3 修法

- `deployment_target()` 改成与宿主无关的纯解析，优先级为 env > manifest > 14.0。`docs/04-mcpp-toml.md:703-711` 描述的正是这个优先级，而且没有提宿主限制。
- 是否适用，由各个读者按 §1.2 的表格分别决定。
  - 三元组：去掉 `#if` 就够了，因为 `min_platform_version()` 只有在 Apple 的非 iOS 目标上才会走到这里。
  - 指纹：不要第二次调用解析函数，改为读取 `min_platform_version()` 的结果。`cache_key.cppm` 的 `minPlatformVersion` 槽已经是这样读的。
  - 平台事实：修好 `min_platform_version()` 后自动正确。
  - `hostflags.cppm:513`：条件改成「目标是 macOS」。
  - build.mcpp 宿主编译：保持按宿主判定。
- 后续事项（未核实）：默认值 14.0 的依据，是官方 LLVM 静态 libc++ 归档的 floor。如果 libc++ 来自这些预构建归档，而 deployment target 设得低于 14.0，产物头部声明的最低系统版本就会低于它所含目标文件的实际要求。mcppls 用的是依赖图里从源码编译的 libc++，不受影响。当 `cxx.prebuilt()` 成立、请求值又低于归档 floor 时要不要警告，需要单独决定。

### 1.4 判据

- 单测：在任何宿主上，解析函数对同一组 (env, manifest) 给出相同的结果。
- Linux 宿主上的纯计划测试，不做完整构建：
  - `--target aarch64-macos`，manifest 写 `11.0`，`build.ninja` 里出现 `--target=arm64-apple-macos11.0`；
  - 改成 `12.0` 后指纹变化；
  - 再设 `MACOSX_DEPLOYMENT_TARGET=13.0`，env 优先生效。
- 在有 macOS 交叉链的环境里（例如 mcppls 的发布流水线）：用 llvm 载荷自带的 `llvm-objdump` 检查，产物的 `LC_BUILD_VERSION` 为 `minos 11.0`。
- 反方向：macOS 宿主构建 `x86_64-linux-musl`，编译命令里不出现 `-mmacosx-version-min`。

### 1.5 同类位置

这类缺陷的形状是「用宿主分支回答目标问题」。`src/` 与 `modules/` 中共有 107 处 `platform::is_{macos,linux,windows}`，以及 96 处 `#if defined(__APPLE__ | _WIN32 | __linux__)`。

- 在 `modules/platform`（进程、文件系统、环境变量）里，按宿主判定是对的。
- 在构建与工具链代码里，出现次数较多的是 `src/toolchain/registry.cppm`（19）、`src/build/ninja_backend.cppm`（17）、`src/build/prepare.cppm`（13）、`src/build/flags.cppm`（7）、`src/build/plan.cppm`（7）。这些文件里的分支值得逐条标注：它回答的是宿主问题还是目标问题。

---

## 2. #687：include-fixed 里冻结的 pthread.h，以及一个从未生效的修复

### 2.1 复现（测量）

- 本机 `xim-x-gcc/13.3.0` 与 `15.1.0` 的 `lib/gcc/x86_64-linux-gnu/<v>/include-fixed/` 下各有一份 `pthread.h`。两份逐字节相同，大小 48635 字节，文件头写着 `auto-edited by fixincludes from "/home/xlings/.xlings_data/subos/linux/usr/include/pthread.h"`。`16.1.0` 的这个目录里只有 `README`。
- 用 mcpp 实际使用的参数（`-idirafter <xim-x-glibc/2.44>/include -idirafter <linux-headers>/include`）编译 `#include <mutex>`：gcc 13.3.0 与 15.1.0 都报 `cannot convert '<brace-enclosed initializer list>' to 'unsigned int'`（15.1.0 报在 `bits/std_mutex.h:208:32`），16.1.0 编译通过，与 issue 一致。
- 头文件搜索顺序依次是 `…/15.1.0/include`、`…/15.1.0/include-fixed`、glibc 2.44、linux-headers。在载荷的副本里删掉 `include-fixed/pthread.h` 后，同一个翻译单元可以编译通过。
- fixincludes 在这份文件上应用的规则是 `pthread_incomplete_struct_argument`，它把 glibc 的 `struct __jmp_buf_tag __env[1]` 改写成 `*__env`。glibc 2.44 的 `pthread.h:773` 仍然是 `__env[1]`，所以这条规则对 2.44 同样会触发。真正的问题不在这条改写，而在于 fixincludes 复制了整份文件，把 2.39 的 `PTHREAD_COND_INITIALIZER` 也一起冻住了。

### 2.2 引擎为什么不能靠调整顺序解决（代码阅读）

- `include-fixed` 在 gcc 内建的搜索链里，排在任何 `-idirafter` 和系统头目录之前。它本来就是为它所修正的那一份 libc 准备的。
- mcpp 用 `-idirafter` 注入 glibc（`modules/toolchain-model/src/linkmodel.cppm:76-91`），是为了让 libstdc++ 的 `#include_next <stdlib.h>` 链成立。换成 `-isystem`，glibc 会排到 libstdc++ 的包装头之前，这条链就断了。
- `src/toolchain/hostflags.cppm:452-460` 还记录了一点：GCC 没有 clang `-nostdlibinc` 那样的隔离开关。

结论：只要载荷里有从另一份 libc 冻结下来的头文件，引擎怎么排参数都绕不过它。修法只能落在载荷上。

### 2.3 修复早已存在，但从未作用到载荷上（测量）

openxlings/xim-pkgindex 的提交 bd4bff2（2026-08-08，#563，对应 #560）给 `pkgs/g/gcc.lua` 加了 `__prune_stale_fixincludes()`，由 `install()` 调用（`gcc.lua:129`）。它的注释把根因写得完全正确，还明确指出这个问题会在每一次 glibc 升级时重现。问题出在命令的拼法（`gcc.lua:230-232`）：

```lua
local out = os.iorun(string.format(
    "sh -c 'grep -rlF %s %s 2>/dev/null || true'",
    __shq(banner), __shq(root)))
```

`__shq()` 本身会用单引号包住参数，外层又是 `sh -c '…'`，拼出来的命令是：

```
sh -c 'grep -rlF 'auto-edited by fixincludes' '/…/lib/gcc' 2>/dev/null || true'
```

外层 shell 会把它拆成下面几个词：

1. `sh`
2. `-c`
3. `grep -rlF auto-edited`
4. `by`
5. `fixincludes /…/lib/gcc 2>/dev/null || true`

内层 shell 真正执行的只有第 3 个词，也就是 `grep -rlF auto-edited`，没有文件参数。GNU grep 在带 `-r` 又没有文件参数时，会递归搜索当前工作目录。`os.iorun` 有两个实现，libxpkg `src/lua-stdlib/prelude.lua:182` 用的是 `io.popen`，`src/xpkg-executor.cppm:311-339` 用的是 `std::system`，两者都经过 `/bin/sh`，所以上述结论对两者都成立。

我把这个函数原样放进 Lua 5.4 执行（`__shq` 和格式串逐字复制），得到三个结果：

- 工作目录是空目录时，输出为空，`pthread.h` 保留。
- 工作目录里包含载荷副本时，返回的是相对于工作目录的路径，连模拟脚本自身也被匹配到。
- 在 mcpp 仓库根目录执行时，这条命令 10 秒内没有结束，因为它在递归扫描整个仓库。

也就是说，这个修复做不做事取决于安装钩子的工作目录，通常情况下什么都不做。如果安装时的工作目录很大（例如 `$HOME`），这一步还会变成一次完整的递归扫描。

注释里另有两处与实现不符：

- 注释说「os.iorun raises on a non-zero exit」，但两个实现都不会抛出。
- 注释说「the recipe would report success having pruned nothing」。实际正是如此，只不过原因不是注释设想的那个。

`tests/g/test_gcc.py` 里也没有针对这个函数的测试。

这就是 #687 在 2026.9.21.3 上仍能复现的原因。按文件 mtime，本机的 15.1.0 在 08-12 安装、13.3.0 在 08-26 安装，都晚于修复合入；已安装目录里的 `.xpkg.lua` 副本与当前的 `gcc.lua` 逐字节相同；`pthread.h` 仍然存在。不管安装时读到的是哪一版配方，上面的模拟已经说明，这段代码在 sh 下不会删掉这个文件。

### 2.4 为什么 16.1.0 没有这个文件（部分未核实）

三份载荷的 `Configured with:`（测量）：

| 版本 | `--prefix` / `--with-sysroot` | libsanitizer |
|---|---|---|
| 13.3.0、15.1.0 | `/home/xlings/.xlings_data/…`，sysroot 为 `…/subos/linux` | disable |
| 16.1.0 | `/home/speak/workspace/github/openxlings/xim-pkgindex-fromsource/.xlings/…`，sysroot 是该目录下的 project subos；配置行带 `(reconfigured)` | enable |

- 已排除的解释：15.1.0 与 16.1.0 的 `install-tools/fixincl` 里都有 `pthread_incomplete_struct_argument` 这条规则，所以差别不是 GCC 16 去掉了它。
- 未核实的部分：16.1.0 的构建 sysroot 现在是一个符号链接目录，`usr/include/*.h` 指向 `xim-x-glibc/2.44/include`。fixincludes 会不会跳过符号链接，构建当时这个目录里是什么内容，都没有查证。
- 可以确定的部分：16.1.0 干净，是构建环境造成的结果，不是配方里的决定。配方（d2learn/xim-pkgindex-fromsource 的 `pkgs/g/gcc.lua`）固定用 glibc 2.39 构建，没有任何处理 include-fixed 的步骤。

### 2.5 影响面

- **受影响**：`gcc@13.3.0`、`gcc@15.1.0`，目标 `x86_64-linux-gnu`，实现的 glibc 为 2.44。凡是用到 libstdc++ 线程原语的 C++ 翻译单元都会编译失败。冻结的头文件来自 glibc 2.39，而 `gcc.lua` 的注释在 2026-08-08 就记录了 sysroot 已经是 2.44，所以这个组合至少从那时起就是坏的。
- **未测量**：`gcc@11.5.0`、`gcc@9.4.0`。它们在索引里，但本机没有安装。13.3.0 与 15.1.0 出自同一台构建机，这两个版本很可能同样受影响。
- **不受影响（测量）**：
  - `musl-gcc` 13.3.0/15.1.0/16.1.0 的 include-fixed 里只有 README。
  - llvm 载荷的 `.cfg` 在安装时被改写成本机路径，没有发现构建机路径泄漏。
- **其他构建机路径（测量，不影响构建）**：`lib64/*.la`、`plugin/include/configargs.h`、`install-tools/mkheaders.conf` 里也有构建机路径。mcpp 不经 libtool 链接，也不构建 gcc 插件，所以用不到这些文件。
- **为什么没被发现**：CI 只使用 gcc 16.1.0 和 musl-gcc，「13.3.0 / 15.1.0 × glibc 2.44」这一格从来没有被编译过。

### 2.6 修法

按见效快慢排序：

1. **修正配方**（xim-pkgindex，对新安装立即生效）。
   - 引号只保留一层：去掉外层的 `sh -c`，或者去掉内层的 `__shq`。如果去掉的是外层，末尾就不能保留 `|| true`：executor 实现在命令后追加的 `> "<tmp>"` 只会作用于 `true`，grep 的输出不会被捕获（实测，见设计文档 `2026-09-24-toolchain-selection-and-payload-trust-design.md` §4）。
   - 零命中时写一行 info 日志，说明搜索的根目录，这样「没有要删的」和「命令没跑到」在日志里能区分开。
   - 在 `tests/g/test_gcc.py` 里加断言：安装 13.3.0 或 15.1.0 之后，`include-fixed` 下没有带 fixincludes 横幅的文件。这个断言必须在修复被撤回时变红。
2. **已经安装的用户。** 索引更新不会让 xlings 重跑 `install()`，所以需要显式重装：先 `mcpp index update`，再 remove + install。
   - mcpp 侧建议在 `mcpp self doctor` 和工具链解析时加一项检查：如果所选 gcc 的 `include-fixed` 里有带 fixincludes 横幅的文件，而横幅里的源路径不属于当前实现的 libc，就打印原因和重装命令。
   - 删除动作本身留在配方里。它针对的是一个具体载荷，按「引擎只做通用能力」的规则不进引擎。
3. **在构建端根治**（与 §4 一起做）。配方在 `make install` 之后清掉 `include-fixed` 中带横幅的文件，或者用 GCC 官方的 `mkheaders` 针对目标 sysroot 重新生成。重新打包的载荷不能沿用原来的资产名，因为 GitCode 上的资产既不能替换，也不能删除。
4. **mcpp-index#464 的验证。** 配方修好、载荷重装之前，在 gcc 15 上的验证还会遇到这个问题。issue 里用的临时办法（在一个 `-isystem` 目录里放一个只 `#include` glibc 2.44 `pthread.h` 的替身）只适合做验证，不要放进任何包里。

---

## 3. Windows 上 clang 与 MSVC STL

### 3.1 现状（代码阅读，关键点已测量）

- **默认工具链。** 检测到可用的 MSVC 时，默认是 `llvm@20.1.7`，目标 `x86_64-windows-msvc`；检测不到时，默认改成 `gcc@16.1.0`，目标 `x86_64-windows-gnu`（`modules/toolchain-model/src/triple.cppm:1049-1055`）。
- **clang 这一行的依赖从哪来。** STL、vcruntime、CRT、UCRT 和 Win32 SDK 都由 clang 驱动自己去探测机器上的 Visual Studio（vswhere、注册表、环境变量），mcpp 不传任何路径。`import std` 用的 `std.ixx` 由 `msvc::find_std_module_source()` 通过系统搜索链定位（`src/toolchain/clang.cppm:202-217`）。
- **受管载荷没有接到 clang 上。** `xim:msvc`（14.44.35207 为 latest，14.52.36629 为 Insiders 版）和 `xim:windows-sdk`（10.0.26100）已经发布，并有 GitCode 镜像。但 mcpp 里的 `resolve_sdk_for()`、`envOverrides` 这些逻辑只有 `cl.exe` 路径在用，`clang.cppm` 一处都没有引用。所以即使装了受管 MSVC，clang 这一行也不会用它。
- **Linux 宿主不能交叉编译到 `x86_64-windows-msvc`。** 这两个包的描述文件都只有 `xpm.windows` 一节，`src/toolchain/lifecycle.cppm:703-709` 也写明了这一点。
- **文档的相关表述。** `docs/20-toolchains.md:1190-1199` 说明 clang 这一行的 CRT 固定是静态的 `libcmt`，`cxx_runtime` 在这一行不生效。`docs/20-toolchains.md:666` 把 MSVC 和 SDK 列为宿主依赖，理由写的是「不可再分发」。

### 3.2 clang 在 MSVC ABI 下依赖什么

| 组件 | 内容 | 当前来源 | 在受管载荷中的位置 |
|---|---|---|---|
| C++ 标准库 | MSVC STL 头、`modules/std.ixx`、`msvcprt.lib` / `libcpmt.lib` | 机器上的 VS | `xim:msvc` 的 `VC/Tools/MSVC/<v>/{include,lib/x64,modules}` |
| 语言运行时 | vcruntime 头、`vcruntime.lib` / `libvcruntime.lib` | 机器上的 VS | 同上 |
| C 运行时 | `libcmt.lib` / `msvcrt.lib`、`oldnames.lib` | 机器上的 VS | 同上 |
| UCRT | `ucrt` 头、`ucrt.lib` | 机器上的 Windows SDK | `xim:windows-sdk` 的 `Include/<v>/ucrt`、`Lib/<v>/ucrt/x64` |
| Win32 API | `um`、`shared` 头，`kernel32.lib` 等 | 机器上的 Windows SDK | 同上的 `um`、`shared` |
| 编译器、链接器、资源编译器、builtins | clang、`lld-link`、`llvm-rc`、compiler-rt | llvm 载荷 | — |

STL 不能单独拆出来：STL 头依赖同一个 toolset 里的 vcruntime 头，CRT 导入库也要与之配套。STL 源码虽然是 Apache-2.0 with LLVM exception 许可，vcruntime 和 CRT 库却不是开源的。所以分发单位应当保持「toolset + SDK」，也就是现有的这两个包。

### 3.3 不绑定的代价

1. **用哪个 STL 由机器决定。** `docs/20-toolchains.md:987-1016` 记录过 clang + MSVC STL 14.51 下 `std::find` 编译失败的问题（microsoft/STL#6294、mcpp#609），文档给出的规避办法之一是「换一个 STL 早于 14.51 的 runner 镜像」。也就是说，选择 STL 的是 CI 镜像，不是项目。
2. **STL 对 clang 有最低版本要求，VS 一升级就可能让固定的 llvm 版本编译失败。** 下表是从 microsoft/STL 各发布标签的 `yvals_core.h` 里读出的 `STL1000` 下限（测量）：

   | STL 标签 | 要求的最低 clang |
   |---|---|
   | vs-2022-17.12 | Clang 17 |
   | vs-2022-17.14（toolset 14.44） | Clang 19 |
   | msvc-build-tools-14.50 | Clang 19 |
   | msvc-build-tools-14.51 | Clang 20 |
   | main（`_MSVC_STL_UPDATE 202609L`） | Clang 22 |

   Windows 默认的 `llvm@20.1.7` 刚好等于 14.51 的下限。main 分支的要求会随下一个 toolset 发布出来。到那时，只要机器上的 VS 自动升级，默认工具链就会在编译第一个翻译单元时报 `STL1000`，而项目本身什么都没改。
3. **缓存键看不到 STL 版本**（代码阅读，未在 Windows 上实测）。`clang.cppm:189` 把 `stdlibVersion` 设成了 clang 自己的版本号。`cache_key.cppm` 里的头文件集合这一项取自编译命令的 token，而 clang 这一行的 STL 路径不出现在任何 token 里。std 模块的 BMI 另有 `std_module_source_hash` 保护，但依赖缓存键在 VS 升级前后是同一个值。
4. **没有 VS 的机器上，默认工具链换成了另一种 ABI。** 回落到 `x86_64-windows-gnu` 之后，用户就不能链接 vcpkg 或其他 MSVC ABI 的 `.lib`。而一个可用的受管 MSVC 其实已经在索引里了。

### 3.4 所需机制已经具备（测量）

- **clang 支持的参数。** clang 22.1.8 的 GNU 驱动接受 `-Xmicrosoft-visualc-tools-root`、`-Xmicrosoft-visualc-tools-version`、`-Xmicrosoft-windows-sdk-root`、`-Xmicrosoft-windows-sdk-version`、`-Xmicrosoft-windows-sys-root`。这些参数必须写成分开的两个参数；写成 `=` 连接的形式会报 unknown argument。clang-cl 对应的是 `/vctoolsdir`、`/winsdkdir`、`/winsysroot`。clang 20.1.7 的二进制里也有这些选项名，但没有逐个调用验证。
- **本机实测。** 我按 `xim:msvc` 和 `xim:windows-sdk` 的目录布局建了一棵空目录树，然后用 `--no-default-config --target=x86_64-pc-windows-msvc` 调用 clang 22.1.8，得到的搜索路径和链接库路径如下：

```
#include <...> search starts here:
 <llvm>/lib/clang/22/include
 <S>/msvc/VC/Tools/MSVC/14.44.35207/include
 <S>/sdk/Include/10.0.26100.0/ucrt
 <S>/sdk/Include/10.0.26100.0/shared
 <S>/sdk/Include/10.0.26100.0/um

-libpath:<S>/msvc/VC/Tools/MSVC/14.44.35207/lib/x64
-libpath:<S>/msvc/VC/Tools/MSVC/14.44.35207/atlmfc/lib/x64
-libpath:<S>/sdk/Lib/10.0.26100.0/ucrt/x64
-libpath:<S>/sdk/Lib/10.0.26100.0/um/x64
```

- **这次测量证明了什么、没证明什么。** 它只验证了驱动的路由：clang 能直接使用这两个载荷的目录布局，编译和链接两侧都会用到。用真实文件编译还需要在 Windows 上做。另外，这个路由与宿主无关，在 Linux 宿主上同样成立。

### 3.5 建议的做法

- **绑定。** 当 clang 这一行（`llvm@X` × `*-windows-msvc`）选定了一个受管 toolset，就在编译、scan-deps、std 模块预编译、链接这四处传入 §3.4 的参数。具体包括：
  - `std.ixx` 从同一个载荷里取；
  - `stdlibVersion` 记 toolset 的版本；
  - `resolution.json` 记录 `msvc-stl@<toolset>` 和 `ucrt@<sdk>`。

  带版本号的路径进入编译 token 之后，§3.3 第 3 点的缓存键问题也随之解决。
- **选择。**
  - 默认值写在数据里，不写在引擎里：由索引记录 llvm 版本与 toolset 的配对，约束条件是 STL 要求的最低 clang 版本不高于该 llvm 版本。
  - 显式覆盖沿用已有的 `msvc@<toolset>` 拼写，挂在目标行上。具体键名需要设计，例如 `[target.x86_64-windows-msvc] msvc = "14.44.35207"`，本文不做决定。
  - `msvc@system` 保留今天的行为（由 clang 自己探测），作为显式的逃生口。
  - 不新增 SDK 版本键，SDK 跟着 toolset 的依赖走。这是 2026-08-16 三轴设计 §2.4 已经做出的决定。
- **在解析阶段拒绝不兼容的组合，不要等编译时报 `STL1000`。** 如果 llvm 版本低于所选 toolset 的 STL 下限，就在解析阶段报错，并列出兼容的 toolset。下限可以作为数据写进 `msvc.lua`，也可以在安装后从 `yvals_core.h` 里读出来。
- **分阶段推进。**
  - 第一阶段：允许显式选择，默认行为不变。
  - 第二阶段：检测不到 VS 时，默认改成 llvm + 受管 MSVC，而不是换一种 ABI。代价是首次下载约 225 MB（据 2026-08-16 设计文档的测量，14.44 toolset 83.5 MB，SDK 139.1 MB）。这一步由维护者决定。
  - 第三阶段：支持 Linux 宿主交叉到 `x86_64-windows-msvc`。前提有两个：一是 `windows-sdk` 要能在 Linux 上解开 MSI/CAB（xwin 的做法）；二是要统一再分发的立场。`docs/20-toolchains.md:666` 写的是「不可再分发」，而索引已经在 GitCode 上镜像了这些 vsix 和 MSI/CAB，两者目前不一致，只能由维护者决定。
- **判据。** 沿用现有那个屏蔽 VS 的 Windows CI job（`.github/workflows/ci-windows.yml:220` 运行 `182_windows_no_msvc_fallback.sh` 的那个）：
  - 正向：屏蔽 VS 后，用 llvm + 受管 toolset 构建一个 `import std` 程序，断言 `resolution.json` 里记录的 STL 身份。
  - 反向：当机器上的 VS 版本与所选 toolset 不同时，`-v` 输出的搜索列表里只出现载荷路径。

### 3.6 机器上有多个 MSVC 时怎么指定（2026-09-24 补充）

> 本节提出的 `system@14.44` 写法已被取代：维护者提议「带版本号时先匹配系统，没有再用生态包」，设计文档 `2026-09-24-toolchain-selection-and-payload-trust-design.md` §2.2 采纳了这一方案。本节对现状的分析仍然有效。

**问题。** clang 这一行目前有两个互相独立的选择器：clang 驱动选头文件和库，mcpp 选 `std.ixx`。两者都读机器状态和 shell 环境，项目本身没有办法指定用哪一个。依据：LLVM llvmorg-22.1.8 的 `clang/lib/Driver/ToolChains/MSVC.cpp:450-460`、`llvm/lib/WindowsDriver/MSVCPaths.cpp`（两处都是代码阅读，20.1.7 的逻辑相同），以及 mcpp 的 `src/toolchain/msvc.cppm:329-468`。

| 步骤 | clang 选头文件与库（驱动内部） | mcpp 选 `std.ixx`（`find_std_module_source`） |
|---|---|---|
| 1 | 命令行上的 `/vctoolsdir`、`/winsysroot` | `VSINSTALLDIR` |
| 2 | 环境变量 `VCToolsInstallDir`，其次 `VCINSTALLDIR`，再次 `PATH` 上的第一个 `cl.exe` | `vswhere -latest -prerelease -products * -requires …VC.Tools.x86.x64` |
| 3 | SetupConfig：枚举全部实例（不要求装了 C++ 组件），取安装版本最高的一个，读它的 `Microsoft.VCToolsVersion.default.txt` | `VS*COMNTOOLS` |
| 4 | 注册表（VS 2015 及更早版本） | 固定路径扫描 |
| 实例内的 toolset | 由 `default.txt` 决定 | `VC/Tools/MSVC` 下按字符串比较最大的目录；不读 `VCToolsInstallDir`，也不读 `default.txt` |
| 头文件的其他来源 | 只要设了 `%INCLUDE%` 或 `%EXTERNAL_INCLUDE%`，就整体采用，跳过上面的探测 | — |
| SDK | 注册表 `KitsRoot10` 下版本最高的那个；不读 `WindowsSdkDir`（源码里标着 FIXME） | clang 这一行不使用 mcpp 的 SDK 解析 |
| 链接库路径 | 只要设了 `%LIB%` 就用它 | — |

**由此导致版本不固定的五种情形**（代码阅读，未在 Windows 上实测）：

1. **两个选择器选中不同的 toolset，`std.ixx` 与头文件的版本不一致。** 例如用 `vcvarsall x64 -vcvars_ver=14.38` 打开 Developer Prompt：clang 读 `VCToolsInstallDir`，用 14.38 的头文件；mcpp 通过 `VSINSTALLDIR` 定位到同一个 VS 实例，再取其中最大的目录 14.44 下的 `std.ixx`。
2. **最新的 VS 实例没有装 C++ 组件。** 例如装了 VS 2026 却没选 C++ 工作负载。clang 的 SetupConfig 会选中这个实例，读不到 `default.txt`，于是退到注册表，仍然找不到。mcpp 的 vswhere 带了 `-requires`，会找到旧实例里的 `std.ixx`。结果是 mcpp 认为 MSVC 可用，clang 却找不到标准头文件。
3. **同一个项目、同一台机器，在普通 shell 和 Developer Prompt 里构建，用到的 toolset 和 SDK 可能不同。**
4. **安装新的 VS、VS 自动升级、安装新的 SDK，都会改变默认选择**，而项目本身没有任何改动。
5. **这些选择既不进缓存键，也不进 `resolution.json`**（见 §3.3 第 3 点）。

所以要先把选择收拢到一处，然后才谈得上「怎么指定」。

**做法：mcpp 选一次，然后显式告诉 clang。**

- **在四处传参。** mcpp 自己解析出三样东西：toolset 目录、SDK 根目录、SDK 版本。然后在编译、scan-deps、std 模块预编译、链接这四处传入 `-Xmicrosoft-visualc-tools-root`、`-Xmicrosoft-windows-sdk-root`、`-Xmicrosoft-windows-sdk-version`。
- **这组参数的效果。** 它们是 `/vctoolsdir`、`/winsdkdir`、`/winsdkversion` 的别名（`clang/include/clang/Options/Options.td:9513-9521`，llvmorg-22.1.8）。带上之后，clang 不再读 `VCToolsInstallDir` 和 `%INCLUDE%`，不再走 SetupConfig；即使设了 `%LIB%`，也会输出显式的 `-libpath`。
- **`std.ixx` 从同一个 toolset 目录里取。**
- **toolset 和 SDK 必须一起传。** 如果只传 toolset，clang 会因为命令行上出现了 `/vctoolsdir` 而不再读取 `%INCLUDE%`，SDK 就退回到注册表里版本最高的那个，vcvars 选定的 SDK 反而丢了。
- **剩下的一处缺口。** `lld-link` 自己仍会把 `%LIB%` 追加到搜索路径的末尾（`lld/COFF/Driver.cpp:874`）。显式路径排在前面，只有显式路径里缺某个库时才会用到它。mcpp 可以在链接命令的环境里去掉 `LIB`，把这个缺口也关上。

**指定方式。** 下表是语义上的建议，具体键名和拼写留到设计阶段再定：

| 写法 | 含义 | 固定程度 |
|---|---|---|
| `msvc@14.44.35207`（已有） | 受管载荷，精确到版本；SDK 使用载荷旁边的 `windows-sdk` | 跨机器固定 |
| `system@14.44`（新增） | 在机器上已安装的 toolset 里，选版本以 14.44 开头的最高一个。候选集合是 `vswhere -all -prerelease -products * -format json` 列出的所有实例，乘以各实例下的 `VC/Tools/MSVC/*`。找不到就拒绝构建，并列出机器上实际有哪些 | 在本机固定；换一台机器时，要求对方也装了同一个版本 |
| `system`（已有，继续作为默认） | 选中的 toolset 与今天 clang 自己会选的相同（先看环境变量，再取最新实例的 `default.txt`）。变化只在于：mcpp 选一次、传给 clang、并记录下来 | 不固定，但会打印出来并记录 |

- **clang 这一行需要一个新键。** 在这一行，编译器是 `llvm@X`，选 MSVC 是另一件独立的事，所以要一个挂在目标行上的新键，例如 `[target.x86_64-windows-msvc] msvc = "14.44.35207"`。`cl.exe` 那一行可以直接在 `msvc@…` 上使用同样的值语法。
- **项目写明了版本时，以项目为准。** shell 里的 `VCToolsInstallDir`、`VSINSTALLDIR`、`WindowsSdkDir` 会被忽略，并打印一行说明。三轴设计里，受管 toolset 忽略 `WindowsSdkDir` 时就是这样打印 note 的，这里沿用同一做法。
- **`system@<前缀>` 与 `msvc@<版本>` 分开写，是为了保持「来源」这条轴清晰。** 受管 toolset 绑定载荷自带的 SDK；system toolset 的 SDK 仍然按机器扫描。两者的可复现程度不同，拼写上应该一眼就能看出来。
- **解析结果写入 `resolution.json` 和缓存键。** `resolution.json` 记录 `msvc-stl@<toolset>`、`ucrt@<sdk>` 和来源。

**判据：**

- **正向。** 用受管载荷在 CI 上制造多版本环境：Windows runner 本来就带 VS，再装上 `xim:msvc` 14.44 和 14.52。分别指定 `14.44.35207`、`14.52.36629`、`system`，检查三点：`-v` 输出的搜索列表里只有所选的目录；`resolution.json` 记录了对应的版本；切换版本后指纹随之变化。
- **环境干扰。** 在用 `vcvarsall -vcvars_ver=<另一个版本>` 打开的环境里再跑一遍。项目写明了版本时，结果不变，并打印忽略说明。
- **反向。** 撤回修复、去掉显式参数后，测试要能区分出「`std.ixx` 与头文件版本不一致」的情形。

---

## 4. 工具链管理的规范化

### 4.1 回答

需要。规范化的对象是**载荷的生产与验收**，具体回答三件事：

- 这个载荷是谁、在什么环境里、用哪一版配方构建的；
- 它必须满足哪些条件；
- 发布前由什么程序来检查。

引擎这一侧已经有描述文件 `.mcpp-toolchain.json`（`src/toolchain/registry.cppm:248-279`，现有字段为 `schema`、`frontend`、`platform_floor`、`std_module_defines`、`runner`），可以用来承载这些信息。

### 4.2 依据：根因在载荷生产或安装阶段的缺陷

| 缺陷 | 类别 |
|---|---|
| #687：冻结的 `pthread.h`，以及从未生效的清理代码 | 构建机的 libc 内容进入载荷；修复没有验证 |
| gcc 的 `specs` 被历次安装累积改写（2026-08-10） | 安装时就地改写共享状态 |
| 安装后二进制被就地 patch：同一版本的十二个副本有十二个不同的 sha256 | 已安装文件无法与发布物比对 |
| riscv64 musl 载荷的 sysroot 缺内核 UAPI 头（PR#402） | 载荷内容不完整 |
| llvm 22.1.8 slim 载荷漏带 `libatomic.so.1` | 载荷内容不完整 |
| 私有 loader 的默认搜索路径是 glibc 载荷构建时的前缀 | 构建机路径进入载荷 |
| llvm 22.1.8 载荷没有 iOS builtins | 载荷覆盖哪些目标没有声明 |
| 自建载荷的闭包检查输出为空，却被判为通过 | 验收程序本身不可信 |
| 16.1.0 与 13.3.0/15.1.0 来自两套构建环境，配置不同（libsanitizer 开关相反） | 构建不可复现，载荷不记录来源 |

这些缺陷有一个共同点：载荷对不对，只有等到消费方（mcpp 自己的构建或用户项目）编译失败时才能发现。

### 4.3 具体做法

1. **载荷契约。** 写进 `docs/specs/`，每一条都要能由程序检查：
   - 载荷可重定位，不含构建机的绝对路径。`.la`、`configargs.h` 这类不参与构建的文件可以列为豁免。
   - `include-fixed` 里不含从 libc 冻结下来的头文件。
   - `specs` 和 `.cfg` 保持发布时的内容，安装时不就地改写。确实需要的改写放进单独生成的覆盖文件，例如 mcpp 已经在用的 `-specs=` 形式。
   - 描述文件要声明构建所用的 libc 及其版本下限、适用的目标行，以及是否带 sanitizer 等能力。
2. **配方即代码，并记录来源。**
   - 每个载荷都由一份检入仓库的配方，在固定的构建容器里产出。
   - 构建 sysroot 取自索引发布的 glibc 载荷，而不是某台机器上的 subos。
   - 产物带上来源记录，写进 `.mcpp-toolchain.json`：配方仓库与提交、`Configured with`、上游源码的 sha256、构建所用的 libc。
   - 可复现分三级：能从配方重新构建；能用程序验收；逐字节可复现。前两级是必须的；第三级（`SOURCE_DATE_EPOCH`、`-ffile-prefix-map`、确定性 tar）可以以后再做。
3. **载荷 lint。** 同一个程序在两处运行：发布前在配方仓库或索引的 CI 里跑，`mcpp self doctor` 也对已安装的载荷跑一次。
   - 检查项：构建机路径扫描、include-fixed 横幅、ELF 的 `PT_INTERP` 与 `RUNPATH`、`specs` 和 `.cfg` 与发布物一致。
   - 每一项都要配一个反向测试：把缺陷放回去，这一项就要变红。§2.3 的修复正是因为缺了这一步，才会看起来生效、实际从未生效。
4. **工具链 × C 库兼容矩阵。** 在索引 CI 里，从索引本身枚举两个维度：一边是 gcc 和 llvm 的各个版本，另一边是该目标行可解析的各个 libc 版本。每一格编译 `<memory>`、`<mutex>`、`<thread>`，再加一个 `import std` 的最小程序。
   - 格子必须从索引枚举，不能靠手写清单。#687 那一格（gcc 15.1.0 × glibc 2.44）正是手写清单里不会出现的组合。
   - 同一个矩阵也覆盖 §3 里 llvm × msvc toolset 的配对。
5. **发布顺序。** 改动 glibc 绑定或 `latest` 之前，先让矩阵在新版本上全部通过。这就是「消费者先发布，`latest` 后移动」这条规则在工具链这一层的应用。

各部分的归属：

- 配方和矩阵放在配方仓库与索引 CI。
- 契约文档和描述文件字段放在 mcpp，因为 mcpp 是消费方。
- lint 程序最好做成独立工具，两边共用。
- 引擎只读取描述文件，并在 doctor 里报告结果。

---

## 5. 建议顺序

1. **#687 的配方修正和测试**（xim-pkgindex），同时给 mcpp doctor 加检查。改动小，对新安装立即生效，也能解除 mcpp-index#464 在 gcc 15 上的阻塞。
2. **#685。** mcpp 侧改动小，mcppls 的下一个版本在等它。
3. **规范化的第 3、4 项**（lint 和兼容矩阵）。它们能直接防止 #687 这类问题再次出现，也为下面第 4 步提供验收手段。
4. **Windows clang 的 MSVC 选择收拢为一处（§3.6）**，同时完成 §3.5 的第一阶段。这一步不改变默认结果，只让选择固定下来、可以指定、可以记录。需要先出一份设计，确定键名、`system@<前缀>` 的拼写、配对数据放在哪里，以及四处参数如何注入。
5. **规范化的第 1、2 项**（契约和配方重建），以及 Windows 的第二、三阶段。

## 6. 需要维护者决定的事项

维护者于 2026-09-24 答复如下：

| 事项 | 答复 |
|---|---|
| 已经安装了 gcc 13.3.0 或 15.1.0 的用户，是主动通知重装，还是只靠 doctor 提示 | 暂不通知 |
| 检测不到 VS 时，Windows 的默认工具链是否从 `x86_64-windows-gnu` 改成 llvm + 受管 MSVC | 不改。§3.5 的第二阶段因此搁置；§3.6 里的 `system` 默认值也保持今天的选择结果 |
| MSVC toolset 和 Windows SDK 的再分发立场（文档与 GitCode 镜像的说法不一致） | 未答复，保持现状 |
| 载荷配方和构建 CI 放在哪个仓库 | 以后专门讨论 |

维护者同时指出：Windows clang 这部分的核心问题是，机器上装有多个 MSVC 版本时如何指定，否则所用版本不固定。§3.6 专门回答这个问题。

---

## 附：测量记录

| 结论 | 做法 |
|---|---|
| 冻结的头文件使 `<mutex>` 编译失败；删掉后编译通过 | 从 mcpplibs/mcpp-index `tests/examples/cli11/compile_commands.json` 取 gcc 15.1.0 的实际参数，`-fsyntax-only` 编译 `#include <mutex>`；再在载荷副本里删掉 `include-fixed/pthread.h`，重跑同一条命令 |
| 清理函数在 sh 下什么都不删 | 把 `gcc.lua` 的 `__shq` 和格式串逐字复制进 Lua 5.4，加上 libxpkg 的 `os.iorun`（`io.popen`），分别在空目录、载荷副本目录、mcpp 仓库根目录下执行 |
| 应用的是哪条 fixincludes 规则，两个版本都有这条规则 | 对比冻结头与 glibc 2.44 头中的 `__jmp_buf_tag` 声明；对 15.1.0、16.1.0 的 `install-tools/fixincl` 执行 `strings \| grep pthread_incomplete_struct_argument` |
| 三个 gcc 载荷的构建来源 | 各载荷 `bin/gcc -v` 的 `Configured with:` |
| clang 接受受管载荷的布局 | 按 `xim:msvc` / `xim:windows-sdk` 布局建空目录树，用 clang 22.1.8 带 `--no-default-config --target=x86_64-pc-windows-msvc -Xmicrosoft-*` 调用 `-v -fsyntax-only` 和 `-###` |
| 在非 Apple 目标上，`-mmacosx-version-min` 只产生警告 | clang 22.1.8，`--target` 分别为 `x86_64-linux-musl`、`wasm32-wasi`、`aarch64-linux-android24`，`-fsyntax-only` |
| STL 要求的最低 clang 版本 | `gh api repos/microsoft/STL/contents/stl/inc/yvals_core.h?ref=<tag>` 读取 `STL1000` 行 |
| clang 的 MSVC 与 SDK 选择顺序、`-Xmicrosoft-*` 的别名关系（代码阅读） | llvmorg-22.1.8 的 `clang/lib/Driver/ToolChains/MSVC.cpp`、`llvm/lib/WindowsDriver/MSVCPaths.cpp`、`clang/include/clang/Options/Options.td`、`lld/COFF/Driver.cpp`；20.1.7 的 `MSVCPaths.cpp` 与 22.1.8 的差异只在命名空间写法 |
