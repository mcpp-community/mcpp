# B3 — 产物命名按 target 而非 host(交叉构建每次重链)— 修复方案

> 2026-08-03 · 基于 `9a696d2`(2026.8.3.2)代码审计 + 本机实测
> 来源:`2026-08-03-windows-host-linux-cross-design.md` §6.5 登记的 follow-up
> 记忆:[[windows-host-linux-cross-canadian]]、[[soname-alias-explicit-ninja-goals]]、
> [[explicit-ninja-goals-two-regressions]]

## 0. 一句话

`plan.cppm::target_output()` 用**主机**常量拼产物名,交叉构建时 **ninja 声明的输出文件根本不会
出现**,于是 **每次 `mcpp build` 都重新链接一遍** —— 这不是命名难看,是增量构建对 PE 目标失效。

---

## 1. 先更正一处判断

`windows-host-linux-cross-design.md` §6.5 把 B3 写成「对称地错」:

> | 方向 | 现状 | 应当 |
> |---|---|---|
> | Windows → linux-musl | `mcpp.exe`(却是 ELF) | `mcpp` |
> | **Linux → windows-gnu** | **`mcpp`(却是 PE)** | `mcpp.exe` |

**下面那一行是错的。** 本机实测,Linux 主机交叉到 `x86_64-windows-gnu`:

```console
$ mcpp build --target x86_64-windows-gnu
$ find target -type f -path "*/bin/*"
target/x86_64-windows-gnu/751cb693195ee82c/bin/b3probe.exe        ← 有 .exe
$ file …/b3probe.exe
PE32+ executable (console) x86-64, for MS Windows
```

产物名是对的。原因是 **mingw 的 GCC driver 自己补的**:`-o foo` 且 `foo` 无扩展名时,
mingw GCC 输出 `foo.exe`。mcpp 从未参与这个决定。

真正的问题因此换了一个形态,而且更实际。

---

## 2. 真实症状:ninja 的输出声明与产物不符 ⇒ 每次重链

`target_output()` 在 Linux 主机上返回 `bin/<name>`(`exe_suffix` 为空),ninja 规则照抄:

```ninja
build bin/b3probe : cxx_link obj/main.o          # ← 声明的产物
```

GCC 却写出 `bin/b3probe.exe`。于是 `bin/b3probe` **从来不存在**:

```console
$ test -f target/x86_64-windows-gnu/*/bin/b3probe && echo EXISTS || echo MISSING
MISSING
```

ninja 每次都发现声明的输出缺失 ⇒ 重跑链接边。实测(连续两次 build 比对 mtime):

```console
mtime before:         1785736156
mtime after rebuild:  1785736180
RESULT: RELINKED — declared output never exists, so ninja reruns the link every build
```

**两个方向的实际后果因此并不对称:**

| 方向 | ninja 声明 | 实际产物 | 后果 |
|---|---|---|---|
| Linux → windows-gnu | `bin/foo` | `bin/foo.exe`(GCC 补) | ❌ **声明永不满足 ⇒ 每次重链** |
| Windows → linux-musl | `bin/foo.exe` | `bin/foo.exe`(ELF) | ⚠️ 声明与产物一致,构建正常;只是 ELF 顶着 `.exe` |

也就是说:**Windows→Linux 只是难看,Linux→Windows 是功能缺陷。** 后者恰恰是已经发布、
CI 每天在跑的那条路径(`mingw-cross linux→windows`)。

> **为什么 CI 一直绿**:`102_mingw_cross_wine.sh:58` 用 `find target/$TRIPLE -name '*.exe'`
> 找产物 —— 它找的是**真实产物**,不是 ninja 声明的那个,所以完全感知不到这层不一致。
> 测试断言的是「产物存在且是 PE32+」,这两条都成立。**一个测试可以全绿而问题就在它脚下**,
> 与 [[explicit-ninja-goals-two-regressions]] 同型。

---

## 3. 根因

`src/build/plan.cppm:200-213`:

```cpp
std::filesystem::path target_output(const mcpp::manifest::Target& t) {
    if (t.kind == Library)
        return "bin" / std::format("{}{}{}", platform::lib_prefix, t.name,
                                             platform::static_lib_ext);
    if (t.kind == SharedLibrary)
        return "bin" / std::format("{}{}{}", platform::lib_prefix, t.name,
                                             platform::shared_lib_ext);
    return "bin" / std::format("{}{}", t.name, platform::exe_suffix);
}
```

四个 `platform::` 常量(`platform/common.cppm:18-33`)按 `#if defined(_WIN32)/__APPLE__` 编译期
分支,描述的是**这台机器**的约定。函数签名 `(const Target&)` 里**没有任何 target 信息**,
所以它连"想按 target 求值"都做不到 —— 这才是问题的结构性来源。

