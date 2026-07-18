# #233 对象路径消歧的两个后续缺口(#240 / #239)——根因分析与统一修复方案

> 日期:2026-07-19
> 基线:mcpp **0.0.97**(HEAD `05155ff`,`MCPP_VERSION = "0.0.97"` @ `src/toolchain/fingerprint.cppm`)
> 范围:GitHub issue **#240**(阻塞 mcpplibs #79 opencv 收录)+ **#239**(同批同区域回归)
> 全部锚点均在 0.0.97 源码 `src/build/plan.cppm` / `src/modgraph/scanner.cppm` 上核实,并以自托管构建产物做了**两份真实最小复现**。

---

## 0. 结论先行

0.0.97 的 #233 修复(commit `3b4f18e`,"mirror source relative path in object paths")把对象输出路径从
`obj/<pkg>_<父目录名>/<file>.o` 改成 `obj/<sanitize(pkg)>/<relPath 的目录部分>/<file>.o`,解决了"同父目录名不同深度"的
折叠碰撞。但这次改动**只覆盖了 scanner 产出的编译单元**,遗漏了两条路径,构成两个 OPEN bug:

| # | 症状 | 触发面 | 根因子系统 |
|---|------|--------|-----------|
| **#240** | `ninja: error: 'obj/main.o' … missing and no known rule` | 依赖包与消费者存在**同名源**(双方都有 `main.cpp`) | 链接输入端的 entry-main 对象路径**独立重算**,没跟随消歧 |
| **#239** | `obj/<pkg>/../…/gen.o`(逃逸出 `obj/`,甚至镜像整棵绝对路径树) | 依赖 `build.mcpp` 把生成源写进 **OUT_DIR**(绝对路径,在包根之外) | 消歧前缀直接取 `relPath.parent_path()`,含 `..`/绝对根时 `obj/<prefix>` 向上逃逸 |

两者**同源**:`#233` 把"对象路径 = 前缀 + 文件名"这件事拆散在两处计算(scanner 单元一处、link 端 entry-main 一处),
且前缀构造对"源在包根之外"的相对路径不做净化。修复思路统一为一句话:

> **对象路径的分配收敛成一个函数,entry-main 走同一个函数;前缀构造对任意 relPath 保证"永远向下、绝不逃逸 `obj/`"。**

修复面小、局部、对常见工程**零字节差异**(非碰撞、relPath 干净时输出与今天完全一致),可一次 PR / 一个版本(**0.0.98**)、多 commit 落地。

---

## 1. 复现(两份,均已在本机自托管产物上跑通)

### 1.1 #240 —— 同名 `main.cpp`

```
mydep/  (kind=lib)      src/main.cpp   -> int dep_helper()
consumer/(kind=bin)     src/main.cpp   -> int main(){…}   dep = { path=../mydep }
                        [modules] sources = ["src/**/*.cpp"]   # main.cpp 被 glob 进来 → 被 scan
```

`mcpp build` →
```
ninja: error: 'obj/main.o', needed by 'bin/consumer', missing and no known rule to make it
```

build.ninja 实况:
```
build obj/mydep/src/main.o    : cxx_object …/mydep/src/main.cpp      # dep 的 main 消歧了
build obj/consumer/src/main.o : cxx_object …/consumer/src/main.cpp   # 消费者的 main 也消歧了
build bin/consumer : link … obj/main.o obj/mydep/src/main.o          # 但链接引用了消歧前的 obj/main.o
```

### 1.2 #239 —— 依赖 `build.mcpp` 生成的 OUT_DIR 绝对路径源

```
gdep/   build.mcpp 把 gen.cpp 写进 out_dir()(= <consumer>/target/.build-mcpp/deps/gdep@0.1.0/out)
        并 mcpp::generated("gen.cpp")
consumer/ src/gen.cpp（与 gdep 生成的 gen.cpp 同 basename → 触发消歧分支）
```

`mcpp build` →
```
cc1plus: fatal error: opening output file
  obj/gdep/../consumer/target/.build-mcpp/deps/gdep@0.1.0/out/gen.o: No such file or directory
```

对象路径里出现 `..`(relPath = `relative(绝对生成源, dep 包根)`),`obj/gdep/../…` 已经逃出 `obj/`;
若 `..` 更多(生成源与包根分处更远),归一化后会爬到根再下钻,等价于在 CWD 下**镜像整棵绝对路径树**。

