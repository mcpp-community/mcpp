# 源文件角色表 与 build.mcpp 运行上限 —— 把两个硬编码变成两条声明

> 日期：2026-08-11
> 基线：main `e53204a`(`2026.8.10.3`)
> 来源：[PR #272](https://github.com/mcpp-community/mcpp/pull/272)(OPEN,未合)、
> [issue #410](https://github.com/mcpp-community/mcpp/issues/410)(OPEN)
> 本文引用的 file:line 全部核于上述基线；行号是证据,不是装饰。

---

## 0. 结论先行

两个诉求表面无关,底下是同一个形状：**一个本该是数据的决策,现在是代码。**

| | PR #272 | issue #410 |
|---|---|---|
| **诉求** | `.ccm` / `.cxxm` / `.ixx` 也要能当模块接口 | build.mcpp 跑久一点别被直接杀掉 |
| **现在的形态** | 「扩展名 → 角色」在 **20 处独立推导**,分成 **8 份互不一致的清单** | `run_timeout()` 是个**无参函数**,唯一入口是环境变量 |
| **改成** | 单一分类器 `SourceKind` + `[build] module_extensions` | `[build] build_program_timeout` |
| **默认行为** | **图形态零差分**(内置表不动,仍只有 `.cppm`;rule 文本会变,见 §3.8.3) | **零差分**(仍是 600s) |
| **进不进指纹** | **进**(它改构建图的形态) | **不进**(它是策略标量,不改图) |
| **老版 mcpp 遇到它** | 警告+忽略 ⇒ **构建出错的东西** ⚠️ | 警告+忽略 ⇒ 退回 600s,**报错文案清晰** ✅ |

最后两行是这份方案里唯一需要记住的架构判据：

> **一个配置键进不进指纹,只问一个问题：它改变了「图是什么」,还是只改变了「跑图时的策略」。**
> `module_extensions` 决定哪个文件产 BMI、哪个 `.o` 进链接 ⇒ 进。
> `build_program_timeout` 不改任何一条边 ⇒ 不进(进了会让「把超时从 600 调到 1800」重建全世界)。

第四行(**默认图形态零差分**)是这份方案的稳定性支点,也是它与 PR #272 现有实现最大的分歧点。见 §1.3。

还有一条贯穿全文的判据,来自 §3.8.1 那次实测:

> **在「维护一张会过期的知识表」和「无条件做一件已证明幂等的事」之间,永远选后者。**
> 前者错了是**退出码 0 的静默空转**,后者错了什么都不会发生。

---

## 1. 为什么 PR #272 不能按现在的样子合

PR #272 改了 4 个文件(`plan.cppm` +4/-4、`execute.cppm` +3/-2,加两个测试),
把链接侧的 `.cppm` 判断换成了 `cu.providesModule`(**这一步是对的,本方案保留**),
并让 4 种扩展名共享 `.m` 对象名前缀。

**先说两个状态事实**(核于 2026-08-11):

- PR 基线是 `27d250c`,**main 已领先它 79 个提交**;`plan.cppm` 的行号从 155 漂到 303。
- **CI 当前 10 个 job 红**(linux/windows/macOS 的 e2e、unit、cross 全红)。
  本文**不把红全部归因给下面的缺陷** —— 79 个提交的漂移足以单独解释一部分。

下面三条是与基线漂移无关的、机制层面的缺陷。

### 1.1 漏了 `pick_rule` —— Clang 侧不产 BMI

`src/build/ninja_backend.cppm:1005`：

```cpp
auto pick_rule = [](const std::filesystem::path& src) -> std::string {
    auto ext = src.extension();
    if (ext == ".cppm") return "cxx_module";      // ← 只认 .cppm
    ...
    return "cxx_object";
};
```

两个调用点(`:1191` dyndep 模式、`:1235` static 模式)都只传 `cu.source`。
PR #272 **完全没有动这个文件**。

GNU 方言下,`cxx_module`(`:668`)与 `cxx_object`(`:696`)的差别就是一个
`module_output_flag`(`:523`,`traits.needsExplicitModuleOutput` 时展开为
`-fmodule-output=$bmi_out`)。于是一个 `.ixx` / `.ccm` 单元：

- 边上**照样声明了 BMI 产物**(`:1193`,`if (cu.providesModule) out_line += " | " + bmi_path(...)`)
- 命令行里**没有 `-fmodule-output=`**

⇒ Clang 上 ninja 报「declared output not produced」;
GCC 上 `gcm.cache` 是驱动的自动行为,可能照旧产出 ⇒ **同一份代码两个编译器两种结果**。

而 e2e `152_module_extensions.sh` 的第 2 行是 `# requires: elf gcc` ——
**Clang 与 MSVC 路径从未被这条测试走到**。

> 注:`module_src_flags`(`:667`)是 `msvcDeps ? " /interface /TP" : ""` ——
> **只对 MSVC 生效**,GNU 方言下是空串。也就是说 GCC/Clang 侧现在**一个 `-x` 都不发**。
> 这条直接引出 §3.8 那个必须靠实测才能定下来的问题。

### 1.2 `.m` 前缀给四种扩展名 = 新开一个对象名碰撞

`src/build/plan.cppm:293`：

```cpp
if (ext == ".S" || ext == ".s" || ext == ".asm")
    return src.filename().string() + objExt;      // 保留完整扩展名 —— 这是对的解法
auto stem = src.stem().string();
return stem + (ext == ".cppm" ? ".m" + objExt : objExt);
```

PR #272 把条件放宽成「四种扩展名都用 `.m`」。于是同目录下的
`foo.cppm` 与 `foo.ccm` **双双变成 `foo.m.o`**。

`object_for`(`:1099`)的兜底救不了它：`rootBasenameCount[fname] > 1` 时的处理是
**把源码目录结构镜像进对象路径**,而这两个文件本来就在同一个目录,镜像之后仍然重名。

注意汇编分支(`:298`)早就把这题做对了 —— **不认识的扩展名就保留完整文件名**。
正确的推广是沿用它,而不是扩大 `.m` 的适用面。见 §3.5。

⚠️ PR 新增的单测 `tests/unit/test_module_extensions.cpp` **把这个碰撞钉成了预期行为**：

```cpp
TEST(ModuleExtensions, CppmGetsDotMPrefix) { EXPECT_EQ(object_filename_for("src/foo.cppm", ".o"), "foo.m.o"); }
TEST(ModuleExtensions, CcmGetsDotMPrefix)  { EXPECT_EQ(object_filename_for("src/foo.ccm",  ".o"), "foo.m.o"); }
```

同一个函数、同一个输入 stem、同一个输出。**采纳本方案时这两条断言必须反过来写**
(`foo.ccm` → `foo.ccm.o`),否则测试会保护住缺陷。
e2e 里的 `grep -q "info.m.o"` 等四条同理。

### 1.3 直接写进内置默认 = 对已发布包的破坏性变更

这是决定方案形态的一条,也是**本方案与 PR #272 的根本分歧**。

若把 `.ixx` 加进内置默认,`src/manifest/toml.cppm:1610` 的默认 sources glob 必然要跟着变成
`src/**/*.{cppm,ccm,cxxm,ixx,cpp,cc,c,S,s,asm}`。后果：

> **任何一个 `src/` 下躺着 `.ixx` 的已发布包,升级 mcpp 之后会突然开始编译它。**

`.ixx` 尤其危险 —— 它是 MSVC 的拼法,现实中最常见的存在形式就是
「vendored 的 MSVC-only 源码,当前平台上根本编不过」。
包描述符按版本冻结,而 mcpp 是滚动升级的 ⇒ **一次 mcpp 升级会让一批已发布包同时变红,
且包作者无法通过改包来避免**(旧版本的 tarball 已经发出去了)。

这是「索引下限把旧客户端变砖」那条判据的另一面：
**发布出去的数据不得让程序失效;升级的程序也不得让已发布的数据失效。**

⇒ **内置表保持现状(只 `.cppm`),四种扩展名全部走配置、opt-in。**
默认路径上生成的 `build.ninja` 与今天逐字节相同。

**PR #272 自己的 e2e 已经证明了这个形态是对的** —— 它的 fixture 里有这么一行注释和一份手写 manifest：

```bash
# Default glob only picks up .cppm/.cpp/.cc/.c — explicitly add .ccm/.cxxm/.ixx
cat > mcpp.toml <<'EOF'
[modules]
sources = ["src/**/*.cppm", "src/**/*.ccm", "src/**/*.cxxm", "src/**/*.ixx", "src/**/*.cpp"]
EOF
```

也就是说:**即使按 PR #272 的实现,用户仍然必须在 mcpp.toml 里显式 opt-in**,
只不过 opt-in 的方式是手写五条 glob、且**语义分散在两处**
(`sources` 决定「编不编」,代码里的硬编码清单决定「算不算模块」)。
本方案做的事就是把这两处合成一处声明。

(顺带:那份 fixture 用的是 `[modules] sources` —— 遗留镜像键;新文档一律用 `[build] sources`。)

---

## 2. 共同根因：同一决策 20 处推导

`grep -rn 'extension()\s*==\|ext ==\|ext !=' src/ --include=*.cppm` 的完整结果,
按「它在回答什么问题」归并：

| # | 位置 | 在问什么 | 用的清单 | `.ixx` 走到这里 |
|---|---|---|---|---|
| 1 | `modgraph/scanner.cppm:573` | 是否 C-like(跳过模块扫描) | `.c .m .S .s .asm` | ✅ 落入 C++ 扫描 |
| 2 | `modgraph/scanner.cppm:690` | 是否实现单元 | `.cpp` | ⚠️ 与 `.cc/.cxx` 同病 |
| 3 | `modgraph/p1689.cppm:388` | 是否模块接口(兜底) | `.cppm` | ⚠️ 全靠编译器 ddi |
| 4 | `modgraph/p1689.cppm:389` | 是否实现单元 | `.cpp .cxx` | ⚠️ **与 #2 不同的清单** |
| 5 | `build/plan.cppm:293` | 对象名怎么起 | `.S .s .asm` / `.cppm` | ❌ 与 `foo.cpp` 撞名 |
| 6 | `build/plan.cppm:388` | 是否实现单元 | `.cpp .cc .cxx .c .m .S .s .asm` | ⚠️ **第三份清单** |
| 7 | `build/plan.cppm:1363` | 模块对象进不进链接 | `.cppm` | ❌ **`.o` 不进链接** ← #272 修了 |
| 8 | `build/plan.cppm:1425` | 同上(link unit) | `.cppm` | ❌ 同上 ← #272 修了 |
| 9 | `build/ninja_backend.cppm:235/240/247` | C / GAS / NASM / 免扫描 | `.c .m` / `.S .s` / `.asm` | ✅ |
| 10 | `build/ninja_backend.cppm:1005` | 用哪条 ninja rule | `.cppm` | ❌ **不产 BMI** ← #272 **漏了** |
| 11 | `build/execute.cppm:591` | 快路径要不要作废 | `.cppm .cpp .cc .cxx .c .h .hpp` | ❌ **静默陈旧** |
| 12 | `build/directives.cppm:649` | 生成物可不可编译 | `…… .cppm .ixx` | ⚠️ **唯一含 `.ixx` 的清单** |
| 13 | `build/compile_commands.cppm:81` | CDB 里算不算 C | `.c .m` | ✅ |
| 14 | `build/compile_commands.cppm:236` | CDB 里算不算 GAS | `.S .s` | ✅ |
| 15 | `build/prepare.cppm:5465` | MSVC 要不要拒绝 GAS | `.S .s` / `.asm` | ✅ |
| 16 | `manifest/toml.cppm:1610` | 默认 sources glob | `cppm cpp cc c S s asm` | ❌ **根本 glob 不到** |
| 17 | `manifest/toml.cppm:1642` | 自动推断 lib target | `.cppm` | ❌ 纯 `.ixx` 库无 target |
| 18 | `build/prepare.cppm:3371` | stage 的兜底 glob | `cppm cpp cc c` | ⚠️ **比 #16 少 asm** |
| 19 | `manifest/types.cppm:1027` | `[lib]` 约定路径 | `.cppm` | — |
| 20 | `toolchain/clang.cppm:196` | std 模块源要不要 `-x c++-module` | `.ixx` | (仅 std) |

**8 份清单**(#2 / #4 / #6 三份「什么算实现单元」互不相同;#11 / #12 / #16 / #18 四份「什么算源文件」互不相同)。

### 2.1 最危险的一处：#11 快路径漏扫

`src/build/execute.cppm:591` 是**唯一一处「漏了会静默出错、不会报错」**的：

```cpp
for (auto& f : expand_glob(projectRoot, "src/**/*")) {
    auto ext = f.extension().string();
    if (ext != ".cppm" && ext != ".cpp" && ext != ".cc" &&
        ext != ".cxx" && ext != ".c" && ext != ".h" && ext != ".hpp")
        continue;                                  // ← .ixx 在这里被跳过
    if (last_write_time(f) > ninjaTime) return true;
}
```

这个 sweep 回答的不是「文件变了没有」(那是 ninja 的活),而是
**「构建图的形态可能变了没有」**。一个 `.ixx` 里新加一句 `import foo;`：

- 扫描器不会重跑(快路径判定为 fresh)
- dyndep 仍是上一次的
- ninja 照旧重编这个 `.o`(它的 mtime 变了),**但 BMI 依赖边是旧的**
- 结果：链接成功 / 运行出错,或者一个指向别处的 `Bad import dependency`

**这就是「修补放在控制流到不了的地方」的镜像版本 —— 判据放在了控制流会跳过的地方。**
PR #272 改了这一行(加了 3 个扩展名),但仍然是第 4 份手写清单;
下一个新增语义还会漏第 5 次。

---

## 3. 设计 A：分类一次,当数据传下去

### 3.1 判据

| # | 判据 | 检验方式 |
|---|---|---|
| A-1′ | 未配置 `module_extensions` 时,构建图的**形态**相同:哪些文件被编、产不产 BMI、哪些 `.o` 进链接、快路径扫哪些文件 | 归一化 diff `build.ninja`,**diff 里只允许出现 rule 定义行**(理由见 §3.8.3) |
| A-2 | 「扩展名 → 角色」在**产品代码里只有一处**表达 | `grep -c 'extension() ==' src/` 只剩分类器 |
| A-3 | 分类**发生一次**(单元入图时),下游读字段而非重新判定 | `CompileUnit::kind` 的读者数 ≫ `classify` 的调用点数 |
| A-4 | 改 `module_extensions` 必然重新 prepare,且落到不同的 `target/<triple>/<fp>/` | 改键前后 `mcpp build --print-fingerprint` 不同 |
| A-5 | 任意两个源文件永不共享对象路径 | 单测穷举 `object_filename_for` |

### 3.2 新叶子模块 `src/source_kind.cppm`

**放在 `src/` 根、只 `import std`**,不是 `src/build/` 下 ——
因为 `mcpp.manifest.toml`(#16/#17)和 `mcpp.modgraph.scanner`(#1/#2)都要用它,
而 `mcpp.build.*` 依赖 `mcpp.manifest`,反向依赖会成环。

```cpp
export module mcpp.source_kind;
import std;

export namespace mcpp {

enum class SourceKind {
    ModuleInterface,   // 产 BMI;.o 无条件进链接单元
    Cxx,               // C++ 实现单元(含模块实现分区)
    C,                 // .c / .m —— 走 C 规则,不扫描
    GasAsm,            // .S / .s —— C driver
    NasmAsm,           // .asm
    Header,            // .h / .hpp —— 不编译,但改动会改图形态
    Other,             // 不是构建输入
};

// 内置表 + manifest 追加项。按值传,构造极廉价(一次 vector 拷贝),
// 因为 prepare 是多包的:root 与每个依赖各有自己的表,
// 全局单例会让依赖包被消费者的配置分类 —— 那是 #405 那类跨包污染的形状。
struct ExtensionTable {
    // 内置:{".cppm"} —— **不含** .ccm/.cxxm/.ixx,见 §1.3
    std::vector<std::string> moduleInterface;
    // 其余四轴当前是常量。留结构不留键:加 cxx_extensions 时
    // 只需在这里加一个 vector + 一处 parse,不需要动 classify 的形状。
};

ExtensionTable builtin_extension_table();
ExtensionTable extension_table_for(std::span<const std::string> manifestExtras);

SourceKind classify(const std::filesystem::path& p, const ExtensionTable& t);

// 由 SourceKind 导出的谓词 —— 供只关心一个轴的读者用,
// 保证它们不会各自再写一份 switch。
bool produces_bmi(SourceKind k);        // == ModuleInterface
bool is_scan_exempt(SourceKind k);      // C / GasAsm / NasmAsm
bool links_unconditionally(SourceKind k); // ModuleInterface
bool affects_graph_shape(SourceKind k); // 除 Other 之外全部 —— #11 用它

// 默认 sources glob 由表导出,而不是与表并列手写。#16/#18 的两份清单
// 就是「并列手写」的产物。
std::vector<std::string> default_source_globs(const ExtensionTable& t);

} // namespace mcpp
```

`ExtensionTable` 的构造要**规范化**:补前导 `.`、小写化(Windows 文件系统大小写不敏感,
`FOO.IXX` 与 `foo.ixx` 必须同类)、去重、拒绝空串。规范化只在构造时做一次,
`classify` 里不做 —— 它在扫描热路径上,每个源文件调用一次。

### 3.3 分类只发生一次

`SourceUnit`(`src/modgraph/graph.cppm:15`)已经有 `packageName` / `relPath`,
`scan_one_into`(`src/modgraph/scanner.cppm:747`)的签名里**已经有 `manifest`**。
所以不需要任何新的管道 —— 这是本方案最省的一处：

```cpp
// scanner.cppm,scan_one_into 内,每个源文件一次
u.kind = classify(file, table);   // table 来自这个包自己的 manifest
```

然后：

| 结构 | 新增字段 | 谁写 | 谁读 |
|---|---|---|---|
| `SourceUnit` | `SourceKind kind` | scanner(#1/#2)、p1689(#3/#4) | plan |
| `CompileUnit` | `SourceKind kind` | `make_plan` 从 `SourceUnit` 拷贝 | #5 #6 #7 #8 #9 #10 #13 #14 #15 |

`SourceUnit::isModuleInterface` / `isImplementation` 两个 bool 变成 `kind` 的导出量。
**保留它们**(约 20 处读者),但改成从 `kind` 赋值的派生字段,不再各自判断扩展名 ——
一次性删掉它们会把这个 PR 的改动面翻倍,而收益是 0。

于是 `ninja_backend.cppm:1005` 变成：

```cpp
auto pick_rule = [](const CompileUnit& cu) -> std::string {
    switch (cu.kind) {
        case SourceKind::ModuleInterface: return "cxx_module";
        case SourceKind::C:               return "c_object";
        case SourceKind::GasAsm:
            return cu.source.extension() == ".S" ? "asm_object" : "asm_object_raw";
        case SourceKind::NasmAsm:         return "nasm_object";
        default:                          return "cxx_object";
    }
};
```

> **`.S` vs `.s` 是 GasAsm 内部唯一保留的扩展名判断**,因为它区分的是
> 「过不过预处理器」,那是同一角色下的两种编译方式,不是两个角色。
> 强行拆成 `GasAsmPreprocessed` / `GasAsmRaw` 会让枚举去表达编译器旗标 —— 那是越界。

**唯一不能读字段的是 #11(快路径)** —— 它跑在 prepare 之前,手里没有 plan。
它调 `classify(f, table)`,`table` 来自它已经加载的 manifest。这是**唯一**一处二次分类,
而且它与 scanner 读的是同一个 manifest 的同一个键,不可能漂移。

### 3.4 配置面

```toml
[build]
module_extensions = [".ixx", ".ccm", ".cxxm"]
```

| 决策 | 取值 | 理由 |
|---|---|---|
| 语义 | **追加**到内置 `[".cppm"]`,不能删 | 删掉 `.cppm` 没有任何正当用例,却能让整个包静默变成「无模块」 |
| 任意后缀 | **全放行** | §3.8.1 之后,放行的代价是零 —— mcpp 不再需要知道编译器认不认。`.mpp` / `.cppmi` / 任何拼法都行 |
| 唯一的守卫 | **拒绝**与内置非模块角色冲突的后缀(`.cpp` `.cc` `.cxx` `.c` `.m` `.mm` `.h` `.hpp` `.S` `.s` `.asm`) | 「声明 `.c` 是模块接口」不存在正当用例,却会让 C 文件走 C++ 模块规则,炸在一个没法诊断的地方。解析期硬错误,不是警告 |
| 排除某文件 | 用 `sources` 的 `!` 前缀 | 已有机制,不再造第二个 |
| 作用域 | **每个包读自己的 manifest** | 与 `cflags` / `globFlags` 同构;消费者不该决定依赖的源码怎么分类 |
| 传播 | **不传播** | 它是包的私有构建属性,不是 usage requirement |
| 默认 sources glob | 随表**自动扩展** | 否则声明了 `module_extensions` 却什么都不发生 —— 见 §3.6 |

### 3.5 对象名：一个总函数,零碰撞

`object_filename_for` 改成对 `SourceKind` + 扩展名的**总函数**,规则只有三条：

```
.cpp  → foo.o        (今天的行为,不动)
.cppm → foo.m.o      (今天的行为,不动)
其余  → foo.<ext>.o  (汇编分支 :298 早就这么做,现在推广到全部)
```

于是 `foo.ixx → foo.ixx.o`、`foo.ccm → foo.ccm.o`,与 `foo.cppm → foo.m.o` 三者互不相撞。

**为什么 `.cpp` / `.cppm` 必须原样不动**：对象名是全局依赖缓存条目的**内部布局**
(`CompileUnit::packageObjectRel`,`plan.cppm:1094`)。缓存键不变而条目内布局变了,
后果不是「缓存未命中」而是 **「命中一个不含所需对象的条目」** ⇒ 链接期缺 `.o`。
`plan.cppm:1091-1094` 的 `mcpp#344` 注释守的就是这件事
(「a pure function of the owning package」)。改这两个的代价是缓存键必须同步改版,
而收益是 0(它们今天不撞)。

⚠️ **已知遗留(不在本方案范围)**：同目录下的 `foo.c` 与 `foo.cpp` 今天都产出 `foo.o`,
真实碰撞,且 `rootBasenameCount` 兜底无效(理由同 §1.2)。
修它需要动缓存条目布局 ⇒ **单独一个 issue,配缓存键版本号**。
本方案在单测里把这条**显式标记为已知失败**,而不是让穷举测试假装通过。

### 3.6 联动：不联动的话这个键什么都不做

| 位置 | 改法 |
|---|---|
| #16 `toml.cppm:1610` | 默认 glob 由 `default_source_globs(table)` 生成 |
| #17 `toml.cppm:1642` | `hasCppm` → `hasModuleInterface`,用 `classify` |
| #18 `prepare.cppm:3371` | 删掉这份兜底清单,复用 `default_source_globs` |
| #19 `types.cppm:1027` | `[lib]` 约定路径仍产 `.cppm` —— **这是写路径,不是读路径**,不动 |

**顺序**：`module_extensions` 在 `apply_defaults_and_infer` 之前解析完成
(`toml.cppm` 的 parse → defaults 顺序天然满足),不需要调整。

用户显式写了 `sources` 时,他的 glob 优先,和今天一样。

### 3.7 指纹与缓存键

折进 `prepare.cppm` 的规范化 compile-flags 串,**root 块与 per-package 块各一处**
(`:265` 与 `:370`),形式抄 `globFlags`：

```cpp
for (auto const& e : m.buildConfig.moduleExtensions) { s += " modext:"; s += e; }
```

per-package 那一处不能省。理由与 `#253` 给依赖的 `globFlags` 补指纹是同一条：
path / git 依赖不按版本冻结,它的 `module_extensions` 改了就是另一份产物。

### 3.8 编译器方言：两组实测,和一张被删掉的表

这一节原本写的是推理。第一组实测推翻了推理,第二组实测又推翻了第一组得出的设计。
两次都记在这里 —— **推导路径本身是这份方案里最有价值的部分**。

**这不是「编译器不支持模块」的问题**,而是低一层的东西:每个驱动都有一张
「后缀 → 这是什么语言」的映射表,**表里没有的后缀被当成链接输入**(和 `.o`/`.a` 一类),
转手给链接器,根本不进编译前端。`.ixx` 是 MSVC 的约定(`cl.exe` 的原生模块接口后缀),
Clang 的表收了 `.cppm`/`.ccm`/`.cxxm`,**没收它**。

**实测**(2026-08-11 本机,GCC 16.1.0 / Clang 22.1.8)。
判据不是「有没有警告」而是**给文件塞一个语法错误,看驱动有没有真的编它**:

```
$ printf 'export module m;\nthis is not valid c++ @@@\n' > bad.<ext>
$ <cc> -std=c++23 -fsyntax-only bad.<ext>
```

| 后缀 | GCC 16.1.0 | Clang 22.1.8 | MSVC `cl` |
|---|---|---|---|
| `.cppm` | ✅ 报语法错误(进了前端) | ✅ 报语法错误 | ❌ 需 `/interface /TP` |
| `.ccm` / `.cxxm` | ✅ | ✅ | ❌ 推定同 `.cppm`(**未验证**) |
| `.ixx` | ✅ | ❌ **`'linker' input unused`** —— 没编 | ✅ **原生** |

**三个编译器三张表,而 MSVC 与 Clang 正好互为盲区 —— 没有一个后缀三家通吃。**

MSVC 两格的证据等级与前两列不同(本机无 MSVC),但都取自本仓库:

- `.ixx` 原生:`toolchain/msvc.cppm:517-537` 编 MSVC STL 的 `modules/std.ixx` 用的是
  `cl /nologo <std> /EHsc /O2 /W0 /c std.ixx /ifcOutput ... /Fo:...` ——
  **没有 `/interface`,没有 `/TP`**,裸 `/c` 就产出 IFC,且这条路在 Windows CI 上跑着。
- `.cppm` 非原生:`ninja_backend.cppm:666` 的注释原话是
  「cl.exe needs /TP (our module interfaces are .cppm, **unknown to cl**) and /interface」。
- `.ccm` / `.cxxm` 两格是**推定**,⚠️ 必须在 Windows CI 上补测,不得当结论用。

**最阴的是它不报错**：

```
$ clang++ -std=c++23 ok.ixx --precompile -o ok.pcm
clang++: warning: ok.ixx: 'linker' input unused
$ echo $?          →  0          # 成功
$ ls ok.pcm        →  不存在      # 但 BMI 没有
```

**退出码 0、零个 error、产物不存在。** 这正是 §1.1 那个缺陷在 Clang 上的实际形态:
命令跑完退 0 什么也没产出,报错要等到下游 `import` 时才炸,且指向别处。

解药一行,实测有效:

```
$ clang++ -std=c++23 -x c++-module ok.ixx --precompile -o ok.pcm
  → ok.pcm, 18892 字节
```

`toolchain/clang.cppm:193-200` 早就为 MSVC STL 的 `std.ixx` 记着同一件事
(「Clang doesn't recognize the .ixx extension as a module source by default」),
只是那段今天只服务 `stdModuleSource`。

**到这里为止,自然的设计是「一张 per-方言 的原生后缀表 + 按需补旗标」。
下一节的第二组实测把这个设计否掉了。**

### 3.8.1 结论:把「谁认哪个后缀」这张表**整个删掉**

初稿在这里设计了一张 `nativeModuleSuffixes` 表 + 一个 `declarePolicy`。
**实测把它否掉了。** 关键的一组测量:

| | GCC 16.1.0 | Clang 22.1.8 | MSVC `cl` |
|---|---|---|---|
| 显式声明模块接口的旗标 | `-x c++` | `-x c++-module` | `/interface /TP` |
| 该旗标**加在原生后缀上**是否幂等 | ✅ 尺寸相同,差异位置=噪声(下注) | ✅ **逐字节相同**(18896 = 18896) | ✅ **今天就在无条件发** |
| 该旗标能否**救回**不认识的后缀 | ✅ `.weirdext` 产出 gcm | ✅ `.ixx` 产出 18892 字节 pcm | ✅(它就是为此存在的) |
| 交叉误用 | `-x c++-module` → `language not recognized` | `-x c++` → **174 字节空壳 pcm** | — |

> ⚠️ **方法论下注**:GCC 的 gcm **本身不可复现** —— 同一条命令跑两次,
> 差异出现在 byte 605;`-x c++` 那次差异在 byte 602,**同一处噪声**。
> 我第一次读成「`-x c++` 改变了产物」,是**缺对照**。
> 任何「加了旗标产物就变了」的判断,都必须先跑一次「不加旗标跑两遍」的对照。

⇒ **既然旗标幂等,就没有理由去判断「要不要发」。永远发。**

```
dialect.moduleInterfaceFlag   // gcc: "-x c++"; clang: "-x c++-module"; msvc: "/interface /TP"(现状)
```

`cxx_module` 规则无条件带上它。**没有第二张表,没有 policy 枚举,没有版本知识。**

注意这个常量是按**编译器家族**分的,不是按 `CommandDialect`(gnu/msvc)——
gcc 与 clang 同属 gnu 方言但取值不同(GCC 直接拒绝 `-x c++-module`)。

### 3.8.2 为什么 X 优于查表:失败模式不对称

| | 查表 | 永远显式 |
|---|---|---|
| 表错了会怎样 | **退出码 0、无 BMI、下游 `import` 时炸在别处**(§3.8 那个静默空转) | 多发一个幂等旗标,什么都不发生 |
| 维护成本 | 3 家 × N 版本,每次升编译器重验 | 一个家族常量,零 |
| 知识时效 | 会过期(GCC 16 认 `.ixx`,GCC 14 呢?) | 无时效 |

**一张会过期、错了还不报错的表,不该存在。**

顺带:**MSVC 是三家里唯一一开始就做对的** —— 它今天就是「永远显式说」。
本方案只是把 GNU 侧对齐到 MSVC 已有的做法,不是发明新机制。

### 3.8.3 这推翻了 A-1 的措辞(不是推翻它的意图)

不敢做 X 的唯一理由是「会改掉每条现存 `.cppm` 命令行」。**这条站不住**:

`toolchain/fingerprint.cppm` 的**第 8 个字段就是 mcpp version**。
任何 mcpp 升级都已经换 `target/<triple>/<fp>/` 并重建全部,
而这个 `-x` 改动**只可能随一个新 mcpp 版本发布** ⇒ 成本已被版本指纹全额吸收,
**增量为零**。A-1 当初防的是一笔 mcpp 自己每次发版都在付的钱。

⇒ **A-1 改写为 A-1′**(§3.1 同步):

> **未配置 `module_extensions` 时,构建图的形态相同** ——
> 哪些文件被编、产不产 BMI、哪些 `.o` 进链接、快路径扫哪些文件。
> **rule 文本可以变**;验证方式从「空 diff」改为「diff 里只有 rule 行」。

---

## 4. 设计 B：`build_program_timeout`

### 4.1 先否掉 issue #410 的字面诉求

issue 原文是「希望能够在超时时**询问用户**, 而不是直接中断」。**不做**,理由三条:

1. **没有可用的交互通道。** build.mcpp 通过 `capture_exec_deadline`
   (`build_program.cppm:766`)运行,stdout/stderr 被 dup2 进管道用于解析
   `mcpp:` 指令协议。要弹交互,得先把这个通道劈开。
2. **构建的多数发生地没有人。** CI、`mcpp build` 进流水线、被 ninja 当子进程调 ——
   一个卡在 prompt 上的构建比一个失败的构建更难诊断(它不会失败,它会一直挂着)。
3. **构建结果不该依赖一次击键。** 同一份源码 + 同一份 lock,两次构建应当等价。

**替代品是把诊断做对**：报错要指名**改哪个文件的哪个键**。见 §4.5 ——
issue #410 的作者真正需要的信息就是这个。

同时 maintainer 在 issue 里已经指出:「build.mcpp 一般只做代码生成/预处理/后处理,
不应该把整个构建期放到 build.mcpp 里」。这条**判断是对的,但不构成拒绝配置的理由** ——
600 s 对「opencv 全量编译」不够,对「protoc 生成 400 个文件」同样不够,
而后者完全是 build.mcpp 的正当用法。

### 4.2 键与语义

```toml
[build]
build_program_timeout = 1800   # 秒;0 = 不限
```

```cpp
// manifest/types.cppm — BuildConfig
std::optional<int> buildProgramTimeoutSecs;
```

⚠️ **`std::optional` 是承重的,不是风格。** 用 `int = 0` 的话「没写」与「写了 0」不可区分,
而 0 的含义是**不限** ⇒ 每个没写这个键的工程都会静默失去运行上限。
`execute.cppm:1010-1014` 的注释已经把这条判据写死过一次:
「`--timeout 0` still means "no limit", it just has to be asked for」。

解析期校验:非整数 → 错误;负数 → 错误。**不接受 `"30m"` 时长串** ——
`--timeout SECS`、`--build-timeout SECS`、`MCPP_BUILD_PROGRAM_TIMEOUT` 全是秒制,
第二种拼法只会制造「哪个键吃哪种格式」的记忆负担。

### 4.3 优先级

```
MCPP_BUILD_PROGRAM_TIMEOUT   (每次调用的显式覆盖)
  > 该包自己的 [build] build_program_timeout   (包作者的知识)
  > 内置 600s                                  (基线)
```

与 `macos_deployment_target` 已文档化的优先级同构(env > manifest > 内置),
不新造一套。

**「该包自己的」是关键**:依赖包的 build.mcpp 用依赖包 manifest 里的值。
消费者不知道 opencv 的生成器要跑多久,opencv 的作者知道。
消费者需要拔高时用 env(它是全局的,这正合适 —— 卡住的是**这一次**构建)。

### 4.4 唯一的策略函数

`src/build/directives.cppm:377` 的 `run_timeout()` 现在是无参的,而且**自己 `getenv`**
—— 于是优先级逻辑无法单测(只能改进程环境)。拆成两个函数,
**优先级在纯函数里解一次**,而不是在调用点拼:

```cpp
// directives.cppm
inline constexpr int kDefaultRunTimeoutSecs = 600;

// 唯一读环境的地方;解析失败 / 负数 → nullopt(沿用现有的宽容行为)
std::optional<int> env_timeout_override();

// 纯函数:两个 optional 进,一个时长出。优先级只在这里表达。
std::chrono::milliseconds run_timeout(std::optional<int> envSecs,
                                      std::optional<int> manifestSecs);
```

调用点(`build_program.cppm:766`)写成
`run_timeout(env_timeout_override(), m.buildConfig.buildProgramTimeoutSecs)`；
`m` 本来就在手边(`:770` 已经在用 `m.package.name`)。

T-3 于是可以穷举 3 × 3 = 9 种组合,一次进程环境都不用碰。

### 4.5 诊断:指名改哪个文件

现在的文案(`build_program.cppm:768`)只说 env。新文案必须回答
**「我该改哪一个 mcpp.toml」** —— 依赖包超时时,用户的直觉是改自己的,那是错的:

```
error: build.mcpp for 'opencv' exceeded its 600s time limit and was killed.
       Raise it in that package's own manifest:
         <store>/opencv-4.10.0/mcpp.toml   →  [build] build_program_timeout = 1800
       Or for this invocation only:
         MCPP_BUILD_PROGRAM_TIMEOUT=1800   (0 = no limit)
       Output so far:
       ...
```

根工程超时则只打第二行的本地路径。**路径必须是真实存在的那一个** ——
`m` 里已经有包根,不要拼一个「大概是这里」的路径。

### 4.6 不进指纹 —— 显式记下来

`build_program_timeout` **不进** compile-flags 串、**不进** 缓存键。
它不改任何一条边。若进了,把超时从 600 调到 1800 会换一个 `target/<triple>/<fp>/`,
**重建全世界** —— 而这恰好发生在用户正因为构建太慢而调这个键的时候。

这条要写进代码注释,否则下一个人「为了一致性」把它加进去。

### 4.7 Windows：诚实地说它不生效

`platform/process.cppm:598` 的 `capture_exec_deadline`：

```cpp
#if defined(__linux__) || defined(__APPLE__)
    ... posix_spawn + SIGKILL ...
#else
    return capture_exec(argv, extraEnv, cwd);   // ← 无界,deadline 被丢弃
#endif
```

⇒ **Windows 上 `build_program_timeout` 是个 no-op**,和 `MCPP_BUILD_PROGRAM_TIMEOUT` 一样。

处理方式:

- **不加每次构建的告警**(设了键的包在 Windows 上每次构建都告警 = 噪声)
- `mcpp doctor` 报一次
- `docs/05-mcpp-toml.md` 与 `docs/07-build-mcpp.md` 各说一次(zh 镜像同步)

**要真正修**需要 Windows 侧改用 `CreateProcess` + `WaitForSingleObject(timeout)` +
`TerminateProcess`,替换现在的残留 shell launcher。这是独立工作量,
**单开 issue,不塞进本方案** —— 否则这两个键会被一个平台移植卡住。

---

## 5. 兼容性与降级

| 场景 | `module_extensions` | `build_program_timeout` |
|---|---|---|
| 新 mcpp + 没写这个键的老工程 | 零差分 | 零差分(仍 600s) |
| **老 mcpp + 写了这个键的包** | ⚠️ **警告+忽略 ⇒ 构建出错的东西**(`.ixx` 被当普通 TU) | ✅ 警告+忽略 ⇒ 退回 600s,失败文案清晰 |
| `--strict` | 老 mcpp 上是硬错误 ✅ | 同 |

`[build]` 的未知键是**警告**不是错误(`toml.cppm:1029-1058`,`kKnownBuildKeys` +
`m.schemaWarnings`),所以老 mcpp 不会拒绝加载整份 manifest ——
这躲开了「一个不认识的键让整个包永远无法被采用」那个坑。

但 `module_extensions` 的降级是**静默错误**,不是干净失败:老 mcpp 忽略这个键之后,
`.ixx` 仍会被 `sources` glob 到(如果作者写了),然后被当成普通 TU 编译 ——
产出一个不含模块的、链接期才炸的东西。而 mcpp.toml **没有** `min_mcpp` 之类的版本下限键
(核实过:`grep -rn 'min_mcpp\|minMcpp' src/manifest/` 为空)。

⇒ **发布约束(必须写进 `docs/10-publishing-a-library.md`)**:
用了 `module_extensions` 的包,索引描述符必须声明 mcpp 版本下限。
索引侧的下限机制必须**可降级** —— 「因版本太低而不可用」要如实拼成「不可用」,
不能拼成「不存在」,否则客户端会去做无穷的重复刷新。
**这一条是 PR-2 发布前的阻塞项**,见 §9.3。

**两个键都要加进 `kKnownBuildKeys`**(`toml.cppm:1040`),
以及同一处的「Supported keys:」文案 —— 那句话是硬编码的,漏了它会现场打脸。

---

## 6. 测试计划

⚠️ 这一节里带 ⚠️ 的,都是**会让缺陷通过测试**的写法。
`build.ninja` 相关的回归测试有过三次「测试自己把缺陷盖住」的记录。

| # | 层 | 断言 | 陷阱 |
|---|---|---|---|
| T-1 | 单测 | `classify()` 对内置表的全部扩展名 × 配置追加后的全部组合 | — |
| T-2 | 单测 | `object_filename_for` 穷举:任意两个不同源文件不共享对象名 | `foo.c`/`foo.cpp` **标记为已知失败**(§3.5),不要为了绿而放宽断言 |
| T-3 | 单测 | `run_timeout` 优先级穷举(env × manifest × 未设 × 0) | env 读取必须已上移,否则测试要改进程环境 |
| T-4 | e2e `217` | 四种扩展名 build → link → **run**,GCC / Clang 双 dialect | ⚠️ **必须 `# requires:` 到 Clang** —— 只跑 GCC 会同时放过 §1.1 的 BMI 缺陷**和** §3.8 的 `.ixx` 驱动不识别 |
| T-5 | e2e `217` | **未配置**时 `.ixx` 不被编译(零差分) | 反向断言,防止有人「顺手」把它加进内置表 |
| T-6 | e2e `218` | 改 `.ixx` 里的 `import` ⇒ 快路径作废 ⇒ 全量 prepare | ⚠️ **不能用 `touch`** —— 要真正加一句 `import`;⚠️ **不能先删产物** —— 那会让 ninja 失败被读成「图过期」,回退全量 prepare,**缺陷被盖住** |
| T-7 | e2e `218` | 改 `module_extensions` ⇒ 指纹变 ⇒ 落到不同 `target/<triple>/<fp>/` | 比对目录名,不是比对「构建成功」 |
| T-8 | 单测 | `moduleInterfaceFlag`:gcc=`-x c++`、clang=`-x c++-module`、msvc=`/interface /TP` | ⚠️ 两者**不可互换**(实测:`-x c++-module` 在 GCC 上 `language not recognized`;`-x c++` 在 Clang 上产出 174 字节空壳 pcm)。这条断言就是防「gnu 方言共用一个常量」的简化 |
| T-10 | e2e `217` | **幂等性回归**:`.cppm` 加了旗标之后仍能正常 build → link → run | 这是 §3.8.1 那个「幂等」实测结论的守卫。它一旦不成立,整个「永远显式」的设计就塌了 |
| T-9 | e2e `186` 扩写 | manifest 键生效;env 覆盖 manifest;报错文案含 manifest 路径 | 已有 `# requires: gcc`;Windows 上这条测不了,如实 skip |

**T-6 的两个陷阱是记录在案的真实事故**:先删产物会让 ninja 失败被读成图过期;
先 `touch` 源码会让快路径按 mtime 直接失效、**根本没走到被测代码**。
两种写法都会得到一个绿色的、什么都没测的测试。

**验证方法**(而不是「看起来对」):归一化 diff 实施前后的 `build.ninja`,
在未配置 `module_extensions` 的工程上,**diff 里只允许出现 `rule cxx_module` 的定义行**
—— 任何一条 `build ...` 边的变化都是 A-1′ 失守。

⚠️ **还有一条方法论**(§3.8.1 的下注):任何「加了旗标之后产物变了」的判断,
必须先跑「不加旗标连跑两遍」的**对照**。GCC 的 gcm 不可复现,
没有对照的 `cmp` 会给出一个看起来很硬、实际是噪声的结论 —— 我在这份文档里踩过一次。

---

## 7. 实施顺序

分两个 PR,因为它们的风险面完全不同。

### PR-1 · `build_program_timeout`(小、独立、可先合)

1. `manifest/types.cppm`:`BuildConfig::buildProgramTimeoutSecs`(`optional<int>`)
2. `manifest/toml.cppm`:解析 + 校验 + `kKnownBuildKeys` + 「Supported keys:」文案
3. `build/directives.cppm`:`run_timeout(optional<int>, optional<int>)` + `env_timeout_override()`
4. `build/build_program.cppm`:传值 + 重写诊断(含 manifest 路径)
5. 显式注释:**不进指纹**,附理由
6. T-3 / T-9;`docs/05-mcpp-toml.md`、`docs/07-build-mcpp.md` + zh 镜像
7. 单开 issue:Windows `capture_exec_deadline` 无界

### PR-2 · `SourceKind` 收敛 + `module_extensions`

按「先收敛、后开放」两步走,**中间必须停下来验一次零差分**:

1. 新建 `src/source_kind.cppm`(内置表 = 今天的行为,**不加新扩展名**)
2. `SourceUnit::kind` / `CompileUnit::kind`;scanner + p1689 写入
3. 20 处判定点逐个改读字段或读 `classify`
4. `object_filename_for` 改成总函数(`.cpp`/`.cppm` 原样)
5. **停:归一化 diff `build.ninja`,此处必须是【真·空 diff】** ← 这一步之前不写配置解析,
   也**还没加**第 9 步的旗标,所以 A-1′ 的「rule 行例外」在这里不适用 —— 一个字节都不许变
6. `[build] module_extensions` 解析 + `kKnownBuildKeys` + 文案
7. 默认 glob / 自动 target 推断 联动(#16 #17 #18)
8. 指纹:root + per-package 两处
9. `toolchain/` 加**一个**按编译器家族取值的 `moduleInterfaceFlag`
   (gcc=`-x c++`、clang=`-x c++-module`、msvc=`/interface /TP` 保持现状);
   `cxx_module` 规则**无条件**带上它(§3.8.1)
   —— **没有后缀表、没有 policy 枚举**;⚠️ 每个方言 leg 跑一次 T-10 幂等回归
10. T-1/T-2/T-4/T-5/T-6/T-7/T-8/T-10;文档 + zh 镜像 + `docs/10-publishing-a-library.md` 的发布约束

第 5 步是这个 PR 唯一的安全带。跳过它,后面任何一处 diff 都无法归因。

---

## 8. 明确不做

| 不做 | 理由 |
|---|---|
| 把 `.ccm/.cxxm/.ixx` 加进内置默认 | §1.3,对已发布包的破坏性变更 |
| 超时时交互询问用户 | §4.1,没有交互通道 + 构建不该依赖击键 |
| `[build.extensions]` 四轴角色表 | C/asm 三轴的清单已完整且稳定;结构留位(§3.2),等真实用例 |
| 维护「哪个编译器认哪个后缀」表 | 会过期、错了还**不报错**(退出码 0 的静默空转);改为永远显式发一个已实测幂等的旗标(§3.8.1) |
| 用「任意后缀全放行」当借口不做校验 | 唯一的守卫仍要留:**拒绝**与内置非模块角色冲突的后缀(§3.4) |
| 时长串 `"30m"` | 与三个既有秒制旗标不一致 |
| `module_extensions` 沿依赖边传播 | 它是私有构建属性,不是 usage requirement |
| 删 `SourceUnit::isModuleInterface` / `isImplementation` | 改动面翻倍,收益 0;改成 `kind` 的派生字段即可 |
| 修 `foo.c` / `foo.cpp` 对象名碰撞 | 要动缓存条目布局 ⇒ 单独 issue + 缓存键版本 |
| Windows 的 deadline 实现 | 独立工作量,不该卡住这两个键 |

---

## 7.1 实施记录:设计与现实的四处偏差

实施 PR-2 步 1–5 时发现的、设计阶段没看到的事实。记在这里,因为其中三条是**潜在缺陷**,
不是风格问题。

### ① `isModuleInterface` / `isImplementation` 是**写而不读**的死字段

设计文档写「保留它们(约 20 处读者)」—— **错的**。
`grep -rn 'isImplementation\|isModuleInterface' src/ tests/` 的结果是:**5 处写,0 处读**。

而这 5 处写里有 **3 份互不一致的推导**(scanner 说 `.cpp`,p1689 说 `.cpp || .cxx`,
scan_overrides 说「有 provides」)。

⇒ **删掉,而不是收敛。** 收敛三份对一个没人读的值的推导,仍然是三份推导。
`provides` 回答「是不是接口」,`kind` 回答其余。

### ② `is_implementation_source` 漏了 `.mm`

旧清单 `{.cpp .cc .cxx .c .m .S .s .asm}` **没有 `.mm`**。
所以一个 Objective-C++ 单元会被编译、然后**永远不进链接**。
改成 kind-based(`Cxx` 含 `.mm`)顺带修掉。

### ③ stage 兜底 glob 漏了全部三种汇编扩展名

`prepare.cppm` 的 `{cppm,cpp,cc,c}` 比约定默认少了 `.S/.s/.asm`。
⇒ stage 一个含汇编的依赖时**静默丢掉那些源文件**。改成 `default_source_globs()` 后消失。

### ④ A-1′ 的验证方式:要归一化两条环境量

fixture(2 个 `.cppm` + 2 个 `.cpp` + 1 个 `.c` + 1 个 `.S`,覆盖全部角色)
在实施前后 `build.ninja` 的差异**只有两行**,且都与语义无关:

```
mcpp    = <这次跑的 mcpp 二进制自己的路径>
ldflags = ... -specs=<build dir>/mcpp-clean-link.specs      # build dir 名含指纹
```

归一化掉这两条之后是**真·空 diff**,且**指纹目录名逐字符相同**(`7ca4d5d84ff1fd98`),
11 个指纹字段逐个相同 —— 包括 `[7] compile flags hash`。

> ⚠️ **验证过程本身踩了一个记录在案的坑**:`find target -name mcpp | head -1` 取到的是
> **上一个会话留下的旧二进制**(指纹目录随版本变,`head -1` 不是「最新」)。
> 它报 `[8] 2026.8.10.2`,让我一度以为自己改动了指纹。
> **取二进制必须按 mtime 排序,并核对 `--version`。**

---

## 7.2 ⚠️ GCC 16.1.0 地雷:新模块的**导出接口**里出现 `std` 类型会毒化下游 BMI

实施 Windows deadline(P6)时撞到的,**与本方案的两个功能都无关,但会咬所有人**。

### 现象

给 `mcpp.platform.process` 加一个 `import`,指向一个**新建的**模块,之后:

```
failed: obj/runtime_selection.m.o gcm.cache/mcpp.xlings.runtime_selection.gcm
mcpp.manifest.xpkg: error: failed to read compiled module cluster 192: Bad file data
mcpp.manifest.toml: error: failed to read compiled module cluster 392: Bad file data
... fatal error: failed to load pendings for 'std::allocator'
```

**下游几十个 BMI 全部报废**,报错指向的模块(`mcpp.manifest.xpkg`)和改动毫无关系。

### 逐步二分(每一步都是干净重建)

| 变体 | 结果 |
|---|---|
| 模块存在,但没人 import 它 | ✅ 通过 |
| 被 import,只导出 `int probe()`,**purview 里没有 `import std;`** | ✅ 通过 |
| 被 import,只导出 `int probe()`,**purview 里有 `import std;`** | ✅ 通过 |
| 被 import,**导出接口里出现 `std::string` / `std::vector` / `std::chrono`** | ❌ 下游 BMI 全坏 |
| `import` 换成一个**已存在**的模块(`mcpp.platform.fs`) | ✅ 通过 |

⇒ 触发条件是 **「新模块 × 被 import × 导出接口里有 std 类型」** 三者同时成立。
模块内部随便用 `std`,没问题。

### 排除项(都实测过,都不是)

- **不是竞态**:`ninja -j1` 串行同样失败,且三次运行完全一致。
- **不是陈旧产物**:每次都整目录重建。
- **不是命名冲突**:换模块名、换目录都一样。
- **不是那条 `-Wglobal-module` 警告**:把 `extern "C" char **environ;` 从
  global module fragment 移进 purview(这本身是对的,消掉了一条真实警告),现象不变。

### 采取的做法

平台 shim 的**导出接口做成 std-free**:输出走回调 `void(*)(void*, const char*, unsigned long)`,
环境变量走 `const char* const*` 的 `"K=V"` 数组,返回值只有 `bool`/`int`。
`std` 只在模块**内部**使用。编组代码放在调用方(`mcpp.platform.process`),
那边本来就持有这些 vector。

这不是权宜之计:一个平台 shim 用 C 风格边界本来就是标准做法,而且它把
「这个模块不得把 std 类型放进接口」变成了一条**写在文件头、有实测支撑**的约束,
而不是一句口头约定。

⚠️ **给后来者**:在 `src/platform/` 下新建模块并让 `mcpp.platform.process`
(或任何被广泛 import 的底层模块)import 它时,**先只导出内置类型**。
BMI 坏掉时报错会指向一个和你的改动毫无关系的模块,极难归因 —— 我花了七轮二分。

---

## 7.3 实施最终形态(与设计的差异)

| 设计写的 | 实际做的 | 为什么 |
|---|---|---|
| 保留 `isModuleInterface`/`isImplementation` | **删掉** | 它们是写而不读的死字段(5 写 0 读),3 份互不一致的推导 |
| 方言侧「按需补 `-x`」 | **无条件补** | 旗标幂等已实测;查表会过期且错了静默 |
| Windows deadline 单开 issue | **本 PR 实现** | 用户要求跨平台;平台代码落到 `src/platform/windows/` |
| —— | **新增 `src/platform/{unix,windows,linux,macos}/`** | 用户要求;`process.cppm` 的 25 处 `#if` 收敛成一处 `if constexpr` 分派 |
| —— | **新增可观察性** | `mcpp self doctor` 报告生效的扩展名表、超时值及其来源、deadline 是否真的强制 |

### 可观察性(实测输出)

```
    Checking build policy
          ok module interfaces: .cppm .ixx .ccm .cxxm  (3 from [build] module_extensions)
          ok build.mcpp run bound: 600s  (from built-in default)
          ok process deadlines: enforced (POSIX SIGKILL / Windows job object)
```

外加:`module_extensions` 里零命中的条目会告警(死配置 ≠ 打字错误,不报就分不清)。

### 实测通过的判据

| 判据 | 结果 |
|---|---|
| A-1′ 图形态零差分 | ✅ 未配置时 `build.ninja` 的 diff 只有 2 条 rule 命令 + 2 个 `unit_lang`,**零 `build` 边变化**,指纹目录名逐字符相同 |
| 四种扩展名 build→link→run | ✅ **GCC 与 Clang 两条腿都过**(e2e 217) |
| 对象名无碰撞 | ✅ `a.m.o` / `b.ccm.o` / `c.cxxm.o` / `d.ixx.o` |
| 快路径扫描 `.ixx` | ✅ 加 `import` 后强制全量 prepare(e2e 218) |
| 指纹包含 `module_extensions` | ✅ `65e7cc0a` → `497632df` |
| manifest 超时键 | ✅ 2s 生效、env=1 覆盖它、负数硬错误(e2e 186) |
| 超时报错指名 manifest | ✅ 打印该包 `mcpp.toml` 的绝对路径 |

---

## 7.4 深度自审(CI 后)发现并修掉的两条

### ① 未捕获的有界运行丢了流式输出 —— 真回归

`run_exec_deadline` 是 `mcpp test` **非 JSON 模式**跑测试二进制的路径,原本
**继承调用方 stdio**。我第一版把它改成「捕获后在结束时回放」,后果两条:

1. 长测试的输出全部憋到退出才出现 —— 恰好抵消 `mcpp test` 可观察性那一整轮工作
   (「只有子进程输出、mcpp 一行没有」正是缓冲问题的指纹);
2. 子进程 stdout 变成管道而非终端 ⇒ gtest 之类**静默关掉彩色输出**。

修法:两侧启动器共用一条契约 —— **`sink == nullptr` 表示不捕获**,子进程直接继承
调用方 stdio,但仍然有界。POSIX 不建管道不设 dup2;Windows 不设
`STARTF_USESTDHANDLES`,也不加 `CREATE_NO_WINDOW`(未捕获的运行本来就是给人看的)。

### ② 依赖缓存键的 E 轴漏了 `module_extensions`

指纹管的是 `target/<triple>/<fp>/`,**全局依赖缓存是另一套键**。一个声明了
`module_extensions` 的依赖产出不同的 `.o`/BMI,但它的缓存键此前不含这个字段。

**今天不可达**:默认 glob 会跟着变 ⇒ `sourceGlobs` 已经动了;而索引包描述符按版本
冻结,`package.version` 在 D 轴。⇒ 但这正是本方案要消灭的形状(同一决策漏一处),
一行补上比留着等它以后变成一次「错误的缓存命中」便宜。

### 顺带确认:`-x c++` 不需要 bump 缓存 epoch

老 mcpp 写入的缓存条目(命令行里没有 `-x c++`)会被新 mcpp 读到。因为该旗标在
已识别后缀上**幂等**(§3.8.1 实测),产物逐字节相同 ⇒ 沿用是安全的,不必让全网
缓存作废。

### 一处**刻意的**行为变化

`is_compilable_output`(build.mcpp 生成物能否编译)原本是**唯一含 `.ixx` 的清单**。
改为按 kind 判定后,`.ixx` 生成物在未声明 `module_extensions` 时不再被自动纳入
`sources`。这是**修正而非回归**:旧路径接受它进 sources,而后面每一个阶段都会
错误处理它 —— 没有任何包能靠那条路径正常工作。

---

## 8.1 本次不做,但已排期

| 项 | 状态 | 说明 |
|---|---|---|
| **基于 xlings 生态的图形开发可用性验收** | **推迟**(2026-08-11 决定) | 等 xlings 最新版本发布后单独做。本 PR 的验收范围到「xlings 生态装→建→跑」为止,不含 GL context 获取那一段。图形栈本身的三层故障见 `2026-08-10-graphics-stack-usability-design.md` |

---

## 9. 开放问题

1. **`[lib]` 约定(#19)** 是否要跟着 `module_extensions` 走?
   倾向**不**:它是**写**路径(`mcpp new` 生成 `src/<name>.cppm`),
   把生成物的拼法交给配置只会让脚手架产出不可预测的文件名。
2. **`module_extensions` 要不要在 `mcpp doctor` 里回显?**
   倾向要 —— 一个「为什么我的 `.ixx` 没被编译」的问题,
   目前唯一的自查手段是读 `inferredNotes`。
3. **索引侧的 mcpp 版本下限**(§5)当前是什么机制、是否可降级 ——
   这是 PR-2 发布前的**阻塞项**,不是实施项。