同一文件里 `shared_library_link_flags()`(`:236-243`)有同样的毛病,而且更隐蔽:

```cpp
if constexpr (mcpp::platform::is_windows) {           // ← host
    flags.push_back(target_output(t).generic_string());
} else {
    flags.push_back("-L" + …);
    if constexpr (mcpp::platform::supports_rpath) {   // ← host
        if constexpr (mcpp::platform::is_macos) { … } // ← host
```

「链接一个共享库要用完整路径还是 `-L`/`-l`/`-rpath`」是 **target** 的属性(PE 无 rpath,
Mach-O 用 `@loader_path`,ELF 用 `$ORIGIN`)。今天交叉构建共享库会发出宿主形态的链接参数。

### 3.1 已核对:其余 `platform::` 用法都是对的

`exe_suffix` 全仓 16 处引用,除 `plan.cppm` 外全部在找**主机上的可执行文件**
(`ninja`/`xlings`/`nasm`/`llvm-ar`/`clang-scan-deps`/`clang++`)——
`config.cppm`、`xlings.cppm`、`clang.cppm`、`llvm.cppm`、`ninja_backend.cppm:1394`、
`fallback/xlings_binary.cppm`。**这些是正确的 host 语义,不要动。**

`build_program.cppm:506` 的 `build.mcpp.exe` 也是 host 语义(L3 构建程序跑在主机上),
同样不动 —— 参见 [[build-mcpp-helper-self-containment]]。

**本方案的改动面只有 `src/build/plan.cppm` 一个文件。**

---

## 4. 正确的命名是 (os, env) 二元函数,不是 os 一元

这是本方案唯一需要动脑的地方 —— 直觉上「Windows 就是 `.lib`」是错的:

| target | exe | 静态库 | 共享库 | 导入库 |
|---|---|---|---|---|
| `*-linux-*` | *(无)* | `libfoo.a` | `libfoo.so` | — |
| `*-macos` | *(无)* | `libfoo.a` | `libfoo.dylib` | — |
| `x86_64-windows-**gnu**` | `.exe` | **`libfoo.a`** | `foo.dll` | `libfoo.dll.a` |
| `x86_64-windows-**msvc**` | `.exe` | **`foo.lib`** | `foo.dll` | `foo.lib` |

`windows-gnu` 用的是 **GNU 约定**(`lib` 前缀 + `.a`),而现行的 `_WIN32` 分支写死
`lib_prefix=""` / `static_lib_ext=".lib"` —— 也就是说**在 Windows 主机上用 mingw 构建静态库,
今天的命名就已经是错的**(会叫 `foo.lib` 而 mingw 的 `ar` 产出的是 GNU archive)。
这一条与交叉无关,是存量缺陷,顺带一并修掉。

---

## 5. 方案

### 5.1 引入 `ArtifactNaming`(从 triple 导出,一次求值)

放在 `toolchain/triple.cppm` 旁边(它是 triple 语义的家),或 `build/plan.cppm` 内部：

```cpp
// 产物命名约定 —— 由 TARGET 的 (os, env) 决定,与构建主机无关。
struct ArtifactNaming {
    std::string_view exeSuffix;      // "" | ".exe"
    std::string_view libPrefix;      // "lib" | ""
    std::string_view staticLibExt;   // ".a" | ".lib"
    std::string_view sharedLibExt;   // ".so" | ".dylib" | ".dll"
    bool             sharedNeedsImportLib;  // PE: 链接消费者要 import lib
};

ArtifactNaming artifact_naming(const triple::Triple& t);
```

空 triple(host target)回退到今天的 `platform::` 常量 —— 与 B2 的
`target_supports_full_static(triple, hostCapability)` 完全同型,**保证 host 构建逐位不变**。

### 5.2 `target_output` 接受它

```cpp
std::filesystem::path target_output(const Target& t, const ArtifactNaming& n);
```

9 处调用点全在 `plan.cppm` 内,且都在 `make_plan()` 的作用域里 ——
`make_plan` 已经有 `const Toolchain& tc`(`plan.cppm:129`),`tc.targetTriple` 直接可达,
在函数入口构造一次 `ArtifactNaming` 传下去即可。**不需要改任何跨模块签名。**

### 5.3 `shared_library_link_flags` 同步改为按 target

`is_windows` → `n.sharedNeedsImportLib`(或直接判 `triple.is_pe()`);
`supports_rpath` / `is_macos` → 按 target 的 os 求值。

### 5.4 `runtime_aliases_for_target` 要一起看

`plan.cppm:215-228` 依赖 `target_output()` 的结果去比对 `t.soname`：

```cpp
auto output = target_output(t);
if (t.soname != output.filename().string())
    aliases.push_back(output.parent_path() / t.soname);
```