---

## 2. 根因(file:line,0.0.97)

### 2.1 #240 —— entry-main 对象路径独立重算,link 用了陈旧值

`src/build/plan.cppm`:

- **消歧只发生在 scanner 单元循环**(L448–L484):`basenameCount` 仅统计 `topoOrder`;碰撞时
  `cu.object = obj / <sanitize(pkg)>/<relDir> / fname`,否则 `obj/fname`。
- **entry-main 是在 link 组装时另造的**(L733–L784):
  ```cpp
  main_cu.object = std::filesystem::path("obj") / object_filename_for(*lu.entryMain, objExt); // L737 恒为 obj/<name>.o
  …
  bool already = …;                       // main.cpp 若被 glob 进来则已在 compileUnits 里
  if (!already) plan.compileUnits.push_back(main_cu);
  lu.objects.push_back(main_cu.object);   // L784 —— 无论 already 与否,都 push 了 L737 的扁平路径
  ```
  当消费者 `main.cpp` 被 glob 进 sources(几乎所有工程 `sources=["src/**/*.cpp"]`)且与依赖的 `main.cpp`
  同名时:scanner 已把它编到 `obj/<pkg>/src/main.o`,但 L784 仍把 `obj/main.o` 塞进链接输入 → 没有规则产出 → 报错。
  entry-main 也从未进入 `basenameCount` 普查,是消歧体系的一个盲区。

### 2.2 #239 —— 消歧前缀对包根外的 relPath 不做净化

`src/build/plan.cppm` L469–L474:
```cpp
if (basenameCount[fname] > 1) {
    auto relDir = u.relPath.parent_path();                        // 可能含 `..` 或为绝对
    auto prefix = u.packageName.empty() ? relDir
                : std::filesystem::path(sanitize(u.packageName)) / relDir;
    cu.object = std::filesystem::path("obj") / prefix / fname;    // `obj/<..>/…` 逃逸
}
```
`relPath` 由 `scanner.cppm` L1005 `std::filesystem::relative(f, p.root)` 得到。依赖 `build.mcpp` 的生成源经
`build_program.cppm` L563–L567 被改写成 **OUT_DIR 下的绝对路径**再进 `modules.sources`;它在包根之外,
`relative()` 于是产出 `../../…` 前缀。碰撞分支把它原样拼进 `obj/`,路径逃逸。

---

## 3. 修复方案(统一,局部)

全部改动集中在 `src/build/plan.cppm` 的 `make_plan`,新增两个纯函数式的小工具,无跨文件/无 schema 变更。

### 3.1 前缀净化:`safe_object_prefix`(修 #239,并顺带护住 entry-main)