签名要一起加参数。**这条边曾经因为类似改动漏生成过 soname 别名**
(见 [[soname-alias-explicit-ninja-goals]]:别名是独立 ninja 边,只挂 `default`,
改成显式目标后被跳过)。**改完必须有一条断言 soname 别名仍然生成的测试**,不能只靠 review。

---

## 6. 影响面(已实测清点)

### 6.1 会改变行为的只有交叉构建

host 构建(host == target)命名逐位不变,因为空 triple 回退到今天的常量。
**22 个 e2e 里出现 `.exe` 的**绝大多数是 Windows 上的 host 构建,不受影响。

真正受影响的是三处交叉断言:

| 文件 | 现状 | 改后 |
|---|---|---|
| `tests/e2e/102_mingw_cross_wine.sh:58` | `find target/$TRIPLE -name '*.exe'` | ✅ 仍然匹配(产物名不变,只是 ninja 声明对上了) |
| `tests/e2e/112_build_mcpp_cross.sh:53` | `find target -name 'crossbp.exe'` | ✅ 仍然匹配 |
| `.github/workflows/cross-build-test.yml` windows→linux job | 已写成 `\( -name "mcpp" -o -name "mcpp.exe" \)` | ✅ 两种拼写都收,改前改后都对 |

**结论与 §6.5 当初的担心相反:e2e 与 CI 基本不用改。** 因为 Linux→Windows 的产物名本来就由
GCC 决定为 `.exe`,修复只是让 **ninja 的声明** 追上事实;Windows→Linux 的产物名会从
`mcpp.exe` 变成 `mcpp`,而唯一消费它的 CI job 已经两种都匹配。

### 6.2 需要确认的一处

`release.yml` / `bootstrap-macos.yml` 里的打包路径(`find target -path "*/bin/mcpp"` 等)全部是
**host 构建**的产物,不经过交叉路径,不受影响。已逐条核对。

---

## 7. 验证判据(缺一不可)

1. **回归断言先行**:新增测试,断言 `artifact_naming()` 对
   `x86_64-linux-musl` / `x86_64-windows-gnu` / `x86_64-windows-msvc` / `aarch64-macos` / 空 triple
   的五元组取值。**先跑一次确认它在修复前是红的** —— 与 B2 的 commit 1 同样的纪律,
   见 [[windows-host-linux-cross-canadian]]。
2. **`windows-gnu` 用 GNU 约定**:静态库断言为 `libfoo.a`,**不是** `foo.lib`(§4 的存量缺陷)。
3. **不再重链**(本方案的核心收益,也是唯一能证伪「修好了」的判据):
   ```bash
   mcpp build --target x86_64-windows-gnu
   T1=$(stat -c %Y <artifact>)
   mcpp build --target x86_64-windows-gnu
   T2=$(stat -c %Y <artifact>)
   [ "$T1" = "$T2" ]        # 修复前必失败
   ```
   这条要进 e2e,否则它会悄悄退化回去。
4. **host 构建逐位不变**:Linux/macOS/Windows 三个 host 的 `mcpp build` 产物名与改前一致。
5. **soname 别名仍然生成**(§5.4)。
6. 现有 22 个含 `.exe` 的 e2e 全绿。

---

## 8. 分期

单 PR,提交分层(与 B2 同款,理由见 [[windows-host-linux-cross-canadian]]):

| commit | 内容 |
|---|---|
| 1 | `artifact_naming()` + 判据 1/2 的断言,**实现为返回 host 常量的桩** ⇒ 测试红 |
| 2 | 实现按 (os, env) 求值 ⇒ 测试绿 |
| 3 | `target_output` / `runtime_aliases_for_target` 接受 `ArtifactNaming` |
| 4 | `shared_library_link_flags` 按 target 求值 |
| 5 | e2e:判据 3 的「不重链」断言 |

---

## 9. 未决

1. **`windows-gnu` 静态库从 `foo.lib` 改名为 `libfoo.a`** 是唯一一处会改变 **host 构建**
   产物名的改动(Windows 主机 + mingw 工具链)。它是修正存量缺陷,但确实是行为变更 ——
   是否要同期做,还是拆一个独立 PR?我倾向同期,因为把「命名由 (os,env) 决定」做成半截
   反而更难解释。
2. **PE 的 import lib(`libfoo.dll.a` / `foo.lib`)目前 mcpp 完全没建模**。共享库交叉到
   Windows 时消费者拿什么链接,现在靠 `shared_library_link_flags` 直接塞产物路径蒙混过去。
   本方案的 `sharedNeedsImportLib` 只是给它留了个位置,**真正的 import lib 支持不在本期范围**,
   建议另开。