把"包名 + relPath 目录部分"折叠成一个**永远向下**的子目录,对每个路径分量做单射映射:
- 根名 / 根目录(`/`、盘符)分量:丢弃(去绝对根);
- `.`:丢弃;
- `..`:映射为 `__up`(保留单射 → 不破坏 #233 的唯一性,除非真有目录名叫 `__up`,概率极低且有 §3.4 断言兜底);
- 其余分量:原样保留。

```cpp
// relPath 可能是绝对的或含 `..`(源在其包根之外 —— 如依赖 build.mcpp 的 OUT_DIR 生成源,#239)。
// 直接拼进 obj/ 会让对象路径爬出构建树。把每个分量折叠成"安全且向下"的 token:
// 去根、丢 `.`、`..`→`__up`、其余保留。逐分量单射,保住 #233 的唯一性前提。
std::filesystem::path safe_object_prefix(const std::string& pkg,
                                         const std::filesystem::path& relDir) {
    std::filesystem::path safe;
    for (auto const& comp : relDir) {
        if (comp.has_root_name() || comp.has_root_directory()) continue; // 去绝对根
        auto s = comp.string();
        if (s.empty() || s == ".") continue;
        safe /= (s == ".." ? "__up" : s);
    }
    return pkg.empty() ? safe : std::filesystem::path(sanitize(pkg)) / safe;
}
```
非碰撞路径不走此函数(仍是扁平 `obj/fname`);碰撞且 relPath 干净时,输出与今天**逐字节一致**(`__up` 只在有 `..` 时出现)。

### 3.2 对象路径分配收敛成一个 lambda:`object_for`(修 #240 的一半 + 复用)

```cpp
auto object_for = [&](const std::filesystem::path& src,
                      const std::string& pkg,
                      const std::filesystem::path& relPath) -> std::filesystem::path {
    const auto fname = object_filename_for(src, objExt);
    if (basenameCount[fname] > 1)
        return std::filesystem::path("obj") / safe_object_prefix(pkg, relPath.parent_path()) / fname;
    return std::filesystem::path("obj") / fname;
};
```
scanner 单元循环(L459–L483)改用 `cu.object = object_for(u.path, u.packageName, u.relPath);`。

### 3.3 entry-main 进入普查、并复用已扫描单元的对象路径(修 #240 的另一半)

1. **普查纳入 entry-main**:在 `basenameCount` 计算处(L448),把 root manifest 里 **binary/test 且带 `main`**、
   且**尚未**被 scan(不在 `scannedSources`)的 entry 源也 `++` 计入。这样"消费者 main 未被 glob、但依赖有同名 main"
   的场景也能正确消歧。
2. **link 端复用**(L777–L784)改为:
   ```cpp
   std::filesystem::path entryObject;
   bool already = false;
   for (auto& cu : plan.compileUnits)
       if (cu.source == main_cu.source) { already = true; entryObject = cu.object; break; }
   if (!already) {
       main_cu.object = object_for(main_cu.source, main_cu.packageName,
                                   std::filesystem::relative(main_cu.source, projectRoot));
       plan.compileUnits.push_back(main_cu);
       entryObject = main_cu.object;
   }
   lu.objects.push_back(entryObject);   // 永远用编译单元真实产出的对象
   ```
   - `already`(main 被 glob):直接复用 scanner 已消歧的 `cu.object` —— **这正是 #240 的直接修复**。
   - `!already`(main 只在 `main=` 里、未被 glob):走同一 `object_for`,与 dep 的同名 main 一致消歧。
   - L737 那句独立重算(`object_filename_for`)删除,不再是路径事实来源。

### 3.4 保留 #233 的唯一性断言(L486–L512)

`safe_object_prefix` 逐分量单射 + entry-main 纳入同一分配器后,理论上不会再碰撞;L486 的"碰撞即报错"防御断言**保留原样**,
作为任何未预料输入的可诊断兜底(把 ninja 的 "multiple rules generate X" 变成点名两个源文件的 mcpp 错误)。

---

## 4. 影响面与兼容性

- **常见工程零差异**:单 binary、`main.cpp` 唯一 → 仍是扁平 `obj/main.o`,link 复用同一路径。e2e 117 现状(`main=src/main.cpp` 且唯一)不变。
- **仅在发生 basename 碰撞时**行为改变,且改后才是正确的(今天是构建直接失败)。
- 无 manifest schema、无 CLI、无跨平台字节差异(`safe_object_prefix` 用 `path` 迭代器,Windows 盘符走 `has_root_name` 分支)。

## 5. 提交拆分(单 PR / 0.0.98,多 commit)

1. `test(e2e): add failing repros for #240 (same-named main) and #239 (OUT_DIR abs gen source)` —— 先落两个红测(TDD)。
2. `fix(build): route entry-main object path through disambiguation; reuse scanned unit's object (#240)`。
3. `fix(build): sanitize collision prefix so out-of-root/abs relPaths stay under obj/ (#239)`。
4. `chore(release): bump mcpp 0.0.97 -> 0.0.98` + CHANGELOG。

> 发版后按既定链路:release 四平台 → 镜像 xlings-res(gh+gtc)→ xim-pkgindex 索引 → bump bootstrap pin;随后 mcpplibs #79 CI pin 升到 0.0.98 即可解阻塞。#242(消费端 default-features opt-out)、#243(依赖 feature 转发)为独立非阻塞项,不在本 PR。

## 6. 验证清单

- `mcpp test`(单测,含 ninja backend)全绿。
- e2e:新增两测 + `tests/e2e/run_all.sh` host-aware 全过(至少 117/118 + 新两测)。
- 手工:§1 两份复现均 `mcpp build && mcpp run` 成功,ninja 中 entry-main link 输入 = 其编译边真实对象;`obj/` 下无 `..`/绝对逃逸。
